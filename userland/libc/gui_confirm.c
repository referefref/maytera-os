// gui_confirm.c - the app-window half of the shared confirm/notice card.
// See gui_style.h's gui_confirm_t block and confirmdialog.h (compositor
// side) for the full rationale (docs/CONFIRM_MODAL_DESIGN.html, 86f3cea).
#include "gui.h"
#include "syscall.h"
#include "string.h"
#include "theme.h"

#define GUI_CONFIRM_SETTLE_MS 250

#define GC_CARD_W      360
#define GC_TITLE_X     16
#define GC_TITLE_Y     16
#define GC_TITLE_PX    20
#define GC_RULE_Y      56
#define GC_RULE_X      16
#define GC_RULE_W      328
#define GC_BODY_X      16
#define GC_BODY_Y      73
#define GC_BODY_W      328    // #204: wrap width, matches GC_RULE_W (content inset to inset)
#define GC_BODY_PX     14
#define GC_BODY_LH     20
#define GC_FOOTER_H    60
#define GC_BTN_H       28
#define GC_BTN_MINW    96
#define GC_BTN_PAD_H   16
#define GC_BTN_GAP     8
#define GC_BTN_INSET_R 16
#define GC_FOCUS_W     2
#define GC_FOCUS_GAP   2

static int gc_card_h(int n_lines) { return 157 + n_lines * GC_BODY_LH; }

void gui_confirm_open(gui_confirm_t *c, gui_confirm_variant_t variant,
                      const char *title, const char *const *lines, int n_lines,
                      const char *cancel_label, const char *action_label)
{
    memset(c, 0, sizeof(*c));
    c->open = 1;
    c->variant = variant;
    strncpy(c->title, title ? title : "", sizeof(c->title) - 1);

    // #204 fix: join whatever paragraph(s) the caller passed into one
    // buffer, then wrap FOR REAL against the card's actual 328px body width
    // (GC_BODY_W below) via gui_wrap_text_ttf() - see gui_style.h's header
    // comment for why "the caller pre-wraps" was a broken assumption at
    // every real call site (Recycle Bin Delete/Empty, End Task/Kill all
    // measured well past 328px on a single unwrapped line).
    if (n_lines < 1) n_lines = 1;
    if (n_lines > GUI_CONFIRM_MAX_LINES) n_lines = GUI_CONFIRM_MAX_LINES;
    char joined[GUI_CONFIRM_LINE_MAX * GUI_CONFIRM_MAX_LINES];
    joined[0] = 0;
    for (int i = 0; i < n_lines; i++) {
        if (i > 0) strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
        if (lines && lines[i])
            strncat(joined, lines[i], sizeof(joined) - strlen(joined) - 1);
    }
    c->n_lines = gui_wrap_text_ttf(joined, GC_BODY_PX, GC_BODY_W, GUI_CONFIRM_MAX_LINES, c->lines);
    if (c->n_lines < 1) c->n_lines = 1;

    if (variant != GUI_CONFIRM_NOTICE)
        strncpy(c->cancel_label, cancel_label ? cancel_label : "Cancel", sizeof(c->cancel_label) - 1);
    strncpy(c->action_label, action_label ? action_label : "OK", sizeof(c->action_label) - 1);
    // Initial focus on Cancel for anything destructive (a bare Enter must
    // never approve), on the one control for anything else - identical rule
    // to confirmdialog.c's confirm_dialog_open().
    c->focus = (variant == GUI_CONFIRM_DESTRUCTIVE) ? 0 : 1;
    c->shown_ms = uptime_ms();
}

void gui_confirm_close(gui_confirm_t *c)
{
    c->open = 0;
}

static int gc_btn_w(const char *label)
{
    int tw = gui_ttf_width(label, GC_BODY_PX);
    int w = tw + GC_BTN_PAD_H * 2;
    return (w < GC_BTN_MINW) ? GC_BTN_MINW : w;
}

typedef struct {
    int px, py, pw, ph;
    int footer_y, btn_y;
    int cancel_x, cancel_w;
    int act_x, act_w;
} gc_geom_t;

static void gc_geom(const gui_confirm_t *c, int win_w, int win_h, gc_geom_t *g)
{
    g->pw = GC_CARD_W;
    g->ph = gc_card_h(c->n_lines);
    g->px = (win_w - g->pw) / 2;
    g->py = (win_h - g->ph) / 2;
    g->footer_y = g->ph - GC_FOOTER_H;
    g->btn_y = g->footer_y + (GC_FOOTER_H - GC_BTN_H) / 2;

    g->act_w = gc_btn_w(c->action_label);
    g->act_x = g->pw - GC_BTN_INSET_R - g->act_w;
    if (c->variant != GUI_CONFIRM_NOTICE) {
        g->cancel_w = gc_btn_w(c->cancel_label);
        g->cancel_x = g->act_x - GC_BTN_GAP - g->cancel_w;
    } else {
        g->cancel_w = 0;
        g->cancel_x = 0;
    }
}

