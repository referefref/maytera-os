// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// ODP (OpenDocument Presentation) import + export. Format agent 7 (officepptx).
// See docs/OFFICE_SUITE_ARCHITECTURE.md.
//
// v1 scope: draw:page -> slide; draw:frame with presentation:class="title" ->
// SHP_TITLE + slide.title; class="outline" -> a bulleted SHP_TEXTBOX; other
// text frames -> plain SHP_TEXTBOX; draw:image -> SHP_IMAGE. text:p carries
// paragraph text. Out of scope: animations, transitions, master/style fidelity
// beyond a minimal valid set, tables/charts.
#include "formats.h"
#include "opc.h"
#include "oxml.h"
#include <stdlib.h>
#include <string.h>

#define NS_OFFICE "urn:oasis:names:tc:opendocument:xmlns:office:1.0"
#define NS_DRAW   "urn:oasis:names:tc:opendocument:xmlns:drawing:1.0"
#define NS_TEXT   "urn:oasis:names:tc:opendocument:xmlns:text:1.0"
#define NS_PRES   "urn:oasis:names:tc:opendocument:xmlns:presentation:1.0"
#define NS_SVG    "urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0"
#define NS_XLINK  "http://www.w3.org/1999/xlink"
#define NS_STYLE  "urn:oasis:names:tc:opendocument:xmlns:style:1.0"
#define NS_FO     "urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0"
#define NS_MANIFEST "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0"

static char *xstrdup(const char *s) {
    if (!s) s = "";
    unsigned long n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
static char *str_append(char *dst, const char *add) {
    unsigned long dl = dst ? strlen(dst) : 0;
    unsigned long al = add ? strlen(add) : 0;
    char *n = (char *)realloc(dst, dl + al + 1);
    if (!n) return dst;
    memcpy(n + dl, add ? add : "", al);
    n[dl + al] = 0;
    return n;
}
static void u2s(unsigned int v, char *out) {
    char tmp[16]; int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v && i < 15) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    int j = 0; while (i > 0) out[j++] = tmp[--i]; out[j] = 0;
}
static int ns_is(const char *ns, const char *want) { return ns && strcmp(ns, want) == 0; }

typedef struct { char *text; int level; int bold, italic; } tpara;
typedef struct { tpara *p; int n, cap; } tparas;

// ---- ODF automatic text-style table (name -> bold/italic), read side --------
typedef struct { char name[80]; int bold, italic; } odpspan;
typedef struct { odpspan *v; int n, c; } odpspantab;
static void odpspan_free(odpspantab *t) { free(t->v); t->v = 0; t->n = t->c = 0; }
static void odpspan_add(odpspantab *t, const char *name, int bold, int italic) {
    if (!name || !name[0]) return;
    if (t->n == t->c) {
        int nc = t->c ? t->c * 2 : 8;
        odpspan *nv = (odpspan *)realloc(t->v, (size_t)nc * sizeof(odpspan));
        if (!nv) return;
        t->v = nv; t->c = nc;
    }
    odpspan *s = &t->v[t->n++];
    memset(s, 0, sizeof(*s));
    int i = 0; for (; name[i] && i < 79; i++) s->name[i] = name[i]; s->name[i] = 0;
    s->bold = bold; s->italic = italic;
}
static void odpspan_lookup(odpspantab *t, const char *name, int *bold, int *italic) {
    if (!name) return;
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->v[i].name, name) == 0) { *bold = t->v[i].bold; *italic = t->v[i].italic; return; }
}

static void tp_reset(tparas *t) { for (int i = 0; i < t->n; i++) free(t->p[i].text); t->n = 0; }
static void tp_free(tparas *t) { tp_reset(t); free(t->p); t->p = 0; t->cap = 0; }
static tpara *tp_new(tparas *t) {
    if (t->n == t->cap) {
        int nc = t->cap ? t->cap * 2 : 8;
        tpara *np = (tpara *)realloc(t->p, (size_t)nc * sizeof(tpara));
        if (!np) return 0;
        t->p = np; t->cap = nc;
    }
    tpara *r = &t->p[t->n++]; r->text = 0; r->level = 0; r->bold = 0; r->italic = 0; return r;
}

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
        doc_runfmt f; memset(&f, 0, sizeof(f)); f.color = 0xFFFFFFFF;
        f.bold = t->p[i].bold;
        f.italic = t->p[i].italic;
        doc_para_add_run(p, t->p[i].text ? t->p[i].text : "", f);
    }
}

