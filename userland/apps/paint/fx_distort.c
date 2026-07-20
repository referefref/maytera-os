// fx_distort.c - Maytera Studio "Distorts" filter category (op registry).
#include "fx_common.inc"

// remap: for each dst pixel compute a source coord (.8 fixed) via f(); sample src.
static int op_lens(const int *p) {
    int main_amt = p[0], edge = p[1];
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px(); int cx = W / 2, cy = H / 2; int R = (cx < cy ? cx : cy);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int dx = x - cx, dy = y - cy; int d = (int)fx_isqrt(dx * dx + dy * dy);
        int rr = R ? (d * 1024 / R) : 0;                 // 0..1024 normalized radius Q10
        int k = 1024 + (main_amt * rr / 100) + (edge * rr * rr / 102400);
        int sx = (cx << 8) + (dx * k >> 2);
        int sy = (cy << 8) + (dy * k >> 2);
        fx_put(px, src, i, fx_sample(src, W, H, sx, sy), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_whirl(const int *p) {
    int whirl = p[0], pinch = p[1], radius = p[2];
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px(); int cx = W / 2, cy = H / 2;
    int R = (cx < cy ? cx : cy) * radius / 100; if (R < 1) R = 1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int dx = x - cx, dy = y - cy; int d = (int)fx_isqrt(dx * dx + dy * dy);
        if (d >= R) continue;
        int f = (R - d) * (R - d) / (R ? R : 1);          // falloff 0..R
        int ang = fx_atan2(dy, dx) + whirl * f / R;
        int nd = d - (pinch * f / 100);
        int sx = (cx << 8) + (nd * fx_cos(ang) >> 4);
        int sy = (cy << 8) + (nd * fx_sin(ang) >> 4);
        fx_put(px, src, i, fx_sample(src, W, H, sx, sy), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_ripple(const int *p) {
    int orient = p[0], amp = p[1], period = p[2]; if (period < 1) period = 1;
    int wrap = (p[3] == 0);   // Undefined Areas: 0 = Wrap Around, 1 = Repeat Edge Pixels
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int shift = amp * fx_sin(((orient ? x : y) * 360 / period)) >> 12;
        int nx = x, ny = y;
        if (orient) ny = y + shift; else nx = x + shift;
        if (wrap) {                       // wrap the displaced axis (Shear edge-mode parity)
            if (orient) { ny %= H; if (ny < 0) ny += H; }
            else        { nx %= W; if (nx < 0) nx += W; }
        }                                 // else: fx_sample clamps = repeat edge pixels
        fx_put(px, src, i, fx_sample(src, W, H, nx << 8, ny << 8), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_waves(const int *p) {
    int amp = p[0], phase = p[1], wl = p[2]; if (wl < 1) wl = 1;
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px(); int cx = W / 2, cy = H / 2;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int dx = x - cx, dy = y - cy; int d = (int)fx_isqrt(dx * dx + dy * dy);
        int s = amp * fx_sin(d * 360 / wl + phase) >> 12;
        int nd = d + s;
        int ang = fx_atan2(dy, dx);
        int sx = (cx << 8) + (nd * fx_cos(ang) >> 4);
        int sy = (cy << 8) + (nd * fx_sin(ang) >> 4);
        fx_put(px, src, i, fx_sample(src, W, H, sx, sy), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_polar(const int *p) {
    int to_polar = p[0];
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px(); int cx = W / 2, cy = H / 2; int R = (cx < cy ? cx : cy);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int sx, sy;
        if (to_polar) {                                  // dst is polar image
            int ang = x * 360 / (W ? W : 1);
            int rad = y * R / (H ? H : 1);
            sx = (cx << 8) + (rad * fx_cos(ang) >> 4);
            sy = (cy << 8) + (rad * fx_sin(ang) >> 4);
        } else {                                         // dst is cartesian
            int dx = x - cx, dy = y - cy; int d = (int)fx_isqrt(dx * dx + dy * dy);
            int ang = fx_atan2(dy, dx);
            sx = (ang * W / 360) << 8;
            sy = (R ? (d * H / R) : 0) << 8;
        }
        fx_put(px, src, i, fx_sample(src, W, H, sx, sy), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_shift(const int *p) {
    int orient = p[0], amt = p[1];
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) {
        unsigned s = 0x9E3779B9u ^ (unsigned)((orient ? y : 0) * 2654435761u);
        int rowsh = orient ? ((int)(fx_rnd(&s) % (2 * amt + 1)) - amt) : 0;
        for (int x = 0; x < W; x++) {
            int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
            unsigned cs = 0x85EBCA6Bu ^ (unsigned)((orient ? 0 : x) * 2246822519u);
            int colsh = orient ? 0 : ((int)(fx_rnd(&cs) % (2 * amt + 1)) - amt);
            int sx = fx_clamp(x + rowsh, 0, W - 1), sy = fx_clamp(y + colsh, 0, H - 1);
            fx_put(px, src, i, src[sy * W + sx], cov);
        }
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_spherize(const int *p) {
    int amt = p[0];
    int mode = fx_clamp(p[1], 0, 2);   // 0 Normal, 1 Horizontal Only, 2 Vertical Only (PS parity)
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px(); int cx = W / 2, cy = H / 2; int R = (cx < cy ? cx : cy);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int dx = x - cx, dy = y - cy;
        if (mode == 1) dy = 0; else if (mode == 2) dx = 0;   // cylinder along one axis
        int d = (int)fx_isqrt(dx * dx + dy * dy);
        if (d >= R || R == 0) { fx_put(px, src, i, src[i], cov); continue; }
        int rn = d * 1024 / R;                            // 0..1024
        int sph = (int)fx_isqrt((unsigned)(1024 * 1024 - (1024 - rn) * (1024 - rn)));
        int nd = d + (sph - rn) * amt / 100;
        int sx, sy;
        if (mode == 1)      { sx = (cx << 8) + ((dx < 0 ? -nd : nd) << 8); sy = y << 8; }
        else if (mode == 2) { sx = x << 8; sy = (cy << 8) + ((dy < 0 ? -nd : nd) << 8); }
        else {
            int ang = fx_atan2(dy, dx);
            sx = (cx << 8) + (nd * fx_cos(ang) >> 4);
            sy = (cy << 8) + (nd * fx_sin(ang) >> 4);
        }
        fx_put(px, src, i, fx_sample(src, W, H, sx, sy), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_mosaic(const int *p) {
    int t = p[0]; if (t < 2) t = 2;
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int by = 0; by < H; by += t) for (int bx = 0; bx < W; bx += t) {
        long r = 0, g = 0, b = 0, a = 0; int n = 0;
        for (int y = by; y < by + t && y < H; y++) for (int x = bx; x < bx + t && x < W; x++) {
            uint32_t o = src[y * W + x]; r += px_r(o); g += px_g(o); b += px_b(o); a += px_a(o); n++;
        }
        if (!n) continue;
        uint32_t avg = argb((int)(a / n), (int)(r / n), (int)(g / n), (int)(b / n));
        for (int y = by; y < by + t && y < H; y++) for (int x = bx; x < bx + t && x < W; x++) {
            int i = y * W + x, cov = draw_cov(x, y); fx_put(px, src, i, avg, cov);
        }
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_kaleido(const int *p) {
    int seg = p[0]; if (seg < 2) seg = 2;
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px(); int cx = W / 2, cy = H / 2; int wedge = 360 / seg;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int dx = x - cx, dy = y - cy; int d = (int)fx_isqrt(dx * dx + dy * dy);
        int ang = fx_atan2(dy, dx) % wedge;
        if ((ang / (wedge / 2)) & 1) ang = wedge - ang;   // mirror alternate wedges
        int sx = (cx << 8) + (d * fx_cos(ang) >> 4);
        int sy = (cy << 8) + (d * fx_sin(ang) >> 4);
        fx_put(px, src, i, fx_sample(src, W, H, sx, sy), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Newsprint: mono halftone screen (PS parity upgrade). Pattern 0 = Dot (round
// dots on the screen lattice), 1 = Line; Angle rotates the whole screen. The
// per-channel RGB screens live in Pixelate / Color Halftone; this stays mono.
static int op_newsprint(const int *p) {
    int cell = p[0]; if (cell < 2) cell = 2;
    int pat = p[1] ? 1 : 0;
    int ang = p[2];
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    int ca = fx_cos(ang), sa = fx_sin(ang);               // Q12 screen rotation
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int u = (x * ca + y * sa) >> 12;                  // rotated screen coords
        int w2 = (y * ca - x * sa) >> 12;
        int lum = fx_lum(px[i]);
        int v;
        if (pat == 0) {                                   // Dot: radius grows w/ darkness
            int cu = ((u % cell) + cell) % cell - cell / 2;
            int cw = ((w2 % cell) + cell) % cell - cell / 2;
            int dist2 = cu * cu + cw * cw;
            int rad2 = (255 - lum) * cell * cell / 1020;
            v = dist2 <= rad2 ? 0 : 255;
        } else {                                          // Line: band thickens w/ darkness
            int band = ((w2 % cell) + cell) % cell;
            int thick = (255 - lum) * cell / 255;
            v = band < thick ? 0 : 255;
        }
        fx_put(px, px, i, argb(px_a(px[i]), v, v, v), cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_engrave(const int *p) {
    int lh = p[0]; if (lh < 2) lh = 2;
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int lum = fx_lum(px[i]);
        int band = (y % lh);
        int thick = lum * lh / 255;
        int v = band < thick ? 255 : 0;
        fx_put(px, px, i, argb(px_a(px[i]), v, v, v), cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_video(const int *p) {
    int scan = p[0], chroma = p[1];
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int rx = fx_clamp(x - chroma, 0, W - 1), bx = fx_clamp(x + chroma, 0, W - 1);
        int r = px_r(src[y * W + rx]), g = px_g(src[i]), b = px_b(src[y * W + bx]);
        if (scan && (y & 1)) { r = r * 3 / 4; g = g * 3 / 4; b = b * 3 / 4; }
        fx_put(px, src, i, argb(px_a(src[i]), r, g, b), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Diffuse Glow: soft halation on highlights (PS Filter > Distort > Diffuse Glow).
// Color-only op; glow color is white, or g_tool.fg when the check is on
// (fx_edge.c precedent). Mask is built from luminance, box-blurred, then
// speckled with coordinate-seeded noise so previews are byte-identical.

// box-blur one line of n mask samples (radius 4, clamped edges, running sum)
static void dg_boxline(const unsigned char *in, unsigned char *out, int n, int stride) {
    int r = 4, win = 2 * r + 1, sum = 0;
    for (int k = -r; k <= r; k++) sum += in[fx_clamp(k, 0, n - 1) * stride];
    for (int i = 0; i < n; i++) {
        out[i * stride] = (unsigned char)(sum / win);
        sum += in[fx_clamp(i + r + 1, 0, n - 1) * stride]
             - in[fx_clamp(i - r, 0, n - 1) * stride];
    }
}

// one separable pass: horizontal a->b, vertical b->a (result lands back in a)
static void dg_boxblur(unsigned char *a, unsigned char *b, int W, int H) {
    for (int y = 0; y < H; y++) dg_boxline(a + y * W, b + y * W, W, 1);
    for (int x = 0; x < W; x++) dg_boxline(b + x, a + x, H, W);
}

static int op_diffuse_glow(const int *p) {
    int grain = fx_clamp(p[0], 0, 10);
    int glow  = fx_clamp(p[1], 0, 20);
    int clear = fx_clamp(p[2], 0, 20);
    int W = draw_w(), H = draw_h();
    uint32_t *px = draw_px(); if (!px) return -1;
    unsigned char *ma = (unsigned char *)malloc((size_t)W * H);
    unsigned char *mb = (unsigned char *)malloc((size_t)W * H);
    if (!ma || !mb) { free(ma); free(mb); return -1; }

    // pass 1: highlight mask from ORIGINAL pixels; higher Clear raises the
    // threshold so more of the image stays clear (do not wire it as a gain)
    int T = fx_clamp(clear * 12, 0, 240);
    int gain = glow * 26;                              // Glow 10 ~ unity gain
    for (int i = 0; i < W * H; i++) {
        int lum = fx_lum(px[i]);
        int m = (lum <= T) ? 0 : ((lum - T) * 255) / (255 - T);
        ma[i] = (unsigned char)fx_clamp((m * gain) >> 8, 0, 255);
    }

    // pass 2: diffuse the mask (separable box blur r=4, run twice) for the
    // soft halation falloff, then add speckle to the MASK, not the pixels
    dg_boxblur(ma, mb, W, H);
    dg_boxblur(ma, mb, W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned s = 0x9E3779B9u ^ (unsigned)i;        // i = y * W + x
        int n = (int)(fx_rnd(&s) % (unsigned)(grain * 24 + 1)) - grain * 12;
        ma[i] = (unsigned char)fx_clamp((int)ma[i] + n, 0, 255);
    }

    // pass 3: composite toward the glow color by mask strength; alpha kept
    uint32_t C = p[3] ? g_tool.fg : 0xFFFFFFu;
    int Cr = px_r(C), Cg = px_g(C), Cb = px_b(C);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t o = px[i]; int m = ma[i];
        int r = px_r(o) + ((Cr - px_r(o)) * m) / 255;
        int g = px_g(o) + ((Cg - px_g(o)) * m) / 255;
        int b = px_b(o) + ((Cb - px_b(o)) * m) / 255;
        fx_put(px, px, i, argb(px_a(o), r, g, b), cov);
    }
    free(ma); free(mb);
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Ocean Ripple: random glassy displacement (PS Filter Gallery / Glass family),
// NOT a directional sine wave. Smooth 2-channel lattice value noise, 2 octaves.

// Deterministic Q12 lattice value in [-4096,4096] at lattice point (gx,gy);
// seed = fixed base constant ^ coordinate hash, never rand() (contract A5).
static int or_lat(unsigned base, int gx, int gy) {
    unsigned s = base ^ ((unsigned)gy * 19349663u ^ (unsigned)gx * 73856093u);
    if (!s) s = 0x6D2B79F5u;                    // xorshift32 must never run on 0
    return (int)(fx_rnd(&s) % 8193u) - 4096;
}

// One octave of smooth value noise at pixel (x,y), lattice cell size 'cell' (>=2).
// Fractions blended with Q8 smoothstep weights (3t^2 - 2t^3); returns Q12.
static int or_noise(unsigned base, int x, int y, int cell) {
    int gx = x / cell, gy = y / cell;
    int tx = ((x - gx * cell) << 8) / cell;
    int ty = ((y - gy * cell) << 8) / cell;
    tx = (tx * tx * (768 - 2 * tx)) >> 16;      // Q8 in, Q8 out
    ty = (ty * ty * (768 - 2 * ty)) >> 16;
    int a = or_lat(base, gx, gy),     b = or_lat(base, gx + 1, gy);
    int c = or_lat(base, gx, gy + 1), d = or_lat(base, gx + 1, gy + 1);
    return fx_bilin_ch(a, b, c, d, tx, ty);
}

// Two octaves for the choppy glassy look: full amplitude at cell c1 plus half
// amplitude at cell c2, clamped back to Q12 so Magnitude m stays +/- m px.
static int or_noise2(unsigned base, int x, int y, int c1, int c2) {
    int n = or_noise(base, x, y, c1) + (or_noise(base ^ 0x27D4EB2Fu, x, y, c2) >> 1);
    return fx_clamp(n, -4096, 4096);
}

static int op_ocean_ripple(const int *p) {
    int cell = p[0] * 2; if (cell < 2) cell = 2;   // Ripple Size -> noise wavelength
    int mag = p[1]; if (mag < 0) mag = 0;          // displacement amplitude in pixels
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int cell2 = cell / 2; if (cell2 < 2) cell2 = 2;
    if (mag) {                                     // Magnitude 0 is an exact no-op
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
            int nx = or_noise2(0x9E3779B9u, x, y, cell, cell2);
            int ny = or_noise2(0x85EBCA6Bu, x, y, cell, cell2);
            int dx = (nx * mag) >> 4;              // Q12 * px -> .8 fixed, +/- mag px
            int dy = (ny * mag) >> 4;
            fx_put(px, src, i, fx_sample(src, W, H, (x << 8) + dx, (y << 8) + dy), cov);
        }
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Shear (Photoshop Distort > Shear): horizontal-only row displacement driven by
// the interactive SP_CURVE editor. Curve x (0..255) is the row position top to
// bottom; the deviation of curve y from the identity diagonal is the horizontal
// push (above the diagonal shifts right, below shifts left), so an unedited
// curve is an exact no-op. Full curve deflection shears +/- half the image width,
// matching the extent of Photoshop's Shear grid. The R/G/B channel tabs are
// surplus here, so the three LUTs are averaged; composite-only editing (the
// normal case, lr==lg==lb) reproduces Photoshop exactly. The SP_CURVE param
// slot p[0] is a placeholder and is never read; p[1] selects Undefined Areas.
static int op_shear(const int *p) {
    int repeat = (p[1] != 0);            // 0 = Wrap Around (PS default), 1 = Repeat Edge Pixels
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int lr[256], lg[256], lb[256];
    curve_get_lut3(lr, lg, lb);
    int Wfp = W << 8;                    // row width in Q8.8
    for (int y = 0; y < H; y++) {
        int idx = (H > 1) ? y * 255 / (H - 1) : 0;
        int v = (lr[idx] + lg[idx] + lb[idx]) / 3;
        int d = v - idx;                 // signed displacement, identity curve gives 0
        int dx_fp = d * Wfp / 510;       // Q8.8 shift; |d|=255 gives +/- W/2 (fits int: 255*4096*256 < 2^31)
        for (int x = 0; x < W; x++) {
            int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
            int sx_fp = (x << 8) - dx_fp;
            uint32_t res;
            if (repeat) {                // clamp; fx_sample repeats the edge pixel
                sx_fp = fx_clamp(sx_fp, 0, (W - 1) << 8);
                res = fx_sample(src, W, H, sx_fp, y << 8);
            } else {                     // wrap-aware 2-tap blend so the seam wraps cleanly
                sx_fp = ((sx_fp % Wfp) + Wfp) % Wfp;
                int x0 = sx_fp >> 8, f = sx_fp & 255;
                int x1 = (x0 + 1 == W) ? 0 : x0 + 1;
                uint32_t A = src[y * W + x0], B = src[y * W + x1];
                res = argb(px_a(A) + (((px_a(B) - px_a(A)) * f) >> 8),
                           px_r(A) + (((px_r(B) - px_r(A)) * f) >> 8),
                           px_g(A) + (((px_g(B) - px_g(A)) * f) >> 8),
                           px_b(A) + (((px_b(B) - px_b(A)) * f) >> 8));
            }
            fx_put(px, src, i, res, cov);
        }
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ZigZag: concentric sine ripples about the canvas center, decaying to zero at
// the corner radius. Style picks the displacement direction (PS parity).
static int op_zigzag(const int *p) {
    int amount = p[0], ridges = p[1], style = p[2];
    if (ridges < 1) ridges = 1;                       // Reset/preview can hand edge values
    if (style < 0) style = 0;
    if (style > 2) style = 2;
    if (amount == 0) return 0;                        // identity, nothing to do
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px(); int cx = W / 2, cy = H / 2;
    int rmax = (int)fx_isqrt((unsigned)(cx * cx + cy * cy)); if (rmax < 1) rmax = 1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int dx = x - cx, dy = y - cy;
        int r = (int)fx_isqrt((unsigned)(dx * dx + dy * dy));
        int theta = (r * ridges * 360) / rmax;        // fx_sin reduces mod 360
        int wave = fx_sin(theta);                     // Q12 in [-4096,4096]
        // .8 fixed-point displacement; |amount| = peak shift in px, fading to 0 at rmax
        int d8 = (int)(((long)amount * wave * (rmax - r)) / (16L * rmax));
        int sx8 = x << 8, sy8 = y << 8;
        if (style == 0) {                             // Around Center: tangent (-dy,dx)/r
            if (r > 0) { sx8 += d8 * -dy / r; sy8 += d8 * dx / r; }
        } else if (style == 1) {                      // Out From Center: radial (dx,dy)/r
            if (r > 0) { sx8 += d8 * dx / r; sy8 += d8 * dy / r; }
        } else {                                      // Pond Ripples: diagonal shift
            sx8 += d8; sy8 += d8;
        }
        fx_put(px, src, i, fx_sample(src, W, H, sx8, sy8), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static const studio_op_t OPS[] = {
    {"Distorts","Lens Distortion",2,{{SP_SLIDER,"Main",-100,100,0},{SP_SLIDER,"Edge",-100,100,0}},op_lens},
    {"Distorts","Whirl and Pinch",3,{{SP_SLIDER,"Whirl",-360,360,90},{SP_SLIDER,"Pinch",-100,100,0},{SP_SLIDER,"Radius",10,100,100}},op_whirl},
    {"Distorts","Ripple",4,{{SP_ENUM,"Orientation",0,1,0,"Vertical|Horizontal"},{SP_SLIDER,"Amplitude",0,64,8},{SP_SLIDER,"Period",2,200,32},{SP_ENUM,"Undefined Areas",0,1,1,"Wrap Around|Repeat Edge Pixels"}},op_ripple},
    {"Distorts","Waves",3,{{SP_SLIDER,"Amplitude",0,64,10},{SP_SLIDER,"Phase",0,360,0},{SP_SLIDER,"Wavelength",2,200,30}},op_waves},
    {"Distorts","Polar Coordinates",1,{{SP_ENUM,"Mode",0,1,1,"From Polar|To Polar"}},op_polar},
    {"Distorts","Shift",2,{{SP_ENUM,"Orientation",0,1,1,"Vertical|Horizontal"},{SP_SLIDER,"Amount",1,64,8}},op_shift},
    {"Distorts","Spherize",2,{{SP_SLIDER,"Amount",-100,100,50},{SP_ENUM,"Mode",0,2,0,"Normal|Horizontal Only|Vertical Only"}},op_spherize},
    {"Distorts","Mosaic",1,{{SP_SLIDER,"Tile Size",2,64,10}},op_mosaic},
    {"Distorts","Kaleidoscope",1,{{SP_SLIDER,"Segments",2,12,6}},op_kaleido},
    {"Distorts","Newsprint",3,{{SP_SLIDER,"Cell Size",2,32,6},{SP_ENUM,"Pattern",0,1,0,"Dot|Line"},{SP_ANGLE,"Angle",0,360,45}},op_newsprint},
    {"Distorts","Engrave",1,{{SP_SLIDER,"Line Height",2,32,8}},op_engrave},
    {"Distorts","Video Degrade",2,{{SP_CHECK,"Scanlines",0,1,1},{SP_SLIDER,"Chroma Shift",0,16,3}},op_video},
    {"Distorts","Diffuse Glow",4,{{SP_SLIDER,"Graininess",0,10,6},{SP_SLIDER,"Glow Amount",0,20,10},{SP_SLIDER,"Clear Amount",0,20,15},{SP_CHECK,"Use FG Color",0,1,0}},op_diffuse_glow},
    {"Distorts","Ocean Ripple",2,{{SP_SLIDER,"Ripple Size",1,15,9},{SP_SLIDER,"Ripple Magnitude",0,20,9}},op_ocean_ripple},
    {"Distorts","Shear",2,{{SP_CURVE,"Shear Curve",0,0,0,0},{SP_ENUM,"Undefined Areas",0,1,0,"Wrap Around|Repeat Edge Pixels"}},op_shear},
    {"Distorts","ZigZag",3,{{SP_SLIDER,"Amount",-100,100,10},{SP_SLIDER,"Ridges",1,20,5},{SP_ENUM,"Style",0,2,2,"Around Center|Out From Center|Pond Ripples"}},op_zigzag},
};
void fx_distort_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
