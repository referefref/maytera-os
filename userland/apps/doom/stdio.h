// stdio.h stub for DOOM
#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <stdint.h>

typedef void FILE;
extern FILE *stdout;
extern FILE *stderr;

#define MAXINT 0x7FFFFFFF

// Use doom_printf
int doom_printf(const char *fmt, ...);
#define fprintf(f, ...) doom_printf(__VA_ARGS__)

#endif
