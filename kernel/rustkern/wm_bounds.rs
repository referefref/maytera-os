// rustkern/wm_bounds.rs - #404 (cfhost): the pure POLICY behind SYS_WM_SET_BOUNDS,
// the one new kernel hook the Cardfile window-hosting layer needs
// (docs/CARDFILE_ARCHITECTURE.md section 4).
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
// It is a pure decision function over a window's current flag word plus a
// requested rect and a small CF_* flag set: no allocation, no I/O, no window
// list. That is exactly the shape that crosses the FFI without risk, and it is
// the whole "flag set / bounds decision" half of the syscall.
//
// THE C GLUE THAT STAYS C, AND WHY (stated per the Rust-first honesty rule):
//   * the caller-is-compositor gate (uw_caller_is_compositor(), proc/syscall.c)
//     and the syscall dispatch case - dispatch and privilege are C plumbing;
//   * the window-table LOOKUP - windows live on wm_state.window_list, a C-static
//     intrusive linked list of window_t with no FFI surface, so walking it to
//     resolve `id` and then APPLYING this plan (store the flag word, set the
//     bounds, call window_resize()/user_window_handle_resize() to reflow, and
//     wm_invalidate_all()) is done by sys_wm_set_bounds() in kernel/gui/window.c.
// Only the arithmetic/flag decision is here. This is the same C-lookup /
// Rust-policy split winbuf.rs (winstate_bits_rs) already uses.
//
// THE HOSTING CONTRACT. The Cardfile deck draws its own card frame, so a hosted
// window must carry NO kernel chrome (CF_MANAGED -> WINDOW_FLAG_NOCHROME), and a
// stowed card's window must not composite at all (CF_HIDDEN -> minimized/hidden,
// only its tab plate shows). A shown card's window is placed at an ARBITRARY
// body rect (open-single, one group pane, or one column), which no existing
// syscall could do for a FOREIGN window (SYS_WIN_MOVE/_BY act on the caller's
// own handle; sys_wm_maximize_focused fills one work-area rect). Placement is
// compositor-privileged: gated C-side exactly like the input-inject and
// screen-capture syscalls.

/// Requested-flag bits carried in SYS_WM_SET_BOUNDS's `flags` argument. These
/// are the ABI the libc wrapper and the compositor's cardfile_host.c pass; they
/// are NOT WINDOW_FLAG_* values (those are kernel-internal and this function
/// maps onto them).
pub const CF_MANAGED: u32 = 1 << 0; // deck draws the frame: window gets NOCHROME
pub const CF_HIDDEN:  u32 = 1 << 1; // stowed: minimized, not composited

// WINDOW_FLAG_* bits this policy reads/writes. Pinned against kernel/gui/window.h
// by _Static_asserts on the C side (sys_wm_set_bounds), so a reshuffle there
// fails the build rather than silently corrupting a flag word here.
const WF_VISIBLE:   u32 = 1 << 0;
const WF_MINIMIZED: u32 = 1 << 7;
const WF_NOCHROME:  u32 = 1 << 9;

/// Dimension bounds for a placed window, in pixels. A placed window is also a
/// content buffer, so the same ceiling winbuf.rs uses applies; a hidden window's
/// geometry is ignored, so these only gate the shown case.
pub const WM_BOUNDS_MIN_DIM: i32 = 1;
pub const WM_BOUNDS_MAX_DIM: i32 = 16384;

/// The decision the C glue applies. `#[repr(C)]`, all 32-bit fields, no padding:
/// its size is locked by a `_Static_assert` on the C side.
#[repr(C)]
pub struct WmBoundsPlan {
    /// 1 = apply this plan; 0 = reject the request (bad dimensions on a shown
    /// window). On reject every other field is left at a safe no-op default.
    pub accept: i32,
    /// The WINDOW_FLAG_* word to store on the window (NOCHROME, VISIBLE and
    /// MINIMIZED all already resolved). The C glue stores this verbatim.
    pub new_flags: u32,
    /// 1 = the window should be hidden (stowed); 0 = shown. Informational for
    /// the C glue's logging/short-circuit; the flag word already reflects it.
    pub hide: i32,
    /// 1 = apply x/y/w/h below (and resize if w/h changed); 0 = leave the
    /// window's geometry untouched (the hidden case never moves a window).
    pub set_geom: i32,
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
}

#[inline]
fn write_default(out: *mut WmBoundsPlan) {
    if !out.is_null() {
        unsafe {
            (*out).accept = 0;
            (*out).new_flags = 0;
            (*out).hide = 0;
            (*out).set_geom = 0;
            (*out).x = 0;
            (*out).y = 0;
            (*out).w = 0;
            (*out).h = 0;
        }
    }
}

