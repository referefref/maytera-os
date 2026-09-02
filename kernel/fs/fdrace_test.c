// fdrace_test.c - #SMPGLOBALS: the negative control for the per-process fd lock.
//
// `make FDRACETEST=1` ONLY. Never in a golden.
//
// WHY THIS FILE EXISTS
// --------------------
// A lock that has only ever been observed not-failing is indistinguishable
// from no lock at all. This project has shipped a GUARD130_DISABLE that built
// a normal kernel, an increment_build.sh that was `exit 0`, and a concurrency
// lint that could not run; the shape they share is an instrument nobody ever
// watched go RED. So the fd-table lock added on 2026-08-30 gets a harness that
// can turn it OFF at run time (drop an empty /FDLOCKOFF.TXT on the ESP) and
// re-run the identical code, and the same numbers must go red.
//
// It drives the REAL shipping bodies. fs/vfs.c's fd_alloc_install() and
// fd_close() are one-line wrappers over fd_alloc_install_on()/fd_close_on(),
// which is what this file calls. There is no second copy of the logic to
// diverge from the one that ships, which is exactly how a differential can be
// green while both arms are wrong.
//
// TWO CHECKS, MEASURING DIFFERENT THINGS
// --------------------------------------
// A. TOCTOU, deterministic, no second core needed. fd_alloc() reports a free
//    slot without reserving it. Two callers that each fd_alloc() then
//    fd_install() therefore get the SAME fd, and the second install evicts and
//    file_put()s the first caller's description while the first caller still
//    believes it owns that fd. This is shown on the real functions by
//    interleaving them by hand: it is not a timing accident, it is what the
//    two-call API means. fd_alloc_install() claims under one lock and cannot
//    do it. Check A is therefore GREEN even in the lock-off arm for the atomic
//    call, and RED in BOTH arms for the split call, which is the point: the
//    split call is the API defect and the lock does not rescue it.
//
// B. CONCURRENT CLAIM. Two kernel threads hammer install/close on ONE victim
//    fd table. Ownership of each slot is tracked with a compare-and-swap, so a
//    slot handed to two claimants at once is caught rather than inferred.
//    With the lock on this must read 0. With /FDLOCKOFF.TXT it must not.
//
// No em-dashes per repo style.

#ifdef FDRACE_TEST

#include "vfs.h"
#include "../proc/process.h"
#include "../sync/spinlock.h"
#include "../sync/waitq.h"
#include "../cpu/mono.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/bootlog.h"

#define FDRACE_ITERS   400
#define FDRACE_SLOTS   MAX_FDS

// Armed by the harness only. In a normal build this whole file is absent, so
// the window call in fs/vfs.c is compiled out with it.
static volatile int      g_window_us = 0;

// Called from inside fd_alloc_install_on(), between finding a free slot and
// storing into it. With the lock ON this runs inside the critical section with
// interrupts off, so it is a bounded stall and nothing more. With the lock OFF
// it is the window the second claimant walks into. Same idea and same bounded
// mono_us() form as proc/schedrace.c's schedrace_delay(); it is separate only
// because that one deliberately does nothing unless g_smp_user_sched is set,
// and check B must be able to fail on a single core too.
void fdrace_window(void) {
    extern int g_fdlock_off;
    extern void proc_yield(void);
    if (g_window_us <= 0) return;

    // WHY THE TWO ARMS DO DIFFERENT THINGS HERE, and why that is not cheating.
    //
    // A busy delay does not work. First try: 3 us of `pause`, RED arm reported
    // ZERO corruption over 8000 claims. Second try: 5 us -> 5000 us, still
    // zero over 800. The reason is not the width of the window, it is that
    // these are KERNEL threads and nothing preempted them inside it, so the
    // two hammers never overlapped in EITHER arm. A harness whose red arm is
    // as quiet as its green arm has measured nothing.
    //
    // So the RED arm yields, which makes the interleave certain instead of
    // probable. The GREEN arm cannot: the lock is held irqsave, IF=0, and
    // blocking there would be exactly the violation sync/noblock.c exists to
    // catch. That asymmetry IS the property under test. With the lock held the
    // section is uninterruptible by construction, which is what the lock buys;
    // with it removed the section is a place another task can be scheduled
    // into, and this shows what happens when one is.
    if (g_fdlock_off) {
        proc_yield();
        proc_yield();
        return;
    }
    uint64_t t0 = mono_us();
    while (mono_us() - t0 < (uint64_t)g_window_us) { __asm__ volatile("pause"); }
}

