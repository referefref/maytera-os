/* Minimal <stdlib.h> for the vendored fixed-point libopus in the MayteraOS
 * kernel. Routes allocation to the kernel heap; no real libc. Part of #331. */
#ifndef _MAYTERA_OPUS_STDLIB_H
#define _MAYTERA_OPUS_STDLIB_H
#include <stddef.h>
void *kmalloc(unsigned long long);
void *kzalloc(unsigned long long);
void *krealloc(void *, unsigned long long);
void  kfree(void *);
static __inline__ void *malloc(size_t n)           { return kmalloc(n); }
static __inline__ void *calloc(size_t a, size_t b) { return kzalloc(a * b); }
static __inline__ void *realloc(void *p, size_t n) { return krealloc(p, n); }
static __inline__ void  free(void *p)              { kfree(p); }
int  abs(int);
long labs(long);
void abort(void);
#endif
