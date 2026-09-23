// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// PPTX (PresentationML) import + export. Format agent 7 (officepptx).
// See docs/OFFICE_SUITE_ARCHITECTURE.md.
//
// v1 scope: slides; per slide the title placeholder (ph type title|ctrTitle ->
// SHP_TITLE + slide.title), body/subtitle text boxes (txBody -> a:p -> a:r ->
// a:t; bullets/levels -> doc_para list fields) -> SHP_TEXTBOX; pictures
// (p:pic -> a:blip r:embed) -> SHP_IMAGE. Out of scope: animations,
// transitions, master/theme fidelity beyond a minimal valid set, tables/charts.
#include "formats.h"
#include "opc.h"
#include "oxml.h"
#include <stdlib.h>
#include <string.h>

#define PML "http://schemas.openxmlformats.org/presentationml/2006/main"
#define DML "http://schemas.openxmlformats.org/drawingml/2006/main"
#define REL "http://schemas.openxmlformats.org/officeDocument/2006/relationships"

// ---- small local helpers (no libc snprintf/itoa dependency) --------------

static char *xstrdup(const char *s) {
    if (!s) s = "";
    unsigned long n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

// append `add` to a heap string `dst` (which may be NULL); returns new buffer.
static char *str_append(char *dst, const char *add) {
    unsigned long dl = dst ? strlen(dst) : 0;
    unsigned long al = add ? strlen(add) : 0;
    char *n = (char *)realloc(dst, dl + al + 1);
    if (!n) return dst;
    memcpy(n + dl, add ? add : "", al);
    n[dl + al] = 0;
    return n;
}

// unsigned int -> decimal string into out (>=16 bytes).
static void u2s(unsigned int v, char *out) {
    char tmp[16];
    int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v && i < 15) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
}

// ---- import: temp paragraph buffer ---------------------------------------

typedef struct { char *text; int level; int bold, italic; } tpara;

typedef struct {
    tpara       *p;
    int          n, cap;
} tparas;

static void tp_reset(tparas *t) {
    for (int i = 0; i < t->n; i++) free(t->p[i].text);
    t->n = 0;
}
static void tp_free(tparas *t) { tp_reset(t); free(t->p); t->p = 0; t->cap = 0; }

static tpara *tp_new(tparas *t) {
    if (t->n == t->cap) {
        int nc = t->cap ? t->cap * 2 : 8;
        tpara *np = (tpara *)realloc(t->p, (size_t)nc * sizeof(tpara));
        if (!np) return 0;
        t->p = np; t->cap = nc;
    }
    tpara *r = &t->p[t->n++];
    r->text = 0; r->level = 0; r->bold = 0; r->italic = 0;
    return r;
}

// DrawingML boolean attribute: "1"/"true"/"on" = set, anything else = clear.
static int dml_bool(const char *v) {
    return v && (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'o' || v[0] == 'O');
}

// Emit a title/textbox shape from the collected paragraphs. is_title selects
// SHP_TITLE; as_list marks each paragraph a bullet (list_level>=0). A shape's
// paras are fully populated before the next slide_add_shape, per the model's
// realloc rule.
static void emit_text_shape(slide *s, int is_title, int as_list, tparas *t) {
    shape *sh = slide_add_shape(s, is_title ? SHP_TITLE : SHP_TEXTBOX);
    if (!sh) return;
    if (t->n <= 0) return;
    sh->paras = (doc_para *)calloc((size_t)t->n, sizeof(doc_para));
    if (!sh->paras) return;
    sh->npara = t->n;
    for (int i = 0; i < t->n; i++) {
        doc_para *p = &sh->paras[i];
        p->align = DOC_ALIGN_L;
        p->style_id = -1;
        p->list_level = as_list ? (t->p[i].level < 0 ? 0 : t->p[i].level) : -1;
        p->list_ordered = 0;
        doc_runfmt f;
        memset(&f, 0, sizeof(f));
        f.color = 0xFFFFFFFF; // inherit
        f.bold = t->p[i].bold;
        f.italic = t->p[i].italic;
        doc_para_add_run(p, t->p[i].text ? t->p[i].text : "", f);
    }
}

