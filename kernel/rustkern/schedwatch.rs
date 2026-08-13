// rustkern/schedwatch.rs - #67 SMP scheduler: livelock diagnostic + run-queue
// placement policy.
//
// WHY THIS EXISTS. Concurrent user-process scheduling on the Application
// Processors (`g_smp_user_sched`) has been disabled since #421 phase 7 because
// turning it on produced a CONTEXT-SWITCH STORM LIVELOCK that "silently wedges
// the whole box": both cores spinning in sched_schedule()/wake_sleeping_procs(),
// heartbeat dead, NO PANIC and NO LOG LINE. The recorded measurement (#421,
// build 912) is a spike to ~190,000 context switches in 2 seconds, a few
// seconds into AssaultCube, non-deterministic: one boot survived, the next
// hung. You cannot debug what produces no output, so the diagnostic is built
// BEFORE the scheduler change it is meant to police.
//
// THIS module owns the DECISIONS as pure functions over caller-built
// snapshots:
//   * is this core's context-switch rate a storm rather than a busy system,
//   * which run queue should a newly-runnable process be placed on,
//   * which run queue should an idle core steal from.
// The list surgery, the locking and the context-switch handoff stay in C
// (proc/process.c, proc/context_switch.asm): they are intrusive-linked-list
// manipulation entangled with the PCB, the #610 cli() critical section and the
// switch asm itself. Same split, and the same reason, as rustkern/sched_age.rs.
//
// Everything here is integer-only: the kernel target is soft-float with SSE
// disabled (x86_64-unknown-none, CFLAGS -mno-sse -mno-sse2), so a percentage is
// computed as a scaled integer and never as a float.

/// Verdict returned by [`sched_storm_verdict_rs`].
pub const SCHED_OK: i32 = 0;
/// The core is switching context far faster than any real workload can, which
/// is the #421 livelock signature.
pub const SCHED_STORM: i32 = 1;

/// Decide whether a closed measurement window is a context-switch STORM.
///
/// `switches`  context switches this core performed in the window.
/// `ticks`     timer ticks that elapsed across the same window.
/// `limit`     switches-per-tick above which the window is a storm.
///
/// Returns [`SCHED_OK`] or [`SCHED_STORM`].
///
/// Policy, and why each rule is here:
///  * A window shorter than one tick carries no rate information at all: two
///    switches inside a single tick is normal, and dividing by zero ticks would
///    make every such window a storm. Such a window is REFUSED (SCHED_OK), and
///    the C caller keeps accumulating rather than closing it.
///  * The comparison is `switches > limit * ticks`, done in u64 with a
///    saturating multiply. There is no division, so there is no rounding to
///    argue about and no divide-by-zero to guard at the call site.
///  * `limit == 0` would make every window with a single switch a storm, so it
///    is refused. A caller that wants "any switching at all is wrong" should
///    not be using a rate detector.
///  * The measured storm was ~380 switches per tick sustained; a busy
///    MayteraOS desktop measures in single digits per tick. Any limit chosen
///    inside that two-order-of-magnitude gap separates them, so this function
///    deliberately does NOT bake the threshold in: the C side owns the constant
///    and can tune it without touching the decision.
#[no_mangle]
pub extern "C" fn sched_storm_verdict_rs(switches: u64, ticks: u64, limit: u32) -> i32 {
    if ticks == 0 || limit == 0 {
        return SCHED_OK;
    }
    // Saturating: an absurd limit*ticks must not wrap to a small number and
    // manufacture a storm verdict out of a quiet window.
    let budget = (limit as u64).saturating_mul(ticks);
    if switches > budget {
        SCHED_STORM
    } else {
        SCHED_OK
    }
}

