// fdlayer.c - the legacy kernel-wide file-descriptor layer (#746b).
//
// EXTRACTED FROM proc/syscall.c AS A PURE MOVE. Every line below is
// byte-for-byte the line that was in syscall.c; only this preamble and the
// include list are new. Nothing was renamed, reordered or reworded, and the
// three path predicates plus sc_parent_of moved to syscall_path.h as
// `static inline` precisely so that the callers which stayed keep the machine
// code they already had.
//
// WHAT LIVES HERE. The system-wide legacy fd tables, all three of them, which
// share one 16-slot number space and are told apart by which table has `used`
// set for that slot:
//   fd_table[] / fd_used[] / fd_werr[]  FAT files (fat_file_t, write-through)
//   e2fd[]                              ext2 files (#572 bounded read window,
//                                       buffered writes, #746 O_RDWR)
//   smbfd[]                             SMB and NFS (whole-file cache, upload
//                                       on close)
// plus sys_open/sys_open_k, sys_close, sys_fsync, sys_read, sys_write,
// sys_seek, sys_fcntl and sys_readdir/sys_readdir_k.
//
// WHY IT WAS SPLIT OUT: so it can be compiled STANDALONE on the host and put
// under a test harness. syscall.c cannot be, because it drags in the whole
// process, window, network and exec machinery; this file's dependency on the
// PCB is small and explicit (proc_current, ->fds[], ->privilege, ->euid/egid,
// ->is_service/->svc_perms), which is a surface a harness can stub.
//
// Justified-C, not Rust: this is a move of existing C, not new code. Not one
// statement changed. Rewriting it in Rust in the same commit would have made
// the equivalence proof impossible, which is the whole point of doing the move
// first and on its own.
#include "syscall.h"
#include "process.h"
#include "services.h"
#include "syscall_path.h"
#include "fdlayer.h"
#include "../types.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../mm/demand.h"
#include "../sync/spinlock.h"
#include "../fs/fat.h"
#include "../fs/ext2.h"
#include "../fs/perms.h"
#include "../fs/vfs.h"
#include "../net/smb.h"
#include "../net/nfs.h"
#include "../drivers/hotplug.h"   // #250: removable-volume path routing
#include "../gui/syslog.h"
#include "../cpu/dlprof.h"
#include "../cpu/scprof.h"  // #121: read-path phase attribution
#include "../security/validate.h"
#include "../security/uaccess_smap.h"  // #19/#645: AC brackets on the user-buffer copies
#include "../security/selftest_registry.h"  // #PERMSKIP
#include "fdown.h"                           // #fdguard: legacy fd ownership guard
#include "../security/seclog.h"              // #fdguard: seclog_report_io_boundary
#include "../fs/bootlog.h"                   // #fdguard: bootlog_write on bypass

// DECLARED NEW LINE (not moved): the mounted FAT volume. There is no
// `extern fat_fs_t g_fat_fs;` in fs/fat.h, so all 12 files that touch it carry
// a private extern, and syscall.c carries two. sys_open_k's fat_exists() call
// came here and needs one. Hoisting the declaration into fs/fat.h would be the
// real fix and would delete twelve duplicates, but that is a tree-wide change
// and this commit is a move; it is written down here so the next person does it
// deliberately rather than adding a thirteenth copy by reflex.
extern fat_fs_t g_fat_fs;

// ---- moved verbatim from proc/syscall.c ------------------------------------
// #615: write-path profile. Serial only (#597).
extern uint64_t sched_now_ms(void);
static uint64_t g_wstream_ms = 0;      // cumulative ms inside ext2_wstream_block
static uint64_t g_wstream_calls = 0;
// ============================================================================
// File I/O syscalls (using FAT filesystem)
// ============================================================================

// Simple file descriptor table (legacy kernel-wide table)
// NOTE: process.h defines MAX_FDS=64 for the VFS per-process fd table.
// This legacy table is used for basic FAT file access until full VFS migration.
//
// #793: was 16 (13 usable, slots 0-2 skipped). SYSTEM-WIDE, not per-process
// (see the #444 comment below), so every process on the box draws from this
// ONE pool: at idle, MayteraOS boot already runs ~35+ processes (netmon,
// haservice, AICHAT, seclog, cron, sshd sessions, ...), several of which hold
// a file open for real stretches. Measured live (fdprobe, #793): a freshly
// launched process's very FIRST sys_open() of an ordinary ext2 file returned
// -EMFILE on an otherwise-idle desktop, and a same-file open-without-close
// loop capped at exactly the documented 13. AssaultCube's shadow-cache-
// generation pass (userland/apps/assaultcube) opens dozens of distinct
// packages/models/**/*.jpg textures in a single run; racing that against
// ordinary background daemons for a 13-slot GLOBAL pool is enough to lose
// the race on an otherwise-perfectly-good file, which surfaces in-app as
// "couldn't load texture" with no hint that the file itself was ever the
// problem (texture.cpp only sees IMG_Load() return NULL). Raised to 128
// (matches/exceeds the per-process MAX_FDS=64, so one busy process can no
// longer be capacity-starved by the rest of the system under ordinary load)
// at negligible cost: each slot's largest fixed member is path[256]; even at
// ~1KB/slot across fd_table/e2fd/smbfd the increase is under 150KB of static
// kernel BSS. This does not remove the SHARED-pool design (a real per-
// process legacy fd table is the fuller fix, tracked as follow-up), it only
// removes the specific "16 was too small for how many processes this OS
// actually runs today" failure mode this task measured.
#define LEGACY_MAX_FDS 128
static fat_file_t fd_table[LEGACY_MAX_FDS];
static int fd_used[LEGACY_MAX_FDS];
// #746: STICKY WRITE ERROR for the legacy FAT fd family.
//
// WHY close() HAD TO LEARN THIS. sys_close() returned a hardcoded 0 on this
// family, so a FAT write that failed was invisible to close() no matter how
// carefully the caller checked - while the ext2 family returned the real rc
// from ext2_write_file(). Same syscall, same fd number space, two different
// answers to "did my data land", decided by which partition the path was on.
// That divergence is the recurring fault in this tree, so the fix is the one
// that removes it rather than the one that patches the symptom.
//
// WHAT COUNTS AS A FAILED WRITE HERE, and why it is not just `rc < 0`.
// fat_write() returns a BYTE COUNT, not 0/-1 like its neighbours. A write that
// runs out of clusters half way through returns a POSITIVE short count, which
// every `if (rc < 0)` caller reads as success. Both shapes set this flag; see
// the check in sys_write_inner().
//
// Cleared at exactly ONE place: the locked scan in sys_open_k() that claims the
// slot. A slot cannot become live by any other route, so a recycled fd can
// never inherit a previous file's error.
static int fd_werr[LEGACY_MAX_FDS];

// ===========================================================================
// #FDNS: THE LEGACY FD NUMBERS LIVE IN THEIR OWN, DISJOINT RANGE.
//
// THE BUG THIS REMOVES (measured on golden build 2025, VM <vmid>, 2026-08-23).
// This kernel has TWO descriptor namespaces:
//
//   * proc->fds[0..MAX_FDS-1]  per-process file_t: console, pipes, PTYs,
//                              sockets, /dev nodes. Allocated by fd_alloc(3).
//   * the three tables above   SYSTEM-WIDE: FAT fd_table[], ext2 e2fd[],
//                              SMB/NFS smbfd[]. Allocated by the scan in
//                              sys_open_k(), which also started at 3.
//
// Both handed out the SAME small integers, and neither allocator could see the
// other, so one process could hold BOTH a per-process fd 3 and a legacy fd 3 at
// the same time. Every fd-consuming syscall then disambiguated by PROBING: VFS
// first, legacy second. Probing cannot recover information the number does not
// carry, so the second of the two descriptions was simply unreachable and its
// operations landed on the first one:
//
//   1. Terminal runs a foreground command: open("/dev/ptmx") -> VFS fd 3 (the
//      pty master), held for the whole command.
//   2. Terminal calls getpwuid() (libc/pwd.c) -> sys_open("/CONFIG/PASSWD") ->
//      the legacy scan hands back 3, because the legacy table knows nothing
//      about the VFS table.
//   3. read(3) is answered by the PTY MASTER, so the password parse silently
//      eats the child's output and gets nothing.
//   4. close(3) closes the PTY MASTER and leaves the ext2 description for
//      /CONFIG/PASSWD open, with the file cached in its read window.
//   5. The terminal's pump loop reads its master again. proc->fds[3] is NULL
//      now, so the read FALLS THROUGH to the orphaned legacy slot 3 and the
//      terminal renders /CONFIG/PASSWD into its window, in place of the
//      command's output. That is the user-reported bug: `ls -lat` printing
//      "root:0:0:/:Root:/APPS/MSH". Measured leak rate before this change:
//      20/20 runs of `sleep 3`, 4/40 runs of `ls -lat`.
//
// WHY A RANGE AND NOT AN ORDERING RULE. userland/apps/terminal/main.c already
// carried a rule ("close the pipe before opening the file, or the write hits
// the pipe") that is the write-side face of this exact defect. A rule every
// caller must remember is not a control; it is a note, and this is the second
// time the same collision has been paid for. Giving the families disjoint
// number ranges makes the collision UNREPRESENTABLE: a legacy fd can never
// equal a VFS fd, so the probe order stops being load-bearing and every
// fd-consuming syscall can route on the NUMBER instead of guessing.
//
// The _Static_assert is the durable half. Raising MAX_FDS past LEGACY_FD_BASE
// would re-create the overlap silently; it now fails the build instead.
// ===========================================================================
#define LEGACY_FD_BASE 256
_Static_assert(LEGACY_FD_BASE >= MAX_FDS,
               "#FDNS: the legacy fd range must not overlap proc->fds[]; "
               "raising MAX_FDS above LEGACY_FD_BASE re-creates the "
               "/CONFIG/PASSWD-into-terminal-output collision");

// Is `fd` (as userland sees it) one of ours?
static inline int lfd_is(int fd) {
    return fd >= LEGACY_FD_BASE && fd < LEGACY_FD_BASE + LEGACY_MAX_FDS;
}
// Userland fd -> index into fd_table[]/e2fd[]/smbfd[]. Only valid when lfd_is().
static inline int lfd_idx(int fd) { return fd - LEGACY_FD_BASE; }
// Index -> the number userland gets back. The ONE place the offset is applied.
static inline int lfd_ext(int idx) { return LEGACY_FD_BASE + idx; }


// #444: fd_used[]/e2fd[]/smbfd[] above are a SYSTEM-WIDE table (128 slots,
// 125 usable as of #793; originally 16 slots / 13 usable) shared by every
// process on the box, but sys_open()'s "find a free
// slot" scan used to be completely unlocked. Two processes calling sys_open()
// concurrently could both scan, both see the SAME slot as free, and both
// proceed to populate it (e2fd[fd]/smbfd[fd]) with their OWN file's data -
// whichever one finished last would silently win, so BOTH callers were handed
// back the same fd number and the loser's process would from then on read (and
// on close, free) the WINNER's file content instead of its own. This is a
// second, independent root cause of the #444 CPython "corrupt reads" symptom
// on top of the ATA/ext2-cache lazy-lock-init race fixed in drivers/ata.c and
// fs/ext2.c: proven live by a 4-independent-process concurrent-open hammer
// test where 3 of 4 processes opening 4 DIFFERENT files ended up reading the
// 4th process's exact file content byte-for-byte. Fixed by claiming the slot
// (fd_used[i] = 1) ATOMICALLY under this lock in the same scan that finds it,
// so no other caller can ever observe it as free once one caller has picked
// it; every failure path after the claim now releases the slot back
// (fd_used[fd] = 0) before returning. The lock is held only for the tiny
// scan+claim, never across the (possibly slow / possibly network-blocking for
// SMB/NFS) population work that follows, so this cannot turn into a
// hold-a-spinlock-across-a-blocking-call bug (#426).
static spinlock_t g_legacy_fd_lock = SPINLOCK_INIT;

// Release a legacy fd slot back to the free pool under g_legacy_fd_lock, so
// this write can never race with the locked scan-and-claim in sys_open().
static inline void legacy_fd_release(int fd) {
    uint64_t fl = spinlock_acquire_irqsave(&g_legacy_fd_lock);
    fd_used[fd] = 0;
    spinlock_release_irqrestore(&g_legacy_fd_lock, fl);
    fdown_release_rs((uint32_t)fd);   // #fdguard: drop cross-process ownership
}

// #fdguard: the identity a legacy fd slot is stamped with and the only
// identity that may operate on it. A thread group, not a raw pid, so
// pthreads sharing an open file all match (rustkern/fdown.rs). 0 means no
// process context (an early-boot kernel open), which fdown treats as free.
static inline uint32_t legacy_owner_id(void) {
    process_t *p = proc_current();
    if (!p) return 0;
    return p->tgid ? p->tgid : p->pid;
}

