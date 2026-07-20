// fx_blur.c - Maytera Studio "Blur" menu ops (registry). Integer/fixed-point.
#include "studio.h"
#include "fx_common.inc"

// ---- shared helpers -------------------------------------------------------
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

// Bhaskara integer sine, returns sin(deg)*1024
static int isin(int deg) {
    deg %= 360; if (deg < 0) deg += 360;
    int sign = 1; if (deg > 180) { deg -= 180; sign = -1; }
    long num = 4L * deg * (180 - deg);
    long den = 40500L - (long)deg * (180 - deg);
    if (den == 0) den = 1;
    return sign * (int)(num * 1024 / den);
}
static int icos(int deg) { return isin(deg + 90); }

static inline uint32_t at(const uint32_t *p, int w, int h, int x, int y, int wrap) {
    if (wrap) { x = ((x % w) + w) % w; y = ((y % h) + h) % h; }
    else { x = clampi(x, 0, w - 1); y = clampi(y, 0, h - 1); }
    return p[(size_t)y * w + x];
}

// One separable box blur src->dst, radius r, wrap edges or clamp.
static void boxblur(uint32_t *src, uint32_t *dst, int w, int h, int r, int wrap) {
    uint32_t *tmp = (uint32_t *)malloc((size_t)w * h * 4);
    if (!tmp) { memcpy(dst, src, (size_t)w * h * 4); return; }
    int win = 2 * r + 1;
    for (int y = 0; y < h; y++) {
        uint32_t *row = src + (size_t)y * w;
        uint32_t *o = tmp + (size_t)y * w;
        long sa = 0, sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) {
            uint32_t p = at(src, w, h, k, y, wrap);
            sa += px_a(p); sr += px_r(p); sg += px_g(p); sb += px_b(p);
        }
        for (int x = 0; x < w; x++) {
            o[x] = argb((int)(sa / win), (int)(sr / win), (int)(sg / win), (int)(sb / win));
            uint32_t po = at(src, w, h, x - r, y, wrap);
            uint32_t pi = at(src, w, h, x + r + 1, y, wrap);
            sa += px_a(pi) - px_a(po); sr += px_r(pi) - px_r(po);
            sg += px_g(pi) - px_g(po); sb += px_b(pi) - px_b(po);
            (void)row;
        }
    }
    for (int x = 0; x < w; x++) {
        long sa = 0, sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) {
            uint32_t p = at(tmp, w, h, x, k, wrap);
            sa += px_a(p); sr += px_r(p); sg += px_g(p); sb += px_b(p);
        }
        for (int y = 0; y < h; y++) {
            dst[(size_t)y * w + x] = argb((int)(sa / win), (int)(sr / win), (int)(sg / win), (int)(sb / win));
            uint32_t po = at(tmp, w, h, x, y - r, wrap);
            uint32_t pi = at(tmp, w, h, x, y + r + 1, wrap);
            sa += px_a(pi) - px_a(po); sr += px_r(pi) - px_r(po);
            sg += px_g(pi) - px_g(po); sb += px_b(pi) - px_b(po);
        }
    }
    free(tmp);
}

// Apply a box/gaussian: `passes` box passes of radius r into the drawable.
static int run_box(int r, int wrap, int passes) {
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px || r < 1) return 0;
    size_t n = (size_t)w * h;
    uint32_t *src = (uint32_t *)malloc(n * 4); if (!src) return -1;
    memcpy(src, px, n * 4);
    uint32_t *a = (uint32_t *)malloc(n * 4);
    uint32_t *b = (uint32_t *)malloc(n * 4);
    if (!a || !b) { free(src); free(a); free(b); return -1; }
    memcpy(a, src, n * 4);
    for (int i = 0; i < passes; i++) { boxblur(a, b, w, h, r, wrap); uint32_t *t = a; a = b; b = t; }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            px[i] = cblend(src[i], a[i], draw_cov(x, y));
        }
    free(src); free(a); free(b);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---- ops ------------------------------------------------------------------
