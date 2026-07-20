// select.c - Maytera Studio selection masks (rect/ellipse/lasso/wand).
// Owner: Agent 2. Contract: studio.h. Selections REPLACE the previous mask
// (no add/subtract modes in v1). "No selection" means everything selected;
// that case is encoded as g_doc.sel_active == 0 (see sel_at() in studio.h).
#include "studio.h"

// ---------------------------------------------------------------------------
// Mask lifecycle
// ---------------------------------------------------------------------------

// Lazily allocate the w*h mask, zeroed. Returns NULL if no doc or OOM.
static uint8_t *sel_ensure(void) {
    if (g_doc.w <= 0 || g_doc.h <= 0) return NULL;
    if (!g_doc.sel) {
        g_doc.sel = (uint8_t *)malloc((size_t)g_doc.w * (size_t)g_doc.h);
        if (!g_doc.sel) return NULL;
    }
    memset(g_doc.sel, 0, (size_t)g_doc.w * (size_t)g_doc.h);
    return g_doc.sel;
}

void sel_clear(void) {
    if (g_doc.sel) { free(g_doc.sel); g_doc.sel = NULL; }
    g_doc.sel_active = 0;
}

void sel_all(void) {
    if (!sel_ensure()) return;
    memset(g_doc.sel, 255, (size_t)g_doc.w * (size_t)g_doc.h);
    g_doc.sel_active = 1;
}

void sel_invert(void) {
    // Inverting "everything selected" selects nothing (an all-zero mask).
    if (!g_doc.sel_active || !g_doc.sel) {
        if (!sel_ensure()) return;      // mask is all zero now
        g_doc.sel_active = 1;
        return;
    }
    size_t n = (size_t)g_doc.w * (size_t)g_doc.h;
    for (size_t i = 0; i < n; i++) g_doc.sel[i] = (uint8_t)(255 - g_doc.sel[i]);
}

