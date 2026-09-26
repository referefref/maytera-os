// testhook.c - #334 headless GUI verification hook.
//
// PROBLEM THIS SOLVES: QEMU relative-mouse injection does not reliably land
// where sent (no software cursor to calibrate against - see blame.md #334/
// #440), and KEY_SUPER injection does not reliably toggle the Start menu
// either, so keyboard injection is not a safe fallback. Seven agents in one
// night independently hit this wall trying to visually verify compositor
// changes. This hook does NOT try to make pixel-coordinate injection more
// reliable; it sidesteps the problem by driving the UI BY NAME instead of
// by pixel: launch an app by its label, force a screensaver type, open/close
// the Start menu, or run a start-menu/desktop-icon item's exact launch
// action by name - none of which require knowing where anything is drawn on
// screen.
//
// WHAT THIS DOES AND DOES NOT PROVE (read before trusting a result):
//   - Driving `ICON <name>` / `MENUITEM <name>` calls straight into
//     desktop_launch_icon_by_name()/startmenu_launch_item_by_name(), which
//     call the exact same launch_app()/sys_spawn()/win16_run()/dos_run()
//     switch a real click runs - but SKIPS the hit-test (find_icon_at() /
//     the category+row geometry walk in startmenu_handle_mouse()) that a
//     real mouse click has to pass first. A pass here proves the launch
//     ACTION is wired correctly. It does NOT prove a real click at the
//     icon's actual screen coordinates would be routed to that action - a
//     geometry bug in the hit-test (wrong rect, dead zone, overlap with
//     another layer) would NOT be caught by this hook. For that class of
//     bug, real coordinate-accurate input is required - see the #440 VNC
//     server (vnc.c), which injects PointerEvents at absolute framebuffer
//     pixel coordinates (not relative deltas), so it does not suffer the
//     QEMU calibration problem and DOES exercise the real hit-test path.
//     Use VNC to verify hit-test/geometry; use this hook to verify what a
//     click is supposed to DO once it lands.
//   - `SAVER <n>` / `STARTMENU OPEN|CLOSE` call the same internal functions
//     Settings/the tray menu/the idle timeout call, so they exercise real
//     state transitions, just not through their own normal trigger path.
//   - `SHOT <path>` is not implemented here: the pre-existing /SCREENSHOT.REQ
//     mechanism (screenshot.c) already does this, file-driven, in every
//     build, so this hook does not duplicate it.
//
// SECURITY: this is an attack surface (anything that can write a file the
// compositor reads can drive the UI) so it must not exist in a shipping
// build. Enforcement is COMPILE-TIME, not a runtime flag: this file is only
// added to SRCS, and MAYTERA_TESTHOOK only defined, when the Makefile is
// invoked as `make TESTHOOK=1` - never how build/build-golden.sh or a
// developer's plain `make`/`make install` builds COMPOSIT. A normal binary
// has none of this file's code or symbols. See testhook.h for how to verify
// that on any given binary.
//
// NO BUSY-WAIT (#426): testhook_poll() is called once per compositor frame
// from main.c's main loop, exactly like screenshot_poll()/vnc_poll() right
// next to it - a cheap sys_open() that returns -1 immediately when
// /TESTHOOK.CMD is absent (the common case), never a spin/poll-sleep loop.

#ifdef MAYTERA_TESTHOOK

#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../libc/string.h"
// #removdev: devinfo.h -> types.h would re-typedef bool (_Bool) and clash
// with compositor.h (typedef int bool); satisfy the guard first.
#ifndef __bool_true_false_are_defined
#define __bool_true_false_are_defined 1
#endif
#include "../../libc/devinfo.h"   // sys_dev_usb_list() for REMOVDEMO
// (#123) write(1,...) serial mirror. Declared here rather than including
// libc/unistd.h: that header pulls in libc/types.h, whose `bool` typedef
// collides with compositor.h/stdbool in this translation unit.
long write(int fd, const void *buf, unsigned long n);
#include "../../libc/gui_theme.h"
#include "../../libc/userconf.h"   // #745 GLASSTHEME
#include "../../libc/dock_opacity.h"  // #132: shared DOCK_OPACITY_MIN/MAX
#include "../../libc/wallpapers.h"  // (wallpersist) wp_entry_t / WP_MAX_ENTRIES for SETWALL
#include "../../libc/stdio.h"    // (#231r) vsnprintf for th_logf below
#include "cardfile.h"   // (cfrender) CFCLICK/CFDOWN/CFMOVE/CFUP/CFKEY verbs below
#include <stdarg.h>
// #404 (cfhost) window-hosting verification: drive cf_host_apply() against a
// hand-built deck so the SYS_WM_SET_BOUNDS placement path is provable on-VM
// before agent A's deck render/input lands. NEVER in a golden (TESTHOOK gate).
#include "cardfile_model.h"
#include "cardfile_host.h"

// (#123) Provided by taskbar.c under the same MAYTERA_TESTHOOK guard.
void taskbar_dock_debug_dump(int force);
int  taskbar_dock_debug_click(int32_t x, int32_t y);
int  taskbar_dock_slot_point(int n, int32_t *x, int32_t *y);

// (#231r) traymenu.c, TESTHOOK-only: where the renderer puts a given
// band's fader cap. See the EQDRAG verb below for why the geometry is
// asked for rather than assumed.
int traymenu_eq_fader_point(int b, int pos, int *out_x, int *out_y);

#define TH_CMD_PATH "/TESTHOOK.CMD"

// Verification-only delayed-lock state for the PANELLOCK verb below: avoids
// needing a second host-side write to a live guest disk (unsafe/racy against
// the running compositor's own filesystem cache) just to sequence "open a
// panel, THEN lock" for a screenshot.
static uint64_t s_th_lock_at_ms = 0;
// (#shutdlg) Same idiom, for PCTEST below: a real click has to land AFTER
// confirmdialog.c's own 250ms input-settle window (CONFIRM_SETTLE_MS) has
// elapsed in REAL uptime, or confirm_dialog_handle_mouse() ignores it by
// design (#745) - an immediate open-then-click in one testhook_poll() call
// would always land inside that window and prove nothing.
static uint64_t s_th_pcclick_at_ms = 0;
static int32_t  s_th_pcclick_x = 0, s_th_pcclick_y = 0;
// (#sqearlyexit) SPAWNLOOP: relaunch one app N times on a wall-clock cadence,
// killing the previous instance first. Exists because the defect being chased
// is an INTERMITTENT failure during app STARTUP, so the only useful harness is
// one that performs many independent startups per boot and labels each one on
// serial. Non-blocking by construction (#426): one uptime_ms() compare per
// frame, no wait, no sleep, no poll loop.
static int      s_sl_left = 0;
static int      s_sl_iter = 0;
static int      s_sl_prev_pid = 0;
static int      s_sl_period = 3000;
static uint64_t s_sl_next_ms = 0;
static char     s_sl_path[96];
static char     s_sl_arg[64];
static int      s_sl_kill = 0;
static int      s_sl_armed = 0;   // one-shot: an offline-baked /TESTHOOK.CMD cannot always be consumed (PERMS-DENY on truncate), so it re-fires every poll
#define TH_OUT_PATH "/TESTHOOK.OUT"
#define TH_O_APPEND (0x1 | 0x40 | 0x400)   // O_WRONLY | O_CREAT | O_APPEND

// (#123) When a SEQ is running, every command it dispatches must hand control
// back to SEQ afterwards, or the sequence stops after one step. Every verb path
// in testhook_poll() ends in exactly one th_log() call, so re-arming here is
// the ONE place that covers all of them without touching each verb - and it
// cannot re-arm when no sequence is running, because g_seq_running is only set
// by the SEQ verb itself.
int g_seq_running = 0;
int g_th_mouse_pinned = 0;   // (#123) see poll_input() in main.c
// (cfrender) Same idea as g_th_mouse_pinned, but for the BUTTON state.
// poll_input() in main.c overwrites g_mouse_buttons from the real (always
// unpressed, in a headless VM) hardware every frame regardless of
// g_th_mouse_pinned, which only ever protected position - so a synthetic
// CFDOWN press was clobbered back to "released" before the next testhook
// verb (CFMOVE) even ran, collapsing every scripted drag into an instant
// no-drag click. See poll_input()'s own comment at the g_mouse_buttons
// assignment. Set for the duration of a CFDOWN..CFUP pair only.
int g_th_buttons_pinned = 0;
// (#123) Extra SEQ hold, in polls, requested by the HOLD verb. A host-side
// screendump watcher with a fixed delay CANNOT reliably capture a step whose
// hold is shorter than its own lag - runs 2 and 3 both produced captures of
// the wrong state for exactly that reason, and a screenshot labelled with the
// wrong state is worse than no screenshot. HOLD lets the sequence freeze a
// state for as long as the capture needs, so the capture is matched to the
// serial log by CONTENT and with a wide margin.
int g_seq_extra_hold = 0;
static void th_rearm_seq(void) {
    if (!g_seq_running) return;
    int fd = sys_open(TH_CMD_PATH, 0x1 | 0x40 | 0x200);
    if (fd >= 0) { sys_write(fd, "SEQ\n", 4); sys_close(fd); }
}
static void th_log(const char *msg) {
    // Mirror to serial FIRST, and re-arm LAST, both unconditionally: the file
    // write below can fail (read-only root, full disk) and an early return
    // there used to take the whole rest of this function with it. That would
    // silently stall a running SEQ, which is exactly the "a guard that never
    // fires and a guard that is absent look identical" failure mode.
    // /TESTHOOK.OUT can only be read after the VM is shut down and its image
    // mounted, which is useless for a live, paced run - hence the mirror.
    write(1, "[TH] ", 5);
    write(1, msg, strlen(msg));
    write(1, "\n", 1);
    int fd = sys_open(TH_OUT_PATH, TH_O_APPEND);
    if (fd >= 0) {
        sys_write(fd, msg, strlen(msg));
        sys_write(fd, "\n", 1);
        sys_close(fd);
    }
    th_rearm_seq();
}


// (#231r) th_log() takes a plain string and every EQ report below carries
// numbers. One local formatter beats a hand-rolled itoa at eight call sites.
static void th_logf(const char *fmt, ...) {
    char b[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    th_log(b);
}

// Split "VERB rest-of-line" in place. Returns the verb (buf, trimmed of
// trailing CR/LF/whitespace); *arg points at the first non-space char after
// the verb, or "" if there is none. Never over-reads: buf is NUL-terminated
// by the caller before this runs.
static char *th_split(char *buf, char **arg) {
    char *p = buf;
    while (*p == ' ' || *p == '\t') p++;
    char *verb = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    int had_space = (*p == ' ' || *p == '\t');
    if (*p) *p++ = '\0';
    if (had_space) {
        while (*p == ' ' || *p == '\t') p++;
    }
    *arg = p;
    // Trim trailing CR/LF/spaces off the argument.
    size_t n = strlen(*arg);
    while (n > 0 && ((*arg)[n-1] == '\r' || (*arg)[n-1] == '\n' ||
                     (*arg)[n-1] == ' '  || (*arg)[n-1] == '\t')) {
        (*arg)[--n] = '\0';
    }
    return verb;
}


// (#745) Append a decimal int to `o`, returning the byte count. testhook.c had
// th_atoi but nothing going the other way, and every #745 hook reports numbers.
static int th_int(char *o, int v) {
    int p = 0;
    if (v < 0) { o[p++] = '-'; v = -v; }
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) o[p++] = t[--n];
    return p;
}

static int th_atoi(const char *s) {
    int neg = 0, v = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}


// ===========================================================================
// #745 GLASS VERIFICATION HOOKS
//
// These exist because the two things that had to be reported for the glass
// work cannot be obtained from a screenshot:
//
//   GLASSBENCH - the per-frame cost. uptime_ms() has millisecond granularity
//     and the recompute is budgeted at fractions of a millisecond, so a single
//     timed call reads 0 or 1 and tells you nothing. Timing N forced-cold
//     recomputes and dividing is what produces a real number. CAVEAT, stated
//     because it changes how the result should be read: repeating the same
//     recompute leaves the source rect hot in cache, so this is a LOWER BOUND
//     on the cold cost of a first-touch recompute in a live frame.
//
//   GLASSPROBE - contrast on the RENDERED pixel over a CONTROLLED backdrop.
//     The rendered pixel on glass is a blend over an arbitrary wallpaper, so
//     sampling a screenshot of one wallpaper measures that wallpaper, not the
//     guarantee. This paints the surface's whole source region (surface +
//     GLASS_BLEED, so the blur sees nothing else) with a known colour, runs
//     the REAL taskbar_render(), and dumps raw framebuffer pixels. The ratios
//     are computed off-box from those bytes; nothing here is analytic.
// ===========================================================================

static int th_hex4(char *o, uint32_t v) {
    const char *H = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) o[i] = H[(v >> (20 - 4 * i)) & 0xF];
    return 6;
}

static void th_sample(const char *name, int32_t x, int32_t y) {
    char line[96];
    int p = 0;
    for (const char *q = name; *q && p < 40; q++) line[p++] = *q;
    line[p++] = ' ';
    p += th_int(line + p, x); line[p++] = ' ';
    p += th_int(line + p, y); line[p++] = ' ';
    uint32_t c = (x >= 0 && y >= 0 && x < g_fb_width && y < g_fb_height)
               ? (g_fb[y * g_fb_pitch + x] & 0x00FFFFFFu) : 0xFFFFFFFFu;
    p += th_hex4(line + p, c);
    line[p] = '\0';
    th_log(line);
}

