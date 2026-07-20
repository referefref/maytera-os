// fx_brushstrokes.c - Maytera Studio "Brush Strokes" filter category (op registry).
#include "fx_common.inc"

// ---------------------------------------------------------------------------
// Accented Edges (Brush Strokes): smooth luma, Sobel accent mask, dilate,
// then darken (ink) or brighten (chalk) edge pixels around a Brightness=25 pivot.
// Helpers are file-static byte-plane utilities (ae_ prefix).

static inline int ae_at(const unsigned char *pl, int W, int H, int x, int y) {
    if (x < 0) x = 0; else if (x >= W) x = W - 1;
    if (y < 0) y = 0; else if (y >= H) y = H - 1;
    return pl[y * W + x];
}

// One separable 3x1 + 1x3 box-blur pass over byte plane a, using b as scratch;
// the result ends back in a. Integer sums, borders clamped.
static void ae_box3(unsigned char *a, unsigned char *b, int W, int H) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
        b[y * W + x] = (unsigned char)((ae_at(a, W, H, x - 1, y) + a[y * W + x] +
                                        ae_at(a, W, H, x + 1, y)) / 3);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
        a[y * W + x] = (unsigned char)((ae_at(b, W, H, x, y - 1) + b[y * W + x] +
                                        ae_at(b, W, H, x, y + 1)) / 3);
}

static int op_accented_edges(const int *p) {
    int width  = fx_clamp(p[0], 1, 14);    // stroke dilation, not blur radius
    int bright = fx_clamp(p[1], 0, 50);    // 0 = black ink, 25 = identity, 50 = 2x chalk
    int smooth = fx_clamp(p[2], 1, 15);    // simplifies edges BEFORE detection
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    unsigned char *pa = (unsigned char *)malloc((size_t)W * H);
    unsigned char *pb = (unsigned char *)malloc((size_t)W * H);
    if (!pa || !pb) { free(pa); free(pb); free(src); return -1; }
    uint32_t *px = draw_px();

    // Pass 1: smoothed 8-bit luma plane; higher Smoothness suppresses fine edges.
    for (int i = 0; i < W * H; i++) pa[i] = (unsigned char)fx_lum(src[i]);
    int npass = (smooth - 1) / 3;
    for (int n = 0; n < npass; n++) ae_box3(pa, pb, W, H);

    // Pass 2: Sobel magnitude on the smoothed luma, soft threshold, into pb.
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int tl = ae_at(pa, W, H, x - 1, y - 1), tt = ae_at(pa, W, H, x, y - 1);
        int tr = ae_at(pa, W, H, x + 1, y - 1);
        int ll = ae_at(pa, W, H, x - 1, y),     rr = ae_at(pa, W, H, x + 1, y);
        int bl = ae_at(pa, W, H, x - 1, y + 1), bb = ae_at(pa, W, H, x, y + 1);
        int br = ae_at(pa, W, H, x + 1, y + 1);
        int gx = (tr + 2 * rr + br) - (tl + 2 * ll + bl);
        int gy = (bl + 2 * bb + br) - (tl + 2 * tt + tr);
        if (gx < 0) gx = -gx;
        if (gy < 0) gy = -gy;
        int m = fx_clamp((gx + gy) >> 1, 0, 255);
        if (m < 24) m = 0;
        pb[y * W + x] = (unsigned char)m;
    }

    // Edge Width: (width-1) 3x3 max-dilate passes, ping-pong pb <-> pa.
    unsigned char *cur = pb, *oth = pa;
    for (int n = 0; n < width - 1; n++) {
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            int mx = 0;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int v = ae_at(cur, W, H, x + dx, y + dy);
                if (v > mx) mx = v;
            }
            oth[y * W + x] = (unsigned char)mx;
        }
        unsigned char *tswap = cur; cur = oth; oth = tswap;
    }

    // Pass 3: Q12 gain pivoting at Brightness 25 (0 -> black ink, 8192 -> 2x chalk).
    int gain = bright * 4096 / 25;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int m = cur[i]; if (!m) continue;      // non-edge pixels stay untouched
        uint32_t o = src[i];
        int r2 = fx_clamp(px_r(o) + (((gain - 4096) * px_r(o) * m) >> 20), 0, 255);
        int g2 = fx_clamp(px_g(o) + (((gain - 4096) * px_g(o) * m) >> 20), 0, 255);
        int b2 = fx_clamp(px_b(o) + (((gain - 4096) * px_b(o) * m) >> 20), 0, 255);
        fx_put(px, src, i, argb(px_a(o), r2, g2, b2), cov);
    }

    free(pa); free(pb); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Angled Strokes: light areas are stroked along one 45 degree diagonal, dark
