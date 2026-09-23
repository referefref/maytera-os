// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// cardfile_model.h - the Cardfile deck DATA MODEL: cards, slots, groups, open
// state, sort, and the pure geometry math the deck-walk needs. Agent B of
// docs/CARDFILE_ARCHITECTURE.md's decomposition.
//
// PURE DATA + LOGIC. No drawing, no syscalls, no blocking, no clock reads: any
// operation that needs "now" takes it as an explicit `now_ms` parameter (the
// caller gets it from uptime_ms(), which this file must never call itself, so
// the whole model stays a deterministic function of its inputs and is fully
// testable on a host compiler with no VM). This header is SELF-SUFFICIENT: it
// includes only <stdint.h> and uses raw C types, so it can be pulled into a
// host unit test without any compositor or kernel headers.
//
// THE MODEL (see docs/CARDFILE_ARCHITECTURE.md section 5 and the porting
// spec's "Design notes" drawer at cardfile-mockup.html for the numbers this
// was ported from):
//
//   A CARD is one hosted app: an id, a title, a launch app_path, a category
//   (the start-menu taxonomy index - the taxonomy ITSELF lives on disk in
//   build/assets/startmenu/system.d/*.MENU and is not duplicated here), a
//   palette colour index, MRU/created/updated timestamps, an unread flag, and
//   the kernel window id hosting it (0 until agent C launches the app and
//   binds it; this file only holds the field).
//
//   A SLOT is one deck position holding 1..CF_MAX_CARDS_PER_SLOT cards. A
//   slot with one card is an ordinary card; a slot with 2+ cards is a GROUP,
//   which the spec tiles into a grid when opened (cf_group_grid()).
//
//   The DECK is an ordered list of slots, left to right. Zero or more slots
//   are OPEN at once: none open, one open (single card, or a group tiled into
//   panes - "group-split" is not separate model state, it falls out of
//   "this open slot has >1 card"), or two-or-more open side by side
//   ("columns" - the spec's headline case is exactly two, but nothing in the
//   model caps it there; cf_layout() shares the remaining width among
//   however many are open with no explicit width, which is what makes N
//   columns work for free). cf_current_mode() names the three cases.
//
// GEOMETRY: every helper below takes plain, ALREADY-RESOLVED pixel inputs
// (edge width, tab step, etc.) rather than computing UI-scaled values itself.
// ui_px()/theme selection is compositor-owned (main.c/draw.c); this file only
// does the arithmetic the spec's "Deck walk" paragraph specifies, so the same
// code runs unchanged whether the caller fed it retro-UNIX or modern-glass
// metrics, and so a host test can drive it with plain numbers.
//
// RESOLVED AMBIGUITIES (the spec/mockup did not pin these down; decisions are
// recorded here so agents A/C/D/F all see the same answer):
//
//   1. Deck capacity. Neither the architecture doc nor the mockup states a
//      maximum number of slots. Chosen: CF_MAX_SLOTS 32, matching the
//      kernel's own window-snapshot cap (wm_window_info_t wins[32],
//      main.c:354) since every launched card eventually owns a live kernel
//      window under that same ceiling.
//   2. Timestamps. The mockup reads Date.now() directly inside its model
//      functions. This file takes `now_ms` as an explicit parameter on every
//      op that touches a timestamp instead, so the model has no hidden clock
//      dependency (#426's "never block/poll" spirit extends to "never read
//      global state you weren't handed" for a host-testable pure module).
//   3. cf_duplicate_card() width. The mockup's dupSlot() computes an explicit
//      half-of-remaining-space column width using deskW()/edgeW() at call
//      time. This file instead resets BOTH the source and the new slot's
//      width to 0 ("fill") and lets cf_layout()'s existing even-split-among-
//      fills logic produce the same roughly-half result, so the mutation op
//      does not need a screen size passed in.
//   4. cf_ungroup_all(). The spec defines "pull one member out" but not
//      "explode a whole group at once". Implemented as: the original slot
//      keeps its id, open state and its first member; every other member
//      becomes its own new, STOWED slot inserted immediately after, in
//      member order (repeated cf_ungroup_pull_out() with open_it=0).
//   5. cf_group_slots() (group two whole slots in one call, not just one
//      card) is not in the mockup either (its drag-and-drop only ever moves
//      one card at a time); added because the task explicitly asks for it.
//      It is all-or-nothing: if src's members would not all fit in dst
//      (dst.ncards + src.ncards > CF_MAX_CARDS_PER_SLOT), NOTHING moves.
//
// (cfmaxwidth) FREE-WIDTH CARDS + MAXIMIZE/COLLAPSE (owner request, no ticket):
//
//   6. Width PERSISTENCE. cf_slot_t.w is TRANSIENT live-layout state (0 =
//      fill; cf_stow_slot()/cf_open_single() zero the STOWED side's w so
//      cf_layout()'s fill-share math stays simple). That means a user's
//      chosen width used to evaporate the moment a card was stowed and
//      reopened. cf_slot_t.pref_w is the FIX: the last explicit width the
//      user dragged to, persisted across stow/reopen. cf_set_column_width()
//      writes both (immediate + remembered); cf_open_single()/
//      cf_open_second_as_column() restore w from pref_w when the caller asks
//      for "fill" (w<=0), so reopening a card the user had widened comes
//      back at that width, not full-fill, until the user chooses otherwise.
//      cf_add_card()/cf_duplicate_card() leave pref_w at its zeroed default
//      (a fresh card has no remembered width; ambiguity #3 above already
//      resets w to 0 for both sides of a duplicate, so pref_w following the
//      same reset is the consistent choice, not a divergence from it).
//
// (cfdock) REPOSITIONABLE DOCK BARS (owner request, no ticket, "cfdock"):
//
//   8. Position model. A dock sits either at a SCREEN EDGE (top/bottom/left/
//      right - cf_dock_edge_t) or BETWEEN CARDS, anchored at a deck-order
//      slot BOUNDARY index in [0, deck->nslots] (0 = immediately right of the
//      rail, before the first card; nslots = immediately after the last
//      card). A tagged union (cf_dock_pos_t) rather than two separate dock
//      lists, so cf_dock_set_pos() can move a dock from an edge to a
//      boundary (or back) by just overwriting the tag - "move it to any
//      screen edge or between cards" is ONE operation, not a family of them.
//   9. Orientation follows position, it is not a separate field: TOP/BOTTOM
//      edge docks are HORIZONTAL bars (span the working width, customisable
//      HEIGHT); LEFT/RIGHT edge docks and EVERY between-cards dock are
//      VERTICAL bars (a between-cards dock runs perpendicular to the deck's
//      left-to-right card flow, exactly like a LEFT/RIGHT edge dock - both
//      customise their WIDTH). cf_dock_t.thickness is the one field for
//      either axis; callers read cf_dock_is_vertical() to know which.
//  10. Geometry / reservation lives in TWO places, matched to what each dock
//      kind needs to know:
//        - EDGE docks only need the raw screen size (cf_dock_reserve_edges(),
//          cf_dock_edge_rect()) - stacking with any other dock sharing that
//          edge (array order = stacking order, outward to inward) is pure
//          arithmetic over deck->docks[], no card layout involved.
//        - BETWEEN docks need the CURRENT card layout (where the cards are
//          determines where the gap is), so they are computed INSIDE the
//          existing deck-walk (cf_layout_ex(), which cf_layout() now wraps -
//          every existing caller/test is unaffected) as one more thing the
//          x-walk reserves space for, exactly like the cfmaxwidth collapse
//          logic already reserves/frees edge units. cf_dock_rect() is the ONE
//          entry point renderers/hit-testers use for either kind (it needs
//          geom+lay for the BETWEEN case, which is why its signature is
//          geom/lay-based rather than the bare screen_w/screen_h a
//          screen-only EDGE query would have sufficed for - one function,
//          not two, for the caller to remember).
//      cf_geom_t gained origin_x/origin_y (where the WORKING area - what
//      cf_layout()'s screen_w/screen_h already meant - starts on the real
//      framebuffer) plus screen_w_raw/screen_h_raw (the real, undocked
//      framebuffer size, needed only to compute EDGE dock rects, which live
//      OUTSIDE the working area cf_layout() reasons about). Existing callers
//      that build a cf_geom_t by hand (the host test) must set the two origin
//      fields to 0 and the two raw fields equal to screen_w/screen_h (or
//      simply zero-initialize the struct first) - a garbage stack value in a
//      field cf_rail_rect()/cf_edge_rect()/cf_body_rect() now read would
//      silently misplace every rect, so this is a REQUIRED update at every
//      existing call site, not an additive no-op.
//  11. Content is an ORDERED list of CF_DOCKITEM_* (cf_dock_t.items[],
//      nitems), so a dock can show a subset, and a future item kind is one
//      more enum value + one more render-side case, not a model change.
//      cf_dock_add() seeds every new dock with the full v1 set (TRAY, GAUGES,
//      CLOCK, in that reading order) - "0, 1 or more docks" already covers
//      "none", so a freshly-added dock should look useful immediately rather
//      than needing a second trip through a content picker. GAUGES is ONE
//      item, not three (CPU/RAM/NET separately), because it reuses the
//      classic taskbar's own gauge_hit()/draw_perf_popup() contract, which
//      already treats CPU+RAM+DSK+NET as one contiguous, atomic block (see
//      cardfile.c/taskbar.c's own cfdock notes) - splitting it into
//      independently placeable items would fork that contract, not reuse it.
//  12. Persistence (profile.c) packs one dock's {kind, edge_or_slot,
//      thickness, item bitmask} into a SINGLE int per dock (cfd0..cfd7 - see
//      cf_dock_pack_for_profile()/cf_dock_add_from_profile()), matching this
//      file's own "no floating point, no unnecessary structure" norm and
//      profile.c's existing flat-scalar-key format (put_kv() takes one int;
//      there is no list/array value type to reuse, so a bitmask trades the
//      items' USER-CHOSEN ORDER for fitting the existing format with zero
//      profile.c parser changes - reloaded docks always show their items in
//      canonical CF_DOCKITEM_* order, not necessarily the order they were in
//      when saved). A BETWEEN dock whose saved slot_index no longer fits the
//      deck at load time (cardfile does not persist CARDS, so the deck is
//      always empty when docks are restored) is clamped into range by
//      cf_dock_add() rather than dropped - it reappears at boundary 0 (right
//      after the rail) and stays valid as cards are added back.
//
//   7. MAXIMIZE + per-side summary tabs. deck->maximized widens whichever
//      slot is the SOLE open one (cf_open_count()==1 - there is no single
//      "the open card" to maximize in N-column mode, so cf_maximize_toggle()
//      is a no-op there) by collapsing the stowed run strictly left, and
//      strictly right, of it into ONE shared edge_w-wide unit each
//      (cf_layout() does this directly - see its own comment), which is
//      exactly the "reclaim the tab space" the owner asked for: cf_layout()
//      already gives the open slot's fill_w every pixel not spent on real
//      edges, so collapsing N stowed edges to 1 hands back (N-1)*edge_w for
//      free, no separate "host" width logic needed. left_collapsed/
//      right_collapsed independently gate whether a side is actually shown
//      as the one summary tab (collapsed) or its ordinary individual fan
//      (expanded) - cf_toggle_side_collapse() flips one side without
//      touching the other or deck->maximized itself. Because a stale
//      "maximized" view of a slot that is no longer the sole open one would
//      be nonsensical, EVERY op that changes which slot(s) are open
//      (cf_open_single(), cf_open_second_as_column(), cf_stow_slot(),
//      cf_close_slot(), cf_duplicate_card(), and cf_ensure_open()/
//      cf_close_card() transitively through the first two) calls
//      cf_maximize_reset() - so opening a different card, per the owner's
//      "keep it intuitive" ask, naturally drops back out of maximize/
//      collapsed view instead of describing a slot that is no longer open.

