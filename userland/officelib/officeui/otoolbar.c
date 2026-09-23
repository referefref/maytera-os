// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// otoolbar.c - the office toolbar (see otoolbar.h).
#include "otoolbar.h"
#include "oicon.h"
#include "oui.h"
#include "../../libc/gui_menu.h"
#include "../../libc/string.h"

#define OTB_INSET     8
#define OTB_GAP       2
#define OTB_COMBO_GAP 4
#define OTB_SEP_W     17
#define OTB_DEF_COMBO 120
#define OTB_DEF_SPACE 8
#define OTB_MORE_ROWS 10

int otb_height(void) { return oui_btn_h() + 16; }

static int otb_item_natural_w(const otb_item_t *it) {
    switch (it->kind) {
        case OTB_SEPARATOR: return OTB_SEP_W;
        case OTB_COMBO:     return it->w > 0 ? it->w : OTB_DEF_COMBO;
        case OTB_SPACER:    return it->w > 0 ? it->w : OTB_DEF_SPACE;
        default:            return OTB_BTN_W;
    }
}

// Gap that precedes item i (0 for the first item; a separator carries its own
// 8+8 clearance; a combo gets 4 px of clear).
static int otb_gap_before(const otb_toolbar_t *tb, int i) {
    if (i <= 0) return 0;
    const otb_item_t *cur = &tb->items[i], *prev = &tb->items[i - 1];
    if (cur->kind == OTB_SEPARATOR || prev->kind == OTB_SEPARATOR) return 0;
    if (cur->kind == OTB_COMBO || prev->kind == OTB_COMBO) return OTB_COMBO_GAP;
    return OTB_GAP;
}

void otb_layout(otb_toolbar_t *tb, int w) {
    tb->w = w;
    tb->h = otb_height();
    int avail = w - 2 * OTB_INSET;
    // Natural total.
    int total = 0;
    for (int i = 0; i < tb->n; i++) total += otb_gap_before(tb, i) + otb_item_natural_w(&tb->items[i]);
    tb->overflow_from = -1;
    tb->more_x = -1;
    int limit = avail;
    if (total > avail) {
        // Hide whole groups from the right end; a MORE button (26 + 8 clear)
        // takes the right end.
        limit = avail - (OTB_BTN_W + OTB_INSET);
        int acc = 0, keep_until = 0;      // keep_until = number of leading items that fit
        int i = 0;
        while (i < tb->n) {
            // A group = items up to (not including) the next separator, plus
            // the separator that precedes it (already counted in `acc`).
            int j = i, gw = 0;
            while (j < tb->n && tb->items[j].kind != OTB_SEPARATOR) { gw += otb_gap_before(tb, j) + otb_item_natural_w(&tb->items[j]); j++; }
            if (acc + gw > limit) break;
            acc += gw;
            keep_until = j;
            if (j < tb->n) {                                   // the separator after this group
                int sw = otb_gap_before(tb, j) + otb_item_natural_w(&tb->items[j]);
                if (acc + sw > limit) { keep_until = j; break; }
                acc += sw;
                keep_until = j + 1;
            }
            i = j + 1;
        }
        // Do not leave a dangling separator as the last visible item.
        while (keep_until > 0 && tb->items[keep_until - 1].kind == OTB_SEPARATOR) keep_until--;
        tb->overflow_from = keep_until;
        tb->more_x = tb->x + w - OTB_INSET - OTB_BTN_W;
    }
    int cx = tb->x + OTB_INSET;
    for (int i = 0; i < tb->n; i++) {
        int vis = (tb->overflow_from < 0) || (i < tb->overflow_from);
        tb->visible[i] = vis;
        int iw = otb_item_natural_w(&tb->items[i]);
        if (!vis) { tb->item_x[i] = -1; tb->item_w[i] = iw; continue; }
        cx += otb_gap_before(tb, i);
        tb->item_x[i] = cx;
        tb->item_w[i] = iw;
        if (tb->items[i].kind == OTB_COMBO && tb->combo_slot[i] >= 0) {
            ocombo_t *c = &tb->combos[tb->combo_slot[i]];
            int ih = oui_input_h();
            ocombo_place(c, cx, tb->y + (tb->h - ih) / 2, iw);
        }
        cx += iw;
    }
    if (tb->hot >= tb->n) tb->hot = -1;
    if (tb->pressed >= tb->n) tb->pressed = -1;
}

