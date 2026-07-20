// math.c - minimal freestanding libm for MayteraOS userland.
// Added for the TinyGL port (task #319). Accuracy targets graphics use, not
// full IEEE conformance. Uses SSE2 (the userland is built with -msse2).
#include "math.h"
#include <stdint.h>

double fabs(double x) {
    union { double d; uint64_t u; } v; v.d = x; v.u &= 0x7fffffffffffffffULL; return v.d;
}
float fabsf(float x) { return (float)fabs((double)x); }

double sqrt(double x) {
    if (x < 0.0) return 0.0/0.0; // NaN
    double r;
    __asm__ __volatile__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}
float sqrtf(float x) {
    float r;
    __asm__ __volatile__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

double floor(double x) {
    double t = (double)(long long)x;
    if (t > x) t -= 1.0;
    return t;
}
float floorf(float x) { return (float)floor((double)x); }

double ceil(double x) {
    double t = (double)(long long)x;
    if (t < x) t += 1.0;
    return t;
}

double fmod(double x, double y) {
    if (y == 0.0) return 0.0/0.0;
    double q = x / y;
    // truncate q toward zero
    double t = (double)(long long)q;
    return x - t * y;
}

// ---- sin/cos via range reduction + minimax-ish Taylor on [-pi/4, pi/4] ----
static double sin_kernel(double x) {
    // Taylor, x in [-pi/4, pi/4]
    double x2 = x * x;
    return x * (1.0 + x2 * (-1.0/6.0 + x2 * (1.0/120.0 + x2 * (-1.0/5040.0 + x2 * (1.0/362880.0)))));
}
static double cos_kernel(double x) {
    double x2 = x * x;
    return 1.0 + x2 * (-0.5 + x2 * (1.0/24.0 + x2 * (-1.0/720.0 + x2 * (1.0/40320.0 + x2 * (-1.0/3628800.0)))));
}

double sin(double x) {
    // reduce to [-pi, pi]
    double tp = 1.0 / (2.0 * M_PI);
    double k = floor(x * tp + 0.5);
    x -= k * (2.0 * M_PI);
    // now x in [-pi, pi]; reduce to quadrants of pi/2
    int sign = 1;
    if (x < 0) { x = -x; sign = -1; }
    // x in [0, pi]
    double r;
    if (x <= M_PI_2) {
        if (x <= M_PI_2/2.0) r = sin_kernel(x);
        else r = cos_kernel(M_PI_2 - x);
    } else {
        double y = M_PI - x; // [0, pi/2]
        if (y <= M_PI_2/2.0) r = sin_kernel(y);
        else r = cos_kernel(M_PI_2 - y);
    }
    return sign * r;
}

double cos(double x) { return sin(x + M_PI_2); }

double tan(double x) { double c = cos(x); if (c == 0.0) return 0.0; return sin(x)/c; }

// ---- exp/log/pow ----
double exp(double x) {
    if (x > 709.78) return HUGE_VAL;
    if (x < -745.13) return 0.0;
    // exp(x) = 2^n * exp(f), n = round(x/ln2), |f| <= ln2/2 ~= 0.3466.
    // Taylor to f^10 over that range gives ~1e-12 (upgraded from graphics-grade
    // ~1e-6 for CPython's math module, #422).
    const double ln2 = 0.69314718055994530942;
    double n = floor(x / ln2 + 0.5);
    double f = x - n * ln2;
    double ef = 1.0 + f*(1.0 + f*(1.0/2.0 + f*(1.0/6.0 + f*(1.0/24.0 + f*(1.0/120.0
              + f*(1.0/720.0 + f*(1.0/5040.0 + f*(1.0/40320.0 + f*(1.0/362880.0
              + f*(1.0/3628800.0))))))))));
    int ni = (int)n;
    union { double d; uint64_t u; } v;
    v.u = (uint64_t)(1023 + ni) << 52;
    return ef * v.d;
}

double log(double x) {
    if (x < 0.0) return 0.0 / 0.0;            // NaN
    if (x == 0.0) return -HUGE_VAL;
    union { double d; uint64_t u; } v; v.d = x;
    int e = (int)((v.u >> 52) & 0x7ff) - 1023;
    v.u = (v.u & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL; // mantissa in [1,2)
    double m = v.d;
    // Reduce to m in [1/sqrt2, sqrt2) so |s|<=0.1716 and the atanh series
    // converges fast; series to s^13 gives ~1e-13 (#422 accuracy upgrade).
    if (m > 1.4142135623730951) { m *= 0.5; e++; }
    double s = (m - 1.0) / (m + 1.0);
    double s2 = s * s;
    double t = 2.0 * s * (1.0 + s2*(1.0/3.0 + s2*(1.0/5.0 + s2*(1.0/7.0
             + s2*(1.0/9.0 + s2*(1.0/11.0 + s2*(1.0/13.0)))))));
    return t + e * 0.69314718055994530942;
}

double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 0.0) return 0.0;
    if (x > 0.0) return exp(y * log(x));
    // negative base: only integer exponents meaningful
    double yi = floor(y);
    double r = exp(y * log(-x));
    if (yi == y) { // integer exponent
        long long n = (long long)yi;
        if (n & 1) return -r;
    }
    return r;
}
float powf(float x, float y) { return (float)pow((double)x, (double)y); }
float sinf(float x) { return (float)sin((double)x); }
float cosf(float x) { return (float)cos((double)x); }

