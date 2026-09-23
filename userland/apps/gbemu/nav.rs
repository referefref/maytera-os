// nav.rs - gbemu's in-LCD ROM navigator (owner feedback, 2026-09-06). The
// on-screen Settings button used to do nothing; it now opens this browser,
// which renders directly into the SAME 160x144 Game Boy framebuffer the
// emulator itself uses (main.rs feeds it through the identical
// compose_frame/win_blit_native or win_blit_gb_screen path as g.ppu.fb - see
// the nav_mode branch in main()), so it is a mode of the LCD, not a separate
// window or a full-window overlay.
//
// Everything here reuses main.rs's existing primitives (sys_open/sys_close/
// sys_readdir, the Dirent mirror, read_whole_file, decode_image) via
// `super::`, and gb.rs's own 4-shade DMG ramp (gb::SHADE) for the preview
// quantisation, rather than declaring second copies of any of them.

use alloc::vec::Vec;
use super::{gb, font, Dirent};

// Preview box size (the spec's own "~128x96" suggestion, trimmed slightly to
// leave room for the header/name/hint rows in the 160x144 screen - see draw()).
pub const PREV_W: i32 = 128;
pub const PREV_H: i32 = 88;

pub struct RomEntry {
    pub path: Vec<u8>,    // full path incl. trailing NUL, e.g. b"/ROMS/TETRIS.GB\0"
    pub display: Vec<u8>, // basename, extension stripped, uppercased, no NUL
}

pub struct Navigator {
    pub roms: Vec<RomEntry>,
    pub sel: usize,
    // Cached quantised preview for `sel`; None means "no matching image was
    // found for this ROM", not "not loaded yet" - draw() shows the pixel-font
    // placeholder in that case, per the owner's spec.
    pub preview: Option<Vec<u32>>,
}

impl Navigator {
    // Lists /ROMS fresh every time the navigator opens, so a ROM (or preview
    // image) the owner just copied in shows up without relaunching gbemu.
    pub fn open() -> Navigator {
        let roms = scan_roms();
        let mut nav = Navigator { roms, sel: 0, preview: None };
        nav.reload_preview();
        nav
    }

    pub fn reload_preview(&mut self) {
        self.preview = None;
        if let Some(entry) = self.roms.get(self.sel) {
            if let Some(data) = find_preview(&entry.path) {
                if let Some((raw, dw, dh)) = decode_preview_pixels(&data) {
                    self.preview = Some(quantize_into_box(&raw, dw, dh));
                }
            }
        }
    }

    // Wraps around both ends, so Left/Right (or Up/Down) at the list edges
    // is not a dead input.
    pub fn move_sel(&mut self, delta: i32) {
        let n = self.roms.len() as i32;
        if n <= 0 {
            return;
        }
        let mut s = (self.sel as i32) + delta;
        s = ((s % n) + n) % n;
        self.sel = s as usize;
        self.reload_preview();
    }

    pub fn current_path(&self) -> Option<Vec<u8>> {
        self.roms.get(self.sel).map(|e| e.path.clone())
    }
}

// ----------------------------------------------------------------------------
// Directory scan
// ----------------------------------------------------------------------------

fn display_name(name: &[u8]) -> Vec<u8> {
    let mut n = super::strip_gb_ext(name).to_vec();
    for b in n.iter_mut() {
        *b = b.to_ascii_uppercase();
    }
    n
}

