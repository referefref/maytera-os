// taskbar.c - Taskbar with system gauges for MayteraOS Userland Compositor
// Phase 3: Complete Desktop Port

#include "compositor.h"
#include "confirmdialog.h"   // #745: shared confirm card, Force Quit gate
#include "../../libc/syscall.h"
#include "../../libc/bt_client.h"   // #372: Bluetooth tray indicator (honest stub, #237)
#include "../../libc/wifi_client.h" // #384: Network/Wi-Fi tray indicator (honest stub, #237)
#include "../../libc/theme.h"       // #711 mtheme v2 metrics (taskbar-tile Close geometry)
#include "../../libc/stdio.h"       // #41: snprintf for the title-fallback debug line
#include "../../libc/math.h"        // #63/#745: sqrtf for xfce_dock_paint_rounded()'s AA corner coverage
#include "../../libc/dock_opacity.h" // #132: shared DOCK_OPACITY_MIN/MAX/WARN
#include "../../libc/settingscfg.h"  // #230: settingscfg_use24h() for tb_clock_str()
#include "../../libc/battery.h"     // #battmeter: tray battery meter
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
// ai_launcher_icon_draw_boxed() and ai_glyphs.h for the full rationale.

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
//
// (#glassmodal) flat_chrome_alpha(), glass_or_flat(), glass_edge_h(),
// glass_edge_v() and glass_highlight_h() MOVED to draw.c (next to
// glass_render(), where their sibling logic already lives) and are no
// longer static, so confirmdialog.c and this file's own draw_perf_popup()
// can call the SAME functions instead of each hand-rolling a third copy -
// see compositor.h for the prototypes and draw.c for the full history
// comments (the #123 item 4 opacity-floor measurement, the #123 item 2 dock
// cache-slice sizing note referenced from there, etc). No behavior change at
// any call site below: same functions, same math, just non-static now.
// tb_lighten_argb() (the taskbar_bg lighten helper glass_or_flat()'s flat
// fallback used) had no other caller, so it moved too, inlined into draw.c's
// glass_or_flat() as glass_flat_lighten() rather than kept here unused.
// ---------------------------------------------------------------------------
// #387 Alternate dock/taskbar layout metrics (used by both the renderers below
// and taskbar_collect_damage above them, so defined here at file scope).
// #uiscale: scaled at the definition, same rationale as compositor.h's
// TASKBAR_*/START_MENU_*/etc block - every consumer (draw + hit-test) picks
// up the factor with no per-call-site edit. Checked for compile-time-context
// use (array size/static init/#if) first; none of these five are.
#define LUMINA_MENUBAR_H     ui_px(24)
#define LUMINA_DOCK_ICON     ui_px(40)
#define LUMINA_DOCK_PAD      ui_px(8)
#define LUMINA_DOCK_RESERVE  ui_px(64)     // work-area reserved at bottom for the dock
#define CLASSIC_UNIX_PANEL_H       ui_px(58)
#define RETRO_BENCH_BAR_H       ui_px(20)
// #26 XFCE ("Marble" in Settings): flush top panel + flush-to-bottom-edge
// fit-content dock (pinned favorites, then a separator, then running-but-
// not-pinned windows). Glass (both surfaces) + a hover lift/grow on the dock
// icons as of #745 - STALE "opaque/no translucency/no magnification" wording
// fixed, see taskbar_render_xfce_panel() below for the current state. See
// docs/DESKTOP_SHELL_RESEARCH.md section 4 and docs/DOCK_XFCE_MOCKUP.html
// for the ported spec table this geometry is taken from verbatim.
#define XFCE_PANEL_H      ui_px(30)
#define XFCE_BTN_SIZE     ui_px(24)
// XFCE_DOCK_ICON is the DEFAULT DOCK TILE size: slot spacing, dock width,
// hit-boxes, hover-rect geometry. (#123 item 2) It is no longer read directly
// by any drawing or hit-testing code - the tile is now derived from the user's
// dock height by xfce_tile_for_h() below, and this constant survives only as
// the term that makes XFCE_DOCK_H_DEFAULT come out at today's geometry. It
// must NOT move for a "shrink the icon" request (the dock itself must not
// change); that is what the 90% rule inside xfce_draw_slot() is for.
#define XFCE_DOCK_ICON    40
// #63/#745 USER-REPORTED: "the icons in the dock (marble) are too large,
// reduce their size compared to the dock by 10%". xfce_draw_slot() below is
// the ONE call site that turns a favourite into pixels on screen (both the
// pinned-favourites and running-only loops in taskbar_render_xfce_dock()
// share it), and 90% of the tile is the base (not-hovered) icon size there.
// (#123) This used to be the literal 36 (= 40 * 0.9) in its own #define; with
// a configurable tile it became `tile * 9 / 10` computed in xfce_draw_slot()
// (`base` there), because a fixed 36 inside a 25px or a 77px tile
// is not "10% smaller than the tile", it is broken. The rule is unchanged;
// only its expression is. The hover grow/lift animation is unaffected: it
// adds to whatever base size that yields, same as before.
#define XFCE_DOCK_PAD     8
#define XFCE_DOCK_IND     8    // indicator-bar band under the icon row
// (#123 item 5) USER-REPORTED: "the padding (below dock icons) is too large".
// The dock's vertical stack used to be PAD(8) + ICON(40) + IND(8) + PAD(8),
// i.e. the SAME constant served as the top pad, the bottom pad and the
// horizontal gutter. The bottom one is the only one the user is complaining
// about (the indicator band already provides 8px of clear space under the
// icon row before it), so it becomes its own constant instead of a fourth
// use of XFCE_DOCK_PAD. MEASURED on a rendered frame before this change: the
// last painted dock pixel below the icon row's baseline was the focused
// running indicator at dock_y+50..53, leaving 10 dead px below it. At 3 it
// leaves 5, which still reads as a margin rather than a clipped edge.
#define XFCE_DOCK_PAD_BOT 3
// (#123 item 2) The marble dock's total height is now a user preference.
// XFCE_DOCK_H is therefore NOT a constant any more: every consumer goes
// through xfce_dock_h() below, which clamps the live value. The DEFAULT is
// the pre-#123 stack with item 5's smaller bottom pad, so a profile that has
// never set a height gets exactly "today's dock, minus the dead 5px".
#define XFCE_DOCK_H_DEFAULT (XFCE_DOCK_ICON + XFCE_DOCK_PAD + XFCE_DOCK_IND + XFCE_DOCK_PAD_BOT)  // 59
// Floor/ceiling. The floor is derived, not picked: at 44 the icon tile is
// 44-8-8-3 = 25px, which is the smallest size icon_draw_scaled()'s
// nearest-sample downscale of a 64x64 source still resolves the CoreUI line
// art at (below that the 1-2px strokes start dropping out entirely). The
// ceiling is what GLASS_DOCK_PX (draw.c) is sized for; raising it past 96
// means raising that cache slice in the same change or the dock silently
// loses its blur at the top of the range.
#define XFCE_DOCK_H_MIN   44
#define XFCE_DOCK_H_MAX   96
// (#123 auto-scale) THE CONTRACT, stated here because the next person to touch
// this WILL otherwise reintroduce the conflict between "the user picked 96" and
// "28 icons do not fit on this screen":
//
//   * g_dock_height is a PREFERRED MAXIMUM, not a fixed value.
//   * Auto-scale may only ever shrink BELOW that preference to satisfy the
//     width budget. It must NEVER grow the dock above what the user asked for.
//   * When the item count drops again the dock returns to the preference. That
//     is guaranteed STRUCTURALLY, not by a reset path that can be forgotten:
//     the effective height is recomputed from scratch every frame as a pure
//     function of (preference, item count, framebuffer width). There is no
//     stored shrunken state, so there is nothing to fail to restore.
//
// THE BUDGET IS 75% OF THE *CURRENT* FRAMEBUFFER WIDTH, read from g_fb_width -
// the same source taskbar_render_xfce_dock() already centres the pane against.
// Not a boot-time constant and not a second copy: the display supports 90/180/
// 270 rotation (#102), which swaps the framebuffer's width and height, and a
// cached width would leave the dock budgeted for the wrong screen after a
// rotation.
//
// MEASURED, since the arithmetic decides whether this rule ever fires: the
// resting pane width is items*tile + (items+1)*pad (+pad+1 for the separator).
// At the default height the tile is 40 and the gutter 8, so the PITCH IS 48,
// not 56 - the tile IS the 40, the gutter is added once per slot. 28 items
// therefore want 28*40 + 29*8 = 1352px. That is over budget on a 1280-wide
// screen (75% = 960) and UNDER budget on 1920 (75% = 1440), so on a 1080p
// display 28 pins need no auto-scale at all and the dock stays at the user's
// height. At the user-selectable floor of 44 the tile is 25, and 28 items want
// 28*25 + 29*8 = 932px, which meets the 960 budget on 1280.
#define XFCE_DOCK_WIDTH_BUDGET_PCT 75
// The emergency floor, below the user-selectable XFCE_DOCK_H_MIN. Reached only
// when the row does not fit the HARD screen budget (W-24) even at H_MIN with
// the gutter already tightened - i.e. never on any supported display: the
// absolute worst case is XDOCK_MAX (44) items, which at this floor (tile 16)
// and a 3px gutter want 44*16 + 45*3 = 839px, inside a 1024-wide screen's
// 1000px hard budget. Dropping items is NOT an accepted outcome, so the ladder
// exhausts every size reduction before the pane is clamped at all; if a future
// display really is too narrow even for that, the pane clamps to W-24 and the
// tail is clipped, which is visible rather than silent.
#define XFCE_DOCK_H_HARD_MIN 35
// (#123) XFCE_DOCK_SEP_EXTRA was (XFCE_DOCK_PAD + 1). The separator's extra
// span is now written as (pad + 1) against the frame's LIVE gutter, in the
// two places that need it (the width computation and the draw loop), because
// the width-fit pass can shrink `pad` below XFCE_DOCK_PAD - a constant built
// from the unshrunk value would have made the separator gap disagree with
// the space actually reserved for it.
// (#123 item 3) Hover magnification, as a PERCENT of the base icon size, so
// the control the user asked for ("hover zoom factor (%)") is the number the
// code actually uses rather than a px delta a UI would have to invent a
// percentage for. 100 = no zoom at all. The default reproduces the pre-#123
// fixed +10px grow EXACTLY at the default height: base icon 36, 36*28/100 =
// 10 (integer), and the lift stays at the same 3:5 ratio to the grow, so a
// profile that never touches the slider sees a byte-identical animation.
#define XFCE_DOCK_ZOOM_MIN     100
#define XFCE_DOCK_ZOOM_MAX     200
#define XFCE_DOCK_ZOOM_DEFAULT 128
// Live values. Owned here (this file is the only consumer of the geometry);
// main.c drives the DOCKHGT.CFG/DOCKZOOM.CFG live channels and profile.c the
// UIPROFIL.YML persistence, exactly like g_dock_style/g_dock_opacity.
int g_dock_height = XFCE_DOCK_H_DEFAULT;
int g_dock_zoom   = XFCE_DOCK_ZOOM_DEFAULT;

// THE clamp. Everything that needs the dock's height calls this, never
// g_dock_height directly, so an out-of-range value written into the profile
// or the CFG by hand can never produce a dock taller than the screen or one
// with a negative icon tile.
// #uiscale: g_dock_height is a PRE-EXISTING user preference (the Settings
// "dock height" slider, 44..96 LOGICAL px - see the #123 block above) and
// g_dock_zoom is a SEPARATE PRE-EXISTING preference (the hover-magnification
// PERCENT applied at icon-draw time in xfce_draw_slot(), unrelated to this
// function). Neither is replaced or fought here.
//
// The clamp against XFCE_DOCK_H_MIN/MAX stays in the SAME logical units the
// Settings slider works in - a value that has always meant "44..96" keeps
// meaning that regardless of scale. ui_px() is applied ONCE, after the
// clamp, to produce the PHYSICAL height this file actually draws and hit-
// tests with (taskbar.c draws straight to the physical framebuffer). That
// physical height is the BASE every downstream consumer derives from -
// xfce_tile_for_h() below turns it into the icon tile, and xfce_draw_slot()
// then applies g_dock_zoom's hover-magnification PERCENT on top of that
// already-scaled tile at draw time, i.e. exactly "scaled_base * dock_zoom_
// fraction", never the other way round.
//
// KNOWN SIMPLIFICATION: XFCE_DOCK_PAD/XFCE_DOCK_IND/XFCE_DOCK_PAD_BOT (the
// fixed chrome bands xfce_tile_for_h() subtracts) are deliberately NOT scaled
// here - they are load-bearing in this file's width-auto-fit-budget
// arithmetic (XFCE_DOCK_WIDTH_BUDGET_PCT and the MEASURED comments above),
// which was tuned against g_fb_width (always physical) at these exact
// values. Scaling them too would keep the proportions of the tile-vs-chrome
// split closer to the original design but risks destabilizing that budget
// fit; left as a follow-up (see the uiscale task's own final report).
static int32_t xfce_dock_h(void) {
    int h = g_dock_height;
    if (h < XFCE_DOCK_H_MIN) h = XFCE_DOCK_H_MIN;
    if (h > XFCE_DOCK_H_MAX) h = XFCE_DOCK_H_MAX;
    return (int32_t)ui_px(h);
}
// The icon TILE size at the current height: slot spacing, hit-boxes, hover
// rect. The chrome bands (top pad, indicator band, bottom pad) are fixed, so
// the whole of a height change lands on the icon, which is what "make the
// dock bigger" means to a user.
static int32_t xfce_tile_for_h(int32_t h) {
    int32_t t = h - XFCE_DOCK_PAD - XFCE_DOCK_IND - XFCE_DOCK_PAD_BOT;
    if (t < 16) t = 16;
    return t;
}
static int32_t xfce_dock_zoom(void) {
    int z = g_dock_zoom;
    if (z < XFCE_DOCK_ZOOM_MIN) z = XFCE_DOCK_ZOOM_MIN;
    if (z > XFCE_DOCK_ZOOM_MAX) z = XFCE_DOCK_ZOOM_MAX;
    return (int32_t)z;
}
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
//
// (#132 CAVEAT, not re-measured here) The "worst case" above was op=70
// because 70 was the lowest opacity that could ever be reached at the time -
// glass_render() hard-clamped everything below it. #132 lowered the real
// floor to DOCK_OPACITY_MIN (15, dock_opacity.h), so op=70 no longer bounds
// the actual worst case: at a genuinely low opacity the composited surface is
// dominated by whatever the desktop wallpaper is, not the derived tint, and
// this fixed ink color's contrast against it is UNMEASURED below 70. This is
// a real, stated gap this change introduces, not a claim that it is fine -
// re-run /tmp/dockicon2/contrast_calc.py (or its equivalent) swept down to 15
// before relying on any specific number here.
#define CLR_DOCK_ICON_INK 0xFFF2F2F2u
// (#123 item 1) 12 -> 28, per the user's ask. This MUST stay equal to
// startmenu.c's MAX_FAVORITES: startmenu_get_favorites() copies out of
// g_fav_paths[MAX_FAVORITES], and the dock asks for XFCE_MAX_FAVS of them.
// A smaller value here silently hides the tail of the user's own pin list.
#define XFCE_MAX_FAVS     28   // matches startmenu.c's MAX_FAVORITES

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
// #uiscale: scaled at the definition (see the compositor.h block comment
// for why this is safe/preferred over per-call-site edits).
#define TB_BTN_W   ui_px(120)   // preferred per-button width (shrinks to fit)
#define TB_BTN_GAP ui_px(3)
#define TB_ICON_SZ ui_px(16)

