/* stdio.h - Duktape-port shim: re-export libc stdio + declare sscanf. */
#ifndef MAYTERA_DUK_STDIO_H
#define MAYTERA_DUK_STDIO_H
#include_next <stdio.h>
extern int sscanf(const char *str, const char *fmt, ...);
#endif
