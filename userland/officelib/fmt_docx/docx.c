// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// DOCX (WordprocessingML) import + export. Owner: format agent 4 (fmt_docx).
//
// Maps WordprocessingML <-> the shared `document` model (docmodel.h) over the
// OPC (opc.h, a zip of XML parts) and the namespace-aware pull-parser/writer
// (oxml.h). v1 scope: paragraphs with runs, run formatting (b/i/u/strike, sz,
// color), paragraph pStyle/jc/numPr, simple tables, inline images. Out of v1:
// sections/headers/footers/fields/track-changes.
//
// Ownership discipline (per the model agent):
//  - opc frees the input package (and office_open_* frees the file buffer)
//    after load() returns, so every string/byte kept is COPIED into the model
//    (doc_para_add_run/doc_add_style/doc_add_image all copy).
//  - document.images realloc's on append: DBLK_IMAGE blocks record an image
//    INDEX during the walk and are wired to &d->images[idx] only after every
//    image has been added (see finalize_images).
#include "formats.h"
#include "opc.h"
#include "oxml.h"
#include <stdlib.h>
#include <string.h>

#define W_NS  "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
#define R_NS  "http://schemas.openxmlformats.org/officeDocument/2006/relationships"

// ---------------------------------------------------------------------------
// small freestanding helpers (no libc atoi/strcasecmp dependency)
// ---------------------------------------------------------------------------
static int streq(const char *a, const char *b) { return a && b && !strcmp(a, b); }

static int u_atoi(const char *s) {
    int v = 0, neg = 0;
    if (!s) return 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// parse an RRGGBB hex color; returns 0xRRGGBB, or 0xFFFFFFFF ("auto"/invalid).
static unsigned parse_color(const char *s) {
    if (!s) return 0xFFFFFFFFu;
    if (streq(s, "auto")) return 0xFFFFFFFFu;
    unsigned v = 0; int i;
    for (i = 0; i < 6; i++) {
        int h = hexval(s[i]);
        if (h < 0) return 0xFFFFFFFFu;
        v = (v << 4) | (unsigned)h;
    }
    return v & 0xFFFFFFu;
}

// write a decimal int into buf, return buf.
static const char *itoa10(int v, char *buf) {
    char tmp[16]; int i = 0, neg = 0;
    unsigned u;
    if (v < 0) { neg = 1; u = (unsigned)(-(long)v); } else u = (unsigned)v;
    if (u == 0) tmp[i++] = '0';
    while (u) { tmp[i++] = (char)('0' + (u % 10)); u /= 10; }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}

static const doc_runfmt RUNFMT_INHERIT = { 0, 0, 0, 0, 0, 0xFFFFFFFFu, 0 };

// ===========================================================================
// LOAD
// ===========================================================================
typedef struct {
    document *d;
    opc_pkg  *pkg;
    // styleId -> style index in d->styles
    struct { char id[64]; int idx; } smap[128];
    int nsmap;
    // numId -> ordered (1 = numbered, 0 = bullet)
    struct { int numId; int ordered; } nmap[64];
    int nnmap;
    // DBLK_IMAGE fixups: block index -> image index (images array may realloc)
    struct { int blk; int img; } ifix[64];
    int nifix;
} loadctx;

static int style_index_for(loadctx *c, const char *styleId) {
    if (!styleId || !*styleId) return -1;
    for (int i = 0; i < c->nsmap; i++)
        if (streq(c->smap[i].id, styleId)) return c->smap[i].idx;
    // referenced but undeclared: register a bare style so it round-trips
    int idx = doc_add_style(c->d, styleId, RUNFMT_INHERIT, DOC_ALIGN_L);
    if (idx >= 0 && c->nsmap < (int)(sizeof(c->smap) / sizeof(c->smap[0]))) {
        unsigned long k = 0;
        for (; styleId[k] && k < sizeof(c->smap[0].id) - 1; k++)
            c->smap[c->nsmap].id[k] = styleId[k];
        c->smap[c->nsmap].id[k] = 0;
        c->smap[c->nsmap].idx = idx;
        c->nsmap++;
    }
    return idx;
}

static int num_ordered(loadctx *c, int numId) {
    for (int i = 0; i < c->nnmap; i++)
        if (c->nmap[i].numId == numId) return c->nmap[i].ordered;
    return 0; // default bullet
}

// consume the current element's subtree (its START was just read at depth d).
static void skip_elt(oxml *x, int d) {
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e == OXML_ERR) return;
        if (e == OXML_END && oxml_depth(x) <= d) return;
    }
}

static int is_w(oxml *x) { return streq(oxml_ns(x), W_NS); }

// gather text of a w:t element (its START just read).
static void consume_t(oxml *x, char *buf, unsigned long cap, unsigned long *len) {
    int d = oxml_depth(x);
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e == OXML_ERR) return;
        if (e == OXML_TEXT) {
            const char *t = oxml_text(x);
            while (*t && *len + 1 < cap) buf[(*len)++] = *t++;
            buf[*len] = 0;
        } else if (e == OXML_END && oxml_depth(x) <= d) {
            return;
        }
    }
}

// consume a w:rPr subtree into fmt (START just read).
static void consume_rPr(oxml *x, doc_runfmt *f) {
    int d = oxml_depth(x);
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e == OXML_ERR) return;
        if (e == OXML_END && oxml_depth(x) <= d && streq(oxml_local(x), "rPr")) return;
        if (e != OXML_START || !is_w(x)) continue;
        const char *ln = oxml_local(x);
        if (streq(ln, "b")) {
            const char *v = oxml_attr_ns(x, W_NS, "val");
            f->bold = (!v || streq(v, "1") || streq(v, "true") || streq(v, "on")) ? 1 : 0;
        } else if (streq(ln, "i")) {
            const char *v = oxml_attr_ns(x, W_NS, "val");
            f->italic = (!v || streq(v, "1") || streq(v, "true") || streq(v, "on")) ? 1 : 0;
        } else if (streq(ln, "u")) {
            const char *v = oxml_attr_ns(x, W_NS, "val");
            f->underline = (v && streq(v, "none")) ? 0 : 1;
        } else if (streq(ln, "strike")) {
            const char *v = oxml_attr_ns(x, W_NS, "val");
            f->strike = (!v || streq(v, "1") || streq(v, "true") || streq(v, "on")) ? 1 : 0;
        } else if (streq(ln, "sz")) {
            const char *v = oxml_attr_ns(x, W_NS, "val");
            if (v) f->size = u_atoi(v) / 2; // half-points -> points
        } else if (streq(ln, "color")) {
            const char *v = oxml_attr_ns(x, W_NS, "val");
            f->color = parse_color(v);
        }
    }
}