#ifndef CARDFILE_MODEL_H
#define CARDFILE_MODEL_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Limits (cf.* tokens from the porting spec's "Geometry" table, plus the
// capacity choices recorded above).
// ---------------------------------------------------------------------------

#define CF_MAX_CARDS_PER_SLOT 8      // cf.group_max
#define CF_GROUP_MAX          CF_MAX_CARDS_PER_SLOT   // spec name, same value
#define CF_MAX_SLOTS          32     // resolved ambiguity #1 above
#define CF_MAX_TITLE          48
#define CF_MAX_APP_PATH       128
#define CF_MAX_GRID_ROWS      3      // group grid is at most 3x3 (8 cards, last row short)
#define CF_MAX_GRID_COLS      3

#define CF_CARD_MIN_W   240   // cf.card_min_w: column resize floor
#define CF_PANE_MIN     120   // cf.pane_min: group split pane floor (both axes)
#define CF_DIVIDER_HIT  8     // divider hit-test width/height in px
#define CF_EDGE_FLOOR   14    // stowed edge never compresses below this
#define CF_EDGE_MAX_PCT 35    // ...and the fully-stowed deck never exceeds this % of screen width
#define CF_STEP_FLOOR   6     // tab ladder step never compresses below this
#define CF_COLUMN_GAP   4     // the spec's small fixed gap reserved past the last column

// Tab plate height budget: label (capped) + padding, plus five 24px buttons
// when the plate is the open one (colour/duplicate/maximize/stow/close -
// cfmaxwidth added the maximize button, CF_TAB_BTN_COUNT 4 -> 5).
// 150 + 18 + 5*24 = 288.
#define CF_TAB_LABEL_MAX 150
#define CF_TAB_PAD       18
#define CF_TAB_BTN_H     24
#define CF_TAB_BTN_COUNT 5
#define CF_TAB_MAX_H     (CF_TAB_LABEL_MAX + CF_TAB_PAD + CF_TAB_BTN_COUNT * CF_TAB_BTN_H)

#define CF_DEPTH_CAP 6   // depth-dim distance caps here (spec: dims 4.5%/step)

// ---------------------------------------------------------------------------
// Palette / taxonomy indices
// ---------------------------------------------------------------------------

// Eight card-colour keys, one hex per theme (the hex tables themselves are
// UI/rendering data owned by agent A/D; this is just the index contract every
// agent sorts and colours by).
enum {
    CF_COLOR_DEFAULT = 0,   // grey / paper / graphite
    CF_COLOR_SAND,
    CF_COLOR_SAGE,
    CF_COLOR_SLATE,
    CF_COLOR_ROSE,
    CF_COLOR_HEATHER,
    CF_COLOR_WHEAT,
    CF_COLOR_SPRUCE,
    CF_COLOR_COUNT
};

// Start-menu taxonomy order (build/assets/startmenu/system.d/*.MENU owns the
// actual category names and app listing; this is only the sort index).
enum {
    CF_CAT_INTERNET = 0,
    CF_CAT_MEDIA,
    CF_CAT_ACCESSORIES,
    CF_CAT_OFFICE,
    CF_CAT_GAMES,
    CF_CAT_SYSTEM,
    CF_CAT_COUNT
};

// Explicit, one-shot sort keys (never live - see cf_sort()).
enum {
    CF_SORT_MANUAL = 0,   // creation order (the default; new slots append)
    CF_SORT_NAME,
    CF_SORT_COLOR,
    CF_SORT_CATEGORY,
    CF_SORT_MRU,          // most recently used, newest first
    CF_SORT_UPDATED,      // unread first, then newest activity, then MRU
    CF_SORT_COUNT
};

