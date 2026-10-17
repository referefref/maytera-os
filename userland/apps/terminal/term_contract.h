// term_contract.h - the Terminal's tool contract (#233/#241), PROJECTED.
//
// WHAT MOVED HERE AND WHY. The contract used to live at the bottom of
// term_prefs.c as eight hand-written static rows, with a comment admitting that
// "contract-lint's structural checks today are scoped to settings.c/calc.c, so
// this is the terminal's OWN honest, if not yet structurally-enforced,
// contract". It is enforced now, and the surface it describes is no longer eight
// preferences: it is every shell builtin, every menu command, the live colour
// scheme / profile / bookmark lists, the profile store's own edit operations, and
// the nine TERMNOTI.CFG keys that had no control of any kind. That is a contract
// in its own right rather than a tail on the preferences file.
//
// THE RULE THIS FILE OBEYS: PROJECT, DO NOT HAND-WRITE ROWS. Every ACTION is
// read out of the table the app already dispatches it from:
//
//   g_builtins[]      term_shell.c    -> term_shell_ct_project()
//   TM_ACTS[]         term_menu.c     -> term_menu_ct_project()
//   g_schemes[]       term_menu.c     -> the live /PALETTES list
//   g_term_profiles[] term_profile.c  -> the live TERMPROF.CFG list
//   g_bookmarks[]     term_menu.c     -> the live TERMBOOK.CFG list
//   PROF_ACTS[]       term_contract.c -> the profile store's edit operations
//
// so an action that is not in one of those tables can be neither run nor
// described. TERMINAL_ITEMS[] keeps only what a table could not carry: VALUES
// with a backing variable and READABLE STATE, which is exactly what static rows
// are for, and whose terms contract_terms_of() DERIVES rather than declaring.
#ifndef TERM_CONTRACT_H
#define TERM_CONTRACT_H

#include "term_common.h"

// The terminal.* tool contract, for `terminal --contract ...` and for
// contract_invoke()/contract_invoke_live() from any other process.
extern const ct_contract_t TERMINAL_CONTRACT;

// ---------------------------------------------------------------------------
// THE LIVE GUARD.
//
// 1 once this process has created its window and cleared its grid. A process
// started with --contract answers and exits BEFORE win_create(), so it never
// sets this, which is what every CT_TF_LIVE_ONLY row's handler refuses on: those
// commands act on this window's tabs, panes, grid and chrome, and a process with
// none of those has nowhere to put the work.
//
// DELIBERATELY NOT contract_serving_live(), which answers a narrower question
// ("is the engine currently serving a mailbox request"). What a handler needs to
// know is whether there is a WINDOW, and the interactive click path must take
// the same branch as a live contract call, which contract_serving_live() would
// not give it.
// ---------------------------------------------------------------------------
void term_contract_note_live(void);
int  term_ct_live(void);

// Shared refusal for a CT_TF_LIVE_ONLY row reached without a live window.
// Returns 1 and fills out[] when there is nothing to act on, 0 otherwise.
int  term_ct_no_live(char *out, int ocap);

// RISK IS DERIVED FROM THE ROW'S DECLARED DEPTH, not written out per row, for
// the same reason contract_terms_of() derives a value row's terms: a
// hand-written second field is a second thing that can disagree with the first,
// and the disagreement is what #235 was made of. A control that starts another
// process or reaches the filesystem outside this app's own config is GUARDED and
// goes to the aicap gate (the SAME gate the AI tool loop uses); a control that
// only changes this window or this app's own preferences is SAFE. So a row
// cannot be marked safe while declaring that it launches a program, because
// nobody gets to mark it at all.
void term_ct_apply_risk(ct_item_t *it, const ct_terms_t *t);

#endif // TERM_CONTRACT_H
