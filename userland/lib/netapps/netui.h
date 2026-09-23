// netui.h - small header-only UI helpers shared by the network apps (dlmgr,
// restcli). Everything draws through the shared style engine (gui_style.h),
// with the live theme mapped into gui_set_palette() the same way Feeds,
// Files and Settings do, so the apps render modern (rounded, antialiased) or
// classic (bevelled) exactly like the rest of the desktop.
//
// The one widget here that is NOT a thin wrapper is nu_field_draw(): the shared
// gui_textfield_tf() has no horizontal scroll, so a URL longer than the field
// simply overflows it. Long URLs and request bodies are the normal case for
// these two apps, so this draws the same field look from the palette tokens
// and keeps the caret in view by scrolling the text. It reuses textfield_t and
// tf_handle_key() for all editing; only the painting differs. If
// gui_textfield_tf() grows a scroll offset this can go away.
#ifndef NETAPPS_NETUI_H
#define NETAPPS_NETUI_H

#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"
#include "../../libc/gui_theme.h"
#include "../../libc/gui_style.h"
#include "../../libc/textfield.h"
#include "../../libc/keys.h"

#define NU_TTF_BODY  14
#define NU_TTF_SMALL 11
#define NU_FIELD_H   28
#define NU_BTN_H     28

// ---- theme-derived palette (same derivation as the Feeds app, #280) --------
static inline unsigned int nu_lum(unsigned int c) {
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return (unsigned int)((r * 30 + g * 59 + b * 11) / 100);
}
static inline int nu_dark(void) { return nu_lum(theme_color(THEME_COLOR_WINDOW_BG)) < 128; }
static inline unsigned int nu_tint(unsigned int base, unsigned int acc, int pct) {
    int br = (base >> 16) & 0xFF, bg = (base >> 8) & 0xFF, bb = base & 0xFF;
    int ar = (acc >> 16) & 0xFF, ag = (acc >> 8) & 0xFF, ab = acc & 0xFF;
    return ((((br * (100 - pct) + ar * pct) / 100) & 0xFF) << 16) |
           ((((bg * (100 - pct) + ag * pct) / 100) & 0xFF) << 8) |
            (((bb * (100 - pct) + ab * pct) / 100) & 0xFF);
}
static inline unsigned int nu_acc(void)     { return theme_color(THEME_COLOR_ACCENT); }
static inline unsigned int nu_content(void) { return nu_tint(nu_dark() ? 0x00262A30 : 0x00F5F6F8, nu_acc(), 5); }
static inline unsigned int nu_panel(void)   { return nu_tint(nu_dark() ? 0x001E2127 : 0x00E8EAEE, nu_acc(), 7); }
static inline unsigned int nu_toolbar(void) { return nu_tint(nu_dark() ? 0x002C313B : 0x00EDEFF3, nu_acc(), 6); }
static inline unsigned int nu_field(void)   { return nu_dark() ? 0x00333A45 : 0x00FFFFFF; }
static inline unsigned int nu_border(void)  { return nu_dark() ? 0x003A424F : 0x00CDD3DB; }
static inline unsigned int nu_sel(void) {
    return nu_dark() ? nu_tint(0x003C434F, nu_acc(), 28) : nu_tint(0x00CCD6E6, nu_acc(), 26);
}
static inline unsigned int nu_fg(unsigned int bg) { return nu_lum(bg) > 140 ? 0x00181818u : 0x00F0F0F0u; }
static inline unsigned int nu_dim(unsigned int bg) {
    unsigned int ink = nu_fg(bg);
    int ir = (ink >> 16) & 0xFF, ig = (ink >> 8) & 0xFF, ib = ink & 0xFF;
    int br = (bg >> 16) & 0xFF, bgc = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    return ((((ir * 5 + br * 3) / 8) & 0xFF) << 16) | ((((ig * 5 + bgc * 3) / 8) & 0xFF) << 8) | (((ib * 5 + bb * 3) / 8) & 0xFF);
}
// Pull a semantic hue toward readable contrast on the given surface.
static inline unsigned int nu_on(unsigned int fg, unsigned int bg) {
    int need_dark = nu_lum(bg) > 140;
    unsigned int lum = nu_lum(fg);
    if (need_dark && lum > 120) return nu_tint(fg, 0x00000000, 55);
    if (!need_dark && lum < 110) return nu_tint(fg, 0x00FFFFFF, 45);
    return fg;
}
#define NU_OK_RAW    0x0058C070
#define NU_ERR_RAW   0x00FF6666
#define NU_WARN_RAW  0x00E0A030