// areas along the opposite one; Direction Balance shifts which luminance band
// gets which diagonal, Sharpness runs an integer unsharp pass over the strokes.
static int op_angled_strokes(const int *p) {
    int bal = fx_clamp(p[0], 0, 100);
    int L = p[1]; if (L < 1) L = 1;
    int sharp = fx_clamp(p[2], 0, 10);
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *stk = (uint32_t *)malloc((size_t)W * H * 4);
    if (!stk) { free(src); return -1; }
    uint32_t *px = draw_px();
    int t = (255 * (100 - bal)) / 100;   // luminance threshold from balance
    int half = L / 2;
    // Pass 1: two diagonal line averages, blended by luminance vs threshold.
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y);
        if (!cov) { stk[i] = src[i]; continue; }   // keep valid data for the box pass
        // per-pixel jitter along the stroke keeps it painterly, not motion-blur flat
        unsigned s = 0x5AB7C9E1u ^ (unsigned)i;
        int jit = (int)(fx_rnd(&s) % 3u) - 1;
        int ar = 0, ag = 0, ab = 0, br = 0, bg = 0, bb = 0;
        for (int k = 0; k < L; k++) {
            int o = k - half + jit;
            int xa = fx_clamp(x + o, 0, W - 1);            // dirA steps (+1,+1)
            int ya = fx_clamp(y + o, 0, H - 1);
            int yb = fx_clamp(y - o, 0, H - 1);            // dirB steps (+1,-1)
            uint32_t pa = src[ya * W + xa], pb = src[yb * W + xa];
            ar += px_r(pa); ag += px_g(pa); ab += px_b(pa);
            br += px_r(pb); bg += px_g(pb); bb += px_b(pb);
        }
        ar /= L; ag /= L; ab /= L; br /= L; bg /= L; bb /= L;
        // Q8 weight, 128-wide soft ramp centered on the balance threshold
        int w = fx_clamp(((fx_lum(src[i]) - t + 64) * 256) / 128, 0, 256);
        int r = (ar * w + br * (256 - w)) >> 8;
        int g = (ag * w + bg * (256 - w)) >> 8;
        int b = (ab * w + bb * (256 - w)) >> 8;
        stk[i] = argb(px_a(src[i]), r, g, b);
    }
    // Pass 2: 3x3 box over the stroked image + integer unsharp, commit via fx_put.
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int sr = 0, sg = 0, sb = 0;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int nx = fx_clamp(x + dx, 0, W - 1), ny = fx_clamp(y + dy, 0, H - 1);
            uint32_t q = stk[ny * W + nx];
            sr += px_r(q); sg += px_g(q); sb += px_b(q);
        }
        sr /= 9; sg /= 9; sb /= 9;
        uint32_t c = stk[i];
        int r = fx_clamp(px_r(c) + ((sharp * (px_r(c) - sr)) >> 3), 0, 255);
        int g = fx_clamp(px_g(c) + ((sharp * (px_g(c) - sg)) >> 3), 0, 255);
        int b = fx_clamp(px_b(c) + ((sharp * (px_b(c) - sb)) >> 3), 0, 255);
        fx_put(px, src, i, argb(px_a(src[i]), r, g, b), cov);
    }
    free(stk); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Brush Strokes / Crosshatch: Strength passes of diagonal stroke smears along
// both 45 degree diagonals, broken into discrete hand drawn segments by a
// stripe seeded phase jitter, then a final integer unsharp stage for edge
// crispness. Keeps the image's own colors (no FG/BG), preserves alpha.

