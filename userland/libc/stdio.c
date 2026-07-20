// stdio.c - Standard I/O implementation
// Phase 1 libc completion (#422 / CPython #359): full printf family with
// float (%f/%F/%e/%E/%g/%G), octal (%o), and complete flag/width/precision
// handling. Studied musl + glibc semantics for behavior; implemented here.
#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include <stdint.h>

int putchar(int c) {
    syscall1(SYS_PUTCHAR, c);
    return c;
}

int puts(const char *s) {
    while (*s) {
        putchar(*s++);
    }
    putchar('\n');
    return 0;
}

int getchar(void) {
    return syscall0(SYS_GETCHAR);
}

// ---------------------------------------------------------------------------
// Bounded output sink. Always counts the number of bytes the fully-formatted
// output WOULD occupy (C99 snprintf return value), but only writes while there
// is room in the caller buffer.
// ---------------------------------------------------------------------------
typedef struct { char *p; char *end; int count; } sink_t;
static void sc(sink_t *s, char c) { if (s->p < s->end) *s->p++ = c; s->count++; }
static void sn(sink_t *s, const char *b, int n) { for (int i = 0; i < n; i++) sc(s, b[i]); }
static void spad(sink_t *s, char c, int n) { while (n-- > 0) sc(s, c); }

// ---------------------------------------------------------------------------
// Integer emit with flags/width/precision.
// ---------------------------------------------------------------------------
static void emit_int(sink_t *s, uint64_t uval, int neg, int base, int upper,
                     int width, int prec, int left, int zero,
                     int plus, int space, int alt) {
    char tmp[24];
    int n = 0;
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (uval == 0) {
        if (prec != 0) tmp[n++] = '0';   // a precision of 0 with value 0 -> no digits
    } else {
        while (uval) { tmp[n++] = digs[uval % base]; uval /= base; }
    }

    int zeros = (prec > n) ? prec - n : 0;

    // '#' octal: force a leading zero.
    if (alt && base == 8 && zeros == 0 && (n == 0 || tmp[n - 1] != '0')) zeros = 1;

    char sign = 0;
    if (neg) sign = '-';
    else if (plus) sign = '+';
    else if (space) sign = ' ';

    char pfx[2]; int plen = 0;
    if (alt && base == 16 && n > 0) { pfx[0] = '0'; pfx[1] = upper ? 'X' : 'x'; plen = 2; }

    int total = n + zeros + (sign ? 1 : 0) + plen;

    // A '0' flag is ignored when a precision is given for integer conversions.
    if (prec >= 0) zero = 0;

    if (!left && !zero) spad(s, ' ', width - total);
    if (sign) sc(s, sign);
    sn(s, pfx, plen);
    if (!left && zero) spad(s, '0', width - total);
    spad(s, '0', zeros);
    for (int i = n - 1; i >= 0; i--) sc(s, tmp[i]);
    if (left) spad(s, ' ', width - total);
}

// ---------------------------------------------------------------------------
// Floating point helpers (freestanding, no libm dependency).
// ---------------------------------------------------------------------------
static int dbl_is_nan(double x) { return x != x; }
static int dbl_is_inf(double x) { return x != 0.0 && x + x == x; }
static double dbl_abs(double x) {
    union { double d; uint64_t u; } v; v.d = x; v.u &= 0x7fffffffffffffffULL; return v.d;
}
static int dbl_signbit(double x) {
    union { double d; uint64_t u; } v; v.d = x; return (int)(v.u >> 63);
}

// Generate `sig` significant decimal digits of a>0 (finite) into digits[],
// returning the decimal exponent E where a ~= d0.d1d2... x 10^E.
static int gen_digits(double a, int sig, char *digits) {
    if (sig < 1) sig = 1;
    if (sig > 36) sig = 36;
    if (a == 0.0) { for (int i = 0; i < sig; i++) digits[i] = '0'; return 0; }

    int E = 0;
    while (a >= 1e16) { a /= 1e16; E += 16; }
    while (a >= 10.0) { a /= 10.0; E++; }
    while (a < 1.0)   { a *= 10.0; E--; }
    // a now in [1,10)

    char tmp[40];
    for (int i = 0; i < sig; i++) {
        int d = (int)a;
        if (d < 0) d = 0;
        if (d > 9) d = 9;
        tmp[i] = (char)('0' + d);
        a = (a - d) * 10.0;
    }
    int guard = (int)a;
    double rem = a - guard;                 // residual beyond the guard digit
    int roundup;
    if (guard > 5) roundup = 1;
    else if (guard < 5) roundup = 0;
    else roundup = (rem > 0.0) ? 1 : ((tmp[sig - 1] - '0') & 1);  // half -> to even
    if (roundup) {
        int i = sig - 1;
        for (; i >= 0; i--) {
            if (tmp[i] != '9') { tmp[i]++; break; }
            tmp[i] = '0';
        }
        if (i < 0) {                        // 9.99..9 -> 1.00..0 x10
            for (int k = sig - 1; k > 0; k--) tmp[k] = tmp[k - 1];
            tmp[0] = '1'; E++;
        }
    }
    for (int i = 0; i < sig; i++) digits[i] = tmp[i];
    return E;
}

