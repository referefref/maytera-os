// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// cardtext.c - rotated (sideways) antialiased text for the Cardfile deck.
// See cardtext.h for the rationale. In one line: no new kernel syscall is
// needed because SYS_FONT_GLYPH (font_glyph()) already hands userland an
// 8-bit alpha coverage bitmap per glyph, and the compositor owns g_fb, so the
// whole lay-out / rotate / blit is done here in Ring 3.

#include "cardtext.h"
#include "compositor.h"                 // g_fb, g_fb_*, g_clip_*, ui_px, draw helpers
#include "../../libc/syscall.h"         // font_glyph, font_metrics, font_kern (SYS_FONT_*)
#include "../../libc/stdlib.h"          // malloc/free

// Everything here scales with ui_px() exactly like draw_text_ttf(): the caller
// passes a LOGICAL size; we rasterise at the same effective size the rest of
// the UI draws at, so a vertical label matches a horizontal one pixel for
// pixel. draw_text_ttf() does `size = ui_px(size)`; mirror that.
static int cf_eff_size(int size) {
    int s = ui_px(size);
    if (s < 4) s = 4;
    if (s > 128) s = 128;
    return s;
}

// Line height (short axis / plate thickness) for a size: ascent - descent.
static int cf_line_h(int eff) {
    int fm[3] = { 0, 0, 0 };            // {ascent, descent, line_gap}; descent <= 0
    if (font_metrics(0, eff, fm) != 0 || (fm[0] - fm[1]) <= 0) {
        return eff + eff / 4;           // sane fallback if metrics unavailable
    }
    return fm[0] - fm[1];
}

int cardtext_vertical_thickness(int size) {
    return cf_line_h(cf_eff_size(size));
}

int cardtext_vertical_len(const char *text, int size) {
    // The tall axis == the horizontal run width, which is exactly what
    // text_width_ttf() measures. Keep the two in lockstep.
    return text_width_ttf(text, size);
}

// Minimal UTF-8 decode: returns the codepoint at *p and advances *p. ASCII
// fast path covers every shipped app name and the "+"/"SORT" chrome labels;
// multibyte is decoded best-effort so a non-Latin title still renders.
static unsigned cf_next_cp(const unsigned char **p) {
    unsigned c = **p;
    if (c < 0x80) { (*p)++; return c; }
    int n; unsigned cp;
    if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else { (*p)++; return c; }          // invalid lead byte: pass through
    (*p)++;
    for (int i = 0; i < n; i++) {
        if (((*p)[0] & 0xC0) != 0x80) return cp;   // truncated
        cp = (cp << 6) | ((*p)[0] & 0x3F);
        (*p)++;
    }
    return cp;
}