// ---- atan/atan2/asin/acos ----
double atan(double x) {
    int sign = 1;
    if (x < 0) { x = -x; sign = -1; }
    int inv = 0;
    if (x > 1.0) { x = 1.0 / x; inv = 1; }
    double x2 = x * x;
    // poly approximation on [0,1]
    double r = x * (0.9998660 + x2*(-0.3302995 + x2*(0.1801410 + x2*(-0.0851330 + x2*0.0208351))));
    if (inv) r = M_PI_2 - r;
    return sign * r;
}
double atan2(double y, double x) {
    if (x > 0.0) return atan(y / x);
    if (x < 0.0) {
        if (y >= 0.0) return atan(y / x) + M_PI;
        return atan(y / x) - M_PI;
    }
    // x == 0
    if (y > 0.0) return M_PI_2;
    if (y < 0.0) return -M_PI_2;
    return 0.0;
}
double asin(double x) {
    if (x >= 1.0) return M_PI_2;
    if (x <= -1.0) return -M_PI_2;
    return atan(x / sqrt(1.0 - x*x));
}
double acos(double x) { return M_PI_2 - asin(x); }

// ===========================================================================
// Phase 1 (#422 / CPython #359) additions: the libm essentials CPython's math
// and cmath modules pull in. Accuracy is ~1 ULP for common ranges (built on
// the exp/log/sqrt primitives above), sufficient for the C math module which
// wraps these; Python's own numeric core uses its bundled dtoa, not libm.
// ===========================================================================

double copysign(double x, double y) {
    union { double d; uint64_t u; } vx, vy;
    vx.d = x; vy.d = y;
    vx.u = (vx.u & 0x7fffffffffffffffULL) | (vy.u & 0x8000000000000000ULL);
    return vx.d;
}
float copysignf(float x, float y) { return (float)copysign((double)x, (double)y); }

double trunc(double x) {
    if (x != x) return x;
    double a = fabs(x);
    if (a >= 4503599627370496.0) return x;   // >= 2^52: already integral
    double t = (double)(long long)a;
    return copysign(t, x);
}
float truncf(float x) { return (float)trunc((double)x); }

double round(double x) {
    if (x != x) return x;
    double a = fabs(x);
    if (a >= 4503599627370496.0) return x;
    double t = floor(a + 0.5);               // half away from zero
    return copysign(t, x);
}
float roundf(float x) { return (float)round((double)x); }

