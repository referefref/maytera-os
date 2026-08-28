// rustkern/dragsess.rs - the cross-window drag session (Tier 5 "docking").
//
// WHAT PROBLEM THIS SOLVES
// -----------------------
// A user grabs a terminal tab in window A and drops it on window B, which is a
// DIFFERENT PROCESS. Nothing in the tree could express that. Every drag that
// existed (desktop icons, sticky notes, the desktop pet, widget panels, and the
// kernel WM title-bar move/resize) is intra-process or WM-internal, so a drop
// could never cross a process boundary.
//
// The missing piece is smaller than it looks, because of how input already
// works here. wm_dispatch_event() routes every mouse event to
// window_get_at_point(), the TOPMOST window under the cursor - there is NO
// pointer grab. So the moment the cursor crosses from A into B, B ALREADY
// receives EVENT_MOUSE_MOVE with the button held, and on release it ALREADY
// receives EVENT_MOUSE_UP. B is being told where the pointer is; it is simply
// not told that a drag is in flight or what the drag carries. Symmetrically A
// stops receiving events at its own edge, so A cannot see its own drop land.
//
// This module is the one shared fact both halves are missing: ONE system-wide
// drag session, plus a per-window table of which payload kinds each window is
// willing to receive. That is the whole protocol. No new input routing, no
// grab, and not one line added to the mouse dispatch path.
//
// WHAT RUNS WHEN NOBODY IS DRAGGING
// ---------------------------------
// Nothing. ACTIVE is 0, no hook exists in wm_handle_mouse_move(),
// wm_dispatch_event() or user_window_queue_event(), and every entry point below
// returns on a single integer test. The only cost anywhere in the system is in
// the compositor, which calls drag_peek exactly once per frame and ONLY while a
// mouse button is physically held (see dragghost.c). Button not held: zero.
//
// WHY RUST (2026-07-16 Rust-by-default kernel rule)
// -------------------------------------------------
// This is new kernel state whose entire risk surface is copying a
// caller-supplied, caller-sized byte range into and out of fixed buffers, and
// indexing a fixed table by a Ring-3-supplied window handle. That is exactly
// the shape bounds checking protects. Every length is clamped HERE, in one
// place; every handle is range-checked HERE. No float, no paging, no hot path.
//
// PAYLOAD PRIVACY IS A DESIGN CONSTRAINT, NOT AN ACCIDENT
// -------------------------------------------------------
// drag_peek_rs() deliberately returns the LABEL and never the PAYLOAD. The
// compositor peeks on every frame of every drag so it can draw the ghost that
// follows the cursor, and a terminal pane payload can contain scrollback: the
// output of the last command the user ran, which routinely includes secrets.
// The compositor has no business reading that to draw a caption. Only
// drag_take_rs() returns bytes, only to the window the kernel itself resolved
// as the drop target, and only after the button has actually been released.
//
// CONCURRENCY
// -----------
// The kernel serializes syscalls behind the big kernel lock, exactly as
// rustkern/clipboard.rs documents for the same reason, so these statics need no
// additional lock. They are plain statics living for the life of the kernel:
// no allocator, so this module needs neither alloc nor core::alloc.
//
// FFI SURFACE (declared in ../rust-symbols.manifest, called from proc/syscall.c)
//   drag_active_rs()                            -> 1 while a session exists
//   drag_accept_rs(win, mask)                   -> register/clear a drop target
//   drag_begin_rs(win, pid, kind, pay, n, lab, m)
//   drag_peek_rs(out)                           -> fill drag_info_t, -1 if idle
//   drag_win_accepts_rs(win, kind)              -> target-resolution predicate
//   drag_take_rs(win, dst, cap)                 -> claim the payload
//   drag_release_rs(x, y, target)               -> button up; target pre-resolved
//   drag_end_rs()                               -> clear, returns source handle
//   drag_cancel_win_rs(win)                     -> a window died
//   drag_selftest_rs()                          -> 0 on pass (boot proof)

use core::ptr;

