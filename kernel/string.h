// string.h - Basic string and memory functions
#ifndef STRING_H
#define STRING_H

#include "types.h"

// Memory functions
void *memset(void *dest, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

// String functions
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t maxlen);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
int strcmp(const char *s1, const char *s2);
// #tls-suppressfix: strcasecmp has been DEFINED in string.c since forever and
// declared in no header, so every caller called it implicitly. fs/exfat.c
// silenced the resulting -Wimplicit-function-declaration file-wide rather than
// adding this one line. An implicit declaration compiles to a call the compiler
// guessed the prototype for; here the guess (int, no arg checking) happened to
// match, so nothing broke, but nothing was checking either.
int strcasecmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

// Conversion functions
int atoi(const char *s);
long atol(const char *s);
char *itoa(int value, char *str, int base);
char *ltoa(long value, char *str, int base);
char *ultoa(unsigned long value, char *str, int base);

// Extended conversion functions
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);

// Formatted output
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);
// deadport: same as vsnprintf(), but also reports how many characters did not
// fit. THIS IS THE ONLY WAY TO DETECT TRUNCATION HERE: vsnprintf() returns the
// bytes actually WRITTEN (capped at size-1), not the C99 "would have written",
// so the standard `if (n >= size)` test can never be true and every copy of it
// in this tree is dead code. *dropped is 0 when the whole line fitted.
int vsnprintf_dropped(char *buf, size_t size, const char *fmt,
                      __builtin_va_list ap, size_t *dropped);

// #672: THE shared format parser. One definition of the printf format language
// for the whole kernel; the DESTINATION is the caller's, supplied as a sink.
// This exists because five independent hand-rolled parsers had drifted apart,
// and four of them consumed no argument for a specifier they did not recognise,
// which shifts every later argument in the line (see the long comment in
// string.c). Do not add a sixth: pass a sink.
typedef void (*kfmt_sink_t)(void *ctx, char c);
void kvformat(kfmt_sink_t sink, void *ctx, const char *fmt, __builtin_va_list ap);

// #672 boot self-test. Formats known width/flag/precision vectors and compares
// against expected output, including the exact SMAP pre-flight format string
// whose argument shift was the reported symptom. Logs [KFMT-SELFTEST].
void kformat_selftest(void);

// Character functions
static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline int isalnum(int c) { return isdigit(c) || isalpha(c); }
static inline int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int toupper(int c) { return islower(c) ? c - 32 : c; }
static inline int tolower(int c) { return isupper(c) ? c + 32 : c; }
static inline int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

#endif // STRING_H
