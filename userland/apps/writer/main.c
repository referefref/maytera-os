// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// WRITER - MayteraOS native office suite word processor. Built on the shared
// office_app shell (officelib/officeui), the shared office UI toolkit
// (officelib/officeui/officetk.h: toolbar, combos, ruler, modals) and the
// shared text layout engine (officelib/layout). Opens/saves .docx/.odt
// (docx_save/odt_save) and reads legacy .doc read-only (doc_ole_load, via
// office_open_document()).
//
// CHROME (docs/OFFICE_UI_DESIGN.md 2.1): menu bar (shell) | toolbar (otoolbar,
// the WR* icon family) | ruler (oruler) | page viewport | status bar (shell).
// The page is an 8.5 x 11 in sheet at 96 ppi (816 px wide, 1 in margins),
// centred in the viewport, scrolled by gui_scroll_t.
//
// EDITING MODEL (minimal, real): a caret = (block index, character offset in
// the paragraph's concatenated run text). Click places it, Left/Right/Home/
// End/Up/Down move it, printable keys insert into the run at the caret,
// Backspace/Delete remove, Enter splits the paragraph, Backspace at offset 0
// joins it with the previous one. Every formatting control (bold / italic /
// underline / alignment / lists / font family / size / Font... / Paragraph...)
// applies to the WHOLE current paragraph: there is no selection model yet, and
// a control that applies to nothing is disabled rather than drawn live.
// Undo/Redo are disabled (no edit history exists yet; the Edit menu says so).
#include "../../libc/gui.h"
#include "../../libc/gui_menu.h"
#include "../../libc/gui_scroll.h"
#include "../../libc/gui_font.h"
#include "../../libc/theme.h"
#include "../../libc/stdio.h"
#include "../../officelib/include/officeui.h"
#include "../../officelib/include/formats.h"
#include "../../officelib/include/layout.h"
#include "../../officelib/officeui/officetk.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

// ---- Page geometry (docs/OFFICE_UI_DESIGN.md 2.1) --------------------------
#define PAGE_W_PX    816    // 8.5 in at 96 ppi
#define PAGE_H_PX    1056   // 11 in
#define PAGE_GAP     16     // viewport edge to sheet, and between sheets
#define MARGIN_PX    96     // 1 in
#define MARGIN_SM    48     // narrow viewport fallback
#define PPI          96
// textlayout.c's own inset (its LO_LEFT_PAD / LO_TOP_PAD are 16): the engine
// wraps to width_px minus 2*16 and puts its first line at (16,16). Writer
// folds that inset INTO the page margin, so text starts exactly where the
// ruler's margin marker says it does.
#define LO_PAD       16
#define LO_PARA_GAP  8      // textlayout.c's inter-block gap
#define LO_EMPTY_H   18     // LO_DEFAULT_SIZE + 6: the line the engine gives an empty paragraph
#define LO_LIST_IND  22     // textlayout.c's per-level list indent
#define SCROLL_STEP  40

enum { T_NEW = 1, T_OPEN, T_SAVE, T_UNDO, T_REDO, T_BOLD, T_ITAL, T_UNDL, T_FONT, T_SIZE,
       T_ALNL, T_ALNC, T_ALNR, T_ALNJ, T_LSTB, T_LSTN, T_TABLE, T_IMAGE };
enum { M_EDIT_UNDO = 20, M_EDIT_REDO,
       M_FMT_FONT = 30, M_FMT_PARA, M_FMT_BOLD, M_FMT_ITAL, M_FMT_UNDL,
       M_INS_TABLE = 40, M_INS_IMAGE,
       M_VIEW_RULER = 50 };

// Toolbar: docs/OFFICE_UI_DESIGN.md section 4, Writer row, WR* family only.
static const otb_item_t ITEMS[] = {
    { OTB_BUTTON, T_NEW,  "WRNEW",  "New (Ctrl+N)",   0, 0 },
    { OTB_BUTTON, T_OPEN, "WROPEN", "Open (Ctrl+O)",  0, 0 },
    { OTB_BUTTON, T_SAVE, "WRSAVE", "Save (Ctrl+S)",  0, 0 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_BUTTON, T_UNDO, "WRUNDO", "Undo (Ctrl+Z)",  0, 0 },
    { OTB_BUTTON, T_REDO, "WRREDO", "Redo (Ctrl+Y)",  0, 0 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_TOGGLE, T_BOLD, "WRBOLD", "Bold (Ctrl+B)",  0, 0 },
    { OTB_TOGGLE, T_ITAL, "WRITAL", "Italic (Ctrl+I)", 0, 0 },
    { OTB_TOGGLE, T_UNDL, "WRUNDL", "Underline (Ctrl+U)", 0, 0 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_COMBO,  T_FONT, 0, "Font", 150, 0 },
    { OTB_COMBO,  T_SIZE, 0, "Size", 56, 0 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_TOGGLE, T_ALNL, "WRALNL", "Align Left (Ctrl+L)",  0, 1 },
    { OTB_TOGGLE, T_ALNC, "WRALNC", "Center (Ctrl+E)",      0, 1 },
    { OTB_TOGGLE, T_ALNR, "WRALNR", "Align Right (Ctrl+R)", 0, 1 },
    { OTB_TOGGLE, T_ALNJ, "WRALNJ", "Justify (Ctrl+J)",     0, 1 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_TOGGLE, T_LSTB, "WRLSTB", "Bullet List",   0, 0 },
    { OTB_TOGGLE, T_LSTN, "WRLSTN", "Numbered List", 0, 0 },
    { OTB_SEPARATOR, 0, 0, 0, 0, 0 },
    { OTB_BUTTON, T_TABLE, "WRTABLE", "Insert Table...", 0, 0 },
    { OTB_BUTTON, T_IMAGE, "WRIMAGE", "Insert Image...", 0, 0 },
};
#define N_ITEMS ((int)(sizeof(ITEMS) / sizeof(ITEMS[0])))

// Menus (docs/OFFICE_UI_DESIGN.md 2.1). FORMAT/VIEW are non-const so their
// check marks can follow the current paragraph / ruler state.
static const gui_menu_item_t FILE_ITEMS[] = {
    { "New",        "Ctrl+N", OAPP_MID_FILE_NEW,     true, false },
    { "Open...",    "Ctrl+O", OAPP_MID_FILE_OPEN,    true, false },
    { "Save",       "Ctrl+S", OAPP_MID_FILE_SAVE,    true, false },
    { "Save As...", 0,        OAPP_MID_FILE_SAVE_AS, true, false },
    { 0, 0, 0, false, false },
    { "Close",      0,        OAPP_MID_FILE_CLOSE,   true, false },
};
static const gui_menu_item_t EDIT_ITEMS[] = {
    // Disabled on purpose: Writer keeps no edit history yet. The label says
    // why, because a disabled toolbar button shows no tooltip (spec 3.2).
    { "Undo (no edit history yet)", "Ctrl+Z", M_EDIT_UNDO, false, false },
    { "Redo (no edit history yet)", "Ctrl+Y", M_EDIT_REDO, false, false },
};
static gui_menu_item_t FORMAT_ITEMS[] = {
    { "Font...",      0,        M_FMT_FONT, true, false },
    { "Paragraph...", 0,        M_FMT_PARA, true, false },
    { 0, 0, 0, false, false },
    { "Bold",         "Ctrl+B", M_FMT_BOLD, true, false },
    { "Italic",       "Ctrl+I", M_FMT_ITAL, true, false },
    { "Underline",    "Ctrl+U", M_FMT_UNDL, true, false },
};
static const gui_menu_item_t INSERT_ITEMS[] = {
    { "Table...", 0, M_INS_TABLE, true, false },
    { "Image...", 0, M_INS_IMAGE, true, false },
};
static gui_menu_item_t VIEW_ITEMS[] = {
    { "Ruler", 0, M_VIEW_RULER, true, true },
};
static const gui_menu_t MENUS[] = {
    { "File", FILE_ITEMS, 6 }, { "Edit", EDIT_ITEMS, 2 }, { "Format", FORMAT_ITEMS, 6 },
    { "Insert", INSERT_ITEMS, 2 }, { "View", VIEW_ITEMS, 1 },
};

static const int SIZES[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 48, 72 };
#define NSIZES ((int)(sizeof(SIZES) / sizeof(SIZES[0])))

// ---- State -------------------------------------------------------------------
static office_app   *g_app;
static document     *g_doc;
static laid_out     *g_lo;
static gui_scroll_t  g_scroll;
static otb_toolbar_t g_tb;
static char g_path[512];
static int  g_has_path;
static int  g_readonly;
static int  g_ruler = 1;
static int  g_words;
static int  g_ticks;

// Caret: block index of the current paragraph (-1 none) + char offset within
// that paragraph's concatenated run text.
static int g_cur_blk = -1;
static int g_cur_off;

