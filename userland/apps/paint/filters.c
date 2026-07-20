// filters.c - Maytera Studio adjustments + convolution filters.
// Contract: studio.h. Operates on the ACTIVE layer only, clipped/feathered by
// the selection mask (sel_at). Caller does undo_push. Integer / fixed-point
// math only in per-pixel loops (no float, no libm). All temp buffers are
// malloc'd (no big static arrays, blame #444); freed on every path.
#include "studio.h"

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Lerp original -> filtered by mask weight m (0..255), per channel incl alpha.
static uint32_t mix_px(uint32_t o, uint32_t n, int m) {
    if (m >= 255) return n;
    if (m <= 0)   return o;
    int a = px_a(o) + (px_a(n) - px_a(o)) * m / 255;
    int r = px_r(o) + (px_r(n) - px_r(o)) * m / 255;
    int g = px_g(o) + (px_g(n) - px_g(o)) * m / 255;
    int b = px_b(o) + (px_b(n) - px_b(o)) * m / 255;
    return argb(a, r, g, b);
}

static int active_ok(void) {
    if (g_doc.w <= 0 || g_doc.h <= 0) return 0;
    if (g_doc.active < 0 || g_doc.active >= g_doc.nlayers) return 0;
    if (!g_doc.layer[g_doc.active].px) return 0;
    return 1;
}

static void mark_done(void) {
    g_doc.comp_dirty = 1;
    g_doc.modified   = 1;
}

// xorshift32; deterministic per pixel (fixed seed + index hash).
static inline uint32_t xs32(uint32_t x) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

// Integer sqrt of a 64-bit value (bit-by-bit method).
static uint32_t isqrt64(uint64_t v) {
    uint64_t r = 0, bit = 1ULL << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else              { r >>= 1; }
        bit >>= 2;
    }
    return (uint32_t)r;
}

// Fixed-point 16.16 pow for base in (0, 1] (i.e. 1..65536) and exponent in
// 16.16. Fractional exponent bits via repeated square roots: after the k-th
// sqrt, cur == base^(1/2^k), multiplied in when that fraction bit is set.
static uint32_t fxpow(uint32_t base, uint32_t exp) {
    if (base == 0) return 0;
    uint64_t result = 65536;
    uint32_t ip = exp >> 16;
    while (ip--) result = (result * base) >> 16;
    uint32_t cur = base;
    for (int bit = 15; bit >= 0; bit--) {
        // sqrt in 16.16: sqrt(x/65536)*65536 == isqrt(x<<16)
        cur = isqrt64((uint64_t)cur << 16);
        if (exp & (1u << bit)) result = (result * cur) >> 16;
    }
    if (result > 65536) result = 65536;
    return (uint32_t)result;
}

// ---------------------------------------------------------------------------
// Integer HSL <-> RGB. H in 0..1535 (6 sextants of 256), S/L in 0..255.
// ---------------------------------------------------------------------------
static void rgb_to_hsl(int r, int g, int b, int *ph, int *ps, int *pl) {
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int l = (mx + mn) / 2;
    int h = 0, s = 0;
    if (mx != mn) {
        int d = mx - mn;
        s = (l > 127) ? d * 255 / (510 - mx - mn) : d * 255 / (mx + mn);
        if (mx == r)      h = (g - b) * 256 / d;
        else if (mx == g) h = 512 + (b - r) * 256 / d;
        else              h = 1024 + (r - g) * 256 / d;
        while (h < 0) h += 1536;
        h %= 1536;
    }
    *ph = h; *ps = s; *pl = l;
}

static int hue2rgb(int p, int q, int t) {
    while (t < 0) t += 1536;
    t %= 1536;
    if (t < 256)  return p + (q - p) * t / 256;
    if (t < 768)  return q;
    if (t < 1024) return p + (q - p) * (1024 - t) / 256;
    return p;
}

