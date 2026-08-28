//! earlyfb.rs - the boot-stage tracker that paints DIRECTLY to the firmware
//! framebuffer, usable from the FIRST instruction of kernel_main.
//!
//! WHY THIS EXISTS (#307-class, generalised to non-Apple hardware).
//!
//! Every diagnostic this kernel emits goes to serial via kprintf, and the
//! persistent copies (/BOOTLOG.TXT, /USBLOG.TXT, /AUDIOLOG.TXT, /boot/STAGE.TXT)
//! only reach a medium once a filesystem is mounted: bootlog_arm() is gated on
//! g_fat_fs.mounted and panic_log_init() on the same. A LAPTOP HAS NO SERIAL
//! PORT. So on a laptop, a failure before the mount writes NOTHING, ANYWHERE,
//! and the machine is indistinguishable from a brick. That is precisely the gap
//! that made the iMac14,4 undiagnosable for days, and on unknown hardware dying
//! before the mount is a LIKELY outcome, not an edge case.
//!
//! The one channel that is guaranteed available is the SCREEN. The UEFI GOP
//! linear framebuffer is already live when the firmware calls the kernel: the
//! bootloader captured its address, geometry and pixel format into boot_info
//! BEFORE ExitBootServices, and the UEFI identity map that the kernel keeps in
//! CR3 for its whole life already maps it. So the framebuffer is writable
//! before the GDT is loaded, before the PIC is remapped, before there is a
//! physical allocator, a heap or an IDT.
//!
//! The kernel's OWN framebuffer stack cannot be used for this. fb_init() is
//! ordered after heap_init() because it kmalloc_aligned()s a back buffer
//! (video/framebuffer.c), and console_init() is called at main.c:961, which is
//! AFTER gdt_init, idt_init, pic_init, pit_init, mono_init, isr_init, sse_init,
//! syscall_init, pmm_init, vmm_init, heap_init and demand_init. Every one of
//! those is a plausible place for an unfamiliar machine to die, and every one of
//! them was previously invisible. This module has NO dependency on any of them:
//! all state is .bss (zeroed by entry.asm's rep stosq before kernel_main is
//! called) and every write is a volatile store to the physical address the
//! firmware handed us.
//!
//! WHY RUST. Project rule: new kernel code is Rust unless there is a stated
//! performance reason. There is none here; this runs a few dozen times per boot.
//! The work is bounds arithmetic over a caller-supplied geometry (width, height,
//! pitch, bits per pixel) that comes from FIRMWARE and is therefore untrusted
//! input, driving raw stores into an 8 MB MMIO aperture. That is exactly the
//! shape of thing worth having the bounds written once, in one place, in a
//! language that will not let the index silently wander. The only C we borrow is
//! font_get_glyph(), the shared 8x16 font primitive, because forking a private
//! copy of the font is the anti-pattern this project keeps paying for.
//!
//! WHAT IT DRAWS
//!   - a two-line header: build identity, and the firmware framebuffer geometry
//!     (so a mode we render wrongly is legible from a photo of the failure)
//!   - a scrolling numbered list of stages reached
//!   - a LARGE stage number in the top-right, readable from a phone photo
//!   - a solid banner across the bottom naming the CURRENT stage
//! The banner is the load-bearing part. It is repainted LAST on every call and
//! it goes straight to the FRONT buffer, so it is whatever is on the glass when
//! a hang freezes the machine. Power the machine off, look at the photograph,
//! and you have the name of the last stage the kernel entered.
//!
//! ROTATION: this module deliberately writes UNROTATED physical pixels. It runs
//! before fb_init() has read display_rotation, and a diagnostic that depends on
//! a config file read from a filesystem that may never mount would defeat its
//! own purpose. On a rotated panel the banner is sideways but still readable.

use core::ptr;

extern "C" {
    /// video/font.c. Returns 16 bytes, one per glyph row, MSB leftmost.
    /// The shared 8x16 font primitive: not re-implemented here on purpose.
    fn font_get_glyph(c: u8) -> *const u8;
}

const GW: u32 = 8; // FONT_WIDTH
const GH: u32 = 16; // FONT_HEIGHT

/// Sanity bounds on the FIRMWARE-supplied geometry. A GOP mode outside these is
/// not something we can render, and clamping silently would put the stores off
/// the end of the aperture, which is the one bug this module must not have.
const MIN_DIM: u32 = 64;
const MAX_DIM: u32 = 16384;

