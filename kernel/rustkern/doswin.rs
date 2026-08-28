// rustkern/doswin.rs - #745 (local 105): where a DOS guest's fixed-resolution
// screen goes inside a host window that the user can resize.
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
// The INPUTS come from existing C (the host window's content size, in
// proc/syscall.c's user_window_t) and the DECISION - how big the guest picture
// is and where it sits - lives here. The pixel loops stay in C: they read
// dos_task_t's private guest memory (VGA RAM, the four EGA planes, the DAC and
// attribute-controller tables) and are the measured hot path, so exporting
// their state across the FFI to move a rectangle would add the risk this rule
// exists to remove.
//
// ===========================================================================
// THE DEFECT THIS REMOVES, MEASURED ON dev (commit 19a48fd, kernel build 1873)
// ---------------------------------------------------------------------------
// A DOS host window was created at exactly 640x400 of CONTENT and every present
// path scaled the guest's screen into `t->win_w x t->win_h`, which were latched
// ONCE at creation. Maximising the window made the kernel WM reallocate the
// window's content buffer to the new size and FREE the old one, and nothing
// told the DOS layer, so:
//   - the visible picture stayed 640x400 in the top-left corner of a 1276x740
//     window, frozen (MEASURED: 0 changed pixels over 8 s), which is the
//     user-visible half of the report, and
//   - the interpreter went on writing 1 MiB of ARGB per frame, ~70 times a
//     second, into the FREED block. MEASURED twice (golden 1874 and a local
//     build of its commit): within seconds of the maximise the machine died
//     with CPU#0 spinning inside heap_acquire_lock (mm/heap.c:61) with
//     interrupts off, no serial output, no timer tick, guest instruction
//     counter frozen. A use-after-free that lands on the allocator's own free
//     list does not corrupt one window, it stops the computer.
//
// ===========================================================================
// THE ASPECT POLICY, STATED RATHER THAN IMPLIED
// ---------------------------------------------------------------------------
// LETTERBOX, never stretch-to-fill. The host presents ONE logical screen of
// DOS_SURF_W x DOS_SURF_H = 640x400 (8:5), which is exactly the content size a
// DOS window has always been created at, and every video mode is scaled into
// that logical screen exactly as before. So at the default window size the
// picture is bit-for-bit what shipped, and at any other size it is the same
// picture scaled uniformly, centred, with the leftover margin painted black
// (a DOS screen's border was black on the CRT this is imitating).
//
// Stretch-to-fill was the alternative and is rejected: a maximised 16:10 or
// 16:9 desktop would render every 320x200 game noticeably wide, and the aspect
// error would depend on the user's monitor, which is not a property a game's
// artwork should have.
//
// Two consequences worth naming:
//   - This is orientation-agnostic. It maps a source rectangle into a
//     destination rectangle and knows nothing about screen orientation, so a
//     display-rotation feature that rotates the composed output (or reports a
//     rotated content size) needs no change here.
//   - The same function serves the DRAW and the INPUT paths. A scaled picture
//     with unscaled mouse input is worse than no scaling, and the only way to
//     be sure they agree is for both to call this.

// Mirrored by `dos_rect_t` in dos/dosexec.c with a _Static_assert on the size.
#[repr(C)]
pub struct DosRect {
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
}