// Every *.gb/*.gbc under /ROMS, sorted by display name. main.rs's own
// scan_roms_dir() (the Start-menu "first ROM" fallback) deliberately stays a
// separate, smaller function - it wants the first match in directory order,
// not a full sorted listing - but both now go through the same sys_readdir
// wrapper, so there is exactly one SYS_READDIR call site.
pub fn scan_roms() -> Vec<RomEntry> {
    let mut out: Vec<RomEntry> = Vec::new();
    let fd = super::sys_open(b"/ROMS\0", 0);
    if fd < 0 {
        return out;
    }
    let mut entry = Dirent { name: [0; 256], ty: 0, size: 0 };
    loop {
        let r = super::sys_readdir(fd, &mut entry);
        if r != 0 {
            break;
        }
        let name_len = entry.name.iter().position(|&b| b == 0).unwrap_or(256);
        let name = &entry.name[..name_len];
        if entry.ty == 0 && (super::ends_with_ci(name, b".gb") || super::ends_with_ci(name, b".gbc")) {
            let mut path = Vec::with_capacity(6 + name.len() + 1);
            path.extend_from_slice(b"/ROMS/");
            path.extend_from_slice(name);
            path.push(0);
            let display = display_name(name);
            out.push(RomEntry { path, display });
        }
    }
    super::sys_close(fd);
    out.sort_by(|a, b| a.display.cmp(&b.display));
    out
}

// ----------------------------------------------------------------------------
// Preview: find the matching image, decode it, quantise to the GB palette
// ----------------------------------------------------------------------------

// Splits "/ROMS/TETRIS.GB\0" into (b"/ROMS/", b"TETRIS") - directory prefix
// (including the trailing slash) and extension-stripped basename.
fn split_dir_base(rom_path: &[u8]) -> (Vec<u8>, Vec<u8>) {
    let mut end = rom_path.len();
    while end > 0 && rom_path[end - 1] == 0 {
        end -= 1;
    }
    let slice = &rom_path[..end];
    let slash = slice.iter().rposition(|&b| b == b'/').map(|i| i + 1).unwrap_or(0);
    let dir = slice[..slash].to_vec();
    let name = super::strip_gb_ext(&slice[slash..]).to_vec();
    (dir, name)
}

// Matches a ROM to its preview by basename (owner's own convention:
// TETRIS.GB + TETRIS.JPG side by side in /ROMS). Tries the common image
// extensions in both cases, since FAT/ext2 filenames on a real device may be
// typed either way.
fn find_preview(rom_path: &[u8]) -> Option<Vec<u8>> {
    let (dir, base) = split_dir_base(rom_path);
    const EXTS: [&[u8]; 8] =
        [b".JPG", b".JPEG", b".PNG", b".BMP", b".jpg", b".jpeg", b".png", b".bmp"];
    for ext in EXTS.iter() {
        let mut path = Vec::with_capacity(dir.len() + base.len() + ext.len() + 1);
        path.extend_from_slice(&dir);
        path.extend_from_slice(&base);
        path.extend_from_slice(ext);
        path.push(0);
        if let Some(data) = super::read_whole_file(&path) {
            return Some(data);
        }
    }
    None
}

// Decodes into a PREV_W x PREV_H box (same pattern as main.rs's load_skin:
// alloc a u32 Vec, view it as a u8 slice for the syscall, then read it back
// as u32 pixels). Returns the raw pixel buffer plus the ACTUAL decoded
// width/height, which the kernel packs tight at the buffer's start and which
// can be smaller than the box in one dimension (aspect-preserving fit -
// proc/syscall.c's sys_decode_image scales the source down to fit inside
// tw x th, it does not stretch to fill it).
fn decode_preview_pixels(data: &[u8]) -> Option<(Vec<u32>, i32, i32)> {
    let mut raw: Vec<u32> = alloc::vec![0u32; (PREV_W * PREV_H) as usize];
    let mut dims = [0i32; 2];
    let out = unsafe { core::slice::from_raw_parts_mut(raw.as_mut_ptr() as *mut u8, raw.len() * 4) };
    let n = super::decode_image(data, PREV_W, PREV_H, out, &mut dims);
    if n <= 0 || dims[0] <= 0 || dims[1] <= 0 {
        return None;
    }
    Some((raw, dims[0], dims[1]))
}

