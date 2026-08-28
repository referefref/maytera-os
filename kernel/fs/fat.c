// fat.c - FAT12/FAT16/FAT32 Filesystem Driver
#include "fat.h"
#include "ext2.h"   // #99 Phase C: ext2-as-root hook (g_root_ext2, ext2_read_whole)

#include "../drivers/ata.h"
#include "blockdev.h"   // #307: route sector I/O to ATA or USB MSC root
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../proc/process.h"
#include "fs/bootlog.h"   // #742: the owning header, NOT a private extern
#include "../sync/waitq.h"    // #746 fatlock: the canonical blocking primitive
#include "../sync/noblock.h"  // #746 fatlock: wq_may_block(), the ONE no-block rule
#include "../security/uaccess_smap.h"  // #19/#645: AC bracket on the caller-buffer copy
#include "../cpu/wallclock.h"   // #115: the ONE wall clock + calendar converter

// Sector buffer
static uint8_t sector_buf[512];

// ---------------------------------------------------------------------------
// FAT global lock (fixes filesystem corruption under concurrent writers).
//
// The driver uses the shared global `sector_buf` for read-modify-write of FAT
// table sectors AND directory sectors. The kernel scheduler is preemptive, and
// FAT operations are now issued from MANY concurrent contexts (the compositor
// saving /UIPROFIL.YML, background services writing logs, and especially Win16
// apps, which run as their own processes via proc_create and write /WIN16LOG.TXT
// frequently). With the shared buffer, one context could be preempted between
// the read of a directory/FAT sector and its write-back, another FAT writer
// would clobber sector_buf, and the first context then committed garbage to
// disk: kernel log-string fragments appearing as directory entries and
// cross-linked clusters (cross-linked /ICONS, /THEMES, truncated files). fsck
// reported "Both FATs appear to be corrupt".
//
// Fix: a single recursive lock held across each whole public FAT operation, so
// the entire sector_buf read-modify-write span (including multi-sector scan
// loops) runs without another FAT context interleaving. Recursive
// (owner-tracked) so nested public calls (fat_copy -> fat_read_file/
// fat_write_file, fat_move -> fat_rename) are safe.
//
// ===========================================================================
// #746 fatlock: WHY THIS IS A WAIT QUEUE AND NOT A YIELD-SPIN ANY MORE
// ===========================================================================
// The original acquire loop was:
//
//     for (;;) {
//         ...test-and-set under sched_set_preemption(false)...
//         sched_schedule();   // another context holds it; yield and retry
//     }
//
// A waiter that "yields" this way STAYS RUNNABLE. It is re-queued on the ready
// queue by sched_schedule() (proc/process.c: prev->state == RUNNING =>
// add_to_ready_queue) and comes straight back. That is fine against any holder
// the scheduler will eventually re-run. It is FATAL against pid 0, because:
//
//   * pid 0 is deliberately NEVER re-queued when it is preempted
//     (proc/process.c: `if (prev->pid != 0) add_to_ready_queue(prev);`, #601),
//     and
//   * pid 0 is ALSO the idle process, so the ONLY way it is ever selected again
//     is the empty-ready-queue fallback (`if (!next) next = &proc_table[0];`).
//
// So: pid 0 takes this lock, a timer tick preempts it mid-write, it is left
// READY but off the queue, and every other FAT toucher then yield-spins and
// stays runnable. The ready queue can never drain, so pid 0 is never selected,
// so the lock is never released. MEASURED on a single-partition-FAT boot
// (3/3 hangs): g_fat_lock_owner == &proc_table[0] ("idle", pid 0),
// g_fat_lock_depth == 2, current_process == "win16auto", proc_table[0].state ==
// READY, ready_queue_head non-NULL on every sample, 100% of one core, serial
// dead. pid 0 IS the desktop context and writes to FAT for the whole session,
// so this is not a boot-only hazard.
//
// The fix is the project's standing rule, which this lock was violating: ALL
// waiting goes through the wait queue (sync/waitq.h). A waiter now LEAVES the
// ready queue, so the queue drains, so the empty-queue fallback selects pid 0,
// which finishes its write and wakes everyone. The shape is copied from
// drivers/usb_msc.c's msc_cmd_lock() (#617), which is the canonical in-tree
// "hand-rolled spin -> wait queue with a counted no-block fallback".
//
// THE NO-BLOCK PROBLEM, AND WHAT WE DO ABOUT IT
// ---------------------------------------------
// fat_lock() IS reachable from contexts that must never park (sync/noblock.h:
// scheduler not live, or proc_current()==NULL, or RFLAGS.IF clear). Measured
// reachability, by reading the tree:
//
//   (a) PRE-SCHEDULER. main.c calls fat_exists()/fat_read_file() ~14 times
//       before proc_init(). Single-threaded, so the lock is UNCONTENDED there
//       by construction; note fat_lock_self() already returns a shared
//       sentinel for all of them, so they also alias as one owner.
//   (b) THE PANIC PATH. kpanic() does cli(), then panic_log_write() ->
//       bootlog_heartbeat_flush() -> bootlog_persist() -> fat_write_file().
//       With the OLD lock, a panic that landed while another context held the
//       lock called sched_schedule() with IF=0 and hung the box instead of
//       recording the panic. fs/panic.c's own fixed_slot_write() is carefully
//       lock-free; the heartbeat flush punched a hole through it.
//   (c) net_lock()'d TX diagnostics. drivers/usb_ecm.c calls bootlog_write()
//       from usbnet_bulk_out(), which runs under net_lock() (cli + a PLAIN,
//       non-irqsave spinlock) and also runs before the scheduler.
//
// A blocking acquire in any of those is wrong, so we do not attempt one. But
// there is no correct way to WAIT in a context that can neither park nor
// yield, so we do not pretend otherwise: the no-block arm spins BOUNDED and
// then FAILS, and fat_lock() returns a bool that every caller checks. A
// bounded, counted, loudly-reported I/O failure is strictly better than an
// unbounded wedge, and it is the only outcome that is honest about the
// constraint. On SMP the bounded spin can genuinely win (the holder is running
// on another core); on a single core it cannot, and it gives up instead of
// hanging. Either way it returns.
//
// Both fallback events are COUNTED rather than asserted away, because "this is
// unreachable" is exactly the kind of claim that rots (#617).
//
// SMP NOTE: the test-and-set no longer relies on sched_set_preemption(), which
// only ever disabled preemption on the LOCAL cpu and therefore never excluded
// a second core at all. It is a compare-and-swap now, which is both correct on
// SMP and cheaper on the uncontended path (one locked cmpxchg instead of two
// preemption toggles). The recursive re-entry test needs no atomic: only the
// owner can observe owner == me, and only the owner ever writes depth.
static void * volatile g_fat_lock_owner = 0;
static volatile int    g_fat_lock_depth = 0;
static wait_queue_head_t g_fat_lock_wq = { .head = NULL, .lock = SPINLOCK_INIT };

// Per-park deadline. Belt and braces only: fat_unlock() ALWAYS wake_up_all()s,
// and the acquire loop re-checks the owner after __wait_prepare() (the
// lost-wake close), so a waiter cannot miss a release. The deadline exists so
// that if that reasoning is ever wrong the symptom is a COUNTER
// (g_fat_lock_wait_timeouts) rather than a hang. Non-zero means the wake path
// is broken and must be fixed; it is not a licence to leave it broken.
#define FAT_LOCK_BLOCK_MS      10

// Bound on the no-block fallback spin. ~200k pause iterations is single-digit
// milliseconds on any plausible core.
#define FAT_LOCK_NOBLOCK_SPINS 200000u

// Iterations burned by the no-block fallback, times it gave up, and times a
// parked waiter hit its deadline with no wake. All three should be ZERO.
volatile uint64_t g_fat_lock_noblock_spins  = 0;
volatile uint64_t g_fat_lock_noblock_fails  = 0;
volatile uint64_t g_fat_lock_wait_timeouts  = 0;

// #746 fatlock exclusion witness (test builds only; see fs/fat_lock_test.c).
// Counts contexts inside the critical section. Anything other than 1 while the
// lock is held means the lock does not exclude.
#ifdef FAT_LOCK_SELFTEST
volatile int      g_fat_lock_occupancy       = 0;
volatile uint64_t g_fat_lock_excl_violations = 0;
#endif

static inline void *fat_lock_self(void) {
    void *p = (void *)proc_current();
    return p ? p : (void *)1;   // sentinel for the early-boot single-threaded path
}

// Claim the lock at depth 1. Returns true if this call took it.
static inline bool fat_lock_try(void *me) {
#ifdef FAT_LOCK_SELFTEST_RED
    // NEGATIVE CONTROL (`make fatlock-selftest-red`). A lock that excludes
    // nobody: every acquire "succeeds". fs/fat_lock_test.c MUST report FAIL
    // against this build, or the test proves nothing about the real one.
    g_fat_lock_owner = me;
    g_fat_lock_depth = 1;
#else
    if (!__sync_bool_compare_and_swap(&g_fat_lock_owner, (void *)0, me))
        return false;
    g_fat_lock_depth = 1;
#endif
#ifdef FAT_LOCK_SELFTEST
    if (__sync_add_and_fetch(&g_fat_lock_occupancy, 1) != 1)
        __sync_fetch_and_add(&g_fat_lock_excl_violations, 1);
#endif
    return true;
}

// Bounded fallback for a context that must not park. See the header comment.
static bool fat_lock_noblock(void *me) {
    for (uint32_t i = 0; i < FAT_LOCK_NOBLOCK_SPINS; i++) {
        if (fat_lock_try(me)) {
            if (__sync_fetch_and_add(&g_fat_lock_noblock_spins, i + 1) == 0)
                kprintf("[FAT] #746: a no-block context hit contention on the FAT "
                        "lock; the bounded fallback is NOT dead code\n");
            return true;
        }
        __asm__ volatile("pause");
    }
    if (__sync_fetch_and_add(&g_fat_lock_noblock_fails, 1) == 0)
        kprintf("[FAT] #746: no-block context (reason=0x%x, caller=%p) could not "
                "take the FAT lock; REFUSING the operation rather than wedging\n",
                (unsigned)wq_noblock_reason(), __builtin_return_address(0));
    return false;
}

// Acquire the FAT lock. Returns false ONLY in a no-block context that lost a
// bounded race for it; a context that is allowed to park never fails, so no
// normal caller sees a new error path.
static bool fat_lock(void) {
    void *me = fat_lock_self();

    // Recursive re-entry: safe without an atomic (see the SMP NOTE above).
    if (g_fat_lock_owner == me) { g_fat_lock_depth++; return true; }

    // Uncontended acquire: one cmpxchg, no queue, no lock. The common case.
    if (fat_lock_try(me)) return true;

    for (;;) {
        if (!wq_may_block()) return fat_lock_noblock(me);

        wait_queue_entry_t wqe;
        __wait_prepare(&g_fat_lock_wq, &wqe, 0);
        // Re-check AFTER queueing: this is the lost-wake close. If the holder
        // released between our failed acquire and here, it either already saw
        // us on the queue and woke us, or the owner now reads 0 and we skip
        // the park entirely.
        if (g_fat_lock_owner != 0) {
            if (__wait_event_wait_deadline(&wqe, sched_now_ms() + FAT_LOCK_BLOCK_MS)
                    == WAIT_TIMEOUT)
                __sync_fetch_and_add(&g_fat_lock_wait_timeouts, 1);
        }
        __wait_finish(&g_fat_lock_wq, &wqe);

        if (fat_lock_try(me)) return true;
    }
}

static void fat_unlock(void) {
    if (g_fat_lock_depth <= 0) return;      // unbalanced release: ignore, as before
    if (--g_fat_lock_depth == 0) {
#ifdef FAT_LOCK_SELFTEST
        __sync_sub_and_fetch(&g_fat_lock_occupancy, 1);
#endif
        __sync_lock_release(&g_fat_lock_owner);
        // Unconditional, exactly like msc_cmd_unlock(): wake_up_all() is
        // documented safe from IRQ context and is a NULL check on an empty
        // queue, so it costs nothing in the uncontended case and cannot lose a
        // wake in the contended one.
        wake_up_all(&g_fat_lock_wq);
    }
}

// Inner (unlocked) implementations; the public wrappers below take the FAT lock.
static int      fat_open_inner(fat_fs_t *fs, const char *path, fat_file_t *file);
static int      fat_read_inner(fat_file_t *file, void *buffer, uint32_t size);
static int      fat_readdir_inner(fat_file_t *dir, fat_dir_entry_t *entry, char *name_out, size_t name_cap);
static void     fat_list_dir_inner(fat_fs_t *fs, const char *path);
static int      fat_create_inner(fat_fs_t *fs, const char *path);
static int      fat_mkdir_inner(fat_fs_t *fs, const char *path);
static int      fat_delete_inner(fat_fs_t *fs, const char *path);
static int      fat_rename_inner(fat_fs_t *fs, const char *old_path, const char *new_path);
static int      fat_write_inner(fat_file_t *file, const void *buffer, uint32_t size);
static int      fat_copy_inner(fat_fs_t *fs, const char *src_path, const char *dst_path);
static int      fat_move_inner(fat_fs_t *fs, const char *src_path, const char *dst_path);
static int      fat_write_file_inner(fat_fs_t *fs, const char *path, const void *data, uint32_t size);
static int      fat_exists_inner(fat_fs_t *fs, const char *path);
static uint32_t fat_get_free_clusters_inner(fat_fs_t *fs);
// #746: fat_truncate() is grouped with the other public wrappers above the two
// statics it uses, so they are declared here rather than the wrapper being
// moved away from its siblings.
static int      fat_free_cluster_chain(fat_fs_t *fs, uint32_t start_cluster);
static int      fat_persist_dirent(fat_file_t *file);
// #745 local 109: fat_truncate_to() walks and re-terminates the cluster chain,
// so it needs these two the same way the wrappers above need the pair below.
static uint32_t fat_next_cluster(fat_fs_t *fs, uint32_t cluster);
static int      fat_set_fat_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value);
static int      fat_erase_name_entries(fat_fs_t *fs, fat_file_t *dir, const char *name);