// ===========================================================================
// PRIORITY-AWARE PLACEMENT (#67 pass 2)
// ===========================================================================
//
// THE POLICY THIS REPLACES WAS PRIORITY-BLIND, and per-core sorted queues make
// that worse rather than better. Each core's queue is kept sorted by effective
// priority, so every core is LOCALLY correct; choosing a core by QUEUE DEPTH
// then makes the SYSTEM globally wrong. A PRIO_REALTIME process could be placed
// second in a two-deep queue while another core ran a PRIO_LOW process, or sat
// idle. On one core that state is impossible; adding a core created it.
//
// THE INVARIANT THIS POLICY DEFENDS: the highest-priority runnable process in
// the system should be RUNNING on some core, not queued behind lower-priority
// work. Exactly maintaining that needs a single global queue, which is the
// contention this ticket exists to remove, so it is maintained by placement
// plus preemption instead:
//
//   1. An IDLE core always wins. Work waiting while a core idles is the worst
//      outcome available and costs nothing to fix.
//   2. Otherwise, a core whose RUNNING process has a strictly LOWER effective
//      priority than the arriving one wins, and the caller is told to PREEMPT
//      it. This is what keeps the invariant true across cores: the lower-
//      priority process goes back on a queue and the higher-priority one runs
//      now. Deepest-victim first, so the highest-priority arrival displaces the
//      most-contended core.
//   3. Otherwise nothing can run immediately, so the arrival is queued where it
//      will be reached soonest: fewest entries AT OR ABOVE its own priority
//      (those are what it actually waits behind - entries below it will sort
//      after it), then fewest entries overall, then affinity, then lowest
//      index. Counting only the entries that outrank it is the difference
//      between "shortest queue" and "shortest WAIT", and they are not the same
//      queue once priorities differ.
//
// Ties are broken deterministically (lowest core index) throughout. #421's
// central difficulty was that the livelock was non-deterministic; a scheduler
// that makes a different choice on identical input would add a second,
// independent source of that.

/// Per-core scheduling snapshot, built by the caller under the run-queue lock.
/// Mirrors sched_core_state_t in proc/process.c; sizeof-locked there by
/// _Static_assert.
#[repr(C)]
pub struct SchedCoreState {
    /// Entries waiting on this core's run queue.
    pub queue_len: u32,
    /// Entries waiting here whose effective priority is >= the arriving
    /// process's. Filled in by the caller, which is the only side that can walk
    /// the intrusive list. u32::MAX is never valid.
    pub above_len: u32,
    /// Effective priority of the process CURRENTLY RUNNING on this core, or
    /// [`PRIO_NONE`] when the core is running its idle process.
    pub cur_prio: i32,
    /// bit 0: this core consumes its own run queue.
    pub flags: u32,
}

/// A core running its idle process reports this as `cur_prio`. Below every real
/// priority including PRIO_IDLE (0), so an idle core always compares as
/// preemptible.
pub const PRIO_NONE: i32 = -1;

/// bit 0 of [`SchedCoreState::flags`]: this core actually pops its own run
/// queue. A core without it is never chosen. See the black-hole note below.
pub const CORE_CONSUMER: u32 = 1;

/// Placement result: preempt the chosen core (it is running something the
/// arriving process outranks).
pub const PLACE_PREEMPT: i32 = 0x100;

