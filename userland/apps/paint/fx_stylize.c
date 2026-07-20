// fx_stylize.c - "Stylize" filter category for Maytera Studio (Photoshop
// Stylize menu parity): Diffuse, Extrude, Solarize, Tiles, Trace Contour,
// Wind, Glowing Edges. Each op follows the studio_op_t contract in studio.h:
// undo_push() is done by ui.c before apply, ops operate on the active
// drawable through draw_px()/draw_cov(), preserve alpha where color-only,
// set g_doc.comp_dirty and g_doc.modified on success, and return -1 having
// changed nothing on any allocation failure. Integer/fixed-point only.
#include "fx_common.inc"

// ---- Stylize / Diffuse -----------------------------------------------------

// Stylize / Diffuse: shuffle each pixel with a nearby one (Photoshop Diffuse).
// Mode 0 Normal moves whole ARGB pixels; 1/2 keep the swap only if the candidate
// is darker/lighter than the original destination pixel; 3 Anisotropic is one
// deterministic integer Perona-Malik smoothing step (edge-preserving, no RNG).
static int op_diffuse(const int *p) {
    int mode = fx_clamp(p[0], 0, 3);
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    const int nx[4] = { -1, 1, 0, 0 }, ny[4] = { 0, 0, -1, 1 };
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t orig = src[i], res;
        if (mode == 3) {
            // one Perona-Malik iteration: Q12 edge-stopping weight, k=8 on 0..255 lum deltas
            int lc = fx_lum(orig);
            int sr = 0, sg = 0, sb = 0;
            for (int n = 0; n < 4; n++) {
                int qx = fx_clamp(x + nx[n], 0, W - 1);
                int qy = fx_clamp(y + ny[n], 0, H - 1);
                uint32_t q = src[qy * W + qx];
                int d = fx_lum(q) - lc;
                int w = 16777216 / (4096 + 8 * d * d);   // 4096*4096/(4096+8*d*d), Q12
                sr += w * (px_r(q) - px_r(orig));
                sg += w * (px_g(q) - px_g(orig));
                sb += w * (px_b(q) - px_b(orig));
            }
            res = argb(px_a(orig),
                       fx_clamp(px_r(orig) + (sr >> 14), 0, 255),
                       fx_clamp(px_g(orig) + (sg >> 14), 0, 255),
                       fx_clamp(px_b(orig) + (sb >> 14), 0, 255));
        } else {
            // coordinate-seeded RNG: bit-identical on every preview re-apply, never crawls
            unsigned s = 0x44494646u ^ (unsigned)i;      // "DIFF" ^ (y*W+x)
            int dx = (int)(fx_rnd(&s) % 5u) - 2;
            int dy = (int)(fx_rnd(&s) % 5u) - 2;
            int cx = fx_clamp(x + dx, 0, W - 1), cy = fx_clamp(y + dy, 0, H - 1);
            uint32_t cand = src[cy * W + cx];
            if (mode == 1)      res = (fx_lum(cand) < fx_lum(orig)) ? cand : orig; // Darken Only
            else if (mode == 2) res = (fx_lum(cand) > fx_lum(orig)) ? cand : orig; // Lighten Only
            else                res = cand;              // Normal: alpha travels with the pixel
        }
        fx_put(px, src, i, res, cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// ---- Stylize / Extrude ----------------------------------------------------

// scale an ARGB color's RGB by num/256 (alpha preserved); num 0..256
static uint32_t ex_shade(uint32_t c, int num) {
    return argb(px_a(c), fx_clamp(px_r(c) * num >> 8, 0, 255),
                         fx_clamp(px_g(c) * num >> 8, 0, 255),
                         fx_clamp(px_b(c) * num >> 8, 0, 255));
}

// scanline-fill a convex polygon (3 or 4 vertices) with a flat color, clipped
static void ex_fill_poly(uint32_t *out, int W, int H,
                         const int *vx, const int *vy, int n, uint32_t col) {
    int miny = vy[0], maxy = vy[0];
    for (int j = 1; j < n; j++) {
        if (vy[j] < miny) miny = vy[j];
        if (vy[j] > maxy) maxy = vy[j];
    }
    if (miny < 0) miny = 0;
    if (maxy > H - 1) maxy = H - 1;
    for (int y = miny; y <= maxy; y++) {
        int xl = W, xr = -1;
        for (int j = 0; j < n; j++) {
            int k = (j + 1) % n;
            int y0 = vy[j], y1 = vy[k], x0 = vx[j], x1 = vx[k];
            if (y0 == y1) {
                if (y == y0) {
                    if (x0 < xl) xl = x0;
                    if (x0 > xr) xr = x0;
                    if (x1 < xl) xl = x1;
                    if (x1 > xr) xr = x1;
                }
                continue;
            }
            int ylo = y0 < y1 ? y0 : y1, yhi = y0 < y1 ? y1 : y0;
            if (y < ylo || y > yhi) continue;
            int xi = x0 + (int)((long)(x1 - x0) * (y - y0) / (y1 - y0));
            if (xi < xl) xl = xi;
            if (xi > xr) xr = xi;
        }
        if (xr < 0) continue;
        xl = fx_clamp(xl, 0, W - 1);
        xr = fx_clamp(xr, 0, W - 1);
        for (int x = xl; x <= xr; x++) out[y * W + x] = col;
    }
}

static int op_extrude(const int *p) {
    int type = p[0];
    int ts = fx_clamp(p[1], 2, 64);
    int depth = fx_clamp(p[2], 1, 255);
    int dmode = p[3], solid = p[4], maskinc = p[5];
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *out = (uint32_t *)malloc((size_t)W * H * 4);
    if (!out) { free(src); return -1; }
    memcpy(out, src, (size_t)W * H * 4);
    int nbx = (W + ts - 1) / ts, nby = (H + ts - 1) / ts, ncell = nbx * nby;
    int *order = (int *)malloc((size_t)ncell * sizeof(int));
    int *d2    = (int *)malloc((size_t)ncell * sizeof(int));
    if (!order || !d2) { free(order); free(d2); free(out); free(src); return -1; }
    int cx = W / 2, cy = H / 2;
    for (int c = 0; c < ncell; c++) {
        int bx = c % nbx, by = c / nbx;
        int x0 = bx * ts, y0 = by * ts;
        int cw = (W - x0 < ts) ? W - x0 : ts, ch = (H - y0 < ts) ? H - y0 : ts;
        int dx = x0 + cw / 2 - cx, dy = y0 + ch / 2 - cy;
        d2[c] = dx * dx + dy * dy;
        order[c] = c;
    }
    // painter's order: farthest-from-center cells first so nearer blocks overdraw.
    // shell sort keeps the worst case (Size=2 on a large canvas) far from O(n^2).
    for (int gap = ncell / 2; gap > 0; gap /= 2)
        for (int i = gap; i < ncell; i++) {
            int t = order[i], j = i;
            while (j >= gap && d2[order[j - gap]] < d2[t]) { order[j] = order[j - gap]; j -= gap; }
            order[j] = t;
        }
    for (int k = 0; k < ncell; k++) {
        int c = order[k], bx = c % nbx, by = c / nbx;
        int x0 = bx * ts, y0 = by * ts;
        int cw = (W - x0 < ts) ? W - x0 : ts, ch = (H - y0 < ts) ? H - y0 : ts;
        int x1 = x0 + cw, y1 = y0 + ch;
        long sr = 0, sg = 0, sb = 0, sa = 0;
        for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) {
            uint32_t v = src[yy * W + xx];
            sa += px_a(v); sr += px_r(v); sg += px_g(v); sb += px_b(v);
        }
        int cnt = cw * ch; if (cnt < 1) cnt = 1;
        uint32_t avg = argb((int)(sa / cnt), (int)(sr / cnt), (int)(sg / cnt), (int)(sb / cnt));
        int h;
        if (dmode == 0) {                                 // Random, stable across previews
            unsigned s = 0x5EEDC0DEu ^ (unsigned)(by * 4096 + bx);
            h = depth * (int)(fx_rnd(&s) & 255) >> 8;
        } else {                                          // Level: bright cells rise
            h = depth * fx_lum(avg) >> 8;
        }
        int pcx = x0 + cw / 2, pcy = y0 + ch / 2;
        int ox = (pcx - cx) * h / 256, oy = (pcy - cy) * h / 256;   // Q8 radial slide
        if (maskinc) {                                    // skip partial or off-canvas blocks
            if (cw < ts || ch < ts) continue;
            if (x0 + ox < 0 || y0 + oy < 0 || x1 + ox > W || y1 + oy > H) continue;
        }
        if (type == 1) {                                  // Pyramids: 4 shaded facets to apex
            int ax = pcx + ox, ay = pcy + oy;
            static const int fsh[4] = { 256, 224, 192, 160 };   // 4/4, 7/8, 6/8, 5/8
            int tvx[4][3] = { { x0, x1, ax }, { x1, x1, ax }, { x1, x0, ax }, { x0, x0, ax } };
            int tvy[4][3] = { { y0, y0, ay }, { y0, y1, ay }, { y1, y1, ay }, { y1, y0, ay } };
            for (int f = 0; f < 4; f++)
                ex_fill_poly(out, W, H, tvx[f], tvy[f], 3, ex_shade(avg, fsh[f]));
        } else {                                          // Blocks
            if (ox > 0) {                                 // left side face visible
                int qx[4] = { x0, x0, x0 + ox, x0 + ox }, qy[4] = { y0, y1, y1 + oy, y0 + oy };
                ex_fill_poly(out, W, H, qx, qy, 4, ex_shade(avg, 192));
            } else if (ox < 0) {                          // right side face visible
                int qx[4] = { x1, x1, x1 + ox, x1 + ox }, qy[4] = { y0, y1, y1 + oy, y0 + oy };
                ex_fill_poly(out, W, H, qx, qy, 4, ex_shade(avg, 192));
            }
            if (oy > 0) {                                 // top side face visible
                int qx[4] = { x0, x1, x1 + ox, x0 + ox }, qy[4] = { y0, y0, y0 + oy, y0 + oy };
                ex_fill_poly(out, W, H, qx, qy, 4, ex_shade(avg, 128));
            } else if (oy < 0) {                          // bottom side face visible
                int qx[4] = { x0, x1, x1 + ox, x0 + ox }, qy[4] = { y1, y1, y1 + oy, y1 + oy };
                ex_fill_poly(out, W, H, qx, qy, 4, ex_shade(avg, 128));
            }
            if (solid) {                                  // flat average-color front face
                int qx[4] = { x0 + ox, x1 + ox, x1 + ox, x0 + ox };
                int qy[4] = { y0 + oy, y0 + oy, y1 + oy, y1 + oy };
                ex_fill_poly(out, W, H, qx, qy, 4, avg);
            } else {                                      // image-textured front face (PS default)
                for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) {
                    int dxp = xx + ox, dyp = yy + oy;
                    if (dxp < 0 || dxp >= W || dyp < 0 || dyp >= H) continue;
                    out[dyp * W + dxp] = src[yy * W + xx];
                }
            }
        }
    }
    free(order); free(d2);
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        fx_put(px, src, i, out[i], cov);
    }
    free(out); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// ---- Stylize / Solarize ---------------------------------------------------