// Per-layout maps (rebuilt by relayout()): which block each span / table rect
// belongs to, and every block's vertical extent in document px, so an EMPTY
// paragraph (which the engine emits no span for) still has a caret position.
static int *g_span_blk, *g_rect_blk, *g_blk_top, *g_blk_bot;
static int  g_map_nblk, g_map_nspan, g_map_nrect;

// Font family list for the toolbar combo (deduplicated registry names).
static char g_fam[32][48];
static int  g_nfam;

// ---- Small helpers -----------------------------------------------------------
static const char *basename_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/') b = q + 1;
    return b;
}

static int has_ext_ci(const char *path, const char *ext) {
    int pl = (int)strlen(path), el = (int)strlen(ext);
    if (pl < el) return 0;
    return strcasecmp(path + pl - el, ext) == 0;
}

static doc_runfmt fmt_inherit(void) {
    doc_runfmt f = { 0, 0, 0, 0, 0, 0xFFFFFFFFu, 0 };
    return f;
}

static doc_para *cur_para(void) {
    if (!g_doc || g_cur_blk < 0 || g_cur_blk >= g_doc->nblk) return 0;
    doc_block *b = &g_doc->blocks[g_cur_blk];
    return (b->type == DBLK_PARA) ? b->para : 0;
}

static int para_len(const doc_para *p) {
    int n = 0;
    for (int i = 0; i < p->nrun; i++) if (p->runs[i].text) n += (int)strlen(p->runs[i].text);
    return n;
}

// The run holding character `idx` (0 <= idx < para_len). -1 if none.
static int para_locate_char(const doc_para *p, int idx, int *ro) {
    int acc = 0;
    for (int i = 0; i < p->nrun; i++) {
        int l = p->runs[i].text ? (int)strlen(p->runs[i].text) : 0;
        if (idx < acc + l) { *ro = idx - acc; return i; }
        acc += l;
    }
    return -1;
}

// The run to INSERT at offset `off`: the run that ENDS at the boundary wins,
// so typing after a bold word stays bold. -1 when the paragraph has no runs.
static int para_locate_insert(const doc_para *p, int off, int *ro) {
    int acc = 0;
    for (int i = 0; i < p->nrun; i++) {
        int l = p->runs[i].text ? (int)strlen(p->runs[i].text) : 0;
        if (l > 0 && off <= acc + l) { *ro = off - acc; return i; }
        acc += l;
    }
    if (p->nrun > 0) {
        int i = p->nrun - 1;
        *ro = p->runs[i].text ? (int)strlen(p->runs[i].text) : 0;
        return i;
    }
    return -1;
}

// Offset (in the paragraph's concatenated text) at which laid span `s`
// starts; -1 for a span that is not paragraph text (list bullet/number).
static int span_start_off(const doc_para *p, const laid_span *s) {
    int acc = 0;
    for (int i = 0; i < p->nrun; i++) {
        const char *t = p->runs[i].text;
        int l = t ? (int)strlen(t) : 0;
        if (t && s->text >= t && s->text <= t + l) return acc + (int)(s->text - t);
        acc += l;
    }
    return -1;
}

static int span_style_bits(const laid_span *s) {
    return (s->fmt.bold ? FONT_STYLE_BOLD : 0) | (s->fmt.italic ? FONT_STYLE_ITALIC : 0);
}

static int span_prefix_w(const laid_span *s, int n) {
    char buf[256];
    if (n <= 0) return 0;
    if (n > s->len) n = s->len;
    if (n > 255) n = 255;
    memcpy(buf, s->text, (size_t)n); buf[n] = 0;
    return ttf_measure_ex(buf, s->fmt.face, s->fmt.size, span_style_bits(s));
}

static int span_space_w(const laid_span *s) {
    int w = ttf_measure_ex(" ", s->fmt.face, s->fmt.size, span_style_bits(s));
    return w > 0 ? w : 4;
}

// Effective (style-resolved) format of a paragraph, the same reading
// textlayout.c uses: the first run with text, OR'd with the paragraph style.
static doc_runfmt para_eff_fmt(const doc_para *p) {
    doc_runfmt r = fmt_inherit(), st = fmt_inherit();
    int have_st = 0;
    if (p->style_id >= 0 && p->style_id < g_doc->nstyle) { st = g_doc->styles[p->style_id].runfmt; have_st = 1; }
    int ri = -1;
    for (int i = 0; i < p->nrun; i++) if (p->runs[i].text && p->runs[i].text[0]) { ri = i; break; }
    if (ri < 0 && p->nrun > 0) ri = 0;
    if (ri >= 0) r = p->runs[ri].fmt;
    doc_runfmt e;
    e.bold      = r.bold      || (have_st && st.bold);
    e.italic    = r.italic    || (have_st && st.italic);
    e.underline = r.underline || (have_st && st.underline);
    e.strike    = r.strike    || (have_st && st.strike);
    e.size  = r.size > 0 ? r.size : ((have_st && st.size > 0) ? st.size : 12);
    e.color = r.color != 0xFFFFFFFFu ? r.color : ((have_st && st.color != 0xFFFFFFFFu) ? st.color : 0xFFFFFFFFu);
    e.face  = r.face ? r.face : (have_st ? st.face : 0);
    return e;
}

// ---- View geometry -----------------------------------------------------------
typedef struct {
    int cx, cy, cw, ch;        // content rect (window coords)
    int page_x, page_w, margin;
    int origin_y;              // window y of document y=0 at scroll offset 0
    int scroll_x, scroll_y;    // what layout_draw() subtracts: window = doc - scroll
} view_t;

static void view_get(view_t *v) {
    oapp_content_rect(g_app, &v->cx, &v->cy, &v->cw, &v->ch);
    int avail = v->cw - GUI_SCROLL_W;
    int pw = PAGE_W_PX;
    if (pw > avail - 2 * PAGE_GAP) pw = avail - 2 * PAGE_GAP;
    if (pw < 160) pw = 160;
    v->page_w = pw;
    v->page_x = v->cx + (avail - pw) / 2;
    if (v->page_x < v->cx + 8) v->page_x = v->cx + 8;
    v->margin = pw >= 640 ? MARGIN_PX : MARGIN_SM;
    v->origin_y = v->cy + PAGE_GAP + v->margin - LO_PAD;
    v->scroll_x = -(v->page_x + v->margin - LO_PAD);
    v->scroll_y = g_scroll.offset - v->origin_y;
}

static int view_layout_w(const view_t *v) { return v->page_w - 2 * v->margin + 2 * LO_PAD; }

static int view_page_h(const view_t *v) {
    int h = g_lo ? (g_lo->height - 2 * LO_PAD + 2 * v->margin) : PAGE_H_PX;
    return h < PAGE_H_PX ? PAGE_H_PX : h;
}

// ---- Layout maps -------------------------------------------------------------
static int para_has_ptr(const doc_para *p, const char *ptr) {
    if (!p) return 0;
    for (int i = 0; i < p->nrun; i++) {
        const char *t = p->runs[i].text;
        if (t && ptr >= t && ptr <= t + strlen(t)) return 1;
    }
    return 0;
}

static int block_has_ptr(const doc_block *b, const char *ptr) {
    if (b->type == DBLK_PARA) return para_has_ptr(b->para, ptr);
    if (b->type == DBLK_TABLE && b->table && b->table->cells) {
        long n = (long)b->table->rows * b->table->cols;
        for (long i = 0; i < n; i++) if (para_has_ptr(b->table->cells[i], ptr)) return 1;
    }
    return 0;
}

static int owner_block(const char *ptr, int hint) {
    if (hint >= 0 && hint < g_doc->nblk && block_has_ptr(&g_doc->blocks[hint], ptr)) return hint;
    if (hint + 1 >= 0 && hint + 1 < g_doc->nblk && block_has_ptr(&g_doc->blocks[hint + 1], ptr)) return hint + 1;
    for (int b = 0; b < g_doc->nblk; b++) if (block_has_ptr(&g_doc->blocks[b], ptr)) return b;
    return -1;
}

static void maps_free(void) {
    free(g_span_blk); free(g_rect_blk); free(g_blk_top); free(g_blk_bot);
    g_span_blk = g_rect_blk = g_blk_top = g_blk_bot = 0;
    g_map_nblk = g_map_nspan = g_map_nrect = 0;
}