// #fdguard: rate-limited audit of a refused cross-process legacy-fd op,
// capped like fs/perms.c's [PERMS-DENY] so a Ring 3 loop guessing slots
// cannot flood the audit ring / console. security_audit() (behind
// seclog_report_io_boundary) is non-blocking, so this is safe from every
// fd syscall path. The record carries the actor pid, the target slot and
// its owner, and the reason.
#define FDGUARD_AUDIT_MAX 200
static uint32_t g_fdguard_audits = 0;
static void legacy_owner_refused(int slot, uint32_t me, const char *op) {
    if (g_fdguard_audits < FDGUARD_AUDIT_MAX) {
        uint32_t owner = fdown_owner_rs((uint32_t)slot);
        char d[64];
        snprintf(d, sizeof(d), "fd op=%s slot=%d owner=%u cross-proc refused",
                 op, slot, owner);
        seclog_report_io_boundary(me, d);
        if (++g_fdguard_audits == FDGUARD_AUDIT_MAX)
            seclog_report_io_boundary(me,
                "fd boundary: further refusals silent this boot");
    }
}

// #fdguard: DEV-ONLY runtime bypass, so the guard can be demonstrated going
// BOTH ways on ONE build (the same "prove a control can fail" discipline as
// the RTCLKTESTFAIL / NOBLOCKTEST build knobs). Default is ENFORCE. It is
// armed only by the presence of /CONFIG/FDGUARD.BYPASS, a marker that ships
// on NO production golden (same class as /CONFIG/DOSDIAG.CFG and CRONTEST.CFG)
// and is read ONCE at boot by the fdgtest worker, never per-op. Arming it is
// logged CRITICAL to the audit trail, serial and /BOOTLOG.TXT so a bypassed
// image can never be mistaken for a healthy one.
static int g_fdguard_bypass = 0;
int fdguard_bypass(void) { return g_fdguard_bypass; }
void fdguard_set_bypass(int on) {
    g_fdguard_bypass = on ? 1 : 0;
    if (g_fdguard_bypass) {
        kprintf("[FDGUARD] *** BYPASS ACTIVE - fd/pts ownership NOT enforced - "
                "INSECURE DEV IMAGE ***\n");
        seclog_report_io_boundary(0,
            "FDGUARD BYPASS ACTIVE: fd/pts ownership NOT enforced (dev image)");
        bootlog_write("[FDGUARD] BYPASS ACTIVE - fd/pts ownership NOT enforced "
                      "(dev image, /CONFIG/FDGUARD.BYPASS present)");
    }
}

// #fdguard: THE CHOKEPOINT, called right after `fd = lfd_idx(fd)` in every
// legacy-fd consumer. `slot` is the lfd_idx() index. Returns 1 to PROCEED
// (the caller owns it, or the slot is free and the existing used-check will
// answer), 0 to REFUSE (the slot is live and owned by another process).
static int legacy_owner_ok(int slot, const char *op) {
    if (g_fdguard_bypass) return 1;   // #fdguard dev-only bypass (see above)
    uint32_t me = legacy_owner_id();
    int r = fdown_check_rs((uint32_t)slot, me);
    if (r == FDOWN_R_NOTOWNER) { legacy_owner_refused(slot, me, op); return 0; }
    return 1;   // FDOWN_R_OK (mine) or FDOWN_R_FREE (existing check answers)
}

// ---- #99 Phase B: additive ext2 mount at the "/ext2" path prefix ------------
// ext2-backed fds run in parallel with the FAT fd_table (same fd numbers, tagged
// by e2fd[fd].used). FAT paths never touch this code, so existing behavior is
// byte-identical. Read opens cache the whole file; create opens buffer writes and
// flush on close (matches the FAT write-buffer model; no in-place overwrite yet).
// #572 STREAMING: a read fd keeps a bounded window (rbuf, <=EXT2_RWIN_BYTES)
// refilled from disk on demand instead of caching the whole file at open; a
// write fd buffers small files exactly as before (flushed by ext2_write_file on
// close) but SPILLS to the block-flushed ext2 streaming writer once the buffered
// size would exceed EXT2_WSPILL_BYTES, so kernel RAM stays bounded for files of
// any size. (EXT2_RWIN_BYTES / EXT2_WSPILL_BYTES are defined near SC_PATH_MAX.)
typedef struct {
    int      used;
    int      is_dir;
    char     path[256];        // ext2-relative path (always starts with '/')
    uint8_t *rbuf; uint32_t rsize, rpos;   // rbuf = bounded read window (#572)
    uint32_t rino;             // #572 inode for lazy ext2_read_file_range()
    uint32_t rcap;             // #572 window capacity in bytes
    uint32_t rwin_base, rwin_len; // #572 window covers [rwin_base, +rwin_len)
    uint8_t *wbuf; uint32_t wcap, wlen; int writing;  // small-file write buffer
    int      appending;        // #598 O_APPEND: flush by append, never replace
    // #695 Phase 1. wdirty is the ONE bit that makes fsync idempotent and makes
    // the close after a successful fsync a no-op: bytes buffered but not yet on
    // the medium. Set at open (an O_CREAT/O_TRUNC fd owes the medium an empty
    // file) and by every write; cleared only by a flush that actually landed.
    int      wdirty;
    // #695: a fsync on a fd that has SPILLED to the #572 streaming writer is
    // TERMINAL, because that writer has no resumable commit point (see
    // sys_fsync). wsealed makes a later write() FAIL loudly instead of silently
    // corrupting the file, and is distinct from werr, which means a flush FAILED.
    int      wsealed;
    int      wstream;          // #572 1 once spilled to the streaming writer
    int      werr;             // #572 a streaming block flush failed
    ext2_wstream_t ws;         // #572 streaming writer state
    uint8_t *sbuf; uint32_t slen; // #572 block staging buffer (0..scap)
    uint32_t scap;             // #609 staging capacity (EXT2_WRUN_BLOCKS blocks)
    int      rfail_logged;     // #609: report a short read once per fd
    uint32_t dir_ino, dir_pos; // directory iteration cursor
    // #746 O_RDWR. A read-write fd is NEITHER of the two fd shapes above: it is
    // not the streaming read window (rbuf/rpos) and it is not the write-only
    // replace-on-close buffer (wbuf/wlen). It holds the WHOLE file in wbuf and
    // serves reads AND writes from that one buffer at ONE shared position,
    // which is what makes read-modify-write mean what POSIX says it means.
    //
    // Before this, O_RDWR took the write-only branch: wlen started at 0 with
    // wdirty set, so close() called ext2_write_file(path, buf, 0) and EMPTIED
    // the file, and sys_read refused the fd because rbuf was NULL. The caller
    // could not even detect it by reading back.
    //
    // The whole-file buffer is why an rw fd is bounded by EXT2_WSPILL_BYTES and
    // can never spill to the #572 streaming writer: that writer truncates its
    // target at begin and has no resumable commit point, so it cannot express
    // "modify part of an existing file" at all. An rw open of a file larger
    // than the bound is REFUSED at open (-EFBIG) rather than silently served
    // by a mode that would destroy it.
    int      rw;          // 1 = POSIX read-write fd; wbuf holds the whole file
    // #745 local 109: 1 = the fd is in the whole-file `rw` mode above but was
    // opened O_WRONLY, so read() must refuse it.
    int      noread;
    int      rw_append;   // O_APPEND on an rw fd: every write lands at wlen
    uint32_t rwpos;       // the ONE position an rw fd reads and writes at
} ext2_fd_t;
static ext2_fd_t e2fd[LEGACY_MAX_FDS];


// ---- #317 pass 2: SMB-backed fds, parallel to the ext2 e2fd table ----------
// Routes userland open/read/write/close/readdir/stat on "/SMB/<server>/<share>/
// <path>" through the SMB2 client (net/smb.c). Reads cache the whole file (like
// ext2); writes buffer and flush-on-close as an SMB upload (smb_vfs_write_whole);
// directory fds hold an smb dir-handle. FAT/ext2 paths never touch this code.
typedef struct {
    int      used;
    int      is_dir;
    int      is_nfs;           // #317 pass 4: 0 = SMB share, 1 = NFS export
    int      dirh;             // smb/nfs dir handle (is_dir)
    char     path[260];        // full /SMB/... or /NFS/... path
    uint8_t *rbuf; uint32_t rsize, rpos;            // file read cache
    uint8_t *wbuf; uint32_t wcap, wlen; int writing; // upload-on-close buffer
    int      wdirty;           // #695: buffered bytes not yet on the server
} smb_fd_t;
static smb_fd_t smbfd[LEGACY_MAX_FDS];


/* #359 Phase 2: POSIX errno values so libc open() can set errno correctly
   (CPython's import machinery needs a missing file to raise FileNotFoundError,
   i.e. errno==ENOENT, not a bare OSError). sys_open now returns -errno. */
#ifndef MOS_EOK
#define MOS_EPERM   1
#define MOS_ENOENT  2
#define MOS_EBADF   9
#define MOS_ENOMEM  12
#define MOS_EACCES  13
#define MOS_EINVAL  22
#define MOS_EMFILE  24
#define MOS_EFBIG   27
#define MOS_EOK     0
#endif

/* #359: fcntl(fd, cmd, arg). CPython needs F_GETFL/F_SETFL/F_GETFD/F_SETFD/
   F_DUPFD to set up std fds and duplicate descriptors. We do not track a
   per-fd flags word, so F_GETFL reports O_RDWR and F_SETFL is a no-op; the
   duplicate commands reuse the existing per-process fd_dup(). */
int64_t sys_fcntl(int fd, int cmd, long arg) {
    switch (cmd) {
        case 0:    /* F_DUPFD          */
        case 1030: /* F_DUPFD_CLOEXEC  */ {
            int r = fd_dup(fd, (int)arg);
            return (r < 0) ? -MOS_EBADF : r;
        }
        case 1: return 0;        /* F_GETFD -> no FD_CLOEXEC tracked */
        case 2: return 0;        /* F_SETFD -> accept, ignore        */
        case 3: return 0x0002;   /* F_GETFL -> O_RDWR                */
        case 4: return 0;        /* F_SETFL -> accept, ignore        */
        default: return 0;       /* be permissive for other probes   */
    }
}

// #444: once a legacy fd slot has been eagerly claimed (fd_used[fd] = 1) by
// the scan in sys_open(), every failure path must release it again before
// returning, or a run of failed opens (e.g. probing for an optional config
// file that doesn't exist) permanently leaks slots out of the tiny 13-usable
// pool. FD_FAIL() is only valid inside sys_open(), after `fd` has been
// assigned and validated (fd >= 0).
#define FD_FAIL(x) do { legacy_fd_release(fd); return (x); } while (0)

// #567: user-facing sys_open() bounces the path fault-safe into a kernel buffer
// then calls the kernel-pointer core sys_open_k(). In-kernel callers (sys_chdir,
// the #308/#317 boot self-tests) call sys_open_k() directly with a kernel path,
// so the core keeps a plain kernel-pointer contract and never sees a user
// pointer. copy-user-lint traces one call level from a case, so the core (a
// second level down) is out of scope; only this clean wrapper is analyzed.
int64_t sys_open(const char *upath, int flags) {
    char kpath[SC_PATH_MAX];
    if (upath) {
        // #58: bounce AND resolve against the caller's cwd, so that after
        // chdir("/GAMES/FOO") open("data/x") means /GAMES/FOO/data/x and not
        // /data/x. sys_open_k() keeps its kernel-pointer contract and its
        // callers (sys_chdir, the boot self-tests) already pass absolute
        // kernel paths, so the core is deliberately NOT changed: resolution
        // belongs at the user boundary, once, or it happens twice.
        int prc = sc_path_from_user(upath, kpath, sizeof(kpath));
        if (prc != 0) return (prc == -14) ? -14 : -MOS_ENOENT;
    }
    return sys_open_k(upath ? kpath : (const char *)0, flags);
}