// Resolve `target` (which may contain ../ or ./) against directory `basedir`
// (e.g. "ppt/slides/"), writing into out.
static void resolve_path(const char *basedir, const char *target,
                         char *out, unsigned long cap) {
    char buf[600];
    unsigned long bl = 0;
    // start from basedir
    for (const char *q = basedir; *q && bl < sizeof(buf) - 1; q++) buf[bl++] = *q;
    const char *t = target;
    while (*t) {
        if (t[0] == '.' && t[1] == '.' && t[2] == '/') {
            // pop one directory segment from buf (drop trailing '/', then to prev '/')
            if (bl > 0 && buf[bl - 1] == '/') bl--;
            while (bl > 0 && buf[bl - 1] != '/') bl--;
            t += 3;
        } else if (t[0] == '.' && t[1] == '/') {
            t += 2;
        } else {
            while (*t && *t != '/' && bl < sizeof(buf) - 1) buf[bl++] = *t++;
            if (*t == '/' && bl < sizeof(buf) - 1) buf[bl++] = *t++;
        }
    }
    if (bl >= cap) bl = cap - 1;
    memcpy(out, buf, bl);
    out[bl] = 0;
}

// Build "<dir>/_rels/<file>.rels" from a part path "<dir>/<file>".
static void rels_for(const char *part, char *out, unsigned long cap) {
    const char *slash = 0;
    for (const char *q = part; *q; q++) if (*q == '/') slash = q;
    out[0] = 0;
    unsigned long o = 0;
    if (slash) {
        unsigned long dl = (unsigned long)(slash - part) + 1; // include '/'
        for (unsigned long i = 0; i < dl && o < cap - 1; i++) out[o++] = part[i];
        const char *r = "_rels/";
        for (int i = 0; r[i] && o < cap - 1; i++) out[o++] = r[i];
        for (const char *f = slash + 1; *f && o < cap - 1; f++) out[o++] = *f;
    } else {
        const char *r = "_rels/";
        for (int i = 0; r[i] && o < cap - 1; i++) out[o++] = r[i];
        for (const char *f = part; *f && o < cap - 1; f++) out[o++] = *f;
    }
    const char *ext = ".rels";
    for (int i = 0; ext[i] && o < cap - 1; i++) out[o++] = ext[i];
    out[o] = 0;
}

static int ns_is(const char *ns, const char *want) {
    return ns && strcmp(ns, want) == 0;
}

static void parse_slide(opc_pkg *pkg, const char *slide_part, presentation *pres) {
    slide *s = pres_add_slide(pres);
    if (!s) return;
    opc_part *sp = opc_get(pkg, slide_part);
    if (!sp || !sp->data) return;

    char rels[600];
    rels_for(slide_part, rels, sizeof(rels));
    char basedir[600];
    resolve_path("", slide_part, basedir, sizeof(basedir)); // = slide_part
    {   // trim to directory
        int last = -1;
        for (int i = 0; basedir[i]; i++) if (basedir[i] == '/') last = i;
        basedir[last + 1] = 0;
    }

    oxml *x = oxml_open((const char *)sp->data, sp->size);
    if (!x) return;

    tparas t; memset(&t, 0, sizeof(t));
    char cur_ph[32] = {0};
    int in_txbody = 0, in_para = 0, in_pic = 0;
    char blip_rid[128] = {0};

    oxml_evt e;
    while ((e = oxml_next(x)) != OXML_EOF && e != OXML_ERR) {
        if (e == OXML_START) {
            const char *ln = oxml_local(x);
            const char *ns = oxml_ns(x);
            if (ns_is(ns, PML) && strcmp(ln, "sp") == 0) {
                cur_ph[0] = 0; tp_reset(&t); in_txbody = 0; in_para = 0;
            } else if (ns_is(ns, PML) && strcmp(ln, "pic") == 0) {
                in_pic = 1; blip_rid[0] = 0;
            } else if (ns_is(ns, PML) && strcmp(ln, "ph") == 0) {
                const char *ty = oxml_attr(x, "type");
                if (ty) { int i = 0; for (; ty[i] && i < 31; i++) cur_ph[i] = ty[i]; cur_ph[i] = 0; }
                else cur_ph[0] = 0;
            } else if (ns_is(ns, PML) && strcmp(ln, "txBody") == 0) {
                in_txbody = 1;
            } else if (ns_is(ns, DML) && strcmp(ln, "p") == 0) {
                if (in_txbody) { tpara *np = tp_new(&t); if (np) { np->text = 0; np->level = 0; } in_para = 1; }
            } else if (ns_is(ns, DML) && strcmp(ln, "pPr") == 0) {
                if (in_txbody && in_para && t.n > 0) {
                    const char *lvl = oxml_attr(x, "lvl");
                    if (lvl) t.p[t.n - 1].level = (int)strtol(lvl, 0, 10);
                }
            } else if (ns_is(ns, DML) && strcmp(ln, "rPr") == 0) {
                // run properties: bold/italic carry on the paragraph's run.
                if (in_txbody && in_para && t.n > 0) {
                    if (dml_bool(oxml_attr(x, "b"))) t.p[t.n - 1].bold = 1;
                    if (dml_bool(oxml_attr(x, "i"))) t.p[t.n - 1].italic = 1;
                }
            } else if (ns_is(ns, DML) && strcmp(ln, "blip") == 0) {
                if (in_pic) {
                    const char *rid = oxml_attr_ns(x, REL, "embed");
                    if (rid) { int i = 0; for (; rid[i] && i < 127; i++) blip_rid[i] = rid[i]; blip_rid[i] = 0; }
                }
            }
        } else if (e == OXML_TEXT) {
            if (in_txbody && in_para && t.n > 0) {
                t.p[t.n - 1].text = str_append(t.p[t.n - 1].text, oxml_text(x));
            }
        } else if (e == OXML_END) {
            const char *ln = oxml_local(x);
            const char *ns = oxml_ns(x);
            if (ns_is(ns, PML) && strcmp(ln, "txBody") == 0) {
                in_txbody = 0; in_para = 0;
            } else if (ns_is(ns, DML) && strcmp(ln, "p") == 0) {
                if (in_txbody) in_para = 0;
            } else if (ns_is(ns, PML) && strcmp(ln, "sp") == 0) {
                int is_title = (strcmp(cur_ph, "title") == 0 || strcmp(cur_ph, "ctrTitle") == 0);
                int as_list  = (strcmp(cur_ph, "body") == 0);
                if (is_title && t.n > 0 && t.p[0].text && !s->title)
                    s->title = xstrdup(t.p[0].text);
                emit_text_shape(s, is_title, as_list, &t);
                tp_reset(&t); cur_ph[0] = 0;
            } else if (ns_is(ns, PML) && strcmp(ln, "pic") == 0) {
                if (blip_rid[0]) {
                    char tgt[600], part[600];
                    if (opc_rel_target(pkg, rels, blip_rid, tgt, sizeof(tgt))) {
                        resolve_path(basedir, tgt, part, sizeof(part));
                        opc_part *mp = opc_get(pkg, part);
                        if (mp && mp->data && mp->size) {
                            shape *sh = slide_add_shape(s, SHP_IMAGE);
                            if (sh) {
                                doc_image *im = (doc_image *)calloc(1, sizeof(doc_image));
                                if (im) {
                                    im->bytes = (unsigned char *)malloc(mp->size);
                                    if (im->bytes) { memcpy(im->bytes, mp->data, mp->size); im->nbytes = mp->size; }
                                    int i = 0; for (; blip_rid[i] && i < 63; i++) im->id[i] = blip_rid[i]; im->id[i] = 0;
                                    sh->image = im;
                                }
                            }
                        }
                    }
                }
                in_pic = 0; blip_rid[0] = 0;
            }
        }
    }
    oxml_close(x);
    tp_free(&t);
}

