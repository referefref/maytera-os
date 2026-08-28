// rustkern/modex.rs - #740: Mode X (unchained 256-colour VGA) geometry.
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
//
// SCOPE, AND WHY IT IS NARROWER THAN IT FIRST LOOKS. `dos/dosexec.c` already has
// ONE CRTC/ATC/Sequencer decode (`dos_vga_decode_geom` -> `dos_vga_geom_t`) that
// every presenter reads: display start address, Offset/stride, Line Compare,
// pixel pan, Map Mask, Chain-4, Read Map Select. Mode X reads THAT, exactly as
// the EGA and chain-4 presenters do; re-decoding those registers here would be
// the fifth ad hoc copy that the shared decode exists to prevent.
//
// What is left, and what this module is for, is the ONE thing no other mode
// needs and no register in that struct answers: **what resolution is the guest
// actually in.** Every other mode gets its width and height from the INT 10h
// mode number. Mode X has no mode number.
//
// ===========================================================================
// WHAT MODE X IS, AND WHY THE EMULATOR COULD NOT SEE IT
// ---------------------------------------------------------------------------
// A game sets INT 10h mode 13h and then, behind the BIOS's back, clears the
// Chain-4 bit (bit 3) of Sequencer register 4 and reprograms the CRTC. The mode
// number stays 0x13 forever - INT 10h AH=0Fh keeps reporting 0x13 - but the
// memory model has changed completely:
//
//   chained mode 13h : A000:xxxx is a flat 320x200 byte array. One pixel per
//                      byte, 64000 bytes, no plane machinery, and 5504 bytes
//                      of the 64 KB window wasted. No page flipping.
//   unchained Mode X : the four VGA planes are exposed. A pixel at (x, y) lives
//                      in plane `x & 3` at byte offset `y * stride + (x >> 2)`.
//                      All 256 KB of VGA RAM is addressable through the same
//                      64 KB window, which is exactly why games chose it: at
//                      320x240 a page is 19200 bytes, so three whole pages fit
//                      and the display start address flips between them.
//
// `dosexec.c` stored the Chain-4 bit for readback and never read it, so every
// Mode X title wrote into the four-plane machinery and then had a flat 320x200
// buffer presented, i.e. garbage or nothing.
//
// ===========================================================================
// WHY THE RESOLUTION IS DERIVED AND NOT LOOKED UP
// ---------------------------------------------------------------------------
// 320x240, 320x400, 320x480, 360x480 and the tweaked 256-colour modes in
// between are all "mode 13h plus a CRTC table", and which one you get is decided
// entirely by registers the game writes directly. A table of magic mode numbers
// would be wrong for the first game that used a fifth variant. So:
//
//   width      CRTC 0x01 (Horizontal Display End) is character clocks minus 1.
//              In a 256-colour mode the dot clock is halved, so an 8-dot
//              character clock carries 4 pixels: width = (r01 + 1) * 4.
//              320-wide has r01 = 0x4F -> 320.  360-wide has 0x59 -> 360.
//   scanlines  CRTC 0x12 (Vertical Display End) low 8 bits, plus bit 8 from
//              Overflow (0x07) bit 1 and bit 9 from Overflow bit 6, plus one.
//              320x240 has r12 = 0xDF and r07 = 0x3E -> 223 + 256 + 1 = 480.
//   rows       scanlines divided by the character height: CRTC 0x09 bits 0-4
//              plus one, doubled again if bit 7 (scan doubling) is set.
//              320x240 has r09 = 0x41 -> 2 scanlines per row -> 240 rows.
//              Clearing that bit 0 is exactly what turns it into 320x480.
//
// Everything is integer: the kernel is built -mno-sse/-mno-sse2 soft-float, so
// a float in a present path is not a style question.

// Mirrored by `dos_modex_geom_t` in dos/dosexec.c with a _Static_assert on the
// size. Field order and types are load-bearing.
#[repr(C)]
pub struct ModeXGeom {
    /// 1 when Chain-4 is OFF, i.e. the guest is in an unchained 256-colour mode.
    pub unchained: i32,
    /// Displayed pixels across, derived from CRTC 0x01.
    pub w: i32,
    /// Displayed rows, derived from CRTC 0x12/0x07/0x09.
    pub h: i32,
    /// Letterbox aspect the host should present this mode at.
    pub aspect_w: i32,
    pub aspect_h: i32,
}

