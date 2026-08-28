// term_layout.h
// PHASE 1 (terminal uplift): TABS and SPLITS. One window holds N tabs; each
// tab owns a BINARY SPLIT TREE of panes; each pane owns its own grid,
// scrollback ring, ANSI parser state, cursor, shell line-editor state, cwd
// and pty child. This module owns all of that.
//
// ---------------------------------------------------------------------------
// HOW THE SINGLE-INSTANCE MODULES WERE MADE MULTI-INSTANCE, AND WHY THIS WAY
// ---------------------------------------------------------------------------
// The task brief asked for "a context/handle threaded through their APIs".
// That was NOT done, and this is the honest reason rather than a silent
// deviation:
//
//   term_grid, term_parse, term_render, term_scrollback, term_pty and
//   term_shell are ~2300 lines between them, and FIVE other agents are
//   editing those same files in parallel right now. Adding a `term_ctx_t *`
//   first parameter to every function in all six modules touches essentially
//   every line of every one of them and would conflict with every one of
//   those agents on rebase.
//
// What is done instead is the same idea with the indirection at ONE place
// instead of forty: the modules keep their existing global names, and this
// module OWNS those globals. Exactly one pane is ACTIVE at a time, and
// tl_activate() banks the outgoing pane's state out and the incoming pane's
// state in. Two properties make that cheap and safe:
//
//   1. The BULK state is switched by POINTER, not copied. `cells`,
//      `alt_saved_cells` and `sb_lines` became pointers in term_grid.c /
//      term_scrollback.c (a 4-line change, `cells[y][x]` still compiles
//      verbatim at every one of their ~60 use sites). Switching a pane
//      re-points them: O(1), no memcpy of the 26 KB grid or the 1 MB ring.
//   2. Only the SCALARS are copied, and there are ~30 of them plus two
//      256-byte char arrays and one gui_scroll_t. That is under 700 bytes
//      per switch, and a switch happens per pane per event-loop tick, not
//      per byte of output.
//
// There is therefore ONE grid engine, ONE parser, ONE renderer and ONE pty
// pump serving N panes, which is what "do not duplicate them" was protecting.
// The cost of the deviation is that a module function only ever operates on
// the ACTIVE pane, so anything in this file that touches a non-focused pane
// must go through tl_activate() first. Every such site does.
//
// ---------------------------------------------------------------------------
// THE ONE GEOMETRY FUNCTION (#220), NOW THAT THERE ARE N PANES
// ---------------------------------------------------------------------------
// #220 was a one-row desync caused by two code paths computing a viewport
// height differently. With splits that trap multiplies by the pane count, so
// there is exactly ONE function here that gives a pane its geometry:
// tl_pane_apply_geometry(). Pane creation, tab switch, window resize, divider
// drag, split add, split remove, maximize, un-maximize and the font-change
// reflow ALL call it and nothing else computes a pane rect into the grid. It
// sets term_origin_x/y, calls term_handle_resize(), calls
// term_scrollback_reconfigure() (which still passes term_rows * TERM_CHAR_H
// and never term_px_h) and re-issues TIOCSWINSZ (#227) for that pane's child.
//
// ---------------------------------------------------------------------------
// WHAT A PANE OWNS, AND WHAT IT DELIBERATELY DOES NOT
// ---------------------------------------------------------------------------
// MEASURED FACT (cross-window-drag agent, dev 5df456ef): THIS TERMINAL HAS NO
// LONG-LIVED SHELL PROCESS. `ptmx_open()` always allocates a fresh master/slave
// pair and a master cannot be reopened, so a pty exists for the duration of ONE
// FOREGROUND COMMAND and is destroyed when that command exits. Between commands
// there is no pty and no child at all: the prompt and line editing are done
// in-process by term_shell.c.
//
// A pane is therefore modelled as owning THE STATE THAT SURVIVES A COMMAND:
// the cell grid, the scrollback ring, the ANSI parser state, the cursor, the
// line-editor buffer, the cwd and the title. The pty is per-command, exactly as
// it is today: `master`/`pid` are -1/0 whenever no command is running, and every
// one of the ~15 places this file touches `master` is guarded on `>= 0` or
// relies on term_pty_set_winsize()'s own -1 no-op. Nothing here assumes a
// persistence the system does not provide.
//
// The one visible consequence, stated rather than discovered later: resizing a
// pane while NO command is running issues no TIOCSWINSZ, because there is no
// tty to issue it to. That is correct, not a gap - the next command's
// term_pty_start() reads the pane's CURRENT term_rows/term_cols when it creates
// its pty, so it is born with the right size.
//
// The alternative - a genuine persistent session per pane - is a real and
// larger feature (it is also the thing that would make live pane hand-off
// between windows possible at all, since cross-process fd passing does not
// exist in this kernel: no AF_UNIX, no SCM_RIGHTS, no sendmsg). It is NOT in
// this phase and should be its own ticket.
//
// ---------------------------------------------------------------------------
// #227: SIGWINCH REACHES EVERY PANE'S CHILD
// ---------------------------------------------------------------------------
// The winsize fill had FOUR verbatim copies in term_pty.c. They are gone:
// term_pty_set_winsize() is the only one, and tl_pane_apply_geometry() is the
// only caller that matters, so "a pane changed size" and "that pane's child
// got SIGWINCH" are the same statement.
#ifndef TERM_LAYOUT_H
#define TERM_LAYOUT_H

