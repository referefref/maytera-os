#!/bin/bash
# table-gate.sh - #183: PROVE midi_tables.rs still matches its generating
# formulas, and prove this check can fail.
#
# Modelled on userland/lib/opl2/table-gate.sh and for the same reason: a
# generated file that nobody regenerates is a transcribed file with a misleading
# comment at the top. The specific fault this guards against is documented in
# blame.md (2026-08-20, #182): the AdLib note table that everyone copies puts
# A440 nine cents flat because it was computed against the wrong chip rate. A
# single edited digit in NOTE_FNUM is exactly that fault, reintroduced by hand.
#
#   ./table-gate.sh <repo-root>   0 = tables match the formulas, 1 = they do not
#   ./table-gate.sh --self-test   PROVES it goes RED on one altered digit
#
# The generator ALSO asserts its own anchors (A440 -> 580/block 4, which
# userland/lib/opl2 arrived at independently), so a formula edited to agree with
# a corrupted table fails inside python before this script ever diffs anything.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELFTEST=0
ROOT=""
for a in "$@"; do
    case "$a" in
        --self-test) SELFTEST=1 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) ROOT="$a" ;;
    esac
done

check() {
    # $1 = directory holding gen_midi_tables.py and midi_tables.rs
    local d="$1"
    local tmp
    tmp=$(mktemp)
    if ! python3 "$d/gen_midi_tables.py" --stdout > "$tmp" 2>/tmp/midi-tablegen.err; then
        echo "[midi-table-gate] FAIL: the generator itself refused to run:"
        sed 's/^/    /' /tmp/midi-tablegen.err
        rm -f "$tmp"
        return 1
    fi
    if diff -q "$tmp" "$d/midi_tables.rs" >/dev/null 2>&1; then
        rm -f "$tmp"
        return 0
    fi
    echo "[midi-table-gate] FAIL: midi_tables.rs does not match gen_midi_tables.py."
    diff "$tmp" "$d/midi_tables.rs" | head -20 | sed 's/^/    /'
    rm -f "$tmp"
    return 1
}

if [ "$SELFTEST" -eq 1 ]; then
    TMP=$(mktemp -d)
    trap 'rm -rf "$TMP"' EXIT
    cp "$HERE/gen_midi_tables.py" "$HERE/midi_tables.rs" "$TMP/"

    echo "[midi-table-gate] self-test 1/2: the checked-in tables must be GREEN"
    if check "$TMP"; then
        echo "  GREEN as required"
    else
        echo "  BROKEN: the checked-in tables do not match their own generator."
        exit 1
    fi

    echo "[midi-table-gate] self-test 2/2: ONE altered digit must be RED"
    # 580 is A440's F-Number. 577 is the value Jeff Lee's table gives, which is
    # 9 cents flat. Plant precisely the historical mistake.
    sed -i '0,/ 580,/s/ 580,/ 577,/' "$TMP/midi_tables.rs"
    if check "$TMP" >/dev/null 2>&1; then
        echo "  BROKEN: the gate PASSED a table with A440 set to 577 (9 cents flat)."
        exit 1
    else
        echo "  RED as required (A440 F-Number 580 -> 577 was caught)"
    fi
    echo "[midi-table-gate] SELF-TEST PASSED (2/2)."
    exit 0
fi

if [ -n "$ROOT" ] && [ -d "$ROOT/userland/lib/midi" ]; then
    D="$ROOT/userland/lib/midi"
else
    D="$HERE"
fi

if check "$D"; then
    echo "[midi-table-gate] OK: midi_tables.rs matches its generating formulas."
    exit 0
else
    exit 1
fi
