// help.h - MayteraOS help subsystem public API (task #267)
//
// libhelp provides a uniform in-memory document model for two on-disk help
// formats:
//
//   1. .MHLP  - the Maytera help format, a human-authorable Markdown-ish
//               markup (see mhlp.c for the full grammar).
//   2. .HLP   - legacy Windows 3.x / 95 WinHelp files. We parse the internal
//               B+tree file system, |SYSTEM, |TOPIC, and the phrase / LZ77
//               compression enough to recover readable topic text and titles.
//               Full RTF layout (fonts, tables, bitmaps) is NOT reproduced;
//               see hlp.c for an honest description of coverage.
//
// Both formats are decoded into the SAME help_doc_t so a single viewer renders
// them identically.
//
// DESIGN RULE: every entry point must be robust against malformed/truncated
// input. A bad help file must never crash the caller; on any error we return
// NULL (open) or an empty result, never a wild pointer.
//
// Freestanding C only. No libm, no C++. No em-dashes anywhere.

#ifndef _LIBHELP_HELP_H
#define _LIBHELP_HELP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Inline run styling (a span of text inside a block).
// ---------------------------------------------------------------------------
typedef enum {
    HELP_RUN_PLAIN = 0,
    HELP_RUN_BOLD,
    HELP_RUN_ITALIC,
    HELP_RUN_LINK_TOPIC,   // internal link; target = topic id
    HELP_RUN_LINK_EXTERN   // external link; target = url (http/https/...)
} help_run_kind_t;

typedef struct {
    help_run_kind_t kind;
    char *text;            // display text (owned)
    char *target;          // topic id or url for link runs, else NULL (owned)
} help_run_t;

// ---------------------------------------------------------------------------
// Block kinds (a topic body is an ordered list of blocks).
// ---------------------------------------------------------------------------
typedef enum {
    HELP_BLK_HEADING = 0,  // level in .heading_level (1 or 2)
    HELP_BLK_PARAGRAPH,
    HELP_BLK_LIST_ITEM,
    HELP_BLK_CODE,         // preformatted; single PLAIN run holds raw text
    HELP_BLK_IMAGE         // image reference; runs[0].target = image path/name
} help_block_kind_t;

typedef struct {
    help_block_kind_t kind;
    int heading_level;     // for HELP_BLK_HEADING: 1 or 2
    help_run_t *runs;      // owned array of inline runs
    int run_count;
} help_block_t;

// ---------------------------------------------------------------------------
// Topic.
// ---------------------------------------------------------------------------
typedef struct {
    char *id;              // stable identifier used by links (owned)
    char *title;           // display title for the TOC (owned)
    help_block_t *blocks;  // owned array of body blocks
    int block_count;
} help_topic_t;

// ---------------------------------------------------------------------------
// Document.
// ---------------------------------------------------------------------------
typedef enum {
    HELP_SRC_MHLP = 0,
    HELP_SRC_HLP
} help_source_t;

typedef struct {
    char *title;           // document title (owned)
    help_source_t source;  // which parser produced this
    help_topic_t *topics;  // owned array
    int topic_count;
} help_doc_t;

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

// Open a help file. Sniffs the magic bytes: WinHelp files begin with
// "?_\3\0" (0x3F 0x5F 0x03 0x00) or the older "LN\3\0" (0x4C 0x4E 0x03 0x00);
// anything else is treated as MHLP text. Returns NULL on any failure.
help_doc_t *help_open(const char *path);

// Parse an in-memory MHLP buffer directly (used by the host unit tests and by
// callers that already have the bytes). Never frees buf. Returns NULL on OOM.
help_doc_t *help_parse_mhlp(const char *buf, size_t len);

// Parse an in-memory WinHelp buffer directly. Returns NULL if it is not a
// recognisable WinHelp image.
help_doc_t *help_parse_hlp(const uint8_t *buf, size_t len);

// Free everything owned by the document.
void help_close(help_doc_t *doc);

// ---------------------------------------------------------------------------
// Accessors / navigation.
// ---------------------------------------------------------------------------
int               help_topic_count(const help_doc_t *doc);
const help_topic_t *help_topic_at(const help_doc_t *doc, int index);

// Find a topic by id (case-insensitive). Returns NULL if not found.
const help_topic_t *help_find_topic(const help_doc_t *doc, const char *id);
int                 help_find_topic_index(const help_doc_t *doc, const char *id);

// ---------------------------------------------------------------------------
// Full-text search. Returns a malloc'd array of topic indices whose title or
// body text contains the query (case-insensitive substring). Caller frees the
// returned array with free(). *out_count receives the number of hits. Returns
// NULL when there are no hits (and sets *out_count = 0).
// ---------------------------------------------------------------------------
int *help_search(const help_doc_t *doc, const char *query, int *out_count);

#ifdef __cplusplus
}
#endif

#endif // _LIBHELP_HELP_H
