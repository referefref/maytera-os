/* Minimal <string.h> for the vendored fixed-point libopus (#331). The kernel
 * provides the actual mem and str routines (string.c / memcpy_fast.asm). */
#ifndef _MAYTERA_OPUS_STRING_H
#define _MAYTERA_OPUS_STRING_H
#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int   memcmp(const void *, const void *, size_t);
void *memchr(const void *, int, size_t);
size_t strlen(const char *);
int   strcmp(const char *, const char *);
int   strncmp(const char *, const char *, size_t);
char *strcpy(char *, const char *);
char *strncpy(char *, const char *, size_t);
char *strcat(char *, const char *);
char *strchr(const char *, int);
#endif
