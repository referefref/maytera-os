#include "formula.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

// Spreadsheet formula evaluator. Owner: agent 3 (officemodel). See formula.h.
// v1 core subset: comparisons (= <> < > <= >=), + - * / ^, postfix %,
// parens, A1/$A$1 cell refs, ranges A1:B3, and the functions
// SUM/AVERAGE/MIN/MAX/COUNT/IF/ABS/ROUND. Single-pass recalc, no cycle solver.
//
// v1 semantics notes (documented, matched to the QA corpus oracle):
//  - '%' is the spreadsheet POSTFIX percent operator (50% -> 0.5), not modulo.
//    (Excel spells modulo MOD(); it is not in the v1 subset.)
//  - COUNT counts NON-EMPTY cells in its arguments (COUNTA-style), so
//    COUNT(A2:B3) over two text + two number cells is 4, per the corpus.
//  - Text/empty cells contribute 0 to arithmetic and are ignored by the
//    numeric aggregates SUM/AVERAGE/MIN/MAX.

typedef struct {
    const char *s;
    workbook   *wb;
    sheet      *sh;
    int         row, col;
    fx_status   err;
} pctx;

static double parse_compare(pctx *c);   // top-level expression

static void skipws(pctx *c){ while(*c->s==' '||*c->s=='\t') c->s++; }

// Read-only cell fetch: NULL if out of the sheet's current extent (empty).
static cell *cell_peek(sheet *s, int row, int col){
    if(!s || row<0 || col<0 || row>=s->nrows || col>=s->ncols) return 0;
    return &s->cells[(long)row*s->ncols+col];
}

static double cell_num(cell *cl){
    if(!cl) return 0.0;
    switch(cl->type){
        case CELL_NUM: case CELL_BOOL: case CELL_FORMULA: return cl->num;
        default: return 0.0;   // CELL_STR / CELL_ERR / CELL_EMPTY -> 0 in arithmetic
    }
}

// Try to parse a cell reference at c->s. Advances c->s and returns 0 on
// success; on failure restores c->s and returns -1 (does NOT set c->err).
static int try_ref(pctx *c, int *row, int *col){
    const char *start=c->s;
    if(*c->s=='$') c->s++;
    if(!isalpha((unsigned char)*c->s)){ c->s=start; return -1; }
    while(isalpha((unsigned char)*c->s)) c->s++;
    if(*c->s=='$') c->s++;
    if(!isdigit((unsigned char)*c->s)){ c->s=start; return -1; }
    while(isdigit((unsigned char)*c->s)) c->s++;
    char buf[40]; unsigned long len=(unsigned long)(c->s-start);
    if(len>=sizeof(buf)){ c->s=start; return -1; }
    memcpy(buf,start,len); buf[len]=0;
    if(a1_to_rc(buf,row,col)!=0){ c->s=start; return -1; }
    return 0;
}

// ---- aggregation for SUM/AVERAGE/MIN/MAX/COUNT ----
typedef struct { double sum, mn, mx; long nnum; long ncount; int have; } agg;

static void agg_add_num(agg *a, double v){
    a->sum+=v; a->nnum++; a->ncount++;
    if(!a->have){ a->mn=a->mx=v; a->have=1; }
    else { if(v<a->mn)a->mn=v; if(v>a->mx)a->mx=v; }
}
static void agg_add_cell(agg *a, cell *cl){
    if(!cl || cl->type==CELL_EMPTY) return;
    if(cl->type==CELL_STR || cl->type==CELL_ERR){ a->ncount++; return; } // non-empty, non-number
    agg_add_num(a, cell_num(cl));
}
static void agg_add_range(pctx *c, agg *a, int r0,int c0,int r1,int c1){
    if(r0>r1){ int t=r0;r0=r1;r1=t; }
    if(c0>c1){ int t=c0;c0=c1;c1=t; }
    for(int r=r0;r<=r1;r++)
        for(int col=c0;col<=c1;col++)
            agg_add_cell(a, cell_peek(c->sh,r,col));
}

// Parse "(" arglist ")" for an aggregate; '(' already consumed.
static void parse_agg_args(pctx *c, agg *a){
    skipws(c);
    if(*c->s==')'){ c->s++; return; }
    for(;;){
        skipws(c);
        const char *save=c->s;
        int r0,c0,r1,c1;
        if(try_ref(c,&r0,&c0)==0){
            skipws(c);
            if(*c->s==':'){
                c->s++; skipws(c);
                if(try_ref(c,&r1,&c1)!=0){ c->err=FX_ERR_REF; return; }
                agg_add_range(c,a,r0,c0,r1,c1);
            } else {
                c->s=save;                 // single ref may be part of an expr
                double v=parse_compare(c);
                if(c->err) return;
                agg_add_num(a,v);
            }
        } else {
            c->s=save;
            double v=parse_compare(c);
            if(c->err) return;
            agg_add_num(a,v);
        }
        skipws(c);
        if(*c->s==','){ c->s++; continue; }
        if(*c->s==')'){ c->s++; break; }
        c->err=FX_ERR_SYNTAX; return;
    }
}

