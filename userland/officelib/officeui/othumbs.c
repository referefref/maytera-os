// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// othumbs.c - the Slides thumbnail strip (see othumbs.h).
#include "othumbs.h"
#include "oui.h"
#include "../../libc/string.h"

void othumbs_config(othumbs_t *s, int x, int y, int w, int h, int count, int sel) {
    s->x = x; s->y = y; s->w = w > 0 ? w : OTHUMB_STRIP_W; s->h = h;
    s->count = count < 0 ? 0 : count;
    s->sel = (sel >= 0 && sel < s->count) ? sel : (s->count ? 0 : -1);
    gui_list_config(&s->list, x, y, s->w, h, OTHUMB_ROW_H, s->count);
    s->list.scroll.snap = 1;
}

int othumbs_rect(const othumbs_t *s, int index, int *x, int *y, int *w, int *h) {
    int ry;
    if (!gui_list_row_y(&s->list, index, &ry)) return 0;
    if (x) *x = s->x + OTHUMB_X;
    if (y) *y = ry + OTHUMB_Y;
    if (w) *w = OTHUMB_W;
    if (h) *h = OTHUMB_H;
    return 1;
}

void othumbs_draw(int win, othumbs_t *s, othumb_render_fn render, void *ctx) {
    uint32_t ground = oui_raised(), paper = oui_sunken();
    uint32_t rule = gui_ensure_contrast(oui_border_subtle(), ground, GUI_FLOOR_NONTEXT);
    uint32_t strong = gui_ensure_contrast(oui_border_strong(), ground, GUI_FLOOR_NONTEXT);
    uint32_t acc = gui_ensure_contrast(oui_accent(), ground, GUI_FLOOR_NONTEXT);
    uint32_t num = oui_muted_on(ground);
    int cap = oui_caption();
    win_draw_rect(win, s->x, s->y, s->w, s->h, ground);
    win_draw_rect(win, s->x + s->w - 1, s->y, 1, s->h, rule);
    int first = gui_list_first(&s->list), span = gui_list_span(&s->list);
    for (int r = 0; r < span; r++) {
        int i = first + r;
        if (i >= s->count) break;
        int ry;
        if (!gui_list_row_y(&s->list, i, &ry)) continue;
        // Clip: a partially visible last row must not draw past the strip.
        int cell_bottom = ry + OTHUMB_ROW_H;
        if (cell_bottom > s->y + s->h) {
            // Draw only when the thumbnail itself fits; the numeral alone is
            // not worth a half-drawn cell.
            if (ry + OTHUMB_Y + OTHUMB_H + 3 > s->y + s->h) continue;
        }
        char n[12]; oui_itoa(i + 1, n, sizeof(n));
        win_draw_text_ttf(win, s->x + 8, ry + 2, n, cap, num);
        int tx = s->x + OTHUMB_X, ty = ry + OTHUMB_Y;
        win_draw_rect(win, tx, ty, OTHUMB_W, OTHUMB_H, paper);
        if (render) render(win, i, tx + 1, ty + 1, OTHUMB_W - 2, OTHUMB_H - 2, ctx);
        gui_draw_rect_outline(win, tx, ty, OTHUMB_W, OTHUMB_H, strong);
        if (i == s->sel) {
            int rx = tx - 4, ryy = ty - 4, rw = OTHUMB_W + 8, rh = OTHUMB_H + 8;
            win_draw_rect(win, rx, ryy, rw, 2, acc);
            win_draw_rect(win, rx, ryy + rh - 2, rw, 2, acc);
            win_draw_rect(win, rx, ryy, 2, rh, acc);
            win_draw_rect(win, rx + rw - 2, ryy, 2, rh, acc);
        } else if (i == s->hot) {
            gui_draw_rect_outline(win, tx - 3, ty - 3, OTHUMB_W + 6, OTHUMB_H + 6, strong);
        }
    }
    gui_scroll_draw_on(win, &s->list.scroll, ground);
}

int othumbs_press(othumbs_t *s, int mx, int my) {
    if (!gui_list_hit(&s->list, mx, my)) return -1;
    int r = gui_list_press(&s->list, mx, my);
    if (r >= 0) { s->sel = r; return r; }
    return -2;
}

int othumbs_motion(othumbs_t *s, int mx, int my) {
    int changed = gui_list_motion(&s->list, mx, my);
    int r = gui_list_hit(&s->list, mx, my) ? gui_list_row_at(&s->list, mx, my) : -1;
    if (r != s->hot) { s->hot = r; changed = 1; }
    return changed;
}

void othumbs_release(othumbs_t *s) { gui_list_release(&s->list); }

int othumbs_wheel(othumbs_t *s, int mx, int my, int delta) {
    return gui_list_wheel(&s->list, mx, my, delta);
}

void othumbs_reveal(othumbs_t *s) {
    if (s->sel >= 0) gui_scroll_reveal(&s->list.scroll, s->sel * OTHUMB_ROW_H, OTHUMB_ROW_H);
}

int othumbs_key(othumbs_t *s, unsigned int keycode) {
    if (s->count <= 0) return 0;
    int page = s->h / OTHUMB_ROW_H; if (page < 1) page = 1;
    int step = 0;
    switch (keycode) {
        case GUI_KEY_UP:   step = -1; break;
        case GUI_KEY_DOWN: step = 1; break;
        case GUI_KEY_PGUP: step = -page; break;
        case GUI_KEY_PGDN: step = page; break;
        case GUI_KEY_HOME: step = -s->count; break;
        case GUI_KEY_END:  step = s->count; break;
        default: return 0;
    }
    int sel = s->sel < 0 ? 0 : s->sel;
    int changed = gui_list_move_sel(&s->list, &sel, step);
    if (sel != s->sel) { s->sel = sel; changed = 1; }
    return changed;
}
