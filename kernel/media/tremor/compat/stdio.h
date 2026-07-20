/* Minimal <stdio.h> for vendored Tremor. The FILE-based ov_open/ov_fopen path
 * is never used (we open via ov_open_callbacks); these are link stubs only. */
#ifndef _MAYTERA_TREMOR_STDIO_H
#define _MAYTERA_TREMOR_STDIO_H
#include "stddefsz.h"
#ifndef NULL
#define NULL ((void*)0)
#endif
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
typedef struct _MAYTERA_FILE FILE;
FILE  *fopen(const char *, const char *);
size_t fread(void *, size_t, size_t, FILE *);
int    fseek(FILE *, long, int);
long   ftell(FILE *);
int    fclose(FILE *);
#endif
