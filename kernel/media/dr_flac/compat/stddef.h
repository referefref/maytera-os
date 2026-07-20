#ifndef _MAYTERA_DRFLAC_STDDEF_H
#define _MAYTERA_DRFLAC_STDDEF_H
/* Freestanding <stddef.h> shim for the vendored dr_flac. dr_flac only needs
 * size_t and NULL. We must NOT clash with the kernel's size_t (types.h defines
 * it as uint64_t == unsigned long long and sets TYPES_H), so size_t is only
 * defined here when types.h has not already provided it (e.g. the standalone
 * dr_flac.c implementation TU). */
#ifndef NULL
#define NULL ((void*)0)
#endif
#if !defined(TYPES_H) && !defined(_MAYTERA_SIZE_T_DEFINED)
#define _MAYTERA_SIZE_T_DEFINED
typedef unsigned long long size_t;
#endif
#ifndef _MAYTERA_PTRDIFF_T_DEFINED
#define _MAYTERA_PTRDIFF_T_DEFINED
typedef long long ptrdiff_t;
#endif
#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
#endif
