// term_util.h
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.
#ifndef TERM_UTIL_H
#define TERM_UTIL_H

#include "term_common.h"

// Small string helpers the terminal has always carried locally.
void str_copy(char *dest, const char *src, int max);
int  str_eq(const char *a, const char *b);
int  str_starts(const char *str, const char *prefix);
void int_to_str(int num, char *buf);

// [tabcomp] Promoted out of term_menu.c (was static tm_begin_output()/
// tm_end_output(), used by its Help rows and bookmark runner). Erase the
// half-typed input_line back to column 0 / reprint the prompt plus whatever
// the user had typed, around a block of output written straight into the
// pane. term_menu.c keeps its own tm_begin_output()/tm_end_output() names
// as thin wrappers over these so its eight existing call sites did not need
// touching; term_shell.c's Tab-completion match listing uses these directly.
void term_begin_output(void);
void term_end_output(void);

#endif // TERM_UTIL_H
