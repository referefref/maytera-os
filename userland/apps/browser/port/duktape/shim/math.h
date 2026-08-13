#ifndef _SHIM_MATH_H
#define _SHIM_MATH_H
#define HUGE_VAL  (__builtin_huge_val())
#define INFINITY  (__builtin_inf())
#define NAN       (__builtin_nan(""))
#define M_PI      3.14159265358979323846
double fabs(double); double floor(double); double ceil(double); double sqrt(double);
double pow(double,double); double fmod(double,double); double exp(double); double log(double);
double sin(double); double cos(double); double tan(double);
double asin(double); double acos(double); double atan(double); double atan2(double,double);
double sinh(double); double cosh(double); double tanh(double);
double log2(double); double log10(double); double cbrt(double);
double trunc(double); double round(double); double copysign(double,double);
double frexp(double,int*); double ldexp(double,int); double nextafter(double,double);
double fmin(double,double); double fmax(double,double); double expm1(double); double log1p(double);
double asinh(double); double acosh(double); double atanh(double);
int isnan(double); int isinf(double); int signbit(double); int isfinite(double);
#endif
