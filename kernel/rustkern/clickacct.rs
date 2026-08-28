// clickacct.rs - #197: click delivery accounting for the testinput channel.
//
// WHY THIS EXISTS. The #334 serial test-input channel injected a click by
// setting the PHYSICAL mouse state (drivers/mouse.c mouse_inject_button ->
// g_mouse.buttons / the mouse_buttons global) for a FIXED 40ms, then clearing
// it. Nothing anywhere counted whether that pulse was ever observed. The
// channel answered "OK CLICK x y" unconditionally, so the ACK was a statement
// about the parser, not about delivery. blame.md's standing rule from the
// wedged-guest false alarm applies exactly: liveness (here, delivery) must be a
// KERNEL-SIDE MONOTONIC COUNTER THAT HAS TO ADVANCE, never the presence of an
// expected reply.
//
// WHY THE PULSE LOSES CLICKS. The compositor does not receive button EVENTS
// from that path; it SAMPLES the level once per frame in process_input()
// (userland/apps/compositor/main.c) and computes the edge itself against the
// previous sample. A 40ms level pulse is therefore only seen if a frame boundary
// falls inside it. When a frame takes longer than the pulse - which is precisely
// what happens during a full-screen repaint, the thing #153 exists to fix - the
// compositor samples 0 before and 0 after, no edge is computed, and the click
// vanishes with no error anywhere in the system.
//
// WHAT IS COUNTED. Four monotonic counters that split the path into the layers
// that can lose a click, so the gap between them names the guilty layer:
//
//   INJECTED  the channel set the physical level        (drivers/mouse.c)
//   SAMPLED   sys_get_mouse() handed the compositor a value whose left-button
//             bit DIFFERS from the value it handed out last time. This is not a
//             guess about what the compositor saw: sys_get_mouse is
//             is_compositor()-gated with a single caller (process_input), and
//             the compositor's own edge test is `(buttons & 1) && !(prev & 1)`
//             over exactly this sequence of returned values. If SAMPLED
//             advanced, the compositor's edge detector fired, unavoidably.
//   ROUTED    the compositor relayed a DOWN back down through sys_inject_mouse,
//             i.e. its whole chrome hit-test chain ran and did not consume the
//             click, so it reached the kernel window manager.
//   HIT       that relayed DOWN landed on an actual window.
//
// SAMPLED-minus-INJECTED is an injection/timing fault. ROUTED-minus-SAMPLED is
// a chrome-consumed click (normal for the taskbar, a loss for an app window).
// HIT-minus-ROUTED is a coordinate or z-order fault.
//
// WHY RUST. CLAUDE.md's standing rule: new kernel code is Rust unless there is
// a stated performance or entanglement reason. This is a test/instrumentation
// path, so there is none; it is counters and an edge comparison, and the one
// caller on a hot path (sys_get_mouse) does an unconditional atomic add and one
// compare, which is what the C it replaces would have compiled to.
//
// NO FLOAT: the kernel target is x86_64-unknown-none (soft-float, SSE
// disabled). Everything below is integer.

use core::sync::atomic::{AtomicU64, Ordering};

// Left-button bit, in the PS/2 packet convention drivers/mouse.c stores
// (bit0=left, bit1=right, bit2=middle) and the compositor tests.
const LEFT: u32 = 1;

static INJ_DOWN: AtomicU64 = AtomicU64::new(0);
static INJ_UP: AtomicU64 = AtomicU64::new(0);
static SMP_DOWN: AtomicU64 = AtomicU64::new(0);
static SMP_UP: AtomicU64 = AtomicU64::new(0);
static ROUTED: AtomicU64 = AtomicU64::new(0);
static HIT: AtomicU64 = AtomicU64::new(0);
static POLLS: AtomicU64 = AtomicU64::new(0);

/// Previous value handed to the compositor by sys_get_mouse(). Starts at an
/// impossible value so the first sample never manufactures an edge.
static LAST_SAMPLE: AtomicU64 = AtomicU64::new(0xFFFF_FFFF);

/// Counter selectors for `clickacct_get_rs`. Kept as an explicit id rather than
/// a struct so the C side needs no layout agreement (no _Static_assert to drift).
pub const CA_INJ_DOWN: u32 = 0;
pub const CA_INJ_UP: u32 = 1;
pub const CA_SMP_DOWN: u32 = 2;
pub const CA_SMP_UP: u32 = 3;
pub const CA_ROUTED: u32 = 4;
pub const CA_HIT: u32 = 5;
pub const CA_POLLS: u32 = 6;

/// Record that the test channel drove the physical button level.
/// `down` != 0 for press, 0 for release.
#[no_mangle]
pub extern "C" fn clickacct_note_inject_rs(down: i32) {
    if down != 0 {
        INJ_DOWN.fetch_add(1, Ordering::Relaxed);
    } else {
        INJ_UP.fetch_add(1, Ordering::Relaxed);
    }
}

/// Record one compositor sample of the physical button state, and report
/// whether this sample carries a left-button EDGE.
///
/// Return value: 0 = no edge, 1 = press edge (0 -> 1), 2 = release edge
/// (1 -> 0). The caller uses a non-zero return to wake the test channel's wait
/// queue; edges are rare (only real button transitions), so the wake costs
/// nothing on the poll-every-frame hot path.
///
/// Sequencing note: this must be called with the value that is ACTUALLY being
/// returned to the compositor on this call, at the point of return, or the
/// counter stops meaning "the compositor's edge detector fired".
#[no_mangle]
pub extern "C" fn clickacct_note_sample_rs(buttons: u32) -> i32 {
    POLLS.fetch_add(1, Ordering::Relaxed);
    let prev = LAST_SAMPLE.swap(buttons as u64, Ordering::AcqRel);
    // First-ever sample (sentinel) is not an edge.
    if prev == 0xFFFF_FFFF {
        return 0;
    }
    let was = (prev as u32) & LEFT;
    let now = buttons & LEFT;
    if now != 0 && was == 0 {
        SMP_DOWN.fetch_add(1, Ordering::Relaxed);
        1
    } else if now == 0 && was != 0 {
        SMP_UP.fetch_add(1, Ordering::Relaxed);
        2
    } else {
        0
    }
}

/// Record a compositor -> kernel-WM relay of a left DOWN, and whether it landed
/// on a window. Called from sys_inject_mouse() for type=down, button=left only;
/// move/up/scroll and the right button are not part of the click ledger.
#[no_mangle]
pub extern "C" fn clickacct_note_routed_rs(hit: i32) {
    ROUTED.fetch_add(1, Ordering::Relaxed);
    if hit != 0 {
        HIT.fetch_add(1, Ordering::Relaxed);
    }
}

/// Read one counter. Unknown ids read as 0 rather than trapping, so a newer
/// host harness talking to an older kernel degrades to zeroes instead of dying.
#[no_mangle]
pub extern "C" fn clickacct_get_rs(which: u32) -> u64 {
    match which {
        CA_INJ_DOWN => INJ_DOWN.load(Ordering::Relaxed),
        CA_INJ_UP => INJ_UP.load(Ordering::Relaxed),
        CA_SMP_DOWN => SMP_DOWN.load(Ordering::Relaxed),
        CA_SMP_UP => SMP_UP.load(Ordering::Relaxed),
        CA_ROUTED => ROUTED.load(Ordering::Relaxed),
        CA_HIT => HIT.load(Ordering::Relaxed),
        CA_POLLS => POLLS.load(Ordering::Relaxed),
        _ => 0,
    }
}
