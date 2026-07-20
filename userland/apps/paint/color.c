// color.c - Maytera Studio "Colors" menu (full GIMP-style adjustments) via the
// op registry. Integer/fixed-point only; every op edits the active drawable,
// clipped/feathered by the selection, preserving alpha. See studio.h.
#include "studio.h"

// --------------------------------------------------------------------------
// Fixed-point + colour-space helpers (mirrored from filters.c conventions;
// this file is a separate TU so it keeps its own static copies).
// --------------------------------------------------------------------------
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
// 16.16 pow, base in (0,1] (1..65536), exp 16.16. Fractional bits via sqrt.
static uint32_t fxpow(uint32_t base, uint32_t exp) {
    if (base == 0) return 0;
    uint64_t result = 65536; uint32_t ip = exp >> 16;
    while (ip--) result = (result * base) >> 16;
    uint32_t cur = base;
    for (int bit = 15; bit >= 0; bit--) {
        cur = isqrt64((uint64_t)cur << 16);
        if (exp & (1u << bit)) result = (result * cur) >> 16;
    }
    if (result > 65536) result = 65536;
    return (uint32_t)result;
}
// build a 0..255 gamma LUT: out = 255 * (in/255)^(1/gamma), gamma = g100/100.
static void gamma_lut(int lut[256], int g100) {
    g100 = clampi(g100, 10, 300);
    uint32_t e = ((uint32_t)100 << 16) / (uint32_t)g100;   // 1/gamma in 16.16
    for (int v = 0; v < 256; v++) {
        uint32_t base = (uint32_t)v * 65536u / 255u;        // v/255 in 16.16
        lut[v] = clampi((int)((fxpow(base ? base : 1, e) * 255) >> 16), 0, 255);
    }
}

// H 0..1535 (6 sextants x 256), S/L 0..255.
static void rgb2hsl(int r, int g, int b, int *ph, int *ps, int *pl) {
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int l = (mx + mn) / 2, h = 0, s = 0;
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
static void hsl2rgb(int h, int s, int l, int *pr, int *pg, int *pb) {
    if (s <= 0) { *pr = *pg = *pb = l; return; }
    int q = (l < 128) ? l * (255 + s) / 255 : l + s - l * s / 255;
    int p = 2 * l - q;
    *pr = clampi(hue2rgb(p, q, h + 512), 0, 255);
    *pg = clampi(hue2rgb(p, q, h),       0, 255);
    *pb = clampi(hue2rgb(p, q, h - 512), 0, 255);
}
static inline int luma(int r, int g, int b) { return (r * 77 + g * 150 + b * 29) >> 8; }

// selection-feathered replace of one pixel's RGB (alpha preserved)
static inline uint32_t covmix(uint32_t o, int nr, int ng, int nb, int cov) {
    if (cov >= 255) return argb(px_a(o), clampi(nr,0,255), clampi(ng,0,255), clampi(nb,0,255));
    if (cov <= 0)   return o;
    int r = px_r(o) + (clampi(nr,0,255) - px_r(o)) * cov / 255;
    int g = px_g(o) + (clampi(ng,0,255) - px_g(o)) * cov / 255;
    int b = px_b(o) + (clampi(nb,0,255) - px_b(o)) * cov / 255;
    return argb(px_a(o), r, g, b);
}
static void done(void) { g_doc.comp_dirty = 1; g_doc.modified = 1; }

// per-channel LUT runner (3 luts) with selection
static int run_lut3(const int *lr, const int *lg, const int *lb) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int W = draw_w(), H = draw_h();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int cov = draw_cov(x, y); if (cov <= 0) continue;
            uint32_t o = px[y * W + x];
            px[y * W + x] = covmix(o, lr[px_r(o)], lg[px_g(o)], lb[px_b(o)], cov);
        }
    done(); return 0;
}
static int run_lut1(const int *l) { return run_lut3(l, l, l); }

