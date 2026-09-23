// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// ocombo.c - the office dropdown combo (see ocombo.h).
#include "ocombo.h"
#include "oui.h"
#include "../../libc/gui_menu.h"     // gui_menu_palette_theme: the repaired popup inks
#include "../../libc/string.h"

#define OC_CHEV_CAP 18
#define OC_FOOTER_H 18

static const char *oc_label(const ocombo_t *c, int i, char *buf, int cap) {
    if (!c->label || i < 0 || i >= c->count) { buf[0] = 0; return buf; }
    const char *s = c->label(c->ctx, i, buf, cap);
    return s ? s : "";
}

static void oc_sync_text(ocombo_t *c) {
    char buf[96];
    if (c->sel >= 0 && c->sel < c->count) oui_copy(c->text, sizeof(c->text), oc_label(c, c->sel, buf, sizeof(buf)));
    else if (!c->editable) c->text[0] = 0;
    c->typed = 0;
}

void ocombo_init(ocombo_t *c, int x, int y, int w, int count, int sel, int editable,
                 ocombo_label_fn label, void *ctx) {
    memset(c, 0, sizeof(*c));
    c->x = x; c->y = y; c->w = w; c->h = oui_input_h();
    c->count = count < 0 ? 0 : count;
    c->sel = (sel >= 0 && sel < c->count) ? sel : -1;
    c->hot = -1;
    c->editable = editable;
    c->label = label; c->ctx = ctx;
    oc_sync_text(c);
}

void ocombo_place(ocombo_t *c, int x, int y, int w) {
    c->x = x; c->y = y; c->w = w; c->h = oui_input_h();
}

void ocombo_set_sel(ocombo_t *c, int sel) {
    c->sel = (sel >= 0 && sel < c->count) ? sel : -1;
    oc_sync_text(c);
}

void ocombo_set_text(ocombo_t *c, const char *text) {
    oui_copy(c->text, sizeof(c->text), text);
    c->typed = 0;
}

void ocombo_set_count(ocombo_t *c, int count) {
    c->count = count < 0 ? 0 : count;
    if (c->sel >= c->count) c->sel = -1;
    if (c->hot >= c->count) c->hot = c->count - 1;
}

int ocombo_hit(const ocombo_t *c, int mx, int my) {
    return oui_in(mx, my, c->x, c->y, c->w, c->h);
}

void ocombo_draw(int win, const ocombo_t *c) { ocombo_draw_ex(win, c, 0, 0); }

void ocombo_draw_ex(int win, const ocombo_t *c, int focused, int disabled) {
    int size = oui_body();
    gui_palette_t *p = gui_pal();
    gui_textfield2(win, c->x, c->y, c->w, c->h, 0, focused ? true : false);
    // Text, ellipsized to the box minus the chevron cap.
    int text_w = c->w - 6 - OC_CHEV_CAP - 4;
    if (text_w > 0 && c->text[0]) {
        char buf[64];
        oui_ellipsize(buf, sizeof(buf), c->text, size, text_w);
        uint32_t ink = disabled ? gui_ensure_contrast(gui_mix(p->ink, p->field_bg, 120), p->field_bg, GUI_FLOOR_NONTEXT)
                                : gui_ensure_contrast(p->ink, p->field_bg, GUI_FLOOR_TEXT);
        win_draw_text_ttf(win, c->x + 6, oui_text_y(c->y, c->h, size), buf, size, ink);
    }
    // Chevron in an 18 px cap at the right, non-text floor against the field.
    uint32_t chev = gui_ensure_contrast(p->ink, p->field_bg, GUI_FLOOR_NONTEXT);
    if (disabled) chev = gui_ensure_contrast(gui_mix(p->ink, p->field_bg, 120), p->field_bg, GUI_FLOOR_NONTEXT);
    gui_chevron(win, c->x + c->w - OC_CHEV_CAP / 2 - 1, c->y + c->h / 2, GUI_CHEV_DOWN, chev);
}

// Popup geometry: below the box, or above when it would leave the window.
static void oc_geom(ocombo_t *c, int win_w, int win_h) {
    int rows = c->count < OCOMBO_MAX_ROWS ? c->count : OCOMBO_MAX_ROWS;
    if (rows < 1) rows = 1;
    int row_h = oui_row_h();
    c->footer_h = (c->count > OCOMBO_MAX_ROWS) ? OC_FOOTER_H : 0;
    int list_h = rows * row_h + 2;
    c->pw = c->w < OCOMBO_MIN_POPW ? OCOMBO_MIN_POPW : c->w;
    c->ph = list_h + c->footer_h;
    c->px = c->x;
    if (c->px + c->pw > win_w) c->px = win_w - c->pw;
    if (c->px < 0) c->px = 0;
    c->py = c->y + c->h + 1;
    if (c->py + c->ph > win_h && c->y - 1 - c->ph >= 0) c->py = c->y - 1 - c->ph;
    gui_list_config(&c->list, c->px, c->py, c->pw, list_h, row_h, c->count);
    c->list.scroll.snap = 1;
}

