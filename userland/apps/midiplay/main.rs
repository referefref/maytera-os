// midiplay - #183: a Standard MIDI File player for MayteraOS, in Ring-3 Rust,
// synthesising through the shared OPL2 FM core.
//
// ===========================================================================
// THIS IS THE THIRD CONSUMER OF ONE FM CORE, NOT A THIRD FM CORE
// ---------------------------------------------------------------------------
// #182's decision is ONE synthesiser, in userland, with more than one
// consumer, and userland/lib/opl2/core-gate.sh FAILS THE BUILD if a second
// implementation appears. /APPS/FMSYNTH (the DOS bridge) and /APPS/FMTEST (the
// verification harness) reach it as
//
//     #[path = "../../lib/opl2/opl2.rs"] mod opl2;
//     chip.init(rate); chip.write_reg(reg, val); chip.render_stereo(&mut buf);
//
// and so does this app, character for character. There is no log-sine table, no
// operator offset map and no MULT table anywhere in this directory or in
// userland/lib/midi. The ONE thing the core did not already expose that this
// player needed, the operator offset for a channel, was ADDED TO THE CORE as
// `op_offset()` rather than copied, which is what "improve the shared
// primitive" means when the alternative is a second copy that drifts.
//
// ===========================================================================
// THERE IS NO CLOCK IN THE PLAYBACK PATH
// ---------------------------------------------------------------------------
// `timer_ticks` is NOT a wall clock on this system: KVM replays lost ticks in
// BURSTS, so a `timer_ticks + N` deadline can fire instantly under vCPU
// starvation. A MIDI player scheduled that way plays at the wrong speed under
// load, and it looks exactly like a synthesis bug.
//
// So MIDI events are scheduled in SAMPLE POSITIONS and the only thing that
// paces playback is the audio sink accepting a buffer. If the sink has taken
// 44100 frames, one second of music has been produced, whatever the scheduler
// was doing. SYS_MONO_US (TSC-backed) appears exactly once in this file, to
// measure the CPU cost of a render block, and never to decide when a note
// starts.
//
// ===========================================================================
// #426: WHERE THIS APP BLOCKS, AND THAT IT NEVER SPINS
// ---------------------------------------------------------------------------
// Two places, both real wait queues, never a poll loop:
//
//   PLAYING  sys_audio_pcm_write parks on the PCM ring's wait queue. Each
//            call returns after roughly one block of audio has drained, which
//            is what paces the loop. Between blocks the GUI queue is drained
//            with timeout 0, which kernel/proc/syscall.c:6875 documents as a
//            genuine non-blocking return, not a spin.
//   IDLE     sys_win_get_event with a finite timeout, which #453 made a real
//            wait_event rather than the proc_yield() spin it used to be.
//
// There is no `while (!done) {}` anywhere in this file.

#![no_std]

use core::panic::PanicInfo;

// #191: THE keycode table. See libc/keys.rs. This app previously declared its
// own KC_UP/KC_DOWN and got both wrong (see below).
#[path = "../../libc/keys.rs"]
mod keys;
#[path = "../../lib/opl2/opl2.rs"]
mod opl2;
#[path = "../../lib/midi/midi.rs"]
mod midi;

use midi::*;
use opl2::NUM_CHANNELS;

#[panic_handler]
fn panic(_i: &PanicInfo) -> ! {
    // panic = abort in Ring 3: exit loudly rather than spin. A spin here would
    // burn a core forever (#426).
    unsafe {
        syscall1(SYS_EXIT, 103);
    }
    loop {}
}

extern "C" {
    fn syscall1(n: i64, a1: i64) -> i64;
    fn syscall2(n: i64, a1: i64, a2: i64) -> i64;
    fn syscall3(n: i64, a1: i64, a2: i64, a3: i64) -> i64;
    fn syscall5(n: i64, a1: i64, a2: i64, a3: i64, a4: i64, a5: i64) -> i64;
    fn syscall6(n: i64, a1: i64, a2: i64, a3: i64, a4: i64, a5: i64, a6: i64) -> i64;

    // The SHARED style engine (userland/libc/gui_style.h), the same object file
    // /APPS/TASKMGR and /APPS/SETTINGS call. Not a Rust reimplementation of
    // bevels: a second widget look is exactly the fragmentation the style guide
    // exists to prevent.
    fn gui_set_style(style: i32);
    fn gui_set_palette(p: *const GuiPalette);
    fn gui_card(handle: i32, x: i32, y: i32, w: i32, h: i32);
    fn gui_button(handle: i32, x: i32, y: i32, w: i32, h: i32, label: *const u8,
                  variant: i32, st: i32);
    fn gui_progress(handle: i32, x: i32, y: i32, w: i32, h: i32, pct: i32);
    fn gui_lighten(c: u32, amt: i32) -> u32;
}

// Syscall numbers. THIS IS THE FIFTH-COPY HAZARD (kernel/tools/
// syscall-number-lint rule 5): a no_std Rust app cannot include the C header,
// so it holds its own copy, and a stale copy compiles fine and calls the wrong
// syscall. The first-boot wizard shipped exactly that bug and #182 caught
// SYS_EXIT = 1 in a new Rust app when it is 0. Rule 5 reads these constants, so
// they cannot drift silently. Every one below was read out of
// userland/libc/syscall.h, not remembered.
const SYS_EXIT: i64 = 0;
const SYS_OPEN: i64 = 10;
const SYS_CLOSE: i64 = 11;
const SYS_READ: i64 = 12;
const SYS_WRITE: i64 = 13;
const SYS_READDIR: i64 = 19;
const SYS_WIN_CREATE: i64 = 30;
const SYS_WIN_DESTROY: i64 = 31;
const SYS_WIN_DRAW_RECT: i64 = 32;
const SYS_WIN_GET_EVENT: i64 = 36;
const SYS_WIN_INVALIDATE: i64 = 37;
const SYS_WIN_GET_SIZE: i64 = 38;
const SYS_GET_THEME: i64 = 134;
const SYS_WIN_DRAW_TTF: i64 = 235;
const SYS_THEME_COLOR: i64 = 290;
// #217: the two refusals this app must tell apart. Mirrors audio_pcm.h.
// ENODEV = this machine cannot make a sound at all. EBUSY = it can, but the
// single PCM stream (PCM_MAX_STREAMS == 1) is held by somebody else RIGHT NOW.
// Rendering both as "No audio device" is what made #205 cost a full
// investigation, and it is the same conflation #217 is about.
const PCM_EBUSY: i64 = -2;
const PCM_ENODEV: i64 = -6;
const SYS_AUDIO_PCM_OPEN: i64 = 315;
const SYS_AUDIO_PCM_WRITE: i64 = 316;
const SYS_AUDIO_PCM_CLOSE: i64 = 317;
const SYS_MONO_US: i64 = 388;

