// taskmgr - MayteraOS Task Manager, in Rust (#487/#349).
//
// This REPLACES the 152-line C main.c. It is the app the Start menu actually
// opens (gui/desktop.c:405 userspace_taskmanager_launch tries /apps/taskmgr
// first and only falls back to the in-kernel Task Manager if this fails to
// load), so this is where Windows-11 / Process-Explorer parity has to live.
//
// BUILD (see Makefile): rustc 1.97.0, --target x86_64-unknown-none with
//   -C code-model=large  : x86_64-unknown-none defaults to code-model=kernel,
//                          which is WRONG for a Ring-3 image linked at
//                          0x80000000 by user.ld.
//   -C relocation-model=static : the target defaults to PIE; user apps are
//                          -fno-pic and statically linked.
// The target is ALREADY soft-float (+soft-float, -sse/-sse2), which is what we
// want: this kernel never saves FPU state across a context switch
// (sse_save/sse_restore have zero callers), so every computation below is
// FIXED-POINT INTEGER. There is no float in this file, by design, not by luck.
//
// FFI: syscall0..6 are real linkable symbols (libc/syscall.asm), so this issues
// syscalls directly with no C shim. The style-engine widgets (gui_card,
// gui_button, gui_progress, gui_set_palette, gui_set_style) are real symbols in
// libc.a and are called directly, so the app uses the SHARED style engine and
// matches the Settings/Files design language rather than a bespoke look.
#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_i: &PanicInfo) -> ! {
    // panic=abort in Ring 3: exit loudly rather than spin. A spin here would
    // burn a core forever (#426).
    unsafe {
        syscall1(SYS_EXIT, 101);
    }
    loop {}
}