// Parse a single scalar argument (expression). Sets err on failure.
static double parse_scalar_arg(pctx *c){
    skipws(c);
    double v=parse_compare(c);
    return v;
}

static double call_function(pctx *c, const char *name){
    // '(' already consumed by caller.
    if(!strcasecmp(name,"SUM")||!strcasecmp(name,"AVERAGE")||
       !strcasecmp(name,"MIN")||!strcasecmp(name,"MAX")||!strcasecmp(name,"COUNT")){
        agg a; memset(&a,0,sizeof(a));
        parse_agg_args(c,&a);
        if(c->err) return 0;
        if(!strcasecmp(name,"SUM"))     return a.sum;
        if(!strcasecmp(name,"COUNT"))   return (double)a.ncount;
        if(!strcasecmp(name,"AVERAGE")){ if(a.nnum==0){ c->err=FX_ERR_DIV0; return 0; } return a.sum/(double)a.nnum; }
        if(!strcasecmp(name,"MIN"))     return a.have? a.mn : 0.0;
        if(!strcasecmp(name,"MAX"))     return a.have? a.mx : 0.0;
        return 0;
    }
    if(!strcasecmp(name,"IF")){
        double cond=parse_scalar_arg(c); if(c->err) return 0;
        skipws(c); if(*c->s!=','){ c->err=FX_ERR_SYNTAX; return 0; } c->s++;
        double a=parse_scalar_arg(c); if(c->err) return 0;
        skipws(c); if(*c->s!=','){ c->err=FX_ERR_SYNTAX; return 0; } c->s++;
        double b=parse_scalar_arg(c); if(c->err) return 0;
        skipws(c); if(*c->s!=')'){ c->err=FX_ERR_SYNTAX; return 0; } c->s++;
        return (cond!=0.0)? a : b;
    }
    if(!strcasecmp(name,"ABS")){
        double x=parse_scalar_arg(c); if(c->err) return 0;
        skipws(c); if(*c->s!=')'){ c->err=FX_ERR_SYNTAX; return 0; } c->s++;
        return fabs(x);
    }
    if(!strcasecmp(name,"ROUND")){
        double x=parse_scalar_arg(c); if(c->err) return 0;
        skipws(c); if(*c->s!=','){ c->err=FX_ERR_SYNTAX; return 0; } c->s++;
        double nd=parse_scalar_arg(c); if(c->err) return 0;
        skipws(c); if(*c->s!=')'){ c->err=FX_ERR_SYNTAX; return 0; } c->s++;
        double p=pow(10.0,nd);
        double scaled=x*p;
        double r=floor(scaled + (scaled>=0?0.5:-0.5));
        return r/p;
    }
    c->err=FX_ERR_NAME;   // unknown function name
    return 0;
}

static double parse_primary(pctx *c){
    skipws(c);
    char ch=*c->s;
    if(ch=='('){
        c->s++;
        double v=parse_compare(c);
        if(c->err) return 0;
        skipws(c);
        if(*c->s!=')'){ c->err=FX_ERR_SYNTAX; return 0; }
        c->s++;
        return v;
    }
    if(isdigit((unsigned char)ch) || ch=='.'){
        char *end=0;
        double v=strtod(c->s,&end);
        if(end==c->s){ c->err=FX_ERR_SYNTAX; return 0; }
        c->s=end;
        return v;
    }
    if(ch=='$'){                          // must be a cell ref
        int r,cc;
        if(try_ref(c,&r,&cc)!=0){ c->err=FX_ERR_REF; return 0; }
        return cell_num(cell_peek(c->sh,r,cc));
    }
    if(isalpha((unsigned char)ch)){
        // Read a letter run to see whether a '(' (function) follows.
        const char *start=c->s;
        char name[32]; int ni=0;
        while(isalpha((unsigned char)*c->s)){
            if(ni<(int)sizeof(name)-1) name[ni++]=*c->s;
            c->s++;
        }
        name[ni]=0;
        const char *after=c->s;
        while(*after==' '||*after=='\t') after++;
        if(*after=='('){                  // function call
            c->s=after+1;
            return call_function(c,name);
        }
        // A cell ref is letters immediately followed by '$' or a digit.
        if(*c->s=='$' || isdigit((unsigned char)*c->s)){
            c->s=start;
            int r,cc;
            if(try_ref(c,&r,&cc)==0)
                return cell_num(cell_peek(c->sh,r,cc));
            c->err=FX_ERR_REF;            // ref-shaped but invalid (e.g. A0)
            return 0;
        }
        // Pure identifier, not a function and not a cell ref -> unknown name.
        c->err=FX_ERR_NAME;
        return 0;
    }
    c->err=FX_ERR_SYNTAX;
    return 0;
}

