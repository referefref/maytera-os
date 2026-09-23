// cardfile_model_test.c - host unit test for the Cardfile deck data model.
//
// Plain host gcc, no VM, no freestanding flags: cardfile_model.c depends only
// on <stdint.h> (via its header) and the freestanding libc's string.h
// prototypes, which match the host libc's real strcmp/strcasecmp/memset/
// strlen/strncpy exactly, so this links against the ordinary host C library.
//
// Not a test framework: a small CHECK() macro that prints and aborts on the
// first failure, run() wrappers for readability, and a final PASS/FAIL tally.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cardfile_model.h"

static int g_checks = 0, g_fails = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_fails++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static uint64_t T = 1000; /* a fake monotonic clock the tests advance by hand */
static uint64_t tick(void) { T += 100; return T; }

// ---------------------------------------------------------------------------
// add / close / stow / duplicate / colour
// ---------------------------------------------------------------------------

static void test_add_close_stow_dup_color(void) {
    cf_deck_t d;
    uint32_t s1, s2, s3, dup_slot;
    cf_slot_t *slot;
    cf_card_t *card;

    cf_deck_init(&d);
    CHECK(d.nslots == 0);
    CHECK(cf_current_mode(&d) == CF_MODE_NONE);

    s1 = cf_add_card(&d, "FILES", "Files", CF_CAT_ACCESSORIES, CF_COLOR_SAGE, tick(), 1, 0);
    CHECK(s1 != 0);
    CHECK(d.nslots == 1);
    CHECK(cf_current_mode(&d) == CF_MODE_SINGLE);
    slot = cf_find_slot(&d, s1);
    CHECK(slot && slot->open == 1);
    CHECK(slot && slot->ncards == 1);
    CHECK(strcmp(slot->cards[0].title, "Files") == 0);
    CHECK(slot->cards[0].color == CF_COLOR_SAGE);

    s2 = cf_add_card(&d, "TERM", "Terminal", CF_CAT_ACCESSORIES, CF_COLOR_SLATE, tick(), 1, 0);
    CHECK(s2 != 0 && d.nslots == 2);
    /* opening s2 without keep_others_open stows s1 */
    CHECK(cf_find_slot(&d, s1)->open == 0);
    CHECK(cf_find_slot(&d, s2)->open == 1);
    CHECK(cf_current_mode(&d) == CF_MODE_SINGLE);

    s3 = cf_add_card(&d, "CALC", "Calculator", CF_CAT_ACCESSORIES, CF_COLOR_DEFAULT, tick(), 1, 1);
    CHECK(s3 != 0);
    /* keep_others_open=1: s2 stays open too -> columns mode */
    CHECK(cf_find_slot(&d, s2)->open == 1);
    CHECK(cf_find_slot(&d, s3)->open == 1);
    CHECK(cf_open_count(&d) == 2);
    CHECK(cf_current_mode(&d) == CF_MODE_COLUMNS);

    /* colour */
    card = cf_find_card(&d, cf_find_slot(&d, s3)->cards[0].id, NULL);
    CHECK(cf_set_color(&d, card->id, CF_COLOR_ROSE));
    CHECK(cf_find_card(&d, card->id, NULL)->color == CF_COLOR_ROSE);
    CHECK(cf_set_color(&d, 99999, CF_COLOR_ROSE) == 0); /* unknown id */

    /* win_id + activity */
    CHECK(cf_card_set_win_id(&d, card->id, 42));
    CHECK(cf_find_card(&d, card->id, NULL)->win_id == 42);
    CHECK(cf_mark_activity(&d, card->id, tick()));
    CHECK(cf_find_card(&d, card->id, NULL)->unread == 1);

    /* (cfmaxwidth) launch_floor_id: cf_host_launch()'s pre-launch snapshot
       exclusion floor. A fresh card starts with none; the setter works;
       unknown id is refused. */
    CHECK(cf_find_card(&d, card->id, NULL)->launch_floor_id == 0);
    CHECK(cf_card_set_launch_floor(&d, card->id, 7));
    CHECK(cf_find_card(&d, card->id, NULL)->launch_floor_id == 7);
    CHECK(cf_card_set_launch_floor(&d, 99999, 7) == 0); /* unknown id */

    /* duplicate: duplicates the WHOLE slot, opens beside it */
    dup_slot = cf_duplicate_card(&d, card->id, tick());
    CHECK(dup_slot != 0);
    CHECK(d.nslots == 4);
    {
        cf_slot_t *orig = cf_find_slot(&d, s3);
        cf_slot_t *copy = cf_find_slot(&d, dup_slot);
        CHECK(orig && orig->open == 1);
        CHECK(copy && copy->open == 1);
        CHECK(copy->cards[0].id != orig->cards[0].id);
        CHECK(strcmp(copy->cards[0].title, "Calculator (2)") == 0);
        CHECK(copy->cards[0].win_id == 0);       /* fresh copy needs its own launch */
        CHECK(copy->cards[0].unread == 0);
        /* (cfmaxwidth) the fresh copy does NOT inherit the source's recorded
           launch floor - it gets its own real cf_host_launch() later, which
           records its own floor at that time; the SOURCE's is untouched. */
        CHECK(copy->cards[0].launch_floor_id == 0);
        CHECK(orig->cards[0].launch_floor_id == 7);
        CHECK(d.active_slot == dup_slot);
        /* duplicating again should read the growing count */
        {
            uint32_t dup2 = cf_duplicate_card(&d, orig->cards[0].id, tick());
            CHECK(dup2 != 0);
            CHECK(strcmp(cf_find_slot(&d, dup2)->cards[0].title, "Calculator (3)") == 0);
        }
    }

    /* stow */
    CHECK(cf_stow_slot(&d, s1) == 1); /* already stowed, still succeeds */
    CHECK(cf_find_slot(&d, s1)->open == 0);
    CHECK(cf_stow_slot(&d, 424242) == 0);

    /* close a lone card: removes the whole slot */
    {
        int nslots_before = d.nslots;
        uint32_t victim_card = cf_find_slot(&d, s1)->cards[0].id;
        CHECK(cf_close_card(&d, victim_card, tick()));
        CHECK(d.nslots == nslots_before - 1);
        CHECK(cf_find_slot(&d, s1) == NULL);
    }

    /* close everything, verify ensure_open leaves nothing open only when deck is empty */
    while (d.nslots > 0) {
        cf_close_slot(&d, d.slots[0].id, tick());
    }
    CHECK(d.nslots == 0);
    CHECK(cf_current_mode(&d) == CF_MODE_NONE);
}

// ---------------------------------------------------------------------------
// grouping to 8, max enforced, ungroup / pull-out
// ---------------------------------------------------------------------------