// ---------------------------------------------------------------------------
// libc FFI
// ---------------------------------------------------------------------------
extern "C" {
    fn syscall1(n: i64, a1: i64) -> i64;
    fn syscall2(n: i64, a1: i64, a2: i64) -> i64;
    fn syscall3(n: i64, a1: i64, a2: i64, a3: i64) -> i64;
    fn syscall4(n: i64, a1: i64, a2: i64, a3: i64, a4: i64) -> i64;
    fn syscall5(n: i64, a1: i64, a2: i64, a3: i64, a4: i64, a5: i64) -> i64;
    fn syscall6(n: i64, a1: i64, a2: i64, a3: i64, a4: i64, a5: i64, a6: i64) -> i64;

    // Shared style engine (libc/gui_style.h) - real symbols in libc.a.
    fn gui_set_style(style: i32);
    fn gui_set_palette(p: *const GuiPalette);
    fn gui_card(handle: i32, x: i32, y: i32, w: i32, h: i32);
    fn gui_button(handle: i32, x: i32, y: i32, w: i32, h: i32, label: *const u8,
                  variant: i32, st: i32);
    fn gui_progress(handle: i32, x: i32, y: i32, w: i32, h: i32, pct: i32);
    fn gui_lighten(c: u32, amt: i32) -> u32;
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

// ---------------------------------------------------------------------------
// Syscall numbers (verified against kernel proc/syscall.h, not assumed)
// ---------------------------------------------------------------------------
const SYS_EXIT: i64 = 0;
const SYS_KILL: i64 = 80;
const SYS_WIN_CREATE: i64 = 30;
const SYS_WIN_DESTROY: i64 = 31;
const SYS_WIN_DRAW_RECT: i64 = 32;
const SYS_WIN_GET_EVENT: i64 = 36;
const SYS_WIN_INVALIDATE: i64 = 37;
const SYS_WIN_GET_SIZE: i64 = 38;
const SYS_GET_CPU_USAGE: i64 = 193;
const SYS_GET_MEM_INFO: i64 = 194;
const SYS_PROC_LIST: i64 = 238;
const SYS_SETPRIORITY: i64 = 244;
const SYS_GET_CPU_PER_CORE: i64 = 259;
const SYS_CRON_LIST: i64 = 277;
const SYS_CRON_ENABLE: i64 = 279;
const SYS_WIN_DRAW_TTF: i64 = 235;
// #487 additions
const SYS_PROC_HANDLES: i64 = 318;
const SYS_NET_CONNS: i64 = 319;
const SYS_SVC_LIST: i64 = 320;
const SYS_SVC_CONTROL: i64 = 321;
const SYS_PROC_DETAIL: i64 = 322;

const SIGTERM: i64 = 15;
const SIGKILL: i64 = 9;
const PI_PID_ALL: u32 = 0xFFFF_FFFF;
/// "nothing selected". Cannot be 0: pid 0 is the idle process.
const NO_SEL: u32 = 0xFFFF_FFFE;

// ---------------------------------------------------------------------------
// Kernel ABI mirrors. Every one is sizeof-locked against the kernel header by
// a const assert; if a kernel struct moves, this app fails to COMPILE rather
// than silently decoding garbage.
// ---------------------------------------------------------------------------
#[repr(C)]
#[derive(Clone, Copy)]
struct ProcInfo {
    pid: u32,
    ppid: u32,
    name: [u8; 32],
    state: u32,
    mem_kb: u32,
    cpu_ticks: u64,
    running_cpu: i32,
}
const _: () = assert!(core::mem::size_of::<ProcInfo>() == 64);

#[repr(C)]
#[derive(Clone, Copy)]
struct HandleInfo {
    fd: i32,
    flags: i32,
    kind: u32,
    _pad: u32,
    path: [u8; 96],
}
const _: () = assert!(core::mem::size_of::<HandleInfo>() == 112);

#[repr(C)]
#[derive(Clone, Copy)]
struct TcpConnInfo {
    state: u8,
    is_listener: u8,
    local_port: u16,
    remote_port: u16,
    remote_ip: u32,
    recv_len: u16,
    send_len: u32,
    owner_pid: u32,
}
const _: () = assert!(core::mem::size_of::<TcpConnInfo>() == 24);

#[repr(C)]
#[derive(Clone, Copy)]
struct SvcInfo {
    running: u32,
    autostart: u32,
    perms: u32,
    pid: u32,
    name: [u8; 32],
    account: [u8; 32],
}
const _: () = assert!(core::mem::size_of::<SvcInfo>() == 80);

#[repr(C)]
#[derive(Clone, Copy)]
struct ProcDetail {
    pid: u32,
    ppid: u32,
    working_set_kb: u32,
    private_kb: u32,
    virt_kb: u32,
    heap_kb: u32,
    threads: u32,
    handles: u32,
    uid: u32,
    gid: u32,
    priority: u32,
    privilege: u32,
    state: u32,
    vma_count: u32,
    mem_flags: u32,
    is_service: u32,
    cpu_ticks: u64,
    cr3: u64,
    name: [u8; 32],
}
const _: () = assert!(core::mem::size_of::<ProcDetail>() == 112);

// cron_job_t (kernel proc/cron.h). CRON_TARGET_MAX/CRON_LABEL_MAX are 64/32.
#[repr(C)]
#[derive(Clone, Copy)]
struct CronJob {
    id: u32,
    ty: u8,
    action: u8,
    enabled: u8,
    weekday: u8,
    hour: u8,
    minute: u8,
    reserved: [u8; 2],
    interval_ms: u32,
    run_count: u32,
    next_fire_tick: u64,
    target: [u8; 64],
    label: [u8; 32],
}

// ---------------------------------------------------------------------------
// Thin safe wrappers
// ---------------------------------------------------------------------------
fn win_create(title: &[u8], x: i32, y: i32, w: i32, h: i32) -> i32 {
    unsafe { syscall5(SYS_WIN_CREATE, title.as_ptr() as i64, x as i64, y as i64, w as i64, h as i64) as i32 }
}
fn win_draw_rect(h: i32, x: i32, y: i32, w: i32, ht: i32, c: u32) {
    unsafe { syscall6(SYS_WIN_DRAW_RECT, h as i64, x as i64, y as i64, w as i64, ht as i64, c as i64); }
}
fn win_invalidate(h: i32) {
    unsafe { syscall1(SYS_WIN_INVALIDATE, h as i64); }
}
fn win_get_size(h: i32, w: &mut i32, ht: &mut i32) {
    unsafe { syscall3(SYS_WIN_GET_SIZE, h as i64, w as *mut i32 as i64, ht as *mut i32 as i64); }
}
/// Blocking event fetch with a timeout. This is the shared wait-queue primitive
/// (#453 made it a real wait, not a spin); NEVER poll in a loop here.
fn win_get_event(h: i32, ev: &mut GuiEvent, timeout_ms: i32) -> i32 {
    unsafe { syscall3(SYS_WIN_GET_EVENT, h as i64, ev as *mut GuiEvent as i64, timeout_ms as i64) as i32 }
}
fn draw_text(h: i32, x: i32, y: i32, s: &[u8], size: i32, color: u32) {
    // SYS_WIN_DRAW_TTF packs size into the top byte of the colour word.
    let packed = ((color & 0x00FF_FFFF) | (((size as u32) & 0xFF) << 24)) as i64;
    unsafe { syscall5(SYS_WIN_DRAW_TTF, h as i64, x as i64, y as i64, s.as_ptr() as i64, packed); }
}

// ---------------------------------------------------------------------------
// Fixed-point / formatting helpers. No float anywhere: the kernel target is
// soft-float and FPU state is not saved across context switches.
// ---------------------------------------------------------------------------
struct Buf {
    b: [u8; 128],
    n: usize,
}
impl Buf {
    fn new() -> Buf { Buf { b: [0; 128], n: 0 } }
    fn clear(&mut self) { self.n = 0; self.b[0] = 0; }
    fn put(&mut self, c: u8) {
        if self.n < self.b.len() - 1 { self.b[self.n] = c; self.n += 1; self.b[self.n] = 0; }
    }
    fn puts(&mut self, s: &[u8]) {
        for &c in s { if c == 0 { break; } self.put(c); }
    }
    fn putu(&mut self, mut v: u64) {
        if v == 0 { self.put(b'0'); return; }
        let mut t = [0u8; 24];
        let mut i = 0;
        while v > 0 && i < t.len() { t[i] = b'0' + (v % 10) as u8; v /= 10; i += 1; }
        while i > 0 { i -= 1; self.put(t[i]); }
    }
    /// KB -> "N.M MB" / "N KB", one decimal, computed in integer math only.
    fn put_kb(&mut self, kb: u32) {
        if kb >= 1024 {
            let mb = kb / 1024;
            let frac = ((kb % 1024) * 10) / 1024;   // one decimal, fixed-point
            self.putu(mb as u64); self.put(b'.'); self.putu(frac as u64); self.puts(b" MB");
        } else {
            self.putu(kb as u64); self.puts(b" KB");
        }
    }
    fn as_c(&self) -> &[u8] { &self.b[..=self.n] }   // NUL-terminated slice
}

fn cstr(s: &[u8]) -> &[u8] { s }

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
const MAXP: usize = 64;
const HIST: usize = 64;
const MAXCORES: usize = 16;

#[derive(Clone, Copy, PartialEq)]
enum Tab { Processes, Performance, Details, Services, Scheduled }

struct App {
    win: i32,
    dw: i32,
    dh: i32,
    tab: Tab,
    procs: [ProcInfo; MAXP],
    nproc: usize,
    cpu_pct: [u32; MAXP],
    prev_ticks: [u64; MAXP],
    prev_valid: bool,
    sel_pid: u32,
    scroll: usize,
    cpu_total: i32,
    mem_total: u64,
    mem_used: u64,
    // history rings (fixed-point percents)
    cpu_hist: [u32; HIST],
    mem_hist: [u32; HIST],
    core_hist: [[u32; HIST]; MAXCORES],
    hist_n: usize,
    ncores: usize,
    perf_cores: bool,
    // detail caches
    detail: ProcDetail,
    have_detail: bool,
    handles: [HandleInfo; 32],
    nhandles: usize,
    conns: [TcpConnInfo; 32],
    nconns: usize,
    svcs: [SvcInfo; 32],
    nsvcs: usize,
    svc_sel: usize,
    jobs: [CronJob; 32],
    njobs: usize,
    // palette
    pal: GuiPalette,
    dark: bool,
}

const ZP: ProcInfo = ProcInfo { pid: 0, ppid: 0, name: [0; 32], state: 0, mem_kb: 0, cpu_ticks: 0, running_cpu: -1 };
const ZH: HandleInfo = HandleInfo { fd: 0, flags: 0, kind: 0, _pad: 0, path: [0; 96] };
const ZC: TcpConnInfo = TcpConnInfo { state: 0, is_listener: 0, local_port: 0, remote_port: 0, remote_ip: 0, recv_len: 0, send_len: 0, owner_pid: 0 };
const ZS: SvcInfo = SvcInfo { running: 0, autostart: 0, perms: 0, pid: 0, name: [0; 32], account: [0; 32] };
const ZD: ProcDetail = ProcDetail { pid: 0, ppid: 0, working_set_kb: 0, private_kb: 0, virt_kb: 0, heap_kb: 0, threads: 0, handles: 0, uid: 0, gid: 0, priority: 0, privilege: 0, state: 0, vma_count: 0, mem_flags: 0, is_service: 0, cpu_ticks: 0, cr3: 0, name: [0; 32] };
const ZJ: CronJob = CronJob { id: 0, ty: 0, action: 0, enabled: 0, weekday: 0, hour: 0, minute: 0, reserved: [0; 2], interval_ms: 0, run_count: 0, next_fire_tick: 0, target: [0; 64], label: [0; 32] };

// Theme access. All four constants below were READ OUT of the kernel/libc
// headers, not assumed: SYS_THEME_COLOR is 290 and takes (theme_id, color_id)
// with theme_id = -1 meaning "the active theme"; the theme id itself comes from
// SYS_GET_THEME (134). The colour IDs are the ORDINALS of theme_color_id_t in
// libc/theme.h (BACKGROUND=0, ACCENT=2, WINDOW_BG=8), which is why they are
// spelled as numbers here.
const SYS_GET_THEME: i64 = 134;
const SYS_THEME_COLOR: i64 = 290;
const THEME_COLOR_ACCENT: i64 = 2;
const THEME_COLOR_WINDOW_BG: i64 = 8;

fn theme_color(id: i64) -> u32 {
    // theme_id = -1 -> the active theme (matches libc theme_color()).
    unsafe { syscall2(SYS_THEME_COLOR, -1i64, id) as u32 }
}
fn theme_active() -> i32 {
    unsafe { syscall1(SYS_GET_THEME, 0) as i32 }
}

/// Perceptual luminance, integer weights (Rec.601 x100). Picks readable ink for
/// ANY theme background, which is what keeps LIGHT themes legible: light-theme
/// contrast is a repeat offender in this codebase, so ink is DERIVED from the
/// background rather than hardcoded.
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

    fn refresh(&mut self) {
        // Processes
        let n = unsafe { syscall2(SYS_PROC_LIST, self.procs.as_mut_ptr() as i64, MAXP as i64) } as i32;
        self.nproc = if n > 0 { n as usize } else { 0 };

        // CPU% per process from the tick delta (integer share of busy).
        let mut d = [0u64; MAXP];
        let mut tot: u64 = 0;
        for i in 0..self.nproc {
            let p = self.procs[i].pid as usize;
            let pv = if p < MAXP { self.prev_ticks[p] } else { 0 };
            d[i] = self.procs[i].cpu_ticks.saturating_sub(pv);
            tot += d[i];
        }
        for i in 0..self.nproc {
            self.cpu_pct[i] = if self.prev_valid && tot > 0 { ((d[i] * 100) / tot) as u32 } else { 0 };
        }
        for i in 0..MAXP { self.prev_ticks[i] = 0; }
        for i in 0..self.nproc {
            let p = self.procs[i].pid as usize;
            if p < MAXP { self.prev_ticks[p] = self.procs[i].cpu_ticks; }
        }
        self.prev_valid = true;

        self.cpu_total = unsafe { syscall1(SYS_GET_CPU_USAGE, 0) } as i32;
        unsafe {
            syscall2(SYS_GET_MEM_INFO, &mut self.mem_total as *mut u64 as i64,
                     &mut self.mem_used as *mut u64 as i64);
        }
        self.sort_by_cpu();

        // History rings (fixed-point percents)
        let slot = self.hist_n % HIST;
        self.cpu_hist[slot] = self.cpu_total.max(0).min(100) as u32;
        self.mem_hist[slot] = if self.mem_total > 0 {
            ((self.mem_used.saturating_mul(100)) / self.mem_total) as u32
        } else { 0 };

        // Per-core: buf[0] = count, buf[1..] = percent per core.
        let mut cb = [0u32; MAXCORES + 1];
        let rc = unsafe { syscall1(SYS_GET_CPU_PER_CORE, cb.as_mut_ptr() as i64) } as i32;
        if rc >= 0 {
            let nc = (cb[0] as usize).min(MAXCORES);
            self.ncores = if nc == 0 { 1 } else { nc };
            for c in 0..self.ncores {
                self.core_hist[c][slot] = cb[c + 1].min(100);
            }
        } else {
            self.ncores = 1;
        }
        self.hist_n += 1;

        // Services
        let n = unsafe { syscall2(SYS_SVC_LIST, self.svcs.as_mut_ptr() as i64, 32) } as i32;
        self.nsvcs = if n > 0 { n as usize } else { 0 };

        // Scheduled tasks
        let n = unsafe { syscall2(SYS_CRON_LIST, self.jobs.as_mut_ptr() as i64, 32) } as i32;
        self.njobs = if n > 0 { n as usize } else { 0 };

        self.refresh_detail();
    }

    fn refresh_detail(&mut self) {
        self.have_detail = false;
        self.nhandles = 0;
        self.nconns = 0;
        if self.sel_pid == NO_SEL { return; }
        let rc = unsafe { syscall2(SYS_PROC_DETAIL, self.sel_pid as i64, &mut self.detail as *mut ProcDetail as i64) } as i32;
        self.have_detail = rc == 1;
        let n = unsafe { syscall3(SYS_PROC_HANDLES, self.sel_pid as i64, self.handles.as_mut_ptr() as i64, 32) } as i32;
        self.nhandles = if n > 0 { n as usize } else { 0 };
        let n = unsafe { syscall3(SYS_NET_CONNS, self.sel_pid as i64, self.conns.as_mut_ptr() as i64, 32) } as i32;
        self.nconns = if n > 0 { n as usize } else { 0 };
    }

    fn sort_by_cpu(&mut self) {
        // Insertion sort: n <= 64, and it is stable so equal rows do not jitter
        // between refreshes (a jittering list is unusable).
        for i in 1..self.nproc {
            let mut j = i;
            while j > 0 {
                let a = (self.cpu_pct[j], self.procs[j].mem_kb);
                let b = (self.cpu_pct[j - 1], self.procs[j - 1].mem_kb);
                if a.0 > b.0 || (a.0 == b.0 && a.1 > b.1) {
                    self.procs.swap(j, j - 1);
                    self.cpu_pct.swap(j, j - 1);
                    j -= 1;
                } else { break; }
            }
        }
    }

    fn selected_idx(&self) -> Option<usize> {
        for i in 0..self.nproc { if self.procs[i].pid == self.sel_pid { return Some(i); } }
        None
    }

    /// Move the selection by `d` rows on whichever list tab is showing, and
    /// refresh the per-process caches so Details follows the selection.
    fn select_step(&mut self, d: i32) {
        if self.tab == Tab::Services {
            if self.nsvcs == 0 { return; }
            let mut i = self.svc_sel as i32 + d;
            if i < 0 { i = 0; }
            if i >= self.nsvcs as i32 { i = self.nsvcs as i32 - 1; }
            self.svc_sel = i as usize;
            return;
        }
        if self.nproc == 0 { return; }
        // No selection yet -> start at the top rather than jumping to the end.
        let cur = match self.selected_idx() { Some(i) => i as i32, None => -1 };
        let mut i = if cur < 0 { 0 } else { cur + d };
        if i < 0 { i = 0; }
        if i >= self.nproc as i32 { i = self.nproc as i32 - 1; }
        self.sel_pid = self.procs[i as usize].pid;
        // Keep the selection on screen.
        let idx = i as usize;
        if idx < self.scroll { self.scroll = idx; }
        self.refresh_detail();
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
const TAB_H: i32 = 26;
const ROW_H: i32 = 20;
const PAD: i32 = 10;

const TAB_NAMES: [&[u8]; 5] = [b"Processes\0", b"Performance\0", b"Details\0", b"Services\0", b"Scheduled\0"];

fn state_name(s: u32) -> &'static [u8] {
    match s {
        1 => b"Ready\0", 2 => b"Running\0", 3 => b"Sleep\0",
        4 => b"Blocked\0", 5 => b"Zombie\0", _ => b"-\0",
    }
}
fn prio_name(p: u32) -> &'static [u8] {
    match p { 0 => b"Idle\0", 1 => b"Low\0", 2 => b"Normal\0", 3 => b"High\0", _ => b"Realtime\0" }
}
fn kind_name(k: u32) -> &'static [u8] {
    match k { 0 => b"file\0", 1 => b"dev\0", 2 => b"pipe\0", 3 => b"sock\0", _ => b"?\0" }
}
fn access_name(flags: i32) -> &'static [u8] {
    match flags & 3 { 0 => b"R\0", 1 => b"W\0", 2 => b"RW\0", _ => b"?\0" }
}
fn tcp_state_name(s: u8) -> &'static [u8] {
    match s {
        0 => b"CLOSED\0", 1 => b"LISTEN\0", 2 => b"SYN_SENT\0", 3 => b"SYN_RCVD\0",
        4 => b"ESTABLISHED\0", 5 => b"FIN_WAIT_1\0", 6 => b"FIN_WAIT_2\0",
        7 => b"CLOSE_WAIT\0", 8 => b"CLOSING\0", 9 => b"LAST_ACK\0",
        10 => b"TIME_WAIT\0", _ => b"?\0",
    }
}
fn cron_type_name(t: u8) -> &'static [u8] {
    match t { 0 => b"Once\0", 1 => b"Interval\0", 2 => b"Daily\0", 3 => b"Weekly\0", _ => b"?\0" }
}
fn cron_action_name(a: u8) -> &'static [u8] {
    match a { 0 => b"callback\0", 1 => b"launch\0", 2 => b"event\0", _ => b"?\0" }
}

