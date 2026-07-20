// test_parsers.c - HOST unit test for the libhelp parsers (task #267).
//
// Compiled with the host's native gcc (NOT freestanding). It includes the
// parser .c files directly so the parsing logic is exercised without a VM.
// It loads sample .MHLP and .HLP files, prints the parsed topic list and the
// extracted text, and runs the search API. Exits non-zero on a hard failure
// (e.g. a parser returning NULL on a valid file).
//
// Build:  make -C host_test         (see host_test/Makefile)
// Run  :  ./host_test/run_tests <file.mhlp> <file.hlp>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../help.h"

static const char *run_kind_name(help_run_kind_t k) {
    switch (k) {
        case HELP_RUN_BOLD:        return "B";
        case HELP_RUN_ITALIC:      return "I";
        case HELP_RUN_LINK_TOPIC:  return "LT";
        case HELP_RUN_LINK_EXTERN: return "LX";
        default:                   return "p";
    }
}

static const char *blk_kind_name(help_block_kind_t k) {
    switch (k) {
        case HELP_BLK_HEADING:   return "HEADING";
        case HELP_BLK_LIST_ITEM: return "LIST";
        case HELP_BLK_CODE:      return "CODE";
        case HELP_BLK_IMAGE:     return "IMAGE";
        default:                 return "PARA";
    }
}

static void dump_doc(const help_doc_t *doc, const char *label) {
    printf("\n================ %s ================\n", label);
    if (!doc) { printf("  <parse returned NULL>\n"); return; }
    printf("  title : %s\n", doc->title ? doc->title : "(null)");
    printf("  source: %s\n", doc->source == HELP_SRC_HLP ? "HLP" : "MHLP");
    printf("  topics: %d\n", help_topic_count(doc));
    for (int i = 0; i < help_topic_count(doc); i++) {
        const help_topic_t *t = help_topic_at(doc, i);
        printf("  --- topic[%d] id='%s' title='%s' (%d blocks)\n",
               i, t->id, t->title, t->block_count);
        for (int j = 0; j < t->block_count; j++) {
            const help_block_t *b = &t->blocks[j];
            printf("      [%s%s]", blk_kind_name(b->kind),
                   b->kind == HELP_BLK_HEADING ?
                       (b->heading_level == 2 ? "2" : "1") : "");
            for (int r = 0; r < b->run_count; r++) {
                const help_run_t *run = &b->runs[r];
                printf(" %s\"%s\"", run_kind_name(run->kind),
                       run->text ? run->text : "");
                if (run->target) printf("->(%s)", run->target);
            }
            printf("\n");
        }
    }
}

static void run_search(const help_doc_t *doc, const char *q) {
    int n = 0;
    int *hits = help_search(doc, q, &n);
    printf("  search '%s' -> %d hit(s):", q, n);
    for (int i = 0; i < n; i++) {
        const help_topic_t *t = help_topic_at(doc, hits[i]);
        printf(" [%d]%s", hits[i], t ? t->id : "?");
    }
    printf("\n");
    free(hits);
}

static unsigned char *slurp(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*len + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, *len, f);
    (void)got;
    b[*len] = 0;
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    int failures = 0;

    // ----- inline self-test of the MHLP markup (no file needed) -----
    const char *sample =
        "@title Inline Test\n"
        "@topic alpha Alpha Topic\n"
        "# Heading One\n"
        "This is **bold** and *italic* and a [link](#beta) plus an "
        "[external](http://example.com).\n"
        "\n"
        "- first item\n"
        "- second item\n"
        "```\n"
        "code line 1\n"
        "code line 2\n"
        "```\n"
        "@topic beta Beta Topic\n"
        "Just a paragraph in beta referencing [Alpha](#alpha).\n";
    help_doc_t *d = help_parse_mhlp(sample, strlen(sample));
    dump_doc(d, "MHLP inline self-test");
    if (!d || help_topic_count(d) != 2) {
        printf("  FAIL: expected 2 topics\n"); failures++;
    }
    if (d && !help_find_topic(d, "beta")) {
        printf("  FAIL: cannot find topic 'beta'\n"); failures++;
    }
    if (d) {
        run_search(d, "italic");
        run_search(d, "beta");
        run_search(d, "nonexistent");
    }
    help_close(d);

    // ----- malformed-input robustness -----
    const char *junk = "@topic\n**unterminated\n[broken](#\n```\nno close";
    help_doc_t *j = help_parse_mhlp(junk, strlen(junk));
    if (!j) { printf("  FAIL: malformed MHLP returned NULL\n"); failures++; }
    else { printf("\n  malformed MHLP survived: %d topic(s)\n",
                  help_topic_count(j)); help_close(j); }

    // ----- file-based tests from argv -----
    for (int a = 1; a < argc; a++) {
        long len = 0;
        unsigned char *buf = slurp(argv[a], &len);
        if (!buf) { printf("\n  cannot open %s\n", argv[a]); failures++; continue; }

        help_doc_t *doc = help_open(argv[a]);
        dump_doc(doc, argv[a]);
        if (!doc) { printf("  FAIL: help_open returned NULL for %s\n", argv[a]);
                    failures++; }
        else { run_search(doc, "the"); help_close(doc); }
        free(buf);
    }

    printf("\n==== host test complete: %d failure(s) ====\n", failures);
    return failures ? 1 : 0;
}
