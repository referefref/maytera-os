#!/bin/bash
# Regenerate the Writer toolbar .ICN rasters from the SVG masters.
#
#   assets/office-icons/writer/build-icn.sh [outdir]      (default: ./icn)
#
# One clean scale, straight from the vector to the 64x64 target canvas with
# rsvg-convert (the renderer that produced the shipped /ICONS set), then packed
# to MICO by png2icn.py (stdlib-only, runs on the build container). Names come
# from MANIFEST.tsv. Fails on the first icon that does not render or pack.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/icn}
SIZE=64
command -v rsvg-convert >/dev/null || { echo "rsvg-convert missing" >&2; exit 1; }
mkdir -p "$OUT"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
n=0
while IFS=$'\t' read -r svg icn _rest; do
    case "$svg" in ''|'#'*) continue;; esac
    [ -f "$HERE/$svg.svg" ] || { echo "missing master $svg.svg" >&2; exit 1; }
    rsvg-convert -w $SIZE -h $SIZE --background-color=none "$HERE/$svg.svg" -o "$TMP/$icn.png"
    python3 "$HERE/png2icn.py" "$TMP/$icn.png" "$OUT/$icn.ICN" >/dev/null
    n=$((n+1))
done < "$HERE/MANIFEST.tsv"
echo "built $n icons into $OUT"
