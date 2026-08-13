// taskbar.c - Taskbar with system gauges for MayteraOS Userland Compositor
// Phase 3: Complete Desktop Port

#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../libc/bt_client.h"   // #372: Bluetooth tray indicator (mock-backed)
#include "../../libc/wifi_client.h" // #384: Network/Wi-Fi tray indicator (wifi mock)
#include "../../libc/theme.h"       // #711 mtheme v2 metrics (taskbar-tile Close geometry)
#include "../../libc/stdio.h"       // #41: snprintf for the title-fallback debug line
#include "../../libc/math.h"        // #63/#745: sqrtf for xfce_dock_paint_rounded()'s AA corner coverage
// #41: NOT #include "../../libc/unistd.h" - it drags in libc/types.h, whose
// own `bool` typedef conflicts with compositor.h's (the documented
// gui_scroll.h/types.h landmine, blame.md 2026-07-29). A local extern for
// the one function needed (matching unistd.h's own prototype) sidesteps it,
// same pattern win16api.c uses for window_create().
extern long write(int fd, const void *buf, size_t count);

// Forward declaration: g_tray_bar_top is DEFINED further down (with the rest
// of the #387 tray metrics, near tray_render_core()), but #26's
// draw_perf_popup() reads it earlier in the file to decide which way the
// performance popup opens. One definition, just declared here too so the
// earlier reader can see it.
extern int g_tray_bar_top;

// (#745) The AI command-launcher glyph (a two-star "sparkle" mark, replacing
// the old /MAYLOGO.DAT wordmark silhouette the user reported as "too skinny
// to be readable") is defined below, near tray_blit_mask() - see
// ai_launcher_icon_draw() and ai_glyphs.h for the full rationale.

// ---------------------------------------------------------------------------
// (#745 taskbar/tray pass) The two launcher chips: Start and Maytera.
//
// SUPERSEDES the earlier #745 "ASK 2" design (history kept in blame.md): that
// pass replaced a blue accent-colored fill with an opaque achromatic plate
// (0xE4E7EB fixed, every theme) specifically to GUARANTEE contrast regardless
// of theme. It did guarantee it - but it also made Start/Maytera the ONLY two
// tray elements with a plate behind their glyph at all: every other tray icon
// (widgets/sound/net/bt/sheep/bell) paints straight onto CLR_TASKBAR_BG via
// readable_ink(), no fill. On the 8 of 14 shipped themes with a dark
// taskbar_bg the fixed light plate reads as a foreign rectangle glued onto
// the bar (8.6:1-16.9:1 chip-to-bar contrast - "light background" exactly as
// reported); on the 6 light-bar themes it nearly vanishes (1.1:1-1.7:1),
// which is why the report said "for most themes", not "always". See
// docs/TASKBAR_AND_TRAY.html section 3 for the full 14-theme table.
//
// Fix: no plate. Ink is readable_ink(CLR_TASKBAR_BG) - the same call every
// other tray icon already makes, 8.39:1 minimum across all 14 themes (both
// above the 3:1 UI floor and the 4.5:1 text floor). State (hover/open) is a
// translucent-looking wash PAINTED UNDER the glyph, never a permanent fill
// swap - the same convention the running-window tile row already uses
// (~line 1039 below: is_focused ? CLR_START_BTN : (is_min ? CLR_TASKBAR_BG :
// CLR_TASKBAR_HOVER)). Open state reuses CLR_TASKBAR_ACTIVE (theme key
// taskbar_active - present in all 14 .mtheme files and already read
// kernel-side, case 31; this is its first compositor reader).
//
// bg_out reports whatever was actually painted under the glyph (the wash, or
// CLR_TASKBAR_BG at rest when nothing was painted at all) so a caller that
// alpha-composites a bitmap over the chip (maylogo_draw) uses ONE canonical
// answer instead of re-deriving hover/open state itself - two call sites
// computing the same thing independently is exactly the class of bug that
// leaves them silently disagreeing later.
//
// Shared by DOCK_DEFAULT and DOCK_XFCE so the two can never draw differently.
// ---------------------------------------------------------------------------
static void chrome_chip(int32_t x, int32_t y, int32_t sz, int open,
                         uint32_t *ink_out, uint32_t *bg_out)
{
    int hover = (g_mouse_x >= x && g_mouse_x < x + sz &&
                 g_mouse_y >= y && g_mouse_y < y + sz);
    uint32_t bg = open ? CLR_TASKBAR_ACTIVE : (hover ? CLR_TASKBAR_HOVER : CLR_TASKBAR_BG);
    if (open || hover) draw_fill_rect(x, y, sz, sz, bg);
    *ink_out = readable_ink(CLR_TASKBAR_BG);
    if (bg_out) *bg_out = bg;
}