// All state is .bss. entry.asm zeroes .bss with rep stosq before kernel_main is
// entered, so every one of these reads as 0 on the first call with no
// initialiser having run.
static mut FB_BASE: u64 = 0;
static mut FB_W: u32 = 0;
static mut FB_H: u32 = 0;
static mut FB_PITCH: u32 = 0;
static mut FB_BGR: u32 = 0; // 1 = red occupies bits 16..23 (PIXEL_FORMAT_BGR)
static mut FB_BPP: u32 = 0; // kept for the geometry header line, which is painted
                            // by arm_screen() and may happen long after init
// TWO flags, not one, and the distinction is the whole quiet-boot design.
//   FB_VALID  the firmware geometry was checked and is usable. Set at init on
//             EVERY boot, armed or not, so a later arm-on-demand (the no-root
//             report) does not have to re-derive or re-trust it.
//   FB_READY  we OWN the screen and are painting on it. Set only when armed.
// Every drawing entry point tests FB_READY, so an unarmed boot leaves the glass
// exactly as the firmware left it and the splash comes up clean.
static mut FB_VALID: u32 = 0;
static mut FB_READY: u32 = 0;
static mut LOG_ROW: u32 = 0; // next free text row in the scrolling area
static mut LOG_TOP: u32 = 0; // first text row of the scrolling area
static mut LOG_BOT: u32 = 0; // one past the last text row of the scrolling area
static mut LAST_STAGE: u32 = 0;

// ---------------------------------------------------------------- primitives

#[inline(always)]
unsafe fn px(x: u32, y: u32 , c: u32) {
    if x >= FB_W || y >= FB_H {
        return;
    }
    let off = (y as u64) * (FB_PITCH as u64) + (x as u64) * 4;
    // volatile: this is an MMIO aperture, not memory. The compiler must not
    // coalesce, reorder or elide these, and on a hang the value that matters is
    // the one already committed.
    ptr::write_volatile((FB_BASE + off) as *mut u32, c);
}

/// Compose a pixel word for the firmware's actual channel order. The kernel's
/// own FB_COLOR() macro hardcodes one order and video/framebuffer.c sets
/// fb_is_bgr and then never reads it, so the rest of the kernel is order-blind.
/// This module is not: it has the format from boot_info and uses it, because a
/// diagnostic that renders in the wrong channel order on unfamiliar firmware is
/// still legible but its colour coding lies.
#[inline(always)]
unsafe fn rgb(r: u8, g: u8, b: u8) -> u32 {
    if FB_BGR != 0 {
        ((r as u32) << 16) | ((g as u32) << 8) | (b as u32)
    } else {
        ((b as u32) << 16) | ((g as u32) << 8) | (r as u32)
    }
}

unsafe fn fill(x: u32, y: u32, w: u32, h: u32, c: u32) {
    let mut yy = y;
    let ymax = y.saturating_add(h);
    while yy < ymax && yy < FB_H {
        let mut xx = x;
        let xmax = x.saturating_add(w);
        while xx < xmax && xx < FB_W {
            px(xx, yy, c);
            xx += 1;
        }
        yy += 1;
    }
}

/// Draw one glyph at pixel (x,y), scaled by `s` in both axes.
unsafe fn glyph(x: u32, y: u32, c: u8, fg: u32, bg: u32, opaque: bool, s: u32) {
    let g = font_get_glyph(c);
    if g.is_null() {
        return;
    }
    let s = if s == 0 { 1 } else { s };
    let mut row = 0u32;
    while row < GH {
        let bits = ptr::read(g.add(row as usize));
        let mut col = 0u32;
        while col < GW {
            let on = (bits >> (7 - col)) & 1 != 0;
            if on || opaque {
                let pv = if on { fg } else { bg };
                if s == 1 {
                    px(x + col, y + row, pv);
                } else {
                    fill(x + col * s, y + row * s, s, s, pv);
                }
            }
            col += 1;
        }
        row += 1;
    }
}

/// Length of a NUL-terminated C string, hard-capped. A firmware-era diagnostic
/// must not be able to run off the end of a bad pointer looking for a NUL.
unsafe fn clen(s: *const u8, max: usize) -> usize {
    if s.is_null() {
        return 0;
    }
    let mut n = 0usize;
    while n < max && ptr::read(s.add(n)) != 0 {
        n += 1;
    }
    n
}

