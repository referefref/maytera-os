// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// cardfile_host.h - the Cardfile WINDOW-HOSTING policy (agent C of
// docs/CARDFILE_ARCHITECTURE.md's decomposition).
//
// Cardfile hosts REAL, UNMODIFIED apps: a card's body IS the app's kernel
// window. This layer launches an app for a card and, every frame, places /
// resizes / shows / hides each hosted window so it matches its card's open
// state (open single card -> its window fills the body rect's CONTENT INSET,
// i.e. cardfile_card_content_rect() - see the #404 follow-up note at
// cf_host_apply() below, NOT the raw body rect; a group -> each member
// window tiled in a pane's content inset; two columns -> each window in its
// column's content inset; everything not shown -> hidden). It never
// reimplements an app and never touches app internals; it only drives the
// kernel window manager.
//
// THE ONE KERNEL HOOK. Placement of a FOREIGN window by id needs
// SYS_WM_SET_BOUNDS (this task added it): wm_set_bounds(win_id, x,y,w,h, flags)
// in <syscall.h>, compositor-privileged, with flags CF_MANAGED (the deck draws
// the frame, so the window carries no kernel chrome) and CF_HIDDEN (stowed:
// minimized, not composited). Stow/focus reuse the existing wm_minimize() /
// wm_focus(). Nothing here blocks or polls (#426): wm_get_windows() is a
// snapshot read and every placement is a single non-blocking syscall.
//
// AGENT A CONTRACT. A owns cardfile.c (the deck render + input) and drives this
// file. The two entry points below are all A needs.

#ifndef COMPOSITOR_CARDFILE_HOST_H
#define COMPOSITOR_CARDFILE_HOST_H

#include "cardfile_model.h"

// Launch the app at app_path for `card_id` (already added to `deck`, e.g. via
// cf_add_card()/cf_duplicate_card()). Spawns it (non-blocking, via the shared
// sys_spawn_args path, with an extra "--popped" argv - see BINDING CAVEAT)
// and returns:
//    0  launched OK; the window is not up yet ("pending") - do NOT wait;
//       cf_host_apply(), called every frame, binds win_id to `card_id`
//       automatically as soon as a matching window appears (subject to the
//       PRE-LAUNCH SNAPSHOT EXCLUSION below);
//   <0  card_id was not found in deck, or the spawn itself failed.
//
// Typical use in the "+" picker's launch path:
//     uint32_t sid = cf_add_card(deck, path, title, cat, color, now, 1, 0);
//     cf_card_t *c = cf_slot_focused_card(cf_find_slot(deck, sid));
//     cf_host_launch(deck, c->id, path);
//     // win_id stays 0; cf_host_apply() binds it within a few frames.
//
// BINDING CAVEAT + (cfmaxwidth) PRE-LAUNCH SNAPSHOT EXCLUSION: the wm
// snapshot exposes an app's BINARY BASENAME (app_id), not its pid, so a
// launch is matched to a window by app_path basename among the not-yet-bound
// windows. Two problems this created, both fixed here without a kernel ABI
// change: (1) among several unbound candidates, cfh_reconcile() prefers the
// HIGHEST window id (ids are allocated monotonically, so this is "most
// recently created" - the best available guess for "the window THIS launch
// made" with no pid field to match against); (2) THAT ALONE IS NOT ENOUGH
// when an app ALSO runs a persistent instance outside any card (e.g. main.c's
// auto-launched /APPS/AICHAT helper, `g_aichat_enabled`): that instance's
// window is often the ONLY candidate present on the very FIRST reconcile pass
// after the card is created (the card's own spawn has not finished creating
// its window yet), so "prefer highest id among what's visible right now"
// still binds to the wrong, pre-existing window - by the time the card's own
// window appears, win_id is already non-zero and reconcile skips it forever.
// cf_host_launch() closes this by recording, on `card_id` itself
// (cf_card_t.launch_floor_id, set via cf_card_set_launch_floor()), the
// highest window id that ALREADY existed for this app_id at the moment of
// THIS launch, taken BEFORE sys_spawn_args() runs. cfh_reconcile() then only
// considers a candidate whose id is STRICTLY GREATER than that floor - so
// the card waits, across as many frames as it takes, for the window its OWN
// spawn creates, and can never latch onto an unrelated pre-existing instance
// regardless of launch order or which reconcile pass sees which window
// first. General, not aichat-specific: a card for an app with no
// pre-existing instance gets floor 0, matching every valid window id and
// behaving exactly as before. An app whose window reports an empty app_id is
// never auto-bound (the kernel could not resolve its identity); such a card
// stays win_id 0 and simply is not hosted until an id is bound by other
// means.
int cf_host_launch(cf_deck_t *deck, uint32_t card_id, const char *app_path);

// The per-frame hosting pass. Call ONCE each frame while the Cardfile layout is
// active, AFTER computing the deck layout (you already fill `geom` for
// cf_layout()) and BEFORE the compositor composites app windows. It:
//   1. reconciles pending launches: binds any card still at win_id 0 to a
//      newly-appeared window whose app identity matches its app_path;
//   2. places every SHOWN card's window with wm_set_bounds(..., CF_MANAGED)
//      at its CONTENT rect, never the raw body/pane rect: an open single
//      card (and each open column - a column is just another open single
//      card) at cardfile_card_content_rect(cf_body_rect()); each member of
//      an open group at cardfile_pane_content_rect(cf_group_pane_rect()).
//      (#404 follow-up, "the frame flicker" fix.) The kernel's
//      SYS_COMPOSITOR_RENDER_WINDOWS re-blits a window's FULL bitmap on
//      every pass with NO regard for the compositor's active clip, so a
//      window placed at the full body/pane rect silently overdraws the
//      frame ring / caption foot / pane header cardfile.c (or
//      cardfile_glass.c on a glass theme) draws around it the instant a
//      partial (clipped) repaint runs - a cursor move is enough. Insetting
//      the window so it never overlaps those chrome pixels at all fixes
//      this by construction: cardfile.h's cardfile_card_content_rect()/
//      cardfile_pane_content_rect() are the ONE place that inset is
//      computed, built from the SAME frame/foot/header constants
//      cardfile.c's own drawing uses, so host and render cannot drift apart;
//   3. hides every card that is not shown this frame (CF_HIDDEN);
//   4. focuses the active slot's focused card.
// `geom` must be the SAME already-resolved metrics (edge_w/tab_step already run
// through cf_edge_width()/cf_tab_step(), screen_w/h set) you pass to
// cf_layout(). Non-blocking. Returns the number of windows it placed (shown +
// hidden), or 0 if the deck hosts nothing yet.
int cf_host_apply(cf_deck_t *deck, const cf_geom_t *geom);

// (cfhostinset, #404 follow-up) Hide every hosted window without placing
// anything - call this INSTEAD of cf_host_apply() for a frame where a
// cardfile popup (a true modal - the "+" picker, Sort, or a colour swatch
// grid) is open. A popup can be far larger than any per-window frame inset
// could account for, and the kernel's SYS_COMPOSITOR_RENDER_WINDOWS re-blits
// every window's full bitmap with no regard for the compositor's active
// clip, so a window left placed under a popup gets restored over it on the
// next partial (clipped) repaint - the same root cause as the frame/foot/
// header flicker cf_host_apply()'s inset placement fixes, but a popup is too
// big to inset around. Hiding outright is safe because a true modal already
// refuses the window any input while it is up. Non-blocking, same cost
// class as cf_host_apply()'s own hide pass.
void cf_host_hide_all(cf_deck_t *deck);

#endif // COMPOSITOR_CARDFILE_HOST_H