static void test_grouping_and_ungroup(void) {
    cf_deck_t d;
    uint32_t slots[9];
    int i;
    char path[32], title[32];

    cf_deck_init(&d);
    for (i = 0; i < 9; i++) {
        sprintf(path, "APP%d", i);
        sprintf(title, "App %d", i);
        slots[i] = cf_add_card(&d, path, title, CF_CAT_SYSTEM, CF_COLOR_WHEAT, tick(), 0, 0);
        CHECK(slots[i] != 0);
    }
    CHECK(d.nslots == 9);

    /* group slots[1..7] (7 more cards) into slots[0] -> 8 total, exactly at the cap */
    for (i = 1; i <= 7; i++) {
        uint32_t card_id = cf_find_slot(&d, slots[i])->cards[0].id;
        CHECK(cf_group_card_into(&d, card_id, slots[0], tick()) == 1);
    }
    {
        cf_slot_t *g = cf_find_slot(&d, slots[0]);
        CHECK(g && g->ncards == 8);
        CHECK(d.nslots == 2); /* the group slot + slots[8], the one never merged */
    }

    /* the 9th card must be REFUSED: group is full (max-8 enforced) */
    {
        uint32_t card9 = cf_find_slot(&d, slots[8])->cards[0].id;
        int before = d.nslots;
        CHECK(cf_group_card_into(&d, card9, slots[0], tick()) == 0);
        CHECK(cf_find_slot(&d, slots[0])->ncards == 8); /* unchanged */
        CHECK(d.nslots == before);                       /* slots[8] still its own slot */
    }

    /* open as group-split: mode must report GROUP_SPLIT */
    CHECK(cf_open_as_group_split(&d, slots[0], tick()));
    CHECK(cf_current_mode(&d) == CF_MODE_GROUP_SPLIT);
    {
        cf_slot_t *g = cf_find_slot(&d, slots[0]);
        CHECK(g->nrows == 3); /* 8 cards -> 3x3 grid, last row short (3,3,2) */
        CHECK(g->row_cols[0] == 3 && g->row_cols[1] == 3 && g->row_cols[2] == 2);
    }

    /* pull one member out to its own slot */
    {
        cf_slot_t *g = cf_find_slot(&d, slots[0]);
        uint32_t pulled_card = g->cards[3].id; /* an arbitrary interior member */
        uint32_t new_slot = cf_ungroup_pull_out(&d, pulled_card, 1, 0, tick());
        CHECK(new_slot != 0);
        CHECK(cf_find_slot(&d, slots[0])->ncards == 7);
        CHECK(cf_find_slot(&d, new_slot)->ncards == 1);
        CHECK(cf_find_slot(&d, new_slot)->cards[0].id == pulled_card);
        CHECK(cf_find_slot(&d, new_slot)->open == 1); /* opened at column width 0 (fill) */
        /* inserted immediately after the group in deck order */
        {
            int gi = -1, ni = -1, k;
            for (k = 0; k < d.nslots; k++) {
                if (d.slots[k].id == slots[0]) gi = k;
                if (d.slots[k].id == new_slot) ni = k;
            }
            CHECK(gi >= 0 && ni == gi + 1);
        }
    }

    /* pulling the sole remaining member of a 1-card slot must fail */
    {
        uint32_t only_card = cf_find_slot(&d, slots[8])->cards[0].id;
        CHECK(cf_ungroup_pull_out(&d, only_card, 0, 0, tick()) == 0);
    }

    /* ungroup_all explodes the rest of the group (7 members) into 6 new + 1 kept */
    {
        int made = cf_ungroup_all(&d, slots[0]);
        CHECK(made == 6);
        CHECK(cf_find_slot(&d, slots[0])->ncards == 1);
    }

    /* cf_group_slots: bulk-group two whole slots, all-or-nothing at the cap */
    {
        cf_deck_t d2;
        uint32_t a, b, c;
        cf_deck_init(&d2);
        a = cf_add_card(&d2, "A", "A", 0, 0, tick(), 0, 0);
        b = cf_add_card(&d2, "B", "B", 0, 0, tick(), 0, 0);
        /* grow b to 7 members via repeated single-card grouping, then try to
           merge a's 1 member in: 7+1=8 must succeed */
        {
            int k;
            for (k = 0; k < 6; k++) {
                sprintf(path, "B%d", k);
                sprintf(title, "B%d", k);
                c = cf_add_card(&d2, path, title, 0, 0, tick(), 0, 0);
                CHECK(cf_group_card_into(&d2, cf_find_slot(&d2, c)->cards[0].id, b, tick()));
            }
        }
        CHECK(cf_find_slot(&d2, b)->ncards == 7);
        CHECK(cf_group_slots(&d2, a, b, tick()) == 1);
        CHECK(cf_find_slot(&d2, b)->ncards == 8);
        CHECK(cf_find_slot(&d2, a) == NULL);

        /* now try to merge one more single card in: 8+1 > 8, all-or-nothing refusal */
        {
            uint32_t extra = cf_add_card(&d2, "X", "X", 0, 0, tick(), 0, 0);
            int nslots_before = d2.nslots;
            CHECK(cf_group_slots(&d2, extra, b, tick()) == 0);
            CHECK(cf_find_slot(&d2, b)->ncards == 8);
            CHECK(d2.nslots == nslots_before);
        }
    }
}

// ---------------------------------------------------------------------------
// sorting
// ---------------------------------------------------------------------------

static void test_sorting(void) {
    cf_deck_t d;
    uint32_t sZ, sA, sM;
    uint64_t t1, t2, t3;

    cf_deck_init(&d);
    /* deliberately out of every natural order except creation */
    sZ = cf_add_card(&d, "Z", "Zebra",  CF_CAT_GAMES,       CF_COLOR_SPRUCE, tick(), 0, 0);
    sA = cf_add_card(&d, "A", "Apple",  CF_CAT_INTERNET,    CF_COLOR_DEFAULT, tick(), 0, 0);
    sM = cf_add_card(&d, "M", "Middle", CF_CAT_MEDIA,       CF_COLOR_ROSE, tick(), 0, 0);

    /* CF_SORT_MANUAL: creation order (id ascending) == insertion order.
       cf_deck_init() seeds sort_key=CF_SORT_MANUAL/sort_dir=1 already (matching
       the mockup's own `let sortKey='manual', sortDir=1`), so the FIRST ever
       cf_sort(MANUAL) call sees "same key as before" and flips direction -
       exactly like calling applySort('manual') first thing in the mockup
       would. Assert the flip, then sort again to get back to ascending. */
    cf_sort(&d, CF_SORT_MANUAL);
    CHECK(d.slots[0].id == sM && d.slots[1].id == sA && d.slots[2].id == sZ);
    cf_sort(&d, CF_SORT_MANUAL);
    CHECK(d.slots[0].id == sZ && d.slots[1].id == sA && d.slots[2].id == sM);

    /* CF_SORT_NAME: Apple, Middle, Zebra */
    cf_sort(&d, CF_SORT_NAME);
    CHECK(d.slots[0].id == sA && d.slots[1].id == sM && d.slots[2].id == sZ);
    /* same key again flips direction: Zebra, Middle, Apple */
    cf_sort(&d, CF_SORT_NAME);
    CHECK(d.slots[0].id == sZ && d.slots[1].id == sM && d.slots[2].id == sA);

    /* CF_SORT_COLOR: palette index ascending: DEFAULT(0)=A, ROSE(4)=M, SPRUCE(7)=Z */
    cf_sort(&d, CF_SORT_COLOR);
    CHECK(d.slots[0].id == sA && d.slots[1].id == sM && d.slots[2].id == sZ);

    /* CF_SORT_CATEGORY: INTERNET(0)=A, MEDIA(1)=M, GAMES(4)=Z */
    cf_sort(&d, CF_SORT_CATEGORY);
    CHECK(d.slots[0].id == sA && d.slots[1].id == sM && d.slots[2].id == sZ);

    /* CF_SORT_MRU: touch in a specific order via opening, newest first */
    cf_open_single(&d, sM, tick());  t2 = cf_find_slot(&d, sM)->cards[0].used_ms;
    cf_open_single(&d, sZ, tick());  t3 = cf_find_slot(&d, sZ)->cards[0].used_ms;
    cf_open_single(&d, sA, tick());  t1 = cf_find_slot(&d, sA)->cards[0].used_ms;
    CHECK(t1 > t3 && t3 > t2);
    cf_sort(&d, CF_SORT_MRU);
    CHECK(d.slots[0].id == sA && d.slots[1].id == sZ && d.slots[2].id == sM);

    /* CF_SORT_UPDATED: unread first, then newest activity */
    cf_mark_activity(&d, cf_find_slot(&d, sM)->cards[0].id, tick());
    cf_sort(&d, CF_SORT_UPDATED);
    CHECK(d.slots[0].id == sM); /* the only unread one sorts first regardless of MRU */
}

