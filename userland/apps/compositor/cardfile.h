// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// cardfile.h - the Cardfile desktop LAYOUT (an alternate shell).
//
// Cardfile is a compositor layout POLICY + chrome renderer over UNMODIFIED
// apps. It is selected the same way every other shell layout is: it is a value
// of g_dock_style (DOCK_CARDFILE), chosen in Settings and carried live to the
// running compositor through /DOCKSTYL.CFG (dock_style_poll() in main.c). When
// active it replaces the classic desktop icons + taskbar + start menu with a
// Rolodex-style card deck: a slim left RAIL ("+" at its head, Sort at its
// foot) and a fanned deck of sideways-tabbed cards, where THE OPEN CARD IS THE
// WINDOW (its body hosts the running app).
//
// (cfrender) THIS FILE NOW CARRIES THE FULL DECK RENDERER + INPUT (agent A of
// docs/CARDFILE_ARCHITECTURE.md's decomposition), built on top of the frozen
// data model in cardfile_model.h (agent B, landed). It draws: the rail, the
// fanned stowed edges (depth-dimmed), the sideways tab plates (count pill +
// member chips for a group), the open card's frame + caption foot + its four
// tab buttons (colour/duplicate/file/close), group-split panes + dividers,
// N-column frames + resize grips, the "+" app picker, the Sort popup and the
// colour swatch popup. It hit-tests and dispatches all of the above from
// cardfile_handle_mouse()/cardfile_handle_key(), called once each from
// main.c's input tick (see that call site's comment).
//
// WINDOW HOSTING: agent C's cardfile_host.c/.h (#404) landed and is wired in
// here. The "+" picker and the duplicate button both call cf_host_launch()
// after creating the model card(s) and bind the win_id when it comes back
// immediately (cf_card_set_win_id()); cardfile_host_tick(), called once per
// frame from main.c BEFORE compositor_render_windows(), calls cf_host_apply()
// to reconcile any still-pending binding and place/hide every hosted window
// to match the deck's current layout. The open card's body (cf_body_rect())
// is still drawn as a frame + caption foot only: the interior is the hosted
// window's rect, owned by the kernel compositor.

#ifndef COMPOSITOR_CARDFILE_H
#define COMPOSITOR_CARDFILE_H

#include <stdint.h>
#include "cardfile_model.h"   // cf_rect_t, for the content-rect helpers below

// 1 when the Cardfile layout is the active shell (g_dock_style == DOCK_CARDFILE).
// The classic layers (desktop icons, taskbar, start menu) gate themselves OFF
// on this, and render_frame_body() calls cardfile_render() instead.
int cardfile_active(void);

// Draw the Cardfile deck chrome for the current frame. Called from
// render_frame_body() AFTER the app-window composite, gated on
// cardfile_active() && !fullscreen && !setup_pending. Honours the active clip.
// Draws straight to g_fb via draw.c primitives; never blocks (#426).
void cardfile_render(void);

// Width in px the rail reserves at the LEFT of the screen. The single source
// of truth for the cardfile left inset (taskbar_left_inset() defers to this
// when the cardfile layout is active, so window placement clears the rail).
int cardfile_rail_width(void);

// (cfrender) The per-frame window-hosting pass (cardfile_host.h, agent C,
// #404): binds any still-pending launch and places/hides every hosted
// window to match the deck's current layout. Call ONCE per frame from
// main.c's render_frame_body(), BEFORE compositor_render_windows() (cf's
// windows must be positioned before the kernel composites them this frame) -
// see that call site's comment. No-op when cardfile is not active or the
// deck is empty. Non-blocking (#426).
void cardfile_host_tick(void);

