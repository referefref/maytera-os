/*
  SDL_config.h - MayteraOS platform configuration for the SDL 1.2 backend
  (docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md Tier 2 #7, task #745).

  This file REPLACES sdl12-compat's own generic SDL_config.h (which guesses
  "everything unix-like probably has this" and reaches for <iconv.h> and an
  X11 probe). Both are wrong for MayteraOS: there is no iconv.h and no X11.
  This file states exactly, and only, what userland/libc actually provides,
  each one checked against a real header in userland/libc/ before being
  turned on (see userland/libSDL/README.md "Header provenance"). An HAVE_*
  that is not backed by a real symbol is a promise SDL_stdinc.h will believe
  and a linker error will collect on later; stating the true set here instead
  of copying sdl12-compat's guess is the whole point of this file.

  No em-dashes (repo writing-style rule).
*/
#ifndef _SDL_config_h
#define _SDL_config_h

#include "SDL_platform.h"

#include <stdint.h>
#define HAVE_STDINT_H 1
#define SDL_HAS_64BIT_TYPE 1

#include <stdarg.h>
#define HAVE_STDARG_H 1
#define HAVE_STDDEF_H 1

/* userland/libc ships stdio.h, stdlib.h, string.h, sys/types.h, ctype.h,
 * math.h with real implementations (already load-bearing for the zlib,
 * PCRE2, Lua and SQLite mports). */
#define HAVE_LIBC 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_STDIO_H 1
#define STDC_HEADERS 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_CTYPE_H 1
#define HAVE_MATH_H 1

/* userland/libc/alloca.h is a real compiler-builtin wrapper. */
#define HAVE_ALLOCA_H 1
#define HAVE_ALLOCA 1

/* Deliberately NOT defined: HAVE_ICONV / HAVE_ICONV_H. There is no iconv on
 * this OS. SDL_stdinc.h's fallback (a real SDL_iconv_open/SDL_iconv/
 * SDL_iconv_close implementation) is what userland/libSDL/src/sdl_misc.c
 * supplies instead, and it is honest about failing every conversion rather
 * than silently returning uncoverted bytes. */

/* No X11, no Windows, no dlopen(): SDL_VIDEO_DRIVER_X11 and friends stay
 * undefined. HAVE_GETENV/HAVE_PUTENV: userland/libc/stdlib.h declares both,
 * see userland/libSDL/README.md for the real (non-fake) behaviour crt0's
 * argv-only entry gives them. */
#define HAVE_GETENV 1
#define HAVE_PUTENV 1
#define HAVE_UNSETENV 1

#define HAVE_MALLOC 1
#define HAVE_CALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FREE 1
#define HAVE_QSORT 1
#define HAVE_ABS 1
#define HAVE_MEMSET 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMCMP 1
#define HAVE_STRLEN 1
#define HAVE_STRDUP 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STRTOL 1
#define HAVE_STRTOUL 1
#define HAVE_STRTOLL 1
#define HAVE_STRTOULL 1
#define HAVE_STRTOD 1
#define HAVE_STRCMP 1
#define HAVE_STRNCMP 1
#define HAVE_STRCASECMP 1
#define HAVE_STRNCASECMP 1
#define HAVE_SSCANF 1
#define HAVE_SNPRINTF 1
#define HAVE_VSNPRINTF 1

/* Real setjmp/longjmp exist (userland/libc/setjmp.h + setjmp.asm), used
 * nowhere in this backend today but harmless to state accurately. */
#define HAVE_SETJMP 1

#endif /* _SDL_config_h */
