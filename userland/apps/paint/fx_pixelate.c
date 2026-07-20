// fx_pixelate.c - Maytera Studio "Pixelate" filter category (op registry).
#include "fx_common.inc"

/* ---- Color Halftone (Pixelate) ------------------------------------------
 * Per-channel rotated halftone screens: R, G and B each get their own dot
 * grid at their own screen angle (PS RGB defaults 108/162/90). Dot AREA is
 * proportional to the channel intensity sampled at the cell center, radius
 * reaching Max Radius at 255. Screen space works in Q4 (16 subunits per
 * pixel) so every squared distance stays inside a 32-bit int; rotations are
 * computed in long then shifted. Areas no dot covers go to 0 (black),
 * exactly like Photoshop; do not fall back to the source pixel. */

#define CHT_HSIZE 256  /* direct-mapped per-channel cell intensity cache */

typedef struct { int gu, gv, val; } cht_cell_t;

/* floor division; cell indices go negative left of / above the origin */
static int cht_fdiv(int a, int b) {
    int q = a / b;
    if ((a % b) != 0 && ((a ^ b) < 0)) q--;
    return q;
}

/* Channel intensity at grid center (gu,gv): rotate the Q4 screen point back
 * to image space (inverse rotation) and bilinearly sample src there.
 * fx_sample clamps at the edges. Q4 * Q12 = Q16, >>8 gives the .8 coords. */
static int cht_cell_val(const uint32_t *src, int W, int H, int gu, int gv,
                        int pitch, int co, int si, int ch) {
    long cu = (long)gu * pitch, cv = (long)gv * pitch;   /* Q4 */
    int sx = (int)((cu * co - cv * si) >> 8);            /* .8 image coords */
    int sy = (int)((cu * si + cv * co) >> 8);
    uint32_t s = fx_sample(src, W, H, sx, sy);
    return ch == 0 ? px_r(s) : (ch == 1 ? px_g(s) : px_b(s));
}

/* Cached cell intensity: tiny malloc'd direct-mapped hash, collisions just
 * recompute. Purely a per-call cache, so apply() stays a pure function of
 * (drawable, p) for the restore-then-apply preview loop. */
static int cht_cache_val(cht_cell_t *tab, const uint32_t *src, int W, int H,
                         int gu, int gv, int pitch, int co, int si, int ch) {
    unsigned h = ((unsigned)gu * 73856093u ^ (unsigned)gv * 19349663u)
                 & (CHT_HSIZE - 1);
    cht_cell_t *e = &tab[h];
    if (e->gu != gu || e->gv != gv) {
        e->gu = gu; e->gv = gv;
        e->val = cht_cell_val(src, W, H, gu, gv, pitch, co, si, ch);
    }
    return e->val;
}