// Hitboxes recorded each frame so taskbar_handle_mouse can focus on click.
// (#231) TB_BTN_MAX, not TB_MAX_WINS: DOCK_DEFAULT's strip can now also hold
// one tile per pinned-but-not-running favourite (see g_tb_btn_fav_idx below),
// and the worst case is every favourite pinned AND none of them running.
#define TB_BTN_MAX (XFCE_MAX_FAVS + TB_MAX_WINS)
static int32_t g_tb_btn_x[TB_BTN_MAX];       // left edge of each drawn button
static int32_t g_tb_btn_id[TB_BTN_MAX];      // window id for that button, or -1 (favourite tile)
static int     g_tb_btn_focused[TB_BTN_MAX]; // is that window currently focused?
// (#231) >= 0 for a pinned-but-not-running favourite tile (index into that
// frame's startmenu_get_favorites() array); -1 for every ordinary
// running-window tile, unchanged from before this existed.
static int     g_tb_btn_fav_idx[TB_BTN_MAX];
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

// #231: true if some running app window already matches this favourite - the
// SAME two-pass identity match (#41, exact app_id then a title-substring
// fallback for Win16/DOS guests) DOCK_XFCE's Marble dock already uses to
// avoid drawing a favourite twice (once as itself, once as its own running
// window - the historical Paint/"Maytera Studio" bug). Factored out here so
// the other four dock styles can ask the one question they actually need -
// "does this favourite already have its own running tile this frame?" -
// without re-deriving DOCK_XFCE's fuller PINNED/MERGED/RUNNING bookkeeping,
// which those styles have no use for (they draw running windows exactly as
// they always did; this only decides whether an ADDITIONAL pinned tile is
// needed for a favourite that is not currently running).
static int dock_fav_is_running(const sm_fav_info_t *fav) {
    for (int j = 0; j < g_tb_win_count; j++) {
        if (!tb_window_is_app(&g_tb_wins[j])) continue;
        if (tb_app_id_matches(g_tb_wins[j].app_id, fav->exec_path)) return 1;
    }
    for (int j = 0; j < g_tb_win_count; j++) {
        if (!tb_window_is_app(&g_tb_wins[j])) continue;
        if (tb_contains(g_tb_wins[j].title, fav->name)) return 1;
    }
    return 0;
}

// #129: see the declaration in compositor.h for the full "why" (fixes a
// pre-existing duplicate-window bug shared by every prior Settings deep-link
// call site, not just the new ones this ticket adds). Reuses the exact same
// wm_get_windows()+tb_app_id_matches() lookup taskbar_force_quit_app_id()
// already does for "is there a window for this app right now".
void settings_open_panel(int tab) {
    set_settings_tab(tab);
    wm_window_info_t wins[TB_MAX_WINS];
    int n = wm_get_windows(wins, TB_MAX_WINS);
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) {
        if (tb_app_id_matches(wins[i].app_id, "/APPS/SETTINGS")) {
            wm_focus(wins[i].id);   // un-minimizes too, same as every tile-click path above
            return;   // already running: the live-poll retarget picks up the new tab itself
        }
    }
    sys_spawn("/APPS/SETTINGS");
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

// #208: DOCK/TASKBAR ICON RESOLUTION BY STABLE IDENTITY, not by title.
// tb_icon_for_title() above is a title-substring guess and was, until this
// ticket, the ONLY path every taskbar style used to pick a running window's
// icon. That made every generic-icon report inevitable for any app whose
// window title does not happen to contain one of that function's hardcoded
// keywords - MEASURED against the real fragment/title pairs in
// build/assets/startmenu/system.d/*.MENU, this was not just the reported
// Music Player ("Maytera HiFi", no substring "music"/"audio"/"player"/"media"
// anywhere in it): AI Chat ("Maytera AI Interface"), Snapshot ("Maytera
// Snap"), AssaultCube, ClassiCube, Solitaire, Pong, Lemmings, Device Manager,
// Disk Images, Install to Disk and App Repo ("Repo", not "Store") ALL fell
// through to ICON_WINDOW the same way. Worse, a handful of "hits" were
// silently WRONG: Media Player's title contains "media"/"player" so this
// function returns ICON_MUSIC, even though its own Start-menu fragment says
// icon=video; Network Settings's title contains "network" so this function
// returns ICON_INFO_CIRCLE, even though sm_icon_by_name("network") (the
// authoritative name->icon table startmenu.c already has) says ICON_NETWORK.
// Two independently hand-maintained keyword lists (this function's, and
// sm_icon_by_name() in startmenu.c) had already drifted apart - the same
// "reinvented a shared primitive" class of bug this project has a standing
// rule against.
//
// The fix routes through the identity #41 already introduced for exactly
// this reason: wm_window_info_t.app_id is a kernel-resolved BINARY BASENAME
// (window.c, sys_wm_get_windows()), never the window's own chosen title, and
// startmenu_find_by_app_id() (startmenu.c) already reverse-looks-up that
// app_id against g_menu_items[] - the SAME registry that already carries the
// correct icon_id for every app that has a Start-menu entry (populated from
// each fragment's `icon=` field via sm_icon_by_name(), see
// build/assets/startmenu/system.d/*.MENU). That path is reused here, not
// duplicated: no new table, no new syscall.
//
// tb_icon_for_title() is kept as the fallback for a window whose identity
// does NOT resolve this way (no app_id at all - a kernel-desktop-fallback
// window, or the owning process already exited; or an app_id that exists but
// has no Start-menu entry, e.g. a Win16/DOS shared host process name - see
// the #41 PASS-2 comment above taskbar_render_xfce_dock() for why that is a
// real, expected case and not a bug). A MISS on the identity path is now
// VISIBLE: logged once per distinct title per throttle window, same idiom as
// the existing dock-merge fallback log a few hundred lines below, so the
// next app whose identity does not resolve is discoverable instead of
// silently repeating this exact ticket.
#define TB_ICON_MISS_CACHE 8
static char     s_icon_miss_title[TB_ICON_MISS_CACHE][64];
static uint64_t s_icon_miss_ms[TB_ICON_MISS_CACHE];
static int      s_icon_miss_next = 0;

// Returns 1 (caller should log) at most once per distinct title per 5000ms;
// returns 0 for a repeat within that window. Small fixed-size cache, no
// malloc, same footprint class as every other per-frame taskbar helper.
static int tb_icon_miss_should_log(const char *title) {
    uint64_t now = uptime_ms();
    for (int i = 0; i < TB_ICON_MISS_CACHE; i++) {
        if (s_icon_miss_ms[i] && strncmp(s_icon_miss_title[i], title, 63) == 0) {
            if (now - s_icon_miss_ms[i] < 5000) return 0;
            s_icon_miss_ms[i] = now;
            return 1;
        }
    }
    int slot = s_icon_miss_next;
    s_icon_miss_next = (s_icon_miss_next + 1) % TB_ICON_MISS_CACHE;
    strncpy(s_icon_miss_title[slot], title, 63);
    s_icon_miss_title[slot][63] = '\0';
    s_icon_miss_ms[slot] = now;
    return 1;
}

