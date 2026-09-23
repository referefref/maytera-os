#!/usr/bin/env bash
# ports/sqlite/build.sh - the build=script step for the SQLite port.
#
# WHY THIS EXISTS (and is not build=objects). mports build=objects archives a
# list of UPSTREAM translation units. This port must put TWO things into one
# libsqlite3.a: the upstream amalgamation sqlite3.c AND our own MayteraOS VFS
# (maytera_vfs.c, tracked beside this script). The VFS is not in the tarball, so
# it cannot go in a `sources` list, which mports verifies against the unpacked
# tree. Hence a script. mports.sh runs us with cwd = the unpacked srcdir and,
# after we return, looks for the archive at $(dirname "$MPORTS_SRC")/<lib> and
# proves every promised symbol is defined in it.
#
# Contract from mports.sh: MPORTS_CFLAGS (the userland cross-build flags plus
# this port's cflags= from PORT), MPORTS_SRC (the unpacked srcdir, our cwd),
# MPORTS_OUT (the install tree). We produce libsqlite3.a where mports expects it.
set -euo pipefail

: "${MPORTS_CFLAGS:?mports must pass MPORTS_CFLAGS}"
: "${MPORTS_SRC:?mports must pass MPORTS_SRC}"

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"   # ports/sqlite (our VFS lives here)
WORKDIR="$(dirname "$MPORTS_SRC")"          # $WORK/sqlite: where mports wants the .a
LIB="$WORKDIR/libsqlite3.a"

echo "sqlite/build.sh: compiling the upstream amalgamation (sqlite3.c)"
# Upstream generated code: silence warnings for it (-w) so a 250k-line
# amalgamation does not bury a real warning from OUR files. The MPORTS_CFLAGS
# already carry the SQLITE_* option set (from PORT's cflags=). cwd is the
# unpacked srcdir, so "sqlite3.h" resolves beside sqlite3.c.
gcc $MPORTS_CFLAGS -w -c sqlite3.c -o "$WORKDIR/sqlite3.o"

echo "sqlite/build.sh: compiling the MayteraOS VFS (maytera_vfs.c)"
# OUR code keeps full warnings (no -w). Same SQLITE_* options so its view of
# sqlite3.h matches the library's. -I. finds sqlite3.h in the unpacked tree.
gcc $MPORTS_CFLAGS -I. -c "$SELF_DIR/maytera_vfs.c" -o "$WORKDIR/maytera_vfs.o"

echo "sqlite/build.sh: archiving libsqlite3.a"
rm -f "$LIB"
ar rcs "$LIB" "$WORKDIR/sqlite3.o" "$WORKDIR/maytera_vfs.o"

echo "sqlite/build.sh: done -> $LIB"