// #725: 8.3-ise an ext2 name into the 11-byte padded field of a synthesized
// fat_dir_entry_t. Callers that read entry->name (dosexec's INT 21h 4Eh/4Fh
// FindFirst/FindNext runs it back through fat11_to_dotname) then behave exactly
// as they do on FAT. Long ext2 names truncate here; the caller's name_out still
// receives the full name, so nothing that uses name_out loses information.
static void ext2_name_to_83(const char *name, uint8_t out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int dot = -1;
    for (int k = 0; name[k]; k++) if (name[k] == '.') dot = k;
    int o = 0;
    for (int i = 0; name[i] && (dot < 0 || i < dot) && o < 8; i++) {
        char ch = name[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        out[o++] = (uint8_t)ch;
    }
    if (dot >= 0) {
        o = 8;
        for (int k = dot + 1; name[k] && o < 11; k++) {
            char ch = name[k];
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
            out[o++] = (uint8_t)ch;
        }
    }
}

// #725: one directory step over an ext2-backed handle. Mirrors
// fat_readdir_inner's contract exactly: 0 = filled entry/name_out and advanced
// the cursor, -1 = end of directory or error.
static int fat_readdir_ext2(fat_file_t *dir, fat_dir_entry_t *entry,
                            char *name_out, size_t name_cap) {
    if (!dir->is_dir) return -1;
    char nm[256];
    uint32_t ino = 0, pos = dir->ext2_dirpos;
    uint8_t ftype = 0;
    if (ext2_readdir_ino(dir->ext2_ino, &pos, nm, (int)sizeof(nm), &ino, &ftype) != 0)
        return -1;
    dir->ext2_dirpos = pos;
    dir->position = pos;
    if (name_out && name_cap > 0) {
        size_t k = 0;
        while (k + 1 < name_cap && nm[k] != 0) { name_out[k] = nm[k]; k++; }
        name_out[k] = 0;
    }
    if (entry) {
        memset(entry, 0, sizeof(*entry));
        ext2_name_to_83(nm, entry->name);
        int isdir = (ftype == 2);            // EXT2_FT_DIR
        if (ftype == 0) {                    // rev0 volume: no type in the dirent
            int d = 0;
            if (ext2_get_is_dir(ino, &d) == 0) isdir = d;
        }
        entry->attr = isdir ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
        if (!isdir) {
            ext2_inode_t in;
            if (ext2_read_inode(ino, &in) == 0) entry->file_size = in.i_size;
        }
    }
    return 0;
}

// ===========================================================================
// #196: is `path` inside a removable drive that currently has a disk IMAGE
// mounted on it? If so, return the drive letter and point *rel at the path
// INSIDE the image; otherwise return 0.
//
// The folder /WINDIR/DRIVE_E also exists on the root filesystem, so this test
// MUST run before the ext2 redirect below or the empty folder wins and the
// mounted disc stays invisible. That ordering is the whole mechanism.
// ===========================================================================
// #193: the STRING half moved to rustkern/drvmap.rs (drvmap_windir_split_rs),
// which is already the authority on what a drive letter means. It was open-
// coded here, and #193 needed the same decision at the syscall layer too; a
// second hand-rolled copy of the parse is exactly how the three drive maps
// drifted apart before #739. New code, so Rust per the standing directive: it
// is a bounded string compare with no float and no allocation, and it is called
// once per path syscall, which a plain call is cheap enough for.
extern int drvmap_windir_split_rs(const unsigned char *path, unsigned int *rel_off);

// #193: does a currently-mounted disk image OWN this path's subtree? The ONE
// definition, used by fat_open() below AND by path_root_ext2() in
// proc/syscall_path.h, so the fat layer and the syscall layer cannot disagree
// about whose file a path is.
//
// NON-BLOCKING BY CONSTRUCTION, and it has to be: path_root_ext2() is evaluated
// on every path syscall, including from contexts that may hold a lock. This
// does a bounded string compare and one spinlock-protected read of the mount
// table (diskimg_is_mounted). It performs no I/O, touches no image and cannot
// sleep. Asking "does the disc CONTAIN this file" here instead would have been
// an ISO directory read behind a wait-queue turnstile on the world's hottest
// predicate, which is the #426 shape.
int path_img_shadows(const char *path) {
    extern int diskimg_is_mounted(char letter);
    if (!path) return 0;
    int li = drvmap_windir_split_rs((const unsigned char *)path, 0);
    if (li < 0) return 0;
    return diskimg_is_mounted((char)('A' + li)) ? 1 : 0;
}

static char fat_img_path(const char *path, const char **rel) {
    extern int diskimg_is_mounted(char letter);
    if (!path) return 0;
    unsigned int roff = 0;
    int li = drvmap_windir_split_rs((const unsigned char *)path, &roff);
    if (li < 0) return 0;
    char letter = (char)('A' + li);
    // #739: ANY letter, not the two hardcoded ones. Which letters may hold an
    // image is rustkern/drvmap.rs's decision, and diskimg_is_mounted() is the
    // single live answer to "is there a disc in it".
    if (!diskimg_is_mounted(letter)) return 0;
    if (rel) *rel = path + roff;
    return letter;
}

// ===========================================================================
// #725: THE ONE PLACE A PATH BECOMES A HANDLE.
//
// fat_open() is the only fat_* entry point that turns a path into a fat_file_t,
// and fat_read/fat_seek/fat_size/fat_readdir_n/fat_is_dir/fat_close are all
// implemented on that handle. Putting the ext2-root redirect HERE therefore
// fixes every one of them at once, plus fat_list_dir() (which is written on top
// of fat_open + fat_readdir), plus every future caller: there is nothing left
// for anyone to forget to add.
//
// This is the fix for the actual reported fault: fat_read_file() had the
// redirect and fat_open() did not, so on an ext2-root golden a DOS program's
// .EXE loaded (whole-file read) and then every data file it opened through
// INT 21h 3Dh failed ("[dos] 3Dh open FAIL '/DOS/KEEN5/EGAGRAPH.CK5'"). The
// same hole silently affected gui/ttf.c font loading, gui/thumbnailer.c,
// gui/filebrowser.c, gui/properties.c, fs/xattr.c, fs/fat_vfs.c, fs/netfs.c and
// exec/win16api.c, all of which fat_open() paths that live only on ext2.
//
// Order is ext2-first-then-FAT, matching fat_read_file()/fat_exists(): a path
// that is not present on ext2 may still be ESP-only, so a miss falls through.
// ===========================================================================
int fat_open(fat_fs_t *fs, const char *path, fat_file_t *file) {
    // #196: a removable drive with a mounted image serves its files from the
    // image. This must precede the ext2 branch (the /WINDIR/DRIVE_E folder is
    // itself an ext2 directory, and it would otherwise shadow the disc).
    if (file) {
        extern int diskimg_stat(char letter, const char *relpath,
                                uint64_t *size_out, int *isdir_out);
        const char *rel = 0;
        char dl = fat_img_path(path, &rel);
        if (dl) {
            uint64_t isz = 0; int isdir = 0;
            if (diskimg_stat(dl, rel, &isz, &isdir)) {
                extern uint32_t diskimg_generation(char letter);
                memset(file, 0, sizeof(*file));
                file->fs = fs;
                file->img_drive = dl;
                // #739: stamp WHICH disc this is. Everything below re-resolves
                // img_rel on the drive, so this is the only thing that ties the
                // handle to the disc that was actually opened.
                file->img_gen = diskimg_generation(dl);
                { size_t k = 0;
                  while (k + 1 < sizeof(file->img_rel) && rel[k]) { file->img_rel[k] = rel[k]; k++; }
                  file->img_rel[k] = 0; }
                file->is_dir = isdir;
                // A handle exposes a 32-bit size. Every real CD file is well
                // under 4 GiB (the largest on the target discs is 500 MB), and
                // clamping is safer than wrapping if one ever is not.
                file->file_size = (isz > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)isz;
                file->attr = isdir ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
                file->position = 0;
                file->open = 1;
                { const char *b = rel, *q = rel;
                  for (; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
                  size_t k = 0;
                  while (k + 1 < sizeof(file->name) && b[k]) { file->name[k] = b[k]; k++; }
                  file->name[k] = 0; }
                return 0;
            }
            // Mounted, but the disc does not have this path: report a miss
            // rather than falling through to the (empty) /WINDIR folder, so a
            // mounted disc is authoritative for its own drive letter.
            return -1;
        }
    }
    if (file && fat_path_on_ext2(fs, path)) {
        uint32_t ino = ext2_resolve_path(fat_ext2_vol_path(path));
        ext2_inode_t in;
        if (ino != 0 && ext2_read_inode(ino, &in) == 0) {
            memset(file, 0, sizeof(*file));
            file->fs = fs;
            file->ext2_ino = ino;
            file->ext2_dirpos = 0;
            file->is_dir = ((in.i_mode & 0xF000) == 0x4000) ? 1 : 0;
            file->file_size = file->is_dir ? 0 : in.i_size;
            file->attr = file->is_dir ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
            file->position = 0;
            file->open = 1;
            // #120: STAMP THE TIMESTAMPS. This branch memset the handle and
            // then filled size/attr/name and nothing else, so an ext2-backed
            // fat_file_t came back with mtime_date == mtime_time == 0. Every
            // consumer that reads those fields therefore rendered the FAT epoch
            // with a zero month and day, which is the literal source of the
            // "1980-00-00 00:00" that gui/properties.c has shown for years.
            //
            // Fixing it in properties.c would have fixed ONE caller and left
            // the handle still lying to the next one. The inode has the real
            // time; converting it here is what makes the handle honest.
            //
            // ktime_unix_to_dos_rs (rustkern/ktime.rs, the ONE converter) is
            // deliberately allowed to REFUSE: it writes nothing and returns -1
            // for a time outside what a FAT directory entry can represent,
            // which includes epoch 0. A volume written before #115 has i_mtime
            // == 0, and 0 stays 0 - "unknown", not 1980-01-01. The fields are
            // already zero from the memset, so a refusal needs no handling.
            {   uint16_t d = 0, t = 0;
                if (ktime_unix_to_dos_rs((int64_t)in.i_mtime, &d, &t) == 0) {
                    file->mtime_date = d; file->mtime_time = t;
                }
                if (ktime_unix_to_dos_rs((int64_t)in.i_ctime, &d, &t) == 0) {
                    file->ctime_date = d; file->ctime_time = t;
                }
                if (ktime_unix_to_dos_rs((int64_t)in.i_atime, &d, &t) == 0) {
                    file->atime_date = d;
                }
            }
            {   // file->name is the basename, as fat_open_inner leaves it
                const char *b = path, *q = path;
                for (; *q; q++) if (*q == '/') b = q + 1;
                size_t k = 0;
                while (k + 1 < sizeof(file->name) && b[k] != 0) { file->name[k] = b[k]; k++; }
                file->name[k] = 0;
            }
            return 0;
        }
        // not on ext2: fall through to the FAT ESP below
    }
    if (!fat_lock()) return -1;
    int r = fat_open_inner(fs, path, file); fat_unlock(); return r;
}
int fat_read(fat_file_t *file, void *buffer, uint32_t size) {
    // #196: image-backed handle. diskimg_read_range streams out of the image
    // through the imgfile cache, so a 454 MB file on a mounted CD reads without
    // ever being resident.
    if (file && file->open && file->img_drive) {
        extern int64_t diskimg_read_range_gen(char letter, uint32_t gen, const char *relpath,
                                              uint64_t off, uint64_t len, void *dst);
        if (file->is_dir || !buffer) return -1;
        if (file->position >= file->file_size) return 0;
        uint32_t avail = file->file_size - file->position;
        if (size > avail) size = avail;
        if (size == 0) return 0;
        // #739: pass the stamp. If the disc was ejected or swapped since
        // fat_open(), this FAILS. A guest gets a read error, which is what a
        // program that had its disc pulled should see, instead of plausible
        // bytes off a different disc.
        int64_t got = diskimg_read_range_gen(file->img_drive, file->img_gen, file->img_rel,
                                             file->position, size, buffer);
        if (got < 0) return -1;
        file->position += (uint32_t)got;
        return (int)got;
    }
    // #725: ext2-backed handle. Bounded streaming read (ext2_read_file_range
    // touches only the covering blocks, #572), so a large file does not need a
    // whole-file kmalloc.
    if (file && file->open && file->ext2_ino) {
        if (file->is_dir || !buffer) return -1;
        if (file->position >= file->file_size) return 0;
        uint32_t avail = file->file_size - file->position;
        if (size > avail) size = avail;
        if (size == 0) return 0;
        int64_t got = ext2_read_file_range(file->ext2_ino, file->position, size, buffer);
        if (got < 0) return -1;
        file->position += (uint32_t)got;
        return (int)got;
    }
    // #250: the medium this handle describes has been removed. Fail, rather
    // than read sectors off whatever occupies that hotplug slot now.
    if (fat_handle_stale(file)) return -1;
    if (!fat_lock()) return -1;
    int r = fat_read_inner(file, buffer, size); fat_unlock(); return r;
}
int fat_readdir_n(fat_file_t *dir, fat_dir_entry_t *entry, char *name_out, size_t name_cap) {
    // #196: image-backed directory. `position` is the entry INDEX here (a byte
    // offset would mean exposing the image's on-disc record layout to callers).
    if (dir && dir->open && dir->img_drive) {
        extern int diskimg_readdir_n_gen(char letter, uint32_t gen, const char *relpath,
                                         unsigned index, char *name_out, int name_cap,
                                         int *isdir_out, unsigned *size_out);
        if (!dir->is_dir) return -1;
        char nm[128]; int isdir = 0; unsigned sz = 0;
        // #739: same stamp check as fat_read. A directory cursor that survived
        // a disc swap would otherwise walk the NEW disc from the OLD index.
        if (diskimg_readdir_n_gen(dir->img_drive, dir->img_gen, dir->img_rel, dir->position,
                                  nm, (int)sizeof nm, &isdir, &sz) != 1)
            return -1;
        dir->position++;
        if (name_out && name_cap > 0) {
            size_t k = 0;
            while (k + 1 < name_cap && nm[k]) { name_out[k] = nm[k]; k++; }
            name_out[k] = 0;
        }
        if (entry) {
            memset(entry, 0, sizeof(*entry));
            ext2_name_to_83(nm, entry->name);
            entry->attr = isdir ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
            if (!isdir) entry->file_size = sz;
        }
        return 0;
    }
    if (dir && dir->open && dir->ext2_ino) return fat_readdir_ext2(dir, entry, name_out, name_cap);
    if (fat_handle_stale(dir)) return -1;   // #250: medium removed
    if (!fat_lock()) return -1;
    int r = fat_readdir_inner(dir, entry, name_out, name_cap); fat_unlock(); return r;
}
void fat_list_dir(fat_fs_t *fs, const char *path) {
    if (!fat_lock()) return;
    fat_list_dir_inner(fs, path); fat_unlock();
}
// #99 cutover: true when ext2 is the root fs and `path` is a normal "/" path
// that should be served from the ext2 volume. The UEFI ESP paths (/boot, /EFI)
// are never redirected; they must stay on the FAT ESP the firmware boots from.
int fat_path_on_ext2(fat_fs_t *fs, const char *path) {
    extern fat_fs_t g_fat_fs;
    if (!g_root_ext2 || fs != &g_fat_fs || !path || path[0] != '/') return 0;
    if (path[1]=='b'&&path[2]=='o'&&path[3]=='o'&&path[4]=='t'&&(path[5]=='/'||path[5]==0)) return 0;
    if (path[1]=='E'&&path[2]=='F'&&path[3]=='I'&&(path[4]=='/'||path[4]==0)) return 0;
    return 1;
}

// #316/#725: map a fat-layer path to its ext2-volume path. Userland strips the
// "/ext2" mount prefix in sys_open (ext2_relpath); kernel-internal callers of
// fat_*_file may still pass "/ext2/...", so strip it here too ("/ext2/a"->"/a",
// "/ext2"->"/"). All other paths (incl. /HOME, /CONFIG, /APPS) pass through
// unchanged, so existing ext2-root persistence is unaffected.
const char *fat_ext2_vol_path(const char *path) {
    if (path && path[0]=='/' && path[1]=='e' && path[2]=='x' && path[3]=='t' &&
        path[4]=='2' && (path[5]=='/' || path[5]==0))
        return (path[5]==0) ? "/" : (path+5);
    return path;
}

int fat_create(fat_fs_t *fs, const char *path) {
    // Create an (empty) file on ext2 when it is root; ext2 has no standalone
    // create, so an empty write produces a zero-length file.
    if (fat_path_on_ext2(fs, path)) return (ext2_write_file(fat_ext2_vol_path(path), "", 0) == 0) ? 0 : -1;
    if (!fat_lock()) return -1;
    int r = fat_create_inner(fs, path); fat_unlock(); return r;
}
int fat_mkdir(fat_fs_t *fs, const char *path) {
    // task #578: preserve ext2_mkdir()'s distinct "-2 == already exists"
    // return instead of collapsing every non-zero result to a generic -1.
    // Callers up the stack (sys_mkdir -> userland mkdir() -> errno) rely on
    // being able to tell "the directory is already there" (POSIX EEXIST=17)
    // apart from a real failure; collapsing both to -1 made EVERY caller's
    // EEXIST check (e.g. ioquake3's Sys_Mkdir/FS_CreatePath, ported
    // verbatim in userland/apps/openarena/sys_maytera.c) silently wrong,
    // not just for OpenArena. -17 matches userland/libc/errno.h's EEXIST so
    // unistd.c's mkdir() (fixed alongside this) can set errno=17 directly.
    if (fat_path_on_ext2(fs, path)) {
        int er = ext2_mkdir(fat_ext2_vol_path(path));
        if (er == 0) return 0;
        if (er == -2) return -17;   // EEXIST
        return -1;
    }
    if (!fat_lock()) return -1;
    int r = fat_mkdir_inner(fs, path); fat_unlock(); return r;
}
int fat_delete(fat_fs_t *fs, const char *path) {
    if (fat_path_on_ext2(fs, path)) {
        int er = ext2_unlink(fat_ext2_vol_path(path));
        if (er == 0) return 0;
        // #736: -2 means "that is a DIRECTORY", which ext2_unlink refuses by
        // design. fat.h documents fat_delete as removing "a file or empty
        // directory", so the directory case has to go somewhere, and falling
        // through to the FAT ESP (which does not have the file at all) turned
        // it into a silent failure on every ext2-rooted image. INT 21h AH=3Ah
        // measured cf=1 ax=0005 because of exactly this.
        if (er == -2) {
            int rr = ext2_rmdir(fat_ext2_vol_path(path));
            if (rr == 0) return 0;
            return rr;            // -3 = not empty, preserved for the caller
        }
        // not on ext2 (or failed): fall back to FAT (file may be ESP-only)
    }
    if (!fat_lock()) return -1;
    int r = fat_delete_inner(fs, path); fat_unlock(); return r;
}
int fat_rename(fat_fs_t *fs, const char *old_path, const char *new_path) {
    // #746: ext2 DOES have an in-place rename now (ext2_rename), and this used
    // to be fat_copy + fat_delete: it read and rewrote every byte, so a failure
    // half way left the destination TRUNCATED. That is precisely why
    // write-temp-then-rename was not a safe save on this system. Relink instead;
    // no file data is touched at all.
    if (fat_path_on_ext2(fs, old_path) && fat_path_on_ext2(fs, new_path)) {
        int er = ext2_rename(fat_ext2_vol_path(old_path), fat_ext2_vol_path(new_path));
        if (er == 0) return 0;
        // -2 means "one endpoint is a directory", which ext2_rename refuses by
        // design. Fall back to the old copy+delete for THAT case only, so the
        // Files app keeps working on directories; a regular-file rename never
        // reaches a data copy again.
        if (er == -2) {
            if (fat_copy(fs, old_path, new_path) != 0) return -1;
            return fat_delete(fs, old_path);
        }
        return -1;
    }
    if (!fat_lock()) return -1;
    int r = fat_rename_inner(fs, old_path, new_path); fat_unlock(); return r;
}
// #746 O_TRUNC. The legacy FAT open path ignored the flags word entirely, so
// O_TRUNC on an existing FAT file was a NO-OP: the new contents were written
// over the old ones from byte 0 and everything past the new end survived as a
// stale tail, while the SAME flag on the ext2 root emptied the file. Two
// filesystems, one flag, opposite behaviour, and nothing said so.
//
// Empties the file: frees the cluster chain and writes size 0 / cluster 0 back
// to the directory entry. The handle is left usable and positioned at 0.
// Refuses the two backings that have no FAT cluster chain, for the same reason
// fat_write() refuses them.
// #745 local 109: THE one FAT truncation primitive. See fat.h for why it had
// to grow a length argument.
int fat_truncate_to(fat_file_t *file, uint32_t new_size) {
    if (!file || !file->fs || !file->open) return -1;
    if (file->img_drive) return -1;     // #196 read-only disk image
    if (file->ext2_ino)  return -1;     // #725 ext2-backed handle
    if (file->is_dir)    return -1;
    // A grow needs bytes written to the medium, not clusters freed. Refusing
    // is the honest answer; the no-op that returned success is the defect this
    // is here to end.
    if (new_size > file->file_size) return -1;
    if (new_size == file->file_size) return 0;
    if (!fat_lock()) return -1;

    fat_fs_t *fs = file->fs;
    uint32_t bpc = fs->bytes_per_sector * fs->sectors_per_cluster;
    int rc = 0;

    if (new_size == 0) {
        if (file->first_cluster >= 2) {
            if (fat_free_cluster_chain(fs, file->first_cluster) != 0) rc = -1;
        }
        file->first_cluster   = 0;
        file->current_cluster = 0;
    } else if (bpc == 0) {
        rc = -1;                        // unmounted / malformed geometry
    } else {
        // Keep exactly the clusters that hold bytes [0, new_size), terminate
        // the chain there, and free everything after it.
        uint32_t keep = (new_size + bpc - 1) / bpc;      // always >= 1
        uint32_t last = file->first_cluster;
        uint32_t i = 1;
        while (last >= 2 && i < keep) {
            last = fat_next_cluster(fs, last);
            i++;
        }
        if (last < 2) {
            // The chain is SHORTER than the size the directory entry records.
            // Rewriting the size to match would hide a corrupt file; refuse.
            rc = -1;
        } else {
            uint32_t tail = fat_next_cluster(fs, last);
            uint32_t eoc  = (fs->fat_type == FAT_TYPE_32) ? FAT32_EOC :
                            (fs->fat_type == FAT_TYPE_16) ? FAT16_EOC : 0xFFF;
            if (fat_set_fat_entry(fs, last, eoc) != 0) rc = -1;
            if (tail >= 2 && fat_free_cluster_chain(fs, tail) != 0) rc = -1;
        }
    }

    if (rc == 0) {
        file->file_size = new_size;
        // POSIX: ftruncate does not move the file offset. Re-anchor
        // current_cluster against the (possibly clamped) position by walking
        // from the start, exactly as fat_seek does, because the cluster the
        // handle was parked on may have just been freed.
        uint32_t pos = (file->position > new_size) ? new_size : file->position;
        file->current_cluster = file->first_cluster;
        uint32_t walked = 0;
        while (bpc && walked + bpc <= pos && file->current_cluster != 0) {
            walked += bpc;
            file->current_cluster = fat_next_cluster(fs, file->current_cluster);
        }
        file->position = pos;
        if (fat_persist_dirent(file) != 0) rc = -1;
    }
    fat_unlock();
    return rc;
}

int fat_truncate(fat_file_t *file) {
    if (!file) return -1;
    if (file->file_size == 0) {
        // fat_truncate_to() short-circuits a no-change truncate, but the O_TRUNC
        // caller wants the handle reset even when the file is already empty.
        file->first_cluster = 0; file->current_cluster = 0; file->position = 0;
        return 0;
    }
    return fat_truncate_to(file, 0);
}

int fat_write(fat_file_t *file, const void *buffer, uint32_t size) {
    // #725: an ext2-backed handle has no FAT cluster chain, so fat_write_inner
    // would write into whatever cluster 0 maps to. Refuse honestly instead.
    // Whole-file and append writes to ext2 go through fat_write_file() /
    // ext2_append_file(); handle-based streaming write to ext2 is not
    // implemented (see the CHANGELOG note on INT 21h 40h).
    // #196: no write-back to a mounted disk image is implemented, and E: is a
    // CD-ROM in any case. Refuse rather than corrupt or silently no-op.
    if (file && file->img_drive) return -1;
    if (file && file->ext2_ino) return -1;
    if (fat_handle_stale(file)) return -1;   // #250: medium removed
    if (!fat_lock()) return -1;
    int r = fat_write_inner(file, buffer, size); fat_unlock(); return r;
}
int fat_copy(fat_fs_t *fs, const char *src_path, const char *dst_path) {
    // #316: fat_copy_inner uses raw FAT file handles (fat_open/read/write),
    // which cannot see files that live only on the ext2 root volume. When
    // either endpoint is on ext2, do a routed whole-file read+write so the
    // copy actually reads from / writes to ext2. This also fixes ext2 rename
    // (fat_rename does copy+delete for ext2 paths).
    if (fat_path_on_ext2(fs, src_path) || fat_path_on_ext2(fs, dst_path)) {
        uint32_t sz = 0;
        void *data = fat_read_file(fs, src_path, &sz);  // ext2-first read
        if (!data) return -1;
        int rc = fat_write_file(fs, dst_path, data, sz); // ext2 write if dst on ext2
        kfree(data);
        return rc;
    }
    if (!fat_lock()) return -1;
    int r = fat_copy_inner(fs, src_path, dst_path); fat_unlock(); return r;
}
int fat_move(fat_fs_t *fs, const char *src_path, const char *dst_path) {
    // #725: fat_move had NO ext2 redirect while its sibling fat_rename did, so
    // Files' cut+paste of an ext2-root file moved nothing (fat_move_inner only
    // ever saw the FAT ESP). ext2 has no in-place rename, so do the same
    // copy+delete fat_rename() does; both halves route through the hooks above.
    if (fat_path_on_ext2(fs, src_path) || fat_path_on_ext2(fs, dst_path)) {
        // #746: when BOTH ends are on ext2 this is a rename, so relink instead
        // of copying every byte. A cross-volume move (one end on the FAT ESP)
        // genuinely has to copy, and keeps the old path.
        if (fat_path_on_ext2(fs, src_path) && fat_path_on_ext2(fs, dst_path)) {
            int er = ext2_rename(fat_ext2_vol_path(src_path), fat_ext2_vol_path(dst_path));
            if (er == 0) return 0;
            if (er != -2) return -1;   // -2 = directory: fall through to copy
        }
        if (fat_copy(fs, src_path, dst_path) != 0) return -1;
        return fat_delete(fs, src_path);
    }
    if (!fat_lock()) return -1;
    int r = fat_move_inner(fs, src_path, dst_path); fat_unlock(); return r;
}
int fat_write_file(fat_fs_t *fs, const char *path, const void *data, uint32_t size) {
    // (#133) Complete the ext2-root cutover: "/" writes must land on the ext2
    // root, matching fat_read_file (which already serves "/" from ext2). Without
    // this, writes went to the FAT ESP while reads came from ext2 - so a written
    // file (e.g. C:\WINDOWS\WIN.INI under /WINDIR) could never be read back.
    // /boot and /EFI stay on the FAT ESP (excluded by fat_path_on_ext2).
    if (fat_path_on_ext2(fs, path)) return (ext2_write_file(fat_ext2_vol_path(path), data, size) == 0) ? 0 : -1;
    if (!fat_lock()) return -1;
    int r = fat_write_file_inner(fs, path, data, size); fat_unlock(); return r;
}
int fat_exists(fat_fs_t *fs, const char *path) {
    // #99 Phase C / #568: mirror fat_read_file's ext2-root dispatch. With ext2
    // as the root fs, an existence test on a normal "/" path must consult the
    // ext2 volume FIRST; fat_exists_inner (below) only sees the FAT ESP, so it
    // falsely reported every ext2-only file (e.g. /CONFIG/PASSWD, /APPS/*) as
    // absent (#568). ext2_resolve_path returns a non-zero inode for a present
    // file OR directory without reading it, and 0 if absent (or ext2 unmounted,
    // so this is safe when ext2 is not the root). Fall through to the FAT ESP
    // check when the path is not on ext2, or when ext2 does not have it (an
    // ESP-only file), matching fat_read_file's ext2-first-then-FAT fallback.
    // #193: a mounted disk image owns its drive's subtree, and fat_exists()
    // cannot borrow fat_open()'s redirect the way fat_read_file() does, because
    // fat_exists_inner() below only ever sees the FAT ESP. So the image is
    // asked here, in fat_open()'s order and with fat_open()'s semantics: a
    // mounted disc is AUTHORITATIVE for its own letter, so a miss is a miss and
    // does not fall back to the folder underneath. Before this, an existence
    // test on a file that is on the disc but not in the folder said "absent"
    // while opening the very same path succeeded.
    {
        extern int diskimg_stat(char letter, const char *relpath,
                               uint64_t *size_out, int *isdir_out);
        extern int diskimg_is_mounted(char letter);
        unsigned int roff = 0;
        int li = drvmap_windir_split_rs((const unsigned char *)path, &roff);
        if (li >= 0 && diskimg_is_mounted((char)('A' + li))) {
            uint64_t isz = 0; int isdir = 0;
            return diskimg_stat((char)('A' + li), path + roff, &isz, &isdir) ? 1 : 0;
        }
    }
    if (fat_path_on_ext2(fs, path)) {
        if (ext2_resolve_path(fat_ext2_vol_path(path)) != 0) return 1;
        // not present on ext2: fall through to the FAT ESP check below
    }
    // A lock failure must NOT read as "present": callers test this for
    // truth, so -1 would mean the opposite of what happened.
    if (!fat_lock()) return 0;
    int r = fat_exists_inner(fs, path); fat_unlock(); return r;
}
uint32_t fat_get_free_clusters(fat_fs_t *fs) {
    if (!fat_lock()) return 0;
    uint32_t r = fat_get_free_clusters_inner(fs); fat_unlock(); return r;
}

// Helper to extract channel and drive from drive ID
// Drive ID: 0=Primary Master, 1=Primary Slave, 2=Secondary Master, 3=Secondary Slave
static inline uint8_t drive_to_channel(int drive) {
    return (drive >> 1) & 1;  // 0,1 -> 0;  2,3 -> 1
}

static inline uint8_t drive_to_unit(int drive) {
    return drive & 1;  // 0,2 -> master(0);  1,3 -> slave(1)
}

// ===========================================================================
// #250: WHICH BLOCK DEVICE DOES THIS MOUNT READ FROM?
//
// ONE answer, asked by every sector operation below, so a removable volume
// cannot be half-routed. Zero (every mount that predates #250, because both
// mount paths memset the struct) means the ROOT device and the call is
// byte-identical to what it was. Non-zero means a hot-plugged USB MSC device,
// which must NOT go through blk_read/blk_write: those consult a RAM cache
// keyed by LBA alone, holding sectors of the root device only.
// ===========================================================================
static inline int fat_aux_vol(const fat_fs_t *fs) {
    return fs->usb_vol_p1 ? (int)(fs->usb_vol_p1 - 1) : -1;
}

// Read sector from partition
static int fat_read_sector(fat_fs_t *fs, uint32_t sector, void *buffer) {
    uint32_t lba = fs->part_start_lba + sector;
    int av = fat_aux_vol(fs);
    if (av >= 0) return blk_read_aux(av, lba, 1, buffer);
    uint8_t channel = drive_to_channel(fs->drive);
    uint8_t unit = drive_to_unit(fs->drive);
    return blk_read(channel, unit, lba, 1, buffer);
}

// Read multiple sectors
static int fat_read_sectors(fat_fs_t *fs, uint32_t sector, uint32_t count, void *buffer) {
    uint32_t lba = fs->part_start_lba + sector;
    int av = fat_aux_vol(fs);
    if (av >= 0) return blk_read_aux(av, lba, count, buffer);
    uint8_t channel = drive_to_channel(fs->drive);
    uint8_t unit = drive_to_unit(fs->drive);
    return blk_read(channel, unit, lba, count, buffer);
}

// Get next cluster in chain.
//
// Uses a LOCAL sector buffer instead of the shared global sector_buf. This is
// called once per cluster while reading a file (the cluster-chain walk). With
// the global buffer, a concurrent FAT operation in another preemptible context
// (e.g. a background service writing its log, the compositor saving its
// profile) could clobber sector_buf between the FAT-table read and the
// next-cluster extraction, making the read jump into a different file's
// clusters. The file then decodes as fragments of several files stitched
// together (seen as a wallpaper made of shifting "magazine clippings" with
// wrong colors). A per-call buffer makes the chain walk race-free.
// Single-entry FAT-sector read cache. A contiguous file walks many clusters
// that all live in the same FAT sector (a FAT32 sector holds 128 entries), so
// caching the last-read FAT sector turns one disk read per cluster into one per
// 128 clusters. Invalidated on every FAT-region write (see fat_write_sector*).
static fat_fs_t *g_fatcache_fs = 0;
static uint32_t  g_fatcache_sector = 0xFFFFFFFFu;
static uint8_t   g_fatcache_buf[512];
static inline void fat_cache_invalidate(void) {
    g_fatcache_fs = 0;
    g_fatcache_sector = 0xFFFFFFFFu;
}
// Return a pointer to a (cached) FAT sector's 512 bytes, or NULL on read error.
// The single-entry cache is invalidated on every FAT-region write
// (fat_write_sector / fat_write_sectors), so a chain walk that follows a freshly
// written link always re-reads it. (Verified innocent of the multi-cluster
// write corruption, which was a navigation bug in fat_write, not the cache.)
static uint8_t *fat_fat_sector(fat_fs_t *fs, uint32_t fat_sector) {
    if (g_fatcache_fs == fs && g_fatcache_sector == fat_sector) {
        return g_fatcache_buf;
    }
    if (fat_read_sector(fs, fat_sector, g_fatcache_buf) <= 0) {
        fat_cache_invalidate();
        return 0;
    }
    g_fatcache_fs = fs;
    g_fatcache_sector = fat_sector;
    return g_fatcache_buf;
}

static uint32_t fat_next_cluster(fat_fs_t *fs, uint32_t cluster) {
    uint32_t fat_offset;
    uint32_t fat_sector;
    uint32_t entry_offset;
    uint32_t next_cluster;
    uint8_t *fbuf;        // points into the cached FAT-sector buffer

    if (fs->fat_type == FAT_TYPE_16) {
        fat_offset = cluster * 2;
        fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
        entry_offset = fat_offset % fs->bytes_per_sector;

        if (!(fbuf = fat_fat_sector(fs, fat_sector))) {
            return 0;
        }

        next_cluster = *(uint16_t *)(fbuf + entry_offset);
        if (next_cluster >= FAT16_EOC) {
            return 0;  // End of chain
        }
    } else if (fs->fat_type == FAT_TYPE_32) {
        fat_offset = cluster * 4;
        fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
        entry_offset = fat_offset % fs->bytes_per_sector;

        if (!(fbuf = fat_fat_sector(fs, fat_sector))) {
            return 0;
        }

        next_cluster = *(uint32_t *)(fbuf + entry_offset) & 0x0FFFFFFF;
        if (next_cluster >= FAT32_EOC) {
            return 0;  // End of chain
        }
    } else {
        // FAT12 - more complex
        fat_offset = cluster + (cluster / 2);
        fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
        entry_offset = fat_offset % fs->bytes_per_sector;

        if (!(fbuf = fat_fat_sector(fs, fat_sector))) {
            return 0;
        }

        if (cluster & 1) {
            next_cluster = (*(uint16_t *)(fbuf + entry_offset)) >> 4;
        } else {
            next_cluster = (*(uint16_t *)(fbuf + entry_offset)) & 0x0FFF;
        }

        if (next_cluster >= 0xFF8) {
            return 0;  // End of chain
        }
    }

    return next_cluster;
}

// Convert cluster to LBA
static uint32_t cluster_to_lba(fat_fs_t *fs, uint32_t cluster) {
    return fs->data_start_lba + (cluster - 2) * fs->sectors_per_cluster;
}

// #418: public wrapper so fs/panic.c can resolve a pre-allocated file's first
// cluster to a partition-relative sector number ONCE at boot (armed under the
// normal fat_lock()), then cache that number and call fat_write_sector()
// directly (unlocked) from fault context forever after. Returns the same
// partition-relative "sector" units fat_read_sector()/fat_write_sector() take
// (i.e. fs->part_start_lba has NOT been added yet).
uint32_t fat_cluster_to_sector(fat_fs_t *fs, uint32_t cluster) {
    return cluster_to_lba(fs, cluster);
}

// Read a cluster
static int fat_read_cluster(fat_fs_t *fs, uint32_t cluster, void *buffer) {
    uint32_t lba = cluster_to_lba(fs, cluster);
    return fat_read_sectors(fs, lba, fs->sectors_per_cluster, buffer);
}

// ============================================
// Write Helper Functions
// ============================================

// Write sector to partition
//
// #418: intentionally NOT static. fs/panic.c needs to overwrite an
// ALREADY-ALLOCATED file's first sector directly from fault context (no
// fat_lock(), no directory traversal, no delete+recreate - see fs/panic.c's
// file header for why fat_write_file_inner()'s normal delete+create+patch-size
// sequence is unsafe to reuse there). This is the same raw primitive every
// other write path in this file already funnels through; exposing it does not
// change any existing locked call site.
// #742: 0 on success, -1 on failure. blk_write() speaks in SECTOR COUNTS; this
// is the boundary where that is translated into fat.h's 0/-1 status polarity,
// so no caller of a fat_* function has to remember which convention it is in.
int fat_write_sector(fat_fs_t *fs, uint32_t sector, const void *buffer) {
    fat_cache_invalidate();   // a FAT-region write may change next-cluster links
    uint32_t lba = fs->part_start_lba + sector;
    int av = fat_aux_vol(fs);   // #250
    if (av >= 0) return (blk_write_aux(av, lba, 1, buffer) == 1) ? 0 : -1;
    uint8_t channel = drive_to_channel(fs->drive);
    uint8_t unit = drive_to_unit(fs->drive);
    return (blk_write(channel, unit, lba, 1, buffer) == 1) ? 0 : -1;
}

// Write multiple sectors
static int fat_write_sectors(fat_fs_t *fs, uint32_t sector, uint32_t count, const void *buffer) {
    fat_cache_invalidate();
    uint32_t lba = fs->part_start_lba + sector;
    int av = fat_aux_vol(fs);   // #250
    if (av >= 0) return blk_write_aux(av, lba, count, buffer);
    uint8_t channel = drive_to_channel(fs->drive);
    uint8_t unit = drive_to_unit(fs->drive);
    return blk_write(channel, unit, lba, count, buffer);
}

// Write a cluster
static int fat_write_cluster(fat_fs_t *fs, uint32_t cluster, const void *buffer) {
    uint32_t lba = cluster_to_lba(fs, cluster);
    return fat_write_sectors(fs, lba, fs->sectors_per_cluster, buffer);
}

// Set FAT entry value (update the FAT table)
static int fat_set_fat_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value) {
    uint32_t fat_offset;
    uint32_t fat_sector;
    uint32_t entry_offset;

    if (fs->fat_type == FAT_TYPE_16) {
        fat_offset = cluster * 2;
    } else if (fs->fat_type == FAT_TYPE_32) {
        fat_offset = cluster * 4;
    } else {
        fat_offset = cluster + (cluster / 2);
    }

    fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
    entry_offset = fat_offset % fs->bytes_per_sector;

    // Read current FAT sector
    if (fat_read_sector(fs, fat_sector, sector_buf) <= 0) {
        return -1;
    }

    // Update entry
    if (fs->fat_type == FAT_TYPE_16) {
        *(uint16_t *)(sector_buf + entry_offset) = (uint16_t)value;
    } else if (fs->fat_type == FAT_TYPE_32) {
        // Preserve upper 4 bits
        uint32_t existing = *(uint32_t *)(sector_buf + entry_offset);
        *(uint32_t *)(sector_buf + entry_offset) = (existing & 0xF0000000) | (value & 0x0FFFFFFF);
    } else {
        // FAT12
        uint16_t existing = *(uint16_t *)(sector_buf + entry_offset);
        if (cluster & 1) {
            existing = (existing & 0x000F) | ((value & 0x0FFF) << 4);
        } else {
            existing = (existing & 0xF000) | (value & 0x0FFF);
        }
        *(uint16_t *)(sector_buf + entry_offset) = existing;
    }

    // Write back FAT sector (to both FAT copies)
    if (fat_write_sector(fs, fat_sector, sector_buf) != 0) {
        return -1;
    }

    // Write to second FAT if present
    if (fs->num_fats > 1) {
        uint32_t fat2_sector = fat_sector + fs->fat_size;
        // #695: was unchecked. A lost mirror write leaves FAT2 disagreeing with
        // FAT1. Deliberately NOT fatal: FAT1 is authoritative here and already
        // landed, so failing now would make the caller treat the allocation as
        // failed and leak the cluster it just claimed. Log it loudly instead,
        // because a silently divergent mirror is what a later repair tool may
        // "fix" in the wrong direction.
        if (fat_write_sector(fs, fat2_sector, sector_buf) != 0)
            kprintf("[FAT] FAT mirror write FAILED (sector %u); FAT2 now stale\n",
                    fat2_sector);
    }

    return 0;
}

// Find a free cluster
static uint32_t fat_alloc_cluster(fat_fs_t *fs) {
    uint32_t current_sector = 0xFFFFFFFF;

    for (uint32_t cluster = 2; cluster < fs->cluster_count + 2; cluster++) {
        uint32_t fat_offset;
        uint32_t fat_sector;
        uint32_t entry_offset;
        uint32_t entry_value;

        if (fs->fat_type == FAT_TYPE_16) {
            fat_offset = cluster * 2;
        } else if (fs->fat_type == FAT_TYPE_32) {
            fat_offset = cluster * 4;
        } else {
            fat_offset = cluster + (cluster / 2);
        }

        fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
        entry_offset = fat_offset % fs->bytes_per_sector;

        if (fat_sector != current_sector) {
            if (fat_read_sector(fs, fat_sector, sector_buf) <= 0) {
                return 0;
            }
            current_sector = fat_sector;
        }

        if (fs->fat_type == FAT_TYPE_16) {
            entry_value = *(uint16_t *)(sector_buf + entry_offset);
        } else if (fs->fat_type == FAT_TYPE_32) {
            entry_value = *(uint32_t *)(sector_buf + entry_offset) & 0x0FFFFFFF;
        } else {
            if (cluster & 1) {
                entry_value = (*(uint16_t *)(sector_buf + entry_offset)) >> 4;
            } else {
                entry_value = (*(uint16_t *)(sector_buf + entry_offset)) & 0x0FFF;
            }
        }

        if (entry_value == 0) {
            // Found free cluster - mark as end of chain
            uint32_t eoc = (fs->fat_type == FAT_TYPE_32) ? FAT32_EOC :
                          (fs->fat_type == FAT_TYPE_16) ? FAT16_EOC : 0xFFF;
            if (fat_set_fat_entry(fs, cluster, eoc) == 0) {
                    if (fs->free_cluster_count > 0) fs->free_cluster_count--;
                return cluster;
            }
            return 0;
        }
    }

    return 0;  // No free clusters
}

// Free a cluster chain
static int fat_free_cluster_chain(fat_fs_t *fs, uint32_t start_cluster) {
    if (start_cluster < 2) return 0;

    uint32_t cluster = start_cluster;
    while (cluster != 0 && cluster < fs->cluster_count + 2) {
        uint32_t next = fat_next_cluster(fs, cluster);
        if (fat_set_fat_entry(fs, cluster, 0) != 0) {
            return -1;
        }
        fs->free_cluster_count++;
        cluster = next;
    }
    return 0;
}

// Convert string to 8.3 FAT name format
static void str_to_fat_name(const char *name, uint8_t *fat_name) {
    memset(fat_name, ' ', 11);

    int i = 0, j = 0;

    // Copy name part (up to 8 chars)
    while (name[i] && name[i] != '.' && j < 8) {
        char c = name[i++];
        if (c >= 'a' && c <= 'z') c -= 32;  // Uppercase
        fat_name[j++] = c;
    }

    // Skip to extension
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') i++;

    // Copy extension (up to 3 chars)
    j = 8;
    while (name[i] && j < 11) {
        char c = name[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        fat_name[j++] = c;
    }
}

// (fat_name_match removed: replaced by the LFN-aware fat_lookup / fat_ci_eq.)

// Convert 8.3 name to readable string
static void fat_name_to_str(const uint8_t *name83, char *str_out) {
    int i, j = 0;

    // Copy name
    for (i = 0; i < 8 && name83[i] != ' '; i++) {
        str_out[j++] = name83[i];
    }

    // Add extension
    if (name83[8] != ' ') {
        str_out[j++] = '.';
        for (i = 8; i < 11 && name83[i] != ' '; i++) {
            str_out[j++] = name83[i];
        }
    }

    str_out[j] = '\0';
}

// Initialize FAT driver
void fat_init(void) {
    kprintf("[FAT] FAT filesystem driver initialized\n");
}

// Mount a FAT partition
int fat_mount(int drive, int partition, fat_fs_t *fs) {
    memset(fs, 0, sizeof(fat_fs_t));
    fs->drive = drive;
    fs->partition = partition;

    uint8_t channel = drive_to_channel(drive);
    uint8_t unit = drive_to_unit(drive);

    kprintf("[FAT] Mounting drive %d (channel %d, unit %d), partition %d\n",
            drive, channel, unit, partition);

    // Read MBR
    if (blk_read(channel, unit, 0, 1, sector_buf) <= 0) {
        kprintf("[FAT] Failed to read MBR\n");
        return -1;
    }

    mbr_t *mbr = (mbr_t *)sector_buf;

    // Check signature
    if (mbr->signature != 0xAA55) {
        kprintf("[FAT] Invalid MBR signature\n");
        return -1;
    }

    // Get partition info
    if (partition < 0 || partition > 3) {
        kprintf("[FAT] Invalid partition number\n");
        return -1;
    }

    mbr_partition_t *part = &mbr->partitions[partition];
    if (part->type == 0) {
        kprintf("[FAT] Partition %d is empty\n", partition);
        return -1;
    }

    // Check for valid FAT partition types
    uint8_t ptype = part->type;
    if (ptype != 0x01 && ptype != 0x04 && ptype != 0x06 && ptype != 0x0B &&
        ptype != 0x0C && ptype != 0x0E && ptype != 0x0F) {
        kprintf("[FAT] Partition %d type 0x%02x is not FAT\n", partition, ptype);
        if (ptype == 0xEE) {
            kprintf("[FAT] GPT partition - not supported yet\n");
        }
        return -1;
    }

    fs->part_start_lba = part->start_lba;
    fs->part_sectors = part->sector_count;

    kprintf("[FAT] Partition %d: LBA %u, %u sectors, type 0x%02x\n",
            partition, fs->part_start_lba, fs->part_sectors, part->type);

    // Read boot sector
    if (fat_read_sector(fs, 0, sector_buf) <= 0) {
        kprintf("[FAT] Failed to read boot sector\n");
        return -1;
    }

    fat_boot_sector_t *bs = (fat_boot_sector_t *)sector_buf;

    // Parse BPB
    fs->bytes_per_sector = bs->bytes_per_sector;
    fs->sectors_per_cluster = bs->sectors_per_cluster;
    fs->reserved_sectors = bs->reserved_sectors;
    fs->num_fats = bs->num_fats;
    fs->root_entry_count = bs->root_entries;

    // Validate critical BPB fields to avoid divide by zero
    if (fs->bytes_per_sector == 0 || fs->sectors_per_cluster == 0) {
        kprintf("[FAT] Invalid BPB: bytes/sector=%u sectors/cluster=%u\n",
                fs->bytes_per_sector, fs->sectors_per_cluster);
        return -1;
    }

    if (bs->total_sectors_16 != 0) {
        fs->total_sectors = bs->total_sectors_16;
    } else {
        fs->total_sectors = bs->total_sectors_32;
    }

    if (bs->fat_size_16 != 0) {
        fs->fat_size = bs->fat_size_16;
    } else {
        fs->fat_size = bs->ext.fat32.fat_size_32;
    }

    // Calculate locations
    fs->fat_start_lba = fs->reserved_sectors;

    fs->root_dir_sectors = ((fs->root_entry_count * 32) + (fs->bytes_per_sector - 1))
                            / fs->bytes_per_sector;

    fs->root_start_lba = fs->reserved_sectors + (fs->num_fats * fs->fat_size);
    fs->data_start_lba = fs->root_start_lba + fs->root_dir_sectors;

    // Calculate cluster count and determine FAT type
    uint32_t data_sectors = fs->total_sectors - fs->data_start_lba;
    fs->cluster_count = data_sectors / fs->sectors_per_cluster;

    if (fs->cluster_count < 4085) {
        fs->fat_type = FAT_TYPE_12;
    } else if (fs->cluster_count < 65525) {
        fs->fat_type = FAT_TYPE_16;
    } else {
        fs->fat_type = FAT_TYPE_32;
        fs->root_cluster = bs->ext.fat32.root_cluster;
    }

    // Get volume label
    if (fs->fat_type == FAT_TYPE_32) {
        memcpy(fs->volume_label, bs->ext.fat32.volume_label, 11);
    } else {
        memcpy(fs->volume_label, bs->ext.fat16.volume_label, 11);
    }
    fs->volume_label[11] = '\0';

    // Trim trailing spaces
    for (int i = 10; i >= 0 && fs->volume_label[i] == ' '; i--) {
        fs->volume_label[i] = '\0';
    }

    fs->mounted = 1;
    // Count free clusters once at mount time (cached for taskbar gauge)
    fs->free_cluster_count = 0;
    // #250: NOT for a hot-plugged volume. This loop walks the WHOLE FAT one
    // 512-byte sector at a time; on the root device those reads are served
    // from the TO-RAM copy, but an aux volume has no cache, so every one is a
    // real SCSI transfer. A 32 GB FAT32 stick has a ~16,000-sector FAT, i.e.
    // ~16,000 round trips before the drive would appear in the UI. This
    // project's recorded failure mode is exactly that (#71/#427 unbounded
    // codec scan, #365 5s-per-empty-port probe), so the removable path
    // reports free space as UNKNOWN rather than making the user wait. The
    // volume record carries MOSVOL_FREE_UNKNOWN and the UI says so.
    if (fs->usb_vol_p1 == 0) {
        uint32_t cur_sec = 0xFFFFFFFF;
        for (uint32_t c = 2; c < fs->cluster_count + 2; c++) {
            uint32_t fo = (fs->fat_type == FAT_TYPE_32) ? c * 4 : c * 2;
            uint32_t fs_sec = fs->reserved_sectors + (fo / fs->bytes_per_sector);
            uint32_t eo = fo % fs->bytes_per_sector;
            if (fs_sec != cur_sec) {
                if (fat_read_sector(fs, fs_sec, sector_buf) <= 0) break;
                cur_sec = fs_sec;
            }
            uint32_t ev = (fs->fat_type == FAT_TYPE_32) ?
                (*(uint32_t *)(sector_buf + eo) & 0x0FFFFFFF) :
                *(uint16_t *)(sector_buf + eo);
            if (ev == 0) fs->free_cluster_count++;
        }
        kprintf("[FAT] Free clusters: %u\n", fs->free_cluster_count);
    }

    kprintf("[FAT] Mounted FAT%d filesystem: %s\n", fs->fat_type, fs->volume_label);
    kprintf("[FAT] %u clusters, %u bytes/cluster\n",
            fs->cluster_count, fs->sectors_per_cluster * fs->bytes_per_sector);

    return 0;
}

// Mount FAT from a specific LBA offset (for GPT partitions or raw FAT volumes)
//
// #250: `aux_vol` is -1 for the ROOT block device (every caller before #250,
// and what fat_mount_lba() still passes) or a USB MSC device index for a
// hot-plugged volume. `gen` is 0 for a fixed mount and a unique non-zero
// stamp for a removable one; see the vol_gen comment in fat.h.
static int fat_mount_lba_inner(int drive, uint32_t start_lba, fat_fs_t *fs,
                               int aux_vol, uint32_t gen) {
    memset(fs, 0, sizeof(fat_fs_t));
    fs->drive = drive;
    fs->partition = -1;  // Indicates raw LBA mount
    fs->part_start_lba = start_lba;
    // Set BEFORE the first sector read below, or the boot sector would be read
    // off the root device and the whole mount would describe the wrong medium.
    fs->usb_vol_p1 = (aux_vol >= 0) ? (uint32_t)(aux_vol + 1) : 0;
    fs->vol_gen = gen;

    uint8_t channel = drive_to_channel(drive);
    uint8_t unit = drive_to_unit(drive);

    kprintf("[FAT] Mounting from drive %d (channel %d, unit %d), LBA %u, aux vol %d\n",
            drive, channel, unit, start_lba, aux_vol);

    // Read boot sector directly from specified LBA
    if ((aux_vol >= 0 ? blk_read_aux(aux_vol, start_lba, 1, sector_buf)
                      : blk_read(channel, unit, start_lba, 1, sector_buf)) <= 0) {
        kprintf("[FAT] Failed to read boot sector at LBA %u\n", start_lba);
        return -1;
    }

    // Debug: dump first 16 bytes of boot sector
    kprintf("[FAT] Boot sector bytes: ");
    for (int i = 0; i < 16; i++) {
        kprintf("%02x ", sector_buf[i]);
    }
    kprintf("\n");

    fat_boot_sector_t *bs = (fat_boot_sector_t *)sector_buf;

    // Check for FAT signature (some basic validation)
    // Valid FAT boot sector should have a jump instruction at start
    if (bs->jmp[0] != 0xEB && bs->jmp[0] != 0xE9) {
        kprintf("[FAT] Invalid boot sector (no jump instruction)\n");
        return -1;
    }

    // Parse BPB
    fs->bytes_per_sector = bs->bytes_per_sector;
    fs->sectors_per_cluster = bs->sectors_per_cluster;
    fs->reserved_sectors = bs->reserved_sectors;
    fs->num_fats = bs->num_fats;
    fs->root_entry_count = bs->root_entries;

    // Validate critical BPB fields
    if (fs->bytes_per_sector == 0 || fs->sectors_per_cluster == 0) {
        kprintf("[FAT] Invalid BPB: bytes/sector=%u sectors/cluster=%u\n",
                fs->bytes_per_sector, fs->sectors_per_cluster);
        return -1;
    }

    if (bs->total_sectors_16 != 0) {
        fs->total_sectors = bs->total_sectors_16;
    } else {
        fs->total_sectors = bs->total_sectors_32;
    }

    fs->part_sectors = fs->total_sectors;

    if (bs->fat_size_16 != 0) {
        fs->fat_size = bs->fat_size_16;
    } else {
        fs->fat_size = bs->ext.fat32.fat_size_32;
    }

    // Calculate locations
    fs->fat_start_lba = fs->reserved_sectors;

    fs->root_dir_sectors = ((fs->root_entry_count * 32) + (fs->bytes_per_sector - 1))
                            / fs->bytes_per_sector;

    fs->root_start_lba = fs->reserved_sectors + (fs->num_fats * fs->fat_size);
    fs->data_start_lba = fs->root_start_lba + fs->root_dir_sectors;

    // Calculate cluster count and determine FAT type
    uint32_t data_sectors = fs->total_sectors - fs->data_start_lba;
    fs->cluster_count = data_sectors / fs->sectors_per_cluster;

    if (fs->cluster_count < 4085) {
        fs->fat_type = FAT_TYPE_12;
    } else if (fs->cluster_count < 65525) {
        fs->fat_type = FAT_TYPE_16;
    } else {
        fs->fat_type = FAT_TYPE_32;
        fs->root_cluster = bs->ext.fat32.root_cluster;
    }

    // Copy volume label
    const uint8_t *label;
    if (fs->fat_type == FAT_TYPE_32) {
        label = bs->ext.fat32.volume_label;
    } else {
        label = bs->ext.fat16.volume_label;
    }
    memcpy(fs->volume_label, label, 11);
    fs->volume_label[11] = '\0';

    // Trim trailing spaces
    for (int i = 10; i >= 0 && fs->volume_label[i] == ' '; i--) {
        fs->volume_label[i] = '\0';
    }

    fs->mounted = 1;
    // Count free clusters once at mount time (cached for taskbar gauge).
    // #250: NOT on a removable volume; see the long comment on the identical
    // guard in fat_mount() above for why (thousands of uncached SCSI reads
    // between plugging a stick in and it appearing).
    fs->free_cluster_count = 0;
    if (fs->usb_vol_p1 == 0) {
        uint32_t cur_sec = 0xFFFFFFFF;
        for (uint32_t c = 2; c < fs->cluster_count + 2; c++) {
            uint32_t fo = (fs->fat_type == FAT_TYPE_32) ? c * 4 : c * 2;
            uint32_t fs_sec = fs->reserved_sectors + (fo / fs->bytes_per_sector);
            uint32_t eo = fo % fs->bytes_per_sector;
            if (fs_sec != cur_sec) {
                if (fat_read_sector(fs, fs_sec, sector_buf) <= 0) break;
                cur_sec = fs_sec;
            }
            uint32_t ev = (fs->fat_type == FAT_TYPE_32) ?
                (*(uint32_t *)(sector_buf + eo) & 0x0FFFFFFF) :
                *(uint16_t *)(sector_buf + eo);
            if (ev == 0) fs->free_cluster_count++;
        }
        kprintf("[FAT] Free clusters: %u\n", fs->free_cluster_count);
    } else {
        kprintf("[FAT] Removable volume: free-cluster scan skipped (bounded hotplug)\n");
    }

    kprintf("[FAT] Mounted FAT%d filesystem: %s\n", fs->fat_type, fs->volume_label);
    kprintf("[FAT] %u clusters, %u bytes/cluster\n",
            fs->cluster_count, fs->sectors_per_cluster * fs->bytes_per_sector);

    return 0;
}

// #250: the two public entry points. Both are thin: fat_mount_lba() is the
// pre-#250 signature and behaviour exactly (root device, generation 0), and
// fat_mount_lba_usb() is the removable one. They share ONE body so a fix to
// BPB parsing can never apply to only one kind of medium.
int fat_mount_lba(int drive, uint32_t start_lba, fat_fs_t *fs) {
    return fat_mount_lba_inner(drive, start_lba, fs, -1, 0);
}

int fat_mount_lba_usb(int usb_index, uint32_t start_lba, uint32_t gen, fat_fs_t *fs) {
    if (usb_index < 0 || gen == 0 || !fs) return -1;
    // The whole mount is one sector_buf read-modify-write span, and unlike the
    // boot-time mount this one runs with the scheduler live and the desktop
    // using the root filesystem. Without the FAT lock it would race every
    // other FAT context for the shared global buffer, which is the exact
    // corruption b103 recorded.
    if (!fat_lock()) return -1;
    int r = fat_mount_lba_inner(0, start_lba, fs, usb_index, gen);
    fat_unlock();
    return r;
}

// #250: ONE definition of "this handle's medium is gone". See fat.h.
int fat_handle_stale(const fat_file_t *file) {
    if (!file || !file->fs) return 0;
    return file->vol_gen != file->fs->vol_gen;
}

// Unmount
void fat_unmount(fat_fs_t *fs) {
    if (fs->mounted) {
        fs->mounted = 0;
        kprintf("[FAT] Filesystem unmounted\n");
    }
}

// ===================== VFAT long-filename (LFN) core =====================
// Checksum of the 11-byte 8.3 name, stamped into every LFN entry of the set.
static uint8_t lfn_checksum(const uint8_t *short11) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + short11[i]);
    return sum;
}

