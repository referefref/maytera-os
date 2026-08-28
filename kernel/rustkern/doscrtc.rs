// rustkern/doscrtc.rs - #740: what an INT 10h mode set leaves in the CRTC.
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
//
// ===========================================================================
// THE BUG THIS MODULE EXISTS TO MAKE IMPOSSIBLE
// ---------------------------------------------------------------------------
// `dos_vga_decode_geom()` derives the framebuffer row length from CRTC 0x13
// (Offset, in words) and falls back to a per-mode default when that register
// reads ZERO:
//
//     stride_bytes = crtc[0x13] ? crtc[0x13] * 2 : default_stride_bytes;
//
// The mode-13h set in dosexec.c deliberately did NOT write 0x13, with a comment
// explaining that the fallback is wanted: the real BIOS value is 0x28 (40 words
// = 80 bytes), which is right for the four-plane layout and FOUR TIMES TOO
// SMALL for the chained view the presenter draws. Leaving the register alone so
// it "reads as unprogrammed" gets the 320 the chained presenter needs.
//
// That reasoning holds for a program that goes straight to mode 13h, because
// `dos_task_t` is memset to zero. It is FALSE for a program that sets an EGA
// mode first. `crtc_mode_0d[0x13] = 0x14` (20 words = 40 bytes, correct for
// 320x200x16 planar), the mode-13h seed never clears it, and 0x14 is not zero,
// so the sentinel never fires and the chained presenter reads the framebuffer
// at 40 bytes per row instead of 320.
//
// Disney's Aladdin does exactly that: INT 10h mode 0Dh, then INT 10h mode 13h.
// The result was a screen of EIGHT identical vertical columns (320 / 40 = 8),
// measured on the #740 screendump as a column-energy autocorrelation peak at
// exactly 40 source pixels. The game was drawing correctly the whole time; the
// presenter was reading its buffer with the previous mode's row length.
//
// THE MECHANISM DEFECT, not the instance: a real BIOS mode set writes ALL 25
// CRTC registers. Ours wrote a subset and inherited the rest from whatever mode
// ran before. Exactly three registers were left unwritten - 0x0C, 0x0D (start
// address) and 0x13 (Offset) - and those three are precisely the ones whose
// ZERO value the decode reads as a sentinel meaning "unprogrammed, use the mode
// default". A stale non-zero value in any of them silently disables the
// fallback. So the fix is not "also clear 0x13"; it is that a mode set must
// leave the CRTC FULLY DETERMINED, and the sentinel registers are part of the
// state it determines.
//
// The self-test below replays the Aladdin transition (0Dh then 13h) and fails
// if the derived stride is anything but 320.

pub const CRTC_LEN: usize = 25;

/// CRTC 0x13 is the Offset register in WORDS; the byte stride is twice that.
/// Zero means "unprogrammed": use the caller's per-mode default.
///
/// ONE definition of the sentinel rule. `dos_vga_decode_geom()` in dosexec.c
/// applies the same rule to fill `stride_bytes`; if the two ever disagree that
/// is a bug in whichever one drifted, not a feature.
#[no_mangle]
pub extern "C" fn dos_crtc_stride_rs(crtc_13: u8, default_stride_bytes: u32) -> u32 {
    if crtc_13 == 0 {
        default_stride_bytes
    } else {
        (crtc_13 as u32) * 2
    }
}

/// The CRTC state an INT 10h mode 13h set leaves behind.
///
/// Indices 0x00-0x18 from the IBM VGA BIOS mode 13h table, with the three
/// sentinel registers forced to their "unprogrammed" reading. Writing them
/// EXPLICITLY is the whole point: it is what makes the mode set independent of
/// the mode that ran before it.
///
/// DELIBERATELY 0, and load-bearing (this repeats the dosexec.c comment because
/// the value is surprising and someone will "correct" it): 0x13 is the real
/// BIOS 0x28 on hardware, which is the four-plane row length. The chained
/// presenter draws a LINEAR 320-byte row, so it must take the 320 default, and
/// the sentinel is how it asks for it. A Mode X guest gets width/4 from ITS
/// default, and either kind of guest that programs Offset for real (a 360-wide
/// title) writes it AFTER the mode set and is honoured.
///
/// `n` is the caller's array length, checked so a shrunk `dos_task_t.crtc`
/// cannot turn into an out-of-bounds write.
#[no_mangle]
pub unsafe extern "C" fn dos_crtc_seed_mode13_rs(crtc: *mut u8, n: u32) -> i32 {
    if crtc.is_null() || (n as usize) < CRTC_LEN {
        return 0;
    }
    const M13: [u8; CRTC_LEN] = [
        0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 0x00, 0x41, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x00, 0x40, 0x96, 0xB9, 0xA3,
        0xFF,
    ];
    let dst = core::slice::from_raw_parts_mut(crtc, CRTC_LEN);
    dst.copy_from_slice(&M13);
    // The three sentinel registers, stated again as an assertion rather than
    // left implicit in the table above: start address high/low and Offset.
    dst[0x0C] = 0;
    dst[0x0D] = 0;
    dst[0x13] = 0;
    // Line Compare = 1023 (no split screen), assembled across three registers
    // exactly as dos_vga_decode_geom() reads it back.
    dst[0x18] = 0xFF;
    dst[0x07] |= 0x10;
    dst[0x09] |= 0x40;
    1
}