// MUST equal MAX_USER_WINDOWS in proc/syscall.c. A window HANDLE (0..15) is the
// identifier throughout this protocol rather than a window_t id, because that
// is what a Ring-3 app already holds from win_create() and what user_windows[]
// is indexed by, so no translation table is needed anywhere. proc/syscall.c
// carries a _Static_assert tying the two together.
pub const DRAG_MAX_WIN: usize = 16;

// Bounded, statically reserved. 4 KiB carries a terminal tab: cwd, title, a
// modest scrollback excerpt. A larger payload is REFUSED, never truncated - a
// half scrollback is still a plausible-looking scrollback, and silently
// dropping the tail of a hand-off is the kind of fault that shows up as
// "sometimes the bottom of my terminal is missing" months later.
const PAYLOAD_CAP: usize = 4096;
const LABEL_CAP: usize   = 64;

// Payload kinds. A bitmask so one window can accept several kinds; also means
// drag_accept_rs(win, 0) is the natural "stop accepting" call.
pub const DRAG_KIND_TERMTAB: u32 = 1 << 0;   // a terminal tab/pane
pub const DRAG_KIND_TEXT:    u32 = 1 << 1;   // reserved
pub const DRAG_KIND_FILE:    u32 = 1 << 2;   // reserved
const DRAG_KIND_ALL: u32 = DRAG_KIND_TERMTAB | DRAG_KIND_TEXT | DRAG_KIND_FILE;

// ---- the single session -----------------------------------------------------
// ACTIVE is THE inertness flag. Every entry point tests it first.
static mut ACTIVE:      u32 = 0;
static mut SRC_WIN:     i32 = -1;
static mut SRC_PID:     u32 = 0;
static mut KIND:        u32 = 0;
static mut PAYLOAD:     [u8; PAYLOAD_CAP] = [0u8; PAYLOAD_CAP];
static mut PAYLOAD_LEN: usize = 0;
static mut LABEL:       [u8; LABEL_CAP] = [0u8; LABEL_CAP];
static mut LABEL_LEN:   usize = 0;
// RELEASED separates "the user is still holding the button" from "the button
// is up and a target has been resolved". drag_take_rs() refuses before this is
// set, so a window cannot claim a payload out from under a drag still in
// flight merely by guessing that one is happening.
static mut RELEASED:    u32 = 0;
static mut DROP_X:      i32 = 0;
static mut DROP_Y:      i32 = 0;
static mut TARGET_WIN:  i32 = -1;

// Per-window accept mask. A window that never calls drag_accept_rs() has 0
// here, is never resolved as a target, and therefore cannot be reached by this
// protocol at all. That is the opt-in.
static mut ACCEPT: [u32; DRAG_MAX_WIN] = [0u32; DRAG_MAX_WIN];

// Mirrors drag_info_t in proc/syscall.h and userland/libc/syscall.h. Both
// carry a _Static_assert on this size, so a one-sided edit goes RED at build
// time instead of silently misreading fields at runtime (the wm_window_info_t
// discipline, #745/#41/#44).
#[repr(C)]
pub struct DragInfo {
    pub active:      i32,
    pub src_win:     i32,
    pub src_pid:     u32,
    pub kind:        u32,
    pub payload_len: i32,
    pub released:    i32,
    pub drop_x:      i32,
    pub drop_y:      i32,
    pub target_win:  i32,
    pub label_len:   i32,
    pub label:       [u8; LABEL_CAP],
}

#[inline]
fn win_ok(win: i32) -> bool { win >= 0 && (win as usize) < DRAG_MAX_WIN }

// Clear every field. Called from drag_end_rs and the cancel paths so there is
// ONE definition of "no session", rather than several places each zeroing the
// subset they happened to think of.
unsafe fn reset() {
    ACTIVE = 0;
    SRC_WIN = -1;
    SRC_PID = 0;
    KIND = 0;
    PAYLOAD_LEN = 0;
    LABEL_LEN = 0;
    RELEASED = 0;
    DROP_X = 0;
    DROP_Y = 0;
    TARGET_WIN = -1;
    // The payload bytes themselves are wiped, not just length-zeroed. They can
    // be terminal scrollback; leaving them in a static for the next drag to
    // partially expose through a short read would be a needless disclosure.
    let p = ptr::addr_of_mut!(PAYLOAD) as *mut u8;
    ptr::write_bytes(p, 0, PAYLOAD_CAP);
}

