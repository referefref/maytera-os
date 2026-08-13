// startmenu.c - MayteraOS Start menu: search + favorites + recents + categories.
//
// Whisker-Menu-style uplift (docs/DESKTOP_SHELL_RESEARCH.md 4.3): a user/profile
// header, a type-to-filter search box that shows live flat results across every
// category while active, persisted Favorites (pin/unpin from a right-click) and
// Recents (auto-tracked on launch) sections shown above the category accordion
// when the search box is empty, an optional "All Apps" flat view, and a 2x2
// power/session grid (Lock / Log Out / Restart / Shut Down) each behind an
// explicit confirm step. Rendering and hit-testing both walk ONE row list
// (g_rows[], built fresh by sm_build_rows() at the top of both render and
// handle_mouse) so the two can never drift apart the way two parallel
// duplicated layouts could.
//
// No malloc; all state is static arrays sized at compile time, same as the
// original accordion this replaces.
//
// Persistence (two small flat files, same idiom as /APPS.CFG et al.):
//   /CONFIG/STARTMENU.CFG   - FAV|<exec_path> / RECENT|<exec_path> lines.
//     Compositor-owned: written immediately on pin/unpin/launch, never touched
//     by Settings, so a Settings-side rewrite can never race or clobber it.
//   /CONFIG/STARTMENU.PREFS - key=value lines (view/show_fav/show_recent/
//     recent_count/focus_search/width/icon_size). Written by the Settings app
//     "Start Menu" panel, polled here on a throttle (startmenu_prefs_poll(),
//     same cadence idiom as main.c's dock_style_poll()) so changes apply live
//     without needing the menu to be reopened.

#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../libc/notify.h"
#define GUI_SCROLL_STDINT_ONLY       // see the note at the top of that header
#include "../../libc/gui_scroll.h"   // gui_scroll_thumb_ink ONLY: the shared
                                     // scrollbar contrast rule (#745 item 77).
                                     // The WIDGET still is not used here (see
                                     // the sm_scroll_t comment above); the
                                     // COLOUR RULE is, so the start menu
                                     // cannot drift away from every other
                                     // scrollbar in the OS the way it just did.
#include "../../libc/userconf.h"   // #745: <home>/CONFIG/STARTMENU, the per-user app layer
#include "../../libc/string.h"   // memcpy/memset/strcmp/strncpy (was relying on implicit
                                  // declarations before; harmless to include, no types.h pull-in)

// #563: a minimal scroll-viewport primitive, modeled on the shared
// userland/libc/gui_scroll.h (same offset/clamp/wheel/reveal semantics and the
// same "positive delta scrolls up" convention) but NOT that header directly:
// gui_scroll.h pulls in libc/types.h, whose `typedef _Bool bool;` conflicts
// with compositor.h's own `typedef int bool;` (a real, previously-undiscovered
// incompatibility - see blame.md #563) - the SAME "handle-based app toolkit is
// not callable in the compositor" boundary this file's other includes already
// respect, just tripped by a header that looked state-only. gui_scroll.h's
// gui_scroll_draw() also needs a window handle (win_draw_rect), which the
// compositor never has for itself either way; the scrollbar below is drawn
// with plain draw.c primitives.
#define SM_SCROLL_W       14
#define SM_SCROLL_MIN_TH  24
typedef struct {
    int offset;        // current scroll offset in px, always in [0, max]
    int content_px;     // total content height in px
    int step_px;        // wheel-notch / arrow-key step
} sm_scroll_t;

static int sm_scroll_max(const sm_scroll_t *s, int viewport_h) {
    int m = s->content_px - viewport_h;
    return m > 0 ? m : 0;
}
// Re-clamp after the viewport/content size may have changed (a filtered list,
// a resized column) so a shrinking content never strands the view.
static void sm_scroll_config(sm_scroll_t *s, int content_px, int step_px, int viewport_h) {
    s->content_px = content_px < 0 ? 0 : content_px;
    s->step_px = step_px > 0 ? step_px : 16;
    int max = sm_scroll_max(s, viewport_h);
    if (s->offset < 0) s->offset = 0;
    if (s->offset > max) s->offset = max;
}
static bool sm_scroll_set(sm_scroll_t *s, int offset, int viewport_h) {
    int max = sm_scroll_max(s, viewport_h);
    int n = offset < 0 ? 0 : (offset > max ? max : offset);
    if (n == s->offset) return false;
    s->offset = n;
    return true;
}
static bool sm_scroll_by(sm_scroll_t *s, int delta_px, int viewport_h) {
    return sm_scroll_set(s, s->offset + delta_px, viewport_h);
}
// Positive delta scrolls UP (toward the content start) - the OS-wide
// convention (kernel gui/terminal.c, gui_scroll.h). Three rows per notch,
// matching the established feel elsewhere (Files, recycle bin).
static bool sm_scroll_wheel(sm_scroll_t *s, int delta, int viewport_h) {
    if (delta == 0) return false;
    return sm_scroll_by(s, -delta * s->step_px * 3, viewport_h);
}
// Scroll the minimum distance needed to bring [top_px, top_px+h_px) into view
// (keeps a keyboard-selected row on screen).
static bool sm_scroll_reveal(sm_scroll_t *s, int top_px, int h_px, int viewport_h) {
    if (top_px < s->offset) return sm_scroll_set(s, top_px, viewport_h);
    if (top_px + h_px > s->offset + viewport_h)
        return sm_scroll_set(s, top_px + h_px - viewport_h, viewport_h);
    return false;
}

// Keycodes as delivered in gui_event_t.keycode (scancode-derived; same values
// as userland/libc/gui_scroll.h's GUI_KEY_* - open-coded here rather than
// including that header, per the bool-conflict note above).
#define SM_KEY_UP    0x80
#define SM_KEY_DOWN  0x81
#define SM_KEY_LEFT  0x82
#define SM_KEY_RIGHT 0x83

// ============================================================================
// Static state
// ============================================================================

// Was 14 (3 hardcoded built-ins + headroom). There is no hardcoded built-in
// category any more (see startmenu_init() below): every category the Rust
// model emits (sm_rust_rebuild()) plus whatever startmenu_load_win16_groups()
// appends after it must fit here, so this needs real headroom rather than a
// count sized to the old compiled-in list.
#define MAX_CATEGORIES 32

static menu_category_t g_categories[MAX_CATEGORIES];
static menu_item_t     g_menu_items[START_MENU_MAX_ITEMS];
static int             g_total_items;
// Next free category slot. sm_rust_rebuild() resets this to 0 and advances it
// (via sm_c_add_category()) as it emits each merged category; the Win16
// program-group loader then continues appending from wherever that left off,
// so it is shared instead of each hardcoding a slot number. The initializer
// here only matters before the first startmenu_init() call.
static int             g_next_cat = 0;

// Unified row list: rendering and hit-testing both walk this so the two can
// never disagree about where anything is on screen. Rebuilt (cheaply - no more
// than a couple hundred items) at the top of every render/handle_mouse call.
#define SM_ROW_FAVHDR   3   // "Favorites" section label (not clickable)
#define SM_ROW_RECHDR   4   // "Recent" section label (not clickable)
#define SM_ROW_ALLHDR   5   // "All Apps" section label (not clickable)
#define SM_ROW_SEARCHHDR 6  // "Search Results" section label (not clickable)
#define SM_ROW_CATHDR   0   // category header (collapsible)
#define SM_ROW_ITEM     1   // launchable item
#define SM_ROW_NOMATCH  2   // "No matches" placeholder (not clickable)
#define SM_MAX_ROWS     160
typedef struct {
    int kind;       // SM_ROW_*
    int cat_idx;    // valid for SM_ROW_CATHDR
    int item_idx;   // valid for SM_ROW_ITEM
} sm_row_t;
static sm_row_t g_rows[SM_MAX_ROWS];
static int      g_row_count;
static int      g_hover_row = -1;     // index into g_rows, or -1
static bool     g_hover_gear;         // header gear (Settings) hover

// Power section hover/target: 0=none, 1=Shutdown, 2=Restart, 3=Log Out, 4=Lock.
static int g_hover_power;
static int g_power_confirm;           // 0=none, else the pending action (1-4)

// Search (type-to-filter).
#define SM_SEARCH_MAX 40
static char g_search[SM_SEARCH_MAX + 1];
static int  g_search_len;
static bool g_search_focused;   // only the search box receives typed characters

// Favorites / Recents (persisted to /CONFIG/STARTMENU.CFG). Keyed by the item's
// exec_path (same identity used by the STARTMENU.YAML "rename:" matcher), not
// by index, so entries survive category renumbering across a rebuild.
#define MAX_FAVORITES 12
#define MAX_RECENTS   10
static char g_fav_paths[MAX_FAVORITES][128];
static int  g_fav_count;
static char g_recent_paths[MAX_RECENTS][128];   // index 0 = most recent
static int  g_recent_count;

// Prefs (Settings "Start Menu" panel writes /CONFIG/STARTMENU.PREFS; we poll it
// live). Defaults reproduce the pre-uplift look (Categories view, both new
// sections on, focus-on-open on, classic width/icon size).
static int g_sm_view          = 0;    // 0 = Categories accordion, 1 = All Apps flat
static int g_sm_show_fav      = 1;
static int g_sm_show_recent   = 1;
static int g_sm_recent_count  = 5;
static int g_sm_focus_search  = 1;
static int g_sm_width         = START_MENU_WIDTH;
static int g_sm_icon_size     = 20;

// Properties popup (right-click -> Properties).
static bool g_props_open;
static int  g_props_item;

// #563: sideways-cascading flyout state (replaces the old inline accordion,
// which let the panel grow taller than the screen - categories + Games/
// Internet/Media/Accessories/System submenus + every game could clip off the
// top/bottom with no way to reach the rest). Opening a category now opens a
// SEPARATE column to the side instead of inlining its items downward, and
// BOTH the root panel and a flyout column are independently height-capped to
// the screen and scroll (via gui_scroll.h) if their content still would not
// fit. Root rows are non-uniform height (headers vs items), so g_sm_scroll is
// driven in raw pixels; a flyout's items are all START_MENU_ITEM_H, so
// sm_flyout_geom() snaps its viewport height to a whole number of rows (never
// leaves a row half-visible at the viewport edge). (local 81) This used to say
// "g_fly_scroll uses snap=1"; sm_scroll_config() takes no snap parameter and
// never did, and the missing snap is exactly why the last flyout row rendered
// as a sliver and was then cut mid-row by the render loop's clip. Keyboard/mouse selection (which row is highlighted) is the menu's
// own job per gui_scroll.h's own division of responsibility ("the app owns
// layout and drawing of the CONTENT; this primitive owns the scroll offset").
static sm_scroll_t g_sm_scroll;       // root row-list viewport
static sm_scroll_t g_fly_scroll;      // open flyout's item viewport
static int      g_flyout_cat = -1;    // category index whose flyout is open, or -1
static int      g_flyout_hover_item = -1;  // absolute index into g_menu_items, or -1
// Forward decls: defined near the geometry helpers further down, used by
// sm_launch_item() (above them in the file) and the public entry points.
static void sm_close_flyout(void);
static void sm_open_flyout(int cat_idx);

// ============================================================================
// Internal helpers: item/category registration (unchanged from the original)
// ============================================================================

// Launch types (must match menu_item_t.launch_type).
#define LAUNCH_NATIVE 0   // sys_spawn() a native ELF
#define LAUNCH_WIN16  1   // win16_run() a Win16 NE/.COM
#define LAUNCH_DOS    2   // dos_run() an MS-DOS .EXE/.COM (#208)

// Append one item to g_menu_items. The item is associated with whichever
// category was most recently registered, so all of a category's items must be
// added before the next add_category() call (they must stay contiguous).
static void add_item_typed(const char *name, icon_id_t icon, const char *path, int launch_type)
{
    if (g_total_items >= START_MENU_MAX_ITEMS)
        return;

    menu_item_t *it = &g_menu_items[g_total_items];
    strncpy(it->name,      name, sizeof(it->name) - 1);
    it->name[sizeof(it->name) - 1] = '\0';
    strncpy(it->exec_path, path, sizeof(it->exec_path) - 1);
    it->exec_path[sizeof(it->exec_path) - 1] = '\0';
    it->icon_id      = icon;
    it->is_separator = false;
    it->is_win16     = (launch_type == LAUNCH_WIN16);
    it->launch_type  = launch_type;

    // Increment the item_count of the last registered category.
    int last_cat = -1;
    for (int i = 0; i < MAX_CATEGORIES; i++) {
        if (g_categories[i].label[0] != '\0')
            last_cat = i;
    }
    if (last_cat >= 0)
        g_categories[last_cat].item_count++;

    g_total_items++;
}

static void add_item_ex(const char *name, icon_id_t icon, const char *path, bool is_win16)
{
    add_item_typed(name, icon, path, is_win16 ? LAUNCH_WIN16 : LAUNCH_NATIVE);
}

static void add_item(const char *name, icon_id_t icon, const char *path)
{
    add_item_typed(name, icon, path, LAUNCH_NATIVE);
}

// Register a category. Must be called before add_item() calls for that group.
static void add_category(int index, const char *label, bool expanded)
{
    // Defensive bounds check (previously absent): every caller used to be a
    // fixed, reviewed set of compile-time indices, so an out-of-range index
    // could not happen. Now the Rust content model (sm_rust_rebuild() below)
    // assigns indices from a runtime count of DATA-driven categories, so an
    // overflow is now a real, reachable case (a config layer with more
    // categories than MAX_CATEGORIES) rather than a theoretical one; without
    // this check it would silently corrupt g_categories[] and whatever static
    // memory follows it instead of just dropping the extra category.
    if (index < 0 || index >= MAX_CATEGORIES) return;
    menu_category_t *cat = &g_categories[index];
    strncpy(cat->label, label, sizeof(cat->label) - 1);
    cat->label[sizeof(cat->label) - 1] = '\0';
    cat->expanded   = expanded;
    cat->item_start = g_total_items;
    cat->item_count = 0;
}

// ============================================================================
// Search matching: fuzzy (subsequence) on the name, substring on the exec path.
// Mirrors the scope docs/DESKTOP_SHELL_RESEARCH.md documents for XFCE's Whisker
// Menu (name fuzzy beats everything-else substring) - apps-only, nothing beyond
// installed items, no calculator/unit-conversion/file-search ambitions.
// ============================================================================

static int sm_ci_eq(char a, char b) {
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    return a == b;
}

// True if every character of q appears, in order, somewhere in name
// (case-insensitive). Empty query matches everything.
static bool sm_subseq_match(const char *name, const char *q) {
    if (!q[0]) return true;
    const char *n = name;
    while (*q) {
        bool found = false;
        while (*n) {
            if (sm_ci_eq(*n, *q)) { found = true; n++; break; }
            n++;
        }
        if (!found) return false;
        q++;
    }
    return true;
}

static bool sm_substr_ci(const char *hay, const char *needle) {
    if (!needle[0]) return true;
    for (const char *h = hay; *h; h++) {
        const char *hh = h, *nn = needle;
        while (*hh && *nn && sm_ci_eq(*hh, *nn)) { hh++; nn++; }
        if (!*nn) return true;
    }
    return false;
}