const O_RDONLY: i64 = 0x0000;
const O_WRONLY: i64 = 0x0001;
const O_CREAT: i64 = 0x0040;
const O_TRUNC: i64 = 0x0200;
const AUDIO_FORMAT_S16_LE: i64 = 0x0002;

const EVENT_MOUSE_DOWN: u32 = 2;
const EVENT_MOUSE_SCROLL: u32 = 4;
const EVENT_KEY_DOWN: u32 = 5;
const EVENT_WINDOW_CLOSE: u32 = 7;
const EVENT_REDRAW: u32 = 11;
const EVENT_RESIZE: u32 = 12;

// Arrow keys carry NO ASCII character and must be matched on the keycode.
//
// #191: this app used to declare its own pair here, 0x48/0x50, under the
// comment "Taken from the same values taskmanager uses" - and taskmanager's
// values were the raw PS/2 SCANCODES, wrong since the day they were written
// (#188). So the file list in this player had never once responded to an
// arrow key; 0x48/0x50 are ASCII 'H'/'P', so shift-H and shift-P moved the
// selection instead. The copy is what spread the defect, so there is no copy
// any more: keys::GUI_KEY_UP / keys::GUI_KEY_DOWN come from libc/keys.rs,
// which build/keycode-gate.sh checks against libc/keys.h.

const RATE: u32 = 44100;
/// 2048 frames is 46 ms. Long enough that the per-block overhead is amortised,
/// short enough that a keypress is answered within one block.
const BLOCK_FRAMES: usize = 2048;

#[repr(C)]
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

#[repr(C)]
struct GuiPalette {
    surface: u32,
    surface_raised: u32,
    ink: u32,
    ink_dim: u32,
    accent: u32,
    accent_hover: u32,
    border: u32,
    field_bg: u32,
    field_border: u32,
    track: u32,
}

const GUI_STYLE_CLASSIC: i32 = 0;
const GUI_STYLE_MODERN: i32 = 1;
const GUI_BTN_PRIMARY: i32 = 0;
const GUI_BTN_SECONDARY: i32 = 1;
const GUI_ST_NORMAL: i32 = 0;
const GUI_ST_DISABLED: i32 = 4;
const THEME_COLOR_ACCENT: i64 = 2;
const THEME_COLOR_WINDOW_BG: i64 = 8;

/// The kernel's directory entry, whose layout is documented at
/// userland/libc/dirent.c:14 as "must match the typedef in
/// kernel/proc/syscall.c sys_readdir". Read with SYS_OPEN + SYS_READDIR, the
/// same convention libc's opendir/readdir use; going through those would pull
/// malloc in for a listing this app already has a fixed buffer for.
#[repr(C)]
struct KDirent {
    name: [u8; 256],
    ty: u32,
    size: u32,
}

// ---------------------------------------------------------------------------
// .bss, NOT the stack. The Ring-3 stack is 16 KB. A 512 KB file buffer on it
// does not fail at the declaration: it corrupts whatever is below and the app
// dies somewhere unrelated, which is the exact fault that bit the earlier Rust
// userland port.
// ---------------------------------------------------------------------------
const FILE_CAP: usize = 512 * 1024;
static mut FILEBUF: [u8; FILE_CAP] = [0; FILE_CAP];
static mut DEMOBUF: [u8; 4096] = [0; 4096];
static mut PCMBUF: [i16; BLOCK_FRAMES * 2] = [0; BLOCK_FRAMES * 2];
static mut SYNTH: FmSynth = FmSynth::new();
static mut PLAYER: Player<'static> = Player::new();
static mut SELFTEST_BUF: [u8; 4096] = [0; 4096];
static mut SELFTEST_SCRATCH: Scratch = Scratch::new();
const REPORT_CAP: usize = 16384;
static mut REPORT: [u8; REPORT_CAP] = [0; REPORT_CAP];
static mut REPORT_N: usize = 0;

const MAX_FILES: usize = 64;
static mut FILES: [[u8; 64]; MAX_FILES] = [[0; 64]; MAX_FILES];
static mut NFILES: usize = 0;

// ---------------------------------------------------------------------------
// Thin wrappers.
// ---------------------------------------------------------------------------
fn win_create(title: &[u8], x: i32, y: i32, w: i32, h: i32) -> i32 {
    unsafe { syscall5(SYS_WIN_CREATE, title.as_ptr() as i64, x as i64, y as i64, w as i64, h as i64) as i32 }
}
fn win_rect(h: i32, x: i32, y: i32, w: i32, ht: i32, c: u32) {
    unsafe { syscall6(SYS_WIN_DRAW_RECT, h as i64, x as i64, y as i64, w as i64, ht as i64, c as i64); }
}
fn win_invalidate(h: i32) {
    unsafe { syscall1(SYS_WIN_INVALIDATE, h as i64); }
}
fn win_get_size(h: i32, w: &mut i32, ht: &mut i32) {
    unsafe { syscall3(SYS_WIN_GET_SIZE, h as i64, w as *mut i32 as i64, ht as *mut i32 as i64); }
}
/// timeout_ms = 0 is a genuine non-blocking return (kernel/proc/syscall.c:6875,
/// "No event, non-blocking"), not a spin. Any other value parks on the window's
/// wait queue (#453).
fn win_event(h: i32, ev: &mut GuiEvent, timeout_ms: i32) -> i32 {
    unsafe { syscall3(SYS_WIN_GET_EVENT, h as i64, ev as *mut GuiEvent as i64, timeout_ms as i64) as i32 }
}
fn draw_text(h: i32, x: i32, y: i32, s: &[u8], size: i32, color: u32) {
    let packed = ((color & 0x00FF_FFFF) | (((size as u32) & 0xFF) << 24)) as i64;
    unsafe { syscall5(SYS_WIN_DRAW_TTF, h as i64, x as i64, y as i64, s.as_ptr() as i64, packed); }
}
fn theme_color(id: i64) -> u32 {
    unsafe { syscall2(SYS_THEME_COLOR, -1i64, id) as u32 }
}
fn theme_active() -> i32 {
    unsafe { syscall1(SYS_GET_THEME, 0) as i32 }
}
fn mono_us() -> u64 {
    unsafe { syscall1(SYS_MONO_US, 0) as u64 }
}

fn lum(c: u32) -> u32 {
    let r = (c >> 16) & 0xFF;
    let g = (c >> 8) & 0xFF;
    let b = c & 0xFF;
    (r * 30 + g * 59 + b * 11) / 100
}
fn ink_on(bg: u32) -> u32 { if lum(bg) > 140 { 0x0018_1818 } else { 0x00F0_F0F0 } }
fn mix(a: u32, b: u32, pct: u32) -> u32 {
    let f = |sh: u32| {
        let x = (a >> sh) & 0xFF;
        let y = (b >> sh) & 0xFF;
        ((x * (100 - pct) + y * pct) / 100) & 0xFF
    };
    (f(16) << 16) | (f(8) << 8) | f(0)
}