// (cfmaxwidth) Which side of the sole open slot a summary tab/collapse flag
// refers to. Deck order: everything with a smaller index than the open slot
// is LEFT, everything with a larger index is RIGHT.
enum { CF_SIDE_LEFT = 0, CF_SIDE_RIGHT = 1 };

// The three open-display states (section headline of the porting spec).
typedef enum {
    CF_MODE_NONE = 0,        // deck fully stowed, nothing open
    CF_MODE_SINGLE,          // one slot open, holding exactly one card
    CF_MODE_GROUP_SPLIT,     // one slot open, holding 2..8 cards (tiled panes)
    CF_MODE_COLUMNS          // 2 or more slots open, side by side
} cf_mode_t;

// Z-order layers, low to high (spec: "desktop < edges/bodies < stowed plates
// < open plates < rail tabs < drag ghost < popups"). Desktop content itself
// has no entry here (it is everything below layer 1).
enum {
    CF_Z_EDGE_BODY    = 1,   // stowed edges and open card bodies, deck order
    CF_Z_PLATE_STOWED = 2,   // sideways tab plates on stowed slots
    CF_Z_PLATE_OPEN   = 3,   // the open slot's tab plate(s)
    CF_Z_RAIL_TAB     = 4,   // "+" and Sort tabs on the rail
    CF_Z_DRAG_GHOST   = 5,
    CF_Z_POPUP        = 6
};

// ---------------------------------------------------------------------------
// (cfdock) Dock bars: 0..CF_MAX_DOCKS repositionable bars hosting the system
// tray + gauges + clock. See this header's own "REPOSITIONABLE DOCK BARS"
// note above for the design rationale.
// ---------------------------------------------------------------------------

#define CF_MAX_DOCKS            8    // "at least 8" per the brief
#define CF_DOCK_THICKNESS_MIN   20
#define CF_DOCK_THICKNESS_MAX   80
#define CF_DOCK_THICKNESS_DEFAULT 32
#define CF_DOCK_WORK_MIN        160  // a docked-in working axis never shrinks below this
#define CF_MAX_DOCK_ITEMS       8

// Which side of the deck a dock's position tag refers to.
enum { CF_DOCK_POS_EDGE = 0, CF_DOCK_POS_BETWEEN = 1 };

// Screen edges an EDGE dock can sit at. TOP/BOTTOM docks are HORIZONTAL bars
// (thickness = height); LEFT/RIGHT docks are VERTICAL bars (thickness =
// width) - see cf_dock_is_vertical().
enum { CF_DOCK_EDGE_TOP = 0, CF_DOCK_EDGE_BOTTOM, CF_DOCK_EDGE_LEFT, CF_DOCK_EDGE_RIGHT, CF_DOCK_EDGE_COUNT };

// Content kinds a dock can host, in the order cf_dock_add() seeds a fresh
// dock with. GAUGES is the CPU+RAM+DSK+NET cluster as ONE atomic item - see
// this header's own note #11 above for why it is not three separate items.
enum { CF_DOCKITEM_TRAY = 0, CF_DOCKITEM_GAUGES, CF_DOCKITEM_CLOCK, CF_DOCKITEM_COUNT };

// Tagged position: an EDGE (`edge` meaningful) or a BETWEEN-cards boundary
// (`slot_index` meaningful, a deck-order boundary in [0, deck->nslots] -
// 0 = right after the rail/before the first card, nslots = after the last
// card).
typedef struct cf_dock_pos {
    int kind;         // CF_DOCK_POS_*
    int edge;         // CF_DOCK_EDGE_*, iff kind == CF_DOCK_POS_EDGE
    int slot_index;   // deck-order boundary, iff kind == CF_DOCK_POS_BETWEEN
} cf_dock_pos_t;

typedef struct cf_dock {
    uint32_t      id;
    cf_dock_pos_t pos;
    int           thickness;                    // px, clamped to [CF_DOCK_THICKNESS_MIN, MAX]
    int           items[CF_MAX_DOCK_ITEMS];      // CF_DOCKITEM_*, in display order
    int           nitems;
} cf_dock_t;

// ---------------------------------------------------------------------------
// Core structures
// ---------------------------------------------------------------------------

typedef struct cf_card {
    uint32_t id;                        // stable id, never reused within a deck's lifetime
    char     title[CF_MAX_TITLE];       // the hosted window's title
    char     app_path[CF_MAX_APP_PATH]; // launch identity, e.g. "/APPS/FILES"
    int      cat;                       // CF_CAT_* taxonomy index
    int      color;                     // CF_COLOR_* palette key
    uint32_t win_id;                    // kernel window id hosting this card; 0 until agent C launches+binds it
    // (cfmaxwidth) PRE-LAUNCH SNAPSHOT EXCLUSION: the highest window id that
    // ALREADY existed for this card's app_id at the moment cf_host_launch()
    // was called for it, 0 if none. cfh_reconcile() (cardfile_host.c) may
    // only bind this card to a window whose id is STRICTLY GREATER than
    // this floor - window ids are allocated monotonically, so "greater than
    // whatever existed before I launched" is exactly "a window my own spawn
    // created", never a pre-existing, unrelated instance of the same binary
    // (e.g. main.c's own persistent auto-launched /APPS/AICHAT helper).
    // Set once by cf_host_launch(), read-only to everyone else; 0 (no
    // floor) is the correct value for a card whose app has no pre-existing
    // instance, which is the common case and behaves exactly as before.
    uint32_t launch_floor_id;
    uint64_t created_ms;
    uint64_t used_ms;                   // MRU timestamp; bumped whenever the card is (re)opened
    uint64_t updated_ms;                // last activity timestamp, 0 = never
    int      unread;                    // activity/notify flag; set by cf_mark_activity(), cleared on open
} cf_card_t;

typedef struct cf_slot {
    uint32_t  id;
    cf_card_t cards[CF_MAX_CARDS_PER_SLOT];
    int       ncards;                   // 1 = ordinary card, 2..8 = a group
    int       open;                     // 1 if this slot's body/panes are shown this frame
    int       w;                        // LIVE explicit column width in px this frame, or 0 = fill remainder
    int       pref_w;                   // (cfmaxwidth) the last explicit width the user chose,
                                         // PERSISTED across stow/reopen (0 = none remembered, always
                                         // fill). w is reset to 0 on stow so the fill-share math in
                                         // cf_layout() stays simple; pref_w is what cf_open_single()/
                                         // cf_open_second_as_column() restore w from. See cf_set_column_width().
    uint32_t  focus;                    // id of the focused member (the pane/tab that is "active")

    // Group-split grid shape and pane weights (relative sizes, like flex-
    // grow). Meaningless while ncards == 1. Kept in sync by cf_slot_sync_grid(),
    // which every op that changes ncards calls automatically.
    int nrows;                                          // rows in the current grid (cf_group_grid())
    int row_cols[CF_MAX_GRID_ROWS];                      // member count in row r
    int row_weight[CF_MAX_GRID_ROWS];                    // height weight of row r
    int col_weight[CF_MAX_GRID_ROWS][CF_MAX_GRID_COLS];  // width weight of column c within row r
} cf_slot_t;

