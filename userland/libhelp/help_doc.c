// help_doc.c - in-memory document model builder, inline parser, accessors,
// and teardown shared by both the MHLP and HLP front ends.
//
// Freestanding-safe: uses only malloc/free/realloc and string/mem from libc.
// Robust against malformed input: every allocation is checked, NULL inputs are
// tolerated, no unbounded recursion.

#include "help.h"
#include "help_internal.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------
static int lc(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

int hlp_ci_eq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *b) {
        if (lc((unsigned char)*a) != lc((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

const char *hlp_ci_strstr(const char *hay, const char *needle) {
    if (!hay || !needle) return NULL;
    if (!*needle) return hay;
    for (; *hay; hay++) {
        const char *h = hay;
        const char *n = needle;
        while (*h && *n && lc((unsigned char)*h) == lc((unsigned char)*n)) {
            h++; n++;
        }
        if (!*n) return hay;
    }
    return NULL;
}

char *hlp_strdup0(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

char *hlp_strndup0(const char *s, size_t n) {
    if (!s) { s = ""; n = 0; }
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

// ---------------------------------------------------------------------------
// Builders. We track capacities out-of-band in parallel static maps would be
// ugly; instead we over-allocate exactly via realloc each append. To keep the
// public struct clean (no cap fields), we round capacity from count using the
// "power-of-two >= count+1" rule so realloc is cheap and amortised.
// ---------------------------------------------------------------------------
static int cap_for(int count) {
    int c = 4;
    while (c < count + 1) c <<= 1;
    return c;
}

int hlp_doc_add_topic(help_doc_t *doc, const char *id, const char *title) {
    if (!doc) return -1;
    int newcap = cap_for(doc->topic_count);
    int oldcap = (doc->topic_count == 0) ? 0 : cap_for(doc->topic_count - 1);
    if (doc->topic_count == 0 || newcap != oldcap) {
        help_topic_t *nt = (help_topic_t *)realloc(
            doc->topics, (size_t)newcap * sizeof(help_topic_t));
        if (!nt) return -1;
        doc->topics = nt;
    }
    help_topic_t *t = &doc->topics[doc->topic_count];
    memset(t, 0, sizeof(*t));
    t->id = hlp_strdup0(id && *id ? id : "topic");
    t->title = hlp_strdup0(title && *title ? title : (id ? id : "Untitled"));
    if (!t->id || !t->title) { free(t->id); free(t->title); return -1; }
    return doc->topic_count++;
}

help_block_t *hlp_topic_add_block(help_topic_t *t, help_block_kind_t kind) {
    if (!t) return NULL;
    int newcap = cap_for(t->block_count);
    int oldcap = (t->block_count == 0) ? 0 : cap_for(t->block_count - 1);
    if (t->block_count == 0 || newcap != oldcap) {
        help_block_t *nb = (help_block_t *)realloc(
            t->blocks, (size_t)newcap * sizeof(help_block_t));
        if (!nb) return NULL;
        t->blocks = nb;
    }
    help_block_t *b = &t->blocks[t->block_count];
    memset(b, 0, sizeof(*b));
    b->kind = kind;
    b->heading_level = 1;
    t->block_count++;
    return b;
}

int hlp_block_add_run(help_block_t *b, help_run_kind_t kind,
                      const char *text, const char *target) {
    if (!b) return -1;
    int newcap = cap_for(b->run_count);
    int oldcap = (b->run_count == 0) ? 0 : cap_for(b->run_count - 1);
    if (b->run_count == 0 || newcap != oldcap) {
        help_run_t *nr = (help_run_t *)realloc(
            b->runs, (size_t)newcap * sizeof(help_run_t));
        if (!nr) return -1;
        b->runs = nr;
    }
    help_run_t *r = &b->runs[b->run_count];
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    r->text = hlp_strdup0(text);
    r->target = target ? hlp_strdup0(target) : NULL;
    if (!r->text || (target && !r->target)) {
        free(r->text); free(r->target);
        return -1;
    }
    b->run_count++;
    return 0;
}

help_block_t *hlp_topic_add_text_block(help_topic_t *t, help_block_kind_t kind,
                                       const char *text) {
    help_block_t *b = hlp_topic_add_block(t, kind);
    if (!b) return NULL;
    hlp_block_add_run(b, HELP_RUN_PLAIN, text ? text : "", NULL);
    return b;
}

// ---------------------------------------------------------------------------
// Inline markup parser.
//   **bold**  *italic*  [label](#topic-id)  [label](http://...)
// Anything malformed degrades to plain text; we never read past the NUL.
// ---------------------------------------------------------------------------
static void flush_plain(help_block_t *b, const char *start, const char *end) {
    if (end <= start) return;
    char *tmp = hlp_strndup0(start, (size_t)(end - start));
    if (tmp) {
        hlp_block_add_run(b, HELP_RUN_PLAIN, tmp, NULL);
        free(tmp);
    }
}

void hlp_parse_inline(help_block_t *b, const char *line) {
    if (!b || !line) return;
    const char *p = line;
    const char *plain = line;

    while (*p) {
        // Bold: **...**
        if (p[0] == '*' && p[1] == '*') {
            const char *close = strstr(p + 2, "**");
            if (close) {
                flush_plain(b, plain, p);
                char *t = hlp_strndup0(p + 2, (size_t)(close - (p + 2)));
                if (t) { hlp_block_add_run(b, HELP_RUN_BOLD, t, NULL); free(t); }
                p = close + 2;
                plain = p;
                continue;
            }
        }
        // Italic: *...* (single star, not part of **)
        if (p[0] == '*' && p[1] != '*') {
            const char *q = p + 1;
            const char *close = NULL;
            while (*q) {
                if (*q == '*') { close = q; break; }
                q++;
            }
            if (close && close > p + 1) {
                flush_plain(b, plain, p);
                char *t = hlp_strndup0(p + 1, (size_t)(close - (p + 1)));
                if (t) { hlp_block_add_run(b, HELP_RUN_ITALIC, t, NULL); free(t); }
                p = close + 1;
                plain = p;
                continue;
            }
        }
        // Link: [label](target)
        if (p[0] == '[') {
            const char *lbl_end = strchr(p + 1, ']');
            if (lbl_end && lbl_end[1] == '(') {
                const char *tgt_end = strchr(lbl_end + 2, ')');
                if (tgt_end) {
                    flush_plain(b, plain, p);
                    char *label = hlp_strndup0(p + 1, (size_t)(lbl_end - (p + 1)));
                    char *target = hlp_strndup0(lbl_end + 2,
                                                (size_t)(tgt_end - (lbl_end + 2)));
                    if (label && target) {
                        if (target[0] == '#') {
                            hlp_block_add_run(b, HELP_RUN_LINK_TOPIC, label,
                                              target + 1);
                        } else {
                            hlp_block_add_run(b, HELP_RUN_LINK_EXTERN, label,
                                              target);
                        }
                    }
                    free(label); free(target);
                    p = tgt_end + 1;
                    plain = p;
                    continue;
                }
            }
        }
        p++;
    }
    flush_plain(b, plain, p);
    // Guarantee at least one run so renderers never see an empty block.
    if (b->run_count == 0) hlp_block_add_run(b, HELP_RUN_PLAIN, "", NULL);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
int help_topic_count(const help_doc_t *doc) {
    return doc ? doc->topic_count : 0;
}

const help_topic_t *help_topic_at(const help_doc_t *doc, int index) {
    if (!doc || index < 0 || index >= doc->topic_count) return NULL;
    return &doc->topics[index];
}

int help_find_topic_index(const help_doc_t *doc, const char *id) {
    if (!doc || !id) return -1;
    for (int i = 0; i < doc->topic_count; i++) {
        if (hlp_ci_eq(doc->topics[i].id, id)) return i;
    }
    return -1;
}

const help_topic_t *help_find_topic(const help_doc_t *doc, const char *id) {
    int i = help_find_topic_index(doc, id);
    return (i < 0) ? NULL : &doc->topics[i];
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------
static void free_block(help_block_t *b) {
    for (int i = 0; i < b->run_count; i++) {
        free(b->runs[i].text);
        free(b->runs[i].target);
    }
    free(b->runs);
}

void help_close(help_doc_t *doc) {
    if (!doc) return;
    for (int i = 0; i < doc->topic_count; i++) {
        help_topic_t *t = &doc->topics[i];
        for (int j = 0; j < t->block_count; j++) free_block(&t->blocks[j]);
        free(t->blocks);
        free(t->id);
        free(t->title);
    }
    free(doc->topics);
    free(doc->title);
    free(doc);
}