// ---------------------------------------------------------------------------
// open states: single / group-split / two-column
// ---------------------------------------------------------------------------

static void test_open_states(void) {
    cf_deck_t d;
    uint32_t s1, s2, s3, g;
    int i;
    char path[32], title[32];

    cf_deck_init(&d);
    s1 = cf_add_card(&d, "A", "A", 0, 0, tick(), 1, 0);
    CHECK(cf_current_mode(&d) == CF_MODE_SINGLE);

    s2 = cf_add_card(&d, "B", "B", 0, 0, tick(), 0, 0);
    CHECK(cf_open_second_as_column(&d, s2, 300, tick()));
    CHECK(cf_current_mode(&d) == CF_MODE_COLUMNS);
    CHECK(cf_find_slot(&d, s1)->open == 1); /* s1 untouched by opening s2 as a column */
    CHECK(cf_find_slot(&d, s2)->w == 300);

    s3 = cf_add_card(&d, "C", "C", 0, 0, tick(), 0, 0);
    CHECK(cf_open_second_as_column(&d, s3, 0, tick()));
    CHECK(cf_open_count(&d) == 3);
    CHECK(cf_current_mode(&d) == CF_MODE_COLUMNS); /* N-column, not capped at 2 */

    /* stow the columns down to one -> falls back to SINGLE/GROUP_SPLIT reporting */
    cf_stow_slot(&d, s2);
    cf_stow_slot(&d, s3);
    CHECK(cf_open_count(&d) == 1);
    CHECK(cf_current_mode(&d) == CF_MODE_SINGLE);

    /* group-split: build an 8-card group and confirm the mode */
    g = cf_add_card(&d, "G0", "G0", 0, 0, tick(), 0, 0);
    for (i = 1; i < 8; i++) {
        uint32_t s;
        sprintf(path, "G%d", i);
        sprintf(title, "G%d", i);
        s = cf_add_card(&d, path, title, 0, 0, tick(), 0, 0);
        CHECK(cf_group_card_into(&d, cf_find_slot(&d, s)->cards[0].id, g, tick()));
    }
    CHECK(cf_open_single(&d, g, tick()));
    CHECK(cf_current_mode(&d) == CF_MODE_GROUP_SPLIT);
    CHECK(cf_open_count(&d) == 1);
}

// ---------------------------------------------------------------------------
// geometry: deck walk, tab rects non-overlap, pane tiling, column clamp
// ---------------------------------------------------------------------------

static void test_geometry(void) {
    cf_deck_t d;
    cf_geom_t geom = {0};   // (cfdock) origin_x/origin_y/screen_w_raw/screen_h_raw default 0
    cf_slot_layout_t lay[16];
    int n, i;
    uint32_t ids[6];
    char path[32], title[32];

    cf_deck_init(&d);
    for (i = 0; i < 6; i++) {
        sprintf(path, "S%d", i);
        sprintf(title, "Slot %d", i);
        ids[i] = cf_add_card(&d, path, title, 0, 0, tick(), 0, 0);
    }
    /* open slot 2 as the sole open slot */
    cf_open_single(&d, ids[2], tick());

    geom.screen_w = 1280;
    geom.screen_h = 800;
    geom.rail_w = 28;
    geom.edge_w = cf_edge_width(28, geom.rail_w, d.nslots, geom.screen_w);
    geom.tab_w = 30;
    geom.tab_top = 48;
    geom.foot = 96;
    geom.tab_step = cf_tab_step(30, geom.tab_top, geom.foot, CF_TAB_MAX_H, d.nslots, geom.screen_h);

    CHECK(geom.edge_w >= CF_EDGE_FLOOR && geom.edge_w <= 28);
    CHECK(geom.tab_step >= CF_STEP_FLOOR);

    n = cf_layout(&d, &geom, lay, 16);
    CHECK(n == 6);

    /* deck-walk invariants: x advances monotonically, one body open, edges
       tile with no gaps or overlaps */
    for (i = 0; i < n; i++) {
        CHECK(lay[i].body_x == lay[i].edge_x + geom.edge_w);
        if (i + 1 < n) CHECK(lay[i + 1].edge_x == lay[i].edge_x + geom.edge_w + lay[i].body_w);
    }
    {
        int open_bodies = 0;
        for (i = 0; i < n; i++) if (lay[i].body_w > 0) open_bodies++;
        CHECK(open_bodies == 1);
        CHECK(lay[2].body_w > 0);
    }

    /* rail rect */
    {
        cf_rect_t rr = cf_rail_rect(&geom);
        CHECK(rr.x == 0 && rr.y == 0 && rr.w == geom.rail_w && rr.h == geom.screen_h);
    }

    /* tab rects: position/size formula. NOTE: adjacent stowed plates are NOT
       required to avoid overlap (that is the intended shingled "staircase"
       look, resolved by z-order/depth - see cf_tab_rect()'s doc comment).
       What IS an invariant: x is centred on the edge, y advances by exactly
       tab_step per deck position, height honours the label/button budget and
       is capped at CF_TAB_MAX_H. */
    {
        cf_rect_t tabs[6];
        for (i = 0; i < n; i++) {
            int label_len = 12 + i * 3; /* short, single-word-ish labels */
            int is_open = lay[i].body_w > 0;
            tabs[i] = cf_tab_rect(&geom, &lay[i], i, label_len, is_open);
            CHECK(tabs[i].w == geom.tab_w);
            CHECK(tabs[i].x == lay[i].edge_x + (geom.edge_w - geom.tab_w) / 2);
            CHECK(tabs[i].y == geom.tab_top + i * geom.tab_step);
            CHECK(tabs[i].h <= CF_TAB_MAX_H);
            CHECK(tabs[i].h >= label_len + CF_TAB_PAD - 1); /* not clamped away at this size */
            if (is_open) CHECK(tabs[i].h >= CF_TAB_BTN_COUNT * CF_TAB_BTN_H);
        }
        /* consecutive plate tops strictly descend with deck order */
        for (i = 0; i + 1 < n; i++) CHECK(tabs[i + 1].y > tabs[i].y);
        /* a wildly long label is still capped, never exceeding CF_TAB_MAX_H */
        {
            cf_rect_t huge = cf_tab_rect(&geom, &lay[0], 0, 10000, 1);
            CHECK(huge.h == CF_TAB_MAX_H);
        }
    }

    /* width-at-pointer: neighbour-preserving clamp respects later slots */
    {
        int w_small = cf_width_at_pointer(&d, &geom, lay, n, ids[2], 10);
        int w_huge  = cf_width_at_pointer(&d, &geom, lay, n, ids[2], 5000);
        CHECK(w_small == CF_CARD_MIN_W);          /* clamped up to the floor */
        CHECK(w_huge < 5000);                      /* clamped down: room reserved for later slots */
        CHECK(w_huge >= CF_CARD_MIN_W);
    }

    /* column divider rect sits between two adjacent entries */
    {
        cf_rect_t dr = cf_column_divider_rect(&geom, &lay[1], &lay[2]);
        CHECK(dr.w == CF_DIVIDER_HIT);
        CHECK(dr.x <= lay[2].edge_x && dr.x + dr.w >= lay[2].edge_x);
    }
}