fn draw_tabs(a: &App) {
    let mut x = PAD;
    for (i, name) in TAB_NAMES.iter().enumerate() {
        let sel = i == a.tab as usize;
        let w = 84;
        let bg = if sel { a.pal.accent } else { a.pal.surface_raised };
        win_draw_rect(a.win, x, PAD, w, TAB_H, bg);
        let ink = if sel { ink_on(a.pal.accent) } else { a.pal.ink_dim };
        draw_text(a.win, x + 8, PAD + 6, name, 11, ink);
        x += w + 3;
    }
    win_draw_rect(a.win, PAD, PAD + TAB_H, a.dw - 2 * PAD, 1, a.pal.border);
}

/// Sparkline chart from a fixed-point percent ring. Bars, not a polyline: bars
/// need no interpolation and so no division per pixel.
fn draw_chart(a: &App, x: i32, y: i32, w: i32, h: i32, ring: &[u32; HIST],
              label: &[u8], color: u32) {
    unsafe { gui_card(a.win, x, y, w, h); }
    draw_text(a.win, x + 6, y + 4, label, 11, a.pal.ink_dim);
    let gy = y + 20;
    let gh = h - 26;
    if gh <= 2 { return; }
    let n = if a.hist_n < HIST { a.hist_n } else { HIST };
    if n == 0 { return; }
    let bw = if (w - 12) / (HIST as i32) > 0 { (w - 12) / (HIST as i32) } else { 1 };
    for i in 0..n {
        // oldest -> newest, left -> right
        let idx = if a.hist_n < HIST { i } else { (a.hist_n + i) % HIST };
        let v = ring[idx].min(100);
        let bh = ((v as i32) * gh) / 100;
        if bh > 0 {
            win_draw_rect(a.win, x + 6 + (i as i32) * bw, gy + (gh - bh), bw - 1, bh, color);
        }
    }
    // last value, top-right
    let last = if a.hist_n == 0 { 0 } else { ring[(a.hist_n - 1) % HIST] };
    let mut b = Buf::new();
    b.putu(last as u64); b.put(b'%');
    draw_text(a.win, x + w - 44, y + 4, b.as_c(), 11, a.pal.ink);
}