static void hsl_to_rgb(int h, int s, int l, int *pr, int *pg, int *pb) {
    if (s <= 0) { *pr = *pg = *pb = l; return; }
    int q = (l < 128) ? l * (255 + s) / 255 : l + s - l * s / 255;
    int p = 2 * l - q;
    *pr = clampi(hue2rgb(p, q, h + 512), 0, 255);
    *pg = clampi(hue2rgb(p, q, h),       0, 255);
    *pb = clampi(hue2rgb(p, q, h - 512), 0, 255);
}

// ---------------------------------------------------------------------------
// Per-pixel LUT application (color-only filters; alpha preserved).
// ---------------------------------------------------------------------------
static void apply_lut(const int lut[256]) {
    uint32_t *px = g_doc.layer[g_doc.active].px;
    int w = g_doc.w, h = g_doc.h;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int m = sel_at(x, y);
            if (m == 0) continue;
            uint32_t o = px[y * w + x];
            uint32_t n = argb(px_a(o), lut[px_r(o)], lut[px_g(o)], lut[px_b(o)]);
            px[y * w + x] = mix_px(o, n, m);
        }
    }
}

// ---------------------------------------------------------------------------
// Separable box blur pass (RGB only). Sliding-window sums, replicated edges:
// window is sum over i in [x-r, x+r] of src[clamp(i)]; shifting the window by
// one removes clamp(x-r) and adds clamp(x+r+1), so the incremental update is
// exact. O(n) per pass independent of radius.
// ---------------------------------------------------------------------------
static void box_h(const uint32_t *src, uint32_t *dst, int w, int h, int r) {
    int div = 2 * r + 1;
    for (int y = 0; y < h; y++) {
        const uint32_t *row = src + (long)y * w;
        uint32_t *out = dst + (long)y * w;
        long sr = 0, sg = 0, sb = 0;
        for (int i = -r; i <= r; i++) {
            uint32_t p = row[clampi(i, 0, w - 1)];
            sr += px_r(p); sg += px_g(p); sb += px_b(p);
        }
        for (int x = 0; x < w; x++) {
            out[x] = argb(px_a(row[x]), (int)(sr / div), (int)(sg / div), (int)(sb / div));
            uint32_t add = row[clampi(x + r + 1, 0, w - 1)];
            uint32_t sub = row[clampi(x - r,     0, w - 1)];
            sr += px_r(add) - px_r(sub);
            sg += px_g(add) - px_g(sub);
            sb += px_b(add) - px_b(sub);
        }
    }
}

static void box_v(const uint32_t *src, uint32_t *dst, int w, int h, int r) {
    int div = 2 * r + 1;
    for (int x = 0; x < w; x++) {
        long sr = 0, sg = 0, sb = 0;
        for (int i = -r; i <= r; i++) {
            uint32_t p = src[(long)clampi(i, 0, h - 1) * w + x];
            sr += px_r(p); sg += px_g(p); sb += px_b(p);
        }
        for (int y = 0; y < h; y++) {
            uint32_t o = src[(long)y * w + x];
            dst[(long)y * w + x] = argb(px_a(o), (int)(sr / div), (int)(sg / div), (int)(sb / div));
            uint32_t add = src[(long)clampi(y + r + 1, 0, h - 1) * w + x];
            uint32_t sub = src[(long)clampi(y - r,     0, h - 1) * w + x];
            sr += px_r(add) - px_r(sub);
            sg += px_g(add) - px_g(sub);
            sb += px_b(add) - px_b(sub);
        }
    }
}

// One full box pass src -> src (uses tmp as scratch).
static void box_pass(uint32_t *buf, uint32_t *tmp, int w, int h, int r) {
    box_h(buf, tmp, w, h, r);
    box_v(tmp, buf, w, h, r);
}

// Write a fully-filtered buffer back to the active layer through the mask,
// taking alpha from the ORIGINAL layer pixel (contract: alpha = src alpha).
static void writeback(const uint32_t *out) {
    uint32_t *px = g_doc.layer[g_doc.active].px;
    int w = g_doc.w, h = g_doc.h;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int m = sel_at(x, y);
            if (m == 0) continue;
            uint32_t o = px[y * (long)w + x];
            uint32_t n = out[y * (long)w + x];
            n = argb(px_a(o), px_r(n), px_g(n), px_b(n));
            px[y * (long)w + x] = mix_px(o, n, m);
        }
    }
}

