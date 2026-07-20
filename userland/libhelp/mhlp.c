// mhlp.c - parser for the Maytera help format (.MHLP).
//
// GRAMMAR (line-oriented, UTF-8/ASCII text; tolerant of CR/LF or LF):
//
//   @title <document title>          once, anywhere before/at top; optional.
//   @topic <id> <Display Title...>   begins a new topic. <id> is a single
//                                    whitespace-delimited token; the rest of
//                                    the line is the display title. Lines that
//                                    appear before the first @topic become an
//                                    implicit "intro" topic.
//
//   Inside a topic, body lines use a Markdown-ish syntax:
//     # text            -> level-1 heading
//     ## text           -> level-2 heading
//     - text            -> list item
//     ```               -> toggles a fenced code block (verbatim until next ```)
//     ![alt](path)      -> image reference (a line by itself)
//     <blank line>      -> paragraph separator (consecutive non-blank, non-
//                          special lines are joined into one paragraph)
//     anything else     -> paragraph text
//
//   Inline markup inside headings/paragraphs/list items:
//     **bold**  *italic*  [label](#topic-id)  [label](http://...)
//
//   Lines beginning with ';' at column 0 are comments (ignored), so authors can
//   annotate files. A literal leading ';' in body text can be escaped as "\;".
//
// ROBUSTNESS: malformed input never crashes. Unterminated code fences are
// closed at EOF. Missing ids/titles get safe defaults. No recursion.

#include "help.h"
#include "help_internal.h"

#include <stdlib.h>
#include <string.h>

