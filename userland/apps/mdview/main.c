// main.c - MayteraOS Markdown Viewer (MDVIEW)
//
// Renders a Markdown file as formatted, antialiased text: headings, paragraphs,
// bold/italic/inline code, fenced and indented code blocks, bullet and numbered
// lists (nested), block quotes, horizontal rules, tables (rows joined with a
// column separator) and links. Fills a real gap: the OS ships an MD4C port
// (userland/ports/md4c, MIT) and a running proof of it (apps/md4ctest), and the
// repo, help and AI outputs are all Markdown, but nothing on the image could
// SHOW a .md file as anything but raw text in Editor.
//
// Parsing is the shared MD4C push parser (md_parse with callbacks); nothing
// here re-implements CommonMark. The callbacks build a flat list of block
// items with styled inline runs; layout wraps those into positioned segments
// for the current content width, so a resize re-lays-out without re-parsing.
//
// Real data only: the document is read from the path given in argv[1] (the
// path Files hands an associated app) or typed into the path field. With no
// argument it shows its own built-in usage page (a real, static Markdown
// string, not sample data).
//
// Keys: Up/Down/PgUp/PgDn/Home/End scroll, F2 or Ctrl+O focus the path field,
// Enter in the field opens the path, Esc leaves the field, E opens the file
// in Editor. Clicking a link: a relative .md link opens that file, any other
// link is copied to the system clipboard (SYS_CLIP_SET) and the status bar
// says so.
//
// Waiting is win_get_event() with a timeout only for the theme poll; the UI
// never busy-waits (freeze-bug class #211/#212/#426).

#include "syscall.h"
#include "gui.h"
#include "gui_style.h"
#include "gui_scroll.h"
#include "theme.h"
#include "textfield.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"
#include "sys/stat.h"
#include "md4c.h"

// ---------------------------------------------------------------------------
// Window geometry
// ---------------------------------------------------------------------------
static int g_win_w = 760, g_win_h = 600;   // live content size (EVENT_RESIZE)
#define MIN_W 420
#define MIN_H 300
#define TOOLBAR_H 44
#define STATUS_H  24
#define PAD       18
#define BTN_W     70
#define BTN_H     26

// ---------------------------------------------------------------------------
// Theme palette: pulled from the kernel's live theme table (theme_color) so a
// theme switch in Settings restyles this window on its next redraw, and the
// shared style engine is synced from the same source (gui_style_sync_from_theme).
// ---------------------------------------------------------------------------
static uint32_t COL_BG, COL_TOOLBAR, COL_TEXT, COL_TEXT2, COL_SEP, COL_ACCENT;
static uint32_t COL_CODE_BG, COL_QUOTE_BAR, COL_LINK, COL_SEL;
static int g_last_theme = -1;

