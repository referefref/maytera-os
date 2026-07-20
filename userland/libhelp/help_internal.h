// help_internal.h - shared helpers internal to libhelp.
// Not part of the public API. Freestanding C; depends only on libc string/mem
// and malloc/free/realloc.
#ifndef _LIBHELP_INTERNAL_H
#define _LIBHELP_INTERNAL_H

#include "help.h"
#include <stddef.h>
#include <stdint.h>

// Small case-insensitive helpers (host build and freestanding build both have
// strcasecmp, but we keep a substring search that is guaranteed available).
int  hlp_ci_eq(const char *a, const char *b);          // 1 if equal (ci)
const char *hlp_ci_strstr(const char *hay, const char *needle); // ci substr

// strdup that never returns a dangling pointer; on OOM returns NULL. A NULL
// input yields an empty string (so callers never deref NULL display text).
char *hlp_strdup0(const char *s);
char *hlp_strndup0(const char *s, size_t n);

// ---------------------------------------------------------------------------
// Builder helpers so both parsers create the model the same way.
// ---------------------------------------------------------------------------

// Append a new topic, returns its index or -1 on OOM.
int  hlp_doc_add_topic(help_doc_t *doc, const char *id, const char *title);

// Append a block to a topic. Returns pointer to the new (zeroed) block or NULL.
help_block_t *hlp_topic_add_block(help_topic_t *t, help_block_kind_t kind);

// Append a run to a block. text/target are copied. Returns 0 on success.
int  hlp_block_add_run(help_block_t *b, help_run_kind_t kind,
                       const char *text, const char *target);

// Convenience: a block that is a single plain run of the given text.
help_block_t *hlp_topic_add_text_block(help_topic_t *t, help_block_kind_t kind,
                                       const char *text);

// Parse a single line of MHLP inline markup (**bold** *italic* [l](#id)
// [l](http..)) into runs appended to block b. Robust to unbalanced markers.
void hlp_parse_inline(help_block_t *b, const char *line);

#endif // _LIBHELP_INTERNAL_H
