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

// ===========================================================================
// (#dosfs) HOW BIG THE PICTURE SHOULD BE, WHICH IS NOT THE SAME QUESTION AS
// WHERE IT SITS
// ---------------------------------------------------------------------------
// dos_letterbox_rs() above answers "where", and until now that was the whole
// policy: the picture was always the largest aspect-correct rectangle that fit
// the content area. On a 1280x800 window that is 1.0 megapixel and nobody
// noticed. On a 3840x2160 panel it is 8.3 MEGAPIXELS, thirty-three megabytes of
// ARGB, written by a per-destination-pixel scaler seventy times a second, on a
// machine that currently runs ONE core (g_smp_user_sched = 0). Then the
// compositor reads all of it and writes all of it again to a write-combining
// framebuffer. Three passes over 8.3 Mpx per frame is not a rendering cost, it
// is the whole machine.
//
// The owner asked for DOS games to open "full screen ... using a 640x480 or
// 800x600 resolution, maybe even 1024x768 for Red Alert era games". A REAL mode
// change cannot deliver that: UEFI GOP SetMode is gone after ExitBootServices,
// so the only place a real mode is chosen is the bootloader, and that changes
// the whole desktop rather than one app. What CAN deliver it is presentation
// scaling: draw the guest into an era-sized region of the framebuffer we
// already have, and paint the surround once instead of every frame.
//
// THE HONEST TRADE, STATED SO NOBODY HAS TO REDISCOVER IT. On a framebuffer with
// no display controller of our own, covering every pixel of a 4K panel COSTS
// 8.3 Mpx per frame no matter WHERE the scaling happens - moving it into the
// compositor's blit moves the cost, it does not remove it. So a cheaper frame
// necessarily means a picture SMALLER than the panel, with a black surround.
// That is the same bargain the original hardware struck (a 320x200 signal on a
// 4:3 CRT was not covering a 4K panel either), and it is the bargain this
// policy makes explicit and configurable rather than implicit and fixed.
//
// THE POLICY, in the order the decisions are made:
//
//   1. A PIXEL BUDGET, not a resolution. The knob is the number of DESTINATION
//      pixels the picture may occupy, because that number IS the per-frame
//      cost; a resolution is only a proxy for it. Default DOS_VIEW_BUDGET_PX.
//
//   2. UNDER BUDGET => UNCHANGED. If the full aspect fit already costs less
//      than the budget, it is used verbatim: dos_present_rect_rs() returns
//      exactly what dos_letterbox_rs() returns. Every window size that ships
//      today (a 640x400 default window, a maximised 1280x800 desktop at
//      1235x772 = 0.95 Mpx) is under budget, so this change is a pixel-for-
//      pixel no-op there and can only alter the case it was written for.
//
//   3. OVER BUDGET => INTEGER SCALE. Pick the largest whole-number multiple of
//      the guest's own resolution that fits both the window and the budget.
//      Integer scaling is chosen over a resampled fit for two reasons that
//      point the same way: each destination pixel is a plain copy of one
//      source pixel (sharp, era-correct, no blur an artist never drew), and it
//      is the cheapest thing the scaler can do. It is deliberately NOT applied
//      under budget, because at small sizes it would SHRINK a picture that is
//      currently correct, to buy speed that is not needed.
//
//   4. THE ASPECT BOX STILL WINS on height. The height comes from the (aw,ah)
//      box that dos_present_aspect() already owns, not from gh directly. Where
//      the box matches the guest's own pixel aspect - mode 13h, Mode X, text,
//      VESA, EGA 640x400 - that is arithmetically identical to gh*n, so those
//      modes get an exact integer scale in BOTH axes. Where it does not (EGA
//      640x350 letterboxed into 8:5), the width is still an exact multiple and
//      only the height is resampled, which is precisely what the CRT did.
//
// WHAT THIS DOES NOT DO. It does not choose a per-title resolution from a table
// of game names. The guest's own video mode already says what era it is from:
// a 320x200 mode-13h title and a 640x480 VESA title want different factors and
// they ASK for them by setting different modes. A per-title table would be a
// second source of truth that goes stale the first time a game switches mode
// mid-run, which many do (menu in text mode, play in 13h). The per-title escape
// hatch that does exist is DATA, not code: /CONFIG/DOSVIEW.CFG.

