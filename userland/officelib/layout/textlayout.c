// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// textlayout.c - text layout / pagination for the document view (agent 9,
// include/layout.h). Wraps each paragraph's runs to width_px using
// ttf_measure_ex(), honoring alignment, list indents/bullets, headings
// (resolved run+style formatting), and simple tables (equal columns, real
// gridlines via the ADDITIVE laid_out.rects - see layout.h's top-of-file
// comment on that field). layout_draw() renders every span with
// win_draw_text_ttf_ex() (a strict superset of win_draw_text_ttf() with face
// and FONT_STYLE_BOLD/ITALIC bits - needed for real bold headings, which
// win_draw_text_ttf()'s plain syscall cannot render at all).
//
// FORMAT RESOLUTION (docx.c/odt.c leave a run's doc_runfmt at RUNFMT_INHERIT
// - bold=0,italic=0,...,size=0,color=0xFFFFFFFF - unless that run's own XML
// carried an explicit override; a Heading1 paragraph's real bold/size lives on
// document.styles[para->style_id].runfmt instead). This file resolves the
// EFFECTIVE format for each run as: bold/italic/underline/strike = the run's
// own flag OR'd with the paragraph style's flag (either saying "on" wins);
// size = the run's own size if >0, else the style's if >0, else a 12pt
// fallback; color = the run's own color if not the inherit sentinel, else the
// style's, else the theme's label-text ink. This is a heuristic the doc model
// does not disambiguate further (a doc_runfmt has no "was this explicitly set
// vs default-zero" bit) and is the intended reading of the task's "headings
// (bigger/bold via the run/style fmt)".
#include "layout.h"
#include "../../libc/types.h"
#include "../../libc/syscall.h"
#include "../../libc/theme.h"
#include "../../libc/stdio.h"
#include <stdlib.h>
#include <string.h>

#define LO_LEFT_PAD       16
#define LO_RIGHT_PAD      16
#define LO_TOP_PAD        16
#define LO_BOT_PAD        16
#define LO_PARA_GAP        8
#define LO_LIST_INDENT    22
#define LO_BULLET_W       20
#define LO_DEFAULT_SIZE   12
#define LO_TABLE_CELL_PAD  8
#define LO_MAX_LINE_TOK  160

// ---- Growable span/rect builder (internal; laid_out itself owns the final
// arrays once layout_document() hands them off) ----------------------------
typedef struct {
    laid_span *spans; int nspan, cap_span;
    laid_rect *rects; int nrect, cap_rect;
    // Strings the layout itself generates (numbered-list labels "1.", "2.",
    // ...) and that spans therefore point INTO. laid_span.text is documented
    // as "borrowed (points into the document)", and for every run that is
    // true; the list number is the one span whose text the document does
    // not contain. It used to point at a stack buffer in layout_para() and
    // dangled the moment that function returned (officeintegrate). The
    // copies live here, hand off to laid_out.owned, and die in layout_free().
    char **owned; int nowned, cap_owned;
} lo_builder_t;

static void lb_init(lo_builder_t *b) { memset(b, 0, sizeof(*b)); }

// Copy `s` into builder-owned storage that outlives layout_para(). Returns the
// copy, or 0 on allocation failure (the caller falls back to a static label).
static const char *lb_own_str(lo_builder_t *b, const char *s) {
    if (b->nowned >= b->cap_owned) {
        int nc = b->cap_owned ? b->cap_owned * 2 : 16;
        char **no = (char **)realloc(b->owned, (size_t)nc * sizeof(char *));
        if (!no) return 0;
        b->owned = no; b->cap_owned = nc;
    }
    char *copy = strdup(s);
    if (!copy) return 0;
    b->owned[b->nowned++] = copy;
    return copy;
}

static laid_span *lb_add_span(lo_builder_t *b) {
    if (b->nspan >= b->cap_span) {
        int nc = b->cap_span ? b->cap_span * 2 : 64;
        laid_span *ns = (laid_span *)realloc(b->spans, (size_t)nc * sizeof(laid_span));
        if (!ns) return 0;
        b->spans = ns; b->cap_span = nc;
    }
    return &b->spans[b->nspan++];
}

static laid_rect *lb_add_rect(lo_builder_t *b) {
    if (b->nrect >= b->cap_rect) {
        int nc = b->cap_rect ? b->cap_rect * 2 : 16;
        laid_rect *nr = (laid_rect *)realloc(b->rects, (size_t)nc * sizeof(laid_rect));
        if (!nr) return 0;
        b->rects = nr; b->cap_rect = nc;
    }
    return &b->rects[b->nrect++];
}

