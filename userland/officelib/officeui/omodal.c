// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// omodal.c - the declarative modal framework (see omodal.h).
#include "omodal.h"
#include "ocombo.h"
#include "oui.h"
#include "officeui_priv.h"
#include "../../libc/string.h"
#include <stdlib.h>

#define OM_SETTLE_MS   250
#define OM_TITLE_X     16
#define OM_TITLE_Y     16
#define OM_RULE_Y      56
#define OM_BODY_Y      73
#define OM_INSET       16
#define OM_LABEL_W     120
#define OM_FIELD_X     144
#define OM_ROW_H       32
#define OM_FOOTER_H    60
#define OM_BTN_H       28
#define OM_BTN_MINW    96
#define OM_BTN_PAD     16
#define OM_BTN_GAP     8
#define OM_NUM_W       96
#define OM_BODY_LH     20
#define OM_MIN_CARD_H  197

// Focus slots beyond the fields.
#define OMF_THIRD  1000
#define OMF_CANCEL 1001
#define OMF_OK     1002
#define OMF_NONE   (-1)

typedef struct {
    office_app *app; int win;
    const om_spec_t *spec;
    const om_custom_t *custom; int custom_h;
    const char *title, *ok_label, *cancel_label, *third_label;
    int size, ok_is_danger;
    // Body text (confirm3).
    char lines[3][GUI_WRAP_COL]; int n_lines;
    // Per-field runtime.
    textfield_t tf[OM_MAX_FIELDS];
    ocombo_t    combo[OM_MAX_FIELDS];
    gui_list_t  list[OM_MAX_FIELDS];
    int         valid[OM_MAX_FIELDS];
    int         fx[OM_MAX_FIELDS], fy[OM_MAX_FIELDS], fw[OM_MAX_FIELDS], fh[OM_MAX_FIELDS]; // field rect
    int         ry[OM_MAX_FIELDS], rh[OM_MAX_FIELDS];                                       // row rect
    // Snapshot for Cancel.
    char *snap_buf[OM_MAX_FIELDS]; int snap_sel[OM_MAX_FIELDS], snap_checked[OM_MAX_FIELDS];
    // Card geometry (window coordinates).
    int cx, cy, cw, ch, body_h, preview_y, custom_y;
    int btn_y, ok_x, ok_w, cancel_x, cancel_w, third_x, third_w;
    // Interaction.
    int focus, hover_btn, pressed_btn, open_combo, all_valid;
    unsigned long long shown_ms;
    int win_w, win_h;
} om_t;

static int om_nf(const om_t *m) { return m->spec ? m->spec->n_fields : 0; }
static const om_field_t *om_f(const om_t *m, int i) { return &m->spec->fields[i]; }

static int om_focusable(const om_t *m, int i) {
    return i >= 0 && i < om_nf(m) && om_f(m, i)->kind != OM_LABEL;
}

// --- Number validation ---------------------------------------------------------
static int om_parse_int(const char *s, int *out) {
    if (!s || !*s) return 0;
    int i = 0, neg = 0, v = 0, digits = 0;
    if (s[0] == '-') { neg = 1; i = 1; }
    for (; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (s[i] - '0'); digits++;
        if (v > 1000000) return 0;
    }
    if (!digits) return 0;
    *out = neg ? -v : v;
    return 1;
}

static void om_validate(om_t *m) {
    m->all_valid = 1;
    for (int i = 0; i < om_nf(m); i++) {
        const om_field_t *f = om_f(m, i);
        m->valid[i] = 1;
        if (f->kind == OM_NUMBER && f->buf) {
            int v;
            if (!om_parse_int(f->buf, &v) || v < f->min || v > f->max) m->valid[i] = 0;
        }
        if (!m->valid[i]) m->all_valid = 0;
    }
}

static void om_changed(om_t *m) {
    om_validate(m);
    if (m->spec && m->spec->on_change) m->spec->on_change(m->spec->ctx);
}

// --- Combo label adapter over a const char *const options[] ---------------------
typedef struct { const char *const *opts; int n; } om_opts_t;
static om_opts_t g_om_opts[OM_MAX_FIELDS];
static const char *om_opt_label(void *ctx, int index, char *buf, int cap) {
    (void)buf; (void)cap;
    om_opts_t *o = (om_opts_t *)ctx;
    if (!o || index < 0 || index >= o->n || !o->opts[index]) return "";
    return o->opts[index];
}

// --- Layout ----------------------------------------------------------------------
static int om_btn_w(const char *label, int bold) {
    int tw = bold ? oui_bold_width(label, oui_body()) : gui_ttf_render_width(label, oui_body());
    int w = tw + 2 * OM_BTN_PAD;
    return w < OM_BTN_MINW ? OM_BTN_MINW : w;
}

