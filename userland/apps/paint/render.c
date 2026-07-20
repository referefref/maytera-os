// render.c - Maytera Studio Render/Generators menu (integer/fixed-point only).
// Every op registers a studio_op_t and FILLS the active drawable, blending each
// written pixel toward the generated colour by the selection coverage so a
// selection is respected. No float / libm anywhere.
#include "studio.h"
#include <stdlib.h>
#include <string.h>
#include "fx_common.inc"

// ---------------------------------------------------------------------------
// Small integer math helpers
// ---------------------------------------------------------------------------
static uint32_t rmix(uint32_t o, uint32_t n, int cov) {
    if (cov >= 255) return n;
    if (cov <= 0) return o;
    int ic = 255 - cov;
    return argb((px_a(o) * ic + px_a(n) * cov) / 255,
                (px_r(o) * ic + px_r(n) * cov) / 255,
                (px_g(o) * ic + px_g(n) * cov) / 255,
                (px_b(o) * ic + px_b(n) * cov) / 255);
}

// Q15 sine over a 1024-step circle, built once via the parabola approximation.
static int   g_sin[1024];
static int   g_sin_ready = 0;
static void sin_init(void) {
    if (g_sin_ready) return;
    // sin(t), t in [0,2pi) mapped to i in [0,1024). Parabolic approx per half.
    for (int i = 0; i < 1024; i++) {
        // x in [0,1024) -> [-512,512) relative to a half period of 512
        int half = i % 512;               // position within a half wave
        int sign = (i < 512) ? 1 : -1;
        // parabola: y = 4*half*(512-half)/512^2  in Q15
        long y = (long)4 * half * (512 - half);
        y = (y * 32767) / (512L * 512L);
        g_sin[i] = (int)(sign * y);
    }
    g_sin_ready = 1;
}
static int isin(int a) { sin_init(); return g_sin[((a % 1024) + 1024) % 1024]; } // Q15
static int icos(int a) { return isin(a + 256); }

// integer atan2 -> angle 0..1023 (full circle). Octant approximation.
static int iatan2(int y, int x) {
    if (x == 0 && y == 0) return 0;
    int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    int a;
    if (ax >= ay) {
        int r = (ax == 0) ? 0 : (ay * 128) / ax;   // 0..128 ~ 0..45deg (of 1024)
        a = r;
    } else {
        int r = (ay == 0) ? 0 : (ax * 128) / ay;
        a = 256 - r;                                 // 45..90deg
    }
    if (x < 0 && y >= 0) a = 512 - a;
    else if (x < 0 && y < 0) a = 512 + a;
    else if (x >= 0 && y < 0) a = 1024 - a;
    return a & 1023;
}
static int isqrt_i(long v) {
    if (v <= 0) return 0;
    long r = 0, b = 1L << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return (int)r;
}

// xorshift PRNG
static unsigned int xs = 0x1234567u;
static void seed_rng(unsigned int s) { xs = s ? s : 0xC0FFEEu; }
static unsigned int rng(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }

// integer value-noise: hashed lattice + smoothstep, returns 0..255
static int vhash(int x, int y, unsigned int seed) {
    unsigned int h = (unsigned int)(x * 374761393 + y * 668265263) ^ seed;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (int)((h ^ (h >> 16)) & 255);
}
static int smoothstep(int a, int b, int t256) {          // t in 0..256
    int t = (t256 * t256 * (768 - 2 * t256)) >> 16;      // 3t^2-2t^3 in 0..256
    return a + ((b - a) * t) / 256;
}
static int vnoise(int x256, int y256, unsigned int seed) {  // coords in .8 fixed
    int xi = x256 >> 8, yi = y256 >> 8, xf = x256 & 255, yf = y256 & 255;
    int a = vhash(xi, yi, seed), b = vhash(xi + 1, yi, seed);
    int c = vhash(xi, yi + 1, seed), d = vhash(xi + 1, yi + 1, seed);
    int ab = smoothstep(a, b, xf), cd = smoothstep(c, d, xf);
    return smoothstep(ab, cd, yf);
}
static int fbm(int x256, int y256, int octaves, unsigned int seed) {
    int sum = 0, amp = 128, tot = 0;
    for (int o = 0; o < octaves; o++) {
        sum += vnoise(x256, y256, seed + o * 97) * amp;
        tot += amp; amp >>= 1; x256 <<= 1; y256 <<= 1;
    }
    return tot ? sum / tot : 0;                          // 0..255
}