static int op_color_halftone(const int *p) {
    int rmax = p[0]; if (rmax < 2) rmax = 2;   /* Reset/preview edge values */
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    cht_cell_t *cache =
        (cht_cell_t *)malloc(sizeof(cht_cell_t) * (size_t)CHT_HSIZE * 3);
    if (!cache) { free(src); return -1; }
    for (int k = 0; k < CHT_HSIZE * 3; k++) {
        cache[k].gu = 0x7FFFFFFF; cache[k].gv = 0x7FFFFFFF; cache[k].val = 0;
    }
    uint32_t *px = draw_px();
    int pitch = rmax * 32;           /* grid pitch = 2*rmax px, in Q4 */
    int rsq   = rmax * rmax * 256;   /* rmax squared in Q4 squared units */
    int co[3], si[3];
    for (int c = 0; c < 3; c++) {    /* p[1..3] = screen angles R,G,B */
        co[c] = fx_cos(p[1 + c]); si[c] = fx_sin(p[1 + c]);
    }
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int out[3];
        for (int c = 0; c < 3; c++) {
            /* rotate into screen space: u = x cos + y sin, v = -x sin + y cos,
             * Q12 products shifted down to the Q4 working scale */
            int u = (int)(((long)x * co[c] + (long)y * si[c]) >> 8);
            int v = (int)(((long)y * co[c] - (long)x * si[c]) >> 8);
            int gu0 = cht_fdiv(u, pitch), gv0 = cht_fdiv(v, pitch);
            int lit = 0;
            for (int gy = 0; gy <= 1 && !lit; gy++)
            for (int gx = 0; gx <= 1 && !lit; gx++) {
                int gu = gu0 + gx, gv = gv0 + gy;
                int val = cht_cache_val(cache + c * CHT_HSIZE, src, W, H,
                                        gu, gv, pitch, co[c], si[c], c);
                if (val <= 0) continue;   /* zero intensity lights nothing */
                int du = u - gu * pitch, dv = v - gv * pitch;
                int d2 = du * du + dv * dv;              /* <= 2*pitch^2 */
                int thr = (int)(((long)rsq * val + 127) / 255);
                if (d2 <= thr) lit = 1;   /* dot area tracks intensity */
            }
            out[c] = lit ? 255 : 0;
        }
        /* color-only op: alpha preserved from the original pixel */
        fx_put(px, src, i, argb(px_a(src[i]), out[0], out[1], out[2]), cov);
    }
    free(cache); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Pixelate / Crystallize: jittered-grid Voronoi, one deterministic seed per SxS
// grid cell so the nearest seed is always within the 3x3 cell neighborhood.
// Seeds come from fx_rnd with fixed constants xor the cell index (never rand(),
// never carried state), so every apply() on the restored original yields the
// identical tessellation (preview loop re-applies on every slider tick).
static void cryst_seed_pos(int S, int gw, int gx, int gy, int *sx, int *sy) {
    unsigned idx = (unsigned)(gy * gw + gx);
    unsigned s1 = 0xC0FFEE21u ^ (idx * 2654435761u); if (!s1) s1 = 1;
    unsigned s2 = 0x5EEDBA5Eu ^ (idx * 2246822519u); if (!s2) s2 = 1;
    *sx = gx * S + (int)(fx_rnd(&s1) % (unsigned)S);
    *sy = gy * S + (int)(fx_rnd(&s2) % (unsigned)S);
}

static int op_crystallize(const int *p) {
    int S = p[0]; if (S < 3) S = 3; if (S > 64) S = 64;
    int W = draw_w(), H = draw_h();
    int gw = (W + S - 1) / S, gh = (H + S - 1) / S, ncell = gw * gh;
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    int *label = (int *)malloc((size_t)W * H * sizeof(int));
    int *acc = (int *)malloc((size_t)ncell * 5 * sizeof(int));
    if (!label || !acc) { free(label); free(acc); free(src); return -1; }
    memset(acc, 0, (size_t)ncell * 5 * sizeof(int));
    int *sumr = acc, *sumg = acc + ncell, *sumb = acc + 2 * ncell;
    int *suma = acc + 3 * ncell, *cnt = acc + 4 * ncell;
    uint32_t *px = draw_px();
    // Pass 1: label every pixel with its nearest seed among the 3x3 neighbor
    // cells (integer squared distance; scan order is ascending cell index and
    // strict < keeps the first winner, so ties break to the lowest index).
    // Accumulate over ALL pixels regardless of selection so cell colors stay
    // stable at selection edges.
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int gx0 = x / S, gy0 = y / S;
        int best = 0x7FFFFFFF, bestc = gy0 * gw + gx0;
        for (int ngy = gy0 - 1; ngy <= gy0 + 1; ngy++) {
            if (ngy < 0 || ngy >= gh) continue;
            for (int ngx = gx0 - 1; ngx <= gx0 + 1; ngx++) {
                if (ngx < 0 || ngx >= gw) continue;
                int sx, sy; cryst_seed_pos(S, gw, ngx, ngy, &sx, &sy);
                int dx = x - sx, dy = y - sy, d2 = dx * dx + dy * dy;
                if (d2 < best) { best = d2; bestc = ngy * gw + ngx; }
            }
        }
        uint32_t o = src[y * W + x];
        label[y * W + x] = bestc;
        sumr[bestc] += px_r(o); sumg[bestc] += px_g(o);
        sumb[bestc] += px_b(o); suma[bestc] += px_a(o);
        cnt[bestc]++;
    }
    // Pass 2: fill each selected pixel with its cell's average (alpha averaged
    // too, so partly transparent layers get faceted alpha, not opaque cells).
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int c = label[i], n = cnt[c]; if (n < 1) n = 1;
        uint32_t res = argb((suma[c] + n / 2) / n, (sumr[c] + n / 2) / n,
                            (sumg[c] + n / 2) / n, (sumb[c] + n / 2) / n);
        fx_put(px, src, i, res, cov);
    }
    free(acc); free(label); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Pixelate / Facet: one-shot edge-preserving quadrant-mean (Kuwahara 3x3).