// The victim table. A bare process_t in .bss, never entered into the process
// table and never scheduled: the harness needs its fds[] and its fd_lock, not
// a task. That keeps the test off every real process's descriptors.
static process_t   g_victim;
static volatile uint32_t g_owner[FDRACE_SLOTS];   // 0 = free, else claimant tag

// Guards g_owner[] and nothing else. THIS LOCK IS NOT UNDER TEST: it protects
// the DETECTOR's bookkeeping, not the fd table, and it is held for two array
// accesses well away from the window in fd_alloc_install_on(). It is the
// shared sync/spinlock.h irqsave lock; the first draft used a bare
// __sync_val_compare_and_swap in the hammer loop and the concurrency lint
// correctly called it a hand-rolled acquire that never masks IF.
static spinlock_t        g_owner_lock;
static wait_queue_head_t g_done_wq;
static volatile int      g_threads_done = 0;
static volatile uint64_t g_double_claim = 0;   // slot handed to two claimants
static volatile uint64_t g_wrong_readback = 0; // slot did not hold what we put
static volatile uint64_t g_claims = 0;
static volatile uint64_t g_exhausted = 0;      // table full: expected sometimes

static void hammer(void *arg) {
    uint32_t tag = (uint32_t)(uint64_t)arg;
    for (int i = 0; i < FDRACE_ITERS; i++) {
        file_t *f = file_alloc(NULL, NULL, O_RDONLY);   // NULL ops: put just kfrees
        if (!f) break;
        int fd = fd_alloc_install_on(&g_victim, f);
        if (fd < 0) { g_exhausted++;
            IGNORE_RESULT("harness teardown of a description that never reached a slot; there is no caller to report a flush failure to", file_put(f));
            continue; }
        g_claims++;

        // Did anyone else already own this slot? Claim it under the detector's
        // own lock, so the detector cannot itself race and under-report.
        { uint64_t __fl = spinlock_acquire_irqsave(&g_owner_lock);
          if (g_owner[fd] != 0) g_double_claim++;
          g_owner[fd] = tag;
          // And is the slot actually holding OUR description?
          if (g_victim.fds[fd] != f) g_wrong_readback++;
          spinlock_release_irqrestore(&g_owner_lock, __fl); }

        { uint64_t __fl = spinlock_acquire_irqsave(&g_owner_lock);
          if (g_owner[fd] == tag) g_owner[fd] = 0;
          spinlock_release_irqrestore(&g_owner_lock, __fl); }
        (void)fd_close_on(&g_victim, fd);
    }
    g_threads_done++;
    wake_up(&g_done_wq);
    proc_exit(0);
}

// ---- Check A: the TOCTOU in the split allocate-then-install API ------------
static void check_a(void) {
    // Runs against the CURRENT task's own table, using the shipping calls.
    int a = fd_alloc(3);
    int b = fd_alloc(3);      // nothing was installed in between
    int split_same = (a >= 0 && a == b);

    file_t *f1 = file_alloc(NULL, NULL, O_RDONLY);
    file_t *f2 = file_alloc(NULL, NULL, O_RDONLY);
    int c = -1, d = -1, atomic_same = 0;
    if (f1 && f2) {
        c = fd_alloc_install(f1);
        d = fd_alloc_install(f2);
        atomic_same = (c >= 0 && c == d);
        if (c >= 0) (void)fd_close(c);
        else IGNORE_RESULT("harness teardown; no recipient for a flush status", file_put(f1));
        if (d >= 0) (void)fd_close(d);
        else IGNORE_RESULT("harness teardown; no recipient for a flush status", file_put(f2));
    }

    bootlog_write("[FDRACE] A split fd_alloc(3) twice -> %d and %d : %s\n",
            a, b, split_same ? "SAME FD (TOCTOU, as expected)" : "different");
    bootlog_write("[FDRACE] A atomic fd_alloc_install twice -> %d and %d : %s\n",
            c, d, atomic_same ? "SAME FD (BROKEN)" : "distinct (correct)");
    bootlog_write("[FDRACE] A VERDICT: %s\n",
            (split_same && !atomic_same) ? "PASS (the split API races, the atomic one does not)"
                                         : "INCONCLUSIVE - read the two lines above");
}

