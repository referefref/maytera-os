// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// wchar.h - the minimum wide-character surface MayteraOS userland supports
// (#745 local 97). Added for the musl-regex port, whose TRE engine is written
// against wchar_t; kept deliberately small.
//
// THE LOCALE MODEL, STATED SO NOBODY HAS TO GUESS. MayteraOS has exactly one
// locale, the C locale, and this header commits to a BYTE-TRANSPARENT mapping:
// every byte 0x00..0xFF is one character whose wide value equals the byte.
// mbtowc() therefore never fails, never consumes more than one byte, and
// never merges a byte pair into one code point. That is the same behaviour
// GNU regex has in the C locale, which is what the engine this replaces was
// running under, so a regex compiled here matches the same bytes it did
// before. It is NOT UTF-8: a two-byte UTF-8 sequence is two characters here.
// If MayteraOS ever grows real locales, this is the file that has to change,
// and every consumer that assumed one byte per character has to be re-read.
#ifndef LIBC_WCHAR_H
#define LIBC_WCHAR_H

#include <stddef.h>

#ifndef __wchar_t_defined
typedef __WCHAR_TYPE__ wchar_t;
#define __wchar_t_defined 1
#endif

#ifndef __wint_t_defined
typedef __WINT_TYPE__ wint_t;
#define __wint_t_defined 1
#endif

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

#ifndef WCHAR_MIN
#define WCHAR_MIN 0
#endif
#ifndef WCHAR_MAX
#define WCHAR_MAX 0x7fffffff
#endif

// One byte per character, always. See the locale note above.
#ifndef MB_CUR_MAX
#define MB_CUR_MAX ((size_t)1)
#endif

typedef struct { unsigned __opaque; } mbstate_t;

int    mbtowc(wchar_t *pwc, const char *s, size_t n);
size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *st);
int    wctomb(char *s, wchar_t wc);
int    mblen(const char *s, size_t n);
size_t wcslen(const wchar_t *s);
int    wcscmp(const wchar_t *a, const wchar_t *b);
int    wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcscpy(wchar_t *d, const wchar_t *s);

#endif // LIBC_WCHAR_H