unsafe fn text(x: u32, y: u32, s: *const u8, fg: u32, bg: u32, opaque: bool, sc: u32) -> u32 {
    let n = clen(s, 512);
    let mut i = 0usize;
    let mut cx = x;
    while i < n {
        let ch = ptr::read(s.add(i));
        // Anything unprintable becomes a dot rather than a random glyph, so a
        // corrupt string is obviously corrupt instead of quietly plausible.
        let ch = if (32..127).contains(&ch) { ch } else { b'.' };
        if cx + GW * sc > FB_W {
            break;
        }
        glyph(cx, y, ch, fg, bg, opaque, sc);
        cx += GW * sc;
        i += 1;
    }
    cx
}

/// Render `v` right-aligned in `width` digits into `buf`, returning the slice
/// start. Pure integer formatting: the kernel is soft-float and this runs before
/// any formatter is proven, so it does not borrow vsnprintf.
fn u32_dec(v: u32, buf: &mut [u8; 12], width: usize) -> usize {
    let mut n = v;
    let mut i = buf.len() - 1;
    buf[i] = 0;
    let end = i;
    loop {
        i -= 1;
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
        if n == 0 {
            break;
        }
    }
    while end - i < width && i > 0 {
        i -= 1;
        buf[i] = b'0';
    }
    i
}

unsafe fn text_u32(x: u32, y: u32, v: u32, w: usize, fg: u32, bg: u32, op: bool, sc: u32) -> u32 {
    let mut b = [0u8; 12];
    let i = u32_dec(v, &mut b, w);
    text(x, y, b.as_ptr().add(i), fg, bg, op, sc)
}

fn hex_u64(v: u64, buf: &mut [u8; 20]) {
    const D: &[u8; 16] = b"0123456789ABCDEF";
    buf[0] = b'0';
    buf[1] = b'x';
    let mut i = 0usize;
    while i < 16 {
        buf[2 + i] = D[((v >> (60 - i * 4)) & 0xF) as usize];
        i += 1;
    }
    buf[18] = 0;
}

// ------------------------------------------------------------------ the frame

unsafe fn banner_y() -> u32 {
    // Bottom band: three text rows' worth, clamped so it always fits.
    if FB_H > GH * 4 {
        FB_H - GH * 3
    } else {
        0
    }
}

/// Repaint the bottom banner with the current stage. Called LAST by every entry
/// point below, and callable on its own after the kernel's own framebuffer stack
/// has taken over (see early_fb_banner_refresh), because this writes to the
/// FRONT buffer while gfx_boot_log() draws into the back buffer and swaps. So
/// the order "gfx_boot_log(); early_fb_banner_refresh();" leaves the banner as
/// the last thing committed to the glass.
unsafe fn paint_banner(stage: u32, name: *const u8) {
    if FB_READY == 0 {
        return;
    }
    let by = banner_y();
    let ink = rgb(255, 255, 255);
    let bar = rgb(24, 32, 72);
    let edge = rgb(120, 140, 200);
    fill(0, by, FB_W, GH * 3, bar);
    fill(0, by, FB_W, 2, edge);
    fill(0, by + GH * 3 - 2, FB_W, 2, edge);

    let y = by + GH / 2;
    let mut x = text(GW, y, b"STAGE \0".as_ptr(), ink, bar, false, 1);
    x = text_u32(x, y, stage, 2, ink, bar, false, 1);
    x = text(x, y, b"  \0".as_ptr(), ink, bar, false, 1);
    text(x, y, name, ink, bar, false, 1);
}

/// The large top-right stage number. Redundant with the banner ON PURPOSE: it is
/// legible from a phone photograph taken at arm's length of a laptop screen,
/// which is the actual retrieval mechanism for this diagnostic.
unsafe fn paint_bignum(stage: u32) {
    if FB_READY == 0 {
        return;
    }
    let sc = 3u32;
    let w = GW * sc * 2;
    let h = GH * sc;
    if FB_W < w + GW * 2 || FB_H < h + GH {
        return;
    }
    let x = FB_W - w - GW;
    let y = GH / 2;
    let bg = rgb(0, 0, 0);
    fill(x, y, w, h, bg);
    text_u32(x, y, stage % 100, 2, rgb(255, 200, 80), bg, false, sc);
}

