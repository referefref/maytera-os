// fx_artistic.c - Maytera Studio "Artistic" filter category (op registry).
// Integer / fixed-point math only; all temporaries are malloc'd and freed.
#include "fx_common.inc"

// clamped luminance sample from a source buffer
static inline uint32_t a_at(const uint32_t *s, int W, int H, int x, int y) {
    x = fx_clamp(x, 0, W - 1); y = fx_clamp(y, 0, H - 1);
    return s[(size_t)y * W + x];
}
static inline int a_lum(const uint32_t *s, int W, int H, int x, int y) {
    return fx_lum(a_at(s, W, H, x, y));
}

// separable box blur over an int luminance plane (src -> dst), radius r, clamped.
static void a_boxlum(const int *src, int *dst, int W, int H, int r) {
    int *tmp = (int *)malloc((size_t)W * H * sizeof(int));
    if (!tmp) { memcpy(dst, src, (size_t)W * H * sizeof(int)); return; }
    int win = 2 * r + 1;
    for (int y = 0; y < H; y++) {
        long s = 0;
        for (int k = -r; k <= r; k++) s += src[(size_t)y * W + fx_clamp(k, 0, W - 1)];
        for (int x = 0; x < W; x++) {
            tmp[(size_t)y * W + x] = (int)(s / win);
            int xo = fx_clamp(x - r, 0, W - 1), xi = fx_clamp(x + r + 1, 0, W - 1);
            s += src[(size_t)y * W + xi] - src[(size_t)y * W + xo];
        }
    }
    for (int x = 0; x < W; x++) {
        long s = 0;
        for (int k = -r; k <= r; k++) s += tmp[(size_t)fx_clamp(k, 0, H - 1) * W + x];
        for (int y = 0; y < H; y++) {
            dst[(size_t)y * W + x] = (int)(s / win);
            int yo = fx_clamp(y - r, 0, H - 1), yi = fx_clamp(y + r + 1, 0, H - 1);
            s += tmp[(size_t)yi * W + x] - tmp[(size_t)yo * W + x];
        }
    }
    free(tmp);
}