// Map the derived palette into the shared style engine. Call once per redraw
// (cheap) so a theme switch only needs a repaint.
static inline void nu_apply_style(void) {
    gui_set_style(gui_theme_is_classic() ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
    gui_palette_t p;
    p.surface        = nu_content();
    p.surface_raised = nu_toolbar();
    p.ink            = nu_fg(nu_content());
    p.ink_dim        = nu_dim(nu_content());
    p.accent         = nu_acc();
    p.accent_hover   = gui_lighten(nu_acc(), 24);
    p.border         = nu_border();
    p.field_bg       = nu_field();
    p.field_border   = theme_color(THEME_COLOR_TEXTBOX_BORDER);
    p.track          = nu_tint(nu_content(), nu_acc(), 30);
    p.focus          = nu_acc();
    p.edge_strong    = nu_border();
    gui_set_palette(&p);
}

// ---- text measuring (no-kerning widths, matching win_draw_text_ttf) --------
static inline int nu_text_w(const char *s, int size) { return gui_ttf_render_width(s, size); }

// Per-glyph width cache for the body size, so wrapping and caret placement
// are one table lookup per character rather than a syscall.
static int nu__cw[128];
static int nu__cw_ready;
static inline int nu_char_w(char c) {
    if (!nu__cw_ready) {
        char one[2] = {0, 0};
        for (int i = 32; i < 127; i++) { one[0] = (char)i; nu__cw[i] = gui_ttf_render_width(one, NU_TTF_BODY); if (nu__cw[i] <= 0) nu__cw[i] = 7; }
        nu__cw_ready = 1;
    }
    unsigned char u = (unsigned char)c;
    if (u < 32 || u > 126) return nu__cw['?'];
    return nu__cw[u];
}
static inline int nu_span_w(const char *s, int from, int to) {
    int w = 0;
    for (int i = from; i < to && s[i]; i++) w += nu_char_w(s[i]);
    return w;
}

// Draw `s` truncated with "..." to fit `maxw` px at the body size.
static inline void nu_text_fit(int win, int x, int y, const char *s, int maxw, unsigned int col) {
    char buf[256];
    int n = 0, w = 0;
    int full = 1;
    for (; s[n] && n < (int)sizeof(buf) - 4; n++) {
        int cw = nu_char_w(s[n]);
        if (w + cw > maxw) { full = 0; break; }
        w += cw;
    }
    if (full && !s[n]) { win_draw_text_ttf(win, x, y, s, NU_TTF_BODY, col); return; }
    int dots = 3 * nu_char_w('.');
    while (n > 0 && w + dots > maxw) { n--; w -= nu_char_w(s[n]); }
    memcpy(buf, s, (unsigned)n);
    buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = '.'; buf[n + 3] = 0;
    win_draw_text_ttf(win, x, y, buf, NU_TTF_BODY, col);
}

// ---- scrolling single-line field -------------------------------------------
typedef struct {
    textfield_t tf;
    int start;        // first visible character index (scroll position)
    int x, y, w, h;   // last drawn rect, for hit testing
} nu_field_t;

static inline void nu_field_init(nu_field_t *f, char *buf, int cap) {
    tf_init(&f->tf, buf, cap);
    f->start = 0;
    f->x = f->y = f->w = f->h = 0;
}

static inline void nu_field_draw(int win, nu_field_t *f, int x, int y, int w, int h,
                                 bool focused, const char *placeholder) {
    gui_palette_t *p = gui_pal();
    ui_style_t st = gui_active_style();
    f->x = x; f->y = y; f->w = w; f->h = h;
    int r = (st.base == GUI_STYLE_CLASSIC) ? 0 : (st.radius > 6 ? 6 : st.radius);
    gui_fill_rounded(win, x, y, w, h, r, p->field_bg);
    gui_rounded_border(win, x, y, w, h, r, focused ? p->focus : p->field_border);
    if (focused && GUI_FOCUS_W > 1) gui_rounded_border(win, x + 1, y + 1, w - 2, h - 2, r > 0 ? r - 1 : 0, p->focus);

    const int pad = 8;
    int inner = w - 2 * pad;
    if (inner < 8) inner = 8;
    const char *s = f->tf.buf;
    int len = f->tf.len, cur = f->tf.cursor;
    int ty = y + (h - NU_TTF_BODY - 4) / 2;
    unsigned int ink = gui_ink_on(p->field_bg);

    if (len == 0) {
        if (placeholder && placeholder[0]) nu_text_fit(win, x + pad, ty, placeholder, inner, gui_mix(p->field_bg, ink, 110));
        if (focused) win_draw_rect(win, x + pad, y + 6, 1, h - 12, ink);
        return;
    }
    // keep the caret inside the window: scroll left if before start, right if past the end
    if (cur < f->start) f->start = cur;
    while (f->start < cur && nu_span_w(s, f->start, cur) > inner) f->start++;
    if (f->start > len) f->start = len;
    // visible run
    int end = f->start, vw = 0;
    while (end < len) { int cw = nu_char_w(s[end]); if (vw + cw > inner) break; vw += cw; end++; }
    char vis[512];
    int vn = end - f->start;
    if (vn > (int)sizeof(vis) - 1) vn = (int)sizeof(vis) - 1;
    memcpy(vis, s + f->start, (unsigned)vn);
    vis[vn] = 0;
    // selection highlight
    if (tf_sel_active(&f->tf)) {
        int lo = tf_sel_lo(&f->tf), hi = tf_sel_hi(&f->tf);
        if (lo < f->start) lo = f->start;
        if (hi > end) hi = end;
        if (hi > lo) {
            int sx = x + pad + nu_span_w(s, f->start, lo);
            int sw = nu_span_w(s, lo, hi);
            win_draw_rect(win, sx, y + 4, sw, h - 8, nu_sel());
        }
    }
    win_draw_text_ttf(win, x + pad, ty, vis, NU_TTF_BODY, ink);
    if (focused && cur >= f->start && cur <= end) {
        int cx = x + pad + nu_span_w(s, f->start, cur);
        win_draw_rect(win, cx, y + 6, 1, h - 12, ink);
    }
}

static inline int nu_field_hit(const nu_field_t *f, int mx, int my) {
    return mx >= f->x && my >= f->y && mx < f->x + f->w && my < f->y + f->h;
}

// Place the caret at the character under mx (content coords).
static inline void nu_field_click(nu_field_t *f, int mx) {
    int px = mx - (f->x + 8);
    int i = f->start, acc = 0;
    const char *s = f->tf.buf;
    while (i < f->tf.len) {
        int cw = nu_char_w(s[i]);
        if (acc + cw / 2 >= px) break;
        acc += cw; i++;
    }
    tf_set_caret(&f->tf, i);
}

// ---- buttons ---------------------------------------------------------------
typedef struct {
    int x, y, w, h;
    const char *label;
    int variant;      // gui_btn_variant_t
    int enabled;
    int id;
} nu_btn_t;

static inline void nu_btn_draw(int win, const nu_btn_t *b, int hover_id, int pressed_id) {
    gui_state_t st = GUI_ST_NORMAL;
    if (!b->enabled) st = GUI_ST_DISABLED;
    else if (pressed_id == b->id) st = GUI_ST_PRESSED;
    else if (hover_id == b->id) st = GUI_ST_HOVER;
    gui_button(win, b->x, b->y, b->w, b->h, b->label, (gui_btn_variant_t)b->variant, st);
}

static inline int nu_btn_hit(const nu_btn_t *b, int mx, int my) {
    return b->enabled && mx >= b->x && my >= b->y && mx < b->x + b->w && my < b->y + b->h;
}

static inline int nu_btn_w(const char *label) {
    int w = nu_text_w(label, NU_TTF_BODY) + 2 * GUI_PAD + 6;
    return w < 64 ? 64 : w;
}

// ---- formatting ------------------------------------------------------------
static inline void nu_fmt_bytes(unsigned long n, char *out, int cap) {
    if (n < 1024UL) snprintf(out, cap, "%lu B", n);
    else if (n < 1024UL * 1024UL) snprintf(out, cap, "%lu.%lu KB", n / 1024UL, (n % 1024UL) * 10UL / 1024UL);
    else if (n < 1024UL * 1024UL * 1024UL) snprintf(out, cap, "%lu.%lu MB", n / (1024UL * 1024UL), (n % (1024UL * 1024UL)) * 10UL / (1024UL * 1024UL));
    else snprintf(out, cap, "%lu.%lu GB", n / (1024UL * 1024UL * 1024UL), (n % (1024UL * 1024UL * 1024UL)) * 10UL / (1024UL * 1024UL * 1024UL));
}

static inline void nu_fmt_ms(unsigned long long us, char *out, int cap) {
    unsigned long ms = (unsigned long)(us / 1000ULL);
    if (ms < 1000UL) snprintf(out, cap, "%lu ms", ms);
    else snprintf(out, cap, "%lu.%02lu s", ms / 1000UL, (ms % 1000UL) / 10UL);
}

// Panel / card with a small caption in the top-left.
static inline void nu_card_titled(int win, int x, int y, int w, int h, const char *title) {
    gui_card(win, x, y, w, h);
    if (title && title[0]) {
        gui_palette_t *p = gui_pal();
        win_draw_text_ttf(win, x + GUI_PAD, y + 6, title, NU_TTF_SMALL, p->ink_dim);
    }
}

#endif // NETAPPS_NETUI_H