int odp_load(const unsigned char *b, unsigned long n, presentation **out) {
    if (out) *out = 0;
    if (!b || !n || !out) return -1;
    opc_pkg *pkg = opc_open_mem(b, n);
    if (!pkg) return -1;
    presentation *pres = pres_new();
    if (!pres) { opc_free(pkg); return -1; }

    opc_part *cp = opc_get(pkg, "content.xml");
    if (cp && cp->data) {
        oxml *x = oxml_open((const char *)cp->data, cp->size);
        if (x) {
            slide *s = 0;
            tparas t; memset(&t, 0, sizeof(t));
            char cur_class[32] = {0};
            int in_frame = 0, in_textp = 0, is_image = 0;
            char href[256] = {0};
            // ODF automatic text styles: name -> bold/italic, resolved for spans.
            odpspantab spans; memset(&spans, 0, sizeof(spans));
            int in_auto = 0;
            char cur_style_name[80] = {0};
            int cur_style_bold = 0, cur_style_italic = 0;

            oxml_evt e;
            while ((e = oxml_next(x)) != OXML_EOF && e != OXML_ERR) {
                if (e == OXML_START) {
                    const char *ln = oxml_local(x);
                    const char *ns = oxml_ns(x);
                    if (ns_is(ns, NS_DRAW) && strcmp(ln, "page") == 0) {
                        s = pres_add_slide(pres);
                    } else if (ns_is(ns, NS_DRAW) && strcmp(ln, "frame") == 0) {
                        in_frame = 1; is_image = 0; href[0] = 0; tp_reset(&t);
                        const char *cl = oxml_attr_ns(x, NS_PRES, "class");
                        cur_class[0] = 0;
                        if (cl) { int i = 0; for (; cl[i] && i < 31; i++) cur_class[i] = cl[i]; cur_class[i] = 0; }
                    } else if (ns_is(ns, NS_DRAW) && strcmp(ln, "image") == 0) {
                        is_image = 1;
                        const char *h = oxml_attr_ns(x, NS_XLINK, "href");
                        if (h) { int i = 0; for (; h[i] && i < 255; i++) href[i] = h[i]; href[i] = 0; }
                    } else if (ns_is(ns, NS_TEXT) && strcmp(ln, "p") == 0) {
                        if (in_frame) { tp_new(&t); in_textp = 1; }
                    } else if (ns_is(ns, NS_OFFICE) && strcmp(ln, "automatic-styles") == 0) {
                        in_auto = 1;
                    } else if (in_auto && ns_is(ns, NS_STYLE) && strcmp(ln, "style") == 0) {
                        const char *nm = oxml_attr_ns(x, NS_STYLE, "name");
                        cur_style_name[0] = 0; cur_style_bold = 0; cur_style_italic = 0;
                        if (nm) { int i = 0; for (; nm[i] && i < 79; i++) cur_style_name[i] = nm[i]; cur_style_name[i] = 0; }
                    } else if (in_auto && ns_is(ns, NS_STYLE) && strcmp(ln, "text-properties") == 0) {
                        const char *fw = oxml_attr_ns(x, NS_FO, "font-weight");
                        const char *fs = oxml_attr_ns(x, NS_FO, "font-style");
                        if (fw && strcmp(fw, "bold") == 0) cur_style_bold = 1;
                        if (fs && strcmp(fs, "italic") == 0) cur_style_italic = 1;
                    } else if (ns_is(ns, NS_TEXT) && strcmp(ln, "span") == 0) {
                        // resolve the span's automatic style onto the current run.
                        if (in_frame && in_textp && t.n > 0) {
                            int sb = 0, si = 0;
                            odpspan_lookup(&spans, oxml_attr_ns(x, NS_TEXT, "style-name"), &sb, &si);
                            if (sb) t.p[t.n - 1].bold = 1;
                            if (si) t.p[t.n - 1].italic = 1;
                        }
                    }
                } else if (e == OXML_TEXT) {
                    if (in_frame && in_textp && t.n > 0)
                        t.p[t.n - 1].text = str_append(t.p[t.n - 1].text, oxml_text(x));
                } else if (e == OXML_END) {
                    const char *ln = oxml_local(x);
                    const char *ns = oxml_ns(x);
                    if (ns_is(ns, NS_TEXT) && strcmp(ln, "p") == 0) {
                        in_textp = 0;
                    } else if (ns_is(ns, NS_STYLE) && strcmp(ln, "style") == 0) {
                        if (in_auto) odpspan_add(&spans, cur_style_name, cur_style_bold, cur_style_italic);
                        cur_style_name[0] = 0;
                    } else if (ns_is(ns, NS_OFFICE) && strcmp(ln, "automatic-styles") == 0) {
                        in_auto = 0;
                    } else if (ns_is(ns, NS_DRAW) && strcmp(ln, "frame") == 0) {
                        if (s) {
                            if (is_image && href[0]) {
                                const char *pn = href;
                                if (pn[0] == '/') pn++;
                                opc_part *mp = opc_get(pkg, pn);
                                if (mp && mp->data && mp->size) {
                                    shape *sh = slide_add_shape(s, SHP_IMAGE);
                                    if (sh) {
                                        doc_image *im = (doc_image *)calloc(1, sizeof(doc_image));
                                        if (im) {
                                            im->bytes = (unsigned char *)malloc(mp->size);
                                            if (im->bytes) { memcpy(im->bytes, mp->data, mp->size); im->nbytes = mp->size; }
                                            int i = 0; for (; pn[i] && i < 63; i++) im->id[i] = pn[i]; im->id[i] = 0;
                                            sh->image = im;
                                        }
                                    }
                                }
                            } else {
                                int is_title = (strcmp(cur_class, "title") == 0);
                                int as_list = (strcmp(cur_class, "outline") == 0);
                                if (is_title && t.n > 0 && t.p[0].text && !s->title)
                                    s->title = xstrdup(t.p[0].text);
                                emit_text_shape(s, is_title, as_list, &t);
                            }
                        }
                        tp_reset(&t); in_frame = 0; is_image = 0; cur_class[0] = 0; href[0] = 0;
                    }
                }
            }
            oxml_close(x);
            tp_free(&t);
            odpspan_free(&spans);
        }
    }

    opc_free(pkg);
    *out = pres;
    return 0;
}