// ---------------------------------------------------------------------------
// Formatting. No float: this target is soft-float, so a float here is a
// libcall, not an instruction.
// ---------------------------------------------------------------------------
struct Buf {
    b: [u8; 160],
    n: usize,
}
impl Buf {
    const fn new() -> Buf { Buf { b: [0; 160], n: 0 } }
    fn clear(&mut self) { self.n = 0; self.b[0] = 0; }
    fn ch(&mut self, c: u8) {
        if self.n < self.b.len() - 1 {
            self.b[self.n] = c;
            self.n += 1;
            self.b[self.n] = 0;
        }
    }
    fn s(&mut self, t: &[u8]) {
        for c in t {
            if *c == 0 { break; }
            self.ch(*c);
        }
    }
    fn i(&mut self, mut v: i64) {
        if v < 0 { self.ch(b'-'); v = -v; }
        let mut d = [0u8; 24];
        let mut n = 0;
        if v == 0 { self.ch(b'0'); return; }
        while v > 0 { d[n] = b'0' + (v % 10) as u8; v /= 10; n += 1; }
        while n > 0 { n -= 1; self.ch(d[n]); }
    }
    /// M:SS from a frame count.
    fn time(&mut self, frames: u64, rate: u32) {
        let secs = frames / rate as u64;
        self.i((secs / 60) as i64);
        self.ch(b':');
        let s = secs % 60;
        self.ch(b'0' + (s / 10) as u8);
        self.ch(b'0' + (s % 10) as u8);
    }
    fn as_bytes(&self) -> &[u8] { &self.b[..self.n + 1] }
}

/// Note name for the voice monitor. C-1 is MIDI 0, matching the convention the
/// note table's own generator uses (A4 = MIDI 69 = 440 Hz).
fn note_name(n: u8, out: &mut Buf) {
    const NAMES: [&[u8]; 12] = [b"C", b"C#", b"D", b"D#", b"E", b"F",
                                b"F#", b"G", b"G#", b"A", b"A#", b"B"];
    out.s(NAMES[(n % 12) as usize]);
    out.i(n as i64 / 12 - 1);
    out.s(b" (");
    out.i(n as i64);
    out.ch(b')');
}

// ---------------------------------------------------------------------------
// The report file. A GUI app's putchar goes to its stdout, which for a
// compositor-spawned process is not the serial console (#182 lost an entire
// 19-check report that way and was left with a one-bit exit code). Anything
// this app wants a harness to read goes to a FILE.
// ---------------------------------------------------------------------------
fn rep(s: &[u8]) {
    unsafe {
        for c in s {
            if *c == 0 { break; }
            let n = REPORT_N;
            if n < REPORT_CAP - 1 {
                (*core::ptr::addr_of_mut!(REPORT))[n] = *c;
                REPORT_N = n + 1;
            }
        }
    }
}
fn rep_buf(b: &Buf) { rep(&b.b[..b.n]); }
fn write_report(path: &[u8]) -> i64 {
    unsafe {
        let n = REPORT_N;
        if n == 0 { return 0; }
        let fd = syscall3(SYS_OPEN, path.as_ptr() as i64, O_WRONLY | O_CREAT | O_TRUNC, 0o644);
        if fd < 0 { return fd; }
        let w = syscall3(SYS_WRITE, fd, core::ptr::addr_of!(REPORT) as i64, n as i64);
        syscall1(SYS_CLOSE, fd);
        w
    }
}

// ---------------------------------------------------------------------------
// App state.
// ---------------------------------------------------------------------------
#[derive(PartialEq, Clone, Copy)]
enum State { Idle, Playing, Paused }

struct App {
    win: i32,
    dw: i32,
    dh: i32,
    pal: GuiPalette,
    dark: bool,
    state: State,
    /// #217: the PCM handle, or -1 when we are NOT holding the stream.
    ///
    /// THERE IS EXACTLY ONE RING-3 PCM STREAM ON THIS MACHINE
    /// (kernel/drivers/audio_pcm.c, PCM_MAX_STREAMS == 1) and the hardware
    /// behind it is one HDA output engine with no mixer
    /// (audio.c sets supports_mixing = false, and audio_open() reconfigures the
    /// SAME engine for each new client). So this handle is an EXCLUSIVE,
    /// machine-wide lock, and this app used to take it at startup and hold it
    /// until it exited, even while stopped. It is now acquired on play and
    /// released on stop; see pcm_acquire()/pcm_release().
    pcm: i64,
    /// #217: false only once an open has returned ENODEV, i.e. we have PROOF
    /// this machine has no sink. Not the same as "we do not currently hold the
    /// stream": with lazy acquisition `pcm` is -1 for most of the app's life,
    /// and gating the Play button on `pcm >= 1` would disable playback for ever.
    audio_ok: bool,
    /// #217: 200 ms idle-loop timeouts spent in Paused. A pause is a promise to
    /// come back; a pause the user walked away from is indistinguishable from a
    /// stop, and it must not hold the whole machine's audio hostage.
    paused_ticks: u32,
    /// Length of the currently loaded file inside FILEBUF, or 0 when the demo
    /// in DEMOBUF is loaded.
    file_len: usize,
    demo_len: usize,
    using_demo: bool,
    total_frames: u64,
    sel: usize,
    scroll: usize,
    name: Buf,
    status: Buf,
    err: i32,
    underruns: u64,
    /// Microseconds of CPU spent rendering, and frames rendered, since load.
    /// The ratio is the honest answer to "how heavy is this?" and #187 is
    /// measuring the same cost for the DOS bridge.
    render_us: u64,
    render_frames: u64,
    /// Blocks of SILENCE still to push after the song ends.
    ///
    /// MEASURED, not theorised (#183's WAV capture, 2026-08-20): once the guest
    /// stops writing, the HDA stream keeps running over the same BDL and the
    /// emulated codec receives the last 131072-byte buffer AGAIN, every 0.743
    /// seconds, indefinitely. The host-side capture shows the final two notes of
    /// the demo looping for over sixty seconds after playback ended. That is a
    /// driver-side behaviour and it is reported as such; what this app can do
    /// about it is refuse to leave stale audio in the ring, which is also just
    /// correct: an app is responsible for what it feeds the sink. One ring's
    /// worth of zeros is enough and is bounded.
    flush_left: u32,
}

/// 32768 frames is the BDL buffer the HDA driver allocates (131072 bytes), so
/// this many blocks of silence overwrite all of it exactly once.
const FLUSH_BLOCKS: u32 = (32768 / BLOCK_FRAMES as u32) + 1;

fn set_status(a: &mut App, s: &[u8]) {
    a.status.clear();
    a.status.s(s);
}

