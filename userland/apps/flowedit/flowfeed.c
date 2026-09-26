// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// flowfeed.c - network-independent logic for the Maytera Flow feed (#153).
// See flowfeed.h. Deliberately free of syscalls, GUI and crypto so it can be
// host-unit-tested (feedtest.c) with the system compiler.

#include "flowfeed.h"

// The host unit test compiles with the system libc; the freestanding build
// pulls libc string.h in via main.c's includes. Provide only what we use.
#ifdef FLOWFEED_HOSTTEST
#include <string.h>
#else
#include "string.h"
#endif

// ---------------------------------------------------------------------------
// Minimal JSON helpers. The manifest is SMALL and its shape is fixed and (by
// the time we parse) signature-verified, so a full JSON parser is unwarranted.
// This mirrors the App Store client's json_str()/brace-matching approach so the
// two feeds parse the same way. Scans only within [obj,end).
// ---------------------------------------------------------------------------
static int ff_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Extract the string value of "key" found within [obj,end) into out (cap).
// Handles the common JSON escapes. out[0]=0 if the key is absent.
static void ff_json_str(const char *obj, const char *end, const char *key,
                        char *out, int cap) {
    if (cap > 0) out[0] = 0;
    int klen = (int)strlen(key);
    for (const char *p = obj; p + klen + 2 < end; p++) {
        if (p[0] != '"') continue;
        if (strncmp(p + 1, key, (size_t)klen) != 0) continue;
        if (p[1 + klen] != '"') continue;
        const char *q = p + 1 + klen + 1;          // just past the closing key quote
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        if (q >= end || *q != ':') continue;       // not a "key": value pair
        q++;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        if (q >= end || *q != '"') return;         // not a string value
        q++;
        int o = 0;
        while (q < end && *q != '"' && o < cap - 1) {
            char c = *q++;
            if (c == '\\' && q < end) {
                char e = *q++;
                switch (e) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case '"': c = '"';  break;
                    case '\\': c = '\\'; break;
                    case '/': c = '/';  break;
                    case 'u': {
                        // Skip a \uXXXX escape; render as '?' (feed strings are
                        // plain ASCII by convention, this is just safety).
                        for (int k = 0; k < 4 && q < end; k++) q++;
                        c = '?';
                        break;
                    }
                    default: c = e; break;
                }
            }
            out[o++] = c;
        }
        out[o] = 0;
        return;
    }
}

static int ff_is_hex64(const char *s) {
    if (!s) return 0;
    for (int i = 0; i < 64; i++) {
        if (s[i] == 0) return 0;
        if (ff_hex(s[i]) < 0) return 0;
    }
    return s[64] == 0;
}

int flowfeed_parse_manifest(const char *json, int len,
                            flowfeed_entry_t *out, int max,
                            char *err, int errcap) {
    if (err && errcap > 0) err[0] = 0;
    if (!json || len <= 0 || !out || max <= 0) {
        if (err && errcap > 0) strncpy(err, "empty manifest", (size_t)errcap - 1);
        return -1;
    }
    const char *manend = json + len;

    // Find the "flows" array.
    const char *fk = NULL;
    for (const char *p = json; p + 8 < manend; p++) {
        if (p[0] == '"' && strncmp(p + 1, "flows", 5) == 0 && p[6] == '"') { fk = p; break; }
    }
    if (!fk) {
        if (err && errcap > 0) strncpy(err, "manifest has no flows[] array", (size_t)errcap - 1);
        return -1;
    }
    const char *arr = NULL;
    for (const char *p = fk; p < manend; p++) { if (*p == '[') { arr = p; break; } if (*p == '}') break; }
    if (!arr) {
        if (err && errcap > 0) strncpy(err, "manifest flows[] malformed", (size_t)errcap - 1);
        return -1;
    }

    int n = 0;
    const char *p = arr + 1;
    while (p < manend && n < max) {
        while (p < manend && *p != '{' && *p != ']') p++;
        if (p >= manend || *p == ']') break;
        const char *obj = p;
        int depth = 0;
        const char *q = p;
        while (q < manend) {
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) { q++; break; } }
            q++;
        }
        const char *oend = q;

        flowfeed_entry_t e;
        memset(&e, 0, sizeof(e));
        ff_json_str(obj, oend, "name",        e.name,   sizeof(e.name));
        ff_json_str(obj, oend, "description", e.desc,   sizeof(e.desc));
        if (e.desc[0] == 0) ff_json_str(obj, oend, "summary", e.desc, sizeof(e.desc));
        ff_json_str(obj, oend, "path",        e.path,   sizeof(e.path));
        ff_json_str(obj, oend, "sha256",      e.sha256, sizeof(e.sha256));

        // Keep ONLY entries we can name and integrity-check. An entry with no
        // 64-char sha256 cannot be verified, so it is dropped, not trusted.
        if (e.name[0] && e.path[0] && ff_is_hex64(e.sha256)) {
            out[n++] = e;
        }
        p = oend;
    }
    return n;
}

int flowfeed_sanitize_name(const char *in, char *out, int cap) {
    if (cap > 0) out[0] = 0;
    if (!in || cap < 2) return -1;
    int o = 0;
    for (const char *p = in; *p && o < cap - 1; p++) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            out[o++] = c;
        } else {
            // Fold any other character (space, slash, etc.) to '-', but never
            // let a run of them explode the length.
            if (o > 0 && out[o - 1] == '-') continue;
            out[o++] = '-';
        }
    }
    while (o > 0 && (out[o - 1] == '-' || out[o - 1] == '.')) o--;
    out[o] = 0;
    // Refuse names that could still be dangerous or meaningless.
    if (o == 0) { out[0] = 0; return -1; }
    if (out[0] == '.') { out[0] = 0; return -1; }               // no dotfiles
    if (strstr(out, "..")) { out[0] = 0; return -1; }           // no traversal
    if (strchr(out, '/')) { out[0] = 0; return -1; }            // no separators (belt+braces)
    return 0;
}