// generic HSL per-pixel op
typedef void (*hslfn_t)(int *h, int *s, int *l, const int *p);
static int run_hsl(hslfn_t fn, const int *p) {
    uint32_t *px = draw_px(); if (!px) return -1;
    int W = draw_w(), H = draw_h();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int cov = draw_cov(x, y); if (cov <= 0) continue;
            uint32_t o = px[y * W + x];
            int h, s, l; rgb2hsl(px_r(o), px_g(o), px_b(o), &h, &s, &l);
            fn(&h, &s, &l, p);
            int r, g, b; hsl2rgb(((h % 1536) + 1536) % 1536, clampi(s,0,255), clampi(l,0,255), &r, &g, &b);
            px[y * W + x] = covmix(o, r, g, b, cov);
        }
    done(); return 0;
}

// --------------------------------------------------------------------------
// Ops
// --------------------------------------------------------------------------
static int op_brightcon(const int *p) {
    int b = clampi(p[0], -255, 255), c = clampi(p[1], -255, 255);
    // contrast factor in 8.8 fixed
    int f = (259 * (c + 255)) / (259 - c > 0 ? (259 - c) : 1); // ~ *256
    int lut[256];
    for (int v = 0; v < 256; v++) lut[v] = clampi(((f * (v - 128)) >> 8) + 128 + b, 0, 255);
    return run_lut1(lut);
}
static const sparam_t P_brightcon[] = {
    {SP_SLIDER,"Brightness",-127,127,0,0}, {SP_SLIDER,"Contrast",-127,127,0,0}
};

static int op_levels(const int *p) {
    int inb = clampi(p[0],0,254), inw = clampi(p[1],1,255);
    int g100 = clampi(p[2],10,300);
    int outb = clampi(p[3],0,255), outw = clampi(p[4],0,255);
    int chan = p[5]; // 0 RGB,1 R,2 G,3 B
    if (inw <= inb) inw = inb + 1;
    int gl[256]; gamma_lut(gl, g100);
    int lut[256];
    for (int v = 0; v < 256; v++) {
        int t = (v - inb) * 255 / (inw - inb); t = clampi(t, 0, 255);
        t = gl[t];
        lut[v] = clampi(outb + t * (outw - outb) / 255, 0, 255);
    }
    int id[256]; for (int v = 0; v < 256; v++) id[v] = v;
    if (chan == 1) return run_lut3(lut, id, id);
    if (chan == 2) return run_lut3(id, lut, id);
    if (chan == 3) return run_lut3(id, id, lut);
    return run_lut1(lut);
}
static const sparam_t P_levels[] = {
    {SP_SLIDER,"In Black",0,254,0,0}, {SP_SLIDER,"In White",1,255,255,0},
    {SP_SLIDER,"Gamma x100",10,300,100,0}, {SP_SLIDER,"Out Black",0,255,0,0},
    {SP_SLIDER,"Out White",0,255,255,0}, {SP_ENUM,"Channel",0,3,0,"RGB|Red|Green|Blue"}
};

// Curves: driven by the interactive SP_CURVE editor (ui.c owns the control-point
// state and the per-channel RGB/R/G/B tabs). The op just reads the composited
// per-channel LUTs the user drew and applies them. The single int param is the
// SP_CURVE placeholder; the real data lives in the widget.
static int op_curves(const int *p) {
    (void)p;
    int lr[256], lg[256], lb[256];
    curve_get_lut3(lr, lg, lb);
    return run_lut3(lr, lg, lb);
}
static const sparam_t P_curves[] = {
    {SP_CURVE,"Curve",0,0,0,0}
};