static void th_glass_bench(int iters) {
    if (iters < 1) iters = 1;
    char line[128];
    int save_dg = g_glass_downgrade_ms;
    g_glass_downgrade_ms = 1000000;   // never downgrade DURING a measurement
    g_glass_live = 1;

    // Rects deliberately sized to the spec's 1024x768 budget table so the
    // numbers are directly comparable, even though the framebuffer is larger.
    struct { const char *nm; int surf; int32_t x, y, w, h; } S[4] = {
        { "TASKBAR", GLASS_SURF_PANEL, 0, g_fb_height - 36, 1024, 36 },
        { "PANEL",   GLASS_SURF_PANEL, 0, 0,                1024, 30 },
        { "DOCK",    GLASS_SURF_DOCK,  300, g_fb_height - 64, 400, 64 },
        { "MENU",    GLASS_SURF_MENU,  4, 32,               300, 470 },
    };

    for (int i = 0; i < 4; i++) {
        // COLD: force a full recompute every iteration.
        uint64_t t0 = uptime_ms();
        for (int k = 0; k < iters; k++) {
            glass_invalidate_all();
            glass_render(S[i].x, S[i].y, S[i].w, S[i].h, CLR_GLASS_TINT, S[i].surf);
        }
        uint64_t cold = uptime_ms() - t0;

        // CACHED, full-frame flavour: signature check + strip blit.
        glass_render(S[i].x, S[i].y, S[i].w, S[i].h, CLR_GLASS_TINT, S[i].surf);
        t0 = uptime_ms();
        for (int k = 0; k < iters; k++)
            glass_render(S[i].x, S[i].y, S[i].w, S[i].h, CLR_GLASS_TINT, S[i].surf);
        uint64_t warm = uptime_ms() - t0;

        // CACHED, clipped-pass flavour: strip blit only, no g_fb read at all.
        g_glass_live = 0;
        t0 = uptime_ms();
        for (int k = 0; k < iters; k++)
            glass_render(S[i].x, S[i].y, S[i].w, S[i].h, CLR_GLASS_TINT, S[i].surf);
        uint64_t blit = uptime_ms() - t0;
        g_glass_live = 1;

        int p = 0;
        for (const char *q = S[i].nm; *q; q++) line[p++] = *q;
        line[p++] = ' ';
        p += th_int(line + p, S[i].w);  line[p++] = 'x';
        p += th_int(line + p, S[i].h);  line[p++] = ' ';
        const char *k1 = "iters="; for (const char *q = k1; *q; q++) line[p++] = *q;
        p += th_int(line + p, iters);
        const char *k2 = " cold_ms="; for (const char *q = k2; *q; q++) line[p++] = *q;
        p += th_int(line + p, (int)cold);
        const char *k3 = " warm_ms="; for (const char *q = k3; *q; q++) line[p++] = *q;
        p += th_int(line + p, (int)warm);
        const char *k4 = " blit_ms="; for (const char *q = k4; *q; q++) line[p++] = *q;
        p += th_int(line + p, (int)blit);
        line[p] = '\0';
        th_log(line);
    }

    g_glass_downgrade_ms = save_dg;
    g_glass_live = 0;
    glass_invalidate_all();
    g_needs_redraw = true;
}

// Paint a controlled backdrop over the whole SOURCE region of the bottom
// taskbar (surface + GLASS_BLEED on the open side), render the real taskbar,
// then dump raw pixels.
static void th_glass_probe(int backdrop_white) {
    char line[96];
    int32_t H = g_fb_height, W = g_fb_width;
    int32_t ty = H - 36;
    uint32_t bd = backdrop_white ? 0xFFFFFFFFu : 0xFF000000u;

    int ob = g_draw_blend; g_draw_blend = 255;
    draw_clear_clip();
    draw_fill_rect(0, ty - 40, W, 40 + 36, bd);
    g_draw_blend = ob;

    g_glass_live = 1;
    glass_invalidate_all();
    taskbar_render();            // the REAL renderer, not a reimplementation
    g_glass_live = 0;

    int p = 0;
    const char *k = backdrop_white ? "BACKDROP WHITE op=" : "BACKDROP BLACK op=";
    for (const char *q = k; *q; q++) line[p++] = *q;
    p += th_int(line + p, g_dock_opacity);
    const char *k2 = " tint="; for (const char *q = k2; *q; q++) line[p++] = *q;
    p += th_hex4(line + p, CLR_GLASS_TINT & 0x00FFFFFFu);
    const char *k3 = " enable="; for (const char *q = k3; *q; q++) line[p++] = *q;
    p += th_int(line + p, g_glass_enable);
    line[p] = '\0';
    th_log(line);

    // Glass surface, well clear of the chips and the right-hand cluster.
    th_sample("GLASS_MID",   W / 2,      ty + 18);
    th_sample("GLASS_LEFT",  300,        ty + 18);
    // Chip 1 (Start) is at TASKBAR_PADDING, 28x28, centred vertically.
    th_sample("CHIP1_FILL",  6,          ty + 6);
    th_sample("CHIP1_BORDER", 4,         ty + 18);
    // Chip 2 (Maytera).
    th_sample("CHIP2_FILL",  38,         ty + 6);
    th_sample("CHIP2_BORDER", 36,        ty + 18);
    // Glass immediately right of the chips: the chip-boundary comparison.
    th_sample("GLASS_NEXTTO", 72,        ty + 18);
    g_needs_redraw = true;
}

// Report the live per-surface counters.
static void th_glass_stat(const char *tag) {
    char line[144];
    // (#glassmodal) GLASS_SURF_MODAL added as the 4th slot (compositor.h) -
    // this array MUST track GLASS_SURF_COUNT or the loop below reads past
    // its end (caught by -Waggressive-loop-optimizations when this was
    // still sized [3] and the loop already ran to 4).
    static const char *NM[GLASS_SURF_COUNT] = { "PANEL", "DOCK", "MENU", "MODAL" };
    for (int i = 0; i < GLASS_SURF_COUNT; i++) {
        uint32_t cn = 0, cms = 0, cw = 0, hn = 0; int tier = 0;
        glass_perf_get(i, &cn, &cms, &cw, &hn, &tier);
        int p = 0;
        const char *t = "STAT["; for (const char *q = t; *q; q++) line[p++] = *q;
        for (const char *q = tag; *q; q++) line[p++] = *q;
        line[p++] = ']'; line[p++] = ' ';
        for (const char *q = NM[i]; *q; q++) line[p++] = *q;
        const char *a = " tier="; for (const char *q = a; *q; q++) line[p++] = *q;
        p += th_int(line + p, tier);
        const char *b = " cold_n="; for (const char *q = b; *q; q++) line[p++] = *q;
        p += th_int(line + p, (int)cn);
        const char *c = " cold_ms="; for (const char *q = c; *q; q++) line[p++] = *q;
        p += th_int(line + p, (int)cms);
        const char *dd = " worst_ms="; for (const char *q = dd; *q; q++) line[p++] = *q;
        p += th_int(line + p, (int)cw);
        const char *e = " hits="; for (const char *q = e; *q; q++) line[p++] = *q;
        p += th_int(line + p, (int)hn);
        const char *f = " opacity="; for (const char *q = f; *q; q++) line[p++] = *q;
        p += th_int(line + p, g_dock_opacity);
        line[p] = '\0';
        th_log(line);
    }
}

static void th_glass_all(int iters) {
    th_log("==== #745 GLASS VERIFICATION SUITE ====");
    th_glass_stat("boot");

    th_log("---- BENCH (rects sized to the spec's 1024x768 budget table) ----");
    th_glass_bench(iters);

    th_log("---- CONTRAST, dark theme (maytera_dark) ----");
    // #745 dockgrey (2026-08-12): 90/75/60 -> 90/75/70. 75 is the new default,
    // 70 the new floor (was 60 - see draw.c glass_render()'s floor comment for
    // why it moved with the tint-lightening in the same change); 90 kept as a
    // high-opacity reference point.
    int op[3] = { 90, 75, 70 };
    for (int i = 0; i < 3; i++) {
        g_dock_opacity = op[i];
        th_glass_probe(1);      // pure white backdrop: worst case for white ink
        th_glass_probe(0);      // pure black backdrop
    }

    th_log("---- CONTRAST, light theme (maytera_light) ----");
    {
        int idx = gui_theme_activate("maytera_light");
        if (idx < 0) th_log("ERR could not activate maytera_light");
        else {
            compositor_apply_theme(idx);
            for (int i = 0; i < 3; i++) {
                g_dock_opacity = op[i];
                th_glass_probe(1);
                th_glass_probe(0);
            }
        }
    }

    th_log("---- TIER 4 opt-out check (retro_unix, style=retro) ----");
    {
        int idx = gui_theme_activate("retro_unix");
        if (idx >= 0) {
            compositor_apply_theme(idx);
            g_dock_opacity = 75;
            th_glass_probe(1);   // glass_enable should report 0 here
        } else th_log("ERR could not activate retro_unix");
    }

    // Back to the theme under test.
    {
        int idx = gui_theme_activate("maytera_dark");
        if (idx >= 0) compositor_apply_theme(idx);
        g_dock_opacity = 75;   // #745 dockgrey: new default (was 90)
    }

    th_log("---- TIER 2 AUTO-DOWNGRADE, forced ----");
    th_glass_stat("before-downgrade");
    g_glass_downgrade_ms = 0;      // any measured cold recompute now trips it
    glass_invalidate_all();
    g_glass_live = 1;
    glass_render(0, g_fb_height - 36, 1024, 36, CLR_GLASS_TINT, GLASS_SURF_PANEL);
    glass_render(300, g_fb_height - 64, 400, 64, CLR_GLASS_TINT, GLASS_SURF_DOCK);
    glass_render(4, 32, 300, 470, CLR_GLASS_TINT, GLASS_SURF_MENU);
    g_glass_live = 0;
    th_glass_stat("after-downgrade");
    g_glass_downgrade_ms = 4;      // restore the shipping threshold
    glass_invalidate_all();

    th_log("==== END SUITE ====");
    g_needs_redraw = true;
}

// ===========================================================================
// #404 (cfhost): Cardfile window-hosting harness. Builds a two-card deck by
// hand, launches the two apps ONCE, and each frame drives cf_host_apply() so
// the real app windows are placed / tiled / hidden by SYS_WM_SET_BOUNDS to
// match the deck's open state. Proves agent C's deliverable without agent A's
// deck render. Verbs: CFHOST SINGLE|GROUP|COLUMNS|STOW  (see below).
// ===========================================================================
static cf_deck_t th_cf_deck;
static int       th_cf_armed = 0;      // 1 => cf_host_apply() runs every frame
static int       th_cf_launched = 0;   // 1 => the two apps have been spawned
static int       th_cf_mode = -1;      // last-built mode; -1 forces a (re)build

// Build the already-resolved pixel geometry the model's deck walk needs, the
// same way agent A will for cf_layout(). Raw metrics (100% ui scale on the
// throwaway VM); edge_w/tab_step go through the model's own compression.
static cf_geom_t th_cf_geom(int nslots) {
    extern int g_glass_enable;
    int modern = g_glass_enable ? 1 : 0;
    cf_geom_t g;
    g.rail_w   = modern ? 30 : 28;
    g.tab_w    = modern ? 32 : 30;
    g.tab_top  = modern ? 52 : 48;
    g.foot     = modern ? 100 : 96;
    g.screen_w = g_fb_width;
    g.screen_h = g_fb_height;
    int theme_edge = modern ? 30 : 28;
    int theme_step = modern ? 34 : 30;
    g.edge_w   = cf_edge_width(theme_edge, g.rail_w, nslots, g.screen_w);
    g.tab_step = cf_tab_step(theme_step, g.tab_top, g.foot, CF_TAB_MAX_H, nslots, g.screen_h);
    return g;
}

// Rebuild the deck for `mode` (0 single, 1 group, 2 columns, 3 stow). The two
// cards are re-created with win_id 0 every time; cf_host_apply()'s reconcile
// re-binds the two ALREADY-RUNNING windows to them by app identity, so the
// apps are never respawned when switching modes.
static void th_cf_build(int mode) {
    unsigned long now = uptime_ms();
    cf_deck_init(&th_cf_deck);
    uint32_t a = cf_add_card(&th_cf_deck, "/APPS/FILES", "Files",
                             CF_CAT_SYSTEM, CF_COLOR_SLATE, now, 0, 0);
    uint32_t bslot = cf_add_card(&th_cf_deck, "/APPS/CALC", "Calc",
                             CF_CAT_ACCESSORIES, CF_COLOR_SAGE, now, 0, 0);
    cf_slot_t *sb = cf_find_slot(&th_cf_deck, bslot);
    uint32_t bcard = sb ? sb->cards[0].id : 0;
    // (cfmaxwidth) cf_host_launch() now takes (deck, card_id, path) so it can
    // record the pre-launch snapshot exclusion floor on the REAL card - moved
    // here (the first rebuild after CFHOST arms) instead of the CFHOST verb
    // itself, which fired before any card existed. Every LATER rebuild
    // (mode switch) creates fresh card ids in a fresh th_cf_deck but does
    // NOT re-launch - cf_host_apply()'s reconcile re-binds the two already-
    // running windows to the new cards by app identity, exactly as before
    // this change; those cards' launch_floor_id is 0 (never launched),
    // which imposes no restriction and matches the existing window as it
    // always did.
    if (!th_cf_launched) {
        cf_host_launch(&th_cf_deck, a, "/APPS/FILES");
        cf_host_launch(&th_cf_deck, bcard, "/APPS/CALC");
        th_cf_launched = 1;
    }
    if (mode == 0) {            // SINGLE: Files fills the body, Calc stowed
        cf_open_single(&th_cf_deck, a, now);
    } else if (mode == 1) {     // GROUP: Files+Calc tiled 1x2 in one slot
        cf_group_card_into(&th_cf_deck, bcard, a, now);
        cf_open_as_group_split(&th_cf_deck, a, now);
    } else if (mode == 2) {     // COLUMNS: Files | Calc side by side
        cf_open_single(&th_cf_deck, a, now);
        cf_open_second_as_column(&th_cf_deck, bslot, 0, now);
    } else {                    // STOW: both hidden
        cf_stow_slot(&th_cf_deck, a);
        cf_stow_slot(&th_cf_deck, bslot);
    }
}

// ==========================================================================
// #removdev (THROWAWAY-ONLY, never shipped): self-driving removable-device
// verification. `REMOVDEMO` arms a monitor that, on each ~1s poll:
//   - logs SYS_VOL_LIST and SYS_DEV_USB_LIST to serial whenever either
//     changes, so a headless VM leaves a persistent trace of a drive or NIC
//     arriving; and
//   - when a removable, mounted volume appears that was NOT present when the
//     monitor armed (a hot-plugged stick), waits 5s (so the desktop icon and
//     toast can be screendumped) then EJECTS it once via vol_eject() - the
//     same path the desktop/File-Manager right-click uses: SYS_VOL_EJECT ->
//     hotplug_eject -> usb_msc_safe_remove -> usb_msc_sync (SYNCHRONIZE
//     CACHE) - logging the return code. One baked command drives the whole
//     flow, since the live VM disk cannot be written from the host to deliver
//     a second command.
static int      s_rd_armed = 0;
static int      s_rd_primed = 0;
static uint64_t s_rd_last = 0;
static unsigned s_rd_volsig = 0, s_rd_devsig = 0;
static int32_t  s_rd_base_idx[SC_VOL_MAX];
static int      s_rd_nbase = 0;
static uint64_t s_rd_eject_at = 0;
static int      s_rd_target = -1;
static int      s_rd_ejected = 0;

