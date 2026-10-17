// term_grid.h
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.
#ifndef TERM_GRID_H
#define TERM_GRID_H

#include "term_common.h"
#include "term_emu.h"   // term_cell_t, term_sgr_t, the parser, wcwidth
#include "term_menu.h"  // TERM_MENU_BAR_H: chrome the initial window must pay for


// Terminal cell metrics, derived from the SELECTED font at runtime (#241).
// These were #define 8 / 16, hardwired to the kernel's fixed 8x16 bitmap font
// (win_draw_text()), which is what made font/size configuration impossible:
// there was no runtime quantity to change. term_apply_font() (below)
// recomputes both from font_metrics()/font_glyph() every time the font or
// size changes, the same pattern userland/apps/editor/main.c's
// ed_apply_font() already ships (#351) - reused here, not reinvented.
extern int term_char_w;
extern int term_char_h;
#define TERM_CHAR_W     term_char_w
#define TERM_CHAR_H     term_char_h
// Maximum grid the static cell buffer can hold (sized for a full-screen window:
// 1280/8 = 160 cols, ~720/16 = 45 rows, with headroom).
#define TERM_MAX_COLS   170
#define TERM_MAX_ROWS   52
// Initial grid used when the window is first created.
#define TERM_INIT_COLS  80
#define TERM_INIT_ROWS  24
// TERM_WIDTH/HEIGHT are only used ONCE, to size the window before the first
// EVENT_RESIZE lands (see main()); by then term_char_w/h already hold the
// LOADED font's metrics (term_prefs_load() runs before win_create()), so a
// non-default font size is honoured from the very first frame, not just
// after the first resize.
//
// TWO SEPARATE ALLOWANCES, AND CONFLATING THEM COST US 78x22 (#745, angband).
// TERM_INIT_COLS/TERM_INIT_ROWS is a PROMISE about the GRID, and 80x24 is the
// floor essentially every curses program assumes: Angband refuses to start
// below it outright ("Angband needs at least an 80x24 'curses' screen",
// src/main-gcu.c MIN_TERM0_COLS/MIN_TERM0_LINES), and it is not alone. What
// the window actually delivered was 78x22, because two later features each
// took their share out of the grid without growing the window:
//   - the SCROLLBAR GUTTER (#206): term_handle_resize() subtracts
//     GUI_SCROLL_W from the content width before dividing by TERM_CHAR_W, so
//     640px of content is 78 columns, not 80.
//   - the MENU BAR (#307): term_content_y is TERM_MENU_BAR_H, and
//     tl_relayout() starts every pane below it, so 384px of content is 22
//     rows, not 24.
// So the reserve is named and added HERE, in the one place the initial window
// size is computed, which is what makes TERM_INIT_COLS x TERM_INIT_ROWS a
// statement about the grid the user gets rather than about a rectangle the
// chrome then eats. A feature that takes pixels off the grid must add its
// reserve to this list, or it silently shrinks the promised grid again.
#define TERM_GRID_RESERVE_W  GUI_SCROLL_W       // #206 scrollbar gutter
#define TERM_GRID_RESERVE_H  TERM_MENU_BAR_H    // #307 menu bar
// The WINDOW CONTENT AREA needed for an exactly TERM_INIT_COLS x TERM_INIT_ROWS
// grid, chrome included.
#define TERM_CONTENT_W  (TERM_INIT_COLS * TERM_CHAR_W + TERM_GRID_RESERVE_W)
#define TERM_CONTENT_H  (TERM_INIT_ROWS * TERM_CHAR_H + TERM_GRID_RESERVE_H)
// SYS_WIN_CREATE takes the OUTER size, so the compositor frame is added on top:
// kernel/gui/window.c's client rect is bounds minus 2*BORDER_WIDTH wide and
// minus TITLEBAR_HEIGHT + 2*BORDER_WIDTH tall, i.e. the shipped 2/20 metrics'
// +4 and +24. These are the FRAME, not grid chrome; keep them distinct.
#define TERM_WIDTH      (TERM_CONTENT_W + 4)    // +4: 2*BORDER_WIDTH
#define TERM_HEIGHT     (TERM_CONTENT_H + 24)   // +24: TITLEBAR_HEIGHT + 2*BORDER_WIDTH

