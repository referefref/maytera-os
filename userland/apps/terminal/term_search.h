// term_search.h - find-in-scrollback for the MayteraOS Terminal (#221, PHASE 1).
//
// OWNERSHIP. This module owns the find bar, the match list, and every pixel of
// the reserved strip at the bottom of the content area. It consumes
// term_scrollback.[ch] and term_grid.[ch] READ-ONLY and restructures neither.
//
// WHAT IT REUSES RATHER THAN REINVENTS, because this tree keeps paying for
// second copies (docs/MPORTS.md owner rule 1, docs/TERMINAL_MODULES.md):
//   * the query field is a libc textfield_t (userland/libc/textfield.h), so it
//     gets a real caret, selection, Ctrl+A/C/X/V against the SYSTEM clipboard
//     and Ctrl+Z/Y undo for free. Editor's own find bar predates that widget
//     and only ever appends and backspaces; see the note at the bottom of this
//     header for why Editor was not converted in the same pass.
//   * scrolling a match into view is gui_scroll_reveal() on the terminal's
//     EXISTING term_scroll_view, never new offset math (#220 is exactly what
//     private geometry math costs here).
//   * the chrome is gui_button()/theme_color(), the shared style engine.
//   * regex is userland/ports/musl-regex (MIT, TRE-derived), the SAME
//     libregex.a /APPS/GREP, /APPS/VI and /APPS/SED already link. #87 records
//     that busybox vi shipped a PRIVATE LGPL-3.0 GNU regex copy; the whole
//     point of that port was to end that, so a fourth private engine here
//     would re-open a licence exposure the tree has already closed.
//   * the modifier chord comes from libc gui_mods.h (#221 phase 0), the ONE
//     tracker. This file does not track Shift/Ctrl itself.
//
// GEOMETRY, AND WHY ONE WINDOW-LEVEL BAR AT THE BOTTOM. docs/
// TERMINAL_KONSOLE_CHROME_SPEC.md section 5 puts a bar under EACH PANE's
// header. Two deviations, both deliberate:
//
// ONE BAR, NOT ONE PER PANE. There is one keyboard and one focused pane, so N
// simultaneous find bars would be N pieces of state the user cannot type into.
// The bar searches the FOCUSED pane and follows the focus rule term_select.h
// already set for selections: the find owns the pane it was opened in.
//
// BOTTOM, NOT UNDER THE PANE HEADER. The strip is taken out of the WINDOW
// content area by term_layout's EVENT_RESIZE case, BEFORE panes are laid out,
// so it costs zero changes to pane geometry: tl_pane_apply_geometry() stays
// the one function that gives a pane its rect (#220), every child still gets
// its TIOCSWINSZ (#227), and the row alignment is bit-for-bit identical
// whether the bar is open or shut. Docking under a pane header would instead
// mean a second thing computing pane rects, which is exactly what #220 was. A
// bottom find bar is also what Konsole 21.x, gnome-terminal and xterm ship.
#ifndef TERM_SEARCH_H
#define TERM_SEARCH_H

#include "term_common.h"

// Height of the find bar, in pixels. 26, the SAME constant as Editor's
// FIND_HEIGHT and the spec's, so the two apps' chrome reads as one system.
#define TERM_SEARCH_BAR_H     26
// Query capacity. Sized to a full terminal line so "search for the command I
// just ran" cannot be truncated by the widget.
#define TERM_SEARCH_QUERY_MAX 192
// Upper bound on tracked matches. A hit count beyond this is reported as
// "999+" rather than silently wrong: see term_search_truncated().
#define TERM_SEARCH_MAX_MATCHES 1000

// ---------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------
int  term_search_is_open(void);
// Pixels the bar takes out of the window's content area RIGHT NOW (0 when
// shut). The single source of truth for the reservation; nothing else may
// re-derive it.
int  term_search_reserved_h(void);
int  term_search_match_count(void);   // matches found so far
int  term_search_match_index(void);   // 1-based index of the active match, 0 = none
int  term_search_truncated(void);     // 1 = more matches exist than are tracked
int  term_search_scanning(void);      // 1 = an incremental scan is still running
int  term_search_bad_regex(void);     // 1 = regex mode with a pattern that will not compile
int  term_search_case_sensitive(void);
int  term_search_regex(void);
const char *term_search_query(void);

// ---------------------------------------------------------------------------
// THE MENU API. The menu-bar agent's Edit menu calls exactly these; none of
// them assume the bar is open, and none of them need an event.
// ---------------------------------------------------------------------------
void term_search_open(void);     // open (or re-focus) the bar, keeping the last query
void term_search_close(void);    // close and restore the pre-search view
void term_search_toggle(void);
void term_search_next(void);     // advance to the next match, wrapping
void term_search_prev(void);     // back to the previous match, wrapping
void term_search_set_case_sensitive(int on);
void term_search_set_regex(int on);
void term_search_set_query(const char *q);