int pptx_load(const unsigned char *b, unsigned long n, presentation **out) {
    if (out) *out = 0;
    if (!b || !n || !out) return -1;
    opc_pkg *pkg = opc_open_mem(b, n);
    if (!pkg) return -1;
    presentation *pres = pres_new();
    if (!pres) { opc_free(pkg); return -1; }

    opc_part *pp = opc_get(pkg, "ppt/presentation.xml");
    if (pp && pp->data) {
        // ordered slide rIds from sldIdLst + slide size from sldSz.
        char (*rids)[128] = 0; int nr = 0, cap = 0;
        oxml *x = oxml_open((const char *)pp->data, pp->size);
        if (x) {
            oxml_evt e;
            while ((e = oxml_next(x)) != OXML_EOF && e != OXML_ERR) {
                if (e == OXML_START) {
                    const char *ln = oxml_local(x);
                    const char *ns = oxml_ns(x);
                    if (ns_is(ns, PML) && strcmp(ln, "sldId") == 0) {
                        const char *rid = oxml_attr_ns(x, REL, "id");
                        if (rid) {
                            if (nr == cap) {
                                int ncap = cap ? cap * 2 : 8;
                                void *nb = realloc(rids, (size_t)ncap * 128);
                                if (nb) { rids = nb; cap = ncap; }
                            }
                            if (nr < cap) { int i = 0; for (; rid[i] && i < 127; i++) rids[nr][i] = rid[i]; rids[nr][i] = 0; nr++; }
                        }
                    } else if (ns_is(ns, PML) && strcmp(ln, "sldSz") == 0) {
                        const char *cx = oxml_attr(x, "cx");
                        const char *cy = oxml_attr(x, "cy");
                        if (cx) pres->cx = (int)((long)strtol(cx, 0, 10) * 96 / 914400);
                        if (cy) pres->cy = (int)((long)strtol(cy, 0, 10) * 96 / 914400);
                    }
                }
            }
            oxml_close(x);
        }
        for (int i = 0; i < nr; i++) {
            char tgt[600], part[600];
            if (opc_rel_target(pkg, "ppt/_rels/presentation.xml.rels", rids[i], tgt, sizeof(tgt))) {
                resolve_path("ppt/", tgt, part, sizeof(part));
                parse_slide(pkg, part, pres);
            }
        }
        free(rids);
    }

    opc_free(pkg);
    *out = pres;
    return 0;
}