typedef struct cf_deck {
    cf_slot_t slots[CF_MAX_SLOTS];
    int       nslots;
    uint32_t  next_id;      // id allocator, shared by cards, slots AND (cfdock) docks
    int       sort_key;     // CF_SORT_* the deck was last explicitly sorted by
    int       sort_dir;     // 1 or -1; re-choosing sort_key flips this
    uint32_t  active_slot;  // id of the open slot with interaction focus (0 = none);
                             // among several open columns, this is the one agents
                             // A/C raise/focus. Updated by every open/stow op.

    // (cfmaxwidth) Maximize/collapse view state. Only meaningful (see
    // cf_maximize_active()) while exactly one slot is open; every op that
    // changes which slot(s) are open calls cf_maximize_reset() so this can
    // never describe a stale open slot - see the header's own "cfmaxwidth"
    // note above.
    int maximized;         // 1: the sole open slot is widened, reclaiming stowed tab space
    int left_collapsed;    // 1: the stowed run LEFT of the open slot shows as one summary tab
    int right_collapsed;   // 1: the stowed run RIGHT of the open slot shows as one summary tab

    // (cfdock) 0..CF_MAX_DOCKS repositionable dock bars. Array order is
    // z/stacking order for same-edge docks (see cf_dock_edge_rect()) and
    // creation order otherwise; docks[] ids are allocated from the SAME
    // next_id allocator cards/slots use (see next_id's own comment above).
    cf_dock_t docks[CF_MAX_DOCKS];
    int       ndocks;
} cf_deck_t;

// A plain screen rectangle. Top-left origin, matching draw.c's convention.
typedef struct cf_rect {
    int x, y, w, h;
} cf_rect_t;

// Already-resolved pixel geometry for one layout pass. The caller (agent A)
// fills this from the theme's metrics table (already through ui_px()) and
// the current screen size; nothing in here is theme-aware by itself.
typedef struct cf_geom {
    int rail_w;      // cf.rail_w
    int edge_w;       // cf.edge_w, ALREADY computed via cf_edge_width() for this deck/screen
    int tab_w;        // cf.tab_w
    int tab_step;      // cf.tab_step, ALREADY computed via cf_tab_step() for this deck/screen
    int tab_top;       // cf.tab_top
    int foot;          // reserved height at the rail foot (the Sort tab etc.)
    int screen_w;      // (cfdock) the WORKING area's width - screen_w_raw minus any EDGE docks
    int screen_h;      // (cfdock) the WORKING area's height - screen_h_raw minus any EDGE docks
    // (cfdock) Where the working area starts on the real framebuffer (0,0
    // when there are no EDGE docks - which is every deck before this
    // feature, so every existing caller/test that never sets these reads
    // them as 0 only if it zero-initializes the struct first; see this
    // header's own "REPOSITIONABLE DOCK BARS" note #10 above - REQUIRED at
    // every cf_geom_t construction site, not an additive no-op).
    int origin_x;
    int origin_y;
    // (cfdock) The real, undocked framebuffer size - needed only to compute
    // an EDGE dock's own rect (cf_dock_edge_rect()), which lives OUTSIDE the
    // working area screen_w/screen_h describe. Equal to screen_w/screen_h
    // when there are no EDGE docks.
    int screen_w_raw;
    int screen_h_raw;
} cf_geom_t;

// One deck-walk entry, in deck order (see cf_layout()).
typedef struct cf_slot_layout {
    uint32_t slot_id;
    int      edge_x;   // left edge of the stowed strip
    int      body_x;   // left edge of the open body (== edge_x + geom->edge_w)
    int      body_w;   // 0 if this slot is not open this frame
} cf_slot_layout_t;

// Result of a neighbour-preserving two-way divider clamp.
typedef struct cf_divide {
    int a, b;
} cf_divide_t;

// ---------------------------------------------------------------------------
// Deck lifecycle
// ---------------------------------------------------------------------------

// Zeroes *deck and seeds the id allocator. Call once before any other op.
void cf_deck_init(cf_deck_t *deck);

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

cf_slot_t *cf_find_slot(cf_deck_t *deck, uint32_t slot_id);

// Finds a card anywhere in the deck. If out_slot is non-NULL, *out_slot is
// set to the owning slot (on success) or NULL (on failure).
cf_card_t *cf_find_card(cf_deck_t *deck, uint32_t card_id, cf_slot_t **out_slot);

// The slot's focused member, or its first card if slot->focus does not match
// any current member (never returns NULL for a slot with ncards >= 1).
cf_card_t *cf_slot_focused_card(cf_slot_t *slot);

uint64_t cf_slot_last_used(const cf_slot_t *slot);      // max used_ms across members
uint64_t cf_slot_last_updated(const cf_slot_t *slot);   // max updated_ms across members
int      cf_slot_has_unread(const cf_slot_t *slot);     // 1 if any member is unread

// Number of currently-open slots (0 = fully stowed).
int cf_open_count(const cf_deck_t *deck);

// Which of the three open-display states the deck is in right now.
cf_mode_t cf_current_mode(const cf_deck_t *deck);

// ---------------------------------------------------------------------------
// Card / slot operations
// ---------------------------------------------------------------------------

// Adds a new card as its own new slot, appended to the end of the deck.
// title/app_path are copied and NUL-truncated to fit. cat/color are clamped
// into range (out-of-range values fall back to CF_CAT_ACCESSORIES / 0 and
// CF_COLOR_DEFAULT respectively). If open_it, the new slot opens immediately
// (stowing every other slot unless keep_others_open is also set, in which
// case it opens alongside whatever is already open - the "+" picker's normal
// case is open_it=1, keep_others_open=0). Returns the new slot's id, or 0 if
// the deck is already at CF_MAX_SLOTS.
uint32_t cf_add_card(cf_deck_t *deck, const char *app_path, const char *title,
                      int cat, int color, uint64_t now_ms,
                      int open_it, int keep_others_open);

// Removes ONE card from the deck. If it was the only member of its slot, the
// whole slot is removed too and, if that left nothing open, the most-
// recently-used remaining slot is opened (cf_ensure_open()). If it was one
// member of a group, only that member is removed (the pane-header "close"
// button). Returns 1 if card_id was found, 0 otherwise.
int cf_close_card(cf_deck_t *deck, uint32_t card_id, uint64_t now_ms);

// Removes an ENTIRE slot (every member) in one step - the open plate's
// "Close" button, which closes the whole slot regardless of member count.
// Runs cf_ensure_open() afterward. Returns 1 if slot_id was found.
int cf_close_slot(cf_deck_t *deck, uint32_t slot_id, uint64_t now_ms);

// Collapses (stows) a slot without removing anything: open=0, w reset to 0
// (fill). If it was deck->active_slot, active_slot falls back to the last
// remaining open slot in deck order, or 0 if none. Returns 1 if found.
int cf_stow_slot(cf_deck_t *deck, uint32_t slot_id);

// Duplicates the ENTIRE slot holding card_id (every member gets a fresh id
// and a "<title> (n)" suffix, n = 1 + the number of existing deck cards
// sharing that member's app_path, computed before any copy is inserted).
// The new slot is inserted immediately after the source slot; both the
// source and the new slot end up open with width 0 (fill - see resolved
// ambiguity #3), and the new slot becomes deck->active_slot. Copies start
// with win_id 0 (agent C must launch and bind them) and unread/updated
// cleared. Returns the new slot's id, or 0 on failure (card not found, or
// the deck is full).
uint32_t cf_duplicate_card(cf_deck_t *deck, uint32_t card_id, uint64_t now_ms);

// Sets one card's palette colour (CF_COLOR_*, clamped into range). Returns 1
// if card_id was found.
int cf_set_color(cf_deck_t *deck, uint32_t card_id, int color);

// Binds the kernel window id to a card once its app has actually launched
// (agent C, after sys_spawn + matching the new window in the wm snapshot).
// Returns 1 if card_id was found.
int cf_card_set_win_id(cf_deck_t *deck, uint32_t card_id, uint32_t win_id);