// One diagonal smear: every covered pixel becomes the integer per channel
// average of L whole pixel steps along the diagonal (diag 0 = (1,1),
// diag 1 = (1,-1)); 45 degrees needs no fx_sample since steps are whole
// pixels. A per stroke phase offset in 0..L-1 comes from fx_rnd seeded by the
// quantized cross stripe index, so runs break into discrete strokes instead
// of a uniform motion blur. Pixels whose luminance band disagrees with this
// diagonal keep 3 parts of the pre smear value to 1 part smear, which
// alternates the dominant hatch direction between light and dark bands.
static void xhatch_smear(uint32_t *dst, const uint32_t *cur, int W, int H,
                         int L, int diag) {
    int dy = diag ? -1 : 1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x;
        if (!draw_cov(x, y)) { dst[i] = cur[i]; continue; }
        int stripe = diag ? (x + y) / L : (x - y + 4096) / L;
        unsigned s = 0xB5297A4Du ^ (unsigned)(stripe * 2654435761u)
                                ^ (unsigned)(diag * 0x68E31DA4u);
        int phase = (int)(fx_rnd(&s) % (unsigned)L);
        int sr = 0, sg = 0, sb = 0, sa = 0;
        for (int k = 0; k < L; k++) {
            int t = k - phase;
            int sx = fx_clamp(x + t, 0, W - 1);
            int sy = fx_clamp(y + t * dy, 0, H - 1);
            uint32_t q = cur[sy * W + sx];
            sr += px_r(q); sg += px_g(q); sb += px_b(q); sa += px_a(q);
        }
        int r = sr / L, g = sg / L, b = sb / L, a = sa / L;
        uint32_t pre = cur[i];
        if (((fx_lum(pre) >> 5) & 1) != diag) {
            r = (px_r(pre) * 3 + r + 2) / 4;
            g = (px_g(pre) * 3 + g + 2) / 4;
            b = (px_b(pre) * 3 + b + 2) / 4;
            a = (px_a(pre) * 3 + a + 2) / 4;
        }
        dst[i] = argb(a, r, g, b);
    }
}

static int op_crosshatch(const int *p) {
    int L = p[0]; if (L < 1) L = 1;               // clamp degenerate inputs
    int sharp = fx_clamp(p[1], 0, 20);
    int strength = fx_clamp(p[2], 1, 3);          // cost bounded at 3*2*50
    int W = draw_w(), H = draw_h();
    uint32_t *orig = fx_dup(W, H); if (!orig) return -1;
    uint32_t *bufa = (uint32_t *)malloc((size_t)W * H * 4);
    uint32_t *bufb = (uint32_t *)malloc((size_t)W * H * 4);
    if (!bufa || !bufb) { free(bufa); free(bufb); free(orig); return -1; }
    memcpy(bufa, orig, (size_t)W * H * 4);
    uint32_t *cur = bufa, *nxt = bufb;
    for (int pass = 0; pass < strength; pass++) {  // Strength = literal re-application
        for (int diag = 0; diag < 2; diag++) {     // (1,1) then (1,-1)
            xhatch_smear(nxt, cur, W, H, L, diag);
            uint32_t *t = cur; cur = nxt; nxt = t; // ping pong keeps sampling pure
        }
    }
    // Sharpen edges: 3x3 integer box average b from the smeared scratch, then
    // res_c = fx_clamp(c + Sharpness*(c-b)/8); alpha taken from the ORIGINAL
    // snapshot for color only alpha fidelity. The drawable is untouched until
    // here, so every failure path above changed nothing.
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int br = 0, bg = 0, bb = 0;
        for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++) {
            uint32_t q = cur[fx_clamp(y + oy, 0, H - 1) * W + fx_clamp(x + ox, 0, W - 1)];
            br += px_r(q); bg += px_g(q); bb += px_b(q);
        }
        br /= 9; bg /= 9; bb /= 9;
        uint32_t c0 = cur[i];
        int r = fx_clamp(px_r(c0) + sharp * (px_r(c0) - br) / 8, 0, 255);
        int g = fx_clamp(px_g(c0) + sharp * (px_g(c0) - bg) / 8, 0, 255);
        int b = fx_clamp(px_b(c0) + sharp * (px_b(c0) - bb) / 8, 0, 255);
        fx_put(px, orig, i, argb(px_a(orig[i]), r, g, b), cov);
    }
    free(bufa); free(bufb); free(orig);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Dark Strokes (Brush Strokes): shadows get crushed black strokes along the
