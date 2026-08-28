// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// stdlib.h - Standard library for MayteraOS userland
#ifndef LIBC_STDLIB_H
#define LIBC_STDLIB_H

#include <stddef.h>
#include <stdint.h>

// Memory allocation
void *malloc(size_t size);
// #613: peak bytes the heap arena has ever grown to (never shrinks).
size_t malloc_heap_highwater(void);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

// Process control
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void exit(int status) __attribute__((noreturn));

// system(): ISO C command processor. MayteraOS has NO command processor a
// program can hand a shell string to, and this reports that honestly rather
// than pretending:
//   system(NULL) returns 0, which is precisely the standard's way of saying
//                "no command processor is available";
//   system(cmd)  returns -1 with errno = ENOSYS.
// A version that returned 0 for a command it never ran would make every caller
// believe the command succeeded. Added for the Lua 5.4 port (local queue 91):
// os.execute() calls this and reports the failure to the script.
int system(const char *command);
void abort(void) __attribute__((noreturn));
int atexit(void (*func)(void));

// String conversion
int atoi(const char *str);
long atol(const char *str);
double atof(const char *str);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
float  strtof(const char *nptr, char **endptr);

// Searching and sorting
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

// Pseudo-random numbers
int rand(void);
void srand(unsigned int seed);
#define RAND_MAX 32767

// Absolute value
int abs(int n);
long labs(long n);
long long llabs(long long n);

// File I/O functions (POSIX-style)
int open(const char *path, int flags, ...);
int close(int fd);
// #695: commit fd's buffered bytes to the medium WITHOUT consuming the fd.
// Returns 0 only if every byte is on the medium; negative on failure.
//
// USE THIS, NOT close(), TO DECIDE THAT A FILE IS SAFE. close() may report the
// error, but it consumes the fd either way: by the time it tells you, you have
// no handle to retry with, and calling close() again can close another thread's
// fd. The pattern that protects data is:
//     write(fd, ...) ; if (fsync(fd) != 0) { /* the destination is DESTROYED */ }
//
// ON A NON-ZERO RETURN the destination file may be EMPTY OR ABSENT and is NEVER
// its previous contents (both filesystems free or delete the old data before
// writing the new). Do NOT delete or overwrite the source you were copying from.
//
// fflush() is NOT this. fflush() only drains the stdio buffer into write(); the
// bytes are then in the kernel, not on the medium.
int fsync(int fd);
long read(int fd, void *buf, size_t count);
long write(int fd, const void *buf, size_t count);

// File opening flags
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

// Time functions
long clock(void);

// Environment (per-process, starts empty; round-trips within the process)
extern char **environ;
void  __libc_init_env(char **envp);   // #112: crt0 hands over the inherited block
char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   putenv(char *string);

#endif // LIBC_STDLIB_H
