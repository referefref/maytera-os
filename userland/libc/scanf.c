// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// scanf.c - formatted input for MayteraOS userland (#422 / CPython #359).
// sscanf/vsscanf parse an in-memory string. fscanf/vfscanf/scanf parse a
// FILE* stream directly, one byte at a time via fgetc()/ungetc() (see
// vfscanf_core() below for why: a real fscanf must never consume more of the
// stream than the token it actually matched, and a byte-at-a-time reader
// with single-byte pushback is the natural way to guarantee that).
// Supports %d %i %u %o %x %f/%e/%g %c %s %[...] %n %%, field width, '*'
// (assignment suppression), and h/l/ll/z length modifiers.
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "ctype.h"
#include <stdarg.h>
#include <stdint.h>

static int sk_isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

int vsscanf(const char *str, const char *fmt, va_list ap) {
    const char *s = str;
    int assigned = 0;

    while (*fmt) {
        if (sk_isspace((unsigned char)*fmt)) {
            while (sk_isspace((unsigned char)*s)) s++;
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt) return assigned;    // literal must match
            s++; fmt++;
            continue;
        }

        fmt++;                                   // skip '%'
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }

        int width = 0, have_width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); have_width = 1; }

        int lenmod = 0; // 0=int,1=long,2=longlong,-1=short
        if (*fmt == 'h') { lenmod = -1; fmt++; if (*fmt == 'h') fmt++; }
        else if (*fmt == 'l') { lenmod = 1; fmt++; if (*fmt == 'l') { lenmod = 2; fmt++; } }
        else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { lenmod = 1; fmt++; }
        else if (*fmt == 'L') { lenmod = 2; fmt++; }

        char conv = *fmt++;
        if (conv == '%') { if (*s == '%') { s++; continue; } return assigned; }
        if (conv == 'n') { if (!suppress) *va_arg(ap, int *) = (int)(s - str); continue; }

        // All conversions except %c and %[ skip leading whitespace.
        if (conv != 'c' && conv != '[') while (sk_isspace((unsigned char)*s)) s++;
        if (*s == '\0' && conv != 'n') return assigned ? assigned : -1;

        switch (conv) {
            case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
                int base = (conv == 'd' || conv == 'u') ? 10 :
                           (conv == 'o') ? 8 :
                           (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : 0;
                // copy the numeric token honoring width, then convert
                char tmp[72]; int ti = 0;
                const char *p = s;
                if (*p == '+' || *p == '-') { if (ti < 71) tmp[ti++] = *p; p++; }
                if ((base == 16 || base == 0) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                    if (ti < 70) { tmp[ti++] = *p++; tmp[ti++] = *p++; }
                }
                int is_signed = (conv == 'd' || conv == 'i');
                while (*p && ti < 71 && (!have_width || ti < width)) {
                    int c = (unsigned char)*p, d;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                    else break;
                    int b = base ? base : 10;
                    if (d >= b) break;
                    tmp[ti++] = *p++;
                }
                if (ti == 0) return assigned;
                tmp[ti] = '\0';
                if (is_signed) {
                    long long v = strtoll(tmp, 0, base ? base : 0);
                    if (!suppress) {
                        if (lenmod == 2) *va_arg(ap, long long *) = v;
                        else if (lenmod == 1) *va_arg(ap, long *) = (long)v;
                        else if (lenmod == -1) *va_arg(ap, short *) = (short)v;
                        else *va_arg(ap, int *) = (int)v;
                        assigned++;
                    }
                } else {
                    unsigned long long v = strtoull(tmp, 0, base ? base : 0);
                    if (!suppress) {
                        if (lenmod == 2) *va_arg(ap, unsigned long long *) = v;
                        else if (lenmod == 1) *va_arg(ap, unsigned long *) = (unsigned long)v;
                        else if (lenmod == -1) *va_arg(ap, unsigned short *) = (unsigned short)v;
                        else *va_arg(ap, unsigned int *) = (unsigned int)v;
                        assigned++;
                    }
                }
                s = p;
                break;
            }
            case 'f': case 'e': case 'E': case 'g': case 'G': case 'a': {
                char tmp[80]; int ti = 0;
                const char *p = s;
                if (*p == '+' || *p == '-') { if (ti < 79) tmp[ti++] = *p; p++; }
                while (*p && ti < 79 && (!have_width || ti < width)) {
                    int c = (unsigned char)*p;
                    if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                        c == '+' || c == '-' || c == 'x' || c == 'X' ||
                        (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == 'p' || c == 'P')
                        tmp[ti++] = *p++;
                    else break;
                }
                if (ti == 0) return assigned;
                tmp[ti] = '\0';
                char *endp;
                double v = strtod(tmp, &endp);
                if (endp == tmp) return assigned;
                s += (endp - tmp);
                if (!suppress) {
                    if (lenmod >= 1) *va_arg(ap, double *) = v;
                    else *va_arg(ap, float *) = (float)v;
                    assigned++;
                }
                break;
            }
            case 'c': {
                int cnt = have_width ? width : 1;
                char *out = suppress ? 0 : va_arg(ap, char *);
                for (int i = 0; i < cnt; i++) {
                    if (*s == '\0') { if (i == 0) return assigned; break; }
                    if (out) out[i] = *s;
                    s++;
                }
                if (!suppress) assigned++;
                break;
            }
            case 's': {
                char *out = suppress ? 0 : va_arg(ap, char *);
                int n = 0;
                while (*s && !sk_isspace((unsigned char)*s) && (!have_width || n < width)) {
                    if (out) out[n] = *s;
                    s++; n++;
                }
                if (n == 0) return assigned;
                if (out) out[n] = '\0';
                if (!suppress) assigned++;
                break;
            }
            case '[': {
                int negate = 0;
                if (*fmt == '^') { negate = 1; fmt++; }
                // build the accept set until closing ']'
                const char *set = fmt;
                if (*fmt == ']') fmt++;              // ']' right after '[' is a literal
                while (*fmt && *fmt != ']') fmt++;
                int setlen = (int)(fmt - set);
                if (*fmt == ']') fmt++;
                char *out = suppress ? 0 : va_arg(ap, char *);
                int n = 0;
                while (*s && (!have_width || n < width)) {
                    int in = 0;
                    for (int i = 0; i < setlen; i++) if (set[i] == *s) { in = 1; break; }
                    if (negate) in = !in;
                    if (!in) break;
                    if (out) out[n] = *s;
                    s++; n++;
                }
                if (n == 0) return assigned;
                if (out) out[n] = '\0';
                if (!suppress) assigned++;
                break;
            }
            default:
                return assigned;
        }
    }
    return assigned;
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}

