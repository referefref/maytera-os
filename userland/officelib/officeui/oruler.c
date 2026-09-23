// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// oruler.c - the Writer ruler (see oruler.h).
#include "oruler.h"
#include "oui.h"

void oruler_draw(int win, int x, int y, int w, int page_x, int page_w,
                 int margin_l_px, int margin_r_px, int ppi) {
    uint32_t ground = oui_raised(), band = oui_sunken();
    uint32_t rule = gui_ensure_contrast(oui_border_subtle(), ground, GUI_FLOOR_NONTEXT);
    uint32_t tick = gui_ensure_contrast(oui_border_strong(), band, GUI_FLOOR_NONTEXT);
    uint32_t num  = oui_muted_on(band);
    uint32_t acc  = gui_ensure_contrast(oui_accent(), ground, GUI_FLOOR_NONTEXT);
    int cap = oui_caption();
    if (ppi <= 0) ppi = 96;
    win_draw_rect(win, x, y, w, ORULER_H, ground);
    win_draw_rect(win, x, y + ORULER_H - 1, w, 1, rule);
    if (page_w <= 0) return;
    int bl = page_x + margin_l_px, br = page_x + page_w - margin_r_px;
    // Clip the band to the strip.
    int cl = bl < x ? x : bl, cr = br > x + w ? x + w : br;
    if (cr > cl) win_draw_rect(win, cl, y + 3, cr - cl, ORULER_H - 7, band);
    // Ticks every 1/8 in from the left margin, both directions inside the band.
    int eighth = ppi / 8; if (eighth < 2) eighth = 2;
    int base_y = y + ORULER_H - 5;
    int max_k = (br - bl) / eighth + 1;
    for (int k = 0; k <= max_k; k++) {
        int tx = bl + k * eighth;
        if (tx >= br) break;
        if (tx < x || tx >= x + w) continue;
        int th = (k % 8 == 0) ? 7 : (k % 4 == 0) ? 5 : 3;
        if (k % 8 == 0 && k > 0) {
            char n[8]; oui_itoa(k / 8, n, sizeof(n));
            int tw = gui_ttf_render_width(n, cap);
            win_draw_text_ttf(win, tx - tw / 2, y + 3, n, cap, num);
            win_draw_rect(win, tx, base_y - 2, 1, 3, tick);
        } else {
            win_draw_rect(win, tx, base_y - th, 1, th, tick);
        }
    }
    // Indent markers (draw only in v1).
    if (bl >= x && bl < x + w) gui_chevron(win, bl, y + 4, GUI_CHEV_DOWN, acc);
    if (br >= x && br < x + w) gui_chevron(win, br, y + 4, GUI_CHEV_DOWN, acc);
}
