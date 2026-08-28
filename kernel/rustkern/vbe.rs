// rustkern/vbe.rs - #740: VESA BIOS Extensions (VBE 1.2) for the DOS guest.
//
// New kernel subsystem with no C twin to strangle, so Rust per the 2026-07-16
// rule. EVERYTHING that decides anything lives here: the mode table, the two
// spec-defined structure layouts, the guest-visible ROM blob, and the whole
// AH=4Fh subfunction dispatcher. The C side in dos/dosexec.c is glue only:
// it resolves ES:DI to a bounds-checked pointer, owns the VRAM allocation,
// and runs the per-pixel present loop (which is the measured hot path; see
// the CHANGELOG entry for the number).
//
// ===========================================================================
// WHY BANKED AND NOT A LINEAR FRAMEBUFFER
// ---------------------------------------------------------------------------
// docs/VESA_VIDEO_DESIGN.md F1: the DOS guest runs through x86_16.c's
// real-mode `lin()`, which masks every address to 20 bits. A VBE 2.0 linear
// framebuffer lives above the first megabyte, so the emulated CPU cannot form
// the address AT ALL. That is not a performance question. A 64 KB bank window
// at A000:0000 is therefore not a fallback, it is the only design that fits,
// and it fits exactly, because dosexec.c already routes 0xA0000-0xAFFFF
// through a memory hook.
//
// Consequences, all deliberate:
//   - VbeVersion is reported as 0x0102 (VBE 1.2), not 0x0200.
//   - ModeAttributes bit 7 (LFB available) is CLEAR on every mode, and a
//     4F02h call that asks for the LFB (BX bit 14) is REFUSED, so a
//     conformant program never tries to address memory it cannot reach.
//   - 4F0Ah (protected-mode interface) is refused. It hands back a pointer to
//     bank-switch code a protected-mode program calls directly; every
//     conformant program falls back to the real-mode INT 10h path when it is
//     refused. Refusing it removes an entire class of entanglement with the
//     DOS/4GW extender work at essentially no compatibility cost.
//
// ===========================================================================
// EVERY UNIMPLEMENTED SUBFUNCTION IS STUBBED, NOT LOGGED-ONLY
// ---------------------------------------------------------------------------
// blame.md records why: an interpreter that logs a miss and returns with the
// registers untouched leaves the guest reading whatever was in AX. The
// pre-#740 behaviour was exactly that - INT 10h's `default:` case fell
// through silently, so `mov ax,0x4F00 ; int 0x10` came back with AX unchanged
// at 0x4F00. To a program that tests the whole of AX that reads as "not
// supported" by luck; to the many that test only `AL == 0x4F` it reads as
// SUPPORTED, and the program then trusts a VbeInfoBlock that was never
// written. So: every 4Fxx this file does not implement returns the spec's
// explicit failure, AX = 0x014F (AL=4F "function supported", AH=01 "call
// failed"), AND reports a miss code that C logs once.

// The VBE mode numbers we implement. Deliberately short: every mode in this
// list is a mode that has to be tested, and an advertised mode that has never
// been drawn is worse than an absent one.
//
// All 8bpp packed-pixel. docs/VESA_VIDEO_DESIGN.md section 5 measured 8bpp
// indexed as the cheapest format both per pixel and per byte, and every VBE
// call site found in a real target binary is VBE 1.2. Direct-colour modes are
// deliberately absent until something measured asks for one.
#[derive(Clone, Copy)]
pub struct VbeMode {
    pub number: u16,
    pub w: u16,
    pub h: u16,
    pub bpp: u8,
}

pub const MODES: [VbeMode; 4] = [
    // 0x100 is first for a reason: 640x400 is EXACTLY the DOS window's
    // default content size, so it is the one mode with no scaling at all,
    // which makes it the mode a stride bug is visible in.
    VbeMode { number: 0x100, w: 640, h: 400, bpp: 8 },
    VbeMode { number: 0x101, w: 640, h: 480, bpp: 8 },
    VbeMode { number: 0x103, w: 800, h: 600, bpp: 8 },
    VbeMode { number: 0x105, w: 1024, h: 768, bpp: 8 },
];

// Total video memory we advertise AND allocate up to. 1 MiB covers everything
// through 1024x768x8 (786,432 bytes). The 4F00h block reports this same
// number, so the guest's own mode filtering agrees with what actually exists
// rather than with an aspiration.
pub const VRAM_BYTES: u32 = 1024 * 1024;

// The bank window. Granularity == size == 64 KB, which is the simplest legal
// VBE 1.2 arrangement and lines up 1:1 with the 0xA0000-0xAFFFF aperture that
// dosexec.c already hooks.
pub const WIN_GRAN_KB: u16 = 64;
pub const WIN_SIZE_KB: u16 = 64;
pub const WIN_SEGMENT: u16 = 0xA000;

// ---------------------------------------------------------------------------
// The guest-visible ROM blob.
//
// 4F00h hands the guest FAR POINTERS: one to an OEM string and one to the mode
// list. They must point at memory the guest can actually read, so a small blob
// is planted in the emulated video-BIOS ROM area at C000:0000. dos_load_image()
// already refuses to load a program past 0xA0000, so nothing else lives there.
//
// The offsets are `pub const` and BOTH the blob builder and the info-block
// builder use them, so the pointer and the thing it points at cannot drift.
// ---------------------------------------------------------------------------
pub const ROM_SEG: u16 = 0xC000;
pub const ROM_OFF_BANKSTUB: u16 = 0x0000;
pub const ROM_OFF_OEM: u16 = 0x0010;
pub const ROM_OFF_MODELIST: u16 = 0x0040;
pub const ROM_SIZE: u32 = 0x0100;

const OEM_STRING: &[u8] = b"MayteraOS VBE 1.2\0";