// Per-document list-numbering state: one running counter per nesting level
// (0..7), auto-incremented across CONSECUTIVE ordered items at that level.
// The doc model carries no numId, so exact Word/ODF renumbering semantics are
// unavailable; this is the best approximation the model supports and is
// documented as such.
typedef struct { int active[8]; int counter[8]; } lo_list_state_t;

static int lo_word_w(const char *s, int len, int face, int size, int style) {
    char buf[256];
    int n = len; if (n > 255) n = 255; if (n < 0) n = 0;
    memcpy(buf, s, (size_t)n);
    buf[n] = 0;
    return ttf_measure_ex(buf, face, size, style);
}

// Lay out one paragraph's runs at (x0,y0) wrapped to `width`, appending spans
// to `b`. Returns the vertical space consumed (excludes the inter-paragraph
// gap; the caller adds that).
static int layout_para(lo_builder_t *b, const document *d, const doc_para *p,
                       int x0, int y0, int width, unsigned int def_ink,
                       lo_list_state_t *ls) {
    doc_runfmt style_fmt = { 0, 0, 0, 0, 0, 0xFFFFFFFFu, 0 };
    int have_style = 0;
    if (p->style_id >= 0 && p->style_id < d->nstyle) {
        style_fmt = d->styles[p->style_id].runfmt;
        have_style = 1;
    }

    int is_list = (p->list_level >= 0);
    int lvl = is_list ? p->list_level : 0;
    if (lvl < 0) lvl = 0;
    if (lvl > 7) lvl = 7;

    const char *bullet = 0;
    char num_buf[16];
    if (is_list) {
        if (p->list_ordered) {
            if (!ls->active[lvl]) ls->counter[lvl] = 1; else ls->counter[lvl]++;
            for (int L = lvl + 1; L < 8; L++) ls->active[L] = 0;
            ls->active[lvl] = 1;
            snprintf(num_buf, sizeof(num_buf), "%d.", ls->counter[lvl]);
            // NOT `bullet = num_buf`: num_buf is this frame's stack and the
            // span outlives it. Own a copy; on OOM degrade to a plain dash
            // rather than to a dangling pointer.
            bullet = lb_own_str(b, num_buf);
            if (!bullet) bullet = "-";
        } else {
            for (int L = 0; L < 8; L++) ls->active[L] = 0;
            // Plain ASCII, not the UTF-8 U+2022 bullet: MEASURED that
            // win_draw_text_ttf_ex() (SYS_WIN_DRAW_TTF_EX/311, used below in
            // layout_draw() for real bold/italic headings) renders each byte
            // of "\xE2\x80\xA2" as its own single-byte glyph ("\xE2\x80\xA2"
            // -> mojibake "\xC3\xA2\xE2\x82\xAC\xC2\xA2" on screen), unlike
            // win_draw_text_ttf() (235, no style bits), which some other apps
            // use with that UTF-8 literal successfully. ASCII has no encoding
            // question either syscall can get wrong.
            bullet = "-";
        }
    } else {
        for (int L = 0; L < 8; L++) ls->active[L] = 0;
    }

    int indent = is_list ? (lvl + 1) * LO_LIST_INDENT : 0;
    int x_text0 = x0 + indent;
    int cw = width - indent;
    if (cw < 40) cw = 40;

    int cursor_y = y0;
    int pen_x = x_text0;
    int line_started = 0;
    int line_max_h = 0;
    int first_line = 1;
    int any_line_emitted = 0;

    struct { int x, w; const char *text; int len; doc_runfmt fmt; } line[LO_MAX_LINE_TOK];
    int nline = 0;

#define LO_FLUSH_LINE() do { \
        int lh = line_max_h > 0 ? line_max_h : (LO_DEFAULT_SIZE + 6); \
        int lw = pen_x - x_text0; \
        int off = 0; \
        if (p->align == DOC_ALIGN_C) off = (cw - lw) / 2; \
        else if (p->align == DOC_ALIGN_R) off = cw - lw; \
        if (off < 0) off = 0; \
        if (first_line && bullet) { \
            laid_span *bs = lb_add_span(b); \
            if (bs) { \
                bs->x = x0 + lvl * LO_LIST_INDENT; bs->y = cursor_y; \
                bs->w = LO_BULLET_W; bs->h = lh; \
                bs->text = bullet; bs->len = (int)strlen(bullet); \
                doc_runfmt bf = { 0, 0, 0, 0, LO_DEFAULT_SIZE, def_ink, 0 }; \
                bs->fmt = bf; \
            } \
        } \
        for (int _i = 0; _i < nline; _i++) { \
            laid_span *sp = lb_add_span(b); \
            if (sp) { \
                sp->x = line[_i].x + off; sp->y = cursor_y; sp->w = line[_i].w; sp->h = lh; \
                sp->text = line[_i].text; sp->len = line[_i].len; sp->fmt = line[_i].fmt; \
            } \
        } \
        any_line_emitted = 1; \
        cursor_y += lh; \
        nline = 0; line_max_h = 0; pen_x = x_text0; line_started = 0; first_line = 0; \
    } while (0)

    for (int ri = 0; ri < p->nrun; ri++) {
        const doc_run *run = &p->runs[ri];
        if (!run->text || !run->text[0]) continue;

        int bold      = run->fmt.bold      || (have_style && style_fmt.bold);
        int italic    = run->fmt.italic    || (have_style && style_fmt.italic);
        int underline = run->fmt.underline || (have_style && style_fmt.underline);
        int strike    = run->fmt.strike    || (have_style && style_fmt.strike);
        int size = run->fmt.size > 0 ? run->fmt.size
                 : (have_style && style_fmt.size > 0 ? style_fmt.size : LO_DEFAULT_SIZE);
        unsigned int color = (run->fmt.color != 0xFFFFFFFFu) ? run->fmt.color
                            : ((have_style && style_fmt.color != 0xFFFFFFFFu) ? style_fmt.color : def_ink);
        int face = run->fmt.face ? run->fmt.face : (have_style ? style_fmt.face : 0);
        int style_bits = (bold ? FONT_STYLE_BOLD : 0) | (italic ? FONT_STYLE_ITALIC : 0);
        doc_runfmt eff = { bold, italic, underline, strike, size, color, face };
        int line_h = size + 6;

        const char *s = run->text;
        int i = 0;
        while (s[i]) {
            if (s[i] == '\n') { LO_FLUSH_LINE(); i++; continue; }
            while (s[i] == ' ') i++;
            if (!s[i]) break;
            int start = i;
            while (s[i] && s[i] != ' ' && s[i] != '\n') i++;
            int wlen = i - start;
            if (wlen <= 0) continue;
            int ww = lo_word_w(s + start, wlen, face, size, style_bits);
            int sp_w = line_started ? lo_word_w(" ", 1, face, size, style_bits) : 0;
            if (pen_x + sp_w + ww > x_text0 + cw && line_started) {
                LO_FLUSH_LINE();
                sp_w = 0;
            }
            if (nline < LO_MAX_LINE_TOK) {
                line[nline].x = pen_x + sp_w;
                line[nline].w = ww;
                line[nline].text = s + start;
                line[nline].len = wlen;
                line[nline].fmt = eff;
                nline++;
            }
            pen_x += sp_w + ww;
            if (line_h > line_max_h) line_max_h = line_h;
            line_started = 1;
        }
    }
    if (nline > 0) {
        LO_FLUSH_LINE();
    } else if (!any_line_emitted) {
        line_max_h = LO_DEFAULT_SIZE + 6;
        LO_FLUSH_LINE();
    }
