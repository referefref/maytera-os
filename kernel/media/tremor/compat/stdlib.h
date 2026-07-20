#ifndef _MAYTERA_TREMOR_STDLIB_H
#define _MAYTERA_TREMOR_STDLIB_H
#include "stddefsz.h"
void *kmalloc(unsigned long long);
void *kzalloc(unsigned long long);
void *krealloc(void *, unsigned long long);
void  kfree(void *);
static __inline__ void *malloc(size_t n)            { return kmalloc(n); }
static __inline__ void *calloc(size_t a, size_t b)  { return kzalloc(a * b); }
static __inline__ void *realloc(void *p, size_t n)  { return krealloc(p, n); }
static __inline__ void  free(void *p)               { kfree(p); }
int  abs(int);
long labs(long);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
#endif