static bool sm_item_matches(int idx, const char *q) {
    if (!q[0]) return true;
    menu_item_t *it = &g_menu_items[idx];
    if (sm_subseq_match(it->name, q)) return true;
    return sm_substr_ci(it->exec_path, q);
}

static int sm_find_item_by_path(const char *path) {
    for (int i = 0; i < g_total_items; i++) {
        if (!g_menu_items[i].is_separator && strcmp(g_menu_items[i].exec_path, path) == 0)
            return i;
    }
    return -1;
}

static int sm_find_item_by_name(const char *name) {
    for (int i = 0; i < g_total_items; i++) {
        if (!g_menu_items[i].is_separator && strcmp(g_menu_items[i].name, name) == 0)
            return i;
    }
    return -1;
}

static bool sm_is_favorite_path(const char *path) {
    for (int i = 0; i < g_fav_count; i++)
        if (strcmp(g_fav_paths[i], path) == 0) return true;
    return false;
}

static bool sm_is_favorite_idx(int idx) {
    if (idx < 0 || idx >= g_total_items) return false;
    return sm_is_favorite_path(g_menu_items[idx].exec_path);
}

// ============================================================================
// Persistence: /CONFIG/STARTMENU.CFG (favorites + recents, compositor-owned)
// ============================================================================

static void sm_save_state(void) {
    static char buf[2048];
    char *p = buf;
    for (int i = 0; i < g_fav_count; i++) {
        const char *k = "FAV|"; while (*k) *p++ = *k++;
        const char *v = g_fav_paths[i]; while (*v) *p++ = *v++;
        *p++ = '\n';
    }
    for (int i = 0; i < g_recent_count; i++) {
        const char *k = "RECENT|"; while (*k) *p++ = *k++;
        const char *v = g_recent_paths[i]; while (*v) *p++ = *v++;
        *p++ = '\n';
    }
    sys_unlink("/CONFIG/STARTMENU.CFG");
    int fd = sys_open("/CONFIG/STARTMENU.CFG", 0x41 | 0x200 /*O_WRONLY|O_CREAT|O_TRUNC*/);
    if (fd < 0) return;
    sys_write(fd, buf, (unsigned long)(p - buf));
    sys_close(fd);
}

static char *sm_trim(char *p) {
    while (*p == ' ' || *p == '\t') p++;
    int n = 0; while (p[n]) n++;
    while (n > 0 && (p[n-1] == ' ' || p[n-1] == '\t' ||
                     p[n-1] == '\r' || p[n-1] == '\n')) p[--n] = 0;
    return p;
}

// Returns true iff /CONFIG/STARTMENU.CFG exists at all, regardless of how
// many FAV|/RECENT| lines it contains (including zero). This is the ONE
// signal that lets startmenu_init() tell "a real profile that has never
// pinned anything, or has deliberately unpinned everything" (file exists,
// g_fav_count possibly 0) apart from "no profile has ever been saved here"
// (file absent, e.g. the first-run wizard was skipped and its apps page
// never wrote FAVCH.CFG - see startmenu_init()'s default-seeding block
// below). sm_save_state() runs on every favorite add/remove, so the file
// exists from the FIRST toggle onward, even a toggle that empties the list;
// only a profile that has NEVER gone through sm_save_state() has no file.
static bool sm_load_state(void) {
    g_fav_count = 0;
    g_recent_count = 0;
    int fd = sys_open("/CONFIG/STARTMENU.CFG", 0);
    if (fd < 0) return false;
    static char buf[2048];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return true;   // file exists (possibly empty/truncated): still "existed"
    buf[n] = 0;

    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) { *p = 0; p++; }
        char *t = sm_trim(line);
        if (t[0] == 0 || t[0] == '#') continue;
        if (strncmp(t, "FAV|", 4) == 0) {
            if (g_fav_count < MAX_FAVORITES) {
                strncpy(g_fav_paths[g_fav_count], t + 4, 127);
                g_fav_paths[g_fav_count][127] = 0;
                g_fav_count++;
            }
        } else if (strncmp(t, "RECENT|", 7) == 0) {
            if (g_recent_count < MAX_RECENTS) {
                strncpy(g_recent_paths[g_recent_count], t + 7, 127);
                g_recent_paths[g_recent_count][127] = 0;
                g_recent_count++;
            }
        }
    }
    return true;
}

// #63/#745: default pinned dock set for a profile that has NEVER saved a
// favorites list at all (STARTMENU.CFG absent - the exact state a skipped
// first-run wizard leaves behind, since the wizard's apps page is what would
// otherwise have written it via FAVCH.CFG/sm_load_favs_channel()). Without
// this, startmenu_get_favorites() legitimately returns 0 (see its own
// comment: an empty favorites list is a valid, persisted state, not an
// error), and the Marble dock renders with nothing pinned - reported live by
// a user who skipped setup and switched to Marble.
//
// USER-SPECIFIED SET (verbatim from the brief): Files, Browser, Settings,
// Terminal, App Store, Music Player, Paint. Paths below are copied from the
// shipped system fragments (build/assets/startmenu/system.d/*.MENU) so they
// match exactly what g_menu_items[] actually contains; see
// sm_find_item_by_path()'s own validation below for what happens if one
// doesn't (skipped, not a dead pin).
static const char *const SM_DEFAULT_FAVORITES[] = {
    "/APPS/FILES",
    "/APPS/BROWSER",
    "/APPS/SETTINGS",
    "/APPS/TERMINAL",
    "/APPS/APPSTORE",
    "/APPS/MUSICPLR",
    "/APPS/PAINT",
};
#define SM_DEFAULT_FAVORITES_N \
    (int)(sizeof(SM_DEFAULT_FAVORITES) / sizeof(SM_DEFAULT_FAVORITES[0]))

// Seed g_fav_paths[] with SM_DEFAULT_FAVORITES, validated against
// g_menu_items exactly like sm_load_favs_channel() validates a wizard
// channel line (sm_find_item_by_path): a default that does not resolve to a
// real shipped app (a golden built without one of these, or a future rename)
// is silently skipped rather than pinned as a dead tile, never a fatal
// error. Persists via sm_save_state() so this only ever happens ONCE per
// profile - the resulting STARTMENU.CFG is what sm_load_state()'s "file
// exists" check reads on every later boot, so a user who then unpins one of
// these defaults stays unpinned forever, exactly like any other favorite
// removal. Matches BY PATH (the list is a plain array of exec_path strings,
// same identity every other favorites writer in this file uses), never by
// array index, so this has none of the index-keyed-persistence hazard
// blame.md warns about.
static void sm_seed_default_favorites(void) {
    for (int i = 0; i < SM_DEFAULT_FAVORITES_N && g_fav_count < MAX_FAVORITES; i++) {
        if (sm_find_item_by_path(SM_DEFAULT_FAVORITES[i]) < 0) continue;
        strncpy(g_fav_paths[g_fav_count], SM_DEFAULT_FAVORITES[i], 127);
        g_fav_paths[g_fav_count][127] = 0;
        g_fav_count++;
    }
    if (g_fav_count > 0) sm_save_state();
}

// ============================================================================
// Persistence: /CONFIG/STARTMENU.PREFS (Settings "Start Menu" panel writes it,
// we read/poll it). Tiny "key=value" parser, same shape as Settings' own
// a_kv()/a_putkv() helpers for ALERTS.CFG/PRIVACY.CFG - kept as an independent
// copy here since these are two different processes/binaries.
// ============================================================================

static int sp_kv(const char *b, const char *key, int def) {
    int kl = 0; while (key[kl]) kl++;
    for (const char *p = b; *p; ) {
        int i = 0; while (key[i] && p[i] == key[i]) i++;
        if (i == kl && p[kl] == '=') {
            const char *v = p + kl + 1;
            int neg = 0, val = 0, any = 0;
            if (*v == '-') { neg = 1; v++; }
            while (*v >= '0' && *v <= '9') { val = val * 10 + (*v - '0'); v++; any = 1; }
            return any ? (neg ? -val : val) : def;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return def;
}

static void sm_load_prefs(void) {
    int fd = sys_open("/CONFIG/STARTMENU.PREFS", 0);
    if (fd < 0) fd = sys_open("/STARTMENU.PREFS", 0);
    if (fd < 0) return;
    static char b[256];
    long n = sys_read(fd, b, sizeof(b) - 1);
    sys_close(fd);
    if (n <= 0) return;
    b[n] = 0;

    g_sm_view = sp_kv(b, "view", 0);
    if (g_sm_view < 0 || g_sm_view > 1) g_sm_view = 0;
    g_sm_show_fav = sp_kv(b, "show_fav", 1) ? 1 : 0;
    g_sm_show_recent = sp_kv(b, "show_recent", 1) ? 1 : 0;
    g_sm_recent_count = sp_kv(b, "recent_count", 5);
    if (g_sm_recent_count < 1) g_sm_recent_count = 1;
    if (g_sm_recent_count > MAX_RECENTS) g_sm_recent_count = MAX_RECENTS;
    g_sm_focus_search = sp_kv(b, "focus_search", 1) ? 1 : 0;
    g_sm_width = sp_kv(b, "width", START_MENU_WIDTH);
    if (g_sm_width < 220) g_sm_width = 220;
    if (g_sm_width > 420) g_sm_width = 420;
    g_sm_icon_size = sp_kv(b, "icon_size", 20);
    if (g_sm_icon_size < 14) g_sm_icon_size = 14;
    if (g_sm_icon_size > 28) g_sm_icon_size = 28;
}

// Called once per compositor frame from main.c, same throttled-poll idiom as
// main.c's dock_style_poll() (#387) - cheap, no busy-wait (#426): a sys_open()
// that returns -1 immediately when the file is absent/unchanged is the common
// case, and we do not even hash-compare, we just re-apply (idempotent).
void startmenu_prefs_poll(void) {
    static int throttle = 0;
    if (++throttle < 30) return;   // ~ once a second at the ~33ms main-loop tick
    throttle = 0;
    sm_load_prefs();
}

// #745 P1: favourites live-apply channel. The first-boot wizard's apps page
// (the last wizard page, not yet built - this is its prerequisite) is a
// SEPARATE process spawned by this already-running compositor, so it cannot
// touch g_fav_paths/g_fav_count in this process directly, and it must not
// write /CONFIG/STARTMENU.CFG itself: that file is compositor-owned (see the
// file header comment above) and sm_save_state() here is the only writer, so
// a direct wizard write would sit unread until the NEXT sm_load_state() -
// which has exactly one call site, startmenu_init(), i.e. never before the
// following reboot. That is the bug this function exists to close. It is
// also NOT the DOCKSTYL.CFG trap: dock_style is re-seeded from UIPROFIL.YML
// at every compositor_init() (main.c dock_style_write_cfg() call, #683
// comment), so a channel write there can be overwritten by the compositor's
// own boot sequence. Favourites are read from disk exactly once
// (sm_load_state(), called only from startmenu_init()) and never rewritten
// from an in-memory snapshot afterward, so there is no analogous "the
// compositor overwrites it on the next tick" race to worry about here - the
// one real race is the one this function fixes (wizard writes after the one
// read already happened).
//
// Modelled exactly on startmenu_prefs_poll() above: same internal throttle
// (~once a second, not the faster ~330ms cadence main.c uses for
// dock_style_poll()/widgets_cfg_poll(), because favourites are a one-time
// wizard action, not a live-dragged Settings control), same no-op-on-
// failed-open behaviour (sm_load_favs_channel() below returns immediately
// if the channel file is absent, exactly like sm_load_prefs() above).
//
// FAVCH.CFG (per-user path via userconf_open_read/userconf_path, no legacy
// root fallback: brand-new channel, same choice widgets_cfg_poll() made for
// WIDGETCH.CFG) holds one exec_path per line, e.g.:
//   /APPS/FILES
//   /APPS/TERMINAL
// Each line is validated against g_menu_items (sm_find_item_by_path) before
// being added, same defensive shape as widgets_cfg_poll() validating a bind
// against widget_registry(): a path that does not resolve to a real menu
// item is skipped rather than stored, so this channel can never plant a dead
// favourite that startmenu_get_favorites() would then silently hide forever
// (see its own "silently skipped rather than shown with placeholder data"
// comment - that skip is a safety net for a renamed/removed app, not a
// place to route unvalidated wizard input into).
//
// A path already in g_fav_paths is treated as a successful no-op (the
// wizard's intent - "this app should be pinned" - is already satisfied),
// NOT a toggle-remove: this deliberately does not reuse
// startmenu_toggle_favorite_path()'s TOGGLE behaviour for the add case,
// whose toggle semantics would unpin an app the user had already pinned by
// hand before running the wizard.
//
// #745 task #67 "dockpanel": A LEADING '-' MEANS "UNPIN THIS PATH", added so
// the new Settings > Dock panel (a SEPARATE process, same constraint as the
// wizard above) can offer a Remove button. This is still not a blind
// toggle: a '-' line is a no-op unless the path IS currently pinned
// (sm_is_favorite_path() checked first, exactly like the add case checks
// the opposite), so it can never accidentally ADD something by relying on
// toggle semantics. Once a '-' line is confirmed pinned, it drives
// startmenu_toggle_favorite_path() directly (safe here precisely because
// pinned-ness was just confirmed, so the toggle's only reachable branch is
// the removal one) - the SAME writer the dock's own right-click "Unpin from
// Favorites" (#44) already uses, so this channel still has exactly one
// path to sm_save_state(), never a second hand-rolled one.
//
// MAX_FAVORITES (12) IS A HARD CAP on ADD lines. If the channel lists more
// valid, not-yet-pinned paths than there is room for, the excess lines are
// dropped silently once the list is full - first line in the file wins,
// matching the file's natural read order. This is a deliberate product
// decision, not an oversight: the alternative (refusing the whole batch, or
// growing past the cap) would either discard a selection that mostly fit or
// break every other piece of code that sizes a favourites list/array off
// MAX_FAVORITES. A '-' (remove) line is never subject to this cap: removing
// an item can only shrink the list.
//
// Applied through the SAME add path startmenu_toggle_favorite_path() uses
// for a brand-new favourite (append + sm_save_state()), just without its
// toggle-off branch, so the write goes through the one function that
// persists this list to disk rather than a second hand-rolled writer.
//
// Consumed (sys_unlink()'d) after processing if at least one line resolved
// to something real (an add that named a real menu item, OR a remove that
// named a currently-pinned path), same one-shot idiom as WIDGETCH.CFG: a
// channel that keeps re-firing would fight a user re-pinning something five
// minutes later. A file where NO line resolved to anything real (every path
// unknown/typo'd) is left in place, same as widgets_cfg_poll()'s "a broken
// write is at least visible on disk" choice, rather than vanishing with
// nothing applied and no trace of why.
static void sm_load_favs_channel(void) {
    int fd = userconf_open_read("FAVCH.CFG", 0);   // no legacy: brand-new channel
    if (fd < 0) return;
    static char buf[1024];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    int resolved = 0;   // at least one line named something real (add-target or existing pin)
    int added    = 0;   // at least one NEW favourite was appended (batched save below)
    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) { *p = 0; p++; }
        char *t = sm_trim(line);
        if (t[0] == 0 || t[0] == '#') continue;

        if (t[0] == '-') {   // #745 task #67: remove-from-dock line
            const char *rp = t + 1;
            if (rp[0] == 0) continue;
            if (!sm_is_favorite_path(rp)) continue;   // not currently pinned: no-op
            resolved = 1;
            startmenu_toggle_favorite_path(rp);   // known-pinned, so this can only remove;
                                                    // saves + redraws internally
            continue;
        }

        if (sm_find_item_by_path(t) < 0) continue;   // not a real menu item: skip
        resolved = 1;

        if (sm_is_favorite_path(t)) continue;         // already pinned: no-op, not a toggle
        if (g_fav_count >= MAX_FAVORITES) continue;    // #745 P1: hard cap, drop silently

        strncpy(g_fav_paths[g_fav_count], t, 127);
        g_fav_paths[g_fav_count][127] = 0;
        g_fav_count++;
        added = 1;
    }

    if (added) sm_save_state();
    if (added) g_needs_redraw = true;

    if (resolved) {
        char path[256];
        if (userconf_path("FAVCH.CFG", path, sizeof(path)) == 0) sys_unlink(path);
    }
}