// (cfhostinset, #404 follow-up) THE CONTENT RECT: the exact interior a
// hosted app window may occupy WITHOUT covering the frame ring / caption
// foot / pane header chrome that cf_draw_single_body()/cf_draw_group_body()
// (and cardfile_glass.c's cfg_card_frame()/cfg_pane() on a glass theme) draw
// around it. cardfile_host.c calls these instead of placing a window at the
// raw cf_body_rect()/cf_group_pane_rect() - the kernel's
// SYS_COMPOSITOR_RENDER_WINDOWS re-blits every window's FULL bitmap on every
// pass with NO regard for the compositor's active clip, so a window placed
// at the full body/pane rect silently overdraws the frame/foot/header on
// the very next partial (clipped) repaint - a cursor move or a taskbar-strip
// damage rect is enough. Insetting the window so it never overlaps those
// chrome pixels at all means an unclipped re-blit has nothing to restore
// over - fixed by construction, no kernel clip change needed. ONE
// definition here (built from the SAME CF_CARD_FRAME_PX/CF_CARD_FOOT_PX/
// CF_PANE_HEADER_PX/CF_PANE_RING_PX cardfile.c's own drawing uses) is what
// keeps host and render from drifting apart - never hardcode these margins
// a second time at a host-side call site.
//
// Single open card AND each open column (a column is just another open
// single card - cf_draw_single_body() draws it) - inset by the frame ring
// on top/left/right and the caption foot at the bottom.
cf_rect_t cardfile_card_content_rect(cf_rect_t body);
// One group-split pane - inset by the pane header on top and a thin ring on
// the other three sides (matches cardfile_glass.c's cfg_pane() ring exactly;
// see cardfile.c's CF_PANE_RING_PX for the cross-file note).
cf_rect_t cardfile_pane_content_rect(cf_rect_t pane);

// (cfhostinset) 1 while a cardfile popup (the "+" app picker, Sort, or a
// colour swatch grid) is open. These are TRUE MODALS (see
// cardfile_handle_mouse()'s own comment: close via ESC/an explicit button
// only, never click-away), so cardfile_host_tick() hides every hosted
// window outright for the frame(s) a popup is open rather than merely
// insetting it - the picker popup in particular is far wider than any frame
// margin, wide enough to sit entirely over an open card's content rect, and
// the SAME unclipped-re-blit behaviour that ate the frame chrome would just
// as happily eat the popup. Hiding is safe precisely because a true modal
// already refuses the window any input while it is up.
int cardfile_popup_active(void);

// (cfrender) Mouse input for the whole deck: tabs (click to open/stow),
// member chips inside a group tab, the four open-tab buttons, group/column
// resize grips and dividers, the "+"/Sort rail tabs, and the three modal
// popups (app picker, sort menu, colour swatches). `clicked` is the SAME
// "left button went down this tick" edge every other overlay's
// *_handle_mouse(x,y,clicked) takes (see startmenu_handle_mouse() etc. in
// compositor.h); ongoing drags are tracked internally off g_mouse_buttons, so
// this must be called EVERY tick cardfile_active() is true, not just on a
// press. `scroll_delta` is the same per-tick wheel delta main.c already reads
// for every other popup (get_mouse_scroll()); pass 0 if none.
//
// Returns 1 if the event was claimed by deck chrome (a tab, a button, a
// popup, an in-progress drag) and must not reach the kernel window manager or
// any classic-shell layer; returns 0 for anything inside an OPEN card's body
// rect (cf_body_rect()) so the caller's normal "forward to the window under
// the cursor" path still runs - the deck deliberately does not compete with
// the hosted app for its own clicks.
int cardfile_handle_mouse(int32_t x, int32_t y, int clicked, int scroll_delta);

// (cfdock) Right-click dispatch, mirroring taskbar_handle_right_click()/
// startmenu_handle_right_click()'s convention (main.c checks this FIRST,
// gated the same way as cardfile_handle_mouse() - see that call site's
// comment). Opens the dock context menu (move/remove) when the press lands
// on a rendered dock bar; returns 0 otherwise so ordinary handling continues.
int cardfile_handle_right_click(int32_t x, int32_t y);

// (cfrender) Keyboard input: ESC closes whichever cardfile popup (picker /
// sort / swatches) is open - true-modal, so this is the ONLY key that closes
// one (no click-away). Returns 1 if the key was consumed (a popup was open
// and closed), 0 otherwise so the caller's normal key handling continues.
int cardfile_handle_key(int key);

