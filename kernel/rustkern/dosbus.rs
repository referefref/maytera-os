// rustkern/dosbus.rs - #176: what a guest port access COSTS in emulated time.
//
// New kernel logic, so Rust per the 2026-07-16 rule. The hot path is one
// unsigned add, one shift and one mask per guest IN/OUT; there is no float
// (the kernel is -mno-sse soft-float, which is why the tick arithmetic below is
// a Q32 fixed-point accumulator and not microseconds-as-double), and the cost
// was MEASURED rather than assumed to be too high for C: see the CHANGELOG.
//
// ===========================================================================
// THE DEFECT
// ---------------------------------------------------------------------------
// The DOS interpreter's emulated clock is `retired guest instructions / the
// measured host interpretation rate` (dosexec.c dos_emu_pit_now). Every
// instruction therefore costs the SAME amount of emulated time, and an
// `in al,dx` costs exactly what a `nop` costs.
//
// On the hardware these games were written for that is not close to true. An
// 8-bit ISA I/O cycle is a bus transaction at 8.33 MHz BCLK, not a CPU
// operation, and it does not get faster when the CPU does. That property is
// not incidental: it is WHY the era's delay loops are written as repeated port
// reads. A loop of N reads takes about N microseconds on an 8 MHz AT and about
// N microseconds on a Pentium, so it SELF-CALIBRATES, which a `mov cx,N / loop`
// spin does not.
//
// An interpreter that charges a port read like a nop destroys exactly that
// property, and the delay loops that depend on it come out roughly an order of
// magnitude short. MEASURED on build 17600 (golden 1991 + this worktree's
// parent commit), Commander Keen 5's AdLib probe, with the chip forced present:
//
//     out 4,0x21   start OPL timer 1     pit_now = 102614, deadline 102709
//     142 x in(0x388)                    pit_now = 102614 -> 102636
//     out 4,0x60   give up               pit_now = 102636
//
// 22 PIT ticks is 18.4 us. The chip's own datasheet period, which the game is
// waiting out, is 80 us (95 ticks). The game abandons the probe 61 us early and
// correctly concludes there is no AdLib, because by ITS clock there is not one.
// 142 reads in 18.4 us is 0.130 us per read.
//
// ===========================================================================
// THE NUMBER, AND THE THREE INDEPENDENT ANCHORS IT HAS TO SIT BETWEEN
// ---------------------------------------------------------------------------
// A cost model is only worth having if the constant is defensible, so this one
// is pinned from three directions that were derived separately:
//
//   1. THE BUS. The AT ISA bus runs at 8.33 MHz. A default 8-bit I/O cycle is
//      6 BCLK periods, which is 720 ns, and 8-bit peripherals routinely stretch
//      it further with IOCHRDY. So the floor is 0.72 us and the typical figure
//      quoted for 8-bit ISA I/O is about 1 us.
//
//   2. THE ADLIB MANUAL'S OWN DELAY RECIPE. The AdLib programming guide states
//      the required post-write settling times as 3.3 us after an index write
//      and 23 us after a data write, and tells you to implement them as 6 and
//      35 reads of the status port respectively. For that recipe to work at
//      all, a status read must cost at least 3.3/6 = 0.55 us and at least
//      23/35 = 0.66 us.
//
//   3. THE GUEST IN FRONT OF US. Keen 5 spends 142 reads waiting out 80 us, so
//      its author was relying on a read costing at least 80/142 = 0.563 us.
//
// All three land in 0.55 .. 1.0 us. `DOS_BUS_NS_DEFAULT` is 1000 ns: the
// standard 8-bit ISA figure, above every one of the three lower bounds, and
// it gives Keen's probe 1.8x margin rather than landing it on the boundary
// where host jitter decides the outcome.
//
// WHAT THIS IS NOT. It is not a per-instruction cost model and it is not a step
// towards one. Every other instruction still costs 1/rate seconds. The claim
// being made is narrow and is the only one the evidence supports: PORT I/O IS
// NOT A CPU OPERATION AND MUST NOT BE PRICED AS ONE.
//
// ===========================================================================
// WHY IT IS SAFE TO CHARGE, WHICH IS A MEASUREMENT AND NOT AN ARGUMENT
// ---------------------------------------------------------------------------
// Charging emulated time for I/O makes the guest's clock run FASTER relative to
// the host than its instruction count alone would. That is correct (on real
// hardware the PIT really does advance during a bus cycle), but it is only
// harmless if guests do not do enough I/O for the inflation to be visible.
//
// The whole risk therefore reduces to ONE number per title: port accesses per
// second. Emulated time already advances at about PIT_HZ (1193182) ticks per
// real second by construction, so charging C microseconds per access inflates
// the guest's clock by a factor of
//
//     1 + (accesses_per_second * C_us) / 1e6
//
// which is 1% at 10k accesses/s and 50% at 500k accesses/s with C = 1 us. That
// is what `[IOCOST]` in dosexec.c prints, per title, in both arms, so the
// decision is read off a log rather than argued. See the CHANGELOG for the
// corpus table.
// ===========================================================================

