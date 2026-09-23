// rustkern/tickack.rs - #tickdead: WHERE AN INTERRUPT IS ACKNOWLEDGED, and how
// to tell a tick that was never DELIVERED from one this kernel refused to
// ACKNOWLEDGE.
//
// THE DEFECT THIS EXISTS FOR
// --------------------------
// cpu/idt.c isr_handler() acquires the Big Kernel Lock BEFORE it dispatches to
// any registered handler. Every handler that owns an End Of Interrupt therefore
// sends that EOI on the far side of an unbounded lock wait. For an ordinary
// device that is merely latency. For the TIMER vectors it is self-destroying,
// because the acknowledgement is the thing that permits the NEXT tick:
//
//   * 8259, vector 32 (IRQ0). The in-service bit is set at delivery and the
//     8259 will not deliver IRQ0 again until it is cleared. cpu/isr.c
//     timer_handler() clears it, and timer_handler() is downstream of
//     bkl_acquire(). So while this core waits for the BKL the 250 Hz clock is
//     STOPPED, and if that frame is then context-switched away from - a nested
//     Local APIC tick reaching sched_schedule() - the un-acknowledged frame is
//     parked on a task stack and the clock stays stopped until that exact task
//     runs again. If the task dies first, it never restarts.
//
//   * Local APIC, vectors 0x41 (the #62 redundant tick) and 0x42 (the #169 AP
//     preemption tick). Same shape: an in-service vector blocks the next one of
//     equal or lower priority, and lapic_eoi() sits below the same lock. So the
//     REDUNDANT clock is gated behind the very contention it exists to survive.
//
// This is the same family as cpu/idt.asm irq_smp_tlb and irq_bkl_wake, both of
// which already refuse to route through isr_common for exactly this reason. The
// tick vectors needed the same treatment and did not have it.
//
// WHAT WENT WRONG IN THE DIAGNOSIS, AND WHY THIS MODULE OWNS THE VERDICT
// ---------------------------------------------------------------------
// cpu/tickwatch.c already detects the dead clock and already attributes it, from
// the shape of the 8259 registers alone:
//
//   "IRQ0 is UNMASKED but its IN-SERVICE bit is STUCK SET ... This is a LOCKING
//    bug, not a timer or routing bug."
//
// That is correct as far as it goes and it is not enough to act on, because the
// register shape is IDENTICAL for two completely different faults: a handler
// that is still waiting for a lock, and a handler that will never run at all.
// The distinguishing evidence is not in any register: it is whether a frame for
// that vector is IN FLIGHT on this kernel right now. One counter separates them,
// and this module owns the rule that reads it.
//
// Integer-only, no floats: the kernel target is soft-float with SSE disabled.

// ---------------------------------------------------------------------------
// Which controller owns a vector's acknowledgement
// ---------------------------------------------------------------------------

/// Not a vector this module acknowledges early. Everything keeps the existing
/// behaviour: its handler sends its own EOI, after the lock.
pub const TICK_ACK_NONE: u32 = 0;
/// 8259 master, IRQ0 (vector 32). Acknowledged with a non-specific EOI.
pub const TICK_ACK_PIC0: u32 = 1;
/// Local APIC delivered (0x41 redundant tick, 0x42 AP preemption tick).
pub const TICK_ACK_LAPIC: u32 = 2;

/// THE SCOPE DECISION, written down rather than implied by an if-chain in C.
///
/// Only the three TIMER vectors are acknowledged before the lock, and the
/// restraint is deliberate. An early EOI says "the controller may deliver this
/// line again", which for a periodic timer is exactly right: the next tick is a
/// new, independent event and losing it costs time itself. For a DEVICE it is
/// not. A level-triggered line whose device has not been serviced yet is still
/// asserted, so acknowledging before the handler runs invites a storm, and the
/// PS/2 and ATA handlers in this tree are not re-entrant. Those vectors keep the
/// old ordering and pay latency instead, which is a cost they can afford because
/// nothing else in the system is paced by them.
#[no_mangle]
pub extern "C" fn tick_ack_kind_rs(vec: u64) -> u32 {
    match vec {
        32 => TICK_ACK_PIC0,
        0x41 | 0x42 => TICK_ACK_LAPIC,
        _ => TICK_ACK_NONE,
    }
}

// ---------------------------------------------------------------------------
// Lateness
// ---------------------------------------------------------------------------

/// Is an entry-to-acknowledge delay LATE, i.e. long enough to have cost a tick?
///
/// One nominal period is the only threshold with a meaning: an acknowledgement
/// that takes longer than the interval between ticks has, by definition,
/// prevented at least one tick from being delivered. A fixed microsecond
/// constant would be wrong at every rate other than the one it was chosen for,
/// and this kernel changes its tick rate at runtime (pit_set_frequency).
#[no_mangle]
pub extern "C" fn tick_ack_is_late_rs(delta_us: u64, hz: u32) -> u32 {
    if hz == 0 {
        return 0;
    }
    let period_us = 1_000_000u64 / (hz as u64);
    if period_us == 0 {
        return 0;
    }
    if delta_us >= period_us { 1 } else { 0 }
}

// ---------------------------------------------------------------------------
// Blame
// ---------------------------------------------------------------------------

/// The window was healthy, or too short to judge. No blame to assign.
pub const TICK_BLAME_NONE: i32 = 0;
/// The tick was DELIVERED and this kernel did not ACKNOWLEDGE it in time. The
/// hardware is innocent; the fault is ours and it is a lock-ordering fault.
pub const TICK_BLAME_BLOCKED: i32 = 1;
/// No frame was in flight and none had been late: the interrupt genuinely did
/// not arrive. Masking, routing, or firmware. A different bug with a different
/// fix, and it looks identical from the desktop.
pub const TICK_BLAME_ABSENT: i32 = 2;

