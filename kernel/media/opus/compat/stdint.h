/* <stdint.h> for the vendored libopus (#331). Matches the kernel's fixed-width
 * types exactly (64-bit = long long) so the Opus wrapper, which also includes
 * the kernel types.h, sees a single consistent set of typedefs (gcc's own
 * stdint.h uses 'long' for 64-bit, which conflicts). */
#ifndef _MAYTERA_OPUS_STDINT_H
#define _MAYTERA_OPUS_STDINT_H
#ifndef TYPES_H   /* kernel types.h already defines these identically */
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef long long          intptr_t;
typedef unsigned long long uintptr_t;
#endif
#ifndef INT16_MAX
#define INT16_MAX  32767
#define INT16_MIN  (-32768)
#endif
#ifndef INT32_MAX
#define INT32_MAX  2147483647
#define INT32_MIN  (-2147483647-1)
#endif
#endif