// True if `name` cannot be stored as a clean uppercase 8.3 name (needs LFN).
static int fat_name_needs_lfn(const char *name) {
    int base = 0, ext = 0, dots = 0, in_ext = 0;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (c == '.') { dots++; in_ext = 1; continue; }
        if (c >= 'a' && c <= 'z') return 1;                 // preserve case
        if (c=='+'||c==','||c==';'||c=='='||c=='['||c==']'||c==' ') return 1;
        if (in_ext) ext++; else base++;
    }
    if (dots > 1 || base > 8 || ext > 3 || base == 0) return 1;
    return 0;
}

// Map a logical directory-entry index to its physical sector + byte offset.
// allocate=1 extends the directory's cluster chain when idx is past the end
// (cluster dirs only; the FAT16 root region is fixed-size). Returns 0 / -1.
static int dir_locate(fat_fs_t *fs, fat_file_t *dir, uint32_t idx,
                      uint32_t *sector, uint32_t *off, int allocate) {
    uint32_t per_sector = fs->bytes_per_sector / 32;
    if (dir->first_cluster == 0 && fs->fat_type != FAT_TYPE_32) {
        if (idx >= fs->root_entry_count) return -1;
        *sector = fs->root_start_lba + idx / per_sector;
        *off = (idx % per_sector) * 32;
        return 0;
    }
    uint32_t per_cluster = (fs->bytes_per_sector * fs->sectors_per_cluster) / 32;
    uint32_t cluster = dir->first_cluster;
    for (uint32_t k = 0; k < idx / per_cluster; k++) {
        uint32_t nxt = fat_next_cluster(fs, cluster);
        if (nxt == 0) {
            if (!allocate) return -1;
            uint32_t nc = fat_alloc_cluster(fs);
            if (!nc) return -1;
            fat_set_fat_entry(fs, cluster, nc);
            uint32_t cb = fs->bytes_per_sector * fs->sectors_per_cluster;
            uint8_t *zb = kmalloc(cb);
            if (zb) { memset(zb, 0, cb); fat_write_cluster(fs, nc, zb); kfree(zb); }
            nxt = nc;
        }
        cluster = nxt;
    }
    uint32_t within = idx % per_cluster;
    *sector = cluster_to_lba(fs, cluster) + within / per_sector;
    *off = (within % per_sector) * 32;
    return 0;
}

