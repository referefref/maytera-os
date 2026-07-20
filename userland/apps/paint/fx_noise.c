// fx_noise.c - Maytera Studio "Noise" filter category (op registry).
#include "fx_common.inc"

// local integer HSV / HSL helpers (color.c's are file-static and cannot be
// included, so we keep small self-contained copies here). H in [0,360),
// S/V/L in [0,255]. Integer/fixed-point only.
static void fx_rgb2hsv(int r, int g, int b, int *ph, int *ps, int *pv) {
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int d = mx - mn, h;
    *pv = mx;
    *ps = mx ? d * 255 / mx : 0;
    if (d == 0) h = 0;
    else if (mx == r) h = 60 * (g - b) / d;
    else if (mx == g) h = 120 + 60 * (b - r) / d;
    else h = 240 + 60 * (r - g) / d;
    if (h < 0) h += 360;
    *ph = h;
}
static void fx_hsv2rgb(int h, int s, int v, int *pr, int *pg, int *pb) {
    if (s <= 0) { *pr = *pg = *pb = v; return; }
    h %= 360; if (h < 0) h += 360;
    int region = h / 60;             // 0..5
    int f = (h % 60) * 255 / 60;     // 0..255 within the segment
    int p = v * (255 - s) / 255;
    int q = v * (255 - s * f / 255) / 255;
    int t = v * (255 - s * (255 - f) / 255) / 255;
    switch (region) {
        case 0:  *pr = v; *pg = t; *pb = p; break;
        case 1:  *pr = q; *pg = v; *pb = p; break;
        case 2:  *pr = p; *pg = v; *pb = t; break;
        case 3:  *pr = p; *pg = q; *pb = v; break;
        case 4:  *pr = t; *pg = p; *pb = v; break;
        default: *pr = v; *pg = p; *pb = q; break;
    }
}
static void fx_rgb2hsl(int r, int g, int b, int *ph, int *ps, int *pl) {
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int d = mx - mn, l = (mx + mn) / 2, h, den;
    *pl = l;
    if (d == 0) { *ph = 0; *ps = 0; return; }
    den = 255 - (2 * l - 255 < 0 ? -(2 * l - 255) : (2 * l - 255));  // 255 - |2L-255|
    *ps = den ? d * 255 / den : 0;
    if (mx == r) h = 60 * (g - b) / d;
    else if (mx == g) h = 120 + 60 * (b - r) / d;
    else h = 240 + 60 * (r - g) / d;
    if (h < 0) h += 360;
    *ph = h;
}
static void fx_hsl2rgb(int h, int s, int l, int *pr, int *pg, int *pb) {
    if (s <= 0) { *pr = *pg = *pb = l; return; }
    h %= 360; if (h < 0) h += 360;
    int c = (255 - (2 * l - 255 < 0 ? -(2 * l - 255) : (2 * l - 255))) * s / 255; // chroma
    int region = h / 60;             // 0..5
    int f = (h % 60) * 255 / 60;     // 0..255 within the segment
    int xf = (region & 1) ? (255 - f) : f;
    int x = c * xf / 255;            // second largest component (pre-offset)
    int m = l - c / 2;
    int rr, gg, bb;
    switch (region) {
        case 0:  rr = c; gg = x; bb = 0; break;
        case 1:  rr = x; gg = c; bb = 0; break;
        case 2:  rr = 0; gg = c; bb = x; break;
        case 3:  rr = 0; gg = x; bb = c; break;
        case 4:  rr = x; gg = 0; bb = c; break;
        default: rr = c; gg = 0; bb = x; break;
    }
    *pr = fx_clamp(rr + m, 0, 255);
    *pg = fx_clamp(gg + m, 0, 255);
    *pb = fx_clamp(bb + m, 0, 255);
}

// One noise sample: Uniform draws one value in [-amt, amt]; Gaussian sums
// three uniforms and halves (Irwin-Hall approximation, PS Add Noise parity;
// range +/- 1.5 amt, standard deviation ~amt/2).
static int rn_sample(unsigned *s, int amt, int gauss) {
    int span = 2 * amt + 1;
    int v = (int)(fx_rnd(s) % (unsigned)span) - amt;
    if (gauss) {
        v += (int)(fx_rnd(s) % (unsigned)span) - amt;
        v += (int)(fx_rnd(s) % (unsigned)span) - amt;
        v /= 2;
    }
    return v;
}