// Build the body of an %e conversion (no sign, no field padding) into out.
static int build_e(char *out, double a, int prec, int upper, int alt) {
    char digits[40];
    int E = gen_digits(a, prec + 1, digits);
    char *o = out;
    *o++ = digits[0];
    if (prec > 0 || alt) {
        *o++ = '.';
        for (int i = 1; i <= prec; i++) *o++ = digits[i];
    }
    *o++ = upper ? 'E' : 'e';
    int e = E;
    if (e < 0) { *o++ = '-'; e = -e; } else *o++ = '+';
    // at least two exponent digits
    char eb[6]; int en = 0;
    if (e == 0) eb[en++] = '0';
    while (e) { eb[en++] = (char)('0' + e % 10); e /= 10; }
    while (en < 2) eb[en++] = '0';
    for (int i = en - 1; i >= 0; i--) *o++ = eb[i];
    return (int)(o - out);
}

// Build the body of an %f conversion (no sign, no field padding) into out.
static int build_f(char *out, double a, int prec, int alt) {
    char probe[40];
    int E = gen_digits(a, 1, probe);        // learn the exponent

    int nsig = E + 1 + prec;                 // sig digits needed through last frac place
    if (nsig < 1) {
        char *o = out;
        *o++ = '0';
        if (prec > 0 || alt) { *o++ = '.'; for (int i = 0; i < prec; i++) *o++ = '0'; }
        return (int)(o - out);
    }

    char digits[48];
    E = gen_digits(a, nsig, digits);         // regenerate with rounding; E may bump
    char *o = out;
    int di = 0;
    if (E >= 0) {
        for (int pos = 0; pos <= E; pos++) {
            *o++ = (di < nsig) ? digits[di++] : '0';
        }
    } else {
        *o++ = '0';
    }
    if (prec > 0 || alt) {
        *o++ = '.';
        for (int k = 0; k < prec; k++) {
            int idx = (E >= 0) ? (E + 1) + k : k + (E + 1);
            if (idx < 0) *o++ = '0';
            else *o++ = (idx < nsig) ? digits[idx] : '0';
        }
    }
    return (int)(o - out);
}

