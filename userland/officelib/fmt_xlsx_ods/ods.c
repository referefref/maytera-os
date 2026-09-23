// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// ODS (OpenDocument Spreadsheet) import/export. Format agent 6.
//
// v1 scope: table cells with office:value-type float/currency/percentage/
// string/boolean, table:formula (strip the "of:="/"=" prefix + ODS "[.A1]"
// bracket refs), text:p cell text, repeated-column/row expansion, sheet names,
// and a currency number format mapped into wb_style.numfmt_code. After load we
// run fx_recalc so CELL_FORMULA cells carry a computed result.
#include "formats.h"
#include "opc.h"
#include "oxml.h"
#include "formula.h"
#include "xlsx_ods_util.h"
#include <stdlib.h>
#include <string.h>

#define ODS_MIMETYPE "application/vnd.oasis.opendocument.spreadsheet"
#define NS_OFFICE   "urn:oasis:names:tc:opendocument:xmlns:office:1.0"
#define NS_TABLE    "urn:oasis:names:tc:opendocument:xmlns:table:1.0"
#define NS_TEXT     "urn:oasis:names:tc:opendocument:xmlns:text:1.0"
#define NS_STYLE    "urn:oasis:names:tc:opendocument:xmlns:style:1.0"
#define NS_NUMBER   "urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0"
#define NS_MANIFEST "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0"
#define ODS_CURRENCY_CODE "\"$\"#,##0.00"
#define REP_CAP 4096   // guard against gigantic repeated-empty runs

// ------------------------------------------------------------------ load ----

// Translate an ODS formula ("of:=SUM([.B2:.B3])") to the fx dialect
// ("SUM(B2:B3)"): drop the of:=/= prefix, and strip '[' ']' and the sheet-local
// '.' that appears inside a bracketed reference. Returns an owned string.
static char *ods_translate_formula(const char *f){
    if(!f) return xo_dup("");
    const char *s=f;
    if(!strncmp(s,"of:=",4)) s+=4;
    else if(!strncmp(s,"of:",3)) s+=3;
    else if(s[0]=='=') s+=1;
    sb b; sb_init(&b);
    int inbr=0;
    for(; *s; s++){
        char ch=*s;
        if(ch=='['){ inbr=1; continue; }
        if(ch==']'){ inbr=0; continue; }
        if(inbr && ch=='.') continue;   // sheet-local separator inside a ref
        sb_putc(&b,ch);
    }
    char *r = xo_dup(sb_str(&b));
    sb_free(&b);
    return r;
}

// name -> wb style_id map for automatic cell styles that carry a numfmt code.
typedef struct { char **names; int *ids; int n; } stylemap;

static int stylemap_lookup(stylemap *m, const char *name){
    if(!name) return -1;
    for(int i=0;i<m->n;i++) if(!strcmp(m->names[i],name)) return m->ids[i];
    return -1;
}
static void stylemap_add(stylemap *m, const char *name, int id){
    char **nn=(char**)realloc(m->names,(size_t)(m->n+1)*sizeof(char*));
    int   *ni=(int*) realloc(m->ids,  (size_t)(m->n+1)*sizeof(int));
    if(nn) m->names=nn;
    if(ni) m->ids=ni;
    if(nn && ni){ m->names[m->n]=xo_dup(name); m->ids[m->n]=id; m->n++; }
}

// A tiny set of currency-style names seen in <office:automatic-styles>.
typedef struct { char **names; int n; } nameset;
static int nameset_has(nameset *s, const char *name){
    if(!name) return 0;
    for(int i=0;i<s->n;i++) if(!strcmp(s->names[i],name)) return 1;
    return 0;
}
static void nameset_add(nameset *s, const char *name){
    if(!name) return;
    char **nn=(char**)realloc(s->names,(size_t)(s->n+1)*sizeof(char*));
    if(nn){ s->names=nn; s->names[s->n]=xo_dup(name); s->n++; }
}