#undef LO_FLUSH_LINE

    return cursor_y - y0;
}

// Simple table: equal-width columns, each cell laid out as its own paragraph
// (no nested lists/tables inside a cell - matches "simple tables" in scope).
// Emits a laid_rect per cell so layout_draw() can stroke a real grid.
static int layout_table(lo_builder_t *b, const document *d, const doc_table *t,
                        int x0, int y0, int width, unsigned int def_ink) {
    int cols = t->cols > 0 ? t->cols : 1;
    int col_w = width / cols;
    int cur_y = y0;
    lo_list_state_t cell_ls;

    for (int r = 0; r < t->rows; r++) {
        int row_y0 = cur_y;
        int row_h = 0;
        for (int c = 0; c < cols; c++) {
            doc_para *cell = t->cells ? t->cells[(long)r * cols + c] : 0;
            int cx = x0 + c * col_w;
            int ch = 0;
            if (cell) {
                memset(&cell_ls, 0, sizeof(cell_ls));
                ch = layout_para(b, d, cell, cx + LO_TABLE_CELL_PAD, row_y0 + LO_TABLE_CELL_PAD,
                                 col_w - 2 * LO_TABLE_CELL_PAD, def_ink, &cell_ls);
            }
            if (ch + 2 * LO_TABLE_CELL_PAD > row_h) row_h = ch + 2 * LO_TABLE_CELL_PAD;
        }
        if (row_h < 24) row_h = 24;
        for (int c = 0; c < cols; c++) {
            laid_rect *rc = lb_add_rect(b);
            if (rc) { rc->x = x0 + c * col_w; rc->y = row_y0; rc->w = col_w; rc->h = row_h; }
        }
        cur_y = row_y0 + row_h;
    }
    return cur_y - y0;
}

