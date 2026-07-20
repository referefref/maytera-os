// brushes.c - Maytera Studio brush/pattern/gradient-shape engine (GIMP P3).
// Owner: Agent (brush engine). Contract: brushes.h (see the CONTRACT-DEVIATION
// note there; folds into studio.h later). This is the ONLY TU that includes the
// generated data headers, so the large stock tables have a single home and no
// duplicate symbols. Integer math only; no libm.
//
// The stock brush masks (grayscale 0..255) and pattern RGB tables come from GIMP
// 2.10 (GPL). brush_current()==-1 keeps the existing parametric round brush; a
// non-negative index selects a bitmap brush whose mask tools.c scales to the dab
// size. pattern_current()==-1 keeps solid-FG fills.
#include "studio.h"
#include "brushes_data.h"      // defines studio_brush_t, STOCK_BRUSHES, STOCK_BRUSH_COUNT
#include "patterns_data.h"     // defines studio_pattern_t, STOCK_PATTERNS, STOCK_PATTERN_COUNT
#include "brushes.h"

// ---------------------------------------------------------------------------
// Selected-tool state (ui wiring sets these; sensible inert defaults)
// ---------------------------------------------------------------------------
static int g_brush_cur   = -1;             // -1 == parametric round brush
static int g_pattern_cur = -1;             // -1 == no pattern (solid FG)
int        g_brush_pattern_fill = 0;       // bucket tool: 0 solid FG, 1 pattern

static int g_grad_shape   = GRAD_LINEAR;
static int g_grad_repeat  = GRAD_REPEAT_NONE;
static int g_grad_reverse = 0;

// ---------------------------------------------------------------------------
// Brush registry
// ---------------------------------------------------------------------------
int brush_count(void) { return STOCK_BRUSH_COUNT; }

const studio_brush_t *brush_get(int i) {
    if (i < 0 || i >= STOCK_BRUSH_COUNT) return NULL;
    return &STOCK_BRUSHES[i];
}

int brush_current(void) { return g_brush_cur; }

void brush_set(int i) {
    if (i < -1) i = -1;
    if (i >= STOCK_BRUSH_COUNT) i = STOCK_BRUSH_COUNT - 1;
    g_brush_cur = i;
}

int brush_sample(int i, int bx, int by) {
    const studio_brush_t *b = brush_get(i);
    if (!b || !b->mask || b->w <= 0 || b->h <= 0) return 0;
    if (bx < 0 || by < 0 || bx >= b->w || by >= b->h) return 0;
    return b->mask[(size_t)by * (size_t)b->w + (size_t)bx];
}

// ---------------------------------------------------------------------------
// Pattern registry
// ---------------------------------------------------------------------------
int pattern_count(void) { return STOCK_PATTERN_COUNT; }

const studio_pattern_t *pattern_get(int i) {
    if (i < 0 || i >= STOCK_PATTERN_COUNT) return NULL;
    return &STOCK_PATTERNS[i];
}

int pattern_current(void) { return g_pattern_cur; }

void pattern_set(int i) {
    if (i < -1) i = -1;
    if (i >= STOCK_PATTERN_COUNT) i = STOCK_PATTERN_COUNT - 1;
    g_pattern_cur = i;
}

uint32_t pattern_px(int i, int x, int y) {
    const studio_pattern_t *p = pattern_get(i);
    if (!p || !p->rgb || p->w <= 0 || p->h <= 0) return 0xFF000000u;
    int tx = x % p->w; if (tx < 0) tx += p->w;    // tile, handle negatives
    int ty = y % p->h; if (ty < 0) ty += p->h;
    const unsigned char *s = &p->rgb[((size_t)ty * (size_t)p->w + (size_t)tx) * 3];
    return argb(255, s[0], s[1], s[2]);
}

