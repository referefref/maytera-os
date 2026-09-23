// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// XLSX (SpreadsheetML) import/export. Format agent 6. See sheetmodel.h/formats.h.
//
// v1 scope: cells (number/string/bool/formula/error), shared strings (t="s")
// and inline strings (t="inlineStr"/"str"), the <f> formula element + cached
// <v>, a core number-format subset incl. a currency numFmt mapped into
// wb_style.numfmt_code, and sheet names. After load we run fx_recalc so
// CELL_FORMULA cells carry a computed result. Charts/pivots/conditional-format
// are out of v1 scope.
#include "formats.h"
#include "opc.h"
#include "oxml.h"
#include "formula.h"
#include "xlsx_ods_util.h"
#include <stdlib.h>
#include <string.h>

#define XLSX_MAIN_NS "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
#define PKG_REL_NS   "http://schemas.openxmlformats.org/package/2006/relationships"
#define CT_NS        "http://schemas.openxmlformats.org/package/2006/content-types"
#define REL_OFFICE   "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument"
#define REL_SHEET    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet"
#define REL_STYLES   "http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles"

// ------------------------------------------------------------------ load ----

// Append one shared string to workbook.sst (the model has no helper for this).
static void xlsx_sst_push(workbook *w, const char *s){
    char **ns = (char**)realloc(w->sst, (size_t)(w->nsst+1)*sizeof(char*));
    if(!ns) return;
    w->sst = ns;
    w->sst[w->nsst] = xo_dup(s ? s : "");
    w->nsst++;
}

static void xlsx_parse_sst(workbook *w, opc_pkg *p){
    opc_part *pt = opc_get(p, "xl/sharedStrings.xml");
    if(!pt || !pt->data) return;
    oxml *x = oxml_open((const char*)pt->data, pt->size);
    if(!x) return;
    sb cur; sb_init(&cur);
    int in_si=0, coll_t=0;
    oxml_evt e;
    while((e=oxml_next(x))!=OXML_EOF && e!=OXML_ERR){
        const char *ln = oxml_local(x);
        if(e==OXML_START){
            if(!strcmp(ln,"si")){ in_si=1; sb_reset(&cur); }
            else if(in_si && !strcmp(ln,"t")) coll_t=1;
        } else if(e==OXML_TEXT){
            if(coll_t) sb_puts(&cur, oxml_text(x));
        } else if(e==OXML_END){
            if(!strcmp(ln,"t")) coll_t=0;
            else if(!strcmp(ln,"si")){ in_si=0; xlsx_sst_push(w, sb_str(&cur)); }
        }
    }
    sb_free(&cur);
    oxml_close(x);
}

// A minimal builtin number-format subset. Returns a static format code or NULL
// (NULL => the "General"/default style, cell keeps numfmt_code unset).
static const char *xlsx_builtin_numfmt(int id){
    switch(id){
        case 1:  return "0";
        case 2:  return "0.00";
        case 3:  return "#,##0";
        case 4:  return "#,##0.00";
        case 9:  return "0%";
        case 10: return "0.00%";
        case 14: return "mm-dd-yy";
        case 5: case 6: case 7: case 8:
        case 42: case 44: return "\"$\"#,##0.00";  // currency builtins
        default: return 0;
    }
}