// resolve a drawing's r:embed relationship to media bytes and add an image
// block. rId already read. block wiring deferred (see finalize_images).
static void add_image_from_rid(loadctx *c, const char *rid) {
    if (!rid || !*rid) return;
    char tgt[512];
    if (!opc_rel_target(c->pkg, "word/_rels/document.xml.rels", rid, tgt, sizeof(tgt)))
        return;
    // Targets are relative to the /word part directory.
    char part[600];
    unsigned long p = 0;
    if (tgt[0] == '/') {
        for (unsigned long i = 1; tgt[i] && p < sizeof(part) - 1; i++) part[p++] = tgt[i];
    } else {
        const char *pre = "word/";
        for (unsigned long i = 0; pre[i] && p < sizeof(part) - 1; i++) part[p++] = pre[i];
        for (unsigned long i = 0; tgt[i] && p < sizeof(part) - 1; i++) part[p++] = tgt[i];
    }
    part[p] = 0;
    opc_part *mp = opc_get(c->pkg, part);
    if (!mp || !mp->data || !mp->size) return;
    doc_add_image(c->d, rid, mp->data, mp->size);
    int img = c->d->nimg - 1;
    doc_block *b = doc_add_block(c->d, DBLK_IMAGE);
    if (!b) return;
    int blk = c->d->nblk - 1;
    if (c->nifix < (int)(sizeof(c->ifix) / sizeof(c->ifix[0]))) {
        c->ifix[c->nifix].blk = blk;
        c->ifix[c->nifix].img = img;
        c->nifix++;
    }
}

// find the first r:embed inside a w:drawing subtree (START just read).
static void consume_drawing(loadctx *c, oxml *x) {
    int d = oxml_depth(x);
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e == OXML_ERR) return;
        if (e == OXML_END && oxml_depth(x) <= d && streq(oxml_local(x), "drawing")) return;
        if (e == OXML_START && streq(oxml_local(x), "blip")) {
            const char *rid = oxml_attr_ns(x, R_NS, "embed");
            if (!rid) rid = oxml_attr(x, "embed");
            if (rid) add_image_from_rid(c, rid);
        }
    }
}

// consume a w:r subtree, appending a run to p (START just read).
static void consume_run(loadctx *c, oxml *x, doc_para *p) {
    int d = oxml_depth(x);
    doc_runfmt f = RUNFMT_INHERIT;
    static char tbuf[8192];
    unsigned long tlen = 0;
    int had_text = 0;
    tbuf[0] = 0;
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e == OXML_ERR) break;
        if (e == OXML_END && oxml_depth(x) <= d && streq(oxml_local(x), "r")) break;
        if (e != OXML_START) continue;
        const char *ln = oxml_local(x);
        if (is_w(x) && streq(ln, "rPr")) {
            consume_rPr(x, &f);
        } else if (is_w(x) && streq(ln, "t")) {
            consume_t(x, tbuf, sizeof(tbuf), &tlen);
            had_text = 1;
        } else if (is_w(x) && streq(ln, "tab")) {
            if (tlen + 1 < sizeof(tbuf)) { tbuf[tlen++] = '\t'; tbuf[tlen] = 0; }
            had_text = 1;
        } else if (is_w(x) && (streq(ln, "br") || streq(ln, "cr"))) {
            if (tlen + 1 < sizeof(tbuf)) { tbuf[tlen++] = '\n'; tbuf[tlen] = 0; }
            had_text = 1;
        } else if (streq(ln, "drawing")) {
            consume_drawing(c, x);
        } else {
            skip_elt(x, oxml_depth(x));
        }
    }
    if (had_text) doc_para_add_run(p, tbuf, f);
}

// consume a w:numPr subtree, setting list fields on p (START just read).
static void consume_numPr(loadctx *c, oxml *x, doc_para *p) {
    int d = oxml_depth(x);
    int ilvl = 0, numId = -1;
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e == OXML_ERR) break;
        if (e == OXML_END && oxml_depth(x) <= d && streq(oxml_local(x), "numPr")) break;
        if (e != OXML_START || !is_w(x)) continue;
        const char *ln = oxml_local(x);
        if (streq(ln, "ilvl")) {
            const char *v = oxml_attr_ns(x, W_NS, "val");
            if (v) ilvl = u_atoi(v);
        } else if (streq(ln, "numId")) {
            const char *v = oxml_attr_ns(x, W_NS, "val");
            if (v) numId = u_atoi(v);
        }
    }
    p->list_level = ilvl < 0 ? 0 : ilvl;
    p->list_ordered = (numId >= 0) ? num_ordered(c, numId) : 0;
}