/// Choose the run queue for a newly-runnable process, and say whether the
/// chosen core must be preempted.
///
/// `cores[0..ncpu]`  per-core snapshot, index = core id.
/// `ncpu`            number of cores.
/// `prio`            effective priority of the arriving process.
/// `prev_cpu`        core it last ran on, or a value >= ncpu for no affinity.
///
/// Returns the core index, ORed with [`PLACE_PREEMPT`] when that core should be
/// made to reschedule immediately; or -1 when no core is eligible.
///
/// ELIGIBILITY IS A CORRECTNESS RULE, NOT AN OPTIMISATION, and the first
/// gate-ON boot proved it: with cores chosen purely by queue depth, a process
/// was placed on cpu1's queue, cpu1's work loop had no code path that pops a run
/// queue, and the process NEVER RAN. The boot reached COMPOSITOR_UP and stopped
/// there, with no panic and no error. A queue with no consumer is a black hole.
/// A core advertises itself with [`CORE_CONSUMER`] only once it is genuinely
/// driving the scheduler.
///
/// AFFINITY is deliberately the LAST tie-break rather than an override. Keeping
/// a process on its old core saves a CR3 reload and a cold cache, which is worth
/// something; it is not worth leaving a core idle or leaving a higher-priority
/// process queued, so it only decides between otherwise-equal candidates.
#[no_mangle]
pub extern "C" fn sched_place_rs(
    cores: *const SchedCoreState,
    ncpu: u32,
    prio: i32,
    prev_cpu: u32,
) -> i32 {
    if cores.is_null() || ncpu == 0 {
        return -1;
    }
    let n = ncpu as usize;
    // SAFETY: the caller guarantees `cores` points at `ncpu` contiguous,
    // readable, properly aligned SchedCoreState (a stack array in
    // sched_rq_push(), filled under the run-queue lock). Every access below goes
    // through this slice, so the indexing is bounds-checked.
    let c: &[SchedCoreState] = unsafe { core::slice::from_raw_parts(cores, n) };
    let ok = |i: usize| (c[i].flags & CORE_CONSUMER) != 0;

    // 1. An idle consumer core. Prefer affinity among idle cores, then lowest.
    let mut idle_pick: Option<usize> = None;
    for i in 0..n {
        if ok(i) && c[i].cur_prio == PRIO_NONE {
            if (prev_cpu as usize) == i {
                return i as i32; // idle AND our old core: nothing beats this
            }
            if idle_pick.is_none() {
                idle_pick = Some(i);
            }
        }
    }
    if let Some(i) = idle_pick {
        return i as i32;
    }

    // 2. A core running something we outrank: take it and preempt. Choose the
    //    one running the LOWEST-priority victim, then the deepest queue, then
    //    the lowest index. Displacing the lowest-priority runner is what makes
    //    this a system-wide priority rule rather than a per-core one.
    let mut vic: Option<usize> = None;
    for i in 0..n {
        if !ok(i) || c[i].cur_prio >= prio {
            continue;
        }
        vic = Some(match vic {
            None => i,
            Some(b) => {
                if c[i].cur_prio < c[b].cur_prio
                    || (c[i].cur_prio == c[b].cur_prio && c[i].queue_len > c[b].queue_len)
                {
                    i
                } else {
                    b
                }
            }
        });
    }
    if let Some(i) = vic {
        return (i as i32) | PLACE_PREEMPT;
    }

    // 3. Nothing can run us now: queue where the WAIT is shortest, which is the
    //    fewest entries that outrank us, not the fewest entries.
    let better = |i: usize, b: usize| -> bool {
        if c[i].above_len != c[b].above_len {
            return c[i].above_len < c[b].above_len;
        }
        if c[i].queue_len != c[b].queue_len {
            return c[i].queue_len < c[b].queue_len;
        }
        if (prev_cpu as usize) == i {
            return true; // affinity, last tie-break only
        }
        false
    };
    let mut best: Option<usize> = None;
    for i in 0..n {
        if !ok(i) {
            continue;
        }
        best = Some(match best {
            None => i,
            Some(b) => {
                if better(i, b) {
                    i
                } else {
                    b
                }
            }
        });
    }
    match best {
        Some(b) => b as i32,
        None => -1,
    }
}