static void maps_build(void) {
    maps_free();
    if (!g_doc || !g_lo) return;
    int nb = g_doc->nblk, ns = g_lo->nspan, nr = g_lo->nrect;
    g_blk_top = (int *)malloc(sizeof(int) * (size_t)(nb > 0 ? nb : 1));
    g_blk_bot = (int *)malloc(sizeof(int) * (size_t)(nb > 0 ? nb : 1));
    g_span_blk = (int *)malloc(sizeof(int) * (size_t)(ns > 0 ? ns : 1));
    g_rect_blk = (int *)malloc(sizeof(int) * (size_t)(nr > 0 ? nr : 1));
    if (!g_blk_top || !g_blk_bot || !g_span_blk || !g_rect_blk) { maps_free(); return; }
    g_map_nblk = nb; g_map_nspan = ns; g_map_nrect = nr;

    int last = 0;
    for (int i = 0; i < ns; i++) {
        int b = owner_block(g_lo->spans[i].text, last);
        g_span_blk[i] = b;
        if (b >= 0) last = b;
    }
    // Bullet / number spans point at text outside any run: they precede
    // their line's text spans, so inherit the NEXT matched block.
    for (int i = ns - 1, nxt = -1; i >= 0; i--) {
        if (g_span_blk[i] >= 0) nxt = g_span_blk[i];
        else g_span_blk[i] = nxt;
    }
    for (int i = 0, prv = -1; i < ns; i++) {
        if (g_span_blk[i] >= 0) prv = g_span_blk[i];
        else g_span_blk[i] = prv;
    }
    // Table rects are emitted one per cell, in block order.
    int ri = 0;
    for (int b = 0; b < nb; b++) {
        const doc_block *blk = &g_doc->blocks[b];
        if (blk->type != DBLK_TABLE || !blk->table) continue;
        long n = (long)blk->table->rows * (blk->table->cols > 0 ? blk->table->cols : 1);
        for (long k = 0; k < n && ri < nr; k++) g_rect_blk[ri++] = b;
    }
    for (; ri < nr; ri++) g_rect_blk[ri] = -1;

    for (int b = 0; b < nb; b++) { g_blk_top[b] = 0x7FFFFFFF; g_blk_bot[b] = -0x7FFFFFFF; }
    for (int i = 0; i < ns; i++) {
        int b = g_span_blk[i]; if (b < 0) continue;
        const laid_span *s = &g_lo->spans[i];
        if (s->y < g_blk_top[b]) g_blk_top[b] = s->y;
        if (s->y + s->h > g_blk_bot[b]) g_blk_bot[b] = s->y + s->h;
    }
    for (int i = 0; i < nr; i++) {
        int b = g_rect_blk[i]; if (b < 0) continue;
        const laid_rect *r = &g_lo->rects[i];
        if (r->y < g_blk_top[b]) g_blk_top[b] = r->y;
        if (r->y + r->h > g_blk_bot[b]) g_blk_bot[b] = r->y + r->h;
    }
    // Blocks the engine emitted nothing for (empty paragraphs) sit exactly
    // LO_PARA_GAP below the previous block, LO_EMPTY_H tall.
    int prev_bot = LO_PAD - LO_PARA_GAP;
    for (int b = 0; b < nb; b++) {
        if (g_blk_top[b] == 0x7FFFFFFF) {
            g_blk_top[b] = prev_bot + LO_PARA_GAP;
            g_blk_bot[b] = g_blk_top[b] + LO_EMPTY_H;
        }
        prev_bot = g_blk_bot[b];
    }
}

// ---- Word count / chrome -----------------------------------------------------
static int count_words_para(const doc_para *p) {
    int n = 0, in = 0;
    if (!p) return 0;
    for (int i = 0; i < p->nrun; i++) {
        const char *t = p->runs[i].text; if (!t) continue;
        for (; *t; t++) {
            int sp = (*t == ' ' || *t == '\n' || *t == '\t');
            if (!sp && !in) n++;
            in = !sp;
        }
    }
    return n;
}

static void count_words(void) {
    g_words = 0;
    if (!g_doc) return;
    for (int b = 0; b < g_doc->nblk; b++) {
        const doc_block *blk = &g_doc->blocks[b];
        if (blk->type == DBLK_PARA) g_words += count_words_para(blk->para);
        else if (blk->type == DBLK_TABLE && blk->table && blk->table->cells) {
            long n = (long)blk->table->rows * blk->table->cols;
            for (long i = 0; i < n; i++) g_words += count_words_para(blk->table->cells[i]);
        }
    }
}

static void update_chrome(void) {
    char msg[300];
    snprintf(msg, sizeof(msg), "%s%s   |   %d word%s",
             g_has_path ? basename_of(g_path) : "Untitled",
             g_readonly ? " (read-only)" : "", g_words, g_words == 1 ? "" : "s");
    oapp_set_status(g_app, msg);
    char title[220];
    snprintf(title, sizeof(title), "WRITER - %s%s",
             g_has_path ? basename_of(g_path) : "Untitled",
             g_readonly ? " [Read-Only]" : "");
    oapp_set_title(g_app, title);
}

// Sync every toolbar / menu state from the model (the model is the truth:
// a toggle the toolkit just flipped is re-derived here after the edit).
static void sync_toolbar(void) {
    doc_para *p = cur_para();
    int en = p != 0;
    static const int fmt_ids[] = { T_BOLD, T_ITAL, T_UNDL, T_FONT, T_SIZE, T_ALNL, T_ALNC, T_ALNR, T_ALNJ, T_LSTB, T_LSTN };
    for (int i = 0; i < (int)(sizeof(fmt_ids) / sizeof(fmt_ids[0])); i++) otb_set_enabled(&g_tb, fmt_ids[i], en);
    otb_set_enabled(&g_tb, T_UNDO, 0);
    otb_set_enabled(&g_tb, T_REDO, 0);
    otb_set_enabled(&g_tb, T_TABLE, g_doc != 0);
    otb_set_enabled(&g_tb, T_IMAGE, g_doc != 0);
    for (int i = 3; i < 6; i++) FORMAT_ITEMS[i].enabled = en;
    FORMAT_ITEMS[0].enabled = en; FORMAT_ITEMS[1].enabled = en;
    VIEW_ITEMS[0].checked = g_ruler ? true : false;
    if (!p) {
        otb_set_checked(&g_tb, T_BOLD, 0); otb_set_checked(&g_tb, T_ITAL, 0); otb_set_checked(&g_tb, T_UNDL, 0);
        otb_set_checked(&g_tb, T_LSTB, 0); otb_set_checked(&g_tb, T_LSTN, 0);
        for (int i = 3; i < 6; i++) FORMAT_ITEMS[i].checked = false;
        return;
    }
    doc_runfmt e = para_eff_fmt(p);
    otb_set_checked(&g_tb, T_BOLD, e.bold);
    otb_set_checked(&g_tb, T_ITAL, e.italic);
    otb_set_checked(&g_tb, T_UNDL, e.underline);
    FORMAT_ITEMS[3].checked = e.bold ? true : false;
    FORMAT_ITEMS[4].checked = e.italic ? true : false;
    FORMAT_ITEMS[5].checked = e.underline ? true : false;
    int al = (int)p->align; if (al < 0 || al > 3) al = 0;
    otb_set_checked(&g_tb, T_ALNL + al, 1);
    otb_set_checked(&g_tb, T_LSTB, p->list_level >= 0 && !p->list_ordered);
    otb_set_checked(&g_tb, T_LSTN, p->list_level >= 0 && p->list_ordered);
    ocombo_t *cs = otb_combo(&g_tb, T_SIZE);
    if (cs) {
        int idx = -1;
        for (int i = 0; i < NSIZES; i++) if (SIZES[i] == e.size) { idx = i; break; }
        if (idx >= 0) ocombo_set_sel(cs, idx);
        else { char b[16]; snprintf(b, sizeof(b), "%d", e.size); ocombo_set_text(cs, b); }
    }
    ocombo_t *cf = otb_combo(&g_tb, T_FONT);
    if (cf) {
        char nm[48] = ""; int idx = -1;
        if (font_name(e.face, nm, sizeof(nm)) > 0)
            for (int i = 0; i < g_nfam; i++) if (!strcmp(g_fam[i], nm)) { idx = i; break; }
        if (idx >= 0) ocombo_set_sel(cf, idx);
        else ocombo_set_text(cf, nm[0] ? nm : "Default");
    }
}