/// The view policy, mirrored by `dos_view_policy_t` in dos/dosexec.c with a
/// _Static_assert on the size. Every field is an override with a documented
/// "leave it alone" value, so a missing or half-written config file degrades to
/// the defaults rather than to a zero-sized picture.
#[repr(C)]
pub struct DosViewPolicy {
    /// Destination pixels the picture may occupy. <= 0 means UNCAPPED, which
    /// restores the pre-#dosfs behaviour exactly.
    pub budget_px: i32,
    /// 0 = off, plain aspect fit scaled down to whatever limits bind.
    /// 1 = snap to a whole multiple of the guest resolution WHEN A LIMIT BINDS
    ///     (the default). Under every limit the picture is left exactly as
    ///     dos_letterbox_rs produced it, so no window size that ships today
    ///     changes by a pixel.
    /// 2 = ALWAYS snap, even when nothing binds. This is smaller than the
    ///     aspect fit at most window sizes (1235x772 maximised becomes 960x600
    ///     for a 320x200 guest) and it is sharper, which is a taste question
    ///     rather than a performance one, so it is opt-in: `integer=always`.
    pub integer: i32,
    /// Hard per-axis ceiling on the picture, independent of the budget. 0 = no
    /// ceiling. This is how a user asks for "never bigger than 1024x768"
    /// literally, rather than by way of a pixel count.
    pub max_w: i32,
    pub max_h: i32,
    /// 0 = SQUARE PIXELS (the default, and what has always shipped).
    /// 1 = CRT: correct the fixed 8:5 box to 4:3, the shape the original
    ///     hardware actually displayed those modes at.
    ///
    /// THE DECISION, STATED RATHER THAN LEFT IMPLICIT. A 320x200 mode-13h
    /// picture is 8:5 in pixels but was displayed on a 4:3 CRT, so every pixel
    /// was 1.2 times taller than it was wide, and artwork of the era was drawn
    /// to look right THAT WAY. Presenting it square is therefore, strictly, a
    /// 17% vertical squash of what the artist saw.
    ///
    /// The DEFAULT is square anyway, for one reason that outranks the
    /// authenticity argument: square is what MayteraOS has always shown, it is
    /// what every screenshot and every prior verification run in this tree
    /// depicts, and silently changing the shape of every DOS game as a side
    /// effect of a PERFORMANCE change would be indefensible. It is a taste
    /// question, so it gets a switch and not a decree: `aspect=crt`.
    ///
    /// It costs almost nothing either way. The measured cost model (blame.md)
    /// is dominated by a term proportional to destination WIDTH, because
    /// dos_row_reuse() already made repeated rows a memcpy; trading height for
    /// black bars at the same width is close to free. So this can be decided on
    /// looks alone, which is the right way to decide it.
    pub aspect: i32,
    /// 1 = SKIP a frame the compositor has not yet shown (the default),
    /// 0 = present on the fixed DOS_PRESENT_MS cadence regardless, which is
    /// byte-for-byte the behaviour before this field existed.
    ///
    /// It lives in THIS struct, and not in a new one behind a new config file,
    /// because it answers the same question the rest of the struct answers -
    /// what does a DOS guest put on the screen and how much does it cost - and
    /// /CONFIG/DOSVIEW.CFG is already the place a user says so. The logic is in
    /// rustkern/dosdisp.rs; this is only where the switch lives.
    pub frameskip: i32,
}

/// `aspect=crt`: the shape the fixed 8:5 box was actually displayed at.
pub const DOS_ASPECT_SQUARE: i32 = 0;
pub const DOS_ASPECT_CRT: i32 = 1;

/// Correct an (aw, ah) aspect box for the policy. ONLY the fixed 8:5 box is
/// touched: a VESA mode and Mode X already carry their own true aspect (640x480
/// and 320x240 are both 4:3 on square pixels, which is precisely why a game
/// chose them), so forcing 4:3 on those would be a second, wrong correction of
/// something already correct. Called from C's dos_present_aspect(), which is the
/// single source of the box for BOTH the draw path and the input path.
#[no_mangle]
pub extern "C" fn dos_aspect_apply_rs(aspect: i32, aw: *mut i32, ah: *mut i32) -> i32 {
    if aw.is_null() || ah.is_null() || aspect != DOS_ASPECT_CRT {
        return 0;
    }
    // SAFETY: both pointers are non-null and the C caller passes the addresses
    // of the two int32_t it is about to return.
    let (w, h) = unsafe { (&mut *aw, &mut *ah) };
    // Is this the fixed 8:5 box? Cross-multiply rather than compare against
    // 640x400, so a future change to DOS_SURF_W/H cannot quietly stop matching.
    if *w <= 0 || *h <= 0 || (*w as i64) * 5 != (*h as i64) * 8 {
        return 0;
    }
    // Same width, 4:3. Keeping the WIDTH is deliberate: width is what the cost
    // model is proportional to, so this is the free direction, and it is also
    // the direction that adds picture rather than cropping it.
    *h = (*w * 3) / 4;
    1
}

