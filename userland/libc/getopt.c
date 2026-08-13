// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// getopt.c - getopt / getopt_long / getopt_long_only for MayteraOS userland.
//
// The contract is documented in getopt.h. This file is the whole engine; there
// is no per-app copy to drift away from it.
//
// WHY THE PERMUTING FORM AND NOT THE SIMPLE ONE. A non-permuting getopt is
// twenty lines, and it silently mis-parses "prog file -v" by treating -v as an
// operand. Ported CLI tools are written against the permuting behaviour, so a
// simple version would compile, link, run, and quietly do the wrong thing on
// the most ordinary command line there is. That is the failure mode this tree
// has the most scar tissue for, so the permuting form is what is implemented.
//
// DIAGNOSTICS GO TO fd 2 WITH write(), NOT fprintf(). getopt is linked into
// tiny tools that otherwise never touch stdio; pulling FILE in through an error
// path nobody hits would be the tail wagging the dog.
#include "getopt.h"
#include "string.h"
#include "unistd.h"

char *optarg = 0;
int   optind = 1;
int   opterr = 1;
int   optopt = '?';

#define ORDER_PERMUTE         0
#define ORDER_REQUIRE_ORDER   1
#define ORDER_RETURN_IN_ORDER 2

// Where we are inside the current clustered argv element ("-abc"). NULL means
// "not part-way through an element", which is also the state a completed or
// not-yet-started scan is in, and is what makes the optind==1 restart check
// below safe: mid-cluster, optind can still be 1, but nextchar is not NULL.
static char *nextchar = 0;

// The block argv[first_nonopt .. last_nonopt) holds operands we have skipped
// past and will rotate to the end of argv.
static int first_nonopt = 1;
static int last_nonopt  = 1;

static void rev(char **v, int b, int e) {
    while (b < e - 1) { char *t = v[b]; v[b] = v[e - 1]; v[e - 1] = t; b++; e--; }
}

// Rotate argv[first_nonopt, optind) left so the options we just consumed sit
// before the operands we skipped. Three reversals, which is the whole of it.
static void exchange(char **v) {
    rev(v, first_nonopt, last_nonopt);
    rev(v, last_nonopt, optind);
    rev(v, first_nonopt, optind);
    first_nonopt += (optind - last_nonopt);
    last_nonopt   = optind;
}

// Join up to four fragments and write one line to stderr. Deliberately not
// printf: see the file header.
static void diag(const char *a, const char *b, const char *c, const char *d) {
    char buf[320];
    const char *p[4];
    unsigned n = 0, i;
    p[0] = a; p[1] = b; p[2] = c; p[3] = d;
    for (i = 0; i < 4; i++) {
        const char *s = p[i];
        if (!s) continue;
        while (*s && n < sizeof(buf) - 2) buf[n++] = *s++;
    }
    buf[n++] = '\n';
    write(2, buf, n);
}

