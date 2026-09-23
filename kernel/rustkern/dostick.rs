// dostick.rs - (#ravideo) A GUEST'S TIMER-INTERRUPT RATE MUST NOT BE A
//              FUNCTION OF THE HOST'S BURST SIZE.
//
// WHAT WENT WRONG, MEASURED
// -------------------------
// dos/dosexec.c runs a DOS/4GW guest in bursts: `x86_32_run(cpu, slice)`
// interprets up to `slice` instructions and returns, and only BETWEEN bursts
// does dos4gw_timebase() get to deliver the guest's IRQ0. That function
// delivers at most four ticks per pass and then RESYNCS, dropping whatever it
// is still behind by:
//
//     while (now_pit >= t->next_irq0_pit && fired < 4) { ...deliver...; }
//     if (now_pit >= t->next_irq0_pit)
//         t->next_irq0_pit = now_pit + div;      // drop the debt
//
// So the ceiling on the guest's timer rate is 4 x (passes per second). The
// pass rate is a HOST PACING number, and dosexec.c's own header says pacing
// must be invisible to the guest: "Everything the GUEST can observe about time
// is deliberately NOT derived from these". The tick rate is the most
// guest-observable clock there is, and it was.
//
// MEASURED, golden 2363, Red Alert's VQA intro (which is paced on INT 08h),
// Ring-3 DOS host, one throwaway VM per arm, /CONFIG/DOSSPEED.CFG armed. The
// only thing that differs between the arms is /DOS/RA/SPEED.CFG:
//
//   cap        guest insn/s   burst (insns)   passes/s   guest redraw/s
//   uncapped   31-34 M        3,600,000       43-45      1.9-2.4
//   33000      32-33 M        1.4-2.0 M       94-226     5.4-15.4
//   25000      24-25 M        25,000-125,000  553-1058   15.9-19.4
//   17000      17 M           17,000          783-963    21.4-22.4
//
// The uncapped arm is the FASTEST guest and the SLOWEST picture. Halving the
// guest's CPU makes the video nine times faster. Within the 33000 arm, where
// the instruction rate does not move at all, the frame rate tracks the pass
// rate. The reference is the disc itself: every VQA on Red Alert CD1 declares
// 15 fps in its own VQHD header, so the uncapped arm is 6x too slow and the
// owner's report of "about 2-3fps" on real hardware is this, exactly.
//
// THE FIX, AND WHY IT IS THIS ONE
// -------------------------------
// Do not raise the four. Delivering a backlog of late ticks in one burst is
// the "run the game forward all at once" failure the existing comment rightly
// refuses. The right answer is to never GET behind: bound the burst by the
// next tick deadline, so at most one tick is ever owed when the loop comes
// back round, the four never binds, and no debt is ever dropped.
//
// That also makes the guest's tick rate independent of `slice`, which is the
// property that was missing. `slice` may be re-tuned by the host for any
// reason (and on a machine whose native tick has died it is re-tuned to
// 3.6 million instructions, which is how this got as bad as it did); none of
// that can now reach the guest's clock.
//
// COST. At Red Alert's timer rate and ~32 M insn/s the bound works out at tens
// of thousands of instructions per burst, i.e. the ~1000 passes/s regime the
// capped arms above already run in happily. The floor stops a guest that
// programs an absurd divisor from turning this into a per-instruction loop.

/// The 8253/8254 input clock. Same constant as DOS_PIT_HZ in dos/dosexec.c;
/// asserted equal by the caller passing it in rather than by two copies.
const PIT_HZ: u64 = 1_193_182;

/// Mirrored by `dostick_acct_t` in dos/dosexec.c with a _Static_assert on the
/// size, the same contract as dosdisp_state_t and vbe_present_t. C owns the one
/// instance; the arithmetic lives here.
///
/// `owed` is counted BEFORE any delivery or limit logic runs, so it is what the
/// emulated clock says the guest is entitled to. `delivered` is what it got.
/// The gap between the two is the defect, as a number, and it is the number the
/// fix is judged on.
#[repr(C)]
pub struct DosTickAcct {
    pub owed: u64,
    pub delivered: u64,
    pub dropped: u64,
    pub refused: u64,
    pub passes: u64,
    pub bounded: u64,
    pub div: u32,
    pub _pad: u32,
}

