// bklstat.rs - #166: the BKL's per-window statistics, and the invariants that
// say when they are lying.
//
// WHY THIS EXISTS. sched_smp_report()'s BKL line printed
// `held=18446743107341936826us` (a 64-bit value 966,367,614,790 BELOW zero) and
// `contended` larger than `acquires`, which is impossible by construction since
// every contention is a subset of an acquisition. #143 had already established
// by measurement that the BKL, not the run-queue lock, is the scaling ceiling,
// so this line is the instrument anyone narrowing the BKL will reach for to
// justify and verify the work. An instrument that produces garbage makes every
// before/after number meaningless, which is why this was fixed first.
//
// WHAT WAS ACTUALLY WRONG is in cpu/smp.c, not here: the per-CPU counter arrays
// were sized 8 while the reader's loop bound came from MAYTERA_MAX_CPUS = 32,
// so on a 12-vCPU boot the summation read six arrays out of bounds and zeroed
// part of a seventh. This module is the second half of the fix: the window
// arithmetic and the invariant checking, moved out of the report and into a
// place where it is stated once, tested, and CANNOT SILENTLY PASS.
//
// THE ONE THING THIS MODULE MUST NEVER DO IS CLAMP. A clamped display of a
// broken counter is strictly worse than an obviously broken one, because it
// looks trustworthy and the next person builds on it. Every delta below is
// returned RAW, exactly as computed; when an invariant is violated the caller
// is handed a flag word and prints a loud line NEXT TO the raw numbers. Note
// that rqlock.rs's percentage helpers do clamp `contended` to `acquires` - that
// is a display guard on a lock whose counters are currently healthy, and if
// they ever invert it will show 100% and say nothing. Do not copy that here.
//
// AND IT MUST NOTICE A DEAD COUNTER. A counter stuck at zero satisfies
// `contended <= acquires`, `held did not go backwards`, and every other
// invariant in this file. Replacing a visibly broken instrument with an
// invisibly broken one is the failure mode this project keeps paying for
// (#83 running_cpu, #69 net_lock holdmax, #167 CANDIDATE 2). So "nothing moved"
// is itself a reported condition: BKLSTAT_STALLED.
//
// WHY RUST. Per the 2026-07-16 rule, new kernel code is Rust unless there is a
// stated performance or entanglement reason. The counter STORES stay in C: they
// sit inside bkl_take_locked() with interrupts masked, between inline-asm cli /
// CAS / sti, on every kernel entry on every core - the same hot, asm-entangled
// path #83 kept in C. This half runs ONCE per ~4-second reporting window, where
// the cost is irrelevant and the arithmetic is the thing that was wrong.
//
// NO FLOAT: the kernel target is soft-float with SSE disabled, so every ratio
// here is integer arithmetic, as in rqlock.rs and schedwatch.rs.

use core::sync::atomic::{AtomicU64, Ordering};

// ---------------------------------------------------------------- flag word
/// Everything checked out.
pub const BKLSTAT_OK: u32 = 0;
/// A cumulative total went BACKWARDS between windows. Cumulative counters are
/// monotonic by construction, so this can only mean the reader and the writer
/// disagree about the array, or a counter was reset underneath the reader.
pub const BKLSTAT_ACQ_BACKWARD: u32 = 1 << 0;
pub const BKLSTAT_CON_BACKWARD: u32 = 1 << 1;
pub const BKLSTAT_HELD_BACKWARD: u32 = 1 << 2;
pub const BKLSTAT_SPIN_BACKWARD: u32 = 1 << 3;
pub const BKLSTAT_LONG_BACKWARD: u32 = 1 << 4;
pub const BKLSTAT_REC_BACKWARD: u32 = 1 << 5;
/// More contentions than acquisitions. Every contention is part of an
/// acquisition, so this is impossible unless the two are counted in different
/// places - which is exactly what #166 found.
pub const BKLSTAT_CON_GT_ACQ: u32 = 1 << 6;
/// A single hold longer than the whole reporting window it was measured in.
pub const BKLSTAT_MAX_GT_WINDOW: u32 = 1 << 7;
/// Total hold time exceeds what the online cores could physically have held in
/// the window (cores * window). held_us is a SUM over cores, so it may exceed
/// one window; it may not exceed all of them.
pub const BKLSTAT_HELD_GT_CAPACITY: u32 = 1 << 8;
/// More over-1ms holds than acquisitions.
pub const BKLSTAT_LONG_GT_ACQ: u32 = 1 << 9;
/// NOTHING moved in a window on a machine whose BKL is engaged. Consistent, and
/// therefore the most dangerous of these: a dead instrument passes every other
/// check in this file.
pub const BKLSTAT_STALLED: u32 = 1 << 10;

