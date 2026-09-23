// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// cardfile.c - the Cardfile desktop layout: deck chrome renderer + input
// (agent A, docs/CARDFILE_ARCHITECTURE.md). Built on the frozen data model in
// cardfile_model.h (agent B, landed at aab852f9) and the rotated-label
// primitive in cardtext.h (already working). Ported from the geometry,
// colour and interaction spec in the build host:/root/cardfile-mockup.html (its
// "Design notes" drawer), adapted to draw.c primitives and this project's
// modal-dialog convention (true modals: ESC/explicit-button only, NEVER
// click-away - see the popup section below, which deliberately differs from
// the mockup's click-away-to-dismiss JS).
//
// WINDOW HOSTING: agent C's cardfile_host.c/.h (task #404) landed while this
// file was being verified and is now wired in below:
//   - the "+" picker calls cf_add_card() then cf_host_launch() and binds the
//     returned win_id (or leaves it 0 for cf_host_apply() to bind once the
//     window appears - see cardfile_host.h's own contract comment).
//   - cardfile_host_tick() (called once per frame from main.c, BEFORE
//     compositor_render_windows(), per cardfile_host.h) calls cf_host_apply()
//     to place/hide every hosted window to match the deck's current layout.
//   - the open card's body (cf_body_rect()) is still drawn as a FRAME +
//     caption foot only: the interior is the hosted window's rect, owned by
//     the kernel compositor and CF_MANAGED (no kernel chrome) - this file
//     never paints inside it.
//
// Z-order (cardfile_model.h CF_Z_*): edges/bodies, stowed plates (depth
// order, farthest first), open plate(s), rail tabs, drag ghost, popups.
// Damage (#379): cardfile_render() is one pass over the WHOLE deck region
// every frame it runs (same as taskbar_render()/startmenu_render() etc. in
// this codebase - none of them sub-tile their own damage either), but it
// never touches the framebuffer outside the deck/rail area and never blocks
// or polls (#426): every op below is plain arithmetic or a single bounded
// draw call.

#include "cardfile.h"
#include "compositor.h"
#include "cardtext.h"
#include "cardfile_model.h"
#include "cardfile_host.h"
#include "cardfile_glass.h"   // (cfglass) modern-theme glass chrome passes, agent D
#include "../../libc/syscall.h"
#include "../../libc/string.h"
#include "../../libc/stdio.h"   // (cfdock) sprintf, for dock labels/thickness readouts

extern int g_dock_style;   // see compositor.h DOCK_* and main.c dock_style_poll()

int cardfile_active(void) { return g_dock_style == DOCK_CARDFILE; }

// ============================================================================
// The deck. One process-lifetime instance; cf_deck_init() runs once.
// ============================================================================
static cf_deck_t g_cf_deck;
static int       g_cf_inited;

static void cf_ensure_init(void) {
    if (!g_cf_inited) { cf_deck_init(&g_cf_deck); g_cf_inited = 1; }
}

// ============================================================================
// Metrics + geometry (porting spec "Geometry" table). g_glass_enable is the
// retro-vs-modern split (#745; see cardfile.h skeleton note - unchanged from
// the landed skeleton), so it picks BOTH the metric set and glass-vs-flat.
// ============================================================================
typedef struct {
    int rail_w, plus_sz, plus_y, tab_w, foot, tab_top;
    int edge_w_theme;   // RAW theme value (28/30) - cf_edge_width() ui_px()s it below
    int step_theme;     // RAW theme value (30/34)
} cf_metrics_t;

static cf_metrics_t cf_metrics(void) {
    int modern = g_glass_enable ? 1 : 0;
    cf_metrics_t m;
    m.rail_w       = ui_px(modern ? 30 : 28);
    m.plus_sz      = ui_px(modern ? 30 : 28);
    m.plus_y       = ui_px(10);
    m.tab_w        = ui_px(modern ? 32 : 30);
    m.foot         = ui_px(modern ? 100 : 96);
    m.tab_top      = ui_px(modern ? 52 : 48);
    m.edge_w_theme = modern ? 30 : 28;
    m.step_theme   = modern ? 34 : 30;
    return m;
}

int cardfile_rail_width(void) { return cf_metrics().rail_w; }

// (cfdock) Persistence accessors - see cardfile.h's own comment.
int cardfile_dock_count_for_profile(void) {
    cf_ensure_init();
    return g_cf_deck.ndocks;
}
int cardfile_dock_pack_for_profile(int idx) {
    cf_ensure_init();
    if (idx < 0 || idx >= g_cf_deck.ndocks) return -1;
    return cf_dock_pack(&g_cf_deck.docks[idx]);
}
void cardfile_dock_add_from_profile(int packed) {
    cf_ensure_init();
    cf_dock_pos_t pos; int thickness, items[CF_DOCKITEM_COUNT], nitems;
    cf_dock_unpack(packed, &pos, &thickness, items, &nitems);
    uint32_t id = cf_dock_add(&g_cf_deck, pos, thickness);
    if (id) cf_dock_set_items(&g_cf_deck, id, items, nitems);
}

static cf_geom_t cf_build_geom(void) {
    cf_metrics_t m = cf_metrics();
    cf_geom_t g;
    g.rail_w   = m.rail_w;
    g.tab_w    = m.tab_w;
    g.tab_top  = m.tab_top;
    g.foot     = m.foot;
    g.screen_w_raw = g_fb_width;
    g.screen_h_raw = g_fb_height;
    // (cfdock) Reserve every EDGE dock's thickness before anything else is
    // computed: the deck's whole working area (rail + card walk) shrinks/
    // shifts by this, so every downstream geometry call already sees the
    // post-reservation size - "cards never draw under a dock" by
    // construction, not by a later clip.
    cf_dock_reserve_edges(&g_cf_deck, g.screen_w_raw, g.screen_h_raw,
                           &g.origin_x, &g.origin_y, &g.screen_w, &g.screen_h);
    g.edge_w   = cf_edge_width(ui_px(m.edge_w_theme), g.rail_w, g_cf_deck.nslots, g.screen_w);
    g.tab_step = cf_tab_step(ui_px(m.step_theme), g.tab_top, g.foot, ui_px(CF_TAB_MAX_H),
                             g_cf_deck.nslots, g.screen_h);
    return g;
}

// ============================================================================
// Small local helpers: colour mixing (draw_blend() is static to draw.c),
// rect hit-test, "fake bold" text (draw twice, 1px offset - no bold glyphs).
// ============================================================================
static uint32_t cf_mix(uint32_t a, uint32_t b, int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int ar = (int)((a >> 16) & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF), bg = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
    int rr = ar + (br - ar) * pct / 100;
    int rg = ag + (bg - ag) * pct / 100;
    int rb = ab + (bb - ab) * pct / 100;
    return 0xFF000000u | ((uint32_t)rr << 16) | ((uint32_t)rg << 8) | (uint32_t)rb;
}
// cardfile_model.h's cf_depth_dim_x10() returns tenths-of-a-percent (e.g. 45 =
// 4.5%). Darken toward black by that fraction - "receding into the stack".
static uint32_t cf_dim(uint32_t color, int dim_x10) {
    return cf_mix(color, 0xFF000000u, dim_x10 / 10);
}
static int cf_rect_hit(cf_rect_t r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}
// (cfmaxwidth) The label centring fix: given the vertical band [area_top,
// area_top+cap) a rotated label may occupy, and the label's already-clamped
// run length, return the y_top that CENTRES it in that band instead of
// pinning it to the top. Every cardtext_vertical() call site in this file
// uses this now (tab plates, the rail's Sort tab, the drag ghost, the
// maximize summary tabs) - the owner's "must be centred, not closer to the
// top" note applied to all of them, not just card tabs.
static int cf_vcenter_top(int area_top, int cap, int run) {
    int off = (cap - run) / 2;
    if (off < 0) off = 0;
    return area_top + off;
}
// ============================================================================
// Card colour palette. Two rows (retro / modern), CF_COLOR_* order, ported
// verbatim from the mockup's PALETTE.retro / PALETTE.light (a "modern" theme
// is not further split light/dark here - the compositor exposes no such flag
// to this file; readable_ink()/readable_ink_dim() still float ink correctly
// against whichever of these two base tones is drawn).
// ============================================================================
static const uint32_t CF_PAL_RETRO[CF_COLOR_COUNT]  = {
    0xFFB9B9B9, 0xFFC9BBA3, 0xFFA9B9A4, 0xFF9FAEC0,
    0xFFC3A7A7, 0xFFAEA4BD, 0xFFCDC49F, 0xFF98B5B3,
};
static const uint32_t CF_PAL_MODERN[CF_COLOR_COUNT] = {
    0xFFF4F5F7, 0xFFECE3D3, 0xFFDCE6D9, 0xFFD8E0EB,
    0xFFEBDCDC, 0xFFE2DDEB, 0xFFEEE8D3, 0xFFD5E4E3,
};
// (cfglass) The mockup's PALETTE.dark row (Graphite/Umber/Moss/Slate/Wine/
// Plum/Ochre/Spruce), chosen by cfg_is_dark() so a dark modern theme frosts
// its cards in the design's dark tones instead of light pastels.
static const uint32_t CF_PAL_MODERN_DARK[CF_COLOR_COUNT] = {
    0xFF26262D, 0xFF3E3830, 0xFF2F3A32, 0xFF2E3744,
    0xFF3F3036, 0xFF37303F, 0xFF3F3A2E, 0xFF2B3A3B,
};
static const char *CF_PAL_NAME[CF_COLOR_COUNT] = {
    "Grey", "Sand", "Sage", "Slate", "Rose", "Heather", "Wheat", "Spruce",
};
static uint32_t cf_card_color(int color) {
    if (color < 0 || color >= CF_COLOR_COUNT) color = CF_COLOR_DEFAULT;
    if (!g_glass_enable) return CF_PAL_RETRO[color];
    return cfg_is_dark() ? CF_PAL_MODERN_DARK[color] : CF_PAL_MODERN[color];
}
static const char *CF_CAT_NAME[CF_CAT_COUNT] = {
    "Internet", "Media", "Accessories", "Office", "Games", "System",
};

// ============================================================================
// "+" picker's app source. cardfile_host.h (agent C) has not landed and
// g_categories[]/g_menu_items[] are private to startmenu.c with no
// enumeration accessor exposed via compositor.h, so this scans /APPS
// directly - the SAME sys_open()+sys_readdir_raw() pattern iconpicker.c
// already uses for the identical reason (#44) - and applies a small local
// keyword table to guess a CF_CAT_* for sorting/grouping. This is a
// deliberate, documented stand-in for "reuse the real taxonomy": it reflects
// the apps genuinely installed on THIS image (which the mockup's hardcoded
// CATS[] cannot), but it does not carry the .MENU model's real category
// assignments. Replace with a real startmenu.c enumeration accessor when one
// exists (see cardfile.h's top-of-file note).
// ============================================================================
typedef struct { char name[CF_MAX_TITLE]; char path[CF_MAX_APP_PATH]; int cat; } cf_pick_app_t;
#define CF_PICK_MAX 128
static cf_pick_app_t g_cf_apps[CF_PICK_MAX];
static int g_cf_app_count;

static int cf_guess_cat(const char *name) {
    // Longest/most-specific keywords first within each category; falls back
    // to Accessories, matching the mockup's default for anything unmatched.
    static const struct { const char *kw; int cat; } table[] = {
        { "BROWSER", CF_CAT_INTERNET }, { "IRC", CF_CAT_INTERNET }, { "AI", CF_CAT_INTERNET },
        { "WEATHER", CF_CAT_INTERNET }, { "FEED", CF_CAT_INTERNET },
        { "PAINT", CF_CAT_MEDIA }, { "IMAGE", CF_CAT_MEDIA }, { "GALLERY", CF_CAT_MEDIA },
        { "MEDIA", CF_CAT_MEDIA }, { "MUSIC", CF_CAT_MEDIA }, { "MIDI", CF_CAT_MEDIA },
        { "SNAPSHOT", CF_CAT_MEDIA },
        { "WRITER", CF_CAT_OFFICE }, { "SHEET", CF_CAT_OFFICE }, { "SLIDE", CF_CAT_OFFICE },
        { "WORD", CF_CAT_OFFICE },
        { "ARENA", CF_CAT_GAMES }, { "CHESS", CF_CAT_GAMES }, { "SQUADRON", CF_CAT_GAMES },
        { "SOLITAIRE", CF_CAT_GAMES }, { "DOOM", CF_CAT_GAMES }, { "LEMMINGS", CF_CAT_GAMES },
        { "PONG", CF_CAT_GAMES }, { "TETRIS", CF_CAT_GAMES }, { "FREECELL", CF_CAT_GAMES },
        { "SKIFREE", CF_CAT_GAMES }, { "KEEN", CF_CAT_GAMES }, { "SIMCITY", CF_CAT_GAMES },
        { "GAME", CF_CAT_GAMES },
        { "SETTINGS", CF_CAT_SYSTEM }, { "FONTBOOK", CF_CAT_SYSTEM }, { "NETWORK", CF_CAT_SYSTEM },
        { "TASKMGR", CF_CAT_SYSTEM }, { "TASKMANAGER", CF_CAT_SYSTEM }, { "SYSMON", CF_CAT_SYSTEM },
        { "STORE", CF_CAT_SYSTEM }, { "SYSLOG", CF_CAT_SYSTEM }, { "RECYCLE", CF_CAT_SYSTEM },
        { NULL, 0 },
    };
    for (int i = 0; table[i].kw; i++) {
        const char *kw = table[i].kw, *p = name;
        int klen = 0; while (kw[klen]) klen++;
        int nlen = 0; while (p[nlen]) nlen++;
        for (int s = 0; s + klen <= nlen; s++) {
            int match = 1;
            for (int c = 0; c < klen; c++) {
                char a = p[s + c]; if (a >= 'a' && a <= 'z') a = (char)(a - 32);
                if (a != kw[c]) { match = 0; break; }
            }
            if (match) return table[i].cat;
        }
    }
    return CF_CAT_ACCESSORIES;
}

// Title-case a FAT-style "TERMINAL" -> "Terminal" for display; leaves
// anything already mixed-case alone.
static void cf_nice_title(char *out, unsigned long cap, const char *raw) {
    unsigned long i = 0, n = 0;
    int seen_lower = 0;
    for (i = 0; raw[i]; i++) if (raw[i] >= 'a' && raw[i] <= 'z') { seen_lower = 1; break; }
    for (i = 0; raw[i] && n + 1 < cap; i++) {
        char c = raw[i];
        if (!seen_lower) {
            if (n == 0 || out[n - 1] == ' ' || out[n - 1] == '_') {
                // keep as-is (first letter of a word) - already upper
            } else if (c >= 'A' && c <= 'Z') {
                c = (char)(c + 32);
            }
        }
        if (c == '_') c = ' ';
        out[n++] = c;
    }
    out[n] = 0;
}

