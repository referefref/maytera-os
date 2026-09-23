#!/usr/bin/env bash
# ports/moonbuggy/build.sh - the build=script step for the moon-buggy port
# (Tier 3 item #17 of docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md).
#
# WHY build=script AND NOT build=objects. Upstream is autoconf, and its build
# GENERATES three headers a static `sources=` list cannot express (they are not
# in the tarball, so mports could never verify them "present in the unpacked
# tree"):
#   * config.h  - normally emitted by ./configure. We write a small, honest one
#                 directly (below), recording the MayteraOS platform facts.
#   * copying.h - the GPL text as a C string array, made by upstream's own
#                 `sed -f text2c.sed COPYING` (a BUILT_SOURCES rule in
#                 Makefile.am). pager.c includes it for the `c' copyright view.
#   * buggy.h   - the buggy/crash sprites as a C array, made by upstream's own
#                 `sed -n -f img.sed car.img` (also BUILT_SOURCES). buggy.c
#                 includes it; without it there is no buggy to draw.
# We run upstream's two seds verbatim, so the sprites and licence text are
# exactly upstream's, not a frozen copy in our patch series.
#
# Contract from mports.sh: cwd = the unpacked, PATCHED srcdir; MPORTS_CFLAGS
# carries the userland cross-build flags PLUS the ncurses out/include path (this
# port's `needs=`) PLUS this port's cflags=; MPORTS_SRC is the unpacked srcdir;
# MPORTS_OUT is the install tree. After we return, mports looks for the archive
# at $(dirname MPORTS_SRC)/libmoonbugy.a and proves every symbol promised in
# PORT is defined in it.
set -euo pipefail

: "${MPORTS_CFLAGS:?mports must pass MPORTS_CFLAGS}"
: "${MPORTS_SRC:?mports must pass MPORTS_SRC}"

WORKDIR="$(dirname "$MPORTS_SRC")"   # where mports expects libmoonbugy.a
LIB="$WORKDIR/libmoonbugy.a"

# ---------------------------------------------------------------------------
# 1. config.h. This port drives the sources directly instead of running
#    upstream's configure, so this replaces the config.h configure would emit.
#    Only HAVE_* that are actually TRUE on this platform are defined, so each
#    source #ifdef takes the branch that compiles and runs here. See the port's
#    three patches for HAVE_TERMIOS_H / HAVE_SYS_SELECT_H / HAVE_SETREUID being
#    deliberately absent.
# ---------------------------------------------------------------------------
echo "moonbuggy/build.sh: writing config.h"
cat > config.h <<'EOF'
#ifndef MB_MAYTERA_CONFIG_H
#define MB_MAYTERA_CONFIG_H
#define PACKAGE "moon-buggy"
#define PACKAGE_NAME "moon-buggy"
#define PACKAGE_TARNAME "moon-buggy"
#define PACKAGE_VERSION "1.0.51"
#define PACKAGE_STRING "moon-buggy 1.0.51"
#define PACKAGE_BUGREPORT "voss@seehuhn.de"
#define VERSION "1.0.51"
#define STDC_HEADERS 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_ERRNO_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_MEMORY_H 1
/* ncurses: this tree's ncurses port installs curses.h (ports/ncurses/PORT). */
#define HAVE_CURSES_H 1
#define CURSES_HEADER <curses.h>
/* getopt_long lives in libc's getopt.h. */
#define HAVE_GETOPT_H 1
#define HAVE_GETOPT_LONG 1
/* setlocale(LC_CTYPE,"") is a harmless no-op on the single C locale. */
#define HAVE_LOCALE_H 1
#define HAVE_SETLOCALE 1
/* ftruncate() exists (unistd.h); write_data() trims the score file with it. */
#define HAVE_FTRUNCATE 1
/* signal handlers return void. */
#define RETSIGTYPE void
/* Take persona.c's compile-time saved-ids branch rather than a
 * sysconf(_SC_SAVED_IDS) this libc does not define; never reached at runtime
 * (uid==euid, single user). */
#ifndef _POSIX_SAVED_IDS
#define _POSIX_SAVED_IDS 1
#endif
/* Highscore directory: a writable absolute path on the MayteraOS image. The
 * game creates SCORE_DIR/mbscore on first run (highscore.c method 2) and reads
 * it thereafter. hpath.c is the only consumer. */
#define SCORE_DIR "/HOME"
#endif /* MB_MAYTERA_CONFIG_H */
EOF

# ---------------------------------------------------------------------------
# 2. Upstream's two BUILT_SOURCES seds, verbatim from Makefile.am.
# ---------------------------------------------------------------------------
echo "moonbuggy/build.sh: generating copying.h and buggy.h via upstream seds"
sed -f text2c.sed COPYING > copying.h
sed -n -f img.sed car.img  > buggy.h
test -s copying.h || { echo "moonbuggy/build.sh: copying.h came out empty" >&2; exit 1; }
test -s buggy.h   || { echo "moonbuggy/build.sh: buggy.h came out empty"   >&2; exit 1; }

# ---------------------------------------------------------------------------
# 3. Compile the 26 translation units into libmoonbugy.a. This is exactly
#    moon_buggy_SOURCES from Makefile.am minus the .h files. -DHAVE_CONFIG_H -I.
#    makes each `#include <config.h>` resolve to the one we wrote (MPORTS_CFLAGS
#    is -nostdinc, so -I. is how cwd gets on the angle-bracket search path).
#    -w because upstream trips -Wall/-Wextra warnings that are not ours to fix;
#    this port ships the sources unaltered but for the three documented patches.
# ---------------------------------------------------------------------------
SRCS="main mode title pager game level ground buggy laser meteor highscore \
realname queue vclock date persona signal keyboard terminal cursor random \
error xmalloc xstrdup hpath"

OBJS=()
echo "moonbuggy/build.sh: cross-compiling the moon-buggy sources"
for f in $SRCS; do
  [ -f "$f.c" ] || { echo "moonbuggy/build.sh: missing source $f.c" >&2; exit 1; }
  gcc $MPORTS_CFLAGS -DHAVE_CONFIG_H -I. -w -c "$f.c" -o "$WORKDIR/mb_$f.o"
  OBJS+=("$WORKDIR/mb_$f.o")
done

echo "moonbuggy/build.sh: archiving libmoonbugy.a"
rm -f "$LIB"
ar rcs "$LIB" "${OBJS[@]}"

echo "moonbuggy/build.sh: done -> $LIB"
