// math.h - freestanding libm for MayteraOS userland.
// Base set added for TinyGL (#319); extended for CPython #359 / #422.
#ifndef _MAYTERA_MATH_H
#define _MAYTERA_MATH_H

#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440
#define M_E        2.7182818284590452354
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.43429448190325182765
#define HUGE_VAL   (1e308*10.0)
#define INFINITY   (__builtin_inff())
#define NAN        (__builtin_nanf(""))

// C99 classification (double-precision arithmetic form; freestanding-safe)
#define isnan(x)     ((x) != (x))
#define isinf(x)     (!isnan(x) && isnan((x) - (x)))
#define isfinite(x)  (!isnan((x) - (x)))
#define signbit(x)   (__builtin_signbit(x))
#define fpclassify(x) (isnan(x)?0:isinf(x)?1:((x)==0.0?2:4))

// Core (double)
double fabs(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double rint(double x);
double nearbyint(double x);
long   lround(double x);
long   lrint(double x);
double sqrt(double x);
double cbrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double asin(double x);
double acos(double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);
double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double pow(double x, double y);
double fmod(double x, double y);
double remainder(double x, double y);
double hypot(double x, double y);
double copysign(double x, double y);
double fmax(double x, double y);
double fmin(double x, double y);
double fdim(double x, double y);
double ldexp(double x, int exp);
double scalbn(double x, int n);
double frexp(double x, int *e);
double modf(double x, double *iptr);

// Float variants
float sqrtf(float x);
float fabsf(float x);
float sinf(float x);
float cosf(float x);
float floorf(float x);
float ceilf(float x);
float truncf(float x);
float roundf(float x);
float powf(float x, float y);
float logf(float x);
float log10f(float x);
float expf(float x);
float fmodf(float x, float y);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);
float hypotf(float x, float y);
float fmaxf(float x, float y);
float fminf(float x, float y);
float copysignf(float x, float y);

// Special functions (gamma/error function family) - additive-only, #359
// Phase 3a. Implementations live in the CPython port's mathsupp supplement
// (userland libc has no libm gamma/erf family yet), these are just the
// missing prototypes so callers outside that supplement can also use them.
double erf(double x);
double erfc(double x);
double tgamma(double x);
double lgamma(double x);
double lgamma_r(double x, int *sign);
double gamma(double x);
double nextafter(double x, double y);
double fma(double a, double b, double c);
float  fmaf(float a, float b, float c);
double remquo(double x, double y, int *quo);
float  lgammaf(float x);
float  tgammaf(float x);
float  erff(float x);
float  erfcf(float x);

#endif
