// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// othumbs.h - the Slides thumbnail strip (docs/OFFICE_UI_DESIGN.md 2.3 / 3.6 / 8.6).
//
// Replaces the shipped 130 x 64 strip (whose 2.4:1 cells cannot show a 16:9
// slide undistorted): a left column 168 wide (8 + 152 + 8), surface_raised
// ground with a 1 px border_subtle right edge. Each cell is 104 px tall:
// caption numeral "N" at (8, +2) muted floored, thumbnail 144 x 81 (16:9) at
// (12, +17) on surface_sunken paper with a 1 px border_strong outline, the
// slide content drawn by the caller's render callback. Selected: 2 px accent
// ring at 2 px outside the thumbnail; hovered: 1 px border_strong ring at the
// same offset. Built on gui_list_t (row_h 104): scrolling, wheel and keys come
// from the shared widget; this file only draws the cell content.
#ifndef OFFICE_OTHUMBS_H
#define OFFICE_OTHUMBS_H

#include "../../libc/gui.h"
#include "../../libc/gui_list.h"

#define OTHUMB_STRIP_W 168
#define OTHUMB_ROW_H   104
#define OTHUMB_W       144
#define OTHUMB_H       81
#define OTHUMB_X       12
#define OTHUMB_Y       17

typedef void (*othumb_render_fn)(int win, int index, int x, int y, int w, int h, void *ctx);

typedef struct {
    gui_list_t list;          // row_h 104, outer box = strip rect
    int x, y, w, h;           // w = 168
    int count, sel, hot;
} othumbs_t;

void othumbs_config(othumbs_t *s, int x, int y, int w, int h, int count, int sel);
void othumbs_draw(int win, othumbs_t *s, othumb_render_fn render, void *ctx);
int  othumbs_press(othumbs_t *s, int mx, int my);   // >=0 selected index, -1 miss, -2 consumed
int  othumbs_motion(othumbs_t *s, int mx, int my);  // 1 = redraw
void othumbs_release(othumbs_t *s);
int  othumbs_wheel(othumbs_t *s, int mx, int my, int delta);
int  othumbs_key(othumbs_t *s, unsigned int keycode);   // moves sel; returns 1 if changed
void othumbs_reveal(othumbs_t *s);                  // scroll the selection into view
// Thumbnail rect of row `index` for THIS frame (1 if at least partially visible).
int  othumbs_rect(const othumbs_t *s, int index, int *x, int *y, int *w, int *h);

#endif // OFFICE_OTHUMBS_H
