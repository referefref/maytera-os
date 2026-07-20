// fx_sketch.c - Maytera Studio "Sketch" filter category (op registry).
// Photoshop Filter Gallery > Sketch ports. Family convention (fx_edge.c
// precedent, contract B1): tone-driven two-color ops read g_tool.fg (dark /
// ink / raised side) and g_tool.bg (light / paper side) live inside apply();
// no SP_COLOR rows. Alpha is always preserved from the source pixel.
#include "fx_common.inc"

// ===========================================================================
// Bas Relief
// ===========================================================================

// Bas Relief (Sketch): carve the image into an FG/BG shaded relief.
// Smoothness box-blurs the luminance field BEFORE relief extraction; Detail
// scales edge contrast around the fixed 128 midpoint so flat areas stay at
// the 50% FG/BG mix. Light direction follows the shared y-down SP_ANGLE
// convention: light vector = (fx_cos(a), fx_sin(a)) in Q12, def 90 = Bottom.

// 1-D edge-clamped running-sum box pass (strided so it serves rows and columns).
static void br_box1d(const int *s, int *d, int n, int stride, int r) {
    int win = 2 * r + 1, sum = 0;
    for (int k = -r; k <= r; k++) sum += s[fx_clamp(k, 0, n - 1) * stride];
    for (int i = 0; i < n; i++) {
        d[i * stride] = sum / win;
        sum += s[fx_clamp(i + r + 1, 0, n - 1) * stride]
             - s[fx_clamp(i - r,     0, n - 1) * stride];
    }
}

// separable box blur of an int plane, in place, using tmp as the H-pass target
static void br_boxblur(int *L, int *tmp, int W, int H, int r) {
    for (int y = 0; y < H; y++) br_box1d(L + (size_t)y * W, tmp + (size_t)y * W, W, 1, r);
    for (int x = 0; x < W; x++) br_box1d(tmp + x, L + x, H, W, r);
}

static int op_bas_relief(const int *p) {
    int detail = fx_clamp(p[0], 1, 15);
    int rad    = fx_clamp(p[1], 1, 15);      // Smoothness = blur radius, >= 1
    int ang    = p[2];
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    int *L   = (int *)malloc((size_t)W * H * sizeof(int));
    int *tmp = (int *)malloc((size_t)W * H * sizeof(int));
    if (!L || !tmp) { free(tmp); free(L); free(src); return -1; }
    uint32_t *px = draw_px();

    for (int i = 0; i < W * H; i++) L[i] = fx_lum(src[i]);
    br_boxblur(L, tmp, W, H, rad);           // flatten texture BEFORE relief

    int ca = fx_cos(ang), sa = fx_sin(ang);  // Q12 light vector, y-down
    int fgr = px_r(g_tool.fg), fgg = px_g(g_tool.fg), fgb = px_b(g_tool.fg);
    int bgr = px_r(g_tool.bg), bgg = px_g(g_tool.bg), bgb = px_b(g_tool.bg);

    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int xl = x > 0 ? x - 1 : 0, xr = x < W - 1 ? x + 1 : x;
        int yu = y > 0 ? y - 1 : 0, yd = y < H - 1 ? y + 1 : y;
        int g = ((L[y * W + xr] - L[y * W + xl]) * ca
               + (L[yd * W + x] - L[yu * W + x]) * sa) >> 12;
        int shade = fx_clamp(128 + (g * detail) / 4, 0, 255);
        int r = fgr + ((bgr - fgr) * shade) / 255;
        int gg = fgg + ((bgg - fgg) * shade) / 255;
        int b = fgb + ((bgb - fgb) * shade) / 255;
        fx_put(px, src, i, argb(px_a(src[i]), r, gg, b), cov);
    }

    free(tmp); free(L); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ===========================================================================
// Chalk & Charcoal
// ===========================================================================

// Sketch / Chalk & Charcoal. Tricolor remap: charcoal (g_tool.fg) hatches the
// shadow band on the down-right diagonal, chalk (g_tool.bg) hatches the
// highlight band on the up-right diagonal, everything else flattens to a
// neutral midtone gray, matching Photoshop's Filter > Sketch behavior.

// Average 5 luminance samples along one 45-degree stroke axis. Each sample is
// jittered 0..2 px along the axis by fx_rnd seeded from the CROSS-diagonal
// coordinate, so the jitter is constant per stroke line and reads as coherent
// hatch strokes rather than per-pixel noise; deterministic across preview runs.
static int cc_smear(const uint32_t *src, int W, int H, int x, int y,
                    int dxs, int dys, unsigned seed) {
    unsigned s = seed; if (!s) s = 1;      // xorshift32 must never start at 0
    int sum = 0;
    for (int k = -2; k <= 2; k++) {
        int t = k + (int)(fx_rnd(&s) % 3u);
        int sx = fx_clamp(x + t * dxs, 0, W - 1);
        int sy = fx_clamp(y + t * dys, 0, H - 1);
        sum += fx_lum(src[sy * W + sx]);
    }
    return sum / 5;
}