// Fit an `aw:ah` rectangle inside `cw x ch`, centred, without ever exceeding it.
//
// Integer only (the kernel is soft-float with SSE disabled), i64 intermediates
// so a large window cannot overflow the multiply, and both candidate fits are
// computed rather than one being derived by division from the other: deriving
// h from a truncated w drifts by a pixel and the drift is what puts a one-pixel
// stripe of stale buffer at the bottom of the picture.
//
// Returns 1 and fills `out` when the result is a usable rectangle, 0 otherwise
// (and then `out` is left zeroed, so a caller that ignores the return value
// draws nothing rather than drawing out of bounds).
#[no_mangle]
pub extern "C" fn dos_letterbox_rs(cw: i32, ch: i32, aw: i32, ah: i32, out: *mut DosRect) -> i32 {
    if out.is_null() {
        return 0;
    }
    // SAFETY: `out` is non-null and the C caller passes the address of a
    // `dos_rect_t`, whose layout is locked to DosRect by a _Static_assert on
    // its size in dos/dosexec.c. Nothing else is dereferenced.
    let r = unsafe { &mut *out };
    r.x = 0;
    r.y = 0;
    r.w = 0;
    r.h = 0;
    if cw <= 0 || ch <= 0 || aw <= 0 || ah <= 0 {
        return 0;
    }

    let (cw64, ch64) = (cw as i64, ch as i64);
    let (aw64, ah64) = (aw as i64, ah as i64);

    // Candidate A: use the full width. Candidate B: use the full height. The
    // correct answer is whichever one fits inside the other dimension.
    let mut w = cw64;
    let mut h = cw64 * ah64 / aw64;
    if h > ch64 {
        h = ch64;
        w = ch64 * aw64 / ah64;
    }
    if w < 1 {
        w = 1;
    }
    if h < 1 {
        h = 1;
    }
    if w > cw64 {
        w = cw64;
    }
    if h > ch64 {
        h = ch64;
    }

    r.w = w as i32;
    r.h = h as i32;
    r.x = ((cw64 - w) / 2) as i32;
    r.y = ((ch64 - h) / 2) as i32;
    1
}

// Self-test. Called once from the DOS layer at guest launch; it costs a few
// dozen integer operations and it is the difference between a geometry rule
// that is asserted and one that has been watched being right. Returns the
// number of FAILING cases, so 0 is the only good answer.
#[no_mangle]
pub extern "C" fn dos_letterbox_selftest_rs() -> i32 {
    let mut bad = 0;
    let mut r = DosRect { x: 0, y: 0, w: 0, h: 0 };

    // The size a DOS window is created at: exact fit, no bars, no offset. This
    // is the case that proves the change is a no-op for the shipped default.
    dos_letterbox_rs(640, 400, 640, 400, &mut r);
    if r.x != 0 || r.y != 0 || r.w != 640 || r.h != 400 {
        bad += 1;
    }

    // Maximised on the 1280x800 verification VM: content 1276x740 is TALLER
    // than 8:5, so the fit is height-bound and the bars are on the left/right.
    dos_letterbox_rs(1276, 740, 640, 400, &mut r);
    if r.w != 1184 || r.h != 740 || r.x != 46 || r.y != 0 {
        bad += 1;
    }

    // Wider than 8:5: the fit is width-bound, bars top and bottom.
    dos_letterbox_rs(1600, 400, 640, 400, &mut r);
    if r.w != 640 || r.h != 400 || r.x != 480 || r.y != 0 {
        bad += 1;
    }

    // The result never exceeds the container, and is never empty, at sizes
    // right down to the window manager's minimum.
    let sizes: [(i32, i32); 6] = [(1, 1), (2, 100), (100, 2), (100, 50), (321, 201), (3840, 2160)];
    for (cw, ch) in sizes.iter() {
        if dos_letterbox_rs(*cw, *ch, 640, 400, &mut r) != 1 {
            bad += 1;
            continue;
        }
        if r.w < 1 || r.h < 1 || r.w > *cw || r.h > *ch {
            bad += 1;
        }
        if r.x < 0 || r.y < 0 || r.x + r.w > *cw || r.y + r.h > *ch {
            bad += 1;
        }
    }

    // Degenerate inputs must refuse, not divide by zero or return a rectangle.
    if dos_letterbox_rs(0, 400, 640, 400, &mut r) != 0 { bad += 1; }
    if dos_letterbox_rs(640, 0, 640, 400, &mut r) != 0 { bad += 1; }
    if dos_letterbox_rs(640, 400, 0, 400, &mut r) != 0 { bad += 1; }
    if dos_letterbox_rs(640, 400, 640, 0, &mut r) != 0 { bad += 1; }
    if dos_letterbox_rs(640, 400, 640, 400, core::ptr::null_mut()) != 0 { bad += 1; }

    bad
}