// ---------------------------------------------------------------- public API

/// Latch the firmware-supplied geometry, and if `arm` is non-zero take the
/// screen over and start painting on it.
///
/// QUIET BOOT (`arm == 0`) is the DEFAULT and is what a normal user sees: the
/// geometry is validated and recorded, nothing is drawn, and every other
/// drawing entry point in this module is a no-op. The persistent channels
/// (/BOOTLOG.TXT, the raw-LBA flight recorder, serial) are not touched by this
/// flag at all and keep recording exactly as before, which is the point: the
/// evidence survives, only the ugly screen goes away.
///
/// ARMED (`arm != 0`) is the behaviour this module was written for, reached by
/// putting \boot\DIAG.TXT on the ESP. See fs/bootstage.h.
///
/// Returns 1 if the geometry is usable (whether or not it was armed), 0 if not.
/// A 0 means the machine is running blind on the screen channel and the caller
/// should say so: it is NOT the same answer as "quiet", and callers must not
/// conflate them, which is why early_fb_valid() and early_fb_ready() are
/// separate calls.
///
/// # Safety
/// `addr` must be the linear framebuffer physical address from the UEFI GOP, and
/// the identity mapping the firmware left in CR3 must still be active. Both hold
/// at kernel_main entry.
#[no_mangle]
pub unsafe extern "C" fn early_fb_init(
    addr: u64,
    w: u32,
    h: u32,
    pitch: u32,
    bpp: u32,
    pixfmt: u32,
    arm: u32,
) -> i32 {
    FB_READY = 0;
    FB_VALID = 0;
    if addr == 0 {
        return 0;
    }
    // 32bpp only, and we REFUSE rather than guess. The bootloader hardcodes
    // bpp=32 and pitch=PixelsPerScanLine*4 regardless of the real GOP
    // PixelFormat, so a 16/24bpp mode arrives here mislabelled as 32; there is
    // nothing this module can do about that except decline to render garbage if
    // the value ever does arrive honest.
    if bpp != 32 {
        return 0;
    }
    if w < MIN_DIM || w > MAX_DIM || h < MIN_DIM || h > MAX_DIM {
        return 0;
    }
    // A pitch below width*4 would put row N's tail inside row N+1. A pitch
    // absurdly above it is more likely a garbage field than a real mode.
    if pitch < w.saturating_mul(4) || pitch > MAX_DIM.saturating_mul(8) {
        return 0;
    }

    FB_BASE = addr;
    FB_W = w;
    FB_H = h;
    FB_PITCH = pitch;
    FB_BPP = bpp;
    FB_BGR = if pixfmt == 1 { 1 } else { 0 }; // PIXEL_FORMAT_BGR == 1
    FB_VALID = 1;

    if arm == 0 {
        return 1; // quiet: geometry latched, not one pixel written
    }
    arm_screen();
    1
}

/// Take the screen over and paint the header. Split out of early_fb_init() so
/// that the no-root-filesystem report can arm a QUIET boot at the moment the
/// boot has definitively failed: at that point there is nothing left to be
/// quiet for, and a black screen would be the brick this module exists to
/// prevent.
unsafe fn arm_screen() {
    if FB_VALID == 0 {
        return;
    }
    FB_READY = 1;
    LOG_TOP = 3;
    LOG_BOT = if FB_H / GH > 8 { FB_H / GH - 4 } else { 4 };
    LOG_ROW = LOG_TOP;
    LAST_STAGE = 0;

    let bg = rgb(0, 0, 0);
    let ink = rgb(220, 230, 245);
    let dim = rgb(140, 150, 170);
    fill(0, 0, FB_W, FB_H, bg);
    text(GW, 0, b"MAYTERAOS DIAGNOSTIC BOOT\0".as_ptr(), ink, bg, false, 1);

    // Line 2: the firmware framebuffer geometry, in full. If we ever render a
    // machine's panel wrongly, this line taken from a photo says why.
    let y = GH;
    let mut x = text(GW, y, b"FB \0".as_ptr(), dim, bg, false, 1);
    x = text_u32(x, y, FB_W, 0, dim, bg, false, 1);
    x = text(x, y, b"x\0".as_ptr(), dim, bg, false, 1);
    x = text_u32(x, y, FB_H, 0, dim, bg, false, 1);
    x = text(x, y, b" pitch \0".as_ptr(), dim, bg, false, 1);
    x = text_u32(x, y, FB_PITCH, 0, dim, bg, false, 1);
    x = text(x, y, b" bpp \0".as_ptr(), dim, bg, false, 1);
    x = text_u32(x, y, FB_BPP, 0, dim, bg, false, 1);
    x = text(
        x,
        y,
        if FB_BGR != 0 {
            b" BGR \0".as_ptr()
        } else {
            b" RGB \0".as_ptr()
        },
        dim,
        bg,
        false,
        1,
    );
    let mut hb = [0u8; 20];
    hex_u64(FB_BASE, &mut hb);
    text(x, y, hb.as_ptr(), dim, bg, false, 1);
}