// simple HSV(0..1023 hue, 0..255 s/v) -> ARGB, for fractal palettes
static uint32_t hsv(int h, int s, int v) {
    h = ((h % 1024) + 1024) % 1024;
    int region = h / 171;                                // 0..5 (1024/6~171)
    int rem = (h - region * 171) * 255 / 171;
    int p = v * (255 - s) / 255;
    int q = v * (255 - s * rem / 255) / 255;
    int t = v * (255 - s * (255 - rem) / 255) / 255;
    int r, g, b;
    switch (region) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return argb(255, r, g, b);
}

// ---------------------------------------------------------------------------
// Generators
// ---------------------------------------------------------------------------
static int gen_perlin(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h();
    int detail = clampi(p[0], 1, 8);
    uint32_t c1 = (uint32_t)p[2] | 0xFF000000u, c2 = (uint32_t)p[3] | 0xFF000000u;
    int scale = 32;                                       // lattice cell in px
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int n = fbm((x << 8) / scale, (y << 8) / scale, detail, 0xA11CE);
            uint32_t c = argb(255,
                (px_r(c1) * (255 - n) + px_r(c2) * n) / 255,
                (px_g(c1) * (255 - n) + px_g(c2) * n) / 255,
                (px_b(c1) * (255 - n) + px_b(c2) * n) / 255);
            int i = y * w + x; px[i] = rmix(px[i], c, draw_cov(x, y));
        }
    (void)p; g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}
static int gen_diffclouds(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h(), detail = clampi(p[0], 1, 8), scale = 40;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int n = fbm((x << 8) / scale, (y << 8) / scale, detail, 0xC10D5);
            int i = y * w + x; uint32_t o = px[i];
            int dr = px_r(o) - n; if (dr < 0) dr = -dr;
            int dg = px_g(o) - n; if (dg < 0) dg = -dg;
            int db = px_b(o) - n; if (db < 0) db = -db;
            px[i] = rmix(o, argb(px_a(o), dr, dg, db), draw_cov(x, y));
        }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}
static int gen_plasma(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h(), turb = clampi(p[0], 1, 100);
    seed_rng((unsigned)p[1] * 2654435761u + 1u);
    int sc = 8 + turb;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int r = fbm((x << 8) / sc, (y << 8) / sc, 6, 0x1);
            int g = fbm((x << 8) / sc + 4096, (y << 8) / sc, 6, 0x2);
            int b = fbm((x << 8) / sc, (y << 8) / sc + 4096, 6, 0x3);
            int i = y * w + x; px[i] = rmix(px[i], argb(255, r, g, b), draw_cov(x, y));
        }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}
static int gen_cell(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h(), density = clampi(p[0], 2, 64), metric = p[1];
    int cell = w / density; if (cell < 4) cell = 4;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int gx = x / cell, gy = y / cell, best = 1 << 30;
            for (int oy = -1; oy <= 1; oy++)
                for (int ox = -1; ox <= 1; ox++) {
                    int cx = (gx + ox) * cell + (vhash(gx + ox, gy + oy, 7) % cell);
                    int cy = (gy + oy) * cell + (vhash(gx + ox, gy + oy, 71) % cell);
                    int dx = x - cx, dy = y - cy, d;
                    if (metric == 1) d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                    else if (metric == 2) { int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy; d = ax > ay ? ax : ay; }
                    else d = isqrt_i((long)dx * dx + (long)dy * dy);
                    if (d < best) best = d;
                }
            int v = clampi(best * 255 / cell, 0, 255);
            int i = y * w + x; px[i] = rmix(px[i], argb(255, v, v, v), draw_cov(x, y));
        }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}