// Parse xl/styles.xml. Produces one wb_style per <cellXfs>/<xf>, in order, so a
// cell's s="N" indexes xf_style[N]. Returns the mapping (caller frees).
static void xlsx_parse_styles(workbook *w, opc_pkg *p, int **xf_style_out, int *nxf_out){
    *xf_style_out = 0; *nxf_out = 0;
    opc_part *pt = opc_get(p, "xl/styles.xml");
    if(!pt || !pt->data) return;
    oxml *x = oxml_open((const char*)pt->data, pt->size);
    if(!x) return;

    int *nf_id=0; char **nf_code=0; int n_nf=0;   // custom numFmts (id -> code)
    int *xf_fmt=0; int n_xf=0;                     // cellXfs numFmtId, in order
    int in_cellxfs=0;
    oxml_evt e;
    while((e=oxml_next(x))!=OXML_EOF && e!=OXML_ERR){
        const char *ln = oxml_local(x);
        if(e==OXML_START){
            if(!strcmp(ln,"numFmt")){
                const char *id = oxml_attr(x,"numFmtId");
                const char *code = oxml_attr(x,"formatCode");
                if(id){
                    int *ni=(int*)realloc(nf_id,(size_t)(n_nf+1)*sizeof(int));
                    char **nc=(char**)realloc(nf_code,(size_t)(n_nf+1)*sizeof(char*));
                    if(ni) nf_id=ni;
                    if(nc) nf_code=nc;
                    if(ni && nc){ nf_id[n_nf]=(int)strtol(id,0,10); nf_code[n_nf]=xo_dup(code?code:""); n_nf++; }
                }
            } else if(!strcmp(ln,"cellXfs")){
                in_cellxfs=1;
            } else if(!strcmp(ln,"xf") && in_cellxfs){
                const char *fid = oxml_attr(x,"numFmtId");
                int *nx=(int*)realloc(xf_fmt,(size_t)(n_xf+1)*sizeof(int));
                if(nx){ xf_fmt=nx; xf_fmt[n_xf]= fid?(int)strtol(fid,0,10):0; n_xf++; }
            }
        } else if(e==OXML_END){
            if(!strcmp(ln,"cellXfs")) in_cellxfs=0;
        }
    }
    oxml_close(x);

    int *xf_style = (n_xf>0) ? (int*)malloc((size_t)n_xf*sizeof(int)) : 0;
    for(int i=0;i<n_xf;i++){
        int fmtid = xf_fmt[i];
        const char *code = 0;
        for(int k=0;k<n_nf;k++) if(nf_id[k]==fmtid){ code = nf_code[k][0]?nf_code[k]:0; break; }
        if(!code) code = xlsx_builtin_numfmt(fmtid);
        wb_style st; memset(&st,0,sizeof(st));
        st.numfmt_code = (char*)code;   // wb_add_style copies it; NULL => default
        xf_style[i] = wb_add_style(w,&st);
    }
    for(int k=0;k<n_nf;k++) free(nf_code[k]);
    free(nf_code); free(nf_id); free(xf_fmt);
    *xf_style_out = xf_style; *nxf_out = n_xf;
}

typedef enum { KI_NUM=0, KI_SHARED, KI_INLINE, KI_STR, KI_BOOL, KI_ERR } cellkind;

static cellkind xlsx_kind(const char *t){
    if(!t) return KI_NUM;
    if(!strcmp(t,"s"))         return KI_SHARED;
    if(!strcmp(t,"inlineStr")) return KI_INLINE;
    if(!strcmp(t,"str"))       return KI_STR;
    if(!strcmp(t,"b"))         return KI_BOOL;
    if(!strcmp(t,"e"))         return KI_ERR;
    return KI_NUM;
}