static void cf_apps_rescan(void) {
    g_cf_app_count = 0;
    int fd = sys_open("/APPS", 0);
    if (fd < 0) return;
    dirent_t de;
    while (g_cf_app_count < CF_PICK_MAX && sys_readdir_raw(fd, &de) == 0) {
        de.name[sizeof(de.name) - 1] = '\0';
        if (de.name[0] == '\0' || de.name[0] == '.') continue;
        if (DIRENT_IS_DIR(de)) continue;
        cf_pick_app_t *a = &g_cf_apps[g_cf_app_count];
        cf_nice_title(a->name, sizeof(a->name), de.name);
        int n = 0; while (de.name[n]) n++;
        int p = 0;
        a->path[p++] = '/'; const char *base = "APPS/";
        for (int k = 0; base[k] && p < (int)sizeof(a->path) - 1; k++) a->path[p++] = base[k];
        for (int k = 0; k < n && p < (int)sizeof(a->path) - 1; k++) a->path[p++] = de.name[k];
        a->path[p] = 0;
        a->cat = cf_guess_cat(de.name);
        g_cf_app_count++;
    }
    sys_close(fd);
}

// ============================================================================
// Modifier keys (Shift/Ctrl "open as column" - "keep others open"). Reads the
// LIVE physical state directly (SYS_KEY_MODS, argument-free, never blocks) -
// gui_mods.h's tracker is for app-side gui_event_t consumers and is not wired
// to the compositor's own raw key queue, so it does not apply here. The low
// bits match kernel/drivers/keymod.h exactly (see gui_mods.h's own comment).
// ============================================================================
#define CF_MOD_SHIFT 0x01u
#define CF_MOD_CTRL  0x02u
static int cf_mod_open_column(void) {
    unsigned int m = sys_key_mods();
    return (m & (CF_MOD_SHIFT | CF_MOD_CTRL)) != 0;
}

// ============================================================================
// Icon glyphs for the tab buttons / pane-header buttons. No SVG in draw.c;
// small hand-drawn shapes with draw_line_aa() (crisp AA strokes) match the
// mockup's thin-stroke icon language well enough at 10-14px.
// ============================================================================
static void cf_icon_close(int32_t cx, int32_t cy, int r, uint32_t c) {
    draw_line_aa(cx - r, cy - r, cx + r, cy + r, 1.6f, c, 255);
    draw_line_aa(cx + r, cy - r, cx - r, cy + r, 1.6f, c, 255);
}
static void cf_icon_file(int32_t cx, int32_t cy, int r, uint32_t c) {   // stow: down arrow into tray
    draw_line_aa(cx, cy - r, cx, cy + r / 2, 1.3f, c, 255);
    draw_line_aa(cx - r / 2, cy, cx, cy + r / 2, 1.3f, c, 255);
    draw_line_aa(cx + r / 2, cy, cx, cy + r / 2, 1.3f, c, 255);
    draw_hline(cx - r, cy + r, (int32_t)(2 * r + 1), c);
}
static void cf_icon_dup(int32_t cx, int32_t cy, int r, uint32_t c) {    // two overlapping squares
    draw_rect_outline(cx - r, cy - r / 2, r + 2, r + 2, c);
    draw_rect_outline(cx - r / 2, cy - r, r + 2, r + 2, c);
}
static void cf_icon_eject(int32_t cx, int32_t cy, int r, uint32_t c) {  // pull-out arrow
    draw_line_aa(cx - r, cy, cx + r, cy, 1.3f, c, 255);
    draw_line_aa(cx + r / 2, cy - r / 2, cx + r, cy, 1.3f, c, 255);
    draw_line_aa(cx + r / 2, cy + r / 2, cx + r, cy, 1.3f, c, 255);
}
static void cf_icon_swatch(int32_t cx, int32_t cy, int r, uint32_t fill, uint32_t border) {
    draw_circle_filled(cx, cy, r, fill);
    draw_circle_outline(cx, cy, r, border);
}
static void cf_icon_sort(int32_t x, int32_t y, int w, uint32_t c) {     // three shrinking bars
    draw_hline(x, y, w, c);
    draw_hline(x, y + 3, (int32_t)(w * 7 / 10), c);
    draw_hline(x, y + 6, (int32_t)(w * 4 / 10), c);
}
// (cfmaxwidth) maximize/restore: four diagonal ticks pointing away from
// centre (maximize - "grow to fill") or toward it (restore - "shrink back").
static void cf_icon_maximize(int32_t cx, int32_t cy, int r, uint32_t c, int active) {
    int h = r / 2 ? r / 2 : 1;
    if (!active) {
        draw_line_aa(cx - h, cy - h, cx - r, cy - r, 1.3f, c, 255);
        draw_line_aa(cx + h, cy - h, cx + r, cy - r, 1.3f, c, 255);
        draw_line_aa(cx - h, cy + h, cx - r, cy + r, 1.3f, c, 255);
        draw_line_aa(cx + h, cy + h, cx + r, cy + r, 1.3f, c, 255);
    } else {
        draw_line_aa(cx - r, cy - r, cx - h, cy - h, 1.3f, c, 255);
        draw_line_aa(cx + r, cy - r, cx + h, cy - h, 1.3f, c, 255);
        draw_line_aa(cx - r, cy + r, cx - h, cy + h, 1.3f, c, 255);
        draw_line_aa(cx + r, cy + r, cx + h, cy + h, 1.3f, c, 255);
    }
}

// ============================================================================
// Tab-button layout (the 4 buttons stacked under the label on an OPEN plate:
// colour, duplicate, file/stow, close - matches the mockup's DOM order).
// ============================================================================
// Rendering-only chrome geometry that the frozen model deliberately does not
// carry (cardfile_model.h works in caller-supplied rects; frame/foot/header
// thickness is chrome, same footing as cf_metrics_t's rail_w/tab_w above).
#define CF_CARD_FRAME_PX   6    // spec "card_frame"
#define CF_CARD_FOOT_PX    20   // spec "caption foot"
#define CF_PANE_HEADER_PX  22   // spec "pane header"
// (cfhostinset) The pane's non-header inset. MUST match cardfile_glass.c's
// cfg_pane()'s own local `ring = ui_px(4)` - that file (agent D) draws the
// glass pane's left/right/bottom bands at this thickness; the flat pane
// (cf_draw_group_body() below) has no equivalent band today (it fills the
// whole pane with the card colour and relies on the window sitting on top),
// so this is also the inset the flat theme's hosted window now gets, for
// the same reason the single-card frame inset is applied uniformly in both
// themes: one hosting behaviour, not two. If cfg_pane()'s ring ever changes,
// change this too - cardfile_glass.c does not expose it as a shared symbol
// (out of this file's ownership), so the two are kept in sync by comment,
// not by the compiler.
#define CF_PANE_RING_PX    4

// (cfmaxwidth) CF_BTN_MAXIMIZE added between DUP and FILE; CF_BTN_N now
// matches cardfile_model.h's CF_TAB_BTN_COUNT (5) - the two are asserted
// against each other in cf_tab_btn_rect() below.
enum { CF_BTN_COLOR = 0, CF_BTN_DUP, CF_BTN_MAXIMIZE, CF_BTN_FILE, CF_BTN_CLOSE, CF_BTN_N };
_Static_assert(CF_BTN_N == CF_TAB_BTN_COUNT, "cardfile.c's button enum and cardfile_model.h's CF_TAB_BTN_COUNT must match");
static cf_rect_t cf_tab_btn_rect(cf_rect_t plate, int idx) {
    int bh = ui_px(CF_TAB_BTN_H);
    cf_rect_t r;
    r.x = plate.x + ui_px(2);
    r.w = plate.w - ui_px(4);
    r.h = bh - ui_px(4);
    r.y = plate.y + plate.h - (CF_BTN_N - idx) * bh + ui_px(2);
    return r;
}
// Group pane header buttons: eject, then close, right-aligned in the 22px ph.
enum { CF_PANE_BTN_EJECT = 0, CF_PANE_BTN_CLOSE, CF_PANE_BTN_N };
static cf_rect_t cf_pane_btn_rect(cf_rect_t pane, int idx) {
    int bh = ui_px(18), bw = ui_px(18);
    cf_rect_t r;
    r.w = bw; r.h = bh;
    r.y = pane.y + ui_px(2);
    r.x = pane.x + pane.w - (CF_PANE_BTN_N - idx) * (bw + ui_px(2));
    return r;
}

// ============================================================================
// Drag state (tab drag/group/pull-out-to-column; column resize grip; group
// row/col divider). Recomputed geometry every tick - all pure arithmetic, no
// syscalls in the hot path (#426).
// ============================================================================
typedef enum {
    CF_DRAG_NONE = 0, CF_DRAG_TAB, CF_DRAG_GRIP, CF_DRAG_VDIV, CF_DRAG_HDIV,
    CF_DRAG_DOCK_THICKNESS,   // (cfdock) dragging a dock bar's inner-edge resize grip
} cf_drag_kind_t;

static cf_drag_kind_t g_cf_drag_kind = CF_DRAG_NONE;
static uint32_t g_cf_drag_slot;     // slot under the grip/divider/tab
static uint32_t g_cf_drag_card;     // the specific card pressed (member chip or the slot's focused card)
static int      g_cf_drag_moved;
static int      g_cf_drag_start_x, g_cf_drag_start_y;
static int      g_cf_drag_size_a, g_cf_drag_size_b;   // divider: sizes CAPTURED AT DRAG START
static int      g_cf_drag_row, g_cf_drag_col;
static uint32_t g_cf_drag_target;   // group-drop target slot id, 0 = none (pull-out)
static uint32_t g_cf_drag_dock_id;  // (cfdock) CF_DRAG_DOCK_THICKNESS: which dock's grip is held

// ============================================================================
// Popups. TRUE MODALS per this project's convention (never click-away -
// close only via an explicit button or ESC). This deliberately differs from
// the mockup's click-away JS.
// ============================================================================
typedef enum {
    CF_POP_NONE = 0, CF_POP_PICKER, CF_POP_SORT, CF_POP_SWATCH,
    CF_POP_DOCKLIST,   // (cfdock) rail "Docks" button: manage/add docks
    CF_POP_DOCKCTX,    // (cfdock) right-click on a dock bar: move/remove
} cf_popup_t;
static cf_popup_t g_cf_popup = CF_POP_NONE;
static cf_rect_t  g_cf_popup_rect;
static int        g_cf_popup_hover;     // hovered row/swatch index, -1 none
static uint32_t   g_cf_swatch_card;     // CF_POP_SWATCH target
static uint32_t   g_cf_popup_dock;      // (cfdock) CF_POP_DOCKCTX target dock id

// picker's flattened row list (header rows + item rows, grouped by category)
typedef struct { int is_header; int app_idx; } cf_pick_row_t;
static cf_pick_row_t g_cf_rows[CF_PICK_MAX + CF_CAT_COUNT];
static int g_cf_row_count;

static void cf_popup_close(void) { g_cf_popup = CF_POP_NONE; g_cf_popup_hover = -1; }

// Forward: popup drawing lives after the deck-layout render code below (both
// are this translation unit; declared here so cardfile_render() can call it
// without an inline extern).
static void cf_draw_popup(void);

// The sideways label length to reserve for a slot's plate: the focused
// member's title length, plus room for the count pill + member ticks when
// the slot is a group (cf_draw_tab_plate() draws those ABOVE and BELOW the
// label respectively - see there).
static int cf_plate_label_px(cf_slot_t *s) {
    cf_card_t *fc = cf_slot_focused_card(s);
    int base = fc ? cardtext_vertical_len(fc->title, 12) : ui_px(40);
    if (s->ncards > 1) base += ui_px(30);
    return base;
}

static void cf_picker_build_rows(void) {
    g_cf_row_count = 0;
    for (int cat = 0; cat < CF_CAT_COUNT; cat++) {
        int any = 0;
        for (int i = 0; i < g_cf_app_count; i++) if (g_cf_apps[i].cat == cat) { any = 1; break; }
        if (!any) continue;
        if (g_cf_row_count < CF_PICK_MAX + CF_CAT_COUNT) {
            g_cf_rows[g_cf_row_count].is_header = 1;
            g_cf_rows[g_cf_row_count].app_idx = cat;
            g_cf_row_count++;
        }
        for (int i = 0; i < g_cf_app_count && g_cf_row_count < CF_PICK_MAX + CF_CAT_COUNT; i++) {
            if (g_cf_apps[i].cat != cat) continue;
            g_cf_rows[g_cf_row_count].is_header = 0;
            g_cf_rows[g_cf_row_count].app_idx = i;
            g_cf_row_count++;
        }
    }
}

static void cf_picker_open(void) {
    cf_apps_rescan();
    cf_picker_build_rows();
    g_cf_popup = CF_POP_PICKER;
    g_cf_popup_hover = -1;
}

// ============================================================================
// RENDER
// ============================================================================

// -- rail (unchanged in spirit from the landed skeleton: hatch + "+" + Sort) -
static void cf_plate(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (g_glass_enable) {   // (cfglass) glass tab on the theme surface colour
        cfg_plate((cf_rect_t){ x, y, w, h }, cfg_surface_color(), 0, 0);
        return;
    }
    glass_or_flat(x, y, w, h, GLASS_SURF_PANEL);
    if (!g_glass_enable) {
        draw_hline(x, y, w, 0xFFFFFFFFu);
        draw_vline(x, y, h, 0xFFFFFFFFu);
        draw_hline(x, y + h - 1, w, CLR_MENU_BORDER);
        draw_vline(x + w - 1, y, h, CLR_MENU_BORDER);
    } else {
        draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    }
}