// consume a w:pPr subtree (START just read).
static void consume_pPr(loadctx *c, oxml *x, doc_para *p) {
    int d = oxml_depth(x);
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e == OXML_ERR) return;
        if (e == OXML_END && oxml_depth(x) <= d && streq(oxml_local(x), "pPr")) return;
        if (e != OXML_START || !is_w(x)) continue;
        const char *ln = oxml_local(x);
        if (streq(ln, "pStyle")) {
            const char *v = oxml_attr_ns(x, W_NS, "val");
            p->style_id = style_index_for(c, v);
        } else if (streq(ln, "jc")) {
            const char *v = oxml_attr_ns(x, W_NS, "val");
            if (v) {
                if (streq(v, "center")) p->align = DOC_ALIGN_C;
                else if (streq(v, "right") || streq(v, "end")) p->align = DOC_ALIGN_R;
                else if (streq(v, "both") || streq(v, "distribute")) p->align = DOC_ALIGN_J;
                else p->align = DOC_ALIGN_L;
            }
        } else if (streq(ln, "numPr")) {
            consume_numPr(c, x, p);
        } else {
            skip_elt(x, oxml_depth(x));
        }
    }
}

// consume a w:p subtree into paragraph p (START just read).
static void consume_paragraph(loadctx *c, oxml *x, doc_para *p) {
    int d = oxml_depth(x);
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e == OXML_ERR) return;
        if (e == OXML_END && oxml_depth(x) <= d && streq(oxml_local(x), "p")) return;
        if (e != OXML_START) continue;
        const char *ln = oxml_local(x);
        if (is_w(x) && streq(ln, "pPr")) consume_pPr(c, x, p);
        else if (is_w(x) && streq(ln, "r")) consume_run(c, x, p);
        else if (is_w(x) && streq(ln, "hyperlink")) { /* descend: keep runs */ }
        else skip_elt(x, oxml_depth(x));
    }
}

// build a standalone paragraph (for a table cell) from a w:p subtree.
static doc_para *new_cell_para(loadctx *c, oxml *x) {
    doc_para *p = (doc_para *)calloc(1, sizeof(doc_para));
    if (!p) return 0;
    p->align = DOC_ALIGN_L;
    p->style_id = -1;
    p->list_level = -1;
    consume_paragraph(c, x, p);
    return p;
}

// consume a w:tc subtree; returns its (first) paragraph (START just read).
static doc_para *consume_tc(loadctx *c, oxml *x) {
    int d = oxml_depth(x);
    doc_para *cell = 0;
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e == OXML_ERR) break;
        if (e == OXML_END && oxml_depth(x) <= d && streq(oxml_local(x), "tc")) break;
        if (e != OXML_START || !is_w(x)) continue;
        const char *ln = oxml_local(x);
        if (streq(ln, "p")) {
            doc_para *cp = new_cell_para(c, x);
            if (!cell) cell = cp;
            else if (cp) {
                // extra paragraphs in a cell: fold their runs into the first
                for (int i = 0; i < cp->nrun; i++)
                    doc_para_add_run(cell, cp->runs[i].text, cp->runs[i].fmt);
                for (int i = 0; i < cp->nrun; i++) free(cp->runs[i].text);
                free(cp->runs); free(cp);
            }
        } else {
            skip_elt(x, oxml_depth(x));
        }
    }
    if (!cell) {
        cell = (doc_para *)calloc(1, sizeof(doc_para));
        if (cell) { cell->align = DOC_ALIGN_L; cell->style_id = -1; cell->list_level = -1; }
    }
    return cell;
}

// consume a w:tbl subtree, appending a DBLK_TABLE (START just read).
static void consume_table(loadctx *c, oxml *x) {
    int d = oxml_depth(x);
    // gather rows as arrays of cell paragraphs
    doc_para **cells = 0;
    int nrows = 0, ncols = 0, cap = 0, count = 0;
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e == OXML_ERR) break;
        if (e == OXML_END && oxml_depth(x) <= d && streq(oxml_local(x), "tbl")) break;
        if (e != OXML_START || !is_w(x)) continue;
        if (streq(oxml_local(x), "tr")) {
            int rd = oxml_depth(x);
            int rowcols = 0;
            oxml_evt e2;
            while ((e2 = oxml_next(x)) != OXML_EOF) {
                if (e2 == OXML_ERR) break;
                if (e2 == OXML_END && oxml_depth(x) <= rd && streq(oxml_local(x), "tr")) break;
                if (e2 != OXML_START || !is_w(x)) continue;
                if (streq(oxml_local(x), "tc")) {
                    doc_para *cell = consume_tc(c, x);
                    if (count == cap) {
                        int nc = cap ? cap * 2 : 8;
                        doc_para **nn = (doc_para **)realloc(cells, (size_t)nc * sizeof(*nn));
                        if (!nn) { if (cell) { free(cell->runs); free(cell); } break; }
                        cells = nn; cap = nc;
                    }
                    cells[count++] = cell;
                    rowcols++;
                } else {
                    skip_elt(x, oxml_depth(x));
                }
            }
            if (rowcols > ncols) ncols = rowcols;
            nrows++;
        } else {
            skip_elt(x, oxml_depth(x)); // tblPr, tblGrid, etc.
        }
    }
    if (nrows == 0 || ncols == 0) {
        for (int i = 0; i < count; i++) if (cells[i]) { free(cells[i]->runs); free(cells[i]); }
        free(cells);
        return;
    }
    // Build a rectangular rows*cols grid (row-major). Rows were appended
    // in order; distribute the flat cell list across ncols per row. Ragged
    // rows pad with empty paragraphs.
    doc_table *t = (doc_table *)calloc(1, sizeof(doc_table));
    if (!t) {
        for (int i = 0; i < count; i++) if (cells[i]) { free(cells[i]->runs); free(cells[i]); }
        free(cells);
        return;
    }
    t->rows = nrows; t->cols = ncols;
    t->cells = (doc_para **)calloc((size_t)nrows * ncols, sizeof(doc_para *));
    if (!t->cells) {
        for (int i = 0; i < count; i++) if (cells[i]) { free(cells[i]->runs); free(cells[i]); }
        free(cells); free(t);
        return;
    }
    // We tracked cells as a flat list but did not record per-row counts; for
    // v1 the common case is a rectangular table, so lay the flat list out
    // row-major over ncols and pad the tail. (Ragged tables degrade, not
    // crash.) count == nrows*perrow in the rectangular case.
    int idx = 0;
    for (int r = 0; r < nrows; r++) {
        for (int col = 0; col < ncols; col++) {
            doc_para *cp = (idx < count) ? cells[idx++] : 0;
            if (!cp) {
                cp = (doc_para *)calloc(1, sizeof(doc_para));
                if (cp) { cp->align = DOC_ALIGN_L; cp->style_id = -1; cp->list_level = -1; }
            }
            t->cells[r * ncols + col] = cp;
        }
    }
    // free any surplus (ragged) cells beyond the grid
    for (; idx < count; idx++)
        if (cells[idx]) { free(cells[idx]->runs); free(cells[idx]); }
    free(cells);
    doc_block *b = doc_add_block(c->d, DBLK_TABLE);
    if (!b) { /* leak-safe: hand ownership to free_table via a temp doc? */
        // no block: free the table we built
        for (int i = 0; i < nrows * ncols; i++)
            if (t->cells[i]) { free(t->cells[i]->runs); free(t->cells[i]); }
        free(t->cells); free(t);
        return;
    }
    b->table = t;
}