// Parse one worksheet part into w->sheets[si] (w->sheets is stable here: all
// sheets were added before any worksheet is parsed).
static void xlsx_parse_sheet(workbook *w, int si, opc_pkg *p, const char *part,
                             int *xf_style, int nxf){
    opc_part *pt = opc_get(p, part);
    if(!pt || !pt->data) return;
    oxml *x = oxml_open((const char*)pt->data, pt->size);
    if(!x) return;

    sb fbuf, vbuf, tbuf; sb_init(&fbuf); sb_init(&vbuf); sb_init(&tbuf);
    enum { CO_NONE, CO_F, CO_V, CO_T } coll = CO_NONE;
    int cur_row=-1, next_col=0;
    int c_row=-1, c_col=-1, c_style=-1, c_has_f=0;
    cellkind kind=KI_NUM;
    oxml_evt e;
    while((e=oxml_next(x))!=OXML_EOF && e!=OXML_ERR){
        const char *ln = oxml_local(x);
        if(e==OXML_START){
            if(!strcmp(ln,"row")){
                const char *r = oxml_attr(x,"r");
                cur_row = r ? (int)strtol(r,0,10)-1 : cur_row+1;
                next_col = 0;
            } else if(!strcmp(ln,"c")){
                sb_reset(&fbuf); sb_reset(&vbuf); sb_reset(&tbuf);
                c_has_f=0; c_style=-1; coll=CO_NONE;
                const char *rref = oxml_attr(x,"r");
                if(rref && a1_to_rc(rref,&c_row,&c_col)==0){
                    next_col = c_col+1;
                } else {
                    c_row = (cur_row>=0)?cur_row:0; c_col = next_col; next_col++;
                }
                kind = xlsx_kind(oxml_attr(x,"t"));
                const char *sv = oxml_attr(x,"s");
                if(sv) c_style = (int)strtol(sv,0,10);
            } else if(!strcmp(ln,"f")){ coll=CO_F; c_has_f=1; }
            else if(!strcmp(ln,"v")){ coll=CO_V; }
            else if(!strcmp(ln,"t")){ coll=CO_T; }   // inside <is>
        } else if(e==OXML_TEXT){
            if(coll==CO_F) sb_puts(&fbuf, oxml_text(x));
            else if(coll==CO_V) sb_puts(&vbuf, oxml_text(x));
            else if(coll==CO_T) sb_puts(&tbuf, oxml_text(x));
        } else if(e==OXML_END){
            if(!strcmp(ln,"f")||!strcmp(ln,"v")||!strcmp(ln,"t")) coll=CO_NONE;
            else if(!strcmp(ln,"c")){
                sheet *s = &w->sheets[si];
                cell *cl = sheet_cell(s, c_row, c_col);
                if(cl){
                    cl->style_id = (c_style>=0 && c_style<nxf) ? xf_style[c_style] : -1;
                    if(c_has_f){
                        cl->type = CELL_FORMULA;
                        cl->formula = xo_dup(sb_str(&fbuf));
                        cl->num = vbuf.len ? strtod(sb_str(&vbuf),0) : 0.0;
                    } else switch(kind){
                        case KI_SHARED: {
                            int idx = vbuf.len ? (int)strtol(sb_str(&vbuf),0,10) : -1;
                            cl->type = CELL_STR;
                            cl->str = xo_dup((idx>=0 && idx<w->nsst) ? w->sst[idx] : "");
                        } break;
                        case KI_INLINE:
                            cl->type = CELL_STR; cl->str = xo_dup(sb_str(&tbuf)); break;
                        case KI_STR:
                            cl->type = CELL_STR; cl->str = xo_dup(sb_str(&vbuf)); break;
                        case KI_BOOL:
                            cl->type = CELL_BOOL;
                            cl->num = (vbuf.len && sb_str(&vbuf)[0]=='1') ? 1.0 : 0.0; break;
                        case KI_ERR:
                            cl->type = CELL_ERR; cl->str = xo_dup(sb_str(&vbuf)); break;
                        default:
                            cl->type = CELL_NUM;
                            cl->num = vbuf.len ? strtod(sb_str(&vbuf),0) : 0.0; break;
                    }
                }
            }
        }
    }
    sb_free(&fbuf); sb_free(&vbuf); sb_free(&tbuf);
    oxml_close(x);
}