// Oilify: per-pixel most-frequent intensity bucket in the window (16 buckets),
// output the average colour of the winning bucket.
static int op_oilify(const int *p) {
    int r = fx_clamp(p[0], 2, 8);
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int cnt[16], sr[16], sg[16], sb[16], sa[16];
        memset(cnt, 0, sizeof(cnt)); memset(sr, 0, sizeof(sr)); memset(sg, 0, sizeof(sg));
        memset(sb, 0, sizeof(sb)); memset(sa, 0, sizeof(sa));
        for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) {
            uint32_t q = a_at(src, W, H, x + dx, y + dy);
            int bkt = fx_lum(q) * 16 / 256; if (bkt > 15) bkt = 15;
            cnt[bkt]++; sr[bkt] += px_r(q); sg[bkt] += px_g(q); sb[bkt] += px_b(q); sa[bkt] += px_a(q);
        }
        int best = 0;
        for (int k = 1; k < 16; k++) if (cnt[k] > cnt[best]) best = k;
        int n = cnt[best]; if (!n) n = 1;
        uint32_t res = argb(sa[best] / n, sr[best] / n, sg[best] / n, sb[best] / n);
        fx_put(px, src, i, res, cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Cartoon: posterize to N levels, then paint dark edge lines where the sobel
// gradient magnitude exceeds a threshold.
static int op_cartoon(const int *p) {
    int levels = fx_clamp(p[0], 2, 8), thr = fx_clamp(p[1], 1, 255);
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px(); int lm1 = levels - 1; if (lm1 < 1) lm1 = 1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t c = src[i];
        int R = px_r(c) * levels / 256; if (R > lm1) R = lm1; R = R * 255 / lm1;
        int G = px_g(c) * levels / 256; if (G > lm1) G = lm1; G = G * 255 / lm1;
        int B = px_b(c) * levels / 256; if (B > lm1) B = lm1; B = B * 255 / lm1;
        int gx = -a_lum(src, W, H, x - 1, y - 1) - 2 * a_lum(src, W, H, x - 1, y) - a_lum(src, W, H, x - 1, y + 1)
                 + a_lum(src, W, H, x + 1, y - 1) + 2 * a_lum(src, W, H, x + 1, y) + a_lum(src, W, H, x + 1, y + 1);
        int gy = -a_lum(src, W, H, x - 1, y - 1) - 2 * a_lum(src, W, H, x, y - 1) - a_lum(src, W, H, x + 1, y - 1)
                 + a_lum(src, W, H, x - 1, y + 1) + 2 * a_lum(src, W, H, x, y + 1) + a_lum(src, W, H, x + 1, y + 1);
        int mag = abs(gx) + abs(gy);
        if (mag > thr) { R = R * 20 / 100; G = G * 20 / 100; B = B * 20 / 100; }
        fx_put(px, src, i, argb(px_a(c), R, G, B), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Photocopy: grayscale, adaptive threshold against a box-blurred copy. White
// background with dark strokes where a pixel is darker than its local average.
// Params reranged to the Photoshop dialog (blueprint): Detail 1..24 def 8
// amplifies the edge response, Darkness 1..50 def 25 deepens stroke coverage.
static int op_photocopy(const int *p) {
    int detail = fx_clamp(p[0], 1, 24), dark = fx_clamp(p[1], 1, 50);
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    size_t n = (size_t)W * H;
    int *L = (int *)malloc(n * sizeof(int)), *B = (int *)malloc(n * sizeof(int));
    if (!L || !B) { free(L); free(B); return -1; }
    for (size_t i = 0; i < n; i++) L[i] = fx_lum(px[i]);
    a_boxlum(L, B, W, H, 8);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int d = B[i] - L[i]; if (d < 0) d = 0;             // how much darker than neighbourhood
        int amt = d * (detail + 4) / 3;                    // Detail amplifies edge response
        amt = amt * (dark * 2 + 20) / 70;                  // Darkness 25 ~ unity gain
        int v = 255 - fx_clamp(amt, 0, 255);               // white bg, dark strokes
        fx_put(px, px, i, argb(px_a(px[i]), v, v, v), cov);
    }
    free(L); free(B); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Apply Canvas: multiplicative procedural canvas-weave relief (period 8), depth.
static int op_canvas(const int *p) {
    int depth = fx_clamp(p[0], 1, 50);
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int cell = ((x >> 2) & 1) ^ ((y >> 2) & 1);        // 4x4 checker of thread direction
        int hgt = cell ? fx_sin((x & 3) * 360 / 4 + 45)    // Q12 -4096..4096
                       : fx_sin((y & 3) * 360 / 4 + 45);
        int fac = 256 + hgt * depth / 4096;                // multiplicative relief
        uint32_t o = px[i];
        int R = fx_clamp(px_r(o) * fac / 256, 0, 255);
        int G = fx_clamp(px_g(o) * fac / 256, 0, 255);
        int B = fx_clamp(px_b(o) * fac / 256, 0, 255);
        fx_put(px, px, i, argb(px_a(o), R, G, B), cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Glass Tile: within each tile, pull the sample toward the tile centre to fake
// per-tile refraction (a grid of little glass blocks).
static int op_glasstile(const int *p) {
    int t = fx_clamp(p[0], 4, 64);
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int lx = x % t - t / 2;                            // local offset within tile
        int ly = y % t - t / 2;
        int sx = ((x - lx / 2) << 8);                      // compress toward tile centre
        int sy = ((y - ly / 2) << 8);
        fx_put(px, src, i, fx_sample(src, W, H, sx, sy), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// rounded ribbon relief: 0 at ribbon edges, +peak at ribbon centre, Q12-ish.
static int a_ribbon(int pos, int rw) {
    int hgt = fx_sin(pos * 180 / (rw ? rw : 1));           // 0..4096 across the ribbon
    return (hgt - 2048) * 2;                               // -4096..4096, lit centre
}

// Weave: procedural over/under ribbon lattice shading multiplied over the image.
static int op_weave(const int *p) {
    int rw = fx_clamp(p[0], 2, 32), gap = fx_clamp(p[1], 0, 16);
    int period = rw + gap; if (period < 1) period = 1;
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int pxi = x % period, pyi = y % period;
        int on_v = pxi < rw, on_h = pyi < rw;
        int over_v = ((x / period + y / period) & 1);      // which ribbon is on top here
        int fac;
        if (on_v && on_h) {
            int r = over_v ? a_ribbon(pxi, rw) : a_ribbon(pyi, rw);
            fac = 256 + r * 90 / 4096;                     // over ribbon keeps its relief
        } else if (on_v) {
            fac = 256 + a_ribbon(pxi, rw) * 90 / 4096;
        } else if (on_h) {
            fac = 256 + a_ribbon(pyi, rw) * 90 / 4096;
        } else {
            fac = 256 - 70;                                // gap between ribbons -> shadow
        }
        uint32_t o = px[i];
        int R = fx_clamp(px_r(o) * fac / 256, 0, 255);
        int G = fx_clamp(px_g(o) * fac / 256, 0, 255);
        int B = fx_clamp(px_b(o) * fac / 256, 0, 255);
        fx_put(px, px, i, argb(px_a(o), R, G, B), cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Colored Pencil: blur-difference stroke mask over background-colored paper.
// Detail |L - box(L)| keeps the original color along edges as diagonal
// crosshatch strokes; flat regions fall to paper. Photoshop uses the CURRENT
// background color as the paper (no SP_COLOR param, contract A6/B1); Paper
// Brightness scales it (0 = black paper, 50 = full bg brightness).
static int op_colored_pencil(const int *p) {
    int pw   = fx_clamp(p[0], 1, 24);              // Pencil Width: blur radius, half hatch period
    int gain = fx_clamp(p[1], 0, 15) * 17;         // Stroke Pressure 0..15 -> gain 0..255
    int pb   = fx_clamp(p[2], 0, 50) * 256 / 50;   // Paper Brightness -> 0..256 scale
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    int *L = (int *)malloc((size_t)W * H * sizeof(int));
    int *B = (int *)malloc((size_t)W * H * sizeof(int));
    if (!L || !B) { free(L); free(B); free(src); return -1; }
    for (int i = 0; i < W * H; i++) L[i] = fx_lum(src[i]);
    a_boxlum(L, B, W, H, pw);                      // separable running-sum box blur, edge-clamped
    int paper_r = (px_r(g_tool.bg) * pb) >> 8;
    int paper_g = (px_g(g_tool.bg) * pb) >> 8;
    int paper_b = (px_b(g_tool.bg) * pb) >> 8;
    int period = 2 * pw;                           // pw >= 1, so period >= 2
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int d = L[i] - B[i]; if (d < 0) d = -d;    // detail term drives the stroke mask
        int ph = (x + y) % period;                 // Q8 triangle wave on the 45-degree diagonal
        int tri = ph < pw ? ph * 255 / pw : (period - ph) * 255 / pw;
        unsigned rs = 0xC01DF00Du ^ (unsigned)(y * W + x);   // coordinate-seeded, deterministic
        int hatch = (tri >> 2) + (int)(fx_rnd(&rs) & 31u);   // stroke texture + small grain
        int m = fx_clamp(((d * gain) >> 6) - hatch, 0, 255); // pressure 0 -> m == 0 (pure paper)
        uint32_t o = src[i];
        int rr = (px_r(o) * m + paper_r * (255 - m) + 127) / 255;
        int gg = (px_g(o) * m + paper_g * (255 - m) + 127) / 255;
        int bb = (px_b(o) * m + paper_b * (255 - m) + 127) / 255;
        fx_put(px, src, i, argb(px_a(o), rr, gg, bb), cov);  // color-only op: alpha preserved
    }
    free(B); free(L); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Artistic / Cutout: Photoshop-style paper-cutout. Blur a luminance copy
// (Edge Simplicity widens the box, Edge Fidelity = fewer passes since higher
// fidelity means edges truer to the source), quantize into 2..8 luminance
// bands, then flat-fill each band with the mean color of the ORIGINAL pixels
// in that band so hues stay faithful.

// 1D sliding-window box blur, edge-clamped; out must not alias line.
static void cutout_box_line(const int *line, int *out, int n, int r) {
    int win = 2 * r + 1;
    long sum = 0;
    for (int j = -r; j <= r; j++) sum += line[fx_clamp(j, 0, n - 1)];
    for (int x = 0; x < n; x++) {
        out[x] = (int)(sum / win);
        sum += line[fx_clamp(x + r + 1, 0, n - 1)];
        sum -= line[fx_clamp(x - r, 0, n - 1)];
    }
}

static int op_cutout(const int *p) {
    int levels = fx_clamp(p[0], 2, 8);
    int simp = fx_clamp(p[1], 0, 10);
    int fid = fx_clamp(p[2], 1, 3);
    int passes = 4 - fid;                     // fidelity 3 = 1 pass, 1 = 3 passes
    int radius = 1 + 2 * simp;
    int W = draw_w(), H = draw_h();
    int n = W > H ? W : H;

    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    int *lum = (int *)malloc((size_t)W * H * sizeof(int));
    int *line = (int *)malloc((size_t)n * 2 * sizeof(int));
    if (!lum || !line) { free(line); free(lum); free(src); return -1; }
    int *lout = line + n;

    for (int i = 0; i < W * H; i++) lum[i] = fx_lum(src[i]);

    if (simp > 0) {
        for (int pass = 0; pass < passes; pass++) {
            for (int y = 0; y < H; y++) {     // horizontal
                int *row = lum + y * W;
                for (int x = 0; x < W; x++) line[x] = row[x];
                cutout_box_line(line, row, W, radius);
            }
            for (int x = 0; x < W; x++) {     // vertical
                for (int y = 0; y < H; y++) line[y] = lum[y * W + x];
                cutout_box_line(line, lout, H, radius);
                for (int y = 0; y < H; y++) lum[y * W + x] = lout[y];
            }
        }
    }

    // Pass 1: per-band sums of the ORIGINAL colors (covered pixels only).
    uint64_t rs[8], gs[8], bs[8], cnt[8];
    for (int b = 0; b < 8; b++) rs[b] = gs[b] = bs[b] = cnt[b] = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x;
        if (!draw_cov(x, y)) continue;
        int b = fx_clamp((lum[i] * levels) >> 8, 0, levels - 1);
        uint32_t o = src[i];
        rs[b] += (uint64_t)px_r(o); gs[b] += (uint64_t)px_g(o);
        bs[b] += (uint64_t)px_b(o); cnt[b]++;
    }
    int br[8], bg[8], bb[8];
    for (int b = 0; b < levels; b++) {
        if (cnt[b]) {
            br[b] = (int)(rs[b] / cnt[b]);
            bg[b] = (int)(gs[b] / cnt[b]);
            bb[b] = (int)(bs[b] / cnt[b]);
        } else {                              // empty band: band-center gray
            int g = (255 * (2 * b + 1)) / (2 * levels);
            br[b] = bg[b] = bb[b] = g;
        }
    }

    // Pass 2: flat fill; color-only op, alpha preserved from the original.
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int b = fx_clamp((lum[i] * levels) >> 8, 0, levels - 1);
        uint32_t res = argb(px_a(src[i]), br[b], bg[b], bb[b]);
        fx_put(px, src, i, res, cov);
    }

    free(line); free(lum); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

/* Artistic / Dry Brush: luminance-bucket mode filter (GIMP Oilify family),
 * all integer. Brush Detail is inverse flattening (more detail = more
 * buckets = less flattening); Texture jitters the bucketing only, so
 * stroke edges roughen without full-frame noise; Texture 1 is a clean
 * zero-jitter path. Color-only op: alpha comes from the center source
 * pixel. Pure function of (restored drawable, p); preview-loop safe. */
static int op_dry_brush(const int *p) {
    int size = fx_clamp(p[0], 0, 10);
    int detail = fx_clamp(p[1], 0, 10);
    int tex = fx_clamp(p[2], 1, 3);
    int r = 1 + (size + 1) / 2;   /* 1..6: Brush Size 0 still smooths */
    int L = 4 + detail;           /* 4..14 luminance buckets */
    int J = 6 * (tex - 1);        /* bucket jitter amplitude; 0 at Texture 1 */
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int cnt[16], rs[16], gs[16], bs[16];
        for (int q = 0; q < L; q++) { cnt[q] = 0; rs[q] = 0; gs[q] = 0; bs[q] = 0; }
        for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) {
            int nx = fx_clamp(x + dx, 0, W - 1), ny = fx_clamp(y + dy, 0, H - 1);
            uint32_t np = src[ny * W + nx];
            int lum = fx_lum(np);
            if (J) {
                unsigned seed = 0x9E3779B9u ^ (unsigned)(nx * 73856093) ^ (unsigned)(ny * 19349663);
                lum += (int)(fx_rnd(&seed) % (unsigned)(2 * J + 1)) - J;
            }
            int q = (fx_clamp(lum, 0, 255) * L) >> 8;
            cnt[q]++; rs[q] += px_r(np); gs[q] += px_g(np); bs[q] += px_b(np);
        }
        int best = 0;
        for (int q = 1; q < L; q++) if (cnt[q] > cnt[best]) best = q;
        int n = cnt[best];  /* window includes the center pixel, so n >= 1 */
        uint32_t res = argb(px_a(src[i]), rs[best] / n, gs[best] / n, bs[best] / n);
        fx_put(px, src, i, res, cov);
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Film Grain (Photoshop Filter Gallery parity). Color-only op: no fx_dup,
// alpha preserved. Monochromatic grain (one sample added equally to R/G/B,
// never per-channel, so no chroma speckle) plus an Intensity-weighted
// highlight wash that also damps the grain inside blown highlights.
// Deterministic: seed is a fixed constant XOR lattice-hashed coordinates,
// never rand() and never carried RNG state, so live preview never crawls.
// In-place fx_put(px, px, ...) is safe: fx_put reads orig[i] before it
// writes dst[i], and each pixel is read fully before its write.
static int op_film_grain(const int *p) {
    int G = fx_clamp(p[0], 0, 20);            /* Grain: noise amplitude only */
    int T = 255 - fx_clamp(p[1], 0, 20) * 8;  /* Highlight Area: threshold 255..95 */
    int inten = fx_clamp(p[2], 0, 10);        /* Intensity: wash strength only */
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px();
    unsigned span = (unsigned)(12 * G + 1);   /* n in [-6G, +6G], max +/-120 */
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t o = px[i];
        int r = px_r(o), g = px_g(o), b = px_b(o);
        unsigned s = 0x9E3779B9u ^ (unsigned)(x * 73856093) ^ (unsigned)(y * 19349663);
        int n = (int)(fx_rnd(&s) % span) - 6 * G;
        int w = 0;                            /* highlight lift weight, Q8 */
        if (T < 255) {                        /* HA 0 leaves highlights untouched */
            int lum = fx_lum(o);
            if (lum > T)
                w = fx_clamp(((lum - T) * 256 / (256 - T)) * inten / 10, 0, 256);
        }
        if (w) {
            r += ((255 - r) * w) >> 8;        /* push toward white */
            g += ((255 - g) * w) >> 8;
            b += ((255 - b) * w) >> 8;
            n = (n * (256 - w)) >> 8;         /* grain damped in highlights */
        }
        r = fx_clamp(r + n, 0, 255);
        g = fx_clamp(g + n, 0, 255);
        b = fx_clamp(b + n, 0, 255);
        fx_put(px, px, i, argb(px_a(o), r, g, b), cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Fresco: oilify-style mode filter producing dark, clumpy daubs. Pass 1
// smudges each pixel to the mean colour of the most-populated luminance
// bucket in its window (low Brush Detail = fewer buckets = broader daubs).
// Pass 2 darkens daub boundaries; that edge pass is the Fresco signature
// (without it the output reads as Dry Brush). Pass 3 adds subtle monochrome
// grain scaled by Texture. Colour-only op: alpha preserved from the source.
static int op_fresco(const int *p) {
    int r = fx_clamp(1 + p[0], 1, 11);       // Brush Size 0 still smudges
    int L = fx_clamp(4 + 2 * p[1], 4, 24);   // low Detail merges colours coarsely
    int tex = fx_clamp(p[2], 1, 3);
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *sm = (uint32_t *)malloc((size_t)W * H * 4);
    if (!sm) { free(src); return -1; }
    uint32_t *px = draw_px();
    // Pass 1: windowed luminance-bucket mode filter into sm[].
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x;
        if (!draw_cov(x, y)) { sm[i] = src[i]; continue; }
        int cnt[24], sr[24], sg[24], sb[24];
        memset(cnt, 0, sizeof(cnt)); memset(sr, 0, sizeof(sr));
        memset(sg, 0, sizeof(sg)); memset(sb, 0, sizeof(sb));
        for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) {
            uint32_t q = a_at(src, W, H, x + dx, y + dy);
            int bkt = fx_lum(q) * L / 256; if (bkt >= L) bkt = L - 1;
            cnt[bkt]++; sr[bkt] += px_r(q); sg[bkt] += px_g(q); sb[bkt] += px_b(q);
        }
        int best = 0;
        for (int k = 1; k < L; k++) if (cnt[k] > cnt[best]) best = k;
        int n = cnt[best] ? cnt[best] : 1;
        sm[i] = argb(px_a(src[i]), sr[best] / n, sg[best] / n, sb[best] / n);
    }
    // Pass 2 + 3: darken daub boundaries, then add coordinate-seeded grain.
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int lc = fx_lum(sm[i]);
        int e, edge = 0;
        e = lc - a_lum(sm, W, H, x - 1, y); edge += e < 0 ? -e : e;
        e = lc - a_lum(sm, W, H, x + 1, y); edge += e < 0 ? -e : e;
        e = lc - a_lum(sm, W, H, x, y - 1); edge += e < 0 ? -e : e;
        e = lc - a_lum(sm, W, H, x, y + 1); edge += e < 0 ? -e : e;
        int dk = edge * 3; if (dk > 160) dk = 160;
        int gain = 256 - dk;                 // blackens daub boundaries
        int rr = (px_r(sm[i]) * gain) >> 8;
        int gg = (px_g(sm[i]) * gain) >> 8;
        int bb = (px_b(sm[i]) * gain) >> 8;
        unsigned s = 0x9E3779B9u ^ (unsigned)i;
        int gr = (((int)(fx_rnd(&s) & 31) - 16) * (2 * tex)) >> 4;
        rr = fx_clamp(rr + gr, 0, 255);
        gg = fx_clamp(gg + gr, 0, 255);
        bb = fx_clamp(bb + gr, 0, 255);
        fx_put(px, src, i, argb(px_a(src[i]), rr, gg, bb), cov);
    }
    free(sm); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Neon Glow: PS-style three-color remap. Desaturate, map shadows to the
// current FG color and highlights to the current BG color (toolbox colors,
// fx_edge.c precedent), then inject the dialog's Glow Color in a luminance
// band. Negative Glow Size moves the band into the shadows, positive into
// the highlights. Color-only op: alpha preserved, no fx_dup needed; one
// malloc'd W*H byte luminance plane gives the 3x3 pre-blur that softens
// the banding PS's slight pre-blur produces.
static int op_neon_glow(const int *p) {
    int size   = fx_clamp(p[0], -24, 24);      // glow band center offset
    int bright = fx_clamp(p[1], 0, 50);        // 0 = pure fg-to-bg duotone
    int gr = (p[2] >> 16) & 255, gg = (p[2] >> 8) & 255, gb = p[2] & 255;
    int fr = px_r(g_tool.fg), fg = px_g(g_tool.fg), fb = px_b(g_tool.fg);
    int br = px_r(g_tool.bg), bg = px_g(g_tool.bg), bb = px_b(g_tool.bg);
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    size_t n = (size_t)W * H;
    unsigned char *L = (unsigned char *)malloc(n);
    if (!L) return -1;
    for (size_t k = 0; k < n; k++) L[k] = (unsigned char)fx_lum(px[k]);
    int Lc = 128 + size * 5;                   // band center, 8..248
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int sum = 0;                           // 3x3 box average, edge-clamped
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
            sum += L[(size_t)fx_clamp(y + dy, 0, H - 1) * W + fx_clamp(x + dx, 0, W - 1)];
        int lum = sum / 9;
        int R = fr + ((br - fr) * lum) / 255;  // fg-to-bg duotone base
        int G = fg + ((bg - fg) * lum) / 255;
        int B = fb + ((bb - fb) * lum) / 255;
        int d = lum - Lc; if (d < 0) d = -d;
        int w = fx_clamp(255 - d * 255 / 96, 0, 255);
        w = w * bright / 50;                   // brightness scales the band
        R += ((gr - R) * w) / 255;
        G += ((gg - G) * w) / 255;
        B += ((gb - B) * w) / 255;
        fx_put(px, px, i, argb(px_a(px[i]), R, G, B), cov);
    }
    free(L);
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Palette Knife: smeared flat patches. For each pixel, scan a square window
// of radius R = Stroke Size / 2 in the snapshot (subsampled at stride R/6 to
// bound cost at large sizes), bucket samples by luminance into 8 * Detail
// bins (8/16/24; fewer bins = flatter, knife-like patches), and output the
// mean RGB of the modal bin (ties break to the lower bin for stable flats).
// Softness feathers patch boundaries with a capped 3x3 box blend, never a
// full blur. Color-only op: alpha preserved from the source pixel.
static int op_palette_knife(const int *p) {
    int size = fx_clamp(p[0], 3, 50);
    int R = size / 2; if (R < 1) R = 1;
    int B = 8 * fx_clamp(p[1], 1, 3);          /* 8, 16 or 24 luminance bins */
    int soft = fx_clamp(p[2], 0, 10);
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *out = (uint32_t *)malloc((size_t)W * H * 4);
    if (!out) { free(src); return -1; }
    uint32_t *px = draw_px();
    int s = R / 6; if (s < 1) s = 1;           /* window subsample stride */
    // Pass 1: modal-bin result per covered pixel into the scratch buffer;
    // uncovered pixels keep the source so the soften pass has sane neighbors.
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x;
        if (!draw_cov(x, y)) { out[i] = src[i]; continue; }
        int cnt[24], sr[24], sg[24], sb[24];
        memset(cnt, 0, sizeof(cnt)); memset(sr, 0, sizeof(sr));
        memset(sg, 0, sizeof(sg)); memset(sb, 0, sizeof(sb));
        for (int dy = -R; dy <= R; dy += s) for (int dx = -R; dx <= R; dx += s) {
            int qx = fx_clamp(x + dx, 0, W - 1), qy = fx_clamp(y + dy, 0, H - 1);
            uint32_t q = src[(size_t)qy * W + qx];
            int bkt = fx_lum(q) * B / 256; if (bkt > B - 1) bkt = B - 1;
            cnt[bkt]++; sr[bkt] += px_r(q); sg[bkt] += px_g(q); sb[bkt] += px_b(q);
        }
        int best = 0;                          /* strict > keeps ties on the lower bin */
        for (int k = 1; k < B; k++) if (cnt[k] > cnt[best]) best = k;
        int n = cnt[best]; if (!n) n = 1;
        out[i] = argb(px_a(src[i]), sr[best] / n, sg[best] / n, sb[best] / n);
    }
    // Pass 2: optional boundary feathering (blend toward the 3x3 box average
    // of the unblended scratch, weight soft*25 in Q8, max 250), then commit.
    int w = soft * 25;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t res = out[i];
        if (w > 0) {
            int ar = 0, ag = 0, ab = 0;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int qx = fx_clamp(x + dx, 0, W - 1), qy = fx_clamp(y + dy, 0, H - 1);
                uint32_t q = out[(size_t)qy * W + qx];
                ar += px_r(q); ag += px_g(q); ab += px_b(q);
            }
            ar /= 9; ag /= 9; ab /= 9;
            int rr = (px_r(res) * (256 - w) + ar * w) >> 8;
            int gg = (px_g(res) * (256 - w) + ag * w) >> 8;
            int bb = (px_b(res) * (256 - w) + ab * w) >> 8;
            res = argb(px_a(res), rr, gg, bb);
        }
        fx_put(px, src, i, res, cov);
    }
    free(out); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Artistic / Plastic Wrap: neutral specular sheen along smoothed luminance
// contours, screened onto the untouched image (PS Filter Gallery port).

// separable clamped-edge box blur passes over an 8-bit plane (running sum)
static void pw_box_h(unsigned char *dst, const unsigned char *s, int W, int H, int r) {
    int n = 2 * r + 1;
    for (int y = 0; y < H; y++) {
        const unsigned char *row = s + y * W;
        int sum = 0;
        for (int k = -r; k <= r; k++) sum += row[fx_clamp(k, 0, W - 1)];
        for (int x = 0; x < W; x++) {
            dst[y * W + x] = (unsigned char)(sum / n);
            sum += row[fx_clamp(x + r + 1, 0, W - 1)] - row[fx_clamp(x - r, 0, W - 1)];
        }
    }
}
static void pw_box_v(unsigned char *dst, const unsigned char *s, int W, int H, int r) {
    int n = 2 * r + 1;
    for (int x = 0; x < W; x++) {
        int sum = 0;
        for (int k = -r; k <= r; k++) sum += s[fx_clamp(k, 0, H - 1) * W + x];
        for (int y = 0; y < H; y++) {
            dst[y * W + x] = (unsigned char)(sum / n);
            sum += s[fx_clamp(y + r + 1, 0, H - 1) * W + x] - s[fx_clamp(y - r, 0, H - 1) * W + x];
        }
    }
}

static int op_plastic_wrap(const int *p) {
    int hs = fx_clamp(p[0], 0, 20);            // Highlight Strength (0 = near-noop)
    int st = fx_clamp(16 - p[1], 1, 15);       // Detail: finer step = more ridges
    int r  = fx_clamp(p[2], 1, 15);            // Smoothness: film blur radius
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    unsigned char *hf  = (unsigned char *)malloc((size_t)W * H);
    unsigned char *tmp = (unsigned char *)malloc((size_t)W * H);
    if (!hf || !tmp) { free(tmp); free(hf); free(src); return -1; }
    uint32_t *px = draw_px();
    // height field: luminance plane smoothed by a separable box blur;
    // this is the "plastic film surface" the highlights ride on
    for (int i = 0; i < W * H; i++) hf[i] = (unsigned char)fx_lum(src[i]);
    pw_box_h(tmp, hf, W, H, r);
    pw_box_v(hf, tmp, W, H, r);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int xm = fx_clamp(x - st, 0, W - 1), xp = fx_clamp(x + st, 0, W - 1);
        int ym = fx_clamp(y - st, 0, H - 1), yp = fx_clamp(y + st, 0, H - 1);
        int gx = hf[y * W + xp] - hf[y * W + xm];
        int gy = hf[yp * W + x] - hf[ym * W + x];
        int lap = 4 * hf[i] - hf[y * W + xm] - hf[y * W + xp]
                            - hf[ym * W + x] - hf[yp * W + x];
        if (lap < 0) lap = 0;
        int m = fx_clamp(((int)fx_isqrt((unsigned)(gx * gx + gy * gy)) + lap) * hs >> 4, 0, 255);
        int spec = (m * m) >> 8;               // gloss-sharpen: only strong contours shine
        uint32_t o = src[i];
        int rr = px_r(o) + (((255 - px_r(o)) * spec) >> 8);   // screen white sheen,
        int gg = px_g(o) + (((255 - px_g(o)) * spec) >> 8);   // never darken the base
        int bb = px_b(o) + (((255 - px_b(o)) * spec) >> 8);
        fx_put(px, src, i, argb(px_a(o), rr, gg, bb), cov);
    }
    free(tmp); free(hf); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Poster Edges: posterize colors to a few levels and lay near-black strokes
// along edges detected on the ORIGINAL luminance (never the posterized output).
// Color-only op: alpha preserved; all scratch malloc'd per call.

// clamped read from a uint8 plane
static inline int a_lum8(const unsigned char *l, int W, int H, int x, int y) {
    x = fx_clamp(x, 0, W - 1); y = fx_clamp(y, 0, H - 1);
    return l[(size_t)y * W + x];
}

static int op_poster_edges(const int *p) {
    int thick = fx_clamp(p[0], 0, 10);                 // Edge Thickness (dilate radius)
    int inten = fx_clamp(p[1], 0, 10);                 // Edge Intensity
    int L = fx_clamp(p[2], 0, 6) + 2;                  // Posterization: 2..8 levels (low = harsher)
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    unsigned char *lum  = (unsigned char *)malloc((size_t)W * H);
    unsigned char *edge = (unsigned char *)malloc((size_t)W * H);
    if (!lum || !edge) { free(lum); free(edge); free(src); return -1; }

    // Pass 1a: luminance plane of the snapshot
    for (int i = 0; i < W * H; i++) lum[i] = (unsigned char)fx_lum(src[i]);

    // Pass 1b: Sobel edge map; higher intensity keeps fainter edges
    int T = 112 - 8 * inten;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int gx = -a_lum8(lum, W, H, x - 1, y - 1) - 2 * a_lum8(lum, W, H, x - 1, y) - a_lum8(lum, W, H, x - 1, y + 1)
                 + a_lum8(lum, W, H, x + 1, y - 1) + 2 * a_lum8(lum, W, H, x + 1, y) + a_lum8(lum, W, H, x + 1, y + 1);
        int gy = -a_lum8(lum, W, H, x - 1, y - 1) - 2 * a_lum8(lum, W, H, x, y - 1) - a_lum8(lum, W, H, x + 1, y - 1)
                 + a_lum8(lum, W, H, x - 1, y + 1) + 2 * a_lum8(lum, W, H, x, y + 1) + a_lum8(lum, W, H, x + 1, y + 1);
        int mag = (int)fx_isqrt((unsigned)(gx * gx + gy * gy));
        edge[(size_t)y * W + x] = (unsigned char)fx_clamp((mag - T) * 3, 0, 255);
    }

    // Pass 1c: thicken strokes, separable running max of radius thick
    // (radius 0 keeps thin 1px edges; it widens strokes, it does not gate them)
    if (thick > 0) {
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {   // horizontal into lum
            int m = 0;
            for (int dx = -thick; dx <= thick; dx++) {
                int v = edge[(size_t)y * W + fx_clamp(x + dx, 0, W - 1)];
                if (v > m) m = v;
            }
            lum[(size_t)y * W + x] = (unsigned char)m;
        }
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {   // vertical back into edge
            int m = 0;
            for (int dy = -thick; dy <= thick; dy++) {
                int v = lum[(size_t)fx_clamp(y + dy, 0, H - 1) * W + x];
                if (v > m) m = v;
            }
            edge[(size_t)y * W + x] = (unsigned char)m;
        }
    }

    // Posterize LUT built once per apply; per-pixel loop is LUT + shifts only
    unsigned char lut[256];
    int lm1 = L - 1; if (lm1 < 1) lm1 = 1;
    for (int c = 0; c < 256; c++) {
        int q = (c * L) >> 8; if (q > lm1) q = lm1;
        lut[c] = (unsigned char)(q * 255 / lm1);
    }
    int deep = 96 + 20 * inten;                        // stroke darkness scale

    // Pass 2: posterize, then multiplicatively darken by the edge map
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t o = src[i];
        int e = edge[i];
        int f = 256 - ((e * deep) >> 8);
        f = fx_clamp(f, 0, 256);
        int R = (lut[px_r(o)] * f) >> 8;
        int G = (lut[px_g(o)] * f) >> 8;
        int B = (lut[px_b(o)] * f) >> 8;
        fx_put(px, src, i, argb(px_a(o), R, G, B), cov);
    }

    free(lum); free(edge); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Shared procedural texture height for the four Texturizer-panel filters
// (Rough Pastels, Underpainting, Conte Crayon, Texturizer). Belongs in
// fx_common.inc; guarded so duplicate copies pasted into one file coexist.
// Enum order Brick=0 Burlap=1 Canvas=2 Sandstone=3; returns height 0..255
// at layer coordinates (x,y) with Scaling in percent (50..200).
// SINGLE canonical copy in this file: both op_rough_pastels and
// op_underpainting call it so the two filters render the texture identically.
#ifndef FX_TEXTURE_HEIGHT_DEFINED
#define FX_TEXTURE_HEIGHT_DEFINED
static int fx_tex_hash8(int x, int y) {
    unsigned s = 0x5D2A1F3Bu ^ ((unsigned)x * 73856093u) ^ ((unsigned)y * 19349663u);
    return (int)(fx_rnd(&s) & 255);
}
static int fx_texture_height(int type, int scaling, int x, int y) {
    scaling = fx_clamp(scaling, 50, 200);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    int sx = x * 100 / scaling, sy = y * 100 / scaling;
    if (type == 0) {                                    // Brick: 24x12 cells, 2px mortar
        int bw = 24, bh = 12, row = sy / bh;
        int ox = sx + ((row & 1) ? bw / 2 : 0);
        int bx = ox % bw, by = sy % bh;
        if (bx < 2 || by < 2) return 60;                // recessed mortar line
        return 150 + fx_tex_hash8(ox / bw, row) * 60 / 255;  // per-brick tint 150..210
    }
    if (type == 1) {                                    // Burlap: coarse weave + grain
        int s12 = fx_sin(sx * 30) + fx_sin(sy * 30);    // period 12 px
        int n = fx_tex_hash8(sx, sy);
        return fx_clamp(128 + s12 * 80 / 8192 + (n - 128) / 3, 0, 255);
    }
    if (type == 3) {                                    // Sandstone: 2x2-averaged grain
        return (fx_tex_hash8(sx, sy) + fx_tex_hash8(sx + 1, sy) +
                fx_tex_hash8(sx, sy + 1) + fx_tex_hash8(sx + 1, sy + 1)) >> 2;
    }
    int wx = fx_sin(sx * 45), wy = fx_sin(sy * 45);     // Canvas: period 8 px weave
    int h = 128 + (wx + wy) * 96 / 8192;
    if (wx > 0 && wy > 0) h += 24;                      // thread crossings (Q12 threshold)
    else if (wx < 0 && wy < 0) h -= 24;
    return fx_clamp(h, 0, 255);
}
#endif

// ---- Rough Pastels (Artistic) --------------------------------------------
// Chalk strokes dragged along a fixed 45 degree diagonal, then relief-shaded
// by a procedural paper texture (shared Texturizer-panel height function
// fx_texture_height above).

#define RP_SEED 0x52505354u

static int op_rough_pastels(const int *p) {
    int len    = fx_clamp(p[0], 0, 40);        // Stroke Length
    int det    = fx_clamp(p[1], 1, 20);        // Stroke Detail (Kd)
    int tex    = fx_clamp(p[2], 0, 3);         // Brick|Burlap|Canvas|Sandstone
    int scal   = p[3]; if (scal < 50) scal = 50; if (scal > 200) scal = 200;
    int relief = fx_clamp(p[4], 0, 50);
    int ang    = p[5];                         // y-down screen degrees; 90 = Bottom
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int J = fx_clamp(21 - det, 1, 20);         // perpendicular jitter magnitude
    int lx = fx_cos(ang), ly = fx_sin(ang);    // Q12 light vector
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t orig = src[i];
        // Pass 1: chalk stroke. Walk k = 0..len along the fixed 45 degree
        // diagonal (dx = +256, dy = -256 in .8 fp), jitter each step
        // perpendicular (the +1,+1 direction), keep the brightest sample:
        // pastel drags the brightest pigment.
        uint32_t best = orig; int bestl = fx_lum(orig);
        for (int k = 0; k <= len; k++) {
            unsigned s = RP_SEED ^ (unsigned)(y * W + x) ^ (unsigned)k;
            int j = (int)(fx_rnd(&s) % (unsigned)(2 * J + 1)) - J;
            uint32_t smp = fx_sample(src, W, H, (x + k + j) << 8, (y - k + j) << 8);
            int l = fx_lum(smp);
            if (l > bestl) { bestl = l; best = smp; }
        }
        // High Stroke Detail preserves the image; Length 0 is a near-noop.
        int r = (px_r(best) * (20 - det) + px_r(orig) * det) / 20;
        int g = (px_g(best) * (20 - det) + px_g(orig) * det) / 20;
        int b = (px_b(best) * (20 - det) + px_b(orig) * det) / 20;
        // Pass 2: texturize. Central-difference gradient of the procedural
        // height (clamped coords), shaded by the Q12 light vector.
        int xm = x > 0 ? x - 1 : 0,     xp = x < W - 1 ? x + 1 : W - 1;
        int ym = y > 0 ? y - 1 : 0,     yp = y < H - 1 ? y + 1 : H - 1;
        int gx = fx_texture_height(tex, scal, xp, y) - fx_texture_height(tex, scal, xm, y);
        int gy = fx_texture_height(tex, scal, x, yp) - fx_texture_height(tex, scal, x, ym);
        int shade = (int)(((long)(gx * lx + gy * ly) * relief) >> 14);
        r = fx_clamp(r + shade, 0, 255);
        g = fx_clamp(g + shade, 0, 255);
        b = fx_clamp(b + shade, 0, 255);
        fx_put(px, src, i, argb(px_a(orig), r, g, b), cov);
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Smudge Stick: directional dark smear along Photoshop's fixed short
// down-left diagonal, plus a highlight lift. Color-preserving (per-RGB
// min-toward-dark blend, alpha kept from the source pixel), all integer.
static int op_smudge_stick(const int *p) {
    int L = fx_clamp(p[0], 0, 10);              // Stroke Length: diagonal sample count
    int h = fx_clamp(p[1], 0, 20);              // Highlight Area
    int w = 96 + 16 * fx_clamp(p[2], 0, 10);    // Intensity 0..10 -> smear weight 96..256
    int T = 255 - 6 * h;                        // highlight threshold (h=0: T=255)
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t c = src[i];
        int cr = px_r(c), cg = px_g(c), cb = px_b(c);
        int mr = cr, mg = cg, mb = cb;          // k=0 sample is the center itself
        for (int k = 1; k <= L; k++) {          // clamped samples at (x-k, y+k)
            uint32_t q = a_at(src, W, H, x - k, y + k);
            int qr = px_r(q), qg = px_g(q), qb = px_b(q);
            if (qr < mr) mr = qr;
            if (qg < mg) mg = qg;
            if (qb < mb) mb = qb;
        }
        // min-toward-dark smear, not an average: charcoal-like streaking
        int r = (mr * w + cr * (256 - w)) >> 8;
        int g = (mg * w + cg * (256 - w)) >> 8;
        int b = (mb * w + cb * (256 - w)) >> 8;
        if (h > 0) {
            int lum = fx_lum(c);
            if (lum >= T) {
                // fade the smear back toward the original in the bright band
                int f = ((lum - T) * 256) / (256 - T + 1);   // Q8, 0..256
                r += ((cr - r) * f) >> 8;
                g += ((cg - g) * f) >> 8;
                b += ((cb - b) * f) >> 8;
                // then brighten: larger Highlight Area lifts a wider band harder
                int boost = ((lum - T) * (32 + 8 * h)) >> 8;
                r = fx_clamp(r + boost, 0, 255);
                g = fx_clamp(g + boost, 0, 255);
                b = fx_clamp(b + boost, 0, 255);
            }
        }
        fx_put(px, src, i, argb(px_a(c), r, g, b), cov);
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Artistic / Sponge: sponge dabs = local box average of the source, modulated
// by a coordinate-seeded value-noise blotch field that is contrast-stretched
// around mid-gray by Definition (signed, so blotches darken AND lighten and
// the image does not dim as Definition rises). Deterministic per pixel: the
// noise lattice is seeded purely from lattice coordinates, no state carried
// between preview calls.
static int sponge_node(int gx, int gy) {
    unsigned s = 0x53504E47u ^ ((unsigned)gx * 73856093u) ^ ((unsigned)gy * 19349663u);
    return (int)(fx_rnd(&s) & 0xFF);
}

static int op_sponge(const int *p) {
    int R = fx_clamp(p[0], 0, 10);        // Brush Size: box radius, 0 = passthrough dab
    int D = fx_clamp(p[1], 0, 25);        // Definition: blotch contrast + depth, 0 = plain average
    int S = fx_clamp(p[2], 1, 15);        // Smoothness: clamp >= 1, Reset/preview can hand edge values
    int C = S + 2;                        // noise lattice cell size in pixels
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        // sponge dab color: (2R+1)^2 box average of the snapshot, edges clamped
        int sr = 0, sg = 0, sb = 0, n = 0;
        for (int dy = -R; dy <= R; dy++) {
            int sy = fx_clamp(y + dy, 0, H - 1);
            for (int dx = -R; dx <= R; dx++) {
                int sx = fx_clamp(x + dx, 0, W - 1);
                uint32_t q = src[sy * W + sx];
                sr += px_r(q); sg += px_g(q); sb += px_b(q); n++;
            }
        }
        sr /= n; sg /= n; sb /= n;
        // blotch field: bilinear value noise between the 4 surrounding lattice nodes
        int gx = x / C, gy = y / C;
        int tx = ((x - gx * C) * 256) / C, ty = ((y - gy * C) * 256) / C;
        int N = fx_bilin_ch(sponge_node(gx, gy),     sponge_node(gx + 1, gy),
                            sponge_node(gx, gy + 1), sponge_node(gx + 1, gy + 1), tx, ty);
        // contrast-stretch around mid-gray so Definition sharpens blotch edges
        int Nc = fx_clamp(128 + (((N - 128) * (64 + D * 12)) >> 6), 0, 255);
        int delta = ((Nc - 128) * D * 5) >> 7;      // signed, about +-124 at D=25, 0 at D=0
        uint32_t o = src[i];
        uint32_t res = argb(px_a(o),
                            fx_clamp(sr - delta, 0, 255),
                            fx_clamp(sg - delta, 0, 255),
                            fx_clamp(sb - delta, 0, 255));
        fx_put(px, src, i, res, cov);
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

/* ---- Artistic / Underpainting ---------------------------------------- */

// clamped-edge horizontal box blur of RGB (alpha copied through), running sums
static void up_box_h(const uint32_t *s, uint32_t *d, int W, int H, int r) {
    int n = 2 * r + 1;
    for (int y = 0; y < H; y++) {
        const uint32_t *row = s + (size_t)y * W; uint32_t *drow = d + (size_t)y * W;
        int sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) {
            uint32_t v = row[fx_clamp(k, 0, W - 1)];
            sr += px_r(v); sg += px_g(v); sb += px_b(v);
        }
        for (int x = 0; x < W; x++) {
            drow[x] = argb(px_a(row[x]), sr / n, sg / n, sb / n);
            uint32_t add = row[fx_clamp(x + r + 1, 0, W - 1)];
            uint32_t sub = row[fx_clamp(x - r, 0, W - 1)];
            sr += px_r(add) - px_r(sub); sg += px_g(add) - px_g(sub); sb += px_b(add) - px_b(sub);
        }
    }
}
// clamped-edge vertical box blur of RGB (alpha copied through), running sums
static void up_box_v(const uint32_t *s, uint32_t *d, int W, int H, int r) {
    int n = 2 * r + 1;
    for (int x = 0; x < W; x++) {
        int sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) {
            uint32_t v = s[(size_t)fx_clamp(k, 0, H - 1) * W + x];
            sr += px_r(v); sg += px_g(v); sb += px_b(v);
        }
        for (int y = 0; y < H; y++) {
            d[(size_t)y * W + x] = argb(px_a(s[(size_t)y * W + x]), sr / n, sg / n, sb / n);
            uint32_t add = s[(size_t)fx_clamp(y + r + 1, 0, H - 1) * W + x];
            uint32_t sub = s[(size_t)fx_clamp(y - r, 0, H - 1) * W + x];
            sr += px_r(add) - px_r(sub); sg += px_g(add) - px_g(sub); sb += px_b(add) - px_b(sub);
        }
    }
}

static int op_underpainting(const int *p) {
    int r = 1 + fx_clamp(p[0], 0, 40) / 6; if (r < 1) r = 1;   // brush size -> blur radius
    int w = fx_clamp(p[1], 0, 40) * 255 / 40;                   // texture coverage weight Q8
    int tex = fx_clamp(p[2], 0, 3);
    int scaling = fx_clamp(p[3], 50, 200);
    int relief = fx_clamp(p[4], 0, 50);
    int lx = fx_cos(p[5]), ly = fx_sin(p[5]);                   // light vector, Q12, y-down
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *ta = (uint32_t *)malloc((size_t)W * H * 4);
    uint32_t *tb = (uint32_t *)malloc((size_t)W * H * 4);
    if (!ta || !tb) { free(ta); free(tb); free(src); return -1; }
    up_box_h(src, ta, W, H, r); up_box_v(ta, tb, W, H, r);      // box blur pass 1
    up_box_h(tb, ta, W, H, r);  up_box_v(ta, tb, W, H, r);      // pass 2: pseudo-Gaussian
    free(ta);
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t base = tb[i];
        int gx = fx_texture_height(tex, scaling, x + 1, y) - fx_texture_height(tex, scaling, x - 1, y);
        int gy = fx_texture_height(tex, scaling, x, y + 1) - fx_texture_height(tex, scaling, x, y - 1);
        int shade = ((gx * lx + gy * ly) * relief) >> 14;
        int d = (shade * w) >> 8;
        uint32_t res = argb(px_a(src[i]),                       // color-only: keep alpha
                            fx_clamp(px_r(base) + d, 0, 255),
                            fx_clamp(px_g(base) + d, 0, 255),
                            fx_clamp(px_b(base) + d, 0, 255));
        fx_put(px, src, i, res, cov);
    }
    free(tb); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Artistic / Watercolor: luminance-keyed whole-pixel median daubs, aggressive
// shadow pooling, coarse midtone paper grain. Integer only, per PS ranges.
static int op_watercolor(const int *p) {
    int detail = fx_clamp(p[0], 1, 14);            // Brush Detail 1..14
    int si     = fx_clamp(p[1], 0, 10);            // Shadow Intensity 0..10
    int tex    = fx_clamp(p[2], 1, 3);             // Texture 1..3 (1 = off)
    int r = fx_clamp(1 + (14 - detail) / 5, 1, 3); // detail 14->r=1, 9->r=2, 1->r=3
    int amp = (tex - 1) * 6;                       // grain amplitude, 0 at tex=1
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        // Gather the clamped (2r+1)^2 window, insertion-sorted by luminance;
        // take the WHOLE ARGB of the median-luminance sample (per-channel
        // medians invent colors, so never split channels here).
        uint32_t win[49]; int lum[49]; int n = 0;
        for (int dy = -r; dy <= r; dy++) {
            int sy = fx_clamp(y + dy, 0, H - 1);
            for (int dx = -r; dx <= r; dx++) {
                int sx = fx_clamp(x + dx, 0, W - 1);
                uint32_t s = src[sy * W + sx];
                int L = fx_lum(s), j = n++;
                while (j > 0 && lum[j - 1] > L) {
                    lum[j] = lum[j - 1]; win[j] = win[j - 1]; j--;
                }
                lum[j] = L; win[j] = s;
            }
        }
        uint32_t med = win[n / 2];
        int rr = px_r(med), gg = px_g(med), bb = px_b(med);
        // Shadow pass: Q8 factor, exact no-op at si=0, crushes hard above ~4
        // (floor 64 reproduces PS's aggressive rolloff, not a linear dim).
        if (si > 0) {
            int L = lum[n / 2];
            int f = fx_clamp(256 - (si * (255 - L) * 3) / 32, 64, 256);
            rr = (rr * f) >> 8; gg = (gg * f) >> 8; bb = (bb * f) >> 8;
        }
        // Texture pass: 2x2-block paper grain, deterministic per block so the
        // preview loop (re-run on the pristine layer each tick) never crawls;
        // midtone-weighted so grain stays out of blacks and whites.
        if (amp > 0) {
            unsigned seed = 0x57A7C0DEu ^ (unsigned)((x >> 1) * 73856093)
                                        ^ (unsigned)((y >> 1) * 19349663);
            if (!seed) seed = 0x6D2B79F5u;         // xorshift must not start at 0
            int nz = (int)(fx_rnd(&seed) % (unsigned)(2 * amp + 1)) - amp;
            int L = (rr * 77 + gg * 150 + bb * 29) >> 8;
            int d = L - 128; if (d < 0) d = -d;
            int w = 256 - fx_clamp(d * 2, 0, 256);
            int add = (nz * w) >> 8;
            rr = fx_clamp(rr + add, 0, 255);
            gg = fx_clamp(gg + add, 0, 255);
            bb = fx_clamp(bb + add, 0, 255);
        }
        // Color-only op: keep the original pixel's alpha.
        fx_put(px, src, i, argb(px_a(src[i]), rr, gg, bb), cov);
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

static const studio_op_t OPS[] = {
    {"Artistic","Oilify",1,{{SP_SLIDER,"Radius",2,8,4}},op_oilify},
    {"Artistic","Cartoon",2,{{SP_SLIDER,"Levels",2,8,4},{SP_SLIDER,"Edge Threshold",1,255,40}},op_cartoon},
    {"Artistic","Photocopy",2,{{SP_SLIDER,"Detail",1,24,8},{SP_SLIDER,"Darkness",1,50,25}},op_photocopy},
    {"Artistic","Apply Canvas",1,{{SP_SLIDER,"Depth",1,50,12}},op_canvas},
    {"Artistic","Glass Tile",1,{{SP_SLIDER,"Tile Size",4,64,16}},op_glasstile},
    {"Artistic","Weave",2,{{SP_SLIDER,"Ribbon Width",2,32,10},{SP_SLIDER,"Spacing",0,16,4}},op_weave},
    {"Artistic","Colored Pencil",3,{{SP_SLIDER,"Pencil Width",1,24,4},{SP_SLIDER,"Stroke Pressure",0,15,8},{SP_SLIDER,"Paper Brightness",0,50,25}},op_colored_pencil},
    {"Artistic","Cutout",3,{{SP_SLIDER,"Levels",2,8,4},{SP_SLIDER,"Edge Simplicity",0,10,4},{SP_SLIDER,"Edge Fidelity",1,3,2}},op_cutout},
    {"Artistic","Dry Brush",3,{{SP_SLIDER,"Brush Size",0,10,2},{SP_SLIDER,"Brush Detail",0,10,8},{SP_INT,"Texture",1,3,1}},op_dry_brush},
    {"Artistic","Film Grain",3,{{SP_SLIDER,"Grain",0,20,4},{SP_SLIDER,"Highlight Area",0,20,8},{SP_SLIDER,"Intensity",0,10,10}},op_film_grain},
    {"Artistic","Fresco",3,{{SP_SLIDER,"Brush Size",0,10,2},{SP_SLIDER,"Brush Detail",0,10,8},{SP_INT,"Texture",1,3,1}},op_fresco},
    {"Artistic","Neon Glow",3,{{SP_SLIDER,"Glow Size",-24,24,5},{SP_SLIDER,"Glow Brightness",0,50,15},{SP_COLOR,"Glow Color",0,0,0x0080FF}},op_neon_glow},
    {"Artistic","Palette Knife",3,{{SP_SLIDER,"Stroke Size",3,50,25},{SP_SLIDER,"Stroke Detail",1,3,3},{SP_SLIDER,"Softness",0,10,0}},op_palette_knife},
    {"Artistic","Plastic Wrap",3,{{SP_SLIDER,"Highlight Strength",0,20,15},{SP_SLIDER,"Detail",1,15,9},{SP_SLIDER,"Smoothness",1,15,7}},op_plastic_wrap},
    {"Artistic","Poster Edges",3,{{SP_SLIDER,"Edge Thickness",0,10,2},{SP_SLIDER,"Edge Intensity",0,10,1},{SP_SLIDER,"Posterization",0,6,2}},op_poster_edges},
    {"Artistic","Rough Pastels",6,{{SP_SLIDER,"Stroke Length",0,40,6},{SP_SLIDER,"Stroke Detail",1,20,4},{SP_ENUM,"Texture",0,3,2,"Brick|Burlap|Canvas|Sandstone"},{SP_SLIDER,"Scaling",50,200,100},{SP_SLIDER,"Relief",0,50,20},{SP_ANGLE,"Light Direction",0,360,90}},op_rough_pastels},
    {"Artistic","Smudge Stick",3,{{SP_SLIDER,"Stroke Length",0,10,2},{SP_SLIDER,"Highlight Area",0,20,0},{SP_SLIDER,"Intensity",0,10,10}},op_smudge_stick},
    {"Artistic","Sponge",3,{{SP_SLIDER,"Brush Size",0,10,2},{SP_SLIDER,"Definition",0,25,12},{SP_SLIDER,"Smoothness",1,15,5}},op_sponge},
    {"Artistic","Underpainting",6,{{SP_SLIDER,"Brush Size",0,40,6},{SP_SLIDER,"Texture Coverage",0,40,16},{SP_ENUM,"Texture",0,3,2,"Brick|Burlap|Canvas|Sandstone"},{SP_SLIDER,"Scaling",50,200,100},{SP_SLIDER,"Relief",0,50,4},{SP_ANGLE,"Light Direction",0,360,225}},op_underpainting},
    {"Artistic","Watercolor",3,{{SP_SLIDER,"Brush Detail",1,14,9},{SP_SLIDER,"Shadow Intensity",0,10,1},{SP_INT,"Texture",1,3,1}},op_watercolor},
};
void fx_artistic_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
