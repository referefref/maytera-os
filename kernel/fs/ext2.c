// ext2.c - Minimal READ-ONLY ext2 filesystem driver for MayteraOS.
//
// Supports: superblock parse, block group descriptor table, inode read,
// directory iteration/lookup, absolute path resolution, and file read via
// direct + singly-indirect + doubly-indirect block pointers. Read-only.
//
// Target image: mke2fs -b 1024 -O ^resize_inode,^dir_index,^ext_attr
//   block size = 1024, inode size = 256, magic 0xEF53, filetype dir entries.

#include "../types.h"
#include "../string.h"
#include "ext2.h"
#include "blockdev.h"   // #307: route sector I/O to ATA or USB MSC root
#include "../sync/spinlock.h"
#include "../proc/process.h"   // #597: proc_current()/sched_set_preemption() for ext2_lock
#include "../sync/waitq.h"     // #609: contenders BLOCK on a wait queue, never spin
#include "../cpu/mono.h"      // #618: real-time clock for the lock-hold profiler
#include "fs/bootlog.h"   // #742: the owning header, NOT a private extern
#include "../security/selftest_registry.h"  // #EXT2SELFTEST: a probe that
                                            // cannot run must SAY so
#include "../cpu/wallclock.h"   // #115: the ONE wall clock + calendar converter

// External kernel services.
extern void  kprintf(const char *fmt, ...);
extern void *kmalloc(unsigned long size);
extern void  kfree(void *ptr);
extern int   ata_read_sectors(uint8_t channel, uint8_t drive, uint32_t lba,
                              uint8_t count, void *buffer);
// Bus-master DMA variants (same speed path FAT uses). Far faster than PIO for
// bulk transfers; fall back to PIO internally if DMA is unavailable. The DMA
// bounce buffer is 64 KB, so a single DMA call must be <= 128 sectors.
extern int   ata_read_sectors_dma(uint8_t channel, uint8_t drive, uint32_t lba,
                                  uint8_t count, void *buffer);
extern int   ata_write_sectors_dma(uint8_t channel, uint8_t drive, uint32_t lba,
                                   uint8_t count, const void *buffer);
#define EXT2_DMA_MAX_SECTORS 128   /* 64 KB DMA bounce-buffer limit */

#define EXT2_MAGIC          0xEF53
#define EXT2_ROOT_INO       2
#define EXT2_SECTOR_SIZE    512
#define EXT2_NDIR_BLOCKS    12
#define EXT2_IND_BLOCK      12      // singly-indirect index in i_block[]
#define EXT2_DIND_BLOCK     13      // doubly-indirect index in i_block[]

// Directory entry file_type values.
#define EXT2_FT_REG_FILE    1
#define EXT2_FT_DIR         2

static ext2_fs_t g_ext2;

// #99 Phase C boot cutover: when non-zero, the kernel treats ext2 as the root
// filesystem. fat_read_file()/fat_write_file() then prefer ext2 for "/" paths
// (with a FAT fallback on read). Set at boot only if the /ROOTEXT2 marker exists
// on the FAT ESP AND the ext2 volume mounted, so it is opt-in and reversible.
int g_root_ext2 = 0;

// ---------------------------------------------------------------------------
// #597 ext2 GLOBAL FILESYSTEM LOCK.
//
// Until now the ONLY lock in this file was g_e2c_lock, which protects the block
// CACHE SLOTS and nothing else. Every metadata mutation here is an unprotected
// read-modify-write that spans SEVERAL blocking disk I/Os: allocate a bitmap bit
// (read bitmap, write bitmap, read+write the group descriptor, read+write the
// superblock), append a block to an inode (read inode, alloc, write indirect,
// write inode), insert a directory entry (read inode, read dir block, write dir
// block, read inode, write inode). The scheduler preempts at 250Hz and
// proc/process.c drops the BKL across every context switch (bkl_release_all()
// around context_switch), so two writers interleave freely inside those spans.
//
// The observable damage: two creates read the SAME stale directory i_size, both
// append at the same logical index, and the second overwrites i_block[n] -> the
// first file's directory block is unlinked and its entry vanishes (orphan inode
// with links=1). Or two allocators hand out the SAME free bitmap bit, so a
// directory block is re-issued as file data and overwritten -> e2fsck
// "Directory inode N ... directory corrupted".
//
// This is exactly the FAT corruption of #150 re-instantiated on ext2; FAT was
// given fat_lock() then, ext2 never was. Same primitive, same reasons: a
// RECURSIVE, owner-tracked lock that YIELDS while contended, so it is safe to
// hold across blocking disk I/O (a spinlock_t is not: it would either spin with
// interrupts off across an ATA transfer or be dropped by the scheduler).
// Recursive because the public entry points nest (ext2_write_file ->
// ext2_lookup, ext2_unlink -> ext2_lookup, ext2_mkdir -> ext2_lookup).
//
// Per-filesystem (not per-inode) is REQUIRED, not laziness: the block bitmap,
// inode bitmap, group descriptor table and superblock free counts are global
// structures touched by every allocation.
static volatile void *g_ext2_lock_owner = 0;
static volatile int   g_ext2_lock_depth = 0;

static inline void *ext2_lock_self(void) {
    void *p = (void *)proc_current();
    return p ? p : (void *)1;   // sentinel for the early-boot single-threaded path
}

// #609 WAITERS MUST SLEEP, NOT SPIN. The #597 version above yielded with
// sched_schedule() and retried, which is the banned hand-rolled poll (#426) and
// is actively harmful HERE: this lock is deliberately held across long runs of
// blocking disk I/O, so every other process that touches any file (the desktop,
// the compositor, syslog) burns its ENTIRE timeslice re-testing an owner word
// that cannot change until a multi-megabyte write finishes. With several such
// processes the box is fully occupied yielding to each other; RIP sampling
// during a large App Store download found CPU0 permanently inside
// sched_schedule(), which is exactly where that retry loop lands. Blocking on a
// wait queue instead means a contender consumes ZERO cpu and is woken by the
// release, so the holder gets the whole machine to finish its I/O.
static wait_queue_head_t g_ext2_wq;
static int g_ext2_wq_ready = 0;

// #617: the ext2 lock is now a FIFO TICKET lock.
//
// It used to be wake-all + re-test, i.e. a BARGING lock: ext2_unlock() woke
// every waiter and they raced for the lock, so whichever thread still HELD THE
// CPU won. During a 103 MB App Store unpack the Ring-3 unpacker is pegged at
// 91-99% and takes/releases this lock thousands of times a second, so it
// re-acquired inside its own time slice every single time, before any woken
// waiter could be scheduled. A waiter was therefore passed over indefinitely.
//
// MEASURED on build 969 (serial [SW] instrumentation, throwaway VM <vmid>, two
// runs): the kernel heartbeat thread - which writes /HEARTBEAT.TXT, and on an
// ext2-root system fat_write_file() routes that to the ext2 volume - sat in
// PROC_STATE_BLOCKED on g_ext2_wq (resolved by symbol from the blocked PCB's
// wait_entry->wq) for the ENTIRE unpack: 45.3 s on run 1 and 47.9 s on run 2,
// consuming zero CPU ticks the whole time. It was BLOCKED, never starved: it
// was not on the ready queue at all.
//
// Tickets make the handoff FIFO. An arriving thread takes the next ticket and
// may not acquire until every earlier ticket has been served, so the unpacker
// cannot barge past a queued waiter no matter how much CPU it has. Recursive
// re-entry by the current owner does NOT take a ticket (it would wait for
// itself); the uncontended path still acquires without sleeping.
//
// WHAT THIS DOES AND DOES NOT FIX - read this before assuming the freeze is
// gone. It removes the barging, and MEASURED on build 971 that turns the
// small-files phase of the unpack from "no heartbeat at all" into a normal 2 s
// cadence at 85-95% APPSTORE. It does NOT fix the reported ~47 s freeze,
// because that is dominated by a SINGLE LONG HOLD, not by barging: across the
// residual 44.6 s gap the contended-acquire counter rose by only 22 (0.5/s)
// while 48,974 block-cache operations completed, versus 300+ contended
// acquires in the preceding 12 s. One caller holds this lock across the whole
// large-file write. Narrowing that hold is the remaining fix and is NOT done
// here.
//
// THE TICKET ISSUE MUST BE ATOMIC, and sched_set_preemption(false) IS NOT
// ENOUGH to make it so. sched_schedule() only honours the preemption flag when
// the current process is still PROC_STATE_RUNNING, but sched_tick() sets
// cur->state = PROC_STATE_READY BEFORE it calls sched_schedule(), so a timer
// tick preempts a "preemption-disabled" section anyway. The old lock survived
// that because its critical section was idempotent (test, retry); issuing a
// ticket is a read-modify-write and is NOT: two threads preempted inside
// `g_ext2_tk_next++` can be handed the SAME ticket and would then both believe
// they own the lock. Use the canonical irqsave spinlock (the same primitive
// fs/blockdev.c's g_blk_lock moved to under #421, for the same reason): with
// IRQs masked there is no tick, so no preemption, so no torn increment. This
// is not theoretical: the first cut used sched_set_preemption and measured a
// WORSE unpack (65.3 s vs 58-59 s baseline); with the atomic issue it is
// 57.0 s, i.e. no throughput cost at all.
static spinlock_t g_ext2_tk_lock = SPINLOCK_INIT;
static uint32_t g_ext2_tk_next = 0;            // next ticket to issue
static uint32_t g_ext2_tk_now  = 0;            // ticket currently entitled to acquire
volatile uint64_t g_ext2_lock_waits = 0;       // contended acquisitions (reported in [HB])

// #618 LOCK-HOLD PROFILER. The freeze was never contention, it was ONE long
// hold; nothing in the tree could name the holder. ext2_lock_at() records the
// public entry point and the wall-clock time of the depth 0->1 acquire, and
// ext2_unlock() prints [EXT2HOLD] for any hold longer than the heartbeat
// cadence. Serial only (a file-write trace would corrupt ext2, #597).
#define EXT2_HOLD_WARN_MS 500u
static const char *g_ext2_hold_who = "?";
static uint64_t     g_ext2_hold_t0 = 0;

static void ext2_lock_at(const char *who) {
    void *me = ext2_lock_self();
    uint32_t my;
    {
        uint64_t fl = spinlock_acquire_irqsave(&g_ext2_tk_lock);
        if (!g_ext2_wq_ready) { wait_queue_head_init(&g_ext2_wq); g_ext2_wq_ready = 1; }
        if (g_ext2_lock_owner == me) {      // recursive: already ours, no ticket
            g_ext2_lock_depth++;            // (taking one would wait for ourselves)
            spinlock_release_irqrestore(&g_ext2_tk_lock, fl);
            return;
        }
        my = g_ext2_tk_next++;
        if (g_ext2_lock_owner == 0 && g_ext2_tk_now == my) {
            g_ext2_lock_owner = me;         // uncontended: our turn already
            g_ext2_lock_depth = 1;
            g_ext2_hold_who = who; g_ext2_hold_t0 = mono_ms();
            spinlock_release_irqrestore(&g_ext2_tk_lock, fl);
            return;
        }
        spinlock_release_irqrestore(&g_ext2_tk_lock, fl);
    }
    g_ext2_lock_waits++;
    for (;;) {
        // A process sleeps on the queue until its ticket comes up. A
        // pre-scheduler / no-process context (early boot mount, boot
        // self-tests) cannot sleep, but it also cannot contend (nothing else is
        // running yet), so that branch is a safety net, as before.
        if (proc_current())
            wait_event(&g_ext2_wq, g_ext2_lock_owner == 0 && g_ext2_tk_now == my);
        else
            sched_schedule();
        uint64_t fl = spinlock_acquire_irqsave(&g_ext2_tk_lock);
        if (g_ext2_lock_owner == 0 && g_ext2_tk_now == my) {
            g_ext2_lock_owner = me;
            g_ext2_lock_depth = 1;
            g_ext2_hold_who = who; g_ext2_hold_t0 = mono_ms();
            spinlock_release_irqrestore(&g_ext2_tk_lock, fl);
            return;
        }
        spinlock_release_irqrestore(&g_ext2_tk_lock, fl);
    }
}

static void ext2_unlock(void) {
    uint64_t fl = spinlock_acquire_irqsave(&g_ext2_tk_lock);
    int released = 0;
    if (g_ext2_lock_depth > 0 && --g_ext2_lock_depth == 0) {
        g_ext2_lock_owner = 0;
        g_ext2_tk_now++;                    // hand the lock to the next in line
        released = 1;
    }
    spinlock_release_irqrestore(&g_ext2_tk_lock, fl);
    if (released) {
        uint64_t held = mono_ms() - g_ext2_hold_t0;
        if (held >= EXT2_HOLD_WARN_MS)
            kprintf("[EXT2HOLD] %s held ext2_lock %ums\n", g_ext2_hold_who, (unsigned)held);
    }
    if (released && g_ext2_wq_ready) wake_up_all(&g_ext2_wq);
}

// Every ext2_lock() call site is a public entry point wrapper, so __func__
// names the operation that is holding the lock.
#define ext2_lock() ext2_lock_at(__func__)

// Little-endian readers (x86 is LE, but use memcpy to avoid alignment issues).
static uint16_t rd16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// --- ext2 block cache -------------------------------------------------------
// Directory blocks, inode-table blocks and indirect-pointer blocks are re-read
// from disk on every path lookup / bmap. A small fully-associative cache of
// recently-read blocks (clock / second-chance eviction) removes those repeat
// disk transfers, which is what made the ext2-root boot slower than FAT (FAT
// had its own sector cache + readahead). Writes update the cache entry in place
// (write-through) so reads always see fresh data. Protected by its own lock
// since ext2 blocks are read concurrently (compositor process + kernel desktop).
#define EXT2_CACHE_SLOTS 192
typedef struct {
    uint32_t block;   // cached block number (meaningful only when valid)
    uint8_t  valid;
    uint8_t  ref;     // clock second-chance bit
    uint8_t *data;    // block_size bytes
} ext2_cache_entry_t;
static ext2_cache_entry_t g_e2c[EXT2_CACHE_SLOTS];
static int       g_e2c_ready = 0;
static uint32_t  g_e2c_clock = 0;
// #444: was `static spinlock_t g_e2c_lock;` + a lazily-run
// `if (!g_e2c_lock_ready) { spinlock_init(&g_e2c_lock); g_e2c_lock_ready = 1; }`
// - the exact same non-atomic check-then-act race as drivers/ata.c's
// g_ata_io_lock (see the #444 comment there). Worse here: the slot-populating
// loop below used to run with NO lock held at all, so two threads racing into
// ext2_cache_init_once() on first use (e.g. CPython opening many stdlib
// modules concurrently at boot) could both re-run the loop, each handing out
// a fresh kmalloc(bs) buffer for the SAME slot index while a third thread was
// concurrently reading/writing that slot's .data pointer under g_e2c_lock in
// ext2_read_block()/ext2_cache_update() - a genuine data race on live cache
// state, and any slot a racing re-init loop reached AFTER a real insert had
// set .valid=1 got silently reset back to .valid=0, invisibly discarding a
// just-cached block. Fixed by statically initializing the lock (no runtime
// init at all) and doing real double-checked locking: the slot-populate loop
// itself now runs under the lock, guarded by a recheck of g_e2c_ready, so it
// can only ever execute once, by exactly one thread, with every other cache
// access properly serialized against it.
static spinlock_t g_e2c_lock = SPINLOCK_INIT;

// #597: write generation. Bumped (under g_e2c_lock) by every write-through, so a
// reader whose disk I/O overlapped a write can tell that the image it just read
// may already be stale and decline to INSTALL it in the cache as valid. Without
// this, ext2_cache_update() only patches an ALREADY-cached slot, so a write to a
// not-currently-cached block that lands between a reader's raw read and its
// insert is silently overwritten by the reader's pre-write image and then served
// as valid=1 forever.
static volatile uint32_t g_e2c_wgen = 0;

static void ext2_cache_init_once(uint32_t bs) {
    if (g_e2c_ready) return;  // fast path once initialized; flag only ever goes 0->1
    uint64_t fl = spinlock_acquire_irqsave(&g_e2c_lock);
    if (!g_e2c_ready) {
        for (int i = 0; i < EXT2_CACHE_SLOTS; i++) {
            g_e2c[i].data  = (uint8_t *)kmalloc(bs);
            g_e2c[i].valid = 0;
            g_e2c[i].ref   = 0;
            g_e2c[i].block = 0;
        }
        g_e2c_ready = 1;
    }
    spinlock_release_irqrestore(&g_e2c_lock, fl);
}

// Raw block read straight from disk (no cache). ata_read_sectors is already
// serialized internally by the global I/O lock.
static int ext2_read_block_raw(const ext2_fs_t *fs, uint32_t block, void *buf) {
    uint32_t sectors_per_block = fs->block_size / EXT2_SECTOR_SIZE;
    uint32_t lba = fs->part_start_lba + block * sectors_per_block;
    int r = blk_read(fs->channel, fs->drive, lba,
                            sectors_per_block, buf);
    if (r != (int)sectors_per_block) {
        return -1;
    }
    return 0;
}

// Read one ext2 block (block_size bytes) into buf, via the block cache.
// Returns 0 on success, <0 on error.
static int ext2_read_block(const ext2_fs_t *fs, uint32_t block, void *buf) {
    uint32_t bs = fs->block_size;
    ext2_cache_init_once(bs);

    // Cache lookup.
    uint64_t fl = spinlock_acquire_irqsave(&g_e2c_lock);
    for (int i = 0; i < EXT2_CACHE_SLOTS; i++) {
        if (g_e2c[i].valid && g_e2c[i].block == block && g_e2c[i].data) {
            memcpy(buf, g_e2c[i].data, bs);
            g_e2c[i].ref = 1;
            spinlock_release_irqrestore(&g_e2c_lock, fl);
            return 0;
        }
    }
    uint32_t gen0 = g_e2c_wgen;   // #597: snapshot before the unlocked I/O
    spinlock_release_irqrestore(&g_e2c_lock, fl);

    // Miss: read from disk (two concurrent misses of the same block just both
    // read it; the result is identical, so no lock is held across the I/O).
    int r = ext2_read_block_raw(fs, block, buf);
    if (r != 0) return r;

    // Insert into the cache, evicting a victim via clock / second-chance.
    fl = spinlock_acquire_irqsave(&g_e2c_lock);
    // #597: if the block became cached while our I/O was in flight (another
    // reader inserted it, or a write-through patched it), that copy is at least
    // as fresh as ours: serve it and do NOT install our image.
    for (int i = 0; i < EXT2_CACHE_SLOTS; i++) {
        if (g_e2c[i].valid && g_e2c[i].block == block && g_e2c[i].data) {
            memcpy(buf, g_e2c[i].data, bs);
            g_e2c[i].ref = 1;
            spinlock_release_irqrestore(&g_e2c_lock, fl);
            return 0;
        }
    }
    // #597: some write completed while we were reading. We cannot tell whether
    // it was THIS block (it was not cached, so ext2_cache_update() left no
    // trace), so the image in `buf` may already be stale. Return it (the read
    // genuinely overlapped the write) but never publish it as a valid cache
    // entry, which would make the staleness permanent.
    if (g_e2c_wgen != gen0) {
        spinlock_release_irqrestore(&g_e2c_lock, fl);
        return 0;
    }
    int victim = -1;
    for (int scan = 0; scan < EXT2_CACHE_SLOTS * 2; scan++) {
        uint32_t i = g_e2c_clock % EXT2_CACHE_SLOTS;
        g_e2c_clock++;
        if (!g_e2c[i].data) continue;          // allocation failed for this slot
        if (!g_e2c[i].valid) { victim = (int)i; break; }
        if (g_e2c[i].ref) { g_e2c[i].ref = 0; continue; }
        victim = (int)i; break;
    }
    if (victim >= 0 && g_e2c[victim].data) {
        memcpy(g_e2c[victim].data, buf, bs);
        g_e2c[victim].block = block;
        g_e2c[victim].valid = 1;
        g_e2c[victim].ref   = 1;
    }
    spinlock_release_irqrestore(&g_e2c_lock, fl);
    return 0;
}

// Keep a cached copy of `block` coherent after a write-through to disk.
static void ext2_cache_update(uint32_t block, const void *buf, uint32_t bs) {
    if (!g_e2c_ready) return;
    uint64_t fl = spinlock_acquire_irqsave(&g_e2c_lock);
    g_e2c_wgen++;               // #597: see the g_e2c_wgen comment above
    for (int i = 0; i < EXT2_CACHE_SLOTS; i++) {
        if (g_e2c[i].valid && g_e2c[i].block == block && g_e2c[i].data) {
            memcpy(g_e2c[i].data, buf, bs);
            g_e2c[i].ref = 1;
            break;
        }
    }
    spinlock_release_irqrestore(&g_e2c_lock, fl);
}

// ---------------------------------------------------------------------------
// Partition-table parse (#404 driver/block tier strangler seam).
//
// The pure byte-walking of ext2_find_partition() is extracted VERBATIM into
// three state-free helpers so it can be (a) mirrored 1:1 by a Rust port and
// (b) differentially self-tested at boot. The blk_read() I/O stays in C below,
// exactly where it was. Every guard, constant, field offset and truncation is
// preserved byte-for-byte; an offline 3-way harness proved the extracted twins
// behave identically to the ORIGINAL function over 40,921 vectors.
//
// This input is UNTRUSTED: LBA 0/1 and the GPT entry array come off whatever
// disk or USB stick is inserted, and the iMac target BOOTS off USB.
typedef struct {
    uint64_t ent_lba;   // PartitionEntryLBA        (hdr +72, u64 LE)
    uint32_t num;       // NumberOfPartitionEntries (hdr +80, u32 LE)
    uint32_t esz;       // SizeOfPartitionEntry     (hdr +84, u32 LE)
    uint32_t per_sec;   // derived: entries per sector
    uint32_t _pad;
} parttbl_gpt_hdr_t;

typedef struct {
    uint32_t consumed;      // entries classified in this sector
    uint32_t found_lin;     // 1 => a Linux-GUID entry was hit (walk stops)
    uint64_t lin_lba;       // its FirstLBA (valid iff found_lin)
} parttbl_scan_t;

static const uint8_t PT_GUID_ESP[16] = {
    0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B };
static const uint8_t PT_GUID_LINUX[16] = {
    0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4 };

// GPT header parse + validation. Verbatim: the "EFI PART" signature test and
// the `esz >= 128 && esz <= 512 && num > 0 && num <= 256` acceptance guard.
// Returns 0 and fills *out on accept, -1 on reject. The live buffer is always
// 512 bytes, so `len` == 512 at every call site; the literal 512 the original
// hardcoded becomes `len` here, which is behavior-identical at len == 512.
//
// NOTE (verbatim): esz >= 128 is LOAD-BEARING for memory safety. The entry read
// below spans 40 bytes at offset e*esz for e < per_sec = len/esz, so the last
// byte touched is (per_sec-1)*esz + 39 <= len - esz + 39 <= 512 - 128 + 39 =
// 423 < 512. Relax that floor below 40, or let a caller pass a per_sec not
// derived from len/esz, and the scan reads past the sector immediately.
int parttbl_gpt_hdr_c(const uint8_t *sec, uint32_t len, parttbl_gpt_hdr_t *out) {
    if (!sec || !out) return -1;

    if (!(sec[0]=='E'&&sec[1]=='F'&&sec[2]=='I'&&sec[3]==' '&&
          sec[4]=='P'&&sec[5]=='A'&&sec[6]=='R'&&sec[7]=='T')) {
        return -1;
    }

    uint64_t ent_lba = 0;
    for (int i=0;i<8;i++) ent_lba |= (uint64_t)sec[72+i] << (8*i);
    uint32_t num = rd32(sec + 80);
    uint32_t esz = rd32(sec + 84);

    if (!(esz >= 128 && esz <= len && num > 0 && num <= 256)) return -1;

    uint32_t per_sec = len / esz;
    if (per_sec == 0) per_sec = 1;

    out->ent_lba = ent_lba;
    out->num     = num;
    out->esz     = esz;
    out->per_sec = per_sec;
    out->_pad    = 0;
    return 0;
}

// Scan ONE sector of the GPT partition-entry array. Verbatim inner loop.
// `esz`/`per_sec` MUST come from a parttbl_gpt_hdr_c-validated header; that is
// the contract this function's memory safety rests on (see the note above).
// `remaining` = num - scanned, caps entries classified in this sector.
//
// io_fallback: the original keeps ONE running u32 fallback across the WHOLE
// entry array and re-tests the TRUNCATED stored value each time, so an entry
// whose FirstLBA truncates to 0 in u32 (e.g. 0x100000000) leaves the slot open
// for a later entry. Threading the running u32 through is what makes this
// byte-faithful; a per-sector "found_fb" flag would NOT be (it latches and
// wrongly discards the later entry). This was caught by the offline oracle.
int parttbl_gpt_sec_scan_c(const uint8_t *sec, uint32_t len, uint32_t esz,
                           uint32_t per_sec, uint32_t remaining,
                           uint32_t *io_fallback, parttbl_scan_t *out) {
    (void)len;
    if (!sec || !out || !io_fallback) return -1;
    memset(out, 0, sizeof(*out));

    for (uint32_t e = 0; e < per_sec && out->consumed < remaining; e++, out->consumed++) {
        const uint8_t *ent = sec + e * esz;   // VERBATIM: unchecked
        int empty = 1, is_esp = 1, is_lin = 1;
        for (int i = 0; i < 16; i++) {
            if (ent[i]) empty = 0;
            if (ent[i] != PT_GUID_ESP[i])   is_esp = 0;
            if (ent[i] != PT_GUID_LINUX[i]) is_lin = 0;
        }
        if (empty || is_esp) continue;        // skip empty + the FAT ESP
        uint64_t first = 0;
        for (int i=0;i<8;i++) first |= (uint64_t)ent[32+i] << (8*i);
        if (is_lin) {
            // Live code returns immediately, so the loop's `scanned++` never
            // runs for this entry; mirror that exactly.
            out->found_lin = 1;
            out->lin_lba   = first;
            return 0;
        }
        if (*io_fallback == 0 && first != 0) *io_fallback = (uint32_t)first;
    }
    return 0;
}

// MBR fallback: primary table at LBA 0, first type-0x83 (Linux) entry. Verbatim.
int parttbl_mbr_find_c(const uint8_t *sec, uint32_t len, uint32_t *out_start) {
    (void)len;
    if (!sec || !out_start) return -1;
    if (!(sec[510]==0x55 && sec[511]==0xAA)) return -1;
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = sec + 446 + i*16;
        uint32_t start = rd32(p + 8);
        if (p[4] == 0x83 && start != 0) { *out_start = start; return 0; }
    }
    return -1;
}

// Rust drop-ins (rustkern.rs, #404). Same signatures, same return contracts;
// slices built over exactly `len` bytes make the OOB-read class impossible by
// construction, and the e*esz index multiply is checked.
extern int parttbl_gpt_hdr_rs(const uint8_t *sec, uint32_t len, parttbl_gpt_hdr_t *out);
extern int parttbl_gpt_sec_scan_rs(const uint8_t *sec, uint32_t len, uint32_t esz,
                                   uint32_t per_sec, uint32_t remaining,
                                   uint32_t *io_fallback, parttbl_scan_t *out);
extern int parttbl_mbr_find_rs(const uint8_t *sec, uint32_t len, uint32_t *out_start);

// Live strangler seam. With -DRUST_PARTTBL (set in the Makefile CFLAGS) the
// real symbols route to the Rust ports; otherwise straight back to the C.
// Rollback = drop the one flag and rebuild. The boot-time [RUST-DIFF] self-test
// (parttbl_rust_selftest) compares the two impls regardless of the flag.
static inline int parttbl_gpt_hdr(const uint8_t *sec, uint32_t len, parttbl_gpt_hdr_t *out) {
#ifdef RUST_PARTTBL
    return parttbl_gpt_hdr_rs(sec, len, out);
#else
    return parttbl_gpt_hdr_c(sec, len, out);
#endif
}
static inline int parttbl_gpt_sec_scan(const uint8_t *sec, uint32_t len, uint32_t esz,
                                       uint32_t per_sec, uint32_t remaining,
                                       uint32_t *io_fallback, parttbl_scan_t *out) {
#ifdef RUST_PARTTBL
    return parttbl_gpt_sec_scan_rs(sec, len, esz, per_sec, remaining, io_fallback, out);
#else
    return parttbl_gpt_sec_scan_c(sec, len, esz, per_sec, remaining, io_fallback, out);
#endif
}
static inline int parttbl_mbr_find(const uint8_t *sec, uint32_t len, uint32_t *out_start) {
#ifdef RUST_PARTTBL
    return parttbl_mbr_find_rs(sec, len, out_start);
#else
    return parttbl_mbr_find_c(sec, len, out_start);
#endif
}

// #365: locate an ext2/Linux partition on a disk and return its base LBA. Parses
// GPT first (our images are GPT: FAT ESP p1 + ext2 p2), then MBR (0x83). blk_read
// routes to the right backend - USB-MSC ignores channel/drive and reads whole-
// device LBAs, ATA uses channel/drive - so this works for both boot media.
//
// The byte-walking now goes through the parttbl seam above (C or Rust per
// -DRUST_PARTTBL); the I/O and control flow are unchanged from the original.
int ext2_find_partition(uint8_t channel, uint8_t drive, uint32_t *out_base_lba) {
    if (!out_base_lba) return -1;
    uint8_t *sec = (uint8_t *)kmalloc(512);
    if (!sec) return -1;

    // GPT: header at LBA 1 with the "EFI PART" signature.
    if (blk_read(channel, drive, 1, 1, sec) == 1) {
        parttbl_gpt_hdr_t h;
        if (parttbl_gpt_hdr(sec, 512, &h) == 0) {
            uint32_t scanned = 0, fallback_lba = 0;  // fallback = first non-ESP non-empty
            for (uint32_t s = 0; scanned < h.num; s++) {
                if (blk_read(channel, drive, h.ent_lba + s, 1, sec) != 1) break;
                parttbl_scan_t sc;
                if (parttbl_gpt_sec_scan(sec, 512, h.esz, h.per_sec,
                                         h.num - scanned, &fallback_lba, &sc) != 0) break;
                if (sc.found_lin) {
                    *out_base_lba = (uint32_t)sc.lin_lba; kfree(sec); return 0;
                }
                // Forward-progress guard (#426): consumed is always >= 1 for a
                // header-validated per_sec, so this is defensive only; it makes
                // the original's implicit termination explicit and bounded.
                if (sc.consumed == 0) break;
                scanned += sc.consumed;
            }
            if (fallback_lba != 0) { *out_base_lba = fallback_lba; kfree(sec); return 0; }
        }
    }

    // MBR fallback: primary partition table at LBA 0, first type-0x83 (Linux) entry.
    if (blk_read(channel, drive, 0, 1, sec) == 1) {
        uint32_t st = 0;
        if (parttbl_mbr_find(sec, 512, &st) == 0) { *out_base_lba = st; kfree(sec); return 0; }
    }

    kfree(sec);
    return -1;
}

// ---------------------------------------------------------------------------
// Boot-time differential self-test for the partition-table parser (#404).
// Asserts parttbl_*_rs == parttbl_*_c over well-formed GPT tables (the real
// #365 ESP+Linux layout, every legal SizeOfPartitionEntry, every entry slot)
// and malformed ones (bad signature, lying NumberOfPartitionEntries, absurd
// SizeOfPartitionEntry, overflowing entries*size, entry array past the buffer,
// FirstLBA truncating to 0) plus a random-fuzz corpus, on THIS exact build,
// then logs ONE [RUST-DIFF] line to serial + the persistent /BOOTLOG.TXT.
// Bounded, runs once at boot, no blocking (#426). It always calls BOTH impls
// so the Rust members are pulled into the link and compared regardless of
// RUST_PARTTBL.
static uint8_t ptdiff_sec[512];

// Serialized TSC read. `lfence; rdtsc` NOT `rdtscp`: rdtscp #UDs on the kvm64
// CPU model the test VMs use.
static inline uint64_t ptdiff_tsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline uint32_t ptdiff_rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

// Run both impls on ptdiff_sec for one (esz, per_sec, remaining) and compare.
static void ptdiff_scan_one(uint32_t esz, uint32_t per_sec, uint32_t remaining,
                            uint32_t *vectors, uint32_t *mismatches, int *first_bad) {
    parttbl_scan_t a, b;
    uint32_t fa = 0, fb = 0;
    int ra = parttbl_gpt_sec_scan_c (ptdiff_sec, 512, esz, per_sec, remaining, &fa, &a);
    int rb = parttbl_gpt_sec_scan_rs(ptdiff_sec, 512, esz, per_sec, remaining, &fb, &b);
    (*vectors)++;
    int bad = (ra != rb) || (fa != fb) || (a.consumed != b.consumed) ||
              (a.found_lin != b.found_lin) ||
              (a.found_lin && a.lin_lba != b.lin_lba);
    if (bad) { (*mismatches)++; if (*first_bad < 0) *first_bad = (int)(*vectors); }
}

