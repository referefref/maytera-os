// term_scrollback.c
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.

#include "term_common.h"
#include "term_grid.h"
#include "term_scrollback.h"

// --- Scrollback (#206) ------------------------------------------------------
// Ring buffer of rows that have scrolled off the top of the live grid. Each
// retained line is a full TERM_MAX_COLS-wide copy of a `cells` row (the tail
// beyond that row's actual column count at capture time is whatever `cells`
// held there, which is always blank: term_putc/term_handle_resize never write
// past the live term_cols, so it reads back as spaces). Allocated once in
// main() with malloc(); sb_lines is NULL (scrollback silently a no-op) if that
// ever fails, rather than risking a NULL-deref crash on a heap that is merely
// tight.
term_cell_t *sb_lines = NULL;
int sb_count = 0;   // valid retained lines, 0..SCROLLBACK_LINES
int sb_head  = 0;   // physical ring index of the OLDEST retained line

// The shared scrollable-viewport widget (userland/libc/gui_scroll.h, #291/
// #96/#438). One virtual document = sb_count history lines followed by
// term_rows live rows; gui_scroll_t owns the offset/clamping/scrollbar
// geometry the same way Files/Settings/the browser already use it. snap=1
// (set once below) keeps every offset on a whole-row boundary, so
// gui_scroll_first_item() is exactly "index of the top visible line" with no
// partial-row math needed.
gui_scroll_t term_scroll_view;
// True = the viewport is pinned to the live screen (normal typing view: new
// output is always visible, matching every terminal's default behaviour).
// False = the user has scrolled back; new output must NOT yank the view back
// down, per #206.
int term_at_bottom = 1;
// Physical pointer to logical scrollback line `idx` (0 = oldest retained).
term_cell_t *sb_row(int idx) {
    int phys = (sb_head + idx) % sb_capacity;
    return &sb_lines[phys * TERM_MAX_COLS];
}

// --- Runtime ring depth (profiles) -----------------------------------------
// sb_capacity is what is ALLOCATED; sb_want is what the active profile asks
// for. term_scrollback_set_capacity() only records the request, and
// term_scrollback_reconfigure() below does the actual reallocation.
//
// WHY THE REALLOC IS IN reconfigure() AND NOT AT THE CALL SITE THAT CHANGED
// THE SETTING: three things can change the depth (the preferences dialog, a
// profile switch, and TERMPREF.CFG live-reload from another process), and
// every one of them is followed by a redraw, which calls reconfigure() first.
// Putting the realloc in the single place they all funnel through means there
// is exactly one copy of "resize the ring, keep the newest lines, re-clamp the
// view" instead of three that can drift apart. This is the same reason
// reconfigure() is already the ONLY caller of gui_scroll_config() (#220).
int sb_capacity = SCROLLBACK_LINES;
int sb_want     = SCROLLBACK_LINES;

void term_scrollback_set_capacity(int lines) {
    if (lines < 200)   lines = 200;
    if (lines > 20000) lines = 20000;
    sb_want = lines;
}

// Copy the newest min(sb_count, cap) retained lines into a freshly allocated
// ring of `cap` lines. Returns 1 if the ring changed, 0 if it could not (the
// old ring is then left exactly as it was: a failed malloc must not cost the
// user their scrollback, and must never leave sb_lines pointing at freed
// memory).
static int sb_realloc(int cap) {
    term_cell_t *nb = (term_cell_t *)malloc((size_t)cap * TERM_MAX_COLS * sizeof(term_cell_t));
    if (!nb) { sb_want = sb_capacity; return 0; }
    int keep = sb_count < cap ? sb_count : cap;
    int first = sb_count - keep;          // drop the oldest lines that no longer fit
    for (int i = 0; i < keep; i++) {
        const term_cell_t *src = sb_lines ? sb_row(first + i) : 0;
        term_cell_t *dst = &nb[i * TERM_MAX_COLS];
        for (int c = 0; c < TERM_MAX_COLS; c++) {
            // blank_cell() is the SHARED definition of an empty cell
            // (term_grid.c). A literal here would have to be revisited every
            // time term_cell_t grows a field, and it already has: the emulation
            // core added `attr` and a designated-initialiser literal silently
            // left it uninitialised.
            if (src) dst[c] = src[c];
            else     blank_cell(&dst[c]);
        }
    }
    // The view is anchored to a LINE, and `first` lines just disappeared off the
    // bottom of history. Slide the offset by the same amount, or a user who was
    // reading scrollback when the depth changed silently ends up looking at
    // different content. gui_scroll_config() (the caller, immediately after)
    // re-clamps, so this can only ever make the view MORE correct.
    if (first > 0 && !term_at_bottom) {
        int off = term_scroll_view.offset - first * TERM_CHAR_H;
        gui_scroll_set(&term_scroll_view, off < 0 ? 0 : off);
    }
    if (sb_lines) free(sb_lines);
    sb_lines   = nb;
    sb_capacity = cap;
    sb_count   = keep;
    sb_head    = 0;
    return 1;
}

void term_scrollback_alloc(void) {
    sb_capacity = sb_want;
    sb_lines = (term_cell_t *)malloc((size_t)sb_capacity * TERM_MAX_COLS * sizeof(term_cell_t));
    if (!sb_lines) {
        // A deep ring the heap cannot serve must not silently disable
        // scrollback altogether: fall back to the historical 2000 lines, and
        // only then to "scrollback is a no-op" (sb_lines == NULL), which every
        // entry point in this file already handles.
        sb_capacity = SCROLLBACK_LINES;
        sb_want     = SCROLLBACK_LINES;
        sb_lines = (term_cell_t *)malloc((size_t)sb_capacity * TERM_MAX_COLS * sizeof(term_cell_t));
    }
}