// (cfmaxwidth) Sets the card's launch_floor_id (see cf_card_t's own comment):
// cf_host_launch() calls this exactly once, right before spawning, with the
// highest pre-existing window id for this card's app_id. Returns 1 if
// card_id was found.
int cf_card_set_launch_floor(cf_deck_t *deck, uint32_t card_id, uint32_t floor_id);

// Marks a card as having new activity: sets unread=1 and updated_ms=now_ms.
// Called by the window-hosting layer when a window's content changes
// (SYS_WM_APPS_DIRTY) - the deck never reads app internals itself, only this
// external signal. Feeds CF_SORT_UPDATED and the activity badge. Returns 1
// if card_id was found.
int cf_mark_activity(cf_deck_t *deck, uint32_t card_id, uint64_t now_ms);

// ---------------------------------------------------------------------------
// Grouping
// ---------------------------------------------------------------------------

// Moves one card into target_slot_id, forming or growing a group. Refuses
// (returns 0, no change) if target_slot_id already holds CF_MAX_CARDS_PER_SLOT
// members, if card_id is already a member of target_slot_id, or if either id
// is not found. If the source slot becomes empty it is removed from the
// deck; if that source slot had been open, target_slot_id opens alongside
// whatever else is open (matching the mockup's drag-a-tab-onto-another-tab
// behaviour), touching every member's used_ms to now_ms. cf_ensure_open()
// runs afterward. Returns 1 on success.
int cf_group_card_into(cf_deck_t *deck, uint32_t card_id, uint32_t target_slot_id,
                        uint64_t now_ms);

// Groups two WHOLE slots in one step: every card of src_slot_id joins
// dst_slot_id, and src_slot_id is removed. All-or-nothing: if
// dst.ncards + src.ncards would exceed CF_MAX_CARDS_PER_SLOT, NOTHING moves
// and 0 is returned. If src had been open, dst opens alongside whatever else
// is open, touching every member's used_ms to now_ms. cf_ensure_open() runs
// afterward. Returns 1 on success.
int cf_group_slots(cf_deck_t *deck, uint32_t src_slot_id, uint32_t dst_slot_id,
                    uint64_t now_ms);

// Pulls one member out of its group into its own new slot, inserted
// immediately after the group's slot. Refuses (returns 0) if card_id's slot
// has only one member (use cf_close_card()/cf_stow_slot() instead) or is not
// found. If open_it, the new slot opens at column width w (0 = fill) and
// becomes deck->active_slot; its card's used_ms/unread are refreshed.
// Returns the new slot's id, or 0 on failure.
uint32_t cf_ungroup_pull_out(cf_deck_t *deck, uint32_t card_id, int open_it,
                              int w, uint64_t now_ms);

// Explodes a whole group into single-card slots in place (resolved ambiguity
// #4): the original slot keeps its id/open-state and its FIRST member; every
// other member becomes its own new, stowed slot inserted immediately after,
// in member order. No-op (returns 0) if slot_id is not found or already
// holds exactly one card. Returns the number of NEW slots created (ncards-1
// on success).
int cf_ungroup_all(cf_deck_t *deck, uint32_t slot_id);

// ---------------------------------------------------------------------------
// Open / display state
// ---------------------------------------------------------------------------

// Opens slot_id as the SOLE open slot: every other slot is stowed first
// (open=0, w reset to 0). Single-card mode if it holds one card, group-split
// mode if it holds 2..8 (there is no separate model flag for "group-split";
// it falls out of ncards). Clears unread and bumps used_ms on every member.
// slot_id becomes deck->active_slot. (cfmaxwidth) Restores slot_id's LIVE
// width from its persisted pref_w (0 if none was ever set, i.e. fill) - a
// card the user had dragged wider/narrower reopens at that same width
// instead of always fill. Calls cf_maximize_reset() (see the header's
// "cfmaxwidth" note): opening a card always leaves maximize/collapse view
// showing the newly-opened slot, never a stale one. Returns 1 if found.
int cf_open_single(cf_deck_t *deck, uint32_t slot_id, uint64_t now_ms);

// Identical to cf_open_single() - a name for call sites that already know the
// slot is a group and want the split-tile behaviour documented at the call
// site. There is only one underlying "open as the sole slot" operation.
int cf_open_as_group_split(cf_deck_t *deck, uint32_t slot_id, uint64_t now_ms);

// Opens slot_id ALONGSIDE whatever is already open (two/N-column mode) at
// column width w (0 = fill remainder), without stowing anything else. Clears
// unread and bumps used_ms on every member of slot_id. slot_id becomes
// deck->active_slot. (cfmaxwidth) w<=0 restores slot_id's persisted pref_w
// (0 if none, i.e. fill) rather than forcing fill outright, the same
// restore-on-reopen behaviour as cf_open_single(); an explicit w>0 both
// applies immediately AND becomes the new pref_w, so a caller-specified
// width (e.g. a pull-out-to-column drag) is remembered too. Calls
// cf_maximize_reset(). Returns 1 if found.
int cf_open_second_as_column(cf_deck_t *deck, uint32_t slot_id, int w,
                              uint64_t now_ms);

// If nothing is open and the deck is non-empty, opens the most-recently-used
// slot as the sole open slot (ties broken by earliest deck position). No-op
// if something is already open, or the deck is empty.
void cf_ensure_open(cf_deck_t *deck, uint64_t now_ms);

// Sets an already-tracked slot's explicit column width (0 = fill remainder)
// AND persists it to pref_w (cfmaxwidth) so the choice survives a later
// stow/reopen - this is the ONE function a live resize-grip drag should call
// every tick, exactly as cardfile.c already does. Does not itself apply
// CF_CARD_MIN_W or the neighbour-preserving clamp - a live drag should
// compute the clamped value with cf_width_at_pointer() first and pass that
// in. Returns 1 if found.
int cf_set_column_width(cf_deck_t *deck, uint32_t slot_id, int w);

// Sets which member has focus within a slot (which pane is "active" in a
// group, or simply the slot's one card). Returns 1 if slot_id was found and
// card_id is one of its members; 0 otherwise (no change made).
int cf_set_focus(cf_deck_t *deck, uint32_t slot_id, uint32_t card_id);

// ---------------------------------------------------------------------------
// (cfmaxwidth) Maximize / per-side collapse
// ---------------------------------------------------------------------------

// 1 if the maximize/collapse view is in effect THIS FRAME: deck->maximized
// is set AND exactly one slot is open (a stale maximized flag left over from
// a deck that has since gone to N-column or fully-stowed reports 0, even
// though nothing has cleared the flag itself yet - defensive; every open/
// stow op already clears it via cf_maximize_reset() so this should not
// normally be reached, but cf_layout()/cf_width_at_pointer() and the render
// side both gate on this rather than the raw flag so a desync can never
// misrender). If it returns 1 and out_open_idx is non-NULL, *out_open_idx is
// set to the open slot's deck-order index; otherwise (0) *out_open_idx (if
// non-NULL) is set to -1.
int cf_maximize_active(const cf_deck_t *deck, int *out_open_idx);

// Toggles deck->maximized. Turning it ON also sets left_collapsed=1 AND
// right_collapsed=1 (a fresh maximize starts fully collapsed on both sides,
// matching "MAXIMIZE the open card so the deck reclaims the tab space").
// Turning it OFF also clears both collapsed flags (so re-maximizing later
// starts fresh rather than remembering a stale per-side choice). No-op,
// returns 0 unchanged, if cf_open_count(deck) != 1 (nothing single to
// maximize). Returns the new deck->maximized value.
int cf_maximize_toggle(cf_deck_t *deck);

// deck->maximized = left_collapsed = right_collapsed = 0. Called
// automatically by every op that changes which slot(s) are open (see the
// header's "cfmaxwidth" note); exposed so a caller that mutates open/w
// directly (there is exactly one such site, cf_duplicate_card()) can call it
// too. Safe to call when not maximized (no-op fields already 0).
void cf_maximize_reset(cf_deck_t *deck);

