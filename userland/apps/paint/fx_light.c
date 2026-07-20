// fx_light.c - Maytera Studio "Light and Shadow" filter category (op registry).
#include "fx_common.inc"

// one separable box-blur pass of an ARGB buffer in place (radius r), using a
// caller-supplied scratch buffer t of the same size to hold the horizontal pass.
static void fx_box1(uint32_t *b, uint32_t *t, int W, int H, int r) {
    for (int y = 0; y < H; y++) {                         // horizontal -> t
        for (int x = 0; x < W; x++) {
            long ar = 0, ag = 0, ab = 0, aa = 0; int n = 0;
            for (int k = -r; k <= r; k++) { int xx = fx_clamp(x + k, 0, W - 1);
                uint32_t p = b[y * W + xx]; ar += px_r(p); ag += px_g(p); ab += px_b(p); aa += px_a(p); n++; }
            t[y * W + x] = argb((int)(aa / n), (int)(ar / n), (int)(ag / n), (int)(ab / n));
        }
    }
    for (int x = 0; x < W; x++) {                         // vertical -> b
        for (int y = 0; y < H; y++) {
            long ar = 0, ag = 0, ab = 0, aa = 0; int n = 0;
            for (int k = -r; k <= r; k++) { int yy = fx_clamp(y + k, 0, H - 1);
                uint32_t p = t[yy * W + x]; ar += px_r(p); ag += px_g(p); ab += px_b(p); aa += px_a(p); n++; }
            b[y * W + x] = argb((int)(aa / n), (int)(ar / n), (int)(ag / n), (int)(ab / n));
        }
    }
}

// 3 successive box-blur passes of an ARGB buffer in place. Three boxes converge
// toward a gaussian (box -> triangle -> quadratic), turning the blocky single-box
// falloff into a smooth halo. Reuses a single malloc scratch buffer, freed here.
static void box3(uint32_t *buf, int w, int h, int radius) {
    if (radius < 1) return;
    uint32_t *t = (uint32_t *)malloc((size_t)w * h * 4); if (!t) return;
    fx_box1(buf, t, w, h, radius);
    fx_box1(buf, t, w, h, radius);
    fx_box1(buf, t, w, h, radius);
    free(t);
}

