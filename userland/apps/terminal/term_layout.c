// term_layout.c
// PHASE 1 (terminal uplift): tabs + splits. See term_layout.h for the design
// decision that shapes this whole file (why the other modules keep their
// globals and this module banks them, rather than a ctx* threaded through
// ~40 functions in six files five other agents are editing right now).

#include "term_common.h"
#include "term_util.h"
#include "term_grid.h"
#include "term_scrollback.h"
#include "term_parse.h"
#include "term_render.h"
#include "term_theme.h"
#include "term_prefs.h"
#include "term_pty.h"
#include "term_shell.h"
#include "term_notify.h"
#include "term_layout.h"
#include "term_select.h"
#include "term_search.h"   // #221: selection owns the click term_mouse_report() does not

// ===========================================================================
// 1. THE PANE
// ===========================================================================
// Everything that used to be a module-scope global in term_grid.c,
// term_scrollback.c, term_parse.c, term_pty.c and term_shell.c, plus the
// pane's own identity and pty child. tl_activate() copies this struct into
// those globals and the previously-active pane's globals back out.
//
// The three BIG buffers are pointers, so a switch re-points them instead of
// copying 1.07 MB. That is the whole reason a per-tick pane switch is free.
typedef struct {
    // --- term_grid.c ---
    int      term_cols, term_rows;
    int      term_px_w, term_px_h;
    int      term_origin_x, term_origin_y;
    int      cursor_x, cursor_y;
    bool     cursor_visible;
    // The rendition pen, replacing the old current_fg/current_bg/
    // current_reverse triple (see term_grid.h).
    term_sgr_t g_pen;
    int      scroll_top, scroll_bottom;
    int      saved_cursor_x, saved_cursor_y;
    term_sgr_t g_saved_pen;
    int      term_autowrap;
    int      in_alt_screen;
    int      alt_saved_cursor_x, alt_saved_cursor_y;
    int      term_clear_calls;
    // --- term_scrollback.c ---
    // sb_capacity is banked WITH sb_count/sb_head, and it has to be. The ring
    // depth became a PROFILE SETTING (term_profile.h), so two panes can hold
    // rings of different lengths: one allocated before the user changed the
    // setting and one after. sb_row() indexes with `% sb_capacity`, so a global
    // capacity paired with a per-pane pointer would index PAST THE END of the
    // shorter ring on the next pane switch. Three ints describe one ring; bank
    // all three or none.
    int          sb_count, sb_head, sb_capacity;
    gui_scroll_t term_scroll_view;
    int          term_at_bottom;
    // --- term_parse.c / term_emu.c ---
    // The whole VT500 parser, not four ints: a half-received escape sequence or
    // a half-received UTF-8 character belongs to the pane whose child sent it,
    // and must not be completed by the NEXT pane's output. Banking only part of
    // the parser state would corrupt exactly the sequences that straddle a pane
    // switch, which is both rare and impossible to reproduce on demand.
    term_parser_t g_parser;
    // The DEC private modes are per-pane too: mouse reporting enabled by the
    // program in one pane must not make the pane next to it report clicks.
    term_modes_t  g_term_modes;
    uint8_t       g_tabstop[TERM_MAX_COLS];
    // --- term_pty.c ---
    int      g_active_master_fd;
    // --- term_shell.c ---
    char     input_line[TERM_SHELL_INPUT_MAX];
    int      input_pos;
    int      history_pos;
    char     cwd[256];
    // --- term_render.c (#damage) ---
    // The generation this pane's last-painted shadow is valid at, and the
    // scrollbar state as it was last painted for this pane. Banked for the
    // same reason everything else here is: a damage record applied to the
    // wrong pane's grid would paint one pane's changes into another's pixels.
    // A fresh pane banks 0, which is stale by construction (the live counter
    // starts at 1), so a pane's first draw is always a full repaint without
    // needing a separate "never drawn" flag.
    unsigned     term_shadow_gen;
    gui_scroll_t term_sb_painted;
    int          term_sb_painted_valid;
} tl_banked_t;

typedef struct {
    int  used;
    tl_banked_t st;
    // Bulk buffers, switched by pointer (see term_layout.h).
    term_cell_t (*cells)[TERM_MAX_COLS];
    term_cell_t (*alt)[TERM_MAX_COLS];
    term_cell_t  *sb;
    // (#damage) This pane's last-painted cell descriptors. A pointer, switched
    // like the three above rather than copied: it is the same 141 KB. NULL is
    // a legal value and means "this pane repaints in full every frame", which
    // is exactly the pre-damage-tracking renderer.
    term_cell_desc_t (*shadow)[TERM_MAX_COLS];
    // The pty of the ONE FOREGROUND COMMAND currently running in this pane, or
    // -1/0 when none is. This is NOT a persistent session: a pty is created per
    // command and destroyed when that command exits (measured; ptmx_open()
    // always allocates a fresh pair and a master cannot be reopened). Between
    // commands the pane has no child and no tty, and its prompt is drawn by
    // term_shell.c in-process. See the note in term_layout.h.
    int  master;
    int  pid;
    // The PANE header's title. It follows the running program (Konsole's
    // default) and reverts to the idle label when the child exits. A user
    // rename is NOT stored here: renaming names a TAB (see tl_tab_t), and
    // there is no pane-rename control in this phase.
    char progname[TL_TITLE_MAX];
    // Last computed on-screen rect of the WHOLE pane (header + grid), in
    // window-local coords. Written only by tl_layout_node().
    int  rx, ry, rw, rh;
    int  visible;          // 0 while another pane in this tab is maximized
    // Which tab this pane belongs to. term_notify.[ch] has been keyed by tab
    // index since it landed, precisely so that tabs arriving would be a WIRING
    // change and not a rewrite; it was being called with TERM_TAB_DEFAULT only
    // because there was one tab. This field is what lets the calls below name
    // the RIGHT tab, which is the whole point of "notify me about the tab I am
    // not looking at".
    int  tab;
} tl_pane_t;

// A node in the tab's binary split tree. Nodes are pool-allocated: this app
// links one RWX PT_LOAD and a malloc'd 40-byte node buys nothing over a
// fixed pool that is bounded by TL_MAX_PANES anyway.
typedef struct {
    int  used;
    int  is_leaf;
    int  pane;             // leaf: index into g_panes
    int  orient;           // internal: TL_SPLIT_*
    int  ratio;            // internal: permille of the space given to kid[0].
                           // INTEGER permille, not the spec's `float ratio`:
                           // exact, no rounding drift across repeated divider
                           // drags, and no float in a size calculation.
    int  kid[2];           // internal: node indices
    int  parent;
    // Last computed rect, window-local. Written only by tl_layout_node().
    int  rx, ry, rw, rh;
    int  dx, dy, dw, dh;   // internal: the divider's own rect
} tl_node_t;

typedef struct {
    int  used;
    int  root;             // node index
    int  focus;            // pane index that has keyboard focus in this tab
    int  maximized;        // pane index shown alone, or -1
    // A user rename belongs to the TAB, not to a pane. MEASURED on golden
    // 2047: with the rename stored on the focused PANE, renaming a tab and
    // then splitting it moved focus to the new pane and the strip silently
    // reverted to "shell" - the label the user typed vanished on an unrelated
    // action. A tab is the thing being named; its panes come and go under it.
    char title[TL_TITLE_MAX];
    int  renamed;
} tl_tab_t;

static tl_pane_t g_panes[TL_MAX_PANES];
static tl_node_t g_nodes[TL_MAX_NODES];
static tl_tab_t  g_tabs[TL_MAX_TABS];

static int g_tab_count = 0;
static int g_tab_cur   = 0;
static int g_active    = -1;      // pane index whose state is IN the globals
static int g_ready     = 0;

static int g_content_w = 0, g_content_h = 0;

// Tab strip scroll offset in pixels, for the too-many-tabs case (spec 2.5).
static int g_tab_scroll = 0;
// Compact mode: the window is too narrow for a real tab strip.
static int g_tab_compact = 0;

// Mouse interaction state.
static int g_drag_div   = -1;     // node index whose divider is being dragged
static int g_drag_tab   = -1;     // tab index being dragged for reorder
static int g_drag_tab_x = 0;
static int g_rename_tab = -1;     // tab index in inline-rename mode
static int g_rename_len = 0;
static char g_rename_buf[TL_TITLE_MAX];
static unsigned long g_last_tab_click_ms = 0;
static int g_last_tab_click_idx = -1;

static unsigned long g_blink_ms = 0;

// ===========================================================================
// 2. SMALL HELPERS
// ===========================================================================
// Forward declarations for the two directions this file legitimately calls in
// (tab bookkeeping needs the strip geometry; the strip needs the tab list).
static void tl_tab_reveal(int idx);
static void tl_tab_scroll_clamp(void);

static int tl_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void tl_basename(const char *path, char *out, int max) {
    int last = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
    const char *b = path + last + 1;
    int i = 0;
    while (b[i] && i < max - 1) { out[i] = b[i]; i++; }
    out[i] = '\0';
}

// Chrome text is the kernel's fixed 8x16 face (win_draw_text), so a column is
// exactly 8px wide. One place says so.
#define TL_CHW 8
#define TL_CHH 16

static void tl_text_fit(int x, int y, const char *s, int maxpx, uint32_t col) {
    int fit = maxpx / TL_CHW;
    if (fit <= 0) return;
    char buf[64];
    if (fit > (int)sizeof(buf) - 1) fit = (int)sizeof(buf) - 1;
    int n = tl_len(s);
    if (n <= fit) {
        win_draw_text(window_handle, x, y, s, col);
        return;
    }
    if (fit <= 3) { for (int i = 0; i < fit; i++) buf[i] = '.'; buf[fit] = '\0'; }
    else {
        for (int i = 0; i < fit - 3; i++) buf[i] = s[i];
        buf[fit - 3] = '.'; buf[fit - 2] = '.'; buf[fit - 1] = '.'; buf[fit] = '\0';
    }
    win_draw_text(window_handle, x, y, buf, col);
}

static uint32_t tl_c(int token) { return theme_color_of(g_term_theme_index, token); }

// (#307) FINISH A FRAME. Window-level chrome (the menu bar, and any open menu
// popup) goes on TOP of whatever was just painted, and only then is the window
// presented. EVERY path in this file that used to call win_invalidate()
// directly calls this instead.
//
// WHY THIS IS A CHOKEPOINT AND NOT A CALL AT THE END OF term_layout_redraw_all().
// MEASURED on golden 2045 with the menu bar landed: term_layout_idle() repaints
// the focused pane with tl_draw_pane() on the 500 ms cursor blink and presents
// directly, so an open menu popup was erased within half a second of being
// opened and the menu looked like it simply did not work. Six other paths
// (tab switch, divider drag, pane focus, header buttons, selection drag,
// scrollbar drag) present the same way. One chokepoint, not seven call sites
// to remember.
static void tl_present(void) {
    // BOTH chrome modules, bottom-most first. #221's find bar draws its strip;
    // #307's hooks draw the menu bar and any open popup, and go LAST because a
    // popup can overlay the find bar. Putting term_search_overlay() here rather
    // than beside two of the seven present sites is the same fix for the same
    // bug: the 500 ms cursor blink in term_layout_idle() repaints a pane and
    // presents, which erased whichever overlay was not redrawn on that path.
    term_search_overlay();
    term_render_draw_chrome();
    win_invalidate(window_handle);
}