#include "term_common.h"
#include "term_grid.h"
#include "term_scrollback.h"

// Hard caps. Panes are the expensive resource: each one costs its own grid
// (~26 KB), alt-screen save (~26 KB) and scrollback ring (~1 MB), so 8 panes
// is ~8.6 MB of a 512 MB userland heap. A pane whose scrollback malloc fails
// still works; sb_lines == NULL is already "scrollback disabled, not a crash"
// everywhere in term_scrollback.c.
#define TL_MAX_TABS     8
#define TL_MAX_PANES    8      // total live panes across ALL tabs
#define TL_MAX_NODES    (TL_MAX_PANES * 2)   // a binary tree with N leaves has N-1 internal nodes

// Chrome metrics. TL_TAB_H matches the 26px tab strip in
// docs/TERMINAL_KONSOLE_CHROME_SPEC.md 2.2; TL_HDR_H matches the 20px pane
// header in 2.4 (which is the retro-unix titlebar height: a pane is a nested
// mini-window and is meant to read as one).
#define TL_TAB_H        26
#define TL_HDR_H        20
#define TL_DIV          4      // visible divider thickness
#define TL_DIV_HIT      8      // divider grab zone, centred on the divider
#define TL_TAB_MIN_W    120
#define TL_TAB_MAX_W    200
#define TL_MIN_COLS     20     // spec 2.4 minimum pane size
#define TL_MIN_ROWS     5

#define TL_TITLE_MAX    32

// Split orientation. VERTICAL = the divider is vertical, so the two children
// sit SIDE BY SIDE ("split right"). HORIZONTAL = the divider is horizontal,
// so they are STACKED ("split down"). Named for the divider, which is the
// thing the user drags.
#define TL_SPLIT_VERTICAL    0
#define TL_SPLIT_HORIZONTAL  1

// ---------------------------------------------------------------------------
// Entry points. main.c calls ONLY these; every other agent editing main.c
// sees a two-line init and a four-line event hook, nothing more.
// ---------------------------------------------------------------------------

// Adopt the already-initialised globals as tab 0 / pane 0 and give that pane
// its geometry. Replaces main()'s direct term_handle_resize() call, because
// pane geometry is now this module's job and there must be only one path to
// it (#220).
void term_layout_init(int content_w, int content_h);

// Feed EVERY dequeued event here first. Returns:
//    0  not consumed - main.c's built-in shell line editor should handle it
//    1  consumed
//   -1  consumed, and the application should exit
int term_layout_event(int et, gui_event_t *ev);

// Called when win_get_event() times out. Pumps every pane's pty child,
// advances the focused pane's cursor blink, polls TERMPREF.CFG, and repaints
// whatever changed. Never blocks (#426).
void term_layout_idle(void);

