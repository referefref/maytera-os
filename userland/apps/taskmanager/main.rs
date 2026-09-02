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

// #188: every coordinate that BOTH the drawing code and the hit-test code need,
// plus the ordering rules, live in one syscall-free file so the two can never
// disagree. They HAD disagreed: the "Prio +/-" button was drawn at dw-90..dw-16
// and hit-tested nowhere, and the column headers were drawn and hit-tested
// nowhere at all. `make logic-test` compiles this same text for the host and
// asserts against a transcription of the old, broken hit-test.
include!("logic.rs");

// #191: THE keycode table (libc/keys.rs), so the values #188 had to correct by
// hand cannot be restated wrongly here again.
#[path = "../../libc/keys.rs"]
mod keys;

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

    // #178: the ONE ranking of "what is eating the CPU" (libc/proccpu.h).
    // This is the SAME OBJECT FILE that /APPS/top and /APPS/SYSMON call, not a
    // Rust re-port of it: a re-port would be a sixth copy and would drift like
    // the other five did. Both arms of the #145 fix (idle in the denominator
    // but out of the list; baselines matched by pid, never indexed) now have
    // exactly one implementation, in C, reached from here through the libc.a
    // this app already links.
    //
    // Every argument is a primitive or a #[repr(C)] struct whose size is
    // asserted below and _Static_assert-locked on the C side. proccpu_rank
    // compacts procs[] and pct[] in place and returns the non-idle row count.
    fn proccpu_rank(st: *mut ProcCpu, procs: *mut ProcInfo, nproc: i32, pct: *mut u32) -> i32;
    fn proccpu_sort(procs: *mut ProcInfo, pct: *mut u32, nproc: i32);

    // #745 (docs/CONFIRM_MODAL_DESIGN.html): the shared confirm/notice card,
    // FFI-safe singleton form (libc/gui_style.h's own comment explains why:
    // every arg here is a primitive, so nothing needs to mirror gui_confirm_t's
    // C struct layout - enum width, char-array padding, the alignment gap gcc
    // inserts before its trailing u64 - across this boundary by hand, which is
    // exactly the class of bug already recorded for this app's Rust port).
    fn gui_confirm_open_s(variant: i32, title: *const u8,
                          line0: *const u8, line1: *const u8, line2: *const u8, n_lines: i32,
                          cancel_label: *const u8, action_label: *const u8);
    fn gui_confirm_singleton_is_open() -> i32;
    fn gui_confirm_singleton_render(handle: i32, win_w: i32, win_h: i32);
    fn gui_confirm_singleton_handle_key(key: i32) -> i32;
    fn gui_confirm_singleton_handle_mouse(x: i32, y: i32, clicked: i32) -> i32;
}

const GUI_CONFIRM_DESTRUCTIVE: i32 = 0;

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
// #188, both re-read out of kernel/proc/syscall.h:131-132 rather than trusted
// from a brief. NEITHER TAKES AN ARGUMENT and both return MEGABYTES, and both
// report `g_fat_fs` (kernel/proc/syscall.c:8474-8491), which on the shipping
// two-partition image is the FAT ESP, i.e. the ~256 MB BOOT VOLUME - it is NOT
// the ext2 root where /APPS and user data live, and there is no syscall in the
// tree that reports ext2 free space. The Performance tab says "boot ESP" on
// the card for exactly that reason: a gauge labelled "Disk" that silently
// measures a different volume is a fabricated number with a true value in it.
const SYS_GET_DISK_TOTAL: i64 = 138;
const SYS_GET_DISK_FREE: i64 = 139;
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
// #188. userland/libc/syscall.h:1763 and kernel/proc/syscall.h:452. Fills a
// net_status_t and NEVER BLOCKS, which is why it is safe on the 1 Hz refresh.
const SYS_NET_STATUS: i64 = 371;

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
    /// #145 PROC_INFO_F_*. Lives in the four bytes of tail padding this struct
    /// already had, so size_of is still 64 and the syscall ABI is unchanged.
    flags: u32,
}
const _: () = assert!(core::mem::size_of::<ProcInfo>() == 64);
/// #145: kernel-authoritative "this row is a per-core idle process". Read only
/// by libc/proccpu.c now; kept here because the ABI comment above belongs with
/// the struct, and because a NAME comparison against "idle" must never come
/// back (#145).
#[allow(dead_code)]
const PROC_INFO_F_IDLE: u32 = 0x0000_0001;

/// #178: mirror of libc/proccpu.h's `proccpu_t`, the previous snapshot kept BY
/// PID. Opaque to this app on purpose: it holds no CPU accounting state of its
/// own any more, so it has none to get wrong. Layout is locked at both ends
/// (`_Static_assert` in proccpu.c, the const assert below).
#[repr(C)]
#[derive(Clone, Copy)]
struct ProcCpu {
    pid: [u32; MAXP],
    ticks: [u64; MAXP],
    n: i32,
    valid: i32,
}
const _: () = assert!(core::mem::size_of::<ProcCpu>() == 8 * MAXP + 4 * MAXP + 8);

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
/// #188: this mirror had no size lock, alone among the five in this file, and
/// SYS_CRON_ENABLE was about to start acting on `id` read out of it. A wrong
/// offset here would disable SOMEBODY ELSE'S job, so lock it like the rest.
const _: () = assert!(core::mem::size_of::<CronJob>() == 128);

