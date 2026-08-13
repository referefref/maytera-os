// noblock.h - #426 Phase 2: the runtime "may this context block?" assertion.
//
// CLAUDE.md has described wq_assert_may_block() as "being added" since
// 2026-07-07. #514 measured it and found it did not exist anywhere in the
// kernel: `grep -rn wq_assert_may_block kernel/` matched documentation only.
// This file closes that gap.
//
// WHAT A NO-BLOCK CONTEXT IS
// --------------------------
// Blocking means "park this thread and let the scheduler run someone else,
// then be woken later". Three preconditions must ALL hold for that to be a
// legal thing to do, and each one has a real bug behind it:
//
//   1. THE SCHEDULER MUST BE LIVE. Before proc_init()/preemption there is
//      nothing to switch to, so a park is a permanent halt. (Early-boot device
//      probes are the usual offender; that is why every pre-scheduler settle
//      loop in ata/ahci/xhci/apic is a delay and not a wait.)
//
//   2. WE MUST BE A PROCESS. A wait_queue_entry_t is allocated on the WAITER'S
//      KERNEL STACK and points at a process_t. With proc_current() == NULL
//      there is no thread to park and nothing for a waker to make runnable.
//
//   3. INTERRUPTS MUST BE ON (RFLAGS.IF set). This is the big one. With IF
//      clear we are either inside an interrupt gate (an ISR) or inside a
//      cli+spinlock critical section, i.e. spinlock_acquire_irqsave(). Parking
//      there is a DEADLOCK, not slowness: the timer tick that would run the
//      wake sweep, and the device IRQ that would call wake_up(), are both
//      masked by the very state we are parked in, and if we hold a lock the
//      waker needs, nobody can ever set our condition either. #549 is the
//      precedent (a TX path holding net_lock that blocked on a wait queue),
//      and #287 (ata_dma_wait under g_ata_io_lock) is a live example.
//
// This is a deliberate GENERALISATION of drivers/xhci.c's xhci_may_block()
// (#614/#615/#616), which already refuses to sleep unless exactly those three
// hold. Rather than let a second private mechanism grow, xhci_may_block() is
// now a thin wrapper over wq_may_block() and there is ONE definition of the
// rule in the tree (CLAUDE.md: reuse the canonical primitive).
//
// WHAT IS **NOT** COVERED, AND WHY (honest scope)
// -----------------------------------------------
// "Holding a plain, non-irqsave spinlock" is NOT detected. Every spinlock in
// the tree that is held across anything interesting is taken with
// spinlock_acquire_irqsave(), so condition 3 already catches those. Detecting
// a bare spinlock_acquire() needs a lock-depth counter, and a GLOBAL counter
// is unsound: a timer IRQ can preempt a thread that holds a plain spinlock,
// switch to another thread, and that innocent thread would then see a non-zero
// depth and be falsely accused. A SOUND counter has to live on the process
// (process_t), which means calling proc_current() on every single lock
// acquire/release in the kernel. That is a change to the hottest primitive we
// have, and landing it in the same commit as the wait-path assertion was
// judged too risky. It is written down here as a known gap rather than
// pretended away, which is the whole point of #514.
//
// WHY THIS IS ALWAYS-ON AND NOT DEBUG-ONLY
// ----------------------------------------
// The detection cost is one call to sched_preemption_enabled() (a bool load),
// one proc_current() (two loads and a branch), and one `pushfq; popq` - a few
// nanoseconds, on a path that is ABOUT TO CONTEXT SWITCH (microseconds). A
// guardrail that only runs in a build nobody makes is worthless (#514's whole
// finding), so the DETECTION always runs, in the shipping kernel, and reports
// on serial. Only the REACTION is build-gated:
//
//   default build          - loud, rate-limited, de-duplicated [WQBLOCK]
//                            report on serial naming pid/name/reason/caller,
//                            then continue. A field kernel must not be made
//                            less bootable by its own assertion.
//   -DWQ_NOBLOCK_PANIC     - kpanic() on the first violation. Use when you are
//                            hunting one. Built by: make wq-noblock-panic
//
// SERIAL ONLY. It must never write to the filesystem: #597 (file-write tracing
// corrupts ext2), and we may be inside the block layer when we fire.

#ifndef SYNC_NOBLOCK_H
#define SYNC_NOBLOCK_H

#include "../types.h"

// Reason bits returned by wq_noblock_reason(). 0 means "this context may block".
#define WQ_NB_OK          0x0u
#define WQ_NB_NO_SCHED    0x1u   // scheduler/preemption not live yet
#define WQ_NB_NO_PROC     0x2u   // proc_current() == NULL: nothing to park
#define WQ_NB_IRQ_OFF     0x4u   // RFLAGS.IF clear: ISR or cli+spinlock section

// Why (if at all) the CURRENT context must not block. 0 == may block.
uint32_t wq_noblock_reason(void);

// Convenience predicate. 1 == it is legal to park this thread.
static inline int wq_may_block(void) { return wq_noblock_reason() == 0; }

// THE ASSERTION. Call at a blocking chokepoint, before anything is parked.
// `what` names the primitive ("wait_event", "futex_wait", ...). `caller` is
// normally __builtin_return_address(0). Reports (or panics, see the header
// comment) if the current context must not block. Returns the reason bits so a
// caller can also degrade gracefully.
uint32_t wq_assert_may_block(const char *what, void *caller);

// Total violations observed since boot. Non-zero is not a warning, it is the
// bug class firing. Surfaced in the [HB] heartbeat next to #610's
// g_wq_unpark_rescues so a long run can be audited without serial capture.
extern volatile uint64_t g_wq_noblock_violations;

#endif // SYNC_NOBLOCK_H
