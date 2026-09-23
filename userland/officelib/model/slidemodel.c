#include "slidemodel.h"
#include <stdlib.h>
#include <string.h>

// Presentation model. Owner: agent 3 (officemodel). See slidemodel.h.
// Ownership note for format agents: presentation has no image store, so a
// SHP_IMAGE shape OWNS its doc_image. Malloc the doc_image (and its ->bytes)
// for the shape; pres_free releases both. Do not share one doc_image between
// two shapes. slides[]/shapes[] are contiguous and grow by realloc, so do not
// cache a slide*/shape* across a further add.

presentation *pres_new(void){ return (presentation*)calloc(1,sizeof(presentation)); }

static void free_para_body(doc_para *p){
    if(!p) return;
    for(int i=0;i<p->nrun;i++) free(p->runs[i].text);
    free(p->runs);
}

void pres_free(presentation *p){
    if(!p) return;
    for(int i=0;i<p->nslide;i++){
        slide *s=&p->slides[i];
        free(s->title);
        for(int j=0;j<s->nshape;j++){
            shape *sh=&s->shapes[j];
            for(int k=0;k<sh->npara;k++) free_para_body(&sh->paras[k]);
            free(sh->paras);
            if(sh->image){ free(sh->image->bytes); free(sh->image); }
        }
        free(s->shapes);
    }
    free(p->slides);
    free(p);
}

slide *pres_add_slide(presentation *p){
    if(!p) return 0;
    slide *ns=(slide*)realloc(p->slides,(size_t)(p->nslide+1)*sizeof(slide));
    if(!ns) return 0;
    p->slides=ns;
    slide *s=&p->slides[p->nslide];
    memset(s,0,sizeof(*s));
    p->nslide++;
    return s;
}

shape *slide_add_shape(slide *s, shape_type t){
    if(!s) return 0;
    shape *ns=(shape*)realloc(s->shapes,(size_t)(s->nshape+1)*sizeof(shape));
    if(!ns) return 0;
    s->shapes=ns;
    shape *sh=&s->shapes[s->nshape];
    memset(sh,0,sizeof(*sh));
    sh->type=t;
    s->nshape++;
    return sh;
}
