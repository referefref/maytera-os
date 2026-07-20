// scanf.c - formatted input for MayteraOS userland (#422 / CPython #359).
// Full sscanf/vsscanf; scanf/fscanf are line-oriented wrappers over vsscanf.
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

// Line-oriented fscanf/scanf: read one line then delegate to vsscanf. This
// covers the overwhelmingly common line-at-a-time usage; it does not span
// input records the way a byte-streaming scanf would.
int vfscanf(FILE *f, const char *fmt, va_list ap) {
    char line[1024];
    if (!fgets(line, sizeof(line), f)) return -1;
    return vsscanf(line, fmt, ap);
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