// ---- export ---------------------------------------------------------------

// image extension by magic; returns "png"/"jpg"/"bin".
static const char *img_ext(const doc_image *im) {
    if (im && im->nbytes >= 8 && im->bytes[0] == 0x89 && im->bytes[1] == 'P' &&
        im->bytes[2] == 'N' && im->bytes[3] == 'G') return "png";
    if (im && im->nbytes >= 3 && im->bytes[0] == 0xFF && im->bytes[1] == 0xD8 &&
        im->bytes[2] == 0xFF) return "jpg";
    return "bin";
}

static const char *ct_for_ext(const char *ext) {
    if (strcmp(ext, "png") == 0) return "image/png";
    if (strcmp(ext, "jpg") == 0) return "image/jpeg";
    return "application/octet-stream";
}

// Emit one slide's shapes into an oxml writer already positioned inside
// <p:spTree>. media_rid/media_name track image rels for this slide (rId2..).
static void write_slide_shapes(oxml_w *w, const slide *s, opc_pkg *pkg,
                               oxml_w *slide_rels, int *media_seq,
                               int *img_png, int *img_jpg) {
    int shid = 2;
    int rid = 2; // rId1 is the slideLayout
    for (int j = 0; j < s->nshape; j++) {
        const shape *sh = &s->shapes[j];
        char idbuf[16];
        if (sh->type == SHP_IMAGE) {
            if (!sh->image) continue;
            const char *ext = img_ext(sh->image);
            if (strcmp(ext, "png") == 0) (*img_png) = 1;
            else if (strcmp(ext, "jpg") == 0) (*img_jpg) = 1;
            (*media_seq)++;
            char num[16]; u2s((unsigned)(*media_seq), num);
            char media[64];
            {
                unsigned long o = 0; const char *pre = "ppt/media/image";
                for (int k = 0; pre[k]; k++) media[o++] = pre[k];
                for (int k = 0; num[k]; k++) media[o++] = num[k];
                media[o++] = '.';
                for (int k = 0; ext[k]; k++) media[o++] = ext[k];
                media[o] = 0;
            }
            opc_put(pkg, media, sh->image->bytes, sh->image->nbytes);
            char ridbuf[16]; u2s((unsigned)rid, ridbuf);
            char ridstr[24]; { ridstr[0]='r';ridstr[1]='I';ridstr[2]='d'; int k=0; for(;ridbuf[k];k++) ridstr[3+k]=ridbuf[k]; ridstr[3+k]=0; }
            // slide rels entry: rId -> ../media/imageN.ext
            oxml_w_start(slide_rels, "Relationship");
            oxml_w_attr(slide_rels, "Id", ridstr);
            oxml_w_attr(slide_rels, "Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image");
            {
                char tgt[64]; unsigned long o=0; const char *pre="../media/image";
                for (int k=0; pre[k]; k++) tgt[o++]=pre[k];
                for (int k=0; num[k]; k++) tgt[o++]=num[k];
                tgt[o++]='.'; for (int k=0; ext[k]; k++) tgt[o++]=ext[k]; tgt[o]=0;
                oxml_w_attr(slide_rels, "Target", tgt);
            }
            oxml_w_end(slide_rels, "Relationship");

            oxml_w_start(w, "p:pic");
              oxml_w_start(w, "p:nvPicPr");
                oxml_w_start(w, "p:cNvPr");
                  u2s((unsigned)shid++, idbuf); oxml_w_attr(w, "id", idbuf);
                  oxml_w_attr(w, "name", "Picture");
                oxml_w_end(w, "p:cNvPr");
                oxml_w_start(w, "p:cNvPicPr"); oxml_w_end(w, "p:cNvPicPr");
                oxml_w_start(w, "p:nvPr"); oxml_w_end(w, "p:nvPr");
              oxml_w_end(w, "p:nvPicPr");
              oxml_w_start(w, "p:blipFill");
                oxml_w_start(w, "a:blip"); oxml_w_attr(w, "r:embed", ridstr); oxml_w_end(w, "a:blip");
                oxml_w_start(w, "a:stretch"); oxml_w_start(w, "a:fillRect"); oxml_w_end(w, "a:fillRect"); oxml_w_end(w, "a:stretch");
              oxml_w_end(w, "p:blipFill");
              oxml_w_start(w, "p:spPr"); oxml_w_end(w, "p:spPr");
            oxml_w_end(w, "p:pic");
            rid++;
            continue;
        }

        // text shape (title or textbox)
        int is_title = (sh->type == SHP_TITLE);
        int as_list = 0;
        for (int k = 0; k < sh->npara; k++) if (sh->paras[k].list_level >= 0) { as_list = 1; break; }
        const char *phtype = is_title ? "title" : (as_list ? "body" : "subTitle");

        oxml_w_start(w, "p:sp");
          oxml_w_start(w, "p:nvSpPr");
            oxml_w_start(w, "p:cNvPr");
              u2s((unsigned)shid++, idbuf); oxml_w_attr(w, "id", idbuf);
              oxml_w_attr(w, "name", phtype);
            oxml_w_end(w, "p:cNvPr");
            oxml_w_start(w, "p:cNvSpPr"); oxml_w_end(w, "p:cNvSpPr");
            oxml_w_start(w, "p:nvPr");
              oxml_w_start(w, "p:ph"); oxml_w_attr(w, "type", phtype); oxml_w_end(w, "p:ph");
            oxml_w_end(w, "p:nvPr");
          oxml_w_end(w, "p:nvSpPr");
          oxml_w_start(w, "p:spPr"); oxml_w_end(w, "p:spPr");
          oxml_w_start(w, "p:txBody");
            oxml_w_start(w, "a:bodyPr"); oxml_w_end(w, "a:bodyPr");
            oxml_w_start(w, "a:lstStyle"); oxml_w_end(w, "a:lstStyle");
            for (int k = 0; k < sh->npara; k++) {
                const doc_para *p = &sh->paras[k];
                oxml_w_start(w, "a:p");
                if (p->list_level > 0) {
                    char lv[16]; u2s((unsigned)p->list_level, lv);
                    oxml_w_start(w, "a:pPr"); oxml_w_attr(w, "lvl", lv); oxml_w_end(w, "a:pPr");
                }
                if (p->nrun <= 0) {
                    oxml_w_start(w, "a:r");
                      oxml_w_start(w, "a:t"); oxml_w_text(w, ""); oxml_w_end(w, "a:t");
                    oxml_w_end(w, "a:r");
                } else {
                    for (int ri = 0; ri < p->nrun; ri++) {
                        const doc_run *rn = &p->runs[ri];
                        oxml_w_start(w, "a:r");
                        // DrawingML run properties: bold/italic via a:rPr b/i,
                        // emitted (only the set attrs) as the first child of a:r.
                        if (rn->fmt.bold || rn->fmt.italic) {
                            oxml_w_start(w, "a:rPr");
                            if (rn->fmt.bold)   oxml_w_attr(w, "b", "1");
                            if (rn->fmt.italic) oxml_w_attr(w, "i", "1");
                            oxml_w_end(w, "a:rPr");
                        }
                        oxml_w_start(w, "a:t"); oxml_w_text(w, rn->text ? rn->text : ""); oxml_w_end(w, "a:t");
                        oxml_w_end(w, "a:r");
                    }
                }
                oxml_w_end(w, "a:p");
            }
          oxml_w_end(w, "p:txBody");
        oxml_w_end(w, "p:sp");
    }
    (void)pkg;
}