int cardtext_vertical(int32_t cx, int32_t y_top, int max_len,
                      const char *text, int size, uint32_t color) {
    if (!text || !*text || !g_fb || max_len <= 0) return 0;

    const int eff = cf_eff_size(size);
    const int LH  = cf_line_h(eff);                 // short axis (thickness)
    if (LH <= 0) return 0;

    int run = cardtext_vertical_len(text, size);    // tall axis before clip
    if (run <= 0) return 0;
    if (run > max_len) run = max_len;               // hard clip (deck ellipsises upstream)

    const int HARD = 1024;                          // sanity cap on the tall axis
    if (run > HARD) run = HARD;

    // Fully outside the active clip? The strip occupies
    // x in [cx-LH/2, cx-LH/2+LH), y in [y_top, y_top+run).
    int32_t sx0 = cx - LH / 2, sy0 = y_top;
    if (sx0 + LH <= g_clip_x0 || sx0 >= g_clip_x1 ||
        sy0 + run <= g_clip_y0 || sy0 >= g_clip_y1) {
        return run;                                  // nothing visible, but report the length
    }

    // Horizontal coverage buffer: width = run (tall axis), height = LH.
    size_t hsz = (size_t)run * (size_t)LH;
    uint8_t *hbuf = (uint8_t *)malloc(hsz);
    if (!hbuf) return 0;
    for (size_t i = 0; i < hsz; i++) hbuf[i] = 0;

    // Per-glyph scratch: font_glyph() wants cap >= glyph w*h. A glyph fits in
    // roughly eff x eff; give margin.
    int gcap_dim = eff + 6;
    size_t gcap = (size_t)gcap_dim * (size_t)gcap_dim;
    uint8_t *gbmp = (uint8_t *)malloc(gcap);
    if (!gbmp) { free(hbuf); return 0; }

    int fm[3] = { eff, 0, 0 };
    font_metrics(0, eff, fm);
    int baseline = fm[0];                            // ascent
    if (baseline <= 0 || baseline >= LH) baseline = (LH * 4) / 5;

    // Lay the run out left to right into hbuf.
    const unsigned char *p = (const unsigned char *)text;
    int pen = 0;
    unsigned prev = 0;
    while (*p && pen < run) {
        unsigned cp = cf_next_cp(&p);
        if (prev) pen += font_kern(0, eff, (int)prev, (int)cp);   // kern(prev, cp)
        font_glyph_meta_t gm;
        int adv = font_glyph(0, eff, 0, (int)cp, &gm, gbmp, (int)gcap);
        if (adv < 0) { prev = cp; continue; }
        // Blit this glyph's coverage into hbuf at (pen + xoff, baseline + yoff).
        int gx = pen + gm.xoff;
        int gy = baseline + gm.yoff;
        for (int ry = 0; ry < gm.height; ry++) {
            int dy = gy + ry;
            if (dy < 0 || dy >= LH) continue;
            const uint8_t *srow = &gbmp[(size_t)ry * gm.width];
            uint8_t *drow = &hbuf[(size_t)dy * run];
            for (int rx = 0; rx < gm.width; rx++) {
                int dx = gx + rx;
                if (dx < 0 || dx >= run) continue;
                uint8_t v = srow[rx];
                if (v > drow[dx]) drow[dx] = v;      // max-merge overlapping coverage
            }
        }
        pen += adv;
        prev = cp;
    }
    free(gbmp);

    // Rotate 90 degrees CLOCKWISE into the framebuffer while blitting, so the
    // first glyph (run x=0) lands at the TOP (dest y=0): the label reads
    // top-to-bottom, like a book spine. Source (sx in [0,run), sy in [0,LH))
    // maps to dest column (LH-1-sy) and dest row sx.
    uint32_t ink = color & 0x00FFFFFFu;
    int32_t base_x = cx - LH / 2;
    for (int sx = 0; sx < run; sx++) {
        int32_t fy = y_top + sx;
        if (fy < g_clip_y0 || fy >= g_clip_y1 || fy < 0 || fy >= g_fb_height) continue;
        const uint8_t *srow = &hbuf[(size_t)sx];     // stride is `run` per LH-row; gather by column
        for (int sy = 0; sy < LH; sy++) {
            uint8_t cov = srow[(size_t)sy * run];
            if (!cov) continue;
            int32_t fx = base_x + (LH - 1 - sy);
            if (fx < g_clip_x0 || fx >= g_clip_x1 || fx < 0 || fx >= g_fb_width) continue;
            uint32_t *dp = &g_fb[(size_t)fy * g_fb_pitch + fx];
            uint32_t bg = *dp;
            // src-over with coverage as alpha.
            uint32_t sr = (ink >> 16) & 0xFF, sg = (ink >> 8) & 0xFF, sb = ink & 0xFF;
            uint32_t br = (bg >> 16) & 0xFF, bg2 = (bg >> 8) & 0xFF, bb = bg & 0xFF;
            uint32_t a = cov;
            uint32_t rr = (sr * a + br * (255 - a) + 127) / 255;
            uint32_t rg = (sg * a + bg2 * (255 - a) + 127) / 255;
            uint32_t rb = (sb * a + bb * (255 - a) + 127) / 255;
            *dp = 0xFF000000u | (rr << 16) | (rg << 8) | rb;
        }
    }
    free(hbuf);
    return run;
}
