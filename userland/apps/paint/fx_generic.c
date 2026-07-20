// fx_generic.c - Maytera Studio "Generic" menu ops (registry). Integer only.
// Custom Convolution (5x5 SP_KERNEL grid widget) now provides the real
// PS-style editable kernel; the older preset-enum "Convolution Matrix" row is
// kept until the integrator wires its presets to seed the SP_KERNEL grid and
// retires it (blueprint note). All other ops as specified.
#include "fx_common.inc"

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

// The old preset-only "Convolution Matrix" op (3x3 enum presets) is retired
// per the blueprint: its presets now live as starting matrices inside the
// Custom Convolution SP_KERNEL grid widget (ui.c seeds the grid + Scale/Offset).

// Dilate: per-channel max in 3x3 (radius)
static int op_dilate(const int *p) {
    int r = clampi(p[0], 1, 8);
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    uint32_t *src = (uint32_t *)malloc(n * 4); if (!src) return -1;
    memcpy(src, px, n * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int mr = 0, mg = 0, mb = 0, ma = 0;
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    uint32_t q = at(src, w, h, x + dx, y + dy);
                    if (px_r(q) > mr) mr = px_r(q);
                    if (px_g(q) > mg) mg = px_g(q);
                    if (px_b(q) > mb) mb = px_b(q);
                    if (px_a(q) > ma) ma = px_a(q);
                }
            size_t i = (size_t)y * w + x;
            px[i] = cblend(src[i], argb(ma, mr, mg, mb), draw_cov(x, y));
        }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}
// Erode: per-channel min
static int op_erode(const int *p) {
    int r = clampi(p[0], 1, 8);
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    uint32_t *src = (uint32_t *)malloc(n * 4); if (!src) return -1;
    memcpy(src, px, n * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int mr = 255, mg = 255, mb = 255, ma = 255;
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    uint32_t q = at(src, w, h, x + dx, y + dy);
                    if (px_r(q) < mr) mr = px_r(q);
                    if (px_g(q) < mg) mg = px_g(q);
                    if (px_b(q) < mb) mb = px_b(q);
                    if (px_a(q) < ma) ma = px_a(q);
                }
            size_t i = (size_t)y * w + x;
            px[i] = cblend(src[i], argb(ma, mr, mg, mb), draw_cov(x, y));
        }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Emboss: azimuth, elevation, depth
static int isin(int deg) {
    deg %= 360; if (deg < 0) deg += 360;
    int sign = 1; if (deg > 180) { deg -= 180; sign = -1; }
    long num = 4L * deg * (180 - deg), den = 40500L - (long)deg * (180 - deg);
    if (den == 0) den = 1;
    return sign * (int)(num * 1024 / den);
}
static int icos(int deg) { return isin(deg + 90); }
// integer square root via Newton's method (kept alongside fx_isqrt; existing
// callers below predate the fx_common.inc include).
static unsigned isqrtu(unsigned x) {
    if (x == 0) return 0;
    unsigned r = x, prev;
    do { prev = r; r = (r + x / r) >> 1; } while (r < prev);
    return prev;
}
static int op_emboss(const int *p) {
    int az = p[0], el = clampi(p[1], 0, 90), depth = clampi(p[2], 1, 40);
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    uint32_t *src = (uint32_t *)malloc(n * 4); if (!src) return -1;
    memcpy(src, px, n * 4);
    // Light vector L in Q10 (|L| ~= 1024) from azimuth/elevation.
    int lx = icos(az) * icos(el) / 1024, ly = isin(az) * icos(el) / 1024, lz = isin(el);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            // Sobel-ish luminance gradients define the surface normal N.
            int gx = lum(at(src, w, h, x + 1, y)) - lum(at(src, w, h, x - 1, y));
            int gy = lum(at(src, w, h, x, y + 1)) - lum(at(src, w, h, x, y - 1));
            int nx = -gx * depth, ny = -gy * depth, nz = 1024;
            long dot = (long)nx * lx + (long)ny * ly + (long)nz * lz;
            // Proper normalization: cos = (N . L) / (|N| * |L|). L is Q10, so
            // (dot / |N|) yields cos scaled by |L| ~= 1024; remap to 0..255.
            unsigned len = isqrtu((unsigned)((long)nx * nx + (long)ny * ny + (long)nz * nz));
            if (len < 1) len = 1;
            long cosv = dot / (long)len;                 // ~ [-1024, 1024]
            int v = clampi((int)(cosv * 255 / 1024), 0, 255);
            size_t i = (size_t)y * w + x;
            px[i] = cblend(src[i], argb(px_a(src[i]), v, v, v), draw_cov(x, y));
        }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Normal map from luminance heightfield
