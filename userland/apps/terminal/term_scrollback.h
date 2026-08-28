// term_scrollback.h
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.
#ifndef TERM_SCROLLBACK_H
#define TERM_SCROLLBACK_H

#include "term_common.h"
#include "term_grid.h"


// Scrollback buffer size (#206). This constant existed with the name and
// nothing behind it (grep for it before this fix found exactly one
// occurrence, its own #define). It now backs a real ring buffer allocated
// with malloc() at startup, not a static array (user.ld links ONE RWX
// PT_LOAD; a large .bss breaks the loader, see blame.md).
//
// 2000 lines * TERM_MAX_COLS(170) * sizeof(term_cell_t)(3 bytes) ~= 1.0MB.
// At the default 80x24 window that is over 80 full screens of history, and
// even at the largest supported grid (TERM_MAX_ROWS=52) it is still ~38
// screens. That comfortably covers "scroll back to find the command I ran a
// few minutes ago" without the allocation being large enough to worry about
// on a userland heap.
// The ring depth is a PROFILE SETTING now, not a fixed constant. This name is
// kept, with its original value, as the DEFAULT and as the "what every window
// had before profiles existed" reference; the live depth is sb_capacity.
#define SCROLLBACK_LINES 2000

// Live ring depth, and the depth the active profile wants. They differ only
// between a profile change and the next term_scrollback_reconfigure(), which
// is the ONE place the reallocation happens (see term_scrollback.c for why it
// is there and not at the call site that changed the setting).
extern int sb_capacity;
extern int sb_want;

// Allocate the ring at sb_want lines. main() calls this once instead of
// malloc-ing SCROLLBACK_LINES itself, so the very first window already honours
// a profile asking for a deeper (or shallower) ring rather than allocating
// 2000 lines and resizing a frame later.
void term_scrollback_alloc(void);
// Request a new depth. Takes effect at the next reconfigure; retained history
// is preserved up to the new depth (newest lines kept).
void term_scrollback_set_capacity(int lines);

// ---- scrollback state (defined in term_scrollback.c) ---------------------
extern term_cell_t *sb_lines;      // NULL = malloc failed, scrollback disabled
extern int sb_count;               // retained lines, 0..SCROLLBACK_LINES
extern int sb_head;                // ring index of the OLDEST retained line
extern gui_scroll_t term_scroll_view;
extern int term_at_bottom;         // 1 = pinned to the live screen

term_cell_t *sb_row(int idx);
void term_history_push(const term_cell_t *row);
// #220: the ONE geometry function. Both first-time creation and every resize
// go through it, so the viewport height it hands gui_scroll_config() cannot
// drift between the two. It passes term_rows * TERM_CHAR_H, NEVER term_px_h.
void term_scrollback_reconfigure(void);
void term_scroll_sync_bottom(void);

#endif // TERM_SCROLLBACK_H
