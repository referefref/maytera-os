// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_filedlg.h - THE shared file Open/Save chooser, with a Places sidebar.
//
// WHERE THIS CAME FROM, and what it is for. This is the chooser that lived
// inside userland/officelib/officeui/officeui.c as `oapp_open_dialog_titled()`
// and `oapp_save_dialog()`. Writer, Calc and Slides already shared it, which is
// the right shape, but it was reachable ONLY by an app built on the office
// shell, so every other app that needs to choose a file grew its own. Measured
// in the tree on 2026-09-30: four more fully navigable browsers
// (apps/paint/ui.c fb_*, apps/gallery, apps/imgconv, apps/ide) plus about ten
// flat single-directory pickers, no two of which agree about where to start,
// and three different idioms for "where is home" (userhome_root(),
// userhome_path(), and a raw getpwuid()->pw_dir with a "/HOME/ADMIN" fallback).
//
// So the chooser moved down here, next to gui_confirm.c and gui_list.c, where
// any app can call it, and officeui's three functions became wrappers. It is
// the same modal convention the rest of this codebase uses: never a separate
// window, never click-away-dismissable, closed only by an explicit button,
// Enter or Esc. BLOCKING: each call runs its own nested event loop on the
// host's window and returns when the user resolves it.
//
// KEYBOARD FIRST (#334). Headless mouse injection cannot reliably drive this
// UI, so every path works with no mouse: Tab moves focus between the file list,
// the Places sidebar and (in Save) the filename field; Up/Down/PgUp/PgDn/Home/
// End move within whichever has focus; Enter activates; Esc cancels.
#ifndef _GUI_FILEDLG_H
#define _GUI_FILEDLG_H

#include "contract.h"

// What the dialog needs from the app hosting it. It draws into the host's
// window and must be able to repaint the host underneath itself, because there
// is no framebuffer read-back on this system: an overlay cannot be undrawn, it
// can only be painted over.
typedef struct {
    int win;                                    // the host's window handle
    void *ctx;                                  // passed back to every callback
    // The host window's CONTENT size. A callback rather than two ints because
    // the window can be resized while the modal is up, and a stale size centres
    // the card in the wrong place.
    void (*size)(void *ctx, int *w, int *h);
    // Repaint the host's own frame. May be NULL, in which case the area under
    // the card keeps whatever was last drawn there.
    void (*repaint)(void *ctx);
    // Tell the host its window was resized, so its own layout updates before
    // repaint() is called. May be NULL.
    void (*note_resize)(void *ctx, int w, int h);
    // The host app's OWN contract, or NULL. APPENDED AT THE END deliberately:
    // every existing initialiser of this struct is a braced list, so a field
    // added here is zero and a field added anywhere else would shift a function
    // pointer into the wrong slot and compile cleanly (the same reasoning that
    // put ct_item_t.terms at the end of ITS struct).
    //
    // WHEN IT IS SET, THE MODAL BECOMES DRIVABLE. filedlg_open()/filedlg_save()
    // run their own NESTED event loop, so while one is up the host's loop is not
    // running and its contract_live_poll() is not being called: a contract
    // request sent to the app sits in the mailbox until the human resolves the
    // dialog, which is the one moment a caller most needs to reach it. With a
    // contract here the nested loop serves the mailbox itself, using a bounded
    // 100 ms event timeout instead of blocking forever. With NULL it blocks
    // exactly as it does today, so a caller that does not opt in is unchanged.
    const ct_contract_t *contract;
} filedlg_host_t;

// Open: pick an EXISTING file. `exts` is concatenated dot-prefixed tokens, e.g.
// ".docx.odt"; NULL or "" matches everything. `title` may be NULL for a default.
// Returns an absolute path valid until the next call, or NULL if cancelled.
const char *filedlg_open(const filedlg_host_t *host, const char *exts, const char *title);

// Save: choose a directory and type a name. `defname` pre-fills the field.
// Returns an absolute path valid until the next call, or NULL if cancelled.
const char *filedlg_save(const filedlg_host_t *host, const char *defname, const char *title);

// ---------------------------------------------------------------------------
// THE CONTRACT FRAGMENT (docs/CONTRACT_COVERAGE_PLAN.md task 3, phase 1).
//
// The chooser is not an app, so it has no ct_contract_t of its own: there is no
// /APPS/FILEDLG to spawn and no process to answer --contract. What it has is a
// SURFACE, and that surface belongs in the contract of whichever app is hosting
// it, because that is the process a caller can actually reach.
//
// So this is a FRAGMENT: a contiguous block of contract rows any host splices
// onto the end of its own projection, in two lines.
//
//     static int myapp_project(int idx, ct_item_t *out) {
//         if (idx < MY_N) { ...my own rows...; return 1; }
//         return filedlg_contract_at(idx - MY_N, out);
//     }
//
// WHY A FRAGMENT AND NOT A SECOND CONTRACT. Writer, Sheets and Slides all reach
// this chooser through officelib/officeui, and four more apps grew their own
// browser before it moved down here (apps/paint/ui.c, gallery, imgconv, ide).
// A per-host description of "the Open dialog" would be one copy per host of a
// surface that is genuinely shared, which is the #235 shape: a description that
// is a COPY of the thing it describes, going stale one host at a time. One
// fragment, projected from the SAME FD_VERBS[] table the key handler dispatches
// through and the SAME places.h model the sidebar draws, cannot drift from any
// of them.
//
// HONEST STATE OF THE WIRING, measured: ZERO of the chooser's consumers declares
// a ct_contract_t today (the seven contracted apps are calc, convert, paint,
// settings, snapshot, terminal and timers, none of which uses this chooser), so
// the fragment ships with no host on a golden image. Wiring Writer / Sheets /
// Slides is item 2 of the coverage plan's task 3 and is a separate change: their
// menus dispatch by numeric command ID through gui_menu_item_t, which carries no
// function pointer, so contracting them means converting that dispatch first.
// The fragment is verified through a throwaway host, not through a shipped one.
//
// WHERE MODAL BELONGS, and it is NOT in this fragment. Every row here is
// CT_TF_LIVE_ONLY: each one moves a selection or a folder view inside a dialog
// that is ALREADY up, and each returns immediately, so none of them blocks a
// caller. The thing that genuinely blocks is filedlg_open() itself, which runs a
// nested event loop until a person answers it.
//
// That call belongs to the HOST, not to this fragment, and deliberately: only the
// host knows WHY it is raising a chooser (open which document, save which one,
// with which extensions and which default name), and re-entering the chooser from
// inside a contract call served by the chooser's own nested loop would be
// recursion. So the fragment deliberately offers no "open the dialog" row.
//
// A HOST THAT DECLARES ONE MUST DECLARE IT CT_TF_MODAL. contract.c then REFUSES
// it on the spawn path, where there is no window and nobody to answer, and ALLOWS
// it when serving a live instance, which has a bounded wait. Declaring it fast
// would not make it fast: it would hand a spawned caller an unbounded
// sys_waitpid() on a dialog waiting for a human who is not there.
int filedlg_contract_count(void);
int filedlg_contract_at(int idx, ct_item_t *out);

#endif
