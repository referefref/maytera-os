#!/usr/bin/env bash
# ports/tcc/build.sh - the build=script step for the TinyCC port (Tier 2 item #7
# of docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md).
#
# WHY build=script AND NOT build=objects. Two reasons, either alone sufficient:
#  1. The archive must contain OUR maytera_compat.c (the WEAK strtold shim) which
#     lives beside this recipe, not in the tarball, so it can never appear in a
#     `sources=` list (mports verifies those against the unpacked tarball). Same
#     structural reason as the quickjs port's maytera_compat.c.
#  2. tcc.c must be compiled with ONE_SOURCE=1 as ONE translation unit (it
#     #includes libtcc.c which #includes tccpp/tccgen/tccelf/x86_64-*). A
#     `sources=` list would name a dozen files that must NOT each be compiled
#     separately.
#
# It also builds libtcc1.a (tcc's own support routines) and assembles the dist/
# runtime tree the recipe ships to /APPS/tcc via install_data.
#
# Contract from mports.sh: cwd = the unpacked srcdir (patched); MPORTS_CFLAGS
# carries the userland cross-build flags PLUS this port's cflags= (-DMAYTERAOS
# -DTCC_TARGET_X86_64 -DCONFIG_TCC_STATIC -DONE_SOURCE=1 the CONFIG_TCC_* paths
# and -w); MPORTS_SRC is the unpacked srcdir; MPORTS_OUT is the install tree.
# After we return, mports looks for the archive at $(dirname MPORTS_SRC)/libtcc.a
# and proves every symbol promised in PORT is defined in it.
set -euo pipefail

: "${MPORTS_CFLAGS:?mports must pass MPORTS_CFLAGS}"
: "${MPORTS_SRC:?mports must pass MPORTS_SRC}"
: "${MPORTS_OUT:?mports must pass MPORTS_OUT}"

WORKDIR="$(dirname "$MPORTS_SRC")"      # where mports expects libtcc.a
LIB="$WORKDIR/libtcc.a"
PORTDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # ports/tcc/, for compat.c
# The repository's freestanding libc. MPORTS_OUT is userland/ports/out, so
# ../../libc is userland/libc. Absolute, because the compiles below run with
# cwd = the unpacked srcdir and a relative path would resolve inside the tarball.
LIBC_DIR="$(cd "$MPORTS_OUT/../../libc" && pwd)"

# --- config.h -------------------------------------------------------------
# tcc.h #includes "config.h" and uses TCC_VERSION from it. Everything else tcc
# needs is a -D in cflags (CONFIG_TCCDIR etc.), so this file is one line. Written
# fresh every build so a stale one can never drift from the pinned version.
echo '#define TCC_VERSION "0.9.27"' > config.h

# --- libtcc.a: the whole compiler in one object, plus our compat shim ------
echo "tcc/build.sh: compiling tcc.c (ONE_SOURCE, the whole compiler)"
# -I. so tcc.c finds tcc.h/config.h/libtcc.c and the *-gen.c/*-link.c it includes.
gcc $MPORTS_CFLAGS -I. -c tcc.c -o "$WORKDIR/tcc_main.o"

echo "tcc/build.sh: compiling maytera_compat.c (weak strtold shim)"
# OUR file: it is already -w-clean, but compile it WITHOUT the port's blanket -w
# is not possible here (MPORTS_CFLAGS carries -w); it is a three-line file with
# no warnings regardless. It needs no tcc headers.
gcc $MPORTS_CFLAGS -c "$PORTDIR/maytera_compat.c" -o "$WORKDIR/maytera_compat.o"

echo "tcc/build.sh: archiving libtcc.a"
rm -f "$LIB"
ar rcs "$LIB" "$WORKDIR/tcc_main.o" "$WORKDIR/maytera_compat.o"

# --- libtcc1.a: tcc's own support routines (alloca, __va_arg, 64-bit helpers) -
# The x86_64 object set from tcc's lib/Makefile (OBJ-x86_64 = libtcc1.o
# alloca86_64.o alloca86_64-bt.o va_list.o), MINUS bcheck.o (bounds checking,
# a -run/-b feature this cross build does not enable). Normally tcc compiles
# these itself; gcc compiles them fine for the same target and avoids a
# bootstrap. A program that uses varargs (printf) or alloca/VLA links this.
echo "tcc/build.sh: building libtcc1.a (compiler support library)"
L1DIR="$WORKDIR/libtcc1-obj"
rm -rf "$L1DIR"; mkdir -p "$L1DIR"
gcc $MPORTS_CFLAGS -c lib/libtcc1.c      -o "$L1DIR/libtcc1.o"
gcc $MPORTS_CFLAGS -c lib/va_list.c      -o "$L1DIR/va_list.o"
gcc $MPORTS_CFLAGS -c lib/alloca86_64.S  -o "$L1DIR/alloca86_64.o"
gcc $MPORTS_CFLAGS -c lib/alloca86_64-bt.S -o "$L1DIR/alloca86_64-bt.o"
rm -f "$WORKDIR/libtcc1.a"
ar rcs "$WORKDIR/libtcc1.a" "$L1DIR"/*.o

# --- the freestanding libc a device-compiled program links against ---------
# Ensure libc.a + crt0.o exist (they are build outputs of userland/libc). In the
# golden build userland is built before this runs, so this is usually a no-op;
# make it robust in a fresh tree too. LOUD if it cannot produce them.
echo "tcc/build.sh: ensuring userland libc.a + crt0.o are built"
make -C "$LIBC_DIR" crt0.o libc.a >/dev/null 2>&1 || true
[ -f "$LIBC_DIR/libc.a" ] || { echo "tcc/build.sh: FATAL: $LIBC_DIR/libc.a not built" >&2; exit 1; }
[ -f "$LIBC_DIR/crt0.o" ] || { echo "tcc/build.sh: FATAL: $LIBC_DIR/crt0.o not built" >&2; exit 1; }

# --- dist/: the runtime tree shipped to /APPS/tcc (install_data=dist:tcc) ---
echo "tcc/build.sh: assembling dist/ runtime tree"
DIST="$MPORTS_SRC/dist"
rm -rf "$DIST"
mkdir -p "$DIST/include" "$DIST/lib"
# tcc's own bundled headers (stddef.h, stdarg.h, float.h, stdbool.h, varargs.h).
cp include/*.h "$DIST/include/"
cp "$WORKDIR/libtcc1.a" "$DIST/lib/libtcc1.a"
cp "$LIBC_DIR/libc.a"   "$DIST/lib/libc.a"
cp "$LIBC_DIR/crt0.o"   "$DIST/lib/crt0.o"

echo "tcc/build.sh: done -> $LIB (+ dist/: $(ls "$DIST/lib" | tr '\n' ' '))"