/// Is the firmware geometry usable? 1 means the screen COULD be used as a
/// diagnostic channel; it says nothing about whether we are painting on it.
/// Distinct from early_fb_ready() on purpose: "quiet" and "blind" are different
/// facts about the machine and a log that confuses them is worse than silence.
#[no_mangle]
pub unsafe extern "C" fn early_fb_valid() -> i32 {
    if FB_VALID != 0 {
        1
    } else {
        0
    }
}

/// LAST RESORT: promote a quiet boot to an armed one. Called only from
/// boot_stage_report_forever(), i.e. only when no filesystem mounted and the
/// boot is over. Returns 1 if the screen is now ours.
#[no_mangle]
pub unsafe extern "C" fn early_fb_force_arm() -> i32 {
    if FB_VALID != 0 && FB_READY == 0 {
        arm_screen();
    }
    if FB_READY != 0 {
        1
    } else {
        0
    }
}

/// Are we PAINTING on the early framebuffer? 0 means either a quiet boot (the
/// default) or no usable geometry at all; early_fb_valid() tells the two apart.
#[no_mangle]
pub unsafe extern "C" fn early_fb_ready() -> i32 {
    if FB_READY != 0 {
        1
    } else {
        0
    }
}

/// Record and display one boot stage. `stage` is the caller's ordinal (see
/// fs/bootstage.h); `name` is a short NUL-terminated label.
#[no_mangle]
pub unsafe extern "C" fn early_fb_stage(stage: u32, name: *const u8) {
    if FB_READY == 0 {
        return;
    }
    LAST_STAGE = stage;

    let bg = rgb(0, 0, 0);
    let ink = rgb(180, 220, 180);

    // The scrolling area does not scroll: it wraps and clears the row it is
    // about to reuse. Scrolling means moving 8 MB of MMIO per line, which on a
    // write-combining-less early aperture is slow enough to look like the hang
    // we are trying to diagnose.
    if LOG_ROW >= LOG_BOT {
        LOG_ROW = LOG_TOP;
    }
    let y = LOG_ROW * GH;
    fill(0, y, FB_W, GH, bg);
    let mut x = text(GW, y, b"  \0".as_ptr(), ink, bg, false, 1);
    x = text_u32(x, y, stage, 2, ink, bg, false, 1);
    x = text(x, y, b"  \0".as_ptr(), ink, bg, false, 1);
    text(x, y, name, ink, bg, false, 1);
    LOG_ROW += 1;

    // Clear the row we will use NEXT, so the boundary between "this boot" and
    // "the wrapped-around tail of this boot" is unambiguous in a photograph.
    if LOG_ROW < LOG_BOT {
        fill(0, LOG_ROW * GH, FB_W, GH, bg);
    }

    paint_bignum(stage);
    paint_banner(stage, name);
}

/// Append a detail line to the scrolling area without advancing the stage.
#[no_mangle]
pub unsafe extern "C" fn early_fb_note(msg: *const u8) {
    if FB_READY == 0 {
        return;
    }
    if LOG_ROW >= LOG_BOT {
        LOG_ROW = LOG_TOP;
    }
    let bg = rgb(0, 0, 0);
    let dim = rgb(150, 160, 180);
    let y = LOG_ROW * GH;
    fill(0, y, FB_W, GH, bg);
    text(GW * 5, y, msg, dim, bg, false, 1);
    LOG_ROW += 1;
    if LOG_ROW < LOG_BOT {
        fill(0, LOG_ROW * GH, FB_W, GH, bg);
    }
}

