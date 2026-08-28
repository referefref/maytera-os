// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// mtool.h - THE shared spine of the MayteraOS command-line tools
//           (#745 local 108, second batch).
//
// WHY THIS FILE EXISTS
// --------------------
// Local 103's survey found twenty apps named after a standard utility that
// implement a fraction of it and REPORT NO ERROR. Eight of them shared ONE
// defect, written out eight times: they take a single operand, act on it, and
// exit 0, so `rm a b c` deletes `a`, `mkdir x y` creates `x`, and `wc f1 f2`
// counts f1. Every one of those eight was `if (argc < 2) usage; do_it(argv[1]);`.
//
// That is not eight bugs to fix eight times. It is one missing primitive. This
// file is that primitive, plus the three other things every one of those tools
// was getting wrong in the same way:
//
//   * an OPERAND LOOP that visits every operand, keeps going after a failure,
//     and returns a non-zero status if any operand failed (mtool_each_operand);
//   * a REFUSAL that is loud and non-zero, so an option we do not implement is
//     a diagnosable error rather than a silently different answer
//     (mtool_refuse / mtool_bad_option);
//   * READING a whole stream with NO SILENT CAP. tail, tac, sort and less each
//     had their own fixed buffer (64 KB, 64 KB, 1000x256, 64 KB) and each
//     TRUNCATED silently at it. mtool_slurp_fd grows, and it FAILS rather than
//     returning a short answer;
//   * a WRITE that checks its result. MayteraOS gained SIGPIPE in build 1994
//     (#111b), so `producer | head -1` now terminates even for a producer that
//     ignores write(); before that it was the producer's error handling alone
//     that decided, and a tool which ignored it spun forever. mtool_wall is
//     still the right thing to call: exiting on a checked write is a cleaner
//     end than being killed by a signal, and it is what lets a tool tell a
//     departed consumer from a full disk.
//
// SCOPE, stated so it is not over-read: this is a helper for the small tools in
// userland/apps. It is not a general application framework, it does no option
// parsing of its own (the tools call the libc's real getopt(), which was
// differentially verified against glibc in local 72), and it holds no state
// beyond the program name.
//
// KEEP THESE FUNCTIONS IN THIS FILE. A static archive member is pulled in as a
// WHOLE OBJECT, so putting an mtool_* function next to, say, fopen would drag
// stdio_file.o into every link that wants a byte of this (blame.md, local 91:
// adding tmpfile() beside fopen broke three game ports). mtool.o defines
// nothing but mtool_*, so it is safe to link from anywhere and it costs nothing
// to an app that does not call it.
#ifndef LIBC_MTOOL_H
#define LIBC_MTOOL_H

#include "types.h"

// Exit statuses. A tool that cannot do what it was asked must not exit 0.
//   0 = did the whole job
//   1 = tried and something failed (a missing file, a write error)
//   2 = REFUSED: the command names something this tool does not implement.
// The split matters: 1 means "the answer is incomplete because of the data",
// 2 means "there is no answer because I do not implement what you typed", and
// a caller can tell them apart without parsing English.
#define MTOOL_EX_OK      0
#define MTOOL_EX_FAIL    1
#define MTOOL_EX_REFUSE  2

// --- program identity ------------------------------------------------------
// Call once from main() with argv[0]. Diagnostics are prefixed with the
// basename, exactly as every POSIX tool does. Never returns NULL: an app that
// forgets to call this gets "mtool", which is a visible bug rather than a crash.
void        mtool_setprog(const char *argv0);
const char *mtool_prog(void);

// --- diagnostics -----------------------------------------------------------
// ALL of these write to fd 2. Diagnostics on stdout are how a tool corrupts
// the pipeline it is a stage of, and three of the tools this file replaced did
// exactly that (they used printf for their error messages).
void mtool_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// warn + exit(status).
void mtool_die(int status, const char *fmt, ...)
        __attribute__((format(printf, 2, 3))) __attribute__((noreturn));

// THE refusal. `what` names the construct precisely as the user typed it; `why`
// is a short reason or NULL. Exits MTOOL_EX_REFUSE and prints NOTHING on
// stdout, so a refusal can never be mistaken for a result.
//
//     $ cut -b1 f
//     cut: -b (byte ranges) is not implemented: this build has no multibyte
//     support, so use -c
//
// A tool that says "I do not implement -b" is fine. A tool that silently
// ignores -b is the defect this whole ticket is about.
void mtool_refuse(const char *what, const char *why) __attribute__((noreturn));

// The specific refusal every hand-rolled option loop in this tree was missing:
// an unrecognised option was skipped and the tool carried on. Call this from
// the getopt '?' case.
void mtool_bad_option(const char *opt) __attribute__((noreturn));