int xlsx_load(const unsigned char *b, unsigned long n, workbook **out){
    if(out) *out=0;
    if(!b || !n) return -1;
    opc_pkg *p = opc_open_mem(b,n);
    if(!p) return -1;
    workbook *w = wb_new();
    if(!w){ opc_free(p); return -1; }

    xlsx_parse_sst(w, p);
    int *xf_style=0, nxf=0;
    xlsx_parse_styles(w, p, &xf_style, &nxf);

    // Collect sheet (name, rId) from xl/workbook.xml, then resolve targets.
    char **names=0, **rids=0; int ns=0;
    opc_part *wbp = opc_get(p, "xl/workbook.xml");
    if(wbp && wbp->data){
        oxml *x = oxml_open((const char*)wbp->data, wbp->size);
        if(x){
            oxml_evt e;
            while((e=oxml_next(x))!=OXML_EOF && e!=OXML_ERR){
                if(e==OXML_START && !strcmp(oxml_local(x),"sheet")){
                    const char *nm = oxml_attr(x,"name");
                    const char *rid = oxml_attr(x,"id");   // r:id (local "id")
                    char **nn=(char**)realloc(names,(size_t)(ns+1)*sizeof(char*));
                    char **nr=(char**)realloc(rids,(size_t)(ns+1)*sizeof(char*));
                    if(nn) names=nn;
                    if(nr) rids=nr;
                    if(nn && nr){ names[ns]=xo_dup(nm?nm:"Sheet"); rids[ns]=xo_dup(rid?rid:""); ns++; }
                }
            }
            oxml_close(x);
        }
    }

    // Add ALL sheets first so w->sheets does not move during cell parsing.
    for(int i=0;i<ns;i++) wb_add_sheet(w, names[i]);

    for(int i=0;i<ns;i++){
        char tgt[256]; char part[320];
        const char *rt = rids[i][0] ? opc_rel_target(p,"xl/_rels/workbook.xml.rels",rids[i],tgt,sizeof(tgt)) : 0;
        if(rt){
            if(rt[0]=='/') snprintf(part,sizeof(part),"%s", rt+1);
            else           snprintf(part,sizeof(part),"xl/%s", rt);
        } else {
            snprintf(part,sizeof(part),"xl/worksheets/sheet%d.xml", i+1);
        }
        xlsx_parse_sheet(w, i, p, part, xf_style, nxf);
    }

    for(int i=0;i<ns;i++){ free(names[i]); free(rids[i]); }
    free(names); free(rids); free(xf_style);
    opc_free(p);

    fx_recalc(w);
    *out = w;
    return 0;
}

// ------------------------------------------------------------------ save ----

static void xlsx_write_worksheet(oxml_w *wx, const workbook *w, int si){
    const sheet *s = &w->sheets[si];
    oxml_w_decl(wx);
    oxml_w_start(wx,"worksheet");
    oxml_w_attr(wx,"xmlns",XLSX_MAIN_NS);
    oxml_w_start(wx,"sheetData");
    for(int r=0;r<s->nrows;r++){
        // skip a fully-empty row (cells carry their own A1, so gaps are fine)
        int any=0;
        for(int c=0;c<s->ncols;c++) if(s->cells[(long)r*s->ncols+c].type!=CELL_EMPTY){ any=1; break; }
        if(!any) continue;
        char rn[16]; snprintf(rn,sizeof(rn),"%d",r+1);
        oxml_w_start(wx,"row"); oxml_w_attr(wx,"r",rn);
        for(int c=0;c<s->ncols;c++){
            const cell *cl = &s->cells[(long)r*s->ncols+c];
            if(cl->type==CELL_EMPTY) continue;
            oxml_w_start(wx,"c");
            char a1[16]; rc_to_a1(r,c,a1,sizeof(a1)); oxml_w_attr(wx,"r",a1);
            if(cl->style_id>=0){
                char sn[16]; snprintf(sn,sizeof(sn),"%d",cl->style_id+1); // xf0 = default
                oxml_w_attr(wx,"s",sn);
            }
            char num[48];
            switch(cl->type){
                case CELL_STR:
                    oxml_w_attr(wx,"t","inlineStr");
                    oxml_w_start(wx,"is"); oxml_w_start(wx,"t");
                    oxml_w_text(wx, cl->str?cl->str:"");
                    oxml_w_end(wx,"t"); oxml_w_end(wx,"is");
                    break;
                case CELL_FORMULA:
                    oxml_w_start(wx,"f"); oxml_w_text(wx, cl->formula?cl->formula:""); oxml_w_end(wx,"f");
                    xo_num_to_str(cl->num,num,sizeof(num));
                    oxml_w_start(wx,"v"); oxml_w_text(wx,num); oxml_w_end(wx,"v");
                    break;
                case CELL_BOOL:
                    oxml_w_attr(wx,"t","b");
                    oxml_w_start(wx,"v"); oxml_w_text(wx, cl->num!=0.0?"1":"0"); oxml_w_end(wx,"v");
                    break;
                case CELL_ERR:
                    oxml_w_attr(wx,"t","e");
                    oxml_w_start(wx,"v"); oxml_w_text(wx, cl->str?cl->str:"#ERR"); oxml_w_end(wx,"v");
                    break;
                default: /* CELL_NUM */
                    xo_num_to_str(cl->num,num,sizeof(num));
                    oxml_w_start(wx,"v"); oxml_w_text(wx,num); oxml_w_end(wx,"v");
                    break;
            }
            oxml_w_end(wx,"c");
        }
        oxml_w_end(wx,"row");
    }
    oxml_w_end(wx,"sheetData");
    oxml_w_end(wx,"worksheet");
}

