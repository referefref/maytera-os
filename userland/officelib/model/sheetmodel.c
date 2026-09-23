#include "sheetmodel.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Spreadsheet model. Owner: agent 3 (officemodel). See sheetmodel.h.
// The sheets/cells arrays are contiguous and grow by realloc: a wb_add_sheet
// can move sheets[] and a growing sheet_cell can move cells[], so callers must
// not cache a sheet* across wb_add_sheet, nor a cell* across a growing
// sheet_cell. Re-fetch by index. New cells default to style_id = -1.

static char *dupstr(const char *s){
    if(!s) return 0;
    unsigned long n=strlen(s)+1;
    char *p=(char*)malloc(n);
    if(p) memcpy(p,s,n);
    return p;
}

workbook *wb_new(void){ return (workbook*)calloc(1,sizeof(workbook)); }

static void free_cell(cell *c){
    if(!c) return;
    free(c->str);
    free(c->formula);
}

void wb_free(workbook *w){
    if(!w) return;
    for(int i=0;i<w->nsheet;i++){
        sheet *s=&w->sheets[i];
        free(s->name);
        long tot=(long)s->nrows*s->ncols;
        for(long j=0;j<tot;j++) free_cell(&s->cells[j]);
        free(s->cells);
    }
    free(w->sheets);
    for(int i=0;i<w->nsst;i++) free(w->sst[i]);
    free(w->sst);
    for(int i=0;i<w->nstyle;i++) free(w->styles[i].numfmt_code);
    free(w->styles);
    free(w);
}

sheet *wb_add_sheet(workbook *w, const char *name){
    if(!w) return 0;
    sheet *ns=(sheet*)realloc(w->sheets,(size_t)(w->nsheet+1)*sizeof(sheet));
    if(!ns) return 0;
    w->sheets=ns;
    sheet *s=&w->sheets[w->nsheet];
    memset(s,0,sizeof(*s));
    s->name=dupstr(name?name:"");
    w->nsheet++;
    return s;
}

cell *sheet_cell(sheet *s, int row, int col){
    if(!s || row<0 || col<0) return 0;
    if(row < s->nrows && col < s->ncols)
        return &s->cells[(long)row*s->ncols+col];
    int nr = (row+1 > s->nrows) ? row+1 : s->nrows;
    int nc = (col+1 > s->ncols) ? col+1 : s->ncols;
    cell *arr=(cell*)calloc((size_t)nr*nc,sizeof(cell));
    if(!arr) return 0;
    for(long i=0;i<(long)nr*nc;i++) arr[i].style_id=-1;   // default style
    for(int r=0;r<s->nrows;r++)
        for(int c=0;c<s->ncols;c++)
            arr[(long)r*nc+c] = s->cells[(long)r*s->ncols+c];
    free(s->cells);
    s->cells=arr; s->nrows=nr; s->ncols=nc;
    return &s->cells[(long)row*nc+col];
}

int wb_add_style(workbook *w, const wb_style *st){
    if(!w || !st) return -1;
    wb_style *ns=(wb_style*)realloc(w->styles,(size_t)(w->nstyle+1)*sizeof(wb_style));
    if(!ns) return -1;
    w->styles=ns;
    wb_style *d=&w->styles[w->nstyle];
    *d=*st;
    d->numfmt_code = st->numfmt_code ? dupstr(st->numfmt_code) : 0;
    return w->nstyle++;
}

// "A1" / "$C$5" / "AB100" -> 0-based (row,col). Returns 0 on success, -1 on error.
int a1_to_rc(const char *a1, int *row, int *col){
    if(!a1) return -1;
    const char *p=a1;
    if(*p=='$') p++;
    int c=0,nl=0;
    while((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')){
        int d=(*p>='a')?(*p-'a'):(*p-'A');
        c=c*26+(d+1); p++; nl++;
        if(nl>7) return -1;               // absurdly long column
    }
    if(nl==0) return -1;
    if(*p=='$') p++;
    int r=0,nd=0;
    while(*p>='0'&&*p<='9'){ r=r*10+(*p-'0'); p++; nd++; if(nd>9) return -1; }
    if(nd==0) return -1;
    if(*p!=0) return -1;                  // trailing garbage
    if(r==0) return -1;                   // rows are 1-based
    if(row) *row=r-1;
    if(col) *col=c-1;
    return 0;
}

// 0-based (row,col) -> "A1". No '$'. Truncates safely into out[cap].
void rc_to_a1(int row, int col, char *out, unsigned long cap){
    if(!out || cap==0) return;
    out[0]=0;
    if(row<0 || col<0) return;
    char letters[16]; int li=0;
    int c=col;
    do { letters[li++]='A'+(c%26); c=c/26-1; } while(c>=0 && li<16);
    unsigned long o=0;
    for(int i=li-1;i>=0 && o+1<cap;i--) out[o++]=letters[i];
    char digits[16]; int di=0; int rr=row+1;
    while(rr>0 && di<16){ digits[di++]='0'+(rr%10); rr/=10; }
    for(int i=di-1;i>=0 && o+1<cap;i--) out[o++]=digits[i];
    out[o]=0;
}