// WinFuncPtr must be a valid far pointer: the VBE spec says a program MAY far-
// call it instead of going through INT 10h AH=4F05h, and some do. Pointing it
// at 0 and hoping is how an interpreter gets a guest jumping to the interrupt
// vector table. So the ROM carries a real 6-byte routine that performs the
// documented call with the caller's own BH/BL/DX and returns:
//
//      B8 05 4F     mov ax, 0x4F05
//      CD 10        int 0x10
//      CB           retf
//
// which means the direct-call path and the INT 10h path are the SAME code, and
// cannot diverge.
const BANK_STUB: [u8; 6] = [0xB8, 0x05, 0x4F, 0xCD, 0x10, 0xCB];

/// Fill the emulated video-BIOS ROM page. `buf` must be at least ROM_SIZE
/// bytes and is the C side's `&t->mem[0xC0000]`. Returns bytes written, or 0
/// if the buffer is unusable.
#[no_mangle]
pub extern "C" fn vbe_build_rom_rs(buf: *mut u8, len: u32) -> u32 {
    if buf.is_null() || len < ROM_SIZE {
        return 0;
    }
    // SAFETY: non-null and at least ROM_SIZE bytes, checked above. The C
    // caller passes the address of a ROM_SIZE-byte region inside the guest's
    // 1 MiB `mem` allocation.
    let m = unsafe { core::slice::from_raw_parts_mut(buf, ROM_SIZE as usize) };
    for b in m.iter_mut() {
        *b = 0;
    }

    let s = ROM_OFF_BANKSTUB as usize;
    m[s..s + BANK_STUB.len()].copy_from_slice(&BANK_STUB);

    let s = ROM_OFF_OEM as usize;
    m[s..s + OEM_STRING.len()].copy_from_slice(OEM_STRING);

    // Mode list: little-endian words, 0xFFFF terminated.
    let mut p = ROM_OFF_MODELIST as usize;
    for md in MODES.iter() {
        m[p] = (md.number & 0xFF) as u8;
        m[p + 1] = (md.number >> 8) as u8;
        p += 2;
    }
    m[p] = 0xFF;
    m[p + 1] = 0xFF;

    ROM_SIZE
}

/// The single definition of how much video memory exists, for the C glue that
/// allocates it. 4F00h advertises this same constant, so the number the guest
/// filters its mode choice with and the number actually allocated cannot drift.
#[no_mangle]
pub extern "C" fn vbe_vram_bytes_rs() -> u32 {
    VRAM_BYTES
}

fn find_mode(number: u16) -> Option<VbeMode> {
    let n = number & 0x1FF; // strip the LFB (bit 14) and no-clear (bit 15) flags
    for md in MODES.iter() {
        if md.number == n {
            return Some(*md);
        }
    }
    None
}

/// Bytes per scan line for a mode at its default (unstretched) logical width.
fn default_bpl(md: &VbeMode) -> u32 {
    (md.w as u32) * ((md.bpp as u32) / 8)
}

// ---------------------------------------------------------------------------
// State. Mirrored by `vbe_state_t` in dos/dosexec.c with a _Static_assert on
// the size, per the established FFI pattern.
// ---------------------------------------------------------------------------
#[repr(C)]
pub struct VbeState {
    pub mode: u16,      // 0 = no VBE mode active, else the mode number (no flags)
    pub width: u16,
    pub height: u16,
    pub bpp: u8,
    pub dac8: u8,       // 0 = 6-bit DAC (default), 1 = 8-bit (after 4F08h)
    pub bpl: u32,       // bytes per scan line (4F06h can widen it)
    pub bank: u32,      // window A position, in WIN_GRAN_KB units
    pub disp_start: u32, // display start, BYTES into VRAM (4F07h)
    pub vram: u32,      // bytes of VRAM actually allocated
}

// ---------------------------------------------------------------------------
// The call. One struct instead of a dozen out-parameters, so the C glue has a
// single thing to get right.
// ---------------------------------------------------------------------------
#[repr(C)]
pub struct VbeCall {
    // In, then out. C copies these back into the guest's registers.
    pub ax: u16,
    pub bx: u16,
    pub cx: u16,
    pub dx: u16,
    // ES:DI resolved to a pointer into guest memory, already bounds-checked by
    // C so that at least `buflen` bytes are addressable. Null when the guest
    // gave an unusable pointer, and every subfunction that needs it then fails
    // rather than writing somewhere else.
    pub buf: *mut u8,
    pub buflen: u32,
    // t->pal, flat: 256 entries of (r,g,b), 768 bytes.
    pub pal: *mut u8,
    // Out. What the C side must do after the call returns.
    pub action: u32,
    // Out. 0 = none. Non-zero = the subfunction C should log as a MISS, which
    // keeps varargs formatting on the C side and the decision on this one.
    pub miss: u32,
}

// `action` values.
pub const ACT_NONE: u32 = 0;
pub const ACT_SET_MODE: u32 = 1; // a VBE mode was selected: (re)allocate VRAM and clear
pub const ACT_SET_VGA: u32 = 2;  // 4F02h with a mode < 0x100: do a standard INT 10h AH=00h

const OK: u16 = 0x004F;   // AL=4F supported, AH=00 success
const FAIL: u16 = 0x014F; // AL=4F supported, AH=01 call failed

fn put16(m: &mut [u8], off: usize, v: u16) {
    m[off] = (v & 0xFF) as u8;
    m[off + 1] = (v >> 8) as u8;
}
fn put32(m: &mut [u8], off: usize, v: u32) {
    m[off] = (v & 0xFF) as u8;
    m[off + 1] = ((v >> 8) & 0xFF) as u8;
    m[off + 2] = ((v >> 16) & 0xFF) as u8;
    m[off + 3] = ((v >> 24) & 0xFF) as u8;
}
fn farptr(seg: u16, off: u16) -> u32 {
    ((seg as u32) << 16) | (off as u32)
}

