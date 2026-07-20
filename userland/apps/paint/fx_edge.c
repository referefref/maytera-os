// fx_edge.c - Maytera Studio "Edge-Detect" menu ops (registry). Integer only.
#include "studio.h"

static int lum(uint32_t p) { return (px_r(p) * 77 + px_g(p) * 150 + px_b(p) * 29) >> 8; }
static uint32_t cblend(uint32_t o, uint32_t r, int cov) {
    if (cov >= 255) return r;
    if (cov <= 0) return o;
    int R = px_r(o) + ((px_r(r) - px_r(o)) * cov) / 255;
    int G = px_g(o) + ((px_g(r) - px_g(o)) * cov) / 255;
    int B = px_b(o) + ((px_b(r) - px_b(o)) * cov) / 255;
    int A = px_a(o) + ((px_a(r) - px_a(o)) * cov) / 255;
    return argb(A, R, G, B);
}
static inline uint32_t at(const uint32_t *p, int w, int h, int x, int y) {
    x = clampi(x, 0, w - 1); y = clampi(y, 0, h - 1);
    return p[(size_t)y * w + x];
}
static inline int latt(const uint32_t *p, int w, int h, int x, int y) { return lum(at(p, w, h, x, y)); }

// two-pass box blur of an int map (defined lower in this file); used by Neon glow
static void boxlum(int *src, int *dst, int w, int h, int r);

// Edge: 0 Sobel, 1 Prewitt, 2 Laplace + amount
static int op_edge(const int *p) {
    int mode = p[0], amt = clampi(p[1], 1, 400);
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    uint32_t *src = (uint32_t *)malloc(n * 4); if (!src) return -1;
    memcpy(src, px, n * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int mag;
            if (mode == 2) {
                int c = latt(src, w, h, x, y);
                int s = latt(src, w, h, x - 1, y) + latt(src, w, h, x + 1, y) +
                        latt(src, w, h, x, y - 1) + latt(src, w, h, x, y + 1) - 4 * c;
                mag = abs(s);
            } else {
                int a = mode == 0 ? 2 : 1;   // sobel weights center row/col = 2
                int gx = -latt(src, w, h, x - 1, y - 1) - a * latt(src, w, h, x - 1, y) - latt(src, w, h, x - 1, y + 1)
                         + latt(src, w, h, x + 1, y - 1) + a * latt(src, w, h, x + 1, y) + latt(src, w, h, x + 1, y + 1);
                int gy = -latt(src, w, h, x - 1, y - 1) - a * latt(src, w, h, x, y - 1) - latt(src, w, h, x + 1, y - 1)
                         + latt(src, w, h, x - 1, y + 1) + a * latt(src, w, h, x, y + 1) + latt(src, w, h, x + 1, y + 1);
                mag = (abs(gx) + abs(gy));
            }
            mag = clampi(mag * amt / 100, 0, 255);
            size_t i = (size_t)y * w + x;
            px[i] = cblend(src[i], argb(px_a(src[i]), mag, mag, mag), draw_cov(x, y));
        }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Laplace only (sharp edges to gray)
static int op_laplace(const int *p) { int q[2] = { 2, p[0] ? p[0] : 100 }; return op_edge(q); }

// Neon: GIMP-style glowing coloured edges screen-composited over a darkened
// source. Compute Sobel edge magnitude, box-blur it into a soft glow, colourise
// by the foreground or a colour param scaled by intensity, then SCREEN the glow
// over the source darkened by the "Darken %" amount.
// Params: 0 glow radius, 1 amount %, 2 colour, 3 use FG colour, 4 darken %.
static int op_neon(const int *p) {
    int radius = clampi(p[0], 1, 8);
    int amt    = clampi(p[1], 1, 400);
    int col    = p[2];
    int usefg  = p[3];
    int darken = clampi(p[4], 0, 100);
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    int *mag  = (int *)malloc(n * sizeof(int));
    int *glow = (int *)malloc(n * sizeof(int));
    if (!mag || !glow) { free(mag); free(glow); return -1; }

    // neon colour: foreground colour, or the colour param (0xRRGGBB)
    int cr, cg, cb;
    if (usefg) { cr = px_r(g_tool.fg); cg = px_g(g_tool.fg); cb = px_b(g_tool.fg); }
    else       { cr = (col >> 16) & 255; cg = (col >> 8) & 255; cb = col & 255; }

    // sharp edge magnitude from luminance (Sobel), scaled by amount
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int gx = -latt(px, w, h, x - 1, y - 1) - 2 * latt(px, w, h, x - 1, y) - latt(px, w, h, x - 1, y + 1)
                     + latt(px, w, h, x + 1, y - 1) + 2 * latt(px, w, h, x + 1, y) + latt(px, w, h, x + 1, y + 1);
            int gy = -latt(px, w, h, x - 1, y - 1) - 2 * latt(px, w, h, x, y - 1) - latt(px, w, h, x + 1, y - 1)
                     + latt(px, w, h, x - 1, y + 1) + 2 * latt(px, w, h, x, y + 1) + latt(px, w, h, x + 1, y + 1);
            mag[(size_t)y * w + x] = clampi((abs(gx) + abs(gy)) * amt / 100, 0, 255);
        }

    // soft glow: box-blur the edge map (two-pass separable, reuses boxlum)
    boxlum(mag, glow, w, h, radius);

    // colourise (sharp core + boosted soft halo), then screen over darkened source
    int keep = 100 - darken;             // percent of the source that survives
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            int inten = clampi(mag[i] + glow[i] * 3, 0, 255);
            int gr = cr * inten / 255, gg = cg * inten / 255, gb = cb * inten / 255;
            uint32_t o = px[i];
            int dr = px_r(o) * keep / 100, dg = px_g(o) * keep / 100, db = px_b(o) * keep / 100;
            int sr = 255 - (255 - dr) * (255 - gr) / 255;   // SCREEN composite
            int sg = 255 - (255 - dg) * (255 - gg) / 255;
            int sb = 255 - (255 - db) * (255 - gb) / 255;
            px[i] = cblend(o, argb(px_a(o), sr, sg, sb), draw_cov(x, y));
        }
    free(mag); free(glow);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Image gradient magnitude -> gray