static int op_exposure(const int *p) {
    int stops100 = clampi(p[0],-300,300);           // stops * 100
    // factor = 2^(stops) in 8.8: use fxpow with base 2? simpler: gain = 256 * 2^stops
    // approximate: gain = 256 + stops*256/100 for small, but do real 2^ via shifts+frac
    // 2^stops = exp2. Build via: whole shifts + fxpow of 2 fractional part.
    int whole = stops100 / 100, frac = stops100 - whole * 100; // frac in -99..99
    // fxpow expects base in (0,1]; use 2^frac = 1/(2^-frac)... simpler linear frac blend
    long gain = 256L << (whole >= 0 ? whole : 0);
    if (whole < 0) gain = 256L >> (-whole);
    gain = gain + gain * frac / 100;                // linear within the octave (approx)
    if (gain < 1) gain = 1;
    int lut[256];
    for (int v = 0; v < 256; v++) lut[v] = clampi((int)((long)v * gain >> 8), 0, 255);
    return run_lut1(lut);
}
static const sparam_t P_exposure[] = { {SP_SLIDER,"Stops x100",-300,300,0,0} };

static int op_shadhigh(const int *p) {
    int sh = clampi(p[0],-100,100), hi = clampi(p[1],-100,100);
    int lut[256];
    for (int v = 0; v < 256; v++) {
        int ws = (255 - v) * (255 - v) / 255, wh = v * v / 255;
        lut[v] = clampi(v + (sh * ws - hi * wh) / 100 * 2, 0, 255);
    }
    return run_lut1(lut);
}
static const sparam_t P_shadhigh[] = {
    {SP_SLIDER,"Shadows",-100,100,0,0}, {SP_SLIDER,"Highlights",-100,100,0,0}
};

static void f_huesat(int *h, int *s, int *l, const int *p) {
    *h += p[0] * 1536 / 360;                          // hue -180..180 deg
    *s = clampi(*s + *s * p[1] / 100, 0, 255);
    *l = clampi(*l + *l * p[2] / 100, 0, 255);
}
static int op_huesat(const int *p) { return run_hsl(f_huesat, p); }
static const sparam_t P_huesat[] = {
    {SP_SLIDER,"Hue",-180,180,0,0}, {SP_SLIDER,"Saturation",-100,100,0,0},
    {SP_SLIDER,"Lightness",-100,100,0,0}
};

static void f_saturation(int *h, int *s, int *l, const int *p) { (void)h; (void)l;
    *s = clampi(*s + *s * p[0] / 100, 0, 255);
}
static int op_saturation(const int *p) { return run_hsl(f_saturation, p); }
static const sparam_t P_sat1[] = { {SP_SLIDER,"Saturation",-100,100,0,0} };

static void f_vibrance(int *h, int *s, int *l, const int *p) { (void)h; (void)l;
    int amt = p[0] * (255 - *s) / 255;               // affects low-sat more
    *s = clampi(*s + *s * amt / 100, 0, 255);
}
static int op_vibrance(const int *p) { return run_hsl(f_vibrance, p); }
static const sparam_t P_vib[] = { {SP_SLIDER,"Vibrance",-100,100,0,0} };

static int op_temperature(const int *p) {
    int t = clampi(p[0],-100,100);                   // + warm, - cool
    int lr[256], lg[256], lb[256];
    for (int v = 0; v < 256; v++) {
        lr[v] = clampi(v + t * 60 / 100, 0, 255);
        lg[v] = clampi(v + t * 10 / 100, 0, 255);
        lb[v] = clampi(v - t * 60 / 100, 0, 255);
    }
    return run_lut3(lr, lg, lb);
}
static const sparam_t P_temp[] = { {SP_SLIDER,"Temperature",-100,100,0,0} };

