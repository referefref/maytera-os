// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// wctype.c - the C-locale wide-character layer (#745 local 97).
//
// Read the locale commitment in wchar.h before changing anything here. One
// byte is one character; the wide value of a byte IS the byte. mbtowc()
// therefore cannot fail, which matters to the TRE engine in the musl-regex
// port: its scanner treats a negative mbtowc() return as "no match" and would
// silently stop matching mid-subject on any byte a stricter decoder rejected.
#include "wchar.h"
#include "wctype.h"
#include "ctype.h"
#include <stddef.h>

int mbtowc(wchar_t *pwc, const char *s, size_t n)
{
	// POSIX: a NULL s means "is the encoding state-dependent"; ours is not.
	if (!s)
		return 0;
	if (n == 0)
		return -1;
	unsigned char b = (unsigned char)*s;
	if (pwc)
		*pwc = (wchar_t)b;
	// Zero for the null character, one for everything else. The zero return
	// is why callers that walk a buffer must add one byte themselves.
	return b ? 1 : 0;
}

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *st)
{
	(void)st;
	if (!s)
		return 0;
	if (n == 0)
		return (size_t)-2;
	unsigned char b = (unsigned char)*s;
	if (pwc)
		*pwc = (wchar_t)b;
	return b ? 1 : 0;
}

int wctomb(char *s, wchar_t wc)
{
	if (!s)
		return 0;
	if (wc < 0 || wc > 0xff)
		return -1;
	*s = (char)(unsigned char)wc;
	return 1;
}

int mblen(const char *s, size_t n)
{
	return mbtowc(NULL, s, n);
}

size_t wcslen(const wchar_t *s)
{
	const wchar_t *p = s;
	while (*p)
		p++;
	return (size_t)(p - s);
}

int wcscmp(const wchar_t *a, const wchar_t *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (*a > *b) - (*a < *b);
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
	while (n && *a && *a == *b) {
		a++;
		b++;
		n--;
	}
	if (!n)
		return 0;
	return (*a > *b) - (*a < *b);
}

wchar_t *wcschr(const wchar_t *s, wchar_t c)
{
	for (; *s; s++)
		if (*s == c)
			return (wchar_t *)s;
	return c ? NULL : (wchar_t *)s;
}

wchar_t *wcscpy(wchar_t *d, const wchar_t *s)
{
	wchar_t *r = d;
	while ((*d++ = *s++))
		;
	return r;
}

// ---------------------------------------------------------------------------
// Classification. Above 0xff nothing is anything: this locale has 256
// characters and saying otherwise would be inventing a character set.
// ---------------------------------------------------------------------------
#define WIDE_PRED(name, byte_pred)             \
	int name(wint_t c)                     \
	{                                      \
		if ((unsigned long)c > 0xff)   \
			return 0;              \
		return byte_pred((int)c);      \
	}

WIDE_PRED(iswalnum, isalnum)
WIDE_PRED(iswalpha, isalpha)
WIDE_PRED(iswblank, isblank)
WIDE_PRED(iswcntrl, iscntrl)
WIDE_PRED(iswdigit, isdigit)
WIDE_PRED(iswgraph, isgraph)
WIDE_PRED(iswlower, islower)
WIDE_PRED(iswprint, isprint)
WIDE_PRED(iswpunct, ispunct)
WIDE_PRED(iswspace, isspace)
WIDE_PRED(iswupper, isupper)
WIDE_PRED(iswxdigit, isxdigit)

wint_t towlower(wint_t c)
{
	if ((unsigned long)c > 0xff)
		return c;
	return (wint_t)tolower((int)c);
}

wint_t towupper(wint_t c)
{
	if ((unsigned long)c > 0xff)
		return c;
	return (wint_t)toupper((int)c);
}

// The twelve POSIX class names, in the order the standard lists them. The
// handle is the index plus one so that zero stays the "no such class" answer.
static const char *const g_class_names[] = {
	"alnum", "alpha", "blank", "cntrl", "digit", "graph",
	"lower", "print", "punct", "space", "upper", "xdigit",
};

static int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

wctype_t wctype(const char *name)
{
	if (!name)
		return 0;
	for (unsigned i = 0; i < sizeof(g_class_names) / sizeof(g_class_names[0]); i++)
		if (str_eq(name, g_class_names[i]))
			return (wctype_t)(i + 1);
	return 0;
}

int iswctype(wint_t c, wctype_t type)
{
	switch (type) {
	case 1:  return iswalnum(c);
	case 2:  return iswalpha(c);
	case 3:  return iswblank(c);
	case 4:  return iswcntrl(c);
	case 5:  return iswdigit(c);
	case 6:  return iswgraph(c);
	case 7:  return iswlower(c);
	case 8:  return iswprint(c);
	case 9:  return iswpunct(c);
	case 10: return iswspace(c);
	case 11: return iswupper(c);
	case 12: return iswxdigit(c);
	default: return 0;
	}
}