#[no_mangle]
pub extern "C" fn drag_active_rs() -> i64 {
    unsafe { if ACTIVE != 0 { 1 } else { 0 } }
}

// Register (or with mask==0, clear) a window as willing to receive `mask`
// kinds. Returns 0, or -1 on a bad handle / unknown kind bit. Rejecting
// unknown bits keeps a future kind from being silently accepted by an app
// built before that kind existed.
#[no_mangle]
pub extern "C" fn drag_accept_rs(win: i32, mask: u32) -> i64 {
    if !win_ok(win) { return -1; }
    if mask & !DRAG_KIND_ALL != 0 { return -1; }
    unsafe {
        let a = ptr::addr_of_mut!(ACCEPT) as *mut u32;
        *a.add(win as usize) = mask;
    }
    0
}

// Start a drag. Refuses if one is already in flight: exactly one drag exists
// system-wide, which is what makes "the window under the cursor" an
// unambiguous answer. Refuses an over-length payload rather than truncating.
#[no_mangle]
pub extern "C" fn drag_begin_rs(win: i32, pid: u32, kind: u32,
                                payload: *const u8, plen: usize,
                                label: *const u8, llen: usize) -> i64 {
    if !win_ok(win) { return -1; }
    if kind == 0 || (kind & !DRAG_KIND_ALL) != 0 { return -1; }
    if plen > PAYLOAD_CAP { return -1; }
    unsafe {
        if ACTIVE != 0 { return -1; }
        reset();
        if plen > 0 && !payload.is_null() {
            let d = ptr::addr_of_mut!(PAYLOAD) as *mut u8;
            ptr::copy_nonoverlapping(payload, d, plen);
            PAYLOAD_LEN = plen;
        }
        // The label is a caption, so it IS clamped rather than refused: a
        // shortened ghost caption is cosmetic, where a shortened payload is
        // corruption. The two are treated differently on purpose.
        let m = if llen > LABEL_CAP { LABEL_CAP } else { llen };
        if m > 0 && !label.is_null() {
            let d = ptr::addr_of_mut!(LABEL) as *mut u8;
            ptr::copy_nonoverlapping(label, d, m);
            LABEL_LEN = m;
        }
        SRC_WIN = win;
        SRC_PID = pid;
        KIND = kind;
        ACTIVE = 1;
    }
    0
}

// Non-destructive read of the session, minus the payload (see the module
// header on why the payload is not here). Returns 0, or -1 when idle so the
// caller can branch on one integer without inspecting the struct.
#[no_mangle]
pub extern "C" fn drag_peek_rs(out: *mut DragInfo) -> i64 {
    if out.is_null() { return -1; }
    unsafe {
        if ACTIVE == 0 { return -1; }
        (*out).active      = 1;
        (*out).src_win     = SRC_WIN;
        (*out).src_pid     = SRC_PID;
        (*out).kind        = KIND;
        (*out).payload_len = PAYLOAD_LEN as i32;
        (*out).released    = RELEASED as i32;
        (*out).drop_x      = DROP_X;
        (*out).drop_y      = DROP_Y;
        (*out).target_win  = TARGET_WIN;
        (*out).label_len   = LABEL_LEN as i32;
        let s = ptr::addr_of!(LABEL) as *const u8;
        let d = ptr::addr_of_mut!((*out).label) as *mut u8;
        ptr::copy_nonoverlapping(s, d, LABEL_CAP);
    }
    0
}

// Does this window accept this kind? Used by the C side when resolving a drop
// target, so the predicate has ONE definition.
#[no_mangle]
pub extern "C" fn drag_win_accepts_rs(win: i32, kind: u32) -> i64 {
    if !win_ok(win) || kind == 0 { return 0; }
    unsafe {
        let a = ptr::addr_of!(ACCEPT) as *const u32;
        if (*a.add(win as usize)) & kind != 0 { 1 } else { 0 }
    }
}

