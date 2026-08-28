// waitq.c - Wait queue implementation for MayteraOS
// See waitq.h for API overview.

#include "waitq.h"
#include "noblock.h"
#include "../proc/process.h"
#include "../types.h"

// Functions we need from process.c. proc_wake() is new and added in the
// Phase 0 process.c patch; add_to_ready_queue stays private to process.c.
extern process_t *proc_current(void);
extern void proc_wake(process_t *p);
extern void sched_schedule(void);

// Interrupt enable/disable primitives.
static inline void wq_cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void wq_sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline uint64_t wq_save_flags(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return flags;
}
static inline void wq_restore_flags(uint64_t flags) {
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

// ============================================================================
// Basic list ops (internal, lock must be held)
// ============================================================================

static void wq_insert_tail(wait_queue_head_t *wq, wait_queue_entry_t *e) {
    e->wq = wq;
    if (!wq->head) {
        e->next = e;
        e->prev = e;
        wq->head = e;
    } else {
        wait_queue_entry_t *h = wq->head;
        e->prev = h->prev;
        e->next = h;
        h->prev->next = e;
        h->prev = e;
    }
    e->on_queue = 1;
}

static void wq_unlink(wait_queue_head_t *wq, wait_queue_entry_t *e) {
    if (!e->on_queue) return;
    if (e->next == e) {
        // last one
        wq->head = NULL;
    } else {
        e->prev->next = e->next;
        e->next->prev = e->prev;
        if (wq->head == e) wq->head = e->next;
    }
    e->next = NULL;
    e->prev = NULL;
    e->on_queue = 0;
}

// ============================================================================
// Public API
// ============================================================================

void wait_queue_head_init(wait_queue_head_t *wq) {
    wq->head = NULL;
    spinlock_init(&wq->lock);
}

void __wait_prepare(wait_queue_head_t *wq, wait_queue_entry_t *entry, int exclusive) {
    // #426 Phase 2 CHOKEPOINT. Every wait_event*() macro in waitq.h reaches a
    // sleep only through here, so ONE check covers the whole family: the
    // interruptible/non-interruptible forms, the timed forms, and the drivers
    // that open-code prepare/wait/finish. Placed FIRST, before the queue lock
    // is taken, so the RFLAGS.IF we sample is the CALLER'S state and not one
    // we created ourselves. Reports (or, with -DWQ_NOBLOCK_PANIC, panics) if
    // this context must never block; see sync/noblock.h.
    wq_assert_may_block("wait_event", __builtin_return_address(0));

    process_t *me = proc_current();
    entry->proc = me;
    entry->next = NULL;
    entry->prev = NULL;
    entry->wq = wq;
    entry->flags = exclusive ? WQ_ENTRY_EXCLUSIVE : 0;
    entry->wake_reason = WAIT_OK;
    entry->on_queue = 0;

    uint64_t flags = spinlock_acquire_irqsave(&wq->lock);
    wq_insert_tail(wq, entry);
    // Mark process blocked and remember the entry so signal delivery can
    // find and kick us.
    if (me) {
        sched_self_block(me, PROC_STATE_BLOCKED);   // #75: arm+set, one op
        me->wait_entry = entry;
    }
    spinlock_release_irqrestore(&wq->lock, flags);
}

int __wait_event_wait(wait_queue_entry_t *entry) {
    // Sleep until someone removes us from the queue and marks us runnable.
    // The scheduler will skip BLOCKED processes; we loop until we're awake.
    while (1) {
        // Re-check state under no-lock (cheap, may race but harmless; the
        // scheduler re-evaluates on next schedule point).
        if (!entry->on_queue) break;
        process_t *me = entry->proc;
        if (!me || me->state != PROC_STATE_BLOCKED) break;

        // Yield.
        sched_schedule();
    }

    // Clear the per-process wait_entry pointer now that we're awake.
    if (entry->proc) entry->proc->wait_entry = NULL;
    return entry->wake_reason;
}

// Sleep until wake_up()/wake_up_process() unlinks us, or the absolute tick
// deadline passes. Factored out of the open-coded copy that lived in
// sys_win_get_event() (proc/syscall.c) so there is ONE implementation (#426).
//
// WHY THE DEADLINE LIVES ON THE PROCESS, NOT ON THE ENTRY:
// wait_queue_entry_t is allocated on the WAITER'S KERNEL STACK (see waitq.h).
// A per-entry timer that fired from the tick would therefore be touching stack
// memory that a concurrent normal wake may already have released, i.e. a
// use-after-free. So we arm the deadline on the PROCESS (wake_time) instead and
// let the existing tick sweep (wake_sleeping_procs) simply mark the process
// runnable. The tick never touches the entry, so the entry-lifetime race cannot
// exist. The waiter re-checks condition-and-deadline at its loop top and draws
// its own conclusion. This is exactly the shape sys_win_get_event proved.
//
// WHY THE WAKE-VS-DEADLINE RACE IS BENIGN:
// Both wake sources converge on "make the process runnable"; neither destroys
// information the other needed. If a wake_up() and the deadline land together,
// the wake unlinks the entry (on_queue=0, wake_reason=WAIT_OK) under the queue
// lock, and our post-sleep classification below reads on_queue under that SAME
// lock, so we cannot half-observe it. Even if we did misclassify, the caller's
// loop re-checks `cond` FIRST on the next pass, and a true condition always
// wins over a timeout. Nothing is lost and nothing is double-removed
// (__wait_finish is idempotent).
int __wait_event_wait_deadline(wait_queue_entry_t *entry, uint64_t deadline) {
    process_t *me = entry->proc;
    wait_queue_head_t *wq = entry->wq;
    int reason;

    if (!wq) return entry->wake_reason;   // not prepared; nothing to wait on

    // Deciding to sleep must be atomic against a concurrent wake, or we could
    // overwrite a just-set READY state with SLEEPING and lose the wake forever.
    // Two hazards, two guards: another CPU's wake_up() (the queue spinlock) and
    // this CPU's own IRQ handlers (cli). Interrupts stay OFF from here all the
    // way into sched_schedule(), which restores them, closing the window the
    // same way proc_sleep() and sys_win_get_event() do.
    uint64_t flags = wq_save_flags();
    wq_cli();
    spinlock_acquire(&wq->lock);

    // Someone already woke us between __wait_prepare() and now: do not sleep.
    if (!entry->on_queue || !me || me->state != PROC_STATE_BLOCKED) {
        reason = entry->wake_reason;
        spinlock_release(&wq->lock);
        wq_restore_flags(flags);
        return reason;
    }

    // Deadline already in the past: report the timeout without ever sleeping.
    if (wq_deadline_expired(deadline)) {
        entry->wake_reason = WAIT_TIMEOUT;
        spinlock_release(&wq->lock);
        wq_restore_flags(flags);
        return WAIT_TIMEOUT;
    }

    // Park as a TIMED sleep so BOTH wake sources can reach us:
    //   - wake_up()/wake_up_process() -> proc_wake(), which accepts SLEEPING
    //   - the timer tick -> wake_sleeping_procs(), which compares wake_time
    // WAIT_DEADLINE_NEVER is UINT64_MAX, which that sweep can never reach, so
    // the "no deadline" case needs no special-casing here.
    sched_self_block(me, PROC_STATE_SLEEPING);  // #75: arm+set, one op
    me->wake_time = deadline;
    spinlock_release(&wq->lock);   // plain release: leaves interrupts OFF
    sched_schedule();              // sleeps here; the scheduler restores IF

    // Awake. Classify under the queue lock so a concurrent wake_up() cannot
    // race our read of on_queue/wake_reason.
    uint64_t flags2 = spinlock_acquire_irqsave(&wq->lock);
    if (entry->on_queue && wq_deadline_expired(deadline)) {
        // Still linked => no wake_up() and no signal reached us => the only
        // thing that could have made us runnable is the tick sweep firing on
        // wake_time. That is a timeout.
        entry->wake_reason = WAIT_TIMEOUT;
    }
    reason = entry->wake_reason;
    if (entry->proc && entry->proc->wait_entry == entry) {
        entry->proc->wait_entry = NULL;
    }
    spinlock_release_irqrestore(&wq->lock, flags2);
    return reason;
}

int __wait_event_wait_timeout(wait_queue_entry_t *entry, uint64_t ticks) {
    return __wait_event_wait_deadline(entry, wq_deadline_in(ticks));
}

// #610 THE LOST-WAKEUP BUG. Counts how many times __wait_finish() had to
// un-park a waiter that was left marked BLOCKED/SLEEPING while it was in fact
// still running. Non-zero is not a warning, it is the race firing; before the
// fix below each one of these was a thread permanently deleted from the
// scheduler. Reported in the [HB] heartbeat line so a long run can be audited.
volatile uint64_t g_wq_unpark_rescues = 0;
static int g_wq_unpark_logged = 0;
#define WQ_UNPARK_LOG_MAX 16
extern int kprintf(const char *fmt, ...);

void __wait_finish(wait_queue_head_t *wq, wait_queue_entry_t *entry) {
    // ------------------------------------------------------------------
    // #610: UNDO THE PARK BEFORE ANYTHING ELSE (Linux's finish_wait() does
    // __set_current_state(TASK_RUNNING) first, for exactly this reason).
    //
    // __wait_prepare() parks the caller: it links the entry AND sets
    // state = PROC_STATE_BLOCKED. Every wait_event*() macro then re-tests the
    // condition ONCE MORE before sleeping, and if the condition became true in
    // that window it breaks straight to here WITHOUT ever sleeping and WITHOUT
    // any wake having run - so nothing ever cleared the BLOCKED state.
    //
    // Returning to the caller still marked BLOCKED is fatal, two ways:
    //   1. sched_schedule() re-queues `prev` only when prev->state ==
    //      PROC_STATE_RUNNING. A BLOCKED-but-running thread is therefore
    //      DROPPED from the scheduler at its very next preemption, and since
    //      __wait_finish() also unlinked it from the queue and cleared
    //      wait_entry, no wake_up_all(), wake_up_process() or tick sweep can
    //      ever find it again. It is gone, holding whatever lock it took.
    //   2. If anything does call proc_wake() on it in the meantime (it still
    //      LOOKS blocked), it is added to the ready queue WHILE RUNNING, and
    //      on an SMP box a second CPU can then context_switch INTO it and run
    //      it on its already-live kernel stack.
    //
    // Both were observed on build 955 during a 103MB App Store install: (1) as
    // a dead halt with every thread asleep, (2) as
    // "[SCHEDBUG] context_switch: pid=10 'heartbeat' rsp=... OUTSIDE kernel
    // stack" followed by a page fault panic.
    //
    // Doing it under the queue lock makes it atomic against a concurrent
    // wake_up_all(): that either sees us still linked and BLOCKED (unlinks +
    // proc_wake, which is a no-op once we are RUNNING) or sees us gone.
    // Only BLOCKED/SLEEPING is rewritten: a waiter that really did sleep comes
    // back as RUNNING (the scheduler sets that when it picks us) or READY, and
    // those must not be touched.
    // ------------------------------------------------------------------
    //
    // #167 ADDS PROC_STATE_READY TO THE SET. A task EXECUTING here can only
    // legitimately be RUNNING: the scheduler is what sets RUNNING, on the task
    // it just switched to. READY-while-executing is a third way of being parked
    // and it became reachable with #167's funnel fix, which performs a deferred
    // wake's state transition at the moment of the decision. The waiter can then
    // observe its entry unlinked, break out of __wait_event_wait() without ever
    // sleeping, and arrive here marked READY. Leaving it that way is fatal in
    // the SAME two ways the comment above describes, plus a third: it is still
    // sitting in the deferred-enqueue table, so the next sched_drain_deferred()
    // would enqueue a task that is running, and a second core could switch into
    // its live kernel stack. sched_self_running() clears both the state and
    // sched_on_cpu, in that order, which is what closes that door.
    uint64_t flags = spinlock_acquire_irqsave(&wq->lock);
    int rescued = 0;
    process_t *me = entry->proc;
    if (me && (me->state == PROC_STATE_BLOCKED || me->state == PROC_STATE_SLEEPING ||
               me->state == PROC_STATE_READY)) {
        sched_self_running(me);                     // #75: set+disarm, one op
        me->wake_time = 0;
        g_wq_unpark_rescues++;
        rescued = 1;
    }
    wq_unlink(wq, entry);
    spinlock_release_irqrestore(&wq->lock, flags);
    if (entry->proc && entry->proc->wait_entry == entry) {
        entry->proc->wait_entry = NULL;
    }
    // Log OUTSIDE the queue lock and with interrupts restored: kprintf drives
    // the serial port and must never run under a spinlock with IF clear.
    if (rescued && g_wq_unpark_logged < WQ_UNPARK_LOG_MAX) {
        g_wq_unpark_logged++;
        kprintf("[WQ] #610 un-parked pid=%u '%s' after a condition-became-true "
                "wait_event break (rescue #%lu)\n",
                me ? me->pid : 0u, me ? me->name : "?",
                (unsigned long)g_wq_unpark_rescues);
    }
}

void wake_up(wait_queue_head_t *wq) {
    uint64_t flags = spinlock_acquire_irqsave(&wq->lock);
    if (wq->head) {
        wait_queue_entry_t *e = wq->head;
        wait_queue_entry_t *target = e;
        wq_unlink(wq, target);
        target->wake_reason = WAIT_OK;
        if (target->proc) {
            // Clear wait_entry back-pointer before unblocking so a racing
            // sig_raise won't try to double-remove.
            if (target->proc->wait_entry == target) target->proc->wait_entry = NULL;
            proc_wake(target->proc);
        }
    }
    spinlock_release_irqrestore(&wq->lock, flags);
}

void wake_up_all(wait_queue_head_t *wq) {
    uint64_t flags = spinlock_acquire_irqsave(&wq->lock);
    while (wq->head) {
        wait_queue_entry_t *e = wq->head;
        wq_unlink(wq, e);
        e->wake_reason = WAIT_OK;
        if (e->proc) {
            if (e->proc->wait_entry == e) e->proc->wait_entry = NULL;
            proc_wake(e->proc);
        }
    }
    spinlock_release_irqrestore(&wq->lock, flags);
}

void wake_up_process(process_t *p) {
    if (!p) return;
    wait_queue_entry_t *e = p->wait_entry;
    if (!e) {
        // Not sleeping interruptibly on a wait queue; just nudge to READY
        // if currently BLOCKED (e.g., waking a proc sleeping without a wq).
        uint64_t flags = wq_save_flags();
        wq_cli();
        if (p->state == PROC_STATE_BLOCKED) proc_wake(p);
        wq_restore_flags(flags);
        return;
    }
    wait_queue_head_t *wq = e->wq;
    if (!wq) return;
    uint64_t flags = spinlock_acquire_irqsave(&wq->lock);
    // Entry may have been removed by a racing wake_up; check.
    if (e->on_queue) {
        wq_unlink(wq, e);
        e->wake_reason = WAIT_EINTR;
        if (e->proc && e->proc->wait_entry == e) e->proc->wait_entry = NULL;
        if (e->proc) proc_wake(e->proc);
    }
    spinlock_release_irqrestore(&wq->lock, flags);
}
