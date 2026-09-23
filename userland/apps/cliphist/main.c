// main.c - MayteraOS Clipboard History (CLIPHIST)
//
// Watches the OS-wide system clipboard (#542, kernel-held: SYS_CLIP_LEN /
// SYS_CLIP_GET / SYS_CLIP_SET, one 64 KiB text store shared by every app) and
// keeps a history of everything copied, so a value that was overwritten by a
// later copy can be found and put back. Fills a real gap: the clipboard holds
// exactly ONE item and the only client in the tree is apps/cliptest, a two-
// process round-trip test, not a tool.
//
// Real data only: entries are captured from the live kernel clipboard; their
// timestamps come from sys_time() (seconds since the UNIX epoch, #113) and are
// shown in the local zone via tz_offset_minutes(); the history persists to the
// per-user preference file CLIPHIST.TXT (userconf.h, #683) and is reloaded on
// start. Nothing is seeded or invented; an empty history draws as empty.
//
// Capture is a poll of the kernel store on the event loop's 500 ms timeout
// (there is no clipboard-changed event to wait on; the timed win_get_event is
// the accepted form for a wake source outside the app's control). The UI never
// busy-waits (freeze-bug class #211/#212/#426).
//
// Keys: Up/Down select, Enter or C copy the selection back to the clipboard,
// P pin/unpin, Delete remove, Ctrl+F focus the search field, Esc leave it.

#include "syscall.h"
#include "gui.h"
#include "gui_style.h"
#include "gui_list.h"
#include "theme.h"
#include "textfield.h"
#include "userconf.h"
#include "tz.h"
#include "time.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

// ---------------------------------------------------------------------------
// Window geometry
// ---------------------------------------------------------------------------
static int g_win_w = 560, g_win_h = 560;   // live content size (EVENT_RESIZE)
#define MIN_W 420
#define MIN_H 380
#define TOOLBAR_H 44
#define PAD       14
#define ROW_H     28
#define BTN_H     26
#define PREVIEW_H 170
#define BTNROW_H  (BTN_H + PAD)

// ---------------------------------------------------------------------------
// Theme (kernel live theme table + shared style engine)
// ---------------------------------------------------------------------------
static uint32_t COL_BG, COL_TOOLBAR, COL_TEXT, COL_TEXT2, COL_SEP, COL_ACCENT;
static uint32_t COL_LIST_BG, COL_SEL, COL_SEL_TEXT, COL_CARD;
static int g_last_theme = -1;

static void apply_theme(void) {
    gui_style_sync_from_theme();
    COL_BG       = theme_color(THEME_COLOR_WINDOW_BG);
    COL_TOOLBAR  = theme_color(THEME_COLOR_BUTTON_FACE);
    COL_TEXT     = theme_color(THEME_COLOR_FOREGROUND);
    COL_TEXT2    = gui_mix(COL_TEXT, COL_BG, 96);
    COL_SEP      = theme_color(THEME_COLOR_WINDOW_BORDER);
    COL_ACCENT   = theme_color(THEME_COLOR_ACCENT);
    COL_LIST_BG  = theme_color(THEME_COLOR_TEXTBOX_BG);
    COL_SEL      = theme_color(THEME_COLOR_SELECTION);
    COL_SEL_TEXT = theme_color(THEME_COLOR_SELECTION_TEXT);
    COL_CARD     = theme_color(THEME_COLOR_BUTTON_FACE);
    if (gui_contrast_x100(COL_TEXT, COL_LIST_BG) < 450) COL_LIST_BG = COL_BG;
}

// ---------------------------------------------------------------------------
// History model
// ---------------------------------------------------------------------------
#define MAX_ENTRIES   200
#define ENTRY_CAP     (16 * 1024)   // bytes kept per entry (the store is 64 KiB)
#define CLIP_CAP      (64 * 1024)
#define HIST_FILE     "CLIPHIST.TXT"
#define HIST_LEGACY   "/CONFIG/CLIPHIST.TXT"

