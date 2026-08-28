// term_grid.c
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.

#include "term_common.h"
#include "term_grid.h"

// Erase fills a cell with the CURRENT background but NO attributes: background
// colour erase. See term_grid.h.
void blank_cell(term_cell_t *c) {
    c->ch = ' ';
    c->fg = g_pen.fg;
    c->bg = g_pen.bg;
    c->attr = 0;
    c->pad[0] = c->pad[1] = c->pad[2] = 0;
}

void cell_break_wide(int row, int col) {
    if (row < 0 || row >= TERM_MAX_ROWS || col < 0 || col >= term_cols) return;
    if (cells[row][col].ch == 0 && col > 0) {
        cells[row][col - 1].ch = ' ';           // orphaned lead
    } else if (term_emu_wcwidth(cells[row][col].ch) == 2 && col + 1 < term_cols &&
               cells[row][col + 1].ch == 0) {
        cells[row][col + 1].ch = ' ';           // orphaned continuation
    }
}

#include "term_scrollback.h"

// Cell metrics, re-derived from the selected font by term_apply_font()
// (term_theme.c). Declared in term_grid.h.
int term_char_w = 8;
int term_char_h = 16;
// Terminal state. PHASE 1 (tabs/splits): the DEFAULT buffers stay right here
// as file-scope arrays and pane 0 adopts them, so the one-tab/one-pane
// terminal allocates exactly what it always did. Only a SECOND pane costs a
// malloc. See term_layout.h.
static term_cell_t g_cells_default[TERM_MAX_ROWS][TERM_MAX_COLS];
static term_cell_t g_alt_default[TERM_MAX_ROWS][TERM_MAX_COLS];
term_cell_t (*cells)[TERM_MAX_COLS] = g_cells_default;
// Runtime grid size (changes when the window is resized/maximized/restored).
int term_cols = TERM_INIT_COLS;
int term_rows = TERM_INIT_ROWS;
// Current content pixel size, used to clear the full content area on redraw.
// #241: TERM_CHAR_W/H are now runtime variables (term_apply_font()), so these
// can no longer be constant-folded static initializers. Seeded with the
// legacy 8x16 literal; term_prefs_load() (main(), before win_create()) and
// the term_handle_resize() call right after it correct these to the real
// selected font's metrics before the first frame is drawn.
int term_px_w = TERM_INIT_COLS * 8;
int term_px_h = TERM_INIT_ROWS * 16;
int cursor_x = 0;
int cursor_y = 0;
// PHASE 1: pane content origin. 0,0 = the whole window, i.e. the pre-splits
// terminal exactly.
int term_origin_x = 0;
int term_origin_y = 0;
bool cursor_visible = true;
bool cursor_blink_on = true;
// A VIEW decision, and deliberately NOT part of `cursor_visible`. See
// term_grid.h.
int term_cursor_suppressed = 0;
term_sgr_t g_pen = { TE_COL_DEFAULT, TE_COL_DEFAULT, 0 };
int term_ascent = 12;
uint8_t g_tabstop[TERM_MAX_COLS];
// THE definition of "the default tab stops". See term_grid.h.
void term_tabstops_default(uint8_t *stops) {
    for (int i = 0; i < TERM_MAX_COLS; i++)
        stops[i] = (uint8_t)((i % 8) == 0 && i != 0);
    stops[0] = 0;
}
// ---- Terminal parity tier 1 state (docs/TERMINAL_PARITY.md) ---------------
// DECSTBM scroll region, 0-based inclusive. scroll_bottom == -1 means "the
// last live row", resolved by term_scroll_bottom_eff() so a resize or a
// grid shrink can never leave it pointing past the new term_rows.
int scroll_top = 0;
int scroll_bottom = -1;
// ESC 7/ESC 8 (DECSC/DECRC) and CSI s / CSI u (SCOSC/SCORC) share one save
// slot. Real terminals give DECSC a second slot that also remembers SGR
// state; sharing is a deliberate simplification (see docs/TERMINAL_PARITY.md)
// since no target app in the approved-ports list (#91) uses both forms in
// the same session.
int saved_cursor_x = 0, saved_cursor_y = 0;
term_sgr_t g_saved_pen = { TE_COL_DEFAULT, TE_COL_DEFAULT, 0 };
// DECAWM (CSI ?7h/l). On by default, matching every real terminal.
int term_autowrap = 1;
// Alternate screen buffer (CSI ?1049h/l, ?47h/l, ?1047h/l). One saved copy of
// the primary screen's cells + cursor; entering alt screen stashes the
// primary grid here and clears `cells` for the alt program, leaving restores
// it. in_alt_screen also gates term_clear()'s scrollback push, so a
// full-screen program's own redraws no longer spam scrollback with alt-screen
// content (a limitation the old #206 comment on term_clear() called out as
// "accepted"; it is fixed here as a side effect of tracking real alt-screen
// state).
int in_alt_screen = 0;
term_cell_t (*alt_saved_cells)[TERM_MAX_COLS] = g_alt_default;
int alt_saved_cursor_x = 0, alt_saved_cursor_y = 0;
// PHASE 1: per-pane, banked by term_layout.c. See term_grid.h.
int term_clear_calls = 0;
// Recompute the grid for a new content size (called on EVENT_RESIZE).
// (#307 PHASE 1) Window-level top chrome inset. See the long note in
// term_grid.h for why this is not term_origin_y.
int term_content_y = 0;

