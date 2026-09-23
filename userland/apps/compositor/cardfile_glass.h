// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// cardfile_glass.h - the Cardfile deck's GLASS chrome passes (agent D of
// docs/CARDFILE_ARCHITECTURE.md). On the modern themes (g_glass_enable) the
// deck chrome is layered frosted glass: each pane is the card's colour at
// the theme's glass alpha over a blurred, saturated backdrop of whatever is
// behind it, with a 1px luminous border, an inset top-edge (or left-edge)
// highlight, a white sheen fading from that edge, and a per-step depth dim
// so the fanned deck reads with depth. Ported from the approved design at
// the build host:/root/cardfile-mockup.html ("Glass approach" + the four glass tokens
// per theme in its Design notes drawer). The app content INSIDE a card body
// or a group pane is never touched: every function here paints rings,
// strips and plates only.
//
// THE N-SURFACE PROBLEM AND ITS SOLUTION. draw.c's glass_render() caches
// one tinted strip per fixed slot (GLASS_SURF_COUNT = 4), which a fanned
// deck (rail + N edges + N plates + frames + panes + popup, each with its
// own tint and dim) would thrash every frame. This module instead uses the
// shared backdrop plane added to draw.c for exactly this shape
// (glass_backdrop_prepare()/glass_backdrop_fill(), compositor.h): ONE
// whole-screen blur per frame (signature-skipped, rate-limited, tiered like
// glass_render), then any number of per-pane tints from it at blit time. No
// pane owns a cache slot, so the surface count is unbounded and nothing
// thrashes. cfg_begin_frame() is the once-per-frame prepare; every other
// function here is a fill from that plane plus its lines.
//
// RETRO stays flat: every caller in cardfile.c gates on g_glass_enable and
// keeps its bevelled/flat path when it is 0; nothing here is reached then.
//
// Never blocks (#426): pure arithmetic and bounded draw calls. Honours the
// active clip through the draw.c primitives it calls.

#ifndef COMPOSITOR_CARDFILE_GLASS_H
#define COMPOSITOR_CARDFILE_GLASS_H

#include <stdint.h>
#include "cardfile_model.h"    // cf_rect_t (self-sufficient header)

// Once per cardfile_render(), BEFORE any chrome is drawn: builds or reuses
// the shared backdrop plane (draw.c). No-op when glass is off.
void cfg_begin_frame(void);

// 1 when the active modern theme is the dark flavour (the design's tokens
// and card palette come in light/dark pairs; retro is neither and never asks).
int cfg_is_dark(void);

// The theme surface colour the rail, its "+"/Sort tabs and the popups are
// tinted with, so their ink is computed against the colour actually painted.
uint32_t cfg_surface_color(void);

// Which side of deck position `idx` the nearest open slot lies on, from a
// cf_layout() open-index list: +1 = to the right (idx is a LEFT spine),
// -1 = to the left (a RIGHT spine), 0 = idx is itself open or nothing is.
int cfg_edge_side(const int *open_idx, int n_open, int idx);

// The rail: full-height strip at the far left, surface tint at .42, sheen
// left-to-right, luminous right border.
void cfg_rail(int32_t x, int32_t y, int32_t w, int32_t h);

// One slot's stowed edge (spine): the card's EDGE colour (card darkened 14%,
// lightened 14% on dark) at the glass alpha, dimmed dim_x10 tenths of a
// percent (cf_depth_dim_x10()), left border + inset highlight, sheen
// left-to-right over 60% of the width, and the soft shadow its nearer
// neighbour casts onto it (spines further from the open card step back).
void cfg_edge(cf_rect_t r, uint32_t card, int dim_x10, int side);

// A sideways tab plate (stowed or open) on a card colour: 7px-rounded glass
// pane, top highlight, sheen over the top 55%, drop shadow below (deeper
// when open), dimmed like its edge when stowed. Used for the rail's "+"/Sort
// tabs (via cardfile.c's cf_plate(), which is not card-attached and keeps
// this uniform shape) - a CARD tab (stowed/open, and the maximize view's
// summary tabs) uses cfg_tab() below instead.
void cfg_plate(cf_rect_t r, uint32_t card, int is_open, int dim_x10);

// (cfmaxwidth; corrected cfdock/scallop-fix) THE SEAMLESSLY-JOINED CARD TAB.
// Every tab in the deck joins its card on the LEFT (the rail/spine is always
// to the left; tabs fan out rightward), so this is NOT cfg_plate() with a
// bigger radius: the two LEFT (join) corners get a small convex BULGE that
// grows OUT of the tab into the space beside it (not a bigger round applied
// to the tab's own corner - that eroded the tab's material AWAY from the
// join and read as a concave scallop, the bug reported and fixed in the
// cfdock scallop-fix), and no border stroke or drop shadow is drawn on that
// side at all (so nothing crosses the flare). The two RIGHT corners (the
// free end) get cfg_plate()'s ordinary 7px round, full border and shadow.
// See cardfile.c's cf_draw_card_tab_flat()/cf_draw_join_bulge() for the
// identical treatment in flat/retro mode (same draw_round_corners_capture()/
// restore() primitive, no glass involved).
void cfg_tab(cf_rect_t r, uint32_t card, int is_open, int dim_x10);

// One of the four small buttons on an open plate: a translucent white pill
// over the plate's glass (brighter on hover), bordered and highlighted.
void cfg_tab_button(cf_rect_t r, int hover);

// The open card's frame: the top/left/right bands (frame_px) and the caption
// foot (foot_px) around the hosted window, glass on the card colour with a
// sheen over the top 30% of the card, border on three sides (the edge is
// flush on the left), top highlight, an inner hairline where the frame
// meets the app content, and a focus ring when is_focus. Never paints the
// interior.
void cfg_card_frame(cf_rect_t body, uint32_t card, int frame_px, int foot_px, int is_focus);

// One group pane's frame: the header strip (header_px) plus a 4px ring
// around the hosted window, card colour at .62, sheen over the top 40%,
// border, highlight, inner hairline, focus ring. Never paints the interior.
void cfg_pane(cf_rect_t pane, uint32_t card, int header_px, int is_focus);

// A popup panel (the "+" picker, Sort, colour swatches): 12px-rounded glass
// on the theme overlay colour at .74, sheen over the top 40%, border,
// highlight, deep drop shadow. Contents are drawn by the caller afterward.
void cfg_popup(int32_t x, int32_t y, int32_t w, int32_t h);

// (cfdock) A dock bar's glass surface: the SAME rail-style translucent strip
// cfg_rail() draws (backdrop tint at the rail alpha, sheen along the bar's
// long axis) but for a bar at ANY of the four screen edges - cfg_rail() is
// always the fixed LEFT rail, this is not. `dock_edge` is the CF_DOCK_EDGE_*
// (cardfile_model.h) the bar itself sits at; the luminous border is drawn
// automatically on the OPPOSITE (inner, desktop-facing) side, matching
// cfg_rail()'s own right border on its always-left rail (a BOTTOM dock's
// inner side is its TOP edge, and so on). A between-cards dock has no single
// screen edge of its own - pass CF_DOCK_EDGE_LEFT by convention (border on
// its right/inner side, the same look as a normal LEFT edge dock).
void cfg_dock_bar(cf_rect_t r, int dock_edge);

#endif // COMPOSITOR_CARDFILE_GLASS_H