// Called once per compositor frame from main.c, same throttled-poll idiom as
// startmenu_prefs_poll() above (this file's own established pattern for a
// wizard/Settings live-apply channel), just with an independent static
// counter so the two throttles cannot be confused with a single shared one.
void startmenu_favs_poll(void) {
    static int throttle = 0;
    if (++throttle < 30) return;   // ~ once a second at the ~33ms main-loop tick
    throttle = 0;
    sm_load_favs_channel();
}

// ============================================================================
// Recents tracking + launch
// ============================================================================

static void sm_record_recent(const char *path) {
    if (path[0] == '@') return;   // skip pseudo-actions (Recycle Bin sentinel)
    int found = -1;
    for (int i = 0; i < g_recent_count; i++) {
        if (strcmp(g_recent_paths[i], path) == 0) { found = i; break; }
    }
    if (found == 0) return;   // already the most recent: nothing to do
    if (found > 0) {
        for (int i = found; i > 0; i--)
            strncpy(g_recent_paths[i], g_recent_paths[i-1], 128);
    } else {
        int last = (g_recent_count < MAX_RECENTS) ? g_recent_count : MAX_RECENTS - 1;
        for (int i = last; i > 0; i--)
            strncpy(g_recent_paths[i], g_recent_paths[i-1], 128);
        if (g_recent_count < MAX_RECENTS) g_recent_count++;
    }
    strncpy(g_recent_paths[0], path, 127);
    g_recent_paths[0][127] = 0;
    sm_save_state();
}

// Launch item idx the same way a real click always has (native sys_spawn /
// Win16 win16_run / DOS dos_run / the @RECYCLE Files sentinel), then record it
// as the most recent launch and close the menu. Shared by the real click path,
// Enter-to-launch-first-result, and the TESTHOOK MENUITEM verb, so there is
// exactly one launch switch instead of three copies drifting apart.
static void sm_launch_item(int idx) {
    if (idx < 0 || idx >= g_total_items) return;
    menu_item_t *it = &g_menu_items[idx];
    const char *path = it->exec_path;
    switch (it->launch_type) {
        case LAUNCH_WIN16: win16_run(path); break;
        case LAUNCH_DOS:   dos_run(path);   break;
        default:
            if (path[0] == '@' && path[1] == 'R') {
                int fd = sys_open("/RECYVIEW.FLG", 0x41);
                if (fd >= 0) { sys_write(fd, "1", 1); sys_close(fd); }
                sys_spawn("/APPS/FILES");
            } else sys_spawn(path);
            break;
    }
    sm_record_recent(path);
    g_start_menu_open = false;
    g_search_len = 0; g_search[0] = 0;
    g_hover_row = -1; g_hover_power = 0;
    sm_close_flyout();   // #563
    g_needs_redraw = true;
}

// ============================================================================
// Context-menu-driven actions (called from contextmenu.c via compositor.h)
// ============================================================================

bool startmenu_item_is_favorite(int item_idx) {
    return sm_is_favorite_idx(item_idx);
}

// #26 XFCE-style dock: path-keyed favorite toggle, factored out of
// startmenu_item_toggle_favorite() below so the dock's right-click "Unpin
// from Favorites" action can drive the SAME list by exec_path. A dock icon
// for a pinned app has no g_menu_items index to work with (the Start Menu
// may not even be open), so the path is the only identity available.
void startmenu_toggle_favorite_path(const char *path) {
    int found = -1;
    for (int i = 0; i < g_fav_count; i++)
        if (strcmp(g_fav_paths[i], path) == 0) { found = i; break; }
    if (found >= 0) {
        for (int i = found; i < g_fav_count - 1; i++)
            strncpy(g_fav_paths[i], g_fav_paths[i+1], 128);
        g_fav_count--;
    } else if (g_fav_count < MAX_FAVORITES) {
        strncpy(g_fav_paths[g_fav_count], path, 127);
        g_fav_paths[g_fav_count][127] = 0;
        g_fav_count++;
    }
    sm_save_state();
    g_needs_redraw = true;
}

void startmenu_item_toggle_favorite(int item_idx) {
    if (item_idx < 0 || item_idx >= g_total_items) return;
    startmenu_toggle_favorite_path(g_menu_items[item_idx].exec_path);
}

bool startmenu_is_favorite_path(const char *path) {
    return sm_is_favorite_path(path);
}

// #26: expose the Favorites list as the dock's "pinned apps" row. Cross-
// references each stored exec_path against g_menu_items to recover the
// display name/icon/launch type (the favorites list itself persists only
// the path). A favorite whose source item no longer exists (app removed)
// is silently skipped rather than shown with placeholder data.
int startmenu_get_favorites(sm_fav_info_t *out, int max) {
    int n = 0;
    for (int i = 0; i < g_fav_count && n < max; i++) {
        for (int j = 0; j < g_total_items; j++) {
            if (g_menu_items[j].is_separator) continue;
            if (strcmp(g_menu_items[j].exec_path, g_fav_paths[i]) != 0) continue;
            strncpy(out[n].name, g_menu_items[j].name, sizeof(out[n].name) - 1);
            out[n].name[sizeof(out[n].name) - 1] = '\0';
            strncpy(out[n].exec_path, g_menu_items[j].exec_path, sizeof(out[n].exec_path) - 1);
            out[n].exec_path[sizeof(out[n].exec_path) - 1] = '\0';
            out[n].icon_id     = g_menu_items[j].icon_id;
            out[n].launch_type = g_menu_items[j].launch_type;
            n++;
            break;
        }
    }
    return n;
}