// ---------------------------------------------------------------------------
// (#745) Glass surface helpers. A glass surface is: the blurred+tinted
// composite, then a 1px hairline at alpha 128 (a fully opaque hairline reads as
// chrome, not as the edge of a pane of glass), then optionally a 1px white
// inner highlight, which is what makes the surface read as a raised pane rather
// than a hole in the wallpaper.
// ---------------------------------------------------------------------------
// (#745) Percentage lighten toward white. darken_argb() (main.c) already does
// the subtract-a-flat-amount version for CLR_MENU_SHADOW; this is the
// blend-toward-white sibling, kept local for the same reason darken_argb is:
// the compositor has no gui_style link, so libc/gui.c's gui_lighten() is not
// reachable from here (see "Key facts": the handle-based gui_* app toolkit is
// not callable in the compositor).
static uint32_t tb_lighten_argb(uint32_t c, int pct) {
    int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
    r += (255 - r) * pct / 100;
    g += (255 - g) * pct / 100;
    b += (255 - b) * pct / 100;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void glass_or_flat(int32_t x, int32_t y, int32_t w, int32_t h, int surf)
{
    if (g_glass_enable) { glass_render(x, y, w, h, CLR_GLASS_TINT, surf); return; }
    // Tier 4 (opaque fallback) only: a subtle 3-band top-light gradient
    // instead of one dead-flat fill, each band a PERCENTAGE lighten of the
    // theme's own taskbar_bg - never a fixed hex pair, so it reads right on
    // all 14 themes with no per-theme tuning. Glass surfaces already vary
    // naturally (blurred, tinted backdrop) so this does not double up on top
    // of glass. See docs/TASKBAR_AND_TRAY.html section 4.
    if (h >= 8) {
        draw_fill_rect(x, y,     w, 4, tb_lighten_argb(CLR_TASKBAR_BG, 10));
        draw_fill_rect(x, y + 4, w, 4, tb_lighten_argb(CLR_TASKBAR_BG, 4));
        draw_fill_rect(x, y + 8, w, h - 8, CLR_TASKBAR_BG);
    } else {
        draw_fill_rect(x, y, w, h, CLR_TASKBAR_BG);
    }
}

// One hairline of the surface edge, at alpha 128 when the surface is glass and
// fully opaque when it is not.
static void glass_edge_h(int32_t x, int32_t y, int32_t w, uint32_t c)
{
    int ob = g_draw_blend;
    if (g_glass_enable) g_draw_blend = 128;
    draw_hline(x, y, w, c);
    g_draw_blend = ob;
}
static void glass_edge_v(int32_t x, int32_t y, int32_t h, uint32_t c)
{
    int ob = g_draw_blend;
    if (g_glass_enable) g_draw_blend = 128;
    draw_vline(x, y, h, c);
    g_draw_blend = ob;
}
// Inner highlight: white at alpha 26 on a dark material, 140 on a light one.
static void glass_highlight_h(int32_t x, int32_t y, int32_t w)
{
    if (!g_glass_enable) return;
    int ob = g_draw_blend;
    g_draw_blend = (draw_luminance(CLR_GLASS_TINT) >= 140) ? 140 : 26;
    draw_hline(x, y, w, 0xFFFFFFFF);
    g_draw_blend = ob;
}
// #387 Alternate dock/taskbar layout metrics (used by both the renderers below
// and taskbar_collect_damage above them, so defined here at file scope).
#define LUMINA_MENUBAR_H     24
#define LUMINA_DOCK_ICON     40
#define LUMINA_DOCK_PAD      8
#define LUMINA_DOCK_RESERVE  64     // work-area reserved at bottom for the dock
#define CLASSIC_UNIX_PANEL_H       58
#define RETRO_BENCH_BAR_H       20
// #26 XFCE ("Marble" in Settings): flush top panel + flush-to-bottom-edge
// fit-content dock (pinned favorites, then a separator, then running-but-
// not-pinned windows). Glass (both surfaces) + a hover lift/grow on the dock
// icons as of #745 - STALE "opaque/no translucency/no magnification" wording
// fixed, see taskbar_render_xfce_panel() below for the current state. See
// docs/DESKTOP_SHELL_RESEARCH.md section 4 and docs/DOCK_XFCE_MOCKUP.html
// for the ported spec table this geometry is taken from verbatim.
#define XFCE_PANEL_H      30
#define XFCE_BTN_SIZE     24
// XFCE_DOCK_ICON is the DOCK TILE size: slot spacing, dock width, hit-boxes,
// hover-rect geometry. It must NOT move for a "shrink the icon" request (the
// dock itself must not change) - see XFCE_DOCK_ICON_DRAW below, the ONE
// place that controls how big the icon GRAPHIC drawn inside that tile is.
#define XFCE_DOCK_ICON    40
// #63/#745 USER-REPORTED: "the icons in the dock (marble) are too large,
// reduce their size compared to the dock by 10%". This is the ONLY knob for
// that: xfce_draw_slot() below is the ONE call site that turns a favourite
// into pixels on screen (both the pinned-favourites and running-only loops
// in taskbar_render_xfce_dock() share it, per its own header comment), and
// it is the base (grow=0, i.e. not hovered) icon size. 40 * 0.9 = 36 exactly
// - no rounding ambiguity. The tile (XFCE_DOCK_ICON, above) and every
// spacing/hit-box constant derived from it is UNCHANGED, so the dock's own
// width/height/hit-testing are byte-identical to before this change; only
// the icon graphic drawn inside each tile is smaller, centred (xfce_draw_
// slot's existing `ix = cx + (XFCE_DOCK_ICON - isize) / 2` formula already
// centres whatever isize it is given) and still bottom-anchored to the same
// baseline. The hover grow/lift animation (XFCE_HOVER_GROW/LIFT below) is
// unaffected: it adds to whatever base isize is, same as before.
#define XFCE_DOCK_ICON_DRAW 36
#define XFCE_DOCK_PAD     8
#define XFCE_DOCK_IND     8    // indicator-bar band under the icon row
#define XFCE_DOCK_H       (XFCE_DOCK_ICON + 2 * XFCE_DOCK_PAD + XFCE_DOCK_IND)  // 64
#define XFCE_DOCK_SEP_EXTRA (XFCE_DOCK_PAD + 1)   // widens one pad gap into a separator
// (#40) The marble dock is a CENTRED, variable-width floating pane, not a
// full-width bar: its x/width depend on how many items it is showing this
// frame. taskbar_render_xfce_dock() caches them here, and taskbar_panel_
// rects() re-publishes them, so a collision consumer never has to reproduce
// the item-count arithmetic. Width 0 = not painted yet (first frame), which
// callers must read as "no dock rect", never as a zero-width rect at x=0.
static int32_t g_xfce_dock_x = 0, g_xfce_dock_w = 0;
// #63/#745 USER-REPORTED: "the bright white of the icons in the dark dock is
// also a bit jarring ... slightly off white". The shipped dock icon set is
// white lineart on transparent (assets/icons/README.md's "recolor to solid
// white only" rule), so the ink is literally baked into the .ICN raster;
// this is what xfce_draw_slot() recolours it TO via icon_draw_dock_icon()
// (icons.c) instead of drawing the raw #fff pixels. A small, deliberate step
// down from pure white (255,255,255 -> 242,242,242, ~5%), NOT a return to
// readable_ink()/CLR_CHROME_TEXT (those are computed per-theme and this is a
// fixed, small desaturation the user asked for specifically on white -
// "slightly off white", not "theme-correct ink"). MEASURED (see
// /tmp/dockicon2/contrast_calc.py, a host-side port of draw_blend() +
// glass_render()'s alpha derivation + the W3C WCAG contrast formula) against
// the WORST CASE composited Marble dock surface: op=70 (the derived opacity
// floor, draw.c glass_render()), white wallpaper backdrop (the adversarial
// case for a LIGHT ink - a brighter backdrop washes the glass tint lighter,
// shrinking the gap to a light ink), maytera_dark's tint -> surface #5F5F63.
// Pure white there is 6.36:1; this off-white is 5.68:1 - both clear the
// WCAG AA 3:1 non-text floor AND the 4.5:1 text floor with real margin, so
// this is a genuine style refinement, not a contrast regression.
//
// (local 63 follow-up, 2026-08-12) THAT MEASUREMENT COVERED ONE THEME. Swept
// across all 14 shipped .mtheme files (the same host-side port of
// glass_theme_apply, glass_render's alpha derivation, draw_blend and the W3C
// contrast formula, now driven from the theme files themselves), a FIXED light
// ink is under the 3:1 non-text floor on SIX of them: the four light themes
// sit at 1.04-1.06:1 (white lineart on a near-white bar, effectively
// invisible) and the two flat retro themes at 1.63:1 (classic) and 1.85:1
// (retro_unix). That is pre-existing, not introduced by the off-white step:
// before the dock recoloured at all it drew the raster's baked-in pure white,
// which measures worse on every one of those six. The single-ink knob is what
// makes it fixable in one place: see dock_icon_ink below. This constant stays
// exactly what the user asked for and is now applied ONLY where it is legible.
#define CLR_DOCK_ICON_INK 0xFFF2F2F2u
#define XFCE_MAX_FAVS     12   // matches startmenu.c's MAX_FAVORITES

// ============================================================================
// Static state
// ============================================================================

static int32_t g_taskbar_y;
static int     g_cpu_percent;
static unsigned int g_cpu_cores[65];   // [0]=count, [1..]=per-core % (#279 per-core meter)
static int     g_cpu_ncores = 1;
static int     g_ram_percent;
static int     g_disk_percent;
static int     g_net_percent;
static uint64_t g_gauge_update_time;
static char    g_cpu_str[8];
static char    g_ram_str[8];
static char    g_disk_str[8];
// #76: not a percent string any more ("<n>KB/s"/"<n>MB/s"). Sized for the
// worst case the NET_BPS_SANITY_MAX clamp in taskbar_update() allows: a
// burst can outrun the averaged unit (KB/s branch, up to 7 digits) before
// the 10-sample average catches up and switches to MB/s - 7 digits + "KB/s"
// + NUL is 12 bytes; 16 leaves margin.
static char    g_net_str[16];

// #241 Performance popup: clicking a taskbar gauge opens a detailed, themed
// read-only system-performance panel anchored above the gauges.
static int      g_perf_open = 0;     // popup visible?
static int      g_perf_sel  = 0;     // highlighted gauge row (0=CPU..3=NET)
static int32_t  g_gauge_x0  = 0;     // left edge of the gauge block (this frame)
static int32_t  g_gauge_y   = 0;     // top of the gauge row (this frame)
static int32_t  g_pp_x, g_pp_y, g_pp_w, g_pp_h;             // popup rect
static int32_t  g_pp_tm_x, g_pp_tm_y, g_pp_tm_w, g_pp_tm_h; // Task Manager button
// Absolute readings captured during taskbar_update for the detail view.
static unsigned long g_ram_used_b = 0, g_ram_total_b = 0;   // bytes
static long          g_disk_total_mb = 0, g_disk_free_mb = 0; // MB
static unsigned long g_net_bps = 0;                          // bytes/sec estimate

// Open-window list for the taskbar app buttons.
#define TB_MAX_WINS 16
static wm_window_info_t g_tb_wins[TB_MAX_WINS];
static int             g_tb_win_count;

// Compact window buttons: icon + short label.
#define TB_BTN_W   120   // preferred per-button width (shrinks to fit)
#define TB_BTN_GAP 3
#define TB_ICON_SZ 16

// Hitboxes recorded each frame so taskbar_handle_mouse can focus on click.
static int32_t g_tb_btn_x[TB_MAX_WINS];       // left edge of each drawn button
static int32_t g_tb_btn_id[TB_MAX_WINS];      // window id for that button
static int     g_tb_btn_focused[TB_MAX_WINS]; // is that window currently focused?
static int     g_tb_btn_n;                    // number of buttons drawn
static int32_t g_tb_btn_w;                    // per-button width this frame
static int32_t g_tb_btn_y;                    // top of the button row
static int32_t g_tb_btn_h;                    // button height

// A window is "real" (worth a taskbar button) if it has a title. Minimized
// windows are still shown (so they can be restored from the taskbar, like
// Windows). The root/desktop window has no title, so it is skipped naturally.
// #341: NOCHROME helper / sub-windows that belong to another app's main window
// (or are docked widget-style panels) must NOT get their own taskbar tile. The
// kernel wm_window_info_t exposes neither the WINDOW_FLAG_NOCHROME flag nor the
// owner PID, and we deploy only the compositor (a struct change would break the
// ABI against the already-deployed kernels), so companion windows are matched by
// their known titles instead. Result: AI Chat = no tile (docked panel); Maytera
// HiFi keeps exactly ONE tile (its main window) while Album Art / Playlist /
// Equalizer / Library / Viz sub-windows are suppressed. Prefix match, so
// "Maytera Viz - MilkDrop" is caught while "Maytera HiFi" is preserved.
static int tb_is_companion(const char *t) {
    static const char *sub[] = {
        // local 66: the AI panel's window title became "Maytera AI Interface".
        // This list is TITLE-keyed, so the rename would silently have given the
        // borderless dock panel a taskbar tile of its own. Both spellings stay:
        // an already-deployed kernel/asset-base binary can still be carrying the
        // old title, and a suppression list that only knows the new one would
        // regress exactly the case it exists to cover.
        "AI Chat",
        "Maytera AI Interface",
        "HiFi Equalizer", "HiFi Playlist",
        "Album Art", "Music Library",
        "Maytera Viz",
        0
    };
    for (int i = 0; sub[i]; i++) {
        int j = 0;
        while (sub[i][j] && sub[i][j] == t[j]) j++;
        if (sub[i][j] == '\0') return 1;   // t starts with sub[i]
    }
    return 0;
}

static int tb_window_is_app(const wm_window_info_t *w) {
    if (w->title[0] == '\0') return 0;
    // Show a tile for live, user-facing windows only: visible (normal) or
    // minimized. A hidden window (closed via the default X action: visible=0,
    // minimized=0) is no longer user-facing, so it must not keep a tile.
    if (!w->visible && !w->minimized) return 0;
    // #341: drop borderless companion/sub-windows so each app gets one tile.
    if (tb_is_companion(w->title)) return 0;
    return 1;
}

// Case-insensitive substring test (no libc strstr dependency here).
static int tb_contains(const char *s, const char *sub) {
    for (int i = 0; s[i]; i++) {
        int j = 0;
        while (sub[j]) {
            char a = s[i + j], b = sub[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            j++;
        }
        if (!sub[j]) return 1;
    }
    return 0;
}

// #41 (task #41, 2026-08-12): exact app-identity match. `app_id` is
// wm_window_info_t.app_id, a kernel-resolved binary basename (e.g. "PAINT"),
// never the window's own chosen title. `exec_path` is a favourite's launch
// path (e.g. "/APPS/PAINT"); only its final path component is compared.
// Case-insensitive because the asset base mixes FAT-uppercase and ext2-
// lowercase paths (see the COMPOSIT-vs-COMPOSITOR install-name trap in
// blame.md) - the identity must survive that, not just the common case. An
// empty app_id NEVER matches: that is the "no identity available" sentinel
// (kernel-desktop-fallback window, or the owning process already exited),
// and the caller must fall back to another heuristic for it, not treat
// empty==empty as a hit.
static int tb_app_id_matches(const char *app_id, const char *exec_path) {
    if (!app_id || !app_id[0] || !exec_path) return 0;
    const char *base = exec_path;
    for (const char *p = exec_path; *p; p++) if (*p == '/') base = p + 1;
    int i = 0;
    for (; app_id[i] && base[i]; i++) {
        char a = app_id[i], b = base[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return app_id[i] == '\0' && base[i] == '\0';
}

// Pick an icon for a window based on its title; ICON_WINDOW is the fallback.
// #562: checks that could collide with a more specific one below them (e.g.
// "task" inside "Task Switcher", "chat" inside "AI Chat") are ordered so the
// specific match runs first.
static icon_id_t tb_icon_for_title(const char *t) {
    if (tb_contains(t, "term"))                          return ICON_TERMINAL;
    if (tb_contains(t, "calc"))                          return ICON_CALCULATOR;
    if (tb_contains(t, "convert"))                       return ICON_CONVERTER;
    if (tb_contains(t, "file") || tb_contains(t, "folder")) return ICON_FOLDER;
    if (tb_contains(t, "gallery"))                       return ICON_GALLERY;
    if (tb_contains(t, "snapshot"))                      return ICON_SNAPSHOT;
    if (tb_contains(t, "note"))                          return ICON_NOTES;
    if (tb_contains(t, "edit"))                          return ICON_HIGHLIGHT;
    if (tb_contains(t, "paint"))                         return ICON_PAINT;
    if (tb_contains(t, "image") || tb_contains(t, "view")) return ICON_IMAGE;
    if (tb_contains(t, "audio") || tb_contains(t, "music") ||
        tb_contains(t, "media") || tb_contains(t, "player")) return ICON_MUSIC;
    if (tb_contains(t, "timer"))                         return ICON_TIMERS;
    if (tb_contains(t, "clock"))                         return ICON_CLOCK;
    if (tb_contains(t, "font"))                          return ICON_FONTBOOK;
    if (tb_contains(t, "setting"))                       return ICON_COG;
    if (tb_contains(t, "service"))                       return ICON_SERVICES;
    if (tb_contains(t, "monitor"))                       return ICON_SYSMON;
    if (tb_contains(t, "switch"))                        return ICON_TASKSWITCH;
    if (tb_contains(t, "launcher"))                      return ICON_LAUNCHER;
    if (tb_contains(t, "task"))                          return ICON_TASK_MANAGER;
    if (tb_contains(t, "log"))                           return ICON_LOG_VIEWER;
    if (tb_contains(t, "recycle") || tb_contains(t, "trash")) return ICON_TRASH;
    if (tb_contains(t, "doom"))                          return ICON_GAME_DOOM;
    if (tb_contains(t, "arena"))                         return ICON_GAME_ARENA;
    if (tb_contains(t, "chess"))                         return ICON_GAME_CHESS;
    if (tb_contains(t, "squadron"))                      return ICON_GAME_SQUADRON;
    if (tb_contains(t, "gl cube") || tb_contains(t, "glcube")) return ICON_GAME_GLCUBE;
    if (tb_contains(t, "gl matrix") || tb_contains(t, "glmatrix")) return ICON_GAME_GLMATRIX;
    if (tb_contains(t, "auth"))                          return ICON_AUTH;
    if (tb_contains(t, "help"))                          return ICON_HELP;
    if (tb_contains(t, "store"))                         return ICON_APPSTORE;
    if (tb_contains(t, "3d print") || tb_contains(t, "print3d")) return ICON_3DPRINT;
    if (tb_contains(t, "weather"))                       return ICON_WEATHER;
    if (tb_contains(t, "feed") || tb_contains(t, "rss")) return ICON_FEEDS;
    if (tb_contains(t, "ai chat") || tb_contains(t, "aichat")) return ICON_AICHAT;
    if (tb_contains(t, "irc"))                           return ICON_IRC;
    if (tb_contains(t, "browser"))                       return ICON_BROWSER;
    if (tb_contains(t, "network") || tb_contains(t, "chat")) return ICON_INFO_CIRCLE;
    return ICON_WINDOW;
}

// ============================================================================
// Internal helpers
// ============================================================================

// Write an integer percentage as "N%" into buf (max 7 chars + NUL).
// No libc sprintf available, so format manually.
static void fmt_percent(char *buf, int pct) {
    // Clamp to valid range.
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    int i = 0;

    if (pct == 100) {
        buf[i++] = '1';
        buf[i++] = '0';
        buf[i++] = '0';
    } else if (pct >= 10) {
        buf[i++] = (char)('0' + pct / 10);
        buf[i++] = (char)('0' + pct % 10);
    } else {
        buf[i++] = (char)('0' + pct);
    }

    buf[i++] = '%';
    buf[i]   = '\0';
}

// Draw a single horizontal gauge bar with label on the left and value on the
// right. No dynamic allocation.
static void draw_gauge(int32_t x, int32_t y, int32_t w, int32_t h,
                       int percent, uint32_t color,
                       const char *label, const char *value) {
    // Background.
    draw_fill_rect(x, y, w, h, CLR_GAUGE_BG);

    // Filled portion. Guard against zero or negative width.
    if (percent > 0) {
        int32_t fill_w = w * percent / 100;
        if (fill_w < 1) fill_w = 1;
        draw_fill_rect(x, y, fill_w, h, color);
    }

    // Border drawn on top so it is always crisp.
    draw_rect_outline(x, y, w, h, CLR_GAUGE_BORDER);

    // Vertical center for text.
    int32_t text_y = y + (h - FONT_CHAR_H) / 2 + 1;

    // Label: left-aligned with small padding.
    draw_text(x + 4, text_y, label, readable_ink(CLR_GAUGE_BG));

    // Value: right-aligned with small padding.
    int32_t val_x = x + w - text_width(value) - 4;
    draw_text(val_x, text_y, value, readable_ink(CLR_GAUGE_BG));
}

// Per-core CPU gauge (#279): same footprint as draw_gauge, but the fill area is
// split into one vertical bar per core (height proportional to that core's %).
// Single-core systems fall back to looking like a normal horizontal gauge.
static void draw_cpu_gauge(int32_t x, int32_t y, int32_t w, int32_t h,
                           int ncores, const unsigned int *cores) {
    draw_fill_rect(x, y, w, h, CLR_GAUGE_BG);

    // Reserve the left side for the "CPU" label; bars fill the rest.
    int32_t label_w = text_width("CPU") + 6;
    int32_t bars_x  = x + label_w;
    int32_t bars_w  = w - label_w - 3;
    if (ncores < 1) ncores = 1;
    if (bars_w < ncores) bars_w = ncores;

    int32_t inner_h = h - 4;
    if (inner_h < 1) inner_h = 1;
    int32_t gap = (ncores > 1) ? 1 : 0;
    int32_t bw  = (bars_w - gap * (ncores - 1)) / ncores;
    if (bw < 1) bw = 1;

    for (int i = 0; i < ncores; i++) {
        int pct = (int)cores[1 + i];
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        int32_t bx = bars_x + i * (bw + gap);
        int32_t fh = inner_h * pct / 100;
        // Track (empty part) then the filled portion from the bottom.
        draw_fill_rect(bx, y + 2, bw, inner_h, CLR_GAUGE_BORDER);
        if (fh > 0)
            draw_fill_rect(bx, y + 2 + (inner_h - fh), bw, fh, CLR_GAUGE_CPU);
    }

    draw_rect_outline(x, y, w, h, CLR_GAUGE_BORDER);
    int32_t text_y = y + (h - FONT_CHAR_H) / 2 + 1;
    draw_text(x + 4, text_y, "CPU", readable_ink(CLR_GAUGE_BG));
}

// ============================================================================
// #241 Performance popup helpers
// ============================================================================

// Unsigned -> decimal string (no libc here).
static void fmt_uint(char *buf, unsigned long v) {
    char tmp[24];
    int  n = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (v > 0 && n < 23) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int i = 0;
    while (n > 0) buf[i++] = tmp[--n];
    buf[i] = '\0';
}
// Append a C string at offset p; returns the new offset.
static int sappend(char *dst, int p, const char *s) {
    while (*s) dst[p++] = *s++;
    dst[p] = '\0';
    return p;
}
static int sappend_u(char *dst, int p, unsigned long v) {
    char num[24];
    fmt_uint(num, v);
    return sappend(dst, p, num);
}

// ============================================================================
// #76 NET gauge: variable-unit throughput display, replacing the old literal
// "percent of a 1 Gbit/s link" scale. That scale read as a meaningless low
// single-digit percentage forever on a busy USB Ethernet dongle (nowhere near
// 1 Gbit), which is why the gauge looked broken even under real load.
// ============================================================================

// 10-sample rolling history of g_net_bps, pushed once per taskbar_update()
// tick (~1Hz normally, ~5Hz while the perf popup is open - see the #102b
// throttle comment in taskbar_update()). Used ONLY to pick KB/s vs MB/s:
// averaging smooths out a single sample landing near the 1024 KB/s boundary
// so the UNIT does not flicker KB/s<->MB/s from one tick to the next - the
// same class of jitter #102b already documents for the displayed digits,
// fixed here the same way (damp, don't invent a second mechanism).
#define NET_HIST_N 10
static unsigned long s_net_hist[NET_HIST_N];
static int           s_net_hist_n = 0, s_net_hist_i = 0;

static void net_hist_push(unsigned long bps) {
    s_net_hist[s_net_hist_i] = bps;
    s_net_hist_i = (s_net_hist_i + 1) % NET_HIST_N;
    if (s_net_hist_n < NET_HIST_N) s_net_hist_n++;
}

static unsigned long net_hist_avg(void) {
    if (s_net_hist_n == 0) return 0;
    unsigned long sum = 0;
    for (int i = 0; i < s_net_hist_n; i++) sum += s_net_hist[i];
    return sum / (unsigned long)s_net_hist_n;
}

// THE single formatter for a bytes/sec rate, "<n>KB/s" or "<n>MB/s". Both the
// tray gauge value and the performance-popup NET row call this - a second,
// independently-drifting copy of the ">=1024 switches to MB/s" rule is
// exactly how these things go stale. `unit_bps` (pass net_hist_avg()) picks
// the unit; `bps` (pass the current, instantaneous g_net_bps) supplies the
// printed number, so a real burst still moves the number even while the unit
// stays pinned by the average - render the CURRENT value in the AVERAGED
// unit, per the ticket brief. Caller's buffer must be at least 12 bytes: a
// burst can transiently outrun the averaged unit (e.g. a gigabit spike while
// the 10-sample average is still KB/s-scale), so the digit count is not
// bounded by the chosen unit the way it would be if both came from the same
// sample - g_net_bps itself is sanity-clamped in taskbar_update() so this
// can never run past 7 digits.
static void net_fmt_rate(char *buf, unsigned long bps, unsigned long unit_bps) {
    int dp = 0;
    if ((unit_bps / 1024UL) >= 1024UL) {
        dp = sappend_u(buf, dp, (bps / 1024UL) / 1024UL);
        dp = sappend(buf, dp, "MB/s");
    } else {
        dp = sappend_u(buf, dp, bps / 1024UL);
        dp = sappend(buf, dp, "KB/s");
    }
    (void)dp;
}

// Replacement bar scale (decision, see CHANGELOG #76): a LOG scale from
// 1 KB/s (bar empty) to ~128 MB/s / ~1 Gbit/s (bar full - the same physical
// ceiling the old percent scale used), computed as the position of the
// highest set bit so it needs no floating point. Each doubling of throughput
// moves the bar a fixed ~5.9%. On the old linear scale a busy few-MB/s USB
// dongle read as a low single-digit percent forever (the reported bug); on
// this log scale the same traffic reads as solidly mid-bar, which is the
// actual point of a log meter - it makes "the network is busy" visible on
// slow links, not just fast ones. NOT a linear percentage: nothing prints
// this fraction with a "%" sign (see draw_perf_popup's i==3 special case,
// which shows the rate instead) - only the bar FILL uses it. A percentage-
// of-observed-session-peak scale was also considered and rejected: it reads
// 100% on the very first non-zero sample (peak == that sample) and then
// visually "shrinks" every subsequent equal reading as the peak climbs,
// which is a worse illusion of meaning than a fixed, physically-anchored
// scale.
static int net_log_percent(unsigned long bps) {
    if (bps < 1024UL) return 0;
    int bit = 0;
    unsigned long v = bps;
    while (v > 1) { v >>= 1; bit++; }
    const int FLOOR_BIT = 10;   // 2^10 = 1 KB/s
    const int CEIL_BIT  = 27;   // 2^27 ~= 128 MB/s ~= 1 Gbit/s
    if (bit <= FLOOR_BIT) return 0;
    if (bit >= CEIL_BIT)  return 100;
    return (bit - FLOOR_BIT) * 100 / (CEIL_BIT - FLOOR_BIT);
}

// The "NET" label + a percent value ("100%") always fit GAUGE_WIDTH; a
// throughput string can be wider ("999MB/s") and, because the unit is
// averaged rather than instantaneous, occasionally much wider during a
// burst. Drop the label rather than let it collide with or clip the value -
// the value's own shape ("<n>KB/s"/"<n>MB/s") is self-identifying, and the
// gauge's fixed position (4th slot) plus its accent colour already say
// "this is NET" the same way the tray icons rely on position, not text.
static void draw_net_gauge(int32_t x, int32_t y, int32_t w, int32_t h,
                            int percent, uint32_t color, const char *value) {
    draw_fill_rect(x, y, w, h, CLR_GAUGE_BG);
    draw_rect_outline(x, y, w, h, CLR_GAUGE_BORDER);

    // #76 (measured, light theme, two prior designs on this same gauge):
    // text sitting ON TOP of a full-height fill - the CPU/RAM/DSK pattern
    // this originally copied - cannot get a correct ink here. No single ink
    // is right on both sides of a boundary that can fall inside the string
    // (readable_ink(CLR_GAUGE_BG) is 2.42:1 on the NET accent 0x8800CC
    // purple; readable_ink(color) is 2.01:1 on this light theme's own plain
    // background - neither clears the 3:1/4.5:1 floors against the other
    // side), and a solid CLR_GAUGE_BG patch painted behind the text to force
    // a known background MEASURABLY destroyed the bar instead: "NET" +
    // "84KB/s" together span nearly the full 80px width, so the patches
    // erased all but a 2px sliver of the fill top and bottom - a bar that no
    // longer communicates anything, the exact "drawn but meaningless
    // control" this project has shipped before.
    //
    // Fix: stop sharing pixels. The log-scale fill lives in its own thin
    // strip along the bottom of the box; text lives in the row above it,
    // always on plain CLR_GAUGE_BG, one ink, no boundary ever crosses it.
    const int32_t BAR_H = 3;
    int32_t bar_y = y + h - BAR_H - 2;
    draw_fill_rect(x + 2, bar_y, w - 4, BAR_H, CLR_GAUGE_BORDER);   // empty track
    if (percent > 0) {
        int32_t fill_w = (w - 4) * percent / 100;
        if (fill_w < 1) fill_w = 1;
        draw_fill_rect(x + 2, bar_y, fill_w, BAR_H, color);
    }

    int32_t text_y = y + 1;
    uint32_t ink = readable_ink(CLR_GAUGE_BG);
    int label_w = text_width("NET");
    int value_w = text_width(value);
    if (label_w + 4 + value_w + 8 <= w) {
        draw_text(x + 4, text_y, "NET", ink);
        draw_text(x + w - value_w - 4, text_y, value, ink);
    } else {
        draw_text(x + (w - value_w) / 2, text_y, value, ink);
    }
}

// Return the gauge index (0=CPU,1=RAM,2=DSK,3=NET) under (x,y), or -1.
static int gauge_hit(int32_t x, int32_t y) {
    if (g_gauge_x0 == 0) return -1;
    if (y < g_gauge_y || y >= g_gauge_y + GAUGE_HEIGHT) return -1;
    for (int i = 0; i < 4; i++) {
        int32_t gx = g_gauge_x0 + i * (GAUGE_WIDTH + GAUGE_SPACING);
        if (x >= gx && x < gx + GAUGE_WIDTH) return i;
    }
    return -1;
}

// Draw the detailed performance popup, anchored above the gauge block. Themed
// via the menu palette (CLR_MENU_*), drawn last in taskbar_render so it sits on
// top of the bar.
static void draw_perf_popup(void) {
    static const char *names[4]   = { "CPU", "RAM", "DSK", "NET" };
    const uint32_t accents[4]     = { CLR_GAUGE_CPU, CLR_GAUGE_RAM,
                                      CLR_GAUGE_DSK, CLR_GAUGE_NET };
    const int pcts[4]             = { g_cpu_percent, g_ram_percent,
                                      g_disk_percent, g_net_percent };

    const int32_t PAD   = 10;
    const int32_t TITLE = 24;
    const int32_t ROW_H = 44;
    const int32_t BTN_H = 26;
    g_pp_w = 252;
    g_pp_h = TITLE + 4 * ROW_H + 8 + BTN_H + PAD;

    // Right-align with the gauge block; sit just above the taskbar - unless
    // the active style keeps its gauges in a TOP bar (#26 DOCK_XFCE), in
    // which case the popup drops DOWN below it instead. g_tray_bar_top is
    // set by whichever tray_render_core() call ran earlier this frame; only
    // a top-bar style ever passes is_top=1 there, so this stays in sync with
    // the bar's actual position with no separate flag to keep consistent.
    g_pp_x = (g_fb_width - TASKBAR_PADDING) - g_pp_w;
    if (g_pp_x < 2) g_pp_x = 2;
    if (g_tray_bar_top) {
        g_pp_y = g_taskbar_y + 4;
        if (g_pp_y + g_pp_h > g_fb_height) g_pp_y = g_fb_height - g_pp_h - 2;
    } else {
        g_pp_y = g_taskbar_y - g_pp_h - 4;
        if (g_pp_y < 2) g_pp_y = 2;
    }

    // Shadow + panel + border.
    draw_fill_rect(g_pp_x + 4, g_pp_y + 4, g_pp_w, g_pp_h, CLR_MENU_SHADOW);
    draw_fill_rect(g_pp_x, g_pp_y, g_pp_w, g_pp_h, CLR_MENU_BG);
    draw_rect_outline(g_pp_x, g_pp_y, g_pp_w, g_pp_h, CLR_MENU_BORDER);

    // Title bar.
    draw_fill_rect(g_pp_x + 1, g_pp_y + 1, g_pp_w - 2, TITLE - 1, CLR_MENU_CAT_BG);
    draw_text(g_pp_x + PAD, g_pp_y + 7, "System Performance", CLR_MENU_TEXT);

    int32_t ry = g_pp_y + TITLE + 2;
    for (int i = 0; i < 4; i++) {
        int32_t rx = g_pp_x + 6;
        int32_t rw = g_pp_w - 12;

        // Highlight the gauge that opened the popup.
        if (i == g_perf_sel)
            draw_fill_rect(rx, ry, rw, ROW_H - 4, CLR_MENU_ITEM_HOVER);

        // Accent swatch + label + value.
        draw_fill_rect(rx + 4, ry + 6, 10, 10, accents[i]);
        draw_rect_outline(rx + 4, ry + 6, 10, 10, CLR_MENU_BORDER);
        draw_text(rx + 22, ry + 5, names[i], CLR_MENU_TEXT);

        // #76: NET's top-right value is a throughput rate, not a percent -
        // pcts[3] (the bar's log-scale fill, see net_log_percent()) is not a
        // linear percentage and would be a fabricated-looking "NET 62%" next
        // to three genuine percentages. Same shared formatter the tray gauge
        // uses (net_fmt_rate), so this can never disagree with it.
        char pv[16];
        if (i == 3) net_fmt_rate(pv, g_net_bps, net_hist_avg());
        else        fmt_percent(pv, pcts[i]);
        draw_text(rx + rw - text_width(pv) - 6, ry + 5, pv, CLR_MENU_TEXT);

        // Progress bar.
        int32_t bx = rx + 22, by = ry + 19, bw = rw - 28, bh = 8;
        int p = pcts[i]; if (p < 0) p = 0; if (p > 100) p = 100;
        draw_fill_rect(bx, by, bw, bh, CLR_GAUGE_BG);
        if (p > 0) {
            int32_t fw = bw * p / 100; if (fw < 1) fw = 1;
            draw_fill_rect(bx, by, fw, bh, accents[i]);
        }
        draw_rect_outline(bx, by, bw, bh, CLR_GAUGE_BORDER);

        // Detail line.
        char det[64];
        int  dp = 0;
        switch (i) {
            case 0: {  // CPU: core count
                dp = sappend_u(det, dp, (unsigned long)g_cpu_ncores);
                dp = sappend(det, dp, g_cpu_ncores == 1 ? " core" : " cores");
                break;
            }
            case 1: {  // RAM: used / total MB
                dp = sappend_u(det, dp, g_ram_used_b  / 1048576UL);
                dp = sappend(det, dp, " / ");
                dp = sappend_u(det, dp, g_ram_total_b / 1048576UL);
                dp = sappend(det, dp, " MB");
                break;
            }
            case 2: {  // DSK: free of total MB
                long fr = g_disk_free_mb  < 0 ? 0 : g_disk_free_mb;
                long tt = g_disk_total_mb < 0 ? 0 : g_disk_total_mb;
                dp = sappend_u(det, dp, (unsigned long)fr);
                dp = sappend(det, dp, " MB free of ");
                dp = sappend_u(det, dp, (unsigned long)tt);
                dp = sappend(det, dp, " MB");
                break;
            }
            default: { // NET: the bar above is a log scale, not a percent -
                       // say so, since the rate itself is already shown at
                       // the top-right of this row (net_fmt_rate, #76).
                dp = sappend(det, dp, "log scale, ~1 Gbit max");
                break;
            }
        }
        draw_text(rx + 22, ry + 30, det, readable_ink_dim(CLR_MENU_BG));

        ry += ROW_H;
    }

    // "Open Task Manager" button.
    g_pp_tm_w = g_pp_w - 2 * PAD;
    g_pp_tm_h = BTN_H;
    g_pp_tm_x = g_pp_x + PAD;
    g_pp_tm_y = ry + 4;
    draw_fill_rect(g_pp_tm_x, g_pp_tm_y, g_pp_tm_w, g_pp_tm_h, CLR_MENU_CAT_BG);
    draw_rect_outline(g_pp_tm_x, g_pp_tm_y, g_pp_tm_w, g_pp_tm_h, CLR_MENU_BORDER);
    {
        const char *lbl = "Open Task Manager";
        int32_t lx = g_pp_tm_x + (g_pp_tm_w - text_width(lbl)) / 2;
        draw_text(lx, g_pp_tm_y + 9, lbl, CLR_MENU_TEXT);
    }
}

// ============================================================================
// Public API
// ============================================================================

void taskbar_init(void) {
    g_taskbar_y = g_fb_height - TASKBAR_HEIGHT;

    g_cpu_percent  = 0;
    g_ram_percent  = 0;
    g_disk_percent = 0;
    g_net_percent  = 0;   // #76: log-scale fill, starts empty like the others.

    fmt_percent(g_cpu_str,  g_cpu_percent);
    fmt_percent(g_ram_str,  g_ram_percent);
    fmt_percent(g_disk_str, g_disk_percent);
    net_fmt_rate(g_net_str, 0, 0);   // "0KB/s"

    g_gauge_update_time = 0;
}

void taskbar_update(void) {
    uint64_t now = (uint64_t)sys_clock();

    // Throttle updates. #102b idle-CPU fix: sampling the gauges ~10Hz made the
    // CPU%/RAM%/NET% integers jitter (e.g. 2->3->2) every ~100ms; because
    // taskbar_collect_damage() marks the bar dirty on any displayed-value change,
    // that jitter forced a full-screen present (~4MB back->front swap) ~10 times a
    // second on an otherwise idle desktop - the dominant residual idle-CPU cost.
    // A taskbar meter only needs ~1Hz, so sample once a second normally and only
    // speed up (~5Hz) while the detailed performance popup is open so its live
    // graph stays smooth. sys_clock() is ~milliseconds.
    uint64_t interval = g_perf_open ? 200 : 1000;
    if (now - g_gauge_update_time < interval) {
        return;
    }
    g_gauge_update_time = now;

    // CPU usage (0-100) aggregate + per-core (#279).
    g_cpu_percent = sys_get_cpu_usage();
    fmt_percent(g_cpu_str, g_cpu_percent);
    g_cpu_ncores = sys_get_cpu_per_core(g_cpu_cores);
    if (g_cpu_ncores < 1) g_cpu_ncores = 1;
    if (g_cpu_ncores > 64) g_cpu_ncores = 64;

    // RAM usage derived from page counts reported by the kernel.
    {
        unsigned long total = 0;
        unsigned long used  = 0;
        sys_get_mem_info(&total, &used);
        g_ram_total_b = total;
        g_ram_used_b  = used;
        if (total > 0) {
            g_ram_percent = (int)(used * 100UL / total);
        } else {
            g_ram_percent = 0;
        }
        fmt_percent(g_ram_str, g_ram_percent);
    }

    // Disk usage derived from cluster counts.
    {
        long total = sys_get_disk_total();
        long free  = sys_get_disk_free();
        g_disk_total_mb = total;
        g_disk_free_mb  = free;
        if (total > 0) {
            long used = total - free;
            if (used < 0) used = 0;
            g_disk_percent = (int)(used * 100L / total);
        } else {
            g_disk_percent = 0;
        }
        fmt_percent(g_disk_str, g_disk_percent);
    }

    // Network gauge (#76): real bytes/sec, from the ACTUAL elapsed wall time
    // between samples - NOT a hardcoded "~100 ms" assumption. That assumption
    // was stale: the #102b throttle above samples at ~1000ms normally / 200ms
    // with the popup open, not 100ms, so the old `d * 10` math was silently
    // ~10x too high the whole time (a "two comments disagree about the rate"
    // bug, the timer-rate one just above it in blame.md). Value and unit come
    // from net_fmt_rate()/net_log_percent() above, shared with the popup.
    {
        static unsigned long s_last_net_bytes = 0;
        static uint64_t      s_last_net_time  = 0;
        static int           s_net_primed     = 0;
        unsigned long now_bytes = get_net_bytes();
        if (!s_net_primed) {
            s_net_primed = 1;
            g_net_bps = 0;
        } else {
            unsigned long d = now_bytes - s_last_net_bytes;       // bytes since last sample
            uint64_t elapsed_ms = now - s_last_net_time;
            if (elapsed_ms < 1) elapsed_ms = 1;                   // guard div-by-zero
            g_net_bps = d * 1000UL / (unsigned long)elapsed_ms;   // bytes/sec
            // Sanity ceiling (~10 Gbit/s): bounds a counter-wraparound or a
            // stalled-then-caught-up sample to something the buffers below
            // and the log-percent bit walk can never overrun.
            const unsigned long NET_BPS_SANITY_MAX = 1250000000UL;
            if (g_net_bps > NET_BPS_SANITY_MAX) g_net_bps = NET_BPS_SANITY_MAX;
        }
        s_last_net_bytes = now_bytes;
        s_last_net_time  = now;

        net_hist_push(g_net_bps);
        net_fmt_rate(g_net_str, g_net_bps, net_hist_avg());
        g_net_percent = net_log_percent(g_net_bps);
    }
}

// #102: expose the taskbar's sampled CPU% + per-core array so the System Monitor
// widget reads the identical source (the two meters can no longer disagree).
int taskbar_cpu_snapshot(unsigned int *cores, int *ncores)
{
    if (cores) {
        int n = g_cpu_ncores; if (n < 1) n = 1; if (n > 64) n = 64;
        cores[0] = (unsigned int)n;
        for (int i = 1; i <= n; i++) cores[i] = g_cpu_cores[i];
    }
    if (ncores) *ncores = g_cpu_ncores;
    return g_cpu_percent;
}

// #102/#379 dirty-rect: damage the taskbar strip only when something it shows
// actually changed (gauge readings, unread-notification badge, or the set of
// open windows). At idle these are constant, so the taskbar contributes no
// damage and forces no present.
void taskbar_collect_damage(void)
{
    static int l_cpu = -1, l_ram = -1, l_disk = -1, l_net = -1, l_unread = -1, l_wins = -1, l_bt = -1;
    // #76: g_net_percent alone used to BE the displayed text (fmt_percent()
    // straight from it), so watching it was sufficient. Now g_net_str is a
    // KB/s or MB/s rate derived from g_net_bps independently of the coarse
    // (~17-bucket) log-scale percent - the number on screen can change while
    // the bucket does not (e.g. 96KB/s -> 110KB/s can land in the same log
    // bucket), so the bar goes stale under real, changing traffic unless the
    // rate itself is also watched. MEASURED: two screendumps 6s apart during
    // a live flood ping showed 0 changed pixels and the compositor's own
    // frame-present counter frozen for 50+ seconds - this is what caused it.
    static unsigned long l_net_bps = (unsigned long)-1;
    int unread = notif_unread();
    int nwins  = wm_get_windows(g_tb_wins, TB_MAX_WINS);
    if (nwins < 0) nwins = 0;
    // #372/#384: re-sync the shared Bluetooth + Wi-Fi state (self-throttled to
    // ~400ms) so the tray icons follow toggles made in the Settings window.
    static int l_conn = -1, l_min = -1, l_style = -1;
    bt_tick(); wifi_tick();
    int bt = bt_tray_state();
    // Combined connectivity signature: BT state + net-up + wifi tray state.
    int conn = bt * 100 + (sys_net_is_up() ? 10 : 0) + wifi_tray_state();
    // #387: the alternate layouts show a clock in their bar; track the minute so
    // an idle desktop still repaints the bar when the time rolls over.
    // #49: the LOCAL minute. Half-hour and 45-minute zones (India +5:30,
    // Adelaide +9:30, Nepal +5:45, Chatham +12:45) change the minute as well as
    // the hour, so tracking the UTC minute here would repaint the bar at the
    // wrong instant for exactly those users.
    int lh = 0, minute = 0, lsec = 0;
    tz_local_hms(&lh, &minute, &lsec);
    if (g_cpu_percent != l_cpu || g_ram_percent != l_ram || g_disk_percent != l_disk ||
        g_net_percent != l_net || g_net_bps != l_net_bps || unread != l_unread || nwins != l_wins ||
        bt != l_bt || conn != l_conn || minute != l_min || g_dock_style != l_style) {
        l_cpu = g_cpu_percent; l_ram = g_ram_percent; l_disk = g_disk_percent;
        l_net = g_net_percent; l_net_bps = g_net_bps; l_unread = unread; l_wins = nwins; l_bt = bt; l_conn = conn;
        l_min = minute; l_style = g_dock_style;
        // #387: damage the actual bar region(s) for the active layout.
        int ti = taskbar_top_inset(), bi = taskbar_bottom_inset();
        if (ti > 0) damage_add(0, 0, g_fb_width, ti);
        if (g_dock_style == DOCK_LUMINA)
            damage_add(0, g_fb_height - (LUMINA_DOCK_ICON + 2 * LUMINA_DOCK_PAD + 12),
                       g_fb_width, LUMINA_DOCK_ICON + 2 * LUMINA_DOCK_PAD + 12);
        else if (bi > 0)
            damage_add(0, g_fb_height - bi, g_fb_width, bi);
    }
}

// ---- System tray / quick settings (#tray) --------------------------------
extern int g_widgets_enabled;   // widgets.c
extern int g_sheep_enabled;     // widgets.c
int g_tray_muted = 0;   // #336: global so the analog EQ popup (traymenu.c) can toggle it
// Tray slot order (left->right). #384 adds Network next to Sound; #372 Bluetooth.
#define TRAY_WIDGETS  0
#define TRAY_SOUND    1
#define TRAY_NET      2
#define TRAY_BT       3
#define TRAY_SHEEP    4
#define TRAY_BELL     5
#define TRAY_N        6
#define TRAY_ICON_W   26
#define TRAY_ICON_GAP 2
static int32_t g_tray_x[TRAY_N];
static int32_t g_tray_y = 0, g_tray_h = 0;

// ---------------------------------------------------------------------------
// (#745 second follow-up) SINGLE shared vertical-centering rule for every
// tray glyph. The "too high up" report traced to exactly this being absent:
// sound/wifi/bt in the previous pass each hand-picked their own top margin
// (y+3, clip-at-y+0, y+6...) against an assumed top-aligned box, never
// checked against the tray SLOT's own centre. Measured before fixing (local
// y, slot is 26px tall, centre=13): widgets was already centred by luck
// (extent y6-20, centre 13); sound centred at y9 (4px high); wifi centred at
// y7.3 (5.7px high); bluetooth centred at y11.25 (1.75px high). One macro,
// one reference, so an eleventh glyph cannot repeat the mistake.
#define TRAY_GLYPH_Y(y, gh) ((y) + 13 - (gh) / 2)

// Blit a coverage mask (0-255 per pixel, row-major) tinted to `color`,
// through the existing draw_hspan_alpha() - the same per-pixel-alpha
// primitive draw_circle_filled_aa/draw_circle_ring_aa/draw_line_aa already
// use, so this is reuse, not a new rasterizer.
static void tray_blit_mask(int x, int y, int w, int h, const uint8_t *mask, uint32_t color) {
    for (int j = 0; j < h; j++)
        draw_hspan_alpha(x, y + j, w, color, mask + j * w, 1);
}

#include "tray_glyphs.h"
#include "ai_glyphs.h"

// AI command-launcher glyph: a two-star "sparkle" mark. See ai_glyphs.h for
// the full rationale and provenance. Exactly two sizes are ever requested
// (24px by DOCK_DEFAULT's taskbar, 20px by DOCK_XFCE's panel), each
// rasterized offline at that exact size. Blended straight onto whatever is
// already on screen via tray_blit_mask() -> draw_hspan_alpha() (real
// per-pixel alpha compositing against g_fb), so unlike the old /MAYLOGO.DAT
// path there is no separate `bg` argument to keep in sync with chrome_chip()
// - it reads correctly against glass, a hover wash, or a flat bar alike.
static int ai_launcher_icon_draw(int bx, int by, int size, uint32_t ink) {
    if (size == 24) { tray_blit_mask(bx, by, 24, 24, AISTAR_24_MASK, ink); return 1; }
    if (size == 20) { tray_blit_mask(bx, by, 20, 20, AISTAR_20_MASK, ink); return 1; }
    return 0;   // no offline table at this size; caller falls back to text
}

static void tray_draw_widgets(int x, int y, int on) {
    uint32_t c = on ? readable_ink(CLR_TASKBAR_BG) : readable_ink_dim(CLR_TASKBAR_BG);
    // Already satisfies TRAY_GLYPH_Y(y,14) == y+6 - this is the glyph every
    // other one is now sized/centred to match.
    draw_fill_rect(x + 6,  y + 6,  6, 6, c);
    draw_fill_rect(x + 14, y + 6,  6, 6, c);
    draw_fill_rect(x + 6,  y + 14, 6, 6, c);
    draw_fill_rect(x + 14, y + 14, 6, 6, c);
}
// (#745 second follow-up) Real artwork, not hand-drawn primitives: rasterized
// offline (no runtime scaling - the exact defect diagnosed on the old
// /ICONS/SLIDERS.ICN path) from Wikimedia Commons File:Speaker_Icon.svg
// (public domain, author Mobius, 2006) at the exact final size, 18x14. A
// monochrome coverage mask, tinted with readable_ink()/readable_ink_dim() at
// draw time exactly like every other tray glyph, so it flips correctly on
// all 8 dark-bar and 6 light-bar themes - an embedded two-tone bitmap could
// not do that (see the sheep below for what happens when the source art
// genuinely needs two tones).
static void tray_draw_sound(int x, int y, int muted) {
    uint32_t c = muted ? readable_ink_dim(CLR_TASKBAR_BG) : readable_ink(CLR_TASKBAR_BG);
    int gy = TRAY_GLYPH_Y(y, 14);
    tray_blit_mask(x + 4, gy, 18, 14, SPEAKER_MASK, c);
    if (muted)
        for (int i = 0; i < 13; i++) draw_putpixel(x + 6 + i, y + 6 + i, 0x00FF4040);
}
// (#745 second follow-up) User-supplied artwork:
// <workspace> sheep.png (40x37 RGBA), pre-scaled OFFLINE
// with area averaging (PIL BOX filter) to 15x14, never at runtime. The
// filename's space is not carried anywhere - the processed pixels are
// embedded directly as two coverage tables (tray_glyphs.h), so there is no
// asset-pipeline file to name at all.
//
// TWO-TONE PROBLEM: the source is genuinely two-tone (cream fleece, BLACK
// head/legs/outline) and a literal recolor of the whole thing to one
// readable_ink() tone would flatten it back into a blob. But the source's
// black head is also exactly the earlier bug: black-on-near-black-bar
// vanishes, recreating "looks like a cloud, no head". MEASURED: on
// maytera_dark (bar (26,30,36)), a literal black head at (20,21,24) sits at
// ~1.05:1 against the bar - invisible. Tried and rejected: recoloring head
// and fleece to the theme's two safe tones (readable_ink + readable_ink_dim)
// keeps BOTH parts light-on-dark (or dark-on-light) at once, losing the
// internal head/body contrast that makes the source read as a sheep at all
// (rendered and looked at - see the design doc follow-up; it reads as a
// blob at every size up to 30px, larger than the whole tray row).
//
// FIX: silhouette (the WHOLE sheep, both materials combined) is filled first
// in readable_ink(CLR_TASKBAR_BG) - always legible against the bar, same as
// every other glyph. Then just the head/legs/outline material is filled in
// readable_ink() OF THAT COLOUR, i.e. the opposite extreme - always maximum
// contrast against its own body, regardless of theme. MEASURED: dark bar,
// silhouette (240,240,240), head (26,26,26) -> 16.05:1 head-to-body. Light
// bar (180,180,180), silhouette (26,26,26), head (240,240,240) -> 8.39:1
// head-to-body. The head does NOT stay literally black on light bars (it
// becomes white-on-black) - that is not the source art's original
// colouring, but the alternative (a literal black head) is the bug this
// replaces.
static void tray_draw_sheep(int x, int y, int on) {
    uint32_t silhouette = on ? readable_ink(CLR_TASKBAR_BG) : readable_ink_dim(CLR_TASKBAR_BG);
    uint32_t head = readable_ink(silhouette);
    int gy = TRAY_GLYPH_Y(y, 14);
    tray_blit_mask(x + 5, gy, 15, 14, SHEEP_SILH, silhouette);
    tray_blit_mask(x + 5, gy, 15, 14, SHEEP_HEAD, head);
}

static void tray_draw_bell(int x, int y, int unread) {
    uint32_t c = readable_ink(CLR_TASKBAR_BG);
    int cx = x + 12, cy = y + 12;
    // (#745) Dome AA'd: was a hard-edge draw_circle_filled at 10px diameter,
    // under the style guide's own "14px+ or not at all" floor for hard-edge
    // circles - a coverage-based edge does not have that small-radius failure
    // mode, so the dome is viable at its existing size.
    draw_circle_filled_aa(cx, cy, 5.0f, c, 255);   // bell dome
    draw_fill_rect(cx - 6, cy, 13, 5, c);        // skirt
    draw_fill_rect(cx - 1, y + 3, 3, 3, c);      // top knob
    draw_fill_rect(cx - 7, cy + 5, 15, 2, c);    // base rim
    draw_fill_rect(cx - 1, cy + 7, 3, 3, c);     // clapper
    if (unread > 0) {
        // (#745) Three measured defects fixed together (see
        // docs/TASKBAR_AND_TRAY.html section 10):
        //  1. Badge moved from center (bx, y+3) r=7 - top edge at
        //     taskbar_y+1, one row below the bar's own top hairline and
        //     directly on the highlight row - to (bx, y+16) r=6, clearing
        //     both border rows with 10px to spare.
        //  2. AA circle instead of hard-edge (also removes the small-radius
        //     chunkiness the old 14px-diameter badge had).
        //  3. Digit requested at size 12 (an exact glyph-cache bucket -
        //     {12,14,16,...}, no 11 - so it no longer snaps from a value that
        //     was never really rendered) with the vertical position computed
        //     from real font metrics instead of a hand-tuned constant that
        //     assumed the old badge geometry.
        int bx  = x + TRAY_ICON_W - 8;
        int bcy = y + 16;
        draw_circle_filled_aa(bx, bcy, 6.0f, 0xFFE5484D, 255);   // unread badge
        char n[3];
        if (unread > 9) { n[0]=57; n[1]=43; n[2]=0; } else { n[0]=(char)(48+unread); n[1]=0; }
        int tw = text_width_ttf(n, 12);
        int ty = bcy - 6;   // fallback if font_metrics() ever fails
        int fm[3];
        if (font_metrics(0, 12, fm) == 0)
            ty = bcy - (fm[0] - fm[1]) / 2;   // fm = {ascent, descent, line_gap}
        draw_text_ttf(bx - tw / 2, ty, n, 12, 0xFFFFFFFF);
    }
}

// #372: Bluetooth indicator. state 0 = off (dim), 1 = on/idle,
// 2 = on/connected (rune + small link dot).
//
// (#745 second follow-up) Real artwork: rasterized offline from Wikimedia
// Commons File:Bluetooth.svg (public domain for copyright - "simple geometry
// ... ineligible"; the rune/word mark itself carries a Bluetooth SIG
// trademark notice, used here only as the conventional OS status glyph, the
// same shape every OS uses) at the exact final size, 11x14 - the background
// plate in the source file was dropped, only the rune path kept. Narrower
// than its neighbours by nature of the rune's own proportions (matches the
// first follow-up's finding that bluetooth is "the narrowest glyph in the
// tray by nature of the shape" - still true of the real mark, not just a
// hand-drawn approximation of it). The earlier hand-drawn draw_line_aa()
// version is superseded by this real artwork per the user's later direction
// (use real icon references, do not hand-draw); draw_line_aa() itself stays
// in draw.c as a shared primitive - nothing else in this file used it, but
// removing a primitive because its one caller changed is not this ticket's
// job, and a later glyph may still want it.
static void tray_draw_bt(int x, int y, int state) {
    uint32_t c = (state == 0) ? readable_ink_dim(CLR_TASKBAR_BG)
                              : readable_ink(CLR_TASKBAR_BG);
    int gy = TRAY_GLYPH_Y(y, 14);
    int gx = x + 7;   // (26-11)/2 = 7.5 -> 7
    tray_blit_mask(gx, gy, 11, 14, BT_MASK, c);
    if (state == 2) {
        // connected link dot, tucked against the rune's top-right corner.
        draw_circle_filled_aa(gx + 11, gy + 1, 2.2f, readable_ink(CLR_TASKBAR_BG), 255);
    }
}

// #384: Network indicator. state 0 = disconnected (dim, slashed), 1 = wired
// connected (real net-up: monitor + plug glyph), 2 = Wi-Fi connected (arcs with
// `bars` filled from the mock signal).
static void tray_draw_net(int x, int y, int state, int bars) {
    (void)bars;   // (#745 second follow-up) per-bar ring granularity dropped, see below
    if (state == 1) {
        // Wired: little monitor/ethernet plug.
        uint32_t c = readable_ink(CLR_TASKBAR_BG);
        draw_rect_outline(x + 5, y + 6, 16, 11, c);
        draw_fill_rect(x + 10, y + 17, 6, 2, c);
        draw_fill_rect(x + 8, y + 19, 10, 2, c);
        return;
    }
    // Wi-Fi. (#745 second follow-up) Real artwork: area-averaged offline
    // (PIL BOX filter, never at runtime) from Wikimedia Commons
    // File:WiFi_icon.png (96x96, CC0, author Lycopene579) to the exact final
    // size, 18x14. The user's original Vexels reference is CC BY-SA 4.0
    // (attribution + share-alike, entangling for this GPLv2 repo) so was not
    // used; a second CC0 candidate, File:Wifi_symbol.svg, turned out on
    // inspection to be a radio-tower broadcast emblem, the wrong visual
    // metaphor for a tray status icon, so this bars-and-dot CC0 PNG was used
    // instead - it is the conventional shape the user's own reference had.
    //
    // This replaces the earlier hand-drawn draw_circle_ring_aa() rings AND
    // their per-bar lit/dim state (each ring individually showing signal
    // strength). A single flat raster mask cannot selectively relight one
    // ring - that granularity is dropped; the whole glyph is either dim
    // (disconnected) or full ink (connected), which is a real, deliberate
    // simplification traded for using real artwork, not an oversight.
    uint32_t ink = readable_ink(CLR_TASKBAR_BG);
    uint32_t dim = readable_ink_dim(CLR_TASKBAR_BG);
    int gy = TRAY_GLYPH_Y(y, 14);
    tray_blit_mask(x + 4, gy, 18, 14, WIFI_MASK, (state == 2) ? ink : dim);
    if (state == 0)  // disconnected slash in dim ink (consistent, not red)
        for (int i = 0; i < 18; i++) draw_putpixel(x + 4 + i, y + 21 - i, dim);
}

// #387: tray metrics + top/bottom anchor state, shared by every dock layout.
int      g_tray_bar_top = 0;   // 1 = tray sits on a TOP bar (menus drop DOWN)
int32_t  g_tray_bar_y   = 0;   // current tray-row top (traymenu anchors to it)
int32_t  g_tray_bar_h   = 26;
static int tray_total_w(void) { return TRAY_N * TRAY_ICON_W + (TRAY_N - 1) * TRAY_ICON_GAP; }

// Render the 6-icon system tray starting at (x0,y). The glyph tint follows the
// current bar background: we briefly retarget CLR_TASKBAR_BG (readable_ink's
// reference) to bar_bg so a glass/blue/beveled bar still gets legible icons,
// then restore it. is_top marks a top-bar tray so its menus drop downward.
static void tray_render_core(int x0, int y, uint32_t bar_bg, int is_top) {
    // #372/#384: re-sync shared BT/Wi-Fi state every frame (self-throttled).
    bt_tick(); wifi_tick();
    g_tray_y = y; g_tray_h = 26;
    g_tray_bar_y = y; g_tray_bar_h = 26; g_tray_bar_top = is_top;
    for (int i = 0; i < TRAY_N; i++) g_tray_x[i] = x0 + i * (TRAY_ICON_W + TRAY_ICON_GAP);
    // Network state: REAL wired link wins; otherwise the Wi-Fi mock.
    int net_state, net_bars = 0;
    if (sys_net_is_up()) net_state = 1;
    else if (wifi_tray_state() == 2) {
        net_state = 2;
        int sig = wifi_tray_signal();
        net_bars = sig >= 75 ? 3 : sig >= 45 ? 2 : sig > 0 ? 1 : 0;
    } else net_state = 0;
    uint32_t saved = CLR_TASKBAR_BG;
    CLR_TASKBAR_BG = bar_bg;
    tray_draw_widgets(g_tray_x[TRAY_WIDGETS], g_tray_y, g_widgets_enabled);
    tray_draw_sound  (g_tray_x[TRAY_SOUND],   g_tray_y, g_tray_muted);
    tray_draw_net    (g_tray_x[TRAY_NET],     g_tray_y, net_state, net_bars);
    tray_draw_bt     (g_tray_x[TRAY_BT],      g_tray_y, bt_tray_state());
    tray_draw_sheep  (g_tray_x[TRAY_SHEEP],   g_tray_y, g_sheep_enabled);
    tray_draw_bell   (g_tray_x[TRAY_BELL],    g_tray_y, notif_unread());
    CLR_TASKBAR_BG = saved;
}

static void tray_render(int right_edge) {
    int x0 = right_edge - tray_total_w();
    int y  = g_taskbar_y + (TASKBAR_HEIGHT - 26) / 2;
    draw_vline(x0 - 6, g_taskbar_y + 6, TASKBAR_HEIGHT - 12, CLR_GAUGE_BORDER);
    // (#745) chrome_surface_bg(), not CLR_TASKBAR_BG: the bar is glass-tinted
    // when g_glass_enable, and tray_render_core uses this value only to pick
    // readable ink for each glyph (readable_ink(CLR_TASKBAR_BG) inside each
    // tray_draw_* - see the temporary-override trick below). Passing the flat
    // opaque token here computed contrast against a colour that is no longer
    // what is actually on screen under the glyphs.
    tray_render_core(x0, y, chrome_surface_bg(), 0);
}

// Shared tray hit-test: works for every layout because each records g_tray_x[]
// + g_tray_y/g_tray_h before a click can arrive. Returns true if consumed.
static bool tray_click(int32_t x, int32_t y) {
    if (g_tray_h <= 0 || y < g_tray_y || y >= g_tray_y + g_tray_h) return false;
    for (int i = 0; i < TRAY_N; i++) {
        if (x >= g_tray_x[i] && x < g_tray_x[i] + TRAY_ICON_W) {
            if (i == TRAY_BELL) notif_toggle_center();          // #168 bell -> center
            else traymenu_open_for_icon(i, g_tray_x[i] + TRAY_ICON_W / 2);
            g_needs_redraw = true;
            return true;
        }
    }
    return false;
}

static void taskbar_render_default(void) {
    // a) Surface. (#745) Glass: the blurred backdrop tinted at the user's
    // dock_opacity, replacing the plain opaque CLR_TASKBAR_BG fill. Bleed is
    // taken on the top side only; the other three are screen edges.
    glass_or_flat(0, g_taskbar_y, g_fb_width, TASKBAR_HEIGHT, GLASS_SURF_PANEL);

    // b) Top border line, then a 1px inner highlight below it.
    glass_edge_h(0, g_taskbar_y, g_fb_width, CLR_TASKBAR_BORDER);
    glass_highlight_h(0, g_taskbar_y + 1, g_fb_width);

    // c) Start button on the left.
    int32_t btn_x = TASKBAR_PADDING;
    int32_t btn_y = g_taskbar_y + (TASKBAR_HEIGHT - TASKBAR_BTN_SIZE) / 2;

    // (#745) No plate: ink direct on the bar, same as every other tray icon.
    uint32_t chip_ink, chip_bg;
    chrome_chip(btn_x, btn_y, TASKBAR_BTN_SIZE, g_start_menu_open, &chip_ink, &chip_bg);
    (void)chip_bg;   // Start uses a solid ICON, no bitmap alpha edge to knock out against

    // Icon is drawn 2px inside the button bounds.
    icon_draw_scaled(ICON_CATEGORIES, btn_x + 2, btn_y + 2, 24, chip_ink);

    // c2) AI command-launcher (Spotlight-style) button, immediately to the
    // right of Start. Opens the centered AI prompt overlay (launcher.c).
    int32_t logo_x = btn_x + TASKBAR_BTN_SIZE + TASKBAR_ICON_SPACE;
    int32_t logo_y = btn_y;
    uint32_t logo_ink, logo_bg;
    chrome_chip(logo_x, logo_y, TASKBAR_BTN_SIZE, g_launcher_open, &logo_ink, &logo_bg);
    (void)logo_bg;   // ai_launcher_icon_draw blends by real alpha; no bg needed
    if (!ai_launcher_icon_draw(logo_x + 2, logo_y + 2, TASKBAR_BTN_SIZE - 4, logo_ink)) {
        // Defensive fallback if a future caller ever asks for a third size.
        draw_text_large(logo_x + 8, logo_y + 6, "M", logo_ink, 2);
    }

    // d) Four gauges, right-aligned.
    // Total block width occupied by all gauges.
    int32_t gauge_block_w = 4 * GAUGE_WIDTH + 3 * GAUGE_SPACING;
    int32_t gauge_start_x = g_fb_width - TASKBAR_PADDING - gauge_block_w;

    // c2) Open-window buttons, filling the strip between the start button and
    // the gauges. Refresh the list every frame so it stays current.
    {
        int n = wm_get_windows(g_tb_wins, TB_MAX_WINS);
        if (n < 0) n = 0;
        g_tb_win_count = n;

        // Count app windows (visible + titled).
        int app_count = 0;
        for (int i = 0; i < n; i++) {
            if (tb_window_is_app(&g_tb_wins[i])) app_count++;
        }

        // Start with the logo-launcher button occupying one more slot after Start.
        int32_t strip_x = logo_x + TASKBAR_BTN_SIZE + TASKBAR_ICON_SPACE;
        int32_t tray_reserve = TRAY_N * TRAY_ICON_W + (TRAY_N - 1) * TRAY_ICON_GAP + 14;
    int32_t strip_end = gauge_start_x - TASKBAR_ICON_SPACE - tray_reserve;
        int32_t strip_w = strip_end - strip_x;

        g_tb_btn_n = 0;
        if (app_count > 0 && strip_w > 40) {
            // Fit buttons in the strip: preferred width TB_BTN_W, shrink if needed.
            int32_t bw = TB_BTN_W;
            int32_t need = app_count * (bw + TB_BTN_GAP);
            if (need > strip_w) bw = (strip_w / app_count) - TB_BTN_GAP;
            if (bw < TB_ICON_SZ + 8) bw = TB_ICON_SZ + 8;   // at least fit the icon
            g_tb_btn_w = bw;

            int32_t by = g_taskbar_y + 6;
            int32_t bh = TASKBAR_HEIGHT - 12;               // compact height
            int32_t bx = strip_x;
            g_tb_btn_y = by;
            g_tb_btn_h = bh;

            for (int i = 0; i < n; i++) {
                if (!tb_window_is_app(&g_tb_wins[i])) continue;
                if (bx + bw > strip_end) break;

                int is_focused = g_tb_wins[i].focused && !g_tb_wins[i].minimized;
                int is_min     = g_tb_wins[i].minimized;
                // Focused window: lighter "pressed/active" button. Minimized:
                // dimmer. Otherwise the normal hover shade.
                uint32_t bg  = is_focused ? CLR_START_BTN
                             : (is_min ? CLR_TASKBAR_BG : CLR_TASKBAR_HOVER);
                uint32_t txt = is_min ? CLR_GAUGE_BORDER : CLR_CHROME_TEXT;
                draw_fill_rect(bx, by, bw, bh, bg);
                // Focused tile gets a subtle mid-grey outline (never white); other
                // tiles keep the themed gauge border. (desktop UX pass)
                extern uint32_t CLR_TASK_FOCUS_BORDER;
                draw_rect_outline(bx, by, bw, bh,
                                  is_focused ? CLR_TASK_FOCUS_BORDER : CLR_GAUGE_BORDER);

                // Icon on the left, vertically centred.
                int32_t icon_y = by + (bh - TB_ICON_SZ) / 2;
                icon_draw_scaled(tb_icon_for_title(g_tb_wins[i].title),
                                 bx + 3, icon_y, TB_ICON_SZ, txt);

                // Label after the icon. #341: measure the pixel width and
                // ellipsize ("Maytera HiFi...") instead of hard-cutting mid-glyph.
                int32_t text_x = bx + 3 + TB_ICON_SZ + 4;
                int32_t avail  = (bx + bw) - text_x - 3;
                if (avail > 6) {
                    char label[72];
                    int  n = 0;
                    while (g_tb_wins[i].title[n] && n < 63) {
                        label[n] = g_tb_wins[i].title[n]; n++;
                    }
                    label[n] = '\0';
                    if (text_width(label) > avail) {
                        // Trim characters until title + "..." fits the width.
                        int ew = text_width("...");
                        while (n > 0 && text_width(label) + ew > avail)
                            label[--n] = '\0';
                        label[n] = '.'; label[n + 1] = '.';
                        label[n + 2] = '.'; label[n + 3] = '\0';
                    }
                    draw_text(text_x, by + (bh - 8) / 2, label, txt);
                }

                // Record hitbox + focus state for click handling.
                if (g_tb_btn_n < TB_MAX_WINS) {
                    g_tb_btn_x[g_tb_btn_n]       = bx;
                    g_tb_btn_id[g_tb_btn_n]      = g_tb_wins[i].id;
                    g_tb_btn_focused[g_tb_btn_n] = is_focused;
                    g_tb_btn_n++;
                }

                bx += bw + TB_BTN_GAP;
            }
        } else {
            g_tb_btn_w = 0;
        }
    }
    int32_t gauge_y       = g_taskbar_y + (TASKBAR_HEIGHT - GAUGE_HEIGHT) / 2;

    // Record gauge geometry so gauge_hit()/draw_perf_popup() can use it (#241).
    g_gauge_x0 = gauge_start_x;
    g_gauge_y  = gauge_y;

    // CPU gauge: per-core vertical bars (#279).
    draw_cpu_gauge(gauge_start_x,
                   gauge_y,
                   GAUGE_WIDTH, GAUGE_HEIGHT,
                   g_cpu_ncores, g_cpu_cores);

    // RAM gauge.
    draw_gauge(gauge_start_x + (GAUGE_WIDTH + GAUGE_SPACING),
               gauge_y,
               GAUGE_WIDTH, GAUGE_HEIGHT,
               g_ram_percent, CLR_GAUGE_RAM,
               "RAM", g_ram_str);

    // Disk gauge.
    draw_gauge(gauge_start_x + 2 * (GAUGE_WIDTH + GAUGE_SPACING),
               gauge_y,
               GAUGE_WIDTH, GAUGE_HEIGHT,
               g_disk_percent, CLR_GAUGE_DSK,
               "DSK", g_disk_str);

    // Network gauge (#76): throughput value, log-scale fill - see
    // draw_net_gauge()/net_log_percent() above.
    draw_net_gauge(gauge_start_x + 3 * (GAUGE_WIDTH + GAUGE_SPACING),
                   gauge_y,
                   GAUGE_WIDTH, GAUGE_HEIGHT,
                   g_net_percent, CLR_GAUGE_NET, g_net_str);

    // Quick-settings system tray just left of the gauges.
    tray_render(gauge_start_x - 8);

    // #241: detailed performance popup, drawn on top of the bar.
    if (g_perf_open) draw_perf_popup();
}

static bool taskbar_handle_default(int32_t x, int32_t y, bool clicked) {
    // Not inside the taskbar strip.
    if (y < g_taskbar_y) {
        return false;
    }

    // System-tray icons: widgets toggle, sound mute, sheep toggle.
    if (clicked && tray_click(x, y)) return true;

    // #241: click a gauge to open the detailed performance popup.
    if (clicked) {
        int gh = gauge_hit(x, y);
        if (gh >= 0) {
            g_perf_open = 1;
            g_perf_sel  = gh;
            g_needs_redraw = true;
            return true;
        }
    }

    // Check whether the click landed on the start button.
    int32_t btn_x = TASKBAR_PADDING;
    int32_t btn_y = g_taskbar_y + (TASKBAR_HEIGHT - TASKBAR_BTN_SIZE) / 2;

    // Maytera-logo command-launcher button sits directly after Start.
    int32_t logo_x = btn_x + TASKBAR_BTN_SIZE + TASKBAR_ICON_SPACE;

    if (clicked &&
        x >= btn_x && x < btn_x + TASKBAR_BTN_SIZE &&
        y >= btn_y && y < btn_y + TASKBAR_BTN_SIZE) {
        // Toggle start menu visibility.
        g_start_menu_open = !g_start_menu_open;
        g_needs_redraw    = true;
    } else if (clicked &&
               x >= logo_x && x < logo_x + TASKBAR_BTN_SIZE &&
               y >= btn_y && y < btn_y + TASKBAR_BTN_SIZE) {
        // Open/close the centered AI command launcher (Spotlight).
        launcher_toggle();
        g_start_menu_open = false;    // never leave the start menu behind it
        g_needs_redraw    = true;
    } else if (clicked && g_tb_btn_w > 0 &&
               y >= g_tb_btn_y && y < g_tb_btn_y + g_tb_btn_h) {
        // Click on an open-window button: focus/raise that window.
        for (int i = 0; i < g_tb_btn_n; i++) {
            if (x >= g_tb_btn_x[i] && x < g_tb_btn_x[i] + g_tb_btn_w) {
                // Windows-style toggle: clicking the already-focused app's
                // button minimizes it; otherwise focus/raise (and restore).
                if (g_tb_btn_focused[i]) wm_minimize(g_tb_btn_id[i]);
                else                     wm_focus(g_tb_btn_id[i]);
                g_needs_redraw = true;
                break;
            }
        }
    }

    // The event is consumed by the taskbar regardless of where in the strip
    // it landed, so underlying desktop or windows do not receive it.
    return true;
}

// ============================================================================
// #387 Alternate dock/taskbar layouts
// ============================================================================
// g_dock_style selects the active layout; DOCK_DEFAULT is the classic bar above
// (byte-identical). The three alternates below each: draw their chrome + the
// shared 6-icon system tray, expose their bar geometry for menu anchoring, and
// hit-test the launcher, running-app switcher, and tray. They keep the desktop
// work area correct via taskbar_top_inset()/taskbar_bottom_inset().

int g_dock_style = DOCK_DEFAULT;
extern int g_draw_blend;   // draw.c global alpha (255 = opaque)

// (Layout metrics LUMINA_*/CLASSIC_UNIX_*/RETRO_BENCH_* are #defined at the top of this file.)

// Shared running-app / launcher slot records (Lumina / Classic UNIX / Retro Bench hit-testing).
// id == -1 marks the launcher/start slot; otherwise it is a window id.
#define LAY_MAX 24
static int32_t g_lay_x[LAY_MAX], g_lay_w[LAY_MAX], g_lay_id[LAY_MAX];
static int     g_lay_focused[LAY_MAX];
static int     g_lay_n;
static int32_t g_lay_y, g_lay_h;
static void lay_reset(int32_t y, int32_t h) { g_lay_n = 0; g_lay_y = y; g_lay_h = h; }
static void lay_add(int32_t x, int32_t w, int32_t id, int focused) {
    if (g_lay_n >= LAY_MAX) return;
    g_lay_x[g_lay_n] = x; g_lay_w[g_lay_n] = w;
    g_lay_id[g_lay_n] = id; g_lay_focused[g_lay_n] = focused; g_lay_n++;
}
static bool lay_click(int32_t x, int32_t y) {
    if (y < g_lay_y || y >= g_lay_y + g_lay_h) return false;
    for (int i = 0; i < g_lay_n; i++) {
        if (x >= g_lay_x[i] && x < g_lay_x[i] + g_lay_w[i]) {
            int32_t id = g_lay_id[i];
            if (id < 0)               g_start_menu_open = !g_start_menu_open;
            else if (g_lay_focused[i]) wm_minimize(id);
            else                       wm_focus(id);
            g_needs_redraw = true;
            return true;
        }
    }
    return false;
}

// #26: XFCE dock icon-slot hit-list. A slot is one of:
//   PINNED  - a favorite that is not currently running (launches by path)
//   MERGED  - a favorite that IS currently running (focus/minimize, like a
//             normal taskbar tile, but drawn in the pinned row)
//   RUNNING - a running window that is not pinned (focus/minimize)
#define XDOCK_MAX 30
#define XDOCK_KIND_PINNED   0
#define XDOCK_KIND_MERGED   1
#define XDOCK_KIND_RUNNING  2
static int32_t g_xdock_x[XDOCK_MAX];
static int     g_xdock_kind[XDOCK_MAX];
static int32_t g_xdock_win_id[XDOCK_MAX];    // valid for MERGED/RUNNING
// #44: was "valid for PINNED/MERGED" only. Now also carries a RUNNING slot's
// exec_path once resolved by the #41 app_id reverse-lookup
// (startmenu_find_by_app_id()), so "Pin to Dock"/"Change Icon" have a real
// target on an unpinned running tile too - empty means "identity
// unresolved", callers must omit those actions, not guess.
static char    g_xdock_path[XDOCK_MAX][128];
static int     g_xdock_launch_type[XDOCK_MAX];
static int     g_xdock_focused[XDOCK_MAX];
// #44: per-slot icon_id (meaningful iff g_xdock_path[i][0] != 0) and app_id
// (meaningful iff a window backs this slot - MERGED/RUNNING). Both feed the
// right-click menu (contextmenu_open_for_dock()); app_id alone also feeds
// Force Quit, independent of whether the identity resolved to a full
// exec_path.
static int     g_xdock_icon_id[XDOCK_MAX];
static char    g_xdock_app_id[XDOCK_MAX][32];
static int     g_xdock_n;
static int32_t g_xdock_y, g_xdock_h;   // dock icon-row hit-test band

static void xdock_reset(int32_t y, int32_t h) { g_xdock_n = 0; g_xdock_y = y; g_xdock_h = h; }

static void xdock_add(int32_t x, int kind, int32_t win_id, const char *path,
                      int launch_type, int focused, int icon_id, const char *app_id) {
    if (g_xdock_n >= XDOCK_MAX) return;
    int i = g_xdock_n;
    g_xdock_x[i] = x; g_xdock_kind[i] = kind; g_xdock_win_id[i] = win_id;
    if (path) { strncpy(g_xdock_path[i], path, 127); g_xdock_path[i][127] = 0; }
    else g_xdock_path[i][0] = 0;
    g_xdock_launch_type[i] = launch_type; g_xdock_focused[i] = focused;
    g_xdock_icon_id[i] = icon_id;
    if (app_id) { strncpy(g_xdock_app_id[i], app_id, 31); g_xdock_app_id[i][31] = 0; }
    else g_xdock_app_id[i][0] = 0;
    g_xdock_n++;
}

// #49: "HH:MM" in the user's chosen zone. THE single formatter for every dock
// style's bar clock (Lumina, Classic Unix, Retro Bench, XFCE panel), so the
// offset lands on all four by construction rather than four times over.
static void tb_clock_str(char *out) {
    int h = 0, m = 0, s = 0;
    tz_local_hms(&h, &m, &s);
    out[0] = (char)('0' + (h / 10) % 10); out[1] = (char)('0' + h % 10);
    out[2] = ':';
    out[3] = (char)('0' + (m / 10) % 10); out[4] = (char)('0' + m % 10);
    out[5] = '\0';
}

// Refresh g_tb_wins/g_tb_win_count for the alternate layouts.
static int lay_refresh_windows(void) {
    int n = wm_get_windows(g_tb_wins, TB_MAX_WINS);
    if (n < 0) n = 0;
    g_tb_win_count = n;
    return n;
}

// A 3D bevel frame (Motif). raised: light top/left, dark bottom/right.
static void bevel(int32_t x, int32_t y, int32_t w, int32_t h, int raised,
                  uint32_t lt, uint32_t dk) {
    uint32_t tl = raised ? lt : dk;
    uint32_t br = raised ? dk : lt;
    draw_hline(x, y, w, tl);              draw_vline(x, y, h, tl);
    draw_hline(x, y + h - 1, w, br);      draw_vline(x + w - 1, y, h, br);
}

// ---------------------------------------------------------------------------
// Lumina: top glass menu bar + floating bottom glass dock.
// ---------------------------------------------------------------------------
static int32_t g_lumina_menu_w = 64;   // clickable width of the left "Maytera" menu

static void taskbar_render_lumina(void) {
    g_gauge_x0 = 0;   // no gauges -> disable perf-popup hit testing
    int W = g_fb_width, mb = LUMINA_MENUBAR_H;
    int n = lay_refresh_windows();

    // ---- Top menu bar ----
    // (#745) This bar was already translucent, but with a HARDCODED colour and
    // a HARDCODED alpha of 205 that no theme and no user could reach, and with
    // no blur behind it. It now uses the same material as every other chrome
    // surface: the themed glass tint, the user's dock_opacity, and the real
    // blurred backdrop. The old 0xFFF3F4F7 is close to the light glass tint, so
    // a light theme looks essentially unchanged at the default opacity.
    uint32_t glass = g_glass_enable ? CLR_GLASS_TINT : 0xFFF3F4F7;
    glass_or_flat(0, 0, W, mb, GLASS_SURF_PANEL);
    glass_edge_h(0, mb - 1, W, CLR_TASKBAR_BORDER);
    uint32_t ink = readable_ink(glass);
    int ob;

    // Left: system/app menu label (opens start menu). Bold-ish Maytera + app.
    draw_text_ttf(12, 5, "Maytera", 15, ink);
    g_lumina_menu_w = text_width_ttf("Maytera", 15) + 20;
    for (int i = 0; i < n; i++) {
        if (g_tb_wins[i].focused && g_tb_wins[i].visible && !g_tb_wins[i].minimized &&
            tb_window_is_app(&g_tb_wins[i])) {
            draw_text_ttf(g_lumina_menu_w + 6, 6, g_tb_wins[i].title, 13, ink);
            break;
        }
    }

    // Right: clock then the system tray (glassy).
    char clk[6]; tb_clock_str(clk);
    int clkw = text_width_ttf(clk, 13);
    int clk_x = W - 12 - clkw;
    draw_text_ttf(clk_x, 6, clk, 13, ink);
    tray_render_core(clk_x - 8 - tray_total_w(), (mb - 26) / 2, glass, 1);

    // ---- Bottom floating dock (glass pill, magnifying icons) ----
    int items = 1;   // launcher + running apps
    for (int i = 0; i < n; i++) if (tb_window_is_app(&g_tb_wins[i])) items++;
    int icon = LUMINA_DOCK_ICON, pad = LUMINA_DOCK_PAD;
    int dock_w = items * icon + (items + 1) * pad;
    if (dock_w > W - 24) dock_w = W - 24;
    int dh = icon + 2 * pad;
    int dx = (W - dock_w) / 2;
    int dy = g_fb_height - dh - 8;

    // (#745) The floating pill follows dock_opacity too, so one slider governs
    // the whole style. It stays TIER 3 (flat tint, no blur): glass_render works
    // on rectangles, and squaring off an 18px-radius pill to blur it would be a
    // worse regression than the missing blur. The pill's rounded silhouette is
    // the point of the Lumina style.
    ob = g_draw_blend;
    g_draw_blend = g_glass_enable ? (g_dock_opacity * 255 / 100) : 175;
    draw_rounded_rect(dx, dy, dock_w, dh, 18,
                      g_glass_enable ? CLR_GLASS_TINT : 0xFFECEDF1);
    g_draw_blend = ob;
    // Subtle top highlight line inside the pill.
    ob = g_draw_blend; g_draw_blend = 90;
    draw_hline(dx + 18, dy + 1, dock_w - 36, 0xFFFFFFFF);
    g_draw_blend = ob;

    lay_reset(dy, dh);
    int step = icon + pad;
    int base_y = dy + pad + icon;      // icon baseline (bottom) so they grow up
    int mouse_in = (g_mouse_y >= dy - 24 && g_mouse_y <= g_fb_height);
    int slot = 0;

    // Draw one dock slot: base cell at index `slot`, icon id, running/focused.
    // Magnifies with cursor proximity; hit-tests on the base cell.
    #define LUMINA_SLOT_CX(s) (dx + pad + (s) * step + icon / 2)
    // Launcher (Categories/start icon).
    {
        int cx = LUMINA_SLOT_CX(slot);
        int sz = icon, dist = g_mouse_x - cx; if (dist < 0) dist = -dist;
        if (mouse_in && dist < 70) sz = icon + (70 - dist) * 20 / 70;
        icon_draw_scaled(ICON_CATEGORIES, cx - sz / 2, base_y - sz, sz, ink);
        lay_add(dx + pad + slot * step, icon, -1, 0);
        slot++;
    }
    for (int i = 0; i < n && slot < items; i++) {
        if (!tb_window_is_app(&g_tb_wins[i])) continue;
        int cx = LUMINA_SLOT_CX(slot);
        int sz = icon, dist = g_mouse_x - cx; if (dist < 0) dist = -dist;
        if (mouse_in && dist < 70) sz = icon + (70 - dist) * 20 / 70;
        int focused = g_tb_wins[i].focused && !g_tb_wins[i].minimized;
        icon_draw_scaled(tb_icon_for_title(g_tb_wins[i].title),
                         cx - sz / 2, base_y - sz, sz,
                         g_tb_wins[i].minimized ? 0xFF8A8A90 : 0xFF303038);
        // Running indicator dot below the icon.
        draw_circle_filled(cx, dy + dh - 3, 2, focused ? 0xFF2A2A30 : 0xFF6A6A72);
        lay_add(dx + pad + slot * step, icon, g_tb_wins[i].id, focused);
        slot++;
    }
    #undef LUMINA_SLOT_CX
}

static bool taskbar_handle_lumina(int32_t x, int32_t y, bool clicked) {
    int mb = LUMINA_MENUBAR_H;
    // Menu bar band.
    if (y >= 0 && y < mb) {
        if (clicked) {
            if (x < g_lumina_menu_w) { g_start_menu_open = !g_start_menu_open; g_needs_redraw = true; return true; }
            if (tray_click(x, y)) return true;
        }
        return true;   // swallow all menu-bar clicks
    }
    // Dock band.
    if (clicked && lay_click(x, y)) return true;
    if (y >= g_lay_y && y < g_lay_y + g_lay_h) return true;  // swallow clicks on the pill
    return false;
}

// ---------------------------------------------------------------------------
// Classic UNIX: beveled bottom panel (CDE/Motif-style), launcher + apps, a workspace
// switcher in the middle, tray + clock at the right.
// ---------------------------------------------------------------------------
static int g_cu_ws = 0;   // highlighted workspace (cosmetic switcher)
static int32_t g_cu_ws_x[4], g_cu_ws_y, g_cu_ws_w, g_cu_ws_h;

static void taskbar_render_classic_unix(void) {
    g_gauge_x0 = 0;
    int W = g_fb_width, py = g_fb_height - CLASSIC_UNIX_PANEL_H;
    uint32_t face = 0xFFB6B6A8;   // Motif gray
    uint32_t lt = 0xFFE6E6DA, dk = 0xFF70706A;
    int n = lay_refresh_windows();

    // Panel background, raised bevel with a thin top rule.
    draw_fill_rect(0, py, W, CLASSIC_UNIX_PANEL_H, face);
    bevel(0, py, W, CLASSIC_UNIX_PANEL_H, 1, lt, dk);
    draw_hline(0, py, W, 0xFF3A3A34);

    uint32_t ink = readable_ink(face);
    int cy = py + 6, ch = CLASSIC_UNIX_PANEL_H - 12;   // tile row
    lay_reset(cy, ch);

    // Launcher tile (start).
    int lx = 6, lw = 46;
    bevel(lx, cy, lw, ch, g_start_menu_open ? 0 : 1, lt, dk);
    draw_fill_rect(lx + 1, cy + 1, lw - 2, ch - 2, face);
    icon_draw_scaled(ICON_CATEGORIES, lx + (lw - 28) / 2, cy + (ch - 28) / 2, 28, ink);
    lay_add(lx, lw, -1, 0);
    int bx = lx + lw + 6;

    // Right cluster width (tray + clock), reserved so app tiles stop short.
    char clk[6]; tb_clock_str(clk);
    int right_w = tray_total_w() + 12 + text_width(clk) + 12;
    // Workspace switcher block (centered-ish, fixed).
    int ws_w = 4 * 22 + 10, ws_h = ch;
    int ws_x = (W - ws_w) / 2;

    // App tiles fill from bx to just before the workspace block.
    int apps_end = ws_x - 8;
    int napps = 0; for (int i = 0; i < n; i++) if (tb_window_is_app(&g_tb_wins[i])) napps++;
    if (napps > 0 && apps_end - bx > 40) {
        int tw = 128; int need = napps * (tw + 4);
        if (need > apps_end - bx) tw = (apps_end - bx) / napps - 4;
        if (tw < 40) tw = 40;
        for (int i = 0; i < n; i++) {
            if (!tb_window_is_app(&g_tb_wins[i])) continue;
            if (bx + tw > apps_end) break;
            int focused = g_tb_wins[i].focused && !g_tb_wins[i].minimized;
            bevel(bx, cy, tw, ch, focused ? 0 : 1, lt, dk);
            draw_fill_rect(bx + 1, cy + 1, tw - 2, ch - 2, focused ? 0xFFC8C8BC : face);
            icon_draw_scaled(tb_icon_for_title(g_tb_wins[i].title),
                             bx + 4, cy + (ch - 20) / 2, 20,
                             g_tb_wins[i].minimized ? dk : ink);
            // Ellipsized label.
            char lbl[48]; int k = 0;
            while (g_tb_wins[i].title[k] && k < 40) { lbl[k] = g_tb_wins[i].title[k]; k++; }
            lbl[k] = '\0';
            int avail = tw - 30;
            while (k > 0 && text_width(lbl) > avail) lbl[--k] = '\0';
            draw_text(bx + 28, cy + (ch - 8) / 2, lbl, g_tb_wins[i].minimized ? dk : ink);
            lay_add(bx, tw, g_tb_wins[i].id, focused);
            bx += tw + 4;
        }
    }

    // Workspace switcher: recessed box with 4 buttons (One..Four).
    bevel(ws_x, cy, ws_w, ws_h, 0, lt, dk);
    draw_fill_rect(ws_x + 1, cy + 1, ws_w - 2, ws_h - 2, 0xFF9A9A8E);
    g_cu_ws_y = cy + 3; g_cu_ws_h = ws_h - 6; g_cu_ws_w = 20;
    static const char *wsl[4] = { "1", "2", "3", "4" };
    for (int i = 0; i < 4; i++) {
        int wx = ws_x + 5 + i * 22;
        g_cu_ws_x[i] = wx;
        int on = (i == g_cu_ws);
        bevel(wx, g_cu_ws_y, 20, g_cu_ws_h, on ? 0 : 1, lt, dk);
        draw_fill_rect(wx + 1, g_cu_ws_y + 1, 18, g_cu_ws_h - 2, on ? 0xFF3C6EB4 : face);
        draw_text(wx + 6, g_cu_ws_y + (g_cu_ws_h - 8) / 2, wsl[i], on ? 0xFFFFFFFF : ink);
    }

    // Right: tray + clock in a recessed strip.
    int rx = W - right_w;
    tray_render_core(rx + 4, cy + (ch - 26) / 2, face, 0);
    draw_text(W - text_width(clk) - 10, cy + (ch - 8) / 2, clk, ink);
    (void)ws_h;
}

static bool taskbar_handle_classic_unix(int32_t x, int32_t y, bool clicked) {
    int py = g_fb_height - CLASSIC_UNIX_PANEL_H;
    if (y < py) return false;
    if (clicked) {
        // Workspace switcher.
        if (y >= g_cu_ws_y && y < g_cu_ws_y + g_cu_ws_h) {
            for (int i = 0; i < 4; i++)
                if (x >= g_cu_ws_x[i] && x < g_cu_ws_x[i] + g_cu_ws_w) {
                    g_cu_ws = i; g_needs_redraw = true; return true;
                }
        }
        if (tray_click(x, y)) return true;
        if (lay_click(x, y)) return true;
    }
    return true;   // swallow all clicks on the panel
}

// ---------------------------------------------------------------------------
// Retro Bench: top screen title bar with depth/zoom gadgets at the right.
// Blue/orange/white/black palette, blocky retro bitmap font.
// ---------------------------------------------------------------------------
#define RB_BLUE   0xFF0055AA
#define RB_WHITE  0xFFFFFFFF
#define RB_BLACK  0xFF000000
#define RB_ORANGE 0xFFFF8800

static void rb_gadget_depth(int x, int y) {
    // Two overlapping windows (the classic depth-arrangement gadget).
    draw_fill_rect(x, y + 2, 12, 10, RB_WHITE); draw_rect_outline(x, y + 2, 12, 10, RB_BLACK);
    draw_fill_rect(x + 6, y - 2, 12, 10, RB_ORANGE); draw_rect_outline(x + 6, y - 2, 12, 10, RB_BLACK);
}
static void rb_gadget_zoom(int x, int y) {
    // Box with a smaller box in the top-left corner (RB zoom gadget).
    draw_fill_rect(x, y, 16, 14, RB_WHITE); draw_rect_outline(x, y, 16, 14, RB_BLACK);
    draw_fill_rect(x + 1, y + 1, 8, 6, RB_ORANGE); draw_rect_outline(x + 1, y + 1, 8, 6, RB_BLACK);
}

static int32_t g_rb_gadg_x = 0;   // left edge of the gadget cluster (non-clickable chrome)

static void taskbar_render_retro_bench(void) {
    g_gauge_x0 = 0;
    int W = g_fb_width, bh = RETRO_BENCH_BAR_H;
    int n = lay_refresh_windows();

    // Bar background + black baseline (Retro Bench screen title bar).
    draw_fill_rect(0, 0, W, bh, RB_BLUE);
    draw_hline(0, bh - 1, W, RB_BLACK);
    draw_hline(0, bh, W, RB_BLACK);

    // Left: screen title (retro look via the 8px bitmap font).
    draw_text(6, (bh - 8) / 2, "Retro Bench Screen", RB_WHITE);

    // Right: gadget cluster (zoom then depth), reserved.
    int gadg_w = 44;
    g_rb_gadg_x = W - gadg_w;
    rb_gadget_zoom(W - 42, (bh - 14) / 2);
    rb_gadget_depth(W - 22, (bh - 14) / 2 + 2);

    // Clock + tray to the left of the gadgets.
    char clk[6]; tb_clock_str(clk);
    int clk_x = g_rb_gadg_x - 8 - text_width(clk);
    draw_text(clk_x, (bh - 8) / 2, clk, RB_WHITE);
    int tray_right = clk_x - 8;
    tray_render_core(tray_right - tray_total_w(), (bh - 26) / 2, RB_BLUE, 1);

    // Running apps: text buttons after the title, focused = orange field.
    lay_reset(0, bh);
    int bx = 6 + text_width("Retro Bench Screen") + 16;
    int apps_end = g_tray_x[0] - 10;   // stop before the tray
    for (int i = 0; i < n; i++) {
        if (!tb_window_is_app(&g_tb_wins[i])) continue;
        char lbl[40]; int k = 0;
        while (g_tb_wins[i].title[k] && k < 32) { lbl[k] = g_tb_wins[i].title[k]; k++; }
        lbl[k] = '\0';
        int tw = text_width(lbl) + 12;
        if (bx + tw > apps_end) break;
        int focused = g_tb_wins[i].focused && !g_tb_wins[i].minimized;
        if (focused) {
            draw_fill_rect(bx, 2, tw, bh - 4, RB_ORANGE);
            draw_text(bx + 6, (bh - 8) / 2, lbl, RB_BLACK);
        } else {
            draw_text(bx + 6, (bh - 8) / 2, lbl, g_tb_wins[i].minimized ? 0xFFAAC4E4 : RB_WHITE);
        }
        lay_add(bx, tw, g_tb_wins[i].id, focused);
        bx += tw + 4;
    }
}

static bool taskbar_handle_retro_bench(int32_t x, int32_t y, bool clicked) {
    if (y >= RETRO_BENCH_BAR_H) return false;
    if (clicked) {
        // Title label acts as the launcher (Retro Bench menu).
        if (x < 6 + text_width("Retro Bench Screen")) {
            g_start_menu_open = !g_start_menu_open; g_needs_redraw = true; return true;
        }
        if (tray_click(x, y)) return true;
        if (lay_click(x, y)) return true;
    }
    return true;   // swallow bar clicks
}

// ---------------------------------------------------------------------------
// #26 XFCE ("Marble" in Settings, see DOCK_STYLE_NAMES): a flush top panel
// (Start + AI launcher + gauges + tray + clock) and a flush-to-bottom-edge
// fit-content dock (pinned favorites, a separator, then running-but-not-
// pinned windows) - ported from docs/DOCK_XFCE_MOCKUP.html. STALE COMMENT
// FIXED (#745): both surfaces are glass (docs/DOCK_XFCE_GLASS.html, the
// same GLASS_SURF_PANEL/GLASS_SURF_DOCK pipeline as every other chrome
// surface) as of the #745 glass chrome pass, and the dock icons now ease a
// hover lift/grow (see xfce_draw_slot(), taskbar_dock_animating()).
// ---------------------------------------------------------------------------
static void taskbar_render_xfce_panel(void) {
    int W = g_fb_width;
    // (#745) Glass. Bleed on the bottom side only (top/left/right are screen
    // edges). No inner highlight: the panel's top edge IS the screen edge, so
    // there is no surface for a highlight to imply.
    glass_or_flat(0, 0, W, XFCE_PANEL_H, GLASS_SURF_PANEL);
    glass_edge_h(0, XFCE_PANEL_H - 1, W, CLR_TASKBAR_BORDER);

    int32_t bs = XFCE_BTN_SIZE;
    int32_t by = (XFCE_PANEL_H - bs) / 2;
    int32_t start_x = 4;
    uint32_t start_ink, start_bg;
    chrome_chip(start_x, by, bs, g_start_menu_open, &start_ink, &start_bg);
    (void)start_bg;
    icon_draw_scaled(ICON_CATEGORIES, start_x + 2, by + 2, bs - 4, start_ink);

    int32_t logo_x = start_x + bs + 4;
    uint32_t logo_ink, logo_bg;
    chrome_chip(logo_x, by, bs, g_launcher_open, &logo_ink, &logo_bg);
    (void)logo_bg;
    if (!ai_launcher_icon_draw(logo_x + 2, by + 2, bs - 4, logo_ink))
        draw_text_large(logo_x + 8, by + 4, "M", logo_ink, 1);

    // Right cluster, computed right-to-left: clock, then tray, then gauges
    // (mirrors the Retro Bench/Lumina clock-rightmost convention).
    char clk[6]; tb_clock_str(clk);
    int32_t clock_w = text_width(clk);
    int32_t clock_x = W - TASKBAR_PADDING - clock_w;
    int32_t tray_right = clock_x - 8;
    int32_t tray_x0 = tray_right - tray_total_w();
    draw_vline(tray_x0 - 6, 6, XFCE_PANEL_H - 12, CLR_GAUGE_BORDER);
    // is_top=1 (not the tray_render() wrapper, which hardcodes 0) so tray
    // popups AND the perf popup (see draw_perf_popup) open downward.
    tray_render_core(tray_x0, (XFCE_PANEL_H - 26) / 2, chrome_surface_bg(), 1);  // #745: glass-aware ink contrast, see tray_render()
    draw_text(clock_x, 7, clk, CLR_CHROME_TEXT);

    int32_t gauge_block_w = 4 * GAUGE_WIDTH + 3 * GAUGE_SPACING;
    int32_t gauge_start_x = tray_x0 - 8 - gauge_block_w;
    int32_t gauge_y = (XFCE_PANEL_H - GAUGE_HEIGHT) / 2;
    g_gauge_x0 = gauge_start_x;
    g_gauge_y  = gauge_y;
    draw_cpu_gauge(gauge_start_x, gauge_y, GAUGE_WIDTH, GAUGE_HEIGHT, g_cpu_ncores, g_cpu_cores);
    draw_gauge(gauge_start_x + (GAUGE_WIDTH + GAUGE_SPACING), gauge_y, GAUGE_WIDTH, GAUGE_HEIGHT,
               g_ram_percent, CLR_GAUGE_RAM, "RAM", g_ram_str);
    draw_gauge(gauge_start_x + 2 * (GAUGE_WIDTH + GAUGE_SPACING), gauge_y, GAUGE_WIDTH, GAUGE_HEIGHT,
               g_disk_percent, CLR_GAUGE_DSK, "DSK", g_disk_str);
    // #76: throughput value, log-scale fill - see draw_net_gauge() above.
    draw_net_gauge(gauge_start_x + 3 * (GAUGE_WIDTH + GAUGE_SPACING), gauge_y, GAUGE_WIDTH, GAUGE_HEIGHT,
                   g_net_percent, CLR_GAUGE_NET, g_net_str);

    // #241: detailed performance popup, same click target as DOCK_DEFAULT.
    if (g_perf_open) draw_perf_popup();
}

// (#745) Hover animation state for the marble (XFCE) dock. Slots are
// addressed by draw order within a frame (favorites first, then
// running-only), stable within a single hover gesture; a mid-hover reorder
// (a window closing) just restarts that slot's ease from wherever it was,
// which is visually harmless. `g_xfce_anim_active` is recomputed from
// scratch every taskbar_render_xfce_dock() call (see there) and read by
// main.c's taskbar_dock_animating() to decide whether the frame loop must
// take the full-composite path this tick instead of the cheap cursor-only
// partial path, whose tiny clip rect does not cover the dock strip - see
// the comment at that call site for why a naive port of DOCK_LUMINA's own
// proximity magnify has the identical latent bug.
#define XFCE_HOVER_SLOTS  (XFCE_MAX_FAVS + TB_MAX_WINS)
#define XFCE_HOVER_MS     120   // ease duration in ms: 0 (rest) to full hover grow/lift
                                 // takes this long, and the same ramp back down on
                                 // un-hover. Chosen to read as a snappy lift, not a swim.
                                 // Deliberately shorter than the 500ms recent_input
                                 // full-composite window in main.c: verification
                                 // temporarily widened this constant past 500ms so an
                                 // ease could still be caught mid-flight AFTER
                                 // recent_input had already expired, proving
                                 // taskbar_dock_animating() (not recent_input) was what
                                 // kept forcing the full-composite path - see blame.md.
                                 // Restored to this real value for the shipped build.
#define XFCE_HOVER_GROW   10    // max extra icon px at full hover
#define XFCE_HOVER_LIFT   6     // max extra upward shift px at full hover
static int32_t  g_xfce_hover_amt[XFCE_HOVER_SLOTS];   // 0..XFCE_HOVER_MS per slot
static uint64_t g_xfce_hover_last_ms = 0;
static int      g_xfce_anim_active = 0;

// (#745) DOCK_XFCE only: is any dock slot still easing toward or away from
// its hover target this frame? Read by main.c to keep forcing the full
// render path for exactly as long as the animation needs and no longer.
int taskbar_dock_animating(void) {
    return (g_dock_style == DOCK_XFCE) ? g_xfce_anim_active : 0;
}

// Draw one dock icon slot + its running-indicator bar, and record its hit
// box. Shared by the pinned and running-only loops in
// taskbar_render_xfce_dock() so the two can never draw differently.
// `slot` indexes the per-slot hover-ease state; `dt_ms` is the real elapsed
// milliseconds since the previous call this frame pass (uptime_ms()-based,
// computed once by the caller - see taskbar_render_xfce_dock()).
// (local 63/#745) The ink an unminimized dock icon is recoloured to. The user
// asked for a slightly off-white INSTEAD OF the bright white that was jarring
// them on the DARK dock; on a light or flat-retro dock surface that same ink
// is illegible (measured 1.04:1 on maytera_light, 1.85:1 on retro_unix, both
// far under the WCAG 3:1 non-text floor). So the light-versus-dark decision is
// delegated to readable_ink, the shared primitive that already owns exactly
// that judgement for every other piece of chrome ink in the compositor, and
// the user's off-white is substituted only for readable_ink's own near-white
// answer. No second luminance threshold is introduced here: if readable_ink
// picked a light ink, this is a refinement of it; if it picked a dark ink, it
// wins outright. The surface is chrome_surface_bg (the tint on a glass theme,
// the opaque bar token on a flat one), the same surface chrome_ink_dim next
// door measures against, so the two dock inks can never disagree about which
// side of the light/dark line the dock is on. Verified stable across the whole
// composited range: for all 14 shipped themes the tint and the op=70 white
// backdrop worst case land on the same side of readable_ink's threshold, so
// the ink cannot flip as a wallpaper moves underneath the glass.
static uint32_t dock_icon_ink(void)
{
    uint32_t ink = readable_ink(chrome_surface_bg());
    return draw_luminance(ink) > 128 ? CLR_DOCK_ICON_INK : ink;
}

static void xfce_draw_slot(int slot, int32_t cx, int32_t dock_y, icon_id_t icon,
                           int minimized, int running, int focused, int32_t dt_ms) {
    int mx = g_mouse_x, my = g_mouse_y;
    int hover = (mx >= cx - 4 && mx < cx + XFCE_DOCK_ICON + 4 &&
                 my >= dock_y + 2 && my < dock_y + XFCE_DOCK_H - 2);
    if (hover)
        draw_fill_rect(cx - 4, dock_y + 2, XFCE_DOCK_ICON + XFCE_DOCK_PAD,
                       XFCE_DOCK_H - 4, CLR_TASKBAR_HOVER);

    // Ease this slot's hover amount toward its target by the real elapsed
    // time (uptime_ms(), NOT timer_ticks - KVM can replay lost ticks in
    // bursts, see blame.md). A linear ramp, not a sleep or a poll: the frame
    // loop already calls this every tick while anything is interactive, so
    // "animate" is just "advance a little each call".
    int32_t amt = 0;
    if (slot >= 0 && slot < XFCE_HOVER_SLOTS) {
        amt = g_xfce_hover_amt[slot];
        int32_t target = hover ? XFCE_HOVER_MS : 0;
        if (amt != target) {
            int32_t step = dt_ms > 0 ? dt_ms : 1;
            if (target > amt) { amt += step; if (amt > target) amt = target; }
            else              { amt -= step; if (amt < target) amt = target; }
            g_xfce_hover_amt[slot] = amt;
        }
        if (amt != target) g_xfce_anim_active = 1;
    }

    // (#745) On glass the effective background is the tint, not CLR_TASKBAR_BG,
    // and the dim mix is 22% rather than 35% (3.18:1 -> 3.83:1 at the floor).
    // (#63/#745) Non-minimized ink is the dock's own slightly-off-white
    // (CLR_DOCK_ICON_INK, defined above), not CLR_CHROME_TEXT - see that
    // constant's comment for the measured contrast. Minimized keeps the
    // existing themed dim ink unchanged (it is already dimmed, not the
    // "bright white" the user reported).
    uint32_t ink = minimized ? chrome_ink_dim() : dock_icon_ink();
    int32_t grow = (int32_t)((int64_t)XFCE_HOVER_GROW * amt / XFCE_HOVER_MS);
    int32_t lift = (int32_t)((int64_t)XFCE_HOVER_LIFT * amt / XFCE_HOVER_MS);
    // (#63/#745) Base size is XFCE_DOCK_ICON_DRAW (36), NOT the tile size
    // XFCE_DOCK_ICON (40) - see XFCE_DOCK_ICON_DRAW's own comment. `bottom`
    // and the centering formula below both still reference the TILE size on
    // purpose, so the icon graphic stays centred and bottom-anchored within
    // an unchanged dock tile.
    int32_t isize  = XFCE_DOCK_ICON_DRAW + grow;
    int32_t bottom = dock_y + XFCE_DOCK_PAD + XFCE_DOCK_ICON;   // fixed baseline
    int32_t iy = bottom - isize - lift;
    int32_t ix = cx + (XFCE_DOCK_ICON - isize) / 2;             // centred in the tile
    // (#63/#745) Try the recoloured+shadowed color-icon path first (every
    // shipped dock favourite has one - see main.c's icon_load_color() calls);
    // fall back to the mono bitmap path (icon_draw_scaled, which already
    // draws in `ink`) for the rare id that has none, same
    // "if (!icon_draw_..._tinted(...)) icon_draw_scaled(...)" idiom
    // startmenu.c already established for icon_draw_color_tinted().
    if (!icon_draw_dock_icon(icon, ix, iy, isize, ink))
        icon_draw_scaled(icon, ix, iy, isize, ink);
    if (running) {
        // Docklike-plugin "Bars" indicator style (research 2.4) - a bar, not
        // a dot, so this style reads differently from DOCK_LUMINA at a glance.
        if (focused)
            draw_fill_rect(cx + 8, dock_y + XFCE_DOCK_PAD + XFCE_DOCK_ICON + 2, 24, 4,
                           readable_ink(CLR_TASKBAR_BG));
        else
            draw_fill_rect(cx + 12, dock_y + XFCE_DOCK_PAD + XFCE_DOCK_ICON + 3, 16, 3,
                           chrome_ink_dim());
    }
}

// #63/#745 USER-REPORTED: "slightly rounded corners of the dock again, VERY
// VERY slight". Deliberately tiny radius, top corners only - the dock's
// bottom edge IS the screen's bottom edge (dock_y + XFCE_DOCK_H ==
// g_fb_height), so there is nothing behind a rounded bottom corner to
// reveal, and rounding it would just cut a hole showing whatever happens to
// be in framebuffer memory past the visible screen.
//
// This does NOT reuse draw_rounded_rect_aa() (draw.c) directly, because a
// glass surface is not a flat fill: glass_or_flat() blits an already-
// composited (blurred+tinted) rect, and the pixels a rounded corner needs to
// reveal instead are whatever was drawn earlier THIS FRAME under that corner
// (wallpaper, a desktop icon, a window edge) - a per-pixel CAPTURED colour,
// not one flat fill colour draw_rounded_rect_aa's single-`color` signature
// can express. This wrapper captures that corner content immediately before
// the dock paints over it, then blends it back over the excluded corner
// pixels afterward, using the EXACT SAME per-pixel coverage algorithm
// kernel/gui/window_decor.c's decor_fill_rounded_rect_aa() already proved on
// the kernel login gate (`cov = clamp(0.5 - (dist - r), 0, 1)`) - ported
// here since the kernel and this compositor are separate binaries and
// cannot share a compiled function; draw_rounded_rect_aa() (draw.c) is the
// SAME algorithm again, for callers that only need a flat-colour fill. Three
// designs did not happen here: this project already has a non-AA
// draw_rounded_rect() and the kernel's AA pair before this task; this is the
// SECOND implementation of the SAME (AA) design, in the other binary that
// needed a per-pixel-captured background version of it.
#define XFCE_DOCK_CORNER_R 6   // "VERY VERY slight" - err small, per the brief

static void xfce_dock_paint_rounded(int32_t dock_x, int32_t dock_y, int32_t dock_w,
                                    int32_t dock_h, int surf)
{
    int32_t r = XFCE_DOCK_CORNER_R;
    if (r > dock_w / 2) r = dock_w / 2;
    if (r > dock_h / 2) r = dock_h / 2;

    uint32_t capL[XFCE_DOCK_CORNER_R][XFCE_DOCK_CORNER_R];
    uint32_t capR[XFCE_DOCK_CORNER_R][XFCE_DOCK_CORNER_R];
    if (r > 0) {
        for (int32_t yy = 0; yy < r; yy++) {
            int32_t py = dock_y + yy;
            for (int32_t xx = 0; xx < r; xx++) {
                int32_t pxl = dock_x + xx, pxr = dock_x + dock_w - r + xx;
                capL[yy][xx] = (py >= 0 && py < g_fb_height && pxl >= 0 && pxl < g_fb_width)
                             ? g_fb[py * g_fb_pitch + pxl] : 0;
                capR[yy][xx] = (py >= 0 && py < g_fb_height && pxr >= 0 && pxr < g_fb_width)
                             ? g_fb[py * g_fb_pitch + pxr] : 0;
            }
        }
    }

    // The dock's own opaque/glass rect, unchanged - the #745 glass cache and
    // its perf machinery (see draw.c) never learn a corner was rounded.
    glass_or_flat(dock_x, dock_y, dock_w, dock_h, surf);

    if (r <= 0) return;
    // Restore each excluded pixel through draw_putpixel()'s OWN g_draw_blend
    // path (draw.c) rather than hand-rolling a second blend formula here:
    // draw_blend() itself is `static` to draw.c (no cross-file linkage), so
    // reusing the primitive means going through the public entry point that
    // already wraps it, exactly like glass_edge_h()/glass_edge_v() above do
    // for their own alpha override.
    int ob = g_draw_blend;
    float rf = (float)r;
    for (int32_t yy = 0; yy < r; yy++) {
        int32_t py = dock_y + yy;
        if (py < 0 || py >= g_fb_height) continue;
        float fdy = rf - (float)yy;
        for (int32_t xx = 0; xx < r; xx++) {
            float fdx = rf - (float)xx;
            float dist = sqrtf(fdx * fdx + fdy * fdy);
            float cov = 0.5f - (dist - rf);   // 1.0 = fully inside the rounded silhouette
            if (cov < 0.0f) cov = 0.0f;
            if (cov > 1.0f) cov = 1.0f;
            int restore_a = (int)((1.0f - cov) * 255.0f + 0.5f);  // how much background to bring back
            if (restore_a == 0) continue;
            g_draw_blend = restore_a;
            int32_t pxl = dock_x + xx, pxr = dock_x + dock_w - r + xx;
            draw_putpixel(pxl, py, capL[yy][xx]);
            draw_putpixel(pxr, py, capR[yy][xx]);
        }
    }
    g_draw_blend = ob;
}

static void taskbar_render_xfce_dock(void) {
    int W = g_fb_width, H = g_fb_height;
    int n = lay_refresh_windows();

    // (#745) Hover-ease timestep: a real monotonic clock (uptime_ms(),
    // SYS_UPTIME_MS/252), computed ONCE for every slot this render pass, not
    // timer_ticks (KVM can replay lost ticks in bursts under vCPU
    // starvation - see blame.md). g_xfce_anim_active is recomputed from
    // scratch each pass: each xfce_draw_slot() call below can only set it,
    // never clear it, so it correctly reads "any slot mid-transition" for
    // this frame once every slot has been visited.
    uint64_t xfce_now = uptime_ms();
    uint64_t xfce_dt64 = (g_xfce_hover_last_ms == 0) ? 16 : xfce_now - g_xfce_hover_last_ms;
    if (xfce_dt64 > 100) xfce_dt64 = 100;   // clamp a resume-from-idle jump
    g_xfce_hover_last_ms = xfce_now;
    int32_t xfce_dt = (int32_t)xfce_dt64;
    g_xfce_anim_active = 0;
    int xfce_slot = 0;

    sm_fav_info_t favs[XFCE_MAX_FAVS];
    int nfav = startmenu_get_favorites(favs, XFCE_MAX_FAVS);

    // #41 (task #41, 2026-08-12): match each favourite to a running window on
    // a STABLE IDENTITY first (kernel-resolved binary basename vs the
    // favourite's own exec_path), not the window's title. The old
    // title-substring-only match is why Paint ("Maytera Studio", no
    // substring "Paint" anywhere in it - MEASURED, see CHANGELOG) always
    // missed: the favourite stayed pinned as not-running AND the same live
    // window drew again in the running-only loop below, i.e. the exact
    // doubled-icon report. wm_window_info_t now carries app_id
    // (kernel/gui/window.h, kernel/gui/window.c:sys_wm_get_windows()).
    int fav_win[XFCE_MAX_FAVS];
    int win_consumed[TB_MAX_WINS];
    for (int i = 0; i < TB_MAX_WINS; i++) win_consumed[i] = 0;
    for (int i = 0; i < nfav; i++) fav_win[i] = -1;
    // PASS 1: exact identity match. This is now the primary path for every
    // app the kernel could resolve an owner for.
    for (int i = 0; i < nfav; i++) {
        for (int j = 0; j < n; j++) {
            if (!tb_window_is_app(&g_tb_wins[j]) || win_consumed[j]) continue;
            if (tb_app_id_matches(g_tb_wins[j].app_id, favs[i].exec_path)) {
                fav_win[i] = j; win_consumed[j] = 1;
                break;
            }
        }
    }
    // PASS 2: title-substring FALLBACK, for any favourite PASS 1 could not
    // resolve. Deliberately NOT gated on app_id being empty: a Win16 or DOS
    // guest's window is owned by a dedicated kernel host process whose name
    // is the fixed literal "win16"/"dos"/"dosrun" (proc_create("win16", ...)
    // in win16_launch()/dos_launch(), kernel/proc/syscall.c and
    // kernel/dos/dosexec.c - one shared host process name for every guest
    // .EXE it ever runs), NOT the guest program's own name. That app_id is
    // real and non-empty, but it can never match a Win16/DOS favourite's
    // exec_path either, so treating "has SOME app_id" as "PASS 1 already had
    // a fair shot" would silently disable the fallback for exactly the
    // windows that most need it. Any window PASS 1 left unconsumed is fair
    // game here, whatever its app_id says. Emits a visible, throttled debug
    // line every time it actually fires, so the next app whose title does
    // not carry a usable identity is diagnosable instead of silently
    // duplicating again - this was previously a silent-failure heuristic
    // with no signal at all when it missed.
    static uint64_t s_fav_fallback_logged_ms[XFCE_MAX_FAVS];
    for (int i = 0; i < nfav; i++) {
        if (fav_win[i] >= 0) continue;
        for (int j = 0; j < n; j++) {
            if (!tb_window_is_app(&g_tb_wins[j]) || win_consumed[j]) continue;
            if (tb_contains(g_tb_wins[j].title, favs[i].name)) {
                fav_win[i] = j; win_consumed[j] = 1;
                uint64_t now = uptime_ms();
                if (now - s_fav_fallback_logged_ms[i] > 5000) {
                    s_fav_fallback_logged_ms[i] = now;
                    // #41: ONE write(1,...) syscall, not printf()/putchar().
                    // MEASURED live (VM 2942, FreeCell): printf() calls
                    // putchar() per character, each of which is its own
                    // SYS_PUTCHAR -> console_write() -> kputc() PLUS a
                    // syslog_log() call for that single byte, so this line
                    // came out as one syslog/serial entry PER CHARACTER
                    // ("[29:55 [INF] [", "t29:55 [INF] t", ...), unreadable
                    // in both the serial capture and the SysLog app. A
                    // single buffered write() sends the whole line through
                    // console_write() ONCE, so kputc() emits it as one
                    // contiguous run and syslog_log() records it as one
                    // entry - "visible" per the brief, not merely emitted.
                    char dbg[256];
                    int dl = snprintf(dbg, sizeof(dbg),
                           "[taskbar] dock: '%s' (%s) not matched by kernel "
                           "app_id ('%s'), fell back to title-match against "
                           "window %d ('%s')\n",
                           favs[i].name, favs[i].exec_path,
                           g_tb_wins[j].app_id[0] ? g_tb_wins[j].app_id : "<empty>",
                           g_tb_wins[j].id, g_tb_wins[j].title);
                    if (dl > 0) {
                        if (dl >= (int)sizeof(dbg)) dl = (int)sizeof(dbg) - 1;
                        write(1, dbg, (size_t)dl);
                    }
                }
                break;
            }
        }
    }
    int n_running_only = 0;
    for (int j = 0; j < n; j++)
        if (tb_window_is_app(&g_tb_wins[j]) && !win_consumed[j]) n_running_only++;

    int items = nfav + n_running_only;
    int has_sep = (nfav > 0 && n_running_only > 0);
    if (items < 1) items = 1;   // never fully vanish - fixed reservation, no per-frame jitter
    int dock_w = items * XFCE_DOCK_ICON + (items + 1) * XFCE_DOCK_PAD
               + (has_sep ? XFCE_DOCK_SEP_EXTRA : 0);
    if (dock_w > W - 24) dock_w = W - 24;
    int dock_x = (W - dock_w) / 2;
    int dock_y = H - XFCE_DOCK_H;
    // (#40) Publish the dock's LIVE rect. Its width is recomputed here every
    // frame from the pinned + running item count, so it cannot be derived
    // from a constant anywhere else; taskbar_panel_rects() below hands this
    // one cached rect to anything that has to treat the dock as a solid
    // surface (today the sheep pet's floor/wall scan).
    g_xfce_dock_x = dock_x;
    g_xfce_dock_w = dock_w;

    // (#745) Glass. Bleed on top, left and right; the bottom is the screen
    // edge. Outer hairline on those same three sides (no bottom line), then a
    // 1px white inner highlight inset from it, which is what makes the dock
    // read as a raised pane rather than a hole in the wallpaper.
    // (#63/#745) xfce_dock_paint_rounded() paints the SAME rect
    // glass_or_flat() would have (unchanged geometry/cache behaviour), then
    // very slightly rounds its TOP two corners only - see its own comment.
    // The hairlines/highlight below are inset by the same radius on their
    // top-corner ends so a straight 1px line does not cut back across the
    // now-rounded corner; the curve itself reads as its own soft edge at
    // this radius (6px) with no separate stroke needed.
    xfce_dock_paint_rounded(dock_x, dock_y, dock_w, XFCE_DOCK_H, GLASS_SURF_DOCK);
    glass_edge_h(dock_x + XFCE_DOCK_CORNER_R, dock_y, dock_w - 2 * XFCE_DOCK_CORNER_R,
                CLR_TASKBAR_BORDER);
    glass_edge_v(dock_x, dock_y + XFCE_DOCK_CORNER_R, XFCE_DOCK_H - XFCE_DOCK_CORNER_R,
                CLR_TASKBAR_BORDER);
    glass_edge_v(dock_x + dock_w - 1, dock_y + XFCE_DOCK_CORNER_R,
                XFCE_DOCK_H - XFCE_DOCK_CORNER_R, CLR_TASKBAR_BORDER);
    glass_highlight_h(dock_x + 1 + XFCE_DOCK_CORNER_R, dock_y + 1,
                      dock_w - 2 - 2 * XFCE_DOCK_CORNER_R);

    xdock_reset(dock_y + 2, XFCE_DOCK_H - 4);

    int32_t cx = dock_x + XFCE_DOCK_PAD;
    for (int i = 0; i < nfav; i++) {
        int running = (fav_win[i] >= 0);
        int minimized = running && g_tb_wins[fav_win[i]].minimized;
        int focused   = running && g_tb_wins[fav_win[i]].focused && !minimized;
        xfce_draw_slot(xfce_slot++, cx, dock_y, favs[i].icon_id, minimized, running, focused, xfce_dt);
        // #44: app_id comes straight from the live window when this favorite
        // is running (MERGED) - feeds Force Quit. A PINNED (not-running)
        // slot has no window and so no app_id, which is correct: there is
        // nothing to force-quit.
        xdock_add(cx - 4, running ? XDOCK_KIND_MERGED : XDOCK_KIND_PINNED,
                  running ? g_tb_wins[fav_win[i]].id : -1,
                  favs[i].exec_path, favs[i].launch_type, focused,
                  (int)favs[i].icon_id, running ? g_tb_wins[fav_win[i]].app_id : "");
        cx += XFCE_DOCK_ICON + XFCE_DOCK_PAD;
    }

    if (has_sep) {
        // #dockpad: the separator's slot is the normal trailing PAD gap after
        // the last pinned icon (already reserved by the loop's cx += ICON+PAD)
        // PLUS XFCE_DOCK_SEP_EXTRA (PAD+1) of additional space. Centering a
        // 1px line in that combined (PAD + SEP_EXTRA) = (2*PAD+1) span means
        // PAD px on each side of the line, which falls exactly at cx (the
        // start of the extra span) - NOT at cx + PAD/2 - 1, which pushed the
        // line 3px right of center (measured: 11px left gap vs 5px right gap
        // instead of 8px/8px). Drawing at cx alone makes both gaps XFCE_DOCK_PAD,
        // matching docs/DOCK_XFCE_MOCKUP.html's spec position
        // (dock_x + 8 + pinned_count*48 == cx here).
        draw_vline(cx, dock_y + XFCE_DOCK_PAD, XFCE_DOCK_ICON, CLR_TASKBAR_BORDER);
        cx += XFCE_DOCK_SEP_EXTRA;
    }

    for (int j = 0; j < n; j++) {
        if (!tb_window_is_app(&g_tb_wins[j]) || win_consumed[j]) continue;
        int minimized = g_tb_wins[j].minimized;
        int focused   = g_tb_wins[j].focused && !minimized;
        xfce_draw_slot(xfce_slot++, cx, dock_y, tb_icon_for_title(g_tb_wins[j].title), minimized, 1, focused, xfce_dt);
        // #44: a RUNNING (not pinned) slot has no favorites-list exec_path,
        // but #41's app_id now makes a reverse lookup against g_menu_items[]
        // possible (startmenu_find_by_app_id()), which is what unblocks
        // "Pin to Dock"/"Change Icon" on a running, unpinned tile - the
        // comment this replaced (taskbar_handle_right_click(), below)
        // explained exactly why this was NOT buildable before #41 landed.
        // Falls through to an empty path (both actions omitted) for a
        // window whose identity does not resolve to any Start-menu item at
        // all, same as any other unresolved-identity case in this file.
        {
            sm_fav_info_t rinfo;
            int has_id = g_tb_wins[j].app_id[0] != '\0';
            const char *rpath = "";
            int ricon = 0;
            if (has_id && startmenu_find_by_app_id(g_tb_wins[j].app_id, &rinfo)) {
                rpath = rinfo.exec_path;
                ricon = (int)rinfo.icon_id;
            }
            xdock_add(cx - 4, XDOCK_KIND_RUNNING, g_tb_wins[j].id, rpath, 0, focused,
                      ricon, g_tb_wins[j].app_id);
        }
        cx += XFCE_DOCK_ICON + XFCE_DOCK_PAD;
    }
}

static void taskbar_render_xfce(void) {
    taskbar_render_xfce_panel();
    taskbar_render_xfce_dock();
}

static bool taskbar_handle_xfce_dock_click(int32_t x, int32_t y) {
    if (y < g_xdock_y || y >= g_xdock_y + g_xdock_h) return false;
    int32_t w = XFCE_DOCK_ICON + XFCE_DOCK_PAD;
    for (int i = 0; i < g_xdock_n; i++) {
        if (x < g_xdock_x[i] || x >= g_xdock_x[i] + w) continue;
        if (g_xdock_kind[i] == XDOCK_KIND_PINNED) {
            startmenu_launch_path(g_xdock_path[i], g_xdock_launch_type[i]);
        } else {
            if (g_xdock_focused[i]) wm_minimize(g_xdock_win_id[i]);
            else                    wm_focus(g_xdock_win_id[i]);
        }
        g_needs_redraw = true;
        return true;
    }
    return false;
}

static bool taskbar_handle_xfce(int32_t x, int32_t y, bool clicked) {
    // Top panel band.
    if (y < XFCE_PANEL_H) {
        if (clicked) {
            int32_t bs = XFCE_BTN_SIZE;
            int32_t start_x = 4, logo_x = 4 + bs + 4;
            if (x >= start_x && x < start_x + bs) {
                g_start_menu_open = !g_start_menu_open; g_needs_redraw = true; return true;
            }
            if (x >= logo_x && x < logo_x + bs) {
                launcher_toggle(); g_start_menu_open = false; g_needs_redraw = true; return true;
            }
            if (tray_click(x, y)) return true;
            int gh = gauge_hit(x, y);
            if (gh >= 0) {
                g_perf_open = 1; g_perf_sel = gh; g_needs_redraw = true; return true;
            }
        }
        return true;   // swallow all top-panel clicks/hover
    }
    // Bottom dock band.
    if (clicked && taskbar_handle_xfce_dock_click(x, y)) return true;
    if (y >= g_fb_height - XFCE_DOCK_H) return true;   // swallow clicks on the dock chrome
    return false;
}

// ---------------------------------------------------------------------------
// Per-app taskbar-tile right-click menu (#: taskbar Close).
//
// Root cause of "no context menu on a taskbar item": taskbar_handle_mouse()
// (all four dock styles) unconditionally returns true for any click inside
// the taskbar strip once it has finished its own left-click handling, so a
// right-press there was already being swallowed with zero visible effect -
// not passed through to the desktop, but not offering anything either. This
// adds the missing menu rather than special-casing one dock style: the hit
// test below reads whichever hit-list the ACTIVE style already populated
// this frame while drawing (g_tb_btn_* for the default bottom bar,
// g_lay_* - shared by Lumina/Classic UNIX/Retro Bench - for the rest), the
// same records lay_click()/the default click handler already use.
//
// Only "Close" is offered. Two other candidates were considered and
// rejected rather than added for parity with other systems:
//   - "Pause": there is no WM or process primitive to suspend a running app
//     (no SIGSTOP-equivalent anywhere in this tree) - it would be a UI
//     control wired to nothing.
//   - "Maximize/Restore": SYS_WM_MAXIMIZE_WINDOW exists, but wm_window_info_t
//     carries no `maximized` flag, so a menu label could not reflect the
//     window's real state (always guessing "Maximize" would be wrong, and
//     therefore misleading, for an already-maximized window). Left for a
//     follow-up that also threads a maximized bit through
//     sys_wm_get_windows().
//
// "Close" itself is implemented as a synthetic left-click on the target
// window's own titlebar close (X) button, not a new close primitive. This
// matters because the close-button rect is themeable (#711 mtheme v2): the
// kernel's is_on_close_button() reads TM_TITLEBAR_H/TM_BORDER_W/
// TM_TITLEBAR_BTN live from the active .mtheme file, so a compile-time guess
// at the button's pixel offset here would silently drift out of sync the
// first time a user picks a different theme - exactly the "second
// implementation of the same geometry" trap this tree has hit before.
// Reading the SAME metric ids through the already-wired SYS_THEME_METRIC
// (theme_metric_or(), userland/libc/theme.h) keeps this to one source of
// truth and needs no kernel/proc/syscall.c change (that file is out of
// scope for this pass): the click lands exactly where the kernel's own
// close-button hit test expects it, so behaviour is byte-for-byte what a
// real click on the X produces - no special-casing, no new server-side
// close path.
#define TBMENU_W  108
#define TBMENU_IH 22
// #44: this single-item "Close" popup now serves ONLY the non-XFCE dock
// styles' plain taskbar tile (DOCK_DEFAULT/LUMINA/CLASSIC_UNIX/RETRO_BENCH,
// which have no favorites/pin concept of their own to right-click). The
// XFCE dock's own icon right-click (PINNED/MERGED/RUNNING, with Unpin/Pin/
// Change Icon/window actions) moved to the richer, kind-aware
// contextmenu.c CTX_MODE_DOCK menu below (taskbar_handle_right_click()'s
// DOCK_XFCE branch) - the old UNPIN mode this popup used to carry is gone
// with it, since nothing sets it any more.
static int32_t g_tbmenu_win_id = -1;
static bool    g_tbmenu_open = false;
static int32_t g_tbmenu_x, g_tbmenu_y; // anchor point (click position)

bool taskbar_menu_is_open(void) { return g_tbmenu_open; }

static int32_t tbmenu_width(void) {
    int32_t w = text_width("Close") + 24;
    return w < TBMENU_W ? TBMENU_W : w;
}

static void taskbar_menu_geom(int32_t *mx, int32_t *my, int32_t *h) {
    int32_t mw = tbmenu_width();
    int32_t hh = TBMENU_IH + 4;   // one item
    int32_t x = g_tbmenu_x, y = g_tbmenu_y;
    if (x + mw > g_fb_width)  x = g_fb_width - mw;
    if (x < 0) x = 0;
    if (y + hh > g_fb_height) y = g_fb_height - hh;
    if (y < 0) y = 0;
    *mx = x; *my = y; *h = hh;
}

void taskbar_menu_render(void) {
    if (!g_tbmenu_open) return;
    int32_t x, y, h; taskbar_menu_geom(&x, &y, &h);
    int32_t w = tbmenu_width();
    draw_fill_rect(x, y, w, h, CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    draw_text(x + 12, y + 4 + 5, "Close", CLR_MENU_TEXT);
}

// Send the same DOWN+UP left-click pair sys_inject_mouse() already relays
// into the kernel WM for a real mouse click, aimed at the target window's
// close button (see the file-comment above for why the coordinates are
// computed from live theme metrics rather than hardcoded). #44: no longer
// static - contextmenu.c's CTX_MODE_DOCK "Close" action calls this directly
// (declared in compositor.h) so the XFCE dock's Close does exactly the same
// synthetic-click thing the non-XFCE taskbar-tile Close above always has,
// one implementation either way.
void taskbar_close_window(int32_t win_id) {
    wm_window_info_t wins[TB_MAX_WINS];
    int n = wm_get_windows(wins, TB_MAX_WINS);
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) {
        if (wins[i].id != win_id) continue;
        int titlebar_h = theme_metric_or(THEME_METRIC_TITLEBAR_H, 20);
        int border_w   = theme_metric_or(THEME_METRIC_BORDER_W,   2);
        int btn_size   = theme_metric_or(THEME_METRIC_TITLEBAR_BTN, 16);
        int btn_y_off  = border_w + (titlebar_h - btn_size) / 2;
        int cx = wins[i].x + wins[i].width - btn_size - 2 + btn_size / 2;
        int cy = wins[i].y + btn_y_off + btn_size / 2;
        sys_inject_mouse(cx, cy, MOUSE_EVENT_DOWN, 1);
        sys_inject_mouse(cx, cy, MOUSE_EVENT_UP,   1);
        break;
    }
}

// #44: force-quit the process matching app_id by NAME via SYS_PROC_LIST (the
// same syscall Task Manager uses to build its own process list -
// userland/apps/taskmanager/main.rs), then SYS_KILL(pid, SIGKILL).
// wm_window_info_t carries no pid field (only app_id, a kernel-resolved
// binary basename - #41), so this is how userland turns a window's identity
// into a killable pid without a new kernel field just for this menu action.
// Matches the first process whose name equals app_id exactly (proc_info_t's
// name IS the same kernel-assigned basename app_id already is, so an exact
// match is correct here, not a heuristic) and does nothing if none is found
// - the menu only offers "Force Quit" when app_id is non-empty
// (contextmenu_open_for_dock()), and this is the other half of that same
// "no target, no action" guarantee for the rare case the process already
// exited between the right-click and the click.
void taskbar_force_quit_app_id(const char *app_id) {
    if (!app_id || !app_id[0]) return;
    proc_info_t procs[64];
    int n = sys_proc_list(procs, 64);
    if (n <= 0) return;
    for (int i = 0; i < n; i++) {
        if (strcmp(procs[i].name, app_id) == 0) {
            syscall2(SYS_KILL, (long)procs[i].pid, 9 /* SIGKILL, libc/signal.h */);
            return;
        }
    }
}

// Handle a click while the menu is open: click the item, or click-away
// dismisses (this is a lightweight popup like the widget right-click menu,
// not a true modal - it has no state worth protecting behind ESC-only).
bool taskbar_menu_handle(int32_t x, int32_t y, int click) {
    if (!g_tbmenu_open) return false;
    if (!click) return true;    // swallow hover while open
    int32_t mx, my, h; taskbar_menu_geom(&mx, &my, &h);
    int32_t w = tbmenu_width();
    if (x >= mx && x < mx + w && y >= my + 2 && y < my + 2 + TBMENU_IH) {
        taskbar_close_window(g_tbmenu_win_id);
    }
    g_tbmenu_open = false;
    g_tbmenu_win_id = -1;
    g_needs_redraw = true;
    return true;
}

// Right-press on a taskbar app tile opens the menu anchored at the click.
// Returns false (not consumed) if the press was not on any app tile, so a
// right-click elsewhere in the taskbar strip still falls through to the
// plain taskbar_handle_mouse() below exactly as before.
bool taskbar_handle_right_click(int32_t x, int32_t y) {
    // #44: DOCK_XFCE dock icons use their own hit-list (g_xdock_*), since a
    // pinned-not-running slot has no window id. Routes into contextmenu.c's
    // CTX_MODE_DOCK, which builds the right item set for whichever of
    // PINNED/MERGED/RUNNING this slot is - see contextmenu_open_for_dock()'s
    // own comment for the exact rule. This used to only offer "Unpin from
    // Favorites" on a PINNED/MERGED slot and nothing at all on RUNNING
    // (bailing out with `return false` immediately below); #41 landing
    // app_id on wm_window_info_t is what unblocked the rest (see
    // g_xdock_path[]'s comment above, and startmenu_find_by_app_id()).
    if (g_dock_style == DOCK_XFCE) {
        if (y < g_xdock_y || y >= g_xdock_y + g_xdock_h) return false;
        int32_t w = XFCE_DOCK_ICON + XFCE_DOCK_PAD;
        for (int i = 0; i < g_xdock_n; i++) {
            if (x < g_xdock_x[i] || x >= g_xdock_x[i] + w) continue;
            // "Maximize" vs "Restore" needs the window's REAL current state
            // (wm_toggle_maximize_focused() is a toggle - see the
            // wm_window_info_t.maximized comment, #44), fetched fresh here
            // rather than cached in the per-frame dock hit-list, same
            // pattern taskbar_close_window() already uses to look up a
            // window by id at click time.
            bool maximized = false;
            if (g_xdock_win_id[i] >= 0) {
                wm_window_info_t wins[TB_MAX_WINS];
                int wn = wm_get_windows(wins, TB_MAX_WINS);
                if (wn < 0) wn = 0;
                for (int k = 0; k < wn; k++) {
                    if (wins[k].id == g_xdock_win_id[i]) { maximized = wins[k].maximized != 0; break; }
                }
            }
            bool is_fav = (g_xdock_kind[i] == XDOCK_KIND_PINNED || g_xdock_kind[i] == XDOCK_KIND_MERGED);
            contextmenu_open_for_dock(x, y, g_xdock_win_id[i], maximized,
                                      g_xdock_app_id[i], g_xdock_path[i],
                                      (icon_id_t)g_xdock_icon_id[i], is_fav);
            g_needs_redraw = true;
            return true;
        }
        return false;
    }
    int32_t win_id = -1;
    if (g_dock_style == DOCK_DEFAULT) {
        if (g_tb_btn_w > 0 && y >= g_tb_btn_y && y < g_tb_btn_y + g_tb_btn_h) {
            for (int i = 0; i < g_tb_btn_n; i++) {
                if (x >= g_tb_btn_x[i] && x < g_tb_btn_x[i] + g_tb_btn_w) {
                    win_id = g_tb_btn_id[i];
                    break;
                }
            }
        }
    } else {
        // Lumina / Classic UNIX / Retro Bench share the g_lay_* hit-list;
        // id == -1 there marks the launcher/start slot, not an app window.
        if (y >= g_lay_y && y < g_lay_y + g_lay_h) {
            for (int i = 0; i < g_lay_n; i++) {
                if (x >= g_lay_x[i] && x < g_lay_x[i] + g_lay_w[i]) {
                    win_id = g_lay_id[i];
                    break;
                }
            }
        }
    }
    if (win_id < 0) return false;   // no app tile under the cursor
    g_tbmenu_win_id = win_id;
    g_tbmenu_x = x; g_tbmenu_y = y;
    g_tbmenu_open = true;
    g_needs_redraw = true;
    return true;
}

// ---------------------------------------------------------------------------
// Dispatchers + work-area insets.
// ---------------------------------------------------------------------------
void taskbar_render(void) {
    switch (g_dock_style) {
        case DOCK_LUMINA: g_taskbar_y = g_fb_height - LUMINA_DOCK_RESERVE; taskbar_render_lumina(); break;
        case DOCK_CLASSIC_UNIX:   g_taskbar_y = g_fb_height - CLASSIC_UNIX_PANEL_H;      taskbar_render_classic_unix();   break;
        case DOCK_RETRO_BENCH: g_taskbar_y = 0;                             taskbar_render_retro_bench(); break;
        case DOCK_XFCE:   g_taskbar_y = XFCE_PANEL_H;                       taskbar_render_xfce(); break;
        default:         g_taskbar_y = g_fb_height - TASKBAR_HEIGHT;   taskbar_render_default(); break;
    }
}

bool taskbar_handle_mouse(int32_t x, int32_t y, bool clicked) {
    switch (g_dock_style) {
        case DOCK_LUMINA: return taskbar_handle_lumina(x, y, clicked);
        case DOCK_CLASSIC_UNIX:   return taskbar_handle_classic_unix(x, y, clicked);
        case DOCK_RETRO_BENCH: return taskbar_handle_retro_bench(x, y, clicked);
        case DOCK_XFCE:   return taskbar_handle_xfce(x, y, clicked);
        default:         return taskbar_handle_default(x, y, clicked);
    }
}

int taskbar_top_inset(void) {
    return (g_dock_style == DOCK_LUMINA) ? LUMINA_MENUBAR_H
         : (g_dock_style == DOCK_RETRO_BENCH) ? RETRO_BENCH_BAR_H
         : (g_dock_style == DOCK_XFCE) ? XFCE_PANEL_H : 0;
}
int taskbar_bottom_inset(void) {
    switch (g_dock_style) {
        case DOCK_LUMINA: return LUMINA_DOCK_RESERVE;
        case DOCK_CLASSIC_UNIX:   return CLASSIC_UNIX_PANEL_H;
        case DOCK_RETRO_BENCH: return 0;
        case DOCK_XFCE:   return XFCE_DOCK_H;
        default:         return TASKBAR_HEIGHT;
    }
}
// (#745) The remaining two edges of the work area. Every dock style shipping
// today occupies a HORIZONTAL edge, so both are 0, but they are derived per
// style in the same shape as the other two rather than assumed: adding a left-
// or right-edge dock is then a case here and nothing else in the tree changes,
// because every placement path consumes the work area, not "the taskbar
// height". Hardcoding "top" would have been the same mistake in reverse: it
// would reserve the wrong edge for the classic bottom-panel styles.
int taskbar_left_inset(void) {
    switch (g_dock_style) {
        default: return 0;
    }
}
int taskbar_right_inset(void) {
    switch (g_dock_style) {
        default: return 0;
    }
}

// The one derivation. Screen minus the four insets of the ACTIVE style,
// sanity-clamped so a bad style can never produce an empty rect.
void taskbar_work_area(int *x, int *y, int *w, int *h) {
    int l = taskbar_left_inset(),  t = taskbar_top_inset();
    int r = taskbar_right_inset(), b = taskbar_bottom_inset();
    int sw = (int)g_fb_width, sh = (int)g_fb_height;
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r < 0) r = 0;
    if (b < 0) b = 0;
    if (l + r >= sw) { l = 0; r = 0; }
    if (t + b >= sh) { t = 0; b = 0; }
    if (x) *x = l;
    if (y) *y = t;
    if (w) *w = sw - l - r;
    if (h) *h = sh - t - b;
}

// (#745, local 81) THE one place a popup, menu, flyout or overlay panel is
// fitted on screen. Six sites hand-rolled this arithmetic and three of them
// got it wrong: the Start-menu flyout and the tray menu clamped against the
// raw framebuffer (so their last rows were painted over the dock, which draws
// BELOW them in the layer order), the tray menu clamped in only one of its
// three anchor branches, the command launcher did not clamp its bottom at all,
// and the per-widget menu had no x<0/y<0 floor. The work area comes from
// taskbar_work_area(), the same strut the kernel WM, desktop icons and widget
// modals already use, so a top-panel style is handled without any caller
// knowing which style is active.
//
// Clamp order is MAX first, then MIN, matching widget_clamp_pos(): a popup
// taller than the work area then starts at the top edge and overflows the
// bottom (visible, scrollable) rather than starting off-screen above.
void popup_clamp_to_work_area(int w, int h, int *x, int *y)
{
    int ax, ay, aw, ah;
    taskbar_work_area(&ax, &ay, &aw, &ah);
    const int m = POPUP_EDGE_MARGIN;
    int x0 = ax + m, x1 = ax + aw - m;
    int y0 = ay + m, y1 = ay + ah - m;
    if (x) {
        if (*x + w > x1) *x = x1 - w;
        if (*x < x0)     *x = x0;
    }
    if (y) {
        if (*y + h > y1) *y = y1 - h;
        if (*y < y0)     *y = y0;
    }
}

// (#40) Does the ACTIVE style's bottom chrome FLOAT OVER the desktop rather
// than fence it off? The marble dock does: it is a centred, variable-width
// glass pane sitting on the screen's bottom edge, and the style's own taskbar
// is the TOP panel. So the thing a piece of desktop furniture must not cover
// in this style is the TOP, and the user asked for the bottom edge back:
// "allow the widgets to go to the bottom of the screen but not right to the
// top (as the taskbar is now there)".
// Deliberately NOT extended to the other styles here. DOCK_DEFAULT and
// DOCK_CLASSIC_UNIX put their one and only panel at the BOTTOM, so relaxing
// the bottom there would bury the taskbar itself; DOCK_LUMINA's bottom dock
// has the same shape as the marble one but was not part of what was asked for
// and is unverified, so it keeps today's behaviour. Adding it is one case
// label here and nothing else in the tree, because everything that places
// desktop furniture goes through taskbar_widget_area() below.
int taskbar_dock_overlays_desktop(void) {
    switch (g_dock_style) {
        case DOCK_XFCE: return 1;
        default:        return 0;
    }
}

// (#40) THE bounds for FLOATING DESKTOP FURNITURE (relocatable widgets, and
// the sheep/dog pets' ground level), as distinct from taskbar_work_area()
// above, which is what an app WINDOW may occupy. They are the same rect
// except under an overlay-dock style, where this one runs to the bottom edge
// of the screen: a widget is allowed to sit under the floating dock, an app
// window is not. Derived by giving back exactly the gap taskbar_work_area()
// left below itself, so the sanity clamps in there (which can zero the insets
// on an absurd screen size) can never be double-counted here.
void taskbar_widget_area(int *x, int *y, int *w, int *h) {
    int ax, ay, aw, ah;
    taskbar_work_area(&ax, &ay, &aw, &ah);
    if (taskbar_dock_overlays_desktop()) {
        int give_back = (int)g_fb_height - (ay + ah);
        if (give_back > 0) ah += give_back;
    }
    if (x) *x = ax;
    if (y) *y = ay;
    if (w) *w = aw;
    if (h) *h = ah;
}

// (#40) The ACTIVE style's chrome as SOLID SURFACES, in screen coordinates.
// This exists so that anything which must treat the panels as obstacles feeds
// them into the rectangle list it ALREADY scans (the sheep pet's floor/wall
// scan already walks the window list; the dock now simply appears in that
// same list) instead of growing a second, style-aware collision path that
// would drift from this file's geometry the next time a dock is restyled.
// Returns the number written, never more than `max`.
int taskbar_panel_rects(chrome_rect_t *out, int max) {
    int n = 0;
    int W = (int)g_fb_width, H = (int)g_fb_height;
    if (!out || max <= 0) return 0;
    int t = taskbar_top_inset();
    if (t > 0 && n < max) {
        out[n].x = 0; out[n].y = 0; out[n].w = W; out[n].h = t; n++;
    }
    if (g_dock_style == DOCK_XFCE) {
        // The one style whose bottom chrome is narrower than the screen: use
        // the live rect cached by taskbar_render_xfce_dock(). Width 0 means
        // it has not painted yet (first frame), and there is then genuinely
        // no dock on screen to collide with.
        if (g_xfce_dock_w > 0 && n < max) {
            out[n].x = g_xfce_dock_x; out[n].y = H - XFCE_DOCK_H;
            out[n].w = g_xfce_dock_w; out[n].h = XFCE_DOCK_H; n++;
        }
    } else {
        int b = taskbar_bottom_inset();
        if (b > 0 && n < max) {
            out[n].x = 0; out[n].y = H - b; out[n].w = W; out[n].h = b; n++;
        }
    }
    return n;
}

// THE "the work area just changed" hook. One function, so no layer can be
// forgotten:
//   1. push the derived strut to the KERNEL window manager, which owns
//      app-window placement (create / maximize / restore / SYS_WIN_MOVE) and
//      the title-bar drag;
//   2. re-clamp the compositor-side saved geometry (desktop icons, relocatable
//      widgets), which may have been written under a different dock style.
// Called from taskbar_set_style() so a live style switch can never leave the
// layers describing different geometry, AND once explicitly at startup,
// because set_style() early-returns when the profile-loaded style already
// equals g_dock_style - which is exactly the case on every normal boot.
void taskbar_apply_work_area(void) {
    sys_wm_set_work_area(taskbar_left_inset(), taskbar_top_inset(),
                         taskbar_right_inset(), taskbar_bottom_inset());
    desktop_reclamp_icons();
    widgets_clamp_to_bounds();
}

int taskbar_menu_drops_from_top(void) {
    return (g_dock_style == DOCK_LUMINA) || (g_dock_style == DOCK_RETRO_BENCH) || (g_dock_style == DOCK_XFCE);
}
void taskbar_set_style(int s) {
    if (s < 0 || s >= DOCK_COUNT) s = DOCK_DEFAULT;
    if (s == g_dock_style) return;
    g_dock_style = s;
    // (#745) The reserved edge just changed: republish immediately, before the
    // next frame places or draws anything, and let the kernel WM re-clamp the
    // windows that are already open under the old reservation.
    taskbar_apply_work_area();
    g_perf_open = 0;
    g_start_menu_open = false;
    g_tray_menu_open = 0;
    g_needs_redraw = true;
}

// #241: is the performance popup currently showing?
bool taskbar_popup_active(void) {
    return g_perf_open != 0;
}

// #241: handle mouse while the performance popup is open. Runs in main.c BEFORE
// taskbar_handle_mouse so it can intercept clicks anywhere on screen.
bool taskbar_popup_handle_mouse(int32_t x, int32_t y, bool clicked) {
    if (!g_perf_open) return false;

    int inside_popup = (x >= g_pp_x && x < g_pp_x + g_pp_w &&
                        y >= g_pp_y && y < g_pp_y + g_pp_h);

    if (!clicked) {
        // Swallow hover only while over the popup, so it does not bleed through.
        return inside_popup != 0;
    }

    if (inside_popup) {
        // Task Manager button?
        if (x >= g_pp_tm_x && x < g_pp_tm_x + g_pp_tm_w &&
            y >= g_pp_tm_y && y < g_pp_tm_y + g_pp_tm_h) {
            sys_spawn("/APPS/taskmgr");
            g_perf_open = 0;
        }
        g_needs_redraw = true;
        return true;
    }

    // Click on a gauge: toggle (same gauge closes) or switch.
    int gh = gauge_hit(x, y);
    if (gh >= 0) {
        if (gh == g_perf_sel) g_perf_open = 0;
        else                  g_perf_sel  = gh;
        g_needs_redraw = true;
        return true;
    }

    // Click anywhere else: dismiss the popup and consume the click.
    g_perf_open = 0;
    g_needs_redraw = true;
    return true;
}

int32_t taskbar_get_y(void) {
    // Top of the reserved BOTTOM work-area edge (layout-aware, #387). For a
    // top-bar-only layout (Retro Bench) the bottom inset is 0 so this is the screen
    // bottom, i.e. no bottom reservation.
    return g_fb_height - taskbar_bottom_inset();
}