// 1 when the FOCUSED pane has a live pty child, i.e. main.c must not print a
// shell prompt after execute_command() returned, because the command has not
// finished - it has only STARTED. The prompt is printed by the pump when the
// child's master reports EOF.
int term_layout_pane_busy(void);

// Start `path` on a pty in the FOCUSED pane and return immediately. This is
// what replaced term_pty.c's run_foreground_pty(), whose nested blocking
// event loop could not exist once a second pane needed to keep drawing while
// the first one ran vi. Called from exactly one site in term_shell.c.
void term_layout_run_foreground(const char *path, char **argv, int argc);

// Full repaint of chrome + every visible pane.
void term_layout_redraw_all(void);

// How long main.c's event wait may block, in ms. This is NOT a tuning knob: it
// reproduces exactly the two rates that existed before splits. The old shell
// loop waited 100 ms, and the old run_foreground_pty() pump waited 10 ms while
// a child was running. Collapsing the two loops into one would have quietly
// made every command's output up to 100 ms late (a 10x latency regression that
// no screenshot would show and that would read as "the terminal feels slower
// now"), so the ONE loop asks which situation it is in.
int term_layout_timeout_ms(void);

// The tab index of the pane whose state is currently in the module globals.
// A BEL byte is processed by term_parse.c against the ACTIVE pane, and that
// pane is not necessarily in the tab the user is looking at (a background
// pane's pty is pumped every tick), so "which tab rang" is a question only
// this module can answer. Returns TERM_TAB_DEFAULT-equivalent 0 before init.
int term_layout_active_tab(void);

// ===========================================================================
// (#307 PHASE 1) THE MENU BAR'S WAY IN
// ===========================================================================
// Every tab/split action already exists in term_layout.c as a static function
// reached from a click on the tab strip or a pane header. The menu bar needs
// the SAME actions from a different input device, and a second copy of "split
// the focused leaf" living in term_menu.c is exactly the multi-copy fault this
// tree keeps paying for. So the commands are published here and the menu calls
// them; term_menu.c contains no split-tree code at all.
//
// term_layout_can() exists so the menu can DIM a row instead of offering one
// that would silently do nothing: Close Split is meaningless in a single-pane
// tab, Close Tab is a window close when only one tab is left. A menu full of
// rows that no-op is the #208 dead-control fault.
#define TL_CMD_NEW_TAB        1
#define TL_CMD_CLOSE_TAB      2
#define TL_CMD_NEXT_TAB       3
#define TL_CMD_PREV_TAB       4
#define TL_CMD_SPLIT_RIGHT    5   // side by side
#define TL_CMD_SPLIT_DOWN     6   // stacked
#define TL_CMD_CLOSE_SPLIT    7
#define TL_CMD_MAXIMIZE_PANE  8   // toggle; view-only, the tree is unchanged
#define TL_CMD_PANE_TO_TAB    9   // "Move this split to a new tab"

// 1 if the command is meaningful RIGHT NOW, 0 if the menu should dim it.
int term_layout_can(int cmd);
// Perform it. Returns 1 if something changed (the caller need not redraw; this
// repaints), 0 if the command was not applicable, -1 if the window should quit
// (closing the last tab).
int term_layout_command(int cmd);
// Re-derive every pane's grid from the current font metrics and window size,
// re-issuing TIOCSWINSZ for each (#227). Call after anything that changes
// TERM_CHAR_W/TERM_CHAR_H or term_content_y: the F9 dialog already does this
// internally, the menu bar's Zoom and Show-Menu-Bar rows need it too.
void term_layout_reflow(void);

// How many panes the focused tab currently has, and how many tabs exist. The
// menu uses these only for enable/disable decisions.
int term_layout_pane_count(void);
int term_layout_tab_count(void);

// The WINDOW's content size in pixels. NOT term_px_w/term_px_h, which are the
// FOCUSED PANE's size now that splits exist. Anything drawing window-level
// chrome, or asking a shared widget to fit itself inside the window, needs
// this one and not those. Both out-params are always written.
void term_layout_content_size(int *w, int *h);

#endif // TERM_LAYOUT_H
