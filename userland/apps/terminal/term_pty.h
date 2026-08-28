// term_pty.h
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.
#ifndef TERM_PTY_H
#define TERM_PTY_H

#include "term_common.h"

// The pty master fd of the CURRENTLY running foreground child (#586's
// run_foreground_pty), so a Device Status Report (CSI n) has somewhere to
// write its reply. -1 when no foreground child owns the terminal (the
// built-in shell prompt never receives DSR queries, so this is enough).
extern int g_active_master_fd;

// Translate a window key event into the byte(s) a tty program expects on
// stdin. Returns the count written into out[] (0 = drop this key).
int key_event_to_bytes(const gui_event_t *ev, char *out);
// #227: THE winsize fill, the only one in the app. Four verbatim copies of it
// lived in term_pty.c and each was a place a future pane could be forgotten.
// A master of -1 is a no-op.
void term_pty_set_winsize(int master, int rows, int cols);

// PHASE 1 (tabs/splits): run_foreground_pty() was REMOVED. It owned a nested
// event loop that ran until the child exited, which froze every other pane in
// a split. The three phases below are what the one event loop in main.c needs
// instead; term_layout.c is the only caller and activates the owning pane
// first, so "the active pane" in each of these is always the child's own pane.
//
// Start `path` on a pty. Returns the child pid (>0) with *out_master set, or
// <=0 having already reported the failure into the grid. Never blocks.
int  term_pty_start(const char *path, char **argv, int argc, int *out_master);
// Drain available child output into the ACTIVE pane. 1 = something was drawn.
// *alive is set to 0 on EOF. Never blocks (#426).
int  term_pty_drain(int master, int *alive);
// Reap the child, close the master, and reset the ACTIVE pane's rendition /
// scroll region / alternate screen for the shell prompt.
// Returns the child's raw exit status; the caller decides what to do with it
// (term_layout.c turns it into a term_notify_cmd_end() for the right TAB).
int  term_pty_finish(int master, int pid);

#endif // TERM_PTY_H