static int op_colorbalance(const int *p) {
    int cr = clampi(p[0],-100,100), mg = clampi(p[1],-100,100), yb = clampi(p[2],-100,100);
    int tone = p[3]; // 0 shadows,1 mids,2 highlights
    uint32_t *px = draw_px(); if (!px) return -1;
    int W = draw_w(), H = draw_h();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int cov = draw_cov(x, y); if (cov <= 0) continue;
            uint32_t o = px[y * W + x]; int r = px_r(o), g = px_g(o), b = px_b(o);
            int lm = luma(r, g, b), w;
            if (tone == 0) w = (255 - lm);
            else if (tone == 2) w = lm;
            else { int dm = (lm < 128) ? (128 - lm) : (lm - 128); w = 255 - dm * 2; }
            if (w < 0) w = 0;
            r += cr * w / 255; g += mg * w / 255; b += yb * w / 255;
            px[y * W + x] = covmix(o, r, g, b, cov);
        }
    done(); return 0;
}
static const sparam_t P_cbal[] = {
    {SP_SLIDER,"Cyan-Red",-100,100,0,0}, {SP_SLIDER,"Magenta-Green",-100,100,0,0},
    {SP_SLIDER,"Yellow-Blue",-100,100,0,0}, {SP_ENUM,"Range",0,2,1,"Shadows|Midtones|Highlights"}
};

static int op_threshold(const int *p) {
    int t = clampi(p[0],0,255), lut[256];
    for (int v = 0; v < 256; v++) lut[v] = v >= t ? 255 : 0;
    // threshold on luma -> gray
    uint32_t *px = draw_px(); if (!px) return -1;
    int W = draw_w(), H = draw_h();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int cov = draw_cov(x, y); if (cov <= 0) continue;
            uint32_t o = px[y * W + x];
            int v = lut[luma(px_r(o), px_g(o), px_b(o))];
            px[y * W + x] = covmix(o, v, v, v, cov);
        }
    done(); return 0;
}
static const sparam_t P_thr[] = { {SP_SLIDER,"Level",0,255,128,0} };

static int op_posterize(const int *p) {
    int n = clampi(p[0],2,16), lut[256];
    for (int v = 0; v < 256; v++) { int q = v * n / 256; lut[v] = q * 255 / (n - 1); }
    return run_lut1(lut);
}
static const sparam_t P_post[] = { {SP_SLIDER,"Levels",2,16,4,0} };

static int op_desaturate(const int *p) {
    int mode = p[0]; uint32_t *px = draw_px(); if (!px) return -1;
    int W = draw_w(), H = draw_h();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int cov = draw_cov(x, y); if (cov <= 0) continue;
            uint32_t o = px[y * W + x]; int r = px_r(o), g = px_g(o), b = px_b(o), v;
            int mx = r>g?(r>b?r:b):(g>b?g:b), mn = r<g?(r<b?r:b):(g<b?g:b);
            switch (mode) {
                case 0: v = luma(r, g, b); break;                 // Luminance (approx)
                case 1: v = (r*54 + g*182 + b*19) >> 8; break;    // Luma (Rec709-ish)
                case 2: v = (mx + mn) / 2; break;                 // Lightness
                case 3: v = (r + g + b) / 3; break;               // Average
                default: v = mx; break;                           // Value
            }
            px[y * W + x] = covmix(o, v, v, v, cov);
        }
    done(); return 0;
}
static const sparam_t P_desat[] = { {SP_ENUM,"Mode",0,4,0,"Luminance|Luma|Lightness|Average|Value"} };

static int op_monomixer(const int *p) {
    int wr = p[0], wg = p[1], wb = p[2], sum = wr + wg + wb; if (sum == 0) sum = 1;
    uint32_t *px = draw_px(); if (!px) return -1;
    int W = draw_w(), H = draw_h();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int cov = draw_cov(x, y); if (cov <= 0) continue;
            uint32_t o = px[y * W + x];
            int v = clampi((px_r(o)*wr + px_g(o)*wg + px_b(o)*wb) / sum, 0, 255);
            px[y * W + x] = covmix(o, v, v, v, cov);
        }
    done(); return 0;
}
static const sparam_t P_mono[] = {
    {SP_SLIDER,"Red",0,200,30,0}, {SP_SLIDER,"Green",0,200,59,0}, {SP_SLIDER,"Blue",0,200,11,0}
};