// #44 (dock context-menu task, 2026-08-12): reverse-lookup a kernel-resolved
// app_id (wm_window_info_t.app_id, a binary basename like "PAINT" - see #41)
// back to the g_menu_items[] entry it came from, so a RUNNING-but-not-pinned
// dock tile can offer "Pin to Dock" and "Change Icon" with a real exec_path/
// icon_id target instead of neither, which was the only option before #41
// landed app_id. Matching logic mirrors taskbar.c's own tb_app_id_matches()
// (case-insensitive compare against exec_path's final path component) -
// small enough that duplicating it here beats adding a cross-file dependency
// for one ~10-line loop. Returns false (and never writes *out) if app_id is
// empty or matches nothing, which callers MUST treat as "no identity" per
// the same rule tb_app_id_matches()'s own comment documents.
bool startmenu_find_by_app_id(const char *app_id, sm_fav_info_t *out) {
    if (!app_id || !app_id[0] || !out) return false;
    for (int j = 0; j < g_total_items; j++) {
        if (g_menu_items[j].is_separator) continue;
        const char *exec_path = g_menu_items[j].exec_path;
        const char *base = exec_path;
        for (const char *p = exec_path; *p; p++) if (*p == '/') base = p + 1;
        int i = 0;
        for (; app_id[i] && base[i]; i++) {
            char a = app_id[i], b = base[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (app_id[i] != '\0' || base[i] != '\0') continue;
        strncpy(out->name, g_menu_items[j].name, sizeof(out->name) - 1);
        out->name[sizeof(out->name) - 1] = '\0';
        strncpy(out->exec_path, exec_path, sizeof(out->exec_path) - 1);
        out->exec_path[sizeof(out->exec_path) - 1] = '\0';
        out->icon_id     = g_menu_items[j].icon_id;
        out->launch_type = g_menu_items[j].launch_type;
        return true;
    }
    return false;
}

// #44: apply every per-user custom dock/menu icon override that exists on
// disk. Storage has NO separate index file to go stale (blame.md is full of
// those going wrong): the override for a given app is simply whether
// "<home>/ICONS/<basename>.MICO" exists, where <basename> is the SAME
// deterministic name icon_mico_basename()/the "Change Icon" import path
// writes (icons.c). Called once at startup, after startmenu_init() has
// populated g_menu_items[] and profile_load() has resolved the session user
// (main.c), so this survives a rebuild of the icon set exactly like any
// other per-user preference under userhome_path() (#683). icon_load_color()
// replaces an existing slot in place rather than appending (see icons.c), so
// this is safe to treat as idempotent if ever called more than once.
void startmenu_apply_icon_overrides(void) {
    for (int j = 0; j < g_total_items; j++) {
        if (g_menu_items[j].is_separator) continue;
        char base[40];
        icon_mico_basename(g_menu_items[j].exec_path, base, sizeof(base));
        if (!base[0]) continue;
        char path[192];
        if (userhome_path("ICONS", base, path, sizeof(path)) != 0) continue;
        int fd = sys_open(path, 0);
        if (fd < 0) continue;
        sys_close(fd);
        icon_load_color(g_menu_items[j].icon_id, path);
    }
}

// #26: launch an arbitrary exec_path/launch_type pair - the same switch
// sm_launch_item() uses, exposed for the dock's pinned-icon click (which has
// no g_menu_items index to hand sm_launch_item() once the item came from the
// favorites list rather than a currently-rendered menu row).
void startmenu_launch_path(const char *path, int launch_type) {
    switch (launch_type) {
        case LAUNCH_WIN16: win16_run(path); break;
        case LAUNCH_DOS:   dos_run(path);   break;
        default:
            if (path[0] == '@' && path[1] == 'R') {
                int fd = sys_open("/RECYVIEW.FLG", 0x41);
                if (fd >= 0) { sys_write(fd, "1", 1); sys_close(fd); }
                sys_spawn("/APPS/FILES");
            } else sys_spawn(path);
            break;
    }
    sm_record_recent(path);
    g_needs_redraw = true;
}

void startmenu_item_add_to_desktop(int item_idx) {
    if (item_idx < 0 || item_idx >= g_total_items) return;
    menu_item_t *it = &g_menu_items[item_idx];
    if (desktop_add_icon(it->name, it->exec_path, it->icon_id))
        notify_post("Start Menu", "Added to the desktop", NOTIFY_SUCCESS);
    else
        notify_post("Start Menu", "Could not add to desktop (already there, or the desktop is full)", NOTIFY_WARNING);
}

void startmenu_item_open_properties(int item_idx) {
    if (item_idx < 0 || item_idx >= g_total_items) return;
    g_props_item = item_idx;
    g_props_open = true;
    g_needs_redraw = true;
}

// ============================================================================
// Properties popup: a small, minimal, TRUE modal (closes only via its own
// Close button or ESC - never click-away - per CLAUDE.md's settings-modal
// rule). Shows exactly what MayteraOS actually knows about an item (name,
// exec path, launch type); no invented metadata.
// ============================================================================

bool startmenu_properties_open(void) { return g_props_open; }

static void sm_props_rect(int *px, int *py, int *pw, int *ph) {
    *pw = 340; *ph = 150;
    *px = (g_fb_width - *pw) / 2;
    *py = (g_fb_height - *ph) / 2;
}

void startmenu_properties_render(void) {
    if (!g_props_open) return;
    if (g_props_item < 0 || g_props_item >= g_total_items) { g_props_open = false; return; }
    menu_item_t *it = &g_menu_items[g_props_item];

    int px, py, pw, ph; sm_props_rect(&px, &py, &pw, &ph);
    draw_fill_rect(px + 3, py + 3, pw, ph, CLR_MENU_SHADOW);
    draw_fill_rect(px, py, pw, ph, CLR_MENU_BG);
    draw_rect_outline(px, py, pw, ph, CLR_MENU_BORDER);

    draw_text(px + 12, py + 10, "Properties", CLR_MENU_TEXT);
    draw_hline(px + 8, py + 30, pw - 16, CLR_MENU_SEP);

    uint32_t dim = readable_ink_dim(CLR_MENU_BG);
    draw_text(px + 12, py + 42, "Name", dim);
    draw_text(px + 100, py + 42, it->name, CLR_MENU_TEXT);
    draw_text(px + 12, py + 64, "Path", dim);
    draw_text(px + 100, py + 64, it->exec_path, CLR_MENU_TEXT);
    const char *type = (it->launch_type == LAUNCH_WIN16) ? "Win16 (NE)"
                      : (it->launch_type == LAUNCH_DOS)   ? "MS-DOS"
                      :                                      "Native ELF";
    draw_text(px + 12, py + 86, "Type", dim);
    draw_text(px + 100, py + 86, type, CLR_MENU_TEXT);

    int bw = 90, bh = 28;
    int bx = px + pw - bw - 12, by = py + ph - bh - 12;
    draw_fill_rect(bx, by, bw, bh, CLR_MENU_ITEM_NORM);
    draw_rect_outline(bx, by, bw, bh, CLR_MENU_BORDER);
    draw_text_centered(bx + bw / 2, by + (bh - FONT_CHAR_H) / 2, "Close", CLR_MENU_TEXT);
}

bool startmenu_properties_handle_mouse(int32_t x, int32_t y, bool clicked) {
    if (!g_props_open) return false;
    int px, py, pw, ph; sm_props_rect(&px, &py, &pw, &ph);
    int bw = 90, bh = 28;
    int bx = px + pw - bw - 12, by = py + ph - bh - 12;
    if (clicked && x >= bx && x < bx + bw && y >= by && y < by + bh) {
        g_props_open = false;
        g_needs_redraw = true;
    }
    // True modal: every other click (including outside the panel) is simply
    // swallowed, never dismisses it - only the Close button or ESC does.
    return true;
}

int startmenu_properties_handle_key(int key) {
    if (!g_props_open) return 0;
    if (key == 27 /* ESC */) { g_props_open = false; g_needs_redraw = true; }
    return 1;
}

// ============================================================================
// Power / session confirm dialog: a TRUE modal (Cancel/confirm buttons or ESC
// only). Shutdown/Restart call the existing real ACPI paths. Log Out and Lock
// are now real (#566, docs/SECURE_LOGIN_DESIGN.md): Lock calls lock_enter()
// (lockscreen.c), which takes over input/rendering via the kernel-authoritative
// SYS_SESSION_LOCK. Log Out ends this compositor session with a clean exit so
// the kernel re-enters its login gate (kernel/main.c loops back to
// login_init()/login_run() when desktop_run() returns, per the #566 kernel
// design) instead of falling through to the unauthenticated kernel_shell().
// ============================================================================

bool startmenu_power_confirm_open(void) { return g_power_confirm != 0; }

static void sm_confirm_rect(int *px, int *py, int *pw, int *ph) {
    *pw = 300; *ph = 130;
    *px = (g_fb_width - *pw) / 2;
    *py = (g_fb_height - *ph) / 2;
}

static const char *sm_confirm_title(int action) {
    switch (action) {
        case 1: return "Shut Down";
        case 2: return "Restart";
        case 3: return "Log Out";
        case 4: return "Lock Screen";
        default: return "";
    }
}

static const char *sm_confirm_body(int action) {
    switch (action) {
        case 1: return "Shut down MayteraOS now?";
        case 2: return "Restart MayteraOS now?";
        case 3: return "Log out and return to the login screen?";
        case 4: return "Lock the screen now?";
        default: return "";
    }
}

void startmenu_power_confirm_render(void) {
    if (!g_power_confirm) return;
    int px, py, pw, ph; sm_confirm_rect(&px, &py, &pw, &ph);

    // Dim the desktop behind the dialog (same technique launcher.c uses).
    g_draw_blend = 130;
    draw_fill_rect(0, 0, g_fb_width, g_fb_height, 0xFF0B0D12);
    g_draw_blend = 255;

    draw_fill_rect(px + 3, py + 3, pw, ph, CLR_MENU_SHADOW);
    draw_fill_rect(px, py, pw, ph, CLR_MENU_BG);
    draw_rect_outline(px, py, pw, ph, CLR_MENU_BORDER);

    draw_text(px + 16, py + 14, sm_confirm_title(g_power_confirm), CLR_MENU_TEXT);
    draw_text(px + 16, py + 42, sm_confirm_body(g_power_confirm), readable_ink_dim(CLR_MENU_BG));

    int bw = (pw - 36) / 2, bh = 30;
    int by = py + ph - bh - 14;
    int bx1 = px + 12, bx2 = px + 12 + bw + 12;

    draw_fill_rect(bx1, by, bw, bh, CLR_MENU_ITEM_NORM);
    draw_rect_outline(bx1, by, bw, bh, CLR_MENU_BORDER);
    draw_text_centered(bx1 + bw / 2, by + (bh - FONT_CHAR_H) / 2, "Cancel", CLR_MENU_TEXT);

    draw_fill_rect(bx2, by, bw, bh, CLR_MENU_ITEM_NORM);
    draw_rect_outline(bx2, by, bw, bh, CLR_MENU_BORDER);
    draw_text_centered(bx2 + bw / 2, by + (bh - FONT_CHAR_H) / 2, sm_confirm_title(g_power_confirm), CLR_POWER_RED);
}

static void sm_power_confirm_yes(void) {
    int action = g_power_confirm;
    g_power_confirm = 0;
    g_needs_redraw = true;
    switch (action) {
        case 1: poweroff(); break;   // real ACPI/emulator power-off
        case 2: reboot();   break;   // real ACPI reset
        case 3:
            // #566 Log Out: clean process exit is the documented log-out path
            // - the kernel re-enters its login gate when desktop_run() returns
            // from a normal exit (not a crash), rather than falling through to
            // the unauthenticated kernel_shell().
            sys_bootlog("compositor: Log Out (Start Menu) -> clean exit for login re-entry");
            sys_exit(0);
            break;
        case 4:
            lock_enter();   // #566 lockscreen.c - kernel-authoritative session lock
            break;
        default: break;
    }
}

bool startmenu_power_confirm_handle_mouse(int32_t x, int32_t y, bool clicked) {
    if (!g_power_confirm) return false;
    int px, py, pw, ph; sm_confirm_rect(&px, &py, &pw, &ph);
    int bw = (pw - 36) / 2, bh = 30;
    int by = py + ph - bh - 14;
    int bx1 = px + 12, bx2 = px + 12 + bw + 12;
    if (clicked) {
        if (x >= bx1 && x < bx1 + bw && y >= by && y < by + bh) {
            g_power_confirm = 0; g_needs_redraw = true; return true;
        }
        if (x >= bx2 && x < bx2 + bw && y >= by && y < by + bh) {
            sm_power_confirm_yes(); return true;
        }
    }
    return true;   // true modal: swallow every other click while open
}

int startmenu_power_confirm_handle_key(int key) {
    if (!g_power_confirm) return 0;
    if (key == 27 /* ESC */) { g_power_confirm = 0; g_needs_redraw = true; return 1; }
    if (key == '\n' || key == '\r') { sm_power_confirm_yes(); return 1; }
    return 1;
}

// ============================================================================
// Power section layout (2x2 grid: Lock/Log Out on top, Restart/Shut Down below)
// ============================================================================

static int32_t sm_power_section_h(void) {
    return 8 + 28 * 2 + 6 + 8;   // top pad + 2 rows + inter-row gap + bottom pad
}

static void sm_power_rects(int32_t mx, int32_t w, int32_t sec_y,
                            int32_t *bw, int32_t *bh,
                            int32_t *row1_y, int32_t *row2_y,
                            int32_t *bx1, int32_t *bx2) {
    *bw = (w - 24) / 2;
    *bh = 28;
    *row1_y = sec_y + 8;
    *row2_y = *row1_y + *bh + 6;
    *bx1 = mx + 8;
    *bx2 = mx + 8 + *bw + 8;
}

// ============================================================================
// Row list builder: the ONE place that decides what the menu currently shows.
// ============================================================================

static void sm_push_row(int kind, int cat_idx, int item_idx) {
    if (g_row_count >= SM_MAX_ROWS) return;
    g_rows[g_row_count].kind = kind;
    g_rows[g_row_count].cat_idx = cat_idx;
    g_rows[g_row_count].item_idx = item_idx;
    g_row_count++;
}

static void sm_build_rows(void) {
    g_row_count = 0;

    if (g_search_len > 0) {
        sm_push_row(SM_ROW_SEARCHHDR, -1, -1);
        int shown = 0;
        for (int i = 0; i < g_total_items && shown < 14; i++) {
            if (g_menu_items[i].is_separator) continue;
            if (!sm_item_matches(i, g_search)) continue;
            sm_push_row(SM_ROW_ITEM, -1, i);
            shown++;
        }
        if (shown == 0) sm_push_row(SM_ROW_NOMATCH, -1, -1);
        return;
    }

    if (g_sm_show_fav && g_fav_count > 0) {
        sm_push_row(SM_ROW_FAVHDR, -1, -1);
        for (int i = 0; i < g_fav_count; i++) {
            int idx = sm_find_item_by_path(g_fav_paths[i]);
            if (idx >= 0) sm_push_row(SM_ROW_ITEM, -1, idx);
        }
    }

    if (g_sm_show_recent && g_recent_count > 0) {
        sm_push_row(SM_ROW_RECHDR, -1, -1);
        int n = g_recent_count;
        if (n > g_sm_recent_count) n = g_sm_recent_count;
        for (int i = 0; i < n; i++) {
            int idx = sm_find_item_by_path(g_recent_paths[i]);
            if (idx >= 0) sm_push_row(SM_ROW_ITEM, -1, idx);
        }
    }

    if (g_sm_view == 1) {
        // All Apps: one flat, non-collapsible list. Not scrollable yet (#see
        // CHANGELOG/design-call note) - a very long list is clipped to the
        // screen height rather than overflowing off it.
        sm_push_row(SM_ROW_ALLHDR, -1, -1);
        for (int i = 0; i < g_total_items && g_row_count < SM_MAX_ROWS; i++) {
            if (g_menu_items[i].is_separator) continue;
            sm_push_row(SM_ROW_ITEM, -1, i);
        }
    } else {
        // #563: category rows are headers ONLY now - opening one shows its
        // items in a side flyout (see startmenu_render_flyout()/g_flyout_cat)
        // instead of inlining them here. This is what keeps the root panel's
        // height bounded regardless of how many categories exist or how many
        // items any one of them holds (previously an accordion, which grew the
        // whole panel taller than the screen with several categories open or
        // one large one like Games).
        for (int c = 0; c < MAX_CATEGORIES; c++) {
            if (g_categories[c].label[0] == '\0') continue;
            sm_push_row(SM_ROW_CATHDR, c, -1);
        }
    }
}

static int32_t sm_row_h(int kind) {
    return (kind == SM_ROW_ITEM || kind == SM_ROW_NOMATCH) ? START_MENU_ITEM_H : START_MENU_CAT_H;
}

// ============================================================================
// #563: root panel geometry - height-capped to the screen. Header+search stay
// fixed at the top and the power grid fixed at the bottom; the row list
// between them (categories/Favorites/Recent/search results) gets a capped
// viewport that scrolls (g_sm_scroll) when its natural content is taller than
// the screen allows. Replaces the old unbounded calc_menu_height(); rebuilds
// g_rows as a side effect (same as the function it replaces), and
// render()/handle_mouse()/handle_right_click() all call this so the three can
// never disagree about where anything is on screen (same "one row list"
// discipline the file already uses for g_rows itself).
// ============================================================================
typedef struct {
    int32_t mx, my, w, mh;     // outer panel bounds (mh already capped)
    int32_t fixed_top;         // header + search height
    int32_t rows_y;            // screen Y where the row-list viewport starts
    int32_t rows_content_h;    // full (uncapped) height of all rows
    int32_t viewport_h;        // capped height actually shown
    bool    scrollable;
} sm_geom_t;

static sm_geom_t sm_root_geom(void) {
    sm_geom_t g;
    sm_build_rows();

    g.w  = g_sm_width;
    g.mx = TASKBAR_PADDING;
    g.fixed_top = START_MENU_HEADER_H + START_MENU_SEARCH_H;
    int32_t fixed_bot = START_MENU_SEP_H + sm_power_section_h();

    g.rows_content_h = 0;
    for (int i = 0; i < g_row_count; i++) g.rows_content_h += sm_row_h(g_rows[i].kind);

    // Vertical budget for the WHOLE panel: #387 top-bar layouts drop the menu
    // down from the top bar instead of up from a bottom taskbar.
    bool drop_top = taskbar_menu_drops_from_top();
    int32_t avail;
    if (drop_top) avail = g_fb_height - (taskbar_top_inset() + 2) - 4;
    else          avail = taskbar_get_y() - 4 - 4;   // 4px taskbar gap + 4px top margin
    int32_t floor = g.fixed_top + fixed_bot + START_MENU_ITEM_H;
    if (avail < floor) avail = floor;

    int32_t viewport_budget = avail - g.fixed_top - fixed_bot;
    if (viewport_budget < START_MENU_ITEM_H) viewport_budget = START_MENU_ITEM_H;

    g.scrollable  = g.rows_content_h > viewport_budget;
    g.viewport_h  = g.scrollable ? viewport_budget : g.rows_content_h;
    g.mh = g.fixed_top + g.viewport_h + fixed_bot;

    g.my = drop_top ? (taskbar_top_inset() + 2) : (taskbar_get_y() - g.mh - 4);
    if (g.my < 0) g.my = 0;
    g.rows_y = g.my + g.fixed_top;

    sm_scroll_config(&g_sm_scroll, g.rows_content_h, START_MENU_ITEM_H, g.viewport_h);
    return g;
}

// Screen Y (top edge) of category cat_idx's header row, given the root
// panel's current scroll. Clamped into the visible viewport if the header is
// currently scrolled past (so a flyout opened via the keyboard always anchors
// somewhere on screen instead of at a coordinate that no longer means
// anything).
static int32_t sm_cathdr_screen_y(int cat_idx, const sm_geom_t *g) {
    int32_t cy = g->rows_y - g_sm_scroll.offset;
    for (int i = 0; i < g_row_count; i++) {
        int32_t rh = sm_row_h(g_rows[i].kind);
        if (g_rows[i].kind == SM_ROW_CATHDR && g_rows[i].cat_idx == cat_idx) {
            if (cy < g->rows_y) cy = g->rows_y;
            if (cy > g->rows_y + g->viewport_h - rh) cy = g->rows_y + g->viewport_h - rh;
            return cy;
        }
        cy += rh;
    }
    return g->rows_y;   // category not currently in the row list; should not happen
}

// #563: geometry for the flyout column of the currently-open category
// (g_flyout_cat), independently height-capped to the screen with its own
// scroll (g_fly_scroll) - so a category with many items (Games, with every
// installed game including AssaultCube) never renders off-screen either, the
// same guarantee sm_root_geom() gives the root panel.
typedef struct {
    int32_t x, y, w, h;
    int32_t viewport_h;
    int     item_count;
    bool    scrollable;
} sm_fly_geom_t;

static sm_fly_geom_t sm_flyout_geom(const sm_geom_t *root) {
    sm_fly_geom_t f;
    f.item_count = 0; f.x = f.y = f.w = f.h = f.viewport_h = 0; f.scrollable = false;
    if (g_flyout_cat < 0 || g_flyout_cat >= MAX_CATEGORIES) return f;
    if (g_categories[g_flyout_cat].label[0] == '\0') return f;

    menu_category_t *cat = &g_categories[g_flyout_cat];
    f.item_count = cat->item_count;
    if (f.item_count <= 0) return f;
    f.w = root->w;   // same width/chrome as the root panel

    // (local 81) The flyout lives in the same WORK AREA as the root panel, not
    // the raw screen. sm_root_geom() has always bounded itself with
    // taskbar_top_inset()/taskbar_get_y(); this column bounded itself with
    // g_fb_height, and since it renders LAST (render_frame_body draws the
    // taskbar at layer 5 and the start menu at layer 6) its overflow was
    // painted ON TOP of the dock rather than clipped by it. With the shipped
    // Games category the clamp pinned the bottom edge at g_fb_height-4, so the
    // final row sat squarely on the taskbar; one more item made it two rows.
    int32_t wa_x, wa_y, wa_w, wa_h;
    taskbar_work_area(&wa_x, &wa_y, &wa_w, &wa_h);
    (void)wa_x; (void)wa_w;
    int32_t area_top = wa_y + POPUP_EDGE_MARGIN;
    int32_t area_bot = wa_y + wa_h - POPUP_EDGE_MARGIN;
    int32_t max_h = area_bot - area_top;
    if (max_h < START_MENU_ITEM_H + 8) max_h = START_MENU_ITEM_H + 8;

    int32_t natural_h = 8 + f.item_count * START_MENU_ITEM_H;   // top+bottom pad
    f.scrollable = natural_h > max_h;
    if (f.scrollable) {
        // Snap the viewport to a WHOLE number of rows. The sm_scroll_t comment
        // at the top of this file claims "g_fly_scroll uses snap=1 (never
        // leaves a row half-visible at the viewport edge)"; sm_scroll_config()
        // has no snap parameter and never did, so the last row was drawn as a
        // few-pixel sliver and then cut mid-row by the draw_push_clip() in the
        // render loop. That is the second half of the reported clipping, and
        // it is the half that appears exactly when a category gains one more
        // item than the viewport can hold.
        int32_t rows = (max_h - 8) / START_MENU_ITEM_H;
        if (rows < 1) rows = 1;
        f.viewport_h = rows * START_MENU_ITEM_H;
        f.h = f.viewport_h + 8;
    } else {
        f.h = natural_h;
        f.viewport_h = f.h - 8;
    }

    f.y = sm_cathdr_screen_y(g_flyout_cat, root);
    if (f.y + f.h > area_bot) f.y = area_bot - f.h;
    if (f.y < area_top) f.y = area_top;

    // Open right; if it would run off the right edge, open left instead.
    if (root->mx + root->w + f.w <= g_fb_width - 4)      f.x = root->mx + root->w;
    else if (root->mx - f.w >= 4)                        f.x = root->mx - f.w;
    else                                                  f.x = g_fb_width - f.w - 4;  // last-resort clamp

    sm_scroll_config(&g_fly_scroll, f.item_count * START_MENU_ITEM_H, START_MENU_ITEM_H, f.viewport_h);
    return f;
}

// Close the flyout (used on menu close, click-away, category re-toggle, and
// LEFT-arrow keyboard navigation).
static void sm_close_flyout(void) {
    if (g_flyout_cat >= 0 && g_flyout_cat < MAX_CATEGORIES)
        g_categories[g_flyout_cat].expanded = false;
    g_flyout_cat = -1;
    g_flyout_hover_item = -1;
    g_fly_scroll.offset = 0;
}

static void sm_open_flyout(int cat_idx) {
    if (g_flyout_cat == cat_idx) return;
    sm_close_flyout();
    g_flyout_cat = cat_idx;
    g_categories[cat_idx].expanded = true;
    g_flyout_hover_item = g_categories[cat_idx].item_count > 0
                         ? g_categories[cat_idx].item_start : -1;
    g_fly_scroll.offset = 0;
}

// ----------------------------------------------------------------------------
// Win16 program groups
// ----------------------------------------------------------------------------
// The native MayteraOS "Win3.x installer" (RC `win16install`) writes a simple
// line-oriented config file that this compositor reads at start-menu init. Each
// program group becomes one collapsible accordion category whose items launch a
// Win16 (NE/.COM) executable via the win16_run() syscall rather than spawning a
// native ELF.
//
// /WIN16GRP.CFG format (one record per line, fields separated by '|'):
//   GROUP|<group display name>
//   ITEM|<item display name>|<exec path>
// ITEM lines apply to the most recently declared GROUP. Lines starting with '#'
// and blank lines are ignored. Categories are labelled "PROGRAMS \\ <group>" so
// they read as a nested program group under Programs.

// Split a config line on the next '|'. Returns the field (trimmed) and advances
// *pp past the '|' (or to the terminating NUL when no '|' is left).
static char *sm_next_field(char **pp)
{
    char *s = *pp;
    char *bar = s;
    while (*bar && *bar != '|') bar++;
    if (*bar == '|') { *bar = 0; *pp = bar + 1; }
    else             { *pp = bar; }
    return sm_trim(s);
}

// startmenu_load_games_cfg() (the old /GAMES.CFG "ITEM|name|path|type" reader)
// is GONE, not merely emptied: its two entries (Commander Keen 4/6) now live
// as ordinary data in build/assets/startmenu/system.d/games.MENU alongside
// every other default game, read through the same sm_rust_rebuild() path as
// everything else. See startmenu_model.rs's header for why one merged path
// replaced four separate ad-hoc loaders.

// Map an icon= field from a Start-menu fragment (startmenu_model.rs) to an
// icon id. Unknown or missing -> a generic window icon. This now has to cover
// every icon the (former) hardcoded defaults used, because those defaults are
// data now (build/assets/startmenu/system.d/*.MENU) and reach the menu
// through this same lookup - it is no longer just the small set the old
// data-driven STARTMENU.YAML loader supported.
static icon_id_t sm_icon_by_name(const char *n)
{
    if (!n || !n[0])                     return ICON_WINDOW;
    if (strcmp(n, "clock")        == 0)  return ICON_CLOCK;
    if (strcmp(n, "calculator")   == 0)  return ICON_CALCULATOR;
    if (strcmp(n, "image")        == 0)  return ICON_IMAGE;
    if (strcmp(n, "network")      == 0)  return ICON_NETWORK;
    if (strcmp(n, "cog")          == 0)  return ICON_COG;
    if (strcmp(n, "task")         == 0)  return ICON_TASK_MANAGER;
    if (strcmp(n, "log")          == 0)  return ICON_LOG_VIEWER;
    if (strcmp(n, "terminal")     == 0)  return ICON_TERMINAL;
    if (strcmp(n, "highlight")    == 0)  return ICON_HIGHLIGHT;
    if (strcmp(n, "folder")       == 0)  return ICON_FOLDER;
    if (strcmp(n, "info")         == 0)  return ICON_INFO_CIRCLE;
    if (strcmp(n, "music")        == 0)  return ICON_MUSIC;
    if (strcmp(n, "power")        == 0)  return ICON_POWER;
    if (strcmp(n, "refresh")      == 0)  return ICON_REFRESH;
    if (strcmp(n, "home")         == 0)  return ICON_HOME;
    if (strcmp(n, "file")         == 0)  return ICON_FILE;
    if (strcmp(n, "palette")      == 0)  return ICON_PALETTE;
    if (strcmp(n, "paint")        == 0)  return ICON_PAINT;
    if (strcmp(n, "trash")        == 0)  return ICON_TRASH;
    if (strcmp(n, "trash_full")   == 0)  return ICON_TRASH_FULL;
    if (strcmp(n, "game")         == 0)  return ICON_GAME;
    if (strcmp(n, "game_doom")    == 0)  return ICON_GAME_DOOM;
    if (strcmp(n, "game_pong")    == 0)  return ICON_GAME_PONG;
    if (strcmp(n, "game_solitaire") == 0) return ICON_GAME_SOLITAIRE;
    if (strcmp(n, "game_lemmings")  == 0) return ICON_GAME_LEMMINGS;
    if (strcmp(n, "computer")     == 0)  return ICON_COMPUTER;
    if (strcmp(n, "browser")      == 0)  return ICON_BROWSER;
    if (strcmp(n, "irc")          == 0)  return ICON_IRC;
    if (strcmp(n, "video")        == 0)  return ICON_VIDEO;
    if (strcmp(n, "win3x")        == 0)  return ICON_WIN3X;
    if (strcmp(n, "dosapp")       == 0)  return ICON_DOSAPP;
    if (strcmp(n, "game_arena")   == 0)  return ICON_GAME_ARENA;
    if (strcmp(n, "game_chess")   == 0)  return ICON_GAME_CHESS;
    if (strcmp(n, "game_squadron") == 0) return ICON_GAME_SQUADRON;
    if (strcmp(n, "game_glcube")  == 0)  return ICON_GAME_GLCUBE;
    if (strcmp(n, "game_glmatrix") == 0) return ICON_GAME_GLMATRIX;
    if (strcmp(n, "aichat")       == 0)  return ICON_AICHAT;
    if (strcmp(n, "weather")      == 0)  return ICON_WEATHER;
    if (strcmp(n, "feeds")        == 0)  return ICON_FEEDS;
    if (strcmp(n, "gallery")      == 0)  return ICON_GALLERY;
    if (strcmp(n, "snapshot")     == 0)  return ICON_SNAPSHOT;
    if (strcmp(n, "notes")        == 0)  return ICON_NOTES;
    if (strcmp(n, "fontbook")     == 0)  return ICON_FONTBOOK;
    if (strcmp(n, "converter")    == 0)  return ICON_CONVERTER;
    if (strcmp(n, "timers")       == 0)  return ICON_TIMERS;
    if (strcmp(n, "python")       == 0)  return ICON_PYTHON;
    if (strcmp(n, "auth")         == 0)  return ICON_AUTH;
    if (strcmp(n, "help")         == 0)  return ICON_HELP;
    if (strcmp(n, "launcher")     == 0)  return ICON_LAUNCHER;
    if (strcmp(n, "taskswitch")   == 0)  return ICON_TASKSWITCH;
    if (strcmp(n, "appstore")     == 0)  return ICON_APPSTORE;
    if (strcmp(n, "sysmon")       == 0)  return ICON_SYSMON;
    if (strcmp(n, "services")     == 0)  return ICON_SERVICES;
    if (strcmp(n, "print3d")      == 0)  return ICON_3DPRINT;
    return ICON_WINDOW;
}

// ============================================================================
// Rust content model bridge (startmenu_model.rs) - the two additive config
// layers, no hardcoded entry anywhere. See that file's header comment for the
// full grammar, merge semantics, and the "no fallback, ever" rule.
// ============================================================================

// dirent.h/unistd.h both pull in libc/types.h ("typedef _Bool bool;"), which
// conflicts with this TU's own compositor.h ("typedef int bool;", unguarded) -
// the exact landmine this file's top comment already documents for
// gui_scroll.h. Open-coded here rather than including those headers: `DIR` is
// opaque everywhere outside dirent.c (safe to forward-declare with no body),
// and this mirrors struct dirent's layout from libc/dirent.h exactly (d_name
// is the only field used).
typedef struct DIR DIR;
struct sm_dirent { unsigned long d_ino; long d_off; unsigned short d_reclen;
                    unsigned char d_type; char d_name[256]; };
extern DIR *opendir(const char *name);
extern struct sm_dirent *readdir(DIR *dirp);
extern int closedir(DIR *dirp);
extern int access(const char *path, int mode);

// ---- Rust FFI: exported by startmenu_model.rs, called from C -------------
extern void  sm_model_reset(void);
extern void  sm_model_add_fragment(const uint8_t *text, uint32_t text_len);
extern int32_t sm_model_finish(void);

// ---- Rust FFI: called BY startmenu_model.rs, implemented here ------------
int32_t sm_c_path_exists(const uint8_t *path, uint32_t path_len)
{
    static char buf[160];
    unsigned n = path_len < sizeof(buf) - 1 ? path_len : (unsigned)sizeof(buf) - 1;
    memcpy(buf, path, n);
    buf[n] = 0;
    if (buf[0] == '@') return 1;   // pseudo-action sentinel (e.g. @RECYCLE): exempt
    return access(buf, 0) == 0 ? 1 : 0;   // F_OK
}

void sm_c_add_category(const uint8_t *label, uint32_t label_len, int32_t expanded)
{
    static char buf[64];
    unsigned n = label_len < sizeof(buf) - 1 ? label_len : (unsigned)sizeof(buf) - 1;
    memcpy(buf, label, n);
    buf[n] = 0;
    if (g_next_cat >= MAX_CATEGORIES) return;   // add_category() also bounds-checks; this
                                                  // avoids burning the item_count-scan below
                                                  // on a category that was never registered.
    add_category(g_next_cat, buf, expanded != 0);
    g_next_cat++;
}

void sm_c_add_item(const uint8_t *name, uint32_t name_len,
                    const uint8_t *path, uint32_t path_len,
                    const uint8_t *icon, uint32_t icon_len,
                    int32_t launch_type)
{
    static char nbuf[64], pbuf[160], ibuf[32];
    unsigned nn = name_len < sizeof(nbuf) - 1 ? name_len : (unsigned)sizeof(nbuf) - 1;
    unsigned pn = path_len < sizeof(pbuf) - 1 ? path_len : (unsigned)sizeof(pbuf) - 1;
    unsigned in = icon_len < sizeof(ibuf) - 1 ? icon_len : (unsigned)sizeof(ibuf) - 1;
    memcpy(nbuf, name, nn); nbuf[nn] = 0;
    memcpy(pbuf, path, pn); pbuf[pn] = 0;
    memcpy(ibuf, icon, in); ibuf[in] = 0;
    add_item_typed(nbuf, sm_icon_by_name(ibuf), pbuf, launch_type);
}

// Read every "*.MENU" fragment in `dir`, filename-sorted, feeding each one's
// full text to the Rust model in order. A missing directory contributes zero
// fragments and is NOT an error and NOT a signal to fall back to anything -
// see startmenu_model.rs's "NO FALLBACK, EVER" note. Single-shot sys_open/
// sys_read per fragment, no loop that can block (#426); fragment counts are
// expected in the tens, not thousands, so an insertion sort is plenty.
#define SM_FRAG_MAX  64
#define SM_FRAG_NAME 64
static void sm_feed_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;

    static char names[SM_FRAG_MAX][SM_FRAG_NAME];
    int n = 0;
    struct sm_dirent *e;
    while (n < SM_FRAG_MAX && (e = readdir(d)) != NULL) {
        int l = 0; while (e->d_name[l]) l++;
        if (l < 6) continue;                                  // shorter than "X.MENU"
        if (strcmp(e->d_name + l - 5, ".MENU") != 0) continue;
        strncpy(names[n], e->d_name, SM_FRAG_NAME - 1);
        names[n][SM_FRAG_NAME - 1] = 0;
        n++;
    }
    closedir(d);

    // Deterministic load order: filename sort (insertion sort; n is tiny).
    for (int i = 1; i < n; i++) {
        char tmp[SM_FRAG_NAME]; strcpy(tmp, names[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], tmp) > 0) { strcpy(names[j + 1], names[j]); j--; }
        strcpy(names[j + 1], tmp);
    }

    static char path[192];
    static char text[8192];
    for (int i = 0; i < n; i++) {
        int di = 0; while (dir[di] && di < 150) { path[di] = dir[di]; di++; }
        path[di++] = '/';
        int fi = 0; while (names[i][fi] && di < 190) path[di++] = names[i][fi++];
        path[di] = 0;

        int fd = sys_open(path, 0);
        if (fd < 0) continue;
        long rl = sys_read(fd, text, sizeof(text) - 1);
        sys_close(fd);
        if (rl <= 0) continue;
        sm_model_add_fragment((const uint8_t *)text, (uint32_t)rl);
    }
}