/// Mirror of net_status_t (kernel/proc/syscall.h:472-489, userland/libc/
/// syscall.h:1766-1782, and NetStatus in kernel/rustkern/netstat.rs - all three
/// are required to stay in step and the C side _Static_asserts 48).
///
/// READ THIS BEFORE CHARTING ANYTHING FROM IT: there is NO byte counter, NO
/// packet counter and no timestamp in this struct, and a tree-wide search found
/// no other syscall that exports one. Network THROUGHPUT therefore cannot be
/// sampled by a userland app at all right now, so the Performance tab shows
/// network STATE and says so on the card. Inventing a rate from these fields
/// would be a fabricated number, which is the specific thing this project keeps
/// having to delete.
///
/// ALL ADDRESS FIELDS ARE HOST BYTE ORDER: (a<<24)|(b<<16)|(c<<8)|d.
#[repr(C)]
#[derive(Clone, Copy)]
struct NetStatus {
    ip: u32,
    netmask: u32,
    gateway: u32,
    dns_active: u32,
    dns_dhcp: u32,
    dhcp_ip: u32,
    link_up: u32,
    dhcp_state: u32,
    config_static: u32,
    faulty: u32,
    driver: u32,
    prefix_len: u32,
}
const _: () = assert!(core::mem::size_of::<NetStatus>() == 48);

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
/// #178: must equal PROCCPU_MAX in libc/proccpu.h, which is the kernel's
/// MAX_PROCESSES. Four copies of this ranking each picked their own bound
/// (64, 96, 192, 256); a row outside your guess is a row with no baseline.
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
    // #178: the previous snapshot, owned by libc/proccpu.c. Was three fields
    // and a lookup method here, and a near-copy of the same three fields in
    // sysmon, top and this app's rollback twin.
    cpu: ProcCpu,
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
    /// #188: the Scheduled tab had no selection at all, which is why it could
    /// not have an Enable/Disable button. Modelled on svc_sel.
    job_sel: usize,
    /// #188: Processes click-to-sort. The initial state is CPU-descending,
    /// which is the order the app has always opened in, and which for that one
    /// column is still produced by the SHARED proccpu_sort() in libc, not by
    /// the comparator in logic.rs. See sort_apply().
    sort_col: SortCol,
    sort_desc: bool,
    /// #188: Details tab connection scope. false = the selected process (what
    /// it always did), true = PI_PID_ALL, every connection on the machine with
    /// its owning pid. PI_PID_ALL was defined and never used.
    conn_all: bool,
    /// #188: Performance tab, boot-volume space. See SYS_GET_DISK_* above for
    /// exactly which volume this is.
    disk_hist: [u32; HIST],
    disk_total_mb: u64,
    disk_free_mb: u64,
    /// #188: Performance tab, network STATE (not throughput - see NetStatus).
    net: NetStatus,
    have_net: bool,
    /// #188: update speed. Was the bare literal 1000 in the event loop.
    speed: Speed,
    // palette
    pal: GuiPalette,
    dark: bool,
    // #161: the result of the last End Task / Kill, shown in the footer.
    // A button that silently does nothing is this project's worst recurring
    // class, and Kill was one: on a ZOMBIE it sent a SIGKILL that is a no-op by
    // definition (the process has already exited; there is nothing left to
    // signal) and reported nothing at all, so the owner clicked it repeatedly
    // on a zombie AssaultCube with no way to tell that the click had landed,
    // that the target was already dead, or that the button could not possibly
    // work. Every path through signal_selected() now sets this.
    msg: &'static [u8],
    // #745: the signal signal_selected() is WAITING to send once the shared
    // confirm card is answered Yes (0 = no confirm pending). Both the
    // keyboard route ('e'/'k') and the mouse route (End Task/Kill buttons)
    // go through signal_selected(), so there is exactly one place this gets
    // set and exactly one place it gets consumed (the EVENT_KEY_DOWN/
    // EVENT_MOUSE_DOWN gate in main(), before either normal handler runs).
    pending_sig: i64,
}