void otb_init(otb_toolbar_t *tb, const otb_item_t *items, int n, int x, int y, int w) {
    memset(tb, 0, sizeof(*tb));
    if (n > OTB_MAX_ITEMS) n = OTB_MAX_ITEMS;
    tb->items = items; tb->n = n;
    tb->x = x; tb->y = y;
    tb->hot = tb->pressed = -1;
    tb->more_hot = tb->more_pressed = 0;
    tb->more_row_hot = -1;
    tb->tooltip_item = -1;
    tb->focus = -1;
    tb->open_combo = -1;
    tb->last_value = 0;
    for (int i = 0; i < n; i++) {
        tb->enabled[i] = 1;
        tb->checked[i] = 0;
        tb->combo_slot[i] = -1;
        if (items[i].kind == OTB_COMBO && tb->ncombos < OTB_MAX_COMBOS) {
            tb->combo_slot[i] = tb->ncombos;
            ocombo_init(&tb->combos[tb->ncombos], 0, 0, otb_item_natural_w(&items[i]), 0, -1, 0, 0, 0);
            tb->ncombos++;
        }
    }
    otb_layout(tb, w);
}

void otb_move(otb_toolbar_t *tb, int x, int y, int w) {
    tb->x = x; tb->y = y;
    otb_layout(tb, w);
}

static int otb_index_of(const otb_toolbar_t *tb, int id) {
    for (int i = 0; i < tb->n; i++) if (tb->items[i].id == id && tb->items[i].kind != OTB_SEPARATOR) return i;
    return -1;
}

void otb_set_enabled(otb_toolbar_t *tb, int id, int enabled) {
    int i = otb_index_of(tb, id);
    if (i >= 0) tb->enabled[i] = enabled ? 1 : 0;
}

static void otb_check_index(otb_toolbar_t *tb, int i, int checked) {
    const otb_item_t *it = &tb->items[i];
    if (checked && it->group > 0) {
        for (int k = 0; k < tb->n; k++)
            if (k != i && tb->items[k].kind == OTB_TOGGLE && tb->items[k].group == it->group) tb->checked[k] = 0;
    }
    tb->checked[i] = checked ? 1 : 0;
}

void otb_set_checked(otb_toolbar_t *tb, int id, int checked) {
    int i = otb_index_of(tb, id);
    if (i >= 0) otb_check_index(tb, i, checked);
}

int otb_is_checked(const otb_toolbar_t *tb, int id) {
    int i = otb_index_of(tb, id);
    return i >= 0 ? tb->checked[i] : 0;
}

ocombo_t *otb_combo(otb_toolbar_t *tb, int id) {
    int i = otb_index_of(tb, id);
    if (i < 0 || tb->combo_slot[i] < 0) return 0;
    return &tb->combos[tb->combo_slot[i]];
}

void otb_set_combo_text(otb_toolbar_t *tb, int id, const char *text) {
    ocombo_t *c = otb_combo(tb, id);
    if (c) ocombo_set_text(c, text);
}

int otb_item_rect(const otb_toolbar_t *tb, int id, int *x, int *y, int *w, int *h) {
    int i = otb_index_of(tb, id);
    if (i < 0 || !tb->visible[i]) return 0;
    int bh = oui_btn_h();
    if (x) *x = tb->item_x[i];
    if (y) *y = tb->y + 8;
    if (w) *w = tb->item_w[i];
    if (h) *h = bh;
    return 1;
}

// The face colour gui_button() paints for a SECONDARY button in `st`, so the
// glyph can be composited over it (no framebuffer read-back exists).
static uint32_t otb_face_bg(gui_state_t st) {
    gui_palette_t *p = gui_pal();
    uint32_t base = gui_mix(p->surface_raised, p->ink, 8);
    if (st == GUI_ST_DISABLED) return gui_mix(base, p->surface, 150);
    if (st == GUI_ST_HOVER)    return gui_lighten(base, 18);
    if (st == GUI_ST_PRESSED)  return gui_darken(base, 18);
    return base;
}