static void apply_theme(void) {
    gui_style_sync_from_theme();
    COL_BG        = theme_color(THEME_COLOR_WINDOW_BG);
    COL_TOOLBAR   = theme_color(THEME_COLOR_BUTTON_FACE);
    COL_TEXT      = theme_color(THEME_COLOR_FOREGROUND);
    COL_TEXT2     = gui_mix(COL_TEXT, COL_BG, 96);
    COL_SEP       = theme_color(THEME_COLOR_WINDOW_BORDER);
    COL_ACCENT    = theme_color(THEME_COLOR_ACCENT);
    COL_CODE_BG   = theme_color(THEME_COLOR_TEXTBOX_BG);
    COL_QUOTE_BAR = gui_mix(COL_ACCENT, COL_BG, 96);
    COL_LINK      = COL_ACCENT;
    COL_SEL       = theme_color(THEME_COLOR_SELECTION);
    // Ensure body text and code text clear the 4.5:1 text floor against their
    // own surface (a themed textbox_bg can be close to the window bg).
    if (gui_contrast_x100(COL_TEXT, COL_CODE_BG) < 450) COL_CODE_BG = gui_mix(COL_BG, COL_TEXT, 24);
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------
static int g_face_ui = 0;     // the desktop UI face (face 0 = default UI font)
static int g_face_mono = 0;   // a monospace face if one is installed, else UI

static void pick_fonts(void) {
    g_face_ui = font_get_ui();
    if (g_face_ui < 0) g_face_ui = 0;
    g_face_mono = g_face_ui;
    int n = font_count();
    for (int i = 0; i < n && i < 64; i++) {
        char name[64];
        int l = font_name(i, name, sizeof(name));
        if (l <= 0) continue;
        name[sizeof(name) - 1] = 0;
        if (strstr(name, "Mono") || strstr(name, "mono") || strstr(name, "Courier")) {
            g_face_mono = i;
            break;
        }
    }
}

// Every draw goes through the face-explicit call and every measure through its
// exact pair (ttf_measure_ex packs face|size|style identically), so caret-free
// layout and glyphs cannot disagree about a width.
static int text_w(const char *s, int face, int size, int style) {
    int w = ttf_measure_ex(s, face, size, style);
    return w < 0 ? 0 : w;
}
static void text_draw(int h, int x, int y, const char *s, int face, int size, int style, uint32_t col) {
    win_draw_text_ttf_ex(h, x, y, s, face, size, style, col);
}

// ---------------------------------------------------------------------------
// Document model (built by the MD4C callbacks)
// ---------------------------------------------------------------------------
#define ARENA_CAP   (512 * 1024)
#define MAX_RUNS    24000
#define MAX_ITEMS   6000
#define MAX_SEGS    40000
#define MAX_LINKS   1024
#define DOC_CAP     (1024 * 1024)

enum { RS_BOLD = 1, RS_ITALIC = 2, RS_CODE = 4, RS_LINK = 8, RS_STRIKE = 16, RS_BR = 32 };
enum { IT_P = 0, IT_H, IT_CODE, IT_HR, IT_TR, IT_BLANK };

typedef struct {
    int start, len;          // into g_arena
    unsigned char style;     // RS_* bits
    short link;              // index into g_links, or -1
} run_t;

typedef struct {
    unsigned char kind;      // IT_*
    unsigned char hlevel;    // heading level for IT_H
    unsigned char indent;    // list nesting depth
    unsigned char quote;     // blockquote depth
    unsigned char is_head;   // table header row
    char bullet[8];          // "-", "1.", "[x]" ... drawn left of the first line
    int run_first, run_count;
} item_t;

typedef struct { int start, len; } link_t;   // href text in g_arena

static char   *g_arena;
static int     g_arena_len;
static int     g_arena_parse_end;   // arena length right after md_parse; layout appends after it
static run_t  *g_runs;   static int g_run_count;
static item_t *g_items;  static int g_item_count;
static link_t *g_links;  static int g_link_count;

// Parser state
static int  g_cur_item = -1;         // item receiving text, or -1
static int  g_in_code_block = 0;
static int  g_style = 0;             // current inline style bits
static int  g_cur_link = -1;
static int  g_quote_depth = 0;
static int  g_list_depth = 0;
static int  g_list_is_ol[16];
static unsigned g_list_next[16];
static char g_pending_bullet[8];
static int  g_pending_indent = 0;
static int  g_in_table_head = 0;
static int  g_table_col = 0;
static int  g_truncated = 0;         // hit a model cap; status bar reports it

static int arena_put(const char *s, int n) {
    if (g_arena_len + n + 1 > ARENA_CAP) { g_truncated = 1; n = ARENA_CAP - g_arena_len - 1; if (n <= 0) return -1; }
    int off = g_arena_len;
    memcpy(g_arena + off, s, n);
    g_arena_len += n;
    g_arena[g_arena_len++] = 0;
    return off;
}

static int new_item(int kind) {
    if (g_item_count >= MAX_ITEMS) { g_truncated = 1; return -1; }
    item_t *it = &g_items[g_item_count];
    memset(it, 0, sizeof(*it));
    it->kind = (unsigned char)kind;
    it->quote = (unsigned char)g_quote_depth;
    it->indent = (unsigned char)g_pending_indent;
    it->run_first = g_run_count;
    if (g_pending_bullet[0]) { strlcpy(it->bullet, g_pending_bullet, sizeof(it->bullet)); g_pending_bullet[0] = 0; }
    return g_item_count++;
}

static void add_run(const char *s, int n, int style, int link) {
    if (g_cur_item < 0) {
        // Text arriving outside a leaf block (e.g. a tight list item with no
        // paragraph): open an implicit paragraph so nothing is dropped.
        g_cur_item = new_item(IT_P);
        if (g_cur_item < 0) return;
    }
    if (g_run_count >= MAX_RUNS) { g_truncated = 1; return; }
    int off = arena_put(s, n);
    if (off < 0) return;
    run_t *r = &g_runs[g_run_count++];
    r->start = off; r->len = (int)strlen(g_arena + off); r->style = (unsigned char)style; r->link = (short)link;
    g_items[g_cur_item].run_count++;
}

static void close_item(void) { g_cur_item = -1; }

static void attr_copy(const MD_ATTRIBUTE *a, char *out, int cap) {
    int n = (int)a->size; if (n > cap - 1) n = cap - 1;
    memcpy(out, a->text, n); out[n] = 0;
}

static int cb_enter_block(MD_BLOCKTYPE type, void *detail, void *ud) {
    (void)ud;
    switch (type) {
        case MD_BLOCK_DOC: break;
        case MD_BLOCK_QUOTE: g_quote_depth++; break;
        case MD_BLOCK_UL:
            if (g_list_depth < 16) { g_list_is_ol[g_list_depth] = 0; }
            g_list_depth++; break;
        case MD_BLOCK_OL: {
            MD_BLOCK_OL_DETAIL *d = (MD_BLOCK_OL_DETAIL *)detail;
            if (g_list_depth < 16) { g_list_is_ol[g_list_depth] = 1; g_list_next[g_list_depth] = d->start; }
            g_list_depth++; break; }
        case MD_BLOCK_LI: {
            MD_BLOCK_LI_DETAIL *d = (MD_BLOCK_LI_DETAIL *)detail;
            int lv = g_list_depth - 1; if (lv < 0) lv = 0; if (lv > 15) lv = 15;
            if (d->is_task) snprintf(g_pending_bullet, sizeof(g_pending_bullet), "[%c]", (d->task_mark == ' ') ? ' ' : 'x');
            else if (g_list_is_ol[lv]) snprintf(g_pending_bullet, sizeof(g_pending_bullet), "%u.", g_list_next[lv]++);
            else strlcpy(g_pending_bullet, "*", sizeof(g_pending_bullet));  // drawn as a dot (seg kind 4), not a glyph
            g_pending_indent = g_list_depth;
            break; }
        case MD_BLOCK_HR: new_item(IT_HR); break;
        case MD_BLOCK_H: {
            MD_BLOCK_H_DETAIL *d = (MD_BLOCK_H_DETAIL *)detail;
            g_cur_item = new_item(IT_H);
            if (g_cur_item >= 0) g_items[g_cur_item].hlevel = (unsigned char)d->level;
            break; }
        case MD_BLOCK_CODE: g_cur_item = new_item(IT_CODE); g_in_code_block = 1; break;
        case MD_BLOCK_HTML: g_cur_item = new_item(IT_CODE); g_in_code_block = 1; break;
        case MD_BLOCK_P: g_cur_item = new_item(IT_P); break;
        case MD_BLOCK_TABLE: break;
        case MD_BLOCK_THEAD: g_in_table_head = 1; break;
        case MD_BLOCK_TBODY: g_in_table_head = 0; break;
        case MD_BLOCK_TR:
            g_cur_item = new_item(IT_TR);
            if (g_cur_item >= 0) g_items[g_cur_item].is_head = (unsigned char)g_in_table_head;
            g_table_col = 0; break;
        case MD_BLOCK_TH: case MD_BLOCK_TD:
            if (g_table_col > 0) add_run("  |  ", 5, 0, -1);
            g_table_col++; break;
        default: break;
    }
    return 0;
}

static int cb_leave_block(MD_BLOCKTYPE type, void *detail, void *ud) {
    (void)detail; (void)ud;
    switch (type) {
        case MD_BLOCK_QUOTE: if (g_quote_depth > 0) g_quote_depth--; break;
        case MD_BLOCK_UL: case MD_BLOCK_OL:
            if (g_list_depth > 0) g_list_depth--;
            g_pending_indent = g_list_depth;
            if (g_list_depth == 0) new_item(IT_BLANK);
            break;
        case MD_BLOCK_LI: close_item(); g_pending_indent = g_list_depth; break;
        case MD_BLOCK_H: case MD_BLOCK_P: case MD_BLOCK_TR:
            close_item(); break;
        case MD_BLOCK_CODE: case MD_BLOCK_HTML:
            close_item(); g_in_code_block = 0; break;
        case MD_BLOCK_TABLE: new_item(IT_BLANK); break;
        default: break;
    }
    return 0;
}

static int cb_enter_span(MD_SPANTYPE type, void *detail, void *ud) {
    (void)ud;
    switch (type) {
        case MD_SPAN_EM: g_style |= RS_ITALIC; break;
        case MD_SPAN_STRONG: g_style |= RS_BOLD; break;
        case MD_SPAN_CODE: g_style |= RS_CODE; break;
        case MD_SPAN_DEL: g_style |= RS_STRIKE; break;
        case MD_SPAN_A: {
            MD_SPAN_A_DETAIL *d = (MD_SPAN_A_DETAIL *)detail;
            if (g_link_count < MAX_LINKS) {
                char href[512]; attr_copy(&d->href, href, sizeof(href));
                int off = arena_put(href, (int)strlen(href));
                if (off >= 0) { g_links[g_link_count].start = off; g_links[g_link_count].len = (int)strlen(href); g_cur_link = g_link_count++; }
            }
            g_style |= RS_LINK; break; }
        case MD_SPAN_IMG: {
            MD_SPAN_IMG_DETAIL *d = (MD_SPAN_IMG_DETAIL *)detail;
            char src[256]; attr_copy(&d->src, src, sizeof(src));
            char lbl[300]; snprintf(lbl, sizeof(lbl), "[image: %s] ", src);
            add_run(lbl, (int)strlen(lbl), RS_ITALIC, -1);
            break; }
        default: break;
    }
    return 0;
}

static int cb_leave_span(MD_SPANTYPE type, void *detail, void *ud) {
    (void)detail; (void)ud;
    switch (type) {
        case MD_SPAN_EM: g_style &= ~RS_ITALIC; break;
        case MD_SPAN_STRONG: g_style &= ~RS_BOLD; break;
        case MD_SPAN_CODE: g_style &= ~RS_CODE; break;
        case MD_SPAN_DEL: g_style &= ~RS_STRIKE; break;
        case MD_SPAN_A: g_style &= ~RS_LINK; g_cur_link = -1; break;
        default: break;
    }
    return 0;
}

static int cb_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *ud) {
    (void)ud;
    switch (type) {
        case MD_TEXT_NULLCHAR: break;
        case MD_TEXT_BR: add_run("", 0, RS_BR, -1); break;
        case MD_TEXT_SOFTBR:
            if (g_in_code_block) add_run("", 0, RS_BR, -1); else add_run(" ", 1, g_style, g_cur_link);
            break;
        case MD_TEXT_ENTITY: {
            const char *rep = 0;
            if      (size == 5 && !memcmp(text, "&amp;", 5))  rep = "&";
            else if (size == 4 && !memcmp(text, "&lt;", 4))   rep = "<";
            else if (size == 4 && !memcmp(text, "&gt;", 4))   rep = ">";
            else if (size == 6 && !memcmp(text, "&quot;", 6)) rep = "\"";
            else if (size == 6 && !memcmp(text, "&nbsp;", 6)) rep = " ";
            else if (size == 6 && !memcmp(text, "&copy;", 6)) rep = "(c)";
            if (rep) add_run(rep, (int)strlen(rep), g_style, g_cur_link);
            else add_run(text, (int)size, g_style, g_cur_link);
            break; }
        case MD_TEXT_CODE:
            // In a code block, keep line structure: split on '\n' into runs + breaks.
            if (g_in_code_block) {
                MD_SIZE i = 0, s = 0;
                for (; i < size; i++) if (text[i] == '\n') { add_run(text + s, (int)(i - s), RS_CODE, -1); add_run("", 0, RS_BR, -1); s = i + 1; }
                if (s < size) add_run(text + s, (int)(size - s), RS_CODE, -1);
            } else add_run(text, (int)size, g_style | RS_CODE, g_cur_link);
            break;
        case MD_TEXT_HTML:
            if (g_in_code_block) {
                MD_SIZE i = 0, s = 0;
                for (; i < size; i++) if (text[i] == '\n') { add_run(text + s, (int)(i - s), RS_CODE, -1); add_run("", 0, RS_BR, -1); s = i + 1; }
                if (s < size) add_run(text + s, (int)(size - s), RS_CODE, -1);
            }
            // inline raw HTML is dropped (a viewer, not a browser)
            break;
        default: add_run(text, (int)size, g_style, g_cur_link); break;
    }
    return 0;
}

