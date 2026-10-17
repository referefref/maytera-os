// uiready.rs - #317 boot auto-launch readiness: "a real UI is on screen" as a
// CONDITION, not as a duration.
//
// WHY THIS EXISTS
// ===========================================================================
// autorun_worker() (gui/desktop.c) used to open with proc_sleep(14000) and a
// comment saying "launched a few seconds after boot once the compositor has
// rendered". Those are two different things, and the code implemented the one
// that cannot be right:
//
//  1. timer_ticks IS NOT A WALL CLOCK. KVM replays lost ticks in BURSTS, so a
//     `timer_ticks + N` deadline can fire early, sometimes immediately. A fixed
//     14000 ms is therefore not even reliably 14 seconds. MEASURED 2026-09-29
//     on a throwaway VM from golden 2496: the sleep took long enough that the
//     in-guest log clock advanced from 01:36 to 02:12 across it, on an idle
//     two-core box with nothing else running.
//
//  2. EVEN A PERFECT 14 SECONDS IS A GUESS AT A CONDITION. The condition the
//     comment names is observable exactly: the compositor's first present,
//     which is what desktop_run()'s wait loop already watches for
//     (g_fb_flip_count leaving 0) before it hands the display over. MEASURED
//     in the same run: the first present landed ONE log second after the
//     compositor was spawned, and the autorun launch happened ~35 s later, so
//     the duration was wrong in the SLOW direction by an order of magnitude.
//     In an earlier run on the same image the ordering inverted and a
//     compositor-spawned app ran to completion BEFORE the first present, which
//     is what "the guess is wrong under exactly the load where it matters"
//     looks like from the other side.
//
// So the wait moved onto the event. This module owns the STATE and the POLICY
// (what counts as "up"); gui/desktop.c owns the wait_queue_head_t, the
// wake_up_all() at each event site, and the wait_event_timeout() macro call,
// because those are C primitives and the call sites are in a C loop. Same
// split as rustkern/sysvol.rs (Rust holds the state machine and the wait-queue
// condition, C does the macro) and rustkern/sessend.rs (Rust holds the rule, C
// does the table walk).
//
// TWO EVENTS, NOT ONE, AND THAT IS THE POINT
// ===========================================================================
// "A real UI is on screen" is true in two different ways, and autorun must
// proceed in both:
//
//   * COMPOSITOR: /APPS/COMPOSIT presented its first frame. The normal boot.
//   * FALLBACK:   the compositor produced no frame inside desktop_run()'s own
//                 bound and the in-kernel desktop engaged instead (#157).
//
// Waking only on the first would leave autorun asleep for its whole timeout on
// exactly the boot that already went wrong. Both sites set a bit here and both
// wake the queue, so the wake source is redundant in the sense the tree's
// waiting rules ask for: there is no boot path that puts a UI on screen without
// passing through one of them.
//
// The wait still carries a generous wait_event_timeout() upper bound on the C
// side. That is belt and braces, NOT the mechanism: a timeout hides a broken
// wake source, so it is there only so that a future third path to a live
// desktop degrades into "autorun is late" instead of "autorun never runs".
//
// New kernel logic with no C twin, so Rust per the 2026-07-16 rule, and no
// -DRUST_* strangler flag and no RUST_PORT_LEDGER row: there is no C original
// to differ from, so the rollback is reverting the commit. uiready_selftest_rs()
// stands in for a differential, over the PURE core rather than the static, so
// running it cannot perturb the live boot state it is reporting on.

#![allow(dead_code)]

use core::sync::atomic::{AtomicU32, Ordering};

/// The compositor presented its first frame (gui/fb_syscall.c g_fb_flip_count
/// left 0, observed by desktop_run()).
pub const UI_COMPOSITOR: u32 = 1 << 0;
/// The compositor never presented inside its bound and the in-kernel fallback
/// desktop engaged instead (#157).
pub const UI_FALLBACK: u32 = 1 << 1;

const UI_ANY: u32 = UI_COMPOSITOR | UI_FALLBACK;

/// Live boot state. Bits are STICKY: a UI that has been on screen once has
/// been on screen, and a later compositor death (which desktop_run() handles by
/// relaunching) must not un-ready a waiter that is already past the gate.
static STATE: AtomicU32 = AtomicU32::new(0);

// --- the pure core, so the self-test can exercise it without touching STATE --

/// Record an event into a state word.
const fn ui_apply(state: u32, ev: u32) -> u32 {
    state | (ev & UI_ANY)
}

/// Is a real UI on screen, given this state word?
const fn ui_is_up(state: u32) -> bool {
    (state & UI_ANY) != 0
}

// --- the live surface used by gui/desktop.c ---------------------------------

/// The compositor's first present. Called from desktop_run()'s wait loop at the
/// same point it releases the boot console, so the two cannot disagree about
/// when the handover happened.
#[no_mangle]
pub extern "C" fn uiready_mark_compositor_rs() {
    STATE.fetch_or(UI_COMPOSITOR, Ordering::SeqCst);
}

/// The in-kernel fallback desktop engaged (#157): the compositor produced no
/// frame inside its bound. A UI, just not the intended one.
#[no_mangle]
pub extern "C" fn uiready_mark_fallback_rs() {
    STATE.fetch_or(UI_FALLBACK, Ordering::SeqCst);
}

/// The wait-queue condition for autorun_worker(). A PURE ATOMIC READ: it takes
/// no lock, allocates nothing and touches no device, so it is legal to evaluate
/// with interrupts off, which is how wait_event_timeout() evaluates it between
/// sleeps.
#[no_mangle]
pub extern "C" fn uiready_up_rs() -> i32 {
    if ui_is_up(STATE.load(Ordering::SeqCst)) { 1 } else { 0 }
}

/// The raw state word, for the one log line that says WHICH event released the
/// gate. Reported rather than inferred, because "the compositor rendered" and
/// "the compositor never rendered and we fell back" are the two boots whose
/// autorun behaviour someone will one day need to tell apart.
#[no_mangle]
pub extern "C" fn uiready_state_rs() -> u32 {
    STATE.load(Ordering::SeqCst)
}

/// Vector test over the pure core. Returns the number of FAILING vectors, so 0
/// is a pass. Run once on boot with one line of output.
#[no_mangle]
pub extern "C" fn uiready_selftest_rs() -> i32 {
    let mut bad = 0;

    // Nothing has happened yet: not up.
    if ui_is_up(0) { bad += 1; }

    // Either event alone releases the gate.
    if !ui_is_up(ui_apply(0, UI_COMPOSITOR)) { bad += 1; }
    if !ui_is_up(ui_apply(0, UI_FALLBACK)) { bad += 1; }

    // Both, in either order, and idempotent: a second first-frame observation
    // must not clear the first (this is the bug a `=` instead of a `|=` makes).
    let s = ui_apply(ui_apply(0, UI_COMPOSITOR), UI_FALLBACK);
    if s != UI_ANY || !ui_is_up(s) { bad += 1; }
    let s = ui_apply(ui_apply(0, UI_FALLBACK), UI_COMPOSITOR);
    if s != UI_ANY || !ui_is_up(s) { bad += 1; }
    if ui_apply(ui_apply(0, UI_COMPOSITOR), UI_COMPOSITOR) != UI_COMPOSITOR { bad += 1; }

    // A bit outside the vocabulary must not release the gate, so a future
    // caller passing a stale or garbage constant fails closed rather than
    // letting autorun run against a black screen.
    if ui_is_up(ui_apply(0, 1 << 7)) { bad += 1; }
    if ui_apply(0, 1 << 7) != 0 { bad += 1; }

    bad
}
