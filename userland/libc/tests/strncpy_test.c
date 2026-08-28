// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// strncpy_test.c - #231 regression test for the MayteraOS userland libc
// strncpy()/strncat() off-by-one (userland/libc/string.c).
//
// WHAT THIS PROVES
//   The libc's own strncpy()/strncat() are linked in here as mos_strncpy()/
//   mos_strncat(), so what runs is the ACTUAL shipping translation unit, not
//   a private reimplementation that can drift from what ships. Built with
//   AddressSanitizer, so a one-byte stack overflow is a hard, visible
//   ASan report rather than "the value looked fine on this run".
//
//   THE NEGATIVE CONTROL IS NOT OPTIONAL: run_strncpy.sh builds this same
//   harness against fixtures/string.c.prefix231, a frozen copy of the
//   PRE-FIX string.c, and REQUIRES AddressSanitizer to report a
//   stack-buffer-overflow for the SHORT, NUL-terminated source case. A test
//   that passes against the broken code is testing nothing, and this exact
//   bug survived one confident code audit already (#223 round 1/2).
//
//   EACH DESTINATION IS ITS OWN PLAIN STACK ARRAY, sized to EXACTLY n bytes,
//   deliberately NOT a struct with a trailing canary field. ASan pads every
//   stack array with its own redzone, but it does NOT insert a redzone
//   between two members of the SAME struct/object - a struct-with-canary
//   version of this test built and ran clean against the KNOWN-BROKEN
//   fixture (caught only by this file's own manual byte check, not by
//   ASan), which is exactly the false-negative shape this test exists to
//   avoid. A lone n-byte array is the shape that makes ASan's own redzone
//   the ground truth.
//
//   THE COUNTERINTUITIVE SHAPE, proven here rather than just asserted: the
//   bug fires on a SHORT source (2 bytes into an 8-byte destination) and
//   does NOT fire on a LONG, non-NUL-terminated source that exactly fills
//   the destination. Do not "simplify" this test to a single long-payload
//   case; that is the exact wrong intuition #223's original repro built on.
//
// Build/run: ./run_strncpy.sh (in this directory).

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

// The units under test, renamed at compile time (see run_strncpy.sh) so they
// can coexist with glibc's own strncpy/strncat in this same host binary.
char *mos_strncpy(char *dest, const char *src, size_t n);
char *mos_strncat(char *dest, const char *src, size_t n);

// --- stubs the libc TU needs when hosted ------------------------------------
// string.c declares these `extern` (real definitions are hand-written x86-64
// asm in the freestanding build) and calls them from memcpy/memset/memmove,
// strcat, strlcpy, strlcat etc, which all compile into this same TU even
// though this test never calls them directly. A trivial C body is enough to
// satisfy the linker; it is not what is under test.
void *memcpy_fast(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}
void *memset_fast(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return s;
}
void *memmove_fast(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }
    else       { while (n--) d[n] = s[n]; }
    return dest;
}

static int failures = 0;
static int checks = 0;

static void ck(const char *what, int cond) {
    checks++;
    if (cond) { printf("ok    %s\n", what); }
    else      { printf("FAIL  %s\n", what); failures++; }
}

int main(void) {
    printf("=== #231 strncpy/strncat canary bounds ===\n");

    // ---- TEST 1: THE #223/#231 SHAPE --------------------------------------
    // strncpy(dst, src, sizeof(dst)) - the single most common way this call
    // is written - with a SHORT, NUL-terminated source. On the pre-fix code
    // this writes n+1 bytes: one past dest[7], into ASan's redzone. A 2-byte
    // source into an 8-byte buffer is enough; no long payload is required.
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

    // ---- TEST 2: LONG, NON-TERMINATED SOURCE ------------------------------
    // Standard strncpy: when src has NO NUL in the first n bytes, copy
    // EXACTLY n bytes and do NOT append a terminator. This is the
    // counterintuitive-but-correct case the bug report warns about: it is
    // ALREADY safe on the pre-fix code (that is why #223's 128-byte repro
    // was irrelevant), and the fix must not change this shape - callers that
    // add their own dst[n-1]=0 depend on it.
    {
        char buf[4];
        memset(buf, 0x55, sizeof(buf));
        const char *src = "ABCDEFGH";  // far longer than n=4, no NUL in first 4
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

    // ---- TEST 4: strncat sibling check (#231 task 4) -----------------------
    // Same visual shape (n-- used as a loop guard) prompted the same
    // suspicion. Proven here rather than assumed: a short, NUL-terminated
    // source appended to an empty destination, with the classic
    // sizeof(dst)-strlen(dst)-1 budget.
    {
        char buf[8];
        memset(buf, 0, sizeof(buf));
        mos_strncat(buf, "ab", sizeof(buf) - strlen(buf) - 1);
        ck("strncat short-src: content correct",
           buf[0] == 'a' && buf[1] == 'b' && buf[2] == '\0');
    }
    {
        // Tightest legal budget per the STANDARD's own strncat contract (n
        // content bytes + 1 terminator = n+1 total): dest holds exactly
        // n+1 bytes. This is NOT the same contract as strncpy; writing up
        // to index n here is correct, not a bug.
        char buf[4];
        memset(buf, 0, sizeof(buf));
        mos_strncat(buf, "abc", 3);  // n=3, buf holds exactly n+1=4 bytes
        ck("strncat exact-budget: content correct",
           buf[0] == 'a' && buf[1] == 'b' && buf[2] == 'c' && buf[3] == '\0');
    }
    {
        // strncat's ACTUAL suspect shape: the loop's own copy of src's
        // terminating NUL consumes one unit of the n budget (n-- fires on
        // that iteration too, unlike strncpy's bug), and the function THEN
        // unconditionally appends a SECOND, redundant terminator afterward.
        // That double-write lands at dest[srclen+1], which is still <= the
        // standard's own n-byte bound whenever srclen < n - so it stays
        // in-bounds rather than being a second #231. Proven here, not
        // assumed: dest sized to the TIGHTEST legal budget (n+1 bytes) with
        // a SHORT source (srclen=1 < n=3) is exactly where a real second
        // overflow would show up if this reasoning were wrong.
        char buf[4];
        memset(buf, 0, sizeof(buf));
        mos_strncat(buf, "a", 3);  // n=3, srclen=1 < n, tightest n+1=4 buf
        ck("strncat short-src tight-budget: content correct",
           buf[0] == 'a' && buf[1] == '\0');
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
