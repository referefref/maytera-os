/* duk_support.c - freestanding libm + date/time + sscanf for the MayteraOS
 * Duktape port. Provides the external symbols Duktape references that the
 * userland libc does not: the math.h functions, the <time.h> calendar
 * helpers, and a small sscanf. Accuracy targets are "good enough for a
 * browser JS engine" (~1e-12 relative for transcendentals), not bit-exact
 * IEEE libm. SSE is available (apps build -msse2) so sqrt uses sqrtsd.
 *
 * Kept local to the Duktape port (linked alongside duktape.o into the
 * browser) so the shared libc is untouched: zero regression risk for
 * every other app. Can be promoted into libc later if useful.
 */

#include <types.h>
#include <stdarg.h>

/* time_t / struct tm come from the shim <time.h>. */
#include <time.h>

typedef union { double d; unsigned long long u; } d2u_t;

#define DS_PI   3.14159265358979323846
#define DS_PI2  1.57079632679489661923
#define DS_2PI  6.28318530717958647692
#define DS_LN2  0.69314718055994530942
#define DS_INF  (__builtin_inf())
#define DS_NAN  (__builtin_nan(""))

static int ds_isnan(double x) { return x != x; }

double fabs(double x) { d2u_t v; v.d = x; v.u &= 0x7fffffffffffffffULL; return v.d; }

double copysign(double x, double y) {
    d2u_t a, b; a.d = x; b.d = y;
    a.u = (a.u & 0x7fffffffffffffffULL) | (b.u & 0x8000000000000000ULL);
    return a.d;
}

double trunc(double x) {
    if (ds_isnan(x)) return x;
    if (fabs(x) >= 4.503599627370496e15) return x; /* >= 2^52: already integral */
    long long i = (long long) x;
    return (double) i;
}

double floor(double x) {
    double t = trunc(x);
    return (t > x) ? t - 1.0 : t;
}

double ceil(double x) {
    double t = trunc(x);
    return (t < x) ? t + 1.0 : t;
}

