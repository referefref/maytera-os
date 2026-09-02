// setup - MayteraOS first-boot setup wizard (OOBE), in Rust (#745).
//
// Replaces the C main.c (kept as the rollback path, the same strangler shape
// taskmgr uses under #487). Geometry and colour tokens come from
// docs/OOBE_SPEC.md, implemented from the numbers rather than by slicing the
// rendered mockup: usernames, inline errors, theme names read from
// /THEMES/INDEX.TXT and 63 real wallpaper thumbnails are all LIVE data, so
// baking text into bitmaps would have frozen exactly the parts that must vary.
//
// BUILD (see Makefile): rustc 1.97.0, --target x86_64-unknown-none
//   -C code-model=large        : the target defaults to code-model=kernel
//   -C relocation-model=pic    : userland is PIE now (user-pie.ld); the
//                                launched-app gate REJECTS a non-PIE binary
//   -C panic=abort             : no unwinder in Ring 3
// The target is already soft-float (+soft-float,-sse,-sse2), which is what we
// want: this kernel never saves FPU state across a context switch. There is no
// float in this file by design, not by luck.
//
// FFI: syscall0..6 are real linkable symbols (libc/syscall.asm), so syscalls
// are issued directly with no C shim. The style-engine widgets and the config
// writers (gui_theme_activate, userconf_*, wp_enumerate) are real symbols in
// libc.a and are called directly, so a setting made here is byte-identical to
// the same setting made later in Settings.
#![no_std]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_i: &PanicInfo) -> ! {
    // panic=abort in Ring 3: exit loudly rather than spin. A spin here would
    // burn a core forever (#426).
    unsafe { syscall1(SYS_EXIT, 101); }
    loop {}
}

// ---------------------------------------------------------------------------
// libc FFI
// ---------------------------------------------------------------------------
extern "C" {
    fn syscall0(n: i64) -> i64;

    // #49/#50 SHARED, not copied. userland/libc/tz.c owns THE timezone list,
    // THE persisted setting (/CONFIG/TZ.CFG) and THE local-clock helper. This
    // wizard used to carry its own 35-entry TZ[]/TZ_OFF_MIN pair and hand-roll
    // the TZ.CFG write, while Settings carried a THIRD, diverged 26-entry copy
    // that never read the file. Same argument as gui_dock_style_name() below:
    // do not reintroduce a local array, not even "just for the readout".
    fn tz_count() -> i32;
    #[link_name = "tz_id"]
    fn tz_id_c(idx: i32) -> *const u8;
    fn tz_offset_min_at(idx: i32) -> i32;
    fn tz_index_utc() -> i32;
    #[link_name = "tz_index_for_offset"]
    fn tz_index_for_offset_c(off_min: i32) -> i32;
    // Writes /CONFIG/TZ.CFG. 0 on success; -1 means the file does NOT hold the
    // zone and the caller must not report success.
    fn tz_set_index(idx: i32) -> i32;
    // Adds off_min to a civil date-time in place, with date rollover. Used here
    // with a NEGATED offset to turn the local time a person typed back into the
    // UTC the RTC is defined to hold.
    fn tz_shift(off_min: i32, hour: *mut i32, min: *mut i32, sec: *mut i32,
                day: *mut i32, month: *mut i32, year: *mut i32);
    fn syscall1(n: i64, a1: i64) -> i64;
    fn syscall2(n: i64, a1: i64, a2: i64) -> i64;
    fn syscall3(n: i64, a1: i64, a2: i64, a3: i64) -> i64;
    fn syscall5(n: i64, a1: i64, a2: i64, a3: i64, a4: i64, a5: i64) -> i64;
    fn syscall6(n: i64, a1: i64, a2: i64, a3: i64, a4: i64, a5: i64, a6: i64) -> i64;

    // Shared style engine - real symbols in libc.a.
    fn gui_set_style(style: i32);
    fn gui_set_palette(p: *const GuiPalette);
    // #745 glass card: every symbol below takes WINDOW coordinates. The whole
    // wizard body is now drawn inside a card that is inset within the window,
    // so each one is re-exported under a _raw name and wrapped by a shim (see
    // "content-box translation" below) that adds the body origin exactly once.
    // Doing it here rather than at ~600 call sites is the same single-choke-
    // point argument that applies to wel_composite_at().
    #[link_name = "gui_button"]
    fn gui_button_raw(h: i32, x: i32, y: i32, w: i32, ht: i32, label: *const u8,
                  variant: i32, st: i32);
    #[link_name = "gui_progress"]
    fn gui_progress_raw(h: i32, x: i32, y: i32, w: i32, ht: i32, pct: i32);
    #[link_name = "gui_fill_rounded_aa"]
    fn gui_fill_rounded_aa_raw(h: i32, x: i32, y: i32, w: i32, ht: i32, r: i32,
                           fill: u32, outer: u32);
    #[link_name = "gui_fill_circle_aa"]
    fn gui_fill_circle_aa_raw(h: i32, cx: i32, cy: i32, r: i32, fill: u32, outer: u32);
    fn gui_ttf_width(s: *const u8, size: i32) -> i32;
    // #745 SHARED, not copied. gui_dock_style_name() is THE dock-style name
    // list (libc gui_dock.c), also read by Settings; this app used to keep its
    // own array and the two drifted, which is the bug being fixed. Do not add
    // a local DOCK_NAMES back: build/dock-name-gate.sh fails the build for it.
    fn gui_dock_style_name(idx: i32) -> *const u8;
    fn gui_dock_style_count() -> i32;
    // #745 theme preview: the top-right corner of an example window, drawn
    // from the theme's REAL tokens by the same libc primitive Settings calls.
    #[link_name = "gui_theme_win_preview"]
    fn gui_theme_win_preview_raw(h: i32, x: i32, y: i32, w: i32, ht: i32,
                                 theme_index: i32, cut_bg: u32);
    // Vertical-gradient rounded rect - what gui_button()'s GUI_BTN_PRIMARY
    // already draws internally. Reused for the welcome screen's primary
    // button (docs/OOBE_WELCOME_REDESIGN.html row 7) rather than hand-rolled.
    #[link_name = "gui_fill_rounded_grad"]
    fn gui_fill_rounded_grad_raw(h: i32, x: i32, y: i32, w: i32, ht: i32, r: i32,
                             top: u32, bottom: u32);

    // Config primitives - the point is that this app owns none of these.
    fn gui_theme_list(out: *mut ThemeEntry, max: i32) -> i32;
    fn gui_theme_get_active_slug(slug: *mut u8, cap: i32) -> i32;
    fn gui_theme_activate(slug: *const u8) -> i32;
    fn wp_enumerate(out: *mut WpEntry, max: i32) -> i32;
    fn userconf_write_all(path: *const u8, buf: *const u8, len: u64) -> i32;
    fn userconf_open_write(name: *const u8) -> i32;
    fn userconf_finish_write(fd: i32, buf: *const u8, len: u64) -> i32;
    // #745 follow-up: read back an already-saved AISVC.CFG (the AI provider
    // page's "key already set" state, and preserving an untouched key when
    // only the provider/model changes). Same per-user resolution aiclient.c's
    // load_aisvc() uses.
    fn userconf_open_read(name: *const u8, legacy: *const u8) -> i32;
}

#[repr(C)]
struct GuiPalette {
    surface: u32, surface_raised: u32, ink: u32, ink_dim: u32,
    accent: u32, accent_hover: u32, border: u32,
    field_bg: u32, field_border: u32, track: u32,
}

const GUI_STYLE_MODERN: i32 = 1;
const GUI_BTN_PRIMARY: i32 = 0;
const GUI_BTN_SECONDARY: i32 = 1;
const GUI_ST_NORMAL: i32 = 0;
const GUI_ST_HOVER: i32 = 1;

// gui_theme_entry_t / wp_entry_t mirrors. Sizes are locked by const assert
// below: if either C struct grows, this app fails to COMPILE rather than
// decoding garbage off the stack.
const THEME_SLUG_MAX: usize = 32;
const THEME_NAME_MAX: usize = 40;
#[repr(C)]
#[derive(Clone, Copy)]
struct ThemeEntry {
    slug: [u8; THEME_SLUG_MAX],
    name: [u8; THEME_NAME_MAX],
    is_dark: i32,
    is_classic: i32,
    index: i32,
}
const WP_FILE_MAX: usize = 40;
const WP_NAME_MAX: usize = 40;
#[repr(C)]
#[derive(Clone, Copy)]
struct WpEntry {
    file: [u8; WP_FILE_MAX],
    name: [u8; WP_NAME_MAX],
}
const _: () = assert!(core::mem::size_of::<ThemeEntry>() == 84);
const _: () = assert!(core::mem::size_of::<WpEntry>() == 80);

// ---------------------------------------------------------------------------
// Syscalls (numbers verified against userland/libc/syscall.h, not assumed)
// ---------------------------------------------------------------------------
const SYS_EXIT: i64 = 0;
const SYS_WIN_CREATE: i64 = 30;
// #198: second window for the bottom-right power corner (docs/
// WIZARD_POWER_CORNER.html section 1). Read out of libc/syscall.h (393),
// same number win_create_bg() there wraps - not guessed, per this file's own
// standing rule for every SYS_* constant it keeps a private copy of.
const SYS_WIN_CREATE_BG: i64 = 393;
const SYS_WIN_DESTROY: i64 = 31;
const SYS_WIN_DRAW_RECT: i64 = 32;
const SYS_WIN_GET_EVENT: i64 = 36;
const SYS_WIN_DRAW_TTF: i64 = 235;
const SYS_SET_WALLPAPER: i64 = 204;
const SYS_SET_AUTOLOGIN: i64 = 339;
// #745 sign-in screen mode, the second key of /CONFIG/LOGIN.CFG.
//
// THE COMMENT THAT USED TO SIT HERE SAID 373/374 AND EXPLAINED, CORRECTLY FOR
// THE DAY IT WAS WRITTEN, HOW THOSE TWO NUMBERS WERE THE NEXT TWO FREE. Both
// were then renumbered to 374/375 during a merge, because 373 had been handed
// to SYS_WM_SET_WORK_AREA on another branch. The renumber reached the kernel
// header, the userland header, the dispatch table and argtab.rs, and did not
// reach THIS BLOCK, because a no_std Rust app cannot include the C header and
// so keeps a FIFTH copy of every number it uses. Nothing failed to build.
//
// What shipped in build 1811: SYS_SET_LOGIN_MODE at 373 called
// sys_wm_set_work_area(), which returns -1 to anything that is not the
// compositor, and SYS_GET_LOGIN_MODE at 374 called sys_set_login_mode() (a
// WRITE, via syscall0, with whatever happened to be in the argument
// registers), which argtab.rs's pointer validation refused with -14. Every
// first-boot run therefore ended on "Sign-in preference could not be saved".
//
// These numbers are now checked by `make syscall-number-lint` rule 5, which
// reads every app-local SYS_* definition in userland/apps and fails the build
// on any disagreement with the headers. Do not re-derive them by hand.
const SYS_SET_LOGIN_MODE: i64 = 374;
const SYS_GET_LOGIN_MODE: i64 = 375;
// 0 = show a list of users, 1 = ask for a name and password. Mirrored from
// kernel/proc/syscall.h (LOGIN_MODE_LIST / LOGIN_MODE_TYPED).
const LOGIN_MODE_LIST: i64 = 0;
const LOGIN_MODE_TYPED: i64 = 1;
const SYS_USER_CREATE_PW: i64 = 362;
// #745, both grepped out of kernel/proc/syscall.h rather than guessed. A wrong
// number here renders a screen that does nothing, which has happened in this
// project often enough to be a rule.
const SYS_PW_CHECK: i64 = 369;
const SYS_FIRSTBOOT_ADMIN: i64 = 370;
// #229 FIRST-RUN STATE. THE fifth copy of these numbers, and the reason the
// kernel asserts them on every boot: this app is no_std and cannot include
// either header, which is how 373/374 above went stale and shipped.
//
// WHY THIS EXISTS AT ALL. Every durable thing this wizard used to do was an
// open(O_CREAT) under /CONFIG, and /CONFIG is root-owned 0711 because it holds
// SHADOW, AUTHKEYS, SSHD.CFG and the owner's API keys. Since #226 removed
// autologin=root the wizard runs as the session user, so all four writes were
// refused. See kernel/rustkern/firstrun.rs.
//
// FR_HANDOVER_SET ALWAYS RETURNS 0. It is a bit of kernel RAM, not a
// filesystem. Do not write a caller that gives up when it "fails": that exact
// shape is what made the old #136 escape hatch (below) share a failure mode
// with the thing it existed to escape.
//
// #136 REMOVED (owner request, verbatim, 2026-08-28: "remove the skip to
// desktop option since it no longer makes sense"). FR_SKIP_SET and
// skip_to_desktop() are gone from this file. Checked before removing, not
// assumed safe: this control stopped being a real escape hatch at #228 (it
// already redirected to flow_error_page() instead of reaching a desktop with
// no account), and after an account exists the actual, robust escape is
// skip_link_bounds()/SKIP_LABEL/skip() (F10, page_skippable() pages,
// including Network with no working link - its default is DHCP, not a live
// connectivity check) which runs apply() and writes /CONFIG/SETUPDONE. This
// control instead quit WITHOUT marking the wizard done, so it silently
// reappeared next boot. tools/dos-harness/dosharness.py's --skip-oobe is a
// different mechanism entirely (writes /CONFIG/SETUPDONE + SETUPUSR to the
// filesystem before the guest boots) and never called FR_SKIP_SET, so it is
// unaffected. The kernel-side FR_SKIP_SET/GET/CLEAR primitives
// (kernel/rustkern/firstrun.rs) and the compositor's own read of
// FR_SKIP_GET (userland/apps/compositor/main.c) are left in place, unused -
// kernel-side plumbing with its own self-tests, not this file's to remove.
const SYS_FIRSTRUN: i64 = 397;
const FR_MARK_DONE: i64 = 0;
const FR_HANDOVER_SET: i64 = 4;
// #OOBEAUTH (2026-08-23): -> 1 iff THIS session may call SYS_FIRSTBOOT_ADMIN
// right now (see machine_admin() below and firstboot_bootstrap_ok_rs() in
// kernel/rustkern/firstrun.rs for the predicate this asks). True for real
// root, and true for the one narrow first-boot bootstrap session besides.
const FR_BOOTSTRAP_QUERY: i64 = 6;
// #229/#OOBEAUTH: whether this session may act on the machine decides which
// pages this wizard may show. A session with neither real root NOR the
// first-boot bootstrap exception cannot create an account or rewrite
// /CONFIG/LOGIN.CFG, and offering it a form that is guaranteed to fail is
// worse than not offering it.
const SYS_GETEUID: i64 = 124;
const SYS_OPEN: i64 = 10;    // verified against libc/syscall.h
const SYS_READ: i64 = 12;
const SYS_CLOSE: i64 = 11;
const SYS_SEEK: i64 = 14;    // verified against libc/syscall.h (SEEK_SET = whence 0)
const SYS_DECODE_IMAGE: i64 = 253;   // verified against libc/syscall.h
const SYS_WIN_DRAW_IMAGE: i64 = 254;
const SYS_GET_RTC_TIME: i64 = 142;   // verified against libc/syscall.h
const SYS_GET_RTC_DATE: i64 = 143;
const SYS_SET_RTC_TIME: i64 = 144;
const SYS_SET_RTC_DATE: i64 = 145;
const SYS_NTP_SYNC_SERVER: i64 = 367;   // #797 SNTP client
// #745 Network page. Every one of these was read out of
// userland/libc/syscall.h before use; none is guessed.
const SYS_NET_STATUS: i64 = 371;         // structured live IPv4 status
const SYS_NET_PROBE: i64 = 372;          // non-blocking ICMP / DHCP probe
const SYS_DNS_START: i64 = 215;          // non-blocking resolve: 1 now, 0 pending
const SYS_DNS_POLL: i64 = 216;
const SYS_HTTP_FETCH_START: i64 = 255;   // async fetch -> job id
// #549: fetch refused because the kernel has marked the adapter NET_FAULTY,
// as opposed to a generic start failure (-1). Mirrors NET_ERR_FAULTY in
// kernel/proc/syscall.h.
const NET_ERR_FAULTY: i64 = -3;
const SYS_HTTP_FETCH_POLL: i64 = 256;    // -> 0 running, 1 done, 2 error
const SYS_HTTP_FETCH_CANCEL: i64 = 258;  // frees the job slot
const SYS_UPTIME_MS: i64 = 252;          // monotonic ms since boot
// #136: verified against libc/syscall.h (SYS_POWEROFF=206, SYS_REBOOT=207),
// same numbers login.c/lockscreen.c already call reboot()/poweroff() with -
// this is that same existing power path, not a new one.
const SYS_POWEROFF: i64 = 206;
const SYS_REBOOT: i64 = 207;
// #198: live theme access for the power-corner window - "OS furniture" like
// the taskbar/volume OSD, so it reads the shared theme table instead of the
// wizard's own bespoke WEL_BG_*/glass palette. Both numbers and every color
// id below were grepped out of kernel/gui/theme.h and libc/syscall.h, not
// guessed (docs/WIZARD_POWER_CORNER.html section 4). theme_id -1 means "the
// active theme", same convention userland/apps/taskmanager/main.rs's own
// theme_color() and libc/theme.h's theme_metric() both already use.
const SYS_THEME_COLOR: i64 = 290;
const SYS_THEME_METRIC: i64 = 357;
const THEME_COLOR_BUTTON_FACE: i64 = 14;
const THEME_COLOR_BUTTON_LIGHT: i64 = 15;
// THEME_COLOR_BUTTON_SHADOW (id 16) intentionally not declared here: review
// flagged it as dead code (#198 follow-up). kernel/gui/themes.c's
// theme_color() switch returns t->button_border for BOTH case 16 (SHADOW)
// and case 17 (DARK, declared below) - there is no separate underlying
// value, so a THEME_COLOR_BUTTON_SHADOW const would just be a second name
// for THEME_COLOR_BUTTON_DARK with no distinct use.
const THEME_COLOR_BUTTON_DARK: i64 = 17;
const THEME_COLOR_BUTTON_TEXT: i64 = 18;
// Best-effort AA blend target for the rounded power-corner panel's outer
// edge, which sits over an arbitrary desktop wallpaper rather than a known
// app surface - see pwr_draw()'s own comment for why this is a bounded
// approximation, not a read-back.
const THEME_COLOR_DESKTOP_BG: i64 = 28;
const THEME_METRIC_CORNER_RADIUS: i64 = 7;
// #198v2: pwr_theme_color()/pwr_theme_metric() were removed here - the v2
// no-box glyph-on-photograph treatment is deliberately theme-INVARIANT (pure
// white glyph, pure black halo, per the owner's explicit request), so the
// power corner no longer reads theme button colors/radius at all. The
// THEME_COLOR_BUTTON_*/THEME_METRIC_CORNER_RADIUS constants above are left
// in place as documentation of the v1 design's now-retired dependency, not
// because anything still calls them.
// #198: bootlog line at the corner window's click handler, mirroring the
// exact precedent lockscreen.c ("compositor: Switch User from lock screen ->
// clean exit for login re-entry") and login.c ("[LOGIN] power: Restart from
// login gate") already use - a serial-visible proof the click handler was
// reached, independent of whether the action completes. SYS_BOOTLOG_WRITE
// (298) is already called raw elsewhere in this file (see the debug-log
// helper above); this just names the constant instead of spelling 298 twice.
const SYS_BOOTLOG_WRITE: i64 = 298;
fn bootlog(msg: &[u8]) { unsafe { syscall1(SYS_BOOTLOG_WRITE, msg.as_ptr() as i64); } }
// (#745) Present a repaint the APP started. Measured on VM <vmid>: the staged
// connection test ran to completion (diagnostic counters proved the state
// machine reached "reached the internet (HTTP 200)") while the SCREEN kept
// showing a frame from 256ms after the page opened. This wizard never called
// win_invalidate() anywhere, so an update not triggered by input reached the
// window's content buffer and stopped there; every visible update so far had
// been an accidental side effect of the compositor recompositing on input.
// SYS_WIN_INVALIDATE is exactly the app -> compositor "I already repainted,
// present it" direction (see sys_win_invalidate()'s #564 comment, which
// documents that it deliberately does NOT re-arm redraw_pending, so this
// cannot ping-pong).
const SYS_WIN_INVALIDATE: i64 = 37;
fn win_invalidate(h: i32) { unsafe { syscall1(SYS_WIN_INVALIDATE, h as i64); } }

fn win_create(t: &[u8], x: i32, y: i32, w: i32, h: i32) -> i32 {
    unsafe { syscall5(SYS_WIN_CREATE, t.as_ptr() as i64, x as i64, y as i64,
                      w as i64, h as i64) as i32 }
}
// #198: the power-corner window's own creation call. focus=0 (kernel/proc/
// syscall.c sys_win_create_impl's shared `focus` gate) - the SAME primitive
// aichat/main.c:952 and snapshot/main.c:1058 already use for a small
// non-focus-stealing floating control tied to one app's lifetime. This app
// (main.rs) issues syscalls directly rather than through libc/gui.h's
// win_create_bg() static inline, because a no_std Rust app cannot include
// that C header - see this file's own header comment on why every syscall
// number here is a private copy, not an include.
fn win_create_bg(t: &[u8], x: i32, y: i32, w: i32, h: i32) -> i32 {
    unsafe { syscall5(SYS_WIN_CREATE_BG, t.as_ptr() as i64, x as i64, y as i64,
                      w as i64, h as i64) as i32 }
}
fn win_destroy(h: i32) { unsafe { syscall1(SYS_WIN_DESTROY, h as i64); } }
fn rect(h: i32, x: i32, y: i32, w: i32, ht: i32, c: u32) {
    let (x, y) = body_to_win(x, y);
    unsafe { syscall6(SYS_WIN_DRAW_RECT, h as i64, x as i64, y as i64,
                      w as i64, ht as i64, c as i64); }
}
// #198: window-local rect draw for the power-corner window, which owns no
// header/footer/card chrome of its own - unlike rect() above, this must NOT
// apply body_to_win()'s card-origin offset (ORG_X/ORG_Y, CARD_X/CARD_Y are
// the MAIN wizard window's concepts and do not apply to a second window).
// Same pattern as draw_image_win() below for the same reason.
fn rect_win_local(h: i32, x: i32, y: i32, w: i32, ht: i32, c: u32) {
    unsafe { syscall6(SYS_WIN_DRAW_RECT, h as i64, x as i64, y as i64,
                      w as i64, ht as i64, c as i64); }
}
fn win_get_event(h: i32, ev: &mut GuiEvent, ms: i32) -> i32 {
    unsafe { syscall3(SYS_WIN_GET_EVENT, h as i64, ev as *mut GuiEvent as i64,
                      ms as i64) as i32 }
}
fn text(h: i32, x: i32, y: i32, s: &[u8], size: i32, color: u32) {
    let (x, y) = body_to_win(x, y);
    // SYS_WIN_DRAW_TTF packs the size into the top byte of the colour word.
    let packed = ((color & 0x00FF_FFFF) | (((size as u32) & 0xFF) << 24)) as i64;
    unsafe { syscall5(SYS_WIN_DRAW_TTF, h as i64, x as i64, y as i64,
                      s.as_ptr() as i64, packed); }
}

#[repr(C)]
struct GuiEvent {
    ty: u32, target_id: u32, mouse_x: i32, mouse_y: i32,
    mouse_buttons: u32, scroll_delta: i8, keycode: u32, key_char: u8,
}
const EV_KEY_DOWN: u32 = 5;      // verified against libc/gui.h event_type_t
const EV_MOUSE_DOWN: u32 = 2;
const EV_MOUSE_MOVE: u32 = 1;
const EV_REDRAW: u32 = 11;
const EV_MOUSE_SCROLL: u32 = 4;   // verified against libc/gui.h event_type_t
const EV_MOUSE_UP: u32 = 3;
// Arrow keycodes as this kernel DELIVERS them, verified against
// userland/apps/editor/main.c which works: 0x80 up, 0x81 down, 0x82 left,
// 0x83 right. I previously used the PC scancodes 0x48/0x50, which matched
// nothing - that is why every list was unreachable past its first viewport.
// #745: the kernel's password-policy result codes, MIRRORED from
// kernel/proc/pwpolicy.h (and libc/pwpolicy.h, which a Rust app cannot
// include). They are a wire format between Ring 0 and Ring 3, so they are not
// free to renumber here. The RULES are not mirrored - only the codes and the
// messages - because the policy is eight rules plus a 50,000-entry
// breached-password table that lives in the kernel image. A copy of the rules
// would drift from the kernel's and a copy of the table is not possible, so
// this app ASKS the kernel (SYS_PW_CHECK) and translates the answer.
const PW_ERR_SAME_AS_OTHER: i32 = 9;
// Policy-rejection return bands from SYS_FIRSTBOOT_ADMIN: -201..-209 says the
// ACCOUNT password broke a rule, -221..-229 says ROOT's did.
const PW_RC_BASE: i32 = 200;
const PW_RC_ROOT_BASE: i32 = 220;
const PW_ERR_LAST: i32 = 9;

fn pw_msg(code: i32) -> &'static [u8] {
    match code {
        1 => b"Password cannot be empty.\0",
        2 => b"Password contains a control character.\0",
        3 => b"Password must be at least 8 characters.\0",
        4 => b"Password is too long (127 characters maximum).\0",
        5 => b"Password must not contain your username.\0",
        6 => b"Password uses too few different characters.\0",
        7 => b"Password is a keyboard or counting sequence.\0",
        8 => b"That password appears in a known breached-password list.\0",
        9 => b"The root password must be different from your account password.\0",
        _ => b"That password was rejected.\0",
    }
}

fn pw_msg_root(code: i32) -> &'static [u8] {
    match code {
        1 => b"Root password cannot be empty.\0",
        2 => b"Root password contains a control character.\0",
        3 => b"Root password must be at least 8 characters.\0",
        4 => b"Root password is too long (127 characters maximum).\0",
        5 => b"Root password must not contain the word 'root'.\0",
        6 => b"Root password uses too few different characters.\0",
        7 => b"Root password is a keyboard or counting sequence.\0",
        8 => b"That root password is in a known breached-password list.\0",
        9 => b"The root password must be different from your account password.\0",
        _ => b"That root password was rejected.\0",
    }
}

// #745. Ask the kernel the SAME question it will answer again when it actually
// sets the password. This is a COURTESY for immediate feedback and never the
// enforcement point: the kernel re-checks at its own chokepoint whether or not
// anyone called this, and apply() still handles a kernel refusal.
//
// Returns a PW_ERR_* code (0 = acceptable), or a NEGATIVE value if the syscall
// is unavailable (an older kernel returns -1 for an unknown number). A negative
// must not block the user: fall back to the one rule this app can be sure of.
fn pw_policy_check(username: &[u8], pw: &Field) -> i32 {
    unsafe {
        syscall2(SYS_PW_CHECK, username.as_ptr() as i64, pw.cstr().as_ptr() as i64) as i32
    }
}

// F10 as this kernel DELIVERS it, read out of kernel/cpu/isr.c's special-key
// table (KEY_F10 = 0x87), the same table KC_UP..KC_RIGHT below come from. F10
// is chosen because it is not a printable character, so a text field cannot
// swallow it, and because nothing else in the kernel or the compositor claims
// it (F11 = 0x85 is the desktop's, F12 = 0x86 launches DOOM).
// #191: the number itself now comes from libc/keys.rs, the one table, rather
// than being restated here. The name and every call site are unchanged.
#[path = "../../libc/keys.rs"]
mod keys;
use keys::GUI_KEY_F10 as KC_F10;

// #136: same table (kernel/cpu/isr.c). Checked against
// userland/apps/compositor/main.c's global-shortcut dispatch before picking
// these: only 0x88 (F1, launcher) and 0x85 (F11, fullscreen) are intercepted
// there and never reach the focused app; F2/F3 are not claimed by anything
// else in the kernel or the compositor and pass straight through, same as
// F10 above. Deliberately NOT F10 itself or a new use of the word "Skip"
// alone: F10/self.skip() already means something different (finish with
// defaults, page_skippable() pages only).
//
// F9 (Skip to Desktop) used to live here too; removed per owner request,
// 2026-08-28 - see the #229 FIRST-RUN STATE comment above for the reasoning.
use keys::GUI_KEY_F2 as KC_F2;   // Restart
use keys::GUI_KEY_F3 as KC_F3;   // Shut Down

// #191: these four were already RIGHT (a comment further up records them being
// fixed off the 0x48/0x50 scancodes once before, which is why the wizard was
// the one app cleared rather than fixed). They are aliases now for the same
// reason the wrong ones spread: a private copy is the mechanism, correct or not.
use keys::GUI_KEY_UP as KC_UP;
use keys::GUI_KEY_DOWN as KC_DOWN;
use keys::GUI_KEY_LEFT as KC_LEFT;
use keys::GUI_KEY_RIGHT as KC_RIGHT;
const MOUSE_LEFT: u32 = 1;

// ---------------------------------------------------------------------------
// Palette (docs/OOBE_SPEC.md section 1). Both are fully specified; the wizard
// starts light and the Appearance page does NOT live-switch it, because the
// chosen theme is applied at Applying, not on selection.
// ---------------------------------------------------------------------------
struct Pal {
    bg: u32, surface: u32, field: u32, border: u32, border2: u32,
    text: u32, muted: u32, accent: u32, accent_h: u32, on_accent: u32,
    tint: u32, btn2: u32, track: u32, thumb: u32, error: u32,
    dis_bg: u32, dis_fg: u32,
}
const LIGHT: Pal = Pal {
    bg: 0xFFFFFF, surface: 0xF5F5F7, field: 0xFFFFFF, border: 0xD1D1D6,
    border2: 0xC1C1C6, text: 0x1D1D1F, muted: 0x6E6E73, accent: 0x007AFF,
    accent_h: 0x0070EB, on_accent: 0xFFFFFF, tint: 0xEAF2FF, btn2: 0xFFFFFF,
    track: 0xE5E5EA, thumb: 0xC7C7CC, error: 0xD70015,
    dis_bg: 0xE9E9EB, dis_fg: 0xAEAEB2,
};
const DARK: Pal = Pal {
    bg: 0x1E1E1E, surface: 0x2A2A2C, field: 0x1C1C1E, border: 0x3A3A3C,
    border2: 0x48484A, text: 0xF5F5F7, muted: 0x98989D, accent: 0x0A84FF,
    accent_h: 0x2B93FF, on_accent: 0xFFFFFF, tint: 0x1A2C42, btn2: 0x3A3A3C,
    track: 0x3A3A3C, thumb: 0x545456, error: 0xFF453A,
    dis_bg: 0x2C2C2E, dis_fg: 0x636366,
};

const W: i32 = 640;
const H: i32 = 480;

// ---------------------------------------------------------------------------
// Text fields. no_std with no allocator, so every buffer is fixed and every
// push is bounds-checked; there is no path here that can grow a buffer.
// ---------------------------------------------------------------------------
const FCAP: usize = 127;
#[derive(Clone, Copy)]
struct Field { b: [u8; 128], n: usize, mask: bool }
impl Field {
    const fn new(mask: bool) -> Self { Field { b: [0; 128], n: 0, mask } }
    fn push(&mut self, c: u8) { if self.n < FCAP { self.b[self.n] = c; self.n += 1; self.b[self.n] = 0; } }
    fn pop(&mut self) { if self.n > 0 { self.n -= 1; self.b[self.n] = 0; } }
    fn cstr(&self) -> &[u8] { &self.b }
    fn is_empty(&self) -> bool { self.n == 0 }
    // #745: "Set up later" discards the pages it skips, so a half-typed static
    // IP or AI key is never applied as though it had been confirmed.
    fn clear(&mut self) { self.b = [0; 128]; self.n = 0; }
    // #745 follow-up: load a NUL-terminated (or plain) byte slice as the
    // field's whole content, e.g. a provider's default model name. Stops at
    // the first embedded NUL so a literal like b"gpt-4.1\0" loads cleanly.
    fn set(&mut self, s: &[u8]) {
        self.clear();
        let mut i = 0;
        while i < s.len() && s[i] != 0 && self.n < FCAP { self.push(s[i]); i += 1; }
    }
    fn eq(&self, o: &Field) -> bool {
        if self.n != o.n { return false; }
        let mut i = 0; while i < self.n { if self.b[i] != o.b[i] { return false; } i += 1; } true
    }
}

// Pages
const PG_WELCOME: usize = 0;
const PG_ACCOUNT: usize = 1;
const PG_SIGNIN: usize = 2;
const PG_NETWORK: usize = 3;
const PG_TIME: usize = 4;
const PG_APPEAR: usize = 5;
const PG_WALL: usize = 6;
// #745 task #15: apps/widgets page, inserted between Desktop picture and AI
// so it can (a) read SYS_NET_STATUS truthfully - Network (page 3) has already
// run - and (b) forward-reference the API key the AI page sets up, rather
// than back-reference a step already passed. PG_AI/PG_APPLY/PG_DONE all
// shift up by one; every other reference in this file is by NAME, not
// number, so this is the only renumbering needed.
const PG_APPSW: usize = 7;
const PG_AI: usize = 8;
const PG_APPLY: usize = 9;
const PG_DONE: usize = 10;

// ---------------------------------------------------------------------------
// TEMPORARY SWITCH: the Network page is currently OFF (owner request,
// 2026-08-18, mid real-hardware test loop).
//
// TO TURN IT BACK ON: set NETWORK_PAGE_ENABLED to `true`. That is the ENTIRE
// revert; there is nothing else to undo. NOTHING was deleted: PG_NETWORK,
// dk_draw_network(), network_rule(), net_tick() and apply()'s
// /CONFIG/NETIP.CFG writer are all still present and still compiled, they are
// simply never reached while this is false.
//
// What `false` means, part by part:
//   - STEP MODEL. page_enabled() filters whichever flow list is current
//     (#126 gave the wizard two: STEP_PAGES and PERS_PAGES), so the
//     header reads "Step N of 8" instead of "of 9", the footer draws 8 dots,
//     and every page after Network renumbers itself. None of those three
//     numbers is written down anywhere, so they cannot disagree.
//   - NAVIGATION. next()/back() move via flow_next()/flow_prev(), which
//     step OVER a disabled page in BOTH directions. Continue on Signing in
//     lands on Date & Time; Back on Date & Time lands on Signing in. Neither
//     direction can reach the hidden page, so Back cannot fall into it.
//   - WHAT IT WOULD HAVE WRITTEN: NOTHING. apply() writes /CONFIG/NETIP.CFG
//     only when `!self.dhcp`, and `dhcp` starts `true` in App::new() and is
//     cleared ONLY by this page's own DHCP/static toggle (on_key / on_click,
//     both unreachable now). So with the page off the wizard makes no
//     statement about the network at all: whatever DHCP lease or static
//     configuration the machine already had survives untouched. That is the
//     safe side of #144 (the wizard overwriting a static IP configured after
//     it ran), not a new exposure. Nothing downstream requires NETIP.CFG to
//     exist: its absence is the ordinary "no static override, use DHCP"
//     state that every machine that never ran the wizard is already in.
//   - COSMETIC, LEFT ALONE DELIBERATELY: the PG_APPLY progress checklist
//     still shows a "Configure network" row and a "Configuring network..."
//     caption. That sub-step is not dead - it is also where the time zone is
//     written - and re-labelling it would be extra surface to revert. It is
//     noted here rather than silently changed.
// ---------------------------------------------------------------------------
const NETWORK_PAGE_ENABLED: bool = false;

// ---------------------------------------------------------------------------
// #OOBEAUTH (2026-08-23, supersedes the #229 version of this comment): CAN
// THIS SESSION ACT ON THE MACHINE?
// ---------------------------------------------------------------------------
// Set ONCE in main(), before the App exists, from geteuid() OR the kernel's
// FR_BOOTSTRAP_QUERY answer (whichever is true). Read-only afterwards, which
// is why a plain static is enough and why every reader goes through
// machine_admin().
//
// #229 SHIPPED THIS WIZARD WITH PG_ACCOUNT AND PG_SIGNIN PERMANENTLY HIDDEN
// FOR EVERY NON-ROOT SESSION, because on the images of the day the ONLY
// non-root session that could ever run this wizard was a NORMAL post-setup
// user, for whom both pages are correctly refused (PG_ACCOUNT would try to
// mint a second admin and rewrite root's password; PG_SIGNIN's gate demands
// proof of a password this app never collected). That reasoning is still
// correct for that caller - it is unchanged below.
//
// WHAT CHANGED IS THAT A SECOND KIND OF NON-ROOT SESSION NOW EXISTS: the
// FIRST-BOOT BOOTSTRAP session gui/login.c hands out when the account table
// is empty (owner decision 2026-08-23, replacing the kernel's OWN duplicate
// "Create your account" form with a handoff to THIS page). For that caller
// both pages are not merely safe but the reason the session exists:
// PG_ACCOUNT collects the fields SYS_FIRSTBOOT_ADMIN needs, and by the time
// apply() reaches PG_SIGNIN's SYS_SET_AUTOLOGIN / SYS_SET_LOGIN_MODE calls
// the account SYS_FIRSTBOOT_ADMIN just created is real, so login_cfg_
// authorize()'s ordinary non-root path (own account, proven password) admits
// them using the SAME self.pw this page collected - no elevated privilege
// needed for that second call at all, see apply() below.
//
// machine_admin() cannot tell these two non-root cases apart by asking
// geteuid() (both are uid 1000) or by asking anything about ITSELF - the
// distinguishing fact, "is the account table still empty and am I the
// process the compositor spawned for this", lives in the kernel's process
// tree and its user table, neither of which this app can see. So it ASKS:
// SYS_FIRSTRUN(FR_BOOTSTRAP_QUERY) runs the EXACT SAME predicate
// (firstboot_bootstrap_ok_rs(), kernel/rustkern/firstrun.rs) that
// sys_firstboot_admin() itself evaluates, so this page is shown if and only
// if its Continue button's syscall would actually succeed.
//
// SO THE PAGES ARE NOT SHOWN to a session with neither real root nor the
// bootstrap exception. Not greyed out, not shown-and-failed: removed from the
// flow, which the step model already knows how to do (page_enabled is the
// ONE predicate that the counter, the dots and both navigation directions all
// consult, so a page cannot end up hidden from the dots yet reachable with
// Back). Such a first run is Welcome, Date & time, Appearance, Desktop
// picture, Apps & widgets, AI - every step it CAN complete, and it completes
// all of them.
//
// THE PRIVILEGE BOUNDARY, one more time, because it matters: the bootstrap
// exception is not "this app is trusted", it is "this ONE process, while the
// account table is EMPTY, may make this ONE syscall succeed". It disappears
// the instant SYS_FIRSTBOOT_ADMIN succeeds (the table is no longer empty) and
// it was never reachable by any other process (kernel/rustkern/firstrun.rs
// checks the caller is a direct child of the compositor). See the long
// comment on firstboot_bootstrap_ok_rs() there for the full argument,
// including the honest cost: a virgin machine now runs this whole wizard,
// not a small kernel-drawn form, before any account exists.
static mut MACHINE_ADMIN: bool = false;

fn machine_admin() -> bool {
    unsafe { core::ptr::read_volatile(core::ptr::addr_of!(MACHINE_ADMIN)) }
}

/// Is this page part of the wizard right now? THE one predicate: the step
/// model and both navigation directions all ask it, so a page cannot end up
/// hidden from the dots yet still reachable with Back (or the reverse).
///
/// #229 made this a runtime question. It was `const fn` while the only input
/// was a compile-time switch; the session's euid is not one, and there is
/// deliberately still only ONE predicate rather than a second "and can I" test
/// bolted on at the call sites.
fn page_enabled(page: usize) -> bool {
    if page == PG_NETWORK && !NETWORK_PAGE_ENABLED { return false; }
    if (page == PG_ACCOUNT || page == PG_SIGNIN) && !machine_admin() { return false; }
    true
}

// #210 MERGE NOTE (agent/session126 -> dev): page_after()/page_before() lived
// here and walked page INDICES, which only worked while there was exactly one
// flow whose pages were consecutive. #126 gave the wizard two flows, so the
// walkers now step through the CURRENT FLOW'S LIST instead - flow_next() and
// flow_prev(), further down, next to flow() itself. They ask this same
// page_enabled() predicate, so the property those two functions existed to
// guarantee is unchanged: a page cannot be hidden from the dots yet still
// reachable with Back. They were DELETED rather than left in place, because a
// second navigation helper that no longer agrees with the flow model is the
// next person's bug.

// PG_WALL grid geometry. ONE definition, because the draw loop, the
// arrow-key/wheel pager and the click hit-test all have to agree about how
// many cells a page holds; three literal 10s (now 20s) drifting apart is
// how a pager ends up skipping or repeating a row.
//
// #745 task #38 (user-reported 2026-08-12): 5 columns x 4 rows, and the
// "Browse more..." link is REMOVED - Up/Down/wheel (list_move) already page
// the grid, so the link duplicated an existing control rather than adding
// one. The position readout STAYS: 64 wallpapers ship and up to 44 remain
// off-page even at 20 cells/page, so removing the readout too would leave
// no sign more exist.
const WALL_PAGE: usize = 20;      // 5 columns x 4 rows of thumbnails
const WALL_COLS: usize = 5;
const WALL_ROWS: usize = 4;       // WALL_PAGE / WALL_COLS
const WALL_X0: i32 = 32;
const WALL_Y0: i32 = 84;
const WALL_PITCH_X: i32 = 112;
const WALL_PITCH_Y: i32 = 74;     // rows land at 84 / 158 / 232 / 306
const WALL_READOUT_Y: i32 = 380;  // below row 4 (306 + 62 tall cell + gap)

// #49/#50 (2026-08-12): the wizard's own TZ[] (35 entries) and its
// index-for-index TZ_OFF_MIN[] have been DELETED. They were the better of the
// two lists in the tree, so they were not thrown away: they were MOVED into
// userland/libc/tz.c, where Settings now reads the same array and every clock
// in the OS reads the same setting. The three-hardcoded-index problem the old
// comment here described is gone with them, because nothing persists an index
// any more (TZ.CFG stores the zone ID STRING) and the one in-code default now
// comes from tz_index_utc() rather than a literal 12.
//
// The shared list is sorted by ascending offset, unlike the old appended tail,
// so a picker can render it top to bottom.

// The zone ID as this file used to spell it: NUL-TERMINATED bytes, exactly the
// shape TZ[i] had, so every call site below reads unchanged. The pointer is
// into libc's static ZONES[], so 'static is honest.
fn tz_id_bytes(i: usize) -> &'static [u8] {
    unsafe {
        let p = tz_id_c(i as i32);
        let mut n = 0usize;
        while *p.add(n) != 0 { n += 1; }
        core::slice::from_raw_parts(p, n + 1)   // include the NUL, as TZ[] did
    }
}
fn tz_len() -> usize { unsafe { tz_count() as usize } }
fn tz_utc_idx() -> usize { unsafe { tz_index_utc() as usize } }

// ---------------------------------------------------------------------------
// World-map city picker (docs/OOBE_TIME_APPEARANCE.html section 2/4,
// userland/apps/setup/assets/TZCITIES_README.md). TZCITIES.DAT and
// WORLDMAP.BMP are already-shipped assets (#tzassets, commit 8fc0ea4); this
// is the wizard-side reader/drawer that finally wires them into the page.
// ---------------------------------------------------------------------------
// #745 (2026-08-12): was 48, raised to 64. TZCITIES.DAT now ships 51 city
// rows (39 original + Brisbane/Melbourne + 10 for the 9 newly-appended
// TZ_OFF_MIN offsets, with two cities at +09:30 to show a real same-offset
// DST divergence) - 48 would silently truncate the last 3 rows read by
// tzcities_load() (it clamps via `.min(TZC_CAP)`, not an error path).
const TZC_CAP: usize = 64;

#[derive(Clone, Copy)]
struct TzCity {
    name: [u8; 32], country: [u8; 24], tz_id: [u8; 32],
    utc_off_min: i16, dst: u8, lat_e2: i16, lon_e2: i16,
}
const TZCITY_ZERO: TzCity = TzCity {
    name: [0; 32], country: [0; 24], tz_id: [0; 32],
    utc_off_min: 0, dst: 0, lat_e2: 0, lon_e2: 0,
};
// .bss, not the stack - see THUMBS/WEL_LOGO_RAW above and their shared
// commit-message reasoning (an ~11KB App on a 16KB user stack killed PID 25
// on build 1768).
static mut TZC: [TzCity; TZC_CAP] = [TZCITY_ZERO; TZC_CAP];
static mut TZC_COUNT: usize = 0;
static mut TZC_FILT: [u8; TZC_CAP] = [0; TZC_CAP];   // indices into TZC
static mut TZC_FILT_COUNT: usize = 0;
const TZCITIES_RAW_CAP: usize = 8192;   // file is 16 + 39*100 = 3916 bytes
static mut TZCITIES_RAW: [u8; TZCITIES_RAW_CAP] = [0; TZCITIES_RAW_CAP];

const MAP_X: i32 = 32;
const MAP_Y: i32 = 86;
const MAP_W: i32 = 352;
const MAP_H: i32 = 138;
const MAP_PX_BYTES: usize = 352 * 138 * 4;
// WORLDMAP.BMP is 352x138, 24-bit uncompressed BI_RGB, 145782 bytes on disk
// (the spec doc's "8-bit indexed" text does not match the actual shipped
// asset - the kernel decoder hard-rejects anything but 24/32bpp, so this
// reads the real format, not the doc's description of it).
static mut MAP_RAW: [u8; 180000] = [0; 180000];
static mut MAP_PX: [u8; MAP_PX_BYTES] = [0; MAP_PX_BYTES];
static mut MAP_OK: bool = false;

const NTP_PRESETS: [&[u8]; 3] =
    [b"pool.ntp.org\0", b"time.google.com\0", b"time.cloudflare.com\0"];

fn rd_u16le(b: &[u8], off: usize) -> u16 { (b[off] as u16) | ((b[off + 1] as u16) << 8) }
fn rd_i16le(b: &[u8], off: usize) -> i16 { rd_u16le(b, off) as i16 }

fn str_trim(s: &[u8]) -> &[u8] {
    let mut n = 0usize;
    while n < s.len() && s[n] != 0 { n += 1; }
    &s[0..n]
}
fn lower(c: u8) -> u8 { if c >= b'A' && c <= b'Z' { c + 32 } else { c } }
fn str_contains_ci(hay: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() { return true; }
    if needle.len() > hay.len() { return false; }
    let mut i = 0usize;
    while i + needle.len() <= hay.len() {
        let mut j = 0usize; let mut ok = true;
        while j < needle.len() { if lower(hay[i + j]) != lower(needle[j]) { ok = false; break; } j += 1; }
        if ok { return true; }
        i += 1;
    }
    false
}
fn wrap_range(v: i32, lo: i32, hi: i32) -> i32 {
    let span = hi - lo + 1;
    if span <= 0 { return lo; }
    let mut x = (v - lo) % span;
    if x < 0 { x += span; }
    lo + x
}
fn clampi(v: i32, lo: i32, hi: i32) -> i32 { if v < lo { lo } else if v > hi { hi } else { v } }
fn is_leap(y: i32) -> bool { (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0) }
fn days_in_month(m: i32, y: i32) -> i32 {
    match m {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        2 => if is_leap(y) { 29 } else { 28 },
        _ => 31,
    }
}
fn fmt_fixed(buf: &mut [u8], width: usize, v: i32) -> usize {
    let mut x = v.max(0);
    let mut i = width;
    while i > 0 { i -= 1; buf[i] = b'0' + (x % 10) as u8; x /= 10; }
    width
}
fn fmt_utc_offset(dst: &mut [u8], at: usize, off_min: i32) -> usize {
    let neg = off_min < 0;
    let a = off_min.abs();
    let hh = a / 60; let mm = a % 60;
    let mut n = 0usize;
    dst[at] = if neg { b'-' } else { b'+' }; n += 1;
    n += fmt_fixed(&mut dst[at + n..], 2, hh);
    dst[at + n] = b':'; n += 1;
    n += fmt_fixed(&mut dst[at + n..], 2, mm);
    n
}
fn tz_marker_xy(lat_e2: i32, lon_e2: i32) -> (i32, i32) {
    let mx = MAP_X + (lon_e2 + 18000) * MAP_W / 36000;
    let my = MAP_Y + (9000 - lat_e2) * MAP_H / 18000;
    (mx, my)
}
// #49/#50: a name-only shim over libc's tz_index_for_offset(). The bound bug
// this function used to carry (a literal 26 that made every appended offset
// unreachable) cannot recur, because there is no local array left to be out of
// step with: the search happens inside the one list.
fn tz_index_for_offset(off_min: i32) -> usize {
    let i = unsafe { tz_index_for_offset_c(off_min) };
    if i < 0 { tz_utc_idx() } else { i as usize }
}

// Reads /OOBE/TZCITIES.DAT (userland/apps/setup/assets/TZCITIES_README.md -
// a fixed 100-byte packed record format). Deliberately NOT #[repr(C)]-cast
// onto the raw buffer: Rust's natural struct layout would insert padding
// that does not match the file's packed layout, so every field is read by
// explicit byte offset instead, exactly as the README's own "why this
// format" section describes. Never panics: any failure (missing file, bad
// magic, truncated read) leaves TZC_COUNT at 0 and the caller degrades per
// the documented fallback - the search/list panel is never blocked on this.
fn tzcities_load() -> usize {
    unsafe {
        TZC_COUNT = 0;
        let fd = syscall3(SYS_OPEN, b"/OOBE/TZCITIES.DAT\0".as_ptr() as i64, 0, 0) as i32;
        if fd < 0 { return 0; }
        let got = syscall3(SYS_READ, fd as i64,
                           core::ptr::addr_of_mut!(TZCITIES_RAW) as i64, TZCITIES_RAW_CAP as i64);
        syscall1(SYS_CLOSE, fd as i64);
        if got < 16 { return 0; }
        let buf = &*core::ptr::addr_of!(TZCITIES_RAW);
        if !(buf[0] == b'T' && buf[1] == b'Z' && buf[2] == b'C' && buf[3] == b'1') { return 0; }
        let rec_size = rd_u16le(buf, 4) as usize;
        if rec_size != 100 { return 0; }
        let rec_count = rd_u16le(buf, 6) as usize;
        let avail = (got as usize).saturating_sub(16);
        let n = rec_count.min(TZC_CAP).min(avail / rec_size);
        let mut i = 0usize;
        while i < n {
            let base = 16 + i * rec_size;
            let mut c = TZCITY_ZERO;
            let mut k = 0usize; while k < 32 { c.name[k] = buf[base + k]; k += 1; }
            k = 0; while k < 24 { c.country[k] = buf[base + 32 + k]; k += 1; }
            k = 0; while k < 32 { c.tz_id[k] = buf[base + 56 + k]; k += 1; }
            c.utc_off_min = rd_i16le(buf, base + 88);
            c.dst = buf[base + 90];
            c.lat_e2 = rd_i16le(buf, base + 92);
            c.lon_e2 = rd_i16le(buf, base + 94);
            TZC[i] = c;
            i += 1;
        }
        TZC_COUNT = n;
        n
    }
}

// Reads /OOBE/WORLDMAP.BMP exactly like wel_draw_logo() reads LOGOMARK.BMP:
// open, read raw, SYS_DECODE_IMAGE at 1:1 (no scaling), leave the decoded
// pixels in MAP_PX for dk_draw_time() to blit. Sets MAP_OK so the caller can
// fall back to a flat panel + fine-print line on any failure (a previous
// OOBE asset silently failed to ship once already - this is an expected
// failure mode, not a hypothetical).
fn map_load() {
    unsafe {
        MAP_OK = false;
        let fd = syscall3(SYS_OPEN, b"/OOBE/WORLDMAP.BMP\0".as_ptr() as i64, 0, 0) as i32;
        if fd < 0 { return; }
        let got = syscall3(SYS_READ, fd as i64,
                           core::ptr::addr_of_mut!(MAP_RAW) as i64, 180000);
        syscall1(SYS_CLOSE, fd as i64);
        if got <= 0 { return; }
        let target = ((MAP_W as u32) << 16) | (MAP_H as u32 & 0xFFFF);
        let mut dims: [i32; 2] = [0, 0];
        let r = syscall6(SYS_DECODE_IMAGE, core::ptr::addr_of!(MAP_RAW) as i64, got,
                         target as i64, core::ptr::addr_of_mut!(MAP_PX) as i64,
                         MAP_PX_BYTES as i64, dims.as_mut_ptr() as i64);
        MAP_OK = r >= 0;
    }
}

// Rebuilds TZC_FILT/TZC_FILT_COUNT from TZC[0..TZC_COUNT], matching `search`
// (case-insensitive substring) against the trimmed name OR country; an empty
// search matches everything, in original TZC order.
fn tz_filter_refresh(search: &Field) {
    unsafe {
        TZC_FILT_COUNT = 0;
        let needle = &search.b[0..search.n];
        let mut i = 0usize;
        while i < TZC_COUNT {
            let name = str_trim(&TZC[i].name);
            let country = str_trim(&TZC[i].country);
            let m = search.n == 0 || str_contains_ci(name, needle) || str_contains_ci(country, needle);
            if m && TZC_FILT_COUNT < TZC_CAP { TZC_FILT[TZC_FILT_COUNT] = i as u8; TZC_FILT_COUNT += 1; }
            i += 1;
        }
    }
}

// Appends "dock_style: N\n" to /UIPROFIL.YML. root's home during OOBE is
// literally "/" (see compositor/profile.c prof_path()), which is ALSO
// profile_load()'s own hardcoded fallback path - so this is the correct
// place for OOBE to seed a machine default that a freshly-created user's
// first compositor session will read via that same fallback. Deliberately
// does NOT go through userconf_open_write()/userconf_path(), which always
// prepend <home>/CONFIG/ - the WRONG path here, since nothing reads
// /CONFIG/UIPROFIL.YML. Reads any existing file first (raw SYS_OPEN/
// SYS_READ, same pattern as thumbs_load()) so an existing profile survives;
// profile.c's parser applies keys in file order, so a later duplicate
// "dock_style:" line simply wins and no de-duplication is needed.
// Live-apply channel (compositor/main.c dock_style_write_cfg()'s own
// recipe): a single ASCII digit written to DOCKSTYL.CFG (per-user path via
// userconf_open_write, same call Settings itself uses), polled by the
// running compositor every 10 ticks and applied immediately
// (taskbar_set_style()), which is also what makes the NEXT profile_tick()
// persist it correctly instead of racing a raw file edit.
fn write_dock_style_live(dock_style: usize) {
    unsafe {
        let fd = userconf_open_write(b"DOCKSTYL.CFG\0".as_ptr());
        if fd < 0 { return; }
        let c: [u8; 1] = [b'0' + (dock_style.min(9) as u8)];
        let _ = userconf_finish_write(fd, c.as_ptr(), 1);
    }
}

// (#wizdock) Mirrors compositor/compositor.h's DOCK_XFCE = 4 ("Marble" in
// Settings). A plain literal, not shared via a generated header - the same
// distance-from-source tradeoff self.dock_style's whole 0..DOCK_COUNT range
// already accepts elsewhere in this file (KC_LEFT/KC_RIGHT clamp against a
// literal 4 too). If DOCK_COUNT ever changes, both need revisiting; grep for
// DOCK_XFCE_IDX.
const DOCK_XFCE_IDX: usize = 4;

// (#wizdock) Owner decision 2026-08-27: Fluent Dark is the default theme,
// matching the new Marble default dock style (write_dock_style() above).
// THEMES[] is populated at runtime from /THEMES/*.mtheme (gui_theme_list()),
// so "the default theme" cannot be a compiled-in array index the way
// DOCK_XFCE is for dock style - the same theme can load at a different
// index depending on what is on disk. Looked up by SLUG instead, once
// THEMES[]/nthemes are populated, and reused at both places self.theme gets
// defaulted (initial construction and the back-navigation reset) so the two
// cannot drift the way a hand-duplicated index would.
fn default_theme_idx(themes: &[ThemeEntry], nthemes: usize) -> usize {
    const WANT: &[u8] = b"fluent_dark";
    let mut i = 0usize;
    while i < nthemes {
        let slug = &themes[i].slug;
        let mut k = 0usize;
        let mut same = true;
        while k < WANT.len() {
            if slug[k] != WANT[k] { same = false; break; }
            k += 1;
        }
        if same && (k >= slug.len() || slug[k] == 0) { return i; }
        i += 1;
    }
    0
}

fn write_dock_style(dock_style: usize) {
    unsafe {
        let mut buf: [u8; 2048] = [0; 2048];
        let mut n = 0usize;
        let fd = syscall3(SYS_OPEN, b"/UIPROFIL.YML\0".as_ptr() as i64, 0, 0) as i32;
        if fd >= 0 {
            let got = syscall3(SYS_READ, fd as i64, buf.as_mut_ptr() as i64, 2048);
            syscall1(SYS_CLOSE, fd as i64);
            if got > 0 { n = (got as usize).min(2048); }
        }
        if n > 0 && buf[n - 1] != b'\n' && n < 2048 { buf[n] = b'\n'; n += 1; }
        let line = b"dock_style: ";
        let mut k = 0usize;
        while k < line.len() && n < 2040 { buf[n] = line[k]; n += 1; k += 1; }
        let mut tmp: [u8; 4] = [0; 4];
        let mut tn = 0usize;
        let mut v = dock_style as i32;
        if v <= 0 { tmp[0] = b'0'; tn = 1; }
        else { while v > 0 && tn < 4 { tmp[tn] = b'0' + (v % 10) as u8; v /= 10; tn += 1; } }
        while tn > 0 { tn -= 1; if n < 2046 { buf[n] = tmp[tn]; n += 1; } }
        if n < 2047 { buf[n] = b'\n'; n += 1; }
        let _ = userconf_write_all(b"/UIPROFIL.YML\0".as_ptr(), buf.as_ptr(), n as u64);
    }
}

// .bss, not the stack. See the commit message: an ~11KB App on a 16KB user
// stack is what killed PID 25 on build 1768.
static mut THEMES: [ThemeEntry; 32] = [ThemeEntry {
    slug: [0; THEME_SLUG_MAX], name: [0; THEME_NAME_MAX],
    is_dark: 0, is_classic: 0, index: 0 }; 32];
const THUMB_W: usize = 100;
const THUMB_H: usize = 62;
const THUMB_PX: usize = THUMB_W * THUMB_H;      // 6200 pixels
const THUMB_CELLS: usize = 20;                  // one screenful of the grid

// Decoded thumbnails for the CURRENTLY VISIBLE cells only. Decoded when the
// page opens or the grid scrolls - never per frame, and never on the draw path
// for an image that is not already on disk. The files are pre-generated at
// build time (/WPTHUMB), so this is a 100x62 read, not a full-res decode.
static mut THUMBS: [u32; THUMB_PX * THUMB_CELLS] = [0; THUMB_PX * THUMB_CELLS];
static mut THUMB_OK: [bool; THUMB_CELLS] = [false; THUMB_CELLS];
static mut THUMB_BASE: usize = usize::MAX;

static mut WALLS: [WpEntry; 96] = [WpEntry {
    file: [0; WP_FILE_MAX], name: [0; WP_NAME_MAX] }; 96];

struct App {
    win: i32,
    p: &'static Pal,
    page: usize,
    focus: usize,
    nfields: usize,
    hover_nav: i32,          // 0 none, 1 back, 2 next
    dragging: bool,          // scrollbar thumb held
    err: &'static [u8],
    // #745: which input the current error refers to, so it is drawn under
    // THAT field. Errors used to be pinned at y=178, under the PASSWORD
    // row, so "Enter your full name" accused the wrong control. -1 = none.
    err_focus: i32,
    // choices
    require_pw: bool,
    // #745 PG_SIGNIN, second decision: true = show a list of account names,
    // false = ask for a name and password. DEFAULT true (a recommendation to
    // somebody who is present and can change it); the FALLBACK when the
    // machine cannot read what was decided is the OPPOSITE, typed, and lives
    // in the kernel. Those answer two different questions and are allowed to
    // differ: a disclosure follows a recorded decision, never a missing file.
    mode_list: bool,
    // Keyboard position on PG_SIGNIN: 0 startup group, 1 sign-in-screen
    // group, 2 Back, 3 Continue. A GROUP-level position, because selection
    // commits on arrow, so the radios themselves cannot show where the
    // keyboard is; the group frame does.
    signin_focus: usize,
    dhcp: bool,
    tz: usize,
    tz_first: usize,
    theme: usize,
    theme_first: usize,
    wall: usize,
    wall_first: usize,
    // fields
    fullname: Field, username: Field, pw: Field, pw2: Field,
    // #745: root's own password, confirmed. Confirmed because a typo here is
    // not recoverable on this machine: MayteraOS has no sudo, so the only way
    // to be root is to sign in as root.
    rootpw: Field, rootpw2: Field,
    ip: Field, mask: Field, gw: Field, dns: Field, aikey: Field,
    // #745 follow-up: AI provider page (docs/OOBE_AI_PROVIDER.html).
    ai_provider: usize, ai_model: Field, ai_endpoint: Field,
    ai_model_touched: bool, ai_focus: usize,
    ai_key_saved: bool, ai_checked_saved: bool,
    // enumerated (data lives in THEMES/WALLS statics; only the counts here)
    nthemes: usize,
    nwalls: usize,
    // apply
    substep: i32,
    apply_err: &'static [u8],
    // welcome page (PG_WELCOME) hit-test rect, set by draw_welcome(). The
    // second rect here was the "Set up later" link's; #745 removed the link
    // and this field with it, rather than leaving a rect nothing draws.
    wel_btn: (i32, i32, i32, i32),
    // #745: set when "Set up later" was pressed at or before Date & Time, so
    // apply() leaves the RTC alone instead of writing this wizard's untouched
    // defaults over it and presenting that as the user's choice.
    clock_skip: bool,
    // #126 (reduced flow only): the page at which Skip was pressed. apply()
    // writes nothing belonging to this page or any page after it, so a skip
    // preserves the settings the user already had instead of overwriting them
    // with list-index 0. usize::MAX = nothing was skipped.
    pers_skip_from: usize,
    // PG_TIME: world-map city picker + manual/NTP clock (docs/
    // OOBE_TIME_APPEARANCE.html section 2).
    tzc_sel: usize,
    tzc_first: usize,
    tz_search: Field,
    time_focus: usize,   // 0 search,1 list,2 ntp-toggle,3..8 spinners,9 ntp combo
    ntp_on: bool,
    ntp_server: Field,
    ntp_preset: usize,
    dt_year: i32, dt_month: i32, dt_day: i32, dt_hour: i32, dt_min: i32, dt_sec: i32,
    // PG_APPEAR: dock-style picker (section 3).
    dock_style: usize,
    appear_zone: usize,   // 0 theme grid focused, 1 dock-style row focused
    // #745 task #15, PG_APPSW (docs/OOBE_APPS_WIDGETS.html). Bitsets rather
    // than [bool;N]: 12 and 15 candidates both fit a u16, count_ones() gives
    // the "n of 12/15" counters for free, and a bitset cannot go out of
    // bounds the way a hand-sized bool array can.
    apps_sel: u16,          // bit i = APPS_UI[i] checked (dock pin)
    apps_focus: usize,      // 0..11, row-major (matches dock/grid order)
    widgets_sel: u16,       // bit i = WIDGETS_UI[i] on
    widgets_cursor: usize,  // 0..14, PERSISTS regardless of zone (spec 6.1)
    appsw_zone: usize,      // 0 apps grid, 1 widgets list, 2 Back, 3 Continue
    // Did the person change ANY widget checkbox this visit? Gates whether
    // apply() writes WIDGETCH.CFG at all (spec 9.3: "nothing touched" must
    // write nothing, but "touched down to zero" must write all fifteen 0s -
    // those are different states and only a touched flag tells them apart).
    appsw_widgets_touched: bool,
}

// ---------------------------------------------------------------------------
// Primitives built from the spec's construction rules
// ---------------------------------------------------------------------------
impl App {
    fn hairline(&self, x: i32, y: i32, w: i32, c: u32) { rect(self.win, x, y, w, 1, c); }

    fn frame(&self, x: i32, y: i32, w: i32, h: i32, t: i32, c: u32) {
        // Borders are drawn INWARD so a focus/error state never shifts geometry
        // (spec section 3). This is why the field's outer rect is constant.
        rect(self.win, x, y, w, t, c);
        rect(self.win, x, y + h - t, w, t, c);
        rect(self.win, x, y, t, h, c);
        rect(self.win, x + w - t, y, t, h, c);
    }

    fn field(&self, f: &Field, x: i32, y: i32, w: i32, focused: bool, err: bool) {
        let p = self.p;
        gui_rr(self.win, x, y, w, 32, 6, p.field, p.bg);
        if err { self.frame(x, y, w, 32, 2, p.error); }
        else if focused { self.frame(x, y, w, 32, 2, p.accent); }
        else { self.frame(x, y, w, 32, 1, p.border2); }

        if f.mask {
            // 6px discs at 10px pitch, drawn as circles rather than font
            // glyphs so the mask never depends on the TTF having a bullet.
            let mut i = 0;
            while i < f.n && i < 32 {
                unsafe { gui_fill_circle_aa(self.win, x + 13 + (i as i32) * 10, y + 16, 3, p.text, p.field); }
                i += 1;
            }
        } else if f.n > 0 {
            text(self.win, x + 10, y + 8, f.cstr(), 13, p.text);
        }
        if focused {
            let cw = if f.mask { (f.n as i32) * 10 } else {
                unsafe { gui_ttf_width(f.cstr().as_ptr(), 13) }
            };
            rect(self.win, x + 10 + cw + 1, y + 8, 1, 16, p.text);
        }
    }

    fn label(&self, x: i32, y: i32, s: &[u8]) { text(self.win, x, y, s, 12, self.p.muted); }

    // Choice row per spec section 3.
    fn choice(&self, x: i32, y: i32, w: i32, h: i32, title: &[u8], sub: &[u8],
              sub2: &[u8], selected: bool, recommended: bool) {
        let p = self.p;
        if selected {
            gui_rr(self.win, x, y, w, h, 6, p.tint, p.bg);
            self.frame(x, y, w, h, 2, p.accent);
        } else {
            self.frame(x, y, w, h, 1, p.border2);
        }
        let ry = if h >= 76 { y + 14 } else { y + 19 };
        let radio_cy = ry + 9;
        // radio: off = 2px border2 ring; on = 2px accent ring + 8px accent disc
        unsafe {
            gui_fill_circle_aa(self.win, x + 25, radio_cy, 9,
                               if selected { p.accent } else { p.border2 },
                               if selected { p.tint } else { p.bg });
            gui_fill_circle_aa(self.win, x + 25, radio_cy, 7,
                               if selected { p.tint } else { p.bg },
                               if selected { p.accent } else { p.border2 });
            if selected { gui_fill_circle_aa(self.win, x + 25, radio_cy, 4, p.accent, p.tint); }
        }
        let ty = if h >= 76 { y + 14 } else { y + 11 };
        text(self.win, x + 46, ty, title, 13, p.text);
        if recommended {
            let tw = unsafe { gui_ttf_width(title.as_ptr(), 13) };
            text(self.win, x + 46 + tw + 8, ty + 1, b"Recommended\0", 11, p.accent);
        }
        let sy = if h >= 76 { y + 36 } else { y + 29 };
        if !sub.is_empty() { text(self.win, x + 46, sy, sub, 12, p.muted); }
        if !sub2.is_empty() { text(self.win, x + 46, sy + 16, sub2, 12, p.muted); }
    }

    // Compact single-select list (time zone).
    fn list_compact(&self, x: i32, y: i32, w: i32, h: i32, count: usize,
                    sel: usize, first: usize, name_of: &dyn Fn(usize) -> &'static [u8]) {
        let p = self.p;
        gui_rr(self.win, x, y, w, h, 6, p.field, p.bg);
        self.frame(x, y, w, h, 1, p.border2);
        let rows = ((h - 4) / 28) as usize;
        let mut i = 0;
        while i < rows && first + i < count {
            let idx = first + i;
            let ry = y + 2 + (i as i32) * 28;
            if idx == sel {
                rect(self.win, x + 1, ry, w - 2, 28, p.accent);
                text(self.win, x + 12, ry + 6, name_of(idx), 13, p.on_accent);
            } else {
                text(self.win, x + 12, ry + 6, name_of(idx), 13, p.text);
            }
            i += 1;
        }
        // Scrollbar: track w=6 inset 2px, thumb length proportional, min 24.
        if count > rows {
            let tx = x + w - 8;
            let tt = y + 2;
            let tl = h - 4;
            gui_rr(self.win, tx, tt, 6, tl, 3, p.track, p.field);
            let mut len = tl * (rows as i32) / (count as i32);
            if len < 24 { len = 24; }
            let top = tt + (tl - len) * (first as i32) / ((count - rows) as i32).max(1);
            gui_rr(self.win, tx, top, 6, len, 3, p.thumb, p.track);
        }
    }

    fn button_dis(&self, x: i32, y: i32, w: i32, label: &[u8]) {
        // Explicit disabled rendering: dis_bg fill, dis_fg label, border hairline.
        let p = self.p;
        gui_rr(self.win, x, y, w, 32, 6, p.dis_bg, p.surface);
        self.frame(x, y, w, 32, 1, p.border);
        let tw = unsafe { gui_ttf_width(label.as_ptr(), 13) };
        text(self.win, x + (w - tw) / 2, y + 8, label, 13, p.dis_fg);
    }

    fn button2(&self, x: i32, y: i32, w: i32, label: &[u8], hover: bool) {
        // Secondary face uses btn2 explicitly so the token is not decorative.
        let p = self.p;
        gui_rr(self.win, x, y, w, 32, 6, if hover { p.surface } else { p.btn2 }, p.surface);
        self.frame(x, y, w, 32, 1, p.border2);
        let tw = unsafe { gui_ttf_width(label.as_ptr(), 13) };
        text(self.win, x + (w - tw) / 2, y + 8, label, 13, p.text);
    }

    fn button(&self, x: i32, y: i32, w: i32, label: &[u8], primary: bool, hover: bool) {
        let st = if hover { GUI_ST_HOVER } else { GUI_ST_NORMAL };
        let v = if primary { GUI_BTN_PRIMARY } else { GUI_BTN_SECONDARY };
        unsafe { gui_button(self.win, x, y, w, 32, label.as_ptr(), v, st); }
    }

    fn step_dots(&self) {
        // 8 dots, d=8, centres x = 257 + 18k, cy = 449. Current dot gets a ring
        // at 14px diameter; done dots are filled; upcoming are border2.
        let p = self.p;
        let cur = self.page as i32 - 1;      // Account = step 1 -> dot 0
        let mut k = 0;
        while k < 8 {
            let cx = 257 + k * 18;
            let c = if k < cur { p.accent } else if k == cur { p.accent } else { p.border2 };
            unsafe {
                if k == cur { gui_fill_circle_aa(self.win, cx, 449, 7, p.surface, p.surface);
                              gui_fill_circle_aa(self.win, cx, 449, 7, p.accent, p.surface);
                              gui_fill_circle_aa(self.win, cx, 449, 5, p.surface, p.accent); }
                gui_fill_circle_aa(self.win, cx, 449, 4, c, p.surface);
            }
            k += 1;
        }
    }
}

fn gui_rr(h: i32, x: i32, y: i32, w: i32, ht: i32, r: i32, fill: u32, outer: u32) {
    unsafe { gui_fill_rounded_aa(h, x, y, w, ht, r, fill, outer); }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Welcome screen (#745 redesign, re-integrated as page 0 of THIS window).
//
// Originally this screen was its own separate full-screen borderless window,
// created and destroyed by run_welcome_screen() BEFORE the chromed wizard
// window existed. That produced a visible bug reported directly by the
// user: "the first step of the wizard is full screen, then suddenly we're
// back to the desktop" - destroying that window uncovered the desktop for a
// compositor frame or more before the second (chromed) window was created
// and painted. The fix is structural, not cosmetic: there is now only ONE
// window for the entire wizard, created once in main() and destroyed once
// at Finish. The welcome screen is PG_WELCOME (page 0) of that same window,
// drawn by App::draw_welcome() below, and the window's chrome is set once,
// immediately after creation, before the first draw, and never touched
// again for the rest of the wizard's life (see win_set_nochrome()'s
// one-way-flag note at its call site in main()).
//
// Geometry below is the docs/OOBE_WELCOME_REDESIGN.html table (authored at
// 1024x768) scaled by exactly 640/1024 = 480/768 = 0.625: the wizard window
// is 4:3 at 640x480, the same aspect as the spec canvas, so one uniform
// scale factor keeps every proportion (logo/headline/button/dots spacing)
// intact with no separate x/y factors. Font sizes were scaled the same way
// and then hand-adjusted UP where the strict 0.625 value fell below the
// legibility floor this app already uses elsewhere (12-13px body text, 11px
// for the smallest existing label, "Recommended" on PG_SIGNIN) - see the
// comment at each wel_text_tracked() call below for the number actually
// used and why it differs from the pure scale.
// ---------------------------------------------------------------------------
const SYS_FB_INFO: i64 = 201;             // verified against libc/syscall.h
const SYS_WIN_SET_NOCHROME: i64 = 262;    // verified against libc/syscall.h (#185)
// #wizfocus: nochrome WITHOUT stealing keyboard focus, the counterpart of
// win_create_bg() above - see fn pwr_create()'s call for why the corner
// window needs this and not win_set_nochrome(). Number verified against
// libc/syscall.h (398) and kernel/proc/syscall.h (#216).
const SYS_WIN_SET_NOCHROME_BG: i64 = 398;
// #198v2: real per-pixel window content alpha (top byte of every BGRA
// word), blended by the compositor against the LIVE framebuffer at blit
// time. See WINDOW_FLAG_ALPHA_CONTENT (kernel/gui/window.h) and
// pwr_composite() above for the full contract and rationale. Number
// verified against libc/syscall.h and kernel/proc/syscall.h (414).
const SYS_WIN_SET_ALPHA_CONTENT: i64 = 414;
const SYS_WIN_SET_SHADOW: i64 = 376;      // verified against libc/syscall.h (#745)
const SYS_WIN_DRAW_TTF_EX: i64 = 311;     // verified against libc/syscall.h
const TTF_STYLE_NORMAL: i32 = 0;          // verified against kernel/gui/ttf.h
const TTF_STYLE_BOLD: i32 = 1;            // verified against kernel/gui/ttf.h

const WEL_BG_TOP: u32       = 0x0A1614;
const WEL_BG_MID: u32       = 0x122420;
const WEL_BG_BOTTOM: u32    = 0x050A09;
const WEL_GLOW: u32         = 0x6AE2CF;
const WEL_LINK: u32         = 0xAAAAAA;  // true neutral grey (R=G=B); the code
                                           // alpha-blends this at 850/1000 over the
                                           // REAL composite background at the link's
                                           // y=354 (wel_blend_over() below), which is
                                           // #0B1816, not the flat #050A09 bottom
                                           // gradient stop - the actual rendered pixel
                                           // is #848686 measuring only 4.96:1 at the
                                           // PREVIOUS #9A9A9A value (a thinner margin
                                           // than a flat-vs-#050A09 comparison implied).
                                           // At #AAAAAA the actual rendered pixel is
                                           // #929493, measuring 5.95:1 against its real
                                           // #0B1816 background - comfortable AA margin,
                                           // measured against what is actually drawn.

// ===========================================================================
// #745 GLASS CARD SHELL (docs/OOBE_GLASS_CARD.html)
// ===========================================================================
// The whole wizard is one borderless window for all ten pages, and that window
// IS the card: 688x616, fixed pixel size, centred on the real framebuffer. It
// does not scale with resolution (the glyph cache has ten fixed size buckets so
// type cannot be scaled by an arbitrary factor, every existing page is
// hardcoded to a 640x480 canvas, and a fixed card is ONE geometry to verify
// instead of one per resolution).
//
// DEVIATION FROM THE SPEC, STATED RATHER THAN HIDDEN. The spec asks for a
// FULLSCREEN window with the wizard painting the user's wallpaper itself and a
// drop shadow around the card. Two measured facts make that the worse of the
// two designs:
//
//   1. The wizard cannot obtain a sharp full-screen wallpaper. sys_fb_map()
//      rejects any non-compositor caller, no drawing primitive reads its
//      destination, and SYS_DECODE_IMAGE needs the WHOLE encoded file in
//      memory: the shipping wallpapers are 3,072,054-byte uncompressed 1280x800
//      BMPs, so painting the real wallpaper means a ~3 MB staging buffer plus a
//      ~4 MB decode target in an app that is no_std with NO allocator and every
//      buffer a fixed static mut. The only wallpaper source that fits is
//      /WPTHUMB/<name>, which build-golden.sh generates at 100x62 (MEASURED,
//      not assumed: build/build-golden.sh's `convert -resize 100x62^`). A
//      100x62 thumbnail stretched to 1280x800 is not a wallpaper, it is a
//      smudge, and it would sit directly against the real wallpaper at the
//      screen edge where the seam is most visible.
//   2. Making the window exactly the card means the COMPOSITOR draws the real
//      wallpaper around it, at full sharpness, for free, on every page. That is
//      strictly what the user asked for.
//
// What that costs is the drop shadow, which cannot be drawn outside a window.
// The spec's own contrast section assigns the shadow no role: it proves the
// card boundary is carried by the glass over a bright wallpaper (8.48:1) and by
// the 1 px edge stroke over a dark one (3.75:1), and says outright that "the
// drop shadow contributes nothing on a dark wallpaper by construction, so it
// cannot be relied on for this". So the shadow was decoration, and the trade is
// a real wallpaper everywhere against a decoration nowhere.
//
// Everything else is the spec unchanged: one surface for all ten pages, one
// card geometry, the content box subdivided 48 header / 640x480 body / 40
// footer, and each page re-anchored by a pure translation with no re-layout.

const SYS_GET_WALLPAPER: i64 = 205;   // verified against libc/syscall.h
const SYS_FONT_COUNT: i64    = 307;   // verified against libc/syscall.h
const SYS_FONT_NAME: i64     = 308;   // verified against libc/syscall.h
const SYS_FONT_GLYPH: i64    = 309;   // verified against libc/syscall.h
const SYS_FONT_KERN: i64     = 312;   // verified against libc/syscall.h
const SYS_FONT_STYLE: i64    = 324;   // verified against libc/syscall.h

const CARD_W: i32   = 688;
const CARD_H: i32   = 616;
// #745 P3: was 16. Reproduced at the user's own VM config (q35/std-vga/
// USB-xHCI, golden 1822, confirmed-live 1280x800) and the arc genuinely
// rounds and anti-aliases correctly there (measured against both the real
// wallpaper and an adversarial flat-grey stress fill, zero square seam
// either way), so the two prior "square corner" fixes were real fixes for a
// real sampling bug, not no-ops. The likeliest remaining explanation for a
// still-square REPORT is that a 16px arc on a 688px-wide card reads as
// square at native (no HiDPI) 1x pixel density, or under any client-side
// view that is not an exact 1:1 pixel scale (VNC/noVNC). 24px is a
// meaningfully bigger, harder-to-miss curve while staying inside every
// existing bound: CORNER_PATCH_MAX=64 native px/axis, and the worst-case
// native/screen ratio across the shipped 64-wallpaper set (all <=1280x800)
// at the GLASS_MIN floor of 736x664 is 1280/736=1.74, so 24*1.74=42px,
// comfortably under 64. Kept equal to CORNER_BOX below.
const CARD_R: i32   = 24;
const CARD_PAD: i32 = 24;    // content-box padding on all four sides
const HDR_H: i32    = 48;    // "Step N of M"
const FTR_H: i32    = 40;    // step dots
// Content box: 640 x 568 = 48 header + the EXISTING 640x480 page canvas
// bit-for-bit + 40 footer. CONTENT_W is W by construction; the assert makes a
// future edit to either one fail the build instead of silently misaligning
// every page by the difference.
const CONTENT_W: i32 = 640;
const CONTENT_H: i32 = HDR_H + 480 + FTR_H;
const _: () = assert!(CONTENT_W == W);
const _: () = assert!(CONTENT_H == 568);
const _: () = assert!(CARD_W == CONTENT_W + 2 * CARD_PAD);
const _: () = assert!(CARD_H == CONTENT_H + 2 * CARD_PAD);

// Small-screen fallback: do NOT reflow, revert to the legacy 640x480 centred
// window with the opaque gradient. There is no layout below the card size that
// has been designed or measured, so inventing one here would ship an unmeasured
// screen to the smallest displays.
const GLASS_MIN_W: i32 = 736;
const GLASS_MIN_H: i32 = 664;

// Glass recipe, from docs/DOCK_XFCE_GLASS.html unchanged. There must not be a
// second recipe.
//   bleed 36 px, downsample 4x, 3 separable box passes r=3 (w=7), sigma 13.90.
// The 4x downsample is NEAREST, not the recipe's 4x4 box: SYS_DECODE_IMAGE is
// the only way to get an image into a userland buffer and its scaler is pure
// nearest-neighbour point sampling (kernel/proc/syscall.c::img_scale_rows). The
// measured max deviation of the nearest path is 2.68 levels out of 255, the box
// contributes 1.25 of variance out of 193.25 so sigma moves 13.90 -> 13.86, and
// the aliasing nearest introduces is destroyed by the three box passes that
// follow. Do NOT add a corrective pass to chase 2 levels.
const BLEED: i32 = 36;
const GQ_W: usize = ((CARD_W + 2 * BLEED) / 4) as usize;   // 190
const GQ_H: usize = ((CARD_H + 2 * BLEED) / 4) as usize;   // 172

// Tint: the wizard's OWN existing mid gradient stop, no new colour token.
// 0.80 is SOLVED FOR, not copied from the mockup (which reads about 0.35, where
// white body text measures 2.14:1 over a white wallpaper and fails 4.5:1 by a
// wide margin). 0.80 is the lowest value at which every ink in the design
// clears its floor over a pure white wallpaper, including the weakest one (the
// inactive step dot, a non-text indicator needing 3:1). It keeps 20% of the
// blurred backdrop, about a 35-level swing over the shipping wallpapers, so the
// card still reads as a material rather than a flat panel. Do NOT "restore" the
// mockup's value.
const GLASS_TINT: u32 = WEL_BG_MID;    // #122420
const GLASS_TINT_A: i32 = 800;         // 0.80

const CARD_EDGE_A: i32 = 340;          // 1 px white @ 0.34 on the rounded path
const CARD_HL_A: i32   = 440;          // top highlight, white @ 0.44

static mut FB_W: i32 = W;
static mut FB_H: i32 = H;
static mut CARD_X: i32 = 0;            // card origin on the SCREEN
static mut CARD_Y: i32 = 0;
// Body origin in WINDOW coordinates. The window is the card, so window-local
// and card-local coordinates are the same thing.
static mut ORG_X: i32 = 0;
static mut ORG_Y: i32 = 0;
static mut CARD_MODE: bool = false;    // false = legacy 640x480 fallback
static mut GLASS_OK: bool = false;     // false = analytic-gradient backdrop

static mut GLASS: [u32; GQ_W * GQ_H] = [0; GQ_W * GQ_H];
static mut GSCRATCH: [u32; GQ_W * GQ_H] = [0; GQ_W * GQ_H];

// Wallpaper source. /WPTHUMB/<name> is what the wizard's own Desktop-picture
// page already opens, and build-golden.sh generates every one of them at
// exactly 100x62 (measured). The buffers below are sized with headroom so a
// larger thumbnail would be DOWNSCALED into them by the decoder rather than
// overrunning; the decoder never upscales (`int dw = sw, dh = sh;` then shrink
// only), so a smaller one arrives at its own size and dims[] reports it.
const WPT_MAX_W: usize = 192;
const WPT_MAX_H: usize = 120;
static mut WPT_RAW: [u8; 32768] = [0; 32768];             // a 100x62 24bpp BMP is 18,654 B
static mut WPT_PX: [u32; WPT_MAX_W * WPT_MAX_H] = [0; WPT_MAX_W * WPT_MAX_H];
static mut WPT_W: i32 = 0;
static mut WPT_H: i32 = 0;

// Native pixel size of the CURRENT wallpaper's real file (not the thumbnail),
// read by wp_read_native_meta(). 0 = unknown (header read failed or has not
// run yet); see wp_screen_to_thumb88() for why this is needed at all.
static mut WP_SRC_W: i32 = 0;
static mut WP_SRC_H: i32 = 0;
// #745 residual fix: BMP metadata needed to seek directly into the native
// file for the four corner patches (see corner_patch_build()). OFFBITS/BPP/
// COMP come from the same 34-byte header peek that used to stop at 26 bytes
// (wp_read_native_meta()). BPP != 24 or COMP != 0 (compressed) disables the
// native-corner path and the old thumbnail sampler is used, unchanged.
static mut WP_SRC_OFFBITS: i32 = 0;
static mut WP_SRC_BPP: i32 = 0;
static mut WP_SRC_COMP: i32 = 0;

// One CARD_R x CARD_R native-resolution patch per corner, sampled directly
// off the full wallpaper BMP through the compositor's OWN nearest-neighbor
// formula (wp_native_rowcol()), not the 100x62 build-time thumbnail.
// CORNER_OK gates per corner: a corner whose native patch could not be read
// (bad header, pathological aspect ratio, short read) falls back to the
// thumbnail sampler exactly as before, corner by corner, not as an
// all-or-nothing switch. #745 P3: 16 -> 24, see CARD_R.
const CORNER_BOX: i32 = 24;            // must match CARD_R, the arc's bounding square
const CORNER_PATCH_MAX: i32 = 64;      // native pixel bound per axis; see corner_patch_build
static mut CORNER_OK: [bool; 4] = [false; 4];
static mut CORNER_PIX: [u32; (4 * CORNER_BOX * CORNER_BOX) as usize] =
    [0; (4 * CORNER_BOX * CORNER_BOX) as usize];
// Scratch reused across the 4 corners: the raw native patch, and one row's
// worth of BMP bytes for a single seek+read.
static mut NPATCH: [u32; (CORNER_PATCH_MAX * CORNER_PATCH_MAX) as usize] =
    [0; (CORNER_PATCH_MAX * CORNER_PATCH_MAX) as usize];
static mut NPATCH_ROW: [u8; (CORNER_PATCH_MAX * 3) as usize] = [0; (CORNER_PATCH_MAX * 3) as usize];

// One horizontal strip of the card backdrop, blitted with a single
// SYS_WIN_DRAW_IMAGE. 688x616 varies per pixel (blurred wallpaper, rounded
// corners, AA edge), so it cannot be drawn with per-row rects the way the flat
// gradient could, and a whole-card buffer would be 1.7 MB of .bss. A 32-row
// strip is 88 KB and turns a full backdrop repaint into 20 syscalls.
const STRIP_H: usize = 32;
static mut STRIP: [u32; CARD_W as usize * STRIP_H] = [0; CARD_W as usize * STRIP_H];

static mut NWALLS_G: usize = 0;        // wp_enumerate() count, for the sentinel

// Translate BODY-local coordinates (what every existing page draw call uses)
// into window coordinates. In the legacy fallback ORG is (0,0) and this is the
// identity, so the old 640x480 screen is byte-for-byte unchanged.
#[inline]
fn body_to_win(x: i32, y: i32) -> (i32, i32) {
    unsafe { (x + ORG_X, y + ORG_Y) }
}
// The inverse, for incoming pointer events.
#[inline]
fn win_to_body(x: i32, y: i32) -> (i32, i32) {
    unsafe { (x - ORG_X, y - ORG_Y) }
}

// ---- shims over the renamed FFI draw symbols ------------------------------
#[inline]
unsafe fn gui_fill_circle_aa(h: i32, cx: i32, cy: i32, r: i32, fill: u32, outer: u32) {
    let (cx, cy) = body_to_win(cx, cy);
    gui_fill_circle_aa_raw(h, cx, cy, r, fill, outer);
}
#[inline]
unsafe fn gui_fill_rounded_aa(h: i32, x: i32, y: i32, w: i32, ht: i32, r: i32, fill: u32, outer: u32) {
    let (x, y) = body_to_win(x, y);
    gui_fill_rounded_aa_raw(h, x, y, w, ht, r, fill, outer);
}
#[inline]
unsafe fn gui_fill_rounded_grad(h: i32, x: i32, y: i32, w: i32, ht: i32, r: i32, top: u32, bottom: u32) {
    let (x, y) = body_to_win(x, y);
    gui_fill_rounded_grad_raw(h, x, y, w, ht, r, top, bottom);
}
#[inline]
unsafe fn gui_button(h: i32, x: i32, y: i32, w: i32, ht: i32, label: *const u8, variant: i32, st: i32) {
    let (x, y) = body_to_win(x, y);
    gui_button_raw(h, x, y, w, ht, label, variant, st);
}
#[inline]
unsafe fn gui_progress(h: i32, x: i32, y: i32, w: i32, ht: i32, pct: i32) {
    let (x, y) = body_to_win(x, y);
    gui_progress_raw(h, x, y, w, ht, pct);
}
#[inline]
unsafe fn gui_theme_win_preview(h: i32, x: i32, y: i32, w: i32, ht: i32, ti: i32, cut: u32) {
    let (x, y) = body_to_win(x, y);
    gui_theme_win_preview_raw(h, x, y, w, ht, ti, cut);
}
// Body-local image blit (thumbnails, world map, logo).
fn draw_image_body(win: i32, x: i32, y: i32, w: i32, h: i32, px: i64) {
    let (x, y) = body_to_win(x, y);
    unsafe { syscall6(SYS_WIN_DRAW_IMAGE, win as i64, x as i64, y as i64, w as i64, h as i64, px); }
}
// Same as draw_image_body(), for the two call sites that spell their w/h/ptr
// on a following line.
fn draw_image_body_raw6(win: i32, x: i64, y: i64, w: i64, h: i64, px: i64) {
    let (bx, by) = body_to_win(x as i32, y as i32);
    unsafe { syscall6(SYS_WIN_DRAW_IMAGE, win as i64, bx as i64, by as i64, w, h, px); }
}

// Window-local image blit, for the card shell itself.
fn draw_image_win(win: i32, x: i32, y: i32, w: i32, h: i32, px: i64) {
    unsafe { syscall6(SYS_WIN_DRAW_IMAGE, win as i64, x as i64, y as i64, w as i64, h as i64, px); }
}

// ---- integer helpers ------------------------------------------------------
fn isqrt(v: i32) -> i32 {
    if v <= 0 { return 0; }
    let mut x = v;
    let mut y = (x + 1) / 2;
    while y < x { x = y; y = (x + v / x) / 2; }
    x
}

// ---- wallpaper sampling ---------------------------------------------------
// #745 (bug: rounded card, square corner seam). This comment used to claim
// "screen -> source is a straight proportional map with no letterboxing to
// account for". MEASURED FALSE: the compositor (wallpaper.c) does stretch the
// native WxH wallpaper to fill the screen on EACH AXIS INDEPENDENTLY (no
// crop), but build-golden.sh does NOT generate WPTHUMB that way. It runs
// `convert src.bmp -resize 100x62^ -gravity center -extent 100x62`, which is
// COVER-FIT + CENTRE-CROP: ONE scale factor for both axes (preserving
// aspect), then whichever axis overhangs the 100x62 box gets its edges cut.
// For the shipped default wallpaper (BACK.BMP, 1280x720 into a 100x62/1.613:1
// box against a 1280x800/1.6:1 screen) that crops about 5 of the thumbnail's
// 100 columns off each side - roughly 32 SCREEN pixels' worth of horizontal
// error at the card's corners once you scale back up, easily enough to
// straddle a cloud edge in a photo wallpaper. The two naive functions below
// used the compositor's transform (proportional, no crop) to address a buffer
// that was actually built with the thumbnail generator's transform (cover +
// crop), so every corner sample pointed at the wrong patch of wallpaper - a
// visible, hard-edged mismatch sitting right on the window's RECTANGULAR
// boundary, which is what reads as "the rounded card is inside a square
// frame". wp_screen_to_thumb88() inverts the actual generator transform using
// the wallpaper's own native size (wp_read_native_meta(), a 34-byte BMP
// header peek, not a decode), and both samplers below now go through it.
//
// #745 FOLLOW-UP: fixing the mapping only shrank the corner-cutout error, it
// did not remove it (MEASURED 76.6/85.0 mean absolute error at TL/BL, see
// blame.md). The 100x62 thumbnail cannot reconstruct detail the build-time
// downsample discarded, no matter how exactly it is addressed. wp_at_screen()
// below still uses this mapping - correctly - for the INTERIOR glass blur,
// which is meant to be a blur and needs the whole card area, not 16 native
// pixels' worth. For the SHARP corner cutout, card_pixel() now prefers
// wp_corner_native() (native-resolution direct read, see below); this
// mapping and wp_at_screen_bilinear() remain only as its fallback.
fn wp_screen_to_thumb88(sx: i32, sy: i32) -> (i32, i32) {
    unsafe {
        let fw: i64 = if FB_W > 0 { FB_W as i64 } else { 1 };
        let fh: i64 = if FB_H > 0 { FB_H as i64 } else { 1 };
        let tw: i64 = if WPT_W > 0 { WPT_W as i64 } else { 1 };
        let th: i64 = if WPT_H > 0 { WPT_H as i64 } else { 1 };
        let (mut tx88, mut ty88): (i64, i64);
        if WP_SRC_W > 0 && WP_SRC_H > 0 {
            let nw = WP_SRC_W as i64;
            let nh = WP_SRC_H as i64;
            // Screen -> NATIVE wallpaper position, 8.8 fixed: this is exactly
            // wallpaper.c's own per-axis proportional stretch.
            let ox88 = sx as i64 * nw * 256 / fw;
            let oy88 = sy as i64 * nh * 256 / fh;
            // Cover-fit picks the LARGER of the two per-axis scale factors so
            // the scaled image covers the whole thumb box; comparing
            // (tw/nw) vs (th/nh) via cross-multiplication avoids a division.
            // Whichever axis picked the scale lands exactly (crop 0 on it);
            // the other axis is scaled the same amount and re-centred, which
            // is the crop build-golden.sh's `-gravity center` performed.
            if tw * nh >= th * nw {
                // width drove the scale: x is exact, y is scaled + cropped.
                tx88 = sx as i64 * tw * 256 / fw;
                ty88 = oy88 * tw / nw - (nh * tw / nw - th) * 128;
            } else {
                // height drove the scale: y is exact, x is scaled + cropped.
                ty88 = sy as i64 * th * 256 / fh;
                tx88 = ox88 * th / nh - (nw * th / nh - tw) * 128;
            }
        } else {
            // Native size unavailable (header read failed): fall back to the
            // old proportional guess. Wrong whenever the source aspect isn't
            // already the thumbnail's, but never worse than what shipped.
            tx88 = sx as i64 * tw * 256 / fw;
            ty88 = sy as i64 * th * 256 / fh;
        }
        let clamp88 = |v: i64, hi: i64| if v < 0 { 0 } else if v > hi { hi } else { v };
        tx88 = clamp88(tx88, tw * 256 - 1);
        ty88 = clamp88(ty88, th * 256 - 1);
        (tx88 as i32, ty88 as i32)
    }
}

// Builds "/<prefix><file>" (e.g. prefix "/" for the native BMP, "/WPTHUMB/"
// for the thumbnail) from the NUL-terminated-or-full-length name
// wp_enumerate() resolved. One definition, three call sites (native dims,
// native corner reads, the WPTHUMB open in glass_build()), so the path
// construction cannot drift between them.
fn wp_build_path(prefix: &[u8], f: &[u8]) -> [u8; 64] {
    let mut path: [u8; 64] = [0; 64];
    let mut n = 0usize;
    while n < prefix.len() && n < 63 { path[n] = prefix[n]; n += 1; }
    let mut k = 0usize;
    while k < f.len() && f[k] != 0 && n < 63 { path[n] = f[k]; n += 1; k += 1; }
    path[n] = 0;
    path
}

// Peeks the wallpaper's OWN (full-resolution) BMP header - BITMAPFILEHEADER
// (14 bytes) then BITMAPINFOHEADER through biCompression (20 bytes) - 34
// bytes total, never a decode. `f` is the same filename wp_enumerate()
// already resolved for the WPTHUMB path; this reads "/<file>" instead of
// "/WPTHUMB/<file>". A bottom-up BMP (what this OS writes) stores a positive
// height; a negative value just means top-down and is made positive, since
// wp_screen_to_thumb88() only needs the pixel COUNT. bfOffBits/biBitCount/
// biCompression are the extra fields corner_patch_build() needs to seek
// straight into the pixel data; wp_read_native_dims() below stays the
// dims-only entry point every existing caller uses.
// Returns (w, h, bfOffBits, biBitCount, biCompression), all zero on failure.
fn wp_read_native_meta(f: &[u8]) -> (i32, i32, i32, i32, i32) {
    let path = wp_build_path(b"/", f);
    let mut hdr: [u8; 34] = [0; 34];
    let got = unsafe {
        let fd = syscall3(SYS_OPEN, path.as_ptr() as i64, 0, 0) as i32;
        if fd < 0 { return (0, 0, 0, 0, 0); }
        let g = syscall3(SYS_READ, fd as i64, hdr.as_mut_ptr() as i64, 34);
        syscall1(SYS_CLOSE, fd as i64);
        g
    };
    if got < 34 || hdr[0] != b'B' || hdr[1] != b'M' { return (0, 0, 0, 0, 0); }
    let rd_i32 = |b: &[u8; 34], off: usize| -> i32 {
        (b[off] as i32) | ((b[off + 1] as i32) << 8)
            | ((b[off + 2] as i32) << 16) | ((b[off + 3] as i32) << 24)
    };
    let rd_u16 = |b: &[u8; 34], off: usize| -> i32 {
        (b[off] as i32) | ((b[off + 1] as i32) << 8)
    };
    let offbits = rd_i32(&hdr, 10);
    let mut w = rd_i32(&hdr, 18);
    let mut h = rd_i32(&hdr, 22);
    let bpp = rd_u16(&hdr, 28);
    let comp = rd_i32(&hdr, 30);
    if w < 0 { w = -w; }
    if h < 0 { h = -h; }
    if w <= 0 || h <= 0 { return (0, 0, 0, 0, 0); }
    (w, h, offbits, bpp, comp)
}

fn wp_at_screen(sx: i32, sy: i32) -> u32 {
    unsafe {
        if WPT_W <= 0 || WPT_H <= 0 { return 0; }
        let (tx88, ty88) = wp_screen_to_thumb88(sx, sy);
        let mut tx = tx88 >> 8;
        let mut ty = ty88 >> 8;
        if tx < 0 { tx = 0; } if tx >= WPT_W { tx = WPT_W - 1; }
        if ty < 0 { ty = 0; } if ty >= WPT_H { ty = WPT_H - 1; }
        WPT_PX[(ty * WPT_W + tx) as usize] & 0x00FF_FFFF
    }
}

// Bilinear, used ONLY for the four rounded corners, where the card does not
// cover the window and the pixels sit directly against the compositor's own
// sharp wallpaper. Nearest there would show as four hard little steps; bilinear
// over a ~16x16 arc spans barely more than one source pixel, so it reads as a
// smooth continuation of what is beside it.
fn wp_at_screen_bilinear(sx: i32, sy: i32) -> u32 {
    unsafe {
        if WPT_W <= 0 || WPT_H <= 0 { return 0; }
        let (fx, fy) = wp_screen_to_thumb88(sx, sy);
        let x0 = fx >> 8; let y0 = fy >> 8;
        let ax = fx & 0xFF; let ay = fy & 0xFF;
        let cl = |v: i32, hi: i32| if v < 0 { 0 } else if v >= hi { hi - 1 } else { v };
        let x0c = cl(x0, WPT_W); let x1c = cl(x0 + 1, WPT_W);
        let y0c = cl(y0, WPT_H); let y1c = cl(y0 + 1, WPT_H);
        let p = |x: i32, y: i32| WPT_PX[(y * WPT_W + x) as usize];
        let (p00, p10, p01, p11) = (p(x0c, y0c), p(x1c, y0c), p(x0c, y1c), p(x1c, y1c));
        let mut out = 0u32;
        let mut sh = 0;
        while sh <= 16 {
            let c00 = ((p00 >> sh) & 0xFF) as i32; let c10 = ((p10 >> sh) & 0xFF) as i32;
            let c01 = ((p01 >> sh) & 0xFF) as i32; let c11 = ((p11 >> sh) & 0xFF) as i32;
            let top = c00 + (c10 - c00) * ax / 256;
            let bot = c01 + (c11 - c01) * ax / 256;
            let v = top + (bot - top) * ay / 256;
            out |= ((v as u32) & 0xFF) << sh;
            sh += 8;
        }
        out
    }
}

// ---- #745 residual fix: native-resolution corner patches ------------------
// wp_screen_to_thumb88() + wp_at_screen_bilinear() correctly invert the
// build-time cover+crop thumbnail transform, but MEASURED mean absolute
// colour error against the true on-screen wallpaper still sat at 76.6 (TL)
// and 85.0 (BL) after that fix, on the shipped default wallpaper - see
// blame.md, "Two 'aspect-correct' resamplers disagreed...". That residual is
// not a further mapping bug: a 100x62 thumbnail upscaled roughly 12x cannot
// reconstruct detail the build-time downsample already discarded, and the
// corners that stayed wrong simply sit over busier parts of the picture than
// the corners that got clean. No coordinate arithmetic on the thumbnail can
// close that gap, because the thumbnail is missing the information, not
// pointing at the wrong pixel.
//
// The fix is to not go through the thumbnail for this one job: read the
// native BMP directly, at the handful of pixels the card's 16px corner arc
// actually needs (15-16 rows x 16 cols per corner for the shipped default,
// MEASURED - not a full decode of a multi-megabyte image), through targeted
// seek+read, and sample with the SAME nearest-neighbor formula
// wallpaper_render_background() uses (wallpaper.c). That makes a corner pixel
// not an approximation of what the compositor drew there, but a read of the
// same source through the same formula - MEASURED 0.0 mean absolute error
// against the compositor's own output at all four corners (vs 76.6/14.1/85.0/
// 29.0 for the corrected-mapping thumbnail sampler it replaces).
//
// Runs ONCE per wallpaper selection (glass_build(), not the draw path), never
// per frame: #426 (draw thread must not block) does not apply to a handful of
// synchronous file reads triggered by a user picking a wallpaper, only to
// anything in the redraw loop, and this is cached into CORNER_PIX exactly
// like the thumbnail buffers already are.

// screen -> native (row, col), nearest. This IS
// wallpaper_render_background()'s own per-axis floor-division stretch, not an
// inversion of a separate build-time transform, so there is nothing here that
// can drift out of sync with it the way the thumbnail mapping once did.
fn wp_native_rowcol(sx: i32, sy: i32) -> (i32, i32) {
    unsafe {
        let fw = if FB_W > 0 { FB_W } else { 1 };
        let fh = if FB_H > 0 { FB_H } else { 1 };
        let nw = WP_SRC_W;
        let nh = WP_SRC_H;
        let mut col = sx * nw / fw;
        let mut row = sy * nh / fh;
        if col < 0 { col = 0; } if col >= nw { col = nw - 1; }
        if row < 0 { row = 0; } if row >= nh { row = nh - 1; }
        (row, col)
    }
}

// Reads all four corners' native patches for the CURRENT wallpaper (WP_SRC_*
// must already be set) and fills CORNER_PIX/CORNER_OK. `f` is the same
// filename glass_build() resolved. Only 24bpp, uncompressed (BI_RGB) BMPs are
// supported (every wallpaper this build pipeline produces is); anything else,
// a short read, or a native span wider than CORNER_PATCH_MAX (a pathological
// source aspect ratio - none shipped) leaves that corner's CORNER_OK false
// and card_pixel() falls back to the thumbnail sampler for it, unchanged.
fn corner_patch_build(f: &[u8]) {
    unsafe {
        CORNER_OK = [false; 4];
        CORNER_REASON = [CR_OK; 4];
        if WP_SRC_W <= 0 || WP_SRC_H <= 0 || WP_SRC_BPP != 24 || WP_SRC_COMP != 0
            || WP_SRC_OFFBITS <= 0 {
            CORNER_REASON = [CR_BAD_HEADER; 4];
            corner_debug_flush(f);
            return;
        }
        let path = wp_build_path(b"/", f);
        let stride = ((WP_SRC_W * 3 + 3) / 4) * 4;   // BMP rows pad to a 4-byte boundary
        let corner_base: [(i32, i32); 4] = [
            (0, 0),                                   // TL
            (CARD_W - CORNER_BOX, 0),                  // TR
            (0, CARD_H - CORNER_BOX),                  // BL
            (CARD_W - CORNER_BOX, CARD_H - CORNER_BOX), // BR
        ];
        let mut ci = 0usize;
        while ci < 4 {
            let (bx, by) = corner_base[ci];

            // Pass 1: bounding box, in native pixels, of the 16x16 screen box.
            let mut rmin = i32::MAX; let mut rmax = i32::MIN;
            let mut cmin = i32::MAX; let mut cmax = i32::MIN;
            let mut yy = 0i32;
            while yy < CORNER_BOX {
                let mut xx = 0i32;
                while xx < CORNER_BOX {
                    let (r, c) = wp_native_rowcol(CARD_X + bx + xx, CARD_Y + by + yy);
                    if r < rmin { rmin = r; } if r > rmax { rmax = r; }
                    if c < cmin { cmin = c; } if c > cmax { cmax = c; }
                    xx += 1;
                }
                yy += 1;
            }
            let rows = rmax - rmin + 1;
            let cols = cmax - cmin + 1;
            if rows <= 0 || cols <= 0 || rows > CORNER_PATCH_MAX || cols > CORNER_PATCH_MAX {
                CORNER_REASON[ci] = CR_BAD_SPAN;
                ci += 1; continue;
            }

            // Pass 2: read the patch, one seek+read per native row.
            let fd = syscall3(SYS_OPEN, path.as_ptr() as i64, 0, 0) as i32;
            if fd < 0 { CORNER_REASON[ci] = CR_SHORT_READ; ci += 1; continue; }
            let mut ok = true;
            let mut lr = 0i32;
            while lr < rows {
                let filerow = WP_SRC_H - 1 - (rmin + lr);   // bottom-up BMP row flip
                let off = (WP_SRC_OFFBITS as i64) + (filerow as i64) * (stride as i64)
                    + (cmin as i64) * 3;
                syscall3(SYS_SEEK, fd as i64, off, 0);   // whence 0 = SEEK_SET
                let n = syscall3(SYS_READ, fd as i64,
                                  core::ptr::addr_of_mut!(NPATCH_ROW) as i64, (cols * 3) as i64);
                if n < (cols * 3) as i64 { ok = false; break; }
                let mut cc = 0i32;
                while cc < cols {
                    let b = NPATCH_ROW[(cc * 3) as usize] as u32;       // BMP pixel order: B,G,R
                    let g = NPATCH_ROW[(cc * 3 + 1) as usize] as u32;
                    let r = NPATCH_ROW[(cc * 3 + 2) as usize] as u32;
                    NPATCH[(lr * CORNER_PATCH_MAX + cc) as usize] = (r << 16) | (g << 8) | b;
                    cc += 1;
                }
                lr += 1;
            }
            syscall1(SYS_CLOSE, fd as i64);
            if !ok { CORNER_REASON[ci] = CR_SHORT_READ; ci += 1; continue; }

            // Pass 3: resolve each of the 256 screen pixels against the patch.
            let mut yy2 = 0i32;
            while yy2 < CORNER_BOX {
                let mut xx2 = 0i32;
                while xx2 < CORNER_BOX {
                    let (r, c) = wp_native_rowcol(CARD_X + bx + xx2, CARD_Y + by + yy2);
                    let lr2 = r - rmin;
                    let lc2 = c - cmin;
                    CORNER_PIX[ci * (CORNER_BOX * CORNER_BOX) as usize
                        + (yy2 * CORNER_BOX + xx2) as usize] =
                        NPATCH[(lr2 * CORNER_PATCH_MAX + lc2) as usize];
                    xx2 += 1;
                }
                yy2 += 1;
            }
            CORNER_OK[ci] = true;
            ci += 1;
        }
        corner_debug_flush(f);
    }
}

// The native-patch value at WINDOW-local (x, y), or None outside the four
// 16x16 corner boxes or when that corner's patch could not be read.
#[inline]
fn wp_corner_native(x: i32, y: i32) -> Option<u32> {
    unsafe {
        let (ci, lx, ly): (usize, i32, i32) =
            if x < CORNER_BOX && y < CORNER_BOX {
                (0, x, y)
            } else if x >= CARD_W - CORNER_BOX && y < CORNER_BOX {
                (1, x - (CARD_W - CORNER_BOX), y)
            } else if x < CORNER_BOX && y >= CARD_H - CORNER_BOX {
                (2, x, y - (CARD_H - CORNER_BOX))
            } else if x >= CARD_W - CORNER_BOX && y >= CARD_H - CORNER_BOX {
                (3, x - (CARD_W - CORNER_BOX), y - (CARD_H - CORNER_BOX))
            } else {
                return None;
            };
        if !CORNER_OK[ci] { return None; }
        Some(CORNER_PIX[ci * (CORNER_BOX * CORNER_BOX) as usize + (ly * CORNER_BOX + lx) as usize])
    }
}

// ---- #745 FOURTH PASS: the compositor's own drop shadow must apply to the
// "behind" pixel this app fakes, or the two meet at a hard, wrong-brightness
// seam on the window's own rectangular edge - NOT on the arc, which every
// prior pass correctly verified. Three prior passes each fixed a real bug
// (thumbnail-vs-native corner colour, shadow-follows-arc) and each still
// shipped this one, because none of them compared a corner pixel against the
// wallpaper pixel one column outside the window, only against the true
// wallpaper's OWN colour at that spot.
//
// MEASURED on a real boot (VM <vmid>, golden build 1843/cda94b8, wallpaper
// BOATING.BMP): windows_render_shadows() (userland/apps/compositor/main.c)
// draws its shadow onto the desktop wallpaper OUTSIDE this window's
// rectangle before the window's own content is composited over it. At
// screen (295, 100), one column left of the window edge (window left edge
// = CARD_X = 296), the compositor drew 173/173/169 - the raw wallpaper
// colour there (224/225/219, confirmed by reading the wallpaper BMP
// directly) darkened by the shadow. At screen (296, 100), the very next
// column, INSIDE the window, card_pixel() drew the raw 224/225/219 with NO
// shadow contribution, because the shadow is compositor-side state this app
// has no channel to read - a 51-point jump, byte-for-byte the "bright flat
// square with hard straight edges" reported four times. CORNER_OK was
// TRUE and wp_corner_native() was returning the EXACT correct wallpaper
// pixel the whole time; the missing piece was never in the sampler, it was
// that a real window's shadow bleeds a few pixels into the area a rounded
// corner exposes, and nothing on this side of the process boundary knew
// that.
//
// FIX: replicate windows_render_shadows()'s alpha math as a single-point
// query (that C code is a scanline FILL of the same ramp/diag tables this
// only needs to INDEX once per pixel; the span-walking and multi-window
// occlusion-cutting it also does are not needed here - this app only ever
// has its own one window, and the pixels in question are exactly the ones
// the real compositor WOULD occlude with this window's own content, which
// is why "what shadow alpha would apply here if this window did not cover
// it" is well-defined and is exactly what card_pixel() needs).
//
// VERIFIED byte-exact (MEASURED, not inferred): a standalone Python replica
// of this exact algorithm, run against the real screendump and the real
// wallpaper BMP, reproduced every shadowed pixel checked OUTSIDE the window
// (4 corners, both axes, ~140 sample points spanning flank/body/diagonal
// cases) with ZERO error before this was ported into Rust.
//
// SPEC COPY WARNING (same caveat as WP_GRAD_TOP/BOT below): SH_SPREAD/PEAK/
// OFFX/OFFY/R are literal copies of SHADOW_SPREAD/PEAK/OFFX/OFFY/CORNER_R in
// userland/apps/compositor/main.c. There is no shared header between the
// compositor and this app, so if those five constants ever change on the
// compositor side, this drifts out of sync silently, exactly like
// WP_GRAD_TOP/BOT already can. SH_R must also keep matching CARD_R /
// CORNER_BOX, same as SHADOW_CORNER_R does on the compositor side.
const SH_SPREAD: i32 = 32;   // SHADOW_SPREAD
const SH_PEAK: i32   = 89;   // SHADOW_PEAK
const SH_OFFX: i32   = 0;    // SHADOW_OFFX
const SH_OFFY: i32   = 6;    // SHADOW_OFFY
const SH_R: i32      = 16;   // SHADOW_CORNER_R; must match CARD_R / CORNER_BOX
const SH_AXQ_MAX: i32 = SH_SPREAD + SH_R;

static mut SH_READY: bool = false;
static mut SH_RAMP: [i32; (SH_SPREAD + 1) as usize] = [0; (SH_SPREAD + 1) as usize];
static mut SH_DIAG: [i32; ((SH_AXQ_MAX + 1) * (SH_AXQ_MAX + 1)) as usize] =
    [0; ((SH_AXQ_MAX + 1) * (SH_AXQ_MAX + 1)) as usize];

// Ports shadow_build_ramp() (main.c) exactly, reusing isqrt() (already used
// by card_cov() above) instead of a second integer-sqrt implementation.
fn shadow_build() {
    unsafe {
        if SH_READY { return; }
        let s = SH_SPREAD;
        let mut d = 0;
        while d <= s {
            let t = s - d;
            SH_RAMP[d as usize] = (SH_PEAK * t * t) / (s * s);
            d += 1;
        }
        let mut yq = 1;
        while yq <= SH_AXQ_MAX {
            let mut xq = 1;
            while xq <= SH_AXQ_MAX {
                let mut dd = isqrt(xq * xq + yq * yq) - SH_R;
                if dd < 0 { dd = 0; }
                let a = if dd >= s { 0 } else { SH_RAMP[dd as usize] };
                SH_DIAG[(yq * (SH_AXQ_MAX + 1) + xq) as usize] = a;
                xq += 1;
            }
            yq += 1;
        }
        SH_READY = true;
    }
}

// Ports shadow_axis_q() (main.c) verbatim.
#[inline]
fn shadow_axis_q(raw_outside: i32, inside_edge_dist: i32) -> i32 {
    if raw_outside > 0 { raw_outside + SH_R } else { SH_R - inside_edge_dist }
}

// Shadow alpha (0..255) the compositor would draw at WINDOW-local (x, y) if
// this window's own content were not there to occlude it - a point-query
// restatement of windows_render_shadows()'s flank/body case split, proved
// equal to it above. Only meaningful (and only ever called) for the sliver
// where card_cov() < 255, i.e. within SH_R of a corner.
fn shadow_alpha_win(x: i32, y: i32) -> i32 {
    unsafe {
        shadow_build();
        let sx = CARD_X + x; let sy = CARD_Y + y;
        let rx0 = CARD_X + SH_OFFX; let ry0 = CARD_Y + SH_OFFY;
        let rx1 = rx0 + CARD_W - 1; let ry1 = ry0 + CARD_H - 1;
        let qx_raw = if sx < rx0 { rx0 - sx } else if sx > rx1 { sx - rx1 } else { 0 };
        let qy_raw = if sy < ry0 { ry0 - sy } else if sy > ry1 { sy - ry1 } else { 0 };
        if qx_raw > SH_SPREAD || qy_raw > SH_SPREAD { return 0; }
        let edge_x = if sx - rx0 < rx1 - sx { sx - rx0 } else { rx1 - sx };
        let edge_y = if sy - ry0 < ry1 - sy { sy - ry0 } else { ry1 - sy };
        let xq = shadow_axis_q(qx_raw, edge_x);
        let yq = shadow_axis_q(qy_raw, edge_y);
        if qx_raw > 0 {
            // flank: outside the window on x.
            if yq <= 0 { SH_RAMP[qx_raw as usize] }
            else { SH_DIAG[(yq * (SH_AXQ_MAX + 1) + xq) as usize] }
        } else if xq > 0 && yq > 0 {
            // body, genuinely near a corner on both axes.
            SH_DIAG[(yq * (SH_AXQ_MAX + 1) + xq) as usize]
        } else {
            // body, far from this corner on x (or not near a top/bottom
            // corner at all): pure vertical ramp, matches shadow_draw_body()'s
            // mid_a exactly (both its Yq<=0 branch and its xq<=0-within-the-
            // corner-zone branch reduce to this same formula).
            let vd = yq - SH_R;
            if vd <= 0 { SH_RAMP[0] } else if vd >= SH_SPREAD { 0 } else { SH_RAMP[vd as usize] }
        }
    }
}

// #745 fourth pass, "make the fallback loud": a corner whose native patch
// could not be read used to fall back to the thumbnail sampler with NOTHING
// recorded anywhere - a silent downgrade is exactly how a real regression
// rode through three review passes that each measured a different corner
// and called it done. CORNER_REASON records WHY corner_patch_build() gave up
// on each corner (0 = it did not, either because it succeeded or because
// this corner has not been attempted this run). Overwrites /OOBEDBG.TXT each
// glass_build() call: this reports current status, not a history, matching
// how CORNER_OK itself is reset per wallpaper (stale "it was fine on the LAST
// wallpaper" is worse than useless here).
const CR_OK: u8          = 0;
const CR_BAD_HEADER: u8  = 1;   // WP_SRC_W/H/BPP/COMP/OFFBITS failed corner_patch_build's gate
const CR_BAD_SPAN: u8    = 2;   // native span for this corner's 16x16 box was <=0 or > CORNER_PATCH_MAX
const CR_SHORT_READ: u8  = 3;   // open() or read() of the native BMP failed partway through
static mut CORNER_REASON: [u8; 4] = [CR_OK; 4];

fn corner_debug_flush(f: &[u8]) {
    unsafe {
        if CORNER_OK[0] && CORNER_OK[1] && CORNER_OK[2] && CORNER_OK[3] { return; }
        let mut buf: [u8; 128] = [0; 128];
        let mut n = 0usize;
        for &b in b"OOBE corner fallback file=" { buf[n] = b; n += 1; }
        let mut k = 0usize;
        while k < f.len() && f[k] != 0 && n < 96 { buf[n] = f[k]; n += 1; k += 1; }
        for &b in b" TL=" { buf[n] = b; n += 1; }
        buf[n] = b'0' + CORNER_REASON[0]; n += 1;
        for &b in b" TR=" { buf[n] = b; n += 1; }
        buf[n] = b'0' + CORNER_REASON[1]; n += 1;
        for &b in b" BL=" { buf[n] = b; n += 1; }
        buf[n] = b'0' + CORNER_REASON[2]; n += 1;
        for &b in b" BR=" { buf[n] = b; n += 1; }
        buf[n] = b'0' + CORNER_REASON[3]; n += 1;
        for &b in b" (0=ok 1=header 2=span 3=short-read)\n" { buf[n] = b; n += 1; }
        let _ = userconf_write_all(b"/OOBEDBG.TXT\0".as_ptr(), buf.as_ptr(), n as u64);
    }
}

// ---- the compositor's OWN built-in gradient wallpaper ---------------------
// wp_enumerate()'s final entry is the built-in gradient: file[0] == 0, no image
// behind it. It is not "no wallpaper", it is a wallpaper the wizard can compute
// instead of decode. userland/apps/compositor/wallpaper.c draws it with
// draw_gradient_v(0, 0, fb_w, fb_h, CLR_WP_GRAD_TOP, CLR_WP_GRAD_BOT), a plain
// vertical lerp in 256 fixed-point steps, restated here EXACTLY (including the
// (h - 1) divisor and the 256-step quantisation) so the card's corners continue
// the wallpaper they sit on rather than approximating it.
//
// If those two constants ever change in compositor.h, the corners of this card
// over the gradient wallpaper stop matching. There is no shared header between
// the compositor and this app, so this is a copy, and it is flagged as one.
const WP_GRAD_TOP: u32 = 0x4A90C2;
const WP_GRAD_BOT: u32 = 0x1E5A8A;

fn wp_grad_screen(sy: i32) -> u32 {
    unsafe {
        let h = if FB_H > 1 { FB_H } else { 2 };
        let mut row = sy; if row < 0 { row = 0; } if row >= h { row = h - 1; }
        let t = row * 256 / (h - 1);
        let inv = 256 - t;
        let mut out = 0u32; let mut sh = 0;
        while sh <= 16 {
            let a = ((WP_GRAD_TOP >> sh) & 0xFF) as i32;
            let b = ((WP_GRAD_BOT >> sh) & 0xFF) as i32;
            out |= (((a * inv + b * t) / 256) as u32 & 0xFF) << sh;
            sh += 8;
        }
        out
    }
}

// ---- the analytic gradient, in SCREEN coordinates -------------------------
// Used when the wallpaper is the built-in gradient (the sentinel below) and on
// any decode failure. A gradient is already low-frequency, so blurring it is a
// no-op and the card looks identical either way. Evaluated in SCREEN
// coordinates so the card still samples the part of the gradient it covers.
fn wel_grad_screen(sy: i32) -> u32 {
    unsafe {
        let h = if FB_H > 0 { FB_H } else { 1 };
        let half = h / 2;
        if sy <= half { wel_lerp_rgb(WEL_BG_TOP, WEL_BG_MID, sy, half) }
        else { wel_lerp_rgb(WEL_BG_MID, WEL_BG_BOTTOM, sy - half, h - half) }
    }
}

// ---- glass construction ---------------------------------------------------
fn glass_box_pass(r: i32) {
    unsafe {
        // horizontal, clamp-to-edge, GLASS -> GSCRATCH
        let mut y = 0usize;
        while y < GQ_H {
            let row = y * GQ_W;
            let mut x = 0i32;
            while x < GQ_W as i32 {
                let (mut sr, mut sg, mut sb, mut n) = (0i32, 0i32, 0i32, 0i32);
                let mut k = -r;
                while k <= r {
                    let mut xx = x + k;
                    if xx < 0 { xx = 0; }
                    if xx >= GQ_W as i32 { xx = GQ_W as i32 - 1; }
                    let p = GLASS[row + xx as usize];
                    sr += ((p >> 16) & 0xFF) as i32; sg += ((p >> 8) & 0xFF) as i32; sb += (p & 0xFF) as i32;
                    n += 1; k += 1;
                }
                GSCRATCH[row + x as usize] =
                    (((sr / n) as u32) << 16) | (((sg / n) as u32) << 8) | ((sb / n) as u32);
                x += 1;
            }
            y += 1;
        }
        // vertical, clamp-to-edge, GSCRATCH -> GLASS
        let mut x = 0usize;
        while x < GQ_W {
            let mut y = 0i32;
            while y < GQ_H as i32 {
                let (mut sr, mut sg, mut sb, mut n) = (0i32, 0i32, 0i32, 0i32);
                let mut k = -r;
                while k <= r {
                    let mut yy = y + k;
                    if yy < 0 { yy = 0; }
                    if yy >= GQ_H as i32 { yy = GQ_H as i32 - 1; }
                    let p = GSCRATCH[yy as usize * GQ_W + x];
                    sr += ((p >> 16) & 0xFF) as i32; sg += ((p >> 8) & 0xFF) as i32; sb += (p & 0xFF) as i32;
                    n += 1; k += 1;
                }
                GLASS[y as usize * GQ_W + x] =
                    (((sr / n) as u32) << 16) | (((sg / n) as u32) << 8) | ((sb / n) as u32);
                y += 1;
            }
            x += 1;
        }
    }
}

// Reads the wallpaper thumbnail, builds the blurred+tinted glass buffer, and
// sets GLASS_OK. Called once at wizard start and again whenever the
// Desktop-picture page live-applies a different wallpaper, so the card actually
// tracks the choice being made behind it.
fn glass_build() {
    unsafe {
        GLASS_OK = false;
        WPT_W = 0; WPT_H = 0;
        WP_SRC_W = 0; WP_SRC_H = 0;
        CORNER_OK = [false; 4];   // #745: stale corners from a previous wallpaper must not leak
        if !CARD_MODE { return; }

        // GRADIENT SENTINEL. SYS_GET_WALLPAPER returns an INDEX, resolved to a
        // filename through wp_enumerate(), whose FINAL entry is the built-in
        // gradient with file[0] == 0 and no image behind it
        // (userland/libc/wallpapers.c: "Always append the gradient entry last
        // (file[0] == 0 signals 'no BMP')"). Asking the decoder for that file
        // would be asking it to decode "/WPTHUMB/".
        let idx = syscall0(SYS_GET_WALLPAPER);
        if idx < 0 || idx as usize >= NWALLS_G { return; }
        let f = &*core::ptr::addr_of!(WALLS[idx as usize].file);
        if f[0] == 0 { return; }

        // #745: needed to invert build-golden.sh's cover+crop thumbnail scale
        // (see wp_screen_to_thumb88()), and to seek directly into the native
        // file for the four corner patches (see corner_patch_build()). Failure
        // is non-fatal: WP_SRC_W/H stay 0 and every sampler falls back to its
        // pre-#745 behaviour.
        let (nw, nh, offbits, bpp, comp) = wp_read_native_meta(f);
        WP_SRC_W = nw; WP_SRC_H = nh;
        WP_SRC_OFFBITS = offbits; WP_SRC_BPP = bpp; WP_SRC_COMP = comp;
        corner_patch_build(f);

        let path = wp_build_path(b"/WPTHUMB/", f);
        let fd = syscall3(SYS_OPEN, path.as_ptr() as i64, 0, 0) as i32;
        if fd < 0 { return; }
        let got = syscall3(SYS_READ, fd as i64, core::ptr::addr_of_mut!(WPT_RAW) as i64, 32768);
        syscall1(SYS_CLOSE, fd as i64);
        if got <= 0 { return; }
        if got as usize >= 32768 { return; }   // truncated: bail on the buffer, not a decode error

        let target = ((WPT_MAX_W as u32) << 16) | (WPT_MAX_H as u32 & 0xFFFF);
        let mut dims: [i32; 2] = [0, 0];
        let r = syscall6(SYS_DECODE_IMAGE, core::ptr::addr_of!(WPT_RAW) as i64, got,
                         target as i64, core::ptr::addr_of_mut!(WPT_PX) as i64,
                         (WPT_MAX_W * WPT_MAX_H * 4) as i64, dims.as_mut_ptr() as i64);
        if r < 0 || dims[0] <= 0 || dims[1] <= 0 { return; }
        WPT_W = dims[0]; WPT_H = dims[1];

        // Downsample 4x by nearest into the bleed-expanded card region. The
        // bleed exists so the blur near the inner edge does not see only card
        // pixels and vignette.
        let mut gy = 0usize;
        while gy < GQ_H {
            let sy = CARD_Y - BLEED + (gy as i32) * 4;
            let mut gx = 0usize;
            while gx < GQ_W {
                let sx = CARD_X - BLEED + (gx as i32) * 4;
                GLASS[gy * GQ_W + gx] = wp_at_screen(sx, sy);
                gx += 1;
            }
            gy += 1;
        }

        glass_box_pass(3);
        glass_box_pass(3);
        glass_box_pass(3);

        let mut i = 0usize;
        while i < GQ_W * GQ_H {
            GLASS[i] = wel_blend_over(GLASS_TINT, GLASS[i], GLASS_TINT_A);
            i += 1;
        }
        GLASS_OK = true;
    }
}

// The glass value at a WINDOW-local pixel, upsampled 4x nearest (invisible at
// sigma 13.9).
#[inline]
fn glass_at_win(wx: i32, wy: i32) -> u32 {
    unsafe {
        if GLASS_OK {
            let mut gx = (wx + BLEED) >> 2;
            let mut gy = (wy + BLEED) >> 2;
            if gx < 0 { gx = 0; } if gx >= GQ_W as i32 { gx = GQ_W as i32 - 1; }
            if gy < 0 { gy = 0; } if gy >= GQ_H as i32 { gy = GQ_H as i32 - 1; }
            GLASS[gy as usize * GQ_W + gx as usize]
        } else if CARD_MODE {
            // No decodable image (the built-in gradient entry, or a decode
            // failure). Blurring a gradient is a no-op, so skip the blur and
            // tint the wallpaper directly. ONE rule either way: glass = tint
            // over the wallpaper.
            wel_blend_over(GLASS_TINT, wp_grad_screen(CARD_Y + wy), GLASS_TINT_A)
        } else {
            wel_blend_over(GLASS_TINT, wel_grad_screen(CARD_Y + wy), GLASS_TINT_A)
        }
    }
}

// Coverage (0..255) of a rounded rect of radius `r` inset by `inset` px, at
// window pixel (x, y). 255 everywhere except the four corner arcs.
fn card_cov(x: i32, y: i32, inset: i32, r: i32) -> i32 {
    let x0 = inset; let y0 = inset;
    let x1 = CARD_W - 1 - inset; let y1 = CARD_H - 1 - inset;
    if x < x0 || x > x1 || y < y0 || y > y1 { return 0; }
    if r <= 0 { return 255; }
    // corner centre, or None when the pixel is in a straight band
    let cx = if x < x0 + r { x0 + r } else if x > x1 - r { x1 - r } else { return 255; };
    let cy = if y < y0 + r { y0 + r } else if y > y1 - r { y1 - r } else { return 255; };
    let dx = x - cx; let dy = y - cy;
    // 4.4 fixed point distance, so the arc gets a one-pixel AA ramp rather than
    // a hard staircase. gui_fill_circle_aa uses the same idea; it just cannot
    // be used here because the fill varies per pixel.
    let d16 = isqrt((dx * dx + dy * dy) * 256);
    let r16 = r * 16;
    let t = r16 - d16;              // >0 inside
    if t >= 16 { 255 } else if t <= -16 { 0 } else { (t + 16) * 255 / 32 }
}

#[inline]
fn mix(bg: u32, fg: u32, a255: i32) -> u32 {
    if a255 <= 0 { return bg; }
    if a255 >= 255 { return fg; }
    wel_blend_over(fg, bg, a255 * 1000 / 255)
}

// One card pixel in WINDOW coordinates: wallpaper outside the rounded path,
// glass inside it, the 1 px edge stroke on the path, and the top highlight.
fn card_pixel(x: i32, y: i32) -> u32 {
    unsafe {
        let cov = card_cov(x, y, 0, CARD_R);
        // Outside the rounded path the card does not cover its own window, so
        // these pixels must continue the WALLPAPER the compositor drew around
        // the card, not the wizard's own gradient. Getting this wrong shows up
        // as dark wedges at the corners, i.e. the card reads as square.
        // #745: prefer the native-resolution read (wp_corner_native(), exact -
        // MEASURED 0.0 MAE against the compositor's own pixel) over the
        // thumbnail-bilinear sampler, which is kept as the fallback for a
        // corner whose native patch could not be read.
        let behind_raw = if cov >= 255 { 0 } else if let Some(np) = wp_corner_native(x, y) {
            np
        } else if GLASS_OK {
            wp_at_screen_bilinear(CARD_X + x, CARD_Y + y)
        } else {
            wp_grad_screen(CARD_Y + y)
        };
        // #745 fourth pass: the compositor darkens this exact pixel with its
        // own drop shadow when the window does not cover it (see
        // shadow_alpha_win()'s block comment above for the measured proof).
        // Applying that same alpha here is what keeps the wallpaper
        // continuous across the window's rectangular edge instead of
        // jumping to the raw, unshadowed colour right where the arc exposes
        // it.
        let behind = if cov >= 255 { 0 } else {
            let sa = shadow_alpha_win(x, y);
            if sa > 0 { mix(behind_raw, 0x000000, sa) } else { behind_raw }
        };
        let mut c = if cov >= 255 { glass_at_win(x, y) } else { mix(behind, glass_at_win(x, y), cov) };
        // 1 px stroke, white @ 0.34, on the rounded path: the difference
        // between the outer path and the same path inset by 1. Over a black
        // wallpaper the glass is nearly invisible (1.21:1) and this stroke is
        // the only thing that says where the card is, which is why it is 0.34
        // and not the 0.26 measured in the mockup.
        let inner = card_cov(x, y, 1, CARD_R - 1);
        let ring = cov - inner;
        if ring > 0 { c = mix(c, 0xFFFFFF, ring * CARD_EDGE_A / 1000); }
        // Top highlight, straight span only: inset by CARD_R on each side so
        // it stops before the rounded corners start. #745 P3: was the
        // literal "16" (the old CARD_R) and "656" (688 - 2*16); both drifted
        // when CARD_R changed, so this is now derived, not restated.
        if y == 0 && x >= CARD_R && x < CARD_W - CARD_R { c = mix(c, 0xFFFFFF, 255 * CARD_HL_A / 1000); }
        c
    }
}

// Repaints the whole card backdrop. 20 blits, not 423,808 rects.
fn card_paint_backdrop(win: i32) {
    unsafe {
        let mut y0 = 0i32;
        while y0 < CARD_H {
            let mut hh = STRIP_H as i32;
            if y0 + hh > CARD_H { hh = CARD_H - y0; }
            let mut row = 0i32;
            while row < hh {
                let y = y0 + row;
                let base = (row * CARD_W) as usize;
                let mut x = 0i32;
                while x < CARD_W { STRIP[base + x as usize] = card_pixel(x, y); x += 1; }
                row += 1;
            }
            draw_image_win(win, 0, y0, CARD_W, hh, core::ptr::addr_of!(STRIP) as i64);
            y0 += hh;
        }
    }
}

// ---------------------------------------------------------------------------
// STEP MODEL: ONE ordered list, everything derived from it.
// ---------------------------------------------------------------------------
// The tree held FOUR different answers at once: the welcome page drew 5 dots,
// every other page drew 9 with PG_NETWORK and PG_TIME DELIBERATELY COLLIDING on
// dot 3, dead code said 8, and the C rollback computes 7. Deriving the dot
// count, the active dot and the "Step N of M" string from one list makes that
// collision impossible by construction, and adding or removing a page updates
// all three with no further edit.
//
// PG_APPLY (progress) and PG_DONE (terminal) are NOT steps: they show no step
// counter and no dots.
const STEP_PAGES: [usize; 9] = [PG_WELCOME, PG_ACCOUNT, PG_SIGNIN, PG_NETWORK,
                                PG_TIME, PG_APPEAR, PG_WALL, PG_APPSW, PG_AI];

// ---------------------------------------------------------------------------
// #126: THE SAME ORDERED-LIST MODEL, NOW WITH TWO FLOWS.
// ---------------------------------------------------------------------------
// The wizard has a MODE, and everything about page order, the step counter, the
// dots, Back, Continue and Skip is derived from the ONE list belonging to that
// mode - exactly the property the comment above this block was written to
// protect. There is still no second place that can disagree; there are two
// lists and one set of derivations over them.
//
// FIRST BOOT (the machine is unconfigured, /CONFIG/SETUPDONE absent): the full
// nine-step flow, unchanged, including Create your account. This is the path
// that turns a shipped image into a machine with a real owner, and nothing
// about it may become optional.
//
// EVERY LATER USER'S FIRST LOGIN (the machine is configured but THIS user has
// never personalised, <home>/CONFIG/SETUPUSR absent): four pages, personalisa-
// tion only. Welcome, then the three pages that own the four things the ticket
// names - theme and dock style (Appearance), wallpaper (Desktop picture),
// widgets and dock pins (Apps & widgets).
//
// WHAT IS DELIBERATELY NOT IN THE REDUCED FLOW, and why:
//   * Create your account - the account already exists; the person is signed
//     into it. Offering it again would be offering to create a SECOND account.
//   * Sign-in options - startup/autologin is a property of the MACHINE (one
//     LOGIN.CFG, one boot), not of whoever happens to be signing in.
//   * Network - one NIC, one /CONFIG/NETIP.CFG, machine scope.
//   * Date & time - machine scope for the same reason: one RTC.
//   * AI settings - per-user in storage (AISVC.CFG goes through userconf), but
//     it is not one of the four the ticket asks for, and it is the one page
//     that asks for a secret. Settings > AI already owns it. FLAGGED for the
//     owner rather than decided quietly, same as timezone below.
//
// TIMEZONE IS THE ARGUABLE ONE AND IS DELIBERATELY LEFT OUT, FLAGGED. A second
// user in a different timezone is a real case, and the clock is the one thing
// on the Date & time page that could sensibly be per-user. It is excluded here
// because TZ.CFG is written through tz_set_index(), a MACHINE-scope writer
// shared with Settings, so making it per-user is a change to that writer and
// to every reader of it, not a change to this wizard. Splitting it is a
// separate piece of work with its own risk; this ticket does not smuggle it in.
const PERS_PAGES: [usize; 4] = [PG_WELCOME, PG_APPEAR, PG_WALL, PG_APPSW];

// Set ONCE in main(), before the App exists, from the two markers on disk.
// Read-only afterwards, which is why a plain static is enough and why every
// reader goes through personalise() rather than touching it.
static mut PERSONALISE: bool = false;

fn personalise() -> bool { unsafe { core::ptr::read_volatile(core::ptr::addr_of!(PERSONALISE)) } }

fn flow() -> &'static [usize] {
    if personalise() { &PERS_PAGES } else { &STEP_PAGES }
}

// #210 MERGE NOTE (agent/session126 -> dev). Every derivation below filters the
// flow through page_enabled(), which is what dev's STEP_COUNT/step_index did
// over STEP_PAGES before this branch replaced them. Dropping that filter would
// have silently put the temporarily disabled Network page (NETWORK_PAGE_ENABLED)
// back into the step total, back into the dots and back into both navigation
// directions. ONE predicate, both flows, both directions: a page cannot end up
// hidden from the dots yet still reachable with Back.

// The number of steps ACTUALLY SHOWN in the current flow.
fn step_count() -> usize {
    let f = flow();
    let mut i = 0usize;
    let mut n = 0usize;
    while i < f.len() { if page_enabled(f[i]) { n += 1; } i += 1; }
    n
}

// Position among the ENABLED steps of the current flow, so the pages after a
// disabled one shift down by one rather than leaving a hole (a wizard that
// jumps 3 -> 5 reads as broken). A disabled page, or a page not in this flow at
// all, has no position and returns -1 - the same answer PG_APPLY/PG_DONE get,
// which is_step_page()/card_chrome() already treat as "no counter, no dots".
fn step_index(page: usize) -> i32 {
    if !page_enabled(page) { return -1; }
    let f = flow();
    let mut i = 0usize;
    let mut n = 0i32;
    while i < f.len() {
        let p = f[i];
        if page_enabled(p) {
            if p == page { return n; }
            n += 1;
        }
        i += 1;
    }
    -1
}

/// The page AFTER `page` in the current flow, or None if `page` is the last
/// one (the caller then runs Apply). None is also returned for a page that is
/// not in the flow at all, which is the correct refusal: PG_APPLY and PG_DONE
/// are not steps and must never be walked into by arithmetic.
// Position of `page` in the flow ARRAY (not among the enabled steps). Private
// to the two walkers below, which need the raw slot so they can then walk over
// disabled neighbours in either direction.
fn flow_slot(page: usize) -> Option<usize> {
    let f = flow();
    let mut i = 0usize;
    while i < f.len() { if f[i] == page { return Some(i); } i += 1; }
    None
}

fn flow_next(page: usize) -> Option<usize> {
    let f = flow();
    let at = match flow_slot(page) { Some(a) => a, None => return None };
    let mut j = at + 1;
    while j < f.len() {
        if page_enabled(f[j]) { return Some(f[j]); }
        j += 1;
    }
    None
}

fn flow_prev(page: usize) -> Option<usize> {
    let f = flow();
    let at = match flow_slot(page) { Some(a) => a, None => return None };
    let mut j = at;
    while j > 0 {
        j -= 1;
        if page_enabled(f[j]) { return Some(f[j]); }
    }
    None
}

/// The last page of the flow: what Back from the Done page returns to, and the
/// page whose Continue runs Apply.
fn flow_last() -> usize {
    let f = flow();
    let mut j = f.len();
    while j > 0 { j -= 1; if page_enabled(f[j]) { return f[j]; } }
    f[0]
}

/// Where Apply sends the person when it FAILS. In the full flow that is the
/// account page, because account creation is the only step that can fail in a
/// way the person can fix. The reduced flow has no account page, so it is the
/// first page they can actually act on.
/// #229: ...and if the account page is not in this flow (a non-root session
/// cannot create an account, see machine_admin()), send them to the FIRST page
/// that is. Returning a disabled page here would have landed the wizard on a
/// screen the dots say does not exist and that Continue cannot navigate away
/// from, which is a second dead end reached only on a failure path - the worst
/// possible place for one.
fn flow_error_page() -> usize {
    if personalise() { return PG_APPEAR; }
    if page_enabled(PG_ACCOUNT) { return PG_ACCOUNT; }
    let f = flow();
    let mut i = 0usize;
    while i < f.len() { if page_enabled(f[i]) { return f[i]; } i += 1; }
    PG_WELCOME
}

// ---------------------------------------------------------------------------
// The real bold outline, and measuring text that ttf_measure() cannot measure.
// ---------------------------------------------------------------------------
// ttf_get_glyph_f() emboldens by SMEARING (+1 px) when TTF_STYLE_BOLD is set on
// a face, so "bold" at face 0 is synthesised. /FONTS/DEJAVUB.TTF is a real bold
// outline and is on the shipping image (measured on the golden ESP), so the
// 48 px headline resolves that FACE and draws it at NORMAL style. Falls back to
// face 0 + synthesised bold if the face is not installed, rather than failing to
// draw.
static mut FACE_BOLD: i32 = -1;
static mut FG_BMP: [u8; 16384] = [0; 16384];

fn face_str_eq(buf: &[u8], want: &[u8]) -> bool {
    let mut i = 0usize;
    while i < want.len() { if buf[i] != want[i] { return false; } i += 1; }
    buf[want.len()] == 0
}

fn resolve_bold_face() {
    unsafe {
        let n = syscall0(SYS_FONT_COUNT) as i32;
        let mut i = 0i32;
        while i < n && i < 128 {
            let mut nm: [u8; 64] = [0; 64];
            let mut st: [u8; 64] = [0; 64];
            syscall3(SYS_FONT_NAME, i as i64, nm.as_mut_ptr() as i64, 64);
            syscall3(SYS_FONT_STYLE, i as i64, st.as_mut_ptr() as i64, 64);
            // Exact family match: "DejaVu Sans Mono" and "DejaVu Sans Condensed"
            // must not win, and face_str_eq requires the NUL right after.
            if face_str_eq(&nm, b"DejaVu Sans") && face_str_eq(&st, b"Bold") {
                FACE_BOLD = i;
                return;
            }
            i += 1;
        }
    }
}

// Advance of one codepoint on a specific face/size/style. ttf_measure() (and
// gui_ttf_width(), which wraps it) uses the ACTIVE face at NORMAL style only, so
// it cannot measure either the tracked eyebrow or the 48 px bold headline, and
// centring either one with it would be wrong.
fn face_advance(face: i32, size: i32, style: i32, cp: u8) -> i32 {
    unsafe {
        let mut meta: [i32; 5] = [0; 5];
        let packed = (face as i64 & 0xFF) | ((size as i64 & 0xFFFF) << 8) | ((style as i64 & 0xFF) << 24);
        let a = syscall5(SYS_FONT_GLYPH, packed, cp as i64, meta.as_mut_ptr() as i64,
                         core::ptr::addr_of_mut!(FG_BMP) as i64, 16384) as i32;
        if a >= 0 { a } else { 0 }
    }
}
fn face_kern(face: i32, size: i32, a: u8, b: u8) -> i32 {
    unsafe {
        let packed = (face as i64 & 0xFF) | ((size as i64 & 0xFFFF) << 8);
        syscall3(SYS_FONT_KERN, packed, a as i64, b as i64) as i32
    }
}

// Face-aware TTF draw. text_ex() hardcodes face 0; this one does not.
fn text_face(win: i32, x: i32, y: i32, s: &[u8], face: i32, style: i32, size: i32, color: u32) {
    let (x, y) = body_to_win(x, y);
    let xy: i64 = ((x as i64) & 0xFFFF) | (((y as i64) & 0xFFFF) << 16);
    let fss: i64 = (face as i64 & 0xFF) | (((size as i64) & 0xFFFF) << 8) | (((style as i64) & 0xFF) << 24);
    unsafe {
        syscall5(SYS_WIN_DRAW_TTF_EX, win as i64, xy, s.as_ptr() as i64, fss,
                 (color & 0xFFFFFF) as i64);
    }
}

// Draws `s` (NO NUL terminator) centred on x_center, measured by SUMMING
// per-glyph advances plus kerning on the face that will actually draw it.
fn draw_centered_face(win: i32, x_center: i32, y: i32, s: &[u8], size: i32, color: u32) {
    unsafe {
        let (face, style) = if FACE_BOLD >= 0 { (FACE_BOLD, TTF_STYLE_NORMAL) } else { (0, TTF_STYLE_BOLD) };
        let n = s.len();
        if n == 0 || n > 48 { return; }
        let mut total = 0i32;
        let mut i = 0usize;
        while i < n {
            total += face_advance(face, size, style, s[i]);
            if i + 1 < n { total += face_kern(face, size, s[i], s[i + 1]); }
            i += 1;
        }
        let mut x = x_center - total / 2;
        i = 0;
        while i < n {
            let buf: [u8; 2] = [s[i], 0];
            text_face(win, x, y, &buf, face, style, size, color);
            x += face_advance(face, size, style, s[i]);
            if i + 1 < n { x += face_kern(face, size, s[i], s[i + 1]); }
            i += 1;
        }
    }
}

// ---------------------------------------------------------------------------
// The logo mark: a 96x96 A8 COVERAGE MASK, ink applied at DRAW time.
// ---------------------------------------------------------------------------
// WHY A MASK AND NOT A BITMAP. The old LOGOMARK.BMP was antialiased by
// PRE-COMPOSITING its edge against the old dark gradient per scanline. Over a
// blurred photograph the composite background is unknowable at asset-generation
// time, so that technique is superseded: it would show a dark halo everywhere
// the glass is lighter than #0A1614..#122420, which is most of the card over
// most wallpapers.
//
// An antialiased edge still works here for one reason only: the wizard DRAWS the
// backdrop and therefore KNOWS it. It cannot read the screen (sys_fb_map()
// rejects any non-compositor caller and is_compositor() latches the first
// caller's pid permanently) and no drawing primitive reads its destination, so
// `dst = mix(dst, ink, coverage)` is not expressible against the window surface.
// What IS expressible is `out = mix(glass(x,y), ink, coverage/255)` computed
// here from our own glass buffer, then written with ONE opaque
// SYS_WIN_DRAW_IMAGE. Same contract gui_fill_circle_aa(..., color, bg) already
// uses: it takes the background as an argument precisely because it cannot
// discover it.
const LOGO_N: usize = 96;
const LOGO_HDR: usize = 16;
static mut LOGO_RAW: [u8; LOGO_HDR + LOGO_N * LOGO_N] = [0; LOGO_HDR + LOGO_N * LOGO_N];
static mut LOGO_MASK_OK: bool = false;
static mut LOGO_PX: [u32; LOGO_N * LOGO_N] = [0; LOGO_N * LOGO_N];

// Loaded once at wizard start, never on the draw path.
fn logo_load() {
    unsafe {
        LOGO_MASK_OK = false;
        let fd = syscall3(SYS_OPEN, b"/OOBE/LOGOMARK.A8\0".as_ptr() as i64, 0, 0) as i32;
        if fd < 0 { return; }
        let want = (LOGO_HDR + LOGO_N * LOGO_N) as i64;
        let got = syscall3(SYS_READ, fd as i64, core::ptr::addr_of_mut!(LOGO_RAW) as i64, want);
        syscall1(SYS_CLOSE, fd as i64);
        if got != want { return; }
        let r = &*core::ptr::addr_of!(LOGO_RAW);
        // Validate the header rather than trusting the path. A file of the right
        // LENGTH with the wrong content would otherwise draw as noise.
        if r[0] != b'M' || r[1] != b'A' || r[2] != b'8' || r[3] != b'1' { return; }
        let w = (r[4] as usize) | ((r[5] as usize) << 8);
        let h = (r[6] as usize) | ((r[7] as usize) << 8);
        let stride = (r[8] as usize) | ((r[9] as usize) << 8);
        if w != LOGO_N || h != LOGO_N || stride != LOGO_N { return; }
        LOGO_MASK_OK = true;
    }
}

// Composites the mask over the glass at BODY-local (x, y) and blits it opaque.
fn logo_draw(win: i32, x: i32, y: i32) {
    unsafe {
        if !LOGO_MASK_OK { return; }
        let (wx, wy) = body_to_win(x, y);
        let r = &*core::ptr::addr_of!(LOGO_RAW);
        let mut row = 0usize;
        while row < LOGO_N {
            let mut col = 0usize;
            while col < LOGO_N {
                let cvg = r[LOGO_HDR + row * LOGO_N + col] as i32;
                let bg = glass_at_win(wx + col as i32, wy + row as i32);
                LOGO_PX[row * LOGO_N + col] = mix(bg, DK_ACCENT, cvg);
                col += 1;
            }
            row += 1;
        }
        draw_image_body(win, x, y, LOGO_N as i32, LOGO_N as i32,
                        core::ptr::addr_of!(LOGO_PX) as i64);
    }
}


// Glow band height, scaled from the spec's 620px (out of a 768px canvas) by
// 0.625: 620 * 0.625 = 387.5, rounded to 388. It is full-width by
// construction (the draw loop below runs x=0..W for every row), so only its
// height and the alpha stops below needed scaling.
const WEL_GLOW_H: i32 = 388;

#[repr(C)]
struct FbInfo { width: u32, height: u32, pitch: u32, bpp: u32, phys_addr: u64 }
const _: () = assert!(core::mem::size_of::<FbInfo>() == 24);

// Real screen size (sys_fb_info() is available to any app, not just the
// compositor - Settings and several other apps already call it). Used once,
// in main(), to CENTRE the whole wizard window on the real screen instead of
// the old hardcoded (120, 60) offset. Falls back to the window's own W x H
// on failure so the centring math never divides by a bogus size.
fn real_screen_size() -> (i32, i32) {
    let mut info = FbInfo { width: 0, height: 0, pitch: 0, bpp: 0, phys_addr: 0 };
    let r = unsafe { syscall1(SYS_FB_INFO, &mut info as *mut FbInfo as i64) };
    if r == 0 && info.width > 0 && info.height > 0 {
        (info.width as i32, info.height as i32)
    } else {
        (W, H)
    }
}

fn win_set_nochrome(h: i32) { unsafe { syscall1(SYS_WIN_SET_NOCHROME, h as i64); } }
// #wizfocus: see the SYS_WIN_SET_NOCHROME_BG comment above.
fn win_set_nochrome_bg(h: i32) { unsafe { syscall1(SYS_WIN_SET_NOCHROME_BG, h as i64); } }
// #198v2: see SYS_WIN_SET_ALPHA_CONTENT above.
fn win_set_alpha_content(h: i32) { unsafe { syscall1(SYS_WIN_SET_ALPHA_CONTENT, h as i64); } }
fn win_set_shadow(h: i32) { unsafe { syscall1(SYS_WIN_SET_SHADOW, h as i64); } }

// Face-aware TTF draw (face 0 = default UI font, per kernel/gui/ttf.c: "face 0
// is the default UI font"). Needed because the plain text()/SYS_WIN_DRAW_TTF
// path used by the rest of this file has no bold, and the spec's eyebrow/
// headline/button label are all Bold 700.
fn text_ex(win: i32, x: i32, y: i32, s: &[u8], style: i32, size: i32, color: u32) {
    let (x, y) = body_to_win(x, y);
    let xy: i64 = ((x as i64) & 0xFFFF) | (((y as i64) & 0xFFFF) << 16);
    let fss: i64 = (0i64 & 0xFF) | (((size as i64) & 0xFFFF) << 8) | (((style as i64) & 0xFF) << 24);
    unsafe {
        syscall5(SYS_WIN_DRAW_TTF_EX, win as i64, xy, s.as_ptr() as i64, fss,
                 (color & 0xFFFFFF) as i64);
    }
}

fn wel_lerp_rgb(c0: u32, c1: u32, num: i32, den: i32) -> u32 {
    let den = if den <= 0 { 1 } else { den };
    let r0 = ((c0 >> 16) & 0xFF) as i32; let g0 = ((c0 >> 8) & 0xFF) as i32; let b0 = (c0 & 0xFF) as i32;
    let r1 = ((c1 >> 16) & 0xFF) as i32; let g1 = ((c1 >> 8) & 0xFF) as i32; let b1 = (c1 & 0xFF) as i32;
    let r = r0 + (r1 - r0) * num / den;
    let g = g0 + (g1 - g0) * num / den;
    let b = b0 + (b1 - b0) * num / den;
    ((r as u32) << 16) | ((g as u32) << 8) | (b as u32)
}

// Row 1: background 3-stop vertical gradient over the window's OWN height H
// (spec table row 1: #0A1614 @0%, #122420 @50%, #050A09 @100%). The window
// IS the canvas now (no larger real-screen framebuffer to stretch into), so
// this is never re-stretched to anything else.
fn wel_bg_at(y: i32) -> u32 {
    let half = H / 2;
    if half <= 0 { return WEL_BG_TOP; }
    if y <= half { wel_lerp_rgb(WEL_BG_TOP, WEL_BG_MID, y, half) }
    else { wel_lerp_rgb(WEL_BG_MID, WEL_BG_BOTTOM, y - half, H - half) }
}

// Row 2: glow alpha (permille, 0..1000) at row y measured from the top of
// the window (the glow band starts at y=0, same origin as the background -
// there is no separate content-centring offset now that the window itself
// is exactly the content size). Piecewise linear across the spec's own stop
// points, scaled by 0.625: (0,0) (136,85)->(85,85) (298,85)->(186,85)
// (465,20)->(291,20) (620,0)->(388,0). The two REJECTED approaches recorded
// in the spec (two overlapping directional-gradient rects; a stack of
// concentric flat-alpha rects) both produced a visible hard edge and are
// not reintroduced here.
fn wel_glow_alpha_permille(y: i32) -> i32 {
    if y <= 0 || y >= WEL_GLOW_H { return 0; }
    const STOPS: [(i32, i32); 5] = [(0, 0), (85, 85), (186, 85), (291, 20), (388, 0)];
    let mut i = 0;
    while i < 4 {
        let (y0, a0) = STOPS[i]; let (y1, a1) = STOPS[i + 1];
        if y >= y0 && y <= y1 {
            if y1 == y0 { return a0; }
            return a0 + (a1 - a0) * (y - y0) / (y1 - y0);
        }
        i += 1;
    }
    0
}

// source-over, fg at alpha_permille/1000 over bg. There is no real alpha
// channel on this compositor's draw primitives, so every "@alpha" colour in
// the spec is pre-blended in software against a KNOWN background (our own
// gradient) before being handed to rect()/text_ex()/gui_fill_circle_aa() as a
// flat colour.
fn wel_blend_over(fg: u32, bg: u32, alpha_permille: i32) -> u32 {
    let a = alpha_permille;
    let fr = ((fg >> 16) & 0xFF) as i32; let fgc = ((fg >> 8) & 0xFF) as i32; let fb = (fg & 0xFF) as i32;
    let br = ((bg >> 16) & 0xFF) as i32; let bgc = ((bg >> 8) & 0xFF) as i32; let bb = (bg & 0xFF) as i32;
    let r = (fr * a + br * (1000 - a)) / 1000;
    let g = (fgc * a + bgc * (1000 - a)) / 1000;
    let b = (fb * a + bb * (1000 - a)) / 1000;
    ((r as u32) << 16) | ((g as u32) << 8) | (b as u32)
}

// THE BACKDROP SAMPLER, and the single point that makes the card safe.
//
// This used to be `wel_composite_at(y)`, a pure function of y measured from the
// TOP OF THE WINDOW, and EVERY alpha blend in this wizard funnels through it
// (about thirty call sites, on every page). The moment the page body is inset
// inside a card, a y-only sampler returns the wrong backdrop for every one of
// them. Nothing errors; the symptom is subtly wrong colours on every page. So
// the fix is here, at the choke point, and NOT by sprinkling offsets at call
// sites and NOT by introducing a child window (which would need its own
// backdrop plumbing and leave the wizard with two surfaces to keep in step,
// which is the problem the single-window design already solved once).
//
// x and y are BODY-LOCAL: exactly the coordinates the caller passes to rect().
// The x argument is not decoration. The glass is a blurred photograph and its
// horizontal variation across the 640 px content box is about 35 levels over
// the shipping wallpapers, so sampling a widget's backdrop at the card's centre
// instead of at the widget would fringe every antialiased edge.
fn wel_composite_at(x: i32, y: i32) -> u32 {
    unsafe {
        if CARD_MODE {
            let (wx, wy) = body_to_win(x, y);
            return glass_at_win(wx, wy);
        }
    }
    // Legacy 640x480 fallback: byte-for-byte the previous behaviour.
    let base = wel_bg_at(y);
    let a = wel_glow_alpha_permille(y);
    if a <= 0 { base } else { wel_blend_over(WEL_GLOW, base, a) }
}

fn wel_measure1(c: u8, size: i32) -> i32 {
    let buf: [u8; 2] = [c, 0];
    unsafe { gui_ttf_width(buf.as_ptr(), size) }
}

// Draws `s` (a plain byte slice, no NUL terminator) centred on x_center at
// the literal top-of-cap-height y (spec: "Y is the top of the cap-height
// box"), with per-glyph tracking of `spacing_tenths` (tenths of a pixel). A
// fixed-point accumulator distributes the fractional remainder across
// glyphs instead of rounding it to zero. Returns the total drawn width, for
// callers that need a click hit-rect (the link).
fn wel_text_tracked(win: i32, x_center: i32, y: i32, s: &[u8], size: i32, bold: bool,
                    color: u32, spacing_tenths: i32) -> i32 {
    let style = if bold { TTF_STYLE_BOLD } else { TTF_STYLE_NORMAL };
    let n = s.len();
    if n == 0 || n > 48 { return 0; }
    let mut widths: [i32; 48] = [0; 48];
    let mut total_w = 0i32;
    let mut i = 0usize;
    while i < n { widths[i] = wel_measure1(s[i], size); total_w += widths[i]; i += 1; }
    if n > 1 {
        let mut acc = 0i32; let mut k = 0usize;
        while k < n - 1 { acc += spacing_tenths; let step = acc / 10; acc -= step * 10; total_w += step; k += 1; }
    }
    let mut x = x_center - total_w / 2;
    let mut acc = 0i32;
    i = 0;
    while i < n {
        let buf: [u8; 2] = [s[i], 0];
        text_ex(win, x, y, &buf, style, size, color);
        x += widths[i];
        if i + 1 < n { acc += spacing_tenths; let step = acc / 10; acc -= step * 10; x += step; }
        i += 1;
    }
    total_w
}

impl App {
    // Draws the welcome screen (PG_WELCOME, page 0) into THIS window at its
    // full 640x480 extent - it is the window's entire content, not an inset
    // panel, so the wizard reads as one continuous surface on page 1 just as
    // it does on every later page (same win handle, same window, no chrome
    // toggle mid-flow). Stores the two clickable hit-boxes (button, link) in
    // self so on_click() can hit-test them. Called on first paint and on
    // every EV_REDRAW, same as the rest of the wizard.
    fn draw_welcome(&mut self) {
        let win = self.win;

        // Backdrop + card chrome (the step counter in the header strip and the
        // step dots in the footer strip). Page 1 used to paint its own gradient
        // and its own hardcoded FIVE dots; it now shares the one hook every
        // other page already used, which is what makes the counter and the dot
        // index incapable of disagreeing.
        dk_fill_bg(win, self.page);

        // Spec section 4. All coordinates below are BODY-local, i.e. the
        // spec's content-box-local values minus the 48 px header strip.
        // cx = 320 is the content box horizontal centre and everything on this
        // page is centred on it.

        // 2. Logo mark, 96x96 at x=272 (272 + 48 = 320), content y=100.
        // A8 coverage mask, ink applied at draw time, composited against the
        // glass this wizard computed and therefore knows. See logo_draw().
        logo_draw(win, 272, 100 - HDR_H);

        // 3. Eyebrow "WELCOME TO", content y=232, #FFFFFF @ 0.75.
        // SMALL CAPS DO NOT EXIST on this platform: stb_truetype is used for
        // glyf outlines and the kern table only, and a tree-wide search for
        // any OpenType feature plumbing returns zero hits. The closest
        // renderable thing is an uppercased string at 18 px regular, which is
        // slightly heavier and taller than a true small cap.
        // LETTER-SPACING DOES NOT EXIST either: every draw path funnels through
        // ttf_cursor_step_f(), which returns advance plus pair kerning and
        // nothing else, and no API in the userland text path takes a tracking
        // value. So this ONE string is drawn glyph by glyph with a manual +5 px
        // advance (50 tenths). Do NOT generalise it: no other label in this
        // wizard is tracked, and wel_text_tracked() also measures per glyph, so
        // the centring is right for the tracked run rather than for the
        // untracked width ttf_measure() would have returned.
        let ey = 232 - HDR_H;
        let eyebrow_col = wel_blend_over(0xFFFFFF, wel_composite_at(W / 2, ey), 750);
        wel_text_tracked(win, 320, ey, b"WELCOME TO", 18, false, eyebrow_col, 50);

        // 4. Headline "MayteraOS", content y=262, 48 px BOLD, #FFFFFF.
        // 48 is a real rasterizer bucket (12/14/16/18/20/24/28/32/48/96; asking
        // for 40 would silently render at 32). draw_centered_face() resolves the
        // REAL DejaVu Sans Bold outline rather than the +1 px smear the style
        // flag applies to face 0, and measures by summing that face's own glyph
        // advances plus kerning, because ttf_measure() uses the active face at
        // NORMAL style only and would centre this line wrong.
        draw_centered_face(win, 320, 262 - HDR_H, b"MayteraOS", 48, 0xFFFFFF);

        // 5. Subtitle, content y=340, 20 px, #FFFFFF @ 0.92.
        let sy = 340 - HDR_H;
        let subtitle_col = wel_blend_over(0xFFFFFF, wel_composite_at(W / 2, sy), 920);
        // #126: the reduced flow is not setting up a machine, it is setting
        // up a DESKTOP for a person who has just signed in for the first time.
        // Saying "let's get it set up" to someone on a machine that is already
        // set up describes the wrong thing.
        dk_centered(win, sy,
            if personalise() { b"Let's set up your desktop the way you like it.\0" }
            else             { b"Your AI-first desktop. Let's get it set up.\0" },
            20, false, subtitle_col);

        // 6. Primary button pill: x=197, content y=420, 246x51, r=25.
        // Fill #6AE2CF @ 0.20, edge #FFFFFF @ 0.34. The fill is deliberately
        // LOWER than the mockup's ~0.28: the pill is defined by its edge, not
        // its fill (the fill measures only 1.48:1 against the glass at either
        // value), and a lighter fill raises the surface the white label sits on,
        // so LOWERING it raises label contrast, 4.91 -> 5.80.
        let (bx, by, bw, bh) = (197, 420 - HDR_H, 246, 51);
        let pill_bg = wel_composite_at(bx + bw / 2, by + bh / 2);
        let edge_col = wel_blend_over(0xFFFFFF, pill_bg, CARD_EDGE_A);
        let fill_col = wel_blend_over(DK_ACCENT, pill_bg, 200);
        // Ring-then-hole, the same construction dk_radio() already uses, because
        // frame_inward() would put a SQUARE frame on a round pill.
        gui_rr(win, bx, by, bw, bh, 25, edge_col, pill_bg);
        gui_rr(win, bx + 1, by + 1, bw - 2, bh - 2, 24, fill_col, edge_col);

        // 6a/6b. Label at 20 px and the arrow glyph, which is drawn with strokes
        // rather than typed: nothing guarantees the loaded TTF carries an arrow
        // codepoint, and a guessed codepoint that renders as a blank box is not
        // closer to the spec than saying what was done instead.
        //
        // MEASURED CORRECTION, stated rather than silently absorbed. The spec
        // puts the label at content y=433 and the arrow box at 445, on the
        // convention that y is the top of the CAP-HEIGHT box. This rasterizer's
        // y is the top of the LINE box, so the two differ by the ascent slack:
        // rendered and measured on real pixels at 1280x800, the label's ink
        // landed 2.5 px ABOVE the pill's centre and the arrow 5 px BELOW it.
        // Both are therefore derived FROM the pill, which is the thing they have
        // to look centred in, so they cannot drift if the pill ever moves.
        let lw = unsafe { gui_ttf_width(b"Get Started\0".as_ptr(), 20) };
        let lcx = 320 - 14;
        dk_solid(win, lcx - lw / 2, by + 16, b"Get Started\0", 20, false, 0xFFFFFF);
        let ax = lcx + lw / 2 + 8;
        let acy = by + bh / 2 + 1;
        dk_line(win, ax, acy, ax + 18, acy, 2, 0xFFFFFF);
        dk_line(win, ax + 12, acy - 5, ax + 18, acy, 2, 0xFFFFFF);
        dk_line(win, ax + 12, acy + 5, ax + 18, acy, 2, 0xFFFFFF);

        self.wel_btn = (bx, by, bw, bh);

        // The two corner chips ("Accessibility Settings", "Language") from the
        // spec's section 8 are DELIBERATELY NOT DRAWN. There is no accessibility
        // surface and no language picker behind either of them, and the spec
        // itself says so: "a focusable pill that does nothing when activated is
        // worse than no pill... If page 1 ships before the targets do, omit both
        // chips; the layout does not depend on them."
    }
}

// ---------------------------------------------------------------------------
// Dark wizard pages (#745 dark-page port, docs/OOBE_DARK_PAGES.html).
//
// PG_ACCOUNT, PG_SIGNIN, PG_NETWORK, PG_APPEAR, PG_WALL, PG_AI, PG_APPLY and
// PG_DONE all move to the same treatment already shipping on PG_WELCOME
// (page 0) above: the same 3-stop gradient + glow background, the same
// wel_composite_at()/wel_blend_over() software-alpha compositing (this
// compositor has no real alpha channel, so every "@alpha" spec colour is
// pre-blended against the known background before being handed to a draw
// primitive as a flat RGB), the same wel_text_tracked()-style tracked-caps
// helper, and gui_fill_rounded_grad() for every gradient fill (buttons, the
// avatar, the Done check circle) rather than a hand-rolled gradient.
//
// PG_TIME (Date & Time, page 4) had NO table in docs/OOBE_DARK_PAGES.html -
// the spec's own page count is 9 (Welcome..Finish) and PG_TIME sits between
// PG_NETWORK and PG_APPEAR with no equivalent anywhere in that document. It
// was left on the OLD light chrome (draw_time_light(), left below unused
// now, same as rich_list()/wall_grid() from the earlier port) and flagged
// as a real, reported gap rather than silently patched with an invented
// palette.
//
// #745 follow-up: ported here as dk_draw_time(), DERIVED from the tables
// and helpers the other 8 pages already established rather than a new
// treatment - dk_page_chrome() for the shared title/subtitle/back/primary/
// dots frame, and dk_list() (below dk_thumb_cell()) for the one genuinely
// new widget this page needs: a dark scrolling single-select list, built
// from the exact selection grammar dk_radio()/dk_theme_card() already
// establish (dim resting stroke, bright teal selected stroke, plus a
// redundant shape signal - colour never carries the state alone).
// ---------------------------------------------------------------------------

// Section 1 token table (docs/OOBE_DARK_PAGES.html).
const DK_FIELD_LABEL: u32      = 0x8FCFC0;
// #745 CONTRAST, MEASURED ON THE COMPOSITED PIXEL, NOT THE TOKEN.
// The glass card is GLASS_TINT #122420 at 0.80 over the live wallpaper, so the
// surface these inks land on is not a constant: it runs from rgb(14,28,25) over
// a black wallpaper to rgb(65,79,76) over a white one. That envelope is only
// 2.04:1 wide, and glass luminance is monotone in wallpaper luminance (verified
// by sweeping all 256 greys through wel_blend_over()), so for a SINGLE flat ink
// the two endpoints do bound it and the BRIGHT end is always the binding one.
//
// The old #7FA79A was measured against the opaque gradient this card replaced
// and was never re-measured: over the bright end it is 3.23:1 at full opacity
// and 2.74:1 at the 0.85 the pages applied, against a 4.5:1 floor for body text
// (WCAG 1.4.3; the 10px/9px sizes here all render at 12px, see dk_fit_label, so
// no large-text exemption applies). #ADC7BF measures 4.78:1 over the bright end
// and 9.76:1 over the dark end, and stays visibly below DK_BODY (5.51:1) so the
// body/fine-print hierarchy survives.
//
// THE 0.85 MULTIPLIER IS GONE, AND THAT IS THE STRUCTURAL HALF OF THE FIX.
// Alpha pulls the ink TOWARDS the surface, so on the glass it is a contrast
// tax. Solved for: the darkest ink of this hue that clears 4.5:1 at alpha 0.85
// is #C1D4CE, which is BRIGHTER than DK_BODY. Any legible fine print at 0.85
// would have to outrank the body text it sits under, so 0.85 is unaffordable
// here at any colour and every fine-print run is drawn at full opacity.
const DK_FINE_PRINT: u32       = 0xADC7BF;
const DK_INPUT_FILL: u32       = 0x213B34;
const DK_INPUT_BORDER: u32     = 0x4E7168;
const DK_INPUT_FOCUS: u32      = 0x6AE2CF;
const DK_INPUT_TEXT: u32       = 0xEAF6F2;
const DK_INPUT_PH: u32         = 0x87ABA2;
const DK_INPUT_FILL_DIS: u32   = 0x121F1C;
const DK_INPUT_BORDER_DIS: u32 = 0x31504A;
const DK_INPUT_TEXT_DIS: u32   = 0x5C7A72;
// #745: this token is NO LONGER a boundary against the glass. It now has one
// caller, the dock schematic's inner "screen" rect, which is a drawn detail
// INSIDE an already-bounded card and sits on DK_CARD_FILL (1.79:1), a pair the
// glass port never touched. Every stroke that is the WHOLE boundary of a
// component sitting on the glass moved to DK_EDGE_GLASS below. Do not re-use
// this one on the glass: it measures 1.81:1 over a black wallpaper and 1.13:1
// over a white one.
const DK_STROKE_UNSEL: u32     = 0x2C4A44;   // dock-schematic inner screen rect, on DK_CARD_FILL
const DK_STROKE_UNSEL_B: u32   = 0x4A6B64;   // radio ring / back-button border resting stroke
// #745 THE RESTING BOUNDARY OF ANYTHING DRAWN ON THE GLASS. Non-text floor,
// 3:1 (WCAG 1.4.11), NOT 4.5:1: these are 1px outlines and pushing a hairline
// to the text floor turns the wizard into a wireframe.
//
// WHY NO FILL CAN DO THIS JOB, PROVED RATHER THAN ASSERTED. An opaque
// DK_CARD_FILL card measures 1.01:1 against the glass over a black wallpaper
// (the two colours are within a level of each other by construction) and 2.02:1
// over a white one, so the shipping theme grid and wallpaper grid had NO resting
// boundary at all on a dark desktop. Darkening cannot rescue it either: pure
// #000000 reaches only 2.45:1 against the BRIGHTEST glass this card can produce,
// so no dark colour clears 3:1 anywhere in the envelope. That also kills the
// two-member boundary pair that fixed the lock-screen password field, where the
// backdrop was the raw wallpaper spanning black to white and a dark halo could
// carry the bright end. Here only a LIGHT member can ever carry anything, so a
// pair degenerates to a single light stroke, which is what this is.
//
// It is deliberately LOUDER than the card's own 1px white @0.34 edge, and that
// is not an inconsistency. The card edge is not load-bearing at the bright end
// (it measures 2.42:1 there); the card boundary is carried by the glass against
// the bare wallpaper, 8.58:1. A grid cell has glass on BOTH sides, so its stroke
// carries the boundary alone and has to clear the floor by itself.
//
// #6FA99E: 3.21:1 over the bright end, 6.54:1 over the dark end. Flat, not
// alpha-over-the-local-glass: a tracking stroke would be quieter over a dark
// wallpaper but its guarantee would depend on the backdrop SAMPLE matching the
// pixel under the stroke, and an accessibility floor must not carry an
// unbounded error term. The flat value's floor follows from the envelope alone.
const DK_EDGE_GLASS: u32       = 0x6FA99E;
const DK_ACCENT: u32           = 0x6AE2CF;
const DK_TOGGLE_TRACK_OFF: u32 = 0x1C2E2A;
const DK_TOGGLE_THUMB_OFF: u32 = 0x7C9C94;
const DK_TOGGLE_THUMB_ON: u32  = 0xF3FBF9;
const DK_CARD_FILL: u32        = 0x0E1D1B;
const DK_BADGE_FILL: u32       = 0x123B32;
const DK_BADGE_TEXT: u32       = 0x6AE2CF;
const DK_THUMB_HALO: u32       = 0x050A09;
const DK_PROGRESS_TRACK: u32   = 0x16241F;
const DK_BACK_BORDER: u32      = 0x4A6B64;
const DK_BACK_LABEL: u32       = 0xA9D9CC;
const DK_HEADLINE: u32         = 0xF3FBF9;
const DK_BODY: u32             = 0xA9D9CC;
// #745: 0xAAAAAA was 3.69:1 on the bright end of the glass at full opacity and
// was drawn at 0.85 (3.11:1). #C6C6C6 is 5.02:1 / 10.26:1. Text floor, 4.5:1.
const DK_LINK: u32             = 0xC6C6C6;
const DK_EYEBROW: u32          = 0x6AE2CF;
const DK_BTN_TOP: u32          = 0x0F8068;
const DK_BTN_BOTTOM: u32       = 0x0A5D4C;
const DK_BTN_TEXT: u32         = 0xFFFFFF;
// NOT in any spec table: validation/apply-error text still has to render
// somewhere on these pages (bad IP, taken username, a failed sub-step), and
// the spec never shows an error state. Reuses the DARK palette's existing
// error red (defined above for the machine-dark Pal, never previously
// drawn anywhere) rather than inventing an unreviewed hex. Flagged as an
// extrapolation in the #745 port report, not a literal spec value.
// #745: on the glass this is TEXT (validation and apply failures) and it was
// 2.52:1 over the bright end at FULL opacity, the worst text pair in the file.
// #FFAAA2 is 4.71:1 / 9.61:1. It is also the ST_FAIL status disc, a non-text
// indicator needing 3:1, which the same value clears with room to spare.
const DK_ERROR: u32            = 0xFFAAA2;

fn frame_inward(win: i32, x: i32, y: i32, w: i32, h: i32, t: i32, c: u32) {
    rect(win, x, y, w, t, c);
    rect(win, x, y + h - t, w, t, c);
    rect(win, x, y, t, h, c);
    rect(win, x + w - t, y, t, h, c);
}

fn dk_hr(win: i32, x: i32, y: i32, w: i32) { rect(win, x, y, w, 1, 0x1E322E); }

// Row 1 of every page: the backdrop, plus the card chrome that is now shared
// by all ten of them. Every page already called this first, so repurposing it
// keeps ONE hook instead of adding three new calls to ten pages.
//
// In CARD_MODE this paints the glass card (blurred+tinted wallpaper, rounded
// corners, edge stroke, top highlight) and then the header counter and footer
// dots. In the legacy 640x480 fallback it is the previous gradient loop,
// unchanged, and the counter is omitted because there is no header strip to put
// it in; the dots stay where they always were, inside the body.
//
// NOTE FOR ANYONE ADDING A PAGE: on a translucent surface a row that is not
// hovered has NO background of its own, the surface IS the background. Any
// fill(rect, SURFACE_BG) on the glass is not a background, it is an eraser.
fn dk_fill_bg(win: i32, page: usize) {
    unsafe {
        if CARD_MODE {
            card_paint_backdrop(win);
            card_chrome(win, page);
            return;
        }
    }
    let mut y = 0;
    while y < H { rect(win, 0, y, W, 1, wel_composite_at(0, y)); y += 1; }
    if !is_step_page(page) { return; }
    dk_step_dots(win, step_index(page), 458);
}

fn is_step_page(page: usize) -> bool { step_index(page) >= 0 }

// Header counter and footer dots. Derived, never hardcoded: PG_APPLY and
// PG_DONE are not in STEP_PAGES, so step_index() returns -1 and they get
// neither, which is exactly what the spec asks for and is now impossible to get
// wrong by editing one of the two in isolation.
fn card_chrome(win: i32, page: usize) {
    let idx = step_index(page);
    if idx < 0 { return; }
    // Content-local y 8 for the counter and 548 for the dots; both are
    // expressed body-local (content-local minus the 48 px header strip) so they
    // go through the same translation and the same sampler as everything else.
    let mut buf: [u8; 24] = [0; 24];
    let mut k = 0usize;
    let pre = b"Step ";
    while k < pre.len() { buf[k] = pre[k]; k += 1; }
    let one = idx + 1;
    if one >= 10 { buf[k] = b'0' + (one / 10) as u8; k += 1; }
    buf[k] = b'0' + (one % 10) as u8; k += 1;
    let mid = b" of ";
    let mut j = 0usize; while j < mid.len() { buf[k] = mid[j]; k += 1; j += 1; }
    let tot = step_count() as i32;
    if tot >= 10 { buf[k] = b'0' + (tot / 10) as u8; k += 1; }
    buf[k] = b'0' + (tot % 10) as u8; k += 1;
    buf[k] = 0;
    let cw = unsafe { gui_ttf_width(buf.as_ptr(), 24) };
    dk_solid(win, (W - cw) / 2, 8 - HDR_H, &buf, 24, false, DK_ACCENT);
    dk_step_dots(win, idx, 548 - HDR_H);
}

fn dk_solid(win: i32, x: i32, y: i32, s: &[u8], size: i32, bold: bool, color: u32) {
    text_ex(win, x, y, s, if bold { TTF_STYLE_BOLD } else { TTF_STYLE_NORMAL }, size, color);
}
// For tokens with alpha < 1.0: blend against the REAL composited background
// at this row (spec section 2's method note - never against a flat
// constant) before handing a flat colour to the draw primitive.
fn dk_alpha(win: i32, x: i32, y: i32, s: &[u8], size: i32, bold: bool, fg: u32, alpha_permille: i32) {
    let col = wel_blend_over(fg, wel_composite_at(x, y), alpha_permille);
    dk_solid(win, x, y, s, size, bold, col);
}
fn dk_centered(win: i32, y: i32, s: &[u8], size: i32, bold: bool, color: u32) {
    let w = unsafe { gui_ttf_width(s.as_ptr(), size) };
    dk_solid(win, (W - w) / 2, y, s, size, bold, color);
}
fn dk_centered_alpha(win: i32, y: i32, s: &[u8], size: i32, bold: bool, fg: u32, alpha_permille: i32) {
    // Centred text: sample at the content-box centre, which is where it lands.
    let col = wel_blend_over(fg, wel_composite_at(W / 2, y), alpha_permille);
    dk_centered(win, y, s, size, bold, col);
}
// Field-caption label: switches to the disabled-text token when its field is
// disabled (docs/OOBE_DARK_PAGES.html page-4 mock inlines exactly this on
// its disabled-example caption), not just the input box itself.
fn dk_label(win: i32, x: i32, y: i32, s: &[u8], disabled: bool) {
    dk_solid(win, x, y, s, 10, true, if disabled { DK_INPUT_TEXT_DIS } else { DK_FIELD_LABEL });
}

// Text input / secret-entry field: the four states from spec section 1+3
// (default, focus, disabled, placeholder). Border drawn INWARD (frame()'s
// existing convention in this file) so a state change never shifts the
// field's outer geometry. Masked glyphs are solid 4px dots at 8px pitch in
// the entered-text colour, per spec section 3 - never a font glyph, so the
// mask never depends on the TTF having a bullet character.
fn dk_field(win: i32, x: i32, y: i32, w: i32, h: i32, f: &Field, placeholder: &[u8],
            focused: bool, disabled: bool) {
    let focused = focused && !disabled;
    let outer = wel_composite_at(x + w / 2, y + h / 2);
    // #745 glass: a focused field or spinner keeps its NORMAL 1 px border and
    // takes the same outside ring as every other focusable control, instead of
    // the old 2 px inward border. The inward border was the same grammar grids use for
    // SELECTION, which is exactly the conflation the ring exists to end.
    let (fill, border, bw) = if disabled { (DK_INPUT_FILL_DIS, DK_INPUT_BORDER_DIS, 1) }
        else { (DK_INPUT_FILL, DK_INPUT_BORDER, 1) };
    gui_rr(win, x, y, w, h, 4, fill, outer);
    frame_inward(win, x, y, w, h, bw, border);
    if focused { dk_focus_frame(win, x, y, w, h); }

    let text_col = if disabled { DK_INPUT_TEXT_DIS } else { DK_INPUT_TEXT };
    let ty = y + (h - 12) / 2 - 1;
    if f.mask {
        let cy = y + h / 2;
        // #745 follow-up: cap the caret to the same 40-dot visible count as
        // the dots themselves (a real Anthropic-length key is well past 40
        // characters; without this the caret would draw outside the field).
        let ndots = f.n.min(40);
        let mut i = 0;
        while i < ndots {
            unsafe { gui_fill_circle_aa(win, x + 12 + (i as i32) * 8, cy, 2, text_col, fill); }
            i += 1;
        }
        if focused { rect(win, x + 12 + (ndots as i32) * 8 + 3, y + 6, 1, h - 12, DK_INPUT_FOCUS); }
    } else if f.n > 0 {
        dk_solid(win, x + 10, ty, f.cstr(), 12, false, text_col);
        if focused {
            let cw = unsafe { gui_ttf_width(f.cstr().as_ptr(), 12) };
            rect(win, x + 10 + cw + 1, y + 6, 1, h - 12, DK_INPUT_FOCUS);
        }
    } else {
        if !placeholder.is_empty() {
            let ph_col = if disabled { DK_INPUT_TEXT_DIS } else { DK_INPUT_PH };
            dk_solid(win, x + 10, ty, placeholder, 12, false, ph_col);
        }
        if focused { rect(win, x + 11, y + 6, 1, h - 12, DK_INPUT_FOCUS); }
    }
}

// Static, non-interactive masked-dot preview (the mini "what you'll see"
// password field on PG_SIGNIN) - a fixed dot count, not tied to the real
// password length, since revealing length is exactly what masking exists to
// avoid and this is an illustration, not a real input.
fn dk_mask_preview(win: i32, x: i32, y: i32, w: i32, h: i32, n: i32) {
    let outer = wel_composite_at(x + w / 2, y + h / 2);
    gui_rr(win, x, y, w, h, 4, DK_INPUT_FILL, outer);
    frame_inward(win, x, y, w, h, 1, DK_INPUT_BORDER);
    let cy = y + h / 2;
    let start_x = x + (w - (n * 8 - 4)) / 2;
    let mut i = 0;
    while i < n { unsafe { gui_fill_circle_aa(win, start_x + i * 8, cy, 2, DK_INPUT_TEXT, DK_INPUT_FILL); } i += 1; }
}

// Selection grammar shared by radios, theme cards and wallpaper thumbnails:
// dim resting stroke, bright teal selected stroke, PLUS a redundant shape
// signal (inner dot / doubled border / checkmark badge) - colour never
// carries the state alone (spec section 3).
fn dk_radio(win: i32, cx: i32, cy: i32, selected: bool) {
    let outer = wel_composite_at(cx, cy);
    // #745: the unselected ring is the ENTIRE control, drawn straight on the
    // glass, so it takes the 3:1 non-text floor on its own. DK_STROKE_UNSEL_B
    // measured 1.46:1 over a white wallpaper here.
    let ring = if selected { DK_ACCENT } else { DK_EDGE_GLASS };
    unsafe {
        gui_fill_circle_aa(win, cx, cy, 6, ring, outer);
        gui_fill_circle_aa(win, cx, cy, 5, outer, ring);
        if selected { gui_fill_circle_aa(win, cx, cy, 3, DK_ACCENT, outer); }
    }
}

// #745 GROUP-LEVEL KEYBOARD FOCUS FRAME. NEW SHARED PRIMITIVE for this
// wizard, deliberately placed with the other dk_* helpers rather than
// private to PG_SIGNIN, so the next page that needs focus on something
// larger than a single input uses this one instead of copying it.
//
// 2px accent, drawn 2px OUTSIDE the content box. Four filled rects, which
// is what the spec's geometry table asks for - no stroke primitive is
// needed and none is invented.
//
// It is a GROUP frame and not a third radio appearance on purpose: moving
// the cursor inside a radio pair SELECTS immediately, so there is no
// browse-without-committing state for a radio to show. Each radio therefore
// has exactly two appearances, and "the keyboard is somewhere but the radios
// look the same whether or not I am in them" is answered by this instead.
//
// ON REUSING THE ACCENT COLOUR, which is also the SELECTION colour. That
// conflation is a real defect on the wizard's GRIDS, where one element can
// be both selected and focused and the two states paint it identically. It
// cannot arise here, because focus and selection are drawn on DIFFERENT
// OBJECTS in different grammars: selection is a 13px ring plus an inner dot
// plus a bold near-white label, on a radio; focus is a 584x84 frame around
// a whole group, on a rect that has no border at rest, and exactly one is
// ever on screen. Nothing on this page can be mistaken for the other.
// The colour is the spec's, and it is the only value on this page measured
// against the composited background (9.36:1 group 1, 8.99:1 group 2);
// inventing a second focus hue here would ship an unmeasured colour to
// solve a problem this page does not have. The GRID conflation is real and
// is a separate fix on the pages that have it.
// #745 glass: the ring now sits 2 px OUTSIDE the control with a 2 px gap,
// which is what stops focus and selection meaning the same thing. Both are
// #6AE2CF (a second focus hue would be an unmeasured colour), so they are told
// apart by GEOMETRY instead: focus is 2 px outside the control edge, selection
// is a 3 px border INSET on it plus a 15% fill. Opposite sides of the edge,
// different weights, and a control that is both shows both, nested, and still
// reads correctly. Changed HERE, once, so every focusable control in the wizard
// moves together rather than one page at a time.
fn dk_focus_frame(win: i32, x: i32, y: i32, w: i32, h: i32) {
    frame_inward(win, x - 4, y - 4, w + 8, h + 8, 2, DK_ACCENT);
}

// #745 task #15 (docs/OOBE_APPS_WIDGETS.html section 8.3): the wizard's first
// checkbox primitive - dk_radio() exists, nothing before this drew a square
// check. Unchecked is a hollow 2px inward frame with NO FILL: the obvious
// DK_INPUT_FILL/DK_INPUT_BORDER pair measures 1.43:1/1.44:1 against the glass
// (see the "no card fill behind an unchecked item" contrast finding, same
// spec section) and would make an unchecked box disappear over both a bright
// and a dark wallpaper - the one thing a checkbox must never be. Checked is a
// solid DK_ACCENT square with a DK_CARD_FILL checkmark, the same
// ring-and-glyph construction dk_status_icon()'s "done" state already uses.
// Used by both the apps grid and the widgets list (spec: "one checkbox in the
// wizard, not two").
fn dk_checkbox(win: i32, x: i32, y: i32, checked: bool) {
    if !checked {
        frame_inward(win, x, y, 12, 12, 2, DK_BODY);
        return;
    }
    rect(win, x, y, 12, 12, DK_ACCENT);
    dk_checkmark(win, x + 6, y + 6, 3, DK_CARD_FILL);
}

// Radio label. Selection is carried by THREE independent signals and never
// by colour alone: the ring colour, the inner dot appearing (a shape change),
// and the label going bold and near-white (a weight change).
fn dk_opt_label(win: i32, x: i32, y: i32, s: &[u8], sel: bool) {
    if sel { dk_solid(win, x, y, s, 12, true, DK_INPUT_TEXT); }
    else   { dk_alpha(win, x, y, s, 12, false, DK_BODY, 880); }
}

// Per-option sub-line, at FULL OPACITY.
//
// #745 CORRECTION: the numbers this comment used to carry (4.27:1 at alpha
// 0.85, 5.30:1 at 1.0, 5.07:1 at y=326) were measured against the OPAQUE
// GRADIENT with its glow plateau, and that background no longer exists in
// CARD_MODE. On the glass the relevant pair is the ink against the composited
// card surface, whose bright end is rgb(65,79,76) regardless of y, so the row
// dependence those numbers described is gone. What survived is the conclusion:
// full opacity, which now measures 4.78:1 at the bright end of the envelope
// (DK_FINE_PRINT's own comment has the derivation). Full opacity alone was
// never enough, though, and believing it was is what left this page shipping a
// 3.23:1 failure while looking like the page that had already been fixed: the
// TOKEN had to move too. The legacy 640x480 fallback still draws the gradient,
// where the same ink measures better than it did, so nothing regressed there.
//
// The colour does NOT change with selection: the sub-line is a fact about the
// option, not a state indicator.
fn dk_opt_sub(win: i32, x: i32, y: i32, s: &[u8]) {
    dk_solid(win, x, y, s, 10, false, DK_FINE_PRINT);
}

// "Sign in automatically as <username>", shortened so the run never passes
// x=608. The USERNAME is what shrinks, never the surrounding words: the words
// are the sentence and the name is the variable.
//
// DEVIATION, stated rather than hidden: the spec says "a single ellipsis
// character". This whole text pipeline is ASCII bytes (dk_solid takes &[u8]
// and gui_ttf_width measures the same bytes), and nothing guarantees the
// loaded TTF carries U+2026, so the marker is three ASCII dots. Guessing a
// codepoint that renders as a blank box is not closer to the spec than
// saying what was done instead.
fn signin_auto_label(out: &mut [u8; 192], user: &Field) -> usize {
    const AVAIL: i32 = 608 - 52;
    let head = put(out, 0, b"Sign in automatically as ");
    let mut take = user.n;
    loop {
        let mut m = head;
        let mut i = 0usize;
        while i < take && m + 4 < out.len() { out[m] = user.b[i]; m += 1; i += 1; }
        if take < user.n { m += put(out, m, b"..."); }
        out[m] = 0;
        if take == 0 { return m; }
        if unsafe { gui_ttf_width(out.as_ptr(), 12) } <= AVAIL { return m; }
        take -= 1;
    }
}

fn dk_toggle(win: i32, x: i32, y: i32, on: bool) {
    let outer = wel_composite_at(x + 17, y + 8);
    if on { unsafe { gui_fill_rounded_grad(win, x, y, 34, 17, 9, DK_BTN_TOP, DK_BTN_BOTTOM); } }
    else { gui_rr(win, x, y, 34, 17, 9, DK_TOGGLE_TRACK_OFF, outer); }
    let (tcx, tcol, tbg) = if on { (x + 25, DK_TOGGLE_THUMB_ON, DK_BTN_BOTTOM) }
                           else { (x + 8,  DK_TOGGLE_THUMB_OFF, DK_TOGGLE_TRACK_OFF) };
    unsafe { gui_fill_circle_aa(win, tcx, y + 8, 6, tcol, tbg); }
}

// Small up/down triangle, stamped as three shrinking/growing 1px rows - the
// same "no diagonal-line primitive, stamp rects" idiom dk_line()/
// dk_checkmark() below already use. Used for spinner arrows and the NTP
// combo's chevron.
fn dk_tri(win: i32, cx: i32, cy: i32, up: bool, color: u32) {
    let rows: [i32; 3] = if up { [6, 4, 2] } else { [2, 4, 6] };
    let mut i = 0i32;
    while i < 3 {
        let ww = rows[i as usize];
        rect(win, cx - ww / 2, cy - 1 + i, ww, 1, color);
        i += 1;
    }
}

// Date/time spinner field: same fill/border/focus/disabled grammar as
// dk_field() (reused rather than duplicated - default/focus/disabled all
// come from the same three token trios), plus small up/down arrows in the
// right ~9px. Up/Down ARROW KEYS on the focused field do the same +-1 step
// as clicking these (the primary path, pointer input is unreliable on this
// platform, #334).
fn dk_spin(win: i32, x: i32, y: i32, w: i32, h: i32, val: &[u8], focused: bool, disabled: bool) {
    let focused = focused && !disabled;
    let outer = wel_composite_at(x + w / 2, y + h / 2);
    // #745 glass: a focused field or spinner keeps its NORMAL 1 px border and
    // takes the same outside ring as every other focusable control, instead of
    // the old 2 px inward border. The inward border was the same grammar grids use for
    // SELECTION, which is exactly the conflation the ring exists to end.
    let (fill, border, bw) = if disabled { (DK_INPUT_FILL_DIS, DK_INPUT_BORDER_DIS, 1) }
        else { (DK_INPUT_FILL, DK_INPUT_BORDER, 1) };
    gui_rr(win, x, y, w, h, 4, fill, outer);
    frame_inward(win, x, y, w, h, bw, border);
    if focused { dk_focus_frame(win, x, y, w, h); }

    let text_col = if disabled { DK_INPUT_TEXT_DIS } else { DK_INPUT_TEXT };
    let tw = unsafe { gui_ttf_width(val.as_ptr(), 12) };
    let avail = (w - 11).max(0);
    dk_solid(win, x + (avail - tw).max(0) / 2, y + (h - 12) / 2 - 1, val, 12, false, text_col);

    let arrow_col = if disabled { DK_INPUT_TEXT_DIS } else { DK_INPUT_PH };
    let ax = x + w - 6;
    dk_tri(win, ax, y + h / 4, true, arrow_col);
    dk_tri(win, ax, y + h - h / 4, false, arrow_col);
}

// #745: slug_is() and theme_swatch_colors() USED to live here: a hardcoded
// 14-entry table of (titlebar, body) colours keyed by theme slug, transcribed
// by hand out of the .mtheme files. It was a copy of theme data, so it could
// only ever be as fresh as the last time someone re-transcribed it, and it had
// nothing to say about a theme the App Store installs. The card now reads each
// theme's REAL tokens live through libc's gui_theme_win_preview(), so there is
// no table to go stale and no slug to special-case.

// Small selected-state check badge shared by the theme mini-card and the
// dock-style card (spec table row 6/16: same gradient circle + checkmark,
// same position class, overlapping the card's top-right corner).
fn dk_mini_check_badge(win: i32, x: i32, y: i32, w: i32) {
    let (bx, by) = (x + w - 10, y - 5);
    unsafe { gui_fill_rounded_grad(win, bx, by, 14, 14, 7, DK_BTN_TOP, DK_BTN_BOTTOM); }
    dk_checkmark(win, bx + 7, by + 7, 3, 0xFFFFFFu32);
}

// #745 THEME CARD (108x72). The user asked for "the top right of the window
// decoration (with the minimise close etc. and showing the colours) instead of
// the current view", and the old card was two flat rectangles from a hardcoded
// colour table. Design spec: docs/OOBE_THEME_PREVIEW.html.
//
// The preview itself is NOT drawn here. It is libc's gui_theme_win_preview(),
// which is also what the Settings theme picker calls, so the two surfaces
// cannot disagree about what a theme looks like; and it reads that theme's own
// live tokens (titlebar, border, close button, radius.window chamfer,
// titlebar height/button metrics, gradient stops) rather than any table in this
// file. Note it takes the KERNEL theme index (ThemeEntry.index), not the
// position in this grid: the two are equal today only because
// /THEMES/INDEX.TXT is the same file both sides read.
//
// SELECTION vs FOCUS. This page had them as the same colour on the same
// object, which is the grid conflation dk_focus_frame's comment flags. They are
// now told apart by GEOMETRY, exactly the way every other control in this
// wizard already does it: selection is a 3px border INSET on the card edge plus
// the check badge, focus is a 2px ring 4px OUTSIDE the whole grid (see
// dk_draw_appear). Opposite sides of the edge, different objects, no new hue.
const THC_W: i32 = 108;
const THC_H: i32 = 68;
fn dk_theme_mini_card(win: i32, x: i32, y: i32, selected: bool, name: &[u8], theme_index: i32) {
    let (w, h) = (THC_W, THC_H);   // preview 4..44, label 48..~61, card ends 68
    let outer = wel_composite_at(x + w / 2, y + h / 2);
    gui_rr(win, x, y, w, h, 6, DK_CARD_FILL, outer);

    // The window fragment fills the preview rect: its top and right edges ARE
    // the window's top-right corner, the other two are the crop. The chamfer
    // cut shows DK_CARD_FILL because that is what is genuinely behind it.
    unsafe { gui_theme_win_preview(win, x + 4, y + 4, 100, 40, theme_index, DK_CARD_FILL); }

    if selected { frame_inward(win, x, y, w, h, 3, DK_ACCENT); }
    else        { frame_inward(win, x, y, w, h, 1, DK_EDGE_GLASS); }

    dk_fit_label(win, x, y + 48, w, name, if selected { DK_HEADLINE } else { DK_BODY }, selected);

    if selected { dk_mini_check_badge(win, x, y, w); }
}

// Width of a run AS IT WILL BE DRAWN. gui_ttf_width() wraps ttf_measure(),
// which measures the active face at NORMAL style, but dk_solid(bold) draws
// TTF_STYLE_BOLD, and bold is SYNTHESISED by SMEARING each glyph one pixel
// wider (see resolve_bold_face()'s comment above). So a bold run is measured
// about one pixel per glyph too narrow, which is 19px on "Default
// (MayteraOS)" - measured 99, drawn ~118 - and that is exactly the overflow
// the second verification screendump caught after the first fit attempt.
// Measuring with the right FUNCTION was not enough; the measurement has to
// describe the run that is actually drawn.
fn dk_drawn_width(name: &[u8], size: i32, bold: bool) -> i32 {
    let base = unsafe { gui_ttf_width(name.as_ptr(), size) };
    if !bold { return base; }
    let mut n = 0i32;
    let mut i = 0usize;
    while i < name.len() && name[i] != 0 { n += 1; i += 1; }
    base + n
}

// Centre a card label in `w`, MEASURED, never estimated: label overflow into
// the neighbouring card was part of the original report.
//
// #745 CORRECTED COMMENT. This used to claim the ladder was
//   10px bold -> 10px regular -> 9px regular -> 9px regular truncated with ".."
// i.e. that dropping from 10px to 9px bought width. IT DOES NOT, AND NEVER DID.
// The renderer has exactly ten glyph-size buckets (kernel/gui/ttf.c
// size_cache_sizes: 12,14,16,18,20,24,28,32,48,96) and get_size_cache() snaps
// every request to the CLOSEST one, so 9, 10, 11 and 12 all render at 12px and
// gui_ttf_width() returns the same width for all of them. The 9px try was
// therefore a byte-for-byte repeat of the 10px regular try: it could only ever
// be reached when the 10px regular try had just failed at the identical width,
// and it then failed identically. It is deleted rather than left as a step that
// runs and does nothing.
//
// What the ladder ACTUALLY has is two outcomes plus truncation, and both are
// real: dropping the synthesised bold smear buys exactly one pixel per glyph
// (bold is a +1px smear, not a face, which is why dk_drawn_width() exists at
// all), 19px on "Default (MayteraOS)", and that is enough to fit the common
// cases; truncation is the genuine last resort. Losing the weight is a smaller
// loss than cutting letters off a name, and selection is already carried by the
// 3px inset border and the check badge, so the weight is not the only signal it
// takes with it. NOTE for anyone re-adding a size step: it will be inert until
// size_cache_sizes gains a bucket below 12.
fn dk_fit_label(win: i32, x: i32, y: i32, w: i32, name: &[u8], colour: u32, bold: bool) {
    let avail = w - 6;
    let tries: [(i32, bool); 2] = [(10, bold), (10, false)];
    let mut t = 0usize;
    while t < 2 {
        let (size, b) = tries[t];
        let nw = dk_drawn_width(name, size, b);
        if nw <= avail {
            dk_solid(win, x + (w - nw) / 2, y, name, size, b, colour);
            return;
        }
        t += 1;
    }
    // Truncate one byte at a time, re-measuring, until the name plus ".." fits.
    let mut buf: [u8; 48] = [0; 48];
    let mut n = 0usize;
    while n < name.len() && name[n] != 0 && n < 44 { buf[n] = name[n]; n += 1; }
    while n > 1 {
        n -= 1;
        buf[n] = b'.'; buf[n + 1] = b'.'; buf[n + 2] = 0;
        let tw = dk_drawn_width(&buf, 9, false);
        if tw <= avail {
            dk_solid(win, x + (w - tw) / 2, y, &buf, 9, false, colour);
            return;
        }
        buf[n] = 0;
    }
}

// Dock-style schematic card (100x110). Five layouts, in the compositor's
// DOCK_* enum order: 0 Default (MayteraOS), 1 Lumina, 2 Classic UNIX,
// 3 Retro Bench, 4 Marble. The enum identifiers and the persisted digits are
// unchanged; only the labels are shown, and they come from libc's ONE list
// (gui_dock_style_name), never from an array in this file. #745: this comment
// used to caption each schematic with the third-party desktop name it was
// modelled on, which is exactly how the wrong name got back onto the screen.
// #745 Appearance-page layout, in ONE place: the draw, the hit test and the
// arrow-key nav all read these, so they cannot describe three different grids.
//
// THE BUDGET, and why it is this tight. Back / Skip / Continue are drawn on
// a FIXED row at y FOOTER_Y..FOOTER_Y+FOOTER_H (dk_back_button,
// skip_link_bounds, dk_primary_button - #745 task #38 moved this from a
// hardcoded 428 to that one constant, currently 440), so everything on this
// page has to be finished well above it. The first cut of the 5x3 grid used
// a 78px row pitch and left the dock row at its old 110px height, which put
// the dock cards at 356..466: straight through the button row. That was
// caught by the first screendump, not by reading the code, which is the
// whole reason the screendump happens before the commit is called done.
//   THEME label      82
//   theme rows       96 / 170 / 244, cards 68 tall  -> ends 312
//   DOCK STYLE label 322
//   dock row         336, cards 76 tall             -> ends 412
//   footer buttons   FOOTER_Y..FOOTER_Y+FOOTER_H     (untouched)
const THG_X: i32 = 32;
const THG_Y: i32 = 96;
const THG_COLS: usize = 5;
const THG_ROWS: usize = 3;
const THG_PITCH_X: i32 = 117;   // 5 * 108 + 4 * 9 = 576 = the 32..608 content band
const THG_PITCH_Y: i32 = 74;    // 68 card + 6 gutter
const DOCK_LABEL_Y: i32 = 322;
const DOCK_X_0: i32 = 32;
const DOCK_PITCH_X: i32 = 117;  // same pitch as the theme grid: the dock labels
                                // are the longest strings on the page, and a
                                // 100px card could not hold "Default
                                // (MayteraOS)" without truncating it.
const DOCK_W: i32 = 108;
const DOCK_H: i32 = 76;
const DOCK_Y: i32 = 336;

// The dock names now live in libc (one list, shared with Settings), so they
// arrive as a C string pointer rather than a Rust byte slice. Wrap once here
// rather than changing every schematic in dk_dock_card.
fn dk_dock_card_c(win: i32, x: i32, y: i32, selected: bool, style_idx: usize, name: *const u8) {
    let mut buf: [u8; 40] = [0; 40];
    let mut i = 0usize;
    unsafe { while i < 39 { let b = *name.add(i); if b == 0 { break; } buf[i] = b; i += 1; } }
    buf[i] = 0;
    dk_dock_card(win, x, y, selected, style_idx, &buf);
}

fn dk_dock_card(win: i32, x: i32, y: i32, selected: bool, style_idx: usize, name: &[u8]) {
    let (w, h) = (DOCK_W, DOCK_H);
    let outer = wel_composite_at(x + w / 2, y + h / 2);
    gui_rr(win, x, y, w, h, 6, DK_CARD_FILL, outer);
    if selected { frame_inward(win, x, y, w, h, 3, DK_ACCENT); }
    else        { frame_inward(win, x, y, w, h, 1, DK_EDGE_GLASS); }

    let (sx, sy, sw, sh) = (x + 6, y + 6, w - 12, 48);
    let screen_bg = 0x16241Fu32;
    gui_rr(win, sx, sy, sw, sh, 3, screen_bg, DK_CARD_FILL);
    // #745: deliberately NOT DK_EDGE_GLASS. This hairline is a drawn detail of
    // the schematic INSIDE a card that DK_EDGE_GLASS already bounds; it never
    // touches the glass, and its pair (against DK_CARD_FILL, 1.79:1) is one the
    // glass port did not change. Naming the sites that carry a boundary is the
    // work; sweeping the token is what avoids doing it.
    frame_inward(win, sx, sy, sw, sh, 1, DK_STROKE_UNSEL);

    match style_idx {
        0 => {
            // Classic taskbar: full-width bottom strip + start-button dot.
            rect(win, sx, sy + sh - 6, sw, 6, DK_INPUT_FILL);
            unsafe { gui_fill_circle_aa(win, sx + 5, sy + sh - 3, 2, DK_ACCENT, DK_INPUT_FILL); }
        }
        1 => {
            // Lumina: top menu bar + a floating bottom-centre pill
            // with a visible gap above the true bottom edge.
            rect(win, sx, sy, sw, 4, DK_INPUT_FILL);
            let (pw, ph) = (40, 6);
            gui_rr(win, sx + (sw - pw) / 2, sy + sh - 4 - ph, pw, ph, 3, DK_INPUT_FILL, screen_bg);
        }
        2 => {
            // Classic UNIX: taller bottom strip with two internal divider cuts.
            rect(win, sx, sy + sh - 7, sw, 7, DK_INPUT_FILL);
            rect(win, sx + sw / 3, sy + sh - 7, 1, 7, screen_bg);
            rect(win, sx + 2 * sw / 3, sy + sh - 7, 1, 7, screen_bg);
        }
        3 => {
            // Retro Bench: top bar only, deliberately the sparsest schematic.
            rect(win, sx, sy, sw, 4, DK_INPUT_FILL);
        }
        _ => {
            // Marble: flush opaque top bar + bottom-centre pill with
            // four small accent dots (the ONE new alpha usage in this
            // page's spec, 0.7 over the pill fill).
            rect(win, sx, sy, sw, 4, DK_INPUT_FILL);
            let (pw, ph) = (36, 6);
            let px = sx + (sw - pw) / 2;
            let py = sy + sh - 3 - ph;
            gui_rr(win, px, py, pw, ph, 3, DK_INPUT_FILL, screen_bg);
            let dot_col = wel_blend_over(DK_ACCENT, DK_INPUT_FILL, 700);
            let mut k = 0i32;
            while k < 4 {
                unsafe { gui_fill_circle_aa(win, px + 6 + k * 8, py + ph / 2, 1, dot_col, DK_INPUT_FILL); }
                k += 1;
            }
        }
    }

    // Measured, like the theme label. "Default (MayteraOS)" is the longest
    // string on this page (99px regular, ~118px bold) and it overflowed the old
    // 100px card. The label box is the COLUMN PITCH centred on the card, not the
    // card width: the 9px gutter between two dock cards is dead space nothing
    // else uses, and spending it here is what keeps the default dock style's
    // name whole instead of truncated.
    dk_fit_label(win, x - (DOCK_PITCH_X - w) / 2, y + 58, DOCK_PITCH_X, name,
                 DK_HEADLINE, selected);
    if selected { dk_mini_check_badge(win, x, y, w); }
}

// Short thick line via stamped squares (no diagonal-line primitive exists -
// framebuffer primitives are rects/circles/gradients/text only). Sampling by
// the longer axis is enough for a stroke this short; no need for true
// Bresenham.
fn dk_line(win: i32, x0: i32, y0: i32, x1: i32, y1: i32, t: i32, color: u32) {
    let dx = x1 - x0; let dy = y1 - y0;
    let steps = dx.abs().max(dy.abs()).max(1);
    let mut i = 0;
    while i <= steps {
        let x = x0 + dx * i / steps;
        let y = y0 + dy * i / steps;
        rect(win, x - t / 2, y - t / 2, t, t, color);
        i += 1;
    }
}

// Hand-drawn checkmark: a short down-right leg then a long up-right leg,
// each a stamped diagonal stroke (see dk_line). REPLACES an earlier two-
// axis-aligned-rects version copied from PG_DONE's original light-theme
// icon (rect(310,168,3,10)+rect(313,172,14,3) around a 48px circle) - that
// shape is two PERPENDICULAR bars, which reads as an "I-beam"/dash at small
// icon size but is unmistakably NOT a checkmark once scaled to the 72px
// Finish-page circle (confirmed visually on VM <vmid>, screendump p_done).
// A real diagonal reads correctly at every size used here (13px status
// icon, 16px thumbnail badge, 72px Finish circle).
fn dk_checkmark(win: i32, cx: i32, cy: i32, scale_tenths: i32, color: u32) {
    let s = |v: i32| (v * scale_tenths / 10).max(1);
    let t = s(3).max(2);
    dk_line(win, cx - s(9), cy + s(1), cx - s(2), cy + s(8), t, color);
    dk_line(win, cx - s(2), cy + s(8), cx + s(10), cy - s(8), t, color);
}

// Wallpaper thumbnail cell: halo THEN ring THEN badge, in that order, so the
// ring always sits against the fixed near-black halo rather than directly
// against unpredictable photo brightness (spec section 3, the structural
// fix called out explicitly in the port brief).
fn dk_thumb_cell(win: i32, x: i32, y: i32, cell_idx: usize, selected: bool) {
    let outer = wel_composite_at(x + 50, y + 31);
    gui_rr(win, x, y, 100, 62, 3, DK_CARD_FILL, outer);
    unsafe {
        if cell_idx < THUMB_CELLS && THUMB_OK[cell_idx] {
            let src = (core::ptr::addr_of!(THUMBS) as *const u32).add(cell_idx * THUMB_PX) as *const u8;
            draw_image_body(win, x, y, 100, 62, src as i64);
        }
    }
    // #745 RESTING BOUNDARY. There was none: the cell edge was the thumbnail
    // photo meeting the glass, and a dark wallpaper's thumbnail over a dark
    // desktop leaves nothing to see (the DK_CARD_FILL underneath is 1.01:1
    // against the glass there). Drawn AFTER the image so it is not overwritten,
    // INWARD so selecting a cell never moves its geometry, and only when
    // unselected because a selected cell is already bounded by the 3px accent
    // ring 3px further out.
    if !selected { frame_inward(win, x, y, 100, 62, 1, DK_EDGE_GLASS); }
    if selected {
        frame_inward(win, x - 2, y - 2, 104, 66, 1, DK_THUMB_HALO);
        frame_inward(win, x - 3, y - 3, 106, 68, 3, DK_ACCENT);
        let (bx, by) = (x + 100 - 24, y + 62 - 24);
        unsafe { gui_fill_rounded_grad(win, bx, by, 16, 16, 8, DK_BTN_TOP, DK_BTN_BOTTOM); }
        dk_checkmark(win, bx + 8, by + 8, 3, 0xFFFFFFu32);
    }
}

// Dark scrolling single-select list (the PG_TIME timezone list; TZ[] is 35
// rows as of #745, 2026-08-12, though as the "dk_list() ... has ZERO
// callers" note near PG_APPSW below records, this widget is not actually
// wired to PG_TIME's live dark UI - dk_draw_time() uses its own map/
// search-list mechanics instead. Kept for reference, not deleted.).
// The one genuinely new widget the #745 follow-up port needed - built from
// the exact selection grammar dk_radio()/dk_theme_card()/dk_thumb_cell()
// already establish (dim resting stroke, bright teal selected stroke, plus
// a redundant shape signal so state is never colour-only), not a new
// visual language. The list body reuses DK_INPUT_FILL, the same fill token
// every text field on this page already uses, so it reads as the same
// material family as the form rather than a third kind of surface.
//
// Cannot call dk_radio() directly for the row bullet: dk_radio() samples
// wel_composite_at(cy) as its own AA "outer" colour, which is correct for
// every existing caller (radios drawn straight on the gradient background)
// but wrong here, where the bullet sits on the list's own solid
// DK_INPUT_FILL fill, not the raw gradient - using the gradient sample
// would anti-alias the ring's edge against a colour that is not actually
// behind it there. The ring/hole/dot construction (radii 6/5/3) is copied
// verbatim from dk_radio(); only the outer colour differs.
fn dk_list(win: i32, x: i32, y: i32, w: i32, h: i32, count: usize, sel: usize,
           first: usize, name_of: &dyn Fn(usize) -> &'static [u8]) {
    let outer = wel_composite_at(x + w / 2, y + h / 2);
    gui_rr(win, x, y, w, h, 6, DK_INPUT_FILL, outer);
    frame_inward(win, x, y, w, h, 1, DK_INPUT_BORDER);

    // Muted row text: DK_BODY blended at 88% over DK_INPUT_FILL - the row's
    // REAL background, a flat fill, not the page gradient. Measured 6.38:1
    // against DK_INPUT_FILL (passes AA normal-text 4.5:1 with margin).
    // Blending against wel_composite_at() here would measure the WRONG
    // pixel: the list's own fill fully covers the gradient underneath it,
    // the exact mistake already made once on this screen (see WEL_LINK's
    // comment above) and now checked against the real composited pixel.
    let muted = wel_blend_over(DK_BODY, DK_INPUT_FILL, 880);

    let row_h = 28;
    let rows = ((h - 4) / row_h) as usize;
    let mut i = 0usize;
    while i < rows && first + i < count {
        let idx = first + i;
        let ry = y + 2 + (i as i32) * row_h;
        let selected = idx == sel;
        let (cx, cy) = (x + 16, ry + row_h / 2);
        let ring = if selected { DK_ACCENT } else { DK_STROKE_UNSEL_B };
        unsafe {
            gui_fill_circle_aa(win, cx, cy, 6, ring, DK_INPUT_FILL);
            gui_fill_circle_aa(win, cx, cy, 5, DK_INPUT_FILL, ring);
            if selected { gui_fill_circle_aa(win, cx, cy, 3, DK_ACCENT, DK_INPUT_FILL); }
        }
        if selected {
            // Bright stroke around just this row - the same "selection
            // gets its own outline" move dk_theme_card()/dk_thumb_cell()
            // already make, never a solid background fill (which would
            // gamble text contrast against an unpredictable label length).
            // This IS the redundant signal alongside the filled bullet
            // above: two independent, non-colour cues for "selected".
            frame_inward(win, x + 1, ry, w - 2, row_h, 1, DK_ACCENT);
        }
        let col = if selected { DK_INPUT_TEXT } else { muted };
        dk_solid(win, x + 30, ry + (row_h - 12) / 2 - 1, name_of(idx), 12, selected, col);
        i += 1;
    }

    // Scrollbar: track/thumb reuse existing dark tokens rather than adding
    // new ones - DK_PROGRESS_TRACK already IS a quiet recessed-track
    // colour (the apply-page progress bar), and DK_STROKE_UNSEL_B already
    // IS a mid-tone resting stroke (radio ring / back-button border),
    // visible against it without competing with the accent used for
    // selection.
    if count > rows {
        let tx = x + w - 8;
        let tt = y + 2;
        let tl = h - 4;
        gui_rr(win, tx, tt, 6, tl, 3, DK_PROGRESS_TRACK, DK_INPUT_FILL);
        let mut len = tl * (rows as i32) / (count as i32);
        if len < 24 { len = 24; }
        let top = tt + (tl - len) * (first as i32) / ((count - rows) as i32).max(1);
        gui_rr(win, tx, top, 6, len, 3, DK_STROKE_UNSEL_B, DK_PROGRESS_TRACK);
    }
}

// Step dots, card chrome now rather than per-page furniture. The COUNT is
// STEP_COUNT, derived from STEP_PAGES, so it is not possible to draw a number
// of dots that disagrees with the number of steps: the old hardcoded 9 (with
// two pages colliding on dot 3), the welcome page's own hardcoded 5, the dead
// 8-dot version and the C rollback's 7 were four different answers to one
// question.
//
// Geometry: pitch 24, centred on the content box, active d=12 #6AE2CF @1.00,
// inactive d=10 #FFFFFF @0.50. The inactive dot is the weakest ink in the whole
// design and is what set the 0.80 tint: at 0.50 it measures 3.50:1 over a white
// wallpaper against a 3:1 floor for a non-text indicator (the mockup's ~0.34
// measures 2.43:1 and fails). 8 dots span 168 px; at 12 steps it is 264 px,
// still inside the 640 px content box, so growing the wizard does not break it.
fn dk_step_dots(win: i32, active_idx: i32, y: i32) {
    let count = step_count() as i32;
    let span = (count - 1) * 24;
    let x0 = W / 2 - span / 2;
    let mut k = 0i32;
    while k < count {
        let cx = x0 + k * 24;
        let bg = wel_composite_at(cx, y);
        let active = k == active_idx;
        let (r, col) = if active { (6, DK_ACCENT) }
                       else { (5, wel_blend_over(0xFFFFFF, bg, 500)) };
        unsafe { gui_fill_circle_aa(win, cx, y, r, col, bg); }
        k += 1;
    }
}

// #745 task #38: ONE footer-row constant. dk_back_button,
// dk_primary_button, footer_bounds, skip_link_bounds and the
// PG_SIGNIN/PG_APPSW focus frames each hardcoded 428 independently before
// this - the exact WALL_PAGE class of bug (see its own comment above),
// just for a y-coordinate instead of a page size. Window is 480 tall,
// buttons are 28 tall: the old y=428 left a 24px bottom margin; FOOTER_Y=440
// leaves 12px. Checked against every page that shares this footer
// (Account/Signin/Network/Time/Appear/Wall/Appsw/AI) by screendump before
// landing - moving the row down only ever ADDS clearance above it.
const FOOTER_Y: i32 = 440;
const FOOTER_H: i32 = 28;

fn dk_back_button(win: i32) {
    let (x, y, w, h) = (32, FOOTER_Y, 88, FOOTER_H);
    frame_inward(win, x, y, w, h, 1, DK_BACK_BORDER);
    let tw = unsafe { gui_ttf_width(b"Back\0".as_ptr(), 12) };
    dk_solid(win, x + (w - tw) / 2, y + 8, b"Back\0", 12, true, DK_BACK_LABEL);
}

fn dk_primary_button(win: i32, label: &[u8], disabled: bool) {
    let (x, y, w, h) = (468, FOOTER_Y, 140, FOOTER_H);
    let bg = wel_composite_at(x + w / 2, y + h / 2);
    let (top, bot) = if disabled { (wel_blend_over(DK_BTN_TOP, bg, 420), wel_blend_over(DK_BTN_BOTTOM, bg, 420)) }
                     else { (DK_BTN_TOP, DK_BTN_BOTTOM) };
    unsafe { gui_fill_rounded_grad(win, x, y, w, h, 5, top, bot); }
    let tw = unsafe { gui_ttf_width(label.as_ptr(), 12) };
    let tcol = if disabled { wel_blend_over(DK_BTN_TEXT, bot, 420) } else { DK_BTN_TEXT };
    dk_solid(win, x + (w - tw) / 2, y + 8, label, 12, true, tcol);
}

// Checklist status icon (PG_APPLY): 0=pending 2px #2C4A44 ring, 1=in
// progress 2px #6AE2CF ring, 2=done filled #6AE2CF disc + #08120F check.
fn dk_status_icon(win: i32, x: i32, y: i32, state: i32) {
    let (cx, cy) = (x + 6, y + 6);
    let outer = wel_composite_at(cx, cy);
    match state {
        2 => unsafe {
            gui_fill_circle_aa(win, cx, cy, 6, DK_ACCENT, outer);
            dk_checkmark(win, cx, cy, 3, 0x08120Fu32);
        },
        1 => unsafe {
            gui_fill_circle_aa(win, cx, cy, 6, DK_ACCENT, outer);
            gui_fill_circle_aa(win, cx, cy, 4, outer, DK_ACCENT);
        },
        _ => unsafe {
            gui_fill_circle_aa(win, cx, cy, 6, DK_EDGE_GLASS, outer);
            gui_fill_circle_aa(win, cx, cy, 4, outer, DK_EDGE_GLASS);
        },
    }
}

fn fmt_pct(buf: &mut [u8; 8], pct: i32) -> usize {
    let mut n = 0usize;
    let mut x = pct.max(0);
    let mut d: [u8; 4] = [0; 4];
    let mut k = 0usize;
    if x == 0 { d[k] = b'0'; k += 1; }
    while x > 0 && k < 4 { d[k] = b'0' + (x % 10) as u8; x /= 10; k += 1; }
    while k > 0 { k -= 1; buf[n] = d[k]; n += 1; }
    buf[n] = b'%'; n += 1;
    buf[n] = 0;
    n
}

// Footer hit-rects for click/hover, ONE place so drawing, click and hover
// can never disagree. Every non-welcome page (PG_TIME included, as of its
// #745 follow-up dark port) now shares the same dark-spec footer: back
// 32-120, primary 468-608, both at FOOTER_Y..FOOTER_Y+FOOTER_H (#745 task #38).
fn footer_bounds(_page: usize) -> (i32, i32, i32, i32, i32, i32) {
    (32, 120, 468, 608, FOOTER_Y, FOOTER_Y + FOOTER_H)
}

// ===========================================================================
// #745 follow-up: AI ASSISTANT PAGE PROVIDER TABLE (docs/OOBE_AI_PROVIDER.html)
// ===========================================================================
// Replaces the single hardcoded Moonshot/Kimi endpoint and the "Get an API
// key ->" text that had no click handler (the exact complaint that started
// this: a link that was not a link). Selecting a card pre-fills endpoint,
// api_style and a default model so the person never has to know Anthropic
// wants a different header shape than everyone else. Contract this writes
// against is userland/libc/aiclient.c's load_aisvc(): key=value lines
// provider= endpoint= model= api_key= api_style=(bearer|anthropic), no third
// style invented. Custom is bearer-only in the wizard by design (spec
// section 1's note): the rarer anthropic-shaped custom endpoint is one field
// edit away in Settings > AI after setup, not framebuffer-real-estate-
// constrained the way this 640x480 page is.
struct AiProvider {
    name: &'static [u8],           // card name, NUL-terminated for direct draw
    desc: &'static [u8],           // card descriptor line, NUL-terminated
    endpoint: &'static [u8],       // real POST URL written to AISVC.CFG, NUL-terminated
    endpoint_info: &'static [u8],  // shortened read-only "Endpoint host/path - auth" line
    model_default: &'static [u8],  // pre-filled model, NUL-terminated
    style: i32,                    // 0 = bearer, 1 = anthropic (aiclient.c AI_STYLE_*)
    signup: &'static [u8],         // "Get a key at <host>" line, NUL-terminated
    cfg_id: &'static [u8],         // provider= value written to AISVC.CFG, NUL-terminated
}

const AI_STYLE_BEARER: i32 = 0;
const AI_STYLE_ANTHROPIC: i32 = 1;
const AI_CUSTOM: usize = 7;
// Moonshot: today's hardcoded default (API_URL/API_MODEL in aiclient.c), so a
// person who changes nothing on this page gets the exact same outcome as
// before the picker existed (spec Stage A note).
const AI_DEFAULT_PROVIDER: usize = 3;

static AI_PROVIDERS: [AiProvider; 8] = [
    AiProvider { name: b"Anthropic\0", desc: b"Claude - x-api-key\0",
        endpoint: b"https://api.anthropic.com/v1/messages\0",
        endpoint_info: b"Endpoint api.anthropic.com/v1/messages - x-api-key auth\0",
        model_default: b"claude-sonnet-4-5\0", style: AI_STYLE_ANTHROPIC,
        signup: b"Get a key at console.anthropic.com\0", cfg_id: b"anthropic\0" },
    AiProvider { name: b"OpenAI\0", desc: b"GPT models\0",
        endpoint: b"https://api.openai.com/v1/chat/completions\0",
        endpoint_info: b"Endpoint api.openai.com/v1 - Bearer auth\0",
        model_default: b"gpt-4.1\0", style: AI_STYLE_BEARER,
        signup: b"Get a key at platform.openai.com\0", cfg_id: b"openai\0" },
    AiProvider { name: b"OpenRouter\0", desc: b"Many models, 1 key\0",
        endpoint: b"https://openrouter.ai/api/v1/chat/completions\0",
        endpoint_info: b"Endpoint openrouter.ai/api/v1 - Bearer auth\0",
        model_default: b"openrouter/auto\0", style: AI_STYLE_BEARER,
        signup: b"Get a key at openrouter.ai/keys\0", cfg_id: b"openrouter\0" },
    AiProvider { name: b"Moonshot\0", desc: b"Current default\0",
        endpoint: b"https://api.moonshot.cn/v1/chat/completions\0",
        endpoint_info: b"Endpoint api.moonshot.cn/v1 - Bearer auth\0",
        model_default: b"moonshot-v1-8k\0", style: AI_STYLE_BEARER,
        signup: b"Get a key at platform.moonshot.cn\0", cfg_id: b"moonshot\0" },
    AiProvider { name: b"GLM\0", desc: b"Zhipu GLM\0",
        endpoint: b"https://open.bigmodel.cn/api/paas/v4/chat/completions\0",
        endpoint_info: b"Endpoint open.bigmodel.cn/api/paas/v4 - Bearer auth\0",
        model_default: b"glm-4\0", style: AI_STYLE_BEARER,
        signup: b"Get a key at open.bigmodel.cn\0", cfg_id: b"glm\0" },
    AiProvider { name: b"xAI\0", desc: b"Grok models\0",
        endpoint: b"https://api.x.ai/v1/chat/completions\0",
        endpoint_info: b"Endpoint api.x.ai/v1 - Bearer auth\0",
        model_default: b"grok-2-latest\0", style: AI_STYLE_BEARER,
        signup: b"Get a key at console.x.ai\0", cfg_id: b"xai\0" },
    AiProvider { name: b"Gemini\0", desc: b"OpenAI-compat API\0",
        endpoint: b"https://generativelanguage.googleapis.com/v1beta/openai/chat/completions\0",
        endpoint_info: b"Endpoint generativelanguage.googleapis.com/v1beta - Bearer auth\0",
        model_default: b"gemini-2.5-flash\0", style: AI_STYLE_BEARER,
        signup: b"Get a key at aistudio.google.com\0", cfg_id: b"gemini\0" },
    AiProvider { name: b"Custom\0", desc: b"Your own endpoint\0",
        endpoint: b"\0",
        endpoint_info: b"\0",
        model_default: b"\0", style: AI_STYLE_BEARER,
        signup: b"\0", cfg_id: b"custom\0" },
];

// Provider option card, 138x40 (spec table row 5): the 4x2-grid sibling of
// dk_theme_mini_card()/dk_dock_card() - same fill/border/selection grammar
// and the SAME dk_mini_check_badge() overlay, not a re-invented one, just no
// preview swatch (a provider is not a colour scheme).
fn dk_provider_card(win: i32, x: i32, y: i32, w: i32, h: i32, selected: bool,
                     name: &[u8], desc: &[u8]) {
    let outer = wel_composite_at(x + w / 2, y + h / 2);
    let (border_c, border_w) = if selected { (DK_ACCENT, 2) } else { (DK_EDGE_GLASS, 1) };
    gui_rr(win, x, y, w, h, 6, DK_CARD_FILL, outer);
    frame_inward(win, x, y, w, h, border_w, border_c);
    dk_solid(win, x + 10, y + 7, name, 11, true, DK_HEADLINE);
    dk_solid(win, x + 10, y + 22, desc, 9, false, DK_FINE_PRINT);
    if selected { dk_mini_check_badge(win, x, y, w); }
}

// Append s into dst, stopping at the first NUL rather than copying it, so a
// NUL-terminated literal (every AiProvider string) can be spliced into the
// MIDDLE of a larger NUL-terminated buffer without truncating everything
// after it - put()/putf() copy raw byte counts and would embed a stray 0
// here since every name/desc/etc. constant carries its own trailing NUL.
fn put_trim0(dst: &mut [u8], at: usize, s: &[u8]) -> usize {
    let mut i = 0;
    while i < s.len() && s[i] != 0 && at + i < dst.len() { dst[at + i] = s[i]; i += 1; }
    i
}

// Transiently read back the api_key= value already saved in AISVC.CFG (the
// same per-user file aiclient.c's load_aisvc() reads), so a person who does
// not retype their key on this visit keeps it instead of this page silently
// deleting it out from under them. Mirrors load_aisvc()'s own line-splitting
// (userland/libc/aiclient.c). Never drawn, logged or sent anywhere: the only
// two callers are the "is a key already saved" probe (used only to draw the
// fixed-length KEY SET state, never the real bytes) and apply()'s write-back
// into this SAME per-user file.
fn ai_read_saved_key(out: &mut [u8]) -> usize {
    let fd = unsafe { userconf_open_read(b"AISVC.CFG\0".as_ptr(), core::ptr::null()) };
    if fd < 0 { return 0; }
    let mut buf: [u8; 1024] = [0; 1024];
    let n = unsafe { syscall3(SYS_READ, fd as i64, buf.as_mut_ptr() as i64, 1023) };
    unsafe { syscall1(SYS_CLOSE, fd as i64); }
    if n <= 0 { return 0; }
    let n = n as usize;
    let mut i = 0usize;
    while i < n {
        let start = i;
        while i < n && buf[i] != b'\n' { i += 1; }
        let mut end = i;
        if i < n { i += 1; }
        if end > start && buf[end - 1] == b'\r' { end -= 1; }
        let line = &buf[start..end];
        const PFX: &[u8] = b"api_key=";
        if line.len() > PFX.len() && &line[..PFX.len()] == PFX {
            let val = &line[PFX.len()..];
            let vlen = val.len().min(out.len());
            out[..vlen].copy_from_slice(&val[..vlen]);
            return vlen;
        }
    }
    0
}


// ===========================================================================
// #745 NETWORK PAGE: LIVE RESULTS + A STAGED CONNECTION TEST
//
// THE COMPLAINT THIS FIXES: with DHCP selected the four address fields simply
// greyed out and sat EMPTY, so the one moment a person most wants to know what
// the network actually did, the page told them nothing.
//
// WHY A STRUCT AND NOT PARSED TEXT. SYS_NET_INFO (243) already reports the
// netmask, gateway and DNS - as a VERBOSE HUMAN REPORT. Recovering four
// addresses from it means string-matching kernel prose, and that fails in the
// worst direction: reword one label and the parse silently yields nothing
// while this page keeps rendering and keeps saying nothing. No compiler, no
// linker and no test notices. So SYS_NET_STATUS (371) was added instead: a
// #[repr(C)] struct of integers whose layout is locked by a _Static_assert on
// the kernel side and a const assert HERE, both of which go RED at build time
// if it ever drifts. Wording changes cannot break an integer.
//
// NOTHING HERE BLOCKS. Every syscall on this page returns immediately:
// SYS_NET_STATUS reads globals, SYS_NET_PROBE start/poll never sleeps,
// SYS_DNS_START/POLL and SYS_HTTP_FETCH_START/POLL are the existing async
// pairs. sys_ping() (66) is deliberately NOT used: it sleeps the caller for up
// to a second in a hand-rolled poll loop, which is exactly the multi-second
// UI freeze (#211/#212) this project treats as a defect, and it would freeze
// hardest on the broken network where this page matters most.
//
// THE CLOCK IS THE EVENT LOOP. net_tick() is called from the wizard's existing
// win_get_event(..., 250) timeout, so the state machine advances roughly every
// 250ms with no timer, no thread and no sleep. Each stage carries its own tick
// budget; a remote host that never answers is precisely the case where a
// timeout is the correct semantics rather than a hidden broken wake source.
// ===========================================================================

// Mirror of net_status_t (kernel/proc/syscall.h) and NetStatus
// (kernel/rustkern/netstat.rs). All address fields are HOST byte order:
// (a<<24)|(b<<16)|(c<<8)|d. 0 means NOT CONFIGURED and is rendered as such.
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

const NET_DHCP_IDLE: u32 = 0;
const NET_DHCP_BOUND: u32 = 3;

const NET_PROBE_PING_START: i64 = 0;
const NET_PROBE_PING_POLL: i64 = 1;
const NET_PROBE_PING_CANCEL: i64 = 2;
const NET_PROBE_DHCP_RESTART: i64 = 3;

static mut NS: NetStatus = NetStatus {
    ip: 0, netmask: 0, gateway: 0, dns_active: 0, dns_dhcp: 0, dhcp_ip: 0,
    link_up: 0, dhcp_state: 0, config_static: 0, faulty: 0, driver: 0,
    prefix_len: 0,
};
// FALSE means the syscall itself failed. That is NOT the same as "no address",
// and the page says so rather than drawing zeroes as if they were an answer.
static mut NS_OK: bool = false;

fn net_status_read() -> bool {
    unsafe {
        let rc = syscall1(SYS_NET_STATUS, &raw mut NS as i64);
        NS_OK = rc == 0;
        NS_OK
    }
}
fn net_probe(op: i64, arg: u64) -> i64 {
    unsafe { syscall2(SYS_NET_PROBE, op, arg as i64) }
}

/// Did the address we are showing actually come from a DHCP server?
///
/// THIS IS NOT THE SAME QUESTION AS "do we have an address". Measured on a VM
/// with no DHCP server on its segment: the kernel falls back to a built-in
/// 192.0.2.1/24 static default, so `ip` is non-zero while nothing on the
/// network ever answered. Reporting that as a lease is a fake success, and it
/// is the exact failure this page exists to stop. The distinction is a FIELD
/// COMPARISON rather than a guess only because the accessor is structured.
fn net_from_dhcp(ns: &NetStatus) -> bool {
    ns.ip != 0 && ns.config_static == 0
        && (ns.dhcp_state == NET_DHCP_BOUND || ns.dhcp_ip != 0)
}

// --- the staged test -------------------------------------------------------
//
// FIVE STAGES, chosen so a failure NAMES THE LAYER instead of lighting one
// lamp. A machine with an address that can ping its gateway but cannot resolve
// a name has a specific, diagnosable problem, and this is exactly where it
// should be visible.
//
//   0 LINK     carrier + a NIC at all. "no adapter" and "no cable" are
//              different failures and are reported differently.
//   1 ADDRESS  a usable IPv4 lease. Distinguishes still-negotiating from
//              timed-out from "address but no default route".
//   2 GATEWAY  ICMP echo to the default router. This is what separates "no
//              address yet" from "address but no route".
//   3 DNS      resolve a real name through the resolver actually configured.
//   4 INTERNET fetch over HTTP, i.e. something past the LAN answered.
//
// FAILURE PROPAGATION IS DELIBERATE AND NARROW. Only LINK and ADDRESS failing
// skip what follows, because nothing else can work without them. A GATEWAY
// failure does NOT skip DNS: plenty of routers drop ICMP while routing fine,
// and reporting "DNS untested" there would be a worse lie than testing it. A
// DNS failure DOES skip INTERNET, because that fetch resolves the same name
// and would spend ten seconds failing for the reason already on screen.
const CT_N: usize = 5;
const CT_LINK: usize = 0;
const CT_ADDR: usize = 1;
const CT_GW: usize = 2;
const CT_DNS: usize = 3;
const CT_NET: usize = 4;

const ST_PEND: u8 = 0;
const ST_RUN: u8 = 1;
const ST_PASS: u8 = 2;
const ST_FAIL: u8 = 3;
const ST_SKIP: u8 = 4;

const CT_MSG_CAP: usize = 48;
static mut CT_ST: [u8; CT_N] = [ST_PEND; CT_N];
static mut CT_MSG: [[u8; CT_MSG_CAP]; CT_N] = [[0; CT_MSG_CAP]; CT_N];
static mut CT_CUR: usize = 0;
static mut CT_T0: u32 = 0;         // ms at which the CURRENT stage started
static mut CT_LAST: u32 = 0;       // ms of the last pass (rate limit)
static mut CT_ARMED: bool = false; // the page has taken ownership of the test
static mut CT_STARTED: bool = false; // the current stage has issued its request
static mut CT_JOB: i32 = -1;       // in-flight SYS_HTTP_FETCH job id, -1 = none

/// Monotonic milliseconds. THE CLOCK IS REAL TIME, NOT A COUNT OF EVENT-LOOP
/// ITERATIONS. The first version of this page counted iterations of the
/// win_get_event() timeout, and on the VM that clock simply STOPPED once the
/// compositor had any steady event to deliver: the Internet stage's budget
/// never expired and it displayed an in-progress row for a fetch that had
/// already finished. Iterations are only a clock if they are regular.
///
/// KNOWN CAVEAT, stated rather than discovered later: SYS_UPTIME_MS is derived
/// from timer_ticks, and under vCPU starvation KVM replays lost ticks in a
/// BURST, so this can jump forward. The failure mode of a jump is a stage that
/// gives up EARLY, which is visible and honest, not one that hangs.
fn now_ms() -> u32 { unsafe { syscall0(SYS_UPTIME_MS) as u32 } }

// Per-stage budgets in MILLISECONDS. These are deadlines on a REMOTE party,
// which is the one case where a timeout is the right answer and not a
// workaround for a wake we forgot to arm.
const T_ADDR: u32 = 15000;  // a DORA to complete
const T_GW: u32 = 6000;     // echo requests to the gateway
const T_DNS: u32 = 6000;    // a resolve
const T_NET: u32 = 12000;   // a real HTTP round trip
// Do no more than one pass per this many ms, however fast the loop spins.
const CT_PASS_MS: u32 = 200;

// The host the DNS and INTERNET stages use. It is this project's own update
// server, so the page never depends on a third party's uptime to say
// "connected", and the stage labels NAME it so a failure is attributable.
const CT_HOST: &[u8] = b"updates.maytera.net\0";
const CT_URL: &[u8] = b"http://updates.maytera.net/\0";

fn ct_set(stage: usize, st: u8, msg: &[u8]) {
    unsafe {
        CT_ST[stage] = st;
        let mut i = 0usize;
        while i < msg.len() && msg[i] != 0 && i < CT_MSG_CAP - 1 {
            CT_MSG[stage][i] = msg[i];
            i += 1;
        }
        CT_MSG[stage][i] = 0;
    }
}
// Same, for a message built into a scratch buffer.
fn ct_setb(stage: usize, st: u8, buf: &[u8], n: usize) {
    unsafe {
        CT_ST[stage] = st;
        let mut i = 0usize;
        while i < n && i < CT_MSG_CAP - 1 { CT_MSG[stage][i] = buf[i]; i += 1; }
        CT_MSG[stage][i] = 0;
    }
}

fn put_u32(dst: &mut [u8], at: usize, v: u32) -> usize {
    let mut d = [0u8; 10];
    let mut k = 0usize;
    let mut x = v;
    if x == 0 { d[0] = b'0'; k = 1; }
    while x > 0 && k < 10 { d[k] = b'0' + (x % 10) as u8; x /= 10; k += 1; }
    let mut n = 0usize;
    while k > 0 && at + n < dst.len() { k -= 1; dst[at + n] = d[k]; n += 1; }
    n
}
// Host-order u32 -> "a.b.c.d". ONE place, so no caller can get the order
// backwards (the kernel stores host order; nothing on this page swaps).
fn put_ip(dst: &mut [u8], at: usize, v: u32) -> usize {
    let mut n = 0usize;
    let mut k = 0;
    while k < 4 {
        if k > 0 && at + n < dst.len() { dst[at + n] = b'.'; n += 1; }
        n += put_u32(dst, at + n, (v >> (24 - 8 * k)) & 0xFF);
        k += 1;
    }
    n
}

/// Abandon anything in flight and put every stage back to pending. Called
/// when the page is left, when the DHCP/static toggle flips, and by Retry.
fn net_test_reset() {
    unsafe {
        if CT_JOB >= 0 {
            syscall1(SYS_HTTP_FETCH_CANCEL, CT_JOB as i64);
            CT_JOB = -1;
        }
        net_probe(NET_PROBE_PING_CANCEL, 0);
        let mut i = 0usize;
        while i < CT_N { CT_ST[i] = ST_PEND; CT_MSG[i][0] = 0; i += 1; }
        CT_CUR = 0;
        CT_T0 = now_ms();
        CT_LAST = 0;
        CT_STARTED = false;
        CT_ARMED = false;
    }
}

fn ct_advance() {
    unsafe { CT_CUR += 1; CT_T0 = now_ms(); CT_STARTED = false; }
}
fn ct_skip_rest(from: usize, why: &[u8]) {
    let mut i = from;
    while i < CT_N { ct_set(i, ST_SKIP, why); i += 1; }
    unsafe { CT_CUR = CT_N; }
}

impl App {
    /// Refresh the cached status. Cheap and non-blocking, so the draw path may
    /// call it; it does NOT advance the test.
    fn net_refresh(&mut self) { net_status_read(); }

    /// One step of the page's state machine. Returns true if anything the user
    /// can see changed, so the caller redraws only when there is something to
    /// redraw. Safe to call from any page: it cleans up and returns false.
    fn net_tick(&mut self) -> bool {
        if self.page != PG_NETWORK || !self.dhcp {
            // Leaving the page (or switching to static) must not leave an HTTP
            // job or a ping half-armed in the kernel.
            unsafe { if CT_ARMED { net_test_reset(); return false; } }
            return false;
        }

        // Called on EVERY loop iteration, so it rate-limits itself against the
        // real clock instead of relying on how often the caller happens to run.
        let now = now_ms();
        unsafe {
            if CT_ARMED && now.wrapping_sub(CT_LAST) < CT_PASS_MS { return false; }
            CT_LAST = now;
        }

        let before = unsafe { (NS.ip, NS.link_up, NS.dhcp_state, NS.gateway,
                               NS.dns_active, NS.netmask, NS.faulty) };
        net_status_read();
        let mut dirty = unsafe {
            (NS.ip, NS.link_up, NS.dhcp_state, NS.gateway, NS.dns_active,
             NS.netmask, NS.faulty) != before
        };

        unsafe {
            if !CT_ARMED { CT_ARMED = true; CT_CUR = 0; CT_T0 = now; CT_STARTED = false; }
            if CT_CUR >= CT_N { return dirty; }
        }

        let cur = unsafe { CT_CUR };
        // Elapsed IN THIS STAGE, in ms. wrapping_sub so a 32-bit ms counter
        // rolling over (49 days) cannot produce a gigantic bogus elapsed.
        let ticks = now.wrapping_sub(unsafe { CT_T0 });
        let ns = unsafe { NS };
        // A running stage must be visibly alive: the row shows its elapsed
        // seconds, so "slow" and "stuck" are distinguishable at a glance.
        if unsafe { CT_ST[cur] } == ST_RUN { dirty = true; }

        match cur {
            CT_LINK => {
                if !unsafe { NS_OK } {
                    ct_set(CT_LINK, ST_FAIL, b"the kernel would not report network status");
                    ct_skip_rest(CT_ADDR, b"not tested");
                } else if ns.driver == 0 {
                    ct_set(CT_LINK, ST_FAIL, b"no network adapter found");
                    ct_skip_rest(CT_ADDR, b"needs an adapter");
                } else if ns.link_up == 0 {
                    ct_set(CT_LINK, ST_FAIL, b"no carrier - check the cable");
                    ct_skip_rest(CT_ADDR, b"needs a link");
                } else {
                    ct_set(CT_LINK, ST_PASS, b"carrier detected");
                    ct_advance();
                }
                dirty = true;
            }

            CT_ADDR => {
                if ns.ip != 0 && ns.gateway != 0 {
                    let mut b = [0u8; CT_MSG_CAP];
                    // "leased" is a claim about where the address CAME FROM,
                    // so it is only used when that is true.
                    let mut n = if net_from_dhcp(&ns) { put(&mut b, 0, b"leased ") }
                                else if ns.config_static != 0 { put(&mut b, 0, b"static ") }
                                else { put(&mut b, 0, b"built-in fallback ") };
                    n += put_ip(&mut b, n, ns.ip);
                    n += put(&mut b, n, b"/");
                    n += put_u32(&mut b, n, ns.prefix_len);
                    ct_setb(CT_ADDR, if net_from_dhcp(&ns) || ns.config_static != 0 { ST_PASS }
                                     else { ST_FAIL }, &b, n);
                    ct_advance();
                    dirty = true;
                } else if ns.ip != 0 {
                    // A real and distinct failure: we HAVE an address but there
                    // is no default route, so nothing off-subnet is reachable.
                    ct_set(CT_ADDR, ST_PASS, b"address only - DHCP offered no gateway");
                    ct_set(CT_GW, ST_FAIL, b"no default route configured");
                    unsafe { CT_CUR = CT_DNS; CT_T0 = now; CT_STARTED = false; }
                    dirty = true;
                } else if ns.dhcp_state != NET_DHCP_IDLE && ns.dhcp_state != NET_DHCP_BOUND {
                    if ticks > T_ADDR {
                        ct_set(CT_ADDR, ST_FAIL, b"DHCP timed out - no server replied");
                        ct_skip_rest(CT_DNS, b"needs an address");
                        ct_set(CT_GW, ST_SKIP, b"needs an address");
                        dirty = true;
                    } else if unsafe { CT_ST[CT_ADDR] } != ST_RUN {
                        ct_set(CT_ADDR, ST_RUN, b"asking a DHCP server for an address");
                        dirty = true;
                    }
                } else if ticks > T_ADDR {
                    ct_set(CT_ADDR, ST_FAIL, b"no address - DHCP did not complete");
                    ct_set(CT_GW, ST_SKIP, b"needs an address");
                    ct_skip_rest(CT_DNS, b"needs an address");
                    dirty = true;
                } else if unsafe { CT_ST[CT_ADDR] } != ST_RUN {
                    ct_set(CT_ADDR, ST_RUN, b"waiting for a DHCP address");
                    dirty = true;
                }
            }

            CT_GW => {
                if unsafe { !CT_STARTED } {
                    let rc = net_probe(NET_PROBE_PING_START, ns.gateway as u64);
                    unsafe { CT_STARTED = true; }
                    if rc < 0 {
                        ct_set(CT_GW, ST_FAIL, b"could not send to the gateway");
                        ct_advance();
                        return true;
                    }
                    let mut b = [0u8; CT_MSG_CAP];
                    let mut n = put(&mut b, 0, b"pinging ");
                    n += put_ip(&mut b, n, ns.gateway);
                    ct_setb(CT_GW, ST_RUN, &b, n);
                    return true;
                }
                let rc = net_probe(NET_PROBE_PING_POLL, 0);
                if rc >= 0 {
                    let mut b = [0u8; CT_MSG_CAP];
                    let mut n = put(&mut b, 0, b"replied in ");
                    if rc == 0 {
                        // The kernel's RTT is derived from the 100Hz tick, so a
                        // sub-tick round trip reads as 0. Say what that means
                        // rather than claiming a 0ms round trip.
                        n += put(&mut b, n, b"under 10 ms");
                    } else {
                        n += put_u32(&mut b, n, rc as u32);
                        n += put(&mut b, n, b" ms");
                    }
                    ct_setb(CT_GW, ST_PASS, &b, n);
                    net_probe(NET_PROBE_PING_CANCEL, 0);
                    ct_advance();
                    dirty = true;
                } else if ticks > T_GW {
                    let mut b = [0u8; CT_MSG_CAP];
                    let mut n = put(&mut b, 0, b"no reply from ");
                    n += put_ip(&mut b, n, ns.gateway);
                    ct_setb(CT_GW, ST_FAIL, &b, n);
                    net_probe(NET_PROBE_PING_CANCEL, 0);
                    // NOT a reason to skip DNS: routers that drop ICMP but
                    // route correctly are common, and calling DNS untested
                    // here would be a worse lie than testing it.
                    ct_advance();
                    dirty = true;
                }
            }

            CT_DNS => {
                let mut ip: u32 = 0;
                if unsafe { !CT_STARTED } {
                    unsafe { CT_STARTED = true; }
                    let rc = unsafe {
                        syscall2(SYS_DNS_START, CT_HOST.as_ptr() as i64,
                                 &mut ip as *mut u32 as i64)
                    };
                    if rc < 0 {
                        // rc < 0 from sys_dns_start is the stack refusing to
                        // send at all (net_is_up() false / no route), not a
                        // server saying no. Say which.
                        ct_set(CT_DNS, ST_FAIL, b"no query could be sent (network unusable)");
                        ct_set(CT_NET, ST_SKIP, b"needs DNS");
                        unsafe { CT_CUR = CT_N; }
                        return true;
                    }
                    if rc == 1 && ip != 0 {
                        let mut b = [0u8; CT_MSG_CAP];
                        let mut n = put(&mut b, 0, b"resolved to ");
                        n += put_ip(&mut b, n, ip);
                        ct_setb(CT_DNS, ST_PASS, &b, n);
                        ct_advance();
                        return true;
                    }
                    let mut b = [0u8; CT_MSG_CAP];
                    let mut n = put(&mut b, 0, b"asking ");
                    n += put_ip(&mut b, n, ns.dns_active);
                    ct_setb(CT_DNS, ST_RUN, &b, n);
                    return true;
                }
                let rc = unsafe { syscall1(SYS_DNS_POLL, &mut ip as *mut u32 as i64) };
                if rc == 1 && ip != 0 {
                    let mut b = [0u8; CT_MSG_CAP];
                    let mut n = put(&mut b, 0, b"resolved to ");
                    n += put_ip(&mut b, n, ip);
                    ct_setb(CT_DNS, ST_PASS, &b, n);
                    ct_advance();
                    dirty = true;
                } else if rc < 0 || ticks > T_DNS {
                    let mut b = [0u8; CT_MSG_CAP];
                    let mut n = put(&mut b, 0, b"no answer from ");
                    n += put_ip(&mut b, n, ns.dns_active);
                    ct_setb(CT_DNS, ST_FAIL, &b, n);
                    ct_set(CT_NET, ST_SKIP, b"needs DNS");
                    unsafe { CT_CUR = CT_N; }
                    dirty = true;
                }
            }

            _ => {
                if unsafe { !CT_STARTED } {
                    unsafe { CT_STARTED = true; }
                    let id = unsafe { syscall1(SYS_HTTP_FETCH_START, CT_URL.as_ptr() as i64) };
                    // #549: name the ACTUAL reason. The kernel refuses every
                    // fetch while the adapter is marked faulty, and it does so
                    // before touching the wire, so link/IP/gateway/DNS can all
                    // be green while this stage fails. "could not start the
                    // request" told the user nothing they could act on; the
                    // adapter being flagged is something they CAN act on, and
                    // the kernel now re-probes on its own every 30s.
                    if id == NET_ERR_FAULTY {
                        ct_set(CT_NET, ST_FAIL,
                               b"adapter marked faulty; auto-retry every 30s");
                        unsafe { CT_CUR = CT_N; }
                        return true;
                    }
                    if id < 0 {
                        ct_set(CT_NET, ST_FAIL, b"the kernel would not start the request");
                        unsafe { CT_CUR = CT_N; }
                        return true;
                    }
                    unsafe { CT_JOB = id as i32; }
                    ct_set(CT_NET, ST_RUN, b"contacting updates.maytera.net");
                    return true;
                }
                let mut status: i32 = 0;
                let mut len: u32 = 0;
                let job = unsafe { CT_JOB };
                let rc = unsafe {
                    syscall3(SYS_HTTP_FETCH_POLL, job as i64,
                             &mut status as *mut i32 as i64,
                             &mut len as *mut u32 as i64)
                };
                if rc == 1 {
                    // ANY HTTP status proves a server past the LAN answered,
                    // which is what this stage claims. It does NOT claim the
                    // page exists, so a 404 is still a pass and says so.
                    let mut b = [0u8; CT_MSG_CAP];
                    let mut n = put(&mut b, 0, b"reached the internet (HTTP ");
                    n += put_u32(&mut b, n, status.max(0) as u32);
                    n += put(&mut b, n, b")");
                    ct_setb(CT_NET, ST_PASS, &b, n);
                    unsafe { syscall1(SYS_HTTP_FETCH_CANCEL, job as i64); CT_JOB = -1; CT_CUR = CT_N; }
                    dirty = true;
                } else if rc == 2 || rc < 0 || ticks > T_NET {
                    ct_set(CT_NET, ST_FAIL, b"no reply from updates.maytera.net");
                    unsafe { syscall1(SYS_HTTP_FETCH_CANCEL, job as i64); CT_JOB = -1; CT_CUR = CT_N; }
                    dirty = true;
                }
            }
        }
        dirty
    }

    /// Retry button. With no address it kicks a fresh DHCP exchange (the
    /// NON-BLOCKING one: SYS_NET_DHCP (218) is dhcp_discover_blocking() and
    /// would freeze this window for seconds). With an address it simply re-runs
    /// the test.
    fn net_retry(&mut self) {
        let need_dhcp = unsafe { NS.ip == 0 };
        net_test_reset();
        if need_dhcp { net_probe(NET_PROBE_DHCP_RESTART, 0); }
        net_status_read();
        self.draw();
        win_invalidate(self.win);
    }
}

// Result-row status glyph. dk_status_icon() only speaks pending/running/done,
// and a test needs FAILED and SKIPPED as well - and needs them to differ by
// SHAPE, not only colour (the dark spec's redundant-signal rule).
fn ct_icon(win: i32, x: i32, y: i32, st: u8) {
    let (cx, cy) = (x + 6, y + 6);
    let outer = wel_composite_at(cx, cy);
    match st {
        ST_PASS => dk_status_icon(win, x, y, 2),
        ST_RUN => dk_status_icon(win, x, y, 1),
        ST_FAIL => unsafe {
            gui_fill_circle_aa(win, cx, cy, 6, DK_ERROR, outer);
            dk_line(win, cx - 3, cy - 3, cx + 3, cy + 3, 2, 0x08120Fu32);
            dk_line(win, cx + 3, cy - 3, cx - 3, cy + 3, 2, 0x08120Fu32);
        },
        ST_SKIP => unsafe {
            gui_fill_circle_aa(win, cx, cy, 6, DK_EDGE_GLASS, outer);
            gui_fill_circle_aa(win, cx, cy, 4, outer, DK_EDGE_GLASS);
            dk_line(win, cx - 3, cy, cx + 3, cy, 2, DK_EDGE_GLASS);
        },
        _ => dk_status_icon(win, x, y, 0),
    }
}

// A read-only RESULT, deliberately NOT drawn as an input box: no field fill,
// no field border, no caret. It sits on the results card, so it reads as
// something the machine is telling you rather than something you can type in.
// `v` of 0 renders as "not set" - never a blank, never a fake zero address.
fn dk_result(win: i32, x: i32, y: i32, label: &[u8], v: u32, prefix: u32) {
    dk_solid(win, x, y, label, 10, true, DK_FIELD_LABEL);
    let mut b = [0u8; 32];
    let n = if v == 0 {
        put(&mut b, 0, b"not set")
    } else {
        let mut k = put_ip(&mut b, 0, v);
        if prefix > 0 { k += put(&mut b, k, b"/"); k += put_u32(&mut b, k, prefix); }
        k
    };
    b[n] = 0;
    let col = if v == 0 { DK_INPUT_TEXT_DIS } else { DK_INPUT_TEXT };
    dk_solid(win, x, y + 14, &b, 14, true, col);
}

// Seconds the given stage has been running. Only meaningful for the CURRENT
// stage, which is the only one that can be ST_RUN.
fn ct_elapsed_s(stage: usize) -> u32 {
    unsafe {
        if stage != CT_CUR { return 0; }
        now_ms().wrapping_sub(CT_T0) / 1000
    }
}

// Hit rect of the Retry / Test again button, in ONE place so the draw and the
// click can never disagree about where it is.
const NET_BTN: (i32, i32, i32, i32) = (486, 216, 122, 24);

// ---------------------------------------------------------------------------
// #745: "SET UP LATER" - WHERE IT APPEARS AND WHAT IT MEANS.
//
// It used to sit on the WELCOME page, and its click handler called self.next(),
// the identical thing "Get Started" called. It skipped nothing. A control that
// lies about what it does is worse than a missing one, because the person who
// pressed it believes they opted out and they did not.
//
// It is gone from Welcome, and not because the geometry was awkward. Setup can
// only be deferred once there is an account to defer INTO. Skipping before one
// exists would leave the machine exactly as shipped: autologin=root, on a root
// account whose password is the published default from the asset base. That is
// the precise posture this whole first-boot flow was built to end, so the two
// pages that establish an account (Welcome, Create your account) have no skip
// control at all, and every page after them does.
//
// On the pages where it DOES appear it now means something: it discards the
// remaining optional pages, restores their defaults (see reset_optional_from)
// and runs the same Apply the Continue path runs, including writing
// /CONFIG/SETUPDONE, so the wizard does not come back on the next boot.
// ---------------------------------------------------------------------------
fn page_skippable(page: usize) -> bool {
    // #126: in the reduced flow every page IS optional - the account the full
    // flow refuses to skip into already exists, which is the entire reason
    // that refusal was written. Welcome is still not skippable, for the same
    // reason it is not skippable in the full flow: it has nothing to skip.
    if personalise() {
        return match page {
            PG_WELCOME => false,
            PG_APPEAR | PG_WALL | PG_APPSW => true,
            _ => false,
        };
    }
    match page {
        // Not optional: nothing exists to skip into yet.
        PG_WELCOME | PG_ACCOUNT => false,
        // Every one of these is a preference with a working default.
        PG_SIGNIN | PG_NETWORK | PG_TIME | PG_APPEAR | PG_WALL | PG_APPSW | PG_AI => true,
        // PG_APPLY is a transient progress page and PG_DONE is the end of the
        // flow; neither draws this footer at all, so neither may claim a
        // hit-rect in it.
        _ => false,
    }
}

// #745 task #38 (user-reported 2026-08-12): shortened to "Skip" at the
// user's explicit request. The paragraph this replaces argued for keeping
// "(F10)" IN the label - this wizard runs on a machine whose owner has not
// configured anything yet, including, on the iMac target, possibly not a
// working pointer, and an accelerator that is not written down is a
// feature only its author can use - and that reasoning is still correct.
// F10 STAYS bound (see on_key's KC_F10 handler); only the visible text
// changed, because the user asked for it directly after being told the
// discoverability tradeoff.
const SKIP_LABEL: &[u8] = b"Skip\0";

// Centred in the gap between Back (32..120) and Continue (468..608), on the
// same FOOTER_Y..FOOTER_Y+FOOTER_H row as both. Measured, not guessed, so the
// hit-rect is exactly the drawn text: a link whose clickable area is wider
// than its glyphs is the same class of lie as the button this replaces.
fn skip_link_bounds() -> (i32, i32, i32, i32) {
    let w = unsafe { gui_ttf_width(SKIP_LABEL.as_ptr(), 11) };
    (294 - w / 2, FOOTER_Y, w, FOOTER_H)
}
// ===========================================================================
// #136: bottom-right corner control "Skip to Desktop" (CORNER_LABEL_SKIP /
// CORNER_Y / CORNER_PAD / CORNER_RIGHT / CORNER_FONT / corner_layout() /
// corner_hit() / draw_corner_controls() / skip_to_desktop() / KC_F9)
// REMOVED, owner request verbatim 2026-08-28: "remove the skip to desktop
// option since it no longer makes sense". Full reasoning: the #229
// FIRST-RUN STATE comment above (SYS_FIRSTRUN/FR_MARK_DONE section).
//
// Restart/Shut Down (also formerly in this same footer row) already moved
// out under #198, to the pwr_win power corner below - see that block
// comment for where they live now.
fn do_reboot()   { unsafe { syscall0(SYS_REBOOT); } }
fn do_poweroff() { unsafe { syscall0(SYS_POWEROFF); } }

// ===========================================================================
// #198v2 (2026-08-29): the power corner - Restart/Shut Down as BARE WHITE
// ANTIALIASED GLYPHS floating directly over the desktop wallpaper, NO button
// box of any kind. This is a deliberate rewrite of the #198 box-button
// design: the owner asked for exactly this treatment three times
// ("I want the buttons to simply be white icons, antialiased, no button box,
// just on-top of the existing image background") and the box was never the
// design intent, it was a workaround for a missing capability - see the
// history below and docs/WIZARD_POWER_CORNER.html revision 2 for the full
// spec this implements numerically.
//
// THE MISSING CAPABILITY, AND WHAT WAS BUILT TO CLOSE IT. Drawing a real
// antialiased white glyph over an ARBITRARY photograph requires blending
// against the pixels that are ACTUALLY there, not a guessed color - and
// until now no Ring-3 window could do that: SYS_WIN_DRAW_IMAGE/DRAW_RECT
// write into a private content_buffer with no read-back, so the old
// pwr_draw_mico() (see git history) could only fake antialiasing by
// blending toward a caller-supplied "best guess" background, which is
// exactly why the box existed - filling a KNOWN color (the button face) was
// the only background this engine could honestly blend against. The new
// primitive is WINDOW_FLAG_ALPHA_CONTENT / SYS_WIN_SET_ALPHA_CONTENT
// (kernel/gui/window.h, kernel/proc/syscall.c): a window opting in gets its
// content_buffer's top byte read as REAL per-pixel alpha and blended by the
// compositor against the LIVE framebuffer at blit time (the exact same
// fb_get_pixel()/fb_put_pixel() loop that already existed for uniform
// per-window opacity, generalised to per-pixel). THIS IS REUSABLE: any
// future app that wants to paint an antialiased overlay straight onto the
// desktop (a cursor decoration, a notification glow, another icon-on-photo
// control) should use SYS_WIN_SET_ALPHA_CONTENT instead of re-deriving this
// same workaround a third time.
//
// One function draws AND hit-tests (pwr_layout(), same idiom as
// footer_bounds()/skip_link_bounds() above) - the #188 "drawn at one rect,
// hit-tested at a different one" bug class is structurally impossible if
// there is only one source of the rectangles. The hit-test geometry below
// is UNCHANGED from the box design (same 40x40 target per icon, same
// screen-corner position) - only the PAINT changed, not the click contract.
const PWR_BTN: i32 = 40;      // hit-target box, square (unchanged from v1)
const PWR_ICON: i32 = 26;     // glyph draw size, logical px. Bumped from the
// v1 box design's 22: a naked glyph with no surrounding chrome reads smaller
// than the same glyph inside a filled box of the same size (the box itself
// used to carry visual weight), so the spec (WIZARD_POWER_CORNER.html
// revision 2, section 3) calls for a modest +4px to keep the same
// perceived size. Still well inside the unchanged 40px hit cell.
const PWR_GAP: i32 = 8;       // gap between the two buttons
// Content box is 2*PWR_BTN + PWR_GAP = 88 wide, PWR_BTN = 40 tall. The
// window itself is padded +8px each axis beyond that (96x48) - the durable
// #trap lesson that SYS_WIN_CREATE/win_create_bg take the OUTER size, so
// asking for exactly the content size clips the bottom/right row. Unlike a
// chromed window, a NOCHROME window (which this is, via win_set_nochrome_bg()
// right after creation) has content == outer once created, so
// PWR_CONTENT_W/H below double as both the win_create_bg() request size AND
// the drawn content size - there is no separate "ask the window what it
// actually got" step, matching the existing win_create()/CARD_W/CARD_H
// precedent in main() below. This window's ENTIRE content area is now
// transparent (alpha 0) except the glyph+halo pixels drawn into it - see
// pwr_draw() below.
const PWR_CONTENT_W: i32 = 2 * PWR_BTN + PWR_GAP + 8;   // 96
const PWR_CONTENT_H: i32 = PWR_BTN + 8;                 // 48
// Right/bottom margin from the outer window edge to the REAL screen edge.
// 20 reuses CORNER_RIGHT's own margin value (not the CORNER_RIGHT constant
// itself, which is relative to W, the wizard's OWN window width - this
// window's position is relative to the real screen, sw/sh, a different
// space entirely). 24 reuses lockscreen.c's own bottom-right power-control
// margin (g.pw_y = sh - g.pw_h - 24) - the sibling feature this most
// resembles. See spec section 3.
const PWR_MARGIN_R: i32 = 20;
const PWR_MARGIN_B: i32 = 24;

// #198v2: the halo/shadow dilation radius, logical px, and its two opacity
// levels (WIZARD_POWER_CORNER.html revision 2, section 7 - "legibility
// treatment"). A soft, subtly-dark halo behind the white glyph, NOT a box:
// its shape is a max-dilation of the glyph's OWN coverage mask (so it
// silhouettes the glyph, it is never a rectangle or a circle), and its peak
// alpha is well short of opaque even at HOVER, so it can never read as a
// filled shape. This is the treatment the task explicitly allows ("a subtle
// drop shadow, a soft dark halo, or a slight outline") and it is what keeps
// a pure-white glyph legible over a bright sky (verified: see CHANGELOG
// screenshots over WPT_LIGHT and WPT_DARK backgrounds).
const PWR_HALO_R: i32 = 2;         // dilation radius, logical px
const PWR_HALO_MAX: u32 = 150;     // baseline peak halo alpha (~59%), out of 255
const PWR_HALO_MAX_HOVER: u32 = 195; // hover peak halo alpha (~76%) - the ONLY
// hover feedback: the halo deepens slightly, nothing box-shaped appears.
const PWR_GLYPH_PRESS_PCT: u32 = 72; // pressed: glyph alpha scaled to 72% -
// a visible "give" on click without any fill color change.
// Composite buffer side length: the glyph itself plus PWR_HALO_R of halo
// bleed on every edge.
const PWR_BUF: i32 = PWR_ICON + 2 * PWR_HALO_R;   // 30

// idx 0 = Restart (left), idx 1 = Shut Down (right, closest to the screen
// corner) - matches the existing text order this replaces (CORNER_LABEL_
// RESTART before CORNER_LABEL_SHUTDOWN) and lockscreen.c/login.c's own
// convention. Content-local coordinates: pwr_win has no header/footer/card
// origin of its own (unlike the main wizard window), so these ARE the
// window-local coordinates events arrive in - no translation step needed.
fn pwr_layout() -> [(i32, i32, i32, i32); 2] {
    let pad_x = (PWR_CONTENT_W - (2 * PWR_BTN + PWR_GAP)) / 2;
    let pad_y = (PWR_CONTENT_H - PWR_BTN) / 2;
    let x0 = pad_x;
    let x1 = pad_x + PWR_BTN + PWR_GAP;
    [(x0, pad_y, PWR_BTN, PWR_BTN), (x1, pad_y, PWR_BTN, PWR_BTN)]
}

// ---- MICO .ICN loader, Rust-native re-implementation -----------------------
// Same format as userland/apps/files/main.c's mico_get()/draw_mico() (12-byte
// header 'MICO' + width/height u32 LE + w*h*4 BGRA), which this app cannot
// call directly: those are private C statics, not FFI-exported (spec section
// 8). This mirrors an existing gap (six separate C copies already exist
// there); promoting it to a shared libc helper is out of scope here, per the
// spec's own recommendation.
const MICO_DIM: usize = 64;
#[derive(Clone, Copy)]
struct MicoIcon { w: i32, h: i32, loaded: i32, px: [u8; MICO_DIM * MICO_DIM * 4] }
const MICO_ZERO: MicoIcon = MicoIcon { w: 0, h: 0, loaded: 0, px: [0u8; MICO_DIM * MICO_DIM * 4] };
// index 0 = RESTART.ICN, 1 = POWER.ICN - same order as pwr_layout().
static mut PWR_ICONS: [MicoIcon; 2] = [MICO_ZERO; 2];
// Coverage scratch (one glyph's worth, no halo margin) and the composited
// BGRA output scratch (glyph + halo margin) - both reused for both
// icons/draws (one at a time).
static mut PWR_COV: [u8; (PWR_ICON * PWR_ICON) as usize] = [0u8; (PWR_ICON * PWR_ICON) as usize];
static mut PWR_ICON_SCRATCH: [u32; (PWR_BUF * PWR_BUF) as usize] = [0; (PWR_BUF * PWR_BUF) as usize];

fn pwr_load_icon(idx: usize, path: &[u8]) {
    let ic = unsafe { &mut PWR_ICONS[idx] };
    if ic.loaded != 0 { return; }   // already tried (loaded or missing)
    ic.loaded = -1;
    let fd = unsafe { syscall3(SYS_OPEN, path.as_ptr() as i64, 0, 0) } as i32;
    if fd < 0 { return; }
    let mut hdr = [0u8; 12];
    let n = unsafe { syscall3(SYS_READ, fd as i64, hdr.as_mut_ptr() as i64, 12) };
    if n != 12 || &hdr[0..4] != b"MICO" {
        unsafe { syscall1(SYS_CLOSE, fd as i64); }
        return;
    }
    let w = (hdr[4] as i32) | ((hdr[5] as i32) << 8) | ((hdr[6] as i32) << 16) | ((hdr[7] as i32) << 24);
    let h = (hdr[8] as i32) | ((hdr[9] as i32) << 8) | ((hdr[10] as i32) << 16) | ((hdr[11] as i32) << 24);
    if w <= 0 || h <= 0 || w as usize > MICO_DIM || h as usize > MICO_DIM {
        unsafe { syscall1(SYS_CLOSE, fd as i64); }
        return;
    }
    let want = (w * h * 4) as usize;
    let mut got = 0usize;
    while got < want {
        let n = unsafe { syscall3(SYS_READ, fd as i64, ic.px.as_mut_ptr().add(got) as i64, (want - got) as i64) };
        if n <= 0 { break; }
        got += n as usize;
    }
    unsafe { syscall1(SYS_CLOSE, fd as i64); }
    if got != want { return; }
    ic.w = w; ic.h = h; ic.loaded = 1;
}

// Sample icon `idx`, nearest-neighbour scaled to PWR_ICON x PWR_ICON, into
// PWR_COV as a pure 0..255 COVERAGE mask (no color, no compositing against
// any guessed background - that whole class of approximation is gone now
// that the destination is real). cov = luma-derived white-glyph coverage,
// combined with the source icon's own alpha byte, same formula the v1
// pwr_draw_mico() used for its coverage term.
fn pwr_sample_coverage(idx: usize) {
    let ic = unsafe { &PWR_ICONS[idx] };
    unsafe { PWR_COV = [0u8; (PWR_ICON * PWR_ICON) as usize]; }
    if ic.loaded != 1 { return; }
    let mut dy = 0i32;
    while dy < PWR_ICON {
        let mut sy = (dy * ic.h) / PWR_ICON; if sy >= ic.h { sy = ic.h - 1; }
        let mut dx = 0i32;
        while dx < PWR_ICON {
            let mut sx = (dx * ic.w) / PWR_ICON; if sx >= ic.w { sx = ic.w - 1; }
            let o = ((sy * ic.w + sx) * 4) as usize;
            let b = ic.px[o] as u32; let g = ic.px[o + 1] as u32; let r = ic.px[o + 2] as u32; let a = ic.px[o + 3] as u32;
            let cov = (r * 30 + g * 59 + b * 11) / 100;   // white glyph -> coverage
            let cov = (a * cov) / 255;
            unsafe { PWR_COV[(dy * PWR_ICON + dx) as usize] = cov as u8; }
            dx += 1;
        }
        dy += 1;
    }
}

// Composite the sampled glyph coverage (PWR_COV) plus its dilated halo into
// PWR_ICON_SCRATCH as REAL BGRA-with-alpha pixels (top byte = alpha), then
// blit the whole PWR_BUF x PWR_BUF buffer in one SYS_WIN_DRAW_IMAGE call onto
// the window this app owns (which must already have called
// win_set_alpha_content() - see pwr_create()). `halo_max` and
// `glyph_pct` are the only two state-dependent knobs (hover deepens the
// halo, press dims the glyph) - see PWR_HALO_MAX/PWR_HALO_MAX_HOVER/
// PWR_GLYPH_PRESS_PCT above. No box, no fill, no theme color is read here:
// the glyph is always pure white, the halo always pure black, per the
// spec's explicit instruction (WIZARD_POWER_CORNER.html revision 2).
fn pwr_composite(win: i32, idx: usize, cell_x: i32, cell_y: i32, halo_max: u32, glyph_pct: u32) {
    pwr_sample_coverage(idx);
    let r = PWR_HALO_R;
    let buf = PWR_BUF;
    let mut by = 0i32;
    while by < buf {
        let mut bx = 0i32;
        while bx < buf {
            // Dilate: max glyph coverage within [-r,+r] of (bx,by), where
            // (bx,by) is expressed in the glyph's own (un-padded) coordinate
            // space, i.e. glyph coordinate = (bx - r, by - r).
            let gx0 = bx - r; let gy0 = by - r;
            let mut halo_cov: u32 = 0;
            let mut ky = -r;
            while ky <= r {
                let sy = gy0 + ky;
                if sy >= 0 && sy < PWR_ICON {
                    let mut kx = -r;
                    while kx <= r {
                        let sx = gx0 + kx;
                        if sx >= 0 && sx < PWR_ICON {
                            let c = unsafe { PWR_COV[(sy * PWR_ICON + sx) as usize] as u32 };
                            if c > halo_cov { halo_cov = c; }
                        }
                        kx += 1;
                    }
                }
                ky += 1;
            }
            // Glyph coverage AT this exact pixel (no dilation), 0 outside
            // the glyph's own (un-padded) region.
            let glyph_cov: u32 = if gx0 >= 0 && gx0 < PWR_ICON && gy0 >= 0 && gy0 < PWR_ICON {
                unsafe { PWR_COV[(gy0 * PWR_ICON + gx0) as usize] as u32 }
            } else { 0 };
            let glyph_a = (glyph_cov * glyph_pct) / 100;
            let halo_a = (halo_cov * halo_max) / 255;
            // Standard source-over: halo (black) first, glyph (white) on top.
            let out_a = glyph_a + (halo_a * (255 - glyph_a)) / 255;
            let px = if out_a == 0 {
                0u32
            } else {
                // white*glyph_a + black*halo_a*(255-glyph_a)/255, and black
                // contributes 0 to every channel, so each channel reduces to
                // 255*glyph_a/out_a.
                let ch = ((255u32 * glyph_a) / out_a).min(255);
                (out_a << 24) | (ch << 16) | (ch << 8) | ch
            };
            unsafe { PWR_ICON_SCRATCH[(by * buf + bx) as usize] = px; }
            bx += 1;
        }
        by += 1;
    }
    draw_image_win(win, cell_x, cell_y, buf, buf, core::ptr::addr_of!(PWR_ICON_SCRATCH) as i64);
}

// pwr_win handle (-1 = does not currently exist), plus press/hover state for
// the two buttons (-1 = neither). Following this file's own established
// idiom of screen/window-derived global state as `static mut` (FB_W/FB_H/
// CARD_MODE/CARD_X/CARD_Y below), rather than threading a field through
// App for state that belongs to a SECOND window, not the wizard's own page
// model.
static mut PWR_WIN: i32 = -1;
static mut PWR_HOVER: i32 = -1;
static mut PWR_PRESS: i32 = -1;

// Paints the whole content area TRANSPARENT (alpha 0 - see win_set_alpha_
// content() in pwr_create()), then composites each icon's glyph+halo on top
// via pwr_composite(). No panel fill of any kind: the window has no visible
// footprint of its own, only the two glyphs do. See the section header
// comment above for why this replaced the v1 box design and what primitive
// made it possible.
fn pwr_draw() {
    let win = unsafe { PWR_WIN };
    if win < 0 { return; }
    // Clear the ENTIRE content area to fully transparent. This is the one
    // rect fill this window ever does, and unlike every other rect fill in
    // this file it deliberately writes alpha 0 in every pixel's top byte -
    // SYS_WIN_DRAW_RECT passes the 32-bit color through to content_buffer
    // verbatim (kernel/proc/syscall.c sys_win_draw_rect: `content_buffer[..] =
    // color`), so 0x00000000 really does mean "fully transparent", not
    // "opaque black", once win_set_alpha_content() is in effect.
    rect_win_local(win, 0, 0, PWR_CONTENT_W, PWR_CONTENT_H, 0x0000_0000);
    let l = pwr_layout();
    let hover = unsafe { PWR_HOVER };
    let press = unsafe { PWR_PRESS };
    let mut i = 0usize;
    while i < 2 {
        let (bx, by, bw, bh) = l[i];
        let halo_max = if hover == i as i32 || press == i as i32 { PWR_HALO_MAX_HOVER } else { PWR_HALO_MAX };
        let glyph_pct = if press == i as i32 { PWR_GLYPH_PRESS_PCT } else { 100 };
        let cx = bx + (bw - PWR_BUF) / 2;
        let cy = by + (bh - PWR_BUF) / 2;
        pwr_composite(win, i, cx, cy, halo_max, glyph_pct);
        i += 1;
    }
    win_invalidate(win);
}

// Creates pwr_win (idempotent) at the REAL screen's bottom-right, sized/
// positioned per docs/WIZARD_POWER_CORNER.html section 3. Small-screen
// fallback: gated on CARD_MODE (!CARD_MODE means there is no room) - F2/F3
// keyboard restart/shutdown still work regardless (#334: keyboard is the
// fallback that must always work).
fn pwr_create() {
    if !unsafe { CARD_MODE } { return; }
    if unsafe { PWR_WIN } >= 0 { return; }
    let sw = unsafe { FB_W }; let sh = unsafe { FB_H };
    let x = sw - PWR_MARGIN_R - PWR_CONTENT_W;
    let y = sh - PWR_MARGIN_B - PWR_CONTENT_H;
    // Empty title, not "PowerCorner": found empirically on the throwaway
    // verification VM (#198) that a titled win_create_bg() window still gets
    // its own taskbar tile - taskbar.c's tb_window_is_app() only skips a
    // window "naturally" (its own comment's word) when title[0] == '\0',
    // the same way the root/desktop window is skipped. taskbar.c also has a
    // title-KEYED companion-window suppression list (tb_is_companion(), #341)
    // for cases that need a title, but this window never needs one, so the
    // simpler no-title path avoids touching a second file for the same fix.
    let h = win_create_bg(b"\0", x, y, PWR_CONTENT_W, PWR_CONTENT_H);
    if h < 0 { return; }
    // #wizfocus (2026-08-28): was win_set_nochrome(h), which is
    // SYS_WIN_SET_NOCHROME (focus=1, kernel/proc/syscall.c
    // sys_win_set_nochrome_impl) - it unconditionally called
    // wm_focus_window() on the power corner EVERY time pwr_create() ran
    // (wizard startup, and again after every PG_APPLY/skip transient via
    // pwr_destroy()+pwr_create()), stealing keyboard focus from the wizard's
    // own window onto this borderless corner panel with no visible focus
    // ring. The wizard then never saw another keystroke: Tab/Enter/arrow
    // keys all went to pwr_win, which has no on_key handling for them.
    // win_create_bg() above already correctly asked for focus=0 at CREATE
    // time (per its own comment) but that intent was undone one line later
    // by this call. win_set_nochrome_bg() is the exact #216 counterpart
    // (SYS_WIN_SET_NOCHROME_BG, focus=0) already used by other background
    // panels for this reason; it never grabs focus, so the wizard keeps it
    // for keyboard-only navigation with no mouse (#307/#433 machine class).
    win_set_nochrome_bg(h);
    // #198v2: win_set_nochrome_impl() ALSO now sets WINDOW_FLAG_NO_FOCUS
    // whenever focus=0 (kernel/gui/window.h, kernel/proc/syscall.c), so this
    // call is what makes the corner permanently unfocusable, not just
    // unfocused-at-create: window_set_focus() (the kernel's single focus
    // chokepoint) refuses the window outright from then on, so a stray
    // click can no longer make it wm_state.focused_window and no later F11
    // can touch it. See the WINDOW_FLAG_NO_FOCUS comment for the exact bug
    // this closes (the owner's reported "F11 maximises the power corner").
    //
    // #198v2: opt this window into REAL per-pixel content alpha - see the
    // section header comment above pwr_layout() for the full rationale.
    // Must happen AFTER win_set_nochrome_bg(), which reallocates and
    // refills content_buffer (0xFFF5F5F5, opaque) as a side effect; setting
    // the flag first would have no buffer to apply to yet, and setting it
    // before that refill would just have it overwritten anyway.
    win_set_alpha_content(h);
    unsafe { PWR_WIN = h; PWR_HOVER = -1; PWR_PRESS = -1; }
    pwr_load_icon(0, b"/ICONS/RESTART.ICN\0");
    pwr_load_icon(1, b"/ICONS/POWER.ICN\0");
    pwr_draw();
}

// Destroys pwr_win if it exists. Called before entering PG_APPLY and at the
// wizard's own exit point - destroying (not hiding: no win_hide()/opacity
// primitive exists, checked) is the only way to make the #188 "drawn
// control with a live hit-test" bug class structurally impossible: if there
// is no window, there is no click target to disagree with a stale draw.
fn pwr_destroy() {
    let h = unsafe { PWR_WIN };
    if h >= 0 { win_destroy(h); }
    unsafe { PWR_WIN = -1; PWR_HOVER = -1; PWR_PRESS = -1; }
}

// Non-blocking poll of pwr_win's own event queue (ms=0 - see main()'s event
// loop for why this piggybacks on the existing 250ms win_get_event(win, ...)
// cadence rather than adding a new loop: #426/#419/#420 class, no new
// busy/spin loop). Hover updates on mouse move, press+bootlog+action fire
// together on mouse down (matching how every OTHER control in this app
// already fires its action from EV_MOUSE_DOWN - see main()'s event loop),
// mouse up just clears the press highlight.
fn pwr_pump() {
    let win = unsafe { PWR_WIN };
    if win < 0 { return; }
    let mut ev = GuiEvent { ty: 0, target_id: 0, mouse_x: 0, mouse_y: 0,
                            mouse_buttons: 0, scroll_delta: 0, keycode: 0, key_char: 0 };
    let got = win_get_event(win, &mut ev, 0);
    if got == 0 { return; }
    match ev.ty {
        EV_MOUSE_MOVE => {
            let l = pwr_layout();
            let mut nh: i32 = -1;
            let mut i = 0usize;
            while i < 2 {
                let (bx, by, bw, bh) = l[i];
                if ev.mouse_x >= bx && ev.mouse_x < bx + bw && ev.mouse_y >= by && ev.mouse_y < by + bh { nh = i as i32; break; }
                i += 1;
            }
            if nh != unsafe { PWR_HOVER } { unsafe { PWR_HOVER = nh; } pwr_draw(); }
        }
        EV_MOUSE_DOWN => {
            if ev.mouse_buttons & MOUSE_LEFT != 0 {
                let l = pwr_layout();
                let mut hit: i32 = -1;
                let mut i = 0usize;
                while i < 2 {
                    let (bx, by, bw, bh) = l[i];
                    if ev.mouse_x >= bx && ev.mouse_x < bx + bw && ev.mouse_y >= by && ev.mouse_y < by + bh { hit = i as i32; break; }
                    i += 1;
                }
                if hit >= 0 {
                    unsafe { PWR_PRESS = hit; }
                    pwr_draw();
                    if hit == 0 {
                        bootlog(b"[SETUP] power: Restart from wizard corner\0");
                        do_reboot();
                    } else {
                        bootlog(b"[SETUP] power: Shut Down from wizard corner\0");
                        do_poweroff();
                    }
                }
            }
        }
        EV_MOUSE_UP => {
            if unsafe { PWR_PRESS } != -1 { unsafe { PWR_PRESS = -1; } pwr_draw(); }
        }
        EV_REDRAW => pwr_draw(),
        _ => {}
    }
}

// ===========================================================================
// #745 task #15: PG_APPSW, "Apps and widgets" (docs/OOBE_APPS_WIDGETS.html).
//
// THE HONEST FINDING THAT SHAPES THIS PAGE (spec section 2): there is nothing
// to install and no offline catalogue to install it from - the image already
// ships 159 /APPS entries and the App Store catalogue is remote-only with no
// on-disk cache. This page therefore does exactly two things: pin already-
// installed apps to the dock (Start menu Favourites, MAX_FAVORITES=12), and
// toggle the 15 desktop widgets. No install flow of any kind.
//
// BOTH LIVE-APPLY CHANNELS THIS PAGE WRITES THROUGH ALREADY EXIST AND ARE
// PROVEN (blame.md, 2026-08-11, #745 P1/P2): startmenu_favs_poll() (P1) and
// widgets_cfg_poll() (P2) in the compositor. This page therefore does NOT
// write STARTMENU.CFG or UIPROFIL.YML directly - both are compositor-owned
// files, and a direct write races the compositor's own rewrite exactly the
// way it already ate a dock-style write once (blame.md, "Two files that each
// believe they own the same setting"). It writes only the two per-user
// channel files the compositor already polls, matching the ACTUAL merged
// implementation rather than this doc's own now-superseded section 9.2 (see
// the port report): FAVCH.CFG (one exec path per line, no "FAV|" prefix -
// see startmenu.c sm_load_favs_channel()) and WIDGETCH.CFG (one
// "<tray bind>=<0|1>" line per widget - see main.c widgets_cfg_poll()).
// ===========================================================================

// The twelve dock candidates (spec section 5.1). Every exec path below was
// grepped out of the real build/assets/startmenu/system.d/*.MENU fragments,
// not copied from the spec doc unverified, because startmenu_get_favorites()
// silently drops any favourite whose path does not string-match a rendered
// menu item - a typo here would compile, run, and pin nothing.
#[derive(Clone, Copy)]
struct AppUi { label: &'static [u8], path: &'static [u8] }
const APPS_UI: [AppUi; 12] = [
    AppUi { label: b"Browser\0",      path: b"/APPS/BROWSER\0" },
    AppUi { label: b"Files\0",        path: b"/APPS/FILES\0" },
    AppUi { label: b"Terminal\0",     path: b"/APPS/TERMINAL\0" },
    AppUi { label: b"Editor\0",       path: b"/APPS/EDITOR\0" },
    AppUi { label: b"Settings\0",     path: b"/APPS/SETTINGS\0" },
    AppUi { label: b"App Repo\0",     path: b"/APPS/APPSTORE\0" },
    AppUi { label: b"Notes\0",        path: b"/APPS/NOTES\0" },
    AppUi { label: b"Calculator\0",   path: b"/APPS/CALC\0" },
    AppUi { label: b"Media Player\0", path: b"/APPS/MEDIAPLAYER\0" },
    AppUi { label: b"Gallery\0",      path: b"/APPS/GALLERY\0" },
    AppUi { label: b"Paint\0",        path: b"/APPS/PAINT\0" },
    // Not uppercase - copied verbatim from 04-system.MENU, where the
    // fragment itself names it this way.
    AppUi { label: b"Task Manager\0", path: b"/APPS/taskmgr\0" },
];
// Owner decision (2026-08-18): all twelve dock candidates default checked.
// Previously only Browser, Files, Terminal, Settings, App Store (indices
// 0,1,2,4,5, the compiled-in desktop icon set minus Recycle Bin) were on;
// the person now sees every candidate pre-pinned and can uncheck what they
// do not want, rather than opt in to the other seven.
const APPS_DEFAULT_MASK: u16 =
    (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) |
    (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11);

// The fifteen widgets (spec section 5.2/9.1), column-major display order
// (col = i/5, row = i%5): column 3 is entirely "needs something first", which
// is why the ring reads as a group. Three names per entry because two of the
// fifteen have a DIFFERENT tray-bind spelling than their UIPROFIL.YML profile
// key (Sheep: sheep_show/sheep; Sheepdog: dog_show/dog) - grepped out of
// compositor/widgets.c's g_widget_registry[] and traymenu.c's tm_get/tm_set,
// not guessed. `ring` marks a widget whose card can ship empty until
// something else is configured. `detail_off`, where non-empty, replaces
// `detail_on` when net_status_read() says this computer has no usable route.
#[derive(Clone, Copy)]
struct WidgetUi {
    label: &'static [u8], bind: &'static [u8], profkey: &'static [u8], ring: bool,
    detail_on: &'static [u8], detail_off: &'static [u8],
}
const WIDGETS_UI: [WidgetUi; 15] = [
    WidgetUi { label: b"Digital Clock\0", bind: b"show_digclock\0", profkey: b"show_digclock\0",
               ring: false, detail_on: b"A clock in the top right corner. Nothing to set up.\0", detail_off: b"\0" },
    WidgetUi { label: b"Clock\0", bind: b"show_clock\0", profkey: b"show_clock\0",
               ring: false, detail_on: b"An analog clock you can drag anywhere on the desktop.\0", detail_off: b"\0" },
    WidgetUi { label: b"Calendar\0", bind: b"show_calendar\0", profkey: b"show_calendar\0",
               ring: false, detail_on: b"This month, on the desktop. Nothing to set up.\0", detail_off: b"\0" },
    WidgetUi { label: b"World Time\0", bind: b"show_worldtime\0", profkey: b"show_worldtime\0",
               ring: false, detail_on: b"Three time zones side by side. Pick them from the widget's own menu.\0", detail_off: b"\0" },
    WidgetUi { label: b"Timer\0", bind: b"show_timer\0", profkey: b"show_timer\0",
               ring: false, detail_on: b"A countdown timer that sits on the desktop.\0", detail_off: b"\0" },
    WidgetUi { label: b"System Monitor\0", bind: b"show_sysmon\0", profkey: b"show_sysmon\0",
               ring: false, detail_on: b"Live CPU, memory and network use. Nothing to set up.\0", detail_off: b"\0" },
    WidgetUi { label: b"Uptime\0", bind: b"show_uptime\0", profkey: b"show_uptime\0",
               ring: false, detail_on: b"How long this computer has been running.\0", detail_off: b"\0" },
    WidgetUi { label: b"Sticky Notes\0", bind: b"show_stickies\0", profkey: b"show_stickies\0",
               ring: false, detail_on: b"Notes pinned to the desktop. Saved on this computer.\0", detail_off: b"\0" },
    WidgetUi { label: b"Sheep\0", bind: b"sheep_show\0", profkey: b"sheep\0",
               ring: false, detail_on: b"A sheep wanders around the desktop. Nothing to set up.\0", detail_off: b"\0" },
    WidgetUi { label: b"Sheepdog\0", bind: b"dog_show\0", profkey: b"dog\0",
               ring: true, detail_on: b"A dog that herds the sheep. Turn Sheep on too, or it has nothing to herd.\0", detail_off: b"\0" },
    WidgetUi { label: b"Weather\0", bind: b"show_weather\0", profkey: b"show_weather\0", ring: true,
               detail_on: b"Needs a working connection. Set your city in the widget's own menu.\0",
               detail_off: b"Needs a network connection. This computer has none yet, so the card stays empty.\0" },
    WidgetUi { label: b"Crypto\0", bind: b"show_crypto\0", profkey: b"show_crypto\0", ring: true,
               detail_on: b"Needs a working connection. Tracks BTC and ETH until you change it.\0",
               detail_off: b"Needs a network connection. This computer has none yet, so the card stays empty.\0" },
    WidgetUi { label: b"Stocks\0", bind: b"show_stocks\0", profkey: b"show_stocks\0", ring: true,
               detail_on: b"Needs a working connection. Tracks AAPL and MSFT until you change it.\0",
               detail_off: b"Needs a network connection. This computer has none yet, so the card stays empty.\0" },
    WidgetUi { label: b"Home Assistant\0", bind: b"show_ha\0", profkey: b"show_ha\0",
               ring: true, detail_on: b"Needs a server address and token. Until you add them the card stays empty.\0", detail_off: b"\0" },
    WidgetUi { label: b"Maytera AI\0", bind: b"show_aichat\0", profkey: b"show_aichat\0",
               ring: true, detail_on: b"Opens the Maytera AI panel. Needs an API key, which the next step sets up.\0", detail_off: b"\0" },
];
// Owner decision (2026-08-18): six widgets default checked - Digital Clock,
// Calendar, Uptime, Weather, Home Assistant and Maytera AI. Two of these
// (Home Assistant, Maytera AI - indices 13, 14) ship ON and EMPTY
// (g_show_ha=1 with no server configured, g_aichat_enabled=1 with no API
// key) - the strongest reason this page exists (spec 2.3); Weather (index
// 10) ships ON and empty too until a network exists (WIDGETS_UI[10]'s own
// detail_off text). Named indices, not a bare hex literal, so a reorder of
// WIDGETS_UI is at least visible in a diff here; the const asserts
// immediately below make a SILENT reorder a build failure instead of a
// mis-selection (the exact trap: initialising a mask by position and
// trusting WIDGETS_UI never moves).
const WIDX_DIGCLOCK: usize = 0;
const WIDX_CALENDAR: usize = 2;
const WIDX_UPTIME: usize = 6;
const WIDX_WEATHER: usize = 10;
const WIDX_HA: usize = 13;
const WIDX_AICHAT: usize = 14;
const WIDGETS_DEFAULT_MASK: u16 =
    (1 << WIDX_DIGCLOCK) | (1 << WIDX_CALENDAR) | (1 << WIDX_UPTIME) |
    (1 << WIDX_WEATHER) | (1 << WIDX_HA) | (1 << WIDX_AICHAT);

// Compile-time proof that the indices above still name what this comment
// claims. bytes_eq is a const fn so these run at build time, not on the
// draw path. Add/adjust one of these whenever WIDX_* changes; a stale
// WIDX_* pointing at the wrong profkey fails the build instead of quietly
// defaulting the wrong widget on.
const fn bytes_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() { return false; }
    let mut i = 0;
    while i < a.len() {
        if a[i] != b[i] { return false; }
        i += 1;
    }
    true
}
const _: () = assert!(bytes_eq(WIDGETS_UI[WIDX_DIGCLOCK].profkey, b"show_digclock\0"));
const _: () = assert!(bytes_eq(WIDGETS_UI[WIDX_CALENDAR].profkey, b"show_calendar\0"));
const _: () = assert!(bytes_eq(WIDGETS_UI[WIDX_UPTIME].profkey, b"show_uptime\0"));
const _: () = assert!(bytes_eq(WIDGETS_UI[WIDX_WEATHER].profkey, b"show_weather\0"));
const _: () = assert!(bytes_eq(WIDGETS_UI[WIDX_HA].profkey, b"show_ha\0"));
const _: () = assert!(bytes_eq(WIDGETS_UI[WIDX_AICHAT].profkey, b"show_aichat\0"));

const APP_X0: i32 = 32;
const APP_Y0: i32 = 96;
const APP_PITCH_X: i32 = 98;
const APP_PITCH_Y: i32 = 60;
const APP_W: i32 = 86;
const APP_H: i32 = 48;

const WID_X0: i32 = 32;
const WID_Y0: i32 = 260;
const WID_PITCH_X: i32 = 196;
const WID_PITCH_Y: i32 = 24;
const WID_W: i32 = 184;
const WID_H: i32 = 20;

// Spec 5.3: "has an address and a route", NOT "the internet is reachable".
// The gateway term is load-bearing: the kernel falls back to a built-in
// 192.0.2.1/24 when nothing answers DHCP, so a non-zero ip alone is not
// evidence of a network (main.rs net_from_dhcp() makes the identical point
// for the Network page).
fn appsw_net_present() -> bool {
    unsafe { NS_OK && NS.driver != 0 && NS.link_up != 0 && NS.ip != 0 && NS.gateway != 0 }
}

// Reads the fifteen widget PROFILE keys out of the live UIPROFIL.YML, per the
// "an app's startup must SYNC FROM live state, never PUSH INTO it" rule
// (blame.md, 2026-08-10). Root's home during OOBE is "/" - the exact same
// hardcoded fallback profile_load() itself uses (compositor/profile.c
// prof_path()) - so this plain root-relative open reads the SAME file the
// compositor guarantees exists (compositor_init() calls profile_save() right
// after profile_load()). Falls back to the compiled-in defaults (2026-08-18:
// Digital Clock, Calendar, Uptime, Weather, HA, Maytera AI on, everything
// else off - kept in lockstep with clock.c/widgets.c's own g_show_* C
// initialisers, see WIDGETS_DEFAULT_MASK above) only if the read fails,
// matching the profile key's own compiled-in default the moment nothing on
// disk says otherwise.
// Modelled on write_dock_style()'s read side: same SYS_OPEN/SYS_READ/SYS_CLOSE
// shape, same "/UIPROFIL.YML" path, same "profile.c applies keys in file
// order, so a later duplicate line wins and no de-duplication is needed"
// property (this function keeps applying every matching line it sees, so the
// LAST one read is the one that sticks, exactly like the writer).
fn read_widgets_from_profile() -> u16 {
    let fd = unsafe { syscall3(SYS_OPEN, b"/UIPROFIL.YML\0".as_ptr() as i64, 0, 0) as i32 };
    if fd < 0 { return WIDGETS_DEFAULT_MASK; }
    let mut buf: [u8; 2048] = [0; 2048];
    let n = unsafe { syscall3(SYS_READ, fd as i64, buf.as_mut_ptr() as i64, 2047) };
    unsafe { syscall1(SYS_CLOSE, fd as i64); }
    if n <= 0 { return WIDGETS_DEFAULT_MASK; }
    let n = (n as usize).min(2047);
    let mut mask: u16 = WIDGETS_DEFAULT_MASK;
    let mut have_any = false;
    let mut i = 0usize;
    while i < n {
        let start = i;
        while i < n && buf[i] != b'\n' { i += 1; }
        let mut end = i;
        if i < n { i += 1; }
        if end > start && buf[end - 1] == b'\r' { end -= 1; }
        let line = &buf[start..end];
        // profile.c's put_kv() writes "key: value" (colon, one space) -
        // verified against write_dock_style()'s own byte sequence above.
        let mut colon: i32 = -1;
        let mut j = 0usize;
        while j < line.len() { if line[j] == b':' { colon = j as i32; break; } j += 1; }
        if colon < 0 { continue; }
        let key = &line[..colon as usize];
        let mut v = colon as usize + 1;
        while v < line.len() && line[v] == b' ' { v += 1; }
        let on = v < line.len() && line[v] == b'1';
        let mut k = 0usize;
        while k < WIDGETS_UI.len() {
            if appsw_key_eq(WIDGETS_UI[k].profkey, key) {
                if !have_any { mask = 0; have_any = true; }   // first real hit: stop trusting the default
                if on { mask |= 1u16 << k; } else { mask &= !(1u16 << k); }
                break;
            }
            k += 1;
        }
    }
    mask
}

// Byte-exact compare of a NUL-terminated constant against a raw (non-
// terminated) file slice - `key` must match `nul_term` for its WHOLE length,
// not just as a prefix, or "show_ha" would false-match a hypothetical
// "show_hair" and vice versa.
fn appsw_key_eq(nul_term: &[u8], key: &[u8]) -> bool {
    let mut i = 0usize;
    while i < key.len() {
        if i >= nul_term.len() || nul_term[i] == 0 || nul_term[i] != key[i] { return false; }
        i += 1;
    }
    i < nul_term.len() && nul_term[i] == 0
}

// App-cell label: centred in the 86px cell, one line for a single-word name,
// two lines split at the (single) space for "App Store"/"Media Player"/
// "Task Manager" (spec row 9). Never bold (10.2: gui_ttf_width() under-reads
// a bold run by 1px per glyph, and this page draws no label near its budget
// closely enough to need the correction anyway - see section 10.3's measured
// 22-28px of headroom on every app label).
fn appsw_draw_app_label(win: i32, cx: i32, cy: i32, name: &[u8], colour: u32) {
    let mut sp: i32 = -1;
    let mut i = 0usize;
    while i < name.len() && name[i] != 0 {
        if name[i] == b' ' { sp = i as i32; break; }
        i += 1;
    }
    if sp < 0 {
        let w = unsafe { gui_ttf_width(name.as_ptr(), 12) };
        dk_solid(win, cx + (APP_W - w) / 2, cy + 19, name, 12, false, colour);
        return;
    }
    let sp = sp as usize;
    let mut a: [u8; 24] = [0; 24];
    let mut b: [u8; 24] = [0; 24];
    let mut k = 0usize;
    while k < sp && k < 23 { a[k] = name[k]; k += 1; }
    a[k] = 0;
    let mut j = sp + 1;
    let mut m = 0usize;
    while j < name.len() && name[j] != 0 && m < 23 { b[m] = name[j]; m += 1; j += 1; }
    b[m] = 0;
    let wa = unsafe { gui_ttf_width(a.as_ptr(), 12) };
    let wb = unsafe { gui_ttf_width(b.as_ptr(), 12) };
    dk_solid(win, cx + (APP_W - wa) / 2, cy + 13, &a, 12, false, colour);
    dk_solid(win, cx + (APP_W - wb) / 2, cy + 29, &b, 12, false, colour);
}

// 7x7 hollow ring: the ring-then-hole construction dk_radio() already uses,
// at the needs-setup marker's own size (spec row 18, 1px stroke, DK_FIELD_LABEL).
fn appsw_ring(win: i32, cx: i32, cy: i32) {
    let outer = wel_composite_at(cx, cy);
    unsafe {
        gui_fill_circle_aa(win, cx, cy, 3, DK_FIELD_LABEL, outer);
        gui_fill_circle_aa(win, cx, cy, 2, outer, DK_FIELD_LABEL);
    }
}

impl App {
    // Shared chrome for the 6 left-aligned dark pages (Account, Signin,
    // Network, Appear, Wall, AI): background, title, subtitle, Back,
    // primary button, dots. PG_APPLY and PG_DONE are centred splash layouts
    // and draw their own chrome directly (see dk_draw_apply/dk_draw_done).
    // dot_idx is GONE: the active dot and the "Step N of M" string are both
    // derived from STEP_PAGES via card_chrome(), so a page cannot disagree with
    // its own counter and two pages cannot collide on one dot (PG_NETWORK and
    // PG_TIME both sat on dot 3, deliberately, with a comment declining to
    // renumber).
    fn dk_page_chrome(&self, title: &[u8], subtitle: &[u8]) {
        let win = self.win;
        dk_fill_bg(win, self.page);
        dk_solid(win, 32, 24, title, 20, true, DK_HEADLINE);
        dk_alpha(win, 32, 50, subtitle, 12, false, DK_BODY, 880);
        dk_back_button(win);
        dk_primary_button(win, b"Continue\0", false);
        // #745: quieter than both buttons by size (11px vs 12px) and by alpha,
        // and drawn ONLY where page_skippable() says it belongs, so the drawn
        // control and the hit-test in on_click() come from one predicate.
        if page_skippable(self.page) {
            let (lx, _, _, _) = skip_link_bounds();
            let ly = FOOTER_Y + 8;
            let col = wel_blend_over(DK_BODY, wel_composite_at(lx, ly), 800);
            dk_solid(win, lx, ly, SKIP_LABEL, 11, false, col);
        }
    }

    // (#wizflash) ONE table of PG_ACCOUNT's six field boxes (x, y, w, h,
    // placeholder), shared by the full page draw below AND draw_field_delta()
    // (see its own comment for why a keystroke needs a way to redraw ONE field
    // without repainting the page). Factored out rather than duplicating the
    // six dk_field() calls' literals a second time: blame.md already records a
    // case where two copies of the same layout numbers drifted apart because
    // nothing forced them to agree.
    fn account_field_geom(i: usize) -> (i32, i32, i32, i32, &'static [u8]) {
        match i {
            0 => (32, 90, 272, 28, b"Ada Lovelace\0" as &[u8]),
            1 => (336, 90, 272, 28, b"\0" as &[u8]),
            2 => (32, 146, 272, 28, b"\0" as &[u8]),
            3 => (336, 146, 272, 28, b"\0" as &[u8]),
            4 => (32, 228, 272, 28, b"\0" as &[u8]),
            _ => (336, 228, 272, 28, b"\0" as &[u8]),
        }
    }

    fn dk_draw_account(&mut self) {
        // #745: six fields now. Root's password is collected HERE, next to the
        // account password, rather than on a page of its own: the two rules
        // that bind them ("both must pass the policy" and "they must differ")
        // are only checkable together, and a rule you can only fail two screens
        // after the field is the failure mode this page already had once.
        self.nfields = 6;
        let win = self.win;
        self.dk_page_chrome(b"Create your account\0",
            b"This account signs you in. The root account owns the system files.\0");

        dk_label(win, 32, 76, b"FULL NAME\0", false);
        let (x, y, w, h, ph) = Self::account_field_geom(0);
        dk_field(win, x, y, w, h, &self.fullname, ph, self.focus == 0, false);
        dk_label(win, 336, 76, b"USERNAME\0", false);
        let (x, y, w, h, ph) = Self::account_field_geom(1);
        dk_field(win, x, y, w, h, &self.username, ph, self.focus == 1, false);

        dk_label(win, 32, 132, b"PASSWORD\0", false);
        let (x, y, w, h, ph) = Self::account_field_geom(2);
        dk_field(win, x, y, w, h, &self.pw, ph, self.focus == 2, false);
        dk_label(win, 336, 132, b"CONFIRM PASSWORD\0", false);
        let (x, y, w, h, ph) = Self::account_field_geom(3);
        dk_field(win, x, y, w, h, &self.pw2, ph, self.focus == 3, false);

        if !self.err.is_empty() {
            // Rows: 0/1 name+username (fields y=90..118), 2/3 passwords
            // (146..174), 4/5 root (228..256). Unattributed errors keep the
            // original position.
            let ey = match self.err_focus { 0 | 1 => 120, 4 | 5 => 258, _ => 178 };
            dk_solid(win, 32, ey, self.err, 12, false, DK_ERROR);
        }

        // #745 REMOVED FROM HERE: a "SIGN-IN METHOD" label with two radio
        // buttons. They were drawn and they were not wired: on_click has no
        // hit-test for their rows and on_key toggles require_pw only on
        // PG_SIGNIN, so clicking either one did nothing at all while looking
        // exactly like a choice. That is the same defect as the old "Set up
        // later" link, three hundred lines away, and it is deleted for the same
        // reason rather than wired up: PG_SIGNIN already owns this setting,
        // where the control DOES work. Removing it also frees the rows the
        // root-password fields now use.
        dk_hr(win, 32, 200, 576);

        dk_label(win, 32, 214, b"ROOT PASSWORD\0", false);
        let (x, y, w, h, ph) = Self::account_field_geom(4);
        dk_field(win, x, y, w, h, &self.rootpw, ph, self.focus == 4, false);
        dk_label(win, 336, 214, b"CONFIRM ROOT PASSWORD\0", false);
        let (x, y, w, h, ph) = Self::account_field_geom(5);
        dk_field(win, x, y, w, h, &self.rootpw2, ph, self.focus == 5, false);

        dk_solid(win, 32, 266, b"root owns the system files and must have its OWN password.\0",
                 12, false, DK_FINE_PRINT);
        dk_solid(win, 32, 286, b"Reusing your account password would mean one stolen password gives\0",
                 10, false, DK_FINE_PRINT);
        dk_solid(win, 32, 302, b"away the whole computer, so setup will not accept the same one twice.\0",
                 10, false, DK_FINE_PRINT);
        dk_solid(win, 32, 322, b"There is no way to recover it: write it down somewhere safe.\0",
                 10, false, DK_FINE_PRINT);
    }

    // #745 PG_SIGNIN, rebuilt from docs/OOBE_SIGNIN_OPTIONS.html.
    //
    // WHAT THIS REPLACES. The page said "Here is what you will see when <user>
    // starts MayteraOS" and drew a miniature of the lock screen. It carried no
    // controls at all: nine keyboard stops in this wizard and this page had
    // zero. It duplicated information supplied on the previous page, and the
    // preview had to be kept pixel-accurate against a lock screen that has since
    // changed twice. It is deleted outright, not adjusted.
    //
    // TWO REAL DECISIONS NOW LIVE HERE, and this page is the SINGLE OWNER of
    // both. Page 2 used to draw a second "SIGN-IN METHOD" radio pair for the
    // first of them; that group was already removed (see dk_draw_account) and
    // must not come back. Two controls writing one config key is how a setting
    // ends up with whichever page happened to be committed last.
    fn dk_draw_signin(&mut self) {
        self.nfields = 0;
        let win = self.win;

        // The subtitle is STATIC and does not interpolate the username. Both
        // questions on this page are about the computer and apply to every
        // account on it, so naming one account here would claim less than the
        // page does.
        self.dk_page_chrome(b"Signing in\0",
            b"Choose what happens when this computer starts, and how you sign in.\0");

        // ---- group 1: what happens at startup ---------------------------
        dk_label(win, 32, 84, b"AT STARTUP\0", false);
        if self.signin_focus == 0 { dk_focus_frame(win, 32, 98, 576, 76); }

        dk_radio(win, 38, 108, self.require_pw);
        dk_opt_label(win, 52, 100, b"Ask for my password\0", self.require_pw);
        dk_opt_sub(win, 52, 118, b"Required every time this computer starts.\0");

        dk_radio(win, 38, 146, !self.require_pw);
        let mut auto: [u8; 192] = [0; 192];
        let an = signin_auto_label(&mut auto, &self.username);
        dk_opt_label(win, 52, 138, &auto[..an + 1], !self.require_pw);
        dk_opt_sub(win, 52, 156,
            b"Starts straight to the desktop. Anyone who can switch this computer on can use it.\0");

        dk_hr(win, 32, 190, 576);

        // ---- group 2: which sign-in screen ------------------------------
        // NOT GREYED under auto-login, deliberately. On the Network page,
        // greying is truthful: with DHCP on, the static IP value is used by
        // nothing. This is not that situation. Auto-login covers COLD START
        // only; this machine still shows a sign-in screen on idle lock, on
        // explicit lock, on sign out and on switch user, and this setting
        // decides what that screen looks like every one of those times. Greying
        // it would say, in the vocabulary the Network page already taught this
        // person, that the setting does nothing - which is false, and a false
        // disabled state is worse than a less guided one. It is also a
        // MACHINE-WIDE setting that governs the other accounts on this computer,
        // and a control that governs other accounts must not be greyed on the
        // grounds that it is irrelevant to this one.
        //
        // #745 task #38 (user-reported 2026-08-12): the paragraph above is
        // still true, and is exactly why this is a HIDE rather than a
        // disable - the setting keeps working, it is just not asked here
        // anymore. The user was told the "still governs lock/sign-out/
        // switch-user" point directly and asked for it hidden under
        // autologin regardless; that decision stands. self.mode_list is
        // never touched by this branch, so whatever was last chosen (or the
        // "Show a list of users" default) is exactly what apply() commits
        // on Continue either way (see LOGIN_MODE_LIST/LOGIN_MODE_TYPED
        // below).
        if self.require_pw {
            dk_label(win, 32, 198, b"SIGN-IN SCREEN\0", false);
            if self.signin_focus == 1 { dk_focus_frame(win, 32, 210, 576, 76); }

            dk_radio(win, 38, 220, self.mode_list);
            dk_opt_label(win, 52, 212, b"Show a list of users\0", self.mode_list);
            dk_opt_sub(win, 52, 230,
                b"Every account name on this computer is visible before anyone signs in.\0");

            dk_radio(win, 38, 258, !self.mode_list);
            dk_opt_label(win, 52, 250, b"Ask for a name and password\0", !self.mode_list);
            dk_opt_sub(win, 52, 268, b"No account names are shown.\0");

            // The ONE mode-dependent element on the page, and what replaces
            // the guidance greying would have given: it says exactly WHEN
            // the sign-in screen still appears. That is strictly more
            // information than grey conveys, in the same space, using an
            // existing token at an existing position.
            dk_alpha(win, 32, 304, b"Shown every time you sign in.\0", 12, false, DK_BODY, 880);
        } else {
            // #745 task #38: HIDDEN, not drawn-and-disabled. Both on_key's
            // Tab handler (skips this stop) and on_click's PG_SIGNIN arm
            // (guards both row hit-tests with self.require_pw) agree with
            // this draw call about the state that hides the group - a
            // hidden control with a live hit-test is the exact defect the
            // paragraph above already documents once on this page, and it
            // is not being reintroduced here for the mouse or the keyboard.
            dk_alpha(win, 32, 204,
                b"Sign-in screen options moved to Settings > Users.\0",
                12, false, DK_BODY, 880);
            dk_alpha(win, 32, 224,
                b"You will still see one when you lock the screen, sign out, or switch user.\0",
                12, false, DK_BODY, 880);
        }

        // Fine print at FULL opacity, same as the four sub-lines. See dk_opt_sub.
        // Drawn either way, so it is never true only some of the time - this
        // is the on-page pointer to where the choice lives when the group
        // above is not drawn at all.
        dk_solid(win, 32, 326, b"You can change this anytime in Settings > Users.\0",
                 10, false, DK_FINE_PRINT);

        // Footer focus, drawn after dk_page_chrome laid the buttons down.
        if self.signin_focus == 2 { dk_focus_frame(win, 32, FOOTER_Y, 88, FOOTER_H); }
        if self.signin_focus == 3 { dk_focus_frame(win, 468, FOOTER_Y, 140, FOOTER_H); }
    }

    // (#wizflash) Same reasoning as account_field_geom() above: ONE table for
    // PG_NETWORK's four static-address fields, shared by the full page draw
    // and draw_field_delta().
    fn network_field_geom(i: usize) -> (i32, i32, i32, i32, &'static [u8]) {
        match i {
            0 => (32, 126, 272, 28, b"192.0.2.1\0" as &[u8]),
            1 => (336, 126, 272, 28, b"255.255.255.0\0" as &[u8]),
            2 => (32, 176, 272, 28, b"192.0.2.1\0" as &[u8]),
            _ => (336, 176, 272, 28, b"Same as gateway\0" as &[u8]),
        }
    }

    fn dk_draw_network(&mut self) {
        let win = self.win;
        let static_on = !self.dhcp;
        self.nfields = if static_on { 4 } else { 0 };
        self.dk_page_chrome(b"Network\0",
            b"MayteraOS gets an address automatically. Turn this on to set a fixed one.\0");

        dk_solid(win, 32, 80, b"Use a static address\0", 12, false, DK_BODY);
        dk_toggle(win, 572, 76, static_on);

        if static_on {
            // Manual configuration: unchanged editable form.
            dk_label(win, 32, 112, b"IP ADDRESS\0", false);
            let (x, y, w, h, ph) = Self::network_field_geom(0);
            dk_field(win, x, y, w, h, &self.ip, ph, self.focus == 0, false);
            dk_label(win, 336, 112, b"NETMASK\0", false);
            let (x, y, w, h, ph) = Self::network_field_geom(1);
            dk_field(win, x, y, w, h, &self.mask, ph, self.focus == 1, false);

            dk_label(win, 32, 162, b"GATEWAY\0", false);
            let (x, y, w, h, ph) = Self::network_field_geom(2);
            dk_field(win, x, y, w, h, &self.gw, ph, self.focus == 2, false);
            dk_label(win, 336, 162, b"DNS SERVER\0", false);
            let (x, y, w, h, ph) = Self::network_field_geom(3);
            dk_field(win, x, y, w, h, &self.dns, ph, self.focus == 3, false);

            if !self.err.is_empty() { dk_solid(win, 32, 210, self.err, 12, false, DK_ERROR); }
            return;
        }

        // ---- DHCP: show what actually happened, never a blank -------------
        if unsafe { !NS_OK } { self.net_refresh(); }
        let ns = unsafe { NS };
        let ok = unsafe { NS_OK };

        // Eyebrow + one-line state. These are the two lines that must never
        // lie: "no address yet" and "address but no route" are different
        // problems and get different words.
        let (eyebrow, state_msg, state_col) = if !ok {
            (b"NETWORK STATUS UNAVAILABLE\0" as &[u8],
             b"The kernel did not return a network status.\0" as &[u8], DK_ERROR)
        } else if ns.driver == 0 {
            (b"NO NETWORK ADAPTER\0" as &[u8],
             b"No network adapter was detected on this computer.\0" as &[u8], DK_ERROR)
        } else if ns.link_up == 0 {
            (b"LINK DOWN\0" as &[u8],
             b"No carrier. Plug in a cable or connect a network adapter.\0" as &[u8], DK_ERROR)
        } else if ns.ip == 0 && ns.dhcp_state != NET_DHCP_IDLE && ns.dhcp_state != NET_DHCP_BOUND {
            (b"REQUESTING AN ADDRESS\0" as &[u8],
             b"Asking a DHCP server for an address...\0" as &[u8], DK_BODY)
        } else if ns.ip == 0 {
            (b"NO ADDRESS\0" as &[u8],
             b"No DHCP server answered. Retry, or set a fixed address above.\0" as &[u8], DK_ERROR)
        } else if ns.gateway == 0 {
            (b"ADDRESS OBTAINED, NO ROUTE\0" as &[u8],
             b"An address was leased but no gateway was offered.\0" as &[u8], DK_ERROR)
        } else if !net_from_dhcp(&ns) && ns.config_static == 0 {
            // An address that no DHCP server sent. See net_from_dhcp().
            (b"FALLBACK ADDRESS (NO DHCP REPLY)\0" as &[u8],
             b"No DHCP server answered. These are built-in defaults, not a lease.\0" as &[u8],
             DK_ERROR)
        } else if ns.config_static != 0 {
            (b"CONFIGURED MANUALLY\0" as &[u8],
             b"This computer is using a saved fixed address, not DHCP.\0" as &[u8], DK_BODY)
        } else if ns.faulty != 0 {
            (b"ADDRESS OBTAINED\0" as &[u8],
             b"The network is marked unreachable; manual setup may be needed.\0" as &[u8], DK_ERROR)
        } else {
            (b"OBTAINED AUTOMATICALLY (DHCP)\0" as &[u8],
             b"These are the settings this computer is using right now.\0" as &[u8], DK_BODY)
        };
        dk_solid(win, 32, 104, eyebrow, 10, true, DK_EYEBROW);

        // Results card. Read-only VALUES, not disabled inputs.
        let outer = wel_composite_at(32 + 288, 118 + 46);
        gui_rr(win, 32, 118, 576, 92, 6, DK_CARD_FILL, outer);
        frame_inward(win, 32, 118, 576, 92, 1, DK_EDGE_GLASS);
        dk_result(win, 48, 130, b"IP ADDRESS\0", ns.ip, ns.prefix_len);
        dk_result(win, 344, 130, b"NETMASK\0", ns.netmask, 0);
        dk_result(win, 48, 172, b"GATEWAY\0", ns.gateway, 0);
        dk_result(win, 344, 172, b"DNS SERVER\0", ns.dns_active, 0);

        dk_solid(win, 32, 218, state_msg, 12, false, state_col);
        // Provenance of the resolver, because "8.8.8.8" appearing on a LAN
        // with its own DNS is a real finding, not a detail: the stack falls
        // back to a compiled-in default when DHCP offers none.
        if ok && net_from_dhcp(&ns) && ns.dns_active != 0 && ns.dns_dhcp == 0 {
            dk_solid(win, 32, 234, b"DNS server was not supplied by DHCP; a built-in default is in use.\0",
                     10, false, DK_FINE_PRINT);
        }

        // Retry / Test again.
        let (bx, by, bw, bh) = NET_BTN;
        let blabel: &[u8] = if ns.ip == 0 { b"Retry DHCP\0" } else { b"Test again\0" };
        frame_inward(win, bx, by, bw, bh, 1, DK_BACK_BORDER);
        let tw = unsafe { gui_ttf_width(blabel.as_ptr(), 11) };
        dk_solid(win, bx + (bw - tw) / 2, by + 7, blabel, 11, true, DK_BACK_LABEL);

        // ---- staged connection test ---------------------------------------
        dk_label(win, 32, 258, b"CONNECTION TEST\0", false);
        let names: [&[u8]; CT_N] = [
            b"Link\0",
            b"IP address\0",
            b"Gateway\0",
            b"DNS\0",
            b"Internet\0",
        ];
        let mut i = 0usize;
        while i < CT_N {
            let y = 276 + (i as i32) * 21;
            let st = unsafe { CT_ST[i] };
            ct_icon(win, 32, y, st);
            let ncol = match st {
                ST_PASS | ST_RUN => DK_BODY,
                ST_FAIL => DK_HEADLINE,
                _ => DK_INPUT_TEXT_DIS,
            };
            dk_solid(win, 54, y, names[i], 12, false, ncol);
            let mcol = match st {
                ST_PASS => DK_ACCENT,
                ST_FAIL => DK_ERROR,
                ST_RUN => DK_BODY,
                _ => DK_INPUT_TEXT_DIS,
            };
            let msg = unsafe { &CT_MSG[i] };
            let has = msg[0] != 0;
            let fallback: &[u8] = if st == ST_PEND { b"waiting\0" } else { b"\0" };
            if st == ST_RUN && has {
                // "contacting x (7s)". A running row that never changes is
                // indistinguishable from a wedged one; the counter is the
                // difference between "slow" and "stuck".
                let mut b = [0u8; CT_MSG_CAP + 12];
                let mut n = 0usize;
                while n < CT_MSG_CAP && msg[n] != 0 { b[n] = msg[n]; n += 1; }
                n += put(&mut b, n, b" (");
                n += put_u32(&mut b, n, ct_elapsed_s(i));
                n += put(&mut b, n, b"s)");
                b[n] = 0;
                dk_solid(win, 168, y, &b, 12, false, mcol);
            } else {
                dk_solid(win, 168, y, if has { msg } else { fallback }, 12, false, mcol);
            }
            i += 1;
        }

        if !self.err.is_empty() { dk_solid(win, 32, 388, self.err, 12, false, DK_ERROR); }
    }

    // Date & Time (PG_TIME, page 4). Replaces the earlier #745 follow-up's
    // plain scrolling TZ[] list with the real world-map city picker +
    // manual/NTP clock from docs/OOBE_TIME_APPEARANCE.html section 2 - the
    // two gaps a direct user complaint named ("no world map city picker...
    // nor hour/minute/second... nor NTP server").
    //
    // Step dot: Network(3) and Time still share dot index 3, unchanged from
    // the #745 follow-up - renumbering every later page's dot to make room
    // for a 10th stays rejected as unrelated churn.
    fn dk_draw_time(&mut self) {
        self.nfields = 0;
        let win = self.win;
        self.dk_page_chrome(b"Date & Time\0",
            b"Pick your time zone, then set the clock or let a time server keep it exact.\0");

        dk_label(win, 32, 72, b"LOCATION\0", false);

        // Map box: fill+border always drawn; the bitmap is blitted over it
        // only if map_load() succeeded at wizard-start, otherwise the flat
        // fill (identical to the ocean tone) IS the fallback per spec
        // section 4, plus one centred fine-print line. The search/list
        // panel below never depends on this succeeding.
        let map_outer = wel_composite_at(MAP_X + MAP_W / 2, MAP_Y + MAP_H / 2);
        gui_rr(win, MAP_X, MAP_Y, MAP_W, MAP_H, 4, WEL_BG_MID, map_outer);
        frame_inward(win, MAP_X, MAP_Y, MAP_W, MAP_H, 1, DK_EDGE_GLASS);

        unsafe {
            if MAP_OK {
                draw_image_body_raw6(win, MAP_X as i64, MAP_Y as i64,
                                 MAP_W as i64, MAP_H as i64, core::ptr::addr_of!(MAP_PX) as i64);
                // Idle markers first, so the committed selection (drawn
                // last, below) always sits on top even if two overlap.
                let mut i = 0usize;
                while i < TZC_COUNT {
                    if i != self.tzc_sel {
                        let (mx, my) = tz_marker_xy(TZC[i].lat_e2 as i32, TZC[i].lon_e2 as i32);
                        gui_fill_circle_aa(win, mx, my, 4, DK_THUMB_HALO, WEL_BG_MID);
                        gui_fill_circle_aa(win, mx, my, 3, DK_STROKE_UNSEL_B, DK_THUMB_HALO);
                    }
                    i += 1;
                }
                if self.tzc_sel < TZC_COUNT {
                    let (mx, my) = tz_marker_xy(TZC[self.tzc_sel].lat_e2 as i32, TZC[self.tzc_sel].lon_e2 as i32);
                    gui_fill_circle_aa(win, mx, my, 6, DK_THUMB_HALO, WEL_BG_MID);
                    gui_fill_circle_aa(win, mx, my, 4, DK_ACCENT, DK_THUMB_HALO);
                }
            } else {
                let msg: &[u8] = b"Map unavailable, use the list\0";
                let mw = gui_ttf_width(msg.as_ptr(), 10);
                dk_solid(win, MAP_X + (MAP_W - mw) / 2, MAP_Y + MAP_H / 2 - 5, msg, 10, false, DK_FINE_PRINT);
            }
        }

        // Zone list panel: the real, always-available keyboard interface to
        // the map (spec section 2 row 8).
        let (lpx, lpy, lpw, lph) = (400, 86, 208, 138);
        let lp_outer = wel_composite_at(lpx + lpw / 2, lpy + lph / 2);
        gui_rr(win, lpx, lpy, lpw, lph, 6, DK_CARD_FILL, lp_outer);
        frame_inward(win, lpx, lpy, lpw, lph, 1, DK_EDGE_GLASS);

        dk_field(win, lpx + 8, lpy + 6, 192, 22, &self.tz_search, b"Search city or zone...\0",
                 self.time_focus == 0, false);

        unsafe {
            let muted = wel_blend_over(DK_BODY, DK_CARD_FILL, 880);
            let mut i = 0usize;
            while i < 5 && self.tzc_first + i < TZC_FILT_COUNT {
                let fi = self.tzc_first + i;
                let idx = TZC_FILT[fi] as usize;
                let ry = lpy + 32 + (i as i32) * 18;
                let selected = idx == self.tzc_sel;

                let mut rowbuf: [u8; 64] = [0; 64];
                let mut n = 0usize;
                n += put(&mut rowbuf, n, str_trim(&TZC[idx].name));
                n += put(&mut rowbuf, n, b", ");
                n += put(&mut rowbuf, n, str_trim(&TZC[idx].country));
                if n < 63 { rowbuf[n] = 0; } else { rowbuf[63] = 0; }

                if selected {
                    rect(win, lpx + 8, ry, 2, 16, DK_ACCENT);
                    dk_solid(win, lpx + 16, ry + 2, &rowbuf, 11, true, DK_HEADLINE);
                    dk_checkmark(win, lpx + lpw - 16, ry + 8, 2, DK_ACCENT);
                } else {
                    dk_solid(win, lpx + 16, ry + 2, &rowbuf, 11, false, muted);
                }
                i += 1;
            }

            if TZC_FILT_COUNT > 5 {
                let tx = lpx + lpw - 8;
                let tt = lpy + 32;
                let tl = 5 * 18;
                gui_rr(win, tx, tt, 6, tl, 3, DK_PROGRESS_TRACK, DK_CARD_FILL);
                let mut len = tl * 5 / (TZC_FILT_COUNT as i32);
                if len < 24 { len = 24; }
                let span = (TZC_FILT_COUNT as i32 - 5).max(1);
                let top = tt + (tl - len) * (self.tzc_first as i32) / span;
                gui_rr(win, tx, top, 6, len, 3, DK_STROKE_UNSEL_B, DK_PROGRESS_TRACK);
            }
        }

        // Selected-location readout.
        let mut rbuf: [u8; 160] = [0; 160];
        self.time_readout(&mut rbuf);
        dk_alpha(win, 32, 228, &rbuf, 12, false, DK_BODY, 880);

        dk_hr(win, 32, 248, 576);

        dk_solid(win, 32, 256, b"Set time automatically (network time / NTP)\0", 12, false, DK_BODY);
        dk_toggle(win, 572, 252, self.ntp_on);

        // Date/time spinners: disabled (greyed) whenever NTP is on, same
        // grammar the Network page already uses for DHCP disabling the IP
        // fields.
        let dis = self.ntp_on;
        let sep_col = if dis { DK_INPUT_TEXT_DIS } else { DK_INPUT_PH };

        dk_label(win, 32, 280, b"DATE\0", dis);
        let mut yb: [u8; 5] = [0; 5]; fmt_fixed(&mut yb, 4, self.dt_year); yb[4] = 0;
        dk_spin(win, 32, 294, 56, 26, &yb, self.time_focus == 3, dis);
        dk_solid(win, 90, 300, b"-\0", 12, false, sep_col);
        let mut mb: [u8; 3] = [0; 3]; fmt_fixed(&mut mb, 2, self.dt_month); mb[2] = 0;
        dk_spin(win, 96, 294, 40, 26, &mb, self.time_focus == 4, dis);
        dk_solid(win, 138, 300, b"-\0", 12, false, sep_col);
        let mut db: [u8; 3] = [0; 3]; fmt_fixed(&mut db, 2, self.dt_day); db[2] = 0;
        dk_spin(win, 144, 294, 40, 26, &db, self.time_focus == 5, dis);

        dk_label(win, 220, 280, b"TIME (24H)\0", dis);
        let mut hb: [u8; 3] = [0; 3]; fmt_fixed(&mut hb, 2, self.dt_hour); hb[2] = 0;
        dk_spin(win, 220, 294, 40, 26, &hb, self.time_focus == 6, dis);
        dk_solid(win, 262, 300, b":\0", 12, false, sep_col);
        let mut mib: [u8; 3] = [0; 3]; fmt_fixed(&mut mib, 2, self.dt_min); mib[2] = 0;
        dk_spin(win, 268, 294, 40, 26, &mib, self.time_focus == 7, dis);
        dk_solid(win, 310, 300, b":\0", 12, false, sep_col);
        let mut sb: [u8; 3] = [0; 3]; fmt_fixed(&mut sb, 2, self.dt_sec); sb[2] = 0;
        dk_spin(win, 316, 294, 40, 26, &sb, self.time_focus == 8, dis);

        dk_label(win, 210, 350, b"NTP SERVER\0", false);
        dk_field(win, 210, 364, 220, 24, &self.ntp_server, b"\0", self.time_focus == 9, false);
        dk_tri(win, 210 + 220 - 12, 364 + 12, false, DK_INPUT_PH);
    }

    fn dk_draw_appear(&mut self) {
        self.nfields = 0;
        let win = self.win;
        self.dk_page_chrome(b"Appearance\0",
            b"Choose how MayteraOS looks, and where your dock sits. You can change both later.\0");

        dk_label(win, 32, 82, b"THEME\0", false);
        if self.nthemes == 0 {
            dk_solid(win, 32, 100, b"No themes found in /THEMES/INDEX.TXT.\0", 13, false, DK_ERROR);
        } else {
            // #745: 5 x 3 (was 7 x 2). The card grew from 74x64 to 108x72
            // because it now shows a 1:1 window corner, and 100px of crop is
            // what four real title-bar buttons need at the largest shipped
            // metric (Maytera: btn 20, gap 4 -> 94px). All 14 themes still fit
            // on the page with no scrolling.
            let n = self.nthemes.min(THG_COLS * THG_ROWS);
            let mut i = 0usize;
            while i < n {
                let col = (i % THG_COLS) as i32;
                let row = (i / THG_COLS) as i32;
                let t = unsafe { &THEMES[i] };
                dk_theme_mini_card(win, THG_X + col * THG_PITCH_X, THG_Y + row * THG_PITCH_Y,
                                   i == self.theme, &t.name, t.index);
                i += 1;
            }
            // Zone focus: ONE ring around the whole grid, outside it. The
            // per-card selection is a 3px INSET border, so focus and selection
            // are different objects on opposite sides of an edge and can never
            // be mistaken for one another (see dk_theme_mini_card).
            if self.appear_zone == 0 {
                let rows = ((n + THG_COLS - 1) / THG_COLS).max(1) as i32;
                dk_focus_frame(win, THG_X, THG_Y, THG_COLS as i32 * THG_PITCH_X - (THG_PITCH_X - THC_W),
                               rows * THG_PITCH_Y - (THG_PITCH_Y - THC_H));
            }
        }

        dk_label(win, 32, DOCK_LABEL_Y, b"DOCK STYLE\0", false);
        // #745: the labels come from libc's ONE dock-style list, the same one
        // Settings reads. This app used to hold a private copy that still said
        // "macOS style", "CDE panel" and "Amiga bar" long after Settings had
        // been renamed off them.
        let nd = unsafe { gui_dock_style_count() } as usize;
        let mut k = 0usize;
        while k < nd && k < 5 {
            let nm = unsafe { gui_dock_style_name(k as i32) };
            dk_dock_card_c(win, DOCK_X_0 + (k as i32) * DOCK_PITCH_X, DOCK_Y,
                           k == self.dock_style, k, nm);
            k += 1;
        }
        if self.appear_zone == 1 {
            dk_focus_frame(win, DOCK_X_0, DOCK_Y,
                           (nd.min(5) as i32) * DOCK_PITCH_X - (DOCK_PITCH_X - DOCK_W), DOCK_H);
        }
    }

    // #745: apply the highlighted wallpaper to the live desktop immediately, so
    // the user sees the actual picture rather than a 100x62 thumbnail. Same
    // syscall the apply step uses, so preview and final state cannot disagree.
    fn wall_apply_live(&self) {
        unsafe { syscall1(SYS_SET_WALLPAPER, self.wall as i64); }
        // The card is standing ON the wallpaper, so a live wallpaper change has
        // to rebuild the glass or the Desktop-picture page would show the new
        // picture around a card still blurring the old one.
        glass_build();
    }

    // #745 task #38: wall_page_next() REMOVED along with "Browse more...".
    // It had exactly one caller (the link's own click handler); once that
    // handler is gone this is dead code with zero callers, and dead code
    // with zero callers cannot be observed to be broken, so it does not
    // stay around as an untested trap. list_move() (below) is the surviving
    // pager: same wrap-at-the-end behaviour, driven by the arrow keys and
    // the wheel instead of a link.
    fn dk_draw_wall(&mut self) {
        self.nfields = 0;
        let win = self.win;
        self.dk_page_chrome(b"Desktop picture\0", b"Pick a wallpaper. You can add your own anytime.\0");

        if self.nwalls == 0 {
            dk_solid(win, 32, 100, b"No wallpapers found.\0", 13, false, DK_ERROR);
        } else {
            thumbs_load(self.wall_first, self.nwalls);
            let mut i = 0usize;
            while i < WALL_PAGE && self.wall_first + i < self.nwalls {
                let idx = self.wall_first + i;
                let cx = WALL_X0 + ((i % WALL_COLS) as i32) * WALL_PITCH_X;
                let cy = WALL_Y0 + ((i / WALL_COLS) as i32) * WALL_PITCH_Y;
                dk_thumb_cell(win, cx, cy, i, idx == self.wall);
                i += 1;
            }
            // #745 task #38 (user-reported 2026-08-12): "Browse more..." is
            // REMOVED - see wall_page_next()'s removal comment above. The
            // position readout STAYS: 64 wallpapers ship and up to 44
            // remain off-page even at 20 cells/page, so removing the
            // readout too would leave no sign more exist.
            if self.nwalls > WALL_PAGE {
                let last = if self.wall_first + WALL_PAGE < self.nwalls {
                    self.wall_first + WALL_PAGE
                } else { self.nwalls };
                let mut rb: [u8; 48] = [0; 48];
                let mut n = 0usize;
                n += put_u32(&mut rb, n, (self.wall_first + 1) as u32);
                n += put(&mut rb, n, b"-");
                n += put_u32(&mut rb, n, last as u32);
                n += put(&mut rb, n, b" of ");
                n += put_u32(&mut rb, n, self.nwalls as u32);
                rb[n] = 0;
                let rw = unsafe { gui_ttf_width(rb.as_ptr(), 10) };
                dk_solid(win, 608 - rw, WALL_READOUT_Y, &rb, 10, false, DK_FINE_PRINT);
            }
        }
    }

    // #745 task #15: PG_APPSW. dk_list() (below dk_thumb_cell) has ZERO
    // callers in this file; both grids below are drawn INLINE, following the
    // established precedent of PG_APPEAR's theme grid and PG_WALL's
    // wallpaper grid (both inline, neither uses dk_list()) rather than
    // reaching for an unused, unproven primitive on a page full of toggles.
    fn dk_draw_appsw(&mut self) {
        self.nfields = 0;
        let win = self.win;
        // Spec 5.3: refreshed on EVERY redraw, not just once - net_refresh()
        // is a single syscall reading globals, cheap enough for the draw path
        // (see the Network page's identical justification).
        self.net_refresh();
        let net_ok = appsw_net_present();

        self.dk_page_chrome(b"Apps and widgets\0",
            b"Choose your dock apps and desktop widgets. A ring marks a widget that needs setting up.\0");

        // ---- APPS ON YOUR DOCK ------------------------------------------
        dk_solid(win, 32, 78, b"APPS ON YOUR DOCK\0", 12, true, DK_FIELD_LABEL);
        {
            let n = self.apps_sel.count_ones();
            let mut cbuf: [u8; 24] = [0; 24];
            let mut cn = 0usize;
            cn += put_u32(&mut cbuf, cn, n);
            cn += put(&mut cbuf, cn, b" of 12 chosen");
            cbuf[cn] = 0;
            let cw = unsafe { gui_ttf_width(cbuf.as_ptr(), 12) };
            dk_solid(win, 608 - cw, 78, &cbuf, 12, false, DK_BODY);
        }

        let mut i = 0usize;
        while i < APPS_UI.len() {
            let col = (i % 6) as i32;
            let row = (i / 6) as i32;
            let cx = APP_X0 + col * APP_PITCH_X;
            let cy = APP_Y0 + row * APP_PITCH_Y;
            let on = self.apps_sel & (1u16 << i) != 0;
            if on {
                let fill = wel_blend_over(DK_ACCENT, wel_composite_at(cx + APP_W / 2, cy + APP_H / 2), 150);
                rect(win, cx, cy, APP_W, APP_H, fill);
                frame_inward(win, cx, cy, APP_W, APP_H, 2, DK_ACCENT);
            }
            // Deliberately NO fill/border on an unchecked cell (spec 7.3): an
            // opaque DK_CARD_FILL card measures 1.00:1 against the glass over
            // a dark wallpaper and is simply invisible there; identity is
            // carried by the checkbox and the label alone.
            dk_checkbox(win, cx + 6, cy + 6, on);
            appsw_draw_app_label(win, cx, cy, APPS_UI[i].label, if on { DK_HEADLINE } else { DK_BODY });
            if self.appsw_zone == 0 && self.apps_focus == i { dk_focus_frame(win, cx, cy, APP_W, APP_H); }
            i += 1;
        }

        dk_solid(win, 32, 214,
            if net_ok { b"Every app stays in the Start menu. Pin more later, or add new ones from the App Repo.\0" as &[u8] }
            else { b"Every app stays in the Start menu. The App Repo needs a network connection.\0" as &[u8] },
            12, false, DK_BODY);

        // ---- DESKTOP WIDGETS ----------------------------------------------
        dk_solid(win, 32, 240, b"DESKTOP WIDGETS\0", 12, true, DK_FIELD_LABEL);
        {
            let n = self.widgets_sel.count_ones();
            let mut cbuf: [u8; 24] = [0; 24];
            let mut cn = 0usize;
            cn += put_u32(&mut cbuf, cn, n);
            cn += put(&mut cbuf, cn, b" of 15 on");
            cbuf[cn] = 0;
            let cw = unsafe { gui_ttf_width(cbuf.as_ptr(), 12) };
            dk_solid(win, 608 - cw, 240, &cbuf, 12, false, DK_BODY);
        }

        let mut i = 0usize;
        while i < WIDGETS_UI.len() {
            let col = (i / 5) as i32;
            let row = (i % 5) as i32;
            let rx = WID_X0 + col * WID_PITCH_X;
            let ry = WID_Y0 + row * WID_PITCH_Y;
            let on = self.widgets_sel & (1u16 << i) != 0;
            if on {
                let fill = wel_blend_over(DK_ACCENT, wel_composite_at(rx + WID_W / 2, ry + WID_H / 2), 150);
                rect(win, rx - 4, ry, WID_W + 8, WID_H, fill);
            }
            dk_checkbox(win, rx, ry + 4, on);
            dk_solid(win, rx + 18, ry + 3, WIDGETS_UI[i].label, 12, false, if on { DK_HEADLINE } else { DK_BODY });
            if WIDGETS_UI[i].ring { appsw_ring(win, rx + 174 + 3, ry + 6 + 3); }
            if self.appsw_zone == 1 && self.widgets_cursor == i { dk_focus_frame(win, rx, ry, WID_W, WID_H); }
            i += 1;
        }

        // Detail line: follows the WIDGET CURSOR, which persists whether or
        // not the widgets zone holds focus, so this line is never stale and
        // never blank (spec row 19 / section 6.1).
        let cur = &WIDGETS_UI[self.widgets_cursor];
        let detail: &[u8] = if !net_ok && cur.detail_off.len() > 1 { cur.detail_off } else { cur.detail_on };
        dk_solid(win, 32, 386, detail, 12, false, DK_BODY);

        if self.appsw_zone == 2 { dk_focus_frame(win, 32, FOOTER_Y, 88, FOOTER_H); }
        if self.appsw_zone == 3 { dk_focus_frame(win, 468, FOOTER_Y, 140, FOOTER_H); }
    }

    // #745 follow-up: the AI provider page. Skip is deliberately NOT
    // re-drawn here as a page-local link: dk_page_chrome() already draws the
    // wizard-wide "Skip" control (#745 task #38: relabelled from "Set up
    // later (F10)", F10 still bound) for every page_skippable() page,
    // PG_AI included, and it is already wired to click + F10 + apply() with
    // defaults. A second, differently-positioned skip link saying the same
    // thing would be a parallel control this file's own conventions warn
    // against (dk_theme_card/dk_thumb_cell/dk_radio all share ONE selection
    // grammar rather than each inventing their own).
    fn ai_ensure_saved_checked(&mut self) {
        if self.ai_checked_saved { return; }
        self.ai_checked_saved = true;
        let mut scratch: [u8; 160] = [0; 160];
        self.ai_key_saved = ai_read_saved_key(&mut scratch) > 0;
    }

    // Selecting a card pre-fills endpoint/style/model. The model field is
    // overwritten ONLY if it still holds the PREVIOUS provider's untouched
    // default (spec section 3): once the person types anything themselves,
    // ai_model_touched latches true and switching providers never clobbers it
    // again, it only updates the read-only endpoint/hint text around it.
    fn select_ai_provider(&mut self, idx: usize) {
        if idx >= 8 { return; }
        self.ai_provider = idx;
        if !self.ai_model_touched {
            self.ai_model.set(AI_PROVIDERS[idx].model_default);
        }
    }

    // (#wizflash) PG_AI's own shift constant, factored out to ONE place: it
    // used to be a local `let shift = if is_custom {28} else {0}` inside
    // dk_draw_ai only, which meant ai_field_geom() below (needed so
    // draw_field_delta() can redraw a single field without repainting the
    // page - same reasoning as account_field_geom()/network_field_geom())
    // would otherwise have carried a SECOND copy of the same "28". One
    // function, two call sites, per this file's own standing rule against
    // exactly that kind of drift.
    fn ai_shift(is_custom: bool) -> i32 { if is_custom { 28 } else { 0 } }

    // (#wizflash) ONE table of PG_AI's three text fields (endpoint, model,
    // key), shared by the full page draw (dk_draw_ai) and draw_field_delta().
    // Depends on is_custom because selecting the Custom provider shifts the
    // model/key rows down 28px (ai_shift() above) - the endpoint row itself
    // never moves, it only appears/disappears.
    fn ai_field_geom(focus: usize, is_custom: bool) -> (i32, i32, i32, i32, &'static [u8]) {
        let shift = Self::ai_shift(is_custom);
        match focus {
            1 => (32, 194, 576, 26,
                  b"https://my-llm-gateway.internal/v1/chat/completions\0" as &[u8]),
            2 => (32, 214 + shift, 576, 28, b"your-model-id\0" as &[u8]),
            _ => (32, 280 + shift, 576, 30, b"Paste your API key\0" as &[u8]),
        }
    }

    // (#wizflash) A saved key with nothing typed THIS session shows the fixed
    // 12-dot "KEY SET" state (spec Stage C) instead of the live field; the
    // instant the person types anything (or backspaces a live edit back to
    // empty), the page's own visible STRUCTURE changes, not just the field's
    // text - so the event loop below must NOT treat that keystroke as a
    // cheap single-field redraw. Shared by dk_draw_ai and the event loop's
    // before/after snapshot so both agree on what "changed" means.
    fn ai_key_show_saved(&self) -> bool { self.ai_key_saved && self.aikey.is_empty() }

    fn dk_draw_ai(&mut self) {
        self.nfields = 0;   // focus lives in ai_focus, not the generic nfields ring
        self.ai_ensure_saved_checked();
        let win = self.win;
        self.dk_page_chrome(b"AI assistant\0",
            b"Pick an AI provider and add a key to enable chat, search and app automation.\0");

        dk_label(win, 32, 68, b"AI PROVIDER\0", false);
        const AI_COLS: [i32; 4] = [32, 178, 324, 470];
        const AI_ROWS: [i32; 2] = [84, 132];
        let mut i = 0usize;
        while i < 8 {
            let col = i % 4; let row = i / 4;
            let p = &AI_PROVIDERS[i];
            dk_provider_card(win, AI_COLS[col], AI_ROWS[row], 138, 40, i == self.ai_provider, p.name, p.desc);
            i += 1;
        }

        let is_custom = self.ai_provider == AI_CUSTOM;
        let p = &AI_PROVIDERS[self.ai_provider];
        // Stage B (spec): selecting Custom swaps the read-only endpoint info
        // line for an editable Endpoint URL field, and shifts everything
        // below it down 28px to make room - one shift constant, not two
        // parallel layouts, so the two states cannot drift apart.
        let shift = Self::ai_shift(is_custom);

        if is_custom {
            dk_label(win, 32, 180, b"ENDPOINT URL\0", false);
            let (x, y, w, h, ph) = Self::ai_field_geom(1, is_custom);
            dk_field(win, x, y, w, h, &self.ai_endpoint, ph, self.ai_focus == 1, false);
        } else {
            dk_alpha(win, 32, 180, p.endpoint_info, 12, false, DK_BODY, 880);
        }

        let model_label_y = 200 + shift;
        dk_label(win, 32, model_label_y, b"MODEL\0", false);
        let (x, model_field_y, w, h, ph) = Self::ai_field_geom(2, is_custom);
        dk_field(win, x, model_field_y, w, h, &self.ai_model, ph, self.ai_focus == 2, false);
        if is_custom {
            dk_solid(win, 32, model_field_y + 34,
                     b"No default for a custom endpoint. Enter the exact model id your server expects. Uses Bearer auth.\0",
                     10, false, DK_FINE_PRINT);
        } else {
            let mut hint: [u8; 80] = [0; 80];
            let mut hn = 0usize;
            hn += put(&mut hint, hn, b"Prefilled default for ");
            hn += put_trim0(&mut hint, hn, p.name);
            hn += put(&mut hint, hn, b". Type any model name.");
            hint[hn.min(79)] = 0;
            dk_solid(win, 32, model_field_y + 32, &hint, 10, false, DK_FINE_PRINT);
        }

        let key_label_y = 266 + shift;
        let (kx, key_field_y, kw, kh, kph) = Self::ai_field_geom(3, is_custom);
        dk_label(win, 32, key_label_y, b"API KEY\0", false);
        // A saved key with nothing typed THIS session shows the fixed
        // 12-dot "KEY SET" state (spec Stage C): the real saved length is
        // never drawn, only whether one exists at all. The instant the
        // person types anything, the normal live field takes over below.
        let show_saved = self.ai_key_show_saved();
        if show_saved {
            let bw2 = unsafe { gui_ttf_width(b"KEY SET\0".as_ptr(), 9) } + 12;
            let (bx, by) = (88, key_label_y - 2);
            gui_rr(win, bx, by, bw2, 14, 3, DK_BADGE_FILL, wel_composite_at(bx + bw2 / 2, by));
            dk_solid(win, bx + 6, by + 3, b"KEY SET\0", 9, true, DK_BADGE_TEXT);
            dk_mask_preview(win, kx, key_field_y, kw, kh, 12);
            if self.ai_focus == 3 {
                // Was a 2 px INWARD border, i.e. the same grammar grids use for
                // selection. Uses the shared ring like every other control now.
                dk_focus_frame(win, kx, key_field_y, kw, kh);
            }
        } else {
            dk_field(win, kx, key_field_y, kw, kh, &self.aikey, kph,
                     self.ai_focus == 3, false);
        }

        let status_y = key_field_y + kh + 6;
        if show_saved {
            dk_solid(win, 32, status_y,
                     b"A key is already saved. Type to replace it, or leave it as-is.\0",
                     10, false, DK_FINE_PRINT);
        }

        if !is_custom {
            dk_solid(win, 32, 336 + shift, p.signup, 10, false, DK_FINE_PRINT);
        }
    }

    // Arrow keys move focus across the 4x2 provider grid (moving past the
    // grid's own edge does nothing, no wrap - same rule appear_on_key()
    // already uses for the theme grid) into the endpoint (Custom only) ->
    // model -> key stops. Tab cycles the same stops in order; Shift+Tab is
    // not implemented anywhere in this app (GuiEvent carries no modifier
    // bit, see time_tab_next()'s comment), so neither is it here.
    fn ai_tab_next(&mut self) {
        let custom = self.ai_provider == AI_CUSTOM;
        let mut f = self.ai_focus;
        loop {
            f = (f + 1) % 4;
            if f == 1 && !custom { continue; }
            break;
        }
        self.ai_focus = f;
    }

    fn ai_on_key(&mut self, ev: &GuiEvent) -> bool {
        let c = ev.key_char;
        let is_custom = self.ai_provider == AI_CUSTOM;
        match self.ai_focus {
            0 => {
                let col = self.ai_provider % 4;
                let row = self.ai_provider / 4;
                let mut idx = self.ai_provider;
                if ev.keycode == KC_LEFT { if col > 0 { idx -= 1; } }
                else if ev.keycode == KC_RIGHT { if col < 3 { idx += 1; } }
                else if ev.keycode == KC_UP { if row > 0 { idx -= 4; } }
                else if ev.keycode == KC_DOWN { if row < 1 { idx += 4; } }
                if idx != self.ai_provider { self.select_ai_provider(idx); }
            }
            1 if is_custom => {
                if c == 8 || c == 127 { self.ai_endpoint.pop(); }
                else if c >= 32 && c < 127 { self.ai_endpoint.push(c); }
            }
            2 => {
                if c == 8 || c == 127 { self.ai_model.pop(); self.ai_model_touched = true; }
                else if c >= 32 && c < 127 { self.ai_model.push(c); self.ai_model_touched = true; }
            }
            3 => {
                if c == 8 || c == 127 { self.aikey.pop(); }
                else if c >= 32 && c < 127 { self.aikey.push(c); }
            }
            _ => {}
        }
        true
    }

    // Checklist rows are 6 (per spec), the REAL apply() sub-steps are 8
    // (0..7, see apply()'s comment) because two real actions each fold under
    // one spec row rather than inventing two extra rows the spec never drew:
    // account-create+signin-policy under "Create user account", and
    // network+timezone under "Configure network". "Apply theme" and "Set
    // desktop picture" get their OWN sub-step boundary each (apply() now
    // draws between them) specifically so they are not shown finishing
    // simultaneously - see the fake-simultaneous-progress note in apply().
    fn dk_draw_apply(&mut self) {
        self.nfields = 0;
        let win = self.win;
        dk_fill_bg(win, self.page);
        dk_centered(win, 118, b"PLEASE WAIT\0", 10, true, DK_EYEBROW);
        dk_centered(win, 136, b"Setting up MayteraOS\0", 26, true, DK_HEADLINE);

        // #126: the reduced flow does none of the machine-scope work, so it
        // must not claim to. A checklist that ticks "Create user account" for a
        // step that never ran is the same class of lie as the old "Set up
        // later" button that skipped nothing.
        let msg: &[u8] = if personalise() {
            match self.substep {
                0 | 1 | 2 | 3 | 4 => b"Applying your theme...\0",
                5     => b"Setting your desktop picture...\0",
                _     => b"Finishing up...\0",
            }
        } else {
            match self.substep {
                0 | 1 => b"Creating your account...\0",
                2 | 3 => b"Configuring network...\0",
                4     => b"Applying your theme...\0",
                5     => b"Setting your desktop picture...\0",
                6     => b"Saving AI settings...\0",
                _     => b"Finishing setup...\0",
            }
        };
        dk_centered_alpha(win, 172, msg, 12, false, DK_BODY, 880);

        let pct = (self.substep * 100 / 7).min(100);
        let pbg = wel_composite_at(W / 2, 200);
        gui_rr(win, 170, 196, 300, 8, 4, DK_PROGRESS_TRACK, pbg);
        if pct > 0 { gui_rr(win, 170, 196, (300 * pct / 100).max(8), 8, 4, DK_ACCENT, pbg); }
        let mut pctbuf: [u8; 8] = [0; 8];
        fmt_pct(&mut pctbuf, pct);
        dk_centered(win, 212, &pctbuf, 10, false, DK_FINE_PRINT);

        // (label, "done" sub-step boundary); "in progress" is
        // [previous boundary, this boundary).
        let full_rows: [(&[u8], i32); 6] = [
            (b"Create user account\0", 2),
            (b"Configure network\0", 4),
            (b"Apply theme\0", 5),
            (b"Set desktop picture\0", 6),
            (b"Save AI settings\0", 7),
            (b"Finish setup\0", 8),
        ];
        // Same sub-step boundaries as the real work in apply(): theme lands at
        // 5, wallpaper at 6, the per-user marker at 8. Derived from the code
        // that runs, not invented for the picture.
        let pers_rows: [(&[u8], i32); 3] = [
            (b"Apply theme\0", 5),
            (b"Set desktop picture\0", 6),
            (b"Finish up\0", 8),
        ];
        let nrows: usize = if personalise() { 3 } else { 6 };
        let mut prev: i32 = if personalise() { 4 } else { 0 };
        let mut i: usize = 0;
        while i < nrows {
            let (label, done_at) = if personalise() { pers_rows[i] } else { full_rows[i] };
            let y = 232 + (i as i32) * 22;
            let state = if self.substep >= done_at { 2 } else if self.substep >= prev { 1 } else { 0 };
            dk_status_icon(win, 200, y, state);
            let col = match state { 2 => DK_INPUT_TEXT, 1 => DK_HEADLINE, _ => DK_INPUT_TEXT_DIS };
            dk_solid(win, 222, y - 2, label, 12, state == 1, col);
            prev = done_at;
            i += 1;
        }

        // Back: NOT DRAWN (spec table row 8) - setup cannot be interrupted
        // once started.
        dk_primary_button(win, b"Continue\0", true);
    }

    fn dk_draw_done(&mut self) {
        self.nfields = 0;
        let win = self.win;
        dk_fill_bg(win, self.page);

        let (ccx, ccy, cr) = (320, 132, 36);
        unsafe { gui_fill_rounded_grad(win, ccx - cr, ccy - cr, cr * 2, cr * 2, cr, DK_BTN_TOP, DK_BTN_BOTTOM); }
        dk_checkmark(win, ccx, ccy, 15, 0xFFFFFFu32);

        dk_centered(win, 182, b"ALL SET\0", 10, true, DK_EYEBROW);
        dk_centered(win, 200, b"Welcome to MayteraOS\0", 30, true, DK_HEADLINE);
        dk_centered_alpha(win, 240, b"Your desktop is ready. Let's get started.\0", 12, false, DK_BODY, 880);

        if !self.apply_err.is_empty() {
            dk_centered(win, 268, self.apply_err, 12, false, DK_ERROR);
        }

        // Back IS kept on Finish (spec table row 5): "the person can still
        // go back and change the wallpaper or theme before committing" - see
        // the back()/next() change below that makes this actually work
        // rather than drawing a dead button.
        dk_back_button(win);
        dk_primary_button(win, b"Finish\0", false);
    }
}

// ---------------------------------------------------------------------------
// Page drawing
// ---------------------------------------------------------------------------
impl App {
    fn title_of(&self) -> &'static [u8] {
        match self.page {
            PG_ACCOUNT => b"Create your account\0",
            PG_SIGNIN  => b"Signing in\0",
            PG_NETWORK => b"Network\0",
            PG_TIME    => b"Date & Time\0",
            PG_APPEAR  => b"Appearance\0",
            PG_WALL    => b"Desktop picture\0",
            PG_APPSW   => b"Apps and widgets\0",
            PG_AI      => b"AI assistant\0",
            PG_APPLY   => b"Setting up MayteraOS\0",
            _ => b"\0",
        }
    }

    fn draw(&mut self) {
        match self.page {
            // Welcome is page 0 of THIS SAME window (see the block comment
            // above draw_welcome()) - it paints its own full-bleed
            // background covering the whole window, so it is handled
            // entirely separately from the chrome/footer/nav-button layout
            // every other page below shares.
            PG_WELCOME => self.draw_welcome(),
            // PG_TIME (#745 follow-up): ported to the dark chrome via
            // dk_draw_time() below, derived from the shared dk_* helpers
            // (see the block comment above the "Dark wizard pages" section).
            PG_TIME => self.dk_draw_time(),
            PG_ACCOUNT => self.dk_draw_account(),
            PG_SIGNIN  => self.dk_draw_signin(),
            PG_NETWORK => self.dk_draw_network(),
            PG_APPEAR  => self.dk_draw_appear(),
            PG_WALL    => self.dk_draw_wall(),
            PG_APPSW   => self.dk_draw_appsw(),
            PG_AI      => self.dk_draw_ai(),
            PG_APPLY   => self.dk_draw_apply(),
            PG_DONE    => self.dk_draw_done(),
            _ => {}
        }
        // #136's "Skip to Desktop" corner text used to draw here, on top of
        // whatever the match above just painted; it is removed (owner
        // request, 2026-08-28 - see the #229 FIRST-RUN STATE comment). The
        // #198 power corner (Restart/Shut Down) is a SEPARATE window
        // (pwr_win) with its own draw call (pwr_draw(), via pwr_pump()),
        // not part of this window's redraw at all.
        // #155: PRESENT the repaint that was just made. Since #131 (local 151)
        // the compositor blits content_presented, published only by this call,
        // so a page drawn without it reached the content buffer and stopped
        // there. That is what rendered every page after Welcome as an empty
        // glass card: the only thing publishing anything was SYS_WIN_DRAW_IMAGE
        // (self-committing since #131), so each page was published frozen at
        // its LAST image blit - the final backdrop strip, drawn before a single
        // glyph of the page. Welcome looked better only because its logo blit
        // comes after the header/footer chrome, so it published those three
        // things and none of its own text.
        // Put HERE, at the one place every redraw path funnels through
        // (EV_REDRAW / EV_KEY_DOWN / EV_MOUSE_DOWN / EV_MOUSE_SCROLL / the
        // hover band / net_tick), rather than at the eight call sites, so a
        // page or an event added later cannot forget it. sys_win_invalidate()
        // deliberately does not re-arm redraw_pending (#564), so this cannot
        // ping-pong with EV_REDRAW.
        win_invalidate(self.win);
    }

    // The ORIGINAL light-theme chrome + PG_TIME content, extracted verbatim
    // (byte-for-byte the same drawing calls that used to live inline in
    // draw()'s match). UNUSED as of the #745 follow-up dark port (draw()
    // now calls dk_draw_time() for PG_TIME) - kept for reference, same as
    // rich_list()/wall_grid() below, which have been unused since the
    // earlier #745 port moved PG_APPEAR/PG_WALL onto their dk_draw_*
    // equivalents.
    fn draw_time_light(&mut self) {
        let p = self.p;
        rect(self.win, 0, 0, W, H, p.bg);
        text(self.win, 40, 26, self.title_of(), 20, p.text);
        self.hairline(40, 64, 560, p.border);

        self.nfields = 0;
        text(self.win, 40, 80, b"Choose your time zone. The clock is set from the network.\0", 13, p.muted);
        let (sel, first, n) = (self.tz, self.tz_first, tz_len());
        self.list_compact(40, 104, 560, 282, n, sel, first, &|i| tz_id_bytes(i));
        text(self.win, 40, 392, tz_id_bytes(self.tz), 12, p.muted);

        rect(self.win, 0, 420, W, 60, p.surface);
        self.hairline(0, 420, W, p.border);
        self.step_dots();
        self.button2(40, 433, 96, b"Back\0", self.hover_nav == 1);
        self.button(504, 433, 96, b"Continue\0", true, self.hover_nav == 2);
    }

    fn centered(&self, y: i32, s: &[u8], size: i32, c: u32) {
        let w = unsafe { gui_ttf_width(s.as_ptr(), size) };
        text(self.win, (W - w) / 2, y, s, size, c);
    }

    fn rich_list(&self) {
        let p = self.p;
        let (x, y, w, h) = (40, 92, 560, 282);
        gui_rr(self.win, x, y, w, h, 6, p.field, p.bg);
        self.frame(x, y, w, h, 1, p.border2);
        let rows = 5usize;
        let mut i = 0;
        while i < rows && self.theme_first + i < self.nthemes {
            let idx = self.theme_first + i;
            let ry = y + 1 + (i as i32) * 56;
            if idx == self.theme { rect(self.win, x + 1, ry, w - 2, 56, p.tint); }
            let t = unsafe { &THEMES[idx] };
            text(self.win, x + 68, ry + 11, &t.name, 13, p.text);
            let sub: &[u8] = if t.is_dark != 0 { b"Dark theme\0" } else { b"Light theme\0" };
            text(self.win, x + 68, ry + 29, sub, 12, p.muted);
            // swatch: mini window = body fill + 6px titlebar strip
            gui_rr(self.win, x + 16, ry + 16, 36, 24, 3,
                   if t.is_dark != 0 { 0x1E1E1E } else { 0xFFFFFF }, p.field);
            rect(self.win, x + 17, ry + 17, 34, 6, if t.is_dark != 0 { 0x3A3A3C } else { 0xE8E8E8 });
            self.frame(x + 16, ry + 16, 36, 24, 1, p.border2);
            if idx == self.theme { text(self.win, x + w - 30, ry + 20, b"OK\0", 13, p.accent); }
            if i + 1 < rows && self.theme_first + i + 1 < self.nthemes {
                self.hairline(x + 1, ry + 56, w - 2, p.border);
            }
            i += 1;
        }
    }

    fn wall_grid(&self) {
        let p = self.p;
        thumbs_load(self.wall_first, self.nwalls);
        let cols = 5usize;
        let mut i = 0usize;
        while i < 20 && self.wall_first + i < self.nwalls {
            let idx = self.wall_first + i;
            let cx = 40 + ((i % cols) as i32) * 108;
            let cy = 96 + ((i / cols) as i32) * 72;
            // Pre-generated at build time (/WPTHUMB), decoded once per viewport
            // into .bss, blitted here. The placeholder still paints first so a
            // missing or unreadable thumbnail degrades to an empty cell rather
            // than to garbage.
            gui_rr(self.win, cx, cy, 100, 62, 4, p.surface, p.bg);
            unsafe {
                if i < THUMB_CELLS && THUMB_OK[i] {
                    let src = (core::ptr::addr_of!(THUMBS) as *const u32).add(i * THUMB_PX) as *const u8;
                    draw_image_body_raw6(self.win, cx as i64, cy as i64,
                                      100, 62, src as i64);
                }
            }
            self.frame(cx, cy, 100, 62, 1, p.border);
            if idx == self.wall {
                self.frame(cx - 4, cy - 4, 108, 70, 2, p.accent);
                unsafe { gui_fill_circle_aa(self.win, cx + 88, cy + 50, 8, p.accent, p.surface); }
            }
            i += 1;
        }
        text(self.win, 40, 388, unsafe { &WALLS[self.wall].name }, 12, p.muted);
    }
}

// ---------------------------------------------------------------------------
// Validation + apply
// ---------------------------------------------------------------------------
fn is_dotted_quad(f: &Field) -> bool {
    let mut parts = 0; let mut digits = 0; let mut val: u32 = 0; let mut i = 0;
    while i < f.n {
        let c = f.b[i];
        if c >= b'0' && c <= b'9' {
            val = val * 10 + (c - b'0') as u32; digits += 1;
            if digits > 3 || val > 255 { return false; }
        } else if c == b'.' {
            if digits == 0 { return false; }
            parts += 1; digits = 0; val = 0;
            if parts > 3 { return false; }
        } else { return false; }
        i += 1;
    }
    parts == 3 && digits > 0
}

// #154 (owner-reported 2026-08-17): the wizard's validation message used to
// be SET once, at Continue-time, and then left alone no matter what the
// person typed afterwards - clear both password fields to start over and
// "The passwords do not match." just sat there, describing a state that no
// longer existed. The general defect was "the message is an EVENT, not a
// STATE": every branch below used to live inline in validate() and nowhere
// else re-ran them.
//
// The fix makes the message a function of CURRENT field state instead of a
// latch: account_rule()/network_rule() are pure (no &mut self, no writes)
// and are the ONE place each rule is written down. validate() (Continue-time)
// and live_recheck() (after every keystroke, see below) both read from them,
// so the rules cannot drift into two different answers for the same fields.
//
// `tolerant` (account_rule only) relaxes ONLY the two password-pair equality
// checks, from "equal right now" to "not yet PROVABLY different"
// (pw_diverged: neither field is a prefix of the other). Retyping the second
// password one keystroke at a time spends most of its time as a valid prefix
// of a password that might still end up matching - that is ordinary typing,
// not an error, and must not keep a stale warning up or make it flicker back
// on for each correct character. Continue-time validation (tolerant = false)
// keeps plain equality: a field that is simply shorter and was never
// finished IS wrong at the moment Continue is pressed, however it got there.
fn pw_diverged(a: &Field, b: &Field) -> bool {
    if a.is_empty() || b.is_empty() { return false; }
    let n = if a.n < b.n { a.n } else { b.n };
    let mut i = 0;
    while i < n { if a.b[i] != b.b[i] { return true; } i += 1; }
    false
}

// The C-string convention this whole file uses for "cleared": every
// `self.err = b"\0"` (and its siblings, e.g. self.apply_err) writes a
// ONE-byte slice containing a single NUL, never a zero-length slice - so
// `.is_empty()` on it is ALWAYS false (len 1, not 0) and cannot be used to
// ask "is there currently a message". The draw-time `!self.err.is_empty()`
// guards elsewhere in this file get away with that because the text
// routine they feed stops at the leading NUL and paints nothing either way;
// live_recheck() below cannot get away with it, because it uses the answer
// to decide whether to do any work at all. Use this everywhere the question
// is "is this NUL-terminated slice logically empty", not `.is_empty()`.
fn strlen0(s: &[u8]) -> usize {
    let mut i = 0; while i < s.len() && s[i] != 0 { i += 1; } i
}

impl App {
    // Returns the first PG_ACCOUNT rule the CURRENT field state violates, as
    // (message, err_focus), or None if the page is currently valid. err_focus
    // of -1 means "no specific field" (draws at the default y). This never
    // touches self.focus (the KEYBOARD focus) - only validate() does that,
    // and only at Continue-time; a live recheck must never yank the cursor
    // out from under whatever field the person is still typing in.
    fn account_rule(&self, tolerant: bool) -> Option<(&'static [u8], i32)> {
        if self.fullname.is_empty() {
            return Some((b"Enter your full name.\0", 0));
        }
        if self.username.is_empty() {
            return Some((b"Enter a username.\0", 1));
        }
        let mut i = 0;
        while i < self.username.n {
            let c = self.username.b[i];
            let ok = (c >= b'a' && c <= b'z') || (c >= b'0' && c <= b'9')
                     || c == b'_' || c == b'-';
            if !ok { return Some((b"Use lowercase letters, digits, '-' or '_' only.\0", -1)); }
            i += 1;
        }
        // 'root' is a reserved system account (SYS_USER_CREATE_PW refuses it);
        // say so here rather than surfacing a bare -2 at the last step.
        if self.username.n == 4 && &self.username.b[0..4] == b"root" {
            return Some((b"'root' is reserved. Choose another username.\0", -1));
        }
        // #745. This used to be `self.pw.n < 6` against a kernel minimum of
        // EIGHT, plus seven other kernel rules and a 50,000-entry breached
        // list this page knew nothing about. So the page could accept a
        // password the kernel then refused at Apply, seven pages later, at
        // the one moment the user can do least about it. It now asks the
        // kernel the same question the kernel will answer again later.
        //
        // The kernel remains the authority: a negative return means the
        // syscall is unavailable, and the only correct response to that is
        // to keep a floor and let the kernel refuse at Apply, which apply()
        // handles and reports per-rule.
        let pc = pw_policy_check(self.username.cstr(), &self.pw);
        if pc > 0 { return Some((pw_msg(pc), 2)); }
        if pc < 0 && self.pw.n < 8 {
            return Some((b"Password must be at least 8 characters.\0", 2));
        }
        let pw_bad = if tolerant { pw_diverged(&self.pw, &self.pw2) } else { !self.pw.eq(&self.pw2) };
        if pw_bad { return Some((b"The passwords do not match.\0", 3)); }

        // Root's password, checked against the name "root" and NOT against
        // the human's: the contains-username rule is per name, and the
        // kernel splits it the same way (users_check_first_boot_pair).
        let rc = pw_policy_check(b"root\0", &self.rootpw);
        if rc > 0 { return Some((pw_msg_root(rc), 4)); }
        if rc < 0 && self.rootpw.n < 8 {
            return Some((b"Root password must be at least 8 characters.\0", 4));
        }
        let rootpw_bad = if tolerant { pw_diverged(&self.rootpw, &self.rootpw2) } else { !self.rootpw.eq(&self.rootpw2) };
        if rootpw_bad { return Some((b"The root passwords do not match.\0", 5)); }

        // Identical passwords put uid 0 back behind the desktop credential,
        // which is the whole thing the second field exists to prevent. The
        // kernel refuses this too; it is repeated here so the message
        // arrives while the fields are still on screen.
        if self.pw.eq(&self.rootpw) {
            return Some((pw_msg_root(PW_ERR_SAME_AS_OTHER), 4));
        }
        // Tell the user NOW, on the page where the field is, rather than
        // after they have walked seven more pages and hit Set Up.
        if username_taken(&self.username) {
            return Some((b"That username is already taken. Choose another.\0", -1));
        }
        None
    }

    // Same shape for PG_NETWORK's static-address fields. No tolerant variant:
    // a dotted-quad check has no "still might complete correctly" ambiguity
    // the way a two-field equality check does, so a plain re-check on every
    // keystroke already behaves correctly (stays up while genuinely
    // incomplete/invalid, clears the moment it parses).
    fn network_rule(&self) -> Option<(&'static [u8], i32)> {
        if !self.dhcp {
            if !is_dotted_quad(&self.ip) { return Some((b"Enter a valid IP address, e.g. 192.0.2.1.\0", -1)); }
            if !is_dotted_quad(&self.mask) { return Some((b"Enter a valid netmask, e.g. 255.255.255.0.\0", -1)); }
            if !self.gw.is_empty() && !is_dotted_quad(&self.gw) { return Some((b"Gateway is not a valid address.\0", -1)); }
            if !self.dns.is_empty() && !is_dotted_quad(&self.dns) { return Some((b"DNS server is not a valid address.\0", -1)); }
        }
        None
    }

    fn validate(&mut self) -> bool {
        self.err = b"\0"; self.err_focus = -1;
        if self.page == PG_ACCOUNT {
            if let Some((msg, ef)) = self.account_rule(false) {
                self.err = msg; self.err_focus = ef;
                // Land the keyboard on the offending field, same as before
                // this was refactored - but only for a real field index
                // (0..=5); the char-set/reserved-name/taken checks return
                // ef == -1 and, as before, leave focus wherever it was.
                if ef >= 0 { self.focus = ef as usize; }
                return false;
            }
            return true;
        }
        if self.page == PG_NETWORK {
            if let Some((msg, _)) = self.network_rule() { self.err = msg; return false; }
        }
        true
    }

    // #154: re-derive the on-screen message from current state after every
    // keystroke, so it can never go stale. Deliberately does nothing while
    // self.err is already empty - it only ever REFRESHES a message that is
    // already up (clearing it, changing it to whatever IS currently wrong,
    // or leaving it as-is), it never invents a new warning on a page nobody
    // has tried to advance from yet. That keeps the wizard's existing
    // "quiet until Continue" first impression: fields do not turn red just
    // because they are empty on a page you have not tried to leave.
    fn live_recheck(&mut self) {
        // NOTE: this is deliberately strlen0(), not self.err.is_empty().
        // Every "cleared" assignment in this file (including this function's
        // own None arms two lines down) writes the ONE-byte slice b"\0", a
        // C-string convention the draw-time checks (dk_solid's own
        // `!self.err.is_empty()` gates) tolerate only because the text
        // routine itself stops at the leading NUL and paints nothing - the
        // slice is never Rust-empty (len 1, not 0), so `.is_empty()` here
        // would ALWAYS be false and this guard would never fire, defeating
        // the whole "do nothing while nothing is shown" contract below.
        // Caught with a temporary debug-log build made for this ticket's own
        // verification (append self.err to a file on every live_recheck()
        // call, read it back offline): a version of this guard written as
        // `self.err.is_empty()` fired on the VERY FIRST keystroke of a blank
        // page, before Continue was ever pressed, because b"\0" is never
        // Rust-empty - it would have painted a live error early. Fixed to
        // strlen0() before this landed; left as a documented trap because it
        // is an easy mistake to reintroduce anywhere else in this file.
        if strlen0(self.err) == 0 { return; }
        if self.page == PG_ACCOUNT {
            match self.account_rule(true) {
                Some((msg, ef)) => { self.err = msg; self.err_focus = ef; }
                None => { self.err = b"\0"; self.err_focus = -1; }
            }
        } else if self.page == PG_NETWORK {
            match self.network_rule() {
                Some((msg, ef)) => { self.err = msg; self.err_focus = ef; }
                None => { self.err = b"\0"; self.err_focus = -1; }
            }
        }
    }

    // #745 task #15: apply the dock pin selection through FAVCH.CFG, the
    // proven P1 channel (startmenu_favs_poll(), ~1s throttle). One exec path
    // per line, IN GRID ORDER (spec: "which is dock order") - NOT prefixed
    // with "FAV|": sm_load_favs_channel() treats each trimmed line as a bare
    // path. Spec 9.3: zero apps checked means do not write the file at all -
    // zero favourites is already the shipped state, and writing an empty file
    // would needlessly risk the existing RECENT| lines sm_load_favs_channel()
    // reads from the SAME parse (this channel does not carry them, but an
    // empty write is still a write nobody asked for).
    fn write_appsw_favs(&self) {
        if self.apps_sel == 0 { return; }
        let mut buf: [u8; 512] = [0; 512];
        let mut n = 0usize;
        let mut i = 0usize;
        while i < APPS_UI.len() {
            if self.apps_sel & (1u16 << i) != 0 {
                n += put_trim0(&mut buf, n, APPS_UI[i].path);
                n += put(&mut buf, n, b"\n");
            }
            i += 1;
        }
        unsafe {
            let fd = userconf_open_write(b"FAVCH.CFG\0".as_ptr());
            if fd >= 0 { let _ = userconf_finish_write(fd, buf.as_ptr(), n as u64); }
        }
    }

    // #745 task #15: apply the widget selection through WIDGETCH.CFG, the
    // proven P2 channel (widgets_cfg_poll() -> traymenu_set_bind(), which is
    // what fires show_aichat's spawn/stop side effect). Written only if the
    // person touched a widget checkbox THIS VISIT (spec 9.3): "touched" and
    // "touched down to zero" both write all fifteen lines; "never touched"
    // writes nothing, so a page that was only looked at cannot turn Home
    // Assistant or AI Chat off by existing.
    fn write_appsw_widgets(&self) {
        if !self.appsw_widgets_touched { return; }
        let mut buf: [u8; 512] = [0; 512];
        let mut n = 0usize;
        let mut i = 0usize;
        while i < WIDGETS_UI.len() {
            let on = self.widgets_sel & (1u16 << i) != 0;
            n += put_trim0(&mut buf, n, WIDGETS_UI[i].bind);
            n += put(&mut buf, n, b"=");
            n += put(&mut buf, n, if on { b"1" } else { b"0" });
            n += put(&mut buf, n, b"\n");
            i += 1;
        }
        unsafe {
            let fd = userconf_open_write(b"WIDGETCH.CFG\0".as_ptr());
            if fd >= 0 { let _ = userconf_finish_write(fd, buf.as_ptr(), n as u64); }
        }
    }

    // Sub-steps are reported honestly: progress advances only on a COMPLETED
    // step, never on a timer, and a failed non-critical step does not dead-end
    // the wizard - it is reported on the Done page instead.
    fn apply(&mut self) -> bool {
        self.substep = 0; self.draw();

        // #126: THE REDUCED FLOW APPLIES ONLY WHAT IT ASKED ABOUT.
        //
        // Everything between here and the "self.substep = 4" theme block is
        // MACHINE scope: creating the first admin account, the startup/autologin
        // preference, the sign-in screen mode, the static IP, the timezone and
        // the hardware clock. A second user personalising their desktop has no
        // business writing any of it, and several of those calls would actively
        // damage the machine if they ran again (SYS_FIRSTBOOT_ADMIN would try to
        // mint another admin; SYS_SET_AUTOLOGIN would rewrite LOGIN.CFG for
        // whoever the wizard happened to be run by).
        //
        // Guarded as ONE block rather than per-call, because "which of these is
        // machine scope" is a single question with a single answer and splitting
        // it into six independent conditions is six chances to get it wrong.
        if !personalise() {
        // #229: MACHINE SCOPE SPLITS IN TWO, BECAUSE ROOT-ONLY AND MACHINE-WIDE
        // ARE NOT THE SAME PROPERTY.
        //
        // Everything under `!personalise()` is machine scope. Of it, exactly two
        // things additionally require ROOT: creating the account (root's
        // password is one of the three arguments) and rewriting
        // /CONFIG/LOGIN.CFG. Their pages are not in a non-root session's flow at
        // all (page_enabled / machine_admin), so running their apply steps
        // anyway would submit fields nobody was ever shown - empty ones - and
        // then report the kernel's refusal of them as the wizard's own failure.
        // That is exactly what a non-root first boot did: "The account could not
        // be created. Try a different username."
        //
        // The rest of the block - the time zone, the hardware clock - is machine
        // scope but NOT root-gated (sys_set_rtc_time/date take no euid check),
        // so it still runs and still takes effect. Guarded as ONE condition for
        // the same reason the outer block is: "which of these needs root" is a
        // single question with a single answer, and splitting it per call is a
        // chance to get it wrong per call.
        if machine_admin() {
        // #745. ONE call that creates the human account AND gives root its own
        // password, validating both under one decision before either lands.
        //
        // This used to be SYS_USER_CREATE_PW, which cannot touch root: it
        // refuses the reserved name by design. So setup finished with a fresh,
        // policy-checked desktop account sitting next to a uid 0 that still
        // held the default credential shipped in the asset base. The account
        // was new; the way in was not.
        //
        // The uid is still the kernel's to allocate (the handler owns the
        // allocator because callers that computed their own collided, and a
        // hardcoded 1000 fails on any shipped image where admin already holds
        // it), and the home path is still the kernel-derived /HOME/<NAME8>.
        let uid = unsafe {
            syscall3(SYS_FIRSTBOOT_ADMIN, self.username.cstr().as_ptr() as i64,
                     self.pw.cstr().as_ptr() as i64,
                     self.rootpw.cstr().as_ptr() as i64) as i32
        };
        if uid < 0 {
            // The kernel says WHICH password it refused and why, in two bands:
            // -201..-209 the account password, -221..-229 root's. Decode it,
            // because "the account could not be created" for eight different
            // rules is the message that sends someone back to a field that was
            // already fine.
            self.err = if uid <= -(PW_RC_ROOT_BASE + 1) && uid >= -(PW_RC_ROOT_BASE + PW_ERR_LAST) {
                pw_msg_root(-uid - PW_RC_ROOT_BASE)
            } else if uid <= -(PW_RC_BASE + 1) && uid >= -(PW_RC_BASE + PW_ERR_LAST) {
                pw_msg(-uid - PW_RC_BASE)
            } else if username_taken(&self.username) {
                b"That username is already taken on this computer.\0"
            } else {
                b"The account could not be created. Try a different username.\0"
            };
            return false;
        }

        self.substep = 1; self.draw();
        let en: i64 = if self.require_pw { 0 } else { 1 };
        let rc = unsafe {
            syscall3(SYS_SET_AUTOLOGIN, self.username.cstr().as_ptr() as i64,
                     self.pw.cstr().as_ptr() as i64, en) as i32
        };
        if rc != 0 {
            self.apply_err = b"Startup preference could not be saved; set it in Settings > Users.\0";
        }

        // #745 sign-in screen mode. ALWAYS written, in BOTH startup modes,
        // because it governs lock, sign out and switch user regardless of what
        // happens at boot - the mechanical consequence of not greying group 2.
        //
        // ORDER IS PART OF THE CONTRACT, not a style preference. SET_AUTOLOGIN
        // ran FIRST, above, and this runs SECOND, so that even an
        // implementation of sys_set_autologin() that rewrote the whole file
        // without preserving login_mode could not clobber what this writes.
        // The kernel composer preserves it too (rustkern/loginmode.rs). Both
        // defences are implemented: either one alone is a single point of
        // failure for a setting whose failure mode is silence.
        let want: i64 = if self.mode_list { LOGIN_MODE_LIST } else { LOGIN_MODE_TYPED };
        let rc_mode = unsafe {
            syscall3(SYS_SET_LOGIN_MODE, want,
                     self.username.cstr().as_ptr() as i64,
                     self.pw.cstr().as_ptr() as i64) as i32
        };
        // READ IT BACK and compare. A write nobody verified is how a setting
        // silently does nothing, and this wizard reports success by moving to
        // the Done page - so it must not do that for a write it did not see
        // land. On any disagreement the person is sent to Settings rather than
        // told it worked.
        let got = unsafe { syscall0(SYS_GET_LOGIN_MODE) };
        if rc_mode != 0 || got != want {
            // NOT "see Settings": Settings has no sign-in-screen control, so
            // that advice would send someone to look for a switch that is not
            // there. Name the fallback instead - login_mode_configured() reads
            // an absent key as LOGIN_MODE_TYPED, which is this exact wording.
            self.apply_err = b"Sign-in screen preference not saved; it will ask for a name and password.\0";
        }

        }   // #229 end of the ROOT-ONLY part of the machine-scope block

        self.substep = 2; self.draw();
        // #229: `&& machine_admin()`. /CONFIG/NETIP.CFG is a write to the same
        // root-owned directory as everything else this ticket moved, so a
        // non-root session cannot make it. It is a guard rather than a move to
        // SYS_FIRSTRUN because this branch is currently UNREACHABLE - the
        // Network page is compile-disabled and `dhcp` starts true and is
        // cleared only by that page - and an op with zero callers is a feature
        // that has never run. When NETWORK_PAGE_ENABLED goes back to true, add
        // FR_SET_NETIP (with a dotted-quad validator) in kernel/rustkern/
        // firstrun.rs and call it from here; do not reach for open() again.
        if !self.dhcp && machine_admin() {
            let mut b: [u8; 200] = [0; 200];
            let mut n = 0usize;
            n += put(&mut b, n, b"ip=");   n += putf(&mut b, n, &self.ip);   n += put(&mut b, n, b"\n");
            n += put(&mut b, n, b"mask="); n += putf(&mut b, n, &self.mask); n += put(&mut b, n, b"\n");
            if !self.gw.is_empty() { n += put(&mut b, n, b"gw="); n += putf(&mut b, n, &self.gw); n += put(&mut b, n, b"\n"); }
            // DNS empty means "same as gateway" per the spec's placeholder.
            let d = if self.dns.is_empty() { &self.gw } else { &self.dns };
            if !d.is_empty() { n += put(&mut b, n, b"dns="); n += putf(&mut b, n, d); n += put(&mut b, n, b"\n"); }
            let r = unsafe { userconf_write_all(b"/CONFIG/NETIP.CFG\0".as_ptr(), b.as_ptr(), n as u64) };
            if r != 0 { self.apply_err = b"Network settings could not be applied; see Settings > Network.\0"; }
        }

        self.substep = 3; self.draw();
        {
            // #49/#50: THE writer for THE setting, shared with Settings. The
            // hand-rolled "tz=" + TZ[self.tz] + newline formatting that used to
            // live here is gone: a second writer is how the file's format and
            // its readers come to disagree.
            let r = unsafe { tz_set_index(self.tz as i32) };
            if r != 0 { self.apply_err = b"Time zone could not be saved; see Settings.\0"; }
        }
        // The actual clock write (#timeapp): folded into this same
        // "Configuring network..." labelled substep rather than adding a
        // new checklist row/substep boundary - the checklist array has a
        // fixed 6 rows/7 boundaries this port does not need to touch.
        if self.clock_skip {
            // #745: "Set up later" was pressed at or before Date & Time, so
            // nobody chose a clock. Leave the RTC exactly as the firmware set
            // it. Writing this wizard's untouched dt_* defaults back would be
            // presenting a default as a decision.
        } else if self.ntp_on {
            let mut sbuf: [u8; 64] = [0; 64];
            let mut sn = 0usize;
            let mut i = 0usize;
            while i < self.ntp_server.n && sn < 63 { sbuf[sn] = self.ntp_server.b[i]; sn += 1; i += 1; }
            sbuf[sn] = 0;
            let r = unsafe { syscall2(SYS_NTP_SYNC_SERVER, sbuf.as_ptr() as i64, 5000) };
            if r != 0 { self.apply_err = b"Could not sync time from the network; set it manually in Settings.\0"; }
        } else {
            // #49: THE RTC HOLDS UTC. It always did (NTP writes UTC into it),
            // but until now nothing applied an offset on the way out, so writing
            // the local wall-clock time a person typed straight into it happened
            // to look right. Now that every clock adds the chosen offset when it
            // draws, storing local time here would double-count it: an Adelaide
            // user who typed 18:00 would see 03:30 the next morning. Convert by
            // shifting by the NEGATED offset of the zone chosen on this same
            // page (self.tz), which the TZ.CFG write above has just committed.
            let mut h = self.dt_hour; let mut mi = self.dt_min; let mut se = self.dt_sec;
            let mut dd = self.dt_day; let mut mo = self.dt_month; let mut yy = self.dt_year;
            unsafe {
                let off = tz_offset_min_at(self.tz as i32);
                tz_shift(-off, &mut h, &mut mi, &mut se, &mut dd, &mut mo, &mut yy);
            }
            let packed_date: i64 = ((yy as i64) << 16) | ((mo as i64) << 8) | (dd as i64);
            let packed_time: i64 = ((h as i64) << 16) | ((mi as i64) << 8) | (se as i64);
            unsafe {
                syscall1(SYS_SET_RTC_DATE, packed_date);
                syscall1(SYS_SET_RTC_TIME, packed_time);
            }
        }

        }   // #126 end of the machine-scope block

        // From here down is PER-USER, and is the whole of the reduced flow.
        // Every write below already lands in the session user's own home
        // (userconf/UIPROFIL.YML/THEME.CFG all resolve through the passwd
        // table), which is why the second user's choices cannot touch root's -
        // that half already worked and is deliberately untouched.
        self.substep = 4; self.draw();
        if self.pers_skip_from > PG_APPEAR {
        if self.nthemes > 0 && self.theme < self.nthemes {
            unsafe { gui_theme_activate(THEMES[self.theme].slug.as_ptr()); }
        }
        // Dock style (#timeapp): folded into the same "Apply theme" substep
        // since Appearance covers both theme and dock. See write_dock_style()
        // for why this writes /UIPROFIL.YML directly rather than through
        // userconf_open_write().
        write_dock_style_live(self.dock_style);
        write_dock_style(self.dock_style);
        }

        // #745 dark port: this used to be one sub-step ("theme and desktop
        // picture") but the dark PG_APPLY checklist draws "Apply theme" and
        // "Set desktop picture" as two SEPARATE rows (spec table rows 3-4).
        // Splitting the real work here too - one more draw() between the two
        // syscalls - means those two rows genuinely finish in sequence
        // instead of both silently flipping to "done" at once, which would
        // have violated the checklist's own "exactly one row in progress at
        // a time" rule with a fabricated instant. Substep numbering below is
        // renumbered accordingly: 0..7 (8 states), not the old 0..6.
        self.substep = 5; self.draw();
        if self.pers_skip_from > PG_WALL && self.nwalls > 0 && self.wall < self.nwalls {
            unsafe { syscall1(SYS_SET_WALLPAPER, self.wall as i64); }
        }

        // #745 task #15: dock pins and widget toggles (PG_APPSW). Folded into
        // this same substep window rather than adding a new checklist row -
        // the PG_APPLY checklist array has a fixed 6 rows / 7 boundaries this
        // port does not need to touch (see the #745 dark-port comment a few
        // lines up), and these two writes are invisible background config the
        // way write_dock_style_live()/write_dock_style() already are two
        // sub-steps back. ORDER MATTERS ONLY between these two calls and
        // nothing else: both are independent per-user channel files, so their
        // relative order to each other does not matter, only that both land
        // well before /CONFIG/SETUPDONE at the very end of this function.
        if self.pers_skip_from > PG_APPSW {
            self.write_appsw_favs();
            self.write_appsw_widgets();
        }

        self.substep = 6; self.draw();
        if !personalise() {
        {
            // #745 follow-up: write the REAL AISVC.CFG contract aiclient.c's
            // load_aisvc() reads (provider=/endpoint=/model=/api_style=/
            // api_key=), not the old "provider=moonshot\nkey=" pair the
            // client never actually parsed (key= was not even a field name
            // load_aisvc() recognizes - it was a silent no-op every time).
            let key_typed = !self.aikey.is_empty();
            let is_custom = self.ai_provider == AI_CUSTOM;
            let provider_changed = self.ai_provider != AI_DEFAULT_PROVIDER;
            let model_touched = self.ai_model_touched;
            let endpoint_typed = is_custom && !self.ai_endpoint.is_empty();

            // Resolve the key to persist: freshly typed, else the previously
            // saved one (read back transiently and written straight back to
            // the SAME per-user file, never logged/echoed), else none.
            let mut keybuf: [u8; 160] = [0; 160];
            let mut keylen = 0usize;
            if key_typed {
                keylen = self.aikey.n.min(keybuf.len());
                keybuf[..keylen].copy_from_slice(&self.aikey.b[..keylen]);
            } else if self.ai_key_saved {
                keylen = ai_read_saved_key(&mut keybuf);
            }
            let have_key = keylen > 0;

            if key_typed || provider_changed || model_touched || endpoint_typed || have_key {
                let p = &AI_PROVIDERS[self.ai_provider];
                let mut b: [u8; 512] = [0; 512];
                let mut n = 0usize;
                n += put(&mut b, n, b"provider=");
                n += put_trim0(&mut b, n, p.cfg_id);
                n += put(&mut b, n, b"\nendpoint=");
                if is_custom { n += putf(&mut b, n, &self.ai_endpoint); }
                else { n += put_trim0(&mut b, n, p.endpoint); }
                n += put(&mut b, n, b"\nmodel=");
                n += putf(&mut b, n, &self.ai_model);
                n += put(&mut b, n, b"\napi_style=");
                n += put(&mut b, n, if p.style == AI_STYLE_ANTHROPIC { b"anthropic" } else { b"bearer" });
                n += put(&mut b, n, b"\n");
                if have_key {
                    n += put(&mut b, n, b"api_key=");
                    n += put(&mut b, n, &keybuf[..keylen]);
                    n += put(&mut b, n, b"\n");
                }
                let fd = unsafe { userconf_open_write(b"AISVC.CFG\0".as_ptr()) };
                let ok = fd >= 0 && unsafe { userconf_finish_write(fd, b.as_ptr(), n as u64) } == 0;
                if !ok { self.apply_err = b"AI settings could not be saved; add them in Settings.\0"; }
            }
        }

        }   // #126 end of the AI block (machine-scope flow only)

        // ------------------------------------------------------------------
        // #126: TWO MARKERS, TWO DIFFERENT STATEMENTS.
        //
        //   /CONFIG/SETUPDONE       "THE MACHINE has been set up."
        //                           One absolute path, written once, on first
        //                           boot only. Unchanged meaning, unchanged
        //                           readers (compositor main.c :307/:1425/
        //                           :2594, setup_pending_recheck()).
        //
        //   <home>/CONFIG/SETUPUSR  "THIS USER has personalised their desktop."
        //                           Per user, resolved by the SAME passwd-table
        //                           join that already puts UIPROFIL.YML in the
        //                           right home (libc/userconf.c). Not a new
        //                           config system: it is one more name in the
        //                           per-user namespace that already exists.
        //
        // Root's home is "/", so root's per-user marker is /CONFIG/SETUPUSR -
        // beside SETUPDONE, and a different file from it. That is the same
        // no-op-for-root property userconf.c was built around, not a collision.
        //
        // ORDER: the per-user marker FIRST, then the machine marker. If the
        // machine marker landed first and the per-user write then failed, the
        // machine would be "configured" with a user who is told to personalise
        // again on the next login. The other way round, a failure means the
        // whole wizard re-runs, which is the failure the existing comment below
        // already argues for.
        //
        // The marker goes LAST and only once the account exists, so a hard
        // failure above re-runs setup on the next boot rather than stranding a
        // machine with no account and no way back into this wizard.
        self.substep = 7; self.draw();
        {
            let fd = unsafe { userconf_open_write(b"SETUPUSR\0".as_ptr()) };
            let ok = fd >= 0 && unsafe { userconf_finish_write(fd, b"1\n".as_ptr(), 2) } == 0;
            if !ok {
                self.err = b"Could not save your setup state. This will run again next time.\0";
                return false;
            }
        }
        if !personalise() {
            // #229: ASKED FOR, NOT WRITTEN. This was
            // userconf_write_all("/CONFIG/SETUPDONE"), which a uid-1000 session
            // cannot do, so on a virgin machine the wizard could never record
            // that it had finished and came back at every boot. The kernel owns
            // the marker now (SYS_FIRSTRUN / rustkern/firstrun.rs) and writes it
            // from Ring 0, after checking the fact the marker actually asserts:
            // that the machine has at least one account. This is the ONE op here
            // that can legitimately fail (-2 no account, -1 no disk), so unlike
            // the two per-boot signals it is checked.
            let r = unsafe { syscall1(SYS_FIRSTRUN, FR_MARK_DONE) };
            if r != 0 { self.err = b"Could not save setup state. Setup will run again.\0"; return false; }
        }
        true
    }
}

// Is this username already in /CONFIG/PASSWD? The image ships root, admin and
// ref, so a collision is the single most likely reason account creation fails -
// and it was previously reported as an unexplained generic error. Read the file
// directly rather than adding a syscall for a question the filesystem answers.
fn username_taken(u: &Field) -> bool {
    let fd = unsafe { syscall3(SYS_OPEN, b"/CONFIG/PASSWD\0".as_ptr() as i64, 0, 0) as i32 };
    if fd < 0 { return false; }
    let mut buf: [u8; 2048] = [0; 2048];
    let n = unsafe { syscall3(SYS_READ, fd as i64, buf.as_mut_ptr() as i64, 2048) };
    unsafe { syscall1(SYS_CLOSE, fd as i64); }
    if n <= 0 { return false; }
    let n = n as usize;
    let mut i = 0usize;
    while i < n {
        // compare up to the first ':' of this line against the typed name
        let ls = i;
        while i < n && buf[i] != b':' && buf[i] != b'\n' { i += 1; }
        let len = i - ls;
        if len == u.n {
            let mut same = true;
            let mut k = 0;
            while k < len { if buf[ls + k] != u.b[k] { same = false; break; } k += 1; }
            if same { return true; }
        }
        while i < n && buf[i] != b'\n' { i += 1; }
        i += 1;
    }
    false
}

// Load the 20 thumbnails for the current viewport. Called only when the base
// index changes, so scrolling costs one batch and idling costs nothing.
fn thumbs_load(first: usize, count: usize) {
    unsafe {
        if THUMB_BASE == first { return; }
        THUMB_BASE = first;
        let mut raw: [u8; 32768] = [0; 32768];   // a 100x62 24-bit BMP is ~18.7KB
        let mut i = 0usize;
        while i < THUMB_CELLS {
            THUMB_OK[i] = false;
            let idx = first + i;
            if idx >= count { i += 1; continue; }

            // "/WPTHUMB/" + the wallpaper's own filename
            let mut path: [u8; 64] = [0; 64];
            let pre = b"/WPTHUMB/";
            let mut n = 0usize;
            while n < pre.len() { path[n] = pre[n]; n += 1; }
            let f = &*core::ptr::addr_of!(WALLS[idx].file);
            let mut k = 0usize;
            while k < f.len() && f[k] != 0 && n < 63 { path[n] = f[k]; n += 1; k += 1; }
            path[n] = 0;

            let fd = syscall3(SYS_OPEN, path.as_ptr() as i64, 0, 0) as i32;
            if fd < 0 { i += 1; continue; }
            let got = syscall3(SYS_READ, fd as i64, raw.as_mut_ptr() as i64, 32768);
            syscall1(SYS_CLOSE, fd as i64);
            if got <= 0 { i += 1; continue; }

            let out = (core::ptr::addr_of_mut!(THUMBS) as *mut u32).add(i * THUMB_PX) as *mut u8;
            let target = ((THUMB_W as u32) << 16) | (THUMB_H as u32 & 0xFFFF);
            let mut dims: [i32; 2] = [0, 0];
            let r = syscall6(SYS_DECODE_IMAGE, raw.as_ptr() as i64, got,
                             target as i64, out as i64,
                             (THUMB_PX * 4) as i64, dims.as_mut_ptr() as i64);
            THUMB_OK[i] = r >= 0;
            // #745 diag: report the FIRST cell only, so serial names the failing
            // step instead of me guessing a fourth time.
            if i == 0 {
                let mut m: [u8; 96] = [0; 96];
                let pre = b"[SETUPTHUMB] fd/got/r = ";
                let mut n = 0usize;
                while n < pre.len() { m[n] = pre[n]; n += 1; }
                let vals: [i64; 3] = [fd as i64, got, r];
                let mut vi = 0usize;
                while vi < 3 {
                    let v = vals[vi];
                    let neg = v < 0;
                    let mut x = if neg { -v } else { v };
                    if neg { m[n] = b'-'; n += 1; }
                    let mut d: [u8; 20] = [0; 20];
                    let mut k = 0usize;
                    if x == 0 { d[k] = b'0'; k += 1; }
                    while x > 0 { d[k] = b'0' + (x % 10) as u8; x /= 10; k += 1; }
                    while k > 0 { k -= 1; m[n] = d[k]; n += 1; }
                    m[n] = b' '; n += 1;
                    vi += 1;
                }
                m[n] = 0;
                syscall1(298, m.as_ptr() as i64);   // SYS_BOOTLOG_WRITE
            }
            i += 1;
        }
    }
}

fn put(dst: &mut [u8], at: usize, s: &[u8]) -> usize {
    let mut i = 0; while i < s.len() && at + i < dst.len() { dst[at + i] = s[i]; i += 1; } i
}
fn putf(dst: &mut [u8], at: usize, f: &Field) -> usize {
    let mut i = 0; while i < f.n && at + i < dst.len() { dst[at + i] = f.b[i]; i += 1; } i
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
impl App {
    fn field_mut(&mut self, i: usize) -> &mut Field {
        match (self.page, i) {
            (PG_ACCOUNT, 0) => &mut self.fullname,
            (PG_ACCOUNT, 1) => &mut self.username,
            (PG_ACCOUNT, 2) => &mut self.pw,
            (PG_ACCOUNT, 3) => &mut self.pw2,
            (PG_ACCOUNT, 4) => &mut self.rootpw,
            (PG_ACCOUNT, 5) => &mut self.rootpw2,
            (PG_NETWORK, 0) => &mut self.ip,
            (PG_NETWORK, 1) => &mut self.mask,
            (PG_NETWORK, 2) => &mut self.gw,
            (PG_NETWORK, _) => &mut self.dns,
            _ => &mut self.aikey,
        }
    }

    // Read-only twin of field_mut(), for draw_field_delta() below, which only
    // ever needs to READ the field it is repainting.
    fn field_ref(&self, i: usize) -> &Field {
        match (self.page, i) {
            (PG_ACCOUNT, 0) => &self.fullname,
            (PG_ACCOUNT, 1) => &self.username,
            (PG_ACCOUNT, 2) => &self.pw,
            (PG_ACCOUNT, 3) => &self.pw2,
            (PG_ACCOUNT, 4) => &self.rootpw,
            (PG_ACCOUNT, 5) => &self.rootpw2,
            (PG_NETWORK, 0) => &self.ip,
            (PG_NETWORK, 1) => &self.mask,
            (PG_NETWORK, 2) => &self.gw,
            (PG_NETWORK, _) => &self.dns,
            _ => &self.aikey,
        }
    }

    // (#wizflash) Redraw ONLY the field that just changed, for the common
    // case of a keystroke that edits PG_ACCOUNT/PG_NETWORK text in place
    // (same page, same focus, no validation-message change - see the call
    // site in main()'s event loop for the exact guard). This is what makes a
    // keystroke stop flashing the wizard window on real hardware.
    //
    // WHY THIS IS SAFE WITHOUT REPAINTING THE BACKDROP FIRST, WHICH IS WHAT
    // draw() ALWAYS DOES: dk_field() (see its own doc comment) always fills
    // its ENTIRE box with an opaque colour before drawing its border/text, so
    // it never depends on what a previous frame left behind - unlike a label
    // or the page title, which alpha-blend their anti-aliased glyph edges
    // against whatever is already in the content buffer and would visibly
    // thicken, redrawn twice in a row without the backdrop under them being
    // repainted first. That is why this function touches ONLY the field box,
    // nothing else on the page.
    //
    // WHY THE FULL draw() FLASHES AND THIS DOES NOT: every full draw() starts
    // with dk_page_chrome() -> dk_fill_bg() -> card_paint_backdrop(), which
    // blits the translucent card background as up to 20 separate SYS_WIN_
    // DRAW_IMAGE calls (see card_paint_backdrop()'s own comment: "20 blits,
    // not 423,808 rects"). Each of those calls self-commits in the kernel
    // (sys_win_draw_image() always calls wm_invalidate_rect_async() +
    // uw_commit_content(), by design, for apps that never call
    // win_invalidate() themselves - see the block comment above
    // uw_commit_content() in kernel/proc/syscall.c). This app DOES call
    // win_invalidate() itself, once, at the end of every draw(), so those
    // per-strip self-commits are pure overhead for it: each one publishes an
    // INCOHERENT partial frame (part of the OLD frame's field text still
    // sitting under a freshly repainted patch of backdrop) that the
    // compositor's own, independently scheduled redraw can and does pick up
    // and show on the real screen before the next strip - or the final
    // win_invalidate() - ever runs. This does not need two cores: it is
    // ordinary preemptive scheduling landing between two of this app's own
    // syscalls, so it reproduces even with the single user core #514
    // measured on the reporting machine. dk_field()'s own primitives
    // (win_draw_rect/win_draw_pixel/win_draw_text_ttf_ex, reached through
    // gui_fill_rounded_aa()/gui_fill_circle_aa()/rect()/dk_solid()) never
    // self-commit, so calling only those and then ONE win_invalidate() here
    // publishes exactly one coherent frame, same as the ideal case draw()
    // itself is aiming for but defeats by also drawing the backdrop.
    //
    // This is NOT the #67/local-128 cross-core race (see blame.md): that bug
    // was window_invalidate() painting the shared framebuffer FROM THE WRONG
    // CORE and was fixed by routing through wm_invalidate_rect_async()
    // instead - a fix already present at every self-commit site this comment
    // just named. "Single-core, those two can never overlap" is that fix's
    // own conclusion, and the reporting machine has exactly one user core
    // (#514: g_smp_user_sched=0), so that mechanism is excluded here by
    // construction. This is a different bug the same self-commit machinery
    // can still produce on any core count: too many published intermediate
    // states, not a torn write to one of them.
    fn draw_field_delta(&mut self) {
        let win = self.win;
        match self.page {
            PG_ACCOUNT => {
                let (x, y, w, h, ph) = Self::account_field_geom(self.focus);
                let f = self.field_ref(self.focus);
                dk_field(win, x, y, w, h, f, ph, true, false);
            }
            PG_NETWORK => {
                let (x, y, w, h, ph) = Self::network_field_geom(self.focus);
                let f = self.field_ref(self.focus);
                dk_field(win, x, y, w, h, f, ph, true, false);
            }
            // (#wizflash) PG_AI's text fields (endpoint/model/key) measured
            // 21 SYS_WIN_DRAW_IMAGE publishes per keystroke before this fix
            // (card_paint_backdrop()'s 20 self-committing blits, plus the
            // page's own win_invalidate()) - the same full-page-redraw cost
            // PG_ACCOUNT/PG_NETWORK had, applied here with the same fix.
            // The event loop's guard only reaches this arm for ai_focus in
            // {1,2,3} (never 0, the provider grid - selecting a different
            // provider changes the card highlights, the endpoint info line,
            // the model prefill and the Custom shift, which is a real
            // structural change) AND only when ai_key_show_saved() has NOT
            // just flipped (that transition swaps a "KEY SET" badge for the
            // live field, also structural - see ai_key_show_saved()'s own
            // comment).
            PG_AI => {
                let is_custom = self.ai_provider == AI_CUSTOM;
                match self.ai_focus {
                    1 if is_custom => {
                        let (x, y, w, h, ph) = Self::ai_field_geom(1, is_custom);
                        dk_field(win, x, y, w, h, &self.ai_endpoint, ph, true, false);
                    }
                    2 => {
                        let (x, y, w, h, ph) = Self::ai_field_geom(2, is_custom);
                        dk_field(win, x, y, w, h, &self.ai_model, ph, true, false);
                    }
                    3 => {
                        let (x, y, w, h, ph) = Self::ai_field_geom(3, is_custom);
                        dk_field(win, x, y, w, h, &self.aikey, ph, true, false);
                    }
                    _ => {}
                }
            }
            // Any other page falls back to the caller doing a full draw() -
            // see the guard in main()'s event loop, which only ever reaches
            // here for PG_ACCOUNT/PG_NETWORK/PG_AI. Kept exhaustive rather
            // than unreachable!() so a future page added to that guard
            // without a matching arm here fails safe (draws nothing extra)
            // instead of panicking the wizard.
            _ => {}
        }
        win_invalidate(win);
    }

    fn next(&mut self) -> bool {
        if !self.validate() { return true; }
        if self.page == PG_DONE {
            // FINISH. ===============================================
            // #203/#126: this is the moment the machine changes hands.
            //
            // The owner reported "in the first run wizard we created a user
            // (james) and then it logged us in as root?", and every write this
            // wizard makes SUCCEEDS: the account exists, LOGIN.CFG says james.
            // The machine stays root's because the wizard runs INSIDE the
            // session that shipped autologin=root, and Finish only exits THIS
            // process. The compositor beneath it never goes anywhere.
            //
            // The handover the machine already has is Log Out: the compositor
            // exits, gui/desktop.c notices, and main.c's login gate re-runs
            // login_check_autologin() in the same boot. So Finish has to tell
            // the compositor to do exactly that.
            //
            // WHY A MARKER AND NOT /CONFIG/SETUPDONE. SETUPDONE is written at
            // the END OF apply(), which runs when Continue is pressed on the
            // last page - BEFORE this Done page is ever drawn. Keying the
            // compositor's exit off SETUPDONE takes the desktop away about
            // half a second after Apply, so the person never sees the Done
            // page and never presses Finish. The marker's NAME suggests
            // otherwise; its write site is what decides.
            //
            // ONLY THE FIRST-BOOT FLOW. In the reduced personalisation flow
            // (#126 Part B) the person is already signed in as themselves,
            // there is nothing to hand over, and exiting the compositor would
            // log them out for having chosen a wallpaper.
            //
            // Same shape as #136's /CONFIG/SETUPSKIP, deliberately: one
            // transient marker, written here, consumed and unlinked in
            // userland/apps/compositor/main.c. Not a new mechanism.
            //
            // #229: A KERNEL FLAG, NOT A FILE, AND ONLY WHEN THE MACHINE
            // ACTUALLY CHANGED HANDS.
            //
            // The file write was refused outright on a non-root session
            // (/CONFIG is root-owned 0711), and the compositor's own
            // "delete any stale SETUPNEW" cleanup - added by #203 because a
            // leftover marker logged somebody out of a machine that had not
            // changed hands - was a write to the same directory and so failed
            // on the same session. A one-boot signal stored on persistent
            // media manufactures that bug; a bit of kernel RAM starts clear at
            // every boot and cannot leave one behind. FR_HANDOVER_TAKE is a
            // CONSUMING read, so it also cannot fire twice.
            //
            // AND IT IS NOW CONDITIONAL ON machine_admin(). The handover
            // exists for one situation: the wizard created an account while
            // running inside somebody else's session (the shipped
            // autologin=root one), so the machine has to be handed to the
            // account that was just made. A non-root session did not create an
            // account - PG_ACCOUNT is not in its flow at all - and it is
            // ALREADY signed in as the owner. Logging that person out at
            // Finish would be a full sign-in cycle charged for choosing a
            // wallpaper.
            if !personalise() && machine_admin() {
                unsafe { syscall1(SYS_FIRSTRUN, FR_HANDOVER_SET); }
            }
            return false;   // Finish -> exit
        }
        // #126: the LAST page of the current flow runs Apply, whichever page
        // that is. The old `if self.page == PG_AI` hardcoded the full flow's
        // last page, so a reduced flow would have walked past its own end.
        if flow_next(self.page).is_none() {
            self.page = PG_APPLY;
            // #198: pwr_win is destroyed for the duration of the transient
            // progress page and recreated on leaving it - see pwr_destroy()'s
            // own comment for why destroying (not hiding) is the structural
            // fix for the #188 bug class.
            pwr_destroy();
            let ok = self.apply();
            self.page = if ok { PG_DONE } else { flow_error_page() };
            pwr_create();
            self.focus = 0;
            return true;
        }
        self.page = match flow_next(self.page) { Some(p) => p, None => self.page };
        self.focus = 0; self.err = b"\0";
        // #745: PG_SIGNIN's entry focus is the startup group, arriving from
        // either direction. The SELECTIONS are not reset - coming Back from
        // page 4 restores what was chosen - only the keyboard position is.
        self.signin_focus = 0;
        true
    }

    // #745: "Set up later". Discards the pages from here on, restores their
    // defaults, and runs the SAME apply() the Continue path runs - so the
    // account is created, the sign-in preference is written, and
    // /CONFIG/SETUPDONE lands, which is what stops the wizard reappearing on
    // the next boot. Skipping is not abandoning: it is finishing with defaults.
    //
    // page_skippable() is the ONE predicate for this; a page that refuses to be
    // skipped never draws the link and is refused here too, so a stray call can
    // not do what the missing control would not.
    fn skip(&mut self) -> bool {
        if !page_skippable(self.page) { return true; }
        // Everything before the current page was already validated by next(),
        // so only the current page and the ones after it are reset. A network
        // configuration the user already confirmed two pages ago is a decision,
        // not a leftover, and is kept.
        self.reset_optional_from(self.page);
        self.page = PG_APPLY;
        pwr_destroy();   // #198: see next()'s identical pair, above
        let ok = self.apply();
        // #229: flow_error_page(), not a hardcoded PG_ACCOUNT. next()'s Apply
        // path already went through that helper; this one did not, and on a
        // non-root session PG_ACCOUNT is not in the flow, so a failed "Set up
        // later" landed on a page the dots say does not exist and that Continue
        // cannot navigate out of. Two call sites, one answer.
        self.page = if ok { PG_DONE } else { flow_error_page() };
        pwr_create();
        self.focus = 0;
        true
    }

    // #136's skip_to_desktop() (the "Skip to Desktop" corner control) lived
    // here. Removed per owner request, 2026-08-28 - see the #229 FIRST-RUN
    // STATE comment above for the full reasoning and why nothing is
    // stranded by its removal.

    // The defaults each optional page would have applied if it had been shown
    // and left alone. Restoring them explicitly matters for the fields a user
    // may have half-typed before pressing the link: a static IP with no netmask
    // must not be written just because the page was open when they gave up.
    fn reset_optional_from(&mut self, from: usize) {
        // #126: IN THE REDUCED FLOW, SKIP MEANS "LEAVE MY SETTINGS ALONE",
        // NOT "SET THEM TO THE FIRST ENTRY OF EACH LIST".
        //
        // On first boot there is nothing to preserve, so restoring the page
        // defaults IS the honest reading of a skip. For a user who already has
        // a theme and a wallpaper, writing theme 0 and wallpaper 0 because they
        // declined to choose would be the wizard making a decision it was
        // explicitly told not to make - and it would do it silently, which is
        // the exact fault the "Set up later" comment above objects to.
        //
        // So the reduced flow records WHERE the person stopped and apply()
        // writes nothing from that page onwards. Pages they already passed are
        // decisions and are still applied, which is the same rule the full flow
        // uses ("a network configuration the user already confirmed two pages
        // ago is a decision, not a leftover").
        if personalise() {
            self.pers_skip_from = from;
            return;
        }
        // Sign-in: REQUIRE a password. The secure default, and the one the
        // wizard already starts with; stated here so a skip can never be the
        // path that quietly turns autologin back on.
        // #745: BOTH sign-in decisions revert to their page defaults, so a
        // skip can never be the path that quietly turns autologin back on, or
        // that leaves a half-set mode behind. "Show a list" is what the page
        // itself starts on, which is what a skip is defined to apply.
        if from <= PG_SIGNIN { self.require_pw = true; self.mode_list = true; }
        // Network: DHCP, discarding any partially-typed static configuration.
        if from <= PG_NETWORK {
            self.dhcp = true;
            self.ip.clear(); self.mask.clear(); self.gw.clear(); self.dns.clear();
        }
        // Clock: leave the hardware clock alone entirely (see apply()).
        if from <= PG_TIME { self.ntp_on = false; self.clock_skip = true; }
        // Appearance and desktop picture: the first entry of each list, which
        // is what the page itself starts on.
        if from <= PG_APPEAR {
            // (#wizdock) Re-default to the SAME choice a virgin machine
            // starts with (Marble / Fluent Dark), not to the pre-#wizdock
            // classic/index-0 pair. If the person had actually picked
            // something on the Appearance page, they would not be
            // navigating back PAST it while it still holds an unconfirmed
            // choice from a step this far forward - PG_APPEAR is where that
            // choice lives, so "back past PG_APPEAR" means "before the
            // Appearance page was ever reached this pass".
            self.theme = default_theme_idx(unsafe { &*core::ptr::addr_of!(THEMES) }, self.nthemes);
            self.dock_style = DOCK_XFCE_IDX;
        }
        if from <= PG_WALL { self.wall = 0; }
        // #745 task #15: apps/widgets. "I did not choose" means the system
        // keeps its shipped defaults (spec 9.3), which for apps is ZERO
        // favourites (that IS the shipped state - write_appsw_favs() already
        // no-ops on an empty selection, regardless of what APPS_DEFAULT_MASK
        // proposes on the page) and for widgets is simply not writing
        // WIDGETCH.CFG at all, so whatever the compositor already shipped
        // (2026-08-18: Digital Clock, Calendar, Uptime, Weather, Home
        // Assistant, Maytera AI - see clock.c/widgets.c compiled-in
        // defaults) stays on exactly as shipped. Clearing widgets_sel back
        // to the compiled-in default would be harmless here (apply() never
        // reads it when untouched) but is deliberately left alone: the
        // touched flag, not the bitset, is what apply() gates on. CONFIRMED
        // deliberate, not the Back-reset bug it might look like: resetting
        // apps_sel to 0 here means "propose nothing, change nothing", which
        // is correct precisely because write_appsw_favs() no-ops on zero -
        // resetting to APPS_DEFAULT_MASK instead would make a declined visit
        // FORCE-WRITE the defaults, which is the opposite of "I did not
        // choose" (verified against write_appsw_favs()'s own zero-guard
        // above; do not "fix" this without re-reading that guard).
        if from <= PG_APPSW { self.apps_sel = 0; self.appsw_widgets_touched = false; }
        // AI: no key. An empty key is already "not configured" in apply().
        if from <= PG_AI {
            self.aikey.clear();
            self.ai_provider = AI_DEFAULT_PROVIDER;
            self.ai_model.set(AI_PROVIDERS[AI_DEFAULT_PROVIDER].model_default);
            self.ai_model_touched = false;
            self.ai_endpoint.clear();
        }
    }

    fn back(&mut self) {
        // Floored at PG_WELCOME: welcome (page 0) has no Back of its own
        // (show_back above excludes it), and pressing Back on Account
        // (page 1) returns to Welcome, which now lives in this same window
        // - see draw_welcome()'s block comment for why the old two-window
        // split made that impossible and how this restores it.
        //
        // PG_DONE (#745 dark port): the spec draws a real, usable Back
        // button on the Finish page ("the person can still go back and
        // change the wallpaper or theme before committing"). A plain
        // self.page -= 1 would land on PG_APPLY, which is a transient
        // progress-only page with no Back/Continue of its own and no event-
        // loop pause (apply() runs it to completion synchronously) - a dead
        // end. So Done skips straight back to PG_AI instead. Pressing
        // Continue again from there re-runs apply(), including
        // SYS_USER_CREATE_PW for a username that already exists; that is
        // caught by the existing username_taken() check and reported as a
        // normal validation error, not a crash, but it IS a real, known
        // rough edge of this addition - flagged in the #745 port report,
        // not fixed here (out of scope for a colour/geometry port).
        if self.page == PG_DONE {
            // #126: the last page of the CURRENT flow, not a hardcoded PG_AI.
            self.page = flow_last(); self.focus = 0; self.err = b"\0";
            return;
        }
        if let Some(prev) = flow_prev(self.page) {
            self.page = prev; self.focus = 0; self.err = b"\0";
            self.signin_focus = 0;   // #745: see next()
        }
    }

    // Which page has a scrollable region, and its track geometry, in ONE place
    // so drawing, wheel, keys and drag can never disagree about where it is.
    // PG_APPEAR and PG_WALL (#745 dark port) no longer draw a scrollbar: the
    // dark spec shows 3 fixed theme cards and a 5x2 fixed wallpaper grid
    // with a "Browse more..." link, not an inline scrollable list, so there
    // is no track to hit-test - drawing hit geometry for a control that is
    // never drawn would be a ghost UI element. PG_TIME's dark list
    // (dk_list(), #745 follow-up) keeps its scrollbar, now at the dark
    // geometry: list at x=32,y=80,w=576,h=284, so tx=x+w-8=600, ty=y+2=82,
    // tl=h-4=280 (row count/rows-visible unchanged at n=TZ.len()=35,rows=10;
    // dead geometry - see track_of() below, PG_TIME returns None here).
    fn track_of(&self) -> Option<(i32, i32, i32, usize, usize)> {
        // PG_TIME's dark list now has its own dedicated searchable-city-
        // list mechanics (tzc_sel/tzc_first/time_list_move), not this
        // generic scrollbar-drag track - dragging that list's scrollbar is
        // documented as NOT required (search + keyboard arrows are the
        // primary paths, #334). PG_APPEAR never had an entry either.
        None
    }

    // Map a y inside the track to a first-visible index and apply it.
    fn scroll_to_px(&mut self, my: i32) {
        if let Some((_tx, ty, tl, n, rows)) = self.track_of() {
            if n <= rows { return; }
            let mut len = tl * (rows as i32) / (n as i32);
            if len < 24 { len = 24; }
            let span = tl - len;
            if span <= 0 { return; }
            let mut rel = my - ty - len / 2;
            if rel < 0 { rel = 0; }
            if rel > span { rel = span; }
            let first = ((n - rows) as i32) * rel / span;
            self.set_first(first as usize);
        }
    }

    fn set_first(&mut self, f: usize) {
        match self.page {
            PG_TIME   => { self.tz_first = f; if self.tz < f { self.tz = f; }
                           if self.tz >= f + 10 { self.tz = f + 9; } }
            PG_APPEAR => { self.theme_first = f; if self.theme < f { self.theme = f; }
                           if self.theme >= f + 5 { self.theme = f + 4; } }
            PG_WALL   => { self.wall_first = f * WALL_COLS; }
            _ => {}
        }
    }

    fn list_move(&mut self, delta: i32) {
        // PG_WALL only. PG_TIME has its own dedicated time_list_move()
        // (searchable city list); PG_APPEAR's 5x3 theme grid and 5-card
        // dock row are both fully visible with no scrolling at all. This
        // guard is deliberate, not incidental: without it, a page that no
        // longer has a PG_APPEAR arm here would silently fall into the
        // wall-grid default arm, so a stray wheel-scroll on PG_APPEAR (or a
        // stray call from anywhere else) would mutate self.wall instead of
        // doing nothing - a real latent bug, not just an omission.
        if self.page != PG_WALL { return; }
        let n = self.nwalls;
        if n == 0 { return; }
        let rows = WALL_PAGE;
        let mut s = self.wall as i32 + delta;
        if s < 0 { s = 0; }
        if s >= n as i32 { s = n as i32 - 1; }
        self.wall = s as usize;
        if self.wall < self.wall_first { self.wall_first = self.wall; }
        else if self.wall >= self.wall_first + rows { self.wall_first = self.wall + 1 - rows; }
    }

    fn on_key(&mut self, ev: &GuiEvent) -> bool {
        let c = ev.key_char;

        if c == b'\t' {
            // PG_TIME/PG_APPEAR each intercept Tab for their own zone/field
            // cycling BEFORE the generic nfields-based Tab handling below
            // (both pages have nfields == 0, so the generic path would
            // otherwise be a no-op here).
            if self.page == PG_TIME { self.time_tab_next(); return true; }
            if self.page == PG_APPEAR { self.appear_zone = 1 - self.appear_zone; return true; }
            if self.page == PG_AI { self.ai_tab_next(); return true; }
            // #745 task #15: PG_APPSW, four stops (apps grid, widgets list,
            // Back, Continue), wrapping, same shape as PG_SIGNIN below.
            if self.page == PG_APPSW { self.appsw_zone = (self.appsw_zone + 1) % 4; return true; }
            // #745 PG_SIGNIN has four stops - startup group, sign-in-screen
            // group, Back, Continue - and WRAPS at both edges. A four-stop
            // page that dead-ends makes a person Tab blindly to find out
            // whether they are stuck. Shift+Tab is NOT implemented: GuiEvent
            // carries ty/target_id/mouse/scroll/keycode/key_char and no
            // modifier state at all, so this app cannot see the Shift, and a
            // reverse stop that silently goes forwards is worse than none.
            // #745 task #38: stop 1 (sign-in-screen group) is SKIPPED when
            // require_pw is false, because dk_draw_signin does not draw it
            // then - a Tab ring that still visits a hidden stop is the
            // keyboard half of the same "hidden control, live hit-test"
            // defect the on_click guard below fixes for the mouse.
            if self.page == PG_SIGNIN {
                self.signin_focus = (self.signin_focus + 1) % 4;
                if self.signin_focus == 1 && !self.require_pw { self.signin_focus = 2; }
                return true;
            }
            if self.nfields > 0 { self.focus = (self.focus + 1) % self.nfields; }
            return true;
        }
        // #745: Enter activates the control that HOLDS FOCUS. On PG_SIGNIN
        // that can be Back, so it is checked before the wizard-wide
        // Enter-means-Continue below.
        //
        // DEVIATION, stated: the spec also asks for Enter to be a no-op inside
        // a radio group, with F10 as the from-anywhere Continue. F10 is ALREADY
        // BOUND, wizard-wide, to "Set up later" (see below), and taking it for
        // Continue would silently change what F10 does on eight other pages.
        // Making Enter inert inside the groups without it would leave this page
        // with no single-key way forward, on a platform where pointer input is
        // the secondary path (#334). So Enter keeps the meaning it has on every
        // other page of this wizard, except on Back.
        if (c == b'\r' || c == b'\n') && self.page == PG_SIGNIN && self.signin_focus == 2 {
            self.back(); return true;
        }
        // #745 task #15: same Back exception on PG_APPSW's own four-stop
        // ring. Everywhere else on this page Enter ADVANCES rather than
        // toggling (spec 6.3: Space toggles, Enter is "I am done with this
        // screen") - that is already the fallthrough below, so only the
        // Back stop needs a special case.
        if (c == b'\r' || c == b'\n') && self.page == PG_APPSW && self.appsw_zone == 2 {
            self.back(); return true;
        }
        if c == b'\r' || c == b'\n' { return self.next(); }
        if c == 27 && self.page != PG_WELCOME { self.back(); return true; }
        // #745: keyboard equivalent of the "Set up later" link. Checked here,
        // above the per-page dispatch, because PG_TIME and PG_APPEAR consume
        // every key they are given. skip() itself re-checks page_skippable(),
        // so pressing it on Welcome or Create-your-account does nothing at all,
        // which is the same answer the missing link gives.
        if ev.keycode == KC_F10 { return self.skip(); }
        // #198: keyboard equivalents of the power-corner buttons, checked
        // here for the same reason as KC_F10 just above (PG_TIME/PG_APPEAR
        // consume every key). Unlike the mouse path (pwr_win), these are NOT
        // gated on CARD_MODE - pwr_create()'s small-screen fallback hides
        // the drawn row for lack of space, but a keyboard-only path to a
        // working Restart/Shut Down must exist regardless of screen size
        // (#334: mouse is unreliable, keyboard is the fallback that has to
        // always work). F9 (Skip to Desktop) used to be checked alongside
        // these; removed per owner request, 2026-08-28.
        if ev.keycode == KC_F2 { do_reboot(); return true; }
        if ev.keycode == KC_F3 { do_poweroff(); return true; }

        if self.page == PG_TIME { return self.time_on_key(ev); }
        if self.page == PG_APPEAR { return self.appear_on_key(ev); }
        if self.page == PG_AI { return self.ai_on_key(ev); }

        match self.page {
            PG_WALL => {
                // Grid: vertical arrows move a whole row of 5, horizontal by one.
                if ev.keycode == KC_UP { self.list_move(-5); }
                else if ev.keycode == KC_DOWN { self.list_move(5); }
                else if ev.keycode == KC_LEFT || c == b'-' { self.list_move(-1); }
                else if ev.keycode == KC_RIGHT || c == b'+' { self.list_move(1); }
                else { return true; }
                self.wall_apply_live();
                return true;
            }
            PG_SIGNIN => {
                // SELECTION COMMITS ON ARROW. There is no browse-then-confirm
                // state inside a radio pair, which is why each radio has two
                // appearances and the group carries the focus frame.
                //
                // Both axes are accepted because the group is a vertical stack
                // a person may reasonably probe horizontally, and mapping both
                // costs nothing. CLAMPED, never wrapped: on a two-item group,
                // wrapping makes Up and Down indistinguishable.
                let prev = ev.keycode == KC_UP || ev.keycode == KC_LEFT;
                let next = ev.keycode == KC_DOWN || ev.keycode == KC_RIGHT;
                if prev || next {
                    match self.signin_focus {
                        0 => self.require_pw = prev,
                        1 => self.mode_list = prev,
                        _ => {}
                    }
                }
                // Space is a NO-OP confirm, accepted for anyone expecting one;
                // nothing waits for it. It used to TOGGLE require_pw from
                // anywhere on this page, including while the keyboard was on
                // the other group or on a button - a key that silently rewrote
                // a security setting the person was not looking at.
                return true;
            }
            PG_NETWORK if self.nfields == 0 => {
                if c == b' ' {
                    self.dhcp = !self.dhcp; net_test_reset();
                    // #154: switching back to DHCP makes the static-address
                    // fields moot, so any "not a valid address" warning they
                    // raised is no longer a real problem - network_rule()
                    // already no-ops while self.dhcp is true, so this just
                    // clears whatever was showing.
                    self.live_recheck();
                }
                return true;
            }
            // #745 task #15: PG_APPSW. Arrows move the cursor WITHIN the
            // focused zone, clamped (never wrapped, matching PG_WALL's own
            // "no escape from the grid" rule above); Space TOGGLES the
            // cursor cell/row. Enter never toggles here - see the Back
            // exception above and spec 6.3.
            PG_APPSW => {
                match self.appsw_zone {
                    0 => {
                        let mut col = (self.apps_focus % 6) as i32;
                        let mut row = (self.apps_focus / 6) as i32;
                        if ev.keycode == KC_LEFT { if col > 0 { col -= 1; } }
                        else if ev.keycode == KC_RIGHT { if col < 5 { col += 1; } }
                        else if ev.keycode == KC_UP { if row > 0 { row -= 1; } }
                        else if ev.keycode == KC_DOWN { if row < 1 { row += 1; } }
                        else if c == b' ' { self.apps_sel ^= 1u16 << self.apps_focus; }
                        self.apps_focus = (row * 6 + col) as usize;
                    }
                    1 => {
                        let mut col = (self.widgets_cursor / 5) as i32;
                        let mut row = (self.widgets_cursor % 5) as i32;
                        if ev.keycode == KC_LEFT { if col > 0 { col -= 1; } }
                        else if ev.keycode == KC_RIGHT { if col < 2 { col += 1; } }
                        else if ev.keycode == KC_UP { if row > 0 { row -= 1; } }
                        else if ev.keycode == KC_DOWN { if row < 4 { row += 1; } }
                        else if c == b' ' {
                            self.widgets_sel ^= 1u16 << self.widgets_cursor;
                            self.appsw_widgets_touched = true;
                        }
                        self.widgets_cursor = (col * 5 + row) as usize;
                    }
                    _ => {}
                }
                return true;
            }
            _ => {}
        }
        if self.nfields == 0 { return true; }
        let f = self.focus;
        let fld = self.field_mut(f);
        if c == 8 || c == 127 { fld.pop(); }
        else if c >= 32 && c < 127 { fld.push(c); }
        // #154: an edit to ANY text field on this page can resolve (or
        // change) whatever validation message is currently on screen -
        // re-derive it from current state instead of leaving it latched
        // from whenever it was first shown.
        self.live_recheck();
        true
    }

    fn on_click(&mut self, mx: i32, my: i32) -> bool {
        if self.page == PG_WELCOME {
            // One control, one action. The second rect here was the "Set up
            // later" link, and it called the very same self.next() - see
            // draw_welcome() and page_skippable() for why it is gone rather
            // than fixed in place.
            let (bx, by, bw, bh) = self.wel_btn;
            if mx >= bx && mx < bx + bw && my >= by && my < by + bh { return self.next(); }
            return true;
        }
        // Scrollbar first: the thumb was drawn with correct proportional
        // geometry and never hit-tested, so it could not be dragged. Clicking
        // anywhere in the track jumps there and begins a drag.
        if let Some((tx, ty, tl, n, rows)) = self.track_of() {
            if n > rows && mx >= tx - 2 && mx <= tx + 8 && my >= ty && my <= ty + tl {
                self.dragging = true;
                self.scroll_to_px(my);
                return true;
            }
        }
        let (bx0, bx1, px0, px1, fy0, fy1) = footer_bounds(self.page);
        if my >= fy0 && my <= fy1 {
            if mx >= px0 && mx <= px1 { return self.next(); }
            if mx >= bx0 && mx <= bx1 { self.back(); return true; }
            // #745: the skip link, hit-tested from the SAME predicate and the
            // SAME geometry the chrome drew it with, so there is no page where
            // one exists without the other.
            if page_skippable(self.page) {
                let (lx, ly, lw, lh) = skip_link_bounds();
                if mx >= lx && mx < lx + lw && my >= ly && my < ly + lh { return self.skip(); }
            }
        }
        match self.page {
            // Dark geometry (#745 port, PG_TIME included as of its #745
            // follow-up dark port) below for every page.
            PG_SIGNIN => {
                // Full-ROW targets, x 32..608, from each label's top to its
                // sub-line's bottom (30px), not the 13px circle. Clicking also
                // moves the group focus there, so the keyboard picks up where
                // the pointer left off.
                //
                // WHAT THIS REPLACES: rows at y 206..230 and 230..254, which
                // are the geometry of a radio pair this page has not drawn
                // since the preview card arrived. They still fired - a click in
                // the middle of that card silently rewrote the startup mode with
                // nothing on screen changing to say so.
                // #745 task #38: the 212..242/250..280 rows are guarded by
                // self.require_pw too, because dk_draw_signin stops drawing
                // this whole group under autologin - see that function's
                // else branch. Without the guard this would be exactly the
                // "hidden control with a live hit-test" bug the comment
                // above already names once on this page, just moved from a
                // preview card to autologin.
                if mx >= 32 && mx < 608 {
                    if my >= 100 && my < 130      { self.require_pw = true;  self.signin_focus = 0; }
                    else if my >= 138 && my < 168 { self.require_pw = false; self.signin_focus = 0; }
                    else if self.require_pw && my >= 212 && my < 242 { self.mode_list = true;   self.signin_focus = 1; }
                    else if self.require_pw && my >= 250 && my < 280 { self.mode_list = false;  self.signin_focus = 1; }
                }
            }
            PG_NETWORK => {
                if my >= 76 && my < 93 && mx >= 572 && mx < 606 {
                    self.dhcp = !self.dhcp; net_test_reset();
                    self.live_recheck();   // #154: see the keyboard toggle above
                }
                else if self.dhcp && my >= NET_BTN.1 && my < NET_BTN.1 + NET_BTN.3
                        && mx >= NET_BTN.0 && mx < NET_BTN.0 + NET_BTN.2 { self.net_retry(); }
                else if !self.dhcp {
                    if my >= 126 && my < 154 { self.focus = if mx < 320 { 0 } else { 1 }; }
                    else if my >= 176 && my < 204 { self.focus = if mx < 320 { 2 } else { 3 }; }
                }
            }
            PG_ACCOUNT => {
                if my >= 90 && my < 118 { self.focus = if mx < 320 { 0 } else { 1 }; }
                else if my >= 146 && my < 174 { self.focus = if mx < 320 { 2 } else { 3 }; }
                // #745 root password row, at the geometry dk_draw_account draws.
                else if my >= 228 && my < 256 { self.focus = if mx < 320 { 4 } else { 5 }; }
            }
            PG_AI => {
                let is_custom = self.ai_provider == AI_CUSTOM;
                if my >= 84 && my < 172 && mx >= 32 && mx < 608
                   && (mx - 32) % 146 < 138 && (my - 84) % 48 < 40 {
                    let col = (mx - 32) / 146;
                    let row = (my - 84) / 48;
                    if col < 4 && row < 2 {
                        let idx = (row * 4 + col) as usize;
                        self.select_ai_provider(idx);
                        self.ai_focus = 0;
                    }
                } else if is_custom && my >= 194 && my < 220 {
                    self.ai_focus = 1;
                } else {
                    let shift = if is_custom { 28 } else { 0 };
                    let model_y = 214 + shift;
                    let key_y = 280 + shift;
                    if my >= model_y && my < model_y + 28 { self.ai_focus = 2; }
                    else if my >= key_y && my < key_y + 30 { self.ai_focus = 3; }
                }
            }
            PG_TIME => {
                if my >= 92 && my < 114 && mx >= 408 && mx < 600 {
                    self.time_focus = 0;
                } else if my >= 118 && my < 118 + 5 * 18 && mx >= 408 && mx < 600 {
                    let row = ((my - 118) / 18) as usize;
                    unsafe {
                        let idx = self.tzc_first + row;
                        if idx < TZC_FILT_COUNT { self.tzc_sel = TZC_FILT[idx] as usize; self.time_focus = 1; }
                    }
                    self.time_sync_tz();
                } else if my >= 252 && my < 269 && mx >= 572 && mx < 606 {
                    self.ntp_on = !self.ntp_on;
                    self.time_focus = if self.ntp_on { 9 } else { 3 };
                } else if !self.ntp_on && my >= 294 && my < 320 {
                    const SPIN_BOXES: [(i32, i32, usize); 6] =
                        [(32, 56, 3), (96, 40, 4), (144, 40, 5), (220, 40, 6), (268, 40, 7), (316, 40, 8)];
                    let mut i = 0usize;
                    while i < 6 {
                        let (sx, sw, focus_idx) = SPIN_BOXES[i];
                        if mx >= sx && mx < sx + sw {
                            self.time_focus = focus_idx;
                            if mx >= sx + sw - 9 {
                                if my < 294 + 13 { self.time_spin_step(focus_idx, 1); }
                                else { self.time_spin_step(focus_idx, -1); }
                            }
                            break;
                        }
                        i += 1;
                    }
                } else if my >= 364 && my < 388 && mx >= 210 && mx < 430 {
                    self.time_focus = 9;
                    self.ntp_cycle(1);
                } else if my >= 86 && my < 224 && mx >= 32 && mx < 384 {
                    self.time_map_click(mx, my);
                }
            }
            PG_APPEAR => {
                // #745: every bound below is DERIVED from the same THG_*/DOCK_*
                // constants the draw uses, so a layout change cannot leave the
                // hit test pointing at where the cards used to be.
                let g_bot = THG_Y + THG_ROWS as i32 * THG_PITCH_Y;
                let g_right = THG_X + THG_COLS as i32 * THG_PITCH_X;
                let d_bot = DOCK_Y + DOCK_H;
                let d_right = DOCK_X_0 + 5 * DOCK_PITCH_X;
                if my >= THG_Y && my < g_bot && mx >= THG_X && mx < g_right {
                    let col = (mx - THG_X) / THG_PITCH_X;
                    let row = (my - THG_Y) / THG_PITCH_Y;
                    if col < THG_COLS as i32 && row < THG_ROWS as i32 {
                        let idx = (row * THG_COLS as i32 + col) as usize;
                        if idx < self.nthemes { self.theme = idx; self.appear_zone = 0; }
                    }
                } else if my >= DOCK_Y && my < d_bot && mx >= DOCK_X_0 && mx < d_right {
                    let col = (mx - DOCK_X_0) / DOCK_PITCH_X;
                    if col < 5 { self.dock_style = col as usize; self.appear_zone = 1; }
                }
            }
            PG_WALL => {
                // #745 task #38: grid is now WALL_COLS x WALL_ROWS (5x4/20).
                // The "Browse more..." link and its hit-test are REMOVED
                // along with the draw call and wall_page_next() itself, so
                // this arm no longer has an else-if branch.
                if my >= WALL_Y0 && my < WALL_Y0 + WALL_ROWS as i32 * WALL_PITCH_Y
                   && mx >= WALL_X0 && mx < WALL_X0 + WALL_COLS as i32 * WALL_PITCH_X {
                    let col = (mx - WALL_X0) / WALL_PITCH_X;
                    let row = (my - WALL_Y0) / WALL_PITCH_Y;
                    if col < WALL_COLS as i32 && row < WALL_ROWS as i32 {
                        let idx = self.wall_first + (row as usize) * WALL_COLS + (col as usize);
                        if idx < self.nwalls && idx != self.wall {
                            self.wall = idx;
                            self.wall_apply_live();
                        }
                    }
                }
            }
            // #745 task #15: PG_APPSW. Bounds use the same APP_*/WID_* pitch
            // constants dk_draw_appsw() draws from (like PG_APPEAR above), so
            // a geometry change here cannot leave the hit test pointing at
            // cells the draw call no longer puts there. Uses the full pitch
            // band, not the exact cell width, matching PG_WALL's own
            // established precedent (a click in the inter-cell gutter still
            // resolves to the nearer cell rather than doing nothing).
            PG_APPSW => {
                let apps_bot = APP_Y0 + 2 * APP_PITCH_Y;
                let apps_right = APP_X0 + 6 * APP_PITCH_X;
                let wid_bot = WID_Y0 + 5 * WID_PITCH_Y;
                let wid_right = WID_X0 + 3 * WID_PITCH_X;
                if my >= APP_Y0 && my < apps_bot && mx >= APP_X0 && mx < apps_right {
                    let col = (mx - APP_X0) / APP_PITCH_X;
                    let row = (my - APP_Y0) / APP_PITCH_Y;
                    let idx = (row * 6 + col) as usize;
                    if idx < APPS_UI.len() {
                        self.apps_focus = idx;
                        self.apps_sel ^= 1u16 << idx;
                        self.appsw_zone = 0;
                    }
                } else if my >= WID_Y0 && my < wid_bot && mx >= WID_X0 && mx < wid_right {
                    let col = (mx - WID_X0) / WID_PITCH_X;
                    let row = (my - WID_Y0) / WID_PITCH_Y;
                    let idx = (col * 5 + row) as usize;
                    if idx < WIDGETS_UI.len() {
                        self.widgets_cursor = idx;
                        self.widgets_sel ^= 1u16 << idx;
                        self.appsw_widgets_touched = true;
                        self.appsw_zone = 1;
                    }
                }
            }
            _ => {}
        }
        true
    }
}

// ---------------------------------------------------------------------------
// PG_TIME / PG_APPEAR interaction (world-map city picker, manual/NTP clock,
// theme grid, dock-style row). One-phase selection model throughout (arrow
// keys move the selection immediately, no separate Enter-to-commit step) -
// deliberately simplified from the spec's two-phase "browsing highlight vs
// committed" model, matching the SAME one-phase model this app's OTHER lists
// already use (list_move()/PG_WALL) rather than introducing a second
// interaction language into one app. Documented as a simplification in the
// port report, not a silently dropped feature.
// ---------------------------------------------------------------------------
impl App {
    // Builds "Selected: <name>, <country> - <tz_id> (UTC<+HH:MM>[, DST +1])"
    // into `buf`, NUL-terminated. Falls back to a plain message if the city
    // table failed to load (TZCITIES.DAT missing/bad) - the map/list degrade
    // but the page still renders something coherent.
    fn time_readout(&self, buf: &mut [u8; 160]) {
        let mut n = 0usize;
        unsafe {
            if TZC_COUNT == 0 || self.tzc_sel >= TZC_COUNT {
                n += put(buf, n, b"No city data available. Time zone: ");
                n += put(buf, n, str_trim(tz_id_bytes(self.tz)));
            } else {
                let c = &TZC[self.tzc_sel];
                n += put(buf, n, b"Selected: ");
                n += put(buf, n, str_trim(&c.name));
                n += put(buf, n, b", ");
                n += put(buf, n, str_trim(&c.country));
                n += put(buf, n, b" - ");
                n += put(buf, n, str_trim(&c.tz_id));
                n += put(buf, n, b" (UTC");
                n += fmt_utc_offset(buf, n, c.utc_off_min as i32);
                if c.dst != 0 { n += put(buf, n, b", DST +1"); }
                n += put(buf, n, b")");
            }
        }
        buf[n.min(159)] = 0;
    }

    // Sync the existing `self.tz` (still what apply()'s TZ.CFG writer reads)
    // to whichever of the 35 shipping offsets (26 original + 9 appended by
    // #745, 2026-08-12) the currently-selected city uses. A matching entry
    // always exists (TZCITIES_README.md: "constrained to exactly that
    // offset set"); tz_index_for_offset()'s fallback of 12 (UTC+00:00) only
    // matters if TZC data is missing or corrupt.
    fn time_sync_tz(&mut self) {
        unsafe {
            if self.tzc_sel < TZC_COUNT {
                self.tz = tz_index_for_offset(TZC[self.tzc_sel].utc_off_min as i32);
            }
        }
    }

    // Tab order: search -> list -> ntp toggle -> [Year..Second, skipped
    // whenever NTP is on] -> ntp combo -> (wraps to search). Reverse-Tab
    // (Shift+Tab) is NOT implemented: GuiEvent (see its definition above)
    // carries no modifier-key bit, so this app cannot detect Shift at all -
    // verified by reading the struct, not assumed.
    fn time_tab_next(&mut self) {
        let mut f = self.time_focus;
        loop {
            f += 1;
            if f > 9 { f = 0; }
            if self.ntp_on && f >= 3 && f <= 8 { continue; }
            break;
        }
        self.time_focus = f;
    }

    // Rebuilds the filtered city list from the search field's current text
    // and jumps the selection to the first match. An empty filtered result
    // leaves tzc_sel wherever it was (never crashes, never picks garbage).
    fn time_refilter(&mut self) {
        tz_filter_refresh(&self.tz_search);
        self.tzc_first = 0;
        unsafe {
            if TZC_FILT_COUNT > 0 {
                self.tzc_sel = TZC_FILT[0] as usize;
                self.time_sync_tz();
            }
        }
    }

    // Up/Down within the FILTERED set, auto-scrolling the 5-row viewport.
    fn time_list_move(&mut self, delta: i32) {
        unsafe {
            if TZC_FILT_COUNT == 0 { return; }
            let mut pos: i32 = -1;
            let mut i = 0usize;
            while i < TZC_FILT_COUNT { if TZC_FILT[i] as usize == self.tzc_sel { pos = i as i32; break; } i += 1; }
            if pos < 0 { pos = 0; }
            pos += delta;
            if pos < 0 { pos = 0; }
            if pos >= TZC_FILT_COUNT as i32 { pos = TZC_FILT_COUNT as i32 - 1; }
            self.tzc_sel = TZC_FILT[pos as usize] as usize;
            let p = pos as usize;
            if p < self.tzc_first { self.tzc_first = p; }
            else if p >= self.tzc_first + 5 { self.tzc_first = p + 1 - 5; }
        }
        self.time_sync_tz();
    }

    // Secondary path (mouse is unreliable on this platform, #334; the list
    // is primary): nearest-city linear scan by squared pixel distance to the
    // click point, exactly one source of truth (TZC's lat/lon), same as the
    // README's projection note describes for a click-to-timezone lookup.
    fn time_map_click(&mut self, mx: i32, my: i32) {
        unsafe {
            if TZC_COUNT == 0 { return; }
            let mut best_i = 0usize;
            let mut best_d = i32::MAX;
            let mut i = 0usize;
            while i < TZC_COUNT {
                let (px, py) = tz_marker_xy(TZC[i].lat_e2 as i32, TZC[i].lon_e2 as i32);
                let dx = px - mx; let dy = py - my;
                let d = dx * dx + dy * dy;
                if d < best_d { best_d = d; best_i = i; }
                i += 1;
            }
            self.tzc_sel = best_i;
        }
        self.time_focus = 1;
        self.time_sync_tz();
    }

    fn ntp_cycle(&mut self, delta: i32) {
        self.ntp_preset = wrap_range(self.ntp_preset as i32 + delta, 0, 2) as usize;
        self.ntp_server = Field::new(false);
        let s = NTP_PRESETS[self.ntp_preset];
        let mut i = 0usize;
        while i < s.len() && s[i] != 0 { self.ntp_server.push(s[i]); i += 1; }
    }

    // Up/Down step; Year clamps (1970-2099), Month/Day/Hour wrap, Minute/
    // Second wrap AND cascade into the next unit on overflow. Hour does NOT
    // cascade into the date fields (deliberate v1 scope limit, per spec).
    fn time_spin_step(&mut self, focus: usize, delta: i32) {
        match focus {
            3 => { self.dt_year = clampi(self.dt_year + delta, 1970, 2099); }
            4 => {
                self.dt_month = wrap_range(self.dt_month + delta, 1, 12);
                let dim = days_in_month(self.dt_month, self.dt_year);
                if self.dt_day > dim { self.dt_day = dim; }
            }
            5 => {
                let dim = days_in_month(self.dt_month, self.dt_year);
                self.dt_day = wrap_range(self.dt_day + delta, 1, dim);
            }
            6 => { self.dt_hour = wrap_range(self.dt_hour + delta, 0, 23); }
            7 => {
                let mut m = self.dt_min + delta;
                let mut hd = 0;
                if m > 59 { m -= 60; hd = 1; }
                if m < 0 { m += 60; hd = -1; }
                self.dt_min = m;
                if hd != 0 { self.dt_hour = wrap_range(self.dt_hour + hd, 0, 23); }
            }
            _ => {
                let mut s = self.dt_sec + delta;
                let mut md = 0;
                if s > 59 { s -= 60; md = 1; }
                if s < 0 { s += 60; md = -1; }
                self.dt_sec = s;
                if md != 0 {
                    let mut m = self.dt_min + md;
                    let mut hd = 0;
                    if m > 59 { m -= 60; hd = 1; }
                    if m < 0 { m += 60; hd = -1; }
                    self.dt_min = m;
                    if hd != 0 { self.dt_hour = wrap_range(self.dt_hour + hd, 0, 23); }
                }
            }
        }
    }

    // Digit typed directly into the focused spinner: simplified two-digit
    // rolling entry (value = (value % 10) * 10 + digit, then CLAMPED, not
    // wrapped) - exact caret-position text editing is out of scope for a
    // fixed-width numeric spinner.
    fn time_spin_digit(&mut self, focus: usize, d: i32) {
        match focus {
            3 => { self.dt_year = clampi((self.dt_year % 1000) * 10 + d, 1970, 2099); }
            4 => {
                self.dt_month = clampi((self.dt_month % 10) * 10 + d, 1, 12);
                let dim = days_in_month(self.dt_month, self.dt_year);
                if self.dt_day > dim { self.dt_day = dim; }
            }
            5 => {
                let dim = days_in_month(self.dt_month, self.dt_year);
                self.dt_day = clampi((self.dt_day % 10) * 10 + d, 1, dim);
            }
            6 => { self.dt_hour = clampi((self.dt_hour % 10) * 10 + d, 0, 23); }
            7 => { self.dt_min = clampi((self.dt_min % 10) * 10 + d, 0, 59); }
            _ => { self.dt_sec = clampi((self.dt_sec % 10) * 10 + d, 0, 59); }
        }
    }

    fn time_spin_key(&mut self, ev: &GuiEvent) {
        let c = ev.key_char;
        if ev.keycode == KC_UP { self.time_spin_step(self.time_focus, 1); }
        else if ev.keycode == KC_DOWN { self.time_spin_step(self.time_focus, -1); }
        else if c >= b'0' && c <= b'9' { self.time_spin_digit(self.time_focus, (c - b'0') as i32); }
    }

    fn time_on_key(&mut self, ev: &GuiEvent) -> bool {
        let c = ev.key_char;
        match self.time_focus {
            0 => {
                if c == 8 || c == 127 { self.tz_search.pop(); self.time_refilter(); }
                else if c >= 32 && c < 127 { self.tz_search.push(c); self.time_refilter(); }
            }
            1 => {
                if ev.keycode == KC_UP { self.time_list_move(-1); }
                else if ev.keycode == KC_DOWN { self.time_list_move(1); }
            }
            2 => {
                if c == b' ' {
                    self.ntp_on = !self.ntp_on;
                    // Exact jump behaviour from the spec's keyboard section:
                    // toggling ON moves focus to the combo, toggling OFF
                    // returns it to Year.
                    self.time_focus = if self.ntp_on { 9 } else { 3 };
                }
            }
            3..=8 => { if !self.ntp_on { self.time_spin_key(ev); } }
            _ => {
                if ev.keycode == KC_UP { self.ntp_cycle(1); }
                else if ev.keycode == KC_DOWN { self.ntp_cycle(-1); }
                else if c == 8 || c == 127 { self.ntp_server.pop(); }
                else if c >= 32 && c < 127 { self.ntp_server.push(c); }
            }
        }
        true
    }

    // Arrow keys move focus across the 5x3 theme grid / the 5-card dock
    // row; moving past the grid's own edge does nothing (no wrap), so Tab
    // is always the only way out (spec section 5). #745: the column count is
    // THG_COLS, shared with the draw and the hit test, not a literal 7 in a
    // third place.
    fn appear_on_key(&mut self, ev: &GuiEvent) -> bool {
        if self.appear_zone == 0 {
            if self.nthemes == 0 { return true; }
            let c = THG_COLS;
            let col = self.theme % c;
            let row = self.theme / c;
            let max_row = (self.nthemes - 1) / c;
            if ev.keycode == KC_LEFT { if col > 0 { self.theme -= 1; } }
            else if ev.keycode == KC_RIGHT { if col + 1 < c && self.theme + 1 < self.nthemes { self.theme += 1; } }
            else if ev.keycode == KC_UP { if row > 0 { self.theme -= c; } }
            else if ev.keycode == KC_DOWN {
                if row < max_row && self.theme + c < self.nthemes { self.theme += c; }
            }
        } else {
            if ev.keycode == KC_LEFT { if self.dock_style > 0 { self.dock_style -= 1; } }
            else if ev.keycode == KC_RIGHT { if self.dock_style < 4 { self.dock_style += 1; } }
        }
        true
    }
}

// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn main() -> i32 {
    // Single window for the entire wizard (fixes the "full screen then back
    // to desktop" flash: there is no second window creation left to cause
    // it - see draw_welcome()'s block comment). Content size is exactly
    // W x H (640x480): a NOCHROME window has no chrome to allow for (see
    // window_get_content_bounds() in kernel/gui/window.c - it returns the
    // full win->bounds once WINDOW_FLAG_NOCHROME is set), so unlike a
    // chromed window, passing an OUTER size bigger than W,H here would leave
    // a dead margin instead of filling it; W,H already IS the outer size for
    // a borderless window.
    // -----------------------------------------------------------------------
    // #126: WHICH WIZARD IS THIS? Decided here, from the two markers on disk,
    // before a single pixel is drawn, and never revisited.
    //
    // The compositor already gates the spawn on the same two facts, so this is
    // in principle a second reader of the same state. It is deliberate: the
    // wizard is also launchable by hand and by an autorun line, and a wizard
    // that decided its own scope from an argument it was not given would
    // default to "create an account on a machine that already has one". The
    // markers are the contract; both ends read the contract.
    let machine_done = unsafe {
        let fd = syscall3(SYS_OPEN, b"/CONFIG/SETUPDONE\0".as_ptr() as i64, 0, 0) as i32;
        if fd >= 0 { syscall1(SYS_CLOSE, fd as i64); true } else { false }
    };
    // NO LEGACY FALLBACK for this one. userconf_open_read()'s second argument
    // is the pre-#683 absolute path to fall back to, and passing NULL is how
    // that file spells "there is no legacy location" (it returns -1 rather than
    // opening anything). A fallback here would make root's presence/absence
    // answer the question for every other user.
    let user_done = unsafe {
        let fd = userconf_open_read(b"SETUPUSR\0".as_ptr(), core::ptr::null());
        if fd >= 0 { syscall1(SYS_CLOSE, fd as i64); true } else { false }
    };
    unsafe { PERSONALISE = machine_done; }
    // #OOBEAUTH: and WHICH STEPS may this session perform? Decided here,
    // once, before a pixel is drawn, from the two facts that can make it
    // true: real root (geteuid() == 0, for any future caller that IS a
    // signed-in root session), OR the kernel's own answer to "would my
    // SYS_FIRSTBOOT_ADMIN call succeed right now" (FR_BOOTSTRAP_QUERY, true
    // only for the narrow first-boot bootstrap session - see the long
    // comment on MACHINE_ADMIN above and firstboot_bootstrap_ok_rs() in
    // kernel/rustkern/firstrun.rs for exactly what that requires).
    //
    // geteuid(), not getuid(), for the first half: every gate downstream
    // (sys_firstboot_admin's `p->euid != 0`, login_cfg_authorize's
    // `p->euid == 0`) reads the EFFECTIVE uid, and asking a different
    // question than the one that will be answered is how a wizard offers a
    // step that then fails.
    unsafe {
        MACHINE_ADMIN = syscall0(SYS_GETEUID) == 0
            || syscall1(SYS_FIRSTRUN, FR_BOOTSTRAP_QUERY) == 1;
    }
    // Nothing to ask. Exit rather than draw an empty wizard: this is the state
    // every login after the first one is in, so it must cost nothing.
    if machine_done && user_done { return 0; }

    let (sw, sh) = real_screen_size();
    unsafe { FB_W = sw; FB_H = sh; }

    // #745 glass card. The window IS the card: 688x616, fixed, centred. The
    // compositor draws the real wallpaper around it at full sharpness, which is
    // the whole point and is something the wizard cannot do for itself (see the
    // GLASS CARD SHELL block comment for the measurement that decided it).
    //
    // Small-screen fallback: below 736x664 there is no room for the card, so
    // revert to the legacy 640x480 centred window with the opaque gradient
    // rather than reflowing into a layout nobody has designed or measured.
    let card = sw >= GLASS_MIN_W && sh >= GLASS_MIN_H;
    let (wx, wy, ww, wh);
    if card {
        wx = (sw - CARD_W) / 2;
        wy = (sh - CARD_H) / 2;
        ww = CARD_W; wh = CARD_H;
        unsafe { CARD_MODE = true; CARD_X = wx; CARD_Y = wy; ORG_X = CARD_PAD; ORG_Y = CARD_PAD + HDR_H; }
    } else {
        wx = if sw > W { (sw - W) / 2 } else { 0 };
        wy = if sh > H { (sh - H) / 2 } else { 0 };
        ww = W; wh = H;
        unsafe { CARD_MODE = false; CARD_X = wx; CARD_Y = wy; ORG_X = 0; ORG_Y = 0; }
    }
    let win = win_create(b"Setup Assistant\0", wx, wy, ww, wh);
    if win < 0 { return 1; }
    // Set NOCHROME immediately, before the first draw: it is a ONE-WAY flag
    // (kernel/proc/syscall.c sys_win_set_nochrome() only ever does
    // `win->flags |= WINDOW_FLAG_NOCHROME` - there is no path anywhere in
    // the kernel that clears it, verified by reading window.c/syscall.c,
    // not assumed), and it reallocates the content buffer to the new
    // (full-window) size, which the very first draw_welcome() call depends
    // on already being correct.
    win_set_nochrome(win);
    // #745: ask the COMPOSITOR for the spec's drop shadow. The GLASS CARD SHELL
    // comment above records that the shadow was dropped because "it cannot be
    // drawn outside a window" - true of this app, which owns no pixel outside
    // its own rectangle and has no way to read one. It is not true of the
    // compositor, which paints the wallpaper the shadow lands on. So the trade
    // stated there (a real wallpaper everywhere against a decoration nowhere)
    // no longer has to be made: the window stays exactly the card, and the
    // layer that already owns the surrounding pixels darkens them. Opt-in, so
    // #189's "no app-window drop shadows" is untouched for every other window.
    // Only in card mode: the 640x480 small-screen fallback is the legacy opaque
    // panel, which the spec's shadow was never designed against.
    if card { win_set_shadow(win); }

    // #198: the power corner's own window - a second window this SAME
    // process owns (docs/WIZARD_POWER_CORNER.html section 1). pwr_create()
    // itself gates on CARD_MODE (already set above).
    pwr_create();

    unsafe { gui_set_style(GUI_STYLE_MODERN); }

    let mut app = App {
        // Starts at PG_WELCOME (page 0), same window as every later page.
        // back()/show_back are floored at PG_WELCOME (see both above), so
        // Welcome itself has no Back and Account (page 1) Back returns here.
        win, p: &LIGHT, page: PG_WELCOME, focus: 0, nfields: 0, hover_nav: 0, err_focus: -1,
        dragging: false,
        err: b"\0", require_pw: true, mode_list: true, signin_focus: 0, dhcp: true,
        tz: tz_utc_idx(), tz_first: 8, theme: 0, theme_first: 0, wall: 0, wall_first: 0,
        fullname: Field::new(false), username: Field::new(false),
        pw: Field::new(true), pw2: Field::new(true),
        rootpw: Field::new(true), rootpw2: Field::new(true),
        ip: Field::new(false), mask: Field::new(false),
        gw: Field::new(false), dns: Field::new(false), aikey: Field::new(true),
        ai_provider: AI_DEFAULT_PROVIDER, ai_model: Field::new(false),
        ai_endpoint: Field::new(false), ai_model_touched: false, ai_focus: 0,
        ai_key_saved: false, ai_checked_saved: false,
        nthemes: 0,
        nwalls: 0,
        substep: 0, apply_err: b"\0",
        wel_btn: (0, 0, 0, 0), clock_skip: false, pers_skip_from: usize::MAX,
        tzc_sel: 0, tzc_first: 0, tz_search: Field::new(false),
        time_focus: 0, ntp_on: false, ntp_server: Field::new(false), ntp_preset: 0,
        dt_year: 2026, dt_month: 1, dt_day: 1, dt_hour: 0, dt_min: 0, dt_sec: 0,
        dock_style: DOCK_XFCE_IDX, appear_zone: 0,
        // #745 task #15: apps_sel seeded to the compiled-in default here
        // (spec 6.1: "the defaults... unless the person has already visited
        // the page and gone Back", which just means never resetting it again
        // after this one seed; APPS_DEFAULT_MASK held five bits when that
        // spec text was written and now holds all twelve, 2026-08-18 owner
        // decision - the "never reset it again" rule is unaffected by how
        // many bits the mask sets). widgets_sel is 0 here and overwritten
        // below from the live profile, once, before the first draw - never
        // re-read on a later visit, matching apps_sel. That live-profile read
        // is also how the six-widget owner default actually reaches this
        // page on a fresh install: WIDGETS_DEFAULT_MASK only fires as
        // read_widgets_from_profile()'s fallback when /UIPROFIL.YML cannot
        // be read at all, so the widgets that must default on were made to
        // match the compositor's OWN compiled-in defaults (clock.c,
        // widgets.c) instead - see CHANGELOG 2026-08-18.
        apps_sel: APPS_DEFAULT_MASK, apps_focus: 0,
        widgets_sel: 0, widgets_cursor: 0,
        appsw_zone: 0, appsw_widgets_touched: false,
    };

    // (moved below: the palette depends on the active theme resolved above)

    app.ai_model.set(AI_PROVIDERS[AI_DEFAULT_PROVIDER].model_default);

    app.nthemes = unsafe { gui_theme_list(core::ptr::addr_of_mut!(THEMES) as *mut ThemeEntry, 32) }.max(0) as usize;

    // Palette follows the machine's ACTIVE theme rather than a hardcoded light,
    // so the wizard does not look foreign on a dark install. The spec leaves
    // this to the implementer; deriving it from state that already exists beats
    // inventing a boot flag for it. Selecting a theme on the Appearance page
    // deliberately does NOT live-switch this: the choice is applied at Applying.
    {
        let mut slug: [u8; 32] = [0; 32];
        unsafe { gui_theme_get_active_slug(slug.as_mut_ptr(), 32); }
        let mut i = 0;
        while i < app.nthemes {
            let t = unsafe { &THEMES[i] };
            let mut same = true; let mut k = 0;
            while k < 32 { if t.slug[k] != slug[k] { same = false; break; } if slug[k] == 0 { break; } k += 1; }
            if same && slug[0] != 0 { if t.is_dark != 0 { app.p = &DARK; } break; }
            i += 1;
        }
        // (#wizdock) slug[0] == 0 means no THEME.CFG exists yet - a genuinely
        // unconfigured machine, the ONLY case this may touch (an existing
        // choice, found above, is left exactly as read). Default the
        // Appearance page's pre-selected theme to Fluent Dark by NAME, since
        // its THEMES[] index depends on what is on /THEMES this build (see
        // default_theme_idx()'s own comment).
        if slug[0] == 0 {
            app.theme = default_theme_idx(unsafe { &*core::ptr::addr_of!(THEMES) }, app.nthemes);
            if app.theme < app.nthemes && unsafe { &THEMES[app.theme] }.is_dark != 0 {
                app.p = &DARK;
            }
        }
    }
    app.nwalls  = unsafe { wp_enumerate(core::ptr::addr_of_mut!(WALLS) as *mut WpEntry, 96) }.max(0) as usize;
    // One-time asset/face loads, never on the draw path. Each fails safe: a
    // missing bold face falls back to face 0, a missing mask skips the mark, and
    // a missing/undecodable wallpaper thumbnail (or the built-in gradient, which
    // has no file behind it at all) leaves GLASS_OK false and the card renders
    // over the analytic gradient instead.
    unsafe { NWALLS_G = app.nwalls; }
    resolve_bold_face();
    logo_load();
    glass_build();

    // #745 task #15: seed the widgets grid from the LIVE profile, once, at
    // wizard start - never pushed, only synced (blame.md, 2026-08-10: "an
    // app's startup must SYNC FROM live state, never PUSH INTO it"). Read
    // here rather than lazily on first page entry so App::new()'s struct
    // literal above can stay a plain zero and this is the one place that
    // ever assigns widgets_sel from disk.
    app.widgets_sel = read_widgets_from_profile();

    {
        let q = app.p;
        let gp = GuiPalette {
            surface: q.bg, surface_raised: q.surface, ink: q.text,
            ink_dim: q.muted, accent: q.accent, accent_hover: q.accent_h,
            border: q.border, field_bg: q.field, field_border: q.border2,
            track: q.track,
        };
        unsafe { gui_set_palette(&gp); }
    }

    // PG_TIME asset load, once at wizard start (spec section 4: "the
    // wizard checks for the asset once at wizard-start"), never on the
    // draw path. Both fail safe: a missing/bad file just leaves
    // TZC_COUNT=0 / MAP_OK false and the page degrades per the documented
    // fallback.
    tzcities_load();
    map_load();
    tz_filter_refresh(&app.tz_search);
    unsafe {
        if TZC_COUNT > 0 {
            // Seed tzc_sel to a city matching the wizard's existing
            // default tz (index 12 = UTC+00:00) so the map/list/self.tz
            // all agree from the very first paint, not just after a
            // person interacts.
            let target_off = tz_offset_min_at(app.tz as i32);
            let mut i = 0usize; let mut found = 0usize;
            while i < TZC_COUNT { if TZC[i].utc_off_min as i32 == target_off { found = i; break; } i += 1; }
            app.tzc_sel = found;
        }
    }
    {
        // Default NTP server text is the first preset, shown even before
        // NTP is switched on, so the combo never renders empty.
        let s = NTP_PRESETS[0];
        let mut i = 0usize; while i < s.len() && s[i] != 0 { app.ntp_server.push(s[i]); i += 1; }
    }
    {
        // Seed the manual date/time spinners from the real RTC so a
        // person who opens the page starts editing from "now", not a
        // fixed 2026-01-01 placeholder.
        let tpacked = unsafe { syscall0(SYS_GET_RTC_TIME) };
        let dpacked = unsafe { syscall0(SYS_GET_RTC_DATE) };
        app.dt_hour  = ((tpacked >> 16) & 0xFF) as i32;
        app.dt_min   = ((tpacked >> 8) & 0xFF) as i32;
        app.dt_sec   = (tpacked & 0xFF) as i32;
        app.dt_year  = ((dpacked >> 16) & 0xFFFF) as i32;
        app.dt_month = ((dpacked >> 8) & 0xFF) as i32;
        app.dt_day   = (dpacked & 0xFF) as i32;
        if app.dt_year < 1970 || app.dt_year > 2099 { app.dt_year = 2026; }
        if app.dt_month < 1 || app.dt_month > 12 { app.dt_month = 1; }
        if app.dt_day < 1 || app.dt_day > 31 { app.dt_day = 1; }
        if app.dt_hour < 0 || app.dt_hour > 23 { app.dt_hour = 0; }
        if app.dt_min < 0 || app.dt_min > 59 { app.dt_min = 0; }
        if app.dt_sec < 0 || app.dt_sec > 59 { app.dt_sec = 0; }
    }

    app.draw();

    let mut ev = GuiEvent { ty: 0, target_id: 0, mouse_x: 0, mouse_y: 0,
                            mouse_buttons: 0, scroll_delta: 0, keycode: 0, key_char: 0 };
    loop {
        let got = win_get_event(win, &mut ev, 250);
        // #745: net_tick() runs on EVERY iteration, not only when the 250ms
        // wait expires. Making it conditional on the timeout was a real bug
        // measured on a VM: once the compositor had any steady event to
        // deliver, win_get_event() stopped returning 0 and the Network page's
        // state machine silently stopped advancing while the app stayed
        // responsive. net_tick() rate-limits itself against the monotonic
        // clock, and every syscall inside it returns immediately, so calling
        // it here cannot stall the wizard.
        if app.net_tick() { app.draw(); win_invalidate(win); }
        // #198: service pwr_win's own event queue every iteration too,
        // piggybacking on this same 250ms win_get_event(win, ...) cadence
        // rather than adding a new loop (#426/#419/#420 class) - a SECOND
        // win_get_event(pwr_win, ..., 0) is a non-blocking immediate poll,
        // same as net_tick() just above it.
        pwr_pump();
        if got == 0 { continue; }
        match ev.ty {
            EV_REDRAW => app.draw(),
            EV_KEY_DOWN => {
                // (#wizflash) Snapshot BEFORE on_key() runs so the check right
                // below can tell "a field's text changed in place" apart from
                // "something else about the page changed" (focus moved,
                // Enter/Esc navigated to a different page, a validation
                // message appeared or disappeared). Only the first case is
                // safe to redraw with the cheap single-field path - see
                // draw_field_delta()'s own comment for exactly why.
                let page_before = app.page;
                let focus_before = app.focus;
                // (#wizflash) PG_AI's own focus lives in ai_focus, not the
                // generic `focus` ring (dk_draw_ai sets nfields = 0) - it needs
                // its own before-snapshot, plus ai_key_show_saved() so a
                // badge<->field transition on the key row is never mistaken
                // for a same-shape text edit (see that method's comment).
                let ai_focus_before = app.ai_focus;
                let ai_key_show_saved_before = app.ai_key_show_saved();
                let err_ptr_before = app.err.as_ptr();
                let err_len_before = app.err.len();
                if !app.on_key(&ev) { break; }
                let simple_text_edit =
                    app.page == page_before
                    && app.err.as_ptr() == err_ptr_before
                    && app.err.len() == err_len_before
                    && (
                        ((app.page == PG_ACCOUNT || app.page == PG_NETWORK)
                            && app.focus == focus_before
                            && app.nfields > 0)
                        || (app.page == PG_AI
                            && app.ai_focus == ai_focus_before
                            && app.ai_focus != 0
                            && app.ai_key_show_saved() == ai_key_show_saved_before)
                    );
                if simple_text_edit { app.draw_field_delta(); } else { app.draw(); }
            }
            EV_MOUSE_DOWN => {
                if ev.mouse_buttons & MOUSE_LEFT != 0 {
                    // Mouse coordinates arrive WINDOW-local; every hit-test in
                    // on_click() is BODY-local, the same space the page drew in.
                    // Translated once, here, for the same reason the backdrop
                    // sampler was fixed at its choke point rather than at its
                    // call sites.
                    let (mx, my) = win_to_body(ev.mouse_x, ev.mouse_y);
                    if !app.on_click(mx, my) { break; }
                    app.draw();
                }
            }
            EV_MOUSE_SCROLL => {
            // scroll_delta is signed notches; one notch = one list row, which is
            // the cadence the rest of the desktop uses. Previously this event
            // was not handled AT ALL, so the wheel did nothing anywhere.
            //
            // PG_TIME is special-cased into its own dedicated
            // time_list_move() rather than falling through to the generic
            // list_move(): list_move() is now guarded to PG_WALL only (see
            // its own comment), so a stray wheel event on PG_TIME would
            // otherwise silently do nothing instead of scrolling the city
            // list it is sitting on top of.
            if ev.scroll_delta != 0 {
                if app.page == PG_TIME { app.time_list_move(-(ev.scroll_delta as i32)); }
                else { app.list_move(-(ev.scroll_delta as i32)); }
                app.draw();
            }
        }
        EV_MOUSE_UP => { if app.dragging { app.dragging = false; } }
        EV_MOUSE_MOVE => {
            let (mx, my) = win_to_body(ev.mouse_x, ev.mouse_y);
            if app.dragging { app.scroll_to_px(my); app.draw(); }
                // (local 128) PG_WELCOME has NO footer nav. footer_bounds()
                // deliberately ignores its page argument, and on_click() already
                // returns early for PG_WELCOME above rather than consulting it,
                // so the hover path was the one place that still applied the
                // Back/Continue band to a page that draws neither. Crossing that
                // band on page 1 flipped hover_nav and ran a FULL draw_welcome()
                // - twenty 32-row backdrop strips plus every glyph - to produce a
                // pixel-identical frame, because draw_welcome() never reads
                // hover_nav. Wasted work at rest; under #67 AP scheduling it is
                // also a window the compositor can composite the card in
                // mid-repaint, which is the flashing this ticket started from.
                // Gated here, next to the same early-return on_click() makes, so
                // the two hit-test paths agree about which pages have a footer.
                let nv = if app.page == PG_WELCOME { 0 } else {
                    let (bx0, bx1, px0, px1, fy0, fy1) = footer_bounds(app.page);
                    if my >= fy0 && my <= fy1 {
                        if mx >= px0 && mx <= px1 { 2 }
                        else if mx >= bx0 && mx <= bx1 { 1 } else { 0 }
                    } else { 0 }
                };
                if nv != app.hover_nav { app.hover_nav = nv; app.draw(); }
            }
            // No window-close case: setup is not dismissable. Closing it would
            // leave a machine with no account and no way back to this wizard.
            _ => {}
        }
    }

    pwr_destroy();   // #198: beside the existing win_destroy(win) below
    win_destroy(win);
    0
}