/// Mirrors `bkl_totals_t` in cpu/smp.h. Locked by a `_Static_assert` on the C
/// side; do not reorder either without the other.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct BklTotals {
    pub acquires: u64,
    pub contended: u64,
    pub recursive: u64,
    pub spins: u64,
    pub held_us: u64,
    pub long_holds: u64,
    pub max_us: u64,
    pub ncpu: u32,
    pub max_cpu: u32,
    pub max_reason: u32,
    pub max_from_switch: u32,
}

/// One reporting window's DELTAS, plus the verdict. Mirrors `bkl_window_t`.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct BklWindow {
    pub acquires: u64,
    pub contended: u64,
    pub recursive: u64,
    pub spins: u64,
    pub held_us: u64,
    pub long_holds: u64,
    pub max_us: u64,
    pub flags: u32,
    pub ncpu: u32,
    pub max_cpu: u32,
    pub max_reason: u32,
    pub max_from_switch: u32,
    pub _pad: u32,
}

// Previous window's cumulative totals. One core runs the report
// (sched_tick() calls it only when sched_rq_cpu() == 0), so these need
// consistency, not contention; Relaxed is the right ordering and the wrong
// answer here would be a lock on the report path.
static P_ACQ: AtomicU64 = AtomicU64::new(0);
static P_CON: AtomicU64 = AtomicU64::new(0);
static P_REC: AtomicU64 = AtomicU64::new(0);
static P_SPIN: AtomicU64 = AtomicU64::new(0);
static P_HELD: AtomicU64 = AtomicU64::new(0);
static P_LONG: AtomicU64 = AtomicU64::new(0);
static SEEN: AtomicU64 = AtomicU64::new(0);
/// Windows in which at least one invariant failed, since boot. Printed, so a
/// single bad window early in a boot cannot be missed by looking at the tail.
static BAD: AtomicU64 = AtomicU64::new(0);

/// One delta with its own backwards flag. RAW: `wrapping_sub` reproduces
/// exactly what the C code used to print, so a regression looks identical to
/// the original bug report instead of being quietly smoothed away.
#[inline]
fn delta(cur: u64, prev: u64, flag: u32, flags: &mut u32) -> u64 {
    if cur < prev {
        *flags |= flag;
    }
    cur.wrapping_sub(prev)
}

/// The whole judgement, as a pure function of (previous, current, window), so
/// the self-test can drive it without touching the statics.
///
/// `window_us`    wall-clock length of the reporting window (0 = unknown, the
///                duration-based checks are then skipped rather than guessed)
/// `ncpu_online`  cores that could have held the lock during the window
/// `armed`        the BKL is actually engaged (g_smp_bkl_full). With the giant
///                lock disengaged nobody takes it, so zero movement is correct
///                and STALLED must not fire.
pub fn window_calc(prev: &BklTotals, cur: &BklTotals, window_us: u64,
                   ncpu_online: u32, armed: bool, first: bool) -> BklWindow {
    let mut f: u32 = 0;
    let mut w = BklWindow::default();
    w.acquires   = delta(cur.acquires,   prev.acquires,   BKLSTAT_ACQ_BACKWARD,  &mut f);
    w.contended  = delta(cur.contended,  prev.contended,  BKLSTAT_CON_BACKWARD,  &mut f);
    w.recursive  = delta(cur.recursive,  prev.recursive,  BKLSTAT_REC_BACKWARD,  &mut f);
    w.spins      = delta(cur.spins,      prev.spins,      BKLSTAT_SPIN_BACKWARD, &mut f);
    w.held_us    = delta(cur.held_us,    prev.held_us,    BKLSTAT_HELD_BACKWARD, &mut f);
    w.long_holds = delta(cur.long_holds, prev.long_holds, BKLSTAT_LONG_BACKWARD, &mut f);
    w.max_us          = cur.max_us;          // already a per-window maximum
    w.ncpu            = cur.ncpu;
    w.max_cpu         = cur.max_cpu;
    w.max_reason      = cur.max_reason;
    w.max_from_switch = cur.max_from_switch;

    if w.contended > w.acquires {
        f |= BKLSTAT_CON_GT_ACQ;
    }
    if w.long_holds > w.acquires {
        f |= BKLSTAT_LONG_GT_ACQ;
    }
    if window_us > 0 && w.max_us > window_us {
        f |= BKLSTAT_MAX_GT_WINDOW;
    }
    if window_us > 0 && ncpu_online > 0 {
        // Saturating: capacity overflowing u64 would need a window of weeks.
        let cap = window_us.saturating_mul(ncpu_online as u64);
        if w.held_us > cap {
            f |= BKLSTAT_HELD_GT_CAPACITY;
        }
    }
    // The first window has no previous sample to difference against; its deltas
    // are whole-boot totals, which is information rather than a fault, so the
    // monotonicity and stall verdicts are withheld rather than fabricated.
    if first {
        f &= !(BKLSTAT_ACQ_BACKWARD | BKLSTAT_CON_BACKWARD | BKLSTAT_REC_BACKWARD
               | BKLSTAT_SPIN_BACKWARD | BKLSTAT_HELD_BACKWARD | BKLSTAT_LONG_BACKWARD
               | BKLSTAT_STALLED);
    } else if armed && w.acquires == 0 && w.recursive == 0 && w.held_us == 0 {
        // The timer ISR takes the BKL on every tick and this report is CALLED
        // from the timer ISR, so on an armed kernel a window with no movement
        // at all cannot be a quiet machine: it is a dead counter.
        f |= BKLSTAT_STALLED;
    }
    w.flags = f;
    w
}