/// The mode 0Dh (320x200x16 planar) table, needed by the self-test so the
/// regression is replayed from the SAME bytes dosexec.c seeds, not from a
/// hand-copied approximation that could drift into agreeing with the fix.
const M0D: [u8; CRTC_LEN] = [
    0x2D, 0x27, 0x28, 0x90, 0x2B, 0x80, 0xBF, 0x1F, 0x00, 0xC0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x14, 0x00, 0x96, 0xB9, 0xE3,
    0xFF,
];

/// Returns the number of FAILING checks; 0 is a pass.
#[no_mangle]
pub extern "C" fn dos_crtc_selftest_rs() -> i32 {
    let mut bad = 0;

    // The sentinel rule itself.
    if dos_crtc_stride_rs(0, 320) != 320 {
        bad += 1;
    }
    if dos_crtc_stride_rs(0, 80) != 80 {
        bad += 1;
    }
    if dos_crtc_stride_rs(0x28, 320) != 80 {
        bad += 1;
    }
    if dos_crtc_stride_rs(0x14, 320) != 40 {
        bad += 1;
    }

    // A fresh guest: zeroed CRTC, straight to mode 13h. This case ALWAYS
    // worked, and must keep working byte-identically.
    let mut c = [0u8; CRTC_LEN];
    unsafe {
        if dos_crtc_seed_mode13_rs(c.as_mut_ptr(), CRTC_LEN as u32) != 1 {
            bad += 1;
        }
    }
    if dos_crtc_stride_rs(c[0x13], 320) != 320 {
        bad += 1;
    }

    // THE REGRESSION. Mode 0Dh first (which is what Aladdin does), then mode
    // 13h. Before the fix this yielded 40 and put eight copies of the game's
    // framebuffer side by side. Prove the stale Offset is gone AND that the
    // whole register file, not just 0x13, now matches the fresh-guest case:
    // any other register the previous mode left behind is the same bug waiting
    // for a different title.
    let mut d = [0u8; CRTC_LEN];
    d.copy_from_slice(&M0D);
    if dos_crtc_stride_rs(d[0x13], 320) != 40 {
        bad += 1; // sanity: mode 0Dh really does leave 40 there
    }
    unsafe {
        if dos_crtc_seed_mode13_rs(d.as_mut_ptr(), CRTC_LEN as u32) != 1 {
            bad += 1;
        }
    }
    if dos_crtc_stride_rs(d[0x13], 320) != 320 {
        bad += 1;
    }
    if d != c {
        bad += 1; // mode set is not fully determined: some register survived
    }
    // Start address must not survive either.
    if d[0x0C] != 0 || d[0x0D] != 0 {
        bad += 1;
    }

    // Line Compare reads back as 1023 (no split) through the decode's own
    // three-register assembly.
    let lc = (d[0x18] as u32)
        | (((d[0x07] & 0x10) as u32) << 4)
        | (((d[0x09] & 0x40) as u32) << 3);
    if lc != 1023 {
        bad += 1;
    }

    // A guest that programs Offset FOR REAL after the mode set is still
    // honoured: the fix must not pin the stride to 320.
    let mut e = [0u8; CRTC_LEN];
    unsafe {
        dos_crtc_seed_mode13_rs(e.as_mut_ptr(), CRTC_LEN as u32);
    }
    e[0x13] = 0x5A; // a 360-wide Mode X title: 90 words = 180 bytes
    if dos_crtc_stride_rs(e[0x13], 80) != 180 {
        bad += 1;
    }

    // Mode X takes width/4 from its own default through the same rule.
    let mut f = [0u8; CRTC_LEN];
    unsafe {
        dos_crtc_seed_mode13_rs(f.as_mut_ptr(), CRTC_LEN as u32);
    }
    if dos_crtc_stride_rs(f[0x13], 320 / 4) != 80 {
        bad += 1;
    }

    // Refusals: a null pointer and a short array must not be written.
    unsafe {
        if dos_crtc_seed_mode13_rs(core::ptr::null_mut(), CRTC_LEN as u32) != 0 {
            bad += 1;
        }
        let mut short = [0u8; 8];
        if dos_crtc_seed_mode13_rs(short.as_mut_ptr(), 8) != 0 {
            bad += 1;
        }
        if short != [0u8; 8] {
            bad += 1;
        }
    }

    bad
}