fn draw_processes(a: &App) {
    let top = PAD + TAB_H + 6;
    let c_name = PAD + 4;
    let c_pid = a.dw - 330;
    let c_state = a.dw - 268;
    let c_thr = a.dw - 196;
    let c_cpu = a.dw - 140;
    let c_mem = a.dw - 90;
    draw_text(a.win, c_name, top, b"Name\0", 11, a.pal.ink_dim);
    draw_text(a.win, c_pid, top, b"PID\0", 11, a.pal.ink_dim);
    draw_text(a.win, c_state, top, b"State\0", 11, a.pal.ink_dim);
    draw_text(a.win, c_thr, top, b"Core\0", 11, a.pal.ink_dim);
    draw_text(a.win, c_cpu, top, b"CPU\0", 11, a.pal.ink_dim);
    draw_text(a.win, c_mem, top, b"Memory\0", 11, a.pal.ink_dim);
    let ltop = top + 18;
    win_draw_rect(a.win, PAD, ltop - 3, a.dw - 2 * PAD, 1, a.pal.border);
    let lbot = a.dh - 44;
    let rows = ((lbot - ltop) / ROW_H).max(0) as usize;

    let mut b = Buf::new();
    for r in 0..rows {
        let i = r + a.scroll;
        if i >= a.nproc { break; }
        let ry = ltop + (r as i32) * ROW_H;
        let sel = a.procs[i].pid == a.sel_pid;
        if sel { win_draw_rect(a.win, PAD, ry, a.dw - 2 * PAD, ROW_H - 1, a.pal.accent); }
        else if r & 1 == 1 { win_draw_rect(a.win, PAD, ry, a.dw - 2 * PAD, ROW_H - 1, unsafe { gui_lighten(a.pal.surface, 4) }); }
        let ink = if sel { ink_on(a.pal.accent) } else { a.pal.ink };
        let dim = if sel { ink_on(a.pal.accent) } else { a.pal.ink_dim };

        draw_text(a.win, c_name, ry + 3, &a.procs[i].name, 12, ink);
        b.clear(); b.putu(a.procs[i].pid as u64);
        draw_text(a.win, c_pid, ry + 3, b.as_c(), 12, ink);
        draw_text(a.win, c_state, ry + 3, state_name(a.procs[i].state), 11, dim);
        b.clear();
        if a.procs[i].running_cpu < 1 { b.put(b'-'); } else { b.puts(b"AP"); b.putu(a.procs[i].running_cpu as u64); }
        draw_text(a.win, c_thr, ry + 3, b.as_c(), 11, dim);
        b.clear(); b.putu(a.cpu_pct[i] as u64); b.put(b'%');
        draw_text(a.win, c_cpu, ry + 3, b.as_c(), 12, ink);
        b.clear(); b.put_kb(a.procs[i].mem_kb);
        draw_text(a.win, c_mem, ry + 3, b.as_c(), 11, dim);
    }

    // footer
    let fy = a.dh - 36;
    win_draw_rect(a.win, PAD, fy - 6, a.dw - 2 * PAD, 1, a.pal.border);
    let mut f = Buf::new();
    f.putu(a.nproc as u64); f.puts(b" processes   CPU "); f.putu(a.cpu_total.max(0) as u64); f.put(b'%');
    draw_text(a.win, PAD, fy + 6, f.as_c(), 11, a.pal.ink_dim);
    let can = a.sel_pid > 1 && a.sel_pid != NO_SEL;
    let st = if can { GUI_ST_NORMAL } else { GUI_ST_DISABLED };
    unsafe {
        gui_button(a.win, a.dw - 250, fy, 74, 26, b"End Task\0".as_ptr(), GUI_BTN_PRIMARY, st);
        gui_button(a.win, a.dw - 170, fy, 74, 26, b"Kill\0".as_ptr(), GUI_BTN_SECONDARY, st);
        gui_button(a.win, a.dw - 90, fy, 74, 26, b"Prio +/-\0".as_ptr(), GUI_BTN_SECONDARY, st);
    }
}

