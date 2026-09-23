// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// oui.c - private helpers shared by the office UI toolkit widgets. See oui.h.
#include "oui.h"
#include "../../libc/gui_font.h"
#include "../../libc/string.h"

void oui_focus_ring(int win, int x, int y, int w, int h) {
    uint32_t ring = oui_focus_color();
    // Guarantee the ring reads on both grounds it can land on (spec 3.4: a
    // ring can sit on the card body or the footer band), same rule
    // gui_set_palette() applies to its own focus token.
    ring = gui_ensure_contrast2(ring, oui_overlay(), oui_raised(), GUI_FLOOR_NONTEXT);
    int rx = x - 4, ry = y - 4, rw = w + 8, rh = h + 8;
    win_draw_rect(win, rx, ry, rw, 2, ring);
    win_draw_rect(win, rx, ry + rh - 2, rw, 2, ring);
    win_draw_rect(win, rx, ry, 2, rh, ring);
    win_draw_rect(win, rx + rw - 2, ry, 2, rh, ring);
}

// Resolved once per process: the UI font's family in its Bold face.
static int g_bold_face = -1, g_bold_bits = 0;
static void oui_bold_resolve(void) {
    if (g_bold_face >= 0) return;
    char fam[GUI_FONT_NAME_MAX];
    int ui = font_get_ui();
    if (ui < 0) ui = 0;
    fam[0] = 0;
    if (font_name(ui, fam, sizeof(fam)) <= 0) fam[0] = 0;
    int face = 0, bits = 0;
    if (gui_font_resolve(fam[0] ? fam : "DejaVu Sans", "Bold", &face, &bits) != 0) {
        face = ui; bits = FONT_STYLE_BOLD;
    }
    g_bold_face = face;
    g_bold_bits = bits;
}

void oui_text_bold(int win, int x, int y, const char *s, int size, uint32_t ink) {
    oui_bold_resolve();
    win_draw_text_ttf_ex(win, x, y, s, g_bold_face, size, g_bold_bits, ink);
}

int oui_bold_width(const char *s, int size) {
    oui_bold_resolve();
    int w = ttf_measure_ex(s, g_bold_face, size, g_bold_bits);
    if (w < 0) w = gui_ttf_render_width(s, size);
    return w;
}

void oui_text_bold_centered(int win, int x, int y, int w, int h, const char *s, int size, uint32_t ink) {
    int tw = oui_bold_width(s, size);
    oui_text_bold(win, x + (w - tw) / 2, oui_text_y(y, h, size), s, size, ink);
}

void oui_ellipsize(char *out, int cap, const char *s, int size, int max_w) {
    if (!out || cap <= 0) return;
    if (!s) { out[0] = 0; return; }
    int n = (int)strlen(s);
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) out[i] = s[i];
    out[n] = 0;
    if (gui_ttf_render_width(out, size) <= max_w) return;
    int ell_w = gui_ttf_render_width("...", size);
    // Shrink until prefix + "..." fits (measured, not counted).
    while (n > 0) {
        n--;
        out[n] = 0;
        if (gui_ttf_render_width(out, size) + ell_w <= max_w) break;
    }
    if (n + 3 < cap) { out[n] = '.'; out[n + 1] = '.'; out[n + 2] = '.'; out[n + 3] = 0; }
}

void oui_scrim(int win, int w, int h) {
    for (int yy = 0; yy < h; yy += 2) win_draw_rect(win, 0, yy, w, 1, 0x00000000);
}

void oui_copy(char *dst, int cap, const char *src) {
    int i = 0;
    if (!dst || cap <= 0) return;
    if (src) { for (; src[i] && i < cap - 1; i++) dst[i] = src[i]; }
    dst[i] = 0;
}

int oui_itoa(int v, char *out, int cap) {
    char tmp[16]; int n = 0, neg = v < 0;
    unsigned int u = neg ? (unsigned int)(-v) : (unsigned int)v;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 15);
    int len = 0;
    if (neg && len < cap - 1) out[len++] = '-';
    while (n > 0 && len < cap - 1) out[len++] = tmp[--n];
    out[len] = 0;
    return len;
}
