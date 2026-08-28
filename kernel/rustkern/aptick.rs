// rustkern/aptick.rs - #169: the PER-CORE PREEMPTION DECISION for a scheduler
// tick taken on an Application Processor.
//
// WHY THIS TICKET EXISTS. The kernel has always run on the PIT (cpu/apic.c),
// and IRQ0 is delivered to the BSP only. The single LAPIC-timer caller was the
// #62 redundant tick, also BSP-only (cpu/isr.c). So an AP took NO scheduler
// tick at all and its preemption was COOPERATIVE: a process there ran until it
// blocked, yielded or exited. sched_ap_enter() in proc/process.c said so in as
// many words and named arming the LAPIC per core as the remaining work.
//
// MEASURED CONSEQUENCE (#168 Job 1, 8 CPU-bound Ring-3 workers on 4 vCPU, gate
// ON, one boot): the worker on slot 5 completed 48 work units while the worker
// on slot 0 completed 40,192 in the same boot. 837x between two identical
// processes. All workers share one guest clock, so the RATIO is trustworthy
// even though that arm's total was not.
//
// WHAT THIS MODULE OWNS. The DECISION only: given what this core is running and
// what it has been asked to do, which of five actions the tick should take. The
// mutation (writing cur->time_slice, calling sched_schedule(), bumping the
// per-core counters) stays in C, exactly as rustkern/sched_age.rs splits the
// promote DECISION from the intrusive-list surgery. The rest of the change,
// programming the LAPIC LVT, an IDT gate, an interrupt stub, is MMIO and
// assembly entanglement and stays in C/asm for that stated reason.
//
// THE ONE INVARIANT THIS ENCODES, and it is the whole reason the AP tick is a
// separate function from sched_tick(): AN AP TICK MUST NEVER TOUCH GLOBAL
// STATE. Not timer_ticks (four writers would make the wall clock run at four
// times real speed and break every "timer_ticks + N" deadline in the tree),
// not sched_ticks, not g_sched_tick_samples, not the g_cpu_idle_acc /
// g_cpu_total_acc aggregate behind g_cpu_pct, and none of the once-per-tick
// global hooks (cron_tick, futex_tick, proc_child_exit_notify, the mm-lock
// watchdog). Those have exactly one owner core and keep it.

/// Actions the caller must perform, as a bitmask. Every one is a write the C
/// side owns; this module never performs any of them.
pub const APTICK_BUSY:     u32 = 1 << 0; // credit this core one busy tick
pub const APTICK_ACK_RESC: u32 = 1 << 1; // clear g_need_resched[cpu]
pub const APTICK_CHARGE:   u32 = 1 << 2; // cur->total_time++
pub const APTICK_SETSLICE: u32 = 1 << 3; // store *out_slice into cur->time_slice
pub const APTICK_SCHED:    u32 = 1 << 4; // call sched_schedule()

/// Decide what an AP scheduler tick should do.
///
/// `preempt_enabled` global sched_set_preemption() state (0/1).
/// `has_cur`         is there a current process on THIS core (0/1).
/// `is_idle`         is that process this core's idle process (0/1).
/// `need_resched`    a cross-core preemption request is pending for this core.
/// `slice`           cur->time_slice as read by the caller.
/// `out_slice`       written with the new slice value whenever APTICK_SETSLICE
///                   is returned; UNTOUCHED otherwise. Never read.
///
/// Returns the action bitmask.
#[no_mangle]
pub extern "C" fn ap_tick_decide_rs(preempt_enabled: u32,
                                    has_cur: u32,
                                    is_idle: u32,
                                    need_resched: u32,
                                    slice: u32,
                                    out_slice: *mut u32) -> u32 {
    // No current process on this core: it is between contexts. Nothing to
    // charge and nothing to preempt. NOT the same as idle, which is a real
    // process with a PCB of its own (#75 gave every AP one).
    if has_cur == 0 { return 0; }

    let idle = is_idle != 0;
    let mut act: u32 = 0;

    // PER-CORE busy accounting, counted on the core itself while it is
    // executing. Unlike the aggregate meter this DOES count a tick taken while
    // the core was in Ring 3, because the interrupt suspends Ring 3 and the
    // current process is the user process.
    if !idle { act |= APTICK_BUSY; }

    // A cross-core preemption request is consumed by EXPIRING THE SLICE and
    // letting the ordinary path below act on it, never by rescheduling
    // directly. sched_tick() learned this the hard way at #67 pass 7: an early
    // return after rescheduling skipped the slice accounting entirely, so with
    // requests arriving regularly round-robin preemption stopped outright
    // (ctxsw fell to about 13/s from about 350/s). One code path does
    // preemption.
    let mut slice_now = slice;
    if need_resched != 0 {
        act |= APTICK_ACK_RESC;
        if !idle && slice_now != 0 { slice_now = 0; act |= APTICK_SETSLICE; }
    }

    // The idle process drives itself. sched_ap_enter()'s loop polls its run
    // queue and calls sched_schedule() on its own, and its halt is a
    // mask/re-check/hlt that cannot lose a wake. Rescheduling it from inside an
    // interrupt would re-enter the scheduler on a core already sitting in it.
    // The tick is still VALUABLE on an idle core: returning from the hlt makes
    // that loop re-poll, which is a redundant always-armed wake source
    // (CLAUDE.md preference order option 1), not a poll loop.
    if idle {
        if act & APTICK_SETSLICE != 0 { unsafe { *out_slice = slice_now; } }
        return act;
    }

    // Preemption globally disabled: still account, never switch.
    if preempt_enabled == 0 {
        if act & APTICK_SETSLICE != 0 { unsafe { *out_slice = slice_now; } }
        return act;
    }

    act |= APTICK_CHARGE;

    if slice_now > 0 { slice_now -= 1; act |= APTICK_SETSLICE; }
    if slice_now == 0 { act |= APTICK_SCHED; }

    if act & APTICK_SETSLICE != 0 { unsafe { *out_slice = slice_now; } }
    act
}
