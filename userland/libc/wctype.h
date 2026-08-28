// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// wctype.h - wide-character classification for MayteraOS userland
// (#745 local 97). Companion to wchar.h; read the locale note there first.
// Every predicate below is the byte predicate from <ctype.h> for wide values
// under 256 and false for everything above, because this OS has one locale
// and it is C.
#ifndef LIBC_WCTYPE_H
#define LIBC_WCTYPE_H

#include "wchar.h"

typedef int wctype_t;

int iswalnum(wint_t c);
int iswalpha(wint_t c);
int iswblank(wint_t c);
int iswcntrl(wint_t c);
int iswdigit(wint_t c);
int iswgraph(wint_t c);
int iswlower(wint_t c);
int iswprint(wint_t c);
int iswpunct(wint_t c);
int iswspace(wint_t c);
int iswupper(wint_t c);
int iswxdigit(wint_t c);

wint_t towlower(wint_t c);
wint_t towupper(wint_t c);

// wctype() returns 0 for a name this locale does not have, which is what
// POSIX requires and what TRE's bracket-expression parser tests for; a
// non-zero value is an opaque handle for iswctype().
wctype_t wctype(const char *name);
int      iswctype(wint_t c, wctype_t type);

#endif // LIBC_WCTYPE_H