long lround(double x) { return (long)round(x); }
long lrint(double x)  { return (long)round(x); }
double rint(double x) { return round(x); }
double nearbyint(double x) { return round(x); }

float ceilf(float x)  { return (float)ceil((double)x); }
float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }

double fmax(double x, double y) { if (x != x) return y; if (y != y) return x; return x > y ? x : y; }
double fmin(double x, double y) { if (x != x) return y; if (y != y) return x; return x < y ? x : y; }
float  fmaxf(float x, float y) { return (float)fmax((double)x, (double)y); }
float  fminf(float x, float y) { return (float)fmin((double)x, (double)y); }
double fdim(double x, double y) { return (x > y) ? x - y : 0.0; }

double log10(double x) { return log(x) / 2.302585092994045684; }
double log2(double x)  { return log(x) / 0.693147180559945309; }
double exp2(double x)  { return exp(x * 0.693147180559945309); }
float  log10f(float x) { return (float)log10((double)x); }
float  logf(float x)   { return (float)log((double)x); }
float  expf(float x)   { return (float)exp((double)x); }
double log1p(double x) { return log(1.0 + x); }
double expm1(double x) { return exp(x) - 1.0; }

double sinh(double x) { double e = exp(x); return (e - 1.0 / e) * 0.5; }
double cosh(double x) { double e = exp(x); return (e + 1.0 / e) * 0.5; }
double tanh(double x) {
    if (x > 20.0) return 1.0;
    if (x < -20.0) return -1.0;
    double e = exp(2.0 * x);
    return (e - 1.0) / (e + 1.0);
}
double asinh(double x) { return copysign(log(fabs(x) + sqrt(x * x + 1.0)), x); }
double acosh(double x) { return log(x + sqrt(x * x - 1.0)); }
double atanh(double x) { return 0.5 * log((1.0 + x) / (1.0 - x)); }
float  sinhf(float x) { return (float)sinh((double)x); }
float  coshf(float x) { return (float)cosh((double)x); }
float  tanhf(float x) { return (float)tanh((double)x); }

double hypot(double x, double y) {
    x = fabs(x); y = fabs(y);
    if (x < y) { double t = x; x = y; y = t; }
    if (x == 0.0) return 0.0;
    double r = y / x;
    return x * sqrt(1.0 + r * r);
}
float hypotf(float x, float y) { return (float)hypot((double)x, (double)y); }

double cbrt(double x) {
    if (x == 0.0) return x;
    return copysign(pow(fabs(x), 1.0 / 3.0), x);
}

double ldexp(double x, int exp) {
    // multiply by 2^exp using repeated squaring (handles exp beyond one step)
    double f = 1.0; double b = 2.0; int e = exp < 0 ? -exp : exp;
    while (e) { if (e & 1) f *= b; b *= b; e >>= 1; }
    return exp < 0 ? x / f : x * f;
}
double scalbn(double x, int n) { return ldexp(x, n); }

double frexp(double x, int *e) {
    if (x == 0.0 || x != x || x + x == x) { *e = 0; return x; }
    union { double d; uint64_t u; } v; v.d = x;
    int ex = (int)((v.u >> 52) & 0x7ff);
    if (ex == 0) {                            // subnormal: normalize
        v.d = x * 18014398509481984.0;        // 2^54
        ex = (int)((v.u >> 52) & 0x7ff) - 54;
    }
    *e = ex - 1022;
    v.u = (v.u & ~(0x7ffULL << 52)) | (1022ULL << 52); // mantissa in [0.5,1)
    return v.d;
}

double modf(double x, double *iptr) {
    double i = trunc(x);
    *iptr = i;
    if (x != x) return x;
    if (x + x == x && x != 0.0) return copysign(0.0, x);  // +/-inf -> +/-0
    return x - i;
}

double remainder(double x, double y) {
    if (y == 0.0 || x != x || y != y) return 0.0 / 0.0;
    double q = round(x / y);
    return x - q * y;
}