double sqrt(double x) {
    if (x < 0.0) return DS_NAN;
    double r;
    __asm__ __volatile__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

static double ds_ldexp(double x, int n) {
    /* x * 2^n via repeated scaling (n is bounded for our callers). */
    while (n > 30) { x *= 1073741824.0; n -= 30; }
    while (n < -30) { x *= (1.0 / 1073741824.0); n += 30; }
    double f = 1.0, base = (n >= 0) ? 2.0 : 0.5;
    int k = (n >= 0) ? n : -n;
    while (k--) f *= base;
    return x * f;
}

double fmod(double x, double y) {
    if (y == 0.0 || ds_isnan(x) || ds_isnan(y)) return DS_NAN;
    double r = x - trunc(x / y) * y;
    return r;
}

double exp(double x) {
    if (ds_isnan(x)) return x;
    if (x > 709.78) return DS_INF;
    if (x < -745.13) return 0.0;
    double k = floor(x / DS_LN2 + 0.5);
    double r = x - k * DS_LN2;            /* |r| <= ln2/2 */
    double term = 1.0, sum = 1.0;
    for (int i = 1; i < 18; i++) { term *= r / i; sum += term; }
    return ds_ldexp(sum, (int) k);
}

double log(double x) {
    if (ds_isnan(x)) return x;
    if (x < 0.0) return DS_NAN;
    if (x == 0.0) return -DS_INF;
    int e = 0; double m = x;
    while (m >= 1.0) { m *= 0.5; e++; }
    while (m < 0.5) { m *= 2.0; e--; }
    /* log(m) = 2*atanh(t), t = (m-1)/(m+1), m in [0.5,1) -> |t| small */
    double t = (m - 1.0) / (m + 1.0), t2 = t * t, p = t, sum = 0.0;
    for (int i = 0; i < 30; i++) { sum += p / (2 * i + 1); p *= t2; }
    return 2.0 * sum + e * DS_LN2;
}

double log2(double x)  { return log(x) * 1.44269504088896340736; }
double log10(double x) { return log(x) * 0.43429448190325182765; }

double pow(double b, double e) {
    if (e == 0.0) return 1.0;
    if (ds_isnan(b) || ds_isnan(e)) return DS_NAN;
    if (b == 0.0) return (e > 0.0) ? 0.0 : DS_INF;
    /* Exact path for integer exponents via binary exponentiation, so e.g.
     * Math.pow(2,10) is bit-exact 1024 (the exp(log) path drifts by ~1e-13). */
    double ei = trunc(e);
    if (ei == e && fabs(ei) <= 1024.0) {
        double r = 1.0, base = b;
        long long n = (long long) ei;
        int neg = (n < 0); if (neg) n = -n;
        while (n) { if (n & 1) r *= base; base *= base; n >>= 1; }
        return neg ? 1.0 / r : r;
    }
    if (b < 0.0) return DS_NAN;                 /* negative base, non-int exp */
    return exp(e * log(b));
}

double cbrt(double x) {
    if (x == 0.0) return 0.0;
    double s = (x < 0.0) ? -1.0 : 1.0;
    return s * exp(log(fabs(x)) / 3.0);
}

double sin(double x) {
    if (ds_isnan(x)) return x;
    x = x - DS_2PI * floor(x / DS_2PI + 0.5);   /* reduce to [-pi, pi] */
    double x2 = x * x, term = x, sum = x;
    for (int i = 1; i < 13; i++) {
        term *= -x2 / ((double)(2 * i) * (double)(2 * i + 1));
        sum += term;
    }
    return sum;
}

double cos(double x) { return sin(x + DS_PI2); }
double tan(double x) { double c = cos(x); return (c == 0.0) ? DS_INF : sin(x) / c; }

double atan(double x) {
    if (ds_isnan(x)) return x;
    int neg = 0, inv = 0;
    if (x < 0.0) { neg = 1; x = -x; }
    if (x > 1.0) { inv = 1; x = 1.0 / x; }
    /* halve twice: atan(x) = 2*atan(x/(1+sqrt(1+x^2))) -> small argument */
    for (int k = 0; k < 2; k++) x = x / (1.0 + sqrt(1.0 + x * x));
    double x2 = x * x, term = x, sum = x;
    for (int i = 1; i < 16; i++) { term *= -x2; sum += term / (2 * i + 1); }
    double r = sum * 4.0;
    if (inv) r = DS_PI2 - r;
    return neg ? -r : r;
}

double atan2(double y, double x) {
    if (x > 0.0) return atan(y / x);
    if (x < 0.0) return (y >= 0.0) ? atan(y / x) + DS_PI : atan(y / x) - DS_PI;
    if (y > 0.0) return DS_PI2;
    if (y < 0.0) return -DS_PI2;
    return 0.0;
}

double asin(double x) {
    if (x <= -1.0) return -DS_PI2;
    if (x >= 1.0) return DS_PI2;
    return atan(x / sqrt(1.0 - x * x));
}

double acos(double x) {
    if (x <= -1.0) return DS_PI;
    if (x >= 1.0) return 0.0;
    return DS_PI2 - asin(x);
}

/* ---------------- date / time ----------------
 * UTC-only calendar. localtime == gmtime (no timezone). time() is wired to
 * the kernel monotonic clock plus a base epoch; wall-clock accuracy is
 * approximate (good enough for JS Date to run; refine with a real RTC later).
 */

/* time() is provided by the userland libc (unistd.o); we only add the
 * calendar helpers (gmtime/localtime/mktime/difftime) it lacks. */
double difftime(time_t a, time_t b) { return (double)(a - b); }

static int ds_is_leap(long y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static struct tm ds_tm_buf;

struct tm *gmtime(const time_t *tp) {
    long secs = (long)(*tp);
    long days = secs / 86400;
    long rem = secs % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }
    ds_tm_buf.tm_hour = (int)(rem / 3600);
    ds_tm_buf.tm_min  = (int)((rem % 3600) / 60);
    ds_tm_buf.tm_sec  = (int)(rem % 60);
    /* 1970-01-01 was a Thursday (wday=4) */
    int wday = (int)((days % 7 + 4) % 7); if (wday < 0) wday += 7;
    ds_tm_buf.tm_wday = wday;
    long y = 1970;
    while (1) {
        long dly = ds_is_leap(y) ? 366 : 365;
        if (days >= dly) { days -= dly; y++; }
        else if (days < 0) { y--; days += ds_is_leap(y) ? 366 : 365; }
        else break;
    }
    ds_tm_buf.tm_year = (int)(y - 1900);
    ds_tm_buf.tm_yday = (int) days;
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int mon = 0;
    while (mon < 12) {
        int dim = mdays[mon] + ((mon == 1 && ds_is_leap(y)) ? 1 : 0);
        if (days >= dim) { days -= dim; mon++; } else break;
    }
    ds_tm_buf.tm_mon = mon;
    ds_tm_buf.tm_mday = (int)(days + 1);
    ds_tm_buf.tm_isdst = 0;
    return &ds_tm_buf;
}

struct tm *localtime(const time_t *tp) { return gmtime(tp); }

time_t mktime(struct tm *tm) {
    long y = tm->tm_year + 1900;
    long days = 0;
    if (y >= 1970) { for (long i = 1970; i < y; i++) days += ds_is_leap(i) ? 366 : 365; }
    else { for (long i = y; i < 1970; i++) days -= ds_is_leap(i) ? 366 : 365; }
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (int m = 0; m < tm->tm_mon && m < 12; m++)
        days += mdays[m] + ((m == 1 && ds_is_leap(y)) ? 1 : 0);
    days += tm->tm_mday - 1;
    return (time_t)(days * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec);
}

/* ---------------- minimal sscanf ----------------
 * Supports whitespace, literal chars, and %[width][l|ll]{d,i,u,x,o,c,s,n}
 * plus %{e,f,g} (with optional l). Enough for Duktape's date-string fallback.
 */

static int ds_isspace(int c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\v'||c=='\f'; }
static int ds_isdigit(int c) { return c >= '0' && c <= '9'; }

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    const char *s = str;
    int assigned = 0;
    while (*fmt) {
        if (ds_isspace((unsigned char)*fmt)) {
            while (ds_isspace((unsigned char)*s)) s++;
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt) break;
            s++; fmt++;
            continue;
        }
        fmt++; /* past % */
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }
        int width = 0, has_width = 0;
        while (ds_isdigit((unsigned char)*fmt)) { width = width*10 + (*fmt - '0'); fmt++; has_width = 1; }
        int lng = 0;
        while (*fmt == 'l' || *fmt == 'L' || *fmt == 'h') { if (*fmt=='l'||*fmt=='L') lng++; fmt++; }
        char conv = *fmt ? *fmt++ : 0;
        if (conv == 0) break;

        if (conv != 'c' && conv != 'n') while (ds_isspace((unsigned char)*s)) s++;

        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'o') {
            int base = (conv == 'x') ? 16 : (conv == 'o') ? 8 : 10;
            int neg = 0; const char *start = s;
            if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
            if (base == 16 && s[0]=='0' && (s[1]=='x'||s[1]=='X')) s += 2;
            unsigned long long val = 0; int got = 0; int cnt = 0;
            while (*s && (!has_width || cnt < width)) {
                int c = (unsigned char)*s, dig;
                if (ds_isdigit(c)) dig = c - '0';
                else if (c >= 'a' && c <= 'f') dig = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') dig = c - 'A' + 10;
                else break;
                if (dig >= base) break;
                val = val * base + dig; got = 1; s++; cnt++;
            }
            if (!got) { s = start; break; }
            if (!suppress) {
                long long sv = neg ? -(long long)val : (long long)val;
                if (lng >= 2) *va_arg(ap, long long *) = sv;
                else if (lng == 1) *va_arg(ap, long *) = (long) sv;
                else *va_arg(ap, int *) = (int) sv;
                assigned++;
            }
        } else if (conv == 'e' || conv == 'f' || conv == 'g' || conv == 'E' || conv == 'G') {
            const char *start = s; int got = 0;
            double sign = 1.0;
            if (*s == '+' || *s == '-') { if (*s=='-') sign = -1.0; s++; }
            double val = 0.0;
            while (ds_isdigit((unsigned char)*s)) { val = val*10.0 + (*s-'0'); s++; got = 1; }
            if (*s == '.') { s++; double f = 0.1; while (ds_isdigit((unsigned char)*s)) { val += (*s-'0')*f; f *= 0.1; s++; got = 1; } }
            if (got && (*s == 'e' || *s == 'E')) {
                s++; int es = 1, ev = 0;
                if (*s=='+'||*s=='-') { if (*s=='-') es=-1; s++; }
                while (ds_isdigit((unsigned char)*s)) { ev = ev*10 + (*s-'0'); s++; }
                val *= pow(10.0, (double)(es*ev));
            }
            if (!got) { s = start; break; }
            val *= sign;
            if (!suppress) {
                if (lng >= 1) *va_arg(ap, double *) = val;
                else *va_arg(ap, float *) = (float) val;
                assigned++;
            }
        } else if (conv == 's') {
            char *out = suppress ? 0 : va_arg(ap, char *);
            int cnt = 0; int got = 0;
            while (*s && !ds_isspace((unsigned char)*s) && (!has_width || cnt < width)) {
                if (out) out[cnt] = *s;
                s++; cnt++; got = 1;
            }
            if (out) out[cnt] = 0;
            if (!got) break;
            if (!suppress) assigned++;
        } else if (conv == 'c') {
            int n = has_width ? width : 1;
            char *out = suppress ? 0 : va_arg(ap, char *);
            int cnt = 0;
            while (*s && cnt < n) { if (out) out[cnt] = *s; s++; cnt++; }
            if (cnt < n) break;
            if (!suppress) assigned++;
        } else if (conv == 'n') {
            if (!suppress) { *va_arg(ap, int *) = (int)(s - str); }
        } else if (conv == '%') {
            if (*s != '%') break;
            s++;
        } else {
            break; /* unsupported conversion */
        }
    }
    va_end(ap);
    return assigned;
}