// ---- export ----------------------------------------------------------------

// Write-side registry of the distinct bold/italic run formats used, each mapped
// to an automatic text style name (T1, T2, ...) referenced by a text:span.
typedef struct { int bold, italic; char name[16]; } autospan;
typedef struct { autospan *v; int n, c; } autoreg;
static int rf_plain(const doc_runfmt *f) { return !f->bold && !f->italic; }
static const char *reg_span(autoreg *r, const doc_runfmt *f) {
    if (rf_plain(f)) return NULL;
    for (int i = 0; i < r->n; i++)
        if (r->v[i].bold == !!f->bold && r->v[i].italic == !!f->italic) return r->v[i].name;
    if (r->n == r->c) {
        int nc = r->c ? r->c * 2 : 8;
        autospan *nv = (autospan *)realloc(r->v, (size_t)nc * sizeof(autospan));
        if (!nv) return NULL;
        r->v = nv; r->c = nc;
    }
    autospan *s = &r->v[r->n];
    s->bold = !!f->bold; s->italic = !!f->italic;
    { char nb[12]; u2s((unsigned)(r->n + 1), nb); s->name[0] = 'T'; int k = 0; for (; nb[k]; k++) s->name[1 + k] = nb[k]; s->name[1 + k] = 0; }
    r->n++;
    return s->name;
}
static const char *img_ext(const doc_image *im) {
    if (im && im->nbytes >= 4 && im->bytes[0] == 0x89 && im->bytes[1] == 'P' &&
        im->bytes[2] == 'N' && im->bytes[3] == 'G') return "png";
    if (im && im->nbytes >= 3 && im->bytes[0] == 0xFF && im->bytes[1] == 0xD8 &&
        im->bytes[2] == 0xFF) return "jpg";
    return "png";
}
static const char *img_ct(const char *ext) {
    if (strcmp(ext, "jpg") == 0) return "image/jpeg";
    return "image/png";
}
static void put_str(opc_pkg *pkg, const char *name, const char *s) {
    opc_put(pkg, name, (const unsigned char *)s, (unsigned long)strlen(s));
}

static const char *STYLES_XML =
"<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
"<office:document-styles xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" office:version=\"1.2\"><office:styles/><office:automatic-styles><style:page-layout style:name=\"PM1\"><style:page-layout-properties fo:page-width=\"25.4cm\" fo:page-height=\"19.05cm\"/></style:page-layout></office:automatic-styles><office:master-styles><style:master-page style:name=\"Default\" style:page-layout-name=\"PM1\"/></office:master-styles></office:document-styles>";