fn luma_shade_index(p: u32) -> usize {
    let r = (p >> 16) & 0xFF;
    let g = (p >> 8) & 0xFF;
    let b = p & 0xFF;
    // standard luma weights; GB screens have no colour, only brightness.
    let luma = (r * 299 + g * 587 + b * 114) / 1000;
    let bucket = (luma / 64).min(3); // 0..=3, 0=darkest source, 3=brightest
    (3 - bucket) as usize // SHADE[] is index 0 = lightest, 3 = darkest
}

// Centres the dw x dh decoded image inside a PREV_W x PREV_H buffer, mapping
// every pixel to one of the four gb::SHADE colours by luma. Letterbox area
// (when the source aspect isn't PREV_W:PREV_H) is filled with the lightest
// shade, matching the navigator's own page background.
fn quantize_into_box(raw: &[u32], dw: i32, dh: i32) -> Vec<u32> {
    let mut out = alloc::vec![gb::SHADE[0]; (PREV_W * PREV_H) as usize];
    let ox = (PREV_W - dw) / 2;
    let oy = (PREV_H - dh) / 2;
    for y in 0..dh {
        for x in 0..dw {
            let p = raw[(y * dw + x) as usize];
            let dx = ox + x;
            let dy = oy + y;
            if dx >= 0 && dx < PREV_W && dy >= 0 && dy < PREV_H {
                out[(dy * PREV_W + dx) as usize] = gb::SHADE[luma_shade_index(p)];
            }
        }
    }
    out
}

// ----------------------------------------------------------------------------
// Rendering: draws straight into the 160x144 Game Boy screen buffer
// ----------------------------------------------------------------------------

fn set_px(buf: &mut [u32], x: i32, y: i32, color: u32) {
    if x < 0 || x >= 160 || y < 0 || y >= 144 {
        return;
    }
    buf[(y * 160 + x) as usize] = color;
}

fn draw_rect_outline(buf: &mut [u32], x: i32, y: i32, w: i32, h: i32, color: u32) {
    for i in 0..w {
        set_px(buf, x + i, y, color);
        set_px(buf, x + i, y + h - 1, color);
    }
    for j in 0..h {
        set_px(buf, x, y + j, color);
        set_px(buf, x + w - 1, y + j, color);
    }
}

fn itoa(mut v: usize, out: &mut Vec<u8>) {
    if v == 0 {
        out.push(b'0');
        return;
    }
    let start = out.len();
    while v > 0 {
        out.push(b'0' + (v % 10) as u8);
        v /= 10;
    }
    out[start..].reverse();
}

fn format_index(n: usize, total: usize) -> Vec<u8> {
    let mut s = Vec::new();
    itoa(n, &mut s);
    s.push(b'/');
    itoa(total, &mut s);
    s
}

// Truncates with a trailing ".." if `name` would not fit `maxlen` glyph cells.
fn clip_text(name: &[u8], maxlen: usize) -> Vec<u8> {
    if name.len() <= maxlen {
        return name.to_vec();
    }
    let keep = if maxlen > 2 { maxlen - 2 } else { 0 };
    let mut v = name[..keep].to_vec();
    v.extend_from_slice(b"..");
    v
}

// Word-wraps `text` (by character count, since the font is fixed-width) into
// as many lines as fit `box_w`, then centres the whole block in box_w x box_h.
// Used for the "no matching preview image" placeholder: the ROM's own name,
// in the pixel font, standing in for the missing artwork.
fn draw_wrapped_centered(buf: &mut [u32], bx: i32, by: i32, box_w: i32, box_h: i32, text: &[u8], color: u32, scale: i32) {
    let cell_w = (font::GLYPH_W + 1) * scale;
    let max_chars = ((box_w / cell_w).max(1)) as usize;
    let mut lines: Vec<&[u8]> = Vec::new();
    let mut i = 0usize;
    while i < text.len() {
        let end = (i + max_chars).min(text.len());
        lines.push(&text[i..end]);
        i = end;
    }
    if lines.is_empty() {
        lines.push(&text[0..0]);
    }
    let line_h = (font::GLYPH_H + 2) * scale;
    let total_h = line_h * (lines.len() as i32);
    let mut y = by + (box_h - total_h) / 2;
    for line in lines.iter() {
        let w = font::text_width(line, scale);
        let x = bx + (box_w - w) / 2;
        font::draw_text(buf, 160, 144, x, y, line, color, scale);
        y += line_h;
    }
}