// styles.xml: one xf per wb style (xf index = style_id+1; xf0 = default).
static void xlsx_write_styles(oxml_w *wx, const workbook *w){
    oxml_w_decl(wx);
    oxml_w_start(wx,"styleSheet"); oxml_w_attr(wx,"xmlns",XLSX_MAIN_NS);
    // custom numFmts for styles that carry a code
    int ncustom=0;
    for(int i=0;i<w->nstyle;i++) if(w->styles[i].numfmt_code) ncustom++;
    if(ncustom>0){
        char cnt[16]; snprintf(cnt,sizeof(cnt),"%d",ncustom);
        oxml_w_start(wx,"numFmts"); oxml_w_attr(wx,"count",cnt);
        for(int i=0;i<w->nstyle;i++){
            if(!w->styles[i].numfmt_code) continue;
            char id[16]; snprintf(id,sizeof(id),"%d",164+i);
            oxml_w_start(wx,"numFmt");
            oxml_w_attr(wx,"numFmtId",id);
            oxml_w_attr(wx,"formatCode",w->styles[i].numfmt_code);
            oxml_w_end(wx,"numFmt");
        }
        oxml_w_end(wx,"numFmts");
    }
    // cellXfs: default + one per style
    char xfcnt[16]; snprintf(xfcnt,sizeof(xfcnt),"%d",w->nstyle+1);
    oxml_w_start(wx,"cellXfs"); oxml_w_attr(wx,"count",xfcnt);
    oxml_w_start(wx,"xf"); oxml_w_attr(wx,"numFmtId","0"); oxml_w_end(wx,"xf");
    for(int i=0;i<w->nstyle;i++){
        oxml_w_start(wx,"xf");
        if(w->styles[i].numfmt_code){
            char id[16]; snprintf(id,sizeof(id),"%d",164+i);
            oxml_w_attr(wx,"numFmtId",id);
            oxml_w_attr(wx,"applyNumberFormat","1");
        } else {
            oxml_w_attr(wx,"numFmtId","0");
        }
        oxml_w_end(wx,"xf");
    }
    oxml_w_end(wx,"cellXfs");
    oxml_w_end(wx,"styleSheet");
}

static int xlsx_put_writer(opc_pkg *p, const char *part, oxml_w *wx){
    unsigned long len=0;
    const char *cs = oxml_w_cstr(wx,&len);
    int rc = opc_put(p, part, (const unsigned char*)cs, len);
    oxml_w_free(wx);
    return rc;
}