/// Choose a run queue for an idle core to steal one process from.
///
/// `cores[0..ncpu]`  per-core snapshot, index = core id.
/// `ncpu`            number of cores.
/// `self_cpu`        the calling (idle) core.
///
/// Returns the core to steal from, or -1 when there is nothing to take.
///
/// Policy, and why each rule is here:
///  * Steal from the core holding the HIGHEST-PRIORITY waiting process, not the
///    deepest queue. The caller is idle, so whatever it takes runs immediately;
///    taking the most important waiting work is what makes the "the highest-
///    priority runnable process is running somewhere" invariant self-healing
///    rather than something placement has to get right every time.
///  * One entry is enough to justify a steal, because THIS core is idle: a
///    process waiting while a core idles is the failure being fixed. The
///    ping-pong that a depth threshold guards against cannot happen here, since
///    the stolen process is run rather than re-queued.
///  * A core that does not consume its own queue must not pull work to itself.
///    It may still be stolen FROM: its queue is exactly the stranded work that
///    needs rescuing.
///  * Deterministic: highest waiting priority, then deepest queue, then lowest
///    index.
#[no_mangle]
pub extern "C" fn sched_steal_rs(
    cores: *const SchedCoreState,
    ncpu: u32,
    self_cpu: u32,
    top_prios: *const i32,
) -> i32 {
    if cores.is_null() || top_prios.is_null() || ncpu < 2 || (self_cpu as usize) >= (ncpu as usize) {
        return -1;
    }
    let n = ncpu as usize;
    // SAFETY: as in sched_place_rs. `top_prios` is a parallel array of `ncpu`
    // readable i32 (effective priority of each queue's head, PRIO_NONE when
    // empty), built by the same caller under the same lock.
    let c: &[SchedCoreState] = unsafe { core::slice::from_raw_parts(cores, n) };
    let t: &[i32] = unsafe { core::slice::from_raw_parts(top_prios, n) };

    let me = self_cpu as usize;
    if (c[me].flags & CORE_CONSUMER) == 0 {
        return -1;
    }
    let mut best: Option<usize> = None;
    for i in 0..n {
        if i == me || c[i].queue_len == 0 || t[i] == PRIO_NONE {
            continue;
        }
        best = Some(match best {
            None => i,
            Some(b) => {
                if t[i] > t[b] || (t[i] == t[b] && c[i].queue_len > c[b].queue_len) {
                    i
                } else {
                    b
                }
            }
        });
    }
    match best {
        Some(b) => b as i32,
        None => -1,
    }
}

/// Summarise per-core busy ticks as whole percentages, for the `[SCHEDCORE]`
/// serial report.
///
/// `busy[0..ncpu]`  ticks each core spent running a non-idle process in the
///                  window.
/// `total`          ticks in the window.
/// `out[0..ncpu]`   written with 0..100 for each core; always fully
///                  initialised on every success path.
///
/// Returns 0, or -1 on invalid arguments.
///
/// This exists so the answer to "is work actually spread across the cores"
/// is a NUMBER on the serial console, comparable with the Proxmox per-VM CPU
/// graph the user read, rather than an aggregate that cannot distinguish
/// "one core at 100%" from "two cores at 50%" - which is precisely the
/// ambiguity that made this ticket necessary.
///
/// Saturates at 100 rather than reporting an impossible value: a core can
/// accumulate slightly more busy ticks than the window if the window boundary
/// is sampled between the two counters, and a 103% reading in a diagnostic
/// destroys trust in the whole line.
#[no_mangle]
pub extern "C" fn sched_core_pct_rs(
    busy: *const u64,
    ncpu: u32,
    total: u64,
    out: *mut u32,
    outcap: u32,
) -> i32 {
    if busy.is_null() || out.is_null() || ncpu == 0 || ncpu > outcap {
        return -1;
    }
    let n = ncpu as usize;
    // SAFETY: caller guarantees `busy` points at `ncpu` readable u64 and `out`
    // at `outcap >= ncpu` writable u32 (both stack arrays in
    // sched_smp_report()). All access is through these bounds-checked slices.
    let b: &[u64] = unsafe { core::slice::from_raw_parts(busy, n) };
    let o: &mut [u32] = unsafe { core::slice::from_raw_parts_mut(out, n) };

    if total == 0 {
        for slot in o.iter_mut() {
            *slot = 0;
        }
        return 0;
    }
    for i in 0..n {
        let pct = b[i].saturating_mul(100) / total;
        o[i] = if pct > 100 { 100 } else { pct as u32 };
    }
    0
}