/// The 8253/8254 input clock, the unit of the emulated timebase. Same constant
/// as DOS_PIT_HZ in dos/dosexec.c; both are the hardware oscillator and neither
/// is free to drift from the other.
const PIT_HZ: u64 = 1_193_182;

/// The shipped bus cost, in nanoseconds per 8-bit port access. See the three
/// anchors above. /CONFIG/DOSBUS.CFG overrides it (0 = charge nothing, i.e. the
/// pre-#176 behaviour), which is how both arms are measured on one binary.
pub const DOS_BUS_NS_DEFAULT: u32 = 1000;

/// Refuse a nonsense override rather than silently producing a guest whose
/// clock runs 1000x fast. 20 us is already far beyond any real ISA cycle and
/// leaves room to run a deliberate over-charge arm as an experiment.
const DOS_BUS_NS_MAX: u32 = 20_000;

/// Per-guest bus-cost state. Layout-locked from C (dos/dosexec.c) because a
/// silent drift here would not crash: it would corrupt the emulated clock,
/// which is the exact shape of failure this module exists to fix.
#[repr(C)]
pub struct DosBus {
    /// Resolved cost per access, nanoseconds. 0 means charge nothing.
    pub ns_per_access: u32,
    /// 1 once configured, so a report can tell "no I/O yet" from "never armed".
    pub armed: u32,
    /// Derived from ns_per_access once, so the hot path never divides.
    /// PIT ticks per access in Q32 fixed point.
    pub q32_per_access: u64,
    /// Sub-tick remainder carried between accesses, Q32. Carrying it is the
    /// difference between an accumulator that is exact over millions of
    /// accesses and one that loses a tick every time the cost is not a whole
    /// number of ticks, which it never is (1 us = 1.193182 ticks).
    pub frac_q32: u64,
    /// Census. Every guest port access, decoded or not, IN or OUT.
    pub n_access: u64,
    /// Whole PIT ticks handed to the caller so far.
    pub ticks_charged: u64,
}

fn q32_for(ns: u32) -> u64 {
    // ticks = ns * PIT_HZ / 1e9, in Q32. u128 because (1000 * 1193182) << 32
    // is 5.1e18, which fits in u64 only by luck and stops fitting at ns > 3500.
    // This runs once per guest launch, not per access.
    ((((ns as u128) * (PIT_HZ as u128)) << 32) / 1_000_000_000u128) as u64
}

