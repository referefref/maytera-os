// widgets.c - Desktop widget layer for the MayteraOS userland compositor (#77).
// Draws always-present desktop gadgets behind app windows: an analog clock
// (#79), a month calendar (#78) and a walking "sheep" desktop pet (#80).
// Network-backed widgets (weather/crypto/ticker) are added separately once the
// VM is back on the LAN.

#include "compositor.h"
extern int g_draw_blend;   // draw.c blend factor (255=opaque)
extern int g_win_opacity;  // main.c global window opacity
#include "../../libc/syscall.h"
#include "../../libc/stdio.h"   // #213 snprintf() for the /RAILPOS.TXT verification dump
#include "../../libc/theme.h"   // #723 theme_color()/THEME_COLOR_* for HA semantic tint
#include "ha_format.h"          // #723 HA entity-state display formatter
#include "../../libc/wifi_client.h"   // #159 sect 6a: wifi_tray_state() for the
                                      // offline-component connectivity gate.
                                      // WIFI_STUB_IMPL (honest stub, #237,
                                      // was WIFI_MOCK_IMPL) already lives in
                                      // bt_impl.c - do not redefine it here.

// #102/#379 dirty-rect: when set, widgets_render() only DRAWS (no state advance:
// no sheep/dog update, no sysmon/netinfo sampling). The idle compositor advances
// state + collects damage once per frame via widgets_collect_damage(), then
// draws each dirty rect with this flag set so per-rect redraws never double-tick.
int g_widgets_draw_only = 0;

// ===========================================================================
// #159 sect 6: shared offline-state component for the four network-backed
// desktop cards (Weather, Crypto, Stocks, Home Assistant). Detection reuses
// the tray's own connectivity signal (taskbar.c tray_render_core, #384) -
// no new syscall, per the ticket. Declared here (top of file, ahead of every
// caller including ha_card_draw further down) so ordering never matters.
// ===========================================================================
static int widget_net_online(void) { return sys_net_is_up() || wifi_tray_state() == 2; }

// Draws "No internet connection" into a card's data region
// [content_top, content_bot) (absolute framebuffer y), replacing whatever the
// live content would have shown there. THE CALLER MUST NOT HAVE READ ANY
// CACHED NETWORK BUFFER (s_weather/s_crypto/s_stocks/s_ha_state/...) before
// reaching this call - see each of the four callers below, which check
// widget_net_online() and return before their own ha_refresh_cache()/
// wsplit(s_weather,...)-equivalent read. That ordering is what makes "no
// data" a structural guarantee instead of a rendering choice: there is no
// code path where a stale cached value and this message can both be drawn.
//
// (#236) Generalized from the offline-only version: the SAME centered
// icon+message block now also serves the "online, but this card has no data
// yet" state that the fixed-footprint rule below needs (a card must fill its
// reserved box with an explicit state, never collapse to fit what little it
// has). `wrap1`/`wrap2` are the caller's own two-line split, used only when
// the single line does not fit the card width - measuring a break point here
// would need a word-wrap pass this file has no other use for.
static void widget_draw_state(int x, int w, int content_top, int content_bot,
                              int icon, const char *msg,
                              const char *wrap1, const char *wrap2) {
    // Undimmed readable_ink(), NOT readable_ink_dim(): the family's usual
    // "unavailable"/"..." placeholder uses the dimmed ink, but that mix
    // measures 4.16:1 on the classic theme - under the WCAG AA 4.5:1 small-
    // text floor (docs/DESKTOP_WIDGET_FAMILY_159.html sect 2a). This state
    // is meant to read as more prominent than a parse failure, and
    // readable_ink()'s worst case (9.57:1) clears AA on every shipping theme.
    uint32_t ink = readable_ink(CLR_MENU_BG);
    int avail_h = content_bot - content_top;
    if (avail_h < ui_px(50)) {
        // Compact fallback (sect 6c): 18px icon + text inline, one row.
        // Not triggered by any current card size - specified defensively,
        // per the brief's question, for any future card under ~50px tall.
        int isz = ui_px(18), rowh = ui_px(22);
        int mw = text_width_ttf(msg, 15);
        int totalw = isz + ui_px(8) + mw;
        int rx = x + (w - totalw) / 2; if (rx < x + ui_px(12)) rx = x + ui_px(12);
        int ry = content_top + (avail_h - rowh) / 2; if (ry < content_top) ry = content_top;
        icon_draw_scaled(icon, rx, ry + (rowh - isz) / 2, isz, ink);
        draw_text_ttf(rx + isz + ui_px(8), ry + (rowh - ui_px(15)) / 2, msg, 15, ink);
        return;
    }
    int isz = ui_px(28), gap = ui_px(6), lineh = ui_px(18);
    int mw = text_width_ttf(msg, 15);
    int avail_w = w - ui_px(24);   // 12px inset each side, matching the family's row inset
    // Re-measured at build time (sect 6d caption): "No internet connection" at
    // TTF size 15 against the narrowest card (HA, 224-24=200px) - if it ever
    // doesn't fit on one line, wrap to two rather than truncate a state message.
    int lines = (mw <= avail_w) ? 1 : 2;
    int blockh = isz + gap + lines * lineh;
    int by = content_top + (avail_h - blockh) / 2; if (by < content_top) by = content_top;
    icon_draw_scaled(icon, x + w / 2 - isz / 2, by, isz, ink);
    int ty = by + isz + gap;
    if (lines == 1) {
        draw_text_ttf(x + w / 2 - mw / 2, ty, msg, 15, ink);
    } else {
        int w1 = text_width_ttf(wrap1, 15), w2 = text_width_ttf(wrap2, 15);
        draw_text_ttf(x + w / 2 - w1 / 2, ty,         wrap1, 15, ink);
        draw_text_ttf(x + w / 2 - w2 / 2, ty + lineh, wrap2, 15, ink);
    }
}
// The two states every network-backed card can be in, both drawn INTO the
// card's full reserved content region so neither one changes its footprint.
static void widget_draw_offline(int x, int w, int content_top, int content_bot) {
    widget_draw_state(x, w, content_top, content_bot, ICON_WIFI_OFF,
                      "No internet connection", "No internet", "connection");
}
static void widget_draw_nodata(int x, int w, int content_top, int content_bot,
                               const char *msg, const char *wrap1, const char *wrap2) {
    widget_draw_state(x, w, content_top, content_bot, ICON_REFRESH, msg, wrap1, wrap2);
}
// Taskbar shares its sampled CPU% (and per-core array) so the sysmon widget and
// the taskbar gauge read the identical source (#102 meter reconciliation).
extern int taskbar_cpu_snapshot(unsigned int *cores, int *ncores);

// Master enable. Defaults on; a Settings toggle can flip g_widgets_enabled.
int g_widgets_enabled = 1;

// sin(i*6deg) * 1024 for clock positions i = 0..59.
static const int SIN60[60] = {
        0,   107,   213,   316,   416,   512,   602,   685,   761,   828,
      887,   935,   974,  1002,  1018,  1024,  1018,  1002,   974,   935,
      887,   828,   761,   685,   602,   512,   416,   316,   213,   107,
        0,  -107,  -213,  -316,  -416,  -512,  -602,  -685,  -761,  -828,
     -887,  -935,  -974, -1002, -1018, -1024, -1018, -1002,  -974,  -935,
     -887,  -828,  -761,  -685,  -602,  -512,  -416,  -316,  -213,  -107,
};
static int sin60(int i) { i %= 60; if (i < 0) i += 60; return SIN60[i]; }
static int cos60(int i) { return sin60(i + 15); }   // cos(t) = sin(t + 90deg)

// Simple integer (Bresenham) line for clock hands.
extern int g_draw_blend;   // draw.c: global blend op (255 = opaque)
// Per-pixel alpha blend (coverage 0..255), honouring g_draw_blend.
static void wdg_aa_px(int x, int y, uint32_t color, int cov) {
    if (x < 0 || y < 0 || x >= g_fb_width || y >= g_fb_height || cov <= 0) return;
    if (cov > 255) cov = 255;
    int op = (g_draw_blend < 255) ? (cov * g_draw_blend / 255) : cov;
    uint32_t *d = &g_fb[y * g_fb_pitch + x];
    if (op >= 255) { *d = color; return; }
    uint32_t bg = *d;
    int r = ((((color >> 16) & 0xFF) * op) + (((bg >> 16) & 0xFF) * (255 - op))) / 255;
    int g = ((((color >> 8)  & 0xFF) * op) + (((bg >> 8)  & 0xFF) * (255 - op))) / 255;
    int b = (((color & 0xFF) * op) + ((bg & 0xFF) * (255 - op))) / 255;
    *d = (uint32_t)((r << 16) | (g << 8) | b);
}
static int wdg_isqrt(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }
// Anti-aliased ring (outline circle) of radius R, ~2px stroke, centered (cx,cy).
static void wdg_ring(int cx, int cy, int R, uint32_t color) {
    for (int dy = -R - 2; dy <= R + 2; dy++)
        for (int dx = -R - 2; dx <= R + 2; dx++) {
            int d = wdg_isqrt(dx * dx + dy * dy);
            int diff = d - R; if (diff < 0) diff = -diff;
            int a = (diff == 0) ? 255 : (diff == 1) ? 150 : 0;   // 2px AA stroke
            if (a) wdg_aa_px(cx + dx, cy + dy, color, a);
        }
}
// Anti-aliased line (Xiaolin Wu, fixed-point) for crisp clock hands + ticks.
static void wdg_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx == 0 && ady == 0) { wdg_aa_px(x0, y0, color, 255); return; }
    int steep = ady > adx, t;
    if (steep) { t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    dx = x1 - x0; dy = y1 - y0;
    int grad = (dx == 0) ? (dy << 8) : ((dy << 8) / dx);
    int inter = (y0 << 8);
    for (int x = x0; x <= x1; x++) {
        int yi = inter >> 8, fr = inter & 0xFF;
        if (steep) { wdg_aa_px(yi, x, color, 255 - fr); wdg_aa_px(yi + 1, x, color, fr); }
        else       { wdg_aa_px(x, yi, color, 255 - fr); wdg_aa_px(x, yi + 1, color, fr); }
        inter += grad;
    }
}

// A hand drawn as a short cluster of parallel lines for visible thickness.
static void wdg_hand(int cx, int cy, int pos, int len, int thick, uint32_t color) {
    int ex = cx + sin60(pos) * len / 1024;
    int ey = cy - cos60(pos) * len / 1024;
    wdg_line(cx, cy, ex, ey, color);
    for (int t = 1; t < thick; t++) {
        wdg_line(cx + t, cy, ex + t, ey, color);
        wdg_line(cx, cy + t, ex, ey + t, color);
    }
}

// --- Analog clock (#79) ---------------------------------------------------
static void widget_analog_clock(int cx, int cy, int r) {
    draw_circle_filled(cx, cy, r, CLR_MENU_BG);
    draw_circle_outline(cx, cy, r, CLR_MENU_TEXT);
    draw_circle_outline(cx, cy, r - 1, CLR_MENU_BORDER);

    // Hour ticks (every 5 positions) + minute ticks.
    for (int i = 0; i < 60; i++) {
        int outer = r - 2;
        int inner = (i % 5 == 0) ? r - 8 : r - 4;
        uint32_t tc = (i % 5 == 0) ? CLR_MENU_TEXT : CLR_MENU_BORDER;
        int ox = cx + sin60(i) * outer / 1024;
        int oy = cy - cos60(i) * outer / 1024;
        int ix = cx + sin60(i) * inner / 1024;
        int iy = cy - cos60(i) * inner / 1024;
        wdg_line(ix, iy, ox, oy, tc);
    }

    int h = 0, m = 0, s = 0;
    tz_local_hms(&h, &m, &s);      // #49: local, not raw RTC
    int hour_pos = ((h % 12) * 5 + m / 12) % 60;

    wdg_hand(cx, cy, hour_pos, r - 22, 2, CLR_MENU_TEXT);  // hour
    wdg_hand(cx, cy, m,        r - 12, 2, CLR_MENU_TEXT);  // minute
    wdg_hand(cx, cy, s,        r - 8,  1, 0x00FF5050);  // second
    draw_circle_filled(cx, cy, ui_px(3), 0x00FFD040);   // hub
}

