// rustkern/cga.rs - #212: the CGA GRAPHICS presenter (modes 04h/05h/06h).
//
// WHY THIS FILE EXISTS, stated as the bug it fixes.
//
// dos_present_inner() dispatched on video_mode and had exactly four arms: VESA,
// text (video_mode <= 3 or 7), EGA planar (0Dh/0Eh/10h/12h), and mode 13h. Every
// other graphics mode fell off the end of
//
//     if (t->video_mode != 0x13) return;
//
// and DREW NOTHING. The window therefore kept whatever pixels were last in it,
// which for a freshly mapped DOS window is the compositor's uniform grey. That
// is the whole of #212: the owner reported "joust doesnt open, grey window",
// and Joust's first act is INT 10h AX=0004, i.e. CGA 320x200x4. The guest was
// running correctly and writing pixels the whole time; nothing read them.
//
// It also explains the SECOND report, "joust went to a black screen with options
// after a minute or so", which looked like a separate slow-startup bug and is
// not. Joust runs its attract sequence in mode 04h (invisible here), then sets
// mode 00h, a TEXT mode, which dos_text_is() matches and dos_present_text()
// draws. Nothing got faster after a minute; the guest simply walked into the one
// mode this OS could already display. A missing presenter and a slow guest look
// identical from the outside, and that is exactly why the measurement that
// settled it was the serial log's mode-set trace and not a stopwatch.
//
// RUST, per the 2026-07-16 rule: a new subsystem with no C twin to strangle. The
// #740 precedent is also explicit that a pixel loop earns no performance
// exemption here: the identical algorithm came out within 1-4% between gcc 12.2
// and rustc 1.97.0 with the kernel's own flags, and rustc was the faster one at
// the large sizes. So the loop lives here and dos/dosexec.c marshals.
//
// ===========================================================================
// THE MEMORY LAYOUT, which is the only genuinely surprising part
// ---------------------------------------------------------------------------
// CGA graphics memory is 16 KB at B800:0000 and it is NOT linear. It is split
// into two 8 KB banks holding the EVEN and the ODD scanlines:
//
//     row y  ->  0xB8000 + (y & 1) * 0x2000 + (y >> 1) * 80
//
// 80 bytes per row in all three modes. What differs is the packing:
//
//     04h/05h  320x200, 2 bits per pixel, 4 pixels per byte, MSB-first:
//              colour = (byte >> (6 - 2 * (x & 3))) & 3
//     06h      640x200, 1 bit per pixel, 8 pixels per byte, MSB-first:
//              colour = (byte >> (7 - (x & 7))) & 1
//
// A presenter that assumed a linear 320-byte-per-row buffer (the shape mode 13h
// has) would draw the top half of the picture stretched over the whole window
// with every other scanline missing, a failure that LOOKS like a corrupted game
// rather than like a wrong address calculation. The interleave is asserted
// directly in the self-test for that reason.
//
// ===========================================================================
// THE PALETTE
// ---------------------------------------------------------------------------
// CGA graphics colour comes from ONE hardware register, the Color Select
// Register at port 0x3D9, and from nothing else. There is no DAC to program:
//
//     bits 0-3  background/border colour, which is ALSO colour index 0 in the
//               320x200 modes, and the FOREGROUND colour in 640x200 mode 06h
//     bit 4     intensity: pick the bright half of the fixed triple
//     bit 5     palette select: 0 = green/red/brown, 1 = cyan/magenta/white
//
// Mode 05h is the same 320x200x4 signal with a third fixed triple
// (cyan/red/white), which is what a composite monitor turned into greys.
//
// The BIOS leaves 0x30 after a mode 04h set: bit 5 (palette 1) and bit 4
// (intensity), i.e. the notorious black / light-cyan / light-magenta / white
// that most people picture when they picture CGA. Getting this default wrong
// does not fail loudly, it just makes every CGA title the wrong colour, so it is
// pinned in the self-test against the BIOS value rather than left to a guess.
#![allow(dead_code)]

// The 16 CGA/EGA attribute colours. Deliberately the SAME sixteen values as
// cga_argb[] in dos/dosexec.c's text presenter: one CGA palette for the machine,
// so a title that switches between mode 04h and mode 00h (which Joust does,
// twice, in the trace that found this bug) cannot change hue when it does.
pub const CGA16: [u32; 16] = [
    0xFF000000, 0xFF0000AA, 0xFF00AA00, 0xFF00AAAA,
    0xFFAA0000, 0xFFAA00AA, 0xFFAA5500, 0xFFAAAAAA,
    0xFF555555, 0xFF5555FF, 0xFF55FF55, 0xFF55FFFF,
    0xFFFF5555, 0xFFFF55FF, 0xFFFFFF55, 0xFFFFFFFF,
];