// --- styles.xml -------------------------------------------------------------
static void parse_styles(loadctx *c) {
    opc_part *sp = opc_get(c->pkg, "word/styles.xml");
    if (!sp || !sp->data) return;
    oxml *x = oxml_open((const char *)sp->data, sp->size);
    if (!x) return;
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e == OXML_ERR) break;
        if (e != OXML_START || !streq(oxml_ns(x), W_NS) || !streq(oxml_local(x), "style"))
            continue;
        int sd = oxml_depth(x);
        char sid[64]; sid[0] = 0;
        const char *v = oxml_attr_ns(x, W_NS, "styleId");
        if (v) { unsigned long k = 0; for (; v[k] && k < sizeof(sid) - 1; k++) sid[k] = v[k]; sid[k] = 0; }
        doc_runfmt f = RUNFMT_INHERIT;
        // scan the style's rPr for default formatting
        oxml_evt e2;
        while ((e2 = oxml_next(x)) != OXML_EOF) {
            if (e2 == OXML_ERR) break;
            if (e2 == OXML_END && oxml_depth(x) <= sd && streq(oxml_local(x), "style")) break;
            if (e2 == OXML_START && streq(oxml_ns(x), W_NS) && streq(oxml_local(x), "rPr"))
                consume_rPr(x, &f);
        }
        if (sid[0]) {
            int idx = doc_add_style(c->d, sid, f, DOC_ALIGN_L);
            if (idx >= 0 && c->nsmap < (int)(sizeof(c->smap) / sizeof(c->smap[0]))) {
                unsigned long k = 0;
                for (; sid[k] && k < sizeof(c->smap[0].id) - 1; k++) c->smap[c->nsmap].id[k] = sid[k];
                c->smap[c->nsmap].id[k] = 0;
                c->smap[c->nsmap].idx = idx;
                c->nsmap++;
            }
        }
    }
    oxml_close(x);
}

// --- numbering.xml ----------------------------------------------------------
static void parse_numbering(loadctx *c) {
    opc_part *np = opc_get(c->pkg, "word/numbering.xml");
    if (!np || !np->data) return;
    // pass 1: abstractNumId -> ordered (from lvl 0 numFmt)
    struct { int aid; int ordered; } amap[64];
    int namap = 0;
    oxml *x = oxml_open((const char *)np->data, np->size);
    if (!x) return;
    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e == OXML_ERR) break;
        if (e != OXML_START || !streq(oxml_ns(x), W_NS)) continue;
        if (streq(oxml_local(x), "abstractNum")) {
            int ad = oxml_depth(x);
            const char *v = oxml_attr_ns(x, W_NS, "abstractNumId");
            int aid = v ? u_atoi(v) : -1;
            int ordered = 0, got0 = 0;
            oxml_evt e2;
            int in_lvl0 = 0, lvld = -1;
            while ((e2 = oxml_next(x)) != OXML_EOF) {
                if (e2 == OXML_ERR) break;
                if (e2 == OXML_END && oxml_depth(x) <= ad && streq(oxml_local(x), "abstractNum")) break;
                if (e2 == OXML_START && streq(oxml_ns(x), W_NS) && streq(oxml_local(x), "lvl")) {
                    const char *lv = oxml_attr_ns(x, W_NS, "ilvl");
                    if (!got0 && (!lv || u_atoi(lv) == 0)) { in_lvl0 = 1; lvld = oxml_depth(x); }
                }
                if (e2 == OXML_END && lvld >= 0 && oxml_depth(x) <= lvld) { if (in_lvl0) got0 = 1; in_lvl0 = 0; lvld = -1; }
                if (in_lvl0 && e2 == OXML_START && streq(oxml_ns(x), W_NS) && streq(oxml_local(x), "numFmt")) {
                    const char *fv = oxml_attr_ns(x, W_NS, "val");
                    if (fv && !streq(fv, "bullet") && !streq(fv, "none")) ordered = 1;
                }
            }
            if (aid >= 0 && namap < (int)(sizeof(amap) / sizeof(amap[0]))) {
                amap[namap].aid = aid; amap[namap].ordered = ordered; namap++;
            }
        }
    }
    oxml_close(x);
    // pass 2: numId -> abstractNumId -> ordered
    x = oxml_open((const char *)np->data, np->size);
    if (!x) return;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e == OXML_ERR) break;
        if (e != OXML_START || !streq(oxml_ns(x), W_NS) || !streq(oxml_local(x), "num")) continue;
        int nd = oxml_depth(x);
        const char *nv = oxml_attr_ns(x, W_NS, "numId");
        int numId = nv ? u_atoi(nv) : -1;
        int aid = -1;
        oxml_evt e2;
        while ((e2 = oxml_next(x)) != OXML_EOF) {
            if (e2 == OXML_ERR) break;
            if (e2 == OXML_END && oxml_depth(x) <= nd && streq(oxml_local(x), "num")) break;
            if (e2 == OXML_START && streq(oxml_ns(x), W_NS) && streq(oxml_local(x), "abstractNumId")) {
                const char *av = oxml_attr_ns(x, W_NS, "val");
                if (av) aid = u_atoi(av);
            }
        }
        int ordered = 0;
        for (int i = 0; i < namap; i++) if (amap[i].aid == aid) { ordered = amap[i].ordered; break; }
        if (numId >= 0 && c->nnmap < (int)(sizeof(c->nmap) / sizeof(c->nmap[0]))) {
            c->nmap[c->nnmap].numId = numId; c->nmap[c->nnmap].ordered = ordered; c->nnmap++;
        }
    }
    oxml_close(x);
}