/// 4F00h: fill the 256-byte VbeInfoBlock.
fn fill_info_block(m: &mut [u8]) {
    for b in m.iter_mut().take(256) {
        *b = 0;
    }
    m[0] = b'V';
    m[1] = b'E';
    m[2] = b'S';
    m[3] = b'A';
    put16(m, 0x04, 0x0102); // VbeVersion: 1.2. See the header comment.
    put32(m, 0x06, farptr(ROM_SEG, ROM_OFF_OEM)); // OemStringPtr
    // Capabilities. Bit 0 = DAC is switchable to 8 bits per primary, which is
    // true here because 4F08h is implemented for real (it changes how the
    // present path expands the palette, it is not a stored-and-ignored flag).
    put32(m, 0x0A, 0x0000_0001);
    put32(m, 0x0E, farptr(ROM_SEG, ROM_OFF_MODELIST)); // VideoModePtr
    put16(m, 0x12, (VRAM_BYTES / (64 * 1024)) as u16); // TotalMemory, 64 KB units
}

/// 4F01h: fill the 256-byte ModeInfoBlock for `md`.
fn fill_mode_info(m: &mut [u8], md: &VbeMode) {
    for b in m.iter_mut().take(256) {
        *b = 0;
    }
    // ModeAttributes: bit0 supported, bit1 optional info present, bit2 BIOS
    // TTY output, bit3 colour, bit4 graphics. Bit 5 (non-VGA-compatible) is
    // CLEAR because the window really does live at A000:0000. Bit 6 (windowed
    // NOT available) is CLEAR because windowed is the only thing we have.
    // Bit 7 (LFB available) is CLEAR: see the header comment, this is the flag
    // that stops a conformant guest asking for memory it cannot address.
    put16(m, 0x00, 0x001B);
    put16(m, 0x02, 0x0007); // WinAAttributes: relocatable, readable, writable
    // WinBAttributes stays 0: there is no window B, and 4F05h refuses BL=1.
    put16(m, 0x04, WIN_GRAN_KB);
    put16(m, 0x06, WIN_SIZE_KB);
    put16(m, 0x08, WIN_SEGMENT); // WinASegment
    put16(m, 0x0A, 0x0000);      // WinBSegment
    put32(m, 0x0C, farptr(ROM_SEG, ROM_OFF_BANKSTUB)); // WinFuncPtr
    put16(m, 0x10, default_bpl(md) as u16); // BytesPerScanLine
    put16(m, 0x12, md.w);
    put16(m, 0x14, md.h);
    m[0x16] = 8;  // XCharSize
    m[0x17] = 16; // YCharSize
    m[0x18] = 1;  // NumberOfPlanes (packed pixel)
    m[0x19] = md.bpp;
    m[0x1A] = 1;  // NumberOfBanks (1 == not a CGA-style banked mode)
    m[0x1B] = 4;  // MemoryModel: 4 = packed pixel
    m[0x1C] = 0;  // BankSize, KB. 0 for packed pixel.
    // NumberOfImagePages is "pages beyond the first", so a mode that exactly
    // fills VRAM reports 0. Computed from the SAME VRAM_BYTES the 4F00h block
    // advertises and the C side allocates.
    let page = default_bpl(md) * (md.h as u32);
    let pages = if page == 0 { 1 } else { VRAM_BYTES / page };
    m[0x1D] = if pages == 0 { 0 } else { (pages - 1).min(255) as u8 };
    m[0x1E] = 1; // Reserved, spec says 1
    // 0x1F..0x27 direct-colour fields stay zero: every mode here is 8bpp
    // packed indexed, so there are no channel masks to describe.
    // 0x28 PhysBasePtr stays zero, consistent with LFB being unavailable.
}