static void test_group_pane_tiling(void) {
    cf_deck_t d;
    uint32_t g;
    int i;
    cf_rect_t body, panes[8];
    int n;
    char path[32], title[32];

    cf_deck_init(&d);
    g = cf_add_card(&d, "G0", "G0", 0, 0, tick(), 0, 0);
    for (i = 1; i < 5; i++) { /* 5 cards -> 2 rows of 3 then 2 */
        sprintf(path, "G%d", i);
        sprintf(title, "G%d", i);
        {
            uint32_t s = cf_add_card(&d, path, title, 0, 0, tick(), 0, 0);
            CHECK(cf_group_card_into(&d, cf_find_slot(&d, s)->cards[0].id, g, tick()));
        }
    }
    {
        cf_slot_t *slot = cf_find_slot(&d, g);
        CHECK(slot->ncards == 5);
        CHECK(slot->nrows == 2);
        CHECK(slot->row_cols[0] == 3 && slot->row_cols[1] == 2);

        body.x = 100; body.y = 0; body.w = 903; body.h = 800; /* odd size to exercise rounding */
        n = cf_group_pane_rects(slot, body, panes, 8);
        CHECK(n == 5);

        /* tiling invariant: every row's panes sum to body.w exactly, every
           column's rows sum to body.h exactly, and panes never overlap */
        {
            int row0_w = panes[0].w + panes[1].w + panes[2].w;
            int row1_w = panes[3].w + panes[4].w;
            CHECK(row0_w == body.w);
            CHECK(row1_w == body.w);
            CHECK(panes[0].h + panes[3].h == body.h); /* column 0 spans both rows */
            CHECK(panes[0].y == body.y);
            CHECK(panes[3].y == panes[0].y + panes[0].h);
            CHECK(panes[0].x == body.x);
            CHECK(panes[1].x == panes[0].x + panes[0].w);
            CHECK(panes[2].x + panes[2].w == body.x + body.w);
            CHECK(panes[4].x + panes[4].w == body.x + body.w);
        }

        /* divider drag: move the row divider down by 50px, re-tile, and
           confirm the neighbour-preserving clamp + the new split is honoured */
        CHECK(cf_set_row_divider(&d, g, 1, panes[0].h, panes[3].h, 50));
        n = cf_group_pane_rects(cf_find_slot(&d, g), body, panes, 8);
        CHECK(panes[0].h + panes[3].h == body.h); /* still tiles exactly */
        CHECK(panes[0].h > 0 && panes[3].h > 0);

        /* an extreme drag is clamped to CF_PANE_MIN on the losing side */
        CHECK(cf_set_row_divider(&d, g, 1, panes[0].h, panes[3].h, -100000));
        n = cf_group_pane_rects(cf_find_slot(&d, g), body, panes, 8);
        CHECK(panes[3].h >= CF_PANE_MIN);
        CHECK(panes[0].h + panes[3].h == body.h);

        /* column divider within row 0 (3 panes: dividers at col 1 and col 2) */
        CHECK(cf_set_col_divider(&d, g, 0, 1, panes[0].w, panes[1].w, 20));
        n = cf_group_pane_rects(cf_find_slot(&d, g), body, panes, 8);
        CHECK(panes[0].w + panes[1].w + panes[2].w == body.w);
        (void)n;
    }
}

/* (cfmaxwidth, owner correction) A 2-member group must stack top over
   bottom (2 rows x 1 column), NOT side by side - and the divider between
   the two panes must be HORIZONTAL (drags height, clamped by CF_PANE_MIN
   on height), never vertical. */
static void test_two_card_group_is_vertical_stack(void) {
    cf_deck_t d;
    uint32_t g, s2;
    cf_rect_t body, panes[8];
    int n;

    cf_deck_init(&d);
    g  = cf_add_card(&d, "TOP", "Top", 0, 0, tick(), 0, 0);
    s2 = cf_add_card(&d, "BOTTOM", "Bottom", 0, 0, tick(), 0, 0);
    CHECK(cf_group_card_into(&d, cf_find_slot(&d, s2)->cards[0].id, g, tick()));

    {
        cf_slot_t *slot = cf_find_slot(&d, g);
        CHECK(slot->ncards == 2);
        /* the grid SHAPE is the vertical stack: 2 rows, 1 column each */
        CHECK(slot->nrows == 2);
        CHECK(slot->row_cols[0] == 1 && slot->row_cols[1] == 1);

        body.x = 50; body.y = 0; body.w = 640; body.h = 481; /* odd height for rounding */
        n = cf_group_pane_rects(slot, body, panes, 8);
        CHECK(n == 2);

        /* top-over-bottom: same x/w (full width each), stacked by y/h,
           exactly tiling body with no gap/overlap - a horizontal split. */
        CHECK(panes[0].x == body.x && panes[1].x == body.x);
        CHECK(panes[0].w == body.w && panes[1].w == body.w);
        CHECK(panes[0].y == body.y);
        CHECK(panes[1].y == panes[0].y + panes[0].h);
        CHECK(panes[0].h + panes[1].h == body.h);

        /* the divider is HORIZONTAL: cf_set_row_divider (height) works... */
        CHECK(cf_set_row_divider(&d, g, 1, panes[0].h, panes[1].h, 40));
        n = cf_group_pane_rects(cf_find_slot(&d, g), body, panes, 8);
        CHECK(panes[0].h + panes[1].h == body.h);
        CHECK(panes[0].h != panes[1].h);      /* the drag actually moved something */
        CHECK(panes[0].w == body.w && panes[1].w == body.w);   /* widths untouched */

        /* ...clamped on HEIGHT by CF_PANE_MIN on an extreme drag... */
        CHECK(cf_set_row_divider(&d, g, 1, panes[0].h, panes[1].h, -100000));
        n = cf_group_pane_rects(cf_find_slot(&d, g), body, panes, 8);
        CHECK(panes[0].h >= CF_PANE_MIN);
        CHECK(panes[0].h + panes[1].h == body.h);

        /* ...and there is NO vertical divider to drag: row 0 (and row 1)
           has exactly 1 column, so col is never in [1, row_cols[row]-1]
           and cf_set_col_divider() must refuse for every row. */
        CHECK(cf_set_col_divider(&d, g, 0, 1, panes[0].w, panes[1].w, 20) == 0);
        CHECK(cf_set_col_divider(&d, g, 1, 1, panes[0].w, panes[1].w, 20) == 0);
        n = cf_group_pane_rects(cf_find_slot(&d, g), body, panes, 8);
        CHECK(panes[0].w == body.w && panes[1].w == body.w);   /* still untouched */
        (void)n;
    }
}

