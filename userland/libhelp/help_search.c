// help_search.c - case-insensitive full-text search over the doc model.
//
// Returns a malloc'd array of matching topic indices (caller frees). A topic
// matches if the query is a substring of its title, any run text, any block
// text, or its id. Empty/NULL query returns no hits.

#include "help.h"
#include "help_internal.h"

#include <stdlib.h>
#include <string.h>

static int block_matches(const help_block_t *b, const char *q) {
    for (int i = 0; i < b->run_count; i++) {
        if (b->runs[i].text && hlp_ci_strstr(b->runs[i].text, q)) return 1;
        if (b->runs[i].target && hlp_ci_strstr(b->runs[i].target, q)) return 1;
    }
    return 0;
}

static int topic_matches(const help_topic_t *t, const char *q) {
    if (t->title && hlp_ci_strstr(t->title, q)) return 1;
    if (t->id && hlp_ci_strstr(t->id, q)) return 1;
    for (int j = 0; j < t->block_count; j++)
        if (block_matches(&t->blocks[j], q)) return 1;
    return 0;
}

int *help_search(const help_doc_t *doc, const char *query, int *out_count) {
    if (out_count) *out_count = 0;
    if (!doc || !query || !*query) return NULL;

    int *hits = (int *)malloc((size_t)(doc->topic_count > 0 ? doc->topic_count : 1)
                              * sizeof(int));
    if (!hits) return NULL;

    int n = 0;
    for (int i = 0; i < doc->topic_count; i++) {
        if (topic_matches(&doc->topics[i], query)) hits[n++] = i;
    }
    if (n == 0) { free(hits); return NULL; }
    if (out_count) *out_count = n;
    return hits;
}