static int op_dropshadow(const int *p) {
    int ox = p[0], oy = p[1], blur = p[2], op = p[3], col = p[4];
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    uint32_t *sh = (uint32_t *)malloc((size_t)W * H * 4); if (!sh) return -1;
    int cr = (col >> 16) & 255, cg = (col >> 8) & 255, cb = col & 255;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int sx = x - ox, sy = y - oy; int a = 0;
        if (sx >= 0 && sy >= 0 && sx < W && sy < H) a = px_a(px[sy * W + sx]) * op / 255;
        sh[y * W + x] = argb(a, cr, cg, cb);
    }
    if (blur > 0) box3(sh, W, H, blur);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {          // shadow UNDER content
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t o = px[i]; int oa = px_a(o); int sa = px_a(sh[i]);
        int ea = oa + sa * (255 - oa) / 255;
        if (ea <= 0) continue;
        int er = (px_r(o) * oa + cr * sa * (255 - oa) / 255) / ea;
        int eg = (px_g(o) * oa + cg * sa * (255 - oa) / 255) / ea;
        int eb = (px_b(o) * oa + cb * sa * (255 - oa) / 255) / ea;
        fx_put(px, px, i, argb(ea, er, eg, eb), cov);
    }
    free(sh); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_longshadow(const int *p) {
    int ang = p[0], len = p[1], col = p[2];
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px(); int dx = fx_cos(ang) >> 8, dy = fx_sin(ang) >> 8;
    int cr = (col >> 16) & 255, cg = (col >> 8) & 255, cb = col & 255;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        if (px_a(src[i]) > 32) continue;                  // only fill transparent-ish
        int hit = 0;
        for (int s = 1; s <= len && !hit; s++) {
            int sx = x - dx * s / 64, sy = y - dy * s / 64;
            if (sx < 0 || sy < 0 || sx >= W || sy >= H) break;
            if (px_a(src[sy * W + sx]) > 128) hit = 1;
        }
        if (hit) fx_put(px, src, i, argb(180, cr, cg, cb), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_vignette(const int *p) {
    int radius = p[0], soft = p[1], col = p[2];
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    int cx = W / 2, cy = H / 2; int maxd = (int)fx_isqrt(cx * cx + cy * cy);
    int inner = maxd * radius / 100; int outer = inner + maxd * soft / 100 + 1;
    int cr = (col >> 16) & 255, cg = (col >> 8) & 255, cb = col & 255;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int dx = x - cx, dy = y - cy; int d = (int)fx_isqrt(dx * dx + dy * dy);
        int a = 0;
        if (d > inner) a = (d >= outer) ? 255 : (d - inner) * 255 / (outer - inner);
        uint32_t o = px[i];
        fx_put(px, px, i, argb(px_a(o), px_r(o) + (cr - px_r(o)) * a / 255,
               px_g(o) + (cg - px_g(o)) * a / 255, px_b(o) + (cb - px_b(o)) * a / 255), cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_bloom(const int *p) {
    int thr = p[0], radius = p[1], strength = p[2];
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    uint32_t *br = (uint32_t *)malloc((size_t)W * H * 4); if (!br) return -1;
    for (int i = 0; i < W * H; i++) {
        int l = fx_lum(px[i]);
        br[i] = (l > thr) ? px[i] : argb(255, 0, 0, 0);
    }
    box3(br, W, H, radius);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t o = px[i]; uint32_t g = br[i];
        int r = fx_clamp(px_r(o) + px_r(g) * strength / 255, 0, 255);
        int gg = fx_clamp(px_g(o) + px_g(g) * strength / 255, 0, 255);
        int b = fx_clamp(px_b(o) + px_b(g) * strength / 255, 0, 255);
        fx_put(px, px, i, argb(px_a(o), r, gg, b), cov);
    }
    free(br); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_softglow(const int *p) {
    int radius = p[0], bright = p[1];
    int W = draw_w(), H = draw_h(); uint32_t *bl = fx_dup(W, H); if (!bl) return -1;
    box3(bl, W, H, radius);
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {          // screen blend blurred
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t o = px[i], b = bl[i];
        int r = 255 - (255 - px_r(o)) * (255 - px_r(b) * bright / 255) / 255;
        int g = 255 - (255 - px_g(o)) * (255 - px_g(b) * bright / 255) / 255;
        int bb = 255 - (255 - px_b(o)) * (255 - px_b(b) * bright / 255) / 255;
        fx_put(px, px, i, argb(px_a(o), r, g, bb), cov);
    }
    free(bl); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_supernova(const int *p) {
    int radius = p[0], spokes = p[1], col = p[2];
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    int cx = W / 2, cy = H / 2; int cr = (col >> 16) & 255, cg = (col >> 8) & 255, cb = col & 255;
    if (spokes < 1) spokes = 1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int dx = x - cx, dy = y - cy; int d = (int)fx_isqrt(dx * dx + dy * dy);
        int core = d < radius ? (255 - d * 255 / radius) : 0;
        int ang = fx_atan2(dy, dx);
        int spoke = (fx_cos(ang * spokes) + 4096) * core / 8192;       // ray intensity
        int a = fx_clamp(core + spoke, 0, 255);
        uint32_t o = px[i];
        int r = fx_clamp(px_r(o) + cr * a / 255, 0, 255);
        int g = fx_clamp(px_g(o) + cg * a / 255, 0, 255);
        int b = fx_clamp(px_b(o) + cb * a / 255, 0, 255);
        fx_put(px, px, i, argb(px_a(o), r, g, b), cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_sparkle(const int *p) {
    int density = p[0], size = p[1];
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    unsigned s = 0x243F6A88u;
    int count = W * H * density / 100000; if (count < 1) count = 1;
    for (int k = 0; k < count; k++) {
        int cx = fx_rnd(&s) % W, cy = fx_rnd(&s) % H;
        for (int d = -size; d <= size; d++) {
            int a = 255 - (d < 0 ? -d : d) * 255 / (size + 1);
            int hx = fx_clamp(cx + d, 0, W - 1), vy = fx_clamp(cy + d, 0, H - 1);
            int ih = cy * W + hx, iv = vy * W + cx;
            if (draw_cov(hx, cy)) fx_put(px, px, ih, argb(px_a(px[ih]), 255, 255, 255), a);
            if (draw_cov(cx, vy)) fx_put(px, px, iv, argb(px_a(px[iv]), 255, 255, 255), a);
        }
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Lens Flare: center is a draggable on-canvas handle (X,Y normalized 0..1000).
// Lens Type varies the artifacts: main glow + halo ring + a chain of coloured
// ghost discs along the flare axis (center -> image center), plus an anamorphic
// horizontal streak for the movie-prime lens. Squared-distance gating keeps the
// per-pixel isqrt count low.
static int op_lensflare(const int *p) {
    int nx = fx_clamp(p[0], 0, 1000), ny = fx_clamp(p[1], 0, 1000);
    int bright = fx_clamp(p[2], 0, 255), lens = p[3] & 3;
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    int cx = nx * W / 1000, cy = ny * H / 1000;
    int icx = W / 2, icy = H / 2;
    int diag = (int)fx_isqrt((unsigned)(W * W + H * H)); if (diag < 1) diag = 1;
    int mainr, halor, nghost, warm;
    switch (lens) {
        case 0:  mainr = diag / 4; halor = diag / 3; nghost = 6; warm = 1; break; // 50-300mm Zoom
        case 1:  mainr = diag / 5; halor = diag / 4; nghost = 3; warm = 1; break; // 35mm Prime
        case 2:  mainr = diag / 6; halor = diag / 5; nghost = 4; warm = 0; break; // 105mm Prime
        default: mainr = diag / 4; halor = 0;        nghost = 8; warm = 0; break; // Movie Prime
    }
    if (mainr < 1) mainr = 1;
    int coreR = mainr / 4; if (coreR < 1) coreR = 1;
    int ringW = diag / 40; if (ringW < 1) ringW = 1;
    // precompute ghost centers along the axis
    int gcx[8], gcy[8], gr[8];
    for (int gi = 0; gi < nghost; gi++) {
        int t = gi + 1;
        gcx[gi] = cx + (icx - cx) * t * 2 / (nghost + 1);
        gcy[gi] = cy + (icy - cy) * t * 2 / (nghost + 1);
        gr[gi]  = diag / 24 + (gi & 1) * diag / 40; if (gr[gi] < 1) gr[gi] = 1;
    }
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int dx = x - cx, dy = y - cy; int d2 = dx * dx + dy * dy;
        int addr = 0, addg = 0, addb = 0;
        if (d2 < mainr * mainr) { int d = (int)fx_isqrt((unsigned)d2);
            int g = (mainr - d) * bright / mainr; addr += g; addg += g; addb += warm ? g * 3 / 4 : g;
            if (d < coreR) { int gc = (coreR - d) * bright / coreR; addr += gc; addg += gc; addb += gc; } }
        if (halor > 0) { int d = (int)fx_isqrt((unsigned)d2); int dr = d - halor; if (dr < 0) dr = -dr;
            if (dr < ringW) { int g = (ringW - dr) * bright / ringW / 2; addr += warm ? g : g / 2; addg += g / 2; addb += warm ? g / 3 : g; } }
        if (lens == 3) { int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
            if (ady < 3 && adx < mainr * 2) { int g = (mainr * 2 - adx) * bright / (mainr * 2) / 2; addb += g; addg += g / 2; } }
        for (int gi = 0; gi < nghost; gi++) {
            int gdx = x - gcx[gi], gdy = y - gcy[gi]; int gd2 = gdx * gdx + gdy * gdy;
            if (gd2 < gr[gi] * gr[gi]) { int gd = (int)fx_isqrt((unsigned)gd2);
                int g = (gr[gi] - gd) * bright / gr[gi] / 3;
                if (gi & 1) { addr += g; addb += warm ? 0 : g; } else { addg += g; addb += g; } }
        }
        uint32_t o = px[i];
        int r = fx_clamp(px_r(o) + addr, 0, 255), g2 = fx_clamp(px_g(o) + addg, 0, 255), b = fx_clamp(px_b(o) + addb, 0, 255);
        fx_put(px, px, i, argb(px_a(o), r, g2, b), cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Normalize a 3-vector to Q10 (length ~1024). Integer only.
static void norm3(long x, long y, long z, int *ox, int *oy, int *oz) {
    long len2 = x * x + y * y + z * z; if (len2 < 1) len2 = 1;
    unsigned len = fx_isqrt((unsigned)len2); if (len < 1) len = 1;
    *ox = (int)(x * 1024 / len); *oy = (int)(y * 1024 / len); *oz = (int)(z * 1024 / len);
}

// Flagship: Photoshop-style Lighting Effects. Treats image luminance as a height
// field, derives a per-pixel surface normal, and shades Lambertian diffuse +
// Blinn-Phong specular from a Spot / Point / Directional light, plus ambient.
// Light position and (for Spot) the cone target are on-canvas draggable handles.
// All fixed-point; squared-distance gating limits the isqrt count.
static int op_lighting(const int *p) {
    int ltype = ((p[0] % 3) + 3) % 3;
    int lnx = fx_clamp(p[1], 0, 1000), lny = fx_clamp(p[2], 0, 1000);
    int tnx = fx_clamp(p[3], 0, 1000), tny = fx_clamp(p[4], 0, 1000);
    int az = p[5];
    int inten = fx_clamp(p[6], 0, 300);
    int focus = fx_clamp(p[7], 0, 100);
    int gloss = fx_clamp(p[8], 0, 100);
    int material = p[9] & 1;
    int expo = fx_clamp(p[10], -100, 100);
    int amb = fx_clamp(p[11], 0, 100);
    int ambcol = p[12];
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int lpx = lnx * W / 1000, lpy = lny * H / 1000;
    int tpx = tnx * W / 1000, tpy = tny * H / 1000;
    int LZ = (W + H) / 6; if (LZ < 8) LZ = 8;
    int diag = (int)fx_isqrt((unsigned)(W * W + H * H)); if (diag < 1) diag = 1;
    int acr = (ambcol >> 16) & 255, acg = (ambcol >> 8) & 255, acb = ambcol & 255;
    int shin = 2 + gloss * gloss / 150; if (material) shin *= 2; if (shin > 80) shin = 80;
    int gain = 256 + expo * 256 / 100;              // Q8 exposure multiplier
    int ambbase = amb * 614 / 100;                  // Q10 ambient floor on the surface
    // Directional light (constant), elevation fixed at 45 degrees.
    int dLx = 0, dLy = 0, dLz = 1024;
    if (ltype == 2) { int he = fx_cos(45);
        long vx = ((long)fx_cos(az) * he) >> 12, vy = ((long)fx_sin(az) * he) >> 12, vz = fx_sin(45);
        norm3(vx, vy, vz, &dLx, &dLy, &dLz); }
    // Spot axis in the plane (light -> target), Q10.
    int aAx = 1024, aAy = 0;
    if (ltype == 0) { int oz; norm3(tpx - lpx, tpy - lpy, 0, &aAx, &aAy, &oz); (void)oz; }
    int cutoff = 307 + focus * 717 / 100;           // Q10 cone cos cutoff ~0.30..1.00
    int poolR = diag * (30 + focus) / 100; if (poolR < 1) poolR = 1;   // spot/point falloff radius (px)
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int xl = x > 0 ? x - 1 : x, xr = x < W - 1 ? x + 1 : x;
        int yt = y > 0 ? y - 1 : y, yb = y < H - 1 ? y + 1 : y;
        int gx = fx_lum(src[y * W + xr]) - fx_lum(src[y * W + xl]);
        int gy = fx_lum(src[yb * W + x]) - fx_lum(src[yt * W + x]);
        int Nx, Ny, Nz; norm3(-(long)gx * 2, -(long)gy * 2, 128, &Nx, &Ny, &Nz);
        int Lx, Ly, Lz;
        if (ltype == 2) { Lx = dLx; Ly = dLy; Lz = dLz; }
        else norm3(lpx - x, lpy - y, LZ, &Lx, &Ly, &Lz);
        int ndotl = (Nx * Lx + Ny * Ly + Nz * Lz) / 1024; if (ndotl < 0) ndotl = 0;
        int lightf = ndotl;                         // Q10
        if (ltype != 2) {
            int pdx = x - lpx, pdy = y - lpy; int pd = (int)fx_isqrt((unsigned)(pdx * pdx + pdy * pdy));
            int atten = 1024 - pd * 1024 / poolR; if (atten < 0) atten = 0;
            lightf = lightf * atten / 1024;
            if (ltype == 0) { int Px, Py, oz2; norm3(pdx, pdy, 0, &Px, &Py, &oz2); (void)oz2;
                int cang = (Px * aAx + Py * aAy) / 1024;
                int denom = 1024 - cutoff; if (denom < 1) denom = 1;
                int spotf = (cang - cutoff) * 1024 / denom; if (spotf < 0) spotf = 0; if (spotf > 1024) spotf = 1024;
                lightf = lightf * spotf / 1024; }
        }
        lightf = lightf * inten / 100;
        int sr = 0, sg = 0, sb = 0;
        if (lightf > 0) {
            int Hnx, Hny, Hnz; norm3(Lx, Ly, Lz + 1024, &Hnx, &Hny, &Hnz);
            int ndh = (Nx * Hnx + Ny * Hny + Nz * Hnz) / 1024; if (ndh < 0) ndh = 0;
            int spec = 1024; for (int e = 0; e < shin && spec > 0; e++) spec = spec * ndh / 1024;
            int specI = spec * inten / 100;
            if (material) { sr = px_r(src[i]) * specI / 1024; sg = px_g(src[i]) * specI / 1024; sb = px_b(src[i]) * specI / 1024; }
            else { sr = sg = sb = 255 * specI / 1024; }
        }
        uint32_t o = src[i];
        int mul = ambbase + lightf;                 // Q10 surface multiplier
        long rr = (long)px_r(o) * mul / 1024 + acr * amb / 100 + sr;
        long gg = (long)px_g(o) * mul / 1024 + acg * amb / 100 + sg;
        long bb = (long)px_b(o) * mul / 1024 + acb * amb / 100 + sb;
        rr = (rr * gain) >> 8; gg = (gg * gain) >> 8; bb = (bb * gain) >> 8;
        fx_put(px, src, i, argb(px_a(o), fx_clamp((int)rr, 0, 255), fx_clamp((int)gg, 0, 255), fx_clamp((int)bb, 0, 255)), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static const studio_op_t OPS[] = {
    {"Light and Shadow","Drop Shadow",5,{{SP_INT,"Offset X",-64,64,8},{SP_INT,"Offset Y",-64,64,8},{SP_SLIDER,"Blur",0,32,6},{SP_SLIDER,"Opacity",0,255,180},{SP_COLOR,"Color",0,0,0x000000}},op_dropshadow},
    {"Light and Shadow","Long Shadow",3,{{SP_ANGLE,"Angle",0,360,45},{SP_SLIDER,"Length",4,256,64},{SP_COLOR,"Color",0,0,0x202020}},op_longshadow},
    {"Light and Shadow","Vignette",3,{{SP_SLIDER,"Radius",0,100,60},{SP_SLIDER,"Softness",1,100,40},{SP_COLOR,"Color",0,0,0x000000}},op_vignette},
    {"Light and Shadow","Bloom",3,{{SP_SLIDER,"Threshold",0,255,180},{SP_SLIDER,"Radius",1,32,8},{SP_SLIDER,"Strength",0,255,120}},op_bloom},
    {"Light and Shadow","Softglow",2,{{SP_SLIDER,"Radius",1,32,10},{SP_SLIDER,"Brightness",0,255,140}},op_softglow},
    {"Light and Shadow","Supernova",3,{{SP_SLIDER,"Radius",8,300,80},{SP_SLIDER,"Spokes",1,20,8},{SP_COLOR,"Color",0,0,0xFFF0C0}},op_supernova},
    {"Light and Shadow","Sparkle",2,{{SP_SLIDER,"Density",1,100,20},{SP_SLIDER,"Size",1,16,5}},op_sparkle},
    {"Light and Shadow","Lens Flare",4,{{SP_INT,"Center",0,1000,300},{SP_INT,"Center Y",0,1000,300},{SP_SLIDER,"Brightness",0,255,170},{SP_ENUM,"Lens Type",0,3,0,"50-300mm Zoom|35mm Prime|105mm Prime|Movie Prime"}},op_lensflare,.nhandles=1,.handle_xp={0},.handle_yp={1}},
    // Flagship Photoshop-style Lighting Effects (Render category). Light position
    // and (Spot) cone target are on-canvas draggable handles.
    {"Render","Lighting Effects",13,{
        {SP_ENUM,"Light Type",0,2,0,"Spot|Point|Directional"},
        {SP_INT,"Light Pos",0,1000,350},{SP_INT,"Light Y",0,1000,320},
        {SP_INT,"Target Pos",0,1000,600},{SP_INT,"Target Y",0,1000,520},
        {SP_ANGLE,"Azimuth",0,360,135},
        {SP_SLIDER,"Intensity",0,300,150},
        {SP_SLIDER,"Cone/Focus",0,100,55},
        {SP_SLIDER,"Gloss",0,100,45},
        {SP_ENUM,"Material",0,1,0,"Plastic|Metallic"},
        {SP_SLIDER,"Exposure",-100,100,0},
        {SP_SLIDER,"Ambience",0,100,25},
        {SP_COLOR,"Ambient Color",0,0,0x404058}
    },op_lighting,.nhandles=2,.handle_xp={1,3},.handle_yp={2,4}},
};
void fx_light_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