static void tl_frame(int x, int y, int w, int h, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    win_draw_rect(window_handle, x, y, w, 1, col);
    win_draw_rect(window_handle, x, y + h - 1, w, 1, col);
    win_draw_rect(window_handle, x, y, 1, h, col);
    win_draw_rect(window_handle, x + w - 1, y, 1, h, col);
}

static int tl_in(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

// ===========================================================================
// 3. BANKING: the ONE place a pane's state enters or leaves the modules
// ===========================================================================
static void tl_bank_out(tl_pane_t *p) {
    tl_banked_t *s = &p->st;
    s->term_cols = term_cols; s->term_rows = term_rows;
    s->term_px_w = term_px_w; s->term_px_h = term_px_h;
    s->term_origin_x = term_origin_x; s->term_origin_y = term_origin_y;
    s->cursor_x = cursor_x; s->cursor_y = cursor_y;
    s->cursor_visible = cursor_visible;
    s->g_pen = g_pen;
    s->scroll_top = scroll_top; s->scroll_bottom = scroll_bottom;
    s->saved_cursor_x = saved_cursor_x; s->saved_cursor_y = saved_cursor_y;
    s->g_saved_pen = g_saved_pen;
    s->term_autowrap = term_autowrap;
    s->in_alt_screen = in_alt_screen;
    s->alt_saved_cursor_x = alt_saved_cursor_x;
    s->alt_saved_cursor_y = alt_saved_cursor_y;
    s->term_clear_calls = term_clear_calls;
    s->sb_count = sb_count; s->sb_head = sb_head; s->sb_capacity = sb_capacity;
    s->term_scroll_view = term_scroll_view;
    s->term_at_bottom = term_at_bottom;
    s->g_parser = g_parser;
    s->g_term_modes = g_term_modes;
    for (int i = 0; i < TERM_MAX_COLS; i++) s->g_tabstop[i] = g_tabstop[i];
    s->g_active_master_fd = g_active_master_fd;
    for (int i = 0; i < TERM_SHELL_INPUT_MAX; i++) s->input_line[i] = input_line[i];
    s->input_pos = input_pos;
    s->history_pos = history_pos;
    for (int i = 0; i < 256; i++) s->cwd[i] = cwd[i];
    p->cells = cells; p->alt = alt_saved_cells; p->sb = sb_lines;
    s->term_shadow_gen = term_shadow_gen;
    s->term_sb_painted = term_sb_painted;
    s->term_sb_painted_valid = term_sb_painted_valid;
    p->shadow = term_shadow;
}

static void tl_bank_in(tl_pane_t *p) {
    tl_banked_t *s = &p->st;
    cells = p->cells; alt_saved_cells = p->alt; sb_lines = p->sb;
    term_shadow = p->shadow;
    term_shadow_gen = s->term_shadow_gen;
    term_sb_painted = s->term_sb_painted;
    term_sb_painted_valid = s->term_sb_painted_valid;
    term_cols = s->term_cols; term_rows = s->term_rows;
    term_px_w = s->term_px_w; term_px_h = s->term_px_h;
    term_origin_x = s->term_origin_x; term_origin_y = s->term_origin_y;
    cursor_x = s->cursor_x; cursor_y = s->cursor_y;
    cursor_visible = s->cursor_visible;
    g_pen = s->g_pen;
    scroll_top = s->scroll_top; scroll_bottom = s->scroll_bottom;
    saved_cursor_x = s->saved_cursor_x; saved_cursor_y = s->saved_cursor_y;
    g_saved_pen = s->g_saved_pen;
    term_autowrap = s->term_autowrap;
    in_alt_screen = s->in_alt_screen;
    alt_saved_cursor_x = s->alt_saved_cursor_x;
    alt_saved_cursor_y = s->alt_saved_cursor_y;
    term_clear_calls = s->term_clear_calls;
    sb_count = s->sb_count; sb_head = s->sb_head; sb_capacity = s->sb_capacity;
    term_scroll_view = s->term_scroll_view;
    term_at_bottom = s->term_at_bottom;
    g_parser = s->g_parser;
    g_term_modes = s->g_term_modes;
    for (int i = 0; i < TERM_MAX_COLS; i++) g_tabstop[i] = s->g_tabstop[i];
    g_active_master_fd = s->g_active_master_fd;
    for (int i = 0; i < TERM_SHELL_INPUT_MAX; i++) input_line[i] = s->input_line[i];
    input_pos = s->input_pos;
    history_pos = s->history_pos;
    for (int i = 0; i < 256; i++) cwd[i] = s->cwd[i];
}

// Make `pi` the pane whose state the modules see. Every function in this file
// that touches a pane other than the current one goes through here first;
// nothing else may write the module globals.
static void tl_activate(int pi) {
    if (pi < 0 || pi >= TL_MAX_PANES || !g_panes[pi].used) return;
    // #221: THE one place that says "this pane's grid is now the current
    // one", which is exactly the fact a selection needs in order to belong to
    // ONE pane. Set before the already-active early return, so it is correct
    // even on the no-op path.
    term_select_note_pane(pi);
    term_search_note_pane(pi);   // #221: a find owns ONE pane, same rule
    term_notify_note_pane(pi);   // #221: the visual bell frames ONE pane too

    if (g_active == pi) return;
    if (g_active >= 0 && g_panes[g_active].used) tl_bank_out(&g_panes[g_active]);
    g_active = pi;
    tl_bank_in(&g_panes[pi]);
}

static tl_tab_t *tl_tab(void) { return &g_tabs[g_tab_cur]; }
static int tl_focus(void)     { return g_tabs[g_tab_cur].focus; }

// THE focus change. Four call sites used to do these lines themselves and one
// of them would eventually forget one.
//
// It used to end with `cursor_visible = true`, to undo the `cursor_visible =
// false` tl_draw_pane() wrote for an unfocused pane. Both are gone: the view
// decision is term_cursor_suppressed now, so there is nothing to undo, and
// forcing the flag true here was ALSO overwriting a DECTCEM hide the pane's
// own program had asked for (measured; see term_grid.h). Focus decides which
// pane the cursor is drawn IN. Whether that pane wants a cursor at all is the
// program's call, and only the program's.
static void tl_set_focus(int pi) {
    if (pi < 0) return;
    g_tabs[g_tab_cur].focus = pi;
    tl_activate(pi);
}

// ===========================================================================
// 4. THE TREE
// ===========================================================================
static int tl_node_new(void) {
    for (int i = 0; i < TL_MAX_NODES; i++)
        if (!g_nodes[i].used) {
            tl_node_t *n = &g_nodes[i];
            for (unsigned long k = 0; k < sizeof(*n); k++) ((char *)n)[k] = 0;
            n->used = 1; n->parent = -1; n->kid[0] = -1; n->kid[1] = -1;
            n->pane = -1; n->ratio = 500;
            return i;
        }
    return -1;
}

static int tl_pane_count_in(int node) {
    if (node < 0 || !g_nodes[node].used) return 0;
    if (g_nodes[node].is_leaf) return 1;
    return tl_pane_count_in(g_nodes[node].kid[0]) + tl_pane_count_in(g_nodes[node].kid[1]);
}

// Collect the leaf pane indices of a subtree, left-to-right / top-to-bottom.
static void tl_collect(int node, int *out, int *n, int max) {
    if (node < 0 || !g_nodes[node].used || *n >= max) return;
    if (g_nodes[node].is_leaf) { out[(*n)++] = g_nodes[node].pane; return; }
    tl_collect(g_nodes[node].kid[0], out, n, max);
    tl_collect(g_nodes[node].kid[1], out, n, max);
}

static int tl_leaf_of_pane(int node, int pane) {
    if (node < 0 || !g_nodes[node].used) return -1;
    if (g_nodes[node].is_leaf) return g_nodes[node].pane == pane ? node : -1;
    int r = tl_leaf_of_pane(g_nodes[node].kid[0], pane);
    if (r >= 0) return r;
    return tl_leaf_of_pane(g_nodes[node].kid[1], pane);
}

// ===========================================================================
// 5. GEOMETRY - THE ONE FUNCTION (#220), AND THE ONE SIGWINCH (#227)
// ===========================================================================
// Give pane `pi` the content rect (gx,gy,gw,gh) - the GRID area, header
// already subtracted. Nothing else in this file computes a grid size, and
// term_scrollback_reconfigure() remains the only caller of gui_scroll_config()
// in the app, so creation and resize cannot drift apart for any pane.
static void tl_pane_apply_geometry(int pi, int gx, int gy, int gw, int gh) {
    tl_activate(pi);
    term_origin_x = gx;
    term_origin_y = gy;
    if (gw < TERM_CHAR_W) gw = TERM_CHAR_W;
    if (gh < TERM_CHAR_H) gh = TERM_CHAR_H;
    term_handle_resize(gw, gh);
    term_scrollback_reconfigure();
    term_scroll_sync_bottom();
    // #227: this pane changed size, so this pane's child gets SIGWINCH. ONE
    // winsize filler exists (term_pty_set_winsize); a -1 master is a no-op.
    term_pty_set_winsize(g_panes[pi].master, term_rows, term_cols);
}

static void tl_layout_node(int node, int x, int y, int w, int h, int show_headers);

static void tl_layout_leaf(int node, int x, int y, int w, int h, int show_headers) {
    tl_node_t *n = &g_nodes[node];
    int pi = n->pane;
    n->rx = x; n->ry = y; n->rw = w; n->rh = h;
    g_panes[pi].rx = x; g_panes[pi].ry = y; g_panes[pi].rw = w; g_panes[pi].rh = h;
    g_panes[pi].visible = 1;
    int hdr = show_headers ? TL_HDR_H : 0;
    tl_pane_apply_geometry(pi, x, y + hdr, w, h - hdr);
}

static void tl_layout_node(int node, int x, int y, int w, int h, int show_headers) {
    if (node < 0 || !g_nodes[node].used) return;
    tl_node_t *n = &g_nodes[node];
    n->rx = x; n->ry = y; n->rw = w; n->rh = h;
    if (n->is_leaf) { tl_layout_leaf(node, x, y, w, h, show_headers); return; }
    if (n->orient == TL_SPLIT_VERTICAL) {
        int avail = w - TL_DIV; if (avail < 2) avail = 2;
        int w0 = avail * n->ratio / 1000;
        if (w0 < 1) w0 = 1;
        if (w0 > avail - 1) w0 = avail - 1;
        n->dx = x + w0; n->dy = y; n->dw = TL_DIV; n->dh = h;
        tl_layout_node(n->kid[0], x, y, w0, h, show_headers);
        tl_layout_node(n->kid[1], x + w0 + TL_DIV, y, avail - w0, h, show_headers);
    } else {
        int avail = h - TL_DIV; if (avail < 2) avail = 2;
        int h0 = avail * n->ratio / 1000;
        if (h0 < 1) h0 = 1;
        if (h0 > avail - 1) h0 = avail - 1;
        n->dx = x; n->dy = y + h0; n->dw = w; n->dh = TL_DIV;
        tl_layout_node(n->kid[0], x, y, w, h0, show_headers);
        tl_layout_node(n->kid[1], x, y + h0 + TL_DIV, w, avail - h0, show_headers);
    }
}

static int tl_strip_h(void) {
    if (g_tab_count <= 1) return 0;      // spec 2.2 auto-hide: one tab, no strip
    return TL_TAB_H;
}

// Recompute EVERY pane of the CURRENT tab. Panes in other tabs keep the
// geometry they had; they are re-laid-out when that tab is switched to, which
// is the same single function, so a background tab can never be handed a
// different geometry rule from a foreground one.
static void tl_relayout(void) {
    if (!g_ready) return;
    g_tab_compact = (g_content_w < 260);
    // (#307 PHASE 1, menu bar) The tab strip and every pane start BELOW the
    // window-level chrome band. term_content_y is the ONE variable that says
    // how tall that band is (term_grid.h); adding it HERE, in the single
    // geometry function, is what makes every pane's origin already include it
    // so nothing downstream adds it a second time.
    int sy = tl_strip_h();
    int cy = term_content_y + sy;
    int ch = g_content_h - cy;
    if (ch < 1) ch = 1;
    tl_tab_t *t = tl_tab();

    // Mark EVERY pane in EVERY tab invisible first, not just this tab's. A
    // background tab's panes keep the rect they had when that tab was last on
    // screen, and the pty pump repaints any pane it thinks is visible, so a
    // background pane left flagged visible would paint its output over the
    // foreground tab. `visible` means "on screen right now", and only the walk
    // below is allowed to assert it.
    for (int i = 0; i < TL_MAX_PANES; i++) g_panes[i].visible = 0;
    int list[TL_MAX_PANES], n = 0;
    tl_collect(t->root, list, &n, TL_MAX_PANES);

    if (t->maximized >= 0 && g_panes[t->maximized].used) {
        int leaf = tl_leaf_of_pane(t->root, t->maximized);
        if (leaf >= 0) {
            tl_layout_leaf(leaf, 0, cy, g_content_w, ch, 1);
            tl_activate(t->focus);
            return;
        }
        t->maximized = -1;
    }
    tl_layout_node(t->root, 0, cy, g_content_w, ch, n > 1);
    // Focus must end up on the pane whose state the modules hold, so the
    // built-in line editor in main.c edits the pane the user is looking at.
    tl_activate(t->focus);
}

// ===========================================================================
// 6. PANE LIFECYCLE
// ===========================================================================
static int tl_pane_new(const char *inherit_cwd) {
    int pi = -1;
    for (int i = 0; i < TL_MAX_PANES; i++) if (!g_panes[i].used) { pi = i; break; }
    if (pi < 0) return -1;
    tl_pane_t *p = &g_panes[pi];
    for (unsigned long k = 0; k < sizeof(*p); k++) ((char *)p)[k] = 0;

    p->cells = (term_cell_t (*)[TERM_MAX_COLS])
               malloc((size_t)TERM_MAX_ROWS * TERM_MAX_COLS * sizeof(term_cell_t));
    p->alt   = (term_cell_t (*)[TERM_MAX_COLS])
               malloc((size_t)TERM_MAX_ROWS * TERM_MAX_COLS * sizeof(term_cell_t));
    if (!p->cells || !p->alt) {
        if (p->cells) free(p->cells);
        if (p->alt) free(p->alt);
        p->cells = 0; p->alt = 0;
        return -1;
    }
    // (#damage) NOT fatal if it fails, and deliberately not grouped with the
    // check above. A pane with no shadow is a pane that repaints in full every
    // frame - slower, and exactly what every pane did before damage tracking
    // existed. Refusing to open a split because 141 KB of an OPTIMISATION
    // could not be allocated would be a worse terminal, not a safer one.
    p->shadow = (term_cell_desc_t (*)[TERM_MAX_COLS])
                malloc((size_t)TERM_MAX_ROWS * TERM_MAX_COLS * sizeof(term_cell_desc_t));
    // Scrollback depth comes from the ACTIVE PROFILE (sb_want), not the fixed
    // SCROLLBACK_LINES default: a user who set 5000 lines and then opened a
    // split would otherwise get 2000 in the new pane and no indication why.
    // A failed allocation is NOT fatal: sb_lines == NULL is already "scrollback
    // disabled" at every entry point in term_scrollback.c, so the pane still
    // works, it just has no history - and then its banked capacity must say so,
    // or sb_row() would index a NULL ring.
    p->sb = (term_cell_t *)malloc((size_t)sb_want * TERM_MAX_COLS * sizeof(term_cell_t));
    p->st.sb_capacity = p->sb ? sb_want : SCROLLBACK_LINES;

    p->used = 1;
    p->master = -1;
    p->pid = 0;
    p->visible = 1;
    p->tab = g_tab_cur;          // corrected by the caller for a brand-new tab
    // "shell" is the IDLE title, and it names the in-process line editor, not a
    // process: there is no shell process to name. While a command runs, the
    // title is that command's basename, which is the Konsole default and the
    // one the owner's reference screenshots show.
    str_copy(p->progname, "shell", TL_TITLE_MAX);

    tl_banked_t *s = &p->st;
    s->term_cols = TERM_INIT_COLS; s->term_rows = TERM_INIT_ROWS;
    s->term_px_w = TERM_INIT_COLS * TERM_CHAR_W;
    s->term_px_h = TERM_INIT_ROWS * TERM_CHAR_H;
    s->cursor_visible = true;
    term_emu_sgr_reset(&s->g_pen);
    term_emu_sgr_reset(&s->g_saved_pen);
    term_emu_reset(&s->g_parser);
    s->g_term_modes.mouse_mode = 0;
    s->g_term_modes.mouse_sgr = 0;
    s->g_term_modes.bracketed_paste = 0;
    s->g_term_modes.focus_events = 0;
    // A new pane starts with the DEFAULT tab stops, from the one definition
    // of what those are (term_grid.c), not a second copy of the loop.
    term_tabstops_default(s->g_tabstop);
    s->scroll_top = 0; s->scroll_bottom = -1;
    s->term_autowrap = 1;
    s->term_at_bottom = 1;
    s->g_active_master_fd = -1;
    str_copy(s->cwd, inherit_cwd && inherit_cwd[0] ? inherit_cwd : "/", 256);

    // Blank the grid through the real engine so a new pane cannot acquire a
    // second, subtly different "what a blank grid looks like".
    int prev = g_active;
    tl_activate(pi);
    term_clear();
    print_prompt();
    if (prev >= 0) tl_activate(prev);
    return pi;
}

static void tl_pane_free(int pi) {
    tl_pane_t *p = &g_panes[pi];
    if (!p->used) return;
    if (p->master >= 0) {
        // Closing the master hangs the child up (the kernel tty layer raises
        // SIGHUP at fg_pgrp), which is the same thing closing the window does
        // today.
        close(p->master);
        p->master = -1;
    }
    if (p->cells) free(p->cells);
    if (p->alt) free(p->alt);
    if (p->sb) free(p->sb);
    if (p->shadow) free(p->shadow);
    p->cells = 0; p->alt = 0; p->sb = 0; p->shadow = 0;
    p->used = 0;
    if (g_active == pi) g_active = -1;
}

// The PANE HEADER's label: always the pane's own program.
static void tl_pane_title(int pi, char *out, int max) {
    str_copy(out, g_panes[pi].progname, max);
}

// The TAB STRIP's label: the user's name for the tab if they gave it one,
// otherwise the program running in the tab's focused pane, which is the
// Konsole default and what the owner's reference screenshots show.
static void tl_tab_title(int ti, char *out, int max) {
    if (g_tabs[ti].renamed) { str_copy(out, g_tabs[ti].title, max); return; }
    int pi = g_tabs[ti].focus;
    if (pi < 0 || !g_panes[pi].used) { str_copy(out, "shell", max); return; }
    str_copy(out, g_panes[pi].progname, max);
}

// ===========================================================================
// 7. TABS
// ===========================================================================
static int tl_tab_new(void) {
    if (g_tab_count >= TL_MAX_TABS) return -1;
    int ti = -1;
    for (int i = 0; i < TL_MAX_TABS; i++) if (!g_tabs[i].used) { ti = i; break; }
    if (ti < 0) return -1;
    const char *inherit = (g_active >= 0) ? g_panes[g_active].st.cwd : cwd;
    int pi = tl_pane_new(inherit);
    if (pi < 0) return -1;
    int nd = tl_node_new();
    if (nd < 0) { tl_pane_free(pi); return -1; }
    g_nodes[nd].is_leaf = 1;
    g_nodes[nd].pane = pi;
    g_tabs[ti].used = 1;
    g_tabs[ti].root = nd;
    g_tabs[ti].focus = pi;
    g_tabs[ti].maximized = -1;
    // A slot is REUSED after a close, and the struct is a static array, so the
    // previous occupant's name is still sitting in it. Clear it here and in the
    // move-to-new-tab path: a brand-new tab wearing a closed tab's name is the
    // kind of thing that only shows up on the sixth tab of a long session.
    g_tabs[ti].renamed = 0;
    g_tabs[ti].title[0] = '\0';
    g_panes[pi].tab = ti;
    g_tab_count++;
    return ti;
}

// Tabs are addressed by their SLOT, and the strip draws them in slot order,
// so reordering is a slot swap and nothing else has to know.
// Rewrite every pane's tab back-pointer from the tree it actually hangs off.
// Called after ANY move of a tab between slots (reorder, close-compaction).
// Doing it by re-derivation rather than by hand-patching indices is deliberate:
// a hand-patched back-pointer is exactly the thing that goes stale silently and
// then names the wrong tab in a notification for the rest of the session.
static void tl_reindex_tabs(void) {
    for (int t = 0; t < TL_MAX_TABS; t++) {
        if (!g_tabs[t].used) continue;
        int list[TL_MAX_PANES], n = 0;
        tl_collect(g_tabs[t].root, list, &n, TL_MAX_PANES);
        for (int i = 0; i < n; i++) g_panes[list[i]].tab = t;
    }
}

static void tl_tab_swap(int a, int b) {
    if (a < 0 || b < 0 || a >= TL_MAX_TABS || b >= TL_MAX_TABS) return;
    tl_tab_t tmp = g_tabs[a]; g_tabs[a] = g_tabs[b]; g_tabs[b] = tmp;
    tl_reindex_tabs();
}

static void tl_free_subtree(int node) {
    if (node < 0 || !g_nodes[node].used) return;
    if (g_nodes[node].is_leaf) tl_pane_free(g_nodes[node].pane);
    else { tl_free_subtree(g_nodes[node].kid[0]); tl_free_subtree(g_nodes[node].kid[1]); }
    g_nodes[node].used = 0;
}

static void tl_switch_tab(int ti) {
    if (ti < 0 || ti >= TL_MAX_TABS || !g_tabs[ti].used) return;
    g_tab_cur = ti;
    // The focused tab does not notify; every other one does. term_notify.c owns
    // that rule, it just has to be told which tab is on screen.
    term_notify_set_focused_tab(ti);
    term_notify_tab_clear(ti);
    tl_tab_reveal(ti);
    tl_set_focus(g_tabs[ti].focus);
    tl_relayout();
    term_layout_redraw_all();
}

// Returns 1 if the LAST tab was closed (the application should exit).
static int tl_tab_close(int ti) {
    if (!g_tabs[ti].used) return 0;
    if (g_tab_count == 1) return 1;
    // Bank the active pane out BEFORE anything is freed. Closing a tab that is
    // NOT the current one must not lose the current pane's state, and closing
    // the current one must not bank state back into freed memory.
    if (g_active >= 0 && g_panes[g_active].used) tl_bank_out(&g_panes[g_active]);
    g_active = -1;
    tl_free_subtree(g_tabs[ti].root);
    g_tabs[ti].used = 0;
    g_tab_count--;
    // Compact the slot array so the strip has no holes.
    for (int i = ti; i < TL_MAX_TABS - 1; i++) g_tabs[i] = g_tabs[i + 1];
    g_tabs[TL_MAX_TABS - 1].used = 0;
    tl_reindex_tabs();
    if (g_tab_cur >= g_tab_count) g_tab_cur = g_tab_count - 1;
    if (g_tab_cur < 0) g_tab_cur = 0;
    tl_set_focus(g_tabs[g_tab_cur].focus);
    tl_relayout();
    term_layout_redraw_all();
    return 0;
}

// ===========================================================================
// 8. SPLITS
// ===========================================================================
// Would splitting `leaf` along `orient` leave either child under the minimum
// pane size (spec 2.4: 20 cols x 5 rows)? The refusal happens BEFORE any tree
// edit, so a refused split leaves nothing half-done.
static int tl_split_fits(int leaf, int orient) {
    tl_node_t *n = &g_nodes[leaf];
    int minw = TL_MIN_COLS * TERM_CHAR_W + GUI_SCROLL_W;
    int minh = TL_MIN_ROWS * TERM_CHAR_H + TL_HDR_H;
    if (orient == TL_SPLIT_VERTICAL) return (n->rw - TL_DIV) / 2 >= minw;
    return (n->rh - TL_DIV) / 2 >= minh;
}

static void tl_split(int orient) {
    tl_tab_t *t = tl_tab();
    if (t->maximized >= 0) return;                  // un-maximize first
    int leaf = tl_leaf_of_pane(t->root, t->focus);
    if (leaf < 0) return;
    int list[TL_MAX_PANES], n = 0;
    tl_collect(t->root, list, &n, TL_MAX_PANES);
    if (n >= TL_MAX_PANES) return;
    if (!tl_split_fits(leaf, orient)) return;

    int newpane = tl_pane_new(g_panes[t->focus].st.cwd);
    if (newpane < 0) return;
    int newleaf = tl_node_new();
    int inner   = tl_node_new();
    if (newleaf < 0 || inner < 0) {
        if (newleaf >= 0) g_nodes[newleaf].used = 0;
        if (inner   >= 0) g_nodes[inner].used = 0;
        tl_pane_free(newpane);
        return;
    }
    g_nodes[newleaf].is_leaf = 1;
    g_nodes[newleaf].pane = newpane;

    // Replace `leaf` in its parent with `inner`, whose children are the old
    // leaf and the new one. This is the whole add-a-split model (spec 2.3).
    int parent = g_nodes[leaf].parent;
    g_nodes[inner].is_leaf = 0;
    g_nodes[inner].orient = orient;
    g_nodes[inner].ratio = 500;
    g_nodes[inner].kid[0] = leaf;
    g_nodes[inner].kid[1] = newleaf;
    g_nodes[inner].parent = parent;
    g_nodes[leaf].parent = inner;
    g_nodes[newleaf].parent = inner;
    if (parent < 0) t->root = inner;
    else g_nodes[parent].kid[g_nodes[parent].kid[0] == leaf ? 0 : 1] = inner;

    tl_set_focus(newpane);
    tl_relayout();
    term_layout_redraw_all();
}

// Remove one leaf: its PARENT is replaced by the leaf's sibling, one level up
// (spec 2.3). The inverse of tl_split(), and the only remove path.
static void tl_close_split(int pane) {
    tl_tab_t *t = tl_tab();
    int leaf = tl_leaf_of_pane(t->root, pane);
    if (leaf < 0) return;
    int parent = g_nodes[leaf].parent;
    if (parent < 0) return;                 // last pane in the tab: use Close Tab
    int sib = g_nodes[parent].kid[0] == leaf ? g_nodes[parent].kid[1]
                                             : g_nodes[parent].kid[0];
    int gp = g_nodes[parent].parent;
    g_nodes[sib].parent = gp;
    if (gp < 0) t->root = sib;
    else g_nodes[gp].kid[g_nodes[gp].kid[0] == parent ? 0 : 1] = sib;
    g_nodes[parent].used = 0;
    g_nodes[leaf].used = 0;
    if (t->maximized == pane) t->maximized = -1;
    // Same rule as tl_tab_close(): bank out before the free.
    if (g_active >= 0 && g_panes[g_active].used) tl_bank_out(&g_panes[g_active]);
    g_active = -1;
    tl_pane_free(pane);
    // Focus the first surviving leaf of the sibling subtree.
    int list[TL_MAX_PANES], n = 0;
    tl_collect(sib, list, &n, TL_MAX_PANES);
    tl_set_focus(n > 0 ? list[0] : -1);
    tl_relayout();
    term_layout_redraw_all();
}

// "Move this split to a new tab" (spec 2.4): a genuine tree edit. The pane
// keeps its grid, scrollback and live child; only its position in the tree
// changes, which is exactly what the tree model makes cheap.
static void tl_pane_to_new_tab(int pane) {
    if (g_tab_count >= TL_MAX_TABS) return;
    tl_tab_t *t = tl_tab();
    int leaf = tl_leaf_of_pane(t->root, pane);
    if (leaf < 0 || g_nodes[leaf].parent < 0) return;   // sole pane: nothing to move
    int parent = g_nodes[leaf].parent;
    int sib = g_nodes[parent].kid[0] == leaf ? g_nodes[parent].kid[1]
                                             : g_nodes[parent].kid[0];
    int gp = g_nodes[parent].parent;
    g_nodes[sib].parent = gp;
    if (gp < 0) t->root = sib;
    else g_nodes[gp].kid[g_nodes[gp].kid[0] == parent ? 0 : 1] = sib;
    g_nodes[parent].used = 0;
    if (t->maximized == pane) t->maximized = -1;
    int list[TL_MAX_PANES], n = 0;
    tl_collect(sib, list, &n, TL_MAX_PANES);
    tl_set_focus(n > 0 ? list[0] : -1);

    int ti = -1;
    for (int i = 0; i < TL_MAX_TABS; i++) if (!g_tabs[i].used) { ti = i; break; }
    if (ti < 0) return;
    g_nodes[leaf].parent = -1;
    g_tabs[ti].used = 1;
    g_tabs[ti].root = leaf;
    g_tabs[ti].focus = pane;
    g_tabs[ti].maximized = -1;
    g_tabs[ti].renamed = 0;
    g_tabs[ti].title[0] = '\0';
    g_panes[pane].tab = ti;
    g_tab_count++;
    tl_switch_tab(ti);
}

// ===========================================================================
// 9. THE PTY, ASYNCHRONOUSLY
// ===========================================================================
// term_pty.c's run_foreground_pty() used to own a NESTED event loop that ran
// until the child exited. That could not survive a second pane: while pane A
// ran vi, nothing else in the window could draw. It is gone. There is now one
// event loop (main.c), and a child is started here and pumped from
// term_layout_idle().
void term_layout_run_foreground(const char *path, char **argv, int argc) {
    int pi = tl_focus();
    if (pi < 0) return;
    tl_activate(pi);
    int master = -1;
    int pid = term_pty_start(path, argv, argc, &master);
    if (pid <= 0) return;                 // term_pty_start already reported it
    g_panes[pi].master = master;
    g_panes[pi].pid = pid;
    tl_basename(path, g_panes[pi].progname, TL_TITLE_MAX);
    term_pty_set_winsize(master, term_rows, term_cols);
    // Start the "did this run long enough to be worth a notification" clock,
    // for THIS pane's tab.
    term_notify_cmd_start(g_panes[pi].tab, path);
}

int term_layout_active_tab(void) {
    if (!g_ready || g_active < 0 || !g_panes[g_active].used) return 0;
    return g_panes[g_active].tab;
}

int term_layout_timeout_ms(void) {
    // ANY pane with a live child, not just the focused one: a background pane
    // running a command deserves the same output latency the foreground one
    // gets, which is the entire point of the panes being independent.
    if (!g_ready) return 100;
    for (int i = 0; i < TL_MAX_PANES; i++)
        if (g_panes[i].used && g_panes[i].master >= 0) return 10;
    return 100;
}

int term_layout_pane_busy(void) {
    int pi = tl_focus();
    return (pi >= 0 && g_panes[pi].used && g_panes[pi].master >= 0);
}

static void tl_child_finished(int pi) {
    tl_activate(pi);
    int st = term_pty_finish(g_panes[pi].master, g_panes[pi].pid);
    // Whether this becomes a toast is term_notify_cmd_end()'s decision; naming
    // the right TAB is ours.
    term_notify_cmd_end(g_panes[pi].tab, st);
    g_panes[pi].master = -1;
    g_panes[pi].pid = 0;
    str_copy(g_panes[pi].progname, "shell", TL_TITLE_MAX);
    print_prompt();
}

// ===========================================================================
// 10. DRAWING
// ===========================================================================
static int tl_tab_w(void) {
    int avail = g_content_w - TL_TAB_H;      // the [+] button lives at the left
    int tw = g_tab_count > 0 ? avail / g_tab_count : avail;
    if (tw > TL_TAB_MAX_W) tw = TL_TAB_MAX_W;
    if (tw < TL_TAB_MIN_W) tw = TL_TAB_MIN_W;
    return tw;
}

// Keep the strip's scroll offset inside its own content. Spec 2.5 says the
// strip SCROLLS rather than shrinking tabs below 120px, and without this the
// tabs past the right edge are reachable by keyboard but not by mouse - which
// is a hole a screenshot of four tabs would never show.
static void tl_tab_scroll_clamp(void) {
    int avail = g_content_w - TL_TAB_H;
    int total = tl_tab_w() * g_tab_count;
    int max = total - avail;
    if (max < 0) max = 0;
    if (g_tab_scroll > max) g_tab_scroll = max;
    if (g_tab_scroll < 0) g_tab_scroll = 0;
}

static void tl_tab_rects(int idx, int *x, int *w) {
    int tw = tl_tab_w();
    *w = tw;
    *x = TL_TAB_H + idx * tw - g_tab_scroll;
}

// Bring tab `idx` fully into view. Called on every tab switch, so a tab
// reached by Ctrl+PgDn or Alt+N is never left off-screen.
static void tl_tab_reveal(int idx) {
    int x, w;
    tl_tab_scroll_clamp();
    tl_tab_rects(idx, &x, &w);
    if (x < TL_TAB_H)            g_tab_scroll -= (TL_TAB_H - x);
    else if (x + w > g_content_w) g_tab_scroll += (x + w - g_content_w);
    tl_tab_scroll_clamp();
}

static void tl_draw_strip(void) {
    int h = tl_strip_h();
    if (h == 0) return;
    uint32_t bg     = tl_c(THEME_COLOR_TAB_BG);
    uint32_t act    = tl_c(THEME_COLOR_TAB_ACTIVE);
    uint32_t bord   = tl_c(THEME_COLOR_TAB_BORDER);
    uint32_t txt    = tl_c(THEME_COLOR_MENU_TEXT);
    uint32_t dim    = tl_c(THEME_COLOR_MENU_TEXT_DISABLED);
    uint32_t accent = tl_c(THEME_COLOR_ACCENT);

    // (#307) sy is the strip's top: the window-level chrome band (the menu
    // bar) sits above it. tl_strip_click() subtracts the same number once.
    int sy = term_content_y;

    win_draw_rect(window_handle, 0, sy, g_content_w, h, bg);
    win_draw_rect(window_handle, 0, sy + h - 1, g_content_w, 1, bord);

    // New-tab button, TL_TAB_H square at the left end (spec 2.2).
    win_draw_rect(window_handle, 2, sy + 2, TL_TAB_H - 4, TL_TAB_H - 4,
                  tl_c(THEME_COLOR_BUTTON_FACE));
    tl_frame(2, sy + 2, TL_TAB_H - 4, TL_TAB_H - 4, bord);
    win_draw_text(window_handle, 2 + (TL_TAB_H - 4 - TL_CHW) / 2,
                  sy + 2 + (TL_TAB_H - 4 - TL_CHH) / 2, "+", txt);

    if (g_tab_compact) {
        // Spec 2.5's too-narrow case. A compact indicator with two arrows,
        // rather than the spec's dropdown: the arrows need no popup widget and
        // reach every tab in the same number of clicks for the small counts
        // TL_MAX_TABS allows. Stated as a deviation, not hidden.
        char lbl[40]; char t[TL_TITLE_MAX];
        tl_tab_title(g_tab_cur, t, TL_TITLE_MAX);
        int o = 0;
        for (int i = 0; t[i] && o < 20; i++) lbl[o++] = t[i];
        lbl[o++] = ' '; lbl[o++] = '(';
        lbl[o++] = (char)('1' + g_tab_cur); lbl[o++] = '/';
        lbl[o++] = (char)('0' + g_tab_count); lbl[o++] = ')'; lbl[o] = '\0';
        tl_text_fit(TL_TAB_H + 4, sy + (h - TL_CHH) / 2, lbl,
                    g_content_w - TL_TAB_H - 4 - 40, txt);
        win_draw_text(window_handle, g_content_w - 34, sy + (h - TL_CHH) / 2, "<", txt);
        win_draw_text(window_handle, g_content_w - 16, sy + (h - TL_CHH) / 2, ">", txt);
        return;
    }

    for (int i = 0; i < g_tab_count; i++) {
        int x, w;
        tl_tab_rects(i, &x, &w);
        if (x + w <= TL_TAB_H || x >= g_content_w) continue;
        int on = (i == g_tab_cur);
        win_draw_rect(window_handle, x, sy, w - 1, h - (on ? 0 : 1), on ? act : bg);
        tl_frame(x, sy, w - 1, h, bord);
        if (on) win_draw_rect(window_handle, x + 1, sy + h - 3, w - 3, 2, accent);

        char title[TL_TITLE_MAX];
        if (g_rename_tab == i) {
            for (int k = 0; k < g_rename_len && k < TL_TITLE_MAX - 2; k++) title[k] = g_rename_buf[k];
            title[g_rename_len < TL_TITLE_MAX - 2 ? g_rename_len : TL_TITLE_MAX - 2] = '_';
            title[(g_rename_len < TL_TITLE_MAX - 2 ? g_rename_len : TL_TITLE_MAX - 2) + 1] = '\0';
        } else {
            tl_tab_title(i, title, TL_TITLE_MAX);
        }
        int tx = x + 6;
        // Attention mark for a background tab (spec 2.2): a 6px dot, left of
        // the title. 6px is not invented here - it is the taskbar's own unread
        // badge radius, so the OS does not gain a second "small circle means
        // attention" size. Bell outranks activity; term_notify.c owns that
        // precedence, this only draws what it reports.
        int mark = term_notify_tab_mark(i);
        if (mark != TERM_MARK_NONE) {
            uint32_t mc = (mark == TERM_MARK_BELL) ? 0x00FFB000u : accent;
            win_draw_rect(window_handle, tx, sy + (h - 6) / 2, 6, 6, mc);
            tx += 10;
        }
        tl_text_fit(tx, sy + (h - TL_CHH) / 2, title, w - (tx - x) - 20, on ? txt : dim);

        // Close box: visible on the active tab only (spec 2.2 says active OR
        // hover; there is no hover tracking in this phase, so active-only).
        if (on) {
            int cx = x + w - 18, cy = sy + (h - 14) / 2;
            tl_frame(cx, cy, 14, 14, bord);
            win_draw_text(window_handle, cx + 3, cy - 1, "x", txt);
        }
    }
}

static void tl_draw_pane_header(int pi, int focused) {
    tl_pane_t *p = &g_panes[pi];
    uint32_t bg  = tl_c(focused ? THEME_COLOR_TITLEBAR_ACTIVE : THEME_COLOR_TITLEBAR_INACTIVE);
    uint32_t txt = tl_c(THEME_COLOR_TITLEBAR_TEXT);
    uint32_t bord = tl_c(THEME_COLOR_WINDOW_BORDER);
    win_draw_rect(window_handle, p->rx, p->ry, p->rw, TL_HDR_H, bg);
    win_draw_rect(window_handle, p->rx, p->ry + TL_HDR_H - 1, p->rw, 1, bord);
    char title[TL_TITLE_MAX];
    tl_pane_title(pi, title, TL_TITLE_MAX);
    tl_text_fit(p->rx + 4, p->ry + (TL_HDR_H - TL_CHH) / 2, title, p->rw - 60, txt);
    // Three 14x14 buttons, right aligned, in the reference screenshot's own
    // order and meaning (spec 2.4): maximize / move-to-new-tab / close.
    int bx = p->rx + p->rw - 3 * 16 - 2;
    int by = p->ry + (TL_HDR_H - 14) / 2;
    for (int i = 0; i < 3; i++) tl_frame(bx + i * 16, by, 14, 14, bord);

    // BUTTON 0 (maximize this split) IS A DRAWN CHEVRON, NOT THE CHARACTER '^'
    // (owner-reported, 2026-08-25: "the button to collapse up uses ^ and its
    // not placed in the centre of the button"). Both halves of that were one
    // fault, and it was the GLYPH, not the centring arithmetic. MEASURED on
    // golden 2053 at 1000% zoom: the button is 14px tall, win_draw_text()'s
    // cell is 16px tall, and '^' lives in rows 2..5 of that cell - so its ink
    // landed at y 98..101 inside a button spanning y 99..112, i.e. above the
    // button's own top border. No y offset can centre it: the mark is at the
    // top of the em box because that is where a caret belongs in text.
    //
    // The same measurement is why buttons 1 and 2 are LEFT ALONE. '>' measured
    // y 102..109 and 'x' y 102..109 inside a button spanning 99..112 - both
    // already centred, because their glyphs sit mid-cell. Rewriting correct
    // pixels to make three call sites look uniform in the source is how
    // working controls get broken.
    gui_chevron(window_handle, bx + 7, by + 7, GUI_CHEV_UP, txt);
    win_draw_text(window_handle, bx + 16 + 3, by - 1, ">", txt);
    win_draw_text(window_handle, bx + 32 + 3, by - 1, "x", txt);
    if (focused) tl_frame(p->rx, p->ry, p->rw, p->rh, tl_c(THEME_COLOR_ACCENT));
}

static void tl_draw_dividers(int node) {
    if (node < 0 || !g_nodes[node].used || g_nodes[node].is_leaf) return;
    tl_node_t *n = &g_nodes[node];
    win_draw_rect(window_handle, n->dx, n->dy, n->dw, n->dh,
                  tl_c(THEME_COLOR_WINDOW_BORDER));
    tl_draw_dividers(n->kid[0]);
    tl_draw_dividers(n->kid[1]);
}

// (#damage) `force` means "this pane's pixels were destroyed, repaint all of
// them". It is a PARAMETER rather than something tl_draw_pane works out for
// itself because only the caller knows: term_layout_redraw_all() clears the
// whole window before it calls this, term_layout_idle() does not.
// Returns the number of cells it actually painted; 0 means nothing on
// screen changed, so the caller has nothing to present.
static int tl_draw_pane(int pi, int focused, int show_header, int force) {
    tl_activate(pi);
    if (force) term_shadow_gen = 0;   // stale by construction; see term_render.h
    // The VIEW's flag, not the application's. Writing cursor_visible here is
    // what made focusing a pane cancel its program's ESC[?25l; see
    // term_grid.h. Cleared immediately after the draw, so nothing downstream
    // can inherit it.
    term_cursor_suppressed = !focused;
    // (#damage) EVERY ONE of this function's ten call sites presents through
    // tl_present() immediately afterwards - term_layout_redraw_all() and
    // term_layout_idle() once per batch, and the six mouse-interaction sites
    // on the very next line. Checked, not assumed. So term_redraw() must not
    // present for itself: win_invalidate() is a synchronous window_draw() plus
    // a full content-buffer memcpy in the kernel (83 us measured against 134 ns
    // for a bare syscall), and a full redraw of N panes was paying N+1 of them.
    // If you add an eleventh call site, it presents too.
    term_render_suppress_present(1);
    term_redraw();
    term_render_suppress_present(0);
    int painted = term_render_last_painted();
    term_cursor_suppressed = 0;
    if (show_header) tl_draw_pane_header(pi, focused);
    return painted;
}

void term_layout_redraw_all(void) {
    if (!g_ready) return;
    tl_tab_t *t = tl_tab();
    // (#damage) THIS FUNCTION CLEARS THE WHOLE WINDOW, so nothing any pane
    // painted survives it - including panes it is not about to draw (a
    // maximized tab draws one pane; the others are still on screen as
    // background). Say so ONCE, here, rather than making every caller of this
    // function remember: this is the single chokepoint every path that
    // destroys pane pixels already goes through (EVENT_REDRAW, EVENT_RESIZE, a
    // theme or font change, a closing menu popup, a tab switch, a relayout).
    term_render_invalidate_all();
    // (#307) Clear only from term_content_y down. The band above it belongs to
    // window-level chrome (the menu bar), which is repainted by the hooks at
    // the end of this function; clearing it here first would make the bar blink
    // on every full redraw.
    win_draw_rect(window_handle, 0, term_content_y, g_content_w,
                  g_content_h - term_content_y, tl_c(THEME_COLOR_WINDOW_BG));
    tl_draw_strip();
    int list[TL_MAX_PANES], n = 0;
    tl_collect(t->root, list, &n, TL_MAX_PANES);
    int show_header = (n > 1);
    if (t->maximized >= 0) {
        tl_draw_pane(t->maximized, 1, 1, 1);
        tl_activate(t->focus);
        tl_present();
        return;
    }
    tl_draw_dividers(t->root);
    for (int i = 0; i < n; i++)
        if (g_panes[list[i]].visible)
            tl_draw_pane(list[i], list[i] == t->focus, show_header, 1);
    tl_activate(t->focus);
    tl_present();
}

// ===========================================================================
// 11. INIT
// ===========================================================================
void term_layout_init(int content_w, int content_h) {
    g_content_w = content_w;
    g_content_h = content_h;

    // Pane 0 ADOPTS the already-live globals rather than allocating: main()
    // has already malloc'd sb_lines and term_grid.c's static default buffers
    // are what `cells`/`alt_saved_cells` point at. That keeps the one-tab,
    // one-pane case byte-for-byte the terminal it was before this file
    // existed, allocation included.
    tl_pane_t *p = &g_panes[0];
    for (unsigned long k = 0; k < sizeof(*p); k++) ((char *)p)[k] = 0;
    p->used = 1;
    p->master = -1;
    p->visible = 1;
    str_copy(p->progname, "shell", TL_TITLE_MAX);
    g_active = 0;
    tl_bank_out(p);                       // capture the live globals as pane 0

    for (int i = 0; i < TL_MAX_NODES; i++) g_nodes[i].used = 0;
    int nd = tl_node_new();
    g_nodes[nd].is_leaf = 1;
    g_nodes[nd].pane = 0;
    g_tabs[0].used = 1;
    g_tabs[0].root = nd;
    g_tabs[0].focus = 0;
    g_tabs[0].maximized = -1;
    g_tabs[0].renamed = 0;
    g_tabs[0].title[0] = '\0';
    g_tab_count = 1;
    g_tab_cur = 0;
    g_ready = 1;
    tl_relayout();
}

// ---------------------------------------------------------------------------
// Per-pane mouse tracking. Motion is reported once per CELL, not per pixel: a
// pixel-rate report floods the pty, and every application quantises to cells.
static int g_mouse_held = -1;
static int g_mouse_last_col = -1, g_mouse_last_row = -1;

// ---------------------------------------------------------------------------
// MOUSE REPORTING (?1000 / ?1002 / ?1003, encoded per ?1006).
//
// Only ever active while a foreground child owns the pty AND that child asked
// for it. That gate matters: without it a terminal keeps emitting mouse bytes
// at its own shell prompt after a TUI exits, which looks to the user like the
// keyboard producing garbage.
//
// The three modes are LEVELS, not flags (see csi_private_mode()):
//   1000  press and release only
//   1002  the above plus motion WHILE A BUTTON IS HELD (drag)
//   1003  the above plus every motion, button or not
//
// Motion is reported at most once per CELL, not once per pixel. A pixel-rate
// report floods the pty, and every application quantises to cells anyway.
static int term_mouse_btn(uint32_t buttons) {
    if (buttons & MOUSE_BUTTON_MIDDLE) return 1;
    if (buttons & MOUSE_BUTTON_RIGHT)  return 2;
    return 0;                                   // left, and the default
}

int term_mouse_report(const gui_event_t *ev, int etype) {
    if (g_term_modes.mouse_mode == 0 || g_active_master_fd < 0) return 0;

    // PHASE 1: the click arrives in WINDOW coordinates; the application thinks
    // in its own pane's cells, so subtract the pane origin before dividing.
    int col = (ev->mouse_x - term_origin_x - 2) / TERM_CHAR_W;
    int row = (ev->mouse_y - term_origin_y - 2) / TERM_CHAR_H;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (col > term_cols - 1) col = term_cols - 1;
    if (row > term_rows - 1) row = term_rows - 1;

    int btn, release = 0;
    if (etype == EVENT_MOUSE_DOWN) {
        btn = term_mouse_btn(ev->mouse_buttons);
        g_mouse_held = btn;
        g_mouse_last_col = col; g_mouse_last_row = row;
    } else if (etype == EVENT_MOUSE_UP) {
        btn = (g_mouse_held >= 0) ? g_mouse_held : 0;
        release = 1;
        g_mouse_held = -1;
    } else if (etype == EVENT_MOUSE_SCROLL) {
        btn = (ev->scroll_delta > 0) ? 64 : 65;   // wheel up / down
    } else {   // EVENT_MOUSE_MOVE
        if (g_term_modes.mouse_mode == 1000) return 0;                       // press/release only
        if (g_term_modes.mouse_mode == 1002 && g_mouse_held < 0) return 0;   // drag only
        if (col == g_mouse_last_col && row == g_mouse_last_row) return 0;
        g_mouse_last_col = col; g_mouse_last_row = row;
        btn = ((g_mouse_held >= 0) ? g_mouse_held : 3) + 32;      // +32 = motion
    }

    char out[40];
    int n;
    if (g_term_modes.mouse_sgr) {
        n = term_emu_mouse_sgr(out, btn, col, row, release);
    } else {
        // The legacy encoding cannot say WHICH button was released - every
        // release is button 3 - and cannot address a column past 222. Both
        // limits are the encoding's, not this terminal's; ?1006 exists to
        // lift them and term_emu_mouse_x10() returns 0 rather than lying
        // about a position it cannot represent.
        n = term_emu_mouse_x10(out, release ? 3 : btn, col, row);
    }
    if (n > 0) term_reply(out, n);
    return 1;
}

// ===========================================================================
// 12. IDLE: pump every pane, blink, prefs reload
// ===========================================================================
// #426: nothing here blocks. Every read() is on a non-blocking master and
// every loop is bounded by the pane count.
// ===========================================================================
// (#307 PHASE 1) THE MENU BAR'S WAY IN. See term_layout.h for why these exist
// rather than a second copy of the split tree in term_menu.c. Every one of
// them is a thin call to the static that the equivalent CLICK already reaches,
// so mouse and menu cannot drift apart.
// ===========================================================================
static void tl_reflow_all_tabs(void);   // defined below; the menu's Zoom needs it

void term_layout_content_size(int *w, int *h) {
    if (w) *w = g_content_w;
    if (h) *h = g_content_h;
}

int term_layout_pane_count(void) {
    if (!g_ready) return 0;
    return tl_pane_count_in(tl_tab()->root);
}

int term_layout_tab_count(void) {
    return g_ready ? g_tab_count : 0;
}

int term_layout_can(int cmd) {
    if (!g_ready) return 0;
    int panes = tl_pane_count_in(tl_tab()->root);
    switch (cmd) {
        case TL_CMD_NEW_TAB:       return g_tab_count < TL_MAX_TABS;
        case TL_CMD_CLOSE_TAB:     return 1;   // last tab closes the window, as the strip's x does
        case TL_CMD_NEXT_TAB:
        case TL_CMD_PREV_TAB:      return g_tab_count > 1;
        // A split that would take either child below TL_MIN_COLS x TL_MIN_ROWS
        // is refused (spec 2.5), and tl_split_fits() is the one place that
        // decides so, so the menu asks IT rather than re-deriving the rule.
        case TL_CMD_SPLIT_RIGHT:   return tl_split_fits(tl_leaf_of_pane(tl_tab()->root, tl_focus()),
                                                        TL_SPLIT_VERTICAL);
        case TL_CMD_SPLIT_DOWN:    return tl_split_fits(tl_leaf_of_pane(tl_tab()->root, tl_focus()),
                                                        TL_SPLIT_HORIZONTAL);
        case TL_CMD_CLOSE_SPLIT:
        case TL_CMD_MAXIMIZE_PANE:
        case TL_CMD_PANE_TO_TAB:   return panes > 1;
        default:                   return 0;
    }
}

int term_layout_command(int cmd) {
    if (!term_layout_can(cmd)) return 0;
    tl_tab_t *t = tl_tab();
    switch (cmd) {
        case TL_CMD_NEW_TAB: {
            int ti = tl_tab_new();
            if (ti < 0) return 0;
            tl_switch_tab(ti);
            return 1;
        }
        case TL_CMD_CLOSE_TAB:
            if (tl_tab_close(g_tab_cur)) return -1;   // that was the last one
            return 1;
        case TL_CMD_NEXT_TAB:
            tl_switch_tab((g_tab_cur + 1) % g_tab_count);
            return 1;
        case TL_CMD_PREV_TAB:
            tl_switch_tab(g_tab_cur > 0 ? g_tab_cur - 1 : g_tab_count - 1);
            return 1;
        case TL_CMD_SPLIT_RIGHT:
            tl_split(TL_SPLIT_VERTICAL);
            return 1;
        case TL_CMD_SPLIT_DOWN:
            tl_split(TL_SPLIT_HORIZONTAL);
            return 1;
        case TL_CMD_CLOSE_SPLIT:
            tl_close_split(t->focus);
            return 1;
        case TL_CMD_MAXIMIZE_PANE:
            // Identical to the pane header's first button, including the
            // "a maximized pane must own the keyboard" rule.
            t->maximized = (t->maximized >= 0) ? -1 : t->focus;
            if (t->maximized >= 0) tl_set_focus(t->focus);
            tl_relayout();
            term_layout_redraw_all();
            return 1;
        case TL_CMD_PANE_TO_TAB:
            tl_pane_to_new_tab(t->focus);
            return 1;
        default:
            return 0;
    }
}

void term_layout_reflow(void) {
    if (!g_ready) return;
    tl_reflow_all_tabs();
    term_layout_redraw_all();
}

void term_layout_idle(void) {
    if (!g_ready) return;
    int prev_focus = tl_focus();
    int dirty = 0;

    for (int i = 0; i < TL_MAX_PANES; i++) {
        if (!g_panes[i].used || g_panes[i].master < 0) continue;
        tl_activate(i);
        int alive = 1;
        int drew = term_pty_drain(g_panes[i].master, &alive);
        // Child output is ACTIVITY for that pane's tab. Once per tick rather
        // than once per read(): term_notify_output() already fires only on the
        // idle-to-active transition, so a chatty program cannot become a toast
        // storm either way, and this keeps the pump loop free of policy.
        if (drew) term_notify_output(g_panes[i].tab);
        if (!alive) { tl_child_finished(i); drew = 1; }
        if (drew) {
            if (g_panes[i].visible) {
                // (#damage) `drew` means BYTES ARRIVED, which is not the same
                // as PIXELS CHANGED: a program that rewrites a line with the
                // same content, or emits a cursor-position report, produces
                // bytes and no visible change at all. Present only what the
                // renderer says it actually painted.
                if (tl_draw_pane(i, i == prev_focus,
                                 tl_pane_count_in(tl_tab()->root) > 1, 0) > 0)
                    dirty = 1;
            }
        }
    }
    tl_activate(prev_focus);

    // Cursor blink, focused pane only. Wall-clock, not a tick count: the
    // event-loop timeout is not a clock (a busy window wakes far more often
    // than every 100 ms).
    unsigned long now = uptime_ms();
    if (now - g_blink_ms >= 500) {
        g_blink_ms = now;
        // The BLINK PHASE, not the application's DECTCEM visibility. These
        // used to be one variable, so ESC[?25l was undone by the next blink
        // tick half a second later.
        cursor_blink_on = !cursor_blink_on;
        if (prev_focus >= 0 && g_panes[prev_focus].visible) {
            // (#damage) A blink toggle changes exactly ONE cell's descriptor,
            // so this repaints one cell instead of the grid. When the pane has
            // no cursor to draw at all (the program set ESC[?25l, or the view
            // is scrolled back, or the profile has blinking off) it paints
            // NOTHING and does not present, which is what stops an idle,
            // unwatched terminal from committing its whole content buffer
            // twice a second forever.
            if (tl_draw_pane(prev_focus, 1,
                             tl_pane_count_in(tl_tab()->root) > 1, 0) > 0)
                dirty = 1;
        }
    }

    // A font/size change reflows EVERY pane, and every pane's child gets
    // SIGWINCH, because tl_relayout() goes through the one geometry function.
    if (term_prefs_poll_reload()) {
        tl_relayout();
        term_layout_redraw_all();
        dirty = 1;
    }

    // NOTE: the modifier tracker's resync is NOT called here. main.c drives
    // the loop with gui_mods_next_event(), which performs the resync itself
    // at exactly the moment it is correct (the wait expired with an empty
    // queue). Two healing mechanisms for one piece of state is how they drift.

    if (dirty) tl_present();
}

// ===========================================================================
// 13. EVENTS
// ===========================================================================
static void tl_reflow_all_tabs(void) {
    // The prefs dialog can change the font, which changes TERM_CHAR_W/H, which
    // changes every pane in every tab. Background tabs are re-laid-out on
    // switch, but their CHILD needs SIGWINCH now, not when the user happens to
    // look at that tab (#227).
    int save_tab = g_tab_cur;
    for (int t = 0; t < TL_MAX_TABS; t++) {
        if (!g_tabs[t].used) continue;
        g_tab_cur = t;
        tl_relayout();
    }
    g_tab_cur = save_tab;
    tl_relayout();
}

static int tl_pane_at(int mx, int my) {
    tl_tab_t *t = tl_tab();
    if (t->maximized >= 0) return t->maximized;
    int list[TL_MAX_PANES], n = 0;
    tl_collect(t->root, list, &n, TL_MAX_PANES);
    for (int i = 0; i < n; i++) {
        tl_pane_t *p = &g_panes[list[i]];
        if (p->visible && tl_in(mx, my, p->rx, p->ry, p->rw, p->rh)) return list[i];
    }
    return -1;
}

static int tl_divider_at(int node, int mx, int my) {
    if (node < 0 || !g_nodes[node].used || g_nodes[node].is_leaf) return -1;
    tl_node_t *n = &g_nodes[node];
    int pad = (TL_DIV_HIT - TL_DIV) / 2;
    if (tl_in(mx, my, n->dx - pad, n->dy - pad, n->dw + 2 * pad, n->dh + 2 * pad))
        return node;
    int r = tl_divider_at(n->kid[0], mx, my);
    if (r >= 0) return r;
    return tl_divider_at(n->kid[1], mx, my);
}

// Header button hit: 0 = maximize, 1 = to-new-tab, 2 = close, -1 = none.
static int tl_hdr_button_at(int pi, int mx, int my) {
    tl_pane_t *p = &g_panes[pi];
    int bx = p->rx + p->rw - 3 * 16 - 2;
    int by = p->ry + (TL_HDR_H - 14) / 2;
    for (int i = 0; i < 3; i++)
        if (tl_in(mx, my, bx + i * 16, by, 14, 14)) return i;
    return -1;
}

static int tl_strip_click(int mx, int my) {
    int h = tl_strip_h();
    if (h == 0) return 0;
    // (#307) Translate ONCE into strip-local coordinates, so every rect below
    // stays written in the same coordinates tl_draw_strip() draws them in. A
    // hit-test that adds the offset per-rect is how a hit-test drifts from its
    // draw, which is the whole reason gui_menu exists (#562).
    my -= term_content_y;
    if (my < 0 || my >= h) return 0;
    if (tl_in(mx, my, 2, 2, TL_TAB_H - 4, TL_TAB_H - 4)) {
        int ti = tl_tab_new();
        if (ti >= 0) tl_switch_tab(ti);
        return 1;
    }
    if (g_tab_compact) {
        if (mx >= g_content_w - 34 && mx < g_content_w - 18) {
            tl_switch_tab(g_tab_cur > 0 ? g_tab_cur - 1 : g_tab_count - 1);
            return 1;
        }
        if (mx >= g_content_w - 18) {
            tl_switch_tab((g_tab_cur + 1) % g_tab_count);
            return 1;
        }
        return 1;
    }
    for (int i = 0; i < g_tab_count; i++) {
        int x, w;
        tl_tab_rects(i, &x, &w);
        if (!tl_in(mx, my, x, 0, w, h)) continue;
        if (i == g_tab_cur && mx >= x + w - 18 && mx < x + w - 4) {
            if (tl_tab_close(i)) return -1;      // last tab: quit
            return 1;
        }
        unsigned long now = uptime_ms();
        if (g_last_tab_click_idx == i && now - g_last_tab_click_ms < 400) {
            // Double click: inline rename (spec 2.2). No dialog: the strip
            // edits in place, Enter commits, Escape cancels.
            g_rename_tab = i;
            g_rename_len = 0;
            g_last_tab_click_idx = -1;
            term_layout_redraw_all();
            return 1;
        }
        g_last_tab_click_ms = now;
        g_last_tab_click_idx = i;
        g_drag_tab = i;
        g_drag_tab_x = mx;
        if (i != g_tab_cur) tl_switch_tab(i);
        return 1;
    }
    return 1;   // a click on empty strip is still the strip's
}

static int tl_rename_key(gui_event_t *ev) {
    char c = ev->key_char;
    uint32_t kc = ev->keycode;
    if (kc == 0x1C || c == '\n' || c == '\r') {
        if (g_rename_len > 0) {
            g_rename_buf[g_rename_len] = '\0';
            str_copy(g_tabs[g_rename_tab].title, g_rename_buf, TL_TITLE_MAX);
            g_tabs[g_rename_tab].renamed = 1;
        }
        g_rename_tab = -1;
    } else if (c == 27) {
        g_rename_tab = -1;
    } else if (c == '\b' || kc == 0x0E) {
        if (g_rename_len > 0) g_rename_len--;
    } else if (c >= ' ' && c < 127 && g_rename_len < TL_TITLE_MAX - 2) {
        g_rename_buf[g_rename_len++] = c;
    }
    term_layout_redraw_all();
    return 1;
}

// Move keyboard focus to the nearest pane in a direction. Uses the laid-out
// rects rather than walking the tree, so it does the geometrically obvious
// thing in a nested tree instead of the tree-order thing.
static void tl_focus_dir(int dx, int dy) {
    tl_tab_t *t = tl_tab();
    if (t->maximized >= 0) return;
    tl_pane_t *f = &g_panes[t->focus];
    int fcx = f->rx + f->rw / 2, fcy = f->ry + f->rh / 2;
    int list[TL_MAX_PANES], n = 0;
    tl_collect(t->root, list, &n, TL_MAX_PANES);
    int best = -1, bestd = 0;
    for (int i = 0; i < n; i++) {
        if (list[i] == t->focus) continue;
        tl_pane_t *p = &g_panes[list[i]];
        int cx = p->rx + p->rw / 2, cy = p->ry + p->rh / 2;
        int ddx = cx - fcx, ddy = cy - fcy;
        if (dx > 0 && ddx <= 0) continue;
        if (dx < 0 && ddx >= 0) continue;
        if (dy > 0 && ddy <= 0) continue;
        if (dy < 0 && ddy >= 0) continue;
        int d = (ddx < 0 ? -ddx : ddx) + (ddy < 0 ? -ddy : ddy);
        if (best < 0 || d < bestd) { best = list[i]; bestd = d; }
    }
    if (best >= 0) {
        tl_set_focus(best);
        term_layout_redraw_all();
    }
}

// Keyboard shortcuts. Built on gui_mods.h, the shared modifier tracker, which
// #221 phase 0 landed and PROVED by injected keystrokes. No private modifier
// tracking exists in this file, by design.
static int tl_shortcut(gui_event_t *ev) {
    uint32_t kc = ev->keycode;
    int letter = gui_mods_letter(ev);

    // gui_mods_is() is an EXACT chord match, not a subset test. That is what
    // keeps Ctrl+Shift+C off a bare Ctrl+C binding, and it is why nothing
    // below ever inspects key_char to decide a chord: Ctrl+c is 0x03,
    // Ctrl+Shift+c is 'C' and plain Shift+c is ALSO 'C', so the character
    // cannot tell them apart. gui_mods_letter() undoes the folding.
    if (gui_mods_is(GUI_MOD_CTRL | GUI_MOD_SHIFT)) {
        switch (letter) {
            case 't': { int ti = tl_tab_new(); if (ti >= 0) tl_switch_tab(ti); return 1; }
            case 'w': return tl_tab_close(g_tab_cur) ? -1 : 1;
            case 'o': tl_close_split(tl_focus()); return 1;
            default: break;
        }
        // Spec 2.1's Split Right / Split Down are Ctrl+Shift+\ and Ctrl+Shift+-.
        // Matched on the character, because gui_mods_letter() is letters only.
        // If the kernel's case folding makes either punctuation unreachable
        // under this chord, the tab-strip buttons and the pane header remain
        // the primary path; this is the accelerator, not the only route.
        if (ev->key_char == '\\' || ev->key_char == '|') { tl_split(TL_SPLIT_VERTICAL); return 1; }
        if (ev->key_char == '-'  || ev->key_char == '_') { tl_split(TL_SPLIT_HORIZONTAL); return 1; }
        if (kc == GUI_KEY_LEFT)  { tl_focus_dir(-1, 0); return 1; }
        if (kc == GUI_KEY_RIGHT) { tl_focus_dir(1, 0);  return 1; }
        if (kc == GUI_KEY_UP)    { tl_focus_dir(0, -1); return 1; }
        if (kc == GUI_KEY_DOWN)  { tl_focus_dir(0, 1);  return 1; }
        if (kc == 0x1C) {          // Enter: maximize / restore this split
            tl_tab_t *t = tl_tab();
            t->maximized = (t->maximized >= 0) ? -1 : t->focus;
            tl_relayout();
            term_layout_redraw_all();
            return 1;
        }
    }
    if (gui_mods_is(GUI_MOD_CTRL)) {
        if (kc == GUI_KEY_PGDN) { tl_switch_tab((g_tab_cur + 1) % g_tab_count); return 1; }
        if (kc == GUI_KEY_PGUP) { tl_switch_tab(g_tab_cur > 0 ? g_tab_cur - 1 : g_tab_count - 1); return 1; }
    }
    if (gui_mods_is(GUI_MOD_ALT)) {
        char c = ev->key_char;
        if (c >= '1' && c <= '9') {
            int want = c - '1';
            if (want < g_tab_count) tl_switch_tab(want);
            return 1;
        }
    }
    return 0;
}

int term_layout_event(int et, gui_event_t *ev) {
    if (!g_ready) return 0;
    // main.c dequeues with gui_mods_next_event(), which has ALREADY fed this
    // event to the tracker. Feeding it a second time is idempotent (a press
    // sets a bit that is already set, a release clears one that is already
    // clear) and it is done here only to get the return value: "this event was
    // nothing but a modifier transition", which must be swallowed so the
    // built-in line editor never sees it. Detecting that from the keycode
    // instead is NOT safe - Alt's RELEASE code is 0x1C, which is also Enter.
    if (gui_mods_feed(ev)) return 1;

    switch (et) {
        case EVENT_REDRAW:
            term_layout_redraw_all();
            return 1;

        case EVENT_RESIZE:
            // #221: the find bar is docked at the BOTTOM of the window and
            // takes its strip out of the content area BEFORE panes are laid
            // out, so pane geometry, the #220 row alignment and every child's
            // TIOCSWINSZ all stay the business of the one geometry function.
            // term_search_reserved_h() is 0 whenever the bar is shut, which is
            // the only state that existed before this feature.
            term_search_note_window(ev->mouse_x, ev->mouse_y);
            g_content_w = ev->mouse_x;
            g_content_h = ev->mouse_y - term_search_reserved_h();
            // #221: the grid does not reflow text on resize, so a selection
            // made at the old width no longer describes the same characters.
            // Cleared, as xterm does.
            term_select_on_resize();
            tl_reflow_all_tabs();
            term_layout_redraw_all();
            return 1;

        case EVENT_MOUSE_DOWN: {
            int mx = ev->mouse_x, my = ev->mouse_y;
            int r = tl_strip_click(mx, my);
            if (r != 0) return r < 0 ? -1 : 1;
            int dn = tl_divider_at(tl_tab()->root, mx, my);
            if (dn >= 0 && tl_tab()->maximized < 0) { g_drag_div = dn; return 1; }
            int pi = tl_pane_at(mx, my);
            if (pi < 0) return 1;
            int npanes = tl_pane_count_in(tl_tab()->root);
            if (npanes > 1 && my < g_panes[pi].ry + TL_HDR_H) {
                int b = tl_hdr_button_at(pi, mx, my);
                if (b == 0) {
                    tl_tab_t *t = tl_tab();
                    t->maximized = (t->maximized >= 0) ? -1 : pi;
                    // A maximized pane is the ONLY one on screen, so it must
                    // also be the one the keyboard talks to.
                    if (t->maximized >= 0) tl_set_focus(pi);
                    tl_relayout(); term_layout_redraw_all(); return 1;
                }
                if (b == 1) { tl_pane_to_new_tab(pi); return 1; }
                if (b == 2) { tl_close_split(pi); return 1; }
                // Clicking the header focuses the pane.
                tl_set_focus(pi); term_layout_redraw_all();
                return 1;
            }
            if (tl_tab()->focus != pi) {
                tl_set_focus(pi);
                term_layout_redraw_all();
                return 1;
            }
            tl_activate(pi);
            term_scrollback_reconfigure();
            if (gui_scroll_press(&term_scroll_view, mx, my)) {
                term_scroll_sync_bottom();
                tl_draw_pane(pi, 1, npanes > 1, 0);
                tl_present();
                return 1;
            }
            // #221 ARBITRATION. The scrollbar gets first refusal above (a
            // press in the gutter is never a selection). term_select then
            // decides between local selection and the application: it returns
            // TERM_SEL_PASS exactly when the app owns the mouse, which is what
            // finally gives term_mouse_report() a caller.
            {
                int sr = term_select_handle_mouse(ev, et);
                if (sr == TERM_SEL_PASS) term_mouse_report(ev, et);
                else if (sr == TERM_SEL_REDRAW) {
                    tl_draw_pane(pi, 1, npanes > 1, 0);
                    tl_present();
                }
            }
            return 1;
        }

        case EVENT_MOUSE_MOVE: {
            int mx = ev->mouse_x, my = ev->mouse_y;
            if (g_drag_div >= 0) {
                tl_node_t *n = &g_nodes[g_drag_div];
                int r;
                if (n->orient == TL_SPLIT_VERTICAL) {
                    int avail = n->rw - TL_DIV; if (avail < 2) avail = 2;
                    r = (mx - n->rx) * 1000 / avail;
                } else {
                    int avail = n->rh - TL_DIV; if (avail < 2) avail = 2;
                    r = (my - n->ry) * 1000 / avail;
                }
                // Clamp so neither child drops below the minimum pane size
                // (spec 2.4/2.5): the drag stops at the limit, it does not
                // jump past it.
                int minw = TL_MIN_COLS * TERM_CHAR_W + GUI_SCROLL_W;
                int minh = TL_MIN_ROWS * TERM_CHAR_H + TL_HDR_H;
                int span = (n->orient == TL_SPLIT_VERTICAL) ? n->rw - TL_DIV : n->rh - TL_DIV;
                int minpx = (n->orient == TL_SPLIT_VERTICAL) ? minw : minh;
                int lo = span > 0 ? minpx * 1000 / span : 0;
                int hi = 1000 - lo;
                if (lo > hi) { lo = 400; hi = 600; }
                if (r < lo) r = lo;
                if (r > hi) r = hi;
                if (r != n->ratio) {
                    n->ratio = r;
                    tl_relayout();
                    term_layout_redraw_all();
                }
                return 1;
            }
            if (g_drag_tab >= 0 && !g_tab_compact) {
                int x, w;
                tl_tab_rects(g_drag_tab, &x, &w);
                // Past a neighbour's midpoint: swap. Live, matching every
                // tabbed UI the user already knows (spec 2.2).
                if (mx > x + w + w / 2 && g_drag_tab + 1 < g_tab_count) {
                    tl_tab_swap(g_drag_tab, g_drag_tab + 1);
                    if (g_tab_cur == g_drag_tab) g_tab_cur++;
                    else if (g_tab_cur == g_drag_tab + 1) g_tab_cur--;
                    g_drag_tab++;
                    term_layout_redraw_all();
                } else if (mx < x - w / 2 && g_drag_tab > 0) {
                    tl_tab_swap(g_drag_tab, g_drag_tab - 1);
                    if (g_tab_cur == g_drag_tab) g_tab_cur--;
                    else if (g_tab_cur == g_drag_tab - 1) g_tab_cur++;
                    g_drag_tab--;
                    term_layout_redraw_all();
                }
                return 1;
            }
            int pi = tl_focus();
            if (pi >= 0) {
                tl_activate(pi);
                if (gui_scroll_motion(&term_scroll_view, mx, my)) {
                    term_scroll_sync_bottom();
                    tl_draw_pane(pi, 1, tl_pane_count_in(tl_tab()->root) > 1, 0);
                    tl_present();
                    return 1;
                }
                int sr = term_select_handle_mouse(ev, et);   // #221
                if (sr == TERM_SEL_PASS) term_mouse_report(ev, et);
                else if (sr == TERM_SEL_REDRAW) {
                    tl_draw_pane(pi, 1, tl_pane_count_in(tl_tab()->root) > 1, 0);
                    tl_present();
                }
            }
            return 1;
        }

        case EVENT_MOUSE_UP: {
            g_drag_div = -1;
            g_drag_tab = -1;
            int pi = tl_focus();
            if (pi >= 0) {
                tl_activate(pi);
                gui_scroll_release(&term_scroll_view);
                int sr = term_select_handle_mouse(ev, et);   // #221
                if (sr == TERM_SEL_PASS) term_mouse_report(ev, et);
                else if (sr == TERM_SEL_REDRAW) {
                    tl_draw_pane(pi, 1, tl_pane_count_in(tl_tab()->root) > 1, 0);
                    tl_present();
                }
            }
            return 1;
        }

        case EVENT_MOUSE_SCROLL: {
            // Over the tab strip, the wheel scrolls the STRIP, not a pane's
            // scrollback. This is the reachability half of spec 2.5: at
            // TL_MAX_TABS on a narrow window the later tabs are off the right
            // edge, and without this they could only be reached by keyboard.
            if (tl_strip_h() > 0 && ev->mouse_y < tl_strip_h()) {
                g_tab_scroll -= ev->scroll_delta * TL_TAB_MIN_W / 2;
                tl_tab_scroll_clamp();
                term_layout_redraw_all();
                return 1;
            }
            int pi = tl_pane_at(ev->mouse_x, ev->mouse_y);
            if (pi < 0) pi = tl_focus();
            if (pi < 0) return 1;
            tl_activate(pi);
            // #221: a full-screen program that asked for mouse reporting gets
            // the wheel; Shift-wheel still scrolls OUR scrollback, the same
            // override that applies to a click.
            if (term_select_app_owns_mouse() && term_mouse_report(ev, et)) {
                tl_activate(tl_focus());
                return 1;
            }
            term_scrollback_reconfigure();
            if (gui_scroll_wheel(&term_scroll_view, ev->scroll_delta)) {
                term_scroll_sync_bottom();
                tl_draw_pane(pi, pi == tl_focus(), tl_pane_count_in(tl_tab()->root) > 1, 0);
                tl_present();
            }
            tl_activate(tl_focus());
            return 1;
        }

        case EVENT_KEY_DOWN: {
            if (g_rename_tab >= 0) return tl_rename_key(ev);

            // F9 is handled HERE and nowhere else. It used to have two copies,
            // one in main.c and one inside run_foreground_pty()'s nested loop,
            // and only the second one re-issued TIOCSWINSZ. With N panes the
            // reflow has to reach every one of them, so there is exactly one
            // F9 path and it goes through tl_reflow_all_tabs().
            // #221: Ctrl+Shift+C / V / A, checked before tl_shortcut() and
            // before the key is forwarded to any child. gui_mods_is() is an
            // EXACT chord test, so plain Ctrl+C is untouched and still
            // reaches the child as SIGINT.
            {
                int pf = tl_focus();
                if (pf >= 0) tl_activate(pf);
                int sr = term_select_handle_key(ev);
                if (sr) {
                    if (sr == TERM_SEL_REDRAW) term_layout_redraw_all();
                    return 1;
                }
            }
            if (ev->keycode == GUI_KEY_F9) {
                if (term_prefs_dialog()) tl_reflow_all_tabs();
                term_layout_redraw_all();
                return 1;
            }
            int s = tl_shortcut(ev);
            if (s != 0) return s;

            int pi = tl_focus();
            if (pi >= 0 && g_panes[pi].master >= 0) {
                // This pane has a live child: the keystroke is the child's.
                tl_activate(pi);
                char kb[8];
                int n = key_event_to_bytes(ev, kb);
                if (n > 0) write(g_panes[pi].master, kb, n);
                return 1;
            }
            return 0;   // built-in shell line editor, in main.c
        }

        default:
            return 0;
    }
}