static int op_normal(const int *p) {
    int str = clampi(p[0], 1, 40);
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    uint32_t *src = (uint32_t *)malloc(n * 4); if (!src) return -1;
    memcpy(src, px, n * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int gx = lum(at(src, w, h, x + 1, y)) - lum(at(src, w, h, x - 1, y));
            int gy = lum(at(src, w, h, x, y + 1)) - lum(at(src, w, h, x, y - 1));
            int nx = -gx * str, ny = -gy * str, nz = 255;
            // |N| via integer sqrt; encode N/|N| into the 0..255 RGB range.
            unsigned m = isqrtu((unsigned)((long)nx * nx + (long)ny * ny + (long)nz * nz));
            if (m < 1) m = 1;
            int R = clampi(128 + nx * 128 / (int)m, 0, 255);
            int G = clampi(128 + ny * 128 / (int)m, 0, 255);
            int B = clampi(128 + nz * 128 / (int)m, 0, 255);
            size_t i = (size_t)y * w + x;
            px[i] = cblend(src[i], argb(px_a(src[i]), R, G, B), draw_cov(x, y));
        }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Distance map: chamfer distance from alpha==0 seeds (or image border), gray
static int op_distance(const int *p) {
    (void)p;
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    int *d = (int *)malloc(n * sizeof(int)); if (!d) return -1;
    int INF = w + h + 10, hasSeed = 0;
    for (size_t i = 0; i < n; i++) { if (px_a(px[i]) == 0) { d[i] = 0; hasSeed = 1; } else d[i] = INF; }
    if (!hasSeed) {
        for (int x = 0; x < w; x++) { d[x] = 0; d[(size_t)(h - 1) * w + x] = 0; }
        for (int y = 0; y < h; y++) { d[(size_t)y * w] = 0; d[(size_t)y * w + w - 1] = 0; }
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            if (x > 0 && d[i - 1] + 1 < d[i]) d[i] = d[i - 1] + 1;
            if (y > 0 && d[i - w] + 1 < d[i]) d[i] = d[i - w] + 1;
        }
    for (int y = h - 1; y >= 0; y--)
        for (int x = w - 1; x >= 0; x--) {
            size_t i = (size_t)y * w + x;
            if (x < w - 1 && d[i + 1] + 1 < d[i]) d[i] = d[i + 1] + 1;
            if (y < h - 1 && d[i + w] + 1 < d[i]) d[i] = d[i + w] + 1;
        }
    int mx = 1; for (size_t i = 0; i < n; i++) if (d[i] < INF && d[i] > mx) mx = d[i];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            int v = clampi(d[i] * 255 / mx, 0, 255);
            px[i] = cblend(px[i], argb(px_a(px[i]), v, v, v), draw_cov(x, y));
        }
    free(d);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// High Pass (Filter > Other > High Pass): original minus Gaussian blur,
// recentered on 50% gray. Blur is approximated with three iterated box
// blurs per axis (Kovesi construction), pure integer running sums.
// Color-only op: alpha is neither blurred nor changed.

// Horizontal box blur, window 2*r+1, edge-clamped (border replicate).
// Blurs R,G,B only; alpha is copied from the input untouched.
static void hp_box_h(const uint32_t *in, uint32_t *out, int W, int H, int r) {
    int win = 2 * r + 1;
    for (int y = 0; y < H; y++) {
        const uint32_t *row = in + (size_t)y * W;
        uint32_t *orow = out + (size_t)y * W;
        int sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) {
            uint32_t q = row[fx_clamp(k, 0, W - 1)];
            sr += px_r(q); sg += px_g(q); sb += px_b(q);
        }
        for (int x = 0; x < W; x++) {
            orow[x] = argb(px_a(row[x]), sr / win, sg / win, sb / win);
            uint32_t add = row[fx_clamp(x + r + 1, 0, W - 1)];
            uint32_t sub = row[fx_clamp(x - r, 0, W - 1)];
            sr += px_r(add) - px_r(sub);
            sg += px_g(add) - px_g(sub);
            sb += px_b(add) - px_b(sub);
        }
    }
}