static void ptdiff_hdr_one(uint32_t *vectors, uint32_t *mismatches, int *first_bad) {
    parttbl_gpt_hdr_t a, b;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    int ra = parttbl_gpt_hdr_c (ptdiff_sec, 512, &a);
    int rb = parttbl_gpt_hdr_rs(ptdiff_sec, 512, &b);
    (*vectors)++;
    int bad = (ra != rb) || (ra == 0 && (a.ent_lba != b.ent_lba || a.num != b.num ||
                                         a.esz != b.esz || a.per_sec != b.per_sec));
    if (bad) { (*mismatches)++; if (*first_bad < 0) *first_bad = (int)(*vectors); }
}

static void ptdiff_mbr_one(uint32_t *vectors, uint32_t *mismatches, int *first_bad) {
    uint32_t sa = 0xC0FFEE00u, sb = 0xC0FFEE00u;
    int ra = parttbl_mbr_find_c (ptdiff_sec, 512, &sa);
    int rb = parttbl_mbr_find_rs(ptdiff_sec, 512, &sb);
    (*vectors)++;
    int bad = (ra != rb) || (ra == 0 && sa != sb);
    if (bad) { (*mismatches)++; if (*first_bad < 0) *first_bad = (int)(*vectors); }
}

void parttbl_rust_selftest(void) {
    uint32_t seed = 0x9e3779b9u;
    uint32_t vectors = 0, mismatches = 0;
    int first_bad = -1;

    // Pass 1: WELL-FORMED GPT headers across every legal SizeOfPartitionEntry
    // and NumberOfPartitionEntries boundary, plus the rejects either side.
    for (uint32_t esz = 120; esz <= 520; esz++) {
        memset(ptdiff_sec, 0, 512);
        memcpy(ptdiff_sec, "EFI PART", 8);
        ptdiff_sec[72] = 2;                                  // ent_lba = 2
        ptdiff_sec[80] = 128;                                // num = 128
        ptdiff_sec[84] = (uint8_t)(esz & 0xFF);
        ptdiff_sec[85] = (uint8_t)((esz >> 8) & 0xFF);
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
    }
    for (uint32_t num = 0; num <= 260; num++) {
        memset(ptdiff_sec, 0, 512);
        memcpy(ptdiff_sec, "EFI PART", 8);
        ptdiff_sec[72] = 2;
        ptdiff_sec[80] = (uint8_t)(num & 0xFF);
        ptdiff_sec[81] = (uint8_t)((num >> 8) & 0xFF);
        ptdiff_sec[84] = 128;
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
    }

    // Pass 2: WELL-FORMED entry sectors. Real ESP/Linux/other GUIDs at every
    // slot for every legal esz, so the FOUND path and the fallback path are
    // both exercised (not just the not-found return).
    for (uint32_t esz = 128; esz <= 512; esz += 8) {
        uint32_t per = 512 / esz; if (per == 0) per = 1;
        for (uint32_t slot = 0; slot < per; slot++) {
            memset(ptdiff_sec, 0, 512);
            for (uint32_t e = 0; e < per; e++) {
                uint8_t *ent = ptdiff_sec + e * esz;
                if (e == slot) memcpy(ent, PT_GUID_LINUX, 16);
                else if (e & 1) memcpy(ent, PT_GUID_ESP, 16);
                else            ent[0] = 0x5A;               // other/non-empty
                ent[32] = (uint8_t)(100 + e);                // FirstLBA
            }
            ptdiff_scan_one(esz, per, per, &vectors, &mismatches, &first_bad);
        }
    }

    // Pass 3: the FirstLBA-truncates-to-0 fallback semantics (a non-Linux entry
    // whose FirstLBA is non-zero as u64 but 0 in u32 must NOT latch the
    // fallback slot shut; a later entry must still claim it).
    {
        memset(ptdiff_sec, 0, 512);
        ptdiff_sec[0] = 0x5A; ptdiff_sec[32+4] = 1;          // first = 0x100000000
        ptdiff_sec[128] = 0x5B; ptdiff_sec[128+32] = 0x09;   // first = 9
        ptdiff_scan_one(128, 4, 4, &vectors, &mismatches, &first_bad);
    }

    // Pass 4: MALFORMED / random fuzz. Every disk-controlled field takes
    // adversarial values; the header guard, the entries*size multiply and the
    // entry walk are all surfaced.
    for (int n = 0; n < 8000; n++) {
        for (uint32_t i = 0; i < 512; i++)
            ptdiff_sec[i] = (uint8_t)(ptdiff_rng(&seed) & 0xFF);
        if (ptdiff_rng(&seed) & 1) memcpy(ptdiff_sec, "EFI PART", 8);
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
        ptdiff_mbr_one(&vectors, &mismatches, &first_bad);
        // Drive the scan with a header-derived (in-contract) esz/per_sec.
        uint32_t esz = 128 + (ptdiff_rng(&seed) % 385);      // 128..512
        uint32_t per = 512 / esz; if (per == 0) per = 1;
        ptdiff_scan_one(esz, per, 1 + (ptdiff_rng(&seed) % per),
                        &vectors, &mismatches, &first_bad);
    }

    // Pass 5: explicit crafted shapes (documents each guard class).
    {
        // (a) bad signature
        memset(ptdiff_sec, 0, 512); memcpy(ptdiff_sec, "EFI PAR!", 8);
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
        // (b) esz below the 128 floor / above the 512 ceiling
        memset(ptdiff_sec, 0, 512); memcpy(ptdiff_sec, "EFI PART", 8);
        ptdiff_sec[80] = 4; ptdiff_sec[84] = 39;
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
        memset(ptdiff_sec, 0, 512); memcpy(ptdiff_sec, "EFI PART", 8);
        ptdiff_sec[80] = 4; ptdiff_sec[84] = 0x01; ptdiff_sec[86] = 0x01; // 65537
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
        // (c) entries*size overflows u32 (num=256, esz=0x1000000 -> 4G wrap)
        memset(ptdiff_sec, 0, 512); memcpy(ptdiff_sec, "EFI PART", 8);
        ptdiff_sec[80] = 0x00; ptdiff_sec[81] = 0x01;        // num = 256
        ptdiff_sec[87] = 0x01;                               // esz = 0x01000000
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
        // (d) num = 0 / num = 257
        memset(ptdiff_sec, 0, 512); memcpy(ptdiff_sec, "EFI PART", 8);
        ptdiff_sec[84] = 128;
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
        memset(ptdiff_sec, 0, 512); memcpy(ptdiff_sec, "EFI PART", 8);
        ptdiff_sec[80] = 0x01; ptdiff_sec[81] = 0x01; ptdiff_sec[84] = 128;
        ptdiff_hdr_one(&vectors, &mismatches, &first_bad);
        // (e) MBR: no signature / 0x83 with start 0 / 0x83 in the last slot
        memset(ptdiff_sec, 0, 512);
        ptdiff_mbr_one(&vectors, &mismatches, &first_bad);
        memset(ptdiff_sec, 0, 512);
        ptdiff_sec[510] = 0x55; ptdiff_sec[511] = 0xAA; ptdiff_sec[446+4] = 0x83;
        ptdiff_mbr_one(&vectors, &mismatches, &first_bad);
        memset(ptdiff_sec, 0, 512);
        ptdiff_sec[510] = 0x55; ptdiff_sec[511] = 0xAA;
        ptdiff_sec[446+3*16+4] = 0x83; ptdiff_sec[446+3*16+8] = 0x2A;
        ptdiff_mbr_one(&vectors, &mismatches, &first_bad);
    }

    {   // #609: prove the new extent math before anything writes with it.
        extern uint32_t ext2_extent_selftest_rs(void);
        uint32_t ef = ext2_extent_selftest_rs();
        kprintf("[RUST-DIFF] ext2extent: %u failures -> %s\n", ef, ef ? "FAIL" : "PASS");
        bootlog_write("[RUST-DIFF] ext2extent: %u failures -> %s", ef, ef ? "FAIL" : "PASS");
    }
    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] parttbl: %u vectors, %u mismatches -> %s\n",
            vectors, mismatches, verdict);
    bootlog_write("[RUST-DIFF] parttbl: %u vectors, %u mismatches -> %s",
                  vectors, mismatches, verdict);
    if (mismatches != 0) {
        kprintf("[RUST-DIFF] parttbl FIRST MISMATCH at vector #%d\n", first_bad);
        bootlog_write("[RUST-DIFF] parttbl FIRST MISMATCH at vector #%d", first_bad);
    }

    // [RUST-PERF]: RDTSC micro-benchmark over the hot per-sector entry walk
    // (4 x 128-byte entries, the real #365 shape). LIGHT: 2k iters, bounded,
    // runs once at boot (#426). `lfence; rdtsc`, never rdtscp (kvm64 #UD).
    {
        const int iters = 2000;
        memset(ptdiff_sec, 0, 512);
        for (int e = 0; e < 4; e++) {
            ptdiff_sec[e*128] = 0x5A;          // non-empty, non-ESP, non-Linux
            ptdiff_sec[e*128 + 32] = (uint8_t)(90 + e);
        }
        parttbl_scan_t sc; uint32_t fb;
        for (int i = 0; i < 200; i++) {
            fb = 0; parttbl_gpt_sec_scan_c (ptdiff_sec, 512, 128, 4, 4, &fb, &sc);
            fb = 0; parttbl_gpt_sec_scan_rs(ptdiff_sec, 512, 128, 4, 4, &fb, &sc);
        }
        uint64_t t0 = ptdiff_tsc();
        for (int i = 0; i < iters; i++) { fb = 0; parttbl_gpt_sec_scan_c (ptdiff_sec, 512, 128, 4, 4, &fb, &sc); }
        uint64_t t1 = ptdiff_tsc();
        for (int i = 0; i < iters; i++) { fb = 0; parttbl_gpt_sec_scan_rs(ptdiff_sec, 512, 128, 4, 4, &fb, &sc); }
        uint64_t t2 = ptdiff_tsc();
        uint64_t c_cyc = (t1-t0)/iters, r_cyc = (t2-t1)/iters;
        uint64_t ratio100 = c_cyc ? (r_cyc*100ULL/c_cyc) : 0;
        kprintf("[RUST-PERF] parttbl: gpt_sec_scan C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc,(unsigned long long)r_cyc,
                (unsigned long long)(ratio100/100),(unsigned long long)(ratio100%100));
        bootlog_write("[RUST-PERF] parttbl: gpt_sec_scan C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                (unsigned long long)c_cyc,(unsigned long long)r_cyc,
                (unsigned long long)(ratio100/100),(unsigned long long)(ratio100%100));
    }
}


// #610: ext2_mount() marks the volume dirty, but the superblock-state writer
// lives with the rest of the #610 code at the end of this file (it belongs next
// to the checker it exists for, not in the middle of the mount path). These are
// the pieces the mount path needs up front.
#define EXT2_VALID_FS        1u   // s_state: last unmount was clean
#define EXT2_ERROR_FS        2u   // s_state: an error has been recorded (sticky)
#define E2ST_DIRTY           0    // clear VALID (mounted / not cleanly closed)
#define E2ST_CLEAN           1    // set VALID   (clean unmount; ERROR preserved)
#define E2ST_ERROR           2    // set ERROR   (sticky)
#define E2ST_VERIFIED_CLEAN  3    // set VALID + clear ERROR (a full check passed)
static int ext2_sb_set_state(ext2_fs_t *fs, int op, int bump_mnt);
void ext2_mark_error(const char *why);

int ext2_mount(uint8_t channel, uint8_t drive, uint32_t part_start_lba) {
    g_ext2.mounted = 0;

    // Superblock is 1024 bytes at byte offset 1024 = LBA 2 (two sectors), relative
    // to the start of the ext2 partition (part_start_lba, 0 for a whole-disk vol).
    uint8_t *sb = (uint8_t *)kmalloc(1024);
    if (!sb) {
        kprintf("[EXT2] mount: kmalloc superblock failed\n");
        return -1;
    }
    if (blk_read(channel, drive, part_start_lba + 2, 2, sb) != 2) {
        kprintf("[EXT2] mount: failed to read superblock\n");
        kfree(sb);
        return -2;
    }

    uint16_t magic = rd16(sb + 56);
    if (magic != EXT2_MAGIC) {
        kprintf("[EXT2] mount: bad magic 0x%x (expected 0x%x)\n",
                (unsigned)magic, (unsigned)EXT2_MAGIC);
        kfree(sb);
        return -3;
    }

    uint32_t log_block_size = rd32(sb + 24);

    g_ext2.channel          = channel;
    g_ext2.drive            = drive;
    g_ext2.part_start_lba   = part_start_lba;   // #365: partition base LBA
    g_ext2.inodes_count     = rd32(sb + 0);
    g_ext2.blocks_count     = rd32(sb + 4);
    g_ext2.first_data_block = rd32(sb + 20);
    g_ext2.block_size       = (uint32_t)1024 << log_block_size;
    g_ext2.blocks_per_group = rd32(sb + 32);
    g_ext2.inodes_per_group = rd32(sb + 40);

    uint16_t inode_size16   = rd16(sb + 88);
    // Revision 0 filesystems report inode size 0; default to 128.
    g_ext2.inode_size = inode_size16 ? (uint32_t)inode_size16 : 128;

    if (g_ext2.blocks_per_group == 0 || g_ext2.inodes_per_group == 0 ||
        g_ext2.block_size == 0) {
        kprintf("[EXT2] mount: invalid superblock geometry\n");
        kfree(sb);
        return -4;
    }

    // Number of block groups (ceil(blocks_count / blocks_per_group)).
    g_ext2.groups_count =
        (g_ext2.blocks_count - g_ext2.first_data_block + g_ext2.blocks_per_group - 1)
        / g_ext2.blocks_per_group;
    if (g_ext2.groups_count == 0) {
        g_ext2.groups_count = 1;
    }

    // The block group descriptor table starts in the block right after the
    // superblock. For 1KB blocks that is first_data_block + 1 (block 2).
    g_ext2.bgd_table_block = g_ext2.first_data_block + 1;

    // #610: capture the state fields BEFORE we touch them. Everything after
    // this point in the boot depends on knowing what the volume looked like
    // when we found it; once we mark it dirty that information is gone.
    {
        uint32_t rev = rd32(sb + 76);
        g_ext2.sb_state_at_mount = rd16(sb + 58);
        g_ext2.sb_mnt_count      = rd16(sb + 52);
        g_ext2.sb_max_mnt_count  = (int16_t)rd16(sb + 54);
        g_ext2.sb_lastcheck      = rd32(sb + 64);
        g_ext2.sb_checkinterval  = rd32(sb + 68);
        g_ext2.sb_first_ino      = (rev >= 1) ? rd32(sb + 84) : 11u;
        if (g_ext2.sb_first_ino == 0 || g_ext2.sb_first_ino > g_ext2.inodes_count)
            g_ext2.sb_first_ino = 11u;
        uint32_t rocompat        = (rev >= 1) ? rd32(sb + 100) : 0u;
        g_ext2.sb_sparse_super   = (rocompat & 0x0001u) ? 1u : 0u;
        g_ext2.sb_reserved_gdt   = (rev >= 1) ? (uint32_t)rd16(sb + 206) : 0u;
        for (int li = 0; li < 16; li++) g_ext2.sb_label[li] = (char)sb[120 + li];
        g_ext2.sb_label[16] = 0;
        if (!g_ext2.sb_label[0]) {
            const char *anon = "(unlabelled)";
            int li = 0; while (anon[li]) { g_ext2.sb_label[li] = anon[li]; li++; }
            g_ext2.sb_label[li] = 0;
        }
    }

    kfree(sb);
    g_ext2.mounted = 1;

    // #610: mark the volume dirty for the duration of the mount and bump the
    // mount count, so an unclean stop is detectable by US and by any Linux
    // host running dumpe2fs/e2fsck. This is a 1KB superblock rewrite with a
    // read-back verify; if the medium is read-only it simply fails and logs.
    {
        int rc = ext2_sb_set_state(&g_ext2, E2ST_DIRTY, 1);
        kprintf("[EXT2] #610: mounted; s_state on disk was 0x%x (%s), mount count now %u%s\n",
                (unsigned)g_ext2.sb_state_at_mount,
                (g_ext2.sb_state_at_mount & EXT2_ERROR_FS) ? "ERROR RECORDED"
                  : ((g_ext2.sb_state_at_mount & EXT2_VALID_FS) ? "clean" : "NOT CLEANLY UNMOUNTED"),
                (unsigned)g_ext2.sb_mnt_count,
                (rc == 0) ? "" : " (could NOT mark dirty)");
        bootlog_write("[EXT2] #610: mount state 0x%x mount-count %u dirty-mark %s",
                      (unsigned)g_ext2.sb_state_at_mount, (unsigned)g_ext2.sb_mnt_count,
                      (rc == 0) ? "ok" : "FAILED");
    }
    return 0;
}

// Fetch the inode table block number for the group that owns inode `ino`.
static int ext2_group_inode_table(const ext2_fs_t *fs, uint32_t group,
                                  uint32_t *out_inode_table) {
    // Each block group descriptor is 32 bytes. Compute which descriptor-table
    // block the requested descriptor lives in and read just that block.
    uint32_t desc_per_block = fs->block_size / 32;
    uint32_t block = fs->bgd_table_block + (group / desc_per_block);
    uint32_t index_in_block = group % desc_per_block;

    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) {
        return -1;
    }
    if (ext2_read_block(fs, block, blk) != 0) {
        kfree(blk);
        return -2;
    }
    // bg_inode_table is at offset 8 within the 32-byte descriptor.
    *out_inode_table = rd32(blk + index_in_block * 32 + 8);
    kfree(blk);
    return 0;
}

// ===========================================================================
// #115: TIMESTAMP STAMPING ON WRITE.
//
// Filling st_mtime from the inode is only half the fix. Before #115 this driver
// wrote i_atime/i_ctime/i_mtime as ZERO on every file it created (see
// ext2_create_file_ino / ext2_mkdir_inner), so "read the real mtime" would have
// returned a real, honestly-read, permanent 0 for every file MayteraOS itself
// produced - the golden image included. A stat that reports a truthfully-read
// zero is indistinguishable to the user from the memset zero it replaced.
//
// The clock is the CMOS RTC via wallclock_now_unix() (cpu/wallclock.h). It is
// NOT sys_time(), which returns seconds since BOOT (#113), and NOT timer_ticks,
// which is not a measure of elapsed time at all (#524).
//
// IF THE CLOCK DOES NOT KNOW, WE DO NOT STAMP. wallclock_now_unix() returns 0
// when the RTC has no plausible date; these helpers then leave the field alone
// rather than writing an epoch-zero that reads as 1970-01-01. "Unknown" must
// stay distinguishable from "1 January 1970".
// ===========================================================================
static void ext2_stamp_raw_new(uint8_t *ri) {
    int64_t now = wallclock_now_unix();
    if (now <= 0) return;              // no clock: leave the fields at 0/unknown
    uint32_t t = (uint32_t)now;
    memcpy(ri + 8,  &t, 4);            // i_atime
    memcpy(ri + 12, &t, 4);            // i_ctime
    memcpy(ri + 16, &t, 4);            // i_mtime
}

// Same, for a MODIFICATION of an existing inode: data changed, so i_mtime and
// i_ctime move and i_atime does not.
static void ext2_stamp_raw_mtime(uint8_t *ri) {
    int64_t now = wallclock_now_unix();
    if (now <= 0) return;
    uint32_t t = (uint32_t)now;
    memcpy(ri + 12, &t, 4);            // i_ctime
    memcpy(ri + 16, &t, 4);            // i_mtime
}

int ext2_read_inode(uint32_t ino, ext2_inode_t *out) {
    if (!g_ext2.mounted || ino == 0) {
        return -1;
    }

    uint32_t group = (ino - 1) / g_ext2.inodes_per_group;
    uint32_t index = (ino - 1) % g_ext2.inodes_per_group;
    if (group >= g_ext2.groups_count) {
        return -2;
    }

    uint32_t inode_table = 0;
    if (ext2_group_inode_table(&g_ext2, group, &inode_table) != 0) {
        return -3;
    }

    // Byte offset of the inode within the device.
    uint64_t byte_off = (uint64_t)inode_table * g_ext2.block_size
                        + (uint64_t)index * g_ext2.inode_size;

    // Read the containing block, then copy the inode out of it. The inode is
    // <= block_size and may straddle nothing because inode_size divides evenly
    // into block_size for sane filesystems (256 | 1024).
    uint32_t block_of_inode = (uint32_t)(byte_off / g_ext2.block_size);
    uint32_t off_in_block   = (uint32_t)(byte_off % g_ext2.block_size);

    uint8_t *blk = (uint8_t *)kmalloc(g_ext2.block_size);
    if (!blk) {
        return -4;
    }
    if (ext2_read_block(&g_ext2, block_of_inode, blk) != 0) {
        kfree(blk);
        return -5;
    }

    const uint8_t *ip = blk + off_in_block;
    out->i_mode      = rd16(ip + 0);
    out->i_size      = rd32(ip + 4);
    out->i_size_high = rd32(ip + 108); // i_dir_acl (high size for large_file)
    for (int i = 0; i < 15; i++) {
        out->i_block[i] = rd32(ip + 40 + i * 4);
    }
    // #115: the rest of the on-disk inode, which this driver read off the disk
    // into `blk` on every single call and then discarded. The offsets are the
    // ext2 rev-0/rev-1 inode layout (ext2fs/ext2_fs.h), NOT the field order of
    // ext2_inode_t; they were checked against a real mke2fs volume, not
    // remembered - a guessed struct offset does not fail loudly, it ships a
    // field that reads plausible garbage.
    //   0  i_mode(16)   2  i_uid(16)    4  i_size(32)     8  i_atime(32)
    //  12  i_ctime(32) 16  i_mtime(32) 20  i_dtime(32)    24  i_gid(16)
    //  26  i_links_count(16)           28  i_blocks(32)   40  i_block[15]
    // 108  i_dir_acl/size_high(32)    120  l_i_uid_high(16) 122 l_i_gid_high(16)
    out->i_atime       = rd32(ip + 8);
    out->i_ctime       = rd32(ip + 12);
    out->i_mtime       = rd32(ip + 16);
    out->i_dtime       = rd32(ip + 20);
    out->i_links_count = rd16(ip + 26);
    out->i_blocks      = rd32(ip + 28);
    // The high halves live in osd2 and are Linux-specific. They are zero on
    // every volume this OS creates, and reading them costs nothing, so read
    // them rather than silently capping ownership at uid 65535.
    out->i_uid = (uint32_t)rd16(ip + 2)  | ((uint32_t)rd16(ip + 120) << 16);
    out->i_gid = (uint32_t)rd16(ip + 24) | ((uint32_t)rd16(ip + 122) << 16);

    kfree(blk);
    return 0;
}

// #554: thin accessor for rustkern/fsperm.rs (Files Properties / details
// columns / chmod-chown routing). Deliberately returns just the one bit Rust
// needs rather than the whole ext2_inode_t: mirroring that struct's layout in
// Rust would be a second hand-maintained copy of an on-disk format this file
// already owns, exactly the class of drift the codebase's sizeof-lock
// convention (_Static_assert + argtab SZ_* constants) exists to catch. A
// one-field accessor has no layout to drift.
int ext2_get_is_dir(uint32_t ino, int *is_dir_out) {
    ext2_inode_t in;
    if (ext2_read_inode(ino, &in) != 0) return -1;
    if (is_dir_out) *is_dir_out = ((in.i_mode & 0xF000) == 0x4000) ? 1 : 0;
    return 0;
}

// Map a logical block index within a file to its physical block number.
// Handles direct, singly-indirect, and doubly-indirect pointers. Returns the
// physical block number, or 0 for a hole / out-of-range. On read error returns
// 0xFFFFFFFF (sentinel) so callers can distinguish from a legitimate hole.
#define EXT2_BMAP_ERR 0xFFFFFFFFu
static uint32_t ext2_bmap(const ext2_fs_t *fs, const ext2_inode_t *inode,
                          uint32_t logical) {
    uint32_t ptrs_per_block = fs->block_size / 4;

    // Direct blocks 0..11.
    if (logical < EXT2_NDIR_BLOCKS) {
        return inode->i_block[logical];
    }
    logical -= EXT2_NDIR_BLOCKS;

    // Singly-indirect: one block of ptrs_per_block pointers.
    if (logical < ptrs_per_block) {
        uint32_t ind = inode->i_block[EXT2_IND_BLOCK];
        if (ind == 0) {
            return 0; // hole
        }
        uint32_t *blk = (uint32_t *)kmalloc(fs->block_size);
        if (!blk) {
            return EXT2_BMAP_ERR;
        }
        if (ext2_read_block(fs, ind, blk) != 0) {
            kfree(blk);
            return EXT2_BMAP_ERR;
        }
        uint32_t phys = blk[logical];
        kfree(blk);
        return phys;
    }
    logical -= ptrs_per_block;

    // Doubly-indirect: block of pointers to singly-indirect blocks.
    if (logical < ptrs_per_block * ptrs_per_block) {
        uint32_t dind = inode->i_block[EXT2_DIND_BLOCK];
        if (dind == 0) {
            return 0; // hole
        }
        uint32_t outer_index = logical / ptrs_per_block;
        uint32_t inner_index = logical % ptrs_per_block;

        uint32_t *blk = (uint32_t *)kmalloc(fs->block_size);
        if (!blk) {
            return EXT2_BMAP_ERR;
        }
        if (ext2_read_block(fs, dind, blk) != 0) {
            kfree(blk);
            return EXT2_BMAP_ERR;
        }
        uint32_t ind = blk[outer_index];
        if (ind == 0) {
            kfree(blk);
            return 0; // hole
        }
        if (ext2_read_block(fs, ind, blk) != 0) {
            kfree(blk);
            return EXT2_BMAP_ERR;
        }
        uint32_t phys = blk[inner_index];
        kfree(blk);
        return phys;
    }

    // Triply-indirect not needed for the test files; treat as out of range.
    return 0;
}

// Resolve a file's logical block -> physical block using caller-provided
// indirect-block caches, so a sequential read reads each singly/doubly-indirect
// pointer block at most once per run instead of once per data block. Returns the
// physical block (0 = sparse hole) or EXT2_BMAP_ERR on a read error.
static uint32_t ext2_resolve_cached(const ext2_inode_t *inode, uint32_t logical,
                                    uint8_t *indbuf, uint32_t *ind_cached,
                                    uint8_t *dindbuf, uint32_t *dind_cached) {
    uint32_t ptrs = g_ext2.block_size / 4;
    if (logical < EXT2_NDIR_BLOCKS) {
        return inode->i_block[logical];
    }
    logical -= EXT2_NDIR_BLOCKS;
    if (logical < ptrs) {                       // singly-indirect
        uint32_t ind = inode->i_block[EXT2_IND_BLOCK];
        if (!ind) return 0;
        if (*ind_cached != ind) {
            if (ext2_read_block(&g_ext2, ind, indbuf) != 0) return EXT2_BMAP_ERR;
            *ind_cached = ind;
        }
        return rd32(indbuf + logical * 4);
    }
    logical -= ptrs;
    if (logical < ptrs * ptrs) {                // doubly-indirect
        uint32_t dind = inode->i_block[EXT2_DIND_BLOCK];
        if (!dind) return 0;
        if (*dind_cached != dind) {
            if (ext2_read_block(&g_ext2, dind, dindbuf) != 0) return EXT2_BMAP_ERR;
            *dind_cached = dind;
        }
        uint32_t ind = rd32(dindbuf + (logical / ptrs) * 4);
        if (!ind) return 0;
        if (*ind_cached != ind) {
            if (ext2_read_block(&g_ext2, ind, indbuf) != 0) return EXT2_BMAP_ERR;
            *ind_cached = ind;
        }
        return rd32(indbuf + (logical % ptrs) * 4);
    }
    return 0;   // triply-indirect not supported (treat as hole)
}

int64_t ext2_read_file_ino(uint32_t ino, void *buf, uint64_t max) {
    if (!g_ext2.mounted) {
        return -1;
    }
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) {
        return -2;
    }

    uint64_t size = inode.i_size;
    // Honor high 32 bits if present (large_file). Test files are small.
    if (inode.i_size_high != 0) {
        size |= (uint64_t)inode.i_size_high << 32;
    }
    if (size > max) {
        size = max;
    }
    if (size == 0) {
        return 0;
    }

    uint8_t *out = (uint8_t *)buf;
    uint32_t bs  = g_ext2.block_size;
    uint32_t spb = bs / EXT2_SECTOR_SIZE;
    uint8_t *indbuf  = (uint8_t *)kmalloc(bs);
    uint8_t *dindbuf = (uint8_t *)kmalloc(bs);
    uint8_t *tmp     = (uint8_t *)kmalloc(bs);   // for the final partial block
    if (!indbuf || !dindbuf || !tmp) {
        if (indbuf) kfree(indbuf);
        if (dindbuf) kfree(dindbuf);
        if (tmp) kfree(tmp);
        return -3;
    }
    uint32_t ind_cached = 0xFFFFFFFFu, dind_cached = 0xFFFFFFFFu;
    int64_t  rc = -5;

    uint64_t copied = 0;
    uint32_t lb = 0;
    while (copied < size) {
        uint32_t phys = ext2_resolve_cached(&inode, lb, indbuf, &ind_cached,
                                            dindbuf, &dind_cached);
        if (phys == EXT2_BMAP_ERR) { rc = -4; goto done; }

        uint64_t remaining = size - copied;
        if (remaining < bs) {
            // Final partial block: read into a temp buffer, copy the tail bytes
            // (avoids writing past a non-block-aligned output buffer).
            if (phys == 0) {
                memset(out + copied, 0, (size_t)remaining);
            } else {
                if (ext2_read_block(&g_ext2, phys, tmp) != 0) goto done;
                memcpy(out + copied, tmp, (size_t)remaining);
            }
            copied += remaining;
            break;
        }
        if (phys == 0) {
            memset(out + copied, 0, bs);   // sparse hole (full block)
            copied += bs; lb++;
            continue;
        }
        // Gather a run of physically-contiguous FULL blocks and read them in one
        // ATA transfer straight into the output buffer (capped to <=240 sectors).
        uint32_t run = 1;
        while (copied + (uint64_t)(run + 1) * bs <= size &&
               (run + 1) * spb <= EXT2_DMA_MAX_SECTORS) {
            uint32_t nx = ext2_resolve_cached(&inode, lb + run, indbuf, &ind_cached,
                                              dindbuf, &dind_cached);
            if (nx != phys + run) break;   // hole, error, or discontiguous
            run++;
        }
        if (blk_read(g_ext2.channel, g_ext2.drive, g_ext2.part_start_lba + phys * spb,
                        run * spb, out + copied) != (int)(run * spb)) {
            goto done;
        }
        copied += (uint64_t)run * bs;
        lb     += run;
    }
    rc = (int64_t)copied;

done:
    kfree(indbuf); kfree(dindbuf); kfree(tmp);
    return rc;
}

// #572: block size of the mounted volume (0 if not mounted). Lets the syscall
// layer size its bounded read window / write staging buffer in whole blocks.
uint32_t ext2_block_size(void) {
    return g_ext2.mounted ? g_ext2.block_size : 0;
}

// #572 STREAMING READ: read up to `len` bytes starting at byte offset `off`
// from inode `ino` into `dst`, touching ONLY the blocks that cover
// [off, off+len). This is the bounded-RAM counterpart of ext2_read_file_ino()
// (which reads the WHOLE file), so a caller can serve arbitrarily large files
// through a small fixed window instead of kmalloc(filesize) at open. Returns
// bytes read (0 at/after EOF), or a negative error. Reuses the exact same
// indirect-block resolver, block cache, and contiguous-run DMA gather as the
// whole-file reader, so the bytes returned are identical.
int64_t ext2_read_file_range(uint32_t ino, uint64_t off, uint64_t len, void *dst) {
    if (!g_ext2.mounted) return -1;
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) return -2;

    uint64_t size = inode.i_size;
    if (inode.i_size_high != 0) size |= (uint64_t)inode.i_size_high << 32;
    if (off >= size) return 0;
    if (off + len > size) len = size - off;   // clamp to EOF
    if (len == 0) return 0;

    uint8_t *out = (uint8_t *)dst;
    uint32_t bs  = g_ext2.block_size;
    uint32_t spb = bs / EXT2_SECTOR_SIZE;
    uint8_t *indbuf  = (uint8_t *)kmalloc(bs);
    uint8_t *dindbuf = (uint8_t *)kmalloc(bs);
    uint8_t *tmp     = (uint8_t *)kmalloc(bs);   // partial (leading/trailing) blocks
    if (!indbuf || !dindbuf || !tmp) {
        if (indbuf) kfree(indbuf);
        if (dindbuf) kfree(dindbuf);
        if (tmp) kfree(tmp);
        return -3;
    }
    uint32_t ind_cached = 0xFFFFFFFFu, dind_cached = 0xFFFFFFFFu;
    int64_t  rc = -5;

    uint64_t copied = 0;
    while (copied < len) {
        uint64_t cur  = off + copied;
        uint32_t lb   = (uint32_t)(cur / bs);
        uint32_t boff = (uint32_t)(cur % bs);
        uint32_t phys = ext2_resolve_cached(&inode, lb, indbuf, &ind_cached,
                                            dindbuf, &dind_cached);
        if (phys == EXT2_BMAP_ERR) { rc = -4; goto rdone; }

        uint64_t remain = len - copied;
        if (boff != 0 || remain < bs) {
            // Unaligned leading fragment or a trailing partial block: read the
            // whole block into a temp, copy just the needed sub-range.
            uint32_t chunk = bs - boff;
            if ((uint64_t)chunk > remain) chunk = (uint32_t)remain;
            if (phys == 0) {
                memset(out + copied, 0, chunk);       // sparse hole
            } else {
                if (ext2_read_block(&g_ext2, phys, tmp) != 0) goto rdone;
                memcpy(out + copied, tmp + boff, chunk);
            }
            copied += chunk;
            continue;
        }
        if (phys == 0) { memset(out + copied, 0, bs); copied += bs; continue; }

        // Aligned full block(s): gather a physically-contiguous run and DMA it
        // straight into the destination in one transfer.
        uint32_t run = 1;
        while (copied + (uint64_t)(run + 1) * bs <= len &&
               (run + 1) * spb <= EXT2_DMA_MAX_SECTORS) {
            uint32_t nx = ext2_resolve_cached(&inode, lb + run, indbuf, &ind_cached,
                                              dindbuf, &dind_cached);
            if (nx != phys + run) break;
            run++;
        }
        if (blk_read(g_ext2.channel, g_ext2.drive, g_ext2.part_start_lba + phys * spb,
                     run * spb, out + copied) != (int)(run * spb)) {
            goto rdone;
        }
        copied += (uint64_t)run * bs;
    }
    rc = (int64_t)copied;

