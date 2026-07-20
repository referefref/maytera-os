// fx_enhance.c - Maytera Studio "Enhance" menu ops (registry). Integer only.
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
// box blur src->dst (clamp)
static void boxblur(uint32_t *src, uint32_t *dst, int w, int h, int r) {
    uint32_t *tmp = (uint32_t *)malloc((size_t)w * h * 4);
    if (!tmp) { memcpy(dst, src, (size_t)w * h * 4); return; }
    int win = 2 * r + 1;
    for (int y = 0; y < h; y++) {
        long sr = 0, sg = 0, sb = 0, sa = 0;
        for (int k = -r; k <= r; k++) { uint32_t p = at(src, w, h, k, y); sr += px_r(p); sg += px_g(p); sb += px_b(p); sa += px_a(p); }
        for (int x = 0; x < w; x++) {
            tmp[(size_t)y * w + x] = argb((int)(sa / win), (int)(sr / win), (int)(sg / win), (int)(sb / win));
            uint32_t po = at(src, w, h, x - r, y), pi = at(src, w, h, x + r + 1, y);
            sr += px_r(pi) - px_r(po); sg += px_g(pi) - px_g(po); sb += px_b(pi) - px_b(po); sa += px_a(pi) - px_a(po);
        }
    }
    for (int x = 0; x < w; x++) {
        long sr = 0, sg = 0, sb = 0, sa = 0;
        for (int k = -r; k <= r; k++) { uint32_t p = at(tmp, w, h, x, k); sr += px_r(p); sg += px_g(p); sb += px_b(p); sa += px_a(p); }
        for (int y = 0; y < h; y++) {
            dst[(size_t)y * w + x] = argb((int)(sa / win), (int)(sr / win), (int)(sg / win), (int)(sb / win));
            uint32_t po = at(tmp, w, h, x, y - r), pi = at(tmp, w, h, x, y + r + 1);
            sr += px_r(pi) - px_r(po); sg += px_g(pi) - px_g(po); sb += px_b(pi) - px_b(po); sa += px_a(pi) - px_a(po);
        }
    }
    free(tmp);
}

// Unsharp mask: p0 radius, p1 amount(%), p2 threshold
static int op_unsharp(const int *p) {
    int r = clampi(p[0], 1, 20), amt = clampi(p[1], 0, 300), th = clampi(p[2], 0, 255);
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    uint32_t *src = (uint32_t *)malloc(n * 4), *blur = (uint32_t *)malloc(n * 4);
    if (!src || !blur) { free(src); free(blur); return -1; }
    memcpy(src, px, n * 4);
    boxblur(src, blur, w, h, r);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            uint32_t o = src[i], bl = blur[i];
            int dr = px_r(o) - px_r(bl), dg = px_g(o) - px_g(bl), db = px_b(o) - px_b(bl);
            int mag = (abs(dr) + abs(dg) + abs(db)) / 3;
            uint32_t res = o;
            if (mag >= th) {
                int R = clampi(px_r(o) + dr * amt / 100, 0, 255);
                int G = clampi(px_g(o) + dg * amt / 100, 0, 255);
                int B = clampi(px_b(o) + db * amt / 100, 0, 255);
                res = argb(px_a(o), R, G, B);
            }
            px[i] = cblend(o, res, draw_cov(x, y));
        }
    free(src); free(blur);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Sharpen: shared 4-neighbor Laplacian pass (fx_lap4_sharpen, defined below)
