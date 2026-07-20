// tools.c - Maytera Studio paint tools. Owner: Agent 2. Contract: studio.h.
//
// Every pixel write lands on the ACTIVE layer, is scaled by the selection
// mask (sel_at()/255), sets g_doc.comp_dirty and g_doc.modified. tool_begin
// calls undo_push() for mutating tools only. Shape tools (line/rect/ellipse/
// gradient) and text commit on tool_end; ui.c draws the rubber-band preview.
#include "studio.h"
#include "brushes.h"

toolstate_t g_tool = {
    .id = TL_BRUSH,
    .fg = 0xFF000000u,            // black
    .bg = 0xFFFFFFFFu,            // white
    .size = 8,
    .opacity = 255,
    .hardness = 200,
    .wand_tolerance = 32,
    .grad_blend = BLEND_NORMAL,
    .text = "",
};

// ---------------------------------------------------------------------------
// Stroke state
// ---------------------------------------------------------------------------
static int st_active = 0;          // inside a begin..end stroke
static int st_bx, st_by;           // begin point
static int st_lx, st_ly;           // last point
// Clone tool: the FIRST click after selecting the tool sets the source
// anchor (no painting). The next stroke fixes offset = anchor - stroke start
// and paints pixels copied from (x + off). The anchor persists until the
// clone tool is re-selected from the toolstrip (ui.c resets via tool change:
// we detect a tool-id change here and drop the anchor).
static int clone_have_src = 0;
static int clone_ax, clone_ay;
static int clone_offx, clone_offy;
static int clone_painting = 0;
static tool_id_t last_tool_id = TL_BRUSH;
// Smudge: carries a malloc'd patch of the previous stamp footprint.
static uint32_t *smudge_patch = NULL;
static int smudge_patch_dim = 0;
// Measure tool: begin point kept in st_bx/st_by; result reported to the status
// bar on drag/end. Path tool: nodes accumulate via path_add_node on each click.

const char *tool_name(tool_id_t t) {
    static const char *names[TL_COUNT] = {
        "Brush", "Pencil", "Eraser", "Airbrush", "Clone", "Smudge",
        "Blur", "Fill", "Gradient", "Line", "Rect", "Ellipse",
        "Text", "Move", "Pick",
        "Select Rect", "Select Ellipse", "Lasso", "Magic Wand",
        "Heal", "Dodge", "Burn", "Sharpen", "Ink", "Select by Color",
        "Crop", "Measure", "Path",
    };
    if ((int)t < 0 || t >= TL_COUNT) return "?";
    return names[t];
}

// ---------------------------------------------------------------------------
// Low-level pixel ops on the active layer
// ---------------------------------------------------------------------------
static layer_t *al(void) {
    if (g_doc.nlayers <= 0) return NULL;
    if (g_doc.active < 0 || g_doc.active >= g_doc.nlayers) return NULL;
    return &g_doc.layer[g_doc.active];
}