// Retain a departing row before it is overwritten (called from term_scroll_region()
// and term_clear()). If the ring is already full, the oldest line is evicted
// and the whole retained window slides forward by one line; a view that is
// scrolled back must slide its offset back by one line-height too, or it
// would silently show newer content than the user left it on (#206).
void term_history_push(const term_cell_t *row) {
    if (!sb_lines) return;   // malloc() failed at startup: scrollback is a no-op, not a crash
    int phys;
    if (sb_count < sb_capacity) {
        phys = (sb_head + sb_count) % sb_capacity;
        sb_count++;
    } else {
        phys = sb_head;
        sb_head = (sb_head + 1) % sb_capacity;
        if (!term_at_bottom) {
            int off = term_scroll_view.offset - TERM_CHAR_H;
            gui_scroll_set(&term_scroll_view, off < 0 ? 0 : off);
        }
    }
    for (int c = 0; c < TERM_MAX_COLS; c++) {
        sb_lines[phys * TERM_MAX_COLS + c] = row[c];
    }
}
// Recompute the shared scroll widget's viewport/content extent from the
// current window size and scrollback depth. Cheap (a handful of int ops);
// safe to call before every redraw. gui_scroll_config() re-clamps the offset
// itself, so a window shrink or a scrollback-depth change can never strand
// the view past the new end.
//
// #resize-geom (owner report, tier 1 follow-up): the viewport height handed
// to the widget MUST be term_rows*TERM_CHAR_H (the row-aligned pixel height
// actually drawn into by term_redraw()), NOT the raw term_px_h. term_px_h is
// whatever pixel height the window happens to be after a drag-resize, and it
// is essentially NEVER an exact multiple of TERM_CHAR_H - there is always a
// leftover sub-row strip (0..TERM_CHAR_H-1 px) that term_redraw() clears but
// never draws a row into.
//
// Passing the un-aligned term_px_h as `h` made gui_scroll_max() = content_px
// - h come out to sb_count*TERM_CHAR_H - <leftover>, i.e. NOT a multiple of
// TERM_CHAR_H whenever sb_count > 0. gui_scroll_set()'s snap-to-max fallback
// (gui_scroll.c: "if (snapped > max) snapped = max") then pins the "at
// bottom" offset to that un-aligned max, and gui_scroll_first_item()
// (offset/step_px, truncating) reads back ONE LESS than sb_count. Every row
// term_redraw() then paints is off by one: row 0 shows the newest RETAINED
// scrollback line instead of the oldest live row, and the true last live row
// (where the cursor and, for a full-screen program, the status/bottom line
// live) is pushed off the bottom and never drawn at all.
//
// This is invisible until BOTH conditions hold: sb_count > 0 (some
// scrollback exists - true after almost any real session) AND the content
// height is not a whole number of rows (true after almost any drag-resize,
// since a resize handle lands on an arbitrary pixel, not a row boundary).
// That is exactly the owner's repro (long `ls` output, then `clear`, cursor
// height wrong) and the vi complaint (bottom line/status row missing) once
// the window had ever been resized - and exactly why a fresh, never-resized
// window (whose TERM_WIDTH/HEIGHT are constructed to be an exact multiple of
// the cell size) never showed it.
//
// The fix: always hand the widget the ROW-ALIGNED height. content_px is
// already an exact multiple of TERM_CHAR_H (total_lines is an integer line
// count), so with an aligned h too, gui_scroll_max() is exactly
// sb_count*TERM_CHAR_H - a clean multiple of the step - and every offset
// gui_scroll_set() ever produces (including the snap-to-max fallback) stays
// on a row boundary. gui_scroll_first_item() == sb_count at the bottom,
// always, regardless of how the window was resized. The sub-row leftover
// strip at the bottom of the window is still cleared to the background
// colour by term_redraw()'s full-content-area fill; it just correctly plays
// no part in the scroll math, since no row is ever drawn there.
void term_scrollback_reconfigure(void) {
    // Apply a pending depth change first, so the gui_scroll_config() below is
    // computed from the ring that actually exists (see sb_realloc's comment).
    if (sb_want != sb_capacity && sb_lines) sb_realloc(sb_want);
    int total_lines = sb_count + term_rows;
    int row_aligned_h = term_rows * TERM_CHAR_H;
    // PHASE 1 (tabs/splits): the viewport rect is the PANE's rect, so the
    // origin goes in here. This is still the ONE call to gui_scroll_config()
    // in the whole app and still the ONE place the row-aligned height is
    // computed, which is exactly what #220 requires - and with N panes it now
    // matters N times over: every pane's creation AND every pane's resize
    // reach the widget through this single function.
    gui_scroll_config(&term_scroll_view, term_origin_x, term_origin_y,
                       term_px_w, row_aligned_h,
                       total_lines * TERM_CHAR_H, TERM_CHAR_H);
    term_scroll_view.snap = 1;   // fixed-height rows: always land on a row boundary
}

// Refresh the "pinned to live" flag from the widget's own offset/max. Call
// after anything that can move term_scroll_view.offset.
void term_scroll_sync_bottom(void) {
    term_at_bottom = (term_scroll_view.offset >= gui_scroll_max(&term_scroll_view));
}
