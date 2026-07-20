/* config.h - hand-written for porting genuine GNU grep 2.5.4 to MayteraOS.
 *
 * Target profile: C/POSIX locale, single-byte (ASCII/Latin-1).
 * No NLS/gettext, no multibyte/wchar, no iconv, no PCRE, no DOS/VMS.
 *
 * This replaces the config.h that grep's ./configure would normally generate,
 * tuned to exactly what the MayteraOS freestanding libc provides.
 */
#ifndef GREP_MAYTERA_CONFIG_H
#define GREP_MAYTERA_CONFIG_H

/* Package identity (grep.c prints these) */
#define PACKAGE "grep"
#define PACKAGE_NAME "grep"
#define PACKAGE_VERSION "2.5.4"
#define PACKAGE_STRING "grep 2.5.4"
#define PACKAGE_BUGREPORT "bug-grep@gnu.org"
#define VERSION "2.5.4"
#define GREP 1

/* ---- Standard headers we DO have (via libc, compat, or gcc freestanding) ---- */
#define STDC_HEADERS 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define HAVE_DIRENT_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_CTYPE_H 1
#define HAVE_ERRNO_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_ASSERT_H 1

/* ---- Library functions present (libc or compat shim) ---- */
#define HAVE_MEMCHR 1
#define HAVE_MEMMOVE 1
#define HAVE_STRERROR 1
#define HAVE_ISASCII 1
#define HAVE_ALLOCA 1
/* strtol/strtoul/strtoull/strtoumax are NOT in MayteraOS libc; gnulib's
 * lib/strtol.c etc. provide them.  The HAVE_DECL_* macros must be defined to 0
 * so those units declare the prototypes themselves (stdlib.h does not). */
#define HAVE_DECL_STRTOL 0
#define HAVE_DECL_STRTOUL 0
#define HAVE_DECL_STRTOULL 0
#define HAVE_DECL_STRTOUMAX 0
#define HAVE_DECL_STRTOIMAX 0
#define HAVE_GETPAGESIZE 1   /* compat/shim.c */
#define HAVE_ATEXIT 1        /* compat/shim.c */
#define HAVE_DUP2 1
#define HAVE_ISATTY 1
#define HAVE_VPRINTF 1       /* libc has vfprintf/vprintf; selects ANSI stdarg
                              * code paths (e.g. error.c VA_START) */

/* strerror_r is not provided by the MayteraOS libc.  HAVE_STRERROR_R must be
 * left *undefined* (not 0): error.c keys the reentrant path off
 * "#if defined HAVE_STRERROR_R", so any definition would select it.  Leaving it
 * undefined makes error.c use plain strerror(), which we do have.
 * HAVE_DECL_STRERROR_R=0 only suppresses the configure-time stringized check. */
#define HAVE_DECL_STRERROR_R 0

/* uintmax_t / intmax_t come from <stdint.h> (gcc freestanding) */
#define HAVE_UINTMAX_T 1
#define HAVE_UNSIGNED_LONG_LONG 1

/* We compile with a real ANSI/ISO C compiler (gcc), so the various PARAMS()
 * prototype-shim macros in dfa.h / regex.h / gnulib must expand to the real
 * argument list rather than the K&R empty "()". */
#define PROTOTYPES 1
#define __PROTOTYPES 1

/* MayteraOS malloc/realloc accept size 0 and behave per C89 (return a unique
 * pointer or NULL); gnulib's xmalloc/xrealloc just want these acknowledged. */
#define HAVE_DONE_WORKING_MALLOC_CHECK 1
#define HAVE_DONE_WORKING_REALLOC_CHECK 1

/* Use malloc inside the regex/dfa engines instead of alloca.
 * The MayteraOS user stack is small and fixed; deep backtracking via alloca
 * would overflow it. */
#define REGEX_MALLOC 1

/* Binary file handling is a no-op (POSIX text==binary) */
#define O_BINARY 0

/* The MayteraOS <sys/stat.h> defines S_ISREG/DIR/CHR/FIFO but not the block-
 * device / socket / symlink variants that grep.c tests for.  stat() never
 * reports those file types on MayteraOS, so these always evaluate false. */
#ifndef S_ISBLK
#define S_ISBLK(m)  (((m) & 0xF000) == 0x6000)
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(m) (((m) & 0xF000) == 0xC000)
#endif
#ifndef S_ISLNK
#define S_ISLNK(m)  (((m) & 0xF000) == 0xA000)
#endif

/* No <wchar.h> in this single-byte build.  A few gnulib units (quotearg.c)
 * still name mbstate_t inside branches that are dead when MB_CUR_MAX==1.
 * Provide a dummy type so those branches compile. */
#ifndef _MBSTATE_T_DEFINED
#define _MBSTATE_T_DEFINED
typedef struct { int __count; unsigned int __value; } mbstate_t;
#endif

/* ---- Explicitly NOT defined (single-byte, no NLS) ----
 * ENABLE_NLS, HAVE_LIBINTL_H, HAVE_GETTEXT, HAVE_DCGETTEXT, HAVE_LC_MESSAGES,
 * HAVE_LANGINFO_CODESET, HAVE_ICONV, HAVE_SETLOCALE,
 * HAVE_WCHAR_H, HAVE_WCTYPE_H, HAVE_WCTYPE, HAVE_ISWCTYPE, HAVE_MBRTOWC,
 * HAVE_MBRLEN, HAVE_WCRTOMB, HAVE_WCSCOLL, HAVE_BTOWC, MBS_SUPPORT,
 * HAVE_DOS_FILE_NAMES, HAVE_DOS_FILE_CONTENTS, HAVE_LIBPCRE, HAVE_MMAP.
 */

#endif /* GREP_MAYTERA_CONFIG_H */