static uint32_t isqrt32(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

// Integer atan2 in degrees (-180..180), no libm. Uses a rational octant
// approximation good to ~1 degree, which is plenty for a measure readout.
static int iatan2_deg(int y, int x) {
    if (x == 0 && y == 0) return 0;
    int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    int a;                                      // angle within [0,45] of the octant
    if (ax >= ay) {
        // atan(ay/ax) ~ 45*r*(1 + 0.28*(1-r)) with r = ay/ax, scaled integer.
        int r = ay * 1024 / ax;                 // Q10 ratio 0..1024
        a = (r * 45) >> 10;
        a += ((1024 - r) * r * 13) >> 20;       // small curvature correction
    } else {
        int r = ax * 1024 / ay;
        a = 90 - ((r * 45) >> 10);
        a -= ((1024 - r) * r * 13) >> 20;
    }
    if (x < 0) a = 180 - a;                      // quadrant fold
    if (y < 0) a = -a;
    return a;
}

// Straight-alpha src-over of (color RGB, alpha a 0..255) onto layer pixel.
// Honors the active layer's mask_active (paint grayscale into the mask instead
// of pixels) and lock_alpha (blend colour but preserve destination alpha).
static void put_px(uint32_t *buf, int x, int y, uint32_t color, int a) {
    if (x < 0 || y < 0 || x >= g_doc.w || y >= g_doc.h) return;
    a = a * sel_at(x, y) / 255;
    if (a <= 0) return;
    layer_t *L = al();
    if (L && L->mask_active && L->mask) {
        // Paint the colour's luminance into the mask with coverage a.
        int lum = (px_r(color) * 77 + px_g(color) * 150 + px_b(color) * 29) >> 8;
        uint8_t *m = &L->mask[(size_t)y * (size_t)g_doc.w + x];
        *m = (uint8_t)clampi(*m + (lum - *m) * a / 255, 0, 255);
        return;
    }
    uint32_t *p = &buf[(size_t)y * (size_t)g_doc.w + x];
    uint32_t d = *p;
    int da = px_a(d);
    int oa = a + da * (255 - a) / 255;
    if (oa <= 0) { *p = 0; return; }
    int r = (px_r(color) * a + px_r(d) * da * (255 - a) / 255) / oa;
    int g = (px_g(color) * a + px_g(d) * da * (255 - a) / 255) / oa;
    int b = (px_b(color) * a + px_b(d) * da * (255 - a) / 255) / oa;
    if (L && L->lock_alpha) {           // colour changes, alpha stays put
        if (da <= 0) return;
        oa = da;
    }
    *p = argb(oa, r, g, b);
}

// Reduce destination alpha by coverage (eraser on transparent-capable layers).
static void erase_px(uint32_t *buf, int x, int y, int cov) {
    if (x < 0 || y < 0 || x >= g_doc.w || y >= g_doc.h) return;
    cov = cov * sel_at(x, y) / 255;
    if (cov <= 0) return;
    uint32_t *p = &buf[(size_t)y * (size_t)g_doc.w + x];
    int da = px_a(*p);
    int na = da * (255 - cov) / 255;
    *p = argb(na, px_r(*p), px_g(*p), px_b(*p));
}

// Round stamp coverage at (dx,dy) from center for radius r and hardness.
// Returns 0..255. Hard core = r*hardness/255, linear falloff to the rim.
static int stamp_cov(int dx, int dy, int r, int hardness) {
    if (r <= 0) return (dx == 0 && dy == 0) ? 255 : 0;
    uint32_t d2 = (uint32_t)(dx * dx + dy * dy);
    uint32_t r2 = (uint32_t)r * (uint32_t)r;
    if (d2 > r2) return 0;
    int core = r * hardness / 255;
    int d = (int)isqrt32(d2);
    if (d <= core) return 255;
    int span = r - core;
    if (span <= 0) return 255;
    return clampi((r - d) * 255 / span, 0, 255);
}

// Coverage of the ACTIVE brush at offset (dx,dy) from the dab center for a dab
// of radius r. When no bitmap brush is selected (brush_current()==-1) this is
// byte-identical to stamp_cov (the parametric round brush). When a bitmap brush
// is selected its stock grayscale mask is nearest-neighbour scaled to the dab's
// (2r+1)-wide bounding box and sampled, then hardness acts as a flat alpha
// multiplier (the bitmap already carries its own soft edge).
static int dab_cov(int dx, int dy, int r, int hardness) {
    int bi = brush_current();
    if (bi < 0) return stamp_cov(dx, dy, r, hardness);
    const studio_brush_t *b = brush_get(bi);
    if (!b || !b->mask || b->w <= 0 || b->h <= 0)
        return stamp_cov(dx, dy, r, hardness);
    int dim = 2 * r + 1;
    if (dim < 1) dim = 1;
    int bx = (dx + r) * b->w / dim;               // nearest-neighbour, centered
    int by = (dy + r) * b->h / dim;
    int m = brush_sample(bi, bx, by);             // 0..255
    m = m * clampi(hardness, 0, 255) / 255;       // hardness as alpha multiplier
    return clampi(m, 0, 255);
}

// Dab spacing in pixels along a stroke. For the parametric brush this keeps the
// caller's value; for a bitmap brush it is the brush's spacing percent (GIMP
// semantics, clamp 5..200) of the dab size.
static int brush_spacing_px(int size, int fallback) {
    int bi = brush_current();
    if (bi < 0) return fallback;
    const studio_brush_t *b = brush_get(bi);
    if (!b) return fallback;
    int pct = clampi(b->spacing, 5, 200);
    int sp = size * pct / 100;
    if (sp < 1) sp = 1;
    return sp;
}

typedef enum { ST_PAINT, ST_ERASE, ST_CLONE, ST_SMUDGE, ST_BLURSTAMP,
               ST_DODGE, ST_BURN, ST_SHARPEN, ST_HEAL } stamp_mode_t;

// Heal tool: colour offset (destination - source) captured at stroke start so
// copied source texture takes on the destination's local tone.
static int heal_dr = 0, heal_dg = 0, heal_db = 0;

// Freehand stroke smoothing. Linear dab interpolation (stamp_line) already
// prevents gaps between samples, but a low mouse report rate delivers sparse,
// far-apart points so a fast flick renders as a coarse angular polyline. To
// smooth that we route the continuous paint tools through a midpoint-quadratic
// curve: the drawn path passes through the MIDPOINTS of consecutive input
// samples, using each raw sample as the Bezier control point. This is the
// standard robust cure for jumpy input and is independent of the mouse rate.
// st_mx/st_my hold the previous midpoint; st_have_mid marks that one exists.
// The last drag's paint parameters are stored so tool_end can flush the final
// midpoint->release segment (otherwise the stroke tail would stop half a
// sample short).
static int st_mx = 0, st_my = 0;
static int st_have_mid = 0;
static int st_paint_active = 0;             // a smoothed stroke laid >=1 segment
static uint32_t st_pcolor = 0;
static int st_psize = 0, st_popa = 0, st_phard = 0, st_pspacing = 1;
static stamp_mode_t st_pmode = ST_PAINT;

// One brush stamp at (cx,cy).
static void stamp(int cx, int cy, uint32_t color, int size, int opa,
                  int hardness, stamp_mode_t mode) {
    layer_t *L = al();
    if (!L || !L->px) return;
    int r = size / 2;
    if (r < 0) r = 0;
    int erase_paints_bg = (mode == ST_ERASE && g_doc.active == 0);

    if (mode == ST_BLURSTAMP) {
        // Box-blur (3x3) the footprint in place, reading from a copy.
        int x0 = clampi(cx - r, 0, g_doc.w - 1), x1 = clampi(cx + r, 0, g_doc.w - 1);
        int y0 = clampi(cy - r, 0, g_doc.h - 1), y1 = clampi(cy + r, 0, g_doc.h - 1);
        int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        if (bw <= 0 || bh <= 0) return;
        uint32_t *tmp = (uint32_t *)malloc((size_t)bw * (size_t)bh * 4);
        if (!tmp) return;
        for (int y = 0; y < bh; y++)
            memcpy(tmp + (size_t)y * (size_t)bw,
                   L->px + (size_t)(y0 + y) * (size_t)g_doc.w + x0, (size_t)bw * 4);
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                int cov = dab_cov(x - cx, y - cy, r, hardness) * opa / 255;
                cov = cov * sel_at(x, y) / 255;
                if (cov <= 0) continue;
                int sa = 0, sr = 0, sg = 0, sb = 0, n = 0;
                for (int ky = -1; ky <= 1; ky++) {
                    for (int kx = -1; kx <= 1; kx++) {
                        int tx = x - x0 + kx, ty = y - y0 + ky;
                        if (tx < 0 || ty < 0 || tx >= bw || ty >= bh) continue;
                        uint32_t c = tmp[(size_t)ty * (size_t)bw + tx];
                        sa += px_a(c); sr += px_r(c); sg += px_g(c); sb += px_b(c);
                        n++;
                    }
                }
                if (!n) continue;
                uint32_t blurred = argb(sa / n, sr / n, sg / n, sb / n);
                uint32_t *p = &L->px[(size_t)y * (size_t)g_doc.w + x];
                uint32_t d = *p;
                // lerp dst -> blurred by cov
                int a2 = px_a(d) + (px_a(blurred) - px_a(d)) * cov / 255;
                int r2 = px_r(d) + (px_r(blurred) - px_r(d)) * cov / 255;
                int g2 = px_g(d) + (px_g(blurred) - px_g(d)) * cov / 255;
                int b2 = px_b(d) + (px_b(blurred) - px_b(d)) * cov / 255;
                *p = argb(a2, r2, g2, b2);
            }
        }
        free(tmp);
        g_doc.comp_dirty = 1; g_doc.modified = 1;
        return;
    }

    if (mode == ST_SHARPEN) {
        // Unsharp mask under the brush: sharp = orig + (orig - blur3x3)*cov.
        int x0 = clampi(cx - r, 0, g_doc.w - 1), x1 = clampi(cx + r, 0, g_doc.w - 1);
        int y0 = clampi(cy - r, 0, g_doc.h - 1), y1 = clampi(cy + r, 0, g_doc.h - 1);
        int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
        if (bw <= 0 || bh <= 0) return;
        uint32_t *tmp = (uint32_t *)malloc((size_t)bw * (size_t)bh * 4);
        if (!tmp) return;
        for (int y = 0; y < bh; y++)
            memcpy(tmp + (size_t)y * (size_t)bw,
                   L->px + (size_t)(y0 + y) * (size_t)g_doc.w + x0, (size_t)bw * 4);
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                int cov = dab_cov(x - cx, y - cy, r, hardness) * opa / 255;
                cov = cov * sel_at(x, y) / 255;
                if (cov <= 0) continue;
                int sr = 0, sg = 0, sb = 0, n = 0;
                for (int ky = -1; ky <= 1; ky++)
                    for (int kx = -1; kx <= 1; kx++) {
                        int tx = x - x0 + kx, ty = y - y0 + ky;
                        if (tx < 0 || ty < 0 || tx >= bw || ty >= bh) continue;
                        uint32_t c = tmp[(size_t)ty * (size_t)bw + tx];
                        sr += px_r(c); sg += px_g(c); sb += px_b(c); n++;
                    }
                if (!n) continue;
                uint32_t o = tmp[(size_t)(y - y0) * (size_t)bw + (x - x0)];
                int nr = clampi(px_r(o) + (px_r(o) - sr / n) * cov / 255, 0, 255);
                int ng = clampi(px_g(o) + (px_g(o) - sg / n) * cov / 255, 0, 255);
                int nb = clampi(px_b(o) + (px_b(o) - sb / n) * cov / 255, 0, 255);
                L->px[(size_t)y * (size_t)g_doc.w + x] = argb(px_a(o), nr, ng, nb);
            }
        }
        free(tmp);
        g_doc.comp_dirty = 1; g_doc.modified = 1;
        return;
    }

    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int cov = dab_cov(dx, dy, r, hardness);
            if (cov <= 0) continue;
            cov = cov * opa / 255;
            if (cov <= 0) continue;
            int x = cx + dx, y = cy + dy;
            switch (mode) {
            case ST_PAINT:
                put_px(L->px, x, y, color, cov);
                break;
            case ST_ERASE:
                if (erase_paints_bg) put_px(L->px, x, y, g_tool.bg, cov);
                else                 erase_px(L->px, x, y, cov);
                break;
            case ST_CLONE: {
                int sx = x + clone_offx, sy = y + clone_offy;
                if (sx < 0 || sy < 0 || sx >= g_doc.w || sy >= g_doc.h) break;
                uint32_t c = L->px[(size_t)sy * (size_t)g_doc.w + sx];
                if (px_a(c) == 0) break;
                put_px(L->px, x, y, c, cov * px_a(c) / 255);
                break;
            }
            case ST_SMUDGE: {
                if (!smudge_patch) break;
                int tx = dx + smudge_patch_dim / 2, ty = dy + smudge_patch_dim / 2;
                if (tx < 0 || ty < 0 || tx >= smudge_patch_dim || ty >= smudge_patch_dim) break;
                uint32_t c = smudge_patch[(size_t)ty * (size_t)smudge_patch_dim + tx];
                if (px_a(c) == 0) break;
                put_px(L->px, x, y, c, cov / 2);   // 50% pull
                break;
            }
            case ST_DODGE: {                        // lighten toward white by cov
                if (x < 0 || y < 0 || x >= g_doc.w || y >= g_doc.h) break;
                if (sel_at(x, y) <= 0) break;
                uint32_t *p = &L->px[(size_t)y * (size_t)g_doc.w + x];
                uint32_t d = *p;
                if (px_a(d) == 0) break;
                int e = cov * sel_at(x, y) / 255;
                int nr = px_r(d) + (255 - px_r(d)) * e / 255;
                int ng = px_g(d) + (255 - px_g(d)) * e / 255;
                int nb = px_b(d) + (255 - px_b(d)) * e / 255;
                *p = argb(px_a(d), nr, ng, nb);
                break;
            }
            case ST_BURN: {                         // darken toward black by cov
                if (x < 0 || y < 0 || x >= g_doc.w || y >= g_doc.h) break;
                if (sel_at(x, y) <= 0) break;
                uint32_t *p = &L->px[(size_t)y * (size_t)g_doc.w + x];
                uint32_t d = *p;
                if (px_a(d) == 0) break;
                int e = cov * sel_at(x, y) / 255;
                int nr = px_r(d) - px_r(d) * e / 255;
                int ng = px_g(d) - px_g(d) * e / 255;
                int nb = px_b(d) - px_b(d) * e / 255;
                *p = argb(px_a(d), nr, ng, nb);
                break;
            }
            case ST_HEAL: {                         // clone texture, match dst tone
                int sx = x + clone_offx, sy = y + clone_offy;
                if (sx < 0 || sy < 0 || sx >= g_doc.w || sy >= g_doc.h) break;
                uint32_t c = L->px[(size_t)sy * (size_t)g_doc.w + sx];
                if (px_a(c) == 0) break;
                uint32_t hc = argb(255,
                                   clampi(px_r(c) + heal_dr, 0, 255),
                                   clampi(px_g(c) + heal_dg, 0, 255),
                                   clampi(px_b(c) + heal_db, 0, 255));
                put_px(L->px, x, y, hc, cov * px_a(c) / 255);
                break;
            }
            default:
                break;
            }
        }
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1;
}

