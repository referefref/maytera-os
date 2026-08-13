// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// getopt_oracle_test.c - #745 (local 72) differential: MayteraOS getopt vs the
// build host's glibc getopt, on the same command lines, comparing the FULL
// trace (every return code, every optarg, optopt where it is specified, the
// final optind, and the operands left behind).
//
// WHY AN ORACLE AND NOT MORE HAND-WRITTEN EXPECTATIONS. Writing down what a
// getopt "should" return is exactly where this went wrong once already while
// this file's sibling battery was being written: a hand-written expectation
// for "prog -a -- -b file" was wrong, and had it not been checked against a
// real implementation it would have been "fixed" by changing the code to match
// the mistake. glibc's getopt is the de facto specification every ported CLI
// tool was written against, so it, and not anyone's recollection, is the
// authority here.
//
// The unit under test is compiled from the SHIPPING getopt.c with its public
// names renamed to mos_*, so it can sit in the same binary as glibc's.
//
// NOTE ON RESETTING: glibc only reinitializes its scan state when optind is
// set to 0 (setting it to 1 is not enough and leaves stale state from the
// previous scan, which produces nonsense). Both sides are reset with 0 here.
#include <stdio.h>
#include <string.h>
#include <getopt.h>

extern int mos_getopt(int argc, char *const argv[], const char *optstring);
extern char *mos_optarg;
extern int mos_optind, mos_opterr, mos_optopt;

static char tg[8192], tm[8192];

// optopt is only specified after a '?' or ':' return, so it is only compared
// there. Return codes print numerically: getopt legitimately returns
// non-printable values (1 for an operand under the '-' ordering flag).
static void run_glibc(int argc, char **av, const char *os) {
    int c;
    size_t n = 0;
    optind = 0; opterr = 0; optarg = 0; optopt = 0;
    tg[0] = 0;
    while ((c = getopt(argc, av, os)) != -1) {
        if (c == '?' || c == ':')
            n += snprintf(tg + n, sizeof(tg) - n, "[rc=%d arg=%s optopt=%d]",
                          c, optarg ? optarg : "(null)", optopt);
        else
            n += snprintf(tg + n, sizeof(tg) - n, "[rc=%d arg=%s]",
                          c, optarg ? optarg : "(null)");
    }
    n += snprintf(tg + n, sizeof(tg) - n, "|optind=%d|rest:", optind);
    for (int i = optind; i < argc; i++)
        n += snprintf(tg + n, sizeof(tg) - n, " %s", av[i]);
}

static void run_mos(int argc, char **av, const char *os) {
    int c;
    size_t n = 0;
    mos_optind = 0; mos_opterr = 0; mos_optarg = 0; mos_optopt = 0;
    tm[0] = 0;
    while ((c = mos_getopt(argc, av, os)) != -1) {
        if (c == '?' || c == ':')
            n += snprintf(tm + n, sizeof(tm) - n, "[rc=%d arg=%s optopt=%d]",
                          c, mos_optarg ? mos_optarg : "(null)", mos_optopt);
        else
            n += snprintf(tm + n, sizeof(tm) - n, "[rc=%d arg=%s]",
                          c, mos_optarg ? mos_optarg : "(null)");
    }
    n += snprintf(tm + n, sizeof(tm) - n, "|optind=%d|rest:", mos_optind);
    for (int i = mos_optind; i < argc; i++)
        n += snprintf(tm + n, sizeof(tm) - n, " %s", av[i]);
}

static int bad = 0, cases = 0;
static char store1[24][64], store2[24][64];

static void cmp(const char *os, int argc, const char **in) {
    char *a1[24], *a2[24];
    for (int i = 0; i < argc; i++) {
        strcpy(store1[i], in[i]); a1[i] = store1[i];
        strcpy(store2[i], in[i]); a2[i] = store2[i];
    }
    run_glibc(argc, a1, os);
    run_mos(argc, a2, os);
    cases++;
    if (strcmp(tg, tm) != 0) {
        bad++;
        printf("DIFF optstring=\"%s\" argv:", os);
        for (int i = 0; i < argc; i++) printf(" '%s'", in[i]);
        printf("\n  glibc    : %s\n  mayteraos: %s\n", tg, tm);
    }
}

#define C(os, ...) do { const char *v[] = { __VA_ARGS__ }; \
    cmp(os, (int)(sizeof(v) / sizeof(v[0])), v); } while (0)

int main(void) {
    // Plain short options, clustering, operands.
    C("ab", "p", "-a", "-b");
    C("ab", "p", "-ab");
    C("ab", "p", "-a", "file", "-b");
    C("ab", "p");
    C("ab", "p", "-a", "-a", "-a");
    C("abc", "p", "-a", "-bc", "f");

    // Permutation.
    C("ab", "p", "file", "-a", "bar");
    C("ab", "p", "f1", "f2", "-a", "f3", "-b");
    C("o:", "p", "f1", "-o", "X", "f2");

    // "--", the one that eats filenames when it is wrong.
    C("ab", "p", "-a", "--", "-b", "file");
    C("ab", "p", "file1", "-a", "--", "-b");
    C("ab", "p", "--", "-a");
    C("ab", "p", "--");
    C("ab", "p", "--", "--", "-a");
    C("ab", "p", "-a", "f1", "-b", "f2", "--", "-c", "f3");
    C("o:", "p", "-o", "--");

    // "-" alone is an operand.
    C("ab", "p", "-");
    C("ab", "p", "-", "-a");
    C("ab", "p", "f1", "-", "-a");

    // Errors.
    C("ab", "p", "-x");
    C("ab", "p", "-axb");
    C("o:", "p", "-o");
    C(":o:", "p", "-o");
    C(":ab", "p", "-x");
    C("o:", "p", "-a", "-o");

    // Arguments, glued and separate, including one that looks like an option.
    C("o:", "p", "-ofile");
    C("o:", "p", "-o", "file");
    C("o:", "p", "-o", "-b");
    C("o:", "p", "-o", "", "f");
    C("abo:", "p", "-abo", "VAL");
    C("abo:", "p", "-aboVAL");
    C("o:x", "p", "-xoVAL", "f");
    C("o:x", "p", "-xo", "VAL", "f");

    // Optional arguments: glued only, never the next word.
    C("a::", "p", "-a");
    C("a::", "p", "-aVAL");
    C("a::", "p", "-a", "VAL");

    // Ordering flags.
    C("+ab", "p", "file", "-a");
    C("+ab", "p", "-a", "file", "-b");
    C("-ab", "p", "file", "-a");
    C("-ab", "p", "-a", "f1", "f2");

    printf("\n%d command lines, %d differences from glibc\n", cases, bad);
    printf(bad == 0 ? "getopt_oracle_test: PASS\n" : "getopt_oracle_test: FAIL\n");
    return bad ? 1 : 0;
}
