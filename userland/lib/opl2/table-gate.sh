#!/bin/bash
# table-gate.sh - #182: the generated OPL2 ROM tables must still match the
# formulas they claim to come from.
#
# opl2_tables.rs is 512 numbers with a header saying "GENERATED, DO NOT EDIT".
# That header is a request, not a control. This is the control: it regenerates
# the tables from gen_tables.py and diffs. If anyone hand-edits a value, or
# edits the generator without regenerating, the build fails here rather than the
# synthesiser quietly going out of tune.
#
# It matters more than a normal generated-file check because these particular
# numbers are UNVERIFIABLE BY INSPECTION. Nobody reviewing a pull request is
# going to notice that entry 137 of the log-sine table is off by one, and being
# off by one there is an audible artifact on every note the machine plays.
#
#   ./table-gate.sh <repo-root>   0 = tables match their formulas
#   ./table-gate.sh --self-test   PROVES it goes RED on a single altered digit

set -uo pipefail

SELFTEST=0
ROOT="."
for a in "$@"; do
    case "$a" in
        --self-test) SELFTEST=1 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) ROOT="$a" ;;
    esac
done

GEN="$ROOT/userland/lib/opl2/gen_tables.py"
TAB="$ROOT/userland/lib/opl2/opl2_tables.rs"
# The self-test must find the generator regardless of where the gate was
# invoked from, because a gate that only works from the repo root is a gate
# that gets skipped from anywhere else.
SELF_GEN="$(cd "$(dirname "$0")" && pwd)/gen_tables.py"

check() {
    local gen="$1" tab="$2"
    if [ ! -f "$gen" ]; then
        echo "[opl-table-gate] FAIL: generator missing: $gen"
        return 1
    fi
    if [ ! -f "$tab" ]; then
        echo "[opl-table-gate] FAIL: generated table missing: $tab"
        return 1
    fi
    local tmp
    tmp=$(mktemp)
    # The generator asserts its own four anchor values (logsin[0] = 2137,
    # logsin[255] = 0, exp[0] = 0, exp[255] = 1018) and both monotonicities, so
    # a generator that has itself been broken fails HERE rather than producing
    # a self-consistent pair of wrong files that diff clean.
    if ! python3 "$gen" > "$tmp" 2>&1; then
        echo "[opl-table-gate] FAIL: the generator itself did not run or its"
        echo "                 anchor assertions failed:"
        sed 's/^/                 /' "$tmp"
        rm -f "$tmp"
        return 1
    fi
    if diff -u "$tab" "$tmp" > /dev/null 2>&1; then
        rm -f "$tmp"
        return 0
    fi
    echo "[opl-table-gate] FAIL: $tab does not match what $gen produces."
    diff -u "$tab" "$tmp" | head -20 | sed 's/^/                 /'
    rm -f "$tmp"
    return 1
}

if [ "$SELFTEST" -eq 1 ]; then
    TMP=$(mktemp -d)
    trap 'rm -rf "$TMP"' EXIT
    mkdir -p "$TMP/userland/lib/opl2"
    cp "$SELF_GEN" "$TMP/userland/lib/opl2/gen_tables.py"
    python3 "$SELF_GEN" > "$TMP/userland/lib/opl2/opl2_tables.rs"

    echo "[opl-table-gate] self-test 1/2: freshly generated tables must be GREEN"
    if check "$TMP/userland/lib/opl2/gen_tables.py" "$TMP/userland/lib/opl2/opl2_tables.rs"; then
        echo "  GREEN as required"
    else
        echo "  BROKEN: the gate failed its own generator's output."
        exit 1
    fi

    echo "[opl-table-gate] self-test 2/2: ONE altered digit must be RED"
    # 1731 is logsin[1]. Change it to 1732: a single digit, the kind of edit
    # that no reviewer would ever spot and that would detune every note.
    sed -i '0,/1731/s//1732/' "$TMP/userland/lib/opl2/opl2_tables.rs"
    if check "$TMP/userland/lib/opl2/gen_tables.py" "$TMP/userland/lib/opl2/opl2_tables.rs" > /dev/null 2>&1; then
        echo "  BROKEN: the gate PASSED a table with an altered entry."
        exit 1
    else
        echo "  RED as required (logsin[1] 1731 -> 1732 detected)"
    fi
    echo "[opl-table-gate] SELF-TEST PASSED (2/2)."
    exit 0
fi

if check "$GEN" "$TAB"; then
    echo "[opl-table-gate] OK: ROM tables match their generating formulas."
    exit 0
else
    exit 1
fi