void ocombo_open(ocombo_t *c, int win_w, int win_h) {
    if (c->open) return;
    c->open = 1;
    c->hot = (c->sel >= 0) ? c->sel : (c->count > 0 ? 0 : -1);
    memset(&c->list, 0, sizeof(c->list));
    oc_geom(c, win_w, win_h);
    if (c->hot >= 0) gui_scroll_reveal(&c->list.scroll, c->hot * c->list.row_h, c->list.row_h);
}

void ocombo_close(ocombo_t *c) {
    c->open = 0;
    c->hot = -1;
}

void ocombo_draw_popup(int win, ocombo_t *c, int win_w, int win_h) {
    if (!c->open) return;
    oc_geom(c, win_w, win_h);
    gui_menu_palette_t pal; gui_menu_palette_theme(&pal, -1);
    int size = oui_body();
    // Ground + border of the whole popup (list + footer).
    win_draw_rect(win, c->px, c->py, c->pw, c->ph, pal.popup_bg);
    gui_draw_rect_outline(win, c->px, c->py, c->pw, c->ph, pal.popup_border);
    int first = gui_list_first(&c->list), span = gui_list_span(&c->list);
    int rw = gui_list_row_w(&c->list);
    for (int r = 0; r < span; r++) {
        int idx = first + r;
        if (idx >= c->count) break;
        int ry;
        if (!gui_list_row_y(&c->list, idx, &ry)) continue;
        char buf[96];
        const char *lbl = oc_label(c, idx, buf, sizeof(buf));
        uint32_t ink = pal.item_text;
        if (idx == c->hot) {
            win_draw_rect(win, c->px + 1, ry, rw, c->list.row_h, pal.item_hover_bg);
            ink = pal.item_hover_text;
        }
        if (idx == c->sel) {
            // Check mark in the left gutter (the current value row).
            int cx = c->px + 8, cy = ry + c->list.row_h / 2;
            gui_thick_line(win, cx, cy, cx + 3, cy + 3, 2, ink);
            gui_thick_line(win, cx + 3, cy + 3, cx + 9, cy - 4, 2, ink);
        }
        char el[96];
        oui_ellipsize(el, sizeof(el), lbl, size, rw - 24 - 6);
        win_draw_text_ttf(win, c->px + 24, oui_text_y(ry, c->list.row_h, size), el, size, ink);
    }
    gui_scroll_draw_on(win, &c->list.scroll, pal.popup_bg);
    if (c->footer_h) {
        char cnt[32]; int n = oui_itoa(c->count, cnt, sizeof(cnt));
        oui_copy(cnt + n, (int)sizeof(cnt) - n, " items");
        int cap = oui_caption();
        int fy = c->py + c->ph - c->footer_h;
        win_draw_rect(win, c->px + 1, fy, c->pw - 2, 1, pal.separator);
        uint32_t ink = gui_ensure_contrast(pal.shortcut_text, pal.popup_bg, GUI_FLOOR_TEXT);
        int tw = gui_ttf_render_width(cnt, cap);
        win_draw_text_ttf(win, c->px + c->pw - 8 - tw, oui_text_y(fy, c->footer_h, cap), cnt, cap, ink);
    }
}

static int oc_commit(ocombo_t *c, int idx) {
    if (idx < 0 || idx >= c->count) { ocombo_close(c); return -2; }
    c->sel = idx;
    oc_sync_text(c);
    ocombo_close(c);
    return idx;
}

int ocombo_press(ocombo_t *c, int mx, int my, int win_w, int win_h) {
    if (c->open) {
        oc_geom(c, win_w, win_h);
        if (gui_list_hit(&c->list, mx, my)) {
            int r = gui_list_press(&c->list, mx, my);
            if (r >= 0) return oc_commit(c, r);
            return -2;                             // scrollbar took it
        }
        // Click on the footer, on the closed box, or anywhere else: close
        // without changing the value; consumed either way (gui_menu rule).
        ocombo_close(c);
        return -2;
    }
    if (ocombo_hit(c, mx, my)) { ocombo_open(c, win_w, win_h); return -2; }
    return -1;
}

int ocombo_motion(ocombo_t *c, int mx, int my) {
    if (!c->open) return 0;
    int changed = gui_list_motion(&c->list, mx, my);
    int r = gui_list_row_at(&c->list, mx, my);
    if (r >= 0 && r != c->hot) { c->hot = r; changed = 1; }
    return changed;
}

int ocombo_wheel(ocombo_t *c, int mx, int my, int delta) {
    if (!c->open) return 0;
    return gui_list_wheel(&c->list, mx, my, delta);
}

void ocombo_release(ocombo_t *c) {
    if (c->open) gui_list_release(&c->list);
}