static void model_reset(void) {
    g_arena_len = 0; g_run_count = 0; g_item_count = 0; g_link_count = 0;
    g_cur_item = -1; g_in_code_block = 0; g_style = 0; g_cur_link = -1;
    g_quote_depth = 0; g_list_depth = 0; g_pending_bullet[0] = 0; g_pending_indent = 0;
    g_in_table_head = 0; g_table_col = 0; g_truncated = 0;
}

static int parse_markdown(const char *text, int len) {
    model_reset();
    MD_PARSER p;
    memset(&p, 0, sizeof(p));
    p.abi_version = 0;
    p.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTMLSPANS;
    p.enter_block = cb_enter_block; p.leave_block = cb_leave_block;
    p.enter_span = cb_enter_span;   p.leave_span = cb_leave_span;
    p.text = cb_text;
    int rc = md_parse(text, (MD_SIZE)len, &p, 0);
    g_arena_parse_end = g_arena_len;
    return rc;
}

// ---------------------------------------------------------------------------
// Layout: items -> positioned segments for the current content width
// ---------------------------------------------------------------------------
typedef struct {
    int x, y, w;             // content coords (y before scroll offset)
    short size;              // font px size
    unsigned char style;     // RS_* bits
    unsigned char kind;      // 0 text, 1 hr, 2 code-bg band (w = band width), 3 quote bar
    short link;
    int toff;                // arena offset of a NUL-terminated string
    int line_h;
} seg_t;