static int op_channelmixer(const int *p) {
    int out = p[0], wr = p[1], wg = p[2], wb = p[3];  // weights -200..200 (percent)
    uint32_t *px = draw_px(); if (!px) return -1;
    int W = draw_w(), H = draw_h();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int cov = draw_cov(x, y); if (cov <= 0) continue;
            uint32_t o = px[y * W + x]; int r = px_r(o), g = px_g(o), b = px_b(o);
            int nv = clampi((r*wr + g*wg + b*wb) / 100, 0, 255);
            int nr = r, ng = g, nb = b;
            if (out == 0) nr = nv; else if (out == 1) ng = nv; else nb = nv;
            px[y * W + x] = covmix(o, nr, ng, nb, cov);
        }
    done(); return 0;
}
static const sparam_t P_chmix[] = {
    {SP_ENUM,"Output",0,2,0,"Red|Green|Blue"},
    {SP_SLIDER,"from Red",-200,200,100,0}, {SP_SLIDER,"from Green",-200,200,0,0},
    {SP_SLIDER,"from Blue",-200,200,0,0}
};

static int op_invert(const int *p) { (void)p; int l[256]; for (int v=0;v<256;v++) l[v]=255-v; return run_lut1(l); }
static const sparam_t P_none[] = { {SP_CHECK,"",0,1,0,0} };
static int op_lininvert(const int *p) { (void)p;
    // invert in a rough linear-light space: undo gamma ~2.2, invert, redo
    int g1[256], g2[256]; gamma_lut(g1, 45); gamma_lut(g2, 220);  // ~1/2.2 and ~2.2
    int lut[256]; for (int v=0;v<256;v++) lut[v] = g1[255 - g2[v]];
    return run_lut1(lut);
}
static int op_valinvert(const int *p) { (void)p;
    // invert Value in HSV keeping hue/sat
    uint32_t *px = draw_px(); if (!px) return -1; int W=draw_w(),H=draw_h();
    for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
        int cov=draw_cov(x,y); if(cov<=0)continue; uint32_t o=px[y*W+x];
        int r=px_r(o),g=px_g(o),b=px_b(o); int mx=r>g?(r>b?r:b):(g>b?g:b);
        int nv=255-mx; int d = nv - mx;
        px[y*W+x]=covmix(o, r+d, g+d, b+d, cov);
    }
    done(); return 0;
}

static int op_colorize(const int *p) {
    int hue = clampi(p[0],0,360) * 1536 / 360, sat = clampi(p[1],0,100) * 255 / 100, lift = clampi(p[2],-100,100);
    uint32_t *px = draw_px(); if (!px) return -1; int W=draw_w(),H=draw_h();
    for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
        int cov=draw_cov(x,y); if(cov<=0)continue; uint32_t o=px[y*W+x];
        int l = clampi(luma(px_r(o),px_g(o),px_b(o)) + lift*255/100, 0, 255);
        int r,g,b; hsl2rgb(hue, sat, l, &r,&g,&b);
        px[y*W+x]=covmix(o, r,g,b, cov);
    }
    done(); return 0;
}
static const sparam_t P_colorize[] = {
    {SP_SLIDER,"Hue",0,360,180,0}, {SP_SLIDER,"Saturation",0,100,50,0}, {SP_SLIDER,"Lightness",-100,100,0,0}
};

static int op_color2alpha(const int *p) {
    int tr=(p[0]>>16)&255, tg=(p[0]>>8)&255, tb=p[0]&255;
    uint32_t *px = draw_px(); if (!px) return -1; int W=draw_w(),H=draw_h();
    for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
        int cov=draw_cov(x,y); if(cov<=0)continue; uint32_t o=px[y*W+x];
        int dr=px_r(o)-tr, dg=px_g(o)-tg, db=px_b(o)-tb;
        if (dr<0) dr=-dr;
        if (dg<0) dg=-dg;
        if (db<0) db=-db;
        int dist = dr>dg?(dr>db?dr:db):(dg>db?dg:db);      // chebyshev
        int na = px_a(o) * dist / 255;
        na = px_a(o) + (na - px_a(o)) * cov / 255;
        px[y*W+x]=argb(clampi(na,0,255), px_r(o),px_g(o),px_b(o));
    }
    done(); return 0;
}
static const sparam_t P_c2a[] = { {SP_COLOR,"Target",0,0xFFFFFF,0xFFFFFF,0} };

