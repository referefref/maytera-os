// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// stdio.c - Standard I/O implementation
// Phase 1 libc completion (#422 / CPython #359): full printf family with
// float (%f/%F/%e/%E/%g/%G), octal (%o), and complete flag/width/precision
// handling. Studied musl + glibc semantics for behavior; implemented here.
#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "unistd.h"
#include "termios.h"   // #745 (local 99): getchar() probes fd 0 for a termios
#include <stdint.h>

// AssaultCube port phase 3: standard remove(), see stdio.h.
int remove(const char *path) {
    if (unlink(path) == 0) return 0;
    return rmdir(path);
}

// #745 printf-shredding fix: putchar()/puts() used to call
// syscall1(SYS_PUTCHAR, c) directly, ONE SYSCALL PER CHARACTER, completely
// bypassing the stdout FILE* stream's buffering (stdio_file.c). Every write()
// under 256 bytes is mirrored to the syslog ring as one record (see
// drivers/console.c), so a line built one putchar() at a time landed in the
// log as one shredded, unreadable record per character. Route through the
// SAME stdout stream fopen()/fwrite()/fprintf() already use (fputc(), a
// pre-existing, already-correct shared primitive) instead of adding a
// second, private buffering scheme. See __stdio_init() in stdio_file.c for
// why stdout's buffering mode itself is chosen per-process (isatty(1)), which
// is what keeps this from breaking a real interactive terminal's immediate
// character echo.
int putchar(int c) {
    return fputc(c, stdout);
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    if (fputc('\n', stdout) == EOF) return EOF;
    return 0;
}