// per the blueprint's "Sharpen family shares kernel constants" rule. Q8 gain
// K = Amount * 2 / 3 so the default Amount 60 lands exactly on the calibrated
// K = 40 (Sharpen More = 96, Sharpen Edges = Sobel-gated full kernel).
static void fx_lap4_sharpen(uint32_t *px, const uint32_t *src, int W, int H, int K);
static int op_sharpen(const int *p) {
    int amt = clampi(p[0], 0, 255);
    int W = draw_w(), h = draw_h();
    uint32_t *px = draw_px(); if (!px) return 0;
    uint32_t *src = fx_dup(W, h); if (!src) return -1;
    fx_lap4_sharpen(px, src, W, h, amt * 2 / 3);
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Noise reduction: edge-preserving mean (average only neighbours within strength)
static int op_denoise(const int *p) {
    int str = clampi(p[0], 1, 255);
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    uint32_t *src = (uint32_t *)malloc(n * 4); if (!src) return -1;
    memcpy(src, px, n * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = src[(size_t)y * w + x];
            long sr = 0, sg = 0, sb = 0; int cnt = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    uint32_t q = at(src, w, h, x + dx, y + dy);
                    int d = abs(px_r(q) - px_r(c)) + abs(px_g(q) - px_g(c)) + abs(px_b(q) - px_b(c));
                    if (d <= str) { sr += px_r(q); sg += px_g(q); sb += px_b(q); cnt++; }
                }
            if (!cnt) cnt = 1;
            uint32_t res = argb(px_a(c), (int)(sr / cnt), (int)(sg / cnt), (int)(sb / cnt));
            px[(size_t)y * w + x] = cblend(c, res, draw_cov(x, y));
        }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Symmetric nearest neighbour: pick, per opposing pair, the closer to center
static int op_snn(const int *p) {
    int r = clampi(p[0], 1, 6);
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    uint32_t *src = (uint32_t *)malloc(n * 4); if (!src) return -1;
    memcpy(src, px, n * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = src[(size_t)y * w + x];
            long sr = px_r(c), sg = px_g(c), sb = px_b(c); int cnt = 1;
            for (int dy = -r; dy <= r; dy++)
                for (int dx = -r; dx <= r; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    uint32_t a = at(src, w, h, x + dx, y + dy);
                    uint32_t b = at(src, w, h, x - dx, y - dy);
                    int da = abs(px_r(a) - px_r(c)) + abs(px_g(a) - px_g(c)) + abs(px_b(a) - px_b(c));
                    int db = abs(px_r(b) - px_r(c)) + abs(px_g(b) - px_g(c)) + abs(px_b(b) - px_b(c));
                    uint32_t s = da <= db ? a : b;
                    sr += px_r(s); sg += px_g(s); sb += px_b(s); cnt++;
                }
            uint32_t res = argb(px_a(c), (int)(sr / cnt), (int)(sg / cnt), (int)(sb / cnt));
            px[(size_t)y * w + x] = cblend(c, res, draw_cov(x, y));
        }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Red-eye removal: desaturate pixels where red strongly dominates
static int op_redeye(const int *p) {
    int th = clampi(p[0], 0, 255);
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            uint32_t o = px[i];
            int R = px_r(o), G = px_g(o), B = px_b(o);
            if (R > 60 && R - (G > B ? G : B) > (255 - th) / 2) {
                int gray = (G + B) / 2;
                px[i] = cblend(o, argb(px_a(o), gray, G, B), draw_cov(x, y));
            }
        }
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Antialias: blend high-contrast edge pixels with neighbour average
static int op_antialias(const int *p) {
    (void)p;
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    uint32_t *src = (uint32_t *)malloc(n * 4); if (!src) return -1;
    memcpy(src, px, n * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = src[(size_t)y * w + x];
            int cl = lum(c);
            int maxd = 0; long sr = 0, sg = 0, sb = 0; int cnt = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    uint32_t q = at(src, w, h, x + dx, y + dy);
                    int d = abs(lum(q) - cl); if (d > maxd) maxd = d;
                    sr += px_r(q); sg += px_g(q); sb += px_b(q); cnt++;
                }
            uint32_t res = c;
            if (maxd > 40)
                res = argb(px_a(c), (int)(sr / cnt), (int)(sg / cnt), (int)(sb / cnt));
            px[(size_t)y * w + x] = cblend(c, res, draw_cov(x, y));
        }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Deinterlace: replace odd/even lines with the average of neighbours
static int op_deinterlace(const int *p) {
    int keepEven = (p[0] == 0);
    uint32_t *px = draw_px(); int w = draw_w(), h = draw_h();
    if (!px) return 0;
    size_t n = (size_t)w * h;
    uint32_t *src = (uint32_t *)malloc(n * 4); if (!src) return -1;
    memcpy(src, px, n * 4);
    for (int y = 0; y < h; y++) {
        int drop = keepEven ? (y & 1) : !(y & 1);
        if (!drop) continue;
        for (int x = 0; x < w; x++) {
            uint32_t a = at(src, w, h, x, y - 1), b = at(src, w, h, x, y + 1);
            uint32_t res = argb((px_a(a) + px_a(b)) / 2, (px_r(a) + px_r(b)) / 2,
                                (px_g(a) + px_g(b)) / 2, (px_b(a) + px_b(b)) / 2);
            size_t i = (size_t)y * w + x;
            px[i] = cblend(src[i], res, draw_cov(x, y));
        }
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

/* Sharpen Edges: oneshot, Photoshop-style edge-gated sharpen.
 * Sobel on luminance gates a classic 4-neighbor Laplacian sharpen (center 5,
 * orthogonals -1) so flat and noisy regions stay untouched. Integer only,
 * exact Q8 blend, alpha preserved (color-only op). */

/* clamped pixel fetch from the snapshot (same convention as fx_sample) */
static uint32_t se_px_at(const uint32_t *s, int W, int H, int x, int y) {
    x = fx_clamp(x, 0, W - 1);
    y = fx_clamp(y, 0, H - 1);
    return s[(size_t)y * W + x];
}

static int op_sharpen_edges(const int *p) {
    (void)p; /* oneshot: no dialog, no params */
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px();
    if (!px || W <= 0 || H <= 0) return 0;
    uint32_t *src = fx_dup(W, H);
    if (!src) return -1; /* OOM: nothing changed */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            int cov = draw_cov(x, y);
            if (!cov) continue;
            /* fetch the clamped 3x3 neighborhood from the snapshot once */
            uint32_t n_tl = se_px_at(src, W, H, x - 1, y - 1);
            uint32_t n_tc = se_px_at(src, W, H, x,     y - 1);
            uint32_t n_tr = se_px_at(src, W, H, x + 1, y - 1);
            uint32_t n_ml = se_px_at(src, W, H, x - 1, y);
            uint32_t n_mr = se_px_at(src, W, H, x + 1, y);
            uint32_t n_bl = se_px_at(src, W, H, x - 1, y + 1);
            uint32_t n_bc = se_px_at(src, W, H, x,     y + 1);
            uint32_t n_br = se_px_at(src, W, H, x + 1, y + 1);
            /* Sobel gx/gy on luminance; gating on fx_lum (not per channel)
             * so chroma noise does not trigger the sharpen */
            int l_tl = fx_lum(n_tl), l_tc = fx_lum(n_tc), l_tr = fx_lum(n_tr);
            int l_ml = fx_lum(n_ml),                      l_mr = fx_lum(n_mr);
            int l_bl = fx_lum(n_bl), l_bc = fx_lum(n_bc), l_br = fx_lum(n_br);
            int gx = (l_tr + 2 * l_mr + l_br) - (l_tl + 2 * l_ml + l_bl);
            int gy = (l_bl + 2 * l_bc + l_br) - (l_tl + 2 * l_tc + l_tr);
            int mag = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
            /* soft Q8 gate: 0 below 32, full 256 above 96, linear ramp between
             * (internal tuning constants, PS shows no dialog) */
            int gate;
            if (mag <= 32) continue; /* smooth area: pixel stays original */
            if (mag > 96) gate = 256;
            else gate = ((mag - 32) * 256) / 64;
            /* fully sharpened value per channel: 4-neighbor Laplacian kernel,
             * center 5 / up down left right -1 (same as plain Sharpen) */
            uint32_t o = src[i];
            int R = px_r(o), G = px_g(o), B = px_b(o);
            int sr = fx_clamp(5 * R - px_r(n_tc) - px_r(n_bc) - px_r(n_ml) - px_r(n_mr), 0, 255);
            int sg = fx_clamp(5 * G - px_g(n_tc) - px_g(n_bc) - px_g(n_ml) - px_g(n_mr), 0, 255);
            int sb = fx_clamp(5 * B - px_b(n_tc) - px_b(n_bc) - px_b(n_ml) - px_b(n_mr), 0, 255);
            /* exact Q8 blend toward the sharpened value; gate 256 lands on it */
            R += ((sr - R) * gate) >> 8;
            G += ((sg - G) * gate) >> 8;
            B += ((sb - B) * gate) >> 8;
            fx_put(px, src, i, argb(px_a(o), R, G, B), cov);
        }
    }
    free(src);
    g_doc.comp_dirty = 1;
    g_doc.modified = 1;
    return 0;
}

/* Sharpen family shared pass: 4-neighbor (plus-shaped) Laplacian sharpen at
 * Q8 gain K, per the blueprint's "Sharpen family shares kernel constants"
 * consistency rule. Intended to be promoted into fx_common.inc as the one
 * helper used by Sharpen (K=40), Sharpen More (K=96) and Sharpen Edges, so
 * the three feel like calibrated siblings.
 * All neighbor reads come from the src snapshot, never from px in place,
 * otherwise raster-order feedback skews the result directionally.
 * Edge rows/columns clamp-replicate (no border darkening). Alpha is never
 * sharpened; it is carried through via px_a(orig). The +128 rounding term
 * makes lap == 0 yield res == c, so constant regions are bit-identical. */
static void fx_lap4_sharpen(uint32_t *px, const uint32_t *src, int W, int H, int K) {
    for (int y = 0; y < H; y++) {
        int yu = (y > 0) ? y - 1 : 0;
        int yd = (y < H - 1) ? y + 1 : H - 1;
        for (int x = 0; x < W; x++) {
            int cov = draw_cov(x, y);
            if (cov <= 0) continue;
            int xl = (x > 0) ? x - 1 : 0;
            int xr = (x < W - 1) ? x + 1 : W - 1;
            int i = y * W + x;
            uint32_t c  = src[i];
            uint32_t up = src[yu * W + x], dn = src[yd * W + x];
            uint32_t lf = src[y * W + xl], rt = src[y * W + xr];
            int r = px_r(c), g = px_g(c), b = px_b(c);
            int lr = 4 * r - (px_r(up) + px_r(dn) + px_r(lf) + px_r(rt));
            int lg = 4 * g - (px_g(up) + px_g(dn) + px_g(lf) + px_g(rt));
            int lb = 4 * b - (px_b(up) + px_b(dn) + px_b(lf) + px_b(rt));
            r = fx_clamp(r + ((lr * K + 128) >> 8), 0, 255);
            g = fx_clamp(g + ((lg * K + 128) >> 8), 0, 255);
            b = fx_clamp(b + ((lb * K + 128) >> 8), 0, 255);
            fx_put(px, src, i, argb(px_a(c), r, g, b), cov);
        }
    }
}

/* Enhance / Sharpen More (oneshot, no dialog): fixed 3x3 plus-shaped
 * Laplacian sharpen at hard-coded Q8 gain K = 96 (0.375), roughly 2.5x the
 * plain Sharpen op's K = 40; equivalent kernel center 2.5, orthogonal
 * neighbors -0.375. Channels sharpen independently (Photoshop behavior,
 * slight fringing on strong chroma edges is intentional). Works unchanged
 * when the active drawable is a layer mask. */
static int op_sharpen_more(const int *p) {
    (void)p;
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H);
    if (!src) return -1;
    uint32_t *px = draw_px();
    fx_lap4_sharpen(px, src, W, H, 96);
    free(src);
    g_doc.comp_dirty = 1;
    g_doc.modified = 1;
    return 0;
}

/* Smart Sharpen (Enhance): unsharp-style sharpen against a blur estimate B
 * chosen by the Remove mode (Gaussian / Lens / Motion), with a quadratic
 * soft-knee Reduce Noise term. Integer only. Helpers use an ss_ prefix so
 * they cannot collide with the other file-static Enhance helpers. */

static inline uint32_t ss_at(const uint32_t *s, int W, int H, int x, int y) {
    x = fx_clamp(x, 0, W - 1); y = fx_clamp(y, 0, H - 1);
    return s[(size_t)y * W + x];
}

/* One horizontal box pass, running-sum accumulators, edge clamped. d != s. */
static void ss_boxh(const uint32_t *s, uint32_t *d, int W, int H, int r) {
    int win = 2 * r + 1;
    for (int y = 0; y < H; y++) {
        long sr = 0, sg = 0, sb = 0, sa = 0;
        for (int k = -r; k <= r; k++) {
            uint32_t q = ss_at(s, W, H, k, y);
            sr += px_r(q); sg += px_g(q); sb += px_b(q); sa += px_a(q);
        }
        for (int x = 0; x < W; x++) {
            d[(size_t)y * W + x] = argb((int)(sa / win), (int)(sr / win),
                                        (int)(sg / win), (int)(sb / win));
            uint32_t po = ss_at(s, W, H, x - r, y);
            uint32_t pi = ss_at(s, W, H, x + r + 1, y);
            sr += px_r(pi) - px_r(po); sg += px_g(pi) - px_g(po);
            sb += px_b(pi) - px_b(po); sa += px_a(pi) - px_a(po);
        }
    }
}

/* One vertical box pass, running-sum accumulators, edge clamped. d != s. */
static void ss_boxv(const uint32_t *s, uint32_t *d, int W, int H, int r) {
    int win = 2 * r + 1;
    for (int x = 0; x < W; x++) {
        long sr = 0, sg = 0, sb = 0, sa = 0;
        for (int k = -r; k <= r; k++) {
            uint32_t q = ss_at(s, W, H, x, k);
            sr += px_r(q); sg += px_g(q); sb += px_b(q); sa += px_a(q);
        }
        for (int y = 0; y < H; y++) {
            d[(size_t)y * W + x] = argb((int)(sa / win), (int)(sr / win),
                                        (int)(sg / win), (int)(sb / win));
            uint32_t po = ss_at(s, W, H, x, y - r);
            uint32_t pi = ss_at(s, W, H, x, y + r + 1);
            sr += px_r(pi) - px_r(po); sg += px_g(pi) - px_g(po);
            sb += px_b(pi) - px_b(po); sa += px_a(pi) - px_a(po);
        }
    }
}

/* Reduce Noise soft knee: quadratic soften of small differences, then the
 * Amount gain, per channel. c and b are 0..255 source and blur channels. */
static inline int ss_sharp_ch(int c, int b, int amount, int knee) {
    int d = c - b;
    if (knee > 0) {
        int ad = d < 0 ? -d : d;
        if (ad <= knee) d = (d * ad) / knee;  /* smooth suppress, keeps sign */
    }
    return fx_clamp(c + (amount * d) / 100, 0, 255);
}

static int op_smart_sharpen(const int *p) {
    int amount = fx_clamp(p[0], 1, 500);
    int radius = fx_clamp(p[1], 1, 32);   /* clamp degenerate values */
    int noise  = fx_clamp(p[2], 0, 100);
    int remove = fx_clamp(p[3], 0, 2);    /* 0=Gaussian 1=Lens 2=Motion */
    int angle  = p[4];                    /* read only by the Motion arm */
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px();
    if (!px || W <= 0 || H <= 0) return 0;
    size_t n = (size_t)W * H;

    uint32_t *src = fx_dup(W, H);
    if (!src) return -1;
    uint32_t *B = (uint32_t *)malloc(n * 4);
    if (!B) { free(src); return -1; }

    if (remove == 0) {
        /* Gaussian Blur: 3 iterated horizontal+vertical box blurs. */
        uint32_t *t1 = (uint32_t *)malloc(n * 4);
        uint32_t *t2 = (uint32_t *)malloc(n * 4);
        if (!t1 || !t2) { free(t1); free(t2); free(B); free(src); return -1; }
        ss_boxh(src, t1, W, H, radius); ss_boxv(t1, B, W, H, radius);
        ss_boxh(B, t1, W, H, radius);   ss_boxv(t1, t2, W, H, radius);
        ss_boxh(t2, t1, W, H, radius);  ss_boxv(t1, B, W, H, radius);
        free(t1); free(t2);
    } else if (remove == 1) {
        /* Lens Blur: circular disc mean, per-row half-spans from fx_isqrt. */
        int span[65];  /* radius <= 32, so at most 2*32+1 rows */
        for (int dy = -radius; dy <= radius; dy++)
            span[dy + radius] = (int)fx_isqrt((unsigned)(radius * radius - dy * dy));
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                size_t i = (size_t)y * W + x;
                if (!draw_cov(x, y)) { B[i] = src[i]; continue; }
                long sr = 0, sg = 0, sb = 0; int cnt = 0;
                for (int dy = -radius; dy <= radius; dy++) {
                    int hs = span[dy + radius];
                    for (int dx = -hs; dx <= hs; dx++) {
                        uint32_t q = ss_at(src, W, H, x + dx, y + dy);
                        sr += px_r(q); sg += px_g(q); sb += px_b(q); cnt++;
                    }
                }
                B[i] = argb(px_a(src[i]), (int)(sr / cnt), (int)(sg / cnt), (int)(sb / cnt));
            }
    } else {
        /* Motion Blur: 1D mean of 2R+1 taps along (fx_cos(a), fx_cos(a-90)),
         * Q12 direction, bilinear fx_sample reads at .8 fixed point. */
        int dxq = fx_cos(angle), dyq = fx_cos(angle - 90);
        int taps = 2 * radius + 1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                size_t i = (size_t)y * W + x;
                if (!draw_cov(x, y)) { B[i] = src[i]; continue; }
                long sr = 0, sg = 0, sb = 0;
                for (int t = -radius; t <= radius; t++) {
                    int fxc = (x << 8) + ((t * dxq) >> 4);  /* Q12 -> Q8 */
                    int fyc = (y << 8) + ((t * dyq) >> 4);
                    uint32_t q = fx_sample(src, W, H, fxc, fyc);
                    sr += px_r(q); sg += px_g(q); sb += px_b(q);
                }
                B[i] = argb(px_a(src[i]), (int)(sr / taps), (int)(sg / taps), (int)(sb / taps));
            }
    }

    int knee = (noise * 48) / 100;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int i = y * W + x, cov = draw_cov(x, y);
            if (!cov) continue;
            uint32_t o = src[i], b = B[i];
            int R = ss_sharp_ch(px_r(o), px_r(b), amount, knee);
            int G = ss_sharp_ch(px_g(o), px_g(b), amount, knee);
            int Bl = ss_sharp_ch(px_b(o), px_b(b), amount, knee);
            fx_put(px, src, i, argb(px_a(o), R, G, Bl), cov);  /* alpha preserved */
        }

    free(B); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

