/* locale.h - minimal compat for MayteraOS. We run in the C locale only;
 * HAVE_SETLOCALE is not defined, so grep does not call setlocale(), but a few
 * gnulib units include <locale.h> for category macros. */
#ifndef COMPAT_LOCALE_H
#define COMPAT_LOCALE_H

#define LC_CTYPE    0
#define LC_NUMERIC  1
#define LC_TIME     2
#define LC_COLLATE  3
#define LC_MONETARY 4
#define LC_MESSAGES 5
#define LC_ALL      6

struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
};

char *setlocale(int category, const char *locale);
struct lconv *localeconv(void);

#endif /* COMPAT_LOCALE_H */