/// The AH=4Fh dispatcher. Returns 0 always; the result is in `c.ax`.
///
/// # Safety
/// `st` must point to a live VbeState. `c.buf` must either be null or point to
/// at least `c.buflen` writable bytes inside the guest's memory; `c.pal` must
/// either be null or point to 768 writable bytes.
#[no_mangle]
pub extern "C" fn vbe_dispatch_rs(st: *mut VbeState, c: *mut VbeCall) -> i32 {
    if st.is_null() || c.is_null() {
        return -1;
    }
    // SAFETY: both non-null (checked). The C caller passes the addresses of a
    // vbe_state_t and a vbe_call_t, whose layouts are locked to these types by
    // _Static_asserts on their sizes in dos/dosexec.c.
    let s = unsafe { &mut *st };
    let c = unsafe { &mut *c };
    c.action = ACT_NONE;
    c.miss = 0;

    let al = (c.ax & 0xFF) as u8;
    let bh = (c.bx >> 8) as u8;
    let bl = (c.bx & 0xFF) as u8;

    match al {
        // ---- 4F00h: Get SuperVGA Information ----------------------------
        0x00 => {
            if c.buf.is_null() || c.buflen < 256 {
                c.ax = FAIL;
                return 0;
            }
            // SAFETY: non-null with at least 256 bytes, checked immediately above.
            let m = unsafe { core::slice::from_raw_parts_mut(c.buf, 256) };
            fill_info_block(m);
            c.ax = OK;
        }

        // ---- 4F01h: Get SuperVGA Mode Information -----------------------
        0x01 => {
            let md = match find_mode(c.cx) {
                Some(m) => m,
                None => {
                    c.ax = FAIL;
                    return 0;
                }
            };
            if c.buf.is_null() || c.buflen < 256 {
                c.ax = FAIL;
                return 0;
            }
            // SAFETY: as above.
            let m = unsafe { core::slice::from_raw_parts_mut(c.buf, 256) };
            fill_mode_info(m, &md);
            c.ax = OK;
        }

        // ---- 4F02h: Set SuperVGA Video Mode -----------------------------
        0x02 => {
            // Bit 14 asks for the LINEAR framebuffer. We never advertise it
            // (ModeAttributes bit 7 is clear on every mode), so a conformant
            // guest never gets here; refuse explicitly anyway, because the
            // alternative is handing back success for a mode whose memory the
            // guest cannot address, and then it draws into nothing.
            if c.bx & 0x4000 != 0 {
                c.ax = FAIL;
                c.miss = 0x4F02_4000;
                return 0;
            }
            let want = c.bx & 0x1FF;
            if want < 0x100 {
                // A standard VGA mode number through the VBE entry point is
                // legal and some programs use it to restore mode 3 on exit.
                s.mode = 0;
                c.action = ACT_SET_VGA;
                c.ax = OK;
                return 0;
            }
            let md = match find_mode(want) {
                Some(m) => m,
                None => {
                    c.ax = FAIL;
                    return 0;
                }
            };
            let need = default_bpl(&md) * (md.h as u32);
            if need > VRAM_BYTES {
                c.ax = FAIL;
                return 0;
            }
            s.mode = md.number;
            s.width = md.w;
            s.height = md.h;
            s.bpp = md.bpp;
            s.bpl = default_bpl(&md);
            s.bank = 0;
            s.disp_start = 0;
            // The DAC width is NOT reset here: the spec makes 4F08h persist
            // across mode sets, and a program that sets 8-bit DAC once and
            // then changes mode would otherwise silently get half-brightness.
            c.action = ACT_SET_MODE;
            // Bit 15 = "do not clear video memory". C reads it from bx.
            c.ax = OK;
        }

        // ---- 4F03h: Get Current Video Mode -------------------------------
        0x03 => {
            if s.mode == 0 {
                // Not in a VBE mode. Refuse rather than inventing a number:
                // the guest should use AH=0Fh for standard modes.
                c.ax = FAIL;
                return 0;
            }
            c.bx = s.mode;
            c.ax = OK;
        }

        // ---- 4F04h: Save/Restore SuperVGA state --------------------------
        0x04 => {
            // Refused deliberately. Programs that get a failure skip it.
            c.ax = FAIL;
            c.miss = 0x4F04;
        }

        // ---- 4F05h: CPU Video Memory Window Control ----------------------
        0x05 => {
            if s.mode == 0 {
                c.ax = FAIL;
                return 0;
            }
            if bl & 0x01 != 0 {
                // Window B. There is none, and WinBAttributes says so.
                c.ax = FAIL;
                return 0;
            }
            match bh {
                0x00 => {
                    // Set. DX is in granularity units. Refuse a position whose
                    // window would start past the end of real VRAM, rather
                    // than clamping: a clamp turns the guest's arithmetic bug
                    // into our corrupted picture, silently.
                    let byte_pos = (c.dx as u32).wrapping_mul((WIN_GRAN_KB as u32) * 1024);
                    if byte_pos >= s.vram {
                        c.ax = FAIL;
                        return 0;
                    }
                    s.bank = c.dx as u32;
                    c.ax = OK;
                }
                0x01 => {
                    // Get.
                    c.dx = s.bank as u16;
                    c.ax = OK;
                }
                _ => {
                    c.ax = FAIL;
                    c.miss = 0x4F05_0000 | (bh as u32);
                }
            }
        }

        // ---- 4F06h: Set/Get Logical Scan Line Length ---------------------
        0x06 => {
            if s.mode == 0 {
                c.ax = FAIL;
                return 0;
            }
            let bytes_per_px = (s.bpp as u32) / 8;
            let mut newbpl = s.bpl;
            match bl {
                0x00 => newbpl = (c.cx as u32) * bytes_per_px, // set in PIXELS
                0x02 => newbpl = c.cx as u32,                  // set in BYTES
                0x01 => {}                                     // get current
                0x03 => {
                    // Get MAXIMUM scan line length.
                    c.bx = (s.vram.min(0xFFFF)) as u16;
                    c.cx = (s.vram / bytes_per_px.max(1)).min(0xFFFF) as u16;
                    c.dx = 1;
                    c.ax = OK;
                    return 0;
                }
                _ => {
                    c.ax = FAIL;
                    c.miss = 0x4F06_0000 | (bl as u32);
                    return 0;
                }
            }
            if bl == 0x00 || bl == 0x02 {
                // A scan line narrower than the display, or one so wide that a
                // single line does not fit in VRAM, is not representable.
                let min = (s.width as u32) * bytes_per_px;
                if newbpl < min || newbpl > s.vram {
                    c.ax = FAIL;
                    return 0;
                }
                s.bpl = newbpl;
                // A wider logical line means the display start can no longer
                // be where it was; the spec leaves this to the program, but a
                // start that now points past VRAM would make the present path
                // read out of bounds, so re-clamp it here where the invariant
                // is owned rather than in the pixel loop.
                if s.disp_start >= s.vram {
                    s.disp_start = 0;
                }
            }
            // All three set/get forms return the resulting geometry.
            c.bx = s.bpl.min(0xFFFF) as u16;
            c.cx = (s.bpl / bytes_per_px.max(1)).min(0xFFFF) as u16;
            c.dx = (s.vram / s.bpl.max(1)).min(0xFFFF) as u16;
            c.ax = OK;
        }

        // ---- 4F07h: Set/Get Display Start --------------------------------
        0x07 => {
            if s.mode == 0 {
                c.ax = FAIL;
                return 0;
            }
            let bytes_per_px = (s.bpp as u32) / 8;
            match bl {
                // 0x00 set, 0x80 set during vertical retrace. We present from
                // a snapshot each frame rather than racing a real CRT beam, so
                // the two are the same action here; the distinction only ever
                // mattered for tearing on real hardware.
                0x00 | 0x80 => {
                    let start = (c.dx as u32)
                        .wrapping_mul(s.bpl)
                        .wrapping_add((c.cx as u32).wrapping_mul(bytes_per_px));
                    // The whole visible page must fit, or the present path
                    // would read past VRAM on the last scan lines.
                    let visible = s.bpl.wrapping_mul(s.height as u32);
                    if start.checked_add(visible).map_or(true, |e| e > s.vram) {
                        c.ax = FAIL;
                        return 0;
                    }
                    s.disp_start = start;
                    c.ax = OK;
                }
                0x01 => {
                    // Get. Report it back in the same (pixel, line) form.
                    let line = s.disp_start / s.bpl.max(1);
                    let px = (s.disp_start % s.bpl.max(1)) / bytes_per_px.max(1);
                    c.dx = line.min(0xFFFF) as u16;
                    c.cx = px.min(0xFFFF) as u16;
                    c.bx = 0;
                    c.ax = OK;
                }
                _ => {
                    c.ax = FAIL;
                    c.miss = 0x4F07_0000 | (bl as u32);
                }
            }
        }

        // ---- 4F08h: Set/Get DAC Palette Format ---------------------------
        0x08 => {
            match bl {
                0x00 => {
                    // Set. BH = 6 or 8 bits per primary.
                    if bh == 6 {
                        s.dac8 = 0;
                    } else if bh == 8 {
                        s.dac8 = 1;
                    } else {
                        c.ax = FAIL;
                        return 0;
                    }
                    c.bx = ((if s.dac8 == 1 { 8u16 } else { 6u16 }) << 8) | (bl as u16);
                    c.ax = OK;
                }
                0x01 => {
                    c.bx = ((if s.dac8 == 1 { 8u16 } else { 6u16 }) << 8) | (bl as u16);
                    c.ax = OK;
                }
                _ => {
                    c.ax = FAIL;
                    c.miss = 0x4F08_0000 | (bl as u32);
                }
            }
        }

        // ---- 4F09h: Set/Get Palette Data ---------------------------------
        //
        // The guest table is 4 bytes per entry, in the order BLUE, GREEN, RED,
        // alignment. That byte order is the single easiest thing to get
        // backwards in this whole file, and getting it backwards produces a
        // picture that looks plausible, so it is stated here and exercised by
        // vbe_selftest_rs().
        0x09 => {
            let count = c.cx as usize;
            let start = c.dx as usize;
            if start + count > 256 {
                c.ax = FAIL;
                return 0;
            }
            if c.pal.is_null() {
                c.ax = FAIL;
                return 0;
            }
            let need = (count * 4) as u32;
            let getting = bl == 0x01;
            if c.buf.is_null() || c.buflen < need {
                c.ax = FAIL;
                return 0;
            }
            // SAFETY: `pal` non-null (checked) and the C caller passes
            // `&t->pal[0][0]`, which is 256*3 = 768 bytes. `buf` non-null with
            // at least `need` bytes, checked immediately above.
            let pal = unsafe { core::slice::from_raw_parts_mut(c.pal, 768) };
            let tbl = unsafe { core::slice::from_raw_parts_mut(c.buf, need as usize) };
            match bl {
                0x00 | 0x80 => {
                    for i in 0..count {
                        let b = tbl[i * 4];
                        let g = tbl[i * 4 + 1];
                        let r = tbl[i * 4 + 2];
                        pal[(start + i) * 3] = r;
                        pal[(start + i) * 3 + 1] = g;
                        pal[(start + i) * 3 + 2] = b;
                    }
                    c.ax = OK;
                }
                0x01 => {
                    for i in 0..count {
                        tbl[i * 4] = pal[(start + i) * 3 + 2];
                        tbl[i * 4 + 1] = pal[(start + i) * 3 + 1];
                        tbl[i * 4 + 2] = pal[(start + i) * 3];
                        tbl[i * 4 + 3] = 0;
                    }
                    c.ax = OK;
                }
                _ => {
                    c.ax = FAIL;
                    c.miss = 0x4F09_0000 | (bl as u32);
                }
            }
            let _ = getting;
        }

        // ---- 4F0Ah: Protected Mode Interface -----------------------------
        //
        // Refused ON PURPOSE, and this is the cheapest correct decision in the
        // design. It returns a pointer to bank-switch code a protected-mode
        // program calls without a real-mode round trip: an OPTIMISATION, and
        // every conformant program falls back to INT 10h when refused. Since
        // no program can then escape our INT 10h handler through it, the whole
        // VBE surface stays observable and stays independent of the DOS/4GW
        // extender work.
        0x0A => {
            c.ax = FAIL;
            c.miss = 0x4F0A;
        }

        // ---- 4F0Bh: Get/Set Pixel Clock ----------------------------------
        0x0B => {
            c.ax = FAIL;
            c.miss = 0x4F0B;
        }

        // ---- everything else ---------------------------------------------
        _ => {
            c.ax = FAIL;
            c.miss = 0x4F00_0000 | (al as u32);
        }
    }
    0
}