// The single entry point every render style now calls for a RUNNING window's
// icon (a PINNED/MERGED dock slot already gets its icon_id straight from the
// favorite's g_menu_items[] entry via startmenu_get_favorites() and never
// needed this - only the "running, not in the favorites row" slots and the
// three legacy taskbar styles' task-list tiles ever called
// tb_icon_for_title() directly).
static icon_id_t tb_icon_for_window(const wm_window_info_t *w) {
    // (#dosicon) THE #223 app_id CHECK BELOW (a few lines down) NEVER FIRES
    // FOR A DOS GUEST WINDOW, and never has. MEASURED (throwaway VM, TESTHOOK=1
    // build, Commander Keen 5 launched via the Start menu): kernel/gui/window.c's
    // sys_wm_get_windows() leaves buf[n].app_id empty unless win->owner_pid != 0
    // (see its own "owner_pid == 0 is 'no Ring-3 owner' by construction"
    // comment), and kernel/dos/dosexec.c - which owns every DOS guest window,
    // both dos_launch() and dos_launch_kernel(), i.e. a real Start-menu click
    // and this project's own MENUITEM test verb alike - never sets owner_pid
    // on the window it creates. So w->app_id is "", not "dos"/"dosrun", for
    // every DOS window that has ever existed; the serial log confirms it:
    // "[taskbar] icon: ... window 3 ('KEEN5 (DOS)', app_id='<empty>') ...".
    // The #223 fix a few lines down was verified against a claim, not this
    // log line, and never actually fired.
    //
    // Use the SAME shared detector #778's per-window Speed dialog already had
    // to build for this identical "RUNNING, identity NOT resolved" case
    // (dosspeed.c's dosspeed_window_is_dos(), matching the " (DOS)" title
    // suffix dos_guest_title() appends - the one piece of identity a DOS
    // window actually carries) rather than inventing a second detector here.
    if (dosspeed_window_is_dos(w->id, NULL, 0)) {
        return ICON_DOSAPP;
    }
    if (w->app_id[0]) {
        sm_fav_info_t info;
        if (startmenu_find_by_app_id(w->app_id, &info)) {
            return info.icon_id;
        }
        // #223: a DOS host process ("dos"/"dosrun", kernel/dos/dosexec.c
        // proc_create() names) shares ONE fixed app_id across every guest
        // .EXE it ever runs, so it can NEVER match a specific Start-menu
        // entry's exec_path above - by design, the exact "no Start-menu
        // entry" case the #41 PASS-2 comment on this function already
        // documents, not a miss to log. Give it the real dosapp glyph (the
        // same ICON_DOSAPP every DOS game's own Start-menu fragment already
        // carries via icon=dosapp, loaded at startup from /ICONS/DOSAPP.ICN
        // - see main.c's icon_load_color() call) instead of falling through
        // to the title-keyword guess below, which is what produced the
        // reported blank ICON_WINDOW for Discworld II (and every other DOS
        // title, none of which happen to contain a keyword that function
        // recognizes as a game).
        if (strcmp(w->app_id, "dos") == 0 || strcmp(w->app_id, "dosrun") == 0) {
            return ICON_DOSAPP;
        }
    }
    icon_id_t fallback = tb_icon_for_title(w->title);
    if (tb_icon_miss_should_log(w->title)) {
        char dbg[192];
        int dl = snprintf(dbg, sizeof(dbg),
               "[taskbar] icon: no Start-menu identity match for window %d "
               "('%s', app_id='%s'), fell back to title-match (icon=%d)\n",
               w->id, w->title, w->app_id[0] ? w->app_id : "<empty>",
               (int)fallback);
        if (dl > 0) {
            if (dl >= (int)sizeof(dbg)) dl = (int)sizeof(dbg) - 1;
            write(1, dbg, (size_t)dl);
        }
    }
    return fallback;
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
    // (#glassmodal) Radius matches the HA/sysmon/WMENU card family elsewhere
    // in the tree (see docs/GLASS_MODALS_AND_POPOUTS.html). Chrome-only: does
    // not feed any hit-test, so it is fine to add here even while chromescale
    // is mid-migration on this file's OTHER (unscaled-on-purpose, see the
    // note below) geometry.
    const int32_t PP_RADIUS = 8;
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

    // (#glassmodal) 3-band shadow approximating the wizard's continuous
    // falloff, reusing draw_rounded_rect (no new shadow primitive) - see
    // docs/GLASS_MODALS_AND_POPOUTS.html's shadow table. Reasoned, not
    // perceptually measured (INFERRED, see CHANGELOG).
    {
        int ob = g_draw_blend;
        g_draw_blend = 46;
        draw_rounded_rect(g_pp_x + 2, g_pp_y + 2, g_pp_w, g_pp_h, PP_RADIUS + 2, 0xFF000000);
        g_draw_blend = 28;
        draw_rounded_rect(g_pp_x + 4, g_pp_y + 4, g_pp_w, g_pp_h, PP_RADIUS + 4, 0xFF000000);
        g_draw_blend = 14;
        draw_rounded_rect(g_pp_x + 6, g_pp_y + 6, g_pp_w, g_pp_h, PP_RADIUS + 6, 0xFF000000);
        g_draw_blend = ob;
    }

    // Capture the real backdrop under all four corners BEFORE painting the
    // panel (see draw_round_corners_capture()'s contract in compositor.h) -
    // restored at the very end of this function, after the Task Manager
    // button, so every band drawn in between still gets rounded off.
    corner_capture_t pp_cc;
    draw_round_corners_capture(&pp_cc, g_pp_x, g_pp_y, g_pp_w, g_pp_h, PP_RADIUS, CORNER_ALL);

    // Panel body: glass when enabled (same CLR_GLASS_TINT/GLASS_SURF_MODAL
    // recipe the taskbar/dock/start menu already use), flat CLR_MENU_BG
    // otherwise - preserves the exact old look when glass is off.
    if (g_glass_enable) glass_render(g_pp_x, g_pp_y, g_pp_w, g_pp_h, CLR_GLASS_TINT, GLASS_SURF_MODAL);
    else                 draw_fill_rect(g_pp_x, g_pp_y, g_pp_w, g_pp_h, CLR_MENU_BG);
    draw_rect_outline(g_pp_x, g_pp_y, g_pp_w, g_pp_h, CLR_MENU_BORDER);
    glass_highlight_h(g_pp_x + PP_RADIUS, g_pp_y + 1, g_pp_w - 2 * PP_RADIUS);

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

    // Restore LAST, after every band above (including the Task Manager
    // button) - see draw_round_corners_capture()'s contract.
    draw_round_corners_restore(&pp_cc);
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
// #battmeter: MUST stay last. Unlike every icon above, this one is not
// always shown - see tray_visible_n()/tray_total_w() and the x-assignment
// loop in tray_render_core(). Putting it last means that when it is hidden
// (no battery, the case for every desktop and every VM this project tests
// on), icons 0..5 are assigned the exact same slots as before this feature
// existed - a no-battery machine's tray is pixel-identical to today, not
// merely visually similar.
#define TRAY_BATTERY  6
#define TRAY_N        7
#define TRAY_ICON_W   ui_px(26)
#define TRAY_ICON_GAP ui_px(2)
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
// #uiscale: `gh` is always a literal (14, from tray_glyphs.h's fixed-size
// pre-rasterized alpha masks - see icons.c/tray_glyphs.h's own "no runtime
// scaling" comment). Those masks are NOT scaled here (known limitation,
// documented at their own definition); what DOES scale is the 13, which is
// half of the tray row height this centers the glyph within - so the fixed-
// size glyph stays correctly centered as the row around it grows.
#define TRAY_GLYPH_Y(y, gh) ((y) + ui_px(13) - (gh) / 2)

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
// the full rationale and provenance. Two sizes are rasterized offline (24px,
// 20px). Blended straight onto whatever is already on screen via
// tray_blit_mask() -> draw_hspan_alpha() (real per-pixel alpha compositing
// against g_fb), so unlike the old /MAYLOGO.DAT path there is no separate
// `bg` argument to keep in sync with chrome_chip() - it reads correctly
// against glass, a hover wash, or a flat bar alike.
// #uiscale BUGFIX: AISTAR_24_MASK/AISTAR_20_MASK (ai_glyphs.h) are fixed-size
// pre-rasterized alpha masks with no runtime scaling - same as every mask in
// tray_glyphs.h. This USED TO require an EXACT size match (24 or 20) and fall
// back to a bitmap "M" letter (draw_text_large(), a raw pixel-replicated 8x16
// glyph drawn at a hardcoded 1x offset/scale - NOT the "DOES scale" this
// comment used to claim) when the caller's box grew past those two exact
// pixel counts. Since the caller's box IS the correctly-scaled
// TASKBAR_BTN_SIZE/XFCE_BTN_SIZE, that meant the AI/"Maytera" launcher button
// broke at EVERY scale except exactly 100% - the box grew, and the glyph
// inside it collapsed to a small stray letter in the corner instead of
// growing with it. Same shape as the desktop-icon-above-64px limitation
// UI_SCALE.md already documents for tray glyphs: draw the fixed-size asset
// at its OWN best-fitting native size, centered in whatever (scaled) box the
// caller gives us, rather than stretching it or replacing it with text. Byte
// identical at 1x (box=24 -> the 24 mask, flush with the box, matching the
// old exact-match call sites exactly).
static void ai_launcher_icon_draw_boxed(int32_t box_x, int32_t box_y, int32_t box_size, uint32_t ink) {
    // Threshold picked so the two shipping 1x callers are byte-identical to
    // their old exact-match behavior: DOCK_DEFAULT's TASKBAR_BTN_SIZE=28 ->
    // the 24 mask (old: bs-4=24, exact match); DOCK_XFCE's XFCE_BTN_SIZE=24
    // -> the 20 mask (old: bs-4=20, exact match). 26 sits strictly between
    // the two, so both land on the same asset they always did at 100%.
    int msize = (box_size >= 26) ? 24 : 20;
    int32_t ix = box_x + (box_size - msize) / 2;
    int32_t iy = box_y + (box_size - msize) / 2;
    if (ix < box_x) ix = box_x;
    if (iy < box_y) iy = box_y;
    if (msize == 24) tray_blit_mask(ix, iy, 24, 24, AISTAR_24_MASK, ink);
    else              tray_blit_mask(ix, iy, 20, 20, AISTAR_20_MASK, ink);
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
        // #uiscale: 12 matches the draw_text_ttf(...,12,...) below; scale both the same way.
        if (font_metrics(0, ui_px(12), fm) == 0)
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
// connected (real net-up: monitor + plug glyph), 2 = Wi-Fi connected (arcs
// with `bars` filled from wifi_tray_signal()). State 2 cannot occur while
// wifi_client.h is a stub (#237): wifi_tray_state() always reports 0/off.
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

// #battmeter: control-method battery tray meter.
//
// RENDERS ONLY WHEN A BATTERY IS PRESENT (g_batt_present == 1). Every VM this
// project tests on has no battery, so the default, measured behaviour is: no
// icon, no reserved slot, no placeholder - see TRAY_BATTERY's comment above
// (it is last in TRAY_N precisely so hiding it does not shift anything else).
//
// POLLING: one syscall (BATT_GEN) at most every 3 seconds decides whether the
// other four are worth asking; #426/CLAUDE.md forbid polling the kernel from
// the draw path, so this is called from tray_render_core() (once per frame)
// but almost always returns on the throttle check before doing anything - the
// same call every tick, gate internally idiom lockscreen.c's
// privacy_cfg_poll() already uses. Never blocks: SYS_BATTERY does a bounded
// in-memory ACPI table scan or a small FAT read, no wait_event anywhere on
// this path.
static int      g_batt_present  = 0;    // 1/0/-1, see battery.h
static int      g_batt_pct      = -1;
static int      g_batt_state    = BATT_ST_UNKNOWN;
static int      g_batt_minutes  = -1;
static int      g_batt_gen      = -1;   // -1 forces the first real fetch
static uint64_t g_batt_poll_ms  = 0;
#define BATT_POLL_INTERVAL_MS 3000

static void battery_tray_poll(void) {
    uint64_t now = uptime_ms();
    if (g_batt_poll_ms != 0 && now - g_batt_poll_ms < BATT_POLL_INTERVAL_MS) return;
    g_batt_poll_ms = now;

    int gen = battery_gen();
    if (gen == g_batt_gen) return;   // nothing changed since last poll
    g_batt_gen = gen;

    int was_present = g_batt_present;
    g_batt_present = battery_present();
    g_batt_pct     = battery_pct();
    g_batt_state   = battery_state();
    g_batt_minutes = battery_minutes();
    if (g_batt_present != was_present) g_needs_redraw = true;  // slot appears/disappears
}

// Fixed-size procedural glyph (18x10 body + 2x4 terminal nub), the same
// fixed-raster-content convention tray_draw_bt/tray_draw_net use (see
// TRAY_GLYPH_Y's comment) - only the CENTERING scales with ui_scale, not the
// glyph's own pixel size. Centering uses the REAL runtime TRAY_ICON_W, not a
// baked-in offset like tray_draw_bt's fixed x+7 (used by a sibling glyph):
// that shortcut only stays correct at 100%, which is exactly the shape of bug
// this feature was told to avoid (traymenu.c's TM_W-vs-tm_item_h() mismatch).
static void tray_draw_battery(int x, int y, int pct, int state) {
    const int BW = 18, BH = 10, NUBW = 2, NUBH = 4;
    int gx = x + (TRAY_ICON_W - (BW + NUBW)) / 2;
    int gy = TRAY_GLYPH_Y(y, BH);

    uint32_t outline = readable_ink(CLR_TASKBAR_BG);
    uint32_t dim = readable_ink_dim(CLR_TASKBAR_BG);

    draw_rect_outline(gx, gy, BW, BH, outline);
    draw_fill_rect(gx + BW, gy + (BH - NUBH) / 2, NUBW, NUBH, outline);

    int p = pct; if (p < 0) p = 0; if (p > 100) p = 100;
    uint32_t fill = (pct < 0) ? dim
                  : (p <= 15) ? CLR_BATTERY_CRIT
                  : (p <= 30) ? CLR_BATTERY_LOW
                  :             CLR_BATTERY_OK;
    int inner_x = gx + 2, inner_y = gy + 2, inner_w = BW - 4, inner_h = BH - 4;
    int fw = (pct < 0) ? 0 : (inner_w * p / 100);
    if (pct >= 0 && p > 0 && fw < 1) fw = 1;
    if (fw > 0) draw_fill_rect(inner_x, inner_y, fw, inner_h, fill);

    if (state == BATT_ST_CHARGING) {
        // Small bolt, drawn with draw_putpixel the same way tray_draw_net's
        // disconnect slash is (no shared line primitive is exported from
        // widgets.c's static wdg_line, so this reuses that exact technique
        // rather than forking a new one). Two short strokes forming a Z
        // kink, centred over the body, in the ink colour so it reads against
        // any fill level including the empty/dim case.
        int cx = gx + BW / 2, cy = gy + BH / 2;
        for (int i = 0; i < 3; i++) { draw_putpixel(cx + i - 1, cy - 3 + i, outline); draw_putpixel(cx + i, cy - 3 + i, outline); }
        for (int i = 0; i < 3; i++) { draw_putpixel(cx - i, cy + i, outline); draw_putpixel(cx - i + 1, cy + i, outline); }
    }
}

// ---------------------------------------------------------------------------
// The click-through info card: percentage + state (+ time remaining when the
// firmware gave a usable rate) in text. A lightweight, click-to-toggle,
// click-elsewhere-to-dismiss popover - the same interaction traymenu.c's
// generic popups already use (see its outside-click check), NOT a Settings-
// style modal (CLAUDE.md's true modal, no click-away-dismiss rule is about
// SETTINGS/WIDGET CONFIGURATION dialogs; this is a status readout, the same
// class as the performance popup (g_perf_open) this file already has).
// ---------------------------------------------------------------------------
static int     g_batt_card_open = 0;
// g_bc_x/y/w/h are the FINALIZED (clamped) on-screen rect, recomputed by
// draw_battery_card() every frame and read back by the hit-test below - they
// are an OUTPUT of that function, not state that should feed its own next
// computation.
//
// #battpop: g_bc_anchor_x is the fixed tray-icon-center anchor, set ONCE by
// battery_card_toggle() and never written anywhere else. Before this fix,
// draw_battery_card() read its anchor from g_bc_x itself (the PREVIOUS
// frame's finalized LEFT EDGE, not a center) and then wrote g_bc_x back to
// that same left edge - so each redraw recomputed "center - w/2" against a
// value that was already a left edge, walking the card left by w/2 every
// single frame (draw_battery_card() runs every frame the card is open,
// gated only by g_batt_card_open, not by g_needs_redraw) until the
// left-edge clamp pinned it at ui_px(4). That is the reported "drifts left,
// flashing, until it reaches the far left and stops" bug - a pure
// self-referential state bug in the anchor variable, present at any
// ui_scale/present_scale (not a scale-conversion bug, though it was
// verified at both anyway per the bug report's suspicion).
static int32_t g_bc_anchor_x = 0;
static int32_t g_bc_x = 0, g_bc_y = 0, g_bc_w = 0, g_bc_h = 0;

static const char *battery_state_label(int state) {
    switch (state) {
        case BATT_ST_CHARGING:    return "Charging";
        case BATT_ST_DISCHARGING: return "On battery";
        case BATT_ST_FULL:        return "Fully charged";
        default:                  return "Status unknown";
    }
}

static void battery_card_toggle(int anchor_x) {
    if (g_batt_card_open) { g_batt_card_open = 0; g_needs_redraw = true; return; }
    if (g_batt_present != 1) return;   // nothing to show; should not be reachable (icon is hidden)
    g_batt_card_open = 1;
    g_bc_anchor_x = anchor_x;   // fixed for the life of this open card; see comment above
    g_needs_redraw = true;
}

// Forward declarations: defined a few lines below (the #387 tray metrics
// block), needed here because draw_battery_card() anchors off the live bar
// position and this file's #battmeter section was placed just above that
// block for narrative order (poll -> glyph -> card -> metrics/hit-test).
extern int     g_tray_bar_top;
extern int32_t g_tray_bar_y;
extern int32_t g_tray_bar_h;

static void draw_battery_card(void) {
    if (!g_batt_card_open) return;
    if (g_batt_present != 1) { g_batt_card_open = 0; return; }

    char line1[32], line2[64];
    if (g_batt_pct >= 0) snprintf(line1, sizeof(line1), "%d%%", g_batt_pct);
    else                 snprintf(line1, sizeof(line1), "Unknown");

    if ((g_batt_state == BATT_ST_DISCHARGING || g_batt_state == BATT_ST_CHARGING) &&
        g_batt_minutes >= 0) {
        int h = g_batt_minutes / 60, m = g_batt_minutes % 60;
        if (h > 0) snprintf(line2, sizeof(line2), "%s - %dh %dm", battery_state_label(g_batt_state), h, m);
        else       snprintf(line2, sizeof(line2), "%s - %dm", battery_state_label(g_batt_state), m);
    } else {
        snprintf(line2, sizeof(line2), "%s", battery_state_label(g_batt_state));
    }

    const int32_t PAD = ui_px(10);
    const int32_t LINE_H = ui_px(18);
    int32_t w1 = text_width(line1), w2 = text_width(line2);
    int32_t content_w = (w1 > w2 ? w1 : w2);
    g_bc_w = content_w + PAD * 2;
    if (g_bc_w < ui_px(120)) g_bc_w = ui_px(120);
    g_bc_h = PAD * 2 + LINE_H * 2;

    // #battpop: always derive from the fixed anchor, never from g_bc_x
    // itself (see the field comments above for why the old version drifted).
    int32_t x = g_bc_anchor_x - g_bc_w / 2;
    if (x < ui_px(4)) x = ui_px(4);
    if (x + g_bc_w > g_fb_width - ui_px(4)) x = g_fb_width - ui_px(4) - g_bc_w;
    int32_t y = g_tray_bar_top ? (g_tray_bar_y + g_tray_bar_h + ui_px(4))
                                : (g_tray_bar_y - g_bc_h - ui_px(4));
    if (y < ui_px(2)) y = ui_px(2);
    g_bc_x = x; g_bc_y = y;   // finalized rect for this frame; hit-test reads these

    draw_fill_rect(x + ui_px(3), y + ui_px(3), g_bc_w, g_bc_h, CLR_MENU_SHADOW);
    draw_fill_rect(x, y, g_bc_w, g_bc_h, CLR_MENU_BG);
    draw_rect_outline(x, y, g_bc_w, g_bc_h, CLR_MENU_BORDER);
    draw_text(x + PAD, y + PAD, line1, CLR_MENU_TEXT);
    draw_text(x + PAD, y + PAD + LINE_H, line2, CLR_MENU_TEXT);
}

// #387: tray metrics + top/bottom anchor state, shared by every dock layout.
int      g_tray_bar_top = 0;   // 1 = tray sits on a TOP bar (menus drop DOWN)
int32_t  g_tray_bar_y   = 0;   // current tray-row top (traymenu anchors to it)
int32_t  g_tray_bar_h   = 26;
// #battmeter: dynamic - the battery slot (always last) counts only while
// present. See TRAY_BATTERY's own comment for why it must stay last.
static int tray_visible_n(void) { return TRAY_N - (g_batt_present == 1 ? 0 : 1); }
static int tray_total_w(void) { int n = tray_visible_n(); return n * TRAY_ICON_W + (n - 1) * TRAY_ICON_GAP; }

// #129: the bar-clock text hitbox, one shared set of globals rather than one
// per dock style, because exactly one dock style (and therefore at most one
// bar clock) is ever on screen at a time. DOCK_DEFAULT draws no bar clock at
// all (g_bar_clock_w stays 0, its handler never sees a hit) - that style's
// clock story is the desktop digital-clock WIDGET instead, see widgets.c's
// "Date & Time" menu item. The other four styles (Lumina, Classic UNIX,
// Retro Bench, XFCE/Marble) each set this to the EXACT rect they just drew
// the clock string into, then their own handle_* checks it - same
// record-then-hit-test discipline notif.c's toast rx/ry already uses, so the
// clickable area can never drift from what is actually on screen.
static int32_t g_bar_clock_x = 0, g_bar_clock_y = 0, g_bar_clock_w = 0, g_bar_clock_h = 0;
static void bar_clock_set_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    g_bar_clock_x = x; g_bar_clock_y = y; g_bar_clock_w = w; g_bar_clock_h = h;
}
static bool bar_clock_hit(int32_t x, int32_t y) {
    return g_bar_clock_w > 0 &&
           x >= g_bar_clock_x && x < g_bar_clock_x + g_bar_clock_w &&
           y >= g_bar_clock_y && y < g_bar_clock_y + g_bar_clock_h;
}
static bool bar_clock_click(int32_t x, int32_t y) {
    if (!bar_clock_hit(x, y)) return false;
    settings_open_panel(SETTINGS_PANEL_DATETIME);
    g_needs_redraw = true;
    return true;
}

// Render the 6-icon system tray starting at (x0,y). The glyph tint follows the
// current bar background: we briefly retarget CLR_TASKBAR_BG (readable_ink's
// reference) to bar_bg so a glass/blue/beveled bar still gets legible icons,
// then restore it. is_top marks a top-bar tray so its menus drop downward.
static void tray_render_core(int x0, int y, uint32_t bar_bg, int is_top) {
    // #372/#384: re-sync shared BT/Wi-Fi state every frame (self-throttled).
    bt_tick(); wifi_tick();
    battery_tray_poll();   // #battmeter: also self-throttled, see its own header
    g_tray_y = y; g_tray_h = 26;
    g_tray_bar_y = y; g_tray_bar_h = 26; g_tray_bar_top = is_top;
    // #battmeter: TRAY_BATTERY (always last) only occupies a slot while
    // present, so a no-battery machine assigns icons 0..5 to the exact same
    // slots this loop always has - see TRAY_BATTERY's own comment.
    {
        int slot = 0;
        for (int i = 0; i < TRAY_N; i++) {
            if (i == TRAY_BATTERY && g_batt_present != 1) {
                g_tray_x[i] = -1000000;   // parked off-screen: never hit-tests true
                continue;
            }
            g_tray_x[i] = x0 + slot * (TRAY_ICON_W + TRAY_ICON_GAP);
            slot++;
        }
    }
    // Network state: REAL wired link wins; otherwise Wi-Fi (a stub, #237 -
    // wifi_tray_state() always reports 0/off, so this branch is currently
    // always false, honestly, because there is no driver).
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
    if (g_batt_present == 1)
        tray_draw_battery(g_tray_x[TRAY_BATTERY], g_tray_y, g_batt_pct, g_batt_state);
    CLR_TASKBAR_BG = saved;
    draw_battery_card();   // #battmeter: click-through info card, drawn last (on top)
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
//
// #129: TRAY_NET and TRAY_BT now deep-link straight to the matching Settings
// panel on a plain (left) click, per the ticket's explicit "wifi -> Network,
// bluetooth -> Bluetooth" spec - previously these opened a small quick-toggle
// popup (see traymenu.c) with the real "...settings" action buried a second
// click deep inside it, which read as "clicking a tray icon does not take
// you anywhere". The quick-toggle popup is NOT removed (nothing is lost -
// bt_power/wifi_power are still one right-click away, see
// taskbar_handle_right_click() below and tray_right_click()); only the
// PRIMARY click's destination changed. TRAY_WIDGETS/TRAY_SOUND/TRAY_SHEEP
// keep their existing left-click-opens-popup behavior unchanged - #129 does
// not name them, and they have no Settings-panel equivalent to deep-link to.
static bool tray_click(int32_t x, int32_t y) {
    if (g_tray_h <= 0 || y < g_tray_y || y >= g_tray_y + g_tray_h) return false;
    for (int i = 0; i < TRAY_N; i++) {
        if (x >= g_tray_x[i] && x < g_tray_x[i] + TRAY_ICON_W) {
            if (i == TRAY_BELL)      notif_toggle_center();     // #168 bell -> center
            else if (i == TRAY_NET)  settings_open_panel(SETTINGS_PANEL_NETWORK);
            else if (i == TRAY_BT)   settings_open_panel(SETTINGS_PANEL_BLUETOOTH);
            else if (i == TRAY_BATTERY) battery_card_toggle(g_tray_x[i] + TRAY_ICON_W / 2);  // #battmeter
            else                     traymenu_open_for_icon(i, g_tray_x[i] + TRAY_ICON_W / 2);
            g_needs_redraw = true;
            return true;
        }
    }
    return false;
}

// #129: right-click on the Wi-Fi/Bluetooth tray icons still reaches the
// quick-toggle popup (power on/off) that a left-click used to open, now that
// left-click deep-links to Settings instead. Uses the exact same g_tray_x[]/
// g_tray_y/g_tray_h hit-list tray_click() does, which every one of the 5 dock
// styles already refreshes each frame via tray_render_core() - so this works
// uniformly across all 5 without a style-specific branch.
static bool tray_right_click(int32_t x, int32_t y) {
    if (g_tray_h <= 0 || y < g_tray_y || y >= g_tray_y + g_tray_h) return false;
    for (int i = 0; i < TRAY_N; i++) {
        if (x >= g_tray_x[i] && x < g_tray_x[i] + TRAY_ICON_W) {
            if (i != TRAY_NET && i != TRAY_BT) return false;   // no secondary action elsewhere
            traymenu_open_for_icon(i, g_tray_x[i] + TRAY_ICON_W / 2);
            g_needs_redraw = true;
            return true;
        }
    }
    return false;
}

// #uiscale hit-test fix: the default-bar Start/logo button geometry used to
// be written out twice - once here for drawing, once again in
// taskbar_handle_default() for hit-testing - as an identical formula over
// already-scaled macros (TASKBAR_PADDING/TASKBAR_HEIGHT/TASKBAR_BTN_SIZE/
// TASKBAR_ICON_SPACE), so it could not drift in VALUE, but it is the same
// duplicated-formula shape this whole audit exists to remove. ONE function
// now, called from both.
static void default_bar_buttons_rect(int32_t *btn_x, int32_t *btn_y, int32_t *logo_x) {
    *btn_x = TASKBAR_PADDING;
    *btn_y = g_taskbar_y + (TASKBAR_HEIGHT - TASKBAR_BTN_SIZE) / 2;
    *logo_x = *btn_x + TASKBAR_BTN_SIZE + TASKBAR_ICON_SPACE;
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
    int32_t btn_x, btn_y, logo_x_default;
    default_bar_buttons_rect(&btn_x, &btn_y, &logo_x_default);   // #uiscale: shared with taskbar_handle_default()

    // (#745) No plate: ink direct on the bar, same as every other tray icon.
    uint32_t chip_ink, chip_bg;
    chrome_chip(btn_x, btn_y, TASKBAR_BTN_SIZE, g_start_menu_open, &chip_ink, &chip_bg);
    (void)chip_bg;   // Start uses a solid ICON, no bitmap alpha edge to knock out against

    // Icon is drawn inset inside the button bounds, both scaled with the
    // button so a 200% Start button does not carry a fixed-size 1x icon
    // rattling around inside a doubled box (#uiscale: this was a bare `24`
    // and a bare `+2`, the sibling of the AI-button bug just below).
    {
        int32_t margin = ui_px(2);
        int32_t isz = TASKBAR_BTN_SIZE - 2 * margin;
        icon_draw_scaled(ICON_CATEGORIES, btn_x + margin, btn_y + margin, isz, chip_ink);
    }

    // c2) AI command-launcher (Spotlight-style) button, immediately to the
    // right of Start. Opens the centered AI prompt overlay (launcher.c).
    int32_t logo_x = logo_x_default;
    int32_t logo_y = btn_y;
    uint32_t logo_ink, logo_bg;
    chrome_chip(logo_x, logo_y, TASKBAR_BTN_SIZE, g_launcher_open, &logo_ink, &logo_bg);
    (void)logo_bg;   // ai_launcher_icon_draw_boxed blends by real alpha; no bg needed
    ai_launcher_icon_draw_boxed(logo_x, logo_y, TASKBAR_BTN_SIZE, logo_ink);

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

        // #231: pinned Start Menu/dock favourites that are NOT currently
        // running get their own tile too, drawn BEFORE the running windows
        // (the same pinned-then-running order DOCK_XFCE's Marble dock already
        // uses). Previously this style - the DEFAULT bottom taskbar, the one
        // most users see first - ignored favourites entirely: pinning an app
        // from the dock/Start Menu changed nothing here. A favourite that IS
        // already running is not drawn twice; dock_fav_is_running() is the
        // same #41 identity match the Marble dock uses to avoid that.
        sm_fav_info_t favs[XFCE_MAX_FAVS];
        int nfav = startmenu_get_favorites(favs, XFCE_MAX_FAVS);
        int fav_count = 0;
        for (int i = 0; i < nfav; i++) if (!dock_fav_is_running(&favs[i])) fav_count++;

        int total_count = app_count + fav_count;

        // Start with the logo-launcher button occupying one more slot after Start.
        int32_t strip_x = logo_x + TASKBAR_BTN_SIZE + TASKBAR_ICON_SPACE;
        int32_t tray_reserve = TRAY_N * TRAY_ICON_W + (TRAY_N - 1) * TRAY_ICON_GAP + 14;
    int32_t strip_end = gauge_start_x - TASKBAR_ICON_SPACE - tray_reserve;
        int32_t strip_w = strip_end - strip_x;

        g_tb_btn_n = 0;
        if (total_count > 0 && strip_w > 40) {
            // Fit buttons in the strip: preferred width TB_BTN_W, shrink if needed.
            int32_t bw = TB_BTN_W;
            int32_t need = total_count * (bw + TB_BTN_GAP);
            if (need > strip_w) bw = (strip_w / total_count) - TB_BTN_GAP;
            if (bw < TB_ICON_SZ + ui_px(8)) bw = TB_ICON_SZ + ui_px(8);   // at least fit the icon
            g_tb_btn_w = bw;

            int32_t by = g_taskbar_y + 6;
            int32_t bh = TASKBAR_HEIGHT - 12;               // compact height
            int32_t bx = strip_x;
            g_tb_btn_y = by;
            g_tb_btn_h = bh;

            // Pinned-but-not-running favourites first. Drawn a shade dimmer
            // than a running tile (CLR_TASKBAR_BG, the same "not active" fill
            // a minimized window tile uses below) so a glance still tells
            // "pinned" from "running", without a bespoke third visual style.
            for (int i = 0; i < nfav; i++) {
                if (dock_fav_is_running(&favs[i])) continue;
                if (bx + bw > strip_end) break;

                draw_fill_rect(bx, by, bw, bh, CLR_TASKBAR_BG);
                draw_rect_outline(bx, by, bw, bh, CLR_GAUGE_BORDER);

                int32_t icon_y = by + (bh - TB_ICON_SZ) / 2;
                icon_draw_scaled(favs[i].icon_id, bx + 3, icon_y, TB_ICON_SZ, CLR_CHROME_TEXT);

                int32_t text_x = bx + 3 + TB_ICON_SZ + 4;
                int32_t avail  = (bx + bw) - text_x - 3;
                if (avail > 6) {
                    char label[48];
                    int  ln = 0;
                    while (favs[i].name[ln] && ln < 40) { label[ln] = favs[i].name[ln]; ln++; }
                    label[ln] = '\0';
                    if (text_width(label) > avail) {
                        int ew = text_width("...");
                        while (ln > 0 && text_width(label) + ew > avail)
                            label[--ln] = '\0';
                        label[ln] = '.'; label[ln + 1] = '.';
                        label[ln + 2] = '.'; label[ln + 3] = '\0';
                    }
                    draw_text(text_x, by + (bh - 8) / 2, label, CLR_CHROME_TEXT);
                }

                if (g_tb_btn_n < TB_BTN_MAX) {
                    g_tb_btn_x[g_tb_btn_n]       = bx;
                    g_tb_btn_id[g_tb_btn_n]      = -1;   // sentinel: not a window
                    g_tb_btn_focused[g_tb_btn_n] = 0;
                    g_tb_btn_fav_idx[g_tb_btn_n] = i;
                    g_tb_btn_n++;
                }
                bx += bw + TB_BTN_GAP;
            }

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
                icon_draw_scaled(tb_icon_for_window(&g_tb_wins[i]),
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
                if (g_tb_btn_n < TB_BTN_MAX) {
                    g_tb_btn_x[g_tb_btn_n]       = bx;
                    g_tb_btn_id[g_tb_btn_n]      = g_tb_wins[i].id;
                    g_tb_btn_focused[g_tb_btn_n] = is_focused;
                    g_tb_btn_fav_idx[g_tb_btn_n] = -1;
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
    // #uiscale hit-test fix: shared with the draw side (default_bar_buttons_
    // rect() below) instead of a second literal copy of the same formula -
    // both used the same already-scaled macros so this could not drift in
    // VALUE, but it is exactly the duplicated-formula shape the rest of this
    // audit fixes on sight.
    int32_t btn_x, btn_y, logo_x;
    default_bar_buttons_rect(&btn_x, &btn_y, &logo_x);

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
        // Click on an open-window button: focus/raise that window. Or, if
        // this slot is a pinned-but-not-running favourite (#231, g_tb_btn_id
        // sentinel -1), launch it by path instead - it has no window yet.
        for (int i = 0; i < g_tb_btn_n; i++) {
            if (x >= g_tb_btn_x[i] && x < g_tb_btn_x[i] + g_tb_btn_w) {
                if (g_tb_btn_fav_idx[i] >= 0) {
                    sm_fav_info_t favs[XFCE_MAX_FAVS];
                    int nfav = startmenu_get_favorites(favs, XFCE_MAX_FAVS);
                    if (g_tb_btn_fav_idx[i] < nfav)
                        startmenu_launch_path(favs[g_tb_btn_fav_idx[i]].exec_path,
                                              favs[g_tb_btn_fav_idx[i]].launch_type);
                } else {
                    // Windows-style toggle: clicking the already-focused app's
                    // button minimizes it; otherwise focus/raise (and restore).
                    if (g_tb_btn_focused[i]) wm_minimize(g_tb_btn_id[i]);
                    else                     wm_focus(g_tb_btn_id[i]);
                }
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

// (#wizdock) Owner decision 2026-08-27: the compiled-in starting layout is
// Marble (DOCK_XFCE), not DOCK_DEFAULT ("classic", 0). DOCK_DEFAULT itself is
// UNCHANGED - it still names the classic bar, and every "== DOCK_DEFAULT"
// comparison elsewhere in this file still means exactly that. Only the
// INITIAL VALUE of the variable changes, which is what a fresh install with
// no /UIPROFIL.YML yet actually starts from. A machine with an existing
// profile is unaffected: profile_load() overwrites this the moment it reads
// a "dock_style:" key, so this line only governs the pre-personalisation
// default (and the first-boot wizard's Finish step, which persists whatever
// this session's g_dock_style is if the Appearance page was never visited -
// see userland/apps/setup/main.rs's matching default).
int g_dock_style = DOCK_XFCE;
extern int g_draw_blend;   // draw.c global alpha (255 = opaque)

// (Layout metrics LUMINA_*/CLASSIC_UNIX_*/RETRO_BENCH_* are #defined at the top of this file.)

// Shared running-app / launcher slot records (Lumina / Classic UNIX / Retro Bench hit-testing).
// id == -1 marks the launcher/start slot; otherwise it is a window id.
// (#231) Sized like DOCK_XFCE's own XDOCK_MAX (see below): the worst case is
// every favourite pinned AND every one of them also running elsewhere, plus a
// full window list, so this must not be a number that merely "looks big
// enough" either - the same rule XDOCK_MAX's own comment states.
#define LAY_MAX (XFCE_MAX_FAVS + TB_MAX_WINS)
static int32_t g_lay_x[LAY_MAX], g_lay_w[LAY_MAX], g_lay_id[LAY_MAX];
static int     g_lay_focused[LAY_MAX];
// (#231) >= 0 marks a PINNED-BUT-NOT-RUNNING favourite tile recorded via
// lay_add_fav() (id/focused are unused for these slots); -1 (LAY_NOT_FAV,
// what lay_add() always sets) is every pre-existing slot kind - launcher or
// running window - completely unchanged.
#define LAY_NOT_FAV (-1)
static int     g_lay_fav_idx[LAY_MAX];
static int     g_lay_n;
static int32_t g_lay_y, g_lay_h;
static void lay_reset(int32_t y, int32_t h) { g_lay_n = 0; g_lay_y = y; g_lay_h = h; }
static void lay_add(int32_t x, int32_t w, int32_t id, int focused) {
    if (g_lay_n >= LAY_MAX) return;
    g_lay_x[g_lay_n] = x; g_lay_w[g_lay_n] = w;
    g_lay_id[g_lay_n] = id; g_lay_focused[g_lay_n] = focused;
    g_lay_fav_idx[g_lay_n] = LAY_NOT_FAV;
    g_lay_n++;
}
// (#231) Record a pinned-but-not-running favourite tile: same slot list as
// lay_add(), so it shares hit-testing/y-band logic, but a click launches
// favs[fav_idx] by path instead of focusing/minimizing a window.
static void lay_add_fav(int32_t x, int32_t w, int fav_idx) {
    if (g_lay_n >= LAY_MAX) return;
    g_lay_x[g_lay_n] = x; g_lay_w[g_lay_n] = w;
    g_lay_id[g_lay_n] = -1; g_lay_focused[g_lay_n] = 0;
    g_lay_fav_idx[g_lay_n] = fav_idx;
    g_lay_n++;
}
static bool lay_click(int32_t x, int32_t y) {
    if (y < g_lay_y || y >= g_lay_y + g_lay_h) return false;
    for (int i = 0; i < g_lay_n; i++) {
        if (x >= g_lay_x[i] && x < g_lay_x[i] + g_lay_w[i]) {
            if (g_lay_fav_idx[i] >= 0) {
                // #231: pinned favourite, not currently running - launch it.
                sm_fav_info_t favs[XFCE_MAX_FAVS];
                int nfav = startmenu_get_favorites(favs, XFCE_MAX_FAVS);
                if (g_lay_fav_idx[i] < nfav)
                    startmenu_launch_path(favs[g_lay_fav_idx[i]].exec_path,
                                          favs[g_lay_fav_idx[i]].launch_type);
                g_needs_redraw = true;
                return true;
            }
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
// (#123 item 1) Was a bare literal 30, which happened to be two more than the
// new 28-favourite cap and would have started dropping slots the moment one
// unpinned app was also running. Derived now, so it can never again be a
// number that merely "looks big enough": the dock draws at most every pinned
// favourite PLUS every running-but-unpinned window, and those two counts have
// their own caps right here.
#define XDOCK_MAX (XFCE_MAX_FAVS + TB_MAX_WINS)
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
// (#123) The slot hit WIDTH for the frame that is currently on screen. Was the
// literal (XFCE_DOCK_ICON + XFCE_DOCK_PAD) recomputed independently in the
// left-click handler AND the right-click handler; with a configurable height
// and a width-fit pass the tile is no longer a compile-time constant, so both
// handlers now read the ONE value the renderer actually laid out with. This is
// the same "one geometry, not two arithmetic chains" rule the Settings Dock
// panel already follows (dock_panel_layout()).
static int32_t g_xdock_slot_w;

static void xdock_reset(int32_t y, int32_t h, int32_t slot_w) {
    g_xdock_n = 0; g_xdock_y = y; g_xdock_h = h; g_xdock_slot_w = slot_w;
}

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
//
// #230: honors settingscfg_use24h() (SETTINGS.CFG 'h' key, docs/SETTINGS_
// CONTROL_AUDIT.md #224 - "Use 24-hour format" persisted a preference every
// clock in the OS ignored, including this one, which was hardcoded 24h no
// matter what the toggle said). `out` must hold >= 10 bytes ("HH:MM AM\0" is
// 9); every call site below was widened from char[6] to char[10] to match.
static void tb_clock_str(char *out) {
    int h = 0, m = 0, s = 0;
    tz_local_hms(&h, &m, &s);
    int use24 = settingscfg_use24h();
    const char *ampm = "";
    if (!use24) {
        ampm = (h >= 12) ? "PM" : "AM";
        h = h % 12; if (h == 0) h = 12;
    }
    out[0] = (char)('0' + (h / 10) % 10); out[1] = (char)('0' + h % 10);
    out[2] = ':';
    out[3] = (char)('0' + (m / 10) % 10); out[4] = (char)('0' + m % 10);
    int i = 5;
    if (ampm[0]) { out[i++] = ' '; out[i++] = ampm[0]; out[i++] = ampm[1]; }
    out[i] = '\0';
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
    char clk[10]; tb_clock_str(clk);   // #230: room for "HH:MM AM"
    int clkw = text_width_ttf(clk, 13);
    int clk_x = W - 12 - clkw;
    draw_text_ttf(clk_x, 6, clk, 13, ink);
    bar_clock_set_rect(clk_x - 4, 0, clkw + 8, mb);   // #129: clock -> Date and Time
    tray_render_core(clk_x - 8 - tray_total_w(), (mb - 26) / 2, glass, 1);

    // ---- Bottom floating dock (glass pill, magnifying icons) ----
    // #231: pinned favourites that are not currently running get a slot too,
    // drawn before the running apps (see taskbar_render_default()'s own
    // comment on the same feature for the full rationale; dock_fav_is_running()
    // is the shared #41 identity match that keeps an already-running favourite
    // from getting a second, duplicate slot here).
    sm_fav_info_t favs[XFCE_MAX_FAVS];
    int nfav = startmenu_get_favorites(favs, XFCE_MAX_FAVS);
    int items = 1;   // launcher + favourites + running apps
    for (int i = 0; i < nfav; i++) if (!dock_fav_is_running(&favs[i])) items++;
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
    // (#123) The non-glass branch was a hardcoded 175/255 (69%) that no theme
    // and no user could reach - the same class of defect as the dock's own
    // opacity being inert on a flat theme. Both branches read the user's
    // preference now; the flat one through the derived floor.
    g_draw_blend = g_glass_enable ? (g_dock_opacity * 255 / 100) : flat_chrome_alpha();
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
    // Pinned-but-not-running favourites, drawn dimmer than a running icon
    // (same 0xFF8A8A90 a minimized window's icon already uses below) so the
    // "not running yet" state reads the same way it does for a minimized app.
    for (int i = 0; i < nfav && slot < items; i++) {
        if (dock_fav_is_running(&favs[i])) continue;
        int cx = LUMINA_SLOT_CX(slot);
        int sz = icon, dist = g_mouse_x - cx; if (dist < 0) dist = -dist;
        if (mouse_in && dist < 70) sz = icon + (70 - dist) * 20 / 70;
        icon_draw_scaled(favs[i].icon_id, cx - sz / 2, base_y - sz, sz, 0xFF8A8A90);
        lay_add_fav(dx + pad + slot * step, icon, i);
        slot++;
    }
    for (int i = 0; i < n && slot < items; i++) {
        if (!tb_window_is_app(&g_tb_wins[i])) continue;
        int cx = LUMINA_SLOT_CX(slot);
        int sz = icon, dist = g_mouse_x - cx; if (dist < 0) dist = -dist;
        if (mouse_in && dist < 70) sz = icon + (70 - dist) * 20 / 70;
        int focused = g_tb_wins[i].focused && !g_tb_wins[i].minimized;
        icon_draw_scaled(tb_icon_for_window(&g_tb_wins[i]),
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
            if (bar_clock_click(x, y)) return true;   // #129
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

// #uiscale BUGFIX (ui-ux screendump): this whole function was a SECOND,
// entirely independent copy of the "preferred tile width, shrink to fit"
// scheme taskbar_render_default() already had (TB_BTN_W et al, correctly
// scaled), but with every one of ITS OWN literals - lx/lw, right_w's
// gutters, ws_w/ws_h, the preferred tw=128, the shrink/floor math, every
// icon size, gap and text-offset - left as raw 1x numbers. The panel's own
// OUTER box (CLASSIC_UNIX_PANEL_H) was already scaled (see compositor.h),
// so the surface grew while everything drawn inside it, and the tile-width
// budget dividing it up, did not: text (scaled via the draw_text_ttf()/
// text_width_ttf() chokepoint) then overflowed a box sized for 1x. Every
// literal below is now ui_px()'d so the box and its contents scale together
// - the "if a box is scaled, keep its text scaled" rule this bug is about.
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
    int cy = py + ui_px(6), ch = CLASSIC_UNIX_PANEL_H - ui_px(12);   // tile row
    lay_reset(cy, ch);

    // Launcher tile (start).
    int lx = ui_px(6), lw = ui_px(46);
    int licon = ui_px(28);
    bevel(lx, cy, lw, ch, g_start_menu_open ? 0 : 1, lt, dk);
    draw_fill_rect(lx + 1, cy + 1, lw - 2, ch - 2, face);
    icon_draw_scaled(ICON_CATEGORIES, lx + (lw - licon) / 2, cy + (ch - licon) / 2, licon, ink);
    lay_add(lx, lw, -1, 0);
    int bx = lx + lw + ui_px(6);

    // Right cluster width (tray + clock), reserved so app tiles stop short.
    char clk[10]; tb_clock_str(clk);   // #230: room for "HH:MM AM"
    int right_w = tray_total_w() + ui_px(12) + text_width(clk) + ui_px(12);
    // Workspace switcher block (centered-ish, fixed).
    int ws_w = 4 * ui_px(22) + ui_px(10), ws_h = ch;
    int ws_x = (W - ws_w) / 2;

    // #231: pinned favourites not currently running - see taskbar_render_
    // default()'s comment on the same feature for the full rationale.
    sm_fav_info_t favs[XFCE_MAX_FAVS];
    int nfav = startmenu_get_favorites(favs, XFCE_MAX_FAVS);
    int nfav_show = 0;
    for (int i = 0; i < nfav; i++) if (!dock_fav_is_running(&favs[i])) nfav_show++;

    // App tiles fill from bx to just before the workspace block.
    int apps_end = ws_x - ui_px(8);
    int napps = 0; for (int i = 0; i < n; i++) if (tb_window_is_app(&g_tb_wins[i])) napps++;
    int total_tiles = napps + nfav_show;
    int tile_icon = ui_px(20), tile_gap = ui_px(4), tile_floor = ui_px(40);
    if (total_tiles > 0 && apps_end - bx > tile_floor) {
        int tw = ui_px(128); int need = total_tiles * (tw + tile_gap);
        if (need > apps_end - bx) tw = (apps_end - bx) / total_tiles - tile_gap;
        if (tw < tile_floor) tw = tile_floor;
        int text_x_off = ui_px(28), avail_off = ui_px(30);
        // Pinned-but-not-running favourites first, in the same "not active"
        // ink (dk) a minimized window's tile already uses below.
        for (int i = 0; i < nfav; i++) {
            if (dock_fav_is_running(&favs[i])) continue;
            if (bx + tw > apps_end) break;
            bevel(bx, cy, tw, ch, 1, lt, dk);
            draw_fill_rect(bx + 1, cy + 1, tw - 2, ch - 2, face);
            icon_draw_scaled(favs[i].icon_id, bx + ui_px(4), cy + (ch - tile_icon) / 2, tile_icon, dk);
            char lbl[48]; int k = 0;
            while (favs[i].name[k] && k < 40) { lbl[k] = favs[i].name[k]; k++; }
            lbl[k] = '\0';
            int avail = tw - avail_off;
            while (k > 0 && text_width(lbl) > avail) lbl[--k] = '\0';
            draw_text(bx + text_x_off, cy + (ch - 8) / 2, lbl, dk);
            lay_add_fav(bx, tw, i);
            bx += tw + tile_gap;
        }
        for (int i = 0; i < n; i++) {
            if (!tb_window_is_app(&g_tb_wins[i])) continue;
            if (bx + tw > apps_end) break;
            int focused = g_tb_wins[i].focused && !g_tb_wins[i].minimized;
            bevel(bx, cy, tw, ch, focused ? 0 : 1, lt, dk);
            draw_fill_rect(bx + 1, cy + 1, tw - 2, ch - 2, focused ? 0xFFC8C8BC : face);
            icon_draw_scaled(tb_icon_for_window(&g_tb_wins[i]),
                             bx + ui_px(4), cy + (ch - tile_icon) / 2, tile_icon,
                             g_tb_wins[i].minimized ? dk : ink);
            // Ellipsized label.
            char lbl[48]; int k = 0;
            while (g_tb_wins[i].title[k] && k < 40) { lbl[k] = g_tb_wins[i].title[k]; k++; }
            lbl[k] = '\0';
            int avail = tw - avail_off;
            while (k > 0 && text_width(lbl) > avail) lbl[--k] = '\0';
            draw_text(bx + text_x_off, cy + (ch - 8) / 2, lbl, g_tb_wins[i].minimized ? dk : ink);
            lay_add(bx, tw, g_tb_wins[i].id, focused);
            bx += tw + tile_gap;
        }
    }

    // Workspace switcher: recessed box with 4 buttons (One..Four).
    bevel(ws_x, cy, ws_w, ws_h, 0, lt, dk);
    draw_fill_rect(ws_x + 1, cy + 1, ws_w - 2, ws_h - 2, 0xFF9A9A8E);
    g_cu_ws_y = cy + ui_px(3); g_cu_ws_h = ws_h - ui_px(6); g_cu_ws_w = ui_px(20);
    static const char *wsl[4] = { "1", "2", "3", "4" };
    for (int i = 0; i < 4; i++) {
        int wx = ws_x + ui_px(5) + i * ui_px(22);
        g_cu_ws_x[i] = wx;
        int on = (i == g_cu_ws);
        bevel(wx, g_cu_ws_y, g_cu_ws_w, g_cu_ws_h, on ? 0 : 1, lt, dk);
        draw_fill_rect(wx + 1, g_cu_ws_y + 1, g_cu_ws_w - 2, g_cu_ws_h - 2, on ? 0xFF3C6EB4 : face);
        draw_text(wx + ui_px(6), g_cu_ws_y + (g_cu_ws_h - 8) / 2, wsl[i], on ? 0xFFFFFFFF : ink);
    }

    // Right: tray + clock in a recessed strip.
    int rx = W - right_w;
    tray_render_core(rx + ui_px(4), cy + (ch - ui_px(26)) / 2, face, 0);
    int clk_tx = W - text_width(clk) - ui_px(10);
    draw_text(clk_tx, cy + (ch - 8) / 2, clk, ink);
    bar_clock_set_rect(clk_tx - ui_px(4), cy, text_width(clk) + ui_px(8), ch);   // #129: clock -> Date and Time
    (void)ws_h;
}

static bool taskbar_handle_classic_unix(int32_t x, int32_t y, bool clicked) {
    int py = g_fb_height - CLASSIC_UNIX_PANEL_H;
    if (y < py) return false;
    if (clicked) {
        if (bar_clock_click(x, y)) return true;   // #129
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
    char clk[10]; tb_clock_str(clk);   // #230: room for "HH:MM AM"
    int clk_x = g_rb_gadg_x - 8 - text_width(clk);
    draw_text(clk_x, (bh - 8) / 2, clk, RB_WHITE);
    bar_clock_set_rect(clk_x - 4, 0, text_width(clk) + 8, bh);   // #129: clock -> Date and Time
    int tray_right = clk_x - 8;
    tray_render_core(tray_right - tray_total_w(), (bh - 26) / 2, RB_BLUE, 1);

    // Running apps: text buttons after the title, focused = orange field.
    lay_reset(0, bh);
    int bx = 6 + text_width("Retro Bench Screen") + 16;
    int apps_end = g_tray_x[0] - 10;   // stop before the tray

    // #231: pinned-but-not-running favourites, as the same plain text
    // buttons (dimmed, like a minimized window's label) - see
    // taskbar_render_default()'s comment on the same feature for the full
    // rationale. Drawn before the running-app buttons.
    sm_fav_info_t favs[XFCE_MAX_FAVS];
    int nfav = startmenu_get_favorites(favs, XFCE_MAX_FAVS);
    for (int i = 0; i < nfav; i++) {
        if (dock_fav_is_running(&favs[i])) continue;
        char lbl[40]; int k = 0;
        while (favs[i].name[k] && k < 32) { lbl[k] = favs[i].name[k]; k++; }
        lbl[k] = '\0';
        int tw = text_width(lbl) + 12;
        if (bx + tw > apps_end) break;
        draw_text(bx + 6, (bh - 8) / 2, lbl, 0xFFAAC4E4);
        lay_add_fav(bx, tw, i);
        bx += tw + 4;
    }
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
        if (bar_clock_click(x, y)) return true;   // #129
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
// #uiscale hit-test fix: the start/logo chip geometry used to be written out
// twice - once here for drawing, once again (with the SAME literals, by luck)
// in taskbar_handle_xfce() for hit-testing, and the hit-test copy never
// tested `y` against the button's actual (vertically centered) band at all,
// so the whole XFCE_PANEL_H-tall band above/below the chip also registered as
// a hit. ONE function now, called from both, so draw and hit-test can never
// drift and the hit-test gets a real Y range for free.
static void xfce_panel_buttons_rect(int32_t *bs, int32_t *by, int32_t *start_x, int32_t *logo_x) {
    *bs = XFCE_BTN_SIZE;
    *by = (XFCE_PANEL_H - *bs) / 2;
    // #uiscale: these two were bare `4` literals - the panel's own metrics
    // (XFCE_PANEL_H/XFCE_BTN_SIZE) scale, so the gap either side of Start
    // shrank, proportionally, as the panel grew.
    *start_x = ui_px(4);
    *logo_x = *start_x + *bs + ui_px(4);
}

static void taskbar_render_xfce_panel(void) {
    int W = g_fb_width;
    // (#745) Glass. Bleed on the bottom side only (top/left/right are screen
    // edges). No inner highlight: the panel's top edge IS the screen edge, so
    // there is no surface for a highlight to imply.
    glass_or_flat(0, 0, W, XFCE_PANEL_H, GLASS_SURF_PANEL);
    glass_edge_h(0, XFCE_PANEL_H - 1, W, CLR_TASKBAR_BORDER);

    int32_t bs, by, start_x, logo_x_unused;
    xfce_panel_buttons_rect(&bs, &by, &start_x, &logo_x_unused);
    uint32_t start_ink, start_bg;
    chrome_chip(start_x, by, bs, g_start_menu_open, &start_ink, &start_bg);
    (void)start_bg;
    // #uiscale: was a bare `+2`/`bs-4` - byte-identical at 1x (bs=24 ->
    // margin 2, isz 20... see below), scales with the button above it.
    {
        int32_t margin = ui_px(2);
        int32_t isz = bs - 2 * margin;
        icon_draw_scaled(ICON_CATEGORIES, start_x + margin, by + margin, isz, start_ink);
    }

    int32_t logo_x = logo_x_unused;
    uint32_t logo_ink, logo_bg;
    chrome_chip(logo_x, by, bs, g_launcher_open, &logo_ink, &logo_bg);
    (void)logo_bg;
    ai_launcher_icon_draw_boxed(logo_x, by, bs, logo_ink);

    // Right cluster, computed right-to-left: clock, then tray, then gauges
    // (mirrors the Retro Bench/Lumina clock-rightmost convention).
    char clk[10]; tb_clock_str(clk);   // #230: room for "HH:MM AM"
    int32_t clock_w = text_width(clk);
    int32_t clock_x = W - TASKBAR_PADDING - clock_w;
    int32_t tray_right = clock_x - 8;
    int32_t tray_x0 = tray_right - tray_total_w();
    draw_vline(tray_x0 - 6, 6, XFCE_PANEL_H - 12, CLR_GAUGE_BORDER);
    // is_top=1 (not the tray_render() wrapper, which hardcodes 0) so tray
    // popups AND the perf popup (see draw_perf_popup) open downward.
    tray_render_core(tray_x0, (XFCE_PANEL_H - 26) / 2, chrome_surface_bg(), 1);  // #745: glass-aware ink contrast, see tray_render()
    draw_text(clock_x, 7, clk, CLR_CHROME_TEXT);
    bar_clock_set_rect(clock_x - 4, 0, clock_w + 8, XFCE_PANEL_H);   // #129: clock -> Date and Time

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
// (#123 item 3) XFCE_HOVER_GROW (a fixed 10 extra px) is gone: the grow is now
// derived from the user's zoom percent and the LIVE icon size, so the same
// percentage reads the same at every dock height. The lift keeps its old 3:5
// ratio to the grow (was 6:10), so the gesture's shape is unchanged and only
// its magnitude is user-controlled.
#define XFCE_HOVER_LIFT_NUM 3
#define XFCE_HOVER_LIFT_DEN 5
static int32_t  g_xfce_hover_amt[XFCE_HOVER_SLOTS];   // 0..XFCE_HOVER_MS per slot
static uint64_t g_xfce_hover_last_ms = 0;
static int      g_xfce_anim_active = 0;

// (#745) DOCK_XFCE only: is any dock slot still easing toward or away from
// its hover target this frame? Read by main.c to keep forcing the full
// render path for exactly as long as the animation needs and no longer.
// (#123) The tile/gutter/height this frame's dock was actually laid out with.
// Set ONCE per frame by taskbar_render_xfce_dock() before it draws anything,
// and read by xfce_draw_slot() below. They are not simply xfce_tile_for_h()/
// XFCE_DOCK_PAD because the auto-scale ladder in
// taskbar_render_xfce_dock() may have had to shrink them so a long pin list
// still fits the 75% width budget.
static int32_t g_xd_tile = XFCE_DOCK_ICON;
static int32_t g_xd_pad  = XFCE_DOCK_PAD;
// The EFFECTIVE dock height (the user's preference after auto-scale). Seeded
// with the default so the work-area/collision consumers below have a sane
// answer on the very first frame, BEFORE the dock has painted once.
static int32_t g_xd_h    = XFCE_DOCK_H_DEFAULT;
// Set when an auto-scale step changed the effective height; consumed by
// taskbar_geom_settle() (below) on main.c's poll cadence.
static int     g_xd_h_dirty = 0;

int taskbar_dock_animating(void) {
    return (g_dock_style == DOCK_XFCE) ? g_xfce_anim_active : 0;
}

// (#123 auto-scale) Apply a pending work-area change caused by the marble
// dock auto-scaling its height. Called from main.c's existing 10-tick poll
// block (next to dock_geom_poll), NOT from the renderer: taskbar_apply_work_
// area() re-clamps desktop icons and widgets, and the renderer runs after
// those have already been painted for this frame. Returns 1 if it did work,
// so a caller (or a test) can tell "settled" from "nothing was pending".
int taskbar_geom_settle(void) {
    if (!g_xd_h_dirty) return 0;
    g_xd_h_dirty = 0;
    taskbar_apply_work_area();
    g_needs_redraw = true;
    return 1;
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
    int32_t tile = g_xd_tile, pad = g_xd_pad, dh = g_xd_h;
    int32_t lead = pad / 2;   // the slot's hit box starts half a gutter left of the tile
    int mx = g_mouse_x, my = g_mouse_y;
    int hover = (mx >= cx - lead && mx < cx + tile + lead &&
                 my >= dock_y + 2 && my < dock_y + dh - 2);
    if (hover)
        draw_fill_rect(cx - lead, dock_y + 2, tile + pad,
                       dh - 4, CLR_TASKBAR_HOVER);

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
    // (#63/#745) Base size is 90% of the TILE, not the tile itself - see the
    // 90%-rule comment on XFCE_DOCK_ICON at the top of this file. Derived from
    // the frame's LIVE tile (which the width-fit pass may have shrunk below
    // xfce_tile_for_h(the preference)), so the graphic can never be sized for a tile the
    // slot is not actually drawing at.
    int32_t base = tile * 9 / 10;
    // (#123 item 3) Full-hover grow in px = base * (zoom% - 100) / 100, so the
    // slider means what it says: 150% draws the hovered icon half again as
    // large as its neighbours, at every dock height. `amt` then eases that
    // full value in/out over XFCE_HOVER_MS exactly as before.
    int32_t full_grow = base * (xfce_dock_zoom() - 100) / 100;
    int32_t grow = (int32_t)((int64_t)full_grow * amt / XFCE_HOVER_MS);
    int32_t lift = grow * XFCE_HOVER_LIFT_NUM / XFCE_HOVER_LIFT_DEN;
    int32_t isize  = base + grow;
    int32_t bottom = dock_y + XFCE_DOCK_PAD + tile;             // fixed baseline
    int32_t iy = bottom - isize - lift;
    int32_t ix = cx + (tile - isize) / 2;                       // centred in the tile
    // (#123 auto-scale) A magnified icon deliberately grows OUT of the pane -
    // that lift-out-of-the-dock is the whole magnify idiom, and it is why the
    // 75% width budget is measured on the RESTING pane width, not the
    // magnified one: the pane's width does not change on hover (it never has),
    // only one icon's does, transiently. What must not happen is that icon
    // running off the FRAMEBUFFER. At the default height the resting pane is
    // centred with 12.5% of the screen free on each side, far more than any
    // magnify overflow, but in the emergency case where the pane is clamped to
    // W-24 there are only 12px of margin. So the drawn rect is clamped to the
    // screen here rather than relying on that margin existing.
    if (ix < 0) ix = 0;
    if (ix + isize > g_fb_width) ix = g_fb_width - isize;
    if (iy < 0) iy = 0;
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
        // (#123) The bar was 24px wide starting 8px into a 40px tile, and the
        // unfocused one 16px starting at 12: both are "60%/40% of the tile,
        // centred". Expressed that way they stay centred and proportionate at
        // every configurable height instead of drifting off-centre (at a 25px
        // tile the old literals would have run 8px past the tile's right edge).
        if (focused)
            draw_fill_rect(cx + (tile - tile * 3 / 5) / 2,
                           dock_y + XFCE_DOCK_PAD + tile + 2, tile * 3 / 5, 4,
                           readable_ink(CLR_TASKBAR_BG));
        else
            draw_fill_rect(cx + (tile - tile * 2 / 5) / 2,
                           dock_y + XFCE_DOCK_PAD + tile + 3, tile * 2 / 5, 3,
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
//
// #104 FIX (2026-08-13): the right corner was cutting a NOTCH inboard of
// itself instead of rounding the true outer corner away. Root cause: `xx`
// (0..r-1) meant two DIFFERENT things depending on which side read it. For
// the left corner, `pxl = dock_x + xx` puts xx=0 AT the true corner column
// and xx=r-1 deepest into the body - "xx is distance from the corner",
// which is exactly what the coverage formula below assumes (`fdx = r - xx`,
// large distance i.e. maximum cut at xx=0). For the right corner, the OLD
// `pxr = dock_x + dock_w - r + xx` put xx=0 at the column NEAREST the body
// (minimum cut, by rights) and xx=r-1 AT the true corner (maximum cut, by
// rights) - the opposite convention - while still being fed through the
// SAME "xx is distance from the corner" formula. So the coverage computed
// for xx=0 (which should barely be touched) was applied as if it were the
// corner pixel, and the pixel that WAS the true corner (xx=r-1, small
// computed distance) was barely restored. A rounded-away outer corner and
// an inboard notch are the same bug seen from either side.
//
// The fix makes `xx` mean "distance from THIS corner" on BOTH sides, the
// same convention kernel/gui/window_decor.c's decor_fill_rounded_rect_aa()
// uses (there, one `radius - dx` distance drives FOUR mirrored blends, one
// per corner, from a single loop). Here the loop only ever needs two
// corners (top-only, see comment above), so the mirroring lives in exactly
// ONE place - the pixel-position expression `dock_x + dock_w - 1 - xx` -
// used identically in the capture pass and the restore pass, and the
// coverage itself is computed ONCE per (xx, yy) and reused for both L and R
// (xfce_corner_coverage()), so there is no second copy of the distance
// formula left to drift out of sync with the first. A future bottom-edge
// corner (e.g. under #102 rotation, where "flush with the screen edge"
// stops being a given) mirrors the same way on the y axis:
// `dock_y + dock_h - 1 - yy`, through the same helper.
#define XFCE_DOCK_CORNER_R 6   // "VERY VERY slight" - err small, per the brief

// Coverage (0.0 = fully outside the rounded silhouette, 1.0 = fully inside)
// for a pixel `dist_x`/`dist_y` pixels in from the two straight edges that
// meet at THIS corner - i.e. already corner-relative: 0 is the corner pixel
// itself, r-1 is the pixel deepest into the straight body. Callers convert
// their own absolute pixel position into this corner-relative distance
// BEFORE calling in (see #104 fix note above); this function never sees
// which side of the dock it is being asked about, so it cannot itself
// develop a handedness bug. Same algorithm as
// kernel/gui/window_decor.c's decor_corner_coverage()
// (`cov = clamp(0.5 - (dist - r), 0, 1)`), kept as a float here since this
// file already links sqrtf (see #include at the top) and the rest of this
// wrapper works in floats.
static float xfce_corner_coverage(int32_t dist_x, int32_t dist_y, int32_t r)
{
    float fdx = (float)(r - dist_x);
    float fdy = (float)(r - dist_y);
    float dist = sqrtf(fdx * fdx + fdy * fdy);
    float cov = 0.5f - (dist - (float)r);
    if (cov < 0.0f) cov = 0.0f;
    if (cov > 1.0f) cov = 1.0f;
    return cov;
}

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
                // xx is distance-from-corner on BOTH sides (see #104 note
                // above): pxl counts up from the left edge, pxr counts DOWN
                // from the right edge, so xx=0 is the true corner column on
                // both, and xx=r-1 is the innermost corner-band column on
                // both.
                int32_t pxl = dock_x + xx, pxr = dock_x + dock_w - 1 - xx;
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
    for (int32_t yy = 0; yy < r; yy++) {
        int32_t py = dock_y + yy;
        if (py < 0 || py >= g_fb_height) continue;
        for (int32_t xx = 0; xx < r; xx++) {
            // ONE coverage value, corner-relative in both xx and yy, reused
            // for both the left and the right pixel this iteration touches
            // - the two sides can no longer disagree about what "distance
            // from the corner" means, because there is only one formula.
            float cov = xfce_corner_coverage(xx, yy, r);   // 1.0 = fully inside the rounded silhouette
            int restore_a = (int)((1.0f - cov) * 255.0f + 0.5f);  // how much background to bring back
            if (restore_a == 0) continue;
            g_draw_blend = restore_a;
            int32_t pxl = dock_x + xx, pxr = dock_x + dock_w - 1 - xx;
            draw_putpixel(pxl, py, capL[yy][xx]);
            draw_putpixel(pxr, py, capR[yy][xx]);
        }
    }
    g_draw_blend = ob;
}

#ifdef MAYTERA_TESTHOOK
// (#123) Geometry telemetry for the throwaway verification build ONLY. There is
// no screenshot that can report "the effective height is 44 because the 75%
// budget forced it down from 59" - that is a number, and a number has to be
// printed. ONE buffered write(1,...) per line (never printf/putchar: MEASURED
// in #41, printf goes one SYS_PUTCHAR per character and each one becomes its
// own syslog entry, which makes the serial capture unreadable). Emitted only
// when something actually changed, plus on demand via the DOCKINFO verb, so it
// cannot flood a 30Hz frame loop. Compiled out of every shipping binary with
// the rest of testhook.c - see that file's header for how to verify that.
static char *td_s(char *o, const char *k) { while (*k) *o++ = *k++; return o; }
static char *td_i(char *o, int v) {
    if (v < 0) { *o++ = '-'; v = -v; }
    char t[12]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *o++ = t[--n];
    return o;
}
static int32_t g_td_last[10];
void taskbar_dock_debug_dump(int force) {
    int32_t cur[10] = { g_fb_width, g_fb_height, g_dock_height, g_xd_h,
                        g_xd_tile, g_xd_pad, g_xfce_dock_w, (int32_t)g_xdock_n,
                        g_dock_zoom, g_dock_opacity };
    if (!force) {
        int same = 1;
        for (int i = 0; i < 10; i++) if (cur[i] != g_td_last[i]) { same = 0; break; }
        if (same) return;
    }
    for (int i = 0; i < 10; i++) g_td_last[i] = cur[i];
    char b[512]; char *o = b;
    o = td_s(o, "[DOCK123] fb=");      o = td_i(o, g_fb_width); *o++='x'; o = td_i(o, g_fb_height);
    o = td_s(o, " style=");            o = td_i(o, g_dock_style);
    o = td_s(o, " pref_h=");           o = td_i(o, g_dock_height);
    o = td_s(o, " eff_h=");            o = td_i(o, g_xd_h);
    o = td_s(o, " tile=");             o = td_i(o, g_xd_tile);
    o = td_s(o, " pad=");              o = td_i(o, g_xd_pad);
    o = td_s(o, " zoom=");             o = td_i(o, g_dock_zoom);
    o = td_s(o, " opac=");             o = td_i(o, g_dock_opacity);
    o = td_s(o, " glass=");            o = td_i(o, g_glass_enable);
    o = td_s(o, " pane_x=");           o = td_i(o, g_xfce_dock_x);
    o = td_s(o, " pane_w=");           o = td_i(o, g_xfce_dock_w);
    o = td_s(o, " budget75=");         o = td_i(o, g_fb_width * XFCE_DOCK_WIDTH_BUDGET_PCT / 100);
    o = td_s(o, " slots=");            o = td_i(o, g_xdock_n);
    o = td_s(o, " slotw=");            o = td_i(o, g_xdock_slot_w);
    o = td_s(o, " band_y=");           o = td_i(o, g_xdock_y);
    o = td_s(o, " band_h=");           o = td_i(o, g_xdock_h);
    if (g_xdock_n > 0) {
        o = td_s(o, " s0=");           o = td_i(o, g_xdock_x[0]);
        o = td_s(o, " slast=");        o = td_i(o, g_xdock_x[g_xdock_n - 1]);
    }
    *o++ = '\n';
    write(1, b, (unsigned long)(o - b));
}
// Drive the REAL hit test at real screen coordinates. Unlike ICON/MENUITEM
// (which skip the hit test entirely, see testhook.c's header), this enters
// taskbar_handle_mouse() exactly where the input path does, so a wrong band or
// a wrong slot width DOES fail here.
// Centre of dock slot `n` in SCREEN coordinates, from the same records the
// hit test consults. Lets a pre-written verification script address "the third
// icon" without knowing the framebuffer width in advance - the geometry is
// recomputed live, so the script does not have to predict it. Returns 0 if
// there is no such slot. The coordinates it hands back are printed, so a
// screenshot can be checked against them independently.
int taskbar_dock_slot_point(int n, int32_t *x, int32_t *y) {
    if (n < 0 || n >= g_xdock_n) return 0;
    if (x) *x = g_xdock_x[n] + g_xdock_slot_w / 2;
    if (y) *y = g_xdock_y + g_xdock_h / 2;
    return 1;
}

int taskbar_dock_debug_click(int32_t x, int32_t y) {
    bool r = taskbar_handle_mouse(x, y, true);
    char b[96]; char *o = b;
    o = td_s(o, "[DOCK123] CLICK ");   o = td_i(o, x); *o++=','; o = td_i(o, y);
    o = td_s(o, " consumed=");         o = td_i(o, r ? 1 : 0);
    *o++ = '\n';
    write(1, b, (unsigned long)(o - b));
    return r ? 1 : 0;
}

// (#glassmodal) verification-only: open the CPU/RAM/DSK/NET perf pop-out by
// gauge index, bypassing the gauge's mouse hit-test - same "drive by name/
// index, sidestep hit-testing" philosophy as taskbar_dock_debug_click() above
// and startmenu.c's testhook exports (#334/#440: QMP mouse cannot reliably
// land a click on a compositor-drawn target). Never shipped: gated identically
// to every other symbol in this block, and only ever called from testhook.c
// (itself excluded from a plain `make`/`make install`).
void taskbar_test_open_perf_popup(int gauge) {
    if (gauge < 0) gauge = 0; if (gauge > 3) gauge = 3;
    g_perf_open = 1;
    g_perf_sel = gauge;
    g_needs_redraw = true;
}

// (#battpop) verification-only: click the tray battery icon by the REAL hit
// test (unlike taskbar_test_open_perf_popup above, this does NOT bypass it) -
// enters taskbar_handle_mouse() at the tray's live-computed battery slot
// center, exactly the path a physical click on the icon takes, so a wrong
// slot index/width fails here the same way DOCKCLICKSLOT would for the dock.
// Returns 0 if the battery icon is not currently in the tray (no battery
// present - g_tray_x[TRAY_BATTERY] is parked off-screen in that case, see
// tray_render_core()'s slot-assignment loop).
int taskbar_test_click_battery_tray(void) {
    if (g_batt_present != 1) return 0;
    int32_t x = g_tray_x[TRAY_BATTERY] + TRAY_ICON_W / 2;
    int32_t y = g_tray_y + g_tray_h / 2;
    return taskbar_handle_mouse(x, y, true) ? 1 : 0;
}

// (#battpop) verification-only: the battery info card's CURRENT finalized
// rect (post-clamp) plus whether it is open at all. Lets a test script print
// the rect on consecutive frames and assert it is identical - the direct
// proof that draw_battery_card() is idempotent (see g_bc_anchor_x's comment
// for the drift bug this replaced). Returns 0 if the card is closed (out
// params left untouched).
int taskbar_test_battery_card_rect(int32_t *x, int32_t *y, int32_t *w, int32_t *h) {
    if (!g_batt_card_open) return 0;
    if (x) *x = g_bc_x; if (y) *y = g_bc_y; if (w) *w = g_bc_w; if (h) *h = g_bc_h;
    return 1;
}
#endif

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
                    // MEASURED live (VM <vmid>, FreeCell): printf() calls
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
    // (#123 item 1 + auto-scale) WIDTH FIT. At the old 12-item cap the dock
    // could not outgrow any supported screen, so `dock_w` was computed once and
    // merely CLAMPED to W-24 if it somehow did. At 28 pins it genuinely does,
    // and the old clamp only narrowed the PANE - the slots kept their 48px
    // pitch and marched straight off the right-hand end of it, drawing (and
    // hit-testing) outside the dock.
    //
    // The ladder below is the auto-scale contract stated on
    // XFCE_DOCK_WIDTH_BUDGET_PCT above. It is a PURE FUNCTION of the user's
    // preferred height, the live item count and g_fb_width, evaluated from
    // scratch every frame, which is what makes the recovery path (fewer items
    // -> back to the preference) structural rather than a reset someone has to
    // remember to write. Bounded arithmetic only: at most ~60 loop iterations,
    // no allocation, no I/O, so it is safe in the draw thread (#426).
    //
    //   0. start at the user's preference, full 8px gutter
    //   1. over the 75% budget: shrink the HEIGHT (and with it the tile) down
    //      to XFCE_DOCK_H_MIN. Height first, because the user asked for the
    //      dock to scale, not for the icons to be crammed together.
    //   2. still over: tighten the gutter to 3px.
    //   3. still over the HARD budget (W-24, not the 75% one - by now we have
    //      accepted going over 75%): shrink below the user floor toward
    //      XFCE_DOCK_H_HARD_MIN. Emergency only, unreachable at >= 1024 wide.
    //   4. the pane itself is clamped to W-24 last, so it can never render
    //      off-screen even in the case step 3 could not solve.
    int32_t budget = (int32_t)W * XFCE_DOCK_WIDTH_BUDGET_PCT / 100;
    int32_t hardw  = (int32_t)W - 24;
    int32_t h    = xfce_dock_h();          // the PREFERENCE = a preferred maximum
    int32_t pad  = XFCE_DOCK_PAD;
    int32_t tile = xfce_tile_for_h(h);
    #define XFCE_ROW_W(t, p) (items * (t) + (items + 1) * (p) + (has_sep ? (p) + 1 : 0))
    int dock_w = XFCE_ROW_W(tile, pad);
    if (dock_w > budget) {
        while (h > XFCE_DOCK_H_MIN && XFCE_ROW_W(xfce_tile_for_h(h), pad) > budget) h--;
        while (pad > 3 && XFCE_ROW_W(xfce_tile_for_h(h), pad) > budget) pad--;
        while (h > XFCE_DOCK_H_HARD_MIN && XFCE_ROW_W(xfce_tile_for_h(h), pad) > hardw) h--;
        tile = xfce_tile_for_h(h);
        dock_w = XFCE_ROW_W(tile, pad);
    }
    #undef XFCE_ROW_W
    if (dock_w > hardw) dock_w = hardw;
    g_xd_tile = tile;
    g_xd_pad  = pad;
    // The EFFECTIVE height, which is what every consumer of "how tall is the
    // dock" must use from here on: the hit-test band, the click swallow test,
    // taskbar_bottom_inset()'s work-area strut and taskbar_panel_rects()'s
    // collision rect. Shrinking the pane without moving those together is the
    // obvious failure mode of this whole feature, so there is exactly ONE
    // variable and they all read it.
    if (h != g_xd_h) {
        g_xd_h = h;
        // The work area (and therefore maximized-window placement, desktop icon
        // clamping and widget bounds) is derived from the dock's height, so an
        // auto-scale step has to re-push it. NOT done inline: taskbar_apply_
        // work_area() re-clamps desktop icons, which have already been painted
        // by the time the taskbar draws, so doing it here would move them after
        // the fact for one frame. The flag is consumed by taskbar_geom_settle()
        // on main.c's existing 10-tick poll cadence instead.
        g_xd_h_dirty = 1;
    }
    int dock_x = (W - dock_w) / 2;
    int dock_y = H - g_xd_h;
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
    xfce_dock_paint_rounded(dock_x, dock_y, dock_w, g_xd_h, GLASS_SURF_DOCK);
    glass_edge_h(dock_x + XFCE_DOCK_CORNER_R, dock_y, dock_w - 2 * XFCE_DOCK_CORNER_R,
                CLR_TASKBAR_BORDER);
    glass_edge_v(dock_x, dock_y + XFCE_DOCK_CORNER_R, g_xd_h - XFCE_DOCK_CORNER_R,
                CLR_TASKBAR_BORDER);
    glass_edge_v(dock_x + dock_w - 1, dock_y + XFCE_DOCK_CORNER_R,
                g_xd_h - XFCE_DOCK_CORNER_R, CLR_TASKBAR_BORDER);
    glass_highlight_h(dock_x + 1 + XFCE_DOCK_CORNER_R, dock_y + 1,
                      dock_w - 2 - 2 * XFCE_DOCK_CORNER_R);

    xdock_reset(dock_y + 2, g_xd_h - 4, tile + pad);

    int32_t cx = dock_x + pad;
    for (int i = 0; i < nfav; i++) {
        int running = (fav_win[i] >= 0);
        int minimized = running && g_tb_wins[fav_win[i]].minimized;
        int focused   = running && g_tb_wins[fav_win[i]].focused && !minimized;
        xfce_draw_slot(xfce_slot++, cx, dock_y, favs[i].icon_id, minimized, running, focused, xfce_dt);
        // #44: app_id comes straight from the live window when this favorite
        // is running (MERGED) - feeds Force Quit. A PINNED (not-running)
        // slot has no window and so no app_id, which is correct: there is
        // nothing to force-quit.
        xdock_add(cx - pad / 2, running ? XDOCK_KIND_MERGED : XDOCK_KIND_PINNED,
                  running ? g_tb_wins[fav_win[i]].id : -1,
                  favs[i].exec_path, favs[i].launch_type, focused,
                  (int)favs[i].icon_id, running ? g_tb_wins[fav_win[i]].app_id : "");
        cx += tile + pad;
    }

    if (has_sep) {
        // #dockpad: the separator's slot is the normal trailing PAD gap after
        // the last pinned icon (already reserved by the loop's cx += ICON+PAD)
        // PLUS (pad + 1) of additional space. Centering a
        // 1px line in that combined (PAD + SEP_EXTRA) = (2*PAD+1) span means
        // PAD px on each side of the line, which falls exactly at cx (the
        // start of the extra span) - NOT at cx + PAD/2 - 1, which pushed the
        // line 3px right of center (measured: 11px left gap vs 5px right gap
        // instead of 8px/8px). Drawing at cx alone makes both gaps XFCE_DOCK_PAD,
        // matching docs/DOCK_XFCE_MOCKUP.html's spec position
        // (dock_x + 8 + pinned_count*48 == cx here).
        draw_vline(cx, dock_y + XFCE_DOCK_PAD, tile, CLR_TASKBAR_BORDER);
        cx += pad + 1;
    }

    for (int j = 0; j < n; j++) {
        if (!tb_window_is_app(&g_tb_wins[j]) || win_consumed[j]) continue;
        int minimized = g_tb_wins[j].minimized;
        int focused   = g_tb_wins[j].focused && !minimized;
        xfce_draw_slot(xfce_slot++, cx, dock_y, tb_icon_for_window(&g_tb_wins[j]), minimized, 1, focused, xfce_dt);
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
            xdock_add(cx - pad / 2, XDOCK_KIND_RUNNING, g_tb_wins[j].id, rpath, 0, focused,
                      ricon, g_tb_wins[j].app_id);
        }
        cx += tile + pad;
    }
#ifdef MAYTERA_TESTHOOK
    taskbar_dock_debug_dump(0);
#endif
}

static void taskbar_render_xfce(void) {
    taskbar_render_xfce_panel();
    taskbar_render_xfce_dock();
}

static bool taskbar_handle_xfce_dock_click(int32_t x, int32_t y) {
    if (y < g_xdock_y || y >= g_xdock_y + g_xdock_h) return false;
    int32_t w = g_xdock_slot_w;
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
            // #uiscale hit-test fix: shared with the draw side
            // (xfce_panel_buttons_rect() above) instead of a second literal
            // copy, and now actually tests `y` against the button's real
            // (vertically centered) band instead of accepting any y in the
            // whole XFCE_PANEL_H-tall strip.
            int32_t bs, by, start_x, logo_x;
            xfce_panel_buttons_rect(&bs, &by, &start_x, &logo_x);
            bool in_band = (y >= by && y < by + bs);
            if (in_band && x >= start_x && x < start_x + bs) {
                g_start_menu_open = !g_start_menu_open; g_needs_redraw = true; return true;
            }
            if (in_band && x >= logo_x && x < logo_x + bs) {
                launcher_toggle(); g_start_menu_open = false; g_needs_redraw = true; return true;
            }
            if (bar_clock_click(x, y)) return true;   // #129
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
    if (y >= g_fb_height - g_xd_h) return true;   // swallow clicks on the dock chrome (EFFECTIVE height, #123)
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
#define TBMENU_W  ui_px(108)
#define TBMENU_IH ui_px(22)
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
// #778: this popup's target is a DOS guest window, resolved once at open time
// (taskbar_handle_right_click() below) via the SAME detector contextmenu.c's
// richer CTX_MODE_DOCK menu uses, so a "Speed..." item can be offered here too
// - the non-XFCE dock styles have no other per-window menu to put it in.
static bool    g_tbmenu_is_dos = false;
static char    g_tbmenu_game[40];

bool taskbar_menu_is_open(void) { return g_tbmenu_open; }

static int32_t tbmenu_width(void) {
    int32_t w = text_width("Close") + 24;
    int32_t w2 = text_width("Speed...") + 24;
    if (w2 > w) w = w2;
    return w < TBMENU_W ? TBMENU_W : w;
}

static void taskbar_menu_geom(int32_t *mx, int32_t *my, int32_t *h) {
    int32_t mw = tbmenu_width();
    int32_t hh = TBMENU_IH * (g_tbmenu_is_dos ? 2 : 1) + 4;
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
    if (g_tbmenu_is_dos) {
        draw_text(x + 12, y + 4 + 5 + TBMENU_IH, "Speed...", CLR_MENU_TEXT);
    }
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
        // #uiscale: this was the ELEVENTH copy of the kernel's close-button
        // geometry, including a bare "- 2" inset - the exact same pattern
        // kernel/gui/window.h's own titlebar_btn_x() comment describes
        // fixing at its ten in-kernel copies ("It was the bare literal 2 at
        // all eleven sites"). titlebar_btn_x() itself is a kernel-only
        // static inline (kernel/gui/window.h) and not reachable from
        // userland, so this mirrors its EXACT formula instead: slot 0 (the
        // close button, TITLEBAR_SLOT_CLOSE), bs=btn_size, using the SAME
        // already-scaled theme metric (THEME_METRIC_TITLEBAR_BTN_GAP) the
        // kernel reads for the inset, with the kernel's own fallback (2) so
        // an unset theme key produces byte-identical geometry on both sides.
        int btn_inset  = theme_metric_or(THEME_METRIC_TITLEBAR_BTN_GAP, 2);
        int btn_y_off  = border_w + (titlebar_h - btn_size) / 2;
        int cx = wins[i].x + wins[i].width - btn_size - btn_inset + btn_size / 2;
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
// #161: PROC_STATE_ZOMBIE from kernel/proc/process.h. proc_info_t.state carries
// the kernel enum straight through (proc_snapshot()), and userland has no
// header for it; kept next to its only use rather than added to the shared
// syscall.h, where a second, drifting copy of the enum would be the worse
// outcome. The kernel-side value is pinned by a _Static_assert in
// kernel/proc/process.c (PROC_STATE_ZOMBIE == 5, for rustkern/procreap.rs).
#define TB_PROC_STATE_ZOMBIE 5

// #745 (docs/CONFIRM_MODAL_DESIGN.html, 86f3cea): this used to fire
// SIGKILL with NO confirmation of any kind - the same gap the design doc's
// audit found in Task Manager's End Task/Kill. Now gated behind the shared
// system-modal confirm card (confirmdialog.c), same component the Start
// menu's power confirm uses. taskbar_force_quit_app_id() below only OPENS
// the dialog; the actual kill moved to tb_force_quit_dispatch().
static void tb_force_quit_dispatch(const char *app_id) {
    proc_info_t procs[64];
    int n = sys_proc_list(procs, 64);
    if (n <= 0) return;
    for (int i = 0; i < n; i++) {
        // #161: SKIP CORPSES. sys_proc_list() reports ZOMBIEs alongside live
        // processes, and this loop killed the FIRST name match in slot order.
        // Before this ticket every app launched from the dock leaked a zombie
        // on exit (see main.c's signal(SIGCHLD, SIG_IGN)), so a user who had
        // opened and closed the music player once already had a dead
        // MUSICPLAY in a LOW slot: Force Quit then found the corpse first, sent
        // it a SIGKILL that is a no-op by definition, got 0 back, and returned
        // without ever looking at the live process the user was pointing at.
        // That is the owner's "force quit from the dock is not working either",
        // and it survives the leak fix because a zombie can still exist in the
        // window between an exit and the next reap sweep.
        if (procs[i].state == TB_PROC_STATE_ZOMBIE) continue;
        if (strcmp(procs[i].name, app_id) == 0) {
            syscall2(SYS_KILL, (long)procs[i].pid, 9 /* SIGKILL, libc/signal.h */);
            return;
        }
    }
}

static confirm_dialog_t g_fq_dialog;
static char             g_fq_target[64];

void taskbar_force_quit_app_id(const char *app_id) {
    if (!app_id || !app_id[0]) return;
    strncpy(g_fq_target, app_id, sizeof(g_fq_target) - 1);
    g_fq_target[sizeof(g_fq_target) - 1] = 0;
    char body[128];
    // "Force quit <name>? Any unsaved work in this app will be lost." -
    // same title/verb/consequence pattern the design doc's destructive
    // variant uses for Shut Down/Restart (design doc 2.2).
    strcpy(body, "Force quit ");
    strncat(body, app_id, sizeof(body) - strlen(body) - 32);
    strcat(body, "? Any unsaved work in this app will be lost.");
    const char *lines[1] = { body };
    confirm_dialog_open(&g_fq_dialog, CONFIRM_DESTRUCTIVE, "Force Quit",
                        lines, 1, "Cancel", "Force Quit");
    g_needs_redraw = true;
}

bool taskbar_force_quit_confirm_open(void) { return confirm_dialog_is_open(&g_fq_dialog); }

void taskbar_force_quit_confirm_render(void) {
    if (!confirm_dialog_is_open(&g_fq_dialog)) return;
    confirm_dialog_render(&g_fq_dialog);
}

int taskbar_force_quit_confirm_handle_key(int key) {
    if (!confirm_dialog_is_open(&g_fq_dialog)) return 0;
    int r = confirm_dialog_handle_key(&g_fq_dialog, key);
    if (r == 2) tb_force_quit_dispatch(g_fq_target);
    return 1;   // true modal: swallow every key while open
}

bool taskbar_force_quit_confirm_handle_mouse(int32_t x, int32_t y, bool clicked) {
    if (!confirm_dialog_is_open(&g_fq_dialog)) return false;
    int r = confirm_dialog_handle_mouse(&g_fq_dialog, x, y, clicked);
    if (r == 2) tb_force_quit_dispatch(g_fq_target);
    return true;   // true modal: swallow every other click while open
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
    } else if (g_tbmenu_is_dos && x >= mx && x < mx + w &&
               y >= my + 2 + TBMENU_IH && y < my + 2 + TBMENU_IH * 2) {
        // #778: opens the true-modal Speed dialog, which takes over input
        // gating from here (g_modal_grabs[] "dos-speed" row) - this popup
        // itself is a lightweight click-away one, so it closes normally below.
        dosspeed_open(g_tbmenu_win_id, g_tbmenu_game);
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
    // #129: tray-icon right-click (Wi-Fi/Bluetooth quick-toggle) - checked
    // first because g_tray_y/g_tray_h is a distinct strip from every dock
    // style's app-tile hit-list below (including DOCK_XFCE's bottom
    // g_xdock_y, since the tray itself lives in XFCE's TOP panel), so this
    // can never shadow the per-style logic that follows it.
    if (tray_right_click(x, y)) return true;
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
        int32_t w = g_xdock_slot_w;
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
    // #778: resolved once here, at open time, exactly like `maximized` above -
    // never re-derived from stale per-frame state.
    g_tbmenu_is_dos = dosspeed_window_is_dos(win_id, g_tbmenu_game, (int)sizeof(g_tbmenu_game)) != 0;
    g_tbmenu_open = true;
    g_needs_redraw = true;
    return true;
}

// ---------------------------------------------------------------------------
// Dispatchers + work-area insets.
// ---------------------------------------------------------------------------
void taskbar_render(void) {
    // #129: reset every frame BEFORE the per-style render, so a live dock-
    // style switch (Settings -> Appearance applies it immediately, no
    // restart) can never leave a stale clickable clock rect hanging over
    // whatever DOCK_DEFAULT now draws in that same screen area - only a
    // style that actually draws a bar clock this frame sets it non-zero
    // again below.
    bar_clock_set_rect(0, 0, 0, 0);
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
        case DOCK_XFCE:   return (int)g_xd_h;   // (#123) EFFECTIVE, i.e. post-auto-scale
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
            out[n].x = g_xfce_dock_x; out[n].y = H - g_xd_h;
            out[n].w = g_xfce_dock_w; out[n].h = g_xd_h; n++;
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
    g_batt_card_open = 0;   // #battmeter: anchored off the old bar position
    g_needs_redraw = true;
}

// #241/#battmeter: is a taskbar-owned popup currently showing? (performance
// popup, or the battery info card)
bool taskbar_popup_active(void) {
    return g_perf_open != 0 || g_batt_card_open != 0;
}

// #241/#battmeter: handle mouse while a taskbar-owned popup is open. Runs in
// main.c BEFORE taskbar_handle_mouse so it can intercept clicks anywhere on
// screen.
bool taskbar_popup_handle_mouse(int32_t x, int32_t y, bool clicked) {
    // #battmeter: the info card is a plain click-through readout, not a
    // control surface, so EVERY click while it is open dismisses it (whether
    // the click landed on the card or elsewhere) - the same "any click closes
    // it" rule the performance popup below already applies on its own
    // "click anywhere else" fallback path.
    if (g_batt_card_open) {
        int inside_card = (x >= g_bc_x && x < g_bc_x + g_bc_w &&
                           y >= g_bc_y && y < g_bc_y + g_bc_h);
        if (!clicked) return inside_card != 0;
        g_batt_card_open = 0;
        g_needs_redraw = true;
        return true;
    }

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