void term_handle_resize(int content_w, int content_h) {
    // Reserve the scrollbar gutter (#206) so the shared widget never draws
    // over the last column or two of text. Reserved unconditionally (not just
    // when scrollback happens to be non-empty right now) so the column count
    // does not jump as soon as the first line scrolls off the top.
    int usable_w = content_w - GUI_SCROLL_W;
    if (usable_w < TERM_CHAR_W) usable_w = content_w;  // degrade gracefully on a tiny window
    int nc = usable_w / TERM_CHAR_W;
    int nr = content_h / TERM_CHAR_H;
    if (nc < 1) nc = 1;
    if (nr < 1) nr = 1;
    if (nc > TERM_MAX_COLS) nc = TERM_MAX_COLS;
    if (nr > TERM_MAX_ROWS) nr = TERM_MAX_ROWS;

    // Initialise any cells newly exposed by growing the grid so they hold a
    // valid blank cell rather than stale memory.
    for (int row = 0; row < nr; row++) {
        for (int col = 0; col < nc; col++) {
            if (row >= term_rows || col >= term_cols) {
                blank_cell(&cells[row][col]);
            }
        }
    }

    term_cols = nc;
    term_rows = nr;
    term_px_w = content_w;
    term_px_h = content_h;

    if (cursor_x >= term_cols) cursor_x = term_cols - 1;
    if (cursor_y >= term_rows) cursor_y = term_rows - 1;

    // Tier 1 (docs/TERMINAL_PARITY.md): a resize invalidates any DECSTBM
    // scroll region a full-screen program set against the OLD row count -
    // real terminals (xterm included) reset margins to the full screen on
    // resize for exactly this reason. The program gets a SIGWINCH (the
    // caller always follows a resize with TIOCSWINSZ) and is expected to
    // redraw and reassert its own region, the same as it would after any
    // other resize.
    scroll_top = 0;
    scroll_bottom = -1;
}
// Effective bottom margin of the current DECSTBM scroll region: -1 means
// "the last live row", resolved against the CURRENT term_rows so a shrink
// can never leave callers reading a stale, out-of-range row index.
int term_scroll_bottom_eff(void) {
    int b = scroll_bottom;
    if (b < 0 || b >= term_rows) b = term_rows - 1;
    return b;
}

// Scroll [top,bottom] (0-based, inclusive) up by one line: row `top` is
// retired, rows top+1..bottom move up one, row `bottom` is blanked. This is
// what a linefeed at the bottom margin does (term_newline() below) and what
// CSI M (delete line) reduces to when it deletes the row right at the
// margin. Only pushes the retired row to scrollback when top==0 (the region
// includes the very top of the screen) AND the alternate screen is not
// active (#tier1: in_alt_screen also fixes the old "accepted limitation"
// where a full-screen program's own redraws spammed scrollback - see the
// comment on in_alt_screen above and on term_clear() below).
void term_scroll_region(int top, int bottom) {
    if (top < 0) top = 0;
    if (bottom >= term_rows) bottom = term_rows - 1;
    if (top >= bottom) return;
    if (top == 0 && !in_alt_screen) term_history_push(cells[top]);
    for (int row = top; row < bottom; row++)
        for (int col = 0; col < term_cols; col++)
            cells[row][col] = cells[row + 1][col];
    for (int col = 0; col < term_cols; col++) {
        blank_cell(&cells[bottom][col]);
    }
    if (top == 0) {
        int was_at_bottom = term_at_bottom;
        term_scrollback_reconfigure();
        if (was_at_bottom) gui_scroll_set(&term_scroll_view, gui_scroll_max(&term_scroll_view));
        term_scroll_sync_bottom();
    }
}

// The opposite direction: scroll [top,bottom] DOWN by one line (a blank row
// appears at `top`, everything else shifts toward `bottom`, the row that was
// at `bottom` is dropped). Used by RI (ESC M, reverse index) when the cursor
// is already at the top margin. Never touches scrollback: nothing new comes
// from history in this direction, exactly like every other terminal's RI.
void term_scroll_region_down(int top, int bottom) {
    if (top < 0) top = 0;
    if (bottom >= term_rows) bottom = term_rows - 1;
    if (top >= bottom) return;
    for (int row = bottom; row > top; row--)
        for (int col = 0; col < term_cols; col++)
            cells[row][col] = cells[row - 1][col];
    for (int col = 0; col < term_cols; col++) {
        blank_cell(&cells[top][col]);
    }
}