static int otb_hit_item(const otb_toolbar_t *tb, int mx, int my) {
    if (!oui_in(mx, my, tb->x, tb->y, tb->w, tb->h)) return -1;
    int bh = oui_btn_h(), by = tb->y + 8;
    for (int i = 0; i < tb->n; i++) {
        if (!tb->visible[i]) continue;
        const otb_item_t *it = &tb->items[i];
        if (it->kind == OTB_SEPARATOR || it->kind == OTB_SPACER) continue;
        if (oui_in(mx, my, tb->item_x[i], by, tb->item_w[i], bh)) return i;
    }
    return -1;
}

static int otb_hit_more(const otb_toolbar_t *tb, int mx, int my) {
    if (tb->more_x < 0) return 0;
    return oui_in(mx, my, tb->more_x, tb->y + 8, OTB_BTN_W, oui_btn_h());
}

static void otb_draw_icon_button(int win, const otb_toolbar_t *tb, int i, int x, int y, int bh,
                                 const char *icon, const char *tooltip, gui_state_t st, int checked, int focused) {
    (void)tb; (void)i;
    gui_button(win, x, y, OTB_BTN_W, bh, "", GUI_BTN_SECONDARY, st);
    uint32_t face = otb_face_bg(st);
    uint32_t ink = (st == GUI_ST_DISABLED) ? oui_tok(THEME_COLOR_BUTTON_DISABLED, 0x00808080) : oui_on_surface();
    ink = oui_glyph_on(ink, face);
    int gx = x + (OTB_BTN_W - OTB_GLYPH) / 2, gy = y + (bh - OTB_GLYPH) / 2;
    if (!icon || !oicon_draw(win, icon, gx, gy, OTB_GLYPH, ink, face)) {
        // Missing icon fallback: first letter of the tooltip, body bold, centred.
        char l[2] = { (tooltip && tooltip[0]) ? tooltip[0] : '?', 0 };
        oui_text_bold_centered(win, x, y, OTB_BTN_W, bh, l, oui_body(), ink);
    }
    if (checked) {
        // 2 px accent bar along the bottom inside edge (x+3 .. x+w-3), so a
        // checked toggle differs from a momentary press on every theme.
        uint32_t acc = gui_ensure_contrast(oui_accent(), face, GUI_FLOOR_NONTEXT);
        win_draw_rect(win, x + 3, y + bh - 4, OTB_BTN_W - 6, 2, acc);
    }
    if (focused) oui_focus_ring(win, x, y, OTB_BTN_W, bh);
}

void otb_draw(int win, otb_toolbar_t *tb) {
    uint32_t ground = oui_raised();
    uint32_t rule   = gui_ensure_contrast(oui_border_subtle(), ground, GUI_FLOOR_NONTEXT);
    int bh = oui_btn_h(), by = tb->y + 8;
    win_draw_rect(win, tb->x, tb->y, tb->w, tb->h, ground);
    win_draw_rect(win, tb->x, tb->y + tb->h - 1, tb->w, 1, rule);
    for (int i = 0; i < tb->n; i++) {
        if (!tb->visible[i]) continue;
        const otb_item_t *it = &tb->items[i];
        int x = tb->item_x[i];
        switch (it->kind) {
            case OTB_SEPARATOR:
                win_draw_rect(win, x + 8, by + 2, 1, bh - 4, rule);
                break;
            case OTB_SPACER:
                break;
            case OTB_COMBO:
                if (tb->combo_slot[i] >= 0)
                    ocombo_draw_ex(win, &tb->combos[tb->combo_slot[i]], tb->focus == i, !tb->enabled[i]);
                break;
            default: {
                gui_state_t st = GUI_ST_NORMAL;
                int momentary = (tb->pressed == i && tb->hot == i) || (tb->focus == i && tb->focus_pressed);
                if (!tb->enabled[i]) st = GUI_ST_DISABLED;
                else if (momentary || tb->checked[i]) st = GUI_ST_PRESSED;
                else if (tb->hot == i) st = GUI_ST_HOVER;
                otb_draw_icon_button(win, tb, i, x, by, bh, it->icon, it->tooltip, st,
                                     tb->checked[i] && tb->enabled[i], tb->focus == i);
                break;
            }
        }
    }
    if (tb->more_x >= 0) {
        gui_state_t st = (tb->more_pressed && tb->more_hot) || tb->more_open ? GUI_ST_PRESSED
                       : tb->more_hot ? GUI_ST_HOVER : GUI_ST_NORMAL;
        otb_draw_icon_button(win, tb, -1, tb->more_x, by, bh, "MORE", "More", st, 0, 0);
    }
}