static void cpystr(char *dst, unsigned long cap, const char *src){
    if(!cap) return;
    if(!src){ dst[0]=0; return; }
    snprintf(dst,(size_t)cap,"%s",src);
}

int ods_load(const unsigned char *b, unsigned long n, workbook **out){
    if(out) *out=0;
    if(!b || !n) return -1;
    opc_pkg *p = opc_open_mem(b,n);
    if(!p) return -1;
    opc_part *ct = opc_get(p,"content.xml");
    if(!ct || !ct->data){ opc_free(p); return -1; }
    workbook *w = wb_new();
    if(!w){ opc_free(p); return -1; }

    oxml *x = oxml_open((const char*)ct->data, ct->size);
    if(!x){ wb_free(w); opc_free(p); return -1; }

    stylemap smap; smap.names=0; smap.ids=0; smap.n=0;
    nameset  cset; cset.names=0; cset.n=0;
    int fallback_currency_style = -1;   // created lazily

    int in_auto=0;
    int si=-1;                 // current sheet index (-1 = none yet)
    int R=0;                   // current row within sheet
    int C=0;                   // current column within row

    // per-cell captured attributes (copied out of transient parser scratch)
    int in_cell=0, coll_text=0;
    char c_vtype[32], c_value[64], c_bool[16], c_style[64], c_formula[512];
    int  c_colrep=1;
    sb   c_text; sb_init(&c_text);
    int  row_rep=1;

    oxml_evt e;
    while((e=oxml_next(x))!=OXML_EOF && e!=OXML_ERR){
        const char *ln = oxml_local(x);
        if(e==OXML_START){
            if(!strcmp(ln,"automatic-styles")){ in_auto=1; }
            else if(in_auto && !strcmp(ln,"currency-style")){
                nameset_add(&cset, oxml_attr(x,"name"));
            }
            else if(in_auto && !strcmp(ln,"style")){
                const char *nm  = oxml_attr(x,"name");
                const char *dsn = oxml_attr(x,"data-style-name");
                if(nm && dsn && nameset_has(&cset,dsn)){
                    wb_style st; memset(&st,0,sizeof(st));
                    st.numfmt_code = (char*)ODS_CURRENCY_CODE;
                    int id = wb_add_style(w,&st);
                    stylemap_add(&smap, nm, id);
                }
            }
            else if(!strcmp(ln,"table") && oxml_attr(x,"name") && !in_auto){
                // A sheet. Adding a sheet may move w->sheets, but we only ever
                // touch the current sheet by index (re-fetched at use), never a
                // cached sheet* across another wb_add_sheet.
                wb_add_sheet(w, oxml_attr(x,"name"));
                si = w->nsheet-1; R=0;
            }
            else if(!strcmp(ln,"table-row") && si>=0){
                const char *rr = oxml_attr(x,"number-rows-repeated");
                row_rep = rr ? (int)strtol(rr,0,10) : 1;
                if(row_rep<1) row_rep=1;
                C=0;
            }
            else if((!strcmp(ln,"table-cell")||!strcmp(ln,"covered-table-cell")) && si>=0){
                in_cell=1; coll_text=0; sb_reset(&c_text);
                cpystr(c_vtype,sizeof(c_vtype), oxml_attr(x,"value-type"));
                cpystr(c_value,sizeof(c_value), oxml_attr(x,"value"));
                cpystr(c_bool, sizeof(c_bool),  oxml_attr(x,"boolean-value"));
                cpystr(c_style,sizeof(c_style), oxml_attr(x,"style-name"));
                cpystr(c_formula,sizeof(c_formula), oxml_attr(x,"formula"));
                const char *cr = oxml_attr(x,"number-columns-repeated");
                c_colrep = cr ? (int)strtol(cr,0,10) : 1;
                if(c_colrep<1) c_colrep=1;
            }
            else if(in_cell && !strcmp(ln,"p")){ coll_text=1; }
        }
        else if(e==OXML_TEXT){
            if(coll_text) sb_puts(&c_text, oxml_text(x));
        }
        else if(e==OXML_END){
            if(!strcmp(ln,"automatic-styles")) in_auto=0;
            else if(in_cell && !strcmp(ln,"p")) coll_text=0;
            else if((!strcmp(ln,"table-cell")||!strcmp(ln,"covered-table-cell")) && in_cell){
                in_cell=0;
                int has_formula = c_formula[0]!=0;
                int has_value   = c_value[0]!=0;
                int has_text    = c_text.len>0;
                int is_currency = !strcmp(c_vtype,"currency");
                int empty = !has_formula && !has_value && !has_text && c_bool[0]==0
                            && c_vtype[0]==0;
                if(!empty){
                    // resolve style id (cell style, else fallback for currency)
                    int style_id = stylemap_lookup(&smap, c_style[0]?c_style:0);
                    if(style_id<0 && is_currency){
                        if(fallback_currency_style<0){
                            wb_style st; memset(&st,0,sizeof(st));
                            st.numfmt_code=(char*)ODS_CURRENCY_CODE;
                            fallback_currency_style = wb_add_style(w,&st);
                        }
                        style_id = fallback_currency_style;
                    }
                    int reps = c_colrep>REP_CAP ? REP_CAP : c_colrep;
                    for(int rp=0; rp<reps; rp++){
                        sheet *s = &w->sheets[si];
                        cell  *cl = sheet_cell(s, R, C+rp);
                        if(!cl) break;
                        cl->style_id = style_id;
                        if(has_formula){
                            cl->type = CELL_FORMULA;
                            cl->formula = ods_translate_formula(c_formula);
                            cl->num = has_value ? strtod(c_value,0) : 0.0;
                        } else if(!strcmp(c_vtype,"float")||is_currency||!strcmp(c_vtype,"percentage")){
                            cl->type = CELL_NUM;
                            cl->num = has_value ? strtod(c_value,0)
                                                : (has_text? strtod(sb_str(&c_text),0):0.0);
                        } else if(!strcmp(c_vtype,"boolean")){
                            cl->type = CELL_BOOL;
                            cl->num = (!strcmp(c_bool,"true")||c_bool[0]=='1') ? 1.0 : 0.0;
                        } else {
                            cl->type = CELL_STR;
                            cl->str = xo_dup(sb_str(&c_text));
                        }
                    }
                }
                C += c_colrep;   // advance past the (possibly empty) repeated span
            }
            else if(!strcmp(ln,"table-row") && si>=0){
                R += row_rep;    // repeated empty rows just advance the row cursor
                row_rep=1;
            }
        }
    }
    sb_free(&c_text);
    oxml_close(x);
    for(int i=0;i<smap.n;i++) free(smap.names[i]);
    free(smap.names); free(smap.ids);
    for(int i=0;i<cset.n;i++) free(cset.names[i]);
    free(cset.names);
    opc_free(p);

    fx_recalc(w);
    *out = w;
    return 0;
}