/* Stylize / Solarize: Photoshop one-shot solarize (no dialog, p unused).
   Per-channel LUT is min(v, 255 - v): identity below 128, inverted above,
   NOT the full-range tent 255-|2v-255|. Output never exceeds 127; the dark
   result is by design (PS suggests Auto Contrast afterwards, do not bake
   any normalization in here). Color-only op: alpha is preserved. Applying
   twice is not a no-op, matching Photoshop; the preview machinery always
   hands apply() the pristine layer, so no state is kept between calls. */
static int op_solarize(const int *p) {
    (void)p;
    int lut[256];
    for (int v = 0; v < 256; v++) lut[v] = (v < 128) ? v : (255 - v);
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t orig = px[i];
        uint32_t res = argb(px_a(orig), lut[px_r(orig)], lut[px_g(orig)], lut[px_b(orig)]);
        /* In-place is safe: orig is read before fx_put writes px[i]. */
        fx_put(px, px, i, res, cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---- Stylize / Tiles ------------------------------------------------------

// Stylize / Tiles: cut the image into a grid of at least p[0] tiles per row
// and column, shift each tile by a deterministic random offset of up to
// p[1] percent of the tile edge, and fill the exposed background per p[2]
// (Background color, Foreground color, Inverse image, Unaltered image).
static int op_tiles(const int *p) {
    int n = p[0]; if (n < 1) n = 1;                      // minimum tiles per axis
    int maxpct = fx_clamp(p[1], 0, 100);
    int mode = fx_clamp(p[2], 0, 3);
    int W = draw_w(), H = draw_h();
    int mind = W < H ? W : H;
    int ts = mind / n; if (ts < 1) ts = 1;               // tile edge in pixels
    int maxoff = ts * maxpct / 100;                      // may be 0 for tiny tiles: identity grid
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *dst = (uint32_t *)malloc((size_t)W * H * 4);
    if (!dst) { free(src); return -1; }
    // Pre-fill the whole scratch canvas with the empty-area fill.
    for (int i = 0; i < W * H; i++) {
        uint32_t s = src[i];
        switch (mode) {
        case 0:  dst[i] = argb(255, px_r(g_tool.bg), px_g(g_tool.bg), px_b(g_tool.bg)); break;
        case 1:  dst[i] = argb(255, px_r(g_tool.fg), px_g(g_tool.fg), px_b(g_tool.fg)); break;
        case 2:  dst[i] = argb(px_a(s), 255 - px_r(s), 255 - px_g(s), 255 - px_b(s)); break;
        default: dst[i] = s; break;
        }
    }
    // Forward-copy each tile with a deterministic per-tile shift; later tiles
    // overwrite earlier ones where shifts overlap (matches Photoshop occlusion).
    int span = 2 * maxoff + 1;
    for (int ty = 0; ty * ts < H; ty++) {
        for (int tx = 0; tx * ts < W; tx++) {
            unsigned seed = 0x7113E5u ^ (unsigned)(ty * 4096 + tx);
            if (!seed) seed = 1;                         // xorshift must not start at 0
            int dx = (int)(fx_rnd(&seed) % (unsigned)span) - maxoff;
            int dy = (int)(fx_rnd(&seed) % (unsigned)span) - maxoff;
            int x0 = tx * ts, y0 = ty * ts;
            int x1 = x0 + ts; if (x1 > W) x1 = W;
            int y1 = y0 + ts; if (y1 > H) y1 = H;
            for (int sy = y0; sy < y1; sy++) {
                int oy = sy + dy; if (oy < 0 || oy >= H) continue;
                for (int sx = x0; sx < x1; sx++) {
                    int ox = sx + dx; if (ox < 0 || ox >= W) continue;
                    dst[oy * W + ox] = src[sy * W + sx];
                }
            }
        }
    }
    // Composite through selection coverage against the pristine snapshot.
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        fx_put(px, src, i, dst[i], cov);
    }
    free(dst); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// ---- Stylize / Trace Contour ----------------------------------------------

// per-channel value fetch for the contour tracer (c: 0=R, 1=G, 2=B)
static int tc_ch(uint32_t p, int c) {
    return c == 0 ? px_r(p) : (c == 1 ? px_g(p) : px_b(p));
}

// Trace Contour: trace each color channel independently against Level.
// A contour pixel goes to 0 in that channel, everything else to 255, so the
// composite is a white field with 1px colored per-channel lines (Photoshop
// look). Edge=Lower marks pixels below Level with a 4-neighbor at or above
// it; Edge=Upper marks pixels at or above Level with a 4-neighbor below it.
static int op_trace_contour(const int *p) {
    int L = fx_clamp(p[0], 0, 255);
    int upper = p[1] ? 1 : 0;
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        // clamped 4-neighbor coords: borders compare against themselves,
        // so the image edge never phantom-traces a frame
        int xl = x > 0 ? x - 1 : 0, xr = x < W - 1 ? x + 1 : W - 1;
        int yu = y > 0 ? y - 1 : 0, yd = y < H - 1 ? y + 1 : H - 1;
        uint32_t o = src[i];
        uint32_t n0 = src[y * W + xl], n1 = src[y * W + xr];
        uint32_t n2 = src[yu * W + x], n3 = src[yd * W + x];
        int out[3];
        for (int c = 0; c < 3; c++) {
            int v = tc_ch(o, c), hit = 0;
            if (upper) {
                if (v >= L)
                    hit = tc_ch(n0, c) < L || tc_ch(n1, c) < L ||
                          tc_ch(n2, c) < L || tc_ch(n3, c) < L;
            } else {
                if (v < L)
                    hit = tc_ch(n0, c) >= L || tc_ch(n1, c) >= L ||
                          tc_ch(n2, c) >= L || tc_ch(n3, c) >= L;
            }
            out[c] = hit ? 0 : 255;
        }
        fx_put(px, src, i, argb(px_a(o), out[0], out[1], out[2]), cov);
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---- Stylize / Wind -------------------------------------------------------

// Stylize / Wind: PS-style directional streaks seeded from bright pixels.
// Methods: Wind (short streaks), Blast (long solid gusts), Stagger (jittered
// per-pixel row displacement). Direction is the side the wind blows FROM,
// so Left drags bright streaks rightward. Pure function of (drawable, p):
// fx_rnd is reseeded per pixel from a fixed constant XOR the coordinate,
// so repeated OK applications compound like Photoshop's Ctrl+F reapply.
static int op_wind(const int *p) {
    int method = fx_clamp(p[0], 0, 2), dir = p[1] ? 1 : 0;    // dir: 0=Left, 1=Right
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    if (method == 2) {                                        // Stagger: whole-pixel jitter
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
            unsigned rs = 0x57494E44u ^ (unsigned)(y * W + x);
            int d = (int)(fx_rnd(&rs) % 17);                  // 0..16 px upwind
            int sx = fx_clamp(dir ? x + d : x - d, 0, W - 1);
            fx_put(px, src, i, src[y * W + sx], cov);         // whole pixel, alpha included
        }
    } else {                                                  // Wind / Blast streaks
        int decay = (method == 1) ? 250 : 230;                // Blast decays slower
        for (int y = 0; y < H; y++) {
            uint32_t carry = 0; int str = 0;                  // carried color + Q8 strength
            int x0 = dir ? W - 1 : 0, step = dir ? -1 : 1;    // scan downwind
            for (int n = 0; n < W; n++) {
                int x = x0 + n * step;
                int i = y * W + x, cov = draw_cov(x, y);
                uint32_t sp = src[i];
                unsigned rs = 0x57494E44u ^ (unsigned)(y * W + x);
                if ((int)(fx_rnd(&rs) & 0xFF) < (fx_lum(sp) >> 1)) {
                    carry = sp; str = 256;                    // bright pixels seed streaks
                } else {
                    str = str * decay >> 8;
                    if (method == 1 && (fx_rnd(&rs) & 7) == 0) str = 256;  // gust hold, 1/8
                }
                if (!cov) continue;                           // carry still crosses gaps
                int r = px_r(sp), g = px_g(sp), b = px_b(sp);
                int cr = px_r(carry) * str >> 8, cg = px_g(carry) * str >> 8, cb = px_b(carry) * str >> 8;
                if (cr > r) r = cr;
                if (cg > g) g = cg;
                if (cb > b) b = cb;
                fx_put(px, src, i, argb(px_a(sp), r, g, b), cov);
            }
        }
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// ---- Stylize / Glowing Edges -----------------------------------------------

// Stylize / Glowing Edges: per-channel Sobel edges on a black background,
// thickened by Edge Width (separable max filter) and softened by Smoothness
// (two passes of a separable running-sum box blur). PS Filter Gallery parity.

// channel c of an ARGB pixel: 0=R 1=G 2=B
static inline int ge_ch(uint32_t v, int c) { return (int)(v >> (16 - 8 * c)) & 255; }

// grayscale dilate over one line (stride-addressed), radius r, per RGB channel
static void ge_max_line(const uint32_t *s, uint32_t *d, int n, int stride, int r) {
    for (int i = 0; i < n; i++) {
        int lo = i - r, hi = i + r, mr = 0, mg = 0, mb = 0;
        if (lo < 0) lo = 0;
        if (hi > n - 1) hi = n - 1;
        for (int j = lo; j <= hi; j++) {
            uint32_t v = s[j * stride];
            if (px_r(v) > mr) mr = px_r(v);
            if (px_g(v) > mg) mg = px_g(v);
            if (px_b(v) > mb) mb = px_b(v);
        }
        d[i * stride] = argb(255, mr, mg, mb);
    }
}

// edge-clamped running-sum box blur over one line (stride-addressed), radius r
static void ge_box_line(const uint32_t *s, uint32_t *d, int n, int stride, int r) {
    int w = 2 * r + 1, sr = 0, sg = 0, sb = 0;
    for (int j = -r; j <= r; j++) {
        uint32_t v = s[fx_clamp(j, 0, n - 1) * stride];
        sr += px_r(v); sg += px_g(v); sb += px_b(v);
    }
    for (int i = 0; i < n; i++) {
        d[i * stride] = argb(255, (sr + w / 2) / w, (sg + w / 2) / w, (sb + w / 2) / w);
        uint32_t ad = s[fx_clamp(i + r + 1, 0, n - 1) * stride];
        uint32_t su = s[fx_clamp(i - r, 0, n - 1) * stride];
        sr += px_r(ad) - px_r(su);
        sg += px_g(ad) - px_g(su);
        sb += px_b(ad) - px_b(su);
    }
}

static int op_glowing_edges(const int *p) {
    int ew = fx_clamp(p[0], 1, 14);          // Edge Width
    int eb = fx_clamp(p[1], 0, 20);          // Edge Brightness (8 ~= unity gain)
    int sm = fx_clamp(p[2], 1, 15);          // Smoothness
    int rw = (ew - 1) / 2;                   // dilate radius: width 1 stays 1 px
    int rs = (sm + 2) / 3; if (rs < 1) rs = 1;   // blur radius per pass
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *edge = (uint32_t *)malloc((size_t)W * H * 4);
    uint32_t *tmp  = (uint32_t *)malloc((size_t)W * H * 4);
    if (!edge || !tmp) { free(edge); free(tmp); free(src); return -1; }

    // pass 1: per-channel Sobel magnitude of the snapshot, scaled by Edge
    // Brightness, onto an implicit black background (Find Edges inverted, so
    // a red/blue boundary glows in both hues rather than one gray line)
    for (int y = 0; y < H; y++) {
        int ym = y > 0 ? y - 1 : 0, yp = y < H - 1 ? y + 1 : H - 1;
        for (int x = 0; x < W; x++) {
            int xm = x > 0 ? x - 1 : 0, xp = x < W - 1 ? x + 1 : W - 1;
            uint32_t tl = src[ym * W + xm], tc = src[ym * W + x], tr = src[ym * W + xp];
            uint32_t ml = src[y  * W + xm],                       mr = src[y  * W + xp];
            uint32_t bl = src[yp * W + xm], bc = src[yp * W + x], br = src[yp * W + xp];
            int v[3];
            for (int c = 0; c < 3; c++) {
                int gx = (ge_ch(tr, c) + 2 * ge_ch(mr, c) + ge_ch(br, c))
                       - (ge_ch(tl, c) + 2 * ge_ch(ml, c) + ge_ch(bl, c));
                int gy = (ge_ch(bl, c) + 2 * ge_ch(bc, c) + ge_ch(br, c))
                       - (ge_ch(tl, c) + 2 * ge_ch(tc, c) + ge_ch(tr, c));
                if (gx < 0) gx = -gx;
                if (gy < 0) gy = -gy;
                v[c] = fx_clamp(((gx + gy) * eb * 32) >> 8, 0, 255);   // mag 0..2040
            }
            edge[y * W + x] = argb(255, v[0], v[1], v[2]);
        }
    }

    // pass 2: thicken with a separable grayscale dilate (skip at radius 0)
    if (rw > 0) {
        for (int y = 0; y < H; y++) ge_max_line(edge + y * W, tmp + y * W, W, 1, rw);
        for (int x = 0; x < W; x++) ge_max_line(tmp + x, edge + x, H, W, rw);
    }

    // pass 3: soften with two passes of a separable running-sum box blur,
    // approximating the Gaussian-like glow of Smoothness
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < H; y++) ge_box_line(edge + y * W, tmp + y * W, W, 1, rs);
        for (int x = 0; x < W; x++) ge_box_line(tmp + x, edge + x, H, W, rs);
    }

    // commit: preserve original alpha, respect selection coverage
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t e = edge[i];
        fx_put(px, src, i, argb(px_a(src[i]), px_r(e), px_g(e), px_b(e)), cov);
    }
    free(tmp); free(edge); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---- Registration -----------------------------------------------------------

static const studio_op_t OPS[] = {
    {"Stylize","Diffuse",1,{{SP_ENUM,"Mode",0,3,0,"Normal|Darken Only|Lighten Only|Anisotropic"}},op_diffuse},
    {"Stylize","Extrude",6,{{SP_ENUM,"Type",0,1,0,"Blocks|Pyramids"},{SP_SLIDER,"Size",2,64,30},{SP_SLIDER,"Depth",1,255,30},{SP_ENUM,"Depth Mode",0,1,0,"Random|Level"},{SP_CHECK,"Solid Front Faces",0,1,0},{SP_CHECK,"Mask Incomplete Blocks",0,1,0}},op_extrude},
    {"Stylize","Solarize",0,{{0,0,0,0,0}},op_solarize},
    {"Stylize","Tiles",3,{{SP_SLIDER,"Number of Tiles",1,99,10},{SP_SLIDER,"Maximum Offset",1,99,10},{SP_ENUM,"Fill Empty Area",0,3,0,"Background|Foreground|Inverse|Unaltered"}},op_tiles},
    {"Stylize","Trace Contour",2,{{SP_SLIDER,"Level",0,255,128},{SP_ENUM,"Edge",0,1,0,"Lower|Upper"}},op_trace_contour},
    {"Stylize","Wind",2,{{SP_ENUM,"Method",0,2,0,"Wind|Blast|Stagger"},{SP_ENUM,"Direction",0,1,0,"Left|Right"}},op_wind},
    {"Stylize","Glowing Edges",3,{{SP_SLIDER,"Edge Width",1,14,2},{SP_SLIDER,"Edge Brightness",0,20,6},{SP_SLIDER,"Smoothness",1,15,5}},op_glowing_edges},
};

void fx_stylize_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