// --- MORE popup -------------------------------------------------------------
static void otb_more_build(otb_toolbar_t *tb, int win_w, int win_h) {
    tb->more_n = 0;
    if (tb->overflow_from < 0) return;
    for (int i = tb->overflow_from; i < tb->n; i++) {
        const otb_item_t *it = &tb->items[i];
        if (it->kind == OTB_SEPARATOR || it->kind == OTB_SPACER) continue;
        tb->more_map[tb->more_n++] = i;
    }
    int rows = tb->more_n < OTB_MORE_ROWS ? tb->more_n : OTB_MORE_ROWS;
    if (rows < 1) rows = 1;
    int row_h = oui_row_h();
    int pw = 200, ph = rows * row_h + 2;
    int px = tb->more_x + OTB_BTN_W - pw, py = tb->y + tb->h;
    if (px < 0) px = 0;
    if (px + pw > win_w) px = win_w - pw;
    if (py + ph > win_h) py = win_h - ph;
    gui_list_config(&tb->more_list, px, py, pw, ph, row_h, tb->more_n);
    tb->more_list.scroll.snap = 1;
}

static const char *otb_more_label(void *ctx, int index, char *buf, int cap) {
    otb_toolbar_t *tb = (otb_toolbar_t *)ctx;
    if (index < 0 || index >= tb->more_n) return "";
    int i = tb->more_map[index];
    const otb_item_t *it = &tb->items[i];
    if (it->kind == OTB_COMBO) {
        const ocombo_t *c = tb->combo_slot[i] >= 0 ? &tb->combos[tb->combo_slot[i]] : 0;
        int n = 0;
        const char *t = it->tooltip ? it->tooltip : "Combo";
        for (; t[n] && n < cap - 3; n++) buf[n] = t[n];
        buf[n++] = ':'; buf[n++] = ' ';
        const char *v = (c && c->text[0]) ? c->text : "";
        for (int k = 0; v[k] && n < cap - 1; k++) buf[n++] = v[k];
        buf[n] = 0;
        return buf;
    }
    return it->tooltip ? it->tooltip : "";
}

static void otb_more_draw(int win, otb_toolbar_t *tb, int win_w, int win_h) {
    if (!tb->more_open) return;
    otb_more_build(tb, win_w, win_h);
    gui_menu_palette_t pal; gui_menu_palette_theme(&pal, -1);
    int size = oui_body();
    gui_list_t *l = &tb->more_list;
    win_draw_rect(win, l->x, l->y, l->w, l->h, pal.popup_bg);
    gui_draw_rect_outline(win, l->x, l->y, l->w, l->h, pal.popup_border);
    int first = gui_list_first(l), span = gui_list_span(l), rw = gui_list_row_w(l);
    for (int r = 0; r < span; r++) {
        int idx = first + r;
        if (idx >= tb->more_n) break;
        int ry;
        if (!gui_list_row_y(l, idx, &ry)) continue;
        int i = tb->more_map[idx];
        char buf[96];
        const char *lbl = otb_more_label(tb, idx, buf, sizeof(buf));
        uint32_t ink = tb->enabled[i] ? pal.item_text : pal.item_text_disabled;
        if (idx == tb->more_row_hot) {
            win_draw_rect(win, l->x + 1, ry, rw, l->row_h, pal.item_hover_bg);
            ink = tb->enabled[i] ? pal.item_hover_text : pal.item_text_disabled;
        }
        if (tb->checked[i]) {
            int cx = l->x + 8, cy = ry + l->row_h / 2;
            gui_thick_line(win, cx, cy, cx + 3, cy + 3, 2, ink);
            gui_thick_line(win, cx + 3, cy + 3, cx + 9, cy - 4, 2, ink);
        }
        char el[96];
        oui_ellipsize(el, sizeof(el), lbl, size, rw - 24 - 6);
        win_draw_text_ttf(win, l->x + 24, oui_text_y(ry, l->row_h, size), el, size, ink);
    }
    gui_scroll_draw_on(win, &l->scroll, pal.popup_bg);
}