// ------------------------------------------------------------------ save ----

static void ods_write_content(oxml_w *wx, const workbook *w){
    oxml_w_decl(wx);
    oxml_w_start(wx,"office:document-content");
    oxml_w_attr(wx,"xmlns:office",NS_OFFICE);
    oxml_w_attr(wx,"xmlns:table", NS_TABLE);
    oxml_w_attr(wx,"xmlns:text",  NS_TEXT);
    oxml_w_attr(wx,"xmlns:style", NS_STYLE);
    oxml_w_attr(wx,"xmlns:number",NS_NUMBER);
    oxml_w_attr(wx,"office:version","1.2");

    // automatic-styles: one shared currency data style + a table-cell style per
    // wb style that carries a numfmt_code (named ceN, N = style_id).
    int ncoded=0;
    for(int i=0;i<w->nstyle;i++) if(w->styles[i].numfmt_code) ncoded++;
    oxml_w_start(wx,"office:automatic-styles");
    if(ncoded>0){
        oxml_w_start(wx,"number:currency-style"); oxml_w_attr(wx,"style:name","N-CUR");
        oxml_w_start(wx,"number:currency-symbol"); oxml_w_text(wx,"$"); oxml_w_end(wx,"number:currency-symbol");
        oxml_w_start(wx,"number:number");
        oxml_w_attr(wx,"number:decimal-places","2"); oxml_w_attr(wx,"number:min-integer-digits","1");
        oxml_w_end(wx,"number:number");
        oxml_w_end(wx,"number:currency-style");
        for(int i=0;i<w->nstyle;i++){
            if(!w->styles[i].numfmt_code) continue;
            char nm[16]; snprintf(nm,sizeof(nm),"ce%d",i);
            oxml_w_start(wx,"style:style");
            oxml_w_attr(wx,"style:name",nm);
            oxml_w_attr(wx,"style:family","table-cell");
            oxml_w_attr(wx,"style:data-style-name","N-CUR");
            oxml_w_end(wx,"style:style");
        }
    }
    oxml_w_end(wx,"office:automatic-styles");

    oxml_w_start(wx,"office:body");
    oxml_w_start(wx,"office:spreadsheet");
    for(int si=0; si<w->nsheet; si++){
        const sheet *s = &w->sheets[si];
        oxml_w_start(wx,"table:table");
        oxml_w_attr(wx,"table:name", s->name?s->name:"Sheet");
        {
            char nc[16]; snprintf(nc,sizeof(nc),"%d", s->ncols>0?s->ncols:1);
            oxml_w_start(wx,"table:table-column");
            oxml_w_attr(wx,"table:number-columns-repeated",nc);
            oxml_w_end(wx,"table:table-column");
        }
        // Dense emission (all rows/cols) keeps cell positions exact on reload.
        for(int r=0;r<s->nrows;r++){
            oxml_w_start(wx,"table:table-row");
            for(int c=0;c<s->ncols;c++){
                const cell *cl = &s->cells[(long)r*s->ncols+c];
                oxml_w_start(wx,"table:table-cell");
                if(cl->type!=CELL_EMPTY && cl->style_id>=0 && cl->style_id<w->nstyle
                   && w->styles[cl->style_id].numfmt_code){
                    char nm[16]; snprintf(nm,sizeof(nm),"ce%d",cl->style_id);
                    oxml_w_attr(wx,"table:style-name",nm);
                }
                char num[48];
                switch(cl->type){
                    case CELL_STR:
                        oxml_w_attr(wx,"office:value-type","string");
                        oxml_w_start(wx,"text:p"); oxml_w_text(wx, cl->str?cl->str:""); oxml_w_end(wx,"text:p");
                        break;
                    case CELL_FORMULA: {
                        // fx-dialect source (no ODS bracket rewrite in v1); our
                        // own loader strips the of:= prefix and recomputes.
                        // TODO(officexlsx): emit [.A1] bracketed refs for full
                        // ODF interop.
                        char fbuf[544]; snprintf(fbuf,sizeof(fbuf),"of:=%s", cl->formula?cl->formula:"");
                        oxml_w_attr(wx,"table:formula",fbuf);
                        oxml_w_attr(wx,"office:value-type","float");
                        xo_num_to_str(cl->num,num,sizeof(num));
                        oxml_w_attr(wx,"office:value",num);
                        oxml_w_start(wx,"text:p"); oxml_w_text(wx,num); oxml_w_end(wx,"text:p");
                    } break;
                    case CELL_BOOL:
                        oxml_w_attr(wx,"office:value-type","boolean");
                        oxml_w_attr(wx,"office:boolean-value", cl->num!=0.0?"true":"false");
                        oxml_w_start(wx,"text:p"); oxml_w_text(wx, cl->num!=0.0?"TRUE":"FALSE"); oxml_w_end(wx,"text:p");
                        break;
                    case CELL_ERR:
                        oxml_w_attr(wx,"office:value-type","string");
                        oxml_w_start(wx,"text:p"); oxml_w_text(wx, cl->str?cl->str:"#ERR"); oxml_w_end(wx,"text:p");
                        break;
                    case CELL_NUM:
                        oxml_w_attr(wx,"office:value-type","float");
                        xo_num_to_str(cl->num,num,sizeof(num));
                        oxml_w_attr(wx,"office:value",num);
                        oxml_w_start(wx,"text:p"); oxml_w_text(wx,num); oxml_w_end(wx,"text:p");
                        break;
                    default: /* CELL_EMPTY */ break;
                }
                oxml_w_end(wx,"table:table-cell");
            }
            oxml_w_end(wx,"table:table-row");
        }
        oxml_w_end(wx,"table:table");
    }
    oxml_w_end(wx,"office:spreadsheet");
    oxml_w_end(wx,"office:body");
    oxml_w_end(wx,"office:document-content");
}

