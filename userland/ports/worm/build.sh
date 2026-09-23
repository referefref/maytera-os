#!/usr/bin/env bash
# worm/build.sh - see PORT for why build=script (our own bsdgame_compat.c).
set -euo pipefail

PORT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
S="${MPORTS_SRC:?worm/build.sh: MPORTS_SRC unset}"
W="$(dirname "$(dirname "$S")")"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

CFLAGS="$MPORTS_CFLAGS -I$PORT_DIR/shim -DBSDGAME_PROGNAME=\"worm\" -w"

gcc $CFLAGS -c worm.c -o "$WORKDIR/wm_worm.o"
gcc $CFLAGS -c "$PORT_DIR/bsdgame_compat.c" -o "$WORKDIR/wm_compat.o"

rm -f "$W/libworm.a"
ar rcs "$W/libworm.a" "$WORKDIR"/wm_*.o
echo "worm/build.sh: OK -> $W/libworm.a"