// ---------------------------------------------------------------------------
// Self-tests. Compiled into the kernel and run from sched_smp_selftest() so
// that a policy regression fails at boot on the serial console rather than as
// a mis-scheduled process three subsystems away. Returns a bitmask of FAILED
// checks (0 = all pass), so one line of output names exactly which rule broke.
// ---------------------------------------------------------------------------

/// Run the schedwatch policy self-tests. Returns 0 on success, or a bitmask of
/// failing check ids.
#[no_mangle]
pub extern "C" fn sched_watch_selftest_rs() -> u32 {
    let mut fail: u32 = 0;

    // 1: a quiet window is not a storm; the measured storm rate is.
    if sched_storm_verdict_rs(500, 250, 80) != SCHED_OK {
        fail |= 1 << 0;
    }
    if sched_storm_verdict_rs(190_000, 500, 80) != SCHED_STORM {
        fail |= 1 << 1;
    }
    // 2: a zero-tick or zero-limit window carries no rate information.
    if sched_storm_verdict_rs(u64::MAX, 0, 80) != SCHED_OK {
        fail |= 1 << 2;
    }
    if sched_storm_verdict_rs(10, 1, 0) != SCHED_OK {
        fail |= 1 << 3;
    }
    // 3: an absurd limit must saturate, not wrap into a storm verdict.
    if sched_storm_verdict_rs(1, u64::MAX, u32::MAX) != SCHED_OK {
        fail |= 1 << 4;
    }

    // 4: PLACEMENT. Helper: (queue_len, above_len, cur_prio, consumer).
    fn cs(q: u32, above: u32, cur: i32, consumer: bool) -> SchedCoreState {
        SchedCoreState {
            queue_len: q,
            above_len: above,
            cur_prio: cur,
            flags: if consumer { CORE_CONSUMER } else { 0 },
        }
    }
    const RT: i32 = 4; // PRIO_REALTIME
    const NORM: i32 = 2; // PRIO_NORMAL
    const LOW: i32 = 1; // PRIO_LOW

    // An idle core always wins, even against an empty queue on a busy core.
    let a = [cs(0, 0, NORM, true), cs(0, 0, PRIO_NONE, true)];
    if sched_place_rs(a.as_ptr(), 2, NORM, 9) != 1 {
        fail |= 1 << 5;
    }
    // THE PRIORITY-INVERSION CASE THIS POLICY EXISTS FOR. cpu0 is idle-free but
    // runs PRIO_LOW with an empty queue; cpu1 runs PRIO_REALTIME with an empty
    // queue. A PRIO_NORMAL arrival must PREEMPT cpu0, not queue behind either.
    // Depth-based placement scored these two cores identically.
    let b = [cs(0, 0, LOW, true), cs(0, 0, RT, true)];
    if sched_place_rs(b.as_ptr(), 2, NORM, 9) != (0 | PLACE_PREEMPT) {
        fail |= 1 << 6;
    }
    // A process that outranks nobody must NOT preempt, and must queue where the
    // fewest entries OUTRANK it - even though that queue is DEEPER. This is the
    // shortest-wait vs shortest-queue distinction.
    //   cpu0: 1 waiting, all above us.   cpu1: 3 waiting, none above us.
    let c = [cs(1, 1, RT, true), cs(3, 0, RT, true)];
    if sched_place_rs(c.as_ptr(), 2, NORM, 9) != 1 {
        fail |= 1 << 7;
    }
    // Affinity is the LAST tie-break: identical cores keep the process put.
    let d = [cs(2, 1, RT, true), cs(2, 1, RT, true)];
    if sched_place_rs(d.as_ptr(), 2, NORM, 1) != 1 {
        fail |= 1 << 8;
    }
    // ...and never overrides eligibility, idleness or priority.
    let e = [cs(0, 0, PRIO_NONE, true), cs(5, 5, RT, true)];
    if sched_place_rs(e.as_ptr(), 2, NORM, 1) != 0 {
        fail |= 1 << 9;
    }
    // THE STRANDING RULE (regression test for the first gate-ON boot): cpu1 is
    // idle with an empty queue, but does not consume it. Everything must still
    // go to cpu0, even though cpu0 is busy and deep.
    let f = [cs(4, 4, RT, true), cs(0, 0, PRIO_NONE, false)];
    if sched_place_rs(f.as_ptr(), 2, NORM, 9) != 0 {
        fail |= 1 << 10;
    }
    if sched_place_rs(f.as_ptr(), 2, NORM, 1) != 0 {
        fail |= 1 << 11; // affinity to a non-consumer must not strand either
    }
    let g = [cs(0, 0, PRIO_NONE, false), cs(0, 0, PRIO_NONE, false)];
    if sched_place_rs(g.as_ptr(), 2, NORM, 9) != -1 {
        fail |= 1 << 12; // nobody consumes: refuse rather than guess
    }
    if sched_place_rs(core::ptr::null(), 2, NORM, 9) != -1 {
        fail |= 1 << 13;
    }
    if sched_place_rs(a.as_ptr(), 0, NORM, 9) != -1 {
        fail |= 1 << 14;
    }

    // 5: STEALING takes the highest-priority waiting process, not the deepest
    //    queue. cpu0 has 4 waiting at PRIO_LOW; cpu2 has 1 waiting at
    //    PRIO_REALTIME. An idle cpu1 must take the REALTIME one.
    let h = [
        cs(4, 0, LOW, true),
        cs(0, 0, PRIO_NONE, true),
        cs(1, 0, NORM, true),
    ];
    let tp = [LOW, PRIO_NONE, RT];
    if sched_steal_rs(h.as_ptr(), 3, 1, tp.as_ptr()) != 2 {
        fail |= 1 << 15;
    }
    // One waiting entry is enough when the caller is idle.
    let i2 = [cs(1, 0, NORM, true), cs(0, 0, PRIO_NONE, true)];
    let tp2 = [NORM, PRIO_NONE];
    if sched_steal_rs(i2.as_ptr(), 2, 1, tp2.as_ptr()) != 0 {
        fail |= 1 << 16;
    }
    // Nothing waiting anywhere, and never from yourself.
    let j = [cs(0, 0, PRIO_NONE, true), cs(0, 0, PRIO_NONE, true)];
    let tp3 = [PRIO_NONE, PRIO_NONE];
    if sched_steal_rs(j.as_ptr(), 2, 1, tp3.as_ptr()) != -1 {
        fail |= 1 << 17;
    }
    if sched_steal_rs(i2.as_ptr(), 2, 0, tp2.as_ptr()) != -1 {
        fail |= 1 << 18; // cpu0 holds the only work; it must not steal from itself
    }
    // A non-consumer must not pull work to itself, even when it is idle.
    let k = [cs(3, 0, NORM, true), cs(0, 0, PRIO_NONE, false)];
    let tp4 = [RT, PRIO_NONE];
    if sched_steal_rs(k.as_ptr(), 2, 1, tp4.as_ptr()) != -1 {
        fail |= 1 << 19;
    }
    if sched_steal_rs(k.as_ptr(), 1, 0, tp4.as_ptr()) != -1 {
        fail |= 1 << 20; // a single core has nobody to steal from
    }

    // 6: percentages are whole, saturated and fully written.
    let busy: [u64; 2] = [250, 125];
    let mut pct: [u32; 2] = [7, 7];
    if sched_core_pct_rs(busy.as_ptr(), 2, 250, pct.as_mut_ptr(), 2) != 0
        || pct[0] != 100
        || pct[1] != 50
    {
        fail |= 1 << 21;
    }
    let over: [u64; 2] = [400, 0];
    if sched_core_pct_rs(over.as_ptr(), 2, 250, pct.as_mut_ptr(), 2) != 0 || pct[0] != 100 {
        fail |= 1 << 22;
    }
    if sched_core_pct_rs(busy.as_ptr(), 2, 250, pct.as_mut_ptr(), 1) != -1 {
        fail |= 1 << 23;
    }

    fail
}