static int getopt_internal(int argc, char *const argv[], const char *optstring,
                           const struct option *longopts, int *longindex,
                           int long_only)
{
    // Permuting means writing to argv. Every getopt since 4.3BSD does this;
    // the const is on the pointers only for source compatibility.
    char **av = (char **)argv;
    const char *prog = (argc > 0 && argv[0]) ? argv[0] : "getopt";
    int ordering = ORDER_PERMUTE;
    int colon = 0;
    int print_errors;

    optarg = 0;

    if (optstring == 0) optstring = "";
    if (*optstring == '-')      { ordering = ORDER_RETURN_IN_ORDER; optstring++; }
    else if (*optstring == '+') { ordering = ORDER_REQUIRE_ORDER;   optstring++; }
    if (*optstring == ':')      { colon = 1; optstring++; }
    print_errors = (opterr && !colon);

    if (optind == 0) optind = 1;
    // Start (or restart) of a scan. Mid-cluster this cannot fire because
    // nextchar is non-NULL there.
    if (optind <= 1 && nextchar == 0) { first_nonopt = last_nonopt = 1; }

    if (nextchar == 0 || *nextchar == '\0') {
        // Move on to the next argv element.
        if (last_nonopt  > optind) last_nonopt  = optind;
        if (first_nonopt > optind) first_nonopt = optind;

        if (ordering == ORDER_PERMUTE) {
            if (first_nonopt != last_nonopt && last_nonopt != optind) exchange(av);
            else if (last_nonopt != optind) first_nonopt = optind;

            // Skip operands, remembering the run so it can be rotated later.
            // "-" on its own is an operand, by long-standing convention.
            while (optind < argc && (av[optind][0] != '-' || av[optind][1] == '\0'))
                optind++;
            last_nonopt = optind;
        }

        // "--" ends option processing and is itself consumed.
        if (optind != argc && strcmp(av[optind], "--") == 0) {
            optind++;
            if (first_nonopt != last_nonopt && last_nonopt != optind) exchange(av);
            else if (first_nonopt == last_nonopt) first_nonopt = optind;
            last_nonopt = argc;
            optind      = argc;
        }

        if (optind == argc) {
            // Leave optind on the first operand.
            if (first_nonopt != last_nonopt) optind = first_nonopt;
            nextchar = 0;
            return -1;
        }

        if (av[optind][0] != '-' || av[optind][1] == '\0') {
            if (ordering == ORDER_REQUIRE_ORDER) { nextchar = 0; return -1; }
            // ORDER_RETURN_IN_ORDER: hand the operand back as option '\1'.
            optarg   = av[optind++];
            nextchar = 0;
            return 1;
        }

        nextchar = av[optind] + 1;
        if (longopts && av[optind][1] == '-') nextchar++;
    }

    // Long option?
    if (longopts != 0 &&
        (av[optind][1] == '-' ||
         (long_only && (av[optind][2] != '\0' || !strchr(optstring, av[optind][1]))))) {
        char *nameend;
        const struct option *p;
        const struct option *pfound = 0;
        int exact = 0, ambig = 0, indfound = 0, option_index;

        for (nameend = nextchar; *nameend && *nameend != '='; nameend++) { }

        for (p = longopts, option_index = 0; p->name; p++, option_index++) {
            if (strncmp(p->name, nextchar, (size_t)(nameend - nextchar)) == 0) {
                if ((size_t)(nameend - nextchar) == strlen(p->name)) {
                    pfound = p; indfound = option_index; exact = 1; break;
                } else if (pfound == 0) {
                    pfound = p; indfound = option_index;
                } else if (long_only || pfound->has_arg != p->has_arg ||
                           pfound->flag != p->flag || pfound->val != p->val) {
                    ambig = 1;
                }
            }
        }

        if (ambig && !exact) {
            if (print_errors) diag(prog, ": option '", av[optind], "' is ambiguous");
            nextchar += strlen(nextchar);
            optind++;
            optopt = 0;
            return '?';
        }

        if (pfound != 0) {
            option_index = indfound;
            optind++;
            if (*nameend) {
                if (pfound->has_arg != no_argument) {
                    optarg = nameend + 1;
                } else {
                    if (print_errors)
                        diag(prog, ": option '--", pfound->name, "' doesn't allow an argument");
                    nextchar += strlen(nextchar);
                    optopt = pfound->val;
                    return '?';
                }
            } else if (pfound->has_arg == required_argument) {
                if (optind < argc) {
                    optarg = av[optind++];
                } else {
                    if (print_errors)
                        diag(prog, ": option '--", pfound->name, "' requires an argument");
                    nextchar += strlen(nextchar);
                    optopt = pfound->val;
                    return colon ? ':' : '?';
                }
            }
            nextchar += strlen(nextchar);
            if (longindex) *longindex = option_index;
            if (pfound->flag) { *(pfound->flag) = pfound->val; return 0; }
            return pfound->val;
        }

        // Not a long option. Under long_only a leading single '-' may still be
        // a short option, so only bail out when it cannot be.
        if (!long_only || av[optind][1] == '-' || strchr(optstring, *nextchar) == 0) {
            if (print_errors) diag(prog, ": unrecognized option '", av[optind], "'");
            nextchar = 0;
            optind++;
            optopt = 0;
            return '?';
        }
    }

    // Short option.
    {
        char c = *nextchar++;
        char *temp = strchr(optstring, c);
        char cs[2];
        cs[0] = c; cs[1] = '\0';

        // Finished this element? Advance now, before the argument logic, so the
        // "argument is the next word" case sees the right optind.
        if (*nextchar == '\0') ++optind;

        if (temp == 0 || c == ':' || c == ';') {
            if (print_errors) diag(prog, ": invalid option -- '", cs, "'");
            optopt = c;
            if (*nextchar == '\0') nextchar = 0;
            return '?';
        }

        if (temp[1] == ':') {
            if (temp[2] == ':') {
                // Optional argument: glued on only, never the next word.
                if (*nextchar != '\0') { optarg = nextchar; optind++; }
                else optarg = 0;
                nextchar = 0;
            } else {
                // Required argument.
                if (*nextchar != '\0') { optarg = nextchar; optind++; }
                else if (optind == argc) {
                    if (print_errors)
                        diag(prog, ": option requires an argument -- '", cs, "'");
                    optopt = c;
                    c = colon ? ':' : '?';
                } else {
                    optarg = av[optind++];
                }
                nextchar = 0;
            }
        } else if (*nextchar == '\0') {
            nextchar = 0;
        }
        return c;
    }
}

int getopt(int argc, char *const argv[], const char *optstring) {
    return getopt_internal(argc, argv, optstring, 0, 0, 0);
}

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex) {
    return getopt_internal(argc, argv, optstring, longopts, longindex, 0);
}

int getopt_long_only(int argc, char *const argv[], const char *optstring,
                     const struct option *longopts, int *longindex) {
    return getopt_internal(argc, argv, optstring, longopts, longindex, 1);
}