static seg_t *g_segs; static int g_seg_count;
static int g_doc_h = 0;
static gui_scroll_t g_scroll;
static char g_wordbuf[1024];

static void seg_add(int x, int y, int w, int size, int style, int kind, int link, int toff, int line_h) {
    if (g_seg_count >= MAX_SEGS) { g_truncated = 1; return; }
    seg_t *s = &g_segs[g_seg_count++];
    s->x = x; s->y = y; s->w = w; s->size = (short)size; s->style = (unsigned char)style;
    s->kind = (unsigned char)kind; s->link = (short)link; s->toff = toff; s->line_h = line_h;
}

static int item_font_size(const item_t *it) {
    if (it->kind == IT_H) { static const int hs[7] = {15, 27, 23, 20, 18, 16, 15}; return hs[it->hlevel > 6 ? 6 : it->hlevel]; }
    if (it->kind == IT_CODE) return 14;
    return 15;
}

static int run_ttf_style(int style) {
    int s = FONT_STYLE_NORMAL;
    if (style & RS_BOLD) s |= FONT_STYLE_BOLD;
    if (style & RS_ITALIC) s |= FONT_STYLE_ITALIC;
    return s;
}
static int run_face(int style) { return (style & RS_CODE) ? g_face_mono : g_face_ui; }