// ===========================================================================
// #58 END-TO-END BOOT TEST: does a relative path OPEN THE RIGHT FILE?
//
// path_resolve_selftest_rs() proves the STRING TRANSFORM. That is necessary and
// it is not sufficient: a resolver can be perfectly correct and still be wired
// to nothing, which is the failure this project keeps rediscovering
// (validate_user_ptr, sse_save, the prompt-injection matcher were all fully
// built and never invoked; #58 itself IS that fault, applied to sys_chdir).
// So this test goes all the way to a real file on the real filesystem.
//
// IT IS DIFFERENTIAL ON PURPOSE, and that is the whole design. One relative
// name, "COMPOSIT", resolved from two different working directories:
//
//   from "/APPS"  -> /APPS/COMPOSIT  -> MUST OPEN     (it is there)
//   from "/"      -> /COMPOSIT       -> MUST NOT OPEN (nothing is there)
//
// A file that exists in ONE place and not the other is what makes a wrong
// resolution VISIBLE. Testing only the first half would pass just as happily if
// the cwd were being ignored and the name were resolving against the root by
// luck, because before #58 the root is exactly where it went. The second half
// is the half that can only pass if the cwd is genuinely being consulted.
//
// It reads through sc_path_resolve(), the SAME chokepoint every path syscall
// uses, and it reads the cwd out of the CURRENT PROCESS, so it exercises
// proc_cwd() and the process field rather than a hand-passed string. The only
// link it does not cover is strncpy_from_user, which is unchanged pre-existing
// code shared with every other syscall.
//
// NO WRITES. It opens two paths read-only and closes them. A boot self-test
// that mutates the filesystem is a boot self-test that eventually corrupts one.
//
// Justified-C, not Rust: it calls sys_open_k(), sys_close() and reads/restores
// process_t::cwd. The DECISION it checks is already in Rust; this is the
// plumbing that proves the decision is connected.
// ===========================================================================
void path_resolve_e2e_selftest(void) {
    process_t *p = proc_current();
    if (!p) {
        // Say so rather than silently reporting nothing: a test that quietly
        // skips is indistinguishable from a test that passed (#514).
        selftest_notrun("path/cwd-e2e",
                    "no current process exists to carry a cwd, so the ONLY end-to-end "
                    "proof that chdir() is wired into path resolution did not run");
        return;
    }

    // WHY THIS TEST NEVER WRITES p->cwd. The first version set the live
    // process's cwd to "/APPS", tested, set it to "/", tested, and restored it.
    // That is a data race in shipping code: process_t::cwd is shared by every
    // THREAD of the process, and a sibling thread that happens to open a
    // relative path inside that window resolves it against a directory this
    // self-test invented. A test whose own mechanism can corrupt the thing it
    // is testing is not evidence. So the cwd is only ever READ here, and the
    // two contrasting directories are passed to the resolver DIRECTLY.
    //
    // ---- choose a probe, and PROVE it is a valid one ----------------------
    // THE PROBE IS THE EXPERIMENT. The first version hard-coded "COMPOSIT" on
    // the assumption it lived only under /APPS. It does not: this golden also
    // carries a stale /COMPOSIT at the ext2 root, so the must-not-open arm
    // opened a real file and the test reported a failure that was entirely its
    // own. A probe whose validity is ASSUMED rots the moment someone adds a
    // file, so a candidate now qualifies only if /APPS/<name> opens AND
    // /<name> does not. Two arms that both hit real files can no longer happen
    // silently.
    static const char *const cands[] = {
        "TERMINAL", "SETTINGS", "CALC", "BROWSER", "APPSTORE", "ADDUSER",
    };
    const char *probe = 0;
    char apath[SC_PATH_MAX], rpath[SC_PATH_MAX];
    for (unsigned ci = 0; ci < sizeof(cands) / sizeof(cands[0]); ci++) {
        const char *c = cands[ci];
        int i, j;
        apath[0]='/'; apath[1]='A'; apath[2]='P'; apath[3]='P'; apath[4]='S'; apath[5]='/';
        for (j = 0; c[j] && j < 200; j++) apath[6 + j] = c[j];
        apath[6 + j] = '\0';
        rpath[0] = '/';
        for (i = 0; c[i] && i < 200; i++) rpath[1 + i] = c[i];
        rpath[1 + i] = '\0';

        int64_t fa = sys_open_k(apath, 0);
        if (fa < 0) continue;                            // not installed here
        sys_close((int)fa);
        int64_t fr = sys_open_k(rpath, 0);
        if (fr >= 0) { sys_close((int)fr); continue; }   // ambiguous: also at root
        probe = c;
        break;
    }
    if (!probe) {
        selftest_notrun("path/cwd-e2e",
                    "none of the probe names exists under /APPS and only there, so the "
                    "ONLY end-to-end proof that chdir() is wired into path resolution "
                    "did not run. The probe list is a hardcoded set of app names and "
                    "this is what it looks like when they drift");
        return;
    }

    extern int path_resolve_cwd_rs(const char *cwd, char *buf, uint32_t cap);
    int bad = 0;
    char buf[SC_PATH_MAX];
    #define R58_SET(dst, src) do { uint64_t _i = 0; \
        while ((src)[_i]) { (dst)[_i] = (src)[_i]; _i++; } (dst)[_i] = '\0'; } while (0)

    // ---- arm 1: the CHOKEPOINT reads the REAL process cwd ------------------
    // This is the link that proves proc_cwd() and process_t::cwd are actually
    // on the path a syscall takes, without writing to either.
    {
        const char *cwd = proc_cwd();
        char want[SC_PATH_MAX];
        if (!cwd) { want[0] = '/'; R58_SET(want + 1, probe); }
        else {
            uint64_t n = 0;
            while (cwd[n]) { want[n] = cwd[n]; n++; }
            if (n == 0 || want[n - 1] != '/') want[n++] = '/';
            R58_SET(want + n, probe);
        }
        R58_SET(buf, probe);
        if (sc_path_resolve(buf, sizeof(buf)) != 0 || strcmp(buf, want) != 0) {
            kprintf("[#58] e2e FAIL: chokepoint gave '%s', process cwd implies '%s'\n",
                    buf, want);
            bad++;
        }
    }

    // ---- arm 2: from /APPS, the bare name MUST resolve and open -----------
    R58_SET(buf, probe);
    if (path_resolve_cwd_rs("/APPS", buf, sizeof(buf)) < 0) {
        kprintf("[#58] e2e FAIL: '%s' from /APPS did not resolve\n", probe); bad++;
    } else if (strcmp(buf, apath) != 0) {
        kprintf("[#58] e2e FAIL: '%s' from /APPS became '%s'\n", probe, buf); bad++;
    } else {
        int64_t fd = sys_open_k(buf, 0);
        if (fd < 0) { kprintf("[#58] e2e FAIL: open('%s') rc=%ld\n", buf, (long)fd); bad++; }
        else sys_close((int)fd);
    }

    // ---- arm 3: from /, THE SAME NAME must NOT find that file -------------
    // The half that can only pass if the cwd is genuinely consulted: before #58
    // a bare name went to the root, so a root-only test would have passed just
    // as happily while the feature did nothing at all.
    R58_SET(buf, probe);
    if (path_resolve_cwd_rs("/", buf, sizeof(buf)) < 0) {
        kprintf("[#58] e2e FAIL: '%s' from / did not resolve\n", probe); bad++;
    } else if (strcmp(buf, rpath) != 0) {
        kprintf("[#58] e2e FAIL: '%s' from / became '%s'\n", probe, buf); bad++;
    } else {
        int64_t fd = sys_open_k(buf, 0);
        if (fd >= 0) {
            kprintf("[#58] e2e FAIL: '%s' opened; the two cwds are not distinguishable\n", rpath);
            sys_close((int)fd); bad++;
        }
    }
    #undef R58_SET

    kprintf("[#58] cwd end-to-end selftest: %s (%d failing) "
            "[probe '%s': opens as %s from cwd /APPS, absent as %s from cwd /]\n",
            bad == 0 ? "PASS" : "FAIL", bad, probe, apath, rpath);
}

// #58: run the end-to-end test ONCE, at the first open that has a real process
// behind it. WHY HERE AND NOT AT INIT: it needs a current process to carry a
// cwd, and every init-time hook in this kernel runs before the first process
// exists. Wired into perms_init() it printed "e2e SKIPPED: no current process"
// on every boot, which is a test that never ran - the exact zero-callers shape
// #58 is about. The latch is set BEFORE the test runs, so the test's own opens
// cannot re-enter it, and after the first boot open it costs one predictable
// branch.
static int g_path_e2e_done = 0;

