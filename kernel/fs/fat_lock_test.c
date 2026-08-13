// fat_lock_test.c - #746 fatlock: does the FAT lock still EXCLUDE?
//
// WHY THIS EXISTS
// ---------------
// The #746 change converts fat_lock() from a yield-spin into a wait-queue
// mutex. Every liveness test in the world is passed by a lock that never
// blocks ANYONE, so "the box boots now" is not evidence that the lock still
// does its job. This is the safety half of the proof, and it is built to go
// RED on demand so that a green result means something:
//
//   make fatlock-selftest       real lock   -> expect PASS
//   make fatlock-selftest-red   broken lock -> expect FAIL
//
// The RED build defines FAT_LOCK_SELFTEST_RED, which makes fat_lock_try() in
// fs/fat.c claim the lock unconditionally instead of compare-and-swapping for
// it. That is a lock that excludes nobody. If this test cannot tell that build
// apart from the real one, the test is worthless and must not be trusted.
//
// WHAT IS MEASURED
// ----------------
//  1. OCCUPANCY. fs/fat.c increments g_fat_lock_occupancy on every depth-1
//     acquire and decrements it on the matching release. Anything but 1 inside
//     the critical section means two contexts are in there at once, which is
//     the exact condition the lock exists to prevent. Counted in
//     g_fat_lock_excl_violations.
//  2. DATA INTEGRITY. Each worker owns one file under /boot (which
//     fat_path_on_ext2() deliberately keeps on the FAT ESP, so this exercises
//     the real FAT path on a two-partition golden as well as on a
//     single-partition image). It writes a round-specific byte pattern, reads
//     it back and compares every byte. The workers share a directory, so they
//     contend on the same directory sectors and the same FAT allocation table
//     through the same global sector_buf: precisely the read-modify-write span
//     the lock was introduced to serialise.
//  3. The no-block counters (g_fat_lock_noblock_*, g_fat_lock_wait_timeouts),
//     which must all be zero on a healthy run.
//
// The launcher parks on a wait queue for the workers to finish. It does not
// poll, because CLAUDE.md forbids hand-rolled polling and a test that breaks
// the rule it is testing would be absurd.

#ifdef FAT_LOCK_SELFTEST

#include "fat.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../proc/process.h"
#include "../sync/waitq.h"

#define FLK_THREADS 4
#define FLK_ROUNDS  40
#define FLK_BYTES   4096

extern fat_fs_t g_fat_fs;

extern volatile int      g_fat_lock_occupancy;
extern volatile uint64_t g_fat_lock_excl_violations;
extern volatile uint64_t g_fat_lock_noblock_spins;
extern volatile uint64_t g_fat_lock_noblock_fails;
extern volatile uint64_t g_fat_lock_wait_timeouts;

static wait_queue_head_t flk_done_wq = { .head = NULL, .lock = SPINLOCK_INIT };
static volatile int      flk_done       = 0;
static volatile uint64_t flk_mismatches = 0;
static volatile uint64_t flk_io_errors  = 0;
static volatile uint64_t flk_rounds_ok  = 0;

static void flk_worker(void *arg) {
    int id = (int)(long)arg;
    char path[24];
    // "/boot/FLK<id>.TST" - lowercase /boot is the FAT-ESP-only prefix.
    strcpy(path, "/boot/FLK0.TST");
    path[9] = (char)('0' + id);

    uint8_t *out = (uint8_t *)kmalloc(FLK_BYTES);
    if (!out) { __sync_fetch_and_add(&flk_io_errors, 1); goto done; }

    for (int r = 0; r < FLK_ROUNDS; r++) {
        uint8_t pat = (uint8_t)(id * 37 + r * 11 + 1);
        for (int i = 0; i < FLK_BYTES; i++) out[i] = pat;

        if (fat_write_file(&g_fat_fs, path, out, FLK_BYTES) != 0) {
            __sync_fetch_and_add(&flk_io_errors, 1);
            continue;
        }

        uint32_t got = 0;
        uint8_t *back = (uint8_t *)fat_read_file(&g_fat_fs, path, &got);
        if (!back) { __sync_fetch_and_add(&flk_io_errors, 1); continue; }

        if (got != FLK_BYTES) {
            __sync_fetch_and_add(&flk_mismatches, 1);
        } else {
            for (uint32_t i = 0; i < got; i++) {
                if (back[i] != pat) { __sync_fetch_and_add(&flk_mismatches, 1); break; }
            }
        }
        kfree(back);

        // Directory-sector and free-cluster contention, on the shared parent.
        (void)fat_exists(&g_fat_fs, "/boot/kernel.elf");
        (void)fat_get_free_clusters(&g_fat_fs);

        __sync_fetch_and_add(&flk_rounds_ok, 1);
    }
    kfree(out);

done:
    (void)fat_delete(&g_fat_fs, path);
    __sync_fetch_and_add(&flk_done, 1);
    wake_up_all(&flk_done_wq);
    proc_exit(0);
}

static void flk_launcher(void *arg) {
    (void)arg;
    kprintf("[FATLOCKTEST] #746: %d workers x %d rounds x %d bytes on the FAT ESP\n",
            FLK_THREADS, FLK_ROUNDS, FLK_BYTES);
#ifdef FAT_LOCK_SELFTEST_RED
    kprintf("[FATLOCKTEST] *** RED BUILD: fat_lock_try() claims unconditionally. "
            "This build MUST FAIL. ***\n");
#endif

    for (int i = 0; i < FLK_THREADS; i++) {
        char nm[12];
        strcpy(nm, "flkw0");
        nm[4] = (char)('0' + i);
        if (proc_create(nm, flk_worker, (void *)(long)i, PRIO_NORMAL) < 0) {
            kprintf("[FATLOCKTEST] proc_create(%s) FAILED\n", nm);
            __sync_fetch_and_add(&flk_done, 1);
        }
    }

    // Canonical primitive, not a poll loop.
    wait_event(&flk_done_wq, flk_done >= FLK_THREADS);

    uint64_t excl = g_fat_lock_excl_violations;
    uint64_t mism = flk_mismatches;
    uint64_t ioe  = flk_io_errors;
    int pass = (excl == 0) && (mism == 0) && (ioe == 0) &&
               (flk_rounds_ok == (uint64_t)FLK_THREADS * FLK_ROUNDS);

    kprintf("[FATLOCKTEST] rounds_ok=%lu/%d occupancy_violations=%lu "
            "data_mismatches=%lu io_errors=%lu\n",
            (unsigned long)flk_rounds_ok, FLK_THREADS * FLK_ROUNDS,
            (unsigned long)excl, (unsigned long)mism, (unsigned long)ioe);
    kprintf("[FATLOCKTEST] noblock_spins=%lu noblock_fails=%lu wait_timeouts=%lu "
            "residual_occupancy=%d\n",
            (unsigned long)g_fat_lock_noblock_spins,
            (unsigned long)g_fat_lock_noblock_fails,
            (unsigned long)g_fat_lock_wait_timeouts,
            g_fat_lock_occupancy);
    kprintf("[FATLOCKTEST] RESULT: %s\n", pass ? "PASS" : "FAIL");
    proc_exit(0);
}

// Called from main.c once the scheduler and the filesystem are both live.
void fat_lock_selftest_start(void) {
    proc_create("flktest", flk_launcher, NULL, PRIO_NORMAL);
}

#endif // FAT_LOCK_SELFTEST
