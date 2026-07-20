// fx_combine.c - Maytera Studio "Combine" filter category (op registry).
// Ops rebuild the whole active drawable in place: they sample from a malloc'd
// copy of the layer (fx_dup) and lay the shrunk image onto a procedural
// backdrop (35mm film strip, or an NxN contact sheet). Selection aware via
// draw_cov(), integer / 16.16-style fixed point only, no big static arrays.
#include "fx_common.inc"

// Returns 1 when pixel (x,y) falls inside a sprocket hole in the top or bottom
// black margin band of a horizontal film strip. Holes are square, evenly
// pitched, centred vertically in the margin band.
static int fx_film_hole(int x, int y, int W, int H, int mh, int hs) {
    (void)W;
    int pitch = hs * 2; if (pitch < 1) pitch = 1;
    int cy;
    if (y < mh) cy = mh / 2;
    else if (y >= H - mh) cy = H - mh / 2;
    else return 0;
    int hhy = hs / 2; if (hhy < 1) hhy = 1;
    if (y < cy - hhy || y >= cy + hhy) return 0;
    int off = (pitch - hs) / 2; if (off < 0) off = 0;
    int phase = x % pitch;
    if (phase < off || phase >= off + hs) return 0;
    return 1;
}

// Filmstrip: shrink the image into N frames laid on a black 35mm strip with
// procedural sprocket holes along the top and bottom edges.
static int op_filmstrip(const int *p) {
    int frames = p[0]; if (frames < 1) frames = 1;
    int hs = p[1]; if (hs < 1) hs = 1;
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int mh = H / 6; if (mh < 1) mh = 1;           // top+bottom film margin height
    int ch2 = H - 2 * mh; if (ch2 < 1) ch2 = 1;   // vertical space for frame content
    int fw = W / frames; if (fw < 1) fw = 1;      // per-frame cell width
    int fb = fw / 24; if (fb < 1) fb = 1;         // black gutter around each frame
    int cw = fw - 2 * fb; if (cw < 1) cw = 1;     // frame content width
    uint32_t black = argb(255, 0, 0, 0);
    uint32_t hole = argb(255, 210, 210, 210);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t res = black;
        if (y >= mh && y < H - mh) {
            int ci = x / fw;
            if (ci < frames) {
                int lx = x - ci * fw;
                if (lx >= fb && lx < fw - fb) {
                    int sx = (int)((((long)(lx - fb) * W) << 8) / cw);
                    int sy = (int)((((long)(y - mh) * H) << 8) / ch2);
                    res = fx_sample(src, W, H, sx, sy);
                }
            }
        } else if (fx_film_hole(x, y, W, H, mh, hs)) {
            res = hole;
        }
        fx_put(px, src, i, res, cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

// Contact Sheet: tile the whole image as NxN thumbnails separated by thin
// borders. Each cell reproduces the entire source image.
static int op_contact(const int *p) {
    int n = p[0]; if (n < 1) n = 1;
    int W = draw_w(), H = draw_h(); uint32_t *src = fx_dup(W, H); if (!src) return -1;
    uint32_t *px = draw_px();
    int cwc = W / n; if (cwc < 1) cwc = 1;        // cell width
    int chc = H / n; if (chc < 1) chc = 1;        // cell height
    int bt = 1;                                   // thin border thickness
    int iw = cwc - 2 * bt; if (iw < 1) iw = 1;    // thumbnail content width
    int ih = chc - 2 * bt; if (ih < 1) ih = 1;    // thumbnail content height
    uint32_t border = argb(255, 40, 40, 40);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int i = y * W + x, cov = draw_cov(x, y); if (!cov) continue;
        uint32_t res = border;
        int cxi = x / cwc, cyi = y / chc;
        if (cxi < n && cyi < n) {
            int lx = x - cxi * cwc, ly = y - cyi * chc;
            if (lx >= bt && lx < cwc - bt && ly >= bt && ly < chc - bt) {
                int sx = (int)((((long)(lx - bt) * W) << 8) / iw);
                int sy = (int)((((long)(ly - bt) * H) << 8) / ih);
                res = fx_sample(src, W, H, sx, sy);
            }
        }
        fx_put(px, src, i, res, cov);
    }
    free(src); g_doc.comp_dirty = 1; g_doc.modified = 1; return 0;
}

static const studio_op_t OPS[] = {
    {"Combine","Filmstrip",2,{{SP_SLIDER,"Frames",2,6,3},{SP_SLIDER,"Hole Size",2,20,8}},op_filmstrip},
    {"Combine","Contact Sheet",1,{{SP_SLIDER,"Grid N",2,4,3}},op_contact},
};
void fx_combine_register_all(void) {
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) studio_register(&OPS[i]);
}