// ---------------------------------------------------------------------------
// Self-test. Called once at DOS guest launch beside dos_letterbox_selftest_rs.
// It costs a few thousand integer operations and it is the difference between
// a structure layout that is asserted and one that has been watched being
// right. Returns the number of FAILING cases, so 0 is the only good answer.
// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn vbe_selftest_rs() -> i32 {
    let mut bad = 0;
    let mut rom = [0u8; ROM_SIZE as usize];
    let mut buf = [0u8; 1024];
    let mut pal = [0u8; 768];

    // --- the ROM blob -----------------------------------------------------
    if vbe_build_rom_rs(rom.as_mut_ptr(), ROM_SIZE) != ROM_SIZE {
        bad += 1;
    }
    // The bank stub must be the exact 6 bytes, because a guest may far-call it.
    if rom[0..6] != BANK_STUB {
        bad += 1;
    }
    if &rom[ROM_OFF_OEM as usize..ROM_OFF_OEM as usize + 6] != b"Mayter" {
        bad += 1;
    }
    // The mode list must hold every mode, in order, 0xFFFF terminated.
    let ml = ROM_OFF_MODELIST as usize;
    for (i, md) in MODES.iter().enumerate() {
        let v = (rom[ml + i * 2] as u16) | ((rom[ml + i * 2 + 1] as u16) << 8);
        if v != md.number {
            bad += 1;
        }
    }
    let term = ml + MODES.len() * 2;
    if rom[term] != 0xFF || rom[term + 1] != 0xFF {
        bad += 1;
    }
    // A short buffer must be refused, not partially written.
    if vbe_build_rom_rs(rom.as_mut_ptr(), ROM_SIZE - 1) != 0 {
        bad += 1;
    }
    if vbe_build_rom_rs(core::ptr::null_mut(), ROM_SIZE) != 0 {
        bad += 1;
    }

    let mut st = VbeState {
        mode: 0, width: 0, height: 0, bpp: 0, dac8: 0,
        bpl: 0, bank: 0, disp_start: 0, vram: 0,
    };

    // --- 4F00h ------------------------------------------------------------
    let mut call = VbeCall {
        ax: 0x4F00, bx: 0, cx: 0, dx: 0,
        buf: buf.as_mut_ptr(), buflen: 256,
        pal: pal.as_mut_ptr(), action: 0, miss: 0,
    };
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != OK { bad += 1; }
    if &buf[0..4] != b"VESA" { bad += 1; }
    if (buf[4] as u16 | ((buf[5] as u16) << 8)) != 0x0102 { bad += 1; }
    // TotalMemory in 64 KB units must equal what we will actually allocate.
    if (buf[0x12] as u16 | ((buf[0x13] as u16) << 8)) != (VRAM_BYTES / 65536) as u16 { bad += 1; }
    // The VideoModePtr must point at the mode list we just built.
    let vmp = (buf[0x0E] as u32) | ((buf[0x0F] as u32) << 8)
            | ((buf[0x10] as u32) << 16) | ((buf[0x11] as u32) << 24);
    if vmp != farptr(ROM_SEG, ROM_OFF_MODELIST) { bad += 1; }
    // A null / short buffer must FAIL, not scribble.
    call.buf = core::ptr::null_mut();
    call.ax = 0x4F00;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != FAIL { bad += 1; }
    call.buf = buf.as_mut_ptr();

    // --- 4F01h ------------------------------------------------------------
    call.ax = 0x4F01; call.cx = 0x101; call.buflen = 256;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != OK { bad += 1; }
    if (buf[0x12] as u16 | ((buf[0x13] as u16) << 8)) != 640 { bad += 1; }
    if (buf[0x14] as u16 | ((buf[0x15] as u16) << 8)) != 480 { bad += 1; }
    if (buf[0x10] as u16 | ((buf[0x11] as u16) << 8)) != 640 { bad += 1; }
    if buf[0x19] != 8 { bad += 1; }
    if buf[0x1B] != 4 { bad += 1; }
    // LFB must NOT be advertised (bit 7) and the mode must be supported (bit 0).
    let attr = buf[0x00] as u16 | ((buf[0x01] as u16) << 8);
    if attr & 0x01 == 0 { bad += 1; }
    if attr & 0x80 != 0 { bad += 1; }
    if (buf[0x08] as u16 | ((buf[0x09] as u16) << 8)) != 0xA000 { bad += 1; }
    // An unknown mode must fail.
    call.ax = 0x4F01; call.cx = 0x4242;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != FAIL { bad += 1; }

    // --- 4F02h ------------------------------------------------------------
    // A linear-framebuffer request must be REFUSED (bit 14).
    call.ax = 0x4F02; call.bx = 0x4101;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != FAIL { bad += 1; }
    if call.miss == 0 { bad += 1; }
    // The real thing.
    call.ax = 0x4F02; call.bx = 0x0101;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != OK { bad += 1; }
    if call.action != ACT_SET_MODE { bad += 1; }
    if st.mode != 0x101 || st.width != 640 || st.height != 480 || st.bpl != 640 { bad += 1; }
    st.vram = VRAM_BYTES; // C does this after it allocates; fake it here.

    // A standard VGA mode through the VBE entry point routes to the VGA path.
    call.ax = 0x4F02; call.bx = 0x0003;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != OK || call.action != ACT_SET_VGA || st.mode != 0 { bad += 1; }
    // Put it back for the remaining cases.
    call.ax = 0x4F02; call.bx = 0x0101;
    vbe_dispatch_rs(&mut st, &mut call);
    st.vram = VRAM_BYTES;

    // --- 4F03h ------------------------------------------------------------
    call.ax = 0x4F03; call.bx = 0;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != OK || call.bx != 0x101 { bad += 1; }

    // --- 4F05h, the bank switch ------------------------------------------
    call.ax = 0x4F05; call.bx = 0x0000; call.dx = 3;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != OK || st.bank != 3 { bad += 1; }
    call.ax = 0x4F05; call.bx = 0x0100; call.dx = 0;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != OK || call.dx != 3 { bad += 1; }
    // A bank past the end of VRAM must FAIL rather than clamp.
    call.ax = 0x4F05; call.bx = 0x0000; call.dx = 999;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != FAIL || st.bank != 3 { bad += 1; }
    // Window B does not exist.
    call.ax = 0x4F05; call.bx = 0x0001; call.dx = 0;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != FAIL { bad += 1; }

    // --- 4F06h, logical scan line ----------------------------------------
    call.ax = 0x4F06; call.bx = 0x0000; call.cx = 1024; // set, in PIXELS
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != OK || st.bpl != 1024 { bad += 1; }
    if call.bx != 1024 || call.cx != 1024 { bad += 1; }
    // Narrower than the display is not representable.
    call.ax = 0x4F06; call.bx = 0x0000; call.cx = 100;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != FAIL || st.bpl != 1024 { bad += 1; }
    // Back to native.
    call.ax = 0x4F06; call.bx = 0x0002; call.cx = 640; // set, in BYTES
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != OK || st.bpl != 640 { bad += 1; }

    // --- 4F07h, display start --------------------------------------------
    call.ax = 0x4F07; call.bx = 0x0000; call.cx = 0; call.dx = 100;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != OK || st.disp_start != 100 * 640 { bad += 1; }
    call.ax = 0x4F07; call.bx = 0x0001; call.cx = 0; call.dx = 0;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != OK || call.dx != 100 { bad += 1; }
    // A start whose visible page would run past VRAM must FAIL, because the
    // present path would otherwise read out of bounds.
    call.ax = 0x4F07; call.bx = 0x0000; call.cx = 0; call.dx = 60000;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != FAIL { bad += 1; }
    call.ax = 0x4F07; call.bx = 0x0000; call.cx = 0; call.dx = 0;
    vbe_dispatch_rs(&mut st, &mut call);

    // --- 4F08h, DAC width -------------------------------------------------
    call.ax = 0x4F08; call.bx = 0x0800; // BH=8, BL=0 set
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != OK || st.dac8 != 1 || (call.bx >> 8) != 8 { bad += 1; }
    call.ax = 0x4F08; call.bx = 0x0001; // BL=1 get
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != OK || (call.bx >> 8) != 8 { bad += 1; }
    call.ax = 0x4F08; call.bx = 0x0700; // BH=7 is not a thing
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != FAIL || st.dac8 != 1 { bad += 1; }
    call.ax = 0x4F08; call.bx = 0x0600;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != OK || st.dac8 != 0 { bad += 1; }

    // --- 4F09h, palette. The BGRA byte order is the trap; prove it. -------
    buf[0] = 0x11; buf[1] = 0x22; buf[2] = 0x33; buf[3] = 0x00; // B=11 G=22 R=33
    call.ax = 0x4F09; call.bx = 0x0000; call.cx = 1; call.dx = 5; call.buflen = 4;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != OK { bad += 1; }
    if pal[5 * 3] != 0x33 || pal[5 * 3 + 1] != 0x22 || pal[5 * 3 + 2] != 0x11 { bad += 1; }
    // Read it back and get the same bytes in the same order.
    buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 0xEE;
    call.ax = 0x4F09; call.bx = 0x0001; call.cx = 1; call.dx = 5; call.buflen = 4;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != OK { bad += 1; }
    if buf[0] != 0x11 || buf[1] != 0x22 || buf[2] != 0x33 || buf[3] != 0x00 { bad += 1; }
    // A range that runs off the end of the DAC must fail.
    call.ax = 0x4F09; call.bx = 0x0000; call.cx = 10; call.dx = 250; call.buflen = 40;
    vbe_dispatch_rs(&mut st, &mut call);
    if call.ax != FAIL { bad += 1; }

    // --- the deliberate refusals --------------------------------------------
    // Each must return the SPEC'S FAILURE, not leave AX alone, and must report
    // a miss so the C side logs it exactly once. This is the blame.md rule.
    for sub in [0x04u16, 0x0A, 0x0B, 0x1E] {
        call.ax = 0x4F00 | sub;
        call.miss = 0;
        vbe_dispatch_rs(&mut st, &mut call);
        if call.ax != FAIL { bad += 1; }
        if call.miss == 0 { bad += 1; }
    }

    // --- null state / null call must not fault -----------------------------
    if vbe_dispatch_rs(core::ptr::null_mut(), &mut call) != -1 { bad += 1; }
    if vbe_dispatch_rs(&mut st, core::ptr::null_mut()) != -1 { bad += 1; }

    bad
}