const ZP: ProcInfo = ProcInfo { pid: 0, ppid: 0, name: [0; 32], state: 0, mem_kb: 0, cpu_ticks: 0, running_cpu: -1, flags: 0 };
const ZH: HandleInfo = HandleInfo { fd: 0, flags: 0, kind: 0, _pad: 0, path: [0; 96] };
const ZC: TcpConnInfo = TcpConnInfo { state: 0, is_listener: 0, local_port: 0, remote_port: 0, remote_ip: 0, recv_len: 0, send_len: 0, owner_pid: 0 };
const ZS: SvcInfo = SvcInfo { running: 0, autostart: 0, perms: 0, pid: 0, name: [0; 32], account: [0; 32] };
const ZD: ProcDetail = ProcDetail { pid: 0, ppid: 0, working_set_kb: 0, private_kb: 0, virt_kb: 0, heap_kb: 0, threads: 0, handles: 0, uid: 0, gid: 0, priority: 0, privilege: 0, state: 0, vma_count: 0, mem_flags: 0, is_service: 0, cpu_ticks: 0, cr3: 0, name: [0; 32] };
const ZJ: CronJob = CronJob { id: 0, ty: 0, action: 0, enabled: 0, weekday: 0, hour: 0, minute: 0, reserved: [0; 2], interval_ms: 0, run_count: 0, next_fire_tick: 0, target: [0; 64], label: [0; 32] };
const ZN: NetStatus = NetStatus { ip: 0, netmask: 0, gateway: 0, dns_active: 0, dns_dhcp: 0,
                                  dhcp_ip: 0, link_up: 0, dhcp_state: 0, config_static: 0,
                                  faulty: 0, driver: 0, prefix_len: 0 };

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

        // #178: CPU% per process, and what this pane is FOR.
        //
        // The arithmetic used to be forty lines right here, and four other
        // programs had their own forty. All three settled decisions - the
        // denominator is every row INCLUDING idle so a share means "share of
        // the machine" and agrees with the footer (#182); idle leaves the LIST
        // because it is capacity nobody asked for and would otherwise top a
        // CPU-sorted list on any machine with headroom; baselines are matched
        // BY PID and never indexed by it - now live in libc/proccpu.c and are
        // documented in libc/proccpu.h. Read that file before changing any of
        // them, and change them THERE, once.
        //
        // proccpu_rank compacts self.procs and self.cpu_pct in place and
        // returns the non-idle row count.
        self.nproc = unsafe {
            proccpu_rank(&mut self.cpu, self.procs.as_mut_ptr(),
                         self.nproc as i32, self.cpu_pct.as_mut_ptr())
        } as usize;

        self.cpu_total = unsafe { syscall1(SYS_GET_CPU_USAGE, 0) } as i32;
        unsafe {
            syscall2(SYS_GET_MEM_INFO, &mut self.mem_total as *mut u64 as i64,
                     &mut self.mem_used as *mut u64 as i64);
        }
        self.sort_apply();

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
        self.refresh_jobs();

        // #188: boot-volume space. Both calls take no argument (the existing
        // no-arg calls in this file are spelled syscall1(n, 0) too) and return
        // MEGABYTES, or a negative on failure - which is rendered as "not
        // reported", never as zero, because a zero would draw a full bar.
        let dt = unsafe { syscall1(SYS_GET_DISK_TOTAL, 0) };
        let df = unsafe { syscall1(SYS_GET_DISK_FREE, 0) };
        self.disk_total_mb = if dt > 0 { dt as u64 } else { 0 };
        self.disk_free_mb = if df > 0 { df as u64 } else { 0 };
        if self.disk_free_mb > self.disk_total_mb { self.disk_free_mb = self.disk_total_mb; }
        self.disk_hist[slot] = if self.disk_total_mb > 0 {
            (((self.disk_total_mb - self.disk_free_mb) * 100) / self.disk_total_mb) as u32
        } else { 0 };

        // #188: network state. Never blocks (kernel comment at syscall.h:451).
        let rc = unsafe { syscall1(SYS_NET_STATUS, &mut self.net as *mut NetStatus as i64) } as i32;
        self.have_net = rc == 0;

        self.refresh_detail();
    }

    /// #188: re-list the cron jobs and keep the selection in range. Called from
    /// refresh() and again right after an Enable/Disable, so the State column
    /// shows the new value in the SAME frame as the click - "did that do
    /// anything?" is the question this whole ticket exists to answer.
    fn refresh_jobs(&mut self) {
        let n = unsafe { syscall2(SYS_CRON_LIST, self.jobs.as_mut_ptr() as i64, 32) } as i32;
        self.njobs = if n > 0 { n as usize } else { 0 };
        if self.njobs == 0 { self.job_sel = 0; }
        else if self.job_sel >= self.njobs { self.job_sel = self.njobs - 1; }
    }

    /// #188: put self.procs / self.cpu_pct in the order the header says.
    ///
    /// CPU-DESCENDING IS NOT SORTED HERE. It is `proccpu_sort()`, the shared
    /// libc ranking that /APPS/top and /APPS/SYSMON also call, exactly as
    /// before this ticket; CPU-ascending is that same shared sort followed by a
    /// reverse. A second CPU ranking in this file would be the sixth copy of
    /// the thing #178 spent a ticket reducing to one, and it would drift.
    fn sort_apply(&mut self) {
        match (self.sort_col, self.sort_desc) {
            (SortCol::Cpu, true) => unsafe {
                proccpu_sort(self.procs.as_mut_ptr(), self.cpu_pct.as_mut_ptr(), self.nproc as i32)
            },
            (SortCol::Cpu, false) => {
                unsafe {
                    proccpu_sort(self.procs.as_mut_ptr(), self.cpu_pct.as_mut_ptr(), self.nproc as i32)
                };
                self.reverse_rows();
            }
            (col, desc) => self.sort_rows(col, desc),
        }
    }

    fn reverse_rows(&mut self) {
        if self.nproc < 2 { return; }
        let mut i = 0usize;
        let mut j = self.nproc - 1;
        while i < j {
            self.procs.swap(i, j);
            self.cpu_pct.swap(i, j);
            i += 1;
            j -= 1;
        }
    }

    /// Apply one permutation to BOTH parallel arrays. They must move together:
    /// cpu_pct[i] belongs to procs[i], and sorting one without the other would
    /// print every process's CPU% against the wrong name.
    fn sort_rows(&mut self, col: SortCol, desc: bool) {
        let n = self.nproc;
        if n < 2 { return; }
        let mut keys = [ROWKEY_ZERO; MAXP];
        for i in 0..n {
            keys[i] = RowKey {
                name: self.procs[i].name,
                pid: self.procs[i].pid,
                state: self.procs[i].state,
                core: self.procs[i].running_cpu,
                cpu: self.cpu_pct[i],
                mem: self.procs[i].mem_kb,
            };
        }
        let mut perm = [0usize; MAXP];
        sort_perm(col, desc, &keys, n, &mut perm);
        let mut tp = [ZP; MAXP];
        let mut tc = [0u32; MAXP];
        for i in 0..n { tp[i] = self.procs[perm[i]]; tc[i] = self.cpu_pct[perm[i]]; }
        for i in 0..n { self.procs[i] = tp[i]; self.cpu_pct[i] = tc[i]; }
    }

    /// #188: adopt a new sort column/direction and re-order immediately, so a
    /// click repaints in the same frame instead of at the next 1 Hz tick (at
    /// Low speed that would be a four-second wait that reads as "nothing
    /// happened", which is the defect class this ticket is about).
    fn sort_click(&mut self, clicked: SortCol) {
        let (c, d) = sort_next(self.sort_col, self.sort_desc, clicked);
        self.sort_col = c;
        self.sort_desc = d;
        self.sort_apply();
    }

    fn refresh_detail(&mut self) {
        self.have_detail = false;
        self.nhandles = 0;
        self.nconns = 0;
        if self.sel_pid != NO_SEL {
            let rc = unsafe { syscall2(SYS_PROC_DETAIL, self.sel_pid as i64, &mut self.detail as *mut ProcDetail as i64) } as i32;
            self.have_detail = rc == 1;
            let n = unsafe { syscall3(SYS_PROC_HANDLES, self.sel_pid as i64, self.handles.as_mut_ptr() as i64, 32) } as i32;
            self.nhandles = if n > 0 { n as usize } else { 0 };
        }
        // #188: connections. PI_PID_ALL was defined at the top of this file and
        // never passed to anything, so the machine-wide view the kernel has
        // supported all along (procinfo.c:138 copies the whole table straight
        // through) was unreachable from the UI. It is also the only view that
        // works with NOTHING selected, so it is fetched outside the sel_pid
        // guard above - "what is talking to the network" is a question about
        // the machine, not about one process.
        let target = if self.conn_all {
            PI_PID_ALL
        } else if self.sel_pid != NO_SEL {
            self.sel_pid
        } else {
            return;
        };
        let n = unsafe { syscall3(SYS_NET_CONNS, target as i64, self.conns.as_mut_ptr() as i64, 32) } as i32;
        self.nconns = if n > 0 { n as usize } else { 0 };
    }

    fn selected_idx(&self) -> Option<usize> {
        for i in 0..self.nproc { if self.procs[i].pid == self.sel_pid { return Some(i); } }
        None
    }

    /// #161: the ONE place End Task and Kill go through, keyboard and mouse
    /// alike (there were four independent `syscall2(SYS_KILL, ...)` call sites
    /// and none of them looked at the result or at the target's state).
    ///
    /// A ZOMBIE is a process that has ALREADY EXITED and is holding a
    /// process-table slot until its parent reaps it. `kill(pid, SIGKILL)` on
    /// one is a no-op by definition, so offering Kill and reporting nothing was
    /// the misleading half of the owner's "clicking kill on the task manager is
    /// not closing it": the kill was correct and useless, and the UI said
    /// neither. The kernel now returns -ESRCH for exactly this case
    /// (proc/signal.c sys_kill), so the state check and the return value agree.
    fn signal_selected(&mut self, sig: i64) {
        if self.sel_pid <= 1 || self.sel_pid == NO_SEL {
            self.msg = b"Select a process first\0";
            return;
        }
        // PROC_STATE_ZOMBIE == 5 (kernel/proc/process.h), the same value
        // state_name() already renders as "Zombie" in the State column.
        let zombie = match self.selected_idx() {
            Some(i) => self.procs[i].state == 5,
            None => false,
        };
        if zombie {
            self.msg = b"Already exited (zombie) - waiting to be reaped\0";
            return;
        }
        // #745 (docs/CONFIRM_MODAL_DESIGN.html): End Task/Kill used to fire
        // here immediately, with NO confirmation at all - the audit's other
        // "biggest gap" alongside Files' Recycle Bin, and Force Quit only
        // recently started reliably delivering SIGKILL (today), which makes
        // an unconfirmed kill more consequential than it used to be. This
        // now only OPENS the shared confirm card; signal_selected_dispatch()
        // below is the one place the syscall actually fires, gated on the
        // user answering Yes (see the EVENT_KEY_DOWN/EVENT_MOUSE_DOWN gate
        // in main()).
        self.pending_sig = sig;
        let name = match self.selected_idx() { Some(i) => self.procs[i].name, None => [0u8; 32] };
        let mut body: [u8; 96] = [0; 96];
        let mut n: usize = 0;
        let prefix: &[u8] = if sig == SIGKILL { b"Force quit " } else { b"End " };
        for &b in prefix { body[n] = b; n += 1; }
        for &b in name.iter() {
            if b == 0 { break; }
            if n < body.len() - 1 { body[n] = b; n += 1; }
        }
        let suffix: &[u8] = b"? Any unsaved work in this app will be lost.";
        for &b in suffix { if n < body.len() - 1 { body[n] = b; n += 1; } }
        body[n] = 0;
        let title: &[u8] = if sig == SIGKILL { b"Force Quit\0" } else { b"End Task\0" };
        let action: &[u8] = if sig == SIGKILL { b"Force Quit\0" } else { b"End Task\0" };
        unsafe {
            gui_confirm_open_s(GUI_CONFIRM_DESTRUCTIVE, title.as_ptr(),
                               body.as_ptr(), core::ptr::null(), core::ptr::null(), 1,
                               b"Cancel\0".as_ptr(), action.as_ptr());
        }
    }

    /// The former body of signal_selected(): the one place SYS_KILL actually
    /// fires, now reached only after the shared confirm card returns Action.
    fn signal_selected_dispatch(&mut self, sig: i64) {
        let rc = unsafe { syscall2(SYS_KILL, self.sel_pid as i64, sig) };
        self.msg = if rc < 0 {
            b"No such process - it had already exited\0"
        } else if sig == SIGKILL {
            b"SIGKILL sent\0"
        } else {
            b"SIGTERM sent\0"
        };
    }

    /// #188: the ONE place priority changes, for the '+'/'-' KEYS and for the
    /// two footer buttons alike.
    ///
    /// Before this ticket the keys had the whole implementation inline, twice
    /// (main.rs:1126-1127), and the button that said "Prio +/-" was wired to
    /// nothing. That inline version also had three defects that only show up
    /// once the control is actually reachable:
    ///
    ///  - NO PID GUARD. SYS_SETPRIORITY (kernel/proc/syscall.c:2701-2714)
    ///    treats `pid <= 0` as THE CALLING PROCESS, so pressing '+' with pid 0
    ///    selected would have reniced the Task Manager itself while appearing
    ///    to act on the row under the cursor. It guards like signal_selected().
    ///  - NO RANGE CHECK. The kernel clamps to PRIO_IDLE..PRIO_REALTIME and
    ///    returns 0, so pressing '+' at Realtime "succeeded" forever and
    ///    changed nothing, with no way to tell that from a working press.
    ///  - NO FEEDBACK. Same footer message channel as End Task / Kill (#161),
    ///    because a control whose only effect is invisible is the defect.
    fn prio_step(&mut self, d: i64) {
        if self.sel_pid <= 1 || self.sel_pid == NO_SEL {
            self.msg = b"Select a process first\0";
            return;
        }
        if !self.have_detail {
            self.msg = b"No detail for that process (it may have exited)\0";
            return;
        }
        let next = self.detail.priority as i64 + d;
        if next < 0 { self.msg = b"Already at the lowest priority (Idle)\0"; return; }
        if next > 4 { self.msg = b"Already at the highest priority (Realtime)\0"; return; }
        let rc = unsafe { syscall2(SYS_SETPRIORITY, self.sel_pid as i64, next) };
        if rc < 0 { self.msg = b"Priority change refused\0"; return; }
        self.msg = match next {
            0 => b"Priority now Idle\0" as &'static [u8],
            1 => b"Priority now Low\0",
            2 => b"Priority now Normal\0",
            3 => b"Priority now High\0",
            _ => b"Priority now Realtime\0",
        };
        // Re-read, so the Details tab and the next press both see the new value
        // rather than stepping from a stale one.
        self.refresh_detail();
    }

    /// #188: Scheduled tab Enable/Disable. SYS_CRON_ENABLE (279) was DECLARED
    /// at the top of this file and never called by anything; the tab was
    /// read-only. cron_enable() (kernel/proc/cron.c:359) re-arms an INTERVAL or
    /// ONESHOT job relative to now when it is re-enabled, and persists the
    /// change to /CONFIG/CRON.CFG, so this survives a reboot.
    fn cron_set(&mut self, on: i64) {
        if self.job_sel >= self.njobs {
            self.msg = b"Select a scheduled task first\0";
            return;
        }
        let id = self.jobs[self.job_sel].id;
        let rc = unsafe { syscall2(SYS_CRON_ENABLE, id as i64, on) };
        self.msg = if rc < 0 {
            b"Scheduled task change refused\0"
        } else if on != 0 {
            b"Scheduled task enabled\0"
        } else {
            b"Scheduled task disabled\0"
        };
        // Re-list immediately so the State column agrees with what just
        // happened, in this frame.
        self.refresh_jobs();
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
        // #188: the Scheduled tab now has a selection, so the arrow keys have
        // to move it too. Without this the new Enable/Disable buttons would be
        // mouse-only, and this app has shipped on hardware with no working
        // pointer more than once.
        if self.tab == Tab::Scheduled {
            if self.njobs == 0 { return; }
            let mut i = self.job_sel as i32 + d;
            if i < 0 { i = 0; }
            if i >= self.njobs as i32 { i = self.njobs as i32 - 1; }
            self.job_sel = i as usize;
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
// #188: TAB_H / ROW_H / PAD moved to logic.rs, together with the column and
// button geometry, so hit-testing and drawing share one definition.

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

/// #188: one column header, showing whether it is the active sort and which
/// way. The indicator is not decoration: click-to-sort with no visible state is
/// a control that appears to do nothing on a list whose natural order already
/// looks plausible. Active gets full-strength ink plus an ASCII caret ('^'
/// ascending, 'v' descending - ASCII because draw_text takes bytes straight to
/// the TTF path and a multi-byte arrow renders as mojibake).
fn draw_col_hdr(a: &App, x: i32, y: i32, label: &[u8], col: SortCol) {
    let active = a.sort_col == col;
    let mut b = Buf::new();
    b.puts(label);
    if active { b.put(b' '); b.put(sort_glyph(a.sort_desc)); }
    draw_text(a.win, x, y, b.as_c(), 11, if active { a.pal.ink } else { a.pal.ink_dim });
}

fn draw_processes(a: &App) {
    let top = LIST_HDR_Y;
    // #188: ONE definition of the column X positions, shared with
    // header_col_at(). Six `a.dw - N` literals used to live here and nowhere
    // else, which is why nothing could hit-test them.
    let c = proc_cols(a.dw);
    let (c_name, c_pid, c_state, c_thr, c_cpu, c_mem) = (c.name, c.pid, c.state, c.core, c.cpu, c.mem);
    draw_col_hdr(a, c_name, top, b"Name\0", SortCol::Name);
    draw_col_hdr(a, c_pid, top, b"PID\0", SortCol::Pid);
    draw_col_hdr(a, c_state, top, b"State\0", SortCol::State);
    draw_col_hdr(a, c_thr, top, b"Core\0", SortCol::Core);
    draw_col_hdr(a, c_cpu, top, b"CPU\0", SortCol::Cpu);
    draw_col_hdr(a, c_mem, top, b"Memory\0", SortCol::Mem);
    let ltop = LIST_TOP_Y;
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
        // (rakbd) THREE STATES, NOT TWO. This used to be `< 1 -> '-'`, which
        // rendered "running on the BSP right now" (running_cpu == 0) and "not
        // running anywhere" (running_cpu == -1) as the SAME dash. Since the
        // scheduler publishes 0 for every process the BSP is executing, and the
        // BSP is where almost everything runs, the column could only ever show
        // a value for an AP - so it read as a nearly empty column with an
        // occasional AP number appearing and vanishing, which is exactly what
        // it was reported as. The data was right; the rendering collapsed the
        // common case into the "nothing here" glyph.
        if a.procs[i].running_cpu < 0 {
            b.put(b'-');
        } else if a.procs[i].running_cpu == 0 {
            b.puts(b"CPU0");
        } else {
            b.puts(b"AP");
            b.putu(a.procs[i].running_cpu as u64);
        }
        draw_text(a.win, c_thr, ry + 3, b.as_c(), 11, dim);
        b.clear(); b.putu(a.cpu_pct[i] as u64); b.put(b'%');
        draw_text(a.win, c_cpu, ry + 3, b.as_c(), 12, ink);
        b.clear(); b.put_kb(a.procs[i].mem_kb);
        draw_text(a.win, c_mem, ry + 3, b.as_c(), 11, dim);
    }

    // footer
    let fy = foot_y(a.dh);
    win_draw_rect(a.win, PAD, fy - 6, a.dw - 2 * PAD, 1, a.pal.border);
    let mut f = Buf::new();
    f.putu(a.nproc as u64); f.puts(b" processes   CPU "); f.putu(a.cpu_total.max(0) as u64); f.put(b'%');
    draw_text(a.win, PAD, fy + 6, f.as_c(), 11, a.pal.ink_dim);
    // #161/#188: result of the last End Task / Kill / priority change. Empty
    // until one is used.
    if a.msg.len() > 1 { draw_text(a.win, PAD + 170, fy + 6, a.msg, 11, a.pal.ink); }
    let can = a.sel_pid > 1 && a.sel_pid != NO_SEL;
    let st = if can { GUI_ST_NORMAL } else { GUI_ST_DISABLED };
    // #188: the SAME proc_btns() the click handler hit-tests. The old single
    // "Prio +/-" button could not have worked even if it had been wired up: one
    // rectangle cannot express two directions, so it is now the two buttons its
    // label was already promising, inside the same strip.
    let bt = proc_btns(a.dw);
    unsafe {
        gui_button(a.win, bt[0].x, fy, bt[0].w, FOOT_H, b"End Task\0".as_ptr(), GUI_BTN_PRIMARY, st);
        gui_button(a.win, bt[1].x, fy, bt[1].w, FOOT_H, b"Kill\0".as_ptr(), GUI_BTN_SECONDARY, st);
        gui_button(a.win, bt[2].x, fy, bt[2].w, FOOT_H, b"Prio -\0".as_ptr(), GUI_BTN_SECONDARY, st);
        gui_button(a.win, bt[3].x, fy, bt[3].w, FOOT_H, b"Prio +\0".as_ptr(), GUI_BTN_SECONDARY, st);
    }
}

/// #188: the boot-volume space card.
///
/// WHAT THIS IS AND IS NOT, because the difference matters more than the chart.
/// SYS_GET_DISK_TOTAL/FREE report `g_fat_fs`, which on the shipping
/// two-partition image is the FAT ESP - the ~256 MB BOOT volume, not the ext2
/// root that /APPS and user data live on, and not disk THROUGHPUT, of which
/// this kernel exports no counter at all. So the card is labelled for the
/// volume it actually measures, and the used/total megabytes are printed
/// alongside the percentage so the reader can check the ratio themselves.
/// A capacity that fails to read is drawn as "not reported", never as 0%,
/// because 0% is a specific and wrong claim.
fn draw_disk_card(a: &App, x: i32, y: i32, w: i32, h: i32) {
    let col = mix(a.pal.accent, 0x00D0A040, 60);
    draw_chart(a, x, y, w, h, &a.disk_hist, b"Disk used - boot ESP\0", col);
    let mut b = Buf::new();
    if a.disk_total_mb == 0 {
        b.puts(b"not reported");
    } else {
        b.putu(a.disk_total_mb - a.disk_free_mb);
        b.puts(b" of ");
        b.putu(a.disk_total_mb);
        b.puts(b" MB");
    }
    draw_text(a.win, x + 122, y + 5, b.as_c(), 10, a.pal.ink_dim);
}

fn put_ip(b: &mut Buf, ip: u32) {
    // net_status_t addresses are HOST byte order: (a<<24)|(b<<16)|(c<<8)|d.
    // (TcpConnInfo.remote_ip in draw_details is the OTHER convention; they are
    // different structs from different subsystems and both are documented.)
    b.putu(((ip >> 24) & 0xFF) as u64); b.put(b'.');
    b.putu(((ip >> 16) & 0xFF) as u64); b.put(b'.');
    b.putu(((ip >> 8) & 0xFF) as u64); b.put(b'.');
    b.putu((ip & 0xFF) as u64);
}

/// #188: the network card.
///
/// THIS IS DELIBERATELY NOT A CHART. The owner asked for a Network chart and
/// this is the honest half of that request: net_status_t (the only network
/// accessor a userland app has) carries link state, addresses, DHCP state and
/// a fault flag, and NO byte or packet counter - there is none anywhere in the
/// syscall table - so there is nothing to differentiate into a rate. Drawing a
/// throughput line here would mean inventing the numbers. The card says on its
/// face that it is state rather than traffic, so nobody reads a flat line as
/// "no traffic".
fn draw_net_card(a: &App, x: i32, y: i32, w: i32, h: i32) {
    unsafe { gui_card(a.win, x, y, w, h); }
    draw_text(a.win, x + 6, y + 4, b"Network\0", 11, a.pal.ink_dim);
    draw_text(a.win, x + 62, y + 5, b"link and address state; no byte counters exist\0",
              9, a.pal.ink_dim);
    let mut yy = y + 24;
    let lx = x + 6;
    if !a.have_net {
        draw_text(a.win, lx, yy, b"Status unavailable (SYS_NET_STATUS failed)\0", 11, a.pal.ink_dim);
        return;
    }
    let n = &a.net;
    let mut b = Buf::new();
    // Driver absent is a different fact from carrier down, and the kernel
    // reports them separately, so render them separately.
    let link: &[u8] = if n.driver == 0 { b"no NIC detected\0" }
                      else if n.link_up != 0 { b"up\0" }
                      else { b"down (no carrier)\0" };
    line(a, lx, &mut yy, b"Link\0", link);
    b.clear();
    if n.ip == 0 { b.puts(b"not configured"); }
    else { put_ip(&mut b, n.ip); b.put(b'/'); b.putu(n.prefix_len as u64); }
    line(a, lx, &mut yy, b"Address\0", b.as_c());
    b.clear();
    if n.gateway == 0 { b.puts(b"no default route"); } else { put_ip(&mut b, n.gateway); }
    line(a, lx, &mut yy, b"Gateway\0", b.as_c());
    b.clear();
    if n.dns_active == 0 { b.puts(b"none"); } else { put_ip(&mut b, n.dns_active); }
    line(a, lx, &mut yy, b"DNS\0", b.as_c());
    let cfg: &[u8] = if n.config_static != 0 { b"static (/CONFIG/NETIP.CFG)\0" }
                     else { match n.dhcp_state {
                         0 => b"idle\0", 1 => b"discovering\0",
                         2 => b"requesting\0", 3 => b"bound\0", _ => b"?\0" } };
    line(a, lx, &mut yy, b"Config\0", cfg);
    if n.faulty != 0 {
        draw_text(a.win, lx, yy, b"! link persistently unreachable\0", 11, 0x00E05050);
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
    // #188: update speed, from the SAME speed_btns() the click handler tests.
    // The refresh interval used to be the literal 1000 in the event loop with
    // no way to change it.
    draw_text(a.win, PAD + 174, top + 5, b"Speed\0", 10, a.pal.ink_dim);
    let sb = speed_btns();
    let sv = [Speed::High, Speed::Normal, Speed::Low, Speed::Paused];
    for i in 0..4 {
        unsafe {
            gui_button(a.win, sb[i].x, top, sb[i].w, 22, speed_label(sv[i]).as_ptr(),
                       if a.speed == sv[i] { GUI_BTN_PRIMARY } else { GUI_BTN_SECONDARY },
                       GUI_ST_NORMAL);
        }
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

    // #188: CPU | Memory on top, Disk | Network underneath. Was two full-width
    // charts with the bottom half of the tab empty.
    let cw = (gw - 6) / 2;
    let cw2 = gw - cw - 6;
    let ch = (gh - 6) / 2;
    let ch2 = gh - ch - 6;
    let x2 = PAD + cw + 6;
    let y2 = gy + ch + 6;
    draw_chart(a, PAD, gy, cw, ch, &a.cpu_hist, b"CPU\0", a.pal.accent);
    draw_chart(a, x2, gy, cw2, ch, &a.mem_hist, b"Memory\0", mix(a.pal.accent, 0x0040_C060, 60));
    let mut b = Buf::new();
    b.putu(a.mem_used / 1048576); b.puts(b" of "); b.putu(a.mem_total / 1048576); b.puts(b" MB");
    draw_text(a.win, x2 + 62, gy + 5, b.as_c(), 10, a.pal.ink_dim);
    if ch2 > 40 {
        draw_disk_card(a, PAD, y2, cw, ch2);
        draw_net_card(a, x2, y2, cw2, ch2);
    }
}

fn line(a: &App, x: i32, y: &mut i32, label: &[u8], val: &[u8]) {
    draw_text(a.win, x, *y, label, 11, a.pal.ink_dim);
    draw_text(a.win, x + 120, *y, val, 11, a.pal.ink);
    *y += 15;
}

/// #188: the connections panel, split out of draw_details() so it can be drawn
/// whether or not a process is selected. The system-wide view is a fact about
/// the MACHINE, and the old code returned early before reaching it whenever
/// nothing was selected, which is precisely when you most want to ask "what is
/// talking to the network".
fn draw_conns(a: &App) {
    let mut cy = a.dh - 108;
    win_draw_rect(a.win, PAD, cy - 6, a.dw - 2 * PAD, 1, a.pal.border);
    let mut cb = Buf::new();
    cb.puts(if a.conn_all { b"Network connections (all processes): " }
            else { b"Network connections (this process): " });
    cb.putu(a.nconns as u64);
    draw_text(a.win, PAD, cy, cb.as_c(), 11, a.pal.ink);
    // The scope toggle, from the SAME conn_scope_btn() the click handler tests.
    let sb = conn_scope_btn(a.dw);
    unsafe {
        gui_button(a.win, sb.x, cy - 5, sb.w, 20,
                   if a.conn_all { b"Scope: All\0".as_ptr() } else { b"Scope: Process\0".as_ptr() },
                   GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    }
    cy += 15;
    for i in 0..a.nconns {
        if cy > a.dh - 40 { break; }
        let c = &a.conns[i];
        let mut lb = Buf::new();
        if a.conn_all {
            // Which pid owns it: the whole point of the machine-wide view.
            lb.puts(b"pid "); lb.putu(c.owner_pid as u64); lb.puts(b"  ");
        }
        lb.puts(tcp_state_name(c.state)); lb.puts(b"  :"); lb.putu(c.local_port as u64);
        if c.is_listener != 0 { lb.puts(b" (listen)"); }
        else {
            lb.puts(b" -> ");
            // #188: THIS WAS PRINTING EVERY ADDRESS BACKWARDS, and had been
            // since #487 - it emitted the low byte first, so this machine's
            // own gateway showed as "251.1.168.192". kernel/net/tcp.c:782 says
            // in as many words that tcp_conn_t stores remote_ip "in host order
            // on both the connect and the accept path", so the high byte is
            // the first octet. Found because the all-processes view added by
            // this same ticket put a KNOWN address on screen (the VNC peer),
            // which is the only reason a reversed IP was recognisable as one:
            // with a single unfamiliar address it just looks like an address.
            // Now the same put_ip() the Network card uses, so there is one
            // definition of "print an IPv4 address" in this file.
            put_ip(&mut lb, c.remote_ip);
            lb.put(b':'); lb.putu(c.remote_port as u64);
        }
        draw_text(a.win, PAD + 8, cy, lb.as_c(), 10, a.pal.ink_dim);
        cy += 13;
    }
    if a.nconns == 0 {
        draw_text(a.win, PAD + 8, cy,
                  if a.conn_all { b"(no TCP connections on this machine)\0" }
                  else { b"(this process has no connections)\0" },
                  10, a.pal.ink_dim);
    }
}

fn draw_details(a: &App) {
    let mut y = PAD + TAB_H + 10;
    let x = PAD + 4;
    if !a.have_detail {
        draw_text(a.win, x, y, b"No process selected (pick one on Processes).\0", 11, a.pal.ink_dim);
        // #188: but the connections panel is still drawn - see draw_conns().
        draw_conns(a);
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

    draw_conns(a);
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
    let top = LIST_HDR_Y;
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
    let ltop = LIST_TOP_Y;
    win_draw_rect(a.win, PAD, ltop - 3, a.dw - 2 * PAD, 1, a.pal.border);
    let mut b = Buf::new();
    for i in 0..a.njobs {
        let ry = ltop + (i as i32) * ROW_H;
        if ry > a.dh - 50 { break; }
        // #188: row selection, drawn exactly like the Services tab's.
        let sel = i == a.job_sel;
        if sel { win_draw_rect(a.win, PAD, ry, a.dw - 2 * PAD, ROW_H - 1, a.pal.accent); }
        else if i & 1 == 1 { win_draw_rect(a.win, PAD, ry, a.dw - 2 * PAD, ROW_H - 1, unsafe { gui_lighten(a.pal.surface, 4) }); }
        let ink = if sel { ink_on(a.pal.accent) } else { a.pal.ink };
        let dim = if sel { ink_on(a.pal.accent) } else { a.pal.ink_dim };
        let j = &a.jobs[i];
        let nm: &[u8] = if j.label[0] != 0 { &j.label } else { &j.target };
        draw_text(a.win, c_name, ry + 3, nm, 12, ink);
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
        draw_text(a.win, c_when, ry + 3, b.as_c(), 11, dim);
        draw_text(a.win, c_act, ry + 3, cron_action_name(j.action), 11, dim);
        b.clear(); b.putu(j.run_count as u64);
        draw_text(a.win, c_runs, ry + 3, b.as_c(), 11, dim);
        draw_text(a.win, c_en, ry + 3, if j.enabled != 0 { b"On\0" } else { b"Off\0" }, 11,
                  if j.enabled != 0 && !sel { 0x0040C060 } else { dim });
    }
    if a.njobs == 0 { draw_text(a.win, PAD + 4, ltop + 6, b"(no scheduled tasks registered)\0", 11, a.pal.ink_dim); }
    // #188: the footer this tab never had. SYS_CRON_ENABLE was declared in this
    // file and called by nothing, so the tab was read-only. Same geometry as
    // the Services tab's Start/Stop, because it is the same idea.
    //
    // THE QUIET CASE IS THE COMMON ONE HERE: a default install registers no
    // cron jobs at all, so both buttons must come up DISABLED rather than
    // firing SYS_CRON_ENABLE on jobs[0] of an empty array.
    let fy = foot_y(a.dh);
    win_draw_rect(a.win, PAD, fy - 6, a.dw - 2 * PAD, 1, a.pal.border);
    let can = a.job_sel < a.njobs;
    let st = if can { GUI_ST_NORMAL } else { GUI_ST_DISABLED };
    let sb = sched_btns();
    unsafe {
        gui_button(a.win, sb[0].x, fy, sb[0].w, FOOT_H, b"Enable\0".as_ptr(), GUI_BTN_PRIMARY, st);
        gui_button(a.win, sb[1].x, fy, sb[1].w, FOOT_H, b"Disable\0".as_ptr(), GUI_BTN_SECONDARY, st);
    }
    if a.msg.len() > 1 { draw_text(a.win, PAD + 170, fy + 6, a.msg, 11, a.pal.ink); }
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
    // #745: the shared End Task/Kill confirm card, drawn last so it sits on
    // top of everything else in the window.
    unsafe {
        if gui_confirm_singleton_is_open() != 0 {
            gui_confirm_singleton_render(a.win, a.dw, a.dh);
        }
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
        procs: [ZP; MAXP], nproc: 0, cpu_pct: [0; MAXP],
        // #178: zeroed once, then owned by libc/proccpu.c. `valid: 0` is what
        // makes the first refresh show 0% rather than a lifetime share.
        cpu: ProcCpu { pid: [0; MAXP], ticks: [0; MAXP], n: 0, valid: 0 },
        // NO_SEL, not 0: pid 0 is the idle process, so initialising
        // sel_pid to 0 made idle look selected on first paint.
        sel_pid: NO_SEL, scroll: 0, cpu_total: 0,
        mem_total: 0, mem_used: 0,
        cpu_hist: [0; HIST], mem_hist: [0; HIST], core_hist: [[0; HIST]; MAXCORES],
        hist_n: 0, ncores: 1, perf_cores: false,
        detail: ZD, have_detail: false,
        handles: [ZH; 32], nhandles: 0, conns: [ZC; 32], nconns: 0,
        svcs: [ZS; 32], nsvcs: 0, svc_sel: 0, jobs: [ZJ; 32], njobs: 0, job_sel: 0,
        // #188: the order the app has always opened in. For this one column it
        // is still produced by the shared libc proccpu_sort(), not by
        // logic.rs's comparator - see sort_apply().
        sort_col: SortCol::Cpu, sort_desc: true,
        conn_all: false,
        disk_hist: [0; HIST], disk_total_mb: 0, disk_free_mb: 0,
        net: ZN, have_net: false,
        speed: Speed::Normal,
        pal: GuiPalette { surface: 0, surface_raised: 0, ink: 0xF0F0F0, ink_dim: 0x909090,
                          accent: 0x3080D0, accent_hover: 0x4090E0, border: 0x404040,
                          field_bg: 0x202020, field_border: 0x404040, track: 0x303030 },
        dark: true,
        msg: b"\0",   // #161: empty until End Task / Kill is used
        pending_sig: 0,
    };
    app.refresh();
    draw(&mut app);

    let mut ev = GuiEvent { ty: 0, target_id: 0, mouse_x: 0, mouse_y: 0,
                            mouse_buttons: 0, scroll_delta: 0, keycode: 0, key_char: 0 };
    loop {
        // Blocking wait with a 1s timeout: the kernel wait-queue wakes us on an
        // event, the timeout drives the 1Hz refresh. This is the shared
        // primitive; a poll loop here would busy-spin a core (#453/#426).
        // #188: the interval was the bare literal 1000 here, with no control
        // anywhere in the UI. Paused still uses a real timeout so the window
        // keeps servicing events; it just stops re-sampling.
        let et = win_get_event(app.win, &mut ev, speed_ms(app.speed));
        if et == 0 {
            if speed_samples(app.speed) {
                app.refresh();
                draw(&mut app);
            }
            continue;
        }
        match ev.ty {
            EVENT_REDRAW | EVENT_RESIZE => draw(&mut app),
            EVENT_WINDOW_CLOSE => break,
            EVENT_KEY_DOWN => {
              if unsafe { gui_confirm_singleton_is_open() } != 0 {
                // #745: the confirm card is a true modal - it gets first
                // crack at every key while open, above the app's own
                // shortcuts below.
                let r = unsafe { gui_confirm_singleton_handle_key(ev.key_char as i32) };
                if r == 2 {
                    let sig = app.pending_sig; app.pending_sig = 0;
                    if sig != 0 { app.signal_selected_dispatch(sig); app.refresh(); }
                } else if r == 1 {
                    app.pending_sig = 0;
                }
                draw(&mut app);
              } else {
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
                    b'e' | b'E' => { app.signal_selected(SIGTERM); app.refresh(); draw(&mut app); }   // #161
                    b'k' | b'K' => { app.signal_selected(SIGKILL); app.refresh(); draw(&mut app); }   // #161
                    // #188: both the keys and the two footer buttons now reach
                    // priority through the SAME prio_step(), which is where the
                    // pid guard, the range check and the feedback live. The
                    // inline versions that used to be here had none of them.
                    b'+' | b'=' => { app.prio_step(1); draw(&mut app); }
                    b'-' | b'_' => { app.prio_step(-1); draw(&mut app); }
                    // #188: sort column / direction without a pointer. 's'
                    // cycles the column, 'S' (shift) reverses the current one -
                    // the same two transitions a header click produces, through
                    // the same sort_click()/sort_next() pair.
                    b's' => { let n = sort_cycle(app.sort_col); app.sort_click(n); draw(&mut app); }
                    b'S' => { let c = app.sort_col; app.sort_click(c); draw(&mut app); }
                    // #188: Details tab connection scope (this process / all).
                    b'a' | b'A' => { app.conn_all = !app.conn_all; app.refresh_detail(); draw(&mut app); }
                    // #188: update speed.
                    b'u' | b'U' => { app.speed = speed_cycle(app.speed); draw(&mut app); }
                    // #188: Scheduled tab - space toggles the selected task.
                    // Deliberately tab-local: on any other tab it does nothing,
                    // rather than doing something invisible somewhere else.
                    b' ' => {
                        if app.tab == Tab::Scheduled {
                            let on = if app.job_sel < app.njobs && app.jobs[app.job_sel].enabled != 0 { 0 } else { 1 };
                            app.cron_set(on);
                            draw(&mut app);
                        }
                    }
                    b'r' | b'R' => { app.refresh(); draw(&mut app); }
                    _ => {
                        // Arrow keys arrive with no ASCII char, so they are
                        // matched on keycode, not key_char. Up/Down move the
                        // selection on the list tabs.
                        //
                        // #188: THESE WERE THE WRONG CONSTANTS AND HAD ALWAYS
                        // BEEN, so arrow-key navigation in this app had never
                        // once worked. They were 0x48/0x50, the raw PS/2
                        // SCANCODES for Up/Down. What actually reaches an app
                        // window is the kernel's TRANSLATED special-key code:
                        // kernel/cpu/isr.h:8-9 defines KEY_UP 0x80 and
                        // KEY_DOWN 0x81 (release = +0x10), and
                        // kernel/gui/window.c:1703 puts exactly that value in
                        // event.keycode. The comment that used to sit here
                        // said this handler was what stopped the app being
                        // "mouse-ONLY... impossible to reach Details on a
                        // machine with no working pointer" - a claim about a
                        // code path that could not fire. Proven by sending a
                        // real RFB KeyEvent for XK_Down (which vnc.c maps to
                        // MK_DOWN 0x81 and injects through the same
                        // sys_inject_key() the PS/2 path uses): with 0x50 the
                        // selection did not move, with 0x81 it does.
                        // #191: the two corrected values are no longer
                        // restated here; they come from the one shared table.
                        match ev.keycode {
                            keys::GUI_KEY_UP => { app.select_step(-1); draw(&mut app); }
                            keys::GUI_KEY_DOWN => { app.select_step(1);  draw(&mut app); }
                            _ => {}
                        }
                    }
                }
              }
            }
            EVENT_MOUSE_DOWN => {
              if unsafe { gui_confirm_singleton_is_open() } != 0 {
                // #745: the confirm card is a true modal - first crack at
                // every click, never dismissed by clicking away from it.
                let r = unsafe { gui_confirm_singleton_handle_mouse(ev.mouse_x, ev.mouse_y, 1) };
                if r == 2 {
                    let sig = app.pending_sig; app.pending_sig = 0;
                    if sig != 0 { app.signal_selected_dispatch(sig); app.refresh(); }
                } else if r == 1 {
                    app.pending_sig = 0;
                }
                draw(&mut app);
              } else {
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
                    let fy = foot_y(app.dh);
                    if my >= fy && my < fy + FOOT_H {
                        // #161/#188: one gated path for every footer button, so
                        // the zombie check and the feedback cannot be present
                        // on the keyboard route and missing on the mouse route.
                        // The rectangles come from proc_btns(), the same call
                        // draw_processes() uses, so the button you can see and
                        // the button that responds cannot drift apart - which
                        // is exactly what had happened to "Prio +/-".
                        //
                        // The old code also gated the whole footer on "something
                        // is selected" and so did NOTHING at all when nothing
                        // was; each handler now says "Select a process first"
                        // in the footer instead.
                        match proc_foot_hit(app.dw, mx) {
                            FootAct::EndTask => { app.signal_selected(SIGTERM); app.refresh(); }
                            FootAct::Kill => { app.signal_selected(SIGKILL); app.refresh(); }
                            FootAct::PrioDown => { app.prio_step(-1); }
                            FootAct::PrioUp => { app.prio_step(1); }
                            FootAct::None => {}
                        }
                        draw(&mut app);
                    } else if my >= LIST_HDR_Y && my < LIST_TOP_Y {
                        // #188: click-to-sort. This band used to fall straight
                        // through: the headers were drawn and hit-tested by
                        // nothing at all.
                        if let Some(c) = header_col_at(app.dw, mx) {
                            app.sort_click(c);
                            draw(&mut app);
                        }
                    } else if my >= LIST_TOP_Y {
                        let r = ((my - LIST_TOP_Y) / ROW_H) as usize + app.scroll;
                        if r < app.nproc { app.sel_pid = app.procs[r].pid; app.refresh_detail(); draw(&mut app); }
                    }
                } else if app.tab == Tab::Performance {
                    let top = PAD + TAB_H + 8;
                    if my >= top && my < top + 22 {
                        if mx >= PAD && mx < PAD + 80 { app.perf_cores = false; draw(&mut app); }
                        else if mx >= PAD + 86 && mx < PAD + 166 { app.perf_cores = true; draw(&mut app); }
                        else if let Some(s) = speed_at(mx) { app.speed = s; draw(&mut app); }   // #188
                    }
                } else if app.tab == Tab::Details {
                    // #188: the connections scope toggle.
                    let cy = app.dh - 108;
                    if my >= cy - 5 && my < cy + 15 && conn_scope_btn(app.dw).hit(mx) {
                        app.conn_all = !app.conn_all;
                        app.refresh_detail();
                        draw(&mut app);
                    }
                } else if app.tab == Tab::Scheduled {
                    // #188: the tab was entirely inert - no selection, no
                    // buttons, and no Tab::Scheduled branch in this chain at
                    // all. SYS_CRON_ENABLE was declared and never called.
                    let fy = foot_y(app.dh);
                    if my >= fy && my < fy + FOOT_H {
                        match sched_foot_hit(mx) {
                            SchedAct::Enable => { app.cron_set(1); draw(&mut app); }
                            SchedAct::Disable => { app.cron_set(0); draw(&mut app); }
                            SchedAct::None => {}
                        }
                    } else if my >= LIST_TOP_Y {
                        let r = ((my - LIST_TOP_Y) / ROW_H) as usize;
                        if r < app.njobs { app.job_sel = r; draw(&mut app); }
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