static int gen_checker(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h(), sz = clampi(p[0], 2, 256);
    uint32_t c1 = (uint32_t)p[1] | 0xFF000000u, c2 = (uint32_t)p[2] | 0xFF000000u;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = (((x / sz) + (y / sz)) & 1) ? c2 : c1;
            int i = y * w + x; px[i] = rmix(px[i], c, draw_cov(x, y));
        }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}
static int gen_grid(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h(), sp = clampi(p[0], 2, 256), lw = clampi(p[1], 1, 16);
    uint32_t c = (uint32_t)p[2] | 0xFF000000u; int off = p[3];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int on = (((x + off) % sp) < lw) || (((y + off) % sp) < lw);
            if (!on) continue;
            int i = y * w + x; px[i] = rmix(px[i], c, draw_cov(x, y));
        }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}
static int gen_maze(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h(), cell = clampi(p[0], 4, 64);
    int gw = w / cell, gh = h / cell; if (gw < 1) gw = 1; if (gh < 1) gh = 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int gx = x / cell, gy = y / cell, lx = x % cell, ly = y % cell;
            // wall on left/top of each cell + a pseudo-random interior wall
            int wall = (lx == 0) || (ly == 0);
            if (!wall) {
                if (vhash(gx, gy, 42) & 1) wall = (ly == cell / 2) && (lx < cell / 2);
                else wall = (lx == cell / 2) && (ly < cell / 2);
            }
            int i = y * w + x; px[i] = rmix(px[i], wall ? argb(255,20,20,20) : argb(255,240,240,240), draw_cov(x, y));
        }
    (void)gw; (void)gh; g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}
static int gen_sinus(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h(), fx = clampi(p[0], 1, 64), fy = clampi(p[1], 1, 64);
    uint32_t c1 = (uint32_t)p[2] | 0xFF000000u, c2 = (uint32_t)p[3] | 0xFF000000u;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int a = (x * fx * 1024 / w) + (y * fy * 1024 / h);
            int s = (isin(a) + 32767) / 256;             // 0..255
            uint32_t c = argb(255,
                (px_r(c1) * (255 - s) + px_r(c2) * s) / 255,
                (px_g(c1) * (255 - s) + px_g(c2) * s) / 255,
                (px_b(c1) * (255 - s) + px_b(c2) * s) / 255);
            int i = y * w + x; px[i] = rmix(px[i], c, draw_cov(x, y));
        }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}
static const int BAYER8[8][8] = {
    {0,32,8,40,2,34,10,42},{48,16,56,24,50,18,58,26},{12,44,4,36,14,46,6,38},
    {60,28,52,20,62,30,54,22},{3,35,11,43,1,33,9,41},{51,19,59,27,49,17,57,25},
    {15,47,7,39,13,45,5,37},{63,31,55,23,61,29,53,21}};
static int gen_bayer(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h(), size = (p[0] == 0) ? 2 : (p[0] == 1) ? 4 : 8;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int t = BAYER8[y % size % 8][x % size % 8] * 4;   // 0..252
            int i = y * w + x; px[i] = rmix(px[i], argb(255, t, t, t), draw_cov(x, y));
        }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}
static int gen_spiral(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h(), turns = clampi(p[0], 1, 40);
    uint32_t c1 = (uint32_t)p[1] | 0xFF000000u, c2 = (uint32_t)p[2] | 0xFF000000u;
    int cx = w / 2, cy = h / 2, maxr = isqrt_i((long)cx * cx + (long)cy * cy);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int dx = x - cx, dy = y - cy;
            int r = isqrt_i((long)dx * dx + (long)dy * dy);
            int ang = iatan2(dy, dx);                     // 0..1023
            int phase = (ang + (maxr ? r * turns * 1024 / maxr : 0)) % 1024;
            uint32_t c = (phase < 512) ? c1 : c2;
            int i = y * w + x; px[i] = rmix(px[i], c, draw_cov(x, y));
        }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}
