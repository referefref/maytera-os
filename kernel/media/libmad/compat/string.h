/* Freestanding <string.h> shim for libmad: declare the mem* the kernel provides. */
#ifndef MAYTERA_MAD_COMPAT_STRING_H
#define MAYTERA_MAD_COMPAT_STRING_H
#include <stddef.h>
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *dest, int c, size_t n);
#endif