// The button is up. `target` is the drop target the CALLER already resolved
// with the kernel WM hit-test (window_get_at_point + drag_win_accepts_rs), or
// -1 for none. Returns the target it recorded.
//
// TRUST NOTE, stated rather than papered over: this is meant to be called by
// the compositor, and nothing enforces that, exactly as nothing enforces it
// for SYS_INJECT_MOUSE / SYS_SET_MOUSE_BUTTONS today. A hostile app can end
// someone else's drag early. It cannot forge a target, because proc/syscall.c
// re-derives the target from the kernel's own hit-test rather than trusting
// the argument, and it cannot read the payload, because only the resolved
// target can take it. Making input arbitration privileged is a real ticket and
// a bigger one than this feature; it should cover all three syscalls together.
#[no_mangle]
pub extern "C" fn drag_release_rs(x: i32, y: i32, target: i32) -> i64 {
    unsafe {
        if ACTIVE == 0 { return -1; }
        RELEASED = 1;
        DROP_X = x;
        DROP_Y = y;
        TARGET_WIN = if win_ok(target) { target } else { -1 };
        TARGET_WIN as i64
    }
}

// The resolved target claims the payload. Refuses unless the button is already
// up AND this window is the target the kernel resolved. Returns the number of
// bytes copied (min(len, cap)), or -1.
#[no_mangle]
pub extern "C" fn drag_take_rs(win: i32, dst: *mut u8, cap: usize) -> i64 {
    if !win_ok(win) { return -1; }
    unsafe {
        if ACTIVE == 0 || RELEASED == 0 { return -1; }
        if TARGET_WIN != win { return -1; }
        let n = if PAYLOAD_LEN > cap { cap } else { PAYLOAD_LEN };
        if n > 0 && !dst.is_null() {
            let s = ptr::addr_of!(PAYLOAD) as *const u8;
            ptr::copy_nonoverlapping(s, dst, n);
        }
        n as i64
    }
}

// End the session. Returns the SOURCE window handle so the caller can post its
// EVENT_DRAG_END before the state is gone, or -1 if there was nothing to end.
#[no_mangle]
pub extern "C" fn drag_end_rs() -> i64 {
    unsafe {
        if ACTIVE == 0 { return -1; }
        let src = SRC_WIN as i64;
        reset();
        src
    }
}

// A window went away (closed, or its process died). Clears its accept
// registration always. If it was the SOURCE the session dies with it and this
// returns -1 (nothing to notify: the source is what vanished). If it was the
// resolved TARGET the session survives with the target cleared, so the source
// still gets told the drop was not taken, and this returns the source handle.
// Returns -2 when the dead window had nothing to do with any session.
#[no_mangle]
pub extern "C" fn drag_cancel_win_rs(win: i32) -> i64 {
    if !win_ok(win) { return -2; }
    unsafe {
        let a = ptr::addr_of_mut!(ACCEPT) as *mut u32;
        *a.add(win as usize) = 0;
        if ACTIVE == 0 { return -2; }
        if SRC_WIN == win { reset(); return -1; }
        if TARGET_WIN == win {
            TARGET_WIN = -1;
            return SRC_WIN as i64;
        }
        -2
    }
}