static int op_chalk_charcoal(const int *p) {
    int carea = fx_clamp(p[0], 0, 20);     // Charcoal Area
    int karea = fx_clamp(p[1], 0, 20);     // Chalk Area
    int press = fx_clamp(p[2], 0, 5);      // Stroke Pressure; (1 + press) so 0 still strokes
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int Tc = 64 + 6 * carea;               // charcoal threshold, 64..184
    int Tk = 192 - 6 * karea;              // chalk threshold, 192..72
    if (Tc > Tk) { int mid = (Tc + Tk) / 2; Tc = mid; Tk = mid; }  // clamp overlap
    // PS draws charcoal in the current FOREGROUND color, chalk in the
    // BACKGROUND color (no swatch in its dialog); fx_edge.c precedent.
    int fr = px_r(g_tool.fg), fg = px_g(g_tool.fg), fb = px_b(g_tool.fg);
    int br = px_r(g_tool.bg), bg = px_g(g_tool.bg), bb = px_b(g_tool.bg);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        // Down-right stroke axis (1,1): cross coordinate x - y is constant per line.
        int Lc = cc_smear(src, W, H, x, y, 1, 1,
                          0xC0FFEE11u ^ ((unsigned)(x - y) * 0x9E3779B9u));
        // Up-right stroke axis (1,-1): cross coordinate x + y is constant per line.
        int Lk = cc_smear(src, W, H, x, y, 1, -1,
                          0x5EED5EEDu ^ ((unsigned)(x + y) * 0x9E3779B9u));
        int r = 128, g = 128, b = 128;     // flat midtone gray outside both bands
        if (Lc < Tc) {                     // charcoal band; wins where both hit
            int s = fx_clamp(((Tc - Lc) * (1 + press) * 64) >> 8, 0, 255);
            r = 128 + (((fr - 128) * s) >> 8);
            g = 128 + (((fg - 128) * s) >> 8);
            b = 128 + (((fb - 128) * s) >> 8);
        } else if (Lk > Tk) {              // chalk band
            int s = fx_clamp(((Lk - Tk) * (1 + press) * 64) >> 8, 0, 255);
            r = 128 + (((br - 128) * s) >> 8);
            g = 128 + (((bg - 128) * s) >> 8);
            b = 128 + (((bb - 128) * s) >> 8);
        }
        fx_put(px, src, i, argb(px_a(src[i]), r, g, b), cov);  // color-only op: keep alpha
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ===========================================================================
// Charcoal
// ===========================================================================

// ---- Sketch category file-static helpers (sk_ prefix) ----

// clamped read from an int plane
static inline int sk_at(const int *pl, int W, int H, int x, int y) {
    return pl[(size_t)fx_clamp(y, 0, H - 1) * W + fx_clamp(x, 0, W - 1)];
}

static inline int sk_max3(int a, int b, int c) {
    int m = a > b ? a : b; return m > c ? m : c;
}

// separable in-place box blur over an int plane, radius r, edge-clamped.
// Same shape as the blueprint's proposed fx_boxblur_plane; drop this local
// copy if that helper gets promoted into fx_common.inc. Returns -1 on OOM.
static int sk_boxblur_plane(int *pl, int W, int H, int r) {
    if (r <= 0) return 0;
    int *tmp = (int *)malloc((size_t)W * H * sizeof(int));
    if (!tmp) return -1;
    int win = 2 * r + 1;
    for (int y = 0; y < H; y++) {
        long s = 0;
        for (int k = -r; k <= r; k++) s += pl[(size_t)y * W + fx_clamp(k, 0, W - 1)];
        for (int x = 0; x < W; x++) {
            tmp[(size_t)y * W + x] = (int)(s / win);
            int xo = fx_clamp(x - r, 0, W - 1), xi = fx_clamp(x + r + 1, 0, W - 1);
            s += pl[(size_t)y * W + xi] - pl[(size_t)y * W + xo];
        }
    }
    for (int x = 0; x < W; x++) {
        long s = 0;
        for (int k = -r; k <= r; k++) s += tmp[(size_t)fx_clamp(k, 0, H - 1) * W + x];
        for (int y = 0; y < H; y++) {
            pl[(size_t)y * W + x] = (int)(s / win);
            int yo = fx_clamp(y - r, 0, H - 1), yi = fx_clamp(y + r + 1, 0, H - 1);
            s += tmp[(size_t)yi * W + x] - tmp[(size_t)yo * W + x];
        }
    }
    free(tmp);
    return 0;
}

// Charcoal: FG-color strokes on BG-color paper (PS Filter > Sketch > Charcoal).
// Hard two-color posterization softened only by the threshold ramp and grain.
static int op_charcoal(const int *p) {
    int thick = fx_clamp(p[0], 1, 7);
    int detail = fx_clamp(p[1], 0, 5);
    int bal = fx_clamp(p[2], 0, 100);
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int *lum = (int *)malloc((size_t)W * H * sizeof(int));
    int *qa = (int *)malloc((size_t)W * H * sizeof(int));
    int *qb = (int *)malloc((size_t)W * H * sizeof(int));
    if (!lum || !qa || !qb) { free(lum); free(qa); free(qb); free(src); return -1; }
    for (int i = 0; i < W * H; i++) lum[i] = fx_lum(src[i]);
    // low Detail suppresses fine structure; Detail=5 keeps it all
    if (sk_boxblur_plane(lum, W, H, 5 - detail) < 0) {
        free(lum); free(qa); free(qb); free(src); return -1;
    }
    // Sobel magnitude + shadow bias, soft-thresholded with a 4x ramp into qa
    int T = 255 - bal * 255 / 100;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x;
        int gx = sk_at(lum, W, H, x + 1, y - 1) + 2 * sk_at(lum, W, H, x + 1, y)
               + sk_at(lum, W, H, x + 1, y + 1) - sk_at(lum, W, H, x - 1, y - 1)
               - 2 * sk_at(lum, W, H, x - 1, y) - sk_at(lum, W, H, x - 1, y + 1);
        int gy = sk_at(lum, W, H, x - 1, y + 1) + 2 * sk_at(lum, W, H, x, y + 1)
               + sk_at(lum, W, H, x + 1, y + 1) - sk_at(lum, W, H, x - 1, y - 1)
               - 2 * sk_at(lum, W, H, x, y - 1) - sk_at(lum, W, H, x + 1, y - 1);
        int G = (gx < 0 ? -gx : gx) + (gy < 0 ? -gy : gy);
        int s = fx_clamp(G + (255 - lum[i]) * bal / 100, 0, 255);
        qa[i] = fx_clamp((s - T) * 4, 0, 255);
    }
    // fatten strokes: (thick - 1) passes of a 3-tap max on the 45-degree diagonal
    for (int pass = 0; pass < thick - 1; pass++) {
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            int i = y * W + x;
            qb[i] = sk_max3(sk_at(qa, W, H, x - 1, y - 1), qa[i],
                            sk_at(qa, W, H, x + 1, y + 1));
        }
        int *t = qa; qa = qb; qb = t;
    }
    // grain + two-color write: lerp paper (bg) toward charcoal (fg) by q/255
    int fr = px_r(g_tool.fg), fg = px_g(g_tool.fg), fb = px_b(g_tool.fg);
    int br = px_r(g_tool.bg), bg = px_g(g_tool.bg), bb = px_b(g_tool.bg);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int q = qa[i];
        if (q > 0) {                        // coordinate-seeded grain, idempotent
            unsigned sd = 0x9E3779B9u ^ (unsigned)i;
            q -= (int)(fx_rnd(&sd) % 48); if (q < 0) q = 0;
        }
        int r = br + (fr - br) * q / 255;
        int g = bg + (fg - bg) * q / 255;
        int b = bb + (fb - bb) * q / 255;
        fx_put(px, src, i, argb(px_a(src[i]), r, g, b), cov);
    }
    free(lum); free(qa); free(qb); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ===========================================================================
