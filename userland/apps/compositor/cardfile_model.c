// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// cardfile_model.c - implementation of the Cardfile deck data model. See
// cardfile_model.h for the full contract and the resolved-ambiguity notes.
//
// Ported line-for-line in spirit from the approved mockup's JS model
// (cardfile-mockup.html's slots/cards state + mkCard/mkSlot/addCard/openSlot/
// stowSlot/removeCardFromSlot/closeSlot/closeCard/moveCardToSlot/ejectCard/
// dupSlot/applySort/layout/widthAtPointer/gridRows/ensureWeights), with the
// resolved ambiguities documented in the header applied where the mockup
// depended on things this pure model does not have (a live clock, a screen
// size at mutation time).
//
// No floating point (there is no need for any here), no syscalls, no
// blocking. Only <stdint.h> and the freestanding libc's string.h are used, so
// this file compiles unchanged in the the build container compositor build AND in a plain
// host gcc unit test (see cardfile_model_test.c).

#include "cardfile_model.h"
#include "../../libc/string.h"

// ---------------------------------------------------------------------------
// small internal helpers
// ---------------------------------------------------------------------------

static void cf_strlcpy(char *dst, const char *src, unsigned long cap) {
    unsigned long i = 0;
    if (cap == 0) return;
    if (src) {
        for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = 0;
}

static int cf_clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int cmp_u64(uint64_t a, uint64_t b) { return a < b ? -1 : (a > b ? 1 : 0); }
static int cmp_i(int a, int b) { return a < b ? -1 : (a > b ? 1 : 0); }

// ---------------------------------------------------------------------------
// deck lifecycle
// ---------------------------------------------------------------------------

void cf_deck_init(cf_deck_t *deck) {
    memset(deck, 0, sizeof(*deck));
    deck->next_id = 1;
    deck->sort_key = CF_SORT_MANUAL;
    deck->sort_dir = 1;
}

// ---------------------------------------------------------------------------
// lookup
// ---------------------------------------------------------------------------

cf_slot_t *cf_find_slot(cf_deck_t *deck, uint32_t slot_id) {
    int i;
    if (!deck || !slot_id) return NULL;
    for (i = 0; i < deck->nslots; i++) {
        if (deck->slots[i].id == slot_id) return &deck->slots[i];
    }
    return NULL;
}

static int cf_find_slot_index(cf_deck_t *deck, uint32_t slot_id) {
    int i;
    for (i = 0; i < deck->nslots; i++) {
        if (deck->slots[i].id == slot_id) return i;
    }
    return -1;
}

cf_card_t *cf_find_card(cf_deck_t *deck, uint32_t card_id, cf_slot_t **out_slot) {
    int i, k;
    if (out_slot) *out_slot = NULL;
    if (!deck || !card_id) return NULL;
    for (i = 0; i < deck->nslots; i++) {
        cf_slot_t *s = &deck->slots[i];
        for (k = 0; k < s->ncards; k++) {
            if (s->cards[k].id == card_id) {
                if (out_slot) *out_slot = s;
                return &s->cards[k];
            }
        }
    }
    return NULL;
}

cf_card_t *cf_slot_focused_card(cf_slot_t *slot) {
    int k;
    if (!slot || slot->ncards <= 0) return NULL;
    for (k = 0; k < slot->ncards; k++) {
        if (slot->cards[k].id == slot->focus) return &slot->cards[k];
    }
    return &slot->cards[0];
}

uint64_t cf_slot_last_used(const cf_slot_t *slot) {
    uint64_t m = 0;
    int k;
    if (!slot) return 0;
    for (k = 0; k < slot->ncards; k++) if (slot->cards[k].used_ms > m) m = slot->cards[k].used_ms;
    return m;
}

uint64_t cf_slot_last_updated(const cf_slot_t *slot) {
    uint64_t m = 0;
    int k;
    if (!slot) return 0;
    for (k = 0; k < slot->ncards; k++) if (slot->cards[k].updated_ms > m) m = slot->cards[k].updated_ms;
    return m;
}

int cf_slot_has_unread(const cf_slot_t *slot) {
    int k;
    if (!slot) return 0;
    for (k = 0; k < slot->ncards; k++) if (slot->cards[k].unread) return 1;
    return 0;
}

int cf_open_count(const cf_deck_t *deck) {
    int i, n = 0;
    if (!deck) return 0;
    for (i = 0; i < deck->nslots; i++) if (deck->slots[i].open) n++;
    return n;
}

cf_mode_t cf_current_mode(const cf_deck_t *deck) {
    int i, n = cf_open_count(deck);
    if (n <= 0) return CF_MODE_NONE;
    if (n >= 2) return CF_MODE_COLUMNS;
    for (i = 0; i < deck->nslots; i++) {
        if (deck->slots[i].open) {
            return deck->slots[i].ncards > 1 ? CF_MODE_GROUP_SPLIT : CF_MODE_SINGLE;
        }
    }
    return CF_MODE_NONE; /* unreachable */
}

// ---------------------------------------------------------------------------
// grid shape (used by add/close/group ops via cf_slot_sync_grid, and exposed
// directly for the geometry section)
// ---------------------------------------------------------------------------

int cf_group_grid(int ncards, int row_cols[CF_MAX_GRID_ROWS]) {
    int cols, i, nrows;
    if (ncards < 1) ncards = 1;
    if (ncards > CF_MAX_CARDS_PER_SLOT) ncards = CF_MAX_CARDS_PER_SLOT;
    // (cfmaxwidth, owner correction) ncards==2 is now 1 COLUMN (was 2), so
    // the row-major fill below produces two ROWS of one member each - a
    // vertical (top/bottom) stack - instead of one row of two side-by-side
    // members. See this function's own header comment for why nothing else
    // needs to change: cf_group_pane_rects()/the divider render+hit-test+
    // drag paths already key off the resulting SHAPE, not off ncards==2
    // specifically.
    cols = (ncards <= 2) ? 1 : (ncards <= 4) ? 2 : 3;
    nrows = 0;
    i = 0;
    while (i < ncards && nrows < CF_MAX_GRID_ROWS) {
        int remain = ncards - i;
        int c = remain < cols ? remain : cols;
        row_cols[nrows] = c;
        nrows++;
        i += c;
    }
    return nrows;
}

void cf_slot_sync_grid(cf_slot_t *slot) {
    int row_cols[CF_MAX_GRID_ROWS];
    int nrows, r, c, changed;
    if (!slot) return;
    nrows = cf_group_grid(slot->ncards, row_cols);
    changed = (nrows != slot->nrows);
    if (!changed) {
        for (r = 0; r < nrows; r++) {
            if (row_cols[r] != slot->row_cols[r]) { changed = 1; break; }
        }
    }
    slot->nrows = nrows;
    for (r = 0; r < CF_MAX_GRID_ROWS; r++) slot->row_cols[r] = (r < nrows) ? row_cols[r] : 0;
    if (changed) {
        for (r = 0; r < CF_MAX_GRID_ROWS; r++) {
            slot->row_weight[r] = 1;
            for (c = 0; c < CF_MAX_GRID_COLS; c++) slot->col_weight[r][c] = 1;
        }
    }
}

// ---------------------------------------------------------------------------
// internal slot/card mutation primitives
// ---------------------------------------------------------------------------

// Removes deck->slots[idx] from the array, shifting the tail down. Does not
// touch open/focus bookkeeping - callers do that.
static void cf_remove_slot_at(cf_deck_t *deck, int idx) {
    int i;
    for (i = idx; i + 1 < deck->nslots; i++) deck->slots[i] = deck->slots[i + 1];
    deck->nslots--;
    memset(&deck->slots[deck->nslots], 0, sizeof(deck->slots[0]));
}

// Inserts a slot (already fully built) at deck order position `at`. Returns 1
// on success, 0 if the deck is full.
static int cf_insert_slot_at(cf_deck_t *deck, int at, const cf_slot_t *slot) {
    int i;
    if (deck->nslots >= CF_MAX_SLOTS) return 0;
    if (at > deck->nslots) at = deck->nslots;
    if (at < 0) at = 0;
    for (i = deck->nslots; i > at; i--) deck->slots[i] = deck->slots[i - 1];
    deck->slots[at] = *slot;
    deck->nslots++;
    return 1;
}

// Removes one card from its slot; if that empties the slot, the slot itself
// is removed from the deck. Fixes up slot->focus if the focused card was the
// one removed. Returns 1 if found and removed, 0 otherwise. *slot_removed is
// set to 1 if the whole slot vanished (so the caller can react, e.g. ensure_open).
static int cf_remove_card_from_slot(cf_deck_t *deck, uint32_t card_id, int *slot_removed) {
    int i, k;
    if (slot_removed) *slot_removed = 0;
    for (i = 0; i < deck->nslots; i++) {
        cf_slot_t *s = &deck->slots[i];
        for (k = 0; k < s->ncards; k++) {
            if (s->cards[k].id != card_id) continue;
            /* shift members down over the removed one */
            for (; k + 1 < s->ncards; k++) s->cards[k] = s->cards[k + 1];
            s->ncards--;
            if (s->ncards == 0) {
                cf_remove_slot_at(deck, i);
                if (slot_removed) *slot_removed = 1;
            } else {
                if (s->focus == card_id) s->focus = s->cards[0].id;
                cf_slot_sync_grid(s);
            }
            return 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// open / display state
// ---------------------------------------------------------------------------

static void cf_touch_slot_open(cf_slot_t *s, uint64_t now_ms) {
    int k;
    for (k = 0; k < s->ncards; k++) {
        s->cards[k].used_ms = now_ms;
        s->cards[k].unread = 0;
    }
}

int cf_open_single(cf_deck_t *deck, uint32_t slot_id, uint64_t now_ms) {
    int i;
    cf_slot_t *target = cf_find_slot(deck, slot_id);
    if (!target) return 0;
    for (i = 0; i < deck->nslots; i++) {
        cf_slot_t *s = &deck->slots[i];
        if (s == target) continue;
        s->open = 0;
        s->w = 0;
    }
    /* (cfmaxwidth) restore the user's remembered width, if any, instead of
       always forcing fill - see cf_set_column_width()/cf_stow_slot(). */
    target->w = target->pref_w > 0 ? target->pref_w : 0;
    target->open = 1;
    cf_touch_slot_open(target, now_ms);
    deck->active_slot = slot_id;
    cf_maximize_reset(deck);
    return 1;
}

int cf_open_as_group_split(cf_deck_t *deck, uint32_t slot_id, uint64_t now_ms) {
    return cf_open_single(deck, slot_id, now_ms);
}

int cf_open_second_as_column(cf_deck_t *deck, uint32_t slot_id, int w, uint64_t now_ms) {
    cf_slot_t *s = cf_find_slot(deck, slot_id);
    if (!s) return 0;
    s->open = 1;
    /* (cfmaxwidth) w<=0 means "fill" from the caller's point of view; restore
       the remembered pref_w first (0 if none) rather than forcing fill
       outright, so a stowed-then-reopened column comes back at its old
       width. An explicit w>0 both applies now AND becomes the new pref_w. */
    if (w > 0) { s->w = w; s->pref_w = w; }
    else s->w = s->pref_w > 0 ? s->pref_w : 0;
    cf_touch_slot_open(s, now_ms);
    deck->active_slot = slot_id;
    cf_maximize_reset(deck);
    return 1;
}

void cf_ensure_open(cf_deck_t *deck, uint64_t now_ms) {
    int i, best = -1;
    uint64_t best_used = 0;
    if (!deck || deck->nslots == 0 || cf_open_count(deck) > 0) return;
    for (i = 0; i < deck->nslots; i++) {
        uint64_t lu = cf_slot_last_used(&deck->slots[i]);
        if (best < 0 || lu > best_used) { best = i; best_used = lu; }
    }
    if (best >= 0) cf_open_single(deck, deck->slots[best].id, now_ms);
}

int cf_stow_slot(cf_deck_t *deck, uint32_t slot_id) {
    cf_slot_t *s = cf_find_slot(deck, slot_id);
    int i;
    if (!s) return 0;
    s->open = 0;
    s->w = 0;   /* (cfmaxwidth) live width only - pref_w is NOT touched, so a
                   reopen restores it (see cf_open_single()). */
    if (deck->active_slot == slot_id) {
        uint32_t fallback = 0;
        for (i = 0; i < deck->nslots; i++) {
            if (deck->slots[i].open) fallback = deck->slots[i].id;
        }
        deck->active_slot = fallback;
    }
    cf_maximize_reset(deck);
    return 1;
}

int cf_set_column_width(cf_deck_t *deck, uint32_t slot_id, int w) {
    cf_slot_t *s = cf_find_slot(deck, slot_id);
    if (!s) return 0;
    s->w = w > 0 ? w : 0;
    s->pref_w = s->w;   /* (cfmaxwidth) persist the choice across stow/reopen */
    return 1;
}

int cf_set_focus(cf_deck_t *deck, uint32_t slot_id, uint32_t card_id) {
    cf_slot_t *s = cf_find_slot(deck, slot_id);
    int k;
    if (!s) return 0;
    for (k = 0; k < s->ncards; k++) {
        if (s->cards[k].id == card_id) { s->focus = card_id; return 1; }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// (cfmaxwidth) maximize / per-side collapse
// ---------------------------------------------------------------------------

int cf_maximize_active(const cf_deck_t *deck, int *out_open_idx) {
    int i, oi = -1, n_open = 0;
    if (out_open_idx) *out_open_idx = -1;
    if (!deck || !deck->maximized) return 0;
    for (i = 0; i < deck->nslots; i++) {
        if (deck->slots[i].open) { oi = i; n_open++; }
    }
    if (n_open != 1) return 0;
    if (out_open_idx) *out_open_idx = oi;
    return 1;
}

int cf_maximize_toggle(cf_deck_t *deck) {
    if (!deck) return 0;
    if (deck->maximized) {
        deck->maximized = 0;
        deck->left_collapsed = 0;
        deck->right_collapsed = 0;
        return 0;
    }
    if (cf_open_count(deck) != 1) return 0;   /* nothing single to maximize */
    deck->maximized = 1;
    deck->left_collapsed = 1;
    deck->right_collapsed = 1;
    return 1;
}

void cf_maximize_reset(cf_deck_t *deck) {
    if (!deck) return;
    deck->maximized = 0;
    deck->left_collapsed = 0;
    deck->right_collapsed = 0;
}

int cf_toggle_side_collapse(cf_deck_t *deck, int side) {
    if (!deck || !deck->maximized) return -1;
    if (side == CF_SIDE_LEFT) { deck->left_collapsed = !deck->left_collapsed; return deck->left_collapsed; }
    deck->right_collapsed = !deck->right_collapsed;
    return deck->right_collapsed;
}

int cf_side_collapsed_count(const cf_deck_t *deck, int side) {
    int open_i;
    if (!cf_maximize_active(deck, &open_i)) return 0;
    if (side == CF_SIDE_LEFT) return open_i;
    return deck->nslots - open_i - 1;
}

// ---------------------------------------------------------------------------
// card / slot operations
// ---------------------------------------------------------------------------

uint32_t cf_add_card(cf_deck_t *deck, const char *app_path, const char *title,
                      int cat, int color, uint64_t now_ms,
                      int open_it, int keep_others_open) {
    cf_slot_t s;
    cf_card_t *c;
    if (!deck || deck->nslots >= CF_MAX_SLOTS) return 0;

    memset(&s, 0, sizeof(s));
    s.id = deck->next_id++;
    s.ncards = 1;
    c = &s.cards[0];
    c->id = deck->next_id++;
    cf_strlcpy(c->title, title ? title : (app_path ? app_path : ""), sizeof(c->title));
    cf_strlcpy(c->app_path, app_path ? app_path : "", sizeof(c->app_path));
    c->cat = cf_clampi(cat, 0, CF_CAT_COUNT - 1);
    c->color = cf_clampi(color, 0, CF_COLOR_COUNT - 1);
    c->win_id = 0;
    c->created_ms = now_ms;
    c->used_ms = now_ms;
    c->updated_ms = 0;
    c->unread = 0;
    s.focus = c->id;
    s.open = 0;
    s.w = 0;
    cf_slot_sync_grid(&s);

    if (!cf_insert_slot_at(deck, deck->nslots, &s)) return 0;

    if (open_it) {
        if (keep_others_open) cf_open_second_as_column(deck, s.id, 0, now_ms);
        else cf_open_single(deck, s.id, now_ms);
    }
    return s.id;
}

int cf_close_card(cf_deck_t *deck, uint32_t card_id, uint64_t now_ms) {
    int removed_slot = 0;
    if (!deck) return 0;
    if (!cf_remove_card_from_slot(deck, card_id, &removed_slot)) return 0;
    if (removed_slot) cf_ensure_open(deck, now_ms);
    return 1;
}

int cf_close_slot(cf_deck_t *deck, uint32_t slot_id, uint64_t now_ms) {
    int idx = cf_find_slot_index(deck, slot_id);
    if (idx < 0) return 0;
    if (deck->active_slot == slot_id) deck->active_slot = 0;
    cf_remove_slot_at(deck, idx);
    cf_maximize_reset(deck);
    cf_ensure_open(deck, now_ms);
    return 1;
}

int cf_set_color(cf_deck_t *deck, uint32_t card_id, int color) {
    cf_card_t *c = cf_find_card(deck, card_id, NULL);
    if (!c) return 0;
    c->color = cf_clampi(color, 0, CF_COLOR_COUNT - 1);
    return 1;
}

int cf_card_set_win_id(cf_deck_t *deck, uint32_t card_id, uint32_t win_id) {
    cf_card_t *c = cf_find_card(deck, card_id, NULL);
    if (!c) return 0;
    c->win_id = win_id;
    return 1;
}

int cf_card_set_launch_floor(cf_deck_t *deck, uint32_t card_id, uint32_t floor_id) {
    cf_card_t *c = cf_find_card(deck, card_id, NULL);
    if (!c) return 0;
    c->launch_floor_id = floor_id;
    return 1;
}

int cf_mark_activity(cf_deck_t *deck, uint32_t card_id, uint64_t now_ms) {
    cf_card_t *c = cf_find_card(deck, card_id, NULL);
    if (!c) return 0;
    c->unread = 1;
    c->updated_ms = now_ms;
    return 1;
}

// count of cards across the WHOLE deck sharing app_path (used to number
// duplicate titles, matching the mockup's allCards().filter(...).length)
static int cf_count_by_app_path(const cf_deck_t *deck, const char *app_path) {
    int i, k, n = 0;
    for (i = 0; i < deck->nslots; i++) {
        const cf_slot_t *s = &deck->slots[i];
        for (k = 0; k < s->ncards; k++) {
            if (strcmp(s->cards[k].app_path, app_path) == 0) n++;
        }
    }
    return n;
}

// Strips a trailing " (<digits>)" suffix in place, matching the mockup's
// title.replace(/ \(\d+\)$/, '').
static void cf_strip_dup_suffix(char *title) {
    unsigned long len = strlen(title);
    unsigned long end, p;
    if (len < 4 || title[len - 1] != ')') return;
    end = len - 1;
    p = end;
    if (p == 0) return;
    p--;
    if (title[p] < '0' || title[p] > '9') return;
    while (p > 0 && title[p - 1] >= '0' && title[p - 1] <= '9') p--;
    if (p == 0) return;
    if (title[p - 1] != '(') return;
    if (p < 2 || title[p - 2] != ' ') return;
    title[p - 2] = 0;
}

uint32_t cf_duplicate_card(cf_deck_t *deck, uint32_t card_id, uint64_t now_ms) {
    cf_slot_t *src;
    cf_slot_t ns;
    int src_idx, k;
    char numbuf[16];

    if (!deck) return 0;
    if (!cf_find_card(deck, card_id, &src)) return 0;
    src_idx = cf_find_slot_index(deck, src->id);
    if (src_idx < 0) return 0;
    if (deck->nslots >= CF_MAX_SLOTS) return 0;

    memset(&ns, 0, sizeof(ns));
    ns.id = deck->next_id++;
    ns.ncards = src->ncards;
    for (k = 0; k < src->ncards; k++) {
        cf_card_t *dst = &ns.cards[k];
        const cf_card_t *srcc = &src->cards[k];
        int n = cf_count_by_app_path(deck, srcc->app_path) + 1;
        int ni, digits;

        *dst = *srcc;
        dst->id = deck->next_id++;
        dst->win_id = 0;
        dst->launch_floor_id = 0;   // (cfmaxwidth) the copy's own future cf_host_launch() sets its own floor
        dst->created_ms = now_ms;
        dst->used_ms = now_ms;
        dst->updated_ms = 0;
        dst->unread = 0;

        cf_strlcpy(dst->title, srcc->title, sizeof(dst->title));
        cf_strip_dup_suffix(dst->title);
        /* append " (" + n + ")" without sprintf (freestanding-safe) */
        ni = n; digits = 0;
        { int t = ni; do { digits++; t /= 10; } while (t); }
        if (digits > (int)sizeof(numbuf) - 1) digits = (int)sizeof(numbuf) - 1;
        numbuf[digits] = 0;
        { int t = ni, i2 = digits - 1; if (t == 0) { numbuf[0] = '0'; }
          while (t && i2 >= 0) { numbuf[i2] = (char)('0' + (t % 10)); t /= 10; i2--; } }
        {
            unsigned long tl = strlen(dst->title);
            unsigned long cap = sizeof(dst->title);
            unsigned long w = 0;
            if (tl + 2 < cap) { dst->title[tl] = ' '; dst->title[tl + 1] = '('; w = tl + 2; }
            else w = cap - 1;
            { unsigned long j = 0; while (numbuf[j] && w + 1 < cap) dst->title[w++] = numbuf[j++]; }
            if (w + 1 < cap) dst->title[w++] = ')';
            dst->title[w < cap ? w : cap - 1] = 0;
        }
    }
    ns.focus = ns.cards[0].id;
    cf_slot_sync_grid(&ns);

    if (!cf_insert_slot_at(deck, src_idx + 1, &ns)) return 0;
    /* re-fetch src: insertion may have shifted the array */
    src = cf_find_slot(deck, src->id);
    /* (cfmaxwidth) resolved ambiguity #3 resets width to 0 (fill) for BOTH
       sides of a duplicate; pref_w follows the same reset for consistency -
       a duplicate starts fresh, it does not inherit a remembered width. */
    if (src) { src->open = 1; src->w = 0; src->pref_w = 0; }
    {
        cf_slot_t *inserted = cf_find_slot(deck, ns.id);
        if (inserted) { inserted->open = 1; inserted->w = 0; inserted->pref_w = 0; }
    }
    deck->active_slot = ns.id;
    /* This function sets open=1 on two slots directly rather than going
       through cf_open_single()/cf_open_second_as_column(), so it must call
       cf_maximize_reset() itself (see the header's "cfmaxwidth" note). */
    cf_maximize_reset(deck);
    return ns.id;
}

// ---------------------------------------------------------------------------
// grouping
// ---------------------------------------------------------------------------

int cf_group_card_into(cf_deck_t *deck, uint32_t card_id, uint32_t target_slot_id,
                        uint64_t now_ms) {
    cf_card_t *c;
    cf_slot_t *src;
    cf_slot_t *target;
    cf_card_t moved;
    int src_was_open;

    if (!deck) return 0;
    c = cf_find_card(deck, card_id, &src);
    if (!c || !src) return 0;
    target = cf_find_slot(deck, target_slot_id);
    if (!target) return 0;
    if (src == target) return 0;
    if (target->ncards >= CF_MAX_CARDS_PER_SLOT) return 0;

    moved = *c;
    src_was_open = src->open;
    {
        int removed_slot = 0;
        cf_remove_card_from_slot(deck, card_id, &removed_slot);
        /* target pointer may be stale if src's removal shifted the array */
        target = cf_find_slot(deck, target_slot_id);
        if (!target) return 0; /* should not happen: target != src, unaffected by shift-down */
        target->cards[target->ncards] = moved;
        target->ncards++;
        target->focus = moved.id;
        cf_slot_sync_grid(target);
        if (removed_slot && src_was_open) {
            cf_open_second_as_column(deck, target->id, target->w, now_ms);
        }
    }
    cf_ensure_open(deck, now_ms);
    return 1;
}

int cf_group_slots(cf_deck_t *deck, uint32_t src_slot_id, uint32_t dst_slot_id,
                    uint64_t now_ms) {
    cf_slot_t *src, *dst;
    int i, src_open, src_w;

    if (!deck || src_slot_id == dst_slot_id) return 0;
    src = cf_find_slot(deck, src_slot_id);
    dst = cf_find_slot(deck, dst_slot_id);
    if (!src || !dst) return 0;
    if (dst->ncards + src->ncards > CF_MAX_CARDS_PER_SLOT) return 0;

    src_open = src->open;
    src_w = src->w;
    for (i = 0; i < src->ncards; i++) {
        dst->cards[dst->ncards] = src->cards[i];
        dst->ncards++;
    }
    dst->focus = src->cards[0].id;
    cf_slot_sync_grid(dst);

    {
        int idx = cf_find_slot_index(deck, src_slot_id);
        if (idx >= 0) {
            if (deck->active_slot == src_slot_id) deck->active_slot = 0;
            cf_remove_slot_at(deck, idx);
        }
    }
    if (src_open) cf_open_second_as_column(deck, dst_slot_id, src_w, now_ms);
    cf_ensure_open(deck, now_ms);
    return 1;
}

uint32_t cf_ungroup_pull_out(cf_deck_t *deck, uint32_t card_id, int open_it,
                              int w, uint64_t now_ms) {
    cf_slot_t *src;
    cf_card_t *c;
    int src_idx;
    cf_slot_t ns;

    if (!deck) return 0;
    c = cf_find_card(deck, card_id, &src);
    if (!c || !src) return 0;
    if (src->ncards <= 1) return 0;

    memset(&ns, 0, sizeof(ns));
    ns.id = deck->next_id++;
    ns.ncards = 1;
    ns.cards[0] = *c;
    ns.focus = c->id;
    cf_slot_sync_grid(&ns);

    src_idx = cf_find_slot_index(deck, src->id);
    {
        int removed_slot = 0;
        cf_remove_card_from_slot(deck, card_id, &removed_slot);
        (void)removed_slot; /* src had >1 member, so it cannot have been removed */
    }
    if (!cf_insert_slot_at(deck, src_idx + 1, &ns)) return 0;

    if (open_it) {
        cf_open_second_as_column(deck, ns.id, w, now_ms);
    }
    return ns.id;
}

int cf_ungroup_all(cf_deck_t *deck, uint32_t slot_id) {
    cf_slot_t *s = cf_find_slot(deck, slot_id);
    int made = 0;
    if (!s || s->ncards <= 1) return 0;
    /* Pull members [1..ncards-1] out one at a time, always taking the CURRENT
       second member (index 1), stowed, inserted right after the shrinking
       original slot; index 0 stays behind as the slot's sole remaining card. */
    while (1) {
        s = cf_find_slot(deck, slot_id);
        if (!s || s->ncards <= 1) break;
        if (!cf_ungroup_pull_out(deck, s->cards[1].id, 0, 0, 0)) break;
        made++;
    }
    return made;
}

// ---------------------------------------------------------------------------
// sorting
// ---------------------------------------------------------------------------

static int cf_title_ci_cmp(const char *a, const char *b) {
    return strcasecmp(a, b);
}

static int cf_by_name(const cf_slot_t *a, const cf_slot_t *b) {
    cf_card_t *fa = cf_slot_focused_card((cf_slot_t *)a);
    cf_card_t *fb = cf_slot_focused_card((cf_slot_t *)b);
    int r = cf_title_ci_cmp(fa->title, fb->title);
    if (r) return r;
    return cmp_u64(a->id, b->id);
}

static int cf_sort_cmp(int key, const cf_slot_t *a, const cf_slot_t *b) {
    switch (key) {
    case CF_SORT_MANUAL:
        return cmp_u64(a->id, b->id);
    case CF_SORT_NAME:
        return cf_by_name(a, b);
    case CF_SORT_COLOR: {
        cf_card_t *fa = cf_slot_focused_card((cf_slot_t *)a);
        cf_card_t *fb = cf_slot_focused_card((cf_slot_t *)b);
        int r = cmp_i(fa->color, fb->color);
        return r ? r : cf_by_name(a, b);
    }
    case CF_SORT_CATEGORY: {
        cf_card_t *fa = cf_slot_focused_card((cf_slot_t *)a);
        cf_card_t *fb = cf_slot_focused_card((cf_slot_t *)b);
        int r = cmp_i(fa->cat, fb->cat);
        return r ? r : cf_by_name(a, b);
    }
    case CF_SORT_MRU:
        return cmp_u64(cf_slot_last_used(b), cf_slot_last_used(a));
    case CF_SORT_UPDATED: {
        int ua = cf_slot_has_unread(a) ? 1 : 0;
        int ub = cf_slot_has_unread(b) ? 1 : 0;
        int r = cmp_i(ub, ua);
        if (r) return r;
        r = cmp_u64(cf_slot_last_updated(b), cf_slot_last_updated(a));
        if (r) return r;
        return cmp_u64(cf_slot_last_used(b), cf_slot_last_used(a));
    }
    default:
        return 0;
    }
}

void cf_sort(cf_deck_t *deck, int key) {
    int i, j, dir;
    if (!deck) return;
    if (deck->sort_key == key) deck->sort_dir = -deck->sort_dir;
    else { deck->sort_key = key; deck->sort_dir = 1; }
    dir = deck->sort_dir;

    /* stable insertion sort: fine for CF_MAX_SLOTS (32) and it is naturally
       stable, unlike qsort(), which the "ties preserve deck order" contract
       requires. */
    for (i = 1; i < deck->nslots; i++) {
        cf_slot_t tmp = deck->slots[i];
        j = i - 1;
        while (j >= 0 && dir * cf_sort_cmp(key, &deck->slots[j], &tmp) > 0) {
            deck->slots[j + 1] = deck->slots[j];
            j--;
        }
        deck->slots[j + 1] = tmp;
    }
}

// ---------------------------------------------------------------------------
// geometry: deck walk
// ---------------------------------------------------------------------------

int cf_edge_width(int theme_edge_w, int rail_w, int nslots, int screen_w) {
    int n = nslots > 1 ? nslots : 1;
    int room = (screen_w * CF_EDGE_MAX_PCT) / 100 - rail_w;
    int w = room / n;
    if (w > theme_edge_w) w = theme_edge_w;
    if (w < CF_EDGE_FLOOR) w = CF_EDGE_FLOOR;
    return w;
}

int cf_tab_step(int theme_step, int tab_top, int foot, int tab_max_h,
                int nslots, int screen_h) {
    int room, step;
    if (nslots <= 1) return theme_step;
    room = screen_h - tab_top - foot - tab_max_h;
    step = room / (nslots - 1);
    if (step > theme_step) step = theme_step;
    if (step < CF_STEP_FLOOR) step = CF_STEP_FLOOR;
    return step;
}

cf_rect_t cf_rail_rect(const cf_geom_t *geom) {
    cf_rect_t r;
    r.x = geom->origin_x; r.y = geom->origin_y;   // (cfdock) offset by the working area's origin
    r.w = geom->rail_w; r.h = geom->screen_h;
    return r;
}

int cf_layout(const cf_deck_t *deck, const cf_geom_t *geom,
              cf_slot_layout_t *out, int out_cap) {
    // (cfdock) cf_layout() is now a thin wrapper: every existing caller/test
    // keeps its exact prior behaviour (deck->ndocks == 0 for all of them,
    // and cf_layout_ex() is behaviourally IDENTICAL to the old cf_layout()
    // body when ndocks == 0 - see that function's own comment).
    return cf_layout_ex(deck, geom, out, out_cap, NULL);
}

int cf_layout_ex(const cf_deck_t *deck, const cf_geom_t *geom,
                  cf_slot_layout_t *out, int out_cap, cf_rect_t *dock_out) {
    int i, n, fixed = 0, fills = 0, bodies_avail, fill_w = 0, x;
    int open_i, maxed, total_edge_units;
    if (!deck || !geom || !out || out_cap <= 0) return 0;
    n = deck->nslots;
    if (n > out_cap) n = out_cap;

    // (cfdock) Zero every dock's rect up front - a dock hidden inside a
    // maximize-collapsed run (see below) is left exactly as this leaves it
    // (a w==0 rect), which is the documented "reserves no space, draws
    // nothing" behaviour for that rare corner.
    if (dock_out) {
        for (int d = 0; d < deck->ndocks; d++) dock_out[d] = (cf_rect_t){0, 0, 0, 0};
    }
    // (cfdock) Total width every BETWEEN-cards dock spends this frame,
    // subtracted from bodies_avail below exactly like a maximize-collapsed
    // edge unit already is - see cf_layout_ex()'s own header comment. A dock
    // hidden inside a collapsed run (not individually walked below) still
    // counts here: conservative (it reserves the width even where it is not
    // drawn this frame) rather than let the fill math silently disagree
    // frame to frame as maximize is toggled.
    int between_total = 0;
    for (int d = 0; d < deck->ndocks; d++) {
        if (deck->docks[d].pos.kind == CF_DOCK_POS_BETWEEN)
            between_total += cf_dock_thickness_clamp(deck->docks[d].thickness);
    }

    /* (cfmaxwidth) is the maximize/collapse view actually in effect this
       frame? See cf_maximize_active()'s own comment for why this is a
       function call rather than reading deck->maximized directly - it also
       hands back the open slot's index, which the collapse logic below
       needs. */
    maxed = cf_maximize_active(deck, &open_i);

    for (i = 0; i < deck->nslots; i++) {
        const cf_slot_t *s = &deck->slots[i];
        if (!s->open) continue;
        if (s->w > 0) fixed += s->w;
        else fills++;
    }

    /* How many "edge units" (each geom->edge_w wide) the deck actually
       spends this frame. Un-maximized: one per slot, as always. Maximized: a
       collapsed run of stowed slots on either side of the open one spends
       ONE unit for the WHOLE run instead of one per member - this is the
       entire "reclaim the tab space" mechanism (see cf_layout()'s own
       header comment): the (count-1) units a collapsed side no longer
       spends flow straight into fill_w below, exactly like any other freed
       width, no separate widening step needed. */
    if (maxed) {
        int left_n = open_i;                   /* slots [0, open_i) */
        int right_n = deck->nslots - open_i - 1; /* slots (open_i, nslots) */
        int left_units = (deck->left_collapsed && left_n > 0) ? 1 : left_n;
        int right_units = (deck->right_collapsed && right_n > 0) ? 1 : right_n;
        total_edge_units = left_units + 1 /* the open slot's own edge */ + right_units;
    } else {
        total_edge_units = deck->nslots;
    }

    bodies_avail = geom->screen_w - geom->rail_w - total_edge_units * geom->edge_w
                   - CF_COLUMN_GAP - between_total;   // (cfdock)
    if (fills > 0) {
        fill_w = (bodies_avail - fixed) / fills;
        if (fill_w < CF_CARD_MIN_W) fill_w = CF_CARD_MIN_W;
    }

    x = geom->origin_x + geom->rail_w;   // (cfdock) offset by the working area's origin
    i = 0;
    while (i < n) {
        // (cfdock) A BETWEEN dock anchored at boundary `i` reserves its gap
        // HERE, before slot i is walked - see cf_layout_ex()'s own header
        // comment for why this only fires for the loop's CURRENT i (a
        // boundary strictly inside a maximize-collapsed run, which the two
        // branches below skip past via `i = open_i`/`i = n`, is not
        // individually reached and so reserves no space while collapsed).
        for (int d = 0; d < deck->ndocks; d++) {
            const cf_dock_t *dk = &deck->docks[d];
            if (dk->pos.kind != CF_DOCK_POS_BETWEEN || dk->pos.slot_index != i) continue;
            int th = cf_dock_thickness_clamp(dk->thickness);
            if (dock_out) dock_out[d] = (cf_rect_t){ x, geom->origin_y, th, geom->screen_h };
            x += th;
        }
        if (maxed) {
            if (i < open_i && deck->left_collapsed) {
                /* Collapse the WHOLE left run [i, open_i) into one shared
                   edge unit: every member gets the SAME edge_x/body_x
                   (body_w 0) - callers must not draw/hit-test them
                   individually while collapsed (see cf_layout()'s header
                   comment); the render/input side draws ONE summary tab at
                   this edge_x instead (cf_side_summary_tab_rect()). */
                int k;
                for (k = i; k < open_i; k++) {
                    out[k].slot_id = deck->slots[k].id;
                    out[k].edge_x = x;
                    out[k].body_x = x + geom->edge_w;
                    out[k].body_w = 0;
                }
                x += geom->edge_w;
                i = open_i;
                continue;
            }
            if (i > open_i && deck->right_collapsed) {
                int k;
                for (k = i; k < n; k++) {
                    out[k].slot_id = deck->slots[k].id;
                    out[k].edge_x = x;
                    out[k].body_x = x + geom->edge_w;
                    out[k].body_w = 0;
                }
                x += geom->edge_w;
                i = n;
                continue;
            }
        }
        {
            const cf_slot_t *s = &deck->slots[i];
            out[i].slot_id = s->id;
            out[i].edge_x = x;
            out[i].body_x = x + geom->edge_w;
            out[i].body_w = s->open ? (s->w > 0 ? s->w : fill_w) : 0;
            x += geom->edge_w + out[i].body_w;
        }
        i++;
    }
    // (cfdock) The TRAILING boundary, slot_index == deck->nslots (a dock
    // "after the last card"): no further slot_layout_t entry to push over,
    // but bodies_avail already reserved its width above, so there is exactly
    // enough room left at the current x for it.
    for (int d = 0; d < deck->ndocks; d++) {
        const cf_dock_t *dk = &deck->docks[d];
        if (dk->pos.kind != CF_DOCK_POS_BETWEEN || dk->pos.slot_index != n) continue;
        int th = cf_dock_thickness_clamp(dk->thickness);
        if (dock_out) dock_out[d] = (cf_rect_t){ x, geom->origin_y, th, geom->screen_h };
    }
    return n;
}

cf_rect_t cf_edge_rect(const cf_geom_t *geom, const cf_slot_layout_t *entry) {
    cf_rect_t r;
    r.x = entry->edge_x; r.y = geom->origin_y;   // (cfdock)
    r.w = geom->edge_w; r.h = geom->screen_h;
    return r;
}

cf_rect_t cf_body_rect(const cf_geom_t *geom, const cf_slot_layout_t *entry) {
    cf_rect_t r;
    r.x = entry->body_x; r.y = geom->origin_y;   // (cfdock)
    r.w = entry->body_w; r.h = geom->screen_h;
    return r;
}

int cf_open_body_rects(const cf_slot_layout_t *lay, int n_lay,
                        const cf_geom_t *geom, cf_rect_t *out, int out_cap) {
    int i, n = 0;
    for (i = 0; i < n_lay && n < out_cap; i++) {
        if (lay[i].body_w > 0) out[n++] = cf_body_rect(geom, &lay[i]);
    }
    return n;
}

cf_rect_t cf_tab_rect(const cf_geom_t *geom, const cf_slot_layout_t *entry,
                      int idx, int label_len_px, int is_open) {
    cf_rect_t r;
    int lbl = label_len_px;
    int h;
    if (lbl > CF_TAB_LABEL_MAX) lbl = CF_TAB_LABEL_MAX;
    if (lbl < 0) lbl = 0;
    h = lbl + CF_TAB_PAD + (is_open ? CF_TAB_BTN_COUNT * CF_TAB_BTN_H : 0);
    if (h > CF_TAB_MAX_H) h = CF_TAB_MAX_H;

    r.w = geom->tab_w;
    r.x = entry->edge_x + (geom->edge_w - geom->tab_w) / 2;
    r.y = geom->origin_y + geom->tab_top + idx * geom->tab_step;   // (cfdock)
    r.h = h;
    return r;
}

cf_rect_t cf_side_summary_tab_rect(const cf_geom_t *geom, int edge_x, int label_len_px) {
    cf_rect_t r;
    int lbl = label_len_px;
    int h;
    if (lbl > CF_TAB_LABEL_MAX) lbl = CF_TAB_LABEL_MAX;
    if (lbl < 0) lbl = 0;
    h = lbl + CF_TAB_PAD;
    if (h > CF_TAB_MAX_H) h = CF_TAB_MAX_H;

    r.w = geom->tab_w;
    r.x = edge_x + (geom->edge_w - geom->tab_w) / 2;
    r.y = geom->origin_y + geom->tab_top;   /* (cfdock) idx 0 - only one entry per collapsed side */
    r.h = h;
    return r;
}

// ---------------------------------------------------------------------------
// (cfdock) Dock bars: ops + geometry
// ---------------------------------------------------------------------------

int cf_dock_thickness_clamp(int px) {
    return cf_clampi(px, CF_DOCK_THICKNESS_MIN, CF_DOCK_THICKNESS_MAX);
}

int cf_dock_is_vertical(const cf_dock_t *dock) {
    if (!dock) return 0;
    if (dock->pos.kind == CF_DOCK_POS_BETWEEN) return 1;   // perpendicular to card flow
    return dock->pos.edge == CF_DOCK_EDGE_LEFT || dock->pos.edge == CF_DOCK_EDGE_RIGHT;
}

cf_dock_t *cf_dock_find(cf_deck_t *deck, uint32_t dock_id) {
    if (!deck || dock_id == 0) return NULL;
    for (int i = 0; i < deck->ndocks; i++) if (deck->docks[i].id == dock_id) return &deck->docks[i];
    return NULL;
}

// Clamps *pos in place: an out-of-range kind falls back to EDGE/BOTTOM; an
// out-of-range edge is clamped into [0, CF_DOCK_EDGE_COUNT); a BETWEEN
// slot_index is clamped into [0, deck->nslots] (see cf_dock_add()'s own
// comment for why this is a clamp, not a rejection).
static void cf_dock_pos_sanitize(cf_dock_pos_t *pos, const cf_deck_t *deck) {
    if (pos->kind != CF_DOCK_POS_EDGE && pos->kind != CF_DOCK_POS_BETWEEN) {
        pos->kind = CF_DOCK_POS_EDGE; pos->edge = CF_DOCK_EDGE_BOTTOM; pos->slot_index = 0;
        return;
    }
    if (pos->kind == CF_DOCK_POS_EDGE) {
        pos->edge = cf_clampi(pos->edge, 0, CF_DOCK_EDGE_COUNT - 1);
        pos->slot_index = 0;
    } else {
        pos->slot_index = cf_clampi(pos->slot_index, 0, deck ? deck->nslots : 0);
        pos->edge = 0;
    }
}

uint32_t cf_dock_add(cf_deck_t *deck, cf_dock_pos_t pos, int thickness) {
    if (!deck || deck->ndocks >= CF_MAX_DOCKS) return 0;
    cf_dock_pos_sanitize(&pos, deck);
    cf_dock_t *d = &deck->docks[deck->ndocks];
    d->id = deck->next_id++;
    d->pos = pos;
    d->thickness = (thickness > 0) ? cf_dock_thickness_clamp(thickness) : CF_DOCK_THICKNESS_DEFAULT;
    d->items[0] = CF_DOCKITEM_TRAY;
    d->items[1] = CF_DOCKITEM_GAUGES;
    d->items[2] = CF_DOCKITEM_CLOCK;
    d->nitems = 3;
    deck->ndocks++;
    return d->id;
}

int cf_dock_remove(cf_deck_t *deck, uint32_t dock_id) {
    if (!deck) return 0;
    for (int i = 0; i < deck->ndocks; i++) {
        if (deck->docks[i].id != dock_id) continue;
        for (int k = i; k < deck->ndocks - 1; k++) deck->docks[k] = deck->docks[k + 1];
        deck->ndocks--;
        return 1;
    }
    return 0;
}

int cf_dock_set_pos(cf_deck_t *deck, uint32_t dock_id, cf_dock_pos_t pos) {
    cf_dock_t *d = cf_dock_find(deck, dock_id);
    if (!d) return 0;
    cf_dock_pos_sanitize(&pos, deck);
    d->pos = pos;
    return 1;
}

int cf_dock_set_thickness(cf_deck_t *deck, uint32_t dock_id, int thickness) {
    cf_dock_t *d = cf_dock_find(deck, dock_id);
    if (!d) return 0;
    d->thickness = cf_dock_thickness_clamp(thickness);
    return 1;
}

int cf_dock_set_items(cf_deck_t *deck, uint32_t dock_id, const int *items, int nitems) {
    cf_dock_t *d = cf_dock_find(deck, dock_id);
    if (!d) return 0;
    int n = 0;
    for (int i = 0; i < nitems && n < CF_MAX_DOCK_ITEMS; i++) {
        if (items[i] < 0 || items[i] >= CF_DOCKITEM_COUNT) continue;   // dropped, not clamped
        d->items[n++] = items[i];
    }
    d->nitems = n;
    return 1;
}

void cf_dock_reserve_edges(const cf_deck_t *deck, int screen_w, int screen_h,
                            int *out_x, int *out_y, int *out_w, int *out_h) {
    int top = 0, bottom = 0, left = 0, right = 0;
    if (deck) {
        for (int i = 0; i < deck->ndocks; i++) {
            const cf_dock_t *d = &deck->docks[i];
            if (d->pos.kind != CF_DOCK_POS_EDGE) continue;
            int th = cf_dock_thickness_clamp(d->thickness);
            switch (d->pos.edge) {
                case CF_DOCK_EDGE_TOP:    top    += th; break;
                case CF_DOCK_EDGE_BOTTOM: bottom += th; break;
                case CF_DOCK_EDGE_LEFT:   left   += th; break;
                case CF_DOCK_EDGE_RIGHT:  right  += th; break;
            }
        }
    }
    // Never collapse a working axis below CF_DOCK_WORK_MIN: if the docked
    // thicknesses would, scale BOTH edges on that axis down proportionally
    // (rare: only reachable with several thick docks stacked on one axis).
    if (top + bottom > screen_h - CF_DOCK_WORK_MIN && top + bottom > 0) {
        int room = screen_h - CF_DOCK_WORK_MIN; if (room < 0) room = 0;
        int sum = top + bottom;
        top = top * room / sum; bottom = bottom * room / sum;
    }
    if (left + right > screen_w - CF_DOCK_WORK_MIN && left + right > 0) {
        int room = screen_w - CF_DOCK_WORK_MIN; if (room < 0) room = 0;
        int sum = left + right;
        left = left * room / sum; right = right * room / sum;
    }
    *out_x = left; *out_y = top;
    *out_w = screen_w - left - right;
    *out_h = screen_h - top - bottom;
}

cf_rect_t cf_dock_edge_rect(const cf_deck_t *deck, const cf_dock_t *dock,
                            int screen_w, int screen_h) {
    cf_rect_t r = {0, 0, 0, 0};
    if (!deck || !dock || dock->pos.kind != CF_DOCK_POS_EDGE) return r;
    int th = cf_dock_thickness_clamp(dock->thickness);
    int top_total = 0, bottom_total = 0;   // full totals, to size the L/R band between them
    int prior = 0;   // sum of same-edge docks BEFORE this one in array order
    int found = 0;
    for (int i = 0; i < deck->ndocks; i++) {
        const cf_dock_t *d = &deck->docks[i];
        if (d->pos.kind == CF_DOCK_POS_EDGE) {
            if (d->pos.edge == CF_DOCK_EDGE_TOP)    top_total    += cf_dock_thickness_clamp(d->thickness);
            if (d->pos.edge == CF_DOCK_EDGE_BOTTOM) bottom_total += cf_dock_thickness_clamp(d->thickness);
        }
        if (d == dock) { found = 1; continue; }
        if (found) continue;   // only docks BEFORE `dock` in array order count toward `prior`
        if (d->pos.kind == CF_DOCK_POS_EDGE && d->pos.edge == dock->pos.edge)
            prior += cf_dock_thickness_clamp(d->thickness);
    }
    switch (dock->pos.edge) {
        case CF_DOCK_EDGE_TOP:
            r = (cf_rect_t){ 0, prior, screen_w, th };
            break;
        case CF_DOCK_EDGE_BOTTOM:
            r = (cf_rect_t){ 0, screen_h - prior - th, screen_w, th };
            break;
        case CF_DOCK_EDGE_LEFT:
            r = (cf_rect_t){ prior, top_total, th, screen_h - top_total - bottom_total };
            break;
        case CF_DOCK_EDGE_RIGHT:
            r = (cf_rect_t){ screen_w - prior - th, top_total, th, screen_h - top_total - bottom_total };
            break;
    }
    return r;
}

cf_rect_t cf_dock_rect(const cf_deck_t *deck, const cf_dock_t *dock,
                       const cf_geom_t *geom,
                       const cf_slot_layout_t *lay, int n_lay) {
    cf_rect_t zero = {0, 0, 0, 0};
    if (!deck || !dock || !geom) return zero;
    if (dock->pos.kind == CF_DOCK_POS_EDGE)
        return cf_dock_edge_rect(deck, dock, geom->screen_w_raw, geom->screen_h_raw);
    // BETWEEN: recompute via a fresh layout pass with a dock_out buffer,
    // rather than trust a caller-supplied `lay` alone (lay carries no dock
    // rects) - cheap (deck->nslots <= CF_MAX_SLOTS), and keeps this the ONE
    // place that walk happens for this query.
    (void)lay; (void)n_lay;
    cf_slot_layout_t tmp[CF_MAX_SLOTS];
    cf_rect_t dock_rects[CF_MAX_DOCKS];
    cf_layout_ex(deck, geom, tmp, CF_MAX_SLOTS, dock_rects);
    for (int i = 0; i < deck->ndocks; i++) if (&deck->docks[i] == dock) return dock_rects[i];
    return zero;
}

int cf_dock_pack(const cf_dock_t *dock) {
    if (!dock) return 0;
    int kind = (dock->pos.kind == CF_DOCK_POS_BETWEEN) ? 1 : 0;
    int edge_or_slot = kind ? cf_clampi(dock->pos.slot_index, 0, 63)
                            : cf_clampi(dock->pos.edge, 0, 63);
    int th = cf_dock_thickness_clamp(dock->thickness);
    int mask = 0;
    for (int i = 0; i < dock->nitems; i++) {
        if (dock->items[i] >= 0 && dock->items[i] < CF_DOCKITEM_COUNT) mask |= (1 << dock->items[i]);
    }
    return kind | (edge_or_slot << 1) | (th << 7) | (mask << 14);
}

void cf_dock_unpack(int packed, cf_dock_pos_t *out_pos, int *out_thickness,
                     int *out_items, int *out_nitems) {
    unsigned int v = (unsigned int)packed;
    int kind = (int)(v & 0x1);
    int edge_or_slot = (int)((v >> 1) & 0x3F);
    int th = (int)((v >> 7) & 0x7F);
    int mask = (int)((v >> 14) & 0x1F);
    out_pos->kind = kind ? CF_DOCK_POS_BETWEEN : CF_DOCK_POS_EDGE;
    out_pos->edge = kind ? 0 : cf_clampi(edge_or_slot, 0, CF_DOCK_EDGE_COUNT - 1);
    out_pos->slot_index = kind ? edge_or_slot : 0;
    *out_thickness = cf_dock_thickness_clamp(th);
    int n = 0;
    for (int i = 0; i < CF_DOCKITEM_COUNT; i++) if (mask & (1 << i)) out_items[n++] = i;
    *out_nitems = n;
}

cf_rect_t cf_column_divider_rect(const cf_geom_t *geom, const cf_slot_layout_t *a,
                                  const cf_slot_layout_t *b) {
    cf_rect_t r;
    (void)a;
    r.x = b->edge_x - CF_DIVIDER_HIT / 2;
    r.y = 0;
    r.w = CF_DIVIDER_HIT;
    r.h = geom->screen_h;
    return r;
}

int cf_width_at_pointer(const cf_deck_t *deck, const cf_geom_t *geom,
                         const cf_slot_layout_t *lay, int n_lay,
                         uint32_t slot_id, int pointer_body_rel_px) {
    int i, idx = -1, reserve = 0, max_w, w;
    int open_i, maxed;
    if (!deck || !geom || !lay) return CF_CARD_MIN_W;
    for (i = 0; i < n_lay; i++) if (lay[i].slot_id == slot_id) { idx = i; break; }
    if (idx < 0) return CF_CARD_MIN_W;

    /* (cfmaxwidth) if slot_id IS the sole maximized open slot and its right
       side is collapsed, everything after it reserves ONE edge_w total
       (cf_layout()'s own collapse), not one per stowed member - mirror that
       here so a live grip-drag on the maximized card can use the space the
       collapse just gave back. */
    maxed = cf_maximize_active(deck, &open_i);
    if (maxed && open_i == idx && deck->right_collapsed && open_i + 1 < deck->nslots) {
        reserve = geom->edge_w;
    } else {
        for (i = idx + 1; i < deck->nslots; i++) {
            const cf_slot_t *s = &deck->slots[i];
            reserve += geom->edge_w;
            if (s->open) reserve += (s->w > 0 ? s->w : CF_CARD_MIN_W);
        }
    }
    max_w = geom->screen_w - lay[idx].body_x - reserve - CF_COLUMN_GAP;
    w = pointer_body_rel_px;
    if (w > max_w) w = max_w;
    if (w < CF_CARD_MIN_W) w = CF_CARD_MIN_W;
    return w;
}

// ---------------------------------------------------------------------------
// geometry: group split panes
// ---------------------------------------------------------------------------

int cf_group_pane_rects(const cf_slot_t *slot, cf_rect_t body,
                         cf_rect_t *out, int out_cap) {
    int r, c, card_idx = 0;
    int y;
    int total_rh = 0;

    if (!slot || slot->ncards <= 1) return 0;
    for (r = 0; r < slot->nrows; r++) total_rh += slot->row_weight[r];
    if (total_rh <= 0) total_rh = 1;

    y = body.y;
    for (r = 0; r < slot->nrows; r++) {
        int rh;
        int total_cw = 0;
        int x;
        if (r == slot->nrows - 1) rh = (body.y + body.h) - y;
        else rh = (body.h * slot->row_weight[r]) / total_rh;

        for (c = 0; c < slot->row_cols[r]; c++) total_cw += slot->col_weight[r][c];
        if (total_cw <= 0) total_cw = 1;

        x = body.x;
        for (c = 0; c < slot->row_cols[r]; c++) {
            int cw;
            if (c == slot->row_cols[r] - 1) cw = (body.x + body.w) - x;
            else cw = (body.w * slot->col_weight[r][c]) / total_cw;

            if (card_idx < out_cap) {
                out[card_idx].x = x; out[card_idx].y = y;
                out[card_idx].w = cw; out[card_idx].h = rh;
            }
            card_idx++;
            x += cw;
        }
        y += rh;
    }
    return slot->ncards;
}

cf_divide_t cf_clamp_divider(int size_a_px, int size_b_px, int delta_px, int min_px) {
    cf_divide_t d;
    int total = size_a_px + size_b_px;
    int na = size_a_px + delta_px;
    if (na < min_px) na = min_px;
    if (na > total - min_px) na = total - min_px;
    d.a = na;
    d.b = total - na;
    return d;
}

int cf_set_col_divider(cf_deck_t *deck, uint32_t slot_id, int row, int col,
                        int size_a_px, int size_b_px, int delta_px) {
    cf_slot_t *s = cf_find_slot(deck, slot_id);
    cf_divide_t d;
    if (!s || s->ncards <= 1) return 0;
    if (row < 0 || row >= s->nrows) return 0;
    if (col < 1 || col >= s->row_cols[row]) return 0;
    d = cf_clamp_divider(size_a_px, size_b_px, delta_px, CF_PANE_MIN);
    s->col_weight[row][col - 1] = d.a;
    s->col_weight[row][col] = d.b;
    return 1;
}

int cf_set_row_divider(cf_deck_t *deck, uint32_t slot_id, int row,
                        int size_a_px, int size_b_px, int delta_px) {
    cf_slot_t *s = cf_find_slot(deck, slot_id);
    cf_divide_t d;
    if (!s || s->ncards <= 1) return 0;
    if (row < 1 || row >= s->nrows) return 0;
    d = cf_clamp_divider(size_a_px, size_b_px, delta_px, CF_PANE_MIN);
    s->row_weight[row - 1] = d.a;
    s->row_weight[row] = d.b;
    return 1;
}

// ---------------------------------------------------------------------------
// depth / z-order
// ---------------------------------------------------------------------------

int cf_depth_distance(const int *open_idx, int n_open, int idx) {
    int i, best = CF_DEPTH_CAP + 1, d;
    if (n_open <= 0) return 0;
    for (i = 0; i < n_open; i++) {
        d = open_idx[i] > idx ? open_idx[i] - idx : idx - open_idx[i];
        if (d < best) best = d;
    }
    if (best > CF_DEPTH_CAP) best = CF_DEPTH_CAP;
    return best;
}

int cf_depth_dim_x10(int dist) {
    return dist * 45;
}
