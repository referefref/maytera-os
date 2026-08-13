#!/bin/bash
# run_malloc.sh - #631 regression test for the userland allocator's free-list
# walks (userland/libc/stdlib.c).
#
# TWO ARMS. Both must behave as stated or this script exits non-zero:
#
#   NEGATIVE CONTROL  recompiles the SAME allocator with the step budget
#                     disabled (limit = UINT64_MAX) and requires every cycle
#                     scenario to HANG, i.e. timeout(1) must kill it (124).
#                     If a scenario finishes with the budget disabled, that
#                     scenario is not reaching a free-list walk and proves
#                     nothing, so that is a failure too.
#
#   POSITIVE          runs the live ../stdlib.c and requires every scenario to
#                     terminate AND to have reported the corruption.
#
# Termination is bounded here, by timeout(1), never inside the test binary: a
# spin that decides for itself when to give up is not a spin test.
#
# The allocator is compiled freestanding exactly as userland/libc/Makefile does
# and then symbol-prefixed with objcopy so it can be linked next to glibc.
set -u

cd "$(dirname "$0")" || exit 1
LIBC=..
GCCINC=$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/include 2>/dev/null | tail -1)
[ -n "$GCCINC" ] || { echo "no gcc include dir found"; exit 1; }

UUT_FLAGS="-m64 -ffreestanding -fno-builtin -nostdinc -nostdlib \
           -fno-stack-protector -mno-red-zone -Wall -Wextra -O1 -g \
           -isystem $GCCINC -I$LIBC"

WORK=$(mktemp -d) || exit 2
trap 'rm -rf "$WORK"' EXIT

build() {   # $1 = allocator source, $2 = output binary
    gcc $UUT_FLAGS -x c -c "$1" -o "$WORK/alloc_raw.o" 2>"$WORK/cc.log" || {
        echo "  compile of $1 FAILED:"; sed -n 1,20p "$WORK/cc.log"; return 2; }
    objcopy --prefix-symbols=mh_ "$WORK/alloc_raw.o" "$WORK/alloc.o" || return 2
    gcc -m64 -O1 -g -c malloc_cycle_test.c -o "$WORK/t.o" 2>"$WORK/cc2.log" || {
        echo "  compile of harness FAILED:"; sed -n 1,20p "$WORK/cc2.log"; return 2; }
    gcc -m64 "$WORK/t.o" "$WORK/alloc.o" -o "$2" 2>"$WORK/ld.log" || {
        echo "  link FAILED:"; sed -n 1,20p "$WORK/ld.log"; return 2; }
    return 0
}

# The negative control is the live allocator with ONE edit: the per-walk step
# budget replaced by an unreachable limit. Everything else (the range checks,
# the size sanity checks, the redzone) stays, which is the point: those guards
# were already there and did NOT stop the spin.
sed 's/uint64_t steps = 0, limit = heap_walk_limit();/uint64_t steps = 0, limit = (uint64_t)-1;  \/* NEGATIVE CONTROL *\//' \
    "$LIBC/stdlib.c" > "$WORK/stdlib_nobudget.c"
NSUB=$(grep -c 'NEGATIVE CONTROL' "$WORK/stdlib_nobudget.c")
if [ "$NSUB" -lt 3 ]; then
    echo "RESULT: BROKEN - expected >=3 step-budget sites to disable, found $NSUB."
    echo "        The walks were restructured; update this test before trusting it."
    exit 1
fi

rc_all=0

echo "=== ARM 1: NEGATIVE CONTROL ($NSUB step budgets disabled, must hang) ==="
if ! build "$WORK/stdlib_nobudget.c" "$WORK/t_neg"; then
    echo "RESULT: BROKEN - negative arm did not build"; exit 1
fi
neg_bad=0
for n in 1 2 3 4; do
    timeout 10 "$WORK/t_neg" $n >/dev/null 2>&1
    rc=$?
    if [ $rc -eq 124 ]; then
        echo "  scenario $n: hung as expected (timeout)"
    else
        echo "  scenario $n: FINISHED (exit $rc) - it never reaches a bounded walk"
        neg_bad=1
    fi
done
if [ $neg_bad -eq 0 ]; then
    echo "RESULT: GOOD - every cycle scenario spins forever without the budget."
else
    echo "RESULT: BAD - a scenario completed without the budget, so it proves nothing."
    rc_all=1
fi

echo
echo "=== ARM 2: POSITIVE (live $LIBC/stdlib.c, must terminate and report) ==="
if ! build "$LIBC/stdlib.c" "$WORK/t_pos"; then
    echo "RESULT: FAIL - positive arm did not build"; exit 1
fi
pos_bad=0
for n in 1 2 3 4 5; do
    out=$(timeout 60 "$WORK/t_pos" $n 2>&1)
    rc=$?
    if [ $rc -eq 124 ]; then
        echo "  scenario $n: FAIL - still hangs"; pos_bad=1
    elif [ $rc -ne 0 ]; then
        echo "  scenario $n: FAIL - exit $rc"
        echo "$out" | sed 's/^/      /'
        pos_bad=1
    else
        echo "  scenario $n: ok - $(echo "$out" | grep -c '\[malloc\] heap corruption' ) corruption report(s), terminated"
    fi
done
if [ $pos_bad -eq 0 ]; then
    echo "RESULT: PASS - every walk is bounded, reports loudly, and returns."
else
    echo "RESULT: FAIL"
    rc_all=1
fi

echo
[ $rc_all -eq 0 ] && echo "#631 regression test: PASS" || echo "#631 regression test: FAIL"
exit $rc_all