typedef enum { GC_BTN_STANDARD, GC_BTN_PRIMARY, GC_BTN_DANGER } gc_btn_style_t;

static void gc_draw_button(int handle, int x, int y, int w, int h, const char *label,
                           gc_btn_style_t style, int focused, int radius,
                           uint32_t on_surface, uint32_t border_strong, uint32_t surface_raised,
                           uint32_t focus_ring)
{
    uint32_t fill, ink;
    if (style == GC_BTN_STANDARD) {
        fill = surface_raised; ink = on_surface;
        gui_fill_rounded(handle, x, y, w, h, radius, fill);
        gui_draw_rect_outline(handle, x, y, w, h, border_strong);
    } else if (style == GC_BTN_PRIMARY) {
        uint32_t accent = theme_color(THEME_COLOR_ACCENT);
        uint32_t on_accent = theme_color(THEME_COLOR_ON_ACCENT);
        on_accent = gui_ensure_contrast(on_accent, accent, GUI_FLOOR_TEXT);
        fill = accent; ink = on_accent;
        gui_fill_rounded(handle, x, y, w, h, radius, fill);
    } else {
        uint32_t danger = theme_color(THEME_COLOR_DANGER);
        uint32_t on_danger = theme_color(THEME_COLOR_ON_DANGER);
        on_danger = gui_ensure_contrast(on_danger, danger, GUI_FLOOR_TEXT);
        fill = danger; ink = on_danger;
        gui_fill_rounded(handle, x, y, w, h, radius, fill);
    }
    gui_text_ttf_centered(handle, x, y, w, h, label, ink, GC_BODY_PX);
    if (focused) {
        int rx = x - GC_FOCUS_GAP - GC_FOCUS_W, ry = y - GC_FOCUS_GAP - GC_FOCUS_W;
        int rw = w + 2 * (GC_FOCUS_GAP + GC_FOCUS_W), rh = h + 2 * (GC_FOCUS_GAP + GC_FOCUS_W);
        win_draw_rect(handle, rx, ry, rw, GC_FOCUS_W, focus_ring);
        win_draw_rect(handle, rx, ry + rh - GC_FOCUS_W, rw, GC_FOCUS_W, focus_ring);
        win_draw_rect(handle, rx, ry, GC_FOCUS_W, rh, focus_ring);
        win_draw_rect(handle, rx + rw - GC_FOCUS_W, ry, GC_FOCUS_W, rh, focus_ring);
    }
}

void gui_confirm_render(int handle, const gui_confirm_t *c, int win_w, int win_h)
{
    if (!c->open) return;

    // Interlaced-scanline scrim, confined to the app's own window (0,0)-
    // (win_w,win_h) - the exact technique Settings' draw_modal() already
    // ships, for the same reason: an app has no framebuffer read-back to
    // alpha-blend against (design doc section 6).
    for (int yy = 0; yy < win_h; yy += 2)
        win_draw_rect(handle, 0, yy, win_w, 1, 0x00000000);

    gc_geom_t g; gc_geom(c, win_w, win_h, &g);

    uint32_t surface_overlay = theme_color(THEME_COLOR_SURFACE_OVERLAY);
    uint32_t surface_raised  = theme_color(THEME_COLOR_SURFACE_RAISED);
    uint32_t on_surface      = theme_color(THEME_COLOR_ON_SURFACE);
    uint32_t on_surface_mut  = theme_color(THEME_COLOR_MUTED);
    uint32_t border_strong   = theme_color(THEME_COLOR_WINDOW_BORDER);   // color.border_strong, same field (kernel themes.c case 9)
    uint32_t border_subtle   = theme_color(THEME_COLOR_BORDER_SUBTLE);
    uint32_t focus_ring      = theme_color(THEME_COLOR_FOCUS_RING);
    int radius_card = theme_metric(THEME_METRIC_RADIUS_CARD);
    int radius_btn  = theme_metric(THEME_METRIC_RADIUS_BTN);

    gui_fill_rounded(handle, g.px, g.py, g.pw, g.ph, radius_card, surface_overlay);
    gui_draw_rect_outline(handle, g.px, g.py, g.pw, g.ph, border_strong);

    win_draw_text_ttf(handle, g.px + GC_TITLE_X, g.py + GC_TITLE_Y, c->title, GC_TITLE_PX, on_surface);
    win_draw_rect(handle, g.px + GC_RULE_X, g.py + GC_RULE_Y, GC_RULE_W, 1, border_subtle);
    for (int i = 0; i < c->n_lines; i++)
        win_draw_text_ttf(handle, g.px + GC_BODY_X, g.py + GC_BODY_Y + i * GC_BODY_LH,
                          c->lines[i], GC_BODY_PX, on_surface_mut);

    win_draw_rect(handle, g.px, g.py + g.footer_y, g.pw, GC_FOOTER_H, surface_raised);
    win_draw_rect(handle, g.px, g.py + g.footer_y, g.pw, 1, border_subtle);

    if (c->variant == GUI_CONFIRM_NOTICE) {
        gc_draw_button(handle, g.px + g.act_x, g.py + g.btn_y, g.act_w, GC_BTN_H, c->action_label,
                       GC_BTN_PRIMARY, 1, radius_btn, on_surface, border_strong, surface_raised, focus_ring);
    } else {
        gc_btn_style_t act_style = (c->variant == GUI_CONFIRM_DESTRUCTIVE) ? GC_BTN_DANGER : GC_BTN_PRIMARY;
        gc_draw_button(handle, g.px + g.cancel_x, g.py + g.btn_y, g.cancel_w, GC_BTN_H, c->cancel_label,
                       GC_BTN_STANDARD, c->focus == 0, radius_btn, on_surface, border_strong, surface_raised, focus_ring);
        gc_draw_button(handle, g.px + g.act_x, g.py + g.btn_y, g.act_w, GC_BTN_H, c->action_label,
                       act_style, c->focus == 1, radius_btn, on_surface, border_strong, surface_raised, focus_ring);
    }
}