// Chrome
// ===========================================================================

// Sketch / Chrome: liquid-metal remap of the smoothed tone ramp.
// Color-only op (no fx_dup): alpha preserved, always neutral grayscale;
// per spec it ignores g_tool.fg/bg. Detail raises band count (folds =
// Detail + 1), Smoothness widens the 3-pass box pre-blur (r = 1 + 2*S).

// horizontal running-sum box blur, edge-clamped, window 2r+1
static void chrome_box_h(const int *src, int *dst, int W, int H, int r) {
    int win = 2 * r + 1;
    for (int y = 0; y < H; y++) {
        const int *row = src + y * W;
        int *out = dst + y * W;
        int sum = 0;
        for (int x = -r; x <= r; x++) sum += row[fx_clamp(x, 0, W - 1)];
        for (int x = 0; x < W; x++) {
            out[x] = sum / win;
            sum += row[fx_clamp(x + r + 1, 0, W - 1)];
            sum -= row[fx_clamp(x - r, 0, W - 1)];
        }
    }
}

// vertical running-sum box blur, edge-clamped, window 2r+1
static void chrome_box_v(const int *src, int *dst, int W, int H, int r) {
    int win = 2 * r + 1;
    for (int x = 0; x < W; x++) {
        int sum = 0;
        for (int y = -r; y <= r; y++) sum += src[fx_clamp(y, 0, H - 1) * W + x];
        for (int y = 0; y < H; y++) {
            dst[y * W + x] = sum / win;
            sum += src[fx_clamp(y + r + 1, 0, H - 1) * W + x];
            sum -= src[fx_clamp(y - r, 0, H - 1) * W + x];
        }
    }
}

static int op_chrome(const int *p) {
    int detail = fx_clamp(p[0], 0, 10);
    int smooth = fx_clamp(p[1], 0, 10);
    int folds = detail + 1;              // Detail 0 still gives one full fold
    int r = 1 + 2 * smooth;              // pre-blur radius, always >= 1
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px(); if (!px) return -1;
    int *lum = (int *)malloc((size_t)W * H * sizeof(int));
    if (!lum) return -1;
    int *tmp = (int *)malloc((size_t)W * H * sizeof(int));
    if (!tmp) { free(lum); return -1; }

    // Pass 1: luminance plane, then 3 separable box passes (~Gaussian)
    for (int i = 0; i < W * H; i++) lum[i] = fx_lum(px[i]);
    for (int pass = 0; pass < 3; pass++) {
        chrome_box_h(lum, tmp, W, H, r);
        chrome_box_v(tmp, lum, W, H, r);
    }

    // Pass 2: ping-pong fold into reflection bands + Q8 smoothstep gloss
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int t = lum[i] * folds;
        int b = t & 511; if (b > 255) b = 511 - b;
        int v = fx_clamp((b * b * (768 - 2 * b)) >> 16, 0, 255);
        fx_put(px, px, i, argb(px_a(px[i]), v, v, v), cov);
    }

    free(tmp); free(lum);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ===========================================================================
// Conte Crayon
// ===========================================================================

// ---- Sketch / Conte Crayon -------------------------------------------------
// PS Sketch semantics: dark tones toward g_tool.fg, light tones toward
// g_tool.bg (fx_edge.c precedent, no SP_COLOR rows), plus the shared
// Texturizer sub-panel (Texture/Scaling/Relief/Light Direction).

// Shared deterministic coordinate hash (design Consistency Fixes: belongs in
// fx_common.inc as fx_hash2; drop this local copy once it lands there).
// Fixed constant XOR coordinates, never rand(), so live preview never crawls.
static unsigned fx_hash2(unsigned seed, int x, int y) {
    unsigned s = seed ^ ((unsigned)x * 73856093u) ^ ((unsigned)y * 19349663u);
    if (!s) s = 0x9E3779B9u;              /* xorshift32 state must be nonzero */
    return fx_rnd(&s);
}

// Integer triangle wave of the given period, returns -64..64.
static int fx_tri(int v, int period) {
    int half = period / 2;
    int ph = v % period; if (ph < 0) ph += period;
    int a = ph <= half ? ph : period - ph;                        /* 0..half */
    return a * 128 / half - 64;
}

// Procedural texture height, 0..255, evaluated at image coords scaled by
// Scaling percent. Design Consistency Fixes: this is the ONE height function
// shared by Rough Pastels, Underpainting, Conte Crayon and Texturizer, enum
// order Brick|Burlap|Canvas|Sandstone; hoist into fx_common.inc as
// fx_texture_height and delete this copy when the other three land.
static int fx_texture_height(int type, int scaling, int x, int y) {
    if (scaling < 50) scaling = 50;
    if (scaling > 200) scaling = 200;
    int tx = x * 100 / scaling, ty = y * 100 / scaling;
    switch (type) {
    case 0: {                             /* Brick: mortar grid, rows of 12 */
        int row = (ty >= 0 ? ty : ty - 11) / 12;                /* floor div */
        int ry = ty % 12; if (ry < 0) ry += 12;
        int bx = (tx + ((row & 1) ? 12 : 0)) % 24; if (bx < 0) bx += 24;
        if (ry == 0 || bx == 0) return 200;                  /* mortar lines */
        return 100 + (int)(fx_hash2(0x517CC1B7u, tx, ty) & 31);
    }
    case 1:                               /* Burlap: coarse weave, period 6 */
        return fx_clamp(128 + (fx_tri(tx, 6) + fx_tri(ty, 6)) / 2
                        + ((int)(fx_hash2(0xB5297A4Du, tx, ty) & 63) - 32), 0, 255);
    case 2:                               /* Canvas: finer weave, period 4 */
        return fx_clamp(128 + (fx_tri(tx, 4) + fx_tri(ty, 4)) / 2
                        + ((int)(fx_hash2(0x68E31DA4u, tx, ty) & 15) - 8), 0, 255);
    default: {                            /* Sandstone: smoothed hash noise */
        int s = 0;
        for (int dy = 0; dy <= 1; dy++) for (int dx = 0; dx <= 1; dx++)
            s += (int)(fx_hash2(0x1B56C4E9u, tx + dx, ty + dy) & 255);
        return s / 4;
    }
    }
}

