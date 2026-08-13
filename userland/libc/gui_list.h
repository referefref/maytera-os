// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_list.h - MayteraOS shared scrollable listbox primitive (#512).
//
// WHY THIS EXISTS:
// gui_scroll.c (#291/#261/#438) gives every app the offset/thumb/wheel/key
// math for a scrollable viewport, but nothing owns the ROW layer on top of
// it: which row is at screen y, whether a row is even partially visible
// (the exact question a caller must get right to avoid drawing content
// outside its box), and hit-testing a click to a row index. Without that,
// every list-of-rows widget in the tree re-derives it by hand, and gets it
// wrong in the same way:
//   * Settings' dropdown (main.c) capped visible rows at a fixed constant
//     (DD_VISIBLE, originally 9) with no scroll at all for the remainder -
//     a screensaver list that grew from 9 to 19 entries silently hid the
//     last ten. No error, no visible cue.
//   * The shared font dialog (#351/#533) built THREE near-identical
//     scroll-plus-row-clip loops directly on gui_scroll, and the size list
//     still overflowed its box before the clip guard was added by hand.
//   * File dialogs and Paint hand-roll their own row lists and frequently
//     forget the wheel entirely.
// gui_list_t is the one place this gets solved: it wraps a gui_scroll_t with
// row geometry (row_h * count content), gives back a SAFE "is this row even
// partially visible" answer (gui_list_row_y), and does the common flat
// string-list draw + hit-test + input wiring in one call each. A list here
// can never silently truncate: if it does not fit, gui_scroll's real
// scrollbar (thumb, wheel, keys, drag) is what shows, not a fixed cutoff.
//
// gui_menu.h (the menu-bar/dropdown-menu primitive) is built ON TOP of this:
// its popup uses gui_list's geometry accessors for its own richer per-row
// content (label + shortcut + disabled state), the same way this file's
// gui_list_draw() does for the plain-string case. That is the intended reuse
// pattern for any future list-shaped widget: use the geometry accessors
// directly when your rows need more than one string.
//
// BOX MODEL: x/y/w/h given to gui_list_config() is the OUTER box INCLUDING a
// 1px frame. gui_list_draw() draws that frame; the scrollable viewport is
// inset by 1px on every side (matches gui_font.c's list_frame() convention,
// so this widget looks identical to what the font dialog already ships).
#ifndef _GUI_LIST_H
#define _GUI_LIST_H

#include "types.h"
#include "gui_scroll.h"

typedef struct {
    gui_scroll_t scroll;   // offset / thumb / wheel / key / drag state
    int x, y, w, h;        // OUTER box rect (includes the 1px frame)
    int row_h;
    int count;
} gui_list_t;

// (Re)configure. Safe to call every draw (re-clamps like gui_scroll_config).
// Rows always snap (a list is always fixed-height rows here).
void gui_list_config(gui_list_t *l, int x, int y, int w, int h, int row_h, int count);

// --- Row geometry ------------------------------------------------------
// Index of the first row that might be visible, and how many rows to
// iterate (the last is possibly clipped; ALWAYS gate drawing with
// gui_list_row_y, do not just trust the span).
int gui_list_first(const gui_list_t *l);
int gui_list_span(const gui_list_t *l);

// Screen y of row `index` for THIS frame. Returns 1 and sets *out_y if the
// row is at least partially inside the box; returns 0 (out_y untouched) if
// it is fully outside. This is the generalized #533 fix: a caller that skips
// drawing when this returns 0 can never render a row outside its list box,
// no matter how long the list or how it was scrolled.
int gui_list_row_y(const gui_list_t *l, int index, int *out_y);

// Row content width: box width minus the 1px frame, minus the scrollbar
// gutter IF one is currently showing (gui_scroll_needed()).
int gui_list_row_w(const gui_list_t *l);

// Hit test: row index under window-local (mx,my), or -1 if the point is
// outside the row content area (including: inside the scrollbar gutter, or
// past the last row, or outside the box). Does not consume scrollbar
// clicks; call gui_list_press (which tries the scrollbar first) instead of
// this directly for pointer-down handling.
int gui_list_row_at(const gui_list_t *l, int mx, int my);

// Hit test: 1 if window-local (mx,my) is anywhere inside the list's FULL
// outer box (the 1px frame, every row whether populated or not, AND the
// scrollbar gutter) - i.e. "is the pointer over this list at all". This is
// deliberately WIDER than gui_list_row_at(), which excludes the gutter and
// anything past the last real row on purpose (a click there should not pick
// a row). Wheel routing wants the wide answer: a caller that gates on
// row_at() instead (or hand-rolls its own smaller rect) only accepts the
// wheel over populated rows, which reads as "scrolling only works in a
// narrow sub-region" - the exact bug gui_list_wheel() closes by owning this
// check itself instead of leaving every caller to reinvent it.
int gui_list_hit(const gui_list_t *l, int mx, int my);

// --- Drawing (the common flat-string-list case) -------------------------
// Returns the label for `index` into `buf` (or a static string), used only
// for this call's frame.
typedef const char *(*gui_list_label_fn)(void *ctx, int index, char *buf, int cap);

// Draws the frame, every visible row (selection background + label), and the
// scrollbar. Nothing left for the caller to hand-roll for a plain string
// list. Rows needing richer content (icons, shortcuts, per-row font) should
// use the geometry accessors above directly instead, the way gui_menu.c and
// gui_font.c's family/style/size lists do.
void gui_list_draw(int win, gui_list_t *l, int sel, uint32_t bg, uint32_t border,
                   uint32_t text, uint32_t sel_bg, uint32_t sel_text,
                   gui_list_label_fn label_of, void *ctx);

// --- Input ---------------------------------------------------------------
// Wheel / keyboard scrolling (Up/Down/PgUp/PgDn/Home/End move the VIEWPORT,
// not any selection - see gui_list_move_sel for selection movement).
//
// gui_list_wheel takes the pointer position and gates on gui_list_hit()
// internally, so "does the wheel apply to this list" is answered once, here,
// the same way for every caller - not re-derived (and easy to get too
// narrow) at each call site. Pass the window-local mouse_x/mouse_y straight
// from the event; returns 0 (no-op, never a crash) both when the point is
// outside the box and when a short list has nothing left to scroll.
int  gui_list_wheel(gui_list_t *l, int mx, int my, int scroll_delta);
int  gui_list_key(gui_list_t *l, uint32_t keycode);

// Pointer press: tries the scrollbar first (drag/page), else returns the
// row index hit (same as gui_list_row_at), else -1. Callers should treat
// >=0 as "select this row" and any other return as "consumed, no row".
// Use gui_list_press_consumed() to tell a scrollbar-only click apart from a
// miss when that distinction matters (both return -1 from row_at).
int  gui_list_press(gui_list_t *l, int mx, int my);
int  gui_list_motion(gui_list_t *l, int mx, int my);   // 1 if redraw needed
void gui_list_release(gui_list_t *l);

// Move a caller-owned selection index by `delta` rows, clamp to
// [0, count-1], and scroll it into view. Returns 1 if the selection (not
// necessarily the viewport) changed.
int  gui_list_move_sel(gui_list_t *l, int *sel, int delta);

#endif // _GUI_LIST_H
