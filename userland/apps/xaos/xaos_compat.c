/* xaos_compat.c - the small set of libc gaps this port needed, kept OUT of
 * userland/libc itself (per the port hazard note: do not widen a shared
 * libc header for one app). Every function here is `#pragma weak` so a
 * future real userland/libc definition silently wins instead of colliding
 * (nm -u was run against libc.a before landing; see README.md).
 *
 * NOTE: usleep() is NOT here. userland/libc/unistd.c already has a real
 * usleep() (userland/libc/unistd.h:59); util/timers.c's tl_sleep() is
 * gated on `#ifdef HAVE_USLEEP` (see aconfig.h), which is defined for
 * exactly that reason. Checked with nm against libc.a, not assumed - an
 * earlier draft of this file shipped a redundant weak usleep() before that
 * check was run.
 */
#include <stdio.h>
#include <math.h>

/* util/xshl.c, util/xstdio.c and util/xmenu.c all call the traditional C
 * `putc(c, f)` macro/function; userland/libc/stdio.h has fputc() but not
 * putc() (they are required to be equivalent by the C standard). */
#pragma weak putc
int putc(int c, FILE *f)
{
    return fputc(c, f);
}

/* engine/fractal.c's rotation code calls the GNU libm extension sincos()
 * (computes sin and cos of the same angle in one call); userland/libc/math.h
 * has sin() and cos() but not the combined form. A plain two-call
 * implementation is correctness-preserving, just without the one shared
 * range-reduction upstream glibc's version does as an optimisation. */
#pragma weak sincos
void sincos(double x, double *s, double *c)
{
    *s = sin(x);
    *c = cos(x);
}

/* PNG image/animation export (util/png.c, needs libpng+zlib) is EXCLUDED
 * from this port's Makefile - nothing on the render path needs it, and nc
 * mports has no libpng recipe yet. ui-hlp/render.c and ui-hlp/ui_helper.c
 * both treat a non-NULL return from writepng() as "failed, here is why";
 * this stub always fails cleanly rather than leaving the reference
 * undefined, so a user who reaches "Save image"/"Render animation" gets a
 * message instead of a link error or a crash. */
#pragma weak writepng
const char *writepng(const void *filename, const void *image)
{
    (void) filename;
    (void) image;
    return "PNG export is not built into this MayteraOS port of XaoS";
}
