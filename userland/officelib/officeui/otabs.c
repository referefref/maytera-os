// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// otabs.c - the Calc sheet-tab strip (see otabs.h).
#include "otabs.h"
#include "oicon.h"
#include "oui.h"
#include "../../libc/string.h"

#define OT_INSET   8
#define OT_PAD     12
#define OT_MIN_W   64
#define OT_MAX_W   140
#define OT_BTN     26
#define OT_P_PREV  (-10)
#define OT_P_NEXT  (-11)

void otabs_init(otabs_t *t, int x, int y, int w) {
    memset(t, 0, sizeof(*t));
    t->x = x; t->y = y; t->w = w; t->h = OTAB_H;
    t->hot = -1; t->pressed = -1; t->add_x = -1; t->chev_x = -1;
    t->renaming_tab = -1;
    t->last_visible = -1;
    tf_init(&t->rename_tf, t->rename_buf, sizeof(t->rename_buf));
}

void otabs_move(otabs_t *t, int x, int y, int w) { t->x = x; t->y = y; t->w = w; t->h = OTAB_H; }

static const char *ot_label(otabs_label_fn label, void *ctx, int i) {
    const char *s = label ? label(ctx, i) : 0;
    return s ? s : "";
}

void otabs_layout(otabs_t *t, int count, int active, otabs_label_fn label, void *ctx) {
    int size = oui_body();
    if (count > OTAB_MAX) count = OTAB_MAX;
    if (count < 0) count = 0;
    t->count = count;
    t->active = (active >= 0 && active < count) ? active : (count ? 0 : -1);
    if (t->first_visible > count - 1) t->first_visible = count - 1;
    if (t->first_visible < 0) t->first_visible = 0;
    // Natural widths.
    int total = 0;
    for (int i = 0; i < count; i++) {
        int tw = gui_ttf_render_width(ot_label(label, ctx, i), size) + 2 * OT_PAD;
        if (tw < OT_MIN_W) tw = OT_MIN_W;
        if (tw > OT_MAX_W) tw = OT_MAX_W;
        t->tab_w[i] = tw;
        total += tw;
    }
    int right = t->x + t->w - OT_INSET;
    int overflow = (total + OT_BTN > t->w - 2 * OT_INSET);
    t->chev_x = overflow ? right - 2 * OT_BTN : -1;
    int limit = overflow ? t->chev_x - 4 : right;
    // Keep the active tab reachable: scroll first_visible so it fits.
    if (overflow && t->active >= 0) {
        if (t->active < t->first_visible) t->first_visible = t->active;
        for (;;) {
            int x = t->x + OT_INSET;
            for (int i = t->first_visible; i <= t->active; i++) x += t->tab_w[i];
            if (x <= limit - OT_BTN || t->first_visible >= t->active) break;
            t->first_visible++;
        }
    } else if (!overflow) t->first_visible = 0;
    int x = t->x + OT_INSET;
    t->last_visible = -1;
    for (int i = 0; i < count; i++) {
        if (i < t->first_visible) { t->tab_x[i] = -1; continue; }
        if (x + t->tab_w[i] > limit - (overflow ? 0 : OT_BTN)) { t->tab_x[i] = -1; continue; }
        t->tab_x[i] = x;
        t->last_visible = i;
        x += t->tab_w[i];
    }
    t->add_x = (x + OT_BTN <= limit) ? x : -1;
}

static void ot_icon_btn(int win, int x, int y, const char *icon, const char *fallback, gui_state_t st) {
    gui_button(win, x, y, OT_BTN, OT_BTN, "", GUI_BTN_SECONDARY, st);
    gui_palette_t *p = gui_pal();
    uint32_t face = gui_mix(p->surface_raised, p->ink, 8);
    if (st == GUI_ST_HOVER) face = gui_lighten(face, 18);
    else if (st == GUI_ST_PRESSED) face = gui_darken(face, 18);
    uint32_t ink = oui_glyph_on(oui_on_surface(), face);
    if (!oicon_draw(win, icon, x + 5, y + 5, 16, ink, face))
        oui_text_bold_centered(win, x, y, OT_BTN, OT_BTN, fallback, oui_body(), ink);
}