// What a real BIOS writes to 0x3D9 when it sets mode 04h/05h/06h.
pub const CGA_PAL_RESET: u8 = 0x30;

pub const CGA_BASE: u32 = 0xB8000;
pub const CGA_PLANE: u32 = 0x2000;   // even/odd bank stride
pub const CGA_ROW_BYTES: u32 = 80;
pub const CGA_SIZE: u32 = 0x4000;    // 16 KB total

// The four-entry palette actually in force, derived from the mode and the one
// hardware register. Returned by value so the pixel loop does no per-pixel
// branching on the mode.
pub fn cga_palette(mode: u8, pal_reg: u8) -> [u32; 4] {
    let bg = CGA16[(pal_reg & 0x0F) as usize];
    if mode == 0x06 {
        // 640x200 mono: bit set = the 0x3D9 colour, bit clear = black. The
        // register's low nibble is the FOREGROUND here, not the background,
        // which is the one place mode 06h inverts the meaning of the field.
        return [CGA16[0], bg, bg, bg];
    }
    let bright = (pal_reg & 0x10) != 0;
    // The three fixed triples, as low-intensity indices. Adding 8 gives the
    // bright half, which is exactly how the hardware's intensity bit behaves.
    let triple: [usize; 3] = if mode == 0x05 {
        [3, 4, 7]           // mode 05h: cyan / red / white
    } else if (pal_reg & 0x20) != 0 {
        [3, 5, 7]           // palette 1: cyan / magenta / white
    } else {
        [2, 4, 6]           // palette 0: green / red / brown
    };
    let b = if bright { 8 } else { 0 };
    [bg, CGA16[triple[0] + b], CGA16[triple[1] + b], CGA16[triple[2] + b]]
}

// Byte offset, relative to CGA_BASE, of the byte holding pixel (x, y).
// Split out of the loop ONLY so the self-test can assert it directly: the
// interleave is the part a reader would otherwise have to take on trust.
#[inline]
pub fn cga_byte_off(mode: u8, x: u32, y: u32) -> u32 {
    let bank = (y & 1) * CGA_PLANE;
    let row = (y >> 1) * CGA_ROW_BYTES;
    let within = if mode == 0x06 { x >> 3 } else { x >> 2 };
    bank + row + within
}

#[inline]
fn cga_pixel(mode: u8, byte: u8, x: u32) -> usize {
    if mode == 0x06 {
        ((byte >> (7 - (x & 7))) & 1) as usize
    } else {
        ((byte >> (6 - 2 * (x & 3))) & 3) as usize
    }
}

// The marshalling struct. Mirrored field-for-field by cga_present_t in
// dos/dosexec.c, which carries the _Static_assert set that locks the ABI: this
// project has been bitten before by a repr(C) struct whose C twin drifted, and
// the failure mode there is a wrong pointer in a pixel loop, not a build error.
#[repr(C)]
pub struct CgaPresent {
    pub dst: *mut u32,          // host window content buffer (ARGB)
    pub src: *const u8,         // guest memory at 0xB8000
    pub dst_stride: u32,        // pixels per destination row (win_w)
    pub dst_x: u32,
    pub dst_y: u32,
    pub dst_w: u32,
    pub dst_h: u32,
    pub src_w: u32,             // 320 or 640
    pub src_h: u32,             // 200
    pub src_len: u32,           // readable bytes at src; the bounds chokepoint
    pub mode: u8,               // 0x04, 0x05 or 0x06
    pub pal_reg: u8,            // the live 0x3D9 Color Select Register
    pub pad: [u8; 2],
}

