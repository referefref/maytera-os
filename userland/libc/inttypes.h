// inttypes.h - PRI*/SCN* format macros for MayteraOS libc (#359 Phase 2).
#ifndef LIBC_INTTYPES_H
#define LIBC_INTTYPES_H
#include <stdint.h>

#define __PRI64  "l"
#define PRId8   "d"
#define PRId16  "d"
#define PRId32  "d"
#define PRId64  __PRI64 "d"
#define PRIi32  "i"
#define PRIi64  __PRI64 "i"
#define PRIu8   "u"
#define PRIu16  "u"
#define PRIu32  "u"
#define PRIu64  __PRI64 "u"
#define PRIx32  "x"
#define PRIx64  __PRI64 "x"
#define PRIX32  "X"
#define PRIX64  __PRI64 "X"
#define PRIo32  "o"
#define PRIo64  __PRI64 "o"
#define PRIdPTR __PRI64 "d"
#define PRIuPTR __PRI64 "u"
#define PRIxPTR __PRI64 "x"
#define PRIiPTR __PRI64 "i"
#define PRIdMAX __PRI64 "d"
#define PRIuMAX __PRI64 "u"
#define PRIxMAX __PRI64 "x"

#define SCNd32  "d"
#define SCNd64  __PRI64 "d"
#define SCNu32  "u"
#define SCNu64  __PRI64 "u"
#define SCNx32  "x"
#define SCNx64  __PRI64 "x"

typedef struct { intmax_t quot; intmax_t rem; } imaxdiv_t;
intmax_t imaxabs(intmax_t j);
imaxdiv_t imaxdiv(intmax_t num, intmax_t den);
intmax_t  strtoimax(const char *nptr, char **endptr, int base);
uintmax_t strtoumax(const char *nptr, char **endptr, int base);

#endif // LIBC_INTTYPES_H