// ===========================================================================
// THE PRESENT LOOP.
// ---------------------------------------------------------------------------
// This is in RUST, and the reason is a MEASUREMENT that came out the opposite
// way to the assumption it was written under.
//
// The 2026-07-16 rule allows C for a measured performance reason, and a
// per-destination-pixel present loop is exactly the kind of place that would
// normally earn one. So this loop was first written in C, and then the claim
// was TESTED rather than asserted: the identical algorithm was compiled both
// ways with the kernel's own codegen flags (-O2 -mno-mmx -mno-sse -mno-sse2
// -mno-red-zone -fno-pic, gcc 12.2 vs rustc 1.97.0), best of 5 runs of 200
// iterations, with the dimensions taken from `volatile` globals so that
// nothing could be constant-folded. C and Rust came out within 1-4% of each
// other, and at 1024x768 the RUST version was the faster of the two. The
// numbers are in the CHANGELOG entry for #740.
//
// There is therefore no performance reason to write it in C, so it is not in C.
//
// The same measurement settled the algorithm, and there the received wisdom
// held: the divide-free accumulator is 2.6x faster than a per-pixel
// `dx*gw/sw`, in BOTH languages. (An earlier version of the benchmark made the
// divide look 5x FASTER, because it was called with literal dimensions and gcc
// folded `dx*640/640` into `dx`. A benchmark that measures a loop the compiler
// deleted is worse than no benchmark.) The two forms were also proven to pick
// the identical source column in all 42,207 (gw, sw) combinations the mode
// table can produce, so this is a speed choice and not a picture choice.
//
// Integer only, because the kernel is soft-float with SSE disabled: the 6-bit
// to 8-bit expansion is an integer divide by a constant, which becomes a
// multiply-and-shift.

