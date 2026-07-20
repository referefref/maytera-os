// waitq.h - Wait queue primitives for MayteraOS
// Part of Phase 0 of the POSIX TTY/signals plan.
//
// A wait queue is a list of processes blocked waiting for some condition
// to become true. Use in all blocking primitives (tty read, pipe read,
// pty master read, waitpid, poll) so that signal delivery can interrupt
// the sleep and return -EINTR.
//
// Usage:
//   wait_queue_head_t wq;
//   wait_queue_head_init(&wq);
//
//   // Producer:
//   buffer_has_data = 1;
//   wake_up(&wq);
//
//   // Consumer:
//   int rc = wait_event_interruptible(&wq, buffer_has_data);
//   if (rc == -EINTR) { ...unwind with EINTR... }
//
// The condition must be a C expression evaluated between sleeps. Holding
// a lock across wait_event_interruptible is NOT safe; drop it before
// sleeping, reacquire after wake.

#ifndef SYNC_WAITQ_H
#define SYNC_WAITQ_H

#include "../types.h"
#include "spinlock.h"

// Forward decl
struct process;

// Reason a wait completed (returned from __wait_event_wait()).
#define WAIT_OK         0       // condition became true
#define WAIT_EINTR      (-4)    // interrupted by signal (POSIX EINTR)
#define WAIT_TIMEOUT    (-110)  // timeout expired (POSIX ETIMEDOUT)

// Absolute-deadline sentinel meaning "no deadline, wait indefinitely".
#define WAIT_DEADLINE_NEVER  ((uint64_t)-1)

// Provided by cpu/isr.c (tick counter) and cpu/pic.c (actual tick rate).
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;

// A single entry on a wait queue (allocated on the waiter's kernel stack).
// Forward decl for back-pointer
struct wait_queue_head;

typedef struct wait_queue_entry {
    struct process *proc;                   // process waiting
    struct wait_queue_entry *next;          // next in list
    struct wait_queue_entry *prev;          // prev in list
    struct wait_queue_head *wq;             // back-pointer to owning queue
    uint32_t flags;                         // WQ_ENTRY_* flags
    int wake_reason;                        // WAIT_OK / WAIT_EINTR / WAIT_TIMEOUT
    volatile int on_queue;                  // 1 if linked, 0 if removed
} wait_queue_entry_t;

#define WQ_ENTRY_EXCLUSIVE  0x1             // wake only one exclusive waiter

// Head of a wait queue (stored inside whatever owns the resource).
typedef struct wait_queue_head {
    wait_queue_entry_t *head;               // circular doubly-linked
    spinlock_t lock;                        // protects head/next/prev
} wait_queue_head_t;

// Initialize a wait queue head.
void wait_queue_head_init(wait_queue_head_t *wq);

// Add ourselves (current process) to wq, mark us TASK_INTERRUPTIBLE, but do
// NOT yet sleep. Paired with __wait_finish().
void __wait_prepare(wait_queue_head_t *wq, wait_queue_entry_t *entry, int exclusive);

// Sleep until wake_up or signal. Returns WAIT_OK / WAIT_EINTR.
int __wait_event_wait(wait_queue_entry_t *entry);

// Sleep until wake_up, signal, or the absolute tick deadline passes.
// Returns WAIT_OK / WAIT_EINTR / WAIT_TIMEOUT. Pass WAIT_DEADLINE_NEVER for
// no deadline (then this behaves exactly like __wait_event_wait()).
// Use the wait_event_*_timeout() macros rather than calling this directly:
// on its own it does NOT re-check the caller's condition.
int __wait_event_wait_deadline(wait_queue_entry_t *entry, uint64_t deadline);

// Relative-tick convenience wrapper around __wait_event_wait_deadline().
// NOTE: "at most `ticks` MORE ticks from now". Do not call this in a retry
// loop: each call re-arms a full fresh timeout, so a stream of spurious wakes
// would extend the total wait without bound. Loops must compute ONE absolute
// deadline up front and use __wait_event_wait_deadline() (as the macros do).
int __wait_event_wait_timeout(wait_queue_entry_t *entry, uint64_t ticks);

// Remove ourselves from wq (idempotent if already removed by wake).
void __wait_finish(wait_queue_head_t *wq, wait_queue_entry_t *entry);

// Wake up (at most) one waiter on wq. Safe from IRQ context.
void wake_up(wait_queue_head_t *wq);

