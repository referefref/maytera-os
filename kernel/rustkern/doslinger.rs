// rustkern/doslinger.rs - the POST-EXIT LINGER policy for a DOS guest.
//
// WHAT THIS REPLACES
// ==========================================================================
// dos/dosexec.c ended every DOS guest's self-exit with
//
//     if (t->running) proc_sleep(2000);
//
// under a comment that said "keep the final frame visible for a moment".
// Three things were wrong with it, and only the first is the obvious one:
//
//  1. It is the #426-banned shape. CLAUDE.md: "BANNED: a hand-rolled
//     proc_sleep/proc_yield poll. There is now no excuse: the timed
//     primitive exists." A fixed delay is not a poll, but it is still a
//     duration standing in for a condition, and the condition it stood in
//     for ("the last frame has reached the screen") is observable.
//
//  2. THE WINDOW'S CLOSE BUTTON WAS DEAD FOR THOSE TWO SECONDS.
//     dos_request_close() (the titlebar X handler) only clears g_dos.running,
//     which nothing but the run loop reads - and the run loop has already
//     exited by the time the sleep starts. So for two seconds the user was
//     looking at a window whose X did nothing. The same comment that
//     installed the sleep says, about the close path, "a window that sits
//     there for two more seconds after you click its X reads as a hang".
//     That is exactly what the self-exit path did.
//
//  3. g_dos_busy stays set across it, so the next DOS launch is REFUSED for
//     two seconds after a game exits.
//
// WHAT THE POLICY IS NOW
// ==========================================================================
// Two questions, both answered here so neither is a magic number buried in
// the middle of a teardown function:
//
//   (a) Does this guest get a linger at all?  Only if it stopped BY ITSELF
//       and it actually drew something. A guest the user closed must go now
//       (that was already true), and a guest that never presented a frame
//       has no final frame to show, so lingering on it only delays the
//       machine (that is new).
//
//   (b) How long?  Until the final frame is ON THE GLASS, which is a real
//       condition the kernel can observe (the compositor's present counter
//       advancing past the value at the DOS layer's last window invalidate),
//       plus a short visible hold so a human registers it.
//
// THE TWO CONSTANTS, AND WHY THEY ARE THESE VALUES
// ==========================================================================
// FRAME_BACKSTOP_MS is a BACKSTOP, not a pace: the wake is ours and always
// armed (sys_fb_flip wakes g_fb_flip_wq on every present), so this expires
// only if the compositor did not present at all, which is a real fault and
// is logged loudly and durably when it happens.
//
// HOLD_MS is the deliberate visible hold, and it is the ONLY duration in
// this path that is a duration on purpose. 400 ms is about 3x the
// compositor's worst idle poll interval (120 ms in
// userland/apps/compositor/main.c), so the final frame is on screen across
// at least two idle refresh opportunities rather than one; and it is well
// under the ~1 s at which a delay stops reading as "it finished" and starts
// reading as "it hung". It is INTERRUPTIBLE: clicking the X ends it at once.
//
// NOT PACED OFF timer_ticks. The caller passes sched_now_ms(), the TSC-backed
// monotonic clock, because timer_ticks counts ticks DELIVERED and KVM replays
// missed ticks in bursts (see blame.md, "timer_ticks is not a wall clock").

use core::sync::atomic::{AtomicI32, AtomicU64, Ordering};

/// Backstop for "the final frame reached the screen". See the header comment.
pub const FRAME_BACKSTOP_MS: u32 = 250;
/// The deliberate visible hold. See the header comment.
pub const HOLD_MS: u32 = 400;

/// Set by the titlebar X (dos_request_close). Sticky for the lifetime of one
/// guest; cleared when the next one launches.
static CLOSE_REQ: AtomicI32 = AtomicI32::new(0);
/// Absolute sched_now_ms() deadline for the visible hold, armed once.
static HOLD_UNTIL_MS: AtomicU64 = AtomicU64::new(0);