// --- the operand loop ------------------------------------------------------
// Calls FN once per operand in argv[first..argc). FN returns 0 for success and
// non-zero for failure; a failure does NOT stop the loop (rm reports the file
// it could not remove and still removes the rest, exactly like rm(1)).
//
// Returns MTOOL_EX_OK if every call returned 0, MTOOL_EX_FAIL otherwise.
//
// If there are no operands: when `missing` is non-NULL it is the message for
// "missing operand" and the function exits MTOOL_EX_FAIL; when `missing` is
// NULL the function calls FN exactly once with "-", which is the POSIX
// "no file operands means read standard input" rule.
typedef int (*mtool_operand_fn)(const char *operand, void *ctx);

int mtool_each_operand(int argc, char **argv, int first,
                       mtool_operand_fn fn, void *ctx, const char *missing);

// --- reading ---------------------------------------------------------------
// Open an operand for reading. "-" means standard input (fd 0). Returns the fd,
// or -1 after printing a diagnostic. Relative paths are resolved against the
// process's cwd, because MayteraOS's open() takes what it is given and several
// of these tools already hand-rolled the same getcwd()+join.
int  mtool_open_read(const char *operand);
// Resolve an operand to an absolute path in `out`. Returns 0, or -1 if it
// would not fit. THE one implementation: the kernel stores a cwd (sys_chdir)
// and nothing reads it, so every tool that wanted relative paths to work has
// had to join them itself, and most simply did not.
int  mtool_resolve(const char *path, char *out, size_t outsz);
// Closes fd unless it is 0 (standard input), which must survive for the next
// operand.
void mtool_close_read(int fd);

// Read fd to end of file into a malloc'd, NUL-terminated buffer. *len_out gets
// the byte count (the NUL is not counted). Returns NULL on a read error or an
// allocation failure, having printed a diagnostic.
//
// THERE IS NO CAP. That is the entire point: the four tools this replaced each
// stopped at a fixed size and printed a confidently wrong answer for the file
// the tool exists to handle. If memory runs out this returns NULL and the tool
// FAILS; it never returns a short buffer as if it were the whole file.
char *mtool_slurp_fd(int fd, size_t *len_out);

// Offsets of the start of every line in [buf, buf+len). A trailing newline does
// NOT start an empty final line. Returns a malloc'd array of *count_out
// offsets, or NULL (count 0) for empty input or on allocation failure.
size_t *mtool_index_lines(const char *buf, size_t len, size_t *count_out);

// --- writing ---------------------------------------------------------------
// write(2) that loops over short writes and reports failure. Returns 0 on
// success, -1 if the write failed - which is how a stage learns its consumer
// has gone. Since build 1994 (#111b) a tool that IGNORES this is killed by
// SIGPIPE rather than spinning forever in `yes | head -1`, but checking is
// still better: it exits 0 instead of 141 and can distinguish a departed
// consumer from a genuine write error.
int mtool_wall(int fd, const void *buf, size_t len);

// Formatted write to fd, capped at 1 KB, same return contract as mtool_wall.
int mtool_wfmt(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// --- the obsolete "-NUM" count syntax --------------------------------------
// `head -5 f` and `tail -5 f` are the forms people actually type, and getopt
// cannot express them (a digit is not an option letter). This rewrites any
// argv word of the exact form "-<digits>" - and, when allow_plus is set,
// "+<digits>" - into the two words "-n" "<digits>", returning a malloc'd argv.
// The result is NULL-terminated. Returns the ORIGINAL argv (and leaves
// *out_argc == argc) when there is nothing to rewrite, so the caller never has
// to care which it got; nothing here is ever freed because these programs exit.
//
// It lives here rather than in head and again in tail because that is the
// duplication this whole ticket is about.
//
// argopts lists the option letters that TAKE AN ARGUMENT ("nc" for head and
// tail). A word that is the argument of one of those is never rewritten: in
// "tail -n +5" the "+5" belongs to the -n in front of it, and rewriting it
// produced "-n -n 5", which parsed as -n with the argument "-n".
char **mtool_expand_count_opts(int argc, char **argv, int *out_argc, int allow_plus,
                               const char *argopts);

// --- numeric arguments -----------------------------------------------------
// Parse a whole non-negative count (an option argument such as -n 20). On
// anything that is not a complete decimal number this REFUSES: `head -n x`
// used to atoi() its way to 0 and print nothing, which is a silent wrong
// answer with exit 0. `opt` is used in the message.
long mtool_count_arg(const char *opt, const char *s);

#endif // LIBC_MTOOL_H
