// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// ODT (OpenDocument Text) import + export: ODF text <-> the shared `document`
// model. v1 scope: headings, paragraphs, styled runs (bold/italic/underline),
// bullet/number lists, simple tables, inline images. See docs/OFFICE_SUITE_ARCHITECTURE.md.
#include "formats.h"
#include "opc.h"
#include "oxml.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ---- ODF namespace URIs -----------------------------------------------------
#define NS_OFFICE "urn:oasis:names:tc:opendocument:xmlns:office:1.0"
#define NS_TEXT   "urn:oasis:names:tc:opendocument:xmlns:text:1.0"
#define NS_TABLE  "urn:oasis:names:tc:opendocument:xmlns:table:1.0"
#define NS_STYLE  "urn:oasis:names:tc:opendocument:xmlns:style:1.0"
#define NS_DRAW   "urn:oasis:names:tc:opendocument:xmlns:drawing:1.0"
#define NS_FO     "urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0"

#define MIME_TEXT "application/vnd.oasis.opendocument.text"

static int nseq(oxml *x, const char *ns) { return strcmp(oxml_ns(x), ns) == 0; }
static int is(oxml *x, const char *ns, const char *local) {
    return nseq(x, ns) && strcmp(oxml_local(x), local) == 0;
}
// ---- automatic-style tables (built while parsing content.xml) --------------
typedef struct { char name[80]; doc_runfmt fmt; } spanstyle;
typedef struct { char name[80]; int ordered; } liststyle;
typedef struct {
    spanstyle *spans; int nspan, cspan;
    liststyle *lists; int nlist, clist;
} sty_tab;

static const doc_runfmt DEFRUN = { 0,0,0,0, 0, 0xFFFFFFFFu, 0 };

static void styles_free(sty_tab *t) { free(t->spans); free(t->lists); }

static doc_runfmt span_lookup(sty_tab *t, const char *name) {
    if (name)
        for (int i = 0; i < t->nspan; i++)
            if (strcmp(t->spans[i].name, name) == 0) return t->spans[i].fmt;
    return DEFRUN;
}
static int list_ordered(sty_tab *t, const char *name) {
    if (name)
        for (int i = 0; i < t->nlist; i++)
            if (strcmp(t->lists[i].name, name) == 0) return t->lists[i].ordered;
    return 0;
}
static void span_add(sty_tab *t, const char *name, doc_runfmt f) {
    if (!name) return;
    if (t->nspan == t->cspan) {
        int nc = t->cspan ? t->cspan * 2 : 8;
        spanstyle *ns = (spanstyle *)realloc(t->spans, (size_t)nc * sizeof(spanstyle));
        if (!ns) return;
        t->spans = ns; t->cspan = nc;
    }
    spanstyle *s = &t->spans[t->nspan++];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->fmt = f;
}
static void list_add(sty_tab *t, const char *name, int ordered) {
    if (!name) return;
    if (t->nlist == t->clist) {
        int nc = t->clist ? t->clist * 2 : 8;
        liststyle *nl = (liststyle *)realloc(t->lists, (size_t)nc * sizeof(liststyle));
        if (!nl) return;
        t->lists = nl; t->clist = nc;
    }
    liststyle *l = &t->lists[t->nlist++];
    memset(l, 0, sizeof(*l));
    strncpy(l->name, name, sizeof(l->name) - 1);
    l->ordered = ordered;
}

// Consume the current element's subtree (we have just seen its START at `d`).
static void skip_subtree(oxml *x, int d) {
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e == OXML_END && oxml_depth(x) == d) return;
    }
}