static int op_conte_crayon(const int *p) {
    int fglev   = fx_clamp(p[0], 1, 15);
    int bglev   = fx_clamp(p[1], 1, 15);
    int tex     = fx_clamp(p[2], 0, 3);
    int scaling = fx_clamp(p[3], 50, 200);
    int relief  = fx_clamp(p[4], 0, 50);
    int lx = fx_cos(p[5]) >> 10, ly = fx_sin(p[5]) >> 10;  /* light vec, -4..4 */
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px(); if (!px) return -1;
    int fr = px_r(g_tool.fg), fg = px_g(g_tool.fg), fb = px_b(g_tool.fg);
    int br = px_r(g_tool.bg), bg = px_g(g_tool.bg), bb = px_b(g_tool.bg);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t o = px[i];
        int L = fx_lum(o);                /* two-tone crayon weight, 0=FG 255=BG */
        int w = fx_clamp(128 + (L * bglev - (255 - L) * fglev) / 15, 0, 255);
        int shade = 0;
        if (relief)                       /* emboss texture along the light */
            shade = (fx_texture_height(tex, scaling, x, y)
                   - fx_texture_height(tex, scaling, x - lx * 2, y - ly * 2))
                  * relief / 8;
        int r = fx_clamp(((fr * (255 - w) + br * w) >> 8) + shade, 0, 255);
        int g = fx_clamp(((fg * (255 - w) + bg * w) >> 8) + shade, 0, 255);
        int b = fx_clamp(((fb * (255 - w) + bb * w) >> 8) + shade, 0, 255);
        fx_put(px, px, i, argb(px_a(o), r, g, b), cov);   /* color-only, keep alpha */
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// ===========================================================================
// Graphic Pen
// ===========================================================================

// Graphic Pen (Sketch): directional ink strokes in the toolbox FG color on
// BG-color paper. Tone-driven like all Sketch filters: source color is
// discarded entirely; only local luminance steers ink density. Strokes are
// coherent L-long segments (lane/segment-quantized hash), never pixel noise.
static int op_graphic_pen(const int *p) {
    // direction vectors in y-down screen space:
    // Right Diagonal (1,-1), Horizontal (1,0), Left Diagonal (1,1), Vertical (0,1)
    static const int GP_DX[4] = { 1, 1, 1, 0 };
    static const int GP_DY[4] = { -1, 0, 1, 1 };
    int L = fx_clamp(p[0], 1, 15);          // Stroke Length; Reset/preview may hand edge values
    int bal = fx_clamp(p[1], 0, 100);       // Light/Dark Balance; higher = more ink
    int dir = fx_clamp(p[2], 0, 3);         // Stroke Direction enum index
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int dx = GP_DX[dir], dy = GP_DY[dir];
    int ir = px_r(g_tool.fg), ig = px_g(g_tool.fg), ib = px_b(g_tool.fg);   // ink
    int pr = px_r(g_tool.bg), pg = px_g(g_tool.bg), pb = px_b(g_tool.bg);   // paper
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        // local tone t: mean luminance over 2L+1 clamped samples along the stroke
        int sum = 0;
        for (int k = -L; k <= L; k++) {
            int sx = fx_clamp(x + k * dx, 0, W - 1);
            int sy = fx_clamp(y + k * dy, 0, H - 1);
            sum += fx_lum(src[sy * W + sx]);
        }
        int t = sum / (2 * L + 1);
        // darkness bias: bal 0 = blank paper, 50 = neutral, 100 = near-solid ink
        int d = fx_clamp(((255 - t) * bal) / 50, 0, 255);
        // lane = coordinate constant along the stroke; along = coordinate that
        // advances with it (offset by +H so the segment divide never sees a
        // negative and splits a segment at zero)
        int lane, along;
        switch (dir) {
        case 0:  lane = x + y;     along = x - y + H; break;  // right diagonal
        case 1:  lane = y;         along = x;         break;  // horizontal
        case 2:  lane = x - y + H; along = x + y;     break;  // left diagonal
        default: lane = x;         along = y;         break;  // vertical
        }
        int seg = along / L;
        // one deterministic draw per L-long segment so strokes cohere into
        // pen-like line segments; fixed constant ^ coordinates, never rand()
        unsigned s = 0x9E3779B9u ^ ((unsigned)lane * 73856093u)
                                ^ ((unsigned)seg * 19349663u);
        int r = (int)(fx_rnd(&s) & 255);
        uint32_t o = src[i];
        uint32_t res = (r < d) ? argb(px_a(o), ir, ig, ib)
                               : argb(px_a(o), pr, pg, pb);
        fx_put(px, src, i, res, cov);
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ===========================================================================
// Halftone Pattern
// ===========================================================================

// Sketch / Halftone Pattern: screen the image's contrast-expanded luminance
// against a coordinate-only threshold pattern, rendering in the current
// foreground (dark end) and background (light end) colors (fx_edge.c precedent).
// Color-only op: no fx_dup, no allocation, alpha preserved.

// triangle wave of m in [0,P) scaled to 0..255 (0 at cell edges, 255 mid-cell)
static int ht_tri255(int m, int P) {
    int tri = m < P - m ? m : P - m;                  // 0..P/2
    return fx_clamp(tri * 512 / P, 0, 255);
}

static int op_halftone_pattern(const int *p) {
    int P = p[0] * 4; if (P < 2) P = 2;               // period in px, Size 1..12
    int gain = 256 + fx_clamp(p[1], 0, 50) * 24;      // Contrast 0 = unity gain
    int type = p[2];                                  // 0 Circle, 1 Dot, 2 Line
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    int fgr = px_r(g_tool.fg), fgg = px_g(g_tool.fg), fgb = px_b(g_tool.fg);
    int bgr = px_r(g_tool.bg), bgg = px_g(g_tool.bg), bgb = px_b(g_tool.bg);
    int mx = W / 2, my = H / 2;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t o = px[i];
        int lum = fx_lum(o);
        int lum2 = fx_clamp((lum - 128) * gain / 256 + 128, 0, 255);
        int t;                                        // pattern threshold 0..255
        if (type == 1) {                              // Dot: clustered-dot screen
            int cx = (x / P) * P + P / 2, cy = (y / P) * P + P / 2;
            int dx = x - cx, dy = y - cy;
            t = fx_clamp((int)fx_isqrt((unsigned)(dx * dx + dy * dy)) * 512 / P, 0, 255);
        } else if (type == 2) {                       // Line: horizontal lines
            t = ht_tri255(y % P, P);
        } else {                                      // Circle: concentric rings
            int dx = x - mx, dy = y - my;             // from the image center
            t = ht_tri255((int)(fx_isqrt((unsigned)(dx * dx + dy * dy)) % (unsigned)P), P);
        }
        int a = fx_clamp((lum2 - t + 16) * 255 / 32, 0, 255);  // +/-16 AA band
        int r = (fgr * (255 - a) + bgr * a) / 255;
        int g = (fgg * (255 - a) + bgg * a) / 255;
        int b = (fgb * (255 - a) + bgb * a) / 255;
        fx_put(px, px, i, argb(px_a(o), r, g, b), cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// ===========================================================================
// Note Paper
// ===========================================================================

/* Sketch / Note Paper. Two-tone paper reconstruction: source luminance picks
 * between a raised dark sheet (g_tool.fg) and a flat light sheet (g_tool.bg),
 * with embossed grain and relief shading at the sheet edge. Color-only op:
 * alpha preserved from the original pixel. Photoshop dialog parity:
 * Image Balance 0..50 def 25, Graininess 0..24 def 10, Relief 0..25 def 11. */

/* Deterministic per-coordinate grain sample in [-128,127]. Fresh xorshift
 * state per call (fixed constant XOR pixel index, never rand(), no carried
 * RNG state) so the live preview repaints identically every frame. */
static int np_grain(int x, int y, int W) {
    unsigned s = 0xC0FFEE5Au ^ (unsigned)(y * W + x);
    return (int)(fx_rnd(&s) & 255) - 128;
}

/* One integer radius-2 box blur of a byte plane, separable, edge-clamped.
 * tmp is a caller-supplied W*H int scratch buffer. */
static void np_boxblur2(unsigned char *L, int W, int H, int *tmp) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int sum = 0;
        for (int k = -2; k <= 2; k++) sum += L[y * W + fx_clamp(x + k, 0, W - 1)];
        tmp[y * W + x] = sum;
    }
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int sum = 0;
        for (int k = -2; k <= 2; k++) sum += tmp[fx_clamp(y + k, 0, H - 1) * W + x];
        L[y * W + x] = (unsigned char)(sum / 25);
    }
}

static int op_note_paper(const int *p) {
    int bal    = fx_clamp(p[0], 0, 50);   /* Image Balance */
    int grain  = fx_clamp(p[1], 0, 24);   /* Graininess */
    int relief = fx_clamp(p[2], 0, 25);   /* Relief */
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    unsigned char *L = (unsigned char *)malloc((size_t)W * H);
    unsigned char *M = (unsigned char *)malloc((size_t)W * H);
    int *tmp = (int *)malloc((size_t)W * H * sizeof(int));
    if (!L || !M || !tmp) {                /* changed nothing yet */
        free(tmp); free(M); free(L); free(src); return -1;
    }
    uint32_t *px = draw_px();

    /* Smoothed luminance plane, then the soft two-tone mask. The *16 slope
     * keeps the sheet edge anti-aliased instead of a hard threshold. */
    for (int i = 0; i < W * H; i++) L[i] = (unsigned char)fx_lum(src[i]);
    np_boxblur2(L, W, H, tmp);
    int T = bal * 255 / 50;                /* higher balance = more raised paper */
    for (int i = 0; i < W * H; i++)
        M[i] = (unsigned char)fx_clamp((T - (int)L[i]) * 16 + 128, 0, 255);

    int fgr = px_r(g_tool.fg), fgg = px_g(g_tool.fg), fgb = px_b(g_tool.fg);
    int bgr = px_r(g_tool.bg), bgg = px_g(g_tool.bg), bgb = px_b(g_tool.bg);
    int off = relief / 8;                  /* relief sampling offset, upper-left */

    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int m = M[i];
        /* Base paper tone: bg = flat light sheet, fg = raised dark sheet. */
        int r = (bgr * (255 - m) + fgr * m) / 255;
        int g = (bgg * (255 - m) + fgg * m) / 255;
        int b = (bgb * (255 - m) + fgb * m) / 255;
        /* Differential embossed grain (paper fiber lit from the upper left);
         * plain additive noise would read as TV static, not paper. */
        int e = ((np_grain(x, y, W)
                - np_grain(fx_clamp(x - 1, 0, W - 1), fx_clamp(y - 1, 0, H - 1), W))
                 * grain) / 24;
        /* Relief: bright upper-left rim, dark lower-right lip at the mask edge. */
        int sx = fx_clamp(x - 1 - off, 0, W - 1);
        int sy = fx_clamp(y - 1 - off, 0, H - 1);
        int sh = (((int)M[sy * W + sx] - m) * relief) / 16;
        r = fx_clamp(r + e + sh, 0, 255);
        g = fx_clamp(g + e + sh, 0, 255);
        b = fx_clamp(b + e + sh, 0, 255);
        fx_put(px, src, i, argb(px_a(src[i]), r, g, b), cov);
    }

    free(tmp); free(M); free(L); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ===========================================================================
// Plaster
// ===========================================================================

// Sketch / Plaster: molten two-tone plaster relief (Photoshop Filter Gallery port).
// Luma is box-blurred BEFORE thresholding (Smoothness controls how gooey the
// plateau boundaries are), then the soft mask is re-blurred into a height field
// whose gradients are lit from Light Direction. Raised (dark) areas take the
// current FG color, recessed (light) areas the BG color (g_tool, fx_edge.c
// precedent). Color-only op: alpha is preserved from the original pixel.

// Separable integer box blur on an int field, radius r, edge-clamped,
// running-sum. src is blurred in place; tmp is caller-provided scratch (W*H).
static void plaster_blur(int *buf, int *tmp, int W, int H, int r) {
    int win = 2 * r + 1;
    for (int y = 0; y < H; y++) {                 // horizontal buf->tmp
        int *row = buf + y * W, *o = tmp + y * W;
        int sum = 0;
        for (int k = -r; k <= r; k++) sum += row[fx_clamp(k, 0, W - 1)];
        for (int x = 0; x < W; x++) {
            o[x] = sum / win;
            sum += row[fx_clamp(x + r + 1, 0, W - 1)] - row[fx_clamp(x - r, 0, W - 1)];
        }
    }
    for (int x = 0; x < W; x++) {                 // vertical tmp->buf
        int sum = 0;
        for (int k = -r; k <= r; k++) sum += tmp[fx_clamp(k, 0, H - 1) * W + x];
        for (int y = 0; y < H; y++) {
            buf[y * W + x] = sum / win;
            sum += tmp[fx_clamp(y + r + 1, 0, H - 1) * W + x]
                 - tmp[fx_clamp(y - r, 0, H - 1) * W + x];
        }
    }
}

static int op_plaster(const int *p) {
    int bal = fx_clamp(p[0], 0, 50);
    int smooth = p[1]; if (smooth < 1) smooth = 1; if (smooth > 15) smooth = 15;
    int ang = p[2];
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px(); if (!px || W < 1 || H < 1) return -1;
    size_t n = (size_t)W * H;
    int *L = (int *)malloc(n * sizeof(int)); if (!L) return -1;
    int *T = (int *)malloc(n * sizeof(int)); if (!T) { free(L); return -1; }

    for (size_t i = 0; i < n; i++) L[i] = fx_lum(px[i]);
    plaster_blur(L, T, W, H, smooth);             // smoothed field B (blur BEFORE threshold)

    int t = bal * 255 / 50;                       // plateau mask: dark raises to 255
    for (size_t i = 0; i < n; i++) L[i] = fx_clamp((t - L[i]) * 8, 0, 255);
    int r2 = smooth / 2; if (r2 < 1) r2 = 1;
    plaster_blur(L, T, W, H, r2);                 // height field: rounded plaster rims

    int lx = fx_cos(ang), ly = fx_sin(ang);       // Q12 light vector, y-down, 270 = Top
    int fr = px_r(g_tool.fg), fg = px_g(g_tool.fg), fb = px_b(g_tool.fg);
    int br = px_r(g_tool.bg), bg = px_g(g_tool.bg), bb = px_b(g_tool.bg);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int xm = x > 0 ? x - 1 : 0, xp = x < W - 1 ? x + 1 : x;
        int ym = y > 0 ? y - 1 : 0, yp = y < H - 1 ? y + 1 : y;
        int gx = L[y * W + xp] - L[y * W + xm];
        int gy = L[yp * W + x] - L[ym * W + x];
        int shade = fx_clamp(128 + ((gx * lx + gy * ly) >> 12), 0, 255);
        int hv = L[i];                            // 255 = raised (FG), 0 = recessed (BG)
        int cr = (fr * hv + br * (255 - hv)) / 255;
        int cg = (fg * hv + bg * (255 - hv)) / 255;
        int cb = (fb * hv + bb * (255 - hv)) / 255;
        int rr = fx_clamp(cr * shade * 2 / 255, 0, 255);
        int rg = fx_clamp(cg * shade * 2 / 255, 0, 255);
        int rb = fx_clamp(cb * shade * 2 / 255, 0, 255);
        fx_put(px, px, i, argb(px_a(px[i]), rr, rg, rb), cov);
    }
    free(L); free(T);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ===========================================================================
// Reticulation
// ===========================================================================

// Sketch / Reticulation: cracked-emulsion film grain rendered in the current
// foreground/background colors (g_tool.fg/g_tool.bg, fx_edge.c precedent; the
// PS dialog has no swatches). Color-only, purely per-pixel, deterministic.

// Deterministic 0..255 grain hash for a lattice or pixel coordinate. Seed is
// a fixed base constant XOR a multiplicative mix of the coordinates, never
// rand(), so the pattern is identical across repeated preview applies.
static int retic_hash(unsigned base, int x, int y) {
    unsigned s = base ^ ((unsigned)x * 0x9E3779B1u) ^ ((unsigned)y * 0x85EBCA77u);
    if (!s) s = base | 1u;                 // xorshift32 must never run on 0
    fx_rnd(&s);                            // one warm-up round decorrelates
    return (int)(fx_rnd(&s) & 255u);       // nearby lattice seeds
}

// p[0]=Density 0..50, p[1]=Foreground Level 0..50, p[2]=Background Level 0..50
static int op_reticulation(const int *p) {
    int density = fx_clamp(p[0], 0, 50);
    int fglevel = fx_clamp(p[1], 0, 50);
    int bglevel = fx_clamp(p[2], 0, 50);
    // Density controls grain FREQUENCY, not opacity: it sets the clump cell
    // size. 0 = large emulsion blotches (cell 10), 50 = near per-pixel grain.
    int cell = fx_clamp(10 - density * 9 / 50, 1, 10);
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px(); if (!px) return -1;
    int fgr = px_r(g_tool.fg), fgg = px_g(g_tool.fg), fgb = px_b(g_tool.fg);
    int bgr = px_r(g_tool.bg), bgg = px_g(g_tool.bg), bgb = px_b(g_tool.bg);
    for (int y = 0; y < H; y++) {
        int cy = y / cell, ty = (y % cell) * 256 / cell;   // .8 lattice frac
        for (int x = 0; x < W; x++) {
            int cov = draw_cov(x, y); if (!cov) continue;
            int i = y * W + x;
            uint32_t o = px[i];
            int lum = fx_lum(o);
            // Coarse clump octave: bilinear .8 interpolation of corner hashes
            // on the cell lattice (fx_bilin_ch pattern), plus a fine per-pixel
            // octave; G is their average, a clumpy 0..255 grain field.
            int cx = x / cell, tx = (x % cell) * 256 / cell;
            int A = retic_hash(0x5EED0F17u, cx,     cy);
            int B = retic_hash(0x5EED0F17u, cx + 1, cy);
            int C = retic_hash(0x5EED0F17u, cx,     cy + 1);
            int D = retic_hash(0x5EED0F17u, cx + 1, cy + 1);
            int coarse = fx_bilin_ch(A, B, C, D, tx, ty);
            int fine = retic_hash(0x2545F491u, x, y);
            int G = (coarse + fine) / 2;
            // FG-grain coverage: Foreground Level clumps dense dark grain in
            // shadows, Background Level seeds sparse grain into highlights.
            int k = fx_clamp((255 - lum) * fglevel / 50 + lum * bglevel / 50,
                             0, 255);
            // Soft threshold of the grain field against the coverage, Q8.
            int w = fx_clamp((k - G) * 4 + 128, 0, 255);
            int r = (fgr * w + bgr * (256 - w)) >> 8;
            int g = (fgg * w + bgg * (256 - w)) >> 8;
            int b = (fgb * w + bgb * (256 - w)) >> 8;
            // Color-only op: alpha preserved from the original pixel. Purely
            // per-pixel with no neighborhood reads, so writing in place is
            // safe (fx_put reads orig[i] before storing dst[i]).
            fx_put(px, px, i, argb(px_a(o), r, g, b), cov);
        }
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ===========================================================================
// Stamp
// ===========================================================================

// Sketch / Stamp: Photoshop Filter > Sketch > Stamp. Thresholds a smoothed
// luminance field into two tones; the dark side takes the toolbox foreground
// color and the light side the background color (Sketch gallery filters show
// no swatches, fx_edge.c g_tool precedent). Color-only op, alpha preserved.

// One separable box-blur pass over an int luma buffer (running-sum,
// edge-clamped), same shape as fx_edge.c's boxlum; tmp is caller-allocated
// so an OOM can be rejected before any pixel is touched.
static void stamp_box(const int *src, int *dst, int *tmp, int w, int h, int r) {
    int win = 2 * r + 1;
    for (int y = 0; y < h; y++) {
        long s = 0;
        for (int k = -r; k <= r; k++) s += src[(size_t)y * w + fx_clamp(k, 0, w - 1)];
        for (int x = 0; x < w; x++) {
            tmp[(size_t)y * w + x] = (int)(s / win);
            int xo = fx_clamp(x - r, 0, w - 1), xi = fx_clamp(x + r + 1, 0, w - 1);
            s += src[(size_t)y * w + xi] - src[(size_t)y * w + xo];
        }
    }
    for (int x = 0; x < w; x++) {
        long s = 0;
        for (int k = -r; k <= r; k++) s += tmp[(size_t)fx_clamp(k, 0, h - 1) * w + x];
        for (int y = 0; y < h; y++) {
            dst[(size_t)y * w + x] = (int)(s / win);
            int yo = fx_clamp(y - r, 0, h - 1), yi = fx_clamp(y + r + 1, 0, h - 1);
            s += tmp[(size_t)yi * w + x] - tmp[(size_t)yo * w + x];
        }
    }
}

static int op_stamp(const int *p) {
    int bal = fx_clamp(p[0], 1, 50);
    int smooth = fx_clamp(p[1], 1, 50);
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px(); if (!px) return 0;
    size_t n = (size_t)W * H;
    int *L = (int *)malloc(n * sizeof(int));
    int *B = (int *)malloc(n * sizeof(int));
    int *T = (int *)malloc(n * sizeof(int));
    if (!L || !B || !T) { free(L); free(B); free(T); return -1; }
    for (size_t k = 0; k < n; k++) L[k] = fx_lum(px[k]);
    int *S = L;                                    // smoothed luminance field
    if (smooth > 1) {                              // 3 box passes ~ Gaussian
        int r = 1 + smooth / 4;                    // radius 1..13
        stamp_box(L, B, T, W, H, r);
        stamp_box(B, L, T, W, H, r);
        stamp_box(L, B, T, W, H, r);
        S = B;
    }
    int t = bal * 5;                               // threshold 5..250: higher
                                                   // Balance = more FG (dark)
    int b = 2 + smooth / 2;                        // anti-aliased band width
    int fcr = px_r(g_tool.fg), fcg = px_g(g_tool.fg), fcb = px_b(g_tool.fg);
    int bcr = px_r(g_tool.bg), bcg = px_g(g_tool.bg), bcb = px_b(g_tool.bg);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int s = fx_clamp(((S[i] - (t - b)) * 255) / (2 * b), 0, 255);
        uint32_t res = argb(px_a(px[i]),
                            (fcr * (255 - s) + bcr * s + 127) / 255,
                            (fcg * (255 - s) + bcg * s + 127) / 255,
                            (fcb * (255 - s) + bcb * s + 127) / 255);
        fx_put(px, px, i, res, cov);
    }
    free(L); free(B); free(T);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ===========================================================================
// Torn Edges
// ===========================================================================

// Sketch / Torn Edges: threshold luminance with coordinate-seeded tear noise,
// box-blur the binary mask to round the tear contour, contrast-remap, then
// paint fg-over-bg through the mask (PS Sketch convention: uses toolbox
// foreground/background colors, no swatch in the dialog).

// Separable box blur on a W*H byte plane, radius r, clamped edges.
// a is source and final destination; b is scratch (horizontal result).
// Replace with the shared fx_boxblur_plane if/when it lands in fx_common.inc.
static void te_boxblur_u8(unsigned char *a, unsigned char *b, int W, int H, int r) {
    int win = 2 * r + 1;
    for (int y = 0; y < H; y++) {                       // horizontal: a -> b
        const unsigned char *row = a + y * W;
        int sum = 0;
        for (int k = -r; k <= r; k++) sum += row[fx_clamp(k, 0, W - 1)];
        for (int x = 0; x < W; x++) {
            b[y * W + x] = (unsigned char)(sum / win);
            sum += row[fx_clamp(x + r + 1, 0, W - 1)] - row[fx_clamp(x - r, 0, W - 1)];
        }
    }
    for (int x = 0; x < W; x++) {                       // vertical: b -> a
        int sum = 0;
        for (int k = -r; k <= r; k++) sum += b[fx_clamp(k, 0, H - 1) * W + x];
        for (int y = 0; y < H; y++) {
            a[y * W + x] = (unsigned char)(sum / win);
            sum += b[fx_clamp(y + r + 1, 0, H - 1) * W + x]
                 - b[fx_clamp(y - r, 0, H - 1) * W + x];
        }
    }
}

static int op_torn_edges(const int *p) {
    int bal = fx_clamp(p[0], 0, 50);                    // Image Balance
    int r   = fx_clamp((p[1] + 1) / 2, 1, 8);           // Smoothness -> blur radius
    int g   = 64 + fx_clamp(p[2], 1, 25) * 40;          // Contrast -> Q8 gain 104..1064
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px(); if (!px) return -1;
    unsigned char *mask = (unsigned char *)malloc((size_t)W * H);
    if (!mask) return -1;
    unsigned char *tmp = (unsigned char *)malloc((size_t)W * H);
    if (!tmp) { free(mask); return -1; }

    // Pass 1: noisy hard threshold to a 0/255 tear mask. Higher Balance covers
    // more of the image in foreground (Balance 25 gives t = 127).
    int t = bal * 255 / 50;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x;
        unsigned s = 0x5EED1E57u ^ (unsigned)i;         // fixed constant ^ index: bit-stable preview
        int n = (((int)(fx_rnd(&s) & 0xFF) - 128) * 96) >> 7;
        mask[i] = (unsigned char)((fx_lum(px[i]) + n < t) ? 255 : 0);
    }

    // Pass 2: round the noisy binary boundary into the torn-paper edge.
    te_boxblur_u8(mask, tmp, W, H, r);

    // Pass 3 + final: contrast-remap the blurred mask, paint fg over bg.
    int fr = px_r(g_tool.fg), fg = px_g(g_tool.fg), fb = px_b(g_tool.fg);
    int br = px_r(g_tool.bg), bg = px_g(g_tool.bg), bb = px_b(g_tool.bg);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int m = fx_clamp((((int)mask[i] - 128) * g >> 8) + 128, 0, 255);
        uint32_t o = px[i];
        uint32_t res = argb(px_a(o),                    // color-only op: keep alpha
                            (fr * m + br * (255 - m)) / 255,
                            (fg * m + bg * (255 - m)) / 255,
                            (fb * m + bb * (255 - m)) / 255);
        fx_put(px, px, i, res, cov);
    }
    free(tmp); free(mask);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ===========================================================================
// Water Paper
// ===========================================================================

// Sketch / Water Paper: darken-weighted vertical smear (pigment wicking along
// wet paper fibers) with per-column jitter, then a brightness/contrast LUT.
// Full-color op (no FG/BG), alpha preserved from the original pixel.
static int op_water_paper(const int *p) {
    int N = fx_clamp(p[0], 3, 50);            // fiber length in pixels
    int half = N / 2;
    int jmod = N / 4 + 1;                     // per-column jitter modulus
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int gb = p[1] * 512 / 100;                // Q8 brightness gain (def 60 = 1.2x)
    int gc = p[2] * 512 / 100;                // Q8 contrast slope (def 80 = 1.6x)
    int lut[256];                             // post-smear LUT, applied per channel
    for (int v = 0; v < 256; v++)
        lut[v] = fx_clamp((((v * gb) >> 8) - 128) * gc / 256 + 128, 0, 255);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        unsigned s = 0x57A9E4C1u ^ (unsigned)x;              // fixed seed ^ column
        int j = (int)(fx_rnd(&s) % (unsigned)jmod);          // ragged run start
        int ar = 0, ag = 0, ab = 0, wsum = 0;
        for (int dy = -half; dy <= half; dy++) {
            int yy = fx_clamp(y + dy + j, 0, H - 1);
            uint32_t sp = src[yy * W + x];
            int tri = half + 1 - (dy < 0 ? -dy : dy);        // triangular falloff
            int w = tri * (256 + (255 - fx_lum(sp)));        // darken-biased weight
            ar += px_r(sp) * w; ag += px_g(sp) * w; ab += px_b(sp) * w;
            wsum += w;
        }
        int r = lut[ar / wsum], g = lut[ag / wsum], b = lut[ab / wsum];
        fx_put(px, src, i, argb(px_a(src[i]), r, g, b), cov);
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ===========================================================================
// Registry
// ===========================================================================

static const studio_op_t OPS[] = {
    {"Sketch","Bas Relief",3,{{SP_SLIDER,"Detail",1,15,13},{SP_SLIDER,"Smoothness",1,15,3},{SP_ANGLE,"Light Direction",0,360,90}},op_bas_relief},
    {"Sketch","Chalk & Charcoal",3,{{SP_SLIDER,"Charcoal Area",0,20,6},{SP_SLIDER,"Chalk Area",0,20,6},{SP_SLIDER,"Stroke Pressure",0,5,1}},op_chalk_charcoal},
    {"Sketch","Charcoal",3,{{SP_SLIDER,"Thickness",1,7,2},{SP_SLIDER,"Detail",0,5,5},{SP_SLIDER,"Light/Dark Balance",0,100,50}},op_charcoal},
    {"Sketch","Chrome",2,{{SP_SLIDER,"Detail",0,10,4},{SP_SLIDER,"Smoothness",0,10,7}},op_chrome},
    {"Sketch","Conte Crayon",6,{{SP_SLIDER,"Foreground Level",1,15,11},{SP_SLIDER,"Background Level",1,15,7},{SP_ENUM,"Texture",0,3,2,"Brick|Burlap|Canvas|Sandstone"},{SP_SLIDER,"Scaling",50,200,100},{SP_SLIDER,"Relief",0,50,4},{SP_ANGLE,"Light Direction",0,360,270}},op_conte_crayon},
    {"Sketch","Graphic Pen",3,{{SP_SLIDER,"Stroke Length",1,15,15},{SP_SLIDER,"Light/Dark Balance",0,100,50},{SP_ENUM,"Stroke Direction",0,3,0,"Right Diagonal|Horizontal|Left Diagonal|Vertical"}},op_graphic_pen},
    {"Sketch","Halftone Pattern",3,{{SP_SLIDER,"Size",1,12,4},{SP_SLIDER,"Contrast",0,50,5},{SP_ENUM,"Pattern Type",0,2,1,"Circle|Dot|Line"}},op_halftone_pattern},
    {"Sketch","Note Paper",3,{{SP_SLIDER,"Image Balance",0,50,25},{SP_SLIDER,"Graininess",0,24,10},{SP_SLIDER,"Relief",0,25,11}},op_note_paper},
    {"Sketch","Plaster",3,{{SP_SLIDER,"Image Balance",0,50,20},{SP_SLIDER,"Smoothness",1,15,2},{SP_ANGLE,"Light Direction",0,360,270}},op_plaster},
    {"Sketch","Reticulation",3,{{SP_SLIDER,"Density",0,50,12},{SP_SLIDER,"Foreground Level",0,50,40},{SP_SLIDER,"Background Level",0,50,5}},op_reticulation},
    {"Sketch","Stamp",2,{{SP_SLIDER,"Light/Dark Balance",1,50,25},{SP_SLIDER,"Smoothness",1,50,5}},op_stamp},
    {"Sketch","Torn Edges",3,{{SP_SLIDER,"Image Balance",0,50,25},{SP_SLIDER,"Smoothness",1,15,11},{SP_SLIDER,"Contrast",1,25,17}},op_torn_edges},
    {"Sketch","Water Paper",3,{{SP_SLIDER,"Fiber Length",3,50,15},{SP_SLIDER,"Brightness",0,100,60},{SP_SLIDER,"Contrast",0,100,80}},op_water_paper},
};

void fx_sketch_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