// static minimal theme (borrowed layout from a generated deck; valid for
// PowerPoint / LibreOffice to open the package).
static const char *THEME1 =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
"<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Maytera\"><a:themeElements><a:clrScheme name=\"Maytera\"><a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/></a:dk1><a:lt1><a:sysClr val=\"window\" lastClr=\"FFFFFF\"/></a:lt1><a:dk2><a:srgbClr val=\"44546A\"/></a:dk2><a:lt2><a:srgbClr val=\"E7E6E6\"/></a:lt2><a:accent1><a:srgbClr val=\"4472C4\"/></a:accent1><a:accent2><a:srgbClr val=\"ED7D31\"/></a:accent2><a:accent3><a:srgbClr val=\"A5A5A5\"/></a:accent3><a:accent4><a:srgbClr val=\"FFC000\"/></a:accent4><a:accent5><a:srgbClr val=\"5B9BD5\"/></a:accent5><a:accent6><a:srgbClr val=\"70AD47\"/></a:accent6><a:hlink><a:srgbClr val=\"0563C1\"/></a:hlink><a:folHlink><a:srgbClr val=\"954F72\"/></a:folHlink></a:clrScheme><a:fontScheme name=\"Maytera\"><a:majorFont><a:latin typeface=\"Calibri Light\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:majorFont><a:minorFont><a:latin typeface=\"Calibri\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:minorFont></a:fontScheme><a:fmtScheme name=\"Maytera\"><a:fillStyleLst><a:solidFill><a:srgbClr val=\"FFFFFF\"/></a:solidFill><a:solidFill><a:srgbClr val=\"FFFFFF\"/></a:solidFill><a:solidFill><a:srgbClr val=\"FFFFFF\"/></a:solidFill></a:fillStyleLst><a:lnStyleLst><a:ln w=\"6350\"><a:solidFill><a:srgbClr val=\"000000\"/></a:solidFill></a:ln><a:ln w=\"12700\"><a:solidFill><a:srgbClr val=\"000000\"/></a:solidFill></a:ln><a:ln w=\"19050\"><a:solidFill><a:srgbClr val=\"000000\"/></a:solidFill></a:ln></a:lnStyleLst><a:effectStyleLst><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle></a:effectStyleLst><a:bgFillStyleLst><a:solidFill><a:srgbClr val=\"FFFFFF\"/></a:solidFill><a:solidFill><a:srgbClr val=\"FFFFFF\"/></a:solidFill><a:solidFill><a:srgbClr val=\"FFFFFF\"/></a:solidFill></a:bgFillStyleLst></a:fmtScheme></a:themeElements></a:theme>";