/// Separate the two faults that produce an identical 8259 register shape.
///
/// `dnative`     native ticks counted in the window (total minus synthesised).
/// `dms`         REAL elapsed milliseconds, from mono_ms(). Never from ticks.
/// `hz`          the programmed rate.
/// `inflight`    frames for this vector currently between IDT entry and EOI.
/// `ack_max_us`  longest entry-to-acknowledge delay seen in this window.
///
/// The `inflight` test is the direct evidence and is checked first: a frame that
/// is in flight RIGHT NOW is a delivered interrupt this kernel has not
/// acknowledged, which is not a dead timer under any reading. `ack_max_us` is
/// the retrospective form of the same evidence, for a window in which the frame
/// happened to complete before the poll ran: if any acknowledgement in the
/// window took longer than a tick period, deliveries were being suppressed by
/// this kernel during it.
#[no_mangle]
pub extern "C" fn tick_blame_rs(dnative: u64, dms: u64, hz: u32,
                                inflight: u32, ack_max_us: u64) -> i32 {
    // Match tickwatch::TICK_MIN_WINDOW_MS. A window too short to contain many
    // ticks cannot support any verdict, and "not measured" must never be
    // reported as a fault.
    if hz == 0 || dms < 200 {
        return TICK_BLAME_NONE;
    }
    if dnative != 0 {
        return TICK_BLAME_NONE;
    }
    if inflight > 0 {
        return TICK_BLAME_BLOCKED;
    }
    if tick_ack_is_late_rs(ack_max_us, hz) != 0 {
        return TICK_BLAME_BLOCKED;
    }
    TICK_BLAME_ABSENT
}

// ---------------------------------------------------------------------------
// Self-tests. Run at boot beside tick_watch_selftest_rs(). 0 = all passed.
// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn tick_ack_selftest_rs() -> u32 {
    let mut fails: u32 = 0;
    let mut check = |cond: bool| { if !cond { fails += 1; } };

    // --- scope ---------------------------------------------------------------
    check(tick_ack_kind_rs(32) == TICK_ACK_PIC0);
    check(tick_ack_kind_rs(0x41) == TICK_ACK_LAPIC);
    check(tick_ack_kind_rs(0x42) == TICK_ACK_LAPIC);
    // The device vectors MUST NOT be early-acked. These are the ones whose
    // handlers are not re-entrant (keyboard, mouse, both ATA channels); if
    // somebody widens the match above, this is what stops it silently.
    check(tick_ack_kind_rs(33) == TICK_ACK_NONE);   // IRQ1  PS/2 keyboard
    check(tick_ack_kind_rs(44) == TICK_ACK_NONE);   // IRQ12 PS/2 mouse
    check(tick_ack_kind_rs(46) == TICK_ACK_NONE);   // IRQ14 primary ATA
    check(tick_ack_kind_rs(47) == TICK_ACK_NONE);   // IRQ15 secondary ATA
    check(tick_ack_kind_rs(0x50) == TICK_ACK_NONE); // HDA MSI
    check(tick_ack_kind_rs(128) == TICK_ACK_NONE);  // INT 0x80
    check(tick_ack_kind_rs(14) == TICK_ACK_NONE);   // page fault
    check(tick_ack_kind_rs(0xF2) == TICK_ACK_NONE); // TLB IPI: own stub already

    // --- lateness ------------------------------------------------------------
    // 250 Hz: one period is 4000 us.
    check(tick_ack_is_late_rs(3999, 250) == 0);
    check(tick_ack_is_late_rs(4000, 250) == 1);
    check(tick_ack_is_late_rs(0, 250) == 0);
    // The threshold must FOLLOW the rate, which is the whole reason it is not a
    // constant: 4000 us is late at 250 Hz and perfectly normal at 100 Hz.
    check(tick_ack_is_late_rs(4000, 100) == 0);
    check(tick_ack_is_late_rs(10_000, 100) == 1);
    check(tick_ack_is_late_rs(1_000_000, 0) == 0);

    // --- blame ---------------------------------------------------------------
    // Healthy window: no blame, whatever the other evidence says.
    check(tick_blame_rs(500, 2000, 250, 1, 999_999) == TICK_BLAME_NONE);
    // Window too short to judge: no blame, NOT a fault.
    check(tick_blame_rs(0, 10, 250, 0, 0) == TICK_BLAME_NONE);
    // Zero ticks with a frame in flight: WE are holding the acknowledgement.
    check(tick_blame_rs(0, 2000, 250, 1, 0) == TICK_BLAME_BLOCKED);
    // Zero ticks, no frame in flight now, but an acknowledgement in this window
    // took longer than a period: still us, seen retrospectively.
    check(tick_blame_rs(0, 2000, 250, 0, 50_000) == TICK_BLAME_BLOCKED);
    // Zero ticks, nothing in flight, every acknowledgement prompt: the interrupt
    // really did not arrive. This is the ONLY shape that may be called a dead
    // timer, and before this function every one of the four above was.
    check(tick_blame_rs(0, 2000, 250, 0, 12) == TICK_BLAME_ABSENT);
    // BLOCKED and ABSENT must not collapse to the same value: the whole point.
    check(tick_blame_rs(0, 2000, 250, 1, 0) != tick_blame_rs(0, 2000, 250, 0, 12));

    fails
}