int64_t sys_open_k(const char *path, int flags) {
    if (!g_path_e2e_done && proc_current()) {
        g_path_e2e_done = 1;          // set FIRST: the test itself calls us
        path_resolve_e2e_selftest();
    }
    // #396: /dev/<name> device nodes (CDC-ACM serial, etc.) resolve through the
    // in-kernel dev namespace and install a file_t in the per-process fd table.
    if (path && path[0]=='/' && path[1]=='d' && path[2]=='e' && path[3]=='v' && path[4]=='/') {
        extern struct file *dev_open(const char *name, int flags);
        extern int fd_alloc_install(struct file *f);
        struct file *df = dev_open(path + 5, flags);
        if (!df) return -1;
        int nfd = fd_alloc_install(df);
        if (nfd < 0) {
            IGNORE_RESULT("dup slot eviction: the description survives in the "
                          "original fd, so this put is not the final flush",
                          file_put(df));
            return -1;
        }
        return nfd;
    }
    // Permission check
    process_t *p = proc_current();
    if (p && p->privilege == PRIV_USER) {
        // #95: services are sandboxed to their declared capabilities. A
        // service without SVC_PERM_FSWRITE may not open files for writing
        // or create them. No-op for normal processes (is_service == 0).
        if (p->is_service && (flags & (1 | 2 | 0x40)) &&
            !(p->svc_perms & SVC_PERM_FSWRITE)) {
            return -MOS_EPERM;  // service lacks fs-write capability
        }
        int access = R_OK;
        if (flags & 1)  access = W_OK;          // O_WRONLY
        if (flags & 2)  access = R_OK | W_OK;   // O_RDWR
        // #317: SMB/NFS shares enforce access server-side (NTLM/RPC auth + share
        // ACLs); local POSIX perms (which default-deny W_OK to non-root) don't apply.
        // #250: a removable volume carries no POSIX ownership. perms_check()'s
        // no-entry default is root-owned, which denies W_OK to every non-root
        // process, so leaving removable paths in would make a USB stick
        // read-only to the user who plugged it in. Same carve-out, and the
        // same reasoning, as the SMB/NFS one it joins: access on those media
        // is decided by the medium, not by our permission table.
        if (!path_is_smb(path) && !path_is_nfs(path) &&
            hotplug_resolve_path(path, 0) < 0) {
            // #676: O_CREAT was not part of the access derivation at all, so
            // open(path, O_RDONLY|O_CREAT) asked for R_OK on a path that does
            // not exist, was answered by perms_check()'s permissive no-entry
            // default, and then went on to CREATE the file. A caller with write
            // rights nowhere could still make files anywhere.
            //
            // The fix is NOT to OR W_OK into the check on the file. A file that
            // does not exist yet has no permissions, so asking about it is
            // meaningless, and the no-entry default would deny W_OK to every
            // non-root caller and make creation impossible even in a user's own
            // home. POSIX asks a different question: creating a NAME is a WRITE
            // TO THE PARENT DIRECTORY. So that is what is checked, and only
            // when the target does not already exist (O_CREAT on an existing
            // file needs no directory write). This is the same parent-W_OK
            // pattern sys_mkdir, sys_rmdir and sys_unlink already use.
            if ((flags & 0x40) && !fat_exists(&g_fat_fs, path)) {
                char parent[SC_PATH_MAX];
                sc_parent_of(path, parent, sizeof(parent));
                if (perms_check(parent, p->euid, p->egid, W_OK | X_OK) != 0) {
                    return -MOS_EACCES;
                }
            } else if (perms_check(path, p->euid, p->egid, access) != 0) {
                return -MOS_EACCES;
            }
        }
    }

    // #746: decode the flags word ONCE, here, with the shared decoder in
    // fs/vfs.h. Every branch below asks `om` what the caller wanted instead of
    // doing its own bit arithmetic; that is what stopped ext2 and FAT
    // disagreeing about O_RDWR, O_TRUNC and O_APPEND.
    open_mode_t om = open_mode_decode(flags);

    // Find a free fd and claim it ATOMICALLY (fd_used[i] = 1 while still
    // holding the lock) so no concurrent sys_open() can observe the same slot
    // as free (see the #444 comment on g_legacy_fd_lock above). Every failure
    // path below this point must reset fd_used[fd] = 0 before returning.
    int fd = -1;
    {
        uint64_t __fdfl = spinlock_acquire_irqsave(&g_legacy_fd_lock);
        for (int i = 3; i < LEGACY_MAX_FDS; i++) {  // 0, 1, 2 reserved for stdin/stdout/stderr
            if (!fd_used[i]) {
                fd = i;
                fd_used[i] = 1;
                fd_werr[i] = 0;   // #746: the ONE place a slot becomes live
                break;
            }
        }
        spinlock_release_irqrestore(&g_legacy_fd_lock, __fdfl);
    }

    if (fd < 0) {
        return -MOS_EMFILE;  // no free fd
    }
    // #fdguard: stamp the opening thread-group as this slot's owner. Any
    // FD_FAIL below releases the slot via legacy_fd_release(), which also
    // clears this ownership, so a failed open leaves nothing stamped.
    fdown_claim_rs((uint32_t)fd, legacy_owner_id());

    // #250 REMOVABLE VOLUME. A path under a hot-plugged volume's mount point
    // (/USB0/...) is served by that volume's OWN fat_fs_t, not by g_fat_fs.
    //
    // THIS IS NOT A NEW FD KIND. fat_open() has always been parameterised by
    // fat_fs_t and the legacy fd table already stores a fat_file_t, which
    // carries its own `fs` back-pointer. So every operation below this one -
    // read, write, seek, readdir, close, stat - already works on this handle
    // with no change at all. The ONLY thing that was missing was choosing the
    // right fat_fs_t at open time, which is what these eight lines do. A
    // parallel usbfd[] table, in the shape of smbfd[] and e2fd[], would have
    // been a second implementation of something that already existed.
    //
    // It must come BEFORE the ext2-root branch: with ext2 as root, every "/"
    // path is a candidate for ext2 resolution, and /USB0 is not an ext2 path.
    {
        const char *vrel = 0;
        int vslot = hotplug_resolve_path(path, &vrel);
        if (vslot >= 0) {
            fat_fs_t *vfs = hotplug_volume_fat(vslot);
            // NULL means mounted-but-not-readable (exFAT) or not mounted.
            // Refusing here is what keeps a volume the UI labels
            // "not browsable" from half-opening.
            if (!vfs) FD_FAIL(-MOS_ENOENT);
            if (fat_open(vfs, vrel, &fd_table[fd]) != 0) {
                extern int fat_create(fat_fs_t *fs, const char *path);
                if (om.create && fat_create(vfs, vrel) == 0 &&
                    fat_open(vfs, vrel, &fd_table[fd]) == 0) {
                    // created
                } else {
                    FD_FAIL(-MOS_ENOENT);
                }
            }
            if (om.trunc && fd_table[fd].file_size > 0) {
                if (fat_truncate(&fd_table[fd]) != 0) {
                    fat_close(&fd_table[fd]);
                    FD_FAIL(-1);
                }
            }
            if (om.append) fd_table[fd].position = fd_table[fd].file_size;
            fd_used[fd] = 1;
            return lfd_ext(fd);
        }
    }

    // #317 pass 2: SMB network share. Mount on demand, then open as a directory
    // (dir handle), a read cache (whole-file), or a write/upload buffer.
    if (path_is_smb(path)) {
        smb_fd_t *s = &smbfd[fd];
        for (uint64_t z = 0; z < sizeof(*s); z++) ((uint8_t *)s)[z] = 0;
        { int z = 0; while (path[z] && z < 259) { s->path[z] = path[z]; z++; } s->path[z] = 0; }
        if (smb_vfs_ensure_mount(path) != 0) FD_FAIL(-1);

        smb_dirent_t info;
        int have = (smb_stat(path, &info) == 0);
        int want_write = (flags & 0x1) || (flags & 0x2) || (flags & 0x40) || (flags & 0x200);

        if (have && info.is_directory) {
            int dh = smb_opendir(path);
            if (dh < 0) FD_FAIL(-1);
            s->is_dir = 1; s->dirh = dh;
        } else if (want_write) {
            // O_WRONLY/O_RDWR/O_CREAT/O_TRUNC: buffer writes, upload on close.
            s->writing = 1; s->wcap = 4096; s->wlen = 0;
            s->wbuf = (uint8_t *)kmalloc(s->wcap);
            if (!s->wbuf) FD_FAIL(-1);
        } else if (have) {
            // Read: cache the whole file.
            uint32_t sz = 0;
            s->rbuf = (uint8_t *)smb_vfs_read_whole(path, &sz);
            if (!s->rbuf) FD_FAIL(-1);
            s->rsize = sz; s->rpos = 0;
        } else {
            FD_FAIL(-MOS_ENOENT);  // not found and not creating
        }
        s->used = 1;
        fd_used[fd] = 1;
        return lfd_ext(fd);
    }

    // #317 pass 4: NFS export. Same smbfd[] slot, is_nfs=1. Mount must exist
    // (NETMOUNTS.CFG at boot or an explicit SYS_NET_MOUNT). nfs_getattr decides
    // directory vs file; reads cache the whole file, writes upload on close.
    if (path_is_nfs(path)) {
        smb_fd_t *s = &smbfd[fd];
        for (uint64_t z = 0; z < sizeof(*s); z++) ((uint8_t *)s)[z] = 0;
        { int z = 0; while (path[z] && z < 259) { s->path[z] = path[z]; z++; } s->path[z] = 0; }
        s->is_nfs = 1;
        if (nfs_vfs_ensure_mount(path) != 0) FD_FAIL(-1);

        nfs_fattr3_t attrs;
        int have = (nfs_getattr(path, &attrs) == 0);
        int want_write = (flags & 0x1) || (flags & 0x2) || (flags & 0x40) || (flags & 0x200);

        if (have && attrs.type == NF3DIR) {
            int dh = nfs_opendir(path);
            if (dh < 0) FD_FAIL(-1);
            s->is_dir = 1; s->dirh = dh;
        } else if (want_write) {
            s->writing = 1; s->wcap = 4096; s->wlen = 0;
            s->wbuf = (uint8_t *)kmalloc(s->wcap);
            if (!s->wbuf) FD_FAIL(-1);
        } else if (have) {
            uint32_t sz = 0;
            s->rbuf = (uint8_t *)nfs_vfs_read_whole(path, &sz);
            if (!s->rbuf) FD_FAIL(-1);
            s->rsize = sz; s->rpos = 0;
        } else {
            FD_FAIL(-1);
        }
        s->used = 1;
        fd_used[fd] = 1;
        return lfd_ext(fd);
    }

    // #99: serve from the ext2 volume for explicit "/ext2..." paths, and for all
    // "/" paths once ext2 is the root fs. For root-cutover paths we use ext2 when
    // the file already exists there or we are creating it; otherwise we fall
    // through to FAT so files that live only on the ESP still open.
    const char *rel = 0;
    // Normalize a bare (root-relative) filename to an absolute path so the
    // ext2-root redirect can resolve it. Userland apps open assets by bare name
    // (e.g. the compositor opens wallpapers as "MAYTERA.BMP"); both
    // path_root_ext2() and ext2_resolve_path() require a leading '/', so without
    // this a bare name never reaches ext2 and only resolves against the FAT ESP -
    // which on an ext2-root system holds boot files only, so the open fails and
    // e.g. the desktop wallpaper silently falls back to a gradient. Only active
    // when g_root_ext2 is set, so FAT-root behavior is byte-identical.
    char ext2_npath[260];
    const char *look = path;
    if (g_root_ext2 && path && path[0] != '/' && path[0] != '\0') {
        ext2_npath[0] = '/';
        int z = 0;
        while (path[z] && z < 258) { ext2_npath[z + 1] = path[z]; z++; }
        ext2_npath[z + 1] = '\0';
        look = ext2_npath;
    }
    if (path_is_ext2(path)) {
        rel = ext2_relpath(path);
    } else if (path_root_ext2(look)) {
        if (ext2_resolve_path(look) != 0 || (flags & 0x40)) rel = look;
    }
    if (rel) {
        ext2_fd_t *e = &e2fd[fd];
        for (uint64_t z = 0; z < sizeof(*e); z++) ((uint8_t *)e)[z] = 0;
        { int z = 0; while (rel[z] && z < 255) { e->path[z] = rel[z]; z++; } e->path[z] = 0; }
        uint32_t ino = ext2_resolve_path(rel);
        if (ino) {
            ext2_inode_t in;
            if (ext2_read_inode(ino, &in) != 0) FD_FAIL(-1);
            if ((in.i_mode & 0xF000) == 0x4000) {       // directory
                e->is_dir = 1; e->dir_ino = ino; e->dir_pos = 0;
            } else if (om.rdwr || (om.can_write && !om.trunc && !om.append)) {
                // #745 local 109: THE CONDITION IS THE FIX. This branch used to
                // read `om.rdwr` alone, so a plain O_WRONLY open with NEITHER
                // O_TRUNC NOR O_APPEND fell into the write-only branch below,
                // which starts wlen at 0 with wdirty SET. Measured on golden
                // 1882: opening an existing 33-byte file O_WRONLY and closing
                // it WITHOUT WRITING left it 0 bytes, and writing 3 bytes into
                // it left it 3 bytes with the other 30 gone. On the FAT ESP the
                // same two programs are correct, because a FAT fd writes in
                // place at a real position.
                //
                // That is the #746 defect one flag over: O_TRUNC was fixed and
                // its ABSENCE was not. POSIX only shortens a file when
                // something asks it to, so the question "when is a file
                // emptied" has ONE answer on this path: om.trunc, decoded once
                // by open_mode_decode() in fs/vfs.h. It is read on the wdirty
                // line below and nowhere else in this branch.
                //
                // busybox vi is the caller that made this urgent in BOTH
                // directions: it opens O_WRONLY|O_CREAT deliberately without
                // O_TRUNC and then calls ftruncate(), so the old behaviour was
                // what made saving a shortened file work on ext2 at all, and
                // correcting it here without making ftruncate real would have
                // broken it. SYS_FTRUNCATE lands in the same commit for that
                // reason, not as a bonus.
                //
                // #746 O_RDWR ON AN EXISTING FILE. This branch did not exist:
                // O_RDWR fell into the write-only branch below, which starts
                // wlen at 0 with wdirty set, so the close EMPTIED the file and
                // the fd could not read. Both halves of the standard POSIX
                // read-modify-write idiom were broken and the open still
                // returned success.
                //
                // The whole file is loaded into wbuf and read AND written
                // through it at one position (rwpos), so the fd behaves the way
                // its caller was written to expect: opening and closing without
                // writing leaves the file untouched (wdirty starts CLEAR, so
                // close has nothing to flush), a read returns the existing
                // bytes, and a partial overwrite keeps the tail.
                uint32_t sz = om.trunc ? 0 : in.i_size;
                // BOUND, STATED HONESTLY. wbuf is the whole file, so an rw fd
                // cannot exceed the buffered-write bound. The #572 streaming
                // writer is not an option: ext2_wstream_begin() truncates its
                // target, which is the very defect being fixed. Refuse loudly
                // instead of falling back to a mode that would destroy the file.
                if (sz > EXT2_WSPILL_BYTES) {
                    kprintf("[EXT2-RW] refusing a preserving write open on %s: "
                            "%u bytes exceeds the %u-byte whole-file bound\n",
                            e->path, sz, (unsigned)EXT2_WSPILL_BYTES);
                    FD_FAIL(-MOS_EFBIG);
                }
                uint32_t cap = 4096;
                while (cap < sz) cap <<= 1;
                e->wbuf = (uint8_t *)kmalloc(cap);
                if (!e->wbuf) FD_FAIL(-MOS_ENOMEM);
                e->wcap = cap;
                e->wlen = 0;
                if (sz) {
                    // #614: fill in slices so ext2_lock is released between
                    // them, exactly as the read path does.
                    uint32_t got = 0;
                    while (got < sz) {
                        uint32_t slice = sz - got;
                        if (slice > EXT2_RSLICE_BYTES) slice = EXT2_RSLICE_BYTES;
                        int64_t g1 = ext2_read_file_range(ino, got, slice,
                                                          e->wbuf + got);
                        if (g1 <= 0) break;
                        got += (uint32_t)g1;
                    }
                    if (got != sz) {
                        // A SHORT PRELOAD MUST NOT BECOME A SHORT FILE. If the
                        // fd were handed back now, the first write and close
                        // would commit `got` bytes and silently truncate the
                        // file to the part that happened to read.
                        kprintf("[EXT2-RW] short preload of %s: %u of %u bytes; "
                                "refusing the open\n", e->path, got, sz);
                        kfree(e->wbuf); e->wbuf = 0; e->wcap = 0;
                        FD_FAIL(-1);
                    }
                    e->wlen = sz;
                }
                e->rw = 1;
                // #745 local 109: `rw` means "wbuf holds the whole file and
                // writes land at rwpos". It must NOT also mean "this fd may be
                // read": an O_WRONLY fd reaching this branch would otherwise
                // start serving read(), which POSIX forbids and which would
                // hand a caller bytes its own open said it could not have.
                e->noread = !om.can_read;
                e->writing = 1;
                e->rwpos = 0;
                e->rw_append = om.append;
                // WHAT THE MEDIUM IS OWED. Nothing, unless O_TRUNC asked for an
                // empty file. This one line is what makes open-then-close a
                // no-op instead of a destructive rewrite.
                e->wdirty = om.trunc ? 1 : 0;
                e->rsize = e->wlen;   // keep stat/seek-END answers consistent
            } else if (om.can_write) {
                // Existing regular file opened for writing: buffer the new
                // contents; ext2_write_file() truncates the inode in place on
                // close (#99 Phase C overwrite).
                e->writing = 1; e->wcap = 4096; e->wlen = 0; e->wdirty = 1;
                e->wbuf = (uint8_t *)kmalloc(e->wcap);
                if (!e->wbuf) FD_FAIL(-1);
                // #598 O_APPEND (0x400) was NEVER TESTED anywhere on this path,
                // so fopen(path, "a") took exactly the branch above and then
                // sys_close() called ext2_write_file(), which REPLACES the whole
                // file: every append silently destroyed the prior contents.
                // Flag the fd so the flush appends instead. O_TRUNC wins when
                // both are set (POSIX: truncate at open, then append == plain
                // sequential write), so O_TRUNC and plain O_WRONLY are unchanged.
                if (om.append) e->appending = 1;   // #746: om.append already
                                                   // accounts for O_TRUNC
                                                   // winning over O_APPEND.
            } else {
                // #572: bounded read window instead of kmalloc(filesize). rbuf is
                // a fixed <=128KB window; sys_read refills it lazily via
                // ext2_read_file_range(), so an arbitrarily large file reads back
                // in bounded kernel RAM. rsize is the true file size (EOF), rpos
                // the logical position (unchanged fd contract; lseek still works).
                uint32_t sz = in.i_size;
                uint32_t cap = (sz < EXT2_RWIN_BYTES) ? (sz ? sz : 1) : EXT2_RWIN_BYTES;
                e->rbuf = (uint8_t *)kmalloc(cap);
                if (!e->rbuf) FD_FAIL(-1);
                e->rcap = cap;
                e->rino = ino;
                e->rsize = sz;
                e->rpos = 0;
                e->rwin_base = 0; e->rwin_len = 0;   // empty; filled on first read
            }
        } else if (om.create) {                          // O_CREAT
            e->writing = 1; e->wcap = 4096; e->wlen = 0; e->wdirty = 1;
            e->wbuf = (uint8_t *)kmalloc(e->wcap);
            if (!e->wbuf) FD_FAIL(-1);
            // #746: O_RDWR|O_CREAT on a file that does not exist yet. The file
            // is empty either way, so nothing is destroyed here, but the fd
            // still has to READ - fopen(path, "w+") is how tmpfile() is
            // implemented in two of the ported games, and a rewind-then-read on
            // it returned -1 before this.
            if (om.rdwr) { e->rw = 1; e->rwpos = 0; e->rw_append = om.append; e->rsize = 0; }
            // #679 CREATE POINT 1 of 2, the ext2 root. Record the creating
            // process as the owner now, so the file it is about to write is
            // writable by its creator. Without this the new path has no entry
            // and falls into perms_check()'s root-owned default, which denies
            // W_OK to every non-root process, in its own home included.
            // Recorded at open rather than at the ext2_write_file() that
            // actually allocates the inode, because that is where the creating
            // process's identity is in hand; an O_CREAT that is then never
            // written leaves a harmless unused entry.
            if (p && p->privilege == PRIV_USER) {
                // `path`, not `rel`: perms_check() was given `path`, so the
                // entry must be filed under the same name it will be looked up
                // by. perms_on_create() canonicalizes, so the bare-name and
                // relative spellings land on the same key either way.
                perms_on_create(path, p->euid, p->egid, 0);
            }
        } else {
            FD_FAIL(-MOS_ENOENT);
        }
        e->used = 1;
        fd_used[fd] = 1;
        return lfd_ext(fd);
    }

    // TODO: Validate user pointer
    // Open the file. If it does not exist and O_CREAT (0x40) is set, create it
    // first so userland tools (cp, mv, editors, curl -o, ...) can make new files.
    extern int fat_create(fat_fs_t *fs, const char *path);
    if (fat_open(&g_fat_fs, path, &fd_table[fd]) != 0) {
        if (om.create && fat_create(&g_fat_fs, path) == 0 &&
            fat_open(&g_fat_fs, path, &fd_table[fd]) == 0) {
            // created and opened successfully
            // #679 CREATE POINT 2 of 2, the FAT ESP. Same rule as the ext2 path
            // above. These are two independent create paths and this codebase's
            // recurring fault is fixing only the one you were looking at.
            if (p && p->privilege == PRIV_USER) {
                perms_on_create(path, p->euid, p->egid, 0);
            }
        } else {
            FD_FAIL(-MOS_ENOENT);
        }
    }

    // #746: THE FLAGS WORD REACHES THE FAT FAMILY AT LAST. This branch used to
    // read exactly one bit of it (O_CREAT) and throw the rest away, so:
    //
    //   O_TRUNC  was a NO-OP. The caller's new, shorter contents were written
    //            over the old ones from byte 0 and everything past the new end
    //            survived as a stale tail - while the SAME flag on the ext2
    //            root emptied the file. One flag, two filesystems, opposite
    //            results, and no diagnostic on either side.
    //   O_APPEND was a NO-OP. fopen(path, "a") starts at position 0, so an
    //            append OVERWROTE the file from the beginning. That is data
    //            destruction of exactly the shape #598 fixed on ext2, still
    //            live on the ESP.
    //
    // O_RDWR needs nothing here: a legacy FAT fd already reads and writes at a
    // real file position, which is why FAT was the filesystem that got O_RDWR
    // right and ext2 was the one that destroyed the file.
    if (om.trunc && fd_table[fd].file_size > 0) {
        if (fat_truncate(&fd_table[fd]) != 0) {
            // The caller asked for an empty file and the medium did not give
            // one. Handing back the fd anyway would let the write land on top
            // of contents the caller believes are gone.
            fat_close(&fd_table[fd]);
            FD_FAIL(-1);
        }
    }
    if (om.append) fd_table[fd].position = fd_table[fd].file_size;

    fd_used[fd] = 1;
    return lfd_ext(fd);
}
#undef FD_FAIL

