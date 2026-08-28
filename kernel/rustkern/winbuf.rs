// rustkern/winbuf.rs - #137: how big a window content buffer is allowed to be,
// and how many bytes it needs.
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
// It is a pure arithmetic decision with no allocation and no I/O, which is
// exactly the shape that crosses the FFI without risk, and it has FOUR C
// callers that were each doing their own (different, and in three cases
// absent) version of it.
//
// ===========================================================================
// THE DEFECT THIS REMOVES, MEASURED (#137, kernel build 1914, VM <vmid>)
// ---------------------------------------------------------------------------
// proc/syscall.c's sys_win_create() took `width` and `height` STRAIGHT FROM
// RING 3 with no validation of any kind, handed them to window_create() (which
// also clamps nothing), and then sized the window's content buffer with
//
//     kmalloc(ww * wh * sizeof(uint32_t))
//
// where ww and wh are int32_t. Two independent faults in one line:
//
//  1. UNBOUNDED SIZE. An ordinary unprivileged app calling
//     win_create("hog", 0, 0, 9000, 6000) asks for ~216 MB of kernel heap in
//     one syscall. HEAP_MAX_SIZE is 256 MB. MEASURED on a 4 GB guest: that
//     single call, which RETURNED -1 and looked like a clean failure, drove
//     heap_expand() to take another 128 MB of physical pages
//     (used 1072380 KB -> 1203708 KB) and the heap never gives memory back, so
//     the kernel heap was permanently pinned at its 256 MB ceiling and 128 MB
//     of RAM was permanently unavailable, from one failed Ring-3 call.
//     MEASURED on a guest with less free RAM than the expansion chunk: the
//     same single call wedged the machine - all four vCPUs halted, CPU#0 at
//     kpanic_halt with RFL.IF=0, i.e. a core that will never take a timer
//     interrupt again. No panic reached the serial log before it died.
//
//  2. int OVERFLOW. `ww * wh` is evaluated as int BEFORE the size_t
//     conversion. Large-but-not-absurd dimensions wrap it to a SMALL POSITIVE
//     value, so a tiny buffer gets published together with content_width /
//     content_height of tens of thousands. Every later draw syscall indexes
//     `content_buffer[py * content_width + px]`, so the very next
//     win_draw_rect() walks gigabytes past a few-megabyte allocation, in
//     Ring 0, over the kernel heap.
//
// ===========================================================================
// THE POLICY, STATED RATHER THAN IMPLIED
// ---------------------------------------------------------------------------
// A window content buffer is a 32-bpp image that has to be composited onto a
// framebuffer, so it is bounded by what a display can show, not by what the
// heap could theoretically hold. Two independent limits, because either one
// alone lets something silly through:
//
//   WINBUF_MAX_DIM   16384 px on a side. Comfortably past any panel that
//                    exists (an 8K display is 7680 wide), and it is the
//                    dimension bound that stops the int multiply from ever
//                    being asked to overflow.
//   WINBUF_MAX_BYTES 64 MiB. 16 megapixels at 4 bytes each: a 4096x4096
//                    window, or 8192x2048. That is four times a 4K desktop's
//                    entire framebuffer, and one quarter of HEAP_MAX_SIZE, so
//                    even a full complement of MAX_USER_WINDOWS cannot corner
//                    the heap with legal requests.
//
// Rejection is deliberately a REFUSAL, not a clamp. A clamp would hand the app
// a buffer of a different size than the window it thinks it has, which is the
// content_width-disagrees-with-the-buffer bug in fault 2 above, arrived at by
// a different road. Refusing means win_create() returns -1 and the app finds
// out, which is what an app asking for a 9000x6000 window deserves.

/// Minimum sane dimension. A zero or negative dimension is a caller bug, and a
/// 1x1 window is legal (nochrome panels really are that small during layout).
pub const WINBUF_MIN_DIM: i32 = 1;
/// Maximum dimension on a side, in pixels.
pub const WINBUF_MAX_DIM: i32 = 16384;
/// Maximum total content-buffer size, in bytes.
pub const WINBUF_MAX_BYTES: u64 = 64 * 1024 * 1024;