// down-right diagonal, highlights get white strokes along the opposing
// down-left diagonal; Balance shifts which regions get which treatment.
static int op_dark_strokes(const int *p) {
    int bal = fx_clamp(p[0], 0, 10);
    int bi  = fx_clamp(p[1], 0, 10);              // Black Intensity
    int wi  = fx_clamp(p[2], 0, 10);              // White Intensity
    int T = fx_clamp(26 * bal, 8, 248);           // split threshold; clamp kills /0 both sides
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int bmul = 192 + bi * 6;                      // shadow crush factor, always < 256
    int bstr = bi * 32; if (bstr > 255) bstr = 255;
    int wstr = wi * 32; if (wstr > 255) wstr = 255;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t o = src[i];
        int L = fx_lum(o);
        int r = px_r(o), g = px_g(o), b = px_b(o);
        if (L < T) {                              // dark side: down-right black stroke
            int K = bi + 1;                       // j=0 is the pixel itself
            int mr = r, mg = g, mb = b;
            for (int j = 1; j < K; j++) {
                int sx = fx_clamp(x + j, 0, W - 1), sy = fx_clamp(y + j, 0, H - 1);
                uint32_t s = src[sy * W + sx];
                if (px_r(s) < mr) mr = px_r(s);
                if (px_g(s) < mg) mg = px_g(s);
                if (px_b(s) < mb) mb = px_b(s);
            }
            int t = (T - L) * 256 / T; if (t > 255) t = 255;
            int wD = (t * bstr) >> 8;             // Q8 weight toward the diagonal minimum
            r += ((mr - r) * wD) >> 8;
            g += ((mg - g) * wD) >> 8;
            b += ((mb - b) * wD) >> 8;
            // extra darkening pass, blended by wD so Intensity 0 stays a noop
            // (spec fidelity note: black strokes dominate at defaults, but
            // Black Intensity 0 must be a near-noop on the dark side)
            r += ((((r * bmul) >> 8) - r) * wD) >> 8;
            g += ((((g * bmul) >> 8) - g) * wD) >> 8;
            b += ((((b * bmul) >> 8) - b) * wD) >> 8;
        } else {                                  // light side: down-left white stroke
            int K = wi + 1;
            int Mr = r, Mg = g, Mb = b;
            for (int j = 1; j < K; j++) {
                int sx = fx_clamp(x - j, 0, W - 1), sy = fx_clamp(y + j, 0, H - 1);
                uint32_t s = src[sy * W + sx];
                if (px_r(s) > Mr) Mr = px_r(s);
                if (px_g(s) > Mg) Mg = px_g(s);
                if (px_b(s) > Mb) Mb = px_b(s);
            }
            int t = (L - T) * 256 / (256 - T); if (t > 255) t = 255;
            int wL = (t * wstr) >> 8;             // Q8 weight toward the diagonal maximum
            r += ((Mr - r) * wL) >> 8;
            g += ((Mg - g) * wL) >> 8;
            b += ((Mb - b) * wL) >> 8;
            // push toward white, scaled by wL (mild at the PS default wi=2)
            r += ((((255 - r) * wi * 10) >> 8) * wL) >> 8;
            g += ((((255 - g) * wi * 10) >> 8) * wL) >> 8;
            b += ((((255 - b) * wi * 10) >> 8) * wL) >> 8;
        }
        r = fx_clamp(r, 0, 255); g = fx_clamp(g, 0, 255); b = fx_clamp(b, 0, 255);
        fx_put(px, src, i, argb(px_a(o), r, g, b), cov);   // preserve alpha
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Brush Strokes / Ink Outlines: Sobel ink lines smeared along the edge tangent
// into pen strokes, plus shadow darkening and highlight lift (PS Filter Gallery).
static inline int ink_lat(const unsigned char *L, int W, int H, int x, int y) {
    if (x < 0) x = 0; else if (x >= W) x = W - 1;
    if (y < 0) y = 0; else if (y >= H) y = H - 1;
    return L[y * W + x];
}

static int op_ink_outlines(const int *p) {
    int len = p[0]; if (len < 1) len = 1; if (len > 50) len = 50;
    int dark = fx_clamp(p[1], 0, 50), light = fx_clamp(p[2], 0, 50);
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    unsigned char *L  = (unsigned char *)malloc((size_t)W * H);
    unsigned char *E2 = (unsigned char *)malloc((size_t)W * H);
    if (!L || !E2) { free(L); free(E2); free(src); return -1; }
    for (int i = 0; i < W * H; i++) L[i] = (unsigned char)fx_lum(src[i]);
    memset(E2, 0, (size_t)W * H);
    // Pass 1+2: Sobel magnitude per pixel; strong edges scatter their magnitude
    // (max) along the edge tangent (perpendicular to the gradient) so outlines
    // elongate into strokes. len<=1 degenerates to plain crisp outlines.
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int gx = (ink_lat(L, W, H, x + 1, y - 1) + 2 * ink_lat(L, W, H, x + 1, y) + ink_lat(L, W, H, x + 1, y + 1))
               - (ink_lat(L, W, H, x - 1, y - 1) + 2 * ink_lat(L, W, H, x - 1, y) + ink_lat(L, W, H, x - 1, y + 1));
        int gy = (ink_lat(L, W, H, x - 1, y + 1) + 2 * ink_lat(L, W, H, x, y + 1) + ink_lat(L, W, H, x + 1, y + 1))
               - (ink_lat(L, W, H, x - 1, y - 1) + 2 * ink_lat(L, W, H, x, y - 1) + ink_lat(L, W, H, x + 1, y - 1));
        int ax = gx < 0 ? -gx : gx, ay = gy < 0 ? -gy : gy;
        int e = fx_clamp((ax + ay) / 2, 0, 255);
        int i = y * W + x;
        if (e > E2[i]) E2[i] = (unsigned char)e;
        if (e < 24 || len <= 1) continue;
        int m = (int)fx_isqrt((unsigned)(gx * gx + gy * gy)); if (!m) continue;
        int ux = -gy * 256 / m, uy = gx * 256 / m;        // .8 unit tangent step
        for (int t = -len; t <= len; t++) {
            int nx = ((x << 8) + t * ux + 128) >> 8;
            int ny = ((y << 8) + t * uy + 128) >> 8;
            if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
            int j = ny * W + nx;
            if (e > E2[j]) E2[j] = (unsigned char)e;
        }
    }
    // Pass 3 (color-only, preserves alpha): darken shadows, lift highlights,
    // then multiply in the ink lines. 12750 = 255 * 50 (slider full scale).
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t o = src[i];
        int lum = L[i], ink = 255 - E2[i];
        int r = px_r(o), g = px_g(o), b = px_b(o);
        r -= r * (255 - lum) * dark / 12750;
        g -= g * (255 - lum) * dark / 12750;
        b -= b * (255 - lum) * dark / 12750;
        r += (255 - r) * lum * light / 12750;
        g += (255 - g) * lum * light / 12750;
        b += (255 - b) * lum * light / 12750;
        r = fx_clamp(r * ink / 255, 0, 255);
        g = fx_clamp(g * ink / 255, 0, 255);
        b = fx_clamp(b * ink / 255, 0, 255);
        fx_put(px, src, i, argb(px_a(o), r, g, b), cov);
    }
    free(E2); free(L); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Brush Strokes / Spatter: geometric displacement through a coarse noise