static void om_layout(om_t *m) {
    int ih = oui_input_h(), row_h = oui_row_h();
    m->cw = m->size;
    // Body height.
    int y = 0;
    int nf = om_nf(m);
    int i = 0;
    while (i < nf) {
        const om_field_t *f = om_f(m, i);
        int h = OM_ROW_H;
        if (f->kind == OM_LIST) { int rows = f->list_rows > 0 ? f->list_rows : 6; h = rows * row_h + 2 + 8; }
        m->ry[i] = y; m->rh[i] = h;
        if (f->half && i + 1 < nf) {           // two fields share this row
            m->ry[i + 1] = y; m->rh[i + 1] = h;
            i += 2;
        } else i += 1;
        y += h;
    }
    m->body_h = y;
    if (m->n_lines) m->body_h += m->n_lines * OM_BODY_LH;
    if (m->custom) { m->custom_y = m->body_h; m->body_h += m->custom_h; }
    if (m->spec && m->spec->preview_h > 0) { m->preview_y = m->body_h + 8; m->body_h += 8 + m->spec->preview_h; }
    m->ch = OM_BODY_Y + m->body_h + 16 + OM_FOOTER_H;
    if (m->ch < OM_MIN_CARD_H) m->ch = OM_MIN_CARD_H;
    m->cx = (m->win_w - m->cw) / 2;
    m->cy = (m->win_h - m->ch) / 2;
    if (m->cx < 0) m->cx = 0;
    if (m->cy < 0) m->cy = 0;
    // Field rects (window coordinates).
    i = 0;
    while (i < nf) {
        const om_field_t *f = om_f(m, i);
        int span_both = (f->label == 0 && f->kind != OM_CHECK) || f->kind == OM_LABEL;
        int colx = m->cx + (span_both ? OM_INSET : OM_FIELD_X);
        int colw = m->cw - OM_INSET - (span_both ? OM_INSET : OM_FIELD_X);
        int pair = (f->half && i + 1 < nf);
        for (int k = 0; k < (pair ? 2 : 1); k++) {
            int j = i + k;
            const om_field_t *g = om_f(m, j);
            int x = colx, w = colw;
            if (pair) { int half = (colw - 8) / 2; x = colx + k * (half + 8); w = half; }
            int rowy = m->cy + OM_BODY_Y + m->ry[j];
            switch (g->kind) {
                case OM_TEXT:   m->fx[j] = x; m->fy[j] = rowy + (OM_ROW_H - ih) / 2; m->fw[j] = w; m->fh[j] = ih; break;
                case OM_NUMBER: m->fx[j] = x; m->fy[j] = rowy + (OM_ROW_H - ih) / 2; m->fw[j] = w < OM_NUM_W ? w : OM_NUM_W; m->fh[j] = ih; break;
                case OM_COMBO:  m->fx[j] = x; m->fy[j] = rowy + (OM_ROW_H - ih) / 2; m->fw[j] = w; m->fh[j] = ih;
                                ocombo_place(&m->combo[j], m->fx[j], m->fy[j], m->fw[j]); break;
                case OM_CHECK:  m->fx[j] = x; m->fy[j] = rowy + (OM_ROW_H - 16) / 2; m->fw[j] = 16; m->fh[j] = 16; break;
                case OM_RADIO:  m->fx[j] = x; m->fy[j] = rowy + (OM_ROW_H - 13) / 2; m->fw[j] = w; m->fh[j] = 13; break;
                case OM_LIST:   m->fx[j] = x; m->fy[j] = rowy + 4; m->fw[j] = w; m->fh[j] = m->rh[j] - 8;
                                gui_list_config(&m->list[j], m->fx[j], m->fy[j], m->fw[j], m->fh[j], row_h, g->n_options);
                                m->list[j].scroll.snap = 1; break;
                default:        m->fx[j] = x; m->fy[j] = rowy; m->fw[j] = w; m->fh[j] = OM_ROW_H; break;
            }
        }
        i += pair ? 2 : 1;
    }
    // Buttons.
    m->btn_y = m->cy + m->ch - OM_FOOTER_H + (OM_FOOTER_H - OM_BTN_H) / 2;
    m->ok_w = om_btn_w(m->ok_label, 1);
    m->ok_x = m->cx + m->cw - OM_INSET - m->ok_w;
    m->cancel_w = om_btn_w(m->cancel_label, 0);
    m->cancel_x = m->ok_x - OM_BTN_GAP - m->cancel_w;
    if (m->third_label) {
        m->third_w = om_btn_w(m->third_label, 0);
        m->third_x = m->cancel_x - OM_BTN_GAP - m->third_w;
    } else { m->third_w = 0; m->third_x = 0; }
}

// --- Drawing ----------------------------------------------------------------------
typedef enum { OMB_STANDARD, OMB_PRIMARY, OMB_DANGER } om_btn_style_t;

