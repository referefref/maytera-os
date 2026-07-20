/* libmad build config for the MayteraOS freestanding kernel.
 * Integer/fixed-point only (FPM_64BIT): uses 64-bit signed multiply, NO FPU.
 * The kernel is built -mno-sse -mno-sse2 so no runtime floating point is used.
 */
#ifndef MAYTERA_LIBMAD_CONFIG_H
#define MAYTERA_LIBMAD_CONFIG_H

#define FPM_64BIT          1   /* 64-bit multiply fixed-point path (no asm, no FPU) */
#define SIZEOF_INT         4   /* mad_fixed_t must be 32-bit signed int */
#define SIZEOF_LONG        8
#define SIZEOF_LONG_LONG   8
#define HAVE_LIMITS_H      1   /* gcc freestanding provides <limits.h> */
#define NDEBUG             1   /* assert() compiles to nothing; no abort needed */

#endif