int xlsx_save(const workbook *w, unsigned char **out, unsigned long *out_len){
    if(out) *out=0;
    if(out_len) *out_len=0;
    if(!w) return -1;
    opc_pkg *p = opc_new();
    if(!p) return -1;

    // [Content_Types].xml
    {
        oxml_w *wx=oxml_w_new(); oxml_w_decl(wx);
        oxml_w_start(wx,"Types"); oxml_w_attr(wx,"xmlns",CT_NS);
        oxml_w_start(wx,"Default"); oxml_w_attr(wx,"Extension","rels");
        oxml_w_attr(wx,"ContentType","application/vnd.openxmlformats-package.relationships+xml"); oxml_w_end(wx,"Default");
        oxml_w_start(wx,"Default"); oxml_w_attr(wx,"Extension","xml");
        oxml_w_attr(wx,"ContentType","application/xml"); oxml_w_end(wx,"Default");
        oxml_w_start(wx,"Override"); oxml_w_attr(wx,"PartName","/xl/workbook.xml");
        oxml_w_attr(wx,"ContentType","application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"); oxml_w_end(wx,"Override");
        for(int i=0;i<w->nsheet;i++){
            char pn[64]; snprintf(pn,sizeof(pn),"/xl/worksheets/sheet%d.xml",i+1);
            oxml_w_start(wx,"Override"); oxml_w_attr(wx,"PartName",pn);
            oxml_w_attr(wx,"ContentType","application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"); oxml_w_end(wx,"Override");
        }
        oxml_w_start(wx,"Override"); oxml_w_attr(wx,"PartName","/xl/styles.xml");
        oxml_w_attr(wx,"ContentType","application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"); oxml_w_end(wx,"Override");
        oxml_w_end(wx,"Types");
        xlsx_put_writer(p,"[Content_Types].xml",wx);
    }
    // _rels/.rels
    {
        oxml_w *wx=oxml_w_new(); oxml_w_decl(wx);
        oxml_w_start(wx,"Relationships"); oxml_w_attr(wx,"xmlns",PKG_REL_NS);
        oxml_w_start(wx,"Relationship");
        oxml_w_attr(wx,"Id","rId1"); oxml_w_attr(wx,"Type",REL_OFFICE); oxml_w_attr(wx,"Target","xl/workbook.xml");
        oxml_w_end(wx,"Relationship");
        oxml_w_end(wx,"Relationships");
        xlsx_put_writer(p,"_rels/.rels",wx);
    }
    // xl/workbook.xml
    {
        oxml_w *wx=oxml_w_new(); oxml_w_decl(wx);
        oxml_w_start(wx,"workbook");
        oxml_w_attr(wx,"xmlns",XLSX_MAIN_NS);
        oxml_w_attr(wx,"xmlns:r","http://schemas.openxmlformats.org/officeDocument/2006/relationships");
        oxml_w_start(wx,"sheets");
        for(int i=0;i<w->nsheet;i++){
            char sid[16], rid[16];
            snprintf(sid,sizeof(sid),"%d",i+1);
            snprintf(rid,sizeof(rid),"rId%d",i+1);
            oxml_w_start(wx,"sheet");
            oxml_w_attr(wx,"name", w->sheets[i].name?w->sheets[i].name:"Sheet");
            oxml_w_attr(wx,"sheetId",sid);
            oxml_w_attr(wx,"r:id",rid);
            oxml_w_end(wx,"sheet");
        }
        oxml_w_end(wx,"sheets");
        oxml_w_end(wx,"workbook");
        xlsx_put_writer(p,"xl/workbook.xml",wx);
    }
    // xl/_rels/workbook.xml.rels
    {
        oxml_w *wx=oxml_w_new(); oxml_w_decl(wx);
        oxml_w_start(wx,"Relationships"); oxml_w_attr(wx,"xmlns",PKG_REL_NS);
        for(int i=0;i<w->nsheet;i++){
            char rid[16], tgt[64];
            snprintf(rid,sizeof(rid),"rId%d",i+1);
            snprintf(tgt,sizeof(tgt),"worksheets/sheet%d.xml",i+1);
            oxml_w_start(wx,"Relationship");
            oxml_w_attr(wx,"Id",rid); oxml_w_attr(wx,"Type",REL_SHEET); oxml_w_attr(wx,"Target",tgt);
            oxml_w_end(wx,"Relationship");
        }
        {
            char rid[16]; snprintf(rid,sizeof(rid),"rId%d",w->nsheet+1);
            oxml_w_start(wx,"Relationship");
            oxml_w_attr(wx,"Id",rid); oxml_w_attr(wx,"Type",REL_STYLES); oxml_w_attr(wx,"Target","styles.xml");
            oxml_w_end(wx,"Relationship");
        }
        oxml_w_end(wx,"Relationships");
        xlsx_put_writer(p,"xl/_rels/workbook.xml.rels",wx);
    }
    // xl/styles.xml
    {
        oxml_w *wx=oxml_w_new();
        xlsx_write_styles(wx,w);
        xlsx_put_writer(p,"xl/styles.xml",wx);
    }
    // xl/worksheets/sheetN.xml
    for(int i=0;i<w->nsheet;i++){
        oxml_w *wx=oxml_w_new();
        xlsx_write_worksheet(wx,w,i);
        char part[64]; snprintf(part,sizeof(part),"xl/worksheets/sheet%d.xml",i+1);
        xlsx_put_writer(p,part,wx);
    }

    unsigned long zl=0;
    unsigned char *z = opc_save_mem(p,&zl);
    opc_free(p);
    if(!z) return -1;
    *out = z; if(out_len) *out_len = zl;
    return 0;
}