static void om_draw_button(om_t *m, int x, int w, const char *label, om_btn_style_t style, int slot, int enabled) {
    int win = m->win;
    gui_palette_t *p = gui_pal();
    gui_state_t st = GUI_ST_NORMAL;
    if (!enabled) st = GUI_ST_DISABLED;
    else if (m->pressed_btn == slot && m->hover_btn == slot) st = GUI_ST_PRESSED;
    else if (m->hover_btn == slot) st = GUI_ST_HOVER;
    gui_btn_variant_t v = style == OMB_PRIMARY ? GUI_BTN_PRIMARY : style == OMB_DANGER ? GUI_BTN_DANGER : GUI_BTN_SECONDARY;
    if (style == OMB_STANDARD) {
        gui_button(win, x, m->btn_y, w, OM_BTN_H, label, v, st);
    } else {
        // Face from the engine, label in body_strong (a real bold face).
        gui_button(win, x, m->btn_y, w, OM_BTN_H, "", v, st);
        uint32_t base = (style == OMB_PRIMARY) ? p->accent : oui_tok(THEME_COLOR_ERROR, 0x00CC0000);
        if (st == GUI_ST_HOVER) base = (style == OMB_PRIMARY) ? p->accent_hover : gui_lighten(base, 18);
        else if (st == GUI_ST_PRESSED) base = gui_darken(base, 18);
        uint32_t ink = gui_ink_on(base);
        if (!enabled) {
            base = gui_mix(base, p->surface, 150);
            ink  = gui_ensure_contrast(gui_mix(ink, p->surface, 110), base, GUI_FLOOR_NONTEXT);
        } else ink = gui_ensure_contrast(ink, base, GUI_FLOOR_TEXT);
        oui_text_bold_centered(win, x, m->btn_y, w, OM_BTN_H, label, oui_body(), ink);
    }
    if (m->focus == slot) oui_focus_ring(win, x, m->btn_y, w, OM_BTN_H);
}

static void om_draw_radio(om_t *m, int i, uint32_t ground) {
    const om_field_t *f = om_f(m, i);
    int win = m->win, size = oui_body();
    gui_palette_t *p = gui_pal();
    uint32_t ring = gui_ensure_contrast(p->field_border, ground, GUI_FLOOR_NONTEXT);
    uint32_t ink  = oui_ink_on(ground);
    int x = m->fx[i], cy = m->fy[i];
    int sel = f->sel ? *f->sel : -1;
    for (int k = 0; k < f->n_options; k++) {
        const char *lbl = f->options[k] ? f->options[k] : "";
        int tw = gui_ttf_render_width(lbl, size);
        if (x + 18 + tw > m->fx[i] + m->fw[i]) break;          // never draw past the column
        gui_fill_circle_aa(win, x, cy, 13, ring, ground);
        gui_fill_circle_aa(win, x + 1, cy + 1, 11, p->field_bg, ring);
        if (k == sel) gui_fill_circle_aa(win, x + 3, cy + 3, 7, gui_ensure_contrast(p->accent, p->field_bg, GUI_FLOOR_NONTEXT), p->field_bg);
        win_draw_text_ttf(win, x + 18, oui_text_y(cy, 13, size), lbl, size, ink);
        if (m->focus == i && k == sel) oui_focus_ring(win, x, cy, 18 + tw, 13);
        x += 18 + tw + 16;
    }
}

static const char *om_opt(const om_field_t *f, int k) {
    return (k >= 0 && k < f->n_options && f->options[k]) ? f->options[k] : "";
}
static const char *om_list_label(void *ctx, int index, char *buf, int cap) {
    (void)buf; (void)cap;
    return om_opt((const om_field_t *)ctx, index);
}