#[no_mangle]
pub extern "C" fn dostick_reset_rs(a: *mut DosTickAcct) {
    if a.is_null() {
        return;
    }
    // SAFETY: non-null, checked; the C caller passes the address of its single
    // static dostick_acct_t.
    let s = unsafe { &mut *a };
    s.owed = 0;
    s.delivered = 0;
    s.dropped = 0;
    s.refused = 0;
    s.passes = 0;
    s.bounded = 0;
    s.div = 0;
}

/// One pass of dos4gw_timebase(). `owed` is how many ticks the emulated clock
/// says are due, `delivered` how many actually reached the guest, `refused`
/// non-zero if a delivery was declined (the guest had interrupts off).
#[no_mangle]
pub extern "C" fn dostick_note_rs(a: *mut DosTickAcct, div: u32, owed: u64,
                                  delivered: u64, refused: i32) {
    if a.is_null() {
        return;
    }
    // SAFETY: non-null, checked.
    let s = unsafe { &mut *a };
    s.passes = s.passes.wrapping_add(1);
    s.div = div;
    s.owed = s.owed.wrapping_add(owed);
    s.delivered = s.delivered.wrapping_add(delivered);
    s.dropped = s.dropped.wrapping_add(owed.saturating_sub(delivered));
    if refused != 0 {
        s.refused = s.refused.wrapping_add(1);
    }
}

/// Count a pass whose burst was shortened by the deadline bound, so "is the fix
/// actually engaging" is a number and not an assumption.
#[no_mangle]
pub extern "C" fn dostick_note_bounded_rs(a: *mut DosTickAcct) {
    if a.is_null() {
        return;
    }
    // SAFETY: non-null, checked.
    unsafe { (*a).bounded = (*a).bounded.wrapping_add(1) };
}

/// How many guest instructions may be interpreted before the next IRQ0 falls
/// due, clamped to [floor, ceil].
///
/// `now_pit`/`next_pit` are the emulated PIT counter now and at the next tick,
/// `emu_hz` the measured guest instruction rate. Returns `floor` when a tick is
/// already due or when the inputs are degenerate: running a minimal burst and
/// coming straight back is always correct, and an instrument that returns zero
/// here would stall the guest.
#[no_mangle]
pub extern "C" fn dostick_budget_rs(now_pit: u64, next_pit: u64, emu_hz: u32,
                                    floor: u64, ceil: u64) -> u64 {
    let floor = if floor == 0 { 1 } else { floor };
    let ceil = if ceil < floor { floor } else { ceil };
    if emu_hz == 0 || next_pit <= now_pit {
        return floor;
    }
    let d_pit = next_pit - now_pit;
    // d_pit is bounded by one divisor (65536 at most in normal use) and emu_hz
    // by a few hundred million, so this cannot overflow u64 by any margin that
    // matters; the saturating form says so rather than relying on the reader
    // checking.
    let insns = d_pit.saturating_mul(emu_hz as u64) / PIT_HZ;
    if insns < floor {
        floor
    } else if insns > ceil {
        ceil
    } else {
        insns
    }
}

/// Ticks per second, in TENTHS, from a count and a millisecond interval. In
/// tenths because the interesting readings are small: a guest that is getting
/// 2.4 timer ticks per video frame instead of 15 must not print as "2".
#[no_mangle]
pub extern "C" fn dostick_per_s_x10_rs(n: u64, ms: u64) -> u64 {
    if ms == 0 {
        return 0;
    }
    n.saturating_mul(10_000) / ms
}

