// term_search.c - find-in-scrollback for the MayteraOS Terminal (#221 PHASE 1).
// See term_search.h for the ownership boundary, what this reuses instead of
// reinventing, and why the bar is docked at the BOTTOM.

#include "term_common.h"
#include "../../libc/ctype.h"
#include "../../libc/textfield.h"
#include <regex.h>

#include "term_util.h"
#include "term_grid.h"
#include "term_scrollback.h"
#include "term_theme.h"
#include "term_render.h"
#include "term_layout.h"
#include "term_search.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
typedef struct {
    int      vline;   // virtual line: 0..sb_count-1 = scrollback, then live rows
    short    col;     // first cell of the run
    short    len;     // run length in cells
    uint32_t lhash;   // hash of the WHOLE line's text; see reanchor()
} tmatch_t;

static int  s_open        = 0;
static int  s_case        = 0;   // 0 = case-insensitive (the useful default)
static int  s_regex       = 0;
static int  s_bad_regex   = 0;

static char        s_query[TERM_SEARCH_QUERY_MAX] = "";
static textfield_t s_field;      // libc widget: caret, selection, clipboard, undo
static int         s_field_init = 0;

static regex_t s_re;
static int     s_re_valid = 0;

static tmatch_t s_match[TERM_SEARCH_MAX_MATCHES];
static int s_nmatch    = 0;
static int s_cur       = -1;     // index into s_match, -1 = none
static int s_truncated = 0;

// Incremental scan cursor. s_scan_active means "there is more of the ring to
// look at"; the scan is resumed a bounded slice at a time from
// term_search_tick(), never run to completion inside an event handler.
static int s_scan_active = 0;
static int s_scan_vline  = 0;
static int s_scan_total  = 0;

// Re-anchoring across a rescan (see reanchor()).
static uint32_t s_anchor_hash = 0;
static int      s_anchor_col  = -1;
static int      s_anchor_have = 0;
static int      s_reveal_pending = 0;

// Content-change detection. A rescan is triggered by the CONTENT changing,
// never by a timer, so an idle terminal costs nothing.
static uint32_t s_content_sig = 0;

// PANE OWNERSHIP (tabs/splits, same rule term_select.h sets out). A find
// belongs to ONE pane: the one that was focused when the bar opened. s_pane is
// that pane; s_active is whichever pane term_layout has banked into the module
// globals right now. Every function that reads the grid, the ring or
// term_scroll_view is guarded on the two being equal, because those globals
// describe s_active and nothing else.
static int s_pane   = -1;
static int s_active = -1;

void term_search_note_pane(int pane) { s_active = pane; }
static int owns(void) { return s_pane >= 0 && s_pane == s_active; }

// The window's FULL content size, as EVENT_RESIZE reports it. term_px_w /
// term_px_h are the GRID's size and already have the bar's strip subtracted.
static int s_win_w = 0, s_win_h = 0;

// The view to go back to when the bar closes.
static int s_saved_offset    = 0;
static int s_saved_at_bottom = 1;

// ---------------------------------------------------------------------------
// Colours.
//
// TWO AXES, exactly as docs/TERMINAL_MODULES.md item 5 insists: the HIGHLIGHT
// is cell-grid content, so it follows the terminal COLOUR SCHEME (with the
// same "system" fallback term_bg_color() uses, copied in shape so the two
// cannot disagree); the BAR is chrome, so it follows the OS theme, which is
// also what gui_button() uses internally and therefore the only way the bar's
// buttons and its background can be guaranteed to match each other.
// ---------------------------------------------------------------------------
// Black or white, whichever reads on `c`. The compositor's readable_ink() is
// not linkable from an app, and gui_style.h exposes no per-colour equivalent,
// so this is the same two-line luminance test every app-side caller needs. Kept
// local and tiny rather than adding a fourth spelling of it to a shared header
// nobody else is asking to change today.
static uint32_t readable_on(uint32_t c) {
    unsigned r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    unsigned lum = (r * 30 + g * 59 + b * 11) / 100;
    return (lum > 140) ? 0x00000000u : 0x00FFFFFFu;
}