// Flips left_collapsed (side=CF_SIDE_LEFT) or right_collapsed (CF_SIDE_RIGHT)
// without touching deck->maximized or the OTHER side - clicking one side's
// summary tab expands just that side; clicking again (or the render layer's
// equivalent persistent toggle control - see cardfile.c) re-collapses it.
// No-op, returns -1, if deck->maximized is not set (a side can only be
// (un)collapsed while the maximize view is actually up). Returns the new
// value of that side's flag (0 or 1) on success.
int cf_toggle_side_collapse(cf_deck_t *deck, int side);

// Number of STOWED slots on `side` (CF_SIDE_LEFT/RIGHT) of the sole open
// slot's deck-order position, independent of whether that side's summary tab
// is actually collapsed right now (a pure count query, per the task's own
// wording) - 0 if cf_maximize_active() would return 0 (no single open slot
// to measure sides against). This is what feeds a summary tab's "N more
// cards" label; render/host code multiplies no logic on top of it.
int cf_side_collapsed_count(const cf_deck_t *deck, int side);

// ---------------------------------------------------------------------------
// Sorting - explicit, one-shot, stable (see cf_deck_t.sort_key/sort_dir)
// ---------------------------------------------------------------------------

// Re-orders deck->slots by `key`. Choosing the SAME key again flips
// direction; a different key resets to the key's natural ascending sense.
// A group slot sorts by its FOCUSED member. Stable among ties (original
// deck order, i.e. slot creation order, is preserved).
//   CF_SORT_MANUAL   - by slot id (creation order)
//   CF_SORT_NAME     - focused member's title, case-insensitive
//   CF_SORT_COLOR    - focused member's palette index
//   CF_SORT_CATEGORY - focused member's CF_CAT_* index
//   CF_SORT_MRU      - cf_slot_last_used(), newest first
//   CF_SORT_UPDATED  - unread slots first, then newest cf_slot_last_updated(),
//                      then cf_slot_last_used()
void cf_sort(cf_deck_t *deck, int key);

// ---------------------------------------------------------------------------
// Geometry: the deck walk (porting spec "Geometry" table + "Deck walk" note)
// ---------------------------------------------------------------------------

// cf.edge_w for the current deck length and screen width: theme_edge_w
// (28 retro / 30 modern) compressed so `nslots` stowed edges plus the rail
// never exceed CF_EDGE_MAX_PCT% of screen_w, floored at CF_EDGE_FLOOR.
int cf_edge_width(int theme_edge_w, int rail_w, int nslots, int screen_w);

// cf.tab_step for the current deck length and screen height: theme_step
// (30 retro / 34 modern) compressed so the tab ladder (nslots-1 steps, one
// slot possibly as tall as tab_max_h if it is the open one) fits between
// tab_top and (screen_h - foot), floored at CF_STEP_FLOOR. tab_max_h is
// normally CF_TAB_MAX_H.
int cf_tab_step(int theme_step, int tab_top, int foot, int tab_max_h,
                int nslots, int screen_h);

// The rail rect: {geom->origin_x, geom->origin_y, geom->rail_w, geom->screen_h}
// (cfdock: offset by the working area's origin, 0,0 when there are no EDGE
// docks - unchanged from before this feature).
cf_rect_t cf_rail_rect(const cf_geom_t *geom);

// The full deck walk (spec: "x starts at rail_w; slot i's edge at x... if
// open, its body at x + edge_w with width w; x advances by edge_w + w. Open
// slots with no explicit width share the remainder equally."). Fills out[]
// in deck order; out_cap should be >= deck->nslots (extra slots are silently
// dropped if out_cap is smaller). Returns the number of entries written.
//
// (cfmaxwidth) MAXIMIZE-AWARE: when cf_maximize_active(deck, &open_i) is
// true, the stowed run strictly left of open_i (if deck->left_collapsed) and
// the stowed run strictly right of it (if deck->right_collapsed) each
// collapse to occupy exactly ONE geom->edge_w-wide shared unit instead of
// one edge_w per member - every slot_layout_t entry inside such a run gets
// the SAME edge_x/body_x (body_w 0), i.e. they are NOT individually
// positioned while collapsed (callers must not draw/hit-test them one by
// one in that state - see cardfile.c's collapsed-member skip). This is the
// WHOLE "reclaim the tab space" mechanism: the reclaimed (N-1)*edge_w per
// collapsed side is never spent on any fixed slot, so it flows straight
// into the open slot's fill_w share like any other freed width - no
// separate host-side widening logic is needed. Expanded sides (or a
// non-maximized/N-column/empty deck) walk exactly as before, unchanged.
int cf_layout(const cf_deck_t *deck, const cf_geom_t *geom,
              cf_slot_layout_t *out, int out_cap);

// (cfdock) The real deck walk, now DOCK-AWARE: identical to cf_layout()'s own
// contract (and its own maximize-collapse behaviour) when deck->ndocks == 0
// (cf_layout() is now a thin wrapper: `return cf_layout_ex(deck, geom, out,
// out_cap, NULL);` - every existing caller/test is unaffected). Every
// BETWEEN-cards dock (cf_dock_pos_t.kind == CF_DOCK_POS_BETWEEN) reserves its
// own thickness as a gap at its slot_index boundary in this SAME x-walk (the
// "push the later cards over" mechanism - see this header's own "cfdock"
// note #10), and its total thickness is subtracted from the fill-share math
// exactly like a maximize-collapsed edge unit already is. If dock_out is
// non-NULL (deck->ndocks entries), every dock's on-screen rect this pass is
// written there: an EDGE dock's rect (cf_dock_edge_rect(), independent of the
// card layout) or, for a BETWEEN dock, the gap THIS pass actually reserved
// for it - a zero rect (w==0 and h==0) if that dock's boundary fell strictly
// inside a maximize-collapsed run this frame (a documented, rare corner: a
// between-cards dock hidden inside a collapsed run reserves no space and
// draws nothing until the run expands or the dock is moved).
int cf_layout_ex(const cf_deck_t *deck, const cf_geom_t *geom,
                  cf_slot_layout_t *out, int out_cap, cf_rect_t *dock_out);

// A layout entry's stowed-edge rect (full screen height).
cf_rect_t cf_edge_rect(const cf_geom_t *geom, const cf_slot_layout_t *entry);

// A layout entry's open-body rect (full screen height; 0-width if not open).
cf_rect_t cf_body_rect(const cf_geom_t *geom, const cf_slot_layout_t *entry);

// Convenience: the body rect of every currently-OPEN entry in `lay` (deck
// order). Yields 0 rects for a fully-stowed deck, 1 for single/group-split,
// N for N-column mode. Returns the count written (<= out_cap).
int cf_open_body_rects(const cf_slot_layout_t *lay, int n_lay,
                        const cf_geom_t *geom, cf_rect_t *out, int out_cap);

// The sideways tab-plate rect for deck-order position `idx` (0-based),
// centred on its entry's edge, at tab_top + idx*tab_step. label_len_px is
// the already-measured tall-axis label length (e.g. cardtext_vertical_len());
// height = min(label_len_px, CF_TAB_LABEL_MAX) + CF_TAB_PAD, plus
// CF_TAB_BTN_COUNT*CF_TAB_BTN_H if is_open, capped at CF_TAB_MAX_H.
//
// NOTE: adjacent plates are NOT guaranteed non-overlapping. cf_tab_step()
// only bounds the worst case (room for one CF_TAB_MAX_H-tall open plate,
// divided evenly otherwise); a real label taller than the resulting step
// WILL overlap its neighbour's plate, and that is the intended "staircase of
// shingled tabs" look the spec describes, not a bug - it is exactly why the
// z-order section exists (CF_Z_PLATE_STOWED, depth-ordered via
// cf_depth_distance(); CF_Z_PLATE_OPEN always on top). Only the EDGE/BODY
// strips (cf_edge_rect()/cf_body_rect(), from cf_layout()) are required to
// tile the screen with no overlap - that invariant IS load-bearing, since it
// is what lets the deck skip a window-stacking rule entirely (spec: "Because
// bodies tile, no window stacking rule is needed").
cf_rect_t cf_tab_rect(const cf_geom_t *geom, const cf_slot_layout_t *entry,
                      int idx, int label_len_px, int is_open);