static int grad_t(int shape, int x, int y, int w, int h) {   // returns 0..255
    int cx = w / 2, cy = h / 2;
    switch (shape) {
        case 0: return clampi(x * 255 / (w ? w : 1), 0, 255);           // Linear
        case 1: { int dx = x - cx, dy = y - cy; int r = isqrt_i((long)dx*dx+(long)dy*dy);
                  int m = isqrt_i((long)cx*cx+(long)cy*cy); return clampi(m ? r*255/m : 0,0,255); } // Radial
        case 2: { int a = iatan2(y - cy, x - cx); return a * 255 / 1024; }  // Conical
        case 3: { int ax = x-cx<0?cx-x:x-cx, ay=y-cy<0?cy-y:y-cy; int m=ax>ay?ax:ay; int mm=cx>cy?cx:cy; return clampi(mm?m*255/mm:0,0,255);} // Square
        default: { int d = x - cx < 0 ? cx - x : x - cx; return clampi(cx ? 255 - d*255/cx : 0,0,255);} // Bilinear
    }
}
static int gen_gradient(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h(), shape = p[0], repeat = p[3];
    uint32_t c1 = (uint32_t)p[1] | 0xFF000000u, c2 = (uint32_t)p[2] | 0xFF000000u;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int t = grad_t(shape, x, y, w, h);
            if (repeat == 1) t = (t * 3) % 256;                       // Sawtooth
            else if (repeat == 2) { int q = (t * 3) % 512; t = q < 256 ? q : 511 - q; } // Triangle
            uint32_t c = argb(255,
                (px_r(c1) * (255 - t) + px_r(c2) * t) / 255,
                (px_g(c1) * (255 - t) + px_g(c2) * t) / 255,
                (px_b(c1) * (255 - t) + px_b(c2) * t) / 255);
            int i = y * w + x; px[i] = rmix(px[i], c, draw_cov(x, y));
        }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}
// Mandelbrot / Julia in Q16.16
#define FX 16
static uint32_t frac_color(int it, int maxit, int pal) {
    if (it >= maxit) return argb(255, 0, 0, 0);
    int t = it * 1023 / maxit;
    if (pal == 0) return hsv(t, 220, 255);
    if (pal == 1) { int v = it * 255 / maxit; return argb(255, v, v, 255 - v / 2); }  // fire-ish
    if (pal == 2) { int v = it * 255 / maxit; return argb(255, v / 2, v, 255 - v); }  // ocean
    int v = it * 255 / maxit; return argb(255, v, v, v);
}
static int gen_mandel(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h();
    long cx = (long)p[0] * 65536 / 100, cy = (long)p[1] * 65536 / 100;
    int zoom = clampi(p[2], 1, 4000), maxit = clampi(p[3], 16, 512), pal = p[4];
    long span = (long)3 * 65536 * 100 / zoom;             // width in Q16.16
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            long re0 = cx + (long)(x - w / 2) * span / w;
            long im0 = cy + (long)(y - h / 2) * span / w;
            long zr = 0, zi = 0; int it = 0;
            while (it < maxit) {
                long zr2 = (zr * zr) >> FX, zi2 = (zi * zi) >> FX;
                if (zr2 + zi2 > (4L << FX)) break;
                long nzi = ((zr * zi) >> (FX - 1)) + im0;
                zr = zr2 - zi2 + re0; zi = nzi; it++;
            }
            int i = y * w + x; px[i] = rmix(px[i], frac_color(it, maxit, pal), draw_cov(x, y));
        }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}