// Manifest carries the images seen during save; built alongside content.
int odp_save(const presentation *p, unsigned char **out, unsigned long *out_len) {
    if (out) *out = 0;
    if (out_len) *out_len = 0;
    if (!p || !out || !out_len) return -1;
    opc_pkg *pkg = opc_new();
    if (!pkg) return -1;

    // mimetype: opc_save_mem_odf emits it FIRST and STORED (uncompressed), as
    // strict ODF requires so a reader can sniff the media type at a fixed offset.
    put_str(pkg, "mimetype", "application/vnd.oasis.opendocument.presentation");

    // content.xml + manifest image list
    oxml_w *mf = oxml_w_new();
    oxml_w_decl(mf);
    oxml_w_start(mf, "manifest:manifest");
    oxml_w_attr(mf, "xmlns:manifest", NS_MANIFEST);
    oxml_w_attr(mf, "manifest:version", "1.2");
    oxml_w_start(mf, "manifest:file-entry"); oxml_w_attr(mf, "manifest:full-path", "/"); oxml_w_attr(mf, "manifest:media-type", "application/vnd.oasis.opendocument.presentation"); oxml_w_end(mf, "manifest:file-entry");
    oxml_w_start(mf, "manifest:file-entry"); oxml_w_attr(mf, "manifest:full-path", "content.xml"); oxml_w_attr(mf, "manifest:media-type", "text/xml"); oxml_w_end(mf, "manifest:file-entry");
    oxml_w_start(mf, "manifest:file-entry"); oxml_w_attr(mf, "manifest:full-path", "styles.xml"); oxml_w_attr(mf, "manifest:media-type", "text/xml"); oxml_w_end(mf, "manifest:file-entry");

    // Register every distinct bold/italic run format used, so we can define an
    // automatic text style per format and reference it from a text:span.
    autoreg reg; memset(&reg, 0, sizeof(reg));
    for (int i = 0; i < p->nslide; i++) {
        const slide *s = &p->slides[i];
        for (int j = 0; j < s->nshape; j++) {
            const shape *sh = &s->shapes[j];
            if (sh->type == SHP_IMAGE) continue;
            for (int k = 0; k < sh->npara; k++)
                for (int ri = 0; ri < sh->paras[k].nrun; ri++)
                    reg_span(&reg, &sh->paras[k].runs[ri].fmt);
        }
    }

    oxml_w *w = oxml_w_new();
    oxml_w_decl(w);
    oxml_w_start(w, "office:document-content");
    oxml_w_attr(w, "xmlns:office", NS_OFFICE);
    oxml_w_attr(w, "xmlns:text", NS_TEXT);
    oxml_w_attr(w, "xmlns:draw", NS_DRAW);
    oxml_w_attr(w, "xmlns:presentation", NS_PRES);
    oxml_w_attr(w, "xmlns:svg", NS_SVG);
    oxml_w_attr(w, "xmlns:xlink", NS_XLINK);
    oxml_w_attr(w, "xmlns:style", NS_STYLE);
    oxml_w_attr(w, "xmlns:fo", NS_FO);
    oxml_w_attr(w, "office:version", "1.2");
      oxml_w_start(w, "office:automatic-styles");
        for (int i = 0; i < reg.n; i++) {
            oxml_w_start(w, "style:style");
            oxml_w_attr(w, "style:name", reg.v[i].name);
            oxml_w_attr(w, "style:family", "text");
            oxml_w_start(w, "style:text-properties");
            if (reg.v[i].bold)   oxml_w_attr(w, "fo:font-weight", "bold");
            if (reg.v[i].italic) oxml_w_attr(w, "fo:font-style", "italic");
            oxml_w_end(w, "style:text-properties");
            oxml_w_end(w, "style:style");
        }
      oxml_w_end(w, "office:automatic-styles");
      oxml_w_start(w, "office:body");
        oxml_w_start(w, "office:presentation");
        int img_seq = 0;
        for (int i = 0; i < p->nslide; i++) {
            const slide *s = &p->slides[i];
            char pname[16]; { pname[0]='p';pname[1]='a';pname[2]='g';pname[3]='e'; char nb[12]; u2s((unsigned)(i+1), nb); int k=0; for(;nb[k];k++) pname[4+k]=nb[k]; pname[4+k]=0; }
            oxml_w_start(w, "draw:page");
            oxml_w_attr(w, "draw:name", pname);
            oxml_w_attr(w, "draw:master-page-name", "Default");
            for (int j = 0; j < s->nshape; j++) {
                const shape *sh = &s->shapes[j];
                if (sh->type == SHP_IMAGE) {
                    if (!sh->image) continue;
                    img_seq++;
                    const char *ext = img_ext(sh->image);
                    char num[12]; u2s((unsigned)img_seq, num);
                    char full[64]; { unsigned long o=0; const char*pre="Pictures/image"; for(int k=0;pre[k];k++) full[o++]=pre[k]; for(int k=0;num[k];k++) full[o++]=num[k]; full[o++]='.'; for(int k=0;ext[k];k++) full[o++]=ext[k]; full[o]=0; }
                    opc_put(pkg, full, sh->image->bytes, sh->image->nbytes);
                    oxml_w_start(mf, "manifest:file-entry"); oxml_w_attr(mf, "manifest:full-path", full); oxml_w_attr(mf, "manifest:media-type", img_ct(ext)); oxml_w_end(mf, "manifest:file-entry");
                    oxml_w_start(w, "draw:frame");
                    oxml_w_attr(w, "svg:x", "1cm"); oxml_w_attr(w, "svg:y", "1cm");
                    oxml_w_attr(w, "svg:width", "20cm"); oxml_w_attr(w, "svg:height", "12cm");
                      oxml_w_start(w, "draw:image");
                      oxml_w_attr(w, "xlink:href", full);
                      oxml_w_attr(w, "xlink:type", "simple");
                      oxml_w_attr(w, "xlink:show", "embed");
                      oxml_w_attr(w, "xlink:actuate", "onLoad");
                      oxml_w_end(w, "draw:image");
                    oxml_w_end(w, "draw:frame");
                    continue;
                }
                int is_title = (sh->type == SHP_TITLE);
                int as_list = 0;
                for (int k = 0; k < sh->npara; k++) if (sh->paras[k].list_level >= 0) { as_list = 1; break; }
                const char *cls = is_title ? "title" : (as_list ? "outline" : "subtitle");
                oxml_w_start(w, "draw:frame");
                oxml_w_attr(w, "presentation:class", cls);
                oxml_w_attr(w, "svg:x", "1cm"); oxml_w_attr(w, "svg:y", "1cm");
                oxml_w_attr(w, "svg:width", "20cm"); oxml_w_attr(w, "svg:height", "3cm");
                  oxml_w_start(w, "draw:text-box");
                  for (int k = 0; k < sh->npara; k++) {
                      const doc_para *pp = &sh->paras[k];
                      oxml_w_start(w, "text:p");
                      for (int ri = 0; ri < pp->nrun; ri++) {
                          const doc_run *rn = &pp->runs[ri];
                          const char *sn = reg_span(&reg, &rn->fmt);  // registered above
                          if (sn) {
                              oxml_w_start(w, "text:span");
                              oxml_w_attr(w, "text:style-name", sn);
                              oxml_w_text(w, rn->text ? rn->text : "");
                              oxml_w_end(w, "text:span");
                          } else {
                              oxml_w_text(w, rn->text ? rn->text : "");
                          }
                      }
                      oxml_w_end(w, "text:p");
                  }
                  oxml_w_end(w, "draw:text-box");
                oxml_w_end(w, "draw:frame");
            }
            oxml_w_end(w, "draw:page");
        }
        oxml_w_end(w, "office:presentation");
      oxml_w_end(w, "office:body");
    oxml_w_end(w, "office:document-content");

    {
        unsigned long l; const char *cs = oxml_w_cstr(w, &l);
        opc_put(pkg, "content.xml", (const unsigned char *)cs, l);
    }
    put_str(pkg, "styles.xml", STYLES_XML);
    oxml_w_end(mf, "manifest:manifest");
    {
        unsigned long l; const char *cs = oxml_w_cstr(mf, &l);
        opc_put(pkg, "META-INF/manifest.xml", (const unsigned char *)cs, l);
    }
    oxml_w_free(w);
    oxml_w_free(mf);
    free(reg.v);

    unsigned long zl = 0;
    unsigned char *z = opc_save_mem_odf(pkg, &zl);   // mimetype first + stored
    opc_free(pkg);
    if (!z) return -1;
    *out = z;
    *out_len = zl;
    return 0;
}
