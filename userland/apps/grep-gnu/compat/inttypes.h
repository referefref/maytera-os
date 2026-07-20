/* inttypes.h - minimal compat for MayteraOS (LP64), C locale. */
#ifndef COMPAT_INTTYPES_H
#define COMPAT_INTTYPES_H

#include <stdint.h>

/* printf/scanf length specifiers for the *max_t types (LP64: intmax_t == long) */
#define PRIdMAX "ld"
#define PRIiMAX "li"
#define PRIoMAX "lo"
#define PRIuMAX "lu"
#define PRIxMAX "lx"
#define PRIXMAX "lX"

uintmax_t strtoumax(const char *nptr, char **endptr, int base);
intmax_t  strtoimax(const char *nptr, char **endptr, int base);

#endif /* COMPAT_INTTYPES_H */
