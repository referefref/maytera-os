/* <math.h> shim for vendored faad2 (#331). The FIXED_POINT AAC-LC path calls
 * NO libm function (sin/cos/pow/sqrt are all under #ifndef FIXED_POINT or only
 * in the excluded SBR/PS code). Empty so float-guarded #include <math.h>
 * resolves; objdump-verified float-free. */
#ifndef _MAYTERA_FAAD_MATH_H
#define _MAYTERA_FAAD_MATH_H
#endif