// term_cell_t is defined in term_emu.h: a Unicode codepoint, tagged 32-bit
// fg/bg (default / 256-index / 24-bit RGB) and an attribute bitset. The old
// 3-byte { char ch; uint8_t fg; uint8_t bg; } is what made 256-colour, true
// colour, UTF-8 and every text attribute structurally impossible rather than
// merely unimplemented - see docs/TERMINAL_EMULATION.md.

// ---- grid state (defined in term_grid.c) ---------------------------------
// PHASE 1 (tabs/splits): `cells` and `alt_saved_cells` became POINTERS to a
// TERM_MAX_COLS-wide row, so that switching the active pane re-points them
// instead of copying 53 KB of grid. Every existing use site (`cells[y][x]`,
// `cells[row]` as a row pointer) compiles unchanged - that is the whole
// reason this shape was chosen over a struct member. term_layout.c owns which
// pane they point at; nothing else may assign them.
extern term_cell_t (*cells)[TERM_MAX_COLS];
extern int term_cols;      // live grid width  in cells
extern int term_rows;      // live grid height in cells
extern int term_px_w;      // content area width  in pixels
extern int term_px_h;      // content area height in pixels
extern int cursor_x;
extern int cursor_y;
// PHASE 1 (tabs/splits): the pane's content origin inside the window, in
// pixels. (0,0) is exactly the single-pane terminal that shipped before this
// existed, so the default costs nothing. term_render.c adds it to every draw
// and term_scrollback.c hands it to gui_scroll_config(), so a pane's
// scrollbar lands on the pane and not on the window.
extern int term_origin_x;
extern int term_origin_y;
// PHASE 1: was a function-static inside term_clear(). It gates the "do not
// push the very first, never-drawn screen into scrollback" rule (#206), and
// that rule is PER PANE: a newly split pane's first clear must not put
// term_rows blank lines at the front of ITS history either. A function-static
// would have been shared by every pane.
extern int term_clear_calls;
// DECTCEM (CSI ?25 h/l): does the APPLICATION want a cursor at all.
extern bool cursor_visible;
// The blink PHASE, this terminal's own cosmetic timer. These used to be the
// SAME variable, so the 500ms blink flipped the cursor back on half a second
// after an application asked for it to be hidden. ?25 had never been verified;
// verifying it is what found this.
extern bool cursor_blink_on;
// "Do not draw a cursor in the pane being drawn RIGHT NOW", set by
// term_layout.c around the redraw of an UNFOCUSED pane and cleared straight
// after. It is a third, separate thing from the two above, and separating it
// is the point.
//
// This used to be expressed by writing cursor_visible = false, i.e. by
// scribbling on the APPLICATION's DECTCEM state to say something about the
// VIEW - and tl_set_focus() then had to write cursor_visible = true to undo
// it, with no way to tell "I cleared this to draw an unfocused pane" apart
// from "the program asked for the cursor to be hidden". MEASURED on golden
// 2052: with a child holding ESC[?25l in a split pane, focusing away and back
// brought the cursor BACK while the program still wanted it gone. ?25 had
// never been verified, which is why nobody had seen it.
extern int term_cursor_suppressed;
// The current rendition ("pen"), replacing current_fg/current_bg/
// current_reverse. Reverse is an ATTRIBUTE BIT applied at DRAW time, not a
// swap of the two colour fields at parse time: the old scheme meant a colour
// set AFTER ESC[7m landed in the wrong slot and SGR 0 could not restore the
// pair.
extern term_sgr_t g_pen;
// Font ascent for the current face/size, so a codepoint the terminal
// rasterises itself (the non-ASCII path) lands on the SAME baseline the
// kernel's win_draw_text_ttf_ex() uses for ASCII.
extern int term_ascent;