/// Snapshot-to-window for the live report. Stores the current totals as the
/// next window's baseline and returns the flag word (also written into `out`).
///
/// # Safety
/// `cur` and `out` are caller-owned and must be valid for one read / one write.
#[no_mangle]
pub extern "C" fn bkl_window_rs(cur: *const BklTotals, window_us: u64,
                                ncpu_online: u32, armed: i32,
                                out: *mut BklWindow) -> u32 {
    if cur.is_null() || out.is_null() {
        return 0;
    }
    let c = unsafe { *cur };
    let first = SEEN.swap(1, Ordering::Relaxed) == 0;
    let prev = BklTotals {
        acquires: P_ACQ.load(Ordering::Relaxed),
        contended: P_CON.load(Ordering::Relaxed),
        recursive: P_REC.load(Ordering::Relaxed),
        spins: P_SPIN.load(Ordering::Relaxed),
        held_us: P_HELD.load(Ordering::Relaxed),
        long_holds: P_LONG.load(Ordering::Relaxed),
        ..BklTotals::default()
    };
    let w = window_calc(&prev, &c, window_us, ncpu_online, armed != 0, first);
    P_ACQ.store(c.acquires, Ordering::Relaxed);
    P_CON.store(c.contended, Ordering::Relaxed);
    P_REC.store(c.recursive, Ordering::Relaxed);
    P_SPIN.store(c.spins, Ordering::Relaxed);
    P_HELD.store(c.held_us, Ordering::Relaxed);
    P_LONG.store(c.long_holds, Ordering::Relaxed);
    if w.flags != 0 {
        BAD.fetch_add(1, Ordering::Relaxed);
    }
    unsafe { *out = w };
    w.flags
}

/// How many windows since boot failed at least one invariant. Printed on every
/// line: a single bad window early in a long boot is invisible if you only read
/// the tail, and this ticket exists because exactly that kind of reading was
/// trusted.
#[no_mangle]
pub extern "C" fn bkl_window_bad_rs() -> u64 {
    BAD.load(Ordering::Relaxed)
}