/// Configure the bus for one guest. `ns` is the resolved cost; the caller has
/// already applied /CONFIG/DOSBUS.CFG if it was present.
#[no_mangle]
pub extern "C" fn dos_bus_init_rs(b: *mut DosBus, ns: u32) {
    if b.is_null() {
        return;
    }
    let ns = if ns > DOS_BUS_NS_MAX { DOS_BUS_NS_MAX } else { ns };
    // SAFETY: caller passes &t->bus, which lives as long as the task.
    let c = unsafe { &mut *b };
    c.ns_per_access = ns;
    c.armed = 1;
    c.q32_per_access = q32_for(ns);
    c.frac_q32 = 0;
    c.n_access = 0;
    c.ticks_charged = 0;
}

/// ONE guest port access. Returns the number of whole PIT ticks of emulated
/// time it cost, which the caller adds to the guest's clock.
///
/// The access is COUNTED even when the cost is zero, deliberately: the census
/// is what makes the off arm measurable, and a counter that only moves in the
/// arm being advocated for is the shape of diagnostic #175 had to fix.
#[no_mangle]
pub extern "C" fn dos_bus_charge_rs(b: *mut DosBus) -> u64 {
    if b.is_null() {
        return 0;
    }
    // SAFETY: as above.
    let c = unsafe { &mut *b };
    c.n_access = c.n_access.wrapping_add(1);
    if c.q32_per_access == 0 {
        return 0;
    }
    c.frac_q32 = c.frac_q32.wrapping_add(c.q32_per_access);
    let whole = c.frac_q32 >> 32;
    c.frac_q32 &= 0xFFFF_FFFF;
    c.ticks_charged = c.ticks_charged.wrapping_add(whole);
    whole
}

/// (#252) Charge an EXPLICIT interval of emulated time, in MICROSECONDS,
/// through the SAME accumulator a port access uses. Returns the whole PIT
/// ticks the caller must add to the guest's clock.
///
/// WHY THIS LIVES HERE RATHER THAN IN A COUNTER OF ITS OWN. #176 established
/// the invariant that ALL non-instruction emulated time is charged in ONE
/// place, so that dos_emu_clock_rate() can take it OUT of the same real second
/// instead of adding it on top. A BIOS wait (INT 15h AH=86h) is exactly that
/// kind of time: the guest asked for N microseconds to pass with no
/// instructions retired. A private accumulator for it would advance the guest's
/// clock without ever appearing in `ticks_charged`, the rate correction would
/// not see it, and the guest's clock would outrun real time by the whole of
/// every wait. That is the precise defect #176 fixed for the bus, and adding a
/// second uncounted time source would reintroduce it by a different door.
///
/// IT DOES NOT TOUCH `n_access`, deliberately. That census exists to price the
/// ISA bus and is read as accesses-per-second; a BIOS wait is not a port
/// access, and folding it in would corrupt the one number #176's safety
/// argument rests on.
///
/// IT IS NOT GATED ON `q32_per_access`. A machine configured with a zero bus
/// cost (/CONFIG/DOSBUS.CFG=0, the #176 control arm) still has a BIOS that
/// waits when a guest asks it to: the two are independent policies and only the
/// bus one is an emulation-fidelity knob.
#[no_mangle]
pub extern "C" fn dos_bus_charge_us_rs(b: *mut DosBus, us: u32) -> u64 {
    if b.is_null() || us == 0 {
        return 0;
    }
    // SAFETY: caller passes &t->bus, which lives as long as the task.
    let c = unsafe { &mut *b };
    // ticks = us * PIT_HZ / 1e6, in Q32. u128 because at us = u32::MAX the
    // product before the shift is 5.1e15 and after it 2.2e28.
    let q32 = (((us as u128) * (PIT_HZ as u128)) << 32) / 1_000_000u128;
    // Split into whole ticks and a Q32 remainder, then run the remainder
    // through the SAME carry `frac_q32` the per-access path uses, so a long run
    // of sub-tick waits accumulates exactly instead of truncating each one to
    // zero. A 3.3 us AdLib index delay is 3.94 ticks; a 1 us one is 1.19.
    let mut whole = (q32 >> 32) as u64;
    c.frac_q32 = c.frac_q32.wrapping_add((q32 & 0xFFFF_FFFF) as u64);
    whole += c.frac_q32 >> 32;
    c.frac_q32 &= 0xFFFF_FFFF;
    c.ticks_charged = c.ticks_charged.wrapping_add(whole);
    whole
}