// Trim trailing CR and spaces; returns pointer into a freshly malloc'd copy of
// one line [start,end). Caller frees.
static char *dup_line(const char *start, const char *end) {
    while (end > start && (end[-1] == '\r' || end[-1] == '\n')) end--;
    return hlp_strndup0(start, (size_t)(end - start));
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

// A pending paragraph accumulator so consecutive plain lines join with spaces.
typedef struct {
    char *buf;
    size_t len, cap;
} para_t;

static void para_reset(para_t *p) { p->len = 0; if (p->buf) p->buf[0] = 0; }

static void para_add(para_t *p, const char *s) {
    size_t add = strlen(s);
    size_t need = p->len + add + 2;
    if (need > p->cap) {
        size_t ncap = p->cap ? p->cap : 64;
        while (ncap < need) ncap <<= 1;
        char *nb = (char *)realloc(p->buf, ncap);
        if (!nb) return;
        p->buf = nb; p->cap = ncap;
    }
    if (p->len > 0) p->buf[p->len++] = ' ';
    memcpy(p->buf + p->len, s, add);
    p->len += add;
    p->buf[p->len] = 0;
}

static void para_flush(para_t *p, help_topic_t *t) {
    if (t && p->len > 0) {
        help_block_t *b = hlp_topic_add_block(t, HELP_BLK_PARAGRAPH);
        if (b) hlp_parse_inline(b, p->buf);
    }
    para_reset(p);
}

help_doc_t *help_parse_mhlp(const char *buf, size_t len) {
    if (!buf) return NULL;
    help_doc_t *doc = (help_doc_t *)calloc(1, sizeof(help_doc_t));
    if (!doc) return NULL;
    doc->source = HELP_SRC_MHLP;
    doc->title = hlp_strdup0("Help");

    int cur = -1;             // current topic index
    int in_code = 0;          // inside a ``` fence
    help_block_t *code_blk = NULL;
    para_t para = {0};

    const char *p = buf;
    const char *end = buf + len;

    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        char *line = dup_line(p, nl);
        p = (nl < end) ? nl + 1 : end;
        if (!line) continue;

        // Inside a code fence: only a lone ``` closes it; everything else is
        // appended verbatim (including blank lines).
        if (in_code) {
            const char *t = skip_ws(line);
            if (t[0] == '`' && t[1] == '`' && t[2] == '`') {
                in_code = 0;
                code_blk = NULL;
            } else if (code_blk) {
                // append a raw line to the code block's single run
                help_run_t *r = &code_blk->runs[0];
                size_t ol = strlen(r->text);
                size_t al = strlen(line);
                char *nb = (char *)realloc(r->text, ol + al + 2);
                if (nb) {
                    r->text = nb;
                    if (ol) nb[ol++] = '\n';
                    memcpy(nb + ol, line, al + 1);
                }
            }
            free(line);
            continue;
        }

        // Comment line
        if (line[0] == ';') { free(line); continue; }

        // Directives
        if (line[0] == '@') {
            // Paragraph break before a directive
            if (cur >= 0) para_flush(&para, &doc->topics[cur]);
            if (strncmp(line, "@title", 6) == 0 &&
                (line[6] == ' ' || line[6] == '\t' || line[6] == 0)) {
                const char *v = skip_ws(line + 6);
                if (*v) { free(doc->title); doc->title = hlp_strdup0(v); }
            } else if (strncmp(line, "@topic", 6) == 0 &&
                       (line[6] == ' ' || line[6] == '\t' || line[6] == 0)) {
                const char *v = skip_ws(line + 6);
                // id = first token
                const char *id_start = v;
                while (*v && *v != ' ' && *v != '\t') v++;
                char *id = hlp_strndup0(id_start, (size_t)(v - id_start));
                const char *title = skip_ws(v);
                cur = hlp_doc_add_topic(doc, id ? id : "topic",
                                        *title ? title : (id ? id : "Topic"));
                free(id);
            }
            // unknown @directive: ignored
            free(line);
            continue;
        }

        const char *body = line;

        // Blank line before any topic is just whitespace: ignore it so we do
        // not create a spurious empty "intro" topic.
        if (cur < 0 && *skip_ws(body) == 0) { free(line); continue; }

        // Body lines belong to a topic. If none yet, start an implicit one.
        if (cur < 0) {
            cur = hlp_doc_add_topic(doc, "intro", "Introduction");
            if (cur < 0) { free(line); break; }
        }
        help_topic_t *t = &doc->topics[cur];

        // Blank line -> paragraph break
        if (*skip_ws(body) == 0) {
            para_flush(&para, t);
            free(line);
            continue;
        }

        // Code fence open
        {
            const char *s = skip_ws(body);
            if (s[0] == '`' && s[1] == '`' && s[2] == '`') {
                para_flush(&para, t);
                code_blk = hlp_topic_add_block(t, HELP_BLK_CODE);
                if (code_blk) hlp_block_add_run(code_blk, HELP_RUN_PLAIN, "", NULL);
                in_code = 1;
                free(line);
                continue;
            }
        }

        // Image: ![alt](path)
        if (body[0] == '!' && body[1] == '[') {
            const char *lbl_end = strchr(body + 2, ']');
            if (lbl_end && lbl_end[1] == '(') {
                const char *tgt_end = strchr(lbl_end + 2, ')');
                if (tgt_end) {
                    para_flush(&para, t);
                    char *alt = hlp_strndup0(body + 2, (size_t)(lbl_end - (body + 2)));
                    char *path = hlp_strndup0(lbl_end + 2,
                                              (size_t)(tgt_end - (lbl_end + 2)));
                    help_block_t *b = hlp_topic_add_block(t, HELP_BLK_IMAGE);
                    if (b) hlp_block_add_run(b, HELP_RUN_PLAIN,
                                             alt ? alt : "", path ? path : "");
                    free(alt); free(path);
                    free(line);
                    continue;
                }
            }
        }

        // Heading
        if (body[0] == '#') {
            int lvl = 1;
            const char *h = body + 1;
            if (*h == '#') { lvl = 2; h++; }
            while (*h == '#') h++;
            h = skip_ws(h);
            para_flush(&para, t);
            help_block_t *b = hlp_topic_add_block(t, HELP_BLK_HEADING);
            if (b) { b->heading_level = lvl; hlp_parse_inline(b, h); }
            free(line);
            continue;
        }

        // List item
        if (body[0] == '-' && (body[1] == ' ' || body[1] == '\t')) {
            para_flush(&para, t);
            help_block_t *b = hlp_topic_add_block(t, HELP_BLK_LIST_ITEM);
            if (b) hlp_parse_inline(b, skip_ws(body + 1));
            free(line);
            continue;
        }

        // Escaped leading semicolon "\;" -> literal ';'
        if (body[0] == '\\' && body[1] == ';') body += 1;

        // Plain paragraph text: accumulate
        para_add(&para, body);
        free(line);
    }

    if (cur >= 0) para_flush(&para, &doc->topics[cur]);
    free(para.buf);

    // A document with zero topics is still valid (shows empty TOC); but make
    // sure there is at least one so the viewer has something to show.
    if (doc->topic_count == 0) {
        int i = hlp_doc_add_topic(doc, "intro", doc->title);
        if (i >= 0)
            hlp_topic_add_text_block(&doc->topics[i], HELP_BLK_PARAGRAPH,
                                     "This help file is empty.");
    }
    return doc;
}