void fdrace_selftest(void);

// Entry point for the kernel thread main.c creates after proc_init(). The
// sleep is not a poll: it is "let the machine finish booting", so the
// measurement is of a steady-state system and not of boot.
void fdrace_boot_worker(void *arg) {
    (void)arg;
    extern void proc_sleep(uint32_t ms);
    proc_sleep(20000);
    fdrace_selftest();
    proc_exit(0);
}

void fdrace_selftest(void) {
    extern int g_fdlock_off;
    bootlog_write("[FDRACE] harness armed. lock is %s\n",
            g_fdlock_off ? "OFF (/FDLOCKOFF.TXT present) - RED ARM" : "ON - GREEN ARM");

    check_a();

    memset(&g_victim, 0, sizeof(g_victim));
    spinlock_init(&g_victim.fd_lock);
    spinlock_init(&g_owner_lock);
    for (int i = 0; i < FDRACE_SLOTS; i++) g_owner[i] = 0;
    wait_queue_head_init(&g_done_wq);
    g_threads_done = 0;
    g_double_claim = g_wrong_readback = g_claims = g_exhausted = 0;
    // WIDER THAN ONE TIMER TICK, ON PURPOSE. The first version used 3 us and
    // the RED arm reported ZERO corruption over 8000 claims, i.e. it did not
    // discriminate and therefore proved nothing about the green arm either. A
    // 3 us window against a 4 ms tick is a 0.075% chance of a preemption
    // landing in it. At 5 ms a preemption is effectively certain, so with the
    // lock OFF the second claimant walks in every time, and with the lock ON
    // the acquire is irqsave (IF=0) so nothing can be scheduled into it at all.
    // That is what makes the two arms different rather than both quiet.
    g_window_us = 200;

    int t1 = proc_create_ex("fdrace1", hammer, (void *)1UL, PRIO_NORMAL, 32 * 1024);
    int t2 = proc_create_ex("fdrace2", hammer, (void *)2UL, PRIO_NORMAL, 32 * 1024);
    if (t1 < 0 || t2 < 0) {
        bootlog_write("[FDRACE] B could not create both hammer threads (%d,%d); "
                "check B did not run\n", t1, t2);
        g_window_us = 0;
        return;
    }

    // The wake source is our own two threads, so this is a wait with a
    // timeout only because a thread that dies mid-loop must not wedge boot.
    int rc = wait_event_timeout(&g_done_wq, g_threads_done >= 2, 60000);
    g_window_us = 0;

    // Drain anything the threads left behind.
    for (int i = 0; i < FDRACE_SLOTS; i++) (void)fd_close_on(&g_victim, i);

    bootlog_write("[FDRACE] B wait rc=%d claims=%llu double_claim=%llu "
            "wrong_readback=%llu exhausted=%llu\n", rc,
            (unsigned long long)g_claims, (unsigned long long)g_double_claim,
            (unsigned long long)g_wrong_readback,
            (unsigned long long)g_exhausted);

    if (rc != WAIT_OK) {
        bootlog_write("[FDRACE] B VERDICT: INCONCLUSIVE (threads did not both finish)\n");
    } else if (g_fdlock_off) {
        bootlog_write("[FDRACE] B VERDICT (RED ARM): %s\n",
                (g_double_claim || g_wrong_readback)
                  ? "RED as required - the unlocked table hands one slot to two claimants"
                  : "NO CORRUPTION SEEN. The harness proved nothing; do not trust the "
                    "green arm on the strength of it");
    } else {
        bootlog_write("[FDRACE] B VERDICT (GREEN ARM): %s\n",
                (g_double_claim || g_wrong_readback) ? "FAIL - the lock did not hold"
                                                     : "PASS - zero double claims");
    }
}

#endif // FDRACE_TEST