/// The shipped default budget, in destination pixels.
///
/// WHY IT IS A PIXEL COUNT AND NOT A RESOLUTION: no single resolution suits
/// every mode in the DOS corpus, and the destination pixel count IS the
/// scaler's per-frame cost. A larger budget means a larger picture and a
/// slower frame; a smaller one the reverse, and the trade is monotonic, which
/// is the entire reason the knob is shaped this way.
///
/// RAISED FROM 2,400,000 TO 6,500,000 (2026-08-28, dw2perf), AND THE REASON IS
/// THAT THE ASSUMPTION THE OLD NUMBER WAS SIZED AGAINST STOPPED BEING TRUE.
///
/// 2,400,000 was chosen against a present cadence of DOS_PRESENT_MS, i.e. about
/// 70 frames a second, because that is what the run loop did. It is not what
/// the SCREEN did. MEASURED on golden 2259 at a real 2560x1600 framebuffer,
/// Aladdin maximised: the guest presented ~65 frames a second and the
/// framebuffer was presented 13-16 times a second, so four frames in five were
/// scaled and thrown away. rustkern/dosdisp.rs now declines those, and the
/// scaler consequently runs four to six times LESS often, which makes a much
/// larger picture affordable out of the same milliseconds:
///
///   MEASURED, 2560x1600, Aladdin, per present and as a share of one core:
///     1280x800  (old budget)   781 us,   5.2-6.4% of a core at 65 frames/s
///     2288x1430 (new budget)  2071 us,   1.0%     of a core at the real rate
///
///   The bigger picture is the CHEAPER one, because the count of frames fell
///   further than the cost of each frame rose. That is the whole argument.
///
/// WHY 6,000,000 SPECIFICALLY, AND WHY NOT MORE. It is the LARGEST value at
/// which every guest mode in the corpus still SNAPS TO A WHOLE-NUMBER SCALE on
/// a 3840x2160 panel, which is the owner's display and the one all three
/// reports came from:
///
///   guest      n   picture      cost      what it is
///   320x200    9   2880x1800    5.18 Mpx  mode 13h (Aladdin, Prince of Persia)
///   320x240    8   2560x1920    4.92 Mpx  Mode X
///   640x400    4   2560x1600    4.10 Mpx  text, Red Alert's VESA mode
///   640x480    4   2560x1920    4.92 Mpx  mode 12h, VESA 101h (Discworld II)
///   800x600    3   2400x1800    4.32 Mpx  VESA 103h
///   1024x768   2   2048x1536    3.15 Mpx  VESA 105h
///
/// At 2,400,000 every one of those was 1920x1200 or smaller, and at the
/// budget=1300000 on the owner's own medium every 320x200 title was 1280x800:
/// one third of the width of his panel, centred in black. Three separate owner
/// reports of "it does not scale to full screen" were that number, not a bug.
///
/// THE CEILING IS NOT MONOTONIC IN QUALITY, WHICH IS THE TRAP THIS NUMBER IS
/// PICKED AROUND, and the self-test caught me walking into it. Integer snapping
/// applies only WHEN A LIMIT BINDS (see `integer` on DosViewPolicy); under every
/// limit the picture is the plain aspect fit. So a budget set TOO HIGH stops
/// binding and silently turns a sharp whole-number scale back into a resampled
/// one. At 6,500,000 the three 4:3 modes (640x480, 800x600, 1024x768) all stop
/// binding on a 3840x2160 panel: their plain fit is 2880x2160 = 6.22 Mpx, which
/// is bigger AND blurrier than the 2560x1920 they snap to here. Discworld II is
/// one of them. 6,000,000 sits just under that 6.22 Mpx cliff, so every mode
/// still binds and every picture is an exact multiple of the guest's own pixels.
///
/// Someone who would rather have edge-to-edge than sharp can say `budget=off`
/// and get the full 3456x2160 aspect fit with uneven pixel duplication. That is
/// a taste question and it already has a switch, so it is not the default.
///
/// THE NO-OP PROPERTY IS PRESERVED, WHICH IS THE POINT OF RAISING IT RATHER
/// THAN REPLACING THE POLICY. dos_present_rect_rs() returns dos_letterbox_rs()
/// verbatim for anything UNDER the budget, so every picture that was under
/// 2,400,000 is still under 6,500,000 and is still byte-identical. Raising a
/// ceiling can only ever un-cap a case that was capped; it cannot change one
/// that was not.
pub const DOS_VIEW_BUDGET_PX: i32 = 6_000_000;

/// The default, handed to C so the number has ONE definition. blame.md records
/// the #mickey re-home interval that existed as a Rust const AND a mirrored C
/// #define, where the C side assigned its copy over the Rust one: the constant
/// could be changed with no effect at all, and it cost a whole verification run
/// to notice. C reads this; C never spells the number.
#[no_mangle]
pub extern "C" fn dos_view_default_budget_rs() -> i32 { DOS_VIEW_BUDGET_PX }

/// The cap on the OPENING WINDOW, which is a DIFFERENT QUESTION from the budget
/// that governs the picture, and conflating the two was a real defect.
///
/// WHY THEY MUST BE SEPARATE. `dos_run_file()` sizes the window it creates by
/// running this same policy with `integer = 2`, so raising DOS_VIEW_BUDGET_PX
/// to let a MAXIMISED window hold a full-panel picture also, silently, made the
/// window every DOS guest OPENS bigger. MEASURED on a 3840x2160 panel, the
/// opening window against the budget in force:
///
///   before presentation scaling existed   640x400     1.0 MB    1.0x
///   budget = 1,300,000                   1280x800     3.9 MB    4.0x
///   budget = 2,400,000                  1920x1200     8.8 MB    9.0x
///   budget = 6,000,000                  2560x1600    15.6 MB   16.0x
///
/// The buffer size is what the per-frame publish memcpy costs (measured with
/// the kernel's own memcpy_fast: 75 us at 640x400, 2007 us at 2560x1600), so a
/// budget raise aimed at the picture multiplied the DEFAULT case sixteenfold.
/// Nobody asked for a large window; the user asked for a large PICTURE when
/// they maximise. Those are now two numbers.
///
/// Note this is invisible below about 2560 wide: on a 1920x1080 panel the work
/// area binds before any of these budgets does, and the window opens 1280x800
/// at all four values. It is a 4K-only effect, which is exactly why it shipped.
pub const DOS_VIEW_OPEN_BUDGET_PX: i32 = 2_400_000;

/// The budget the OPENING window should use, given the picture budget in force.
///
/// A user who asks for a SMALLER picture gets a smaller window (their number
/// wins); a user who asks for a bigger picture does NOT get a bigger window
/// (this cap wins). Asymmetric on purpose: "make everything smaller" is a
/// request the opening size should honour, and "let the picture fill the panel
/// when I maximise" is not a request about the opening size at all.
#[no_mangle]
pub extern "C" fn dos_view_open_budget_rs(picture_budget: i32) -> i32 {
    if picture_budget > 0 && picture_budget < DOS_VIEW_OPEN_BUDGET_PX {
        picture_budget
    } else {
        DOS_VIEW_OPEN_BUDGET_PX
    }
}

/// Largest integer n >= 1 with (gw*n) x (box height for gw*n) fitting inside
/// `cw x ch`, inside `max_w x max_h`, and inside `budget`. 0 if even n = 1
/// does not fit, in which case the caller falls back to the aspect fit.
fn dos_int_scale(cw: i64, ch: i64, aw: i64, ah: i64, gw: i64,
                 budget: i64, max_w: i64, max_h: i64) -> i64 {
    if gw <= 0 {
        return 0;
    }
    let mut best = 0i64;
    // 64 is far past any real case (320x200 at 64x is 20480 wide) and bounds
    // the loop by construction rather than by trusting the inputs.
    let mut n = 1i64;
    while n <= 64 {
        let w = gw * n;
        if w > cw {
            break;
        }
        // Round to nearest rather than truncating: at 8:5 the exact height is
        // integral anyway, and where it is not, truncation biases the picture
        // one row short every time, which is the pixel stripe of stale buffer
        // dos_letterbox_rs's own comment warns about.
        let h = (w * ah + aw / 2) / aw;
        if h < 1 || h > ch {
            break;
        }
        if max_w > 0 && w > max_w {
            break;
        }
        if max_h > 0 && h > max_h {
            break;
        }
        if budget > 0 && w * h > budget {
            break;
        }
        best = n;
        n += 1;
    }
    best
}