// --- Tooltip ----------------------------------------------------------------
static void otb_tooltip_draw(int win, const otb_toolbar_t *tb, int win_w, int win_h) {
    if (!tb->tooltip_shown || tb->tooltip_item < 0 || tb->tooltip_item >= tb->n) return;
    const otb_item_t *it = &tb->items[tb->tooltip_item];
    if (!it->tooltip || !tb->visible[tb->tooltip_item]) return;
    int cap = oui_caption();
    uint32_t bg  = oui_tok(THEME_COLOR_TOOLTIP_BG, 0x00FFFFE0);
    uint32_t bd  = gui_ensure_contrast(oui_tok(THEME_COLOR_TOOLTIP_BORDER, 0x00404040), bg, GUI_FLOOR_NONTEXT);
    uint32_t ink = gui_ensure_contrast(oui_tok(THEME_COLOR_TOOLTIP_TEXT, 0x00000000), bg, GUI_FLOOR_TEXT);
    int tw = gui_ttf_render_width(it->tooltip, cap);
    int w = tw + 12, h = cap + 12;
    int x = tb->item_x[tb->tooltip_item], y = tb->y + 8 + oui_btn_h() + 4;
    if (x + w > win_w) x = win_w - w;
    if (x < 0) x = 0;
    if (y + h > win_h) y = tb->y + 8 - h - 4;
    win_draw_rect(win, x, y, w, h, bg);
    gui_draw_rect_outline(win, x, y, w, h, bd);
    win_draw_text_ttf(win, x + 6, oui_text_y(y, h, cap), it->tooltip, cap, ink);
}

void otb_draw_overlay(int win, otb_toolbar_t *tb, int win_w, int win_h) {
    if (tb->open_combo >= 0 && tb->combo_slot[tb->open_combo] >= 0)
        ocombo_draw_popup(win, &tb->combos[tb->combo_slot[tb->open_combo]], win_w, win_h);
    otb_more_draw(win, tb, win_w, win_h);
    if (!tb->more_open && tb->open_combo < 0) otb_tooltip_draw(win, tb, win_w, win_h);
}

int otb_popup_open(const otb_toolbar_t *tb) {
    return tb->more_open || tb->open_combo >= 0;
}

// Activate item i (button press, toggle flip, or MORE-row selection).
static int otb_activate(otb_toolbar_t *tb, int i) {
    const otb_item_t *it = &tb->items[i];
    if (!tb->enabled[i]) return -2;
    if (it->kind == OTB_TOGGLE) {
        int now = tb->checked[i] ? 0 : 1;
        if (it->group > 0 && !now) now = 1;     // a radio toggle cannot be un-clicked
        otb_check_index(tb, i, now);
        tb->last_value = now;
        return it->id;
    }
    if (it->kind == OTB_BUTTON) { tb->last_value = 0; return it->id; }
    return -2;
}

static int otb_open_combo(otb_toolbar_t *tb, int i, int win_w, int win_h, int at_more) {
    if (tb->combo_slot[i] < 0 || !tb->enabled[i]) return -2;
    ocombo_t *c = &tb->combos[tb->combo_slot[i]];
    if (at_more) {
        // Hidden combo opened from the MORE popup: anchor it under MORE.
        int w = otb_item_natural_w(&tb->items[i]);
        ocombo_place(c, tb->more_x + OTB_BTN_W - w, tb->y + (tb->h - oui_input_h()) / 2, w);
    }
    ocombo_open(c, win_w, win_h);
    tb->open_combo = i;
    tb->tooltip_shown = 0;
    return -2;
}

