// stdlib.h - Standard library for MayteraOS userland
#ifndef LIBC_STDLIB_H
#define LIBC_STDLIB_H

#include <stddef.h>
#include <stdint.h>

// Memory allocation
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

// Process control
void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

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
char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   putenv(char *string);

#endif // LIBC_STDLIB_H
