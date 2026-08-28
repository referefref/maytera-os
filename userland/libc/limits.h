// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// limits.h - the C standard implementation limits, for the LP64 x86-64
// userland target.
//
// WHY THIS FILE EXISTS, AND WHY IT IS IN THE SHARED LIBC AND NOT IN A PORT.
//
// Userland compiles -nostdinc with only the freestanding gcc headers on
// -isystem. gcc ships a limits.h, but it is not self-contained: it chains to
// syslimits.h, which does `#include_next <limits.h>` "to recurse down to the
// real one". Under -nostdinc there is no real one, so any translation unit
// that includes <limits.h> fails with
//
//     no include path in which to search for limits.h
//
// Six userland ports and three kernel media decoders each answered that with a
// private copy: userland/apps/{doom,rogue,vi,grep-gnu,openarena}, the NetSurf
// shim tree, and kernel/media/{libmad,opus,tremor}. Nine copies in three
// versions of the same twenty lines, with one of them (doom) defining only
// INT_MAX/INT_MIN. That duplication is one of the specific findings in
// docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md section 2.8 and it is
// exactly what the owner's standing rule forbids: if a port needs something the
// libc lacks, EXTEND THE SHARED LIBC, do not fork a private copy.
//
// So this is the shared one. It was added when the mports zlib recipe (local
// queue 91) hit the same wall, and it is the answer for every port after it.
//
// THE PRE-EXISTING PRIVATE COPIES ARE NOT DELETED HERE, deliberately. Five of
// the six userland ones are reached through a -Icompat that already precedes
// -I$(LIBC_DIR), so they still win their own builds and nothing about them
// changes. Removing them is a separate, individually-verifiable change per
// port, not a drive-by in the pass that introduces the shared header. The one
// exception is userland/apps/openarena, whose CFLAGS put -I$(LIBC_DIR) FIRST:
// this header therefore shadows openarena/compat/limits.h, whose only material
// difference is PATH_MAX (4096 there, 1024 here). 1024 is the correct value,
// see below.

#ifndef _MAYTERA_LIMITS_H
#define _MAYTERA_LIMITS_H

#define CHAR_BIT     8
#define MB_LEN_MAX   16

#define SCHAR_MIN    (-128)
#define SCHAR_MAX    127
#define UCHAR_MAX    255

// Plain char is signed on x86-64 System V, but honour the flag rather than
// assuming it, so -funsigned-char does not silently produce wrong limits.
#ifdef __CHAR_UNSIGNED__
#define CHAR_MIN     0
#define CHAR_MAX     UCHAR_MAX
#else
#define CHAR_MIN     SCHAR_MIN
#define CHAR_MAX     SCHAR_MAX
#endif

#define SHRT_MIN     (-32768)
#define SHRT_MAX     32767
#define USHRT_MAX    65535

#define INT_MAX      2147483647
#define INT_MIN      (-INT_MAX - 1)
#define UINT_MAX     4294967295U

#define LONG_MAX     9223372036854775807L
#define LONG_MIN     (-LONG_MAX - 1L)
#define ULONG_MAX    18446744073709551615UL

#define LLONG_MAX    9223372036854775807LL
#define LLONG_MIN    (-LLONG_MAX - 1LL)
#define ULLONG_MAX   18446744073709551615ULL

// POSIX, and used by enough ported code to be worth having here.
#ifndef SSIZE_MAX
#define SSIZE_MAX    LONG_MAX
#endif

// PATH_MAX and NAME_MAX are POSIX additions to <limits.h>. They are guarded,
// and they carry THE SAME VALUES as sys/param.h, which is where this libc
// already defined them: PATH_MAX is 1024 because that is SC_PATH_MAX in the
// kernel, so a longer path cannot be passed to open()/stat() anyway, and a
// larger constant here would only invite a caller to build a path the syscall
// layer will refuse. Two headers in one libc must not disagree about a limit.
#ifndef PATH_MAX
#define PATH_MAX     1024
#endif
#ifndef NAME_MAX
#define NAME_MAX     255
#endif

// POSIX regex limits, added for the musl-regex port (#745 local 97). Both
// carry musl's own values, which are the POSIX minimums. RE_DUP_MAX is the one
// to know about: it is what the regex engine enforces on {n,m}, and the port's
// gnuregex.h uses the same number as its fallback, so grep's DFA prefilter and
// the matcher cannot disagree about which intervals are legal.
#ifndef CHARCLASS_NAME_MAX
#define CHARCLASS_NAME_MAX 14
#endif
#ifndef RE_DUP_MAX
#define RE_DUP_MAX   255
#endif

#endif // _MAYTERA_LIMITS_H