// All-users layer: shipped from the repo (build/assets/startmenu/system.d/)
// and overlaid onto every golden by build-golden.sh, PLUS whatever the App
// Store / auto-updater writes at install time straight into the same live
// directory (userland/libc/startmenu_reg.c) - both are "the system layer",
// one populated at build time, one at install time, exactly the "adapted
// when apps are built/installed" requirement. Multiple absolute paths are
// tried for the same reason startmenu_load_win16_groups() tries several: the
// FAT-only single-partition live image and the ext2-root two-partition
// golden route "/CONFIG" differently, and only one of a given image's routes
// will resolve.
static void sm_feed_system_layer(void)
{
    static const char *const dirs[] = {
        "/CONFIG/STARTMENU/SYSTEM.D",
        "/ext2/CONFIG/STARTMENU/SYSTEM.D",
    };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        sm_feed_dir(dirs[i]);
}

// Per-user layer: personal hides/renames/pins, keyed by the session username
// (profile_current_username(), profile.c) so multiple accounts do not share
// one file. Never shipped by the build; an absent or empty directory means
// "no personal customization", not "no menu" - the system layer alone still
// has to produce the menu, per the same no-fallback rule.
static void sm_feed_user_layer(void)
{
    char user[64];
    profile_current_username(user, sizeof(user));
    if (!user[0]) return;   // no resolvable session identity: no user layer, not an error

    static const char *const prefixes[] = { "/CONFIG/STARTMENU/USER/", "/ext2/CONFIG/STARTMENU/USER/" };
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        char dir[128];
        int di = 0; const char *pfx = prefixes[i];
        while (pfx[di] && di < 100) { dir[di] = pfx[di]; di++; }
        int ui = 0; while (user[ui] && di < 126) dir[di++] = user[ui++];
        dir[di] = 0;
        sm_feed_dir(dir);
    }

    // #745 THIRD FEED: the user's OWN home, <home>/CONFIG/STARTMENU/.
    //
    // This is where an unprivileged App Store install registers itself
    // (startmenu_register_app_user(), libc/startmenu_reg.c), and it is the
    // reason a per-user install becomes VISIBLE rather than merely successful.
    // Before this, nothing anywhere in the OS searched any per-user location
    // for an application, so an app installed into a home directory existed and
    // was unreachable.
    //
    // It is a separate feed from the two above and not a third entry in that
    // array because the path is not "<prefix><username>": it comes from the
    // PASSWD home field via the one shared join, so an account whose home is
    // not /HOME/<name> still works.
    //
    // Root's home is "/", so this resolves to "/CONFIG/STARTMENU", the parent
    // of SYSTEM.D. That directory holds no .MENU fragments of its own, so a
    // root session feeds nothing extra here and its menu is unchanged.
    {
        char hdir[192];
        if (userhome_path("CONFIG", "STARTMENU", hdir, sizeof(hdir)) == 0)
            sm_feed_dir(hdir);
    }
}