// ---- automatic-styles parse -------------------------------------------------
// At entry the office:automatic-styles START was just consumed (depth = d).
static void parse_auto_styles(oxml *x, int d, sty_tab *t) {
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e == OXML_END && oxml_depth(x) == d) return;
        if (e != OXML_START) continue;
        if (is(x, NS_STYLE, "style")) {
            int sd = oxml_depth(x);
            const char *nm = oxml_attr_ns(x, NS_STYLE, "name");
            char name[80]; name[0] = 0;
            if (nm) strncpy(name, nm, sizeof(name) - 1), name[sizeof(name) - 1] = 0;
            doc_runfmt f = DEFRUN;
            // read the text-properties child for run formatting
            oxml_evt e2;
            while ((e2 = oxml_next(x)) != OXML_EOF) {
                if (e2 == OXML_END && oxml_depth(x) == sd) break;
                if (e2 == OXML_START && is(x, NS_STYLE, "text-properties")) {
                    const char *w  = oxml_attr_ns(x, NS_FO, "font-weight");
                    const char *st = oxml_attr_ns(x, NS_FO, "font-style");
                    const char *ul = oxml_attr_ns(x, NS_STYLE, "text-underline-style");
                    const char *lt = oxml_attr_ns(x, NS_STYLE, "text-line-through-style");
                    if (w && strcmp(w, "bold") == 0) f.bold = 1;
                    if (st && strcmp(st, "italic") == 0) f.italic = 1;
                    if (ul && strcmp(ul, "none") != 0) f.underline = 1;
                    if (lt && strcmp(lt, "none") != 0) f.strike = 1;
                }
            }
            span_add(t, name[0] ? name : NULL, f);
        } else if (is(x, NS_TEXT, "list-style")) {
            int sd = oxml_depth(x);
            const char *nm = oxml_attr_ns(x, NS_STYLE, "name");
            char name[80]; name[0] = 0;
            if (nm) strncpy(name, nm, sizeof(name) - 1), name[sizeof(name) - 1] = 0;
            int ordered = 0;
            oxml_evt e2;
            while ((e2 = oxml_next(x)) != OXML_EOF) {
                if (e2 == OXML_END && oxml_depth(x) == sd) break;
                if (e2 == OXML_START && is(x, NS_TEXT, "list-level-style-number")) ordered = 1;
            }
            list_add(t, name[0] ? name : NULL, ordered);
        } else {
            skip_subtree(x, oxml_depth(x));
        }
    }
}

// ---- paragraph run parse ----------------------------------------------------
// We have just seen the START of a text:h/text:p/(cell text:p) at depth `d`.
// Fill `p` with runs. Handles text nodes, text:span (styled), text:s/tab/line-break.
static void parse_runs(oxml *x, int d, doc_para *p, sty_tab *t) {
    doc_runfmt cur = DEFRUN;
    int span_depth = -1;
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        int cd = oxml_depth(x);
        if (e == OXML_END && cd == d) return;
        if (e == OXML_END && cd == span_depth) { cur = DEFRUN; span_depth = -1; continue; }
        if (e == OXML_TEXT) {
            const char *tx = oxml_text(x);
            if (tx && *tx) doc_para_add_run(p, tx, cur);
        } else if (e == OXML_START) {
            if (is(x, NS_TEXT, "span")) {
                const char *sn = oxml_attr_ns(x, NS_TEXT, "style-name");
                cur = span_lookup(t, sn);
                span_depth = cd;
            } else if (is(x, NS_TEXT, "s")) {
                const char *c = oxml_attr_ns(x, NS_TEXT, "c");
                int n = c ? atoi(c) : 1; if (n < 1) n = 1;
                char sp[64]; int k = 0; for (; k < n && k < 63; k++) sp[k] = ' ';
                sp[k] = 0; doc_para_add_run(p, sp, cur);
            } else if (is(x, NS_TEXT, "tab")) {
                doc_para_add_run(p, "\t", cur);
            } else if (is(x, NS_TEXT, "line-break")) {
                doc_para_add_run(p, "\n", cur);
            } else {
                skip_subtree(x, cd);
            }
        }
    }
}

// Add (once) a document style named "Heading N"; return its index.
static int heading_style(document *d, int level, int *cache /*[10]*/) {
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    if (cache[level] >= 0) return cache[level];
    char nm[16]; snprintf(nm, sizeof(nm), "Heading %d", level);
    doc_runfmt f = DEFRUN; f.bold = 1;
    int id = doc_add_style(d, nm, f, DOC_ALIGN_L);
    cache[level] = id;
    return id;
}

// Allocate a standalone cell paragraph (freed by doc_free via the table).
static doc_para *new_cell_para(void) {
    doc_para *p = (doc_para *)calloc(1, sizeof(doc_para));
    if (!p) return NULL;
    p->align = DOC_ALIGN_L; p->style_id = -1; p->list_level = -1;
    return p;
}