// ---------------------------------------------------------------------------
// Convolution filters that need a source copy
// ---------------------------------------------------------------------------
static int do_blur(int radius) {
    int w = g_doc.w, h = g_doc.h;
    long n = (long)w * h;
    uint32_t *buf = (uint32_t *)malloc(n * 4);
    uint32_t *tmp = (uint32_t *)malloc(n * 4);
    if (!buf || !tmp) { free(buf); free(tmp); return -1; }
    memcpy(buf, g_doc.layer[g_doc.active].px, n * 4);
    // 3 box passes approximate a gaussian of sigma ~= radius.
    box_pass(buf, tmp, w, h, radius);
    box_pass(buf, tmp, w, h, radius);
    box_pass(buf, tmp, w, h, radius);
    writeback(buf);
    free(buf); free(tmp);
    return 0;
}

static int do_sharpen(int amount) {
    int w = g_doc.w, h = g_doc.h;
    long n = (long)w * h;
    uint32_t *blur = (uint32_t *)malloc(n * 4);
    uint32_t *tmp  = (uint32_t *)malloc(n * 4);
    uint32_t *out  = (uint32_t *)malloc(n * 4);
    if (!blur || !tmp || !out) { free(blur); free(tmp); free(out); return -1; }
    const uint32_t *src = g_doc.layer[g_doc.active].px;
    memcpy(blur, src, n * 4);
    box_pass(blur, tmp, w, h, 2);
    for (long i = 0; i < n; i++) {
        uint32_t o = src[i], b = blur[i];
        int r = px_r(o) + (px_r(o) - px_r(b)) * amount / 128;
        int g = px_g(o) + (px_g(o) - px_g(b)) * amount / 128;
        int bl = px_b(o) + (px_b(o) - px_b(b)) * amount / 128;
        out[i] = argb(px_a(o), clampi(r, 0, 255), clampi(g, 0, 255), clampi(bl, 0, 255));
    }
    writeback(out);
    free(blur); free(tmp); free(out);
    return 0;
}

static inline int gray_of(uint32_t p) {
    return (77 * px_r(p) + 150 * px_g(p) + 29 * px_b(p)) >> 8;
}

static int do_edge(void) {
    int w = g_doc.w, h = g_doc.h;
    long n = (long)w * h;
    uint32_t *out = (uint32_t *)malloc(n * 4);
    if (!out) return -1;
    const uint32_t *src = g_doc.layer[g_doc.active].px;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int g00 = gray_of(src[(long)clampi(y-1,0,h-1)*w + clampi(x-1,0,w-1)]);
            int g01 = gray_of(src[(long)clampi(y-1,0,h-1)*w + x]);
            int g02 = gray_of(src[(long)clampi(y-1,0,h-1)*w + clampi(x+1,0,w-1)]);
            int g10 = gray_of(src[(long)y*w + clampi(x-1,0,w-1)]);
            int g12 = gray_of(src[(long)y*w + clampi(x+1,0,w-1)]);
            int g20 = gray_of(src[(long)clampi(y+1,0,h-1)*w + clampi(x-1,0,w-1)]);
            int g21 = gray_of(src[(long)clampi(y+1,0,h-1)*w + x]);
            int g22 = gray_of(src[(long)clampi(y+1,0,h-1)*w + clampi(x+1,0,w-1)]);
            int gx = (g02 + 2 * g12 + g22) - (g00 + 2 * g10 + g20);
            int gy = (g20 + 2 * g21 + g22) - (g00 + 2 * g01 + g02);
            if (gx < 0) gx = -gx;
            if (gy < 0) gy = -gy;
            int mag = clampi(gx + gy, 0, 255);   // |gx|+|gy| approximates magnitude
            out[(long)y*w + x] = argb(px_a(src[(long)y*w + x]), mag, mag, mag);
        }
    }
    writeback(out);
    free(out);
    return 0;
}