// ---------------------------------------------------------------------------
// Integer sqrt (for the ellipse feather ramp)
// ---------------------------------------------------------------------------
static uint32_t isqrt64(uint64_t v) {
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

// ---------------------------------------------------------------------------
// Rectangle (feather = inward linear ramp over `feather` pixels)
// ---------------------------------------------------------------------------
void sel_rect(int x0, int y0, int x1, int y1, int feather) {
    if (!sel_ensure()) return;
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    int rx0 = clampi(x0, 0, g_doc.w - 1), rx1 = clampi(x1, 0, g_doc.w - 1);
    int ry0 = clampi(y0, 0, g_doc.h - 1), ry1 = clampi(y1, 0, g_doc.h - 1);
    if (feather < 0) feather = 0;
    for (int y = ry0; y <= ry1; y++) {
        uint8_t *row = g_doc.sel + (size_t)y * (size_t)g_doc.w;
        int dy = y - ry0, dy2 = ry1 - y;
        int dv = dy < dy2 ? dy : dy2;                 // inside distance (vertical)
        for (int x = rx0; x <= rx1; x++) {
            int dx = x - rx0, dx2 = rx1 - x;
            int d = dx < dx2 ? dx : dx2;              // inside distance (horizontal)
            if (dv < d) d = dv;
            int v = 255;
            if (feather > 0 && d < feather) v = clampi((d + 1) * 255 / feather, 0, 255);
            row[x] = (uint8_t)v;
        }
    }
    g_doc.sel_active = 1;
}

// ---------------------------------------------------------------------------
// Ellipse inscribed in the given box. Feather ramps inward from the edge.
// Normalized radial distance in 1/256 units: 256 == on the ellipse edge.
// ---------------------------------------------------------------------------
void sel_ellipse(int x0, int y0, int x1, int y1, int feather) {
    if (!sel_ensure()) return;
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
    int rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    if (rx < 1) rx = 1;
    if (ry < 1) ry = 1;
    int cx = x0 + rx, cy = y0 + ry;
    int minr = rx < ry ? rx : ry;
    if (feather < 0) feather = 0;
    if (feather > minr) feather = minr;
    // Feather converted to normalized units (out of 256).
    int fn = feather > 0 ? feather * 256 / minr : 0;
    int64_t rx2 = (int64_t)rx * rx, ry2 = (int64_t)ry * ry;
    int bx0 = clampi(cx - rx, 0, g_doc.w - 1), bx1 = clampi(cx + rx, 0, g_doc.w - 1);
    int by0 = clampi(cy - ry, 0, g_doc.h - 1), by1 = clampi(cy + ry, 0, g_doc.h - 1);
    for (int y = by0; y <= by1; y++) {
        uint8_t *row = g_doc.sel + (size_t)y * (size_t)g_doc.w;
        int64_t dy = y - cy;
        for (int x = bx0; x <= bx1; x++) {
            int64_t dx = x - cx;
            int64_t f = dx * dx * ry2 + dy * dy * rx2;      // vs rx2*ry2 at edge
            int64_t edge = rx2 * ry2;
            if (f > edge) continue;                          // outside
            int v = 255;
            if (fn > 0) {
                // nd = 256 * sqrt(f/edge)
                uint64_t nd = isqrt64((uint64_t)f * 65536u / (uint64_t)edge);
                int inner = 256 - fn;
                if ((int)nd > inner) {
                    v = clampi((int)(256 - (int)nd) * 255 / fn, 0, 255);
                }
            }
            row[x] = (uint8_t)v;
        }
    }
    g_doc.sel_active = 1;
}

// ---------------------------------------------------------------------------
// Lasso: collect points during the drag, then scanline even-odd fill the
// closed polygon into a fresh mask.
// ---------------------------------------------------------------------------
static int  *lasso_pts = NULL;      // x,y pairs
static int   lasso_n   = 0;        // number of points
static int   lasso_cap = 0;

static int lasso_push(int x, int y) {
    if (lasso_n >= lasso_cap) {
        int ncap = lasso_cap ? lasso_cap * 2 : 64;
        int *np = (int *)realloc(lasso_pts, (size_t)ncap * 2 * sizeof(int));
        if (!np) return -1;
        lasso_pts = np;
        lasso_cap = ncap;
    }
    lasso_pts[lasso_n * 2] = x;
    lasso_pts[lasso_n * 2 + 1] = y;
    lasso_n++;
    return 0;
}

static void lasso_reset(void) {
    if (lasso_pts) { free(lasso_pts); lasso_pts = NULL; }
    lasso_n = 0;
    lasso_cap = 0;
}

void sel_lasso_begin(int x, int y) {
    lasso_reset();
    lasso_push(x, y);
}

void sel_lasso_point(int x, int y) {
    if (lasso_n > 0 &&
        lasso_pts[(lasso_n - 1) * 2] == x && lasso_pts[(lasso_n - 1) * 2 + 1] == y)
        return;                                   // skip duplicates
    lasso_push(x, y);
}

void sel_lasso_end(void) {
    if (lasso_n < 3) { lasso_reset(); return; }   // not a polygon; no change
    if (!sel_ensure()) { lasso_reset(); return; }
    // Scanline even-odd polygon fill. Intersections per scanline are bounded
    // by the point count; collect, sort, fill pairs.
    int *xs = (int *)malloc((size_t)lasso_n * sizeof(int));
    if (!xs) { lasso_reset(); return; }
    for (int y = 0; y < g_doc.h; y++) {
        int nx = 0;
        for (int i = 0; i < lasso_n; i++) {
            int j = (i + 1) % lasso_n;            // closing edge included
            int ax = lasso_pts[i * 2], ay = lasso_pts[i * 2 + 1];
            int bx = lasso_pts[j * 2], by = lasso_pts[j * 2 + 1];
            if ((ay <= y && by > y) || (by <= y && ay > y)) {
                xs[nx++] = ax + (int)((int64_t)(y - ay) * (bx - ax) / (by - ay));
            }
        }
        // insertion sort (nx is small)
        for (int i = 1; i < nx; i++) {
            int v = xs[i], k = i - 1;
            while (k >= 0 && xs[k] > v) { xs[k + 1] = xs[k]; k--; }
            xs[k + 1] = v;
        }
        uint8_t *row = g_doc.sel + (size_t)y * (size_t)g_doc.w;
        for (int i = 0; i + 1 < nx; i += 2) {
            int fx0 = clampi(xs[i], 0, g_doc.w - 1);
            int fx1 = clampi(xs[i + 1], 0, g_doc.w - 1);
            for (int x = fx0; x <= fx1; x++) row[x] = 255;
        }
    }
    free(xs);
    lasso_reset();
    g_doc.sel_active = 1;
}

// ---------------------------------------------------------------------------
// Magic wand: BFS flood select over the composited image. Tolerance is the
// maximum per-channel (R,G,B) delta from the seed color.
// ---------------------------------------------------------------------------
void sel_wand(int x, int y, int tolerance) {
    if (g_doc.w <= 0 || g_doc.h <= 0) return;
    doc_composite();                               // make sure comp is fresh
    if (!g_doc.comp) return;
    if (x < 0 || y < 0 || x >= g_doc.w || y >= g_doc.h) return;
    if (!sel_ensure()) return;
    tolerance = clampi(tolerance, 0, 255);

    int w = g_doc.w, h = g_doc.h;
    uint32_t seed = g_doc.comp[(size_t)y * (size_t)w + x];
    int sr = px_r(seed), sg = px_g(seed), sb = px_b(seed);

    int *queue = (int *)malloc((size_t)w * (size_t)h * sizeof(int));
    if (!queue) return;
    int qh = 0, qt = 0;
    queue[qt++] = y * w + x;
    g_doc.sel[y * w + x] = 255;                    // mask doubles as visited

    while (qh < qt) {
        int p = queue[qh++];
        int py = p / w, pxx = p % w;
        static const int dx4[4] = { 1, -1, 0, 0 };
        static const int dy4[4] = { 0, 0, 1, -1 };
        for (int k = 0; k < 4; k++) {
            int nx = pxx + dx4[k], ny = py + dy4[k];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            int np = ny * w + nx;
            if (g_doc.sel[np]) continue;
            uint32_t c = g_doc.comp[np];
            int dr = abs(px_r(c) - sr), dg = abs(px_g(c) - sg), db = abs(px_b(c) - sb);
            int d = dr > dg ? dr : dg;
            if (db > d) d = db;
            if (d <= tolerance) {
                g_doc.sel[np] = 255;
                queue[qt++] = np;
            }
        }
    }
    free(queue);
    g_doc.sel_active = 1;
}

// ===========================================================================
// GIMP-parity selection ops (grow/shrink/feather/border/round/by-color)
// ===========================================================================

// Separable min(erode)/max(dilate) of the mask by a square structuring element
// of radius px. Materializes "select all" first if there is no active mask.
static void sel_morph(int px, int dilate) {
    if (px <= 0) return;
    if (!g_doc.sel_active) sel_all();
    if (!g_doc.sel) return;
    int w = g_doc.w, h = g_doc.h;
    uint8_t *tmp = (uint8_t *)malloc((size_t)w * (size_t)h);
    if (!tmp) return;
    for (int y = 0; y < h; y++) {
        uint8_t *row = g_doc.sel + (size_t)y * (size_t)w;
        uint8_t *tr  = tmp + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            int v = dilate ? 0 : 255;
            int a = x - px < 0 ? 0 : x - px, b = x + px >= w ? w - 1 : x + px;
            for (int xx = a; xx <= b; xx++) {
                int s = row[xx];
                if (dilate) { if (s > v) v = s; } else { if (s < v) v = s; }
            }
            tr[x] = (uint8_t)v;
        }
    }
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) {
            int v = dilate ? 0 : 255;
            int a = y - px < 0 ? 0 : y - px, b = y + px >= h ? h - 1 : y + px;
            for (int yy = a; yy <= b; yy++) {
                int s = tmp[(size_t)yy * (size_t)w + x];
                if (dilate) { if (s > v) v = s; } else { if (s < v) v = s; }
            }
            g_doc.sel[(size_t)y * (size_t)w + x] = (uint8_t)v;
        }
    }
    free(tmp);
    g_doc.sel_active = 1;
}