impl App {
    fn apply_theme(&mut self) {
        let tid = theme_active();
        unsafe { gui_set_style(if tid == 4 { GUI_STYLE_CLASSIC } else { GUI_STYLE_MODERN }); }
        let wb = theme_color(THEME_COLOR_WINDOW_BG);
        let accent = theme_color(THEME_COLOR_ACCENT);
        self.dark = lum(wb) < 128;
        let surface = mix(if self.dark { 0x0026_2A30 } else { 0x00F5_F6F8 }, accent, 5);
        let raised = mix(if self.dark { 0x002C_313B } else { 0x00ED_EFF3 }, accent, 6);
        let ink = ink_on(surface);
        self.pal = GuiPalette {
            surface,
            surface_raised: raised,
            ink,
            ink_dim: mix(ink, surface, 45),
            accent,
            accent_hover: unsafe { gui_lighten(accent, 24) },
            border: if self.dark { 0x003A_424F } else { 0x00CD_D3DB },
            field_bg: if self.dark { 0x0033_3A45 } else { 0x00FF_FFFF },
            field_border: if self.dark { 0x003A_424F } else { 0x00CD_D3DB },
            track: mix(surface, accent, 20),
        };
        unsafe { gui_set_palette(&self.pal); }
    }
}

// ---------------------------------------------------------------------------
// Loading.
// ---------------------------------------------------------------------------
fn load_bytes(a: &mut App, from_demo: bool) {
    // #217: loading a different file ends the current performance, and every
    // arm below leaves us in State::Idle. An idle player must not be holding
    // the machine's only PCM stream, so give it back here rather than in three
    // separate arms.
    pcm_release(a);
    // SAFETY: single-threaded app. PLAYER borrows FILEBUF/DEMOBUF for 'static;
    // both are statics that outlive it, and nothing writes to whichever buffer
    // the player currently holds until the next load, which reopens it.
    unsafe {
        let d: &'static [u8] = if from_demo {
            core::slice::from_raw_parts(core::ptr::addr_of!(DEMOBUF) as *const u8, a.demo_len)
        } else {
            core::slice::from_raw_parts(core::ptr::addr_of!(FILEBUF) as *const u8, a.file_len)
        };
        let p = &mut *core::ptr::addr_of_mut!(PLAYER);
        let s = &mut *core::ptr::addr_of_mut!(SYNTH);
        a.using_demo = from_demo;
        match p.open(d, RATE) {
            Ok(()) => {
                let m = p.measure();
                a.total_frames = m.total_frames;
                s.init(RATE, m.percussion);
                a.err = E_OK;
                a.state = State::Idle;
                a.render_us = 0;
                a.render_frames = 0;
                a.underruns = 0;
                if m.track_errors > 0 {
                    a.status.clear();
                    a.status.s(b"Parse error in ");
                    a.status.i(m.track_errors as i64);
                    a.status.s(b" track(s): ");
                    a.status.s(err_name(m.last_error).as_bytes());
                    a.status.s(b" (the rest still plays)");
                } else if p.hdr.format == 2 && p.format2_ignored > 0 {
                    a.status.clear();
                    a.status.s(b"Format 2 is independent sequences: playing track 1 of ");
                    a.status.i((p.format2_ignored + 1) as i64);
                } else if p.hdr.tracks_dropped > 0 {
                    a.status.clear();
                    a.status.s(b"File has more than 64 tracks; ");
                    a.status.i(p.hdr.tracks_dropped as i64);
                    a.status.s(b" not played");
                } else {
                    set_status(a, b"Ready");
                }
            }
            Err(e) => {
                a.err = e;
                a.total_frames = 0;
                a.state = State::Idle;
                a.status.clear();
                a.status.s(b"Cannot play: ");
                a.status.s(err_name(e).as_bytes());
            }
        }
    }
}

fn load_demo(a: &mut App, tone: bool) {
    unsafe {
        let db = &mut *core::ptr::addr_of_mut!(DEMOBUF);
        a.demo_len = build_demo(db, tone);
    }
    a.name.clear();
    a.name.s(if tone { b"[built-in A440 tone]" } else { b"[built-in demo]" });
    load_bytes(a, true);
}

/// Read a .MID off the filesystem into FILEBUF.
fn load_file(a: &mut App, idx: usize) {
    unsafe {
        if idx >= NFILES { return; }
        let mut path = Buf::new();
        path.s(&(*core::ptr::addr_of!(FILES))[idx]);
        let fd = syscall2(SYS_OPEN, path.b.as_ptr() as i64, O_RDONLY);
        if fd < 0 {
            a.status.clear();
            a.status.s(b"Cannot open file (");
            a.status.i(fd);
            a.ch_close();
            return;
        }
        let mut got = 0usize;
        loop {
            let r = syscall3(SYS_READ, fd, (core::ptr::addr_of_mut!(FILEBUF) as i64) + got as i64,
                             (FILE_CAP - got) as i64);
            if r <= 0 { break; }
            got += r as usize;
            if got >= FILE_CAP { break; }
        }
        syscall1(SYS_CLOSE, fd);
        a.file_len = got;
        a.name.clear();
        // Show the leaf name only; the directory is on the list label.
        let full = &(*core::ptr::addr_of!(FILES))[idx];
        let mut last = 0usize;
        let mut i = 0usize;
        while i < full.len() && full[i] != 0 {
            if full[i] == b'/' { last = i + 1; }
            i += 1;
        }
        a.name.s(&full[last..]);
        load_bytes(a, false);
    }
}

impl App {
    fn ch_close(&mut self) { self.status.ch(b')'); }
}

/// Scan the usual places for .MID files. The FIRST directory that exists and
/// contains one wins; the label says which, so an empty list is never
/// ambiguous about where it looked.
fn scan_files(a: &mut App, label: &mut Buf) {
    const DIRS: [&[u8]; 4] = [b"/MEDIA/MIDI\0", b"/MIDI\0", b"/MUSIC\0", b"/\0"];
    unsafe {
        NFILES = 0;
        for d in DIRS.iter() {
            let fd = syscall2(SYS_OPEN, d.as_ptr() as i64, O_RDONLY);
            if fd < 0 { continue; }
            let mut de = KDirent { name: [0; 256], ty: 0, size: 0 };
            loop {
                let r = syscall2(SYS_READDIR, fd, &mut de as *mut KDirent as i64);
                if r != 0 { break; }
                if de.ty == 1 { continue; }
                // .MID or .mid, case-insensitively, on the last four bytes.
                let mut n = 0usize;
                while n < 255 && de.name[n] != 0 { n += 1; }
                if n < 5 { continue; }
                let ext = &de.name[n - 4..n];
                let low = |c: u8| if c >= b'A' && c <= b'Z' { c + 32 } else { c };
                if !(ext[0] == b'.' && low(ext[1]) == b'm' && low(ext[2]) == b'i' && low(ext[3]) == b'd') {
                    continue;
                }
                if NFILES >= MAX_FILES { break; }
                let mut p = Buf::new();
                // d ends with a NUL; strip it, and avoid "//" for the root.
                let mut i = 0usize;
                while d[i] != 0 { p.ch(d[i]); i += 1; }
                if p.n > 0 && p.b[p.n - 1] != b'/' { p.ch(b'/'); }
                p.s(&de.name[..n]);
                let dst = &mut (*core::ptr::addr_of_mut!(FILES))[NFILES];
                let m = if p.n < 63 { p.n } else { 63 };
                dst[..m].copy_from_slice(&p.b[..m]);
                dst[m] = 0;
                NFILES += 1;
            }
            syscall1(SYS_CLOSE, fd);
            if NFILES > 0 {
                label.clear();
                label.s(b"MIDI files in ");
                let mut i = 0usize;
                while d[i] != 0 { label.ch(d[i]); i += 1; }
                return;
            }
        }
        label.clear();
        label.s(b"No .MID files found in /MEDIA/MIDI, /MIDI, /MUSIC or /");
        let _ = a;
    }
}