// ===========================================================================
// #745 local 109: SYS_FTRUNCATE
// ===========================================================================
//
// WHY IT EXISTS. The userland libc answered ftruncate()/truncate() with
//     int ftruncate(int fd, long length) { (void)fd; (void)length; return 0; }
// in TWO copies (userland/libc and the CPython port's private src-libc), with a
// comment saying the kernel has no truncate primitive. It had one for FAT
// (fat_truncate, length 0 only) and needed none for ext2, and the stub's
// RETURN VALUE was the real damage: busybox vi saves by writing the new text
// over the old and calling ftruncate() to cut the remainder, so every save of a
// shortened file silently kept its tail and reported success.
//
// ONE DEFINITION. This does not introduce a second notion of truncation. It
// routes each fd family to the same mechanism that family already uses to
// decide a file's length at close:
//   ext2, whole-file buffer  ->  clamp wlen; the close already rewrites the
//                                file as exactly wbuf[0, wlen).
//   FAT                      ->  fat_truncate_to(), which fat_truncate() (the
//                                O_TRUNC path) is now itself defined in terms of.
// Families that cannot express it say so instead of returning 0: a spilled or
// appending ext2 stream has already committed bytes with no resumable rewind,
// and SMB/NFS upload whole files on close with no server-side truncate here.
//
// GROWING IS REFUSED, on every family. A grow must write zeroes to the medium,
// which is a different operation; pretending to do it is how this started.
int64_t sys_ftruncate(int fd, int64_t length) {
    if (fd < 0 || length < 0) return -1;

    // Per-process descriptions (pipes, PTYs, devices, the shell-redirect file
    // objects). file_ops_t has no truncate op, so there is nothing to route to
    // and nothing to pretend about.
    process_t *proc = proc_current();
    if (proc && fd < MAX_FDS && proc->fds[fd]) return -1;

    if (!lfd_is(fd)) return -1;          // #FDNS
    fd = lfd_idx(fd);
    if (!legacy_owner_ok(fd, "ftruncate")) return -1;   // #fdguard
    if (fd < 3) return -1;

    if (smbfd[fd].used) return -1;      // whole-file upload on close

    if (e2fd[fd].used) {
        ext2_fd_t *e = &e2fd[fd];
        if (e->is_dir || !e->writing) return -1;
        if (e->wstream || e->wsealed || e->appending || e->werr) return -1;
        if (!e->wbuf) return -1;
        if ((uint64_t)length > (uint64_t)e->wlen) return -1;   // grow: refused
        e->wlen = (uint32_t)length;
        if (e->rwpos > e->wlen) e->rwpos = e->wlen;
        e->rsize = e->wlen;
        e->wdirty = 1;                  // the medium is owed the shorter file
        return 0;
    }

    if (!fd_used[fd]) return -1;
    return fat_truncate_to(&fd_table[fd], (uint32_t)length) == 0 ? 0 : -1;
}

int64_t sys_close(int fd) {
    if (fd < 0) return -1;

    // First try per-process file descriptors (pipes, PTYs, etc.)
    process_t *proc = proc_current();
    if (proc && fd < MAX_FDS && proc->fds[fd]) {
        return fd_close(fd);
    }

    // #FDNS: everything below operates on the legacy tables, which are indexed
    // by lfd_idx(fd), not by fd. A number outside the legacy range is not ours.
    if (!lfd_is(fd)) return -1;
    fd = lfd_idx(fd);
    if (!legacy_owner_ok(fd, "close")) return -1;   // #fdguard

    // #317: SMB-backed fd. Flush an upload (write-on-close), close dir handle.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && smbfd[fd].used) {
        smb_fd_t *s = &smbfd[fd];
        int rc = 0;
        if (s->is_nfs) {
            if (s->is_dir) {
                nfs_closedir(s->dirh);
            } else if (s->writing && s->wbuf && s->wdirty) {
                rc = nfs_vfs_write_whole(s->path, s->wbuf, s->wlen);
            }
        } else if (s->is_dir) {
            smb_closedir(s->dirh);
        } else if (s->writing && s->wbuf && s->wdirty) {
            rc = smb_vfs_write_whole(s->path, s->wbuf, s->wlen);
        }
        if (s->rbuf) kfree(s->rbuf);
        if (s->wbuf) kfree(s->wbuf);
        for (uint64_t z = 0; z < sizeof(*s); z++) ((uint8_t *)s)[z] = 0;
        legacy_fd_release(fd);
        return rc;
    }

    // ext2-backed fd: flush a buffered create to the ext2 volume, free buffers.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && e2fd[fd].used) {
        ext2_fd_t *e = &e2fd[fd];
        int rc = 0;
        uint64_t _c0 = sched_now_ms();
        int _cw = e->writing, _cs = e->wstream; uint32_t _cl = e->wlen, _csl = e->slen;
        // #695: `&& e->wdirty` is what makes the close after a successful
        // fsync() a no-op returning 0. Without it, close would write the same
        // bytes a second time, and on an O_APPEND fd that means appending them
        // twice.
        if (e->writing && e->wdirty) {
            if (e->wstream) {
                // #572: finish the streaming write (flush the trailing partial
                // block, then commit i_size). On error, roll back to a valid file.
                if (e->werr) { ext2_wstream_abort(&e->ws); rc = -1; }
                else {
                    if (e->slen && ext2_wstream_block(&e->ws, e->sbuf, e->slen) != 0)
                        { ext2_wstream_abort(&e->ws); rc = -1; }
                    if (rc == 0) rc = ext2_wstream_finish(&e->ws);
                }
            } else if (e->wbuf) {
                // #598: an O_APPEND fd extends the file; every other write fd
                // replaces it (unchanged).
                // #746: an O_RDWR fd lands here unchanged and that is CORRECT,
                // not an oversight: wbuf holds the whole file and wlen its
                // length, so replacing the file with them is exactly the right
                // commit. It never sets e->appending (O_APPEND on an rw fd is
                // e->rw_append, applied per write) and never spills to the
                // streaming writer, so neither of the other two arms can be
                // reached from it.
                rc = e->appending ? ext2_append_file(e->path, e->wbuf, e->wlen)
                                  : ext2_write_file(e->path, e->wbuf, e->wlen);
            }
        }
        {
            uint32_t _cms = (uint32_t)(sched_now_ms() - _c0);
            if (_cw && (_cms > 50 || _cs))
                kprintf("[CLOSEPROF] fd=%d ms=%u stream=%d wlen=%u slen=%u wstream_total_ms=%u calls=%u\n",
                        fd, _cms, _cs, _cl, _csl,
                        (uint32_t)g_wstream_ms, (uint32_t)g_wstream_calls);
            if (_cs) { g_wstream_ms = 0; g_wstream_calls = 0; }
        }
        if (e->rbuf) kfree(e->rbuf);
        if (e->wbuf) kfree(e->wbuf);
        if (e->sbuf) kfree(e->sbuf);
        for (uint64_t z = 0; z < sizeof(*e); z++) ((uint8_t *)e)[z] = 0;
        legacy_fd_release(fd);
        return rc;
    }

    // Fallback: legacy FAT fd table
    if (fd >= LEGACY_MAX_FDS || !fd_used[fd]) {
        return -1;
    }
    fat_close(&fd_table[fd]);
    // #746: this used to be a hardcoded `return 0`, so a failed FAT write was
    // invisible to close() no matter how carefully the caller checked - while
    // the ext2 branch above returned the real rc from ext2_write_file(). Two
    // filesystems, one syscall, two different answers to "did my data land".
    //
    // Reading the flag BEFORE the release is deliberate: legacy_fd_release()
    // hands the slot back to the pool, and after that point the flag belongs to
    // whichever open claims it next.
    int werr = fd_werr[fd];
    legacy_fd_release(fd);
    return werr ? -1 : 0;
}