static void emit_float(sink_t *s, double val, int prec, char conv,
                       int width, int left, int zero,
                       int plus, int space, int alt) {
    int upper = (conv >= 'A' && conv <= 'Z');
    int neg = dbl_signbit(val);
    char sign = 0;
    if (neg) sign = '-'; else if (plus) sign = '+'; else if (space) sign = ' ';

    if (dbl_is_nan(val) || dbl_is_inf(val)) {
        const char *w = dbl_is_nan(val) ? (upper ? "NAN" : "nan")
                                        : (upper ? "INF" : "inf");
        int len = 3 + (sign ? 1 : 0);
        if (!left) spad(s, ' ', width - len);   // never zero-pad nan/inf
        if (sign) sc(s, sign);
        sn(s, w, 3);
        if (left) spad(s, ' ', width - len);
        return;
    }

    if (prec < 0) prec = 6;
    double a = dbl_abs(val);
    char body[80];
    int blen = 0;
    char lc = upper ? (conv + 32) : conv;

    if (lc == 'f') {
        blen = build_f(body, a, prec, alt);
    } else if (lc == 'e') {
        blen = build_e(body, a, prec, upper, alt);
    } else { // 'g'
        int P = prec ? prec : 1;
        char probe[40];
        int X = gen_digits(a, P, probe);       // exponent with P sig digits
        if (X >= -4 && X < P) {
            blen = build_f(body, a, P - 1 - X, alt);
        } else {
            blen = build_e(body, a, P - 1, upper, alt);
        }
        if (!alt) {
            // strip trailing zeros (and a trailing '.') from the mantissa part
            int epos = -1;
            for (int i = 0; i < blen; i++) if (body[i] == 'e' || body[i] == 'E') { epos = i; break; }
            int mend = (epos < 0) ? blen : epos;
            int has_dot = 0;
            for (int i = 0; i < mend; i++) if (body[i] == '.') { has_dot = 1; break; }
            if (has_dot) {
                int i = mend - 1;
                while (i >= 0 && body[i] == '0') i--;
                if (i >= 0 && body[i] == '.') i--;
                int newm = i + 1;
                if (epos >= 0) {
                    memmove(body + newm, body + epos, blen - epos);
                    blen = newm + (blen - epos);
                } else {
                    blen = newm;
                }
            }
        }
    }

    int total = blen + (sign ? 1 : 0);
    if (!left && !zero) spad(s, ' ', width - total);
    if (sign) sc(s, sign);
    if (!left && zero) spad(s, '0', width - total);
    sn(s, body, blen);
    if (left) spad(s, ' ', width - total);
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    sink_t s;
    s.p = str;
    s.end = (size > 0) ? str + size - 1 : str;   // reserve room for NUL
    s.count = 0;

    while (*format) {
        if (*format != '%') { sc(&s, *format++); continue; }
        format++;   // skip %

        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;;) {
            if (*format == '-') { left = 1; format++; }
            else if (*format == '0') { zero = 1; format++; }
            else if (*format == '+') { plus = 1; format++; }
            else if (*format == ' ') { space = 1; format++; }
            else if (*format == '#') { alt = 1; format++; }
            else break;
        }

        int width = 0;
        if (*format == '*') { width = va_arg(ap, int); format++; if (width < 0) { left = 1; width = -width; } }
        else while (*format >= '0' && *format <= '9') { width = width * 10 + (*format++ - '0'); }

        int prec = -1;
        if (*format == '.') {
            format++;
            if (*format == '*') { prec = va_arg(ap, int); format++; if (prec < 0) prec = -1; }
            else { prec = 0; while (*format >= '0' && *format <= '9') prec = prec * 10 + (*format++ - '0'); }
        }

        int lenmod = 0;   // 0=int, >=1 = long/long long/size_t etc.
        if (*format == 'l') { lenmod = 1; format++; if (*format == 'l') { lenmod = 2; format++; } }
        else if (*format == 'h') { format++; if (*format == 'h') format++; }
        else if (*format == 'z' || *format == 'j' || *format == 't') { lenmod = 1; format++; }
        else if (*format == 'L') { format++; }
        int is_long = (lenmod >= 1);

        char conv = *format;
        switch (conv) {
            case 'd': case 'i': {
                int64_t v = is_long ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
                int neg = v < 0;
                uint64_t uv = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
                emit_int(&s, uv, neg, 10, 0, width, prec, left, zero, plus, space, 0);
                break;
            }
            case 'u': {
                uint64_t v = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
                emit_int(&s, v, 0, 10, 0, width, prec, left, zero, 0, 0, 0);
                break;
            }
            case 'o': {
                uint64_t v = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
                emit_int(&s, v, 0, 8, 0, width, prec, left, zero, 0, 0, alt);
                break;
            }
            case 'x': case 'X': {
                uint64_t v = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
                emit_int(&s, v, 0, 16, conv == 'X', width, prec, left, zero, 0, 0, alt);
                break;
            }
            case 'p': {
                uint64_t v = (uint64_t)va_arg(ap, void *);
                sc(&s, '0'); sc(&s, 'x');
                emit_int(&s, v, 0, 16, 0, 0, -1, 0, 0, 0, 0, 0);
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                double v = va_arg(ap, double);
                emit_float(&s, v, prec, conv, width, left, zero, plus, space, alt);
                break;
            }
            case 'c': {
                char ch = (char)va_arg(ap, int);
                if (!left) spad(&s, ' ', width - 1);
                sc(&s, ch);
                if (left) spad(&s, ' ', width - 1);
                break;
            }
            case 's': {
                const char *str2 = va_arg(ap, const char *);
                if (!str2) str2 = "(null)";
                int len = 0;
                while (str2[len] && (prec < 0 || len < prec)) len++;
                if (!left) spad(&s, ' ', width - len);
                sn(&s, str2, len);
                if (left) spad(&s, ' ', width - len);
                break;
            }
            case '%':
                sc(&s, '%');
                break;
            case '\0':
                goto done;
            default:
                // Unknown conversion: emit literally, do NOT consume an arg.
                sc(&s, '%');
                sc(&s, conv);
                break;
        }
        if (*format) format++;
    }

done:
    if (size > 0) *s.p = '\0';
    return s.count;
}

int vsprintf(char *str, const char *format, va_list ap) {
    return vsnprintf(str, (size_t)-1 >> 1, format, ap);
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(str, size, format, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsprintf(str, format, ap);
    va_end(ap);
    return ret;
}

int vprintf(const char *format, va_list ap) {
    char buf[1024];
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    int n = len;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    for (int i = 0; i < n; i++) putchar(buf[i]);
    return len;
}

int printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vprintf(format, ap);
    va_end(ap);
    return ret;
}
