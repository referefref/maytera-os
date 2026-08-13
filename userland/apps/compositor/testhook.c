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
#include "../../libc/gui_theme.h"
#include "../../libc/userconf.h"   // #745 GLASSTHEME

#define TH_CMD_PATH "/TESTHOOK.CMD"
#define TH_OUT_PATH "/TESTHOOK.OUT"
#define TH_O_APPEND (0x1 | 0x40 | 0x400)   // O_WRONLY | O_CREAT | O_APPEND

static void th_log(const char *msg) {
    int fd = sys_open(TH_OUT_PATH, TH_O_APPEND);
    if (fd < 0) return;
    sys_write(fd, msg, strlen(msg));
    sys_write(fd, "\n", 1);
    sys_close(fd);
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
    static const char *NM[3] = { "PANEL", "DOCK", "MENU" };
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
    int fd = sys_open(TH_CMD_PATH, 0 /* O_RDONLY */);
    if (fd < 0) return;   // no pending command: fast common-case return

    char buf[192];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    sys_unlink(TH_CMD_PATH);   // consume so a command never re-fires
    if (n < 0) n = 0;
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
            if (v >= 70 && v <= 100) g_dock_opacity = v;   // #745 dockgrey: floor 60 -> 70
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

    if (strcmp(verb, "LAUNCH") == 0) {
        if (arg[0] == '\0') { th_log("ERR LAUNCH needs a path"); return; }
        sys_spawn(arg);
        th_log("OK LAUNCH");
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

    th_log("ERR unknown verb");
}

#endif // MAYTERA_TESTHOOK