// ---- Layout ------------------------------------------------------------------
static void relayout(void) {
    view_t v; view_get(&v);
    if (g_lo) { layout_free(g_lo); g_lo = 0; }
    if (g_doc) g_lo = layout_document(g_doc, view_layout_w(&v));
    maps_build();
    // The engine's default ink is THEME_COLOR_LABEL_TEXT, chosen for the
    // window ground; Writer draws the text on the page's surface_sunken paper,
    // where that ink can be unreadable (MEASURED: white on white on the
    // shipped theme). Repair every span's ink against the paper at layout
    // time, the same gui_ensure_contrast() rule the shell applies to its own
    // muted inks; an explicit document colour that already reads stays as is.
    if (g_lo) {
        uint32_t paper = theme_color(THEME_COLOR_TEXTBOX_BG);
        uint32_t label = theme_color(THEME_COLOR_LABEL_TEXT);
        uint32_t ink   = theme_color(THEME_COLOR_ON_SURFACE);
        if (!paper) paper = 0x00FFFFFF;
        if (!label) label = 0x00FFFFFF;
        if (!ink) ink = 0x00000000;
        for (int i = 0; i < g_lo->nspan; i++) {
            laid_span *s = &g_lo->spans[i];
            uint32_t c = s->fmt.color & 0xFFFFFFu;
            if (c == (label & 0xFFFFFFu)) c = ink & 0xFFFFFFu;   // the engine's default: the theme's primary ink instead
            s->fmt.color = gui_ensure_contrast(c, paper, GUI_FLOOR_TEXT);
        }
    }
    // gui_scroll_t's rect is in ABSOLUTE window coordinates (its bar is
    // drawn there), so it lives at (cx,cy): the content area starts below
    // the shell's chrome, not at 0.
    gui_scroll_config(&g_scroll, v.cx, v.cy, v.cw, v.ch, view_page_h(&v) + 2 * PAGE_GAP, SCROLL_STEP);
    count_words();
}

// ---- Caret geometry ----------------------------------------------------------
// Document-px position of the caret. Returns 0 when there is no caret.
static int caret_pos(int *x, int *y, int *h) {
    doc_para *p = cur_para();
    if (!p || !g_lo || !g_blk_top || g_map_nblk != g_doc->nblk) return 0;
    int len = para_len(p);
    if (g_cur_off > len) g_cur_off = len;
    if (g_cur_off < 0) g_cur_off = 0;
    int best = -1, best_off = -1;
    for (int i = 0; i < g_lo->nspan; i++) {
        if (g_span_blk[i] != g_cur_blk) continue;
        int so = span_start_off(p, &g_lo->spans[i]);
        if (so < 0) continue;
        if (so <= g_cur_off && so > best_off) { best = i; best_off = so; }
    }
    if (best >= 0) {
        const laid_span *s = &g_lo->spans[best];
        int rel = g_cur_off - best_off;
        if (rel <= s->len) *x = s->x + span_prefix_w(s, rel);
        else *x = s->x + s->w + (rel - s->len) * span_space_w(s);   // caret in the blanks after a word
        *y = s->y; *h = s->h;
        return 1;
    }
    // Empty paragraph (or offset 0 in a paragraph the engine gave only a
    // bullet span): synthesize the line the engine would have emitted.
    view_t v; view_get(&v);
    int lvl = p->list_level < 0 ? 0 : (p->list_level > 7 ? 7 : p->list_level);
    int indent = p->list_level >= 0 ? (lvl + 1) * LO_LIST_IND : 0;
    int cw = view_layout_w(&v) - 2 * LO_PAD - indent; if (cw < 40) cw = 40;
    *x = LO_PAD + indent;
    if (p->align == DOC_ALIGN_C) *x += cw / 2;
    else if (p->align == DOC_ALIGN_R) *x += cw;
    *y = g_blk_top[g_cur_blk]; *h = LO_EMPTY_H;
    for (int i = 0; i < g_lo->nspan; i++) {
        if (g_span_blk[i] != g_cur_blk) continue;
        *y = g_lo->spans[i].y; *h = g_lo->spans[i].h;
        if (span_start_off(p, &g_lo->spans[i]) < 0) *x = g_lo->spans[i].x + g_lo->spans[i].w;   // after the bullet
        break;
    }
    return 1;
}

// Nearest caret position to a document-px point. Returns 1 and fills
// blk/off, 0 when the document has no paragraph to land in.
static int hit_test(int dx, int dy, int *blk, int *off) {
    if (!g_doc || !g_lo || !g_blk_top || g_map_nblk != g_doc->nblk) return 0;
    long best = 0x7FFFFFFFFFFFLL; int best_span = -1, best_blk = -1;
    for (int i = 0; i < g_lo->nspan; i++) {
        int b = g_span_blk[i];
        if (b < 0 || g_doc->blocks[b].type != DBLK_PARA || !g_doc->blocks[b].para) continue;
        const laid_span *s = &g_lo->spans[i];
        int vy = dy < s->y ? s->y - dy : (dy >= s->y + s->h ? dy - (s->y + s->h) + 1 : 0);
        int vx = dx < s->x ? s->x - dx : (dx >= s->x + s->w ? dx - (s->x + s->w) + 1 : 0);
        long sc = (long)vy * 4096 + vx;
        if (sc < best) { best = sc; best_span = i; best_blk = b; }
    }
    for (int b = 0; b < g_doc->nblk; b++) {
        if (g_doc->blocks[b].type != DBLK_PARA || !g_doc->blocks[b].para) continue;
        int has_span = 0;
        for (int i = 0; i < g_lo->nspan; i++) if (g_span_blk[i] == b) { has_span = 1; break; }
        if (has_span) continue;
        int ty = g_blk_top[b], th = g_blk_bot[b] - g_blk_top[b];
        int vy = dy < ty ? ty - dy : (dy >= ty + th ? dy - (ty + th) + 1 : 0);
        long sc = (long)vy * 4096;
        if (sc < best) { best = sc; best_span = -1; best_blk = b; }
    }
    if (best_blk < 0) return 0;
    *blk = best_blk; *off = 0;
    if (best_span >= 0) {
        const laid_span *s = &g_lo->spans[best_span];
        int so = span_start_off(g_doc->blocks[best_blk].para, s);
        if (so >= 0) {
            int k = 0, bestd = 0x7FFFFFFF;
            for (int n = 0; n <= s->len; n++) {
                int px = s->x + span_prefix_w(s, n);
                int d = px > dx ? px - dx : dx - px;
                if (d < bestd) { bestd = d; k = n; }
                if (px > dx) break;
            }
            *off = so + k;
        }
    }
    return 1;
}

static void ensure_caret_visible(void) {
    int x, y, h;
    if (!caret_pos(&x, &y, &h)) return;
    view_t v; view_get(&v);
    gui_scroll_reveal(&g_scroll, (v.origin_y - v.cy) + y - 4, h + 8);
}

static void caret_set(int blk, int off) {
    g_cur_blk = blk; g_cur_off = off;
    doc_para *p = cur_para();
    if (p) { int len = para_len(p); if (g_cur_off > len) g_cur_off = len; if (g_cur_off < 0) g_cur_off = 0; }
    sync_toolbar();
}

static int first_para_block(void) {
    if (!g_doc) return -1;
    for (int b = 0; b < g_doc->nblk; b++) if (g_doc->blocks[b].type == DBLK_PARA && g_doc->blocks[b].para) return b;
    return -1;
}

// ---- Model edits -------------------------------------------------------------
static void edited(void) {
    oapp_set_dirty(g_app, 1);
    relayout();
    ensure_caret_visible();
    sync_toolbar();
    update_chrome();
}

static doc_block *insert_block_at(int idx, doc_blocktype t) {
    doc_block *b = doc_add_block(g_doc, t);
    if (!b) return 0;
    int last = g_doc->nblk - 1;
    if (idx < 0) idx = 0;
    if (idx < last) {
        doc_block tmp = g_doc->blocks[last];
        memmove(&g_doc->blocks[idx + 1], &g_doc->blocks[idx], (size_t)(last - idx) * sizeof(doc_block));
        g_doc->blocks[idx] = tmp;
    } else idx = last;
    return &g_doc->blocks[idx];
}

static doc_para *new_para_like(const doc_para *src) {
    doc_para *np = (doc_para *)calloc(1, sizeof(doc_para));
    if (!np) return 0;
    np->style_id = -1; np->list_level = -1; np->align = DOC_ALIGN_L;
    if (src) { np->align = src->align; np->list_level = src->list_level; np->list_ordered = src->list_ordered; }
    return np;
}

// Insert an empty paragraph block at idx and return its index (-1 on failure).
static int insert_para_at(int idx, const doc_para *like) {
    doc_para *np = new_para_like(like);
    if (!np) return -1;
    doc_block *b = insert_block_at(idx, DBLK_PARA);
    if (!b) { free(np); return -1; }
    b->para = np;
    return (int)(b - g_doc->blocks);
}

static void remove_para_block(int idx) {
    if (idx < 0 || idx >= g_doc->nblk || g_doc->blocks[idx].type != DBLK_PARA) return;
    doc_para *p = g_doc->blocks[idx].para;
    if (p) { for (int i = 0; i < p->nrun; i++) free(p->runs[i].text); free(p->runs); free(p); }
    memmove(&g_doc->blocks[idx], &g_doc->blocks[idx + 1], (size_t)(g_doc->nblk - idx - 1) * sizeof(doc_block));
    g_doc->nblk--;
}

static void ensure_run(doc_para *p) {
    if (p->nrun == 0) doc_para_add_run(p, "", fmt_inherit());
}