// Read the 32-byte entry at logical index. Returns its first name byte, or 0x00
// (virtual free / end) if past the end. Copies bytes to out32 if non-NULL.
static int dir_read_entry(fat_fs_t *fs, fat_file_t *dir, uint32_t idx, uint8_t *out32) {
    uint32_t sec, off;
    if (dir_locate(fs, dir, idx, &sec, &off, 0) != 0) return 0x00;
    if (fat_read_sector(fs, sec, sector_buf) <= 0) return -1;
    if (out32) memcpy(out32, sector_buf + off, 32);
    return sector_buf[off];
}

// Write a 32-byte entry at logical index (extends the directory if needed).
static int dir_write_entry(fat_fs_t *fs, fat_file_t *dir, uint32_t idx, const uint8_t *in32) {
    uint32_t sec, off;
    if (dir_locate(fs, dir, idx, &sec, &off, 1) != 0) return -1;
    if (fat_read_sector(fs, sec, sector_buf) <= 0) return -1;
    memcpy(sector_buf + off, in32, 32);
    if (fat_write_sector(fs, sec, sector_buf) != 0) return -1;
    return 0;
}

// Pull the (up to) 13 chars of one LFN entry into out (ASCII subset).
static int lfn_extract(const uint8_t *e, char *out) {
    static const int slot[13] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
    int n = 0;
    for (int k = 0; k < 13; k++) {
        uint16_t ch = (uint16_t)(e[slot[k]] | (e[slot[k]+1] << 8));
        if (ch == 0x0000 || ch == 0xFFFF) break;
        out[n++] = (ch < 0x80) ? (char)ch : '_';
    }
    return n;
}

static int fat_ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        a++; b++;
    }
    return *a == *b;
}

// ===================== #404 Rust-port seam: pure per-entry dir/LFN decode ====
// The VFAT directory walk (fat_readdir_inner, below) is an untrusted-input
// parser: it decodes 32-byte on-disk directory entries and reassembles VFAT
// long-file-name (LFN) fragments straight from an attacker-supplyable FAT image.
// The golden is FAT-root, so this runs at every boot (e.g. ttf_init() scans
// /FONTS) and from userland via SYS_READDIR, on bytes a malicious USB stick /
// disk fully controls. The per-entry decode + LFN reassembly is lifted here as a
// PURE seam (no disk I/O, no FS mutation) so it can be ported to Rust behind
// -DRUST_FAT and differentially proven byte-identical to the C on real entries.
// dir_read_entry() (the cluster-chain walk + sector read) stays in C.
//
// fat_lfn_state_t carries the LFN accumulation across the entries of one
// directory walk. fat_parsed_entry_t is the decoded output. Layout is asserted
// so the #[repr(C)] Rust mirror in rustkern.rs can never silently drift.
typedef struct {
    char longname[260];   // reassembled UTF-16 -> ASCII long name (in progress)
    int  have_long;       // 1 while an LFN set is being accumulated
} fat_lfn_state_t;