const CRTC_LEN: usize = 32;

/// Decode the unchained-mode resolution out of the CRTC and Sequencer.
///
/// `crtc` points at 32 CRTC register bytes, `seq4` is Sequencer register 4
/// (Memory Mode). Returns 1 when the guest is unchained AND the derived
/// geometry is usable, 0 otherwise. On 0 the caller must not present through
/// the Mode X path.
///
/// Deliberately does NOT return stride, start address, Line Compare or pan:
/// those come from `dos_vga_decode_geom()` in dosexec.c, which is the one
/// decode every presenter in the tree shares.
///
/// # Safety
/// `crtc` must point to at least 32 readable bytes and `out` to a writable
/// `ModeXGeom`. Both come from `dos_task_t`, which owns them for the life of
/// the guest.
#[no_mangle]
pub extern "C" fn dos_modex_geom_rs(crtc: *const u8, seq4: u8, out: *mut ModeXGeom) -> i32 {
    if crtc.is_null() || out.is_null() {
        return 0;
    }
    // SAFETY: contract above. Read the register file once into a local copy so
    // nothing below can be perturbed by a concurrent guest OUT to 0x3D5 while
    // the derivation is half done - a torn read here would present one frame
    // with a mixed-mode geometry, which is the kind of one-frame glitch that
    // gets misdiagnosed as a renderer bug.
    let r: [u8; CRTC_LEN] = unsafe {
        let mut buf = [0u8; CRTC_LEN];
        for (i, b) in buf.iter_mut().enumerate() {
            *b = *crtc.add(i);
        }
        buf
    };
    // SAFETY: contract above.
    let g = unsafe { &mut *out };

    g.unchained = 0;
    g.w = 0;
    g.h = 0;
    g.aspect_w = 0;
    g.aspect_h = 0;

    // Sequencer Memory Mode bit 3 is Chain-4. SET means chained (plain mode
    // 13h); CLEAR means the four planes are exposed.
    if (seq4 & 0x08) != 0 {
        return 0;
    }
    g.unchained = 1;

    let w = (r[0x01] as i32 + 1) * 4;
    let vde = (r[0x12] as u32)
        | (((r[0x07] & 0x02) as u32) << 7)
        | (((r[0x07] & 0x40) as u32) << 3);
    let scanlines = vde as i32 + 1;
    let mut cell = (r[0x09] & 0x1F) as i32 + 1;
    if (r[0x09] & 0x80) != 0 {
        cell *= 2;
    }
    let h = scanlines / cell;

    // A guest that has unchained but has not yet finished writing its CRTC
    // table can transiently present nonsense. Refuse rather than draw it: the
    // caller falls back to the chained path for that frame, which is what the
    // screen showed a millisecond earlier anyway.
    if !(8..=1024).contains(&w) || !(8..=1024).contains(&h) {
        g.unchained = 0;
        return 0;
    }

    g.w = w;
    g.h = h;
    // ASPECT. Every one of these modes filled the same 4:3 CRT, and 320x240 is
    // the square-pixel mode games picked precisely so their art was not
    // squashed. The chained modes keep the existing 8:5 logical screen (see
    // doswin.rs) untouched, so nothing that shipped changes shape.
    g.aspect_w = 4;
    g.aspect_h = 3;
    1
}