// ---------------------------------------------------------------------------
// Drawing. Geometry from docs/MIDIPLAY_SPEC.md.
// ---------------------------------------------------------------------------
fn draw(a: &mut App, list_label: &Buf) {
    let w = a.dw;
    let h = a.dh;
    let p = &a.pal;
    win_rect(a.win, 0, 0, w, h, p.surface);

    let mut b = Buf::new();

    // --- file info row -----------------------------------------------------
    draw_text(a.win, 8, 8, a.name.as_bytes(), 14, p.ink);
    b.clear();
    unsafe {
        let pl = &*core::ptr::addr_of!(PLAYER);
        if a.err == E_OK {
            b.s(b"Format ");
            b.i(pl.hdr.format as i64);
            b.s(b"  ");
            b.i(pl.hdr.tracks_used as i64);
            b.s(b" trk  ");
            b.i(pl.hdr.ppq as i64);
            b.s(b" PPQ");
        } else {
            b.s(b"not loaded");
        }
    }
    draw_text(a.win, w - 210, 10, b.as_bytes(), 11, p.ink_dim);

    // --- transport ---------------------------------------------------------
    let playing = a.state == State::Playing;
    // #217: was `a.pcm >= 1`. With lazy acquisition the handle is -1 for most
    // of this app's life, so that test would have disabled Play permanently.
    // The question the button is really asking is "could this machine play?",
    // which is audio_ok: true until an open actually returns ENODEV.
    let can_play = a.err == E_OK && a.audio_ok;
    unsafe {
        gui_button(a.win, 8, 36, 110, 24,
                   if playing { b"Pause (Space)\0".as_ptr() } else { b"Play (Space)\0".as_ptr() },
                   GUI_BTN_PRIMARY,
                   if can_play { GUI_ST_NORMAL } else { GUI_ST_DISABLED });
        gui_button(a.win, 126, 36, 90, 24, b"Stop (S)\0".as_ptr(), GUI_BTN_SECONDARY,
                   if a.state == State::Idle { GUI_ST_DISABLED } else { GUI_ST_NORMAL });
        gui_button(a.win, 224, 36, 100, 24, b"Rewind (R)\0".as_ptr(), GUI_BTN_SECONDARY,
                   GUI_ST_NORMAL);
        gui_button(a.win, 332, 36, 90, 24, b"Open (O)\0".as_ptr(), GUI_BTN_SECONDARY,
                   if unsafe_nfiles() > 0 { GUI_ST_NORMAL } else { GUI_ST_DISABLED });
        gui_button(a.win, 430, 36, 122, 24, b"Self-test (T)\0".as_ptr(), GUI_BTN_SECONDARY,
                   GUI_ST_NORMAL);
    }

    // --- progress + position ----------------------------------------------
    let cur = unsafe { (*core::ptr::addr_of!(PLAYER)).cur };
    let pct = if a.total_frames > 0 {
        let v = cur * 100 / a.total_frames;
        if v > 100 { 100 } else { v as i32 }
    } else { 0 };
    unsafe { gui_progress(a.win, 8, 68, w - 16, 12, pct); }
    b.clear();
    b.time(cur, RATE);
    b.s(b" / ");
    b.time(a.total_frames, RATE);
    draw_text(a.win, w / 2 - 40, 84, b.as_bytes(), 14, p.ink_dim);

    // --- tempo / rate ------------------------------------------------------
    b.clear();
    b.s(b"Tempo: ");
    unsafe { b.i((*core::ptr::addr_of!(PLAYER)).clock.bpm() as i64); }
    b.s(b" BPM    Sample rate: ");
    b.i(RATE as i64);
    b.s(b" Hz    ");
    // The cost, in the only unit that matters: microseconds of CPU per second
    // of audio produced. Reported rather than left to #187.
    if a.render_frames > 0 {
        // Tenths. The first VM run printed "Synth: 0% of one core" for a real
        // cost near half a percent, which is a number that tells nobody
        // anything. #187 is measuring the same cost for the DOS FM bridge.
        let audio_us = (a.render_frames * 1_000_000 / RATE as u64).max(1);
        let tenths = a.render_us * 1000 / audio_us;
        b.s(b"Synth: ");
        b.i((tenths / 10) as i64);
        b.ch(b'.');
        b.i((tenths % 10) as i64);
        b.s(b"% of one core");
    }
    draw_text(a.win, 8, 112, b.as_bytes(), 14, p.ink);

    // --- voice monitor -----------------------------------------------------
    unsafe { gui_card(a.win, 8, 140, w - 16, 180); }
    draw_text(a.win, 14, 142, b"V\0", 11, p.ink_dim);
    draw_text(a.win, 34, 142, b"On\0", 11, p.ink_dim);
    draw_text(a.win, 58, 142, b"Ch\0", 11, p.ink_dim);
    draw_text(a.win, 110, 142, b"Note\0", 11, p.ink_dim);
    draw_text(a.win, 182, 142, b"Level (commanded, not measured)\0", 11, p.ink_dim);
    for i in 0..NUM_CHANNELS {
        let y = 158 + i as i32 * 18;
        let v = unsafe { (*core::ptr::addr_of!(SYNTH)).voices[i] };
        b.clear();
        b.i(i as i64 + 1);
        draw_text(a.win, 14, y, b.as_bytes(), 11, p.ink_dim);
        let lamp = if v.on { p.accent } else if v.held { p.accent_hover } else { p.track };
        win_rect(a.win, 34, y + 3, 12, 12, lamp);
        b.clear();
        if v.on || v.held {
            b.s(b"Ch ");
            b.i(v.mch as i64 + 1);
        } else {
            b.s(b"-");
        }
        draw_text(a.win, 58, y, b.as_bytes(), 11, p.ink);
        b.clear();
        if v.on || v.held {
            note_name(v.note, &mut b);
        } else {
            b.s(b"-");
        }
        draw_text(a.win, 110, y, b.as_bytes(), 11, p.ink);
        // The bar is the COMMANDED state, not a measured amplitude: the FM core
        // reports silence for the whole chip, not per channel, so there is no
        // honest way to draw a real per-voice meter from here. Full while keyed,
        // half while sustained by the pedal, empty otherwise, and the header
        // says so rather than implying a VU meter.
        let tw = w - 16 - 182;
        win_rect(a.win, 182, y + 4, tw, 10, p.track);
        let fillw = if v.on { tw - 2 } else if v.held { (tw - 2) / 2 } else { 0 };
        if fillw > 0 {
            win_rect(a.win, 183, y + 5, fillw, 8, p.accent);
        }
    }

    // --- counters ----------------------------------------------------------
    let (notes, steals, clamped) = unsafe {
        let s = &*core::ptr::addr_of!(SYNTH);
        (s.notes, s.steals, s.clamped)
    };
    b.clear();
    b.s(b"Notes: ");
    b.i(notes as i64);
    draw_text(a.win, 8, 328, b.as_bytes(), 11, p.ink_dim);
    b.clear();
    b.s(b"Stolen: ");
    b.i(steals as i64);
    draw_text(a.win, 144, 328, b.as_bytes(), 11,
              if steals > 0 { p.accent } else { p.ink_dim });
    b.clear();
    b.s(b"Clamped: ");
    b.i(clamped as i64);
    draw_text(a.win, 280, 328, b.as_bytes(), 11, p.ink_dim);
    b.clear();
    b.s(b"Short writes: ");
    b.i(a.underruns as i64);
    draw_text(a.win, 416, 328, b.as_bytes(), 11, p.ink_dim);

    // --- file list ---------------------------------------------------------
    draw_text(a.win, 8, 351, list_label.as_bytes(), 11, p.ink_dim);
    let list_h = h - 369 - 26;
    let rows = (list_h / 20).max(1) as usize;
    unsafe { gui_card(a.win, 8, 369, w - 16, list_h); }
    let n = unsafe_nfiles();
    for r in 0..rows {
        let idx = a.scroll + r;
        if idx >= n { break; }
        let y = 369 + r as i32 * 20;
        if idx == a.sel {
            win_rect(a.win, 10, y, w - 20, 20, p.accent);
        }
        let name = unsafe { &(*core::ptr::addr_of!(FILES))[idx] };
        draw_text(a.win, 14, y + 2, name, 14,
                  if idx == a.sel { ink_on(p.accent) } else { p.ink });
    }

    // --- status ------------------------------------------------------------
    draw_text(a.win, 8, h - 20, a.status.as_bytes(), 11, p.ink_dim);
    win_invalidate(a.win);
}

