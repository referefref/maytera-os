// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// oruler.h - the Writer ruler (docs/OFFICE_UI_DESIGN.md 2.1 / 3.7 / 8.7).
//
// 20 px strip on surface_raised with a 1 px border_subtle rule at the bottom;
// a surface_sunken band spanning the page's printable width (left margin to
// right margin, in window x); minor ticks every 1/8 in (12 px at 96 ppi,
// 3 px tall), half-inch ticks 5 px, inch ticks 7 px with the inch numeral
// (caption 11, muted floored) centred on the tick, measured from the left
// margin; left/right indent markers as two gui_chevron(GUI_CHEV_DOWN) accent
// marks at the margins (drag is v2: draw only). Pure draw, no state: it reads
// the page geometry from the app each frame.
#ifndef OFFICE_ORULER_H
#define OFFICE_ORULER_H

#define ORULER_H 20

void oruler_draw(int win, int x, int y, int w,               // strip rect, h = ORULER_H
                 int page_x, int page_w,                     // page sheet in window coords
                 int margin_l_px, int margin_r_px, int ppi); // ppi 96 = 12 px per 1/8 in

#endif // OFFICE_ORULER_H
