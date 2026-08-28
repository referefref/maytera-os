// rqlock.rs - #143 part 2: is the run-queue lock actually the bottleneck?
//
// WHY THIS EXISTS. #143 was raised as "8 run queues share ONE global lock", with
// per-queue locking as the obvious remedy. Before restructuring a scheduler lock
// in a kernel where SMP already wedges three other ways (#165), the claim has to
// be a NUMBER. This module turns the raw counters C collects at the lock into a
// verdict, so "contended" stops being an adjective.
//
// WHY THE COUNTERS ARE INCREMENTED IN C AND ONLY JUDGED HERE. The increments sit
// inside spinlock_acquire_irqsave_acct() and between rq_lock_at()/rq_unlock(),
// i.e. on the scheduler's hot path with interrupts already masked, on a core
// that by construction may not block. That is the same split #83 used: C for the
// two hot stores, Rust for the accounting. The judgement below runs once per
// ~4 s reporting window from sched_smp_report(), where the cost is irrelevant
// and the arithmetic is worth having checked.
//
// NO FLOAT. The kernel target is soft-float with SSE disabled
// (x86_64-unknown-none, CFLAGS -mno-sse -mno-sse2), so every ratio here is
// integer arithmetic scaled by hand, exactly as schedwatch.rs does.

/// The lock was not a limiting factor in this window.
pub const RQ_OK: i32 = 0;
/// A noticeable share of acquisitions had to wait for another core.
pub const RQ_HOT: i32 = 1;
/// The lock is held for so much of wall time that cores cannot help but collide;
/// this is the state where splitting the lock, or shortening the critical
/// section, actually buys something.
pub const RQ_SATURATED: i32 = 2;

/// Percent of acquisitions that found the lock already held, above which the
/// window is called HOT.
///
/// Chosen, not measured, and deliberately low. An uncontended run-queue lock on
/// a machine where only one core schedules (the shipping gate-off config) should
/// report ZERO contended acquisitions, not "a few percent": there is no second
/// scheduler to collide with. So any sustained non-trivial figure here is
/// already telling you something, and 5% is low enough to notice a regression
/// while being clear of single-sample noise on a nearly idle window.
const HOT_CONTENDED_PCT: u64 = 5;

/// Percent of WALL TIME the lock was held, above which the window is called
/// SATURATED. Held-time is the quantity that decides whether splitting the lock
/// can help at all: if the lock is held 2% of the time, four cores contending
/// for it is a scheduling curiosity, and if it is held 60% of the time it is the
/// ceiling no amount of queue-splitting will lift until the critical section
/// itself shrinks.
const SAT_HELD_PCT: u64 = 25;

/// Judge one reporting window.
///
/// `acquires`  - lock acquisitions in the window (all cores)
/// `contended` - of those, how many found it already held
/// `held_us`   - total microseconds the lock was held, summed over cores
/// `window_us` - wall-clock length of the window
///
/// Returns RQ_OK / RQ_HOT / RQ_SATURATED. A window with no acquisitions is OK:
/// a lock nobody took cannot be a bottleneck, and reporting anything else would
/// make an idle machine look broken.
#[no_mangle]
pub extern "C" fn rqlock_verdict_rs(acquires: u64, contended: u64, held_us: u64, window_us: u64) -> i32 {
    if acquires == 0 {
        return RQ_OK;
    }
    if window_us > 0 && held_us.saturating_mul(100) / window_us >= SAT_HELD_PCT {
        return RQ_SATURATED;
    }
    // contended can never exceed acquires; guard anyway rather than trust a
    // counter pair updated by several cores without a lock between them.
    let c = if contended > acquires { acquires } else { contended };
    if c.saturating_mul(100) / acquires >= HOT_CONTENDED_PCT {
        return RQ_HOT;
    }
    RQ_OK
}

/// Contended acquisitions as a percentage, for the report line. Saturating and
/// zero-safe so the caller never has to guard the division.
#[no_mangle]
pub extern "C" fn rqlock_contended_pct_rs(acquires: u64, contended: u64) -> u32 {
    if acquires == 0 {
        return 0;
    }
    let c = if contended > acquires { acquires } else { contended };
    (c.saturating_mul(100) / acquires) as u32
}

/// Percentage of wall time the lock was held. Can legitimately exceed 100 once
/// more than one core is taking it, because held_us is a sum over cores; that is
/// information, not an error, so it is NOT clamped. A figure over 100 means the
/// lock was held on more than one core at once across the window, which for a
/// single mutual-exclusion lock means the cores were serialising on it.
#[no_mangle]
pub extern "C" fn rqlock_held_pct_rs(held_us: u64, window_us: u64) -> u32 {
    if window_us == 0 {
        return 0;
    }
    (held_us.saturating_mul(100) / window_us) as u32
}

/// RED/GREEN self-test. Returns 0 on success, or the number of the first failing
/// case, because "it compiled" is not evidence that a verdict function verdicts.
/// Every case asserts a specific classification, including the ones that must
/// NOT fire.
#[no_mangle]
pub extern "C" fn rqlock_selftest_rs() -> u32 {
    // 1: an idle window is OK, not saturated.
    if rqlock_verdict_rs(0, 0, 0, 1_000_000) != RQ_OK {
        return 1;
    }
    // 2: busy but uncontended is OK. This is the shipping gate-off shape.
    if rqlock_verdict_rs(100_000, 0, 1_000, 1_000_000) != RQ_OK {
        return 2;
    }
    // 3: 10% contended, briefly held -> HOT, not SATURATED.
    if rqlock_verdict_rs(100_000, 10_000, 1_000, 1_000_000) != RQ_HOT {
        return 3;
    }
    // 4: 4% contended is below the line and must NOT report HOT. A threshold
    //    that fires on everything is not a threshold.
    if rqlock_verdict_rs(100_000, 4_000, 1_000, 1_000_000) != RQ_OK {
        return 4;
    }
    // 5: held 30% of wall time -> SATURATED even with modest contention, because
    //    held-time is the quantity that decides whether splitting can help.
    if rqlock_verdict_rs(100_000, 1_000, 300_000, 1_000_000) != RQ_SATURATED {
        return 5;
    }
    // 6: percentages.
    if rqlock_contended_pct_rs(200, 50) != 25 {
        return 6;
    }
    if rqlock_contended_pct_rs(0, 0) != 0 {
        return 7;
    }
    // 8: held_pct is deliberately NOT clamped at 100.
    if rqlock_held_pct_rs(2_000_000, 1_000_000) != 200 {
        return 8;
    }
    if rqlock_held_pct_rs(1, 0) != 0 {
        return 9;
    }
    // 10: a corrupt counter pair (contended > acquires) must not divide out
    //     above 100 or panic.
    if rqlock_contended_pct_rs(10, 999) != 100 {
        return 10;
    }
    0
}