int gui_confirm_handle_key(gui_confirm_t *c, int key)
{
    if (!c->open) return 0;
    if (uptime_ms() - c->shown_ms < GUI_CONFIRM_SETTLE_MS) return 0;

    if (key == 27) {
        int r = (c->variant == GUI_CONFIRM_NOTICE) ? 2 : 1;
        gui_confirm_close(c);
        return r;
    }
    if (key == '\t') {
        if (c->variant != GUI_CONFIRM_NOTICE) c->focus = c->focus ? 0 : 1;
        return 0;
    }
    if (key == '\n' || key == '\r' || key == ' ') {
        if (c->variant == GUI_CONFIRM_NOTICE) { gui_confirm_close(c); return 2; }
        int r = c->focus ? 2 : 1;
        gui_confirm_close(c);
        return r;
    }
    return 0;
}

int gui_confirm_handle_mouse(gui_confirm_t *c, int32_t x, int32_t y, int clicked,
                             int win_w, int win_h)
{
    if (!c->open) return 0;
    if (uptime_ms() - c->shown_ms < GUI_CONFIRM_SETTLE_MS) return 0;
    if (!clicked) return 0;

    gc_geom_t g; gc_geom(c, win_w, win_h, &g);
    int by = g.py + g.btn_y;
    if (y >= by && y < by + GC_BTN_H) {
        if (c->variant == GUI_CONFIRM_NOTICE) {
            int ax = g.px + g.act_x;
            if (x >= ax && x < ax + g.act_w) { gui_confirm_close(c); return 2; }
            return 0;
        }
        int cx = g.px + g.cancel_x, ax = g.px + g.act_x;
        if (x >= cx && x < cx + g.cancel_w) { gui_confirm_close(c); return 1; }
        if (x >= ax && x < ax + g.act_w)    { gui_confirm_close(c); return 2; }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// FFI-safe singleton wrapper (gui_style.h has the full rationale).
// ---------------------------------------------------------------------------
static gui_confirm_t g_gui_confirm_singleton;
static int g_gui_confirm_singleton_win_w, g_gui_confirm_singleton_win_h;

void gui_confirm_open_s(int variant, const char *title,
                        const char *line0, const char *line1, const char *line2, int n_lines,
                        const char *cancel_label, const char *action_label)
{
    const char *lines[3] = { line0, line1, line2 };
    if (n_lines < 1) n_lines = 1;
    if (n_lines > 3) n_lines = 3;
    gui_confirm_open(&g_gui_confirm_singleton, (gui_confirm_variant_t)variant, title,
                     lines, n_lines, cancel_label, action_label);
}

int gui_confirm_singleton_is_open(void) { return gui_confirm_is_open(&g_gui_confirm_singleton); }

void gui_confirm_singleton_render(int handle, int win_w, int win_h)
{
    g_gui_confirm_singleton_win_w = win_w;
    g_gui_confirm_singleton_win_h = win_h;
    gui_confirm_render(handle, &g_gui_confirm_singleton, win_w, win_h);
}

int gui_confirm_singleton_handle_key(int key)
{
    return gui_confirm_handle_key(&g_gui_confirm_singleton, key);
}

int gui_confirm_singleton_handle_mouse(int32_t x, int32_t y, int clicked)
{
    return gui_confirm_handle_mouse(&g_gui_confirm_singleton, x, y, clicked,
                                    g_gui_confirm_singleton_win_w, g_gui_confirm_singleton_win_h);
}