static int gen_julia(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h();
    long cr = (long)p[0] * 65536 / 100, ci = (long)p[1] * 65536 / 100;
    int maxit = clampi(p[2], 16, 512), pal = p[3];
    long span = (long)3 * 65536;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            long zr = (long)(x - w / 2) * span / w, zi = (long)(y - h / 2) * span / w;
            int it = 0;
            while (it < maxit) {
                long zr2 = (zr * zr) >> FX, zi2 = (zi * zi) >> FX;
                if (zr2 + zi2 > (4L << FX)) break;
                long nzi = ((zr * zi) >> (FX - 1)) + ci;
                zr = zr2 - zi2 + cr; zi = nzi; it++;
            }
            int i = y * w + x; px[i] = rmix(px[i], frac_color(it, maxit, pal), draw_cov(x, y));
        }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}
static int gen_jigsaw(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h(), tx = clampi(p[0], 2, 16), ty = clampi(p[1], 2, 16);
    int cw = w / tx, ch = h / ty;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int lx = cw ? x % cw : 0, ly = ch ? y % ch : 0;
            int edge = (lx < 2 || ly < 2);
            // knob bumps at mid edges
            if (!edge && cw > 8 && ch > 8) {
                if (ly < 2 + 0 && lx > cw/2-4 && lx < cw/2+4) edge = 1;
            }
            if (!edge) continue;
            int i = y * w + x; px[i] = rmix(px[i], argb(255, 40, 40, 40), draw_cov(x, y));
        }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}
static int gen_qbist(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h();
    seed_rng((unsigned)p[0] * 40503u + 7u);
    unsigned k1 = rng(), k2 = rng(), k3 = rng();
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int u = x * 256 / w, v = y * 256 / h;
            int r = (u * v + (k1 & 255)) & 255;
            int g = ((u ^ v) * ((k2 >> 3) & 7) + (k2 & 255)) & 255;
            int b = (isin((u + v) * 4 + (int)(k3 & 1023)) + 32767) / 256;
            int i = y * w + x; px[i] = rmix(px[i], argb(255, r, g, b), draw_cov(x, y));
        }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}
static int gen_lava(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h(), scale = clampi(p[0], 2, 64), rough = clampi(p[1], 1, 100);
    int oct = 2 + rough / 20;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int n = fbm((x << 8) / scale, (y << 8) / scale, oct, 0x1A7A);
            // lava ramp: black -> red -> orange -> yellow
            int r = clampi(n * 3, 0, 255), g = clampi((n - 80) * 3, 0, 255), b = clampi((n - 200) * 5, 0, 255);
            int i = y * w + x; px[i] = rmix(px[i], argb(255, r, g, b), draw_cov(x, y));
        }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}