static void para_insert_text(doc_para *p, int off, const char *s) {
    ensure_run(p);
    int ro = 0, ri = para_locate_insert(p, off, &ro);
    if (ri < 0) return;
    doc_run *r = &p->runs[ri];
    int l = r->text ? (int)strlen(r->text) : 0, n = (int)strlen(s);
    if (ro > l) ro = l;
    char *nt = (char *)realloc(r->text, (size_t)(l + n + 1));
    if (!nt) return;
    if (!r->text) nt[0] = 0;
    memmove(nt + ro + n, nt + ro, (size_t)(l - ro + 1));
    memcpy(nt + ro, s, (size_t)n);
    r->text = nt;
}

static int para_delete_char(doc_para *p, int idx) {
    int ro = 0, ri = para_locate_char(p, idx, &ro);
    if (ri < 0) return 0;
    char *t = p->runs[ri].text;
    int l = (int)strlen(t);
    memmove(t + ro, t + ro + 1, (size_t)(l - ro));
    return 1;
}

// Enter: split the caret paragraph at the caret. Enter at the END of a styled
// (heading) paragraph starts a plain one, as every word processor does.
static void split_para(void) {
    doc_para *p = cur_para();
    if (!p) return;
    int len = para_len(p), off = g_cur_off;
    int ni = insert_para_at(g_cur_blk + 1, p);
    if (ni < 0) return;
    doc_para *np = g_doc->blocks[ni].para;
    p = g_doc->blocks[g_cur_blk].para;
    if (off >= len) {
        if (p->nrun) doc_para_add_run(np, "", p->runs[p->nrun - 1].fmt);
    } else {
        np->style_id = p->style_id;
        int ro = 0, ri = para_locate_char(p, off, &ro);
        if (ri >= 0) {
            doc_para_add_run(np, p->runs[ri].text + ro, p->runs[ri].fmt);
            p->runs[ri].text[ro] = 0;
            for (int k = ri + 1; k < p->nrun; k++) {
                doc_run *nr = doc_para_add_run(np, "", p->runs[k].fmt);
                if (nr) { free(nr->text); nr->text = p->runs[k].text; p->runs[k].text = 0; }
                else free(p->runs[k].text);
            }
            p->nrun = ri + 1;
        }
    }
    caret_set(ni, 0);
    edited();
}

// Backspace at offset 0: join with the previous paragraph.
static void join_prev_para(void) {
    if (g_cur_blk <= 0) return;
    doc_block *pb = &g_doc->blocks[g_cur_blk - 1];
    if (pb->type != DBLK_PARA || !pb->para) return;
    doc_para *q = pb->para, *p = cur_para();
    if (!p) return;
    int qlen = para_len(q);
    for (int k = 0; k < p->nrun; k++) {
        doc_run *nr = doc_para_add_run(q, "", p->runs[k].fmt);
        if (nr) { free(nr->text); nr->text = p->runs[k].text; p->runs[k].text = 0; }
    }
    remove_para_block(g_cur_blk);
    caret_set(g_cur_blk - 1, qlen);
    edited();
}

// Apply a run-format change to the WHOLE current paragraph. Turning a flag
// OFF that the paragraph STYLE turns on (a bold heading) detaches the style
// and copies its other attributes into the runs, because the layout ORs the
// style's flags in and a flag the model cannot express as "explicitly off"
// would otherwise look like a button that does nothing.
enum { FA_BOLD, FA_ITAL, FA_UNDL, FA_SIZE, FA_FACE };
static void para_apply(doc_para *p, int what, int val) {
    ensure_run(p);
    if (what <= FA_UNDL && !val && p->style_id >= 0 && p->style_id < g_doc->nstyle) {
        const doc_runfmt st = g_doc->styles[p->style_id].runfmt;
        int st_on = what == FA_BOLD ? st.bold : (what == FA_ITAL ? st.italic : st.underline);
        if (st_on) {
            for (int i = 0; i < p->nrun; i++) {
                doc_runfmt *f = &p->runs[i].fmt;
                f->bold |= st.bold; f->italic |= st.italic; f->underline |= st.underline; f->strike |= st.strike;
                if (f->size <= 0) f->size = st.size;
                if (f->color == 0xFFFFFFFFu) f->color = st.color;
                if (!f->face) f->face = st.face;
            }
            p->style_id = -1;
        }
    }
    for (int i = 0; i < p->nrun; i++) {
        doc_runfmt *f = &p->runs[i].fmt;
        switch (what) {
            case FA_BOLD: f->bold = val; break;
            case FA_ITAL: f->italic = val; break;
            case FA_UNDL: f->underline = val; break;
            case FA_SIZE: f->size = val; break;
            case FA_FACE: f->face = val; break;
        }
    }
}

static void cur_toggle(int what) {
    doc_para *p = cur_para();
    if (!p) return;
    doc_runfmt e = para_eff_fmt(p);
    int cur = what == FA_BOLD ? e.bold : (what == FA_ITAL ? e.italic : e.underline);
    para_apply(p, what, !cur);
    edited();
}

static void cur_set_flag(int what, int val) {
    doc_para *p = cur_para();
    if (!p) return;
    para_apply(p, what, val ? 1 : 0);
    edited();
}

static void cur_set_align(doc_align a) {
    doc_para *p = cur_para();
    if (!p) return;
    p->align = a;
    edited();
}

static void cur_set_list(int on, int ordered) {
    doc_para *p = cur_para();
    if (!p) return;
    if (on) { p->list_level = 0; p->list_ordered = ordered; }
    else p->list_level = -1;
    edited();
}

static void cur_set_size(int size) {
    doc_para *p = cur_para();
    if (!p) return;
    if (size < 4 || size > 200) { oapp_set_status(g_app, "Font size must be between 4 and 200"); sync_toolbar(); return; }
    para_apply(p, FA_SIZE, size);
    edited();
}

static void cur_set_family(const char *family) {
    doc_para *p = cur_para();
    if (!p || !family || !family[0]) return;
    int face = 0, bits = 0;
    if (gui_font_resolve(family, "Regular", &face, &bits) != 0) {
        char m[120]; snprintf(m, sizeof(m), "Unknown font family: %s", family);
        oapp_set_status(g_app, m); sync_toolbar(); return;
    }
    para_apply(p, FA_FACE, face);
    edited();
}

// ---- Document lifecycle ------------------------------------------------------
static void new_document(void) {
    if (g_doc) { doc_free(g_doc); g_doc = 0; }
    g_doc = doc_new();
    if (g_doc) doc_add_para(g_doc);      // one empty paragraph so typing works at once
    g_has_path = 0; g_readonly = 0; g_path[0] = 0;
    oapp_set_dirty(g_app, 0);
    gui_scroll_set(&g_scroll, 0);
    relayout();
    caret_set(first_para_block(), 0);
    update_chrome();
}

static void load_document(const char *path) {
    document *nd = 0; int ro = 0;
    int rc = office_open_document(path, &nd, &ro);
    if (rc != 0 || !nd) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Failed to open %s", path);
        oapp_set_status(g_app, msg);
        return;
    }
    if (g_doc) doc_free(g_doc);
    g_doc = nd;
    if (g_doc->nblk == 0) doc_add_para(g_doc);
    g_readonly = ro;
    strncpy(g_path, path, sizeof(g_path) - 1); g_path[sizeof(g_path) - 1] = 0;
    g_has_path = 1;
    oapp_set_dirty(g_app, 0);
    gui_scroll_set(&g_scroll, 0);
    relayout();
    caret_set(first_para_block(), 0);
    update_chrome();
}

static int save_document(const char *path) {
    unsigned char *out = 0; unsigned long outlen = 0;
    int rc = has_ext_ci(path, ".odt") ? odt_save(g_doc, &out, &outlen)
                                      : docx_save(g_doc, &out, &outlen);
    if (rc != 0 || !out) {
        oapp_set_status(g_app, "Save failed: could not serialize document");
        if (out) free(out);
        return 0;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        oapp_set_status(g_app, "Save failed: could not write file");
        free(out);
        return 0;
    }
    unsigned long written = 0;
    while (written < outlen) {
        long w = write(fd, out + written, (size_t)(outlen - written));
        if (w <= 0) break;
        written += (unsigned long)w;
    }
    close(fd);
    free(out);
    if (written != outlen) {
        oapp_set_status(g_app, "Save failed: short write");
        return 0;
    }
    strncpy(g_path, path, sizeof(g_path) - 1); g_path[sizeof(g_path) - 1] = 0;
    g_has_path = 1;
    g_readonly = 0;
    oapp_set_dirty(g_app, 0);
    update_chrome();
    return 1;
}

// Save (to the current path when there is a writable one) or Save As.
static int save_flow(int force_dialog) {
    if (!force_dialog && g_has_path && !g_readonly) return save_document(g_path);
    const char *path = oapp_save_dialog(g_app, g_has_path ? basename_of(g_path) : "Untitled.docx");
    if (!path) return 0;
    return save_document(path);
}

