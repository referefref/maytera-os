// term_notify.h
// PHASE 1 (terminal uplift): Konsole-class terminal NOTIFICATIONS.
//
// This module decides WHEN the terminal should ask for the user's attention,
// and it asks through the notification system the OS already has: every toast
// below is one notify_post() call (userland/libc/notify.h), spooled to
// /CONFIG/NOTIFY.TXT and rendered by the compositor's notif.c (themed toast,
// tray bell badge, /CONFIG/ALERTS.CFG do-not-disturb). There is deliberately
// no second toast/queue/renderer in here; the only pixels this file owns are
// the in-window attention pip and the visual bell, both of which are terminal
// chrome rather than system notifications.
//
// THE JUDGEMENT THIS FILE ENCODES, stated once, here, because a notification
// that fires too often is a notification nobody reads:
//
//   * A TOAST is expensive attention. It fires only when the user cannot
//     already see the answer: the terminal window is minimized, hidden, or
//     not the focused window ("unattended"), or the tab is a background tab.
//     If the terminal is on screen and focused, `ls` finishing, a bell, and a
//     burst of output are all things the user is already looking at, so none
//     of them toast.
//   * A MARK (the pip / the tab-strip dot) is cheap attention. It costs a few
//     pixels inside the terminal's own window and interrupts nothing, so it
//     is set whenever an unattended tab produces output or rings, regardless
//     of whether the matching toast monitor is enabled.
//
// Defaults follow Konsole's: "Notify on finish" and bell notifications ON,
// "Monitor for activity" and "Monitor for silence" OFF, because those two are
// per-session opt-ins in Konsole for exactly the noise reason above.
#ifndef TERM_NOTIFY_H
#define TERM_NOTIFY_H

#include "term_common.h"

// Overridable at runtime from this flat file, re-read on a throttle so an
// edit (or a future preferences page) applies to an already-open window
// without a restart. Same "poll a flat file, no filesystem watcher exists"
// idiom as term_prefs_poll_reload() and the compositor's SETTINGS.CFG poll.
//
// THE REAL PATH IS <home>/CONFIG/TERMNOTI.CFG, resolved by userconf_* like
// every other per-user preference; the constant below is only the LEGACY
// read fallback userconf_open_read() takes when the per-user copy is absent.
// For root, and for any account whose passwd home is "/", the two are the
// same string, which is why it reads as an absolute path here. Do not open
// this constant directly.
//
// The file is the DEFAULT; the per-tab monitor switches are the override.
// A change to the file is re-applied to every open tab (see
// cfg_apply_to_tabs); a per-tab toggle survives until the file changes.
#define TERM_NOTIFY_CFG "/CONFIG/TERMNOTI.CFG"

// Tabs do not exist yet (term_layout.[ch], separate agent). Everything below
// is keyed by tab index so that landing tabs is a WIRING change here, not a
// rewrite: today the terminal is one tab, TERM_TAB_DEFAULT.
#define TERM_NOTIFY_MAX_TABS 16
#define TERM_TAB_DEFAULT      0

// Attention marks, in ascending precedence. A bell outranks activity: if a
// background tab both produced output and rang, the tab shows the bell mark.
#define TERM_MARK_NONE      0
#define TERM_MARK_ACTIVITY  1
#define TERM_MARK_SILENCE   2
#define TERM_MARK_BELL      3

// Per-tab monitor toggles. Konsole's "Monitor for ..." menu, same names.
#define TERM_MON_FINISH     0
#define TERM_MON_ACTIVITY   1
#define TERM_MON_SILENCE    2
#define TERM_MON_BELL       3
#define TERM_MON_COUNT      4

// Call once, after win_create() has assigned window_handle.
void term_notify_init(void);

// Call from every loop that already has an idle branch (main.c's event loop
// and term_pty.c's foreground pump). Returns 1 if something the terminal
// PAINTS changed (a visual bell started or expired, a mark appeared or was
// cleared), i.e. the caller should term_redraw().
//
// #426: this function NEVER blocks and NEVER sleeps. Everything it does is a
// non-blocking read of state the kernel already holds, gated behind its own
// millisecond throttles off uptime_ms(); it adds no wait of any kind to the
// draw path. It is called from loops that are already parked in
// win_get_event()'s per-window wait queue, so it introduces no new poll.
int term_notify_tick(void);

// --- Window attention state -------------------------------------------------
// Cached WIN_STATE_* bitmask for this terminal's own window, refreshed by
// term_notify_tick(). -1 until the first successful read.
int term_notify_window_state(void);
// 1 = the user can see this window and it has focus. 0 = minimized, hidden,
// or behind/unfocused. This is the predicate every toast rule is built on.
int term_notify_window_attended(void);
// 1 = this terminal's window is minimized right now (the owner's explicit ask).
int term_notify_window_minimized(void);

// --- Events fed in by the rest of the terminal -------------------------------
// A foreground child was successfully spawned on the pty. `program` is a path
// or a bare name; only the basename is kept.
void term_notify_cmd_start(int tab, const char *program);
// That child was reaped. `exit_status` is what sys_waitpid() wrote.
void term_notify_cmd_end(int tab, int exit_status);
// A BEL byte (0x07) reached the parser outside an escape sequence.
void term_notify_bell(int tab);

// #221: which pane the module globals (term_origin_x/y, term_px_w/h) currently
// describe. tl_activate() calls this exactly the way it already calls
// term_select_note_pane() and term_search_note_pane(). Without it the visual
// bell had no way to know which pane rang, and drew a PANE-SIZED rectangle at
// the WINDOW origin: correct only in the one case with no splits and no
// chrome, wrong the moment a split existed.
void term_notify_note_pane(int pane);
// One or more bytes of child output were rendered into `tab`.
void term_notify_output(int tab);

// --- Tab wiring: term_layout.[ch] calls these once tabs land -----------------
void term_notify_set_focused_tab(int tab);
int  term_notify_focused_tab(void);
// What the tab strip should draw next to this tab's title (TERM_MARK_*).
int  term_notify_tab_mark(int tab);
// Clear a tab's mark, e.g. because the user just switched to it.
void term_notify_tab_clear(int tab);
// 1 if ANY tab currently carries a mark (what the single-tab pip draws off).
int  term_notify_any_mark(void);
void term_notify_set_monitor(int tab, int monitor, int on);
int  term_notify_get_monitor(int tab, int monitor);

// --- Chrome ------------------------------------------------------------------
// Called from the tail of term_redraw(). Draws the visual bell frame and the
// attention pip. Draws nothing at all when neither is active, so an idle
// terminal spends no pixels on this.
void term_notify_paint_overlay(void);

#endif // TERM_NOTIFY_H