static void oc_move_hot(ocombo_t *c, int delta) {
    if (c->count <= 0) return;
    int h = c->hot < 0 ? 0 : c->hot;
    h += delta;
    if (h < 0) h = 0;
    if (h > c->count - 1) h = c->count - 1;
    c->hot = h;
    gui_scroll_reveal(&c->list.scroll, h * c->list.row_h, c->list.row_h);
}

// First row (after `from`, wrapping) whose label starts with `ch`.
static int oc_jump(const ocombo_t *c, int from, char ch) {
    if (c->count <= 0) return -1;
    char lo = ch; if (lo >= 'A' && lo <= 'Z') lo = (char)(lo + 32);
    for (int k = 1; k <= c->count; k++) {
        int i = (from + k) % c->count;
        char buf[96];
        const char *l = oc_label(c, i, buf, sizeof(buf));
        char f = l[0]; if (f >= 'A' && f <= 'Z') f = (char)(f + 32);
        if (f == lo) return i;
    }
    return -1;
}

// Row whose label equals `text` exactly (case-insensitive), or -1.
static int oc_find_text(const ocombo_t *c, const char *text) {
    for (int i = 0; i < c->count; i++) {
        char buf[96];
        const char *l = oc_label(c, i, buf, sizeof(buf));
        if (strcasecmp(l, text) == 0) return i;
    }
    return -1;
}

static int oc_edit_text(ocombo_t *c, char ch) {
    int n = (int)strlen(c->text);
    if (ch == (char)0x08) {                       // Backspace
        if (n > 0) { c->text[n - 1] = 0; c->typed = 1; return 1; }
        return 0;
    }
    if (ch >= 0x20 && ch <= 0x7E && n < OCOMBO_TEXT_CAP - 1) {
        if (!c->typed) { n = 0; }                 // first keystroke replaces the label
        c->text[n] = ch; c->text[n + 1] = 0;
        c->typed = 1;
        return 1;
    }
    return 0;
}

int ocombo_key(ocombo_t *c, unsigned int keycode, char key_char, int win_w, int win_h) {
    if (c->open) {
        if (key_char == GUI_KEY_ESC) { c->typed = 0; oc_sync_text(c); ocombo_close(c); return -2; }
        if (key_char == GUI_KEY_ENTER || keycode == GUI_KEY_ENTER) {
            if (c->editable && c->typed) {
                int i = oc_find_text(c, c->text);
                if (i >= 0) return oc_commit(c, i);
                c->sel = -1; c->typed = 0; ocombo_close(c);
                return OCOMBO_TEXT;
            }
            return oc_commit(c, c->hot);
        }
        switch (keycode) {
            case GUI_KEY_UP:   oc_move_hot(c, -1); return -2;
            case GUI_KEY_DOWN: oc_move_hot(c, 1);  return -2;
            case GUI_KEY_PGUP: oc_move_hot(c, -OCOMBO_MAX_ROWS); return -2;
            case GUI_KEY_PGDN: oc_move_hot(c, OCOMBO_MAX_ROWS);  return -2;
            case GUI_KEY_HOME: oc_move_hot(c, -c->count); return -2;
            case GUI_KEY_END:  oc_move_hot(c, c->count);  return -2;
            default: break;
        }
        if (c->editable) {
            if (oc_edit_text(c, key_char)) {
                int i = oc_find_text(c, c->text);
                if (i < 0) i = oc_jump(c, -1, c->text[0]);
                if (i >= 0) { c->hot = i; gui_scroll_reveal(&c->list.scroll, i * c->list.row_h, c->list.row_h); }
                return -2;
            }
        } else if (key_char > 0x20 && key_char <= 0x7E) {
            int i = oc_jump(c, c->hot, key_char);
            if (i >= 0) { c->hot = i; gui_scroll_reveal(&c->list.scroll, i * c->list.row_h, c->list.row_h); }
            return -2;
        }
        return -2;                                // open popup swallows everything else
    }
    // Closed, focused (modal field): step, open, or edit.
    if (keycode == GUI_KEY_UP || keycode == GUI_KEY_DOWN) {
        if (c->count <= 0) return -2;
        int s = c->sel < 0 ? 0 : c->sel + (keycode == GUI_KEY_DOWN ? 1 : -1);
        if (s < 0) s = 0;
        if (s > c->count - 1) s = c->count - 1;
        c->sel = s; oc_sync_text(c);
        return s;
    }
    if (key_char == ' ' && !c->editable) { ocombo_open(c, win_w, win_h); return -2; }
    if (c->editable) {
        if (key_char == GUI_KEY_ENTER || keycode == GUI_KEY_ENTER) {
            if (!c->typed) return -1;             // nothing typed: let the modal take Enter
            int i = oc_find_text(c, c->text);
            if (i >= 0) { c->sel = i; oc_sync_text(c); return i; }
            c->sel = -1; c->typed = 0;
            return OCOMBO_TEXT;
        }
        if (oc_edit_text(c, key_char)) return -2;
    }
    return -1;
}
