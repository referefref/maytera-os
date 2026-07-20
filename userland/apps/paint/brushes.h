// CONTRACT-DEVIATION: brush/pattern engine API lives here instead of studio.h
// because studio.h is concurrently owned; fold into studio.h later.
//
// brushes.c is the ONLY translation unit that includes brushes_data.h and
// patterns_data.h (they contain definitions). tools.c and render.c include just
// this header and reach the stock assets through the accessor functions below.
#ifndef BRUSHES_H
#define BRUSHES_H

#include "studio.h"

// The stock-asset structs are defined by brushes_data.h / patterns_data.h. When
// a client TU (tools.c/render.c) includes only this header, those data headers
// are absent, so mirror the layouts here. brushes.c includes the data headers
// FIRST, which set these guards, so the typedefs below are skipped there and no
// duplicate/incompatible typedef is produced.
#ifndef BRUSHES_DATA_H
typedef struct { const char *name; int w, h, spacing; const unsigned char *mask; } studio_brush_t;
#endif
#ifndef PATTERNS_DATA_H
typedef struct { const char *name; int w, h; const unsigned char *rgb; } studio_pattern_t;
#endif

// ---------------------------------------------------------------------------
// Brush registry. Index -1 (the default) means the parametric round brush that
// stamp()/stamp_cov() already implement; indices 0..brush_count()-1 select a
// stock bitmap brush.
// ---------------------------------------------------------------------------
int                   brush_count(void);
const studio_brush_t *brush_get(int i);            // NULL if out of range
int                   brush_current(void);         // -1 == parametric round
void                  brush_set(int i);            // clamps to [-1, count-1]
int                   brush_sample(int i, int bx, int by);  // 0..255 mask value

// ---------------------------------------------------------------------------
// Pattern registry. Index -1 means "no pattern" (solid FG). pattern_px tiles
// the stock pattern at canvas coordinates and returns opaque ARGB.
// ---------------------------------------------------------------------------
int                     pattern_count(void);
const studio_pattern_t *pattern_get(int i);        // NULL if out of range
int                     pattern_current(void);     // -1 == none
void                    pattern_set(int i);        // clamps to [-1, count-1]
uint32_t                pattern_px(int i, int x, int y);   // tiled opaque ARGB

// Bucket-fill pattern switch. ui wiring sets it; default 0 keeps solid FG fill.
extern int g_brush_pattern_fill;

// ---------------------------------------------------------------------------
// Gradient shapes (P3-GRADSHAPE). grad_shape_t01 returns the gradient position
// at canvas point (x,y) as 0..65536 fixed point, for the given shape and the
// drag endpoints (x0,y0)-(x1,y1).
// ---------------------------------------------------------------------------
enum {
    GRAD_LINEAR = 0, GRAD_BILINEAR, GRAD_RADIAL, GRAD_SQUARE,
    GRAD_CONICAL, GRAD_SPIRAL, GRAD_SHAPE_COUNT
};
int  grad_shape_get(void);
void grad_shape_set(int s);                        // clamps to a valid shape
int  grad_shape_t01(int shape, int x, int y, int x0, int y0, int x1, int y1);

// ---------------------------------------------------------------------------
// Repeat modes (P3-GRADREPEAT) + reverse. grad_apply_repeat maps a raw t
// (0..65536) through the current repeat mode and reverse flag.
// ---------------------------------------------------------------------------
enum { GRAD_REPEAT_NONE = 0, GRAD_REPEAT_SAW, GRAD_REPEAT_TRI, GRAD_REPEAT_COUNT };
int  grad_repeat_get(void);
void grad_repeat_set(int r);                       // clamps to a valid mode
int  grad_reverse_get(void);
void grad_reverse_set(int on);                     // 0/1
int  grad_apply_repeat(int t);                     // 0..65536 -> 0..65536

#endif // BRUSHES_H