static void finalize_images(loadctx *c) {
    for (int i = 0; i < c->nifix; i++) {
        int blk = c->ifix[i].blk, img = c->ifix[i].img;
        if (blk >= 0 && blk < c->d->nblk && img >= 0 && img < c->d->nimg)
            c->d->blocks[blk].image = &c->d->images[img];
    }
}

int docx_load(const unsigned char *b, unsigned long n, document **out) {
    if (out) *out = 0;
    if (!b || !n) return -1;
    opc_pkg *pkg = opc_open_mem(b, n);
    if (!pkg) return -1;
    opc_part *dp = opc_get(pkg, "word/document.xml");
    if (!dp || !dp->data) { opc_free(pkg); return -1; }
    document *d = doc_new();
    if (!d) { opc_free(pkg); return -1; }

    loadctx c;
    memset(&c, 0, sizeof(c));
    c.d = d; c.pkg = pkg;
    parse_styles(&c);
    parse_numbering(&c);

    oxml *x = oxml_open((const char *)dp->data, dp->size);
    if (!x) { doc_free(d); opc_free(pkg); return -1; }

    // walk to w:body, then iterate its children
    oxml_evt e;
    int in_body = 0, body_depth = -1;
    while ((e = oxml_next(x)) != OXML_EOF) {
        if (e == OXML_ERR) break;
        if (!in_body) {
            if (e == OXML_START && streq(oxml_ns(x), W_NS) && streq(oxml_local(x), "body")) {
                in_body = 1; body_depth = oxml_depth(x);
            }
            continue;
        }
        if (e == OXML_END && oxml_depth(x) <= body_depth && streq(oxml_local(x), "body")) break;
        if (e != OXML_START || !streq(oxml_ns(x), W_NS)) continue;
        const char *ln = oxml_local(x);
        if (streq(ln, "p")) {
            doc_para *p = doc_add_para(d);
            if (p) consume_paragraph(&c, x, p);
        } else if (streq(ln, "tbl")) {
            consume_table(&c, x);
        } else {
            skip_elt(x, oxml_depth(x)); // sectPr, etc.
        }
    }
    oxml_close(x);
    finalize_images(&c);
    opc_free(pkg);
    *out = d;
    return 0;
}

// ===========================================================================
// SAVE
// ===========================================================================
static void w_bool_elt(oxml_w *w, const char *qname, int on) {
    if (!on) return;
    oxml_w_start(w, qname);
    oxml_w_end(w, qname);
}

static void write_runfmt(oxml_w *w, const doc_runfmt *f) {
    // emit only if something is set
    if (!f->bold && !f->italic && !f->underline && !f->strike &&
        f->size == 0 && f->color == 0xFFFFFFFFu)
        return;
    char nb[16];
    oxml_w_start(w, "w:rPr");
    w_bool_elt(w, "w:b", f->bold);
    w_bool_elt(w, "w:i", f->italic);
    if (f->underline) {
        oxml_w_start(w, "w:u"); oxml_w_attr(w, "w:val", "single"); oxml_w_end(w, "w:u");
    }
    w_bool_elt(w, "w:strike", f->strike);
    if (f->size > 0) {
        itoa10(f->size * 2, nb);
        oxml_w_start(w, "w:sz"); oxml_w_attr(w, "w:val", nb); oxml_w_end(w, "w:sz");
        oxml_w_start(w, "w:szCs"); oxml_w_attr(w, "w:val", nb); oxml_w_end(w, "w:szCs");
    }
    if (f->color != 0xFFFFFFFFu) {
        char hx[8];
        const char *H = "0123456789ABCDEF";
        unsigned v = f->color & 0xFFFFFFu;
        hx[0] = H[(v >> 20) & 0xF]; hx[1] = H[(v >> 16) & 0xF];
        hx[2] = H[(v >> 12) & 0xF]; hx[3] = H[(v >> 8) & 0xF];
        hx[4] = H[(v >> 4) & 0xF];  hx[5] = H[v & 0xF]; hx[6] = 0;
        oxml_w_start(w, "w:color"); oxml_w_attr(w, "w:val", hx); oxml_w_end(w, "w:color");
    }
    oxml_w_end(w, "w:rPr");
}

static const char *align_val(doc_align a) {
    switch (a) {
        case DOC_ALIGN_C: return "center";
        case DOC_ALIGN_R: return "right";
        case DOC_ALIGN_J: return "both";
        default: return 0;
    }
}

