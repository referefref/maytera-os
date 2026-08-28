// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// getopt.h - POSIX/GNU command line option parsing for MayteraOS userland.
//
// Backed by a real implementation in getopt.c. Everything declared here works;
// nothing here is a stub. The engine is the classic permuting one:
//
//   * short options, with and without arguments, clustered ("-abc") and with
//     the argument glued on ("-ofile") or in the next word ("-o file"),
//   * optional short arguments ("a::"), which are ONLY taken glued on, never
//     from the next word, exactly as POSIX/GNU specify,
//   * "--" ends option processing and is REMOVED from the permuted argv; the
//     word after it is a plain operand even if it starts with '-',
//   * "-" alone is an operand, not an option,
//   * long options with "--name=value" and "--name value", unambiguous
//     abbreviation, and the flag/val indirection,
//   * a leading '+' in optstring means "stop at the first operand" (POSIX
//     order), a leading '-' means "return operands as option '\1'",
//   * a leading ':' means "report a missing argument as ':' instead of '?'
//     and print nothing".
//
// By default operands are PERMUTED to the end of argv, so "prog file -v" sees
// -v, which is what nearly every ported tool expects. POSIXLY_CORRECT is not
// consulted. That used to be because MayteraOS had no environment at all;
// since #112 it has one, so this is now a plain design choice: permuting is
// what the ported tools in this tree expect, and one env var quietly changing
// argument parsing for all of them is a worse default than none.
// Use a leading '+' in optstring if you need strict POSIX ordering.
//
// RESTARTING A SCAN: set optind to 0 or 1 before the next call. That is the
// only supported reset; there is no optreset here because there is no BSD
// source in this tree that wants one.
//
// argv is declared "char *const argv[]" for source compatibility, and the
// permuting mode writes to it (it reorders the pointers, it does not touch the
// strings). That is what every getopt implementation since 4.3BSD has done.
#ifndef LIBC_GETOPT_H
#define LIBC_GETOPT_H

// The argument of the option just returned, or NULL. For an optional argument
// that was not supplied this is NULL, which is how a caller tells "-a" from
// "-aVALUE".
extern char *optarg;

// Index of the next argv element to process. After getopt() returns -1 this is
// the index of the first operand.
extern int optind;

// Non-zero (the default) means print a diagnostic on stderr for a bad option.
// Set to 0, or put ':' first in optstring, to silence it.
extern int opterr;

// The option character that caused the last '?' or ':' return. For an
// unrecognized LONG option this is 0, because a long option has no character.
extern int optopt;

int getopt(int argc, char *const argv[], const char *optstring);

// has_arg values for struct option.
#define no_argument       0
#define required_argument 1
#define optional_argument 2

struct option {
    const char *name;   // long option name, without the leading "--"
    int         has_arg;
    int        *flag;   // if non-NULL, *flag = val and getopt_long returns 0
    int         val;    // value to return (or to store through flag)
};

// longindex, if non-NULL, receives the index into longopts of the option that
// matched. It is only written when a LONG option matched.
int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex);

// As getopt_long, but a single '-' also introduces a long option, so "-name"
// works. A single '-' followed by a character that IS in optstring is still a
// short option, so the two can coexist.
int getopt_long_only(int argc, char *const argv[], const char *optstring,
                     const struct option *longopts, int *longindex);

#endif // LIBC_GETOPT_H
