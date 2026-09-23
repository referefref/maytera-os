// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// See docs/OFFICE_SUITE_ARCHITECTURE.md for the module map and contracts.
// NOTE: officelib headers are SELF-SUFFICIENT (raw C types only). Do NOT add
//       #include "types.h" or any libc header here: libc's bool/off_t/pid_t
//       leak into apps carrying legacy compat typedefs and break the userland.
#ifndef OFFICE_LAYOUT_H
#define OFFICE_LAYOUT_H
// Text layout / pagination for the document view. Wraps runs to a pixel width
// using ttf_measure() and produces positioned spans a window can draw.
// Owner: agent 9 (layout/textlayout.c). Used by apps/writer + print/export.
#include "docmodel.h"

typedef struct laid_span {
    int             x, y, w, h;  // window-relative px
    const char     *text;        // borrowed (points into the document)
    int             len;
    doc_runfmt      fmt;
} laid_span;

// ADDITIVE (agent 9, officewriter, no-ticket): a plain border/gridline rect
// (window-relative px, 1px stroke drawn by the caller of layout_draw's own
// win_draw_rect, not part of any text span). layout_document() emits one of
// these per table cell so a "simple table" reads as an actual grid instead of
// four floating paragraphs. This is a genuinely NEW field on laid_out, not a
// change to any existing field or to layout_document()/layout_free()/
// layout_draw()'s signatures - see textlayout.c's top-of-file comment and the
// implementing agent's final report for the full rationale. Safe: nothing
// else in the tree reads laid_out's fields yet (grep-verified at the time of
// this change), so there is no other consumer to break.
typedef struct laid_rect {
    int x, y, w, h;
} laid_rect;

typedef struct laid_out {
    laid_span *spans; int nspan;
    laid_rect *rects; int nrect;  // ADDITIVE: see laid_rect above. May be NULL/0.
    int width, height;           // total content px (height drives scrolling)
    // ADDITIVE (officeintegrate): strings the layout GENERATES (numbered-list
    // labels) that spans point into. layout_free() releases them. Every other
    // span text is borrowed from the document, as documented on laid_span.
    char **owned; int nowned;
} laid_out;

laid_out *layout_document(const document *d, int width_px);
void      layout_free(laid_out *lo);
// Draw the laid_out into window `win` offset by scroll (px). Clips to [0,vh).
void      layout_draw(int win, const laid_out *lo, int scroll_x, int scroll_y, int vh);
#endif