laid_out *layout_document(const document *d, int width_px) {
    if (!d) return 0;
    laid_out *lo = (laid_out *)calloc(1, sizeof(*lo));
    if (!lo) return 0;

    lo_builder_t b; lb_init(&b);
    unsigned int def_ink = theme_color(THEME_COLOR_LABEL_TEXT);
    if (!def_ink) def_ink = 0x00FFFFFF;

    int content_w = width_px - LO_LEFT_PAD - LO_RIGHT_PAD;
    if (content_w < 40) content_w = (width_px > 40) ? width_px : 40;

    int cursor_y = LO_TOP_PAD;
    lo_list_state_t ls; memset(&ls, 0, sizeof(ls));

    for (int i = 0; i < d->nblk; i++) {
        const doc_block *blk = &d->blocks[i];
        if (blk->type == DBLK_PARA && blk->para) {
            int h = layout_para(&b, d, blk->para, LO_LEFT_PAD, cursor_y, content_w, def_ink, &ls);
            cursor_y += h + LO_PARA_GAP;
        } else if (blk->type == DBLK_TABLE && blk->table) {
            int h = layout_table(&b, d, blk->table, LO_LEFT_PAD, cursor_y, content_w, def_ink);
            cursor_y += h + LO_PARA_GAP;
            memset(&ls, 0, sizeof(ls));   // a table breaks any list numbering run
        } else if (blk->type == DBLK_IMAGE) {
            // layout.h's laid_span is text-only (no image decode/blit path in
            // this pass); a labeled placeholder keeps the block visible and
            // taking up real space instead of silently vanishing.
            static const char *IMG_LABEL = "[image]";
            laid_span *sp = lb_add_span(&b);
            if (sp) {
                sp->x = LO_LEFT_PAD; sp->y = cursor_y; sp->w = content_w; sp->h = LO_DEFAULT_SIZE + 6;
                sp->text = IMG_LABEL; sp->len = (int)strlen(IMG_LABEL);
                doc_runfmt f = { 0, 1, 0, 0, LO_DEFAULT_SIZE, def_ink, 0 };
                sp->fmt = f;
            }
            cursor_y += LO_DEFAULT_SIZE + 6 + LO_PARA_GAP;
            memset(&ls, 0, sizeof(ls));
        }
    }

    lo->spans = b.spans; lo->nspan = b.nspan;
    lo->rects = b.rects; lo->nrect = b.nrect;
    lo->owned = b.owned; lo->nowned = b.nowned;
    lo->width = width_px;
    lo->height = cursor_y + LO_BOT_PAD;
    return lo;
}

void layout_free(laid_out *lo) {
    if (!lo) return;
    for (int i = 0; i < lo->nowned; i++) free(lo->owned[i]);
    free(lo->owned);
    free(lo->spans);
    free(lo->rects);
    free(lo);
}

void layout_draw(int win, const laid_out *lo, int scroll_x, int scroll_y, int vh) {
    if (!lo) return;

    unsigned int border = theme_color(THEME_COLOR_BORDER_SUBTLE);
    if (!border) border = 0x00606060;
    for (int i = 0; i < lo->nrect; i++) {
        const laid_rect *r = &lo->rects[i];
        int y = r->y - scroll_y;
        if (y + r->h < 0 || y > vh) continue;
        int x = r->x - scroll_x;
        win_draw_rect(win, x, y, r->w, 1, border);
        win_draw_rect(win, x, y, 1, r->h, border);
        win_draw_rect(win, x, y + r->h - 1, r->w, 1, border);
        win_draw_rect(win, x + r->w - 1, y, 1, r->h, border);
    }

    char buf[512];
    for (int i = 0; i < lo->nspan; i++) {
        const laid_span *s = &lo->spans[i];
        int y = s->y - scroll_y;
        if (y + s->h < 0 || y > vh) continue;
        int n = s->len; if (n < 0) n = 0; if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
        if (s->text && n > 0) memcpy(buf, s->text, (size_t)n);
        buf[n] = 0;
        if (!buf[0]) continue;
        int x = s->x - scroll_x;
        int style_bits = (s->fmt.bold ? FONT_STYLE_BOLD : 0) | (s->fmt.italic ? FONT_STYLE_ITALIC : 0);
        win_draw_text_ttf_ex(win, x, y, buf, s->fmt.face, s->fmt.size, style_bits, s->fmt.color);
        if (s->fmt.underline || s->fmt.strike) {
            int tw = ttf_measure_ex(buf, s->fmt.face, s->fmt.size, style_bits);
            int uy = s->fmt.underline ? (y + s->fmt.size + 2) : (y + s->fmt.size / 2);
            win_draw_rect(win, x, uy, tw, 1, s->fmt.color & 0xFFFFFFu);
        }
    }
}
