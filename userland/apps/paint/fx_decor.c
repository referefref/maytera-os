// fx_decor.c - Maytera Studio "Decor" filter category (op registry).
#include "fx_common.inc"

static int op_border(const int *p) {
    int w = p[0], col = p[1];
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    int cr = (col >> 16) & 255, cg = (col >> 8) & 255, cb = col & 255;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        if (x >= w && y >= w && x < W - w && y < H - w) continue;
        int i = y * W + x, cov = draw_cov(x, y);
        fx_put(px, px, i, argb(255, cr, cg, cb), cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_bevel(const int *p) {
    int t = p[0]; if (t < 1) t = 1;
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int d = x; if (y < d) d = y; if (W - 1 - x < d) d = W - 1 - x; if (H - 1 - y < d) d = H - 1 - y;
        if (d >= t) continue;
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int lightside = (x <= y && x <= W - 1 - x) || (y <= x && y <= H - 1 - y); // top/left lit
        int shade = (t - d) * 90 / t; if (!lightside) shade = -shade;
        uint32_t o = px[i];
        fx_put(px, px, i, argb(px_a(o), fx_clamp(px_r(o) + shade, 0, 255),
               fx_clamp(px_g(o) + shade, 0, 255), fx_clamp(px_b(o) + shade, 0, 255)), cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_round(const int *p) {
    int r = p[0];
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int cxx = -1, cyy = -1;
        if (x < r && y < r) { cxx = r; cyy = r; }
        else if (x >= W - r && y < r) { cxx = W - 1 - r; cyy = r; }
        else if (x < r && y >= H - r) { cxx = r; cyy = H - 1 - r; }
        else if (x >= W - r && y >= H - r) { cxx = W - 1 - r; cyy = H - 1 - r; }
        if (cxx < 0) continue;
        int dx = x - cxx, dy = y - cyy; int d = (int)fx_isqrt(dx * dx + dy * dy);
        if (d > r) { int i = y * W + x, cov = draw_cov(x, y);
            fx_put(px, px, i, argb(0, 0, 0, 0), cov); }
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_fuzzy(const int *p) {
    int sz = p[0], col = p[1];
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    int cr = (col >> 16) & 255, cg = (col >> 8) & 255, cb = col & 255;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int d = x; if (y < d) d = y; if (W - 1 - x < d) d = W - 1 - x; if (H - 1 - y < d) d = H - 1 - y;
        if (d >= sz) continue;
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        unsigned s = 0x9E3779B9u ^ (unsigned)(i * 2654435761u);
        int a = (sz - d) * 255 / sz; a = a * (200 + (int)(fx_rnd(&s) % 56)) / 255;
        uint32_t o = px[i];
        fx_put(px, px, i, argb(px_a(o), px_r(o) + (cr - px_r(o)) * a / 255,
               px_g(o) + (cg - px_g(o)) * a / 255, px_b(o) + (cb - px_b(o)) * a / 255), cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_oldphoto(const int *p) {
    (void)p;
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    int cx = W / 2, cy = H / 2; int maxd = (int)fx_isqrt(cx * cx + cy * cy);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t o = px[i]; int l = fx_lum(o);
        int r = fx_clamp(l * 240 / 255 + 20, 0, 255);      // sepia
        int g = fx_clamp(l * 200 / 255 + 15, 0, 255);
        int b = fx_clamp(l * 150 / 255, 0, 255);
        int dx = x - cx, dy = y - cy; int d = (int)fx_isqrt(dx * dx + dy * dy);
        int vig = d > maxd / 2 ? (d - maxd / 2) * 160 / (maxd / 2) : 0;
        r = fx_clamp(r - vig, 0, 255); g = fx_clamp(g - vig, 0, 255); b = fx_clamp(b - vig, 0, 255);
        unsigned s = 0x27D4EB2Fu ^ (unsigned)(i * 2246822519u);
        int n = (int)(fx_rnd(&s) % 31) - 15;
        r = fx_clamp(r + n, 0, 255); g = fx_clamp(g + n, 0, 255); b = fx_clamp(b + n, 0, 255);
        fx_put(px, px, i, argb(px_a(o), r, g, b), cov);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_coffee(const int *p) {
    int count = p[0];
    int W = draw_w(), H = draw_h(); uint32_t *px = draw_px(); if (!px) return -1;
    unsigned s = 0x5DEECE66u;
    for (int k = 0; k < count; k++) {
        int cx = fx_rnd(&s) % W, cy = fx_rnd(&s) % H, rad = 15 + (int)(fx_rnd(&s) % 40);
        for (int y = cy - rad; y <= cy + rad; y++) for (int x = cx - rad; x <= cx + rad; x++) {
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            int dx = x - cx, dy = y - cy; int d = (int)fx_isqrt(dx * dx + dy * dy);
            if (d > rad) continue;
            int ring = (d > rad - 3) ? 120 : 50;           // darker ring edge
            int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
            uint32_t o = px[i];
            fx_put(px, px, i, argb(px_a(o), fx_clamp(px_r(o) - ring / 2, 0, 255),
                   fx_clamp(px_g(o) - ring * 3 / 4, 0, 255), fx_clamp(px_b(o) - ring, 0, 255)), cov * (rad - d) / rad);
        }
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static const studio_op_t OPS[] = {
    {"Decor","Add Border",2,{{SP_SLIDER,"Width",1,64,12},{SP_COLOR,"Color",0,0,0xFFFFFF}},op_border},
    {"Decor","Add Bevel",1,{{SP_SLIDER,"Thickness",1,40,8}},op_bevel},
    {"Decor","Round Corners",1,{{SP_SLIDER,"Radius",2,120,24}},op_round},
    {"Decor","Fuzzy Border",2,{{SP_SLIDER,"Size",2,64,16},{SP_COLOR,"Color",0,0,0xFFFFFF}},op_fuzzy},
    {"Decor","Old Photo",0,{{0,0,0,0,0}},op_oldphoto},
    {"Decor","Coffee Stain",1,{{SP_SLIDER,"Count",1,10,3}},op_coffee},
};
void fx_decor_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
