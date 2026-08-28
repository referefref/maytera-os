// proc/schedrace.h - #75: a REPRODUCER for the SMP context-corruption fault.
//
// THE FAULT (#67 pass 12b, 2 of 5 gate-ON boots):
//     [KERNEL PANIC] Invalid Opcode at RIP=0x41602  RSP=0x10002
//     [KERNEL PANIC] Invalid Opcode at RIP=0x45     RSP=0x10cfaff8
// Neither RIP is a code address in this kernel and RSP=0x10002 is not a stack.
// A core is executing from a wild RIP on a wild RSP: a corrupted or
// half-restored context. "Invalid Opcode" is only what happens when a core
// lands on rubbish. It survives per-core idle processes, the #67 safe handoff
// (process_t::sched_on_cpu cleared inside the switch asm), the BKL theft fix
// and both wake fixes, so it is a SECOND window in the same path.
//
// WHY A REPRODUCER RATHER THAN MORE MEASUREMENT. Twelve passes of #67 each
// found a real defect by measuring, and each time the next measurement found
// another. A fault that appears in 2 boots out of 5, minutes apart, cannot be
// debugged that way. The goal here is inverted from the usual instinct: WIDEN
// THE RACE WINDOW rather than narrow it, until the fault happens every run, and
// capture the full machine state at the instant it does.
//
// THREE PARTS, all compiled in, cheap enough to leave on:
//   1. A per-core RING of the last SCHEDRACE_RING switches. Two stores and an
//      index bump per switch.
//   2. A VALIDATOR run immediately before every context switch, checking the
//      incoming context against everything we know must be true of it. On
//      failure it dumps BOTH cores' rings and the full process state.
//   3. A deliberate DELAY injected into the switch path to widen the window,
//      enabled only by `make SCHEDRACE=1` so it can never reach a golden.
//
// The validator's REACTION is gated the same way sync/noblock.c gates its own:
// detection is always on and always reports; `make SCHEDRACE=1` turns the first
// detection into a kpanic, because for a reproducer a panic WITH STATE is the
// deliverable.
#ifndef SCHEDRACE_H
#define SCHEDRACE_H

#include "../types.h"

#define SCHEDRACE_RING   16   // switches remembered per core
#define SCHEDRACE_CPUS    8

// Sites at which the window can be widened. Named so a report says WHICH gap
// was open when the fault landed.
typedef enum {
    SR_SITE_AFTER_PUBLISH = 0,  // next claimed, prev queued, before the switch
    SR_SITE_AFTER_BKL_DROP,     // BKL released, before context_switch/start
    // #745 (#75): inside the physical allocator's critical section. Unlike the
    // two sites above, interrupts here are NOT off by construction: that is
    // exactly the property under test. On the unfixed allocator the hold runs
    // with IF=1, so widening it makes an interrupt land on the holder, which
    // turns the holder into a Big Kernel Lock waiter and completes the AB-BA
    // deadlock with a core that holds the BKL and wants pmm_lock. On the fixed
    // allocator the same widening runs with IF=0 and cannot be interrupted, so
    // no amount of widening can produce the inversion. The two arms differ in
    // exactly one thing and the reproducer tells them apart in one boot.
    SR_SITE_PMM_HOLD,
    // #75 (enqrace75b): BETWEEN proc_yield() ENQUEUEING A TASK THAT IS STILL
    // RUNNING AND sched_schedule() ARMING sched_on_cpu ON IT.
    //
    // MEASURED, build 1980, 4 vCPU + DOS, 130 s of steady state: proc_yield()
    // enqueued a task that a core was RUNNING 21317 times, which is 100% of that
    // call site's enqueues, and sched_rq_pop_locked() popped a running task ZERO
    // times. The window exists structurally - nothing on the enqueue path can
    // see a running task, because sched_on_cpu is 0 for the whole of a timeslice
    // - but at natural timings it is a handful of instructions wide and was
    // never observed to be entered.
    //
    // A probe that has never fired says nothing about the world until it has
    // been shown capable of firing. This site holds that window open so the
    // [RUNPOP] counter can be SEEN to move, which is the difference between
    // "the window is never entered" and "the probe is dead".
    SR_SITE_YIELD_ENQ,
    SR_SITE_MAX
} schedrace_site_t;

// Record one context switch on this core. Called with interrupts off from
// sched_schedule(), immediately before the switch asm.
void schedrace_note(uint32_t cpu, const void *prev, const void *next);

// Validate the INCOMING context before switching to it. Returns 0 if sane,
// non-zero (and reports, and panics under SCHEDRACE=1) if not.
int  schedrace_check(uint32_t cpu, const void *prev, const void *next,
                     const char *when);

// Widen the window at `site`. A no-op unless built with SCHEDRACE=1.
void schedrace_delay(schedrace_site_t site);

// Dump both cores' switch rings. Safe from a panic path.
void schedrace_dump(const char *why);

// Boot-time proof that the DETECTOR fires on a known-bad context and stays
// quiet on a known-good one. Four measurement instruments were believed and
// wrong during #67; this one gets validated before it is trusted.
void schedrace_selftest(void);

#endif // SCHEDRACE_H