// (cfmaxwidth) THE SEAMLESSLY-JOINED CARD-TAB SHAPE. A card tab must not
// read as a rectangle stuck onto the side of its card: every tab in this
// deck joins its card on the LEFT (the rail/spine is always to the left,
// tabs fan out rightward), so the two LEFT corners get different treatment
// from the two RIGHT corners (the free end, poking into open space, gets an
// ordinary small round - unchanged). No border stroke is drawn on the left
// side at all, so no seam line crosses the join.
//
// (cfdock, scallop-fix) CORRECTED - the ORIGINAL cfmaxwidth version of this
// function applied draw_round_corners_capture()/restore() to the TAB'S OWN
// rect at the join corners, with a bigger radius than the outer corners.
// That primitive can only ever ERODE the rect it is given - see
// draw_round_corners_capture()'s own math (draw.c): coverage is 0 (fully
// erased, revealing whatever was behind) exactly AT the rect's own corner
// point and rises to 1 (fully kept) moving away from it. Applied to the
// TAB's own TL/BL corners with a radius comparable to (or bigger than) the
// tab's own height, this erodes the tab's material away from the join at
// the top and bottom, leaving it flush only somewhere in the middle - a
// concave notch that curves IN toward the tab's own centre, exactly
// backwards from a seamless flare. The owner's report ("the scallop curves
// in when it should curve out towards the card edge") is this bug.
//
// The fix keeps the erosion technique (still no new draw.c primitive) but
// points it the other way: cf_draw_join_bulge() below erodes an AUXILIARY
// box positioned diagonally OUTWARD from the tab's own join corner (up-left
// of the top corner, down-left of the bottom corner) instead of the tab's
// own corner box. The auxiliary box is filled with the tab's own surface
// style FIRST, then eroded so coverage is 1 (kept, tab-coloured) right next
// to the tab and fades to 0 (revealing the true background) at the box's
// OWN far corner, deep away from the tab. The result is a convex quarter-
// disk of tab material that bulges OUT of the tab's corner into the space
// beside it, tapering to nothing a short distance out - the tab's silhouette
// GROWS at the join instead of receding from it. Used for stowed/open card
// tabs, AND (cf_draw_side_summary() below) the maximize view's "N more
// cards" summary tabs, so the whole deck reads as one consistent tab family.
#define CF_TAB_R_OUTER 7    // matches the previous uniform glass radius
// (cfdock, scallop-fix) The join bulge now reaches OUTSIDE the tab's own
// rect, into territory this function has no geometry for (it only receives
// `r`, not the surrounding edge/neighbour bounds) - so it is kept modest
// (matching CF_TAB_R_OUTER's own scale, not the old 16px) to stay a small,
// safe accent rather than risk visibly overlapping an adjacent slot's own
// tab when many cards are stowed and edge columns are narrow.
#define CF_TAB_R_JOIN  9
// Erodes a jr x jr auxiliary box straddling (cx,cy) - the tab's own TL
// corner when top=1, its own BL corner when top=0 - so the bulge grows OUT
// of the tab (toward the card) instead of the tab's own corner receding IN.
// See the big comment above cf_draw_card_tab_flat() for the full rationale.
static void cf_draw_join_bulge(int32_t cx, int32_t cy, int32_t jr, int top) {
    if (jr <= 0) return;
    corner_capture_t cap;
    int32_t bx = cx - jr, by = cy - jr, bs = jr * 2;
    draw_round_corners_capture(&cap, bx, by, bs, bs, jr, top ? CORNER_TL : CORNER_BL);
    glass_or_flat(bx, top ? by : cy, jr, jr, GLASS_SURF_PANEL);
    draw_round_corners_restore(&cap);
}
static void cf_draw_card_tab_flat(cf_rect_t r) {
    corner_capture_t cap_outer;
    draw_round_corners_capture(&cap_outer, r.x, r.y, r.w, r.h, ui_px(CF_TAB_R_OUTER), CORNER_TR | CORNER_BR);

    glass_or_flat(r.x, r.y, r.w, r.h, GLASS_SURF_PANEL);
    draw_hline(r.x, r.y, r.w, 0xFFFFFFFFu);                 // top highlight
    draw_vline(r.x + r.w - 1, r.y, r.h, CLR_MENU_BORDER);   // right (outer/free side)
    draw_hline(r.x, r.y + r.h - 1, r.w, CLR_MENU_BORDER);   // bottom
    // (left/join side: deliberately no stroke - see the comment above)

    draw_round_corners_restore(&cap_outer);

    // (cfdock, scallop-fix) The join-side bulge, drawn OUTSIDE r - see the
    // comment above cf_draw_card_tab_flat().
    int32_t jr = ui_px(CF_TAB_R_JOIN);
    if (jr > r.h / 2) jr = r.h / 2;
    cf_draw_join_bulge(r.x, r.y,         jr, 1);   // top join corner: bulge up-left
    cf_draw_join_bulge(r.x, r.y + r.h,   jr, 0);   // bottom join corner: bulge down-left
}
// Dispatches to cfg_tab() (cardfile_glass.c) on a modern theme, the flat
// shape above otherwise - the ONE call site cf_draw_tab_plate()/
// cf_draw_side_summary() use, so both stay in lockstep.
static void cf_draw_tab_shape(cf_rect_t r, uint32_t card_color, int is_open, int dim_x10) {
    if (g_glass_enable) cfg_tab(r, card_color, is_open, dim_x10);
    else { (void)card_color; (void)is_open; (void)dim_x10; cf_draw_card_tab_flat(r); }
}

static cf_rect_t g_cf_plus_rect, g_cf_sort_rect;   // cached each render, for hit-test

static cf_rect_t g_cf_dockbtn_rect;   // cached each render, for hit-test (cfdock)

// (cfdock) Every dock's on-screen rect this frame, cached for the NEXT
// input tick's hit-test (the same one-frame-stale convention g_cf_plus_rect/
// gauge_hit() etc. already rely on elsewhere in this codebase).
static cf_rect_t g_cf_dock_rect[CF_MAX_DOCKS];
static uint32_t  g_cf_dock_id[CF_MAX_DOCKS];
static int       g_cf_dock_rect_n;

static void cf_draw_rail(const cf_geom_t *geom) {
    const int OX = geom->origin_x, OY = geom->origin_y;   // (cfdock)
    const int H = geom->screen_h;
    if (g_glass_enable) {   // (cfglass) glass rail: surface tint, sheen, luminous right border
        cfg_rail(OX, OY, geom->rail_w, H);
    } else {
        glass_or_flat(OX, OY, geom->rail_w, H, GLASS_SURF_PANEL);
        for (int y = OY; y < OY + H; y += ui_px(8)) draw_hline(OX, y, geom->rail_w, CLR_MENU_SEP);
        draw_vline(OX + geom->rail_w - 1, OY, H, CLR_MENU_BORDER);
    }

    cf_metrics_t m = cf_metrics();
    uint32_t surf = g_glass_enable ? cfg_surface_color() : CLR_MENU_BG;
    uint32_t ink  = readable_ink(surf);

    int px = OX + (geom->rail_w - m.plus_sz) / 2, py = OY + m.plus_y;
    g_cf_plus_rect = (cf_rect_t){ px, py, m.plus_sz, m.plus_sz };
    cf_plate(px, py, m.plus_sz, m.plus_sz);
    draw_text_centered(px + m.plus_sz / 2, py + m.plus_sz / 2 - ui_px(8), "+", ink);

    // (cfdock) "Docks" rail button, directly below "+": opens the dock
    // manage/add popup (CF_POP_DOCKLIST). A single glyph label, same idiom
    // as the "+" tab just above it - no new icon art needed.
    int dpy = py + m.plus_sz + ui_px(6);
    g_cf_dockbtn_rect = (cf_rect_t){ px, dpy, m.plus_sz, m.plus_sz };
    cf_plate(px, dpy, m.plus_sz, m.plus_sz);
    draw_text_centered(px + m.plus_sz / 2, dpy + m.plus_sz / 2 - ui_px(8), "D", ink);

    int sw = m.tab_w, sx = OX + (geom->rail_w - sw) / 2, sh = ui_px(56);
    int sy = OY + H - m.foot + ui_px(12);
    if (sy + sh > OY + H) sy = OY + H - sh - ui_px(4);
    g_cf_sort_rect = (cf_rect_t){ sx, sy, sw, sh };
    cf_plate(sx, sy, sw, sh);
    cf_icon_sort(sx + ui_px(4), sy + ui_px(4), sw - ui_px(8), ink);
    // (cfmaxwidth) centred, not pinned to the top: the icon owns a fixed
    // band at the top, the label centres in whatever remains below it.
    {
        int area_top = sy + ui_px(20);
        int cap = sh - ui_px(24);
        int run = cardtext_vertical_len("Sort", 12);
        if (run > cap) run = cap;
        if (cap > 0) cardtext_vertical(sx + sw / 2, cf_vcenter_top(area_top, cap, run), cap, "Sort", 12, ink);
    }
}

// -- one slot's stowed edge --------------------------------------------------
// (cfglass) `side` is cfg_edge_side(): which side the nearest open slot is
// on, so the glass spine knows where the shadow it receives falls.
static void cf_draw_edge(cf_rect_t r, uint32_t base, int dim_x10, int side) {
    if (g_glass_enable) { cfg_edge(r, base, dim_x10, side); return; }
    draw_fill_rect(r.x, r.y, r.w, r.h, cf_dim(base, dim_x10));
    draw_vline(r.x + r.w - 1, r.y, r.h, CLR_MENU_BORDER);
}

// -- one slot's sideways tab plate (stowed OR open) --------------------------
static void cf_draw_tab_plate(cf_slot_t *s, cf_rect_t plate, int is_open, int dim_x10,
                              int mx, int my, int drag_target_hl)
{
    cf_card_t *fc = cf_slot_focused_card(s);
    if (!fc) return;
    uint32_t base = cf_dim(cf_card_color(fc->color), is_open ? 0 : dim_x10);
    uint32_t ink  = readable_ink(base);

    cf_draw_tab_shape(plate, cf_card_color(fc->color), is_open, is_open ? 0 : dim_x10);   // (cfmaxwidth)

    if (drag_target_hl) {
        int full = s->ncards >= CF_MAX_CARDS_PER_SLOT;
        uint32_t hl = full ? 0xFFCC3333u : 0xFF3388CCu;
        draw_rect_outline(plate.x - 2, plate.y - 2, plate.w + 4, plate.h + 4, hl);
        draw_rect_outline(plate.x - 1, plate.y - 1, plate.w + 2, plate.h + 2, hl);
    }

    int label_bottom = plate.y + plate.h;
    if (is_open) label_bottom -= ui_px(CF_TAB_BTN_H) * CF_BTN_N;

    if (s->ncards > 1) {
        // group: a count pill at the top, then the focused member's sideways
        // label, then small colour ticks for the other members (a documented
        // simplification of the mockup's per-member nested sideways chips -
        // full per-member vertical sub-labels are deferred, see the note at
        // the top of this file).
        int py = plate.y + ui_px(3);
        char cnt[4]; int n = s->ncards, ci = 0;
        if (n >= 10) cnt[ci++] = (char)('0' + n / 10);
        cnt[ci++] = (char)('0' + n % 10); cnt[ci] = 0;
        int pillw = ui_px(18), pillh = ui_px(14);
        draw_rounded_rect(plate.x + (plate.w - pillw) / 2, py, pillw, pillh, ui_px(6), ink);
        draw_text_centered(plate.x + plate.w / 2, py + ui_px(2), cnt, base);
        py += pillh + ui_px(3);
        if (cf_slot_has_unread(s)) {
            draw_circle_filled(plate.x + plate.w - ui_px(6), plate.y + ui_px(4), ui_px(3), 0xFFCC0000u);
        }
        int lblcap = label_bottom - py - ui_px(2);
        if (lblcap > 0) {
            int run = cardtext_vertical_len(fc->title, 11);   // (cfmaxwidth) centred, not top-pinned
            if (run > lblcap) run = lblcap;
            cardtext_vertical(plate.x + plate.w / 2, cf_vcenter_top(py, lblcap, run), lblcap, fc->title, 11, ink);
        }
        // member ticks along the very bottom of the label area.
        int tickw = plate.w - ui_px(6);
        int ticky = label_bottom - ui_px(6);
        if (tickw > 4 && ticky > py) {
            int seg = tickw / s->ncards; if (seg < 2) seg = 2;
            for (int k = 0; k < s->ncards; k++) {
                uint32_t tc = cf_card_color(s->cards[k].color);
                int tx = plate.x + ui_px(3) + k * seg;
                draw_fill_rect(tx, ticky, seg > 2 ? seg - 1 : seg, ui_px(4),
                               s->cards[k].id == s->focus ? ink : tc);
            }
        }
    } else {
        if (fc->unread) draw_circle_filled(plate.x + plate.w - ui_px(6), plate.y + ui_px(6), ui_px(3), 0xFFCC0000u);
        int area_top = plate.y + ui_px(4);
        int lblcap = label_bottom - area_top;
        if (lblcap > 0) {
            int run = cardtext_vertical_len(fc->title, 12);   // (cfmaxwidth) centred, not top-pinned
            if (run > lblcap) run = lblcap;
            cardtext_vertical(plate.x + plate.w / 2, cf_vcenter_top(area_top, lblcap, run), lblcap, fc->title, 12, ink);
        }
    }

    if (is_open) {
        for (int b = 0; b < CF_BTN_N; b++) {
            cf_rect_t br = cf_tab_btn_rect(plate, b);
            int hover = cf_rect_hit(br, mx, my);
            uint32_t bg = hover ? CLR_MENU_ITEM_HOVER : base;
            if (g_glass_enable) cfg_tab_button(br, hover);   // (cfglass) translucent white pill over the plate
            else {
                draw_fill_rect(br.x, br.y, br.w, br.h, bg);
                draw_rect_outline(br.x, br.y, br.w, br.h, CLR_MENU_BORDER);
            }
            int32_t cx = br.x + br.w / 2, cy = br.y + br.h / 2;
            int r = ui_px(4);
            uint32_t bink = g_glass_enable ? ink : readable_ink(bg);
            switch (b) {
                case CF_BTN_COLOR: cf_icon_swatch(cx, cy, r, cf_card_color(fc->color), bink); break;
                case CF_BTN_DUP:   cf_icon_dup(cx, cy, r, bink); break;
                case CF_BTN_MAXIMIZE: {
                    // (cfmaxwidth) dimmed when there is nothing single to
                    // maximize (N-column mode) - the click still no-ops
                    // there (cf_maximize_toggle()'s own guard), this is
                    // purely the visual cue.
                    int can = (cf_open_count(&g_cf_deck) == 1);
                    uint32_t mink = can ? bink : cf_mix(bink, base, 55);
                    cf_icon_maximize(cx, cy, r, mink, g_cf_deck.maximized);
                    break;
                }
                case CF_BTN_FILE:  cf_icon_file(cx, cy, r, bink); break;
                case CF_BTN_CLOSE: cf_icon_close(cx, cy, r, hover ? 0xFFCC3333u : bink); break;
            }
        }
    }
}