// (cfdock) Persistence accessors profile.c calls (see its own "cfd<N>" key
// note) - profile.c owns WHERE the bytes live, cardfile.c owns the deck and
// the pack/unpack format (cardfile_model.h's cf_dock_pack()/cf_dock_unpack()).
// Mirrors the existing desktop_icon_count()/desktop_get_icon_pos() split for
// desktop icon persistence. Safe to call before any card/dock exists (the
// deck may still be zero-length; cf_ensure_init() runs internally).
int cardfile_dock_count_for_profile(void);
int cardfile_dock_pack_for_profile(int idx);              // idx in [0, count); -1 if out of range
void cardfile_dock_add_from_profile(int packed);

// (cfmaxwidth) TESTHOOK-only verification hooks. NEVER compiled into the
// shipping COMPOSIT: gated by the exact same MAYTERA_TESTHOOK the rest of
// testhook.c's own attack-surface-avoidance argument covers (see that
// file's top-of-file comment) - a plain `make`/`make install` (what
// build/build-golden.sh always uses) defines neither symbol. testhook.c is
// the only caller. Exposed here (rather than duplicating cardfile.c's
// static geometry helpers - cf_build_geom()/cf_grip_rect()/cf_tab_btn_rect()/
// cf_side_collapse_btn_rect() - a second time at the call site) so a SEQ
// script can read exact on-screen pixel rects from serial instead of
// hand-computing them, then drive the REAL cardfile_handle_mouse() hit-test
// at those coordinates (CFCLICK/CFDOWN/CFMOVE/CFUP), proving the actual
// input path, not just the model.
#ifdef MAYTERA_TESTHOOK
// Direct access to the deck, for scripted setup (add/group/open cards) that
// would otherwise need pixel-exact tab-position clicks just to reach a
// group-split/multi-column starting state.
cf_deck_t *cardfile_debug_deck(void);
// Fills buf with a single human-readable line: the current geom, every
// slot's cf_layout() entry, and (for the first open slot, plus the
// maximize-view side widgets when active) the exact rects a click needs.
void cardfile_debug_dump(char *buf, unsigned long cap);
// (cfmaxwidth) Diagnostic: every card's app_path/win_id/launch_floor_id, to
// verify the pre-launch snapshot exclusion actually bound the RIGHT window.
void cardfile_debug_wins(char *buf, unsigned long cap);
// Opens deck-order slot `idx` as the sole open slot (setup only, not itself
// a hit-test - see cardfile.c's own comment). Returns 1 if idx was in range.
int cardfile_debug_open_index(int idx);
// The three below drive the REAL cardfile_handle_mouse() dispatch, computing
// the target rect from this file's own geometry so a SEQ script needs no
// precomputed pixel coordinates for a resize grip, a plate button, or a
// maximize-view side widget (whose position depends on live collapse
// state). See cardfile.c for the full contract of each.
int cardfile_debug_grip_drag(int target_w);
int cardfile_debug_click_maximize(void);
int cardfile_debug_click_side(int side);
// (cfdock) Direct dock model manipulation - see cardfile.c's own comment on
// why this is setup, not a hit-test (input is spot-checked separately via
// cardfile_handle_mouse()/cardfile_handle_right_click() through CFCLICK/
// CFRCLICK on the real rail button and popups). `kind` is CF_DOCK_POS_*;
// `edge_or_slot` is CF_DOCK_EDGE_* when kind is EDGE, a slot boundary index
// when kind is BETWEEN.
uint32_t cardfile_debug_dock_add(int kind, int edge_or_slot, int thickness);
int cardfile_debug_dock_set_pos(uint32_t dock_id, int kind, int edge_or_slot);
int cardfile_debug_dock_set_thickness(uint32_t dock_id, int thickness);
int cardfile_debug_dock_remove(uint32_t dock_id);
#endif

#endif // COMPOSITOR_CARDFILE_H