// lattice. Smoothness sets the lattice cell size (the spatial frequency of
// the displacement noise, NOT a post-blur); Spray Radius sets the max offset.
// Node offsets are hashed from lattice coordinates, never rand(), so the
// live preview is deterministic across repeated apply() calls.

// Q8 displacement pair in [-R*256, +R*256] for lattice node (gx,gy).
static void spat_node(int gx, int gy, int R, int *odx, int *ody) {
    unsigned s = 0x53504154u ^ ((unsigned)gx * 0x9E3779B9u + (unsigned)gy * 0x85EBCA6Bu);
    if (!s) s = 0x53504154u;               // xorshift32 must never be seeded 0
    unsigned span = (unsigned)(2 * R * 256 + 1);
    *odx = (int)(fx_rnd(&s) % span) - R * 256;
    *ody = (int)(fx_rnd(&s) % span) - R * 256;
}

static int op_spatter(const int *p) {
    int R = p[0], S = p[1];                // Reset/preview can hand edge values
    if (R < 0) R = 0;
    if (R > 25) R = 25;
    if (S < 1) S = 1;
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    int gw = W / S + 2, gh = H / S + 2;    // node grid, one column/row of slack
    int *lat = (int *)malloc((size_t)gw * gh * 2 * sizeof(int));
    if (!lat) { free(src); return -1; }    // nothing changed yet
    for (int gy = 0; gy < gh; gy++)
        for (int gx = 0; gx < gw; gx++)
            spat_node(gx, gy, R, &lat[(gy * gw + gx) * 2], &lat[(gy * gw + gx) * 2 + 1]);
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) {
        int gy = y / S, ty = ((y % S) << 8) / S;
        for (int x = 0; x < W; x++) {
            int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
            int gx = x / S, tx = ((x % S) << 8) / S;
            int b0 = (gy * gw + gx) * 2, b1 = b0 + gw * 2;
            int ox = fx_bilin_ch(lat[b0], lat[b0 + 2], lat[b1], lat[b1 + 2], tx, ty);
            int oy = fx_bilin_ch(lat[b0 + 1], lat[b0 + 3], lat[b1 + 1], lat[b1 + 3], tx, ty);
            fx_put(px, src, i, fx_sample(src, W, H, (x << 8) + ox, (y << 8) + oy), cov);
        }
    }
    free(lat); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Brush Strokes / Sprayed Strokes: radial spatter jitter plus a directional