// ---- list parse (recursive over nesting levels) -----------------------------
static void parse_list(oxml *x, int d, document *doc, sty_tab *t,
                       int level, int ordered, int *hcache) {
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        int cd = oxml_depth(x);
        if (e == OXML_END && cd == d) return;
        if (e != OXML_START) continue;
        if (is(x, NS_TEXT, "list-item")) {
            int idd = cd;
            oxml_evt e2;
            while ((e2 = oxml_next(x)) != OXML_EOF) {
                int c2 = oxml_depth(x);
                if (e2 == OXML_END && c2 == idd) break;
                if (e2 != OXML_START) continue;
                if (is(x, NS_TEXT, "p") || is(x, NS_TEXT, "h")) {
                    doc_para *p = doc_add_para(doc);
                    if (p) { p->list_level = level; p->list_ordered = ordered; }
                    parse_runs(x, c2, p, t);
                } else if (is(x, NS_TEXT, "list")) {
                    const char *sn = oxml_attr_ns(x, NS_TEXT, "style-name");
                    int ord = sn ? list_ordered(t, sn) : ordered;
                    parse_list(x, c2, doc, t, level + 1, ord, hcache);
                } else {
                    skip_subtree(x, c2);
                }
            }
        } else {
            skip_subtree(x, cd);
        }
    }
}

// ---- table parse ------------------------------------------------------------
static void parse_table(oxml *x, int d, document *doc, sty_tab *t) {
    // gather rows dynamically, then assemble a fixed rows*cols doc_table
    doc_para ***rows = NULL; int *rowlen = NULL; int nrows = 0, crows = 0;
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        int cd = oxml_depth(x);
        if (e == OXML_END && cd == d) break;
        if (e != OXML_START) continue;
        if (is(x, NS_TABLE, "table-row")) {
            int rd = cd;
            doc_para **cells = NULL; int ncell = 0, ccell = 0;
            oxml_evt e2;
            while ((e2 = oxml_next(x)) != OXML_EOF) {
                int c2 = oxml_depth(x);
                if (e2 == OXML_END && c2 == rd) break;
                if (e2 != OXML_START) continue;
                if (is(x, NS_TABLE, "table-cell") || is(x, NS_TABLE, "covered-table-cell")) {
                    int celld = c2;
                    int repeat = 1;
                    const char *rep = oxml_attr_ns(x, NS_TABLE, "number-columns-repeated");
                    if (rep) { repeat = atoi(rep); if (repeat < 1) repeat = 1; if (repeat > 256) repeat = 256; }
                    doc_para *cp = new_cell_para();
                    // read the cell's paragraphs (concatenate into one)
                    oxml_evt e3;
                    while ((e3 = oxml_next(x)) != OXML_EOF) {
                        int c3 = oxml_depth(x);
                        if (e3 == OXML_END && c3 == celld) break;
                        if (e3 == OXML_START && (is(x, NS_TEXT, "p") || is(x, NS_TEXT, "h")))
                            parse_runs(x, c3, cp, t);
                        else if (e3 == OXML_START)
                            skip_subtree(x, c3);
                    }
                    for (int r = 0; r < repeat; r++) {
                        doc_para *use = cp;
                        if (r > 0) {  // duplicate for repeated empty columns
                            use = new_cell_para();
                        }
                        if (ncell == ccell) {
                            int nc = ccell ? ccell * 2 : 4;
                            doc_para **nn = (doc_para **)realloc(cells, (size_t)nc * sizeof(doc_para *));
                            if (!nn) break;
                            cells = nn; ccell = nc;
                        }
                        cells[ncell++] = use;
                    }
                } else {
                    skip_subtree(x, c2);
                }
            }
            if (nrows == crows) {
                int nc = crows ? crows * 2 : 4;
                doc_para ***nr = (doc_para ***)realloc(rows, (size_t)nc * sizeof(doc_para **));
                int *nl = (int *)realloc(rowlen, (size_t)nc * sizeof(int));
                if (!nr || !nl) { free(nr); free(nl); break; }
                rows = nr; rowlen = nl; crows = nc;
            }
            rows[nrows] = cells; rowlen[nrows] = ncell; nrows++;
        } else if (is(x, NS_TABLE, "table-column") || is(x, NS_TABLE, "table-header-rows")) {
            skip_subtree(x, cd);
        } else {
            skip_subtree(x, cd);
        }
    }
    // assemble
    int cols = 0;
    for (int r = 0; r < nrows; r++) if (rowlen[r] > cols) cols = rowlen[r];
    if (nrows > 0 && cols > 0) {
        doc_block *b = doc_add_block(doc, DBLK_TABLE);
        doc_table *tb = (doc_table *)calloc(1, sizeof(doc_table));
        if (b && tb) {
            tb->rows = nrows; tb->cols = cols;
            tb->cells = (doc_para **)calloc((size_t)nrows * cols, sizeof(doc_para *));
            if (tb->cells) {
                for (int r = 0; r < nrows; r++)
                    for (int c = 0; c < rowlen[r]; c++)
                        tb->cells[r * cols + c] = rows[r][c];
            }
            b->table = tb;
        } else { free(tb); }
    } else {
        // discard collected cells if we cannot build a table
        for (int r = 0; r < nrows; r++)
            for (int c = 0; c < rowlen[r]; c++) {
                doc_para *cp = rows[r][c];
                if (cp) { for (int i = 0; i < cp->nrun; i++) free(cp->runs[i].text); free(cp->runs); free(cp); }
            }
    }
    for (int r = 0; r < nrows; r++) free(rows[r]);
    free(rows); free(rowlen);
}