// The draggable divider grip between two ADJACENT layout entries `a` and `b`
// (b immediately follows a in a cf_layout() result), CF_DIVIDER_HIT wide,
// centred on b's edge_x. Meaningful only when both a and b are open (an
// N-column divider); callers should skip it otherwise.
cf_rect_t cf_column_divider_rect(const cf_geom_t *geom, const cf_slot_layout_t *a,
                                  const cf_slot_layout_t *b);

// The column width implied by dragging slot_id's resize grip so the pointer
// sits `pointer_body_rel_px` pixels right of that slot's body_x (i.e.
// clientX - rect.left - bodyX in the mockup). Applies the neighbour-
// preserving clamp: never below CF_CARD_MIN_W, and never so wide it eats the
// space every LATER slot in deck order needs to keep its own edge_w (plus,
// if that later slot is open, its current width or CF_CARD_MIN_W if it has
// none). `lay`/`n_lay` must be a fresh cf_layout() of the deck's CURRENT
// state. (cfmaxwidth) MAXIMIZE-AWARE: the neighbour reserve for slots after
// slot_id in deck order collapses the same way cf_layout() itself does (a
// collapsed run right of slot_id reserves ONE edge_w total, not one per
// member) - so dragging the sole open (and maximized) card's grip can use
// the space the collapsed side just gave back, not just what an
// un-maximized layout would have reserved. Returns the clamped width, or
// CF_CARD_MIN_W if slot_id is not found in `lay`.
int cf_width_at_pointer(const cf_deck_t *deck, const cf_geom_t *geom,
                         const cf_slot_layout_t *lay, int n_lay,
                         uint32_t slot_id, int pointer_body_rel_px);

// (cfmaxwidth) The summary tab's rect for a collapsed side, replacing that
// whole side's fan with ONE sideways tab labelled by count
// (cf_side_collapsed_count()). `edge_x` is the shared edge_x cf_layout()
// gave every member of that collapsed run (identical for all of them, so
// callers can read it off ANY one member's cf_slot_layout_t entry);
// label_len_px is the already-measured tall-axis label length (e.g.
// cardtext_vertical_len() on "13 more cards +"). Positioned like an
// ordinary idx-0 tab (geom->tab_top, no stagger - there is only one entry on
// this side while collapsed), same width/pad/cap conventions as
// cf_tab_rect() so it reads as one more tab in the same family, not a
// different shape.
cf_rect_t cf_side_summary_tab_rect(const cf_geom_t *geom, int edge_x, int label_len_px);

// ---------------------------------------------------------------------------
// (cfdock) Dock bars: ops + geometry
// ---------------------------------------------------------------------------

// Clamps a thickness value to [CF_DOCK_THICKNESS_MIN, CF_DOCK_THICKNESS_MAX].
// Exposed so a live resize-grip drag preview and cf_dock_set_thickness() (and
// profile.c's loader) all clamp identically.
int cf_dock_thickness_clamp(int px);

// 1 if this dock's bar is VERTICAL (a LEFT/RIGHT edge dock, or ANY
// between-cards dock - see this header's own note #9 above), 0 if HORIZONTAL
// (a TOP/BOTTOM edge dock). thickness means WIDTH when this returns 1,
// HEIGHT when it returns 0.
int cf_dock_is_vertical(const cf_dock_t *dock);

cf_dock_t *cf_dock_find(cf_deck_t *deck, uint32_t dock_id);

// Adds a dock at `pos` with `thickness` (0 or out-of-range -> the default,
// CF_DOCK_THICKNESS_DEFAULT; otherwise clamped) and the full v1 item set
// (TRAY, GAUGES, CLOCK - see note #11). A BETWEEN pos's slot_index is
// CLAMPED into [0, deck->nslots] rather than rejected (so a dock restored
// from a profile before any cards exist still lands somewhere valid, see
// note #12); an EDGE pos's `edge` is clamped into [0, CF_DOCK_EDGE_COUNT); an
// invalid `kind` falls back to CF_DOCK_POS_EDGE/CF_DOCK_EDGE_BOTTOM. Returns
// the new dock's id, or 0 if the deck already holds CF_MAX_DOCKS.
uint32_t cf_dock_add(cf_deck_t *deck, cf_dock_pos_t pos, int thickness);

// Removes a dock. Returns 1 if dock_id was found.
int cf_dock_remove(cf_deck_t *deck, uint32_t dock_id);

// Moves an existing dock to a new position (same clamping as cf_dock_add()).
// Returns 1 if dock_id was found, 0 otherwise (no change).
int cf_dock_set_pos(cf_deck_t *deck, uint32_t dock_id, cf_dock_pos_t pos);

// Clamps (cf_dock_thickness_clamp()) and sets a dock's thickness. Returns 1
// if dock_id was found.
int cf_dock_set_thickness(cf_deck_t *deck, uint32_t dock_id, int thickness);

// Replaces a dock's content list wholesale: nitems is clamped to
// [0, CF_MAX_DOCK_ITEMS] and each item to [0, CF_DOCKITEM_COUNT) (an
// out-of-range item is DROPPED, not clamped into range, so a future build's
// unknown item kind read back by an older one is silently omitted rather
// than misrendered as kind 0). Returns 1 if dock_id was found.
int cf_dock_set_items(cf_deck_t *deck, uint32_t dock_id, const int *items, int nitems);

// Reserves every EDGE dock's thickness from a `screen_w` x `screen_h` full
// screen rect, in ARRAY ORDER per edge (several docks can share one edge,
// stacking outward-to-inward - see cf_dock_edge_rect()'s own comment).
// Fills *out_x/*out_y/*out_w/*out_h with the remaining WORKING rect (what
// cf_geom_t.origin_x/origin_y/screen_w/screen_h should be set to); never
// shrinks either working axis below CF_DOCK_WORK_MIN (a pathological
// dock-thickness sum is capped there rather than collapsing the deck to
// nothing). BETWEEN docks are NOT edges and reserve nothing here - see
// cf_layout_ex().
void cf_dock_reserve_edges(const cf_deck_t *deck, int screen_w, int screen_h,
                            int *out_x, int *out_y, int *out_w, int *out_h);

// An EDGE dock's own on-screen rect against the RAW (undocked) screen size:
// TOP/BOTTOM docks span the full width and stack top-down/bottom-up in array
// order (a TOP dock's y = the sum of any EARLIER TOP dock's thickness, so
// the first TOP dock in the array sits flush at y=0 and later ones sit
// further down, INSIDE it); LEFT/RIGHT docks stack the same way but fit
// between the reserved TOP/BOTTOM bands rather than spanning the full
// height, and stack left-to-right/right-to-left. Zero rect (w=0) if
// dock->pos.kind is not CF_DOCK_POS_EDGE (use cf_dock_rect() for the
// unified, layout-aware entry point that also handles BETWEEN docks) or
// dock is not found in deck->docks[].
cf_rect_t cf_dock_edge_rect(const cf_deck_t *deck, const cf_dock_t *dock,
                            int screen_w, int screen_h);