// Full rebuild: reset -> feed system layer -> feed user layer -> merge +
// existence-check + emit (sm_model_finish(), which calls sm_c_add_category()/
// sm_c_add_item() above). g_categories/g_menu_items/g_total_items are reset
// here too so this function is a complete, idempotent replacement for the old
// startmenu_init() hardcoded block - call it and the menu IS whatever the two
// config layers say, nothing more.
static void sm_rust_rebuild(void)
{
    memset(g_categories, 0, sizeof(g_categories));
    memset(g_menu_items,  0, sizeof(g_menu_items));
    g_total_items = 0;
    g_next_cat    = 0;   // no built-in categories reserve a slot any more

    sm_model_reset();
    sm_feed_system_layer();
    sm_feed_user_layer();
    sm_model_finish();
}

// Throttled poll, same idiom as startmenu_prefs_poll() (~once/second at the
// ~33ms main-loop tick): cheap for the expected fragment counts (tens, not
// thousands), and it is what lets an App Store install or a hand-edited user
// fragment reach the live menu without a compositor restart, let alone a
// rebuild. Runs sm_rust_rebuild() unconditionally on each throttled tick
// rather than trying to detect "did anything change" first - see this
// function's own comment in the .h for why that tradeoff was made.
void startmenu_rust_poll(void)
{
    static int throttle = 0;
    if (++throttle < 30) return;
    throttle = 0;
    sm_rust_rebuild();
}

// startmenu_load_apps_cfg() (the #454 /CONFIG/STARTMNU.YML "YAML-subset"
// reader: category:/item:/rename:) is GONE. Its grammar is a strict subset of
// startmenu_model.rs's (same three directive names, same '|' field
// separator, PLUS this file's hide: and per-item id= for stable identity
// across layers) - anything that lived in STARTMNU.YML now belongs in a
// build/assets/startmenu/system.d/*.MENU fragment (or, for a purely local
// customization never meant to ship, a /CONFIG/STARTMENU/USER/<user>/*.MENU
// fragment). See startmenu_model.rs's header comment for the full grammar.

// #134: Load Win16 program groups from /WIN16GRP.CFG and render each as its own
// collapsible Start-menu category (folder), with items launched via win16_run().
// The ole2c kernel's Win3.x installer writes this file to the ext2 root. Format,
// one record per line (fields separated by '|'):
//   GROUP|<group display name>
//   ITEM|<item display name>|<exec path>[|<icon, ignored>]
// ITEM lines belong to the most recently declared GROUP. Lines beginning with
// '#' and blank lines are ignored. Groups occupy category slots AFTER
// whatever sm_rust_rebuild() (the two-layer config merge) already emitted, up
// to MAX_CATEGORIES, sharing the same g_next_cat counter. This loader stays a
// separate, legitimately-already-data-driven third source deliberately: it is
// synthesized by the kernel's own Win3.x installer, not hand-authored by a
// user or the App Store, and folding its producer into the system/user
// fragment format would mean changing kernel Win16-install code, out of this
// file's scope. If the file is absent nothing is added.
static void startmenu_load_win16_groups(void)
{
    // Try several locations. On the ole2c kernel the config lives at the ext2
    // root; the explicit "/ext2/..." forms bypass the root-fs routing so the
    // read still succeeds regardless of how paths are resolved.
    static const char *const paths[] = {
        "/WIN16GRP.CFG", "/ext2/WIN16GRP.CFG",
        "/CONFIG/WIN16GRP.CFG", "/ext2/CONFIG/WIN16GRP.CFG",
    };
    static char buf[8192];
    long n = -1;
    for (unsigned pi = 0; pi < sizeof(paths) / sizeof(paths[0]); pi++) {
        int fd = sys_open(paths[pi], 0);
        if (fd < 0) continue;
        n = sys_read(fd, buf, sizeof(buf) - 1);
        sys_close(fd);
        if (n > 0) break;
    }
    if (n <= 0) return;
    buf[n] = 0;

    // Next free category slot is shared (g_next_cat): sm_rust_rebuild() (the
    // two-layer config merge) has already consumed however many categories
    // the system/user fragments produced by the time this runs.
    bool have_group = false;

    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) { *p = 0; p++; }
        char *t = sm_trim(line);
        if (t[0] == 0 || t[0] == '#') continue;

        if (strncmp(t, "GROUP|", 6) == 0) {
            char *cur  = t + 6;
            char *name = sm_next_field(&cur);
            if (!name[0]) { have_group = false; continue; }
            if (g_next_cat >= MAX_CATEGORIES) { have_group = false; continue; }
            add_category(g_next_cat, name, false);   // collapsed by default
            g_next_cat++;
            have_group = true;
        } else if (strncmp(t, "ITEM|", 5) == 0) {
            if (!have_group) continue;   // ITEM before any GROUP: skip
            char *cur  = t + 5;
            char *name = sm_next_field(&cur);
            char *path = sm_next_field(&cur);
            // Optional 4th icon field is ignored; all Win16 items use ICON_WIN3X.
            if (!name[0] || !path[0]) continue;
            add_item_typed(name, ICON_WIN3X, path, LAUNCH_WIN16);
        }
        // Any other record type is ignored.
    }
}

// ============================================================================
// Public API
// ============================================================================

// THE DIRECTIVE THIS FUNCTION EXISTS TO SATISFY: no hardcoded Start-menu
// entry, ever, not even as a fallback for a missing or unreadable config
// layer. Every category and item that used to be compiled in here (58
// add_item()/add_item_typed() calls across 5 categories, measured before this
// change) is now DATA in build/assets/startmenu/system.d/*.MENU (git-tracked,
// shipped onto every golden by build-golden.sh so a build reproduces the
// default menu byte-for-byte) merged with any per-user customization by
// sm_rust_rebuild() (startmenu_model.rs). If both config layers are absent,
// empty, or fail to parse, the menu comes up with the Win16 program-group
// folders and NOTHING else - not a smaller version of the old list, none of
// it. That is correct, not a bug: see startmenu_model.rs's "NO FALLBACK,
// EVER" note.
void startmenu_init(void)
{
    g_hover_row   = -1;
    g_hover_gear  = false;
    g_hover_power = 0;
    g_power_confirm = 0;
    g_props_open  = false;
    g_search_len  = 0;
    g_search[0]   = 0;
    g_search_focused = true;

    // The two-layer additive config merge (system, then user), existence-
    // checked, emitted into g_categories/g_menu_items. Resets and rebuilds
    // those arrays itself.
    sm_rust_rebuild();

    // #134: Win16 program groups (ProgMan) as extra Start-menu folders. Read
    // from /WIN16GRP.CFG (written by the ole2c kernel's Win3.x installer). Each
    // GROUP becomes a collapsible category whose items launch via win16_run().
    // A separate, already-data-driven source; see its own comment for why it
    // was not folded into the fragment format above.
    startmenu_load_win16_groups();

    // Start-menu uplift: load persisted favorites/recents + Settings prefs.
    // #63/#745: sm_load_state() returning false means STARTMENU.CFG has
    // never been written for this profile at all (not merely "zero
    // favorites saved") - the skip-the-wizard case. Seed the default pinned
    // set then, and only then, so a profile that DID save an intentionally
    // empty list (every default later unpinned by hand) is never re-seeded.
    if (!sm_load_state()) sm_seed_default_favorites();
    sm_load_prefs();
}

// Shared scrollbar draw (root and flyout both use this - see the sm_scroll_t
// comment near the top of this file for why gui_scroll.h/gui_scroll_draw()
// itself is not used).
static void sm_draw_scrollbar(const sm_scroll_t *s, int32_t x, int32_t y, int32_t h)
{
    int32_t max = sm_scroll_max(s, h);
    if (max <= 0 || s->content_px <= 0) return;
    int32_t th = (int32_t)(((int64_t)h * (int64_t)h) / (int64_t)s->content_px);
    if (th < SM_SCROLL_MIN_TH) th = SM_SCROLL_MIN_TH;
    if (th > h) th = h;
    int32_t travel = h - th;
    int32_t ty = y + (travel > 0 ? (int32_t)(((int64_t)travel * (int64_t)s->offset) / (int64_t)max) : 0);
    // The start menu paints its gutter from the MENU palette, not the
    // scrollbar tokens, so it takes the rule rather than the colours. Measured
    // before this (tools/contrast/scrollbar-contrast.sh, second table): the
    // thumb was below the 3:1 non-text floor on 10 of the 14 themes, and on
    // sunset it was 1.00:1, the thumb drawn in the same colour as the trough it
    // sits in. It is now 3.30:1 to 6.03:1 everywhere.
    // The 0xFF prefix is the compositor's own convention for an opaque colour
    // (see CLR_* above); the shared rule returns a plain 0x00RRGGBB.
    uint32_t sm_thumb = 0xFF000000u | gui_scroll_thumb_ink(CLR_MENU_BORDER,
                                                           CLR_MENU_CAT_BG,
                                                           CLR_MENU_BG);
    draw_fill_rect(x, y, SM_SCROLL_W, h, CLR_MENU_CAT_BG);
    draw_fill_rect(x + 2, ty, SM_SCROLL_W - 4, th, sm_thumb);
}

// #563: the flyout column for the currently-open category (g_flyout_cat),
// drawn to the side of the root panel instead of the old inline accordion.
// Independently height-capped + scrollable, same guarantee as the root panel.
static void startmenu_render_flyout(const sm_geom_t *root)
{
    if (g_flyout_cat < 0) return;
    sm_fly_geom_t f = sm_flyout_geom(root);
    if (f.item_count <= 0) return;
    menu_category_t *cat = &g_categories[g_flyout_cat];

    draw_fill_rect(f.x + 3, f.y + 3, f.w, f.h, CLR_MENU_SHADOW);
    draw_fill_rect(f.x, f.y, f.w, f.h, CLR_MENU_BG);
    draw_rect_outline(f.x, f.y, f.w, f.h, CLR_MENU_BORDER);

    int32_t list_y = f.y + 4;
    // (#745 follow-up) push/pop, not set/clear: this scrollable list is
    // reachable only while g_start_menu_open (render_frame_idle()'s partial
    // path never runs then, so today set/clear is inert here), but push/pop
    // costs nothing extra and removes the latent full-screen-reset hazard
    // for good rather than leaving it dependent on that gate never changing.
    if (f.scrollable) draw_push_clip(f.x, list_y, f.w, f.viewport_h);

    int32_t first = g_fly_scroll.offset / START_MENU_ITEM_H;
    int32_t iy = list_y - (g_fly_scroll.offset - first * START_MENU_ITEM_H);
    for (int i = first; i < cat->item_count && iy < list_y + f.viewport_h; i++) {
        int abs_idx = cat->item_start + i;
        menu_item_t *it = &g_menu_items[abs_idx];
        uint32_t bg = (g_flyout_hover_item == abs_idx) ? CLR_MENU_ITEM_HOVER : CLR_MENU_BG;
        draw_fill_rect(f.x, iy, f.w, START_MENU_ITEM_H, bg);
        int32_t isz = g_sm_icon_size;
        icon_draw_scaled(it->icon_id, f.x + 8, iy + (START_MENU_ITEM_H - isz) / 2, isz, CLR_MENU_TEXT);
        draw_text(f.x + 8 + isz + 6, iy + (START_MENU_ITEM_H - FONT_CHAR_H) / 2, it->name, CLR_MENU_TEXT);
        if (sm_is_favorite_idx(abs_idx))
            draw_text(f.x + f.w - START_MENU_PADDING - FONT_CHAR_W - 2,
                      iy + (START_MENU_ITEM_H - FONT_CHAR_H) / 2, "*", CLR_MENU_TEXT);
        iy += START_MENU_ITEM_H;
    }
    if (f.scrollable) draw_pop_clip();

    if (f.scrollable) sm_draw_scrollbar(&g_fly_scroll, f.x + f.w - SM_SCROLL_W, list_y, f.viewport_h);
}

// (#745) Fill a row background on the start-menu surface. On GLASS, a plain
// CLR_MENU_BG fill is not a background, it is an eraser: it paints over the
// blurred+tinted surface with the opaque token and the translucency vanishes.
// Anything that is genuinely a row STATE (hover, expanded category) still
// paints, because that is a real mark on the surface rather than the surface's
// own colour.
static void sm_row_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bg)
{
    if (g_glass_enable && bg == CLR_MENU_BG) return;   // the surface IS the bg
    draw_fill_rect(x, y, w, h, bg);
}

// Muted ink on the menu surface: measured against the material actually on
// screen (the glass tint) at the glass mix, not against the opaque token.
static uint32_t sm_dim_ink(void)
{
    return g_glass_enable ? readable_ink_dim_mix(CLR_GLASS_TINT, 22)
                          : readable_ink_dim(CLR_MENU_BG);
}

// (#745 contrast) What a caller filling `bg` actually leaves on screen: under
// glass, sm_row_fill() ERASES a plain CLR_MENU_BG fill (the surface already
// shows the blurred+tinted glass, painting the opaque token over it would
// hide the translucency), so the pixel the eye sees there is CLR_GLASS_TINT,
// not CLR_MENU_BG. Any other bg (a real hover/expanded/selection paint) is
// exactly what gets drawn. Mirroring sm_row_fill()'s own condition here is
// what keeps ink and surface from ever disagreeing about what is on screen.
static uint32_t sm_actual_bg(uint32_t bg)
{
    return (g_glass_enable && bg == CLR_MENU_BG) ? CLR_GLASS_TINT : bg;
}