/// Self-test over the register tables real Mode X code writes. Returns the
/// number of FAILING cases, so 0 is the only good answer. Called once from the
/// DOS layer at guest launch, for the same reason `dos_letterbox_selftest_rs`
/// is: a geometry rule nobody has watched being right is a rule that is
/// probably wrong, and these bit positions cannot be checked by looking at a
/// screenshot.
#[no_mangle]
pub extern "C" fn dos_modex_selftest_rs() -> i32 {
    let mut bad = 0;
    let mut g = ModeXGeom { unchained: 0, w: 0, h: 0, aspect_w: 0, aspect_h: 0 };

    // The BIOS mode 13h CRTC table, chained. This is the case that proves the
    // change is a no-op for every game that ships working today: Chain-4 set
    // means "not my business", whatever the rest of the registers say.
    let mut m13 = [0u8; CRTC_LEN];
    m13[0x01] = 0x4F; m13[0x06] = 0xBF; m13[0x07] = 0x1F; m13[0x09] = 0x41;
    m13[0x10] = 0x9C; m13[0x11] = 0x8E; m13[0x12] = 0x8F; m13[0x13] = 0x28;
    m13[0x14] = 0x40; m13[0x15] = 0x96; m13[0x16] = 0xB9; m13[0x17] = 0xA3;
    if dos_modex_geom_rs(m13.as_ptr(), 0x0E, &mut g) != 0 || g.unchained != 0 {
        bad += 1;
    }

    // Same table, Chain-4 cleared and nothing else touched: unchained 320x200,
    // the mode usually called Mode Y. 400 scanlines / 2 = 200 rows.
    if dos_modex_geom_rs(m13.as_ptr(), 0x06, &mut g) != 1
        || g.unchained != 1 || g.w != 320 || g.h != 200
    {
        bad += 1;
    }

    // Abrash's 320x240 table (Zen of Graphics Programming, listing 47.1): the
    // canonical Mode X. 480-line vertical timing, cell height 2, dword mode
    // off, byte mode on.
    let mut x240 = m13;
    x240[0x06] = 0x0D; x240[0x07] = 0x3E; x240[0x09] = 0x41;
    x240[0x10] = 0xEA; x240[0x11] = 0xAC; x240[0x12] = 0xDF;
    x240[0x14] = 0x00; x240[0x15] = 0xE7; x240[0x16] = 0x06; x240[0x17] = 0xE3;
    if dos_modex_geom_rs(x240.as_ptr(), 0x06, &mut g) != 1
        || g.w != 320 || g.h != 240 || g.aspect_w != 4 || g.aspect_h != 3
    {
        bad += 1;
    }

    // 320x400: mode 13h timing with the cell height dropped to 1, so the 400
    // scanlines become 400 rows instead of 200 double-scanned ones. This is the
    // case that proves the height is DERIVED and not a constant.
    let mut x400 = m13;
    x400[0x09] = 0x40; x400[0x14] = 0x00; x400[0x17] = 0xE3;
    if dos_modex_geom_rs(x400.as_ptr(), 0x06, &mut g) != 1 || g.w != 320 || g.h != 400 {
        bad += 1;
    }

    // 320x480: the 480-line timing AND cell height 1.
    let mut x480 = x240;
    x480[0x09] = 0x40;
    if dos_modex_geom_rs(x480.as_ptr(), 0x06, &mut g) != 1 || g.w != 320 || g.h != 480 {
        bad += 1;
    }

    // 360-wide, the widest of the family: a different dot clock (Misc Output)
    // and a wider CRTC, so the width moves off 320.
    let mut x360 = x240;
    x360[0x01] = 0x59;
    if dos_modex_geom_rs(x360.as_ptr(), 0x06, &mut g) != 1 || g.w != 360 || g.h != 240 {
        bad += 1;
    }

    // Scan doubling (CRTC 0x09 bit 7) halves the row count again.
    let mut xdbl = x240;
    xdbl[0x09] = 0x41 | 0x80;
    if dos_modex_geom_rs(xdbl.as_ptr(), 0x06, &mut g) != 1 || g.h != 120 {
        bad += 1;
    }

    // A half-programmed CRTC must be REFUSED, not presented. A guest that has
    // cleared Chain-4 but not yet written its timing has r01 = 0 here, and a
    // width of 4 must not reach the presenter.
    let mut half = [0u8; CRTC_LEN];
    half[0x12] = 0xDF;
    if dos_modex_geom_rs(half.as_ptr(), 0x06, &mut g) != 0 || g.unchained != 0 {
        bad += 1;
    }
    // A cell height of 0 is impossible on hardware (the field is height-1), but
    // a zeroed register file would produce a divide here if it were not for the
    // +1. Prove the +1 rather than trust it.
    let mut zero = [0u8; CRTC_LEN];
    zero[0x01] = 0x4F;
    if dos_modex_geom_rs(zero.as_ptr(), 0x06, &mut g) != 0 {
        bad += 1;   // 1 scanline / 1 = 1 row, below the floor, so: refused
    }

    // Null pointers must be refused, not dereferenced.
    if dos_modex_geom_rs(core::ptr::null(), 0x06, &mut g) != 0 {
        bad += 1;
    }

    bad
}