// ---- inline image parse (draw:frame > draw:image) ---------------------------
static void parse_frame(oxml *x, int d, document *doc, opc_pkg *pkg) {
    char href[256]; href[0] = 0;
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        int cd = oxml_depth(x);
        if (e == OXML_END && cd == d) break;
        if (e == OXML_START && is(x, NS_DRAW, "image")) {
            const char *h = oxml_attr(x, "href");   // xlink:href, matched by local
            if (h) { strncpy(href, h, sizeof(href) - 1); href[sizeof(href) - 1] = 0; }
        }
    }
    if (!href[0]) return;
    const char *pn = href; if (*pn == '/') pn++;
    opc_part *part = opc_get(pkg, pn);
    if (!part || !part->data || !part->size) return;
    doc_image *im = doc_add_image(doc, href, part->data, part->size);
    if (!im) return;
    int idx = doc->nimg - 1;
    doc_block *b = doc_add_block(doc, DBLK_IMAGE);
    if (b) b->image = &doc->images[idx];   // borrowed from d->images
}

// ---- body parse -------------------------------------------------------------
static void parse_body_text(oxml *x, int d, document *doc, sty_tab *t,
                            opc_pkg *pkg, int *hcache) {
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        int cd = oxml_depth(x);
        if (e == OXML_END && cd == d) return;
        if (e != OXML_START) continue;
        if (is(x, NS_TEXT, "h")) {
            const char *lvl = oxml_attr_ns(x, NS_TEXT, "outline-level");
            int lv = lvl ? atoi(lvl) : 1; if (lv < 1) lv = 1;
            doc_para *p = doc_add_para(doc);
            if (p) p->style_id = heading_style(doc, lv, hcache);
            parse_runs(x, cd, p, t);
        } else if (is(x, NS_TEXT, "p")) {
            doc_para *p = doc_add_para(doc);
            parse_runs(x, cd, p, t);
        } else if (is(x, NS_TEXT, "list")) {
            const char *sn = oxml_attr_ns(x, NS_TEXT, "style-name");
            int ord = sn ? list_ordered(t, sn) : 0;
            parse_list(x, cd, doc, t, 0, ord, hcache);
        } else if (is(x, NS_TABLE, "table")) {
            parse_table(x, cd, doc, t);
        } else if (is(x, NS_DRAW, "frame")) {
            parse_frame(x, cd, doc, pkg);
        } else {
            skip_subtree(x, cd);
        }
    }
}

// ---- odt_load ---------------------------------------------------------------
int odt_load(const unsigned char *b, unsigned long n, document **out) {
    if (out) *out = NULL;
    if (!b || !n || !out) return -1;
    opc_pkg *pkg = opc_open_mem(b, n);
    if (!pkg) return -1;
    opc_part *content = opc_get(pkg, "content.xml");
    if (!content || !content->data) { opc_free(pkg); return -1; }

    document *doc = doc_new();
    if (!doc) { opc_free(pkg); return -1; }
    sty_tab tab; memset(&tab, 0, sizeof(tab));
    int hcache[10]; for (int i = 0; i < 10; i++) hcache[i] = -1;

    oxml *x = oxml_open((const char *)content->data, content->size);
    if (!x) { styles_free(&tab); doc_free(doc); opc_free(pkg); return -1; }

    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e != OXML_START) continue;
        if (is(x, NS_OFFICE, "automatic-styles")) {
            parse_auto_styles(x, oxml_depth(x), &tab);
        } else if (is(x, NS_OFFICE, "text")) {
            parse_body_text(x, oxml_depth(x), doc, &tab, pkg, hcache);
        }
    }
    oxml_close(x);
    styles_free(&tab);
    opc_free(pkg);   // frees content->data (copied into the model already)
    *out = doc;
    return 0;
}

