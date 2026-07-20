// waitq_test.c - boot self-test for the timed wait-queue primitives (#426).
//
// Proves all three outcomes of wait_event_timeout() / the interruptible form
// on THIS build, at boot, once:
//
//   1. cond-wakes-first  - a waker sets the condition and calls wake_up()
//                          before the deadline  -> WAIT_OK, returning EARLY.
//   2. timeout-fires     - nothing ever wakes the queue                -> WAIT_TIMEOUT.
//   3. signal-interrupts - wake_up_process() (the path signal delivery
//                          uses) kicks the sleeper                     -> WAIT_EINTR.
//
// Outcome 2 is the one that did not exist before this test: WAIT_TIMEOUT was a
// #define nothing ever set. It is also the one that silently regresses to "hang
// forever" if the wake_time arming or the tick sweep ever breaks, which is
// exactly the failure a serial integrator needs to see at boot rather than in a
// user report.
//
// #426-safe: every phase BLOCKS on the primitive under test or on proc_sleep()
// (a real timer-driven sleep). There is no poll loop and no busy-wait anywhere
// in this file, and the whole test is bounded at roughly 4.5 seconds.

#include "waitq.h"
#include "../proc/process.h"
#include "../serial.h"

static wait_queue_head_t st_wq;
static volatile int      st_cond;
static process_t        *st_waiter;

// The wakers fire well after the waiter has parked. Parking takes microseconds,
// so 100ms is a ~1000x margin. If a waker ever DID fire first, phase 1 would
// still pass (cond is checked before sleeping) and phase 3 would report a loud
// FAIL with its elapsed time rather than silently passing.
#define ST_WAKER_DELAY_MS    100
#define ST_LONG_TIMEOUT_MS   3000   // must NOT be reached in phases 1 and 3
#define ST_SHORT_TIMEOUT_MS  100    // phase 2's deadline

static void st_waker_cond(void *arg) {
    (void)arg;
    proc_sleep(ST_WAKER_DELAY_MS);   // one-shot delay, NOT a poll loop
    st_cond = 1;
    wake_up(&st_wq);
}

static void st_waker_signal(void *arg) {
    (void)arg;
    proc_sleep(ST_WAKER_DELAY_MS);
    // Deliberately does NOT set st_cond: this must interrupt the wait with the
    // condition still false, which is what makes WAIT_EINTR distinguishable
    // from WAIT_OK.
    if (st_waiter) wake_up_process(st_waiter);
}

static uint64_t st_ms_since(uint64_t t0_ticks) {
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    return ((timer_ticks - t0_ticks) * 1000) / hz;
}

static void waitq_selftest_worker(void *arg) {
    (void)arg;

    // Let boot settle before spawning helpers and measuring timings. This is a
    // diagnostic; it must never perturb bring-up.
    proc_sleep(4000);

    int fails = 0;
    uint64_t t0, el;
    int rc;

    wait_queue_head_init(&st_wq);

    // ---- Phase 1: the condition wakes us BEFORE the deadline -> WAIT_OK ----
    st_cond = 0;
    t0 = timer_ticks;
    proc_create("wq_test_wake", st_waker_cond, NULL, PRIO_NORMAL);
    rc = wait_event_timeout(&st_wq, st_cond, wq_ms_to_ticks(ST_LONG_TIMEOUT_MS));
    el = st_ms_since(t0);
    if (rc == WAIT_OK && st_cond == 1 && el < ST_LONG_TIMEOUT_MS) {
        kprintf("[WAITQ] cond-wakes-first: WAIT_OK after %lums (deadline was %ums) -> PASS\n",
                el, ST_LONG_TIMEOUT_MS);
    } else {
        kprintf("[WAITQ] cond-wakes-first: rc=%d cond=%d el=%lums -> FAIL "
                "(expected WAIT_OK(%d) well before %ums)\n",
                rc, st_cond, el, WAIT_OK, ST_LONG_TIMEOUT_MS);
        fails++;
    }

    // ---- Phase 2: nothing ever wakes the queue -> WAIT_TIMEOUT ----
    // No waker is spawned. The ONLY thing that can end this wait is the
    // wake_time deadline being swept by the timer tick.
    st_cond = 0;
    t0 = timer_ticks;
    rc = wait_event_timeout(&st_wq, st_cond, wq_ms_to_ticks(ST_SHORT_TIMEOUT_MS));
    el = st_ms_since(t0);
    if (rc == WAIT_TIMEOUT && el >= (ST_SHORT_TIMEOUT_MS / 2)) {
        kprintf("[WAITQ] timeout-fires: WAIT_TIMEOUT after %lums (asked %ums) -> PASS\n",
                el, ST_SHORT_TIMEOUT_MS);
    } else {
        kprintf("[WAITQ] timeout-fires: rc=%d el=%lums -> FAIL "
                "(expected WAIT_TIMEOUT(%d) at about %ums)\n",
                rc, el, WAIT_TIMEOUT, ST_SHORT_TIMEOUT_MS);
        fails++;
    }

    // ---- Phase 3: a signal interrupts the wait -> WAIT_EINTR ----
    st_cond = 0;
    st_waiter = proc_current();
    t0 = timer_ticks;
    proc_create("wq_test_sig", st_waker_signal, NULL, PRIO_NORMAL);
    rc = wait_event_interruptible_timeout(&st_wq, st_cond,
                                          wq_ms_to_ticks(ST_LONG_TIMEOUT_MS));
    el = st_ms_since(t0);
    st_waiter = NULL;
    if (rc == WAIT_EINTR && el < ST_LONG_TIMEOUT_MS) {
        kprintf("[WAITQ] signal-interrupts: WAIT_EINTR after %lums (deadline was %ums) -> PASS\n",
                el, ST_LONG_TIMEOUT_MS);
    } else {
        kprintf("[WAITQ] signal-interrupts: rc=%d el=%lums -> FAIL "
                "(expected WAIT_EINTR(%d) well before %ums)\n",
                rc, el, WAIT_EINTR, ST_LONG_TIMEOUT_MS);
        fails++;
    }

    if (fails == 0) {
        kprintf("[WAITQ] self-test: 3/3 PASS (cond-wake, timeout, signal)\n");
    } else {
        kprintf("[WAITQ] self-test: %d/3 FAILED\n", fails);
    }
}

// Spawn the self-test. Call once from main.c, from the late worker cluster
// (after preemption is enabled), never from early late_init: a worker spawned
// too early is created but never scheduled.
void waitq_start_selftest(void) {
    proc_create("waitq_test", waitq_selftest_worker, NULL, PRIO_NORMAL);
}