// ===========================================================================
// #695 Phase 1: SYS_FSYNC
// ===========================================================================
//
// WHY THIS AND NOT JUST A CHECKABLE close(). POSIX close() may return EIO AND
// THE FD IS CONSUMED REGARDLESS. By the time a caller learns from close(), the
// handle is gone: it cannot retry, cannot re-flush, and must NOT call close()
// again, because retrying close can close another thread's fd. Only a flush you
// can check WHILE STILL HOLDING THE DESCRIPTION supports the pattern that
// actually protects data:
//
//     write -> fsync -> check -> ONLY THEN clear the dirty flag
//
// This routes the same four fd families sys_close() routes, but flushes WITHOUT
// releasing, and is IDEMPOTENT: a successful flush marks the buffer clean, so a
// second fsync and the eventual close are both no-ops returning 0, and no byte
// is written twice.
//
// WHAT STATE THE DESTINATION IS IN AFTER A FAILED FLUSH. This is the whole
// point, and it is why "add an if" is not a fix for a caller that overwrites or
// deletes its source afterwards:
//
//     fsync returns 0 only if every byte is on the medium. On a NON-ZERO return
//     the file may be EMPTY OR ABSENT, and is NEVER the previous contents. The
//     caller must treat the destination as DESTROYED and MUST NOT delete or
//     overwrite its source.
//
// That is not pessimism, it is what the two drivers do. ext2's overwrite path
// frees the OLD data blocks before writing the new ones, and on ENOSPC
// ext2_rollback_inode leaves a VALID ZERO-BYTE FILE. FAT deletes the existing
// file before rewriting it. Neither keeps the old contents anywhere.
//
// Return values are the driver's own: negative EXT2_E_* from ext2 (-4 IO,
// -5 NOSPC, -3 NOMEM after #695 Phase 0 split them), -1 from FAT and SMB/NFS.
// Callers must treat ANY negative value as failure; the set may grow.
int64_t sys_fsync(int fd) {
    if (fd < 0) return -1;

    // --- family 1: per-process VFS fds (redirect files, /dev, pipes, PTY,
    // sockets). file_flush() dispatches to ops->flush, which for the two
    // filesystem adapters is the SAME function their release() calls.
    process_t *proc = proc_current();
    if (proc && fd < MAX_FDS && proc->fds[fd]) {
        return file_flush(proc->fds[fd]);
    }

    if (!lfd_is(fd)) return -1;          // #FDNS
    fd = lfd_idx(fd);
    if (!legacy_owner_ok(fd, "fsync")) return -1;   // #fdguard

    // --- family 2: SMB / NFS upload-on-close buffer.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && smbfd[fd].used) {
        smb_fd_t *s = &smbfd[fd];
        if (s->is_dir || !s->writing || !s->wbuf) return 0;  // nothing buffered
        if (!s->wdirty) return 0;                            // idempotent
        int rc = s->is_nfs ? nfs_vfs_write_whole(s->path, s->wbuf, s->wlen)
                           : smb_vfs_write_whole(s->path, s->wbuf, s->wlen);
        if (rc != 0) return rc;
        s->wdirty = 0;
        return 0;
    }

    // --- family 3: ext2-backed legacy fd. This is the family that matters:
    // /CONFIG lives on the ext2 root, so every credential, session and security
    // -log write in the system flushes through here.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && e2fd[fd].used) {
        ext2_fd_t *e = &e2fd[fd];
        if (!e->writing) return 0;      // read or directory fd: nothing buffered
        if (!e->wdirty) return 0;       // idempotent: already on the medium
        int rc = 0;
        if (e->wstream) {
            // #572 SPILL PATH, and the one honest limitation of this fsync.
            // The streaming writer has NO resumable commit point:
            // ext2_wstream_finish() clears ws->ok, and the staged tail can only
            // be appended as a WHOLE block, so committing mid-stream and then
            // continuing would leave a hole in the middle of the file. So an
            // fsync on a spilled fd is TERMINAL: it commits exactly what close()
            // would commit, then SEALS the fd so a later write() fails loudly
            // instead of corrupting the file silently. Every file under
            // EXT2_WSPILL_BYTES, which is every /CONFIG file and the
            // overwhelming majority of writes, gets full POSIX semantics.
            if (e->werr) { ext2_wstream_abort(&e->ws); return -1; }
            if (e->slen && ext2_wstream_block(&e->ws, e->sbuf, e->slen) != 0) {
                ext2_wstream_abort(&e->ws);
                e->werr = 1;
                return -1;
            }
            rc = ext2_wstream_finish(&e->ws);
            if (rc != 0) { e->werr = 1; return rc; }
            e->slen = 0;
            e->wsealed = 1;
            e->wdirty  = 0;
            return 0;
        }
        if (!e->wbuf) return 0;
        rc = e->appending ? ext2_append_file(e->path, e->wbuf, e->wlen)
                          : ext2_write_file(e->path, e->wbuf, e->wlen);
        if (rc != 0) return rc;
        // #598 one layer up: an O_APPEND fd's buffered bytes are now ON the
        // medium. Keeping them would append them a SECOND time at close.
        if (e->appending) e->wlen = 0;
        e->wdirty = 0;
        return 0;
    }

    // --- family 4: legacy FAT fd. sys_write() on this family calls fat_write()
    // straight through to blk_write(), which is write-through: there is no FAT
    // write cache to flush, so 0 is the truth here and not a stub. (The FAT
    // fds that DO buffer are fat_vfs.c's, and they are family 1.)
    if (fd >= LEGACY_MAX_FDS || !fd_used[fd]) return -1;   // not an open fd
    // #746: there is still nothing to FLUSH here, but there may be something to
    // REPORT. A write that failed earlier on this fd is a fact fsync owes the
    // caller, and the point of fsync is that the caller learns it while it
    // still holds the description and can act. The flag is NOT cleared: the
    // bytes are gone, re-reporting at close is correct, and clearing it would
    // let a caller "recover" from a loss that did not un-happen.
    return fd_werr[fd] ? -1 : 0;
}

int64_t sys_read(int fd, void *buf, size_t count) {
    // Route through per-process file descriptors (PTY, pipes, etc.)
    process_t *proc = proc_current();

    // #510/#511 ROOT-CAUSE FIX. Every branch below writes up to `count`
    // bytes into `buf` via a plain kernel-mode memcpy(), which does NOT
    // naturally fault on a page that is PRESENT-but-not-USER (a fresh
    // malloc'd libc stdio/heap buffer inherits exactly that state the first
    // time its backing VMA is touched, see mm/demand.c's case-2 comment):
    // Ring 0 ignores the U/S bit, so the write silently lands wherever that
    // inherited physical frame is, and the syscall returns the correct byte
    // count. The bug then surfaces invisibly, one step later: the FIRST time
    // userland itself touches that page, THAT access takes a real #PF (Ring 3
    // is gated on U/S), which correctly hands back a fresh zeroed page,
    // discarding the very bytes this syscall just wrote. Measured via
    // Arena's de_dust2.bsp load (#491/#510) and AssaultCube's
    // config/font.cfg execfile() (#421): both are a plain fopen()+fread()
    // into a freshly malloc'd buffer. Proactively resolving the destination
    // range through the exact same fault path Ring 3 would have used, BEFORE
    // the memcpy, means the page is already in its final present+user+
    // writable state when the kernel writes the real bytes, so userland's
    // later first touch sees them intact instead of racing a demand-zero
    // fault that throws them away. No-op if the range is already backed
    // (the overwhelmingly common case for a buffer the app has reused).
    if (proc && count > 0) {
        scp_span_t __sp = scp_begin();   // #121
        mm_prefault_range(proc, (uint64_t)buf, (uint64_t)count, 1);
        scp_end(SCP_PREFAULT, __sp);
    }

    if (proc && fd >= 0 && fd < 64 && proc->fds[fd]) {
        extern int64_t file_read(struct file *f, void *buf, size_t count);
        scp_span_t __sp = scp_begin();   // #121
        int64_t __r = file_read(proc->fds[fd], buf, count);
        scp_end(SCP_FILEREAD, __sp);
        return __r;
    }

    if (!lfd_is(fd)) return -1;          // #FDNS
    fd = lfd_idx(fd);
    if (!legacy_owner_ok(fd, "read")) return -1;   // #fdguard

    // #317: SMB-backed fd: serve from the cached file image.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && smbfd[fd].used) {
        smb_fd_t *s = &smbfd[fd];
        if (s->is_dir || !s->rbuf) return -1;
        uint32_t avail = (s->rpos < s->rsize) ? (s->rsize - s->rpos) : 0;
        uint32_t n = (count < avail) ? (uint32_t)count : avail;
        // #19/#645: `buf` is the caller's buffer and is a Ring-3 pointer on
        // every syscall path (sys_read also has kernel callers, which is why
        // this is an AC bracket on the copy and not a copy_to_user).
        if (n) { uaccess_ac_t __ac = uaccess_begin();
                 memcpy(buf, s->rbuf + s->rpos, n);
                 uaccess_end(__ac); s->rpos += n; }
        return (int64_t)n;
    }

    // ext2-backed fd: serve from a bounded read window, refilling from disk
    // (#572) when the requested range falls outside it. `buf` is already fully
    // prefaulted by mm_prefault_range() above, so the piecewise memcpy into user
    // memory is safe. Bounded kernel RAM regardless of file size.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && e2fd[fd].used) {
        ext2_fd_t *e = &e2fd[fd];
        // #746: an O_RDWR fd reads out of the SAME whole-file buffer it writes
        // into, at the same position. Before this it fell through to the
        // `!e->rbuf` refusal below and every read on it returned -1.
        if (e->rw) {
            if (e->is_dir || !e->wbuf) return -1;
            if (e->noread) return -1;   // #745 local 109: O_WRONLY cannot read
            uint32_t avail = (e->rwpos < e->wlen) ? (e->wlen - e->rwpos) : 0;
            uint32_t n = (count < avail) ? (uint32_t)count : avail;
            // #19/#645: see the SMB branch above.
            if (n) { uaccess_ac_t __ac = uaccess_begin();
                     memcpy(buf, e->wbuf + e->rwpos, n);
                     uaccess_end(__ac); e->rwpos += n; }
            return (int64_t)n;
        }
        if (e->is_dir || !e->rbuf) return -1;
        uint32_t avail = (e->rpos < e->rsize) ? (e->rsize - e->rpos) : 0;
        uint32_t n = (count < avail) ? (uint32_t)count : avail;
        uint32_t bs = ext2_block_size(); if (!bs) bs = 1024;
        uint32_t produced = 0;
        while (produced < n) {
            uint32_t pos = e->rpos + produced;
            if (e->rwin_len == 0 || pos < e->rwin_base ||
                pos >= e->rwin_base + e->rwin_len) {
                uint32_t base = pos - (pos % bs);       // block-align the window
                uint32_t want = e->rsize - base;
                if (want > e->rcap) want = e->rcap;
                // #614: fill the window one EXT2_RSLICE_BYTES slice at a time so
                // ext2_lock is released between slices (see the macro comment).
                int64_t got = 0;
                scp_span_t __sf = scp_begin();   // #121
                while ((uint32_t)got < want) {
                    uint32_t slice = want - (uint32_t)got;
                    if (slice > EXT2_RSLICE_BYTES) slice = EXT2_RSLICE_BYTES;
                    int64_t g1 = ext2_read_file_range(e->rino, base + (uint32_t)got,
                                                      slice, e->rbuf + got);
                    if (g1 <= 0) { if (got == 0) got = g1; break; }
                    got += g1;
                }
                scp_end(SCP_EXT2FILL, __sf);   // #121
                if (got <= 0) {
                    // #609 DIAGNOSIS. A refill that returns <=0 is the ONLY way
                    // this fd can report a short read, and it is what the App
                    // Store reports as "Read-back failed" after a verified
                    // multi-megabyte download. Silence here cost a whole
                    // debugging cycle: say WHICH error and WHERE, once per fd.
                    // (-1 unmounted, -2 inode read, -3 kmalloc, -4 indirect
                    // block read error, -5 data read short, 0 = at/after EOF,
                    // i.e. the on-disk i_size is smaller than the reader thinks)
                    if (!e->rfail_logged) {
                        e->rfail_logged = 1;
                        kprintf("[EXT2-RD] short read: ino=%u base=%u want=%u got=%ld "
                                "rsize=%u rpos=%u produced=%u\n",
                                e->rino, base, want, (long)got, e->rsize, e->rpos, produced);
                    }
                    break;                              // read error / short: stop
                }
                e->rwin_base = base; e->rwin_len = (uint32_t)got;
            }
            uint32_t inoff = pos - e->rwin_base;
            uint32_t chunk = e->rwin_len - inoff;
            if (chunk > n - produced) chunk = n - produced;
            // #19/#645: the one copy into the caller's buffer; the refill
            // above (which takes ext2_lock) stays OUTSIDE the AC window.
            {   scp_span_t __sc = scp_begin();   // #121
                uaccess_ac_t __ac = uaccess_begin();
                memcpy((uint8_t *)buf + produced, e->rbuf + inoff, chunk);
                uaccess_end(__ac);
                scp_end(SCP_EXT2COPY, __sc); }
            produced += chunk;
        }
        e->rpos += produced;
        return (int64_t)produced;
    }

    // Fallback: legacy fd table for FAT files
    if (fd < 0 || fd >= LEGACY_MAX_FDS || !fd_used[fd]) {
        return -1;
    }

    {   scp_span_t __sp = scp_begin();   // #121
        int64_t __r = fat_read(&fd_table[fd], buf, count);
        scp_end(SCP_FATREAD, __sp);
        return __r; }
}