static int gen_spyro(const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int w = draw_w(), h = draw_h();
    int type = p[0]; int R = clampi(p[1], 10, 300), r = clampi(p[2], 5, 200);
    uint32_t col = (uint32_t)p[3] | 0xFF000000u;
    int cx = w / 2, cy = h / 2;
    int d = r + (type ? r / 2 : 0);                       // pen offset
    // draw the curve by sampling t
    int prevx = -1, prevy = -1;
    for (int t = 0; t <= 1024 * 20; t += 2) {
        int a1 = t & 1023;
        int a2 = (R + r) ? (t * (R + r) / (r ? r : 1)) & 1023 : 0;
        int x = cx + ((R - r) * icos(a1)) / 32767 + (d * icos(a2)) / 32767;
        int y = cy + ((R - r) * isin(a1)) / 32767 - (d * isin(a2)) / 32767;
        if (prevx >= 0) {
            // draw line prev->cur (Bresenham-lite)
            int dx = x - prevx, dy = y - prevy, steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
            if (steps < 1) steps = 1;
            for (int s = 0; s <= steps; s++) {
                int lx = prevx + dx * s / steps, ly = prevy + dy * s / steps;
                if (lx >= 0 && ly >= 0 && lx < w && ly < h) {
                    int i = ly * w + lx; px[i] = rmix(px[i], col, draw_cov(lx, ly));
                }
            }
        }
        prevx = x; prevy = y;
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Render / Fibers: vertical fiber generator (Photoshop Filter > Render > Fibers).
// Renders with the current FG/BG tool colors (fx_edge.c precedent); set FG/BG first.
// Position-only generator: never samples drawable neighbors, so no fx_dup needed.

// One vertical clamped-window box-average pass over column x of the noise buffer.
// tmp is a caller-provided H-entry scratch so the pass reads pre-pass values.
static void fib_box_col(uint8_t *nz, int *tmp, int W, int H, int x, int R) {
    int y, sum = 0;
    int hi0 = (R < H - 1) ? R : (H - 1);
    for (y = 0; y < H; y++) tmp[y] = nz[y * W + x];
    for (y = 0; y <= hi0; y++) sum += tmp[y];             // window for y == 0
    for (y = 0; y < H; y++) {
        if (y > 0) {
            int add = y + R, rem = y - R - 1;
            if (add < H)  sum += tmp[add];
            if (rem >= 0) sum -= tmp[rem];
        }
        int lo = y - R; if (lo < 0) lo = 0;
        int hi = y + R; if (hi > H - 1) hi = H - 1;
        nz[y * W + x] = (uint8_t)(sum / (hi - lo + 1));   // actual window size
    }
}

static int op_fibers(const int *p) {
    int variance = fx_clamp(p[0], 1, 64);
    int strength = fx_clamp(p[1], 1, 64);
    unsigned seedp = (unsigned)p[2];
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px(); if (!px) return -1;

    uint8_t *nz = (uint8_t *)malloc((size_t)W * H);
    if (!nz) return -1;
    int *tmp = (int *)malloc((size_t)H * sizeof(int));
    if (!tmp) { free(nz); return -1; }

    // Per-column white noise, deterministically seeded (never rand()).
    for (int x = 0; x < W; x++) {
        unsigned s = 0x9E3779B9u ^ ((unsigned)x * 2654435761u) ^ (seedp * 40503u);
        if (!s) s = 0x9E3779B9u;                          // xorshift32 must not be 0
        for (int y = 0; y < H; y++)
            nz[y * W + x] = (uint8_t)(fx_rnd(&s) & 255);
    }

    // Stretch into vertical fibers: TWO clamped sliding-window box passes.
    // Low Variance = long smooth streaks, high Variance = short choppy fibers.
    int R = 1 + (64 - variance);
    for (int pass = 0; pass < 2; pass++)
        for (int x = 0; x < W; x++)
            fib_box_col(nz, tmp, W, H, x, R);

    // Contrast-expand for stringiness, then color from FG/BG tool colors,
    // preserving original alpha and blending by selection coverage.
    int fr = px_r(g_tool.fg), fg = px_g(g_tool.fg), fb = px_b(g_tool.fg);
    int br = px_r(g_tool.bg), bg = px_g(g_tool.bg), bb = px_b(g_tool.bg);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int v = nz[i];
        int t = fx_clamp(128 + ((v - 128) * strength) / 2, 0, 255);
        int r = (fr * t + br * (255 - t) + 127) / 255;
        int g = (fg * t + bg * (255 - t) + 127) / 255;
        int b = (fb * t + bb * (255 - t) + 127) / 255;
        uint32_t res = argb(px_a(px[i]), r, g, b);
        fx_put(px, px, i, res, cov);
    }

    free(tmp); free(nz);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
static const studio_op_t OPS[] = {
  {"Render","Solid Noise",4,{{SP_SLIDER,"Detail",1,8,4,0},{SP_CHECK,"Tileable",0,1,0,0},{SP_COLOR,"Color 1",0,0,0x000000,0},{SP_COLOR,"Color 2",0,0,0xFFFFFF,0}},gen_perlin},
  {"Render","Difference Clouds",1,{{SP_SLIDER,"Detail",1,8,5,0}},gen_diffclouds},
  {"Render","Plasma",2,{{SP_SLIDER,"Turbulence",1,100,40,0},{SP_INT,"Seed",0,9999,1,0}},gen_plasma},
  {"Render","Cell Noise",2,{{SP_SLIDER,"Density",2,64,16,0},{SP_ENUM,"Metric",0,2,0,"Euclidean|Manhattan|Chebyshev"}},gen_cell},
  {"Render","Checkerboard",3,{{SP_SLIDER,"Size",2,256,16,0},{SP_COLOR,"Color 1",0,0,0x202020,0},{SP_COLOR,"Color 2",0,0,0xE0E0E0,0}},gen_checker},
  {"Render","Grid",4,{{SP_SLIDER,"Spacing",2,256,32,0},{SP_SLIDER,"Width",1,16,1,0},{SP_COLOR,"Color",0,0,0x000000,0},{SP_INT,"Offset",0,256,0,0}},gen_grid},
  {"Render","Maze",1,{{SP_SLIDER,"Cell",4,64,16,0}},gen_maze},
  {"Render","Sinus",4,{{SP_SLIDER,"X Freq",1,64,8,0},{SP_SLIDER,"Y Freq",1,64,8,0},{SP_COLOR,"Color 1",0,0,0x1050A0,0},{SP_COLOR,"Color 2",0,0,0xF0D060,0}},gen_sinus},
  {"Render","Bayer Pattern",1,{{SP_ENUM,"Size",0,2,1,"2x2|4x4|8x8"}},gen_bayer},
  {"Render","Spiral",3,{{SP_SLIDER,"Turns",1,40,8,0},{SP_COLOR,"Color 1",0,0,0x000000,0},{SP_COLOR,"Color 2",0,0,0xFFFFFF,0}},gen_spiral},
  {"Render","Gradient Fill",4,{{SP_ENUM,"Shape",0,4,0,"Linear|Radial|Conical|Square|Bilinear"},{SP_COLOR,"Color 1",0,0,0x000000,0},{SP_COLOR,"Color 2",0,0,0xFFFFFF,0},{SP_ENUM,"Repeat",0,2,0,"None|Sawtooth|Triangle"}},gen_gradient},
  {"Render","Mandelbrot",5,{{SP_INT,"Center X",-300,300,-50,0},{SP_INT,"Center Y",-300,300,0,0},{SP_SLIDER,"Zoom",1,4000,100,0},{SP_SLIDER,"Iterations",16,512,128,0},{SP_ENUM,"Palette",0,3,0,"Rainbow|Fire|Ocean|Gray"}},gen_mandel},
  {"Render","Julia",4,{{SP_INT,"C Re",-200,200,-40,0},{SP_INT,"C Im",-200,200,60,0},{SP_SLIDER,"Iterations",16,512,128,0},{SP_ENUM,"Palette",0,3,0,"Rainbow|Fire|Ocean|Gray"}},gen_julia},
  {"Render","Jigsaw",2,{{SP_SLIDER,"Tiles X",2,16,5,0},{SP_SLIDER,"Tiles Y",2,16,4,0}},gen_jigsaw},
  {"Render","Qbist",1,{{SP_INT,"Seed",1,9999,1234,0}},gen_qbist},
  {"Render","Lava",2,{{SP_SLIDER,"Scale",2,64,24,0},{SP_SLIDER,"Roughness",1,100,60,0}},gen_lava},
  {"Render","Spyrograph",4,{{SP_ENUM,"Type",0,1,0,"Hypotrochoid|Epitrochoid"},{SP_SLIDER,"Outer R",10,300,120,0},{SP_SLIDER,"Inner r",5,200,45,0},{SP_COLOR,"Color",0,0,0x2060C0,0}},gen_spyro},
  {"Render","Fibers",3,{{SP_SLIDER,"Variance",1,64,16,0},{SP_SLIDER,"Strength",1,64,4,0},{SP_INT,"Seed",0,9999,1,0}},op_fibers},
};
void render_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
