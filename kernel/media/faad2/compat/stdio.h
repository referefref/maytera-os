/* Minimal <stdio.h> for vendored faad2 (#331): only printf-in-debug refs. */
#ifndef _MAYTERA_FAAD_STDIO_H
#define _MAYTERA_FAAD_STDIO_H
#include <stddef.h>
#ifndef NULL
#define NULL ((void*)0)
#endif
typedef struct _MAYTERA_FAAD_FILE FILE;
extern FILE *stderr;
int printf(const char *, ...);
int fprintf(FILE *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
#endif