static uint32_t hl_bg(void) {
    return g_term_palette_is_system
        ? theme_color_of(g_term_theme_index, THEME_COLOR_SELECTION)
        : g_term_palette.selection_bg;
}
static uint32_t hl_fg(void) {
    return g_term_palette_is_system
        ? theme_color_of(g_term_theme_index, THEME_COLOR_SELECTION_TEXT)
        : g_term_palette.selection_fg;
}

// ---------------------------------------------------------------------------
// Reading a line out of the grid / ring. READ-ONLY: nothing here writes to
// term_grid.[ch] or term_scrollback.[ch] state.
// ---------------------------------------------------------------------------
static const term_cell_t *vline_row(int vline) {
    if (vline < 0) return NULL;
    if (vline < sb_count) return sb_lines ? sb_row(vline) : NULL;
    int live = vline - sb_count;
    if (live < 0 || live >= term_rows) return NULL;
    return cells[live];
}

// Flatten one line to printable text, trailing blanks trimmed. Returns length.
// A scrollback row was captured at the full TERM_MAX_COLS width (the tail past
// the then-current term_cols is blank, see term_history_push), so it is read at
// that width and the trim does the rest; a live row is only term_cols wide.
static int line_text(int vline, char *out, int cap) {
    const term_cell_t *row = vline_row(vline);
    if (!row) { out[0] = '\0'; return 0; }
    int width = (vline < sb_count) ? TERM_MAX_COLS : term_cols;
    if (width > cap - 1) width = cap - 1;
    int n = 0;
    for (int c = 0; c < width; c++) {
        char ch = row[c].ch;
        out[n++] = (ch >= ' ' && ch < 127) ? ch : ' ';
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';
    return n;
}

static uint32_t fnv1a(const char *p, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 16777619u; }
    return h;
}

// A cheap, EXACT signature of everything this module searches. Exact rather
// than a heuristic because a wrong answer here is either a stale highlight
// (too lax) or a rescan storm (too eager). sb_count/sb_head cover the ring;
// the live cells cover typing and program output. At 80x24 this is under 2KB
// of reads, run at most once per event-loop iteration, and ONLY while the bar
// is open.
static uint32_t content_sig(void) {
    uint32_t h = 2166136261u;
    h ^= (uint32_t)sb_count; h *= 16777619u;
    h ^= (uint32_t)sb_head;  h *= 16777619u;
    h ^= (uint32_t)term_rows; h *= 16777619u;
    h ^= (uint32_t)term_cols; h *= 16777619u;
    for (int r = 0; r < term_rows; r++)
        for (int c = 0; c < term_cols; c++) {
            h ^= (unsigned char)cells[r][c].ch;
            h *= 16777619u;
        }
    return h;
}

// ---------------------------------------------------------------------------
// Pattern compilation
// ---------------------------------------------------------------------------
static void free_regex(void) {
    if (s_re_valid) { regfree(&s_re); s_re_valid = 0; }
}

// Recompile after any change to the query, the case flag or the regex flag.
// REG_EXTENDED because that is what a user typing ".*" into a box labelled
// ".*" expects (POSIX ERE), and it is the same dialect /APPS/GREP -E accepts
// from this same engine.
static void compile_pattern(void) {
    free_regex();
    s_bad_regex = 0;
    if (!s_regex || s_query[0] == '\0') return;
    int flags = REG_EXTENDED | (s_case ? 0 : REG_ICASE);
    if (regcomp(&s_re, s_query, flags) != 0) { s_bad_regex = 1; return; }
    s_re_valid = 1;
}

// ---------------------------------------------------------------------------
// The scan
// ---------------------------------------------------------------------------
static void add_match(int vline, int col, int len, uint32_t lhash) {
    if (len <= 0) return;
    if (s_nmatch >= TERM_SEARCH_MAX_MATCHES) { s_truncated = 1; return; }
    s_match[s_nmatch].vline = vline;
    s_match[s_nmatch].col   = (short)col;
    s_match[s_nmatch].len   = (short)len;
    s_match[s_nmatch].lhash = lhash;
    s_nmatch++;
}