void otabs_draw(int win, otabs_t *t, otabs_label_fn label, void *ctx) {
    int size = oui_body();
    uint32_t ground = oui_raised(), paper = oui_sunken();
    uint32_t rule = gui_ensure_contrast(oui_border_subtle(), ground, GUI_FLOOR_NONTEXT);
    uint32_t acc  = gui_ensure_contrast(oui_accent(), paper, GUI_FLOOR_NONTEXT);
    uint32_t ink_active = oui_ink_on(paper), ink_muted = oui_muted_on(ground);
    uint32_t hover = oui_tok(THEME_COLOR_BUTTON_LIGHT, gui_lighten(ground, 12));
    win_draw_rect(win, t->x, t->y, t->w, t->h, ground);
    win_draw_rect(win, t->x, t->y, t->w, 1, rule);
    for (int i = t->first_visible; i <= t->last_visible && i < t->count; i++) {
        int x = t->tab_x[i], w = t->tab_w[i];
        if (x < 0) continue;
        int active = (i == t->active);
        if (active) {
            win_draw_rect(win, x, t->y, w, t->h, paper);
            win_draw_rect(win, x, t->y, w, 2, acc);
        } else {
            if (t->hot == i) win_draw_rect(win, x, t->y + 1, w, t->h - 1, hover);
            win_draw_rect(win, x + w - 1, t->y + 4, 1, t->h - 8, rule);
        }
        if (t->renaming && t->renaming_tab == i) {
            textfield_t *tf = &t->rename_tf;
            gui_textfield_tf(win, x, t->y + 1, w, t->h - 2, tf->buf, tf->len, tf->cursor, tf->sel_anchor, true, 0);
            continue;
        }
        char el[64];
        oui_ellipsize(el, sizeof(el), ot_label(label, ctx, i), size, w - 2 * OT_PAD + 6);
        int tw = gui_ttf_render_width(el, size);
        uint32_t ink = active ? ink_active : (t->hot == i ? gui_ensure_contrast(ink_muted, hover, GUI_FLOOR_TEXT) : ink_muted);
        win_draw_text_ttf(win, x + (w - tw) / 2, oui_text_y(t->y, t->h, size), el, size, ink);
    }
    if (t->add_x >= 0) {
        gui_state_t st = (t->pressed == OTAB_ADD && t->hot == OTAB_ADD) ? GUI_ST_PRESSED : (t->hot == OTAB_ADD ? GUI_ST_HOVER : GUI_ST_NORMAL);
        ot_icon_btn(win, t->add_x, t->y, "PLUS", "+", st);
    }
    if (t->chev_x >= 0) {
        gui_state_t s1 = (t->pressed == OT_P_PREV && t->hot == OT_P_PREV) ? GUI_ST_PRESSED : (t->hot == OT_P_PREV ? GUI_ST_HOVER : GUI_ST_NORMAL);
        gui_state_t s2 = (t->pressed == OT_P_NEXT && t->hot == OT_P_NEXT) ? GUI_ST_PRESSED : (t->hot == OT_P_NEXT ? GUI_ST_HOVER : GUI_ST_NORMAL);
        if (t->first_visible <= 0) s1 = GUI_ST_DISABLED;
        if (t->last_visible >= t->count - 1) s2 = GUI_ST_DISABLED;
        ot_icon_btn(win, t->chev_x, t->y, "CHEVL", "<", s1);
        ot_icon_btn(win, t->chev_x + OT_BTN, t->y, "CHEVR", ">", s2);
    }
}

static int ot_hit(const otabs_t *t, int mx, int my) {
    if (!oui_in(mx, my, t->x, t->y, t->w, t->h)) return -1;
    for (int i = t->first_visible; i <= t->last_visible && i < t->count; i++)
        if (t->tab_x[i] >= 0 && oui_in(mx, my, t->tab_x[i], t->y, t->tab_w[i], t->h)) return i;
    if (t->add_x >= 0 && oui_in(mx, my, t->add_x, t->y, OT_BTN, t->h)) return OTAB_ADD;
    if (t->chev_x >= 0) {
        if (oui_in(mx, my, t->chev_x, t->y, OT_BTN, t->h)) return OT_P_PREV;
        if (oui_in(mx, my, t->chev_x + OT_BTN, t->y, OT_BTN, t->h)) return OT_P_NEXT;
    }
    return -2;
}