// Stamp along the segment (x0,y0)-(x1,y1) with the given spacing.
static void stamp_line(int x0, int y0, int x1, int y1, uint32_t color, int size,
                       int opa, int hardness, stamp_mode_t mode, int spacing) {
    int dx = x1 - x0, dy = y1 - y0;
    int dist = (int)isqrt32((uint32_t)(dx * dx + dy * dy));
    // A bitmap brush spaces its dabs by its own spacing percent (GIMP), overriding
    // the caller's value; the parametric round brush keeps the caller's spacing.
    if (brush_current() >= 0) spacing = brush_spacing_px(size, spacing);
    if (spacing < 1) spacing = 1;
    int steps = dist / spacing;
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; i++) {
        stamp(x0 + dx * i / steps, y0 + dy * i / steps, color, size, opa,
              hardness, mode);
    }
}

// Stamp along the quadratic Bezier P0 -> (control C) -> P1 at the given spacing.
// Same dab-density semantics as stamp_line (bitmap brushes override spacing with
// their own percent), so opacity build-up matches a straight segment.
static void stamp_qbez(int x0, int y0, int cx, int cy, int x1, int y1,
                       uint32_t color, int size, int opa, int hardness,
                       stamp_mode_t mode, int spacing) {
    if (brush_current() >= 0) spacing = brush_spacing_px(size, spacing);
    if (spacing < 1) spacing = 1;
    // Upper-bound the arc length by the control polygon; slightly over-samples,
    // which only makes the curve denser (smoother), never gappy.
    int l0 = (int)isqrt32((uint32_t)((cx - x0) * (cx - x0) + (cy - y0) * (cy - y0)));
    int l1 = (int)isqrt32((uint32_t)((x1 - cx) * (x1 - cx) + (y1 - cy) * (y1 - cy)));
    int len = l0 + l1;
    int steps = len / spacing;
    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; i++) {
        long long t = (long long)i * 1024 / steps;   // 0..1024 fixed point
        long long u = 1024 - t;
        long long uu = u * u, ut = 2 * u * t, tt = t * t;
        int px = (int)((uu * x0 + ut * cx + tt * x1) / (1024LL * 1024LL));
        int py = (int)((uu * y0 + ut * cy + tt * y1) / (1024LL * 1024LL));
        stamp(px, py, color, size, opa, hardness, mode);
    }
}

// Smoothed freehand segment for the continuous paint tools. Uses st_lx/st_ly
// (the PREVIOUS input sample, still current here because tool_drag updates them
// only after the switch) as the control point and the midpoint of (previous,
// current) as the curve endpoint. The first segment of a stroke has no prior
// midpoint, so it runs straight from the begin point to the first midpoint.
static void paint_smooth(int x, int y, uint32_t color, int size, int opa,
                         int hardness, stamp_mode_t mode, int spacing) {
    int mx = (st_lx + x) / 2, my = (st_ly + y) / 2;
    if (!st_have_mid) {
        stamp_line(st_bx, st_by, mx, my, color, size, opa, hardness, mode, spacing);
        st_have_mid = 1;
    } else {
        stamp_qbez(st_mx, st_my, st_lx, st_ly, mx, my,
                   color, size, opa, hardness, mode, spacing);
    }
    st_mx = mx; st_my = my;
    // Remember params so tool_end can flush the tail (last midpoint -> release).
    st_paint_active = 1;
    st_pcolor = color; st_psize = size; st_popa = opa;
    st_phard = hardness; st_pmode = mode; st_pspacing = spacing;
}

// Grab a size x size patch centered at (cx,cy) into smudge_patch.
static void smudge_grab(int cx, int cy, int size) {
    layer_t *L = al();
    if (!L || !L->px) return;
    int dim = size | 1;                       // odd, so it has a center
    if (dim < 3) dim = 3;
    if (smudge_patch_dim != dim) {
        free(smudge_patch);
        smudge_patch = (uint32_t *)malloc((size_t)dim * (size_t)dim * 4);
        smudge_patch_dim = smudge_patch ? dim : 0;
    }
    if (!smudge_patch) return;
    for (int y = 0; y < dim; y++) {
        for (int x = 0; x < dim; x++) {
            int sx = cx - dim / 2 + x, sy = cy - dim / 2 + y;
            uint32_t c = 0;
            if (sx >= 0 && sy >= 0 && sx < g_doc.w && sy < g_doc.h)
                c = L->px[(size_t)sy * (size_t)g_doc.w + sx];
            smudge_patch[(size_t)y * (size_t)dim + x] = c;
        }
    }
}

// ---------------------------------------------------------------------------
// Flood fill (BFS, tolerance on the active layer's pixels incl. alpha)
// ---------------------------------------------------------------------------
static void flood_fill(int x, int y) {
    layer_t *L = al();
    if (!L || !L->px) return;
    if (x < 0 || y < 0 || x >= g_doc.w || y >= g_doc.h) return;
    int w = g_doc.w, h = g_doc.h;
    uint32_t seed = L->px[(size_t)y * (size_t)w + x];
    int tol = clampi(g_tool.wand_tolerance, 0, 255);

    uint8_t *seen = (uint8_t *)malloc((size_t)w * (size_t)h);
    int *queue = (int *)malloc((size_t)w * (size_t)h * sizeof(int));
    if (!seen || !queue) { free(seen); free(queue); return; }
    memset(seen, 0, (size_t)w * (size_t)h);

    int qh = 0, qt = 0;
    queue[qt++] = y * w + x;
    seen[y * w + x] = 1;
    int sr = px_r(seed), sg = px_g(seed), sb = px_b(seed), sa = px_a(seed);
    // Pattern-fill mode: fill each region pixel from the tiled pattern instead of
    // solid FG. Default (g_brush_pattern_fill==0) keeps solid-FG fill. Either way
    // the write goes through put_px so selection/mask/lock-alpha still apply.
    int use_pat = (g_brush_pattern_fill && pattern_current() >= 0);

    while (qh < qt) {
        int p = queue[qh++];
        int py = p / w, pxx = p % w;
        uint32_t fc = use_pat ? pattern_px(pattern_current(), pxx, py) : g_tool.fg;
        put_px(L->px, pxx, py, fc, g_tool.opacity);
        static const int dx4[4] = { 1, -1, 0, 0 };
        static const int dy4[4] = { 0, 0, 1, -1 };
        for (int k = 0; k < 4; k++) {
            int nx = pxx + dx4[k], ny = py + dy4[k];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            int np = ny * w + nx;
            if (seen[np]) continue;
            uint32_t c = L->px[(size_t)np];
            int d = abs(px_r(c) - sr);
            int t = abs(px_g(c) - sg); if (t > d) d = t;
            t = abs(px_b(c) - sb);     if (t > d) d = t;
            t = abs(px_a(c) - sa);     if (t > d) d = t;
            if (d <= tol) { seen[np] = 1; queue[qt++] = np; }
        }
    }
    free(seen);
    free(queue);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
}