// CSI L (insert line) / CSI M (delete line), clamped to the DECSTBM region:
// a program that inserts/deletes lines outside its own scroll region (rare,
// and arguably a program bug) is a no-op here rather than corrupting rows it
// does not own.
void term_insert_line(int at, int top, int bottom) {
    if (at < top || at > bottom) return;
    for (int row = bottom; row > at; row--)
        for (int col = 0; col < term_cols; col++)
            cells[row][col] = cells[row - 1][col];
    for (int col = 0; col < term_cols; col++) {
        blank_cell(&cells[at][col]);
    }
}
void term_delete_line(int at, int top, int bottom) {
    if (at < top || at > bottom) return;
    for (int row = at; row < bottom; row++)
        for (int col = 0; col < term_cols; col++)
            cells[row][col] = cells[row + 1][col];
    for (int col = 0; col < term_cols; col++) {
        blank_cell(&cells[bottom][col]);
    }
}

// CSI @ (insert n blank chars at cursor, shifting the rest of the row right,
// discarding whatever falls off the end) / CSI P (delete n chars at cursor,
// shifting the rest of the row left, blanking the vacated tail).
void term_insert_chars(int row, int at, int n) {
    if (n < 1) n = 1;
    for (int col = term_cols - 1; col >= at + n; col--) cells[row][col] = cells[row][col - n];
    int end = at + n; if (end > term_cols) end = term_cols;
    for (int col = at; col < end; col++) {
        blank_cell(&cells[row][col]);
    }
}
void term_delete_chars(int row, int at, int n) {
    if (n < 1) n = 1;
    int col;
    for (col = at; col + n < term_cols; col++) cells[row][col] = cells[row][col + n];
    for (; col < term_cols; col++) {
        blank_cell(&cells[row][col]);
    }
}
// ESC c (RIS - full reset). vim/less/mc issue this only on a genuine "reset
// the terminal" request (rare), but an app that DOES send it and gets a
// terminal that only half-resets is a worse bug than one that never needed
// this case at all.
void term_full_reset(void) {
    term_emu_sgr_reset(&g_pen);
    term_emu_sgr_reset(&g_saved_pen);
    saved_cursor_x = 0; saved_cursor_y = 0;
    cursor_blink_on = true;
    // RIS also silences mouse reporting and drops bracketed paste. A child that
    // crashed with mouse reporting on used to leave the SHELL PROMPT emitting
    // mouse bytes as text, which looks like a keyboard fault.
    term_emu_modes_reset();
    term_tabstops_default(g_tabstop);
    scroll_top = 0; scroll_bottom = -1;
    term_autowrap = 1;
    cursor_visible = true;
    in_alt_screen = 0;   // RIS drops any alternate-screen state outright
    term_clear();
}
// Handle newline (LF). Tier 1 (docs/TERMINAL_PARITY.md): respects a DECSTBM
// scroll region set by CSI r - a linefeed at the BOTTOM MARGIN scrolls only
// that region, which is what lets vim/less/mc pin a status line while the
// text area above it scrolls. A linefeed anywhere else in the region (or
// outside it entirely, e.g. writing to a pinned footer row) just advances
// the cursor, clamped to the last screen row as a safety net.
void term_newline(void) {
    cursor_x = 0;
    int bot = term_scroll_bottom_eff();
    if (cursor_y == bot) {
        term_scroll_region(scroll_top, bot);
    } else {
        cursor_y++;
        if (cursor_y >= term_rows) cursor_y = term_rows - 1;
    }
}
// Clear terminal. Retains the pre-clear screen into scrollback (#206) before
// blanking it, the same convention xterm and friends use: `clear`/ESC[2J wipe
// the visible screen, not the history, so a scroll-up after clearing still
// finds what was there.
//
// FIXED (tier 1, docs/TERMINAL_PARITY.md): this used to say "Known, accepted
// limitation: this does not distinguish the primary screen from the
// alternate screen, so a full-screen program's repeated ESC[?1049h/ESC[2J
// redraws also push rows here". Real alternate-screen tracking (in_alt_screen,
// see handle_escape_char()'s ?1049/?47/?1047 handling) now exists, so that
// limitation is fixed as a side effect: a clear while in_alt_screen never
// pushes to scrollback, matching every real terminal (vi's own redraws do
// not spam the shell's scroll-up history).
void term_clear(void) {
    // #206: main() calls term_clear() once at true startup, before a single
    // character has ever been drawn, to establish a known-blank grid. That
    // first call has nothing worth preserving - pushing it anyway put
    // term_rows blank lines at the very front of scrollback, so Home/PageUp-
    // to-the-top landed on a blank screen instead of the oldest real output.
    // Every LATER call on the PRIMARY screen (the "clear" command, ESC[2J/3J)
    // still pushes: at that point the screen may hold real content.
    term_clear_calls++;
    int push_history = (term_clear_calls > 1) && !in_alt_screen;

    for (int row = 0; row < term_rows; row++) {
        if (push_history) term_history_push(cells[row]);
        for (int col = 0; col < term_cols; col++) {
            blank_cell(&cells[row][col]);
        }
    }
    cursor_x = 0;
    cursor_y = 0;

    // A clear always returns the view to "live", matching every other
    // terminal: there is nothing left on the current screen to be scrolled
    // away from.
    term_at_bottom = 1;
    term_scrollback_reconfigure();
    gui_scroll_set(&term_scroll_view, gui_scroll_max(&term_scroll_view));
    term_scroll_sync_bottom();
}