// Present one frame. Nearest-neighbour scale into the letterboxed rect the
// caller computed with dos_letterbox_rs(), which is the SAME rect the input path
// uses, so the picture and the pointer cannot disagree about where it is.
//
// SAFETY: dst must be writable for dst_stride * (dst_y + dst_h) u32s, and src
// readable for src_len bytes. The source side is checked against src_len here;
// the destination is the window buffer the caller owns and has just sized.
#[no_mangle]
pub unsafe extern "C" fn cga_present_rs(p: *const CgaPresent) -> i32 {
    if p.is_null() { return -1; }
    let p = &*p;
    if p.dst.is_null() || p.src.is_null() { return -1; }
    if p.dst_w == 0 || p.dst_h == 0 || p.src_w == 0 || p.src_h == 0 { return -1; }
    if p.mode != 0x04 && p.mode != 0x05 && p.mode != 0x06 { return -1; }
    // The largest offset the loop can form. Refusing here rather than clamping
    // per pixel keeps the inner loop branch-free AND makes a short buffer a LOUD
    // failure instead of a picture with a plausible-looking corner.
    let need = cga_byte_off(p.mode, p.src_w - 1, p.src_h - 1) + 1;
    if need > p.src_len { return -2; }

    let pal = cga_palette(p.mode, p.pal_reg);
    let sw = p.dst_w as usize;
    let sh = p.dst_h as usize;
    let stride = p.dst_stride as usize;

    // Row reuse: consecutive destination rows that map to the same SOURCE row
    // are copied rather than re-expanded. At the shipped 640x400 window a
    // 320x200 mode maps two destination rows to every source row, so this halves
    // the per-pixel work. The C presenters do the same thing via dos_row_reuse();
    // this is that idea, local, because that helper is a C-side static.
    let mut prev_sy: i64 = -1;
    for dy in 0..sh {
        let sy = (dy as u64 * p.src_h as u64 / p.dst_h as u64) as u32;
        let sy = if sy >= p.src_h { p.src_h - 1 } else { sy };
        let drow = p.dst.add((p.dst_y as usize + dy) * stride + p.dst_x as usize);
        if prev_sy == sy as i64 && dy > 0 {
            let prow = p.dst.add((p.dst_y as usize + dy - 1) * stride + p.dst_x as usize);
            core::ptr::copy_nonoverlapping(prow, drow, sw);
            continue;
        }
        prev_sy = sy as i64;
        let bank = (sy & 1) * CGA_PLANE + (sy >> 1) * CGA_ROW_BYTES;
        // One source-byte fetch per group of pixels, not one per destination
        // pixel: at the default scale that is 80 loads a row rather than 640.
        let mut last_byte_off = u32::MAX;
        let mut byte: u8 = 0;
        for dx in 0..sw {
            let sx = (dx as u64 * p.src_w as u64 / p.dst_w as u64) as u32;
            let sx = if sx >= p.src_w { p.src_w - 1 } else { sx };
            let off = bank + if p.mode == 0x06 { sx >> 3 } else { sx >> 2 };
            if off != last_byte_off {
                byte = *p.src.add(off as usize);
                last_byte_off = off;
            }
            *drow.add(dx) = pal[cga_pixel(p.mode, byte, sx)];
        }
    }
    0
}

