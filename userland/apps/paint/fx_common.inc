// fx_common.inc - static helpers shared by the fx_*.c category files (each file
// #includes this so all symbols stay file-static; no cross-file duplicate defs).
#include "studio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"

static inline int fx_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// xorshift32 PRNG (no rand()); seed per-call from a fixed base ^ index.
static inline unsigned fx_rnd(unsigned *s) {
    unsigned x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x;
}

// integer isqrt
static unsigned fx_isqrt(unsigned x) {
    unsigned r = 0, b = 1u << 30;
    while (b > x) b >>= 2;
    while (b) { if (x >= r + b) { x -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}

// Bhaskara integer sine/cosine, returns Q12 in [-4096,4096]. Arg in degrees.
static int fx_sin(int deg) {
    deg %= 360; if (deg < 0) deg += 360;
    int sign = 1; if (deg > 180) { deg -= 180; sign = -1; }
    long num = 4L * deg * (180 - deg);
    long den = 40500L - (long)deg * (180 - deg);
    if (den == 0) den = 1;
    return (int)(sign * (num * 4096 / den));
}
static int fx_cos(int deg) { return fx_sin(deg + 90); }

// atan2 in degrees (0..359), integer. dy,dx any range.
static int fx_atan2(int dy, int dx) {
    if (dx == 0 && dy == 0) return 0;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    long a;                                   // octant approximation, 0..45 deg
    if (ax >= ay) a = (long)ay * 45 / (ax ? ax : 1);
    else          a = 90 - (long)ax * 45 / (ay ? ay : 1);
    int ang = (int)a;
    if (dx < 0 && dy >= 0) ang = 180 - ang;
    else if (dx < 0 && dy < 0) ang = 180 + ang;
    else if (dx >= 0 && dy < 0) ang = 360 - ang;
    return ang % 360;
}

static int fx_bilin_ch(int A, int B, int C, int D, int tx, int ty) {
    int top = A + (((B - A) * tx) >> 8);
    int bot = C + (((D - C) * tx) >> 8);
    return top + (((bot - top) * ty) >> 8);
}
// sample ARGB source at fixed-point .8 coordinates (clamped)
static uint32_t fx_sample(const uint32_t *s, int W, int H, int fx, int fy) {
    int x = fx >> 8, y = fy >> 8, tx = fx & 255, ty = fy & 255;
    int x1 = fx_clamp(x, 0, W - 1), y1 = fx_clamp(y, 0, H - 1);
    int x2 = fx_clamp(x + 1, 0, W - 1), y2 = fx_clamp(y + 1, 0, H - 1);
    uint32_t a = s[y1 * W + x1], b = s[y1 * W + x2], c = s[y2 * W + x1], d = s[y2 * W + x2];
    int rr = fx_bilin_ch(px_r(a), px_r(b), px_r(c), px_r(d), tx, ty);
    int gg = fx_bilin_ch(px_g(a), px_g(b), px_g(c), px_g(d), tx, ty);
    int bb = fx_bilin_ch(px_b(a), px_b(b), px_b(c), px_b(d), tx, ty);
    int aa = fx_bilin_ch(px_a(a), px_a(b), px_a(c), px_a(d), tx, ty);
    return argb(aa, rr, gg, bb);
}

// write result blended with the original by selection coverage (0..255)
static void fx_put(uint32_t *dst, const uint32_t *orig, int i, uint32_t res, int cov) {
    if (cov <= 0) return;
    if (cov >= 255) { dst[i] = res; return; }
    uint32_t o = orig[i];
    int r = px_r(o) + (((px_r(res) - px_r(o)) * cov) >> 8);
    int g = px_g(o) + (((px_g(res) - px_g(o)) * cov) >> 8);
    int b = px_b(o) + (((px_b(res) - px_b(o)) * cov) >> 8);
    int a = px_a(o) + (((px_a(res) - px_a(o)) * cov) >> 8);
    dst[i] = argb(a, r, g, b);
}
// duplicate the active drawable; caller frees. Returns NULL on OOM.
static uint32_t *fx_dup(int W, int H) {
    uint32_t *px = draw_px(); if (!px) return (uint32_t *)0;
    uint32_t *s = (uint32_t *)malloc((size_t)W * H * 4);
    if (s) memcpy(s, px, (size_t)W * H * 4);
    return s;
}
static inline int fx_lum(uint32_t p) { return (px_r(p) * 77 + px_g(p) * 150 + px_b(p) * 29) >> 8; }