// ---------------------------------------------------------------------------
// Gradient fg -> bg projected on the begin->end axis, over selection/layer
// ---------------------------------------------------------------------------
static void gradient_commit(int x0, int y0, int x1, int y1) {
    layer_t *L = al();
    if (!L || !L->px) return;
    int vx = x1 - x0, vy = y1 - y0;
    int64_t len2 = (int64_t)vx * vx + (int64_t)vy * vy;
    if (len2 <= 0) return;
    int shape = grad_shape_get();
    int fr = px_r(g_tool.fg), fg2 = px_g(g_tool.fg), fb = px_b(g_tool.fg);
    int br = px_r(g_tool.bg), bg2 = px_g(g_tool.bg), bb = px_b(g_tool.bg);
    for (int y = 0; y < g_doc.h; y++) {
        for (int x = 0; x < g_doc.w; x++) {
            int s = sel_at(x, y);
            if (s <= 0) continue;
            // Shape (linear/bilinear/radial/square/conical/spiral) yields a raw
            // position, then repeat/reverse remap it; the colour interpolation
            // and the blend_px + put_px write path below are unchanged.
            int t01 = grad_shape_t01(shape, x, y, x0, y0, x1, y1);   // 0..65536
            t01 = grad_apply_repeat(t01);
            int t = clampi(t01 >> 8, 0, 256);                       // 0..256
            uint32_t c = argb(255,
                              fr + (br - fr) * t / 256,
                              fg2 + (bg2 - fg2) * t / 256,
                              fb + (bb - fb) * t / 256);
            // Route through the shared layer compositor so the gradient honours
            // its blend mode (Normal/Multiply/Screen/Difference/Overlay). blend_px
            // bakes in tool opacity; put_px then applies selection coverage plus
            // the layer's mask/lock-alpha rules exactly as before. For BLEND_NORMAL
            // this is identical to the old direct write.
            uint32_t dst = L->px[(size_t)y * (size_t)g_doc.w + x];
            uint32_t out = blend_px(dst, c, (blend_t)g_tool.grad_blend, g_tool.opacity);
            put_px(L->px, x, y, out, 255);
        }
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1;
}

// ---------------------------------------------------------------------------
// Shape outlines committed with the brush stamp
// ---------------------------------------------------------------------------
static void shape_line(int x0, int y0, int x1, int y1) {
    stamp_line(x0, y0, x1, y1, g_tool.fg, g_tool.size, g_tool.opacity,
               g_tool.hardness, ST_PAINT, 1);
}

static void shape_rect(int x0, int y0, int x1, int y1) {
    shape_line(x0, y0, x1, y0);
    shape_line(x1, y0, x1, y1);
    shape_line(x1, y1, x0, y1);
    shape_line(x0, y1, x0, y0);
}

static void shape_ellipse(int x0, int y0, int x1, int y1) {
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    int rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    if (rx < 1) rx = 1;
    if (ry < 1) ry = 1;
    int cx = x0 + rx, cy = y0 + ry;
    // Walk the parameter with enough segments for smoothness; stamp each point.
    int per = 4 * (rx + ry);
    if (per < 16) per = 16;
    int px_ = cx + rx, py_ = cy;
    for (int i = 1; i <= per; i++) {
        // 16.16 angle step around the circle using the integer sine below.
        // Angle = i * 2pi / per; use a 1024-step sine table computed on the fly
        // via the small-angle recurrence (integer rotation).
        // Simpler: parametric via isqrt on x-scan per octant is messy for
        // ellipses; use fixed-point rotation instead.
        // Rotation recurrence would drift; with per ~ perimeter the cheap and
        // exact approach is to evaluate x = rx*cos, y = ry*sin from a scaled
        // integer table. Build the table lazily once (1024 entries, 4KB).
        static int32_t *sin_tab = NULL;    // sin in Q15, 1024 steps per turn
        if (!sin_tab) {
            sin_tab = (int32_t *)malloc(1024 * sizeof(int32_t));
            if (!sin_tab) return;
            // Integer sine via the recurrence s[n+1] = s[n]*c2 - s[n-1]
            // is drift-prone; instead fill by symmetry from a quarter wave
            // computed with the standard integer approximation:
            //   sin(a) ~ Bhaskara: 16a(pi-a)/(5pi^2 - 4a(pi-a)), scaled.
            for (int k = 0; k < 1024; k++) {
                int q = k & 511;               // half period position
                // a in units where half period = 512
                int64_t a = q, pi = 512;
                int64_t num = 16 * a * (pi - a) * 32768;
                int64_t den = 5 * pi * pi - 4 * a * (pi - a);
                int32_t s = (int32_t)(num / (den * 4));   // ~Q15
                if (s > 32767) s = 32767;
                sin_tab[k] = (k < 512) ? s : -s;
            }
        }
        int idx = (int)(((int64_t)i << 10) / per) & 1023;
        int cs = sin_tab[(idx + 256) & 1023];             // cos = sin(a+90deg)
        int sn = sin_tab[idx];
        int nx = cx + (int)(((int64_t)rx * cs) >> 15);
        int ny = cy + (int)(((int64_t)ry * sn) >> 15);
        shape_line(px_, py_, nx, ny);
        px_ = nx; py_ = ny;
    }
}

// ---------------------------------------------------------------------------
// Text stamping with a compact built-in 8x8 font (ASCII 32..126). Public
// domain "font8x8_basic" data; each byte is a row, bit N = pixel at x=N.
// 95 glyphs x 8 bytes = 760 bytes (small static const is fine).
// ---------------------------------------------------------------------------
static const uint8_t font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // !
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // "
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // #
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // $
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // %
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // &
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // '
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // (
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // )
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // *
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // +
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // ,
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // -
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // .
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // /
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // 0
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // 1
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // 2
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // 3
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // 4
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // 5
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // 6
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // 7
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // 8
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // 9
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // :
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // ;
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // <
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // =
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // >
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // ?
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // @
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // A
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // B
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // C
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // D
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // E
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // F
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // G
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // H
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // I
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // J
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // K
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // L
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // M
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // N
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // O
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // P
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // Q
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // R
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // S
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // T
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // U
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // V
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // W
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // X
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // Y
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // Z
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // [
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // backslash
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // ]
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // _
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // `
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // a
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, // b
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // c
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, // d
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, // e
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, // f
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // g
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // h
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // i
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // j
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // k
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // l
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // m
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // n
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // o
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // p
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // q
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // r
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // s
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // t
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // u
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // v
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // w
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // x
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // y
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, // z
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // {
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // |
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // }
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, // ~
};

