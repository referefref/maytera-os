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

// #portstack: these are provided by libc for general use, but a port that
// ships its own (e.g. the CPython wsupp compat layer) must win at its own
// link. Marking them weak lets a strong app/port definition override, while
// libc still serves every app that has none. Fixes CPython relink dup-symbols.
#pragma weak iswalnum
#pragma weak iswalpha
#pragma weak iswblank
#pragma weak iswcntrl
#pragma weak iswctype
#pragma weak iswdigit
#pragma weak iswgraph
#pragma weak iswlower
#pragma weak iswprint
#pragma weak iswpunct
#pragma weak iswspace
#pragma weak iswupper
#pragma weak iswxdigit
#pragma weak mbrtowc
#pragma weak mbsrtowcs
#pragma weak mbstowcs
#pragma weak towlower
#pragma weak towupper
#pragma weak wcrtomb
#pragma weak wcschr
#pragma weak wcscmp
#pragma weak wcscpy
#pragma weak wcscspn
#pragma weak wcslen
#pragma weak wcsncat
#pragma weak wcsncmp
#pragma weak wcsncpy
#pragma weak wcsstr
#pragma weak wcstol
#pragma weak wcstombs
#pragma weak wctob
#pragma weak wctype
#pragma weak wmemcpy


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

// ---------------------------------------------------------------------------
// Wide string/conversion helpers added for the libedit port (#745). Every one
// obeys the byte-transparent C-locale model from wchar.h: a wchar_t in
// 0x00..0xFF is exactly the byte, and nothing above 0xFF is representable as a
// multibyte character. These were absent when musl-regex first needed a wide
// layer (it used only the classification set above); libedit is the first
// consumer of the string/conversion half, so it is added here once, not forked.
// ---------------------------------------------------------------------------

size_t wcstombs(char *dst, const wchar_t *src, size_t n)
{
	size_t i = 0;
	for (; i < n; i++) {
		wchar_t wc = src[i];
		if (dst)
			dst[i] = (char)(unsigned char)wc;
		if (wc == 0)
			return i;   // POSIX: the terminating null is not counted
	}
	return i;
}

size_t mbstowcs(wchar_t *dst, const char *src, size_t n)
{
	size_t i = 0;
	for (; i < n; i++) {
		unsigned char b = (unsigned char)src[i];
		if (dst)
			dst[i] = (wchar_t)b;
		if (b == 0)
			return i;   // terminating null not counted
	}
	return i;
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *st)
{
	(void)st;
	// NULL s: return the length of the reset sequence, which for a
	// stateless encoding is that of L0, i.e. 1.
	if (!s)
		return 1;
	if (wc < 0 || wc > 0xff)
		return (size_t)-1;
	*s = (char)(unsigned char)wc;
	return 1;
}

int wctob(wint_t c)
{
	if (c == WEOF || (unsigned long)c > 0xff)
		return -1;   // EOF
	return (int)(unsigned char)c;
}

int wcwidth(wchar_t c)
{
	if (c == 0)
		return 0;
	if ((unsigned long)c > 0xff)
		return -1;
	return isprint((int)c) ? 1 : -1;
}

wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		d[i] = s[i];
	return d;
}

wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n)
{
	size_t i = 0;
	for (; i < n && s[i]; i++)
		d[i] = s[i];
	for (; i < n; i++)
		d[i] = 0;
	return d;
}

wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n)
{
	wchar_t *r = d;
	while (*d)
		d++;
	size_t i = 0;
	for (; i < n && s[i]; i++)
		d[i] = s[i];
	d[i] = 0;
	return r;
}

size_t wcscspn(const wchar_t *s, const wchar_t *reject)
{
	const wchar_t *p = s;
	for (; *p; p++) {
		const wchar_t *r;
		for (r = reject; *r; r++)
			if (*p == *r)
				return (size_t)(p - s);
	}
	return (size_t)(p - s);
}

wchar_t *wcsstr(const wchar_t *hay, const wchar_t *needle)
{
	if (!*needle)
		return (wchar_t *)hay;
	for (; *hay; hay++) {
		const wchar_t *h = hay, *n = needle;
		while (*h && *n && *h == *n) {
			h++;
			n++;
		}
		if (!*n)
			return (wchar_t *)hay;
	}
	return NULL;
}


// wcstol: the byte-transparent wide twin of strtol. Written directly rather
// than by narrowing-then-strtol so that endptr lands inside the wide string.
long wcstol(const wchar_t *nptr, wchar_t **endptr, int base)
{
	const wchar_t *p = nptr;
	while (iswspace((wint_t)*p))
		p++;
	int neg = 0;
	if (*p == (wchar_t)'+' || *p == (wchar_t)'-') {
		neg = (*p == (wchar_t)'-');
		p++;
	}
	if ((base == 0 || base == 16) && p[0] == (wchar_t)'0' &&
	    (p[1] == (wchar_t)'x' || p[1] == (wchar_t)'X')) {
		p += 2;
		base = 16;
	} else if (base == 0 && p[0] == (wchar_t)'0') {
		base = 8;
	} else if (base == 0) {
		base = 10;
	}
	long acc = 0;
	int any = 0;
	for (;; p++) {
		int d;
		wchar_t c = *p;
		if (c >= (wchar_t)'0' && c <= (wchar_t)'9')
			d = (int)(c - (wchar_t)'0');
		else if (c >= (wchar_t)'a' && c <= (wchar_t)'z')
			d = (int)(c - (wchar_t)'a') + 10;
		else if (c >= (wchar_t)'A' && c <= (wchar_t)'Z')
			d = (int)(c - (wchar_t)'A') + 10;
		else
			break;
		if (d >= base)
			break;
		acc = acc * base + d;
		any = 1;
	}
	if (endptr)
		*endptr = (wchar_t *)(any ? p : nptr);
	return neg ? -acc : acc;
}

// mbsrtowcs: byte-transparent restartable multibyte->wide conversion, added
// for the libedit port (#745). One byte is one wide character (see wchar.h).
size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *st)
{
	(void)st;
	const char *s = *src;
	size_t i = 0;
	for (;;) {
		if (dst && i >= len)
			break;
		unsigned char b = (unsigned char)*s;
		if (dst)
			dst[i] = (wchar_t)b;
		if (b == 0) {
			if (dst)
				*src = (const char *)0;
			return i;   // terminating null not counted
		}
		i++;
		s++;
	}
	*src = s;   // dst full and not at end: leave *src at the next byte
	return i;
}