int otb_press(otb_toolbar_t *tb, int mx, int my, int win_w, int win_h) {
    tb->tooltip_shown = 0;
    if (tb->open_combo >= 0) {
        int i = tb->open_combo;
        ocombo_t *c = &tb->combos[tb->combo_slot[i]];
        int r = ocombo_press(c, mx, my, win_w, win_h);
        if (!c->open) { tb->open_combo = -1; if (tb->overflow_from >= 0 && i >= tb->overflow_from) otb_layout(tb, tb->w); }
        if (r >= 0) { tb->last_value = r; return tb->items[i].id; }
        if (r == OCOMBO_TEXT) { tb->last_value = -1; return tb->items[i].id; }
        return -2;
    }
    if (tb->more_open) {
        otb_more_build(tb, win_w, win_h);
        if (gui_list_hit(&tb->more_list, mx, my)) {
            int r = gui_list_press(&tb->more_list, mx, my);
            if (r >= 0 && r < tb->more_n) {
                int i = tb->more_map[r];
                tb->more_open = 0;
                if (tb->items[i].kind == OTB_COMBO) return otb_open_combo(tb, i, win_w, win_h, 1);
                return otb_activate(tb, i);
            }
            return -2;
        }
        tb->more_open = 0;
        return -2;
    }
    if (otb_hit_more(tb, mx, my)) { tb->more_pressed = 1; tb->more_hot = 1; return -2; }
    int i = otb_hit_item(tb, mx, my);
    if (i >= 0) {
        const otb_item_t *it = &tb->items[i];
        if (!tb->enabled[i]) return -2;
        if (it->kind == OTB_COMBO) return otb_open_combo(tb, i, win_w, win_h, 0);
        tb->pressed = i; tb->hot = i;
        return -2;
    }
    if (oui_in(mx, my, tb->x, tb->y, tb->w, tb->h)) return -2;
    return -1;
}

int otb_release(otb_toolbar_t *tb, int mx, int my) {
    if (tb->open_combo >= 0) {
        ocombo_release(&tb->combos[tb->combo_slot[tb->open_combo]]);
        return -2;
    }
    if (tb->more_open) { gui_list_release(&tb->more_list); return -2; }
    if (tb->more_pressed) {
        tb->more_pressed = 0;
        if (otb_hit_more(tb, mx, my)) { tb->more_open = 1; tb->more_row_hot = -1; memset(&tb->more_list, 0, sizeof(tb->more_list)); }
        return -2;
    }
    if (tb->pressed < 0) return -1;
    int i = tb->pressed;
    tb->pressed = -1;
    if (otb_hit_item(tb, mx, my) == i) return otb_activate(tb, i);
    return -2;
}

int otb_motion(otb_toolbar_t *tb, int mx, int my) {
    if (tb->open_combo >= 0) return ocombo_motion(&tb->combos[tb->combo_slot[tb->open_combo]], mx, my);
    if (tb->more_open) {
        int changed = gui_list_motion(&tb->more_list, mx, my);
        int r = gui_list_row_at(&tb->more_list, mx, my);
        if (r != tb->more_row_hot) { tb->more_row_hot = r; changed = 1; }
        return changed;
    }
    int changed = 0;
    int mh = otb_hit_more(tb, mx, my);
    if (mh != tb->more_hot) { tb->more_hot = mh; changed = 1; }
    int i = otb_hit_item(tb, mx, my);
    // A combo draws its own field look (no hover face), and a disabled item
    // gets no hover and no tooltip (spec 3.2).
    if (i >= 0 && (tb->items[i].kind == OTB_COMBO || !tb->enabled[i])) i = -1;
    if (i != tb->hot) {
        tb->hot = i;
        tb->hover_since_ms = uptime_ms();
        tb->tooltip_shown = 0;
        tb->tooltip_item = -1;
        changed = 1;
    }
    return changed;
}

int otb_wheel(otb_toolbar_t *tb, int mx, int my, int delta) {
    if (tb->open_combo >= 0) return ocombo_wheel(&tb->combos[tb->combo_slot[tb->open_combo]], mx, my, delta);
    if (tb->more_open) return gui_list_wheel(&tb->more_list, mx, my, delta);
    return 0;
}

void otb_leave(otb_toolbar_t *tb) {
    tb->hot = -1; tb->pressed = -1;
    tb->more_hot = 0; tb->more_pressed = 0; tb->more_open = 0;
    tb->tooltip_shown = 0; tb->tooltip_item = -1;
    tb->focus = -1; tb->focus_pressed = 0;
    if (tb->open_combo >= 0) { ocombo_close(&tb->combos[tb->combo_slot[tb->open_combo]]); tb->open_combo = -1; otb_layout(tb, tb->w); }
}

int otb_tick(otb_toolbar_t *tb, unsigned long long now_ms) {
    if (tb->hot < 0 || tb->tooltip_shown || otb_popup_open(tb)) return 0;
    if (!tb->enabled[tb->hot]) return 0;
    if (now_ms - tb->hover_since_ms >= OTB_TOOLTIP_MS) {
        tb->tooltip_shown = 1;
        tb->tooltip_item = tb->hot;
        return 1;
    }
    return 0;
}

