#!/usr/bin/env bash
# ports/quickjs/build.sh - the build=script step for the QuickJS-ng port
# (Tier 2 item #8 of docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md).
#
# WHY build=script AND NOT build=objects. The archive must contain BOTH the four
# upstream engine translation units AND one MayteraOS-specific file,
# maytera_compat.c, which lives beside this recipe (OUR code, tracked in git),
# not in the tarball. A `sources=` list is verified present IN THE UNPACKED
# TARBALL, so maytera_compat.c could never appear there. Same structural reason
# as the sqlite port's maytera_vfs.c.
#
# WHAT maytera_compat.c IS. Exactly one symbol: a WEAK pthread_condattr_setclock.
# cutils.h's js_cond_init (reached by the Atomics.wait path) calls it, and the
# MayteraOS libc does not define it. It is WEAK so that if libc ever grows a real
# one, the real one wins, and so no other app that links libc is affected. This
# keeps the SHARED libc untouched (owner rule 1's "do not fork a private copy"
# does not apply: this is a one-symbol compat shim scoped to this archive, not a
# reimplementation of a shared primitive). The anti-no-op symbol check in PORT
# names pthread_condattr_setclock, so a build that dropped this file fails HERE.
#
# Contract from mports.sh: cwd = the unpacked srcdir (patched); MPORTS_CFLAGS
# carries the userland cross-build flags PLUS this port's cflags= (-DMAYTERAOS
# -Dalloca=__builtin_alloca); MPORTS_SRC is the unpacked srcdir; MPORTS_OUT is
# the install tree. After we return, mports looks for the archive at
# $(dirname MPORTS_SRC)/libquickjs.a and proves every symbol promised in PORT is
# defined in it.
set -euo pipefail

: "${MPORTS_CFLAGS:?mports must pass MPORTS_CFLAGS}"
: "${MPORTS_SRC:?mports must pass MPORTS_SRC}"

WORKDIR="$(dirname "$MPORTS_SRC")"   # where mports expects libquickjs.a
LIB="$WORKDIR/libquickjs.a"
# ports/quickjs/ (this script's own directory), for maytera_compat.c.
PORTDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The four upstream engine translation units. This is qjs_sources from
# upstream's CMakeLists.txt MINUS quickjs-libc.c (the OS-binding module, which
# this port deliberately does not build; see PORT). NOT a glob of *.c, which
# would sweep in qjs.c/qjsc.c (each with its own main()), the test harnesses
# (api-test.c, ctest.c, run-test262.c, fuzz.c) and the table generators
# (unicode_gen.c, whose output libunicode-table.h already ships pre-generated).
ENGINE_SRCS="dtoa libregexp libunicode quickjs"

OBJS=()

echo "quickjs/build.sh: compiling the engine (${ENGINE_SRCS})"
# -w because upstream trips -Wall/-Wextra (alloca int-conversion warnings from
# the -Dalloca macro, misleading-indentation, etc.); this port ships the engine
# unaltered but for the one cutils.h target patch, so its warnings are not ours
# to fix and would only bury a real one from our own maytera_compat.c.
for f in $ENGINE_SRCS; do
  gcc $MPORTS_CFLAGS -I. -w -c "$f.c" -o "$WORKDIR/qjs_$f.o"
  OBJS+=("$WORKDIR/qjs_$f.o")
done

echo "quickjs/build.sh: compiling maytera_compat.c (weak libc shim)"
# OUR file: compile it WITHOUT -w so a real problem in first-party code is loud.
# It needs quickjs's own headers on the include path (it includes <pthread.h>
# only), and the unpacked srcdir is the include root for consistency.
gcc $MPORTS_CFLAGS -I. -I"$PORTDIR" -c "$PORTDIR/maytera_compat.c" -o "$WORKDIR/maytera_compat.o"
OBJS+=("$WORKDIR/maytera_compat.o")

echo "quickjs/build.sh: archiving libquickjs.a"
rm -f "$LIB"
ar rcs "$LIB" "${OBJS[@]}"

echo "quickjs/build.sh: done -> $LIB"