static const char *SLIDEMASTER1 =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
"<p:sldMaster xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/></p:spTree></p:cSld><p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/><p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/></p:sldLayoutIdLst><p:txStyles><p:titleStyle/><p:bodyStyle/><p:otherStyle/></p:txStyles></p:sldMaster>";

static const char *SLIDEMASTER1_RELS =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/><Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\" Target=\"../theme/theme1.xml\"/></Relationships>";

static const char *SLIDELAYOUT1 =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
"<p:sldLayout xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" type=\"title\" preserve=\"1\"><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/></p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>";

static const char *SLIDELAYOUT1_RELS =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"../slideMasters/slideMaster1.xml\"/></Relationships>";

static const char *ROOT_RELS =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
"<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"ppt/presentation.xml\"/></Relationships>";

static void put_str(opc_pkg *pkg, const char *name, const char *s) {
    opc_put(pkg, name, (const unsigned char *)s, (unsigned long)strlen(s));
}

int pptx_save(const presentation *p, unsigned char **out, unsigned long *out_len) {
    if (out) *out = 0;
    if (out_len) *out_len = 0;
    if (!p || !out || !out_len) return -1;
    opc_pkg *pkg = opc_new();
    if (!pkg) return -1;

    int ns = p->nslide;
    int img_png = 0, img_jpg = 0;

    // --- static parts ---
    put_str(pkg, "_rels/.rels", ROOT_RELS);
    put_str(pkg, "ppt/theme/theme1.xml", THEME1);
    put_str(pkg, "ppt/slideMasters/slideMaster1.xml", SLIDEMASTER1);
    put_str(pkg, "ppt/slideMasters/_rels/slideMaster1.xml.rels", SLIDEMASTER1_RELS);
    put_str(pkg, "ppt/slideLayouts/slideLayout1.xml", SLIDELAYOUT1);
    put_str(pkg, "ppt/slideLayouts/_rels/slideLayout1.xml.rels", SLIDELAYOUT1_RELS);

    // --- presentation.xml ---
    {
        oxml_w *w = oxml_w_new();
        oxml_w_decl(w);
        oxml_w_start(w, "p:presentation");
        oxml_w_attr(w, "xmlns:p", PML);
        oxml_w_attr(w, "xmlns:r", REL);
        oxml_w_attr(w, "xmlns:a", DML);
          oxml_w_start(w, "p:sldMasterIdLst");
            oxml_w_start(w, "p:sldMasterId"); oxml_w_attr(w, "id", "2147483648"); oxml_w_attr(w, "r:id", "rId1"); oxml_w_end(w, "p:sldMasterId");
          oxml_w_end(w, "p:sldMasterIdLst");
          oxml_w_start(w, "p:sldIdLst");
            for (int i = 0; i < ns; i++) {
                char idb[16]; u2s((unsigned)(256 + i), idb);
                char ridb[16]; u2s((unsigned)(2 + i), ridb);
                char rid[24]; rid[0]='r';rid[1]='I';rid[2]='d'; { int k=0; for(;ridb[k];k++) rid[3+k]=ridb[k]; rid[3+k]=0; }
                oxml_w_start(w, "p:sldId"); oxml_w_attr(w, "id", idb); oxml_w_attr(w, "r:id", rid); oxml_w_end(w, "p:sldId");
            }
          oxml_w_end(w, "p:sldIdLst");
          {
            int pxw = p->cx > 0 ? p->cx : 960;
            int pxh = p->cy > 0 ? p->cy : 720;
            char cx[24], cy[24];
            u2s((unsigned)((long)pxw * 914400 / 96), cx);
            u2s((unsigned)((long)pxh * 914400 / 96), cy);
            oxml_w_start(w, "p:sldSz"); oxml_w_attr(w, "cx", cx); oxml_w_attr(w, "cy", cy); oxml_w_end(w, "p:sldSz");
          }
          oxml_w_start(w, "p:notesSz"); oxml_w_attr(w, "cx", "6858000"); oxml_w_attr(w, "cy", "9144000"); oxml_w_end(w, "p:notesSz");
        oxml_w_end(w, "p:presentation");
        unsigned long l; const char *cs = oxml_w_cstr(w, &l);
        opc_put(pkg, "ppt/presentation.xml", (const unsigned char *)cs, l);
        oxml_w_free(w);
    }

    // --- presentation.xml.rels ---
    {
        oxml_w *w = oxml_w_new();
        oxml_w_decl(w);
        oxml_w_start(w, "Relationships");
        oxml_w_attr(w, "xmlns", "http://schemas.openxmlformats.org/package/2006/relationships");
          oxml_w_start(w, "Relationship"); oxml_w_attr(w, "Id", "rId1"); oxml_w_attr(w, "Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster"); oxml_w_attr(w, "Target", "slideMasters/slideMaster1.xml"); oxml_w_end(w, "Relationship");
          for (int i = 0; i < ns; i++) {
              char ridb[16]; u2s((unsigned)(2 + i), ridb);
              char rid[24]; rid[0]='r';rid[1]='I';rid[2]='d'; { int k=0; for(;ridb[k];k++) rid[3+k]=ridb[k]; rid[3+k]=0; }
              char nb[16]; u2s((unsigned)(i + 1), nb);
              char tgt[48]; { unsigned long o=0; const char*pre="slides/slide"; for(int k=0;pre[k];k++) tgt[o++]=pre[k]; for(int k=0;nb[k];k++) tgt[o++]=nb[k]; const char*e=".xml"; for(int k=0;e[k];k++) tgt[o++]=e[k]; tgt[o]=0; }
              oxml_w_start(w, "Relationship"); oxml_w_attr(w, "Id", rid); oxml_w_attr(w, "Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide"); oxml_w_attr(w, "Target", tgt); oxml_w_end(w, "Relationship");
          }
        oxml_w_end(w, "Relationships");
        unsigned long l; const char *cs = oxml_w_cstr(w, &l);
        opc_put(pkg, "ppt/_rels/presentation.xml.rels", (const unsigned char *)cs, l);
        oxml_w_free(w);
    }

    // --- slides + their rels + media ---
    int media_seq = 0;
    for (int i = 0; i < ns; i++) {
        const slide *s = &p->slides[i];
        oxml_w *w = oxml_w_new();
        oxml_w *sr = oxml_w_new();
        oxml_w_decl(sr);
        oxml_w_start(sr, "Relationships");
        oxml_w_attr(sr, "xmlns", "http://schemas.openxmlformats.org/package/2006/relationships");
        oxml_w_start(sr, "Relationship"); oxml_w_attr(sr, "Id", "rId1"); oxml_w_attr(sr, "Type", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout"); oxml_w_attr(sr, "Target", "../slideLayouts/slideLayout1.xml"); oxml_w_end(sr, "Relationship");

        oxml_w_decl(w);
        oxml_w_start(w, "p:sld");
        oxml_w_attr(w, "xmlns:p", PML);
        oxml_w_attr(w, "xmlns:a", DML);
        oxml_w_attr(w, "xmlns:r", REL);
          oxml_w_start(w, "p:cSld");
            oxml_w_start(w, "p:spTree");
              oxml_w_start(w, "p:nvGrpSpPr");
                oxml_w_start(w, "p:cNvPr"); oxml_w_attr(w, "id", "1"); oxml_w_attr(w, "name", ""); oxml_w_end(w, "p:cNvPr");
                oxml_w_start(w, "p:cNvGrpSpPr"); oxml_w_end(w, "p:cNvGrpSpPr");
                oxml_w_start(w, "p:nvPr"); oxml_w_end(w, "p:nvPr");
              oxml_w_end(w, "p:nvGrpSpPr");
              oxml_w_start(w, "p:grpSpPr"); oxml_w_end(w, "p:grpSpPr");
              write_slide_shapes(w, s, pkg, sr, &media_seq, &img_png, &img_jpg);
            oxml_w_end(w, "p:spTree");
          oxml_w_end(w, "p:cSld");
        oxml_w_end(w, "p:sld");
        oxml_w_end(sr, "Relationships");

        char nb[16]; u2s((unsigned)(i + 1), nb);
        char part[64]; { unsigned long o=0; const char*pre="ppt/slides/slide"; for(int k=0;pre[k];k++) part[o++]=pre[k]; for(int k=0;nb[k];k++) part[o++]=nb[k]; const char*e=".xml"; for(int k=0;e[k];k++) part[o++]=e[k]; part[o]=0; }
        char rpart[80]; { unsigned long o=0; const char*pre="ppt/slides/_rels/slide"; for(int k=0;pre[k];k++) rpart[o++]=pre[k]; for(int k=0;nb[k];k++) rpart[o++]=nb[k]; const char*e=".xml.rels"; for(int k=0;e[k];k++) rpart[o++]=e[k]; rpart[o]=0; }
        unsigned long l; const char *cs = oxml_w_cstr(w, &l);
        opc_put(pkg, part, (const unsigned char *)cs, l);
        unsigned long l2; const char *cs2 = oxml_w_cstr(sr, &l2);
        opc_put(pkg, rpart, (const unsigned char *)cs2, l2);
        oxml_w_free(w);
        oxml_w_free(sr);
    }

    // --- [Content_Types].xml ---
    {
        oxml_w *w = oxml_w_new();
        oxml_w_decl(w);
        oxml_w_start(w, "Types");
        oxml_w_attr(w, "xmlns", "http://schemas.openxmlformats.org/package/2006/content-types");
          oxml_w_start(w, "Default"); oxml_w_attr(w, "Extension", "rels"); oxml_w_attr(w, "ContentType", "application/vnd.openxmlformats-package.relationships+xml"); oxml_w_end(w, "Default");
          oxml_w_start(w, "Default"); oxml_w_attr(w, "Extension", "xml"); oxml_w_attr(w, "ContentType", "application/xml"); oxml_w_end(w, "Default");
          if (img_png) { oxml_w_start(w, "Default"); oxml_w_attr(w, "Extension", "png"); oxml_w_attr(w, "ContentType", "image/png"); oxml_w_end(w, "Default"); }
          if (img_jpg) { oxml_w_start(w, "Default"); oxml_w_attr(w, "Extension", "jpg"); oxml_w_attr(w, "ContentType", "image/jpeg"); oxml_w_end(w, "Default"); }
          oxml_w_start(w, "Override"); oxml_w_attr(w, "PartName", "/ppt/presentation.xml"); oxml_w_attr(w, "ContentType", "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml"); oxml_w_end(w, "Override");
          for (int i = 0; i < ns; i++) {
              char nb[16]; u2s((unsigned)(i + 1), nb);
              char pn[48]; { unsigned long o=0; const char*pre="/ppt/slides/slide"; for(int k=0;pre[k];k++) pn[o++]=pre[k]; for(int k=0;nb[k];k++) pn[o++]=nb[k]; const char*e=".xml"; for(int k=0;e[k];k++) pn[o++]=e[k]; pn[o]=0; }
              oxml_w_start(w, "Override"); oxml_w_attr(w, "PartName", pn); oxml_w_attr(w, "ContentType", "application/vnd.openxmlformats-officedocument.presentationml.slide+xml"); oxml_w_end(w, "Override");
          }
          oxml_w_start(w, "Override"); oxml_w_attr(w, "PartName", "/ppt/slideMasters/slideMaster1.xml"); oxml_w_attr(w, "ContentType", "application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml"); oxml_w_end(w, "Override");
          oxml_w_start(w, "Override"); oxml_w_attr(w, "PartName", "/ppt/slideLayouts/slideLayout1.xml"); oxml_w_attr(w, "ContentType", "application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml"); oxml_w_end(w, "Override");
          oxml_w_start(w, "Override"); oxml_w_attr(w, "PartName", "/ppt/theme/theme1.xml"); oxml_w_attr(w, "ContentType", "application/vnd.openxmlformats-officedocument.theme+xml"); oxml_w_end(w, "Override");
        oxml_w_end(w, "Types");
        unsigned long l; const char *cs = oxml_w_cstr(w, &l);
        opc_put(pkg, "[Content_Types].xml", (const unsigned char *)cs, l);
        oxml_w_free(w);
    }
    (void)ct_for_ext;

    unsigned long zl = 0;
    unsigned char *z = opc_save_mem(pkg, &zl);
    opc_free(pkg);
    if (!z) return -1;
    *out = z;
    *out_len = zl;
    return 0;
}
