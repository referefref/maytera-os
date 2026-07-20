// help_open.c - file loading and format dispatch for libhelp.
//
// Reads the whole help file into memory (help files are small), sniffs the
// WinHelp magic, and routes to the matching parser. Uses libc fopen/fread on
// the target and the host alike.

#include "help.h"
#include "help_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Declared in hlp.c
int help_hlp_sniff(const uint8_t *buf, size_t len);

// Read an entire file into a malloc'd buffer. *out_len gets the byte count.
// Returns NULL on any error. The buffer is NUL-terminated (one extra byte) so
// the MHLP text parser can treat it as a C string if it wants.
static uint8_t *read_all(const char *path, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    size_t cap = 8192, len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap + 1);
    if (!buf) { fclose(f); return NULL; }

    for (;;) {
        if (len + 4096 + 1 > cap) {
            size_t ncap = cap * 2;
            uint8_t *nb = (uint8_t *)realloc(buf, ncap + 1);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb; cap = ncap;
        }
        size_t got = fread(buf + len, 1, 4096, f);
        len += got;
        if (got < 4096) break;     // EOF or short read
    }
    fclose(f);
    buf[len] = 0;
    if (out_len) *out_len = len;
    return buf;
}

help_doc_t *help_open(const char *path) {
    size_t len = 0;
    uint8_t *buf = read_all(path, &len);
    if (!buf) return NULL;

    help_doc_t *doc;
    if (help_hlp_sniff(buf, len)) {
        doc = help_parse_hlp(buf, len);
    } else {
        doc = help_parse_mhlp((const char *)buf, len);
    }
    free(buf);
    return doc;
}
