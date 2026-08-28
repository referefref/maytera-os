#!/bin/bash
# run.sh - #183: the table gate, then the host self-test. One command.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB="$(dirname "$HERE")"
bash "$LIB/table-gate.sh" || exit 1
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
rustc --edition 2021 -O -o "$OUT/midihosttest" "$HERE/main.rs" || exit 1
"$OUT/midihosttest"