typedef struct {
    uint8_t raw[32];      // the short directory entry (valid on FAT_STEP_SHORT)
    char    name[260];    // decoded name (long or 8.3), NUL-terminated
} fat_parsed_entry_t;

_Static_assert(sizeof(fat_lfn_state_t) == 264, "fat_lfn_state_t must be 264 bytes for the Rust FFI");
_Static_assert(sizeof(fat_parsed_entry_t) == 292, "fat_parsed_entry_t must be 292 bytes for the Rust FFI");

#define FAT_STEP_CONTINUE 0   // LFN fragment or 0xE5 deleted entry consumed
#define FAT_STEP_SHORT    1   // short entry decoded into *out
#define FAT_STEP_END      2   // 0x00 end-of-directory marker
// #490: downstream caller name buffers are 256 bytes == 255 chars + NUL.
// Both fat_dir_step_c and fat_dir_step_rs cap the emitted name to this.
#define FAT_NAME_MAX      255

// Rust mirror (rustkern.rs), live under -DRUST_FAT. It CONFINES the reachable
// overflow the C reference has downstream (see below).
extern int fat_dir_step_rs(fat_lfn_state_t *st, const uint8_t *e, fat_parsed_entry_t *out);

// fat_dir_step_c: VERBATIM reference. This is the decode logic lifted, byte for
// byte, out of the original fat_readdir_inner loop: the 0x00 end marker, the
// 0xE5 deleted-entry reset, the e[11]==FAT_ATTR_LFN branch with the EXACT
// (seq-1)*13<255 bound the C already had, lfn_extract, and the short-entry
// long-vs-8.3 name selection. It does NOT validate the LFN checksum (neither did
// the original readdir/lookup) - kept identical so the differential is honest.
//
// The internal longname[260] reassembly is self-contained safe (the
// (seq-1)*13<255 guard caps the last write index at 259). #490 HARDENING
// (2026-07-26): the OLD reference could EMIT a name of up to 259 chars, which
// fat_readdir_inner then strcpy()s into a 256-byte caller buffer -> a REACHABLE
// heap overflow on a crafted LFN set when this C fallback ships (-DRUST_FAT
// dropped). It now caps the emitted name at FAT_NAME_MAX (255), matching
// fat_dir_step_rs; on any real name (< 256 chars) it is byte-identical to before.
// It still does NOT validate the LFN checksum (neither did the original
// readdir/lookup). See fat_rust_selftest's [RUST-SEC].
int fat_dir_step_c(fat_lfn_state_t *st, const uint8_t *e, fat_parsed_entry_t *out) {
    uint8_t b = e[0];
    if (b == 0x00) return FAT_STEP_END;
    if (b == 0xE5) { st->have_long = 0; return FAT_STEP_CONTINUE; }
    if (e[11] == FAT_ATTR_LFN) {
        int seq = e[0] & 0x3F;
        if (e[0] & 0x40) { st->have_long = 1; memset(st->longname, 0, sizeof(st->longname)); }
        if (st->have_long && seq >= 1 && (seq - 1) * 13 < 255) {
            char piece[16]; int n = lfn_extract(e, piece);
            for (int j = 0; j < n; j++) st->longname[(seq - 1) * 13 + j] = piece[j];
        }
        return FAT_STEP_CONTINUE;
    }
    // short entry
    memcpy(out->raw, e, 32);
    if (st->have_long) {
        st->longname[259] = 0;
        // #490 confinement: copy at most FAT_NAME_MAX (255) chars, stopping
        // at the first NUL, so the downstream strcpy(name_out, out->name) in
        // fat_readdir_inner cannot overflow a 256-byte caller buffer.
        int k = 0;
        while (k < FAT_NAME_MAX && st->longname[k] != 0) { out->name[k] = st->longname[k]; k++; }
        out->name[k] = 0;
    }
    else fat_name_to_str(e, out->name);
    return FAT_STEP_SHORT;
}

// LFN-aware directory lookup. Matches `name` case-insensitively against either
// the reconstructed long name or the 8.3 (alias) name. On success returns 0,
// fills *entry_out (short entry, 32 bytes), and the logical indices of the short
// entry (*short_idx) and the first LFN entry of its set (*first_idx).
static int fat_lookup(fat_fs_t *fs, uint32_t dir_cluster, const char *name,
                      fat_dir_entry_t *entry_out, uint32_t *short_idx, uint32_t *first_idx) {
    fat_file_t d; memset(&d, 0, sizeof(d)); d.fs = fs; d.first_cluster = dir_cluster; d.is_dir = 1;
    char longname[260]; int have_long = 0; uint32_t lfn_start = 0;
    for (uint32_t idx = 0; idx < 200000; idx++) {
        uint8_t e[32];
        int b = dir_read_entry(fs, &d, idx, e);
        if (b < 0) return -1;
        if (b == 0x00) return -1;                  // end of directory
        if (b == 0xE5) { have_long = 0; continue; }
        if (e[11] == FAT_ATTR_LFN) {
            int seq = e[0] & 0x3F;
            if (e[0] & 0x40) { have_long = 1; lfn_start = idx; memset(longname, 0, sizeof(longname)); }
            if (have_long && seq >= 1 && (seq - 1) * 13 < 255) {
                char piece[16]; int n = lfn_extract(e, piece);
                for (int j = 0; j < n; j++) longname[(seq - 1) * 13 + j] = piece[j];
            }
            continue;
        }
        // short entry
        char s83[13]; fat_name_to_str(e, s83);
        int match = (have_long && fat_ci_eq(longname, name)) || fat_ci_eq(s83, name);
        if (match) {
            memcpy(entry_out, e, 32);
            if (short_idx) *short_idx = idx;
            if (first_idx) *first_idx = have_long ? lfn_start : idx;
            return 0;
        }
        have_long = 0;
    }
    return -1;
}

// Compatibility wrapper (same signature/semantics as before; now LFN-aware).
// out_lba/out_off point at the SHORT directory entry.
static int fat_find_in_dir(fat_fs_t *fs, uint32_t dir_cluster, const char *name,
                           fat_dir_entry_t *entry_out,
                           uint32_t *out_lba, uint32_t *out_off) {
    fat_dir_entry_t e; uint32_t sidx, fidx;
    if (fat_lookup(fs, dir_cluster, name, &e, &sidx, &fidx) != 0) return -1;
    *entry_out = e;
    if (out_lba) {
        fat_file_t d; memset(&d, 0, sizeof(d)); d.fs = fs; d.first_cluster = dir_cluster; d.is_dir = 1;
        uint32_t sec, off;
        if (dir_locate(fs, &d, sidx, &sec, &off, 0) == 0) { *out_lba = sec; *out_off = off; }
    }
    return 0;
}

// Build an uppercase 8.3 base + ext from a long name (for alias generation).
static void fat_make_short_base(const char *name, char *base, char *ext) {
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    int bi = 0;
    for (const char *p = name; *p && p != dot && bi < 8; p++) {
        char c = *p;
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='~')) c = '_';
        base[bi++] = c;
    }
    base[bi] = 0;
    int ei = 0;
    if (dot) for (const char *p = dot + 1; *p && ei < 3; p++) {
        char c = *p;
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='~')) c = '_';
        ext[ei++] = c;
    }
    ext[ei] = 0;
    if (bi == 0) { base[0] = '_'; base[1] = 0; }
}

// Generate a unique "BASE~N.EXT" 8.3 alias for a long name. out83 = 11 bytes.
static int fat_gen_short_alias(fat_fs_t *fs, uint32_t dir_cluster,
                               const char *longname, uint8_t out83[11]) {
    char base[16], ext[8];
    fat_make_short_base(longname, base, ext);
    int blen = 0; while (base[blen]) blen++;
    for (int n = 1; n < 1000000; n++) {
        char num[8]; int nl = 0, t = n; char tmp[8]; int ti = 0;
        do { tmp[ti++] = (char)('0' + (t % 10)); t /= 10; } while (t);
        while (ti) num[nl++] = tmp[--ti];
        int keep = 8 - 1 - nl; if (keep < 1) keep = 1;
        int bl = (blen > keep) ? keep : blen;
        memset(out83, ' ', 11);
        for (int i = 0; i < bl; i++) out83[i] = (uint8_t)base[i];
        out83[bl] = '~';
        for (int i = 0; i < nl; i++) out83[bl + 1 + i] = (uint8_t)num[i];
        for (int i = 0; ext[i] && i < 3; i++) out83[8 + i] = (uint8_t)ext[i];
        char alias[13]; fat_name_to_str(out83, alias);
        fat_dir_entry_t tmpe;
        if (fat_find_in_dir(fs, dir_cluster, alias, &tmpe, 0, 0) != 0) return 0;  // free
    }
    return -1;
}

// Write a directory record set for `longname`: N LFN entries (if needed) plus the
// short entry (whose .name is the 8.3 alias). Places them in a contiguous free
// run, extending the directory as needed. Returns 0 on success.
static int fat_write_entry_set(fat_fs_t *fs, fat_file_t *dir, const char *longname,
                               const fat_dir_entry_t *short_entry) {
    int need = fat_name_needs_lfn(longname);
    int namelen = 0; while (longname[namelen]) namelen++;
    if (namelen > 255) namelen = 255;
    int nlfn = need ? ((namelen + 12) / 13) : 0;
    int total = nlfn + 1;

    // Find a contiguous run of `total` free entries (0x00 end region or 0xE5 holes).
    uint32_t run_start = 0, run_len = 0, start = 0; int have = 0, reached_end = 0;
    for (uint32_t idx = 0; idx < 200000; idx++) {
        int b = dir_read_entry(fs, dir, idx, 0);
        if (b < 0) return -1;
        if (b == 0x00) { if (run_len == 0) run_start = idx; reached_end = 1; start = run_start; have = 1; break; }
        if (b == 0xE5) {
            if (run_len == 0) run_start = idx;
            if (++run_len == (uint32_t)total) { start = run_start; have = 1; break; }
        } else {
            run_len = 0;
        }
    }
    if (!have) return -1;
    if (dir->first_cluster == 0 && fs->fat_type != FAT_TYPE_32 &&
        start + (uint32_t)total + (reached_end ? 1 : 0) > fs->root_entry_count) return -1;

    uint8_t checksum = lfn_checksum(short_entry->name);
    static const int slot[13] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
    for (int seq = nlfn; seq >= 1; seq--) {
        uint8_t e[32]; memset(e, 0, 32);
        e[0] = (uint8_t)(seq | ((seq == nlfn) ? 0x40 : 0x00));
        e[11] = FAT_ATTR_LFN; e[12] = 0; e[13] = checksum;
        int cbase = (seq - 1) * 13;
        for (int k = 0; k < 13; k++) {
            int ci = cbase + k;
            uint16_t ch = (ci < namelen) ? (uint16_t)(uint8_t)longname[ci]
                        : (ci == namelen) ? 0x0000 : 0xFFFF;
            e[slot[k]] = (uint8_t)(ch & 0xFF);
            e[slot[k] + 1] = (uint8_t)(ch >> 8);
        }
        if (dir_write_entry(fs, dir, start + (uint32_t)(nlfn - seq), e) != 0) return -1;
    }
    if (dir_write_entry(fs, dir, start + (uint32_t)nlfn, (const uint8_t *)short_entry) != 0) return -1;
    if (reached_end) {
        uint8_t z[32]; memset(z, 0, 32);
        dir_write_entry(fs, dir, start + (uint32_t)total, z);  // preserve end marker
    }
    return 0;
}