static void test_clamp_divider_unit(void) {
    cf_divide_t d;
    d = cf_clamp_divider(200, 200, 30, CF_PANE_MIN);
    CHECK(d.a == 230 && d.b == 170 && d.a + d.b == 400);
    d = cf_clamp_divider(200, 200, -1000, CF_PANE_MIN);
    CHECK(d.a == CF_PANE_MIN && d.b == 400 - CF_PANE_MIN);
    d = cf_clamp_divider(200, 200, 1000, CF_PANE_MIN);
    CHECK(d.a == 400 - CF_PANE_MIN && d.b == CF_PANE_MIN);
}

static void test_depth_and_grid(void) {
    int row_cols[CF_MAX_GRID_ROWS];
    int open_idx[2];

    /* (cfmaxwidth, owner correction) a 2-member group is now a VERTICAL
       stack - 2 rows of 1 column each (top over bottom, horizontal
       divider) - not 1 row of 2 side-by-side columns. */
    CHECK(cf_group_grid(2, row_cols) == 2 && row_cols[0] == 1 && row_cols[1] == 1);
    CHECK(cf_group_grid(3, row_cols) == 2 && row_cols[0] == 2 && row_cols[1] == 1);
    CHECK(cf_group_grid(4, row_cols) == 2 && row_cols[0] == 2 && row_cols[1] == 2);
    CHECK(cf_group_grid(5, row_cols) == 2 && row_cols[0] == 3 && row_cols[1] == 2);
    CHECK(cf_group_grid(6, row_cols) == 2 && row_cols[0] == 3 && row_cols[1] == 3);
    CHECK(cf_group_grid(7, row_cols) == 3 && row_cols[0] == 3 && row_cols[1] == 3 && row_cols[2] == 1);
    CHECK(cf_group_grid(8, row_cols) == 3 && row_cols[0] == 3 && row_cols[1] == 3 && row_cols[2] == 2);

    open_idx[0] = 3;
    CHECK(cf_depth_distance(open_idx, 1, 3) == 0);
    CHECK(cf_depth_distance(open_idx, 1, 4) == 1);
    CHECK(cf_depth_distance(open_idx, 1, 0) == 3);
    CHECK(cf_depth_distance(open_idx, 1, 20) == CF_DEPTH_CAP); /* capped */
    CHECK(cf_depth_dim_x10(0) == 0);
    CHECK(cf_depth_dim_x10(CF_DEPTH_CAP) == CF_DEPTH_CAP * 45);

    open_idx[0] = 1; open_idx[1] = 8;
    CHECK(cf_depth_distance(open_idx, 2, 5) == 3); /* closer to idx 8 */
}

// ---------------------------------------------------------------------------
// (cfmaxwidth) width persistence across stow/reopen
// ---------------------------------------------------------------------------

static void test_width_persistence(void) {
    cf_deck_t d;
    uint32_t s1, s2;

    cf_deck_init(&d);
    s1 = cf_add_card(&d, "A", "A", 0, 0, tick(), 1, 0);
    s2 = cf_add_card(&d, "B", "B", 0, 0, tick(), 0, 0);

    /* a fresh card has no remembered width */
    CHECK(cf_find_slot(&d, s1)->pref_w == 0);

    /* dragging (cf_set_column_width) persists to pref_w immediately */
    CHECK(cf_open_second_as_column(&d, s2, 0, tick()));
    CHECK(cf_set_column_width(&d, s2, 420));
    CHECK(cf_find_slot(&d, s2)->w == 420);
    CHECK(cf_find_slot(&d, s2)->pref_w == 420);

    /* stow zeroes the LIVE width but keeps the remembered one */
    CHECK(cf_stow_slot(&d, s2));
    CHECK(cf_find_slot(&d, s2)->w == 0);
    CHECK(cf_find_slot(&d, s2)->pref_w == 420);

    /* reopening (as the sole open slot) restores it */
    CHECK(cf_open_single(&d, s2, tick()));
    CHECK(cf_find_slot(&d, s2)->w == 420);

    /* stow again, then reopen ALONGSIDE something else (second-as-column,
       w<=0 meaning "fill") - still restores pref_w, not fill */
    CHECK(cf_stow_slot(&d, s2));
    CHECK(cf_open_single(&d, s1, tick()));
    CHECK(cf_open_second_as_column(&d, s2, 0, tick()));
    CHECK(cf_find_slot(&d, s2)->w == 420);

    /* an explicit new width overrides AND re-persists */
    CHECK(cf_open_second_as_column(&d, s2, 300, tick()));
    CHECK(cf_find_slot(&d, s2)->w == 300);
    CHECK(cf_find_slot(&d, s2)->pref_w == 300);

    /* duplicating resets width to fill for BOTH sides, pref_w included */
    {
        uint32_t card_id = cf_find_slot(&d, s2)->cards[0].id;
        uint32_t dup = cf_duplicate_card(&d, card_id, tick());
        CHECK(dup != 0);
        CHECK(cf_find_slot(&d, s2)->w == 0 && cf_find_slot(&d, s2)->pref_w == 0);
        CHECK(cf_find_slot(&d, dup)->w == 0 && cf_find_slot(&d, dup)->pref_w == 0);
    }
}

// ---------------------------------------------------------------------------
// (cfmaxwidth) maximize + per-side collapse
// ---------------------------------------------------------------------------