// max-luminance smear along the stroke vector (PS Filter Gallery parity:
// Stroke Length 0..20 def 12, Spray Radius 0..25 def 7, direction dropdown
// Right Diagonal|Horizontal|Left Diagonal|Vertical, Right Diagonal default).
static int op_sprayed_strokes(const int *p) {
    int L = p[0], R = p[1], dir = p[2];
    if (L < 0) L = 0;                        // clamp degenerates per contract B2
    if (R < 0) R = 0;                        // L==0 && R==0 degrades to copy-through
    if (dir < 0 || dir > 3) dir = 0;
    // Q12 unit stroke vectors in y-down screen space, so Right Diagonal runs
    // lower-left to upper-right exactly like the PS dropdown.
    static const int UXT[4] = {  2896, 4096, 2896,    0 };
    static const int UYT[4] = { -2896,    0, 2896, 4096 };
    int ux = UXT[dir], uy = UYT[dir];
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int L128 = L * 128;                      // stroke half-length in .8 fixed point
    int span = 2 * L128 + 1;
    int rspan = 2 * R + 1, rr2 = R * R;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        unsigned s = 0x5A7C39B1u ^ (unsigned)i;
        if (!s) s = 0x5A7C39B1u;             // xorshift32 must never seed with 0
        // Spray offset: uniform square draw rejected once against the disc,
        // halved if the retry still lands outside; deterministic per pixel so
        // repeated preview calls are byte-identical.
        int jx = (int)(fx_rnd(&s) % (unsigned)rspan) - R;
        int jy = (int)(fx_rnd(&s) % (unsigned)rspan) - R;
        if (jx * jx + jy * jy > rr2) {
            jx = (int)(fx_rnd(&s) % (unsigned)rspan) - R;
            jy = (int)(fx_rnd(&s) % (unsigned)rspan) - R;
            if (jx * jx + jy * jy > rr2) { jx /= 2; jy /= 2; }
        }
        // Three along-stroke candidates: both stroke tips plus one jittered
        // interior offset. Keeping the brightest smears bright ridges along u
        // into directional strokes; a single jittered sample would read as
        // Spatter, not Sprayed Strokes.
        int tc[3];
        tc[0] = -L128;
        tc[1] = (int)(fx_rnd(&s) % (unsigned)span) - L128;
        tc[2] = L128;
        int bx = (x << 8) + (jx << 8), by = (y << 8) + (jy << 8);
        uint32_t res = 0; int best = -1;
        for (int k = 0; k < 3; k++) {
            int t = tc[k];
            uint32_t c = fx_sample(src, W, H, bx + ((t * ux) >> 12), by + ((t * uy) >> 12));
            int l = fx_lum(c);
            if (l > best) { best = l; res = c; }
        }
        fx_put(px, src, i, res, cov);
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Brush Strokes / Sumi-e: darkest-sample smear along the fixed 45 degree
// diagonal (the wet-ink directional look), pressure-weighted blend toward
// the ink pixel, then a gentle contrast stage pivoted at mid-gray (128, not
// the image mean). Photoshop parity: Stroke Width 3..15 def 10, Stroke
// Pressure 0..15 def 2, Contrast 0..50 def 16; the stroke direction is a
// fixed diagonal in PS (no angle control), so no SP_ANGLE here by design.
// Pressure 0 still shows directional smearing from the min-pick alone.
static int op_sumi_e(const int *p) {
    int w = p[0]; if (w < 3) w = 3; if (w > 15) w = 15;   // clamp degenerates
    int q = fx_clamp(p[1], 0, 15) * 17;                   // pressure 0..15 -> 0..255
    int gain = 256 + fx_clamp(p[2], 0, 50) * 8;           // Q8 gain: 1.0 at 0, ~1.56 at 50
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int half = w / 2;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t orig = src[i];
        uint32_t ink = orig; int best = fx_lum(orig);
        for (int d = -half; d <= half; d++) {             // fixed 45 deg diagonal
            int sx = fx_clamp(x + d, 0, W - 1);
            int sy = fx_clamp(y + d, 0, H - 1);
            uint32_t s = src[sy * W + sx];
            int l = fx_lum(s);
            if (l < best) { best = l; ink = s; }          // lowest-luminance sample wins
        }
        int r = (px_r(orig) * (255 - q) + px_r(ink) * q + 127) / 255;
        int g = (px_g(orig) * (255 - q) + px_g(ink) * q + 127) / 255;
        int b = (px_b(orig) * (255 - q) + px_b(ink) * q + 127) / 255;
        r = fx_clamp(128 + (((r - 128) * gain) >> 8), 0, 255);
        g = fx_clamp(128 + (((g - 128) * gain) >> 8), 0, 255);
        b = fx_clamp(128 + (((b - 128) * gain) >> 8), 0, 255);
        fx_put(px, src, i, argb(px_a(orig), r, g, b), cov);
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---------------------------------------------------------------------------
static const studio_op_t OPS[] = {
    {"Brush Strokes","Accented Edges",3,{{SP_SLIDER,"Edge Width",1,14,2},{SP_SLIDER,"Edge Brightness",0,50,38},{SP_SLIDER,"Smoothness",1,15,5}},op_accented_edges},
    {"Brush Strokes","Angled Strokes",3,{{SP_SLIDER,"Direction Balance",0,100,50},{SP_SLIDER,"Stroke Length",3,50,15},{SP_SLIDER,"Sharpness",0,10,3}},op_angled_strokes},
    {"Brush Strokes","Crosshatch",3,{{SP_SLIDER,"Stroke Length",3,50,9},{SP_SLIDER,"Sharpness",1,20,6},{SP_INT,"Strength",1,3,1}},op_crosshatch},
    {"Brush Strokes","Dark Strokes",3,{{SP_SLIDER,"Balance",0,10,5},{SP_SLIDER,"Black Intensity",0,10,6},{SP_SLIDER,"White Intensity",0,10,2}},op_dark_strokes},
    {"Brush Strokes","Ink Outlines",3,{{SP_SLIDER,"Stroke Length",1,50,4},{SP_SLIDER,"Dark Intensity",0,50,20},{SP_SLIDER,"Light Intensity",0,50,10}},op_ink_outlines},
    {"Brush Strokes","Spatter",2,{{SP_SLIDER,"Spray Radius",0,25,10},{SP_SLIDER,"Smoothness",1,15,5}},op_spatter},
    {"Brush Strokes","Sprayed Strokes",3,{{SP_SLIDER,"Stroke Length",0,20,12},{SP_SLIDER,"Spray Radius",0,25,7},{SP_ENUM,"Stroke Direction",0,3,0,"Right Diagonal|Horizontal|Left Diagonal|Vertical"}},op_sprayed_strokes},
    {"Brush Strokes","Sumi-e",3,{{SP_SLIDER,"Stroke Width",3,15,10},{SP_SLIDER,"Stroke Pressure",0,15,2},{SP_SLIDER,"Contrast",0,50,16}},op_sumi_e},
};
void fx_brushstrokes_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