// Open file/directory
static int fat_open_inner(fat_fs_t *fs, const char *path, fat_file_t *file) {
    if (!fs->mounted) {
        return -1;
    }


    memset(file, 0, sizeof(fat_file_t));
    file->fs = fs;
    // #250: stamp WHICH medium this handle describes. Zero on every fixed
    // mount, so fat_handle_stale() is a compare against zero there. On a
    // removable volume it is what makes a pulled stick fail the handle
    // instead of serving whatever is in that slot next. Same mechanism and
    // same reason as img_gen (#739), one struct up.
    file->vol_gen = fs->vol_gen;

    // Handle root directory
    if (path[0] == '/' && path[1] == '\0') {
        file->is_dir = 1;
        file->open = 1;
        file->attr = FAT_ATTR_DIRECTORY;
        if (fs->fat_type == FAT_TYPE_32) {
            file->first_cluster = fs->root_cluster;
            file->current_cluster = fs->root_cluster;
        } else {
            file->first_cluster = 0;
            file->current_cluster = 0;
        }
        strcpy(file->name, "/");
        return 0;
    }

    // Parse path
    const char *p = path;
    if (*p == '/') p++;

    uint32_t current_cluster = (fs->fat_type == FAT_TYPE_32) ? fs->root_cluster : 0;
    fat_dir_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    int found = 0;
    uint32_t found_lba = 0, found_off = 0;

    while (*p) {
        // Extract next path component
        char component[256];
        int i = 0;
        while (*p && *p != '/' && i < 255) {
            component[i++] = *p++;
        }
        component[i] = '\0';

        if (*p == '/') p++;

        // Find in current directory
        if (fat_find_in_dir(fs, current_cluster, component, &entry, &found_lba, &found_off) != 0) {
            return -1;  // Not found
        }
        found = 1;

        // Get cluster for this entry
        current_cluster = ((uint32_t)entry.cluster_hi << 16) | entry.cluster_lo;

        // If there's more path, this must be a directory
        if (*p && !(entry.attr & FAT_ATTR_DIRECTORY)) {
            return -1;  // Not a directory
        }
    }

    // Must have found something
    if (!found) {
        return -1;
    }

    // Fill file structure
    file->first_cluster = ((uint32_t)entry.cluster_hi << 16) | entry.cluster_lo;
    file->current_cluster = file->first_cluster;
    file->file_size = entry.file_size;
    file->attr = entry.attr;
    file->is_dir = (entry.attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
    file->position = 0;
    file->open = 1;
    file->dirent_lba = found_lba;
    file->dirent_off = found_off;
    // #115: the directory entry is already in hand here; carrying its date/time
    // words costs nothing and saves sys_stat_path() a second read of the same
    // sector. K1 was not "FAT has no timestamp", it was "nobody carried it".
    file->mtime_date = entry.modify_date;
    file->mtime_time = entry.modify_time;
    file->atime_date = entry.access_date;
    file->ctime_date = entry.create_date;
    file->ctime_time = entry.create_time;
    fat_name_to_str(entry.name, file->name);

    return 0;
}

// ===========================================================================
// #115: FAT TIMESTAMP STAMPING.
//
// fat_create_inner() built its short directory entry with memset(0) + name +
// attr and wrote it. A FAT entry with create_date == 0 is not "1980-01-01", it
// is UNSTAMPED, and that is what every file MayteraOS has ever written to a FAT
// volume carries. gui/properties.c has been rendering the consequence to users
// for its whole life: it decodes those zero words and prints "1980-00-00
// 00:00", a date with a zeroth day of a zeroth month.
//
// So reading the field is only half the ticket. These two helpers are the other
// half, and they are shared so the create path and the write path cannot drift
// into stamping differently.
//
// IF THE CLOCK DOES NOT KNOW, WE DO NOT STAMP: wallclock_now_unix() returns 0
// when the RTC has no plausible date and ktime_unix_to_dos_rs() refuses
// anything outside FAT's 1980..2107 range WITHOUT writing its outputs, so the
// entry keeps whatever it had. An unstamped entry is honest; a fabricated one
// is the bug.
// ===========================================================================
static void fat_stamp_new_entry(fat_dir_entry_t *de) {
    uint16_t d = 0, t = 0;
    if (ktime_unix_to_dos_rs(wallclock_now_unix(), &d, &t) != 0) return;
    de->create_date = d;
    de->create_time = t;
    de->create_time_tenth = 0;
    de->access_date = d;
    de->modify_date = d;
    de->modify_time = t;
}

static void fat_stamp_modified(fat_dir_entry_t *de) {
    uint16_t d = 0, t = 0;
    if (ktime_unix_to_dos_rs(wallclock_now_unix(), &d, &t) != 0) return;
    de->modify_date = d;
    de->modify_time = t;
    de->access_date = d;
}

// Close file
void fat_close(fat_file_t *file) {
    file->open = 0;
    // #725: clear the ext2 backing too, so a reused stack handle cannot be
    // mistaken for an ext2 handle by fat_read/fat_write after a failed reopen.
    file->ext2_ino = 0;
    file->ext2_dirpos = 0;
    // #196: and the disk-image backing, for the same reason. A stale img_drive
    // on a reused stack handle would send the next fat_read at a disc that is
    // no longer the one this handle was opened on.
    file->img_drive = 0;
    file->img_rel[0] = 0;
    file->img_gen = 0;
    // #115: and the cached dirent times, for the same reason - a reused stack
    // handle must not report the PREVIOUS file's timestamp after a failed
    // reopen. That is exactly how #725 and #196 shipped.
    file->mtime_date = 0;
    file->mtime_time = 0;
    file->atime_date = 0;
    file->ctime_date = 0;
    file->ctime_time = 0;
}

// Read from file
static int fat_read_inner(fat_file_t *file, void *buffer, uint32_t size) {
    if (!file->open || file->is_dir) return -1;

    fat_fs_t *fs = file->fs;
    uint32_t bytes_per_cluster = fs->sectors_per_cluster * fs->bytes_per_sector;
    uint8_t *cluster_buf = kmalloc(bytes_per_cluster);
    if (!cluster_buf) return -1;

    uint32_t bytes_read = 0;
    uint8_t *out = (uint8_t *)buffer;
    // kprintf("[FAT] buffer=%p out=%p\n", buffer, out);
    uint32_t progress_interval = size / 10;  // Report every 10%
    uint32_t next_progress = progress_interval;

    // Limit read to file size
    if (file->position + size > file->file_size) {
        size = file->file_size - file->position;
    }

    while (bytes_read < size && file->current_cluster != 0) {
        // Read current cluster
        if (fat_read_cluster(fs, file->current_cluster, cluster_buf) <= 0) {
            kprintf("[FAT] Read failed at cluster %u\n", file->current_cluster);
            break;
        }

        // Calculate offset in cluster
        uint32_t cluster_offset = file->position % bytes_per_cluster;
        uint32_t bytes_in_cluster = bytes_per_cluster - cluster_offset;
        uint32_t bytes_to_copy = (size - bytes_read < bytes_in_cluster) ?
                                  (size - bytes_read) : bytes_in_cluster;

        // kprintf("[FAT] memcpy dest=%p src=%p len=%u\n", out + bytes_read, cluster_buf + cluster_offset, bytes_to_copy);
        // #19/#645: `out` is the CALLER's buffer, and on the sys_read path it
        // is a raw Ring-3 pointer (proc/fdlayer.c hands it straight through).
        // fat_read() also has many kernel callers, so this is an AC bracket on
        // the one copy rather than a copy_to_user.
        {   uaccess_ac_t __ac = uaccess_begin();
            memcpy(out + bytes_read, cluster_buf + cluster_offset, bytes_to_copy);
            uaccess_end(__ac); }
        bytes_read += bytes_to_copy;
        file->position += bytes_to_copy;

        // Progress indicator
        if (bytes_read >= next_progress) {
            // (Progress logging disabled)
            // (Progress logging disabled)
            next_progress += progress_interval;
        }

        // Move to next cluster if needed
        if (file->position % bytes_per_cluster == 0) {
            file->current_cluster = fat_next_cluster(fs, file->current_cluster);
        }
    }

    kfree(cluster_buf);
    return bytes_read;
}

// Get file size
uint32_t fat_size(fat_file_t *file) {
    return file->file_size;
}

// Seek in file
int fat_seek(fat_file_t *file, uint32_t position) {
    if (!file->open) return -1;
    if (fat_handle_stale(file)) return -1;   // #250: medium removed
    if (position > file->file_size) position = file->file_size;
    // #725: an ext2-backed handle has no cluster chain to walk; the position IS
    // the state (ext2_read_file_range takes an absolute byte offset). This is
    // what makes INT 21h 42h (lseek) work on an ext2 root.
    // #196: an image-backed handle has no cluster chain either; the position is
    // an absolute byte offset handed straight to diskimg_read_range. This is
    // what makes INT 21h 42h (lseek) work on a mounted CD.
    if (file->img_drive) { file->position = position; return 0; }
    if (file->ext2_ino) { file->position = position; return 0; }

    fat_fs_t *fs = file->fs;
    uint32_t bytes_per_cluster = fs->sectors_per_cluster * fs->bytes_per_sector;

    // Reset to start
    file->current_cluster = file->first_cluster;
    file->position = 0;

    // Skip clusters
    while (file->position + bytes_per_cluster <= position && file->current_cluster != 0) {
        file->position += bytes_per_cluster;
        file->current_cluster = fat_next_cluster(fs, file->current_cluster);
    }

    file->position = position;
    return 0;
}

// Read directory entry
static int fat_readdir_inner(fat_file_t *dir, fat_dir_entry_t *entry, char *name_out, size_t name_cap) {
    if (!dir->open || !dir->is_dir) return -1;

    fat_fs_t *fs = dir->fs;
    uint32_t idx = dir->position / 32;
    // #404: the per-entry decode + LFN reassembly moved into the pure seam
    // fat_dir_step_c / fat_dir_step_rs (routed by -DRUST_FAT). The b<0 (disk read
    // error) and b==0x00 (end; e may be uninitialized on the dir_locate-fail
    // path) checks stay on the RETURN VALUE here, exactly as before, so the seam
    // is only ever handed a fully-read 32-byte entry.
    fat_lfn_state_t st; st.have_long = 0;

    for (;; idx++) {
        uint8_t e[32];
        int b = dir_read_entry(fs, dir, idx, e);
        if (b < 0) { dir->position = idx * 32; return -1; }
        if (b == 0x00) { dir->position = (idx + 1) * 32; return -1; }   // end

        fat_parsed_entry_t pe;
#ifdef RUST_FAT
        int r = fat_dir_step_rs(&st, e, &pe);
#else
        int r = fat_dir_step_c(&st, e, &pe);
#endif
        if (r == FAT_STEP_END) { dir->position = (idx + 1) * 32; return -1; }
        if (r == FAT_STEP_SHORT) {
            memcpy(entry, pe.raw, 32);
            // #591 bounded emit: copy at most name_cap-1 chars + NUL, never
            // past the caller's buffer. pe.name is already <=255 (the #490 cap /
            // -DRUST_FAT), but bounding here makes the primitive safe for ANY
            // buffer size, so a smaller caller truncates instead of overflowing.
            if (name_cap > 0) {
                size_t k = 0;
                while (k + 1 < name_cap && pe.name[k] != 0) { name_out[k] = pe.name[k]; k++; }
                name_out[k] = 0;
            }
            dir->position = (idx + 1) * 32;
            return 0;
        }
        // FAT_STEP_CONTINUE: LFN fragment or deleted entry consumed.
    }
}

// ============================================================================
// #404 Phase S boot-time self-test: prove fat_dir_step_rs (Rust, live under
// -DRUST_FAT) == fat_dir_step_c (verbatim reference) on the agreement domain
// (well-formed VFAT directories: LFN sets + short entries, deleted 0xE5,
// orphan LFN, bad seq numbers), characterize the SECURITY divergence HONESTLY,
// and micro-benchmark both. LIGHT (#426, bounded, runs once): ~1000 file
// decodes + a small security sweep + a ~5k-iter RDTSC walk. The heavy work
// (>=2M-vector differential + ASan/UBSan witness of the C overflow) is the
// OFFLINE pre-flight. One [RUST-DIFF] fat, one [RUST-SEC] fat, one [RUST-PERF].
//
// SAFETY at boot: every crafted directory lives in a static buffer, and the seam
// writes only into fat_parsed_entry_t.name[260], which holds even the maximal
// 259-char reference name without overflow. The REACHABLE overflow is the
// DOWNSTREAM strcpy into an undersized caller buffer; this self-test never does
// that copy (it reads pe.name directly), so the C reference stays in-bounds here.
// ============================================================================
static uint32_t fatdiff_rng(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x;
}
static inline uint64_t fat_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

// Build one 32-byte LFN entry for sequence `seq` (1-based) of `name`.
static void fat_build_lfn_entry(uint8_t *e, int seq, int is_last,
                                const char *name, int namelen) {
    static const int slot[13] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
    memset(e, 0, 32);
    e[0] = (uint8_t)(seq | (is_last ? 0x40 : 0x00));
    e[11] = FAT_ATTR_LFN;
    int start = (seq - 1) * 13;
    for (int k = 0; k < 13; k++) {
        int ci = start + k;
        uint16_t ch;
        if (ci < namelen)       ch = (uint16_t)(uint8_t)name[ci];
        else if (ci == namelen) ch = 0x0000;
        else                    ch = 0xFFFF;
        e[slot[k]]     = (uint8_t)(ch & 0xFF);
        e[slot[k] + 1] = (uint8_t)((ch >> 8) & 0xFF);
    }
}
static void fat_build_short(uint8_t *e, const char *n83, uint8_t attr) {
    memset(e, 0, 32);
    for (int i = 0; i < 11; i++) e[i] = n83[i] ? (uint8_t)n83[i] : ' ';
    e[11] = attr;
}
// Emit a full VFAT record set (N LFN entries top-down + 1 short) for `name`
// into buf at *pos. namelen chars, ASCII. Returns via *pos advance.
static void fat_emit_file(uint8_t *buf, int *pos, const char *name, int namelen,
                          const char *n83) {
    int nlfn = (namelen + 12) / 13;
    if (nlfn < 1) nlfn = 1;
    for (int seq = nlfn; seq >= 1; seq--) {
        fat_build_lfn_entry(buf + (*pos) * 32, seq, seq == nlfn, name, namelen);
        (*pos)++;
    }
    fat_build_short(buf + (*pos) * 32, n83, FAT_ATTR_ARCHIVE);
    (*pos)++;
}
// Walk a directory of `nent` 32-byte entries with the C or Rust step, copying
// each decoded short-entry name into out_names[]. Returns the count.
static int fat_walk(const uint8_t *dir, int nent, char out_names[][260], int use_rust) {
    fat_lfn_state_t st; st.have_long = 0;
    int cnt = 0;
    for (int i = 0; i < nent && cnt < 8; i++) {
        fat_parsed_entry_t pe;
        int r = use_rust ? fat_dir_step_rs(&st, dir + i * 32, &pe)
                         : fat_dir_step_c(&st, dir + i * 32, &pe);
        if (r == FAT_STEP_END) break;
        if (r == FAT_STEP_SHORT) { strcpy(out_names[cnt], pe.name); cnt++; }
    }
    return cnt;
}

void fat_rust_selftest(void) {
    static uint8_t buf[2048];
    static char names_c[8][260];
    static char names_r[8][260];
    static char nm[300];

    // Force-reference the Rust symbol so its archive member always links
    // (matches the icmp/arp/dns/dhcp/url/elf/pe pattern), regardless of RUST_FAT.
    { fat_lfn_state_t st; st.have_long = 0; fat_parsed_entry_t pe;
      uint8_t z[32]; memset(z, 0, 32); fat_dir_step_rs(&st, z, &pe); }

    // Part 1: agreement domain. Random well-formed directories (1..3 files, each
    // a random-length ASCII long name 1..250 chars + short alias), plus periodic
    // structural mutations (deleted 0xE5, orphan LFN with no short, bad seq).
    // Both walks must produce the identical sequence of decoded names.
    uint32_t seed = 0xfa7c0de1u;
    uint32_t files = 0, mismatches = 0;
    int first_bad = -1;
    for (uint32_t iter = 0; iter < 400; iter++) {
        int pos = 0;
        int nfiles = 1 + (int)(fatdiff_rng(&seed) % 3);
        for (int f = 0; f < nfiles && pos < 56; f++) {
            int len = 1 + (int)(fatdiff_rng(&seed) % 250);
            for (int c = 0; c < len; c++) {
                // printable ASCII 0x21..0x7E (avoid space / control)
                nm[c] = (char)(0x21 + (fatdiff_rng(&seed) % (0x7E - 0x21)));
            }
            nm[len] = 0;
            char n83[12]; for (int i = 0; i < 11; i++) n83[i] = (char)('A' + (i + f) % 26); n83[11] = 0;
            fat_emit_file(buf, &pos, nm, len, n83);
        }
        // structural mutations on some iterations (still must AGREE)
        uint32_t mut = fatdiff_rng(&seed) % 6;
        if (mut == 1 && pos > 0) buf[0] = 0xE5;                 // first entry deleted
        else if (mut == 2 && pos > 1) buf[(pos - 1) * 32] = 0xE5; // final short deleted (orphan LFN)
        else if (mut == 3 && pos > 0) buf[0] = (buf[0] & 0xC0) | 0x3F; // seq -> 63 (guard rejects)
        else if (mut == 4 && pos > 0) buf[0] = (buf[0] & 0x40);        // seq -> 0 (guard rejects)
        memset(buf + pos * 32, 0, 32);                          // 0x00 terminator
        int nent = pos + 1;

        int cc = fat_walk(buf, nent, names_c, 0);
        int rc = fat_walk(buf, nent, names_r, 1);
        int bad = (cc != rc);
        for (int i = 0; i < cc && i < rc && !bad; i++)
            if (strcmp(names_c[i], names_r[i]) != 0) bad = 1;
        files += (cc > rc ? rc : cc);
        if (bad) { mismatches++; if (first_bad < 0) first_bad = (int)iter; }
    }
    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] fat: %u files, %u mismatches -> %s\n", files, mismatches, verdict);
    bootlog_write("[RUST-DIFF] fat: %u files, %u mismatches -> %s", files, mismatches, verdict);
    if (mismatches != 0) {
        kprintf("[RUST-DIFF] fat FIRST MISMATCH iter=%d\n", first_bad);
        bootlog_write("[RUST-DIFF] fat FIRST MISMATCH iter=%d", first_bad);
    }

    // Part 2: SECURITY posture (HONEST). The reachable class: an LFN name of
    // 256..259 chars. The verbatim C emits the full name (up to 259 chars); the
    // downstream strcpy in fat_readdir_inner then overflows a 256-byte name_buf
    // (SYS_READDIR), a 64-byte buffer (fat_delete's dir-empty scan) or a 16-byte
    // one (dosexec find), by up to 4 / 195 / 243 bytes. The Rust confines the
    // decoded name to <=255 chars so that downstream copy can never exceed 256.
    // (Reading pe.name here is in-bounds for both; the ASan witness of the heap
    // overflow is the OFFLINE pre-flight.)
    {
        uint32_t n = 0, c_overlong = 0, r_overlong = 0, divergences = 0;
        uint32_t s2 = 0x5ec0fa7u;
        for (uint32_t r = 0; r < 200; r++) {
            int len = 256 + (int)(fatdiff_rng(&s2) % 4);   // 256..259
            for (int c = 0; c < len; c++) nm[c] = (char)(0x41 + (fatdiff_rng(&s2) % 26));
            nm[len] = 0;
            int pos = 0;
            fat_emit_file(buf, &pos, nm, len, "OVERLONGTXT");
            memset(buf + pos * 32, 0, 32);
            fat_lfn_state_t st; fat_parsed_entry_t pe;
            // C reference
            st.have_long = 0; int lc = 0;
            for (int i = 0; i <= pos; i++) { int rr = fat_dir_step_c(&st, buf + i * 32, &pe);
                if (rr == FAT_STEP_SHORT) { lc = (int)strlen(pe.name); break; }
                if (rr == FAT_STEP_END) break; }
            // Rust
            st.have_long = 0; int lr = 0;
            for (int i = 0; i <= pos; i++) { int rr = fat_dir_step_rs(&st, buf + i * 32, &pe);
                if (rr == FAT_STEP_SHORT) { lr = (int)strlen(pe.name); break; }
                if (rr == FAT_STEP_END) break; }
            n++;
            if (lc > 255) c_overlong++;
            if (lr > 255) r_overlong++;
            if (lc != lr) divergences++;
        }
        // #490 REGRESSION GUARD (2026-07-26): fat_dir_step_c is now hardened to
        // cap the emitted name at 255 chars, matching fat_dir_step_rs, so on
        // these crafted 256..259-char LFN sets BOTH paths must confine and the
        // downstream strcpy into a 256-byte name_buf can no longer overflow.
        const char *sverdict = (c_overlong == 0 && r_overlong == 0) ? "PASS" : "FAIL";
        kprintf("[RUST-SEC] fat: #490 both paths confine crafted 256..259-char LFN names to <=255 - "
                "C overlong=%u/%u Rust overlong=%u/%u divergences=%u -> %s\n",
                c_overlong, n, r_overlong, n, divergences, sverdict);
        bootlog_write("[RUST-SEC] fat: #490 C overlong=%u/%u Rust overlong=%u/%u div=%u -> %s",
                      c_overlong, n, r_overlong, n, divergences, sverdict);
    }

    // Part 3: RDTSC micro-benchmark over a fixed well-formed directory. LIGHT: 5k.
    {
        const int iters = 5000;
        int pos = 0;
        fat_emit_file(buf, &pos, "ExampleLongFileName.txt", 23, "EXAMPLETXT");
        fat_emit_file(buf, &pos, "readme.md", 9, "READMEMD ");
        memset(buf + pos * 32, 0, 32);
        int nent = pos + 1;

        for (int i = 0; i < 300; i++) { fat_walk(buf, nent, names_c, 0); fat_walk(buf, nent, names_r, 1); }
        uint64_t t0 = fat_tsc_serialized();
        for (int i = 0; i < iters; i++) fat_walk(buf, nent, names_c, 0);
        uint64_t t1 = fat_tsc_serialized();
        for (int i = 0; i < iters; i++) fat_walk(buf, nent, names_r, 1);
        uint64_t t2 = fat_tsc_serialized();
        uint64_t c_cyc = (t1 - t0) / iters;
        uint64_t r_cyc = (t2 - t1) / iters;
        uint64_t ratio100 = (c_cyc != 0) ? (r_cyc * 100ULL / c_cyc) : 0;
        kprintf("[RUST-PERF] fat: C=%llu cyc/walk RS=%llu cyc/walk ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
        bootlog_write("[RUST-PERF] fat: C=%llu RS=%llu ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}

// Check if directory
int fat_is_dir(fat_file_t *file) {
    return file->is_dir;
}

// Print filesystem info
void fat_print_info(fat_fs_t *fs) {
    if (!fs->mounted) {
        kprintf("[FAT] Not mounted\n");
        return;
    }

    kprintf("\n[FAT] Filesystem Information:\n");
    kprintf("  Type:           FAT%d\n", fs->fat_type);
    kprintf("  Volume Label:   %s\n", fs->volume_label);
    kprintf("  Bytes/Sector:   %u\n", fs->bytes_per_sector);
    kprintf("  Sectors/Cluster: %u\n", fs->sectors_per_cluster);
    kprintf("  Total Clusters: %u\n", fs->cluster_count);
    kprintf("  Total Size:     %u MB\n",
            (fs->cluster_count * fs->sectors_per_cluster * fs->bytes_per_sector) / (1024*1024));
}

// List directory
static void fat_list_dir_inner(fat_fs_t *fs, const char *path) {
    fat_file_t dir;
    if (fat_open(fs, path, &dir) != 0) {
        kprintf("[FAT] Cannot open: %s\n", path);
        return;
    }

    if (!dir.is_dir) {
        kprintf("[FAT] Not a directory: %s\n", path);
        fat_close(&dir);
        return;
    }

    kprintf("\nDirectory of %s\n", path);
    kprintf("%-12s %10s  %s\n", "Name", "Size", "Attr");
    kprintf("%-12s %10s  %s\n", "----", "----", "----");

    fat_dir_entry_t entry;
    char name[256];
    int count = 0;

    while (fat_readdir(&dir, &entry, name) == 0) {
        char attr_str[8] = "------";
        if (entry.attr & FAT_ATTR_DIRECTORY) attr_str[0] = 'D';
        if (entry.attr & FAT_ATTR_READ_ONLY) attr_str[1] = 'R';
        if (entry.attr & FAT_ATTR_HIDDEN) attr_str[2] = 'H';
        if (entry.attr & FAT_ATTR_SYSTEM) attr_str[3] = 'S';
        if (entry.attr & FAT_ATTR_ARCHIVE) attr_str[4] = 'A';

        if (entry.attr & FAT_ATTR_DIRECTORY) {
            kprintf("%-12s %10s  %s\n", name, "<DIR>", attr_str);
        } else {
            kprintf("%-12s %10u  %s\n", name, entry.file_size, attr_str);
        }
        count++;
    }

    kprintf("\n%d entries\n", count);
    fat_close(&dir);
}

// #192: WHOLE-FILE-READ TRACING, OFF BY DEFAULT. The four narration lines
// inside fat_read_file() below used to print unconditionally, so every config
// read, every app launch and every 2-second timezone refresh cost six serial
// lines. An always-on debugging aid at a chokepoint this hot is one of the ways
// a boot log becomes something nobody reads, and it is half of the #192 flood.
// `make FATTRACE=1` turns them back on. The FAILURE lines are NOT gated - they
// stay on always, and they now carry the path, because the only reason the
// "opening %s" line was load-bearing was that "open failed" alone did not say
// what had failed. The disabled form still type-checks its arguments, so a
// format string cannot rot while the trace is off.
#ifdef FAT_TRACE_READS
#define FAT_TRACE(...) kprintf(__VA_ARGS__)
#else
#define FAT_TRACE(...) do { if (0) { kprintf(__VA_ARGS__); } } while (0)
#endif

// Read entire file
void *fat_read_file(fat_fs_t *fs, const char *path, uint32_t *size_out) {
    extern fat_fs_t g_fat_fs;
    // task #317: network-filesystem routing. Paths under "/SMB/<server>/<share>"
    // are served by the SMB2 client (net/smb.c), mirroring the ext2 "/ext2"
    // redirect below. The share is mounted on demand on first access.
    if (path && (path[0]=='/') &&
        (path[1]=='S'||path[1]=='s') && (path[2]=='M'||path[2]=='m') &&
        (path[3]=='B'||path[3]=='b') && (path[4]=='/' || path[4]==0)) {
        extern void *smb_vfs_read_whole(const char *p, uint32_t *sz);
        return smb_vfs_read_whole(path, size_out);
    }
    // task #317 pass 3: NFSv3 routing. Paths under "/NFS/<server>/<label>" are
    // served by the NFS client (net/nfs.c). The mount must already exist (no
    // lazy auto-mount, since the export path is not encoded in the /NFS path).
    if (path && (path[0]=='/') &&
        (path[1]=='N'||path[1]=='n') && (path[2]=='F'||path[2]=='f') &&
        (path[3]=='S'||path[3]=='s') && (path[4]=='/' || path[4]==0)) {
        extern void *nfs_vfs_read_whole(const char *p, uint32_t *sz);
        return nfs_vfs_read_whole(path, size_out);
    }
    // #196: a removable drive (A:/E:) with a mounted disk image is handled by
    // the ONE redirect in fat_open() below, which this function falls through
    // to. It used to be hooked here INSTEAD, and that was the #725 defect in
    // miniature: whole-file reads saw the disc and handle-based opens did not.
    // #99 Phase C: with ext2 as the root fs, serve "/" reads from the ext2
    // volume first; fall back to FAT when a file is not on ext2. The FAT ESP /
    // boot paths (/boot, /EFI) are never redirected (UEFI loads from them).
    // #725: this used to be an INLINE SECOND COPY of the fat_path_on_ext2()
    // test, and it had already drifted: it matched any path merely STARTING
    // "/boot"/"/EFI" (no separator check), so "/bootcfg.txt" would have been
    // read from the FAT ESP while fat_write_file() wrote it to ext2. One
    // predicate now, shared with every other entry point.
    // #193: ...UNLESS a mounted disk image owns this subtree, in which case the
    // ext2 folder underneath it is not the answer and this must fall through to
    // fat_open(), which serves the disc. Without this test a whole-file read of
    // a path present in BOTH the image and the folder returned the FOLDER's
    // bytes, while the DOS guest's fat_open() on the identical path returned the
    // IMAGE's. Same class as the #725 defect this comment block describes, one
    // function over.
    if (!path_img_shadows(path) && fat_path_on_ext2(fs, path)) {
        uint32_t esz = 0;
        void *eb = ext2_read_whole(fat_ext2_vol_path(path), &esz);
        if (eb) { if (size_out) *size_out = esz; return eb; }
        // not present on ext2: fall through to the FAT read below
    }
    fat_file_t file;
    FAT_TRACE("[FAT] fat_read_file: opening %s\n", path);
    if (fat_open(fs, path, &file) != 0) {
        kprintf("[FAT] fat_read_file: open failed: %s\n", path);
        return NULL;
    }
    FAT_TRACE("[FAT] fat_read_file: opened, is_dir=%d\n", file.is_dir);

    if (file.is_dir) {
        fat_close(&file);
        return NULL;
    }

    uint32_t size = fat_size(&file);
    FAT_TRACE("[FAT] fat_read_file: size=%u bytes, allocating\n", size);
    void *buffer = kmalloc(size + 1);  // +1 for null terminator
    if (!buffer) {
        kprintf("[FAT] fat_read_file: kmalloc failed for %u bytes: %s\n", size, path);
        fat_close(&file);
        return NULL;
    }
    FAT_TRACE("[FAT] fat_read_file: allocated at %p, reading...\n", buffer);

    int bytes_read = fat_read(&file, buffer, size);
    FAT_TRACE("[FAT] fat_read_file: read %d bytes\n", bytes_read);
    fat_close(&file);

    if (bytes_read != (int)size) {
        kprintf("[FAT] fat_read_file: read mismatch (%d != %u): %s\n",
                bytes_read, size, path);
        kfree(buffer);
        return NULL;
    }

    ((uint8_t *)buffer)[size] = 0;  // Null terminate

    if (size_out) *size_out = size;
    return buffer;
}

// ============================================
// Write Operations
// ============================================

// Helper: Find parent directory and get filename
static int fat_split_path(const char *path, char *parent_out, char *name_out) {
    int len = strlen(path);
    if (len == 0) return -1;

    // Find last slash
    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') {
            last_slash = i;
            break;
        }
    }

    if (last_slash < 0) {
        // No slash - file in root
        strcpy(parent_out, "/");
        strcpy(name_out, path);
    } else if (last_slash == 0) {
        // File in root (e.g., "/file.txt")
        strcpy(parent_out, "/");
        strcpy(name_out, path + 1);
    } else {
        // File in subdirectory
        strncpy(parent_out, path, last_slash);
        parent_out[last_slash] = '\0';
        strcpy(name_out, path + last_slash + 1);
    }

    return 0;
}

// Helper: Find empty directory entry in a directory
static int fat_find_free_dir_entry(fat_fs_t *fs, fat_file_t *dir, uint32_t *sector_out, uint32_t *offset_out) {
    uint8_t *cluster_buf = kmalloc(fs->bytes_per_sector * fs->sectors_per_cluster);
    if (!cluster_buf) return -1;

    uint32_t cluster = dir->first_cluster;
    uint32_t prev_cluster = 0;

    // For root directory in FAT16
    if (cluster == 0 && fs->fat_type != FAT_TYPE_32) {
        for (uint32_t s = 0; s < fs->root_dir_sectors; s++) {
            if (fat_read_sector(fs, fs->root_start_lba + s, sector_buf) <= 0) {
                kfree(cluster_buf);
                return -1;
            }

            for (uint32_t i = 0; i < fs->bytes_per_sector; i += 32) {
                fat_dir_entry_t *entry = (fat_dir_entry_t *)(sector_buf + i);
                if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
                    *sector_out = fs->root_start_lba + s;
                    *offset_out = i;
                    kfree(cluster_buf);
                    return 0;
                }
            }
        }
        kfree(cluster_buf);
        return -1;  // Root directory full
    }

    // Traverse cluster chain for FAT32 or subdirectories
    while (cluster != 0) {
        if (fat_read_cluster(fs, cluster, cluster_buf) <= 0) {
            kfree(cluster_buf);
            return -1;
        }

        uint32_t cluster_bytes = fs->bytes_per_sector * fs->sectors_per_cluster;
        for (uint32_t i = 0; i < cluster_bytes; i += 32) {
            fat_dir_entry_t *entry = (fat_dir_entry_t *)(cluster_buf + i);
            if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
                *sector_out = cluster_to_lba(fs, cluster) + (i / fs->bytes_per_sector);
                *offset_out = i % fs->bytes_per_sector;
                kfree(cluster_buf);
                return 0;
            }
        }

        prev_cluster = cluster;
        cluster = fat_next_cluster(fs, cluster);
    }

    // No free entry - need to allocate new cluster for directory
    uint32_t new_cluster = fat_alloc_cluster(fs);
    if (new_cluster == 0) {
        kfree(cluster_buf);
        return -1;
    }

    // Link new cluster to chain
    if (prev_cluster != 0) {
        fat_set_fat_entry(fs, prev_cluster, new_cluster);
    }

    // Clear new cluster
    memset(cluster_buf, 0, fs->bytes_per_sector * fs->sectors_per_cluster);
    fat_write_cluster(fs, new_cluster, cluster_buf);

    *sector_out = cluster_to_lba(fs, new_cluster);
    *offset_out = 0;

    kfree(cluster_buf);
    return 0;
}

