// main.rs - gbemu, MayteraOS Game Boy emulator, Ring-3 userland front end.
//
// The actual SM83 CPU / PPU / MBC / timer core lives in gb.rs (shared with
// hosttest/main.rs, which runs it on a plain Linux host for fast, VM-free
// validation - see that file's header for how and why). This file is ONLY the
// MayteraOS glue: window creation, the per-window event queue (the same path
// DOOM and every other windowed app uses, and the path the capability API's
// input.inject can drive - see the task brief), frame pacing via win_get_event
// with a timeout (the same non-busy-wait pattern userland/apps/pong/main.c
// uses), the compositor blit, and battery-save persistence to a file.
//
// CLEAN-ROOM NOTICE: see gb.rs. This file and gb.rs owe no code or structure
// to any specific existing Game Boy emulator, in particular NOT to
// https://github.com/GaelCathelin/Game-Boy-DOS (all-rights-reserved, never
// read). Built from the public Game Boy hardware spec plus this project's own
// existing Rust-userland conventions (taskmanager/setup/rss/arena).

#![no_std]
#![allow(dead_code)]

extern crate alloc;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::panic::PanicInfo;

#[path = "gb.rs"]
mod gb;

// #191/#243: THE keycode table (libc/keys.rs) instead of a private redeclaration
// (the keycode-gate blocks any second copy of these constants; see blame.md).
#[path = "../../libc/keys.rs"]
mod keys;

// ROM navigator (owner feedback, 2026-09-06): the on-screen Settings button
// opens an in-LCD ROM browser. Its own tiny bitmap font (font.rs) and the
// browsing/preview logic (nav.rs) are separate modules so this glue file
// doesn't grow a second unrelated subsystem inline.
#[path = "font.rs"]
mod font;
#[path = "nav.rs"]
mod nav;

// ----------------------------------------------------------------------------
// libc-backed global allocator (same pattern as arena_rs.rs / rss_rs.rs /
// startmenu_model.rs: align<=16 goes straight to malloc, which this libc
// guarantees 16-byte-aligned; larger alignment over-allocates and stashes the
// real base pointer just before the aligned pointer it hands back).
// ----------------------------------------------------------------------------
extern "C" {
    fn malloc(size: usize) -> *mut u8;
    fn free(ptr: *mut u8);
    fn realloc(ptr: *mut u8, size: usize) -> *mut u8;
}

const WORD: usize = core::mem::size_of::<usize>();

struct LibcAllocator;

unsafe impl GlobalAlloc for LibcAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let align = layout.align();
        let size = layout.size();
        if align <= 16 {
            malloc(size)
        } else {
            let total = match size.checked_add(align).and_then(|v| v.checked_add(WORD)) {
                Some(t) => t,
                None => return core::ptr::null_mut(),
            };
            let raw = malloc(total);
            if raw.is_null() {
                return core::ptr::null_mut();
            }
            let raw_addr = raw as usize + WORD;
            let aligned = (raw_addr + align - 1) & !(align - 1);
            *((aligned - WORD) as *mut usize) = raw as usize;
            aligned as *mut u8
        }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        if layout.align() <= 16 {
            free(ptr);
        } else {
            let base = *((ptr as usize - WORD) as *const usize);
            free(base as *mut u8);
        }
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if layout.align() <= 16 {
            realloc(ptr, new_size)
        } else {
            let new_layout = match Layout::from_size_align(new_size, layout.align()) {
                Ok(l) => l,
                Err(_) => return core::ptr::null_mut(),
            };
            let new_ptr = self.alloc(new_layout);
            if !new_ptr.is_null() {
                let copy_size = layout.size().min(new_size);
                core::ptr::copy_nonoverlapping(ptr, new_ptr, copy_size);
                self.dealloc(ptr, layout);
            }
            new_ptr
        }
    }
}

#[global_allocator]
static ALLOCATOR: LibcAllocator = LibcAllocator;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    unsafe {
        syscall1(SYS_EXIT, 101);
    }
    loop {}
}

// ----------------------------------------------------------------------------
// Raw syscall FFI (syscall.asm real symbols). A no_std app cannot include the
// C headers, so every SYS_* number below is its own copy, read out of
// userland/libc/syscall.h at the time this file was written, per this
// project's standing convention for Rust apps (see setup/main.rs).
// ----------------------------------------------------------------------------
extern "C" {
    fn syscall0(n: i64) -> i64;
    fn syscall1(n: i64, a1: i64) -> i64;
    fn syscall2(n: i64, a1: i64, a2: i64) -> i64;
    fn syscall3(n: i64, a1: i64, a2: i64, a3: i64) -> i64;
    fn syscall4(n: i64, a1: i64, a2: i64, a3: i64, a4: i64) -> i64;
    fn syscall5(n: i64, a1: i64, a2: i64, a3: i64, a4: i64, a5: i64) -> i64;
    fn syscall6(n: i64, a1: i64, a2: i64, a3: i64, a4: i64, a5: i64, a6: i64) -> i64;
}