fn unsafe_nfiles() -> usize { unsafe { NFILES } }

// ---------------------------------------------------------------------------
// The in-OS self-test. Same suite as the host harness, same source, and the
// report goes to a FILE because a compositor-spawned app's putchar does not
// reach the serial console.
// ---------------------------------------------------------------------------
fn run_selftest(a: &mut App) {
    unsafe {
        REPORT_N = 0;
        rep(b"[MIDIPLAY] #183 MIDI player self-test, IN-OS arm (Ring 3), 44100 Hz\n");
        rep(b"[MIDIPLAY] library: userland/lib/midi, FM core: userland/lib/opl2\n");
        rep(b"[MIDIPLAY] THIS ARM PROVES: the Ring-3 ELF links and loads, its .bss\n");
        rep(b"[MIDIPLAY] buffers are reachable, and the arithmetic is unchanged after\n");
        rep(b"[MIDIPLAY] the userland toolchain. It does NOT prove a speaker made a\n");
        rep(b"[MIDIPLAY] sound; there is no loopback in this machine.\n\n");
        let buf = &mut *core::ptr::addr_of_mut!(SELFTEST_BUF);
        let pl = &mut *core::ptr::addr_of_mut!(PLAYER);
        let sy = &mut *core::ptr::addr_of_mut!(SYNTH);
        let sc = &mut *core::ptr::addr_of_mut!(SELFTEST_SCRATCH);
        let mut emit = |r: &MidiReport| {
            let mut l = Buf::new();
            l.s(b"  ");
            l.s(r.name.as_bytes());
            let mut k = r.name.len();
            while k < 24 { l.ch(b' '); k += 1; }
            l.s(if r.pass { b"PASS  " } else { b"FAIL  " });
            l.i(r.measured);
            l.ch(b' ');
            l.i(r.expected);
            l.s(b"  ");
            l.s(r.note.as_bytes());
            l.ch(b'\n');
            rep_buf(&l);
        };
        let fails = midi_run_all(RATE, buf, pl, sy, sc, &mut emit);
        let mut l = Buf::new();
        l.s(b"\n[MIDIPLAY] SUITE: ");
        l.i(fails as i64);
        l.s(b" failing checks");
        if fails == 0 {
            l.s(b" (every RED arm passed by correctly FAILING)\n");
        } else {
            l.s(b" <<<< SUITE FAILED\n");
        }
        rep_buf(&l);
        let n = write_report(b"/MIDIPLAY.TXT\0");
        // RELOAD FIRST, THEN SET THE STATUS. The suite left the player pointed
        // at its own generated files, so the UI has to be re-pointed at the
        // user's file; but load_bytes sets its own status, so doing it the other
        // way round overwrote the result with "Ready" and the first VM run
        // showed a self-test that had plainly run and reported nothing.
        let was_demo = a.using_demo;
        load_bytes(a, was_demo);
        a.status.clear();
        a.status.s(b"Self-test: ");
        a.status.i(fails as i64);
        a.status.s(b" failures, report written to /MIDIPLAY.TXT (");
        a.status.i(n);
        a.status.s(b" bytes)");
    }
}

// ---------------------------------------------------------------------------
// #217: THE PCM STREAM IS AN EXCLUSIVE MACHINE-WIDE RESOURCE. HOLD IT ONLY
// WHILE ACTUALLY MAKING SOUND.
//
// CORRECTED 2026-08-26 (#205): THERE IS A MIXER NOW, and the paragraph below
// described the world it replaced. The old model was one hardware engine, no
// mixer, ONE software stream, first-opener-wins, and a second opener refused
// with EBUSY. That contract is exactly what made this app silent on the owner's
// laptop: /APPS/FMSYNTH took the one stream for a DOS game and Play here was
// answered EBUSY with a message no user could act on. drivers/audio_pcm.c now
// sums up to PCM_MAX_STREAMS producers into the one hardware stream, so this
// app and a DOS game sound at the same time.
//
// WHAT DOES NOT CHANGE, and why these two functions stay: holding a stream you
// are not using is still wasteful and still consumes one of a small number of
// slots, so lazy acquire on Play and release on Stop remains correct. EBUSY is
// still possible, just no longer routine, and is still reported honestly.
// ---------------------------------------------------------------------------

