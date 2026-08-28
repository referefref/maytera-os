// term_theme.h
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.
#ifndef TERM_THEME_H
#define TERM_THEME_H

#include "term_common.h"


// ANSI color indices
#define COLOR_INDEX_BLACK   0
#define COLOR_INDEX_RED     1
#define COLOR_INDEX_GREEN   2
#define COLOR_INDEX_YELLOW  3
#define COLOR_INDEX_BLUE    4
#define COLOR_INDEX_MAGENTA 5
#define COLOR_INDEX_CYAN    6
#define COLOR_INDEX_WHITE   7

// ---- terminal COLOUR SCHEME + window theme + font (term_theme.c) ---------
// TWO DIFFERENT THINGS, and they must stay that way (owner correction, tier 2
// of docs/TERMINAL_PARITY.md):
//   g_term_palette_slug  = the TERMINAL COLOUR SCHEME. It governs the CELL
//                          GRID: the 16 ANSI colours plus the default
//                          fg/bg/cursor. Data-driven from /PALETTES/*.tpalette.
//   g_term_theme_slug    = the WINDOW THEME. It governs the CHROME (and, only
//                          when the colour scheme is "system", the default
//                          fg/bg/cursor too).
// Never collapse one into the other.
extern uint32_t ansi_colors[16];

extern char g_term_theme_slug[GUI_THEME_SLUG_MAX];
extern int  g_term_theme_index;
extern gui_font_sel_t g_term_font;

// ---- CURSOR SHAPE + BLINK (profiles) -------------------------------------
// Part of the COLOUR SCHEME's surface in every other terminal, but a shape is
// not a colour, so it lives here beside term_cursor_color() rather than in
// gui_palette.h's on-disk .tpalette format (a scheme file describes colours
// only, and adding a shape key would make every scheme carry a field that has
// nothing to do with colour). The PROFILE owns the value; these two globals
// are just the live copy the renderer reads.
#define TERM_CURSOR_BLOCK      0   // solid full-cell block
#define TERM_CURSOR_UNDERLINE  1   // 2px bar on the baseline (the historical shape)
#define TERM_CURSOR_BAR        2   // 2px vertical bar at the cell's left edge
#define TERM_CURSOR_SHAPE_COUNT 3
extern int g_term_cursor_shape;
// 0 pins the cursor solid. main.c's idle branch keeps toggling cursor_visible
// either way (it is the same counter that drives the ~500ms cadence); the
// RENDERER is what honours this, so switching blink off needs no change to the
// event loop and cannot desync from it. See term_render.c.
extern int g_term_cursor_blink;

extern char g_term_palette_slug[GUI_PALETTE_SLUG_MAX];
extern term_palette_t g_term_palette;
extern int  g_term_palette_is_system;

void term_resolve_theme(void);
void term_resolve_palette(void);
uint32_t term_bg_color(void);
uint32_t term_fg_color(void);
uint32_t term_cursor_color(void);
void term_apply_font(void);

#endif // TERM_THEME_H