/// RED/GREEN self-test. Returns 0, or the number of the first failing case.
/// "It compiled" is not evidence that an invariant checker checks invariants,
/// and this file's whole purpose is to be believed.
#[no_mangle]
pub extern "C" fn bkl_stat_selftest_rs() -> u32 {
    let z = BklTotals::default();
    let mk = |a: u64, c: u64, r: u64, s: u64, h: u64, l: u64, m: u64| BklTotals {
        acquires: a, contended: c, recursive: r, spins: s,
        held_us: h, long_holds: l, max_us: m,
        ncpu: 32, max_cpu: 0, max_reason: 0x120, max_from_switch: 0,
    };
    // 1: a healthy window is clean and the deltas are the differences.
    let p = mk(1000, 100, 50, 900, 5_000, 3, 40);
    let c = mk(2000, 300, 90, 9_000, 9_000, 5, 40);
    let w = window_calc(&p, &c, 4_000_000, 4, true, false);
    if w.flags != BKLSTAT_OK { return 1; }
    if w.acquires != 1000 || w.contended != 200 || w.held_us != 4_000 { return 2; }

    // 3: THE ORIGINAL BUG, both halves, must be caught. held goes backwards by
    //    exactly the reported amount and contended exceeds acquires.
    let p = mk(1000, 100, 0, 0, 1_000_000_000_000, 0, 0);
    let c = mk(1100, 5000, 0, 0, 1_000_000_000_000 - 966_367_614_790, 0, 0);
    let w = window_calc(&p, &c, 4_000_000, 12, true, false);
    if w.flags & BKLSTAT_HELD_BACKWARD == 0 { return 3; }
    if w.flags & BKLSTAT_CON_GT_ACQ == 0 { return 4; }
    // and the value is reported RAW, not clamped: a regression must look
    // exactly like the original bug report.
    if w.held_us != 18_446_743_107_341_936_826u64 { return 5; }
    if w.contended != 4900 { return 6; }

    // 7: a single hold longer than the window it was measured in.
    let p = mk(10, 0, 0, 0, 0, 0, 0);
    let c = mk(20, 0, 0, 0, 100, 0, 143_860_845);
    let w = window_calc(&p, &c, 4_000_000, 12, true, false);
    if w.flags & BKLSTAT_MAX_GT_WINDOW == 0 { return 7; }

    // 8: total hold beyond what the online cores could have held.
    let p = mk(10, 0, 0, 0, 0, 0, 0);
    let c = mk(20, 0, 0, 0, 4_000_000 * 4 + 1, 0, 0);
    let w = window_calc(&p, &c, 4_000_000, 4, true, false);
    if w.flags & BKLSTAT_HELD_GT_CAPACITY == 0 { return 8; }
    // 9: ... and exactly at capacity must NOT fire. A check that fires on
    //    everything is not a check.
    let c = mk(20, 0, 0, 0, 4_000_000 * 4, 0, 0);
    let w = window_calc(&p, &c, 4_000_000, 4, true, false);
    if w.flags & BKLSTAT_HELD_GT_CAPACITY != 0 { return 9; }

    // 10: more over-1ms holds than acquisitions (the `long=3878413` shape).
    let p = mk(10, 0, 0, 0, 0, 0, 0);
    let c = mk(20, 0, 0, 0, 0, 3_878_413, 0);
    let w = window_calc(&p, &c, 4_000_000, 12, true, false);
    if w.flags & BKLSTAT_LONG_GT_ACQ == 0 { return 10; }

    // 11: A DEAD COUNTER IS NOT A HEALTHY ONE. Zero movement on an armed
    //     kernel satisfies every other invariant in this file and must still
    //     be reported.
    let p = mk(1000, 100, 50, 900, 5_000, 3, 0);
    let w = window_calc(&p, &p, 4_000_000, 4, true, false);
    if w.flags & BKLSTAT_STALLED == 0 { return 11; }
    // 12: ... but not when the giant lock is disengaged, where zero is correct.
    let w = window_calc(&p, &p, 4_000_000, 4, false, false);
    if w.flags != BKLSTAT_OK { return 12; }
    // 13: ... and not on the first window, which has no baseline.
    let w = window_calc(&z, &z, 4_000_000, 4, true, true);
    if w.flags != BKLSTAT_OK { return 13; }

    // 14: spins going backwards is its own flag (the report used to ZERO
    //     g_bkl_spin_pc[0..3] through an out-of-bounds g_bkl_hold_max write,
    //     and printed the underflow as `bkl=1002/1002c/18446744073709550388s`).
    let p = mk(10, 0, 0, 1229, 0, 0, 0);
    let c = mk(20, 0, 0, 1, 0, 0, 0);
    let w = window_calc(&p, &c, 4_000_000, 12, true, false);
    if w.flags & BKLSTAT_SPIN_BACKWARD == 0 { return 14; }
    if w.spins != 18_446_744_073_709_550_388u64 { return 15; }

    // 16: window_us == 0 means "unknown", and an unknown must not be turned
    //     into a verdict either way.
    let p = mk(10, 0, 0, 0, 0, 0, 0);
    let c = mk(20, 0, 0, 0, 999_999_999, 0, 999_999_999);
    let w = window_calc(&p, &c, 0, 4, true, false);
    if w.flags != BKLSTAT_OK { return 16; }
    0
}