// ---------------------------------------------------------------------------
// Wiring. main.c (and term_pty.c's foreground resize) call these; everything
// here is a no-op or a pass-through while the bar is shut, so the terminal
// behaves exactly as it did before this module existed.
// ---------------------------------------------------------------------------
// THE ONE geometry entry point. Pass the window's FULL content size, exactly
// as EVENT_RESIZE reports it; this remembers it and calls term_handle_resize()
// with the bar's reservation already subtracted. Call it instead of calling
// term_handle_resize() directly from the event loop, or the bar's strip gets
// handed back to the grid on the next resize and the bar is drawn over.
void term_search_note_window(int content_w, int content_h);

// Feed EVERY EVENT_KEY_DOWN here FIRST, before the layout and before the
// shell's own key handling. The three-valued return is the same vocabulary
// term_select.h uses (PASS / REDRAW / TAKEN) and it is NOT cosmetic: a full
// term_layout_redraw_all() repaints every pane of the window with a TTF draw
// per cell, and doing that for every keystroke of a typed query is enough to
// overflow the per-window event queue and LOSE keys. MEASURED: nine characters
// injected as a burst lost the first one; the same nine typed with a gap did
// not. Only a query change needs the whole window; a caret move, a selection
// or a modifier transition needs the 26px bar and nothing else.
#define TERM_SEARCH_PASS    0   // not ours; the caller's own handling applies
#define TERM_SEARCH_REDRAW  1   // consumed, and the MATCHES changed: full repaint
#define TERM_SEARCH_BAR     2   // consumed, and only the BAR changed
int  term_search_key_event(const gui_event_t *ev);

// Window-local mouse press. Same three-valued return as the key hook.
int  term_search_mouse_down(int mx, int my);

// Advance the incremental scan. Call once per event-loop iteration (the idle
// branch is enough). Bounded by a wall-clock budget, so it can never stall the
// UI however deep the ring is; returns 1 if anything changed and a redraw is
// warranted. A no-op when the bar is shut or the scan is complete.
int  term_search_tick(void);

// Painted by term_layout_redraw_all() just before win_invalidate(): the bar
// itself, in the strip EVENT_RESIZE reserved for it. No-op when shut.
void term_search_overlay(void);

// ---------------------------------------------------------------------------
// RENDER HOOK, called from draw_row_cell() for every cell after the SGR
// colours are fully resolved and before the glyph is drawn - the SAME hook
// term_select_cell_colors() uses, deliberately, so there is one convention for
// "something outside the SGR model recolours this cell". Returns 1 and
// overwrites *fg/*bg when the cell is inside a match. `screen_row` is the
// destination row being painted, not a grid row: this file does the
// screen-row -> virtual-line mapping itself, so the renderer needs no new
// argument. Applied BEFORE the selection hook, so an explicit selection wins
// where the two overlap.
// ---------------------------------------------------------------------------
int  term_search_cell_colors(int screen_row, int col, uint32_t *fg, uint32_t *bg);

// Called once per term_redraw(), beside term_select_track().
void term_search_track(void);

// ---------------------------------------------------------------------------
// PANE OWNERSHIP (tabs/splits), the same rule term_select_note_pane() states:
// a find belongs to ONE pane, the one focused when the bar opened. term_layout
// calls this from tl_activate() with the pane whose state is being banked into
// the module globals; while that is not the owning pane the highlight is not
// drawn and the scan does not run, because those globals describe a different
// pane's grid and ring.
// ---------------------------------------------------------------------------
void term_search_note_pane(int pane);

// ---------------------------------------------------------------------------
// WHAT WAS DELIBERATELY NOT DONE HERE, so the next person does not have to
// rediscover it.
//
// Editor was NOT converted onto this bar. The honest reason is verification
// budget, not architecture: this pass had two build cycles for a terminal
// feature that has to be proved with typed input and screendumps, and
// converting Editor means re-proving Editor's find AND replace AND replace-all
// as well. The reusable half is already shared (textfield_t, gui_button,
// gui_scroll_reveal); what is left app-private here is genuinely
// terminal-shaped (cell grid + scrollback ring), so a future extraction of a
// gui_findbar widget would take the LAYOUT and the toggles from this file, not
// the matching. Recorded in CHANGELOG.md and blame.md rather than left as an
// unexplained second find bar.
//
// A match cannot span a wrapped line. The terminal stores a wrapped line as
// two independent grid rows with no continuation flag anywhere in
// term_grid.[ch], so there is nothing to join them by without restructuring a
// module this task explicitly consumes read-only.
// ---------------------------------------------------------------------------

#endif // TERM_SEARCH_H