rdone:
    kfree(indbuf); kfree(dindbuf); kfree(tmp);
    return rc;
}

// ---------------------------------------------------------------------------
// Directory-block entry scan (#404 / #485 Phase C strangler seam).
//
// ext2_dirblock_find_c() is the inner per-block entry loop of ext2_lookup(),
// extracted VERBATIM (with the #476 OOB guards intact) into a stand-alone,
// state-free helper so it can be (a) shared, (b) mirrored 1:1 by a Rust port,
// and (c) differentially self-tested at boot. Returns 1 and fills *out_ino /
// *out_type if `name` (length name_len) is found in this directory block, 0
// otherwise. `ci` != 0 => case-insensitive compare (the caller passes
// g_root_ext2, exactly as the original inline loop tested it). `blk` must point
// to at least block_size readable bytes. The three guards below (header-fits,
// rec sane, name-fits) are the #476 hardening and MUST be preserved byte-for-
// byte: they are what the Rust drop-in mirrors and what the boot-time
// [RUST-DIFF] self-test checks equivalence against.
int ext2_dirblock_find_c(const uint8_t *blk, uint32_t block_size,
                         const char *name, uint32_t name_len,
                         int ci, uint32_t *out_ino, uint8_t *out_type) {
    uint32_t off = 0;
    // Bounds-check every disk-controlled field before use: the 8-byte entry
    // header must fit, rec_len must be sane (>= header size and within the
    // block), and the name must fit. A crafted/corrupt ext2 image otherwise
    // drives off past the block buffer (#476 OOB read).
    while (off + 8 <= block_size) {
        uint32_t e_ino  = rd32(blk + off + 0);
        uint16_t rec    = rd16(blk + off + 4);
        uint8_t  nlen   = blk[off + 6];
        uint8_t  ftype  = blk[off + 7];
        if (rec < 8 || off + rec > block_size) {
            break; // malformed rec_len; avoid OOB read / infinite loop
        }
        if (e_ino != 0 && nlen == name_len) {
            // The name field must lie within the block; reject otherwise
            // instead of reading past the buffer.
            if (off + 8 + (uint32_t)name_len > block_size) {
                break;
            }
            int match = 1;
            for (uint32_t i = 0; i < name_len; i++) {
                char a = (char)blk[off + 8 + i];
                char b = name[i];
                // Under ext2-root, match case-insensitively to mirror FAT
                // semantics (kernel/app paths mix case, e.g. /APPS vs /apps).
                if (ci) {
                    if (a >= 'a' && a <= 'z') a -= 32;
                    if (b >= 'a' && b <= 'z') b -= 32;
                }
                if (a != b) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                if (out_ino)  *out_ino  = e_ino;
                if (out_type) *out_type = ftype;
                return 1;
            }
        }
        off += rec;
    }
    return 0;
}

// Rust drop-in for ext2_dirblock_find_c (rustkern.rs, #404 / #485 Phase C).
// Same signature, same return contract; a slice built over exactly block_size
// bytes makes the OOB read class impossible by construction. `const char *`
// here and `*const u8` in Rust are ABI-identical (both a pointer); int == i32.
extern int ext2_dirblock_find_rs(const uint8_t *blk, uint32_t block_size,
                                 const char *name, uint32_t name_len,
                                 int ci, uint32_t *out_ino, uint8_t *out_type);

// Live strangler seam: ext2_lookup() calls ext2_dirblock_find(). With
// -DRUST_EXT2_DIRFIND (set in the Makefile CFLAGS for build 794), the real
// symbol routes to the Rust port; otherwise it falls straight back to the C.
// Rollback = drop the one flag and rebuild. The boot-time [RUST-DIFF] self-test
// (ext2_dir_rust_selftest) compares the two impls regardless of the flag.
int ext2_dirblock_find(const uint8_t *blk, uint32_t block_size,
                       const char *name, uint32_t name_len,
                       int ci, uint32_t *out_ino, uint8_t *out_type) {
#ifdef RUST_EXT2_DIRFIND
    return ext2_dirblock_find_rs(blk, block_size, name, name_len, ci, out_ino, out_type);
#else
    return ext2_dirblock_find_c(blk, block_size, name, name_len, ci, out_ino, out_type);
#endif
}

static int ext2_lookup_inner(uint32_t dir_ino, const char *name,
                             uint32_t *out_ino, uint8_t *out_type);

// #597: public entry -> take the filesystem lock. Recursive, so the write paths
// below (which all call ext2_lookup first) nest safely.
int ext2_lookup(uint32_t dir_ino, const char *name,
                uint32_t *out_ino, uint8_t *out_type) {
    ext2_lock();
    int r = ext2_lookup_inner(dir_ino, name, out_ino, out_type);
    ext2_unlock();
    return r;
}

static int ext2_lookup_inner(uint32_t dir_ino, const char *name,
                             uint32_t *out_ino, uint8_t *out_type) {
    if (!g_ext2.mounted) {
        return -1;
    }
    ext2_inode_t inode;
    if (ext2_read_inode(dir_ino, &inode) != 0) {
        return -2;
    }

    uint32_t name_len = (uint32_t)strlen(name);
    uint64_t size = inode.i_size;
    uint8_t *blk = (uint8_t *)kmalloc(g_ext2.block_size);
    if (!blk) {
        return -3;
    }

    uint64_t consumed = 0;
    uint32_t logical = 0;
    while (consumed < size) {
        uint32_t phys = ext2_bmap(&g_ext2, &inode, logical);
        if (phys == EXT2_BMAP_ERR) {
            kfree(blk);
            return -4;
        }
        if (phys != 0) {
            if (ext2_read_block(&g_ext2, phys, blk) != 0) {
                kfree(blk);
                return -5;
            }
            // Scan this directory block via the strangler seam (Rust under
            // -DRUST_EXT2_DIRFIND, else the #476-hardened C). ci = g_root_ext2,
            // exactly as the original inline loop tested it.
            if (ext2_dirblock_find(blk, g_ext2.block_size, name, name_len,
                                   g_root_ext2, out_ino, out_type)) {
                kfree(blk);
                return 0;
            }
        }
        consumed += g_ext2.block_size;
        logical  += 1;
    }

    kfree(blk);
    return 1; // not found
}

// ---------------------------------------------------------------------------
// Boot-time differential self-test for the ext2 directory-block parser
// (#404 / #485 Phase C). Asserts ext2_dirblock_find_rs == ext2_dirblock_find_c
// over WELL-FORMED and MALFORMED directory blocks (truncated header at the
// block tail, zero rec_len, rec_len < 8, oversized / boundary-straddling
// rec_len, name_len that straddles the block end) plus a large random-fuzz
// corpus, on THIS exact build, then logs ONE [RUST-DIFF] line to serial
// (kprintf) + the persistent /BOOTLOG.TXT (bootlog_write). Bounded, runs once
// at boot, no blocking (#426). Any mismatch => FAIL (reported; the golden fold
// stays human-gated). It always calls BOTH impls (and references
// ext2_dirblock_find_rs) so the Rust member is pulled into the link and the two
// are compared regardless of RUST_EXT2_DIRFIND.
#define EXT2DIFF_MAXBS 4096
static uint8_t ext2diff_blk[EXT2DIFF_MAXBS];

static inline uint32_t ext2diff_rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

// Run both impls on ext2diff_blk[0..bs] for (name,name_len,ci); compare the
// return value and, on a hit, the written out_ino/out_type. Bump counters.
static void ext2diff_one(uint32_t bs, const char *name, uint32_t name_len, int ci,
                         uint32_t *vectors, uint32_t *mismatches, int *first_bad) {
    uint32_t ino_c = 0xC0FFEE00u, ino_rs = 0xC0FFEE00u;
    uint8_t  ty_c  = 0xAB, ty_rs = 0xAB;
    int rc_c  = ext2_dirblock_find_c (ext2diff_blk, bs, name, name_len, ci, &ino_c,  &ty_c);
    int rc_rs = ext2_dirblock_find_rs(ext2diff_blk, bs, name, name_len, ci, &ino_rs, &ty_rs);
    (*vectors)++;
    int bad = (rc_c != rc_rs) || (rc_c == 1 && (ino_c != ino_rs || ty_c != ty_rs));
    if (bad) {
        (*mismatches)++;
        if (*first_bad < 0) *first_bad = (int)(*vectors);
    }
}

void ext2_dir_rust_selftest(void) {
    uint32_t seed = 0x9e3779b9u;
    uint32_t vectors = 0, mismatches = 0;
    int first_bad = -1;
    const uint32_t bslist[3] = {256u, 512u, 1024u};

    // Pass 1: WELL-FORMED blocks. Lay out random entries, then query every
    // present name (case-sensitive AND case-insensitive) plus random absent
    // names. This is what exercises the FOUND path so out_ino/out_type equality
    // is actually checked, not just the not-found return.
    for (int r = 0; r < 400; r++) {
        uint32_t bs = bslist[ext2diff_rng(&seed) % 3];
        memset(ext2diff_blk, 0, bs);
        char     names[16][16];
        int nent = 0;
        uint32_t off = 0;
        while (off + 12 <= bs && nent < 16) {
            uint32_t nl = 1 + (ext2diff_rng(&seed) % 10);
            uint32_t need = 8 + ((nl + 3u) & ~3u); // ext2 4-byte alignment
            if (off + need > bs) break;
            uint32_t ino = 1 + (ext2diff_rng(&seed) & 0x7fffffu);
            uint8_t  ty  = (uint8_t)(1 + (ext2diff_rng(&seed) % 7));
            for (uint32_t i = 0; i < nl; i++) {
                char c = (char)('a' + (ext2diff_rng(&seed) % 26));
                if (ext2diff_rng(&seed) & 1) c = (char)(c - 32); // mixed case
                names[nent][i] = c;
                ext2diff_blk[off + 8 + i] = (uint8_t)c;
            }
            names[nent][nl] = 0;
            ext2diff_blk[off + 0] = (uint8_t)(ino & 0xFF);
            ext2diff_blk[off + 1] = (uint8_t)((ino >> 8) & 0xFF);
            ext2diff_blk[off + 2] = (uint8_t)((ino >> 16) & 0xFF);
            ext2diff_blk[off + 3] = (uint8_t)((ino >> 24) & 0xFF);
            ext2diff_blk[off + 4] = (uint8_t)(need & 0xFF);
            ext2diff_blk[off + 5] = (uint8_t)((need >> 8) & 0xFF);
            ext2diff_blk[off + 6] = (uint8_t)nl;
            ext2diff_blk[off + 7] = ty;
            off += need;
            nent++;
        }
        for (int e = 0; e < nent; e++) {
            uint32_t nl = (uint32_t)strlen(names[e]);
            ext2diff_one(bs, names[e], nl, 0, &vectors, &mismatches, &first_bad);
            ext2diff_one(bs, names[e], nl, 1, &vectors, &mismatches, &first_bad);
        }
        // A few absent queries (random letters) both ways.
        for (int a = 0; a < 4; a++) {
            char q[16];
            uint32_t nl = 1 + (ext2diff_rng(&seed) % 12);
            for (uint32_t i = 0; i < nl; i++)
                q[i] = (char)('a' + (ext2diff_rng(&seed) % 26));
            q[nl] = 0;
            ext2diff_one(bs, q, nl, ext2diff_rng(&seed) & 1, &vectors, &mismatches, &first_bad);
        }
    }

    // Pass 2: MALFORMED / random fuzz. Fill the block with random bytes so every
    // disk-controlled rec_len / name_len / off takes adversarial values (zero
    // rec_len, rec_len < 8, oversized rec_len, boundary-straddling names,
    // truncated tail headers) and query a random name. Any guard divergence
    // between the C and Rust surfaces here.
    for (int n = 0; n < 24000; n++) {
        uint32_t bs = bslist[ext2diff_rng(&seed) % 3];
        for (uint32_t i = 0; i < bs; i++)
            ext2diff_blk[i] = (uint8_t)(ext2diff_rng(&seed) & 0xFF);
        char q[20];
        uint32_t nl = 1 + (ext2diff_rng(&seed) % 18);
        for (uint32_t i = 0; i < nl; i++)
            q[i] = (char)(ext2diff_rng(&seed) & 0xFF); // full byte range, incl >=0x80
        q[nl] = 0;
        ext2diff_one(bs, q, nl, ext2diff_rng(&seed) & 1, &vectors, &mismatches, &first_bad);
    }

    // Pass 3: explicit crafted malformed shapes (documents each guard class).
    {
        uint32_t bs = 1024;
        // (a) zero rec_len
        memset(ext2diff_blk, 0, bs);
        ext2diff_blk[0] = 5; ext2diff_blk[6] = 3; // e_ino=5, nlen=3, rec=0
        ext2diff_blk[8] = 'a'; ext2diff_blk[9] = 'b'; ext2diff_blk[10] = 'c';
        ext2diff_one(bs, "abc", 3, 0, &vectors, &mismatches, &first_bad);
        // (b) rec_len < 8
        memset(ext2diff_blk, 0, bs);
        ext2diff_blk[0] = 5; ext2diff_blk[4] = 5; ext2diff_blk[6] = 3;
        ext2diff_blk[8] = 'a'; ext2diff_blk[9] = 'b'; ext2diff_blk[10] = 'c';
        ext2diff_one(bs, "abc", 3, 0, &vectors, &mismatches, &first_bad);
        // (c) oversized rec_len (off+rec > bs)
        memset(ext2diff_blk, 0, bs);
        ext2diff_blk[0] = 5; ext2diff_blk[4] = 0xD0; ext2diff_blk[5] = 0x07; // rec=2000
        ext2diff_blk[6] = 3;
        ext2diff_blk[8] = 'a'; ext2diff_blk[9] = 'b'; ext2diff_blk[10] = 'c';
        ext2diff_one(bs, "abc", 3, 0, &vectors, &mismatches, &first_bad);
        // (d) name_len straddles block end (header fits, name does not)
        memset(ext2diff_blk, 0, bs);
        {
            uint32_t off = bs - 12; // 1012; off+8 = 1020 <= bs
            ext2diff_blk[off + 0] = 5;      // e_ino
            ext2diff_blk[off + 4] = 12;     // rec=12 -> off+rec=1024 <= bs (ok)
            ext2diff_blk[off + 6] = 10;     // nlen=10 -> off+8+10=1030 > bs
            for (int i = 0; i < 4; i++) ext2diff_blk[off + 8 + i] = 'x';
        }
        ext2diff_one(bs, "xxxxxxxxxx", 10, 0, &vectors, &mismatches, &first_bad);
        // (e) truncated header at the tail (off+8 > bs handled by loop guard)
        memset(ext2diff_blk, 0, bs);
        ext2diff_blk[0] = 5; ext2diff_blk[4] = (uint8_t)((bs - 4) & 0xFF);
        ext2diff_blk[5] = (uint8_t)(((bs - 4) >> 8) & 0xFF); ext2diff_blk[6] = 3;
        ext2diff_blk[8] = 'a'; ext2diff_blk[9] = 'b'; ext2diff_blk[10] = 'c';
        ext2diff_one(bs, "abc", 3, 0, &vectors, &mismatches, &first_bad);
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] ext2_dir: %u vectors, %u mismatches -> %s\n",
            vectors, mismatches, verdict);
    bootlog_write("[RUST-DIFF] ext2_dir: %u vectors, %u mismatches -> %s",
                  vectors, mismatches, verdict);
    if (mismatches != 0) {
        kprintf("[RUST-DIFF] ext2_dir FIRST MISMATCH at vector #%d\n", first_bad);
        bootlog_write("[RUST-DIFF] ext2_dir FIRST MISMATCH at vector #%d", first_bad);
    }
}

uint32_t ext2_resolve_path(const char *path) {
    if (!g_ext2.mounted || !path || path[0] != '/') {
        return 0;
    }

    uint32_t cur = EXT2_ROOT_INO;
    const char *p = path + 1; // skip leading '/'

    char component[256];
    while (*p) {
        // Extract next path component.
        uint32_t len = 0;
        while (*p && *p != '/') {
            if (len < sizeof(component) - 1) {
                component[len++] = *p;
            }
            p++;
        }
        component[len] = '\0';

        // Skip empty components (e.g. trailing slash or "//").
        if (len == 0) {
            if (*p == '/') p++;
            continue;
        }

        uint32_t next = 0;
        uint8_t  type = 0;
        if (ext2_lookup(cur, component, &next, &type) != 0) {
            return 0; // not found
        }
        cur = next;

        if (*p == '/') p++;
    }
    return cur;
}

// ---------------------------------------------------------------------------
// Self-test  (retargeted 2026-08-27, #EXT2SELFTEST)
// ---------------------------------------------------------------------------
//
// WHAT THIS WAS, AND WHY IT PROVED NOTHING
//
// Until this change ext2_selftest() opened with ext2_mount(0, 1, 0): a
// WHOLE-DISK ext2 volume on the primary IDE slave. That is the two-disk
// DEVELOPMENT layout. No shipping image has it. The product ships ONE disk
// carrying a GPT with a FAT ESP and an ext2 root at a non-zero base LBA, and
// that root is mounted several hundred lines later in main.c by the #365 block.
//
// MEASURED 2026-08-27 on golden build 2243 (<workspace>, booted
// under QEMU over the shipping USB-MSC path). The ENTIRE output of this
// function was three lines:
//
//     [EXT2] === ext2 read-only self-test (ch0, drive1) ===
//     [EXT2] mount: bad magic 0x45 (expected 0xef53)
//     [EXT2] mount FAILED (rc=-3). Aborting self-test.
//
// None of them reached /BOOTLOG.TXT, so on the two machines whose evidence
// matters (the owner's ASUS laptop and the iMac14,4, neither of which has a
// serial port) it produced literally nothing. Its diaglog-gate registration
// claimed eighteen lines. This is the subsystem whose block-layer write
// staging destroyed the owner's root filesystem on two machines earlier in the
// same session, so "the filesystem self-test has never run on the shipping
// layout" is not a tidy-up.
//
// Below the mount, the three read probes were pinned to fixtures that exist
// only on a hand-made dev image (/this-is-a-long-filename.txt,
// /big-indirect-test.dat, /subdir/note.md), each guarded by `if (ino) { ... }`
// with NO else. On a volume without them the function reached
// "=== self-test complete ===" having asserted nothing whatsoever.
//
// WHAT IT IS NOW
//
// 1. It tests THE MOUNTED ROOT, whatever that is. It performs no mount of its
//    own. main.c calls it after the #365 partition mount and after the
//    /ROOTEXT2 root cutover, so on a shipping image the volume under test is
//    the volume every app, every config file and every user's home lives on.
//    If nothing is mounted it says so through selftest_notrun(), which is loud,
//    durable and counted in the end-of-boot summary, instead of returning.
//
// 2. Every probe is DISCOVERED, not named. The read and indirect-block probes
//    walk the volume looking for a file of the shape they need. A fixture
//    cannot go missing because there is no fixture, and nothing has to be
//    declared in an asset manifest to keep this honest.
//
// 3. Absence is never silence. Where the volume genuinely cannot supply a
//    subject (an ext2 root holding no file big enough to reach an indirect
//    block) that arm calls selftest_notrun() with the reason. There is no path
//    through this function that asserts nothing and still reads like a pass:
//    the summary line carries the CHECK COUNT, and zero checks prints NOT-RUN.
//
// 4. It is durable and BOUNDED. /BOOTLOG.TXT is rewritten in full on every
//    bootlog_write() (an O(n^2) series over the slow USB-MSC stack, #373),
//    which is exactly why the diaglog-gate baseline deferred converting the old
//    eighteen lines. So the per-check detail stays on serial and only a summary
//    plus at most E2ST_LOG_FAILS failure lines go to the durable sink, inside
//    one bootlog_defer_begin()/end() window, i.e. one flush.
//
// THE CHECK THAT EARNS ITS PLACE. Check group ext2/backupsb reads the BACKUP
// superblock in block group 1 and compares five independently-stored fields
// against the primary. Nothing else in this kernel ever reads that copy, so a
// divergence between the two is invisible to every other code path, and silent
// divergence between two on-disk copies of the same fact is precisely the shape
// of the corruption that ate the root filesystem.

#define E2ST_SCAN_MAX   512u    // inode reads the whole subject scan may cost
#define E2ST_DIRS       8u      // subdirectories of / the scan may descend into
#define E2ST_LOG_FAILS  6u      // failure lines allowed into /BOOTLOG.TXT

static uint32_t g_e2st_checks;
static uint32_t g_e2st_fail;
static uint32_t g_e2st_logged;
static char     g_e2st_first[96];

// ONE assertion. Counted whether it passes or fails, which is the property the
// old probes lacked: there is no way to reach the summary without having moved
// this counter, so "0 checks" is a reportable state rather than a quiet pass.
static int e2st_check(int ok, const char *what) {
    g_e2st_checks++;
    if (ok) {
        kprintf("[EXT2-SELFTEST]   ok   %s\n", what);
        return 1;
    }
    g_e2st_fail++;
    kprintf("[EXT2-SELFTEST]   FAIL %s\n", what);
    if (g_e2st_logged < E2ST_LOG_FAILS) {
        g_e2st_logged++;
        bootlog_write("[EXT2-SELFTEST] FAIL %s", what);
    }
    if (!g_e2st_first[0]) {
        strncpy(g_e2st_first, what, sizeof(g_e2st_first) - 1);
        g_e2st_first[sizeof(g_e2st_first) - 1] = '\0';
    }
    return 0;
}

// ONE bounded walk of the volume that collects BOTH probe subjects at once:
// the first non-empty regular file (read path) and the largest regular file
// (indirect-block path). It looks in the root directory and, if the root does
// not supply what is needed, in up to E2ST_DIRS of the root's subdirectories.
//
// Discovery, not fixtures: the old probes named /this-is-a-long-filename.txt,
// /big-indirect-test.dat and /subdir/note.md, which ship on no image, so on
// every shipping machine all three did nothing and said nothing. Nothing named
// here can go missing because nothing is named.
//
// Bounded twice over (E2ST_SCAN_MAX inode reads in total, E2ST_DIRS
// directories) so a huge or corrupt tree cannot turn a boot self-test into a
// boot delay.
typedef struct {
    uint32_t small_ino;  uint64_t small_sz;  char small_nm[80];
    uint32_t big_ino;    uint64_t big_sz;    char big_nm[80];
    uint32_t reads;      uint32_t dirs;
} e2st_subjects_t;

typedef struct { uint32_t ino; char nm[48]; } e2st_dirent_t;

static e2st_subjects_t g_e2st_subj;

// dst = "<dir>/<name>", both bounded. No fixed path is spelled anywhere; this
// only exists so the log names the file that WAS chosen, which is what makes a
// discovered subject auditable after the fact.
static void e2st_join(char *dst, uint32_t cap, const char *dir, const char *name) {
    uint32_t l = 0;
    while (dir[l] && l + 1 < cap) { dst[l] = dir[l]; l++; }
    if (l + 1 < cap) dst[l++] = '/';
    uint32_t k = 0;
    while (name[k] && l + 1 < cap) { dst[l++] = name[k++]; }
    dst[l] = '\0';
}

static void e2st_scan_dir(uint32_t dir_ino, const char *dir_name,
                          e2st_subjects_t *s, e2st_dirent_t *subdirs,
                          uint32_t *nsubdirs, uint32_t subdir_max) {
    uint32_t pos = 0, ino = 0;
    uint8_t  type = 0;
    char nm[256];

    s->dirs++;
    while (s->reads < E2ST_SCAN_MAX &&
           ext2_readdir_ino(dir_ino, &pos, nm, (int)sizeof(nm), &ino, &type) == 0) {
        if (ino == 0) continue;
        if (type == EXT2_FT_DIR) {
            if (subdirs && nsubdirs && *nsubdirs < subdir_max) {
                subdirs[*nsubdirs].ino = ino;
                strncpy(subdirs[*nsubdirs].nm, nm, sizeof(subdirs[0].nm) - 1);
                subdirs[*nsubdirs].nm[sizeof(subdirs[0].nm) - 1] = '\0';
                (*nsubdirs)++;
            }
            continue;
        }
        if (type != EXT2_FT_REG_FILE) continue;
        ext2_inode_t in;
        s->reads++;
        if (ext2_read_inode(ino, &in) != 0) continue;
        uint64_t sz = in.i_size;
        if (sz == 0) continue;
        if (s->small_ino == 0) {
            s->small_ino = ino; s->small_sz = sz;
            e2st_join(s->small_nm, (uint32_t)sizeof(s->small_nm), dir_name, nm);
        }
        if (sz > s->big_sz) {
            s->big_ino = ino; s->big_sz = sz;
            e2st_join(s->big_nm, (uint32_t)sizeof(s->big_nm), dir_name, nm);
        }
    }
}

static void e2st_find_subjects(e2st_subjects_t *s) {
    e2st_dirent_t subdirs[E2ST_DIRS];
    uint32_t nsub = 0;

    memset(s, 0, sizeof(*s));
    memset(subdirs, 0, sizeof(subdirs));
    e2st_scan_dir(EXT2_ROOT_INO, "", s, subdirs, &nsub, E2ST_DIRS);

    // Descend only while something is still missing. On a shipping ext2 root
    // the files big enough to reach an indirect block live under /APPS and
    // /FONTS, not at the top level, so a root-only scan would have reported
    // "no subject" on exactly the layout this self-test exists to cover.
    for (uint32_t i = 0; i < nsub && s->reads < E2ST_SCAN_MAX; i++) {
        if (s->small_ino != 0 && s->big_sz > (uint64_t)g_ext2.block_size * 12) break;
        e2st_scan_dir(subdirs[i].ino, subdirs[i].nm, s, 0, 0, 0);
    }
}

// The whole root directory, walked as RAW BLOCKS rather than through
// ext2_readdir_ino(), because the point is to assert the on-disk record
// geometry (#476 / #597 shape) that the iterator is built to survive. Fills
// the four out-params; returns 0 if the inode could not be read.
static int e2st_walk_root(uint32_t *entries_out, uint32_t *geom_bad_out,
                          uint32_t *chain_bad_out, uint32_t *dot_bits_out) {
    ext2_inode_t root;
    uint32_t entries = 0, geom_bad = 0, chain_bad = 0, dot_bits = 0;

    *entries_out = 0; *geom_bad_out = 0; *chain_bad_out = 0; *dot_bits_out = 0;
    if (ext2_read_inode(EXT2_ROOT_INO, &root) != 0) return 0;

    uint8_t *blk = (uint8_t *)kmalloc(g_ext2.block_size);
    if (!blk) return 0;

    uint64_t size = root.i_size;
    uint32_t logical = 0;
    for (uint64_t consumed = 0; consumed < size; consumed += g_ext2.block_size, logical++) {
        uint32_t phys = ext2_bmap(&g_ext2, &root, logical);
        if (phys == 0 || phys == EXT2_BMAP_ERR ||
            ext2_read_block(&g_ext2, phys, blk) != 0) {
            chain_bad++;
            continue;
        }
        uint32_t off = 0;
        int walked_clean = 1;
        while (off + 8 <= g_ext2.block_size) {
            uint32_t e_ino = rd32(blk + off + 0);
            uint16_t rec   = rd16(blk + off + 4);
            uint8_t  nlen  = blk[off + 6];
            // ext2 requires rec_len >= 8, 4-byte aligned, inside the block, and
            // large enough for its own name. Any of these false means the block
            // is not walkable, which is the exact over-read #476 fixed.
            if (rec < 8 || (rec & 3u) != 0 || off + rec > g_ext2.block_size ||
                off + 8u + nlen > g_ext2.block_size || (uint32_t)8 + nlen > rec) {
                geom_bad++;
                walked_clean = 0;
                break;
            }
            if (e_ino != 0) {
                if (e_ino > g_ext2.inodes_count) geom_bad++;
                entries++;
                if (logical == 0) {
                    if (nlen == 1 && blk[off + 8] == '.' && e_ino == EXT2_ROOT_INO)
                        dot_bits |= 1u;
                    if (nlen == 2 && blk[off + 8] == '.' && blk[off + 9] == '.')
                        dot_bits |= 2u;
                }
            }
            off += rec;
        }
        // A well-formed directory block's rec_len chain lands EXACTLY on the
        // end of the block. Landing short or long means a record was lost or
        // invented, which no per-entry check can see on its own.
        if (walked_clean && off != g_ext2.block_size) chain_bad++;
    }
    kfree(blk);
    *entries_out = entries; *geom_bad_out = geom_bad;
    *chain_bad_out = chain_bad; *dot_bits_out = dot_bits;
    return 1;
}

