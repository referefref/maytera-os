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

#endif // TERM_UTIL_H
