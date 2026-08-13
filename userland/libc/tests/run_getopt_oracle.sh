#!/bin/bash
# run_getopt_oracle.sh - #745 (local 72) differential between the SHIPPING
# userland/libc/getopt.c and the build host's glibc getopt.
#
# TWO ARMS. Both must behave as stated or this script exits non-zero:
#
#   NEGATIVE CONTROL  runs the same table against fixtures/getopt.c.no-ddash,
#                     which is getopt.c with the "--" end-of-options block
#                     removed. It REQUIRES differences to appear. A differential
#                     that reports agreement for a knowingly different
#                     implementation is measuring nothing.
#
#   POSITIVE          runs the real getopt.c and requires ZERO differences.
#
# The unit under test is compiled with the shipping freestanding flags and its
# public names renamed to mos_*, so glibc's getopt and ours coexist in one
# binary and answer the same argv.
set -u

cd "$(dirname "$0")" || exit 1
LIBC=..
GCCINC=$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/include 2>/dev/null | tail -1)
[ -n "$GCCINC" ] || { echo "no gcc include dir found"; exit 1; }

RENAME="-Dgetopt=mos_getopt -Dgetopt_long=mos_getopt_long \
        -Dgetopt_long_only=mos_getopt_long_only -Doptarg=mos_optarg \
        -Doptind=mos_optind -Dopterr=mos_opterr -Doptopt=mos_optopt"

UUT_FLAGS="-m64 -ffreestanding -fno-builtin -nostdinc -fno-stack-protector \
           -mno-red-zone -Wall -Wextra -Werror -O2 -g -isystem $GCCINC -I$LIBC $RENAME"

rc=0
d=$(mktemp -d) || exit 2
trap 'rm -rf "$d"' EXIT

build() {   # $1 = getopt source, $2 = tag
    gcc $UUT_FLAGS -x c -c "$1" -o "$d/$2.o" || return 2
    gcc -m64 -O1 -g -Wall -Wextra -c getopt_oracle_test.c -o "$d/$2-h.o" || return 2
    gcc -m64 "$d/$2-h.o" "$d/$2.o" -o "$d/$2" || return 2
    return 0
}

echo "=== ARM 1: NEGATIVE CONTROL (fixtures/getopt.c.no-ddash must DIFFER) ==="
if ! build fixtures/getopt.c.no-ddash neg; then
    echo "RESULT: BROKEN - negative arm did not build"; rc=1
else
    "$d/neg" > "$d/neg.out" 2>&1
    n=$?
    cp "$d/neg.out" ./getopt_oracle_negative.out
    if [ $n -eq 0 ]; then
        echo "RESULT: BAD - a getopt without \"--\" handling matched glibc exactly."
        echo "        The differential is measuring nothing."
        rc=1
    else
        echo "RESULT: GOOD - the differential separates them:"
        grep -m2 -A2 "^DIFF" "$d/neg.out" | sed 's/^/    /'
        tail -2 "$d/neg.out" | sed 's/^/    /'
    fi
fi

echo
echo "=== ARM 2: POSITIVE (the real getopt.c must MATCH glibc exactly) ==="
if ! build "$LIBC/getopt.c" pos; then
    echo "RESULT: FAIL - positive arm did not build"; rc=1
else
    "$d/pos" > "$d/pos.out" 2>&1
    p=$?
    cp "$d/pos.out" ./getopt_oracle_positive.out
    cat "$d/pos.out"
    if [ $p -ne 0 ]; then echo "RESULT: FAIL"; rc=1; else echo "RESULT: PASS"; fi
fi

echo
if [ $rc -eq 0 ]; then echo "run_getopt_oracle.sh: PASS (both arms)"; else echo "run_getopt_oracle.sh: FAIL"; fi
exit $rc