/// The picture rectangle inside a `cw x ch` content buffer: aspect box `aw:ah`,
/// guest resolution `gw x gh`, under `pol`.
///
/// Returns 1 and fills `out` when the result is usable, 0 otherwise (and then
/// `out` is zeroed, so a caller that ignores the return draws nothing rather
/// than out of bounds) - the same contract as dos_letterbox_rs, deliberately,
/// because this is a drop-in replacement for it on both the draw path and the
/// input path and a different contract is how those two come to disagree.
#[no_mangle]
pub extern "C" fn dos_present_rect_rs(cw: i32, ch: i32, aw: i32, ah: i32,
                                      gw: i32, gh: i32,
                                      pol: *const DosViewPolicy,
                                      out: *mut DosRect) -> i32 {
    // The full aspect fit is computed FIRST and unconditionally, so every path
    // below either returns it or returns something derived from the same
    // function. There is no second geometry rule hiding in here.
    if dos_letterbox_rs(cw, ch, aw, ah, out) == 0 {
        return 0;
    }
    if pol.is_null() {
        return 1;
    }
    // SAFETY: `pol` is non-null and the C caller passes the address of a
    // `dos_view_policy_t`, whose layout is locked to DosViewPolicy by a
    // _Static_assert on its size in dos/dosexec.c. Read only.
    let p = unsafe { &*pol };
    // SAFETY: dos_letterbox_rs returned 1, so `out` is non-null and points at
    // a DosRect it has already written.
    let r = unsafe { &mut *out };

    let (cw64, ch64) = (cw as i64, ch as i64);
    let (aw64, ah64) = (aw as i64, ah as i64);
    let budget = p.budget_px as i64;
    let max_w = p.max_w as i64;
    let max_h = p.max_h as i64;

    let over_budget = budget > 0 && (r.w as i64) * (r.h as i64) > budget;
    let over_max = (max_w > 0 && (r.w as i64) > max_w) || (max_h > 0 && (r.h as i64) > max_h);
    if !over_budget && !over_max && p.integer < 2 {
        // THE NO-OP ARM, and it is the one that protects everything that ships:
        // a picture already inside every limit is returned byte-identical to
        // what dos_letterbox_rs alone produced. `integer=always` (2) is the
        // only way past it, and it is opt-in for exactly that reason.
        return 1;
    }

    let mut w;
    let mut h;
    let n = if p.integer != 0 {
        dos_int_scale(cw64, ch64, aw64, ah64, gw as i64, budget, max_w, max_h)
    } else {
        0
    };
    let _ = gh;
    if n > 0 {
        w = (gw as i64) * n;
        h = (w * ah64 + aw64 / 2) / aw64;
    } else {
        // No usable integer scale (the guest resolution alone already exceeds a
        // limit, or integer snapping is off). Shrink the aspect fit until it is
        // inside every limit, by BINARY SEARCH on the width rather than by a
        // square root: the kernel is soft-float with SSE disabled, and an
        // integer sqrt would still need the same clamping afterwards. ~21
        // iterations of two multiplies, once per present, against a scaler that
        // is about to write a million pixels.
        let mut lo = 1i64;
        let mut hi = r.w as i64;
        let fits = |ww: i64| -> bool {
            let hh = (ww * ah64 + aw64 / 2) / aw64;
            if hh < 1 || hh > ch64 || ww > cw64 {
                return false;
            }
            if max_w > 0 && ww > max_w {
                return false;
            }
            if max_h > 0 && hh > max_h {
                return false;
            }
            if budget > 0 && ww * hh > budget {
                return false;
            }
            true
        };
        if !fits(lo) {
            // Even one pixel of width does not satisfy the limits, which means
            // the limits are nonsense (a budget of 0 would have been read as
            // "uncapped"). Keep the full fit: a too-slow picture beats none.
            return 1;
        }
        while lo < hi {
            let mid = lo + (hi - lo + 1) / 2;
            if fits(mid) { lo = mid; } else { hi = mid - 1; }
        }
        w = lo;
        h = (w * ah64 + aw64 / 2) / aw64;
    }

    if w < 1 { w = 1; }
    if h < 1 { h = 1; }
    if w > cw64 { w = cw64; }
    if h > ch64 { h = ch64; }
    r.w = w as i32;
    r.h = h as i32;
    // RE-CENTRE IN THE REAL CONTENT AREA, not in some intermediate box. The
    // surround is the margin of the WINDOW, so a picture centred in anything
    // smaller would sit off-centre on the screen.
    r.x = ((cw64 - w) / 2) as i32;
    r.y = ((ch64 - h) / 2) as i32;
    1
}