static void removdemo_tick(void) {
    uint64_t now = uptime_ms();
    if (s_rd_last != 0 && (now - s_rd_last) < 1000) return;
    s_rd_last = now;

    sc_volume_t vols[SC_VOL_MAX];
    int nv = vol_list(vols, SC_VOL_MAX);
    if (nv < 0) nv = 0;
    devinfo_usb_t devs[24];
    int nd = sys_dev_usb_list(devs, 24);
    if (nd < 0) nd = 0;

    unsigned vsig = 2166136261u;
    for (int i = 0; i < nv; i++) {
        vsig = (vsig ^ (unsigned)vols[i].index) * 16777619u;
        vsig = (vsig ^ vols[i].flags) * 16777619u;
    }
    vsig ^= (unsigned)nv;
    if (vsig != s_rd_volsig) {
        s_rd_volsig = vsig;
        th_logf("REMOVDEMO vols=%d", nv);
        for (int i = 0; i < nv; i++)
            th_logf("REMOVDEMO   vol idx=%d flags=0x%02x name='%s' mount='%s'",
                    vols[i].index, vols[i].flags, vols[i].name, vols[i].mount);
    }
    unsigned dsig = 2166136261u;
    for (int i = 0; i < nd; i++) {
        if (devs[i].is_controller) continue;
        dsig = (dsig ^ (((unsigned)devs[i].vendor_id << 16) | devs[i].product_id)) * 16777619u;
        dsig = (dsig ^ devs[i].dev_class) * 16777619u;
    }
    if (dsig != s_rd_devsig) {
        s_rd_devsig = dsig;
        for (int i = 0; i < nd; i++) {
            if (devs[i].is_controller) continue;
            th_logf("REMOVDEMO   usb cls=0x%02x %04x:%04x addr=%d",
                    devs[i].dev_class, devs[i].vendor_id, devs[i].product_id, devs[i].address);
        }
    }

    if (!s_rd_primed) {
        s_rd_nbase = 0;
        for (int i = 0; i < nv && s_rd_nbase < SC_VOL_MAX; i++)
            s_rd_base_idx[s_rd_nbase++] = vols[i].index;
        s_rd_primed = 1;
        th_logf("REMOVDEMO armed: baseline %d volume(s)", s_rd_nbase);
        return;
    }

    if (s_rd_target < 0 && !s_rd_ejected) {
        for (int i = 0; i < nv; i++) {
            int isbase = 0;
            for (int j = 0; j < s_rd_nbase; j++)
                if (vols[i].index == s_rd_base_idx[j]) { isbase = 1; break; }
            if (isbase) continue;
            if (!(vols[i].flags & MOSVOL_REMOVABLE) || !(vols[i].flags & MOSVOL_MOUNTED)) continue;
            s_rd_target = vols[i].index;
            s_rd_eject_at = now + 5000;
            th_logf("REMOVDEMO hot-plug volume idx=%d name='%s' -> eject in 5s",
                    vols[i].index, vols[i].name);
            break;
        }
    }
    if (s_rd_target >= 0 && !s_rd_ejected && now >= s_rd_eject_at) {
        int rc = vol_eject(s_rd_target);
        th_logf("REMOVDEMO eject idx=%d rc=%d (flush+unmount)", s_rd_target, rc);
        s_rd_ejected = 1;
        s_rd_target = -1;
    }
}

// #emfield (owner request 2026-09-22): self-driving EMFIELD proof harness.
// Arms EMFIELD, spawns two windows (Calc, and Files which it oscillates back and
// forth every frame via wm_set_bounds()), so a headless VM can be screendumped
// with the swirl/vortex mid-motion without coordinate-accurate mouse drag
// (#334) - the same wm_set_bounds() path a titlebar drag calls, just a
// different entry point. After the swirl has built up it opens the Start menu
// once, so the frosted-glass-over-the-live-field proof (requirement 4) can be
// captured too. Compile-gated with the rest of this file: never in a golden.
static int      s_emf_armed = 0;
static int      s_emf_phase = 0;   // 0 = spawn, 1 = oscillate, 2 = +start menu
static uint64_t s_emf_t0    = 0;

static int emf_name_eq(const char *a, const char *want) {
    int j = 0;
    for (; a[j] && want[j]; j++) {
        char x = a[j], y = want[j];
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
    }
    return a[j] == '\0' && want[j] == '\0';
}

static void emfdemo_tick(void) {
    if (s_emf_phase == 0) {
        sys_spawn("/APPS/CALC");
        sys_spawn("/APPS/FILES");
        s_emf_t0 = uptime_ms();
        s_emf_phase = 1;
        return;   // let the windows appear before moving one
    }
    if (s_emf_phase >= 2) {
        // Parked: Start menu is open for the glass proof. Do NOT move a window
        // now - a wm_set_bounds re-focuses that window and closes the menu.
        // The ambient swirl + persistent dye keep the field alive underneath.
        return;
    }

    // Locate windows. Oscillate CALC (small, central open field) with a
    // triangle wave (constant speed, smooth turn) so the vortex is steady and
    // the dye trails it.
    wm_window_info_t wins[16];
    int n = wm_get_windows(wins, 16);
    if (n < 0) n = 0;
    int id = -1, w = 0, h = 0, files_id = -1, files_w = 0, files_h = 0;
    for (int i = 0; i < n; i++) {
        if (!wins[i].app_id[0]) continue;
        if (emf_name_eq(wins[i].app_id, "CALC")) { id = wins[i].id; w = wins[i].width; h = wins[i].height; }
        else if (emf_name_eq(wins[i].app_id, "FILES")) { files_id = wins[i].id; files_w = wins[i].width; files_h = wins[i].height; }
    }
    if (id < 0) return;   // Calc not up yet

    uint64_t dt = uptime_ms() - s_emf_t0;

    // After ~18s of swirling: shove BOTH windows to opposite top corners so the
    // Start menu's frosted glass sits over the open swirling field, then open it
    // once and park (nothing re-focuses and closes it after this).
    if (dt > 18000) {
        if (files_id >= 0)
            wm_set_bounds(files_id, (int)g_fb_width - 220, 40, files_w, files_h, 0);
        wm_set_bounds(id, 20, 40, w, h, 0);
        if (!g_start_menu_open) startmenu_toggle();
        s_emf_phase = 2;
        g_needs_redraw = true;
        return;
    }

    uint64_t period = 3000;
    uint64_t p = dt % period;
    int half = (int)(period / 2);
    int tri_num = ((int)p <= half) ? (int)p : (int)(period - p);   // 0..half..0
    int range = (int)g_fb_width - w - 80;
    if (range < 0) range = 0;
    int nx = 40 + (range * tri_num) / (half > 0 ? half : 1);
    int ny = (int)g_fb_height / 2 - h / 2;
    wm_set_bounds(id, nx, ny, w, h, 0);
    g_needs_redraw = true;
}

// (#sqearlyexit) Close the first window whose title contains `want` (case
// insensitive), through the same taskbar_close_window() a real taskbar click
// runs. Used by SPAWNLOOP mode 2, whose whole point is that the win_destroy()
// then happens IN THE APP'S OWN PROCESS on its own next event-loop turn, i.e.
// asynchronously to whatever the compositor is doing - which is exactly the
// condition the window-lifetime bug needs, and the condition a SIGKILL from
// this same poll can never create.
static int th_close_by_title(const char *want) {
    wm_window_info_t wins[16];
    int n = wm_get_windows(wins, 16);
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) {
        for (const char *h = wins[i].title; *h; h++) {
            const char *hh = h, *nn = want;
            while (*hh && *nn) {
                char a = *hh, b = *nn;
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) break;
                hh++; nn++;
            }
            if (!*nn) { taskbar_close_window(wins[i].id); return wins[i].id; }
        }
    }
    return -1;
}