/// Take the stream. Returns false and sets a status line saying WHICH refusal
/// it was; the caller must not enter Playing when it returns false.
fn pcm_acquire(a: &mut App) -> bool {
    if a.pcm >= 1 {
        return true;
    }
    let h = unsafe { syscall3(SYS_AUDIO_PCM_OPEN, RATE as i64, 2, AUDIO_FORMAT_S16_LE) };
    if h >= 1 {
        a.pcm = h;
        a.audio_ok = true;
        return true;
    }
    a.pcm = -1;
    a.status.clear();
    if h == PCM_ENODEV {
        // A VM with no audio device is a legitimate configuration. Saying so is
        // the point: #182 found a chip advertising itself as PRESENT with
        // silence behind it, because a successful call is not proof that sound
        // will come out. Playback is DISABLED here rather than pretending.
        a.audio_ok = false;
        a.status.s(b"No audio device on this machine: playback disabled, self-test still works");
    } else if h == PCM_EBUSY {
        // #205: with a mixer this is no longer "something else is playing", it
        // is "every mixer slot is taken", which is rare and worth naming as the
        // different thing it is. /AUDIOLOG.TXT lists every holder by pid.
        a.status.s(b"Audio unavailable: every PCM mixer slot is in use. Close a sound-playing app and press Play again (/AUDIOLOG.TXT names every holder)");
    } else {
        a.status.s(b"Cannot open audio (pcm_open returned ");
        a.status.i(h);
        a.status.s(b")");
    }
    false
}

/// Give the stream back. Idempotent, and safe to call from any state.
///
/// The close DRAINS: the kernel plays out whatever is still in the ring before
/// the pump exits, which is right for a stop (the last rendered block is heard)
/// and is why Pause does NOT call this immediately (see PAUSE_RELEASE_TICKS).
fn pcm_release(a: &mut App) {
    if a.pcm >= 1 {
        unsafe { syscall1(SYS_AUDIO_PCM_CLOSE, a.pcm); }
    }
    a.pcm = -1;
    a.paused_ticks = 0;
}

/// #217: 200 ms idle-loop timeouts before a PAUSED player releases the stream.
/// 50 = 10 seconds.
///
/// WHY PAUSE HOLDS AT ALL. Releasing on the pause keystroke would be worse in
/// two measurable ways: audio_pcm_close() drains, so up to a full ring
/// (PCM_RING_FRAMES = 32768 frames, ~0.74 s at this app's 44.1 kHz) of
/// already-rendered music would keep playing AFTER the user pressed Pause; and
/// the resume could then be refused, turning a pause into a stop. WHY IT DOES
/// NOT HOLD FOR EVER: a pause the user walked away from is a stop that has not
/// been typed, and #217 is precisely the bug where a silent app owned the whole
/// machine's audio. Ten seconds is long enough that a deliberate pause-and-
/// resume never notices, and short enough that walking away frees the machine.
const PAUSE_RELEASE_TICKS: u32 = 50;

// ---------------------------------------------------------------------------
// One block of audio. THIS IS THE ONLY PLACE PLAYBACK ADVANCES.
// ---------------------------------------------------------------------------
fn pump(a: &mut App) {
    unsafe {
        let pl = &mut *core::ptr::addr_of_mut!(PLAYER);
        let sy = &mut *core::ptr::addr_of_mut!(SYNTH);
        let pb = &mut *core::ptr::addr_of_mut!(PCMBUF);
        let t0 = mono_us();
        let n = pl.render_block(sy, pb);
        // SYS_MONO_US is TSC-backed. It appears here and nowhere else: this is
        // a COST measurement, never a schedule.
        a.render_us += mono_us().saturating_sub(t0);
        a.render_frames += n as u64;
        if n > 0 {
            let wrote = syscall3(SYS_AUDIO_PCM_WRITE, a.pcm, pb.as_ptr() as i64, n as i64);
            if wrote < 0 {
                a.state = State::Idle;
                // #217: the sink is gone or the stream was torn down under us.
                // Drop the handle so the next Play re-acquires cleanly instead
                // of writing for ever into a dead handle, and so the slot is
                // not pinned by a player that has stopped.
                pcm_release(a);
                a.status.clear();
                a.status.s(b"Audio sink error ");
                a.status.i(wrote);
                return;
            }
            if wrote as usize != n {
                a.underruns += 1;
            }
        }
        if pl.finished {
            if a.flush_left == 0 {
                a.flush_left = FLUSH_BLOCKS;
                sy.all_off();
                set_status(a, b"Finished");
            }
        }
        if a.flush_left > 0 {
            a.flush_left -= 1;
            for v in pb.iter_mut() {
                *v = 0;
            }
            let _ = syscall3(SYS_AUDIO_PCM_WRITE, a.pcm, pb.as_ptr() as i64,
                             BLOCK_FRAMES as i64);
            if a.flush_left == 0 {
                a.state = State::Idle;
                // #217: THE SONG IS OVER. This is the state the reported fault
                // lived in: a player sitting at "Finished" with nothing to say,
                // holding the only PCM stream on the machine. The silence
                // flush above has been written, so the close's drain plays it
                // out and then the slot goes back to the pool.
                pcm_release(a);
            }
        }
    }
}

fn rewind(a: &mut App) {
    a.flush_left = 0;
    unsafe {
        let pl = &mut *core::ptr::addr_of_mut!(PLAYER);
        let sy = &mut *core::ptr::addr_of_mut!(SYNTH);
        sy.all_off();
        pl.rewind();
        let perc = sy.perc_reserved();
        sy.init(RATE, perc);
        a.underruns = 0;
        a.render_us = 0;
        a.render_frames = 0;
    }
}

