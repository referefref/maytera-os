// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
//
// strncpy_test.c - #231 regression test for the MayteraOS KERNEL strncpy()/
// strncat() off-by-one (kernel/string.c), the second live instance of the
// same bug found in userland/libc/string.c (see that file's own
// tests/strncpy_test.c for the primary writeup).
//
// WHAT THIS PROVES
//   kernel/string.c's own strncpy()/strncat() are linked in here as
//   mos_strncpy()/mos_strncat(), so what runs is the ACTUAL shipping
//   translation unit. Built with AddressSanitizer, so a one-byte stack
//   overflow is a hard, visible ASan report.
//
//   THE NEGATIVE CONTROL IS NOT OPTIONAL: run_strncpy.sh builds this same
//   harness against fixtures/string.c.prefix231, a frozen copy of the
//   PRE-FIX kernel/string.c, and REQUIRES AddressSanitizer to report a
//   stack-buffer-overflow. A test that passes against the broken code is
//   testing nothing.
//
//   EACH DESTINATION IS ITS OWN PLAIN STACK ARRAY, sized to EXACTLY n
//   bytes, deliberately NOT a struct with a trailing canary field - ASan
//   does not redzone between members of the same object, only between
//   separate stack allocations. See userland/libc/tests/strncpy_test.c for
//   the full reasoning.
//
// Build/run: ./run_strncpy.sh (in this directory).

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

// The units under test, renamed at compile time (see run_strncpy.sh) so they
// can coexist with glibc's own strncpy/strncat in this same host binary.
char *mos_strncpy(char *dest, const char *src, unsigned long n);
char *mos_strncat(char *dest, const char *src, unsigned long n);

// --- stubs the kernel string.c TU needs when hosted -------------------------
// kernel/string.c declares these `extern` (real definitions are hand-written
// x86-64 asm) and calls them from memcpy/memset/memmove and other functions
// that compile into this same TU even though this test never calls them
// directly. A trivial C body is enough to satisfy the linker.
void *memcpy_fast(void *dest, const void *src, unsigned long n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned long i = 0; i < n; i++) d[i] = s[i];
    return dest;
}
void *memset_fast(void *s, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)s;
    for (unsigned long i = 0; i < n; i++) p[i] = (unsigned char)c;
    return s;
}
void *memmove_fast(void *dest, const void *src, unsigned long n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) { for (unsigned long i = 0; i < n; i++) d[i] = s[i]; }
    else       { while (n--) d[n] = s[n]; }
    return dest;
}

// kernel/string.c's own kformat_selftest()/kfmt_expect() call kprintf() for
// diagnostic output; a trivial variadic stub satisfies the linker (this test
// never triggers that self-test path, but the symbol is referenced from code
// that compiles into this same TU).
#include <stdarg.h>
int kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

static int failures = 0;
static int checks = 0;

static void ck(const char *what, int cond) {
    checks++;
    if (cond) { printf("ok    %s\n", what); }
    else      { printf("FAIL  %s\n", what); failures++; }
}

int main(void) {
    printf("=== #231 KERNEL strncpy/strncat canary bounds ===\n");

    // ---- TEST 1: THE #223/#231 SHAPE --------------------------------------
    {
        char buf[8];
        memset(buf, 0x55, sizeof(buf));
        mos_strncpy(buf, "ab", sizeof(buf));
        ck("strncpy short-src: content correct",
           buf[0] == 'a' && buf[1] == 'b' && buf[2] == '\0');
        ck("strncpy short-src: pads remainder with NUL",
           buf[3] == '\0' && buf[4] == '\0' && buf[5] == '\0' &&
           buf[6] == '\0' && buf[7] == '\0');
    }

    // ---- TEST 2: LONG, NON-TERMINATED SOURCE (standard-preserving) --------
    {
        char buf[4];
        memset(buf, 0x55, sizeof(buf));
        const char *src = "ABCDEFGH";
        mos_strncpy(buf, src, sizeof(buf));
        ck("strncpy long-src: copies exactly n bytes",
           buf[0] == 'A' && buf[1] == 'B' && buf[2] == 'C' && buf[3] == 'D');
    }

    // ---- TEST 3: n == 0 edge case ------------------------------------------
    {
        char buf[1];
        buf[0] = (char)0x33;
        mos_strncpy(buf, "x", 0);
        ck("strncpy n=0: writes nothing", buf[0] == (char)0x33);
    }

    // ---- TEST 4: strncat sibling check --------------------------------------
    {
        char buf[8];
        memset(buf, 0, sizeof(buf));
        mos_strncat(buf, "ab", sizeof(buf) - strlen(buf) - 1);
        ck("strncat short-src: content correct",
           buf[0] == 'a' && buf[1] == 'b' && buf[2] == '\0');
    }
    {
        char buf[4];
        memset(buf, 0, sizeof(buf));
        mos_strncat(buf, "abc", 3);
        ck("strncat exact-budget: content correct",
           buf[0] == 'a' && buf[1] == 'b' && buf[2] == 'c' && buf[3] == '\0');
    }
    {
        char buf[4];
        memset(buf, 0, sizeof(buf));
        mos_strncat(buf, "a", 3);
        ck("strncat short-src tight-budget: content correct",
           buf[0] == 'a' && buf[1] == '\0');
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