void ext2_selftest(void) {
    g_e2st_checks = 0; g_e2st_fail = 0; g_e2st_logged = 0; g_e2st_first[0] = '\0';

    // One flush for the whole run rather than one per line (#373).
    bootlog_defer_begin();

    if (!ext2_is_mounted()) {
        // NOT a silent return, and NOT a kprintf that reads like boot noise.
        // On an ATA-only or FAT-only machine this is the honest answer and it
        // belongs in the end-of-boot summary; on a machine that is SUPPOSED to
        // have an ext2 root it is the loudest thing in the log.
        selftest_notrun("ext2/root",
                        "no ext2 volume is mounted at this point in boot, so "
                        "nothing about the root filesystem was verified");
        bootlog_defer_end();
        return;
    }

    kprintf("[EXT2-SELFTEST] === ext2 root self-test: ch%u drv%u base_lba=%u "
            "root=%s ===\n",
            (unsigned)g_ext2.channel, (unsigned)g_ext2.drive,
            (unsigned)g_ext2.part_start_lba,
            g_root_ext2 ? "ext2" : "fat");

    // ---- group: geometry -------------------------------------------------
    // Cross-checks between fields read from DIFFERENT offsets of the
    // superblock. A single corrupt field breaks at least one of these.
    {
        uint32_t bs = g_ext2.block_size;
        e2st_check(bs == 1024 || bs == 2048 || bs == 4096, "block_size is 1K/2K/4K");
        e2st_check(g_ext2.inode_size >= 128 && g_ext2.inode_size <= bs &&
                   (g_ext2.inode_size & (g_ext2.inode_size - 1)) == 0,
                   "inode_size is a power of two in [128, block_size]");
        e2st_check(g_ext2.inodes_count != 0 && g_ext2.blocks_count != 0,
                   "inodes_count and blocks_count are non-zero");
        e2st_check(g_ext2.blocks_per_group != 0 && g_ext2.inodes_per_group != 0,
                   "blocks_per_group and inodes_per_group are non-zero");
        e2st_check(g_ext2.first_data_block == (bs == 1024 ? 1u : 0u),
                   "first_data_block matches the block size");
        e2st_check(g_ext2.inodes_per_group <= g_ext2.inodes_count,
                   "inodes_per_group <= inodes_count");
        e2st_check((uint64_t)g_ext2.inodes_per_group * g_ext2.groups_count
                   >= (uint64_t)g_ext2.inodes_count,
                   "inodes_per_group * groups covers inodes_count");
        e2st_check((uint64_t)g_ext2.blocks_per_group * g_ext2.groups_count
                   >= (uint64_t)(g_ext2.blocks_count - g_ext2.first_data_block),
                   "blocks_per_group * groups covers blocks_count");
        e2st_check(g_ext2.bgd_table_block > g_ext2.first_data_block &&
                   g_ext2.bgd_table_block < g_ext2.blocks_count,
                   "group descriptor table lies inside the volume");
        // #365: the field this whole retarget is about. A partition never
        // begins inside the protective MBR + GPT header + entry array.
        e2st_check(g_ext2.part_start_lba == 0 || g_ext2.part_start_lba >= 34,
                   "part_start_lba is 0 (whole disk) or past the GPT header");
        uint64_t endb = ext2_end_bytes();
        e2st_check(endb > (uint64_t)g_ext2.part_start_lba * 512,
                   "volume end lies past the partition base");
        e2st_check(endb == (uint64_t)g_ext2.part_start_lba * 512 +
                           (uint64_t)g_ext2.blocks_count * g_ext2.block_size,
                   "volume end equals base + blocks_count * block_size");
        selftest_ran("ext2/geometry");
    }

    // ---- group: backup superblock ---------------------------------------
    // The one check nothing else in this kernel can make. Block group 1 always
    // carries a backup superblock (sparse_super or not), and NO other code path
    // in this tree ever reads it, so a divergence from the primary is invisible
    // everywhere else. Two on-disk copies of the same fact disagreeing is the
    // signature of a block-layer write landing where it should not.
    if (g_ext2.groups_count >= 2) {
        uint32_t sbblk = g_ext2.first_data_block + g_ext2.blocks_per_group;
        uint8_t *b = (uint8_t *)kmalloc(g_ext2.block_size);
        if (!b) {
            selftest_notrun("ext2/backupsb",
                            "kmalloc of one block failed, so the backup "
                            "superblock could not be compared to the primary");
        } else if (sbblk >= g_ext2.blocks_count ||
                   ext2_read_block(&g_ext2, sbblk, b) != 0) {
            selftest_notrun("ext2/backupsb",
                            "the group-1 backup superblock block could not be "
                            "read from this volume");
            kfree(b);
        } else {
            e2st_check(rd16(b + 56) == 0xEF53, "backup superblock magic is 0xEF53");
            e2st_check(rd32(b + 0) == g_ext2.inodes_count,
                       "backup inodes_count matches the primary");
            e2st_check(rd32(b + 4) == g_ext2.blocks_count,
                       "backup blocks_count matches the primary");
            e2st_check(rd32(b + 32) == g_ext2.blocks_per_group,
                       "backup blocks_per_group matches the primary");
            e2st_check(rd32(b + 40) == g_ext2.inodes_per_group,
                       "backup inodes_per_group matches the primary");
            kfree(b);
            selftest_ran("ext2/backupsb");
        }
    } else {
        selftest_notrun("ext2/backupsb",
                        "this volume has only one block group, so there is no "
                        "backup superblock to compare the primary against");
    }

    // ---- group: root inode + directory geometry --------------------------
    {
        ext2_inode_t root;
        int got = (ext2_read_inode(EXT2_ROOT_INO, &root) == 0);
        e2st_check(got, "root inode 2 is readable");
        if (got) {
            e2st_check((root.i_mode & 0xF000u) == 0x4000u, "root inode is a directory");
            e2st_check(root.i_links_count >= 2, "root inode link count >= 2");
            e2st_check(root.i_size >= g_ext2.block_size &&
                       (root.i_size % g_ext2.block_size) == 0,
                       "root directory size is a non-zero multiple of block_size");
        }

        uint32_t entries = 0, geom_bad = 0, chain_bad = 0, dot_bits = 0;
        if (e2st_walk_root(&entries, &geom_bad, &chain_bad, &dot_bits)) {
            kprintf("[EXT2-SELFTEST]   root dir: %u entries, %u bad records, "
                    "%u bad block chains\n",
                    (unsigned)entries, (unsigned)geom_bad, (unsigned)chain_bad);
            e2st_check(geom_bad == 0, "every root directory record is walkable");
            e2st_check(chain_bad == 0,
                       "every root directory block's rec_len chain lands on the block end");
            e2st_check((dot_bits & 1u) != 0, "root directory holds '.' pointing at inode 2");
            e2st_check((dot_bits & 2u) != 0, "root directory holds '..'");
            e2st_check(entries >= 1, "root directory is not empty");
            selftest_ran("ext2/rootdir");
        } else {
            selftest_notrun("ext2/rootdir",
                            "the root inode or a scratch block could not be "
                            "read, so directory record geometry was not checked");
        }
    }

    // ---- group: resolver bounds -----------------------------------------
    // Absence must be PROVABLE, not assumed. A resolver that invents an inode
    // for a path that is not there is the same defect class as a probe with no
    // else: it cannot report the thing it exists to report.
    {
        e2st_check(ext2_resolve_path("/") == EXT2_ROOT_INO, "'/' resolves to inode 2");
        e2st_check(ext2_resolve_path("/__maytera_ext2_selftest_absent__") == 0,
                   "an absent path resolves to 0");
        e2st_check(ext2_resolve_path("bad-relative-path") == 0,
                   "a relative path is refused");
        ext2_inode_t tmp;
        e2st_check(ext2_read_inode(0, &tmp) != 0, "inode 0 is refused");
        e2st_check(ext2_read_inode(g_ext2.inodes_count + 1, &tmp) != 0,
                   "an inode past inodes_count is refused");
        selftest_ran("ext2/bounds");
    }

    // ONE walk feeds both remaining groups.
    e2st_find_subjects(&g_e2st_subj);
    kprintf("[EXT2-SELFTEST]   subject scan: %u inode read(s) across %u "
            "director(ies)\n",
            (unsigned)g_e2st_subj.reads, (unsigned)g_e2st_subj.dirs);

    // ---- group: read path ------------------------------------------------
    // DISCOVERED subject, not a fixture. The two independent readers
    // (whole-file and range) must agree byte for byte; a staging or cache bug
    // that serves one of them stale bytes shows up here and nowhere else at
    // boot.
    {
        uint32_t ino = g_e2st_subj.small_ino;
        uint64_t sz  = g_e2st_subj.small_sz;
        const char *nm = g_e2st_subj.small_nm;
        if (!ino) {
            selftest_notrun("ext2/read",
                            "this volume holds no regular file with a non-zero "
                            "size within the scan bound, so no read path was "
                            "exercised on this boot");
        } else {
            uint64_t cap = sz < 4096 ? sz : 4096;
            uint8_t *a = (uint8_t *)kmalloc((unsigned long)cap);
            uint8_t *b = (uint8_t *)kmalloc((unsigned long)cap);
            if (!a || !b) {
                selftest_notrun("ext2/read",
                                "kmalloc of two read buffers failed, so the "
                                "whole-file and range readers were not compared");
            } else {
                kprintf("[EXT2-SELFTEST]   read subject '%s' ino=%u size=%lu\n",
                        nm, (unsigned)ino, (unsigned long)sz);
                int64_t n1 = ext2_read_file_ino(ino, a, cap);
                e2st_check(n1 == (int64_t)cap,
                           "whole-file read returns the requested byte count");
                int64_t n2 = ext2_read_file_range(ino, 0, cap, b);
                e2st_check(n2 == (int64_t)cap,
                           "range read returns the requested byte count");
                int same = (n1 == n2 && n1 > 0);
                if (same) {
                    for (int64_t i = 0; i < n1; i++) {
                        if (a[i] != b[i]) { same = 0; break; }
                    }
                }
                e2st_check(same, "whole-file and range readers agree byte for byte");
                // Re-read the same range. Two identical reads of an unchanging
                // file must be identical; they were not, on the machines that
                // lost a filesystem to write staging.
                int64_t n3 = ext2_read_file_range(ino, 0, cap, b);
                int stable = (n3 == n1);
                if (stable) {
                    for (int64_t i = 0; i < n1; i++) {
                        if (a[i] != b[i]) { stable = 0; break; }
                    }
                }
                e2st_check(stable, "the same range read twice returns the same bytes");
                e2st_check(ext2_read_file_range(ino, sz, 1, b) == 0,
                           "a read starting at EOF returns 0 bytes");
                selftest_ran("ext2/read");
            }
            if (a) kfree(a);
            if (b) kfree(b);
        }
    }

    // ---- group: indirect blocks -----------------------------------------
    // Replaces /big-indirect-test.dat. Any file larger than 12 blocks reaches
    // the singly-indirect pointer; the LAST block of the largest file on the
    // volume usually reaches the doubly-indirect one. Both are discovered.
    {
        uint32_t bs = g_ext2.block_size;
        uint64_t sz  = g_e2st_subj.big_sz;
        uint32_t ino = (sz > (uint64_t)bs * 12) ? g_e2st_subj.big_ino : 0;
        const char *nm = g_e2st_subj.big_nm;
        if (!ino) {
            // A REPORTED absence, not a silent one: it means this boot did not
            // exercise indirect block mapping at all.
            selftest_notrun("ext2/indirect",
                            "no regular file found within the scan bound exceeds "
                            "12 blocks, so indirect block mapping was not "
                            "exercised on this boot");
        } else {
            uint8_t *b = (uint8_t *)kmalloc(bs);
            ext2_inode_t in;
            if (!b || ext2_read_inode(ino, &in) != 0) {
                selftest_notrun("ext2/indirect",
                                "the discovered large file's inode or a scratch "
                                "block could not be read");
            } else {
                kprintf("[EXT2-SELFTEST]   indirect subject '%s' ino=%u size=%lu "
                        "(%lu blocks)\n", nm, (unsigned)ino, (unsigned long)sz,
                        (unsigned long)((sz + bs - 1) / bs));
                uint32_t last_l = (uint32_t)((sz - 1) / bs);
                uint32_t m12 = ext2_bmap(&g_ext2, &in, 12);
                uint32_t mlast = ext2_bmap(&g_ext2, &in, last_l);
                e2st_check(m12 != 0 && m12 != EXT2_BMAP_ERR,
                           "logical block 12 maps through the singly-indirect pointer");
                e2st_check(mlast != 0 && mlast != EXT2_BMAP_ERR,
                           "the file's last logical block maps to a real block");
                e2st_check(ext2_read_file_range(ino, (uint64_t)bs * 12, bs, b)
                           == (int64_t)bs,
                           "a full block read at logical block 12 returns block_size bytes");
                uint64_t tail_off = (uint64_t)last_l * bs;
                e2st_check(ext2_read_file_range(ino, tail_off, bs, b)
                           == (int64_t)(sz - tail_off),
                           "the tail read returns exactly the remaining bytes");
                selftest_ran("ext2/indirect");
            }
            if (b) kfree(b);
        }
    }

    // ---- verdict ---------------------------------------------------------
    // ONE durable line, and it carries the check count. A run that asserted
    // nothing cannot print a pass: zero checks is reported as NOT-RUN through
    // the same register every other declining group uses.
    if (g_e2st_checks == 0) {
        selftest_notrun("ext2/root",
                        "the self-test reached its verdict having made zero "
                        "assertions about the mounted volume");
    } else {
        const char *verdict = (g_e2st_fail == 0) ? "PASS" : "FAIL";
        kprintf("[EXT2-SELFTEST] %s %u/%u checks (base_lba=%u bs=%u groups=%u root=%s)%s%s\n",
                verdict, (unsigned)(g_e2st_checks - g_e2st_fail),
                (unsigned)g_e2st_checks, (unsigned)g_ext2.part_start_lba,
                (unsigned)g_ext2.block_size, (unsigned)g_ext2.groups_count,
                g_root_ext2 ? "ext2" : "fat",
                g_e2st_fail ? " first=" : "", g_e2st_fail ? g_e2st_first : "");
        bootlog_write("[EXT2-SELFTEST] %s %u/%u checks base_lba=%u bs=%u groups=%u root=%s%s%s",
                      verdict, (unsigned)(g_e2st_checks - g_e2st_fail),
                      (unsigned)g_e2st_checks, (unsigned)g_ext2.part_start_lba,
                      (unsigned)g_ext2.block_size, (unsigned)g_ext2.groups_count,
                      g_root_ext2 ? "ext2" : "fat",
                      g_e2st_fail ? " first=" : "", g_e2st_fail ? g_e2st_first : "");
        if (g_e2st_fail) {
            // A failing filesystem self-test is a fact the NEXT boot should act
            // on, so it goes into the superblock the same way a bad directory
            // record does. #610's boot gate then schedules a full check.
            ext2_mark_error("ext2 boot self-test reported a failed check");
        }
    }

    bootlog_defer_end();
}

// ===========================================================================
// WRITE SUPPORT (#99 Phase A). On-disk bookkeeping: block/inode bitmaps,
// inode write-back, block allocation (direct + single-indirect), directory
// entry insertion, and file/dir creation. Updates the PRIMARY superblock +
// primary block-group descriptor free counts to stay consistent with bitmaps.
// Target geometry: 1KB blocks, 256B inodes, filetype dir entries.
// ===========================================================================
extern int ata_write_sectors(uint8_t channel, uint8_t drive, uint32_t lba,
                             uint8_t count, void *buffer);

// Forward declarations for the delete/overwrite path (#99 Phase C prereq).
static void ext2_free_block(ext2_fs_t *fs, uint32_t blk);
static void ext2_free_inode(ext2_fs_t *fs, uint32_t ino, int is_dir);
static void ext2_truncate_inode(ext2_fs_t *fs, uint8_t *ri);
static void ext2_rollback_inode(ext2_fs_t *fs, uint32_t ino, int free_ino);
static int  ext2_write_data_to_inode(ext2_fs_t *fs, uint32_t ino,
                                      const void *data, uint32_t len);

// #695 Phase 0. Why the failure CAUSE needs a side channel: the allocators
// (ext2_alloc_block, ext2_alloc_inode, ext2_inode_append_block) return a block
// or inode NUMBER, so 0 is the only value left to mean "failed". They have
// nowhere to put "full" versus "the device rejected the write", which is why
// the public entry points could not tell those apart and merged both into -3.
// Rather than widen those signatures, the failing site records the cause here
// and the public entry point consumes it. Set and read under ext2_lock(), like
// the rest of the mutable state in this file. FIRST error wins, so a cascade of
// follow-on failures cannot mask the root cause.
static int g_e2_werr;
static inline void e2_werr_clear(void) { g_e2_werr = 0; }
static inline void e2_werr_set(int e)  { if (!g_e2_werr) g_e2_werr = e; }
static inline int  e2_werr_take(int dflt) {
    int e = g_e2_werr; g_e2_werr = 0; return e ? e : dflt;
}

static int ext2_write_block(const ext2_fs_t *fs, uint32_t block, const void *buf) {
    uint32_t spb = fs->block_size / EXT2_SECTOR_SIZE;
    int r = blk_write(fs->channel, fs->drive, fs->part_start_lba + block * spb,
                             spb, buf);
    if (r != (int)spb) {
        // #695: record the cause HERE, at the one place every ext2 write passes
        // through, rather than at each of the 22 call sites. Several callers
        // (ext2_inode_append_block and friends) DID check this return but then
        // reported failure as a bare 0, which the entry point could only read
        // as "allocation failed" and mapped to ENOSPC. A failed device write is
        // never ENOSPC, and first-error-wins means this genuine cause survives
        // any follow-on allocation failure it goes on to provoke.
        e2_werr_set(EXT2_E_IO);
        return -1;
    }
    ext2_cache_update(block, buf, fs->block_size);   // keep cache coherent
    return 0;
}

// #742: the accounting-write failure sink.
//
// ext2_sb_adjust() and ext2_bgd_adjust() update the FREE-COUNT metadata after a
// bitmap change has ALREADY landed. When one of them fails there is nothing the
// call site can undo: re-writing the bitmap would go to the same failing device,
// and leaving the bitmap alone is the lesser evil anyway (a block marked in use
// that nobody owns is recoverable; a block marked free that a file still points
// at is not). So the correct response is not a rollback, it is to make the
// divergence DISCOVERABLE, on two channels that already exist:
//
//   1. e2_werr, so the ext2_write_file()/ext2_mkdir()/... entry point returns
//      EXT2_E_IO to its caller instead of 0, and
//   2. ext2_mark_error(), which sets EXT2_ERROR_FS in the superblock so the
//      NEXT boot and any Linux host actually run a check.
//
// Before this, all 12 call sites discarded the return, ext2_sb_adjust() wrote
// the superblock with a RAW blk_write() that never touched e2_werr, and the
// result was the single most invisible corruption this driver can produce: an
// on-disk free count that disagrees with the bitmap, with nothing anywhere
// saying so.
// ext2_mark_error() is already forward-declared near the top of this file.
static void ext2_acct_failed(const char *what) {
    e2_werr_set(EXT2_E_IO);
    kprintf("[EXT2] free-count accounting write FAILED (%s): the on-disk free"
            " counts now disagree with the bitmaps\n", what);
    ext2_mark_error(what);
}

// Adjust primary superblock free-block / free-inode counts (signed deltas).
// Returns 0 only if the superblock reached the medium.
static MUST_CHECK int ext2_sb_adjust(ext2_fs_t *fs, int32_t dblocks, int32_t dinodes) {
    uint8_t *sb = (uint8_t *)kmalloc(1024);
    if (!sb) { e2_werr_set(EXT2_E_NOMEM); return -1; }
    if (blk_read(fs->channel, fs->drive, fs->part_start_lba + 2, 2, sb) != 2) { kfree(sb); e2_werr_set(EXT2_E_IO); return -1; }
    uint32_t fb = rd32(sb + 12), fi = rd32(sb + 16);
    fb = (uint32_t)((int32_t)fb + dblocks);
    fi = (uint32_t)((int32_t)fi + dinodes);
    memcpy(sb + 12, &fb, 4); memcpy(sb + 16, &fi, 4);
    // #742: this is the ONE ext2 write that does not go through
    // ext2_write_block(), so it is also the one that never recorded its own
    // failure. Record it here, at the same place ext2_write_block() does.
    int rc = (blk_write(fs->channel, fs->drive, fs->part_start_lba + 2, 2, sb) == 2) ? 0 : -1;
    if (rc != 0) e2_werr_set(EXT2_E_IO);
    kfree(sb);
    return rc;
}

// Read a 32-bit field from a block group descriptor (off within the 32-byte desc).
static uint32_t ext2_bgd_get32(ext2_fs_t *fs, uint32_t group, int off) {
    uint32_t dpb = fs->block_size / 32;
    uint32_t block = fs->bgd_table_block + group / dpb;
    uint32_t idx = group % dpb;
    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) return 0;
    if (ext2_read_block(fs, block, blk) != 0) { kfree(blk); return 0; }
    uint32_t v = rd32(blk + idx * 32 + off);
    kfree(blk);
    return v;
}

// Adjust a block group descriptor's free-block / free-inode / used-dir counts.
// Returns 0 only if the descriptor block reached the medium.
static MUST_CHECK int ext2_bgd_adjust(ext2_fs_t *fs, uint32_t group, int32_t db, int32_t di, int32_t dd) {
    uint32_t dpb = fs->block_size / 32;
    uint32_t block = fs->bgd_table_block + group / dpb;
    uint32_t idx = group % dpb;
    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) return -1;
    if (ext2_read_block(fs, block, blk) != 0) { kfree(blk); return -1; }
    uint8_t *d = blk + idx * 32;
    uint16_t fbc = rd16(d + 12), fic = rd16(d + 14), ud = rd16(d + 16);
    fbc = (uint16_t)(fbc + db); fic = (uint16_t)(fic + di); ud = (uint16_t)(ud + dd);
    memcpy(d + 12, &fbc, 2); memcpy(d + 14, &fic, 2); memcpy(d + 16, &ud, 2);
    int rc = ext2_write_block(fs, block, blk);
    kfree(blk);
    return rc;
}

// Allocate a free data block (zeroed). Returns block number or 0 on failure.
// Allocate a free block. When `zero` is set the new block is zeroed on disk
// (required for indirect-pointer blocks, which are read before being fully
// populated). Data blocks pass zero=0: the caller always overwrites the whole
// block, so the extra zero-write would be pure waste (it roughly doubled the
// write cost of large files).
static uint32_t ext2_alloc_block(ext2_fs_t *fs, int zero) {
    for (uint32_t g = 0; g < fs->groups_count; g++) {
        uint32_t bbmp = ext2_bgd_get32(fs, g, 0);  // bg_block_bitmap
        if (!bbmp) continue;
        uint8_t *bm = (uint8_t *)kmalloc(fs->block_size);
        if (!bm) return 0;
        if (ext2_read_block(fs, bbmp, bm) != 0) { kfree(bm); continue; }
        for (uint32_t i = 0; i < fs->blocks_per_group; i++) {
            if (!(bm[i / 8] & (1 << (i % 8)))) {
                uint32_t blkno = fs->first_data_block + g * fs->blocks_per_group + i;
                if (blkno >= fs->blocks_count) { kfree(bm); return 0; }
                bm[i / 8] |= (uint8_t)(1 << (i % 8));
                if (ext2_write_block(fs, bbmp, bm) != 0) {
                    kfree(bm); e2_werr_set(EXT2_E_IO); return 0;
                }
                kfree(bm);
                if (ext2_bgd_adjust(fs, g, -1, 0, 0) != 0) ext2_acct_failed("bgd/alloc_block");
                if (ext2_sb_adjust(fs, -1, 0)        != 0) ext2_acct_failed("sb/alloc_block");
                if (zero) {
                    // #695: EVERY ext2_alloc_block(fs, 1) caller uses the block
                    // as an INDIRECT block and depends on it reading back as
                    // all zeroes (see the "zeroed by ext2_alloc_block" comment
                    // at the double-indirect call site). This write was
                    // unchecked, AND a failed kmalloc skipped the zeroing
                    // entirely, so a recycled block could be handed back still
                    // holding a previous file's block pointers. Published as an
                    // indirect block, those stale pointers invent data blocks
                    // out of unrelated bytes and cross-link two files. Undo the
                    // allocation instead of handing back a dirty block.
                    uint8_t *z = (uint8_t *)kmalloc(fs->block_size);
                    int zrc = z ? 0 : EXT2_E_NOMEM;
                    if (z) {
                        memset(z, 0, fs->block_size);
                        if (ext2_write_block(fs, blkno, z) != 0) zrc = EXT2_E_IO;
                        kfree(z);
                    }
                    if (zrc != 0) {
                        ext2_free_block(fs, blkno);  // restores bit + counters
                        e2_werr_set(zrc);
                        return 0;
                    }
                }
                return blkno;
            }
        }
        kfree(bm);
    }
    e2_werr_set(EXT2_E_NOSPC);   // #695: every group scanned, no free block
    return 0;
}

// Allocate a free inode. Returns inode number or 0 on failure.
static uint32_t ext2_alloc_inode(ext2_fs_t *fs, int is_dir) {
    for (uint32_t g = 0; g < fs->groups_count; g++) {
        uint32_t ibmp = ext2_bgd_get32(fs, g, 4);  // bg_inode_bitmap
        if (!ibmp) continue;
        uint8_t *bm = (uint8_t *)kmalloc(fs->block_size);
        if (!bm) return 0;
        if (ext2_read_block(fs, ibmp, bm) != 0) { kfree(bm); continue; }
        for (uint32_t i = 0; i < fs->inodes_per_group; i++) {
            if (!(bm[i / 8] & (1 << (i % 8)))) {
                uint32_t ino = g * fs->inodes_per_group + i + 1;
                bm[i / 8] |= (uint8_t)(1 << (i % 8));
                if (ext2_write_block(fs, ibmp, bm) != 0) {
                    kfree(bm); e2_werr_set(EXT2_E_IO); return 0;   // #695
                }
                kfree(bm);
                if (ext2_bgd_adjust(fs, g, 0, -1, is_dir ? 1 : 0) != 0) ext2_acct_failed("bgd/alloc_inode");
                if (ext2_sb_adjust(fs, 0, -1)                     != 0) ext2_acct_failed("sb/alloc_inode");
                return ino;
            }
        }
        kfree(bm);
    }
    e2_werr_set(EXT2_E_NOSPC);   // #695: every group scanned, no free inode
    return 0;
}

// Raw inode read/write (inode_size bytes) for read-modify-write of any field.
// #742: MUST_CHECK. write=1 is an inode WRITE-BACK, and a lost one is not a
// cosmetic accounting slip: in the rollback path it leaves the inode pointing
// at blocks that were just freed (a cross-linked filesystem), and in the
// unlink/rmdir paths it leaves i_links_count non-zero and i_dtime zero on an
// inode the bitmap now says is free, which is exactly the "deleted inode has
// zero dtime" e2fsck reports.
static MUST_CHECK int ext2_inode_raw(ext2_fs_t *fs, uint32_t ino, uint8_t *buf, int write) {
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;
    if (group >= fs->groups_count) return -1;
    uint32_t itable = 0;
    if (ext2_group_inode_table(fs, group, &itable) != 0) return -1;
    uint64_t byte_off = (uint64_t)itable * fs->block_size + (uint64_t)index * fs->inode_size;
    uint32_t blk_of = (uint32_t)(byte_off / fs->block_size);
    uint32_t off    = (uint32_t)(byte_off % fs->block_size);
    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) return -1;
    if (ext2_read_block(fs, blk_of, blk) != 0) { kfree(blk); return -1; }
    if (write) { memcpy(blk + off, buf, fs->inode_size); int rc = ext2_write_block(fs, blk_of, blk); kfree(blk); return rc; }
    memcpy(buf, blk + off, fs->inode_size);
    kfree(blk);
    return 0;
}

static inline uint32_t e2_round4(uint32_t n) { return (n + 3) & ~3u; }

// Append a freshly-allocated data block as the next logical block of a file/dir
// inode (direct + single-indirect). Updates i_block[]/i_blocks via raw inode.
// Returns the new physical block number, or 0 on failure.
// #597 POPULATE-BEFORE-PUBLISH. `init` (optional, block_size bytes) is written
// into the freshly allocated block BEFORE any on-disk pointer references it, and
// `size_add` is folded into i_size in the SAME inode write that installs the
// pointer. ext2_dir_add() previously linked the block first and grew i_size in a
// SECOND, best-effort inode read-modify-write two blocking I/Os later; anything
// that failed or interleaved in between left a directory whose i_size did not
// cover its last block (the entry is then invisible: an orphan inode with
// links=1). Callers that fill the block themselves afterwards pass (0, 0) and
// get byte-identical behavior to before.
// ---------------------------------------------------------------------------
// #609 RUN APPEND: one metadata transaction per RUN of blocks, not per block.
//
// MEASURED PROBLEM (tools/ext2-harness, real 103,563,185-byte package on a copy
// of the golden-937 ext2 root): appending it one 4KB block at a time cost
// 151,826 blk_write() calls / 1,062,742 sectors = 544 MB of device traffic for
// 103 MB of payload. 6.005 device writes per data block, because EVERY block
// re-wrote the block bitmap, the group descriptor, the superblock free count,
// the indirect pointer block and the inode before writing the data. On a
// USB-MSC or ATA root each of those is a real device round trip, and ext2_lock
// is held across all of them.
//
// This path allocates a CONTIGUOUS RUN of free blocks (one bitmap write, one
// group-descriptor update, one superblock update), writes the payload with
// large multi-block transfers, publishes the pointers with ONE pointer-block
// write and ONE inode write. Same on-disk result, ~30x fewer transactions.
//
// The two pure computations it needs (find/claim a bitmap run; map a logical
// block to its pointer slot and report how far the same pointer block reaches)
// live in Rust: rustkern/ext2extent.rs. See the Rust-first mandate; they are
// index arithmetic over untrusted on-disk bytes with no I/O.
typedef struct { uint32_t start; uint32_t len; } ext2_run_t;
typedef struct { uint32_t kind; uint32_t slot; uint32_t inner; uint32_t same_ptr_run; } ext2_lblkmap_t;
_Static_assert(sizeof(ext2_run_t) == 8,     "#609 FFI: Ext2Run is two u32");
_Static_assert(sizeof(ext2_lblkmap_t) == 16, "#609 FFI: Ext2LblkMap is four u32");
_Static_assert(sizeof(uint32_t) == 4,        "#609 FFI: u32 width");
extern int ext2_bitmap_find_run_rs(uint8_t *bm, uint32_t bm_len, uint32_t bits,
                                   uint32_t hint, uint32_t max_run, ext2_run_t *out);
extern int ext2_lblk_map_rs(uint32_t logical, uint32_t ptrs, ext2_lblkmap_t *out);

// Rover so a long streaming write does not re-scan every full group from 0 on
// each allocation (the old allocator did, once group 0 filled).
static uint32_t g_alloc_group_hint = 0;

// Allocate up to `want` CONSECUTIVE free data blocks. Returns the first
// physical block (0 = failure) and stores the run length in *got. Blocks are
// NOT zeroed: the caller always overwrites them completely.
static uint32_t ext2_alloc_run(ext2_fs_t *fs, uint32_t want, uint32_t *got) {
    *got = 0;
    if (want == 0 || fs->groups_count == 0) return 0;
    for (uint32_t n = 0; n < fs->groups_count; n++) {
        uint32_t g = (g_alloc_group_hint + n) % fs->groups_count;
        uint32_t bbmp = ext2_bgd_get32(fs, g, 0);   // bg_block_bitmap
        if (!bbmp) continue;
        uint8_t *bm = (uint8_t *)kmalloc(fs->block_size);
        if (!bm) return 0;
        if (ext2_read_block(fs, bbmp, bm) != 0) { kfree(bm); continue; }
        // Never hand out a bit past the end of the volume (the last group is
        // short, and its padding bits are not always set on disk).
        uint64_t gbase = (uint64_t)fs->first_data_block + (uint64_t)g * fs->blocks_per_group;
        uint32_t bits = fs->blocks_per_group;
        if (gbase >= fs->blocks_count) bits = 0;
        else if (gbase + bits > fs->blocks_count) bits = (uint32_t)(fs->blocks_count - gbase);
        ext2_run_t r; r.start = 0; r.len = 0;
        if (bits == 0 ||
            ext2_bitmap_find_run_rs(bm, fs->block_size, bits, 0, want, &r) != 1 ||
            r.len == 0) {
            kfree(bm);
            continue;
        }
        if (ext2_write_block(fs, bbmp, bm) != 0) { kfree(bm); return 0; }
        kfree(bm);
        if (ext2_bgd_adjust(fs, g, -(int32_t)r.len, 0, 0) != 0) ext2_acct_failed("bgd/alloc_run");
        if (ext2_sb_adjust(fs, -(int32_t)r.len, 0)        != 0) ext2_acct_failed("sb/alloc_run");
        g_alloc_group_hint = g;
        *got = r.len;
        return (uint32_t)(gbase + r.start);
    }
    return 0;
}