static double parse_postfix(pctx *c){
    double v=parse_primary(c);
    if(c->err) return 0;
    skipws(c);
    while(*c->s=='%'){ c->s++; v=v/100.0; skipws(c); }
    return v;
}

static double parse_unary(pctx *c);

static double parse_power(pctx *c){
    double base=parse_postfix(c);
    if(c->err) return 0;
    skipws(c);
    if(*c->s=='^'){
        c->s++;
        double e=parse_unary(c);          // right-associative, allows -exp
        if(c->err) return 0;
        return pow(base,e);
    }
    return base;
}

static double parse_unary(pctx *c){
    skipws(c);
    if(*c->s=='-'){ c->s++; double v=parse_unary(c); return -v; }
    if(*c->s=='+'){ c->s++; return parse_unary(c); }
    return parse_power(c);
}

static double parse_mul(pctx *c){
    double v=parse_unary(c);
    if(c->err) return 0;
    for(;;){
        skipws(c);
        char op=*c->s;
        if(op=='*' || op=='/'){
            c->s++;
            double r=parse_unary(c);
            if(c->err) return 0;
            if(op=='*') v*=r;
            else { if(r==0.0){ c->err=FX_ERR_DIV0; return 0; } v/=r; }
        } else break;
    }
    return v;
}

static double parse_add(pctx *c){
    double v=parse_mul(c);
    if(c->err) return 0;
    for(;;){
        skipws(c);
        char op=*c->s;
        if(op=='+' || op=='-'){
            c->s++;
            double r=parse_mul(c);
            if(c->err) return 0;
            if(op=='+') v+=r; else v-=r;
        } else break;
    }
    return v;
}

static double parse_compare(pctx *c){
    double v=parse_add(c);
    if(c->err) return 0;
    skipws(c);
    char a=c->s[0];
    char b=a ? c->s[1] : 0;   // never read past the terminator
    int op=0;   // 1:= 2:<> 3:< 4:> 5:<= 6:>=
    if(a=='='){ op=1; c->s+=1; }
    else if(a=='<' && b=='>'){ op=2; c->s+=2; }
    else if(a=='<' && b=='='){ op=5; c->s+=2; }
    else if(a=='>' && b=='='){ op=6; c->s+=2; }
    else if(a=='<'){ op=3; c->s+=1; }
    else if(a=='>'){ op=4; c->s+=1; }
    if(op){
        double r=parse_add(c);
        if(c->err) return 0;
        int res=0;
        switch(op){
            case 1: res=(v==r); break;
            case 2: res=(v!=r); break;
            case 3: res=(v<r);  break;
            case 4: res=(v>r);  break;
            case 5: res=(v<=r); break;
            case 6: res=(v>=r); break;
        }
        return res?1.0:0.0;
    }
    return v;
}

fx_status fx_eval(workbook *wb, sheet *s, int row, int col,
                  const char *src, double *out){
    if(out) *out=0;
    if(!src) return FX_ERR_SYNTAX;
    pctx c; c.s=src; c.wb=wb; c.sh=s; c.row=row; c.col=col; c.err=FX_OK;
    double v=parse_compare(&c);
    if(c.err!=FX_OK) return c.err;
    skipws(&c);
    if(*c.s!=0) return FX_ERR_SYNTAX;      // trailing garbage
    if(out) *out=v;
    return FX_OK;
}

void fx_recalc(workbook *wb){
    if(!wb) return;
    for(int si=0; si<wb->nsheet; si++){
        sheet *s=&wb->sheets[si];
        long tot=(long)s->nrows*s->ncols;
        for(long idx=0; idx<tot; idx++){
            cell *cl=&s->cells[idx];
            if(cl->type==CELL_FORMULA && cl->formula){
                int row=(int)(idx / s->ncols), col=(int)(idx % s->ncols);
                double v=0;
                fx_status st=fx_eval(wb,s,row,col,cl->formula,&v);
                cl->num = (st==FX_OK) ? v : 0.0;
            }
        }
    }
}
