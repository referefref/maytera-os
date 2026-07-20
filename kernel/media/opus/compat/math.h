/* <math.h> shim for the vendored fixed-point libopus (#331). The integer
 * (FIXED_POINT) decode path calls NO libm function; this header exists only so
 * that the float-guarded #include <math.h> directives resolve. If any libm
 * symbol were actually referenced, the freestanding link would fail (verified
 * float-free via objdump). */
#ifndef _MAYTERA_OPUS_MATH_H
#define _MAYTERA_OPUS_MATH_H
#endif
