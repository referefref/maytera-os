// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// printf_float_test.c - #621 regression test for the MayteraOS userland libc
// float formatter (userland/libc/stdio.c).
//
// WHAT THIS PROVES
//   The libc's own vsnprintf() is linked in here as mos_vsnprintf() and driven
//   with the float conversions that used to smash emit_float()'s stack frame.
//   Built with AddressSanitizer, so a stack-buffer-overflow is a hard, visible
//   failure rather than "it seemed to work".
//
//   The NEGATIVE CONTROL is not optional: run.sh builds this against the
//   PRE-FIX stdio.c first and requires ASan to REPORT an overflow. A test that
//   passes against the broken code is testing nothing.
//
//   Where the value is representable in <= 17 significant digits the output is
//   also diffed against the host glibc printf, which is a real oracle.
//   Beyond that MayteraOS's generator caps at 36 significant digits and pads
//   with '0', so it legitimately differs from glibc; those cases are checked
//   for shape (length, prefix, no crash), not exact equality. That limit is a
//   property of the fixed-point-ish double loop in gen_digits(), not something
//   this fix changed.
//
// Build/run: ./run.sh (in this directory).

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>

// The unit under test, renamed at compile time so it can coexist with glibc.
int mos_snprintf(char *str, size_t size, const char *format, ...);

// --- stubs the libc TU needs when hosted -----------------------------------
long syscall0(long num) { (void)num; return 0; }
long syscall1(long num, long a1) { (void)num; (void)a1; return 0; }
int unlink(const char *p) { (void)p; return -1; }
int rmdir(const char *p) { (void)p; return -1; }

static int failures = 0;
static int checks = 0;

// Guarded call: the destination is deliberately huge, so any overflow that
// ASan reports comes from stdio.c's own internal buffers, not from ours.
static void run(const char *fmt, double v) {
    static char out[8192];
    checks++;
    memset(out, 0xAA, sizeof(out));
    int n = mos_snprintf(out, sizeof(out), fmt, v);
    if (n < 0 || n >= (int)sizeof(out)) {
        printf("FAIL  %-12s -> bad return %d\n", fmt, n);
        failures++;
        return;
    }
    if ((int)strlen(out) != n) {
        printf("FAIL  %-12s -> strlen %zu != return %d\n", fmt, strlen(out), n);
        failures++;
        return;
    }
    printf("ok    %-12s len=%-5d %.48s%s\n", fmt, n, out, n > 48 ? "..." : "");
}

// Exact-match check against glibc for values that fit in double's honest range.
static void oracle(const char *fmt, double v) {
    char mine[8192], theirs[8192];
    checks++;
    int a = mos_snprintf(mine, sizeof(mine), fmt, v);
    int b = snprintf(theirs, sizeof(theirs), fmt, v);
    if (a != b || strcmp(mine, theirs) != 0) {
        printf("FAIL  oracle %-10s mine=[%s] glibc=[%s]\n", fmt, mine, theirs);
        failures++;
    } else {
        printf("ok    oracle %-10s [%s]\n", fmt, mine);
    }
}

int main(void) {
    printf("=== #621 printf float bounds ===\n");

    // 1. Ordinary large doubles. build_f()'s integer loop runs E+1 times;
    //    1e300 needs 301 bytes and the body buffer used to be 80.
    run("%f", 1e300);
    run("%f", -1e308);
    run("%f", 1.7976931348623157e308);   // DBL_MAX
    run("%f", 1e100);
    run("%F", 9.99e299);

    // 2. Unbounded precision.
    run("%.100f", 3.14159265358979);
    run("%.200e", 2.718281828459045);
    run("%.300f", 1.0);
    run("%.400f", 0.1);
    run("%.500e", 1.0);

    // 3. Both at once: large exponent AND large precision.
    run("%.100f", 1e300);
    run("%.200f", -1e308);

    // 4. %g derives its own precision (P-1-X) and can exceed the requested one.
    run("%.100g", 1e-300);
    run("%g", 1e300);
    run("%.50g", 5e-324);                // smallest subnormal
    run("%#.200g", 1.5);

    // 5. Width interacting with a huge body.
    run("%400.100f", 1e300);
    run("%-400.100f", -1e300);

    // 6. Specials must stay untouched.
    oracle("%f", 0.0);
    oracle("%f", -0.0);
    oracle("%.3f", 2.5);
    oracle("%.0f", 0.5);
    oracle("%e", 1234.5678);
    oracle("%.6e", 0.000123);
    oracle("%g", 100000.0);
    oracle("%g", 0.0001);
    oracle("%.2f", 1.005);
    oracle("%12.4f", 3.14159);
    oracle("%-12.4f", 3.14159);
    oracle("%+.3e", -42.0);
    oracle("%08.2f", -1.5);

    // 7. #621 follow-up: ROUNDING correctness in build_f(), not bounds. Each
    //    of these produced a WRONG string before the follow-up fix, and the
    //    frozen pre-#621 fixture still does, so these are not regressions
    //    introduced by the bounds work. Two defects, both described in full in
    //    stdio.c's build_f() comment: the "below the last printed place"
    //    branch never rounded at all, and the significant-digit count came
    //    from a ONE-DIGIT probe whose carry made the rounding land one place
    //    too far right, where the display loop then truncated it away.
    oracle("%.0f", 0.9);                      // was "0"
    oracle("%.0f", 0.6);                      // was "0"
    oracle("%.2f", 0.006);                    // was "0.00"
    oracle("%.0f", 9.9);                      // was "9"
    oracle("%.0f", 99.9);                     // was "99"
    oracle("%.0f", 9.5);                      // was "9"
    oracle("%.1f", 97497.182552569167);       // was "97497.1"
    oracle("%.1f", 0.99);                     // was "0.9"
    oracle("%.2f", 0.999);                    // was "0.99"
    oracle("%.2f", 0.0093507010154073051);    // was "0.00"
    oracle("%.6f", 9.7817937461262859e-06);   // was "0.000009"
    oracle("%#.0f", 9.9);                     // was "9."

    //    The other direction matters just as much: an exact half still rounds
    //    to even, and a value below a tenth of the last printed place stays
    //    zero even when its leading digit is 9. The probe carries for those,
    //    which made an intermediate version of this fix print "1" for 0.095.
    oracle("%.0f", 0.5);
    oracle("%.0f", 0.4);
    oracle("%.0f", 0.095);
    oracle("%.0f", 96.5);
    oracle("%.0f", 2.5);

    // 8. #621 follow-up: spad() must stop looping once the destination is
    //    full. It used to call sc() the full count regardless, so a
    //    caller-controlled field width ran billions of no-op iterations (a
    //    sweep including width 2147483647 wedged for over ten minutes). The
    //    C99 return value must be unchanged, so assert the exact count.
    {
        static char wbuf[64];
        checks++;
        int wn = mos_snprintf(wbuf, sizeof(wbuf), "%2000000000.2f", 1.0);
        if (wn != 2000000000) {
            printf("FAIL  huge width -> return %d, expected 2000000000\n", wn);
            failures++;
        } else {
            printf("ok    huge width returns %d without spinning\n", wn);
        }
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
