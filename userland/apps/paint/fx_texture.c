// fx_texture.c - Studio "Texture" filter category (Photoshop Filter Gallery
// parity): Craquelure, Grain, Mosaic Tiles, Patchwork, Stained Glass,
// Texturizer. All ops are deterministic under the restore-then-apply live
// preview loop (coordinate-seeded fx_rnd, no carried RNG state), preserve
// alpha, write through fx_put with draw_cov, and free all scratch.
#include "fx_common.inc"

// ---------------------------------------------------------------------------
// Texture / Craquelure: Worley-edge crack network over plaster relief.
// Color-only op (no geometric resampling): each pixel's crack mask is a pure
// function of (x, y, params), so preview re-apply on the restored original is
// deterministic and idempotent-safe.

// One jittered feature point per grid cell; returns the crack mask m in 0..256.
// m -> 0 exactly on cell boundaries (F2 == F1), 256 deep inside a cell.
static int craq_mask(int x, int y, int S) {
    int cx = x / S, cy = y / S;
    unsigned f1 = 0xFFFFFFFFu, f2 = 0xFFFFFFFFu;
    for (int j = cy - 1; j <= cy + 1; j++) {
        for (int i = cx - 1; i <= cx + 1; i++) {
            unsigned s = 0x9E3779B9u ^ (unsigned)(i * 73856093) ^ (unsigned)(j * 19349663);
            if (!s) s = 0x6D2B79F5u;                  // xorshift32 must never run from 0
            unsigned r1 = fx_rnd(&s), r2 = fx_rnd(&s);
            int fxp = i * S + (int)(r1 % (unsigned)S); // jitter spans the cell (amp ~ S/2)
            int fyp = j * S + (int)(r2 % (unsigned)S);
            int dx = x - fxp, dy = y - fyp;
            unsigned d2 = (unsigned)(dx * dx + dy * dy);
            if (d2 < f1) { f2 = f1; f1 = d2; }
            else if (d2 < f2) f2 = d2;
        }
    }
    int F1 = (int)fx_isqrt(f1), F2 = (int)fx_isqrt(f2);
    int half = S >> 1; if (half < 1) half = 1;
    return fx_clamp(((F2 - F1) << 8) / half, 0, 256);
}

static int op_craquelure(const int *p) {
    int S = fx_clamp(p[0], 2, 100);                   // crack spacing = cell pitch
    int depth = fx_clamp(p[1], 0, 10);
    int bright = fx_clamp(p[2], 0, 10);
    int W = draw_w(), H = draw_h();
    if (W < 1 || H < 1) return 0;
    uint32_t *px = draw_px(); if (!px) return -1;
    // Two mask rows so the ridge term can read m at (x-1, y-1) without
    // recomputing the Worley field twice per pixel.
    int *rows = (int *)malloc((size_t)W * 2 * sizeof(int));
    if (!rows) return -1;
    int *prev = rows, *cur = rows + W;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) cur[x] = craq_mask(x, y, S);
        for (int x = 0; x < W; x++) {
            int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
            int m = cur[x];
            int m_ul = (x > 0 && y > 0) ? prev[x - 1] : m;  // border: ridge term 0
            uint32_t o = px[i];
            // Valley shading: depth 10 nearly blackens crack centers, depth 0 is a noop.
            int shade = 256 - depth * 24 * (256 - m) / 256;
            // Ridge lighting (fixed top-left light) plus flat plateau lift.
            int add = 0, dm = m - m_ul;
            if (dm > 0) add = (dm * bright * 20) >> 8;
            add += (bright * 10 * m) >> 8;
            int r = fx_clamp(((px_r(o) * shade) >> 8) + add, 0, 255);
            int g = fx_clamp(((px_g(o) * shade) >> 8) + add, 0, 255);
            int b = fx_clamp(((px_b(o) * shade) >> 8) + add, 0, 255);
            fx_put(px, px, i, argb(px_a(o), r, g, b), cov);
        }
        int *t = prev; prev = cur; cur = t;
    }
    free(rows);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Texture / Grain: Photoshop Filter Gallery grain with 10 grain types.
// Color-only op, no fx_dup: pure per-pixel work; the Soft type averages
// noise values that are pure functions of coordinates, never image reads.

