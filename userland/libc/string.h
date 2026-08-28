// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// string.h - String functions for MayteraOS userland
#ifndef LIBC_STRING_H
#define LIBC_STRING_H

#include <stddef.h>
#include <stdint.h>

// Memory functions
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
void *memchr(const void *s, int c, size_t n);

// (#73) secure_zero - erase a credential buffer so it CANNOT be optimised away.
//
// WHY THIS EXISTS AND WHY IT IS NOT memset(). A compiler is allowed to delete a
// memset() whose result is never read again ("dead store elimination"), which is
// exactly the shape every credential wipe has: you zero the password precisely
// because you are finished with it. gcc -O2 does this in real programs. The
// wipe you can read in the source is then not in the binary, and the password
// stays in .bss for the life of the process.
//
// The fix is a volatile-qualified pointer: writes through it are observable
// behaviour, so the standard forbids eliding them. This is the same mechanism
// as OpenBSD's explicit_bzero / C11 memset_s.
//
// ONE definition, per CLAUDE.md's shared-primitive rule: every credential
// surface in the OS calls THIS, not a private per-file copy. Callers today are
// the lock screen (lockscreen.c) and the elevation prompt (elevate.c).
void secure_zero(void *p, size_t n);

// String length and copy
size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
size_t strlcpy(char *dest, const char *src, size_t size);

// String comparison
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);

// strcoll(): compare two strings in the current locale's collating order.
// MayteraOS has only the C locale (see locale.h), and in the C locale the
// collating order IS the byte order, so this is strcmp() by definition, not by
// approximation. Added for the Lua 5.4 port (local queue 91): lvm.c's
// l_strcmp() uses it for every Lua `<` between strings.
int strcoll(const char *s1, const char *s2);
int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);

// String concatenation
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
size_t strlcat(char *dest, const char *src, size_t size);

// String searching
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);

// Bounded length and tokenizers
size_t strnlen(const char *s, size_t maxlen);
char *strtok(char *str, const char *delim);
char *strtok_r(char *str, const char *delim, char **saveptr);
char *strsep(char **stringp, const char *delim);
void *memccpy(void *dest, const void *src, int c, size_t n);

// String duplication (requires malloc)
char *strdup(const char *s);
char *strndup(const char *s, size_t n);

#endif // LIBC_STRING_H