void testhook_poll(void) {
    if (s_th_lock_at_ms != 0 && uptime_ms() >= s_th_lock_at_ms) {
        s_th_lock_at_ms = 0;
        lock_enter();
        th_log("OK PANELLOCK fired");
    }
    if (s_th_pcclick_at_ms != 0 && uptime_ms() >= s_th_pcclick_at_ms) {
        s_th_pcclick_at_ms = 0;
        startmenu_test_power_confirm_click(s_th_pcclick_x, s_th_pcclick_y);
        th_log("OK PCTEST click fired");
    }
    // #404 (cfhost): while armed, place/hide the hosted windows every frame.
    // cf_host_apply() reconciles the async window bind and re-issues placement,
    // so a window that only just appeared snaps to its card rect within a frame.
    if (th_cf_armed) {
        // Auto-cycle SINGLE -> GROUP -> COLUMNS -> STOW on a 4s wall clock, so a
        // headless throwaway VM can be screendumped through every hosting state
        // without live command delivery. Rebuild only when the mode changes.
        int m = (int)((uptime_ms() / 4000UL) % 4UL);
        if (m != th_cf_mode) { th_cf_mode = m; th_cf_build(m); }
        cf_geom_t g = th_cf_geom(th_cf_deck.nslots);
        cf_host_apply(&th_cf_deck, &g);
    }
    if (s_sl_left > 0 && uptime_ms() >= s_sl_next_ms) {
        int closed = -1;
        if (s_sl_kill == 2) {
            if (s_sl_iter > 0) closed = th_close_by_title(s_sl_arg);
        } else if (s_sl_kill == 1 && s_sl_prev_pid > 0) {
            syscall2(SYS_KILL, s_sl_prev_pid, 9);
        }
        s_sl_iter++;
        char *av[2]; av[0] = s_sl_path; av[1] = s_sl_arg;
        int argn = (s_sl_kill == 2 || !s_sl_arg[0]) ? 1 : 2;
        int pid = sys_spawn_args(s_sl_path, av, argn);
        (void)closed;
        s_sl_prev_pid = pid;
        s_sl_left--;
        s_sl_next_ms = uptime_ms() + (uint64_t)s_sl_period;
        th_logf("SPAWNLOOP iter=%d left=%d pid=%d path=%s", s_sl_iter, s_sl_left, pid, s_sl_path);
    }
    if (s_rd_armed) removdemo_tick();   // #removdev self-driving removable monitor
    if (s_emf_armed) emfdemo_tick();     // #emfield self-driving EMFIELD swirl proof
    int fd = sys_open(TH_CMD_PATH, 0 /* O_RDONLY */);
    if (fd < 0) return;   // no pending command: fast common-case return

    char buf[192];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    // (#231) Consume BEFORE dispatch, and do not rely on sys_unlink() alone.
    // MEASURED this session: a /TESTHOOK.CMD baked onto the image OFFLINE
    // before boot (the documented way to deliver a one-shot command - see
    // the file-top comment) is owned by a different uid/context than the
    // logged-in session that runs this poll, and sys_unlink() on it silently
    // no-ops - the file was still present, byte-for-byte unchanged, after a
    // command had been dispatched from it dozens of times over a live
    // session. Its return value was never checked, so "consume so a command
    // never re-fires" was not true for exactly the offline-baked delivery
    // path this file's own design relies on. A truncate-to-empty (O_TRUNC on
    // the existing file, no directory entry change, no ownership
    // dependency - proven to work here even when unlink on the same path did
    // not) is what actually stops the re-fire; unlink is kept as a best-
    // effort cleanup on top; a WLOCK/WDESIGN toggle-style verb fired this
    // way flips on every single poll and its net effect is essentially
    // parity noise, not a controlled one-shot change.
    { int tfd = sys_open(TH_CMD_PATH, 0x1 | 0x40 | 0x200); if (tfd >= 0) sys_close(tfd); }
    sys_unlink(TH_CMD_PATH);
    if (n <= 0) return;   // already-empty file: nothing pending, not an error
    buf[n] = '\0';

    char *arg = "";
    char *verb = th_split(buf, &arg);

    if (verb[0] == '\0') {
        th_log("ERR empty command");
        return;
    }

    if (strcmp(verb, "EMFDEMO") == 0) {   // #emfield self-driving EMFIELD swirl proof
        // Idempotent: a baked /TESTHOOK.CMD on ext2 cannot be consumed
        // (truncate/unlink no-op for the offline-baked owner), so this verb
        // re-fires every frame. Ignore re-fires so the phase/t0 timeline
        // actually advances (otherwise the window never oscillates).
        if (s_emf_armed) return;
        set_wallpaper_anim(5 /* WPANIM_EMFIELD */);
        set_wallpaper_anim_intensity(80);
        set_wallpaper_anim_repel(1);
        s_emf_armed = 1;
        s_emf_phase = 0;
        g_needs_redraw = true;
        th_log("OK EMFDEMO armed");
        return;
    }

    if (strcmp(verb, "REMOVDEMO") == 0) {   // #removdev throwaway-only removable-device demo
        s_rd_armed = 1;
        th_log("OK REMOVDEMO armed");
        return;
    }

    if (strcmp(verb, "CFHOST") == 0) {
        // CFHOST SINGLE|GROUP|COLUMNS|STOW - hand-built deck window hosting.
        // First invocation switches the shell to the Cardfile layout (so the
        // rail draws and the classic taskbar stops); every invocation
        // rebuilds the open state for the named mode. (cfmaxwidth) Files +
        // Calc are now spawned from inside th_cf_build()'s first call
        // (th_cf_launched gates it there), once real card ids exist for
        // cf_host_launch()'s pre-launch snapshot exclusion to record against.
        taskbar_set_style(DOCK_CARDFILE);
        th_cf_armed = 1;
        th_cf_mode = -1;   // force the driver to (re)build on the next frame
        th_logf("OK CFHOST armed, auto-cycling SINGLE/GROUP/COLUMNS/STOW (fb=%dx%d)",
                (int)g_fb_width, (int)g_fb_height);
        return;
    }

    if (strcmp(verb, "DOCKOPACTEST") == 0) {
        // A HONEST persistence test has to change the value LONG AFTER boot.
        // The first attempt changed it within ~330ms and "failed"; the cause
        // was the test, not the code. compositor_init() calls profile_save()
        // directly ("ensure the profile file exists"), so the file already had
        // the boot-time value; profile_tick() then took its FIRST hash sample
        // (last == -1 returns without saving) AFTER the poll had already
        // applied the new value, so the hash never appeared to change and no
        // save was owed. In real use the change arrives seconds or minutes
        // after boot, with a baseline hash long since recorded.
        //
        // So: idle for ~200 frames, THEN write the CFG, then keep sampling.
        extern void dock_opacity_write_cfg(int v);
        static int reps = 0;
        static int want = 0;
        if (reps == 0) want = th_atoi(arg);
        reps++;
        if (reps < 600) {
            int fd2 = sys_open(TH_CMD_PATH, 0x1 | 0x40 | 0x200);
            if (fd2 >= 0) {
                char c[32]; int q = 0;
                const char *v = "DOCKOPACTEST "; for (const char *z = v; *z; z++) c[q++] = *z;
                q += th_int(c + q, want); c[q++] = 10;
                sys_write(fd2, c, (unsigned long)q); sys_close(fd2);
            }
        }
        if (reps == 200) {
            char l[96]; int p = 0;
            const char *t = "T+200 baseline g_dock_opacity=";
            for (const char *z = t; *z; z++) l[p++] = *z;
            p += th_int(l + p, g_dock_opacity);
            const char *t2 = " -> writing CFG="; for (const char *z = t2; *z; z++) l[p++] = *z;
            p += th_int(l + p, want);
            l[p] = 0; th_log(l);
            dock_opacity_write_cfg(want);
            return;
        }
        if (reps == 260 || reps == 400 || reps == 599) {
            char l[96]; int p = 0;
            const char *t = "T+"; for (const char *z = t; *z; z++) l[p++] = *z;
            p += th_int(l + p, reps);
            const char *t2 = " g_dock_opacity="; for (const char *z = t2; *z; z++) l[p++] = *z;
            p += th_int(l + p, g_dock_opacity);
            l[p] = 0; th_log(l);
        }
        return;
    }

    if (strcmp(verb, "DOCKOPAC") == 0) {
        // Write /CONFIG/DOCKOPAC.CFG exactly as the Settings app does, from a
        // point in time when the compositor is already RUNNING. That ordering
        // is the whole point: compositor_init() re-seeds this file from the
        // loaded profile, so a value written while the desktop is DOWN is
        // correctly discarded (same semantics dock_style has had since #387).
        // Writing it here exercises the real live path: poll -> apply ->
        // profile_tick hash -> profile_save.
        extern void dock_opacity_write_cfg(int v);
        int v = th_atoi(arg);
        char l[80]; int p = 0;
        const char *t = "DOCKOPAC before g_dock_opacity=";
        for (const char *q = t; *q; q++) l[p++] = *q;
        p += th_int(l + p, g_dock_opacity);
        const char *t2 = " writing="; for (const char *q = t2; *q; q++) l[p++] = *q;
        p += th_int(l + p, v);
        l[p] = 0; th_log(l);
        dock_opacity_write_cfg(v);
        // Re-arm the hook so a LATER frame reports what the poll actually did.
        // testhook_poll() consumes TESTHOOK.CMD, so writing a new one here is
        // the only way to sequence two observations without an interactive
        // shell (the VM's serial is a log stream and its network is degraded).
        { int fd = sys_open(TH_CMD_PATH, 0x1 | 0x40 | 0x200);
          if (fd >= 0) { sys_write(fd, "DOCKOPACCHECK\n", 14); sys_close(fd); } }
        th_log("OK DOCKOPAC");
        return;
    }

    if (strcmp(verb, "DOCKOPACCHECK") == 0) {
        // Re-arm ourselves so this becomes a TIME SERIES, not one sample. A
        // single reading taken one frame after the write proves nothing: the
        // poll only runs every 10th loop iteration, so the first sample is
        // expected to be stale. What matters is whether it EVER changes.
        static int reps = 0;
        if (reps < 400) {
            reps++;
            int fd2 = sys_open(TH_CMD_PATH, 0x1 | 0x40 | 0x200);
            if (fd2 >= 0) { sys_write(fd2, "DOCKOPACCHECK\n", 14); sys_close(fd2); }
        }
        if (reps != 1 && reps != 20 && reps != 60 && reps != 150 && reps != 300 && reps != 400)
            return;
        char l[96]; int p = 0;
        p += th_int(l + p, reps); l[p++] = ' ';
        const char *t = "DOCKOPACCHECK g_dock_opacity=";
        for (const char *q = t; *q; q++) l[p++] = *q;
        p += th_int(l + p, g_dock_opacity);
        // Read the file back through the SAME pair the poll uses, so a path
        // mismatch between writer and reader shows up here as a differing value.
        int fd = userconf_open_read("DOCKOPAC.CFG", "/DOCKOPAC.CFG");
        const char *t2 = " cfg_read="; for (const char *q = t2; *q; q++) l[p++] = *q;
        if (fd < 0) { const char *e = "OPENFAIL"; for (const char *q = e; *q; q++) l[p++] = *q; }
        else {
            char b[8]; long n2 = sys_read(fd, b, 7); sys_close(fd);
            if (n2 <= 0) { const char *e = "EMPTY"; for (const char *q = e; *q; q++) l[p++] = *q; }
            else for (long i = 0; i < n2; i++) l[p++] = b[i];
        }
        l[p] = 0; th_log(l);
        return;
    }

    if (strcmp(verb, "GLASSALL") == 0) {
        th_glass_all(arg[0] ? th_atoi(arg) : 200);
        th_log("OK GLASSALL");
        return;
    }

    if (strcmp(verb, "GLASSBENCH") == 0) {
        th_glass_bench(arg[0] ? th_atoi(arg) : 100);
        th_log("OK GLASSBENCH");
        return;
    }

    if (strcmp(verb, "GLASSPROBE") == 0) {
        // arg: "WHITE" or "BLACK", optionally followed by an opacity to set.
        int white = (arg[0] == 'W' || arg[0] == 'w');
        const char *sp = arg;
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        if (*sp) {
            int v = th_atoi((char *)sp);
            // (#132) was a hard `v >= 70`, silently dropping any lower value
            // a verification run tried to probe with - it would have hidden
            // the very regression this test hook exists to catch.
            if (v >= DOCK_OPACITY_MIN && v <= DOCK_OPACITY_MAX) g_dock_opacity = v;
        }
        th_glass_probe(white);
        th_log("OK GLASSPROBE");
        return;
    }

    if (strcmp(verb, "GLASSTHEME") == 0) {
        if (arg[0] == '\0') { th_log("ERR GLASSTHEME needs a slug"); return; }
        int idx = gui_theme_activate(arg);
        if (idx < 0) { th_log("ERR GLASSTHEME activate failed"); return; }
        compositor_apply_theme(idx);
        g_needs_redraw = true;
        th_log("OK GLASSTHEME");
        return;
    }

    if (strcmp(verb, "GLASSDOWNGRADE") == 0) {
        // Force the tier-2 auto-downgrade by setting its measured-ms threshold
        // to 0, so the very next cold recompute trips it. This proves the
        // mechanism FIRES, rather than asserting that it would.
        g_glass_downgrade_ms = arg[0] ? th_atoi(arg) : 0;
        glass_invalidate_all();
        g_needs_redraw = true;
        th_log("OK GLASSDOWNGRADE");
        return;
    }

    if (strcmp(verb, "GLASSSTAT") == 0) {
        th_glass_stat("manual");
        th_log("OK GLASSSTAT");
        return;
    }

    if (strcmp(verb, "SPAWNLOOP") == 0) {
        // SPAWNLOOP <count> <periodms> <path> [arg]
        char *t = arg, *f[5]; int nf = 0;
        while (*t && nf < 5) {
            while (*t == ' ') t++;
            if (!*t) break;
            f[nf++] = t;
            while (*t && *t != ' ') t++;
            if (*t) *t++ = '\0';
        }
        if (nf < 4) { th_log("ERR SPAWNLOOP needs <count> <periodms> <kill01> <path> [arg]"); return; }
        if (s_sl_armed) return;   // silent: a re-fire must not restart the run
        s_sl_armed = 1;
        s_sl_left = th_atoi(f[0]);
        s_sl_period = th_atoi(f[1]);
        if (s_sl_period < 250) s_sl_period = 250;
        s_sl_kill = th_atoi(f[2]);
        { int i = 0; for (; f[3][i] && i < 95; i++) s_sl_path[i] = f[3][i]; s_sl_path[i] = 0; }
        { int i = 0; if (nf == 5) for (; f[4][i] && i < 63; i++) s_sl_arg[i] = f[4][i]; s_sl_arg[i] = 0; }
        s_sl_iter = 0; s_sl_prev_pid = 0;
        // Do NOT start spawning the instant the desktop appears: boot is still
        // bringing up audio, DHCP and cron for tens of seconds afterwards, and a
        // launch inside that window measures a boot race rather than the steady
        // state a real user launches in.
        s_sl_next_ms = uptime_ms() + 30000;
        th_logf("OK SPAWNLOOP n=%d period=%d kill=%d path=%s arg=%s", s_sl_left, s_sl_period, s_sl_kill, s_sl_path, s_sl_arg);
        return;
    }

    if (strcmp(verb, "LAUNCHARG") == 0) {
        // LAUNCHARG <path> <arg>  - spawn with one argv[1], via the same
        // sys_spawn_args() the shell and Files "Open with" already use.
        char *sp = arg;
        while (*sp && *sp != ' ') sp++;
        if (*sp == ' ') *sp++ = '\0';
        if (arg[0] == '\0') { th_log("ERR LAUNCHARG needs a path"); return; }
        char *av[2]; av[0] = arg; av[1] = sp;
        sys_spawn_args(arg, av, 2);
        th_log("OK LAUNCHARG");
        return;
    }

    // ======================================================================
    // #123 marble dock verbs.
    //
    // WHAT THESE PROVE AND DO NOT PROVE, same honesty rule as the file header:
    //   DOCKINFO  - prints the dock's real geometry (effective height, tile,
    //               gutter, pane width, the 75% budget, every slot x) to SERIAL,
    //               live. No screenshot can report a number, and "measure it,
    //               do not assert it" needs a number.
    //   DOCKH/DOCKZ - write the SAME /CONFIG CFG files the Settings sliders
    //               write, so they exercise the real poll -> apply -> persist
    //               channel end to end, not a private setter. What they do NOT
    //               exercise is the Settings slider's own hit test; that is what
    //               the VNC client is for.
    //   DOCKCLICK - enters taskbar_handle_mouse() at real screen coordinates,
    //               i.e. the REAL hit test, unlike ICON/MENUITEM. It bypasses
    //               only the mouse DRIVER, not the geometry.
    //   DOCKMOUSE - moves the compositor's notion of the pointer so a hover
    //               magnify can be screenshotted deterministically (QEMU
    //               relative-mouse injection cannot place a pointer, #334).
    // ======================================================================
    if (strcmp(verb, "DOCKINFO") == 0) {
        taskbar_dock_debug_dump(1);
        th_log("OK DOCKINFO");
        return;
    }

    if (strcmp(verb, "DOCKH") == 0) {
        extern void dock_height_write_cfg(int v);
        dock_height_write_cfg(th_atoi(arg));
        th_log("OK DOCKH");
        return;
    }

    if (strcmp(verb, "DOCKZ") == 0) {
        extern void dock_zoom_write_cfg(int v);
        dock_zoom_write_cfg(th_atoi(arg));
        th_log("OK DOCKZ");
        return;
    }

    // (#132) DOCKOP <pct> - same shape as DOCKH/DOCKZ above: writes
    // /CONFIG/DOCKOPAC.CFG through dock_opacity_write_cfg(), the SAME
    // function the Settings opacity slider calls, so this exercises the real
    // write -> poll -> apply -> persist channel end to end, not a private
    // setter (that is what GLASSPROBE's optional trailing number already is -
    // it pokes g_dock_opacity directly and exists to probe glass_render() in
    // isolation, not to stand in for this). Added because #132 removed the
    // hard 70% floor and the only way to prove a value below it is actually
    // honoured, not just accepted by one function, is to drive it through the
    // full channel a screendump can then show.
    if (strcmp(verb, "DOCKOP") == 0) {
        extern void dock_opacity_write_cfg(int v);
        dock_opacity_write_cfg(th_atoi(arg));
        th_log("OK DOCKOP");
        return;
    }

    // #opacityglass verification-only verb: WINOPACITY <0-100> drives the
    // SAME channel the tray quick-settings slider uses (traymenu.c's
    // win_opacity setter: set g_win_opacity, call set_win_opacity()), which
    // is the real global-default window opacity syscall (SYS_SET_WIN_OPACITY)
    // every open window's win->opacity is seeded/overwritten from - not a
    // private setter. This exists to let a headless run set window
    // translucency without a coordinate-accurate mouse click on the tray
    // menu, so the stable-glass fix (any translucent window forces the full
    // render_frame() path - see main.c's _any_translucent) can be verified
    // by screendump alone. Never shipped: gated the same as every other verb
    // in this file.
    if (strcmp(verb, "WINOPACITY") == 0) {
        extern int g_win_opacity;
        int pct = th_atoi(arg);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        int o = pct * 255 / 100;
        if (o < 40) o = 40;
        if (o > 255) o = 255;
        g_win_opacity = o;
        set_win_opacity(o);
        th_logf("OK WINOPACITY %d", pct);
        return;
    }

    // Write the CFG, do NOT call taskbar_set_style() directly. MEASURED: a
    // direct call is reverted within ~10 frames, because dock_style_poll()
    // reads /CONFIG/DOCKSTYL.CFG on main.c's poll cadence and re-applies
    // whatever is in the file. The first attempt at the style-regression pass
    // produced five screendumps that all showed the marble dock for exactly
    // this reason.
    if (strcmp(verb, "DOCKSTYLE") == 0) {
        extern void dock_style_write_cfg(int v);
        dock_style_write_cfg(th_atoi(arg));
        g_needs_redraw = true;
        th_log("OK DOCKSTYLE");
        return;
    }

    // Release the #123 pointer pin so the real mouse takes over again.
    if (strcmp(verb, "HOLD") == 0) {
        g_seq_extra_hold = th_atoi(arg);
        th_log("OK HOLD");
        return;
    }

    if (strcmp(verb, "DOCKMOUSEFREE") == 0) {
        g_th_mouse_pinned = 0;
        th_log("OK DOCKMOUSEFREE");
        return;
    }

    if (strcmp(verb, "DOCKCLICK") == 0) {
        // "x y"
        int x = th_atoi(arg);
        const char *sp = arg;
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        int y = th_atoi(sp);
        taskbar_dock_debug_click(x, y);
        th_log("OK DOCKCLICK");
        return;
    }

    // Address a slot by INDEX instead of by pixel, so a pre-written sequence
    // does not have to predict the framebuffer width or the auto-scaled
    // geometry. The resolved pixel coordinates are printed and then fed
    // through the SAME taskbar_handle_mouse() a real click takes.
    if (strcmp(verb, "DOCKCLICKSLOT") == 0) {
        int32_t x = 0, y = 0;
        if (!taskbar_dock_slot_point(th_atoi(arg), &x, &y)) { th_log("ERR DOCKCLICKSLOT no such slot"); return; }
        taskbar_dock_debug_click(x, y);
        th_log("OK DOCKCLICKSLOT");
        return;
    }

    if (strcmp(verb, "DOCKMOUSESLOT") == 0) {
        int32_t x = 0, y = 0;
        if (!taskbar_dock_slot_point(th_atoi(arg), &x, &y)) { th_log("ERR DOCKMOUSESLOT no such slot"); return; }
        g_th_mouse_pinned = 1;
        g_mouse_x = x; g_mouse_y = y;
        g_needs_redraw = true;
        { char b[96]; int q = 0;
          const char *k = "[DOCK123] HOVER slot ";
          for (const char *z = k; *z; z++) b[q++] = *z;
          q += th_int(b + q, th_atoi(arg));
          const char *k2 = " at "; for (const char *z = k2; *z; z++) b[q++] = *z;
          q += th_int(b + q, x); b[q++] = ','; q += th_int(b + q, y);
          // The zoom percent goes in the SAME banner the host watcher triggers
          // on, so every hover screendump is self-labelling from the serial log
          // instead of being matched to a step number by timing (which is what
          // made run 2's zoom captures unusable - the watcher's fixed delay
          // does not track a step whose hold is longer than the delay).
          const char *k3 = " zoom="; for (const char *z = k3; *z; z++) b[q++] = *z;
          q += th_int(b + q, g_dock_zoom); b[q++] = 10;
          write(1, b, (unsigned long)q); }
        th_log("OK DOCKMOUSESLOT");
        return;
    }

    // Trim the favourites list to exactly <n> entries by unpinning from the
    // tail, through startmenu_toggle_favorite_path() - the SAME writer the
    // dock's own right-click "Unpin from Favorites" and the Settings Remove
    // button use, so this is the real removal path and it persists through the
    // real sm_save_state(). Exists so the auto-scale RECOVERY case (item count
    // drops -> the dock returns to the user's preferred height) is one script
    // step instead of twenty-two.
    if (strcmp(verb, "DOCKPINS") == 0) {
        int want = th_atoi(arg);
        if (want < 0) want = 0;
        for (int guard = 0; guard < 64; guard++) {
            sm_fav_info_t f[64];
            int n = startmenu_get_favorites(f, 64);
            if (n <= want) break;
            startmenu_toggle_favorite_path(f[n - 1].exec_path);
        }
        { sm_fav_info_t f[64];
          int n = startmenu_get_favorites(f, 64);
          char b[64]; int q = 0;
          const char *k = "[DOCK123] PINS now ";
          for (const char *z = k; *z; z++) b[q++] = *z;
          q += th_int(b + q, n); b[q++] = 10;
          write(1, b, (unsigned long)q); }
        g_needs_redraw = true;
        th_log("OK DOCKPINS");
        return;
    }

    if (strcmp(verb, "DOCKMOUSE") == 0) {
        int x = th_atoi(arg);
        const char *sp = arg;
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        int y = th_atoi(sp);
        g_th_mouse_pinned = 1;
        g_mouse_x = x; g_mouse_y = y;
        g_needs_redraw = true;
        th_log("OK DOCKMOUSE");
        return;
    }

    // Run /DOCK123.SEQ, one line per invocation, re-arming itself between
    // lines. The compositor has no shell and its serial is a one-way log, so a
    // multi-step verification run has to come from a file placed on the image
    // BEFORE boot. Each step announces itself on serial, which is what a host
    // watcher synchronises its QMP screendumps against - a real handshake, not
    // a guessed sleep. `#` comments and blank lines are skipped.
    if (strcmp(verb, "SEQ") == 0) {
        static int line_no = 0;
        static int delay = 0;
        g_seq_running = 1;
        if (arg[0] >= '0' && arg[0] <= '9') line_no = th_atoi(arg);
        // Pace: hold each step for SEQ_HOLD polls before advancing, so the host
        // has a stable frame to capture.
        #define SEQ_HOLD 90
        if (g_seq_extra_hold > 0) { delay += g_seq_extra_hold; g_seq_extra_hold = 0; }
        if (delay > 0) {
            delay--;
            int fd2 = sys_open(TH_CMD_PATH, 0x1 | 0x40 | 0x200);
            if (fd2 >= 0) { sys_write(fd2, "SEQ\n", 4); sys_close(fd2); }
            return;
        }
        int fd = sys_open("/DOCK123.SEQ", 0);
        if (fd < 0) { th_log("ERR SEQ no /DOCK123.SEQ"); return; }
        static char sq[4096];
        long n = sys_read(fd, sq, sizeof(sq) - 1);
        sys_close(fd);
        if (n <= 0) { th_log("ERR SEQ empty"); return; }
        sq[n] = 0;
        // Find line `line_no` (counting only non-blank, non-comment lines).
        char *p = sq; int idx = 0; char *pick = 0;
        while (*p) {
            char *ls = p;
            while (*p && *p != '\n') p++;
            if (*p) *p++ = 0;
            char *t = ls;
            while (*t == ' ' || *t == '\t') t++;
            if (*t == 0 || *t == '#') continue;
            if (idx == line_no) { pick = t; break; }
            idx++;
        }
        if (!pick) {
            char e[64]; int q = 0;
            const char *k = "[DOCK123] SEQ END at line ";
            for (const char *z = k; *z; z++) e[q++] = *z;
            q += th_int(e + q, line_no); e[q++] = 10;
            write(1, e, (unsigned long)q);
            return;
        }
        { char e[160]; int q = 0;
          const char *k = "[DOCK123] STEP ";
          for (const char *z = k; *z; z++) e[q++] = *z;
          q += th_int(e + q, line_no); e[q++] = ' ';
          for (const char *z = pick; *z && q < 150; z++) e[q++] = *z;
          e[q++] = 10;
          write(1, e, (unsigned long)q); }
        // Execute the picked line by re-entering this dispatcher's body: write
        // it as the pending command, then queue ourselves for the NEXT line.
        { int fd2 = sys_open(TH_CMD_PATH, 0x1 | 0x40 | 0x200);
          if (fd2 >= 0) {
              char c[192]; int q = 0;
              for (const char *z = pick; *z && q < 180; z++) c[q++] = *z;
              c[q++] = 10;
              sys_write(fd2, c, (unsigned long)q); sys_close(fd2);
          } }
        line_no++;
        delay = SEQ_HOLD;
        return;
    }

    if (strcmp(verb, "LAUNCH") == 0) {
        if (arg[0] == '\0') { th_log("ERR LAUNCH needs a path"); return; }
        sys_spawn(arg);
        th_log("OK LAUNCH");
        return;
    }

    // #dosfspacing verification-only verb: DOSRUN <path> - the exact call the
    // Start menu makes to launch a bundled DOS game (dos_run(), SYS_DOS_RUN,
    // startmenu.c LAUNCH_DOS case), used here instead of a generic LAUNCH so
    // the verification exercises a REAL DOS host window (mode-13h/text video,
    // a real kernel dos_run() proc) rather than depending on any particular
    // app's own internal wrapping. Non-blocking (kernel spawns a dedicated
    // proc + host window per its own doc comment), so this cannot stall the
    // draw thread (#426). Never shipped: gated the same as every other verb.
    if (strcmp(verb, "DOSRUN") == 0) {
        if (arg[0] == '\0') { th_log("ERR DOSRUN needs a path"); return; }
        int r = dos_run(arg);
        th_log(r == 0 ? "OK DOSRUN" : "ERR DOSRUN failed");
        return;
    }

    // #dosfspacing verification-only verbs: drive #158 native fullscreen
    // enter/exit DIRECTLY, the SAME kernel entry point Alt+Enter uses
    // (SYS_WM_FULLSCREEN_ENTER/EXIT - kernel/proc/syscall.c), sidestepping
    // raw scancode injection. sys_wm_fullscreen_enter()'s own permission
    // check explicitly allows this: "the COMPOSITOR itself... a second entry
    // point to one Ring-0 already grants unconditionally to itself" - this
    // process (compositor + testhook) IS the framebuffer owner. Needed
    // because a testhook-launched DOS window has never been proven to
    // reliably receive raw scancodes (dosfullscreen commit's own caveat), so
    // this is the sanctioned way to verify the fast path end-to-end without
    // that dependency. Never shipped: gated the same as every other verb
    // here. Acts on window_get_focused() - LAUNCH (or a real click) must put
    // the target window in focus first.
    if (strcmp(verb, "FSENTER") == 0) {
        int r = sys_wm_fullscreen_enter();
        th_log(r == 0 ? "OK FSENTER" : "ERR FSENTER failed");
        return;
    }
    if (strcmp(verb, "FSEXIT") == 0) {
        sys_wm_fullscreen_exit();
        th_log("OK FSEXIT");
        return;
    }

    // (#dosfsmax) Verification-only verb: drive the MAXIMIZE gesture on the
    // focused window through the SAME kernel entry point the taskbar/
    // context-menu "Maximize/Restore" item uses (SYS_WM_MAXIMIZE_WINDOW ->
    // wm_toggle_maximize_focused()), rather than a coordinate-accurate click
    // on the titlebar's maximize button or its double-click detector (#334 -
    // headless mouse does not land reliably, and while a window holds #158
    // native fullscreen the kernel suppresses ALL chrome, including any
    // titlebar to click on, so a real click could not reach a restore either
    // way). This is a TOGGLE, exactly like FSENTER/FSEXIT's own real button:
    // called on a focused DOS-hosted window it now enters #158 native
    // fullscreen (window_maximize()'s DOS routing, kernel/gui/window.c);
    // called again it exits fullscreen back to the pre-maximize windowed
    // bounds (wm_toggle_maximize_focused()'s fullscreen-exit check, same
    // file). Called on a focused NORMAL (non-DOS) window it is the ordinary,
    // unchanged maximize/restore toggle - proves no-regression for that case
    // from the same verb. DOSRUN (above) leaves its window focused, so no
    // separate focus step is needed for the DOS case. Never shipped: gated
    // the same as every other verb in this file.
    if (strcmp(verb, "MAXIMIZE") == 0) {
        int r = sys_wm_maximize_focused();
        th_log(r == 0 ? "OK MAXIMIZE" : "ERR MAXIMIZE failed");
        return;
    }

    // #223 ROUND 2 verification-only verb: close a window the SAME way a real
    // titlebar-X click does, by finding it via wm_get_windows() and driving
    // taskbar_close_window() (the existing synthetic-click closer #44 already
    // uses for the dock's own "Close" context-menu action - no new close
    // mechanism, just a way to name a target without a coordinate-accurate
    // mouse click, same "sidestep hit-testing" philosophy as MENUITEM/ICON).
    // Matches by app_id (kernel-resolved binary basename, #41) case-
    // insensitively, exact match preferred; falls back to a case-insensitive
    // SUBSTRING of the window title if no app_id matches (some windows have
    // no app_id - see wm_window_info_t's own comment). First match wins.
    // Never shipped: gated the same as every other verb in this file.
    if (strcmp(verb, "WINCLOSE") == 0) {
        if (arg[0] == '\0') { th_log("ERR WINCLOSE needs a name"); return; }
        wm_window_info_t wins[16];
        int n = wm_get_windows(wins, 16);
        if (n < 0) n = 0;
        int target = -1;
        for (int i = 0; i < n && target < 0; i++) {
            if (wins[i].app_id[0] == '\0') continue;
            int j = 0;
            for (; arg[j] && wins[i].app_id[j]; j++) {
                char a = arg[j], b = wins[i].app_id[j];
                if (a >= 'a' && a <= 'z') a = (char)(a - 32);
                if (b >= 'a' && b <= 'z') b = (char)(b - 32);
                if (a != b) break;
            }
            if (arg[j] == '\0' && wins[i].app_id[j] == '\0') target = wins[i].id;
        }
        if (target < 0) {
            for (int i = 0; i < n && target < 0; i++) {
                const char *t = wins[i].title, *want = arg;
                for (const char *h = t; *h; h++) {
                    const char *hh = h, *nn = want;
                    while (*hh && *nn) {
                        char a = *hh, b = *nn;
                        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                        if (a != b) break;
                        hh++; nn++;
                    }
                    if (!*nn) { target = wins[i].id; break; }
                }
            }
        }
        if (target < 0) { th_log("ERR WINCLOSE not found"); return; }
        taskbar_close_window(target);
        th_log("OK WINCLOSE");
        return;
    }

    // #wpanim verification-only verb: move a window by name to an ABSOLUTE
    // (x,y), by app_id (falling back to a title substring) - the exact same
    // matching WINCLOSE above uses, so this needs no new lookup mechanism.
    // Drives SYS_WM_SET_BOUNDS (#404, compositor-only) directly, keeping the
    // window's own current width/height so this is a pure move, not a
    // resize. Exists purely to make the animated-wallpaper "field deforms as
    // a window moves" proof reproducible without coordinate-accurate mouse
    // drag (#334) - a real drag would exercise the same wm_set_bounds() path
    // a titlebar drag already calls, just via a different entry point, so
    // this does not test anything a real drag would not also exercise.
    // Format: "WINPOS <name> <x> <y>". Never shipped (TESTHOOK gate).
    if (strcmp(verb, "WINPOS") == 0) {
        char name[32]; int ni = 0;
        const char *sp = arg;
        while (*sp && *sp != ' ' && ni < (int)sizeof(name) - 1) name[ni++] = *sp++;
        name[ni] = '\0';
        while (*sp == ' ') sp++;
        int32_t nx = th_atoi(sp);
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        int32_t ny = th_atoi(sp);

        if (name[0] == '\0') { th_log("ERR WINPOS needs a name"); return; }
        wm_window_info_t wins[16];
        int n = wm_get_windows(wins, 16);
        if (n < 0) n = 0;
        int target = -1, tw = 0, th_ = 0;
        for (int i = 0; i < n && target < 0; i++) {
            if (wins[i].app_id[0] == '\0') continue;
            int j = 0;
            for (; name[j] && wins[i].app_id[j]; j++) {
                char a = name[j], b = wins[i].app_id[j];
                if (a >= 'a' && a <= 'z') a = (char)(a - 32);
                if (b >= 'a' && b <= 'z') b = (char)(b - 32);
                if (a != b) break;
            }
            if (name[j] == '\0' && wins[i].app_id[j] == '\0') { target = wins[i].id; tw = wins[i].width; th_ = wins[i].height; }
        }
        if (target < 0) {
            for (int i = 0; i < n && target < 0; i++) {
                const char *t = wins[i].title, *want = name;
                for (const char *h = t; *h; h++) {
                    const char *hh = h, *nn = want;
                    while (*hh && *nn) {
                        char a = *hh, b = *nn;
                        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                        if (a != b) break;
                        hh++; nn++;
                    }
                    if (!*nn) { target = wins[i].id; tw = wins[i].width; th_ = wins[i].height; break; }
                }
            }
        }
        if (target < 0) { th_log("ERR WINPOS not found"); return; }
        int r = wm_set_bounds(target, nx, ny, tw, th_, 0);
        g_needs_redraw = true;
        th_logf("OK WINPOS %s x=%d y=%d r=%d", name, (int)nx, (int)ny, r);
        return;
    }

    // Verification-only verb: open Settings straight to a named panel via the
    // real settings_open_panel() singleton-safe path (same one the tray/
    // context-menu/widget shortcuts use), so a specific panel (e.g. Network,
    // #144) can be screenshotted without navigating the sidebar by mouse.
    if (strcmp(verb, "PANEL") == 0) {
        if (arg[0] == ' ') { th_log("ERR PANEL needs a number"); return; }
        settings_open_panel(th_atoi(arg));
        th_log("OK PANEL");
        return;
    }

    // #151 verification-only verb: trigger an explicit session lock the same
    // way Start Menu / Super+L do (lock_enter() -> sys_session_lock()), so a
    // throwaway TESTHOOK build can screenshot the lock overlay without
    // needing a real idle timeout or a landed mouse click. Never shipped:
    // gated identically to every other verb in this file.
    if (strcmp(verb, "LOCK") == 0) {
        lock_enter();
        th_log("OK LOCK");
        return;
    }

    if (strcmp(verb, "APPLOCK") == 0) {
        if (arg[0] == ' ') { th_log("ERR APPLOCK needs a path"); return; }
        sys_spawn(arg);
        s_th_lock_at_ms = uptime_ms() + 2500;
        th_log("OK APPLOCK");
        return;
    }

    if (strcmp(verb, "PANELLOCK") == 0) {
        if (arg[0] == ' ') { th_log("ERR PANELLOCK needs a number"); return; }
        settings_open_panel(th_atoi(arg));
        s_th_lock_at_ms = uptime_ms() + 1500;
        th_log("OK PANELLOCK");
        return;
    }

    if (strcmp(verb, "SAVER") == 0) {
        if (arg[0] == '\0') { th_log("ERR SAVER needs a type number"); return; }
        screensaver_set_type(th_atoi(arg));
        // #560: screensaver_set_type() only changes WHICH effect is
        // selected; g_screensaver_active is still gated by
        // screensaver_check_timeout()'s real elapsed-idle-time check
        // (#652: SS_DEFAULT_TIMEOUT is 600s/10min now, but that is only a
        // fallback guard - the LIVE default a fresh boot actually waits on
        // is the kernel's own g_screensaver_delay, kernel/proc/syscall.c,
        // unchanged this session), same as a live idle timeout or the
        // Settings "Test Screensaver" button (SYS_SCREENSAVER_TEST ->
        // get_ss_test(), main.c). Force it here too so SAVER truly force-
        // activates instantly with no idle wait, matching what this hook is
        // documented to do.
        g_screensaver_active = true;
        screensaver_note_activated();   // #570: starts the input-ignore grace
        g_needs_redraw = true;
        th_log("OK SAVER");
        return;
    }

    // (#glassmodal) verification-only: open a power confirm dialog by action
    // number (1=Shut Down, 2=Restart, 3=Log Out, 4=Lock), bypassing the
    // power-grid icon's mouse hit-test (#334/#440). Used to screenshot the
    // glass confirm-dialog port without needing a landed click.
    if (strcmp(verb, "POWERCONFIRM") == 0) {
        if (arg[0] == '\0') { th_log("ERR POWERCONFIRM needs 1-4"); return; }
        startmenu_test_power_confirm(th_atoi(arg));
        th_log("OK POWERCONFIRM");
        return;
    }

    // (#shutdlg) verification-only: click at "x y" straight into the open
    // power confirm dialog's REAL hit-test (startmenu_power_confirm_handle_
    // mouse -> cd_geom()), the same one a real mouse click reaches. This is
    // deliberately NOT a bypass, unlike POWERCONFIRM above - the whole point
    // is proving the button rects a real click would hit, since #440's QMP
    // mouse cannot reliably land on a compositor-drawn target and this
    // dialog's buttons are destructive (Cancel must never resolve to
    // Shut Down). Logs [PCCLICK] consumed=0|1 on serial.
    if (strcmp(verb, "PCCLICK") == 0) {
        int x = th_atoi(arg);
        const char *sp = arg;
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        int y = th_atoi(sp);
        startmenu_test_power_confirm_click(x, y);
        th_log("OK PCCLICK");
        return;
    }

    // (#shutdlg) Convenience composite for a single-boot verification pass:
    // "action x y" opens the power confirm dialog for `action` (same as
    // POWERCONFIRM), then ARMS a click at (x,y) to fire ~400ms later (past
    // CONFIRM_SETTLE_MS) via s_th_pcclick_at_ms above, so a real
    // Cancel-vs-confirm-button hit-test can be proven without a second boot
    // (see DEMO127's file-top rationale: a throwaway VM disk cannot be
    // re-written with a second /TESTHOOK.CMD while qemu holds it open).
    if (strcmp(verb, "PCTEST") == 0) {
        // (#shutdlg) MEASURED this session: an offline-baked TESTHOOK.CMD
        // (written to the disk image before boot, as this composite verb
        // always is - see the DEMO127 rationale above) is NOT reliably
        // consumed by the truncate+unlink above, so this verb can be
        // re-dispatched every single poll. A one-shot guard makes that safe:
        // without it, a repeat dispatch would re-open the dialog and push
        // the deferred click's deadline forward every frame, so the click
        // would never fire.
        static int s_pctest_done = 0;
        if (s_pctest_done) return;
        s_pctest_done = 1;
        int action = th_atoi(arg);
        const char *sp = arg;
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        int x = th_atoi(sp);
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        int y = th_atoi(sp);
        startmenu_test_power_confirm(action);
        s_th_pcclick_x = x; s_th_pcclick_y = y;
        s_th_pcclick_at_ms = uptime_ms() + 400;
        th_log("OK PCTEST armed");
        return;
    }

    // (#glassmodal) verification-only: open the CPU/RAM/DSK/NET perf pop-out
    // by gauge index (0=CPU,1=RAM,2=DSK,3=NET), bypassing the gauge's mouse
    // hit-test.
    if (strcmp(verb, "PERFPOPUP") == 0) {
        taskbar_test_open_perf_popup(th_atoi(arg));
        th_log("OK PERFPOPUP");
        return;
    }

    // (#battpop) verification-only: click the tray battery icon through the
    // REAL taskbar_handle_mouse() hit test (toggles the info card open/shut,
    // same as a physical click). Logs consumed=0 if there was no battery
    // icon to hit (no battery present).
    if (strcmp(verb, "BATTCLICK") == 0) {
        int r = taskbar_test_click_battery_tray();
        char b[48]; int p = 0;
        const char *k = "[BATTCLICK] consumed="; for (const char *q = k; *q; q++) b[p++] = *q;
        p += th_int(b + p, r);
        b[p] = '\0';
        th_log(b);
        return;
    }

    // (#battpop) verification-only: print the battery info card's CURRENT
    // finalized rect, or "closed" if it is not open. A script calling this
    // on consecutive frames and diffing the printed rect is the direct
    // proof that the card is stationary (see g_bc_anchor_x's comment in
    // taskbar.c for the drift bug this replaced).
    if (strcmp(verb, "BATTRECT") == 0) {
        int32_t x = 0, y = 0, w = 0, h = 0;
        if (!taskbar_test_battery_card_rect(&x, &y, &w, &h)) {
            th_log("[BATTRECT] closed");
        } else {
            char b[96]; int p = 0;
            const char *k1 = "[BATTRECT] x="; for (const char *q = k1; *q; q++) b[p++] = *q;
            p += th_int(b + p, x);
            const char *k2 = " y="; for (const char *q = k2; *q; q++) b[p++] = *q;
            p += th_int(b + p, y);
            const char *k3 = " w="; for (const char *q = k3; *q; q++) b[p++] = *q;
            p += th_int(b + p, w);
            const char *k4 = " h="; for (const char *q = k4; *q; q++) b[p++] = *q;
            p += th_int(b + p, h);
            b[p] = '\0';
            th_log(b);
        }
        return;
    }

    if (strcmp(verb, "STARTMENU") == 0) {
        if (strcmp(arg, "OPEN") == 0) {
            if (!g_start_menu_open) startmenu_toggle();
            th_log("OK STARTMENU OPEN");
        } else if (strcmp(arg, "CLOSE") == 0) {
            if (g_start_menu_open) startmenu_toggle();
            th_log("OK STARTMENU CLOSE");
        } else if (strncmp(arg, "OPENCAT ", 8) == 0) {
            // #563: open the Start menu and a category's cascading flyout by
            // exact label, bypassing hit-testing - verifies the height-cap/
            // render logic (screenshot) without needing coordinate-accurate
            // mouse input.
            startmenu_open_category_by_name(arg + 8);
            th_log("OK STARTMENU OPENCAT");
        } else {
            th_log("ERR STARTMENU wants OPEN, CLOSE or OPENCAT <label>");
        }
        return;
    }

    if (strcmp(verb, "ICON") == 0) {
        if (arg[0] == '\0') { th_log("ERR ICON needs a name"); return; }
        if (desktop_launch_icon_by_name(arg)) th_log("OK ICON");
        else th_log("ERR ICON not found");
        return;
    }

    if (strcmp(verb, "MENUITEM") == 0) {
        if (arg[0] == '\0') { th_log("ERR MENUITEM needs a name"); return; }
        if (startmenu_launch_item_by_name(arg)) th_log("OK MENUITEM");
        else th_log("ERR MENUITEM not found");
        return;
    }

    // #131 (local 150) throwaway verification-only verb: launch two named
    // items in one shot (this hook has no way to queue a second /TESTHOOK.CMD
    // while the disk is exclusively held by a running VM in the throwaway
    // test harness). Names separated by '|', e.g. "OPENAB Settings|Terminal".
    // Never shipped: gated the same as every other verb in this file, by
    // MAYTERA_TESTHOOK / `make TESTHOOK=1`.
    if (strcmp(verb, "OPENAB") == 0) {
        char *sep = arg;
        while (*sep && *sep != '|') sep++;
        if (*sep != '|') { th_log("ERR OPENAB needs a|b"); return; }
        *sep = '\0';
        char *b = sep + 1;
        bool ok1 = startmenu_launch_item_by_name(arg);
        bool ok2 = startmenu_launch_item_by_name(b);
        th_log(ok1 && ok2 ? "OK OPENAB" : "ERR OPENAB one or both not found");
        return;
    }

    // #: Start-menu uplift verification verbs. Same "drive by name, sidestep
    // hit-testing" philosophy as MENUITEM/ICON above - these prove the search
    // filter / favorites-toggle / context-menu-open LOGIC is wired, not that a
    // real right-click/keystroke lands on the right pixel (see the file-top
    // comment and #334/#440 for that half of the picture).
    if (strcmp(verb, "STARTSEARCH") == 0) {
        // arg may be empty (clears the search / shows Favorites+Recent again).
        startmenu_set_search(arg);
        th_log("OK STARTSEARCH");
        return;
    }

    if (strcmp(verb, "MENUCTX") == 0) {
        if (arg[0] == '\0') { th_log("ERR MENUCTX needs a name"); return; }
        int idx = startmenu_find_item_by_name(arg);
        if (idx < 0) { th_log("ERR MENUCTX not found"); return; }
        // Fixed, reproducible anchor point (not the current cursor position)
        // so a screenshot is deterministic regardless of where the injected
        // mouse happens to be.
        contextmenu_open_for_menuitem(400, 300, idx);
        th_log("OK MENUCTX");
        return;
    }

    if (strcmp(verb, "MENUPIN") == 0) {
        if (arg[0] == '\0') { th_log("ERR MENUPIN needs a name"); return; }
        int idx = startmenu_find_item_by_name(arg);
        if (idx < 0) { th_log("ERR MENUPIN not found"); return; }
        startmenu_item_toggle_favorite(idx);
        th_log("OK MENUPIN");
        return;
    }

    // #223 rd2 GUARD VERIFICATION verb: force g_fav_count to 0 in memory with
    // NO legitimate write (simulates the exact glitch this investigation
    // chased but could not catch live), so the sm_save_recents_only() fix can
    // be proven to stop the NEXT launch/recents save from stamping that
    // glitch onto disk - independent of ever reproducing the real trigger.
    // Diagnostic-only, never shipped, gated the same as every other verb here.
    if (strcmp(verb, "FAVZERO") == 0) {
        startmenu_debug_force_fav_zero();
        th_log("OK FAVZERO");
        return;
    }

    // #223 rd3 HYPOTHESIS TEST verb: drives the REAL sm_record_recent() (via
    // startmenu_debug_record_recent()) with arg verbatim as the path, so a
    // path >=128 bytes can be recorded without needing a real app whose
    // exec_path happens to be that long. arg is not space-split (a real
    // path can't contain a space anyway in this tree's usage), so the whole
    // rest of the line after the verb is the path. Built with FAVDEBUG=1
    // this logs fav_count/canary state to serial + /FAVDEBUG.OUT on every
    // call via sm_favdebug_check() inside sm_record_recent() itself -
    // proof or refutation needs no separate accessor. Diagnostic-only,
    // never shipped, gated the same as every other verb here.
    if (strcmp(verb, "RECENTPUSH") == 0) {
        if (arg[0] == '\0') { th_log("ERR RECENTPUSH needs a path"); return; }
        startmenu_debug_record_recent(arg);
        th_log("OK RECENTPUSH");
        return;
    }

    // #223 rd3: single-shot version of the above that exercises every
    // sm_record_recent() branch (fill, already-full eviction, found>0
    // promote) with maximal-length paths, since one TESTHOOK.CMD can only
    // carry one verb. See startmenu_debug_recent_stress()'s own comment.
    if (strcmp(verb, "RECENTSTRESS") == 0) {
        startmenu_debug_recent_stress();
        th_log("OK RECENTSTRESS");
        return;
    }

    // #223 rd3b: coordinator-requested correction - RECENTSTRESS above ran
    // against g_fav_count==0 (this diagnostic VM never seeds favourites on
    // its own), so it could not have shown a nonzero-to-zero transition even
    // if one exists. This verb forces a real nonzero baseline (7 default
    // favourites, same as a real first boot) FIRST, confirmed via
    // sm_favdebug_check, then runs the identical long-path stress sequence.
    if (strcmp(verb, "SEEDSTRESS") == 0) {
        startmenu_debug_seed_and_stress();
        th_log("OK SEEDSTRESS");
        return;
    }

    // #127/#128/#129 verification-only verbs. Same "drive by name/number,
    // sidestep hit-testing" philosophy as MENUITEM/STARTMENU above - these
    // let a throwaway VM be screenshotted in every dock style and with a
    // controlled notification history WITHOUT fighting #334/#440 mouse
    // injection. They prove the RENDER/geometry logic, same caveat as the
    // rest of this file: they do NOT prove a real click at a tray icon's
    // actual screen coordinates reaches settings_open_panel() - that needs
    // the #440 VNC path (vnc.c) injecting a real PointerEvent.
    if (strcmp(verb, "DOCKSTYLE") == 0) {
        extern void dock_style_write_cfg(int v);   // main.c
        if (arg[0] == '\0') { th_log("ERR DOCKSTYLE needs 0-4"); return; }
        int s = th_atoi(arg);
        if (s < 0 || s >= DOCK_COUNT) { th_log("ERR DOCKSTYLE out of range"); return; }
        taskbar_set_style(s);
        dock_style_write_cfg(s);   // else dock_style_poll() reverts it in ~10 ticks
        g_needs_redraw = true;
        th_log("OK DOCKSTYLE");
        return;
    }

    if (strcmp(verb, "NOTIFCENTER") == 0) {
        notif_toggle_center();
        g_needs_redraw = true;
        th_log("OK NOTIFCENTER");
        return;
    }

    // arg is "sev|title|body", sev 0-3 (info/success/warning/error) matching
    // notif.c's NTF_* constants - same wire format the real spool file uses,
    // so this exercises the exact same push_notification() the spool poller
    // calls, not a parallel path.
    if (strcmp(verb, "NOTIFY") == 0) {
        char *p = arg, *title, *body;
        int sev = th_atoi(p);
        while (*p && *p != '|') p++;
        if (*p != '|') { th_log("ERR NOTIFY needs sev|title|body"); return; }
        *p++ = '\0'; title = p;
        while (*p && *p != '|') p++;
        if (*p != '|') { th_log("ERR NOTIFY needs sev|title|body"); return; }
        *p++ = '\0'; body = p;
        notif_test_push(sev, title, body);
        g_needs_redraw = true;
        th_log("OK NOTIFY");
        return;
    }

    // Convenience composite for a single-boot #127/#128/#129 verification
    // screenshot pass (a throwaway VM disk cannot be re-written with a
    // second /TESTHOOK.CMD while qemu holds it open, see the file-top
    // comment, so a multi-step scenario needs everything in ONE command).
    // Reuses the exact same primitives DOCKSTYLE/NOTIFY/NOTIFCENTER above
    // call - no separate logic path.
    if (strcmp(verb, "DEMO127") == 0) {
        extern void dock_style_write_cfg(int v);   // main.c
        int s = arg[0] ? th_atoi(arg) : DOCK_XFCE;
        if (s >= 0 && s < DOCK_COUNT) {
            taskbar_set_style(s);
            // dock_style_poll() re-applies from /CONFIG/DOCKSTYL.CFG every 10
            // ticks and would otherwise revert this in-memory change back to
            // whatever stale value Settings/boot last wrote there (that file
            // is the live IPC channel FROM Settings, and taskbar_set_style()
            // alone does not update it - only Settings' own apply path does).
            dock_style_write_cfg(s);
        }
        notif_test_push(0, "Background Update", "A background update finished successfully and needs a restart to apply the change.");
        notif_test_push(1, "Backup Complete", "Nightly backup finished without errors.");
        notif_test_push(2, "Low Disk Space", "Only 800MB remain on the root volume.");
        notif_test_push(3, "Network Fault", "Lost connection to the update server, retrying.");
        notif_toggle_center();
        g_needs_redraw = true;
        th_log("OK DEMO127");
        return;
    }

    // (#231) Verification-only verbs for the profile-persistence fix: toggle
    // the analog clock/calendar Lock flags and cycle the digital clock's
    // "Next design", via the EXACT SAME assignments the widget's own
    // right-click menu (widget_menu_handle(), widgets.c) makes when those
    // items are clicked. This proves the FIX under test - that the value now
    // survives profile_tick()'s change-detection and a reboot - not the
    // menu's own hit-test/geometry, the same scope every other verb in this
    // file keeps to (see the file-top comment on what this class of verb
    // does and does not prove).

    // (#231r) EQ verification, and it drives the REAL input path.
    //
    // "EQDRAG <band> <pos>" opens the sound tray panel, asks traymenu.c where
    // band <band>'s fader cap sits for position <pos> using the renderer's own
    // geometry, and then feeds that point to traymenu_handle_mouse() - the
    // same entry point a real click reaches. So a pass proves the hit-test
    // accepts the pixel the draw code puts the cap at, that snd_val_from_y()
    // inverts snd_cap_y() exactly at this UI scale, and that the drag ends in
    // eq_band_set() reaching the kernel's filter bank.
    //
    // #334 is why this exists: QEMU relative-mouse injection does not reliably
    // land where it is sent, so the geometry is computed here and the EVENT is
    // real rather than the other way round.
    if (strcmp(verb, "EQDRAG") == 0) {
        const char *p = arg;
        int band = th_atoi(p);
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        int pos = th_atoi(p);
        if (*p == '\0') { th_log("ERR EQDRAG needs <band> <pos>"); return; }

        extern int g_tray_bar_y;
        traymenu_open_for_icon(1 /* sound */, g_fb_width / 2);
        int fx = 0, fy = 0;
        if (traymenu_eq_fader_point(band, pos, &fx, &fy) != 0) {
            th_logf("ERR EQDRAG could not locate band %d (panel not open?)", band);
            traymenu_close();
            return;
        }
        int before = eq_band_get(band);
        // One press with held=false is a complete grab-set-release in
        // snd_mouse(): the fader loop claims the drag, the drag branch applies
        // the value, and the release branch logs it to /AUDIOLOG.TXT.
        traymenu_handle_mouse(fx, fy, true, false);
        int after = eq_band_get(band);
        th_logf("OK EQDRAG band=%d asked=%d point=(%d,%d) before=%d after=%d %s",
               band, pos, fx, fy, before, after,
               (after == pos) ? "HIT" : "<<<< MISS: the hit-test and the draw disagree");
        traymenu_close();
        g_needs_redraw = true;
        return;
    }

    // "EQSTATE" reports every band the kernel currently holds, so a reboot can
    // be asked what survived without a GUI.
    // "EQPANEL <p0> <p1> <p2> <p3> <p4>" sets all five bands and leaves the
    // sound tray panel OPEN, so a host-side screendump can show the restored
    // #336 faceplate with the faders at distinct, known positions. Purely for
    // producing a picture of the thing the ticket asked to be restored;
    // EQDRAG is what proves the input path.
    if (strcmp(verb, "EQPANEL") == 0) {
        const char *p = arg;
        int n = eq_band_count();
        for (int i = 0; i < n && i < 8; i++) {
            while (*p == ' ') p++;
            if (*p == '\0') break;
            eq_band_set(i, th_atoi(p));
            while (*p && *p != ' ') p++;
        }
        traymenu_open_for_icon(1 /* sound */, g_fb_width / 2);
        g_needs_redraw = true;
        th_logf("OK EQPANEL open, faders %d/%d/%d/%d/%d",
                eq_band_get(0), eq_band_get(1), eq_band_get(2),
                eq_band_get(3), eq_band_get(4));
        return;
    }

    if (strcmp(verb, "EQSTATE") == 0) {
        int n = eq_band_count();
        th_logf("OK EQSTATE bands=%d active=%d selftest=0x%x",
               n, eq_is_active(), eq_selftest_mask());
        for (int i = 0; i < n && i < 8; i++)
            th_logf("OK EQSTATE band%d %d Hz pos=%d gain=%d tenths-dB",
                   i, eq_band_freq(i), eq_band_get(i), eq_band_db10(i));
        return;
    }

    if (strcmp(verb, "WLOCK") == 0) {
        extern int g_clock_locked, g_cal_locked;   // widgets.c
        if (arg[0] == '\0') { th_log("ERR WLOCK needs 0 (clock) or 1 (calendar)"); return; }
        int which = th_atoi(arg);
        if (which == 0) g_clock_locked = !g_clock_locked;
        else if (which == 1) g_cal_locked = !g_cal_locked;
        else { th_log("ERR WLOCK 0 or 1 only"); return; }
        g_needs_redraw = true;
        th_log("OK WLOCK");
        return;
    }

    if (strcmp(verb, "WDESIGN") == 0) {
        extern int g_digclk_style;   // clock.c
        g_digclk_style = (g_digclk_style + 1) % 5;
        g_needs_redraw = true;
        th_log("OK WDESIGN");
        return;
    }

    // #keydrop verification-only verb: create a sticky note straight into
    // edit mode via sticky_new_at() (stickies.c), sidestepping the "right-
    // click desktop -> New Sticky Note" context-menu hit-test the same way
    // every other verb here sidesteps hit-testing. sticky_new_at() itself
    // sets s_edit to the new note, which is what routes subsequently-typed
    // keys to stickies_handle_key() via the g_modal_grabs[] "sticky-editor"
    // row in main.c - exactly the class of compositor-native typing surface
    // the #keydrop dropped-keypress fix is about, so this is what lets a
    // burst of real KEY/TYPE injections (testinput.c, kernel/drivers/) be
    // driven straight at it without needing a landed mouse click. Default
    // position (400, 300) if no "x y" arg is given. Never shipped: gated
    // identically to every other verb in this file.
    if (strcmp(verb, "STICKY") == 0) {
        extern int sticky_new_at(int px, int py);   // stickies.c
        int px = 400, py = 300;
        if (arg[0] != '\0') {
            px = th_atoi(arg);
            const char *sp = arg;
            while (*sp && *sp != ' ') sp++;
            while (*sp == ' ') sp++;
            if (*sp) py = th_atoi(sp);
        }
        int idx = sticky_new_at(px, py);
        th_logf("OK STICKY idx=%d", idx);
        return;
    }

    // #keydrop verification-only verb: read back the in-memory text of
    // whichever note is currently being edited (stickies.c's s_edit), so a
    // burst of KEY/TYPE injections sent over the testinput.c serial channel
    // can be checked against what the compositor's g_modal_grabs[]
    // "sticky-editor" dispatch actually delivered - the exact mechanism the
    // #keydrop dropped-keypress fix changes. No disk round trip.
    if (strcmp(verb, "STICKYTXT") == 0) {
        extern int stickies_edit_index(void);
        extern int stickies_edit_len(void);
        extern const char *stickies_edit_text(void);
        int idx = stickies_edit_index();
        if (idx < 0) { th_log("ERR STICKYTXT no note editing"); return; }
        th_logf("OK STICKYTXT idx=%d len=%d text=%s",
                idx, stickies_edit_len(), stickies_edit_text());
        return;
    }

    // (cfrender) Cardfile deck verification verbs. Same shape and same
    // honesty rule as DOCKCLICK/DOCKMOUSE above (testhook.c's own file-
    // header note applies): these call cardfile_handle_mouse()/
    // cardfile_handle_key() directly at caller-given coordinates, which is
    // the REAL hit-test cardfile.c's process_events() dispatch line runs -
    // not a private setter, and not the by-name ICON/MENUITEM shortcut that
    // would skip the very geometry this feature is. Use DOCKSTYLE 5 first
    // to enter the Cardfile layout (writes /CONFIG/DOCKSTYL.CFG through the
    // same live channel Settings uses - see the DOCKSTYLE verb above).
    //
    // CFCLICK x y   - a full press+release at (x,y) in one shot: for taps
    //                 that resolve on release with no movement (a tab,
    //                 a tab button, the "+"/Sort rail tabs, a popup row/
    //                 swatch, a stowed edge).
    // CFDOWN x y    - press only (leaves the button "held" via
    //                 g_mouse_buttons so a drag can be driven step by step
    //                 from a /DOCK123.SEQ, one verb per line/HOLD).
    // CFMOVE x y    - move while held (no new press edge); this is what
    //                 crosses the drag-start threshold and updates a grip/
    //                 divider drag or the drop-target highlight.
    // CFUP x y      - release at (x,y), committing whatever CFDOWN/CFMOVE
    //                 started (group-into, pull-out-to-column, a resize, a
    //                 divider).
    // CFKEY n       - cardfile_handle_key(n); n=27 is ESC, the only way a
    //                 cardfile popup (picker/sort/swatches) closes besides
    //                 its own explicit button (true-modal, no click-away).
    if (strcmp(verb, "CFCLICK") == 0 || strcmp(verb, "CFDOWN") == 0 ||
        strcmp(verb, "CFMOVE") == 0 || strcmp(verb, "CFUP") == 0) {
        int x = th_atoi(arg);
        const char *sp = arg;
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        int y = th_atoi(sp);
        g_th_mouse_pinned = 1;
        g_mouse_x = x; g_mouse_y = y;
        if (strcmp(verb, "CFCLICK") == 0) {
            g_mouse_buttons |= 1u;
            cardfile_handle_mouse(x, y, 1, 0);
            g_mouse_buttons &= ~1u;
            cardfile_handle_mouse(x, y, 0, 0);
        } else if (strcmp(verb, "CFDOWN") == 0) {
            g_th_buttons_pinned = 1;   // hold across the whole CFDOWN..CFUP script
            g_mouse_buttons |= 1u;
            cardfile_handle_mouse(x, y, 1, 0);
        } else if (strcmp(verb, "CFMOVE") == 0) {
            cardfile_handle_mouse(x, y, 0, 0);   // buttons unchanged - still held from CFDOWN
        } else {   // CFUP
            g_mouse_buttons &= ~1u;
            cardfile_handle_mouse(x, y, 0, 0);
            g_th_buttons_pinned = 0;   // release control back to real hardware
        }
        g_needs_redraw = true;
        th_logf("OK %s %d %d", verb, x, y);
        return;
    }

    if (strcmp(verb, "CFKEY") == 0) {
        int consumed = cardfile_handle_key(th_atoi(arg));
        g_needs_redraw = true;
        th_logf("OK CFKEY consumed=%d", consumed);
        return;
    }

    // (cfmaxwidth) Dumps the CURRENT frame's exact cardfile geometry (rail/
    // edge/tab metrics, every slot's cf_layout() entry, the first open
    // slot's grip/maximize-button/plate rects, and the maximize view's
    // side widget rects) to serial, so a /DOCK123.SEQ script can read real
    // pixel coordinates instead of hand-computing them - the SAME formulas
    // cardfile.c's own render/input use (cardfile_debug_dump() lives there,
    // not duplicated here) rather than a guess that could drift.
    if (strcmp(verb, "CFDUMP") == 0) {
        char b[512];
        cardfile_debug_dump(b, sizeof(b));
        th_log(b);
        return;
    }

    // (cfmaxwidth) every card's app_path/win_id/launch_floor_id - verifies
    // the pre-launch snapshot exclusion actually bound the RIGHT window.
    if (strcmp(verb, "CFWINS") == 0) {
        char b[512];
        cardfile_debug_wins(b, sizeof(b));
        th_log(b);
        return;
    }

    // (cfmaxwidth) Seeds the deck with `n` real, distinct /APPS cards
    // (unopened), for width-drag/maximize verification scenarios that need
    // a specific, known slot count. Real app_paths (not synthetic ones), so
    // cf_host_launch() below binds a genuine hosted window, same as the "+"
    // picker's own path.
    if (strcmp(verb, "CFSEED") == 0) {
        static const struct { const char *path, *title; int cat, color; } apps[] = {
            { "/APPS/FILES",    "Files",     CF_CAT_ACCESSORIES, CF_COLOR_SAGE },
            { "/APPS/TERMINAL", "Terminal",  CF_CAT_ACCESSORIES, CF_COLOR_SLATE },
            { "/APPS/CALC",     "Calculator",CF_CAT_ACCESSORIES, CF_COLOR_WHEAT },
            { "/APPS/EDITOR",   "Editor",    CF_CAT_ACCESSORIES, CF_COLOR_ROSE },
            { "/APPS/BROWSER",  "Browser",   CF_CAT_INTERNET,    CF_COLOR_SPRUCE },
            { "/APPS/AICHAT",   "Maytera AI",CF_CAT_INTERNET,    CF_COLOR_HEATHER },
            { "/APPS/NOTES",    "Notes",     CF_CAT_ACCESSORIES, CF_COLOR_SAND },
        };
        cf_deck_t *d = cardfile_debug_deck();
        int n = th_atoi(arg);
        int made = 0, i;
        if (n <= 0) n = 5;
        if (n > (int)(sizeof(apps) / sizeof(apps[0]))) n = (int)(sizeof(apps) / sizeof(apps[0]));
        for (i = 0; i < n; i++) {
            uint32_t sid = cf_add_card(d, apps[i].path, apps[i].title, apps[i].cat, apps[i].color, uptime_ms(), 0, 0);
            if (!sid) continue;
            {
                cf_slot_t *s = cf_find_slot(d, sid);
                cf_card_t *c = s ? cf_slot_focused_card(s) : NULL;
                if (c) cf_host_launch(d, c->id, apps[i].path);
            }
            made++;
        }
        g_needs_redraw = true;
        th_logf("OK CFSEED made=%d", made);
        return;
    }

    // (cfmaxwidth) Owner-requested demo layout: TWO open columns, each a
    // GROUP-SPLIT of two real apps - left = Browser over Terminal, right =
    // Maytera AI (aichat) over Editor. Built via the SAME model ops the
    // real UI drag/drop path uses (cf_add_card/cf_group_card_into/
    // cf_open_second_as_column), then launched via cf_host_launch() exactly
    // as the "+" picker does, so every pane hosts a REAL running app window
    // through the normal cf_host_apply() placement path - this is not a
    // fake screenshot, cardfile_host_tick() (called every frame from
    // main.c) is what puts each window where it lands.
    if (strcmp(verb, "CFDEMO") == 0) {
        cf_deck_t *d = cardfile_debug_deck();
        uint64_t now = uptime_ms();
        uint32_t s_browser = cf_add_card(d, "/APPS/BROWSER",  "Browser",    CF_CAT_INTERNET,    CF_COLOR_SLATE, now, 0, 0);
        uint32_t s_term    = cf_add_card(d, "/APPS/TERMINAL", "Terminal",   CF_CAT_ACCESSORIES, CF_COLOR_SAGE,  now, 0, 0);
        uint32_t s_aichat  = cf_add_card(d, "/APPS/AICHAT",   "Maytera AI", CF_CAT_INTERNET,    CF_COLOR_ROSE,  now, 0, 0);
        uint32_t s_editor  = cf_add_card(d, "/APPS/EDITOR",   "Editor",     CF_CAT_ACCESSORIES, CF_COLOR_WHEAT, now, 0, 0);
        int ok = (s_browser && s_term && s_aichat && s_editor);
        if (ok) {
            // Capture both member card ids BY VALUE before EITHER grouping
            // call: cf_group_card_into()'s cf_remove_card_from_slot() can
            // cf_remove_slot_at() the source slot, which shifts every LATER
            // slot down one array position and memsets the vacated tail -
            // a cf_card_t* held across that call (e.g. into editor's slot,
            // which sits after term's in deck order) can end up pointing at
            // the zeroed tail, reading id=0 and silently failing the second
            // group. Reading ->id into a plain uint32_t up front is immune:
            // ids are stable across the shift, only array POSITIONS move.
            uint32_t term_card_id   = cf_slot_focused_card(cf_find_slot(d, s_term))->id;
            uint32_t editor_card_id = cf_slot_focused_card(cf_find_slot(d, s_editor))->id;
            ok = ok && cf_group_card_into(d, term_card_id, s_browser, now);
            ok = ok && cf_group_card_into(d, editor_card_id, s_aichat, now);
            cf_open_single(d, s_browser, now);
            cf_open_second_as_column(d, s_aichat, 0, now);
            {
                cf_slot_t *g = cf_find_slot(d, s_browser);
                int k;
                for (k = 0; g && k < g->ncards; k++) {
                    cf_host_launch(d, g->cards[k].id, g->cards[k].app_path);
                }
            }
            {
                cf_slot_t *g = cf_find_slot(d, s_aichat);
                int k;
                for (k = 0; g && k < g->ncards; k++) {
                    cf_host_launch(d, g->cards[k].id, g->cards[k].app_path);
                }
            }
        }
        g_needs_redraw = true;
        th_logf("OK CFDEMO ok=%d", ok);
        return;
    }

    // (cfmaxwidth) Opens deck-order slot `idx` (pure setup, see
    // cardfile_debug_open_index()'s own comment - not itself a hit-test).
    if (strcmp(verb, "CFOPENIDX") == 0) {
        int ok = cardfile_debug_open_index(th_atoi(arg));
        g_needs_redraw = true;
        th_logf("OK CFOPENIDX ok=%d", ok);
        return;
    }

    // (cfmaxwidth) Full press-move-release resize-grip drag through the REAL
    // cardfile_handle_mouse() dispatch, targeting column width `w` on
    // whichever slot is open - cardfile_debug_grip_drag() computes the
    // grip's on-screen rect itself.
    if (strcmp(verb, "CFGRIPDRAG") == 0) {
        int ok = cardfile_debug_grip_drag(th_atoi(arg));
        g_needs_redraw = true;
        th_logf("OK CFGRIPDRAG ok=%d", ok);
        return;
    }

    // (cfmaxwidth) A real click on the open slot's MAXIMIZE button.
    if (strcmp(verb, "CFMAXBTN") == 0) {
        int ok = cardfile_debug_click_maximize();
        g_needs_redraw = true;
        th_logf("OK CFMAXBTN ok=%d", ok);
        return;
    }

    // (cfmaxwidth) A real click on side 0(left)/1(right)'s maximize-view
    // widget - the summary tab if collapsed, the small collapse button if
    // expanded (whichever is currently showing).
    if (strcmp(verb, "CFSIDECLICK") == 0) {
        int ok = cardfile_debug_click_side(th_atoi(arg));
        g_needs_redraw = true;
        th_logf("OK CFSIDECLICK ok=%d", ok);
        return;
    }

    // (cfdock) Direct dock model manipulation for screenshot setup - see
    // cardfile.h's own comment on cardfile_debug_dock_add() etc. for why
    // this is setup, not a hit-test. `kind` is CF_DOCK_POS_EDGE(0)/BETWEEN(1);
    // `edge_or_slot` is CF_DOCK_EDGE_TOP/BOTTOM/LEFT/RIGHT(0-3) when kind==0,
    // a slot boundary index when kind==1.
    //   CFDOCKADD kind edge_or_slot thickness  - adds a dock, logs its id
    //   CFDOCKPOS id kind edge_or_slot          - moves an existing dock
    //   CFDOCKTHICK id thickness                - resizes an existing dock
    //   CFDOCKRM id                              - removes a dock
    if (strcmp(verb, "CFDOCKADD") == 0) {
        int kind = th_atoi(arg);
        const char *sp = arg;
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        int eos = th_atoi(sp);
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        int thick = th_atoi(sp);
        uint32_t id = cardfile_debug_dock_add(kind, eos, thick);
        g_needs_redraw = true;
        th_logf("OK CFDOCKADD id=%u", id);
        return;
    }
    if (strcmp(verb, "CFDOCKPOS") == 0) {
        int id = th_atoi(arg);
        const char *sp = arg;
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        int kind = th_atoi(sp);
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        int eos = th_atoi(sp);
        int ok = cardfile_debug_dock_set_pos((uint32_t)id, kind, eos);
        g_needs_redraw = true;
        th_logf("OK CFDOCKPOS ok=%d", ok);
        return;
    }
    if (strcmp(verb, "CFDOCKTHICK") == 0) {
        int id = th_atoi(arg);
        const char *sp = arg;
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        int thick = th_atoi(sp);
        int ok = cardfile_debug_dock_set_thickness((uint32_t)id, thick);
        g_needs_redraw = true;
        th_logf("OK CFDOCKTHICK ok=%d", ok);
        return;
    }
    if (strcmp(verb, "CFDOCKRM") == 0) {
        int ok = cardfile_debug_dock_remove((uint32_t)th_atoi(arg));
        g_needs_redraw = true;
        th_logf("OK CFDOCKRM ok=%d", ok);
        return;
    }
    // (cfdock) Right-click, for a real end-to-end proof of the dock context
    // menu path (cardfile_handle_right_click()), the same real-dispatch role
    // CFCLICK plays for left clicks.
    if (strcmp(verb, "CFRCLICK") == 0) {
        int x = th_atoi(arg);
        const char *sp = arg;
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        int y = th_atoi(sp);
        int consumed = cardfile_handle_right_click(x, y);
        g_needs_redraw = true;
        th_logf("OK CFRCLICK consumed=%d", consumed);
        return;
    }


    // (wallpersist, no-ticket) SETWALL <filename> - drive the wallpaper
    // picker's own selection code path (wallpaper.c's mouse handler:
    // wallpaper_load() + set_wallpaper() + profile_save()) BY NAME, the same
    // "by name not by pixel" idiom every other verb in this file uses,
    // so a headless run can prove the real runtime persistence sequence
    // without a coordinate-accurate click on the picker grid. Resolves the
    // name through wp_enumerate(), the SAME enumeration wallpaper.c and
    // Settings both use, so the index this picks is byte-identical to what
    // a real click on that thumbnail would pick. Logs the resolved index so
    // a serial capture can be cross-checked against the on-disk
    // UIPROFIL.YML "wallpaper: N" line.
    if (strcmp(verb, "SETWALL") == 0) {
        wp_entry_t list[WP_MAX_ENTRIES];
        int count = wp_enumerate(list, WP_MAX_ENTRIES);
        int found = -1;
        for (int i = 0; i < count; i++) {
            if (strcmp(list[i].file, arg) == 0) { found = i; break; }
        }
        if (found < 0) {
            th_logf("ERR SETWALL no such wallpaper '%s'", arg);
            return;
        }
        wallpaper_load(found);
        set_wallpaper(found);
        profile_save();
        g_needs_redraw = true;
        th_logf("OK SETWALL %s idx=%d", arg, found);
        return;
    }

    // #wpanim verification-only verbs: drive the animated-wallpaper state
    // directly (sidesteps the context-menu hit-test the same way
    // ICON/MENUITEM sidestep the desktop/start-menu ones - proves the
    // EFFECT and the window-reactivity, not the "how do I open the picker"
    // click path, which is a separate, ordinary hit-test already covered by
    // the same class of bug WINCLOSE's own comment describes).
    // Format: "WPANIM <mode 0-4> <intensity 0-100> <repel 0|1> [/path/to/app]".
    // The optional trailing app path is spawned in the SAME call (sys_spawn(),
    // the exact call LAUNCH above makes) - /TESTHOOK.CMD is a ONE-SHOT,
    // baked-before-boot command (see testhook_poll()'s own comment on why),
    // so a live verification session that needs both "pick an effect" and "a
    // window to react to" needs them in one verb, not two.
    if (strcmp(verb, "WPANIM") == 0) {
        int mode = th_atoi(arg);
        const char *sp = arg;
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        int inten = th_atoi(sp);
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        int repel = th_atoi(sp);
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        set_wallpaper_anim(mode);
        set_wallpaper_anim_intensity(inten);
        set_wallpaper_anim_repel(repel);
        if (*sp != '\0') sys_spawn(sp);
        g_needs_redraw = true;
        th_logf("OK WPANIM mode=%d intensity=%d repel=%d spawn=%s", get_wallpaper_anim(),
                get_wallpaper_anim_intensity(), get_wallpaper_anim_repel(), sp);
        return;
    }
    // "WPICK OPEN" / "WPICK CLOSE": drive the picker overlay directly, to
    // screenshot the effect-selector UI itself without a coordinate-exact
    // right-click on the desktop.
    if (strcmp(verb, "WPICK") == 0) {
        if (strcmp(arg, "OPEN") == 0) { wallpaper_picker_open(); g_needs_redraw = true; th_log("OK WPICK OPEN"); return; }
        if (strcmp(arg, "CLOSE") == 0) { wallpaper_picker_close(); g_needs_redraw = true; th_log("OK WPICK CLOSE"); return; }
        th_log("ERR WPICK needs OPEN|CLOSE");
        return;
    }

    th_log("ERR unknown verb");
}

#endif // MAYTERA_TESTHOOK