// ---------------------------------------------------------------------------
// Integer helpers for the gradient shapes
// ---------------------------------------------------------------------------
static uint32_t isqrt64(uint64_t v) {
    uint64_t r = 0, bit = 1ULL << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

// Integer atan2 mapped to a full turn of 65536 units (65536 == 360 degrees).
// Octant/rational approximation, accurate to ~1 degree, which is plenty for a
// conical/spiral gradient. Screen coordinates (y grows downward), so increasing
// angle runs clockwise on screen.
static int atan2_65536(int y, int x) {
    if (x == 0 && y == 0) return 0;
    int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    int a;                                          // reference angle 0..16384 (0..90deg)
    if (ax >= ay) {
        a = ax ? (int)((int64_t)ay * 8192 / ax) : 0;             // 0..8192  (0..45deg)
    } else {
        a = ay ? 16384 - (int)((int64_t)ax * 8192 / ay) : 0;     // 8192..16384 (45..90deg)
    }
    if (x >= 0 && y >= 0) return a;                 // Q1: 0..16384
    if (x <  0 && y >= 0) return 32768 - a;         // Q2: 16384..32768
    if (x <  0 && y <  0) return 32768 + a;         // Q3: 32768..49152
    return 65536 - a;                               // Q4: 49152..65536
}

static int grad_linear_t(int px, int py, int vx, int vy, int64_t len2) {
    int64_t proj = (int64_t)px * vx + (int64_t)py * vy;
    int64_t t = proj * 65536 / len2;
    if (t < 0) t = 0;
    if (t > 65536) t = 65536;
    return (int)t;
}

// ---------------------------------------------------------------------------
// Gradient shape / repeat state
// ---------------------------------------------------------------------------
int  grad_shape_get(void) { return g_grad_shape; }
void grad_shape_set(int s) {
    if (s < 0) s = 0;
    if (s >= GRAD_SHAPE_COUNT) s = GRAD_SHAPE_COUNT - 1;
    g_grad_shape = s;
}

int  grad_repeat_get(void) { return g_grad_repeat; }
void grad_repeat_set(int r) {
    if (r < 0) r = 0;
    if (r >= GRAD_REPEAT_COUNT) r = GRAD_REPEAT_COUNT - 1;
    g_grad_repeat = r;
}

int  grad_reverse_get(void) { return g_grad_reverse; }
void grad_reverse_set(int on) { g_grad_reverse = on ? 1 : 0; }

// Gradient position at (x,y) for the drag axis (x0,y0)-(x1,y1), 0..65536.
int grad_shape_t01(int shape, int x, int y, int x0, int y0, int x1, int y1) {
    int vx = x1 - x0, vy = y1 - y0;
    int px = x - x0, py = y - y0;
    int64_t len2 = (int64_t)vx * vx + (int64_t)vy * vy;
    if (len2 <= 0) return 0;

    switch (shape) {
    case GRAD_LINEAR:
        return grad_linear_t(px, py, vx, vy, len2);

    case GRAD_BILINEAR: {
        // Symmetric ramp: 0 at the start, rising to 1 at both ends of the axis.
        int64_t proj = (int64_t)px * vx + (int64_t)py * vy;
        if (proj < 0) proj = -proj;
        int64_t t = proj * 65536 / len2;
        if (t > 65536) t = 65536;
        return (int)t;
    }

    case GRAD_RADIAL: {
        uint32_t r = isqrt64((uint64_t)((int64_t)px * px + (int64_t)py * py));
        uint32_t L = isqrt64((uint64_t)len2);
        if (L == 0) return 65536;
        int64_t t = (int64_t)r * 65536 / L;
        if (t > 65536) t = 65536;
        return (int)t;
    }

    case GRAD_SQUARE: {
        // Chebyshev (max-norm) distance from the start, normalised by the axis.
        int apx = px < 0 ? -px : px, apy = py < 0 ? -py : py;
        int m = apx > apy ? apx : apy;
        uint32_t L = isqrt64((uint64_t)len2);
        if (L == 0) return 65536;
        int64_t t = (int64_t)m * 65536 / L;
        if (t > 65536) t = 65536;
        return (int)t;
    }

    case GRAD_CONICAL: {
        // Symmetric conical: angle between P and the axis, folded 0..180 -> 0..1.
        int pa = atan2_65536(py, px);
        int va = atan2_65536(vy, vx);
        int d = (pa - va) % 65536;
        if (d < 0) d += 65536;                       // 0..65536
        if (d > 32768) d = 65536 - d;                // fold to 0..32768
        return d * 2;                                // 0..65536
    }

    case GRAD_SPIRAL: {
        // Clockwise spiral: angle around the axis plus the radial fraction, wrapped.
        int pa = atan2_65536(py, px);
        int va = atan2_65536(vy, vx);
        int d = (pa - va) % 65536;
        if (d < 0) d += 65536;
        uint32_t r = isqrt64((uint64_t)((int64_t)px * px + (int64_t)py * py));
        uint32_t L = isqrt64((uint64_t)len2);
        int64_t rad = L ? (int64_t)r * 65536 / L : 0;
        int64_t t = ((int64_t)d + rad) % 65536;
        return (int)t;
    }

    default:
        return grad_linear_t(px, py, vx, vy, len2);
    }
}

// Map a raw gradient position through the current repeat mode, then reverse.
int grad_apply_repeat(int t) {
    if (t < 0) t = 0;
    if (t > 65536) t = 65536;
    switch (g_grad_repeat) {
    case GRAD_REPEAT_SAW:
        t = (t * 3) & 65535;                         // 3 cycles, sawtooth wrap
        break;
    case GRAD_REPEAT_TRI: {
        int q = (t * 3) % 131072;                    // 0..131071 over 2 half-cycles
        t = q < 65536 ? q : 131071 - q;              // reflect
        break;
    }
    default:
        break;
    }
    if (g_grad_reverse) t = 65536 - t;
    if (t < 0) t = 0;
    if (t > 65536) t = 65536;
    return t;
}