// ---- DECSTBM / DECSC / DECAWM / alternate screen (term_grid.c) -----------
extern int scroll_top;
extern int scroll_bottom;  // -1 = "last live row", resolve via term_scroll_bottom_eff()
extern int saved_cursor_x, saved_cursor_y;
// DECSC saves the whole PEN, not just the two colours: a save/restore across
// ESC[1m used to lose the bold because there was nowhere to put it.
extern term_sgr_t g_saved_pen;
extern int term_autowrap;
extern int in_alt_screen;
extern term_cell_t (*alt_saved_cells)[TERM_MAX_COLS];
extern int alt_saved_cursor_x, alt_saved_cursor_y;

// ---- grid operations ------------------------------------------------------
// #220 NOTE FOR EVERY FUTURE CALLER: the row-aligned pixel height of the live
// grid is term_rows * TERM_CHAR_H, and that is the ONLY height that may be
// handed to the shared gui_scroll_t (see term_scrollback_reconfigure()).
// term_px_h is the raw window content height and is almost never a whole
// number of rows.
int  term_scroll_bottom_eff(void);
void term_scroll_region(int top, int bottom);
void term_scroll_region_down(int top, int bottom);
void term_insert_line(int at, int top, int bottom);
void term_delete_line(int at, int top, int bottom);
void term_insert_chars(int row, int at, int n);
void term_delete_chars(int row, int at, int n);
void term_newline(void);
void term_clear(void);
void term_full_reset(void);

// Fill a cell with a blank in the CURRENT BACKGROUND and no attributes
// ("background colour erase": what xterm does, and what every TUI that paints a
// coloured region and then erases part of it depends on).
void blank_cell(term_cell_t *c);
// A double-width character occupies TWO cells: the lead holds the codepoint,
// the next holds ch == 0. Overwriting either half alone leaves an orphan, and
// from then on the grid and the application disagree about which column is
// which for the rest of the row. Every write goes through this first.
void cell_break_wide(int row, int col);
// Horizontal tab stops. The old code hard-coded "every 8 columns" AND WROTE
// SPACES to get there, erasing whatever was underneath; a tab is
// non-destructive cursor motion in every real terminal.
extern uint8_t g_tabstop[TERM_MAX_COLS];
// Write the DEFAULT tab stops (every 8 columns, none at column 0) into any
// TERM_MAX_COLS-wide tabstop array. Exists because there were TWO copies of
// that loop the moment panes arrived: the live one here and one initialising a
// newly created pane's banked copy in term_layout.c. Two copies of "what a
// fresh terminal's tab stops are" is how a split pane quietly gets different
// tab behaviour from the pane it was split from.
void term_tabstops_default(uint8_t *stops);
void term_handle_resize(int content_w, int content_h);

// (#307 PHASE 1, menu bar) Pixels of WINDOW-LEVEL CHROME reserved at the TOP
// of the window content area, above everything term_layout lays out. 0 when
// the menu bar is hidden, TERM_MENU_BAR_H (24) when it is shown.
//
// THIS IS NOT term_origin_y. term_origin_y is the FOCUSED PANE's origin and is
// computed by term_layout from the tab strip and the split tree. term_content_y
// is the band above all of that, which the layout must not lay anything out
// in. They stack: tl_relayout() starts its work at term_content_y, so a pane's
// origin already includes this inset and NOTHING adds it a second time.
//
// #220 NOTE. Reserving chrome this way is NOT the same as taking a row off
// term_rows behind the grid's back. Every pane still gets an explicit rect from
// tl_pane_apply_geometry(), and the viewport HEIGHT handed to
// gui_scroll_config() is still the row-aligned term_rows * TERM_CHAR_H. Only
// where the layout STARTS moves. If you add another band of window-level
// chrome, add its height into THIS variable; do not invent a second offset and
// do not subtract rows anywhere.
extern int term_content_y;

#endif // TERM_GRID_H