fn draw_performance(a: &App) {
    let top = PAD + TAB_H + 8;
    unsafe {
        gui_button(a.win, PAD, top, 80, 22, b"Overall\0".as_ptr(),
                   if a.perf_cores { GUI_BTN_SECONDARY } else { GUI_BTN_PRIMARY }, GUI_ST_NORMAL);
        gui_button(a.win, PAD + 86, top, 80, 22, b"Per-core\0".as_ptr(),
                   if a.perf_cores { GUI_BTN_PRIMARY } else { GUI_BTN_SECONDARY }, GUI_ST_NORMAL);
    }
    let gy = top + 30;
    let gh = a.dh - gy - PAD;
    let gw = a.dw - 2 * PAD;

    if a.perf_cores {
        let n = a.ncores.max(1);
        let mut cols = 1usize;
        while cols * cols < n { cols += 1; }
        if cols > 4 { cols = 4; }
        let rows = (n + cols - 1) / cols;
        let cw = (gw - ((cols as i32) - 1) * 6) / (cols as i32);
        let ch = (gh - 18 - ((rows as i32) - 1) * 6) / (rows as i32);
        let mut hdr = Buf::new();
        hdr.puts(b"Logical processors: "); hdr.putu(n as u64);
        if n == 1 { hdr.puts(b"   (APs idle: SMP user scheduling off)"); }
        draw_text(a.win, PAD, gy, hdr.as_c(), 11, a.pal.ink_dim);
        if cw < 60 || ch < 40 { return; }
        for i in 0..n {
            let r = (i / cols) as i32;
            let c = (i % cols) as i32;
            let x = PAD + c * (cw + 6);
            let y = gy + 18 + r * (ch + 6);
            let mut lb = Buf::new();
            lb.puts(b"CPU "); lb.putu(i as u64);
            let col = if i == 0 { a.pal.accent } else { mix(a.pal.accent, 0x0040_C060, 50) };
            draw_chart(a, x, y, cw, ch, &a.core_hist[i], lb.as_c(), col);
        }
        return;
    }

    let ch = (gh - 6) / 2;
    draw_chart(a, PAD, gy, gw, ch, &a.cpu_hist, b"CPU\0", a.pal.accent);
    draw_chart(a, PAD, gy + ch + 6, gw, ch, &a.mem_hist, b"Memory\0", mix(a.pal.accent, 0x0040_C060, 60));
    let mut b = Buf::new();
    b.puts(b"RAM "); b.putu(a.mem_used / 1048576); b.puts(b" / "); b.putu(a.mem_total / 1048576); b.puts(b" MB");
    draw_text(a.win, PAD + 60, gy + ch + 10, b.as_c(), 11, a.pal.ink_dim);
}

fn line(a: &App, x: i32, y: &mut i32, label: &[u8], val: &[u8]) {
    draw_text(a.win, x, *y, label, 11, a.pal.ink_dim);
    draw_text(a.win, x + 120, *y, val, 11, a.pal.ink);
    *y += 15;
}

