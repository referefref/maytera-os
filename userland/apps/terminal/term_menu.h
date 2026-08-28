// term_menu.h - the Terminal's Konsole-shaped MENU BAR (terminal uplift
// PHASE 1). See docs/TERMINAL_MODULES.md for the module map this slots into
// and docs/TERMINAL_KONSOLE_CHROME_SPEC.md section 2.1 for the design.
//
// ===========================================================================
// THE ONE RULE THIS FILE IS BUILT AROUND: NO DEAD ENTRIES
// ===========================================================================
// Every row in every menu below invokes a function that already exists and
// already does the thing. #208 is an open ticket class in this tree for
// controls that are drawn but do nothing, and a menu is the easiest place in a
// UI to accumulate them.
//
// The rule has two halves, and they are different:
//
//   ABSENT. A capability nothing in the app has. Find is the only one left:
//   `term_search.[ch]` has not landed, so there is no Find row. Not stubbed,
//   not permanently greyed, simply not there until that module publishes a
//   header. When it does, add the row and call its API.
//
//   DIMMED, TRANSIENTLY. A real command that has no meaning in the CURRENT
//   state: Close Split in a single-pane tab, Copy with nothing selected,
//   `cd` into a bookmark while `vi` owns the terminal. Those rows dim and
//   absorb the click exactly as a native menu does, and come back the instant
//   the state changes. tm_apply_context() is the one place that decides, and
//   for the layout rows it ASKS term_layout_can() rather than re-deriving the
//   rule, so the menu and the pane-header buttons can never disagree.
//
// ===========================================================================
// THE WIDGET IS NOT OURS, AND NEITHER IS THE SPLIT TREE
// ===========================================================================
// The bar, the popup, the scrolling popup, the hit-testing, the keyboard
// navigation and the disabled-row semantics are all userland/libc/gui_menu.h
// (#562/#512), the same instance-of-one-widget Editor uses. This file contains
// no hit-test, no popup geometry and no dropdown drawing.
//
// Tabs and splits are term_layout's; selection and the clipboard are
// term_select's; notification monitors are term_notify's; profiles are
// term_profile's. Every row here is a call into one of those. The three things
// gui_menu was missing were added TO THE SHARED WIDGET (a checked/radio mark,
// an Alt mnemonic, and clamping the popup to the window) rather than worked
// around locally, so Editor and every future adopter get them too.
//
// ===========================================================================
// GEOMETRY, AND WHY #220 CANNOT COME BACK THROUGH THIS DOOR
// ===========================================================================
// The bar occupies the top TERM_MENU_BAR_H pixels of the window content area.
// It does NOT take a row off any grid. It sets term_content_y (term_grid.h),
// and term_layout's ONE geometry function (tl_relayout) starts its work there:
// the tab strip and every pane rect already include the inset, so nothing
// downstream adds it a second time. Each pane still gets an explicit rect from
// tl_pane_apply_geometry(), and the viewport HEIGHT handed to
// gui_scroll_config() is still the row-aligned term_rows * TERM_CHAR_H, which
// is the invariant #220 is about.
#ifndef TERM_MENU_H
#define TERM_MENU_H

#include "term_common.h"

// 24px, the same number as the Editor's MENU_HEIGHT. Deliberately the same
// value and not a coincidence: the two apps' chrome has to read as one system
// (docs/TERMINAL_KONSOLE_CHROME_SPEC.md 2.1).
#define TERM_MENU_BAR_H 24

// Build the menu tables (including the live colour-scheme, profile and
// bookmark lists), reserve term_content_y, and register the chrome draw hook.
// Call ONCE, after win_create() has assigned window_handle and BEFORE
// term_layout_init(), so the layout's first pass already has the inset.
void term_menu_init(void);

// Re-read anything the menu displays that lives outside it: the colour-scheme
// list, the profile list, the bookmark file, and the chrome palette. Call
// after the F9 dialog or a live config reload.
void term_menu_refresh(void);

// THE SINGLE ENTRY POINT. Give the menu first refusal on every event, BEFORE
// term_layout_event(). Returns 1 if the menu consumed it, in which case the
// caller must not pass it on to the layout, a pane, or the pty. 0 means "not
// mine". Cheap for the events it does not want.
//
// ARGUMENT ORDER NOTE for anything else added here: gui_menu's coordinate
// pairs are (X, Y), in that order, and both are ints. gcc accepts them swapped
// in silence and no lint in this tree checks it; the only symptom is a
// hit-test that misses. The same trap rendered a badge at 113pt elsewhere in
// this codebase when gui_text_ttf_centered(s, color, size) was called
// (s, size, color). Read the header, do not copy a call site.
int term_menu_event(const gui_event_t *ev);

// Call from the main loop's IDLE branch. Heals the menu state when the window
// has lost focus (this kernel sends no blur event; see the implementation).
// Returns 1 if a redraw is needed. Costs two int compares when there is
// nothing open and nothing hovered, which is almost always.
int term_menu_tick(void);

// True once File > Close Window (or Exit) has been chosen.
extern int term_menu_quit;

// 1 while a foreground child owns the FOCUSED pane. Dims the shell-only rows.
void term_menu_set_pty(int active);

#endif // TERM_MENU_H
