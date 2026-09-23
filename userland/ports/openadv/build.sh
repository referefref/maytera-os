#!/usr/bin/env bash
# openadv/build.sh - generate dungeon.c/dungeon.h with upstream's own
# make_dungeon.py, then compile the interpreter. See PORT for why build=script.
set -euo pipefail

PORT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
S="${MPORTS_SRC:?openadv/build.sh: MPORTS_SRC unset}"
W="$(dirname "$S")"
WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

# HOST dependency check, LOUD rather than a silent stale/empty dungeon.c
# (see PORT's HOST DEPENDENCY note). This is a build-host tool, not
# something linked into the target binary.
python3 -c 'import yaml' 2>/dev/null || {
  echo "openadv/build.sh: FATAL: python3 has no 'yaml' module (PyYAML)." >&2
  echo "  make_dungeon.py cannot generate dungeon.c/dungeon.h without it." >&2
  echo "  Install it on the BUILD HOST with: apt-get install -y python3-yaml" >&2
  exit 1
}

echo "openadv/build.sh: generating dungeon.c/dungeon.h from adventure.yaml"
python3 make_dungeon.py
[ -f dungeon.c ] && [ -f dungeon.h ] || {
  echo "openadv/build.sh: FATAL: make_dungeon.py did not produce dungeon.c/dungeon.h" >&2
  exit 1
}

# $MPORTS_CFLAGS already carries -I<ports/out/include> (mports.sh appends it
# before invoking any build=script), which is where libedit's histedit.h and
# ncurses's headers live; add only our own editline/readline.h shim on top.
# VERSION matches NEWS.adoc's latest entry (1.22) the way upstream's own
# Makefile derives it from that file at build time.
CFLAGS="$MPORTS_CFLAGS -I$PORT_DIR/shim -DVERSION=\"\\\"1.22\\\"\" -w"

SRCS="main actions init misc saveresume score dungeon"  # cheat.c is upstreams standalone save-file test tool with its OWN main(); it must NOT go in the game archive or the linker resolves crt0 main() to it instead of the game (fixes #246 tuigames ADVENTURE)
for f in $SRCS; do
  gcc $CFLAGS -c "$f.c" -o "$WORKDIR/adv_$f.o"
done

rm -f "$W/libopenadv.a"
ar rcs "$W/libopenadv.a" "$WORKDIR"/adv_*.o
echo "openadv/build.sh: OK -> $W/libopenadv.a"