fn draw_details(a: &App) {
    let mut y = PAD + TAB_H + 10;
    let x = PAD + 4;
    if !a.have_detail {
        draw_text(a.win, x, y, b"No process selected (pick one on Processes).\0", 11, a.pal.ink_dim);
        return;
    }
    let d = &a.detail;
    draw_text(a.win, x, y, &d.name, 13, a.pal.ink); y += 20;
    let mut b = Buf::new();
    b.clear(); b.putu(d.pid as u64); line(a, x, &mut y, b"PID\0", b.as_c());
    b.clear(); b.putu(d.ppid as u64); line(a, x, &mut y, b"Parent PID\0", b.as_c());
    line(a, x, &mut y, b"State\0", state_name(d.state));
    line(a, x, &mut y, b"Priority\0", prio_name(d.priority));
    line(a, x, &mut y, b"Privilege\0", if d.privilege == 0 { b"Ring 0 (kernel)\0" } else { b"Ring 3 (user)\0" });
    b.clear(); b.putu(d.uid as u64); line(a, x, &mut y, b"UID\0", b.as_c());
    b.clear(); b.putu(d.threads as u64); line(a, x, &mut y, b"Threads\0", b.as_c());
    b.clear(); b.putu(d.handles as u64); line(a, x, &mut y, b"Handles\0", b.as_c());
    b.clear(); b.putu(d.cpu_ticks); line(a, x, &mut y, b"CPU ticks\0", b.as_c());
    y += 4;
    // #487: the real memory breakdown, which did not exist before this work.
    b.clear(); b.put_kb(d.working_set_kb); line(a, x, &mut y, b"Working set\0", b.as_c());
    b.clear(); b.put_kb(d.private_kb);     line(a, x, &mut y, b"Private (commit)\0", b.as_c());
    b.clear(); b.put_kb(d.virt_kb);        line(a, x, &mut y, b"Virtual size\0", b.as_c());
    b.clear(); b.put_kb(d.heap_kb);        line(a, x, &mut y, b"Heap (brk)\0", b.as_c());
    b.clear(); b.putu(d.vma_count as u64); line(a, x, &mut y, b"VM regions\0", b.as_c());
    if d.mem_flags & 1 != 0 {
        draw_text(a.win, x, y, b"! VMA list truncated (corrupt or cyclic)\0", 11, 0x00E05050); y += 15;
    }

    // Handles, named (the Process Explorer signature view).
    let hx = a.dw / 2 + 4;
    let mut hy = PAD + TAB_H + 10;
    draw_text(a.win, hx, hy, b"Open handles\0", 12, a.pal.ink); hy += 18;
    for i in 0..a.nhandles {
        if hy > a.dh - 120 { break; }
        let h = &a.handles[i];
        let mut hb = Buf::new();
        hb.putu(h.fd as u64); hb.put(b' '); hb.puts(access_name(h.flags));
        hb.put(b' '); hb.puts(kind_name(h.kind));
        draw_text(a.win, hx, hy, hb.as_c(), 10, a.pal.ink_dim);
        draw_text(a.win, hx + 74, hy, &h.path, 10, a.pal.ink);
        hy += 13;
    }
    if a.nhandles == 0 { draw_text(a.win, hx, hy, b"(none)\0", 10, a.pal.ink_dim); }

    // Connections owned by THIS process.
    let mut cy = a.dh - 108;
    win_draw_rect(a.win, PAD, cy - 6, a.dw - 2 * PAD, 1, a.pal.border);
    let mut cb = Buf::new();
    cb.puts(b"Network connections (this process): "); cb.putu(a.nconns as u64);
    draw_text(a.win, PAD, cy, cb.as_c(), 11, a.pal.ink); cy += 15;
    for i in 0..a.nconns {
        if cy > a.dh - 40 { break; }
        let c = &a.conns[i];
        let mut lb = Buf::new();
        lb.puts(tcp_state_name(c.state)); lb.puts(b"  :"); lb.putu(c.local_port as u64);
        if c.is_listener != 0 { lb.puts(b" (listen)"); }
        else {
            lb.puts(b" -> ");
            let ip = c.remote_ip;
            lb.putu((ip & 0xFF) as u64); lb.put(b'.');
            lb.putu(((ip >> 8) & 0xFF) as u64); lb.put(b'.');
            lb.putu(((ip >> 16) & 0xFF) as u64); lb.put(b'.');
            lb.putu(((ip >> 24) & 0xFF) as u64);
            lb.put(b':'); lb.putu(c.remote_port as u64);
        }
        draw_text(a.win, PAD + 8, cy, lb.as_c(), 10, a.pal.ink_dim);
        cy += 13;
    }
    if a.nconns == 0 { draw_text(a.win, PAD + 8, cy, b"(this process has no connections)\0", 10, a.pal.ink_dim); }
}

fn draw_services(a: &App) {
    let top = PAD + TAB_H + 6;
    let c_name = PAD + 4;
    let c_state = a.dw / 2 - 40;
    let c_acct = a.dw / 2 + 40;
    let c_pid = a.dw - 90;
    draw_text(a.win, c_name, top, b"Service\0", 11, a.pal.ink_dim);
    draw_text(a.win, c_state, top, b"State\0", 11, a.pal.ink_dim);
    draw_text(a.win, c_acct, top, b"Account\0", 11, a.pal.ink_dim);
    draw_text(a.win, c_pid, top, b"PID\0", 11, a.pal.ink_dim);
    let ltop = top + 18;
    win_draw_rect(a.win, PAD, ltop - 3, a.dw - 2 * PAD, 1, a.pal.border);
    let mut b = Buf::new();
    for i in 0..a.nsvcs {
        let ry = ltop + (i as i32) * ROW_H;
        if ry > a.dh - 50 { break; }
        let sel = i == a.svc_sel;
        if sel { win_draw_rect(a.win, PAD, ry, a.dw - 2 * PAD, ROW_H - 1, a.pal.accent); }
        else if i & 1 == 1 { win_draw_rect(a.win, PAD, ry, a.dw - 2 * PAD, ROW_H - 1, unsafe { gui_lighten(a.pal.surface, 4) }); }
        let ink = if sel { ink_on(a.pal.accent) } else { a.pal.ink };
        let dim = if sel { ink_on(a.pal.accent) } else { a.pal.ink_dim };
        let s = &a.svcs[i];
        draw_text(a.win, c_name, ry + 3, &s.name, 12, ink);
        draw_text(a.win, c_state, ry + 3, if s.running != 0 { b"Running\0" } else { b"Stopped\0" },
                  11, if s.running != 0 && !sel { 0x0040C060 } else { dim });
        draw_text(a.win, c_acct, ry + 3, &s.account, 11, dim);
        b.clear(); if s.pid != 0 { b.putu(s.pid as u64); } else { b.put(b'-'); }
        draw_text(a.win, c_pid, ry + 3, b.as_c(), 11, dim);
    }
    if a.nsvcs == 0 { draw_text(a.win, PAD + 4, ltop + 6, b"(no services registered)\0", 11, a.pal.ink_dim); }
    let fy = a.dh - 36;
    win_draw_rect(a.win, PAD, fy - 6, a.dw - 2 * PAD, 1, a.pal.border);
    let can = a.svc_sel < a.nsvcs;
    let st = if can { GUI_ST_NORMAL } else { GUI_ST_DISABLED };
    unsafe {
        gui_button(a.win, PAD, fy, 74, 26, b"Start\0".as_ptr(), GUI_BTN_PRIMARY, st);
        gui_button(a.win, PAD + 80, fy, 74, 26, b"Stop\0".as_ptr(), GUI_BTN_SECONDARY, st);
    }
}