// For each pixel, the four overlapping 3x3 quadrants of the 5x5 window around
// it (NW/NE/SW/SE, sharing the center row/column) are averaged; the quadrant
// with the lowest luminance variance wins. Clumps similar colors into flat
// blocks while keeping hard edges. One pass is deliberately subtle, matching
// Photoshop; users re-apply to strengthen. Deterministic, integer-only.
static int op_facet(const int *p) {
    (void)p;                                          // oneshot, no params
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    // top-left offset of each 3x3 quadrant relative to the center pixel
    static const int qox[4] = { -2, 0, -2, 0 };       // NW, NE, SW, SE
    static const int qoy[4] = { -2, -2, 0, 0 };
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int best_r = 0, best_g = 0, best_b = 0;
        int64_t best_score = 0; int have = 0;
        for (int q = 0; q < 4; q++) {
            int sum_r = 0, sum_g = 0, sum_b = 0, sum_l = 0, sum_l2 = 0;
            for (int dy = 0; dy < 3; dy++) for (int dx = 0; dx < 3; dx++) {
                int sx = fx_clamp(x + qox[q] + dx, 0, W - 1);
                int sy = fx_clamp(y + qoy[q] + dy, 0, H - 1);
                uint32_t s = src[sy * W + sx];
                sum_r += px_r(s); sum_g += px_g(s); sum_b += px_b(s);
                int l = fx_lum(s);                    // 0..255
                sum_l += l; sum_l2 += l * l;          // max 9*255*255, fits int
            }
            // variance score n*sumSq - sum*sum needs 64-bit (sum_l^2 can hit ~5.3e6^2 scale)
            int64_t score = 9LL * sum_l2 - (int64_t)sum_l * sum_l;
            if (!have || score < best_score) {
                have = 1; best_score = score;
                best_r = sum_r; best_g = sum_g; best_b = sum_b;
            }
        }
        uint32_t orig = src[i];                       // color-only op: keep alpha
        uint32_t res = argb(px_a(orig), (best_r + 4) / 9, (best_g + 4) / 9, (best_b + 4) / 9);
        fx_put(px, src, i, res, cov);
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Fragment: four copies of the image at fixed diagonal +-4 px offsets, averaged.
// Oneshot (no dialog); p is unused. Alpha is averaged along with RGB, and the
// (sum+2)>>2 rounding keeps flat areas byte-identical to the source.
static int op_fragment(const int *p) {
    (void)p;
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int xl = fx_clamp(x - 4, 0, W - 1), xr = fx_clamp(x + 4, 0, W - 1);
        int yt = fx_clamp(y - 4, 0, H - 1), yb = fx_clamp(y + 4, 0, H - 1);
        uint32_t s0 = src[yt * W + xl], s1 = src[yt * W + xr];
        uint32_t s2 = src[yb * W + xl], s3 = src[yb * W + xr];
        int a = (px_a(s0) + px_a(s1) + px_a(s2) + px_a(s3) + 2) >> 2;
        int r = (px_r(s0) + px_r(s1) + px_r(s2) + px_r(s3) + 2) >> 2;
        int g = (px_g(s0) + px_g(s1) + px_g(s2) + px_g(s3) + 2) >> 2;
        int b = (px_b(s0) + px_b(s1) + px_b(s2) + px_b(s3) + 2) >> 2;
        fx_put(px, src, i, argb(a, r, g, b), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Pixelate / Mezzotint: per-element random-threshold binarization.
// Every output channel is exactly 0 or 255 (fully saturated), matching
// Photoshop: RGB collapses to the 8 primary/secondary colors plus black
// and white. Element shape comes from Type: dot cells (side 1/2/3/4,
// Grainy jitters the cell origin), 1 px vertical line segments (length
// 8/16/32), or diagonal stroke segments (same lengths, per-block axis).
// Deterministic per the A5 fx_rnd rule (fixed constant xor element
// coordinates) so the pattern does not boil under the preview loop.

static void mz_element(int type, int x, int y, int *eix, int *eiy) {
    if (type <= 3) {                        // dots: Fine/Medium/Grainy/Coarse
        static const int side[4] = { 1, 2, 3, 4 };
        int s = side[type];
        if (type == 2) {                    // Grainy: jitter cell origin -1..1
            unsigned js = 0xB5297A4Du ^ ((unsigned)(x / s) * 0x9E3779B1u
                                       + (unsigned)(y / s) * 0x85EBCA6Bu);
            if (!js) js = 1;
            unsigned r = fx_rnd(&js);
            x += (int)(r % 3u) - 1;
            y += (int)((r >> 8) % 3u) - 1;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
        }
        *eix = x / s; *eiy = y / s;
        return;
    }
    if (type <= 6) {                        // lines: vertical, length 8/16/32
        int L = 8 << (type - 4);
        *eix = x; *eiy = y / L;
        return;
    }
    {                                       // strokes: diagonal, length 8/16/32
        int L = 8 << (type - 7);
        int u = x + y + 65536, v = x - y + 65536;   // rotated coords, kept positive
        unsigned bs = 0x27220A95u ^ ((unsigned)(x / L) * 0x9E3779B1u
                                   + (unsigned)(y / L) * 0x85EBCA6Bu);
        if (!bs) bs = 1;
        if (fx_rnd(&bs) & 1u) { *eix = u / L; *eiy = v; }     // axis along (1,1)
        else                  { *eix = u;     *eiy = v / L; } // axis along (1,-1)
    }
}

static int op_mezzotint(const int *p) {
    int type = fx_clamp(p[0], 0, 9);
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int eix, eiy;
        mz_element(type, x, y, &eix, &eiy);
        unsigned s = 0xC0FFEE21u ^ ((unsigned)eix * 0x9E3779B1u + (unsigned)eiy);
        if (!s) s = 1;
        int tr = (int)(fx_rnd(&s) & 255u);
        int tg = (int)(fx_rnd(&s) & 255u);
        int tb = (int)(fx_rnd(&s) & 255u);
        uint32_t o = src[i];
        int r = px_r(o) > tr ? 255 : 0;
        int g = px_g(o) > tg ? 255 : 0;
        int b = px_b(o) > tb ? 255 : 0;
        fx_put(px, src, i, argb(px_a(o), r, g, b), cov);
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Pointillize: jittered-Voronoi-disc dots, one dot per c x c cell; gaps fill
// with the CURRENT background color (g_tool.bg, fx_edge.c precedent, no swatch).
// Dot parameters are a pure function of the cell coordinates (coordinate-seeded
// fx_rnd, never rand()), so apply() is deterministic under the restore-then-apply
// preview loop.
static void pt_dot(const uint32_t *src, int W, int H, int c, int gx, int gy,
                   int *dcx, int *dcy, int *dr, uint32_t *dcol)
{
    unsigned s = 0x9E3779B9u ^ ((unsigned)gx * 73856093u ^ (unsigned)gy * 19349663u);
    if (!s) s = 0x1B873593u;                      // xorshift must not start at 0
    int jx = (int)(fx_rnd(&s) % (unsigned)c);     // jitter within the cell
    int jy = (int)(fx_rnd(&s) % (unsigned)c);
    int r  = c / 2 + (int)(fx_rnd(&s) % (unsigned)(c / 2 + 1));
    int cx = gx * c + jx, cy = gy * c + jy;       // dot center (may be off-canvas)
    int sx = fx_clamp(cx, 0, W - 1), sy = fx_clamp(cy, 0, H - 1);
    *dcx = cx; *dcy = cy; *dr = r; *dcol = src[sy * W + sx];
}

static int op_pointillize(const int *p) {
    int c = p[0]; if (c < 3) c = 3;
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int br = px_r(g_tool.bg), bg = px_g(g_tool.bg), bb = px_b(g_tool.bg);
    for (int y = 0; y < H; y++) {
        int by = y / c;
        for (int x = 0; x < W; x++) {
            int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
            int bx = x / c;
            int best_d2 = 0x7FFFFFFF, best_r = 0, found = 0;
            uint32_t best_col = 0;
            for (int gy = by - 1; gy <= by + 1; gy++)     // 3x3 cell search
            for (int gx = bx - 1; gx <= bx + 1; gx++) {
                int dcx, dcy, dr; uint32_t dcol;
                pt_dot(src, W, H, c, gx, gy, &dcx, &dcy, &dr, &dcol);
                int dx = x - dcx, dy = y - dcy;
                int d2 = dx * dx + dy * dy;
                if (d2 <= dr * dr && d2 < best_d2) {
                    best_d2 = d2; best_r = dr; best_col = dcol; found = 1;
                }
            }
            int rr, gg, bv;
            if (found) {
                rr = px_r(best_col); gg = px_g(best_col); bv = px_b(best_col);
                int r2 = best_r * best_r, r1 = (best_r - 1) * (best_r - 1);
                if (best_d2 > r1 && r2 > r1) {            // soft ~1px rim toward bg
                    int w = (r2 - best_d2) * 255 / (r2 - r1);
                    rr = br + (rr - br) * w / 255;
                    gg = bg + (gg - bg) * w / 255;
                    bv = bb + (bv - bb) * w / 255;
                }
            } else { rr = br; gg = bg; bv = bb; }         // gap = background color
            fx_put(px, src, i, argb(px_a(src[i]), rr, gg, bv), cov);
        }
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static const studio_op_t OPS[] = {
    {"Pixelate","Color Halftone",4,{{SP_SLIDER,"Max Radius",4,64,8},{SP_ANGLE,"Angle 1 (R)",0,360,108},{SP_ANGLE,"Angle 2 (G)",0,360,162},{SP_ANGLE,"Angle 3 (B)",0,360,90}},op_color_halftone},
    {"Pixelate","Crystallize",1,{{SP_SLIDER,"Cell Size",3,64,50}},op_crystallize},
    {"Pixelate","Facet",0,{{0,0,0,0,0}},op_facet},
    {"Pixelate","Fragment",0,{{0,0,0,0,0}},op_fragment},
    {"Pixelate","Mezzotint",1,{{SP_ENUM,"Type",0,9,0,"Fine Dots|Medium Dots|Grainy Dots|Coarse Dots|Short Lines|Medium Lines|Long Lines|Short Strokes|Medium Strokes|Long Strokes"}},op_mezzotint},
    {"Pixelate","Pointillize",1,{{SP_SLIDER,"Cell Size",3,64,5}},op_pointillize},
};
void fx_pixelate_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
