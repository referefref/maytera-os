// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// cardfile_host.c - Cardfile WINDOW HOSTING (agent C). See cardfile_host.h for
// the contract. This file is PURE POLICY over the kernel window manager: it
// launches apps, binds their windows to cards, and every frame places / hides /
// focuses each hosted window to match the deck's open state, using the new
// SYS_WM_SET_BOUNDS hook plus the existing wm_focus()/wm_get_windows(). It never
// blocks or polls (#426): the snapshot is one read and each placement is one
// non-blocking syscall.

#include "cardfile_host.h"
// (cfhostinset, #404 follow-up) cardfile_card_content_rect()/
// cardfile_pane_content_rect() - the ONE shared definition of how far a
// hosted window must be inset from the raw body/pane rect to stay off the
// frame ring / caption foot / pane header cardfile.c (agent A) draws around
// it. See cardfile_host.h's own updated comment on cf_host_apply() for why
// this is needed at all (the kernel's unclipped-window-re-blit behaviour).
#include "cardfile.h"
#include "../../libc/syscall.h"   // wm_get_windows, wm_set_bounds, wm_focus, sys_spawn, CF_MANAGED/CF_HIDDEN
#include "../../libc/string.h"    // strrchr, strcasecmp, strncpy

// The wm snapshot cap matches the model's CF_MAX_SLOTS and the kernel's own
// wm_window_info_t wins[32] ceiling (main.c:355): every hosted card owns a live
// window under that same ceiling.
#define CFH_MAX_WINS CF_MAX_SLOTS

// ---------------------------------------------------------------------------
// Identity matching
// ---------------------------------------------------------------------------