static void test_maximize_and_collapse(void) {
    cf_deck_t d;
    uint32_t ids[7];
    int i, open_i;
    char path[32], title[32];
    cf_geom_t geom = {0};   // (cfdock) origin_x/origin_y/screen_w_raw/screen_h_raw default 0
    cf_slot_layout_t lay[16];
    int n;

    cf_deck_init(&d);
    for (i = 0; i < 7; i++) {
        sprintf(path, "M%d", i);
        sprintf(title, "Card %d", i);
        ids[i] = cf_add_card(&d, path, title, 0, 0, tick(), 0, 0);
    }
    /* open slot 3: two on its left (0,1,2), three on its right (4,5,6) */
    cf_open_single(&d, ids[3], tick());

    /* not maximized yet: no-op query, active() false */
    CHECK(cf_maximize_active(&d, &open_i) == 0);
    CHECK(cf_side_collapsed_count(&d, CF_SIDE_LEFT) == 0);
    CHECK(cf_side_collapsed_count(&d, CF_SIDE_RIGHT) == 0);
    CHECK(cf_toggle_side_collapse(&d, CF_SIDE_LEFT) == -1);   /* not maximized: refused */

    /* N-column mode: toggle refuses (no single "the open card") */
    CHECK(cf_open_second_as_column(&d, ids[5], 0, tick()));
    CHECK(cf_maximize_toggle(&d) == 0);
    CHECK(d.maximized == 0);
    cf_stow_slot(&d, ids[5]);   /* back to single-open */

    /* toggle ON: both sides start collapsed */
    CHECK(cf_maximize_toggle(&d) == 1);
    CHECK(d.maximized == 1 && d.left_collapsed == 1 && d.right_collapsed == 1);
    CHECK(cf_maximize_active(&d, &open_i) == 1);
    CHECK(d.slots[open_i].id == ids[3]);
    CHECK(cf_side_collapsed_count(&d, CF_SIDE_LEFT) == 3);
    CHECK(cf_side_collapsed_count(&d, CF_SIDE_RIGHT) == 3);

    /* geometry: cf_layout() reclaims the tab space - collapsed side members
       all share ONE edge_x/body_x, and the open slot's fill is wider than a
       non-maximized layout of the same deck would give it. */
    geom.screen_w = 1280; geom.screen_h = 800; geom.rail_w = 28;
    geom.edge_w = cf_edge_width(28, geom.rail_w, d.nslots, geom.screen_w);
    geom.tab_w = 30; geom.tab_top = 48; geom.foot = 96;
    geom.tab_step = cf_tab_step(30, geom.tab_top, geom.foot, CF_TAB_MAX_H, d.nslots, geom.screen_h);
    n = cf_layout(&d, &geom, lay, 16);
    CHECK(n == 7);
    {
        int k;
        for (k = 0; k < 3; k++) CHECK(lay[k].edge_x == lay[0].edge_x && lay[k].body_w == 0);
        for (k = 4; k < 7; k++) CHECK(lay[k].edge_x == lay[4].edge_x && lay[k].body_w == 0);
        CHECK(lay[3].body_w > 0);
        CHECK(lay[0].edge_x == geom.rail_w);                    /* left run starts right at the rail */
        CHECK(lay[4].edge_x == lay[3].body_x + lay[3].body_w);  /* right run starts right after the open body */
    }
    {
        /* un-maximized comparison deck: same 7 cards, same open slot,
           collapsed OFF - the open slot's body must be narrower there than
           in the maximized/collapsed layout above (space was reclaimed). */
        cf_deck_t d2; cf_slot_layout_t lay2[16]; int n2;
        cf_deck_init(&d2);
        for (i = 0; i < 7; i++) {
            sprintf(path, "M%d", i); sprintf(title, "Card %d", i);
            cf_add_card(&d2, path, title, 0, 0, tick(), 0, 0);
        }
        cf_open_single(&d2, d2.slots[3].id, tick());
        n2 = cf_layout(&d2, &geom, lay2, 16);
        CHECK(n2 == 7);
        CHECK(lay[3].body_w > lay2[3].body_w);
    }

    /* summary tab rect: same width/tab_top family as an ordinary tab. This
       is a host test with no cardtext.c (freestanding-only) to measure a
       real label with, so a plausible fixed pixel length stands in for
       cardtext_vertical_len("13 more cards +", 12) - cf_side_summary_tab_rect()
       only cares about the number, not how it was measured. */
    {
        int lbl = 90;
        cf_rect_t sr = cf_side_summary_tab_rect(&geom, lay[0].edge_x, lbl);
        CHECK(sr.w == geom.tab_w);
        CHECK(sr.x == lay[0].edge_x + (geom.edge_w - geom.tab_w) / 2);
        CHECK(sr.y == geom.tab_top);
    }

    /* expand one side without touching the other or the maximized flag */
    CHECK(cf_toggle_side_collapse(&d, CF_SIDE_LEFT) == 0);   /* now expanded */
    CHECK(d.left_collapsed == 0 && d.right_collapsed == 1 && d.maximized == 1);
    n = cf_layout(&d, &geom, lay, 16);
    {
        /* left run now walks individually again (own edge_x each, strictly
           increasing), right run still collapsed to one shared edge_x. */
        CHECK(lay[1].edge_x > lay[0].edge_x);
        CHECK(lay[2].edge_x > lay[1].edge_x);
        CHECK(lay[4].edge_x == lay[5].edge_x && lay[5].edge_x == lay[6].edge_x);
    }
    /* second click re-collapses */
    CHECK(cf_toggle_side_collapse(&d, CF_SIDE_LEFT) == 1);
    CHECK(d.left_collapsed == 1);

    /* opening a DIFFERENT card resets maximize/collapse entirely - "keep it
       intuitive": maximize never describes a stale open slot. */
    cf_open_single(&d, ids[0], tick());
    CHECK(d.maximized == 0 && d.left_collapsed == 0 && d.right_collapsed == 0);
    CHECK(cf_maximize_active(&d, &open_i) == 0);
}

// ---------------------------------------------------------------------------
// (cfdock) repositionable dock bars: thickness clamp, add/remove/move,
// edge-rect stacking, and cf_layout_ex()'s space reservation for both an
// EDGE dock (via cf_dock_reserve_edges() feeding cf_geom_t's origin/screen_w/
// screen_h) and a BETWEEN-cards dock (a gap inserted at a slot boundary that
// pushes later cards over).
// ---------------------------------------------------------------------------

static void test_dock_thickness_clamp_and_crud(void) {
    cf_deck_t d;
    cf_dock_pos_t pos;
    uint32_t id1, id2;

    CHECK(cf_dock_thickness_clamp(0) == CF_DOCK_THICKNESS_MIN);
    CHECK(cf_dock_thickness_clamp(5) == CF_DOCK_THICKNESS_MIN);
    CHECK(cf_dock_thickness_clamp(200) == CF_DOCK_THICKNESS_MAX);
    CHECK(cf_dock_thickness_clamp(40) == 40);

    cf_deck_init(&d);
    CHECK(d.ndocks == 0);   // "none" is the default - zero docks

    pos.kind = CF_DOCK_POS_EDGE; pos.edge = CF_DOCK_EDGE_BOTTOM; pos.slot_index = 0;
    id1 = cf_dock_add(&d, pos, 0);   // 0 -> default thickness
    CHECK(id1 != 0);
    CHECK(d.ndocks == 1);
    {
        cf_dock_t *dk = cf_dock_find(&d, id1);
        CHECK(dk != NULL);
        CHECK(dk->thickness == CF_DOCK_THICKNESS_DEFAULT);
        CHECK(dk->nitems == 3);   // seeded with TRAY, GAUGES, CLOCK
        CHECK(dk->items[0] == CF_DOCKITEM_TRAY);
        CHECK(dk->items[1] == CF_DOCKITEM_GAUGES);
        CHECK(dk->items[2] == CF_DOCKITEM_CLOCK);
        CHECK(cf_dock_is_vertical(dk) == 0);   // bottom edge = horizontal
    }

    CHECK(cf_dock_set_thickness(&d, id1, 999) == 1);
    CHECK(cf_dock_find(&d, id1)->thickness == CF_DOCK_THICKNESS_MAX);

    {
        int items[2] = { CF_DOCKITEM_CLOCK, CF_DOCKITEM_TRAY };
        CHECK(cf_dock_set_items(&d, id1, items, 2) == 1);
        cf_dock_t *dk = cf_dock_find(&d, id1);
        CHECK(dk->nitems == 2 && dk->items[0] == CF_DOCKITEM_CLOCK && dk->items[1] == CF_DOCKITEM_TRAY);
    }
    {
        int items[2] = { CF_DOCKITEM_CLOCK, 99 /* out of range: dropped */ };
        CHECK(cf_dock_set_items(&d, id1, items, 2) == 1);
        CHECK(cf_dock_find(&d, id1)->nitems == 1);
    }

    /* move: bottom edge -> left edge -> between cards (clamped to 0, deck is empty) */
    pos.kind = CF_DOCK_POS_EDGE; pos.edge = CF_DOCK_EDGE_LEFT; pos.slot_index = 0;
    CHECK(cf_dock_set_pos(&d, id1, pos) == 1);
    CHECK(cf_dock_is_vertical(cf_dock_find(&d, id1)) == 1);   // left edge = vertical
    pos.kind = CF_DOCK_POS_BETWEEN; pos.slot_index = 7;   // out of range: deck has 0 slots
    CHECK(cf_dock_set_pos(&d, id1, pos) == 1);
    CHECK(cf_dock_find(&d, id1)->pos.slot_index == 0);    // clamped, not rejected
    CHECK(cf_dock_is_vertical(cf_dock_find(&d, id1)) == 1);   // between = always vertical

    pos.kind = CF_DOCK_POS_EDGE; pos.edge = CF_DOCK_EDGE_TOP; pos.slot_index = 0;
    id2 = cf_dock_add(&d, pos, 40);
    CHECK(id2 != 0 && d.ndocks == 2);
    CHECK(cf_dock_find(&d, id2)->thickness == 40);

    CHECK(cf_dock_remove(&d, id1) == 1);
    CHECK(d.ndocks == 1);
    CHECK(cf_dock_find(&d, id1) == NULL);
    CHECK(cf_dock_find(&d, id2) != NULL);   // the remaining dock shifted into slot 0, same id
    CHECK(cf_dock_remove(&d, id1) == 0);    // already gone

    /* CF_MAX_DOCKS ceiling */
    {
        cf_deck_t d2; cf_deck_init(&d2);
        cf_dock_pos_t p2; p2.kind = CF_DOCK_POS_EDGE; p2.edge = CF_DOCK_EDGE_BOTTOM; p2.slot_index = 0;
        int k, added = 0;
        for (k = 0; k < CF_MAX_DOCKS + 2; k++) if (cf_dock_add(&d2, p2, 0)) added++;
        CHECK(added == CF_MAX_DOCKS);
        CHECK(d2.ndocks == CF_MAX_DOCKS);
    }
}

