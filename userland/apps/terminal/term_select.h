// term_select.h
// PHASE 1 (terminal uplift, #221): SELECTION AND CLIPBOARD for the Terminal.
// New module. Consumes term_grid / term_scrollback / term_emu READ-ONLY.
//
// ===========================================================================
// THE CLIPBOARD QUESTION, ANSWERED BEFORE ANY CODE WAS WRITTEN
// ===========================================================================
//
// MayteraOS ALREADY HAS ONE SHARED, OS-WIDE CLIPBOARD and this module uses it
// rather than growing a terminal-local twin. It is kernel-held (#542,
// SYS_CLIP_SET/GET/LEN), a single bounded 64 KiB buffer with no per-process
// state, reached from userland through clipboard_set()/clipboard_get()/
// clipboard_len() in userland/libc/syscall.h. Its cross-process behaviour is
// not assumed: userland/apps/cliptest/main.c is a two-process round-trip
// test, and userland/libc/textfield.h already routes every text field's
// Ctrl+C/X/V through the same three calls. So a copy here pastes into Editor,
// Files, Paint and any text field, and theirs pastes in here, with no new
// primitive and no second store to keep in sync.
//
// ===========================================================================
// WHAT THIS FILE DELIBERATELY DOES NOT OWN
// ===========================================================================
//
// THE DEC PRIVATE MODES ARE term_emu's. ?2004 (bracketed paste) and
// ?1000/?1002/?1003/?1006 (mouse reporting) live in `g_term_modes`
// (term_emu.h) and are read here through term_emu_bracketed_paste() and
// term_emu_mouse_reporting(). There is no copy in this file and there must
// never be one: selection and reporting disagreeing about who owns a click is
// a bug nobody finds by reading either file alone, which is exactly why
// term_emu.h says so at its own declaration.
//
// THE MOUSE REPORT ITSELF IS term_layout's. term_mouse_report() already
// encodes and writes it (X10 or SGR, per-cell motion coalescing, the
// g_active_master_fd gate). This file decides ONLY who the event belongs to,
// and says so by returning TERM_SEL_PASS.
#ifndef TERM_SELECT_H
#define TERM_SELECT_H

#include "term_common.h"
#include "term_grid.h"

// ---------------------------------------------------------------------------
// Return codes. Distinguishing "consumed" from "consumed AND the screen
// changed" matters: with ?1003 (any-event tracking) every pointer motion
// produces a report, and repainting a pane for each one would make a
// mouse-driven TUI unusable.
// ---------------------------------------------------------------------------
#define TERM_SEL_PASS    0   // NOT OURS. The caller's own handling applies:
                             // for a mouse event that means term_mouse_report().
#define TERM_SEL_REDRAW  1   // consumed, and the caller must repaint the pane
#define TERM_SEL_TAKEN   2   // consumed, nothing on screen changed

// ---------------------------------------------------------------------------
// EDIT-MENU / SHORTCUT VERBS. The menu-bar agent's Edit menu calls these
// directly; so do the Ctrl+Shift+C / Ctrl+Shift+V / Ctrl+Shift+A shortcuts
// inside term_select_handle_key(). Every one is safe to call with no
// selection and with an empty clipboard.
// ---------------------------------------------------------------------------
int  term_select_have(void);            // 1 if a non-empty selection exists
int  term_select_copy(void);            // -> system clipboard; bytes stored, 0 = nothing
int  term_select_paste(void);           // system clipboard -> pty, or the shell line
void term_select_all(void);             // whole scrollback + live screen
void term_select_clear(void);
// Copy the selection into `out` WITHOUT touching the clipboard, NUL
// terminated. Returns the length. For Find-in-buffer and anything else that
// wants the text but must not clobber what the user has copied.
int  term_select_get_text(char *out, int cap);

// ---------------------------------------------------------------------------
// EVENT PLUMBING, called from term_layout_event().
//
// term_select_handle_mouse() is where mouse reporting and selection are
// ARBITRATED, so they cannot both claim the same click. It returns
// TERM_SEL_PASS when the application owns the mouse, which is the caller's
// signal to call term_mouse_report(). Holding SHIFT overrides that and gives
// the click to selection, which is the convention xterm, Konsole and
// gnome-terminal all use and which term_emu.h already writes down.
//
// Call it AFTER the tab strip, pane headers, dividers and the shared
// scrollbar widget have each had their chance, so none of them lose a click.
// ---------------------------------------------------------------------------
int  term_select_handle_mouse(const gui_event_t *ev, int etype);
// Does the APPLICATION own the mouse right now: reporting enabled, a child to
// report to, and Shift NOT held? Exported so the wheel path can ask the same
// question without a second copy of the Shift rule.
int  term_select_app_owns_mouse(void);
int  term_select_handle_key(const gui_event_t *ev);

// ---------------------------------------------------------------------------
// RENDER HOOK. Called from draw_row_cell() for every cell, after the SGR
// colours have been fully resolved (reverse video, bold-brightening and dim
// included) and before the glyph is drawn. Returns 1 and overwrites *fg/*bg
// with the colour scheme's selection colours when this cell is selected.
// `screen_row` is the destination row being painted, NOT a grid row: this
// file does the screen-row -> virtual-line mapping itself so the renderer
// needs no new argument.
// ---------------------------------------------------------------------------
int  term_select_cell_colors(int screen_row, int col, uint32_t *fg, uint32_t *bg);

// ---------------------------------------------------------------------------
// PANE OWNERSHIP (tabs/splits). A selection belongs to ONE pane. term_layout
// calls this with the pane index before it draws a pane and before it routes
// a mouse event; while the current pane is not the owner the highlight is not
// drawn and a drag cannot extend the old selection, but the selection is NOT
// destroyed, so switching back to its pane shows it again. A press in another
// pane re-owns it, which is what starting a new selection means.
// ---------------------------------------------------------------------------
void term_select_note_pane(int pane);

// ---------------------------------------------------------------------------
// INVALIDATION. #220 is the standing warning that geometry in this app is
// where the bugs are, and selection coordinates ARE geometry.
//
// Anchors are stored as (VIRTUAL LINE, column), the same virtual-document
// coordinate gui_scroll_first_item() returns: line 0 is the oldest retained
// scrollback line, line sb_count is the first live row. That choice is what
// makes two of the three required invalidations free:
//
//   SCROLLING       moves term_scroll_view.offset and changes nothing else,
//                   so the selection stays on the same characters. No work.
//   NEW OUTPUT      a row leaving the live grid for scrollback keeps its
//                   virtual line exactly (sb_count grows by one and its live
//                   row index shrinks by one). No work, UNTIL the ring is
//                   full, at which point sb_head advances and EVERY virtual
//                   line drops by one; term_select_track() detects that from
//                   sb_head and slides the anchors, the same compensation
//                   term_history_push() already applies to the scroll offset.
//   RESIZE          the grid does NOT reflow text (term_handle_resize() only
//                   truncates or extends rows), so a selection made at the
//                   old width no longer describes the same characters at the
//                   new one. There is no correct remap, so it is CLEARED,
//                   which is what xterm does for the same reason.
// ---------------------------------------------------------------------------
void term_select_on_resize(void);
void term_select_track(void);   // called once per term_redraw()

#endif // TERM_SELECT_H