void sel_grow(int px)   { sel_morph(px, 1); }
void sel_shrink(int px) { sel_morph(px, 0); }

// Box-blur the mask by `radius` px (soft-edge feather).
void sel_feather(int radius) {
    if (radius <= 0) return;
    if (!g_doc.sel_active) sel_all();
    if (!g_doc.sel) return;
    int w = g_doc.w, h = g_doc.h, win = radius * 2 + 1;
    uint8_t *tmp = (uint8_t *)malloc((size_t)w * (size_t)h);
    if (!tmp) return;
    for (int y = 0; y < h; y++) {
        uint8_t *row = g_doc.sel + (size_t)y * (size_t)w;
        uint8_t *tr  = tmp + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            int sum = 0;
            for (int k = -radius; k <= radius; k++) {
                int xx = x + k; xx = xx < 0 ? 0 : (xx >= w ? w - 1 : xx);
                sum += row[xx];
            }
            tr[x] = (uint8_t)(sum / win);
        }
    }
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) {
            int sum = 0;
            for (int k = -radius; k <= radius; k++) {
                int yy = y + k; yy = yy < 0 ? 0 : (yy >= h ? h - 1 : yy);
                sum += tmp[(size_t)yy * (size_t)w + x];
            }
            g_doc.sel[(size_t)y * (size_t)w + x] = (uint8_t)(sum / win);
        }
    }
    free(tmp);
    g_doc.sel_active = 1;
}