const SYS_EXIT: i64 = 0;
const SYS_OPEN: i64 = 10;
const SYS_CLOSE: i64 = 11;
const SYS_READ: i64 = 12;
const SYS_WRITE: i64 = 13;
const SYS_SEEK: i64 = 14;
const SYS_SLEEP: i64 = 7;
const SYS_WIN_CREATE: i64 = 30;
const SYS_WIN_DESTROY: i64 = 31;
const SYS_WIN_DRAW_RECT: i64 = 32;
const SYS_WIN_DRAW_TEXT: i64 = 33;
const SYS_WIN_BLIT: i64 = 35;
const SYS_WIN_GET_EVENT: i64 = 36;
const SYS_WIN_INVALIDATE: i64 = 37;
const SYS_AUDIO_PCM_OPEN: i64 = 315;
const SYS_AUDIO_PCM_WRITE: i64 = 316;
const SYS_AUDIO_PCM_CLOSE: i64 = 317;
const AUDIO_FORMAT_S16_LE: i64 = 0x0002;
const EVENT_MOUSE_DOWN: u32 = 2;
const EVENT_MOUSE_UP: u32 = 3;
const SYS_WIN_GET_SIZE: i64 = 38;
const SYS_DECODE_IMAGE: i64 = 253;
const SYS_WIN_DRAW_IMAGE: i64 = 254;

// Device-skin geometry (owner's gameboy.png background, 2026-09-06). The skin is a
// full Game Boy body with an LCD cutout; the emulator screen is composited into it.
const SKIN_W: i32 = 738;
const SKIN_H: i32 = 827;
const LCD_X: i32 = 26;
const LCD_Y: i32 = 5;
const LCD_W: i32 = 676; // 702 - 26
const LCD_H: i32 = 454; // 459 - 5
const SYS_READDIR: i64 = 19;

const O_RDONLY: i64 = 0x0000;
const O_WRONLY: i64 = 0x0001;
const O_CREAT: i64 = 0x0040;
const O_TRUNC: i64 = 0x0200;
const SEEK_SET: i64 = 0;
const SEEK_END: i64 = 2;

// pub: nav.rs (the ROM navigator) reuses these instead of redeclaring its own
// open/close/readdir wrappers.
pub fn sys_open(path: &[u8], flags: i64) -> i32 {
    unsafe { syscall2(SYS_OPEN, path.as_ptr() as i64, flags) as i32 }
}
pub fn sys_close(fd: i32) {
    unsafe {
        syscall1(SYS_CLOSE, fd as i64);
    }
}
fn sys_read(fd: i32, buf: &mut [u8]) -> i64 {
    unsafe { syscall3(SYS_READ, fd as i64, buf.as_mut_ptr() as i64, buf.len() as i64) }
}
fn sys_write(fd: i32, buf: &[u8]) -> i64 {
    unsafe { syscall3(SYS_WRITE, fd as i64, buf.as_ptr() as i64, buf.len() as i64) }
}
fn sys_seek(fd: i32, off: i64, whence: i64) -> i64 {
    unsafe { syscall3(SYS_SEEK, fd as i64, off, whence) }
}
fn sys_sleep(ms: i64) {
    unsafe {
        syscall1(SYS_SLEEP, ms);
    }
}

fn win_create(title: &[u8], x: i32, y: i32, w: i32, h: i32) -> i32 {
    unsafe {
        syscall5(
            SYS_WIN_CREATE,
            title.as_ptr() as i64,
            x as i64,
            y as i64,
            w as i64,
            h as i64,
        ) as i32
    }
}
fn win_destroy(h: i32) {
    unsafe {
        syscall1(SYS_WIN_DESTROY, h as i64);
    }
}
fn win_draw_rect(h: i32, x: i32, y: i32, w: i32, ht: i32, color: u32) {
    unsafe {
        syscall6(SYS_WIN_DRAW_RECT, h as i64, x as i64, y as i64, w as i64, ht as i64, color as i64);
    }
}
fn win_draw_text(h: i32, x: i32, y: i32, text: &[u8], color: u32) {
    unsafe {
        syscall5(SYS_WIN_DRAW_TEXT, h as i64, x as i64, y as i64, text.as_ptr() as i64, color as i64);
    }
}
fn win_invalidate(h: i32) {
    unsafe {
        syscall1(SYS_WIN_INVALIDATE, h as i64);
    }
}
fn win_blit_gb_screen(h: i32, fb: &[u32]) {
    let packed: i64 = (gb::SCREEN_W as i64 & 0xFFFF) | ((gb::SCREEN_H as i64 & 0xFFFF) << 16);
    unsafe {
        syscall5(SYS_WIN_BLIT, h as i64, 0, 0, packed, fb.as_ptr() as i64);
    }
}