// Wake up all waiters on wq. Safe from IRQ context.
void wake_up_all(wait_queue_head_t *wq);

// Wake a specific process if it's sleeping on some wq (used by signal
// delivery to kick an interruptibly-sleeping target).
void wake_up_process(struct process *p);

// Convenience macro: sleep until cond is true or a signal arrives.
// Evaluates cond zero or more times. Returns WAIT_OK or WAIT_EINTR.
//
// Implementation note: we loop so that spurious wakes (e.g., wake_up() fired
// but cond hasn't become true yet) put us back to sleep.
#define wait_event_interruptible(wq, cond) ({                       \
    int __rc = WAIT_OK;                                             \
    wait_queue_entry_t __wqe;                                       \
    while (!(cond)) {                                               \
        __wait_prepare((wq), &__wqe, 0);                            \
        if ((cond)) { __wait_finish((wq), &__wqe); break; }         \
        __rc = __wait_event_wait(&__wqe);                           \
        __wait_finish((wq), &__wqe);                                \
        if (__rc == WAIT_EINTR) break;                              \
    }                                                               \
    __rc;                                                           \
})

// Non-interruptible variant (ignores signals; use ONLY for short critical
// waits such as inside a disk driver). Most users want the interruptible
// form above.
#define wait_event(wq, cond) ({                                     \
    wait_queue_entry_t __wqe;                                       \
    while (!(cond)) {                                               \
        __wait_prepare((wq), &__wqe, 0);                            \
        if ((cond)) { __wait_finish((wq), &__wqe); break; }         \
        (void)__wait_event_wait(&__wqe);                            \
        __wait_finish((wq), &__wqe);                                \
    }                                                               \
    WAIT_OK;                                                        \
})

// ============================================================================
// Timed waits (#426)
// ============================================================================
//
// wait_event_timeout(wq, cond, ticks) = "sleep until cond is true, but give up
// after `ticks` timer ticks". It is the ONLY sanctioned way to wait with a
// bound; a hand-rolled proc_sleep(1)/proc_yield() poll loop is banned (#426).
//
// RETURN CONVENTION (deliberately the header's existing WAIT_* scheme, NOT
// Linux's "0 on timeout / remaining jiffies otherwise" - here WAIT_OK IS 0, so
// Linux's convention would invert the meaning of 0 for every existing caller):
//
//   WAIT_OK      (0)     - the condition was observed TRUE. Always means this,
//                          and only this. Safe to consume the resource.
//   WAIT_TIMEOUT (-110)  - the deadline passed and the condition was FALSE at
//                          the last check.
//   WAIT_EINTR   (-4)    - a signal arrived and the condition was FALSE at the
//                          last check (interruptible variants only).
//
// The condition ALWAYS wins: if cond is true, you get WAIT_OK even if the
// deadline expired or a signal arrived in the same instant. So a caller can
// rely on `if (rc == WAIT_OK) { consume; }` and treat both negative returns as
// "nothing to consume". Remaining ticks are not returned; a caller that cares
// can read timer_ticks itself.
//
// `cond` is evaluated zero or more times (it is a macro argument): it must be
// cheap and free of side effects. Do NOT hold a lock across these.
//
// Callable only once the scheduler is live (they sleep). Not from an IRQ
// handler, not from the compositor draw thread, and not from pid 0.
//
// TWO CAUTIONS, both learned the hard way:
//
//  1. A TIMEOUT HIDES A BROKEN WAKE SOURCE. If the waker is buggy or never
//     arms, a timeout silently converts a debuggable hang into intermittent
//     slowness that shows up as "the OS feels laggy" months later and is far
//     harder to trace. Prefer making the wake source redundant and always
//     armed (e.g. an ISR wake PLUS a periodic worker wake, so no wake can be
//     lost) over papering the gap with a timeout. Reach for a timeout only
//     when the wake genuinely is not ours to guarantee: a remote peer, an
//     unresponsive server, hardware that may never interrupt.
//
//  2. EVERY TIMEOUT VALUE IS WRONG AT SOME LOAD. Resolution is one timer tick
//     (#227: 4ms at the default 250Hz, 10ms on a 100Hz build), deadlines only
//     ever round UP, and a loaded box can overshoot arbitrarily. Never encode
//     a timeout that is also a correctness assumption.

