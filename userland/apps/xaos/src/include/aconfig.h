/* aconfig.h - MayteraOS port of XaoS 3.6 (#745, docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md
 * Tier 3 #16). Hand-written in place of the autoconf-generated aconfig.h,
 * mirroring src/include/config/aconfig.std (the documented fallback for
 * platforms with no configure) but pared to what MayteraOS userland/libc
 * actually provides. Every define below was checked against a real header in
 * userland/libc, not assumed; see userland/apps/xaos/README.md for the
 * per-symbol rationale.
 */
#ifndef ACONFIG_H
#define ACONFIG_H

#define DATAPATH "/"

/* No <sys/time.h> in userland/libc: time.h has clock()/nanosleep()/
 * clock_gettime() instead. Leaving HAVE_SYS_TIME_H and HAVE_GETTIMEOFDAY
 * undefined routes util/timers.c to plain <time.h> and the USE_CLOCK path
 * (see config.h), which is what CLOCKS_PER_SEC=250 (kernel tick rate) in
 * userland/libc/time.h is there to drive. */
/* #undef HAVE_SYS_TIME_H */
/* #undef HAVE_GETTIMEOFDAY */
#define HAVE_UNISTD_H 1
#define HAVE_LIMITS_H 1
#define STDC_HEADERS 1

/* userland/apps/xaos/xaos_compat.c supplies usleep() (nanosleep()-backed;
 * userland/libc has no usleep of its own) so tl_sleep() is real, not the
 * documented busy-spin fallback. */
#define HAVE_USLEEP 1

/* long double: NOT defined. userland/libc/math.h has no long-double (*l)
 * function family (fabsl/sqrtl/powl/...), and grep across the whole XaoS
 * 3.6 tree found FPOINT_TYPE's only use is `typedef FPOINT_TYPE number_t`
 * (src/include/fconfig.h) plus one HAVE_LONG_DOUBLE branch in
 * src/include/gccbuild.h (myabs()), so plain `double` throughout is a
 * correctness-preserving substitution, not a precision hack. See config.h. */
/* #undef HAVE_LONG_DOUBLE */

#define SIZEOF_INT 4
#define SIZEOF_SHORT 2
/* x86-64 LP64: long is 8 bytes here, unlike the i386-era aconfig.std this is
 * based on (which hardcoded 4). Checked with the build compiler, not copied. */
#define SIZEOF_LONG 8

/* No libpng in this tree (util/png.c is EXCLUDED from the Makefile SRCS for
 * exactly this reason) so image-catalog PNG export/import stays off. Nothing
 * on the render path needs it. */
/* #undef USE_PNG */

/* No finite(): userland/libc/math.h has the C99 isfinite() macro instead,
 * and grep found zero XaoS callers of finite() outside the HAVE_FINITE
 * ifdef itself. */
/* #undef HAVE_FINITE */

/* No aalib, allegro, ncurses mouse mask, terminal attrs, uclock, setitimer:
 * this is a real preemptively-scheduled OS with a normal windowed
 * framebuffer, none of those DOS/curses-era compat shims are relevant. */
/* #undef AA_DRIVER */
/* #undef USE_ALLEGRO */
/* #undef HAVE_MOUSEMASK */
/* #undef HAVE_TERMATTRS */
/* #undef HAVE_UCLOCK */
/* #undef HAVE_SETITIMER */

/* Our driver, wired into src/ui/drivers.c same as every other *_DRIVER. */
#define MAYTERA_DRIVER 1

#endif /* ACONFIG_H */
