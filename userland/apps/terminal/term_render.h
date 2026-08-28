// term_render.h
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.
#ifndef TERM_RENDER_H
#define TERM_RENDER_H

#include "term_common.h"
#include "term_grid.h"

// The compositor window this terminal draws into. Owned by the renderer;
// main() assigns it once win_create() has returned.
extern int window_handle;

// ---------------------------------------------------------------------------
// (#damage) WHAT ONE CELL RESOLVES TO, AS ACTUALLY PAINTED.
//
// This is the WHOLE input to painting one cell: the codepoint, the colours
// AFTER every rendition rule (reverse, bold-brighten, dim, hidden) and after
// the two render hooks (find-match, then selection), the font style bits, the
// two decorations, the cell width, and whether the cursor sits on it.
//
// It exists because damage tracking has to answer "would this cell paint the
// same pixels as last frame". Answering that from term_cell_t is WRONG: a
// cell's stored colour is a TAGGED value (default / 256-index / RGB) and the
// DEFAULT case resolves through the colour scheme, so two frames with an
// identical term_cell_t paint different pixels after a scheme change - and
// two frames with an identical term_cell_t paint DIFFERENT pixels when a
// selection or a find match arrives over it, because neither of those touches
// the grid at all. The resolved descriptor captures all of it.
//
// resolve_cell() is the ONE function that produces one, and paint_desc() is
// the ONE function that turns one into pixels. The comparison and the drawing
// therefore cannot drift apart: there is no second copy of "what a cell looks
// like" to forget to update when a rendition rule is added. (#578's lesson:
// a partial path that RE-LISTS the layers of the full path silently stops
// repainting whichever layer is added next.)
//
// 16 bytes, no padding, so the compare is four aligned word compares.
typedef struct {
    uint32_t ch;        // codepoint; 0 = the right half of a double-width cell
    uint32_t fg;        // fully resolved foreground, as handed to the draw call
    uint32_t bg;        // fully resolved background, as handed to the draw call
    uint8_t  style;     // FONT_STYLE_* bits actually used
    uint8_t  deco;      // bit0 underline, bit1 strike
    uint8_t  wide;      // cells this glyph paints across: 1 or 2 (0 = paints none)
    uint8_t  cursor;    // 0 = no cursor here, else 1 + TERM_CURSOR_*
} term_cell_desc_t;

// The last-painted descriptor for every cell of THIS pane, or NULL when the
// allocation failed (in which case every frame is a full repaint, i.e. exactly
// the behaviour that shipped before damage tracking existed - a terminal that
// cannot allocate 141 KB must still be a working terminal).
//
// A POINTER, and banked by term_layout.c alongside `cells`, for the same
// reason `cells` is a pointer: switching panes re-points it instead of copying
// the array. A dirty record applied to the wrong pane's grid is the single
// most likely bug in this feature.
extern term_cell_desc_t (*term_shadow)[TERM_MAX_COLS];

// The GENERATION this pane's shadow is valid at. term_render_invalidate_all()
// bumps the global counter; a pane whose banked generation is behind it
// repaints in full and catches up. This is how "something outside the cell
// grid painted over the window" is expressed, and it reaches EVERY pane
// including ones that are not being drawn right now - which a per-pane flag
// set at draw time could not do.
extern unsigned term_shadow_gen;

// The scrollbar state as it was last PAINTED for this pane. Banked with the
// pane. Compared whole rather than hashed: a scrollbar that silently stops
// tracking the thumb is a real bug, and there is no reason to accept even a
// 2^-32 chance of one to save 40 bytes.
extern gui_scroll_t term_sb_painted;
extern int          term_sb_painted_valid;

// (#damage) Counters, so the effect of this feature is a MEASUREMENT and not
// an impression. `scanned` is the cell count the pre-damage renderer painted
// for the same work (term_rows * term_cols per term_redraw); `painted` is what
// this one actually painted. The ratio is the result. Printed by the
// `termstat` builtin (term_shell.c).
extern unsigned long term_stat_frames;        // term_redraw() calls
extern unsigned long term_stat_full_frames;   // of those, full repaints
extern unsigned long term_stat_cells_scanned; // cells the old renderer would have painted
extern unsigned long term_stat_cells_painted; // cells this renderer painted
extern unsigned long term_stat_invalidates;   // win_invalidate() calls made
extern unsigned long term_stat_frames_idle;   // frames that painted nothing at all
void term_stat_reset(void);

// Draw one cell of `src_row` (a live grid row OR a scrollback row) at screen
// row `dest_row`. Colours come from the COLOUR SCHEME (term_theme.h).
void draw_row_cell(const term_cell_t *src_row, int col, int dest_row);
// Repaint the content area for the current scroll offset. Paints only the
// cells whose resolved descriptor changed since the last paint of this pane,
// unless this pane's shadow generation is stale, in which case it clears the
// content area and paints everything (which is what every caller got before
// damage tracking existed).
void term_redraw(void);

// "The window's pixels are no longer a reliable record of what was painted."
// Every pane's next term_redraw() becomes a full repaint. Call this from any
// path that clears, resizes, re-themes, or paints something over a pane:
// term_layout_redraw_all() (which clears the whole window), EVENT_REDRAW and
// EVENT_RESIZE (the kernel refills a resized window's buffer with its own fill
// colour and pixel-shifts nothing), and a font or colour-scheme change.
void term_render_invalidate_all(void);
// Cells painted by the most recent term_redraw(). 0 means the frame changed
// nothing, so there is nothing to present.
int  term_render_last_painted(void);
// "A tl_present() is guaranteed to follow, so do not present from term_redraw()
// itself." win_invalidate() is a SYNCHRONOUS window_draw() plus a full
// content_width*content_height memcpy in the kernel (uw_commit_content), so a
// frame that presents once per pane AND once at the end costs N+1 of them.
// Set around a group of pane draws that ends in tl_present().
void term_render_suppress_present(int on);

// (#307 PHASE 1) Chrome draw hooks. A module that owns pixels OUTSIDE every
// pane (the menu bar today) registers one here. They run, in registration
// order, at the end of term_redraw() and at the end of
// term_layout_redraw_all(), which are the two functions that finish a frame.
//
// WHY A HOOK AND NOT A CALL FROM main(): term_redraw() is reached from dozens
// of sites across term_parse.c, term_pty.c, term_grid.c, term_layout.c and
// main.c. Chrome painted only from main()'s call sites would be erased by any
// repaint caused by program output or the cursor blink, and an open menu popup
// (which overlays a pane) would vanish on the next blink tick. It also keeps
// the dependency direction in docs/TERMINAL_MODULES.md intact: term_render
// knows nothing about term_menu, it calls function pointers.
//
// (#damage) They now run only on a frame that PAINTED something, i.e. exactly
// the frames that go on to present. A frame that painted nothing cannot have
// erased the chrome, so there is nothing to repair. That is what stops the
// 500 ms cursor blink from repainting the menu bar forever on an idle window.
//
// term_render_add_chrome_hook() returns 0, or -1 if the small fixed table is
// full. term_render_draw_chrome() runs them; term_layout_redraw_all() calls it
// because that function clears the WHOLE window, chrome band included.
#define TERM_CHROME_HOOK_MAX 4
int  term_render_add_chrome_hook(void (*fn)(void));
void term_render_draw_chrome(void);

#endif // TERM_RENDER_H