// ---------------------------------------------------------------------------
// /CONFIG/DOSVIEW.CFG - the DATA escape hatch.
//
// Lines of `key=value`, '#' to end of line is a comment, whitespace and case
// insensitive on keys. Unknown keys are IGNORED rather than fatal: a config
// written for a later build must not stop a guest launching on an earlier one.
//
//   budget=2400000   destination pixels the picture may occupy; 0 = uncapped
//   budget=off       same as 0, spelled the way a person would write it
//   max=1024x768     hard per-axis ceiling, independent of the budget
//   integer=on|off   snap to whole multiples of the guest resolution
//   frameskip=on|off do not compute a frame the compositor has not shown yet
//                    (default on; see rustkern/dosdisp.rs for the measurement)
//
// Parsed here rather than in C because it is the policy's own file and the
// policy lives here; dos/dosexec.c already carries three hand-rolled inline
// config parsers and a fourth would be the fork the reuse rule forbids.
#[no_mangle]
pub extern "C" fn dos_view_parse_rs(buf: *const u8, len: u32,
                                    pol: *mut DosViewPolicy) -> i32 {
    if pol.is_null() {
        return 0;
    }
    // SAFETY: non-null, and the C caller passes a `dos_view_policy_t` it has
    // already initialised to the defaults. Fields are only overwritten for keys
    // that actually parse.
    let p = unsafe { &mut *pol };
    if buf.is_null() || len == 0 {
        return 0;
    }
    // SAFETY: the C caller passes fat_read_file()'s buffer and its byte count.
    let s = unsafe { core::slice::from_raw_parts(buf, len as usize) };

    let mut changed = 0;
    let mut i = 0usize;
    while i < s.len() {
        // One line.
        let start = i;
        while i < s.len() && s[i] != b'\n' && s[i] != b'\r' {
            i += 1;
        }
        let mut line = &s[start..i];
        while i < s.len() && (s[i] == b'\n' || s[i] == b'\r') {
            i += 1;
        }
        if let Some(c) = line.iter().position(|&b| b == b'#') {
            line = &line[..c];
        }
        let eq = match line.iter().position(|&b| b == b'=') {
            Some(e) => e,
            None => continue,
        };
        let key = trim(&line[..eq]);
        let val = trim(&line[eq + 1..]);
        if key.is_empty() {
            continue;
        }
        if eq_ci(key, b"budget") {
            if eq_ci(val, b"off") || eq_ci(val, b"none") {
                p.budget_px = 0;
                changed += 1;
            } else if let Some(v) = num(val) {
                p.budget_px = clamp_i32(v);
                changed += 1;
            }
        } else if eq_ci(key, b"integer") {
            if eq_ci(val, b"always") || eq_ci(val, b"2") {
                p.integer = 2;
                changed += 1;
            } else if eq_ci(val, b"on") || eq_ci(val, b"1") || eq_ci(val, b"yes") {
                p.integer = 1;
                changed += 1;
            } else if eq_ci(val, b"off") || eq_ci(val, b"0") || eq_ci(val, b"no") {
                p.integer = 0;
                changed += 1;
            }
        } else if eq_ci(key, b"aspect") {
            if eq_ci(val, b"crt") || eq_ci(val, b"4:3") {
                p.aspect = DOS_ASPECT_CRT;
                changed += 1;
            } else if eq_ci(val, b"square") || eq_ci(val, b"1:1") {
                p.aspect = DOS_ASPECT_SQUARE;
                changed += 1;
            }
        } else if eq_ci(key, b"frameskip") {
            if eq_ci(val, b"on") || eq_ci(val, b"1") || eq_ci(val, b"yes") {
                p.frameskip = 1;
                changed += 1;
            } else if eq_ci(val, b"off") || eq_ci(val, b"0") || eq_ci(val, b"no") {
                p.frameskip = 0;
                changed += 1;
            }
        } else if eq_ci(key, b"max") {
            if eq_ci(val, b"off") || eq_ci(val, b"none") {
                p.max_w = 0;
                p.max_h = 0;
                changed += 1;
            } else if let Some(x) = val.iter().position(|&b| b == b'x' || b == b'X') {
                if let (Some(w), Some(h)) = (num(trim(&val[..x])), num(trim(&val[x + 1..]))) {
                    p.max_w = clamp_i32(w);
                    p.max_h = clamp_i32(h);
                    changed += 1;
                }
            }
        }
    }
    changed
}

fn trim(b: &[u8]) -> &[u8] {
    let mut a = 0usize;
    let mut z = b.len();
    while a < z && (b[a] == b' ' || b[a] == b'\t') {
        a += 1;
    }
    while z > a && (b[z - 1] == b' ' || b[z - 1] == b'\t') {
        z -= 1;
    }
    &b[a..z]
}

fn eq_ci(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    for k in 0..a.len() {
        let x = a[k] | 0x20;
        let y = b[k] | 0x20;
        if x != y {
            return false;
        }
    }
    true
}

/// Decimal, saturating rather than wrapping. A config typo of forty digits must
/// clamp to "very large", never wrap to a small or negative budget - a negative
/// budget reads as "uncapped" and would silently undo the whole feature.
fn num(b: &[u8]) -> Option<i64> {
    if b.is_empty() {
        return None;
    }
    let mut v: i64 = 0;
    let mut any = false;
    for &c in b {
        if !(b'0'..=b'9').contains(&c) {
            return None;
        }
        any = true;
        v = v.saturating_mul(10).saturating_add((c - b'0') as i64);
    }
    if any { Some(v) } else { None }
}

fn clamp_i32(v: i64) -> i32 {
    if v > i32::MAX as i64 { i32::MAX } else if v < 0 { 0 } else { v as i32 }
}