// The basename of an app_path, e.g. "/APPS/FILES" -> "FILES". The wm snapshot
// exposes exactly this (wm_window_info_t.app_id, the binary basename the kernel
// spawned the window's process from), so a card is matched to its window by
// comparing this against app_id, case-insensitively.
static const char *cfh_basename(const char *path) {
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// 1 if win_id is already bound to some card anywhere in the deck.
static int cfh_win_id_bound(const cf_deck_t *deck, uint32_t win_id) {
    if (win_id == 0) return 0;
    for (int i = 0; i < deck->nslots; i++) {
        const cf_slot_t *s = &deck->slots[i];
        for (int k = 0; k < s->ncards; k++) {
            if (s->cards[k].win_id == win_id) return 1;
        }
    }
    return 0;
}

// The highest window id in `wins` whose app_id case-insensitively matches
// `base`, or 0 if none. Shared by cf_host_launch() (the PRE-LAUNCH snapshot,
// taken before spawning) and cfh_reconcile()'s per-card scan (the POST-launch
// candidate search, gated by the recorded floor) below - one definition of
// "the newest matching window" for both.
static uint32_t cfh_max_id_for(const wm_window_info_t *wins, int n, const char *base) {
    uint32_t best = 0;
    for (int w = 0; w < n; w++) {
        if (wins[w].id <= 0) continue;
        if (wins[w].app_id[0] == '\0') continue;      // no identity: never match (blame.md)
        if (strcasecmp(wins[w].app_id, base) != 0) continue;
        if ((uint32_t)wins[w].id > best) best = (uint32_t)wins[w].id;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Launch
// ---------------------------------------------------------------------------

int cf_host_launch(cf_deck_t *deck, uint32_t card_id, const char *app_path) {
    if (!deck || !app_path || !app_path[0]) return -1;
    if (!cf_find_card(deck, card_id, NULL)) return -1;

    // (cfmaxwidth) PRE-LAUNCH SNAPSHOT EXCLUSION: record, on the card itself,
    // the highest window id that ALREADY exists for this app_id BEFORE
    // spawning - see cardfile_host.h's own comment on cf_host_launch() for
    // the full rationale (closes the race a persistent auto-launched helper,
    // e.g. main.c's /APPS/AICHAT, would otherwise win by simply existing
    // first). A snapshot read, same cost class as cf_host_apply()'s own.
    {
        wm_window_info_t wins[CFH_MAX_WINS];
        int n = wm_get_windows(wins, CFH_MAX_WINS);
        if (n < 0) n = 0;
        uint32_t floor_id = cfh_max_id_for(wins, n, cfh_basename(app_path));
        cf_card_set_launch_floor(deck, card_id, floor_id);
    }

    // (cfmaxwidth) Launch with an extra "--popped" argv, through the SAME
    // sys_spawn_args() every other compositor-side launcher already uses
    // (desktop.c's two Files launches, screenshot.c's Snapshot launch) - not
    // a new mechanism. WHY: a card hosts a REAL window at an ARBITRARY size
    // the deck chose, not a screen-edge dock panel, but an app whose default
    // launch state assumes it owns a docked edge renders wrong when force-
    // placed into a pane. Measured on aichat (/APPS/AICHAT): it boots
    // DOCK_COLLAPSED - a 12px "sliver is the whole window" state - so
    // launched bare into a wide card pane it drew only the 12px dock-handle
    // strip stretched across the pane (solid colour, no chat UI at all).
    // aichat's own main.c ALREADY has the fix for exactly this: `--popped`
    // sets g_popped, which makes it create an ordinary framed window with
    // zero dock-handle insets and skips the DOCK_COLLAPSED "draw only the
    // handle" early return in draw_all() - i.e. "act like a normal app
    // window", which is exactly what a card needs, using the app's OWN
    // documented mode rather than the deck reaching into its internals.
    // NOT AN AICHAT SPECIAL CASE: every card launch gets the same extra
    // argv, unconditionally. It is a no-op for every OTHER launchable app
    // checked against this argv (editor/browser ignore argv entirely; calc/
    // terminal gate argv behind an explicit contract-invocation check that
    // "--popped" does not match; files/main.c only treats argv[1] as a
    // start path when it begins with '/', which "--popped" does not) - so
    // this is a general "cards launch apps in embedded-window mode, not
    // screen-edge-panel mode" policy that happens to matter to one app
    // today, not a per-app branch.
    char *argv[2];
    argv[0] = (char *)app_path;
    argv[1] = (char *)"--popped";
    int r = sys_spawn_args(app_path, argv, 2);
    // Spawning does not block and does not wait for the window: the child
    // creates its window asynchronously, so we return "pending" (0) and let
    // cf_host_apply() bind it when it shows.
    if (r < 0) return -1;
    return 0;   // launched OK; pending bind (see cf_host_apply reconcile)
}

// ---------------------------------------------------------------------------
// Per-frame reconcile + placement
// ---------------------------------------------------------------------------

// Bind any card still at win_id 0 to a newly-appeared, not-yet-bound window
// whose app_id matches the card's app_path basename. Deck order, so duplicates
// bind in a stable order.
//
// (cfmaxwidth) Among several eligible candidates sharing one app_id, picks
// the HIGHEST window id (most recently created, ids are allocated
// monotonically - the best available guess with no pid field on
// wm_window_info_t to match against directly). A candidate is ELIGIBLE only
// if its id is STRICTLY GREATER than the card's own launch_floor_id (set once
// by cf_host_launch(), before it spawned, to the highest id that already
// existed for this app_id) - the PRE-LAUNCH SNAPSHOT EXCLUSION. This is what
// actually closes the race an app like /APPS/AICHAT exposes: main.c
// auto-launches its own persistent instance at boot, which is ALREADY in the
// snapshot on the very first reconcile pass after a card for the same app is
// created - "prefer the highest id among what's visible right now" alone
// still picks that pre-existing window, since the card's own spawn has not
// created its window yet. Excluding every id that existed before THIS card's
// launch means the card simply waits (across as many frames as it takes) for
// a genuinely new window, never latching onto an unrelated pre-existing
// instance regardless of launch order. General, not aichat-specific: a card
// whose app had no pre-existing instance gets floor 0, which every valid
// window id is greater than, so behaviour is unchanged from before. Returns
// the number of cards newly bound.
static int cfh_reconcile(cf_deck_t *deck, const wm_window_info_t *wins, int n) {
    int bound = 0;
    for (int i = 0; i < deck->nslots; i++) {
        cf_slot_t *s = &deck->slots[i];
        for (int k = 0; k < s->ncards; k++) {
            cf_card_t *c = &s->cards[k];
            if (c->win_id != 0) continue;
            const char *base = cfh_basename(c->app_path);
            uint32_t best_id = 0;
            for (int w = 0; w < n; w++) {
                if (wins[w].id <= 0) continue;
                if (wins[w].app_id[0] == '\0') continue;      // no identity: never match (blame.md)
                if (strcasecmp(wins[w].app_id, base) != 0) continue;
                if ((uint32_t)wins[w].id <= c->launch_floor_id) continue;   // (cfmaxwidth) pre-existing: excluded
                if (cfh_win_id_bound(deck, (uint32_t)wins[w].id)) continue;
                if ((uint32_t)wins[w].id > best_id) best_id = (uint32_t)wins[w].id;
            }
            if (best_id > 0) {
                cf_card_set_win_id(deck, c->id, best_id);
                bound++;
            }
        }
    }
    return bound;
}

// Place one shown card's window at `r` with the deck drawing the frame.
static int cfh_show(uint32_t win_id, cf_rect_t r) {
    if (win_id == 0) return 0;
    if (r.w < 1 || r.h < 1) return 0;
    wm_set_bounds((int)win_id, r.x, r.y, r.w, r.h, CF_MANAGED);
    return 1;
}

// Hide one hosted window. Geometry is ignored for a hidden window, so pass
// zeros; CF_MANAGED is kept so the window stays chrome-free for when it
// reappears. Shared by cf_host_apply()'s step 4 (stowed cards) and
// cf_host_hide_all() (cfhostinset, #404 follow-up - the whole deck, while a
// true-modal cardfile popup is open).
static void cfh_hide(uint32_t win_id) {
    if (win_id == 0) return;
    wm_set_bounds((int)win_id, 0, 0, 0, 0, CF_MANAGED | CF_HIDDEN);
}

void cf_host_hide_all(cf_deck_t *deck) {
    if (!deck) return;
    for (int i = 0; i < deck->nslots; i++) {
        cf_slot_t *s = &deck->slots[i];
        for (int k = 0; k < s->ncards; k++) cfh_hide(s->cards[k].win_id);
    }
}

int cf_host_apply(cf_deck_t *deck, const cf_geom_t *geom) {
    if (!deck || !geom) return 0;

    wm_window_info_t wins[CFH_MAX_WINS];
    int n = wm_get_windows(wins, CFH_MAX_WINS);
    if (n < 0) n = 0;

    // 1. Bind pending launches.
    cfh_reconcile(deck, wins, n);

    // 2. Compute the deck walk once.
    cf_slot_layout_t lay[CF_MAX_SLOTS];
    int n_lay = cf_layout(deck, geom, lay, CF_MAX_SLOTS);

    // Track which card win_ids we SHOW this frame, so everything else is hidden.
    uint32_t shown[CF_MAX_SLOTS * CF_MAX_CARDS_PER_SLOT];
    int n_shown = 0;
    int placed = 0;

    // 3. Place every shown window. An open slot's body is cf_body_rect(); a
    //    single card fills it, a group tiles it into panes. N-column mode falls
    //    out for free: each open slot's layout entry already carries its own
    //    body_x/body_w, so this same loop places one, two or N columns.
    for (int li = 0; li < n_lay; li++) {
        if (lay[li].body_w <= 0) continue;   // not an open slot this frame
        cf_slot_t *s = cf_find_slot(deck, lay[li].slot_id);
        if (!s) continue;
        cf_rect_t body = cf_body_rect(geom, &lay[li]);

        if (s->ncards <= 1) {
            uint32_t wid = s->cards[0].win_id;
            // (cfhostinset) Inset from the raw body rect so the window never
            // covers the frame ring / caption foot cardfile.c draws around
            // it (a column is just another open single card, so this same
            // branch already places N columns correctly).
            cf_rect_t content = cardfile_card_content_rect(body);
            if (cfh_show(wid, content)) {
                if (n_shown < (int)(sizeof(shown)/sizeof(shown[0]))) shown[n_shown++] = wid;
                placed++;
            }
        } else {
            cf_rect_t panes[CF_MAX_CARDS_PER_SLOT];
            int np = cf_group_pane_rects(s, body, panes, CF_MAX_CARDS_PER_SLOT);
            for (int k = 0; k < np; k++) {
                uint32_t wid = s->cards[k].win_id;
                // (cfhostinset) Inset from the raw pane rect so the window
                // never covers that pane's header/ring.
                cf_rect_t content = cardfile_pane_content_rect(panes[k]);
                if (cfh_show(wid, content)) {
                    if (n_shown < (int)(sizeof(shown)/sizeof(shown[0]))) shown[n_shown++] = wid;
                    placed++;
                }
            }
        }
    }

    // 4. Hide every hosted window NOT shown this frame (stowed cards, and the
    //    non-focused members of an open group are already shown as panes, so
    //    only genuinely-stowed slots hide).
    for (int i = 0; i < deck->nslots; i++) {
        cf_slot_t *s = &deck->slots[i];
        for (int k = 0; k < s->ncards; k++) {
            uint32_t wid = s->cards[k].win_id;
            if (wid == 0) continue;
            int is_shown = 0;
            for (int j = 0; j < n_shown; j++) { if (shown[j] == wid) { is_shown = 1; break; } }
            if (is_shown) continue;
            cfh_hide(wid);
            placed++;
        }
    }

    // 5. Focus the active slot's focused card (raise it within the shown set;
    //    columns do not overlap so this only decides keyboard focus).
    if (deck->active_slot != 0) {
        cf_slot_t *as = cf_find_slot(deck, deck->active_slot);
        if (as) {
            cf_card_t *fc = cf_slot_focused_card(as);
            if (fc && fc->win_id != 0) wm_focus((int)fc->win_id);
        }
    }

    return placed;
}