// Append up to `nblocks` consecutive logical blocks starting at `logical`,
// filled from `data` (nblocks * block_size bytes). Returns how many were
// appended (0 = failure); the caller loops for the remainder. `size_add` is
// folded into i_size by the SAME inode write that publishes the pointers
// (#597 populate-before-publish, preserved).
static uint32_t ext2_inode_append_run(ext2_fs_t *fs, uint32_t ino, uint32_t logical,
                                      const void *data, uint32_t nblocks, uint32_t size_add) {
    uint32_t bs = fs->block_size, ptrs = bs / 4, spb = bs / EXT2_SECTOR_SIZE;
    if (nblocks == 0 || spb == 0) return 0;
    ext2_lblkmap_t m;
    if (ext2_lblk_map_rs(logical, ptrs, &m) != 1 || m.kind == 3) return 0;
    if (nblocks > m.same_ptr_run) nblocks = m.same_ptr_run;   // one pointer block

    uint8_t *ribuf = (uint8_t *)kmalloc(fs->inode_size);
    if (!ribuf) return 0;
    if (ext2_inode_raw(fs, ino, ribuf, 0) != 0) { kfree(ribuf); return 0; }

    uint32_t got = 0;
    uint32_t first = ext2_alloc_run(fs, nblocks, &got);
    if (!first || got == 0) { kfree(ribuf); return 0; }
    nblocks = got;

    uint32_t add_meta = 0, new_ind = 0, new_dind = 0;
    uint8_t *ib = 0, *db = 0;

    // Contents FIRST, while the blocks are still unreachable from the inode.
    // Split into transfers the DMA bounce buffer can take.
    {
        uint32_t done = 0;
        while (done < nblocks) {
            uint32_t chunk = nblocks - done;
            if (chunk * spb > EXT2_DMA_MAX_SECTORS) chunk = EXT2_DMA_MAX_SECTORS / spb;
            if (chunk == 0) chunk = 1;
            const uint8_t *src = (const uint8_t *)data + (uint64_t)done * bs;
            if (blk_write(fs->channel, fs->drive,
                          fs->part_start_lba + (uint64_t)(first + done) * spb,
                          chunk * spb, src) != (int)(chunk * spb)) goto rfail;
            for (uint32_t k = 0; k < chunk; k++)          // keep the cache coherent
                ext2_cache_update(first + done + k, src + (uint64_t)k * bs, bs);
            done += chunk;
        }
    }

    if (m.kind == 0) {                       // ---- direct ----
        for (uint32_t k = 0; k < nblocks; k++) {
            uint32_t v = first + k;
            memcpy(ribuf + 40 + (m.slot + k) * 4, &v, 4);
        }
    } else if (m.kind == 1) {                // ---- single indirect ----
        uint32_t ind = rd32(ribuf + 40 + EXT2_IND_BLOCK * 4);
        if (ind == 0) {
            ind = ext2_alloc_block(fs, 1);   // zeroed
            if (!ind) goto rfail;
            new_ind = ind; add_meta++;
        }
        ib = (uint8_t *)kmalloc(bs);
        if (!ib) goto rfail;
        if (ext2_read_block(fs, ind, ib) != 0) goto rfail;
        for (uint32_t k = 0; k < nblocks; k++) {
            uint32_t v = first + k;
            memcpy(ib + (m.inner + k) * 4, &v, 4);
        }
        if (ext2_write_block(fs, ind, ib) != 0) goto rfail;
        kfree(ib); ib = 0;
        if (new_ind) memcpy(ribuf + 40 + EXT2_IND_BLOCK * 4, &ind, 4);
    } else {                                 // ---- double indirect ----
        uint32_t dind = rd32(ribuf + 40 + EXT2_DIND_BLOCK * 4);
        if (dind == 0) {
            dind = ext2_alloc_block(fs, 1);
            if (!dind) goto rfail;
            new_dind = dind; add_meta++;
        }
        db = (uint8_t *)kmalloc(bs);
        if (!db) goto rfail;
        if (ext2_read_block(fs, dind, db) != 0) goto rfail;
        uint32_t ind = rd32(db + m.slot * 4);
        if (ind == 0) {
            ind = ext2_alloc_block(fs, 1);
            if (!ind) goto rfail;
            new_ind = ind; add_meta++;
            memcpy(db + m.slot * 4, &ind, 4);        // in memory until persisted
        }
        ib = (uint8_t *)kmalloc(bs);
        if (!ib) goto rfail;
        if (ext2_read_block(fs, ind, ib) != 0) goto rfail;
        for (uint32_t k = 0; k < nblocks; k++) {
            uint32_t v = first + k;
            memcpy(ib + (m.inner + k) * 4, &v, 4);
        }
        if (ext2_write_block(fs, ind, ib) != 0) goto rfail;
        kfree(ib); ib = 0;
        if (new_ind && ext2_write_block(fs, dind, db) != 0) goto rfail;
        kfree(db); db = 0;
        if (new_dind) memcpy(ribuf + 40 + EXT2_DIND_BLOCK * 4, &dind, 4);
    }

    {
        uint32_t iblocks = rd32(ribuf + 28);
        iblocks += (nblocks + add_meta) * (bs / 512);
        memcpy(ribuf + 28, &iblocks, 4);
    }
    if (size_add) {
        uint32_t sz = rd32(ribuf + 4) + size_add;
        memcpy(ribuf + 4, &sz, 4);
    }
    // #695: was discarded. This commits i_blocks and i_size for the run just
    // written. If it does not land, the blocks are on disk but the inode does
    // not describe them, so reporting success reports a file that is not there.
    if (ext2_inode_raw(fs, ino, ribuf, 1) != 0) {
        kfree(ribuf); e2_werr_set(EXT2_E_IO); return 0;
    }
    kfree(ribuf);
    return nblocks;

rfail:
    if (ib) kfree(ib);
    if (db) kfree(db);
    if (new_ind)  ext2_free_block(fs, new_ind);
    if (new_dind) ext2_free_block(fs, new_dind);
    for (uint32_t k = 0; k < nblocks; k++) ext2_free_block(fs, first + k);
    kfree(ribuf);
    return 0;
}

static uint32_t ext2_inode_append_block_ex(ext2_fs_t *fs, uint32_t ino, uint32_t logical,
                                           const void *init, uint32_t size_add) {
    uint32_t ptrs = fs->block_size / 4;
    uint8_t *ribuf = (uint8_t *)kmalloc(fs->inode_size);
    if (!ribuf) return 0;
    if (ext2_inode_raw(fs, ino, ribuf, 0) != 0) { kfree(ribuf); return 0; }
    uint32_t newb = ext2_alloc_block(fs, 0);   // data block: caller overwrites it fully
    if (!newb) { kfree(ribuf); return 0; }
    // Contents first, while the block is still unreachable from the inode.
    if (init && ext2_write_block(fs, newb, init) != 0) {
        ext2_free_block(fs, newb);
        kfree(ribuf);
        return 0;
    }

    uint32_t add_meta  = 0;          // extra (indirect) blocks allocated this call
    uint32_t new_ind   = 0;          // freshly-allocated single-indirect block
    uint32_t new_dind  = 0;          // freshly-allocated double-indirect block
    uint8_t *db = 0, *ib = 0;        // scratch buffers (freed in cleanup)

    if (logical < EXT2_NDIR_BLOCKS) {
        memcpy(ribuf + 40 + logical * 4, &newb, 4);
    } else if (logical - EXT2_NDIR_BLOCKS < ptrs) {
        // ---- single indirect ----
        uint32_t li  = logical - EXT2_NDIR_BLOCKS;
        uint32_t ind = rd32(ribuf + 40 + EXT2_IND_BLOCK * 4);
        if (ind == 0) {
            ind = ext2_alloc_block(fs, 1);          // zeroed by ext2_alloc_block
            if (!ind) goto fail;
            new_ind = ind;
            add_meta = 1;
        }
        ib = (uint8_t *)kmalloc(fs->block_size);
        if (!ib) goto fail;
        if (ext2_read_block(fs, ind, ib) != 0) goto fail;
        memcpy(ib + li * 4, &newb, 4);
        if (ext2_write_block(fs, ind, ib) != 0) goto fail;
        kfree(ib); ib = 0;
        if (new_ind) memcpy(ribuf + 40 + EXT2_IND_BLOCK * 4, &ind, 4);
    } else if (logical - EXT2_NDIR_BLOCKS - ptrs < ptrs * ptrs) {
        // ---- double indirect ----
        // Allocate every level first; persist only after they all succeed, so no
        // on-disk pointer ever references a block we might still roll back.
        uint32_t li2   = logical - EXT2_NDIR_BLOCKS - ptrs;
        uint32_t outer = li2 / ptrs;             // index into double-indirect block
        uint32_t inner = li2 % ptrs;             // index into single-indirect block
        uint32_t dind  = rd32(ribuf + 40 + EXT2_DIND_BLOCK * 4);
        if (dind == 0) {
            dind = ext2_alloc_block(fs, 1);
            if (!dind) goto fail;
            new_dind = dind;
            add_meta++;
        }
        db = (uint8_t *)kmalloc(fs->block_size);
        if (!db) goto fail;
        if (ext2_read_block(fs, dind, db) != 0) goto fail;
        uint32_t ind = rd32(db + outer * 4);
        if (ind == 0) {
            ind = ext2_alloc_block(fs, 1);
            if (!ind) goto fail;
            new_ind = ind;
            add_meta++;
            memcpy(db + outer * 4, &ind, 4);     // in-memory until persisted below
        }
        ib = (uint8_t *)kmalloc(fs->block_size);
        if (!ib) goto fail;
        if (ext2_read_block(fs, ind, ib) != 0) goto fail;
        memcpy(ib + inner * 4, &newb, 4);
        if (ext2_write_block(fs, ind, ib) != 0) goto fail;            // data-index block
        kfree(ib); ib = 0;
        if (new_ind && ext2_write_block(fs, dind, db) != 0) goto fail; // dind->ind link
        kfree(db); db = 0;
        if (new_dind) memcpy(ribuf + 40 + EXT2_DIND_BLOCK * 4, &dind, 4);
    } else {
        goto fail;   // beyond double-indirect (triple not supported)
    }

    // bump i_blocks (in 512-byte units) and persist the inode pointers.
    {
        uint32_t iblocks = rd32(ribuf + 28);
        iblocks += (1 + add_meta) * (fs->block_size / 512);
        memcpy(ribuf + 28, &iblocks, 4);
    }
    // #597: grow i_size in the SAME write that publishes the pointer, so the
    // block is never reachable-but-outside-i_size (or sized-in but unlinked).
    if (size_add) {
        uint32_t sz = rd32(ribuf + 4) + size_add;
        memcpy(ribuf + 4, &sz, 4);
    }
    // #695. The note that used to sit here justified ignoring this return
    // because "rolling back would free a block that is still referenced": by
    // this point an already-existing indirect block on disk may already point
    // at newb, and the fail: path below frees newb, which would leave a live
    // on-disk pointer to a free block. That argument is CORRECT and is kept.
    //
    // But "do not take the fail: rollback" is a different decision from "do not
    // report", and conflating the two is what made a lost inode write silent.
    // Losing this write leaves the on-disk inode describing the PREVIOUS extent
    // of the file, so the block just published is unreachable and an earlier
    // logical block resolves to stale data: measured as a full-length file with
    // wrong contents, returned as SUCCESS. So report it, and return 0 WITHOUT
    // taking the fail: path, which frees the individual blocks. The caller
    // (ext2_write_data_to_inode) responds by rolling the WHOLE inode back,
    // which is safe here precisely because it also frees the indirect block
    // that points at newb, so no live pointer to a free block can survive.
    if (ext2_inode_raw(fs, ino, ribuf, 1) != 0) {
        kfree(ribuf);
        e2_werr_set(EXT2_E_IO);
        return 0;
    }
    kfree(ribuf);
    return newb;

fail:
    // Roll back everything reserved this call so the bitmaps stay consistent.
    if (ib) kfree(ib);
    if (db) kfree(db);
    if (new_ind)  ext2_free_block(fs, new_ind);
    if (new_dind) ext2_free_block(fs, new_dind);
    ext2_free_block(fs, newb);
    kfree(ribuf);
    return 0;
}

// Original signature: append an EMPTY block that the caller fills afterwards.
static uint32_t ext2_inode_append_block(ext2_fs_t *fs, uint32_t ino, uint32_t logical) {
    return ext2_inode_append_block_ex(fs, ino, logical, 0, 0);
}

// ---------------------------------------------------------------------------
// #605 (extends the #485 seam): the PURE, I/O-free directory-block record walk
// + insert, lifted verbatim out of ext2_dir_add()'s inner loop so it can be
// ported to Rust behind a one-function FFI.
//
// WHY THIS SEAM EXISTS. #597 fixed a MEMORY-SAFETY bug in this exact loop, in
// C: `slack = rec - used` is unsigned, so a record whose own name_len did not
// fit inside its own rec_len made slack wrap to ~4e9, every room test passed,
// and the code wrote a rec_len OUTSIDE the block and memcpy'd the name PAST the
// end of the kmalloc(block_size) buffer. That is precisely the class Rust
// removes by construction, so the guard below is now a hand-written C mirror of
// what rustkern/ext2.rs gets from `checked_sub` for free.
//
// The seam is deliberately narrow: NOTHING about ext2_lock, ext2_read_block /
// ext2_write_block, inode or bitmap allocation, ext2_inode_append_block, i_size
// or any ext2_fs_t static crosses it. Only (block buffer, block_size, name,
// name_len, ino, file_type) go in, and a status code comes back.
//
// Returns  1 = INSERTED, `blk` mutated, caller writes the block back.
//          0 = NO ROOM in this block, `blk` untouched.
//         -1 = CORRUPT geometry / invalid argument, `blk` untouched.
// The caller treats 0 and -1 identically (skip to the next block), which is
// exactly what the pre-#605 code did when its inner loop fell out or `break`ed.
//
// FFI ABI locks for the Rust twin (rustkern/ext2.rs ext2_dirblock_insert_rs).
// The FFI is scalars + pointers only, so the thing worth locking is the on-disk
// record header both sides agree on, plus the scalar widths.
typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
} ext2_dirent_hdr_t;
_Static_assert(sizeof(ext2_dirent_hdr_t) == 8, "#605 FFI: ext2 dirent header must be 8 bytes");
_Static_assert(sizeof(uint8_t)  == 1, "#605 FFI: u8 width");
_Static_assert(sizeof(uint16_t) == 2, "#605 FFI: on-disk rec_len width");
_Static_assert(sizeof(uint32_t) == 4, "#605 FFI: u32 width");
_Static_assert(sizeof(int)      == 4, "#605 FFI: i32 return width");
_Static_assert(sizeof(void *)   == 8, "#605 FFI: pointer width");

int ext2_dirblock_insert_c(uint8_t *blk, uint32_t block_size,
                           const char *name, uint32_t name_len,
                           uint32_t child_ino, uint8_t ftype) {
    if (!blk || !name) return -1;
    // An ext2 name_len is a single on-disk byte and a zero-length name is not a
    // directory entry. Pre-#605 this loop had no such check: a name_len > 255
    // was silently truncated into blk[off+6], writing a dirent whose recorded
    // name length did not match the bytes actually stored. Both arms now
    // refuse, identically.
    if (name_len == 0 || name_len > 255) return -1;

    uint32_t nlen = name_len;
    uint32_t need = e2_round4(8u + nlen);
    uint32_t off  = 0;
    // off + 8 <= block_size guarantees the entry header read below is in
    // bounds before rec_len is validated (#476).
    while (off + 8 <= block_size) {
        uint32_t e_ino = rd32(blk + off);
        uint16_t rec   = rd16(blk + off + 4);
        uint8_t  e_nl  = blk[off + 6];
        if (rec < 8 || off + rec > block_size) return -1;  // corrupt; bail this block
        uint32_t used = (e_ino == 0) ? 0 : e2_round4((uint32_t)(8 + e_nl));
        // #597 (2e): the unsigned-underflow guard. See the header comment.
        if (used > rec) return -1;
        uint32_t slack = rec - used;
        if (slack >= need) {
            uint32_t newoff, nrec32;
            if (e_ino == 0) {
                newoff = off;                 // reuse empty slot, take whole record
                nrec32 = rec;
            } else {
                newoff = off + used;          // new entry spans the rest
                nrec32 = slack;
            }
            // #597 (2e): belt-and-braces bound on the writes below. Both the
            // record header and the name must lie inside the block.
            if (newoff + 8 + nlen > block_size || newoff + nrec32 > block_size) return -1;
            if (e_ino != 0) {
                uint16_t shrunk = (uint16_t)used;
                memcpy(blk + off + 4, &shrunk, 2);  // shrink existing
            }
            memcpy(blk + newoff + 0, &child_ino, 4);
            uint16_t nrec = (uint16_t)nrec32;
            memcpy(blk + newoff + 4, &nrec, 2);
            blk[newoff + 6] = (uint8_t)nlen;
            blk[newoff + 7] = ftype;
            memcpy(blk + newoff + 8, name, nlen);
            return 1;
        }
        off += rec;
    }
    return 0;
}

// Rust drop-in for ext2_dirblock_insert_c (rustkern/ext2.rs, #605). Same
// signature, same return contract. `const char *` here and `*const u8` in Rust
// are ABI-identical; int == i32.
extern int ext2_dirblock_insert_rs(uint8_t *blk, uint32_t block_size,
                                   const char *name, uint32_t name_len,
                                   uint32_t child_ino, uint8_t ftype);

// Live strangler seam. With -DRUST_EXT2_DIRADD the real symbol routes to Rust;
// rollback = drop the one flag and rebuild. The boot-time [RUST-DIFF] self-test
// (ext2_dirinsert_rust_selftest) compares the two impls regardless of the flag.
int ext2_dirblock_insert(uint8_t *blk, uint32_t block_size,
                         const char *name, uint32_t name_len,
                         uint32_t child_ino, uint8_t ftype) {
#ifdef RUST_EXT2_DIRADD
    return ext2_dirblock_insert_rs(blk, block_size, name, name_len, child_ino, ftype);
#else
    return ext2_dirblock_insert_c(blk, block_size, name, name_len, child_ino, ftype);
#endif
}

// #746: REPOINT an existing directory record at a different inode, in place.
// C reference twin of rustkern/ext2.rs `dirrep::repoint`; the Rust one is live
// under -DRUST_EXT2_DIRREPOINT and the boot differential compares them either
// way. Returns 1 = found and repointed, 0 = not in this block, -1 = the record
// geometry is not walkable.
//
// This is the ATOMIC step of a rename over an existing destination: the record
// already carries the name, so nothing about its geometry changes and the whole
// replacement is ONE directory-block write. See ext2_rename_inner below.
int ext2_dirblock_repoint_c(uint8_t *blk, uint32_t block_size,
                            const char *name, uint32_t name_len,
                            uint32_t new_ino, uint8_t ftype) {
    if (!blk || !name || name_len == 0 || name_len > 255) return -1;
    if (block_size == 0 || block_size > 65536) return -1;
    uint32_t off = 0;
    while (off + 8 <= block_size) {
        uint32_t ino = rd32(blk + off);
        uint16_t rec_len = blk[off + 4] | (blk[off + 5] << 8);
        uint8_t  nlen    = blk[off + 6];
        // Same two guards as the scan and the remove (#476/#597): a record must
        // be walkable and must stay inside the block.
        if (rec_len < 8 || off + rec_len > block_size) return -1;
        if (ino != 0 && e2_round4((uint32_t)(8 + nlen)) > rec_len) return -1;
        if (ino != 0 && nlen == name_len &&
            off + 8 + name_len <= block_size &&
            memcmp(blk + off + 8, name, name_len) == 0) {
            memcpy(blk + off, &new_ino, 4);
            blk[off + 7] = ftype;
            return 1;
        }
        off += rec_len;
    }
    return 0;
}

// Rust drop-in for ext2_dirblock_repoint_c (rustkern/ext2.rs, #746).
extern int ext2_dirblock_repoint_rs(uint8_t *blk, uint32_t block_size,
                                    const char *name, uint32_t name_len,
                                    uint32_t new_ino, uint8_t ftype);

int ext2_dirblock_repoint(uint8_t *blk, uint32_t block_size,
                          const char *name, uint32_t name_len,
                          uint32_t new_ino, uint8_t ftype) {
#ifdef RUST_EXT2_DIRREPOINT
    return ext2_dirblock_repoint_rs(blk, block_size, name, name_len, new_ino, ftype);
#else
    return ext2_dirblock_repoint_c(blk, block_size, name, name_len, new_ino, ftype);
#endif
}

// Add a directory entry (name -> child_ino, type) to directory `dir_ino`.
static int ext2_dir_add(ext2_fs_t *fs, uint32_t dir_ino, const char *name,
                        uint32_t child_ino, uint8_t ftype) {
    int nlen = 0; while (name[nlen]) nlen++;
    ext2_inode_t di;
    if (ext2_read_inode(dir_ino, &di) != 0) return -1;
    uint32_t nblocks = di.i_size / fs->block_size;
    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) return -1;

    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t phys = ext2_bmap(fs, &di, lb);
        if (phys == 0 || phys == EXT2_BMAP_ERR) continue;
        if (ext2_read_block(fs, phys, blk) != 0) continue;
        // #605 seam: the whole per-block record walk + split now lives behind
        // one call (Rust under -DRUST_EXT2_DIRADD, else the C reference). 0 (no
        // room) and -1 (corrupt block) both mean "try the next block", exactly
        // as the pre-#605 inner loop's fall-through and `break` did.
        int ir = ext2_dirblock_insert(blk, fs->block_size, name, (uint32_t)nlen,
                                      child_ino, ftype);
        if (ir == 1) {
            int rc = ext2_write_block(fs, phys, blk);
            kfree(blk);
            return rc;
        }
    }

    // No room: append a new directory block holding a single full-block entry.
    // #597 POPULATE-BEFORE-PUBLISH: build the block image FIRST and hand it to
    // ext2_inode_append_block_ex(), which writes the contents before linking
    // i_block[] and grows i_size in the SAME inode write. The old sequence
    // (append empty block -> write contents -> separate best-effort i_size
    // read-modify-write) both widened the race window and made the i_size grow
    // OPTIONAL: on kmalloc failure it was silently skipped, leaving a directory
    // whose last block is unreachable and its entry permanently orphaned.
    // #605: nlen > 255 added to the guard. name_len is a single on-disk byte,
    // so a longer component was previously truncated by `blk[6] = (uint8_t)nlen`
    // into a dirent whose recorded length did not match its stored bytes.
    if (nlen <= 0 || nlen > 255 || 8 + (uint32_t)nlen > fs->block_size) { kfree(blk); return -1; }
    memset(blk, 0, fs->block_size);
    memcpy(blk + 0, &child_ino, 4);
    uint16_t rec = (uint16_t)fs->block_size;
    memcpy(blk + 4, &rec, 2);
    blk[6] = (uint8_t)nlen;
    blk[7] = ftype;
    memcpy(blk + 8, name, nlen);
    uint32_t newb = ext2_inode_append_block_ex(fs, dir_ino, nblocks, blk, fs->block_size);
    kfree(blk);
    return newb ? 0 : -1;
}

// ---------------------------------------------------------------------------
// #605 boot-time self-tests for the ext2 dirent INSERT seam.
//
//  [RUST-DIFF] ext2_dirinsert - runs ext2_dirblock_insert_c and
//              ext2_dirblock_insert_rs over IDENTICAL COPIES of the same
//              generated directory block and asserts the return code AND every
//              byte of the resulting block match. Families: fresh/empty block,
//              one entry, many entries, exactly-full, tombstoned, name lengths
//              1..255, and a random-byte fuzz corpus (adversarial rec_len /
//              name_len / used-vs-rec).
//  [RUST-SEC]  ext2_dirinsert - the HOSTILE block from #597, where a record's
//              own name_len does not fit its own rec_len. Shows the arithmetic
//              the pre-#597 C performed (evaluated, not executed) and that both
//              current arms refuse it with the block byte-unmodified.
//
// Both are bounded, run once at boot, and never block (#426). They call BOTH
// impls explicitly, so the comparison happens regardless of RUST_EXT2_DIRADD
// and pulls the Rust member into the link either way.
#define E2INS_MAXBS 4096
static uint8_t e2ins_src[E2INS_MAXBS];   // generated block (the pristine input)
static uint8_t e2ins_a[E2INS_MAXBS];     // C arm working copy
static uint8_t e2ins_b[E2INS_MAXBS];     // Rust arm working copy

static inline uint32_t e2ins_rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

// Lay one dirent header (+ a deterministic name) at `off`. Name bytes are
// clamped to the buffer so a deliberately malformed name_len cannot overrun
// e2ins_src itself while we are building the hostile vector.
static void e2ins_put(uint32_t bs, uint32_t off, uint32_t ino, uint32_t rec,
                      uint32_t nl, uint8_t ty, uint32_t ch) {
    if (off + 8 > bs) return;
    e2ins_src[off + 0] = (uint8_t)(ino & 0xFF);
    e2ins_src[off + 1] = (uint8_t)((ino >> 8) & 0xFF);
    e2ins_src[off + 2] = (uint8_t)((ino >> 16) & 0xFF);
    e2ins_src[off + 3] = (uint8_t)((ino >> 24) & 0xFF);
    e2ins_src[off + 4] = (uint8_t)(rec & 0xFF);
    e2ins_src[off + 5] = (uint8_t)((rec >> 8) & 0xFF);
    e2ins_src[off + 6] = (uint8_t)nl;
    e2ins_src[off + 7] = ty;
    for (uint32_t i = 0; i < nl && off + 8 + i < bs; i++)
        e2ins_src[off + 8 + i] = (uint8_t)('a' + ((ch + i) % 26));
}

// Run BOTH arms on identical copies of e2ins_src[0..bs]; compare rc + all bytes.
static void e2ins_one(uint32_t bs, const char *name, uint32_t nlen,
                      uint32_t ino, uint8_t ftype,
                      uint32_t *vectors, uint32_t *mism, int *first_bad) {
    memcpy(e2ins_a, e2ins_src, bs);
    memcpy(e2ins_b, e2ins_src, bs);
    int rc_c  = ext2_dirblock_insert_c (e2ins_a, bs, name, nlen, ino, ftype);
    int rc_rs = ext2_dirblock_insert_rs(e2ins_b, bs, name, nlen, ino, ftype);
    (*vectors)++;
    if (rc_c != rc_rs || memcmp(e2ins_a, e2ins_b, bs) != 0) {
        (*mism)++;
        if (*first_bad < 0) *first_bad = (int)(*vectors);
    }
}

// #746: differential for the block-level REPOINT (the atomic step of rename).
// Same discipline as e2ins_one: BOTH arms on identical copies, compare the
// return code AND every byte. Reuses e2ins_src/e2ins_a/e2ins_b and the vector
// generators rather than growing a second set of them.
static void e2rep_one(uint32_t bs, const char *name, uint32_t nlen,
                      uint32_t ino, uint8_t ftype,
                      uint32_t *vectors, uint32_t *mism, int *first_bad) {
    memcpy(e2ins_a, e2ins_src, bs);
    memcpy(e2ins_b, e2ins_src, bs);
    int rc_c  = ext2_dirblock_repoint_c (e2ins_a, bs, name, nlen, ino, ftype);
    int rc_rs = ext2_dirblock_repoint_rs(e2ins_b, bs, name, nlen, ino, ftype);
    (*vectors)++;
    if (rc_c != rc_rs || memcmp(e2ins_a, e2ins_b, bs) != 0) {
        (*mism)++;
        if (*first_bad < 0) *first_bad = (int)(*vectors);
    }
}

void ext2_dirrepoint_rust_selftest(void) {
    uint32_t seed = 0x9e3779b9u;
    uint32_t vectors = 0, mism = 0, hits = 0;
    int first_bad = -1;
    const uint32_t bslist[4] = {256u, 512u, 1024u, 4096u};
    char nm[260];

    // Family 1: WELL-FORMED blocks, and a name that IS present. This is the
    // family that proves the arms agree on the operation that actually matters;
    // a differential that only ever exercises the not-found path would agree
    // trivially, which is the "control that never fires" mistake.
    for (int r = 0; r < 600; r++) {
        uint32_t bs = bslist[e2ins_rng(&seed) % 4];
        memset(e2ins_src, 0, bs);
        uint32_t off = 0, nent = 1 + (e2ins_rng(&seed) % 20);
        uint32_t pick = e2ins_rng(&seed) % nent, pick_nl = 0, pick_ch = 0;
        int placed = 0;
        for (uint32_t e = 0; e < nent; e++) {
            uint32_t nl = 1 + (e2ins_rng(&seed) % 40);
            uint32_t used = (8 + nl + 3) & ~3u;
            if (off + used + 8 > bs) break;
            uint32_t rec = (e == nent - 1) ? (bs - off) : used;
            uint32_t ch  = e2ins_rng(&seed) % 26;
            e2ins_put(bs, off, 100 + e, rec, nl, 1, ch);
            if (e == pick) { pick_nl = nl; pick_ch = ch; placed = 1; }
            off += rec;
            if (rec == 0) break;
        }
        if (placed) {
            for (uint32_t i = 0; i < pick_nl; i++)
                nm[i] = (char)('a' + ((pick_ch + i) % 26));
            nm[pick_nl] = 0;
            // Assert the vector actually HITS: a repoint differential whose
            // every vector missed would be green for the wrong reason.
            memcpy(e2ins_a, e2ins_src, bs);
            if (ext2_dirblock_repoint_c(e2ins_a, bs, nm, pick_nl, 4242, 1) == 1) hits++;
            e2rep_one(bs, nm, pick_nl, 4242, 1, &vectors, &mism, &first_bad);
        }
        // ... and a name that is NOT present, in the same well-formed block.
        uint32_t nl2 = 1 + (e2ins_rng(&seed) % 40);
        for (uint32_t i = 0; i < nl2; i++) nm[i] = (char)('A' + (i % 26));
        nm[nl2] = 0;
        e2rep_one(bs, nm, nl2, 4243, 2, &vectors, &mism, &first_bad);
    }

    // Family 2: RANDOM FUZZ, so every disk-controlled rec_len / name_len takes
    // adversarial values at random offsets, including the #597 used > rec shape.
    for (int n = 0; n < 12000; n++) {
        uint32_t bs = bslist[e2ins_rng(&seed) % 4];
        for (uint32_t i = 0; i < bs; i++)
            e2ins_src[i] = (uint8_t)(e2ins_rng(&seed) & 0xFF);
        uint32_t nl = 1 + (e2ins_rng(&seed) % 255);
        for (uint32_t i = 0; i < nl; i++) nm[i] = (char)(e2ins_rng(&seed) & 0xFF);
        nm[nl] = 0;
        e2rep_one(bs, nm, nl, e2ins_rng(&seed), (uint8_t)(e2ins_rng(&seed) & 7),
                  &vectors, &mism, &first_bad);
    }

    // hits == 0 is a BROKEN TEST, not a pass: it means no vector ever reached
    // the code under test. Reported as FAIL so it can never read as evidence.
    const char *verdict = (mism == 0 && hits > 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] ext2_dirrepoint: %u vectors, %u hits, %u mismatches -> %s\n",
            vectors, hits, mism, verdict);
    bootlog_write("[RUST-DIFF] ext2_dirrepoint: %u vectors, %u hits, %u mismatches -> %s",
                  vectors, hits, mism, verdict);
    if (mism != 0)
        kprintf("[RUST-DIFF] ext2_dirrepoint FIRST MISMATCH at vector #%d\n", first_bad);
}