/// Decide whether a `w` x `h` 32-bpp content buffer is allowed, and if so how
/// many bytes it needs.
///
/// Returns 1 and writes the byte count to `out_bytes` when the geometry is
/// acceptable; returns 0 and writes 0 otherwise. The byte count is computed in
/// u64 throughout, so the caller never performs the multiply that overflowed.
///
/// `out_bytes` is always written when non-null, including on rejection, so a
/// caller that forgets to check the return value allocates nothing rather than
/// allocating from an uninitialised size.
#[no_mangle]
pub extern "C" fn winbuf_bytes_rs(w: i32, h: i32, out_bytes: *mut u64) -> i32 {
    if !out_bytes.is_null() {
        unsafe { *out_bytes = 0 };
    }
    if w < WINBUF_MIN_DIM || h < WINBUF_MIN_DIM {
        return 0;
    }
    if w > WINBUF_MAX_DIM || h > WINBUF_MAX_DIM {
        return 0;
    }
    // Both operands are already known to be in 1..=16384, so this cannot
    // overflow u64 and needs no checked_mul theatre; the widening is the point.
    let bytes = (w as u64) * (h as u64) * 4u64;
    if bytes > WINBUF_MAX_BYTES {
        return 0;
    }
    if !out_bytes.is_null() {
        unsafe { *out_bytes = bytes };
    }
    1
}

/// The same policy applied to the OUTER window geometry an app asks
/// win_create() for, before window_create() has run and before the content
/// rectangle is known. The content rectangle is always SMALLER than the outer
/// one (title bar + borders come off it), so accepting the outer size here can
/// never let a content size through that `winbuf_bytes_rs` would reject.
///
/// Returns 1 if the request is worth attempting, 0 to refuse it outright.
#[no_mangle]
pub extern "C" fn winbuf_geom_ok_rs(w: i32, h: i32) -> i32 {
    let mut bytes: u64 = 0;
    winbuf_bytes_rs(w, h, &mut bytes as *mut u64)
}

// ===========================================================================
// #221 terminal notifications: ONE window's state, for its OWN process.
//
// New kernel code, so Rust (2026-07-16 rule). Only the POLICY is here: the
// C caller (proc/syscall.c sys_win_get_state) does the user_windows[] slot
// lookup, because that table is a C static in syscall.c with no FFI surface
// and mirroring it into Rust would be more new surface than the feature.
// This is the same split winbuf_geom_ok_rs above already uses.
//
// WHY THIS EXISTS AT ALL. A MayteraOS app could not discover that it was
// minimized, or even that it was unfocused: the kernel emits no focus/blur/
// minimize event to an app (EVENT_WINDOW_FOCUS and EVENT_WINDOW_BLUR each
// appear exactly once in the kernel tree and it is the enum declaration), and
// while sys_wm_get_windows() reports `minimized` per window, an app cannot
// find ITS OWN row: win_create() hands back the user_windows[] SLOT INDEX and
// wm_window_info_t.id is the window manager's window id, with no mapping
// between them. So every window's state was legible to everyone except that
// window's own process.
//
// The mapping is deliberately NOT the identity function. WINDOW_FLAG_* is a
// kernel-internal layout that has grown four times (NOCHROME, SHADOW,
// FULLSCREEN, MAXIMIZED were all appended); WIN_STATE_* is a small stable ABI
// of the four bits a window's owner has any business knowing about. Handing
// Ring 3 the raw flag word would publish DRAGGING/RESIZING/NOCHROME as ABI by
// accident and pin the internal bit positions forever.
const WF_VISIBLE:   u32 = 1 << 0;
const WF_FOCUSED:   u32 = 1 << 1;
const WF_MINIMIZED: u32 = 1 << 7;
const WF_MAXIMIZED: u32 = 1 << 8;

const WIN_STATE_VISIBLE:   u32 = 0x01;
const WIN_STATE_MINIMIZED: u32 = 0x02;
const WIN_STATE_FOCUSED:   u32 = 0x04;
const WIN_STATE_MAXIMIZED: u32 = 0x08;

/// Map one window's WINDOW_FLAG_* word onto the WIN_STATE_* bitmask userland
/// sees. Total, pure, cannot fail; every other bit in `flags` is dropped.
#[no_mangle]
pub extern "C" fn winstate_bits_rs(flags: u32) -> u32 {
    let mut out = 0u32;
    if flags & WF_VISIBLE   != 0 { out |= WIN_STATE_VISIBLE; }
    if flags & WF_MINIMIZED != 0 { out |= WIN_STATE_MINIMIZED; }
    if flags & WF_FOCUSED   != 0 { out |= WIN_STATE_FOCUSED; }
    if flags & WF_MAXIMIZED != 0 { out |= WIN_STATE_MAXIMIZED; }
    out
}
