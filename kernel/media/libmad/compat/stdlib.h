/* Freestanding <stdlib.h> shim for libmad: route malloc/calloc/free to the
 * kernel heap. Macros, so only libmad translation units are affected. */
#ifndef MAYTERA_MAD_COMPAT_STDLIB_H
#define MAYTERA_MAD_COMPAT_STDLIB_H
#include <stddef.h>
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void  kfree(void *ptr);
static inline void *mad_compat_malloc(size_t n)            { return kmalloc(n); }
static inline void *mad_compat_calloc(size_t a, size_t b)  { return kzalloc(a * b); }
static inline void  mad_compat_free(void *p)               { kfree(p); }
#define malloc(n)    mad_compat_malloc(n)
#define calloc(a,b)  mad_compat_calloc((a),(b))
#define free(p)      mad_compat_free(p)
#endif