// Stamp the text string onto the active layer as real antialiased TrueType,
// using the OS font registry (selected face + point size + bold/italic +
// underline). Each glyph is rasterized by the kernel (font_glyph) into an
// alpha coverage bitmap that we composite through put_px so the selection
// mask, layer opacity and lock-alpha all still apply. Falls back to the
// compact 8x8 bitmap font only if the TTF registry is unavailable.
static unsigned char g_text_gbmp[192 * 192];
static void text_commit(int x, int y) {
    layer_t *L = al();
    if (!L || !L->px || !g_tool.text[0]) return;

    int face  = g_tool.text_font; if (face < 0) face = 0;
    int size  = clampi(g_tool.size, 6, 64);
    int style = (g_tool.text_bold   ? FONT_STYLE_BOLD   : 0) |
                (g_tool.text_italic ? FONT_STYLE_ITALIC : 0);

    // Vertical metrics -> baseline. If the font system is not ready these come
    // back as {size,0,0}, which still gives a usable baseline.
    int m[3] = { size, 0, 0 };
    font_metrics(face, size, m);
    int ascent = m[0] > 0 ? m[0] : size;

    // Probe the registry: a valid glyph advance means TTF is available.
    font_glyph_meta_t meta;
    int probe = font_glyph(face, size, style, 'A', &meta, g_text_gbmp, (int)sizeof(g_text_gbmp));
    if (probe < 0) {
        // Fallback: the legacy 8x8 bitmap stamp (integer-scaled) so text still
        // works on a kernel without the font registry.
        int scale = clampi(size / 8, 1, 8);
        int cx = x;
        for (const char *s = g_tool.text; *s; s++) {
            unsigned char ch = (unsigned char)*s;
            if (ch < 32 || ch > 126) { cx += 8 * scale; continue; }
            const uint8_t *gl = font8x8[ch - 32];
            for (int gy = 0; gy < 8; gy++)
                for (int gx = 0; gx < 8; gx++)
                    if ((gl[gy] >> gx) & 1)
                        for (int sy = 0; sy < scale; sy++)
                            for (int sx = 0; sx < scale; sx++)
                                put_px(L->px, cx + gx * scale + sx, y + gy * scale + sy,
                                       g_tool.fg, g_tool.opacity);
            cx += 8 * scale;
        }
        g_doc.comp_dirty = 1; g_doc.modified = 1;
        return;
    }

    int baseline = y + ascent;
    int cx = x;
    for (const unsigned char *s = (const unsigned char *)g_tool.text; *s; s++) {
        int cp = *s;
        int adv = font_glyph(face, size, style, cp, &meta, g_text_gbmp, (int)sizeof(g_text_gbmp));
        if (adv < 0) { cx += size / 2; continue; }
        if (meta.width > 0 && meta.height > 0 &&
            (long)meta.width * meta.height <= (long)sizeof(g_text_gbmp)) {
            int gx = cx + meta.xoff;
            int gy = baseline + meta.yoff;
            for (int ry = 0; ry < meta.height; ry++) {
                for (int rx = 0; rx < meta.width; rx++) {
                    unsigned char a = g_text_gbmp[ry * meta.width + rx];
                    if (!a) continue;
                    int alpha = a * g_tool.opacity / 255;
                    if (alpha > 0) put_px(L->px, gx + rx, gy + ry, g_tool.fg, alpha);
                }
            }
        }
        cx += adv;
        if (s[1]) cx += font_kern(face, size, cp, s[1]);
    }

    // Underline: a solid rule just below the baseline spanning the text run.
    if (g_tool.text_underline && cx > x) {
        int uoff = size / 12; if (uoff < 1) uoff = 1;
        int uy = baseline + uoff + 1;
        int th = (size >= 28) ? 2 : 1;
        for (int t = 0; t < th; t++)
            for (int ux = x; ux < cx; ux++)
                put_px(L->px, ux, uy + t, g_tool.fg, g_tool.opacity);
    }

    g_doc.comp_dirty = 1; g_doc.modified = 1;
}