// ===========================================================================
// SELF-TEST. Runs at boot and prints the check COUNT, for the reason #514 gives:
// a self-test that ran zero assertions and one that passed look identical.
//
// THE SCRATCH BUFFER IS THE CALLER'S, AND THAT IS NOT A STYLE CHOICE.
// The end-to-end check needs a whole 16 KB CGA aperture, because the odd-bank
// pixel it has to prove lands at offset 0x2000. The first version of this
// function declared that as `let mut mem = [0u8; CGA_SIZE]` - a 16 KB array in
// a stack frame. It runs on the DOS task's KERNEL stack, which is 64 KB
// (proc/process.h KERNEL_STACK_SIZE) and is already deep by the time the launch
// path reaches the self-test block. MEASURED: build 2020 produced 40
// "[SCHEDBUG] context_switch: pid=28 'dos' ... SHARED STACK or deep overflow"
// lines that build 2009 produced ZERO of, and one run ended in
// "[KERNEL PANIC] Invalid Opcode at RIP=0x45" with an all-zero stack walk, i.e.
// a corrupted return address. dos/dos4gw.c already kmallocs its self-test arena
// for exactly this reason; this now does the same, which is the shared-primitive
// rule applied to "where does scratch memory live".
#[no_mangle]
pub unsafe extern "C" fn cga_selftest_rs(scratch: *mut u8, scratch_len: u32,
                                         checks: *mut u32) -> i32 {
    if scratch.is_null() || scratch_len < CGA_SIZE {
        if !checks.is_null() { *checks = 0; }
        return -100;      // reported as SKIPPED by the caller, never as a pass
    }
    let mut n: u32 = 0;
    let mut fail: i32 = 0;
    macro_rules! ck {
        ($cond:expr, $id:expr) => {{
            n += 1;
            if !($cond) && fail == 0 { fail = $id; }
        }};
    }

    // ---- the interleave, which is the claim a reader cannot check by eye ----
    // Row 0 is the first byte of bank 0; row 1 is the first byte of bank 1;
    // row 2 is 80 bytes into bank 0. A linear-buffer bug puts row 1 at 80.
    ck!(cga_byte_off(0x04, 0, 0) == 0, 1);
    ck!(cga_byte_off(0x04, 0, 1) == CGA_PLANE, 2);
    ck!(cga_byte_off(0x04, 0, 2) == CGA_ROW_BYTES, 3);
    ck!(cga_byte_off(0x04, 0, 3) == CGA_PLANE + CGA_ROW_BYTES, 4);
    // The last pixel of a 320x200 frame must land inside the 16 KB aperture.
    ck!(cga_byte_off(0x04, 319, 199) == CGA_PLANE + 99 * CGA_ROW_BYTES + 79, 5);
    ck!(cga_byte_off(0x04, 319, 199) < CGA_SIZE, 6);
    // 640x200 mono packs eight pixels per byte, so the same row is still 80.
    ck!(cga_byte_off(0x06, 639, 199) == CGA_PLANE + 99 * CGA_ROW_BYTES + 79, 7);
    ck!(cga_byte_off(0x04, 3, 0) == 0 && cga_byte_off(0x04, 4, 0) == 1, 8);

    // ---- pixel extraction order: MSB-first within the byte ----
    ck!(cga_pixel(0x04, 0b11100100, 0) == 3, 10);
    ck!(cga_pixel(0x04, 0b11100100, 1) == 2, 11);
    ck!(cga_pixel(0x04, 0b11100100, 2) == 1, 12);
    ck!(cga_pixel(0x04, 0b11100100, 3) == 0, 13);
    ck!(cga_pixel(0x06, 0b10000001, 0) == 1, 14);
    ck!(cga_pixel(0x06, 0b10000001, 7) == 1, 15);
    ck!(cga_pixel(0x06, 0b10000001, 3) == 0, 16);

    // ---- the palette, pinned against the BIOS default ----
    // 0x30 is what a real mode 04h set leaves: bright cyan/magenta/white on a
    // black background. If this ever changes, every CGA title changes colour.
    let p = cga_palette(0x04, CGA_PAL_RESET);
    ck!(p[0] == CGA16[0], 20);
    ck!(p[1] == CGA16[11], 21);
    ck!(p[2] == CGA16[13], 22);
    ck!(p[3] == CGA16[15], 23);
    // Palette 0, low intensity: green / red / brown.
    let p0 = cga_palette(0x04, 0x00);
    ck!(p0[1] == CGA16[2] && p0[2] == CGA16[4] && p0[3] == CGA16[6], 24);
    // The low nibble really is colour 0 and the border.
    let pb = cga_palette(0x04, 0x01);
    ck!(pb[0] == CGA16[1], 25);
    // Mode 05h has its own triple and ignores the palette-select bit.
    let p5a = cga_palette(0x05, 0x00);
    let p5b = cga_palette(0x05, 0x20);
    ck!(p5a[1] == CGA16[3] && p5a[2] == CGA16[4] && p5a[3] == CGA16[7], 26);
    ck!(p5a[1] == p5b[1] && p5a[3] == p5b[3], 27);
    // Mode 06h: the low nibble is the FOREGROUND and 0 is black.
    let p6 = cga_palette(0x06, 0x0F);
    ck!(p6[0] == CGA16[0] && p6[1] == CGA16[15], 28);

    // ---- the presenter refuses a short source instead of reading past it ----
    let mut fb = [0u32; 8 * 4];
    let src = [0u8; 4];
    let short = CgaPresent {
        dst: fb.as_mut_ptr(), src: src.as_ptr(),
        dst_stride: 8, dst_x: 0, dst_y: 0, dst_w: 8, dst_h: 4,
        src_w: 320, src_h: 200, src_len: 4, mode: 0x04, pal_reg: 0x30, pad: [0; 2],
    };
    ck!(cga_present_rs(&short) == -2, 30);
    // ...and refuses a mode it does not implement rather than drawing noise.
    let badmode = CgaPresent { mode: 0x13, src_len: CGA_SIZE, ..short };
    ck!(cga_present_rs(&badmode) == -1, 31);

    // ---- END TO END: a known bit pattern must land at known pixels ----
    // 4x2 source pixels scaled 1:1 into a 4x2 window. Byte 0 of bank 0 is the
    // top row, byte 0 of bank 1 is the SECOND row. If the interleave were wrong
    // the two rows would come out identical, which is exactly what this catches.
    core::ptr::write_bytes(scratch, 0, CGA_SIZE as usize);
    *scratch.add(0) = 0b11100100;                       // row 0: colours 3,2,1,0
    *scratch.add(CGA_PLANE as usize) = 0b00011011;      // row 1: colours 0,1,2,3
    let mut out = [0u32; 4 * 2];
    let e2e = CgaPresent {
        dst: out.as_mut_ptr(), src: scratch as *const u8,
        dst_stride: 4, dst_x: 0, dst_y: 0, dst_w: 4, dst_h: 2,
        src_w: 4, src_h: 2, src_len: CGA_SIZE, mode: 0x04, pal_reg: CGA_PAL_RESET,
        pad: [0; 2],
    };
    ck!(cga_present_rs(&e2e) == 0, 40);
    let q = cga_palette(0x04, CGA_PAL_RESET);
    ck!(out[0] == q[3] && out[1] == q[2] && out[2] == q[1] && out[3] == q[0], 41);
    ck!(out[4] == q[0] && out[5] == q[1] && out[6] == q[2] && out[7] == q[3], 42);
    // The two rows MUST differ; identical rows means the odd bank was ignored.
    ck!(out[0] != out[4], 43);

    if !checks.is_null() { *checks = n; }
    fail
}