// #745 (local 99): getchar() reads STDIN, not the raw kernel keyboard.
//
// This was `return syscall0(SYS_GETCHAR);`, which reads the kernel's GLOBAL
// keyboard buffer and has nothing to do with fd 0. For anything the Terminal
// spawns that is doubly wrong: the keystrokes meant for the child are written
// to the pty master and arrive on its fd 0 through the tty line discipline,
// while SYS_GETCHAR reads a buffer the COMPOSITOR drains eight keys per frame
// in its own input loop. So the child raced the compositor for bytes it should
// never have been reading, and lost nearly every time. MEASURED on golden
// build 1872: in `less`, neither SPACE nor `q` did anything at all, and the
// whole line discipline (raw mode, ^D, VMIN/VTIME, ICANON editing) was bypassed
// for every getchar() caller in the tree.
//
// WHY THE GATE, rather than just reading fd 0: a process whose fd 0 is
// /dev/console (anything launched detached, from AUTORUN.CFG, or as a GUI app)
// gets 0 = EOF from console_read() forever (kernel/drivers/console.c), so an
// unconditional read(0) would turn every one of those callers into an instant
// EOF. console_fops.ioctl is NULL, so tcgetattr() FAILS there and succeeds on a
// pty slave, which separates the two cases exactly. Existing non-pty callers
// (doom, rogue, assaultcube) keep the old code path byte for byte.
//
// The answer is cached because otherwise every character costs an extra
// syscall. An app that dup2()s a different fd onto 0 after its first getchar()
// would keep the stale answer; nothing in the tree does that, and the honest
// fix if one ever does is to re-probe on dup2, not to pay a syscall per byte.
int getchar(void) {
    static int stdin_is_tty = -1;
    if (stdin_is_tty < 0) {
        struct termios t;
        stdin_is_tty = (tcgetattr(0, &t) == 0) ? 1 : 0;
    }
    if (stdin_is_tty) {
        unsigned char c;
        long n = read(0, &c, 1);
        if (n == 1) return (int)c;
        return EOF;          // 0 = ^D / master closed; <0 = error
    }
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
// #621 follow-up: this used to call sc() n times unconditionally. sc() stops
// WRITING once the destination is full but the loop kept running, so a
// caller-controlled field width (printf("%*f", w, x), or a literal
// "%2000000000.2f") spun through billions of iterations producing nothing.
// Measured: a sweep that included a width of 2147483647 wedged for over ten
// minutes here. The C99 return value is unchanged - every padding byte is
// still counted - the loop just stops once it can no longer store anything.
static void spad(sink_t *s, char c, int n) {
    if (n <= 0) return;
    while (n > 0 && s->p < s->end) { *s->p++ = c; s->count++; n--; }
    if (n > 0) {
        // Saturate instead of overflowing the signed count.
        if (s->count > 0x7fffffff - n) s->count = 0x7fffffff;
        else s->count += n;
    }
}

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

// ---------------------------------------------------------------------------
// #621: bounds for the float formatter. Every one of these was previously
// implicit, and each implicit assumption was false for some ordinary double.
//
//   GEN_SIG_MAX   gen_digits() has ALWAYS refused to produce more than this
//                 many significant digits (the clamp below). That clamp is
//                 fine in itself, but callers used to compute a digit count
//                 (nsig = E+1+prec, or prec+1) and then INDEX the buffer with
//                 it, reading far past the digits that were actually written.
//                 Callers must now use the generated count and pad with '0'.
//   FLT_PREC_MAX  hard clamp on a caller-supplied precision. printf("%.100f")
//                 used to write 100 fraction bytes into an 80-byte buffer.
//   FLT_BODY_MAX  worst-case formatted body. The widest reachable case is %f
//                 of a value near DBL_MAX: 309 integer digits + '.' +
//                 FLT_PREC_MAX fraction digits = 822. %g can reach ~1028 when
//                 a tiny value pushes the derived precision up. Sized above
//                 both, and every write into it is bounds-checked anyway, so
//                 a mistake in this arithmetic truncates instead of smashing.
// ---------------------------------------------------------------------------
#define GEN_SIG_MAX   36
#define FLT_PREC_MAX  512
#define FLT_BODY_MAX  1152

// Bounded byte sink for the float body builders. Silently drops writes past
// the end rather than running off the buffer; the builders return the number
// of bytes actually stored.
typedef struct { char *p; char *end; } fbuf_t;
static void fput(fbuf_t *b, char c) { if (b->p < b->end) *b->p++ = c; }

// Generate up to `sig` significant decimal digits of a>0 (finite) into
// digits[] (capacity `cap`), returning the decimal exponent E where
// a ~= d0.d1d2... x 10^E. The number ACTUALLY generated is stored through
// *ngen and is min(sig, GEN_SIG_MAX, cap); callers must not index past it.
static int gen_digits(double a, int sig, char *digits, int cap, int *ngen) {
    if (sig < 1) sig = 1;
    if (sig > GEN_SIG_MAX) sig = GEN_SIG_MAX;
    if (cap > 0 && sig > cap) sig = cap;
    if (ngen) *ngen = sig;
    if (a == 0.0) { for (int i = 0; i < sig; i++) digits[i] = '0'; return 0; }

    int E = 0;
    while (a >= 1e16) { a /= 1e16; E += 16; }
    while (a >= 10.0) { a /= 10.0; E++; }
    while (a < 1.0)   { a *= 10.0; E--; }
    // a now in [1,10)

    char tmp[GEN_SIG_MAX];
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

// Build the body of an %e conversion (no sign, no field padding) into out,
// writing at most `cap` bytes. Returns the number of bytes stored.
static int build_e(char *out, int cap, double a, int prec, int upper, int alt) {
    fbuf_t b = { out, out + cap };
    char digits[GEN_SIG_MAX];
    int ngen = 0;
    int E = gen_digits(a, prec + 1, digits, (int)sizeof(digits), &ngen);
    fput(&b, (ngen > 0) ? digits[0] : '0');
    if (prec > 0 || alt) {
        fput(&b, '.');
        // Past the digits the generator actually produced, pad with '0'. This
        // used to index digits[] with i up to prec (unbounded), reading off
        // the end of a 40-byte stack array for any %.60e or wider.
        for (int i = 1; i <= prec; i++) fput(&b, (i < ngen) ? digits[i] : '0');
    }
    fput(&b, upper ? 'E' : 'e');
    int e = E;
    if (e < 0) { fput(&b, '-'); e = -e; } else fput(&b, '+');
    // at least two exponent digits
    char eb[8]; int en = 0;
    if (e == 0) eb[en++] = '0';
    while (e && en < (int)sizeof(eb)) { eb[en++] = (char)('0' + e % 10); e /= 10; }
    while (en < 2 && en < (int)sizeof(eb)) eb[en++] = '0';
    for (int i = en - 1; i >= 0; i--) fput(&b, eb[i]);
    return (int)(b.p - out);
}

// Build the body of an %f conversion (no sign, no field padding) into out,
// writing at most `cap` bytes. Returns the number of bytes stored.
//
// #621 follow-up (correctness, NOT the stack smash): rounding here was wrong
// in two independent ways, both PRE-EXISTING. The frozen pre-#621 fixture
// produces byte-identical wrong output, so neither was introduced by the
// bounds fix; both were found by diffing 369k conversions against glibc.
//
//   A. The "value is below the last printed place" branch emitted a hard zero
//      with no rounding at all, so printf("%.0f", 0.9) printed "0" and
//      printf("%.2f", 0.006) printed "0.00". Money and percentages formatted
//      through this path were silently wrong, not merely imprecise.
//
//   B. The significant-digit count was derived from a ONE-DIGIT probe of the
//      exponent. gen_digits() rounds, and rounding one digit can carry across
//      a power of ten (9.9 -> 1e1), so the probe's exponent is sometimes one
//      too high. That made this function ask for one digit too many, which
//      moved the rounding one place to the right, and the display loop then
//      truncated the extra digit away: a truncation dressed up as a round.
//      printf("%.0f", 9.9) printed "9"; printf("%.1f", 97497.18255) printed
//      "97497.1". Fix: after generating, CONFIRM the exponent the generator
//      actually produced and regenerate ONCE with the corrected count. Once
//      is provably enough (the probe can only introduce a single carry), and
//      it is a straight-line correction rather than a loop, so no adversarial
//      value can spin here.
static int build_f(char *out, int cap, double a, int prec, int alt) {
    fbuf_t b = { out, out + cap };
    char probe[GEN_SIG_MAX];
    int pn = 0;
    int Ep = gen_digits(a, 1, probe, (int)sizeof(probe), &pn);   // rough exponent

    char digits[GEN_SIG_MAX];
    int ngen = 0;

    int nsig = Ep + 1 + prec;             // sig digits through the last frac place
    if (nsig < 1) {
        // Defect A. Every significant digit of `a` sits to the right of the
        // last printed place, so the result is 0 there UNLESS `a` rounds up
        // into it. That can only happen when nsig == 0, i.e. `a` lies in
        // [10^-(prec+1), 10^-prec): then it rounds up exactly when
        // a >= 0.5 * 10^-prec. nsig < 0 puts `a` below a tenth of the last
        // place, which can never round up.
        //
        // gen_digits() only ever rounds AWAY from zero, so the probe exponent
        // is never too LOW; nsig < 0 therefore cannot be a probe artefact
        // hiding a value that should have rounded up.
        int roundup = 0;
        if (nsig == 0) {
            // The probe exponent can be one too HIGH (gen_digits() rounds away
            // from zero, so a leading 9 can carry to 1e+1), which makes nsig
            // read as 0 for a value that is really below a tenth of the last
            // printed place - printf("%.0f", 0.095) must be "0", not "1". Do
            // not trust Ep for this decision: generate the digits and use the
            // exponent the generator actually produced. Ep >= E2 always, so
            // nsig == 0 can only mean the true count is 0 or negative.
            int n2 = 0;
            int E2 = gen_digits(a, GEN_SIG_MAX, digits, (int)sizeof(digits), &n2);
            if (E2 + 1 + prec == 0) {
                if (n2 > 0 && digits[0] > '5') roundup = 1;
                else if (n2 > 0 && digits[0] == '5') {
                    for (int i = 1; i < n2; i++) if (digits[i] != '0') { roundup = 1; break; }
                    // An exact half rounds to even, and the digit it would land
                    // on is 0, which is already even, so leave roundup at 0.
                }
            }
        }
        if (!roundup) {
            fput(&b, '0');
            if (prec > 0 || alt) {
                fput(&b, '.');
                for (int i = 0; i < prec; i++) fput(&b, '0');
            }
        } else {
            // One unit in the last printed place: "1" for %.0f, otherwise
            // "0." then prec-1 zeros and a final '1'.
            fput(&b, prec > 0 ? '0' : '1');
            if (prec > 0 || alt) {
                fput(&b, '.');
                for (int i = 0; i < prec; i++) fput(&b, (i == prec - 1) ? '1' : '0');
            }
        }
        return (int)(b.p - out);
    }

    int E = gen_digits(a, nsig, digits, (int)sizeof(digits), &ngen);
    if (E != Ep) {
        // Defect B: the probe carried, so `nsig` asked for one digit too many
        // and the value was rounded one place too far right. The fix is NOT to
        // regenerate with a smaller count: gen_digits() normalises by repeated
        // multiply/divide by ten, so every extra pass drifts, and re-running it
        // shallower loses the exact-half information (it made printf("%.0f",
        // 96.5) print "97" where the true half-to-even answer is "96").
        // Instead round the digit string ALREADY IN HAND, in decimal. Those
        // digits came from the deeper, more accurate pass, and a trailing "5"
        // with nothing after it is an exact half that decimal rounding gets
        // right by construction.
        int keep = E + 1 + prec;
        if (keep < 1) keep = 1;
        if (keep < ngen) {
            int up = 0;
            if (digits[keep] > '5') up = 1;
            else if (digits[keep] == '5') {
                for (int i = keep + 1; i < ngen; i++)
                    if (digits[i] != '0') { up = 1; break; }
                if (!up) up = (digits[keep - 1] - '0') & 1;   // exact half -> to even
            }
            ngen = keep;
            if (up) {
                int i = keep - 1;
                for (; i >= 0; i--) {
                    if (digits[i] != '9') { digits[i]++; break; }
                    digits[i] = '0';
                }
                if (i < 0) {                    // 99..9 -> 10..0, one place wider
                    for (int k = keep - 1; k > 0; k--) digits[k] = digits[k - 1];
                    digits[0] = '1';
                    E++;
                }
            }
        }
    }

    int di = 0;
    if (E >= 0) {
        // E is the decimal exponent, so this loop runs E+1 times: 301 times for
        // 1e300 and 309 for DBL_MAX. It used to write straight into an 80-byte
        // caller buffer, and it used to index digits[] by `di < nsig` where
        // nsig could be 307, over-reading a 48-byte array. Both are bounded
        // now: every store goes through fput(), and past the digits the
        // generator actually produced (ngen of them) the value is all zeros.
        for (int pos = 0; pos <= E; pos++) {
            fput(&b, (di < ngen) ? digits[di++] : '0');
        }
    } else {
        fput(&b, '0');
    }
    if (prec > 0 || alt) {
        fput(&b, '.');
        for (int k = 0; k < prec; k++) {
            int idx = (E >= 0) ? (E + 1) + k : k + (E + 1);
            if (idx < 0 || idx >= ngen) fput(&b, '0');
            else fput(&b, digits[idx]);
        }
    }
    return (int)(b.p - out);
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
    if (prec > FLT_PREC_MAX) prec = FLT_PREC_MAX;   // #621: was unbounded
    double a = dbl_abs(val);
    char body[FLT_BODY_MAX];                        // #621: was char body[80]
    int blen = 0;
    char lc = upper ? (conv + 32) : conv;

    if (lc == 'f') {
        blen = build_f(body, (int)sizeof(body), a, prec, alt);
    } else if (lc == 'e') {
        blen = build_e(body, (int)sizeof(body), a, prec, upper, alt);
    } else { // 'g'
        int P = prec ? prec : 1;
        char probe[GEN_SIG_MAX];
        int pn = 0;
        int X = gen_digits(a, P, probe, (int)sizeof(probe), &pn); // exp with P sig digits
        if (X >= -4 && X < P) {
            int fp = P - 1 - X;                 // derived precision, can exceed P
            if (fp < 0) fp = 0;
            if (fp > FLT_PREC_MAX) fp = FLT_PREC_MAX;
            blen = build_f(body, (int)sizeof(body), a, fp, alt);
        } else {
            blen = build_e(body, (int)sizeof(body), a, P - 1, upper, alt);
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

// #745 printf-shredding fix: this used to format into a local buffer and
// then loop putchar() (== one raw SYS_PUTCHAR syscall) per character. printf
// on every other libc is just vfprintf(stdout, ...); do the same here rather
// than keeping a second, parallel copy of "format then push through stdout"
// that only this function used.
int vprintf(const char *format, va_list ap) {
    return vfprintf(stdout, format, ap);
}

int printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vprintf(format, ap);
    va_end(ap);
    return ret;
}