/// Self-test: every arm of the budget, including the two that must never return
/// zero. Returns failed checks, 0 = pass. Wired into the same
/// /CONFIG/DOSSPEED.CFG arming as the [DOSFRAME] profile, so this is a test that
/// has been watched go green rather than one that merely compiles.
#[no_mangle]
pub extern "C" fn dostick_selftest_rs() -> i32 {
    let mut bad = 0i32;

    // A tick already due, or exactly due: a minimal burst, never zero.
    if dostick_budget_rs(1000, 1000, 32_000_000, 1024, 4_000_000) != 1024 { bad += 1; }
    if dostick_budget_rs(2000, 1000, 32_000_000, 1024, 4_000_000) != 1024 { bad += 1; }
    // A rate of zero is degenerate and must not divide.
    if dostick_budget_rs(0, 1024, 0, 1024, 4_000_000) != 1024 { bad += 1; }
    // A floor of zero must still not return zero.
    if dostick_budget_rs(1000, 1000, 32_000_000, 0, 4_000_000) != 1 { bad += 1; }

    // Red Alert's case, the one this exists for: a 1024 divisor (1165 Hz) at
    // 32 M insn/s is 1024 * 32e6 / 1193182 = 27,464 instructions to the next
    // tick, which is a burst the loop can afford ~1000 times a second.
    let b = dostick_budget_rs(0, 1024, 32_000_000, 1024, 4_000_000);
    if b < 27_000 || b > 28_000 { bad += 1; }

    // The BIOS divisor, 65536 = 18.2 Hz: 1.76 M instructions, under the
    // ceiling, so an ordinary guest is not slowed down by this at all.
    let b18 = dostick_budget_rs(0, 65536, 32_000_000, 1024, 4_000_000);
    if b18 < 1_700_000 || b18 > 1_800_000 { bad += 1; }
    // ...and with a lower ceiling the ceiling wins.
    if dostick_budget_rs(0, 65536, 32_000_000, 1024, 100_000) != 100_000 { bad += 1; }

    // A pathological divisor of 1 must be caught by the floor, not turned into
    // a 26-instruction burst.
    if dostick_budget_rs(0, 1, 32_000_000, 1024, 4_000_000) != 1024 { bad += 1; }

    // The accounting: owed beyond delivered is dropped, and it accumulates.
    let mut a = DosTickAcct { owed: 9, delivered: 9, dropped: 9, refused: 9,
                              passes: 9, bounded: 9, div: 9, _pad: 0 };
    dostick_reset_rs(&mut a);
    if a.owed != 0 || a.delivered != 0 || a.dropped != 0 || a.refused != 0
       || a.passes != 0 || a.bounded != 0 || a.div != 0 { bad += 1; }
    dostick_note_rs(&mut a, 1024, 25, 4, 0);
    if a.owed != 25 || a.delivered != 4 || a.dropped != 21 || a.passes != 1 { bad += 1; }
    if a.div != 1024 { bad += 1; }
    dostick_note_rs(&mut a, 1024, 1, 1, 1);
    if a.owed != 26 || a.delivered != 5 || a.dropped != 21 || a.refused != 1 { bad += 1; }
    // delivered > owed (a rounding artefact at a divisor change) must not
    // underflow the dropped count into 18 quintillion.
    dostick_note_rs(&mut a, 1024, 0, 2, 0);
    if a.dropped != 21 { bad += 1; }
    dostick_note_bounded_rs(&mut a);
    if a.bounded != 1 { bad += 1; }

    // The rate helper, in tenths.
    if dostick_per_s_x10_rs(0, 0) != 0 { bad += 1; }
    if dostick_per_s_x10_rs(2330, 2000) != 11650 { bad += 1; }   // 1165.0/s

    // Null must not fault and must not be silently mistaken for success.
    dostick_reset_rs(core::ptr::null_mut());
    dostick_note_rs(core::ptr::null_mut(), 0, 1, 1, 0);
    dostick_note_bounded_rs(core::ptr::null_mut());

    bad
}