// Primary ink for the menu surface's own resting background (header, search
// placeholder swap, anywhere no specific row fill is in play).
//
// Before this existed, every one of these sites used CLR_MENU_TEXT, which
// main.c computes ONCE as readable_ink(CLR_MENU_BG) - the flat, opaque
// THEME_COLOR_MENU_BG token. That is only the real on-screen colour when the
// surface is opaque. Under glass the resting surface is CLR_GLASS_TINT, which
// is derived from THEME_COLOR_TASKBAR_BG (not menu_bg) and darkened/lightened
// again - a genuinely different colour, and on themes that pair a dark
// taskbar with a light "paper" menu_bg (ocean, forest, sunset) it is a
// different LUMINANCE CLASS, so readable_ink(CLR_MENU_BG) picks the ink that
// is wrong for what glass actually painted. Text stayed legible only on rows
// that ALSO painted a real opaque fill on top (hover, an expanded category,
// a selection highlight) - which is exactly the "only the item I'm hovering
// is readable" shape reported against this menu and the desktop context menu
// alike. See sm_ink_for() for the per-row-background form of the same fix.
static uint32_t sm_ink(void) { return readable_ink(sm_actual_bg(CLR_MENU_BG)); }

// Primary ink for a specific fill a caller is about to paint (or, under
// glass, skip painting per sm_row_fill's erase rule) - the per-row form of
// sm_ink(); see the comment there for why a single fixed CLR_MENU_TEXT could
// not be right for every row state.
static uint32_t sm_ink_for(uint32_t bg) { return readable_ink(sm_actual_bg(bg)); }

void startmenu_render(void)
{
    if (!g_start_menu_open) return;

    sm_geom_t G = sm_root_geom();   // #563: capped height, rebuilds g_rows
    int32_t w  = G.w, mx = G.mx, my = G.my, mh = G.mh;

    // Shadow + surface + border.
    // (#745) The menu is a glass surface: blurred backdrop, then the themed
    // tint at the user's dock_opacity. Bleed is taken on all four sides.
    //
    // The shadow drops to alpha 64. It used to be an opaque CLR_MENU_SHADOW
    // slab, and behind a translucent panel an opaque shadow reads as a second,
    // SOLID panel sitting behind a pane of glass, which is worse than no shadow
    // at all.
    {
        int ob = g_draw_blend;
        if (g_glass_enable) g_draw_blend = 64;
        draw_fill_rect(mx + 3, my + 3, w, mh, CLR_MENU_SHADOW);
        g_draw_blend = ob;
    }
    if (g_glass_enable) {
        glass_render(mx, my, w, mh, CLR_GLASS_TINT, GLASS_SURF_MENU);
        int ob = g_draw_blend;
        g_draw_blend = 128;
        draw_rect_outline(mx, my, w, mh, CLR_MENU_BORDER);
        // 1px white inner highlight along the top and left inside edges, at the
        // same alphas the dock uses.
        g_draw_blend = (draw_luminance(CLR_GLASS_TINT) >= 140) ? 140 : 26;
        draw_hline(mx + 1, my + 1, w - 2, 0xFFFFFFFF);
        draw_vline(mx + 1, my + 1, mh - 2, 0xFFFFFFFF);
        g_draw_blend = ob;
    } else {
        draw_fill_rect(mx, my, w, mh, CLR_MENU_BG);
        draw_rect_outline(mx, my, w, mh, CLR_MENU_BORDER);
    }

    int32_t cy = my;

    // ---- Header: user avatar + name + Settings gear ----
    {
        int32_t r = 15;
        int32_t acx = mx + START_MENU_PADDING + r;
        int32_t acy = cy + START_MENU_HEADER_H / 2;
        draw_circle_filled(acx, acy, r, CLR_LOGIN_AVATAR);
        draw_circle_outline(acx, acy, r, CLR_MENU_BORDER);
        char initial[2];
        initial[0] = g_login_username[0] ? g_login_username[0] : 'U';
        if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] = (char)(initial[0] - 32);
        initial[1] = 0;
        draw_text(acx - FONT_CHAR_W / 2, acy - FONT_CHAR_H / 2, initial, sm_ink());
        draw_text(acx + r + 10, cy + (START_MENU_HEADER_H - FONT_CHAR_H) / 2,
                  g_login_username[0] ? g_login_username : "User", sm_ink());

        int32_t gx = mx + w - START_MENU_PADDING - 20;
        int32_t gy = cy + (START_MENU_HEADER_H - 16) / 2;
        // (#745) Both arms now read the material actually on screen: the old
        // hover arm was CLR_MENU_TEXT (the flat, possibly-wrong-for-glass
        // token) while only the resting arm went through the glass-aware
        // sm_dim_ink() - backwards from the usual defect shape, but the same
        // bug: one of the two states was measured against the wrong bg.
        uint32_t gear_ink = g_hover_gear ? sm_ink() : sm_dim_ink();
        if (!icon_draw_color_tinted(ICON_COG, gx, gy, 16, gear_ink))
            icon_draw_scaled(ICON_COG, gx, gy, 16, gear_ink);
    }
    cy += START_MENU_HEADER_H;
    draw_hline(mx + 8, cy, w - 16, CLR_MENU_SEP);

    // ---- Search box ----
    {
        int32_t sx = mx + 8, sy = cy + 4, sw = w - 16, sh = START_MENU_SEARCH_H - 8;
        draw_fill_rect(sx, sy, sw, sh, CLR_MENU_CAT_BG);
        draw_rect_outline(sx, sy, sw, sh, CLR_MENU_BORDER);
        // (#745) The search box is a real opaque fill (CLR_MENU_CAT_BG, the
        // theme's selection colour), not the glass surface, so its ink must
        // be measured against THAT background, not CLR_MENU_BG - on ocean/
        // forest/sunset the two are opposite luminance classes.
        uint32_t search_ink = sm_ink_for(CLR_MENU_CAT_BG);
        uint32_t dim = readable_ink_dim(CLR_MENU_CAT_BG);
        if (g_search_len > 0) {
            draw_text(sx + 8, sy + (sh - FONT_CHAR_H) / 2, g_search, search_ink);
            if (g_search_focused && ((uptime_ms() / 500) & 1)) {
                int32_t cxp = sx + 8 + text_width(g_search) + 1;
                draw_fill_rect(cxp, sy + 3, 2, sh - 6, search_ink);
            }
        } else {
            draw_text(sx + 8, sy + (sh - FONT_CHAR_H) / 2, "Type to search...", dim);
        }
    }
    cy += START_MENU_SEARCH_H;

    // ---- Rows (search results, or Favorites/Recent + categories/all-apps) ----
    // #563: this region is a capped, scrollable VIEWPORT (G.viewport_h tall,
    // starting at G.rows_y == cy here) instead of growing to fit every row -
    // rows above/below the visible window are skipped entirely (not drawn),
    // and a clip keeps a partially-visible row at the top/bottom from
    // painting outside the viewport.
    if (G.scrollable) draw_push_clip(mx, cy, w, G.viewport_h);   // #745 follow-up: push/pop, see note above
    {
        int32_t ry = cy - g_sm_scroll.offset;
        for (int r = 0; r < g_row_count; r++) {
            sm_row_t *row = &g_rows[r];
            int32_t rh = sm_row_h(row->kind);
            if (ry + rh <= cy || ry >= cy + G.viewport_h) { ry += rh; continue; }
            // (#745) sm_dim_ink(), not readable_ink_dim(CLR_MENU_BG) directly:
            // the helper already accounts for glass, this call site was one of
            // the ones that bypassed it and measured against the flat token.
            uint32_t dim = sm_dim_ink();

            switch (row->kind) {
            case SM_ROW_CATHDR: {
                menu_category_t *cat = &g_categories[row->cat_idx];
                uint32_t bg = cat->expanded ? CLR_MENU_CAT_BG : CLR_MENU_BG;
                if (g_hover_row == r) bg = CLR_MENU_ITEM_HOVER;
                sm_row_fill(mx, ry, w, rh, bg);
                // (#745) Ink measured against what THIS row's bg actually put
                // on screen (glass tint at rest, the real hover/expanded fill
                // otherwise) - not a single CLR_MENU_TEXT fixed for the
                // opaque menu_bg token, which left every resting heading
                // unreadable on any theme where menu_bg and the glass tint
                // (derived from taskbar_bg) land on opposite luminance
                // classes. That is why only the hovered/expanded heading used
                // to read: it was the only row painting a bg ink was ever
                // actually right for.
                uint32_t ink = sm_ink_for(bg);
                draw_text(mx + START_MENU_PADDING + 4, ry + (rh - FONT_CHAR_H) / 2, cat->label, ink);
                // #563: chevron now always points right (opens a side flyout,
                // never expands down) while its category is not the open one;
                // the OPEN category's chevron points right too but highlighted,
                // matching the cascade convention (no more up/down "v"/">").
                if (!icon_draw_color_tinted(ICON_CHEVR,
                        mx + w - START_MENU_PADDING - 16, ry + (rh - 14) / 2, 14, ink))
                    draw_text(mx + w - START_MENU_PADDING - FONT_CHAR_W - 2,
                              ry + (rh - FONT_CHAR_H) / 2, ">", ink);
                break;
            }
            case SM_ROW_ITEM: {
                menu_item_t *it = &g_menu_items[row->item_idx];
                uint32_t bg = (g_hover_row == r) ? CLR_MENU_ITEM_HOVER : CLR_MENU_BG;
                sm_row_fill(mx, ry, w, rh, bg);
                uint32_t ink = sm_ink_for(bg);   // (#745) see SM_ROW_CATHDR above
                int32_t isz = g_sm_icon_size;
                int32_t icy = ry + (rh - isz) / 2;
                icon_draw_scaled(it->icon_id, mx + 8, icy, isz, ink);
                draw_text(mx + 8 + isz + 6, ry + (rh - FONT_CHAR_H) / 2, it->name, ink);
                if (sm_is_favorite_idx(row->item_idx))
                    draw_text(mx + w - START_MENU_PADDING - FONT_CHAR_W - 2,
                              ry + (rh - FONT_CHAR_H) / 2, "*", ink);
                break;
            }
            case SM_ROW_NOMATCH:
                draw_text(mx + START_MENU_PADDING + 4, ry + (rh - FONT_CHAR_H) / 2, "No matches", dim);
                break;
            case SM_ROW_FAVHDR:
                draw_text(mx + START_MENU_PADDING + 4, ry + (rh - FONT_CHAR_H) / 2, "Favorites", dim);
                break;
            case SM_ROW_RECHDR:
                draw_text(mx + START_MENU_PADDING + 4, ry + (rh - FONT_CHAR_H) / 2, "Recent", dim);
                break;
            case SM_ROW_ALLHDR:
                draw_text(mx + START_MENU_PADDING + 4, ry + (rh - FONT_CHAR_H) / 2, "All Apps", dim);
                break;
            case SM_ROW_SEARCHHDR:
                draw_text(mx + START_MENU_PADDING + 4, ry + (rh - FONT_CHAR_H) / 2, "Search Results", dim);
                break;
            }
            ry += rh;
        }
    }
    if (G.scrollable) draw_pop_clip();
    cy += G.viewport_h;
    if (G.scrollable) sm_draw_scrollbar(&g_sm_scroll, mx + w - SM_SCROLL_W, G.rows_y, G.viewport_h);

    // ---- Power section (2x2 grid) ----
    draw_hline(mx + 8, cy, w - 16, CLR_MENU_SEP);
    int32_t sec_y = cy;
    int32_t sec_h = sm_power_section_h();
    sm_row_fill(mx, sec_y, w, sec_h, CLR_MENU_BG);

    int32_t bw, bh, row1_y, row2_y, bx1, bx2;
    sm_power_rects(mx, w, sec_y, &bw, &bh, &row1_y, &row2_y, &bx1, &bx2);

    // (#745) "starkly bright" power row: main.c sets
    // CLR_MENU_ITEM_NORM = TC(THEME_COLOR_MENU_BG), the SAME value as
    // CLR_MENU_BG - but these four rects were painted with a plain
    // draw_fill_rect(), never through sm_row_fill(), so on a glass surface
    // they were the one thing in the whole panel still an OPAQUE flat menu_bg
    // slab sitting on top of the darker/lighter blurred CLR_GLASS_TINT
    // everything else on the panel is made of. Routing the same fill through
    // sm_row_fill() makes the four buttons erase to glass at rest exactly
    // like every other row, and still paint a real opaque highlight on
    // hover - matching the rest of the menu instead of standing out from it.
    struct { int32_t x, y; int code; const char *label; icon_id_t icon; bool danger; } btns[4] = {
        { bx1, row1_y, 4, "Lock",     ICON_COG,     false },
        { bx2, row1_y, 3, "Log Out",  ICON_REFRESH, false },
        { bx1, row2_y, 2, "Restart",  ICON_REFRESH, false },
        { bx2, row2_y, 1, "Shutdown", ICON_POWER,   true  },
    };
    for (int i = 0; i < 4; i++) {
        uint32_t bg = (g_hover_power == btns[i].code) ? CLR_MENU_ITEM_HOVER : CLR_MENU_ITEM_NORM;
        sm_row_fill(btns[i].x, btns[i].y, bw, bh, bg);
        draw_rect_outline(btns[i].x, btns[i].y, bw, bh, CLR_MENU_BORDER);
        // (#745) Ink measured against what THIS button's bg actually put on
        // screen (glass tint at rest, the real hover fill otherwise), same
        // sm_ink_for() as every other row above - not a fixed CLR_MENU_TEXT
        // chosen for the opaque menu_bg token. Shutdown keeps its red danger
        // accent, but readable_accent() keeps that red legible against
        // whichever background is actually behind it instead of assuming
        // the opaque token.
        uint32_t actual_bg = sm_actual_bg(bg);
        uint32_t ink = readable_ink(actual_bg);
        uint32_t icon_ink = btns[i].danger ? readable_accent(CLR_POWER_RED, actual_bg) : ink;
        icon_draw_scaled(btns[i].icon, btns[i].x + 4, btns[i].y + (bh - 14) / 2, 14, icon_ink);
        draw_text(btns[i].x + 22, btns[i].y + (bh - FONT_CHAR_H) / 2, btns[i].label, ink);
    }

    // #563: the cascading flyout renders LAST (on top of everything else,
    // including the power grid) so it is never occluded by the root panel.
    startmenu_render_flyout(&G);
}

// #563: flyout hit-test/click, shared by startmenu_handle_mouse() below and
// (read-only geometry reuse) startmenu_handle_right_click(). Returns true if
// (x,y) is within the flyout's bounds (consumed either way - hover or click).
static bool sm_flyout_hit(const sm_geom_t *root, int32_t x, int32_t y, bool clicked)
{
    if (g_flyout_cat < 0) return false;
    sm_fly_geom_t f = sm_flyout_geom(root);
    if (f.item_count <= 0) return false;
    if (x < f.x || x >= f.x + f.w || y < f.y || y >= f.y + f.h) return false;

    int32_t list_y = f.y + 4;
    if (y >= list_y && y < list_y + f.viewport_h) {
        int32_t rel = y - list_y + g_fly_scroll.offset;
        int i = rel / START_MENU_ITEM_H;
        menu_category_t *cat = &g_categories[g_flyout_cat];
        if (i >= 0 && i < cat->item_count) {
            int abs_idx = cat->item_start + i;
            g_flyout_hover_item = abs_idx;
            if (clicked) sm_launch_item(abs_idx);
        }
    }
    return true;   // inside flyout bounds either way: consume the click
}