void ext2_dirinsert_rust_selftest(void) {
    uint32_t seed = 0x2545f491u;
    uint32_t vectors = 0, mism = 0;
    int first_bad = -1;
    const uint32_t bslist[4] = {256u, 512u, 1024u, 4096u};
    char nm[260];

    // Family 1: FRESH/EMPTY directory block (one tombstone spanning the block),
    // inserting names of every length 1..255.
    for (int bi = 0; bi < 4; bi++) {
        uint32_t bs = bslist[bi];
        for (uint32_t nl = 1; nl <= 255; nl++) {
            memset(e2ins_src, 0, bs);
            e2ins_put(bs, 0, 0, bs, 0, 0, 0);      // ino=0, rec_len=bs
            for (uint32_t i = 0; i < nl; i++) nm[i] = (char)('A' + (i % 26));
            nm[nl] = 0;
            e2ins_one(bs, nm, nl, 0x1234u + nl, 1, &vectors, &mism, &first_bad);
        }
    }

    // Family 2: ONE entry, then MANY entries, with varying trailing slack; the
    // last record always absorbs the rest of the block (real ext2 layout).
    for (int r = 0; r < 300; r++) {
        uint32_t bs = bslist[e2ins_rng(&seed) % 4];
        memset(e2ins_src, 0, bs);
        uint32_t off = 0;
        uint32_t nent = 1 + (e2ins_rng(&seed) % 24);
        for (uint32_t e = 0; e < nent; e++) {
            uint32_t nl = 1 + (e2ins_rng(&seed) % 40);
            uint32_t used = (8 + nl + 3) & ~3u;
            if (off + used + 8 > bs) break;
            uint32_t rec = used;
            if (e + 1 == nent) rec = bs - off;              // last absorbs tail
            else if (e2ins_rng(&seed) & 1) {                 // sometimes leave slack
                uint32_t extra = (e2ins_rng(&seed) % 64) & ~3u;
                if (off + used + extra <= bs) rec = used + extra;
            }
            uint32_t ino = 11 + (e2ins_rng(&seed) & 0xFFFFu);
            if (e2ins_rng(&seed) % 7 == 0) ino = 0;          // tombstone
            e2ins_put(bs, off, ino, rec, nl, (uint8_t)(1 + (e2ins_rng(&seed) % 7)), e);
            off += rec;
            if (off + 8 > bs) break;
        }
        for (int q = 0; q < 6; q++) {
            uint32_t nl = 1 + (e2ins_rng(&seed) % 60);
            for (uint32_t i = 0; i < nl; i++) nm[i] = (char)('a' + (e2ins_rng(&seed) % 26));
            nm[nl] = 0;
            e2ins_one(bs, nm, nl, 500 + (uint32_t)q, 2, &vectors, &mism, &first_bad);
        }
    }

    // Family 3: EXACTLY-FULL block. Every record is rec == used, so no slot has
    // one spare byte and the correct answer is "no room" (0) for every name.
    for (int bi = 0; bi < 4; bi++) {
        uint32_t bs = bslist[bi];
        memset(e2ins_src, 0, bs);
        uint32_t off = 0, e = 0;
        while (off + 16 <= bs) {
            uint32_t rec = 16;                    // name_len 8 -> used == 16
            if (off + 32 > bs) rec = bs - off;    // tail record absorbs remainder
            uint32_t nl = (rec == 16) ? 8 : (rec - 8);
            if (nl > 255) nl = 255;
            e2ins_put(bs, off, 20 + e, rec, nl, 1, e);
            off += rec; e++;
        }
        for (uint32_t nl = 1; nl <= 32; nl++) {
            for (uint32_t i = 0; i < nl; i++) nm[i] = 'z';
            nm[nl] = 0;
            e2ins_one(bs, nm, nl, 9000 + nl, 1, &vectors, &mism, &first_bad);
        }
    }

    // Family 4: TOMBSTONE reuse. A dead record (inode == 0) in the middle of a
    // live block must be taken whole, not split.
    for (int r = 0; r < 200; r++) {
        uint32_t bs = bslist[2 + (int)(e2ins_rng(&seed) % 2)];   // 1024 / 4096
        memset(e2ins_src, 0, bs);
        uint32_t off = 0, e = 0;
        uint32_t dead_at = 1 + (e2ins_rng(&seed) % 5);
        while (off + 8 < bs) {
            uint32_t nl = 1 + (e2ins_rng(&seed) % 30);
            uint32_t rec = (8 + nl + 3) & ~3u;
            if (e == dead_at) rec += 64;                 // a roomier dead slot
            if (off + rec + 8 > bs) { rec = bs - off; }
            uint32_t ino = (e == dead_at) ? 0 : (30 + e);
            e2ins_put(bs, off, ino, rec, nl, 1, e);
            off += rec; e++;
            if (rec == 0) break;
        }
        for (int q = 0; q < 4; q++) {
            uint32_t nl = 1 + (e2ins_rng(&seed) % 50);
            for (uint32_t i = 0; i < nl; i++) nm[i] = (char)('m' + (i % 13));
            nm[nl] = 0;
            e2ins_one(bs, nm, nl, 7000 + (uint32_t)q, 2, &vectors, &mism, &first_bad);
        }
    }

    // Family 5: RANDOM FUZZ. Fully random bytes, so every disk-controlled
    // rec_len / name_len / used-vs-rec relation takes adversarial values,
    // including the #597 used > rec underflow shape, at random offsets.
    for (int n = 0; n < 12000; n++) {
        uint32_t bs = bslist[e2ins_rng(&seed) % 4];
        for (uint32_t i = 0; i < bs; i++)
            e2ins_src[i] = (uint8_t)(e2ins_rng(&seed) & 0xFF);
        uint32_t nl = 1 + (e2ins_rng(&seed) % 255);
        for (uint32_t i = 0; i < nl; i++) nm[i] = (char)(e2ins_rng(&seed) & 0xFF);
        nm[nl] = 0;
        e2ins_one(bs, nm, nl, e2ins_rng(&seed), (uint8_t)(e2ins_rng(&seed) & 7),
                  &vectors, &mism, &first_bad);
    }

    const char *verdict = (mism == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] ext2_dirinsert: %u vectors, %u mismatches -> %s\n",
            vectors, mism, verdict);
    bootlog_write("[RUST-DIFF] ext2_dirinsert: %u vectors, %u mismatches -> %s",
                  vectors, mism, verdict);
    if (mism != 0) {
        kprintf("[RUST-DIFF] ext2_dirinsert FIRST MISMATCH at vector #%d\n", first_bad);
        bootlog_write("[RUST-DIFF] ext2_dirinsert FIRST MISMATCH at vector #%d", first_bad);
    }

    // ----- [RUST-SEC]: the #597 hostile block -------------------------------
    // 56 tightly-packed live records (rec == used == 16) leave no slack, so the
    // walk reaches offset 896, where a record claims rec_len = 128 but
    // name_len = 200: its own name does NOT fit its own rec_len, i.e.
    // used = round4(8+200) = 208 > rec = 128. Pre-#597 C then evaluated
    // `slack = rec - used` on uint32_t.
    {
        const uint32_t bs = 1024, hoff = 896, hrec = 128, hnl = 200;
        memset(e2ins_src, 0, bs);
        for (uint32_t off = 0; off + 16 <= hoff; off += 16)
            e2ins_put(bs, off, 11 + off, 16, 8, 1, off);
        e2ins_put(bs, hoff, 4242, hrec, hnl, 1, 3);

        memcpy(e2ins_a, e2ins_src, bs);
        memcpy(e2ins_b, e2ins_src, bs);
        int rc_c  = ext2_dirblock_insert_c (e2ins_a, bs, "hostile1", 8, 777, 1);
        int rc_rs = ext2_dirblock_insert_rs(e2ins_b, bs, "hostile1", 8, 777, 1);
        int unmod_c  = (memcmp(e2ins_a, e2ins_src, bs) == 0);
        int unmod_rs = (memcmp(e2ins_b, e2ins_src, bs) == 0);

        // The pre-#597 arithmetic, EVALUATED (never executed as a write) so the
        // numbers are on the record rather than only in a comment.
        uint32_t used_pre   = e2_round4(8u + hnl);          // 208
        uint32_t slack_pre  = hrec - used_pre;              // UNSIGNED UNDERFLOW
        uint32_t newoff_pre = hoff + used_pre;              // 1104 > bs
        uint32_t nrec_pre   = (uint16_t)slack_pre;          // truncated to u16
        kprintf("[RUST-SEC] ext2_dirinsert hostile: bs=%u off=%u rec_len=%u name_len=%u "
                "used=%u -> pre-#597 slack=%u newoff=%u rec_len_written=%u; "
                "name bytes would land at %u..%u, PAST the kmalloc(%u) buffer\n",
                bs, hoff, hrec, hnl, used_pre, slack_pre, newoff_pre, nrec_pre,
                newoff_pre + 8, newoff_pre + 8 + 8, bs);
        bootlog_write("[RUST-SEC] ext2_dirinsert hostile: pre-#597 slack=%u newoff=%u "
                      "(bs=%u) rec_len_written=%u -> OOB write",
                      slack_pre, newoff_pre, bs, nrec_pre);

        int ok = (rc_c == -1) && (rc_rs == -1) && unmod_c && unmod_rs;
        kprintf("[RUST-SEC] ext2_dirinsert: rust rc=%d (block %s), c rc=%d (block %s) -> %s\n",
                rc_rs, unmod_rs ? "UNMODIFIED" : "MODIFIED",
                rc_c,  unmod_c  ? "UNMODIFIED" : "MODIFIED",
                ok ? "PASS (both refuse; Rust by construction via checked_sub)" : "FAIL");
        bootlog_write("[RUST-SEC] ext2_dirinsert: rust rc=%d c rc=%d -> %s",
                      rc_rs, rc_c, ok ? "PASS" : "FAIL");
    }
}

// Create a regular file with `data` under directory `dir_ino`. Returns ino/0.
static uint32_t ext2_create_file_ino(ext2_fs_t *fs, uint32_t dir_ino, const char *name,
                                     const void *data, uint32_t len) {
    uint32_t ino = ext2_alloc_inode(fs, 0);
    if (!ino) return 0;
    // zero a fresh inode then fill: mode 0100644, links 1.
    uint8_t *ri = (uint8_t *)kmalloc(fs->inode_size);
    if (!ri) return 0;
    memset(ri, 0, fs->inode_size);
    uint16_t mode = 0x81A4; memcpy(ri + 0, &mode, 2);
    memcpy(ri + 4, &len, 4);
    uint16_t links = 1; memcpy(ri + 26, &links, 2);
    ext2_stamp_raw_new(ri);   // #115: was created with all three times zero
    if (ext2_inode_raw(fs, ino, ri, 1) != 0) {   // #695: was discarded
        kfree(ri);
        ext2_free_inode(fs, ino, 0);
        e2_werr_set(EXT2_E_IO);
        return 0;
    }
    kfree(ri);
    // write data block by block
    const uint8_t *p = (const uint8_t *)data;
    uint32_t remaining = len, lb = 0;
    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) return 0;
    while (remaining > 0) {
        uint32_t phys = ext2_inode_append_block(fs, ino, lb);
        if (!phys) {
            // Out of space mid-write: free the partial blocks + the inode so we
            // never leave an orphan (the dir entry was not added yet).
            kfree(blk);
            ext2_rollback_inode(fs, ino, 1);
            return 0;
        }
        uint32_t chunk = remaining < fs->block_size ? remaining : fs->block_size;
        memset(blk, 0, fs->block_size);
        memcpy(blk, p, chunk);
        if (ext2_write_block(fs, phys, blk) != 0) {
            // #695: this return was discarded, so a genuine media error made
            // the whole create report SUCCESS. Same cleanup as the out-of-space
            // case above: the directory entry has not been added yet, so
            // freeing the partial blocks and the inode leaves no orphan.
            kfree(blk);
            ext2_rollback_inode(fs, ino, 1);
            e2_werr_set(EXT2_E_IO);
            return 0;
        }
        p += chunk; remaining -= chunk; lb++;
    }
    kfree(blk);
    if (ext2_dir_add(fs, dir_ino, name, ino, EXT2_FT_REG_FILE) != 0) {
        ext2_rollback_inode(fs, ino, 1);   // could not link it; reclaim everything
        return 0;
    }
    return ino;
}

// ---- path-based public API ------------------------------------------------
// Split "/a/b/c" -> parent ino (resolve "/a/b") + basename "c".
static uint32_t ext2_parent_and_base(const char *path, char *base_out, int base_sz) {
    int len = 0; while (path[len]) len++;
    int slash = -1;
    for (int i = len - 1; i >= 0; i--) if (path[i] == '/') { slash = i; break; }
    if (slash < 0) { // no slash: relative to root
        int j = 0; for (; path[j] && j < base_sz - 1; j++) base_out[j] = path[j];
        base_out[j] = 0;
        return EXT2_ROOT_INO;
    }
    int bl = len - slash - 1; if (bl >= base_sz) bl = base_sz - 1;
    for (int i = 0; i < bl; i++) base_out[i] = path[slash + 1 + i];
    base_out[bl] = 0;
    if (slash == 0) return EXT2_ROOT_INO;
    char parent[256]; int pl = slash < 255 ? slash : 255;
    for (int i = 0; i < pl; i++) parent[i] = path[i];
    parent[pl] = 0;
    return ext2_resolve_path(parent);
}

// Write (create/overwrite-by-create) a file at an absolute ext2 path.
static int ext2_write_file_inner(const char *path, const void *data, uint32_t len);

int ext2_write_file(const char *path, const void *data, uint32_t len) {
    ext2_lock();
    int r = ext2_write_file_inner(path, data, len);
    ext2_unlock();
    return r;
}

// #115: the ext2 half of utime(2). Pass -1 to leave a time unchanged.
// i_ctime always moves to now, because that is what POSIX says a metadata
// change does; a utime() that silently left ctime alone would be a second
// field quietly reporting a stale value, which is the defect class #115 is
// about. Takes the same ext2_lock() every other public entry point takes.
int ext2_set_times(uint32_t ino, int64_t atime, int64_t mtime) {
    if (!g_ext2.mounted || ino == 0) return -1;
    ext2_lock();
    uint8_t *ri = (uint8_t *)kmalloc(g_ext2.inode_size);
    if (!ri) { ext2_unlock(); return -1; }
    if (ext2_inode_raw(&g_ext2, ino, ri, 0) != 0) { kfree(ri); ext2_unlock(); return -1; }
    if (atime >= 0) { uint32_t t = (uint32_t)atime; memcpy(ri + 8,  &t, 4); }
    if (mtime >= 0) { uint32_t t = (uint32_t)mtime; memcpy(ri + 16, &t, 4); }
    int64_t now = wallclock_now_unix();
    if (now > 0) { uint32_t t = (uint32_t)now; memcpy(ri + 12, &t, 4); }
    int rc = ext2_inode_raw(&g_ext2, ino, ri, 1);
    kfree(ri);
    ext2_unlock();
    return rc == 0 ? 0 : -1;
}

static int ext2_write_file_inner(const char *path, const void *data, uint32_t len) {
    if (!g_ext2.mounted) return -1;
    char base[256];
    e2_werr_clear();            // #695: this operation starts with no cause
    uint32_t parent = ext2_parent_and_base(path, base, sizeof(base));
    if (!parent || !base[0]) return -1;
    // If it already exists: overwrite in place (truncate the inode, keep its
    // number + directory entry, write the new contents). #99 Phase C.
    uint32_t existing = 0; uint8_t t;
    if (ext2_lookup(parent, base, &existing, &t) == 0) {
        if (t == EXT2_FT_DIR) return -2;   // never clobber a directory
        uint8_t *ri = (uint8_t *)kmalloc(g_ext2.inode_size);
        if (!ri) return -3;
        if (ext2_inode_raw(&g_ext2, existing, ri, 0) != 0) { kfree(ri); return -3; }
        ext2_truncate_inode(&g_ext2, ri);          // frees old data blocks
        uint32_t sz = len; memcpy(ri + 4, &sz, 4);  // new i_size
        ext2_stamp_raw_mtime(ri);   // #115: overwrite-in-place is a modification
        int irc = ext2_inode_raw(&g_ext2, existing, ri, 1);   // #695: was discarded
        kfree(ri);
        if (irc != 0) return EXT2_E_IO;
        return ext2_write_data_to_inode(&g_ext2, existing, data, len);
    }
    uint32_t ino = ext2_create_file_ino(&g_ext2, parent, base, data, len);
    // #695: -3 used to mean BOTH "filesystem full" and "out of kernel memory".
    return ino ? 0 : e2_werr_take(EXT2_E_NOMEM);
}

// ---------------------------------------------------------------------------
// #598 APPEND. O_APPEND was silently behaving as O_TRUNC: sys_open() started the
// write buffer at wlen = 0 for any write-flagged open, and sys_close() flushed it
// with ext2_write_file(), which REPLACES the whole file. So fopen(path, "a")
// destroyed everything already in the file.
//
// This appends to the END of an existing regular file WITHOUT reading it back
// into kernel RAM, so a 400MB log costs the same kernel memory as a 400-byte
// one: fill the tail of the current last (partial) block in place, then append
// whole blocks. i_size is committed so that it never describes bytes that have
// not been written. Creates the file when absent, so O_CREAT|O_APPEND works.
// Returns 0 on success, -2 if the path is a directory, negative otherwise.
static int ext2_append_file_inner(const char *path, const void *data, uint32_t len) {
    if (!g_ext2.mounted) return -1;
    e2_werr_clear();            // #695: this operation starts with no cause
    char base[256];
    uint32_t parent = ext2_parent_and_base(path, base, sizeof(base));
    if (!parent || !base[0]) return -1;
    uint32_t ino = 0; uint8_t t = 0;
    if (ext2_lookup_inner(parent, base, &ino, &t) != 0)
        return ext2_write_file_inner(path, data, len);   // absent: plain create
    if (t == EXT2_FT_DIR) return -2;                     // never clobber a directory
    if (len == 0) return 0;

    uint32_t bs = g_ext2.block_size;
    uint8_t *ri = (uint8_t *)kmalloc(g_ext2.inode_size);
    if (!ri) return -3;
    if (ext2_inode_raw(&g_ext2, ino, ri, 0) != 0) { kfree(ri); return -3; }
    uint32_t size = rd32(ri + 4);
    kfree(ri);

    const uint8_t *p = (const uint8_t *)data;
    uint32_t remaining = len;
    uint8_t *blk = (uint8_t *)kmalloc(bs);
    if (!blk) return -3;

    // 1) Top up the existing final partial block in place.
    uint32_t tail = size % bs;
    if (tail) {
        ext2_inode_t in;
        if (ext2_read_inode(ino, &in) != 0) { kfree(blk); return -3; }
        uint32_t phys = ext2_bmap(&g_ext2, &in, size / bs);
        if (!phys || phys == EXT2_BMAP_ERR ||
            ext2_read_block(&g_ext2, phys, blk) != 0) { kfree(blk); return -3; }
        uint32_t room  = bs - tail;
        uint32_t chunk = remaining < room ? remaining : room;
        memcpy(blk + tail, p, chunk);
        if (ext2_write_block(&g_ext2, phys, blk) != 0) { kfree(blk); return -3; }
        p += chunk; remaining -= chunk; size += chunk;
        // Commit the new size now: the bytes are already on disk, and the whole
        // -block appends below fold their own growth into their inode write.
        ri = (uint8_t *)kmalloc(g_ext2.inode_size);
        if (!ri) { kfree(blk); return -3; }
        int src = -1;
        if (ext2_inode_raw(&g_ext2, ino, ri, 0) == 0) {
            memcpy(ri + 4, &size, 4);
            ext2_stamp_raw_mtime(ri);   // #115
            src = ext2_inode_raw(&g_ext2, ino, ri, 1);   // #695: was discarded
        }
        kfree(ri);
        // The bytes are already on disk; if i_size never lands they are
        // unreachable, and returning 0 would call that an append.
        if (src != 0) { kfree(blk); return EXT2_E_IO; }
    }

    // 2) Append the rest as whole blocks (size is block-aligned from here).
    while (remaining > 0) {
        uint32_t chunk = remaining < bs ? remaining : bs;
        memset(blk, 0, bs);
        memcpy(blk, p, chunk);
        if (!ext2_inode_append_block_ex(&g_ext2, ino, size / bs, blk, chunk)) {
            kfree(blk);
            return e2_werr_take(EXT2_E_NOSPC);   // #695: was a merged -3
        }
        p += chunk; remaining -= chunk; size += chunk;
    }
    kfree(blk);
    return 0;
}

int ext2_append_file(const char *path, const void *data, uint32_t len) {
    ext2_lock();
    int r = ext2_append_file_inner(path, data, len);
    ext2_unlock();
    return r;
}

// ---------------------------------------------------------------------------
// #572 STREAMING WRITE (bounded-RAM). ext2_write_file() holds the ENTIRE file
// in one kernel buffer before writing it; a 130MB/391MB file needs 130/391MB of
// contiguous-ish heap. These three functions let a caller append a file block
// by block, flushing each full block to disk as it fills, so kernel RAM stays
// bounded regardless of file size. They reuse the SAME primitives as
// ext2_write_file (ext2_alloc_inode / ext2_dir_add / ext2_inode_append_block /
// ext2_write_block / ext2_truncate_inode, zero-padded final block, mode 0x81A4,
// i_size set once), so the final on-disk result is byte-for-byte identical to a
// single ext2_write_file(path, whole_buffer, total). The only difference is that
// the inode/dir-entry are linked at begin() (i_size grows to its final value at
// finish()) instead of appearing atomically at the end; a crash mid-stream
// therefore leaves a shorter-but-valid file, matching ext2_write_data_to_inode's
// existing rollback semantics rather than "no file".
static int ext2_wstream_begin_inner(const char *path, ext2_wstream_t *ws);

int ext2_wstream_begin(const char *path, ext2_wstream_t *ws) {
    ext2_lock();
    int r = ext2_wstream_begin_inner(path, ws);
    ext2_unlock();
    return r;
}

static int ext2_wstream_begin_inner(const char *path, ext2_wstream_t *ws) {
    if (!g_ext2.mounted || !ws) return -1;
    e2_werr_clear();            // #695: this operation starts with no cause
    ws->ino = 0; ws->next_lblk = 0; ws->written = 0; ws->ok = 0;
    char base[256];
    uint32_t parent = ext2_parent_and_base(path, base, sizeof(base));
    if (!parent || !base[0]) return -1;
    uint32_t existing = 0; uint8_t t;
    if (ext2_lookup(parent, base, &existing, &t) == 0) {
        if (t == EXT2_FT_DIR) return -2;           // never clobber a directory
        // Overwrite in place: truncate (frees old data blocks), keep inode+dirent.
        uint8_t *ri = (uint8_t *)kmalloc(g_ext2.inode_size);
        if (!ri) return -3;
        if (ext2_inode_raw(&g_ext2, existing, ri, 0) != 0) { kfree(ri); return -3; }
        ext2_truncate_inode(&g_ext2, ri);
        uint32_t z = 0; memcpy(ri + 4, &z, 4);     // i_size = 0 (low)
        memcpy(ri + 108, &z, 4);                    // i_size_high = 0
        int irc = ext2_inode_raw(&g_ext2, existing, ri, 1);   // #695: was discarded
        kfree(ri);
        if (irc != 0) return EXT2_E_IO;
        ws->ino = existing; ws->ok = 1;
        return 0;
    }
    // Create a fresh, empty regular file and link it now.
    uint32_t ino = ext2_alloc_inode(&g_ext2, 0);
    if (!ino) return e2_werr_take(EXT2_E_NOSPC);   // #695: was a merged -3
    uint8_t *ri = (uint8_t *)kmalloc(g_ext2.inode_size);
    if (!ri) { ext2_free_inode(&g_ext2, ino, 0); return -3; }
    memset(ri, 0, g_ext2.inode_size);
    uint16_t mode = 0x81A4; memcpy(ri + 0, &mode, 2);   // regular, 0644
    uint16_t links = 1;     memcpy(ri + 26, &links, 2);
    ext2_stamp_raw_new(ri);   // #115
    int irc = ext2_inode_raw(&g_ext2, ino, ri, 1);   // #695: was discarded
    kfree(ri);
    if (irc != 0) { ext2_free_inode(&g_ext2, ino, 0); return EXT2_E_IO; }
    if (ext2_dir_add(&g_ext2, parent, base, ino, EXT2_FT_REG_FILE) != 0) {
        ext2_rollback_inode(&g_ext2, ino, 1);           // reclaim inode + blocks
        return e2_werr_take(EXT2_E_NOMEM);
    }
    ws->ino = ino; ws->ok = 1;
    return 0;
}

// #609: append `len` bytes (ANY length, not just one block) as the next logical
// blocks of the stream. Whole blocks are appended in contiguous RUNS, one
// metadata transaction per run; a trailing partial block is zero-padded exactly
// like ext2_write_data_to_inode. Only the LAST call of a stream may pass a
// non-multiple of block_size. `data` is a KERNEL pointer.
static int ext2_wstream_block_inner(ext2_wstream_t *ws, const void *data, uint32_t len);

int ext2_wstream_block(ext2_wstream_t *ws, const void *data, uint32_t len) {
    ext2_lock();
    int r = ext2_wstream_block_inner(ws, data, len);
    ext2_unlock();
    return r;
}

static int ext2_wstream_block_inner(ext2_wstream_t *ws, const void *data, uint32_t len) {
    if (!ws || !ws->ok) return -1;
    uint32_t bs = g_ext2.block_size;
    if (len == 0) return -1;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t off = 0;
    while (off < len) {
        uint32_t remain = len - off;
        uint32_t nb = remain / bs;
        if (nb == 0) {
            // Final short block: zero-padded through a one-block staging buffer,
            // exactly as before.
            uint8_t *blk = (uint8_t *)kmalloc(bs);
            if (!blk) { ws->ok = 0; return -3; }
            memset(blk, 0, bs);
            memcpy(blk, p + off, remain);
            uint32_t n = ext2_inode_append_run(&g_ext2, ws->ino, ws->next_lblk, blk, 1, 0);
            kfree(blk);
            if (n != 1) { ws->ok = 0; return -3; }
            ws->next_lblk++;
            ws->written += remain;
            return 0;
        }
        if (nb > EXT2_WRUN_BLOCKS) nb = EXT2_WRUN_BLOCKS;
        uint32_t n = ext2_inode_append_run(&g_ext2, ws->ino, ws->next_lblk, p + off, nb, 0);
        // #695: "out of space / I/O error" was literally the comment, and the
        // single -3 was exactly why the caller could not tell which.
        if (n == 0) { ws->ok = 0; return e2_werr_take(EXT2_E_NOSPC); }
        ws->next_lblk += n;
        ws->written   += (uint64_t)n * bs;
        off           += n * bs;
    }
    return 0;
}

// Commit the final size. After this the file is complete and byte-identical to
// ext2_write_file(path, whole_buffer, ws->written).
static int ext2_wstream_finish_inner(ext2_wstream_t *ws);

int ext2_wstream_finish(ext2_wstream_t *ws) {
    ext2_lock();
    int r = ext2_wstream_finish_inner(ws);
    ext2_unlock();
    return r;
}