/// Self-test for the view policy, run once at guest launch beside the other
/// two. Returns the number of FAILING cases, so 0 is the only good answer.
#[no_mangle]
pub extern "C" fn dos_view_selftest_rs() -> i32 {
    let mut bad = 0;
    let mut r = DosRect { x: 0, y: 0, w: 0, h: 0 };
    let def = DosViewPolicy { budget_px: DOS_VIEW_BUDGET_PX, integer: 1, max_w: 0, max_h: 0, aspect: 0, frameskip: 1 };

    // ---- THE NO-OP CASES. Every window size that ships today must come back
    // bit-for-bit identical to dos_letterbox_rs alone, or this change is a
    // regression dressed as a feature.
    let noop: [(i32, i32); 4] = [(640, 400), (1276, 740), (800, 500), (1600, 400)];
    for (cw, ch) in noop.iter() {
        let mut a = DosRect { x: 0, y: 0, w: 0, h: 0 };
        let mut b = DosRect { x: 0, y: 0, w: 0, h: 0 };
        dos_letterbox_rs(*cw, *ch, 640, 400, &mut a);
        dos_present_rect_rs(*cw, *ch, 640, 400, 320, 200, &def, &mut b);
        if a.x != b.x || a.y != b.y || a.w != b.w || a.h != b.h {
            bad += 1;
        }
    }

    // ---- THE CASE THIS EXISTS FOR. A maximised 3840x2160 window, mode 13h.
    // The plain aspect fit is 3456x2160 = 7.46 Mpx. Wanted: the largest whole
    // multiple of 320x200 inside DOS_VIEW_BUDGET_PX, which at 6.0 Mpx is 9x.
    // These numbers moved when the budget was raised (2026-08-28, dw2perf) and
    // they are written out rather than derived FROM the constant on purpose: a
    // test that recomputes the thing it is testing passes whatever the code
    // does. If you change the budget, change these by hand and check the
    // arithmetic, which is exactly the review the change deserves.
    dos_present_rect_rs(3840, 2160, 640, 400, 320, 200, &def, &mut r);
    if r.w != 2880 || r.h != 1800 { bad += 1; }
    if r.x != 480 || r.y != 180 { bad += 1; }
    if (r.w as i64) * (r.h as i64) > DOS_VIEW_BUDGET_PX as i64 { bad += 1; }
    // AN EXACT INTEGER SCALE IN BOTH AXES, which is the sharpness claim. If
    // this ever fails, the picture is being resampled and the claim is false.
    if r.w % 320 != 0 || r.h % 200 != 0 { bad += 1; }
    if r.w / 320 != r.h / 200 { bad += 1; }

    // Same panel, the other modes in the corpus. Each must be an exact
    // multiple of its own guest resolution and inside the budget.
    let modes: [(i32, i32, i32, i32, i32, i32); 5] = [
        // gw, gh, aw, ah, want_w, want_h
        (320, 240, 320, 240, 2560, 1920),   // Mode X, 4:3, 8x
        (640, 400, 640, 400, 2560, 1600),   // text / Red Alert VESA, 4x
        (640, 480, 640, 480, 2560, 1920),   // mode 12h / VESA 101h, 4x (Discworld II)
        (800, 600, 800, 600, 2400, 1800),   // VESA 103h, 3x
        (1024, 768, 1024, 768, 2048, 1536), // VESA 105h, 2x (height binds)
    ];
    for (gw, gh, aw, ah, ww, wh) in modes.iter() {
        dos_present_rect_rs(3840, 2160, *aw, *ah, *gw, *gh, &def, &mut r);
        if r.w != *ww || r.h != *wh { bad += 1; }
        if r.w % *gw != 0 || r.h % *gh != 0 { bad += 1; }
        if (r.w as i64) * (r.h as i64) > DOS_VIEW_BUDGET_PX as i64 { bad += 1; }
    }

    // ---- ASPECT-MISMATCHED MODE. EGA 640x350 letterboxed into the 8:5 box:
    // the WIDTH must still be an exact multiple (4x here), the height follows the
    // box. This is the arm that keeps a resampled height from silently
    // becoming a resampled width as well.
    dos_present_rect_rs(3840, 2160, 640, 400, 640, 350, &def, &mut r);
    if r.w % 640 != 0 { bad += 1; }
    if r.w != 2560 || r.h != 1600 { bad += 1; }
    // ...and the height is NOT an integer multiple of 350, which is the point:
    // the box wins on height, so this is the arm where a resample is correct.
    if r.h % 350 == 0 { bad += 1; }

    // `integer=always` must bind where nothing else does. 1280x772 (the
    // maximised window on the 1280x800 verification VM) is 0.95 Mpx, well under
    // budget, so only this setting can change it.
    let always = DosViewPolicy { budget_px: DOS_VIEW_BUDGET_PX, integer: 2, max_w: 0, max_h: 0, aspect: 0, frameskip: 1 };
    dos_present_rect_rs(1280, 772, 640, 400, 320, 200, &always, &mut r);
    if r.w != 960 || r.h != 600 { bad += 1; }
    // and the default must NOT change that same case
    dos_present_rect_rs(1280, 772, 640, 400, 320, 200, &def, &mut r);
    if r.w != 1235 || r.h != 772 { bad += 1; }

    // ---- THE PICTURE IS ALWAYS INSIDE THE BUFFER. The scaler indexes
    // win_buf[(r.y+dy)*win_w + r.x+dx] with no further clamp, so an out-of-
    // bounds rectangle here is a heap write, not a cosmetic fault.
    let sizes: [(i32, i32); 8] = [(1, 1), (2, 100), (100, 2), (319, 199),
                                  (640, 400), (1920, 1080), (2560, 1600), (3840, 2160)];
    let pols: [DosViewPolicy; 7] = [
        DosViewPolicy { budget_px: DOS_VIEW_BUDGET_PX, integer: 1, max_w: 0, max_h: 0, aspect: 0, frameskip: 1 },
        DosViewPolicy { budget_px: DOS_VIEW_BUDGET_PX, integer: 0, max_w: 0, max_h: 0, aspect: 0, frameskip: 1 },
        DosViewPolicy { budget_px: DOS_VIEW_BUDGET_PX, integer: 2, max_w: 0, max_h: 0, aspect: 0, frameskip: 1 },
        DosViewPolicy { budget_px: 0, integer: 1, max_w: 0, max_h: 0, aspect: 0, frameskip: 1 },
        DosViewPolicy { budget_px: 0, integer: 2, max_w: 0, max_h: 0, aspect: 0, frameskip: 1 },
        DosViewPolicy { budget_px: 1, integer: 1, max_w: 1, max_h: 1, aspect: 0, frameskip: 1 },
        DosViewPolicy { budget_px: 1, integer: 2, max_w: 1, max_h: 1, aspect: 0, frameskip: 1 },
    ];
    let guests: [(i32, i32, i32, i32); 4] = [
        (320, 200, 640, 400), (320, 240, 320, 240),
        (640, 350, 640, 400), (800, 600, 800, 600),
    ];
    for (cw, ch) in sizes.iter() {
        for pl in pols.iter() {
            for (gw, gh, aw, ah) in guests.iter() {
                if dos_present_rect_rs(*cw, *ch, *aw, *ah, *gw, *gh, pl, &mut r) != 1 {
                    bad += 1;
                    continue;
                }
                if r.w < 1 || r.h < 1 { bad += 1; }
                if r.x < 0 || r.y < 0 { bad += 1; }
                if r.x + r.w > *cw || r.y + r.h > *ch { bad += 1; }
            }
        }
    }

    // ---- THE OPENING CAP IS SEPARATE FROM THE PICTURE BUDGET. Written out as
    // literal geometries for the two panels that matter, because the whole
    // point of this pair of numbers is that they diverge only at 4K and a test
    // that derived them from the constants would not have caught the defect.
    if dos_view_open_budget_rs(0) != DOS_VIEW_OPEN_BUDGET_PX { bad += 1; }        // uncapped picture
    if dos_view_open_budget_rs(DOS_VIEW_BUDGET_PX) != DOS_VIEW_OPEN_BUDGET_PX { bad += 1; } // bigger loses
    if dos_view_open_budget_rs(1_300_000) != 1_300_000 { bad += 1; }              // smaller wins
    {
        // dos_run_file's opening block: work area minus its (64, 96) margin,
        // sized from the 640x400 text mode every guest starts in, integer=2.
        let open = DosViewPolicy { budget_px: dos_view_open_budget_rs(DOS_VIEW_BUDGET_PX),
                                   integer: 2, max_w: 0, max_h: 0, aspect: 0, frameskip: 1 };
        // 3840x2160 panel, ~110px of taskbar: 1920x1200, NOT the 2560x1600 the
        // picture budget alone would have produced.
        dos_present_rect_rs(3840 - 64, 2160 - 110 - 96, 640, 400, 640, 400, &open, &mut r);
        if r.w != 1920 || r.h != 1200 { bad += 1; }
        // 1920x1080 panel: the screen binds first, so every budget gives this.
        dos_present_rect_rs(1920 - 64, 1080 - 110 - 96, 640, 400, 640, 400, &open, &mut r);
        if r.w != 1280 || r.h != 800 { bad += 1; }
    }

    // ---- AN UNCAPPED POLICY IS EXACTLY dos_letterbox_rs. This is what a user
    // who writes `budget=off` is promised, so it is checked rather than
    // assumed.
    let off = DosViewPolicy { budget_px: 0, integer: 1, max_w: 0, max_h: 0, aspect: 0, frameskip: 1 };
    let mut a = DosRect { x: 0, y: 0, w: 0, h: 0 };
    dos_letterbox_rs(3840, 2160, 640, 400, &mut a);
    dos_present_rect_rs(3840, 2160, 640, 400, 320, 200, &off, &mut r);
    if a.w != r.w || a.h != r.h || a.x != r.x || a.y != r.y { bad += 1; }

    // ---- A HARD max= CEILING BINDS EVEN WHEN THE BUDGET DOES NOT. 1024x768 is
    // 0.79 Mpx, well under the budget, so only max= can produce it.
    let cap = DosViewPolicy { budget_px: 0, integer: 1, max_w: 1024, max_h: 768, aspect: 0, frameskip: 1 };
    dos_present_rect_rs(3840, 2160, 640, 400, 320, 200, &cap, &mut r);
    if r.w > 1024 || r.h > 768 { bad += 1; }
    if r.w != 960 || r.h != 600 { bad += 1; }   // 3x, the largest that fits

    // ---- THE ASPECT SWITCH. Only the 8:5 box is corrected, and only when
    // asked; a box that is already 4:3 or is a VESA mode's own resolution must
    // come back untouched, or a correct shape would be corrected twice.
    let mut caw = 640i32; let mut cah = 400i32;
    if dos_aspect_apply_rs(DOS_ASPECT_CRT, &mut caw, &mut cah) != 1 { bad += 1; }
    if caw != 640 || cah != 480 { bad += 1; }
    // square (the default) is a no-op
    caw = 640; cah = 400;
    if dos_aspect_apply_rs(DOS_ASPECT_SQUARE, &mut caw, &mut cah) != 0 { bad += 1; }
    if caw != 640 || cah != 400 { bad += 1; }
    // a box that is not 8:5 is left alone even under crt
    for (w0, h0) in [(320i32, 240i32), (640, 480), (800, 600), (1024, 768)].iter() {
        let mut w = *w0; let mut h = *h0;
        if dos_aspect_apply_rs(DOS_ASPECT_CRT, &mut w, &mut h) != 0 { bad += 1; }
        if w != *w0 || h != *h0 { bad += 1; }
    }
    // degenerate input refuses rather than dividing
    let mut zw = 0i32; let mut zh = 400i32;
    if dos_aspect_apply_rs(DOS_ASPECT_CRT, &mut zw, &mut zh) != 0 { bad += 1; }
    if dos_aspect_apply_rs(DOS_ASPECT_CRT, core::ptr::null_mut(), &mut zh) != 0 { bad += 1; }

    // AND THE WHOLE PIPELINE UNDER aspect=crt: 320x200 into a 4:3 box on a 4K
    // window. The WIDTH must still be an exact multiple of 320 (the free axis,
    // and the one the cost is proportional to); the height comes from the box.
    let crt = DosViewPolicy { budget_px: DOS_VIEW_BUDGET_PX, integer: 1,
                              max_w: 0, max_h: 0, aspect: DOS_ASPECT_CRT, frameskip: 1 };
    let (mut aw2, mut ah2) = (640i32, 400i32);
    dos_aspect_apply_rs(crt.aspect, &mut aw2, &mut ah2);
    dos_present_rect_rs(3840, 2160, aw2, ah2, 320, 200, &crt, &mut r);
    if r.w % 320 != 0 { bad += 1; }
    if r.w != 2560 || r.h != 1920 { bad += 1; }
    if (r.w as i64) * (r.h as i64) > DOS_VIEW_BUDGET_PX as i64 { bad += 1; }
    // and 1600:1200 IS 4:3, which is the whole point of the switch
    if (r.w as i64) * 3 != (r.h as i64) * 4 { bad += 1; }

    let mut pc = DosViewPolicy { budget_px: DOS_VIEW_BUDGET_PX, integer: 1,
                                 max_w: 0, max_h: 0, aspect: 0, frameskip: 1 };
    let ctxt = b"aspect = CRT\n";
    dos_view_parse_rs(ctxt.as_ptr(), ctxt.len() as u32, &mut pc);
    if pc.aspect != DOS_ASPECT_CRT { bad += 1; }

    // ---- DEGENERATE INPUTS refuse, exactly as dos_letterbox_rs does.
    if dos_present_rect_rs(0, 400, 640, 400, 320, 200, &def, &mut r) != 0 { bad += 1; }
    if dos_present_rect_rs(640, 400, 0, 400, 320, 200, &def, &mut r) != 0 { bad += 1; }
    if dos_present_rect_rs(640, 400, 640, 400, 320, 200, &def, core::ptr::null_mut()) != 0 { bad += 1; }
    // A NULL policy must mean "no policy", not a crash: it is what a caller
    // that has not loaded the config yet would pass.
    if dos_present_rect_rs(3840, 2160, 640, 400, 320, 200, core::ptr::null(), &mut r) != 1 { bad += 1; }
    // A guest resolution of zero must not divide by zero; it falls back to the
    // aspect-fit arm.
    if dos_present_rect_rs(3840, 2160, 640, 400, 0, 0, &def, &mut r) != 1 { bad += 1; }
    if r.w < 1 || r.h < 1 { bad += 1; }

    // ---- THE CONFIG PARSER. Values that parse must land; a typo must leave
    // the default standing rather than zero the budget.
    let mut p = DosViewPolicy { budget_px: DOS_VIEW_BUDGET_PX, integer: 1, max_w: 0, max_h: 0, aspect: 0, frameskip: 1 };
    let txt = b"# comment\nbudget = 1000000\ninteger=off\nmax=1024x768\nnonsense=1\n";
    dos_view_parse_rs(txt.as_ptr(), txt.len() as u32, &mut p);
    if p.budget_px != 1000000 || p.integer != 0 || p.max_w != 1024 || p.max_h != 768 { bad += 1; }

    let mut pa = DosViewPolicy { budget_px: DOS_VIEW_BUDGET_PX, integer: 1, max_w: 0, max_h: 0, aspect: 0, frameskip: 1 };
    let atxt = b"integer = ALWAYS   # sharp everywhere\n";
    dos_view_parse_rs(atxt.as_ptr(), atxt.len() as u32, &mut pa);
    if pa.integer != 2 { bad += 1; }

    let mut p2 = DosViewPolicy { budget_px: DOS_VIEW_BUDGET_PX, integer: 1, max_w: 0, max_h: 0, aspect: 0, frameskip: 1 };
    let bad_txt = b"budget=abc\nmax=wide\ninteger=maybe\n";
    dos_view_parse_rs(bad_txt.as_ptr(), bad_txt.len() as u32, &mut p2);
    if p2.budget_px != DOS_VIEW_BUDGET_PX || p2.integer != 1 || p2.max_w != 0 { bad += 1; }

    let mut p3 = DosViewPolicy { budget_px: DOS_VIEW_BUDGET_PX, integer: 1, max_w: 0, max_h: 0, aspect: 0, frameskip: 1 };
    let off_txt = b"budget=off\r\n";
    dos_view_parse_rs(off_txt.as_ptr(), off_txt.len() as u32, &mut p3);
    if p3.budget_px != 0 { bad += 1; }

    // A forty-digit budget must SATURATE, never wrap to a negative (which reads
    // as "uncapped" and would undo the feature without a word).
    let mut p4 = DosViewPolicy { budget_px: DOS_VIEW_BUDGET_PX, integer: 1, max_w: 0, max_h: 0, aspect: 0, frameskip: 1 };
    let huge = b"budget=99999999999999999999999999999999999999\n";
    dos_view_parse_rs(huge.as_ptr(), huge.len() as u32, &mut p4);
    if p4.budget_px <= 0 { bad += 1; }

    // An empty or absent file leaves every default standing.
    let mut p5 = DosViewPolicy { budget_px: DOS_VIEW_BUDGET_PX, integer: 1, max_w: 0, max_h: 0, aspect: 0, frameskip: 1 };
    if dos_view_parse_rs(core::ptr::null(), 0, &mut p5) != 0 { bad += 1; }
    if p5.budget_px != DOS_VIEW_BUDGET_PX { bad += 1; }

    bad
}