/// Repaint ONLY the bottom banner, with the stage last passed to
/// early_fb_stage(). This is the call that matters once the kernel's own
/// framebuffer stack owns the screen: gfx_boot_log() renders into the back
/// buffer and fb_swap_buffers()es it over the whole front buffer, erasing the
/// banner, so bootstage.c calls this AFTERWARDS to put it back. The banner is
/// therefore always the most recent thing committed to the glass, which is the
/// whole point: it is what a frozen machine is still showing.
#[no_mangle]
pub unsafe extern "C" fn early_fb_banner_refresh(stage: u32, name: *const u8) {
    if FB_READY == 0 {
        return;
    }
    // TAKE THE ORDINAL FROM THE CALLER, DO NOT REUSE LAST_STAGE.
    //
    // MEASURED on an armed VM boot: the bottom banner read "STAGE 19
    // USB_ROOT_MOUNT" while the boot-log console beside it read "[26]
    // USB_ROOT_MOUNT", and the large ordinal top right sat on 19 for the whole
    // rest of the boot. LAST_STAGE is written only by early_fb_stage(), which
    // stops being called the moment console_init() runs (bootstage.c switches to
    // the gfx_boot_log branch and calls THIS function instead), so the ordinal
    // froze at the last pre-console stage while the NAME kept updating.
    //
    // That is the worst possible place for this instrument to be wrong. The
    // entire design is "photograph the hung screen and read the big number", and
    // for every stage after 19, which is most of the boot, the big number named
    // a different step than the banner under it. A disagreeing instrument is
    // worse than an absent one: it sends the reader to the wrong subsystem.
    LAST_STAGE = stage;
    paint_banner(stage, name);
    paint_bignum(stage);
}

/// Paint a full-screen page of pre-wrapped text, for the hardware report the
/// user photographs. `lines` is a NUL-terminated string; newlines break rows.
/// Returns the number of bytes consumed, so the caller can page a buffer larger
/// than one screen by calling again with the returned offset.
#[no_mangle]
pub unsafe extern "C" fn early_fb_page(
    body: *const u8,
    title: *const u8,
    page: u32,
    pages: u32,
) -> u32 {
    if FB_READY == 0 || body.is_null() {
        return 0;
    }
    let bg = rgb(0, 0, 0);
    let ink = rgb(215, 225, 240);
    let hdr = rgb(255, 210, 90);
    fill(0, 0, FB_W, FB_H, bg);

    let mut x = text(GW, 0, title, hdr, bg, false, 1);
    x = text(x, 0, b"   page \0".as_ptr(), hdr, bg, false, 1);
    x = text_u32(x, 0, page, 0, hdr, bg, false, 1);
    x = text(x, 0, b"/\0".as_ptr(), hdr, bg, false, 1);
    // The caller cannot know the page count until it has walked the whole
    // buffer once, and it walks it BY DRAWING it, so the first pass legitimately
    // does not know. Say "?" rather than "0": a reader looking at a photograph
    // of "page 3/0" has to work out whether that is a bug, and the answer to
    // "is this instrument broken" should never cost anyone a minute.
    if pages == 0 {
        text(x, 0, b"?\0".as_ptr(), hdr, bg, false, 1);
    } else {
        text_u32(x, 0, pages, 0, hdr, bg, false, 1);
    }
    fill(0, GH + 2, FB_W, 1, rgb(90, 100, 120));

    let cols = if FB_W / GW > 2 { FB_W / GW - 2 } else { 1 };
    let rows_avail = if FB_H / GH > 3 { FB_H / GH - 3 } else { 1 };

    let mut i = 0usize;
    let mut row = 0u32;
    let mut col = 0u32;
    let mut y = GH * 2;
    // Hard cap on the scan: a body without a NUL must not spin here.
    let limit = 256 * 1024usize;
    while i < limit && row < rows_avail {
        let c = ptr::read(body.add(i));
        if c == 0 {
            i += 1;
            break;
        }
        i += 1;
        if c == b'\n' {
            row += 1;
            col = 0;
            y += GH;
            continue;
        }
        if c == b'\r' {
            continue;
        }
        if col >= cols {
            // Wrap rather than clip: a truncated PCI line is a missing device.
            row += 1;
            col = 0;
            y += GH;
            if row >= rows_avail {
                break;
            }
        }
        let c = if (32..127).contains(&c) { c } else { b'.' };
        glyph(GW + col * GW, y, c, ink, bg, false, 1);
        col += 1;
    }
    i as u32
}