static void write_para(oxml_w *w, const document *d, const doc_para *p) {
    oxml_w_start(w, "w:p");
    const char *jc = align_val(p->align);
    int have_style = (p->style_id >= 0 && p->style_id < d->nstyle);
    if (have_style || jc || p->list_level >= 0) {
        oxml_w_start(w, "w:pPr");
        if (have_style) {
            oxml_w_start(w, "w:pStyle");
            oxml_w_attr(w, "w:val", d->styles[p->style_id].name ? d->styles[p->style_id].name : "");
            oxml_w_end(w, "w:pStyle");
        }
        if (p->list_level >= 0) {
            char nb[16];
            oxml_w_start(w, "w:numPr");
            oxml_w_start(w, "w:ilvl");
            oxml_w_attr(w, "w:val", itoa10(p->list_level, nb));
            oxml_w_end(w, "w:ilvl");
            oxml_w_start(w, "w:numId");
            oxml_w_attr(w, "w:val", p->list_ordered ? "2" : "1");
            oxml_w_end(w, "w:numId");
            oxml_w_end(w, "w:numPr");
        }
        if (jc) {
            oxml_w_start(w, "w:jc"); oxml_w_attr(w, "w:val", jc); oxml_w_end(w, "w:jc");
        }
        oxml_w_end(w, "w:pPr");
    }
    for (int i = 0; i < p->nrun; i++) {
        oxml_w_start(w, "w:r");
        write_runfmt(w, &p->runs[i].fmt);
        oxml_w_start(w, "w:t");
        oxml_w_attr(w, "xml:space", "preserve");
        oxml_w_text(w, p->runs[i].text ? p->runs[i].text : "");
        oxml_w_end(w, "w:t");
        oxml_w_end(w, "w:r");
    }
    oxml_w_end(w, "w:p");
}

static void write_table(oxml_w *w, const document *d, const doc_table *t) {
    char nb[16];
    oxml_w_start(w, "w:tbl");
    // borders (helps real viewers; harmless to the oracle)
    oxml_w_start(w, "w:tblPr");
    oxml_w_start(w, "w:tblBorders");
    const char *edges[6] = { "w:top", "w:left", "w:bottom", "w:right", "w:insideH", "w:insideV" };
    for (int i = 0; i < 6; i++) {
        oxml_w_start(w, edges[i]);
        oxml_w_attr(w, "w:val", "single");
        oxml_w_attr(w, "w:sz", "4");
        oxml_w_end(w, edges[i]);
    }
    oxml_w_end(w, "w:tblBorders");
    oxml_w_end(w, "w:tblPr");
    oxml_w_start(w, "w:tblGrid");
    for (int col = 0; col < t->cols; col++) {
        oxml_w_start(w, "w:gridCol");
        oxml_w_attr(w, "w:w", "4680");
        oxml_w_end(w, "w:gridCol");
    }
    oxml_w_end(w, "w:tblGrid");
    for (int r = 0; r < t->rows; r++) {
        oxml_w_start(w, "w:tr");
        for (int col = 0; col < t->cols; col++) {
            oxml_w_start(w, "w:tc");
            oxml_w_start(w, "w:tcPr");
            oxml_w_start(w, "w:tcW");
            oxml_w_attr(w, "w:w", itoa10(4680, nb));
            oxml_w_attr(w, "w:type", "dxa");
            oxml_w_end(w, "w:tcW");
            oxml_w_end(w, "w:tcPr");
            doc_para *cell = t->cells ? t->cells[r * t->cols + col] : 0;
            if (cell) write_para(w, d, cell);
            else { oxml_w_start(w, "w:p"); oxml_w_end(w, "w:p"); }
            oxml_w_end(w, "w:tc");
        }
        oxml_w_end(w, "w:tr");
    }
    oxml_w_end(w, "w:tbl");
}

// image content-type + extension by sniffing the encoded bytes
static void img_kind(const doc_image *im, const char **ext, const char **ct) {
    *ext = "png"; *ct = "image/png";
    if (im->nbytes >= 3 && im->bytes[0] == 0xFF && im->bytes[1] == 0xD8 && im->bytes[2] == 0xFF) {
        *ext = "jpeg"; *ct = "image/jpeg";
    } else if (im->nbytes >= 6 && im->bytes[0] == 'G' && im->bytes[1] == 'I' && im->bytes[2] == 'F') {
        *ext = "gif"; *ct = "image/gif";
    }
}

static int put_str(opc_pkg *pkg, const char *part, oxml_w *w) {
    unsigned long len = 0;
    const char *s = oxml_w_cstr(w, &len);
    int rc = opc_put(pkg, part, (const unsigned char *)s, len);
    oxml_w_free(w);
    return rc;
}