// Create a new file
static int fat_create_inner(fat_fs_t *fs, const char *path) {
    if (!fs || !fs->mounted || !path) return -1;

    char parent_path[256];
    char filename[256];

    if (fat_split_path(path, parent_path, filename) != 0) {
        return -1;
    }

    // Check if file already exists
    fat_file_t test_file;
    if (fat_open(fs, path, &test_file) == 0) {
        fat_close(&test_file);
        return -1;  // Already exists
    }

    // Open parent directory
    fat_file_t parent_dir;
    if (fat_open(fs, parent_path, &parent_dir) != 0) {
        return -1;  // Parent doesn't exist
    }

    if (!fat_is_dir(&parent_dir)) {
        fat_close(&parent_dir);
        return -1;  // Parent is not a directory
    }

    // Build the short directory entry (8.3 alias if the name needs an LFN), then
    // write it together with any LFN entries as a contiguous set.
    fat_dir_entry_t se; memset(&se, 0, sizeof(se));
    if (fat_name_needs_lfn(filename)) {
        if (fat_gen_short_alias(fs, parent_dir.first_cluster, filename, se.name) != 0) {
            fat_close(&parent_dir);
            return -1;
        }
    } else {
        str_to_fat_name(filename, se.name);
    }
    se.attr = FAT_ATTR_ARCHIVE;
    fat_stamp_new_entry(&se);   // #115: was created with all date/time words 0

    if (fat_write_entry_set(fs, &parent_dir, filename, &se) != 0) {
        fat_close(&parent_dir);
        return -1;
    }

    fat_close(&parent_dir);
    kprintf("[FS] Created file: %s\n", path);
    return 0;
}

// Create a new directory
static int fat_mkdir_inner(fat_fs_t *fs, const char *path) {
    if (!fs || !fs->mounted || !path) return -1;

    char parent_path[256];
    char dirname[256];

    if (fat_split_path(path, parent_path, dirname) != 0) {
        return -1;
    }

    // Check if directory already exists
    // task #578: -17 (EEXIST), not a generic -1, matching the ext2-redirect
    // fix above in fat_mkdir() so both backends give callers the same
    // already-exists signal instead of an indistinguishable hard failure.
    fat_file_t test_dir;
    if (fat_open(fs, path, &test_dir) == 0) {
        fat_close(&test_dir);
        return -17;  // Already exists (EEXIST)
    }

    // Open parent directory
    fat_file_t parent_dir;
    if (fat_open(fs, parent_path, &parent_dir) != 0) {
        return -1;
    }

    if (!fat_is_dir(&parent_dir)) {
        fat_close(&parent_dir);
        return -1;
    }

    // Allocate a cluster for the new directory
    uint32_t new_cluster = fat_alloc_cluster(fs);
    if (new_cluster == 0) {
        fat_close(&parent_dir);
        return -1;
    }

    // Build the directory's short entry (8.3 alias if the name needs an LFN) and
    // write it together with any LFN entries.
    fat_dir_entry_t se; memset(&se, 0, sizeof(se));
    if (fat_name_needs_lfn(dirname)) {
        if (fat_gen_short_alias(fs, parent_dir.first_cluster, dirname, se.name) != 0) {
            fat_set_fat_entry(fs, new_cluster, 0);
            fat_close(&parent_dir);
            return -1;
        }
    } else {
        str_to_fat_name(dirname, se.name);
    }
    se.attr = FAT_ATTR_DIRECTORY;
    se.cluster_hi = (new_cluster >> 16) & 0xFFFF;
    se.cluster_lo = new_cluster & 0xFFFF;
    if (fat_write_entry_set(fs, &parent_dir, dirname, &se) != 0) {
        fat_set_fat_entry(fs, new_cluster, 0);
        fat_close(&parent_dir);
        return -1;
    }

    // Initialize new directory with . and .. entries
    uint8_t *dir_buf = kmalloc(fs->bytes_per_sector * fs->sectors_per_cluster);
    if (!dir_buf) {
        fat_close(&parent_dir);
        return -1;
    }

    memset(dir_buf, 0, fs->bytes_per_sector * fs->sectors_per_cluster);

    // . entry
    fat_dir_entry_t *dot = (fat_dir_entry_t *)dir_buf;
    memset(dot->name, ' ', 11);
    dot->name[0] = '.';
    dot->attr = FAT_ATTR_DIRECTORY;
    dot->cluster_hi = (new_cluster >> 16) & 0xFFFF;
    dot->cluster_lo = new_cluster & 0xFFFF;

    // .. entry
    fat_dir_entry_t *dotdot = (fat_dir_entry_t *)(dir_buf + 32);
    memset(dotdot->name, ' ', 11);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = FAT_ATTR_DIRECTORY;
    dotdot->cluster_hi = (parent_dir.first_cluster >> 16) & 0xFFFF;
    dotdot->cluster_lo = parent_dir.first_cluster & 0xFFFF;

    // Write new directory cluster
    fat_write_cluster(fs, new_cluster, dir_buf);

    kfree(dir_buf);
    fat_close(&parent_dir);
    kprintf("[FS] Created directory: %s\n", path);
    return 0;
}

// Delete a file or empty directory
static int fat_delete_inner(fat_fs_t *fs, const char *path) {
    if (!fs || !fs->mounted || !path) return -1;

    char parent_path[256];
    char filename[256];

    if (fat_split_path(path, parent_path, filename) != 0) {
        return -1;
    }

    // Open the file to get its cluster chain
    fat_file_t file;
    if (fat_open(fs, path, &file) != 0) {
        return -1;  // File doesn't exist
    }

    // If it's a directory, check if it's empty
    if (fat_is_dir(&file)) {
        fat_dir_entry_t entry;
        // #404 plain-C fix: fat_readdir reconstructs VFAT long names of up to 255
        // chars, so this MUST be a full 256-byte buffer. The old char name[64]
        // let a directory containing a long-named entry overflow this stack
        // buffer via the strcpy in fat_readdir_inner (reachable on rmdir of a
        // crafted FAT dir). See blame.md / [RUST-SEC] fat.
        char name[256];
        int count = 0;

        while (fat_readdir(&file, &entry, name) == 0) {
            if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
                count++;
                break;
            }
        }

        if (count > 0) {
            fat_close(&file);
            return -1;  // Directory not empty
        }

        // Reset position for finding the entry
        file.position = 0;
    }

    // Free the cluster chain
    if (file.first_cluster >= 2) {
        fat_free_cluster_chain(fs, file.first_cluster);
    }

    fat_close(&file);

    // Find the entry (LFN-aware) and erase its whole set: the LFN entries plus
    // the short entry (logical indices first_idx..short_idx).
    fat_file_t parent_dir;
    if (fat_open(fs, parent_path, &parent_dir) != 0) {
        return -1;
    }

    if (fat_erase_name_entries(fs, &parent_dir, filename) != 0) {
        fat_close(&parent_dir);
        return -1;  // Entry not found
    }
    fat_close(&parent_dir);
    kprintf("[FS] Deleted: %s\n", path);
    return 0;
}

// #746: erase the WHOLE entry set for `name` in an open directory: the LFN
// entries plus the short 8.3 entry. Removes the NAME only; it never touches the
// cluster chain, which is what makes it usable by rename as well as delete.
//
// Lifted out of fat_delete_inner rather than copied, because the copy that
// already existed inside fat_rename_inner cleared only the SHORT entry's first
// byte and left the preceding LFN entries live - so a long-named file that was
// moved kept a ghost of its old name in the directory.
static int fat_erase_name_entries(fat_fs_t *fs, fat_file_t *dir, const char *name) {
    fat_dir_entry_t e; uint32_t sidx = 0, fidx = 0;
    if (fat_lookup(fs, dir->first_cluster, name, &e, &sidx, &fidx) != 0) return -1;
    for (uint32_t i = fidx; i <= sidx; i++) {
        uint8_t buf[32];
        if (dir_read_entry(fs, dir, i, buf) < 0) break;
        buf[0] = 0xE5;
        dir_write_entry(fs, dir, i, buf);
    }
    return 0;
}

// Rename a file or directory
static int fat_rename_inner(fat_fs_t *fs, const char *old_path, const char *new_path) {
    if (!fs || !fs->mounted || !old_path || !new_path) return -1;

    // #746: was char[64] while fat_split_path() strcpy()s an arbitrary-length
    // component into it, and every other buffer of this kind in this file is
    // char[256]. A path component over 63 bytes smashed this stack frame.
    char old_parent[256], old_name[256];
    char new_parent[256], new_name[256];

    fat_split_path(old_path, old_parent, old_name);
    fat_split_path(new_path, new_parent, new_name);

    // ------------------------------------------------------------------
    // #746 ATOMIC REPLACE, the FAT half.
    //
    // WHAT WAS WRONG. Neither branch below looked at the DESTINATION at all.
    // Renaming over an existing file therefore left TWO directory entries: the
    // old destination, still pointing at its own cluster chain, and the renamed
    // source. userland/libc/userconf.c documents the consequence: deleting
    // either name frees blocks the other still points at. So the standard
    // write-temp-then-rename safe-save was not safe here, which is the whole
    // reason the pattern is unavailable in this OS.
    //
    // WHAT THIS DOES INSTEAD, and why the order is this order. FAT has no
    // atomic-replace primitive, but a directory entry is 32 bytes inside one
    // sector, so REPOINTING the destination entry at the source's cluster chain
    // is a SINGLE SECTOR WRITE - the same shape as the ext2 dirent repoint, and
    // the same guarantee:
    //
    //   1. the destination entry starts naming the SOURCE's chain (one write);
    //   2. the source's entry set is erased, WITHOUT freeing the chain;
    //   3. only then is the destination's OLD chain freed.
    //
    // A crash at any point leaves the caller's new data reachable under one
    // name or the other. It never leaves a truncated destination, which is
    // exactly what copy-then-delete could not promise.
    // ------------------------------------------------------------------
    {
        fat_file_t dst;
        if (fat_open(fs, new_path, &dst) == 0) {
            if (dst.is_dir || dst.ext2_ino || dst.img_drive || dst.dirent_lba == 0) {
                fat_close(&dst);
                return -1;      // not a plain FAT file we can replace in place
            }
            uint32_t dest_lba = dst.dirent_lba, dest_off = dst.dirent_off;
            uint32_t dest_old_chain = dst.first_cluster;
            fat_close(&dst);

            fat_file_t src;
            if (fat_open(fs, old_path, &src) != 0) return -1;
            if (src.is_dir || src.ext2_ino || src.img_drive) { fat_close(&src); return -1; }
            uint32_t src_chain = src.first_cluster, src_size = src.file_size;
            fat_close(&src);

            if (src_chain == dest_old_chain) return 0;   // already the same file

            // STEP 1: one sector write, and the replacement has happened.
            if (fat_read_sector(fs, dest_lba, sector_buf) <= 0) return -1;
            if (dest_off + 32 > fs->bytes_per_sector) return -1;
            fat_dir_entry_t *de = (fat_dir_entry_t *)(sector_buf + dest_off);
            de->cluster_lo = (uint16_t)(src_chain & 0xFFFF);
            de->cluster_hi = (uint16_t)((src_chain >> 16) & 0xFFFF);
            de->file_size  = src_size;
            if (fat_write_sector(fs, dest_lba, sector_buf) != 0) return -1;

            // STEP 2: the source name goes away. The chain is NOT freed: the
            // destination entry owns it now.
            {
                fat_file_t odir;
                if (fat_open(fs, old_parent, &odir) == 0) {
                    if (fat_erase_name_entries(fs, &odir, old_name) != 0)
                        kprintf("[FAT] rename: source dirent %s NOT cleared; it now "
                                "shares a cluster chain with %s\n", old_path, new_path);
                    fat_close(&odir);
                }
            }
            // STEP 3: and only now the destination's old data.
            if (dest_old_chain >= 2) fat_free_cluster_chain(fs, dest_old_chain);
            kprintf("[FS] Replaced (dirent): %s -> %s (cl=%u sz=%u)\n",
                    old_path, new_path, src_chain, src_size);
            return 0;
        }
    }

    if (strcmp(old_parent, new_parent) != 0) {
        // Cross-directory move by relocating the directory entry (no data copy):
        // the file keeps its cluster chain; we add an entry in the new parent
        // pointing at the same clusters, then mark the old entry deleted.
        // Files only (directories would also need their ".." updated).
        fat_file_t src;
        if (fat_open(fs, old_path, &src) != 0) { kprintf("[FS] xdir: open old fail %s\n", old_path); return -1; }
        if (src.is_dir) { fat_close(&src); kprintf("[FS] xdir: src is dir\n"); return -1; }
        uint32_t first_cluster = src.first_cluster;
        uint32_t fsize = src.file_size;
        uint32_t old_lba = src.dirent_lba;
        uint32_t old_off = src.dirent_off;
        fat_close(&src);
        if (old_lba == 0) return -1;   // unknown source dir-entry location

        fat_file_t ndir;
        if (fat_open(fs, new_parent, &ndir) != 0) { kprintf("[FS] xdir: open newparent fail %s\n", new_parent); return -1; }
        uint32_t esec = 0, eoff = 0;
        if (fat_find_free_dir_entry(fs, &ndir, &esec, &eoff) != 0) { fat_close(&ndir); kprintf("[FS] xdir: no free slot in %s\n", new_parent); return -1; }
        fat_close(&ndir);

        // Write the relocated entry into the destination directory.
        if (fat_read_sector(fs, esec, sector_buf) <= 0) return -1;
        fat_dir_entry_t *ne = (fat_dir_entry_t *)(sector_buf + eoff);
        memset(ne, 0, 32);
        str_to_fat_name(new_name, ne->name);
        ne->attr = FAT_ATTR_ARCHIVE;
        ne->cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
        ne->cluster_hi = (uint16_t)((first_cluster >> 16) & 0xFFFF);
        ne->file_size = fsize;
        if (fat_write_sector(fs, esec, sector_buf) != 0) return -1;

        // Mark the old entry free (0xE5). Clusters are now owned by the new
        // entry, so we deliberately do NOT free them.
        if (fat_read_sector(fs, old_lba, sector_buf) > 0) {
            sector_buf[old_off] = 0xE5;
            // #695: was unchecked. The new entry already owns the cluster
            // chain, so if the old entry is not cleared the SAME chain is
            // reachable under two names, and deleting either one frees clusters
            // the other still points at. Fail the move rather than leave that.
            if (fat_write_sector(fs, old_lba, sector_buf) != 0) {
                kprintf("[FAT] move: old dirent NOT cleared (lba=%u); "
                        "%s and %s would share a cluster chain\n",
                        old_lba, old_path, new_path);
                return -1;
            }
        }
        kprintf("[FS] Moved (dirent): %s -> %s (cl=%u sz=%u)\n",
                old_path, new_path, first_cluster, fsize);
        return 0;
    }

    // Find the old entry and update its name
    fat_file_t parent_dir;
    if (fat_open(fs, old_parent, &parent_dir) != 0) {
        return -1;
    }

    uint8_t old_fat_name[11], new_fat_name[11];
    str_to_fat_name(old_name, old_fat_name);
    str_to_fat_name(new_name, new_fat_name);

    uint8_t *cluster_buf = kmalloc(fs->bytes_per_sector * fs->sectors_per_cluster);
    if (!cluster_buf) {
        fat_close(&parent_dir);
        return -1;
    }

    uint32_t cluster = parent_dir.first_cluster;

    // Handle FAT16 root directory
    if (cluster == 0 && fs->fat_type != FAT_TYPE_32) {
        for (uint32_t s = 0; s < fs->root_dir_sectors; s++) {
            if (fat_read_sector(fs, fs->root_start_lba + s, sector_buf) <= 0) {
                continue;
            }

            for (uint32_t i = 0; i < fs->bytes_per_sector; i += 32) {
                fat_dir_entry_t *entry = (fat_dir_entry_t *)(sector_buf + i);
                if (memcmp(entry->name, old_fat_name, 11) == 0) {
                    memcpy(entry->name, new_fat_name, 11);
                    // #695: was unchecked, so a failed rename reported success
                    // and the file kept its old name.
                    int wr = fat_write_sector(fs, fs->root_start_lba + s, sector_buf);
                    kfree(cluster_buf);
                    fat_close(&parent_dir);
                    if (wr != 0) {
                        kprintf("[FAT] rename: dirent write FAILED (lba=%u)\n",
                                fs->root_start_lba + s);
                        return -1;
                    }
                    kprintf("[FS] Renamed: %s -> %s\n", old_path, new_path);
                    return 0;
                }
            }
        }
    } else {
        while (cluster != 0) {
            if (fat_read_cluster(fs, cluster, cluster_buf) <= 0) {
                break;
            }

            uint32_t cluster_bytes = fs->bytes_per_sector * fs->sectors_per_cluster;
            for (uint32_t i = 0; i < cluster_bytes; i += 32) {
                fat_dir_entry_t *entry = (fat_dir_entry_t *)(cluster_buf + i);
                if (memcmp(entry->name, old_fat_name, 11) == 0) {
                    memcpy(entry->name, new_fat_name, 11);
                    fat_write_cluster(fs, cluster, cluster_buf);
                    kfree(cluster_buf);
                    fat_close(&parent_dir);
                    kprintf("[FS] Renamed: %s -> %s\n", old_path, new_path);
                    return 0;
                }
            }

            cluster = fat_next_cluster(fs, cluster);
        }
    }

    kfree(cluster_buf);
    fat_close(&parent_dir);
    return -1;
}