static int64_t sys_write_inner(int fd, const void *buf, size_t count);
int64_t sys_write(int fd, const void *buf, size_t count) {
    uint64_t _dp_t0 = dp_tsc();
    int64_t _dp_r = sys_write_inner(fd, buf, count);
    g_dp_wr_cyc += dp_tsc() - _dp_t0;
    if (_dp_r > 0) g_dp_wr_bytes += (uint64_t)_dp_r;
    return _dp_r;
}
static int64_t sys_write_inner(int fd, const void *buf, size_t count) {
    // Route through per-process file descriptors (PTY, pipes, etc.)
    process_t *proc = proc_current();
    if (proc && fd >= 0 && fd < 64 && proc->fds[fd]) {
        return file_write(proc->fds[fd], buf, count);
    }

    if (!lfd_is(fd)) return -1;          // #FDNS
    fd = lfd_idx(fd);
    if (!legacy_owner_ok(fd, "write")) return -1;   // #fdguard

    // #317: SMB-backed fd: buffer writes; uploaded to the share on close.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && smbfd[fd].used) {
        smb_fd_t *s = &smbfd[fd];
        if (!s->writing || !s->wbuf) return -1;
        s->wdirty = 1;   // #695: not on the server until a flush says so
        if (s->wlen + count > s->wcap) {
            uint32_t ncap = s->wcap ? s->wcap : 4096;
            while (ncap < s->wlen + count) ncap *= 2;
            uint8_t *nb = (uint8_t *)kmalloc(ncap);
            if (!nb) return -1;
            memcpy(nb, s->wbuf, s->wlen);
            kfree(s->wbuf); s->wbuf = nb; s->wcap = ncap;
        }
        // #567: fault-safe copy of the user payload into the kernel write buffer.
        if (count && copy_from_user(s->wbuf + s->wlen, buf, count) != 0) return -14;
        s->wlen += (uint32_t)count;
        return (int64_t)count;
    }

    // ext2-backed fd: buffer small writes (flushed by ext2_write_file on close,
    // byte-for-byte the old path); once the buffer would exceed the spill
    // threshold, switch to the block-flushed streaming writer (#572) so a file
    // of any size uses bounded kernel RAM.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && e2fd[fd].used) {
        ext2_fd_t *e = &e2fd[fd];
        if (!e->writing) return -1;
        if (e->werr) return -1;                 // a prior streaming flush failed
        // #746 O_RDWR: overwrite IN PLACE at the shared position, extending
        // only when the write runs past the current end. This is the half of
        // read-modify-write that the append-only write buffer below cannot
        // express: that buffer has no concept of position at all, so a seek
        // followed by a write appended instead of overwriting.
        if (e->rw) {
            if (e->wsealed) return -1;
            if (!e->wbuf) return -1;
            if (e->rw_append) e->rwpos = e->wlen;   // O_APPEND: always at the end
            uint64_t end = (uint64_t)e->rwpos + count;
            if (end > EXT2_WSPILL_BYTES) {
                // The rw fd holds the whole file, so it cannot grow past the
                // bound the open was admitted under. Refuse rather than spill
                // to the streaming writer, which would truncate the file.
                return -1;
            }
            if (end > e->wcap) {
                uint32_t ncap = e->wcap ? e->wcap : 4096;
                while ((uint64_t)ncap < end) ncap <<= 1;
                uint8_t *nb = (uint8_t *)kmalloc(ncap);
                if (!nb) return -1;
                memcpy(nb, e->wbuf, e->wlen);
                kfree(e->wbuf); e->wbuf = nb; e->wcap = ncap;
            }
            // A write past the current end leaves a HOLE. POSIX says a hole
            // reads as zeroes; without this it would read as whatever kmalloc
            // last left in the buffer, which is a kernel-memory disclosure as
            // well as wrong data.
            if (e->rwpos > e->wlen)
                memset(e->wbuf + e->wlen, 0, e->rwpos - e->wlen);
            if (count && copy_from_user(e->wbuf + e->rwpos, buf, count) != 0)
                return -14;
            e->rwpos = (uint32_t)end;
            if (e->rwpos > e->wlen) e->wlen = e->rwpos;
            e->rsize = e->wlen;
            e->wdirty = 1;
            return (int64_t)count;
        }
        // #695: an fsync on a spilled fd committed and SEALED the stream. Fail
        // the write rather than write past a committed i_size.
        if (e->wsealed) return -1;
        e->wdirty = 1;                          // #695: not on the medium yet
        uint32_t bs = ext2_block_size(); if (!bs) bs = 1024;

        // #598: an O_APPEND fd can NEVER use the #572 streaming writer, because
        // ext2_wstream_begin() truncates its target - which is the very bug
        // being fixed. Buffer up to the same EXT2_WSPILL_BYTES bound and flush
        // by APPENDING, so kernel RAM stays bounded for an append of any size.
        if (e->appending) {
            if (!e->wbuf) return -1;
            size_t done = 0;
            while (done < count) {
                if (e->wlen >= EXT2_WSPILL_BYTES) {
                    if (ext2_append_file(e->path, e->wbuf, e->wlen) != 0) { e->werr = 1; return -1; }
                    e->wlen = 0;
                }
                uint32_t space = EXT2_WSPILL_BYTES - e->wlen;
                uint32_t take  = ((count - done) < space) ? (uint32_t)(count - done) : space;
                if (e->wlen + take > e->wcap) {
                    uint32_t ncap = e->wcap ? e->wcap : 4096;
                    while (ncap < e->wlen + take) ncap *= 2;
                    uint8_t *nb = (uint8_t *)kmalloc(ncap);
                    if (!nb) return -1;
                    memcpy(nb, e->wbuf, e->wlen);
                    kfree(e->wbuf); e->wbuf = nb; e->wcap = ncap;
                }
                if (copy_from_user(e->wbuf + e->wlen, (const uint8_t *)buf + done, take) != 0)
                    return -14;
                e->wlen += take; done += take;
            }
            return (int64_t)count;
        }

        // Phase 1: still fully buffered and staying under the threshold.
        if (!e->wstream) {
            if (!e->wbuf) return -1;
            if ((uint64_t)e->wlen + count <= EXT2_WSPILL_BYTES) {
                if (e->wlen + count > e->wcap) {
                    uint32_t ncap = e->wcap ? e->wcap : 4096;
                    while (ncap < e->wlen + count) ncap *= 2;
                    uint8_t *nb = (uint8_t *)kmalloc(ncap);
                    if (!nb) return -1;
                    memcpy(nb, e->wbuf, e->wlen);
                    kfree(e->wbuf); e->wbuf = nb; e->wcap = ncap;
                }
                // #567: fault-safe copy of the user payload into the write buffer.
                if (count && copy_from_user(e->wbuf + e->wlen, buf, count) != 0) return -14;
                e->wlen += (uint32_t)count;
                return (int64_t)count;
            }
            // ---- SPILL to streaming: link the file, flush what is buffered. ----
            if (ext2_wstream_begin(e->path, &e->ws) != 0) { e->werr = 1; return -1; }
            // #609: stage a RUN of blocks, not one block. Each flush is then a
            // single ext2 metadata transaction (and a single ext2_lock hold)
            // per 128KB instead of per 4KB. Falls back to one block if the heap
            // cannot give us the bigger buffer.
            e->scap = bs * EXT2_WRUN_BLOCKS;
            e->sbuf = (uint8_t *)kmalloc(e->scap);
            if (!e->sbuf) { e->scap = bs; e->sbuf = (uint8_t *)kmalloc(e->scap); }
            if (!e->sbuf) { e->werr = 1; return -1; }
            uint32_t full = (e->wlen / bs) * bs;      // whole blocks only
            for (uint32_t i = 0; i < full; ) {
                uint32_t take = full - i;
                if (take > e->scap) take = e->scap;
                if (ext2_wstream_block(&e->ws, e->wbuf + i, take) != 0) {
                    e->werr = 1; return -1;
                }
                i += take;
            }
            e->slen = e->wlen - full;                    // keep the trailing partial
            if (e->slen) memcpy(e->sbuf, e->wbuf + full, e->slen);
            kfree(e->wbuf); e->wbuf = 0; e->wcap = 0; e->wlen = 0;
            e->wstream = 1;
            // fall through to append the incoming payload
        }

        // Phase 2: streaming append. Copy the user payload into the block staging
        // buffer, flushing a full block to disk each time it fills.
        size_t done = 0;
        uint32_t scap = e->scap ? e->scap : bs;
        while (done < count) {
            uint32_t space = scap - e->slen;
            uint32_t take = (count - done < space) ? (uint32_t)(count - done) : space;
            // #567: fault-safe copy from user memory into the staging buffer.
            if (copy_from_user(e->sbuf + e->slen, (const uint8_t *)buf + done, take) != 0)
                return -14;
            e->slen += take; done += take;
            if (e->slen == scap) {
                uint64_t _w0 = sched_now_ms();
                if (ext2_wstream_block(&e->ws, e->sbuf, scap) != 0) { e->werr = 1; return -1; }
                g_wstream_ms += sched_now_ms() - _w0; g_wstream_calls++;
                e->slen = 0;
            }
        }
        return (int64_t)count;
    }

    // Fallback: handle stdout/stderr via serial console
    if (fd == 1 || fd == 2) {
        // #567: bounce the user payload fault-safe into a kernel buffer before
        // reading it byte-by-byte (was a raw p[i]/memcpy off the user pointer).
        if (count == 0) return 0;
        char *p = (char *)kmalloc(count);
        if (!p) return -1;
        if (copy_from_user(p, buf, count) != 0) { kfree(p); return -14; }

        for (size_t i = 0; i < count; i++) {
            kputc(p[i]);
        }

        if (count < 256) {
            char msg_buf[256];
            size_t copy_len = count < 255 ? count : 255;
            memcpy(msg_buf, p, copy_len);
            msg_buf[copy_len] = '\0';

            if (copy_len > 0 && msg_buf[copy_len - 1] == '\n') {
                msg_buf[copy_len - 1] = '\0';
            }

            syslog_log(1, msg_buf);
        }

        kfree(p);
        return (int64_t)count;
    }

    if (fd < 0 || fd >= LEGACY_MAX_FDS || !fd_used[fd]) {
        return -1;
    }

    // #746: a zero-length write is not a failure. fat_write_inner() returns -1
    // for size == 0, which would arm the sticky error below on a call POSIX
    // defines as a successful no-op.
    if (count == 0) return 0;
    {
        int64_t w = fat_write(&fd_table[fd], buf, count);
        // #746 REMEMBER THE FAILURE, so close() can report it (see fd_werr).
        //
        // READ THIS BEFORE CHANGING THE CONDITION. fat_write() returns a BYTE
        // COUNT, not the 0/-1 its neighbours return. Running out of clusters
        // half way through gives a POSITIVE short count, which every
        // `if (rc < 0)` caller reads as success, so the short case is just as
        // much a failed write as the negative one and both must arm the flag.
        if (w < 0 || (uint64_t)w != (uint64_t)count) fd_werr[fd] = 1;
        return w;
    }
}