static void test_dock_edge_rect_stacking(void) {
    cf_deck_t d;
    cf_dock_pos_t pos;
    uint32_t top1, top2, bottom1, left1, right1;

    cf_deck_init(&d);
    pos.kind = CF_DOCK_POS_EDGE; pos.slot_index = 0;
    pos.edge = CF_DOCK_EDGE_TOP;    top1    = cf_dock_add(&d, pos, 30);
    pos.edge = CF_DOCK_EDGE_TOP;    top2    = cf_dock_add(&d, pos, 20);
    pos.edge = CF_DOCK_EDGE_BOTTOM; bottom1 = cf_dock_add(&d, pos, 40);
    pos.edge = CF_DOCK_EDGE_LEFT;   left1   = cf_dock_add(&d, pos, 25);
    pos.edge = CF_DOCK_EDGE_RIGHT;  right1  = cf_dock_add(&d, pos, 35);

    {
        cf_rect_t r = cf_dock_edge_rect(&d, cf_dock_find(&d, top1), 1000, 600);
        CHECK(r.x == 0 && r.y == 0 && r.w == 1000 && r.h == 30);
    }
    {
        /* second TOP dock stacks BELOW the first (array order = outward-in) */
        cf_rect_t r = cf_dock_edge_rect(&d, cf_dock_find(&d, top2), 1000, 600);
        CHECK(r.x == 0 && r.y == 30 && r.w == 1000 && r.h == 20);
    }
    {
        cf_rect_t r = cf_dock_edge_rect(&d, cf_dock_find(&d, bottom1), 1000, 600);
        CHECK(r.x == 0 && r.y == 600 - 40 && r.w == 1000 && r.h == 40);
    }
    {
        /* LEFT/RIGHT fit BETWEEN the (combined 50px) top band and the 40px
           bottom band, not the full raw height. */
        cf_rect_t r = cf_dock_edge_rect(&d, cf_dock_find(&d, left1), 1000, 600);
        CHECK(r.x == 0 && r.y == 50 && r.w == 25 && r.h == 600 - 50 - 40);
    }
    {
        cf_rect_t r = cf_dock_edge_rect(&d, cf_dock_find(&d, right1), 1000, 600);
        CHECK(r.x == 1000 - 35 && r.y == 50 && r.w == 35 && r.h == 600 - 50 - 40);
    }

    {
        int ox, oy, ow, oh;
        cf_dock_reserve_edges(&d, 1000, 600, &ox, &oy, &ow, &oh);
        CHECK(ox == 25 && oy == 50);              /* left1 + (top1+top2) */
        CHECK(ow == 1000 - 25 - 35 && oh == 600 - 50 - 40);
    }

    /* a dock the wrong kind for the query returns a zero rect */
    {
        cf_deck_t d3; cf_deck_init(&d3);
        cf_dock_pos_t bp; bp.kind = CF_DOCK_POS_BETWEEN; bp.slot_index = 0;
        uint32_t bid = cf_dock_add(&d3, bp, 30);
        cf_rect_t r = cf_dock_edge_rect(&d3, cf_dock_find(&d3, bid), 1000, 600);
        CHECK(r.w == 0 && r.h == 0);
    }
}

// A screen with no docks at all must render EXACTLY as before this feature:
// origin (0,0), working size == raw size. This is the "zero docks is a valid,
// unperturbed default" invariant the owner explicitly asked for.
static void test_dock_zero_is_unperturbed(void) {
    cf_deck_t d;
    cf_deck_init(&d);
    int ox, oy, ow, oh;
    cf_dock_reserve_edges(&d, 1280, 800, &ox, &oy, &ow, &oh);
    CHECK(ox == 0 && oy == 0 && ow == 1280 && oh == 800);
}

// EDGE reservation feeding cf_geom_t.origin_x/origin_y/screen_w/screen_h:
// cf_rail_rect()/cf_edge_rect()/cf_body_rect() must all shift by the working
// area's origin, so cards never draw under a BOTTOM+LEFT dock pair.
static void test_dock_edge_reservation_shifts_deck(void) {
    cf_deck_t d;
    cf_dock_pos_t pos;
    cf_geom_t geom = {0};
    cf_slot_layout_t lay[8];
    uint32_t ids[3];
    int i, n;
    char path[8], title[8];

    cf_deck_init(&d);
    pos.kind = CF_DOCK_POS_EDGE; pos.slot_index = 0;
    pos.edge = CF_DOCK_EDGE_BOTTOM; cf_dock_add(&d, pos, 40);
    pos.edge = CF_DOCK_EDGE_LEFT;   cf_dock_add(&d, pos, 60);

    for (i = 0; i < 3; i++) {
        sprintf(path, "D%d", i); sprintf(title, "D%d", i);
        ids[i] = cf_add_card(&d, path, title, 0, 0, tick(), 0, 0);
    }
    cf_open_single(&d, ids[0], tick());

    cf_dock_reserve_edges(&d, 1280, 800, &geom.origin_x, &geom.origin_y,
                           &geom.screen_w, &geom.screen_h);
    geom.screen_w_raw = 1280; geom.screen_h_raw = 800;
    geom.rail_w = 28; geom.tab_w = 30; geom.tab_top = 48; geom.foot = 96;
    geom.edge_w = cf_edge_width(28, geom.rail_w, d.nslots, geom.screen_w);
    geom.tab_step = cf_tab_step(30, geom.tab_top, geom.foot, CF_TAB_MAX_H, d.nslots, geom.screen_h);

    CHECK(geom.origin_x == 60 && geom.origin_y == 0);
    CHECK(geom.screen_w == 1280 - 60 && geom.screen_h == 800 - 40);

    n = cf_layout(&d, &geom, lay, 8);
    CHECK(n == 3);
    {
        cf_rect_t rail = cf_rail_rect(&geom);
        CHECK(rail.x == 60 && rail.y == 0 && rail.h == geom.screen_h);
        cf_rect_t edge0 = cf_edge_rect(&geom, &lay[0]);
        CHECK(edge0.x >= 60 + geom.rail_w);   // never draws under the LEFT dock
        CHECK(edge0.y == 0 && edge0.h == geom.screen_h);   // never draws under the BOTTOM dock
        CHECK(edge0.y + edge0.h <= 800 - 40);
    }
}