// Deterministic per-coordinate noise in [-amp, amp]. Fixed base seed xor a
// hashed coordinate key, so the pattern is stable across the live-preview
// restore-then-apply loop (a running RNG state would make the grain crawl).
static int grain_noise(unsigned key, int amp) {
    unsigned s = 0xA5F15EEDu ^ (key * 2654435761u);
    return (int)(fx_rnd(&s) % (unsigned)(2 * amp + 1)) - amp;
}

static int op_grain(const int *p) {
    int inten = fx_clamp(p[0], 0, 100);
    int contr = fx_clamp(p[1], 0, 100);
    int type  = fx_clamp(p[2], 0, 9);
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px(); if (!px) return -1;
    int amp = inten * 127 / 100;               // noise amplitude from Intensity
    if (inten > 0 && amp < 1) amp = 1;
    // Contrast gain around mid-gray: kq = contr*256/100 gives 128 at
    // Contrast 50, and the >>7 makes that exactly unity (blueprint rule:
    // Contrast 50 must be unity so switching grain types compares cleanly).
    int kq = contr * 256 / 100;
    int mix = inten * 255 / 100;               // Stippled blend ratio, Q8
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t o = px[i];
        int r = px_r(o), g = px_g(o), b = px_b(o);
        switch (type) {
        case 2: {                              // Sprinkles: sparse bg-color dots
            unsigned s = 0xA5F15EEDu ^ ((unsigned)i * 2654435761u);
            if ((int)(fx_rnd(&s) % 1000u) < inten * 10 / 4) {
                r = px_r(g_tool.bg); g = px_g(g_tool.bg); b = px_b(g_tool.bg);
            }
            break;
        }
        case 6: {                              // Stippled: push toward fg/bg
            int n = grain_noise((unsigned)i, amp);
            uint32_t t = (fx_lum(o) + n >= 128) ? g_tool.bg : g_tool.fg;
            r += (px_r(t) - r) * mix >> 8;
            g += (px_g(t) - g) * mix >> 8;
            b += (px_b(t) - b) * mix >> 8;
            break;
        }
        case 9: {                              // Speckle: independent RGB draws
            unsigned s = 0xA5F15EEDu ^ ((unsigned)i * 2654435761u);
            unsigned m = (unsigned)(2 * amp + 1);
            r += (int)(fx_rnd(&s) % m) - amp;
            g += (int)(fx_rnd(&s) % m) - amp;
            b += (int)(fx_rnd(&s) % m) - amp;
            break;
        }
        default: {                             // additive monochrome noise
            int n;
            if (type == 1) {                   // Soft: 5-tap noise average
                n = (grain_noise((unsigned)i, amp)
                   + grain_noise((unsigned)((y - 1) * W + x), amp)
                   + grain_noise((unsigned)((y + 1) * W + x), amp)
                   + grain_noise((unsigned)(y * W + x - 1), amp)
                   + grain_noise((unsigned)(y * W + x + 1), amp)) / 5;
            } else {
                unsigned key;
                switch (type) {
                case 3:  key = (unsigned)((y >> 1) * W + (x >> 1)); break; // Clumped
                case 5:  key = (unsigned)((y >> 2) * W + (x >> 2)); break; // Enlarged
                case 7:  key = (unsigned)(y * W + (x >> 3)); break;        // Horizontal, 8 px streaks
                case 8:  key = (unsigned)((y >> 3) * W + x); break;        // Vertical
                default: key = (unsigned)i; break;                         // Regular, Contrasty
                }
                n = grain_noise(key, amp);
                if (type == 4) n = (n >= 0) ? amp : -amp;  // Contrasty: hard specks
            }
            r += n; g += n; b += n;
            break;
        }
        }
        r = fx_clamp(128 + ((r - 128) * kq >> 7), 0, 255);
        g = fx_clamp(128 + ((g - 128) * kq >> 7), 0, 255);
        b = fx_clamp(128 + ((b - 128) * kq >> 7), 0, 255);
        fx_put(px, px, i, argb(px_a(o), r, g, b), cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Texture / Mosaic Tiles: wavy-edged tile shards separated by mortar grout.
// PS parity: Tile Size 2..64 (blueprint cap, PS def 12), Grout Width 1..15
// (def 3), Lighten Grout 0..10 (def 9). Color-only op: no fx_dup, no scratch;
// tile interiors keep the original pixels, only grout and the 1-step bevel
// ring modify anything. Grout brightness tracks Lighten Grout while keeping a
// hint of the underlying image, so there is no SP_COLOR swatch by design.
static int op_mosaic_tiles(const int *p) {
    int ts = fx_clamp(p[0], 2, 64);                      // clamp degenerate inputs
    int gw = fx_clamp(p[1], 1, ts - 1);
    int lg = p[2];
    lg = fx_clamp(lg, 0, 10);
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px(); if (!px) return -1;
    int T = 40 + lg * 20;                                // mortar tone 40..240
    int half = (gw + 1) / 2;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        // Per-cell phase jitter, deterministic across preview re-runs: seed
        // from the unwobbled cell only (fixed constant ^ cell hash, never rand).
        unsigned seed = 0x9E3779B9u
            ^ ((unsigned)(x / ts) * 2654435761u + (unsigned)(y / ts) * 40503u);
        int jit = (int)(fx_rnd(&seed) % 61) - 30;        // +/-30 degrees
        // Sine wobble so grout lines wave instead of forming a straight grid
        // (a plain modulo grid would read as Pixelate/Mosaic, not Mosaic Tiles).
        int u = x + ((fx_sin(((y * 90) / ts + 45 + jit) % 360) * ts) >> 14);
        int v = y + ((fx_sin(((x * 90) / ts + jit) % 360) * ts) >> 14);
        if (u < 0) u = 0;
        if (v < 0) v = 0;
        int lx = u % ts, ly = v % ts;                    // in-cell offsets
        int dl = lx, dr = ts - 1 - lx, dt = ly, db = ts - 1 - ly;
        int d = dl;                                      // distance to nearest grout line
        if (dr < d) d = dr;
        if (dt < d) d = dt;
        if (db < d) d = db;
        uint32_t o = px[i];
        if (d < half) {                                  // grout: gray mortar with a
            int c = (3 * T + fx_lum(o)) / 4;             // hint of the image beneath
            fx_put(px, px, i, argb(px_a(o), c, c, c), cov);
        } else if (d == half) {                          // one-step bevel for tile relief:
            int e = (dl == d || dt == d) ? 24 : -24;     // lit above/left, shaded below/right
            int r = fx_clamp(px_r(o) + e, 0, 255);
            int g = fx_clamp(px_g(o) + e, 0, 255);
            int b = fx_clamp(px_b(o) + e, 0, 255);
            fx_put(px, px, i, argb(px_a(o), r, g, b), cov);
        }
        // all other pixels: tile interior, pass through unchanged
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Texture / Patchwork: the image becomes flat averaged squares with a
// deterministic per-tile depth and a 1 px relief bevel, after
// Photoshop Filter > Texture > Patchwork. Square Size is PS's abstract
// 0..10 unit mapped to a 3..23 px cell pitch (S = 3 + 2*n); Relief drives
// both the random tile depth and the edge shading, and Relief 0 still
// tiles (flat mosaic squares), it is not a no-op.
static int op_patchwork(const int *p) {
    int S = 3 + 2 * fx_clamp(p[0], 0, 10); if (S < 2) S = 2;
    int relief = fx_clamp(p[1], 0, 25);
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int bev = relief * 3;
    for (int by = 0, cy = 0; by < H; by += S, cy++) for (int bx = 0, cx = 0; bx < W; bx += S, cx++) {
        // clipped cell bounds (right/bottom edge cells average over their real pixel count)
        int x2 = bx + S - 1; if (x2 > W - 1) x2 = W - 1;
        int y2 = by + S - 1; if (y2 > H - 1) y2 = H - 1;
        long r = 0, g = 0, b = 0; int n = 0;
        for (int y = by; y <= y2; y++) for (int x = bx; x <= x2; x++) {
            uint32_t o = src[y * W + x]; r += px_r(o); g += px_g(o); b += px_b(o); n++;
        }
        if (!n) continue;
        int ar = (int)(r / n), ag = (int)(g / n), ab = (int)(b / n);
        // deterministic per-tile depth in [-relief, +relief]: seed from cell
        // coordinates only (never rand(), no carried state), so the live-preview
        // loop's repeated apply() calls on the restored original never shimmer.
        int delta = 0;
        if (relief) {
            unsigned seed = 0x9D2C5681u ^ ((unsigned)cx * 73856093u ^ (unsigned)cy * 19349663u);
            delta = ((int)(fx_rnd(&seed) % (unsigned)(2 * relief + 1)) - relief) * 5;
        }
        int tr = fx_clamp(ar + delta, 0, 255);
        int tg = fx_clamp(ag + delta, 0, 255);
        int tb = fx_clamp(ab + delta, 0, 255);
        for (int y = by; y <= y2; y++) for (int x = bx; x <= x2; x++) {
            int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
            int rr = tr, gg = tg, bb = tb;
            if (relief) {                                 // 1 px bevel fakes tile relief
                if (y == by || x == bx) {                 // lit top/left edges
                    rr = fx_clamp(rr + bev, 0, 255);
                    gg = fx_clamp(gg + bev, 0, 255);
                    bb = fx_clamp(bb + bev, 0, 255);
                } else if (y == y2 || x == x2) {          // shaded bottom/right edges
                    rr = fx_clamp(rr - bev, 0, 255);
                    gg = fx_clamp(gg - bev, 0, 255);
                    bb = fx_clamp(bb - bev, 0, 255);
                }
            }
            // color-only op: alpha stays the original pixel's alpha per pixel
            // (average alpha is deliberately not written, preserving edge transparency)
            fx_put(px, src, i, argb(px_a(src[i]), rr, gg, bb), cov);
        }
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// ---------------------------------------------------------------------------
// Texture / Stained Glass: jittered-grid Voronoi cells filled with a 3x3 box
// average around each cell seed, Voronoi edges drawn in the CURRENT foreground
// color (g_tool.fg, Photoshop convention, fx_edge.c precedent), plus a fixed
// center backlight (Light Intensity) that brightens fills toward the image
// center while the leading stays unlit. Deterministic: seeds come from fx_rnd
// with a fixed constant ^ grid coordinates, so every preview re-apply on the
// restored original yields the identical tessellation.
static int op_stained_glass(const int *p) {
    int cell = p[0] < 2 ? 2 : p[0];
    int border = p[1] < 1 ? 1 : p[1];
    int light = fx_clamp(p[2], 0, 10);
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int gw = (W + cell - 1) / cell, gh = (H + cell - 1) / cell;
    if (gw < 1) gw = 1;
    if (gh < 1) gh = 1;
    int *seeds = (int *)malloc((size_t)gw * gh * 2 * sizeof(int));
    uint32_t *fill = (uint32_t *)malloc((size_t)gw * gh * sizeof(uint32_t));
    if (!seeds || !fill) {
        if (seeds) free(seeds);
        if (fill) free(fill);
        free(src);
        return -1;                                    // OOM, nothing changed
    }
    // One deterministic jittered seed per grid cell; fill = 3x3 box average
    // around the seed (closer to Photoshop's flat region average than a
    // single-pixel sample, without a second full-image pass).
    for (int gy = 0; gy < gh; gy++) for (int gx = 0; gx < gw; gx++) {
        unsigned s = 0xC2B2AE35u ^ (unsigned)(gx * 73856093) ^ (unsigned)(gy * 19349663);
        int sx = fx_clamp(gx * cell + (int)(fx_rnd(&s) % (unsigned)cell), 0, W - 1);
        int sy = fx_clamp(gy * cell + (int)(fx_rnd(&s) % (unsigned)cell), 0, H - 1);
        int ci = gy * gw + gx;
        seeds[ci * 2] = sx; seeds[ci * 2 + 1] = sy;
        int sr = 0, sg = 0, sb = 0, n = 0;
        for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++) {
            int qx = fx_clamp(sx + ox, 0, W - 1), qy = fx_clamp(sy + oy, 0, H - 1);
            uint32_t q = src[qy * W + qx];
            sr += px_r(q); sg += px_g(q); sb += px_b(q); n++;
        }
        fill[ci] = argb(255, sr / n, sg / n, sb / n);
    }
    int fgr = px_r(g_tool.fg), fgg = px_g(g_tool.fg), fgb = px_b(g_tool.fg);
    int ccx = W / 2, ccy = H / 2;
    int rmax = (int)fx_isqrt((unsigned)(ccx * ccx + ccy * ccy));
    if (rmax < 1) rmax = 1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int pgx = x / cell, pgy = y / cell;
        // nearest + second-nearest seed among the 3x3 neighboring grid cells
        int d1 = 0x7FFFFFFF, d2 = 0x7FFFFFFF, best = pgy * gw + pgx;
        for (int gy = pgy - 1; gy <= pgy + 1; gy++) {
            if (gy < 0 || gy >= gh) continue;
            for (int gx = pgx - 1; gx <= pgx + 1; gx++) {
                if (gx < 0 || gx >= gw) continue;
                int ci = gy * gw + gx;
                int dx = x - seeds[ci * 2], dy = y - seeds[ci * 2 + 1];
                int d = dx * dx + dy * dy;
                if (d < d1) { d2 = d1; d1 = d; best = ci; }
                else if (d < d2) d2 = d;
            }
        }
        uint32_t o = src[i];
        uint32_t res;
        if ((int)fx_isqrt((unsigned)d2) - (int)fx_isqrt((unsigned)d1) < border) {
            res = argb(px_a(o), fgr, fgg, fgb);       // leading, stays unlit
        } else {
            int dxc = x - ccx, dyc = y - ccy;
            int r = (int)fx_isqrt((unsigned)(dxc * dxc + dyc * dyc));
            int gain = 256 + light * 256 * (rmax - r) / (rmax * 10);   // Q8
            uint32_t f = fill[best];
            int rr = fx_clamp((px_r(f) * gain) >> 8, 0, 255);
            int gg = fx_clamp((px_g(f) * gain) >> 8, 0, 255);
            int bb = fx_clamp((px_b(f) * gain) >> 8, 0, 255);
            res = argb(px_a(o), rr, gg, bb);          // preserve alpha
        }
        fx_put(px, src, i, res, cov);
    }
    free(fill); free(seeds); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// ---------------------------------------------------------------------------
// Texture / Texturizer. Color-only bump map: the height field is a pure
// function of (x,y) so no fx_dup snapshot is needed; alpha is preserved.

// Deterministic 2-D coordinate hash (blueprint shared helper: hoist to
// fx_common.inc as fx_hash2 and dedupe with the other texture-panel filters).
static unsigned fx_hash2(unsigned seed, int x, int y) {
    unsigned s = seed ^ ((unsigned)x * 73856093u) ^ ((unsigned)y * 19349663u);
    if (!s) s = 0x6D2B79F5u;                    // xorshift32 must not start at 0
    fx_rnd(&s);
    return fx_rnd(&s);
}

// Shared procedural height field for the four Texturizer-panel filters
// (Texturizer, Rough Pastels, Underpainting, Conte Crayon); blueprint says to
// hoist this to fx_common.inc so all four render identically. Returns 0..255.
// type: 0 Brick, 1 Burlap, 2 Canvas, 3 Sandstone. Scaling is a percent
// (50..200): pattern coordinates are divided by scaling/100, so Scaling 200
// doubles the feature size, matching the Photoshop dial.
static int fx_texture_height(int type, int scaling, int x, int y) {
    scaling = fx_clamp(scaling, 50, 200);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    int sx = x * 100 / scaling, sy = y * 100 / scaling;
    switch (type) {
    case 0: {                                   // Brick: running-bond 48x24 cells
        int bw = 48, bh = 24, g = 1 + bh / 16;  // mortar groove, always >= 1
        int row = sy / bh;
        int xo = sx + ((row & 1) ? bw / 2 : 0); // odd rows offset half a brick
        int col = xo / bw, mx = xo % bw, my = sy % bh;
        if (mx < g || my < g) return 40;        // inside the mortar groove
        return 220 - (int)(fx_hash2(0x42524B31u, col, row) % 24); // face jitter
    }
    case 1:
    case 2: {                                   // Burlap (coarse) / Canvas (fine)
        int t = (type == 1) ? 12 : 6;           // thread period in scaled px
        int half = t / 2;                       // abs(sin) ridge period
        int ph = ((sx / half + sy / half) & 1) ? 90 : 0; // over/under weave flip
        int hx = fx_sin(sx * 360 / t + ph); if (hx < 0) hx = -hx;
        int hy = fx_sin(sy * 360 / t + ph); if (hy < 0) hy = -hy;
        if (type == 1) return (hx + hy) * 255 / 8192;
        return 64 + (hx + hy) * 255 / 16384;    // half amplitude, mid-gray bias
    }
    default: {                                  // Sandstone: value noise + grain
        int L = 8;                              // lattice step in scaled px
        int gx = sx / L, gy = sy / L;
        int tx = (sx % L) * 256 / L, ty = (sy % L) * 256 / L;
        int h00 = (int)(fx_hash2(0x53414E44u, gx,     gy    ) & 255);
        int h10 = (int)(fx_hash2(0x53414E44u, gx + 1, gy    ) & 255);
        int h01 = (int)(fx_hash2(0x53414E44u, gx,     gy + 1) & 255);
        int h11 = (int)(fx_hash2(0x53414E44u, gx + 1, gy + 1) & 255);
        int h = fx_bilin_ch(h00, h10, h01, h11, tx, ty);
        h += (int)(fx_hash2(0x47524E31u, x, y) % 33) - 16;   // per-pixel grain
        return fx_clamp(h, 0, 255);
    }
    }
}

static int op_texturizer(const int *p) {
    int type    = fx_clamp(p[0], 0, 3);
    int scaling = fx_clamp(p[1], 50, 200);
    int relief  = fx_clamp(p[2], 0, 50);        // 0 = flat no-op shade
    int angle   = p[3];
    int invert  = p[4];
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px(); if (!px) return -1;
    unsigned char *hb = (unsigned char *)malloc((size_t)W * H);
    if (!hb) return -1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int h = fx_texture_height(type, scaling, x, y);
        hb[y * W + x] = (unsigned char)(invert ? 255 - h : h);
    }
    // Shared compass mapping, y-down screen space: light vector is
    // (fx_cos(a), fx_sin(a)) in Q12, same convention as SP_ANGLE / ui_atan2.
    int lx = fx_cos(angle), ly = fx_sin(angle);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int x0 = x > 0 ? x - 1 : 0, x1 = x < W - 1 ? x + 1 : W - 1;
        int y0 = y > 0 ? y - 1 : 0, y1 = y < H - 1 ? y + 1 : H - 1;
        int gx = hb[y * W + x1] - hb[y * W + x0];   // central difference
        int gy = hb[y1 * W + x] - hb[y0 * W + x];
        int shade = ((gx * lx + gy * ly) >> 12) * relief / 8;
        shade = fx_clamp(shade, -160, 160);
        uint32_t o = px[i];
        int R = fx_clamp(px_r(o) + (px_r(o) + 64) * shade / 256, 0, 255);
        int G = fx_clamp(px_g(o) + (px_g(o) + 64) * shade / 256, 0, 255);
        int B = fx_clamp(px_b(o) + (px_b(o) + 64) * shade / 256, 0, 255);
        fx_put(px, px, i, argb(px_a(o), R, G, B), cov);
    }
    free(hb);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---------------------------------------------------------------------------
static const studio_op_t OPS[] = {
    {"Texture","Craquelure",3,{{SP_SLIDER,"Crack Spacing",2,100,15},{SP_SLIDER,"Crack Depth",0,10,6},{SP_SLIDER,"Crack Brightness",0,10,9}},op_craquelure},
    {"Texture","Grain",3,{{SP_SLIDER,"Intensity",0,100,40},{SP_SLIDER,"Contrast",0,100,50},{SP_ENUM,"Grain Type",0,9,0,"Regular|Soft|Sprinkles|Clumped|Contrasty|Enlarged|Stippled|Horizontal|Vertical|Speckle"}},op_grain},
    {"Texture","Mosaic Tiles",3,{{SP_SLIDER,"Tile Size",2,64,12},{SP_SLIDER,"Grout Width",1,15,3},{SP_SLIDER,"Lighten Grout",0,10,9}},op_mosaic_tiles},
    {"Texture","Patchwork",2,{{SP_SLIDER,"Square Size",0,10,4},{SP_SLIDER,"Relief",0,25,8}},op_patchwork},
    {"Texture","Stained Glass",3,{{SP_SLIDER,"Cell Size",2,64,10},{SP_SLIDER,"Border Thickness",1,20,4},{SP_SLIDER,"Light Intensity",0,10,3}},op_stained_glass},
    {"Texture","Texturizer",5,{{SP_ENUM,"Texture",0,3,2,"Brick|Burlap|Canvas|Sandstone"},{SP_SLIDER,"Scaling",50,200,100},{SP_SLIDER,"Relief",0,50,4},{SP_ANGLE,"Light Direction",0,360,225},{SP_CHECK,"Invert",0,1,0}},op_texturizer},
};

void fx_texture_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
