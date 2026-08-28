// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// locale.c - the C locale, which is the only locale MayteraOS has.
//
// SEE locale.h FOR WHY THIS IS A SHARED FILE. Keep setlocale() and localeconv()
// TOGETHER in this one translation unit and define NOTHING ELSE here. That is
// load-bearing: userland/apps/grep-gnu/compat/shim.c and
// userland/apps/python/port/src-cpython/misc.c each still define BOTH names in
// one of their own objects, so the linker never needs a symbol from this file
// when building those two apps and never pulls it out of libc.a. Add a third
// unrelated function here and the day some app references it, this object gets
// pulled in and collides with their definitions.
#include "locale.h"
#include "string.h"

// CHAR_MAX in the monetary fields is the C-standard way of saying "this value
// is not available in this locale", which is exactly true here.
#define LC_UNAVAILABLE ((char)127)

static char g_dot[]   = ".";
static char g_empty[] = "";

static struct lconv g_c_locale = {
    g_dot,          // decimal_point
    g_empty,        // thousands_sep
    g_empty,        // grouping
    g_empty,        // int_curr_symbol
    g_empty,        // currency_symbol
    g_empty,        // mon_decimal_point
    g_empty,        // mon_thousands_sep
    g_empty,        // mon_grouping
    g_empty,        // positive_sign
    g_empty,        // negative_sign
    LC_UNAVAILABLE, // int_frac_digits
    LC_UNAVAILABLE, // frac_digits
    LC_UNAVAILABLE, // p_cs_precedes
    LC_UNAVAILABLE, // p_sep_by_space
    LC_UNAVAILABLE, // n_cs_precedes
    LC_UNAVAILABLE, // n_sep_by_space
    LC_UNAVAILABLE, // p_sign_posn
    LC_UNAVAILABLE  // n_sign_posn
};

static char g_name_C[] = "C";

char *setlocale(int category, const char *locale) {
    (void)category;                 // every category is the C locale
    if (locale == 0) return g_name_C;   // query: report what is in force
    if (locale[0] == '\0') return g_name_C;  // "" = the implementation default
    if (strcmp(locale, "C") == 0) return g_name_C;
    if (strcmp(locale, "POSIX") == 0) return g_name_C;
    // Anything else genuinely is not available. Saying so is the point: a
    // caller that asks for a real locale and is silently given "C" will format
    // numbers and compare strings wrongly with no way to find out.
    return 0;
}

struct lconv *localeconv(void) {
    return &g_c_locale;
}