static void om_draw(om_t *m) {
    int win = m->win;
    oui_scrim(win, m->win_w, m->win_h);
    uint32_t overlay = oui_overlay(), raised = oui_raised();
    uint32_t ink = oui_ink_on(overlay), muted = oui_muted_on(overlay);
    uint32_t border = gui_ensure_contrast(oui_border_strong(), overlay, GUI_FLOOR_NONTEXT);
    uint32_t subtle = gui_ensure_contrast(oui_border_subtle(), overlay, GUI_FLOOR_NONTEXT);
    int radius = theme_metric(THEME_METRIC_RADIUS_CARD);
    int size = oui_body(), cap = oui_caption();
    gui_palette_t *p = gui_pal();

    gui_fill_rounded(win, m->cx, m->cy, m->cw, m->ch, radius, overlay);
    gui_draw_rect_outline(win, m->cx, m->cy, m->cw, m->ch, border);
    oui_text_bold(win, m->cx + OM_TITLE_X, m->cy + OM_TITLE_Y, m->title, oui_heading(), ink);
    win_draw_rect(win, m->cx + OM_INSET, m->cy + OM_RULE_Y, m->cw - 2 * OM_INSET, 1, subtle);

    // Body text lines (confirm3).
    for (int i = 0; i < m->n_lines; i++)
        win_draw_text_ttf(win, m->cx + OM_INSET, m->cy + OM_BODY_Y + i * OM_BODY_LH, m->lines[i], size, muted);

    // Fields.
    for (int i = 0; i < om_nf(m); i++) {
        const om_field_t *f = om_f(m, i);
        int rowy = m->cy + OM_BODY_Y + m->ry[i];
        int focused = (m->focus == i);
        // Left-column label (not for CHECK: inline; not when spanning).
        if (f->label && f->kind != OM_CHECK) {
            int ly = (f->kind == OM_LIST) ? rowy + 4 + (oui_row_h() - size) / 2 : oui_text_y(rowy, OM_ROW_H, size);
            char el[64]; oui_ellipsize(el, sizeof(el), f->label, size, OM_LABEL_W - 8);
            win_draw_text_ttf(win, m->cx + OM_INSET, ly, el, size, ink);
        }
        switch (f->kind) {
            case OM_TEXT:
            case OM_NUMBER: {
                textfield_t *tf = &m->tf[i];
                gui_textfield_tf(win, m->fx[i], m->fy[i], m->fw[i], m->fh[i], tf->buf, tf->len, tf->cursor,
                                 tf->sel_anchor, focused ? true : false, 0);
                if (f->kind == OM_NUMBER && !m->valid[i]) {
                    uint32_t danger = gui_ensure_contrast(oui_tok(THEME_COLOR_DANGER, 0x00CC0000), overlay, GUI_FLOOR_NONTEXT);
                    gui_draw_rect_outline(win, m->fx[i], m->fy[i], m->fw[i], m->fh[i], danger);
                    char hint[40]; int n = oui_itoa(f->min, hint, sizeof(hint));
                    hint[n++] = '.'; hint[n++] = '.'; hint[n] = 0;
                    n += oui_itoa(f->max, hint + n, (int)sizeof(hint) - n);
                    uint32_t hink = gui_ensure_contrast(danger, overlay, GUI_FLOOR_TEXT);
                    win_draw_text_ttf(win, m->fx[i] + m->fw[i] + 8, oui_text_y(m->fy[i], m->fh[i], cap), hint, cap, hink);
                }
                if (focused) oui_focus_ring(win, m->fx[i], m->fy[i], m->fw[i], m->fh[i]);
                break;
            }
            case OM_COMBO:
                ocombo_draw_ex(win, &m->combo[i], focused, 0);
                if (focused) oui_focus_ring(win, m->fx[i], m->fy[i], m->fw[i], m->fh[i]);
                break;
            case OM_CHECK: {
                int on = f->checked ? *f->checked : 0;
                gui_checkbox(win, m->fx[i], m->fy[i], 16, on ? true : false, f->label, GUI_ST_NORMAL);
                if (focused) oui_focus_ring(win, m->fx[i], m->fy[i], 16, 16);
                break;
            }
            case OM_RADIO:
                om_draw_radio(m, i, overlay);
                break;
            case OM_LIST: {
                int sel = f->sel ? *f->sel : -1;
                gui_list_config(&m->list[i], m->fx[i], m->fy[i], m->fw[i], m->fh[i], oui_row_h(), f->n_options);
                gui_list_draw(win, &m->list[i], sel, p->field_bg, border, gui_ensure_contrast(p->ink, p->field_bg, GUI_FLOOR_TEXT),
                              oui_tok(THEME_COLOR_SELECTION, p->accent), oui_tok(THEME_COLOR_SELECTION_TEXT, 0x00FFFFFF),
                              om_list_label, (void *)f);
                if (gui_scroll_needed(&m->list[i].scroll) && f->label) {
                    char cnt[32]; int n = oui_itoa(f->n_options, cnt, sizeof(cnt));
                    oui_copy(cnt + n, (int)sizeof(cnt) - n, " items");
                    win_draw_text_ttf(win, m->cx + OM_INSET, rowy + 4 + oui_row_h() + 4, cnt, cap, muted);
                }
                if (focused) oui_focus_ring(win, m->fx[i], m->fy[i], m->fw[i], m->fh[i]);
                break;
            }
            case OM_LABEL:
                if (f->buf && f->buf[0]) {
                    char el[128]; oui_ellipsize(el, sizeof(el), f->buf, size, m->fw[i]);
                    win_draw_text_ttf(win, m->fx[i], oui_text_y(rowy, OM_ROW_H, size), el, size, muted);
                }
                break;
        }
    }
    // Preview pane.
    if (m->spec && m->spec->preview_h > 0) {
        int px = m->cx + OM_INSET, py = m->cy + OM_BODY_Y + m->preview_y, pw = m->cw - 2 * OM_INSET, ph = m->spec->preview_h;
        win_draw_rect(win, px, py, pw, ph, oui_sunken());
        gui_draw_rect_outline(win, px, py, pw, ph, gui_ensure_contrast(oui_border_subtle(), oui_sunken(), GUI_FLOOR_NONTEXT));
        if (m->spec->preview) m->spec->preview(win, px + 1, py + 1, pw - 2, ph - 2, m->spec->ctx);
    }
    // Custom body.
    if (m->custom && m->custom->draw)
        m->custom->draw(win, m->cx + OM_INSET, m->cy + OM_BODY_Y + m->custom_y, m->cw - 2 * OM_INSET, m->custom_h, m->custom->ctx);

    // Footer band + buttons.
    int fy = m->cy + m->ch - OM_FOOTER_H;
    win_draw_rect(win, m->cx, fy, m->cw, OM_FOOTER_H, raised);
    win_draw_rect(win, m->cx, fy, m->cw, 1, gui_ensure_contrast(oui_border_subtle(), raised, GUI_FLOOR_NONTEXT));
    if (m->third_label) om_draw_button(m, m->third_x, m->third_w, m->third_label, OMB_STANDARD, OMF_THIRD, 1);
    om_draw_button(m, m->cancel_x, m->cancel_w, m->cancel_label, OMB_STANDARD, OMF_CANCEL, 1);
    om_draw_button(m, m->ok_x, m->ok_w, m->ok_label, m->ok_is_danger ? OMB_DANGER : OMB_PRIMARY, OMF_OK, m->all_valid);

    // Open combo popup LAST.
    if (m->open_combo >= 0) ocombo_draw_popup(win, &m->combo[m->open_combo], m->win_w, m->win_h);
    win_invalidate(win);
}