// Lay out one item's runs as wrapped text starting at (x0, *y), width avail.
static void layout_flow(const item_t *it, int x0, int avail, int *y, int size, int base_style, int line_h) {
    int x = x0;
    for (int ri = 0; ri < it->run_count; ri++) {
        const run_t *r = &g_runs[it->run_first + ri];
        int style = r->style | base_style;
        if (r->style & RS_BR) { x = x0; *y += line_h; continue; }
        const char *p = g_arena + r->start;
        int face = run_face(style), ts = run_ttf_style(style);
        int space_w = text_w(" ", face, size, ts);
        // Split into words; a leading/trailing space in the run is significant
        // between runs, so emit spaces as their own advances.
        while (*p) {
            if (*p == ' ') { x += space_w; p++; if (x > x0 + avail) { x = x0; *y += line_h; } continue; }
            int n = 0;
            while (p[n] && p[n] != ' ' && n < (int)sizeof(g_wordbuf) - 1) n++;
            memcpy(g_wordbuf, p, n); g_wordbuf[n] = 0;
            int w = text_w(g_wordbuf, face, size, ts);
            if (x > x0 && x + w > x0 + avail) { x = x0; *y += line_h; }
            // A single word wider than the column: hard-split it by characters.
            if (w > avail) {
                int i = 0;
                while (i < n) {
                    int j = i, cw = 0;
                    while (j < n) {
                        char save = g_wordbuf[j + 1]; g_wordbuf[j + 1] = 0;
                        cw = text_w(g_wordbuf + i, face, size, ts);
                        g_wordbuf[j + 1] = save;
                        if (cw > avail && j > i) break;
                        j++;
                    }
                    int off = arena_put(p + i, j - i);
                    if (off < 0) return;
                    char tmp = g_wordbuf[j]; g_wordbuf[j] = 0;
                    int ww = text_w(g_wordbuf + i, face, size, ts);
                    g_wordbuf[j] = tmp;
                    seg_add(x, *y, ww, size, style, 0, r->link, off, line_h);
                    x = x0; *y += line_h; i = j;
                }
                x = x0; p += n; continue;
            }
            int off = arena_put(g_wordbuf, n);
            if (off < 0) return;
            seg_add(x, *y, w, size, style, 0, r->link, off, line_h);
            x += w; p += n;
        }
    }
    *y += line_h;
}

static void layout(int content_w) {
    g_seg_count = 0;
    g_arena_len = g_arena_parse_end;   // drop the previous layout's word copies
    int y = PAD;
    int left = PAD, right = content_w - PAD;
    for (int ii = 0; ii < g_item_count; ii++) {
        const item_t *it = &g_items[ii];
        int size = item_font_size(it);
        int line_h = (size * 29) / 20;
        int x0 = left + it->quote * 18 + it->indent * 22;
        int avail = right - x0;
        if (avail < 60) avail = 60;
        if (it->quote) for (int q = 0; q < it->quote; q++) seg_add(left + q * 18, y, 3, size, 0, 3, -1, 0, 0);
        switch (it->kind) {
            case IT_BLANK: y += line_h / 2; break;
            case IT_HR: seg_add(left, y + line_h / 2, right - left, size, 0, 1, -1, 0, 1); y += line_h; break;
            case IT_H: {
                if (ii > 0) y += line_h / 3;
                layout_flow(it, x0, avail, &y, size, RS_BOLD, line_h);
                if (it->hlevel <= 2) { seg_add(x0, y - 2, avail, size, 0, 1, -1, 0, 1); y += 6; }
                y += line_h / 3;
                break; }
            case IT_CODE: {
                int y_top = y; int band_idx = g_seg_count;
                seg_add(x0, y_top, avail, size, 0, 2, -1, 0, 0);   // band; height fixed up below
                y += 6;
                layout_flow(it, x0 + 10, avail - 20, &y, size, RS_CODE, line_h);
                y += 6;
                g_segs[band_idx].line_h = y - y_top;
                y += line_h / 2;
                break; }
            case IT_TR: layout_flow(it, x0, avail, &y, size, it->is_head ? RS_BOLD : 0, line_h); break;
            default: {
                if (it->bullet[0] == '*' && it->bullet[1] == 0) {
                    seg_add(x0 - 14, y, 5, size, 0, 4, -1, 0, line_h);   // unordered: a drawn dot
                } else if (it->bullet[0]) {
                    int off = arena_put(it->bullet, (int)strlen(it->bullet));
                    if (off >= 0) seg_add(x0 - 20, y, 18, size, 0, 0, -1, off, line_h);
                }
                layout_flow(it, x0, avail, &y, size, 0, line_h);
                if (!it->bullet[0] && !it->indent) y += line_h / 2;
                break; }
        }
        // Quote bars span the item's height: patch their line_h now.
        if (it->quote) {
            for (int q = g_seg_count - 1; q >= 0; q--) {
                if (g_segs[q].kind == 3 && g_segs[q].line_h == 0) g_segs[q].line_h = y - g_segs[q].y;
                else if (g_segs[q].kind == 3) break;
            }
        }
    }
    g_doc_h = y + PAD;
}

// ---------------------------------------------------------------------------
// File state
// ---------------------------------------------------------------------------
static char  g_path[512];
static char  g_field_buf[512];
static textfield_t g_field;
static int   g_field_focus = 0;
static char *g_doc;        // raw file bytes
static int   g_doc_len;
static long  g_file_size = -1;
static int   g_line_count = 0;
static char  g_status[160];
static int   g_window = -1;
static int   g_hover_link = -1;   // segment index under pointer
static int   g_hover_btn = -1;
static int   g_layout_w = -1;