// ============================================================================
// SAVE
// ============================================================================

static int runfmt_plain(const doc_runfmt *f) {
    return !f->bold && !f->italic && !f->underline && !f->strike;
}
static int runfmt_eq(const doc_runfmt *a, const doc_runfmt *b) {
    return a->bold == b->bold && a->italic == b->italic &&
           a->underline == b->underline && a->strike == b->strike;
}

// registry of distinct non-plain run formats used in the document
typedef struct { doc_runfmt fmt; char name[16]; } autospan;
typedef struct { autospan *v; int n, c; } autoreg;

static const char *reg_span(autoreg *r, const doc_runfmt *f) {
    if (runfmt_plain(f)) return NULL;
    for (int i = 0; i < r->n; i++)
        if (runfmt_eq(&r->v[i].fmt, f)) return r->v[i].name;
    if (r->n == r->c) {
        int nc = r->c ? r->c * 2 : 8;
        autospan *nv = (autospan *)realloc(r->v, (size_t)nc * sizeof(autospan));
        if (!nv) return NULL;
        r->v = nv; r->c = nc;
    }
    autospan *s = &r->v[r->n];
    s->fmt = *f;
    snprintf(s->name, sizeof(s->name), "T%d", r->n + 1);
    r->n++;
    return s->name;
}

// scan the whole document, registering every distinct run format
static void collect_spans(const document *d, autoreg *r) {
    for (int i = 0; i < d->nblk; i++) {
        const doc_block *b = &d->blocks[i];
        if (b->type == DBLK_PARA && b->para) {
            for (int j = 0; j < b->para->nrun; j++) reg_span(r, &b->para->runs[j].fmt);
        } else if (b->type == DBLK_TABLE && b->table && b->table->cells) {
            long nc = (long)b->table->rows * b->table->cols;
            for (long k = 0; k < nc; k++) {
                doc_para *cp = b->table->cells[k];
                if (cp) for (int j = 0; j < cp->nrun; j++) reg_span(r, &cp->runs[j].fmt);
            }
        }
    }
}

// If a style name looks like "Heading N", return N (1..9), else 0.
static int heading_level_of(const document *d, int style_id) {
    if (style_id < 0 || style_id >= d->nstyle) return 0;
    const char *nm = d->styles[style_id].name;
    if (!nm) return 0;
    if (strncmp(nm, "Heading ", 8) == 0) {
        int v = atoi(nm + 8);
        if (v >= 1 && v <= 9) return v;
    }
    if (strncmp(nm, "Heading", 7) == 0 && nm[7] >= '1' && nm[7] <= '9') return nm[7] - '0';
    return 0;
}

static void w_runs(oxml_w *w, const doc_para *p, autoreg *reg) {
    for (int j = 0; j < p->nrun; j++) {
        const doc_run *rn = &p->runs[j];
        const char *sn = reg_span(reg, &rn->fmt);   // already registered; returns name
        if (sn) {
            oxml_w_start(w, "text:span");
            oxml_w_attr(w, "text:style-name", sn);
            oxml_w_text(w, rn->text ? rn->text : "");
            oxml_w_end(w, "text:span");
        } else {
            oxml_w_text(w, rn->text ? rn->text : "");
        }
    }
}

static void w_para(oxml_w *w, const document *d, const doc_para *p, autoreg *reg) {
    int hl = heading_level_of(d, p->style_id);
    if (hl > 0) {
        char sn[24], lv[12];
        snprintf(sn, sizeof(sn), "Heading_20_%d", hl);
        snprintf(lv, sizeof(lv), "%d", hl);
        oxml_w_start(w, "text:h");
        oxml_w_attr(w, "text:style-name", sn);
        oxml_w_attr(w, "text:outline-level", lv);
        w_runs(w, p, reg);
        oxml_w_end(w, "text:h");
    } else {
        oxml_w_start(w, "text:p");
        w_runs(w, p, reg);
        oxml_w_end(w, "text:p");
    }
}