static int do_emboss(void) {
    int w = g_doc.w, h = g_doc.h;
    long n = (long)w * h;
    uint32_t *out = (uint32_t *)malloc(n * 4);
    if (!out) return -1;
    const uint32_t *src = g_doc.layer[g_doc.active].px;
    // Zero-sum diagonal relief kernel + 128 bias, per channel:
    //   -1 -1  0
    //   -1  0  1
    //    0  1  1
    static const int k[3][3] = { { -1, -1, 0 }, { -1, 0, 1 }, { 0, 1, 1 } };
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sr = 128, sg = 128, sb = 128;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int kv = k[dy + 1][dx + 1];
                    if (!kv) continue;
                    uint32_t p = src[(long)clampi(y+dy,0,h-1)*w + clampi(x+dx,0,w-1)];
                    sr += kv * px_r(p); sg += kv * px_g(p); sb += kv * px_b(p);
                }
            }
            out[(long)y*w + x] = argb(px_a(src[(long)y*w + x]),
                                      clampi(sr, 0, 255), clampi(sg, 0, 255), clampi(sb, 0, 255));
        }
    }
    writeback(out);
    free(out);
    return 0;
}

// ---------------------------------------------------------------------------
// In-place per-pixel filters that are not plain single-channel LUTs
// ---------------------------------------------------------------------------
static void do_huesat(int hue_deg, int sat, int light) {
    uint32_t *px = g_doc.layer[g_doc.active].px;
    int w = g_doc.w, hgt = g_doc.h;
    int hshift = hue_deg * 1536 / 360;   // degrees -> 0..1535 wheel units
    for (int y = 0; y < hgt; y++) {
        for (int x = 0; x < w; x++) {
            int m = sel_at(x, y);
            if (m == 0) continue;
            uint32_t o = px[(long)y * w + x];
            int hh, ss, ll;
            rgb_to_hsl(px_r(o), px_g(o), px_b(o), &hh, &ss, &ll);
            hh += hshift;
            while (hh < 0) hh += 1536;
            hh %= 1536;
            ss = clampi(ss + sat,   0, 255);
            ll = clampi(ll + light, 0, 255);
            int r, g, b;
            hsl_to_rgb(hh, ss, ll, &r, &g, &b);
            px[(long)y * w + x] = mix_px(o, argb(px_a(o), r, g, b), m);
        }
    }
}

static void do_sepia(void) {
    uint32_t *px = g_doc.layer[g_doc.active].px;
    int w = g_doc.w, h = g_doc.h;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int m = sel_at(x, y);
            if (m == 0) continue;
            uint32_t o = px[(long)y * w + x];
            int r = px_r(o), g = px_g(o), b = px_b(o);
            // Classic sepia matrix in 8.8 fixed point.
            int tr = (101 * r + 197 * g + 48 * b) >> 8;
            int tg = ( 89 * r + 175 * g + 43 * b) >> 8;
            int tb = ( 69 * r + 137 * g + 33 * b) >> 8;
            uint32_t n = argb(px_a(o), clampi(tr, 0, 255), clampi(tg, 0, 255), clampi(tb, 0, 255));
            px[(long)y * w + x] = mix_px(o, n, m);
        }
    }
}

static void do_gray_or_threshold(int threshold, int level) {
    uint32_t *px = g_doc.layer[g_doc.active].px;
    int w = g_doc.w, h = g_doc.h;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int m = sel_at(x, y);
            if (m == 0) continue;
            uint32_t o = px[(long)y * w + x];
            int v = gray_of(o);
            if (threshold) v = (v >= level) ? 255 : 0;
            px[(long)y * w + x] = mix_px(o, argb(px_a(o), v, v, v), m);
        }
    }
}