// -- open single card: frame + caption foot (BODY LEFT FOR THE HOST WINDOW) -
static void cf_draw_single_body(cf_slot_t *s, cf_rect_t body, int is_focus) {
    cf_card_t *c = cf_slot_focused_card(s);
    if (!c) return;
    uint32_t base = cf_card_color(c->color);
    int frame = ui_px(CF_CARD_FRAME_PX);
    // Frame only - the interior (body.x+frame .. body.x+body.w-frame etc,
    // above the caption foot) is deliberately NOT filled: that rect is
    // cf_body_rect() and belongs to a hosted app window once cardfile_host.h
    // (agent C) exists. Until then it just shows whatever is already
    // composited underneath (wallpaper/other windows), matching the "render
    // the deck with placeholder card bodies" instruction.
    int foot_h = ui_px(CF_CARD_FOOT_PX);
    if (g_glass_enable) {   // (cfglass) glass frame ring + caption foot, interior untouched
        cfg_card_frame(body, base, frame, foot_h, is_focus);
    } else {
        draw_fill_rect(body.x, body.y, body.w, frame, base);                                    // top
        draw_fill_rect(body.x, body.y, frame, body.h, base);                                    // left
        draw_fill_rect(body.x + body.w - frame, body.y, frame, body.h, base);                   // right
        draw_fill_rect(body.x, body.y + body.h - foot_h, body.w, foot_h, base);                  // caption foot
        draw_rect_outline(body.x, body.y, body.w, body.h, is_focus ? readable_ink(base) : CLR_MENU_BORDER);
    }

    uint32_t ink = readable_ink(base), inkd = readable_ink_dim(base);
    int ty = body.y + body.h - foot_h + (foot_h - ui_px(10)) / 2;
    int tx = body.x + ui_px(10);
    draw_text(tx, ty, c->title, ink);
    tx += text_width(c->title) + ui_px(12);
    if (tx < body.x + body.w - ui_px(80)) draw_text(tx, ty, CF_CAT_NAME[c->cat >= 0 && c->cat < CF_CAT_COUNT ? c->cat : CF_CAT_ACCESSORIES], inkd);

    // resize grip: a short vertical tick straddling the right edge.
    int gy = body.y + body.h / 2 - ui_px(17);
    draw_vline(body.x + body.w - 1, gy, ui_px(34), readable_ink_dim(base));
}

static cf_rect_t cf_grip_rect(cf_rect_t body) {
    return (cf_rect_t){ body.x + body.w - ui_px(5), 0, ui_px(10), body.h };
}