static int op_sepia(const int *p) { (void)p;
    uint32_t *px = draw_px(); if (!px) return -1; int W=draw_w(),H=draw_h();
    for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
        int cov=draw_cov(x,y); if(cov<=0)continue; uint32_t o=px[y*W+x];
        int r=px_r(o),g=px_g(o),b=px_b(o);
        int nr=(r*393+g*769+b*189)/1000, ng=(r*349+g*686+b*168)/1000, nb=(r*272+g*534+b*131)/1000;
        px[y*W+x]=covmix(o, nr,ng,nb, cov);
    }
    done(); return 0;
}

static int op_dither(const int *p) {
    int n = clampi(p[0],2,8);
    // ordered 4x4 Bayer
    static const int bay[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    uint32_t *px = draw_px(); if (!px) return -1; int W=draw_w(),H=draw_h();
    for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
        int cov=draw_cov(x,y); if(cov<=0)continue; uint32_t o=px[y*W+x];
        int thr = bay[(y&3)*4+(x&3)] * 255 / 16;
        int ch[3] = {px_r(o),px_g(o),px_b(o)}, nc[3];
        for (int c=0;c<3;c++){ int v=ch[c]+ (thr - 128) * 255 / (n*255/2 + 1);
            int q=clampi(v,0,255)*n/256; nc[c]=q*255/(n-1); }
        px[y*W+x]=covmix(o, nc[0],nc[1],nc[2], cov);
    }
    done(); return 0;
}
static const sparam_t P_dither[] = { {SP_SLIDER,"Levels",2,8,4,0} };

static int op_extract(const int *p) {
    int mode = p[0]; uint32_t *px = draw_px(); if (!px) return -1; int W=draw_w(),H=draw_h();
    for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
        int cov=draw_cov(x,y); if(cov<=0)continue; uint32_t o=px[y*W+x];
        int r=px_r(o),g=px_g(o),b=px_b(o), v;
        if (mode==0) v=r; else if (mode==1) v=g; else if (mode==2) v=b;
        else if (mode==3) v=luma(r,g,b);
        else { int h,s,l; rgb2hsl(r,g,b,&h,&s,&l); v = (mode==4)? h*255/1536 : (mode==5? s : l); }
        px[y*W+x]=covmix(o, v,v,v, cov);
    }
    done(); return 0;
}
static const sparam_t P_extract[] = { {SP_ENUM,"Component",0,6,3,"Red|Green|Blue|Luminance|Hue|Saturation|Value"} };

