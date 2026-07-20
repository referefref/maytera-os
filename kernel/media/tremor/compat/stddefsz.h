#ifndef _MAYTERA_TREMOR_SIZE_H
#define _MAYTERA_TREMOR_SIZE_H
#if !defined(TYPES_H) && !defined(_MAYTERA_SIZE_T_DEFINED)
#define _MAYTERA_SIZE_T_DEFINED
typedef unsigned long long size_t;   /* match the kernel's size_t (uint64_t) */
#endif
#ifndef NULL
#define NULL ((void*)0)
#endif
/* Tremor uses alloca() for scratch buffers; route to the gcc builtin so it
 * works under -fno-builtin (no libc alloca symbol). */
#ifndef alloca
#define alloca(n) __builtin_alloca(n)
#endif
#endif