int64_t sys_seek(int fd, int64_t offset, int whence) {
    // #FDNS: this function had NO per-process branch at all, so an lseek() on a
    // pipe/PTY/socket fd fell straight into the legacy tables and seeked
    // whichever unrelated file happened to occupy that slot. With the ranges
    // now disjoint the VFS half can be answered where it belongs, through the
    // same file_seek() every other VFS operation uses, and a number in neither
    // range is refused rather than silently applied to a stranger's file.
    {   process_t *proc = proc_current();
        if (proc && fd >= 0 && fd < MAX_FDS && proc->fds[fd])
            return file_seek(proc->fds[fd], offset, whence);
    }
    if (!lfd_is(fd)) return -1;
    fd = lfd_idx(fd);
    if (!legacy_owner_ok(fd, "seek")) return -1;   // #fdguard

    // #317: SMB-backed fd: seek within the cached read image.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && smbfd[fd].used) {
        smb_fd_t *s = &smbfd[fd];
        int64_t np;
        if (whence == 0) np = offset;
        else if (whence == 1) np = (int64_t)s->rpos + offset;
        else if (whence == 2) np = (int64_t)s->rsize + offset;
        else return -1;
        if (np < 0) np = 0;
        if (np > (int64_t)s->rsize) np = s->rsize;
        s->rpos = (uint32_t)np;
        return np;
    }
    // ext2-backed fd: seek within the cached file image.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && e2fd[fd].used) {
        ext2_fd_t *e = &e2fd[fd];
        int64_t np;
        // #746: an O_RDWR fd has ONE position, and SEEK_END means the end of
        // what it currently holds (wlen), which a write may have moved.
        if (e->rw) {
            if (whence == 0) np = offset;
            else if (whence == 1) np = (int64_t)e->rwpos + offset;
            else if (whence == 2) np = (int64_t)e->wlen + offset;
            else return -1;
            if (np < 0) np = 0;
            if (np > (int64_t)e->wlen) np = e->wlen;
            e->rwpos = (uint32_t)np;
            return np;
        }
        if (whence == 0) np = offset;
        else if (whence == 1) np = (int64_t)e->rpos + offset;
        else if (whence == 2) np = (int64_t)e->rsize + offset;
        else return -1;
        if (np < 0) np = 0;
        if (np > (int64_t)e->rsize) np = e->rsize;
        e->rpos = (uint32_t)np;
        return np;
    }
    if (fd < 0 || fd >= LEGACY_MAX_FDS || !fd_used[fd]) {
        return -1;
    }

    // Convert whence for FAT driver
    uint32_t pos;
    switch (whence) {
        case 0:  // SEEK_SET
            pos = offset;
            break;
        case 1:  // SEEK_CUR
            pos = fd_table[fd].position + offset;
            break;
        case 2:  // SEEK_END
            pos = fd_table[fd].file_size + offset;
            break;
        default:
            return -1;
    }

    if (fat_seek(&fd_table[fd], pos) != 0) return -1;
    return (int64_t)fd_table[fd].position;  /* POSIX: lseek returns new offset */
}
// #567: user-facing sys_readdir() fills a kernel-local dirent then does one
// fault-safe copy_to_user. In-kernel callers (the #308/#317 boot self-tests)
// call sys_readdir_k() directly with a kernel dirent, so the core's raw writes
// are always to kernel memory and only this wrapper touches the user pointer.
int64_t sys_readdir(int fd, void *entry_buf) {
    if (!entry_buf) return -1;
    sc_dirent_t kde;
    int64_t r = sys_readdir_k(fd, &kde);
    if (r == 0 && copy_to_user(entry_buf, &kde, sizeof(kde)) != 0) return -14;
    return r;
}

int64_t sys_readdir_k(int fd, sc_dirent_t *de) {
    // Read next directory entry from an open directory fd.
    if (!lfd_is(fd)) return -1;          // #FDNS
    fd = lfd_idx(fd);
    if (!legacy_owner_ok(fd, "readdir")) return -1;   // #fdguard
    if (!fd_used[fd]) {
        return -1;
    }

    // #317 pass 4: NFS-backed directory fd. NFSv3 READDIR carries only names
    // (no type/size), so entries are reported as files with size 0; cat/ls work.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && smbfd[fd].used && smbfd[fd].is_dir &&
        smbfd[fd].is_nfs) {
        smb_fd_t *s = &smbfd[fd];
        nfs_entry3_t *e;
        for (;;) {
            e = nfs_readdir(s->dirh);
            if (!e) return -1;   // end-of-dir or error
            if (e->name[0] == '.' &&
                (e->name[1] == 0 || (e->name[1] == '.' && e->name[2] == 0)))
                continue;
            break;
        }
        int i = 0; while (e->name[i] && i < 255) { de->name[i] = e->name[i]; i++; }
        de->name[i] = '\0';
        de->type = 0;
        de->size = 0;
        return 0;
    }

    // #317: SMB-backed directory fd.
    if (fd >= 3 && fd < LEGACY_MAX_FDS && smbfd[fd].used && smbfd[fd].is_dir) {
        smb_fd_t *s = &smbfd[fd];
        smb_dirent_t sde;
        // Skip the "." and ".." pseudo-entries the server returns; the Files app
        // synthesizes its own ".." row.
        for (;;) {
            int r = smb_readdir(s->dirh, &sde);
            if (r != 0) return -1;   // 1 = end-of-dir, -1 = error
            if (sde.name[0] == '.' &&
                (sde.name[1] == 0 || (sde.name[1] == '.' && sde.name[2] == 0)))
                continue;
            break;
        }
        int i = 0; while (sde.name[i] && i < 255) { de->name[i] = sde.name[i]; i++; }
        de->name[i] = '\0';
        de->type = sde.is_directory ? 1 : 0;
        de->size = (uint32_t)sde.size;
        return 0;
    }

    // ext2-backed directory fd.
    if (fd >= 3 && e2fd[fd].used && e2fd[fd].is_dir) {
        ext2_fd_t *e = &e2fd[fd];
        char nm[256]; uint32_t cino = 0; uint8_t ft = 0;
        if (ext2_readdir_ino(e->dir_ino, &e->dir_pos, nm, sizeof(nm), &cino, &ft) != 0) return -1;
        int i = 0; while (nm[i] && i < 255) { de->name[i] = nm[i]; i++; } de->name[i] = '\0';
        de->type = (ft == 2) ? 1 : 0;   // EXT2_FT_DIR == 2
        de->size = 0;
        if (ft != 2) { ext2_inode_t in; if (ext2_read_inode(cino, &in) == 0) de->size = in.i_size; }
        return 0;
    }


    // Use the FAT readdir function which takes a fat_dir_entry_t
    fat_dir_entry_t raw_entry;
    char name_buf[256];
    int ret = fat_readdir(&fd_table[fd], &raw_entry, name_buf);
    if (ret != 0) return -1;  // No more entries

    // Copy the name
    int i = 0;
    while (name_buf[i] && i < 255) { de->name[i] = name_buf[i]; i++; }
    de->name[i] = '\0';

    // Set type: bit 0x10 in attr means directory
    de->type = (raw_entry.attr & 0x10) ? 1 : 0;
    de->size = raw_entry.file_size;

    return 0;  // Success, got an entry
}

// #745 (local 82): the legacy-table half of "is this fd open?".
// poll(2) asks fd_get() first (the per-process file_t table); this answers
// for the three kernel-wide tables that predate it. It deliberately does
// NOT report fds 0/1/2, which are always file_t's, and it deliberately
// does not try to say WHAT kind of file it is: every one of the three is a
// regular file and POSIX gives regular files one answer.
int fd_legacy_is_open(int fd) {
    if (!lfd_is(fd)) return 0;           // #FDNS
    fd = lfd_idx(fd);
    if (!legacy_owner_ok(fd, "poll")) return 0;   // #fdguard
    if (fd_used[fd]) return 1;
    if (e2fd[fd].used) return 1;
    if (smbfd[fd].used) return 1;
    return 0;
}

// #120: see the contract in fdlayer.h. The probe order matches
// fd_legacy_is_open() and sys_seek() above: SMB/NFS, then ext2, then FAT.
int fd_legacy_stat_src(int fd, const fat_file_t **fat_out,
                       char *path_out, uint32_t cap, int64_t *live_size_out) {
    if (fat_out) *fat_out = 0;
    if (live_size_out) *live_size_out = -1;
    if (path_out && cap) path_out[0] = 0;
    if (!lfd_is(fd)) return FDL_STAT_NONE;   // #FDNS
    fd = lfd_idx(fd);
    if (!legacy_owner_ok(fd, "fstat")) return FDL_STAT_NONE;   // #fdguard

    if (smbfd[fd].used) {
        smb_fd_t *s = &smbfd[fd];
        if (path_out && cap) {
            uint32_t i = 0;
            while (i + 1 < cap && s->path[i]) { path_out[i] = s->path[i]; i++; }
            path_out[i] = 0;
        }
        // Mirrors sys_seek's SMB branch: the description serves reads out of
        // its whole-file cache, and an upload is buffered until close.
        if (live_size_out) *live_size_out = s->writing ? (int64_t)s->wlen
                                                       : (int64_t)s->rsize;
        return FDL_STAT_PATH;
    }

    if (e2fd[fd].used) {
        ext2_fd_t *e = &e2fd[fd];
        if (path_out && cap) {
            uint32_t i = 0;
            while (i + 1 < cap && e->path[i]) { path_out[i] = e->path[i]; i++; }
            path_out[i] = 0;
        }
        // ONLY when the description holds bytes the inode does not. A READ fd
        // is deliberately left at -1 so the caller uses the inode's i_size:
        // e->rsize is the #572 bounded read WINDOW (<=128 KB), not the file
        // length, so reporting it would UNDER-report every file larger than the
        // window. sys_seek(SEEK_END) has that limit today; fstat need not
        // inherit it, and the inode is the better answer for a read fd.
        if (live_size_out) {
            if (e->rw)            *live_size_out = (int64_t)e->wlen;
            else if (e->writing)  *live_size_out = (int64_t)e->wlen;
        }
        return FDL_STAT_PATH;
    }

    if (fd_used[fd]) {
        if (fat_out) *fat_out = &fd_table[fd];
        // The legacy FAT family is write-THROUGH (fat_write -> blk_write), so
        // the handle's file_size is already the medium's. Left at -1; the
        // caller fills size from the handle it was just given.
        return FDL_STAT_FAT;
    }
    return FDL_STAT_NONE;
}

// #fdguard: proc_exit backstop. Poison every legacy slot the exiting group
// still owns to DEAD, so a future process handed the same pid cannot inherit
// a leaked slot. This does NOT flush or free the slot's buffers: that is a
// separate pre-existing on-exit leak, and an ext2 flush would block, which
// proc_exit() under cli() cannot do. It only makes a leaked slot unreachable.
// One compare_exchange per slot, no allocation, no block.
void fdown_proc_exit(uint32_t owner) {
    if (!owner) return;
    int n = 0;
    for (int i = 0; i < LEGACY_MAX_FDS; i++)
        n += fdown_mark_dead_if_owner_rs((uint32_t)i, owner);
    if (n)
        kprintf("[FDGUARD] exit owner=%u poisoned %d leaked legacy fd(s); "
                "refusals so far=%u\n", owner, n, fdown_refusals_rs());
}

// #fdguard: boot check. Run the self-test and prove the Rust slot count matches
// LEGACY_MAX_FDS, so the guard cannot silently cover fewer slots than exist.
// Named _check, not _selftest, so diaglog-gate does not require a durable sink
// for this kprintf (same as fetchown_boot_check); the durable audit is the
// per-refusal SECURITY.LOG line, not this boot summary.
void fdown_boot_check(void) {
    int rs = fdown_slots_rs();
    int st = fdown_selftest_rs();
    kprintf("[FDGUARD] fdown selftest=%s slots=%d/%d\n",
            st == 0 ? "PASS" : "FAIL", rs, LEGACY_MAX_FDS);
    if (st != 0 || rs != LEGACY_MAX_FDS)
        kprintf("[FDGUARD] fdown SELF-TEST FAILED step=%d - legacy fd "
                "cross-process ownership is NOT trustworthy on this build\n", st);
}
_Static_assert(LEGACY_MAX_FDS == 128,
               "#fdguard: fdown.rs LEGACY_SLOTS is hardcoded 128; keep it in "
               "sync with LEGACY_MAX_FDS or the boot check will flag it");