// ---------------------------------------------------------------------------
// Move: shift the whole active layer by (dx,dy); vacated area transparent.
// v1 moves the full layer (not the selection contents).
// ---------------------------------------------------------------------------
static void shift_layer(int dx, int dy) {
    layer_t *L = al();
    if (!L || !L->px || (dx == 0 && dy == 0)) return;
    int w = g_doc.w, h = g_doc.h;
    if (abs(dx) >= w || abs(dy) >= h) {
        memset(L->px, 0, (size_t)w * (size_t)h * 4);
        g_doc.comp_dirty = 1; g_doc.modified = 1;
        return;
    }
    int ystart = dy > 0 ? h - 1 : 0;
    int yend   = dy > 0 ? -1 : h;
    int ystep  = dy > 0 ? -1 : 1;
    for (int y = ystart; y != yend; y += ystep) {
        uint32_t *dst = L->px + (size_t)y * (size_t)w;
        int sy = y - dy;
        if (sy < 0 || sy >= h) {
            memset(dst, 0, (size_t)w * 4);
            continue;
        }
        uint32_t *src = L->px + (size_t)sy * (size_t)w;
        int n = w - abs(dx);
        if (dx >= 0) {
            memmove(dst + dx, src, (size_t)n * 4);
            memset(dst, 0, (size_t)dx * 4);
        } else {
            memmove(dst, src - dx, (size_t)n * 4);
            memset(dst + n, 0, (size_t)(-dx) * 4);
        }
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1;
}

// ---------------------------------------------------------------------------
// Tool lifecycle
// ---------------------------------------------------------------------------
static int tool_mutates(tool_id_t t) {
    switch (t) {
    case TL_BRUSH: case TL_PENCIL: case TL_ERASER: case TL_AIRBRUSH:
    case TL_SMUDGE: case TL_BLUR: case TL_FILL: case TL_GRADIENT:
    case TL_LINE: case TL_RECT: case TL_ELLIPSE: case TL_TEXT: case TL_MOVE:
    case TL_HEAL: case TL_DODGE: case TL_BURN: case TL_SHARPEN: case TL_INK:
    case TL_CROP:
        return 1;
    default:
        return 0;
    }
}

void tool_begin(int x, int y) {
    if (g_doc.nlayers <= 0) return;
    // Selecting a different tool invalidates the clone anchor.
    if (g_tool.id != last_tool_id) {
        clone_have_src = 0;
        last_tool_id = g_tool.id;
    }
    st_active = 1;
    st_bx = st_lx = x;
    st_by = st_ly = y;
    st_have_mid = 0;
    st_paint_active = 0;
    clone_painting = 0;

    if (g_tool.id == TL_CLONE || g_tool.id == TL_HEAL) {
        if (!clone_have_src) {
            clone_ax = x; clone_ay = y;
            clone_have_src = 1;
            return;                          // anchor click paints nothing
        }
        clone_offx = clone_ax - x;
        clone_offy = clone_ay - y;
        clone_painting = 1;
        if (g_tool.id == TL_HEAL) {
            // Capture the tone offset (destination start - source anchor) so the
            // copied source texture is retinted to the destination's local tone.
            layer_t *L = al();
            heal_dr = heal_dg = heal_db = 0;
            if (L && L->px && x >= 0 && y >= 0 && x < g_doc.w && y < g_doc.h) {
                uint32_t ds = L->px[(size_t)y * (size_t)g_doc.w + x];
                uint32_t ss = L->px[(size_t)clone_ay * (size_t)g_doc.w + clone_ax];
                heal_dr = px_r(ds) - px_r(ss);
                heal_dg = px_g(ds) - px_g(ss);
                heal_db = px_b(ds) - px_b(ss);
            }
        }
    }

    if (tool_mutates(g_tool.id) &&
        !((g_tool.id == TL_CLONE || g_tool.id == TL_HEAL) && !clone_painting))
        undo_push(tool_name(g_tool.id));

    switch (g_tool.id) {
    case TL_BRUSH:
        stamp(x, y, g_tool.fg, g_tool.size, g_tool.opacity, g_tool.hardness, ST_PAINT);
        break;
    case TL_PENCIL:
        stamp(x, y, g_tool.fg, g_tool.size, 255, 255, ST_PAINT);
        break;
    case TL_ERASER:
        stamp(x, y, 0, g_tool.size, g_tool.opacity, g_tool.hardness, ST_ERASE);
        break;
    case TL_AIRBRUSH:
        stamp(x, y, g_tool.fg, g_tool.size, g_tool.opacity / 8 + 1,
              g_tool.hardness / 2, ST_PAINT);
        break;
    case TL_CLONE:
        stamp(x, y, 0, g_tool.size, g_tool.opacity, g_tool.hardness, ST_CLONE);
        break;
    case TL_SMUDGE:
        smudge_grab(x, y, g_tool.size);
        break;
    case TL_BLUR:
        stamp(x, y, 0, g_tool.size, g_tool.opacity, g_tool.hardness, ST_BLURSTAMP);
        break;
    case TL_FILL:
        flood_fill(x, y);
        break;
    case TL_PICK: {
        doc_composite();
        if (g_doc.comp && x >= 0 && y >= 0 && x < g_doc.w && y < g_doc.h) {
            uint32_t c = g_doc.comp[(size_t)y * (size_t)g_doc.w + x];
            g_tool.fg = argb(255, px_r(c), px_g(c), px_b(c));
        }
        break;
    }
    case TL_SEL_LASSO:
        sel_lasso_begin(x, y);
        break;
    case TL_SEL_WAND:
        sel_wand(x, y, g_tool.wand_tolerance);
        break;
    case TL_HEAL:
        if (clone_painting)
            stamp(x, y, 0, g_tool.size, g_tool.opacity, g_tool.hardness, ST_HEAL);
        break;
    case TL_DODGE:
        stamp(x, y, 0, g_tool.size, g_tool.opacity, g_tool.hardness, ST_DODGE);
        break;
    case TL_BURN:
        stamp(x, y, 0, g_tool.size, g_tool.opacity, g_tool.hardness, ST_BURN);
        break;
    case TL_SHARPEN:
        stamp(x, y, 0, g_tool.size, g_tool.opacity, g_tool.hardness, ST_SHARPEN);
        break;
    case TL_INK:
        stamp(x, y, g_tool.fg, g_tool.size, g_tool.opacity, 255, ST_PAINT);
        break;
    case TL_SEL_BYCOLOR:
        sel_by_color(x, y, g_tool.wand_tolerance);
        break;
    case TL_PATH:
        path_add_node(x, y, 0, 0, 0, 0);
        ui_status("Path: node added");
        break;
    case TL_MEASURE:
        ui_status("Measure: drag to measure");
        break;
    default:
        break;                               // shape/gradient/crop/text/move: on end
    }
}

void tool_drag(int x, int y) {
    if (!st_active) return;
    int spacing = g_tool.size / 4;
    if (spacing < 1) spacing = 1;

    switch (g_tool.id) {
    case TL_BRUSH:
        paint_smooth(x, y, g_tool.fg, g_tool.size, g_tool.opacity,
                     g_tool.hardness, ST_PAINT, spacing);
        break;
    case TL_PENCIL:
        paint_smooth(x, y, g_tool.fg, g_tool.size, 255, 255, ST_PAINT, 1);
        break;
    case TL_ERASER:
        paint_smooth(x, y, 0, g_tool.size, g_tool.opacity, g_tool.hardness,
                     ST_ERASE, 1);
        break;
    case TL_AIRBRUSH:
        stamp(x, y, g_tool.fg, g_tool.size, g_tool.opacity / 8 + 1,
              g_tool.hardness / 2, ST_PAINT);
        break;
    case TL_CLONE:
        if (clone_painting)
            paint_smooth(x, y, 0, g_tool.size, g_tool.opacity,
                         g_tool.hardness, ST_CLONE, spacing);
        break;
    case TL_SMUDGE:
        stamp(x, y, 0, g_tool.size, g_tool.opacity, g_tool.hardness / 2, ST_SMUDGE);
        smudge_grab(x, y, g_tool.size);
        break;
    case TL_BLUR:
        stamp(x, y, 0, g_tool.size, g_tool.opacity, g_tool.hardness, ST_BLURSTAMP);
        break;
    case TL_MOVE:
        shift_layer(x - st_lx, y - st_ly);
        break;
    case TL_SEL_LASSO:
        sel_lasso_point(x, y);
        break;
    case TL_HEAL:
        if (clone_painting)
            paint_smooth(x, y, 0, g_tool.size, g_tool.opacity,
                         g_tool.hardness, ST_HEAL, spacing);
        break;
    case TL_DODGE:
        paint_smooth(x, y, 0, g_tool.size, g_tool.opacity,
                     g_tool.hardness, ST_DODGE, spacing);
        break;
    case TL_BURN:
        paint_smooth(x, y, 0, g_tool.size, g_tool.opacity,
                     g_tool.hardness, ST_BURN, spacing);
        break;
    case TL_SHARPEN:
        stamp(x, y, 0, g_tool.size, g_tool.opacity, g_tool.hardness, ST_SHARPEN);
        break;
    case TL_INK: {
        // Calligraphic nib: faster strokes lay a thinner line, slow strokes
        // fatten it (size scaled by an inverse-speed factor, clamped).
        int dxs = x - st_lx, dys = y - st_ly;
        int speed = (int)isqrt32((uint32_t)(dxs * dxs + dys * dys));
        int nib = g_tool.size - speed / 2;
        nib = clampi(nib, (g_tool.size / 4) + 1, g_tool.size);
        stamp_line(st_lx, st_ly, x, y, g_tool.fg, nib, g_tool.opacity, 255,
                   ST_PAINT, 1);
        break;
    }
    case TL_MEASURE: {
        int dxs = x - st_bx, dys = y - st_by;
        int dist = (int)isqrt32((uint32_t)(dxs * dxs + dys * dys));
        int ang = iatan2_deg(dys, dxs);
        char msg[64];
        snprintf(msg, sizeof(msg), "Measure: %d px, %d deg", dist, ang);
        ui_status(msg);
        break;
    }
    default:
        break;                               // shapes/crop preview in ui.c
    }
    st_lx = x;
    st_ly = y;
}

void tool_end(int x, int y) {
    if (!st_active) return;
    // Smoothed freehand strokes draw only up to the last sample midpoint during
    // the drag; flush the final midpoint -> release segment so the stroke ends
    // exactly under the cursor.
    if (st_paint_active && st_have_mid)
        stamp_line(st_mx, st_my, x, y, st_pcolor, st_psize, st_popa,
                   st_phard, st_pmode, st_pspacing);
    switch (g_tool.id) {
    case TL_LINE:
        shape_line(st_bx, st_by, x, y);
        break;
    case TL_RECT:
        shape_rect(st_bx, st_by, x, y);
        break;
    case TL_ELLIPSE:
        shape_ellipse(st_bx, st_by, x, y);
        break;
    case TL_GRADIENT:
        gradient_commit(st_bx, st_by, x, y);
        break;
    case TL_TEXT:
        text_commit(x, y);
        break;
    case TL_SEL_RECT:
        sel_rect(st_bx, st_by, x, y, 0);
        break;
    case TL_SEL_ELLIPSE:
        sel_ellipse(st_bx, st_by, x, y, 0);
        break;
    case TL_SEL_LASSO:
        sel_lasso_end();
        break;
    case TL_CROP:
        // Drag defines a rectangle; crop the whole document to it via a
        // temporary selection so all layers stay canvas-consistent.
        sel_rect(st_bx, st_by, x, y, 0);
        doc_crop_to_selection();
        break;
    case TL_MEASURE: {
        int dxs = x - st_bx, dys = y - st_by;
        int dist = (int)isqrt32((uint32_t)(dxs * dxs + dys * dys));
        int ang = iatan2_deg(dys, dxs);
        char msg[64];
        snprintf(msg, sizeof(msg), "Measure: %d px, %d deg", dist, ang);
        ui_status(msg);
        break;
    }
    default:
        break;
    }
    st_active = 0;
}

// ===========================================================================
// Fixed-point trig + bilinear sampling for the transforms
// ===========================================================================

// sin/cos in Q15 (-32768..32768) via the Bhaskara I approximation, integer
// degrees. Good to well under 1% - fine for image rotation.
static int32_t isin_q15(int deg) {
    deg %= 360;
    if (deg < 0) deg += 360;
    int sign = 1;
    if (deg > 180) { deg -= 180; sign = -1; }
    int64_t x = deg;                              // 0..180
    int64_t num = 4 * x * (180 - x);
    int64_t den = 40500 - x * (180 - x);
    int32_t v = (int32_t)((num << 15) / den);
    if (v > 32767) v = 32767;
    return sign * v;
}
static int32_t icos_q15(int deg) { return isin_q15(deg + 90); }

// Single-pixel fetch: transparent when out of bounds.
static inline uint32_t txsamp(const uint32_t *src, int w, int h, int x, int y) {
    if (x < 0 || y < 0 || x >= w || y >= h) return 0;
    return src[(size_t)y * (size_t)w + x];
}

// Bilinear sample of src at (fx,fy) given in Q16 source coordinates. Alpha-
// weighted (premultiplied) so transparent neighbours do not darken the result.
static uint32_t bilerp(const uint32_t *src, int w, int h, int fx, int fy) {
    int ix = fx >> 16, iy = fy >> 16;
    int tx = (fx >> 8) & 255, ty = (fy >> 8) & 255;
    uint32_t c00 = txsamp(src, w, h, ix, iy),     c10 = txsamp(src, w, h, ix + 1, iy);
    uint32_t c01 = txsamp(src, w, h, ix, iy + 1), c11 = txsamp(src, w, h, ix + 1, iy + 1);
    int w00 = (255 - tx) * (255 - ty), w10 = tx * (255 - ty);
    int w01 = (255 - tx) * ty,         w11 = tx * ty;             // sum = 65025
    int64_t Aw = (int64_t)px_a(c00) * w00 + (int64_t)px_a(c10) * w10 +
                 (int64_t)px_a(c01) * w01 + (int64_t)px_a(c11) * w11;
    if (Aw <= 0) return 0;
    int64_t Rw = (int64_t)px_r(c00) * px_a(c00) * w00 + (int64_t)px_r(c10) * px_a(c10) * w10 +
                 (int64_t)px_r(c01) * px_a(c01) * w01 + (int64_t)px_r(c11) * px_a(c11) * w11;
    int64_t Gw = (int64_t)px_g(c00) * px_a(c00) * w00 + (int64_t)px_g(c10) * px_a(c10) * w10 +
                 (int64_t)px_g(c01) * px_a(c01) * w01 + (int64_t)px_g(c11) * px_a(c11) * w11;
    int64_t Bw = (int64_t)px_b(c00) * px_a(c00) * w00 + (int64_t)px_b(c10) * px_a(c10) * w10 +
                 (int64_t)px_b(c01) * px_a(c01) * w01 + (int64_t)px_b(c11) * px_a(c11) * w11;
    return argb((int)(Aw / 65025), (int)(Rw / Aw), (int)(Gw / Aw), (int)(Bw / Aw));
}

// Remap the active-idx layer (pixels + optional mask) through an inverse affine
// map given in Q16: source = (m00*x+m01*y+tx, m10*x+m11*y+ty). Same dimensions.
static int layer_affine(int idx, int64_t m00, int64_t m01, int64_t m10, int64_t m11,
                        int64_t tx, int64_t ty) {
    if (idx < 0 || idx >= g_doc.nlayers) return -1;
    layer_t *L = &g_doc.layer[idx];
    if (!L->px) return -1;
    int w = g_doc.w, h = g_doc.h;
    size_t n = (size_t)w * (size_t)h;
    uint32_t *src = (uint32_t *)malloc(n * 4);
    if (!src) return -1;
    memcpy(src, L->px, n * 4);
    uint8_t *msrc = NULL;
    if (L->mask) {
        msrc = (uint8_t *)malloc(n);
        if (!msrc) { free(src); return -1; }
        memcpy(msrc, L->mask, n);
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int64_t sxq = m00 * x + m01 * y + tx;
            int64_t syq = m10 * x + m11 * y + ty;
            L->px[(size_t)y * (size_t)w + x] = bilerp(src, w, h, (int)sxq, (int)syq);
            if (L->mask && msrc) {
                int sx = (int)(sxq >> 16), sy = (int)(syq >> 16);
                L->mask[(size_t)y * (size_t)w + x] =
                    (sx >= 0 && sy >= 0 && sx < w && sy < h) ? msrc[(size_t)sy * w + sx] : 0;
            }
        }
    }
    free(src);
    free(msrc);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ===========================================================================
// Transforms (tools.c). Per-layer ops keep the canvas size; doc-wide ops
// (rotate90/crop/resize) reallocate every layer to stay canvas-consistent.
// ===========================================================================

int layer_flip(int idx, int horizontal) {
    if (idx < 0 || idx >= g_doc.nlayers) return -1;
    layer_t *L = &g_doc.layer[idx];
    if (!L->px) return -1;
    int w = g_doc.w, h = g_doc.h;
    if (horizontal) {
        for (int y = 0; y < h; y++) {
            uint32_t *row = L->px + (size_t)y * w;
            uint8_t *mrow = L->mask ? L->mask + (size_t)y * w : NULL;
            for (int x = 0; x < w / 2; x++) {
                uint32_t t = row[x]; row[x] = row[w - 1 - x]; row[w - 1 - x] = t;
                if (mrow) { uint8_t m = mrow[x]; mrow[x] = mrow[w - 1 - x]; mrow[w - 1 - x] = m; }
            }
        }
    } else {
        for (int y = 0; y < h / 2; y++) {
            uint32_t *a = L->px + (size_t)y * w, *b = L->px + (size_t)(h - 1 - y) * w;
            for (int x = 0; x < w; x++) { uint32_t t = a[x]; a[x] = b[x]; b[x] = t; }
            if (L->mask) {
                uint8_t *ma = L->mask + (size_t)y * w, *mb = L->mask + (size_t)(h - 1 - y) * w;
                for (int x = 0; x < w; x++) { uint8_t t = ma[x]; ma[x] = mb[x]; mb[x] = t; }
            }
        }
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// CONTRACT-DEVIATION: layers are canvas-sized, so a 90-degree turn must swap
// the whole document's w/h. This rotates EVERY layer (and its mask) and the
// canvas; idx is validated but the rotation is document-wide (a lone-layer
// rotate would desync layer dims from the doc).
typedef struct { uint32_t *px; uint8_t *mask; } lbuf_t;

static lbuf_t *lbufs_alloc(int nw, int nh) {
    lbuf_t *b = (lbuf_t *)calloc((size_t)g_doc.nlayers, sizeof(lbuf_t));
    if (!b) return NULL;
    for (int i = 0; i < g_doc.nlayers; i++) {
        b[i].px = (uint32_t *)calloc((size_t)nw * (size_t)nh, 4);
        if (!b[i].px) goto fail;
        if (g_doc.layer[i].mask) {
            b[i].mask = (uint8_t *)calloc((size_t)nw * (size_t)nh, 1);
            if (!b[i].mask) goto fail;
        }
    }
    return b;
fail:
    for (int i = 0; i < g_doc.nlayers; i++) { free(b[i].px); free(b[i].mask); }
    free(b);
    return NULL;
}

static void lbufs_install(lbuf_t *b, int nw, int nh) {
    for (int i = 0; i < g_doc.nlayers; i++) {
        free(g_doc.layer[i].px);
        g_doc.layer[i].px = b[i].px;
        if (g_doc.layer[i].mask) { free(g_doc.layer[i].mask); g_doc.layer[i].mask = b[i].mask; }
    }
    free(b);
    free(g_doc.sel); g_doc.sel = NULL; g_doc.sel_active = 0;
    free(g_doc.comp);
    g_doc.comp = (uint32_t *)calloc((size_t)nw * (size_t)nh, 4);   // resized, recomputed on demand
    g_doc.w = nw; g_doc.h = nh;
    g_doc.comp_dirty = 1; g_doc.modified = 1;
}

int layer_rotate90(int idx, int cw) {
    if (idx < 0 || idx >= g_doc.nlayers) return -1;
    int w = g_doc.w, h = g_doc.h, nw = h, nh = w;
    lbuf_t *b = lbufs_alloc(nw, nh);
    if (!b) return -1;
    for (int i = 0; i < g_doc.nlayers; i++) {
        uint32_t *src = g_doc.layer[i].px;
        uint8_t *msrc = g_doc.layer[i].mask;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int nx, ny;
                if (cw) { nx = h - 1 - y; ny = x; }
                else    { nx = y;         ny = w - 1 - x; }
                b[i].px[(size_t)ny * nw + nx] = src[(size_t)y * w + x];
                if (b[i].mask && msrc)
                    b[i].mask[(size_t)ny * nw + nx] = msrc[(size_t)y * w + x];
            }
        }
    }
    lbufs_install(b, nw, nh);
    return 0;
}

int layer_rotate_arbitrary(int idx, int degrees) {
    if (idx < 0 || idx >= g_doc.nlayers) return -1;
    int cx = g_doc.w / 2, cy = g_doc.h / 2;
    int64_t co = icos_q15(degrees), sn = isin_q15(degrees);
    // inverse rotation (dest -> source), coefficients in Q16 (== Q15 value * 2)
    int64_t m00 = 2 * co, m01 = 2 * sn, m10 = -2 * sn, m11 = 2 * co;
    int64_t tx = ((int64_t)cx << 16) - (m00 * cx + m01 * cy);
    int64_t ty = ((int64_t)cy << 16) - (m10 * cx + m11 * cy);
    return layer_affine(idx, m00, m01, m10, m11, tx, ty);
}

int layer_scale(int idx, int new_w, int new_h) {
    if (idx < 0 || idx >= g_doc.nlayers) return -1;
    if (new_w <= 0 || new_h <= 0) return -1;
    int w = g_doc.w, h = g_doc.h;
    int offx = (w - new_w) / 2, offy = (h - new_h) / 2;
    int64_t m00 = ((int64_t)w << 16) / new_w;
    int64_t m11 = ((int64_t)h << 16) / new_h;
    int64_t tx = -(int64_t)offx * m00;
    int64_t ty = -(int64_t)offy * m11;
    return layer_affine(idx, m00, 0, 0, m11, tx, ty);
}

int layer_shear(int idx, int shear_x_pct, int shear_y_pct) {
    if (idx < 0 || idx >= g_doc.nlayers) return -1;
    int cx = g_doc.w / 2, cy = g_doc.h / 2;
    int64_t kx = ((int64_t)shear_x_pct << 16) / 100;   // Q16 shear factors
    int64_t ky = ((int64_t)shear_y_pct << 16) / 100;
    // inverse map: sx = x - kx*(y-cy), sy = y - ky*(x-cx)
    int64_t m00 = 1LL << 16, m01 = -kx, m10 = -ky, m11 = 1LL << 16;
    int64_t tx = kx * cy;
    int64_t ty = ky * cx;
    return layer_affine(idx, m00, m01, m10, m11, tx, ty);
}

int doc_crop_to_selection(void) {
    if (!g_doc.sel_active || !g_doc.sel) return -1;
    int w = g_doc.w, h = g_doc.h;
    int minx = w, miny = h, maxx = -1, maxy = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (g_doc.sel[(size_t)y * w + x]) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
    if (maxx < minx) return -1;
    int nw = maxx - minx + 1, nh = maxy - miny + 1;
    lbuf_t *b = lbufs_alloc(nw, nh);
    if (!b) return -1;
    for (int i = 0; i < g_doc.nlayers; i++) {
        uint32_t *src = g_doc.layer[i].px;
        uint8_t *msrc = g_doc.layer[i].mask;
        for (int y = 0; y < nh; y++) {
            memcpy(b[i].px + (size_t)y * nw,
                   src + (size_t)(miny + y) * w + minx, (size_t)nw * 4);
            if (b[i].mask && msrc)
                memcpy(b[i].mask + (size_t)y * nw,
                       msrc + (size_t)(miny + y) * w + minx, (size_t)nw);
        }
    }
    lbufs_install(b, nw, nh);
    return 0;
}

int doc_resize(int w, int h, int anchor) {
    if (w <= 0 || h <= 0 || w > STUDIO_MAX_W || h > STUDIO_MAX_H) return -1;
    anchor = clampi(anchor, 0, 8);
    int ow = g_doc.w, oh = g_doc.h;
    int col = anchor % 3, row = anchor / 3;
    int offx = (col == 0) ? 0 : (col == 1 ? (w - ow) / 2 : (w - ow));
    int offy = (row == 0) ? 0 : (row == 1 ? (h - oh) / 2 : (h - oh));
    lbuf_t *b = lbufs_alloc(w, h);
    if (!b) return -1;
    for (int i = 0; i < g_doc.nlayers; i++) {
        uint32_t *src = g_doc.layer[i].px;
        uint8_t *msrc = g_doc.layer[i].mask;
        for (int oy = 0; oy < oh; oy++) {
            int dy = oy + offy;
            if (dy < 0 || dy >= h) continue;
            for (int ox = 0; ox < ow; ox++) {
                int dx = ox + offx;
                if (dx < 0 || dx >= w) continue;
                b[i].px[(size_t)dy * w + dx] = src[(size_t)oy * ow + ox];
                if (b[i].mask && msrc)
                    b[i].mask[(size_t)dy * w + dx] = msrc[(size_t)oy * ow + ox];
            }
        }
    }
    lbufs_install(b, w, h);
    return 0;
}

// ===========================================================================
// Bezier path (tools.c): one working path, malloc-growable node list.
// ===========================================================================
typedef struct { int x, y, idx, idy, odx, ody; } pnode_t;
static pnode_t *g_path = NULL;
static int g_path_n = 0, g_path_cap = 0, g_path_closed = 0;

void path_reset(void) {
    free(g_path);
    g_path = NULL;
    g_path_n = 0; g_path_cap = 0; g_path_closed = 0;
}

void path_add_node(int x, int y, int in_dx, int in_dy, int out_dx, int out_dy) {
    if (g_path_n >= g_path_cap) {
        int nc = g_path_cap ? g_path_cap * 2 : 8;
        pnode_t *np = (pnode_t *)realloc(g_path, (size_t)nc * sizeof(pnode_t));
        if (!np) return;
        g_path = np; g_path_cap = nc;
    }
    g_path[g_path_n].x = x;   g_path[g_path_n].y = y;
    g_path[g_path_n].idx = in_dx;  g_path[g_path_n].idy = in_dy;
    g_path[g_path_n].odx = out_dx; g_path[g_path_n].ody = out_dy;
    g_path_n++;
}

void path_close(void) { if (g_path_n >= 3) g_path_closed = 1; }
int  path_node_count(void) { return g_path_n; }

// Cubic bezier point at t (0..256 fixed-point) for control points P0..P3.
static void bez(int t, int x0, int y0, int x1, int y1, int x2, int y2,
                int x3, int y3, int *ox, int *oy) {
    int u = 256 - t;
    int64_t a = (int64_t)u * u * u;
    int64_t b = 3LL * u * u * t;
    int64_t c = 3LL * u * t * t;
    int64_t d = (int64_t)t * t * t;
    int64_t den = 256LL * 256 * 256;
    *ox = (int)((a * x0 + b * x1 + c * x2 + d * x3) / den);
    *oy = (int)((a * y0 + b * y1 + c * y2 + d * y3) / den);
}

#define PATH_STEPS 18
// Flatten the path to a polyline. On success sets *oxs/*oys (caller frees) and
// returns the point count; returns 0 (pointers untouched) on failure.
static int path_flatten(int **oxs, int **oys) {
    if (g_path_n < 2) return 0;
    int segs = g_path_n - 1 + (g_path_closed ? 1 : 0);
    int cap = segs * PATH_STEPS + 2;
    int *xs = (int *)malloc((size_t)cap * sizeof(int));
    int *ys = (int *)malloc((size_t)cap * sizeof(int));
    if (!xs || !ys) { free(xs); free(ys); return 0; }
    int n = 0;
    xs[n] = g_path[0].x; ys[n] = g_path[0].y; n++;
    for (int s = 0; s < segs; s++) {
        int i = s, j = (s + 1) % g_path_n;
        int x0 = g_path[i].x, y0 = g_path[i].y;
        int x1 = x0 + g_path[i].odx, y1 = y0 + g_path[i].ody;
        int x3 = g_path[j].x, y3 = g_path[j].y;
        int x2 = x3 + g_path[j].idx, y2 = y3 + g_path[j].idy;
        for (int t = 1; t <= PATH_STEPS; t++) {
            int px, py;
            bez(t * 256 / PATH_STEPS, x0, y0, x1, y1, x2, y2, x3, y3, &px, &py);
            xs[n] = px; ys[n] = py; n++;
        }
    }
    *oxs = xs; *oys = ys;
    return n;
}

void path_stroke(int width, uint32_t color) {
    int *xs = NULL, *ys = NULL;
    int n = path_flatten(&xs, &ys);
    if (n < 2) { free(xs); free(ys); return; }
    undo_push("Stroke Path");
    if (width < 1) width = 1;
    for (int i = 1; i < n; i++)
        stamp_line(xs[i - 1], ys[i - 1], xs[i], ys[i], color, width, 255,
                   g_tool.hardness, ST_PAINT, 1);
    free(xs); free(ys);
}

void path_to_selection(int feather) {
    int *xs = NULL, *ys = NULL;
    int n = path_flatten(&xs, &ys);
    if (n < 3) { free(xs); free(ys); return; }
    int w = g_doc.w, h = g_doc.h;
    if (!g_doc.sel) {
        g_doc.sel = (uint8_t *)calloc((size_t)w * (size_t)h, 1);
        if (!g_doc.sel) { free(xs); free(ys); return; }
    } else {
        memset(g_doc.sel, 0, (size_t)w * (size_t)h);
    }
    int *cross = (int *)malloc((size_t)n * sizeof(int));
    if (!cross) { free(xs); free(ys); return; }
    for (int y = 0; y < h; y++) {
        int nc = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            int y0 = ys[i], y1 = ys[j];
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                int x0 = xs[i], x1 = xs[j];
                cross[nc++] = x0 + (int)((int64_t)(y - y0) * (x1 - x0) / (y1 - y0));
            }
        }
        for (int a = 1; a < nc; a++) {          // insertion sort crossings
            int v = cross[a], b = a - 1;
            while (b >= 0 && cross[b] > v) { cross[b + 1] = cross[b]; b--; }
            cross[b + 1] = v;
        }
        for (int a = 0; a + 1 < nc; a += 2) {
            int xl = clampi(cross[a], 0, w - 1), xr = clampi(cross[a + 1], 0, w - 1);
            for (int x = xl; x <= xr; x++) g_doc.sel[(size_t)y * w + x] = 255;
        }
    }
    free(cross);
    free(xs); free(ys);
    g_doc.sel_active = 1;
    if (feather > 0) sel_feather(feather);
}