// THE unified per-dock rect entry point. For an EDGE dock this is exactly
// cf_dock_edge_rect(deck, dock, geom->screen_w_raw, geom->screen_h_raw) (lay/
// n_lay are ignored). For a BETWEEN dock, `lay`/`n_lay` MUST be a fresh
// cf_layout_ex() of the deck's CURRENT state with a dock_out[] buffer (see
// that function) - this function then simply returns dock_out[]'s entry for
// `dock`'s array index, since a between-cards dock's rect is a function of
// the CURRENT card layout, not of the screen size alone (this is the one
// place this API asks for more than the verbal "cf_dock_rect(deck, dock,
// screen_w, screen_h)" sketch would suggest - see this header's own note #10
// for why: the gap's position falls out of the SAME x-walk cf_layout_ex()
// already does, so recomputing it a second, independent way here would risk
// drifting out of sync with what was actually drawn/reserved this frame).
// Zero rect if dock is not found in deck->docks[].
cf_rect_t cf_dock_rect(const cf_deck_t *deck, const cf_dock_t *dock,
                       const cf_geom_t *geom,
                       const cf_slot_layout_t *lay, int n_lay);

// (cfdock) Packs one dock's persisted fields into a SINGLE int - profile.c's
// flat "key: int" format (put_kv()) has no list/array value type, so this
// trades the dock's item ORDER for fitting that format with zero profile.c
// parser changes (see this header's own note #12 above). Bit layout (LSB
// first, all pure arithmetic, no struct-layout dependence so it is stable
// across a rebuild): bit 0 = pos.kind; bits 1-6 = edge_or_slot (edge 0-3, or
// slot_index 0-63 - deck->nslots is capped at CF_MAX_SLOTS=32, so 6 bits is
// margin, not a tight fit); bits 7-13 = thickness (0-127, CF_DOCK_THICKNESS_
// MAX=80 fits); bits 14-18 = item bitmask (bit k set = CF_DOCKITEM_k
// present, CANONICAL order - not necessarily the dock's own display order).
// Bits 19-31 are reserved (zero) for future item kinds or fields.
int cf_dock_pack(const cf_dock_t *dock);

// Unpacks a value cf_dock_pack() produced (or any other int, hand-edited or
// from an older/newer build) into *out_pos/*out_thickness/*out_items/
// *out_nitems (out_items must be CF_DOCKITEM_COUNT ints long) - every field
// is clamped/masked exactly as cf_dock_add()/cf_dock_set_thickness()/
// cf_dock_set_items() would, so this never produces an out-of-range dock.
// Never fails.
void cf_dock_unpack(int packed, cf_dock_pos_t *out_pos, int *out_thickness,
                     int *out_items, int *out_nitems);

// ---------------------------------------------------------------------------
// Geometry: group split grid + panes
// ---------------------------------------------------------------------------

// The group grid SHAPE for `ncards` (1..CF_MAX_CARDS_PER_SLOT): columns per
// row are 1 for ncards<=2, 2 for ncards in [3,4], 3 for ncards in [5,8];
// members fill rows left to right, the last row may be shorter. (cfmaxwidth,
// owner correction) ncards==2 is 1 column, which - since cf_group_grid()
// fills ROWS, not columns, first - means TWO ROWS of ONE column: the pair
// stacks top over bottom (a horizontal divider), not side by side. This
// was "2: 1x2" (side by side) before; the owner's explicit read of "split"
// was top/bottom, so cf_group_pane_rects()'s existing row-major tiling
// (unchanged - it already tiles ANY row/col shape correctly) now produces a
// vertical stack for a pair with no other code needing to change: the
// divider render/hit-test/drag paths already pick HDIV vs VDIV purely from
// the grid SHAPE (rows>1 -> horizontal, a row's cols>1 -> vertical), so a
// 2-row x 1-col group already gets a correctly-oriented horizontal divider
// dragging pane HEIGHT, clamped by CF_PANE_MIN on height, for free. 3-8
// members are UNCHANGED: "3-4: 2x2, 5-6: 2x3, 7-8: 3x3(short)". Fills
// row_cols[0..nrows-1] and returns nrows.
int cf_group_grid(int ncards, int row_cols[CF_MAX_GRID_ROWS]);

// Recomputes slot->nrows/row_cols for slot->ncards. If the grid SHAPE
// changed (different row count, or a different member count in some row)
// from what row_weight/col_weight currently encode, every weight is reset to
// 1 (matching the mockup's rows=rowH=null invalidation whenever membership
// changes the grid). No-op if the shape is unchanged, so a live divider drag
// survives an unrelated call. Called automatically by every op that changes
// a slot's card count; safe to call again by hand after direct field edits.
void cf_slot_sync_grid(cf_slot_t *slot);

// Split-pane rects for a group slot's grid, tiling `body` (typically
// cf_body_rect() for that slot's layout entry) by the stored row/col
// weights. Rounds so the panes EXACTLY tile body (no gaps, no overhang): the
// last row/column in each dimension absorbs any rounding remainder. Fills
// out[] in member order (out[k] is slot->cards[k]'s pane); out_cap must be
// >= slot->ncards. Returns slot->ncards, or 0 if slot->ncards <= 1 (use
// cf_body_rect() directly for a single card).
int cf_group_pane_rects(const cf_slot_t *slot, cf_rect_t body,
                         cf_rect_t *out, int out_cap);

// Neighbour-preserving divider clamp shared by both divider kinds: given two
// neighbours' CURRENT pixel sizes and a pointer delta, returns their new
// sizes with the total preserved and each floored at min_px (CF_CARD_MIN_W
// for a column grip that only has one neighbour conceptually, CF_PANE_MIN
// for a group pane/row divider). Exposed standalone so a caller can preview
// a drag before committing it.
cf_divide_t cf_clamp_divider(int size_a_px, int size_b_px, int delta_px, int min_px);

// Applies cf_clamp_divider() (min_px = CF_PANE_MIN) to the vertical divider
// between columns (col-1) and col within row `row` of slot_id's group grid,
// and stores the result into col_weight[row][col-1]/[col]. size_a_px/
// size_b_px are the two panes' CURRENT on-screen widths (from the caller's
// last cf_group_pane_rects()); delta_px is the pointer's horizontal movement
// since the drag started. Returns 1 on success (slot found, is a group,
// row/col in range), 0 otherwise (no change made).
int cf_set_col_divider(cf_deck_t *deck, uint32_t slot_id, int row, int col,
                        int size_a_px, int size_b_px, int delta_px);

// Same as cf_set_col_divider() but for the horizontal divider between rows
// (row-1) and row, storing into row_weight[row-1]/[row]. delta_px is
// vertical pointer movement.
int cf_set_row_divider(cf_deck_t *deck, uint32_t slot_id, int row,
                        int size_a_px, int size_b_px, int delta_px);

// ---------------------------------------------------------------------------
// Depth / z-order helpers
// ---------------------------------------------------------------------------

// The minimum distance, in slot POSITIONS, from deck-order index `idx` to
// any open slot (0 if idx itself is open, or if nothing is open at all).
// open_idx[] must be the sorted (ascending) deck-order indices of every open
// slot, n_open long. Capped at CF_DEPTH_CAP, matching the spec's dim-per-step
// falloff, which stops increasing past that many steps away from the open
// card.
int cf_depth_distance(const int *open_idx, int n_open, int idx);

// Percent (in tenths of a percent, e.g. 45 = 4.5%) to dim a stowed edge/plate
// at `dist` steps from the open card (already-capped output of
// cf_depth_distance()): dist * 45. No floating point, per the "userland
// still shouldn't reach for float where an integer says the same thing"
// norm; callers divide by 10 when they blend it into a percentage.
int cf_depth_dim_x10(int dist);

#endif // CARDFILE_MODEL_H