static void do_noise(int amount) {
    uint32_t *px = g_doc.layer[g_doc.active].px;
    int w = g_doc.w, h = g_doc.h;
    int span = 2 * amount + 1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int m = sel_at(x, y);
            if (m == 0) continue;
            long i = (long)y * w + x;
            uint32_t rnd = xs32(0x9E3779B9u ^ ((uint32_t)i * 2654435761u));
            rnd = xs32(rnd);
            uint32_t o = px[i];
            int dr = (int)((rnd >> 0)  & 255) % span - amount;
            int dg = (int)((rnd >> 8)  & 255) % span - amount;
            int db = (int)((rnd >> 16) & 255) % span - amount;
            uint32_t n = argb(px_a(o), clampi(px_r(o) + dr, 0, 255),
                              clampi(px_g(o) + dg, 0, 255), clampi(px_b(o) + db, 0, 255));
            px[i] = mix_px(o, n, m);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
const char *filter_name(filter_id_t f) {
    static const char *names[F_COUNT] = {
        "Brightness", "Contrast", "Hue/Saturation", "Levels", "Invert",
        "Grayscale", "Sepia", "Gaussian Blur", "Sharpen", "Edge Detect",
        "Emboss", "Threshold", "Posterize", "Noise"
    };
    if ((int)f < 0 || f >= F_COUNT) return "?";
    return names[f];
}

int filter_apply(filter_id_t f, int p1, int p2, int p3) {
    if ((int)f < 0 || f >= F_COUNT) return -1;
    if (!active_ok()) return -1;
    int lut[256];
    int rc = 0;

    switch (f) {
    case F_BRIGHTNESS: {
        int amt = clampi(p1, -255, 255);
        for (int v = 0; v < 256; v++) lut[v] = clampi(v + amt, 0, 255);
        apply_lut(lut);
        break;
    }
    case F_CONTRAST: {
        int amt = clampi(p1, -255, 255);
        // factor = 259*(amt+255) / (255*(259-amt)) in integer math
        long num = 259L * (amt + 255);
        long den = 255L * (259 - amt);
        for (int v = 0; v < 256; v++)
            lut[v] = clampi((int)(((v - 128) * num) / den + 128), 0, 255);
        apply_lut(lut);
        break;
    }
    case F_HUESAT:
        do_huesat(clampi(p1, -180, 180), clampi(p2, -255, 255), clampi(p3, -255, 255));
        break;
    case F_LEVELS: {
        int black = clampi(p1, 0, 254);
        int white = clampi(p2, black + 1, 255);
        int gamma = clampi(p3, 10, 300);           // gamma * 100
        uint32_t e = (uint32_t)(100 << 16) / (uint32_t)gamma;   // exponent 1/gamma, 16.16
        for (int v = 0; v < 256; v++) {
            int t = clampi((v - black) * 255 / (white - black), 0, 255);
            if (t == 0)        lut[v] = 0;
            else if (t == 255) lut[v] = 255;
            else {
                uint32_t base = ((uint32_t)t << 16) / 255;      // t/255 in 16.16
                lut[v] = clampi((int)((fxpow(base, e) * 255) >> 16), 0, 255);
            }
        }
        apply_lut(lut);
        break;
    }
    case F_INVERT:
        for (int v = 0; v < 256; v++) lut[v] = 255 - v;
        apply_lut(lut);
        break;
    case F_GRAYSCALE:
        do_gray_or_threshold(0, 0);
        break;
    case F_SEPIA:
        do_sepia();
        break;
    case F_BLUR:
        rc = do_blur(clampi(p1, 1, 16));
        break;
    case F_SHARPEN:
        rc = do_sharpen(clampi(p1, 0, 255));
        break;
    case F_EDGE:
        rc = do_edge();
        break;
    case F_EMBOSS:
        rc = do_emboss();
        break;
    case F_THRESHOLD:
        do_gray_or_threshold(1, clampi(p1, 0, 255));
        break;
    case F_POSTERIZE: {
        int levels = clampi(p1, 2, 16);
        for (int v = 0; v < 256; v++) {
            int q = (v * (levels - 1) + 127) / 255;
            lut[v] = clampi(q * 255 / (levels - 1), 0, 255);
        }
        apply_lut(lut);
        break;
    }
    case F_NOISE:
        do_noise(clampi(p1, 0, 255));
        break;
    default:
        return -1;
    }

    if (rc == 0) mark_done();
    return rc;
}