// Emit a list covering blocks [start, end) that are all list paragraphs at
// >= `level`, grouping by nesting level. Returns the index after the group.
static int w_list(oxml_w *w, const document *d, int start, int end,
                  int level, autoreg *reg) {
    const doc_block *b0 = &d->blocks[start];
    int ordered = b0->para->list_ordered;
    oxml_w_start(w, "text:list");
    oxml_w_attr(w, "text:style-name", ordered ? "Lnum" : "Lbul");
    int i = start;
    while (i < end) {
        const doc_block *b = &d->blocks[i];
        if (b->type != DBLK_PARA || !b->para || b->para->list_level < level) break;
        if (b->para->list_level > level) {
            // nested list nested inside the previous list-item is uncommon in v1;
            // emit as its own item wrapper
            oxml_w_start(w, "text:list-item");
            i = w_list(w, d, i, end, level + 1, reg);
            oxml_w_end(w, "text:list-item");
            continue;
        }
        oxml_w_start(w, "text:list-item");
        oxml_w_start(w, "text:p");
        w_runs(w, b->para, reg);
        oxml_w_end(w, "text:p");
        // absorb any immediately-following deeper items into this item
        int j = i + 1;
        if (j < end && d->blocks[j].type == DBLK_PARA && d->blocks[j].para &&
            d->blocks[j].para->list_level > level) {
            j = w_list(w, d, j, end, level + 1, reg);
        }
        oxml_w_end(w, "text:list-item");
        i = j;
    }
    oxml_w_end(w, "text:list");
    return i;
}

static void w_table(oxml_w *w, const doc_table *t, autoreg *reg) {
    oxml_w_start(w, "table:table");
    oxml_w_attr(w, "table:name", "Table1");
    oxml_w_start(w, "table:table-column");
    char rc[16]; snprintf(rc, sizeof(rc), "%d", t->cols);
    oxml_w_attr(w, "table:number-columns-repeated", rc);
    oxml_w_end(w, "table:table-column");
    for (int r = 0; r < t->rows; r++) {
        oxml_w_start(w, "table:table-row");
        for (int c = 0; c < t->cols; c++) {
            oxml_w_start(w, "table:table-cell");
            oxml_w_attr(w, "office:value-type", "string");
            doc_para *cp = t->cells ? t->cells[r * t->cols + c] : NULL;
            oxml_w_start(w, "text:p");
            if (cp) w_runs(w, cp, reg);
            oxml_w_end(w, "text:p");
            oxml_w_end(w, "table:table-cell");
        }
        oxml_w_end(w, "table:table-row");
    }
    oxml_w_end(w, "table:table");
}

// declare all ODF namespaces on a root element
static void decl_ns(oxml_w *w) {
    oxml_w_attr(w, "xmlns:office", NS_OFFICE);
    oxml_w_attr(w, "xmlns:style",  NS_STYLE);
    oxml_w_attr(w, "xmlns:text",   NS_TEXT);
    oxml_w_attr(w, "xmlns:table",  NS_TABLE);
    oxml_w_attr(w, "xmlns:draw",   NS_DRAW);
    oxml_w_attr(w, "xmlns:fo",     NS_FO);
    oxml_w_attr(w, "xmlns:xlink",  "http://www.w3.org/1999/xlink");
    oxml_w_attr(w, "office:version", "1.2");
}