// Vertical box blur, window 2*r+1, edge-clamped (border replicate).
static void hp_box_v(const uint32_t *in, uint32_t *out, int W, int H, int r) {
    int win = 2 * r + 1;
    for (int x = 0; x < W; x++) {
        int sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) {
            uint32_t q = in[(size_t)fx_clamp(k, 0, H - 1) * W + x];
            sr += px_r(q); sg += px_g(q); sb += px_b(q);
        }
        for (int y = 0; y < H; y++) {
            size_t i = (size_t)y * W + x;
            out[i] = argb(px_a(in[i]), sr / win, sg / win, sb / win);
            uint32_t add = in[(size_t)fx_clamp(y + r + 1, 0, H - 1) * W + x];
            uint32_t sub = in[(size_t)fx_clamp(y - r, 0, H - 1) * W + x];
            sr += px_r(add) - px_r(sub);
            sg += px_g(add) - px_g(sub);
            sb += px_b(add) - px_b(sub);
        }
    }
}

static int op_high_pass(const int *p) {
    int r = p[0]; if (r < 1) r = 1;
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px(); if (!px) return 0;
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *ba = (uint32_t *)malloc((size_t)W * H * 4);
    uint32_t *bb = (uint32_t *)malloc((size_t)W * H * 4);
    if (!ba || !bb) { free(ba); free(bb); free(src); return -1; }
    // Three iterated H+V box passes, ping-ponging ba/bb; final blur in bb.
    hp_box_h(src, ba, W, H, r); hp_box_v(ba, bb, W, H, r);
    hp_box_h(bb, ba, W, H, r); hp_box_v(ba, bb, W, H, r);
    hp_box_h(bb, ba, W, H, r); hp_box_v(ba, bb, W, H, r);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
            uint32_t s = src[i], b = bb[i];
            int R = fx_clamp(128 + px_r(s) - px_r(b), 0, 255);
            int G = fx_clamp(128 + px_g(s) - px_g(b), 0, 255);
            int B = fx_clamp(128 + px_b(s) - px_b(b), 0, 255);
            fx_put(px, src, i, argb(px_a(s), R, G, B), cov);
        }
    free(ba); free(bb); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Custom Convolution: PS Filter > Other > Custom. 5x5 signed kernel from the
// SP_KERNEL grid widget (state owned by ui.c, fetched via kernel_get25(); the
// SP_KERNEL slot in p[] is a placeholder and is never read, SP_CURVE pattern).
// Per channel: acc = sum(k * src), res = clamp(round(acc / Scale) + Offset).
// PS Offset is -9999..9999 but is trimmed here to -255..255: 8-bit output
// clamps, so every value outside that range is indistinguishable from the cap
// and the contract requires every in-range value to matter.

// Symmetric rounded integer division: avoids the -1 bias plain C truncation
// puts on negative accumulators (PS does not exhibit that bias). scale >= 1.
static int cc_sdiv_round(int acc, int scale) {
    return acc >= 0 ? (acc + scale / 2) / scale : -((-acc + scale / 2) / scale);
}