// --- Focus --------------------------------------------------------------------------
static int om_first_field(const om_t *m) {
    for (int i = 0; i < om_nf(m); i++) if (om_focusable(m, i)) return i;
    return OMF_NONE;
}

static void om_focus_step(om_t *m, int dir) {
    // Order: fields, third, cancel, ok.
    int order[OM_MAX_FIELDS + 3]; int n = 0;
    for (int i = 0; i < om_nf(m); i++) if (om_focusable(m, i)) order[n++] = i;
    if (m->third_label) order[n++] = OMF_THIRD;
    order[n++] = OMF_CANCEL;
    order[n++] = OMF_OK;
    int cur = -1;
    for (int k = 0; k < n; k++) if (order[k] == m->focus) { cur = k; break; }
    if (cur < 0) { m->focus = order[dir > 0 ? 0 : n - 1]; return; }
    cur += dir;
    if (cur < 0) cur = n - 1;
    if (cur >= n) cur = 0;
    m->focus = order[cur];
    // Tabbing into a text/number field selects its content, so typing
    // replaces the value instead of appending to it (measured on the
    // Paragraph modal: Tab then "12" produced "012").
    if (m->focus >= 0 && m->focus < om_nf(m)) {
        om_kind_t k = om_f(m, m->focus)->kind;
        if (k == OM_TEXT || k == OM_NUMBER) tf_select_all(&m->tf[m->focus]);
    }
}

// --- Snapshot / restore ---------------------------------------------------------------
static void om_snapshot(om_t *m) {
    for (int i = 0; i < om_nf(m); i++) {
        const om_field_t *f = om_f(m, i);
        m->snap_buf[i] = 0;
        if ((f->kind == OM_TEXT || f->kind == OM_NUMBER) && f->buf && f->cap > 0) {
            m->snap_buf[i] = (char *)malloc((size_t)f->cap);
            if (m->snap_buf[i]) oui_copy(m->snap_buf[i], f->cap, f->buf);
        }
        m->snap_sel[i] = f->sel ? *f->sel : 0;
        m->snap_checked[i] = f->checked ? *f->checked : 0;
    }
}
static void om_restore(om_t *m) {
    for (int i = 0; i < om_nf(m); i++) {
        const om_field_t *f = om_f(m, i);
        if (m->snap_buf[i] && f->buf) oui_copy(f->buf, f->cap, m->snap_buf[i]);
        if (f->sel) *f->sel = m->snap_sel[i];
        if (f->checked) *f->checked = m->snap_checked[i];
    }
}
static void om_free_snapshot(om_t *m) {
    for (int i = 0; i < om_nf(m); i++) { if (m->snap_buf[i]) free(m->snap_buf[i]); m->snap_buf[i] = 0; }
}

// --- Input -----------------------------------------------------------------------------
static int om_btn_hit(const om_t *m, int mx, int my) {
    if (my < m->btn_y || my >= m->btn_y + OM_BTN_H) return OMF_NONE;
    if (mx >= m->ok_x && mx < m->ok_x + m->ok_w) return OMF_OK;
    if (mx >= m->cancel_x && mx < m->cancel_x + m->cancel_w) return OMF_CANCEL;
    if (m->third_label && mx >= m->third_x && mx < m->third_x + m->third_w) return OMF_THIRD;
    return OMF_NONE;
}

// Caret index from a click x inside a TTF field (measured, matches the draw).
static int om_caret_from_x(const textfield_t *tf, int rel_x) {
    int size = oui_body();
    if (rel_x <= 0) return 0;
    char pre[512];
    int best = tf->len;
    for (int i = 1; i <= tf->len && i < (int)sizeof(pre) - 1; i++) {
        for (int k = 0; k < i; k++) pre[k] = tf->buf[k];
        pre[i] = 0;
        int w = gui_ttf_render_width(pre, size);
        if (w > rel_x) { best = i - 1; break; }
    }
    return best;
}

// Returns 1 when the value changed.
static int om_radio_press(om_t *m, int i, int mx, int my) {
    const om_field_t *f = om_f(m, i);
    int size = oui_body(), x = m->fx[i], cy = m->fy[i];
    for (int k = 0; k < f->n_options; k++) {
        int tw = gui_ttf_render_width(om_opt(f, k), size);
        if (oui_in(mx, my, x - 2, cy - 6, 18 + tw + 4, 25)) {
            if (f->sel && *f->sel != k) { *f->sel = k; return 1; }
            return 0;
        }
        x += 18 + tw + 16;
    }
    return 0;
}

