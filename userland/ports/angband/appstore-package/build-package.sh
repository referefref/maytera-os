#!/usr/bin/env bash
# build-package.sh - build the Angband App Store .mpkg.
#
# Run on the build container. Takes no build slot: it packs, it does not compile. The lib data
# tree it packs is produced by mports (`userland/ports/mports.sh build angband`,
# which DOES take a slot) at userland/ports/out/data/angband, and the binary is
# built by userland/apps/angband/Makefile or by the golden build.
#
# NO ASSET FIX STEP, unlike AssaultCube: every file here is plain text that the
# game parses itself, so the bytes mports copied out of the upstream tarball are
# the bytes that ship.
#
# USAGE
#   bash userland/ports/angband/appstore-package/build-package.sh [VERSION]
# ENV
#   AB_DATA   lib data tree     (default: the golden build tree, then the repo's)
#   AB_BIN    the ANGBAND ELF   (default: the golden build tree, then the app dir)
#   OUTDIR    where the .mpkg lands (default /root/mpkg-out)
set -euo pipefail

VER=${1:-4.2.6-1}
OUTDIR=${OUTDIR:-/root/mpkg-out}

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$HERE
while [ "$REPO" != / ] && [ ! -r "$REPO/userland/libc/pkgman.c" ]; do REPO=$(dirname -- "$REPO"); done
[ -r "$REPO/userland/libc/pkgman.c" ] || { echo "cannot locate the repo root from $HERE" >&2; exit 2; }

AB_DATA=${AB_DATA:-}
if [ -z "$AB_DATA" ]; then
    # QUOTED on purpose. The publish scrubber rewrites this internal path to a
    # placeholder, and an UNQUOTED placeholder containing < is read by bash as a
    # redirection, which made the published script a syntax error. Quoting is
    # also just correct: an unquoted path word-splits and globs.
    for c in "<repo>-build/userland/ports/out/data/angband" \
             "$REPO/userland/ports/out/data/angband"; do
        [ -d "$c/gamedata" ] && { AB_DATA=$c; break; }
    done
fi
[ -d "${AB_DATA:-}/gamedata" ] || { echo "no angband lib data found. Run mports.sh build angband, or set AB_DATA." >&2; exit 2; }

AB_BIN=${AB_BIN:-}
if [ -z "$AB_BIN" ]; then
    # Quoted for the same reason as the AB_DATA list above.
    for c in "<repo>-build/userland/apps/angband/ANGBAND" \
             "$REPO/userland/apps/angband/ANGBAND"; do
        [ -s "$c" ] && { AB_BIN=$c; break; }
    done
fi
[ -s "${AB_BIN:-}" ] || { echo "no ANGBAND ELF found. Build it first, or set AB_BIN." >&2; exit 2; }
file "$AB_BIN" | grep -q ELF || { echo "$AB_BIN is not an ELF" >&2; exit 2; }

# The binary must actually carry the NEW data root, or the package installs its
# payload somewhere the engine will not look. The path is compiled in, so this is
# checkable in the artifact rather than inferred from the PORT file (#gamedata2:
# in-tree prose lies, verify the artifact).
if ! strings -a "$AB_BIN" | grep -qx '/GAMES/ANGBAND/lib/'; then
    echo "$AB_BIN does not contain the string /GAMES/ANGBAND/lib/ : it predates the data-root move and would not find this package's payload." >&2
    exit 1
fi
echo "build-package: the ANGBAND binary carries the /GAMES/ANGBAND/lib/ data root"

exec bash "$REPO/tools/mpkg/build-mpkg.sh" \
    --id angband --version "$VER" --out "$OUTDIR" \
    --install "$HERE/INSTALL" \
    --file "$AB_BIN:ANGBAND" \
    --tree "$AB_DATA:lib"