static int chr_eq(char a, char b) {
    if (a == b) return 1;
    if (s_case) return 0;
    return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static void scan_one_line(int vline) {
    char buf[TERM_MAX_COLS + 1];
    int n = line_text(vline, buf, (int)sizeof(buf));
    if (n <= 0) return;
    uint32_t h = fnv1a(buf, n);

    if (s_regex) {
        if (!s_re_valid) return;
        regmatch_t m;
        int off = 0;
        while (off <= n) {
            int eflags = (off > 0) ? REG_NOTBOL : 0;
            if (regexec(&s_re, buf + off, 1, &m, eflags) != 0) break;
            int st = off + (int)m.rm_so;
            int en = off + (int)m.rm_eo;
            if (en > st) { add_match(vline, st, en - st, h); off = en; }
            else         { off = st + 1; }   // empty match: never loop on it
            if (s_truncated) break;
        }
        return;
    }

    int qlen = 0;
    while (s_query[qlen]) qlen++;
    if (qlen == 0) return;
    for (int i = 0; i + qlen <= n; i++) {
        int k = 0;
        while (k < qlen && chr_eq(buf[i + k], s_query[k])) k++;
        if (k == qlen) {
            add_match(vline, i, qlen, h);
            if (s_truncated) break;
            i += qlen - 1;             // non-overlapping, like every find bar
        }
    }
}

// Remember which match is active in a form that survives the whole buffer
// sliding upward under it. A (vline, col) pair does NOT survive that: every
// index shifts when a line scrolls off the top, and once the 2000-line ring is
// full the shift is not even recoverable from sb_count. The LINE'S CONTENT
// does survive, because a retained row is copied verbatim into the ring, so
// the anchor is (hash of the line text, column) and re-anchoring is a lookup,
// not arithmetic.
static void remember_anchor(void) {
    if (s_cur >= 0 && s_cur < s_nmatch) {
        s_anchor_hash = s_match[s_cur].lhash;
        s_anchor_col  = s_match[s_cur].col;
        s_anchor_have = 1;
    }
}

static void reanchor(void) {
    if (s_nmatch == 0) { s_cur = -1; return; }
    if (s_anchor_have) {
        for (int i = 0; i < s_nmatch; i++) {
            if (s_match[i].lhash == s_anchor_hash && s_match[i].col == s_anchor_col) {
                s_cur = i;
                return;
            }
        }
    }
    if (s_cur >= s_nmatch) s_cur = s_nmatch - 1;
    if (s_cur < 0) {
        // No previous selection: start from what the user is looking at, so
        // "find" means "find from here", not "jump to the top of history".
        int top = gui_scroll_first_item(&term_scroll_view);
        s_cur = 0;
        for (int i = 0; i < s_nmatch; i++)
            if (s_match[i].vline >= top) { s_cur = i; break; }
    }
}

static void reveal_current(void) {
    if (s_cur < 0 || s_cur >= s_nmatch) return;
    term_scrollback_reconfigure();
    if (gui_scroll_reveal(&term_scroll_view, s_match[s_cur].vline * TERM_CHAR_H,
                          TERM_CHAR_H))
        term_scroll_sync_bottom();
}

// Throw the match list away and start again from the oldest retained line.
// `keep_anchor` carries the active match across; `reveal` says whether to
// scroll to it when the scan finishes (true for a user action, FALSE for a
// rescan forced by new program output, which must never yank the view).
static void restart_scan(int keep_anchor, int reveal) {
    if (keep_anchor) remember_anchor(); else s_anchor_have = 0;
    s_nmatch = 0;
    s_cur = -1;
    s_truncated = 0;
    s_scan_vline = 0;
    s_scan_total = sb_count + term_rows;
    s_reveal_pending = reveal;
    s_scan_active = (s_query[0] != '\0') && !(s_regex && !s_re_valid);
    if (!s_scan_active) { s_reveal_pending = 0; s_anchor_have = 0; }
}

// Advance the scan by a WALL-CLOCK slice. A line-count budget alone is the
// wrong shape: a literal scan of a line is a few hundred nanoseconds and a
// regexec on a pathological pattern is orders of magnitude more, so a fixed
// line count is either pointlessly small or silently a stall. 12ms is under
// one frame at 60Hz, and the scan simply resumes on the next call.
#define SCAN_SLICE_MS   12
#define SCAN_CHECK_EVERY 32

static int advance_scan(void) {
    if (!s_scan_active) return 0;
    unsigned long start = uptime_ms();
    int since_check = 0;
    while (s_scan_vline < s_scan_total) {
        scan_one_line(s_scan_vline);
        s_scan_vline++;
        if (s_truncated) break;
        if (++since_check >= SCAN_CHECK_EVERY) {
            since_check = 0;
            if ((unsigned long)(uptime_ms() - start) >= SCAN_SLICE_MS) return 1;
        }
    }
    s_scan_active = 0;
    reanchor();
    if (s_reveal_pending) { reveal_current(); s_reveal_pending = 0; }
    s_content_sig = content_sig();
    return 1;
}

// Anything that changes what is being searched for goes through here.
static void query_changed(void) {
    compile_pattern();
    s_cur = -1;
    restart_scan(0, 1);
    advance_scan();          // one slice now, so the first hits appear at once
}

// ---------------------------------------------------------------------------
// Geometry. THE ONE reservation. See the header for why this is a bottom dock.
// ---------------------------------------------------------------------------
int term_search_reserved_h(void) { return s_open ? TERM_SEARCH_BAR_H : 0; }
int term_search_is_open(void)    { return s_open; }

// Re-run the WHOLE layout for the window size we already know, so the reserved
// strip appears or disappears. Deliberately a synthesised EVENT_RESIZE rather
// than a new entry point in term_layout: that case already subtracts
// term_search_reserved_h(), already re-flows every pane in every tab through
// tl_pane_apply_geometry() (the ONE geometry function, #220) and already
// re-issues TIOCSWINSZ for every pane's child (#227). A second path into pane
// geometry is precisely what #220 punishes, so there is not one.
static void apply_geometry(void) {
    if (s_win_w <= 0 || s_win_h <= 0) return;
    gui_event_t ev;
    ev.type = EVENT_RESIZE;
    ev.mouse_x = s_win_w;
    ev.mouse_y = s_win_h;
    term_layout_event(EVENT_RESIZE, &ev);
}

void term_search_note_window(int content_w, int content_h) {
    s_win_w = content_w;
    s_win_h = content_h;
}

// ---------------------------------------------------------------------------
// Bar layout. ONE function computes every rect; draw and hit-test both read it,
// so a button can never be drawn somewhere it cannot be clicked (the drift the
// Editor menu bar shipped for months, #562).
// ---------------------------------------------------------------------------
typedef struct {
    int y, h;
    int field_x, field_w, field_y, field_h;
    int case_x, regex_x, tgl_w, tgl_y, tgl_h;
    int count_x, count_w;
    int prev_x, next_x, close_x, btn_w;
} bar_rects_t;

#define BAR_LABEL_X   6
#define BAR_TGL_W    26
#define BAR_BTN_W    24
// Preferred query-field width. 220px is the spec's number; 300 is chosen here
// because this field is the one place a user retypes a whole command line.
#define BAR_FIELD_W 300

static void bar_layout(bar_rects_t *r) {
    // The bar spans the WINDOW, not a pane: with splits there are N panes and
    // one keyboard, so one bar searching the focused pane is the honest model.
    int W = s_win_w;
    r->y = s_win_h - TERM_SEARCH_BAR_H;
    r->h = TERM_SEARCH_BAR_H;
    r->field_y = r->y + 4;
    r->field_h = 18;
    r->tgl_y   = r->y + 4;
    r->tgl_h   = 18;
    r->tgl_w   = BAR_TGL_W;
    r->btn_w   = BAR_BTN_W;
    r->count_w = 13 * FONT_WIDTH;            // "999+ / 999+" and "searching..."

    // PACKED LEFT, in the spec's own order (docs/
    // TERMINAL_KONSOLE_CHROME_SPEC.md section 5:
    // "[Find:] [input] [Aa] [.*] [n / m matches] [< >]"), with only the close
    // button pinned to the right edge. The field has a FIXED preferred width
    // rather than absorbing every spare pixel: this terminal is routinely
    // maximised to 1280px, and an 950px-wide input for a one-word query reads
    // as a layout accident rather than a decision.
    r->field_x = BAR_LABEL_X + 5 * FONT_WIDTH + 8;   // after the "Find:" label
    r->close_x = W - 6 - r->btn_w;

    int fw = BAR_FIELD_W;
    // Everything to the right of the field, plus the gap before the close
    // button. If it does not fit, the FIELD gives up the pixels, because it is
    // the only element whose usefulness degrades gracefully.
    int right_of_field = 8 + r->tgl_w + 4 + r->tgl_w + 10 + r->count_w
                       + 10 + r->btn_w + 4 + r->btn_w + 8;
    int avail = r->close_x - r->field_x - right_of_field;
    if (fw > avail) fw = avail;
    if (fw < 48) fw = 48;                            // degrade, never invert
    r->field_w = fw;

    r->case_x  = r->field_x + r->field_w + 8;
    r->regex_x = r->case_x  + r->tgl_w + 4;
    r->count_x = r->regex_x + r->tgl_w + 10;
    r->prev_x  = r->count_x + r->count_w + 10;
    r->next_x  = r->prev_x  + r->btn_w + 4;
}

static int in_rect(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------
void term_search_open(void) {
    if (s_open) return;
    // The find follows the FOCUSED pane, which is whichever one term_layout
    // has banked in right now (main.c hands us the key before the layout sees
    // it, and the layout leaves the focused pane active after every repaint).
    s_pane = s_active;

    term_scrollback_reconfigure();
    term_scroll_sync_bottom();      // term_at_bottom must be FRESH, not last frame's
    s_saved_offset    = term_scroll_view.offset;
    s_saved_at_bottom = term_at_bottom;

    s_open = 1;
    apply_geometry();

    if (!s_field_init) { tf_init(&s_field, s_query, (int)sizeof(s_query)); s_field_init = 1; }
    tf_select_all(&s_field);      // typing replaces the old query, Enter repeats it
    compile_pattern();
    restart_scan(0, 1);
    advance_scan();
}

void term_search_close(void) {
    if (!s_open) return;
    s_open = 0;
    s_scan_active = 0;
    s_pane = -1;
    apply_geometry();             // give the strip back to the panes FIRST
    term_scrollback_reconfigure();// then re-derive the scroll extents for it
    if (s_saved_at_bottom)
        gui_scroll_set(&term_scroll_view, gui_scroll_max(&term_scroll_view));
    else
        gui_scroll_set(&term_scroll_view, s_saved_offset);
    term_scroll_sync_bottom();
}

void term_search_toggle(void) { if (s_open) term_search_close(); else term_search_open(); }

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------
static void step(int dir) {
    if (s_nmatch == 0) return;
    if (s_cur < 0) s_cur = (dir > 0) ? 0 : s_nmatch - 1;
    else {
        s_cur += dir;
        if (s_cur >= s_nmatch) s_cur = 0;          // wrap, like every find bar
        if (s_cur < 0) s_cur = s_nmatch - 1;
    }
    reveal_current();
    remember_anchor();
}

void term_search_next(void) { if (s_scan_active) advance_scan(); step(1); }
void term_search_prev(void) { if (s_scan_active) advance_scan(); step(-1); }

// ---------------------------------------------------------------------------
// Toggles and the programmatic query setter (the menu-bar agent's surface)
// ---------------------------------------------------------------------------
void term_search_set_case_sensitive(int on) {
    if (s_case == !!on) return;
    s_case = !!on;
    query_changed();
}
void term_search_set_regex(int on) {
    if (s_regex == !!on) return;
    s_regex = !!on;
    query_changed();
}
void term_search_set_query(const char *q) {
    int i = 0;
    if (q) while (q[i] && i < (int)sizeof(s_query) - 1) { s_query[i] = q[i]; i++; }
    s_query[i] = '\0';
    if (!s_field_init) { tf_init(&s_field, s_query, (int)sizeof(s_query)); s_field_init = 1; }
    else tf_set_text(&s_field, s_query);
    query_changed();
}

int term_search_case_sensitive(void) { return s_case; }
int term_search_regex(void)          { return s_regex; }
int term_search_match_count(void)    { return s_nmatch; }
int term_search_match_index(void)    { return (s_cur >= 0) ? s_cur + 1 : 0; }
int term_search_truncated(void)      { return s_truncated; }
int term_search_scanning(void)       { return s_scan_active; }
int term_search_bad_regex(void)      { return s_bad_regex; }
const char *term_search_query(void)  { return s_query; }

// ---------------------------------------------------------------------------
// The incremental pump. Never blocks, never spins: it either advances a
// bounded slice of a scan that is already running, or notices the terminal's
// content changed and starts one. Both are finite work with no wait in them,
// which is the rule the whole #211/#212/#230/#347/#419/#420 family came from.
// ---------------------------------------------------------------------------
int term_search_tick(void) {
    if (!s_open || !owns()) return 0;
    if (s_scan_active) return advance_scan();
    if (s_query[0] == '\0') return 0;
    uint32_t sig = content_sig();
    if (sig != s_content_sig) {
        s_content_sig = sig;
        // New output (or typing) moved the buffer under the match list. Rescan,
        // keeping the active match by CONTENT, and do NOT scroll: the user is
        // reading, and yanking the view is the exact behaviour #206 rejected
        // for output.
        restart_scan(1, 0);
        return advance_scan();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
int term_search_mouse_down(int mx, int my) {
    if (!s_open) return TERM_SEARCH_PASS;
    bar_rects_t r;
    bar_layout(&r);
    if (my < r.y || my >= r.y + r.h) return TERM_SEARCH_PASS;   // not ours

    if (in_rect(mx, my, r.close_x, r.tgl_y, r.btn_w, r.tgl_h)) { term_search_close(); return TERM_SEARCH_BAR; }
    if (in_rect(mx, my, r.next_x,  r.tgl_y, r.btn_w, r.tgl_h)) { term_search_next();  return TERM_SEARCH_REDRAW; }
    if (in_rect(mx, my, r.prev_x,  r.tgl_y, r.btn_w, r.tgl_h)) { term_search_prev();  return TERM_SEARCH_REDRAW; }
    if (in_rect(mx, my, r.case_x,  r.tgl_y, r.tgl_w, r.tgl_h)) { term_search_set_case_sensitive(!s_case);  return TERM_SEARCH_REDRAW; }
    if (in_rect(mx, my, r.regex_x, r.tgl_y, r.tgl_w, r.tgl_h)) { term_search_set_regex(!s_regex); return TERM_SEARCH_REDRAW; }
    if (in_rect(mx, my, r.field_x, r.field_y, r.field_w, r.field_h)) {
        tf_set_caret(&s_field, tf_index_from_px_mono(&s_field, mx - (r.field_x + 4)));
        return TERM_SEARCH_BAR;
    }
    // A click anywhere else in the bar is still the bar's, not a pane's, but
    // nothing moved, so nothing needs repainting beyond the bar.
    return TERM_SEARCH_BAR;
}

int term_search_key_event(const gui_event_t *ev) {
    // A pure modifier transition changes no pixel anywhere. It must still be
    // CONSUMED while the bar is open (the bar is modal over the pty), but
    // asking for a repaint here is what made a typed query lose keys.
    if (ev->keycode == GUI_KEY_LSHIFT || ev->keycode == GUI_KEY_RSHIFT ||
        ev->keycode == GUI_KEY_LCTRL  || ev->keycode == GUI_KEY_ALT    ||
        ev->keycode == GUI_KEY_SUPER)
        return s_open ? TERM_SEARCH_BAR : TERM_SEARCH_PASS;

    if (!s_open) {
        // Ctrl+Shift+F, per docs/TERMINAL_KONSOLE_CHROME_SPEC.md section 7. The
        // chord comes from the SHARED tracker (libc gui_mods.h, #221 phase 0);
        // this file tracks no modifier state of its own.
        // gui_mods_is() is an EXACT chord test, which is what keeps this from
        // also firing on a bare Ctrl+F (a byte the shell may want).
        if (gui_mods_is(GUI_MOD_CTRL | GUI_MOD_SHIFT) && gui_mods_letter(ev) == 'f') {
            term_search_open();
            // TERM_SEARCH_BAR, not REDRAW: opening and closing go through
            // apply_geometry(), which re-runs term_layout's EVENT_RESIZE case
            // and that ALREADY repaints the whole window. Asking main.c for a
            // second full repaint doubled the work at exactly the moment the
            // user's next keystroke arrives, which is measurably enough to
            // lose it.
            return TERM_SEARCH_BAR;
        }
        // F3 with the bar shut reopens the last search, the spec's no-modifier
        // fallback. It needs no chord, so it works whatever the modifier
        // tracker knows.
        if (ev->keycode == GUI_KEY_F3) {
            term_search_open();
            if (s_query[0]) term_search_next();
            return TERM_SEARCH_REDRAW;   // next() moved the view; that IS new content
        }
        return TERM_SEARCH_PASS;
    }

    // --- the bar owns the keyboard while it is open ------------------------
    char c = ev->key_char;
    uint32_t kc = ev->keycode;

    // The open shortcut is a TOGGLE. Without this arm the chord falls through
    // to the field below and types an 'F' into the query, because with Shift
    // held the kernel does not fold Ctrl+Shift+F to a control character - it
    // delivers an ordinary capital 'F' (userland/libc/gui_mods.h documents
    // exactly this, and it is why a shortcut table may never compare
    // key_char).
    if (gui_mods_is(GUI_MOD_CTRL | GUI_MOD_SHIFT) && gui_mods_letter(ev) == 'f') {
        term_search_close();
        return TERM_SEARCH_BAR;      // apply_geometry() already repainted
    }

    if (c == 27 || kc == GUI_KEY_ESC) { term_search_close(); return TERM_SEARCH_BAR; }

    if (kc == GUI_KEY_F3) {
        if (gui_mods_is(GUI_MOD_SHIFT)) term_search_prev(); else term_search_next();
        return TERM_SEARCH_REDRAW;
    }
    if (kc == 0x1C || c == '\n' || c == '\r') {
        if (gui_mods_is(GUI_MOD_SHIFT)) term_search_prev(); else term_search_next();
        return TERM_SEARCH_REDRAW;
    }
    // Alt+C / Alt+R toggle case-sensitivity and regex from the keyboard. Alt+
    // letter is the spec's own fallback column (section 7) and collides with
    // nothing: while the bar is open no byte reaches the pty at all.
    if (gui_mods_is(GUI_MOD_ALT)) {
        int L = gui_mods_letter(ev);
        if (L == 'c') { term_search_set_case_sensitive(!s_case); return TERM_SEARCH_REDRAW; }
        if (L == 'r') { term_search_set_regex(!s_regex);         return TERM_SEARCH_REDRAW; }
    }

    // Everything else is text editing, handled by the SHARED field widget, so
    // the query box gets a caret, selection, the system clipboard and undo
    // without this file implementing any of them.
    uint32_t before = fnv1a(s_query, (int)strlen(s_query));
    if (tf_handle_key(&s_field, ev)) {
        uint32_t after = fnv1a(s_query, (int)strlen(s_query));
        // ONLY a real content change costs the whole window. A caret move, a
        // selection change or a paste of identical text costs the bar.
        if (before == after) return TERM_SEARCH_BAR;
        query_changed();
        return TERM_SEARCH_REDRAW;
    }
    // Swallow anything left over (Tab, function keys): a find bar is modal
    // over the pty, so nothing here may fall through to the shell. Nothing
    // moved, so nothing outside the bar needs repainting.
    return TERM_SEARCH_BAR;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// THE RENDER HOOK. Called from draw_row_cell() for every cell, after the SGR
// colours are fully resolved and before the glyph is drawn - the same hook
// term_select uses, for the same reason: this file owns the screen-cell ->
// match mapping, so the renderer needs no new argument and no drawing code is
// duplicated anywhere.
//
// DELIBERATE DEVIATION FROM docs/TERMINAL_KONSOLE_CHROME_SPEC.md section 5,
// which asks for a 1px accent OUTLINE around the active hit. An outline is not
// expressible from a per-cell colour hook, and expressing it would mean a
// second pass with its own copy of the cell geometry - which is exactly what
// #220 was. The active hit is distinguished by being filled with the ACCENT
// instead, which reads at a glance and costs no geometry at all.
// ---------------------------------------------------------------------------
int term_search_cell_colors(int screen_row, int col, uint32_t *fg, uint32_t *bg) {
    if (!s_open || s_nmatch == 0 || !owns()) return 0;
    int vline = gui_scroll_first_item(&term_scroll_view) + screen_row;
    for (int i = 0; i < s_nmatch; i++) {
        if (s_match[i].vline != vline) continue;
        if (col < s_match[i].col || col >= s_match[i].col + s_match[i].len) continue;
        if (i == s_cur) {
            uint32_t a = theme_color_of(g_term_theme_index, THEME_COLOR_ACCENT);
            *bg = a;
            *fg = readable_on(a);
        } else {
            *bg = hl_bg();
            *fg = hl_fg();
        }
        return 1;
    }
    return 0;
}

// Called once per term_redraw(), the same place term_select_track() is called.
// Cheap and side-effect-free unless the bar is open and the content changed.
void term_search_track(void) {
    (void)0;   // the scan is pumped from main.c's idle branch; see the header
}

static void draw_bar(void) {
    bar_rects_t r;
    bar_layout(&r);

    uint32_t bar_bg  = theme_color(THEME_COLOR_WINDOW_BG);
    uint32_t label   = theme_color(THEME_COLOR_LABEL_TEXT);
    uint32_t muted   = theme_color(THEME_COLOR_MUTED);
    uint32_t fld_bg  = theme_color(THEME_COLOR_TEXTBOX_BG);
    uint32_t fld_tx  = theme_color(THEME_COLOR_TEXTBOX_TEXT);
    uint32_t fld_br  = theme_color(THEME_COLOR_TEXTBOX_BORDER);
    uint32_t sel_bg  = theme_color(THEME_COLOR_SELECTION);

    win_draw_rect(window_handle, 0, r.y, term_px_w, r.h, bar_bg);
    gui_line(window_handle, 0, r.y, term_px_w - 1, r.y,
             theme_color(THEME_COLOR_WINDOW_BORDER));
    win_draw_text(window_handle, BAR_LABEL_X, r.y + 5, "Find:", label);

    // The query field is the SHARED widget's own renderer, so its selection
    // and caret look exactly like every other text field in the OS.
    if (s_bad_regex) fld_br = theme_color(THEME_COLOR_ERROR);
    gui_draw_textfield_tf(window_handle, r.field_x, r.field_y, r.field_w,
                          r.field_h, &s_field, 1, fld_bg, fld_tx, fld_br, sel_bg);

    gui_button(window_handle, r.case_x, r.tgl_y, r.tgl_w, r.tgl_h, "Aa",
               s_case  ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    gui_button(window_handle, r.regex_x, r.tgl_y, r.tgl_w, r.tgl_h, ".*",
               s_regex ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY, GUI_ST_NORMAL);

    char cnt[24];
    if (s_bad_regex)            str_copy(cnt, "bad regex", sizeof(cnt));
    else if (s_query[0] == 0)   str_copy(cnt, "", sizeof(cnt));
    else if (s_scan_active)     str_copy(cnt, "searching...", sizeof(cnt));
    else if (s_nmatch == 0)     str_copy(cnt, "0 / 0", sizeof(cnt));
    else {
        char a[12], b[12];
        int_to_str(term_search_match_index(), a);
        int_to_str(s_nmatch, b);
        int o = 0;
        for (int i = 0; a[i] && o < (int)sizeof(cnt) - 6; i++) cnt[o++] = a[i];
        cnt[o++] = ' '; cnt[o++] = '/'; cnt[o++] = ' ';
        for (int i = 0; b[i] && o < (int)sizeof(cnt) - 2; i++) cnt[o++] = b[i];
        if (s_truncated && o < (int)sizeof(cnt) - 1) cnt[o++] = '+';
        cnt[o] = '\0';
    }
    win_draw_text(window_handle, r.count_x, r.y + 5, cnt,
                  s_bad_regex ? theme_color(THEME_COLOR_ERROR) : muted);

    gui_button(window_handle, r.prev_x,  r.tgl_y, r.btn_w, r.tgl_h, "<",
               GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    gui_button(window_handle, r.next_x,  r.tgl_y, r.btn_w, r.tgl_h, ">",
               GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    gui_button(window_handle, r.close_x, r.tgl_y, r.btn_w, r.tgl_h, "x",
               GUI_BTN_SECONDARY, GUI_ST_NORMAL);
}

void term_search_overlay(void) {
    if (!s_open) return;
    draw_bar();
}