static int om_field_hit(const om_t *m, int i, int mx, int my) {
    const om_field_t *f = om_f(m, i);
    if (f->kind == OM_CHECK) {
        int tw = f->label ? gui_ttf_render_width(f->label, oui_body()) : 0;
        return oui_in(mx, my, m->fx[i] - 2, m->fy[i] - 4, 16 + 8 + tw + 4, 24);
    }
    if (f->kind == OM_RADIO) return oui_in(mx, my, m->fx[i], m->fy[i] - 6, m->fw[i], 25);
    return oui_in(mx, my, m->fx[i], m->fy[i], m->fw[i], m->fh[i]);
}

// Result codes from the press/key handlers: 0 nothing, 1 redraw, 2 full repaint
// (the app frame underneath must be redrawn, e.g. a popup closed), 10+ done.
#define R_NONE   0
#define R_REDRAW 1
#define R_FULL   2
#define R_DONE   10   // + OM_* result

static int om_activate(om_t *m, int slot) {
    if (slot == OMF_OK)     return m->all_valid ? (R_DONE + OM_OK) : R_REDRAW;
    if (slot == OMF_CANCEL) return R_DONE + OM_CANCEL;
    if (slot == OMF_THIRD)  return R_DONE + OM_THIRD;
    return R_NONE;
}

static int om_press(om_t *m, int mx, int my) {
    if (m->open_combo >= 0) {
        int i = m->open_combo;
        int r = ocombo_press(&m->combo[i], mx, my, m->win_w, m->win_h);
        if (!m->combo[i].open) m->open_combo = -1;
        if (r >= 0) { const om_field_t *f = om_f(m, i); if (f->sel) *f->sel = r; om_changed(m); }
        return R_FULL;
    }
    int b = om_btn_hit(m, mx, my);
    if (b != OMF_NONE) { m->pressed_btn = b; m->hover_btn = b; m->focus = b; return R_REDRAW; }
    for (int i = 0; i < om_nf(m); i++) {
        if (!om_focusable(m, i) || !om_field_hit(m, i, mx, my)) continue;
        const om_field_t *f = om_f(m, i);
        m->focus = i;
        switch (f->kind) {
            case OM_TEXT: case OM_NUMBER: {
                int idx = om_caret_from_x(&m->tf[i], mx - (m->fx[i] + 8));
                tf_set_caret(&m->tf[i], idx);
                break;
            }
            case OM_CHECK:
                if (f->checked) { *f->checked = !*f->checked; om_changed(m); }
                break;
            case OM_RADIO:
                if (om_radio_press(m, i, mx, my)) om_changed(m);
                break;
            case OM_LIST: {
                int r = gui_list_press(&m->list[i], mx, my);
                if (r >= 0 && f->sel && *f->sel != r) { *f->sel = r; om_changed(m); }
                break;
            }
            case OM_COMBO:
                ocombo_press(&m->combo[i], mx, my, m->win_w, m->win_h);
                if (m->combo[i].open) m->open_combo = i;
                break;
            default: break;
        }
        return R_REDRAW;
    }
    if (m->custom && m->custom->press) {
        int bx = m->cx + OM_INSET, by = m->cy + OM_BODY_Y + m->custom_y;
        if (oui_in(mx, my, bx, by, m->cw - 2 * OM_INSET, m->custom_h)) {
            m->focus = OMF_NONE;
            if (m->custom->press(bx, by, mx, my, m->custom->ctx)) return R_REDRAW;
        }
    }
    return R_NONE;                                 // scrim / card body: consumed, nothing
}

static int om_release(om_t *m, int mx, int my) {
    if (m->open_combo >= 0) { ocombo_release(&m->combo[m->open_combo]); return R_NONE; }
    for (int i = 0; i < om_nf(m); i++) if (om_f(m, i)->kind == OM_LIST) gui_list_release(&m->list[i]);
    if (m->pressed_btn == OMF_NONE) return R_NONE;
    int b = m->pressed_btn;
    m->pressed_btn = OMF_NONE;
    if (om_btn_hit(m, mx, my) == b) return om_activate(m, b);
    return R_REDRAW;
}

static int om_motion(om_t *m, int mx, int my) {
    if (m->open_combo >= 0) return ocombo_motion(&m->combo[m->open_combo], mx, my) ? R_REDRAW : R_NONE;
    int r = R_NONE;
    int b = om_btn_hit(m, mx, my);
    if (b != m->hover_btn) { m->hover_btn = b; r = R_REDRAW; }
    for (int i = 0; i < om_nf(m); i++)
        if (om_f(m, i)->kind == OM_LIST && gui_list_motion(&m->list[i], mx, my)) r = R_REDRAW;
    return r;
}

static int om_wheel(om_t *m, int mx, int my, int delta) {
    if (m->open_combo >= 0) return ocombo_wheel(&m->combo[m->open_combo], mx, my, delta) ? R_REDRAW : R_NONE;
    for (int i = 0; i < om_nf(m); i++)
        if (om_f(m, i)->kind == OM_LIST && gui_list_wheel(&m->list[i], mx, my, delta)) return R_REDRAW;
    return R_NONE;
}