// Decode a PNG/BMP/JPEG to a tw x th box, BGRA (framebuffer order) into `out`.
// Returns bytes written (>0) or <=0 on failure; dims[0/1] receive the result w/h.
// The kernel (sys_decode_image, proc/syscall.c) letterboxes: it writes exactly
// dims[0]*dims[1]*4 bytes packed tight at the START of `out` (row stride is
// dims[0], NOT tw), preserving the source aspect ratio; the rest of `out` is
// left untouched. pub: nav.rs (ROM preview quantisation) reuses this same
// wrapper rather than re-deriving the syscall packing.
pub fn decode_image(data: &[u8], tw: i32, th: i32, out: &mut [u8], dims: &mut [i32; 2]) -> i32 {
    let target: i64 = (((tw as i64) & 0xFFFF) << 16) | ((th as i64) & 0xFFFF);
    unsafe {
        syscall6(
            SYS_DECODE_IMAGE,
            data.as_ptr() as i64,
            data.len() as i64,
            target,
            out.as_mut_ptr() as i64,
            out.len() as i64,
            dims.as_mut_ptr() as i64,
        ) as i32
    }
}

// Blit a w*ht BGRA buffer into the window content at (x,y) (clipped by the compositor).
fn win_blit_native(h: i32, buf: &[u32]) {
    // SYS_WIN_BLIT nearest-scales a src_w x src_h buffer to the window's content
    // rectangle, so composing at the skin's native size lets the window be ANY size
    // (and freely resizable) with the compositor doing the scale for us.
    let packed: i64 = (SKIN_W as i64 & 0xFFFF) | ((SKIN_H as i64 & 0xFFFF) << 16);
    unsafe {
        syscall5(SYS_WIN_BLIT, h as i64, 0, 0, packed, buf.as_ptr() as i64);
    }
}

// Load and decode the device skin at its native SKIN_W x SKIN_H, KEEPING its alpha.
// The LCD cutout is transparent (alpha 0) with anti-aliased rounded corners; the
// screen is drawn UNDERNEATH and the skin composited on top, so those corners clip
// the screen edges. None => no skin file, and the app falls back to the plain window.
fn load_skin() -> Option<Vec<u32>> {
    let png = read_whole_file(b"/ROMS/GAMEBOY.PNG\0")
        .or_else(|| read_whole_file(b"/APPS/GAMEBOY.PNG\0"))?;
    let mut skin: Vec<u32> = alloc::vec![0u32; (SKIN_W * SKIN_H) as usize];
    let mut dims = [0i32; 2];
    let out =
        unsafe { core::slice::from_raw_parts_mut(skin.as_mut_ptr() as *mut u8, skin.len() * 4) };
    let n = decode_image(&png, SKIN_W, SKIN_H, out, &mut dims);
    if n <= 0 || dims[0] != SKIN_W || dims[1] != SKIN_H {
        return None;
    }
    Some(skin)
}

// Multiply an 0xRRGGBB colour's brightness by k/255 (k in 0..=255).
fn scale_rgb(c: u32, k: u32) -> u32 {
    let r = (((c >> 16) & 0xFF) * k / 255) & 0xFF;
    let g = (((c >> 8) & 0xFF) * k / 255) & 0xFF;
    let b = ((c & 0xFF) * k / 255) & 0xFF;
    (r << 16) | (g << 8) | b
}

