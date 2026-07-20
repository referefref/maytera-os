/* Minimal <stdio.h> for the vendored fixed-point libopus (#331). Opus only
 * references printf/fprintf inside assert/debug blocks that are compiled out;
 * these declarations exist solely to satisfy the compiler. */
#ifndef _MAYTERA_OPUS_STDIO_H
#define _MAYTERA_OPUS_STDIO_H
#include <stddef.h>
#ifndef NULL
#define NULL ((void*)0)
#endif
typedef struct _MAYTERA_OPUS_FILE FILE;
extern FILE *stderr;
int printf(const char *, ...);
int fprintf(FILE *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
#endif
