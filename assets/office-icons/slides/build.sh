#!/bin/bash
# Rasterize the SLIDES toolbar icon masters to 64x64 MICO (.ICN) files.
#
# One scale, straight from the vector master to the 64x64 target, transparent
# background, exactly as assets/icons/README.md prescribes for every other
# icon in the set. Run from anywhere; writes ./icn/<ICN-NAME>.ICN next to
# the SVGs. Needs rsvg-convert (librsvg) and python3 (stdlib only).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
PACK="$ROOT/tools/icons/png2icn.py"
[ -x "$PACK" ] || [ -f "$PACK" ] || { echo "missing $PACK" >&2; exit 1; }
command -v rsvg-convert >/dev/null || { echo "rsvg-convert not installed" >&2; exit 1; }

OUT="$HERE/icn"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"

# svg master -> .ICN name (8.3 uppercase, see README.md for the prefix rule)
while read -r svg icn; do
    [ -n "$svg" ] || continue
    rsvg-convert -w 64 -h 64 --background-color=none "$HERE/$svg.svg" -o "$TMP/$icn.png"
    python3 "$PACK" "$TMP/$icn.png" "$OUT/$icn.ICN"
done <<'MAP'
new             OFNEW
open            OFOPEN
save            OFSAVE
new-slide       SLNEW
duplicate-slide SLDUP
delete-slide    SLDEL
slide-layout    SLLAYOUT
text-box        OFTEXTBX
insert-image    OFIMAGE
insert-shape    OFSHAPE
bold            OFBOLD
italic          OFITALIC
align-left      OFALIGNL
align-center    OFALIGNC
align-right     OFALIGNR
bullet-list     OFBULLET
prev-slide      SLPREV
next-slide      SLNEXT
present-play    SLPLAY
notes           SLNOTES
MAP

# Every output must be exactly a 64x64 MICO: 12-byte header + 64*64*4.
for f in "$OUT"/*.ICN; do
    sz=$(stat -c %s "$f")
    [ "$sz" -eq $((12 + 64 * 64 * 4)) ] || { echo "BAD SIZE $f ($sz)" >&2; exit 1; }
    [ "$(head -c 4 "$f")" = "MICO" ] || { echo "BAD MAGIC $f" >&2; exit 1; }
done
echo "OK: $(ls "$OUT"/*.ICN | wc -l) icons in $OUT"
