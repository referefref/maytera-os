// rustkern/winblit.rs - #blitguard: the geometry contract of sys_win_blit().
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule
// (the same shape and the same justification as rustkern/winbuf.rs next door: a
// pure arithmetic decision, no allocation, no I/O, one C caller).
//
// ===========================================================================
// THE DEFECT THIS REMOVES (#blitguard, read out of dev at 6a6731f2)
// ---------------------------------------------------------------------------
// proc/syscall.c sys_win_blit() scaled a Ring-3 pixel buffer into a window with
//
//     for (dy...) { int sy = (dy * scale_y_fp) >> 8;
//                   if (sy >= src_h) sy = src_h - 1;
//                   uint32_t *src_row = src_buffer + sy * src_w;
//                   for (dx...) dst_row[dx] = src_row[sx]; }
//
// and src_w / src_h come STRAIGHT FROM RING 3: the dispatcher unpacks them as
// (arg4 & 0xFFFF) and ((arg4 >> 16) & 0xFFFF) and hands them over unchecked.
//
//  1. THE NEGATIVE-ROW READ. Nothing rejected src_h == 0. With src_h == 0,
//     scale_y_fp is 0, so sy is 0 for every destination row, and the clamp
//     "if (sy >= src_h) sy = src_h - 1" then drives sy to MINUS ONE. The row
//     pointer becomes (src_buffer - src_w), so the kernel reads src_w pixels
//     from BEFORE the buffer Ring 3 named, up to 65535 pixels or 256 KiB, and
//     paints them into a window the calling app can see.
//
//  2. AND THE VALIDATOR COULD NOT SEE IT. The #503 dispatcher choke point does
//     validate this pointer, through the descriptor
//     "Desc { num: 35, args: [.., sx(rp16(4,4)), ..] }" in rustkern/argtab.rs,
//     which computes src_w * src_h * 4 bytes. When src_h is 0 that product is
//     ZERO, and syscall_validate_args() skips a zero-length range by design:
//     "A zero-length buffer is dereferenced by nobody." That comment is true of
//     every other descriptor in the table and was false of exactly this one. So
//     arg4 = 0 with arg5 = any address at all reached the loop above with NO
//     validation performed. The kernel is identity mapped, so arg5 = 0x400000
//     is kernel text and the window comes back painted with the dword there; an
//     unmapped arg5 is a Ring-0 page fault instead.
//
//     The lesson is NOT "make the validator paranoid". A validator can only
//     promise things about the range the table DECLARES. The fix is to make the
//     handler read only what it declared, which is what the per-row
//     copy_from_user bounce in the caller now does, and to refuse the
//     degenerate geometry here so the clamp can never produce a negative row.
//
//  3. THE FAILED-GROW OVERFLOW. sys_win_blit sizes its destination from the
//     window's content RECTANGLE and, when that is larger than the current
//     content buffer, tries to grow the buffer. When that kmalloc FAILED it
//     carried on regardless with the large dst_w/dst_h and the small buffer, so
//     the write loop ran off the end of a kernel heap allocation. Refusal is
//     the wrong answer for that one: the plan CLAMPS the destination to the
//     buffer that actually exists, so a failed grow degrades to a partial paint
//     instead of corrupting the heap.
//
// ===========================================================================
// THE POLICY, STATED RATHER THAN IMPLIED
// ---------------------------------------------------------------------------
// A source image with a zero or negative side is not a small blit, it is not a
// blit at all: there are no pixels to read. Refuse it, the way winbuf.rs
// refuses an impossible window rather than clamping it, because a clamp would
// invent a geometry the app never asked for and the app would never find out.
//
// The maximum side is WINBUF_MAX_DIM, SHARED with winbuf.rs rather than
// redeclared, so the source of a blit and the buffer it lands in cannot drift
// into two different ideas of "too big".
use crate::winbuf::WINBUF_MAX_DIM;

/// The plan is usable.
pub const WINBLIT_OK: i32 = 0;
/// src_w or src_h was < 1. THE #blitguard hole: src_h == 0 drove sy to -1.
pub const WINBLIT_REJ_SRC_DEGENERATE: i32 = 1;
/// src_w or src_h exceeded WINBUF_MAX_DIM.
pub const WINBLIT_REJ_SRC_HUGE: i32 = 2;
/// The window has no usable content buffer to land in.
pub const WINBLIT_REJ_DST_EMPTY: i32 = 3;

/// Everything the C caller needs in order to run the copy, decided once, up
/// front, before a single user byte is touched.
///
/// #[repr(C)], and locked to the C view by a _Static_assert on sizeof in
/// proc/syscall.c, per the established FFI rule.
#[repr(C)]
pub struct WinBlitPlan {
    /// Source dimensions, after refusal of the degenerate cases. Always >= 1.
    pub src_w: i32,
    pub src_h: i32,
    /// Destination extent, CLAMPED to the content buffer that actually exists.
    pub dst_w: i32,
    pub dst_h: i32,
    /// Elements between the start of one destination row and the next.
    pub dst_stride: i32,
    /// 8.8 fixed-point source step per destination pixel / scanline.
    pub scale_x_fp: i32,
    pub scale_y_fp: i32,
    /// 1 when src_w == dst_w, i.e. no horizontal resampling is needed and a
    /// source row can be copied STRAIGHT into the destination row with no
    /// bounce buffer and no per-pixel loop at all. This is the overwhelmingly
    /// common case (an app blitting a buffer it sized to its own window), and
    /// it is why bouncing the copy does not cost a second pass over the frame.
    pub one_to_one: i32,
    /// Bytes in one source row, src_w * 4. The bounce allocation is O(width),
    /// never O(width*height): the #137/#567 rule, kept.
    pub row_bytes: u64,
    /// WINBLIT_OK, or a WINBLIT_REJ_* code.
    pub reason: i32,
}