// Alpha-blend fg over bg (both 0xRRGGBB, a in 0..=255).
fn blend_rgb(fg: u32, bg: u32, a: u32) -> u32 {
    let ia = 255 - a;
    let r = ((((fg >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * ia) / 255) & 0xFF;
    let g = ((((fg >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * ia) / 255) & 0xFF;
    let b = (((fg & 0xFF) * a + (bg & 0xFF) * ia) / 255) & 0xFF;
    (r << 16) | (g << 8) | b
}

// Compose one native-size frame. SYS_DECODE_IMAGE flattens the transparent LCD
// cutout to opaque WHITE, so the near-white pixels INSIDE the cutout rect are exactly
// the screen area, and the art's own grey body just outside that white (including its
// rounded corners) clips the screen for free. The screen is drawn there, ramped by how
// white the pixel is so the anti-aliased rounded-corner pixels blend body->screen, with
// a soft inner shadow at the cutout edge for a recessed-LCD emboss.
fn compose_frame(work: &mut [u32], skin: &[u32], fb: &[u32]) {
    let sw = SKIN_W as usize;
    let shadow: i32 = 8; // inner-shadow (emboss) width along the cutout edge
    for pidx in 0..(SKIN_W * SKIN_H) as usize {
        let sk = skin[pidx] & 0x00FF_FFFF;
        let x = (pidx % sw) as i32;
        let y = (pidx / sw) as i32;
        let r = (sk >> 16) & 0xFF;
        let g = (sk >> 8) & 0xFF;
        let b = sk & 0xFF;
        let minch = if r < g {
            if r < b { r } else { b }
        } else if g < b {
            g
        } else {
            b
        };
        let in_rect = x >= LCD_X && x < LCD_X + LCD_W && y >= LCD_Y && y < LCD_Y + LCD_H;
        if !in_rect || minch < 200 {
            work[pidx] = 0xFF00_0000 | sk;
            continue;
        }
        // screen pixel: cutout rect stretched to the 160x144 frame
        let mut gx = (x - LCD_X) * 160 / LCD_W;
        let mut gy = (y - LCD_Y) * 144 / LCD_H;
        if gx < 0 { gx = 0; }
        if gx > 159 { gx = 159; }
        if gy < 0 { gy = 0; }
        if gy > 143 { gy = 143; }
        let mut px = fb[(gy as usize) * 160 + gx as usize] & 0x00FF_FFFF;
        // recessed-LCD emboss: darken toward the cutout edge
        let dl = x - LCD_X;
        let dr = (LCD_X + LCD_W) - 1 - x;
        let dt = y - LCD_Y;
        let db = (LCD_Y + LCD_H) - 1 - y;
        let mut d = dl;
        if dr < d { d = dr; }
        if dt < d { d = dt; }
        if db < d { d = db; }
        if d >= 0 && d < shadow {
            let k = 90 + (165 * d / shadow); // 90..255 brightness ramp
            px = scale_rgb(px, k as u32);
        }
        // ramp screen over the near-white art by whiteness, so AA rounded corners are smooth
        let alpha = if minch >= 245 { 255 } else { (minch - 200) * 255 / 45 };
        work[pidx] = 0xFF00_0000 | blend_rgb(px, sk, alpha as u32);
    }
}

// event_type_t values (libc/gui.h), read out of the header, not guessed - see
// the standing note in taskmanager/main.rs about this exact mistake class.
const EVENT_KEY_DOWN: u32 = 5;
const EVENT_KEY_UP: u32 = 6;
const EVENT_WINDOW_CLOSE: u32 = 7;
const EVENT_REDRAW: u32 = 11;

// Mirror of gui_event_t (libc/gui.h): a #[repr(C)] struct with the SAME field
// order/types as the C struct reproduces its padding for free (see
// taskmanager/main.rs, which carries the identical mirror and the same
// caution about hand-computed offsets being exactly what people get wrong).
#[repr(C)]
#[derive(Clone, Copy)]
struct GuiEvent {
    ty: u32,
    target_id: u32,
    mouse_x: i32,
    mouse_y: i32,
    mouse_buttons: u32,
    scroll_delta: i8,
    keycode: u32,
    key_char: u8,
}
fn win_get_event(h: i32, ev: &mut GuiEvent, timeout_ms: i32) -> i32 {
    unsafe { syscall3(SYS_WIN_GET_EVENT, h as i64, ev as *mut GuiEvent as i64, timeout_ms as i64) as i32 }
}

// Mirror of dirent_t (libc/syscall.h): name[256], type (0=file,1=dir), size.
// pub (struct and fields): nav.rs's full-listing scan reuses this SAME mirror
// rather than declaring a second copy (see taskmanager/main.rs's standing
// note about hand-computed struct mirrors being exactly where these bugs
// happen - one mirror, reused, not two that can drift).
#[repr(C)]
pub struct Dirent {
    pub name: [u8; 256],
    pub ty: u32,
    pub size: u32,
}

// pub: the single SYS_READDIR call site both scan_roms_dir (below) and
// nav::scan_roms (the navigator's full listing) go through.
pub fn sys_readdir(fd: i32, entry: &mut Dirent) -> i64 {
    unsafe { syscall2(SYS_READDIR, fd as i64, entry as *mut Dirent as i64) }
}

pub fn ends_with_ci(s: &[u8], suffix: &[u8]) -> bool {
    if s.len() < suffix.len() {
        return false;
    }
    s[s.len() - suffix.len()..].eq_ignore_ascii_case(suffix)
}

// The Start-menu native-app launcher (compositor/startmenu.c LAUNCH_NATIVE)
// calls plain sys_spawn(path) with no argv, unlike the DOS/Win16 launch types
// which split a trailing command tail (see launchline.rs) - so a Start-menu
// item cannot hand this app a ROM path today. Rather than extend the
// compositor's native launch path (out of scope for this app), we fall back
// to a directory convention: the first *.gb/*.gbc file under /ROMS. A direct
// terminal launch with a real argv[1] (e.g. `gbemu /ROMS/POKEMON.GB`) still
// takes priority when present.
fn scan_roms_dir() -> Option<Vec<u8>> {
    let fd = sys_open(b"/ROMS\0", 0);
    if fd < 0 {
        return None;
    }
    let mut entry = Dirent { name: [0; 256], ty: 0, size: 0 };
    let mut found: Option<Vec<u8>> = None;
    loop {
        let r = sys_readdir(fd, &mut entry);
        if r != 0 {
            break;
        }
        let name_len = entry.name.iter().position(|&b| b == 0).unwrap_or(256);
        let name = &entry.name[..name_len];
        if entry.ty == 0 && (ends_with_ci(name, b".gb") || ends_with_ci(name, b".gbc")) {
            let mut path = Vec::with_capacity(6 + name.len() + 1);
            path.extend_from_slice(b"/ROMS/");
            path.extend_from_slice(name);
            path.push(0);
            found = Some(path);
            break;
        }
    }
    sys_close(fd);
    found
}

// pub: nav.rs reads /ROMS/<name>.jpg preview candidates through this same
// whole-file reader.
pub fn read_whole_file(path: &[u8]) -> Option<Vec<u8>> {
    let fd = sys_open(path, O_RDONLY);
    if fd < 0 {
        return None;
    }
    let size = sys_seek(fd, 0, SEEK_END);
    sys_seek(fd, 0, SEEK_SET);
    if size <= 0 {
        sys_close(fd);
        return None;
    }
    let mut buf = alloc::vec![0u8; size as usize];
    let mut off = 0usize;
    while off < buf.len() {
        let n = sys_read(fd, &mut buf[off..]);
        if n <= 0 {
            break;
        }
        off += n as usize;
    }
    sys_close(fd);
    if off == 0 {
        None
    } else {
        buf.truncate(off);
        Some(buf)
    }
}

fn write_whole_file(path: &[u8], data: &[u8]) {
    let fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if fd < 0 {
        return;
    }
    sys_write(fd, data);
    sys_close(fd);
}

// Strips a trailing ".gb"/".gbc" (case-insensitive) if present. pub: the one
// place this idiom lives - sav_path_for (below) and nav.rs's display-name and
// preview-basename derivation all go through this instead of each re-writing
// the same four-line extension check.
pub fn strip_gb_ext(name: &[u8]) -> &[u8] {
    if name.len() > 4 && name[name.len() - 4..].eq_ignore_ascii_case(b".gbc") {
        &name[..name.len() - 4]
    } else if name.len() > 3 && name[name.len() - 3..].eq_ignore_ascii_case(b".gb") {
        &name[..name.len() - 3]
    } else {
        name
    }
}

// Builds "<path-without-.gb-or-.gbc-extension>.sav\0" for the battery save.
fn sav_path_for(rom_path: &[u8]) -> Vec<u8> {
    let mut end = rom_path.len();
    // strip a trailing NUL if the caller passed one in already
    while end > 0 && rom_path[end - 1] == 0 {
        end -= 1;
    }
    let base = strip_gb_ext(&rom_path[..end]);
    let mut out = Vec::with_capacity(base.len() + 5);
    out.extend_from_slice(base);
    out.extend_from_slice(b".sav\0");
    out
}

// Re-inits the emulator with a ROM picked from the navigator (A button). One
// call site shared by the on-screen A button and the physical 'x' key, so
// the "read ROM, build Gb, load its .sav if any, and swap every piece of
// per-ROM state together" sequence exists exactly once (the same reasoning
// as sys_readdir above: two copies of a sequence like this is how they drift).
// A short/unreadable ROM is silently ignored (stays on the previous game)
// rather than crashing the emulator mid-session.
fn load_selected_rom(
    path: &[u8],
    g: &mut gb::Gb,
    rom_reset: &mut Vec<u8>,
    rom_path: &mut Vec<u8>,
    sav_path: &mut Vec<u8>,
    held_btn: &mut Option<gb::Button>,
    frame_counter: &mut u32,
) {
    let newrom = match read_whole_file(path) {
        Some(r) if r.len() >= 0x150 => r,
        _ => return,
    };
    *rom_reset = newrom.clone();
    let mut newg = gb::Gb::new(newrom);
    let new_sav = sav_path_for(path);
    if newg.has_battery() {
        if let Some(sav) = read_whole_file(&new_sav) {
            newg.load_ram(&sav);
        }
    }
    *g = newg;
    *rom_path = path.to_vec();
    *sav_path = new_sav;
    *held_btn = None;
    *frame_counter = 0;
}

fn cstr(s: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(s.len() + 1);
    v.extend_from_slice(s);
    v.push(0);
    v
}

fn win_get_size(h: i32, w: &mut i32, ht: &mut i32) -> i32 {
    unsafe { syscall3(SYS_WIN_GET_SIZE, h as i64, w as *mut i32 as i64, ht as *mut i32 as i64) as i32 }
}

// On-screen control hit test. The centres are the owner's exact button centres in
// the skin's native 738x827 space (2026-09-06); a window click is mapped back into
// that space before testing. Closest centre within its radius wins, which resolves
// the tightly-packed D-pad correctly.
enum Hit {
    Btn(gb::Button),
    Reset,
    Settings,
    None,
}
fn hit_button(sx: i32, sy: i32) -> Hit {
    let btns: [(i32, i32, i32, u8); 10] = [
        (114, 521, 46, 0),
        (114, 627, 46, 1),
        (60, 574, 46, 2),
        (164, 572, 46, 3),
        (639, 533, 58, 4),
        (555, 583, 58, 5),
        (429, 698, 62, 6),
        (318, 699, 62, 7),
        (110, 758, 60, 8),
        (621, 759, 60, 9),
    ];
    let mut best: i32 = -1;
    let mut bestd: i32 = i32::MAX;
    for &(cx, cy, r, id) in btns.iter() {
        let dx = sx - cx;
        let dy = sy - cy;
        let d = dx * dx + dy * dy;
        if d <= r * r && d < bestd {
            bestd = d;
            best = id as i32;
        }
    }
    match best {
        0 => Hit::Btn(gb::Button::Up),
        1 => Hit::Btn(gb::Button::Down),
        2 => Hit::Btn(gb::Button::Left),
        3 => Hit::Btn(gb::Button::Right),
        4 => Hit::Btn(gb::Button::A),
        5 => Hit::Btn(gb::Button::B),
        6 => Hit::Btn(gb::Button::Start),
        7 => Hit::Btn(gb::Button::Select),
        8 => Hit::Reset,
        9 => Hit::Settings,
        _ => Hit::None,
    }
}

fn apply_key(g: &mut gb::Gb, keycode: u32, key_char: u8, pressed: bool) {
    use gb::Button::*;
    match keycode {
        keys::GUI_KEY_UP | keys::GUI_KEY_UP_REL => g.joyp.set(Up, pressed),
        keys::GUI_KEY_DOWN | keys::GUI_KEY_DOWN_REL => g.joyp.set(Down, pressed),
        keys::GUI_KEY_LEFT | keys::GUI_KEY_LEFT_REL => g.joyp.set(Left, pressed),
        keys::GUI_KEY_RIGHT | keys::GUI_KEY_RIGHT_REL => g.joyp.set(Right, pressed),
        keys::GUI_KEY_ENTER => g.joyp.set(Start, pressed),
        keys::GUI_KEY_RSHIFT => g.joyp.set(Select, pressed),
        _ => match key_char {
            b'z' | b'Z' => g.joyp.set(B, pressed),
            b'x' | b'X' => g.joyp.set(A, pressed),
            b'a' | b'A' => g.joyp.set(Start, pressed), // convenience alias
            b's' | b'S' => g.joyp.set(Select, pressed), // convenience alias
            _ => {}
        },
    }
}

fn show_message(title: &[u8], lines: &[&[u8]]) -> ! {
    let win = win_create(title, 200, 150, 460, 200);
    win_draw_rect(win, 0, 0, 460, 200, 0x00202020);
    let mut y = 20;
    for line in lines {
        win_draw_text(win, 16, y, line, 0x00FFFFFF);
        y += 18;
    }
    win_invalidate(win);
    let mut ev = GuiEvent {
        ty: 0,
        target_id: 0,
        mouse_x: 0,
        mouse_y: 0,
        mouse_buttons: 0,
        scroll_delta: 0,
        keycode: 0,
        key_char: 0,
    };
    loop {
        let r = win_get_event(win, &mut ev, -1);
        if r > 0 && ev.ty == EVENT_WINDOW_CLOSE {
            break;
        }
    }
    win_destroy(win);
    unsafe {
        syscall1(SYS_EXIT, 0);
    }
    loop {}
}

#[no_mangle]
pub extern "C" fn main(argc: i64, argv: *const *const u8) -> i32 {
    // Resolve the ROM path. Priority:
    //  1. argv[1], for a direct terminal launch (`gbemu /ROMS/POKEMON.GB`) -
    //     a plain ELF spawned with a real argv gets this normally.
    //  2. the first *.gb/*.gbc file under /ROMS, for a Start-menu launch:
    //     compositor/startmenu.c's LAUNCH_NATIVE path calls plain
    //     sys_spawn(path) with no argv (unlike the DOS/Win16 launch types,
    //     which split a trailing command tail - see launchline.rs), so a
    //     Start-menu item cannot hand this app a path today.
    // We ship NO ROM ourselves either way.
    let mut rom_path: Vec<u8> = Vec::new();
    if argc >= 2 && !argv.is_null() {
        unsafe {
            let p = *argv.offset(1);
            if !p.is_null() {
                let mut len = 0usize;
                while *p.add(len) != 0 {
                    len += 1;
                }
                rom_path.extend_from_slice(core::slice::from_raw_parts(p, len + 1));
            }
        }
    }
    if rom_path.is_empty() {
        if let Some(found) = scan_roms_dir() {
            rom_path = found;
        }
    }
    if rom_path.is_empty() {
        rom_path.extend_from_slice(b"/ROMS/GAME.GB\0");
    }

    let rom = match read_whole_file(&rom_path) {
        Some(r) if r.len() >= 0x150 => r,
        Some(_) => show_message(
            b"Game Boy\0",
            &[
                b"ROM file is too small to be a valid",
                b"Game Boy cartridge image.",
            ],
        ),
        None => show_message(
            b"Game Boy\0",
            &[
                b"No ROM found.",
                b"Launch gbemu with a .gb ROM path argument,",
                b"or place one at /ROMS/GAME.GB",
                b"(no game ROM ships with MayteraOS).",
            ],
        ),
    };

    let mut rom_reset = rom.clone();
    let mut g = gb::Gb::new(rom);
    let mut sav_path = sav_path_for(&rom_path);
    if g.has_battery() {
        if let Some(sav) = read_whole_file(&sav_path) {
            g.load_ram(&sav);
        }
    }

    let title = {
        let t = g.title();
        let mut v = Vec::with_capacity(17);
        for &b in t.iter() {
            if b == 0 {
                break;
            }
            v.push(b);
        }
        if v.is_empty() {
            v.extend_from_slice(b"Game Boy");
        }
        v.push(0);
        v
    };

    let skin = load_skin();
    let mut work: Vec<u32> = if skin.is_some() {
        alloc::vec![0u32; (SKIN_W * SKIN_H) as usize]
    } else {
        Vec::new()
    };
    let win = if skin.is_some() {
        // Default to <=50% of a 1280x800 screen (outer 400px tall) at the skin's
        // 738:827 aspect; SYS_WIN_BLIT rescales the native compose to any window size,
        // so the window is freely resizable from here.
        win_create(&title, 100, 60, 344, 400)
    } else {
        win_create(&title, 120, 80, 496, 464)
    };

    let mut ev = GuiEvent {
        ty: 0,
        target_id: 0,
        mouse_x: 0,
        mouse_y: 0,
        mouse_buttons: 0,
        scroll_delta: 0,
        keycode: 0,
        key_char: 0,
    };
    let mut running = true;
    let mut held_btn: Option<gb::Button> = None;
    let mut frame_counter: u32 = 0;
    // OS PCM sink for Game Boy audio (44.1 kHz stereo s16). No audio device -> <1,
    // and the emulator simply runs silent (not a failure).
    let pcm: i32 = unsafe { syscall3(SYS_AUDIO_PCM_OPEN, 44100, 2, AUDIO_FORMAT_S16_LE) as i32 };

    // ROM navigator (Settings button): drawn INTO the same 160x144 Game Boy
    // framebuffer the emulator itself uses, so it goes through the exact same
    // compose_frame/win_blit_native (skin) or win_blit_gb_screen (no skin)
    // path below - the navigator is not a separate window or overlay, it is
    // a different producer of the one LCD-sized buffer. `nav_mode` gates
    // whether g.run_frame() runs each tick (paused while browsing) and which
    // buffer gets rendered/blitted.
    let mut nav_mode = false;
    let mut nav: Option<nav::Navigator> = None;
    let mut nav_fb: Vec<u32> = alloc::vec![0u32; (gb::SCREEN_W * gb::SCREEN_H) as usize];
    while running {
        // Pace to ~60 Hz via the blocking wait-with-timeout on the window
        // event queue (the same non-busy-wait pattern userland/apps/pong/
        // main.c uses), not a spin loop. The real GB frame rate (59.7275 Hz,
        // ~16.74 ms) is close enough to this project's existing 16 ms game-
        // loop convention that we follow it rather than invent a new one;
        // noted as a small, deliberate timing approximation.
        let r = win_get_event(win, &mut ev, 16);
        if r > 0 {
            match ev.ty {
                EVENT_WINDOW_CLOSE => running = false,
                EVENT_KEY_DOWN => {
                    if nav_mode {
                        if let Some(n) = nav.as_mut() {
                            let kc = ev.keycode;
                            if kc == keys::GUI_KEY_UP || kc == keys::GUI_KEY_LEFT {
                                n.move_sel(-1);
                            } else if kc == keys::GUI_KEY_DOWN || kc == keys::GUI_KEY_RIGHT {
                                n.move_sel(1);
                            } else {
                                match ev.key_char {
                                    b'x' | b'X' => {
                                        // A: load the highlighted ROM and return to the game.
                                        if let Some(path) = n.current_path() {
                                            load_selected_rom(
                                                &path,
                                                &mut g,
                                                &mut rom_reset,
                                                &mut rom_path,
                                                &mut sav_path,
                                                &mut held_btn,
                                                &mut frame_counter,
                                            );
                                        }
                                        nav_mode = false;
                                        nav = None;
                                    }
                                    b'z' | b'Z' => {
                                        // B: back to the game, selection discarded.
                                        nav_mode = false;
                                        nav = None;
                                    }
                                    _ => {}
                                }
                            }
                        }
                    } else {
                        apply_key(&mut g, ev.keycode, ev.key_char, true);
                    }
                }
                EVENT_KEY_UP => {
                    if !nav_mode {
                        apply_key(&mut g, ev.keycode, ev.key_char, false);
                    }
                }
                EVENT_REDRAW => {}
                EVENT_MOUSE_DOWN => {
                    if skin.is_some() {
                        let mut cw: i32 = 0;
                        let mut ch: i32 = 0;
                        win_get_size(win, &mut cw, &mut ch);
                        if cw > 0 && ch > 0 {
                            let sx = ev.mouse_x * SKIN_W / cw;
                            let sy = ev.mouse_y * SKIN_H / ch;
                            let hit = hit_button(sx, sy);
                            if nav_mode {
                                if let Some(n) = nav.as_mut() {
                                    match hit {
                                        Hit::Btn(gb::Button::Up) | Hit::Btn(gb::Button::Left) => n.move_sel(-1),
                                        Hit::Btn(gb::Button::Down) | Hit::Btn(gb::Button::Right) => n.move_sel(1),
                                        Hit::Btn(gb::Button::A) => {
                                            if let Some(path) = n.current_path() {
                                                load_selected_rom(
                                                    &path,
                                                    &mut g,
                                                    &mut rom_reset,
                                                    &mut rom_path,
                                                    &mut sav_path,
                                                    &mut held_btn,
                                                    &mut frame_counter,
                                                );
                                            }
                                            nav_mode = false;
                                            nav = None;
                                        }
                                        Hit::Btn(gb::Button::B) => {
                                            nav_mode = false;
                                            nav = None;
                                        }
                                        _ => {}
                                    }
                                }
                            } else {
                                match hit {
                                    Hit::Btn(b) => {
                                        g.joyp.set(b, true);
                                        held_btn = Some(b);
                                    }
                                    Hit::Reset => {
                                        let ram = if g.has_battery() { Some(g.save_ram().to_vec()) } else { None };
                                        g = gb::Gb::new(rom_reset.clone());
                                        if let Some(rr) = ram {
                                            g.load_ram(&rr);
                                        }
                                        held_btn = None;
                                    }
                                    Hit::Settings => {
                                        // Owner feedback fix: this used to be a no-op. Opens
                                        // (or re-scans, if already open) the ROM navigator.
                                        nav_mode = true;
                                        nav = Some(nav::Navigator::open());
                                    }
                                    Hit::None => {}
                                }
                            }
                        }
                    }
                }
                EVENT_MOUSE_UP => {
                    if let Some(b) = held_btn.take() {
                        g.joyp.set(b, false);
                    }
                }
                _ => {}
            }
        }
        if !running {
            break;
        }

        if nav_mode {
            // Browsing: the game is paused (no run_frame, no audio push, no
            // autosave tick) and the navigator draws into nav_fb instead of
            // reading g.ppu.fb - everything downstream (compose over the
            // skin, or the raw blit with no skin) is identical to the normal
            // gameplay path, just fed a different 160x144 source buffer.
            if let Some(ref n) = nav {
                nav::draw(&mut nav_fb, n);
            }
            if let Some(ref sk) = skin {
                compose_frame(&mut work, sk, &nav_fb);
                win_blit_native(win, &work);
            } else {
                win_blit_gb_screen(win, &nav_fb);
            }
            win_invalidate(win);
            continue;
        }

        g.run_frame();
        // push this frame's Game Boy audio to the PCM sink (drain the APU buffer)
        if pcm >= 1 && !g.apu.out.is_empty() {
            let total = (g.apu.out.len() / 2) as u32;
            let base = g.apu.out.as_ptr();
            let mut done: u32 = 0;
            while done < total {
                let n = unsafe {
                    let ptr = base.add((done as usize) * 2);
                    syscall3(SYS_AUDIO_PCM_WRITE, pcm as i64, ptr as i64, (total - done) as i64)
                };
                if n <= 0 {
                    break;
                }
                done += n as u32;
            }
        }
        g.apu.out.clear();
        if let Some(ref sk) = skin {
            compose_frame(&mut work, sk, &g.ppu.fb);
            win_blit_native(win, &work);
        } else {
            win_blit_gb_screen(win, &g.ppu.fb);
        }
        win_invalidate(win);

        frame_counter = frame_counter.wrapping_add(1);
        if g.has_battery() && frame_counter % 300 == 0 {
            write_whole_file(&sav_path, g.save_ram());
        }
    }

    if g.has_battery() {
        write_whole_file(&sav_path, g.save_ram());
    }
    if pcm >= 1 {
        unsafe {
            syscall1(SYS_AUDIO_PCM_CLOSE, pcm as i64);
        }
    }
    win_destroy(win);
    let _ = cstr; // silence unused-helper warning if not needed on this path
    0
}
