// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// locale.h - ISO C localization for MayteraOS userland.
//
// MayteraOS HAS EXACTLY ONE LOCALE, THE "C" LOCALE, AND THIS HEADER SAYS SO.
// That is not a stub: ISO C requires a conforming implementation to support
// only "C" and "", and everything below is the complete, correct behaviour for
// an implementation with no locale database. setlocale() therefore SUCCEEDS for
// "C"/""/NULL and FAILS (returns NULL) for anything else, so a caller asking for
// de_DE.UTF-8 is told no rather than being silently handed the C locale.
//
// WHY IT IS HERE AND NOT IN AN APP. Two private copies already existed when
// this was written - userland/apps/grep-gnu/compat/locale.h + compat/shim.c and
// userland/apps/python/port/src-cpython/misc.c - and the Lua port (local queue
// 91) would have been the third. The owner rule is to extend the SHARED libc,
// so this is the shared one. The two existing copies are left alone
// DELIBERATELY: each defines setlocale() and localeconv() TOGETHER in its own
// object, so the linker satisfies both names from that object and never pulls
// libc.a's locale.o at all. Retiring them is a separate, testable change, not a
// side effect of adding this file.
//
// struct lconv is the FULL C89 member set on purpose. python's misc.c assigns
// int_curr_symbol, mon_grouping, p_sign_posn and friends; a three-field struct
// (which is what grep-gnu's private copy carries) would not compile there.
#ifndef LIBC_LOCALE_H
#define LIBC_LOCALE_H

// Same numbering as glibc AND as the existing private copy in
// userland/apps/grep-gnu/compat/locale.h, so nothing changes meaning if an app
// is ever migrated off its private header onto this one.
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
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
};

// Returns the name of the locale in force for 'category' after the call, or
// NULL if the requested locale is not available. Only "C" and "" are available.
char *setlocale(int category, const char *locale);

// Never NULL. Always describes the C locale.
struct lconv *localeconv(void);

#endif // LIBC_LOCALE_H