static const char *USAGE_MD =
"# Markdown Viewer\n"
"\n"
"No file is open. Type a path in the field above and press **Enter**, or "
"double-click a `.md` file in Files (this app is its associated viewer).\n"
"\n"
"## Keys\n"
"\n"
"- Up / Down, PgUp / PgDn, Home / End: scroll\n"
"- F2 or Ctrl+O: focus the path field; Esc leaves it\n"
"- E: open the current file in Editor\n"
"\n"
"## Links\n"
"\n"
"Clicking a relative `.md` link opens that file. Any other link is copied to "
"the system clipboard, ready to paste into the browser's address bar.\n"
"\n"
"## What renders\n"
"\n"
"Headings, paragraphs, *emphasis*, **strong**, `inline code`, fenced and indented "
"code blocks, nested bullet and numbered lists, task lists, block quotes, "
"horizontal rules, tables and links, via the shared MD4C CommonMark parser.\n";

static void set_status(const char *s) { strlcpy(g_status, s, sizeof(g_status)); }

static void relayout_and_scroll_top(void) {
    g_layout_w = -1;   // force layout on next draw
    gui_scroll_set(&g_scroll, 0);
}

static void load_usage(void) {
    g_path[0] = 0;
    g_file_size = -1;
    g_doc_len = (int)strlen(USAGE_MD);
    if (g_doc_len > DOC_CAP - 1) g_doc_len = DOC_CAP - 1;
    memcpy(g_doc, USAGE_MD, g_doc_len); g_doc[g_doc_len] = 0;
    g_line_count = 0;
    parse_markdown(g_doc, g_doc_len);
    set_status("No file open");
    relayout_and_scroll_top();
}

static int load_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        char m[600]; snprintf(m, sizeof(m), "Cannot open %s", path); set_status(m);
        return -1;
    }
    struct stat st; long size = -1;
    if (fstat(fd, &st) == 0) size = st.st_size;
    int total = 0;
    while (total < DOC_CAP - 1) {
        long n = read(fd, g_doc + total, DOC_CAP - 1 - total);
        if (n <= 0) break;
        total += (int)n;
    }
    close(fd);
    g_doc[total] = 0;
    g_doc_len = total;
    g_file_size = size >= 0 ? size : total;
    g_line_count = 0;
    for (int i = 0; i < total; i++) if (g_doc[i] == '\n') g_line_count++;
    if (total > 0 && g_doc[total - 1] != '\n') g_line_count++;
    strlcpy(g_path, path, sizeof(g_path));
    tf_set_text(&g_field, g_path);
    parse_markdown(g_doc, g_doc_len);
    char m[600];
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    snprintf(m, sizeof(m), "%s: %ld bytes, %d lines%s%s", base, g_file_size, g_line_count,
             total >= DOC_CAP - 1 ? ", truncated at 1 MiB" : "",
             g_truncated ? ", document too large to lay out fully" : "");
    set_status(m);
    relayout_and_scroll_top();
    return 0;
}

// Resolve a relative link against the open file's directory.
static void resolve_relative(const char *href, char *out, int cap) {
    if (href[0] == '/' || g_path[0] == 0) { strlcpy(out, href, cap); return; }
    const char *slash = strrchr(g_path, '/');
    int dlen = slash ? (int)(slash - g_path) + 1 : 0;
    if (dlen >= cap) dlen = cap - 1;
    memcpy(out, g_path, dlen); out[dlen] = 0;
    strlcpy(out + dlen, href, cap - dlen);
}

static int href_is_local_md(const char *h) {
    if (strstr(h, "://") || h[0] == '#' || !strncmp(h, "mailto:", 7)) return 0;
    int n = (int)strlen(h);
    return (n > 3 && (!strcmp(h + n - 3, ".md") || !strcmp(h + n - 3, ".MD")));
}

static void follow_link(int link) {
    if (link < 0 || link >= g_link_count) return;
    const char *href = g_arena + g_links[link].start;
    if (href_is_local_md(href)) {
        char full[600]; resolve_relative(href, full, sizeof(full));
        load_file(full);
        return;
    }
    if (clipboard_set_text(href) >= 0) {
        char m[600]; snprintf(m, sizeof(m), "Link copied to clipboard: %s", href); set_status(m);
    } else set_status("Could not copy link to clipboard");
}