static int ext2_wstream_finish_inner(ext2_wstream_t *ws) {
    if (!ws || !ws->ino) return -1;
    uint8_t *ri = (uint8_t *)kmalloc(g_ext2.inode_size);
    if (!ri) return -3;
    if (ext2_inode_raw(&g_ext2, ws->ino, ri, 0) != 0) { kfree(ri); return -3; }
    uint32_t lo = (uint32_t)(ws->written & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(ws->written >> 32);
    memcpy(ri + 4, &lo, 4);      // i_size (low)
    memcpy(ri + 108, &hi, 4);    // i_dir_acl / i_size_high (large_file)
    ext2_stamp_raw_mtime(ri);    // #115: this is the write that makes it non-empty
    // #695: was discarded. This is the ONLY write that makes a streamed file
    // non-empty. If it does not land, every byte is on disk but i_size stays 0,
    // the file reads back EMPTY, and sys_close() still returned 0.
    int irc = ext2_inode_raw(&g_ext2, ws->ino, ri, 1);
    kfree(ri);
    ws->ok = 0;
    if (irc != 0) return EXT2_E_IO;
    return 0;
}

// Abandon a partially-written stream: roll the inode back to a valid empty file
// (keeps it linked, matching ext2_write_data_to_inode's mid-write failure path).
void ext2_wstream_abort(ext2_wstream_t *ws) {
    ext2_lock();
    if (!ws || !ws->ino) { ext2_unlock(); return; }
    ext2_rollback_inode(&g_ext2, ws->ino, 0);
    ws->ok = 0; ws->written = 0; ws->next_lblk = 0;
    ext2_unlock();
}

// Create a directory at an absolute ext2 path.
static int ext2_mkdir_inner(const char *path);

int ext2_mkdir(const char *path) {
    ext2_lock();
    int r = ext2_mkdir_inner(path);
    ext2_unlock();
    return r;
}

static int ext2_mkdir_inner(const char *path) {
    if (!g_ext2.mounted) return -1;
    char base[256];
    uint32_t parent = ext2_parent_and_base(path, base, sizeof(base));
    if (!parent || !base[0]) return -1;
    uint32_t existing = 0; uint8_t t;
    if (ext2_lookup(parent, base, &existing, &t) == 0) return -2;
    uint32_t ino = ext2_alloc_inode(&g_ext2, 1);
    if (!ino) return -3;
    // inode: dir mode 040755, links 2 (self + "."), size = 1 block
    uint8_t *ri = (uint8_t *)kmalloc(g_ext2.inode_size);
    if (!ri) return -3;
    memset(ri, 0, g_ext2.inode_size);
    uint16_t mode = 0x41ED; memcpy(ri + 0, &mode, 2);
    uint32_t sz = g_ext2.block_size; memcpy(ri + 4, &sz, 4);
    uint16_t links = 2; memcpy(ri + 26, &links, 2);
    ext2_stamp_raw_new(ri);   // #115
    int irc = ext2_inode_raw(&g_ext2, ino, ri, 1);   // #695: was discarded
    kfree(ri);
    if (irc != 0) { ext2_free_inode(&g_ext2, ino, 1); return EXT2_E_IO; }
    // first data block with "." and ".."
    uint32_t db = ext2_inode_append_block(&g_ext2, ino, 0);
    if (!db) return -3;
    uint8_t *blk = (uint8_t *)kmalloc(g_ext2.block_size);
    if (!blk) return -3;
    memset(blk, 0, g_ext2.block_size);
    // "."
    memcpy(blk + 0, &ino, 4);
    uint16_t r1 = 12; memcpy(blk + 4, &r1, 2);
    blk[6] = 1; blk[7] = EXT2_FT_DIR; blk[8] = '.';
    // ".." spans rest of block
    memcpy(blk + 12 + 0, &parent, 4);
    uint16_t r2 = (uint16_t)(g_ext2.block_size - 12); memcpy(blk + 12 + 4, &r2, 2);
    blk[12 + 6] = 2; blk[12 + 7] = EXT2_FT_DIR; blk[12 + 8] = '.'; blk[12 + 9] = '.';
    if (ext2_write_block(&g_ext2, db, blk) != 0) {   // #695: was discarded
        // Without "." and ".." on disk this is not a usable directory.
        kfree(blk);
        ext2_rollback_inode(&g_ext2, ino, 1);
        return EXT2_E_IO;
    }
    kfree(blk);
    // link into parent + bump parent links_count (for "..")
    if (ext2_dir_add(&g_ext2, parent, base, ino, EXT2_FT_DIR) != 0) return -3;
    uint8_t *pri = (uint8_t *)kmalloc(g_ext2.inode_size);
    if (pri && ext2_inode_raw(&g_ext2, parent, pri, 0) == 0) {
        uint16_t pl = rd16(pri + 26); pl++; memcpy(pri + 26, &pl, 2);
        // #695: a lost links_count bump is an e2fsck "link count wrong", not a
        // failed mkdir: the directory itself is complete and linked. Report it
        // loudly rather than failing an operation that did succeed.
        if (ext2_inode_raw(&g_ext2, parent, pri, 1) != 0)
            kprintf("[EXT2] mkdir: parent link count not committed (ino=%u)\n", parent);
    }
    if (pri) kfree(pri);
    return 0;
}

// Iterate directory entries by byte position (#99 Phase B). *pos = byte offset to
// resume from; on success returns 0, fills name_out/ino_out/type_out and advances
// *pos. Skips inode-0 slots and "." / "..". Returns -1 at end / on error.
static int ext2_readdir_ino_inner(uint32_t dir_ino, uint32_t *pos, char *name_out, int name_max,
                                  uint32_t *ino_out, uint8_t *type_out);

int ext2_readdir_ino(uint32_t dir_ino, uint32_t *pos, char *name_out, int name_max,
                     uint32_t *ino_out, uint8_t *type_out) {
    ext2_lock();
    int r = ext2_readdir_ino_inner(dir_ino, pos, name_out, name_max, ino_out, type_out);
    ext2_unlock();
    return r;
}

static int ext2_readdir_ino_inner(uint32_t dir_ino, uint32_t *pos, char *name_out, int name_max,
                                  uint32_t *ino_out, uint8_t *type_out) {
    if (!g_ext2.mounted || !pos || !name_out || name_max < 2) return -1;
    ext2_inode_t inode;
    if (ext2_read_inode(dir_ino, &inode) != 0) return -1;
    uint64_t size = inode.i_size;
    uint8_t *blk = (uint8_t *)kmalloc(g_ext2.block_size);
    if (!blk) return -1;
    uint32_t p = *pos;
    while (p < size) {
        uint32_t logical = p / g_ext2.block_size;
        uint32_t off = p % g_ext2.block_size;
        uint32_t phys = ext2_bmap(&g_ext2, &inode, logical);
        if (phys == 0 || phys == EXT2_BMAP_ERR) { p = (logical + 1) * g_ext2.block_size; continue; }
        if (ext2_read_block(&g_ext2, phys, blk) != 0) { kfree(blk); return -1; }
        // The 8-byte entry header must fit before we read it; a corrupt block
        // that parks off in the trailing < 8 bytes otherwise over-reads (#476).
        if (off + 8 > g_ext2.block_size) { p = (logical + 1) * g_ext2.block_size; continue; }
        uint32_t e_ino = rd32(blk + off + 0);
        uint16_t rec   = rd16(blk + off + 4);
        uint8_t  nlen  = blk[off + 6];
        uint8_t  ftype = blk[off + 7];
        if (rec < 8 || off + rec > g_ext2.block_size) {
            // #610: this is not a "no more entries" condition, it is a
            // structurally impossible directory block (the #476 / #597 shape).
            // Record it in the superblock so the next boot checks the volume.
            ext2_mark_error("readdir: rec_len out of range");
            kfree(blk); return -1;
        }
        if ((uint32_t)off + 8 + nlen > g_ext2.block_size) {
            ext2_mark_error("readdir: name_len overruns the block");
            kfree(blk); return -1;
        }
        p += rec;
        if (e_ino != 0 && nlen > 0 &&
            !(nlen == 1 && blk[off + 8] == '.') &&
            !(nlen == 2 && blk[off + 8] == '.' && blk[off + 9] == '.')) {
            int n = nlen; if (n >= name_max) n = name_max - 1;
            for (int i = 0; i < n; i++) name_out[i] = (char)blk[off + 8 + i];
            name_out[n] = 0;
            if (ino_out)  *ino_out  = e_ino;
            if (type_out) *type_out = ftype;
            *pos = p;
            kfree(blk);
            return 0;
        }
    }
    kfree(blk);
    return -1;
}

// ===========================================================================
// DELETE / OVERWRITE support (#99 Phase C). Free blocks + inodes back into the
// bitmaps, truncate an inode's data (direct + single + double indirect), unlink
// a regular file (free its data + inode, then clear its directory entry).
// ===========================================================================

// ---------------------------------------------------------------------------
// #618 BATCHED BLOCK FREES: the App Store install freeze.
//
// MEASURED (build 972 lock-hold profiler, throwaway VM <vmid>): the App Store
// finishes an install by deleting its 103,563,185-byte downloaded archive
// (/STOREDL.TMP). That single sys_unlink held ext2_lock for tens of seconds in
// ONE acquisition, which is why the FIFO ticket lock of #617 could not help:
// there was nothing to be fair BETWEEN. The kernel heartbeat thread writes
// /HEARTBEAT.TXT, which on an ext2 root takes this same lock, so it sat in
// PROC_STATE_BLOCKED on g_ext2_wq for the whole delete and the desktop froze.
//
// The cost is write amplification, not I/O volume. ext2_free_block() freed ONE
// block at a time, and for EVERY block did: read the group block bitmap, clear
// one bit, WRITE the bitmap back; read+WRITE the group descriptor
// (ext2_bgd_adjust); read+WRITE the superblock (ext2_sb_adjust). blk_write is
// write-through, so that is three real device writes plus a real device read
// PER FREED BLOCK, i.e. tens of thousands of device round trips to clear bits
// that nearly all live in the SAME bitmap block. That is exactly the measured
// signature: ~49,000 block-cache operations across the freeze while the
// contended-acquire counter stayed almost flat.
//
// A batch holds the current group's bitmap image in memory, clears every bit
// that falls in that group, and writes the bitmap plus its group descriptor
// ONCE when the group changes or the batch ends; the superblock free count is
// adjusted ONCE for the whole batch. The same bits end up clear and the same
// counters end up correct, so the on-disk result is identical; only the number
// of device writes changes.
//
// CONSISTENCY, and why the lock is NOT dropped inside a batch. While a batch is
// open the in-memory bitmap has bits clear that the ON-DISK bitmap still shows
// as used. An allocator running concurrently would read the disk image, so it
// would simply decline to hand out those blocks (conservative, never wrong),
// but it would also dirty the same bitmap block behind us, and our later flush,
// written from an image read BEFORE that allocation, would silently un-set its
// bits and double-allocate. So the invariant is absolute: a batch is opened and
// closed with ext2_lock held throughout, nothing inside a batch allocates, and
// ext2_lock is never released between the first bitmap read and the final
// flush. Flushing goes through ext2_write_block(), which patches the block
// cache, so no stale cached bitmap survives a batch either.
//
// This stayed C rather than moving to Rust deliberately: it is not new
// separable logic, it is surgery on the existing free path. It must call
// ext2_read_block / ext2_write_block / ext2_bgd_adjust / ext2_sb_adjust and it
// shares ext2_lock with the rest of this file, so a Rust module would have to
// FFI back into all four, buying no safety and adding a seam.
typedef struct {
    int      depth;      // >0 = a batch is open (nesting-safe)
    uint32_t group;      // group whose bitmap is loaded (EXT2_BFB_NOGRP = none)
    uint32_t bbmp;       // that group's bitmap block number
    uint8_t *bm;         // in-memory bitmap image
    uint32_t cleared;    // bits cleared in the loaded bitmap, not yet on disk
    uint32_t total;      // bits cleared across the whole batch (for the sb)
} ext2_bfree_batch_t;
#define EXT2_BFB_NOGRP 0xFFFFFFFFu
static ext2_bfree_batch_t g_bfb = { 0, EXT2_BFB_NOGRP, 0, 0, 0, 0 };

// Write back the currently loaded group bitmap (if any bits were cleared) and
// drop it. Leaves no group loaded.
static void ext2_bfb_flush_group(ext2_fs_t *fs) {
    if (g_bfb.bm && g_bfb.cleared) {
        // #695: this write was unchecked. If the bitmap does not reach the
        // disk the freed bits are still SET there, so bumping the group and
        // superblock free counts makes the counters disagree with the bitmap:
        // e2fsck then reports BOTH a wrong free count and blocks marked in use
        // that nobody owns. Only account for what actually landed.
        if (ext2_write_block(fs, g_bfb.bbmp, g_bfb.bm) == 0) {
            if (ext2_bgd_adjust(fs, g_bfb.group, (int32_t)g_bfb.cleared, 0, 0) != 0)
                ext2_acct_failed("bgd/free_batch");
            g_bfb.total += g_bfb.cleared;
        } else {
            kprintf("[EXT2] block-bitmap writeback FAILED (group=%u blk=%u): "
                    "%u freed bits lost\n",
                    g_bfb.group, g_bfb.bbmp, g_bfb.cleared);
        }
    }
    if (g_bfb.bm) { kfree(g_bfb.bm); g_bfb.bm = 0; }
    g_bfb.group = EXT2_BFB_NOGRP; g_bfb.bbmp = 0; g_bfb.cleared = 0;
}

// Open a batch. MUST be called with ext2_lock held, and no allocation may occur
// before the matching ext2_bfb_end().
static void ext2_bfb_begin(void) { g_bfb.depth++; }

// Close a batch: flush the last group and fold the total into the superblock.
static void ext2_bfb_end(ext2_fs_t *fs) {
    if (g_bfb.depth == 0) return;
    if (--g_bfb.depth) return;                 // still nested
    ext2_bfb_flush_group(fs);
    if (g_bfb.total) {
        if (ext2_sb_adjust(fs, (int32_t)g_bfb.total, 0) != 0) ext2_acct_failed("sb/free_batch");
        g_bfb.total = 0;
    }
}

// Clear one data/metadata block in its group block-bitmap, bump free counts.
static void ext2_free_block(ext2_fs_t *fs, uint32_t blk) {
    if (blk < fs->first_data_block) return;
    uint32_t rel = blk - fs->first_data_block;
    uint32_t g   = rel / fs->blocks_per_group;
    uint32_t idx = rel % fs->blocks_per_group;

    // #618: batched path. Same bit, same counters, one bitmap write per group
    // instead of one per block. Falls through to the single-block path below if
    // the heap cannot give us a bitmap buffer.
    if (g_bfb.depth) {
        if (g_bfb.bm && g_bfb.group != g) ext2_bfb_flush_group(fs);
        if (!g_bfb.bm) {
            uint32_t bb = ext2_bgd_get32(fs, g, 0);   // bg_block_bitmap
            if (!bb) return;
            uint8_t *nbm = (uint8_t *)kmalloc(fs->block_size);
            if (nbm) {
                if (ext2_read_block(fs, bb, nbm) != 0) { kfree(nbm); return; }
                g_bfb.bm = nbm; g_bfb.group = g; g_bfb.bbmp = bb; g_bfb.cleared = 0;
            }
        }
        if (g_bfb.bm) {
            if (g_bfb.bm[idx / 8] & (1 << (idx % 8))) {
                g_bfb.bm[idx / 8] &= (uint8_t)~(1 << (idx % 8));
                g_bfb.cleared++;
            }
            return;
        }
    }

    uint32_t bbmp = ext2_bgd_get32(fs, g, 0);   // bg_block_bitmap
    if (!bbmp) return;
    uint8_t *bm = (uint8_t *)kmalloc(fs->block_size);
    if (!bm) return;
    if (ext2_read_block(fs, bbmp, bm) == 0) {
        if (bm[idx / 8] & (1 << (idx % 8))) {
            bm[idx / 8] &= ~(1 << (idx % 8));
            // #695: unchecked. Adjust the free counts only if the bitmap
            // actually reached the disk, or the counters and the bitmap
            // disagree (an e2fsck "free blocks count wrong").
            if (ext2_write_block(fs, bbmp, bm) == 0) {
                if (ext2_bgd_adjust(fs, g, +1, 0, 0) != 0) ext2_acct_failed("bgd/free_block");
                if (ext2_sb_adjust(fs, +1, 0)        != 0) ext2_acct_failed("sb/free_block");
            } else {
                kprintf("[EXT2] free_block: bitmap write FAILED (blk=%u)\n", blk);
            }
        }
    }
    kfree(bm);
}

// Clear an inode bit in its group inode-bitmap, bump free counts.
static void ext2_free_inode(ext2_fs_t *fs, uint32_t ino, int is_dir) {
    if (ino == 0) return;
    uint32_t g   = (ino - 1) / fs->inodes_per_group;
    uint32_t idx = (ino - 1) % fs->inodes_per_group;
    uint32_t ibmp = ext2_bgd_get32(fs, g, 4);   // bg_inode_bitmap
    if (!ibmp) return;
    uint8_t *bm = (uint8_t *)kmalloc(fs->block_size);
    if (!bm) return;
    if (ext2_read_block(fs, ibmp, bm) == 0) {
        if (bm[idx / 8] & (1 << (idx % 8))) {
            bm[idx / 8] &= ~(1 << (idx % 8));
            // #695: unchecked, same reasoning as ext2_free_block.
            if (ext2_write_block(fs, ibmp, bm) == 0) {
                if (ext2_bgd_adjust(fs, g, 0, +1, is_dir ? -1 : 0) != 0) ext2_acct_failed("bgd/free_inode");
                if (ext2_sb_adjust(fs, 0, +1)                      != 0) ext2_acct_failed("sb/free_inode");
            } else {
                kprintf("[EXT2] free_inode: bitmap write FAILED (ino=%u)\n", ino);
            }
        }
    }
    kfree(bm);
}

// Free every data block referenced by a raw inode (in `ri`) and zero out its
// i_block[] / i_size / i_blocks. Handles direct, single- and double-indirect.
static void ext2_truncate_inode(ext2_fs_t *fs, uint8_t *ri) {
    uint32_t ptrs = fs->block_size / 4;
    uint32_t zero = 0;
    // #618: every block this function frees goes through one batched bitmap per
    // group. All four callers (ext2_unlink, ext2_wstream_begin,
    // ext2_rollback_inode, the ext2_write_file overwrite path) hold ext2_lock,
    // and this function neither allocates nor releases the lock, which is what
    // makes the batch safe. There is no early return between here and the
    // matching ext2_bfb_end().
    ext2_bfb_begin();
    // direct blocks
    for (int i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        uint32_t b = rd32(ri + 40 + i * 4);
        if (b) ext2_free_block(fs, b);
        memcpy(ri + 40 + i * 4, &zero, 4);
    }
    // single indirect
    uint32_t ind = rd32(ri + 40 + EXT2_IND_BLOCK * 4);
    if (ind) {
        uint8_t *ib = (uint8_t *)kmalloc(fs->block_size);
        if (ib) {
            if (ext2_read_block(fs, ind, ib) == 0)
                for (uint32_t k = 0; k < ptrs; k++) {
                    uint32_t b = rd32(ib + k * 4);
                    if (b) ext2_free_block(fs, b);
                }
            kfree(ib);
        }
        ext2_free_block(fs, ind);
        memcpy(ri + 40 + EXT2_IND_BLOCK * 4, &zero, 4);
    }
    // double indirect
    uint32_t dind = rd32(ri + 40 + EXT2_DIND_BLOCK * 4);
    if (dind) {
        uint8_t *ob = (uint8_t *)kmalloc(fs->block_size);
        if (ob) {
            if (ext2_read_block(fs, dind, ob) == 0)
                for (uint32_t o = 0; o < ptrs; o++) {
                    uint32_t ind2 = rd32(ob + o * 4);
                    if (!ind2) continue;
                    uint8_t *ib = (uint8_t *)kmalloc(fs->block_size);
                    if (ib) {
                        if (ext2_read_block(fs, ind2, ib) == 0)
                            for (uint32_t k = 0; k < ptrs; k++) {
                                uint32_t b = rd32(ib + k * 4);
                                if (b) ext2_free_block(fs, b);
                            }
                        kfree(ib);
                    }
                    ext2_free_block(fs, ind2);
                }
            kfree(ob);
        }
        ext2_free_block(fs, dind);
        memcpy(ri + 40 + EXT2_DIND_BLOCK * 4, &zero, 4);
    }
    ext2_bfb_end(fs);            // #618: flush the last group + the superblock
    memcpy(ri + 4,  &zero, 4);   // i_size  = 0
    memcpy(ri + 28, &zero, 4);   // i_blocks = 0
}

// Roll an inode back to empty (free its data + indirect blocks, zero its size).
// When free_ino is set, also release the inode itself; used to clean up after a
// write that failed part-way so the filesystem is never left inconsistent.
static void ext2_rollback_inode(ext2_fs_t *fs, uint32_t ino, int free_ino) {
    uint8_t *ri = (uint8_t *)kmalloc(fs->inode_size);
    if (ri && ext2_inode_raw(fs, ino, ri, 0) == 0) {
        ext2_truncate_inode(fs, ri);
        if (ext2_inode_raw(fs, ino, ri, 1) != 0)
            ext2_acct_failed("inode-writeback/rollback");
    }
    if (ri) kfree(ri);
    if (free_ino) ext2_free_inode(fs, ino, 0);
}

// Write `len` bytes into an (already-sized) inode, growing blocks as needed.
static int ext2_write_data_to_inode(ext2_fs_t *fs, uint32_t ino,
                                    const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t remaining = len, lb = 0;
    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) return -3;
    while (remaining > 0) {
        uint32_t phys = ext2_inode_append_block(fs, ino, lb);
        if (!phys) {
            // Out of space mid-overwrite: roll the inode back to a valid empty
            // file (data lost, but the filesystem stays consistent) rather than
            // leaving a half-written file with dangling size.
            kfree(blk);
            ext2_rollback_inode(fs, ino, 0);
            // #695: the caller truncated this inode BEFORE calling us, and the
            // rollback above frees whatever was written since, so the previous
            // contents are already gone whatever we return. That is the real
            // hazard, and the least we can do is say WHY, so the caller can
            // tell "free something and retry" from "give up".
            return e2_werr_take(EXT2_E_NOSPC);
        }
        uint32_t chunk = remaining < fs->block_size ? remaining : fs->block_size;
        memset(blk, 0, fs->block_size);
        memcpy(blk, p, chunk);
        if (ext2_write_block(fs, phys, blk) != 0) {
            // #695 THE headline Phase 0 bug: this return was DISCARDED and the
            // function then returned 0 unconditionally, so a failing disk made
            // ext2_write_file() report SUCCESS. Every layer above it, up to and
            // including fclose(), then faithfully reported that success. The
            // error-reporting chain was never broken; the error was destroyed
            // at the bottom of it, before it could enter the chain at all.
            kfree(blk);
            ext2_rollback_inode(fs, ino, 0);
            return EXT2_E_IO;
        }
        p += chunk; remaining -= chunk; lb++;
    }
    kfree(blk);
    return 0;
}

// Remove the directory entry for `name` in `dir_ino` (clear its inode field,
// merging its space into the previous entry's rec_len). Returns 0 on success.
static int ext2_dir_remove(ext2_fs_t *fs, uint32_t dir_ino, const char *name) {
    int nlen = 0; while (name[nlen]) nlen++;
    ext2_inode_t di;
    if (ext2_read_inode(dir_ino, &di) != 0) return -1;
    uint32_t nblocks = di.i_size / fs->block_size;
    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) return -1;
    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t phys = ext2_bmap(fs, &di, lb);
        if (!phys) continue;
        if (ext2_read_block(fs, phys, blk) != 0) continue;
        uint32_t off = 0, prev = 0xFFFFFFFF;
        // off + 8 <= block_size keeps the header read in bounds; rec_len and the
        // name field are then validated before use (#476).
        while (off + 8 <= fs->block_size) {
            uint32_t ino = rd32(blk + off);
            uint16_t rec_len = blk[off + 4] | (blk[off + 5] << 8);
            uint8_t  name_len = blk[off + 6];
            if (rec_len < 8 || off + rec_len > fs->block_size) break;
            // #597 (2e) mirror: a live entry whose name does not fit inside its
            // own rec_len is corrupt. Absorbing it into the previous record
            // (prev.rec_len += rec_len) would propagate the bad geometry, so
            // stop scanning this block instead.
            if (ino != 0 && e2_round4((uint32_t)(8 + name_len)) > rec_len) break;
            if (ino != 0 && name_len == nlen &&
                off + 8 + (uint32_t)nlen <= fs->block_size &&
                memcmp(blk + off + 8, name, nlen) == 0) {
                if (prev != 0xFFFFFFFF) {
                    // absorb this slot into the previous entry
                    uint16_t pl = blk[prev + 4] | (blk[prev + 5] << 8);
                    pl += rec_len;
                    blk[prev + 4] = pl & 0xFF; blk[prev + 5] = (pl >> 8) & 0xFF;
                } else {
                    uint32_t z = 0; memcpy(blk + off, &z, 4);  // first entry: just void inode
                }
                // #695: unchecked. If the directory block does not land the
                // entry is still on disk, so reporting removal would be a lie.
                if (ext2_write_block(fs, phys, blk) != 0) {
                    kfree(blk);
                    return -1;
                }
                kfree(blk);
                return 0;
            }
            prev = off;
            off += rec_len;
        }
    }
    kfree(blk);
    return -1;
}

// Public: delete a regular file at an absolute ext2 path.
static int ext2_unlink_inner(const char *path);

int ext2_unlink(const char *path) {
    ext2_lock();
    int r = ext2_unlink_inner(path);
    ext2_unlock();
    return r;
}

// #746: free a regular file's data blocks and its inode. Lifted verbatim out of
// ext2_unlink_inner() so ext2_rename_inner() releases a replaced destination by
// calling the SAME code, rather than carrying a second copy of it that would
// drift (this tree's most repeated fault). Assumes the ext2 lock is held.
//
// A CAVEAT THAT MATTERS FOR RENAME. This does NOT consult i_links_count: it
// assumes the caller has established that the name being removed was the LAST
// one. That assumption is sound here only because ext2_dir_add/ext2_dir_remove
// are the only ways a name is created or destroyed on this volume and there is
// no hard-link primitive. Any future ext2_link() MUST make this decrement and
// check first.
static void ext2_release_inode(uint32_t ino) {
    uint8_t *ri = (uint8_t *)kmalloc(g_ext2.inode_size);
    if (ri) {
        if (ext2_inode_raw(&g_ext2, ino, ri, 0) == 0) {
            ext2_truncate_inode(&g_ext2, ri);
            uint16_t links = 0; memcpy(ri + 26, &links, 2);  // i_links_count = 0
            // i_dtime (offset 20) must be non-zero on a deleted inode, else
            // e2fsck reports "deleted inode has zero dtime".
            uint32_t dtime = 1781000000u; memcpy(ri + 20, &dtime, 4);
            if (ext2_inode_raw(&g_ext2, ino, ri, 1) != 0)
                ext2_acct_failed("inode-writeback/unlink");
        }
        kfree(ri);
    }
    ext2_free_inode(&g_ext2, ino, 0);
}

static int ext2_unlink_inner(const char *path) {
    if (!g_ext2.mounted) return -1;
    char base[256];
    uint32_t parent = ext2_parent_and_base(path, base, sizeof(base));
    if (!parent || !base[0]) return -1;
    uint32_t ino = 0; uint8_t ft = 0;
    if (ext2_lookup(parent, base, &ino, &ft) != 0) return -1;  // not found
    if (ft == EXT2_FT_DIR) return -2;                          // rmdir not supported
    ext2_release_inode(ino);                                   // #746
    return ext2_dir_remove(&g_ext2, parent, base);
}

// ===========================================================================
// #746: ATOMIC REPLACE. rename() RELINKS; it must never copy file data.
// ===========================================================================
//
// WHAT WAS WRONG. There was no ext2_rename at all. fat_rename() emulated it for
// ext2 paths as fat_copy() + fat_delete(), i.e. read every byte of the source
// and write every byte to the destination. That makes the standard safe-save
// pattern - write a temp file, then rename it over the real one - NOT SAFE on
// this system, because the copy can fail half way and leave the destination
// truncated. The pattern exists precisely to make a partial write unobservable,
// and here the rename itself was the partial write. userland/libc/userconf.c
// documents the pattern as unavailable for exactly this reason.
//
// WHY A RELINKING rename() AND NOT A NEW "atomic write" SYSCALL.
//   1. Every caller already knows rename(). Fixing rename fixes the ported code
//      we did not write (vi, busybox, curl, the Python ports) at the same time
//      as our own. A bespoke primitive only helps the call sites we edit, and
//      the ports would keep using the broken one.
//   2. The reason temp+rename is unsafe here is THAT rename copies. Removing
//      the copy removes the failure mode by construction. Adding a second
//      mechanism beside a broken one leaves the broken one to be found later.
//   3. No file data is touched, so there is no half-written destination to
//      reason about at all.
//
// WHAT IS AND IS NOT GUARANTEED. There is no journal on this volume, so this is
// not atomic against power loss in the strict sense, and no honest
// implementation on plain ext2 can be. What IS guaranteed, and what the
// safe-save pattern actually needs, is that NO CRASH WINDOW LOSES DATA:
//
//   * Replacing an existing destination is ONE directory-block write
//     (ext2_dirblock_repoint): the name resolves either to the old inode or to
//     the new one, never to nothing.
//   * Creating a new destination name is one block write, then removing the
//     source name is another. A crash between them leaves TWO names for one
//     inode: an inflated link count that e2fsck repairs, and both names read
//     the correct, complete file.
//   * The inode the destination used to name is released only AFTER the
//     destination name no longer refers to it.
//
// The destination is therefore never truncated and the source is never lost,
// which is exactly the property copy-then-delete could not offer.
static int ext2_rename_inner(const char *old_path, const char *new_path);

int ext2_rename(const char *old_path, const char *new_path) {
    ext2_lock();
    int r = ext2_rename_inner(old_path, new_path);
    ext2_unlock();
    return r;
}

// Repoint the record for `name` in directory `dir_ino` at `new_ino`, in place.
// Returns 0 on success, -1 if the name is not there or the write failed.
// Assumes the ext2 lock is held.
static int ext2_dir_repoint(ext2_fs_t *fs, uint32_t dir_ino, const char *name,
                            uint32_t new_ino, uint8_t ftype) {
    uint32_t nlen = 0; while (name[nlen]) nlen++;
    if (nlen == 0 || nlen > 255) return -1;
    ext2_inode_t di;
    if (ext2_read_inode(dir_ino, &di) != 0) return -1;
    uint32_t nblocks = di.i_size / fs->block_size;
    uint8_t *blk = (uint8_t *)kmalloc(fs->block_size);
    if (!blk) return -1;
    for (uint32_t lb = 0; lb < nblocks; lb++) {
        uint32_t phys = ext2_bmap(fs, &di, lb);
        if (phys == 0 || phys == EXT2_BMAP_ERR) continue;
        if (ext2_read_block(fs, phys, blk) != 0) continue;
        int r = ext2_dirblock_repoint(blk, fs->block_size, name, nlen, new_ino, ftype);
        if (r == 1) {
            // THE one write that makes the replacement atomic. If it does not
            // land, the destination still names the OLD inode, which is a
            // consistent state; report failure so the caller does not go on to
            // free that inode.
            if (ext2_write_block(fs, phys, blk) != 0) { kfree(blk); return -1; }
            kfree(blk);
            return 0;
        }
        // r == 0: not in this block. r == -1: this block is not walkable; the
        // scan and the remove both stop on that, so do the same.
        if (r < 0) break;
    }
    kfree(blk);
    return -1;
}

static int ext2_rename_inner(const char *old_path, const char *new_path) {
    if (!g_ext2.mounted) return -1;
    char obase[256], nbase[256];
    uint32_t oparent = ext2_parent_and_base(old_path, obase, sizeof(obase));
    if (!oparent || !obase[0]) return -1;
    uint32_t nparent = ext2_parent_and_base(new_path, nbase, sizeof(nbase));
    if (!nparent || !nbase[0]) return -1;

    uint32_t oino = 0; uint8_t otype = 0;
    if (ext2_lookup(oparent, obase, &oino, &otype) != 0 || oino == 0) return -1;

    // POSIX: rename(x, x) succeeds and changes nothing. Getting this wrong here
    // would DELETE the file, since the two names are one name.
    if (oparent == nparent && strcmp(obase, nbase) == 0) return 0;

    uint32_t nino = 0; uint8_t ntype = 0;
    int have_dest = (ext2_lookup(nparent, nbase, &nino, &ntype) == 0 && nino != 0);
    if (have_dest && nino == oino) return 0;   // already two names for one inode

    // Directories are refused, with the same -2 ext2_unlink uses for the same
    // reason: moving a directory must also rewrite its ".." entry and adjust
    // both parents' link counts, and getting that half-right corrupts the tree.
    // Reported honestly rather than emulated, so the caller can fall back.
    if (otype == EXT2_FT_DIR) return -2;
    if (have_dest && ntype == EXT2_FT_DIR) return -2;

    // STEP 1: the destination name starts resolving to the source inode.
    if (have_dest) {
        if (ext2_dir_repoint(&g_ext2, nparent, nbase, oino, otype) != 0) return -1;
    } else {
        if (ext2_dir_add(&g_ext2, nparent, nbase, oino, otype) != 0) return -1;
    }

    // From here the caller's data is safe under any crash: it is named by the
    // destination. Everything below is cleanup, and a failure in it costs disk
    // space (which e2fsck reclaims), never data.

    // STEP 2: the source name goes away. If this fails the file simply has two
    // names; say so rather than reporting a failed rename the caller would
    // retry, because the rename DID happen.
    if (ext2_dir_remove(&g_ext2, oparent, obase) != 0)
        ext2_acct_failed("dir-remove/rename");

    // STEP 3: and only now is the inode the destination used to name released.
    if (have_dest) ext2_release_inode(nino);
    return 0;
}

// ===========================================================================
// #736 Stage 2: REMOVE AN EMPTY DIRECTORY.
//
// ext2_unlink_inner() refuses a directory with -2, so INT 21h AH=3Ah, the Files
// app and every other caller got a failure on the shipping ext2-rooted image
// while the same call worked on the FAT ESP. Nothing was missing except this
// function: the block/inode/dirent primitives it uses are the ones unlink
// already uses.
//
// The three things that make it rmdir rather than unlink-on-a-directory, and
// that e2fsck checks:
//   1. IT MUST BE EMPTY. Only "." and ".." may remain; anything else is -3, and
//      the caller must see a distinct code rather than a generic failure.
//   2. THE PARENT LOSES A LINK. A subdirectory's ".." is a hard link to its
//      parent, so the parent's i_links_count is decremented. Skipping this
//      leaves e2fsck reporting a wrong link count on the parent forever.
//   3. THE INODE IS FREED AS A DIRECTORY. ext2_free_inode(..., is_dir=1)
//      decrements the block group's used_dirs_count; freeing it as a file
//      leaves that counter permanently high.
//
// Returns 0 on success, -3 if not empty, -1 on any other failure.
// ===========================================================================
static int ext2_rmdir_inner(const char *path) {
    if (!g_ext2.mounted) return -1;
    char base[256];
    uint32_t parent = ext2_parent_and_base(path, base, sizeof(base));
    if (!parent || !base[0]) return -1;
    uint32_t ino = 0; uint8_t ft = 0;
    if (ext2_lookup(parent, base, &ino, &ft) != 0) return -1;   // not found
    if (ft != EXT2_FT_DIR) return -1;                           // not a directory
    if (ino == EXT2_ROOT_INO) return -1;                        // never the root

    // 1. EMPTY?
    {
        uint32_t pos = 0, e_ino = 0;
        uint8_t  e_ft = 0;
        char nm[256];
        while (ext2_readdir_ino(ino, &pos, nm, (int)sizeof(nm), &e_ino, &e_ft) == 0) {
            if (nm[0] == '.' && nm[1] == '\0') continue;
            if (nm[0] == '.' && nm[1] == '.' && nm[2] == '\0') continue;
            return -3;                                          // has children
        }
    }

    // 2. free the directory's own blocks and inode
    uint8_t *ri = (uint8_t *)kmalloc(g_ext2.inode_size);
    if (ri) {
        if (ext2_inode_raw(&g_ext2, ino, ri, 0) == 0) {
            ext2_truncate_inode(&g_ext2, ri);
            uint16_t links = 0; memcpy(ri + 26, &links, 2);     // i_links_count
            uint32_t dtime = 1781000000u; memcpy(ri + 20, &dtime, 4);
            if (ext2_inode_raw(&g_ext2, ino, ri, 1) != 0)
                ext2_acct_failed("inode-writeback/rmdir");
        }
        kfree(ri);
    }
    ext2_free_inode(&g_ext2, ino, 1);                           // is_dir = 1

    // 3. the parent loses the link its ".." entry held
    {
        uint8_t *rp = (uint8_t *)kmalloc(g_ext2.inode_size);
        if (rp) {
            if (ext2_inode_raw(&g_ext2, parent, rp, 0) == 0) {
                uint16_t links = 0; memcpy(&links, rp + 26, 2);
                if (links > 2) { links--; memcpy(rp + 26, &links, 2);
                                 // #742: found by the MUST_CHECK attribute, NOT by the
                                 // static sweep, which did not see it because the call is
                                 // not at statement start. A lost write here leaves the
                                 // parent directory claiming a link to the subdirectory
                                 // that was just removed.
                                 if (ext2_inode_raw(&g_ext2, parent, rp, 1) != 0)
                                     ext2_acct_failed("inode-writeback/rmdir-parent"); }
            }
            kfree(rp);
        }
    }
    return ext2_dir_remove(&g_ext2, parent, base);
}

int ext2_rmdir(const char *path) {
    ext2_lock();
    int r = ext2_rmdir_inner(path);
    ext2_unlock();
    return r;
}

// ===========================================================================
// Root-cutover helper (#99 Phase C). Read an entire regular file from the
// mounted ext2 volume into a freshly kmalloc'd buffer. Returns NULL if the
// volume is not mounted, the path is missing, or it is a directory. Used by the
// fat_read_file() ext2-first hook when g_root_ext2 is set.
// ===========================================================================
void *ext2_read_whole(const char *path, uint32_t *size_out) {
    if (!g_ext2.mounted || !path) return NULL;
    uint32_t ino = ext2_resolve_path(path);
    if (!ino) return NULL;
    ext2_inode_t in;
    if (ext2_read_inode(ino, &in) != 0) return NULL;
    if ((in.i_mode & 0xF000) == 0x4000) return NULL;   // directory, not a file
    uint32_t sz = in.i_size;
    // #tls-suppressfix: allocate sz+1 and NUL-terminate, matching the FAT half
    // of this same interface. fat_read_file() has always done kmalloc(size+1)
    // plus a terminator; ext2_read_whole() allocated exactly i_size and wrote
    // none, so callers that reached the ext2 branch of fat_read_file() got a
    // buffer that looked identical and behaved differently. The TLS trust-store
    // loader read /CONFIG/CACERTS.PEM this way and scanned it with strstr, off
    // the end of the allocation, on every boot of the ext2-root golden. The PEM
    // scanners are now length-bounded and no longer rely on this, but an
    // interface whose two implementations disagree about termination will keep
    // producing this bug for the next caller, so terminate here too.
    void *buf = (void *)kmalloc((sz ? sz : 1) + 1);
    if (!buf) {
        // #COMPRESPAWN: THIS WAS A BARE `return NULL` AND IT LIED ABOUT ITSELF.
        //
        // fat_read_file() (fs/fat.c) treats NULL from here as "not present on
        // ext2" and falls through to the FAT ESP, which then reports
        // "[FAT] fat_read_file: open failed: <path>". So on an ext2-root image
        // - i.e. every current golden - a KERNEL HEAP EXHAUSTION reading an app
        // binary presented, in the only log that survives a reboot, as a
        // MISSING FILE. That is the worst possible mis-report: it points the
        // reader at the disk image while the real fault is memory pressure.
        //
        // One line. It costs nothing on the path that already failed.
        kprintf("[EXT2] OUT OF MEMORY reading %s: kmalloc(%u) failed. "
                "This is NOT a missing file; the caller may report it as one.\n",
                path, (unsigned)(sz + 1));
        bootlog_write("[EXT2] OOM reading %s (needed %u bytes) - a caller that "
                      "reports this as a missing file is mis-reporting it",
                      path, (unsigned)(sz + 1));
        return NULL;
    }
    int64_t n = sz ? ext2_read_file_ino(ino, buf, sz) : 0;
    if (n < 0) { kfree(buf); return NULL; }
    // The reader is bounded by sz and the allocation is sz+1, so buf[n] is in
    // range. Asserted rather than assumed: this is the index that turns a
    // reader bug into a heap write.
    if ((uint64_t)n > (uint64_t)sz) { kfree(buf); return NULL; }
    ((uint8_t *)buf)[n] = 0;   // NUL-terminate (allocation is sz+1)
    if (size_out) *size_out = (uint32_t)n;
    return buf;
}

// Is the ext2 volume currently mounted? (for the root-cutover marker check)
int ext2_is_mounted(void) { return g_ext2.mounted; }

// #539: absolute device byte offset just past the END of the mounted ext2
// volume, i.e. part_start_lba*512 + blocks_count*block_size. Used by main.c to
// size the USB TO-RAM window so it spans the whole two-partition image (FAT ESP
// + ext2 ROOT), not just the FAT ESP. Returns 0 if no ext2 volume is mounted.
uint64_t ext2_end_bytes(void) {
    if (!g_ext2.mounted) return 0;
    return (uint64_t)g_ext2.part_start_lba * EXT2_SECTOR_SIZE +
           (uint64_t)g_ext2.blocks_count * (uint64_t)g_ext2.block_size;
}


// ===========================================================================
// #610 SUPERBLOCK STATE (dirty detection) + THE READ-ONLY CONSISTENCY CHECKER
//
// MEASURED STARTING POINT: before this change the driver read s_state,
// s_mnt_count, s_lastcheck, s_max_mnt_count and s_checkinterval EXACTLY NEVER.
// `grep -rn 's_state\|s_mnt_count\|s_lastcheck\|s_checkinterval\|VALID_FS'`
// over the whole kernel tree returned nothing from fs/. That means a MayteraOS
// volume yanked mid-write still reported "clean" to a Linux host, and every
// "e2fsck says clean" result this project ever leaned on was clean by luck
// rather than because the volume was ever KNOWN to have been shut down cleanly.
//
// What we now do, and nothing more:
//   mount            -> clear EXT2_VALID_FS (fs is dirty), bump s_mnt_count
//   clean unmount    -> set EXT2_VALID_FS   (ACPI poweroff/reboot path)
//   driver sees rot  -> set EXT2_ERROR_FS   (sticky; only a clean CHECK clears it)
//   verified clean   -> clear EXT2_ERROR_FS (only after a full check found zero
//                       problems: never mark clean what was not verified)
//
// We deliberately DO NOT write s_mtime/s_wtime/s_lastcheck: this kernel has no
// wall clock (no CMOS/RTC driver), and stamping 0 into a time field is worse
// than leaving the host's value alone. The same absence is why the
// s_checkinterval trigger is NOT implemented; it is unimplementable without a
// clock, and pretending otherwise would fire a full scan on every boot.
// ===========================================================================

