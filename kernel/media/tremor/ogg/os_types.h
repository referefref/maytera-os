/* MayteraOS Tremor/libogg os_types: route allocation to the kernel heap.
 * Replaces libogg's stock os_types.h (BSD, Xiph.org). No libc, no FPU. */
#ifndef _OS_TYPES_H
#define _OS_TYPES_H

#if !defined(TYPES_H) && !defined(_MAYTERA_SIZE_T_DEFINED)
#define _MAYTERA_SIZE_T_DEFINED
typedef unsigned long long size_t;   /* match the kernel's size_t (uint64_t) */
#endif

extern void *kmalloc(unsigned long long);
extern void *kzalloc(unsigned long long);
extern void *krealloc(void *, unsigned long long);
extern void  kfree(void *);
static __inline__ void *_ogg_calloc_kern(unsigned long long n, unsigned long long s){
  return kzalloc(n * s);
}
#define _ogg_malloc  kmalloc
#define _ogg_calloc  _ogg_calloc_kern
#define _ogg_realloc krealloc
#define _ogg_free    kfree

typedef short              ogg_int16_t;
typedef unsigned short     ogg_uint16_t;
typedef int                ogg_int32_t;
typedef unsigned int       ogg_uint32_t;
typedef long long          ogg_int64_t;
typedef unsigned long long ogg_uint64_t;

#endif