// BETWEEN-cards dock: reserves a gap at a slot boundary and pushes every
// LATER card over by exactly its (clamped) thickness - the model's core
// "inserts a gap and pushes the later cards over" contract.
static void test_dock_between_cards_pushes_layout(void) {
    cf_deck_t d, d0;
    cf_geom_t geom = {0}, geom0 = {0};
    cf_slot_layout_t lay[8], lay0[8];
    uint32_t dock_id;
    int i, n, n0;
    char path[8], title[8];

    cf_deck_init(&d); cf_deck_init(&d0);
    for (i = 0; i < 4; i++) {
        sprintf(path, "B%d", i); sprintf(title, "B%d", i);
        cf_add_card(&d, path, title, 0, 0, tick(), 0, 0);
        sprintf(path, "B%d", i); sprintf(title, "B%d", i);
        cf_add_card(&d0, path, title, 0, 0, tick(), 0, 0);
    }
    /* dock BETWEEN slot 2 and slot 3 */
    {
        cf_dock_pos_t p; p.kind = CF_DOCK_POS_BETWEEN; p.slot_index = 2;
        dock_id = cf_dock_add(&d, p, 50);
    }

    geom.screen_w = geom0.screen_w = 1280; geom.screen_h = geom0.screen_h = 800;
    geom.screen_w_raw = geom0.screen_w_raw = 1280; geom.screen_h_raw = geom0.screen_h_raw = 800;
    geom.rail_w = geom0.rail_w = 28;
    geom.edge_w = geom0.edge_w = cf_edge_width(28, 28, 4, 1280);
    geom.tab_w = geom0.tab_w = 30; geom.tab_top = geom0.tab_top = 48; geom.foot = geom0.foot = 96;
    geom.tab_step = geom0.tab_step = cf_tab_step(30, 48, 96, CF_TAB_MAX_H, 4, 800);

    n  = cf_layout(&d,  &geom,  lay,  8);
    n0 = cf_layout(&d0, &geom0, lay0, 8);
    CHECK(n == 4 && n0 == 4);

    /* slots 0/1 (before the boundary) are UNCHANGED by the dock */
    CHECK(lay[0].edge_x == lay0[0].edge_x);
    CHECK(lay[1].edge_x == lay0[1].edge_x);
    /* slots 2/3 (at and after the boundary) are pushed right by the dock's
       thickness - the gap sits between slot 1's body and slot 2's edge. */
    CHECK(lay[2].edge_x == lay0[2].edge_x + 50);
    CHECK(lay[3].edge_x == lay0[3].edge_x + 50);

    /* cf_dock_rect() reports the reserved gap's own rect: right after slot
       1's full width, exactly `thickness` wide, spanning the deck's height. */
    {
        cf_dock_t *dk = cf_dock_find(&d, dock_id);
        cf_rect_t r = cf_dock_rect(&d, dk, &geom, lay, n);
        CHECK(r.w == 50);
        CHECK(r.h == geom.screen_h);
        CHECK(r.x == lay[1].body_x + lay[1].body_w);
        CHECK(r.x + r.w == lay[2].edge_x);
    }

    /* moving the SAME dock to boundary 0 (right after the rail, before every
       card) pushes ALL FOUR cards over instead. */
    {
        cf_dock_pos_t p; p.kind = CF_DOCK_POS_BETWEEN; p.slot_index = 0;
        CHECK(cf_dock_set_pos(&d, dock_id, p) == 1);
        n = cf_layout(&d, &geom, lay, 8);
        CHECK(n == 4);
        for (i = 0; i < 4; i++) CHECK(lay[i].edge_x == lay0[i].edge_x + 50);
    }
}

static void test_dock_pack_unpack_roundtrip(void) {
    cf_dock_t dk;
    cf_dock_pos_t out_pos; int out_th, out_items[CF_DOCKITEM_COUNT], out_n;

    memset(&dk, 0, sizeof(dk));
    dk.pos.kind = CF_DOCK_POS_EDGE; dk.pos.edge = CF_DOCK_EDGE_RIGHT;
    dk.thickness = 55;
    dk.items[0] = CF_DOCKITEM_CLOCK; dk.items[1] = CF_DOCKITEM_TRAY; dk.nitems = 2;

    int packed = cf_dock_pack(&dk);
    cf_dock_unpack(packed, &out_pos, &out_th, out_items, &out_n);
    CHECK(out_pos.kind == CF_DOCK_POS_EDGE && out_pos.edge == CF_DOCK_EDGE_RIGHT);
    CHECK(out_th == 55);
    /* item ORDER is not preserved (bitmask - see cf_dock_pack()'s own
       comment); canonical CF_DOCKITEM_* order comes back instead. */
    CHECK(out_n == 2);
    CHECK(out_items[0] == CF_DOCKITEM_TRAY);
    CHECK(out_items[1] == CF_DOCKITEM_CLOCK);

    /* a BETWEEN dock round-trips its slot_index the same way */
    dk.pos.kind = CF_DOCK_POS_BETWEEN; dk.pos.slot_index = 9; dk.thickness = 20;
    packed = cf_dock_pack(&dk);
    cf_dock_unpack(packed, &out_pos, &out_th, out_items, &out_n);
    CHECK(out_pos.kind == CF_DOCK_POS_BETWEEN && out_pos.slot_index == 9);
    CHECK(out_th == 20);
}

int main(void) {
    test_add_close_stow_dup_color();
    test_grouping_and_ungroup();
    test_sorting();
    test_open_states();
    test_geometry();
    test_group_pane_tiling();
    test_two_card_group_is_vertical_stack();
    test_clamp_divider_unit();
    test_depth_and_grid();
    test_width_persistence();
    test_maximize_and_collapse();
    test_dock_thickness_clamp_and_crud();
    test_dock_edge_rect_stacking();
    test_dock_zero_is_unperturbed();
    test_dock_edge_reservation_shifts_deck();
    test_dock_between_cards_pushes_layout();
    test_dock_pack_unpack_roundtrip();

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