static int op_rgb_noise(const int *p) {
    int amt = p[0], gauss = p[1], mono = p[2];
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        unsigned s = 0x9E3779B9u ^ (unsigned)(i * 2654435761u);
        int nr = rn_sample(&s, amt, gauss);
        int ng = mono ? nr : rn_sample(&s, amt, gauss);
        int nb = mono ? nr : rn_sample(&s, amt, gauss);
        uint32_t o = px[i];
        uint32_t r = argb(px_a(o), fx_clamp(px_r(o) + nr, 0, 255),
                          fx_clamp(px_g(o) + ng, 0, 255), fx_clamp(px_b(o) + nb, 0, 255));
        fx_put(px, px, i, r, cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Real HSV jitter: convert each affected pixel to HSV, jitter hue by +-hn
// (wrapping around the colour wheel), saturation by +-sn, value by +-vn,
// then convert back. hn is in degrees; sn/vn are in 0..255 units.
static int op_hsv_noise(const int *p) {
    int hn = p[0], sn = p[1], vn = p[2];
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        unsigned s = 0x85EBCA6Bu ^ (unsigned)(i * 2246822519u);
        uint32_t o = px[i];
        int hh, ss, vv;
        fx_rgb2hsv(px_r(o), px_g(o), px_b(o), &hh, &ss, &vv);
        hh += (int)(fx_rnd(&s) % (2 * hn + 1)) - hn;   // jitter hue, wrapped
        hh %= 360; if (hh < 0) hh += 360;
        ss = fx_clamp(ss + ((int)(fx_rnd(&s) % (2 * sn + 1)) - sn), 0, 255);
        vv = fx_clamp(vv + ((int)(fx_rnd(&s) % (2 * vn + 1)) - vn), 0, 255);
        int nr, ng, nb;
        fx_hsv2rgb(hh, ss, vv, &nr, &ng, &nb);
        fx_put(px, px, i, argb(px_a(o), nr, ng, nb), cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// CIE-style Lightness Noise: jitter only the HSL lightness channel by +-amt,
// leaving hue and saturation untouched.
static int op_lightness_noise(const int *p) {
    int ln = p[0];
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        unsigned s = 0x1B873593u ^ (unsigned)(i * 2654435761u);
        uint32_t o = px[i];
        int hh, ss, ll;
        fx_rgb2hsl(px_r(o), px_g(o), px_b(o), &hh, &ss, &ll);
        ll = fx_clamp(ll + ((int)(fx_rnd(&s) % (2 * ln + 1)) - ln), 0, 255);
        int nr, ng, nb;
        fx_hsl2rgb(hh, ss, ll, &nr, &ng, &nb);
        fx_put(px, px, i, argb(px_a(o), nr, ng, nb), cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_hurl(const int *p) {
    int amt = p[0];
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        unsigned s = 0xC2B2AE35u ^ (unsigned)(i * 40503u + 12345u);
        if ((int)(fx_rnd(&s) % 256) < amt) {
            uint32_t r = argb(px_a(px[i]), fx_rnd(&s) & 255, fx_rnd(&s) & 255, fx_rnd(&s) & 255);
            fx_put(px, px, i, r, cov);
        }
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_pick(const int *p) {
    int amt = p[0];
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        unsigned s = 0x27D4EB2Fu ^ (unsigned)(i * 2654435761u);
        if ((int)(fx_rnd(&s) % 256) < amt) {
            int dx = (int)(fx_rnd(&s) % 3) - 1, dy = (int)(fx_rnd(&s) % 3) - 1;
            int nx = fx_clamp(x + dx, 0, W - 1), ny = fx_clamp(y + dy, 0, H - 1);
            fx_put(px, src, i, src[ny * W + nx], cov);
        }
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_slur(const int *p) {
    int amt = p[0];
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        unsigned s = 0x165667B1u ^ (unsigned)(i * 374761393u);
        if ((int)(fx_rnd(&s) % 256) < amt) {
            int sy = y - 1 - (int)(fx_rnd(&s) % 2); sy = fx_clamp(sy, 0, H - 1);
            int sx = x + (int)(fx_rnd(&s) % 3) - 1; sx = fx_clamp(sx, 0, W - 1);
            fx_put(px, src, i, src[sy * W + sx], cov);
        }
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_spread(const int *p) {
    int amt = p[0];
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        unsigned s = 0xD3A2646Cu ^ (unsigned)(i * 2246822519u);
        int dx = (int)(fx_rnd(&s) % (2 * amt + 1)) - amt;
        int dy = (int)(fx_rnd(&s) % (2 * amt + 1)) - amt;
        int nx = fx_clamp(x + dx, 0, W - 1), ny = fx_clamp(y + dy, 0, H - 1);
        fx_put(px, src, i, src[ny * W + nx], cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Noise / Despeckle (oneshot, Photoshop-style: blur everything except edges).
// For each pixel: 3x3 Gaussian blur (1 2 1 / 2 4 2 / 1 2 1, sum 16) sampled
// strictly from the fx_dup snapshot, and an edge magnitude gmax = max over the
// 8 neighbors of |fx_lum(neighbor) - fx_lum(center)|. gmax maps to a Q8 blend
// weight k with a smooth ramp between LO and HI: flat areas (gmax <= LO) take
// the full blur, real edges (gmax >= HI) pass through untouched, in between a
// graded mix avoids banding at the threshold. Color-only: alpha is preserved.
static int op_despeckle(const int *p) {
    (void)p;                                  // oneshot: no parameters
    enum { DSP_LO = 8, DSP_HI = 40 };         // luminance-step thresholds
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t o = src[i];
        int lc = fx_lum(o);
        int sr = 0, sg = 0, sb = 0, gmax = 0;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int nx = fx_clamp(x + dx, 0, W - 1);
            int ny = fx_clamp(y + dy, 0, H - 1);
            uint32_t q = src[ny * W + nx];
            int w = (dx ? 1 : 2) * (dy ? 1 : 2);   // 1 2 1 / 2 4 2 / 1 2 1
            sr += px_r(q) * w; sg += px_g(q) * w; sb += px_b(q) * w;
            if (dx || dy) {
                int d = fx_lum(q) - lc; if (d < 0) d = -d;
                if (d > gmax) gmax = d;
            }
        }
        int k;                                    // Q8 blur weight from edge magnitude
        if (gmax <= DSP_LO) k = 256;
        else if (gmax >= DSP_HI) k = 0;
        else k = ((DSP_HI - gmax) * 256) / (DSP_HI - DSP_LO);
        if (!k) continue;                         // genuine edge: leave untouched
        int r = (px_r(o) * (256 - k) + (sr >> 4) * k) >> 8;
        int g = (px_g(o) * (256 - k) + (sg >> 4) * k) >> 8;
        int b = (px_b(o) * (256 - k) + (sb >> 4) * k) >> 8;
        fx_put(px, src, i, argb(px_a(o), r, g, b), cov);
    }
    free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// Noise / Dust and Scratches: per-channel median with a protect threshold.
// Huang sliding-histogram median (three 256-bin uint16 histograms slid one
// column at a time along each row), so Radius 16 (33x33 window, 1089 taps)
// stays interactive; a naive per-pixel sort would not.
static int dns_median(const unsigned short *h, int n) {
    int k = n >> 1, cum = 0;
    for (int v = 0; v < 256; v++) {
        cum += h[v];
        if (cum > k) return v;
    }
    return 255;
}

// add (d = +1) or remove (d = -1) one source column of the window
static void dns_col(unsigned short *hr, unsigned short *hg, unsigned short *hb,
                    const uint32_t *src, int W, int cx, int y0, int y1, int d) {
    for (int yy = y0; yy <= y1; yy++) {
        uint32_t s = src[yy * W + cx];
        hr[px_r(s)] = (unsigned short)(hr[px_r(s)] + d);
        hg[px_g(s)] = (unsigned short)(hg[px_g(s)] + d);
        hb[px_b(s)] = (unsigned short)(hb[px_b(s)] + d);
    }
}

static int op_dust_and_scratches(const int *p) {
    int R = fx_clamp(p[0], 1, 16);          // Radius: window is (2R+1)x(2R+1)
    int T = fx_clamp(p[1], 0, 255);         // Threshold: 0 = pure median
    int W = draw_w(), H = draw_h();
    uint32_t *src = fx_dup(W, H); if (!src) return -1;
    unsigned short *hist = (unsigned short *)malloc(3 * 256 * sizeof(unsigned short));
    if (!hist) { free(src); return -1; }
    unsigned short *hr = hist, *hg = hist + 256, *hb = hist + 512;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) {
        int y0 = fx_clamp(y - R, 0, H - 1), y1 = fx_clamp(y + R, 0, H - 1);
        int rows = y1 - y0 + 1;
        memset(hist, 0, 3 * 256 * sizeof(unsigned short));
        int hi = fx_clamp(R, 0, W - 1), lo = 0;    // window columns for x == 0
        for (int cx = lo; cx <= hi; cx++) dns_col(hr, hg, hb, src, W, cx, y0, y1, 1);
        for (int x = 0; x < W; x++) {
            int i = y * W + x, cov = draw_cov(x, y);
            if (cov) {
                int n = rows * (hi - lo + 1);
                uint32_t o = src[i];
                int r = px_r(o), g = px_g(o), b = px_b(o);
                int mr = dns_median(hr, n), mg = dns_median(hg, n), mb = dns_median(hb, n);
                int dr = r - mr; if (dr < 0) dr = -dr;
                int dg = g - mg; if (dg < 0) dg = -dg;
                int db = b - mb; if (db < 0) db = -db;
                // PS semantics: replace a channel only when it differs from
                // the neighborhood median by at least the threshold
                if (dr >= T) r = mr;
                if (dg >= T) g = mg;
                if (db >= T) b = mb;
                fx_put(px, src, i, argb(px_a(o), r, g, b), cov);
            }
            // slide the window one column right for the next x (even when
            // cov was 0: the histograms are positional)
            if (x + 1 < W) {
                int nlo = fx_clamp(x + 1 - R, 0, W - 1);
                int nhi = fx_clamp(x + 1 + R, 0, W - 1);
                while (hi < nhi) { hi++; dns_col(hr, hg, hb, src, W, hi, y0, y1, 1); }
                while (lo < nlo) { dns_col(hr, hg, hb, src, W, lo, y0, y1, -1); lo++; }
            }
        }
    }
    free(hist); free(src);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

static const studio_op_t OPS[] = {
    {"Noise", "RGB Noise", 3, {{SP_SLIDER,"Amount",0,128,24}, {SP_ENUM,"Distribution",0,1,0,"Uniform|Gaussian"}, {SP_CHECK,"Monochrome",0,1,0}}, op_rgb_noise},
    {"Noise", "HSV Noise", 3, {{SP_SLIDER,"Hue",0,180,20}, {SP_SLIDER,"Saturation",0,255,30}, {SP_SLIDER,"Value",0,255,30}}, op_hsv_noise},
    {"Noise", "Lightness Noise", 1, {{SP_SLIDER,"Amount",0,128,24}}, op_lightness_noise},
    {"Noise", "Hurl", 1, {{SP_SLIDER,"Amount",0,255,40}}, op_hurl},
    {"Noise", "Pick", 1, {{SP_SLIDER,"Amount",0,255,60}}, op_pick},
    {"Noise", "Slur", 1, {{SP_SLIDER,"Amount",0,255,60}}, op_slur},
    {"Noise", "Spread", 1, {{SP_SLIDER,"Amount",1,32,6}}, op_spread},
    {"Noise", "Despeckle", 0, {{0,0,0,0,0}}, op_despeckle},
    {"Noise", "Dust and Scratches", 2, {{SP_SLIDER,"Radius",1,16,2}, {SP_SLIDER,"Threshold",0,255,0}}, op_dust_and_scratches},
};
void fx_noise_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
