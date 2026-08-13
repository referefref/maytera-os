// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// stdio.h - Standard I/O for MayteraOS userland
#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

#ifndef SEEK_SET
#define SEEK_SET 0   // task #568: ANSI stdio.h must define these (was unistd.h-only)
#define SEEK_CUR 1
#define SEEK_END 2
#endif

// Output functions
int putchar(int c);
int puts(const char *s);
int printf(const char *format, ...) __attribute__((format(printf, 1, 2)));
int sprintf(char *str, const char *format, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *str, size_t size, const char *format, ...) __attribute__((format(printf, 3, 4)));
int vprintf(const char *format, va_list ap);
int vsprintf(char *str, const char *format, va_list ap);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);

// Input functions (basic)
int getchar(void);

// File operations (simplified)
#define EOF (-1)
#define BUFSIZ 4096

// Standard file descriptors
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

// Stream abstraction
typedef struct FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
int   fclose(FILE *f);
// AssaultCube port phase 3 (docs/ASSAULTCUBE_PORT_PLAN.md): standard
// <stdio.h> remove(), a genuinely missing generic primitive (was only
// exposed as unlink()/rmdir() before). Tries unlink() first (files, the
// common case), falls back to rmdir() for a directory path.
int   remove(const char *path);
size_t fread(void *buf, size_t sz, size_t n, FILE *f);
size_t fwrite(const void *buf, size_t sz, size_t n, FILE *f);
int   fseek(FILE *f, long off, int whence);
long  ftell(FILE *f);
void  rewind(FILE *f);
int   feof(FILE *f);
int   ferror(FILE *f);
int   fflush(FILE *f);
int   fileno(FILE *f);
int   fputc(int c, FILE *f);
int   fputs(const char *s, FILE *f);
int   fgetc(FILE *f);
char *fgets(char *s, int n, FILE *f);
int   ungetc(int c, FILE *f);
int   fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int   vfprintf(FILE *f, const char *fmt, va_list ap);
int   setvbuf(FILE *f, char *buf, int mode, size_t sz);
void  setbuf(FILE *f, char *buf);

// Formatted input
int sscanf(const char *str, const char *fmt, ...) __attribute__((format(scanf, 2, 3)));
int scanf(const char *fmt, ...) __attribute__((format(scanf, 1, 2)));
int fscanf(FILE *f, const char *fmt, ...) __attribute__((format(scanf, 2, 3)));
int vsscanf(const char *str, const char *fmt, va_list ap);
int vfscanf(FILE *f, const char *fmt, va_list ap);

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

void __stdio_init(void);

#endif // LIBC_STDIO_H
