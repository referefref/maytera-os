/* <stdint.h> for vendored faad2 (#331). 64-bit = long long to match the
 * kernel's types.h exactly. */
#ifndef _MAYTERA_FAAD_STDINT_H
#define _MAYTERA_FAAD_STDINT_H
#ifndef TYPES_H
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
#define INT16_MAX 32767
#define INT16_MIN (-32768)
#endif
#ifndef INT32_MAX
#define INT32_MAX 2147483647
#define INT32_MIN (-2147483647-1)
#endif
#endif
