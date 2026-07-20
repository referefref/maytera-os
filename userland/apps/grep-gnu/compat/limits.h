/* limits.h - compat for MayteraOS freestanding build (LP64, signed char).
 * gcc's own limits.h does #include_next <limits.h>, which fails under
 * -nostdinc; providing this (found first via -Icompat) avoids that. */
#ifndef COMPAT_LIMITS_H
#define COMPAT_LIMITS_H

#define CHAR_BIT    8
#define MB_LEN_MAX  16

#define SCHAR_MIN   (-128)
#define SCHAR_MAX   127
#define UCHAR_MAX   255

/* char is signed on x86-64 */
#define CHAR_MIN    SCHAR_MIN
#define CHAR_MAX    SCHAR_MAX

#define SHRT_MIN    (-32768)
#define SHRT_MAX    32767
#define USHRT_MAX   65535

#define INT_MIN     (-INT_MAX - 1)
#define INT_MAX     2147483647
#define UINT_MAX    4294967295U

#define LONG_MIN    (-LONG_MAX - 1L)
#define LONG_MAX    9223372036854775807L
#define ULONG_MAX   18446744073709551615UL

#define LLONG_MIN   (-LLONG_MAX - 1LL)
#define LLONG_MAX   9223372036854775807LL
#define ULLONG_MAX  18446744073709551615ULL

#define SSIZE_MAX   LONG_MAX

#ifndef PATH_MAX
#define PATH_MAX    4096
#endif
#ifndef NAME_MAX
#define NAME_MAX    255
#endif

#endif /* COMPAT_LIMITS_H */
