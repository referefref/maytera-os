/* err.h - MayteraOS shim for the four BSD err(3) family functions.
 *
 * OUR FILE, not upstream. userland/libc has no err.h (it is a BSD, not a
 * POSIX/ISO, interface); this recipe's build.sh puts this directory on the
 * -I search path ahead of everything else so the game's own
 * `#include <err.h>` resolves here instead of failing the compile. The
 * definitions live in bsdgame_compat.c, compiled as an extra source
 * alongside the upstream sources (see build.sh; mports.sh's build=objects
 * `sources=` list can only name files already inside the unpacked upstream
 * tree, so a port that needs one own file uses build=script, same as the
 * tcc and sqlite ports' maytera_compat.c / maytera_vfs.c).
 */
#ifndef MAYTERA_BSDGAMES_ERR_H
#define MAYTERA_BSDGAMES_ERR_H

void err(int eval, const char *fmt, ...) __attribute__((noreturn));
void errx(int eval, const char *fmt, ...) __attribute__((noreturn));
void warn(const char *fmt, ...);
void warnx(const char *fmt, ...);

#endif