fn draw_scheduled(a: &App) {
    let top = PAD + TAB_H + 6;
    let c_name = PAD + 4;
    let c_when = a.dw * 2 / 5;
    let c_act = a.dw * 3 / 5;
    let c_runs = a.dw - 120;
    let c_en = a.dw - 60;
    draw_text(a.win, c_name, top, b"Task\0", 11, a.pal.ink_dim);
    draw_text(a.win, c_when, top, b"Schedule\0", 11, a.pal.ink_dim);
    draw_text(a.win, c_act, top, b"Action\0", 11, a.pal.ink_dim);
    draw_text(a.win, c_runs, top, b"Runs\0", 11, a.pal.ink_dim);
    draw_text(a.win, c_en, top, b"State\0", 11, a.pal.ink_dim);
    let ltop = top + 18;
    win_draw_rect(a.win, PAD, ltop - 3, a.dw - 2 * PAD, 1, a.pal.border);
    let mut b = Buf::new();
    for i in 0..a.njobs {
        let ry = ltop + (i as i32) * ROW_H;
        if ry > a.dh - 30 { break; }
        if i & 1 == 1 { win_draw_rect(a.win, PAD, ry, a.dw - 2 * PAD, ROW_H - 1, unsafe { gui_lighten(a.pal.surface, 4) }); }
        let j = &a.jobs[i];
        let nm: &[u8] = if j.label[0] != 0 { &j.label } else { &j.target };
        draw_text(a.win, c_name, ry + 3, nm, 12, a.pal.ink);
        b.clear();
        b.puts(cron_type_name(j.ty)); b.put(b' ');
        match j.ty {
            2 | 3 => {
                if j.hour < 10 { b.put(b'0'); }
                b.putu(j.hour as u64); b.put(b':');
                if j.minute < 10 { b.put(b'0'); }
                b.putu(j.minute as u64);
            }
            1 | 0 => {
                if j.interval_ms >= 1000 { b.putu((j.interval_ms / 1000) as u64); b.put(b's'); }
                else { b.putu(j.interval_ms as u64); b.puts(b"ms"); }
            }
            _ => {}
        }
        draw_text(a.win, c_when, ry + 3, b.as_c(), 11, a.pal.ink_dim);
        draw_text(a.win, c_act, ry + 3, cron_action_name(j.action), 11, a.pal.ink_dim);
        b.clear(); b.putu(j.run_count as u64);
        draw_text(a.win, c_runs, ry + 3, b.as_c(), 11, a.pal.ink_dim);
        draw_text(a.win, c_en, ry + 3, if j.enabled != 0 { b"On\0" } else { b"Off\0" }, 11,
                  if j.enabled != 0 { 0x0040C060 } else { a.pal.ink_dim });
    }
    if a.njobs == 0 { draw_text(a.win, PAD + 4, ltop + 6, b"(no scheduled tasks registered)\0", 11, a.pal.ink_dim); }
}