int ods_save(const workbook *w, unsigned char **out, unsigned long *out_len){
    if(out) *out=0;
    if(out_len) *out_len=0;
    if(!w) return -1;
    opc_pkg *p = opc_new();
    if(!p) return -1;

    // mimetype: opc_save_mem_odf emits it FIRST and STORED, as strict ODF wants.
    opc_put(p,"mimetype",(const unsigned char*)ODS_MIMETYPE,(unsigned long)strlen(ODS_MIMETYPE));

    // content.xml
    {
        oxml_w *wx=oxml_w_new();
        ods_write_content(wx,w);
        unsigned long len=0; const char *cs=oxml_w_cstr(wx,&len);
        opc_put(p,"content.xml",(const unsigned char*)cs,len);
        oxml_w_free(wx);
    }
    // styles.xml (minimal but valid)
    {
        oxml_w *wx=oxml_w_new(); oxml_w_decl(wx);
        oxml_w_start(wx,"office:document-styles");
        oxml_w_attr(wx,"xmlns:office",NS_OFFICE);
        oxml_w_attr(wx,"xmlns:style", NS_STYLE);
        oxml_w_attr(wx,"office:version","1.2");
        oxml_w_start(wx,"office:styles"); oxml_w_end(wx,"office:styles");
        oxml_w_end(wx,"office:document-styles");
        unsigned long len=0; const char *cs=oxml_w_cstr(wx,&len);
        opc_put(p,"styles.xml",(const unsigned char*)cs,len);
        oxml_w_free(wx);
    }
    // META-INF/manifest.xml
    {
        oxml_w *wx=oxml_w_new(); oxml_w_decl(wx);
        oxml_w_start(wx,"manifest:manifest");
        oxml_w_attr(wx,"xmlns:manifest",NS_MANIFEST);
        oxml_w_attr(wx,"manifest:version","1.2");
        oxml_w_start(wx,"manifest:file-entry");
        oxml_w_attr(wx,"manifest:full-path","/");
        oxml_w_attr(wx,"manifest:media-type",ODS_MIMETYPE);
        oxml_w_end(wx,"manifest:file-entry");
        oxml_w_start(wx,"manifest:file-entry");
        oxml_w_attr(wx,"manifest:full-path","content.xml");
        oxml_w_attr(wx,"manifest:media-type","text/xml");
        oxml_w_end(wx,"manifest:file-entry");
        oxml_w_start(wx,"manifest:file-entry");
        oxml_w_attr(wx,"manifest:full-path","styles.xml");
        oxml_w_attr(wx,"manifest:media-type","text/xml");
        oxml_w_end(wx,"manifest:file-entry");
        oxml_w_end(wx,"manifest:manifest");
        unsigned long len=0; const char *cs=oxml_w_cstr(wx,&len);
        opc_put(p,"META-INF/manifest.xml",(const unsigned char*)cs,len);
        oxml_w_free(wx);
    }

    unsigned long zl=0;
    unsigned char *z = opc_save_mem_odf(p,&zl);   // mimetype first + stored
    opc_free(p);
    if(!z) return -1;
    *out = z; if(out_len) *out_len = zl;
    return 0;
}