// #554: FAT native permission/attribute support. FAT has no uid/gid/mode
// concept - unlike ext2/POSIX paths (which route through the path-keyed
// perms.c overlay), these operate on the REAL on-disk FAT_ATTR_* byte in the
// directory entry, so a Properties dialog or `ls` reading it back sees a true
// filesystem attribute, not a fabricated value. Callers (proc/syscall.c's
// sys_chmod/sys_chown routing, via rustkern/fsperm.rs) only reach these for
// genuine FAT (ESP: /boot, /EFI) paths; ext2/root paths never call them.
//
// fat_open() already resolves and stores dirent_lba/dirent_off for the file's
// OWN short directory entry (see fat_rename_inner's cross-directory move,
// which rewrites the identical two fields), so no new directory-walk logic is
// needed here - just a read-modify-write of the one attribute byte.
static int fat_set_readonly_inner(fat_fs_t *fs, const char *path, int readonly) {
    fat_file_t f;
    if (fat_open_inner(fs, path, &f) != 0) return -1;
    uint32_t lba = f.dirent_lba, off = f.dirent_off;
    fat_close(&f);
    if (lba == 0) return -1;  // FAT12/16 root-dir entry or unknown location
    if (fat_read_sector(fs, lba, sector_buf) <= 0) return -1;
    fat_dir_entry_t *e = (fat_dir_entry_t *)(sector_buf + off);
    if (readonly) e->attr |= FAT_ATTR_READ_ONLY;
    else          e->attr &= (uint8_t)~FAT_ATTR_READ_ONLY;
    if (fat_write_sector(fs, lba, sector_buf) != 0) return -1;
    return 0;
}
int fat_set_readonly(const char *path, int readonly) {
    // #725: the comment above says ext2 paths "never call" these. There are
    // currently ZERO callers of either function, so that sentence has never
    // been tested; make it true by construction instead. An ext2 inode has no
    // FAT attribute byte, and fat_set_readonly_inner()'s read-modify-write of a
    // directory entry has no meaning on ext2, so refuse rather than silently
    // touch a stale ESP copy of the same path.
    {
        extern fat_fs_t g_fat_fs;
        if (fat_path_on_ext2(&g_fat_fs, path)) return -1;
    }
    extern fat_fs_t g_fat_fs;
    if (!g_fat_fs.mounted) return -1;
    if (!fat_lock()) return -1;
    int r = fat_set_readonly_inner(&g_fat_fs, path, readonly); fat_unlock(); return r;
}

static int fat_get_attr_info_inner(fat_fs_t *fs, const char *path,
                                   uint8_t *attr_out, int *is_dir_out) {
    fat_file_t f;
    if (fat_open_inner(fs, path, &f) != 0) return -1;
    if (attr_out)  *attr_out  = f.attr;
    if (is_dir_out) *is_dir_out = (f.is_dir || (f.attr & FAT_ATTR_DIRECTORY)) ? 1 : 0;
    fat_close(&f);
    return 0;
}
int fat_get_attr_info(const char *path, uint8_t *attr_out, int *is_dir_out) {
    extern fat_fs_t g_fat_fs;
    if (!g_fat_fs.mounted) return -1;
    // #725: fat_get_attr_info_inner() calls fat_open_inner(), which is the
    // UNROUTED open and so only ever sees the FAT ESP. Answer ext2-root paths
    // through the routed fat_open() instead, so this reports the real file
    // rather than "not found" (it has no callers today; a future one must not
    // rediscover the #725 hole).
    if (fat_path_on_ext2(&g_fat_fs, path)) {
        fat_file_t f;
        if (fat_open(&g_fat_fs, path, &f) != 0) return -1;
        if (attr_out)   *attr_out   = f.attr;
        if (is_dir_out) *is_dir_out = f.is_dir ? 1 : 0;
        fat_close(&f);
        return 0;
    }
    if (!fat_lock()) return -1;
    int r = fat_get_attr_info_inner(&g_fat_fs, path, attr_out, is_dir_out); fat_unlock(); return r;
}

// Write data to a file at current position
// #746: write first_cluster + file_size back into this handle's directory
// entry. Factored out of fat_write_inner() so fat_truncate() below performs the
// SAME operation rather than a second copy of it. Returns 0 when the entry is
// consistent with the handle (including the "we do not know where the entry is"
// case, which is not an error the caller can act on), -1 when the sector write
// failed - in which case the data on disk is unreachable and the clusters are
// leaked, so it must NOT be reported as success.
static int fat_persist_dirent(fat_file_t *file) {
    if (!file || !file->fs) return -1;
    fat_fs_t *fs = file->fs;
    if (!file->dirent_lba || fs->bytes_per_sector > 512) return 0;
    uint8_t dbuf[512];
    if (fat_read_sector(fs, file->dirent_lba, dbuf) <= 0) return 0;
    if (file->dirent_off + 32 > fs->bytes_per_sector) return 0;
    fat_dir_entry_t *de = (fat_dir_entry_t *)(dbuf + file->dirent_off);
    de->cluster_lo = (uint16_t)(file->first_cluster & 0xFFFF);
    de->cluster_hi = (uint16_t)(file->first_cluster >> 16);
    de->file_size  = file->file_size;
    fat_stamp_modified(de);   // #115: the contents just changed; so does mtime
    file->mtime_date = de->modify_date;   // keep the handle consistent with disk
    file->mtime_time = de->modify_time;
    // #695: was unchecked. Without this the data is on disk but the directory
    // entry still reports the OLD size / cluster, so the bytes are unreachable
    // and the clusters are leaked, while fat_write() returned a healthy count.
    if (fat_write_sector(fs, file->dirent_lba, dbuf) != 0) {
        kprintf("[FAT] dirent update FAILED (lba=%u)\n", file->dirent_lba);
        return -1;
    }
    return 0;
}

// #115: the FAT half of utime(2). Pass -1 to leave a time unchanged.
//
// FAT has no ctime-equivalent to update (create_date means CREATION, and
// rewriting it on a utime() would be a lie), and its access field is a DATE
// with no time-of-day, so an atime set through here lands at midnight. Both are
// properties of the on-disk format and are documented rather than papered over.
//
// Refuses on an ext2-backed or image-backed handle: those are not FAT entries
// and silently succeeding on them is how a caller ends up believing a timestamp
// was set when nothing was written (#725 in miniature).
int fat_set_times(fat_fs_t *fs, const char *path, int64_t atime, int64_t mtime) {
    if (!fs || !fs->mounted || !path) return -1;
    fat_file_t f;
    if (fat_open(fs, path, &f) != 0) return -1;
    if (f.ext2_ino || f.img_drive || !f.dirent_lba || fs->bytes_per_sector > 512) {
        fat_close(&f);
        return -1;
    }
    uint32_t lba = f.dirent_lba, off = f.dirent_off;
    fat_close(&f);
    if (off + 32 > fs->bytes_per_sector) return -1;

    uint8_t dbuf[512];
    if (fat_read_sector(fs, lba, dbuf) <= 0) return -1;
    fat_dir_entry_t *de = (fat_dir_entry_t *)(dbuf + off);
    if (mtime >= 0) {
        uint16_t d = 0, t = 0;
        if (ktime_unix_to_dos_rs(mtime, &d, &t) != 0) return -1;   // out of FAT range
        de->modify_date = d;
        de->modify_time = t;
    }
    if (atime >= 0) {
        uint16_t d = 0, t = 0;
        if (ktime_unix_to_dos_rs(atime, &d, &t) != 0) return -1;
        de->access_date = d;   // FAT stores no access time-of-day
    }
    return (fat_write_sector(fs, lba, dbuf) == 0) ? 0 : -1;
}

static int fat_write_inner(fat_file_t *file, const void *buffer, uint32_t size) {
    if (!file || !file->fs || !buffer || size == 0) return -1;

    fat_fs_t *fs = file->fs;
    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t bytes_written = 0;
    uint32_t cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;

    uint8_t *cluster_buf = kmalloc(cluster_size);
    if (!cluster_buf) return -1;

    // If file has no clusters yet, allocate the first one
    if (file->first_cluster < 2) {
        uint32_t new_cluster = fat_alloc_cluster(fs);
        if (new_cluster == 0) {
            kfree(cluster_buf);
            return -1;
        }
        file->first_cluster = new_cluster;
        file->current_cluster = new_cluster;

        // Update directory entry with new cluster
        // TODO: We need to store the directory entry location in fat_file_t
    }

    // Navigate to the cluster holding `position`, EXTENDING the chain (allocate
    // + link) whenever navigation runs past the current end. Previously, when
    // the chain was shorter than cluster_index, navigation left `cluster` at the
    // last existing cluster (or 0), so each append-call rewrote that one cluster
    // instead of growing the file -- multi-cluster writes (e.g. cp of a 3MB BMP,
    // which the cp app issues as many 4KB append calls) collapsed to a single
    // cluster and the rest were leaked. Now the chain is grown to reach the
    // target cluster.
    uint32_t cluster_index = file->position / cluster_size;
    uint32_t offset_in_cluster = file->position % cluster_size;

    uint32_t cluster = file->first_cluster;
    for (uint32_t i = 0; i < cluster_index; i++) {
        uint32_t next = fat_next_cluster(fs, cluster);
        if (next == 0) {
            uint32_t nc = fat_alloc_cluster(fs);
            if (nc == 0) { kfree(cluster_buf); return bytes_written; }
            fat_set_fat_entry(fs, cluster, nc);
            next = nc;
        }
        cluster = next;
    }

    // Write data
    while (bytes_written < size) {
        if (cluster == 0) {
            // Need new cluster
            uint32_t new_cluster = fat_alloc_cluster(fs);
            if (new_cluster == 0) break;

            if (file->first_cluster < 2) {
                file->first_cluster = new_cluster;
            } else {
                // Find last cluster and link
                uint32_t last = file->first_cluster;
                while (fat_next_cluster(fs, last) != 0) {
                    last = fat_next_cluster(fs, last);
                }
                fat_set_fat_entry(fs, last, new_cluster);
            }
            cluster = new_cluster;
            offset_in_cluster = 0;

            // Clear new cluster
            memset(cluster_buf, 0, cluster_size);
        } else {
            // Read existing cluster
            if (fat_read_cluster(fs, cluster, cluster_buf) <= 0) {
                break;
            }
        }

        // Calculate how much to write to this cluster
        uint32_t space_in_cluster = cluster_size - offset_in_cluster;
        uint32_t to_write = size - bytes_written;
        if (to_write > space_in_cluster) to_write = space_in_cluster;

        // Copy data
        // #19/#645: `src` is the CALLER's buffer; Ring-3 on the sys_write path.
        {   uaccess_ac_t __ac = uaccess_begin();
            memcpy(cluster_buf + offset_in_cluster, src + bytes_written, to_write);
            uaccess_end(__ac); }

        // Write cluster back
        if (fat_write_cluster(fs, cluster, cluster_buf) <= 0) {
            break;
        }

        bytes_written += to_write;
        file->position += to_write;
        offset_in_cluster = 0;

        // Move to next cluster if needed
        if (bytes_written < size) {
            uint32_t next = fat_next_cluster(fs, cluster);
            cluster = next;
        }
    }

    // Update file size if we extended it
    if (file->position > file->file_size) {
        file->file_size = file->position;
    }

    // Persist first_cluster + file_size back to the directory entry. Without
    // this, written data is invisible (the entry still reports size 0 / cluster 0)
    // and the allocated cluster is leaked. (Was an unfinished TODO.)
    // #746: the write-back is now fat_persist_dirent(), because fat_truncate()
    // needs the identical operation and a second hand-written copy of it is
    // exactly how the two halves of a pair drift apart in this tree.
    if (fat_persist_dirent(file) != 0) {
        kfree(cluster_buf);
        return -1;
    }

    kfree(cluster_buf);
    return bytes_written;
}

// Copy a file from src_path to dst_path
static int fat_copy_inner(fat_fs_t *fs, const char *src_path, const char *dst_path) {
    if (!fs || !src_path || !dst_path) return -1;

    // Open source file
    fat_file_t src_file;
    if (fat_open(fs, src_path, &src_file) != 0) {
        return -1;
    }

    if (fat_is_dir(&src_file)) {
        fat_close(&src_file);
        return -1;  // Can't copy directories this way
    }

    // Create destination file
    if (fat_create(fs, dst_path) != 0) {
        fat_close(&src_file);
        return -1;
    }

    // Open destination file
    fat_file_t dst_file;
    if (fat_open(fs, dst_path, &dst_file) != 0) {
        fat_close(&src_file);
        return -1;
    }

    // Copy data in chunks
    uint8_t *buf = kmalloc(4096);
    if (!buf) {
        fat_close(&src_file);
        fat_close(&dst_file);
        return -1;
    }

    int total = 0;
    int bytes;
    while ((bytes = fat_read(&src_file, buf, 4096)) > 0) {
        int written = fat_write(&dst_file, buf, bytes);
        if (written != bytes) break;
        total += written;
    }

    kfree(buf);
    fat_close(&src_file);
    fat_close(&dst_file);

    kprintf("[FS] Copied %s to %s (%d bytes)\n", src_path, dst_path, total);
    return (total > 0) ? 0 : -1;
}

// Move a file from src_path to dst_path
static int fat_move_inner(fat_fs_t *fs, const char *src_path, const char *dst_path) {
    if (!fs || !src_path || !dst_path) return -1;

    // Simple implementation: copy then delete
    if (fat_copy(fs, src_path, dst_path) != 0) {
        return -1;
    }

    return fat_delete(fs, src_path);
}

// Write entire buffer to a file (creates if doesn't exist, truncates if does)
static int fat_write_file_inner(fat_fs_t *fs, const char *path, const void *data, uint32_t size) {
    if (!fs || !fs->mounted || !path || !data) return -1;

    // Delete existing file if present
    fat_file_t existing;
    if (fat_open(fs, path, &existing) == 0) {
        fat_close(&existing);
        if (fat_delete(fs, path) != 0) {
            return -1;
        }
    }

    // Create new file
    if (fat_create(fs, path) != 0) {
        return -1;
    }

    // Open for writing
    fat_file_t file;
    if (fat_open(fs, path, &file) != 0) {
        return -1;
    }

    // Write data
    int written = fat_write(&file, data, size);

    // Update the short directory entry with the real size + first cluster.
    // Uses the LFN-aware lookup so it works for both 8.3 and long (aliased) names.
    int dirent_ok = 1;          // #695: did the size/cluster update land?
    char parent_path[256], filename[256];
    fat_split_path(path, parent_path, filename);

    fat_file_t parent_dir;
    if (fat_open(fs, parent_path, &parent_dir) == 0) {
        fat_dir_entry_t e; uint32_t sidx = 0, fidx = 0;
        if (fat_lookup(fs, parent_dir.first_cluster, filename, &e, &sidx, &fidx) == 0) {
            uint32_t sec, off;
            if (dir_locate(fs, &parent_dir, sidx, &sec, &off, 0) == 0 &&
                fat_read_sector(fs, sec, sector_buf) > 0) {
                fat_dir_entry_t *de = (fat_dir_entry_t *)(sector_buf + off);
                de->file_size = size;
                de->cluster_hi = (file.first_cluster >> 16) & 0xFFFF;
                de->cluster_lo = file.first_cluster & 0xFFFF;
                fat_stamp_modified(de);   // #115
                // #695: was unchecked, and this function returns
                // (written == size) ? 0 : -1, so a failed dirent update
                // reported SUCCESS while the file reads back as size 0 with its
                // clusters leaked. That is the FAT twin of the ext2 bug.
                if (fat_write_sector(fs, sec, sector_buf) != 0) {
                    kprintf("[FAT] write_file: dirent update FAILED for %s\n", path);
                    dirent_ok = 0;
                }
            }
        }
        fat_close(&parent_dir);
    }

    fat_close(&file);
    kprintf("[FS] Wrote file: %s (%d bytes)\n", path, written);
    return (dirent_ok && written == (int)size) ? 0 : -1;
}

// Check if a path exists
static int fat_exists_inner(fat_fs_t *fs, const char *path) {
    if (!fs || !fs->mounted || !path) {
        return -1;
    }

    fat_file_t file;
    if (fat_open(fs, path, &file) == 0) {
        fat_close(&file);
        return 1;  // Exists
    }
    return 0;  // Does not exist
}

// Count free clusters in the FAT
static uint32_t fat_get_free_clusters_inner(fat_fs_t *fs) {
    if (!fs || !fs->mounted) return 0;
    return fs->free_cluster_count;
}