// Boot proof. Every case asserts the REFUSAL as well as the success, because a
// protocol whose refusals have never been seen to fire is not a protocol, it is
// an intention. Returns 0 on pass, else a bitmask naming which cases failed.
#[no_mangle]
pub extern "C" fn drag_selftest_rs() -> i64 {
    let mut fail: i64 = 0;
    unsafe {
        let saved_active = ACTIVE;
        reset();
        let a = ptr::addr_of_mut!(ACCEPT) as *mut u32;
        let mut i = 0usize;
        while i < DRAG_MAX_WIN { *a.add(i) = 0; i += 1; }

        // 1. Idle: peek refuses, active is 0, take refuses.
        if drag_active_rs() != 0 { fail |= 1 << 0; }
        if drag_take_rs(0, ptr::null_mut(), 0) != -1 { fail |= 1 << 0; }

        // 2. Bad handles and bad kinds are refused.
        if drag_accept_rs(-1, DRAG_KIND_TERMTAB) != -1 { fail |= 1 << 1; }
        if drag_accept_rs(DRAG_MAX_WIN as i32, DRAG_KIND_TERMTAB) != -1 { fail |= 1 << 1; }
        if drag_accept_rs(0, 0x8000_0000) != -1 { fail |= 1 << 1; }
        if drag_begin_rs(0, 1, 0, ptr::null(), 0, ptr::null(), 0) != -1 { fail |= 1 << 1; }

        // 3. An over-length payload is REFUSED, not truncated.
        let big = PAYLOAD_CAP + 1;
        if drag_begin_rs(0, 1, DRAG_KIND_TERMTAB, ptr::null(), big, ptr::null(), 0) != -1 {
            fail |= 1 << 2;
        }
        if ACTIVE != 0 { fail |= 1 << 2; }

        // 4. A real begin succeeds; a SECOND one is refused.
        let pay: [u8; 4] = [97, 98, 99, 100];
        let lab: [u8; 3] = [116, 97, 98];
        if drag_begin_rs(2, 77, DRAG_KIND_TERMTAB,
                         pay.as_ptr(), 4, lab.as_ptr(), 3) != 0 { fail |= 1 << 3; }
        if drag_begin_rs(3, 78, DRAG_KIND_TERMTAB,
                         pay.as_ptr(), 4, lab.as_ptr(), 3) != -1 { fail |= 1 << 3; }
        if drag_active_rs() != 1 { fail |= 1 << 3; }

        // 5. take() refuses BEFORE release, even from a plausible target.
        let mut got: [u8; 8] = [0u8; 8];
        if drag_take_rs(5, got.as_mut_ptr(), 8) != -1 { fail |= 1 << 4; }

        // 6. accept() gates target resolution.
        if drag_win_accepts_rs(5, DRAG_KIND_TERMTAB) != 0 { fail |= 1 << 5; }
        if drag_accept_rs(5, DRAG_KIND_TERMTAB) != 0 { fail |= 1 << 5; }
        if drag_win_accepts_rs(5, DRAG_KIND_TERMTAB) != 1 { fail |= 1 << 5; }
        if drag_win_accepts_rs(5, DRAG_KIND_FILE) != 0 { fail |= 1 << 5; }

        // 7. After release, only the RESOLVED target may take, and it gets the
        //    exact bytes.
        if drag_release_rs(100, 200, 5) != 5 { fail |= 1 << 6; }
        if drag_take_rs(6, got.as_mut_ptr(), 8) != -1 { fail |= 1 << 6; }
        if drag_take_rs(5, got.as_mut_ptr(), 8) != 4 { fail |= 1 << 6; }
        if got[0] != 97 || got[3] != 100 { fail |= 1 << 6; }

        // 8. end() returns the SOURCE handle and truly clears.
        if drag_end_rs() != 2 { fail |= 1 << 7; }
        if drag_active_rs() != 0 { fail |= 1 << 7; }
        if drag_end_rs() != -1 { fail |= 1 << 7; }

        // 9. A dead SOURCE window kills the session; a dead TARGET does not.
        if drag_begin_rs(2, 77, DRAG_KIND_TERMTAB,
                         pay.as_ptr(), 4, lab.as_ptr(), 3) != 0 { fail |= 1 << 8; }
        if drag_release_rs(1, 1, 5) != 5 { fail |= 1 << 8; }
        if drag_cancel_win_rs(5) != 2 { fail |= 1 << 8; }   // target died -> source notified
        if drag_active_rs() != 1 { fail |= 1 << 8; }        // session survives
        if drag_cancel_win_rs(2) != -1 { fail |= 1 << 8; }  // source died -> gone
        if drag_active_rs() != 0 { fail |= 1 << 8; }

        // 10. reset() wipes the payload bytes, not just the length.
        if drag_begin_rs(1, 9, DRAG_KIND_TERMTAB,
                         pay.as_ptr(), 4, ptr::null(), 0) != 0 { fail |= 1 << 9; }
        drag_end_rs();
        let p = ptr::addr_of!(PAYLOAD) as *const u8;
        if *p != 0 { fail |= 1 << 9; }

        reset();
        let mut j = 0usize;
        while j < DRAG_MAX_WIN { *a.add(j) = 0; j += 1; }
        ACTIVE = saved_active;
    }
    fail
}
