#!/usr/bin/env bash
# frotz/build.sh - build the dumb (terminal, non-curses) frotz interface
# only: common/*.c (the Z-machine core) + dumb/*.c (the dumb terminal
# frontend). Deliberately excludes src/curses, src/sdl, src/blorb, src/x
# and src/dos - none of those are needed for a plain terminal interpreter,
# and the dumb target in upstream's own Makefile does not link blorb either.
set -euo pipefail

PORT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
S="${MPORTS_SRC:?frotz/build.sh: MPORTS_SRC unset}"
W="$(dirname "$S")"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

CFLAGS="$MPORTS_CFLAGS -DCONFIG_DIR=\"\\\"/CONFIG\\\"\" -DVERSION=\"\\\"2.44\\\"\" -DNO_SOUND -DFILENAME_MAX=4096 -D__bool_true_false_are_defined=1 -fcommon -w"

COMMON="buffer err fastmem files hotkey input main math object process quetzal random redirect screen sound stream table text variable"
DUMB="dumb_init dumb_input dumb_output dumb_pic"

for f in $COMMON; do
  gcc $CFLAGS -c "src/common/$f.c" -o "$WORKDIR/fz_$f.o"
done
for f in $DUMB; do
  gcc $CFLAGS -Isrc/dumb -c "src/dumb/$f.c" -o "$WORKDIR/fz_$f.o"
done

rm -f "$W/libfrotz.a"
ar rcs "$W/libfrotz.a" "$WORKDIR"/fz_*.o
echo "frotz/build.sh: OK -> $W/libfrotz.a"