// ===========================================================================
// #163: HOW MANY ROWS THE DISPLAY ACTUALLY SHOWS, WHICH IS NOT THE BIOS MODE'S
// NOMINAL HEIGHT
// ---------------------------------------------------------------------------
// Every presenter in dos/dosexec.c scales `gfx_h` rows into the host picture,
// where gfx_h is a constant chosen by the INT 10h mode number: 200 for 0Dh, 350
// for 10h, 480 for 12h. That is right until a program reprograms the CRTC's
// vertical timing, and the program that opened this ticket does.
//
// MEASURED, The Incredible Machine, build 1930-1932, mode 12h (NOT mode 13h;
// the ticket's premise was wrong and this is what tracing it showed):
//   - it leaves Vertical Display End alone at 479, and writes START VERTICAL
//     BLANK = 399, which is how a program gets a 640x400 picture out of a
//     640x480 mode;
//   - it then page-flips the display start between byte 0 and byte 33280.
// 33280 + 400*80 = 65280 fits one 64 KB plane EXACTLY. 480 rows never could:
// the fetch ran off the end of the plane, wrapped to address 0, and drew the
// TOP of the screen at the BOTTOM of the screen, in a band about a sixth of
// the height. That is the user's report of #163, verbatim: "the top of the
// screen is duplicated at the bottom of the screen about 1/10th of the top of
// the screen duplicated at the bottom".
//
// THREE REGISTER GROUPS, none of which the shared decode read:
//   Vertical Display End   CRTC 0x12, bit 8 in CRTC 0x07 bit 1, bit 9 in bit 6
//   Start Vertical Blank   CRTC 0x15, bit 8 in CRTC 0x07 bit 3, bit 9 in CRTC
//                          0x09 bit 5. Blanking beats display-enable on real
//                          hardware, so it can END THE PICTURE EARLIER.
//   Scanlines per row      CRTC 0x09 bits 0-4 plus one, doubled again when the
//                          scan-doubling bit (0x09 bit 7) is set. Both of the
//                          above count SCANLINES; a presenter indexes ROWS.
//
// WHY THIS IS SAFE FOR EVERYTHING THAT ALREADY WORKS, and it is the only reason
// it is worth doing: for each of the five IBM BIOS mode tables this returns the
// mode's nominal height EXACTLY, which the self-test below checks one by one.
// A program that does not touch the CRTC therefore sees the same picture it saw
// before, to the row. Only a program that deliberately reprogrammed the display
// gets a different answer, and for that program the different answer is the
// correct one.
//
// New logic with no C twin, so Rust per the 2026-07-16 rule. It is called once
// per frame from dos_vga_decode_geom(); the per-pixel loops stay in C for the
// reasons already argued above dos_letterbox_rs.

/// Displayed rows for the current CRTC programming, or 0 for "cannot be
/// derived, keep the mode's nominal height".
///
/// `crtc` is the guest CRTC register file and `ncrtc` its length, so a shrunk
/// array cannot be read past.
#[no_mangle]
pub extern "C" fn dos_vga_rows_rs(crtc: *const u8, ncrtc: u32) -> u32 {
    if crtc.is_null() || ncrtc < 0x19 {
        return 0;
    }
    // SAFETY: `crtc` is non-null and points at `ncrtc` >= 0x19 readable bytes
    // (the caller passes dos_task_t::crtc and sizeof it); every index is <= 0x18.
    let r = |i: usize| -> u32 { unsafe { *crtc.add(i) as u32 } };

    let mut sl_per_row = (r(0x09) & 0x1F) + 1;
    if (r(0x09) & 0x80) != 0 {
        sl_per_row *= 2;
    }
    if sl_per_row == 0 {
        sl_per_row = 1;
    }

    let vde = r(0x12) | ((r(0x07) & 0x02) << 7) | ((r(0x07) & 0x40) << 3);
    let svb = r(0x15) | ((r(0x07) & 0x08) << 5) | ((r(0x09) & 0x20) << 4);

    let mut lines = vde + 1;
    // START VERTICAL BLANK IS TAKEN AS THE LAST DISPLAYED LINE, not the first
    // blanked one, so the height is svb+1. The strict reading of the register is
    // one line fewer; the reason to prefer this one is arithmetic rather than
    // taste. TIM writes 399 with its second page at byte 33280, and
    // 33280 + 400*80 = 65280 is the last byte of a 64 KB plane exactly, while
    // 399 rows would leave 80 bytes stranded for no reason. The bottom scanline
    // of a CRT is also the least reliable thing on it, which is why programs of
    // the era are not careful about this edge. Either reading leaves all five
    // BIOS tables untouched, because each puts SVB comfortably ABOVE its own
    // Vertical Display End (487 vs 480, 406 vs 400, 355 vs 350).
    if svb > 0 && svb + 1 < lines {
        lines = svb + 1;
    }

    let rows = lines / sl_per_row;
    // Refuse an absurd answer rather than hand a presenter a height it would
    // read display memory far outside of; the caller then keeps gfx_h.
    if rows >= 32 && rows <= 2048 { rows } else { 0 }
}