bool startmenu_handle_mouse(int32_t x, int32_t y, bool clicked)
{
    if (!g_start_menu_open) return false;

    sm_geom_t G = sm_root_geom();   // #563: capped height, rebuilds g_rows
    int32_t w = G.w, mx = G.mx, my = G.my, mh = G.mh;

    // #563: the flyout renders on top and can sit OUTSIDE the root panel's
    // bounds (it opens to whichever side has room), so it must be tested
    // before the root out-of-bounds "click away closes the menu" check below.
    if (sm_flyout_hit(&G, x, y, clicked)) return true;

    // Outside menu bounds: a mouse-DOWN dismisses the menu (click-away to
    // close - standard launcher UX, not one of the settings-style modals
    // CLAUDE.md restricts), consumed so the same click does not also activate
    // whatever sits underneath.
    if (x < mx || x >= mx + w || y < my || y >= my + mh) {
        if (clicked) {
            g_start_menu_open = false;
            g_hover_row = -1; g_hover_power = 0; g_hover_gear = false;
            sm_close_flyout();
            g_needs_redraw = true;
            return true;
        }
        return false;
    }

    g_hover_row = -1; g_hover_power = 0; g_hover_gear = false;
    int32_t cy = my;

    // Header: only the gear (Settings) icon is interactive.
    {
        int32_t gx = mx + w - START_MENU_PADDING - 20 - 4;
        int32_t gy = cy + (START_MENU_HEADER_H - 16) / 2 - 4;
        if (x >= gx && x < gx + 28 && y >= gy && y < gy + 28) {
            g_hover_gear = true;
            if (clicked) {
                set_settings_tab(17);   // PANEL_STARTMENU - keep in sync with settings/main.c
                sys_spawn("/APPS/SETTINGS");
                g_start_menu_open = false;
                sm_close_flyout();
                g_needs_redraw = true;
            }
            return true;
        }
    }
    cy += START_MENU_HEADER_H;

    // Search box: clicking it (re)focuses typing there; consume so the click
    // does not fall through to anything behind the menu.
    if (y >= cy && y < cy + START_MENU_SEARCH_H) {
        if (clicked) { g_search_focused = true; g_needs_redraw = true; }
        return true;
    }
    cy += START_MENU_SEARCH_H;

    // #563: row-list viewport (scrollable) - same skip-if-outside-viewport
    // window as the render loop, so hover/click can never disagree with what
    // is actually drawn on screen.
    if (y >= cy && y < cy + G.viewport_h) {
        int32_t ry = cy - g_sm_scroll.offset;
        for (int r = 0; r < g_row_count; r++) {
            int32_t rh = sm_row_h(g_rows[r].kind);
            if (ry + rh <= cy || ry >= cy + G.viewport_h) { ry += rh; continue; }
            if (y >= ry && y < ry + rh) {
                g_hover_row = r;
                if (g_rows[r].kind == SM_ROW_CATHDR) {
                    if (clicked) {
                        int c = g_rows[r].cat_idx;
                        if (g_flyout_cat == c) sm_close_flyout();
                        else                   sm_open_flyout(c);
                        g_needs_redraw = true;
                    }
                } else if (g_rows[r].kind == SM_ROW_ITEM) {
                    if (clicked) sm_launch_item(g_rows[r].item_idx);
                }
                return true;
            }
            ry += rh;
        }
    }
    cy += G.viewport_h;

    // Power section (2x2 grid): clicking a button opens the confirm dialog
    // (the actual action only runs once the user confirms).
    int32_t sec_y = cy;
    int32_t bw, bh, row1_y, row2_y, bx1, bx2;
    sm_power_rects(mx, w, sec_y, &bw, &bh, &row1_y, &row2_y, &bx1, &bx2);

    struct { int32_t x, y; int code; } hits[4] = {
        { bx1, row1_y, 4 }, { bx2, row1_y, 3 }, { bx1, row2_y, 2 }, { bx2, row2_y, 1 },
    };
    for (int i = 0; i < 4; i++) {
        if (x >= hits[i].x && x < hits[i].x + bw && y >= hits[i].y && y < hits[i].y + bh) {
            g_hover_power = hits[i].code;
            if (clicked) {
                g_power_confirm = hits[i].code;
                g_start_menu_open = false;
                sm_close_flyout();
                g_needs_redraw = true;
            }
            return true;
        }
    }

    // Inside the menu bounds but over a gap/border: consume so clicks do not
    // pass through to the desktop underneath.
    return true;
}

// #563: mouse wheel over the root row-list viewport or an open flyout scrolls
// it. Called from main.c the same way widget_settings_handle_scroll() is -
// see the get_mouse_scroll() call site there. Returns 1 if consumed.
int startmenu_handle_scroll(int32_t x, int32_t y, int delta)
{
    if (!g_start_menu_open || delta == 0) return 0;

    sm_geom_t G = sm_root_geom();

    if (g_flyout_cat >= 0) {
        sm_fly_geom_t f = sm_flyout_geom(&G);
        if (f.item_count > 0 && x >= f.x && x < f.x + f.w && y >= f.y && y < f.y + f.h) {
            if (sm_scroll_wheel(&g_fly_scroll, delta, f.viewport_h)) g_needs_redraw = true;
            return 1;
        }
    }

    if (G.scrollable && x >= G.mx && x < G.mx + G.w && y >= G.rows_y && y < G.rows_y + G.viewport_h) {
        if (sm_scroll_wheel(&g_sm_scroll, delta, G.viewport_h)) g_needs_redraw = true;
        return 1;
    }

    return 0;
}

// Right-click on a Start-menu item opens its context menu (Pin/Unpin, Add to
// Desktop, Properties) via the shared contextmenu.c primitive. Returns true if
// the click landed on an item (and the context menu was opened).
bool startmenu_handle_right_click(int32_t x, int32_t y)
{
    if (!g_start_menu_open) return false;

    sm_geom_t G = sm_root_geom();   // rebuilds g_rows (harmless/idempotent)

    // #563: an open flyout's items support the same Pin/Add-to-Desktop/
    // Properties context menu as root-panel items.
    if (g_flyout_cat >= 0) {
        sm_fly_geom_t f = sm_flyout_geom(&G);
        if (f.item_count > 0 && x >= f.x && x < f.x + f.w && y >= f.y && y < f.y + f.h) {
            int32_t list_y = f.y + 4;
            if (y >= list_y && y < list_y + f.viewport_h) {
                int32_t rel = y - list_y + g_fly_scroll.offset;
                int i = rel / START_MENU_ITEM_H;
                menu_category_t *cat = &g_categories[g_flyout_cat];
                if (i >= 0 && i < cat->item_count) {
                    contextmenu_open_for_menuitem(x, y, cat->item_start + i);
                    return true;
                }
            }
            return true;   // inside the flyout but not on an item: consume, no menu
        }
    }

    if (x < G.mx || x >= G.mx + G.w || y < G.my || y >= G.my + G.mh) return false;

    if (y < G.rows_y || y >= G.rows_y + G.viewport_h) return false;
    int32_t cy = G.rows_y - g_sm_scroll.offset;
    for (int r = 0; r < g_row_count; r++) {
        int32_t rh = sm_row_h(g_rows[r].kind);
        if (cy + rh <= G.rows_y || cy >= G.rows_y + G.viewport_h) { cy += rh; continue; }
        if (y >= cy && y < cy + rh) {
            if (g_rows[r].kind == SM_ROW_ITEM) {
                contextmenu_open_for_menuitem(x, y, g_rows[r].item_idx);
                return true;
            }
            return false;
        }
        cy += rh;
    }
    return false;
}

// Keyboard: the search box (append/backspace/Enter-launches-first-match/ESC).
// Captures every key while the menu is open, same "modal while open" idiom as
// launcher.c's command launcher. Returns 1 if consumed.
int startmenu_handle_key(int key)
{
    if (!g_start_menu_open) return 0;

    if (key == 0x9B /* KEY_SUPER */) { startmenu_toggle(); return 1; }

    // ESC backs out one level at a time: an open flyout first, then the
    // search text, then finally the whole menu - same "undo the most recent
    // thing" idiom as a real cascading menu (Escape on a Windows Start Menu
    // submenu closes the submenu, not the whole menu).
    if (key == 27 /* ESC */) {
        if (g_flyout_cat >= 0) { sm_close_flyout(); g_needs_redraw = true; }
        else if (g_search_len > 0) { g_search_len = 0; g_search[0] = 0; g_needs_redraw = true; }
        else startmenu_toggle();
        return 1;
    }

    // #563: Up/Down/Left/Right navigate the category list and its cascading
    // flyout (mirrors mouse hover/click - both drive the same g_hover_row /
    // g_flyout_hover_item state the renderer highlights). Only meaningful
    // outside search (categories are not shown while searching).
    if (g_search_len == 0 && (key == SM_KEY_UP || key == SM_KEY_DOWN)) {
        sm_geom_t G = sm_root_geom();
        if (g_flyout_cat >= 0) {
            menu_category_t *cat = &g_categories[g_flyout_cat];
            if (cat->item_count > 0) {
                int local = (g_flyout_hover_item >= cat->item_start)
                          ? (g_flyout_hover_item - cat->item_start) : -1;
                if (key == SM_KEY_UP) local = (local <= 0) ? cat->item_count - 1 : local - 1;
                else                   local = (local + 1 >= cat->item_count) ? 0 : local + 1;
                g_flyout_hover_item = cat->item_start + local;
                sm_fly_geom_t f = sm_flyout_geom(&G);   // refresh g_fly_scroll's viewport/content for reveal below
                sm_scroll_reveal(&g_fly_scroll, local * START_MENU_ITEM_H, START_MENU_ITEM_H, f.viewport_h);
            }
        } else if (g_row_count > 0) {
            int idx = g_hover_row;
            for (int step = 0; step < g_row_count; step++) {
                idx = (key == SM_KEY_UP) ? (idx - 1 + g_row_count) % g_row_count
                                           : (idx + 1) % g_row_count;
                int k = g_rows[idx].kind;
                if (k == SM_ROW_CATHDR || k == SM_ROW_ITEM) { g_hover_row = idx; break; }
            }
            if (g_hover_row >= 0) {
                int32_t top = 0;
                for (int i = 0; i < g_hover_row; i++) top += sm_row_h(g_rows[i].kind);
                sm_scroll_reveal(&g_sm_scroll, top, sm_row_h(g_rows[g_hover_row].kind), G.viewport_h);
            }
        }
        g_needs_redraw = true;
        return 1;
    }
    if (g_search_len == 0 && key == SM_KEY_RIGHT) {
        if (g_flyout_cat < 0 && g_hover_row >= 0 && g_hover_row < g_row_count &&
            g_rows[g_hover_row].kind == SM_ROW_CATHDR) {
            sm_open_flyout(g_rows[g_hover_row].cat_idx);
            g_needs_redraw = true;
        }
        return 1;
    }
    if (g_search_len == 0 && key == SM_KEY_LEFT) {
        if (g_flyout_cat >= 0) { sm_close_flyout(); g_needs_redraw = true; }
        return 1;
    }

    if (key == '\n' || key == '\r') {
        // Enter activates whatever is currently selected: a flyout item, a
        // root category (opens its flyout, same as Right), a root item, or -
        // while actively typing a search - the first filtered match (unchanged).
        if (g_flyout_cat >= 0 && g_flyout_hover_item >= 0) {
            sm_launch_item(g_flyout_hover_item);
            return 1;
        }
        if (g_search_len == 0 && g_hover_row >= 0 && g_hover_row < g_row_count) {
            if (g_rows[g_hover_row].kind == SM_ROW_CATHDR) {
                sm_open_flyout(g_rows[g_hover_row].cat_idx);
                g_needs_redraw = true;
                return 1;
            }
            if (g_rows[g_hover_row].kind == SM_ROW_ITEM) {
                sm_launch_item(g_rows[g_hover_row].item_idx);
                return 1;
            }
        }
        if (g_search_len > 0) {
            for (int i = 0; i < g_total_items; i++) {
                if (g_menu_items[i].is_separator) continue;
                if (sm_item_matches(i, g_search)) { sm_launch_item(i); break; }
            }
        }
        return 1;
    }

    if (!g_search_focused) return 1;   // typing elsewhere (focus-on-open off): swallow, no-op

    if (key == '\b' || key == 8 || key == 127) {
        if (g_search_len > 0) { g_search[--g_search_len] = 0; g_needs_redraw = true; }
        return 1;
    }

    if (key >= 0x20 && key <= 0x7E) {
        if (g_search_len < SM_SEARCH_MAX) {
            g_search[g_search_len++] = (char)key;
            g_search[g_search_len] = 0;
            g_needs_redraw = true;
        }
        return 1;
    }

    return 1;
}

void startmenu_toggle(void)
{
    g_start_menu_open = !g_start_menu_open;
    g_hover_row   = -1;
    g_hover_power = 0;
    g_hover_gear  = false;
    sm_close_flyout();         // #563: never reopen with a stale flyout
    g_sm_scroll.offset = 0;    // #563: always start scrolled to the top
    if (g_start_menu_open) {
        g_search_len = 0; g_search[0] = 0;
        g_search_focused = g_sm_focus_search ? true : false;
    }
    g_needs_redraw = true;
}

#ifdef MAYTERA_TESTHOOK
// #334 headless verification hook ONLY - see compositor.h's declaration and
// testhook.c for the full explanation. Never compiled into a normal build.
// Mirrors the real launch path exactly (sm_launch_item()) but reaches it by
// NAME instead of by hit-testing a click inside the popup - the menu does not
// even need to be open for this to run.
bool startmenu_launch_item_by_name(const char *name) {
    if (!name || !name[0]) return false;
    int idx = sm_find_item_by_name(name);
    if (idx < 0) return false;
    sm_launch_item(idx);
    return true;
}

// Lookup-only variant (does not launch) for the MENUCTX/MENUPIN test verbs.
int startmenu_find_item_by_name(const char *name) {
    if (!name || !name[0]) return -1;
    return sm_find_item_by_name(name);
}

// Directly sets the search query (bypasses key-by-key injection) so the live
// type-to-filter logic can be verified deterministically. Opens the menu if it
// is not already open, since a search only means anything while visible.
void startmenu_set_search(const char *q) {
    if (!g_start_menu_open) startmenu_toggle();
    int i = 0;
    for (; q[i] && i < SM_SEARCH_MAX; i++) g_search[i] = q[i];
    g_search[i] = 0;
    g_search_len = i;
    g_search_focused = true;
    g_needs_redraw = true;
}

// #563: opens the Start menu and the named category's cascading flyout, by
// exact label match, bypassing hit-testing - see the compositor.h declaration.
void startmenu_open_category_by_name(const char *label) {
    if (!label || !label[0]) return;
    if (!g_start_menu_open) startmenu_toggle();
    for (int c = 0; c < MAX_CATEGORIES; c++) {
        if (g_categories[c].label[0] && strcmp(g_categories[c].label, label) == 0) {
            sm_open_flyout(c);
            break;
        }
    }
    g_needs_redraw = true;
}
#endif