// --- Auto ops (histogram-driven) ------------------------------------------
static void hist_rgb(unsigned int hr[256], unsigned int hg[256], unsigned int hb[256]) {
    for (int i=0;i<256;i++){hr[i]=hg[i]=hb[i]=0;}
    uint32_t *px = draw_px(); int W=draw_w(),H=draw_h(); if(!px)return;
    for (int y=0;y<H;y++) for (int x=0;x<W;x++){ if(draw_cov(x,y)<=0)continue;
        uint32_t o=px[y*W+x]; hr[px_r(o)]++; hg[px_g(o)]++; hb[px_b(o)]++; }
}
static void stretch_lut(unsigned int h[256], int lut[256]) {
    int lo=0, hi=255; unsigned int total=0; for(int i=0;i<256;i++)total+=h[i];
    if (!total){ for(int i=0;i<256;i++)lut[i]=i; return; }
    unsigned int acc=0, cut=total/200;                 // 0.5% clip
    for (int i=0;i<256;i++){acc+=h[i]; if(acc>cut){lo=i;break;}}
    acc=0; for (int i=255;i>=0;i--){acc+=h[i]; if(acc>cut){hi=i;break;}}
    if (hi<=lo) hi=lo+1;
    for (int i=0;i<256;i++) lut[i]=clampi((i-lo)*255/(hi-lo),0,255);
}
static int op_stretch(const int *p) { (void)p;
    unsigned int hr[256],hg[256],hb[256]; hist_rgb(hr,hg,hb);
    int lr[256],lg[256],lb[256]; stretch_lut(hr,lr); stretch_lut(hg,lg); stretch_lut(hb,lb);
    return run_lut3(lr,lg,lb);
}
static int op_whitebal(const int *p) { return op_stretch(p); } // per-channel stretch == grey-world-ish
static int op_equalize(const int *p) { (void)p;
    unsigned int hr[256],hg[256],hb[256]; hist_rgb(hr,hg,hb);
    unsigned int hl[256]; for(int i=0;i<256;i++) hl[i]=hr[i]+hg[i]+hb[i];
    unsigned int total=0; for(int i=0;i<256;i++)total+=hl[i]; if(!total)return 0;
    int lut[256]; unsigned int acc=0;
    for (int i=0;i<256;i++){acc+=hl[i]; lut[i]=clampi((int)((uint64_t)acc*255/total),0,255);}
    return run_lut1(lut);
}
static int op_colorenh(const int *p) { (void)p;
    // boost saturation adaptively (like GIMP Color Enhance on HSV S)
    int amt[1] = {30}; return run_hsl(f_saturation, amt);
}

// --------------------------------------------------------------------------
// Registration
// --------------------------------------------------------------------------
// Ops are built into a static table at init (persistent for the registry).
static studio_op_t g_ops_col[40];
static int g_n;
static void add(const char *name, int (*ap)(const int*), const sparam_t *pr, int np) {
    if (g_n >= (int)(sizeof(g_ops_col)/sizeof(g_ops_col[0]))) return;
    studio_op_t *o = &g_ops_col[g_n++];
    o->menu = "Colors"; o->name = name; o->apply = ap; o->nparams = np;
    for (int i = 0; i < np && i < STUDIO_MAX_PARAMS; i++) o->params[i] = pr[i];
    studio_register(o);
}

void color_register_all(void) {
    g_n = 0;
    add("Brightness-Contrast", op_brightcon, P_brightcon, 2);
    add("Levels",              op_levels,    P_levels,    6);
    add("Curves",              op_curves,    P_curves,    1);
    add("Exposure",            op_exposure,  P_exposure,  1);
    add("Shadows-Highlights",  op_shadhigh,  P_shadhigh,  2);
    add("Hue-Saturation",      op_huesat,    P_huesat,    3);
    add("Saturation",          op_saturation,P_sat1,      1);
    add("Vibrance",            op_vibrance,  P_vib,       1);
    add("Color Temperature",   op_temperature,P_temp,     1);
    add("Color Balance",       op_colorbalance,P_cbal,    4);
    add("Threshold",           op_threshold, P_thr,       1);
    add("Posterize",           op_posterize, P_post,      1);
    add("Desaturate",          op_desaturate,P_desat,     1);
    add("Mono Mixer",          op_monomixer, P_mono,      3);
    add("Channel Mixer",       op_channelmixer,P_chmix,   4);
    add("Invert",              op_invert,    P_none,      0);
    add("Linear Invert",       op_lininvert, P_none,      0);
    add("Value Invert",        op_valinvert, P_none,      0);
    add("Colorize",            op_colorize,  P_colorize,  3);
    add("Color to Alpha",      op_color2alpha,P_c2a,      1);
    add("Sepia",               op_sepia,     P_none,      0);
    add("Dither",              op_dither,    P_dither,    1);
    add("Extract Component",   op_extract,   P_extract,   1);
    add("Auto White Balance",  op_whitebal,  P_none,      0);
    add("Auto Stretch Contrast",op_stretch,  P_none,      0);
    add("Auto Equalize",       op_equalize,  P_none,      0);
    add("Auto Color Enhance",  op_colorenh,  P_none,      0);
}