static int op_gradient(const int *p) { int q[2] = { 0, p[0] ? p[0] : 100 }; return op_edge(q); }

// Difference of Gaussians (r1<r2): edges from two box-blur passes of luminance
static void boxlum(int *src, int *dst, int w, int h, int r) {
    int *tmp = (int *)malloc((size_t)w * h * sizeof(int));
    if (!tmp) { memcpy(dst, src, (size_t)w * h * sizeof(int)); return; }
    int win = 2 * r + 1;
    for (int y = 0; y < h; y++) {
        long s = 0;
        for (int k = -r; k <= r; k++) s += src[(size_t)y * w + clampi(k, 0, w - 1)];
        for (int x = 0; x < w; x++) {
            tmp[(size_t)y * w + x] = (int)(s / win);
            int xo = clampi(x - r, 0, w - 1), xi = clampi(x + r + 1, 0, w - 1);
            s += src[(size_t)y * w + xi] - src[(size_t)y * w + xo];
        }
    }
    for (int x = 0; x < w; x++) {
        long s = 0;
        for (int k = -r; k <= r; k++) s += tmp[(size_t)clampi(k, 0, h - 1) * w + x];
        for (int y = 0; y < h; y++) {
            dst[(size_t)y * w + x] = (int)(s / win);
            int yo = clampi(y - r, 0, h - 1), yi = clampi(y + r + 1, 0, h - 1);
            s += tmp[(size_t)yi * w + x] - tmp[(size_t)yo * w + x];
        }
    }
    free(tmp);
}
static int op_dog(const int *p) {
    int r1 = clampi(p[0], 1, 20), r2 = clampi(p[1], 1, 40);
    if (r2 <= r1) r2 = r1 + 1;
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    int *L = (int *)malloc(n * sizeof(int)), *a = (int *)malloc(n * sizeof(int)), *b = (int *)malloc(n * sizeof(int));
    if (!L || !a || !b) { free(L); free(a); free(b); return -1; }
    for (size_t i = 0; i < n; i++) L[i] = lum(px[i]);
    boxlum(L, a, w, h, r1); boxlum(L, b, w, h, r2);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            int v = clampi(abs(a[i] - b[i]) * 4, 0, 255);
            px[i] = cblend(px[i], argb(px_a(px[i]), v, v, v), draw_cov(x, y));
        }
    free(L); free(a); free(b);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

static const studio_op_t OPS[] = {
    { "Edge-Detect", "Edge Detect", 2, {{SP_ENUM,"Method",0,2,0,"Sobel|Prewitt|Laplace"},
                                        {SP_SLIDER,"Amount %",1,400,100,0}}, op_edge },
    { "Edge-Detect", "Laplace", 1, {{SP_SLIDER,"Amount %",1,400,100,0}}, op_laplace },
    { "Edge-Detect", "Neon", 5, {{SP_SLIDER,"Glow Radius",1,8,3,0},
                                 {SP_SLIDER,"Amount %",1,400,150,0},
                                 {SP_COLOR,"Color",0,0,0x30FFFF,0},
                                 {SP_CHECK,"Use FG Color",0,1,0,0},
                                 {SP_SLIDER,"Darken %",0,100,70,0}}, op_neon },
    { "Edge-Detect", "Image Gradient", 1, {{SP_SLIDER,"Amount %",1,400,100,0}}, op_gradient },
    { "Edge-Detect", "Difference of Gaussians", 2, {{SP_SLIDER,"Radius 1",1,20,2,0},
                                                    {SP_SLIDER,"Radius 2",1,40,5,0}}, op_dog },
};

void fx_edge_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