static int om_key(om_t *m, const gui_event_t *ev) {
    unsigned int kc = ev->keycode; char ch = ev->key_char;
    if (m->open_combo >= 0) {
        int i = m->open_combo;
        int r = ocombo_key(&m->combo[i], kc, ch, m->win_w, m->win_h);
        int closed = !m->combo[i].open;
        if (closed) m->open_combo = -1;
        if (r >= 0) { const om_field_t *f = om_f(m, i); if (f->sel) *f->sel = r; om_changed(m); }
        return closed ? R_FULL : R_REDRAW;
    }
    if (m->custom && m->custom->key && m->custom->key(kc, ch, m->custom->ctx)) return R_REDRAW;
    if (ch == GUI_KEY_ESC) return R_DONE + OM_CANCEL;
    if (kc == GUI_KEY_TAB || ch == GUI_KEY_TAB) { om_focus_step(m, gui_mods_is(GUI_MOD_SHIFT) ? -1 : 1); return R_REDRAW; }
    int enter = (ch == GUI_KEY_ENTER || kc == GUI_KEY_ENTER);
    if (m->focus >= OMF_THIRD) {
        if (enter || ch == ' ') return om_activate(m, m->focus);
        return R_NONE;
    }
    int i = m->focus;
    if (i >= 0 && i < om_nf(m)) {
        const om_field_t *f = om_f(m, i);
        switch (f->kind) {
            case OM_TEXT:
            case OM_NUMBER: {
                if (enter) break;
                if (f->kind == OM_NUMBER && ch >= 0x20 && ch <= 0x7E && !(ch >= '0' && ch <= '9') && ch != '-') return R_NONE;
                if (tf_handle_key(&m->tf[i], ev)) { om_changed(m); return R_REDRAW; }
                return R_NONE;
            }
            case OM_CHECK:
                if (ch == ' ') { if (f->checked) { *f->checked = !*f->checked; om_changed(m); } return R_REDRAW; }
                if (enter) break;
                return R_NONE;
            case OM_RADIO: {
                if (enter) break;
                int d = 0;
                if (kc == GUI_KEY_LEFT || kc == GUI_KEY_UP) d = -1;
                if (kc == GUI_KEY_RIGHT || kc == GUI_KEY_DOWN) d = 1;
                if (d && f->sel && f->n_options > 0) {
                    int s = *f->sel + d;
                    if (s < 0) s = 0;
                    if (s > f->n_options - 1) s = f->n_options - 1;
                    if (s != *f->sel) { *f->sel = s; om_changed(m); }
                    return R_REDRAW;
                }
                return R_NONE;
            }
            case OM_LIST: {
                if (enter) break;
                if (f->sel) {
                    int s = *f->sel;
                    int step = 0;
                    if (kc == GUI_KEY_UP) step = -1; else if (kc == GUI_KEY_DOWN) step = 1;
                    else if (kc == GUI_KEY_PGUP) step = -(f->list_rows > 0 ? f->list_rows : 6);
                    else if (kc == GUI_KEY_PGDN) step = (f->list_rows > 0 ? f->list_rows : 6);
                    else if (kc == GUI_KEY_HOME) step = -f->n_options;
                    else if (kc == GUI_KEY_END) step = f->n_options;
                    if (step) {
                        if (s < 0) s = 0;
                        gui_list_move_sel(&m->list[i], &s, step);
                        if (s != *f->sel) { *f->sel = s; om_changed(m); }
                        return R_REDRAW;
                    }
                }
                return R_NONE;
            }
            case OM_COMBO: {
                int r = ocombo_key(&m->combo[i], kc, ch, m->win_w, m->win_h);
                if (m->combo[i].open) { m->open_combo = i; return R_REDRAW; }
                if (r >= 0) { if (f->sel) *f->sel = r; om_changed(m); return R_REDRAW; }
                if (r == OCOMBO_TEXT) { if (f->sel) *f->sel = -1; om_changed(m); return R_REDRAW; }
                if (r == -2) return R_REDRAW;
                if (enter) break;
                return R_NONE;
            }
            default: break;
        }
    }
    if (enter) return om_activate(m, OMF_OK);
    return R_NONE;
}