// Mirrored by `vbe_present_t` in dos/dosexec.c with a _Static_assert on the size.
#[repr(C)]
pub struct VbePresent {
    pub dst: *mut u32,      // the host window's ARGB content buffer
    pub src: *const u8,     // guest VRAM
    pub pal: *const u8,     // 256 * (r,g,b)
    pub dst_stride: i32,    // in pixels
    pub dst_w: i32,
    pub dst_h: i32,
    pub x: i32,             // the letterboxed picture rectangle, from
    pub y: i32,             // dos_letterbox_rs(), which the INPUT path also
    pub w: i32,             // uses, so they cannot disagree about where the
    pub h: i32,             // picture is
    pub gw: i32,            // guest resolution
    pub gh: i32,
    pub src_len: u32,
    pub bpl: u32,           // bytes per scan line (4F06h)
    pub disp_start: u32,    // display start in bytes (4F07h)
    pub dac8: u8,           // 4F08h: 0 = 6-bit primaries, 1 = 8-bit
}

/// Present an 8bpp packed VESA mode into the host window. Returns 0 on success,
/// -1 if any argument is unusable (and then nothing is drawn, rather than
/// something being drawn out of bounds).
///
/// # Safety
/// `dst` must point to at least `dst_stride * dst_h` u32s, `src` to at least
/// `src_len` bytes and `pal` to 768 bytes.
#[no_mangle]
pub extern "C" fn vbe_present_rs(p: *const VbePresent) -> i32 {
    if p.is_null() {
        return -1;
    }
    // SAFETY: non-null, checked. The C caller passes the address of a
    // vbe_present_t whose layout is locked to this type by a _Static_assert.
    let p = unsafe { &*p };
    if p.dst.is_null() || p.src.is_null() || p.pal.is_null() {
        return -1;
    }
    if p.w <= 0 || p.h <= 0 || p.gw <= 0 || p.gh <= 0 {
        return -1;
    }
    if p.dst_w <= 0 || p.dst_h <= 0 || p.dst_stride < p.dst_w {
        return -1;
    }
    // The picture rectangle comes from dos_letterbox_rs() and is already inside
    // the buffer, but it is re-checked HERE, at the boundary, because "the
    // caller already checked" is how an out-of-bounds write gets written.
    if p.x < 0 || p.y < 0 || p.x + p.w > p.dst_w || p.y + p.h > p.dst_h {
        return -1;
    }

    let dst_len = (p.dst_stride as usize) * (p.dst_h as usize);
    // SAFETY: bounds established immediately above; the C caller owns a buffer
    // of exactly win_w * win_h u32s and passes those as dst_stride/dst_w/dst_h.
    let dst = unsafe { core::slice::from_raw_parts_mut(p.dst, dst_len) };
    let src = unsafe { core::slice::from_raw_parts(p.src, p.src_len as usize) };
    let pal = unsafe { core::slice::from_raw_parts(p.pal, 768) };

    // Built once per present rather than per pixel. After 4F08h the DAC holds
    // 8-bit primaries and the *255/63 expansion would overflow, so the choice
    // is made here, once, and not inside the pixel loop.
    let mut lut = [0u32; 256];
    if p.dac8 != 0 {
        for i in 0..256 {
            lut[i] = 0xFF00_0000
                | ((pal[i * 3] as u32) << 16)
                | ((pal[i * 3 + 1] as u32) << 8)
                | (pal[i * 3 + 2] as u32);
        }
    } else {
        for i in 0..256 {
            let r = (pal[i * 3] as u32) * 255 / 63;
            let g = (pal[i * 3 + 1] as u32) * 255 / 63;
            let b = (pal[i * 3 + 2] as u32) * 255 / 63;
            lut[i] = 0xFF00_0000 | (r << 16) | (g << 8) | b;
        }
    }

    let stride = p.dst_stride as usize;
    let base = (p.y as usize) * stride + (p.x as usize);
    let sw = p.w;
    let sh = p.h;
    let mut prev_sy: i32 = -1;
    let mut prev_off: usize = 0;

    for dy in 0..sh {
        let mut sy = dy * p.gh / sh;
        if sy >= p.gh {
            sy = p.gh - 1;
        }
        let doff = base + (dy as usize) * stride;
        if sy == prev_sy {
            // Identical source row: copy the one already converted. At any
            // upscale this is most of the rows.
            dst.copy_within(prev_off..prev_off + sw as usize, doff);
            continue;
        }
        prev_sy = sy;
        prev_off = doff;
        // 4F07h moves the display start and 4F06h can make the logical scan
        // line wider than the display, so the source row is
        // disp_start + sy*bpl, NOT sy*width. Getting that wrong is the classic
        // stride bug: it shears the picture progressively down the screen,
        // which is exactly what the test program's vertical colour bars are
        // there to make unmissable.
        let rowoff = (p.disp_start as usize) + (sy as usize) * (p.bpl as usize);
        if rowoff + (p.gw as usize) > src.len() {
            // Unreachable while 4F06h/4F07h validate, and checked anyway: a
            // guest-controlled offset indexing past VRAM is a kernel read
            // overrun, not a cosmetic fault.
            for dx in 0..sw as usize {
                dst[doff + dx] = 0xFF00_0000;
            }
            continue;
        }
        let srow = &src[rowoff..rowoff + p.gw as usize];
        let drow = &mut dst[doff..doff + sw as usize];
        let mut sxi: i32 = 0;
        let mut sxacc: i32 = 0;
        for dx in 0..sw as usize {
            let mut sx = sxi;
            sxacc += p.gw;
            while sxacc >= sw {
                sxacc -= sw;
                sxi += 1;
            }
            if sx >= p.gw {
                sx = p.gw - 1;
            }
            drow[dx] = lut[srow[sx as usize] as usize];
        }
    }
    0
}