static int op_custom_convolution(const int *p) {
    int k[25];
    kernel_get25(k);                       // 25 cells, each -999..999; p[0] ignored
    int scale = p[1] < 1 ? 1 : p[1];       // 1..9999; clamp degenerate Reset values
    int offset = fx_clamp(p[2], -255, 255);
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H);
    if (!src) return -1;                   // OOM, nothing changed
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x, cov = draw_cov(x, y);
            if (!cov) continue;
            // Worst case |acc| = 25 * 999 * 255 ~= 6.4M, fits int32 easily.
            int ar = 0, ag = 0, ab = 0;
            for (int dy = -2; dy <= 2; dy++) {
                int sy = fx_clamp(y + dy, 0, H - 1);      // edge-replicate, PS behavior
                const uint32_t *row = src + (size_t)sy * W;
                const int *krow = k + (dy + 2) * 5;
                for (int dx = -2; dx <= 2; dx++) {
                    int kv = krow[dx + 2];
                    if (!kv) continue;                    // blank cells are 0 in PS
                    uint32_t q = row[fx_clamp(x + dx, 0, W - 1)];
                    ar += kv * px_r(q);
                    ag += kv * px_g(q);
                    ab += kv * px_b(q);
                }
            }
            int r = fx_clamp(cc_sdiv_round(ar, scale) + offset, 0, 255);
            int g = fx_clamp(cc_sdiv_round(ag, scale) + offset, 0, 255);
            int b = fx_clamp(cc_sdiv_round(ab, scale) + offset, 0, 255);
            // Alpha is NOT convolved (PS behavior): keep the source pixel's alpha.
            fx_put(px, src, i, argb(px_a(src[i]), r, g, b), cov);
        }
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Generic / NTSC Colors: Photoshop Filter > Video > NTSC Colors equivalent.
// Martindale/Paeth "hot pixel" repair (Graphics Gems): constrain the composite
// envelope Y +/- sqrt(I^2+Q^2) to the legal -20..120 IRE window (0..255 maps to
// 0..100 IRE) by reducing saturation only, never luma. Pixels already legal
// pass through bit-exact, so the op is idempotent. Do NOT clamp I and Q to
// their nominal boxes independently; that over-restricts legal colors.
// Color-only op: preserves alpha, no fx_dup needed, integer math throughout.
static int op_ntsc_colors(const int *p) {
    (void)p;                                  /* oneshot: no params */
    uint32_t *px = draw_px();
    int W = draw_w(), H = draw_h();
    if (!px) return 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int cov = draw_cov(x, y);
            if (!cov) continue;
            int i = y * W + x;
            uint32_t o = px[i];               /* read orig before computing */
            int r = px_r(o), g = px_g(o), b = px_b(o);
            /* RGB -> YIQ, Q8 matrices (Y weights match fx_lum) */
            int Y = (77 * r + 150 * g + 29 * b) >> 8;
            int I = (153 * r - 70 * g - 83 * b) >> 8;
            int Q = (54 * r - 133 * g + 79 * b) >> 8;
            /* peak chroma excursion; |I|<=153, |Q|<=133 so I*I+Q*Q fits int32 */
            int C = (int)fx_isqrt((unsigned)(I * I + Q * Q));
            /* legal if Y+C <= 306 (120 IRE) and Y-C >= -51 (-20 IRE) */
            if (C <= 0 || (Y + C <= 306 && Y - C >= -51))
                continue;                     /* in-gamut: bit-exact passthrough */
            /* desaturate by the smaller (most violated) legal ratio, Q8 */
            int s1 = ((306 - Y) << 8) / C;
            int s2 = ((Y + 51) << 8) / C;
            int s = fx_clamp(s1 < s2 ? s1 : s2, 0, 256);
            I = (I * s) >> 8;
            Q = (Q * s) >> 8;
            /* YIQ -> RGB, Q8 matrices; luma untouched */
            int R = fx_clamp(Y + ((245 * I + 159 * Q) >> 8), 0, 255);
            int G = fx_clamp(Y - ((70 * I + 166 * Q) >> 8), 0, 255);
            int B = fx_clamp(Y + ((436 * Q - 283 * I) >> 8), 0, 255);
            fx_put(px, px, i, argb(px_a(o), R, G, B), cov);
        }
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

static const studio_op_t OPS[] = {
    { "Generic", "Dilate", 1, {{SP_SLIDER,"Radius",1,8,1,0}}, op_dilate },
    { "Generic", "Erode", 1, {{SP_SLIDER,"Radius",1,8,1,0}}, op_erode },
    { "Generic", "Emboss", 3, {{SP_ANGLE,"Azimuth",0,360,30,0},
                               {SP_SLIDER,"Elevation",0,90,45,0},
                               {SP_SLIDER,"Depth",1,40,10,0}}, op_emboss },
    { "Generic", "Normal Map", 1, {{SP_SLIDER,"Strength",1,40,8,0}}, op_normal },
    { "Generic", "Distance Map", 0, {{0,0,0,0,0,0}}, op_distance },
    { "Generic", "High Pass", 1, {{SP_SLIDER,"Radius",1,100,10,0}}, op_high_pass },
    { "Generic", "Custom Convolution", 3, {{SP_KERNEL,"Kernel",0,0,0,0}, {SP_INT,"Scale",1,9999,1,0}, {SP_INT,"Offset",-255,255,0,0}}, op_custom_convolution },
    { "Generic", "NTSC Colors", 0, {{0,0,0,0,0,0}}, op_ntsc_colors },
};

void fx_generic_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