static void open_in_editor(void) {
    if (!g_path[0]) { set_status("No file open to edit"); return; }
    char *av[2]; av[0] = (char *)"/APPS/EDITOR"; av[1] = g_path;
    if (sys_spawn_args("/APPS/EDITOR", av, 2) < 0) set_status("Could not launch Editor");
    else set_status("Opened in Editor");
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static int content_top(void) { return TOOLBAR_H; }
static int content_h(void) { int h = g_win_h - TOOLBAR_H - STATUS_H; return h < 40 ? 40 : h; }
static int content_w(void) { return g_win_w - GUI_SCROLL_W; }

static void ensure_layout(void) {
    int cw = content_w();
    if (cw != g_layout_w) {
        layout(cw);
        g_layout_w = cw;
        gui_scroll_config(&g_scroll, g_win_w - GUI_SCROLL_W, content_top(), GUI_SCROLL_W, content_h(), g_doc_h, 22);
    } else {
        gui_scroll_config(&g_scroll, g_win_w - GUI_SCROLL_W, content_top(), GUI_SCROLL_W, content_h(), g_doc_h, 22);
    }
}

typedef struct { int x, y, w, h; const char *label; } btn_t;
static btn_t g_btns[2];

static void build_buttons(void) {
    int x = g_win_w - PAD - BTN_W;
    g_btns[1].x = x; g_btns[1].y = (TOOLBAR_H - BTN_H) / 2; g_btns[1].w = BTN_W; g_btns[1].h = BTN_H; g_btns[1].label = "Edit";
    x -= BTN_W + 8;
    g_btns[0].x = x; g_btns[0].y = (TOOLBAR_H - BTN_H) / 2; g_btns[0].w = BTN_W; g_btns[0].h = BTN_H; g_btns[0].label = "Open";
}
static int hit_btn(int mx, int my) {
    for (int i = 0; i < 2; i++) if (mx >= g_btns[i].x && mx < g_btns[i].x + g_btns[i].w && my >= g_btns[i].y && my < g_btns[i].y + g_btns[i].h) return i;
    return -1;
}
static int field_x(void) { return PAD; }
static int field_w(void) { int w = g_btns[0].x - 10 - PAD; return w < 80 ? 80 : w; }

static void draw_toolbar(void) {
    win_draw_rect(g_window, 0, 0, g_win_w, TOOLBAR_H, COL_TOOLBAR);
    win_draw_rect(g_window, 0, TOOLBAR_H - 1, g_win_w, 1, COL_SEP);
    build_buttons();
    gui_textfield_tf(g_window, field_x(), (TOOLBAR_H - BTN_H) / 2, field_w(), BTN_H,
                     g_field.buf, g_field.len, g_field.cursor, g_field.sel_anchor,
                     g_field_focus != 0, "Path to a .md file");
    for (int i = 0; i < 2; i++)
        gui_button(g_window, g_btns[i].x, g_btns[i].y, g_btns[i].w, g_btns[i].h, g_btns[i].label,
                   i == 0 ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY,
                   g_hover_btn == i ? GUI_ST_HOVER : GUI_ST_NORMAL);
}

static void draw_status(void) {
    int y = g_win_h - STATUS_H;
    win_draw_rect(g_window, 0, y, g_win_w, STATUS_H, COL_TOOLBAR);
    win_draw_rect(g_window, 0, y, g_win_w, 1, COL_SEP);
    text_draw(g_window, PAD, y + 4, g_status, g_face_ui, 13, FONT_STYLE_NORMAL, COL_TEXT2);
}

static void draw_content(void) {
    int top = content_top(), h = content_h(), cw = content_w();
    win_draw_rect(g_window, 0, top, g_win_w, h, COL_BG);
    ensure_layout();
    int off = g_scroll.offset;
    int lo = off, hi = off + h;
    // Pass 1: bands and rules (behind text). Pass 2: text.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < g_seg_count; i++) {
            const seg_t *s = &g_segs[i];
            int sy = s->y, sh = s->kind == 0 ? s->line_h : (s->line_h > 0 ? s->line_h : 1);
            if (sy + sh < lo || sy > hi) continue;
            int y = top + sy - off;
            if (pass == 0) {
                if (s->kind == 1) win_draw_rect(g_window, s->x, y, s->w, 1, COL_SEP);
                else if (s->kind == 2) {
                    int yy = y, hh = sh;
                    if (yy < top) { hh -= top - yy; yy = top; }
                    if (yy + hh > top + h) hh = top + h - yy;
                    if (hh > 0) win_draw_rect(g_window, s->x, yy, s->w, hh, COL_CODE_BG);
                } else if (s->kind == 3) {
                    int yy = y, hh = sh;
                    if (yy < top) { hh -= top - yy; yy = top; }
                    if (yy + hh > top + h) hh = top + h - yy;
                    if (hh > 0) win_draw_rect(g_window, s->x, yy, 3, hh, COL_QUOTE_BAR);
                } else if (s->kind == 4) {
                    int dy = y + s->line_h / 2 - 2;
                    if (dy >= top && dy + 5 <= top + h) win_draw_rect(g_window, s->x, dy, 5, 5, COL_TEXT);
                } else if (s->style & RS_CODE) {
                    // Inline code chip. Block code already has its band in the same
                    // colour, so a chip drawn under a band is invisible; only rows
                    // fully inside the content band are painted (no toolbar bleed).
                    if (y >= top && y + s->line_h <= top + h)
                        win_draw_rect(g_window, s->x - 2, y + 1, s->w + 4, s->line_h - 2, COL_CODE_BG);
                }
                continue;
            }
            if (s->kind != 0) continue;
            if (y + s->line_h <= top || y >= top + h) continue;
            // Clip: the TTF draw clips to the window, not to the content band, so
            // skip rows that would bleed into the toolbar or status bar.
            if (y < top || y + s->line_h > top + h) continue;
            uint32_t col = COL_TEXT;
            if (s->style & RS_LINK) col = (i == g_hover_link) ? gui_mix(COL_LINK, COL_TEXT, 128) : COL_LINK;
            text_draw(g_window, s->x, y + (s->line_h - s->size) / 2 - 1, g_arena + s->toff,
                      run_face(s->style), s->size, run_ttf_style(s->style), col);
            if (s->style & RS_LINK) win_draw_rect(g_window, s->x, y + s->line_h - 3, s->w, 1, col);
            if (s->style & RS_STRIKE) win_draw_rect(g_window, s->x, y + s->line_h / 2, s->w, 1, col);
        }
    }
    gui_scroll_draw(g_window, &g_scroll);
    (void)cw;
}

