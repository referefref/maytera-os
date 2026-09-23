// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// Private helpers shared by xlsx.c and ods.c (format agent 6). Everything here
// is `static inline`, so each translation unit gets its own copy and there are
// no duplicate external symbols when both .c files are folded into libmoffice.a.
#ifndef OFFICE_XLSX_ODS_UTIL_H
#define OFFICE_XLSX_ODS_UTIL_H
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ---- small growable string buffer (NUL-terminated) ----
typedef struct { char *buf; unsigned long len, cap; } sb;

static inline void sb_init(sb *b){ b->buf=0; b->len=0; b->cap=0; }
static inline void sb_reset(sb *b){ b->len=0; if(b->buf) b->buf[0]=0; }
static inline int  sb_ensure(sb *b, unsigned long extra){
    if(b->buf && b->len+extra+1 <= b->cap) return 0;
    unsigned long nc = b->cap ? b->cap : 64;
    while(nc < b->len+extra+1) nc *= 2;
    char *nb = (char*)realloc(b->buf, nc);
    if(!nb) return -1;
    b->buf = nb; b->cap = nc; b->buf[b->len]=0;
    return 0;
}
static inline void sb_putn(sb *b, const char *s, unsigned long n){
    if(!s || !n) return;
    if(sb_ensure(b,n)) return;
    memcpy(b->buf+b->len, s, n);
    b->len += n; b->buf[b->len]=0;
}
static inline void sb_puts(sb *b, const char *s){ if(s) sb_putn(b, s, (unsigned long)strlen(s)); }
static inline void sb_putc(sb *b, char c){
    if(sb_ensure(b,1)) return;
    b->buf[b->len++]=c; b->buf[b->len]=0;
}
static inline const char *sb_str(sb *b){ return b->buf ? b->buf : ""; }
static inline void sb_free(sb *b){ free(b->buf); b->buf=0; b->len=b->cap=0; }

// ---- owned string dup (heap; caller/model frees) ----
static inline char *xo_dup(const char *s){
    if(!s) s="";
    unsigned long n=(unsigned long)strlen(s)+1;
    char *p=(char*)malloc(n);
    if(p) memcpy(p,s,n);
    return p;
}

// Minimal number -> text (drops trailing zeros; integers print without a dot).
static inline void xo_num_to_str(double v, char *out, unsigned long cap){
    snprintf(out, (size_t)cap, "%.15g", v);
}

#endif