// Border = grown XOR shrunk (a band around the selection edge).
void sel_border(int px) {
    if (px <= 0 || !g_doc.sel_active || !g_doc.sel) return;
    int w = g_doc.w, h = g_doc.h;
    size_t n = (size_t)w * (size_t)h;
    uint8_t *base  = (uint8_t *)malloc(n);
    uint8_t *grown = (uint8_t *)malloc(n);
    if (!base || !grown) { free(base); free(grown); return; }
    memcpy(base, g_doc.sel, n);
    sel_morph(px, 1); memcpy(grown, g_doc.sel, n);   // grown copy
    memcpy(g_doc.sel, base, n);
    sel_morph(px, 0);                                // sel = shrunk
    for (size_t i = 0; i < n; i++) {
        int b = grown[i] > g_doc.sel[i] ? grown[i] - g_doc.sel[i] : 0;
        g_doc.sel[i] = (uint8_t)b;
    }
    free(base); free(grown);
    g_doc.sel_active = 1;
}

// Round the corners of the current (rectangular) selection to `radius`.
void sel_round(int radius) {
    if (radius <= 0 || !g_doc.sel_active || !g_doc.sel) return;
    int w = g_doc.w, h = g_doc.h;
    int minx = w, miny = h, maxx = -1, maxy = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (g_doc.sel[(size_t)y * (size_t)w + x]) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
    if (maxx < minx) return;
    int r = radius, rw = (maxx - minx + 1) / 2, rh = (maxy - miny + 1) / 2;
    if (r > rw) r = rw;
    if (r > rh) r = rh;
    if (r <= 0) return;
    int r2 = r * r;
    int ccx[4] = { minx + r, maxx - r, minx + r, maxx - r };
    int ccy[4] = { miny + r, miny + r, maxy - r, maxy - r };
    int sgx[4] = { -1, 1, -1, 1 }, sgy[4] = { -1, -1, 1, 1 };
    for (int c = 0; c < 4; c++)
        for (int dy = 0; dy <= r; dy++)
            for (int dx = 0; dx <= r; dx++) {
                if (dx * dx + dy * dy <= r2) continue;      // inside corner circle
                int x = ccx[c] + sgx[c] * dx, y = ccy[c] + sgy[c] * dy;
                if (x >= 0 && y >= 0 && x < w && y < h)
                    g_doc.sel[(size_t)y * (size_t)w + x] = 0;
            }
}

// Whole-image select by color: every pixel within `tolerance` of the clicked
// composite colour (per-channel max delta), NOT a contiguous flood.
void sel_by_color(int x, int y, int tolerance) {
    if (g_doc.w <= 0 || g_doc.h <= 0) return;
    doc_composite();
    if (!g_doc.comp) return;
    if (x < 0 || y < 0 || x >= g_doc.w || y >= g_doc.h) return;
    if (!sel_ensure()) return;
    tolerance = clampi(tolerance, 0, 255);
    uint32_t seed = g_doc.comp[(size_t)y * (size_t)g_doc.w + x];
    int sr = px_r(seed), sg = px_g(seed), sb = px_b(seed);
    size_t n = (size_t)g_doc.w * (size_t)g_doc.h;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = g_doc.comp[i];
        int dr = abs(px_r(c) - sr), dg = abs(px_g(c) - sg), db = abs(px_b(c) - sb);
        int d = dr > dg ? dr : dg; if (db > d) d = db;
        g_doc.sel[i] = (d <= tolerance) ? 255 : 0;
    }
    g_doc.sel_active = 1;
}