// (cfhostinset, #404 follow-up) See cardfile.h's own comment for WHY these
// exist: cardfile_host.c places a hosted window here instead of at the raw
// body/pane rect, so an unclipped kernel window re-blit can never paint over
// the frame ring / caption foot / pane header this file draws around it.
// Same frame/foot/header constants cf_draw_single_body()/cf_draw_group_body()
// (and cardfile_glass.c's cfg_card_frame()/cfg_pane() on a glass theme) use -
// ONE definition, deliberately theme-uniform (a flat pane has no drawn ring
// today, but the window is inset from one anyway so both themes host
// identically and neither can silently drift from the other).
cf_rect_t cardfile_card_content_rect(cf_rect_t body) {
    int frame = ui_px(CF_CARD_FRAME_PX);
    int foot_h = ui_px(CF_CARD_FOOT_PX);
    cf_rect_t r;
    r.x = body.x + frame;
    r.y = body.y + frame;
    r.w = body.w - 2 * frame;
    r.h = body.h - frame - foot_h;
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

cf_rect_t cardfile_pane_content_rect(cf_rect_t pane) {
    int header = ui_px(CF_PANE_HEADER_PX);
    int ring = ui_px(CF_PANE_RING_PX);
    cf_rect_t r;
    r.x = pane.x + ring;
    r.y = pane.y + header;
    r.w = pane.w - 2 * ring;
    r.h = pane.h - header - ring;
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

// -- group-split body: pane frames + dividers --------------------------------
static void cf_draw_group_body(cf_deck_t *deck, cf_slot_t *s, cf_rect_t body, int is_focus, int mx, int my) {
    (void)deck;
    cf_rect_t panes[CF_MAX_CARDS_PER_SLOT];
    int n = cf_group_pane_rects(s, body, panes, CF_MAX_CARDS_PER_SLOT);
    for (int k = 0; k < n; k++) {
        cf_card_t *c = &s->cards[k];
        uint32_t base = cf_card_color(c->color);
        cf_rect_t p = panes[k];
        int focus = (c->id == s->focus);
        int ph = ui_px(CF_PANE_HEADER_PX);
        if (g_glass_enable) {   // (cfglass) header + 4px ring only: the hosted window sits inside
            cfg_pane(p, base, ph, focus);
        } else {
            draw_fill_rect(p.x, p.y, p.w, p.h, base);
            draw_rect_outline(p.x, p.y, p.w, p.h, focus ? readable_ink(base) : CLR_MENU_BORDER);
        }
        uint32_t ink = readable_ink(base);
        draw_circle_filled(p.x + ui_px(8), p.y + ph / 2, ui_px(3), cf_dim(base, 300));
        draw_text(p.x + ui_px(16), p.y + (ph - ui_px(10)) / 2, c->title, ink);
        for (int b = 0; b < CF_PANE_BTN_N; b++) {
            cf_rect_t br = cf_pane_btn_rect(p, b);
            int hover = cf_rect_hit(br, mx, my);
            uint32_t bg = hover ? CLR_MENU_ITEM_HOVER : base;
            draw_rect_outline(br.x, br.y, br.w, br.h, CLR_MENU_BORDER);
            int32_t cx = br.x + br.w / 2, cy = br.y + br.h / 2, r = ui_px(3);
            if (b == CF_PANE_BTN_EJECT) cf_icon_eject(cx, cy, r, readable_ink(bg));
            else cf_icon_close(cx, cy, r, hover ? 0xFFCC3333u : readable_ink(bg));
        }
    }
    (void)is_focus;
    // divider grip hints: a short tick centered on each internal boundary,
    // drawn on top of the two panes' shared border. Hit-testing recomputes
    // the same pane rects independently in cardfile_handle_mouse().
    {
        int idx = 0;
        for (int r = 0; r < s->nrows; r++) {
            int cols = s->row_cols[r];
            for (int c = 0; c < cols; c++, idx++) {
                if (c > 0) {
                    cf_rect_t right = panes[idx];
                    draw_vline(right.x, right.y + right.h / 2 - ui_px(12), ui_px(24), CLR_MENU_BORDER);
                }
            }
            if (r > 0) {
                cf_rect_t below = panes[idx - cols];
                draw_hline(below.x + below.w / 2 - ui_px(12), below.y, ui_px(24), CLR_MENU_BORDER);
            }
        }
    }
}

// ============================================================================
// (cfmaxwidth) MAXIMIZE VIEW: per-side "N more cards +" summary tabs.
//
// cf_layout() already collapses a side's stowed run to one shared edge_x
// when deck->{left,right}_collapsed - see its own comment - so ALL this
// section does is: (1) draw ONE summary widget (edge strip + tab, in the
// SAME seamless-join shape as a real card tab) where that side's fan would
// otherwise start, and (2) when a side is NOT collapsed (still inside a
// maximize view), draw a small fixed "collapse" affordance so there is
// always something to click back to the summary - the individual fan tabs
// occupy the reclaimed space in that state, so the summary tab itself is
// not on screen to reuse as the toggle.
// ============================================================================

// 1 if deck-order index i is a member of a currently-collapsed run (skip it
// entirely in the ordinary per-slot edge/tab loops - cf_draw_side_summary()
// draws the whole run's visuals once, from its first member).
static int cf_slot_collapsed_member(int i, int maxed, int open_i) {
    if (!maxed) return 0;
    if (i < open_i && g_cf_deck.left_collapsed) return 1;
    if (i > open_i && g_cf_deck.right_collapsed) return 1;
    return 0;
}

// "13 more cards +" / "1 more card +" - no sprintf (freestanding-safe),
// matching the digit-building pattern cf_duplicate_card() already uses.
static void cf_more_label(char *buf, unsigned long cap, int count) {
    char num[12];
    int n = count < 0 ? 0 : count, t = n, digits = 0;
    unsigned long w = 0;
    const char *rest = (n == 1) ? " more card +" : " more cards +";
    if (cap == 0) return;
    do { digits++; t /= 10; } while (t && digits < (int)sizeof(num) - 1);
    { int i2 = digits - 1, tt = n; if (tt == 0) num[0] = '0';
      while (tt && i2 >= 0) { num[i2] = (char)('0' + tt % 10); tt /= 10; i2--; } }
    num[digits] = 0;
    for (int k = 0; num[k] && w + 1 < cap; k++) buf[w++] = num[k];
    for (int k = 0; rest[k] && w + 1 < cap; k++) buf[w++] = rest[k];
    buf[w < cap ? w : cap - 1] = 0;
}

// The small fixed "collapse this side back to a summary tab" control, shown
// only while that side is EXPANDED within an active maximize view (while
// collapsed, the summary tab itself is the toggle - see cf_draw_side_summary()
// and its matching hit-test below). Sits in the margin above where that
// side's fan starts (geom->tab_top already reserves room before the first
// tab), so it never overlaps a real card tab.
static cf_rect_t cf_side_collapse_btn_rect(const cf_geom_t *geom, int edge_x) {
    cf_rect_t r;
    r.w = ui_px(16); r.h = ui_px(16);
    r.x = edge_x + (geom->edge_w - r.w) / 2;
    r.y = geom->tab_top - r.h - ui_px(4);
    if (r.y < ui_px(2)) r.y = ui_px(2);
    return r;
}

static void cf_draw_side_collapse_btn(cf_rect_t r, uint32_t surf, uint32_t ink) {
    cf_plate(r.x, r.y, r.w, r.h);
    // a small chevron pointing back toward the rail/spine - "collapse".
    int32_t cx = r.x + r.w / 2, cy = r.y + r.h / 2, k = ui_px(3);
    draw_line_aa(cx + k, cy - k, cx - k, cy, 1.3f, ink, 255);
    draw_line_aa(cx + k, cy + k, cx - k, cy, 1.3f, ink, 255);
    (void)surf;
}

// Draws the WHOLE summary widget for `side` (edge strip + seamlessly-joined
// tab + centred rotated "N more cards +" label) at the shared edge_x
// cf_layout() gave every member of that collapsed run. No-op if the side
// isn't actually collapsed or has nothing to summarize.
static void cf_draw_side_summary(int side, const cf_geom_t *geom, const cf_slot_layout_t *entry) {
    int count = cf_side_collapsed_count(&g_cf_deck, side);
    if (count <= 0) return;
    uint32_t surf = g_glass_enable ? cfg_surface_color() : CLR_MENU_BG;
    uint32_t ink = readable_ink(surf);

    cf_rect_t edge = cf_edge_rect(geom, entry);
    // Reuse the ordinary edge renderer at a shallow dist=1 dim (matches the
    // subtle depth cue a real dist=1 edge next to the open card would carry)
    // rather than a plain flat fill, so the summary reads as part of the
    // same fan, not a different kind of chrome.
    cf_draw_edge(edge, surf, 45, side == CF_SIDE_LEFT ? 1 : -1);

    char label[24];
    cf_more_label(label, sizeof(label), count);
    int run = cardtext_vertical_len(label, 12);
    cf_rect_t tab = cf_side_summary_tab_rect(geom, entry->edge_x, run);
    cf_draw_tab_shape(tab, surf, 0, 0);

    int pad = ui_px(4);
    int cap = tab.h - 2 * pad;
    int r2 = run; if (r2 > cap) r2 = cap;
    if (cap > 0) cardtext_vertical(tab.x + tab.w / 2, cf_vcenter_top(tab.y + pad, cap, r2), cap, label, 12, ink);
}

// ============================================================================
// TOP-LEVEL RENDER
// ============================================================================
// ============================================================================
// (cfdock) Dock bars: render + hosting. See cardfile_model.h's own "cfdock"
// note and docs/CARDFILE_ARCHITECTURE.md section 10 for the design.
// ============================================================================

// Which CF_DOCK_EDGE_* a dock's chrome (border side, resize-grip side)
// should draw as: its own edge for an EDGE dock, CF_DOCK_EDGE_LEFT (the
// documented convention - see cardfile_glass.h's cfg_dock_bar()) for a
// BETWEEN dock, which has no single screen edge of its own.
static int cf_dock_edge_for_style(const cf_dock_t *dk) {
    return (dk->pos.kind == CF_DOCK_POS_EDGE) ? dk->pos.edge : CF_DOCK_EDGE_LEFT;
}

// A short human-readable label for the Docks list popup: "Top edge" /
// "Bottom edge" / "Left edge" / "Right edge" / "Between card N".
static void cf_dock_label(const cf_dock_t *dk, char *buf, unsigned long cap) {
    (void)cap;   // callers pass a fixed-size stack buffer comfortably large enough
    if (dk->pos.kind == CF_DOCK_POS_EDGE) {
        static const char *names[CF_DOCK_EDGE_COUNT] = { "Top edge", "Bottom edge", "Left edge", "Right edge" };
        sprintf(buf, "%s", names[dk->pos.edge >= 0 && dk->pos.edge < CF_DOCK_EDGE_COUNT ? dk->pos.edge : 0]);
    } else {
        sprintf(buf, "Between card %d", dk->pos.slot_index + 1);
    }
}

// The deck-order index of deck->active_slot, or 0 if none is open - "Move
// between cards" from the dock context menu anchors just after this slot,
// the least surprising boundary when the user has a card open.
static int cf_active_slot_index(void) {
    if (g_cf_deck.active_slot == 0) return 0;
    for (int i = 0; i < g_cf_deck.nslots; i++) if (g_cf_deck.slots[i].id == g_cf_deck.active_slot) return i;
    return 0;
}

static void cf_draw_dock_bg(cf_rect_t r, int edge_style) {
    if (g_glass_enable) { cfg_dock_bar(r, edge_style); return; }
    glass_or_flat(r.x, r.y, r.w, r.h, GLASS_SURF_PANEL);
    switch (edge_style) {
        case CF_DOCK_EDGE_TOP:    draw_hline(r.x, r.y + r.h - 1, r.w, CLR_MENU_BORDER); break;
        case CF_DOCK_EDGE_BOTTOM: draw_hline(r.x, r.y,           r.w, CLR_MENU_BORDER); break;
        case CF_DOCK_EDGE_RIGHT:  draw_vline(r.x,           r.y, r.h, CLR_MENU_BORDER); break;
        default:                  draw_vline(r.x + r.w - 1, r.y, r.h, CLR_MENU_BORDER); break;   // LEFT + between
    }
}

// The resize-grip strip along a dock's INNER (desktop-facing) edge -
// CF_DOCK_GRIP_PX thick, spanning the bar's full length. Dragging it changes
// thickness (see cardfile_handle_mouse()'s CF_DRAG_DOCK_THICKNESS case).
#define CF_DOCK_GRIP_PX 6
static cf_rect_t cf_dock_grip_rect(cf_rect_t r, int edge_style) {
    int g = ui_px(CF_DOCK_GRIP_PX);
    switch (edge_style) {
        case CF_DOCK_EDGE_TOP:    return (cf_rect_t){ r.x, r.y + r.h - g, r.w, g };
        case CF_DOCK_EDGE_BOTTOM: return (cf_rect_t){ r.x, r.y, r.w, g };
        case CF_DOCK_EDGE_RIGHT:  return (cf_rect_t){ r.x, r.y, g, r.h };
        default:                  return (cf_rect_t){ r.x + r.w - g, r.y, g, r.h };   // LEFT + between
    }
}

// Renders every dock bar for this frame, refreshing g_cf_dock_rect[]/
// g_cf_dock_id[]/g_cf_dock_rect_n for the NEXT input tick's hit-test.
// `lay`/`n_lay` must be a fresh cf_layout_ex() of the CURRENT deck state with
// `dock_rects` filled (BETWEEN docks' rects come from that same pass - see
// cf_dock_rect()'s own comment on why this needs the layout, not just the
// screen size).
static void cf_draw_docks(const cf_geom_t *geom, const cf_rect_t *dock_rects) {
    g_cf_dock_rect_n = 0;
    for (int i = 0; i < g_cf_deck.ndocks; i++) {
        cf_dock_t *dk = &g_cf_deck.docks[i];
        cf_rect_t r = (dk->pos.kind == CF_DOCK_POS_EDGE)
            ? cf_dock_edge_rect(&g_cf_deck, dk, geom->screen_w_raw, geom->screen_h_raw)
            : dock_rects[i];
        if (r.w <= 0 || r.h <= 0) continue;   // (cfdock) hidden inside a collapsed maximize run this frame
        int edge_style = cf_dock_edge_for_style(dk);
        cf_draw_dock_bg(r, edge_style);
        draw_push_clip(r.x, r.y, r.w, r.h);
        uint32_t surf = g_glass_enable ? cfg_surface_color() : CLR_MENU_BG;
        taskbar_render_dock_items(r.x, r.y, r.w, r.h, dk->items, dk->nitems, surf,
                                   cf_dock_is_vertical(dk));
        draw_pop_clip();
        if (g_cf_dock_rect_n < CF_MAX_DOCKS) {
            g_cf_dock_rect[g_cf_dock_rect_n] = r;
            g_cf_dock_id[g_cf_dock_rect_n]   = dk->id;
            g_cf_dock_rect_n++;
        }
    }
}

void cardfile_render(void) {
    cf_ensure_init();
    if (!cardfile_active()) return;
    const int W = g_fb_width, H = g_fb_height;
    if (W <= 0 || H <= 0) return;
    cf_geom_t geom = cf_build_geom();

    cfg_begin_frame();   // (cfglass) one shared backdrop blur for every glass pane below
    cf_draw_rail(&geom);

    // (cfdock) Docks render regardless of card count (0..N docks is
    // independent of 0..N cards) - one cf_layout_ex() pass with a dock_out
    // buffer serves both this and (when nslots > 0) the ordinary deck walk
    // below, so a between-cards dock's reservation is computed exactly once
    // per frame.
    cf_rect_t dock_rects[CF_MAX_DOCKS];
    cf_slot_layout_t lay0[CF_MAX_SLOTS];
    cf_layout_ex(&g_cf_deck, &geom, lay0, CF_MAX_SLOTS, dock_rects);
    cf_draw_docks(&geom, dock_rects);

    if (g_cf_deck.nslots == 0) {
        uint32_t surf = g_glass_enable ? cfg_surface_color() : CLR_MENU_BG;
        uint32_t ink = readable_ink(surf);
        int cxc = geom.origin_x + geom.rail_w + (geom.screen_w - geom.rail_w) / 2;
        int cyc = geom.origin_y + geom.screen_h / 2;
        draw_text_centered(cxc, cyc - ui_px(18), "Cardfile", ink);
        draw_text_centered(cxc, cyc + ui_px(4), "Pick + on the rail to open an app as a card", ink);
    } else {
        cf_slot_layout_t lay[CF_MAX_SLOTS];
        int n = cf_layout_ex(&g_cf_deck, &geom, lay, CF_MAX_SLOTS, NULL);

        int open_idx[CF_MAX_SLOTS], n_open = 0;
        for (int i = 0; i < n; i++) if (lay[i].body_w > 0) open_idx[n_open++] = i;

        // (cfmaxwidth) is the maximize/collapse view actually in effect?
        int cf_open_i;
        int cf_maxed = cf_maximize_active(&g_cf_deck, &cf_open_i);

        // pass 1: edges (Z_EDGE_BODY). A collapsed run draws ONE summary
        // widget from its first member and is otherwise skipped entirely
        // (cf_layout() already gave every member of the run the SAME
        // edge_x/body_x, so drawing them individually would just overpaint
        // the same strip n times).
        for (int i = 0; i < n; i++) {
            if (cf_slot_collapsed_member(i, cf_maxed, cf_open_i)) {
                if (i == 0 || i == cf_open_i + 1) {
                    int side = (i == 0) ? CF_SIDE_LEFT : CF_SIDE_RIGHT;
                    cf_draw_side_summary(side, &geom, &lay[i]);
                }
                continue;
            }
            cf_slot_t *s = cf_find_slot(&g_cf_deck, lay[i].slot_id);
            if (!s) continue;
            cf_rect_t edge = cf_edge_rect(&geom, &lay[i]);
            int dist = cf_depth_distance(open_idx, n_open, i);
            cf_card_t *fc = cf_slot_focused_card(s);
            cf_draw_edge(edge, fc ? cf_card_color(fc->color) : CLR_MENU_BG, dist * 45,
                         cfg_edge_side(open_idx, n_open, i));
        }

        // (cfmaxwidth) a side that's part of an active maximize view but NOT
        // collapsed shows its ordinary individual fan (drawn by the normal
        // passes below, unchanged) PLUS a small fixed "collapse" affordance
        // in the margin above it, since the summary tab that would
        // otherwise be the toggle is not on screen in that state.
        if (cf_maxed) {
            uint32_t surf = g_glass_enable ? cfg_surface_color() : CLR_MENU_BG;
            uint32_t ink = readable_ink(surf);
            if (cf_open_i > 0 && !g_cf_deck.left_collapsed)
                cf_draw_side_collapse_btn(cf_side_collapse_btn_rect(&geom, lay[0].edge_x), surf, ink);
            if (cf_open_i + 1 < n && !g_cf_deck.right_collapsed)
                cf_draw_side_collapse_btn(cf_side_collapse_btn_rect(&geom, lay[cf_open_i + 1].edge_x), surf, ink);
        }

        // pass 2: open bodies (frame + caption / group panes)
        for (int i = 0; i < n; i++) {
            if (lay[i].body_w <= 0) continue;
            cf_slot_t *s = cf_find_slot(&g_cf_deck, lay[i].slot_id);
            if (!s) continue;
            cf_rect_t body = cf_body_rect(&geom, &lay[i]);
            int is_focus = (s->id == g_cf_deck.active_slot);
            if (s->ncards > 1) cf_draw_group_body(&g_cf_deck, s, body, is_focus, g_mouse_x, g_mouse_y);
            else cf_draw_single_body(s, body, is_focus);
        }

        // pass 3: stowed plates, farthest-from-open first (so nearer plates
        // paint over farther ones - the shingled staircase).
        for (int dist = CF_DEPTH_CAP; dist >= 0; dist--) {
            for (int i = 0; i < n; i++) {
                if (lay[i].body_w > 0) continue;
                if (cf_slot_collapsed_member(i, cf_maxed, cf_open_i)) continue;   // (cfmaxwidth) already summarized in pass 1
                if (cf_depth_distance(open_idx, n_open, i) != dist) continue;
                cf_slot_t *s = cf_find_slot(&g_cf_deck, lay[i].slot_id);
                if (!s) continue;
                cf_rect_t plate = cf_tab_rect(&geom, &lay[i], i, cf_plate_label_px(s), 0);
                int drop_hl = (g_cf_drag_kind == CF_DRAG_TAB && g_cf_drag_moved && g_cf_drag_target == s->id);
                cf_draw_tab_plate(s, plate, 0, dist * 45, g_mouse_x, g_mouse_y, drop_hl);
            }
        }
        // pass 4: open plate(s), always on top of stowed ones.
        for (int i = 0; i < n; i++) {
            if (lay[i].body_w <= 0) continue;
            cf_slot_t *s = cf_find_slot(&g_cf_deck, lay[i].slot_id);
            if (!s) continue;
            cf_rect_t plate = cf_tab_rect(&geom, &lay[i], i, cf_plate_label_px(s), 1);
            cf_draw_tab_plate(s, plate, 1, 0, g_mouse_x, g_mouse_y, 0);
        }
    }

    // drag ghost (Z_DRAG_GHOST)
    if (g_cf_drag_kind == CF_DRAG_TAB && g_cf_drag_moved) {
        cf_card_t *c = cf_find_card(&g_cf_deck, g_cf_drag_card, NULL);
        if (c) {
            int gw = ui_px(30), gh = ui_px(90);
            int gx = g_mouse_x - gw / 2, gy = g_mouse_y - ui_px(10);
            uint32_t base = cf_card_color(c->color);
            draw_fill_rect(gx, gy, gw, gh, base);
            draw_rect_outline(gx, gy, gw, gh, readable_ink(base));
            {   // (cfmaxwidth) centred, not top-pinned
                int pad = ui_px(6), cap = gh - 2 * pad;
                int run = cardtext_vertical_len(c->title, 11);
                if (run > cap) run = cap;
                if (cap > 0) cardtext_vertical(gx + gw / 2, cf_vcenter_top(gy + pad, cap, run), cap, c->title, 11, readable_ink(base));
            }
        }
    }

    // popups (Z_POPUP) - drawn last of all.
    cf_draw_popup();
}

// ============================================================================
// cardfile_host_tick() - the per-frame window-placement pass (cardfile_host.h,
// agent C, #404). Called ONCE per frame from main.c's render_frame_body(),
// BEFORE compositor_render_windows(), per that header's own contract: it
// reconciles pending launches (binds a win_id once a just-spawned window
// appears in the wm snapshot) and places/hides every hosted window to match
// the deck's CURRENT layout (open single card at its body rect, group members
// at their pane rects, column bodies at their column rects, everything else
// hidden). Non-blocking (#426): cf_host_apply() is a snapshot read plus a
// bounded number of wm_set_bounds() syscalls, no wait, no poll.
// ============================================================================
int cardfile_popup_active(void) { return g_cf_popup != CF_POP_NONE; }

void cardfile_host_tick(void) {
    cf_ensure_init();
    if (!cardfile_active()) return;
    if (g_cf_deck.nslots == 0) return;
    // (cfhostinset) A cardfile popup is a TRUE MODAL (close via ESC/an
    // explicit button only - see cardfile_handle_mouse()'s own comment) and
    // the "+" picker in particular is far wider than any frame inset could
    // compensate for, wide enough to sit entirely over an open card's
    // content rect. Rather than teach the placement math about popup
    // geometry, just hide every hosted window outright while one is open -
    // safe precisely because a true modal already refuses the window any
    // input, and it is the one placement no unclipped kernel re-blit can
    // then paint over, because there is nothing placed to re-blit.
    if (cardfile_popup_active()) { cf_host_hide_all(&g_cf_deck); return; }
    cf_geom_t geom = cf_build_geom();
    cf_host_apply(&g_cf_deck, &geom);
}

// ============================================================================
// Popup rendering (forward-declared above cardfile_render(), which calls it).
// ============================================================================
#define CF_POPROW_H   ui_px(20)

static void cf_draw_popup(void) {
    if (g_cf_popup == CF_POP_NONE) return;
    uint32_t surf = g_glass_enable ? cfg_surface_color() : CLR_MENU_BG;
    uint32_t ink = readable_ink(surf), inkd = readable_ink_dim(surf);

    if (g_cf_popup == CF_POP_PICKER) {
        int w = ui_px(260);
        int avail_h = g_fb_height - ui_px(40);
        int max_rows = avail_h / CF_POPROW_H; if (max_rows < 4) max_rows = 4;
        int rows_shown = g_cf_row_count < max_rows ? g_cf_row_count : max_rows;
        int h = ui_px(26) + rows_shown * CF_POPROW_H + ui_px(6);
        int x = cf_metrics().rail_w + ui_px(8), y = ui_px(10);
        g_cf_popup_rect = (cf_rect_t){ x, y, w, h };
        if (g_glass_enable) cfg_popup(x, y, w, h); else draw_popup_panel(x, y, w, h, ui_px(6));   // (cfglass)
        draw_text(x + ui_px(8), y + ui_px(6), "Open a task", ink);
        cf_rect_t closeb = { x + w - ui_px(22), y + ui_px(4), ui_px(16), ui_px(16) };
        draw_rect_outline(closeb.x, closeb.y, closeb.w, closeb.h, CLR_MENU_BORDER);
        cf_icon_close(closeb.x + closeb.w / 2, closeb.y + closeb.h / 2, ui_px(4), ink);
        int ly = y + ui_px(26);
        for (int i = 0; i < rows_shown; i++) {
            cf_pick_row_t *rw = &g_cf_rows[i];
            int ry = ly + i * CF_POPROW_H;
            if (rw->is_header) {
                draw_text(x + ui_px(8), ry + ui_px(4), CF_CAT_NAME[rw->app_idx], inkd);
            } else {
                int hov = (g_cf_popup_hover == i);
                if (hov) draw_fill_rect(x + ui_px(3), ry, w - ui_px(6), CF_POPROW_H, CLR_MENU_ITEM_HOVER);
                draw_rect_outline(x + ui_px(14), ry + ui_px(4), ui_px(10), ui_px(10),
                                  hov ? readable_ink(CLR_MENU_ITEM_HOVER) : inkd);
                draw_text(x + ui_px(30), ry + ui_px(4),
                         g_cf_apps[rw->app_idx].name, hov ? readable_ink(CLR_MENU_ITEM_HOVER) : ink);
            }
        }
        if (g_cf_row_count == 0) draw_text(x + ui_px(8), ly, "No apps found under /APPS", inkd);
        return;
    }

    if (g_cf_popup == CF_POP_SORT) {
        static const char *names[CF_SORT_COUNT] = {
            "Manual (creation order)", "Name", "Colour", "Category",
            "Most recently used", "Recently updated",
        };
        int w = ui_px(232), h = ui_px(10) + CF_SORT_COUNT * CF_POPROW_H + ui_px(6);
        cf_metrics_t m = cf_metrics();
        int x = m.rail_w + ui_px(8);
        int y = g_fb_height - m.foot - h; if (y < ui_px(8)) y = ui_px(8);
        g_cf_popup_rect = (cf_rect_t){ x, y, w, h };
        if (g_glass_enable) cfg_popup(x, y, w, h); else draw_popup_panel(x, y, w, h, ui_px(6));   // (cfglass)
        for (int i = 0; i < CF_SORT_COUNT; i++) {
            int ry = y + ui_px(6) + i * CF_POPROW_H;
            int hov = (g_cf_popup_hover == i);
            if (hov) draw_fill_rect(x + ui_px(3), ry, w - ui_px(6), CF_POPROW_H, CLR_MENU_ITEM_HOVER);
            uint32_t tink = hov ? readable_ink(CLR_MENU_ITEM_HOVER) : ink;
            draw_text(x + ui_px(10), ry + ui_px(4), names[i], tink);
            if (g_cf_deck.sort_key == i) {
                const char *arrow = (g_cf_deck.sort_dir > 0) ? "^" : "v";
                draw_text(x + w - ui_px(16), ry + ui_px(4), arrow, tink);
            }
        }
        return;
    }

    if (g_cf_popup == CF_POP_SWATCH) {
        int cell = ui_px(26), gap = ui_px(6), cols = 4;
        int rows = (CF_COLOR_COUNT + cols - 1) / cols;
        int w = cols * cell + (cols + 1) * gap;
        int h = rows * cell + (rows + 1) * gap + ui_px(16);
        int x = g_cf_popup_rect.x, y = g_cf_popup_rect.y;   // anchored by caller before open
        if (x + w > g_fb_width) x = g_fb_width - w - ui_px(4);
        if (y + h > g_fb_height) y = g_fb_height - h - ui_px(4);
        g_cf_popup_rect = (cf_rect_t){ x, y, w, h };
        if (g_glass_enable) cfg_popup(x, y, w, h); else draw_popup_panel(x, y, w, h, ui_px(6));   // (cfglass)
        cf_card_t *c = cf_find_card(&g_cf_deck, g_cf_swatch_card, NULL);
        for (int i = 0; i < CF_COLOR_COUNT; i++) {
            int cx = x + gap + (i % cols) * (cell + gap);
            int cy = y + gap + (i / cols) * (cell + gap);
            uint32_t col = cf_card_color(i);
            draw_fill_rect(cx, cy, cell, cell, col);
            int hov = (g_cf_popup_hover == i);
            draw_rect_outline(cx, cy, cell, cell, hov ? readable_ink(col) : CLR_MENU_BORDER);
            if (c && c->color == i) draw_circle_outline(cx + cell / 2, cy + cell / 2, ui_px(6), readable_ink(col));
        }
        if (c) draw_text(x + gap, y + h - ui_px(14), CF_PAL_NAME[c->color >= 0 && c->color < CF_COLOR_COUNT ? c->color : 0], inkd);
        return;
    }

    if (g_cf_popup == CF_POP_DOCKLIST) {
        int w = ui_px(220);
        int rows_shown = g_cf_deck.ndocks + 1;   // +1 for the "Add dock" row
        int h = ui_px(26) + rows_shown * CF_POPROW_H + ui_px(6);
        int x = cf_metrics().rail_w + ui_px(8), y = ui_px(10);
        g_cf_popup_rect = (cf_rect_t){ x, y, w, h };
        if (g_glass_enable) cfg_popup(x, y, w, h); else draw_popup_panel(x, y, w, h, ui_px(6));   // (cfglass)
        draw_text(x + ui_px(8), y + ui_px(6), "Dock bars", ink);
        int ly = y + ui_px(26);
        for (int i = 0; i < g_cf_deck.ndocks; i++) {
            int ry = ly + i * CF_POPROW_H;
            int hov = (g_cf_popup_hover == i);
            if (hov) draw_fill_rect(x + ui_px(3), ry, w - ui_px(6), CF_POPROW_H, CLR_MENU_ITEM_HOVER);
            char lbl[32]; cf_dock_label(&g_cf_deck.docks[i], lbl, sizeof(lbl));
            draw_text(x + ui_px(8), ry + ui_px(4), lbl, hov ? readable_ink(CLR_MENU_ITEM_HOVER) : ink);
            char th[8]; sprintf(th, "%dpx", g_cf_deck.docks[i].thickness);
            draw_text(x + w - ui_px(58), ry + ui_px(4), th, inkd);
            cf_icon_close(x + w - ui_px(14), ry + CF_POPROW_H / 2, ui_px(4), ink);
        }
        {
            int ry = ly + g_cf_deck.ndocks * CF_POPROW_H;
            int hov = (g_cf_popup_hover == g_cf_deck.ndocks);
            if (hov) draw_fill_rect(x + ui_px(3), ry, w - ui_px(6), CF_POPROW_H, CLR_MENU_ITEM_HOVER);
            draw_text(x + ui_px(8), ry + ui_px(4), "+ Add dock", hov ? readable_ink(CLR_MENU_ITEM_HOVER) : ink);
        }
        return;
    }

    if (g_cf_popup == CF_POP_DOCKCTX) {
        static const char *names[7] = {
            "Move to top edge", "Move to bottom edge", "Move to left edge",
            "Move to right edge", "Move between cards", "-", "Remove dock",
        };
        int nrows = (g_cf_deck.nslots > 0) ? 7 : 6;   // "between cards" needs at least one card
        int w = ui_px(200), h = ui_px(10) + nrows * CF_POPROW_H + ui_px(6);
        int x = g_cf_popup_rect.x, y = g_cf_popup_rect.y;
        if (x + w > g_fb_width) x = g_fb_width - w - ui_px(4);
        if (y + h > g_fb_height) y = g_fb_height - h - ui_px(4);
        g_cf_popup_rect = (cf_rect_t){ x, y, w, h };
        if (g_glass_enable) cfg_popup(x, y, w, h); else draw_popup_panel(x, y, w, h, ui_px(6));   // (cfglass)
        int ri = 0;
        for (int i = 0; i < 7; i++) {
            if (i == 4 && g_cf_deck.nslots == 0) continue;   // "between cards" hidden, empty deck
            int ry = y + ui_px(6) + ri * CF_POPROW_H;
            if (i == 5) { draw_hline(x + ui_px(4), ry + CF_POPROW_H / 2, w - ui_px(8), CLR_MENU_SEP); ri++; continue; }
            int hov = (g_cf_popup_hover == i);
            if (hov) draw_fill_rect(x + ui_px(3), ry, w - ui_px(6), CF_POPROW_H, CLR_MENU_ITEM_HOVER);
            draw_text(x + ui_px(10), ry + ui_px(4), names[i], hov ? readable_ink(CLR_MENU_ITEM_HOVER) : ink);
            ri++;
        }
        return;
    }
}

// ============================================================================
// INPUT
// ============================================================================

// Fresh layout + open-index snapshot, computed on demand (pure math, cheap).
static int cf_snapshot(cf_geom_t *geom, cf_slot_layout_t *lay, int cap, int *open_idx, int *n_open) {
    *geom = cf_build_geom();
    int n = cf_layout(&g_cf_deck, geom, lay, cap);
    *n_open = 0;
    for (int i = 0; i < n; i++) if (lay[i].body_w > 0) open_idx[*n_open] = i, (*n_open)++;
    return n;
}

static int cf_find_lay_index(const cf_slot_layout_t *lay, int n, uint32_t slot_id) {
    for (int i = 0; i < n; i++) if (lay[i].slot_id == slot_id) return i;
    return -1;
}

// ---- popup input ------------------------------------------------------------
static int cf_picker_hit_index(int x, int y) {
    if (!cf_rect_hit(g_cf_popup_rect, x, y)) return -1;
    int rel = y - (g_cf_popup_rect.y + ui_px(26));
    if (rel < 0) return -1;
    int idx = rel / CF_POPROW_H;
    int avail_h = g_fb_height - ui_px(40);
    int max_rows = avail_h / CF_POPROW_H; if (max_rows < 4) max_rows = 4;
    int shown = g_cf_row_count < max_rows ? g_cf_row_count : max_rows;
    if (idx < 0 || idx >= shown) return -1;
    return idx;
}

static int cf_handle_popup_mouse(int32_t x, int32_t y, int clicked) {
    if (g_cf_popup == CF_POP_NONE) return 0;
    if (g_cf_popup == CF_POP_PICKER) {
        int idx = cf_picker_hit_index(x, y);
        g_cf_popup_hover = idx;
        cf_rect_t closeb = { g_cf_popup_rect.x + g_cf_popup_rect.w - ui_px(22), g_cf_popup_rect.y + ui_px(4), ui_px(16), ui_px(16) };
        if (clicked) {
            if (cf_rect_hit(closeb, x, y)) { cf_popup_close(); return 1; }
            if (idx >= 0 && !g_cf_rows[idx].is_header) {
                cf_pick_app_t *a = &g_cf_apps[g_cf_rows[idx].app_idx];
                int keep = cf_mod_open_column();
                uint32_t sid = cf_add_card(&g_cf_deck, a->path, a->name, a->cat, CF_COLOR_DEFAULT,
                                           uptime_ms(), 1, keep);
                // cardfile_host (#404): spawn the app; win_id stays 0 until
                // cardfile_host_tick()'s cf_host_apply() binds it within a
                // few frames once its window appears (cfmaxwidth: gated by
                // the pre-launch snapshot exclusion cf_host_launch() records
                // on the card, so a pre-existing instance of the same app
                // can never be mistaken for the one this launch just made).
                if (sid) {
                    cf_slot_t *ns = cf_find_slot(&g_cf_deck, sid);
                    cf_card_t *nc = ns ? cf_slot_focused_card(ns) : NULL;
                    if (nc) cf_host_launch(&g_cf_deck, nc->id, a->path);
                }
                cf_popup_close();
            }
        }
        return 1;   // true modal: swallow every click while open
    }
    if (g_cf_popup == CF_POP_SORT) {
        int rel = y - (g_cf_popup_rect.y + ui_px(6));
        int idx = (rel >= 0) ? rel / CF_POPROW_H : -1;
        if (idx < 0 || idx >= CF_SORT_COUNT || !cf_rect_hit(g_cf_popup_rect, x, y)) idx = -1;
        g_cf_popup_hover = idx;
        if (clicked && idx >= 0) { cf_sort(&g_cf_deck, idx); cf_popup_close(); }
        return 1;
    }
    if (g_cf_popup == CF_POP_SWATCH) {
        int cell = ui_px(26), gap = ui_px(6), cols = 4;
        int hit = -1;
        for (int i = 0; i < CF_COLOR_COUNT; i++) {
            int cx = g_cf_popup_rect.x + gap + (i % cols) * (cell + gap);
            int cy = g_cf_popup_rect.y + gap + (i / cols) * (cell + gap);
            if (x >= cx && x < cx + cell && y >= cy && y < cy + cell) { hit = i; break; }
        }
        g_cf_popup_hover = hit;
        if (clicked && hit >= 0) { cf_set_color(&g_cf_deck, g_cf_swatch_card, hit); cf_popup_close(); }
        return 1;
    }
    if (g_cf_popup == CF_POP_DOCKLIST) {
        int idx = -1;
        if (cf_rect_hit(g_cf_popup_rect, x, y)) {
            int rel = y - (g_cf_popup_rect.y + ui_px(26));
            if (rel >= 0) {
                idx = rel / CF_POPROW_H;
                if (idx < 0 || idx > g_cf_deck.ndocks) idx = -1;
            }
        }
        g_cf_popup_hover = idx;
        if (clicked && idx >= 0) {
            if (idx == g_cf_deck.ndocks) {
                // "+ Add dock": a fresh BOTTOM edge dock, the classic
                // taskbar's own position, is the least surprising default.
                cf_dock_pos_t pos; pos.kind = CF_DOCK_POS_EDGE; pos.edge = CF_DOCK_EDGE_BOTTOM; pos.slot_index = 0;
                cf_dock_add(&g_cf_deck, pos, 0);
            } else {
                int rowx_close = g_cf_popup_rect.x + g_cf_popup_rect.w - ui_px(20);
                if (x >= rowx_close) cf_dock_remove(&g_cf_deck, g_cf_deck.docks[idx].id);
            }
            cf_popup_close();
        }
        return 1;
    }
    if (g_cf_popup == CF_POP_DOCKCTX) {
        int idx = -1;
        if (cf_rect_hit(g_cf_popup_rect, x, y)) {
            int rel = y - (g_cf_popup_rect.y + ui_px(6));
            if (rel >= 0) {
                int ri = rel / CF_POPROW_H, seen = 0;
                for (int i = 0; i < 7; i++) {
                    if (i == 4 && g_cf_deck.nslots == 0) continue;
                    if (seen == ri) { idx = i; break; }
                    seen++;
                }
            }
        }
        g_cf_popup_hover = (idx == 5) ? -1 : idx;   // 5 = separator, never selectable
        if (clicked && idx >= 0 && idx != 5) {
            cf_dock_t *dk = cf_dock_find(&g_cf_deck, g_cf_popup_dock);
            if (dk) {
                cf_dock_pos_t pos;
                switch (idx) {
                    case 0: pos.kind = CF_DOCK_POS_EDGE; pos.edge = CF_DOCK_EDGE_TOP;    cf_dock_set_pos(&g_cf_deck, dk->id, pos); break;
                    case 1: pos.kind = CF_DOCK_POS_EDGE; pos.edge = CF_DOCK_EDGE_BOTTOM; cf_dock_set_pos(&g_cf_deck, dk->id, pos); break;
                    case 2: pos.kind = CF_DOCK_POS_EDGE; pos.edge = CF_DOCK_EDGE_LEFT;   cf_dock_set_pos(&g_cf_deck, dk->id, pos); break;
                    case 3: pos.kind = CF_DOCK_POS_EDGE; pos.edge = CF_DOCK_EDGE_RIGHT;  cf_dock_set_pos(&g_cf_deck, dk->id, pos); break;
                    case 4: pos.kind = CF_DOCK_POS_BETWEEN; pos.slot_index = cf_active_slot_index() + 1;
                            cf_dock_set_pos(&g_cf_deck, dk->id, pos); break;
                    case 6: cf_dock_remove(&g_cf_deck, dk->id); break;
                }
            }
            cf_popup_close();
        }
        return 1;
    }
    return 0;
}

int cardfile_handle_key(int key) {
    cf_ensure_init();
    if (g_cf_popup != CF_POP_NONE && key == 0x1B /* ESC, matches main.c's process_events() convention */) {
        cf_popup_close(); return 1;
    }
    return 0;
}

// (cfdock) Right-click: opens the dock context menu (move/remove) when the
// press lands on a rendered dock bar (see cardfile.h's own comment - main.c
// calls this FIRST, mirroring taskbar_handle_right_click()'s convention).
int cardfile_handle_right_click(int32_t x, int32_t y) {
    cf_ensure_init();
    if (!cardfile_active()) return 0;
    if (g_cf_popup != CF_POP_NONE) return 0;   // a left-click/ESC modal is already up
    for (int di = 0; di < g_cf_dock_rect_n; di++) {
        if (!cf_rect_hit(g_cf_dock_rect[di], x, y)) continue;
        g_cf_popup = CF_POP_DOCKCTX;
        g_cf_popup_hover = -1;
        g_cf_popup_dock = g_cf_dock_id[di];
        g_cf_popup_rect = (cf_rect_t){ x, y, 0, 0 };   // anchor point; cf_draw_popup() sizes it
        return 1;
    }
    return 0;
}

// ---- main dispatch ----------------------------------------------------------
int cardfile_handle_mouse(int32_t x, int32_t y, int clicked, int scroll_delta) {
    (void)scroll_delta;   // picker does not scroll yet (documented limit, see report)
    cf_ensure_init();
    if (!cardfile_active()) return 0;

    if (g_cf_popup != CF_POP_NONE) return cf_handle_popup_mouse(x, y, clicked);

    int now_down = (g_mouse_buttons & 1) != 0;

    // (cfdock) cf_snapshot() (cf_build_geom()+cf_layout()) is safe to call
    // unconditionally - it handles an empty deck (nslots==0) fine, returning
    // n==0 and an untouched lay[] - so this is hoisted ABOVE the nslots==0
    // short-circuit below: docks exist independently of cards, and an
    // in-progress CF_DRAG_DOCK_THICKNESS must keep updating even on an
    // otherwise-empty deck.
    cf_geom_t geom; cf_slot_layout_t lay[CF_MAX_SLOTS]; int open_idx[CF_MAX_SLOTS], n_open;
    int n = cf_snapshot(&geom, lay, CF_MAX_SLOTS, open_idx, &n_open);
    uint64_t now_ms = uptime_ms();

    // ---- ongoing drag: update on move, commit on release ----
    if (g_cf_drag_kind != CF_DRAG_NONE) {
        if (!g_cf_drag_moved &&
            ((x - g_cf_drag_start_x) * (x - g_cf_drag_start_x) +
             (y - g_cf_drag_start_y) * (y - g_cf_drag_start_y)) > 36) {
            g_cf_drag_moved = 1;
        }
        if (now_down) {
            if (g_cf_drag_kind == CF_DRAG_DOCK_THICKNESS) {
                // (cfdock) delta direction depends on which side is the
                // INNER edge being dragged - see cf_dock_grip_rect()'s own
                // convention (matches cf_draw_dock_bg()'s border side).
                cf_dock_t *dk = cf_dock_find(&g_cf_deck, g_cf_drag_dock_id);
                if (dk) {
                    int edge_style = cf_dock_edge_for_style(dk);
                    int delta;
                    switch (edge_style) {
                        case CF_DOCK_EDGE_TOP:    delta =  (y - g_cf_drag_start_y); break;
                        case CF_DOCK_EDGE_BOTTOM: delta = -(y - g_cf_drag_start_y); break;
                        case CF_DOCK_EDGE_RIGHT:  delta = -(x - g_cf_drag_start_x); break;
                        default:                  delta =  (x - g_cf_drag_start_x); break;   // LEFT + between
                    }
                    cf_dock_set_thickness(&g_cf_deck, dk->id, g_cf_drag_size_a + delta);
                }
            } else if (g_cf_drag_kind == CF_DRAG_GRIP && g_cf_drag_moved) {
                int li = cf_find_lay_index(lay, n, g_cf_drag_slot);
                if (li >= 0) {
                    int w = cf_width_at_pointer(&g_cf_deck, &geom, lay, n, g_cf_drag_slot,
                                                x - lay[li].body_x);
                    cf_set_column_width(&g_cf_deck, g_cf_drag_slot, w);
                }
            } else if ((g_cf_drag_kind == CF_DRAG_VDIV || g_cf_drag_kind == CF_DRAG_HDIV) && g_cf_drag_moved) {
                int delta = (g_cf_drag_kind == CF_DRAG_VDIV) ? (x - g_cf_drag_start_x) : (y - g_cf_drag_start_y);
                if (g_cf_drag_kind == CF_DRAG_VDIV)
                    cf_set_col_divider(&g_cf_deck, g_cf_drag_slot, g_cf_drag_row, g_cf_drag_col,
                                       g_cf_drag_size_a, g_cf_drag_size_b, delta);
                else
                    cf_set_row_divider(&g_cf_deck, g_cf_drag_slot, g_cf_drag_row,
                                       g_cf_drag_size_a, g_cf_drag_size_b, delta);
            } else if (g_cf_drag_kind == CF_DRAG_TAB && g_cf_drag_moved) {
                // drop target: another slot's plate or edge under the pointer
                uint32_t target = 0;
                for (int i = 0; i < n; i++) {
                    if (lay[i].slot_id == g_cf_drag_slot) continue;
                    cf_slot_t *ts = cf_find_slot(&g_cf_deck, lay[i].slot_id);
                    if (!ts) continue;
                    cf_rect_t plate = cf_tab_rect(&geom, &lay[i], i, cf_plate_label_px(ts),
                                                  lay[i].body_w > 0);
                    cf_rect_t edge = cf_edge_rect(&geom, &lay[i]);
                    if (cf_rect_hit(plate, x, y) || cf_rect_hit(edge, x, y)) { target = ts->id; break; }
                }
                g_cf_drag_target = target;
            }
            return 1;
        }
        // release
        cf_slot_t *src_slot; cf_card_t *pc = cf_find_card(&g_cf_deck, g_cf_drag_card, &src_slot);
        cf_drag_kind_t kind = g_cf_drag_kind;
        int moved = g_cf_drag_moved;
        uint32_t target = g_cf_drag_target;
        uint32_t press_slot = g_cf_drag_slot;
        g_cf_drag_kind = CF_DRAG_NONE; g_cf_drag_moved = 0; g_cf_drag_target = 0; 
        if (kind == CF_DRAG_TAB && pc && src_slot) {
            int was_member_chip = (src_slot->ncards > 1);
            if (!moved) {
                if (was_member_chip) {
                    cf_set_focus(&g_cf_deck, src_slot->id, pc->id);
                    if (!src_slot->open) {
                        if (cf_mod_open_column()) cf_open_second_as_column(&g_cf_deck, src_slot->id, 0, now_ms);
                        else cf_open_single(&g_cf_deck, src_slot->id, now_ms);
                    }
                } else if (src_slot->open && !cf_mod_open_column()) {
                    cf_stow_slot(&g_cf_deck, src_slot->id);
                } else {
                    if (cf_mod_open_column()) cf_open_second_as_column(&g_cf_deck, src_slot->id, 0, now_ms);
                    else cf_open_single(&g_cf_deck, src_slot->id, now_ms);
                }
            } else if (target) {
                cf_group_card_into(&g_cf_deck, pc->id, target, now_ms);
            } else {
                int li = cf_find_lay_index(lay, n, press_slot);
                int want_w = (li >= 0) ? (x - lay[li].body_x) : 0;
                if (want_w > 60) {
                    if (was_member_chip) {
                        cf_ungroup_pull_out(&g_cf_deck, pc->id, 1, want_w, now_ms);
                    } else if (!src_slot->open) {
                        cf_open_second_as_column(&g_cf_deck, src_slot->id, want_w, now_ms);
                    } else {
                        cf_set_column_width(&g_cf_deck, src_slot->id, want_w);
                    }
                }
                // else: dropped back near its own edge - no-op (cancel).
            }
        }
        return 1;
    }

    // ---- rail: "+", "Docks", and Sort ----
    if (clicked && cf_rect_hit(g_cf_plus_rect, x, y)) { cf_picker_open(); return 1; }
    if (clicked && cf_rect_hit(g_cf_dockbtn_rect, x, y)) {
        g_cf_popup = CF_POP_DOCKLIST; g_cf_popup_hover = -1; return 1;
    }
    if (clicked && cf_rect_hit(g_cf_sort_rect, x, y)) { g_cf_popup = CF_POP_SORT; g_cf_popup_hover = -1; return 1; }

    // (cfdock) Dock bars claim every PRESS inside their own rect outright -
    // BEFORE the nslots==0 short-circuit below (docks exist independently of
    // cards) and before the ordinary per-card hit-tests further down: a
    // dock's rect is reserved space no card ever draws into (cf_build_geom()/
    // cf_layout_ex() - see their own comments), so there is no ambiguity to
    // resolve between a dock and a card here.
    if (clicked) {
        for (int di = 0; di < g_cf_dock_rect_n; di++) {
            if (!cf_rect_hit(g_cf_dock_rect[di], x, y)) continue;
            cf_dock_t *dk = cf_dock_find(&g_cf_deck, g_cf_dock_id[di]);
            if (!dk) return 1;
            int edge_style = cf_dock_edge_for_style(dk);
            cf_rect_t grip = cf_dock_grip_rect(g_cf_dock_rect[di], edge_style);
            if (cf_rect_hit(grip, x, y)) {
                g_cf_drag_kind = CF_DRAG_DOCK_THICKNESS;
                g_cf_drag_dock_id = dk->id;
                g_cf_drag_start_x = x; g_cf_drag_start_y = y; g_cf_drag_moved = 0;
                g_cf_drag_size_a = dk->thickness;
                return 1;
            }
            taskbar_dock_items_handle_mouse(x, y, clicked, 0, cf_dock_is_vertical(dk));
            return 1;   // claim the click regardless (dock background, no sub-item hit)
        }
    }

    if (g_cf_deck.nslots == 0) return 0;   // empty deck: only the rail/docks are live

    // ---- no drag in progress: hit-test a fresh press ----
    if (!clicked) return 0;

    // (cfmaxwidth) maximize view: summary-tab / collapse-button clicks take
    // priority over the ordinary tab/edge loops below (which skip collapsed
    // members entirely - see cf_slot_collapsed_member()). A collapsed side's
    // click target is the WHOLE summary edge strip (forgiving, same parity
    // as clicking any other stowed edge to open it); an expanded side's is
    // the small fixed collapse button drawn above its fan.
    int cf_maxed_open_i;
    int cf_maxed = cf_maximize_active(&g_cf_deck, &cf_maxed_open_i);
    if (cf_maxed) {
        if (cf_maxed_open_i > 0) {
            if (g_cf_deck.left_collapsed) {
                if (cf_rect_hit(cf_edge_rect(&geom, &lay[0]), x, y)) {
                    cf_toggle_side_collapse(&g_cf_deck, CF_SIDE_LEFT); return 1;
                }
            } else if (cf_rect_hit(cf_side_collapse_btn_rect(&geom, lay[0].edge_x), x, y)) {
                cf_toggle_side_collapse(&g_cf_deck, CF_SIDE_LEFT); return 1;
            }
        }
        if (cf_maxed_open_i + 1 < n) {
            if (g_cf_deck.right_collapsed) {
                if (cf_rect_hit(cf_edge_rect(&geom, &lay[cf_maxed_open_i + 1]), x, y)) {
                    cf_toggle_side_collapse(&g_cf_deck, CF_SIDE_RIGHT); return 1;
                }
            } else if (cf_rect_hit(cf_side_collapse_btn_rect(&geom, lay[cf_maxed_open_i + 1].edge_x), x, y)) {
                cf_toggle_side_collapse(&g_cf_deck, CF_SIDE_RIGHT); return 1;
            }
        }
    }

    // buttons on every OPEN plate (highest priority: they sit above the body/edge)
    for (int oi = 0; oi < n_open; oi++) {
        int i = open_idx[oi];
        cf_slot_t *s = cf_find_slot(&g_cf_deck, lay[i].slot_id);
        if (!s) continue;
        cf_rect_t plate = cf_tab_rect(&geom, &lay[i], i, cf_plate_label_px(s), 1);
        for (int b = 0; b < CF_BTN_N; b++) {
            cf_rect_t br = cf_tab_btn_rect(plate, b);
            if (!cf_rect_hit(br, x, y)) continue;
            cf_card_t *fc = cf_slot_focused_card(s);
            switch (b) {
                case CF_BTN_CLOSE: cf_close_slot(&g_cf_deck, s->id, now_ms); break;
                case CF_BTN_FILE:  cf_stow_slot(&g_cf_deck, s->id); break;
                case CF_BTN_MAXIMIZE: cf_maximize_toggle(&g_cf_deck); break;   // (cfmaxwidth) no-op in N-column mode
                case CF_BTN_DUP:
                    if (fc) {
                        uint32_t nsid = cf_duplicate_card(&g_cf_deck, fc->id, now_ms);
                        // cardfile_host (#404): every member of the freshly
                        // duplicated slot starts at win_id 0 (cf_duplicate_card's
                        // own contract) - launch a real window for each so the
                        // copy is not just a model entry.
                        cf_slot_t *ndup = nsid ? cf_find_slot(&g_cf_deck, nsid) : NULL;
                        if (ndup) {
                            for (int k = 0; k < ndup->ncards; k++) {
                                cf_host_launch(&g_cf_deck, ndup->cards[k].id, ndup->cards[k].app_path);
                            }
                        }
                    }
                    break;
                case CF_BTN_COLOR:
                    if (fc) {
                        g_cf_swatch_card = fc->id;
                        g_cf_popup_rect = (cf_rect_t){ plate.x + plate.w + ui_px(4), plate.y, 0, 0 };
                        g_cf_popup = CF_POP_SWATCH; g_cf_popup_hover = -1;
                    }
                    break;
            }
            return 1;
        }
        // group pane header buttons
        if (s->ncards > 1) {
            cf_rect_t body = cf_body_rect(&geom, &lay[i]);
            cf_rect_t panes[CF_MAX_CARDS_PER_SLOT];
            int npn = cf_group_pane_rects(s, body, panes, CF_MAX_CARDS_PER_SLOT);
            for (int k = 0; k < npn; k++) {
                for (int pb = 0; pb < CF_PANE_BTN_N; pb++) {
                    cf_rect_t br = cf_pane_btn_rect(panes[k], pb);
                    if (!cf_rect_hit(br, x, y)) continue;
                    if (pb == CF_PANE_BTN_CLOSE) cf_close_card(&g_cf_deck, s->cards[k].id, now_ms);
                    else cf_ungroup_pull_out(&g_cf_deck, s->cards[k].id, 1, 0, now_ms);
                    return 1;
                }
                int ph = ui_px(CF_PANE_HEADER_PX);
                cf_rect_t hdr = { panes[k].x, panes[k].y, panes[k].w, ph };
                if (cf_rect_hit(hdr, x, y)) { cf_set_focus(&g_cf_deck, s->id, s->cards[k].id); return 1; }
            }
            // group row/col dividers. row_start[r] is captured BEFORE each
            // row's inner loop so the horizontal (row-to-row) check below can
            // reference the first pane of each row without re-deriving it
            // from a post-loop idx (that off-by-one - "below" landing one row
            // past the real boundary and "above" landing one row short - was
            // caught in review before this ever reached a build).
            int rows = s->nrows;
            int idx = 0;
            int row_start[CF_MAX_GRID_ROWS];
            for (int r = 0; r < rows; r++) {
                row_start[r] = idx;
                int cols = s->row_cols[r];
                for (int c = 0; c < cols; c++, idx++) {
                    if (c > 0) {
                        cf_rect_t left = panes[idx - 1], right = panes[idx];
                        cf_rect_t div = { right.x - ui_px(CF_DIVIDER_HIT) / 2, right.y, ui_px(CF_DIVIDER_HIT), right.h };
                        if (cf_rect_hit(div, x, y)) {
                            g_cf_drag_kind = CF_DRAG_VDIV; g_cf_drag_slot = s->id;
                            g_cf_drag_row = r; g_cf_drag_col = c;
                            g_cf_drag_size_a = left.w; g_cf_drag_size_b = right.w;
                            g_cf_drag_start_x = x; g_cf_drag_start_y = y; g_cf_drag_moved = 0;
                            return 1;
                        }
                    }
                }
            }
            for (int r = 1; r < rows; r++) {
                cf_rect_t above = panes[row_start[r - 1]], below = panes[row_start[r]];
                cf_rect_t div = { below.x, below.y - ui_px(CF_DIVIDER_HIT) / 2, below.w, ui_px(CF_DIVIDER_HIT) };
                if (cf_rect_hit(div, x, y)) {
                    g_cf_drag_kind = CF_DRAG_HDIV; g_cf_drag_slot = s->id;
                    g_cf_drag_row = r;
                    g_cf_drag_size_a = above.h; g_cf_drag_size_b = below.h;
                    g_cf_drag_start_x = x; g_cf_drag_start_y = y; g_cf_drag_moved = 0;
                    return 1;
                }
            }
        } else {
            // single-card open slot: resize grip
            cf_rect_t body = cf_body_rect(&geom, &lay[i]);
            cf_rect_t grip = cf_grip_rect(body);
            if (cf_rect_hit(grip, x, y)) {
                g_cf_drag_kind = CF_DRAG_GRIP; g_cf_drag_slot = s->id;
                g_cf_drag_start_x = x; g_cf_drag_start_y = y; g_cf_drag_moved = 0;
                return 1;
            }
        }
    }

    // tab plates + member chips (any slot, stowed or open) - start a tab drag
    for (int i = n - 1; i >= 0; i--) {   // topmost (closest to open) first
        if (cf_slot_collapsed_member(i, cf_maxed, cf_maxed_open_i)) continue;   // (cfmaxwidth) handled above
        cf_slot_t *s = cf_find_slot(&g_cf_deck, lay[i].slot_id);
        if (!s) continue;
        int is_open = lay[i].body_w > 0;
        cf_rect_t plate = cf_tab_rect(&geom, &lay[i], i, cf_plate_label_px(s), is_open);
        if (!cf_rect_hit(plate, x, y)) continue;
        uint32_t press_card = cf_slot_focused_card(s)->id;
        // (member-chip sub-hit-testing is approximated: dragging any point on
        // a group's plate acts on its currently-focused member, matching the
        // "click a member chip to focus + open" case; per-pixel chip
        // targeting is part of the deferred nested-chip polish noted above.)
        g_cf_drag_kind = CF_DRAG_TAB; g_cf_drag_slot = s->id; g_cf_drag_card = press_card;
        g_cf_drag_start_x = x; g_cf_drag_start_y = y; g_cf_drag_moved = 0; g_cf_drag_target = 0;
        return 1;
    }

    // edges (click a stowed slot to open it)
    for (int i = 0; i < n; i++) {
        if (lay[i].body_w > 0) continue;   // already open, handled above
        if (cf_slot_collapsed_member(i, cf_maxed, cf_maxed_open_i)) continue;   // (cfmaxwidth) handled above
        cf_rect_t edge = cf_edge_rect(&geom, &lay[i]);
        if (!cf_rect_hit(edge, x, y)) continue;
        if (cf_mod_open_column()) cf_open_second_as_column(&g_cf_deck, lay[i].slot_id, 0, now_ms);
        else cf_open_single(&g_cf_deck, lay[i].slot_id, now_ms);
        return 1;
    }

    return 0;   // inside an open body: let the caller forward it to the hosted window
}

// ============================================================================
// (cfmaxwidth) TESTHOOK-only verification hooks. See cardfile.h's own
// comment: NEVER compiled into the shipping COMPOSIT (MAYTERA_TESTHOOK is
// only ever defined by `make TESTHOOK=1`, never build-golden.sh's plain
// `make`/`make install`).
// ============================================================================
#ifdef MAYTERA_TESTHOOK
#include "../../libc/stdio.h"   // snprintf, TESTHOOK-only use

cf_deck_t *cardfile_debug_deck(void) { cf_ensure_init(); return &g_cf_deck; }

void cardfile_debug_dump(char *buf, unsigned long cap) {
    int w = 0, i, open_i, maxed;
    cf_geom_t geom;
    cf_slot_layout_t lay[CF_MAX_SLOTS];
    int n;
    if (!buf || cap == 0) return;
    cf_ensure_init();
    geom = cf_build_geom();
    n = cf_layout(&g_cf_deck, &geom, lay, CF_MAX_SLOTS);
    maxed = cf_maximize_active(&g_cf_deck, &open_i);

    w += snprintf(buf + w, cap - w,
                  "n=%d rail=%d edge=%d tabw=%d tabtop=%d step=%d scr=%dx%d max=%d L=%d R=%d",
                  n, geom.rail_w, geom.edge_w, geom.tab_w, geom.tab_top, geom.tab_step,
                  geom.screen_w, geom.screen_h, g_cf_deck.maximized,
                  g_cf_deck.left_collapsed, g_cf_deck.right_collapsed);
    for (i = 0; i < n && w < (int)cap; i++) {
        w += snprintf(buf + w, cap - w, " | s%d:id=%u ex=%d bx=%d bw=%d",
                      i, lay[i].slot_id, lay[i].edge_x, lay[i].body_x, lay[i].body_w);
    }
    // The first open slot's grip / maximize-button / plate rects (the
    // common single-open-card verification scenario).
    for (i = 0; i < n && w < (int)cap; i++) {
        cf_slot_t *s;
        cf_rect_t body, grip, plate, maxbtn;
        if (lay[i].body_w <= 0) continue;
        s = cf_find_slot(&g_cf_deck, lay[i].slot_id);
        if (!s) break;
        body = cf_body_rect(&geom, &lay[i]);
        grip = cf_grip_rect(body);
        plate = cf_tab_rect(&geom, &lay[i], i, cf_plate_label_px(s), 1);
        maxbtn = cf_tab_btn_rect(plate, CF_BTN_MAXIMIZE);
        w += snprintf(buf + w, cap - w,
                      " | grip=%d,%d,%d,%d maxbtn=%d,%d,%d,%d plate=%d,%d,%d,%d",
                      grip.x, grip.y, grip.w, grip.h,
                      maxbtn.x, maxbtn.y, maxbtn.w, maxbtn.h,
                      plate.x, plate.y, plate.w, plate.h);
        break;
    }
    if (maxed && w < (int)cap) {
        if (open_i > 0) {
            cf_rect_t r = g_cf_deck.left_collapsed ? cf_edge_rect(&geom, &lay[0])
                                                    : cf_side_collapse_btn_rect(&geom, lay[0].edge_x);
            w += snprintf(buf + w, cap - w, " | left=%d,%d,%d,%d", r.x, r.y, r.w, r.h);
        }
        if (open_i + 1 < n && w < (int)cap) {
            cf_rect_t r = g_cf_deck.right_collapsed ? cf_edge_rect(&geom, &lay[open_i + 1])
                                                     : cf_side_collapse_btn_rect(&geom, lay[open_i + 1].edge_x);
            w += snprintf(buf + w, cap - w, " | right=%d,%d,%d,%d", r.x, r.y, r.w, r.h);
        }
    }
}

// The three helpers below all drive the REAL cardfile_handle_mouse()
// dispatch (the exact function CFCLICK/CFDOWN/CFMOVE/CFUP call) - they are
// NOT private setters - but compute the target rect THEMSELVES from this
// file's own geometry helpers, so a SEQ script does not need pixel-exact
// coordinates precomputed by hand (which would drift the moment a metric
// changed) just to reach a resize grip, a button 3-4 buttons deep in a
// plate, or a side widget whose position depends on live collapse state.

// Full press-move-release grip drag on the first OPEN slot found, targeting
// column width `target_w`. Returns 1 if a grip was found and dragged, 0
// otherwise (nothing open).
int cardfile_debug_grip_drag(int target_w) {
    cf_geom_t geom;
    cf_slot_layout_t lay[CF_MAX_SLOTS];
    int n, i;
    cf_ensure_init();
    geom = cf_build_geom();
    n = cf_layout(&g_cf_deck, &geom, lay, CF_MAX_SLOTS);
    for (i = 0; i < n; i++) {
        cf_rect_t body, grip;
        int32_t gx, gy, tx;
        int saved_buttons;
        if (lay[i].body_w <= 0) continue;
        body = cf_body_rect(&geom, &lay[i]);
        grip = cf_grip_rect(body);
        gx = grip.x + grip.w / 2; gy = grip.y + grip.h / 2;
        tx = body.x + target_w;
        saved_buttons = g_mouse_buttons;
        g_mouse_buttons |= 1;
        cardfile_handle_mouse(gx, gy, 1, 0);   /* press on the grip */
        cardfile_handle_mouse(tx, gy, 0, 0);   /* move to the target width */
        g_mouse_buttons &= ~1;
        cardfile_handle_mouse(tx, gy, 0, 0);   /* release, committing the width */
        g_mouse_buttons = saved_buttons;
        return 1;
    }
    return 0;
}

// A full click on the first open slot's MAXIMIZE button. Returns 1 if an
// open slot was found (the click may still no-op per cf_maximize_toggle()'s
// own N-column guard - that is the behaviour under test, not a failure
// here), 0 if nothing is open at all.
int cardfile_debug_click_maximize(void) {
    cf_geom_t geom;
    cf_slot_layout_t lay[CF_MAX_SLOTS];
    int n, i;
    cf_ensure_init();
    geom = cf_build_geom();
    n = cf_layout(&g_cf_deck, &geom, lay, CF_MAX_SLOTS);
    for (i = 0; i < n; i++) {
        cf_slot_t *s;
        cf_rect_t plate, btn;
        if (lay[i].body_w <= 0) continue;
        s = cf_find_slot(&g_cf_deck, lay[i].slot_id);
        if (!s) continue;
        plate = cf_tab_rect(&geom, &lay[i], i, cf_plate_label_px(s), 1);
        btn = cf_tab_btn_rect(plate, CF_BTN_MAXIMIZE);
        cardfile_handle_mouse(btn.x + btn.w / 2, btn.y + btn.h / 2, 1, 0);
        cardfile_handle_mouse(btn.x + btn.w / 2, btn.y + btn.h / 2, 0, 0);
        return 1;
    }
    return 0;
}

// A click on `side`'s maximize-view widget: the summary edge (collapsed) or
// the small collapse button (expanded) - whichever is CURRENTLY showing, so
// one verb covers both directions of the toggle. Returns 1 if the maximize
// view is active and that side has something to click, 0 otherwise.
int cardfile_debug_click_side(int side) {
    cf_geom_t geom;
    cf_slot_layout_t lay[CF_MAX_SLOTS];
    int n, open_i, maxed;
    cf_rect_t r;
    int32_t cx, cy;
    cf_ensure_init();
    geom = cf_build_geom();
    n = cf_layout(&g_cf_deck, &geom, lay, CF_MAX_SLOTS);
    maxed = cf_maximize_active(&g_cf_deck, &open_i);
    if (!maxed) return 0;
    if (side == CF_SIDE_LEFT) {
        if (open_i <= 0) return 0;
        r = g_cf_deck.left_collapsed ? cf_edge_rect(&geom, &lay[0])
                                      : cf_side_collapse_btn_rect(&geom, lay[0].edge_x);
    } else {
        if (open_i + 1 >= n) return 0;
        r = g_cf_deck.right_collapsed ? cf_edge_rect(&geom, &lay[open_i + 1])
                                       : cf_side_collapse_btn_rect(&geom, lay[open_i + 1].edge_x);
    }
    cx = r.x + r.w / 2; cy = r.y + r.h / 2;
    cardfile_handle_mouse(cx, cy, 1, 0);
    cardfile_handle_mouse(cx, cy, 0, 0);
    return 1;
}

// Opens the slot at deck-order position `idx` as the sole open slot, via
// cf_open_single() directly - not a mouse click, deliberately: this is pure
// setup (reaching a known starting deck state after CFSEED), not itself
// part of any feature under test, so it does not need to prove a hit-test.
// Returns 1 if idx was in range.
int cardfile_debug_open_index(int idx) {
    cf_ensure_init();
    if (idx < 0 || idx >= g_cf_deck.nslots) return 0;
    return cf_open_single(&g_cf_deck, g_cf_deck.slots[idx].id, uptime_ms());
}

// (cfmaxwidth) Diagnostic: every card's app_path, win_id and launch_floor_id
// - so a SEQ script can tell, from serial, whether a card ever got bound at
// all and to what, without guessing from a screendump.
void cardfile_debug_wins(char *buf, unsigned long cap) {
    int i, k, w = 0;
    cf_ensure_init();
    if (!buf || cap == 0) return;
    for (i = 0; i < g_cf_deck.nslots && w < (int)cap; i++) {
        cf_slot_t *s = &g_cf_deck.slots[i];
        for (k = 0; k < s->ncards && w < (int)cap; k++) {
            cf_card_t *c = &s->cards[k];
            w += snprintf(buf + w, cap - w, "%s%s:id=%u win=%u floor=%u",
                          (i == 0 && k == 0) ? "" : " | ", c->app_path, c->id,
                          c->win_id, c->launch_floor_id);
        }
    }
}

// (cfdock) TESTHOOK-only direct dock model manipulation - same "setup, not
// itself a hit-test" role as cardfile_debug_open_index() above: builds an
// exact dock configuration without first solving the rail "Docks" button +
// popup-row pixel hit-test, so a screenshot can isolate RENDER/LAYOUT
// correctness (the harder, higher-risk half of this feature) from INPUT
// correctness (spot-checked separately with a real CFCLICK/CFRCLICK on the
// actual rail button/popup). Calls the exact same cf_dock_add()/
// cf_dock_set_pos()/cf_dock_set_thickness()/cf_dock_remove() the real UI
// calls - never a shortcut that bypasses the model's own clamping.
uint32_t cardfile_debug_dock_add(int kind, int edge_or_slot, int thickness) {
    cf_ensure_init();
    cf_dock_pos_t pos;
    pos.kind = kind;
    pos.edge = edge_or_slot;
    pos.slot_index = edge_or_slot;
    return cf_dock_add(&g_cf_deck, pos, thickness);
}
int cardfile_debug_dock_set_pos(uint32_t dock_id, int kind, int edge_or_slot) {
    cf_ensure_init();
    cf_dock_pos_t pos;
    pos.kind = kind;
    pos.edge = edge_or_slot;
    pos.slot_index = edge_or_slot;
    return cf_dock_set_pos(&g_cf_deck, dock_id, pos);
}
int cardfile_debug_dock_set_thickness(uint32_t dock_id, int thickness) {
    cf_ensure_init();
    return cf_dock_set_thickness(&g_cf_deck, dock_id, thickness);
}
int cardfile_debug_dock_remove(uint32_t dock_id) {
    cf_ensure_init();
    return cf_dock_remove(&g_cf_deck, dock_id);
}
#endif // MAYTERA_TESTHOOK