// Unsaved-changes prompt (spec 5.1 item 5). 1 = proceed with the destructive
// action, 0 = the user cancelled (or a requested Save failed / was cancelled).
static int confirm_discard(void) {
    if (!oapp_is_dirty(g_app)) return 1;
    char body[300];
    snprintf(body, sizeof(body), "Save changes to %s before closing?",
             g_has_path ? basename_of(g_path) : "Untitled.docx");
    int r = omodal_confirm3(g_app, "Unsaved changes", body, "Save", "Discard");
    if (r == OM_THIRD) return save_flow(0);
    return r == OM_OK ? 1 : 0;
}

static void file_new(void)  { if (confirm_discard()) new_document(); }
static void file_open(void) {
    if (!confirm_discard()) return;
    const char *path = oapp_open_dialog(g_app, ".docx.odt.doc");
    if (path) load_document(path);
}

// ---- Modals (spec 5.1) -------------------------------------------------------
static const char *ALIGN_OPTS[] = { "Left", "Center", "Right", "Justify" };
static const char *LIST_OPTS[]  = { "None", "Bullet", "Numbered" };

static void paragraph_modal(void) {
    doc_para *p = cur_para();
    if (!p) { oapp_set_status(g_app, "Click in a paragraph first"); return; }
    int align = (int)p->align; if (align < 0 || align > 3) align = 0;
    int list = p->list_level < 0 ? 0 : (p->list_ordered ? 2 : 1);
    // The document model (docmodel.h) has no indent / spacing fields, so those
    // rows are not offered as inputs whose values would be silently dropped.
    static char note[] = "not stored by the document model yet";
    om_field_t f[] = {
        { OM_RADIO, "Alignment", 0, 0, 0, 0, ALIGN_OPTS, 4, &align, 0, 0, 0 },
        { OM_COMBO, "List",      0, 0, 0, 0, LIST_OPTS,  3, &list,  0, 0, 0 },
        { OM_LABEL, "Spacing",   note, sizeof(note), 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    om_spec_t s = { "Paragraph", OM_SIZE_M, f, 3, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (omodal_run(g_app, &s) != OM_OK) return;
    p = cur_para(); if (!p) return;
    p->align = (doc_align)align;
    if (list == 0) p->list_level = -1; else { p->list_level = 0; p->list_ordered = (list == 2); }
    edited();
}

static int  g_tbl_rows = 3, g_tbl_cols = 3;
static char g_tbl_rows_s[8] = "3", g_tbl_cols_s[8] = "3";
static int  g_tbl_header = 1;
static void table_change(void *ctx) { (void)ctx; g_tbl_rows = atoi(g_tbl_rows_s); g_tbl_cols = atoi(g_tbl_cols_s); }
static void table_preview(int win, int x, int y, int w, int h, void *ctx) {
    (void)ctx;
    uint32_t line = theme_color(THEME_COLOR_BORDER_SUBTLE);
    uint32_t hdr  = theme_color(THEME_COLOR_SURFACE_RAISED);
    int r = g_tbl_rows < 1 ? 1 : (g_tbl_rows > 12 ? 12 : g_tbl_rows);
    int c = g_tbl_cols < 1 ? 1 : (g_tbl_cols > 12 ? 12 : g_tbl_cols);
    int gx = x + 8, gy = y + 8, gw = w - 16, gh = h - 16;
    if (g_tbl_header) win_draw_rect(win, gx, gy, gw, gh / r, hdr);
    for (int i = 0; i <= r; i++) win_draw_rect(win, gx, gy + i * gh / r, gw, 1, line);
    for (int j = 0; j <= c; j++) win_draw_rect(win, gx + j * gw / c, gy, 1, gh, line);
}

static void insert_table_modal(void) {
    if (!g_doc) return;
    snprintf(g_tbl_rows_s, sizeof(g_tbl_rows_s), "%d", g_tbl_rows);
    snprintf(g_tbl_cols_s, sizeof(g_tbl_cols_s), "%d", g_tbl_cols);
    om_field_t f[] = {
        { OM_NUMBER, "Rows",       g_tbl_rows_s, sizeof(g_tbl_rows_s), 1, 100, 0, 0, 0, 0, 0, 0 },
        { OM_NUMBER, "Columns",    g_tbl_cols_s, sizeof(g_tbl_cols_s), 1, 20,  0, 0, 0, 0, 0, 0 },
        { OM_CHECK,  "Header row", 0, 0, 0, 0, 0, 0, 0, &g_tbl_header, 0, 0 },
    };
    om_spec_t s = { "Insert Table", OM_SIZE_S, f, 3, "Insert", 0, 0, 0, 120, table_preview, table_change, 0 };
    if (omodal_run(g_app, &s) != OM_OK) return;
    table_change(0);
    int rows = g_tbl_rows < 1 ? 1 : (g_tbl_rows > 100 ? 100 : g_tbl_rows);
    int cols = g_tbl_cols < 1 ? 1 : (g_tbl_cols > 20 ? 20 : g_tbl_cols);
    doc_table *t = (doc_table *)calloc(1, sizeof(doc_table));
    if (!t) return;
    t->rows = rows; t->cols = cols;
    t->cells = (doc_para **)calloc((size_t)rows * (size_t)cols, sizeof(doc_para *));
    if (!t->cells) { free(t); return; }
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            doc_para *cp = new_para_like(0);
            if (!cp) continue;
            if (g_tbl_header && r == 0) {
                char lbl[24]; snprintf(lbl, sizeof(lbl), "Column %d", c + 1);
                doc_runfmt bf = fmt_inherit(); bf.bold = 1;
                doc_para_add_run(cp, lbl, bf);
            }
            t->cells[(long)r * cols + c] = cp;
        }
    int at = g_cur_blk >= 0 ? g_cur_blk + 1 : g_doc->nblk;
    doc_block *b = insert_block_at(at, DBLK_TABLE);
    if (!b) { free(t->cells); free(t); return; }
    b->table = t;
    int ti = (int)(b - g_doc->blocks);
    // A paragraph after the table so the caret has somewhere to continue.
    int ni = ti + 1;
    if (ni >= g_doc->nblk || g_doc->blocks[ni].type != DBLK_PARA) ni = insert_para_at(ti + 1, 0);
    caret_set(ni, 0);
    edited();
}

static unsigned char *read_whole_file(const char *path, unsigned long *n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned long cap = 65536, len = 0;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) { close(fd); return 0; }
    for (;;) {
        if (len == cap) {
            unsigned char *nb = (unsigned char *)realloc(buf, cap * 2);
            if (!nb) { free(buf); close(fd); return 0; }
            buf = nb; cap *= 2;
        }
        long r = read(fd, buf + len, (size_t)(cap - len));
        if (r <= 0) break;
        len += (unsigned long)r;
    }
    close(fd);
    *n = len;
    return buf;
}

static void image_dims(const unsigned char *b, unsigned long n, int *w, int *h) {
    *w = *h = 0;
    if (n >= 24 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
        *w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
        *h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
    } else if (n >= 26 && b[0] == 'B' && b[1] == 'M') {
        *w = (int)(b[18] | (b[19] << 8) | (b[20] << 16) | (b[21] << 24));
        *h = (int)(b[22] | (b[23] << 8) | (b[24] << 16) | (b[25] << 24));
        if (*h < 0) *h = -*h;
    }
}

static void insert_image_flow(void) {
    if (!g_doc) return;
    const char *path = oapp_open_dialog_titled(g_app, ".png.jpg.jpeg.bmp", "Insert Image");
    if (!path) return;
    unsigned long n = 0;
    unsigned char *bytes = read_whole_file(path, &n);
    if (!bytes || n == 0) { oapp_set_status(g_app, "Insert Image: could not read the file"); free(bytes); return; }
    // document.images reallocs on append (docmodel.c), so every existing
    // DBLK_IMAGE block's borrowed pointer must be re-based afterwards.
    int *idx = (int *)malloc(sizeof(int) * (size_t)(g_doc->nblk + 1));
    if (!idx) { free(bytes); return; }
    for (int b = 0; b < g_doc->nblk; b++) {
        doc_block *blk = &g_doc->blocks[b];
        idx[b] = (blk->type == DBLK_IMAGE && blk->image) ? (int)(blk->image - g_doc->images) : -1;
    }
    doc_image *im = doc_add_image(g_doc, basename_of(path), bytes, n);
    free(bytes);
    if (!im) { free(idx); oapp_set_status(g_app, "Insert Image: out of memory"); return; }
    for (int b = 0; b < g_doc->nblk; b++) if (idx[b] >= 0) g_doc->blocks[b].image = &g_doc->images[idx[b]];
    free(idx);
    image_dims(im->bytes, im->nbytes, &im->w, &im->h);
    int img_i = g_doc->nimg - 1;
    int at = g_cur_blk >= 0 ? g_cur_blk + 1 : g_doc->nblk;
    doc_block *b = insert_block_at(at, DBLK_IMAGE);
    if (!b) return;
    b->image = &g_doc->images[img_i];
    int bi = (int)(b - g_doc->blocks);
    int ni = bi + 1;
    if (ni >= g_doc->nblk || g_doc->blocks[ni].type != DBLK_PARA) ni = insert_para_at(bi + 1, 0);
    caret_set(ni, 0);
    edited();
}

// Format > Font...: the platform ChooseFont dialog (gui_font.h), applied to
// the current paragraph. The family's REGULAR face is stored with the doc's
// own bold/italic flags (so the model stays semantic and the layout's
// synthetic bold is not stacked on a real bold face).
static void font_modal(void) {
    doc_para *p = cur_para();
    if (!p) { oapp_set_status(g_app, "Click in a paragraph first"); return; }
    doc_runfmt e = para_eff_fmt(p);
    gui_font_sel_t sel; memset(&sel, 0, sizeof(sel));
    gui_font_sel_default(&sel);
    char nm[48];
    if (font_name(e.face, nm, sizeof(nm)) > 0) strncpy(sel.family, nm, sizeof(sel.family) - 1);
    strncpy(sel.style, e.bold && e.italic ? "Bold Italic" : (e.bold ? "Bold" : (e.italic ? "Italic" : "Regular")), sizeof(sel.style) - 1);
    sel.size = e.size;
    sel.title = "Font";
    if (!gui_font_dialog(&sel)) return;
    p = cur_para(); if (!p) return;
    int face = sel.face, bits = 0;
    if (gui_font_resolve(sel.family, "Regular", &face, &bits) != 0) face = sel.face;
    int bold = (sel.style_bits & FONT_STYLE_BOLD) || strstr(sel.style, "Bold") != 0;
    int ital = (sel.style_bits & FONT_STYLE_ITALIC) || strstr(sel.style, "Italic") != 0 || strstr(sel.style, "Oblique") != 0;
    para_apply(p, FA_FACE, face);
    para_apply(p, FA_SIZE, sel.size > 0 ? sel.size : 12);
    para_apply(p, FA_BOLD, bold ? 1 : 0);
    para_apply(p, FA_ITAL, ital ? 1 : 0);
    edited();
}

// ---- Drawing -----------------------------------------------------------------
static void draw(office_app *a) {
    int win = oapp_win(a);
    view_t v; view_get(&v);
    int ty, th, by, bh; oapp_chrome_rects(a, &ty, &th, &by, &bh);
    uint32_t bg     = theme_color(THEME_COLOR_WINDOW_BG);
    uint32_t paper  = theme_color(THEME_COLOR_TEXTBOX_BG);
    uint32_t border = theme_color(THEME_COLOR_WINDOW_BORDER);
    uint32_t ink    = theme_color(THEME_COLOR_ON_SURFACE);
    if (!bg) bg = 0x001E1E1E;
    if (!paper) paper = 0x00FFFFFF;
    if (!border) border = 0x00404040;
    if (!ink) ink = 0x00000000;
    win_draw_rect(win, v.cx, v.cy, v.cw, v.ch, bg);

    // The page sheet, clipped to the viewport.
    int page_h = view_page_h(&v);
    int py = v.cy + PAGE_GAP - g_scroll.offset;
    int y0 = py < v.cy ? v.cy : py, y1 = py + page_h; if (y1 > v.cy + v.ch) y1 = v.cy + v.ch;
    if (y1 > y0) {
        win_draw_rect(win, v.page_x, y0, v.page_w, y1 - y0, paper);
        win_draw_rect(win, v.page_x, y0, 1, y1 - y0, border);
        win_draw_rect(win, v.page_x + v.page_w - 1, y0, 1, y1 - y0, border);
        if (py >= v.cy) win_draw_rect(win, v.page_x, py, v.page_w, 1, border);
        if (py + page_h - 1 < v.cy + v.ch) win_draw_rect(win, v.page_x, py + page_h - 1, v.page_w, 1, border);
    }
    if (g_lo) layout_draw(win, g_lo, v.scroll_x, v.scroll_y, v.cy + v.ch);

    // Caret: 1 px on_surface, solid (spec 2.1).
    int kx, ky, kh;
    if (caret_pos(&kx, &ky, &kh)) {
        int wx = kx - v.scroll_x, wy = ky - v.scroll_y;
        int top = wy < v.cy ? v.cy : wy, bot = wy + kh; if (bot > v.cy + v.ch) bot = v.cy + v.ch;
        if (bot > top && wx >= v.page_x && wx < v.page_x + v.page_w) win_draw_rect(win, wx, top, 1, bot - top, ink);
    }
    gui_scroll_draw(win, &g_scroll);
    // The ruler last: it owns the chrome band above the viewport and covers
    // any span the layout clipped only against the viewport bottom.
    if (g_ruler && th > 0) oruler_draw(win, 0, ty, v.cw, v.page_x, v.page_w, v.margin, v.margin, PPI);
}

// ---- Input -------------------------------------------------------------------
static void caret_move_h(int delta) {
    doc_para *p = cur_para();
    if (!p) return;
    int len = para_len(p);
    int off = g_cur_off + delta;
    if (off < 0) {
        for (int b = g_cur_blk - 1; b >= 0; b--)
            if (g_doc->blocks[b].type == DBLK_PARA && g_doc->blocks[b].para) { caret_set(b, para_len(g_doc->blocks[b].para)); break; }
    } else if (off > len) {
        for (int b = g_cur_blk + 1; b < g_doc->nblk; b++)
            if (g_doc->blocks[b].type == DBLK_PARA && g_doc->blocks[b].para) { caret_set(b, 0); break; }
    } else caret_set(g_cur_blk, off);
    ensure_caret_visible();
}

static void caret_move_v(int dir) {
    int x, y, h;
    if (!caret_pos(&x, &y, &h)) return;
    int ty = dir < 0 ? y - LO_PARA_GAP - 2 : y + h + LO_PARA_GAP + 2;
    int blk, off;
    if (hit_test(x, ty, &blk, &off)) caret_set(blk, off);
    ensure_caret_visible();
}

static void type_char(char c) {
    doc_para *p = cur_para();
    if (!p) return;
    char s[2] = { c, 0 };
    para_insert_text(p, g_cur_off, s);
    g_cur_off++;
    edited();
}

static void do_backspace(void) {
    doc_para *p = cur_para();
    if (!p) return;
    if (g_cur_off <= 0) { join_prev_para(); return; }
    if (para_delete_char(p, g_cur_off - 1)) { g_cur_off--; edited(); }
}

static void do_delete(void) {
    doc_para *p = cur_para();
    if (!p) return;
    if (g_cur_off >= para_len(p)) {
        // Delete at the end joins the NEXT paragraph into this one.
        int nb = g_cur_blk + 1;
        if (nb < g_doc->nblk && g_doc->blocks[nb].type == DBLK_PARA && g_doc->blocks[nb].para) {
            int keep = g_cur_off;
            caret_set(nb, 0); join_prev_para(); g_cur_off = keep;
        }
        return;
    }
    if (para_delete_char(p, g_cur_off)) edited();
}

static int on_key(int keycode, char c) {
    gui_event_t ev; memset(&ev, 0, sizeof(ev));
    ev.type = EVENT_KEY_DOWN; ev.keycode = (uint32_t)keycode; ev.key_char = c;
    unsigned mods = gui_mods_get();
    if (mods & GUI_MOD_CTRL) {
        int L = gui_mods_letter(&ev);
        switch (L) {
            case 'n': file_new(); break;
            case 'o': file_open(); break;
            case 's': save_flow(0); break;
            case 'b': cur_toggle(FA_BOLD); break;
            case 'i': cur_toggle(FA_ITAL); break;
            case 'u': cur_toggle(FA_UNDL); break;
            case 'l': cur_set_align(DOC_ALIGN_L); break;
            case 'e': cur_set_align(DOC_ALIGN_C); break;
            case 'r': cur_set_align(DOC_ALIGN_R); break;
            case 'j': cur_set_align(DOC_ALIGN_J); break;
            case 'z': case 'y': oapp_set_status(g_app, "Undo/Redo are not available: Writer keeps no edit history yet"); break;
            default: break;
        }
        return 1;
    }
    if (mods & GUI_MOD_ALT) return 1;
    switch ((unsigned)keycode) {
        case GUI_KEY_LEFT:  caret_move_h(-1); return 1;
        case GUI_KEY_RIGHT: caret_move_h(1);  return 1;
        case GUI_KEY_UP:    caret_move_v(-1); return 1;
        case GUI_KEY_DOWN:  caret_move_v(1);  return 1;
        case GUI_KEY_HOME:  caret_set(g_cur_blk, 0); ensure_caret_visible(); return 1;
        case GUI_KEY_END:   { doc_para *p = cur_para(); if (p) caret_set(g_cur_blk, para_len(p)); ensure_caret_visible(); return 1; }
        case GUI_KEY_PGUP: case GUI_KEY_PGDN: gui_scroll_key(&g_scroll, (uint32_t)keycode); return 1;
        case GUI_KEY_DEL:   do_delete(); return 1;
        default: break;
    }
    if (c == GUI_KEY_ENTER || keycode == GUI_KEY_ENTER) { split_para(); return 1; }
    if (c == GUI_KEY_BKSP || keycode == 0x0E) { do_backspace(); return 1; }
    if (c == GUI_KEY_TAB) { type_char(' '); type_char(' '); type_char(' '); type_char(' '); return 1; }
    if (c == GUI_KEY_ESC) return 1;
    if ((unsigned char)c >= 32 && (unsigned char)c < 127) { type_char(c); return 1; }
    return 1;
}

static int on_tool(int id, int value) {
    switch (id) {
        case T_NEW:   file_new(); break;
        case T_OPEN:  file_open(); break;
        case T_SAVE:  save_flow(0); break;
        case T_UNDO: case T_REDO: break;   // disabled; never delivered
        case T_BOLD:  cur_set_flag(FA_BOLD, value); break;
        case T_ITAL:  cur_set_flag(FA_ITAL, value); break;
        case T_UNDL:  cur_set_flag(FA_UNDL, value); break;
        case T_ALNL:  cur_set_align(DOC_ALIGN_L); break;
        case T_ALNC:  cur_set_align(DOC_ALIGN_C); break;
        case T_ALNR:  cur_set_align(DOC_ALIGN_R); break;
        case T_ALNJ:  cur_set_align(DOC_ALIGN_J); break;
        case T_LSTB:  cur_set_list(value, 0); break;
        case T_LSTN:  cur_set_list(value, 1); break;
        case T_FONT: {
            ocombo_t *c = otb_combo(&g_tb, T_FONT);
            if (value >= 0 && value < g_nfam) cur_set_family(g_fam[value]);
            else if (c) cur_set_family(c->text);
            break;
        }
        case T_SIZE: {
            ocombo_t *c = otb_combo(&g_tb, T_SIZE);
            if (value >= 0 && value < NSIZES) cur_set_size(SIZES[value]);
            else if (c) cur_set_size(atoi(c->text));
            break;
        }
        case T_TABLE: insert_table_modal(); break;
        case T_IMAGE: insert_image_flow(); break;
        default: break;
    }
    return 1;
}

static int on_menu(int id) {
    switch (id) {
        case M_EDIT_UNDO: case M_EDIT_REDO: break;   // disabled items
        case M_FMT_FONT:  font_modal(); break;
        case M_FMT_PARA:  paragraph_modal(); break;
        case M_FMT_BOLD:  cur_toggle(FA_BOLD); break;
        case M_FMT_ITAL:  cur_toggle(FA_ITAL); break;
        case M_FMT_UNDL:  cur_toggle(FA_UNDL); break;
        case M_INS_TABLE: insert_table_modal(); break;
        case M_INS_IMAGE: insert_image_flow(); break;
        case M_VIEW_RULER:
            g_ruler = !g_ruler;
            oapp_set_chrome(g_app, g_ruler ? ORULER_H : 0, 0);
            relayout(); sync_toolbar();
            break;
        default: break;
    }
    return 1;
}

static int on_event(office_app *a, int type, int p1, int p2) {
    (void)a;
    switch (type) {
        case OAPP_EVENT_TOOL: return on_tool(p1, p2);
        case OAPP_EVENT_MENU: return on_menu(p1);
        case OAPP_EVENT_FILE_NEW:  file_new(); return 1;
        case OAPP_EVENT_FILE_OPEN: file_open(); return 1;
        case OAPP_EVENT_FILE_SAVE: save_flow(0); return 1;
        case OAPP_EVENT_FILE_SAVE_AS: save_flow(1); return 1;
        case OAPP_EVENT_FILE_CLOSE: if (confirm_discard()) new_document(); return 1;
        case OAPP_EVENT_TICK:
            g_ticks++;
#ifdef WRITER_VERIFY_AUTOMODAL
            // THROWAWAY VERIFICATION BUILD ONLY (compile-gated, never in a
            // normal make): opens the Insert Table modal by itself after N
            // ticks so a headless screendump can show it (#334/#440).
            if (g_ticks == WRITER_VERIFY_AUTOMODAL) insert_table_modal();
#endif
            return 1;
        case EVENT_WINDOW_CLOSE:
            return confirm_discard() ? 0 : 1;
        case EVENT_RESIZE:
            relayout();
            ensure_caret_visible();
            return 1;
        case EVENT_MOUSE_SCROLL:
            // (a,b) = (scroll_delta, mouse_y) - see officeui.h's mapping table.
            gui_scroll_wheel(&g_scroll, p1);
            return 1;
        case EVENT_KEY_DOWN:
            return on_key(p1, (char)p2);
        case EVENT_MOUSE_DOWN: {
            if (gui_scroll_press(&g_scroll, p1, p2)) return 1;
            view_t v; view_get(&v);
            if (p1 >= v.cx && p1 < v.cx + v.cw - GUI_SCROLL_W && p2 >= v.cy && p2 < v.cy + v.ch) {
                int blk, off;
                if (hit_test(p1 + v.scroll_x, p2 + v.scroll_y, &blk, &off)) caret_set(blk, off);
            }
            return 1;
        }
        case EVENT_MOUSE_MOVE:
            gui_scroll_motion(&g_scroll, p1, p2);
            return 1;
        case EVENT_MOUSE_UP:
            gui_scroll_release(&g_scroll);
            return 1;
        default:
            return 1;
    }
}

// ---- Combos ------------------------------------------------------------------
static void build_families(void) {
    int n = font_count(); if (n > 64) n = 64;
    for (int i = 0; i < n && g_nfam < 32; i++) {
        char nm[48];
        if (font_name(i, nm, sizeof(nm)) <= 0) continue;
        int dup = 0;
        for (int k = 0; k < g_nfam; k++) if (!strcmp(g_fam[k], nm)) { dup = 1; break; }
        if (!dup) { strncpy(g_fam[g_nfam], nm, 47); g_fam[g_nfam][47] = 0; g_nfam++; }
    }
    if (!g_nfam) { strncpy(g_fam[0], "DejaVu Sans", 47); g_nfam = 1; }
}
static const char *fam_label(void *ctx, int i, char *buf, int cap) { (void)ctx; (void)buf; (void)cap; return i >= 0 && i < g_nfam ? g_fam[i] : ""; }
static const char *size_label(void *ctx, int i, char *buf, int cap) { (void)ctx; if (i < 0 || i >= NSIZES) return ""; snprintf(buf, cap, "%d", SIZES[i]); return buf; }

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    g_app = oapp_create("WRITER", 900, 640);
    if (!g_app) return 1;
    build_families();
    oapp_set_menus(g_app, MENUS, 5);
    otb_init(&g_tb, ITEMS, N_ITEMS, 0, 0, 900);   // the shell re-lays it out to the real width
    ocombo_init(otb_combo(&g_tb, T_FONT), 0, 0, 150, g_nfam, 0, 0, fam_label, 0);
    ocombo_init(otb_combo(&g_tb, T_SIZE), 0, 0, 56, NSIZES, 4, 1, size_label, 0);
    oapp_set_toolbar(g_app, &g_tb);
    oapp_set_chrome(g_app, ORULER_H, 0);
    new_document();
#ifdef WRITER_VERIFY_AUTOOPEN
    // THROWAWAY VERIFICATION BUILD ONLY (never in the shipped/landed source):
    // headless QMP keyboard injection into this VM proved unreliable past the
    // first synthesized key per boot (measured: Alt+F alone opened the File
    // menu correctly on one boot, a bare Down arrow reached this window
    // correctly on another, but no sequence of more than one key landed in
    // repeated attempts). This bypasses the interactive File>Open dialog so
    // the RENDER path (docx/odt load -> layout_document -> layout_draw) can
    // be screenshotted directly. See CHANGELOG/officewriter final report.
    load_document(WRITER_VERIFY_AUTOOPEN);
#endif
#ifdef WRITER_VERIFY_AUTOMODAL
    oapp_set_tick_ms(g_app, 1000);
#endif
    oapp_run(g_app, draw, on_event);
    maps_free();
    if (g_lo) layout_free(g_lo);
    if (g_doc) doc_free(g_doc);
    oapp_destroy(g_app);
    return 0;
}