#[no_mangle]
pub extern "C" fn main() -> i32 {
    // SYS_WIN_CREATE takes the OUTER size; content is roughly outer minus 4
    // horizontally and 24 vertically. The layout below uses the size READ BACK
    // from win_get_size rather than that arithmetic, because a hard-coded
    // content size is how Rogue lost its status row.
    let win = win_create(b"MIDI Player\0", 100, 60, 564, 584);
    if win < 0 {
        return 1;
    }
    let mut a = App {
        win,
        dw: 560,
        dh: 560,
        pal: GuiPalette { surface: 0, surface_raised: 0, ink: 0, ink_dim: 0, accent: 0,
                          accent_hover: 0, border: 0, field_bg: 0, field_border: 0, track: 0 },
        dark: false,
        state: State::Idle,
        pcm: -1,
        audio_ok: true,
        paused_ticks: 0,
        file_len: 0,
        demo_len: 0,
        using_demo: true,
        total_frames: 0,
        sel: 0,
        scroll: 0,
        name: Buf::new(),
        status: Buf::new(),
        err: E_OK,
        underruns: 0,
        render_us: 0,
        render_frames: 0,
        flush_left: 0,
    };
    win_get_size(win, &mut a.dw, &mut a.dh);
    a.apply_theme();

    let mut list_label = Buf::new();
    scan_files(&mut a, &mut list_label);
    load_demo(&mut a, false);

    // #217: NO OPEN HERE. This line used to be `a.pcm = pcm_open(...)`, and it
    // is the whole of the reported fault: the player took the machine's only
    // Ring-3 PCM stream before it had been asked to play anything, and did not
    // give it back until it exited. On the owner's real boot that meant a
    // stopped MIDI player made every other audio client on the box fail with
    // EBUSY. The stream is now taken in pcm_acquire(), on the Play that
    // actually needs it.
    //
    // The cost of removing it: we no longer learn at startup whether this
    // machine has a sink, so the Play button starts ENABLED and the first Play
    // is where ENODEV is reported. That is a better answer anyway - a device
    // that exists at startup can be busy at Play, and vice versa, so the
    // startup answer was never the one that mattered.

    draw(&mut a, &list_label);

    let mut ev = GuiEvent { ty: 0, target_id: 0, mouse_x: 0, mouse_y: 0, mouse_buttons: 0,
                            scroll_delta: 0, keycode: 0, key_char: 0 };
    let mut dirty = true;
    let mut blocks_since_draw = 0u32;

    loop {
        // WHERE THIS LOOP BLOCKS. Playing: inside sys_audio_pcm_write, on the
        // PCM ring's wait queue, which is also what paces playback. Idle:
        // inside sys_win_get_event, on the window's wait queue. Never a spin.
        if a.state == State::Playing || a.flush_left > 0 {
            pump(&mut a);
            blocks_since_draw += 1;
            // Two blocks is 93 ms, about 11 redraws a second. Redrawing every
            // block would spend more time in the compositor than in the synth.
            if blocks_since_draw >= 2 {
                blocks_since_draw = 0;
                dirty = true;
            }
            while win_event(win, &mut ev, 0) != 0 {
                if handle(&mut a, &ev, &mut list_label) {
                    unsafe { syscall1(SYS_WIN_DESTROY, win as i64); }
                    pcm_release(&mut a);
                    return 0;
                }
                dirty = true;
            }
        } else {
            // #217: a PAUSED player that nobody comes back to is a stopped
            // player that still owns the machine's audio. win_event() returns 0
            // on its 200 ms timeout, so this counts real elapsed idle time
            // without adding a timer or a poll of its own.
            if a.state == State::Paused && a.pcm >= 1 {
                a.paused_ticks += 1;
                if a.paused_ticks >= PAUSE_RELEASE_TICKS {
                    pcm_release(&mut a);
                    set_status(&mut a, b"Paused (audio released; Play re-acquires it)");
                    dirty = true;
                }
            }
            if win_event(win, &mut ev, 200) != 0 {
                if handle(&mut a, &ev, &mut list_label) {
                    unsafe { syscall1(SYS_WIN_DESTROY, win as i64); }
                    pcm_release(&mut a);
                    return 0;
                }
                dirty = true;
            }
        }
        if dirty {
            dirty = false;
            draw(&mut a, &list_label);
        }
    }
}

/// Returns true when the app should quit.
fn handle(a: &mut App, ev: &GuiEvent, list_label: &mut Buf) -> bool {
    match ev.ty {
        EVENT_WINDOW_CLOSE => return true,
        EVENT_REDRAW => {}
        EVENT_RESIZE => {
            win_get_size(a.win, &mut a.dw, &mut a.dh);
        }
        EVENT_MOUSE_SCROLL => {
            let d = ev.scroll_delta as i32;
            if d < 0 { a.scroll += 1; } else if a.scroll > 0 { a.scroll -= 1; }
        }
        EVENT_MOUSE_DOWN => {
            // Content-relative coordinates. The compositor delivers screen
            // coordinates and the 2 px border plus 20 px titlebar have to come
            // off; that translation is the recurring userland-app bug this
            // project has a durable note about, so it is done ONCE here.
            let x = ev.mouse_x;
            let y = ev.mouse_y;
            if y >= 36 && y < 60 {
                if x >= 8 && x < 118 { toggle_play(a); }
                else if x >= 126 && x < 216 { stop(a); }
                else if x >= 224 && x < 324 { rewind(a); }
                else if x >= 332 && x < 422 { load_file(a, a.sel); }
                else if x >= 430 && x < 552 { run_selftest(a); }
            } else if y >= 369 {
                let row = ((y - 369) / 20) as usize + a.scroll;
                if row < unsafe_nfiles() {
                    a.sel = row;
                }
            }
        }
        EVENT_KEY_DOWN => {
            match ev.keycode {
                keys::GUI_KEY_UP => {
                    if a.sel > 0 { a.sel -= 1; }
                    if a.sel < a.scroll { a.scroll = a.sel; }
                    return false;
                }
                keys::GUI_KEY_DOWN => {
                    if a.sel + 1 < unsafe_nfiles() { a.sel += 1; }
                    let rows = ((a.dh - 369 - 26) / 20).max(1) as usize;
                    if a.sel >= a.scroll + rows { a.scroll = a.sel + 1 - rows; }
                    return false;
                }
                _ => {}
            }
            let c = ev.key_char;
            let lower = if c >= b'A' && c <= b'Z' { c + 32 } else { c };
            match lower {
                27 => return true,
                b' ' => toggle_play(a),
                b's' => stop(a),
                b'r' => { rewind(a); set_status(a, b"Rewound to start"); }
                b'o' | 13 => load_file(a, a.sel),
                b'd' => load_demo(a, false),
                b'a' => load_demo(a, true),
                b't' => run_selftest(a),
                b'f' => { scan_files(a, list_label); }
                _ => {}
            }
        }
        _ => {}
    }
    false
}

fn toggle_play(a: &mut App) {
    if a.err != E_OK {
        return;
    }
    match a.state {
        State::Playing => {
            a.state = State::Paused;
            a.paused_ticks = 0;
            unsafe { (*core::ptr::addr_of_mut!(SYNTH)).all_off(); }
            set_status(a, b"Paused");
        }
        _ => {
            // #217: ACQUIRE HERE, AND NOWHERE ELSE. If the stream is held by
            // another app we must NOT enter Playing: the render loop would spin
            // rendering blocks it cannot write, and pcm_acquire() has already
            // put the reason on the status line.
            if !pcm_acquire(a) {
                return;
            }
            let finished = unsafe { (*core::ptr::addr_of!(PLAYER)).finished };
            if finished {
                rewind(a);
            }
            a.state = State::Playing;
            set_status(a, b"Playing");
        }
    }
}

fn stop(a: &mut App) {
    a.state = State::Idle;
    rewind(a);
    // #217: a stopped player must not hold the machine's only PCM stream. This
    // is the exact state the owner's boot was measured in.
    pcm_release(a);
    set_status(a, b"Stopped");
}