/// Self-test, run once at guest launch beside the letterbox one. Returns the
/// number of FAILING cases, so 0 is the only good answer.
#[no_mangle]
pub extern "C" fn dos_vga_rows_selftest_rs() -> i32 {
    let mut bad = 0;

    // EVERY IBM BIOS MODE TABLE MUST REPRODUCE ITS MODE'S NOMINAL HEIGHT. This
    // is the property that makes deriving the height safe rather than
    // adventurous: no program that leaves the CRTC alone can see a change.
    // (CRTC 0x07, 0x09, 0x12, 0x15, expected rows)
    let tables: [(u8, u8, u8, u8, u32); 5] = [
        (0xBF, 0xC0, 0x8F, 0x96, 200),   // 0Dh 320x200 planar, scan-doubled
        (0xBF, 0xC0, 0x8F, 0x96, 200),   // 0Eh 640x200 planar, scan-doubled
        (0x1F, 0x40, 0x5D, 0x63, 350),   // 10h 640x350 planar
        (0x3E, 0x40, 0xDF, 0xE7, 480),   // 12h 640x480 planar
        (0x1F, 0x41, 0x8F, 0x96, 200),   // 13h 320x200 chained, 2 scanlines/row
    ];
    for (c07, c09, c12, c15, want) in tables.iter() {
        let mut t = [0u8; 32];
        t[0x07] = *c07;
        t[0x09] = *c09;
        t[0x12] = *c12;
        t[0x15] = *c15;
        t[0x18] = 0xFF;
        if dos_vga_rows_rs(t.as_ptr(), 32) != *want {
            bad += 1;
        }
    }

    // THE INCREDIBLE MACHINE, measured on build 1930 once the CRTC readback
    // stopped lying to it: the BIOS mode 12h table with Start Vertical Blank
    // moved to 399. 400 is the only height for which its second page at byte
    // 33280 fits inside one 64 KB plane; 480 is what wrapped the fetch back to
    // address 0 and repeated the top of the screen at the bottom. If this case
    // ever answers 480 again, the band is back.
    let mut tim = [0u8; 32];
    tim[0x07] = 0x3E;
    tim[0x09] = 0x40;
    tim[0x12] = 0xDF;    // VDE 479, untouched by the game
    tim[0x15] = 0x8F;    // SVB 0x8F | 0x100 = 399
    tim[0x18] = 0xFF;
    let rows = dos_vga_rows_rs(tim.as_ptr(), 32);
    if rows != 400 { bad += 1; }
    if 33280u32 + rows * 80 > 0x10000 { bad += 1; }   // must fit one plane

    // Mode X 320x240: the classic tweak is Vertical Display End 479 with two
    // scanlines per row, i.e. 240 rows. Checked because #740's Mode X presenter
    // shares this decode.
    let mut mx = [0u8; 32];
    mx[0x07] = 0x3E;
    mx[0x09] = 0x41;     // 2 scanlines per row
    mx[0x12] = 0xDF;
    mx[0x15] = 0xE7;
    mx[0x18] = 0xFF;
    if dos_vga_rows_rs(mx.as_ptr(), 32) != 240 { bad += 1; }

    // An unprogrammed or nonsensical register file must answer 0 ("keep gfx_h"),
    // never a tiny height that would crop the picture to nothing.
    let z = [0u8; 32];
    if dos_vga_rows_rs(z.as_ptr(), 32) != 0 { bad += 1; }
    if dos_vga_rows_rs(core::ptr::null(), 32) != 0 { bad += 1; }
    if dos_vga_rows_rs(z.as_ptr(), 8) != 0 { bad += 1; }

    bad
}