static void draw_all(void) {
    { int w = g_win_w, h = g_win_h;
      win_get_size(g_window, &w, &h);
      if (w >= MIN_W) g_win_w = w;
      if (h >= MIN_H) g_win_h = h; }
    draw_toolbar();
    draw_content();
    draw_status();
}

// Segment (link) under a content-area pointer, or -1.
static int link_at(int mx, int my) {
    int top = content_top();
    if (my < top || my >= top + content_h() || mx >= content_w()) return -1;
    int cy = my - top + g_scroll.offset;
    for (int i = 0; i < g_seg_count; i++) {
        const seg_t *s = &g_segs[i];
        if (s->kind != 0 || !(s->style & RS_LINK) || s->link < 0) continue;
        if (mx >= s->x && mx < s->x + s->w && cy >= s->y && cy < s->y + s->line_h) return i;
    }
    return -1;
}

static void submit_field(void) {
    if (g_field.len == 0) { load_usage(); return; }
    load_file(g_field.buf);
    g_field_focus = 0;
}

static void on_key(const gui_event_t *ev) {
    char c = ev->key_char;
    if (g_field_focus) {
        if (c == 27) { g_field_focus = 0; return; }
        if (c == '\n' || c == '\r') { submit_field(); return; }
        tf_handle_key(&g_field, ev);
        return;
    }
    if (ev->keycode == GUI_KEY_F2 || c == 0x0F /* Ctrl+O */) { g_field_focus = 1; tf_select_all(&g_field); return; }
    if (c == 'e' || c == 'E') { open_in_editor(); return; }
    if (gui_scroll_key(&g_scroll, ev->keycode)) return;
}

int main(int argc, char **argv) {
    g_arena = malloc(ARENA_CAP);
    g_runs  = malloc(sizeof(run_t) * MAX_RUNS);
    g_items = malloc(sizeof(item_t) * MAX_ITEMS);
    g_segs  = malloc(sizeof(seg_t) * MAX_SEGS);
    g_links = malloc(sizeof(link_t) * MAX_LINKS);
    g_doc   = malloc(DOC_CAP);
    if (!g_arena || !g_runs || !g_items || !g_segs || !g_links || !g_doc) {
        printf("mdview: out of memory\n"); return 1;
    }
    tf_init(&g_field, g_field_buf, sizeof(g_field_buf));
    pick_fonts();
    g_last_theme = get_theme();
    apply_theme();

    g_window = win_create("Markdown Viewer", 160, 80, g_win_w, g_win_h);
    if (g_window < 0) { printf("mdview: failed to create window\n"); return 1; }
    gui_scroll_config(&g_scroll, g_win_w - GUI_SCROLL_W, content_top(), GUI_SCROLL_W, content_h(), 0, 22);

    if (argc > 1 && argv[1] && argv[1][0]) {
        if (load_file(argv[1]) < 0) {
            // keep the failure message visible but show the usage page beneath
            char keep[160]; strlcpy(keep, g_status, sizeof(keep));
            load_usage(); set_status(keep);
            tf_set_text(&g_field, argv[1]);
        }
    } else load_usage();

    draw_all();
    gui_event_t ev;
    int running = 1;
    while (running) {
        { int th = get_theme();
          if (th != g_last_theme) { g_last_theme = th; apply_theme(); draw_all(); } }
        int et = win_get_event(g_window, &ev, 500);
        if (et == 0) continue;
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
                if (gui_scroll_wheel(&g_scroll, ev.scroll_delta)) draw_all();
                break;
            case EVENT_MOUSE_MOVE: {
                int changed = gui_scroll_motion(&g_scroll, ev.mouse_x, ev.mouse_y);
                int hb = hit_btn(ev.mouse_x, ev.mouse_y);
                int hl = link_at(ev.mouse_x, ev.mouse_y);
                if (hb != g_hover_btn || hl != g_hover_link) { g_hover_btn = hb; g_hover_link = hl; changed = 1; }
                if (changed) draw_all();
                break; }
            case EVENT_MOUSE_DOWN:
                if (ev.mouse_buttons & MOUSE_BUTTON_LEFT) {
                    if (gui_scroll_press(&g_scroll, ev.mouse_x, ev.mouse_y)) { draw_all(); break; }
                    int b = hit_btn(ev.mouse_x, ev.mouse_y);
                    if (b == 0) { submit_field(); draw_all(); break; }
                    if (b == 1) { open_in_editor(); draw_all(); break; }
                    if (ev.mouse_y < TOOLBAR_H && ev.mouse_x >= field_x() && ev.mouse_x < field_x() + field_w()) {
                        g_field_focus = 1; tf_clear_sel(&g_field); tf_end(&g_field); draw_all(); break;
                    }
                    if (g_field_focus) { g_field_focus = 0; }
                    int l = link_at(ev.mouse_x, ev.mouse_y);
                    if (l >= 0) follow_link(g_segs[l].link);
                    draw_all();
                }
                break;
            case EVENT_MOUSE_UP: gui_scroll_release(&g_scroll); break;
            default: break;
        }
    }
    win_destroy(g_window);
    return 0;
}