/// Compute the placement plan for one SYS_WM_SET_BOUNDS request.
///
/// `cur_flags` is the window's current WINDOW_FLAG_* word; `x`/`y`/`w`/`h` the
/// requested rect; `req_flags` the CF_* request bits. Writes the plan through
/// `out` (always written when non-null, including on reject, so a caller that
/// ignores the return value applies a no-op rather than uninitialised memory).
/// Returns `out.accept` (1 apply, 0 reject).
///
/// Pure and total: it reads nothing but its arguments and cannot fail.
#[no_mangle]
pub extern "C" fn wm_bounds_plan_rs(cur_flags: u32, x: i32, y: i32, w: i32, h: i32,
                                    req_flags: u32, out: *mut WmBoundsPlan) -> i32 {
    write_default(out);
    if out.is_null() {
        return 0;
    }

    let managed = req_flags & CF_MANAGED != 0;
    let hidden = req_flags & CF_HIDDEN != 0;

    // NOCHROME is a one-way opt-in under Cardfile: a managed window never
    // regains its kernel titlebar mid-session, so this only ever SETS the bit.
    let mut new_flags = cur_flags;
    if managed {
        new_flags |= WF_NOCHROME;
    }

    if hidden {
        // Stowed: minimized and not visible, geometry left where it was so the
        // window keeps a sane rect for when it reappears. Equivalent to
        // window_minimize(), but expressed as a flag word so the C glue applies
        // exactly one thing.
        new_flags |= WF_MINIMIZED;
        new_flags &= !WF_VISIBLE;
        unsafe {
            (*out).accept = 1;
            (*out).new_flags = new_flags;
            (*out).hide = 1;
            (*out).set_geom = 0;
        }
        return 1;
    }

    // Shown: the rect must be sane, because it sizes a real content buffer.
    // Reject (not clamp): a clamp would hand the app a buffer of a different
    // size than the card body it is told it fills (the winbuf.rs lesson).
    if w < WM_BOUNDS_MIN_DIM || h < WM_BOUNDS_MIN_DIM
        || w > WM_BOUNDS_MAX_DIM || h > WM_BOUNDS_MAX_DIM {
        // out stays at the reject default.
        return 0;
    }

    new_flags &= !WF_MINIMIZED;
    new_flags |= WF_VISIBLE;
    unsafe {
        (*out).accept = 1;
        (*out).new_flags = new_flags;
        (*out).hide = 0;
        (*out).set_geom = 1;
        (*out).x = x;
        (*out).y = y;
        (*out).w = w;
        (*out).h = h;
    }
    1
}

// ---------------------------------------------------------------------------
// Self-test: proves the four state transitions the syscall relies on. Registered
// through the Rust self-test registry (selftestreg.rs) like the other modules,
// so `[RUST-SELFTEST]` at boot exercises it.
// ---------------------------------------------------------------------------

/// Returns 0 on success, or a small non-zero code naming the first failed case.
#[no_mangle]
pub extern "C" fn wm_bounds_selftest_rs() -> i32 {
    let mut p = WmBoundsPlan { accept: 0, new_flags: 0, hide: 0, set_geom: 0,
                               x: 0, y: 0, w: 0, h: 0 };

    // 1. Shown + managed: NOCHROME set, VISIBLE set, MINIMIZED cleared, geom applied.
    wm_bounds_plan_rs(WF_MINIMIZED, 30, 0, 800, 600, CF_MANAGED, &mut p);
    if p.accept != 1 || p.set_geom != 1 || p.hide != 0 { return 1; }
    if p.new_flags & WF_NOCHROME == 0 { return 2; }
    if p.new_flags & WF_VISIBLE == 0 { return 3; }
    if p.new_flags & WF_MINIMIZED != 0 { return 4; }
    if p.x != 30 || p.y != 0 || p.w != 800 || p.h != 600 { return 5; }

    // 2. Hidden: MINIMIZED set, VISIBLE cleared, geometry NOT applied, NOCHROME kept.
    wm_bounds_plan_rs(WF_VISIBLE | WF_NOCHROME, 10, 20, 400, 300,
                      CF_MANAGED | CF_HIDDEN, &mut p);
    if p.accept != 1 || p.hide != 1 || p.set_geom != 0 { return 6; }
    if p.new_flags & WF_MINIMIZED == 0 { return 7; }
    if p.new_flags & WF_VISIBLE != 0 { return 8; }
    if p.new_flags & WF_NOCHROME == 0 { return 9; }

    // 3. Reject: a zero/oversized dimension on a shown window is refused, not clamped.
    if wm_bounds_plan_rs(WF_VISIBLE, 0, 0, 0, 600, CF_MANAGED, &mut p) != 0 { return 10; }
    if p.accept != 0 { return 11; }
    if wm_bounds_plan_rs(WF_VISIBLE, 0, 0, 800, WM_BOUNDS_MAX_DIM + 1, CF_MANAGED, &mut p) != 0 { return 12; }
    if p.accept != 0 { return 13; }

    // 4. Unmanaged shown request leaves NOCHROME untouched (does not force it).
    wm_bounds_plan_rs(WF_VISIBLE, 0, 0, 800, 600, 0, &mut p);
    if p.accept != 1 || p.new_flags & WF_NOCHROME != 0 { return 14; }

    0
}