// ---------------------------------------------------------------------------
// Self-test. Reported at boot beside the #172 PIT one and the #175 OPL2 one.
//
// EVERY CASE ASSERTS BOTH DIRECTIONS. A delay-loop test that only ever shows
// the loop being long enough would score a pass for a cost of 1 ns, so the
// under-cost case is checked as explicitly as the shipped one.
// ---------------------------------------------------------------------------
fn tot(ns: u32, n: u32) -> u64 {
    let mut b = DosBus {
        ns_per_access: 0,
        armed: 0,
        q32_per_access: 0,
        frac_q32: 0,
        n_access: 0,
        ticks_charged: 0,
    };
    dos_bus_init_rs(&mut b, ns);
    let mut acc = 0u64;
    for _ in 0..n {
        acc += dos_bus_charge_rs(&mut b);
    }
    // The accumulator's own total and the caller's must agree, or one of the
    // two is lying about how much time the guest was given.
    if acc != b.ticks_charged {
        return u64::MAX;
    }
    acc
}

#[no_mangle]
pub extern "C" fn dos_bus_selftest_rs() -> u32 {
    let mut fail = 0u32;

    // 1. NO FIXED-POINT DRIFT. One million accesses at 1 us is one second of
    //    bus time, which is exactly PIT_HZ ticks. Truncating the per-access
    //    cost to a whole tick instead of carrying the remainder would give
    //    1_000_000 here, i.e. 16% short, so this is the case that would catch
    //    the obvious wrong implementation.
    let t = tot(1000, 1_000_000);
    if t != PIT_HZ && t != PIT_HZ - 1 {
        fail += 1;
    }

    // 2. OFF IS OFF, and still counts. ns = 0 must charge nothing at all.
    let mut b = DosBus {
        ns_per_access: 0,
        armed: 0,
        q32_per_access: 0,
        frac_q32: 0,
        n_access: 0,
        ticks_charged: 0,
    };
    dos_bus_init_rs(&mut b, 0);
    let mut acc = 0u64;
    for _ in 0..1000 {
        acc += dos_bus_charge_rs(&mut b);
    }
    if acc != 0 || b.n_access != 1000 {
        fail += 1;
    }

    // 3. THE TICKET'S OWN CRITERION, as an assertion rather than a hope.
    //    Keen 5 waits out the OPL2's 80 us timer 1 period (95 PIT ticks) with
    //    142 status reads, and the interpreter's instruction time contributes
    //    22 ticks of that. At the shipped cost the loop must CLEAR 95.
    const KEEN_READS: u32 = 142;
    const KEEN_INSN_TICKS: u64 = 22;
    const OPL_80US_TICKS: u64 = 95;
    if tot(DOS_BUS_NS_DEFAULT, KEEN_READS) + KEEN_INSN_TICKS <= OPL_80US_TICKS {
        fail += 1;
    }

    // 4. AND THE SAME LOOP MUST STILL FAIL WHEN THE BUS IS FREE. This is the
    //    RED arm of case 3: without it, case 3 passes for any cost including a
    //    absurdly large one and proves only that addition works.
    if tot(0, KEEN_READS) + KEEN_INSN_TICKS > OPL_80US_TICKS {
        fail += 1;
    }

    // 5. AND IT MUST STILL FAIL AT A COST BELOW THE ANCHORS. 400 ns is under
    //    all three lower bounds in the header, and 142 reads at 400 ns is
    //    56.8 us, which with the 22 ticks of instruction time is 90 ticks:
    //    short of 95. So the test discriminates between a defensible constant
    //    and a too-small one, not merely between zero and non-zero.
    if tot(400, KEEN_READS) + KEEN_INSN_TICKS > OPL_80US_TICKS {
        fail += 1;
    }

    // 6. THE CLAMP IS REAL. An absurd override must be refused, not adopted.
    let mut c = DosBus {
        ns_per_access: 0,
        armed: 0,
        q32_per_access: 0,
        frac_q32: 0,
        n_access: 0,
        ticks_charged: 0,
    };
    dos_bus_init_rs(&mut c, 10_000_000);
    if c.ns_per_access != DOS_BUS_NS_MAX {
        fail += 1;
    }

    // 7. MONOTONIC AND PROPORTIONAL. Twice the cost is twice the ticks over the
    //    same access count, within the one-tick rounding the carry allows.
    let a = tot(1000, 10_000);
    let d = tot(2000, 10_000);
    if d < a * 2 - 2 || d > a * 2 + 2 {
        fail += 1;
    }

    // ---------------------------------------------------------------------
    // (#252) dos_bus_charge_us_rs. Every case asserts BOTH directions, because
    // the whole point of this function is that a delay which is too SHORT is
    // indistinguishable from a working one until a guest measures it.
    // ---------------------------------------------------------------------
    let mut w = DosBus {
        ns_per_access: 0,
        armed: 0,
        q32_per_access: 0,
        frac_q32: 0,
        n_access: 0,
        ticks_charged: 0,
    };
    dos_bus_init_rs(&mut w, 0); // bus cost OFF: the wait must work anyway

    // 8. THE NUMBER THE WHOLE TICKET TURNS ON. The AdLib datasheet period the
    //    detection protocol waits out is 80 us, which is 95 PIT ticks
    //    (80 * 1193182 / 1e6 = 95.45). A guest that arms OPL timer 1 and then
    //    asks the BIOS for 80 us must come back on the far side of its own
    //    deadline, so anything BELOW 95 is the #175/#176 failure again.
    let t80 = dos_bus_charge_us_rs(&mut w, 80);
    if t80 != 95 {
        fail += 1;
    }

    // 9. ZERO IS ZERO, and a null pointer charges nothing rather than faulting.
    if dos_bus_charge_us_rs(&mut w, 0) != 0 {
        fail += 1;
    }
    if dos_bus_charge_us_rs(core::ptr::null_mut(), 80) != 0 {
        fail += 1;
    }

    // 10. THE CARRY IS EXACT OVER A LONG RUN OF SUB-TICK WAITS. This is the
    //     RED arm for "truncate each wait to whole ticks": 1 us is 1.193182
    //     ticks, so 1000 of them are 1193 ticks and a truncating implementation
    //     returns 1000. Anything at or below 1000 has thrown the fraction away.
    let mut y = DosBus {
        ns_per_access: 0,
        armed: 0,
        q32_per_access: 0,
        frac_q32: 0,
        n_access: 0,
        ticks_charged: 0,
    };
    dos_bus_init_rs(&mut y, 0);
    let mut acc = 0u64;
    for _ in 0..1000 {
        acc += dos_bus_charge_us_rs(&mut y, 1);
    }
    if acc != 1193 || acc != y.ticks_charged {
        fail += 1;
    }

    // 11. IT IS COUNTED WHERE THE RATE CORRECTION CAN SEE IT. A wait that does
    //     not land in ticks_charged is a wait dos_emu_clock_rate() cannot take
    //     out of the second, which is the #176 defect wearing a new hat.
    let before = y.ticks_charged;
    let got = dos_bus_charge_us_rs(&mut y, 1_000_000);
    if y.ticks_charged != before + got || got < PIT_HZ - 1 || got > PIT_HZ + 1 {
        fail += 1;
    }

    // 12. IT DOES NOT DISTURB THE PORT-ACCESS CENSUS. n_access prices the ISA
    //     bus and is read as accesses per second; a BIOS wait must not appear
    //     in it.
    if y.n_access != 0 {
        fail += 1;
    }

    fail
}