int otb_timer_ms(const otb_toolbar_t *tb, unsigned long long now_ms) {
    if (tb->hot < 0 || tb->tooltip_shown || otb_popup_open(tb)) return -1;
    if (!tb->enabled[tb->hot]) return -1;
    unsigned long long el = now_ms - tb->hover_since_ms;
    if (el >= OTB_TOOLTIP_MS) return 1;
    int left = (int)(OTB_TOOLTIP_MS - el);
    return left < 10 ? 10 : left;
}

// --- Keyboard (Alt+T focus walk; popup keys) ----------------------------------
static int otb_focusable(const otb_toolbar_t *tb, int i) {
    return i >= 0 && i < tb->n && tb->visible[i] && tb->enabled[i] &&
           tb->items[i].kind != OTB_SEPARATOR && tb->items[i].kind != OTB_SPACER;
}

void otb_focus_enter(otb_toolbar_t *tb) {
    tb->focus = -1;
    for (int i = 0; i < tb->n; i++) if (otb_focusable(tb, i)) { tb->focus = i; break; }
}

static void otb_focus_step(otb_toolbar_t *tb, int dir) {
    if (tb->focus < 0) { otb_focus_enter(tb); return; }
    int i = tb->focus;
    for (int k = 0; k < tb->n; k++) {
        i += dir;
        if (i < 0) i = tb->n - 1;
        if (i >= tb->n) i = 0;
        if (otb_focusable(tb, i)) { tb->focus = i; return; }
    }
}

int otb_key(otb_toolbar_t *tb, unsigned int keycode, char key_char, int win_w, int win_h) {
    if (tb->open_combo >= 0) {
        int i = tb->open_combo;
        ocombo_t *c = &tb->combos[tb->combo_slot[i]];
        int r = ocombo_key(c, keycode, key_char, win_w, win_h);
        if (!c->open) { tb->open_combo = -1; if (tb->overflow_from >= 0 && i >= tb->overflow_from) otb_layout(tb, tb->w); }
        if (r >= 0) { tb->last_value = r; return tb->items[i].id; }
        if (r == OCOMBO_TEXT) { tb->last_value = -1; return tb->items[i].id; }
        return -2;
    }
    if (tb->more_open) {
        if (key_char == GUI_KEY_ESC) { tb->more_open = 0; return -2; }
        if (keycode == GUI_KEY_UP || keycode == GUI_KEY_DOWN) {
            int h = tb->more_row_hot < 0 ? 0 : tb->more_row_hot + (keycode == GUI_KEY_DOWN ? 1 : -1);
            if (h < 0) h = 0;
            if (h > tb->more_n - 1) h = tb->more_n - 1;
            tb->more_row_hot = h;
            gui_scroll_reveal(&tb->more_list.scroll, h * tb->more_list.row_h, tb->more_list.row_h);
            return -2;
        }
        if (key_char == GUI_KEY_ENTER || keycode == GUI_KEY_ENTER || key_char == ' ') {
            if (tb->more_row_hot >= 0 && tb->more_row_hot < tb->more_n) {
                int i = tb->more_map[tb->more_row_hot];
                tb->more_open = 0;
                if (tb->items[i].kind == OTB_COMBO) return otb_open_combo(tb, i, win_w, win_h, 1);
                return otb_activate(tb, i);
            }
            return -2;
        }
        return -2;
    }
    if (tb->focus < 0) return -1;
    if (key_char == GUI_KEY_ESC) { tb->focus = -1; tb->focus_pressed = 0; return -2; }
    if (keycode == GUI_KEY_LEFT)  { otb_focus_step(tb, -1); return -2; }
    if (keycode == GUI_KEY_RIGHT || keycode == GUI_KEY_TAB || key_char == GUI_KEY_TAB) { otb_focus_step(tb, 1); return -2; }
    if (key_char == ' ' || key_char == GUI_KEY_ENTER || keycode == GUI_KEY_ENTER) {
        int i = tb->focus;
        if (tb->items[i].kind == OTB_COMBO) return otb_open_combo(tb, i, win_w, win_h, 0);
        return otb_activate(tb, i);
    }
    return -2;                                    // focus swallows other keys
}