/// Called at guest LAUNCH. A close request belongs to one guest; a stale one
/// left by the previous guest would cancel the next guest's linger before it
/// started, which is the class of bug this tree has hit before with
/// process-wide latches (see #736 in dosexec.c).
#[no_mangle]
pub extern "C" fn dos_linger_reset_rs() {
    CLOSE_REQ.store(0, Ordering::SeqCst);
    HOLD_UNTIL_MS.store(0, Ordering::SeqCst);
}

/// The titlebar X, from dos_request_close(). Runs on the WM/compositor
/// thread, so it does the one safe thing: set a flag. The CALLER does the
/// wake, because a wake is a kernel primitive and this module is policy.
#[no_mangle]
pub extern "C" fn dos_linger_close_rs() {
    CLOSE_REQ.store(1, Ordering::SeqCst);
}

/// Does the guest that has just stopped get a linger at all?
///
/// `self_exit` - 1 if the guest halted itself / errored / hit the run cap,
///               0 if the user asked for the window to close.
/// `published`  - 1 if this guest ever published a frame to its window.
///
/// Returns 1 to linger, 0 to tear down immediately.
#[no_mangle]
pub extern "C" fn dos_linger_wanted_rs(self_exit: i32, published: u32) -> i32 {
    if self_exit == 0 {
        return 0; // the user asked for it gone; making them wait reads as a hang
    }
    if CLOSE_REQ.load(Ordering::SeqCst) != 0 {
        return 0; // the X was clicked between the guest stopping and here
    }
    if published == 0 {
        return 0; // nothing was ever drawn: there is no final frame to show
    }
    1
}

/// Arm the visible hold against the monotonic clock. Call once, after the
/// final frame is known to be on screen.
#[no_mangle]
pub extern "C" fn dos_linger_arm_hold_rs(now_ms: u64) {
    HOLD_UNTIL_MS.store(now_ms + HOLD_MS as u64, Ordering::SeqCst);
}

/// Wait-queue condition: has the final frame reached the screen?
///
/// `flips_since_publish` is (present counter now) - (present counter at the
/// DOS layer's last win16_host_invalidate). One present after the window was
/// marked dirty is the compositor having composited and flipped it.
///
/// A PURE READ - one atomic load and an integer compare, no lock, no drain -
/// which is what wait_event() requires of a condition, since it is evaluated
/// with the caller's interrupts in whatever state the macro left them.
#[no_mangle]
pub extern "C" fn dos_linger_frame_done_rs(flips_since_publish: u64) -> i32 {
    if CLOSE_REQ.load(Ordering::SeqCst) != 0 {
        return 1;
    }
    (flips_since_publish >= 1) as i32
}

/// Wait-queue condition: has the visible hold elapsed (or been cut short by
/// the X)? `now_ms` is sched_now_ms(). Same purity contract as above.
#[no_mangle]
pub extern "C" fn dos_linger_hold_done_rs(now_ms: u64) -> i32 {
    if CLOSE_REQ.load(Ordering::SeqCst) != 0 {
        return 1;
    }
    let until = HOLD_UNTIL_MS.load(Ordering::SeqCst);
    // Signed compare so a deadline already behind us reads as expired rather
    // than as ~584 million years away.
    ((now_ms as i64).wrapping_sub(until as i64) >= 0) as i32
}

/// The two durations, for the C side's wq_ms_to_ticks() calls, so the values
/// have ONE definition and the C cannot drift from the policy (the
/// dosmick.rs lesson in blame.md: a constant mirrored into C is a constant
/// that will disagree).
#[no_mangle]
pub extern "C" fn dos_linger_frame_backstop_ms_rs() -> u32 { FRAME_BACKSTOP_MS }
#[no_mangle]
pub extern "C" fn dos_linger_hold_ms_rs() -> u32 { HOLD_MS }
