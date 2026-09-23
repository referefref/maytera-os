#include "docmodel.h"
#include <stdlib.h>
#include <string.h>

// Rich-text document model. Owner: agent 3 (officemodel). See docmodel.h.
// Arrays grow by realloc; note that appending to document.images can move the
// array, so callers that hold a doc_image* from doc_add_image should add all
// images before wiring DBLK_IMAGE blocks (or re-fetch &d->images[i]).

static char *dupstr(const char *s){
    if(!s) return 0;
    unsigned long n=strlen(s)+1;
    char *p=(char*)malloc(n);
    if(p) memcpy(p,s,n);
    return p;
}

document *doc_new(void){ return (document*)calloc(1,sizeof(document)); }

static void free_para_body(doc_para *p){
    if(!p) return;
    for(int i=0;i<p->nrun;i++) free(p->runs[i].text);
    free(p->runs);
}

static void free_table(doc_table *t){
    if(!t) return;
    long cells=(long)t->rows*t->cols;
    if(t->cells){
        for(long i=0;i<cells;i++){
            if(t->cells[i]){ free_para_body(t->cells[i]); free(t->cells[i]); }
        }
        free(t->cells);
    }
}

void doc_free(document *d){
    if(!d) return;
    for(int i=0;i<d->nblk;i++){
        doc_block *b=&d->blocks[i];
        if(b->type==DBLK_PARA && b->para){ free_para_body(b->para); free(b->para); }
        else if(b->type==DBLK_TABLE && b->table){ free_table(b->table); free(b->table); }
        // DBLK_IMAGE: image is borrowed from d->images, freed in the images loop
    }
    free(d->blocks);
    for(int i=0;i<d->nstyle;i++) free(d->styles[i].name);
    free(d->styles);
    for(int i=0;i<d->nimg;i++) free(d->images[i].bytes);
    free(d->images);
    free(d);
}

doc_block *doc_add_block(document *d, doc_blocktype t){
    if(!d) return 0;
    doc_block *nb=(doc_block*)realloc(d->blocks,(size_t)(d->nblk+1)*sizeof(doc_block));
    if(!nb) return 0;
    d->blocks=nb;
    doc_block *b=&d->blocks[d->nblk++];
    memset(b,0,sizeof(*b));
    b->type=t;
    return b;
}

doc_para *doc_add_para(document *d){
    doc_block *b=doc_add_block(d,DBLK_PARA);
    if(!b) return 0;
    doc_para *p=(doc_para*)calloc(1,sizeof(doc_para));
    if(!p){ d->nblk--; return 0; }
    p->align=DOC_ALIGN_L;
    p->style_id=-1;
    p->list_level=-1;
    b->para=p;
    return p;
}

doc_run *doc_para_add_run(doc_para *p, const char *text, doc_runfmt fmt){
    if(!p) return 0;
    doc_run *nr=(doc_run*)realloc(p->runs,(size_t)(p->nrun+1)*sizeof(doc_run));
    if(!nr) return 0;
    p->runs=nr;
    doc_run *r=&p->runs[p->nrun++];
    r->text=dupstr(text?text:"");
    r->fmt=fmt;
    return r;
}

int doc_add_style(document *d, const char *name, doc_runfmt f, doc_align a){
    if(!d) return -1;
    doc_style *ns=(doc_style*)realloc(d->styles,(size_t)(d->nstyle+1)*sizeof(doc_style));
    if(!ns) return -1;
    d->styles=ns;
    doc_style *s=&d->styles[d->nstyle];
    s->name=dupstr(name);
    s->runfmt=f;
    s->align=a;
    return d->nstyle++;
}

doc_image *doc_add_image(document *d, const char *id, const unsigned char *b, unsigned long n){
    if(!d) return 0;
    doc_image *ni=(doc_image*)realloc(d->images,(size_t)(d->nimg+1)*sizeof(doc_image));
    if(!ni) return 0;
    d->images=ni;
    doc_image *im=&d->images[d->nimg];
    memset(im,0,sizeof(*im));
    if(id){
        unsigned long i=0;
        for(;id[i] && i<sizeof(im->id)-1;i++) im->id[i]=id[i];
        im->id[i]=0;
    }
    if(b && n){
        im->bytes=(unsigned char*)malloc(n);
        if(im->bytes){ memcpy(im->bytes,b,n); im->nbytes=n; }
    }
    d->nimg++;
    return im;
}