// --- Month calendar (#78) -------------------------------------------------
static const char *MON_NAMES[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static int days_in_month(int m, int y) {
    static const int d[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    if (m < 1 || m > 12) return 30;
    return d[m - 1];
}

// #50: was a second private copy of Zeller's congruence (clock.c had a third).
// tz_wday() in libc/tz.c is THE implementation; this is a name-only shim so the
// calendar's call sites read unchanged.
static int day_of_week(int d, int m, int y) { return tz_wday(d, m, y); }

static void itoa2(char *b, int v) { b[0] = '0' + (v / 10) % 10; b[1] = '0' + v % 10; b[2] = '\0'; }

// (#236) The calendar's height, in ONE place. widgets_render() used to carry
// a hand-copied duplicate of this expression, which is the same
// keep-two-formulas-in-sync arrangement that let the Weather card and its
// reserved slot disagree. The 6 rows are unconditional on purpose: a month
// that needs only 5 must still occupy the same box as one that needs 6, or
// the widget below it would move between months.
#define CAL_ROWS 6
// #uiscale: FONT_CHAR_H is already ui_px()'d (compositor.h) - do NOT wrap it
// in ui_px() again (see the sysmon_bar()/widget_timer() fix above for what
// that bug looks like). Every OTHER literal here gets its own ui_px() call.
static int calendar_card_h(void) {
    return (FONT_CHAR_H + ui_px(10)) + (FONT_CHAR_H + ui_px(2)) + CAL_ROWS * (FONT_CHAR_H + ui_px(2)) + ui_px(6);
}

static void widget_calendar(int x, int y, int w) {
    int dd, mm, yy;
    tz_local_date(&dd, &mm, &yy);   // #49: "today" is a LOCAL date
    if (mm < 1 || mm > 12) mm = 1;
    if (yy < 1970 || yy > 3000) yy = 2026;

    int cell = w / 7;
    // #199: header_h was FONT_CHAR_H+6 (22px), too tight for the family's
    // title baseline (every other data widget anchors its title at y+8, e.g.
    // widget_uptime()/widget_timer()/widget_worldtime() above); +10 (26px)
    // matches that same convention so this card's title no longer crowds the
    // weekday row 2px below it.
    int header_h = FONT_CHAR_H + ui_px(10);
    int dow_h = FONT_CHAR_H + ui_px(2);
    int h = calendar_card_h();

    draw_rounded_rect(x, y, w, h, ui_px(8), CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    // #199: Calendar was the one card-shaped desktop widget #159 missed - every
    // other data card (System Monitor/Timer/World Time/Uptime/Weather/Crypto/
    // Stocks/Home Assistant) got a left accent bar + a left-aligned title in
    // the accent color at x+12,y+8; Calendar kept its pre-#159 centered title
    // and no accent bar, which is exactly the "doesn't match its neighbours"
    // the owner is looking at. Same gold hue it already used for its title,
    // now also the accent bar, so the diff is chrome-only, not a new color.
    draw_rounded_rect(x, y, ui_px(4), h, ui_px(2), 0x00FFD040);   // left accent bar

    // Title: "Month Year"
    char title[32];
    const char *mn = MON_NAMES[mm - 1];
    int ti = 0;
    while (mn[ti]) { title[ti] = mn[ti]; ti++; }
    title[ti++] = ' ';
    title[ti++] = '0' + (yy / 1000) % 10;
    title[ti++] = '0' + (yy / 100) % 10;
    title[ti++] = '0' + (yy / 10) % 10;
    title[ti++] = '0' + yy % 10;
    title[ti] = '\0';
    draw_text(x + ui_px(12), y + ui_px(8), title, readable_accent(0x00FFD040, CLR_MENU_BG));

    // Weekday header
    static const char *wd[7] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
    int gy = y + header_h;
    for (int c = 0; c < 7; c++) {
        uint32_t wc = (c == 0 || c == 6) ? readable_accent(0x00FF8080, CLR_MENU_BG) : CLR_MENU_TEXT;
        draw_text(x + c * cell + (cell - text_width(wd[c])) / 2, gy, wd[c], wc);
    }

    int first = day_of_week(1, mm, yy);
    int ndays = days_in_month(mm, yy);
    int gy0 = gy + dow_h;
    int row_h = FONT_CHAR_H + ui_px(2);
    for (int day = 1; day <= ndays; day++) {
        int idx = first + day - 1;
        int col = idx % 7;
        int row = idx / 7;
        int cx = x + col * cell;
        int cyy = gy0 + row * row_h;
        char num[3]; int n = day; char nb[4];
        if (n < 10) { nb[0] = '0' + n; nb[1] = '\0'; }
        else itoa2(nb, n);
        (void)num;
        uint32_t col_text = (col == 0 || col == 6) ? readable_accent(0x00FF9090, CLR_MENU_BG) : CLR_MENU_TEXT;
        if (day == dd) {
            // Circle the current day with a high-contrast AA ring (2px). The old
            // readable_accent(gold) rendered near-invisible on dark cards; use a
            // luminance-contrast ink so it stands out on ANY theme.
            int rr = (row_h < cell ? row_h : cell) / 2 - ui_px(1); if (rr < ui_px(6)) rr = ui_px(6);
            uint32_t ring = readable_ink(CLR_MENU_BG);
            wdg_ring(cx + cell / 2, cyy + FONT_CHAR_H / 2, rr,     ring);
            wdg_ring(cx + cell / 2, cyy + FONT_CHAR_H / 2, rr - ui_px(1), ring);
        }
        draw_text(cx + (cell - text_width(nb)) / 2, cyy, nb, col_text);
    }
}

// --- Sheep desktop pets (#80, multi): drag + gravity + speed/size/style ---
#define MAX_SHEEP 50
int g_sheep_enabled = 0;
int g_show_clock     = 0;   // analog Clock stays OFF by default (owner's
                             // 2026-08-18 list is Digital Clock, not this one)
int g_aichat_enabled = 1;   // #185 AI Chat docked panel app (default ON; #453 busy-spin fixed in b740)
                             // also the 2026-08-18 owner default (setup/main.rs WIDX_AICHAT)
int g_show_calendar  = 1;   // default ON, 2026-08-18 owner decision - see
                             // setup/main.rs WIDX_CALENDAR / WIDGETS_DEFAULT_MASK
int g_sheep_speed    = 3;   // 1..5
int g_sheep_size     = 2;   // 1..3 (small/normal/large)
int g_sheep_style    = 0;   // 0 classic, 1 spotted
int g_sheep_count    = 1;   // 1..50

// Sheepdog state (declared early so sheep flee logic can see it).
int g_dog_enabled = 0;   // sheepdog: enable via the sheep tray menu
static int dog_x = -1, dog_y, dog_dir = 1, dog_dragging = 0, dog_gdx, dog_gdy;
static unsigned dog_frame = 0;
#define DOG_W ui_px(56)
#define DOG_H ui_px(30)

typedef struct {
    int x, y, vy, dir, state, land_t, blink_t, inited;
    int behavior, btimer;   // 0 walk,1 sleep,2 run,3 fart
    int climb;              // climbing the screen edge
    unsigned frame, rng;
    int anim, aidx, ahold;   // one-shot eSheep animation (#80): -1 = none
    // (#40) Where the current climb ends. climb_ty is the y the pet stops at;
    // climb_tx is the x it stands at when it gets there, or -1 for "it does
    // not get over this edge", which is a screen edge: it turns around at the
    // top and drops back down, exactly as it always did. A dock wall sets a
    // real climb_tx, so the pet steps over onto the dock's top surface.
    int climb_ty, climb_tx;
} sheep_t;
static sheep_t g_sheep[MAX_SHEEP];
static int g_grabbed = -1;
static int g_grab_dx = 0, g_grab_dy = 0;

static int sheep_sc(void){ return (g_sheep_size<=1)?75 : (g_sheep_size>=3)?135 : 100; }
// #uiscale: ui_px() on the base 1x dimension, sheep_sc()'s own percent size
// setting applied on top - the two multipliers are independent (screen DPI
// vs. the user's small/normal/large choice) and compose cleanly. sheep_hit()/
// sheep_grab() below call these SAME functions, so the drag hit-box can never
// drift from the drawn footprint.
static int sheep_w(void){ return ui_px(50) * sheep_sc() / 100; }
static int sheep_h(void){ return ui_px(34) * sheep_sc() / 100; }

// (#40) The band a desktop pet lives in: the WIDGET AREA (taskbar_widget_
// area()), not a hardcoded "screen minus 36". Under a bottom-taskbar style
// that is screen-minus-the-taskbar, which is byte-identical to the old
// `g_fb_height - 36` since TASKBAR_HEIGHT is 36; under the marble dock it
// runs to the BOTTOM EDGE of the screen, which is what the user asked for,
// and the dock itself then shows up as a wall in the surface list below.
static void pet_bounds(int *top, int *bot) {
    int wax, way, waw, wah;
    taskbar_widget_area(&wax, &way, &waw, &wah);
    if (top) *top = way;
    if (bot) *bot = way + wah;
}
static int sheep_ground(void){ int b; pet_bounds(NULL, &b); return b - sheep_h(); }

// (#40) THE surface list the pets collide with: app windows AND the active
// dock style's chrome (taskbar_panel_rects()), in ONE array, so the floor
// scan below stays a single loop over a single list rather than growing a
// second, style-aware collision path.
// `wall` marks a surface the pet cannot walk THROUGH. A window is not one: it
// is walked behind, exactly as it always was. The marble dock is, because it
// sits ON the screen's bottom edge and paints over the pets (widgets_render()
// runs before taskbar_render()), so a sheep walking behind it would simply
// vanish. Treating it as a wall is what makes it behave "like a window or
// screen edge (climb and fall etc)".
typedef struct { int x, y, w, h, wall; } pet_surface_t;
#define PET_MAX_SURFACES  (16 + 4)
static wm_window_info_t g_wlist[16];    // scratch for wm_get_windows()
static int g_wn = 0;
static pet_surface_t g_psurf[PET_MAX_SURFACES];
static int g_psurf_n = 0;

// Refresh the surface list. Called once per pet tick from the same two places
// that used to refresh the window cache alone (the busy render path and the
// idle damage-collect path), so there is still exactly one wm_get_windows()
// syscall per tick and nothing in the draw path blocks.
static void pets_refresh_surfaces(void) {
    g_psurf_n = 0;
    g_wn = wm_get_windows(g_wlist, 16);
    if (g_wn < 0) g_wn = 0;
    for (int i = 0; i < g_wn && g_psurf_n < PET_MAX_SURFACES; i++) {
        if (!g_wlist[i].visible || g_wlist[i].minimized) continue;
        g_psurf[g_psurf_n].x = g_wlist[i].x;
        g_psurf[g_psurf_n].y = g_wlist[i].y;
        g_psurf[g_psurf_n].w = g_wlist[i].width;
        g_psurf[g_psurf_n].h = g_wlist[i].height;
        g_psurf[g_psurf_n].wall = 0;
        g_psurf_n++;
    }
    chrome_rect_t pr[4];
    int pn = taskbar_panel_rects(pr, 4);
    for (int i = 0; i < pn && g_psurf_n < PET_MAX_SURFACES; i++) {
        g_psurf[g_psurf_n].x = pr[i].x;
        g_psurf[g_psurf_n].y = pr[i].y;
        g_psurf[g_psurf_n].w = pr[i].w;
        g_psurf[g_psurf_n].h = pr[i].h;
        g_psurf[g_psurf_n].wall = 1;
        g_psurf_n++;
    }
}

static int sheep_floor(int x, int w, int cur_y) {
    int top_lim; pet_bounds(&top_lim, NULL);
    int best = sheep_ground();                 // lowest floor = largest y
    int cx = x + w / 2;
    for (int i = 0; i < g_psurf_n; i++) {
        if (cx >= g_psurf[i].x && cx < g_psurf[i].x + g_psurf[i].w) {
            int top = g_psurf[i].y - sheep_h();
            // Only a valid floor if it is at/below the sheep's current feet, so a
            // sheep on the taskbar does NOT teleport up onto a window above it.
            // A sheep only rests on a surface it fell (or climbed) onto, then
            // walks off it. The `>= top_lim` test is what keeps a pet out of a
            // TOP panel: that panel's own top is above the widget area, so it
            // is never offered as a floor. It replaces a hardcoded 24, which
            // matched no dock style's top bar exactly.
            if (top >= top_lim && top >= cur_y && top < best) best = top;
        }
    }
    return best;
}

// (#40) The first WALL surface the box at (nx, y) would enter, or -1. A wall
// only blocks where the pet's body actually OVERLAPS it vertically: standing
// ON its top edge is not a collision, which is exactly what lets a sheep that
// has climbed the dock walk along its top and off the far end.
static int pet_wall_hit(int nx, int y, int w, int h) {
    for (int i = 0; i < g_psurf_n; i++) {
        if (!g_psurf[i].wall) continue;
        if (nx + w <= g_psurf[i].x || nx >= g_psurf[i].x + g_psurf[i].w) continue;
        if (y + h <= g_psurf[i].y || y >= g_psurf[i].y + g_psurf[i].h) continue;
        return i;
    }
    return -1;
}

static void sheep_spawn(int i) {
    unsigned seed = (unsigned)(i * 2654435761u) ^ 0x9e3779b9u;
    int span = g_fb_width - 120; if (span < 1) span = 1;
    g_sheep[i].x = 40 + (int)((seed >> 13) % (unsigned)span);
    g_sheep[i].y = sheep_ground();
    g_sheep[i].vy = 0;
    g_sheep[i].dir = (i & 1) ? 1 : -1;
    g_sheep[i].state = 0;
    g_sheep[i].frame = seed & 0x3F;
    g_sheep[i].land_t = 0;
    g_sheep[i].blink_t = 0;
    g_sheep[i].behavior = 0;
    g_sheep[i].climb = 0;
    g_sheep[i].climb_ty = 0; g_sheep[i].climb_tx = -1;
    g_sheep[i].anim = -1; g_sheep[i].aidx = 0; g_sheep[i].ahold = 0;
    g_sheep[i].btimer = (int)(seed % 120u);
    g_sheep[i].rng = seed | 1u;
    g_sheep[i].inited = 1;
}

// Public hit / drag interface (operates on the sheep under the cursor).
int sheep_hit(int x, int y) {
    if (!g_sheep_enabled) return 0;
    int w = sheep_w(), h = sheep_h();
    for (int i = 0; i < g_sheep_count && i < MAX_SHEEP; i++) {
        if (!g_sheep[i].inited) continue;
        if (x >= g_sheep[i].x && x < g_sheep[i].x + w &&
            y >= g_sheep[i].y && y < g_sheep[i].y + h) return 1;
    }
    return 0;
}
void sheep_grab(int x, int y) {
    int w = sheep_w(), h = sheep_h();
    for (int i = g_sheep_count - 1; i >= 0; i--) {     // topmost first
        if (i >= MAX_SHEEP || !g_sheep[i].inited) continue;
        if (x >= g_sheep[i].x && x < g_sheep[i].x + w &&
            y >= g_sheep[i].y && y < g_sheep[i].y + h) {
            g_grabbed = i; g_sheep[i].state = 3;
            g_grab_dx = x - g_sheep[i].x; g_grab_dy = y - g_sheep[i].y;
            g_sheep[i].vy = 0;
            return;
        }
    }
}
void sheep_drag_to(int x, int y) {
    if (g_grabbed < 0) return;
    sheep_t *sp = &g_sheep[g_grabbed];
    sp->x = x - g_grab_dx; sp->y = y - g_grab_dy;
    if (sp->x < 0) sp->x = 0;
    if (sp->x > g_fb_width - sheep_w()) sp->x = g_fb_width - sheep_w();
    // (#40) Drag is bounded by the SAME band the pet walks in, so a dragged
    // sheep can be put right on the screen's bottom edge under the marble
    // dock, and can never be dropped behind a top panel. This was a bare
    // `if (sp->y < 0) sp->y = 0;`, i.e. the top of the screen, with no bottom
    // bound at all.
    int ptop, pbot; pet_bounds(&ptop, &pbot);
    if (sp->y < ptop) sp->y = ptop;
    if (sp->y > pbot - sheep_h()) sp->y = pbot - sheep_h();
}
void sheep_release(void) {
    if (g_grabbed < 0) return;
    g_sheep[g_grabbed].state = 1; g_sheep[g_grabbed].vy = 0;
    g_grabbed = -1;
}
int sheep_is_dragging(void) { return g_grabbed >= 0; }


// One-shot eSheep behaviours (frame indices into the 16x11 sheet, from
// src/Actions.js) the idle RNG can randomly trigger (#80).
static const unsigned char SHEEP_ANIM_ROLL[]   = {9,10,126,125,124,123,122,121,120,119,118,117,116,115,114,113,112,10,9};
static const unsigned char SHEEP_ANIM_METEOR[] = {134,135,136,137,138,139,140,141,142,143,144,145};
static const unsigned char SHEEP_ANIM_EAT[]    = {58,59,60,61,60,61,60,61,58,59,60,61};
static const unsigned char SHEEP_ANIM_YAWN[]   = {31,107,108,110,111,110,111,109,31};
static const unsigned char SHEEP_ANIM_HANDS[]  = {78,86,87,86,87,86,87,86,87,78};
static const struct { const unsigned char *f; int count; int hold; } g_sheep_anims[] = {
    { SHEEP_ANIM_ROLL,   19, 3 },   // tumble
    { SHEEP_ANIM_METEOR, 12, 4 },   // fall on fire
    { SHEEP_ANIM_EAT,    12, 6 },   // graze
    { SHEEP_ANIM_YAWN,    9, 8 },   // yawn
    { SHEEP_ANIM_HANDS,  10, 5 },   // walk on hands
};
#define SHEEP_NANIM ((int)(sizeof(g_sheep_anims)/sizeof(g_sheep_anims[0])))

// (#40) The pet reached an edge it cannot walk past. This is the coin flip the
// SCREEN edges have always used, pulled out so the dock wall uses the identical
// behaviour instead of a second, subtly different one: 30% of the time climb
// the edge, otherwise turn away from it.
//   climb_ty: the y the climb ends at.
//   climb_tx: the x to stand at once up there, or -1 for "no top to stand on"
//             (a screen edge), which turns and drops back down.
//   away:     the direction to face if it does not climb.
static void sheep_edge(sheep_t *sp, int away, int climb_ty, int climb_tx) {
    int top_lim; pet_bounds(&top_lim, NULL);
    if (climb_ty < top_lim) climb_ty = top_lim;
    sp->rng = sp->rng * 1103515245u + 12345u;
    if (((sp->rng >> 16) % 100u) < 30) {
        sp->climb = 1; sp->climb_ty = climb_ty; sp->climb_tx = climb_tx;
    } else {
        sp->dir = away;
    }
}

static void sheep_one_update(int idx) {
    sheep_t *sp = &g_sheep[idx];
    sp->frame++;
    int ground = sheep_floor(sp->x, sheep_w(), sp->y);
    if (sp->y > ground && idx != g_grabbed) sp->y = ground;

    if (idx == g_grabbed) { sp->state = 3; return; }

    if (sp->climb) {                       // climbing an edge: screen or dock
        sp->state = 1;                     // cling/splayed pose
        sp->y -= 3;
        if (sp->y <= sp->climb_ty) {
            sp->y = sp->climb_ty;
            sp->climb = 0; sp->vy = 0;
            // (#40) climb_tx >= 0 means this edge HAS a top the pet can stand
            // on (the dock): step over onto it, keeping its direction, and it
            // will walk along the dock and fall off the far end. A screen edge
            // has no top (climb_tx < 0), so it turns around and drops back
            // down, which is the behaviour this block always had.
            if (sp->climb_tx >= 0) sp->x = sp->climb_tx;
            else                   sp->dir = -sp->dir;
        }
        sp->frame++;
        return;
    }

    if (sp->y < ground) {
        sp->state = 1;
        if ((sp->frame & 1) == 0 && sp->vy < 9) sp->vy++;
        sp->y += sp->vy;
        if (sp->y >= ground) {
            sp->y = ground;
            if (sp->vy > 4) sp->vy = -(sp->vy / 3);
            else { sp->vy = 0; sp->state = 2; sp->land_t = 10; }
        }
    } else {
        sp->y = ground;
        if (sp->state == 2) { if (--sp->land_t <= 0) sp->state = 0; }
        else {
            sp->state = 0;
            // A one-shot special animation is playing: advance it, hold position.
            if (sp->anim >= 0) {
                if (++sp->ahold >= g_sheep_anims[sp->anim].hold) {
                    sp->ahold = 0;
                    if (++sp->aidx >= g_sheep_anims[sp->anim].count) { sp->anim = -1; sp->btimer = 20; }
                }
                if (sp->blink_t > 0) sp->blink_t--;
                return;
            }
            // Idle-behavior machine: walk / sleep / run / fart + random special anims.
            if (sp->btimer > 0) sp->btimer--;
            if (sp->btimer == 0) {
                sp->rng = sp->rng * 1103515245u + 12345u;
                unsigned roll = (sp->rng >> 16) % 100u;
                if      (roll < 45) sp->behavior = 0;          // walk
                else if (roll < 78) sp->behavior = 1;          // sleep
                else if (roll < 83) sp->behavior = 2;          // run
                else if (roll < 87) sp->behavior = 3;          // fart
                else {                                          // ~13%: a special eSheep animation
                    sp->rng = sp->rng * 1103515245u + 12345u;
                    sp->anim = (int)((sp->rng >> 16) % (unsigned)SHEEP_NANIM);
                    sp->aidx = 0; sp->ahold = 0;
                    sp->behavior = 0;
                }
                sp->rng = sp->rng * 1103515245u + 12345u;
                sp->btimer = 90 + (int)((sp->rng >> 16) % 210u);   // ~1.5-5s
            }
            // Flee from the sheepdog if it is close.
            if (g_dog_enabled) {
                int ddx = (sp->x + sheep_w()/2) - (dog_x + DOG_W/2);
                int ady = (sp->y - dog_y); if (ady < 0) ady = -ady;
                int adx = ddx < 0 ? -ddx : ddx;
                if (adx < 150 && ady < 120) {
                    sp->dir = (ddx >= 0) ? 1 : -1;     // run away from the dog
                    sp->behavior = 2;                  // run
                    sp->btimer = 30;
                }
            }
            if (sp->behavior == 0 || sp->behavior == 2) {          // walk / run
                int run = (sp->behavior == 2);
                int iv = run ? 2 : (8 - g_sheep_speed); if (iv < 2) iv = 2;
                int step = (2 + g_sheep_speed) * (run ? 2 : 1);
                if ((sp->frame % (unsigned)iv) == 0) {
                    int sw = sheep_w(), sh = sheep_h();
                    int nx = sp->x + sp->dir * step;
                    // A screen edge: no top to stand on, so the climb ends a
                    // third of the screen up and the pet drops back down.
                    int edge_ty = ground - (int)g_fb_height / 3;
                    int wall;
                    if (nx > (int)g_fb_width - sw) {
                        sp->x = (int)g_fb_width - sw;
                        sheep_edge(sp, -1, edge_ty, -1);
                    } else if (nx < 0) {
                        sp->x = 0;
                        sheep_edge(sp, 1, edge_ty, -1);
                    } else if ((wall = pet_wall_hit(nx, sp->y, sw, sh)) >= 0) {
                        // (#40) The dock. Stop at the face it walked into,
                        // then the same climb-or-turn roll as a screen edge,
                        // with the dock's TOP as somewhere to stand: climbing
                        // it puts the pet on the dock, walking on from there
                        // eventually takes it off the far end and it falls.
                        const pet_surface_t *s = &g_psurf[wall];
                        int ty = s->y - sh, tx;
                        if (sp->dir > 0) { sp->x = s->x - sw;        tx = s->x + 2; }
                        else             { sp->x = s->x + s->w;      tx = s->x + s->w - sw - 2; }
                        // Both the face and the landing spot stay on screen:
                        // a dock whose edge is within one sheep width of the
                        // screen edge must not push the pet off it.
                        if (sp->x < 0) sp->x = 0;
                        if (sp->x > (int)g_fb_width - sw) sp->x = (int)g_fb_width - sw;
                        if (tx < 0) tx = 0;
                        if (tx > (int)g_fb_width - sw) tx = (int)g_fb_width - sw;
                        sheep_edge(sp, -sp->dir, ty, tx);
                    } else {
                        sp->x = nx;
                    }
                }
            }
            if (sp->blink_t > 0) sp->blink_t--;
            else if ((sp->frame % 180) == 0) sp->blink_t = 8;
        }
    }
}


// ===== eSheep sprite-sheet rendering (#80/#93). Loads /SHEEP.SPR once (raw
// ARGB grid, 16x11 cells of 40x40 from github.com/kuindji/sheep-js) and blits
// the frame for the current state, replacing the procedural draw. Falls back to
// the procedural sheep if the asset is missing. =====
#define SHEEP_SHEET_W   640
#define SHEEP_SHEET_H   440
#define SHEEP_CELL      40
#define SHEEP_GRID_COLS 16
static uint32_t g_sheep_sheet[SHEEP_SHEET_W * SHEEP_SHEET_H];
static int g_sheep_sheet_ok = 0;
static int g_sheep_sheet_tried = 0;

static void sheep_sheet_load(void) {
    if (g_sheep_sheet_tried) return;
    g_sheep_sheet_tried = 1;
    int fd = sys_open("/SHEEP.SPR", 0);
    if (fd < 0) return;
    unsigned char hdr[12];
    if (sys_read(fd, hdr, 12) != 12 ||
        hdr[0] != 'S' || hdr[1] != 'H' || hdr[2] != 'P' || hdr[3] != '1') {
        sys_close(fd); return;
    }
    unsigned int w = hdr[4] | (hdr[5]<<8) | (hdr[6]<<16) | ((unsigned)hdr[7]<<24);
    unsigned int h = hdr[8] | (hdr[9]<<8) | (hdr[10]<<16) | ((unsigned)hdr[11]<<24);
    if (w != SHEEP_SHEET_W || h != SHEEP_SHEET_H) { sys_close(fd); return; }
    int total = (int)(w * h * 4), got = 0;
    unsigned char *dst = (unsigned char *)g_sheep_sheet;
    while (got < total) {
        long n = sys_read(fd, dst + got, total - got);
        if (n <= 0) break;
        got += (int)n;
    }
    sys_close(fd);
    if (got == total) g_sheep_sheet_ok = 1;
}

static int sheep_sprites_ready(void) {
    sheep_sheet_load();
    return g_sheep_sheet_ok;
}

// Blit grid frame `idx` (row-major, 0..175) to (dx,dy) scaled to dw x dh, with
// alpha compositing and optional horizontal flip.
static void sheep_blit_frame(int idx, int dx, int dy, int dw, int dh, int hflip) {
    if (!g_sheep_sheet_ok || dw <= 0 || dh <= 0) return;
    int sx0 = (idx % SHEEP_GRID_COLS) * SHEEP_CELL;
    int sy0 = (idx / SHEEP_GRID_COLS) * SHEEP_CELL;
    int stepx = (SHEEP_CELL << 8) / dw, stepy = (SHEEP_CELL << 8) / dh, syf = 0;
    for (int r = 0; r < dh; r++, syf += stepy) {
        int sy = syf >> 8; if (sy > SHEEP_CELL-1) sy = SHEEP_CELL-1;
        int py = dy + r; if (py < 0 || py >= g_fb_height) continue;
        int sxf = 0;
        for (int c = 0; c < dw; c++, sxf += stepx) {
            int sx = sxf >> 8; if (sx > SHEEP_CELL-1) sx = SHEEP_CELL-1;
            int col = hflip ? (SHEEP_CELL-1 - sx) : sx;
            int px = dx + c; if (px < 0 || px >= g_fb_width) continue;
            uint32_t spx = g_sheep_sheet[(sy0 + sy) * SHEEP_SHEET_W + (sx0 + col)];
            uint32_t a = (spx >> 24) & 0xFF;
            if (a == 0) continue;
            uint32_t sr = (spx >> 16) & 0xFF, sg = (spx >> 8) & 0xFF, sb = spx & 0xFF;
            uint32_t *d = &g_fb[py * g_fb_pitch + px];
            if (a == 255) { *d = 0xFF000000u | (sr<<16) | (sg<<8) | sb; continue; }
            uint32_t dv = *d, dR = (dv>>16)&0xFF, dG = (dv>>8)&0xFF, dB = dv & 0xFF;
            uint32_t rr = (sr*a + dR*(255-a))/255, rg = (sg*a + dG*(255-a))/255, rb = (sb*a + dB*(255-a))/255;
            *d = 0xFF000000u | (rr<<16) | (rg<<8) | rb;
        }
    }
}

// Map the widget's sheep state to an eSheep frame index (left-facing base art).
static int sheep_frame_idx(const sheep_t *sp) {
    if (sp->anim >= 0 && sp->aidx < g_sheep_anims[sp->anim].count)
        return g_sheep_anims[sp->anim].f[sp->aidx];
    unsigned fr = sp->frame;
    if (sp->climb)                              { int q[2] = {40, 41}; return q[(fr >> 2) & 1]; } // climb
    if (sp->state == 3)                         return 71;                                       // held/grabbed (arms up)
    if (sp->state == 0 && sp->behavior == 1)    { int q[2] = {0, 1};   return q[(fr >> 4) & 1]; } // sleep
    if (sp->state == 0 && sp->behavior == 3)    return 6;                                        // fart -> stand (no fart art)
    if (sp->state == 2)                         { int q[2] = {4, 5};   return q[(fr >> 1) & 1]; } // run
    if (sp->state == 1)                         return 6;                                        // stand / fall / cling
    int q[2] = {2, 3}; return q[(fr >> 2) & 1];                                                   // walk
}

static void sheep_draw_sprite(const sheep_t *sp) {
    int sc = sheep_sc();
    // #uiscale: ui_px() on SHEEP_CELL so the sprite frame grows with screen
    // DPI too, not just with the size-setting percent (sc).
    int dw = ui_px(SHEEP_CELL) * sc / 100, dh = dw;  // square frame, scaled to sheep size
    int dx = sp->x + (sheep_w() - dw) / 2;          // centre over the logical box
    int dy = sp->y + sheep_h() - dh;                // feet on the box bottom (ground)
    int hflip = (sp->dir >= 0);                     // sheet faces LEFT; flip for right
    sheep_blit_frame(sheep_frame_idx(sp), dx, dy, dw, dh, hflip);
}

static void sheep_one_draw(const sheep_t *sp) {
    if (sheep_sprites_ready()) { sheep_draw_sprite(sp); return; }
    int sc = sheep_sc();
    int bx = sp->x, by = sp->y;
    uint32_t wool = 0x00F0F0F0, woolsh = 0x00C8C8C8, face = 0x00303030;
    // #uiscale: ui_px() wraps the WHOLE (v)*sc/100 computation - the same
    // "wrap the exact 1x value" rule as everywhere else in this file, just
    // applied once here since every body-part call site already routes
    // through one of these three macros.
    #define PX(v) (bx + ui_px((v) * sc / 100))
    #define PY(v) (by + ui_px((v) * sc / 100))
    #define PR(v) (ui_px(((v) * sc / 100) < 1 ? 1 : ((v) * sc / 100)))
    int squash = (sp->state == 2) ? PR(4) : 0;
    int lie = (sp->state == 0 && sp->behavior == 1) ? PR(8) : 0;  // sleeping: lie down

    if (sp->climb) {
        // Rotated 90deg against the wall (feet on the edge), head up. Left wall
        // = clockwise, right wall = counter-clockwise.
        int left = (bx < g_fb_width / 2);
        int wallx = left ? 0 : g_fb_width;                       // the edge the feet grip
        // ly = distance perpendicular to the wall (feet at ly~34 grip the edge),
        // lx = position along the body (maps to vertical; head lx=40 is up).
        #define TX(ly) (left ? (wallx + ui_px((34-(ly))*sc/100)) : (wallx - ui_px((34-(ly))*sc/100)))
        #define TY(lx) (by + ui_px((50-(lx))*sc/100))
        draw_circle_filled(TX(14), TY(18), PR(11), woolsh);
        draw_circle_filled(TX(13), TY(30), PR(11), woolsh);
        draw_circle_filled(TX(10), TY(24), PR(12), wool);
        draw_circle_filled(TX(12), TY(16), PR(9),  wool);
        draw_circle_filled(TX(12), TY(32), PR(9),  wool);
        if (g_sheep_style == 1) {
            draw_circle_filled(TX(11), TY(20), PR(3), woolsh);
            draw_circle_filled(TX(15), TY(29), PR(3), woolsh);
        }
        draw_circle_filled(TX(14), TY(40), PR(7), face);   // head (up)
        draw_circle_filled(TX(9),  TY(40), PR(6), wool);
        draw_circle_filled(TX(13), TY(42), PR(1), 0x00FFFFFF);
        // 4 legs gripping the wall, animated so they "step" while climbing
        int ph = sp->frame / 4;
        for (int i = 0; i < 4; i++) {
            int lxp = 8 + i * 9;                  // position along the body
            int step = ((ph + i) & 1);            // alternate legs reach/pull
            int footly = step ? 34 : 29;          // 34 = flush to the wall
            int kX = TX(22), kY = TY(lxp);
            int fX = TX(footly), fY = TY(lxp + (step ? 1 : -1));
            int rx = fX < kX ? fX : kX, ry = fY < kY ? fY : kY;
            int rw = (fX > kX ? fX-kX : kX-fX) + PR(3);
            int rh = (fY > kY ? fY-kY : kY-fY) + PR(3);
            draw_fill_rect(rx, ry, rw, rh, face);
        }
        #undef TX
        #undef TY
        return;
    }

    if (lie) {                                   // lying: tucked legs (low stubs)
        draw_fill_rect(PX(11), PY(24) + lie, PR(8), PR(3), face);
        draw_fill_rect(PX(27), PY(24) + lie, PR(8), PR(3), face);
    } else if (sp->state == 1) {
        draw_fill_rect(PX(6),  PY(20), PR(4), PR(8), face);
        draw_fill_rect(PX(16), PY(22), PR(4), PR(6), face);
        draw_fill_rect(PX(30), PY(22), PR(4), PR(6), face);
        draw_fill_rect(PX(40), PY(20), PR(4), PR(8), face);
    } else if (sp->state == 3) {
        draw_fill_rect(PX(12), PY(22), PR(4), PR(11), face);
        draw_fill_rect(PX(22), PY(22), PR(4), PR(12), face);
        draw_fill_rect(PX(34), PY(22), PR(4), PR(11), face);
    } else {
        int walk = (sp->state == 0) ? ((sp->frame >> 2) & 1) : 0;
        draw_fill_rect(PX(10), PY(22) + squash, PR(4), PR(9 - walk * 2), face);
        draw_fill_rect(PX(22), PY(22) + squash, PR(4), PR(7 + walk * 2), face);
        draw_fill_rect(PX(34), PY(22) + squash, PR(4), PR(9 - walk * 2), face);
    }
    int byo = squash + lie;
    draw_circle_filled(PX(18), PY(14) + byo, PR(11), woolsh);
    draw_circle_filled(PX(30), PY(13) + byo, PR(11), woolsh);
    draw_circle_filled(PX(24), PY(10) + byo, PR(12), wool);
    draw_circle_filled(PX(16), PY(12) + byo, PR(9),  wool);
    draw_circle_filled(PX(32), PY(12) + byo, PR(9),  wool);
    if (g_sheep_style == 1) {
        draw_circle_filled(PX(20), PY(11) + byo, PR(3), woolsh);
        draw_circle_filled(PX(29), PY(15) + byo, PR(3), woolsh);
        draw_circle_filled(PX(24), PY(8)  + byo, PR(2), woolsh);
    }
    int hx = (sp->dir >= 0) ? PX(40) : PX(8);
    draw_circle_filled(hx, PY(14) + byo, PR(7), face);
    draw_circle_filled(hx, PY(9)  + byo, PR(6), wool);
    int eye = (sp->dir >= 0) ? hx + PR(2) : hx - PR(2);
    if (sp->blink_t > 0) draw_fill_rect(eye - ui_px(1), PY(13) + byo, ui_px(3), ui_px(1), 0x00FFFFFF);
    else draw_circle_filled(eye, PY(13) + byo, PR(1), 0x00FFFFFF);
    draw_fill_rect((sp->dir >= 0) ? hx - PR(4) : hx + PR(2), PY(12) + byo, PR(3), PR(4), woolsh);
    if (sp->state == 0 && sp->behavior == 1) {          // sleeping: closed eye + Zzz
        draw_fill_rect(eye - ui_px(1), PY(13) + byo, ui_px(3), ui_px(1), face);
        int zx = (sp->dir >= 0) ? PX(44) : PX(0);
        draw_text(zx, by - ui_px(7), "z", CLR_MENU_TEXT);
        draw_text(zx + ui_px(5), by - ui_px(14), "z", CLR_MENU_TEXT);
    } else if (sp->state == 0 && sp->behavior == 3) {   // fart puff behind
        int px = (sp->dir >= 0) ? PX(2) : PX(46);
        draw_circle_filled(px, PY(21) + byo, PR(3), 0x0098C878);
        draw_circle_filled(px + (sp->dir >= 0 ? -ui_px(4) : ui_px(4)), PY(18) + byo, PR(2), 0x00B8E0A0);
    }
    #undef PX
    #undef PY
    #undef PR
}

// --- Sheepdog (border collie): drag to herd; sheep flee from it (#93) -------
// (#40) Same band as the sheep it herds (pet_bounds()), so the two can never
// stand at different "ground" levels: under the marble dock both reach the
// screen's bottom edge, under a bottom-taskbar style both stop on top of it.
static int dog_ground(void) { int b; pet_bounds(NULL, &b); return b - DOG_H; }

int dog_hit(int x, int y) {
    if (!g_dog_enabled) return 0;
    return x >= dog_x && x < dog_x + DOG_W && y >= dog_y && y < dog_y + DOG_H;
}
void dog_grab(int x, int y) { dog_dragging = 1; dog_gdx = x - dog_x; dog_gdy = y - dog_y; }
void dog_drag_to(int x, int y) {
    if (!dog_dragging) return;
    int new_x = x - dog_gdx;
    if (new_x < dog_x) dog_dir = -1;        // dragged left: face left
    else if (new_x > dog_x) dog_dir = 1;    // dragged right: face right
    dog_x = new_x; dog_y = y - dog_gdy;
    if (dog_x < 0) dog_x = 0;
    if (dog_x > g_fb_width - DOG_W) dog_x = g_fb_width - DOG_W;
    // (#40) Bounded by the pet band, same as sheep_drag_to().
    int ptop, pbot; pet_bounds(&ptop, &pbot);
    if (dog_y < ptop) dog_y = ptop;
    if (dog_y > pbot - DOG_H) dog_y = pbot - DOG_H;
}
void dog_release(void) { dog_dragging = 0; }
int dog_is_dragging(void) { return dog_dragging; }

static void dog_update(void) {
    if (dog_x < 0) { dog_x = g_fb_width / 2; dog_y = dog_ground(); }
    dog_frame++;
    if (dog_dragging) return;
    if (dog_y < dog_ground()) { dog_y += 5; if (dog_y > dog_ground()) dog_y = dog_ground(); }
    else if ((dog_frame & 7) == 0) {
        int nx = dog_x + dog_dir * 2;
        // (#40) The dog uses the same wall list as the sheep. It does not
        // climb (it never has, at a screen edge either), it just turns, so
        // the dock stops it the same way the screen edge does instead of
        // letting it walk behind the dock glass and disappear.
        int wall = pet_wall_hit(nx, dog_y, DOG_W, DOG_H);
        if (wall >= 0) {
            const pet_surface_t *s = &g_psurf[wall];
            nx = (dog_dir > 0) ? s->x - DOG_W : s->x + s->w;
            dog_dir = -dog_dir;
        }
        dog_x = nx;
        if (dog_x > (int)g_fb_width - DOG_W) { dog_x = (int)g_fb_width - DOG_W; dog_dir = -1; }
        if (dog_x < 0) { dog_x = 0; dog_dir = 1; }
    }
}

static void dog_draw(void) {
    int bx = dog_x, by = dog_y;
    uint32_t blk = 0x00282828, wht = 0x00F0F0F0;
    int fwd = (dog_dir >= 0);
    int walk = (dog_frame >> 2) & 1;
    // #uiscale: DX/DY wrap the offset-from-origin in ui_px(), same idiom as
    // sheep_one_draw()'s PX/PY above. Every rect/circle SIZE argument gets
    // its own ui_px() call on the whole 1x expression (e.g. "9 - walk * 2").
    #define DX(v) (bx + ui_px(v))
    #define DY(v) (by + ui_px(v))
    // four thin legs
    draw_fill_rect(DX(13), DY(19), ui_px(3), ui_px(9 - walk * 2), blk);
    draw_fill_rect(DX(20), DY(19), ui_px(3), ui_px(7 + walk * 2), blk);
    draw_fill_rect(DX(33), DY(19), ui_px(3), ui_px(7 + walk * 2), blk);
    draw_fill_rect(DX(40), DY(19), ui_px(3), ui_px(9 - walk * 2), blk);
    // elongated body with rounded ends
    draw_fill_rect(DX(13), DY(9), ui_px(30), ui_px(11), blk);
    draw_circle_filled(DX(15), DY(14), ui_px(6), blk);
    draw_circle_filled(DX(41), DY(14), ui_px(6), blk);
    draw_fill_rect(DX(17), DY(16), ui_px(22), ui_px(4), wht);          // white underbelly
    // raised tail at the rear
    if (fwd) { draw_fill_rect(DX(8), DY(5), ui_px(3), ui_px(9), blk); draw_fill_rect(DX(6), DY(4), ui_px(4), ui_px(3), blk); }
    else     { draw_fill_rect(DX(45), DY(5), ui_px(3), ui_px(9), blk); draw_fill_rect(DX(46), DY(4), ui_px(4), ui_px(3), blk); }
    // neck + head at the front
    int hx = fwd ? DX(46) : DX(10);
    draw_fill_rect(fwd ? DX(40) : DX(14), DY(7), ui_px(6), ui_px(9), blk);   // neck
    draw_circle_filled(hx, DY(8), ui_px(6), blk);                       // head
    // snout poking forward + nose
    if (fwd) { draw_fill_rect(hx + ui_px(3), DY(9), ui_px(8), ui_px(4), blk); draw_fill_rect(hx + ui_px(10), DY(9), ui_px(2), ui_px(3), blk); }
    else     { draw_fill_rect(hx - ui_px(11), DY(9), ui_px(8), ui_px(4), blk); draw_fill_rect(hx - ui_px(12), DY(9), ui_px(2), ui_px(3), blk); }
    // pointy ears
    draw_fill_rect(hx - ui_px(4), DY(0), ui_px(3), ui_px(5), blk);
    draw_fill_rect(hx + ui_px(1), DY(0), ui_px(3), ui_px(5), blk);
    // white face blaze + eye
    draw_circle_filled(hx, DY(6), ui_px(2), wht);
    draw_circle_filled(fwd ? hx + ui_px(2) : hx - ui_px(2), DY(6), ui_px(1), 0x00101010);
    #undef DX
    #undef DY
}

// ===========================================================================
// #274: three new desktop widgets (System Monitor, Timer/Stopwatch, World
// Time). Each is a normal draggable/lockable/theme-aware widget that the
// widget framework owns: position + drag + lock + right-click + visibility +
// persistence are all handled exactly like the clock/calendar via widget_box(),
// widget_lock_ptr(), widget_vis_ptr() and the widget_registry().
//   widget id 6 = System Monitor, 7 = Timer/Stopwatch, 8 = World Time.
// ===========================================================================

// Small helper: int -> decimal string (no libc itoa in this freestanding unit).
static char *w_itoa(char *b, int v) {
    int i = 0, neg = 0; char t[12];
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) t[i++] = '0';
    while (v) { t[i++] = '0' + v % 10; v /= 10; }
    int j = 0; if (neg) b[j++] = '-';
    while (i) b[j++] = t[--i];
    b[j] = '\0';
    return b;
}

// --- System Monitor mini (id 6): live CPU / RAM / Net bars + CPU sparkline ---
int g_show_sysmon = 0;
int g_sysmon_x = -1, g_sysmon_y = -1, g_sysmon_locked = 0;
#define SYSMON_W   ui_px(188)
#define SYSMON_H   ui_px(162)
#define SPARK_N    48
static unsigned char s_spark_cpu[SPARK_N];   // ring buffer of recent CPU %
static int s_spark_head = 0, s_spark_filled = 0;
static int s_sysmon_tick = 0;
static int s_cpu_pct = 0, s_ram_pct = 0, s_net_pct = 0;
static unsigned int s_cpu_cores[65];          // [0]=count, [1..]=per-core % (#279)
static int s_cpu_ncores = 1;

static void sysmon_sample(void) {
    // #102: read the SAME CPU sample the taskbar gauge uses so the widget CPU%
    // and the taskbar CPU% never disagree (single accurate source).
    s_cpu_pct = taskbar_cpu_snapshot(s_cpu_cores, &s_cpu_ncores);
    if (s_cpu_pct < 0) s_cpu_pct = 0;
    if (s_cpu_pct > 100) s_cpu_pct = 100;
    if (s_cpu_ncores < 1) s_cpu_ncores = 1;
    if (s_cpu_ncores > 64) s_cpu_ncores = 64;
    unsigned long total = 0, used = 0;
    sys_get_mem_info(&total, &used);
    s_ram_pct = (total > 0) ? (int)(used * 100UL / total) : 0;
    if (s_ram_pct > 100) s_ram_pct = 100;
    static unsigned long s_last_bytes = 0; static int primed = 0;
    unsigned long now_bytes = get_net_bytes();
    if (!primed) { primed = 1; s_net_pct = 0; }
    else {
        unsigned long d = now_bytes - s_last_bytes;     // bytes since last sample
        s_net_pct = (int)((d * 100UL) / 6250000UL);     // ~half-Gbit window full-scale
        if (s_net_pct > 100) s_net_pct = 100;
    }
    s_last_bytes = now_bytes;
    s_spark_cpu[s_spark_head] = (unsigned char)s_cpu_pct;
    s_spark_head = (s_spark_head + 1) % SPARK_N;
    if (s_spark_filled < SPARK_N) s_spark_filled++;
}

// One labeled meter row: label, colored fill bar, value %.
// #uiscale (report 2, "the widgets ... didn't scale yet"): SYSMON_W/H are
// scaled (their #define, above); every offset/size literal below is now
// ui_px()'d to match, so the card's CONTENT grows with its box instead of
// staying pinned to its 1x layout inside a bigger box. Byte-identical at
// 100% (ui_px is the identity there).
static void sysmon_bar(int x, int y, int w, const char *label, int pct, uint32_t col) {
    draw_text(x, y, label, CLR_MENU_TEXT);
    char vb[8]; w_itoa(vb, pct);
    int vl = text_width(vb) + text_width("%");
    char vv[10]; int vi = 0; for (int k = 0; vb[k]; k++) vv[vi++] = vb[k]; vv[vi++]='%'; vv[vi]=0;
    draw_text(x + w - vl, y, vv, CLR_MENU_TEXT);
    // #uiscale FIX: FONT_CHAR_H is already ui_px()'d (compositor.h) - wrapping
    // it in ui_px() again double-scaled this offset above 100%. Found while
    // converting the rest of this file's gadgets to the same convention.
    int by = y + FONT_CHAR_H + ui_px(1), bh = ui_px(7);
    draw_fill_rect(x, by, w, bh, 0x00202830);
    draw_rect_outline(x, by, w, bh, CLR_MENU_BORDER);
    int fw = pct * (w - 2) / 100; if (fw < 0) fw = 0; if (fw > w - 2) fw = w - 2;
    draw_fill_rect(x + 1, by + 1, fw, bh - 2, col);
}

static void widget_sysmon(int x, int y) {
    // Sampling side-effect is suppressed in draw-only (idle per-rect) mode; the
    // idle path advances the sample once via sysmon_tick_sample().
    if (!g_widgets_draw_only && (s_sysmon_tick++ % 15 == 0)) sysmon_sample();
    int w = SYSMON_W, h = SYSMON_H;
    draw_rounded_rect(x, y, w, h, ui_px(8), CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    draw_rounded_rect(x, y, ui_px(4), h, ui_px(2), 0x0066C0FF);   // #159 sect 3: left accent bar
    draw_text(x + ui_px(12), y + ui_px(8), "System Monitor", readable_accent(0x0066C0FF, CLR_MENU_BG));
    int ix = x + ui_px(12), iw = w - ui_px(24);
    sysmon_bar(ix, y + ui_px(26), iw, "CPU", s_cpu_pct, 0x0050C050);
    // Per-core strip (#279): one vertical bar per core, height = that core's %.
    {
        int cy = y + ui_px(60), ch = ui_px(16);
        draw_text(ix, cy, "Cores", CLR_MENU_TEXT);
        int bx0 = ix + text_width("Cores") + ui_px(6);
        int bw_avail = iw - (bx0 - ix);
        int nc = s_cpu_ncores; if (nc < 1) nc = 1;
        int gap = (nc > 1) ? ui_px(1) : 0;
        int cbw = (bw_avail - gap * (nc - 1)) / nc; if (cbw < 1) cbw = 1;
        int cy0 = ui_px(2);
        for (int i = 0; i < nc; i++) {
            int pct = (int)s_cpu_cores[1 + i];
            if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            int bx = bx0 + i * (cbw + gap);
            draw_fill_rect(bx, cy - cy0, cbw, ch, 0x00202830);
            int fh = pct * (ch - cy0) / 100; if (fh < 0) fh = 0;
            // green<60, amber<85, red otherwise
            uint32_t col = pct < 60 ? 0x0050C050 : pct < 85 ? 0x00E0C040 : 0x00E05050;
            if (fh > 0) draw_fill_rect(bx, cy - cy0 + (ch - ui_px(1) - fh), cbw, fh, col);
        }
    }
    sysmon_bar(ix, y + ui_px(82), iw, "RAM", s_ram_pct, 0x000088CC);
    // CPU sparkline strip across the bottom.
    int gx = ix, gy = y + ui_px(116), gw = iw, gh = ui_px(30);
    draw_fill_rect(gx, gy, gw, gh, 0x00161C22);
    draw_rect_outline(gx, gy, gw, gh, CLR_MENU_BORDER);
    int n = s_spark_filled;
    for (int i = 0; i < n; i++) {
        int idx = (s_spark_head - n + i + SPARK_N * 2) % SPARK_N;
        int v = s_spark_cpu[idx];
        int bx = gx + ui_px(1) + i * (gw - ui_px(2)) / SPARK_N;
        int bh = v * (gh - ui_px(2)) / 100; if (bh < 1) bh = 1;
        draw_fill_rect(bx, gy + gh - ui_px(1) - bh, (gw - ui_px(2)) / SPARK_N + 1, bh, 0x0040A0E0);
    }
    // Net indicator dot (top-right) reflecting current throughput.
    uint32_t nd = s_net_pct > 50 ? 0x00FF8040 : s_net_pct > 5 ? 0x00FFD040 : 0x00608060;
    draw_circle_filled(x + w - ui_px(14), y + ui_px(12), ui_px(4), nd);
}

// --- Timer / Stopwatch (id 7): stopwatch + countdown, uses uptime_ms() ------
int g_show_timer = 0;
int g_timer_x = -1, g_timer_y = -1, g_timer_locked = 0;
#define TIMER_W  ui_px(176)
#define TIMER_H  ui_px(96)
// mode 0 = stopwatch (counts up), 1 = countdown timer.
static int s_tmr_mode = 0;
static int s_tmr_running = 0;
static unsigned long s_tmr_base = 0;       // uptime_ms at last (re)start
static unsigned long s_tmr_acc = 0;        // accumulated ms while paused
static unsigned long s_tmr_target = 60000; // countdown preset (ms), default 1:00

static unsigned long tmr_elapsed(void) {
    unsigned long e = s_tmr_acc;
    if (s_tmr_running) e += uptime_ms() - s_tmr_base;
    return e;
}
// remaining ms for countdown (clamped at 0)
static unsigned long tmr_remaining(void) {
    unsigned long e = tmr_elapsed();
    return (e >= s_tmr_target) ? 0 : (s_tmr_target - e);
}
static void tmr_start(void) { if (!s_tmr_running) { s_tmr_base = uptime_ms(); s_tmr_running = 1; } }
static void tmr_pause(void) { if (s_tmr_running) { s_tmr_acc += uptime_ms() - s_tmr_base; s_tmr_running = 0; } }
static void tmr_reset(void) { s_tmr_running = 0; s_tmr_acc = 0; s_tmr_base = uptime_ms(); }

// Format ms as M:SS.t (tenths) into buf.
static void tmr_fmt(char *buf, unsigned long ms) {
    unsigned long tenths = (ms / 100) % 10;
    unsigned long secs = (ms / 1000) % 60;
    unsigned long mins = (ms / 60000);
    if (mins > 999) mins = 999;
    int i = 0; char t[6];
    w_itoa(t, (int)mins); for (int k = 0; t[k]; k++) buf[i++] = t[k];
    buf[i++] = ':';
    buf[i++] = '0' + (secs / 10); buf[i++] = '0' + (secs % 10);
    buf[i++] = '.';
    buf[i++] = '0' + (char)tenths;
    buf[i] = '\0';
}

// Button rects within the widget (computed from top-left x,y). Shared by
// draw (widget_timer) and hit-test (timer_click) so the two cannot drift.
// #uiscale: every literal here is now ui_px()'d, byte-identical at 100%.
static void tmr_btn_rect(int x, int y, int i, int *bx, int *by, int *bw, int *bh) {
    int n = 3, pad = ui_px(8), gap = ui_px(6);
    int tw = (TIMER_W - pad * 2 - gap * (n - 1)) / n;
    *bx = x + pad + i * (tw + gap);
    *by = y + TIMER_H - ui_px(30);
    *bw = tw; *bh = ui_px(22);
}
static void widget_timer(int x, int y) {
    int w = TIMER_W, h = TIMER_H;
    draw_rounded_rect(x, y, w, h, ui_px(8), CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    draw_rounded_rect(x, y, ui_px(4), h, ui_px(2), 0x00FFC850);   // #159 sect 3: left accent bar
    const char *title = s_tmr_mode ? "Countdown" : "Stopwatch";
    draw_text(x + ui_px(12), y + ui_px(8), title, readable_accent(0x00FFC850, CLR_MENU_BG));
    unsigned long ms = s_tmr_mode ? tmr_remaining() : tmr_elapsed();
    char tb[16]; tmr_fmt(tb, ms);
    uint32_t tc = (s_tmr_mode && ms == 0) ? 0x00FF6060 : readable_ink(CLR_MENU_BG);
    draw_text_centered(x + w / 2, y + ui_px(26), tb, tc);
    static const char *lbl[3];
    lbl[0] = s_tmr_running ? "Pause" : "Start";
    lbl[1] = "Reset";
    lbl[2] = s_tmr_mode ? "Watch" : "Timer";
    for (int i = 0; i < 3; i++) {
        int bx, by, bw, bh; tmr_btn_rect(x, y, i, &bx, &by, &bw, &bh);
        draw_fill_rect(bx, by, bw, bh, CLR_MENU_ITEM_HOVER);
        draw_rect_outline(bx, by, bw, bh, CLR_MENU_BORDER);
        // #uiscale FIX: same double-scale as sysmon_bar() above - FONT_CHAR_H
        // is already scaled.
        draw_text_centered(bx + bw / 2, by + (bh - FONT_CHAR_H) / 2, lbl[i], CLR_MENU_TEXT);
    }
}
// Handle a click inside the timer widget (returns 1 if a button was hit).
static int timer_click(int x, int y) {
    int wx = g_timer_x, wy = g_timer_y;
    for (int i = 0; i < 3; i++) {
        int bx, by, bw, bh; tmr_btn_rect(wx, wy, i, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            if (i == 0) { if (s_tmr_running) tmr_pause(); else tmr_start(); }
            else if (i == 1) tmr_reset();
            else { s_tmr_mode ^= 1; tmr_reset(); }
            return 1;
        }
    }
    return 0;
}

// --- World Time (id 8): three configurable timezone clocks -------------------
int g_show_worldtime = 0;
int g_worldtime_x = -1, g_worldtime_y = -1, g_worldtime_locked = 0;
#define WT_W   ui_px(188)
#define WT_H   ui_px(96)
// WT_ZONES is defined in compositor.h (shared with profile.c).
// Persisted: per-zone UTC offset in minutes and a 3-char label. The compositor
// clock is treated as UTC reference; offsets shift it per zone. (No DST.)
int g_wt_off[WT_ZONES] = { 0, -300, 540 };   // UTC, New York(-5), Tokyo(+9)
static const char *s_wt_lbl[WT_ZONES] = { "UTC", "NYC", "TYO" };

// Apply a signed minute offset to UTC and render HH:MM.
//
// #49 DELIBERATELY NOT tz_local_hms(): this widget is UTC-referenced by
// construction (its own g_wt_off[] rows are offsets FROM UTC, and its first row
// is literally labelled "UTC"). Adding the session timezone here would apply
// two offsets and make every row wrong by the local offset. The raw RTC read is
// the correct primitive for this one widget, and the same is true of the World
// Clock app (userland/apps/clock).
static void wt_fmt(char *buf, int off_min) {
    long rtc = sys_get_rtc_time();
    int h = (int)((rtc >> 16) & 0xFF), m = (int)((rtc >> 8) & 0xFF);
    int total = h * 60 + m + off_min;
    total %= (24 * 60); if (total < 0) total += 24 * 60;
    int oh = total / 60, om = total % 60;
    buf[0] = '0' + (oh / 10); buf[1] = '0' + (oh % 10);
    buf[2] = ':';
    buf[3] = '0' + (om / 10); buf[4] = '0' + (om % 10);
    buf[5] = '\0';
}
static void widget_worldtime(int x, int y) {
    int w = WT_W, h = WT_H;
    draw_rounded_rect(x, y, w, h, ui_px(8), CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    draw_rounded_rect(x, y, ui_px(4), h, ui_px(2), 0x0066FF99);   // #159 sect 3: left accent bar
    draw_text(x + ui_px(12), y + ui_px(8), "World Time", readable_accent(0x0066FF99, CLR_MENU_BG));
    int row0 = y + ui_px(26), rh = (h - ui_px(32)) / WT_ZONES;
    for (int z = 0; z < WT_ZONES; z++) {
        int ry = row0 + z * rh;
        draw_text(x + ui_px(12), ry, s_wt_lbl[z], CLR_MENU_TEXT);
        char tb[8]; wt_fmt(tb, g_wt_off[z]);
        draw_text(x + w - text_width(tb) - ui_px(12), ry, tb, readable_ink(CLR_MENU_BG));
    }
}

// (#282) Uptime widget (id 9): shows time since boot via uptime_ms().
int g_show_uptime = 1;   // default ON, 2026-08-18 owner decision - see
                          // setup/main.rs WIDX_UPTIME / WIDGETS_DEFAULT_MASK
int g_uptime_x = -1, g_uptime_y = -1, g_uptime_locked = 0;
#define UPT_W   ui_px(188)
#define UPT_H   ui_px(72)
static int upt_num(char *o, unsigned long v) {
    char t[12]; int n = 0;
    if (v == 0) { o[0] = '0'; return 1; }
    while (v) { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for (int k = 0; k < n; k++) o[k] = t[n - 1 - k];
    return n;
}
static int upt_2d(char *o, unsigned long v) {
    o[0] = (char)('0' + (int)((v / 10) % 10));
    o[1] = (char)('0' + (int)(v % 10));
    return 2;
}
static void widget_uptime(int x, int y) {
    int w = UPT_W, h = UPT_H;
    draw_rounded_rect(x, y, w, h, ui_px(8), CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    draw_rounded_rect(x, y, ui_px(4), h, ui_px(2), 0x00FFB060);   // #159 sect 3: left accent bar
    draw_text(x + ui_px(12), y + ui_px(8), "Uptime", readable_accent(0x00FFB060, CLR_MENU_BG));
    unsigned long sec = (unsigned long)(uptime_ms() / 1000UL);
    unsigned long d = sec / 86400UL; sec %= 86400UL;
    unsigned long hh = sec / 3600UL;  sec %= 3600UL;
    unsigned long mm = sec / 60UL;    unsigned long ss = sec % 60UL;
    char buf[40]; int i = 0;
    if (d > 0) { i += upt_num(buf + i, d); buf[i++] = 'd'; buf[i++] = ' '; }
    i += upt_2d(buf + i, hh); buf[i++] = ':';
    i += upt_2d(buf + i, mm); buf[i++] = ':';
    i += upt_2d(buf + i, ss); buf[i] = 0;
    draw_text(x + ui_px(12), y + ui_px(34), buf, readable_ink(CLR_MENU_BG));
}

// --- Public entry ---------------------------------------------------------
// Persisted widget positions (-1 = use default on first render). ALL widgets
// are draggable, lockable and have a right-click menu. Widget ids:
//   0 clock, 1 calendar, 2 weather, 3 crypto, 4 stocks, 5 digital clock,
//   6 system monitor, 7 timer/stopwatch, 8 world time.
int g_clock_cx = -1, g_clock_cy = -1, g_cal_x = -1, g_cal_y = -1;
int g_clock_locked = 0, g_cal_locked = 0;   // per-widget lock (persisted)
// Digital clock widget (id 5) - state in clock.c, drawn by digclk_draw().
// #235: g_digclk_12h is GONE. The 12/24-hour format is /CONFIG/SETTINGS.CFG key
// 'h' (Settings' clock.use_24hour contract row), read by clock.c through the one
// shared libc/settingscfg.c reader. This widget no longer keeps a private copy
// and its menu no longer offers a private switch; the "Date & Time..." item
// below deep-links to the one control, which the taskbar clock also follows.
extern int g_show_digclock, g_digclk_x, g_digclk_y, g_digclk_locked, g_digclk_secs, g_digclk_style;
void digclk_geom(int *w, int *h);
void digclk_draw(int x, int y);
static int s_digclk_w = 0, s_digclk_h = 0;
// #81-83 info cards: top-left positions (-1 = default top-center row) + locks.
int g_weather_x = -1, g_weather_y = -1, g_crypto_x = -1, g_crypto_y = -1, g_stocks_x = -1, g_stocks_y = -1;
int g_weather_locked = 0, g_crypto_locked = 0, g_stocks_locked = 0;
static int s_clk_r = 44, s_cal_w = 196, s_cal_h = 0;
#define CARD_W ui_px(248)
// Verbosity per card: 1 = detailed (extra lines), 0 = compact. Persisted.
int g_weather_verbose = 1, g_crypto_verbose = 1, g_stocks_verbose = 1;
static int g_wdrag = -1, g_wdx = 0, g_wdy = 0;   // -1 none; 0..4 widget id

// Card descriptor arrays (index 0..2 = weather, crypto, stocks; widget id = 2+i)
static int *s_card_x[3]    = { &g_weather_x, &g_crypto_x, &g_stocks_x };
static int *s_card_y[3]    = { &g_weather_y, &g_crypto_y, &g_stocks_y };
static int *s_card_lock[3] = { &g_weather_locked, &g_crypto_locked, &g_stocks_locked };
static int *s_card_vis[3]  = { &g_show_weather, &g_show_crypto, &g_show_stocks };
static int *s_card_verbose[3] = { &g_weather_verbose, &g_crypto_verbose, &g_stocks_verbose };

// ===========================================================================
// (#236) A DESKTOP WIDGET'S FOOTPRINT IS A FUNCTION OF ITS SETTINGS, NEVER OF
// WHETHER ITS DATA ARRIVED.
//
// MEASURED (golden 2054 = dev c537f63c, throwaway VM <vmid>, 1280x800, fresh
// first boot through the OOBE wizard, positions read back out of the
// /HOME/JAMES/UIPROFIL.YML that boot wrote): Calendar y=14 h=158, Weather
// y=184, Uptime y=314, Home Assistant y=398. widgets_layout_rail_defaults()
// had correctly reserved 118px for Weather, but draw_weather_card() sized
// itself from the PARSED PAYLOAD
//        h = (verbose && nf >= 9) ? 118 : 64
// and /WEATHER.TXT did not exist yet, so the card PAINTED 64px tall. Result,
// exactly as reported: the card looks glued to the Calendar 12px above it and
// adrift from Uptime 66px below it, and every dimension the rest of this file
// derives from s_card_h[0] (drag box, damage rect, the packing cursor for the
// next widget in the column) is wrong for as long as the fetch has not landed.
//
// The shallow bug is the 64. The real one is that a widget whose SIZE depends
// on a network fetch makes the whole desktop layout non-deterministic: two
// boots of the same machine lay out differently depending on whether DHCP and
// the netinfo fetch beat the first frame, and a position baked into
// UIPROFIL.YML under one outcome is stale under the other. That is a
// permanent supply of "it looked fine yesterday" reports, and it is also what
// lets a stale-but-right-aligned stored y sit INSIDE the widget above it
// (the overlap #213's rail_pos_trusted() cannot see - now closed too).
//
// THE RULE, for every card in this file:
//
//   ONE pure function per card computes its height from CONFIGURATION ONLY
//   (verbosity, display mode, how many tickers the USER asked for). The draw
//   path and the layout pass BOTH call that one function, so the reserved
//   slot and the painted box cannot disagree. A card with no data draws an
//   explicit empty state INSIDE its full footprint; it never collapses.
//
// This also deletes the s_card_h[]/s_ha_h "dynamic per-card height" trackers
// outright. Those existed only to let the rest of the file find out what the
// last draw happened to choose, which is precisely the coupling that made the
// first frame (nothing drawn yet) and every no-data frame wrong.
// ===========================================================================
static int weather_card_h(void) { return g_weather_verbose ? 118 : 64; }

// Crypto/Stocks: how many rows the user ASKED FOR, from the same
// /CRYPTOID.TXT and /STOCKID.TXT this widget's own Settings dialog writes
// (wcfg_path0()/wcfg_def0() below) and the netinfo service fetches from -
// never how many the fetch came back with. Cached; refreshed on the netinfo
// throttle, so the draw thread does no I/O it was not already doing (#426:
// async-fetch-then-cache, a small flat-file read, never a poll or a wait).
#define QUOTE_ROWS_MAX 8
static int s_quote_rows[3] = { 1, 2, 2 };   // [0] unused (Weather), 1 crypto, 2 stocks
static void quote_rows_refresh(void);       // fwd; defined with wcfg_path0()/wcfg_def0()
static int quote_card_h(int rows) { return ui_px(30) + rows * ui_px(18) + ui_px(6); }

// The one height accessor for info card `c` (0 weather, 1 crypto, 2 stocks).
//
// The lazy one-shot matters: s_quote_rows starts at the built-in {1,2,2}, and
// the FIRST caller of this function is whichever of the rail-placement pass,
// widget_box() or the damage collector happens to run first on frame 1 - which
// may be before netinfo_refresh() has read /CRYPTOID.TXT. A user with four
// coins configured would then have had 2 rows reserved for them exactly once,
// at first placement, which is the same never-drawn-yet staleness this whole
// change removes, reintroduced through the back door. Seeding it here makes
// "the first read is the configured value" true for every caller regardless of
// order. It is two small file reads, once per process, off the same flat-file
// hand-off the cards already use - no wait, no poll (#426).
static int s_quote_rows_ready = 0;
static int card_h(int c) {
    if (!s_quote_rows_ready) { s_quote_rows_ready = 1; quote_rows_refresh(); }
    return c == 0 ? weather_card_h() : quote_card_h(s_quote_rows[c]);
}

// Per-widget right-click menu.
static int g_wmenu = -1, g_wmenu_x = 0, g_wmenu_y = 0;
#define WMENU_W ui_px(124)
#define WMENU_IH ui_px(22)

void widget_settings_open(int id);   // forward decl (settings dialog, below)

// ===========================================================================
// #414 Home Assistant desktop widget (id 10). A normal draggable / lockable /
// hideable card that shows the live state of one HA entity. The blocking HTTP
// is done by the background haservice (writes /HA0.TXT = "entity|friendly|state|
// unit|domain"); this widget only reads that cache - it never touches the
// network on the draw thread (#211/#381 rule). Its Settings dialog is an entity
// picker (search + domain filter) over the /HALIST.TXT catalog.
// ===========================================================================
#define HA_ID 10
#define HA_W  ui_px(224)
int g_show_ha = 1;                 // flagship widget: shown by default
                                    // also the 2026-08-18 owner default (setup/main.rs WIDX_HA)
int g_ha_x = -1, g_ha_y = -1, g_ha_locked = 0;
static char s_ha_entity[96]="", s_ha_friendly[96]="", s_ha_state[64]="", s_ha_unit[24]="", s_ha_domain[24]="";
static char s_ha_dclass[24]="";    // #723 device_class (6th /HA0.TXT field)
static char s_ha_cat[200000];      // /HALIST.TXT catalog for the picker
static int  s_ha_cat_len = 0;

// #419 per-widget display config (mode + rename + gauge range + sparkline).
//   mode 0 = value + unit (default)   1 = large number
//        2 = on/off/state badge       3 = gauge/bar (numeric, auto min..max)
int g_ha_mode = 0;                     // persisted (profile: hamode)
int g_ha_min  = 0, g_ha_max = 100;     // gauge bounds, auto-grown + persisted
static char g_ha_label[64] = "";       // custom display name (overrides HA name)
static int  s_ha_spark[64];            // normalized 0..1000 sparkline series
static int  s_ha_spark_n = 0;
static int  s_ha_io_tick  = 0;

static void ha_field(const char *line,int f,char *out,int cap){
    int idx=0,oi=0; out[0]=0;
    for(int i=0; line[i] && line[i]!='\n' && line[i]!='\r'; i++){
        if(line[i]=='|'){ if(idx==f){ out[oi]=0; return; } idx++; oi=0; continue; }
        if(idx==f && oi<cap-1) out[oi++]=line[i];
    }
    if(idx==f) out[oi]=0;
}
static void ha_refresh_cache(void){
    int fd=sys_open("/HA0.TXT",0); if(fd<0) return;
    char b[420]; long n=sys_read(fd,b,sizeof(b)-1); sys_close(fd);
    if(n<=0) return;
    b[n]=0;
    ha_field(b,0,s_ha_entity,sizeof(s_ha_entity));
    ha_field(b,1,s_ha_friendly,sizeof(s_ha_friendly));
    ha_field(b,2,s_ha_state,sizeof(s_ha_state));
    ha_field(b,3,s_ha_unit,sizeof(s_ha_unit));
    ha_field(b,4,s_ha_domain,sizeof(s_ha_domain));
    ha_field(b,5,s_ha_dclass,sizeof(s_ha_dclass));   // #723 (absent on an old
                                                      // cache line -> stays "")
}
static char ha_lc(char c){ return (c>='A'&&c<='Z')?(char)(c+32):c; }
// case-insensitive: does haystack contain needle?
static int ha_ci_has(const char *hay,const char *nd){
    if(!nd[0]) return 1;
    for(int i=0; hay[i]; i++){ int j=0; while(nd[j]&&ha_lc(hay[i+j])==ha_lc(nd[j])) j++; if(!nd[j]) return 1; }
    return 0;
}
// Return the nth catalog line (as raw "id|friendly|state") matching filter `flt`
// (substring over the whole line). Returns 1 and fills out, else 0.
static int ha_cat_nth(const char *flt,int n,char *out,int cap){
    int count=0,ls=0;
    for(int i=0;;i++){
        char c=s_ha_cat[i];
        if(c=='\n'||c==0){
            int len=i-ls; if(len>0 && len<cap){
                char line[256]; int k=0; for(int j=ls;j<i&&k<255;j++) line[k++]=s_ha_cat[j]; line[k]=0;
                if(ha_ci_has(line,flt)){ if(count==n){ int m=0; for(;line[m]&&m<cap-1;m++) out[m]=line[m]; out[m]=0; return 1; } count++; }
            }
            ls=i+1; if(c==0) break;
        }
    }
    return 0;
}
static void ha_load_catalog(void){
    s_ha_cat_len=0; s_ha_cat[0]=0;
    int fd=sys_open("/HALIST.TXT",0); if(fd<0) return;
    int got=0;
    while(got<(int)sizeof(s_ha_cat)-1){ long n=sys_read(fd,s_ha_cat+got,sizeof(s_ha_cat)-1-got); if(n<=0) break; got+=(int)n; }
    s_ha_cat[got]=0; s_ha_cat_len=got; sys_close(fd);
}
// Ask the haservice to (re)generate the catalog.
static void ha_request_catalog(void){ int fd=sys_open("/HALIST.REQ",0x0001|0x0040); if(fd>=0){ sys_write(fd,"1\n",2); sys_close(fd);} }
// Persist the chosen entity for slot 0 so the service caches it. O_TRUNC (not
// just O_CREAT) so a PRIOR longer /HAENT.TXT (e.g. leftover multi-entity
// lines past this write's padding) can never survive underneath the new
// single-entity selection - haservice reads up to 8 lines from this file and
// refreshes a slot per line, so stale trailing entities would otherwise keep
// being refreshed even after the user picked a single entity here.
static void ha_write_entity(const char *eid){
    int fd=sys_open("/HAENT.TXT",0x0001|0x0040|0x0200); if(fd<0) return;
    char out[128]; int i=0; for(;eid[i]&&i<95;i++) out[i]=eid[i]; out[i++]='\n';
    sys_write(fd,out,i); sys_close(fd);
    int n=0; for(;eid[n]&&n<95;n++) s_ha_entity[n]=eid[n]; s_ha_entity[n]=0;
}
// #419 Load the custom display label from /HALABEL.TXT (blank => use HA name).
static void ha_load_label(void){
    int fd=sys_open("/HALABEL.TXT",0); if(fd<0){ g_ha_label[0]=0; return; }
    char b[80]; long n=sys_read(fd,b,sizeof(b)-1); sys_close(fd);
    if(n<=0){ g_ha_label[0]=0; return; }
    b[n]=0; int i=0; for(;b[i]&&b[i]!='\n'&&b[i]!='\r'&&i<63;i++) g_ha_label[i]=b[i]; g_ha_label[i]=0;
    if(g_ha_label[0]==' ') g_ha_label[0]=0;                // padded/blank => none
}
static void ha_write_label(const char *lbl){
    int fd=sys_open("/HALABEL.TXT",0x0001|0x0040); if(fd<0) return;
    char out[80]; int i=0; for(;lbl[i]&&i<63;i++) out[i]=lbl[i]; out[i++]='\n';
    while(i<72) out[i++]=' '; out[i++]='\n';               // pad over any old longer label
    sys_write(fd,out,i); sys_close(fd);
    int n=0; for(;lbl[n]&&n<63;n++) g_ha_label[n]=lbl[n]; g_ha_label[n]=0;
}
// #419 Load the sparkline series (/HAHIST0.TXT: line1 entity_id, line2 ints 0..1000).
static void ha_load_spark(void){
    s_ha_spark_n=0;
    int fd=sys_open("/HAHIST0.TXT",0); if(fd<0) return;
    char b[640]; long n=sys_read(fd,b,sizeof(b)-1); sys_close(fd);
    if(n<=0) return; b[n]=0;
    int i=0; char hid[96]; int k=0; for(;b[i]&&b[i]!='\n'&&k<95;i++) hid[k++]=b[i]; hid[k]=0;
    if(b[i]=='\n') i++;
    // Ignore a series cached for a different entity than the one we display.
    if(s_ha_entity[0]){ int j=0; for(;hid[j]&&s_ha_entity[j];j++){ if(hid[j]!=s_ha_entity[j]) return; }
                        if(hid[j]!=s_ha_entity[j]) return; }
    while(b[i] && s_ha_spark_n<64){
        while(b[i]==' '||b[i]=='\n'||b[i]=='\r') i++;
        if(b[i]<'0'||b[i]>'9') break;
        int v=0; while(b[i]>='0'&&b[i]<='9'){ v=v*10+(b[i]-'0'); i++; }
        if(v>1000)v=1000; s_ha_spark[s_ha_spark_n++]=v;
    }
}
// #419 Parse the leading numeric part of the state into value*10 (one decimal).
// Returns 1 if the state is numeric.
static int ha_state_num(long *out){
    const char *s=s_ha_state; int i=0,neg=0; if(s[0]=='-'){neg=1;i=1;}
    if(!(s[i]>='0'&&s[i]<='9')) return 0;
    long v=0; for(;s[i]>='0'&&s[i]<='9';i++) v=v*10+(s[i]-'0');
    long d=0; if(s[i]=='.'&&s[i+1]>='0'&&s[i+1]<='9') d=s[i+1]-'0';
    long m=v*10+d; *out = neg?-m:m; return 1;
}
// #419 classify a non-numeric state as "active" (green) vs inactive (gray).
// #723 Map a formatter semantic bucket to the active theme's semantic token
// (THEME_COLOR_SUCCESS/WARNING/ERROR/INFO/MUTED - see ../../libc/theme.h).
// These are the SAME tokens every other themed surface draws from; no new
// hardcoded literal is introduced for what is, precisely, what that token
// table exists for. UNAVAILABLE deliberately maps to MUTED, not ERROR: most
// of an instance's "unavailable" entities are disabled/removed integrations,
// not active problems, and painting all of them alarm-red is noise fatigue -
// the WARN triangle icon shape still marks the reading as suspect.
static uint32_t ha_sem_color(ha_semantic_t sem){
    switch(sem){
        case HA_SEM_ACTIVE: return theme_color(THEME_COLOR_SUCCESS);
        case HA_SEM_WARN:   return theme_color(THEME_COLOR_WARNING);
        case HA_SEM_ALERT:  return theme_color(THEME_COLOR_ERROR);
        case HA_SEM_INFO:   return theme_color(THEME_COLOR_INFO);
        case HA_SEM_NEUTRAL:
        case HA_SEM_UNAVAILABLE:
        default:            return theme_color(THEME_COLOR_MUTED);
    }
}
// Truncate `src` to fit `max_w` pixels (appending "...") instead of a blind
// character-count cut, so a long HA friendly name or humanized state never
// overflows the card. `scale`=1 measures with text_width(); >1 uses the
// draw_text_large metric (draw_text_large scale must match what's on screen).
// `sz` <=1 means "measure with the default UI TTF size" (text_width());
// otherwise `sz` is a literal draw_text_ttf() point size to measure AT (mode
// 0/1 HA values pass 28/42 - the exact size they will be painted at). #159
// sect 5a: this used to be a `scale` multiplier measured with
// text_width_large() (the bitmap font's metric) while the paint call was
// switched to draw_text_ttf() at a literal size - a fit computed against one
// font and painted in another either clips early or overflows the card.
static void ha_fit_text(const char *src,int max_w,int sz,char *out,int cap){
    int n=0; for(;src[n]&&n<cap-1;n++) out[n]=src[n]; out[n]=0;
    int w = (sz<=1)? text_width(out) : text_width_ttf(out,sz);
    if (w<=max_w || max_w<=0) return;
    while (n>1){
        n--;
        char t[64]; int m=0; for(;m<n&&m<60;m++) t[m]=out[m];
        t[m++]='.'; t[m++]='.'; t[m++]='.'; t[m]=0;
        int tw = (sz<=1)? text_width(t) : text_width_ttf(t,sz);
        if (tw<=max_w || n<=1){ int i=0; for(;t[i]&&i<cap-1;i++) out[i]=t[i]; out[i]=0; return; }
    }
}
// #419 mini sparkline (line chart) of the cached series in box (x,y,w,h).
static void ha_draw_spark(int x,int y,int w,int h){
    if(s_ha_spark_n<2) return;
    draw_fill_rect(x,y,w,h,0x00161E26);
    draw_rect_outline(x,y,w,h,0x00304556);
    int n=s_ha_spark_n, px=-1,py=-1;
    int inset = ui_px(1), trim = ui_px(3);   // #uiscale: was a bare 1px/3px inset
    for(int i=0;i<n;i++){
        int vx = x+inset + i*(w-trim)/(n-1);
        int vy = y+inset + (h-trim) - s_ha_spark[i]*(h-trim)/1000;
        if(px>=0) wdg_line(px,py,vx,vy,0x0041B0E0);
        px=vx; py=vy;
    }
}
// The single HA accent hue (#159 sect 1b/2b): used raw for the identity bar
// fill and, theme-corrected via readable_accent(), for the title - folding
// what used to be two literals (bar 0x0041B0E0, title 0x00CFE8F5) into one,
// exactly like Weather/Crypto/Stocks each use one accent hue for both.
#define HA_ACCENT 0x0041B0E0

// (#236) The HA card's height, from the display MODE alone. The history row
// used to be present iff s_ha_spark_n >= 2, i.e. iff /HASPARK.TXT had already
// been written: the card grew 32px the moment a series landed and shrank
// again whenever it did not. Home Assistant is currently LAST in the
// canonical rail order, so that never displaced another widget on a default
// desktop - it is the same fault as the Weather one with the blast radius
// hidden by an ordering accident, which is exactly the kind of latent thing
// that surfaces the day someone reorders the rail. The row is now RESERVED by
// mode (every mode except Gauge, which has no history row at all) and drawn
// as an explicit empty frame until there is a series to put in it.
static int ha_card_h(void){
    // #uiscale: FONT_CHAR_H is already ui_px()'d - every OTHER literal below
    // is its own ui_px() call.
    int top  = ui_px(10) + FONT_CHAR_H + ui_px(8);          // below the title
    int valh = (g_ha_mode==1)? (FONT_CHAR_H*3+ui_px(6)) : (g_ha_mode==2)? ui_px(30) : (g_ha_mode==3)? ui_px(30) : (FONT_CHAR_H*2+ui_px(6));
    int sparkh = (g_ha_mode!=3) ? ui_px(26) : 0;
    return top + valh + (sparkh?sparkh+ui_px(6):0) + ui_px(8);
}
// Empty history frame: the same box ha_draw_spark() paints into, with the
// reason it is empty, so the reserved row reads as deliberate rather than as
// a rendering failure.
static void ha_draw_spark_empty(int x,int y,int w,int h){
    draw_fill_rect(x,y,w,h,0x00161E26);
    draw_rect_outline(x,y,w,h,0x00304556);
    const char *m = "No history yet";
    int tw = text_width(m);
    draw_text(x + (w - tw)/2, y + (h - FONT_CHAR_H)/2, m, readable_ink_dim(0x00161E26));
}

static void ha_card_draw(int x,int y){
    // #159 sect 6a: offline gate FIRST, before ha_refresh_cache() touches the
    // /HA0.TXT cache at all. This is what makes "no stale reading" structural
    // rather than a rendering choice - there is no code path below this
    // return that has read s_ha_state/s_ha_friendly/s_ha_entity/... yet.
    if (!widget_net_online()) {
        int W = HA_W;
        int top = ui_px(10) + FONT_CHAR_H + ui_px(8);
        // (#236) ha_card_h(), the SAME height this card has in every other
        // state. This used to compute its own top+valh+8 (no history row),
        // which made "offline" a THIRD distinct footprint on top of the
        // have-spark / no-spark pair.
        int H = ha_card_h();
        draw_rounded_rect(x,y,W,H,ui_px(8),CLR_MENU_BG);
        draw_rect_outline(x,y,W,H,CLR_MENU_BORDER);
        draw_rounded_rect(x,y,ui_px(4),H,ui_px(2),HA_ACCENT);
        // g_ha_label is a local Settings value (never populated by
        // ha_refresh_cache()), so it is safe to read here; the HA friendly
        // name / entity id are NOT (they come from the cache we are not
        // reading), so this falls back to the generic title instead.
        const char *title = g_ha_label[0] ? g_ha_label : "Home Assistant";
        char tt[64]; ha_fit_text(title,W-ui_px(24),1,tt,sizeof(tt));
        draw_text(x+ui_px(12),y+ui_px(10),tt,readable_accent(HA_ACCENT, CLR_MENU_BG));
        widget_draw_offline(x,W,y+top,y+H-ui_px(8));
        return;
    }
    ha_refresh_cache();
    if((s_ha_io_tick++ % 24)==0){ ha_load_label(); ha_load_spark(); }
    // #723 domain/device_class -> humanized text + icon + semantic, once per
    // frame. Never touches the network (ha_refresh_cache() above only reads
    // the /HA0.TXT cache haservice already wrote - #211/#381 rule).
    ha_display_t hd; ha_format_state(s_ha_domain,s_ha_dclass,s_ha_state,s_ha_unit,&hd);
    uint32_t semc = ha_sem_color(hd.sem);
    int W=HA_W;
    int spark_slot = (g_ha_mode!=3);                      // reserved by MODE (#236)
    int have_spark = (s_ha_spark_n>=2 && spark_slot);     // ...filled only when there IS a series
    int top = ui_px(10) + FONT_CHAR_H + ui_px(8);         // below the title
    int valh = (g_ha_mode==1)? (FONT_CHAR_H*3+ui_px(6)) : (g_ha_mode==2)? ui_px(30) : (g_ha_mode==3)? ui_px(30) : (FONT_CHAR_H*2+ui_px(6));
    // No trailing entity-id row (#723: the raw id is noise on the always-
    // visible card - it stays available in the widget's Settings picker and,
    // for an already-bound widget, in the settings header caption below).
    int H = ha_card_h();
    // #159 sect 1b finding 1: was draw_fill_rect(...,0x00202832) / hardcoded
    // border 0x00415A6E - theme-token fill/border, matching Weather/Crypto/
    // Stocks (widgets.c draw_weather_card/draw_quote_card).
    draw_rounded_rect(x,y,W,H,ui_px(8),CLR_MENU_BG);
    draw_rect_outline(x,y,W,H,CLR_MENU_BORDER);
    // #159 sect 1b finding 2: was a 4px TOP bar (draw_fill_rect(x,y,W,4,...));
    // now the family's 4px LEFT bar, matching Weather/Crypto/Stocks/the four
    // redesigned widgets - 8-of-8 consistent instead of 7-of-8.
    draw_rounded_rect(x,y,ui_px(4),H,ui_px(2),HA_ACCENT);
    // Title: custom label overrides HA friendly name (#419 rename). The
    // entity_id fallback only fires before anything has ever loaded (#723:
    // it is not "the name", it is "nothing better to show yet").
    const char *title = g_ha_label[0]?g_ha_label:(s_ha_friendly[0]?s_ha_friendly:(s_ha_entity[0]?s_ha_entity:"Home Assistant"));
    char tt[64]; ha_fit_text(title,W-ui_px(24),1,tt,sizeof(tt));
    // #159 sect 1b finding 3: was a hardcoded 0x00CFE8F5 title color; now
    // readable_accent() like every other reference widget's title.
    draw_text(x+ui_px(12),y+ui_px(10),tt,readable_accent(HA_ACCENT, CLR_MENU_BG));
    int vy = y+top;
    long num; int is_num = ha_state_num(&num);
    int isz = (g_ha_mode==1)?ui_px(32):(g_ha_mode==2)?ui_px(24):(g_ha_mode==3)?ui_px(16):ui_px(28);   // icon size per mode
    if(g_ha_mode==2){                                     // ---- state badge ----
        icon_draw_scaled(hd.icon, x+ui_px(12), vy, isz, semc);
        int bx = x+ui_px(12)+isz+ui_px(8);
        char bt[40]; ha_fit_text(hd.text, W-ui_px(24)-isz-ui_px(8)-ui_px(24), 1, bt, sizeof(bt));
        int bw = text_width(bt)+ui_px(24); if(bw<ui_px(64))bw=ui_px(64); if(bw>W-ui_px(24)-isz-ui_px(8))bw=W-ui_px(24)-isz-ui_px(8);
        draw_fill_rect(bx,vy,bw,ui_px(24),semc);
        draw_rect_outline(bx,vy,bw,ui_px(24),readable_ink_dim(semc));
        draw_text_centered(bx+bw/2,vy+ui_px(4),bt,readable_ink(semc));   // #723 contrast-safe on any theme's semantic color
    } else if(g_ha_mode==3){                              // ---- gauge / bar ----
        long v = is_num? num : 0;
        if(is_num && v/10 > g_ha_max){ g_ha_max = (int)(v/10); }   // auto-grow range
        long lo=(long)g_ha_min*10, hi=(long)g_ha_max*10; if(hi<=lo)hi=lo+10;
        long frac = (v-lo)*1000/(hi-lo); if(frac<0)frac=0; if(frac>1000)frac=1000;
        int gw=W-ui_px(24), gh=ui_px(16);
        draw_fill_rect(x+ui_px(12),vy,gw,gh,0x00161E26);
        draw_fill_rect(x+ui_px(12),vy,(int)((long)gw*frac/1000),gh,0x0041B0E0);
        draw_rect_outline(x+ui_px(12),vy,gw,gh,0x00304556);
        icon_draw_scaled(hd.icon, x+ui_px(12), vy+gh+ui_px(3), isz, semc);
        char val[48]; ha_fit_text(hd.text, W-ui_px(24)-isz-ui_px(6), 1, val, sizeof(val));
        draw_text(x+ui_px(12)+isz+ui_px(6),vy+gh+ui_px(3),val,0x00CFE8F5);
    } else {                                              // ---- value / big ----
        // #159 sect 5: was draw_text_large()/text_width_large() - the sole
        // bitmap-font call in the whole widget family (the "Unknown" bug).
        // Mode 1 ("large number") -> size 42; mode 0 ("value+unit") -> 28,
        // both literal draw_text_ttf() point sizes, not integer scales - they
        // are NOT wrapped in ui_px() here because draw_text_ttf()/
        // text_width_ttf() already scale their own `size` argument
        // internally (main.c #uiscale chokepoint); wrapping here too would
        // be the same double-scale bug fixed at the top of this file.
        int ttf_size = (g_ha_mode==1) ? 42 : 28;
        icon_draw_scaled(hd.icon, x+ui_px(12), vy+(valh-isz)/2, isz, semc);
        int tx = x+ui_px(12)+isz+ui_px(8);
        char val[48]; ha_fit_text(hd.text, W-ui_px(24)-isz-ui_px(8), ttf_size, val, sizeof(val));
        draw_text_ttf(tx,vy,val,ttf_size,readable_ink(CLR_MENU_BG));
    }
    if(spark_slot){
        if(have_spark) ha_draw_spark(x+ui_px(12),vy+valh+ui_px(6),W-ui_px(24),ui_px(20));
        else           ha_draw_spark_empty(x+ui_px(12),vy+valh+ui_px(6),W-ui_px(24),ui_px(20));
    }
}

// Bounding box of widget `id` (top-left). Returns 0 if hidden/unplaced.
static int widget_box(int id, int *bx, int *by, int *bw, int *bh) {
    if (!g_widgets_enabled) return 0;
    if (id == 0) {
        if (!g_show_clock || g_clock_cx < 0) return 0;
        *bx = g_clock_cx - s_clk_r; *by = g_clock_cy - s_clk_r;
        *bw = s_clk_r * 2; *bh = s_clk_r * 2; return 1;
    }
    if (id == 1) {
        if (!g_show_calendar || g_cal_x < 0) return 0;
        *bx = g_cal_x; *by = g_cal_y; *bw = s_cal_w; *bh = s_cal_h; return 1;
    }
    if (id >= 2 && id <= 4) {
        int c = id - 2;
        if (!*s_card_vis[c] || *s_card_x[c] < 0) return 0;
        *bx = *s_card_x[c]; *by = *s_card_y[c]; *bw = CARD_W; *bh = card_h(c); return 1;
    }
    if (id == 5) {
        if (!g_show_digclock || g_digclk_x < 0) return 0;
        *bx = g_digclk_x; *by = g_digclk_y; *bw = s_digclk_w; *bh = s_digclk_h; return 1;
    }
    if (id == 6) {
        if (!g_show_sysmon || g_sysmon_x < 0) return 0;
        *bx = g_sysmon_x; *by = g_sysmon_y; *bw = SYSMON_W; *bh = SYSMON_H; return 1;
    }
    if (id == 7) {
        if (!g_show_timer || g_timer_x < 0) return 0;
        *bx = g_timer_x; *by = g_timer_y; *bw = TIMER_W; *bh = TIMER_H; return 1;
    }
    if (id == 8) {
        if (!g_show_worldtime || g_worldtime_x < 0) return 0;
        *bx = g_worldtime_x; *by = g_worldtime_y; *bw = WT_W; *bh = WT_H; return 1;
    }
    if (id == 9) {
        if (!g_show_uptime || g_uptime_x < 0) return 0;
        *bx = g_uptime_x; *by = g_uptime_y; *bw = UPT_W; *bh = UPT_H; return 1;
    }
    if (id == 10) {
        if (!g_show_ha || g_ha_x < 0) return 0;
        *bx = g_ha_x; *by = g_ha_y; *bw = HA_W; *bh = ha_card_h(); return 1;
    }
    return 0;
}
static int *widget_lock_ptr(int id) {
    if (id == 0) return &g_clock_locked;
    if (id == 1) return &g_cal_locked;
    if (id >= 2 && id <= 4) return s_card_lock[id - 2];
    if (id == 5) return &g_digclk_locked;
    if (id == 6) return &g_sysmon_locked;
    if (id == 7) return &g_timer_locked;
    if (id == 8) return &g_worldtime_locked;
    if (id == 9) return &g_uptime_locked;
    if (id == 10) return &g_ha_locked;
    return 0;
}
static int *widget_vis_ptr(int id) {
    if (id == 0) return &g_show_clock;
    if (id == 1) return &g_show_calendar;
    if (id >= 2 && id <= 4) return s_card_vis[id - 2];
    if (id == 5) return &g_show_digclock;
    if (id == 6) return &g_show_sysmon;
    if (id == 7) return &g_show_timer;
    if (id == 8) return &g_show_worldtime;
    if (id == 9) return &g_show_uptime;
    if (id == 10) return &g_show_ha;
    return 0;
}

int widget_hit(int x, int y) {
    // Topmost first (cards over calendar over clock on overlap).
    for (int id = 10; id >= 0; id--) {
        int bx, by, bw, bh;
        if (!widget_box(id, &bx, &by, &bw, &bh)) continue;
        if (id == 0) {
            int dx = x - g_clock_cx, dy = y - g_clock_cy;
            if (dx*dx + dy*dy <= s_clk_r * s_clk_r) return 0;
        } else if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            return id;
        }
    }
    return -1;
}
void widget_grab(int x, int y) {
    g_wdrag = widget_hit(x, y);
    if (g_wdrag < 0) return;
    // Timer/Stopwatch: a click on one of its buttons acts on the timer and does
    // NOT begin a drag (so the user can press Start/Reset/Mode without moving it).
    if (g_wdrag == 7 && timer_click(x, y)) { g_wdrag = -1; return; }
    int *lk = widget_lock_ptr(g_wdrag);
    if (lk && *lk) { g_wdrag = -1; return; }            // locked: no drag
    int bx, by, bw, bh; widget_box(g_wdrag, &bx, &by, &bw, &bh);
    g_wdx = x - bx; g_wdy = y - by;
}
// (#745) Clamp a widget box into the WORK AREA, not into three guessed
// constants. The old floor of 24 was a stand-in for "some top bar" that
// matched no style exactly (Retro Bench is 20, Lumina 24, XFCE 30) and the -30
// bottom margin matched neither the 36px taskbar nor the 64px docks.
// (#40) The bounds are the WIDGET AREA, not the window work area: under the
// marble dock a widget may be dragged right down to the screen's bottom edge
// (the dock floats over the desktop there and the style's taskbar is at the
// TOP, which this still keeps clear), while an app window still may not.
static void widget_clamp_pos(int bw, int bh, int *nx, int *ny) {
    int wax, way, waw, wah;
    taskbar_widget_area(&wax, &way, &waw, &wah);
    if (*nx > wax + waw - bw) *nx = wax + waw - bw;
    if (*ny > way + wah - bh) *ny = way + wah - bh;
    if (*nx < wax) *nx = wax;      // after the max clamps, so a widget wider
    if (*ny < way) *ny = way;      // than the work area still starts inside it
}

// The single write-back for a widget's top-left. Was inline in widget_drag_to;
// pulled out so the re-clamp path below cannot drift from the drag path.
static void widget_set_pos(int id, int nx, int ny) {
    if (id == 0)       { g_clock_cx = nx + s_clk_r; g_clock_cy = ny + s_clk_r; }
    else if (id == 1)  { g_cal_x = nx; g_cal_y = ny; }
    else if (id == 5)  { g_digclk_x = nx; g_digclk_y = ny; }
    else if (id == 6)  { g_sysmon_x = nx; g_sysmon_y = ny; }
    else if (id == 7)  { g_timer_x = nx; g_timer_y = ny; }
    else if (id == 8)  { g_worldtime_x = nx; g_worldtime_y = ny; }
    else if (id == 9)  { g_uptime_x = nx; g_uptime_y = ny; }
    else if (id == 10) { g_ha_x = nx; g_ha_y = ny; }
    else               { int c = id - 2; *s_card_x[c] = nx; *s_card_y[c] = ny; }
}

void widget_drag_to(int x, int y) {
    if (g_wdrag < 0) return;
    int bx, by, bw, bh; if (!widget_box(g_wdrag, &bx, &by, &bw, &bh)) return;
    int nx = x - g_wdx, ny = y - g_wdy;                 // new top-left
    widget_clamp_pos(bw, bh, &nx, &ny);
    widget_set_pos(g_wdrag, nx, ny);
}

// (#745/#40) Re-clamp every relocatable widget into the CURRENT widget area
// (taskbar_widget_area(), which is the work area except under an overlay dock
// style). A widget position comes out of UIPROFIL.YML, which may have been
// written under a different dock style (or before this fix existed), so "it
// was clamped when it was dragged" is not enough: it has to be re-clamped
// when the reserved edge changes. Called from taskbar_apply_work_area() and,
// per #40, once per frame from widgets_render() so a widget's FIRST-EVER
// default placement is covered too.
void widgets_clamp_to_bounds(void) {
    for (int id = 0; id <= 10; id++) {
        int bx, by, bw, bh;
        if (!widget_box(id, &bx, &by, &bw, &bh)) continue;
        int nx = bx, ny = by;
        widget_clamp_pos(bw, bh, &nx, &ny);
        if (nx != bx || ny != by) {
            widget_set_pos(id, nx, ny);
            g_needs_redraw = true;
        }
    }
}
void widget_release(void) { g_wdrag = -1; }

// --- Per-widget right-click menu (Hide / Lock|Unlock / [Settings]) ---------
void widget_menu_open(int which, int x, int y) { g_wmenu = which; g_wmenu_x = x; g_wmenu_y = y; }
int  widget_menu_is_open(void) { return g_wmenu >= 0; }
static int widget_menu_nitems(void) {
    // #129: digital clock: Hide, Lock, Seconds, Design, Date & Time
    // (the last item deep-links to Settings -> Date and Time - "clock" is one
    // of the three tray/desktop clocks that ticket names; this is how the
    // DESKTOP WIDGET form of the clock reaches it, since DOCK_DEFAULT has no
    // bar-clock text to click - see taskbar.c's bar_clock_click()/g_bar_
    // clock_* for the other 4 dock styles' bar-clock text).
    // #235: the 12/24h item that used to sit at index 2 is GONE. It flipped a
    // PRIVATE g_digclk_12h that the Settings control did not govern, so this
    // menu and Settings could put the desktop clock and the taskbar clock into
    // different formats at the same time. The one control is the "Date &
    // Time..." item's destination; both clocks now follow it.
    if (g_wmenu == 5) return 5;
    if (g_wmenu >= 2 && g_wmenu <= 4) return 3;  // info cards: Hide, Lock, Settings
    if (g_wmenu == 10) return 4;                 // #414/#419 HA: Hide, Lock, Display, Settings
    return 2;                            // all other widgets: Hide, Lock
}

// #uiscale: WMENU_W/WMENU_IH are now scaled (their #define, above); this
// shared "+4" row-start inset is the same value the draw loop and the
// hit-test loop both read, so it cannot drift the way the traymenu.c
// checkbox/slider geometry did before this pass.
#define WMENU_TOP_PAD ui_px(4)
static void widget_menu_geom(int *mx, int *my, int *h) {
    int hh = widget_menu_nitems() * WMENU_IH + WMENU_TOP_PAD;
    int x = g_wmenu_x, y = g_wmenu_y;
    // (local 81) was clamped against the raw framebuffer with no x<0/y<0 floor,
    // so a menu wider/taller than the screen started off-screen and one opened
    // near the dock was painted over it. Shared helper, one definition.
    popup_clamp_to_work_area(WMENU_W, hh, &x, &y);
    *mx = x; *my = y; *h = hh;
}
void widget_menu_render(void) {
    if (g_wmenu < 0) return;
    // #uiscale FIXED: WMENU_W/WMENU_IH (this file) are now scaled, and every
    // item offset below is ui_px()'d to match, so this menu no longer needs
    // the native-text opt-out - it was one of the few widgets.c surfaces
    // small and self-contained enough to convert in this pass.
    int x, y, h; widget_menu_geom(&x, &y, &h);
    int n = widget_menu_nitems();
    draw_fill_rect(x, y, WMENU_W, h, CLR_MENU_BG);
    draw_rect_outline(x, y, WMENU_W, h, CLR_MENU_BORDER);
    int *lk = widget_lock_ptr(g_wmenu);
    static const char *ha_mode_names[4] = { "Value", "Big", "Badge", "Gauge" };
    const char *items[6];
    items[0] = "Hide";
    items[1] = (lk && *lk) ? "Unlock" : "Lock";
    if (g_wmenu == 5) {
        items[2] = g_digclk_secs ? "Hide seconds" : "Show seconds";
        items[3] = "Next design";
        items[4] = "Date & Time...";   // #129/#235: deep-link to THE clock control
    } else if (g_wmenu == 10) {
        static char mbuf[24];
        const char *mn = ha_mode_names[g_ha_mode & 3];
        int q = 0; const char *pfx = "Mode: ";
        for (int j = 0; pfx[j] && q < 20; j++) mbuf[q++] = pfx[j];
        for (int j = 0; mn[j] && q < 23; j++) mbuf[q++] = mn[j];
        mbuf[q] = 0;
        items[2] = mbuf;
        items[3] = "Settings...";
    } else {
        items[2] = "Settings";
    }
    for (int i = 0; i < n; i++)
        draw_text(x + ui_px(12), y + WMENU_TOP_PAD + i * WMENU_IH + ui_px(5), items[i], CLR_MENU_TEXT);
}
int widget_menu_handle(int x, int y, int click) {
    if (g_wmenu < 0) return 0;
    if (!click) return 1;
    int mx, my, h; widget_menu_geom(&mx, &my, &h);
    int n = widget_menu_nitems();
    if (x >= mx && x < mx + WMENU_W && y >= my + WMENU_TOP_PAD && y < my + WMENU_TOP_PAD + n * WMENU_IH) {
        int idx = (y - (my + WMENU_TOP_PAD)) / WMENU_IH;
        int *vis = widget_vis_ptr(g_wmenu);
        int *lk  = widget_lock_ptr(g_wmenu);
        int which = g_wmenu;
        if (idx == 0)      { if (vis) *vis = 0; }                 // Hide
        else if (idx == 1) { if (lk) *lk = !*lk; }               // Lock/Unlock
        // #235: no 12/24h case here any more - see widget_menu_nitems().
        else if (which == 5 && idx == 2) { g_digclk_secs = !g_digclk_secs; }  // seconds
        else if (which == 5 && idx == 3) { g_digclk_style = (g_digclk_style + 1) % 5; } // design
        else if (which == 5 && idx == 4) { g_wmenu = -1; settings_open_panel(SETTINGS_PANEL_DATETIME); return 1; } // #129
        else if (which == 10 && idx == 2) { g_ha_mode = (g_ha_mode + 1) & 3; }   // #419 cycle display mode
        else if (which == 10 && idx == 3) { g_wmenu = -1; widget_settings_open(10); return 1; }
        else if (idx == 2 && which >= 2 && which <= 4) { g_wmenu = -1; widget_settings_open(which); return 1; }
    }
    g_wmenu = -1;
    return 1;
}
int  widget_is_dragging(void) { return g_wdrag >= 0; }

// ===========================================================================
// #81-83: internet info widgets (weather / crypto / stock ticker)
//
// The slow network fetch is done by the background `netinfo` service, which
// writes short result files. These widgets just read the cached files every
// few seconds and draw a row of info cards along the top of the desktop.
// ===========================================================================
// g_show_weather default ON, 2026-08-18 owner decision (see setup/main.rs
// WIDX_WEATHER / WIDGETS_DEFAULT_MASK); Crypto and Stocks were NOT in the
// owner's list and stay OFF, matching WIDGETS_UI indices 11/12 being absent
// from WIDGETS_DEFAULT_MASK.
int g_show_weather = 1, g_show_crypto = 0, g_show_stocks = 0;
static char s_weather[256], s_crypto[256], s_stocks[256];
static int  s_netinfo_tick = 0;

// Validate that a quote/weather payload looks like real card data (CODE,price..
// or loc|cond|...) and NOT some other file's content (e.g. the heartbeat log
// /SVCLOG.TXT, which a stale/short FAT read could surface). A real payload has
// no spaces inside its first token and uses ',' or '|' as field separators.
// #303: this stops "MayteraOS heartbeat service tick=N" from rendering as a
// bogus stock/crypto ticker when the data file is briefly unreadable.
static int netinfo_payload_ok(const char *s) {
    if (!s || !s[0]) return 0;
    // First token (up to first separator) must be short and space-free.
    int i = 0;
    for (; s[i] && s[i] != ',' && s[i] != '|'; i++) {
        if (s[i] == ' ') return 0;            // ticker codes never contain spaces
        if (i >= 16) return 0;                // codes/locations are short
    }
    return 1;
}

static void netinfo_read(const char *path, char *dst, int cap) {
    // Always start from a known-clean buffer so a failed read can never leave
    // stale or uninitialized bytes on screen (#303).
    dst[0] = '\0';
    int fd = sys_open(path, 0);
    if (fd < 0) return;                 // file missing -> leave "" (shows "...")
    char buf[256];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    int i = 0;
    char tmp[256];
    for (; i < (int)sizeof(tmp) - 1 && buf[i] && buf[i] != '\n' && buf[i] != '\r'; i++) tmp[i] = buf[i];
    while (i > 0 && tmp[i - 1] == ' ') i--;   // trim trailing pad
    tmp[i] = '\0';
    if (!netinfo_payload_ok(tmp)) return;     // reject garbage -> leave ""
    // Commit validated payload to dst.
    int j = 0;
    for (; j < cap - 1 && tmp[j]; j++) dst[j] = tmp[j];
    dst[j] = '\0';
}
static void netinfo_refresh(void) {
    netinfo_read("/WEATHER.TXT", s_weather, sizeof(s_weather));
    netinfo_read("/CRYPTO.TXT",  s_crypto,  sizeof(s_crypto));
    netinfo_read("/STOCKS.TXT",  s_stocks,  sizeof(s_stocks));
    quote_rows_refresh();   // (#236) the CONFIGURED row count, same throttle
}

// --- small drawing + parsing helpers --------------------------------------
static void sb(char *d, int *di, int cap, const char *s) { for (int i = 0; s[i] && *di < cap - 1; i++) d[(*di)++] = s[i]; d[*di] = '\0'; }
// Split s on delim into out[maxf][fcap]; returns field count.
static int wsplit(const char *s, char delim, char out[][48], int maxf, int fcap) {
    int fi = 0, ci = 0;
    for (int i = 0; ; i++) {
        char c = s[i];
        if (c == delim || c == '\0') {
            if (fi < maxf) { out[fi][ci] = '\0'; fi++; }
            ci = 0;
            if (c == '\0' || fi >= maxf) break;
        } else if (ci < fcap - 1) out[fi][ci++] = c;
    }
    return fi;
}
// 9px-wide up/down arrow centered at cx, top at y (5 rows).
static void warrow(int cx, int y, int up, uint32_t col) {
    for (int r = 0; r < 5; r++) {
        int wdt = up ? (2 * r + 1) : (2 * (4 - r) + 1);
        draw_fill_rect(cx - wdt / 2, y + r, wdt, 1, col);
    }
}
// All offsets/radii are multiplied by scale s so the icon can be drawn larger.
static void wx_cloud(int x, int y, uint32_t c, int s) {
    draw_circle_filled(x + 7*s,  y + 12*s, 6*s, c);
    draw_circle_filled(x + 14*s, y + 10*s, 7*s, c);
    draw_circle_filled(x + 20*s, y + 13*s, 5*s, c);
    draw_fill_rect(x + 7*s, y + 13*s, 14*s, 5*s, c);
}
// One sun ray from radius ri to ro along unit dir (dx,dy in /1024 fixed point),
// drawn `thick` parallel pixels wide (perpendicular to the ray).
static int wx_sgn(int v) { return v > 512 ? 1 : (v < -512 ? -1 : 0); }
static void wx_ray(int cx, int cy, int dx, int dy, int ri, int ro, int thick, uint32_t c) {
    int ax = cx + dx * ri / 1024, ay = cy + dy * ri / 1024;
    int bx = cx + dx * ro / 1024, by = cy + dy * ro / 1024;
    int ox = wx_sgn(-dy), oy = wx_sgn(dx);          // perpendicular step (1px)
    for (int t = 0; t < thick; t++)
        wdg_line(ax + ox * t, ay + oy * t, bx + ox * t, by + oy * t, c);
}

// A teardrop-shaped raindrop: round bulb at the bottom, tapered point on top.
static void wx_drop(int cx, int cy, int s, uint32_t c) {
    int r = (3 * s) / 2; if (r < 1) r = 1;
    draw_circle_filled(cx, cy, r, c);                 // round bottom
    for (int i = 0; i < 3 * s; i++) {                 // pointed top
        int w = 3 * s - i; if (w < 1) w = 1;
        draw_fill_rect(cx - w / 2, cy - r - i, w, 1, c);
    }
}
// #uiscale: warrow() (the crypto/stocks change-% arrow, 9x5) is left at its
// native pixel size deliberately, like the tray glyphs in tray_glyphs.h - a
// fixed hand-built shape with no `s`/scale parameter of its own. Its POSITION
// is computed from already-scaled coordinates at every call site, so it stays
// correctly PLACED at any UI scale; only its own tiny shape does not grow.
// One thick segment of a lightning bolt.
static void wx_bolt_seg(int x0, int y0, int x1, int y1, int s, uint32_t c) {
    for (int t = 0; t < s; t++) wdg_line(x0 + t, y0, x1 + t, y1, c);
}
// A small jagged Z lightning bolt with its top at (bx,by).
static void wx_bolt(int bx, int by, int s, uint32_t c) {
    wx_bolt_seg(bx + s, by,         bx - s, by + 2*s, s, c);
    wx_bolt_seg(bx - s, by + 2*s,   bx + s, by + 2*s, s, c);
    wx_bolt_seg(bx + s, by + 2*s,   bx - s, by + 5*s, s, c);
}

static void wx_icon(int x, int y, int idx, int s) {
    // One consistent light grey for every cloud. Draw the whole icon opaque so
    // overlapping cloud circles don't double-blend (which produced patchy greys
    // when the widget honors global window transparency).
    int saved_blend = g_draw_blend; g_draw_blend = 255;
    uint32_t sun = 0x00FFD040, cl = 0x00CAD2DE, rn = 0x0064B0FF, sw = 0x00FFFFFF, bo = 0x00FFE000;
    int cx = x + 12*s, cy = y + 10*s;
    if (idx == 0) {                                   // clear / sun
        // Core disc + 8 evenly-spaced thin rays at a uniform gap from the disc.
        int cr = 5 * s;                               // core radius
        int ri = cr + 2 * s;                          // ray start (even gap all round)
        int ro = ri + 4 * s;                          // ray end
        static const int rd[8][2] = {
            {0,-1024}, {724,-724}, {1024,0}, {724,724},
            {0,1024}, {-724,724}, {-1024,0}, {-724,-724}
        };
        draw_circle_filled(cx, cy, cr, sun);
        for (int k = 0; k < 8; k++)
            wx_ray(cx, cy, rd[k][0], rd[k][1], ri, ro, s, sun);
    } else if (idx == 1) { draw_circle_filled(x + 8*s, y + 6*s, 5*s, sun); wx_cloud(x, y + 3*s, cl, s); }
    else if (idx == 3)   {                            // fog: light grey cloud + fog lines
        wx_cloud(x, y, cl, s);
        for (int i = 0; i < 3; i++) draw_fill_rect(x + 6*s, y + (21 + i*2)*s, 17*s, s, 0x00A8B0BC);
    }
    else if (idx == 4)   {                            // rain: teardrops only, no cloud
        wx_drop(x + 8*s,  y + 8*s,  s, rn);
        wx_drop(x + 16*s, y + 6*s,  s, rn);
        wx_drop(x + 22*s, y + 10*s, s, rn);
        wx_drop(x + 11*s, y + 18*s, s, rn);
        wx_drop(x + 19*s, y + 18*s, s, rn);
    }
    else if (idx == 5)   {                            // snow: cloud + 3 staggered rows of dots
        wx_cloud(x, y, cl, s);
        for (int r = 0; r < 3; r++)
            for (int i = 0; i < 4; i++)
                draw_circle_filled(x + (7 + i*5 + (r & 1) * 2) * s, y + (20 + r*3) * s, s, sw);
    }
    else if (idx == 6)   {                            // thunder: cloud + 3 bolts (big centre)
        wx_cloud(x, y, cl, s);
        wx_bolt(x + 7*s,  y + 19*s, s, bo);           // left (small)
        wx_bolt(cx,       y + 16*s, s + 1, bo);       // centre: larger, from cloud base
        wx_bolt(x + 17*s, y + 19*s, s, bo);           // right (small)
    }
    else                 { wx_cloud(x, y, cl, s); }   // cloudy (2) / default
    g_draw_blend = saved_blend;                        // restore caller's blend
}
// Currency symbol: dollar-family -> "$", otherwise "" (currency shown in title).
static const char *cursym(const char *cur) {
    static const char *d[] = { "USD","AUD","CAD","NZD","SGD","HKD","MXN","BRL" };
    for (int i = 0; i < 8; i++) { int j = 0; while (d[i][j] && cur[j] && d[i][j] == cur[j]) j++; if (!d[i][j] && !cur[j]) return "$"; }
    return "";
}

// Card chrome shared by every weather state, so the box, the border, the
// accent bar and the title cannot drift between them.
static void wx_chrome(int x, int y, int w, int h, const char *title) {
    draw_rounded_rect(x, y, w, h, ui_px(8), CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    draw_rounded_rect(x, y, ui_px(4), h, ui_px(2), 0x0066C0FF);
    draw_text(x + ui_px(12), y + ui_px(8), title, readable_accent(0x0066C0FF, CLR_MENU_BG));
}

static int draw_weather_card(int x, int y, int w) {
    // (#236) ONE height, from the verbosity SETTING, in every data state:
    // loaded, partial, failed, still loading, never configured, offline. See
    // the card-footprint block comment above weather_card_h(). Deriving a
    // height from the payload here is what this whole change exists to stop -
    // do not reintroduce a local `h =` that reads nf.
    int h = weather_card_h();
    int verbose = g_weather_verbose;   // local widget setting, not cached data
    // #159 sect 6a: offline gate FIRST, before wsplit(s_weather,...) reads the
    // cached buffer at all - g_weather_verbose is a Settings toggle, not part
    // of the network cache, so it is safe to read for sizing the fallback card.
    if (!widget_net_online()) {
        wx_chrome(x, y, w, h, "WEATHER");
        widget_draw_offline(x, w, y + ui_px(30), y + h - ui_px(8));
        return h;
    }
    char f[9][48]; int nf = wsplit(s_weather, '|', f, 9, 48);
    if (nf < 3) {
        // Online, but nothing usable cached: the netinfo service has not
        // written /WEATHER.TXT yet (first boot / still loading), the fetch
        // failed, or the payload was rejected by netinfo_payload_ok(). Same
        // footprint, an explicit empty state inside it - the old code drew a
        // bare "..." in a card it had already shrunk to 64px, which is both
        // the layout bug and an unhelpful thing to look at.
        wx_chrome(x, y, w, h, "WEATHER");
        widget_draw_nodata(x, w, y + ui_px(30), y + h - ui_px(8),
                           "No weather data yet", "No weather", "data yet");
        return h;
    }
    int icon = f[2][0] ? (f[2][0] - '0') : 2;
    // #uiscale: isc was a bare 2 (icon draw-scale, unrelated to sc/sheep-style
    // percent multipliers) - wx_icon()'s whole geometry is parametrized by
    // this one value, so scaling it here scales the icon with no other change.
    int isc = ui_px(2);                                         // 2x-size icon
    // Drop the icon down so it lines up with the Min/Max row (verbose) instead
    // of colliding with the location text on the top line. Keyed off the
    // verbosity SETTING now, not off nf: the card is 118px tall whenever
    // verbose is on, so the icon belongs at the Min/Max row either way.
    int icy = verbose ? y + ui_px(44) : y + ui_px(24);
    wx_chrome(x, y, w, h, f[0]);                                // title = location
    wx_icon(x + w - 24*isc - ui_px(12), icy, icon, isc);
    char l[64]; int li = 0;
    sb(l, &li, sizeof(l), f[1]); sb(l, &li, sizeof(l), "  ");
    sb(l, &li, sizeof(l), f[3]); sb(l, &li, sizeof(l), "\xB0" "C");    // condition + now temp
    draw_text(x + ui_px(12), y + ui_px(30), l, CLR_MENU_TEXT);
    if (verbose && nf >= 9) {
        li = 0; sb(l, &li, sizeof(l), "Min "); sb(l, &li, sizeof(l), f[4]); sb(l, &li, sizeof(l), "\xB0" "C   Max ");
        sb(l, &li, sizeof(l), f[5]); sb(l, &li, sizeof(l), "\xB0" "C");
        draw_text(x + ui_px(12), y + ui_px(52), l, readable_ink_dim(CLR_MENU_BG));
        li = 0; sb(l, &li, sizeof(l), "Rain "); sb(l, &li, sizeof(l), f[7]); sb(l, &li, sizeof(l), "%  ");
        sb(l, &li, sizeof(l), f[8]); sb(l, &li, sizeof(l), "mm");
        draw_text(x + ui_px(12), y + ui_px(72), l, readable_ink_dim(CLR_MENU_BG));
        li = 0; sb(l, &li, sizeof(l), "Humidity "); sb(l, &li, sizeof(l), f[6]); sb(l, &li, sizeof(l), "%");
        draw_text(x + ui_px(12), y + ui_px(92), l, readable_ink_dim(CLR_MENU_BG));
    } else if (verbose) {
        // Verbose card, but the payload is short of the 9 fields the detail
        // rows need (a truncated or older /WEATHER.TXT). The card keeps its
        // 118px footprint; say why the detail is missing rather than leaving
        // 60px of unexplained space.
        draw_text(x + ui_px(12), y + ui_px(52), "Forecast detail unavailable", readable_ink_dim(CLR_MENU_BG));
    }
    return h;
}

// Shared renderer for crypto/stocks (per-item line: CODE  $price  ^chg%).
static int draw_quote_card(int x, int y, int w, const char *raw, unsigned int accent,
                           const char *title_base, const char *cur, int verbose,
                           int rows_cfg) {
    // (#236) `rows_cfg` is how many tickers the USER configured (s_quote_rows,
    // read from /CRYPTOID.TXT / /STOCKID.TXT). The row count used to come from
    // the fetched payload - `rows = nq > 0 ? nq : 1` - so the card was 54px
    // tall until data arrived and then jumped to 30+n*18+6, the same
    // data-dependent footprint the Weather card had.
    int rows = rows_cfg; if (rows < 1) rows = 1; if (rows > QUOTE_ROWS_MAX) rows = QUOTE_ROWS_MAX;
    int h = quote_card_h(rows);
    // #159 sect 6a: offline gate FIRST, before wsplit(raw,...) reads the
    // cached s_crypto/s_stocks buffer at all. title_base ("CRYPTO"/"STOCKS")
    // and cur ("USD"/NULL) are literal call-site constants from
    // netinfo_render(), not cache reads, so they are safe to use; the
    // currency-code suffix normally appended from f[0] IS cache-derived and
    // is therefore dropped here rather than shown stale.
    if (!widget_net_online()) {
        draw_rounded_rect(x, y, w, h, ui_px(8), CLR_MENU_BG);
        draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
        draw_rounded_rect(x, y, ui_px(4), h, ui_px(2), accent);
        draw_text(x + ui_px(12), y + ui_px(8), title_base, readable_accent(accent, CLR_MENU_BG));
        widget_draw_offline(x, w, y + ui_px(30), y + h - ui_px(6));
        return h;
    }
    char f[12][48]; int nf = wsplit(raw, '|', f, 12, 48);
    int first = cur ? 1 : 0;                 // crypto: f[0] is the currency
    int nq = nf - first; if (nq < 0) nq = 0;
    if (nq > rows) nq = rows;                // never paint past the reserved box
    draw_rounded_rect(x, y, w, h, ui_px(8), CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    draw_rounded_rect(x, y, ui_px(4), h, ui_px(2), accent);
    char title[40]; int ti = 0; sb(title, &ti, sizeof(title), title_base);
    if (cur && f[0][0]) { sb(title, &ti, sizeof(title), "  "); sb(title, &ti, sizeof(title), f[0]); }
    draw_text(x + ui_px(12), y + ui_px(8), title, readable_accent(accent, CLR_MENU_BG));
    if (nq == 0) {
        widget_draw_nodata(x, w, y + ui_px(30), y + h - ui_px(6), "No data yet", "No data", "yet");
        return h;
    }
    const char *sym = cur ? cursym(f[0]) : "$";       // crypto: symbol from the file's currency
    for (int i = nq; i < rows; i++)                   // configured but not (yet) returned
        draw_text(x + ui_px(12), y + ui_px(30) + i * ui_px(18), "-", readable_ink_dim(CLR_MENU_BG));
    for (int i = 0; i < nq; i++) {
        char q[3][48]; int qn = wsplit(f[first + i], ',', q, 3, 48);   // code, price, chg
        int yy = y + ui_px(30) + i * ui_px(18);
        // #303: only render a row that parsed into the expected code,price[,chg]
        // shape with a clean (space-free, short) ticker code; otherwise show a
        // placeholder rather than echoing whatever bytes ended up in the buffer.
        if (qn < 2 || !netinfo_payload_ok(q[0])) {
            draw_text(x + ui_px(12), yy, "unavailable", readable_ink_dim(CLR_MENU_BG));
            continue;
        }
        draw_text(x + ui_px(12), yy, q[0], CLR_MENU_TEXT);            // code
        char pr[40]; int pi = 0; sb(pr, &pi, sizeof(pr), sym); sb(pr, &pi, sizeof(pr), q[1]);
        draw_text(x + ui_px(64), yy, pr, CLR_MENU_TEXT);              // $price
        if (verbose && q[2][0]) {                             // change arrow + %
            int up = (q[2][0] != '-');
            uint32_t col = up ? 0x0050E070 : 0x00FF6060;
            warrow(x + w - ui_px(70), yy + ui_px(4), up, col);
            char cg[24]; int ci = 0;
            sb(cg, &ci, sizeof(cg), (q[2][0] == '-') ? q[2] + 1 : q[2]); sb(cg, &ci, sizeof(cg), "%");
            draw_text(x + w - ui_px(60), yy, cg, col);
        }
    }
    return h;
}

// Test-only: draw a labeled gallery of all weather icons (gated by /WXTEST.TXT)
// so the icon set can be eyeballed without waiting on real conditions.
static int g_wxtest = -1;
// #uiscale: debug-only gallery (gated by /WXTEST.TXT, never shown on a
// shipping desktop) - converted anyway for the same "every literal is its
// own ui_px() call" consistency as everything else in this file, including
// the icon draw-scale (was a bare 2, see draw_weather_card()'s isc).
static void draw_icon_gallery(void) {
    static const char *names[7] = { "Clear", "Partly Cloudy", "Cloudy", "Fog", "Rain", "Snow", "Thunder" };
    int x = ui_px(60), y = ui_px(80), w = ui_px(210), rowh = ui_px(56);
    int h = 7 * rowh + ui_px(36);
    draw_rounded_rect(x, y, w, h, ui_px(8), CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    draw_text(x + ui_px(12), y + ui_px(8), "Weather Icons (test)", 0x0066C0FF);
    for (int i = 0; i < 7; i++) {
        int ry = y + ui_px(30) + i * rowh;
        wx_icon(x + ui_px(18), ry, i, ui_px(2));
        char lbl[24]; int li = 0;
        lbl[li++] = '0' + i; lbl[li++] = ':'; lbl[li++] = ' ';
        for (int j = 0; names[i][j] && li < 22; j++) lbl[li++] = names[i][j];
        lbl[li] = 0;
        draw_text(x + ui_px(78), ry + ui_px(16), lbl, CLR_MENU_TEXT);
    }
}

static void netinfo_render(void) {
    // Draw-only (idle per-rect) mode never advances the refresh tick; the idle
    // path refreshes + damages the cards once via netinfo_collect_damage().
    if (!g_widgets_draw_only && (s_netinfo_tick++ % 150 == 0)) netinfo_refresh();
    int w = CARD_W;
    // (#199) Default first-render position for weather/crypto/stocks now
    // comes from widgets_layout_rail_defaults() (called once per frame from
    // widgets_render(), before this function), which puts them in the same
    // right-hand rail as every other data widget. This used to compute its
    // OWN independent default here - a centered top row - which is exactly
    // one of the three uncoordinated placement groups the owner's "all over
    // the place" report was describing; do not reintroduce a second default
    // here.
    for (int i = 0; i < 3; i++) {
        if (!*s_card_vis[i]) continue;
        int hx = *s_card_x[i], hy = *s_card_y[i];
        // (#236) No write-back of a "measured" height any more: card_h(c) is
        // the one source of truth and every caller reads it directly.
        if (i == 0)      draw_weather_card(hx, hy, w);
        else if (i == 1) draw_quote_card(hx, hy, w, s_crypto, 0x00FFC850, "CRYPTO", "USD", g_crypto_verbose, s_quote_rows[1]);
        else             draw_quote_card(hx, hy, w, s_stocks, 0x0066FF99, "STOCKS", 0, g_stocks_verbose, s_quote_rows[2]);
    }
}

// ===========================================================================
// #81-83: per-widget Settings dialog (configure weather location / symbols).
// Editable text field (keyboard input) that saves to a small config file the
// netinfo service reads. Opened from the widget right-click menu -> Settings.
// ===========================================================================
static int  g_wsettings = -1;          // widget id being configured, -1 = closed
static char g_wset_buf[2][64];         // field 0 + (crypto) field 1
static int  g_wset_len[2] = { 0, 0 };
static int  g_wset_focus = 0;
#define WSET_W 400
#define WXCUR_PATH "/CRYPTOCUR.TXT"

// #419b: draggable settings modal. The modal opens centred; g_wset_off_x/y is a
// live offset from that centre applied by every geom(). Dragging the 26px title
// bar moves it (same title-bar-drag pattern as the desktop widgets/windows).
// g_wset_drag: 0 = none, 1 = moving the modal, 2 = dragging the HA list scrollbar.
static int  g_wset_off_x = 0, g_wset_off_y = 0;
static int  g_wset_drag = 0, g_wset_grab_dx = 0, g_wset_grab_dy = 0;
// Keep the title bar reachable: clamp so a margin of the modal + its full 26px
// header stay on-screen no matter how far the user drags.
static void modal_clamp(int *x, int *y, int w, int h) {
    int margin = 72;
    // (#745) Relative to the work area: under a top-panel dock style a floor of
    // 0 put this modal's own 26px header under the panel, which is the same
    // unreachable-header fault as the app-window one.
    int wax, way, waw, wah;
    taskbar_work_area(&wax, &way, &waw, &wah);
    if (*x > wax + waw - margin)  *x = wax + waw - margin;
    if (*x < wax + margin - w)    *x = wax + margin - w;
    if (*y < way)                 *y = way;
    if (*y > way + wah - 30)      *y = way + wah - 30;
    (void)h;
}

static int  wset_nfields(int id) { return id == 3 ? 2 : 1; }       // crypto has currency too
static int *wset_verbose_ptr(int id) { return s_card_verbose[id - 2]; }   // 2/3/4 -> 0/1/2
static int  wset_h(void)         { return (wset_nfields(g_wsettings) == 2 ? 214 : 156) + 34; }
static void wset_geom(int *x, int *y) {
    *x = (g_fb_width - WSET_W) / 2 + g_wset_off_x;
    *y = (g_fb_height - wset_h()) / 2 + g_wset_off_y;
    modal_clamp(x, y, WSET_W, wset_h());
}
// Verbose checkbox box, just above the buttons.
static void wset_chk_box(int *cx, int *cy) {
    int x, y; wset_geom(&x, &y);
    *cx = x + 14; *cy = y + wset_h() - 36 - 30;
}

static const char *wcfg_path0(int id)  { return id==2 ? "/WXLOC.TXT" : id==3 ? "/CRYPTOID.TXT" : "/STOCKID.TXT"; }
static const char *wcfg_def0(int id)   { return id==2 ? "London" : id==3 ? "BTC,ETH" : "AAPL,MSFT"; }
static const char *wcfg_title(int id)  { return id==2 ? "Weather Settings" : id==3 ? "Crypto Settings" : "Stock Settings"; }
static const char *wcfg_lbl0(int id)   { return id==2 ? "Location" : id==3 ? "Coins (short codes)" : "Tickers"; }
// (#236) How many rows Crypto/Stocks reserve. Deliberately defined HERE, next
// to wcfg_path0()/wcfg_def0(), so the count can never be read from a
// different file (or a different default) than the Settings dialog writes.
static int quote_rows_of(const char *csv) {
    int n = 0, tok = 0;
    for (int i = 0; csv[i]; i++) {
        if (csv[i] == ',') { if (tok) { n++; tok = 0; } }
        else if (csv[i] != ' ') tok = 1;
    }
    if (tok) n++;
    if (n < 1) n = 1;
    if (n > QUOTE_ROWS_MAX) n = QUOTE_ROWS_MAX;
    return n;
}
static void quote_rows_one(int id, int slot) {
    char b[128]; b[0] = '\0';
    int fd = sys_open(wcfg_path0(id), 0);
    if (fd >= 0) {
        char raw[128];
        long n = sys_read(fd, raw, sizeof(raw) - 1);
        sys_close(fd);
        if (n > 0) {
            int i = 0;
            for (; i < (int)sizeof(b) - 1 && i < n && raw[i] && raw[i] != '\n' && raw[i] != '\r'; i++) b[i] = raw[i];
            while (i > 0 && b[i-1] == ' ') i--;
            b[i] = '\0';
        }
    }
    s_quote_rows[slot] = quote_rows_of(b[0] ? b : wcfg_def0(id));
}
static void quote_rows_refresh(void) { quote_rows_one(3, 1); quote_rows_one(4, 2); }

static const char *wcfg_hint0(int id)  { return id==2 ? "City,Country  e.g. Perth,AU  or  London"
                                              : id==3 ? "e.g. BTC,ETH,USDT,SOL,XRP"
                                                      : "e.g. AAPL,MSFT,GOOG"; }

static void wset_load(int f, const char *path, const char *def) {
    g_wset_buf[f][0] = '\0'; g_wset_len[f] = 0;
    int fd = sys_open(path, 0);
    if (fd >= 0) {
        char b[128]; long n = sys_read(fd, b, sizeof(b) - 1); sys_close(fd);
        if (n > 0) {
            int i = 0;
            for (; i < (int)sizeof(g_wset_buf[f]) - 1 && i < n && b[i] && b[i] != '\n' && b[i] != '\r'; i++)
                g_wset_buf[f][i] = b[i];
            while (i > 0 && g_wset_buf[f][i-1] == ' ') i--;
            g_wset_buf[f][i] = '\0'; g_wset_len[f] = i;
        }
    }
    if (g_wset_len[f] == 0) {
        int i = 0; for (; def[i] && i < (int)sizeof(g_wset_buf[f]) - 1; i++) g_wset_buf[f][i] = def[i];
        g_wset_buf[f][i] = '\0'; g_wset_len[f] = i;
    }
}

// ---- #414/#419 HA entity picker + rename + display mode (widget_settings id 10)
#define HAP_W 480
#define HAP_H 468
#define HAP_SB_W 12                 // #419b entity-list scrollbar width
static int ha_pick_scroll = 0;
static const char *HA_MODE_LBL[4] = { "Value", "Big", "Badge", "Gauge" };
static void hap_geom(int *x,int *y){
    *x=(g_fb_width-HAP_W)/2 + g_wset_off_x;
    *y=(g_fb_height-HAP_H)/2 + g_wset_off_y;
    modal_clamp(x,y,HAP_W,HAP_H);
}
// #419b count catalog lines matching the current filter (for scroll bounds).
static int ha_cat_count(const char *flt){
    int count=0,ls=0;
    for(int i=0;;i++){
        char c=s_ha_cat[i];
        if(c=='\n'||c==0){
            int len=i-ls;
            if(len>0){
                char line[256]; int k=0; for(int j=ls;j<i&&k<255;j++) line[k++]=s_ha_cat[j]; line[k]=0;
                if(ha_ci_has(line,flt)) count++;
            }
            ls=i+1; if(c==0) break;
        }
    }
    return count;
}
// shared layout offsets (render + click must agree)
#define HAP_RENAME_DY 48
#define HAP_MODE_DY   100
#define HAP_SEARCH_DY 152
#define HAP_LIST_DY   184
static void ha_chip_box(int x,int y,int i,int *cx,int *cy,int *cw,int *ch){
    int fw=HAP_W-28, gap=6, w=(fw-3*gap)/4;
    *cx=x+14+i*(w+gap); *cy=y+HAP_MODE_DY; *cw=w; *ch=26;
}
static int hap_rows(void){ int rowh=20; return (HAP_H-HAP_LIST_DY-46)/rowh; }
// #414: true while haservice is actively fetching the catalog. The service drops
// /HALIST.PRG during a fetch and removes it once /HALIST.TXT has been written.
static int ha_prg_active(void){ int fd=sys_open("/HALIST.PRG",0); if(fd<0) return 0;
    char c; long n=sys_read(fd,&c,1); sys_close(fd); return n>0; }  // 0-byte (truncate-delete) = not fetching
static void ha_picker_render(void){
    int x,y; hap_geom(&x,&y);
    // Self-refresh while the catalog is still empty: re-read /HALIST.TXT (the
    // haservice writes it when the fetch finishes) and periodically re-issue the
    // request, so the panel populates itself when data arrives - no "reopen" needed.
    if(s_ha_cat_len==0){
        static unsigned long s_ha_rl=0, s_ha_rq=0; unsigned long now=uptime_ms();
        if(now-s_ha_rl>700){ s_ha_rl=now; ha_load_catalog(); }
        if(now-s_ha_rq>6000){ s_ha_rq=now; ha_request_catalog(); }
    }
    draw_fill_rect(x,y,HAP_W,HAP_H,CLR_MENU_BG);
    draw_rect_outline(x,y,HAP_W,HAP_H,CLR_MENU_BORDER);
    draw_fill_rect(x,y,HAP_W,26,CLR_MENU_ITEM_HOVER);
    draw_text(x+12,y+7,"Home Assistant widget - settings",CLR_MENU_TEXT);
    // #723 The bound entity_id is deliberately NOT shown on the desktop card
    // (that was the raw-id noise the redesign removed) - this settings header
    // is the "properties view" it moved to instead: the one place a user who
    // wants the technical identifier (to verify the right entity is bound, or
    // to cross-reference it in Home Assistant itself) can find it, without it
    // cluttering the always-visible card.
    if (s_ha_entity[0]) {
        char eid[40]; ha_fit_text(s_ha_entity, 180, 1, eid, sizeof(eid));
        int ew = text_width(eid);
        draw_text(x+HAP_W-24-8-ew, y+7, eid, readable_ink_dim(CLR_MENU_ITEM_HOVER));
    }
    // Close (X) button, top-right of the title bar (excluded from the header drag).
    { int cxx=x+HAP_W-24; draw_fill_rect(cxx,y+5,18,16,0x00A83232);
      draw_rect_outline(cxx,y+5,18,16,0x00D06060); draw_text_centered(cxx+9,y+6,"X",0x00FFFFFF); }
    int fw=HAP_W-28;
    // --- rename field (#419), focus 1 ---
    // (#745) the 0x0066B3FF focus rings below are HARDCODED and theme-blind.
    // They were checked and NOT changed: they measure 6.20:1 on the default
    // CLR_MENU_BG and 7.34:1 on the #202020 field fill, so they are not a
    // contrast defect and there is nothing here for the 3:1 floor to fix. They
    // are still debt, because a theme cannot influence them; the compositor
    // does not link the libc style engine's palette, so moving them wants a
    // theme_color(THEME_COLOR_FOCUS_RING) read rather than gui_pal().
    int rx=x+14, ry=y+HAP_RENAME_DY;
    draw_text(rx,ry-14,"Display name (blank = use HA name)",readable_ink_dim(CLR_MENU_BG));
    draw_fill_rect(rx,ry,fw,24,0x00202020);
    draw_rect_outline(rx,ry,fw,24,(g_wset_focus==1)?0x0066B3FF:CLR_MENU_BORDER);
    draw_text(rx+6,ry+5,g_wset_buf[1],0x00FFFFFF);
    if(g_wset_focus==1){ int cw=text_width(g_wset_buf[1]); draw_fill_rect(rx+6+cw,ry+4,2,16,0x00FFFFFF); }
    // --- display mode chips (#419) ---
    draw_text(x+14,y+HAP_MODE_DY-14,"Display mode",readable_ink_dim(CLR_MENU_BG));
    for(int i=0;i<4;i++){
        int cx,cy,cw,ch; ha_chip_box(x,y,i,&cx,&cy,&cw,&ch);
        int sel=(g_ha_mode==i);
        draw_fill_rect(cx,cy,cw,ch,sel?0x00005FB8:0x00303840);
        draw_rect_outline(cx,cy,cw,ch,sel?0x0066B3FF:CLR_MENU_BORDER);
        draw_text_centered(cx+cw/2,cy+5,HA_MODE_LBL[i],0x00FFFFFF);
    }
    // --- search field (#419 task 2), focus 0 ---
    int sx=x+14, sy=y+HAP_SEARCH_DY;
    draw_text(sx,sy-14,"Search entity id / name / domain",readable_ink_dim(CLR_MENU_BG));
    draw_fill_rect(sx,sy,fw,24,0x00202020);
    draw_rect_outline(sx,sy,fw,24,(g_wset_focus==0)?0x0066B3FF:CLR_MENU_BORDER);
    draw_text(sx+6,sy+5,g_wset_buf[0],0x00FFFFFF);
    if(g_wset_focus==0){ int cw=text_width(g_wset_buf[0]); draw_fill_rect(sx+6+cw,sy+4,2,16,0x00FFFFFF); }
    // --- filtered entity list (#419b scrollable: wheel + draggable thumb) ---
    int ly=y+HAP_LIST_DY, rowh=20, rows=hap_rows();
    int total=ha_cat_count(g_wset_buf[0]);
    int maxsc=(total>rows)?(total-rows):0;
    if(ha_pick_scroll>maxsc) ha_pick_scroll=maxsc;   // keep scroll in range (also after filtering)
    if(ha_pick_scroll<0) ha_pick_scroll=0;
    char row[256];
    int shown=0;
    for(int r=0;r<rows;r++){
        if(!ha_cat_nth(g_wset_buf[0], ha_pick_scroll+r, row, sizeof(row))) break;
        char eid[96],fn[96]; ha_field(row,0,eid,sizeof(eid)); ha_field(row,1,fn,sizeof(fn));
        int ry2=ly+r*rowh;
        if(fn[0]){ char lbl[60]; int k=0; for(;fn[k]&&k<40;k++) lbl[k]=fn[k]; lbl[k]=0; draw_text(sx+6,ry2+3,lbl,0x00E0E0E0); }
        char es[40]; int k=0; for(;eid[k]&&k<28;k++) es[k]=eid[k]; es[k]=0;   // narrowed for the scrollbar gutter
        draw_text(sx+6+232,ry2+3,es,0x008FA9BD);
        shown++;
    }
    if(shown==0){
        if(s_ha_cat_len){
            draw_text(sx+6,ly+3,"No entities match - clear the search.",readable_ink_dim(CLR_MENU_BG));
        } else {
            // Animated spinner + live status while the background fetch runs; the
            // auto-reload above fills the list when /HALIST.TXT arrives.
            static const int SDX[8]={0,7,10,7,0,-7,-10,-7};
            static const int SDY[8]={-10,-7,0,7,10,7,0,-7};
            unsigned long t=uptime_ms(); int head=(int)((t/90)%8);
            int scx=sx+18, scy=ly+22;
            for(int d=0;d<8;d++){ int ph=(head-d+8)%8;
                unsigned c=(ph<4)?(0x00FFFFFFu-(unsigned)ph*0x00303030u):0x00384048u;
                draw_fill_rect(scx+SDX[d]-2,scy+SDY[d]-2,4,4,c); }
            int active=ha_prg_active();
            draw_text(scx+26,scy-6, active? "Fetching entities from Home Assistant..."
                                          : "Contacting Home Assistant...", 0x00E6E6E6);
            draw_text(scx+26,scy+8, "The list updates automatically - no need to reopen.",
                      readable_ink_dim(CLR_MENU_BG));
        }
    }
    // Scrollbar track + thumb on the right of the list (only when it overflows).
    if(maxsc>0){
        int track_h=rows*rowh, sbx=x+HAP_W-14-HAP_SB_W;
        draw_fill_rect(sbx,ly,HAP_SB_W,track_h,0x00202830);
        draw_rect_outline(sbx,ly,HAP_SB_W,track_h,CLR_MENU_BORDER);
        int th=track_h*rows/total; if(th<24)th=24; if(th>track_h)th=track_h;
        int ty=ly+(track_h-th)*ha_pick_scroll/maxsc;
        draw_fill_rect(sbx+1,ty,HAP_SB_W-2,th,0x005A6A7A);
        draw_rect_outline(sbx+1,ty,HAP_SB_W-2,th,0x0066B3FF);
    }
    int by=y+HAP_H-34;
    draw_text(x+14,by+7,"Click an entity to select.",readable_ink_dim(CLR_MENU_BG));
    draw_fill_rect(x+HAP_W-134,by,124,26,0x00005FB8);
    draw_text_centered(x+HAP_W-134+62,by+6,"Save & Close",0x00FFFFFF);
}
static int ha_picker_click(int mx,int my){
    int x,y; hap_geom(&x,&y);
    // Modal: clicks outside the panel do nothing (close only via X / Save & Close / ESC).
    if(mx<x||mx>=x+HAP_W||my<y||my>=y+HAP_H){ return 1; }
    // Close (X) button in the title bar.
    if(mx>=x+HAP_W-24&&mx<x+HAP_W-6&&my>=y+5&&my<y+21){ ha_write_label(g_wset_buf[1]); g_wsettings=-1; return 1; }
    int fw=HAP_W-28;
    // rename field focus
    int rx=x+14, ry=y+HAP_RENAME_DY;
    if(mx>=rx&&mx<rx+fw&&my>=ry&&my<ry+24){ g_wset_focus=1; return 1; }
    // search field focus
    int sy=y+HAP_SEARCH_DY;
    if(mx>=rx&&mx<rx+fw&&my>=sy&&my<sy+24){ g_wset_focus=0; return 1; }
    // mode chips
    for(int i=0;i<4;i++){ int cx,cy,cw,ch; ha_chip_box(x,y,i,&cx,&cy,&cw,&ch);
        if(mx>=cx&&mx<cx+cw&&my>=cy&&my<cy+ch){ g_ha_mode=i; return 1; } }
    // Save & Close
    int by=y+HAP_H-34;
    if(my>=by&&my<by+26&&mx>=x+HAP_W-134&&mx<x+HAP_W-10){ ha_write_label(g_wset_buf[1]); g_wsettings=-1; return 1; }
    // entity rows
    int ly=y+HAP_LIST_DY, rowh=20, rows=hap_rows();
    if(my>=ly&&my<ly+rows*rowh){
        int r=(my-ly)/rowh; char row[256];
        if(ha_cat_nth(g_wset_buf[0], ha_pick_scroll+r, row, sizeof(row))){
            char eid[96]; ha_field(row,0,eid,sizeof(eid));
            ha_write_entity(eid); g_show_ha=1; ha_write_label(g_wset_buf[1]); g_wsettings=-1;
        }
    }
    return 1;
}

// ---- #419b modal drag (title bar) + HA entity-list scroll -----------------
int widget_settings_handle_mouse(int x, int y, int click);   // fwd (defined below)
// Active modal rect: HA picker (id 10) or the generic weather/crypto/stock modal.
static void modal_rect(int *x,int *y,int *w,int *h){
    if(g_wsettings==10){ hap_geom(x,y); *w=HAP_W; *h=HAP_H; }
    else { wset_geom(x,y); *w=WSET_W; *h=wset_h(); }
}
int widget_settings_header_hit(int mx,int my){
    if(g_wsettings<0) return 0;
    int x,y,w,h; modal_rect(&x,&y,&w,&h);
    if(!(mx>=x && mx<x+w && my>=y && my<y+26)) return 0;   // the 26px title bar only
    // Exclude the HA picker's top-right close (X) button so a click there closes
    // the dialog instead of starting a drag.
    if(g_wsettings==10 && mx>=x+w-24 && mx<x+w-6 && my>=y+5 && my<y+21) return 0;
    return 1;
}
// HA list scrollbar track geometry; returns 0 when the list does not overflow.
static int ha_scroll_track(int *tx,int *ty,int *tw,int *th,int *maxsc,int *total,int *rows){
    if(g_wsettings!=10) return 0;
    int x,y; hap_geom(&x,&y);
    *rows=hap_rows(); *total=ha_cat_count(g_wset_buf[0]);
    *maxsc=(*total>*rows)?(*total-*rows):0;
    if(*maxsc<=0) return 0;
    *tw=HAP_SB_W; *tx=x+HAP_W-14-HAP_SB_W; *ty=y+HAP_LIST_DY; *th=(*rows)*20;
    return 1;
}
static void ha_scroll_to_y(int my){
    int tx,ty,tw,th,maxsc,total,rows;
    if(!ha_scroll_track(&tx,&ty,&tw,&th,&maxsc,&total,&rows)) return;
    int thmb=th*rows/total; if(thmb<24)thmb=24; if(thmb>th)thmb=th;
    int span=th-thmb; if(span<1) span=1;
    int rel=my-ty-thmb/2; if(rel<0)rel=0; if(rel>span)rel=span;   // centre thumb on cursor
    ha_pick_scroll=rel*maxsc/span;
    if(ha_pick_scroll>maxsc)ha_pick_scroll=maxsc; if(ha_pick_scroll<0)ha_pick_scroll=0;
    (void)tw;
}
static int ha_scrollbar_press(int mx,int my){
    int tx,ty,tw,th,maxsc,total,rows;
    if(!ha_scroll_track(&tx,&ty,&tw,&th,&maxsc,&total,&rows)) return 0;
    if(mx<tx||mx>=tx+tw||my<ty||my>=ty+th) return 0;
    g_wset_drag=2; ha_scroll_to_y(my); return 1;    // begin thumb drag + jump
}
// A fresh left-press on the open modal. Returns 1 if it began an ongoing drag
// (header move or scrollbar), else 0 (handled as a normal click).
int widget_settings_press(int mx,int my){
    if(g_wsettings<0) return 0;
    if(widget_settings_header_hit(mx,my)){                       // drag the modal
        int x,y,w,h; modal_rect(&x,&y,&w,&h);
        g_wset_drag=1; g_wset_grab_dx=mx-x; g_wset_grab_dy=my-y; return 1;
    }
    if(ha_scrollbar_press(mx,my)) return 1;                      // drag the list scrollbar
    widget_settings_handle_mouse(mx,my,1);                       // normal control click
    return 0;
}
void widget_settings_drag_to(int mx,int my){
    if(g_wset_drag==2){ ha_scroll_to_y(my); return; }           // scrollbar thumb
    if(g_wset_drag!=1) return;                                  // header move
    int w=(g_wsettings==10)?HAP_W:WSET_W;
    int h=(g_wsettings==10)?HAP_H:wset_h();
    int nx=mx-g_wset_grab_dx, ny=my-g_wset_grab_dy;
    modal_clamp(&nx,&ny,w,h);
    g_wset_off_x=nx-(g_fb_width-w)/2;
    g_wset_off_y=ny-(g_fb_height-h)/2;
}
void widget_settings_drag_end(void){ g_wset_drag=0; }
int  widget_settings_is_dragging(void){ return g_wset_drag!=0; }
// Mouse wheel scrolls the HA entity list whenever its (modal) picker is open.
// Bounds are not gated on the exact cursor position: the picker is modal, so a
// wheel notch anywhere scrolls its list (also robust to cursor-tracking lag).
int widget_settings_handle_scroll(int mx,int my,int delta){
    if(g_wsettings!=10) return 0;
    (void)mx; (void)my;
    int rows=hap_rows(), total=ha_cat_count(g_wset_buf[0]);
    int maxsc=(total>rows)?(total-rows):0;
    ha_pick_scroll-=delta*3;                                    // wheel up = list up
    if(ha_pick_scroll>maxsc)ha_pick_scroll=maxsc;
    if(ha_pick_scroll<0)ha_pick_scroll=0;
    return 1;
}

void widget_settings_open(int id) {
    g_wsettings = id; g_wset_focus = 0;
    g_wset_off_x = 0; g_wset_off_y = 0; g_wset_drag = 0;    // #419b open centred, no drag
    if (id == 10) {                          // #414/#419 HA settings (picker+rename+mode)
        g_wset_buf[0][0] = '\0'; g_wset_len[0] = 0; ha_pick_scroll = 0;
        ha_load_label();                     // seed rename field with the current label
        int n=0; for(; g_ha_label[n] && n<(int)sizeof(g_wset_buf[1])-1; n++) g_wset_buf[1][n]=g_ha_label[n];
        g_wset_buf[1][n]=0; g_wset_len[1]=n;
        g_wset_focus = 0;                    // start on the search field
        ha_request_catalog();                // ask the service to (re)build /HALIST.TXT
        ha_load_catalog();                   // load whatever exists now
        return;
    }
    wset_load(0, wcfg_path0(id), wcfg_def0(id));
    if (id == 3) wset_load(1, WXCUR_PATH, "USD");
}
int widget_settings_is_open(void) { return g_wsettings >= 0; }

static void wset_write(const char *path, const char *buf, int len) {
    int fd = sys_open(path, 0x0001 | 0x0040);  // O_WRONLY|O_CREAT
    if (fd < 0) return;
    char out[130]; int i = 0;
    for (; i < len && i < 120; i++) out[i] = buf[i];
    out[i++] = '\n';
    while (i < 125) out[i++] = ' ';        // pad so a shorter value leaves no stale tail
    out[i++] = '\n';
    sys_write(fd, out, i); sys_close(fd);
}
static void wset_save(void) {
    if (g_wsettings < 0) return;
    if (g_wsettings == 10) { return; }       // #414 HA saves on entity click
    wset_write(wcfg_path0(g_wsettings), g_wset_buf[0], g_wset_len[0]);
    if (g_wsettings == 3) wset_write(WXCUR_PATH, g_wset_buf[1], g_wset_len[1]);
}

static void wset_field_box(int f, int *fx, int *fy, int *fw, int *fh) {
    int x, y; wset_geom(&x, &y);
    *fx = x + 14; *fw = WSET_W - 28; *fh = 26; *fy = y + 56 + f * 54;
}

void widget_settings_render(void) {
    if (g_wsettings < 0) return;
    if (g_wsettings == 10) { ha_picker_render(); return; }
    int id = g_wsettings, nf = wset_nfields(id);
    int x, y; wset_geom(&x, &y); int H = wset_h();
    draw_fill_rect(x, y, WSET_W, H, CLR_MENU_BG);
    draw_rect_outline(x, y, WSET_W, H, CLR_MENU_BORDER);
    draw_fill_rect(x, y, WSET_W, 26, CLR_MENU_ITEM_HOVER);
    draw_text(x + 12, y + 7, wcfg_title(id), CLR_MENU_TEXT);
    for (int f = 0; f < nf; f++) {
        int fx, fy, fw, fh; wset_field_box(f, &fx, &fy, &fw, &fh);
        const char *lbl  = (f == 0) ? wcfg_lbl0(id) : "Currency";
        const char *hint = (f == 0) ? wcfg_hint0(id) : "e.g. USD, EUR, GBP, AUD, JPY";
        draw_text(fx, fy - 14, lbl, readable_ink_dim(CLR_MENU_BG));
        draw_fill_rect(fx, fy, fw, fh, 0x00202020);
        draw_rect_outline(fx, fy, fw, fh, (f == g_wset_focus) ? 0x0066B3FF : CLR_MENU_BORDER);
        draw_text(fx + 6, fy + 6, g_wset_buf[f], 0x00FFFFFF);
        if (f == g_wset_focus) {
            int cw = text_width(g_wset_buf[f]);
            draw_fill_rect(fx + 6 + cw, fy + 5, 2, fh - 10, 0x00FFFFFF);
        }
        draw_text(fx, fy + fh + 2, hint, readable_ink_dim(CLR_MENU_BG));
    }
    int cbx, cby; wset_chk_box(&cbx, &cby);
    int on = *wset_verbose_ptr(id);
    draw_fill_rect(cbx, cby, 18, 18, 0x00202020);
    draw_rect_outline(cbx, cby, 18, 18, CLR_MENU_BORDER);
    if (on) {                                       // checkmark
        draw_fill_rect(cbx + 4, cby + 8, 3, 5, 0x0066B3FF);
        draw_fill_rect(cbx + 6, cby + 10, 8, 3, 0x0066B3FF);
        draw_fill_rect(cbx + 11, cby + 4, 3, 9, 0x0066B3FF);
    }
    draw_text(cbx + 26, cby + 2, "Verbose (show details / per-line)", CLR_MENU_TEXT);
    int by = y + H - 36;
    draw_fill_rect(x + WSET_W - 184, by, 84, 28, 0x00005FB8);
    draw_text_centered(x + WSET_W - 184 + 42, by + 8, "Save", 0x00FFFFFF);
    draw_fill_rect(x + WSET_W - 94, by, 84, 28, 0x00444444);
    draw_text_centered(x + WSET_W - 94 + 42, by + 8, "Cancel", 0x00FFFFFF);
}

int widget_settings_handle_key(int key) {
    if (g_wsettings < 0) return 0;
    if (key == 27) { g_wsettings = -1; return 1; }                       // ESC
    if (g_wsettings == 10) {                          // #414/#419 edit search / rename
        if (key == '\t') { g_wset_focus ^= 1; return 1; }           // Tab: switch fields
        int f = (g_wset_focus == 1) ? 1 : 0;
        if (f == 0) ha_pick_scroll = 0;                             // search edit resets scroll
        if (key == '\b' || key == 8 || key == 127) { if (g_wset_len[f] > 0) g_wset_buf[f][--g_wset_len[f]] = '\0'; return 1; }
        if (key == '\n' || key == '\r') { ha_load_catalog(); return 1; }
        if (key >= 0x20 && key <= 0x7E && g_wset_len[f] < (int)sizeof(g_wset_buf[f]) - 1) {
            g_wset_buf[f][g_wset_len[f]++] = (char)key; g_wset_buf[f][g_wset_len[f]] = '\0';
        }
        return 1;
    }
    if (key == '\n' || key == '\r') {
        if (wset_nfields(g_wsettings) == 2 && g_wset_focus == 0) { g_wset_focus = 1; return 1; }
        wset_save(); g_wsettings = -1; return 1;
    }
    if (key == '\t') { if (wset_nfields(g_wsettings) == 2) g_wset_focus ^= 1; return 1; }
    int f = g_wset_focus;
    if (key == '\b' || key == 8 || key == 127) { if (g_wset_len[f] > 0) g_wset_buf[f][--g_wset_len[f]] = '\0'; return 1; }
    if (key >= 0x20 && key <= 0x7E && g_wset_len[f] < (int)sizeof(g_wset_buf[f]) - 1) {
        g_wset_buf[f][g_wset_len[f]++] = (char)key; g_wset_buf[f][g_wset_len[f]] = '\0';
    }
    return 1;
}

int widget_settings_handle_mouse(int x, int y, int click) {
    if (g_wsettings < 0) return 0;
    if (!click) return 1;
    if (g_wsettings == 10) return ha_picker_click(x, y);
    int dx, dy; wset_geom(&dx, &dy); int H = wset_h();
    for (int f = 0; f < wset_nfields(g_wsettings); f++) {
        int fx, fy, fw, fh; wset_field_box(f, &fx, &fy, &fw, &fh);
        if (x >= fx && x < fx + fw && y >= fy && y < fy + fh) { g_wset_focus = f; return 1; }
    }
    int cbx, cby; wset_chk_box(&cbx, &cby);
    if (x >= cbx && x < cbx + 260 && y >= cby && y < cby + 18) {     // toggle verbose
        int *v = wset_verbose_ptr(g_wsettings); *v = !*v; return 1;
    }
    int by = dy + H - 36;
    if (y >= by && y < by + 28) {
        if (x >= dx + WSET_W - 184 && x < dx + WSET_W - 100) { wset_save(); g_wsettings = -1; return 1; }
        if (x >= dx + WSET_W - 94  && x < dx + WSET_W - 10)  { g_wsettings = -1; return 1; }
    }
    if (x < dx || x >= dx + WSET_W || y < dy || y >= dy + H) g_wsettings = -1;   // click-away cancels
    return 1;
}

// ===========================================================================
// (#199) Right-hand widget rail: ONE default-placement pass.
//
// OWNER REPORT (first boot, Marble dock): "the widgets are all over the
// place. i'd like ... the widgets [to] stack on the right hand side with an
// even gap between them and the far right side of the screen ... starting
// from the top of the screen ... and going downwards."
//
// ROOT CAUSE (measured, not assumed): the scattered look was NOT a stale
// persisted profile and NOT a layout pass that failed to run - there simply
// never was one. Three independent hardcoded default-position groups grew up
// ticket-by-ticket, each unaware of the others:
//   - #79/#78/#129: analog clock + calendar + digital clock, right edge,
//     top of screen (calendar's own default Y was even derived from the
//     analog clock's CURRENT g_clock_cy - which is populated whether or not
//     the clock is shown, since #79 unconditionally computes the clock's
//     default before checking g_show_clock - so calendar silently reserved a
//     phantom clock-sized gap above itself even with the clock hidden).
//   - #274: system monitor / timer / world time, LEFT edge, explicitly
//     commented "so they don't collide with the right-side clock column" -
//     i.e. the left-edge placement was a direct reaction to the right-edge
//     group already being there, not a design.
//   - #81-83: weather / crypto / stocks, centered in their OWN top row,
//     inside netinfo_render(), oblivious to both of the above.
// A 1990s-UNIX desktop with three uncoordinated corners is exactly "all over
// the place". This replaces all three with one pass, reusing
// taskbar_widget_area() (the same dock-style-aware bounds widget_clamp_pos(),
// desktop icon placement and the kernel WM strut already use) so Marble's
// floating overlay dock and a classic fenced bottom bar each get the correct
// start Y and usable height with no dock-style special case written here.
//
// Only a widget whose stored position is still -1 (never dragged, nothing
// persisted in UIPROFIL.YML) gets placed; anything the user has already
// positioned - or that a PRIOR call to this same function already placed
// this session - is left exactly where it is. Hidden widgets never reserve a
// slot, so toggling one on/off does not leave a gap in the stack.
//
// Column packing: a simple top-to-bottom "shelf" pass in a fixed, stable
// order (matching the widget tray menu's own order). When the next widget
// would run past the bottom of the widget area, a new column starts
// immediately to the left of the widest widget placed in the column so far,
// so the rail overflows sideways instead of piling widgets on top of each
// other or running off the bottom edge.
// ===========================================================================
// #uiscale: these three were bare 1x literals independent of the widgets
// they space - at 200% every card in the rail doubled while the gaps
// between them stayed fixed, which is a proportion drift even though it
// never overlaps or clips anything. ui_px() keeps the rail's own spacing in
// the same ratio to the widgets it lays out at any scale.
#define RAIL_RIGHT_INSET ui_px(16)   // gap between the rail and the screen's right edge
#define RAIL_TOP_GAP     ui_px(14)   // gap between the widget area's top and the first widget
#define RAIL_V_GAP       ui_px(12)   // gap between consecutively stacked widgets

// ===========================================================================
// (#213) SELF-HEALING: a stored position is not automatically "placed
// already" just because it fails the -1 sentinel check.
//
// MEASURED root cause (owner report on golden 2009, real iMac14,4): reading
// the shipped out-of-git asset base's own UIPROFIL.YML (the file every
// golden overlays onto, per build/build-golden.sh) showed exactly the four
// defects reported, and NONE of them were unset (-1):
//   upx: 16   upy: 460   (Uptime)          - #274's LEFT-EDGE default, baked
//   hax: 16   hay: 544   (Home Assistant)  - in before #199 ever ran
//   smx/tmx/wtx: 16      (sysmon/timer/worldtime, hidden but same stale left
//                         value - would surface on the left the moment a
//                         user enables one, so it needs the same healing)
//   calx: 1068 caly: 150 (Calendar)  - a RAIL position, but baked at a
//                         session where Clock was also shown above it; with
//                         Clock now hidden, #199's own packing loop (see the
//                         fix below) advanced its counter from RAIL_TOP_GAP
//                         instead of Calendar's real y=150, so Weather (the
//                         one position that WAS correctly reset to -1) got
//                         placed at y=~186, inside Calendar's own 150-290
//                         rect: an overlap, which reads as "no gap between
//                         them".
//   dcx: 1138 dcy: 8     (Digital Clock)  - a pre-#199 TOP-right default,
//                         nowhere near the bottom-right corner #199 asked for
// Every one of these is UNLOCKED (*lk: 0 for all of them in that file), so
// the user never deliberately pinned any of them there - #199's own comment
// ("anything already positioned ... is left exactly where it is") was
// written assuming "already positioned" only ever meant "the user dragged
// it", but a stale bake from an old scheme (or an old resolution, or a
// session where a different widget was visible above it) satisfies the same
// "not -1" test and is indistinguishable from a real placement by that test
// alone.
//
// The fix: an UNLOCKED stored position is trusted only if it still looks
// like a genuine rail slot ON THIS SCREEN, RIGHT NOW - right-aligned to the
// rail's current edge. A locked widget is always trusted (the user pinned
// it via the widget's own Lock menu item; that is an explicit, informed
// decision this pass must never override, however odd the result looks).
// Known limitation: this checks alignment against the PRIMARY column only,
// so a widget legitimately overflowed into a second column on a very small
// screen could be mis-judged "stale" and pulled back into column one on a
// later boot at the same tiny resolution. Not a regression (#199 had no
// second-column persistence story either) and not reachable at any
// resolution this OS currently ships at.
//
// SECOND HEALING NEEDED (found verifying this same fix, real reproduction on
// a 1280x800 Marble/XFCE screen): the first cut of this check trusted any
// by >= ay - TOL, which is not tight enough. widgets_clamp_to_bounds() (#745)
// runs every frame AND once at startup from taskbar_apply_work_area() -
// BEFORE this function ever sees a widget's raw stored value on its very
// first frame. A wildly-stale position (measured: clky baked at 6, i.e.
// clock top = 6 - r = -38, off the top of the screen entirely) gets clamped
// UP to exactly `ay` (the widget area's own top edge) by that startup clamp,
// with NO RAIL_TOP_GAP margin - widget_clamp_pos() only enforces "not above
// the area", it has no concept of the rail's own top gap. By the time this
// function runs, the ORIGINAL -38 is gone; it only ever sees the LAUNDERED
// by == ay (gap = 0), which the old `by >= ay - TOL` test wrongly called
// trusted - reproducing "no gap between taskbar and calendar" (the clock in
// this case) even with this fix applied, because the bad value never made it
// here. A minimum-gap floor closes it: a trusted position must be at or past
// where a FRESH placement would have put it (ay + RAIL_TOP_GAP), not merely
// not-above-the-area. This costs nothing for a real fresh placement (which
// always lands at exactly ay + RAIL_TOP_GAP or later) and nothing for a
// widget stacked further down a column (whose y is always well past this
// floor too) - it only rejects the narrow was-clamped-to-the-bare-top band
// that used to slip through.
// ===========================================================================
#define RAIL_POS_TOL 4   // rounding-only slack; a different COLUMN never
                          // lands within this of the current one

// (#236) THIRD HEALING: a stored position that OVERLAPS the widget above it
// in the same column is not a rail slot either, however well right-aligned it
// is. #213's own block comment describes the fault it could not see - a
// Weather y baked in a session where a different widget was above it lands
// "inside Calendar's own 150-290 rect" - and then fixed only the CAUSE it had
// measured, leaving the SHAPE undetected: the two tests above judge each
// widget in isolation, so any stale y that happens to be right-aligned and
// below the top gap is trusted no matter what it lands on top of. That is
// exactly the owner's "weather widget positioned on top of the calendar
// widget", and it survives the height fix because a position already written
// to UIPROFIL.YML under the old data-dependent sizing does not heal itself.
// `prev_bottom` is the bottom edge of the last widget placed or trusted in
// this column (the column's own top edge for the first one), so this costs
// nothing for a correct stack and rejects only a real collision.
static int rail_pos_trusted(int locked, int bx, int by, int bw, int bh,
                             int col_right, int ay, int area_bottom, int prev_bottom) {
    (void)bh;
    if (locked) return 1;
    if (bx + bw < col_right - RAIL_POS_TOL || bx + bw > col_right + RAIL_POS_TOL) return 0;
    if (by < ay + RAIL_TOP_GAP - RAIL_POS_TOL) return 0;
    if (by > area_bottom) return 0;
    if (by < prev_bottom) return 0;                 // sits on top of its neighbour
    return 1;
}

static void widgets_layout_rail_defaults(void) {
    int ax, ay, aw, ah;
    taskbar_widget_area(&ax, &ay, &aw, &ah);
    int area_bottom = ay + ah;
    int col_right = ax + aw - RAIL_RIGHT_INSET;
    int y = ay + RAIL_TOP_GAP;
    int col_max_w = 0;

    int prev_bottom = ay;   // bottom edge of the last widget in this column
    // (#236) There is no "height estimate" here any more, for ANY card. This
    // block used to mirror draw_weather_card()'s verbose->height mapping by
    // hand (because s_card_h[0] was a stale tracker), and explained that
    // Crypto/Stocks/HA kept reading the tracker since "their real height also
    // depends on the user's configured ticker count, which cannot be known
    // ahead of a first draw either way". It can: the ticker count is in
    // /CRYPTOID.TXT and /STOCKID.TXT, which is what s_quote_rows caches. Every
    // card now reports its height through the one pure function the draw path
    // also uses (card_h()/ha_card_h()), so a reserved slot and a painted box
    // are the same number BY CONSTRUCTION rather than by a mirrored formula
    // that has to be kept in sync by hand.

    // Canonical rail order. `unset` is the SAME field widget_box()/widget_
    // set_pos() treat as that widget's "-1 = not yet placed" sentinel, so
    // this never drifts from how the rest of the file already tests it.
    // (#236) w/h are not an "estimate" any more: every card reports through
    // card_h()/ha_card_h(), the same pure functions widget_box() uses for
    // damage and drag and the draw path uses to paint. This comment used to
    // say crypto/stocks/HA read "their own already-tracked s_card_h[]/s_ha_h"
    // - those trackers are gone, and a comment describing a mechanism that no
    // longer exists is worse than none.
    struct { int id; int vis; int *unset; int *lock; int w, h; } r[10] = {
        { 0,  g_show_clock,      &g_clock_cx,     &g_clock_locked,     2 * s_clk_r, 2 * s_clk_r },
        { 1,  g_show_calendar,   &g_cal_x,        &g_cal_locked,       s_cal_w,     s_cal_h     },
        { 2,  g_show_weather,    &g_weather_x,    &g_weather_locked,   CARD_W,      card_h(0)   },
        { 3,  g_show_crypto,     &g_crypto_x,     &g_crypto_locked,    CARD_W,      card_h(1)   },
        { 4,  g_show_stocks,     &g_stocks_x,     &g_stocks_locked,    CARD_W,      card_h(2)   },
        { 6,  g_show_sysmon,     &g_sysmon_x,     &g_sysmon_locked,    SYSMON_W,    SYSMON_H    },
        { 7,  g_show_timer,      &g_timer_x,      &g_timer_locked,     TIMER_W,     TIMER_H     },
        { 8,  g_show_worldtime,  &g_worldtime_x,  &g_worldtime_locked, WT_W,        WT_H        },
        { 9,  g_show_uptime,     &g_uptime_x,     &g_uptime_locked,    UPT_W,       UPT_H       },
        { 10, g_show_ha,         &g_ha_x,         &g_ha_locked,        HA_W,        ha_card_h() },
    };

    for (int i = 0; i < 10; i++) {
        if (!r[i].vis) continue;                    // hidden widgets reserve no slot
        int w = r[i].w, h = r[i].h;
        int need_place = (*r[i].unset < 0);
        int bx = 0, by = 0, bw = 0, bh = 0, have_box = 0;
        if (!need_place) {
            // (#213) Already has a stored position - but "not -1" is not the
            // same as "a real rail slot on this screen right now". See the
            // rail_pos_trusted() block comment above for the measured bug
            // this closes.
            have_box = widget_box(r[i].id, &bx, &by, &bw, &bh);
            if (have_box && !rail_pos_trusted(*r[i].lock, bx, by, bw, bh, col_right, ay, area_bottom, prev_bottom))
                need_place = 1;
        }
        if (need_place) {
            if (col_max_w > 0 && y + h > area_bottom) {
                // Out of room in this column: start a new one immediately to
                // its left (never off the bottom edge, never stacked on top
                // of the previous widget).
                col_right -= (col_max_w + RAIL_V_GAP);
                y = ay + RAIL_TOP_GAP;
                col_max_w = 0;
                prev_bottom = ay;                        // new column, nothing above yet
            }
            widget_set_pos(r[i].id, col_right - w, y);   // reuse the ONE write-back
            prev_bottom = y + h;
            y += h + RAIL_V_GAP;
            if (w > col_max_w) col_max_w = w;
        } else if (have_box) {
            // (#213) Trusted in-place widget: continue the packing cursor
            // from where it REALLY is on screen, not from an accumulated
            // counter that has no relationship to it. This is what stops a
            // freshly-placed neighbour from overlapping a widget that kept
            // its own (legitimately trusted) position.
            y = by + bh + RAIL_V_GAP;
            prev_bottom = by + bh;
            if (bw > col_max_w) col_max_w = bw;
        }
    }
}

// (#199) The digital clock is deliberately NOT in the rail above: the owner
// asked for it specifically underneath the version/build watermark in the
// far bottom-right corner. `vy` mirrors desktop_render_version()'s own
// formula (taskbar_get_y() - 20) so this never drifts from where that text
// actually is - taskbar_get_y() is the one exported primitive both call.
//
// Whether "underneath" fits depends on the ACTIVE dock style, stated
// explicitly rather than hardcoding Marble's geometry:
//   - Overlay styles (Marble/XFCE): taskbar_widget_area() already extends to
//     the true screen bottom (the floating dock does not fence off the
//     desktop - #40), so there is real room below the watermark and the
//     clock sits there, exactly as asked.
//   - Fencing styles (Default/Classic UNIX/Lumina/Retro Bench): the widget
//     area's bottom edge IS the watermark's own baseline, so there is no
//     room below it at all. Falling through to the generic widget-area
//     clamp here would have pulled the clock UP into the watermark's own
//     text row (measured: clamped Y range 708-764 vs. watermark Y 744 on a
//     1280x800 DOCK_DEFAULT screen - a real overlap, not just a reordering).
//     Instead the clock explicitly falls back to sitting directly ABOVE the
//     watermark, keeping the same bottom-right corner pairing without ever
//     drawing on top of it.
//
// (#213) MEASURED on the exact 1280x800 Marble/XFCE reproduction this ticket
// used: with GAP=8 the "below" arithmetic (version text row FONT_CHAR_H=16 +
// GAP + clock height 56, all measured off taskbar_get_y()-20) needed 80px but
// only 79px were available between the watermark and the true screen bottom
// (area_bottom) - short by exactly 1px, so "below" was rejected every time
// and the clock silently fell back to sitting ABOVE the watermark instead of
// "underneath" it as specced, even though the underlying fix here (below)
// otherwise worked. GAP=6 reclaims those 2px (1 of slack) with no visible
// difference in the gap - it is still a clear, distinct gap, just not 8px -
// and lets the intended "below" placement actually fire on this canonical
// resolution instead of only in taller/looser configurations.
#define DIGCLK_WATERMARK_GAP 6

// (#213) Same "not -1 is not the same as still correct" healing as the rail
// above: a bare "is it set" check let a pre-#199 TOP-right digital clock
// default (measured: dcx=1138 dcy=8 in the shipped asset base, unlocked)
// survive forever, because it is not -1 and nothing ever re-examined it.
// Trusted only if right-aligned to the rail edge AND sitting in the bottom
// quarter of the widget area - a clock parked at y=8 fails the second test
// regardless of x, which is exactly the defect this closes.
static int digclk_pos_trusted(int locked, int bx, int by, int bw,
                               int ax, int aw, int ay, int area_bottom) {
    if (locked) return 1;
    int col_right = ax + aw - RAIL_RIGHT_INSET;
    if (bx + bw < col_right - RAIL_POS_TOL || bx + bw > col_right + RAIL_POS_TOL) return 0;
    int bottom_band_top = ay + (area_bottom - ay) * 3 / 4;
    if (by < bottom_band_top) return 0;
    return 1;
}

// KNOWN FOLLOW-UP (found verifying #213, not one of the four reported
// defects and NOT fixed here): this function only checks its own position
// against the version-text watermark and area_bottom. It does not know
// where the rail column (widgets_layout_rail_defaults() above) ended up. On
// a screen where every widget is enabled AND System Monitor (SYSMON_H=162,
// unusually tall) lands last in the same right-hand column, System Monitor's
// own box can run from y=596 to y=758 while the digital clock's default
// (same 188px column width) lands at y=657-713 - fully inside it, a real
// visual overlap. The owner's actual reported profile has System Monitor,
// Timer and World Time all OFF, so this never fires for the four defects
// this ticket fixes, but it is a real gap for whoever enables all 11
// widgets at once: the two placement passes need to coordinate on the
// occupied Y-range of shared columns, not just on the watermark.
static void widgets_layout_digclock_default(void) {
    int ax, ay, aw, ah;
    taskbar_widget_area(&ax, &ay, &aw, &ah);
    int area_bottom = ay + ah;
    int need_place = (g_digclk_x < 0);
    if (!need_place &&
        !digclk_pos_trusted(g_digclk_locked, g_digclk_x, g_digclk_y, s_digclk_w, ax, aw, ay, area_bottom))
        need_place = 1;
    if (!need_place) return;

    int vy = taskbar_get_y() - 20;             // same formula as desktop_render_version()
    int below = vy + FONT_CHAR_H + DIGCLK_WATERMARK_GAP;
    g_digclk_x = ax + aw - s_digclk_w - RAIL_RIGHT_INSET;
    g_digclk_y = (below + s_digclk_h <= area_bottom) ? below
                                                      : (vy - s_digclk_h - DIGCLK_WATERMARK_GAP);
}

// (#213 verification) Existence-gated position dump, same idiom as this
// file's own g_wxtest/draw_icon_gallery gate just above and perfframe.c's
// PF_GATE_PATH: armed only when /RAILDBG.TXT exists, so it costs nothing on
// a normal boot and never blocks the draw thread (a throttled flat-file
// write, not a poll/sleep - #426). Written to /RAILPOS.TXT so the exact
// pixel positions the layout pass computed can be read back after boot
// without eyeballing a screenshot.
static int g_raildbg = -1;
static void widgets_debug_dump_positions(void) {
    if (g_raildbg < 0) {
        int fd = sys_open("/RAILDBG.TXT", 0);
        g_raildbg = (fd >= 0);
        if (fd >= 0) sys_close(fd);
    }
    if (!g_raildbg) return;
    static unsigned long last_ms = 0;
    unsigned long now = uptime_ms();
    if (now - last_ms < 1000) return;
    last_ms = now;

    int ax, ay, aw, ah; taskbar_widget_area(&ax, &ay, &aw, &ah);
    char buf[900]; int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n,
        "area ax=%d ay=%d aw=%d ah=%d taskbar_get_y=%d\n", ax, ay, aw, ah, taskbar_get_y());
    n += snprintf(buf + n, sizeof(buf) - n, "clock vis=%d cx=%d cy=%d r=%d\n",
        g_show_clock, g_clock_cx, g_clock_cy, s_clk_r);
    n += snprintf(buf + n, sizeof(buf) - n, "calendar vis=%d x=%d y=%d w=%d h=%d\n",
        g_show_calendar, g_cal_x, g_cal_y, s_cal_w, s_cal_h);
    n += snprintf(buf + n, sizeof(buf) - n, "weather vis=%d x=%d y=%d\n",
        g_show_weather, g_weather_x, g_weather_y);
    n += snprintf(buf + n, sizeof(buf) - n, "crypto vis=%d x=%d y=%d\n",
        g_show_crypto, g_crypto_x, g_crypto_y);
    n += snprintf(buf + n, sizeof(buf) - n, "stocks vis=%d x=%d y=%d\n",
        g_show_stocks, g_stocks_x, g_stocks_y);
    n += snprintf(buf + n, sizeof(buf) - n, "sysmon vis=%d x=%d y=%d\n",
        g_show_sysmon, g_sysmon_x, g_sysmon_y);
    n += snprintf(buf + n, sizeof(buf) - n, "timer vis=%d x=%d y=%d\n",
        g_show_timer, g_timer_x, g_timer_y);
    n += snprintf(buf + n, sizeof(buf) - n, "worldtime vis=%d x=%d y=%d\n",
        g_show_worldtime, g_worldtime_x, g_worldtime_y);
    n += snprintf(buf + n, sizeof(buf) - n, "uptime vis=%d x=%d y=%d\n",
        g_show_uptime, g_uptime_x, g_uptime_y);
    n += snprintf(buf + n, sizeof(buf) - n, "ha vis=%d x=%d y=%d\n",
        g_show_ha, g_ha_x, g_ha_y);
    n += snprintf(buf + n, sizeof(buf) - n, "digclock vis=%d x=%d y=%d w=%d h=%d\n",
        g_show_digclock, g_digclk_x, g_digclk_y, s_digclk_w, s_digclk_h);

    sys_unlink("/RAILPOS.TXT");                 // clean truncate, matches profile_save()
    int fd = sys_open("/RAILPOS.TXT", 0x41);    // O_WRONLY | O_CREAT
    if (fd >= 0) { sys_write(fd, buf, (unsigned long)n); sys_close(fd); }
}

void widgets_render(void) {
    // #uiscale (see main.c's g_ui_scale_native_text declaration for the full
    // rationale): every desktop gadget in this file is now converted. The
    // six done earlier (analog + digital clock, sysmon, timer, worldtime,
    // uptime) are joined in this pass by the calendar, the weather/crypto/
    // stocks cards, the Home Assistant card, the debug icon gallery and the
    // sheep/sheepdog pets - see each one's own #uiscale comment for its box
    // geometry. The only push/pop pair left below brackets digclk_geom()'s
    // TEXT MEASUREMENT (it must run scaled, see its own comment); every
    // other gadget draws with the opt-out OFF, which is also the state this
    // function is entered in, so the net effect is still zero on every path.
    //
    // NOT covered by this pass: the per-widget Settings modal
    // (widget_settings_render(), g_wsettings) draws OUTSIDE this function
    // (see main.c's render loop) and is NOT gated by this opt-out at all -
    // its own box literals (WSET_W=400 etc.) are still unscaled while its
    // text already scales, which is the MIRROR of the bug this pass fixes
    // (text growing inside a box that does not, not a box growing around
    // text that does not). Tracked as a follow-up, not fixed here.
    ui_scale_native_push();
    if (g_widgets_enabled) {
        g_draw_blend = g_win_opacity;   // desktop widgets honor the global transparency
        // #uiscale: r (the analog clock radius) is now scaled - see
        // widget_analog_clock()'s own geometry, which is entirely DERIVED
        // from r (ticks/hands/hub), so scaling this one value scales the
        // whole clock face proportionally.
        int r = ui_px(44); s_clk_r = r;
        int calw = ui_px(196); s_cal_w = calw;
        s_cal_h = calendar_card_h();
        // #uiscale: digclk_geom() measures via text_width_ttf(), which
        // honors the SAME native-text opt-out as draw_text_ttf() - it must
        // run OUTSIDE that opt-out (like digclk_draw() below) or it would
        // size the hit-box for 1x text while the glyphs it actually draws
        // are scaled, reintroducing the exact overflow this opt-out exists
        // to prevent, just inverted (box too SMALL for the drawn text).
        ui_scale_native_pop();
        digclk_geom(&s_digclk_w, &s_digclk_h);
        ui_scale_native_push();
        // (#199) One rail-placement pass for every still-unpositioned widget,
        // replacing the three independent hardcoded default groups that used
        // to live inline here (clock/calendar/digital-clock at the right edge,
        // sysmon/timer/worldtime/uptime/HA at the LEFT edge specifically so
        // they would not collide with that right-edge group, and weather/
        // crypto/stocks centered in their own top row inside netinfo_render())
        // - see widgets_layout_rail_defaults() below for why that was exactly
        // the "all over the place" the owner reported on first boot.
        widgets_layout_rail_defaults();
        widgets_layout_digclock_default();
        widgets_debug_dump_positions();   // #213 verification, no-op unless /RAILDBG.TXT exists

        // #uiscale: pop once here and STAY popped for everything below - all
        // eleven gadgets in this file (the six converted earlier plus
        // calendar/HA/weather/crypto/stocks/icon-gallery/sheep/dog converted
        // in this pass) now have scaled box geometry, so scaled text is
        // correct for all of them and there is no later re-arm.
        ui_scale_native_pop();
        if (g_show_clock)     widget_analog_clock(g_clock_cx, g_clock_cy, r);
        // Digital clock (id 5): a normal draggable/lockable widget, not an overlay.
        if (g_show_digclock)  digclk_draw(g_digclk_x, g_digclk_y);
        if (g_show_sysmon)    widget_sysmon(g_sysmon_x, g_sysmon_y);
        if (g_show_timer)     widget_timer(g_timer_x, g_timer_y);
        if (g_show_worldtime) widget_worldtime(g_worldtime_x, g_worldtime_y);
        if (g_show_uptime)    widget_uptime(g_uptime_x, g_uptime_y);

        if (g_show_calendar) widget_calendar(g_cal_x, g_cal_y, calw);
        if (g_show_ha)        ha_card_draw(g_ha_x, g_ha_y);
        netinfo_render();               // #81-83 weather/crypto/stock cards
        if (g_wxtest < 0) { int fd = sys_open("/WXTEST.TXT", 0); g_wxtest = (fd >= 0); if (fd >= 0) sys_close(fd); }
        if (g_wxtest) draw_icon_gallery();
        // (#40/#199) The safety-net clamp. Drag clamps (widget_drag_to) and a
        // dock-style change clamps (taskbar_apply_work_area); this is the
        // third path, for the first frame a still-"unset" widget gets shown.
        // widgets_layout_rail_defaults()/widgets_layout_digclock_default()
        // above already compute their defaults FROM taskbar_widget_area(),
        // so this is normally a no-op - it remains as the final guard for
        // degenerate cases those two did not special-case (e.g. a screen too
        // small to fit even one widget), so nothing can ever be placed
        // fully off-screen. Re-running THE one clamp here (not a second copy
        // of the arithmetic) is cheap and idempotent once everything already
        // sits inside the band.
        if (!g_widgets_draw_only) widgets_clamp_to_bounds();
        g_draw_blend = 255;             // sheep + dog are never transparent
    }
    // (#40) One surface-list refresh per tick for BOTH pets: the dog used to
    // get no list at all when the sheep were disabled, which would have left
    // it walking through the dock.
    if (!g_widgets_draw_only && (g_sheep_enabled || g_dog_enabled))
        pets_refresh_surfaces();
    if (g_sheep_enabled) {
        if (g_sheep_count < 1) g_sheep_count = 1;
        if (g_sheep_count > MAX_SHEEP) g_sheep_count = MAX_SHEEP;
        if (!g_widgets_draw_only) {
            // Advance positions only in the full/busy path; the idle path has
            // already stepped the sheep in widgets_collect_damage().
            for (int i = 0; i < g_sheep_count; i++) {
                if (!g_sheep[i].inited) sheep_spawn(i);
                sheep_one_update(i);
            }
        }
        for (int i = 0; i < g_sheep_count; i++) sheep_one_draw(&g_sheep[i]);
    }
    if (g_dog_enabled) { if (!g_widgets_draw_only) dog_update(); dog_draw(); }
    ui_scale_native_pop();   // #uiscale: matches the push() at the top of this function
}

// ===========================================================================
// #102/#379 dirty-rect: advance per-frame widget state ONCE and record which
// regions changed. Called by the idle compositor before the clipped redraw.
// After this, widgets_render() is invoked in draw-only mode per damage rect.
// Only elements whose displayed content changed contribute damage, so a static
// desktop yields no damage (present nothing = 0 CPU); an animated pet/clock
// yields only its own small rect.
// ===========================================================================
static unsigned strsum(const char *s) {
    unsigned h = 2166136261u;
    for (int i = 0; s && s[i]; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

void widgets_collect_damage(void) {
    int bx, by, bw, bh;

    if (g_widgets_enabled) {
        // #49: damage must be driven by the LOCAL reading, because that is what
        // is on screen. A :30/:45 zone rolls its minute half an hour away from
        // UTC's, and a zone can be on a different calendar DAY, so a UTC-driven
        // rollover would repaint the clock and the calendar at the wrong moment.
        tz_time_t lt; tz_local_now(&lt);
        int sec = lt.sec;
        int minu = lt.min;
        int day = lt.day, mon = lt.month, yr = lt.year;
        (void)mon; (void)yr;

        static int last_sec = -1, last_min = -1, last_day = -1;
        int sec_ch = (sec != last_sec);
        int min_ch = (minu != last_min);
        int day_ch = (day != last_day);
        last_sec = sec; last_min = minu; last_day = day;

        // Analog clock (0): second hand moves each second.
        if (sec_ch && g_show_clock && widget_box(0, &bx, &by, &bw, &bh)) damage_add(bx, by, bw, bh);
        // Calendar (1): only at day rollover.
        if (day_ch && g_show_calendar && widget_box(1, &bx, &by, &bw, &bh)) damage_add(bx, by, bw, bh);
        // Digital clock (5): seconds (when shown) else minute.
        if (((g_digclk_secs && sec_ch) || min_ch) && g_show_digclock &&
            widget_box(5, &bx, &by, &bw, &bh)) damage_add(bx, by, bw, bh);
        // World time (8): minute.
        if (min_ch && g_show_worldtime && widget_box(8, &bx, &by, &bw, &bh)) damage_add(bx, by, bw, bh);
        // Uptime (9): second.
        if (sec_ch && g_show_uptime && widget_box(9, &bx, &by, &bw, &bh)) damage_add(bx, by, bw, bh);
        // Timer (7): while running the display changes continuously.
        if (g_show_timer && s_tmr_running && widget_box(7, &bx, &by, &bw, &bh)) damage_add(bx, by, bw, bh);

        // System monitor (6): sample on a fixed ~0.5s wall-clock cadence (so the
        // rate is independent of the adaptive idle poll interval); damage on a
        // new sample.
        if (g_show_sysmon) {
            static unsigned long last_sm_ms = 0;
            unsigned long nowms = uptime_ms();
            if (nowms - last_sm_ms >= 500) {
                last_sm_ms = nowms;
                sysmon_sample();
                if (widget_box(6, &bx, &by, &bw, &bh)) damage_add(bx, by, bw, bh);
            }
        }

        // Network info cards (2-4): refresh cached files on cadence; damage when
        // the payload actually changed.
        {
            static unsigned last_card_sig = 0;
            if ((s_netinfo_tick++ % 150) == 0) netinfo_refresh();
            unsigned sig = strsum(s_weather) * 131u + strsum(s_crypto) * 17u + strsum(s_stocks);
            if (sig != last_card_sig) {
                last_card_sig = sig;
                for (int c = 0; c < 3; c++)
                    if (widget_box(2 + c, &bx, &by, &bw, &bh)) damage_add(bx, by, bw, bh);
            }
        }
    }

    // Desktop pets: step them on a fixed ~11 FPS wall-clock cadence (so their
    // walk speed stays the same regardless of the adaptive idle poll interval)
    // and damage OLD+NEW rects. The generous margin covers sprite overflow above
    // the logical box and the wall-climb pose. Drawing happens later in
    // widgets_render() (draw-only).
    const int M = 20;   // safety margin around the pet sprite
    if (g_sheep_enabled || g_dog_enabled) {
        static unsigned long last_pet_ms = 0;
        unsigned long nowms = uptime_ms();
        if (nowms - last_pet_ms >= 200) {   // ~5 FPS pets (the old idle path drew
            last_pet_ms = nowms;
            pets_refresh_surfaces();   // (#40) windows + dock chrome, once per tick
            if (g_sheep_enabled) {
                if (g_sheep_count < 1) g_sheep_count = 1;
                if (g_sheep_count > MAX_SHEEP) g_sheep_count = MAX_SHEEP;
                int sw = sheep_w(), sh = sheep_h();
                for (int i = 0; i < g_sheep_count; i++) {
                    if (!g_sheep[i].inited) sheep_spawn(i);
                    int ox = g_sheep[i].x, oy = g_sheep[i].y;
                    sheep_one_update(i);
                    damage_add(ox - M, oy - M, sw + 2 * M, sh + 2 * M);
                    damage_add(g_sheep[i].x - M, g_sheep[i].y - M, sw + 2 * M, sh + 2 * M);
                }
            }
            if (g_dog_enabled) {
                int ox = dog_x, oy = dog_y;
                dog_update();
                if (ox >= 0) damage_add(ox - M, oy - M, DOG_W + 2 * M, DOG_H + 2 * M);
                damage_add(dog_x - M, dog_y - M, DOG_W + 2 * M, DOG_H + 2 * M);
            }
        }
    }
}

// ===========================================================================
// Widget registry: SINGLE source of truth for the available desktop widgets.
// The dynamic widgets tray menu (traymenu.c) enumerates this instead of a
// hardcoded list, so adding a widget here makes it appear in the tray menu
// automatically. The `flag` pointers are the same live globals the desktop
// widget layer toggles, so menu state and on-screen state never diverge.
// ===========================================================================
extern int g_show_digclock;   // defined in clock.c (top-right digital clock)
static const widget_desc_t g_widget_registry[] = {
    { "Digital Clock", "show_digclock", &g_show_digclock  },
    { "Clock",         "show_clock",    &g_show_clock     },
    { "Calendar",      "show_calendar", &g_show_calendar  },
    { "Weather",       "show_weather",  &g_show_weather   },
    { "Crypto",        "show_crypto",   &g_show_crypto    },
    { "Stocks",        "show_stocks",   &g_show_stocks    },
    { "System Monitor","show_sysmon",   &g_show_sysmon    },   // #274
    { "Timer",         "show_timer",    &g_show_timer     },   // #274
    { "World Time",    "show_worldtime",&g_show_worldtime },   // #274
    { "Uptime",        "show_uptime",   &g_show_uptime    },   // #282
    { "Home Assistant","show_ha",       &g_show_ha        },   // #414
    { "Sticky Notes",  "show_stickies", &g_show_stickies  },   // #270
    { "Sheep",         "sheep_show",    &g_sheep_enabled  },
    { "Dog",           "dog_show",      &g_dog_enabled    },   // #745 P2: has a draw fn
                                                                 // (dog_update/dog_draw), a bind
                                                                 // (traymenu.c tm_get/tm_set
                                                                 // "dog_show"), a profile key
                                                                 // ("dog") and its own hash term
                                                                 // in profile_tick(), but was never
                                                                 // added here, so it was invisible
                                                                 // to the Tray > Widgets menu and to
                                                                 // anything (like the widget
                                                                 // live-apply channel) that treats
                                                                 // this array as the widget list.
    { "Maytera AI",    "show_aichat",   &g_aichat_enabled },   // #185 external app
};
const widget_desc_t *widget_registry(int *count) {
    if (count) *count = (int)(sizeof(g_widget_registry) / sizeof(g_widget_registry[0]));
    return g_widget_registry;
}