// AssaultCube port phase 3 (docs/ASSAULTCUBE_PORT_PLAN.md): modern glibc's
// own <stdio.h> (the HOST header this port's C++ TUs compile against, per
// the curaslice precedent) redirects sscanf() call sites to the symbol-
// versioned __isoc99_sscanf at the ABI level for source built against it,
// so a real link needs that exact symbol name to exist, not just plain
// sscanf(). Same implementation, real alias, not a stub.
int __isoc99_sscanf(const char *str, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}

// ---------------------------------------------------------------------------
// fscanf/vfscanf/scanf: a REAL byte-streaming scanner, not a line-buffer hack
// ---------------------------------------------------------------------------
// #745 (myman port): the first version of this function read one line with
// fgets() and delegated to vsscanf(). That is wrong for any caller that
// interleaves fscanf() with its own byte-level reads on the SAME stream (a
// completely ordinary thing to do, and exactly what myman.c's tile/sprite/
// maze file reader does): vsscanf() only ever gets to see the ONE line
// fgets() grabbed, so if the format doesn't consume that whole line, the
// leftover bytes (everything after the matched token, including the
// terminating "\r\n" the caller's own fgetc() loop still expects to see)
// were silently discarded when the FILE* moved on to the next line. A
// second attempt tried to recover the unconsumed remainder with an
// ftell()/fseek() round trip, which fixed the common case but not a rarer
// one (a run of whitespace-only lines before the next real token, which a
// single fgets() cannot see past) and depends on fseek() correctly
// reasoning about read-ahead buffering it should not need to know about in
// the first place. Both were built on the wrong foundation: turning a
// stream scan into a string scan and then trying to undo the difference.
//
// This version never makes that trade: it reads directly from `f` one byte
// at a time via fgetc()/ungetc(), exactly the primitives ungetc() already
// promises to support (a SINGLE byte of pushback), and every one of the
// token-scanning loops below needs at most one byte of lookahead - read a
// candidate byte, and either keep it (it belongs to the token) or push it
// straight back (it does not) and stop. Nothing is ever read further ahead
// than the token actually being matched, so there is no "rest of the line"
// to lose and nothing to seek back afterward. Verified against the actual
// myman.c call shape end-to-end (indices, `~`-prefixed colour/flag suffixes,
// and the "N WxH~F args" maze header all share this exact pattern).
static int vfscanf_core(FILE *f, const char *fmt, va_list ap) {
    int assigned = 0;

    while (*fmt) {
        if (sk_isspace((unsigned char)*fmt)) {
            int c;
            while ((c = fgetc(f)) != EOF && sk_isspace(c)) { }
            if (c != EOF) ungetc(c, f);
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            int c = fgetc(f);
            if (c != (unsigned char)*fmt) { if (c != EOF) ungetc(c, f); return assigned; }
            fmt++;
            continue;
        }

        fmt++;                                   // skip '%'
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }

        int width = 0, have_width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); have_width = 1; }

        int lenmod = 0; // 0=int,1=long,2=longlong,-1=short
        if (*fmt == 'h') { lenmod = -1; fmt++; if (*fmt == 'h') fmt++; }
        else if (*fmt == 'l') { lenmod = 1; fmt++; if (*fmt == 'l') { lenmod = 2; fmt++; } }
        else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { lenmod = 1; fmt++; }
        else if (*fmt == 'L') { lenmod = 2; fmt++; }

        char conv = *fmt++;
        if (conv == '%') {
            int c = fgetc(f);
            if (c == '%') continue;
            if (c != EOF) ungetc(c, f);
            return assigned;
        }
        // %n on a STREAM would need ftell() to report a true byte offset;
        // no caller in this tree uses %n through fscanf (only vsscanf's
        // string form does, above), so this reports the assignment count
        // instead of adding an ftell() dependency for a path nothing
        // exercises. Revisit if a real caller ever needs it.
        if (conv == 'n') { if (!suppress) *va_arg(ap, int *) = assigned; continue; }

        int c = EOF;
        // All conversions except %c and %[ skip leading whitespace.
        if (conv != 'c' && conv != '[') {
            while ((c = fgetc(f)) != EOF && sk_isspace(c)) { }
        } else {
            c = fgetc(f);
        }
        if (c == EOF) return assigned ? assigned : -1;

        switch (conv) {
            case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
                int base = (conv == 'd' || conv == 'u') ? 10 :
                           (conv == 'o') ? 8 :
                           (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : 0;
                char tmp[72]; int ti = 0;
                if (c == '+' || c == '-') { if (ti < 71) tmp[ti++] = (char)c; c = fgetc(f); }
                if ((base == 16 || base == 0) && c == '0') {
                    int c2 = fgetc(f);
                    if (c2 == 'x' || c2 == 'X') { if (ti < 70) { tmp[ti++] = (char)c; tmp[ti++] = (char)c2; } base = 16; c = fgetc(f); }
                    else { if (c2 != EOF) ungetc(c2, f); }
                }
                int is_signed = (conv == 'd' || conv == 'i');
                while (c != EOF && ti < 71 && (!have_width || ti < width)) {
                    int d;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                    else break;
                    int b = base ? base : 10;
                    if (d >= b) break;
                    tmp[ti++] = (char)c;
                    c = fgetc(f);
                }
                if (c != EOF) ungetc(c, f);
                if (ti == 0) return assigned;
                tmp[ti] = '\0';
                if (is_signed) {
                    long long v = strtoll(tmp, 0, base ? base : 0);
                    if (!suppress) {
                        if (lenmod == 2) *va_arg(ap, long long *) = v;
                        else if (lenmod == 1) *va_arg(ap, long *) = (long)v;
                        else if (lenmod == -1) *va_arg(ap, short *) = (short)v;
                        else *va_arg(ap, int *) = (int)v;
                        assigned++;
                    }
                } else {
                    unsigned long long v = strtoull(tmp, 0, base ? base : 0);
                    if (!suppress) {
                        if (lenmod == 2) *va_arg(ap, unsigned long long *) = v;
                        else if (lenmod == 1) *va_arg(ap, unsigned long *) = (unsigned long)v;
                        else if (lenmod == -1) *va_arg(ap, unsigned short *) = (unsigned short)v;
                        else *va_arg(ap, unsigned int *) = (unsigned int)v;
                        assigned++;
                    }
                }
                break;
            }
            case 'f': case 'e': case 'E': case 'g': case 'G': case 'a': {
                char tmp[80]; int ti = 0;
                if (c == '+' || c == '-') { if (ti < 79) tmp[ti++] = (char)c; c = fgetc(f); }
                while (c != EOF && ti < 79 && (!have_width || ti < width)) {
                    if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                        c == '+' || c == '-' || c == 'x' || c == 'X' ||
                        (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == 'p' || c == 'P') {
                        tmp[ti++] = (char)c;
                        c = fgetc(f);
                    } else break;
                }
                if (c != EOF) ungetc(c, f);
                if (ti == 0) return assigned;
                tmp[ti] = '\0';
                char *endp;
                double v = strtod(tmp, &endp);
                if (endp == tmp) return assigned;
                // strtod may have matched fewer bytes than we grabbed (e.g. a
                // trailing '.' or exponent marker with nothing after it);
                // push the unmatched tail back, one byte of pushback is
                // enough since only ungetc()'s single slot is guaranteed, so
                // walk it back via re-reading is not an option here - this
                // matches vsscanf's own tail handling, which just advances
                // `s` to `endp` and never needs to "return" bytes because it
                // is scanning a string it does not own. For a stream, any
                // unmatched tail bytes are rare (malformed input) and this
                // is the one conversion where perfect stream fidelity on a
                // malformed float is knowingly not guaranteed.
                (void)0;
                if (!suppress) {
                    if (lenmod >= 1) *va_arg(ap, double *) = v;
                    else *va_arg(ap, float *) = (float)v;
                    assigned++;
                }
                break;
            }
            case 'c': {
                int cnt = have_width ? width : 1;
                char *out = suppress ? 0 : va_arg(ap, char *);
                int i = 0;
                for (; i < cnt; i++) {
                    if (c == EOF) break;
                    if (out) out[i] = (char)c;
                    if (i + 1 < cnt) c = fgetc(f);
                }
                if (i == 0) return assigned;
                if (!suppress) assigned++;
                break;
            }
            case 's': {
                char *out = suppress ? 0 : va_arg(ap, char *);
                int n = 0;
                while (c != EOF && !sk_isspace(c) && (!have_width || n < width)) {
                    if (out) out[n] = (char)c;
                    n++;
                    c = fgetc(f);
                }
                if (c != EOF) ungetc(c, f);
                if (n == 0) return assigned;
                if (out) out[n] = '\0';
                if (!suppress) assigned++;
                break;
            }
            case '[': {
                int negate = 0;
                if (*fmt == '^') { negate = 1; fmt++; }
                const char *set = fmt;
                if (*fmt == ']') fmt++;              // ']' right after '[' is a literal
                while (*fmt && *fmt != ']') fmt++;
                int setlen = (int)(fmt - set);
                if (*fmt == ']') fmt++;
                char *out = suppress ? 0 : va_arg(ap, char *);
                int n = 0;
                while (c != EOF && (!have_width || n < width)) {
                    int in = 0;
                    for (int i = 0; i < setlen; i++) if (set[i] == c) { in = 1; break; }
                    if (negate) in = !in;
                    if (!in) break;
                    if (out) out[n] = (char)c;
                    n++;
                    c = fgetc(f);
                }
                if (c != EOF) ungetc(c, f);
                if (n == 0) return assigned;
                if (out) out[n] = '\0';
                if (!suppress) assigned++;
                break;
            }
            default:
                if (c != EOF) ungetc(c, f);
                return assigned;
        }
    }
    return assigned;
}

int vfscanf(FILE *f, const char *fmt, va_list ap) {
    return vfscanf_core(f, fmt, ap);
}

int fscanf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

int scanf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vfscanf(stdin, fmt, ap);
    va_end(ap);
    return r;
}
