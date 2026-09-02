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
// (#123) write(1,...) serial mirror. Declared here rather than including
// libc/unistd.h: that header pulls in libc/types.h, whose `bool` typedef
// collides with compositor.h/stdbool in this translation unit.
long write(int fd, const void *buf, unsigned long n);
#include "../../libc/gui_theme.h"
#include "../../libc/userconf.h"   // #745 GLASSTHEME
#include "../../libc/dock_opacity.h"  // #132: shared DOCK_OPACITY_MIN/MAX
#include "../../libc/stdio.h"    // (#231r) vsnprintf for th_logf below
#include <stdarg.h>

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

    th_log("ERR unknown verb");
}

#endif // MAYTERA_TESTHOOK
