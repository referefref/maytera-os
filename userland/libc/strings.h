// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// strings.h - the BSD/POSIX <strings.h> surface for MayteraOS userland,
// added for the libedit port (#745). Distinct from <string.h>: this is the
// legacy set (ffs, bzero, bcopy, bcmp, index, rindex) plus the case-insensitive
// compares. tty.c in libedit includes it for ffs(). Every function is defined
// in string.c as a thin wrapper over the <string.h> primitive, so there is one
// implementation, not two.
#ifndef LIBC_STRINGS_H
#define LIBC_STRINGS_H

#include <stddef.h>

int   ffs(int i);
int   strcasecmp(const char *a, const char *b);
int   strncasecmp(const char *a, const char *b, size_t n);
void  bzero(void *s, size_t n);
void  bcopy(const void *src, void *dst, size_t n);
int   bcmp(const void *a, const void *b, size_t n);
char *index(const char *s, int c);
char *rindex(const char *s, int c);

#endif // LIBC_STRINGS_H