static unsigned char *build_content(const document *d, autoreg *reg,
                                    int *used_bul, int *used_num, unsigned long *outlen) {
    oxml_w *w = oxml_w_new();
    if (!w) return NULL;
    oxml_w_decl(w);
    oxml_w_start(w, "office:document-content");
    decl_ns(w);

    // --- automatic-styles ---
    oxml_w_start(w, "office:automatic-styles");
    for (int i = 0; i < reg->n; i++) {
        oxml_w_start(w, "style:style");
        oxml_w_attr(w, "style:name", reg->v[i].name);
        oxml_w_attr(w, "style:family", "text");
        oxml_w_start(w, "style:text-properties");
        if (reg->v[i].fmt.bold)      oxml_w_attr(w, "fo:font-weight", "bold");
        if (reg->v[i].fmt.italic)    oxml_w_attr(w, "fo:font-style", "italic");
        if (reg->v[i].fmt.underline) { oxml_w_attr(w, "style:text-underline-style", "solid");
                                        oxml_w_attr(w, "style:text-underline-width", "auto");
                                        oxml_w_attr(w, "style:text-underline-color", "font-color"); }
        if (reg->v[i].fmt.strike)    oxml_w_attr(w, "style:text-line-through-style", "solid");
        oxml_w_end(w, "style:text-properties");
        oxml_w_end(w, "style:style");
    }
    // detect list usage
    for (int i = 0; i < d->nblk; i++) {
        const doc_block *b = &d->blocks[i];
        if (b->type == DBLK_PARA && b->para && b->para->list_level >= 0) {
            if (b->para->list_ordered) *used_num = 1; else *used_bul = 1;
        }
    }
    if (*used_bul) {
        oxml_w_start(w, "text:list-style");
        oxml_w_attr(w, "style:name", "Lbul");
        oxml_w_start(w, "text:list-level-style-bullet");
        oxml_w_attr(w, "text:level", "1");
        oxml_w_attr(w, "text:bullet-char", "\xe2\x80\xa2"); // U+2022
        oxml_w_end(w, "text:list-level-style-bullet");
        oxml_w_end(w, "text:list-style");
    }
    if (*used_num) {
        oxml_w_start(w, "text:list-style");
        oxml_w_attr(w, "style:name", "Lnum");
        oxml_w_start(w, "text:list-level-style-number");
        oxml_w_attr(w, "text:level", "1");
        oxml_w_attr(w, "style:num-format", "1");
        oxml_w_attr(w, "text:num-suffix", ".");
        oxml_w_end(w, "text:list-level-style-number");
        oxml_w_end(w, "text:list-style");
    }
    oxml_w_end(w, "office:automatic-styles");

    // --- body ---
    oxml_w_start(w, "office:body");
    oxml_w_start(w, "office:text");
    int i = 0;
    while (i < d->nblk) {
        const doc_block *b = &d->blocks[i];
        if (b->type == DBLK_PARA && b->para) {
            if (b->para->list_level >= 0) {
                i = w_list(w, d, i, d->nblk, 0, reg);
                continue;
            }
            w_para(w, d, b->para, reg);
        } else if (b->type == DBLK_TABLE && b->table) {
            w_table(w, b->table, reg);
        } else if (b->type == DBLK_IMAGE && b->image) {
            oxml_w_start(w, "text:p");
            oxml_w_start(w, "draw:frame");
            oxml_w_attr(w, "draw:name", b->image->id[0] ? b->image->id : "Image");
            oxml_w_start(w, "draw:image");
            char hp[320];
            snprintf(hp, sizeof(hp), "Pictures/%s", b->image->id[0] ? b->image->id : "image1.png");
            oxml_w_attr(w, "xlink:href", hp);
            oxml_w_attr(w, "xlink:type", "simple");
            oxml_w_end(w, "draw:image");
            oxml_w_end(w, "draw:frame");
            oxml_w_end(w, "text:p");
        }
        i++;
    }
    oxml_w_end(w, "office:text");
    oxml_w_end(w, "office:body");
    oxml_w_end(w, "office:document-content");

    unsigned long len = 0;
    const char *s = oxml_w_cstr(w, &len);
    unsigned char *copy = NULL;
    if (s) { copy = (unsigned char *)malloc(len ? len : 1); if (copy) memcpy(copy, s, len); }
    oxml_w_free(w);
    if (outlen) *outlen = len;
    return copy;
}

static unsigned char *build_styles(const document *d, unsigned long *outlen) {
    oxml_w *w = oxml_w_new();
    if (!w) return NULL;
    oxml_w_decl(w);
    oxml_w_start(w, "office:document-styles");
    decl_ns(w);
    oxml_w_start(w, "office:styles");
    // emit heading paragraph styles for the levels the document uses
    int emitted[10]; for (int i = 0; i < 10; i++) emitted[i] = 0;
    for (int i = 0; i < d->nblk; i++) {
        const doc_block *b = &d->blocks[i];
        if (b->type != DBLK_PARA || !b->para) continue;
        int hl = heading_level_of(d, b->para->style_id);
        if (hl < 1 || hl > 9 || emitted[hl]) continue;
        emitted[hl] = 1;
        char sn[24], dn[16], sz[8];
        snprintf(sn, sizeof(sn), "Heading_20_%d", hl);
        snprintf(dn, sizeof(dn), "Heading %d", hl);
        int pts = 20 - hl; if (pts < 10) pts = 10;
        snprintf(sz, sizeof(sz), "%dpt", pts);
        oxml_w_start(w, "style:style");
        oxml_w_attr(w, "style:name", sn);
        oxml_w_attr(w, "style:display-name", dn);
        oxml_w_attr(w, "style:family", "paragraph");
        oxml_w_start(w, "style:text-properties");
        oxml_w_attr(w, "fo:font-weight", "bold");
        oxml_w_attr(w, "fo:font-size", sz);
        oxml_w_end(w, "style:text-properties");
        oxml_w_end(w, "style:style");
    }
    oxml_w_end(w, "office:styles");
    oxml_w_end(w, "office:document-styles");
    unsigned long len = 0;
    const char *s = oxml_w_cstr(w, &len);
    unsigned char *copy = NULL;
    if (s) { copy = (unsigned char *)malloc(len ? len : 1); if (copy) memcpy(copy, s, len); }
    oxml_w_free(w);
    if (outlen) *outlen = len;
    return copy;
}