int otabs_press(otabs_t *t, int mx, int my) {
    int h = ot_hit(t, mx, my);
    if (h == -1) return -1;
    if (t->renaming && h != t->renaming_tab) otabs_cancel_rename(t);
    if (h >= 0) { t->pressed = -1; return h; }          // a tab: select on press (spreadsheet feel)
    if (h == OTAB_ADD || h == OT_P_PREV || h == OT_P_NEXT) { t->pressed = h; t->hot = h; return -2; }
    return -2;
}

int otabs_release(otabs_t *t, int mx, int my) {
    if (t->pressed == -1) return -1;
    int p = t->pressed; t->pressed = -1;
    if (ot_hit(t, mx, my) != p) return -2;
    if (p == OTAB_ADD) return OTAB_ADD;
    if (p == OT_P_PREV) { if (t->first_visible > 0) t->first_visible--; return -2; }
    if (p == OT_P_NEXT) { if (t->last_visible < t->count - 1) t->first_visible++; return -2; }
    return -2;
}

int otabs_dblclick(otabs_t *t, int mx, int my) {
    int h = ot_hit(t, mx, my);
    if (h < 0) return -1;
    return h;
}

int otabs_motion(otabs_t *t, int mx, int my) {
    int h = ot_hit(t, mx, my);
    if (h == -2) h = -1;
    if (h != t->hot) { t->hot = h; return 1; }
    return 0;
}

void otabs_leave(otabs_t *t) { t->hot = -1; t->pressed = -1; }

void otabs_begin_rename(otabs_t *t, int index, const char *current) {
    if (index < 0 || index >= t->count) return;
    t->renaming = 1;
    t->renaming_tab = index;
    tf_init(&t->rename_tf, t->rename_buf, sizeof(t->rename_buf));
    tf_set_text(&t->rename_tf, current ? current : "");
    tf_select_all(&t->rename_tf);
}

void otabs_cancel_rename(otabs_t *t) { t->renaming = 0; t->renaming_tab = -1; }

// Sheet names: 1..31 chars, none of [ ] : * ? / \ (spec 5.2.3).
static int ot_name_ok(const char *s) {
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (c == '[' || c == ']' || c == ':' || c == '*' || c == '?' || c == '/' || c == '\\') return 0;
    }
    return n >= 1 && n <= 31;
}

int otabs_key(otabs_t *t, unsigned int keycode, char key_char, int mods) {
    if (t->renaming) {
        if (key_char == GUI_KEY_ESC) { otabs_cancel_rename(t); return -2; }
        if (key_char == GUI_KEY_ENTER || keycode == GUI_KEY_ENTER) {
            if (!ot_name_ok(t->rename_buf)) return -2;   // keep editing until valid
            t->renaming = 0;
            return OTAB_RENAMED;                          // renaming_tab + rename_buf hold the result
        }
        gui_event_t ev; memset(&ev, 0, sizeof(ev));
        ev.type = EVENT_KEY_DOWN; ev.keycode = keycode; ev.key_char = key_char;
        // Refuse the forbidden characters at the keystroke, so the field never
        // holds a name the commit would reject.
        if (key_char == '[' || key_char == ']' || key_char == ':' || key_char == '*' ||
            key_char == '?' || key_char == '/' || key_char == '\\') return -2;
        tf_handle_key(&t->rename_tf, &ev);
        return -2;
    }
    if ((mods & GUI_MOD_CTRL) && keycode == GUI_KEY_PGUP) return OTAB_PREV;
    if ((mods & GUI_MOD_CTRL) && keycode == GUI_KEY_PGDN) return OTAB_NEXT;
    return -1;
}
