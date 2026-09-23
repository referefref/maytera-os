/* config.h - MayteraOS port of XaoS 3.6 (#745,
 * docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md Tier 3 #16). There is no
 * ./configure in this build (see userland/apps/xaos/README.md); this file
 * replaces the `cp include/config/config.autoconf include/config.h` step the
 * upstream top-level Makefile does after a real ./configure run. Modelled on
 * that generated config.autoconf (checked into the upstream tree as a
 * worked example of a real Linux build), NOT on the older config.std
 * fallback: config.std is missing INLINE/CONST, which real engine code
 * (src/engine/formulas.c) needs unconditionally.
 */
#ifndef CONFIG_H
#define CONFIG_H
#define HOMEDIR
#define CONFIGFILE "xaos.cfg"

#define FPOINT_TYPE long double
#include <aconfig.h>
#define USE_STDIO
/* No HAVE_LONG_DOUBLE (see aconfig.h): userland/libc/math.h has no
 * long-double function family, so this drops FPOINT_TYPE to plain double,
 * same as config.autoconf's own "if !defined(HAVE_LONG_DOUBLE)" fallback. */
#if !defined(HAVE_LONG_DOUBLE)
#undef FPOINT_TYPE
#define FPOINT_TYPE double
#endif
#define CONST const
#define INLINE inline

#include <gccaccel.h>

/* util/timers.c timing path: no gettimeofday/uclock/allegro (see aconfig.h),
 * so select clock()+CLOCKS_PER_SEC (userland/libc/time.h defines
 * CLOCKS_PER_SEC 250, the real kernel tick rate). */
#define USE_CLOCK

#ifndef HAVE_LIMITS_H
#define INT_MAX 2147483647
#endif

/* MayteraOS driver (src/ui/ui-drv/maytera): a true-colour bitmap-blit
 * window, same class as upstream's X11/GTK/COCOA drivers just above this in
 * config.autoconf, so it gets the same feature flags they do. */
#ifdef MAYTERA_DRIVER
#define SFIXEDCOLOR
#define STRUECOLOR
#define STRUECOLOR16
#define STRUECOLOR24
#define SMBITMAPS
#define SLBITMAPS
#endif

#endif /* CONFIG_H */