typedef struct {
    char *text;        // malloc'd, NUL-terminated
    int   len;         // stored bytes
    int   full_len;    // bytes the clipboard held (>= len when truncated)
    long  when;        // UNIX epoch seconds at capture
    int   pinned;
    unsigned hash;
} entry_t;

static entry_t g_ent[MAX_ENTRIES];
static int g_count = 0;              // newest first
static unsigned g_last_hash = 0;     // hash of the clipboard content last seen
static int g_last_len = -1;
static int g_capture = 1;            // capture toggle
static int g_dirty = 0;              // history changed since last save
static char *g_clipbuf;              // CLIP_CAP scratch for reads

static unsigned fnv1a(const char *s, int n) {
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

static void entry_free(entry_t *e) { free(e->text); e->text = 0; e->len = 0; }

static void remove_at(int i) {
    if (i < 0 || i >= g_count) return;
    entry_free(&g_ent[i]);
    memmove(&g_ent[i], &g_ent[i + 1], sizeof(entry_t) * (g_count - i - 1));
    g_count--;
    g_dirty = 1;
}

static void move_to_top(int i) {
    if (i <= 0 || i >= g_count) return;
    entry_t e = g_ent[i];
    memmove(&g_ent[1], &g_ent[0], sizeof(entry_t) * i);
    g_ent[0] = e;
    g_dirty = 1;
}

// Insert a captured clipboard text at the top; an identical existing entry is
// moved up rather than duplicated. Returns 1 if the list changed.
static int push_entry(const char *text, int full_len, long when, int pinned) {
    int len = full_len > ENTRY_CAP ? ENTRY_CAP : full_len;
    unsigned h = fnv1a(text, len);
    for (int i = 0; i < g_count; i++) {
        if (g_ent[i].hash == h && g_ent[i].len == len && !memcmp(g_ent[i].text, text, len)) {
            g_ent[i].when = when;
            move_to_top(i);
            return 1;
        }
    }
    if (g_count >= MAX_ENTRIES) {
        // evict the oldest unpinned entry
        int victim = -1;
        for (int i = g_count - 1; i >= 0; i--) if (!g_ent[i].pinned) { victim = i; break; }
        if (victim < 0) return 0;
        remove_at(victim);
    }
    char *copy = malloc(len + 1);
    if (!copy) return 0;
    memcpy(copy, text, len); copy[len] = 0;
    memmove(&g_ent[1], &g_ent[0], sizeof(entry_t) * g_count);
    g_ent[0].text = copy; g_ent[0].len = len; g_ent[0].full_len = full_len;
    g_ent[0].when = when; g_ent[0].pinned = pinned; g_ent[0].hash = h;
    g_count++;
    g_dirty = 1;
    return 1;
}

// One poll of the kernel clipboard. Returns 1 if the history changed.
static int poll_clipboard(void) {
    int n = clipboard_len();
    if (n <= 0) { g_last_len = n; g_last_hash = 0; return 0; }
    int got = clipboard_get(g_clipbuf, CLIP_CAP - 1);
    if (got <= 0) return 0;
    if (got > CLIP_CAP - 1) got = CLIP_CAP - 1;
    g_clipbuf[got] = 0;
    unsigned h = fnv1a(g_clipbuf, got);
    if (h == g_last_hash && got == g_last_len) return 0;
    g_last_hash = h; g_last_len = got;
    if (!g_capture) return 0;
    // Skip content that is only whitespace; that is never something to recall.
    int ws = 1; for (int i = 0; i < got; i++) if (g_clipbuf[i] > ' ') { ws = 0; break; }
    if (ws) return 0;
    return push_entry(g_clipbuf, got, sys_time(), 0);
}

// ---------------------------------------------------------------------------
// Persistence: <home>/CONFIG/CLIPHIST.TXT
//   CLIPHIST 1\n
//   E <len> <full_len> <epoch> <pinned>\n<len raw bytes>\n   (repeated)
// ---------------------------------------------------------------------------
static void hist_save(void) {
    int fd = userconf_open_write(HIST_FILE);
    if (fd < 0) return;
    char hdr[64];
    int hl = snprintf(hdr, sizeof(hdr), "CLIPHIST 1\n");
    write(fd, hdr, hl);
    for (int i = 0; i < g_count; i++) {
        entry_t *e = &g_ent[i];
        hl = snprintf(hdr, sizeof(hdr), "E %d %d %ld %d\n", e->len, e->full_len, e->when, e->pinned);
        write(fd, hdr, hl);
        write(fd, e->text, e->len);
        write(fd, "\n", 1);
    }
    close(fd);
    g_dirty = 0;
}

static void hist_load(void) {
    int fd = userconf_open_read(HIST_FILE, HIST_LEGACY);
    if (fd < 0) return;
    int cap = 1024 * 1024;
    char *buf = malloc(cap + 1);
    if (!buf) { close(fd); return; }
    int total = 0;
    while (total < cap) { long n = read(fd, buf + total, cap - total); if (n <= 0) break; total += (int)n; }
    close(fd);
    buf[total] = 0;
    char *p = buf, *end = buf + total;
    if (strncmp(p, "CLIPHIST 1\n", 11) != 0) { free(buf); return; }
    p += 11;
    // The file is newest-first; push in reverse so order is preserved.
    typedef struct { char *t; int len, full; long when; int pin; } rec_t;
    rec_t *recs = malloc(sizeof(rec_t) * MAX_ENTRIES);
    int nrec = 0;
    if (!recs) { free(buf); return; }
    while (p < end && nrec < MAX_ENTRIES) {
        if (*p != 'E' || p[1] != ' ') break;
        char *q = p + 2; char *e2;
        long len = strtol(q, &e2, 10); if (e2 == q) break; q = e2;
        long full = strtol(q, &e2, 10); if (e2 == q) break; q = e2;
        long when = strtol(q, &e2, 10); if (e2 == q) break; q = e2;
        long pin = strtol(q, &e2, 10); if (e2 == q) break; q = e2;
        if (*q != '\n') break;
        q++;
        if (len < 0 || q + len > end) break;
        recs[nrec].t = q; recs[nrec].len = (int)len; recs[nrec].full = (int)full;
        recs[nrec].when = when; recs[nrec].pin = pin ? 1 : 0; nrec++;
        p = q + len; if (p < end && *p == '\n') p++;
    }
    for (int i = nrec - 1; i >= 0; i--) {
        // push_entry stores min(full_len, ENTRY_CAP) bytes from the text; feed it
        // the stored length so a truncated record stays truncated, not extended.
        int keep_full = recs[i].full > recs[i].len ? recs[i].full : recs[i].len;
        char *t = recs[i].t; char save = t[recs[i].len]; t[recs[i].len] = 0;
        // temporarily bound ENTRY_CAP by the record length
        int len = recs[i].len;
        unsigned h = fnv1a(t, len);
        int dup = 0;
        for (int k = 0; k < g_count; k++) if (g_ent[k].hash == h && g_ent[k].len == len && !memcmp(g_ent[k].text, t, len)) { dup = 1; break; }
        if (!dup && g_count < MAX_ENTRIES) {
            char *copy = malloc(len + 1);
            if (copy) {
                memcpy(copy, t, len); copy[len] = 0;
                memmove(&g_ent[1], &g_ent[0], sizeof(entry_t) * g_count);
                g_ent[0].text = copy; g_ent[0].len = len; g_ent[0].full_len = keep_full;
                g_ent[0].when = recs[i].when; g_ent[0].pinned = recs[i].pin; g_ent[0].hash = h;
                g_count++;
            }
        }
        t[recs[i].len] = save;
    }
    free(recs);
    free(buf);
    g_dirty = 0;
}

// ---------------------------------------------------------------------------
// View state
// ---------------------------------------------------------------------------
static int g_window = -1;
static gui_list_t g_list;
static int g_sel = -1;             // index into g_view
static int g_view[MAX_ENTRIES];    // indices into g_ent matching the filter
static int g_view_count = 0;
static char g_search_buf[128];
static textfield_t g_search;
static int g_search_focus = 0;
static int g_hover_btn = -1;
static char g_status[160];

static void set_status(const char *s) { strlcpy(g_status, s, sizeof(g_status)); }

static int ci_contains(const char *hay, int hlen, const char *needle) {
    int nl = (int)strlen(needle);
    if (nl == 0) return 1;
    for (int i = 0; i + nl <= hlen; i++) {
        int k = 0;
        for (; k < nl; k++) {
            char a = hay[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (k == nl) return 1;
    }
    return 0;
}

static void rebuild_view(void) {
    int keep_ent = (g_sel >= 0 && g_sel < g_view_count) ? g_view[g_sel] : -1;
    g_view_count = 0;
    // pinned first, then the rest, both newest-first
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < g_count; i++) {
            if ((pass == 0) != (g_ent[i].pinned != 0)) continue;
            if (!ci_contains(g_ent[i].text, g_ent[i].len, g_search.buf)) continue;
            g_view[g_view_count++] = i;
        }
    g_sel = -1;
    for (int v = 0; v < g_view_count; v++) if (g_view[v] == keep_ent) { g_sel = v; break; }
    if (g_sel < 0 && g_view_count > 0) g_sel = 0;
}

static entry_t *sel_entry(void) {
    if (g_sel < 0 || g_sel >= g_view_count) return 0;
    return &g_ent[g_view[g_sel]];
}

// First line of an entry, whitespace collapsed, for the list row.
static const char *preview_of(void *ctx, int index, char *buf, int cap) {
    (void)ctx;
    if (index < 0 || index >= g_view_count) { buf[0] = 0; return buf; }
    entry_t *e = &g_ent[g_view[index]];
    int o = 0;
    if (e->pinned && o < cap - 3) { buf[o++] = '*'; buf[o++] = ' '; }
    int prev_space = 1;
    for (int i = 0; i < e->len && o < cap - 1; i++) {
        unsigned char c = (unsigned char)e->text[i];
        if (c <= ' ') { if (!prev_space) { buf[o++] = ' '; prev_space = 1; } continue; }
        buf[o++] = (char)c; prev_space = 0;
    }
    buf[o] = 0;
    return buf;
}

static void fmt_when(long when, char *out, int cap) {
    if (when <= 0) { strlcpy(out, "unknown time", cap); return; }
    long now = sys_time();
    long d = now - when;
    if (d >= 0 && d < 60) { snprintf(out, cap, "just now"); return; }
    if (d >= 60 && d < 3600) { snprintf(out, cap, "%ld min ago", d / 60); return; }
    time_t local = (time_t)(when + (long)tz_offset_minutes() * 60);
    struct tm *t = gmtime(&local);
    static const char *MON[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    if (!t) { snprintf(out, cap, "%ld", when); return; }
    if (d >= 0 && d < 86400) snprintf(out, cap, "%02d:%02d today", t->tm_hour, t->tm_min);
    else snprintf(out, cap, "%02d:%02d, %d %s %d", t->tm_hour, t->tm_min, t->tm_mday,
                  MON[(t->tm_mon >= 0 && t->tm_mon < 12) ? t->tm_mon : 0], t->tm_year + 1900);
}

static void fmt_age_short(long when, char *out, int cap) {
    if (when <= 0) { out[0] = 0; return; }
    long d = sys_time() - when;
    if (d < 0) d = 0;
    if (d < 60) snprintf(out, cap, "now");
    else if (d < 3600) snprintf(out, cap, "%ldm", d / 60);
    else if (d < 86400) snprintf(out, cap, "%ldh", d / 3600);
    else snprintf(out, cap, "%ldd", d / 86400);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
static void act_copy(void) {
    entry_t *e = sel_entry();
    if (!e) return;
    int n = clipboard_set(e->text, e->len);
    if (n < 0) { set_status("Clipboard set failed"); return; }
    g_last_hash = e->hash; g_last_len = e->len;   // do not re-capture our own write
    e->when = sys_time();
    move_to_top(g_view[g_sel]);
    rebuild_view();
    char m[96]; snprintf(m, sizeof(m), "Copied %d bytes to the clipboard", n); set_status(m);
}
static void act_pin(void) {
    entry_t *e = sel_entry();
    if (!e) return;
    e->pinned = !e->pinned; g_dirty = 1;
    rebuild_view();
    set_status(e->pinned ? "Pinned: kept at the top and never evicted" : "Unpinned");
}
static void act_delete(void) {
    if (!sel_entry()) return;
    int ent = g_view[g_sel];
    remove_at(ent);
    rebuild_view();
    set_status("Entry removed");
}
static void act_clear(void) {
    int removed = 0;
    for (int i = g_count - 1; i >= 0; i--) if (!g_ent[i].pinned) { remove_at(i); removed++; }
    rebuild_view();
    char m[96]; snprintf(m, sizeof(m), "Cleared %d unpinned entr%s", removed, removed == 1 ? "y" : "ies"); set_status(m);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
typedef struct { int x, y, w, h; const char *label; gui_btn_variant_t var; void (*fn)(void); int need_sel; } btn_t;
static btn_t g_btns[4];
static int g_toggle_x, g_toggle_y, g_toggle_w = 44, g_toggle_h = 22;

static int list_top(void) { return TOOLBAR_H + PAD; }
static int list_h(void) { int h = g_win_h - TOOLBAR_H - PAD - PREVIEW_H - BTNROW_H - PAD; return h < ROW_H * 3 ? ROW_H * 3 : h; }
static int preview_top(void) { return list_top() + list_h() + PAD; }

static void build_buttons(void) {
    int y = g_win_h - BTNROW_H + (PAD / 2) - 2;
    int x = PAD;
    static const char *labels[4] = {"Copy", "Pin", "Delete", "Clear"};
    static void (*fns[4])(void) = {act_copy, act_pin, act_delete, act_clear};
    static const gui_btn_variant_t vars[4] = {GUI_BTN_PRIMARY, GUI_BTN_SECONDARY, GUI_BTN_SECONDARY, GUI_BTN_DANGER};
    for (int i = 0; i < 4; i++) {
        int w = 70;
        g_btns[i].x = x; g_btns[i].y = y; g_btns[i].w = w; g_btns[i].h = BTN_H;
        g_btns[i].label = labels[i]; g_btns[i].var = vars[i]; g_btns[i].fn = fns[i]; g_btns[i].need_sel = (i < 3);
        x += w + 8;
    }
    entry_t *e = sel_entry();
    if (e && e->pinned) g_btns[1].label = "Unpin";
    g_toggle_x = g_win_w - PAD - g_toggle_w; g_toggle_y = y + (BTN_H - g_toggle_h) / 2;
}
static int hit_btn(int mx, int my) {
    for (int i = 0; i < 4; i++) if (mx >= g_btns[i].x && mx < g_btns[i].x + g_btns[i].w && my >= g_btns[i].y && my < g_btns[i].y + g_btns[i].h) return i;
    return -1;
}
static int hit_toggle(int mx, int my) {
    return mx >= g_toggle_x - 70 && mx < g_toggle_x + g_toggle_w && my >= g_toggle_y - 4 && my < g_toggle_y + g_toggle_h + 4;
}
static int search_x(void) { return PAD; }
static int search_w(void) { int w = g_win_w - PAD * 2 - 110; return w < 100 ? 100 : w; }

static void draw_toolbar(void) {
    win_draw_rect(g_window, 0, 0, g_win_w, TOOLBAR_H, COL_TOOLBAR);
    win_draw_rect(g_window, 0, TOOLBAR_H - 1, g_win_w, 1, COL_SEP);
    gui_textfield_tf(g_window, search_x(), (TOOLBAR_H - BTN_H) / 2, search_w(), BTN_H,
                     g_search.buf, g_search.len, g_search.cursor, g_search.sel_anchor,
                     g_search_focus != 0, "Search history");
    char cnt[64];
    if (g_search.len) snprintf(cnt, sizeof(cnt), "%d of %d", g_view_count, g_count);
    else snprintf(cnt, sizeof(cnt), "%d item%s", g_count, g_count == 1 ? "" : "s");
    int w = gui_ttf_render_width(cnt, 13);
    win_draw_text_ttf(g_window, g_win_w - PAD - w, (TOOLBAR_H - 13) / 2 - 1, cnt, 13, COL_TEXT2);
}

static void draw_list(void) {
    gui_list_config(&g_list, PAD, list_top(), g_win_w - PAD * 2, list_h(), ROW_H, g_view_count);
    gui_list_draw(g_window, &g_list, g_sel, COL_LIST_BG, COL_SEP, COL_TEXT, COL_SEL, COL_SEL_TEXT, preview_of, 0);
    if (g_view_count == 0) {
        const char *msg = g_count == 0 ? (g_capture ? "Nothing copied yet. Copy text in any app and it appears here."
                                                    : "Capture is off. Turn it on to record copies.")
                                       : "No entries match the search.";
        int w = gui_ttf_render_width(msg, 14);
        win_draw_text_ttf(g_window, PAD + (g_win_w - PAD * 2 - w) / 2, list_top() + list_h() / 2 - 8, msg, 14, COL_TEXT2);
        return;
    }
    // Right-aligned age column over the shared list rows.
    int first = gui_list_first(&g_list), span = gui_list_span(&g_list);
    int roww = gui_list_row_w(&g_list);
    for (int v = first; v < first + span && v < g_view_count; v++) {
        int ry; if (!gui_list_row_y(&g_list, v, &ry)) continue;
        char age[16]; fmt_age_short(g_ent[g_view[v]].when, age, sizeof(age));
        int w = gui_ttf_render_width(age, 12);
        uint32_t col = (v == g_sel) ? COL_SEL_TEXT : COL_TEXT2;
        win_draw_rect(g_window, PAD + roww - w - 10, ry + 1, w + 8, ROW_H - 2, (v == g_sel) ? COL_SEL : COL_LIST_BG);
        win_draw_text_ttf(g_window, PAD + roww - w - 6, ry + (ROW_H - 12) / 2 - 1, age, 12, col);
    }
}

static void draw_preview(void) {
    int y = preview_top(), h = PREVIEW_H, x = PAD, w = g_win_w - PAD * 2;
    gui_card(g_window, x, y, w, h);
    entry_t *e = sel_entry();
    if (!e) {
        win_draw_text_ttf(g_window, x + 12, y + 12, "Select an entry to preview it.", 14, COL_TEXT2);
        return;
    }
    char when[64]; fmt_when(e->when, when, sizeof(when));
    char hdr[160];
    if (e->full_len > e->len) snprintf(hdr, sizeof(hdr), "%d bytes (first %d kept), %s", e->full_len, e->len, when);
    else snprintf(hdr, sizeof(hdr), "%d byte%s, %s", e->len, e->len == 1 ? "" : "s", when);
    win_draw_text_ttf(g_window, x + 12, y + 10, hdr, 12, COL_TEXT2);
    // Body: first ~1000 bytes, wrapped by the shared wrapper.
    char body[1024];
    int n = e->len < (int)sizeof(body) - 1 ? e->len : (int)sizeof(body) - 1;
    int o = 0;
    for (int i = 0; i < n; i++) { char c = e->text[i]; body[o++] = (c == '\t') ? ' ' : (c == '\r' ? ' ' : c); }
    body[o] = 0;
    // The wrapper takes a single flow; render each source line separately so
    // newlines in the copied text stay visible.
    int line_h = 18, yy = y + 32, max_y = y + h - line_h - 6;
    char lines[6][GUI_WRAP_COL];
    char *p = body;
    while (*p && yy <= max_y) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        int cnt = gui_wrap_text_ttf(p, 14, w - 24, 6, lines);
        if (cnt == 0 && !*p) { yy += line_h; }
        for (int i = 0; i < cnt && yy <= max_y; i++) { win_draw_text_ttf(g_window, x + 12, yy, lines[i], 14, COL_TEXT); yy += line_h; }
        if (!nl) break;
        p = nl + 1;
    }
    if (yy > max_y && (*p || e->len > n)) win_draw_text_ttf(g_window, x + 12, max_y + 2, "...", 14, COL_TEXT2);
}

static void draw_buttons(void) {
    build_buttons();
    int has = sel_entry() != 0;
    for (int i = 0; i < 4; i++) {
        gui_state_t st = GUI_ST_NORMAL;
        if (g_btns[i].need_sel && !has) st = GUI_ST_DISABLED;
        else if (g_btns[i].need_sel == 0 && g_count == 0) st = GUI_ST_DISABLED;
        else if (g_hover_btn == i) st = GUI_ST_HOVER;
        gui_button(g_window, g_btns[i].x, g_btns[i].y, g_btns[i].w, g_btns[i].h, g_btns[i].label, g_btns[i].var, st);
    }
    const char *lbl = "Capture";
    int lw = gui_ttf_render_width(lbl, 13);
    win_draw_text_ttf(g_window, g_toggle_x - lw - 8, g_toggle_y + (g_toggle_h - 13) / 2 - 1, lbl, 13, COL_TEXT2);
    gui_toggle(g_window, g_toggle_x, g_toggle_y, g_toggle_w, g_toggle_h, g_capture != 0, GUI_ST_NORMAL);
    if (g_status[0]) {
        // status sits between the buttons and the toggle
        int sx = g_btns[3].x + g_btns[3].w + 12;
        int avail = g_toggle_x - lw - 16 - sx;
        if (avail > 60) {
            char st[160]; strlcpy(st, g_status, sizeof(st));
            while (st[0] && gui_ttf_render_width(st, 12) > avail) st[strlen(st) - 1] = 0;
            win_draw_text_ttf(g_window, sx, g_btns[3].y + (BTN_H - 12) / 2 - 1, st, 12, COL_TEXT2);
        }
    }
}

static void draw_all(void) {
    { int w = g_win_w, h = g_win_h;
      win_get_size(g_window, &w, &h);
      if (w >= MIN_W) g_win_w = w;
      if (h >= MIN_H) g_win_h = h; }
    win_draw_rect(g_window, 0, 0, g_win_w, g_win_h, COL_BG);
    draw_toolbar();
    draw_list();
    draw_preview();
    draw_buttons();
}

static void on_key(const gui_event_t *ev) {
    char c = ev->key_char;
    if (g_search_focus) {
        if (c == 27) { g_search_focus = 0; return; }
        if (c == '\n' || c == '\r') { g_search_focus = 0; return; }
        if (tf_handle_key(&g_search, ev)) rebuild_view();
        return;
    }
    if (c == 0x06 /* Ctrl+F */) { g_search_focus = 1; tf_select_all(&g_search); return; }
    if (c == '\n' || c == '\r' || c == 'c' || c == 'C') { act_copy(); return; }
    if (c == 'p' || c == 'P') { act_pin(); return; }
    if (ev->keycode == GUI_KEY_DEL || c == 0x7F) { act_delete(); return; }
    if (c == 27) { if (g_search.len) { tf_set_text(&g_search, ""); rebuild_view(); } return; }
    if (ev->keycode == GUI_KEY_UP)   { gui_list_move_sel(&g_list, &g_sel, -1); return; }
    if (ev->keycode == GUI_KEY_DOWN) { gui_list_move_sel(&g_list, &g_sel, 1); return; }
    if (ev->keycode == GUI_KEY_PGUP) { gui_list_move_sel(&g_list, &g_sel, -gui_list_span(&g_list)); return; }
    if (ev->keycode == GUI_KEY_PGDN) { gui_list_move_sel(&g_list, &g_sel, gui_list_span(&g_list)); return; }
    if (ev->keycode == GUI_KEY_HOME && g_view_count) { g_sel = 0; gui_scroll_reveal(&g_list.scroll, 0, ROW_H); return; }
    if (ev->keycode == GUI_KEY_END && g_view_count) { g_sel = g_view_count - 1; gui_scroll_reveal(&g_list.scroll, g_sel * ROW_H, ROW_H); return; }
}

int main(void) {
    g_clipbuf = malloc(CLIP_CAP);
    if (!g_clipbuf) { printf("cliphist: out of memory\n"); return 1; }
    tf_init(&g_search, g_search_buf, sizeof(g_search_buf));
    g_last_theme = get_theme();
    apply_theme();
    hist_load();
    // Seed the last-seen hash with whatever is on the clipboard right now so the
    // first poll records it exactly once (and not again on every later poll).
    rebuild_view();
    g_status[0] = 0;

    g_window = win_create("Clipboard History", 200, 90, g_win_w, g_win_h);
    if (g_window < 0) { printf("cliphist: failed to create window\n"); return 1; }
    gui_list_config(&g_list, PAD, list_top(), g_win_w - PAD * 2, list_h(), ROW_H, g_view_count);
    if (poll_clipboard()) rebuild_view();
    draw_all();

    gui_event_t ev;
    int running = 1;
    unsigned long last_save = uptime_ms();
    while (running) {
        { int th = get_theme();
          if (th != g_last_theme) { g_last_theme = th; apply_theme(); draw_all(); } }
        int et = win_get_event(g_window, &ev, 500);
        if (et == 0) {
            if (poll_clipboard()) { rebuild_view(); draw_all(); }
            // Autosave at most every 5 s while dirty (and on exit).
            if (g_dirty && uptime_ms() - last_save > 5000) { hist_save(); last_save = uptime_ms(); }
            continue;
        }
        switch (ev.type) {
            case EVENT_REDRAW: draw_all(); break;
            case EVENT_RESIZE:
                if (ev.mouse_x > 0 && ev.mouse_y > 0) {
                    g_win_w = ev.mouse_x; g_win_h = ev.mouse_y;
                    if (g_win_w < MIN_W) g_win_w = MIN_W;
                    if (g_win_h < MIN_H) g_win_h = MIN_H;
                }
                draw_all(); break;
            case EVENT_WINDOW_CLOSE: running = 0; break;
            case EVENT_KEY_DOWN: on_key(&ev); draw_all(); break;
            case EVENT_MOUSE_SCROLL:
                if (gui_list_wheel(&g_list, ev.mouse_x, ev.mouse_y, ev.scroll_delta)) draw_all();
                break;
            case EVENT_MOUSE_MOVE: {
                int changed = gui_list_motion(&g_list, ev.mouse_x, ev.mouse_y);
                int hb = hit_btn(ev.mouse_x, ev.mouse_y);
                if (hb != g_hover_btn) { g_hover_btn = hb; changed = 1; }
                if (changed) draw_all();
                break; }
            case EVENT_MOUSE_DOWN:
                if (ev.mouse_buttons & MOUSE_BUTTON_LEFT) {
                    if (ev.mouse_y < TOOLBAR_H) {
                        if (ev.mouse_x >= search_x() && ev.mouse_x < search_x() + search_w()) { g_search_focus = 1; tf_clear_sel(&g_search); tf_end(&g_search); }
                        else g_search_focus = 0;
                        draw_all(); break;
                    }
                    g_search_focus = 0;
                    int b = hit_btn(ev.mouse_x, ev.mouse_y);
                    if (b >= 0) {
                        int has = sel_entry() != 0;
                        if ((g_btns[b].need_sel && has) || (!g_btns[b].need_sel && g_count > 0)) g_btns[b].fn();
                        draw_all(); break;
                    }
                    if (hit_toggle(ev.mouse_x, ev.mouse_y)) {
                        g_capture = !g_capture;
                        set_status(g_capture ? "Capture on" : "Capture off: copies are not recorded");
                        draw_all(); break;
                    }
                    // Scrollbar first (drag/page), else the row hit, else -1 (miss).
                    { int row = gui_list_press(&g_list, ev.mouse_x, ev.mouse_y);
                      if (row >= 0 && row < g_view_count) g_sel = row; }
                    draw_all();
                }
                break;
            case EVENT_MOUSE_UP: gui_list_release(&g_list); break;
            default: break;
        }
    }
    if (g_dirty) hist_save();
    win_destroy(g_window);
    return 0;
}