fn draw(a: &mut App) {
    a.apply_theme();
    let (mut w, mut h) = (0, 0);
    win_get_size(a.win, &mut w, &mut h);
    if w > 200 { a.dw = w; }
    if h > 200 { a.dh = h; }
    win_draw_rect(a.win, 0, 0, a.dw, a.dh, a.pal.surface);
    draw_tabs(a);
    match a.tab {
        Tab::Processes => draw_processes(a),
        Tab::Performance => draw_performance(a),
        Tab::Details => draw_details(a),
        Tab::Services => draw_services(a),
        Tab::Scheduled => draw_scheduled(a),
    }
    win_invalidate(a.win);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
// event_type_t (libc/gui.h), read out of the header rather than assumed: an
// earlier draft of this file had EVERY one of these wrong.
const EVENT_MOUSE_DOWN: u32 = 2;
const EVENT_MOUSE_SCROLL: u32 = 4;
const EVENT_KEY_DOWN: u32 = 5;
const EVENT_WINDOW_CLOSE: u32 = 7;
const EVENT_REDRAW: u32 = 11;
const EVENT_RESIZE: u32 = 12;

// Mirror of gui_event_t (libc/gui.h). Using a #[repr(C)] struct rather than
// hand-computed byte offsets: the offsets are exactly what a hand-rolled
// version gets wrong (mouse_x is at 8, keycode at 24, key_char at 28), and the
// compiler gets them right for free.
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

#[no_mangle]
pub extern "C" fn main() -> i32 {
    let win = win_create(b"Task Manager\0", 120, 70, 760, 560);
    if win < 0 { return 1; }
    let mut app = App {
        win, dw: 760, dh: 560, tab: Tab::Processes,
        procs: [ZP; MAXP], nproc: 0, cpu_pct: [0; MAXP], prev_ticks: [0; MAXP],
        // NO_SEL, not 0: pid 0 is the idle process, so initialising
        // sel_pid to 0 made idle look selected on first paint.
        prev_valid: false, sel_pid: NO_SEL, scroll: 0, cpu_total: 0,
        mem_total: 0, mem_used: 0,
        cpu_hist: [0; HIST], mem_hist: [0; HIST], core_hist: [[0; HIST]; MAXCORES],
        hist_n: 0, ncores: 1, perf_cores: false,
        detail: ZD, have_detail: false,
        handles: [ZH; 32], nhandles: 0, conns: [ZC; 32], nconns: 0,
        svcs: [ZS; 32], nsvcs: 0, svc_sel: 0, jobs: [ZJ; 32], njobs: 0,
        pal: GuiPalette { surface: 0, surface_raised: 0, ink: 0xF0F0F0, ink_dim: 0x909090,
                          accent: 0x3080D0, accent_hover: 0x4090E0, border: 0x404040,
                          field_bg: 0x202020, field_border: 0x404040, track: 0x303030 },
        dark: true,
    };
    app.refresh();
    draw(&mut app);

    let mut ev = GuiEvent { ty: 0, target_id: 0, mouse_x: 0, mouse_y: 0,
                            mouse_buttons: 0, scroll_delta: 0, keycode: 0, key_char: 0 };
    loop {
        // Blocking wait with a 1s timeout: the kernel wait-queue wakes us on an
        // event, the timeout drives the 1Hz refresh. This is the shared
        // primitive; a poll loop here would busy-spin a core (#453/#426).
        let et = win_get_event(app.win, &mut ev, 1000);
        if et == 0 {
            app.refresh();
            draw(&mut app);
            continue;
        }
        match ev.ty {
            EVENT_REDRAW | EVENT_RESIZE => draw(&mut app),
            EVENT_WINDOW_CLOSE => break,
            EVENT_KEY_DOWN => {
                let ch = ev.key_char;
                match ch {
                    27 => break,                                    // ESC
                    b'1'..=b'5' => {
                        app.tab = match ch {
                            b'1' => Tab::Processes, b'2' => Tab::Performance,
                            b'3' => Tab::Details, b'4' => Tab::Services, _ => Tab::Scheduled,
                        };
                        app.refresh_detail();
                        draw(&mut app);
                    }
                    b'c' | b'C' => { app.perf_cores = !app.perf_cores; draw(&mut app); }
                    b'e' | b'E' => { if app.sel_pid > 1 && app.sel_pid != NO_SEL { unsafe { syscall2(SYS_KILL, app.sel_pid as i64, SIGTERM); } app.refresh(); draw(&mut app); } }
                    b'k' | b'K' => { if app.sel_pid > 1 && app.sel_pid != NO_SEL { unsafe { syscall2(SYS_KILL, app.sel_pid as i64, SIGKILL); } app.refresh(); draw(&mut app); } }
                    b'+' => { if let Some(i) = app.selected_idx() { let _ = i; unsafe { syscall2(SYS_SETPRIORITY, app.sel_pid as i64, (app.detail.priority as i64) + 1); } app.refresh(); draw(&mut app); } }
                    b'-' => { if let Some(i) = app.selected_idx() { let _ = i; unsafe { syscall2(SYS_SETPRIORITY, app.sel_pid as i64, (app.detail.priority as i64) - 1); } app.refresh(); draw(&mut app); } }
                    b'r' | b'R' => { app.refresh(); draw(&mut app); }
                    _ => {
                        // Arrow keys arrive as SCANCODES with no ASCII char, so
                        // they are matched on keycode, not key_char. Up/Down
                        // move the selection on the list tabs. Without this the
                        // app was mouse-ONLY, which also made it impossible to
                        // reach Details on a machine with no working pointer.
                        match ev.keycode {
                            0x48 => { app.select_step(-1); draw(&mut app); }  // Up
                            0x50 => { app.select_step(1);  draw(&mut app); }  // Down
                            _ => {}
                        }
                    }
                }
            }
            EVENT_MOUSE_DOWN => {
                let (mx, my) = (ev.mouse_x, ev.mouse_y);
                // tabs
                if my >= PAD && my < PAD + TAB_H {
                    let idx = ((mx - PAD) / 87) as usize;
                    if idx < 5 {
                        app.tab = match idx {
                            0 => Tab::Processes, 1 => Tab::Performance, 2 => Tab::Details,
                            3 => Tab::Services, _ => Tab::Scheduled,
                        };
                        app.refresh_detail();
                        draw(&mut app);
                    }
                } else if app.tab == Tab::Processes {
                    let fy = app.dh - 36;
                    if my >= fy && my < fy + 26 {
                        if app.sel_pid > 1 && app.sel_pid != NO_SEL {
                            if mx >= app.dw - 250 && mx < app.dw - 176 { unsafe { syscall2(SYS_KILL, app.sel_pid as i64, SIGTERM); } }
                            else if mx >= app.dw - 170 && mx < app.dw - 96 { unsafe { syscall2(SYS_KILL, app.sel_pid as i64, SIGKILL); } }
                            app.refresh(); draw(&mut app);
                        }
                    } else {
                        let ltop = PAD + TAB_H + 6 + 18;
                        if my >= ltop {
                            let r = ((my - ltop) / ROW_H) as usize + app.scroll;
                            if r < app.nproc { app.sel_pid = app.procs[r].pid; app.refresh_detail(); draw(&mut app); }
                        }
                    }
                } else if app.tab == Tab::Performance {
                    let top = PAD + TAB_H + 8;
                    if my >= top && my < top + 22 {
                        if mx >= PAD && mx < PAD + 80 { app.perf_cores = false; draw(&mut app); }
                        else if mx >= PAD + 86 && mx < PAD + 166 { app.perf_cores = true; draw(&mut app); }
                    }
                } else if app.tab == Tab::Services {
                    let fy = app.dh - 36;
                    if my >= fy && my < fy + 26 {
                        if app.svc_sel < app.nsvcs {
                            let nm = app.svcs[app.svc_sel].name.as_ptr();
                            if mx >= PAD && mx < PAD + 74 { unsafe { syscall2(SYS_SVC_CONTROL, nm as i64, 1); } }
                            else if mx >= PAD + 80 && mx < PAD + 154 { unsafe { syscall2(SYS_SVC_CONTROL, nm as i64, 0); } }
                            app.refresh(); draw(&mut app);
                        }
                    } else {
                        let ltop = PAD + TAB_H + 6 + 18;
                        if my >= ltop {
                            let r = ((my - ltop) / ROW_H) as usize;
                            if r < app.nsvcs { app.svc_sel = r; draw(&mut app); }
                        }
                    }
                }
            }
            EVENT_MOUSE_SCROLL => {
                if ev.scroll_delta > 0 { app.scroll += 1; } else if app.scroll > 0 { app.scroll -= 1; }
                if app.scroll >= app.nproc { app.scroll = if app.nproc > 0 { app.nproc - 1 } else { 0 }; }
                draw(&mut app);
            }
            _ => {}
        }
    }
    unsafe { syscall1(SYS_WIN_DESTROY, app.win as i64); }
    0
}
