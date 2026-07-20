// math.h stub for DOOM (uses fixed-point math)
#ifndef DOOM_MATH_H
#define DOOM_MATH_H

// DOOM doesn't really use floating point
static inline double fabs(double x) { return x < 0 ? -x : x; }
static inline double floor(double x) { return (double)(int)x; }
static inline double ceil(double x) { return (double)((int)x + 1); }

#endif