// minimal manifest listing the parts we wrote
static unsigned char *build_manifest(const document *d, unsigned long *outlen) {
    oxml_w *w = oxml_w_new();
    if (!w) return NULL;
    oxml_w_decl(w);
    oxml_w_start(w, "manifest:manifest");
    oxml_w_attr(w, "xmlns:manifest", "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0");
    oxml_w_attr(w, "manifest:version", "1.2");
    oxml_w_start(w, "manifest:file-entry");
    oxml_w_attr(w, "manifest:full-path", "/");
    oxml_w_attr(w, "manifest:media-type", MIME_TEXT);
    oxml_w_end(w, "manifest:file-entry");
    oxml_w_start(w, "manifest:file-entry");
    oxml_w_attr(w, "manifest:full-path", "content.xml");
    oxml_w_attr(w, "manifest:media-type", "text/xml");
    oxml_w_end(w, "manifest:file-entry");
    oxml_w_start(w, "manifest:file-entry");
    oxml_w_attr(w, "manifest:full-path", "styles.xml");
    oxml_w_attr(w, "manifest:media-type", "text/xml");
    oxml_w_end(w, "manifest:file-entry");
    for (int i = 0; i < d->nimg; i++) {
        char full[320];
        snprintf(full, sizeof(full), "Pictures/%s", d->images[i].id[0] ? d->images[i].id : "image1.png");
        oxml_w_start(w, "manifest:file-entry");
        oxml_w_attr(w, "manifest:full-path", full);
        oxml_w_attr(w, "manifest:media-type", "application/octet-stream");
        oxml_w_end(w, "manifest:file-entry");
    }
    oxml_w_end(w, "manifest:manifest");
    unsigned long len = 0;
    const char *s = oxml_w_cstr(w, &len);
    unsigned char *copy = NULL;
    if (s) { copy = (unsigned char *)malloc(len ? len : 1); if (copy) memcpy(copy, s, len); }
    oxml_w_free(w);
    if (outlen) *outlen = len;
    return copy;
}

int odt_save(const document *d, unsigned char **out, unsigned long *out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!d || !out || !out_len) return -1;

    autoreg reg; memset(&reg, 0, sizeof(reg));
    collect_spans(d, &reg);

    int used_bul = 0, used_num = 0;
    unsigned long clen = 0, slen = 0, mlen = 0;
    unsigned char *content = build_content(d, &reg, &used_bul, &used_num, &clen);
    unsigned char *styles  = build_styles(d, &slen);
    unsigned char *manifest = build_manifest(d, &mlen);
    free(reg.v);
    if (!content || !styles || !manifest) {
        free(content); free(styles); free(manifest);
        return -1;
    }

    opc_pkg *pkg = opc_new();
    int ok = pkg != NULL;
    if (ok) ok = opc_put(pkg, "mimetype", (const unsigned char *)MIME_TEXT,
                         (unsigned long)strlen(MIME_TEXT)) == 0;
    if (ok) ok = opc_put(pkg, "content.xml", content, clen) == 0;
    if (ok) ok = opc_put(pkg, "styles.xml", styles, slen) == 0;
    if (ok) ok = opc_put(pkg, "META-INF/manifest.xml", manifest, mlen) == 0;
    // inline image binaries
    for (int i = 0; ok && i < d->nimg; i++) {
        if (!d->images[i].bytes || !d->images[i].nbytes) continue;
        char full[320];
        snprintf(full, sizeof(full), "Pictures/%s",
                 d->images[i].id[0] ? d->images[i].id : "image1.png");
        ok = opc_put(pkg, full, d->images[i].bytes, d->images[i].nbytes) == 0;
    }
    free(content); free(styles); free(manifest);
    if (!ok) { opc_free(pkg); return -1; }

    unsigned long zl = 0;
    unsigned char *z = opc_save_mem_odf(pkg, &zl);   // mimetype first + stored
    opc_free(pkg);
    if (!z) return -1;
    *out = z; *out_len = zl;
    return 0;
}