// --- The nested loop --------------------------------------------------------------------
static int om_loop(om_t *m) {
    office_app *a = m->app;
    oapp_size(a, &m->win_w, &m->win_h);
    om_layout(m);
    om_validate(m);
    m->hover_btn = m->pressed_btn = OMF_NONE;
    m->open_combo = -1;
    m->shown_ms = uptime_ms();
    oapp_repaint(a);
    om_draw(m);
    int result = OM_CANCEL, done = 0;
    while (!done) {
        gui_event_t ev;
        int et = gui_mods_next_event(m->win, &ev, -1);   // blocks on the window wait-queue (#426)
        if (et <= 0) continue;
        int settled = (uptime_ms() - m->shown_ms) >= OM_SETTLE_MS;
        int r = R_NONE;
        switch (ev.type) {
            case EVENT_WINDOW_CLOSE: done = 1; result = OM_CANCEL; break;
            case EVENT_RESIZE:
                if (ev.mouse_x > 0 && ev.mouse_y > 0) {
                    oapp__note_resize(a, ev.mouse_x, ev.mouse_y);
                    oapp_size(a, &m->win_w, &m->win_h);
                    om_layout(m);
                    r = R_FULL;
                }
                break;
            case EVENT_REDRAW:      r = R_FULL; break;
            case EVENT_KEY_DOWN:    if (settled) r = om_key(m, &ev); break;
            case EVENT_MOUSE_DOWN:  if (settled) r = om_press(m, ev.mouse_x, ev.mouse_y); break;
            case EVENT_MOUSE_UP:    r = om_release(m, ev.mouse_x, ev.mouse_y); break;
            case EVENT_MOUSE_MOVE:  r = om_motion(m, ev.mouse_x, ev.mouse_y); break;
            case EVENT_MOUSE_SCROLL: r = om_wheel(m, ev.mouse_x, ev.mouse_y, ev.scroll_delta); break;
            default: break;
        }
        if (r >= R_DONE) { result = r - R_DONE; done = 1; break; }
        if (r == R_FULL) { oapp_repaint(a); om_draw(m); }
        else if (r == R_REDRAW) om_draw(m);
    }
    oapp_repaint(a);
    return result;
}

static void om_common_init(om_t *m, office_app *a, const char *title, int size,
                           const char *ok, const char *cancel, const char *third, int danger) {
    memset(m, 0, sizeof(*m));
    m->app = a; m->win = oapp_win(a);
    m->title = title ? title : "";
    m->size = (size == OM_SIZE_S || size == OM_SIZE_M || size == OM_SIZE_L) ? size : (size > 200 ? size : OM_SIZE_M);
    m->ok_label = ok ? ok : "OK";
    m->cancel_label = cancel ? cancel : "Cancel";
    m->third_label = third;
    m->ok_is_danger = danger;
    m->focus = OMF_NONE;
}

int omodal_run(office_app *a, const om_spec_t *spec) {
    if (!a || !spec) return OM_CANCEL;
    om_t *m = (om_t *)malloc(sizeof(om_t));
    if (!m) return OM_CANCEL;
    om_common_init(m, a, spec->title, spec->size, spec->ok_label, spec->cancel_label, spec->third_label, spec->ok_is_danger);
    m->spec = spec;
    if (spec->n_fields > OM_MAX_FIELDS) { free(m); return OM_CANCEL; }
    for (int i = 0; i < spec->n_fields; i++) {
        const om_field_t *f = &spec->fields[i];
        switch (f->kind) {
            case OM_TEXT: case OM_NUMBER:
                if (f->buf && f->cap > 0) tf_init(&m->tf[i], f->buf, f->cap);
                else { static char dummy[2]; tf_init(&m->tf[i], dummy, 2); }
                break;
            case OM_COMBO:
                g_om_opts[i].opts = f->options; g_om_opts[i].n = f->n_options;
                ocombo_init(&m->combo[i], 0, 0, 100, f->n_options, f->sel ? *f->sel : -1, 0, om_opt_label, &g_om_opts[i]);
                break;
            default: break;
        }
    }
    om_snapshot(m);
    m->focus = om_first_field(m);
    if (m->focus == OMF_NONE) m->focus = spec->ok_is_danger ? OMF_CANCEL : OMF_OK;
    int r = om_loop(m);
    if (r == OM_CANCEL) om_restore(m);
    om_free_snapshot(m);
    free(m);
    return r;
}

int omodal_run_custom(office_app *a, const char *title, int size, int body_h,
                      const char *ok_label, const char *cancel_label, const om_custom_t *body) {
    if (!a || !body) return OM_CANCEL;
    om_t *m = (om_t *)malloc(sizeof(om_t));
    if (!m) return OM_CANCEL;
    om_common_init(m, a, title, size, ok_label, cancel_label, 0, 0);
    m->custom = body;
    m->custom_h = body_h > 0 ? body_h : 120;
    m->focus = OMF_OK;
    int r = om_loop(m);
    free(m);
    return r;
}

int omodal_confirm3(office_app *a, const char *title, const char *body,
                    const char *save_label, const char *discard_label) {
    if (!a) return OM_CANCEL;
    om_t *m = (om_t *)malloc(sizeof(om_t));
    if (!m) return OM_CANCEL;
    om_common_init(m, a, title, OM_SIZE_S, discard_label ? discard_label : "Discard", "Cancel",
                   save_label ? save_label : "Save", 1);
    m->n_lines = gui_wrap_text_ttf(body ? body : "", oui_body(), OM_SIZE_S - 2 * OM_INSET, 3, m->lines);
    if (m->n_lines < 1) { m->n_lines = 1; m->lines[0][0] = 0; }
    m->focus = OMF_CANCEL;                           // a bare Enter never discards
    int r = om_loop(m);
    free(m);
    return r;
}