// Renders the whole navigator screen into `buf` (must be a 160x144 buffer,
// row-major, same layout as gb::Ppu's fb). Called once per loop tick while
// nav_mode is active (main.rs); cheap enough (23,040 pixels) to redraw every
// tick rather than track a separate "needs redraw" flag.
pub fn draw(buf: &mut [u32], nav: &Navigator) {
    let bg = gb::SHADE[0];
    let fg = gb::SHADE[3];
    for p in buf.iter_mut() {
        *p = bg;
    }

    if nav.roms.is_empty() {
        let msg = b"NO ROMS FOUND";
        let w = font::text_width(msg, 2);
        font::draw_text(buf, 160, 144, (160 - w) / 2, 56, msg, fg, 2);
        let hint = b"PLACE .GB FILES IN /ROMS";
        let hw = font::text_width(hint, 1);
        font::draw_text(buf, 160, 144, (160 - hw) / 2, 84, hint, fg, 1);
        let hint2 = b"<B> BACK";
        let hw2 = font::text_width(hint2, 1);
        font::draw_text(buf, 160, 144, (160 - hw2) / 2, 110, hint2, fg, 1);
        return;
    }

    // Header row: title left-centred, "n/N" index top-right.
    let header = b"ROM SELECT";
    let hw = font::text_width(header, 1);
    font::draw_text(buf, 160, 144, (160 - hw) / 2, 2, header, fg, 1);
    let idx_text = format_index(nav.sel + 1, nav.roms.len());
    let iw = font::text_width(&idx_text, 1);
    font::draw_text(buf, 160, 144, 160 - iw - 3, 2, &idx_text, fg, 1);

    // Preview box, centred, with a 1px selection frame around it.
    let bx = (160 - PREV_W) / 2;
    let by = 12;
    draw_rect_outline(buf, bx - 2, by - 2, PREV_W + 4, PREV_H + 4, fg);
    if let Some(ref pv) = nav.preview {
        for y in 0..PREV_H {
            for x in 0..PREV_W {
                buf[((by + y) * 160 + (bx + x)) as usize] = pv[(y * PREV_W + x) as usize];
            }
        }
    } else {
        // Tasteful placeholder: a coarse checkerboard tile plus the ROM's own
        // name in the pixel font, standing in for a missing preview image.
        for y in 0..PREV_H {
            for x in 0..PREV_W {
                let tile = ((x / 8) + (y / 8)) & 1;
                let c = if tile == 0 { gb::SHADE[0] } else { gb::SHADE[1] };
                buf[((by + y) * 160 + (bx + x)) as usize] = c;
            }
        }
        let name = &nav.roms[nav.sel].display;
        draw_wrapped_centered(buf, bx, by, PREV_W, PREV_H, name, fg, 2);
    }

    // Name row (always shown, even with a real preview image).
    let name = clip_text(&nav.roms[nav.sel].display, 26);
    let nw = font::text_width(&name, 1);
    font::draw_text(buf, 160, 144, (160 - nw) / 2, by + PREV_H + 6, &name, fg, 1);

    // Hint rows.
    let h1 = b"<A> LOAD   <B> BACK";
    let h1w = font::text_width(h1, 1);
    font::draw_text(buf, 160, 144, (160 - h1w) / 2, 122, h1, fg, 1);
    let h2 = b"UP/DOWN: SELECT";
    let h2w = font::text_width(h2, 1);
    font::draw_text(buf, 160, 144, (160 - h2w) / 2, 132, h2, fg, 1);
}