// True once `deadline` (an ABSOLUTE timer_ticks value) has passed. Compares the
// SIGNED difference, so it cannot be fooled by unsigned underflow when a caller
// hands us a deadline that is already behind us. Never expires for
// WAIT_DEADLINE_NEVER.
//
// Honest scope: this comparison is wrap-safe, but the tick sweep it relies on
// (wake_sleeping_procs) still does a naive `timer_ticks >= wake_time`, so a
// 64-bit tick wrap would strand a timed sleep regardless. That is ~2.3 billion
// years away at 250Hz, so the signed compare here is defensive, not a fix.
static inline int wq_deadline_expired(uint64_t deadline) {
    if (deadline == WAIT_DEADLINE_NEVER) return 0;
    return (int64_t)(timer_ticks - deadline) >= 0;
}

// Turn a relative tick count into an absolute deadline (pinned to "now").
static inline uint64_t wq_deadline_in(uint64_t ticks) {
    if (ticks == WAIT_DEADLINE_NEVER) return WAIT_DEADLINE_NEVER;
    return timer_ticks + ticks;
}

// Milliseconds -> ticks against the ACTUAL tick rate. Rounds UP and clamps to
// at least 1 tick, so a wait never returns EARLY. This is the same conversion
// proc_sleep() uses; use it instead of open-coding the arithmetic again (the
// hardcoded-100Hz copy of this formula is exactly what made every sys_sleep
// sleep ~40% short).
static inline uint64_t wq_ms_to_ticks(uint64_t ms) {
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t t = (ms * (uint64_t)hz + 999) / 1000;
    return t ? t : 1;
}

// Core timed wait against an ABSOLUTE deadline. Ignores signals (see the
// _interruptible_ variant below). Pass WAIT_DEADLINE_NEVER to wait forever.
//
// The deadline is computed ONCE, before the loop: a spurious wake must not
// re-arm a fresh full timeout, or the total wait would be unbounded.
//
// Race note: the loop re-checks `cond` at the top on every pass, so a wake_up()
// landing at the same instant the deadline expires can never be lost - whoever
// wins the race, the next thing we do is look at the condition itself, and the
// condition is the truth. __wait_finish() is idempotent, so a wake that already
// unlinked the entry cannot cause a double-remove.
#define wait_event_deadline(wq, cond, deadline) ({                  \
    int __rc = WAIT_OK;                                             \
    uint64_t __dl = (deadline);                                     \
    wait_queue_entry_t __wqe;                                       \
    for (;;) {                                                      \
        if (cond) { __rc = WAIT_OK; break; }                        \
        if (wq_deadline_expired(__dl)) { __rc = WAIT_TIMEOUT; break; } \
        __wait_prepare((wq), &__wqe, 0);                            \
        if (cond) { __wait_finish((wq), &__wqe); __rc = WAIT_OK; break; } \
        (void)__wait_event_wait_deadline(&__wqe, __dl);             \
        __wait_finish((wq), &__wqe);                                \
    }                                                               \
    __rc;                                                           \
})

// As above, but a signal aborts the wait with WAIT_EINTR (unless the condition
// became true, which always wins).
#define wait_event_interruptible_deadline(wq, cond, deadline) ({    \
    int __rc = WAIT_OK;                                             \
    uint64_t __dl = (deadline);                                     \
    wait_queue_entry_t __wqe;                                       \
    for (;;) {                                                      \
        if (cond) { __rc = WAIT_OK; break; }                        \
        if (wq_deadline_expired(__dl)) { __rc = WAIT_TIMEOUT; break; } \
        __wait_prepare((wq), &__wqe, 0);                            \
        if (cond) { __wait_finish((wq), &__wqe); __rc = WAIT_OK; break; } \
        __rc = __wait_event_wait_deadline(&__wqe, __dl);            \
        __wait_finish((wq), &__wqe);                                \
        if (__rc == WAIT_EINTR) { if (cond) __rc = WAIT_OK; break; } \
    }                                                               \
    __rc;                                                           \
})

// Relative-tick forms. `ticks` is evaluated once. Use wq_ms_to_ticks() to
// convert from milliseconds.
#define wait_event_timeout(wq, cond, ticks) \
    wait_event_deadline((wq), (cond), wq_deadline_in((uint64_t)(ticks)))

#define wait_event_interruptible_timeout(wq, cond, ticks) \
    wait_event_interruptible_deadline((wq), (cond), wq_deadline_in((uint64_t)(ticks)))

#endif // SYNC_WAITQ_H
