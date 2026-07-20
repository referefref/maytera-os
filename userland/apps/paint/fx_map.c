// fx_map.c - Maytera Studio "Map" filter category (op registry).
#include "fx_common.inc"

static int op_bump(const int *p) {
    int azimuth = p[0], elev = p[1], depth = p[2];
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int lx = fx_cos(azimuth) * fx_cos(elev) >> 12;         // light vector (Q12-ish)
    int ly = fx_sin(azimuth) * fx_cos(elev) >> 12;
    int lz = fx_sin(elev);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int xl = fx_clamp(x - 1, 0, W - 1), xr = fx_clamp(x + 1, 0, W - 1);
        int yu = fx_clamp(y - 1, 0, H - 1), yd = fx_clamp(y + 1, 0, H - 1);
        int gx = (fx_lum(src[y * W + xr]) - fx_lum(src[y * W + xl])) * depth / 10;
        int gy = (fx_lum(src[yd * W + x]) - fx_lum(src[yu * W + x])) * depth / 10;
        int nz = 4096;
        long dot = (long)(-gx) * lx + (long)(-gy) * ly + (long)nz * lz;
        int shade = (int)(dot >> 16); shade = fx_clamp(128 + shade, 0, 255);
        uint32_t o = src[i];
        fx_put(px, src, i, argb(px_a(o), px_r(o) * shade / 255, px_g(o) * shade / 255, px_b(o) * shade / 255), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_displace(const int *p) {
    int amt = p[0];
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int xl = fx_clamp(x - 1, 0, W - 1), xr = fx_clamp(x + 1, 0, W - 1);
        int yu = fx_clamp(y - 1, 0, H - 1), yd = fx_clamp(y + 1, 0, H - 1);
        int gx = (fx_lum(src[y * W + xr]) - fx_lum(src[y * W + xl])) * amt / 64;
        int gy = (fx_lum(src[yd * W + x]) - fx_lum(src[yu * W + x])) * amt / 64;
        int sx = fx_clamp(x + gx, 0, W - 1), sy = fx_clamp(y + gy, 0, H - 1);
        fx_put(px, src, i, src[sy * W + sx], cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_fractal_trace(const int *p) {
    // GIMP-style Fractal Trace: warp the image through Mandelbrot space. Each
    // destination pixel is mapped to a complex c in ~[-2,2], iterated a few
    // times with z = z^2 + c, and the escaped coordinates address the source.
    // All math is 16.16 fixed point (int64_t intermediates for the multiplies)
    // because the old (x-W/2)*4/W integer form collapses to 0 and no-ops.
    int depth = p[0]; if (depth < 1) depth = 1; if (depth > 16) depth = 16;   // cap ~16
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    if (W < 1) W = 1; if (H < 1) H = 1;
    const int64_t ESCAPE = (int64_t)4 << 16;                 // |z|^2 > 4.0 escapes
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        // Map pixel to complex plane, ~[-2,2] on both axes, in 16.16.
        int64_t cx = ((int64_t)(x - W / 2) << 16) * 4 / W;
        int64_t cy = ((int64_t)(y - H / 2) << 16) * 4 / H;
        int64_t zx = cx, zy = cy;
        for (int k = 0; k < depth; k++) {
            int64_t zx2 = (zx * zx) >> 16;                   // zx^2 (16.16)
            int64_t zy2 = (zy * zy) >> 16;                   // zy^2 (16.16)
            if (zx2 + zy2 > ESCAPE) break;                   // point escaped
            int64_t nzx = zx2 - zy2 + cx;                    // z = z^2 + c
            int64_t nzy = ((zx * zy) >> 15) + cy;            // 2*zx*zy (>>16 then *2)
            zx = nzx; zy = nzy;
        }
        // Map escaped coord back to a source pixel in .8 fixed point for
        // fx_sample: px = W/2 + (z/4)*W, so .8 coord = (W<<7) + z*W/1024.
        int sx8 = (W << 7) + (int)((zx * W) / 1024);
        int sy8 = (H << 7) + (int)((zy * H) / 1024);
        fx_put(px, src, i, fx_sample(src, W, H, sx8, sy8), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_illusion(const int *p) {
    int div = p[0]; if (div < 1) div = 1;
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px(); int cx = W / 2, cy = H / 2;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int dx = x - cx, dy = y - cy; int ang = fx_atan2(dy, dx) + 360 / div;
        int d = (int)fx_isqrt(dx * dx + dy * dy) * (div - 1) / div;
        int sx = fx_clamp(cx + (d * fx_cos(ang) >> 12), 0, W - 1);
        int sy = fx_clamp(cy + (d * fx_sin(ang) >> 12), 0, H - 1);
        uint32_t a = src[i], b = src[sy * W + sx];
        fx_put(px, src, i, argb((px_a(a) + px_a(b)) / 2, (px_r(a) + px_r(b)) / 2,
               (px_g(a) + px_g(b)) / 2, (px_b(a) + px_b(b)) / 2), cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_tile(const int *p) {
    int off = p[0];
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int sx = ((x + off) % W + W) % W, sy = ((y + off) % H + H) % H;
        fx_put(px, src, i, src[sy * W + sx], cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_little_planet(const int *p) {
    (void)p;
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px(); int cx = W / 2, cy = H / 2; int R = (cx < cy ? cx : cy);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int dx = x - cx, dy = y - cy; int d = (int)fx_isqrt(dx * dx + dy * dy);
        int ang = fx_atan2(dy, dx);
        int sx = fx_clamp(ang * W / 360, 0, W - 1);          // longitude
        int sy = fx_clamp(R ? (d * H / R) : 0, 0, H - 1);    // latitude from center out
        fx_put(px, src, i, src[sy * W + sx], cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static int op_paper_tile(const int *p) {
    int t = p[0], move = p[1]; if (t < 4) t = 4;
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        int tx = x / t, ty = y / t;
        unsigned s = 0x2545F491u ^ (unsigned)((ty * 733 + tx) * 2654435761u);
        int mx = (int)(fx_rnd(&s) % (2 * move + 1)) - move;
        int my = (int)(fx_rnd(&s) % (2 * move + 1)) - move;
        int sx = fx_clamp(x + mx, 0, W - 1), sy = fx_clamp(y + my, 0, H - 1);
        fx_put(px, src, i, src[sy * W + sx], cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static const studio_op_t OPS[] = {
    {"Map","Bump Map",3,{{SP_ANGLE,"Azimuth",0,360,135},{SP_SLIDER,"Elevation",0,90,45},{SP_SLIDER,"Depth",1,40,10}},op_bump},
    {"Map","Displace",1,{{SP_SLIDER,"Amount",1,64,16}},op_displace},
    {"Map","Fractal Trace",1,{{SP_SLIDER,"Depth",1,16,8}},op_fractal_trace},
    {"Map","Illusion",1,{{SP_SLIDER,"Divisions",1,12,2}},op_illusion},
    {"Map","Tile Shift",1,{{SP_SLIDER,"Offset",1,256,64}},op_tile},
    {"Map","Little Planet",0,{{0,0,0,0,0}},op_little_planet},
    {"Map","Paper Tile",2,{{SP_SLIDER,"Tile Size",4,64,16},{SP_SLIDER,"Move",1,16,4}},op_paper_tile},
};
void fx_map_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