static const studio_op_t OPS[] = {
    { "Enhance", "Unsharp Mask", 3, {{SP_SLIDER,"Radius",1,20,3,0},
                                     {SP_SLIDER,"Amount %",0,300,80,0},
                                     {SP_SLIDER,"Threshold",0,255,0,0}}, op_unsharp },
    { "Enhance", "Sharpen", 1, {{SP_SLIDER,"Amount",0,255,60,0}}, op_sharpen },
    { "Enhance", "Noise Reduction", 1, {{SP_SLIDER,"Strength",1,255,40,0}}, op_denoise },
    { "Enhance", "Symmetric Nearest Neighbor", 1, {{SP_SLIDER,"Radius",1,6,3,0}}, op_snn },
    { "Enhance", "Red Eye Removal", 1, {{SP_SLIDER,"Threshold",0,255,128,0}}, op_redeye },
    { "Enhance", "Antialias", 0, {{0,0,0,0,0,0}}, op_antialias },
    { "Enhance", "Deinterlace", 1, {{SP_ENUM,"Keep",0,1,0,"Even|Odd"}}, op_deinterlace },
    { "Enhance", "Sharpen Edges", 0, {{0,0,0,0,0,0}}, op_sharpen_edges },
    { "Enhance", "Sharpen More", 0, {{0,0,0,0,0,0}}, op_sharpen_more },
    { "Enhance", "Smart Sharpen", 5, {{SP_SLIDER,"Amount",1,500,200,0},
                                      {SP_SLIDER,"Radius",1,32,2,0},
                                      {SP_SLIDER,"Reduce Noise",0,100,10,0},
                                      {SP_ENUM,"Remove",0,2,0,"Gaussian Blur|Lens Blur|Motion Blur"},
                                      {SP_ANGLE,"Angle",0,360,0,0}}, op_smart_sharpen },
};

void fx_enhance_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