int docx_save(const document *d, unsigned char **out, unsigned long *out_len) {
    if (out) *out = 0;
    if (out_len) *out_len = 0;
    if (!d || !out || !out_len) return -1;

    // does the document use any list / any image?
    int has_list = 0, nimg = d->nimg;
    for (int i = 0; i < d->nblk; i++)
        if (d->blocks[i].type == DBLK_PARA && d->blocks[i].para && d->blocks[i].para->list_level >= 0)
            has_list = 1;

    opc_pkg *pkg = opc_new();
    if (!pkg) return -1;

    // --- word/document.xml ---
    {
        oxml_w *w = oxml_w_new();
        if (!w) { opc_free(pkg); return -1; }
        oxml_w_decl(w);
        oxml_w_start(w, "w:document");
        oxml_w_attr(w, "xmlns:w", W_NS);
        oxml_w_attr(w, "xmlns:r", R_NS);
        oxml_w_attr(w, "xmlns:wp", "http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing");
        oxml_w_attr(w, "xmlns:a", "http://schemas.openxmlformats.org/drawingml/2006/main");
        oxml_w_start(w, "w:body");
        int imgseq = 0;
        for (int i = 0; i < d->nblk; i++) {
            const doc_block *bl = &d->blocks[i];
            if (bl->type == DBLK_PARA && bl->para) {
                write_para(w, d, bl->para);
            } else if (bl->type == DBLK_TABLE && bl->table) {
                write_table(w, d, bl->table);
            } else if (bl->type == DBLK_IMAGE && bl->image) {
                // rId assignment: styles=rId1, numbering=rId2, images from rId3
                char rid[16]; char nb[16];
                int rnum = 3 + imgseq;
                rid[0] = 'r'; rid[1] = 'I'; rid[2] = 'd';
                itoa10(rnum, rid + 3);
                oxml_w_start(w, "w:p"); oxml_w_start(w, "w:r");
                oxml_w_start(w, "w:drawing");
                oxml_w_start(w, "wp:inline");
                oxml_w_start(w, "wp:extent");
                oxml_w_attr(w, "cx", itoa10((bl->image->w > 0 ? bl->image->w : 200) * 9525, nb));
                oxml_w_attr(w, "cy", itoa10((bl->image->h > 0 ? bl->image->h : 200) * 9525, nb));
                oxml_w_end(w, "wp:extent");
                oxml_w_start(w, "a:graphic");
                oxml_w_start(w, "a:graphicData");
                oxml_w_attr(w, "uri", "http://schemas.openxmlformats.org/drawingml/2006/picture");
                oxml_w_start(w, "a:blip");
                oxml_w_attr(w, "r:embed", rid);
                oxml_w_end(w, "a:blip");
                oxml_w_end(w, "a:graphicData");
                oxml_w_end(w, "a:graphic");
                oxml_w_end(w, "wp:inline");
                oxml_w_end(w, "w:drawing");
                oxml_w_end(w, "w:r"); oxml_w_end(w, "w:p");
                imgseq++;
            }
        }
        oxml_w_start(w, "w:sectPr");
        oxml_w_start(w, "w:pgSz");
        oxml_w_attr(w, "w:w", "12240");
        oxml_w_attr(w, "w:h", "15840");
        oxml_w_end(w, "w:pgSz");
        oxml_w_end(w, "w:sectPr");
        oxml_w_end(w, "w:body");
        oxml_w_end(w, "w:document");
        if (put_str(pkg, "word/document.xml", w) != 0) { opc_free(pkg); return -1; }
    }

    // --- word/styles.xml ---
    {
        oxml_w *w = oxml_w_new();
        if (!w) { opc_free(pkg); return -1; }
        oxml_w_decl(w);
        oxml_w_start(w, "w:styles");
        oxml_w_attr(w, "xmlns:w", W_NS);
        // always provide a default Normal
        int have_normal = 0;
        for (int i = 0; i < d->nstyle; i++)
            if (streq(d->styles[i].name, "Normal")) have_normal = 1;
        if (!have_normal) {
            oxml_w_start(w, "w:style");
            oxml_w_attr(w, "w:type", "paragraph");
            oxml_w_attr(w, "w:styleId", "Normal");
            oxml_w_attr(w, "w:default", "1");
            oxml_w_start(w, "w:name"); oxml_w_attr(w, "w:val", "Normal"); oxml_w_end(w, "w:name");
            oxml_w_end(w, "w:style");
        }
        for (int i = 0; i < d->nstyle; i++) {
            const doc_style *s = &d->styles[i];
            const char *nm = s->name ? s->name : "Style";
            oxml_w_start(w, "w:style");
            oxml_w_attr(w, "w:type", "paragraph");
            oxml_w_attr(w, "w:styleId", nm);
            if (streq(nm, "Normal")) oxml_w_attr(w, "w:default", "1");
            oxml_w_start(w, "w:name"); oxml_w_attr(w, "w:val", nm); oxml_w_end(w, "w:name");
            write_runfmt(w, &s->runfmt);
            oxml_w_end(w, "w:style");
        }
        oxml_w_end(w, "w:styles");
        if (put_str(pkg, "word/styles.xml", w) != 0) { opc_free(pkg); return -1; }
    }

    // --- word/numbering.xml (only if a list is present) ---
    if (has_list) {
        oxml_w *w = oxml_w_new();
        if (!w) { opc_free(pkg); return -1; }
        oxml_w_decl(w);
        oxml_w_start(w, "w:numbering");
        oxml_w_attr(w, "xmlns:w", W_NS);
        // abstractNum 0 = bullet, 1 = decimal
        const char *fmt[2] = { "bullet", "decimal" };
        const char *txt[2] = { "\xe2\x80\xa2", "%1." };
        for (int a = 0; a < 2; a++) {
            char nb[16];
            oxml_w_start(w, "w:abstractNum");
            oxml_w_attr(w, "w:abstractNumId", itoa10(a, nb));
            oxml_w_start(w, "w:lvl");
            oxml_w_attr(w, "w:ilvl", "0");
            oxml_w_start(w, "w:numFmt"); oxml_w_attr(w, "w:val", fmt[a]); oxml_w_end(w, "w:numFmt");
            oxml_w_start(w, "w:lvlText"); oxml_w_attr(w, "w:val", txt[a]); oxml_w_end(w, "w:lvlText");
            oxml_w_end(w, "w:lvl");
            oxml_w_end(w, "w:abstractNum");
        }
        // num 1 -> abstract 0 (bullet), num 2 -> abstract 1 (decimal)
        for (int nnum = 1; nnum <= 2; nnum++) {
            char nb[16];
            oxml_w_start(w, "w:num");
            oxml_w_attr(w, "w:numId", itoa10(nnum, nb));
            oxml_w_start(w, "w:abstractNumId");
            oxml_w_attr(w, "w:val", itoa10(nnum - 1, nb));
            oxml_w_end(w, "w:abstractNumId");
            oxml_w_end(w, "w:num");
        }
        oxml_w_end(w, "w:numbering");
        if (put_str(pkg, "word/numbering.xml", w) != 0) { opc_free(pkg); return -1; }
    }

    // --- word/media/* images ---
    for (int i = 0; i < nimg; i++) {
        const doc_image *im = &d->images[i];
        if (!im->bytes || !im->nbytes) continue;
        const char *ext, *ct; img_kind(im, &ext, &ct);
        char part[64]; char nb[16];
        // "word/media/image<N>.<ext>"
        int pi = 0; const char *pre = "word/media/image";
        for (int k = 0; pre[k]; k++) part[pi++] = pre[k];
        itoa10(i + 1, nb);
        for (int k = 0; nb[k]; k++) part[pi++] = nb[k];
        part[pi++] = '.';
        for (int k = 0; ext[k]; k++) part[pi++] = ext[k];
        part[pi] = 0;
        if (opc_put(pkg, part, im->bytes, im->nbytes) != 0) { opc_free(pkg); return -1; }
    }

    // --- word/_rels/document.xml.rels ---
    {
        oxml_w *w = oxml_w_new();
        if (!w) { opc_free(pkg); return -1; }
        oxml_w_decl(w);
        oxml_w_start(w, "Relationships");
        oxml_w_attr(w, "xmlns", "http://schemas.openxmlformats.org/package/2006/relationships");
        oxml_w_start(w, "Relationship");
        oxml_w_attr(w, "Id", "rId1");
        oxml_w_attr(w, "Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles");
        oxml_w_attr(w, "Target", "styles.xml");
        oxml_w_end(w, "Relationship");
        oxml_w_start(w, "Relationship");
        oxml_w_attr(w, "Id", "rId2");
        oxml_w_attr(w, "Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering");
        oxml_w_attr(w, "Target", "numbering.xml");
        oxml_w_end(w, "Relationship");
        for (int i = 0; i < nimg; i++) {
            const doc_image *im = &d->images[i];
            if (!im->bytes || !im->nbytes) continue;
            const char *ext, *ct; img_kind(im, &ext, &ct);
            char rid[16], tgt[64], nb[16];
            rid[0] = 'r'; rid[1] = 'I'; rid[2] = 'd'; itoa10(3 + i, rid + 3);
            int ti = 0; const char *pre = "media/image";
            for (int k = 0; pre[k]; k++) tgt[ti++] = pre[k];
            itoa10(i + 1, nb);
            for (int k = 0; nb[k]; k++) tgt[ti++] = nb[k];
            tgt[ti++] = '.';
            for (int k = 0; ext[k]; k++) tgt[ti++] = ext[k];
            tgt[ti] = 0;
            oxml_w_start(w, "Relationship");
            oxml_w_attr(w, "Id", rid);
            oxml_w_attr(w, "Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image");
            oxml_w_attr(w, "Target", tgt);
            oxml_w_end(w, "Relationship");
        }
        oxml_w_end(w, "Relationships");
        if (put_str(pkg, "word/_rels/document.xml.rels", w) != 0) { opc_free(pkg); return -1; }
    }

    // --- _rels/.rels ---
    {
        oxml_w *w = oxml_w_new();
        if (!w) { opc_free(pkg); return -1; }
        oxml_w_decl(w);
        oxml_w_start(w, "Relationships");
        oxml_w_attr(w, "xmlns", "http://schemas.openxmlformats.org/package/2006/relationships");
        oxml_w_start(w, "Relationship");
        oxml_w_attr(w, "Id", "rId1");
        oxml_w_attr(w, "Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument");
        oxml_w_attr(w, "Target", "word/document.xml");
        oxml_w_end(w, "Relationship");
        oxml_w_end(w, "Relationships");
        if (put_str(pkg, "_rels/.rels", w) != 0) { opc_free(pkg); return -1; }
    }

    // --- [Content_Types].xml ---
    {
        oxml_w *w = oxml_w_new();
        if (!w) { opc_free(pkg); return -1; }
        oxml_w_decl(w);
        oxml_w_start(w, "Types");
        oxml_w_attr(w, "xmlns", "http://schemas.openxmlformats.org/package/2006/content-types");
        oxml_w_start(w, "Default");
        oxml_w_attr(w, "Extension", "rels");
        oxml_w_attr(w, "ContentType", "application/vnd.openxmlformats-package.relationships+xml");
        oxml_w_end(w, "Default");
        oxml_w_start(w, "Default");
        oxml_w_attr(w, "Extension", "xml");
        oxml_w_attr(w, "ContentType", "application/xml");
        oxml_w_end(w, "Default");
        // image defaults (only emit those actually used, but harmless to add all)
        int need_png = 0, need_jpeg = 0, need_gif = 0;
        for (int i = 0; i < nimg; i++) {
            const doc_image *im = &d->images[i];
            if (!im->bytes || !im->nbytes) continue;
            const char *ext, *ct; img_kind(im, &ext, &ct);
            if (streq(ext, "png")) need_png = 1;
            else if (streq(ext, "jpeg")) need_jpeg = 1;
            else if (streq(ext, "gif")) need_gif = 1;
        }
        if (need_png) { oxml_w_start(w, "Default"); oxml_w_attr(w, "Extension", "png"); oxml_w_attr(w, "ContentType", "image/png"); oxml_w_end(w, "Default"); }
        if (need_jpeg) { oxml_w_start(w, "Default"); oxml_w_attr(w, "Extension", "jpeg"); oxml_w_attr(w, "ContentType", "image/jpeg"); oxml_w_end(w, "Default"); }
        if (need_gif) { oxml_w_start(w, "Default"); oxml_w_attr(w, "Extension", "gif"); oxml_w_attr(w, "ContentType", "image/gif"); oxml_w_end(w, "Default"); }
        oxml_w_start(w, "Override");
        oxml_w_attr(w, "PartName", "/word/document.xml");
        oxml_w_attr(w, "ContentType", "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml");
        oxml_w_end(w, "Override");
        oxml_w_start(w, "Override");
        oxml_w_attr(w, "PartName", "/word/styles.xml");
        oxml_w_attr(w, "ContentType", "application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml");
        oxml_w_end(w, "Override");
        if (has_list) {
            oxml_w_start(w, "Override");
            oxml_w_attr(w, "PartName", "/word/numbering.xml");
            oxml_w_attr(w, "ContentType", "application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml");
            oxml_w_end(w, "Override");
        }
        oxml_w_end(w, "Types");
        if (put_str(pkg, "[Content_Types].xml", w) != 0) { opc_free(pkg); return -1; }
    }

    unsigned long zl = 0;
    unsigned char *z = opc_save_mem(pkg, &zl);
    opc_free(pkg);
    if (!z) return -1;
    *out = z;
    *out_len = zl;
    return 0;
}