// EXT2_VALID_FS / EXT2_ERROR_FS / E2ST_* live near ext2_mount() (which marks
// the volume dirty) so the mount path can use them before this section.

// Superblock byte offsets (relative to the 1024-byte superblock).
#define E2SB_FREE_BLOCKS     12
#define E2SB_FREE_INODES     16
#define E2SB_MNT_COUNT       52
#define E2SB_MAX_MNT_COUNT   54
#define E2SB_MAGIC           56
#define E2SB_STATE           58
#define E2SB_LASTCHECK       64
#define E2SB_CHECKINTERVAL   68
#define E2SB_REV_LEVEL       76
#define E2SB_FIRST_INO       84
#define E2SB_FEATURE_ROCOMPAT 100
#define E2SB_RESERVED_GDT    206
#define E2SB_ROCOMPAT_SPARSE_SUPER 0x0001u

static int g_e2_error_marked = 0;   // one loud ERROR write per boot, not a storm
static int g_e2fsck_ffi_ok  = 1;    // cleared if the Rust FFI sizeof lock fails

// Read/write the PRIMARY superblock. Backup superblocks are intentionally left
// alone: a half-updated set of backups is worse than stale backups, and nothing
// in this kernel reads them.
static int ext2_sb_read_raw(const ext2_fs_t *fs, uint8_t *sb1024) {
    return (blk_read(fs->channel, fs->drive, fs->part_start_lba + 2, 2, sb1024) == 2)
           ? 0 : -1;
}

// The ONLY function in #610 that writes to the filesystem. It rewrites the
// superblock it just read with at most two 16-bit fields changed, refuses to
// write if the magic is wrong, and READS THE RESULT BACK (#444: ATA DMA has
// been measured returning zero-filled chunks under load; a silently corrupted
// superblock write would be far worse than never marking the volume dirty).
static int ext2_sb_set_state(ext2_fs_t *fs, int op, int bump_mnt) {
    uint8_t *sb = (uint8_t *)kmalloc(1024);
    if (!sb) return -1;
    if (ext2_sb_read_raw(fs, sb) != 0) { kfree(sb); return -1; }
    if (rd16(sb + E2SB_MAGIC) != EXT2_MAGIC) { kfree(sb); return -2; }

    uint16_t cur = rd16(sb + E2SB_STATE);
    uint16_t nv  = cur;
    switch (op) {
        case E2ST_DIRTY:          nv = (uint16_t)(cur & ~EXT2_VALID_FS); break;
        case E2ST_CLEAN:          nv = (uint16_t)(cur | EXT2_VALID_FS);  break;
        case E2ST_ERROR:          nv = (uint16_t)(cur | EXT2_ERROR_FS);  break;
        case E2ST_VERIFIED_CLEAN: nv = (uint16_t)((cur | EXT2_VALID_FS) & ~EXT2_ERROR_FS); break;
        default: kfree(sb); return -3;
    }
    memcpy(sb + E2SB_STATE, &nv, 2);

    uint16_t mc = rd16(sb + E2SB_MNT_COUNT);
    if (bump_mnt) {
        mc = (uint16_t)(mc + 1);
        memcpy(sb + E2SB_MNT_COUNT, &mc, 2);
    }

    int rc = (blk_write(fs->channel, fs->drive, fs->part_start_lba + 2, 2, sb) == 2) ? 0 : -4;
    kfree(sb);
    if (rc != 0) return rc;

    // Read-back verify. If this fails the volume's superblock may now be
    // damaged, and that must be LOUD, not inferred later from a boot failure.
    uint8_t *vb = (uint8_t *)kmalloc(1024);
    if (!vb) return 0;                       // cannot verify; do not claim failure
    int vr = ext2_sb_read_raw(fs, vb);
    if (vr == 0) {
        if (rd16(vb + E2SB_MAGIC) != EXT2_MAGIC) {
            kprintf("[EXT2] #610: SUPERBLOCK READ-BACK LOST ITS MAGIC after a state write\n");
            rc = -5;
        } else if (rd16(vb + E2SB_STATE) != nv) {
            kprintf("[EXT2] #610: superblock state write did not stick (want %u got %u)\n",
                    (unsigned)nv, (unsigned)rd16(vb + E2SB_STATE));
            rc = -6;
        } else {
            fs->sb_mnt_count = rd16(vb + E2SB_MNT_COUNT);
        }
    }
    kfree(vb);
    return rc;
}

// Public: mark the volume clean. Called from the ACPI poweroff/reboot flush
// path (drivers/acpi.c) and from the shell's shutdown command, so a graceful
// stop is distinguishable from a yanked USB stick. Idempotent.
void ext2_mark_clean(void) {
    if (!g_ext2.mounted) return;
    int rc = ext2_sb_set_state(&g_ext2, E2ST_CLEAN, 0);
    if (rc == 0) {
        kprintf("[EXT2] #610: volume marked CLEAN (s_state |= EXT2_VALID_FS)\n");
    } else {
        kprintf("[EXT2] #610: WARNING could not mark volume clean (rc=%d)\n", rc);
    }
}

// Public: mark the volume DIRTY again. #229. Needed wherever a clean mark has
// been written but the volume then turns out to still be mounted and writable:
// a shutdown that failed to power the machine off, or any future path that
// flushes speculatively. Marking clean must never outlive the moment the
// volume actually stops being written, or a power cut after that point would
// look like a graceful stop and the next boot would skip the check that would
// have caught it. Idempotent.
void ext2_mark_dirty(void) {
    if (!g_ext2.mounted) return;
    int rc = ext2_sb_set_state(&g_ext2, E2ST_DIRTY, 0);
    if (rc == 0) {
        kprintf("[EXT2] #229: volume re-marked DIRTY (still mounted and writable)\n");
    } else {
        kprintf("[EXT2] #229: WARNING could not re-mark volume dirty (rc=%d)\n", rc);
    }
}

// Public: the driver has just seen something that cannot happen on a healthy
// filesystem. Record it in the superblock so the NEXT boot (and any Linux host)
// knows this volume needs checking. Sticky: only a full clean check clears it.
void ext2_mark_error(const char *why) {
    if (!g_ext2.mounted) return;
    if (g_e2_error_marked) return;
    g_e2_error_marked = 1;
    kprintf("[EXT2] #610: INCONSISTENCY DETECTED (%s) -> marking EXT2_ERROR_FS\n",
            why ? why : "unspecified");
    bootlog_write("[EXT2] #610: inconsistency detected (%s), volume marked with error",
                  why ? why : "unspecified");
    IGNORE_RESULT("we are already reporting an inconsistency on serial and in "
                  "the bootlog; if the superblock write ALSO fails there is no "
                  "further channel and no retry that could help",
                  ext2_sb_set_state(&g_ext2, E2ST_ERROR, 0));
}

// Public: why (if at all) this volume wants checking. Bit 0 = not cleanly
// unmounted, bit 1 = an error was recorded, bit 2 = mount count exceeded.
// Bit 3 (check interval) is deliberately never set: see the header comment.
uint32_t ext2_fsck_needed(void) {
    if (!g_ext2.mounted) return 0;
    uint32_t why = 0;
    if (!(g_ext2.sb_state_at_mount & EXT2_VALID_FS)) why |= 1u;
    if (g_ext2.sb_state_at_mount & EXT2_ERROR_FS)    why |= 2u;
    if (g_ext2.sb_max_mnt_count > 0 &&
        (int32_t)g_ext2.sb_mnt_count >= (int32_t)g_ext2.sb_max_mnt_count) why |= 4u;
    return why;
}

uint16_t ext2_state_at_mount(void) { return g_ext2.sb_state_at_mount; }
uint16_t ext2_mnt_count(void)      { return g_ext2.sb_mnt_count; }

// ---------------------------------------------------------------------------
// THE CHECKER. C owns I/O + memory; rustkern/ext2fsck.rs owns every parse,
// every bound and every verdict. See that file for what is checked and why.
// ---------------------------------------------------------------------------

// Layout lock: these MUST match the #[repr(C)] structs in rustkern/ext2fsck.rs.
// A silent drift here is a wild pointer, so the sizes are asserted at compile
// time here AND re-checked at runtime against ext2_fsck_sizeof_rs() in the boot
// self-test (a _Static_assert can only see the C side of the pair).
_Static_assert(sizeof(ext2_fsck_geom_t)    == 80,  "#610 E2fsckGeom layout drift");
_Static_assert(sizeof(ext2_fsck_scratch_t) == 112, "#610 E2fsckScratch layout drift");
_Static_assert(sizeof(ext2_fsck_report_t) == 200, "#610 E2fsckReport layout drift");

extern int      ext2_fsck_run_rs(const ext2_fsck_geom_t *g,
                                 const ext2_fsck_scratch_t *s,
                                 ext2_fsck_report_t *rep);
extern uint32_t ext2_fsck_sizeof_rs(uint32_t which);

// The single capability Rust is granted. Deliberately does NOT take ext2_lock:
// a full scan under the FS mutex is the exact shape of the #618 App Store
// freeze (one multi-second lock acquisition starves the heartbeat thread and
// the desktop wedges). Per-block reads go through the existing block cache,
// which has its own spinlock. The cost is that a check of a LIVE, being-written
// filesystem can see a torn view; that is inherent to checking a mounted fs
// (host `e2fsck -fn` on a mounted volume has the same caveat) and the caller
// prints it. The boot-time check runs before userland, where nothing writes.
static int e2fsck_read_block_cb(uint32_t block, uint8_t *dst) {
    if (!g_ext2.mounted) return -1;
    return (ext2_read_block(&g_ext2, block, dst) == 0) ? 0 : -1;
}

// Pass names. The numbering is what this checker ACTUALLY does, not a copy of
// e2fsck's table: passes 3 and 4 (connectivity and reference counts) are one
// loop over the inode table here, and splitting the display would imply a
// second sweep that does not happen. Index is the `pass` value Rust sends.
static const char *e2fsck_pass_name(uint32_t pass) {
    switch (pass) {
        case 0: return "Scanning metadata (superblock, descriptors, bitmaps, inode tables)";
        case 1: return "Pass 1/4  Checking inodes, blocks and sizes";
        case 2: return "Pass 2/4  Checking directory structure";
        case 4: return "Pass 3/4  Checking directory connectivity and reference counts";
        case 5: return "Pass 4/4  Checking group summary information";
        default: return "Checking";
    }
}

static int      g_fsck_ui = 0;          // paint to the boot splash?
static uint32_t g_fsck_last_pass = 0xFFFFFFFFu;
static uint64_t g_fsck_last_paint_ms = 0;
static uint32_t g_fsck_ms = 0;
static uint32_t g_fsck_paint_ms = 0;    // wall time spent PAINTING progress
static uint32_t g_fsck_paints = 0;      // number of repaints
static uint32_t g_fsck_ticks = 0;       // number of progress calls

uint32_t ext2_fsck_last_ms(void)       { return g_fsck_ms; }
uint32_t ext2_fsck_last_paint_ms(void) { return g_fsck_paint_ms; }
uint32_t ext2_fsck_last_paints(void)   { return g_fsck_paints; }

// Called from Rust. Returns non-zero to ABORT the scan.
//
// THROTTLING: the paint, not the call. Rust ticks once per inode-table block
// (and once per group), which is already coarse, but a full repaint of the
// boot-log console per tick would make the display dominate the check's
// runtime. 100 ms is fast enough to look live and slow enough to be free; the
// measured cost is in the CHANGELOG entry, not assumed.
static int e2fsck_progress_cb(uint32_t pass, uint32_t cur, uint32_t total) {
    extern int keyboard_has_char(void);
    extern int keyboard_get_char(void);
    extern void gfx_boot_log_replace(const char *);
    extern void gfx_boot_log(const char *);

    // ESC is checked on EVERY tick, not on the painted ones: a user holding
    // ESC through a slow check must not have to hit the 100 ms window.
    // #616: this callback runs from the BOOT filesystem check, where main()
    // has not yet called sti(): the IRQ1 handler that normally fills the
    // keyboard ring cannot run, so keyboard_has_char() was permanently false
    // and the advertised ESC skip did nothing at all. Pump both input paths by
    // hand first, then use the ordinary cooked-key API unchanged.
    //   - keyboard_poll_i8042(): PS/2, i.e. every VM and any box with an i8042.
    //   - usb_hid_poll_all():    USB HID, i.e. the real iMac14,4, which has no
    //     PS/2 port at all and whose keyboard sits behind a hub. usb_init() has
    //     already enumerated it by this point (main.c), but its poll WORKER is
    //     a scheduler thread that cannot run until after sti(), so the
    //     synchronous poll is the only way to see a key here.
    // Both are bounded and both are no-ops when the corresponding device is
    // absent, so this costs nothing on a machine that has only one of them.
    { extern void keyboard_poll_i8042(void); keyboard_poll_i8042(); }
    { extern void usb_hid_poll_all(void);    usb_hid_poll_all();    }

    if (keyboard_has_char()) {
        int ch = keyboard_get_char();
        if (ch == 27) {
            kprintf("[FSCK] ESC pressed: aborting the check\n");
            if (g_fsck_ui) gfx_boot_log("[FSCK] Skipped by user (ESC). Filesystem NOT verified.");
            return 1;
        }
    }
    g_fsck_ticks++;
    if (!g_fsck_ui) return 0;

    uint64_t now = mono_ms();
    int newpass = (pass != g_fsck_last_pass);
    int final   = (total != 0 && cur >= total);
    if (!newpass && !final && (now - g_fsck_last_paint_ms) < 100) return 0;
    g_fsck_last_paint_ms = now;

    char line[128];
    uint32_t pct = total ? (uint32_t)(((uint64_t)cur * 100u) / total) : 0u;
    if (pct > 100) pct = 100;
    if (total) {
        snprintf(line, sizeof(line), "%s  %u/%u (%u%%)  [ESC skips]",
                 e2fsck_pass_name(pass), (unsigned)cur, (unsigned)total, (unsigned)pct);
    } else {
        snprintf(line, sizeof(line), "%s  [ESC skips]", e2fsck_pass_name(pass));
    }
    uint64_t p0 = mono_ms();
    if (newpass) {
        g_fsck_last_pass = pass;
        gfx_boot_log(line);          // new pass: a new line
        kprintf("[FSCK] %s\n", e2fsck_pass_name(pass));
    } else {
        gfx_boot_log_replace(line);  // same pass: update in place
    }
    g_fsck_paint_ms += (uint32_t)(mono_ms() - p0);
    g_fsck_paints++;
    // Serial gets a coarser cadence than the screen so a headless boot log is
    // still readable, but it does get the same information.
    if (final && total) {
        kprintf("[FSCK]   %s: %u/%u (100%%)\n", e2fsck_pass_name(pass),
                (unsigned)cur, (unsigned)total);
    }
    return 0;
}

int ext2_fsck_run(ext2_fsck_report_t *out) { return ext2_fsck_run_ex(out, 0); }

int ext2_fsck_run_ex(ext2_fsck_report_t *out, int ui) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!g_ext2.mounted) return -1;
    if (!g_e2fsck_ffi_ok) return -4;   // layout drift: refuse rather than guess

    ext2_fsck_geom_t g;
    memset(&g, 0, sizeof(g));
    g.block_size          = g_ext2.block_size;
    g.blocks_count        = g_ext2.blocks_count;
    g.inodes_count        = g_ext2.inodes_count;
    g.first_data_block    = g_ext2.first_data_block;
    g.blocks_per_group    = g_ext2.blocks_per_group;
    g.inodes_per_group    = g_ext2.inodes_per_group;
    g.inode_size          = g_ext2.inode_size;
    g.groups_count        = g_ext2.groups_count;
    g.bgd_table_block     = g_ext2.bgd_table_block;
    g.reserved_gdt_blocks = g_ext2.sb_reserved_gdt;
    g.first_ino           = g_ext2.sb_first_ino;
    g.sparse_super        = g_ext2.sb_sparse_super;

    // Free counts are re-read here rather than trusted from mount time: the
    // driver has been allocating since, and comparing the checker's tally
    // against a stale number would be a guaranteed false positive.
    {
        uint8_t *sb = (uint8_t *)kmalloc(1024);
        if (!sb) return -2;
        if (ext2_sb_read_raw(&g_ext2, sb) != 0) { kfree(sb); return -2; }
        g.sb_free_blocks = rd32(sb + E2SB_FREE_BLOCKS);
        g.sb_free_inodes = rd32(sb + E2SB_FREE_INODES);
        kfree(sb);
    }
    g.read_block = e2fsck_read_block_cb;
    g.progress   = e2fsck_progress_cb;
    g_fsck_ui = ui;
    g_fsck_paint_ms = 0;
    g_fsck_paints = 0;
    g_fsck_ticks = 0;
    g_fsck_last_pass = 0xFFFFFFFFu;
    g_fsck_last_paint_ms = 0;
    uint64_t t_start = mono_ms();

    uint32_t bmap_bytes    = ((g.blocks_count - g.first_data_block) + 7u) / 8u;
    uint32_t imap_bytes    = (g.inodes_count + 7u) / 8u;
    uint32_t links_entries = g.inodes_count + 1u;
    uint32_t bs            = g.block_size;

    ext2_fsck_scratch_t s;
    memset(&s, 0, sizeof(s));
    s.bmap_bytes    = bmap_bytes;
    s.imap_bytes    = imap_bytes;
    s.links_entries = links_entries;
    s.blk_used  = (uint8_t *)kmalloc(bmap_bytes);
    s.blk_dup   = (uint8_t *)kmalloc(bmap_bytes);
    s.ino_used  = (uint8_t *)kmalloc(imap_bytes);
    s.ino_isdir = (uint8_t *)kmalloc(imap_bytes);
    s.links     = (uint16_t *)kmalloc(links_entries * 2u);
    s.itbuf     = (uint8_t *)kmalloc(bs);
    s.dirbuf    = (uint8_t *)kmalloc(bs);
    s.ind0      = (uint8_t *)kmalloc(bs);
    s.ind1      = (uint8_t *)kmalloc(bs);
    s.ind2      = (uint8_t *)kmalloc(bs);
    s.bmapbuf   = (uint8_t *)kmalloc(bs);
    s.gdbuf     = (uint8_t *)kmalloc(bs);

    int rc;
    if (!s.blk_used || !s.blk_dup || !s.ino_used || !s.ino_isdir || !s.links ||
        !s.itbuf || !s.dirbuf || !s.ind0 || !s.ind1 || !s.ind2 || !s.bmapbuf || !s.gdbuf) {
        rc = -3;
    } else {
        rc = ext2_fsck_run_rs(&g, &s, out);
    }
    g_fsck_ms = (uint32_t)(mono_ms() - t_start);
    g_fsck_ui = 0;

    if (s.blk_used)  kfree(s.blk_used);
    if (s.blk_dup)   kfree(s.blk_dup);
    if (s.ino_used)  kfree(s.ino_used);
    if (s.ino_isdir) kfree(s.ino_isdir);
    if (s.links)     kfree(s.links);
    if (s.itbuf)     kfree(s.itbuf);
    if (s.dirbuf)    kfree(s.dirbuf);
    if (s.ind0)      kfree(s.ind0);
    if (s.ind1)      kfree(s.ind1);
    if (s.ind2)      kfree(s.ind2);
    if (s.bmapbuf)   kfree(s.bmapbuf);
    if (s.gdbuf)     kfree(s.gdbuf);
    return rc;
}

// Print a report to the serial console (and the persistent boot log). Returns
// the total problem count.
uint32_t ext2_fsck_print(const ext2_fsck_report_t *r, int to_bootlog) {
    if (!r) return 0;
    kprintf("[FSCK] inodes-used=%u dirs=%u blocks-used=%u\n",
            (unsigned)r->inodes_used, (unsigned)r->dirs_used, (unsigned)r->blocks_used);
    if (r->total == 0) {
        kprintf("[FSCK] CLEAN: no problems found\n");
        if (to_bootlog) bootlog_write("[FSCK] CLEAN: no problems found");
        return 0;
    }
    kprintf("[FSCK] %u PROBLEM(S) FOUND:\n", (unsigned)r->total);
    if (r->e_bad_block_ptr)          kprintf("[FSCK]   out-of-range block pointers ...... %u\n", (unsigned)r->e_bad_block_ptr);
    if (r->e_dup_block)              kprintf("[FSCK]   multiply-claimed blocks .......... %u\n", (unsigned)r->e_dup_block);
    if (r->e_phantom_inode)          kprintf("[FSCK]   PHANTOM inodes (used, bmap free) . %u\n", (unsigned)r->e_phantom_inode);
    if (r->e_leaked_inode)           kprintf("[FSCK]   leaked inodes (bmap used, free) .. %u\n", (unsigned)r->e_leaked_inode);
    if (r->e_block_used_bitmap_free) kprintf("[FSCK]   blocks in use but FREE in bitmap . %u\n", (unsigned)r->e_block_used_bitmap_free);
    if (r->e_block_free_bitmap_used) kprintf("[FSCK]   blocks marked used, unreferenced . %u\n", (unsigned)r->e_block_free_bitmap_used);
    if (r->e_bad_dirent)             kprintf("[FSCK]   corrupt directory entries ........ %u\n", (unsigned)r->e_bad_dirent);
    if (r->e_orphan_inode)           kprintf("[FSCK]   orphan inodes (lost+found) ....... %u\n", (unsigned)r->e_orphan_inode);
    if (r->e_link_mismatch)          kprintf("[FSCK]   wrong link counts ................ %u\n", (unsigned)r->e_link_mismatch);
    if (r->e_group_free_bad)         kprintf("[FSCK]   wrong group summary counts ....... %u\n", (unsigned)r->e_group_free_bad);
    if (r->e_sb_free_bad)            kprintf("[FSCK]   wrong superblock free counts ..... %u\n", (unsigned)r->e_sb_free_bad);
    if (r->e_bad_inode)              kprintf("[FSCK]   implausible inodes ............... %u\n", (unsigned)r->e_bad_inode);
    if (r->e_io)                     kprintf("[FSCK]   block read failures .............. %u\n", (unsigned)r->e_io);
    if (r->first_msg[0])             kprintf("[FSCK]   first: %s\n", (const char *)r->first_msg);
    if (to_bootlog) {
        bootlog_write("[FSCK] %u problem(s): %s", (unsigned)r->total,
                      r->first_msg[0] ? (const char *)r->first_msg : "(see serial)");
    }
    return r->total;
}

// Boot-time gate. Runs a check ONLY when the superblock says the volume needs
// one, prints to serial + the boot splash, and NEVER writes anything except,
// when the check found ZERO problems, clearing the error/dirty state it just
// verified. Report-only means it cannot make the machine unbootable: the worst
// case is a slower boot, and even that only after an unclean stop.
void ext2_fsck_boot_check(int skip_requested, int bench) {
    extern void gfx_boot_log(const char *);
    if (!g_ext2.mounted) return;

    uint32_t why = ext2_fsck_needed();
    if (why == 0) {
        kprintf("[FSCK] volume was cleanly unmounted (s_state=0x%x, mount %u); skipping check\n",
                (unsigned)g_ext2.sb_state_at_mount, (unsigned)g_ext2.sb_mnt_count);
        return;
    }
    // Escape hatch (checked by the caller, which owns the FAT ESP): an empty
    // /NOFSCK marker skips the check for good. A boot-time check must never be
    // something a user cannot get past.
    if (skip_requested) {
        kprintf("[FSCK] /NOFSCK present: skipping the boot check (why=0x%x)\n", (unsigned)why);
        gfx_boot_log("[BOOT] Filesystem check skipped (/NOFSCK marker present)");
        return;
    }

    char line[128];
    // WHY. A boot that pauses without saying why is how a user learns to fear
    // their own machine.
    // #229 WORDING. Until #229 this first line fired on EVERY boot, because
    // the restart path never marked the volume clean, so the one message that
    // should mean "something went wrong last time" meant nothing at all. Now
    // that a graceful stop really is recorded as clean, this line only appears
    // when the previous session did NOT stop gracefully, and it says so in
    // those terms rather than in filesystem jargon.
    const char *reason = (why & 1u) ? "the last session ended without unmounting this filesystem "
                                      "(power loss, reset, or a crash)"
                       : (why & 2u) ? "an error was recorded on this filesystem"
                                    : "maximum mount count reached";
    uint32_t mb = (uint32_t)(((uint64_t)g_ext2.blocks_count * g_ext2.block_size) >> 20);
    snprintf(line, sizeof(line), "Filesystem check: %s  (ext2, %uK blocks, %u MB)",
             g_ext2.sb_label, (unsigned)(g_ext2.block_size / 1024), (unsigned)mb);
    gfx_boot_log(line);
    kprintf("[FSCK] %s\n", line);
    snprintf(line, sizeof(line), "Reason: %s (mount %u, s_state 0x%x)",
             reason, (unsigned)g_ext2.sb_mnt_count, (unsigned)g_ext2.sb_state_at_mount);
    gfx_boot_log(line);
    kprintf("[FSCK] %s\n", line);
    gfx_boot_log("READ-ONLY check. Nothing will be repaired.  Press ESC to skip.");
    kprintf("[FSCK] READ-ONLY check; press ESC to skip\n");
    bootlog_write("[FSCK] boot check starting (why=0x%x: %s)", (unsigned)why, reason);

    ext2_fsck_report_t rep;
    int rc = ext2_fsck_run_ex(&rep, 1);
    uint32_t ms = ext2_fsck_last_ms();

    if (rc == -9 || (rc != 0 && !rep.completed)) {
        // Aborted (ESC) or could not run. Either way the volume is NOT
        // verified, so nothing is cleared and nothing claims it is clean.
        snprintf(line, sizeof(line), "Filesystem check %s after %u.%us: volume NOT verified",
                 (rc == -9) ? "SKIPPED" : "could not run", (unsigned)(ms / 1000), (unsigned)((ms / 100) % 10));
        gfx_boot_log(line);
        kprintf("[FSCK] %s (rc=%d)\n", line, rc);
        bootlog_write("[FSCK] %s (rc=%d)", line, rc);
        return;
    }

    // e2fsck-shaped summary: label, files used/total, blocks used/total, time.
    snprintf(line, sizeof(line), "%s: %u/%u files, %u/%u blocks, %u directories",
             g_ext2.sb_label,
             (unsigned)rep.inodes_used, (unsigned)g_ext2.inodes_count,
             (unsigned)rep.blocks_used, (unsigned)g_ext2.blocks_count,
             (unsigned)rep.dirs_used);
    gfx_boot_log(line);
    kprintf("[FSCK] %s\n", line);

    // Snapshot the display counters BEFORE the bench re-run: the second run
    // resets them, and the summary must report what the DISPLAYED check cost.
    uint32_t ui_paint_ms = ext2_fsck_last_paint_ms();
    uint32_t ui_paints   = ext2_fsck_last_paints();
    if (bench) {
        // Same volume, same boot, caches in the same state: the only variable
        // is whether the display was painted.
        uint32_t paint_ms = ui_paint_ms;
        uint32_t paints   = ui_paints;
        ext2_fsck_report_t rep2;
        int rc2 = ext2_fsck_run_ex(&rep2, 0);
        uint32_t ms2 = ext2_fsck_last_ms();
        kprintf("[FSCK-BENCH] display ON  %ums (paint %ums over %u repaints)\n",
                (unsigned)ms, (unsigned)paint_ms, (unsigned)paints);
        kprintf("[FSCK-BENCH] display OFF %ums (rc=%d, total=%u)\n",
                (unsigned)ms2, rc2, (unsigned)rep2.total);
        bootlog_write("[FSCK-BENCH] on=%ums off=%ums paint=%ums repaints=%u",
                      (unsigned)ms, (unsigned)ms2, (unsigned)paint_ms, (unsigned)paints);
        snprintf(line, sizeof(line), "Bench: display ON %ums, OFF %ums (paint %ums/%u repaints)",
                 (unsigned)ms, (unsigned)ms2, (unsigned)paint_ms, (unsigned)paints);
        gfx_boot_log(line);
    }

    if (rep.total == 0) {
        snprintf(line, sizeof(line),
                 "Result: CLEAN - no problems found (%u.%us; display %ums over %u repaints)",
                 (unsigned)(ms / 1000), (unsigned)((ms / 100) % 10),
                 (unsigned)ui_paint_ms, (unsigned)ui_paints);
        gfx_boot_log(line);
        kprintf("[FSCK] %s\n", line);
        bootlog_write("[FSCK] CLEAN: %u/%u files, %u/%u blocks, %ums",
                      (unsigned)rep.inodes_used, (unsigned)g_ext2.inodes_count,
                      (unsigned)rep.blocks_used, (unsigned)g_ext2.blocks_count, (unsigned)ms);
        // The ONE case where the flags may be cleared: we verified the whole
        // volume ourselves, every pass, this boot.
        (void)ext2_sb_set_state(&g_ext2, E2ST_VERIFIED_CLEAN, 0);
        g_ext2.sb_state_at_mount = (uint16_t)EXT2_VALID_FS;
        // Re-mark dirty: the volume is mounted and about to be written again.
        (void)ext2_sb_set_state(&g_ext2, E2ST_DIRTY, 0);
        return;
    }

    snprintf(line, sizeof(line),
             "Result: %u PROBLEM(S) FOUND in %u.%us (display %ums) - NOTHING WAS REPAIRED",
             (unsigned)rep.total, (unsigned)(ms / 1000), (unsigned)((ms / 100) % 10),
             (unsigned)ui_paint_ms);
    gfx_boot_log(line);
    kprintf("[FSCK] %s\n", line);

    // A numbered list of the specific problems, on screen and on serial.
    static const char *names[13] = {
        "out-of-range block pointers",
        "multiply-claimed blocks",
        "phantom inodes (in use, free in bitmap)",
        "leaked inodes (bitmap used, inode free)",
        "blocks in use but free in the bitmap",
        "blocks marked used but unreferenced",
        "corrupt directory entries",
        "orphan inodes (lost+found candidates)",
        "wrong link counts",
        "wrong group summary counts",
        "wrong superblock free counts",
        "implausible inodes",
        "block read failures",
    };
    uint32_t vals[13];
    vals[0]  = rep.e_bad_block_ptr;          vals[1]  = rep.e_dup_block;
    vals[2]  = rep.e_phantom_inode;          vals[3]  = rep.e_leaked_inode;
    vals[4]  = rep.e_block_used_bitmap_free; vals[5]  = rep.e_block_free_bitmap_used;
    vals[6]  = rep.e_bad_dirent;             vals[7]  = rep.e_orphan_inode;
    vals[8]  = rep.e_link_mismatch;          vals[9]  = rep.e_group_free_bad;
    vals[10] = rep.e_sb_free_bad;            vals[11] = rep.e_bad_inode;
    vals[12] = rep.e_io;
    int n = 0;
    for (int k = 0; k < 13; k++) {
        if (!vals[k]) continue;
        n++;
        snprintf(line, sizeof(line), "  %d) %s: %u", n, names[k], (unsigned)vals[k]);
        gfx_boot_log(line);
        kprintf("[FSCK] %s\n", line);
    }
    if (rep.first_msg[0]) {
        snprintf(line, sizeof(line), "  first: %s", (const char *)rep.first_msg);
        gfx_boot_log(line);
        kprintf("[FSCK] %s\n", line);
    }
    gfx_boot_log("This check is REPORT-ONLY. Run `fsck` in Terminal for detail.");
    bootlog_write("[FSCK] %u problem(s): %s", (unsigned)rep.total,
                  rep.first_msg[0] ? (const char *)rep.first_msg : "(see serial)");
}


// Boot self-test: proves the Rust FFI structs this build compiled against are
// the ones rustkern actually produced. A mismatch here is a wild pointer
// waiting to happen, so it is checked on every boot, not assumed.
void ext2_fsck_selftest(void) {
    uint32_t rg = ext2_fsck_sizeof_rs(0);
    uint32_t rs = ext2_fsck_sizeof_rs(1);
    uint32_t rr = ext2_fsck_sizeof_rs(2);
    int ok = (rg == (uint32_t)sizeof(ext2_fsck_geom_t)) &&
             (rs == (uint32_t)sizeof(ext2_fsck_scratch_t)) &&
             (rr == (uint32_t)sizeof(ext2_fsck_report_t));
    kprintf("[RUST-FFI] ext2fsck: geom C=%u RS=%u scratch C=%u RS=%u report C=%u RS=%u -> %s\n",
            (unsigned)sizeof(ext2_fsck_geom_t), (unsigned)rg,
            (unsigned)sizeof(ext2_fsck_scratch_t), (unsigned)rs,
            (unsigned)sizeof(ext2_fsck_report_t), (unsigned)rr,
            ok ? "MATCH" : "DRIFT");
    bootlog_write("[RUST-FFI] ext2fsck sizeof lock: %s", ok ? "MATCH" : "DRIFT");
    if (!ok) {
        kprintf("[RUST-FFI] ext2fsck FFI layout DRIFT: the checker is DISABLED this boot\n");
        g_e2fsck_ffi_ok = 0;
    } else {
        g_e2fsck_ffi_ok = 1;
    }
}