/// Decide the whole blit up front, from Ring-3 geometry plus the window's real
/// content buffer. Returns 1 and fills `out` on success; returns 0 and fills
/// `out.reason` with the refusal code otherwise.
///
/// Total and pure: no allocation, and it dereferences nothing but `out`.
#[no_mangle]
pub extern "C" fn winblit_plan_rs(
    src_w: i32,
    src_h: i32,
    content_rect_w: i32,
    content_rect_h: i32,
    buf_w: i32,
    buf_h: i32,
    out: *mut WinBlitPlan,
) -> i32 {
    if out.is_null() {
        return 0;
    }
    // SAFETY: `out` is non-null and the C caller passes the address of a stack
    // WinBlitPlan whose size is _Static_assert-locked against this struct.
    let p = unsafe { &mut *out };
    // Written on EVERY path including refusal, so a caller that ignores the
    // return value copies nothing rather than copying from uninitialised
    // geometry. The same discipline as winbuf_bytes_rs's out_bytes.
    p.src_w = 0;
    p.src_h = 0;
    p.dst_w = 0;
    p.dst_h = 0;
    p.dst_stride = 0;
    p.scale_x_fp = 0;
    p.scale_y_fp = 0;
    p.one_to_one = 0;
    p.row_bytes = 0;
    p.reason = WINBLIT_REJ_SRC_DEGENERATE;

    // THE FIX for fault 1. A side of zero or less has no pixels, and there is
    // no geometry to clamp it to that the app would recognise.
    if src_w < 1 || src_h < 1 {
        p.reason = WINBLIT_REJ_SRC_DEGENERATE;
        return 0;
    }
    if src_w > WINBUF_MAX_DIM || src_h > WINBUF_MAX_DIM {
        p.reason = WINBLIT_REJ_SRC_HUGE;
        return 0;
    }
    if buf_w < 1 || buf_h < 1 {
        p.reason = WINBLIT_REJ_DST_EMPTY;
        return 0;
    }

    // THE FIX for fault 3. The destination is the content RECTANGLE, clamped to
    // the BUFFER, because those two disagree exactly when a grow has failed.
    let mut dst_w = content_rect_w;
    let mut dst_h = content_rect_h;
    if dst_w < 1 {
        dst_w = 1;
    }
    if dst_h < 1 {
        dst_h = 1;
    }
    if dst_w > buf_w {
        dst_w = buf_w;
    }
    if dst_h > buf_h {
        dst_h = buf_h;
    }

    // src_w and src_h are now in 1..=16384, so "<< 8" is at most 4_194_304 and
    // the divisor is at least 1: neither the shift nor the divide can wrap or
    // trap. This is the arithmetic the C did, with the operand ranges now
    // established rather than hoped for.
    p.src_w = src_w;
    p.src_h = src_h;
    p.dst_w = dst_w;
    p.dst_h = dst_h;
    p.dst_stride = buf_w;
    p.scale_x_fp = (src_w << 8) / dst_w;
    p.scale_y_fp = (src_h << 8) / dst_h;
    p.one_to_one = if src_w == dst_w { 1 } else { 0 };
    p.row_bytes = (src_w as u64) * 4u64;
    p.reason = WINBLIT_OK;
    1
}

/// Resample ONE row, nearest-neighbour, from a KERNEL-resident source row into
/// a KERNEL-resident destination row.
///
/// This is the inner loop of the scaling path. It never sees a user pointer:
/// the caller has already bounced the source row into kernel memory with
/// copy_from_user, which is the whole point of the #blitguard change. Keeping
/// it here rather than in C is the 2026-07-16 rule applied to the one piece of
/// this function that is genuinely hot; whether that costs anything is
/// MEASURED by winblit_bench_report() in proc/syscall.c, not asserted.
///
/// # Safety
/// `dst` must be writable for `dst_w` u32s and `src` readable for `src_w` u32s,
/// both in kernel memory. `dst_w` and `src_w` must be >= 1. The caller gets all
/// three from a WinBlitPlan that `winblit_plan_rs` accepted.
#[no_mangle]
pub unsafe extern "C" fn winblit_scale_row_rs(
    dst: *mut u32,
    dst_w: i32,
    src: *const u32,
    src_w: i32,
    scale_x_fp: i32,
) {
    if dst.is_null() || src.is_null() || dst_w < 1 || src_w < 1 {
        return;
    }
    let mut dx: i32 = 0;
    while dx < dst_w {
        let mut sx = (dx.wrapping_mul(scale_x_fp)) >> 8;
        if sx >= src_w {
            sx = src_w - 1;
        }
        if sx < 0 {
            sx = 0;
        }
        // SAFETY: sx is clamped to 0..=src_w-1 and dx < dst_w, per the contract
        // above. Both pointers are kernel-resident.
        *dst.offset(dx as isize) = *src.offset(sx as isize);
        dx += 1;
    }
}