static int op_gaussian(const int *p) { int r = p[0]; return run_box(clampi((r + 1) / 2, 1, 40), 0, 3); }
static int op_box(const int *p)      { return run_box(clampi(p[0], 1, 40), 0, 1); }
static int op_tileable(const int *p) { return run_box(clampi((p[0] + 1) / 2, 1, 40), 1, 3); }

// Motion blur: enum 0 Linear,1 Radial,2 Zoom
static int op_motion(const int *p) {
    int mode = p[0], len = clampi(p[1], 1, 100), ang = p[2];
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    uint32_t *src = (uint32_t *)malloc(n * 4); if (!src) return -1;
    memcpy(src, px, n * 4);
    int cx = w / 2, cy = h / 2;
    int dx1024 = icos(ang), dy1024 = isin(ang);
    int steps = len; if (steps < 1) steps = 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            long sa = 0, sr = 0, sg = 0, sb = 0; int cnt = 0;
            for (int s = -steps / 2; s <= steps / 2; s++) {
                int sx, sy;
                if (mode == 0) {               // linear
                    sx = x + (dx1024 * s) / 1024;
                    sy = y + (dy1024 * s) / 1024;
                } else if (mode == 2) {        // zoom
                    int rx = x - cx, ry = y - cy;
                    int f = 1024 + (s * 8);    // scale factor per step
                    sx = cx + (rx * f) / 1024;
                    sy = cy + (ry * f) / 1024;
                } else {                       // radial (rotate)
                    int rx = x - cx, ry = y - cy;
                    int a = s;                 // degrees offset
                    sx = cx + (rx * icos(a) - ry * isin(a)) / 1024;
                    sy = cy + (rx * isin(a) + ry * icos(a)) / 1024;
                }
                uint32_t q = at(src, w, h, sx, sy, 0);
                sa += px_a(q); sr += px_r(q); sg += px_g(q); sb += px_b(q); cnt++;
            }
            if (!cnt) cnt = 1;
            uint32_t res = argb((int)(sa / cnt), (int)(sr / cnt), (int)(sg / cnt), (int)(sb / cnt));
            size_t i = (size_t)y * w + x;
            px[i] = cblend(src[i], res, draw_cov(x, y));
        }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Median filter, per channel, radius 1..16 (cap raised from 5 per the blueprint;
// the sliding Huang histogram below keeps large radii interactive).
// Sliding-window histogram method: one 256-bin histogram per channel is kept
// per row and updated incrementally as x advances (drop the leaving column,
// add the entering column) instead of re-collecting the whole window per pixel.
// Edge handling matches the old brute-force path exactly: every window column
// coordinate is clamped independently, so out-of-image slots fold onto the
// border column and the total count stays win for every pixel (identical result).
static int op_median(const int *p) {
    int r = clampi(p[0], 1, 16);
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    uint32_t *src = (uint32_t *)malloc(n * 4); if (!src) return -1;
    memcpy(src, px, n * 4);
    int win = (2 * r + 1) * (2 * r + 1);
    int half = win / 2;
    int hr[256], hg[256], hb[256], ha[256];
    for (int y = 0; y < h; y++) {
        int y0 = y - r, y1 = y + r;
        memset(hr, 0, sizeof(hr)); memset(hg, 0, sizeof(hg));
        memset(hb, 0, sizeof(hb)); memset(ha, 0, sizeof(ha));
        // Seed the histograms with the window centred on x = 0: columns [-r..r].
        for (int cx = -r; cx <= r; cx++) {
            int sx = cx < 0 ? 0 : (cx > w - 1 ? w - 1 : cx);
            for (int cy = y0; cy <= y1; cy++) {
                int sy = cy < 0 ? 0 : (cy > h - 1 ? h - 1 : cy);
                uint32_t q = src[(size_t)sy * w + sx];
                hr[px_r(q)]++; hg[px_g(q)]++; hb[px_b(q)]++; ha[px_a(q)]++;
            }
        }
        for (int x = 0; x < w; x++) {
            if (x > 0) {
                // Advance the window: drop column (x-1-r), add column (x+r).
                int ox = x - 1 - r; ox = ox < 0 ? 0 : (ox > w - 1 ? w - 1 : ox);
                int nx = x + r;     nx = nx < 0 ? 0 : (nx > w - 1 ? w - 1 : nx);
                for (int cy = y0; cy <= y1; cy++) {
                    int sy = cy < 0 ? 0 : (cy > h - 1 ? h - 1 : cy);
                    uint32_t qo = src[(size_t)sy * w + ox];
                    uint32_t qn = src[(size_t)sy * w + nx];
                    hr[px_r(qo)]--; hg[px_g(qo)]--; hb[px_b(qo)]--; ha[px_a(qo)]--;
                    hr[px_r(qn)]++; hg[px_g(qn)]++; hb[px_b(qn)]++; ha[px_a(qn)]++;
                }
            }
            int cov = draw_cov(x, y);
            if (cov <= 0) continue;   // cblend would return the original untouched
            int mr = 0, mg = 0, mb = 0, ma = 0, c;
            c = 0; for (int v = 0; v < 256; v++) { c += hr[v]; if (c > half) { mr = v; break; } }
            c = 0; for (int v = 0; v < 256; v++) { c += hg[v]; if (c > half) { mg = v; break; } }
            c = 0; for (int v = 0; v < 256; v++) { c += hb[v]; if (c > half) { mb = v; break; } }
            c = 0; for (int v = 0; v < 256; v++) { c += ha[v]; if (c > half) { ma = v; break; } }
            size_t i = (size_t)y * w + x;
            px[i] = cblend(src[i], argb(ma, mr, mg, mb), cov);
        }
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Pixelize: block average; Block Height 0 keeps square blocks, nonzero makes
// rectangular cells (GIMP/PS Mosaic parity upgrade).
static int op_pixelize(const int *p) {
    int b = clampi(p[0], 2, 64);
    int bh = (p[1] <= 0) ? b : clampi(p[1], 2, 64);
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    uint32_t *src = (uint32_t *)malloc(n * 4); if (!src) return -1;
    memcpy(src, px, n * 4);
    for (int by = 0; by < h; by += bh)
        for (int bx = 0; bx < w; bx += b) {
            long sa = 0, sr = 0, sg = 0, sb = 0; int cnt = 0;
            for (int y = by; y < by + bh && y < h; y++)
                for (int x = bx; x < bx + b && x < w; x++) {
                    uint32_t q = src[(size_t)y * w + x];
                    sa += px_a(q); sr += px_r(q); sg += px_g(q); sb += px_b(q); cnt++;
                }
            if (!cnt) cnt = 1;
            uint32_t av = argb((int)(sa / cnt), (int)(sr / cnt), (int)(sg / cnt), (int)(sb / cnt));
            for (int y = by; y < by + bh && y < h; y++)
                for (int x = bx; x < bx + b && x < w; x++) {
                    size_t i = (size_t)y * w + x;
                    px[i] = cblend(src[i], av, draw_cov(x, y));
                }
        }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Selective gaussian: average neighbours within threshold luminance of center.
// The window compares each neighbour's luminance to the centre's, so the set of
// averaged pixels varies per centre and a colour prefix-sum cannot be reused.
// The hot path is instead hoisted: every source pixel's luminance is computed
// exactly once into a precomputed map (lm) rather than re-derived (2r+1)^2 times
// through overlapping windows. Row base pointers are cached so the inner loop
// only clamps and indexes. Results are bit-identical to the naive version.
static int op_selective(const int *p) {
    int r = clampi(p[0], 1, 12), th = clampi(p[1], 0, 255);
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    uint32_t *src = (uint32_t *)malloc(n * 4); if (!src) return -1;
    memcpy(src, px, n * 4);
    unsigned char *lm = (unsigned char *)malloc(n);   // per-pixel luminance cache
    if (!lm) { free(src); return -1; }
    for (size_t i = 0; i < n; i++) lm[i] = (unsigned char)lum(src[i]);
    for (int y = 0; y < h; y++) {
        int y0 = y - r, y1 = y + r;
        for (int x = 0; x < w; x++) {
            int cov = draw_cov(x, y);
            if (cov <= 0) continue;   // cblend would return the original untouched
            size_t ci = (size_t)y * w + x;
            uint32_t c = src[ci];
            int cl = lm[ci];
            int x0 = x - r, x1 = x + r;
            long sa = 0, sr = 0, sg = 0, sb = 0; int cnt = 0;
            for (int cy = y0; cy <= y1; cy++) {
                int sy = cy < 0 ? 0 : (cy > h - 1 ? h - 1 : cy);
                const uint32_t *srow = src + (size_t)sy * w;
                const unsigned char *lrow = lm + (size_t)sy * w;
                for (int cx = x0; cx <= x1; cx++) {
                    int sx = cx < 0 ? 0 : (cx > w - 1 ? w - 1 : cx);
                    int d = (int)lrow[sx] - cl; if (d < 0) d = -d;
                    if (d <= th) {
                        uint32_t q = srow[sx];
                        sa += px_a(q); sr += px_r(q); sg += px_g(q); sb += px_b(q); cnt++;
                    }
                }
            }
            if (!cnt) cnt = 1;
            uint32_t res = argb((int)(sa / cnt), (int)(sr / cnt), (int)(sg / cnt), (int)(sb / cnt));
            px[ci] = cblend(c, res, cov);
        }
    }
    free(lm); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---- Average (Blur > Average, oneshot) ------------------------------------
// Fills the covered area with the mean visible color, like Photoshop's
// Filter > Blur > Average. Two passes, no scratch buffer. Pass 1 accumulates
// each channel weighted by cov * alpha (so transparent pixels do not drag the
// average toward black; PS averages only visible content) in uint64_t
// accumulators: worst case 4096x4096 fully opaque is ~2.8e14, safely inside
// 64 bits. Pass 2 writes argb(px_a(orig), avg) through fx_put, so every pixel
// keeps its own original alpha (color-only op) and feathered selections both
// contribute proportionally and receive a proportional blend. In-place safe:
// pass 2 depends only on the precomputed average and the pixel's own original
// value, so no fx_dup snapshot is needed. Idempotent-safe by construction.
static int op_average(const int *p) {
    (void)p;
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px(); if (!px) return -1;
    uint64_t sr = 0, sg = 0, sb = 0, sw = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int cov = draw_cov(x, y); if (!cov) continue;
            uint32_t o = px[y * W + x];
            uint64_t w = (uint64_t)cov * (uint64_t)px_a(o);   // 0..65025
            if (!w) continue;
            sr += (uint64_t)px_r(o) * w;
            sg += (uint64_t)px_g(o) * w;
            sb += (uint64_t)px_b(o) * w;
            sw += w;
        }
    }
    if (!sw) return 0;   // empty selection or fully transparent: change nothing
    // Rounded division; truncation reads visibly darker than PS on mid tones.
    int ar = (int)((sr + sw / 2) / sw);
    int ag = (int)((sg + sw / 2) / sw);
    int ab = (int)((sb + sw / 2) / sw);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
            uint32_t res = argb(px_a(px[i]), ar, ag, ab);
            fx_put(px, px, i, res, cov);
        }
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Lens Blur: gather-convolve with a polygonal iris kernel (Photoshop Lens Blur,
// uniform depth, Source=None). Integer only; kernel built once per apply.
//
// Iris membership test for offset (dx,dy): the regular N-gon boundary radius at
// polar angle a is rpoly(a) = R * cos(180/N) / cos(((a - rot) mod (360/N)) - 180/N),
// computed in Q12 via fx_cos with sector math in 1/64-degree units so the
// non-integer sector widths (N=7) stay accurate. Blade Curvature blends the
// polygon toward a circle: redge = rpoly + (R - rpoly) * curv / 100.
static int op_lens_blur(const int *p) {
    int R      = fx_clamp(p[0], 1, 32);
    int N      = fx_clamp(p[1], 0, 5) + 3;   // Triangle=3 .. Octagon=8
    int curv   = fx_clamp(p[2], 0, 100);
    int rot    = p[3];
    int bright = fx_clamp(p[4], 0, 100);
    int thresh = fx_clamp(p[5], 0, 255);
    int W = draw_w(), H = draw_h();
    int D = 2 * R + 1;

    // Sparse iris kernel: malloc'd (dx,dy) pair list covering [-R..R]^2.
    int *ofs = (int *)malloc((size_t)D * D * 2 * sizeof(int));
    if (!ofs) return -1;
    int nofs = 0;
    int w64 = 23040 / N;                          // sector width, 1/64 deg (360*64/N)
    int coshalf = fx_cos((w64 / 2 + 32) / 64);    // Q12 cos of the half-sector angle
    for (int dy = -R; dy <= R; dy++)
        for (int dx = -R; dx <= R; dx++) {
            int a  = fx_atan2(dy, dx);                     // 0..359
            int aa = ((a - rot) % 360 + 360) % 360;
            int m  = (aa * 64) % w64 - w64 / 2;            // signed offset within sector
            int deg = m >= 0 ? (m + 32) / 64 : -((-m + 32) / 64);
            int cd = fx_cos(deg); if (cd < 1) cd = 1;      // |deg| <= 60, cd stays positive
            int rpoly = (R * coshalf) / cd;                // Q12 ratio cancels; <= R
            int redge = rpoly + ((R - rpoly) * curv) / 100;
            if (dx * dx + dy * dy <= redge * redge) {
                ofs[nofs * 2] = dx; ofs[nofs * 2 + 1] = dy; nofs++;
            }
        }
    if (nofs < 1) { ofs[0] = 0; ofs[1] = 0; nofs = 1; }    // degenerate guard

    uint32_t *src = fx_dup(W, H);
    if (!src) { free(ofs); return -1; }

    // Per-pixel Q8 gain: specular highlights bloom into bokeh discs.
    uint16_t *gain = (uint16_t *)malloc((size_t)W * H * sizeof(uint16_t));
    if (!gain) { free(src); free(ofs); return -1; }
    for (int i = 0; i < W * H; i++)
        gain[i] = (uint16_t)(fx_lum(src[i]) >= thresh ? 256 + bright * 20 : 256);

    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
            // Per-sample >>8 keeps int accumulators safe:
            // max (255 * 2256) >> 8 = 2247, times 65*65 samples = ~9.5e6.
            int sr = 0, sg = 0, sb = 0, sa = 0;
            for (int k = 0; k < nofs; k++) {
                int sx = fx_clamp(x + ofs[k * 2],     0, W - 1);
                int sy = fx_clamp(y + ofs[k * 2 + 1], 0, H - 1);
                int j = sy * W + sx;
                uint32_t q = src[j]; int g = gain[j];
                sr += (px_r(q) * g) >> 8;
                sg += (px_g(q) * g) >> 8;
                sb += (px_b(q) * g) >> 8;
                sa += px_a(q);                        // alpha ungained
            }
            // Gain can push channels past white, which is the point; clamp.
            uint32_t res = argb(sa / nofs,
                                fx_clamp(sr / nofs, 0, 255),
                                fx_clamp(sg / nofs, 0, 255),
                                fx_clamp(sb / nofs, 0, 255));
            fx_put(px, src, i, res, cov);
        }

    free(gain); free(src); free(ofs);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

/* ---- Shape Blur (Blur menu) ----------------------------------------------
   Convolves the drawable with a binary preset-shape kernel (Disc, Square,
   Diamond, Triangle, Star). The shape is rasterized once as a run list of
   horizontal segments, then gathered per pixel over per-row prefix-sum
   planes, so cost is O(kernel rows) per pixel. Integer math only. */

typedef struct { int dy, x0, x1; } sb_seg_t;

/* Inside test for the shape mask; kernel coords dx,dy in [-R..R]. */
static int sb_inside(int shape, int dx, int dy, int R) {
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    switch (shape) {
    case 1: /* Square: the [-R..R] loop bounds already are the square */
        return 1;
    case 2: /* Diamond */
        return ax + ay <= R;
    case 3: /* Triangle: apex (0,-R), base corners (-R,R/2) and (R,R/2);
               two half-plane tests, y-down screen space */
        return 2 * dy <= R && 2 * (dy + R) >= 3 * ax;
    case 4: { /* Star: 5 points, boundary radius linearly interpolated in
                 integer math between outer R and inner R*38/100 across each
                 36 degree half-sector; one point aimed at the top (270) */
        int d2 = dx * dx + dy * dy;
        int dist, inner, m, dd, bound;
        if (d2 == 0) return 1;
        dist = (int)fx_isqrt((unsigned)d2);
        inner = R * 38 / 100; if (inner < 1) inner = 1;
        m = (fx_atan2(dy, dx) - 270 + 3600) % 72;
        dd = m <= 36 ? m : 72 - m;      /* 0 at a point tip, 36 mid-sector */
        bound = inner + (R - inner) * (36 - dd) / 36;
        return dist <= bound;
    }
    default: /* Disc */
        return dx * dx + dy * dy <= R * R;
    }
}

static int op_shape_blur(const int *p) {
    int R = fx_clamp(p[0], 1, 32);                    /* defensive clamp */
    int shape = (p[1] >= 0 && p[1] <= 4) ? p[1] : 0;  /* out of range: Disc */
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px();
    int D = 2 * R + 1, maxseg = D * 8, nseg = 0;
    sb_seg_t *segs;
    uint32_t *src, *pa, *pr, *pg, *pb;
    size_t pn;
    int x, y, k;

    if (!px || W <= 0 || H <= 0) return 0;

    /* 1) rasterize the shape and compress it into horizontal run segments.
       Star rows can carry more than 2 runs (a scanline can cross up to two
       lobes per side), so allow 8 segments per row; still tiny at D <= 65. */
    segs = (sb_seg_t *)malloc((size_t)maxseg * sizeof(sb_seg_t));
    if (!segs) return -1;
    for (int dy = -R; dy <= R; dy++) {
        int run = 0, x0 = 0;
        for (int dx = -R; dx <= R + 1; dx++) {        /* +1 flushes open run */
            int in = dx <= R && sb_inside(shape, dx, dy, R);
            if (in && !run) { run = 1; x0 = dx; }
            else if (!in && run) {
                run = 0;
                if (nseg < maxseg) {
                    segs[nseg].dy = dy; segs[nseg].x0 = x0;
                    segs[nseg].x1 = dx - 1; nseg++;
                }
            }
        }
    }
    if (nseg == 0) {                                  /* cannot happen; guard */
        segs[0].dy = 0; segs[0].x0 = 0; segs[0].x1 = 0; nseg = 1;
    }

    /* 2) snapshot the layer and build four per-row prefix-sum planes, W+1
       entries per row; a full row sum is at most W*255 so uint32 is safe. */
    src = fx_dup(W, H);
    pn = (size_t)H * (size_t)(W + 1);
    pa = (uint32_t *)malloc(pn * 4);
    pr = (uint32_t *)malloc(pn * 4);
    pg = (uint32_t *)malloc(pn * 4);
    pb = (uint32_t *)malloc(pn * 4);
    if (!src || !pa || !pr || !pg || !pb) {
        free(segs); free(src); free(pa); free(pr); free(pg); free(pb);
        return -1;                                    /* nothing changed yet */
    }
    for (y = 0; y < H; y++) {
        size_t b = (size_t)y * (W + 1), r0 = (size_t)y * W;
        pa[b] = pr[b] = pg[b] = pb[b] = 0;
        for (x = 0; x < W; x++) {
            uint32_t s = src[r0 + x];
            pa[b + x + 1] = pa[b + x] + px_a(s);
            pr[b + x + 1] = pr[b + x] + px_r(s);
            pg[b + x + 1] = pg[b + x] + px_g(s);
            pb[b + x + 1] = pb[b + x] + px_b(s);
        }
    }

    /* 3) gather: per selected pixel walk the run list, row clamped to
       [0,H-1] and segment clamped to [0,W-1], and divide by the ACTUAL
       clamped sample count (edge-extend, prevents edge darkening). All four
       channels including alpha are blurred (geometric-op convention). */
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            int i = y * W + x, cov = draw_cov(x, y), n = 0;
            uint32_t sa = 0, sr = 0, sg = 0, sb = 0, res;
            if (!cov) continue;
            for (k = 0; k < nseg; k++) {
                int yy = fx_clamp(y + segs[k].dy, 0, H - 1);
                int xs = fx_clamp(x + segs[k].x0, 0, W - 1);
                int xe = fx_clamp(x + segs[k].x1, 0, W - 1);
                size_t b = (size_t)yy * (W + 1);
                sa += pa[b + xe + 1] - pa[b + xs];
                sr += pr[b + xe + 1] - pr[b + xs];
                sg += pg[b + xe + 1] - pg[b + xs];
                sb += pb[b + xe + 1] - pb[b + xs];
                n += xe - xs + 1;
            }
            res = n ? argb((int)(sa / n), (int)(sr / n), (int)(sg / n), (int)(sb / n))
                    : src[i];
            fx_put(px, src, i, res, cov);
        }

    free(segs); free(src); free(pa); free(pr); free(pg); free(pb);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---- registration ---------------------------------------------------------
static const studio_op_t OPS[] = {
    { "Blur", "Gaussian Blur", 1, {{SP_SLIDER,"Radius",1,40,6,0}}, op_gaussian },
    { "Blur", "Box Blur",      1, {{SP_SLIDER,"Radius",1,40,4,0}}, op_box },
    { "Blur", "Tileable Blur", 1, {{SP_SLIDER,"Radius",1,40,6,0}}, op_tileable },
    { "Blur", "Motion Blur",   3, {{SP_ENUM,"Type",0,2,0,"Linear|Radial|Zoom"},
                                   {SP_SLIDER,"Length",1,100,20,0},
                                   {SP_ANGLE,"Angle",0,360,0,0}}, op_motion },
    { "Blur", "Median Blur",   1, {{SP_SLIDER,"Radius",1,16,2,0}}, op_median },
    { "Blur", "Pixelize",      2, {{SP_SLIDER,"Block",2,64,8,0},{SP_INT,"Block Height",0,64,0,0}}, op_pixelize },
    { "Blur", "Selective Gaussian", 2, {{SP_SLIDER,"Radius",1,12,4,0},
                                        {SP_SLIDER,"Threshold",0,255,40,0}}, op_selective },
    { "Blur", "Average",       0, {{0,0,0,0,0,0}}, op_average },
    { "Blur", "Lens Blur", 6, {{SP_SLIDER,"Radius",1,32,8,0},
                               {SP_ENUM,"Iris Shape",0,5,3,"Triangle|Square|Pentagon|Hexagon|Heptagon|Octagon"},
                               {SP_SLIDER,"Blade Curvature",0,100,0,0},
                               {SP_ANGLE,"Rotation",0,360,0,0},
                               {SP_SLIDER,"Brightness",0,100,0,0},
                               {SP_SLIDER,"Threshold",0,255,255,0}}, op_lens_blur },
    { "Blur", "Shape Blur", 2, {{SP_SLIDER,"Radius",2,32,8,0},{SP_ENUM,"Shape",0,4,0,"Disc|Square|Diamond|Triangle|Star"}}, op_shape_blur },
};

void fx_blur_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
