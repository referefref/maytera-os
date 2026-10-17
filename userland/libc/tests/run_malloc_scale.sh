#!/bin/bash
# run_malloc_scale.sh - #245 regression gate: the userland allocator's
# per-allocation work must not grow with the live block count.
#
# TWO ARMS, and the RED one uses the REAL pre-fix allocator pulled out of git,
# not a hand-written imitation of it. A gate that has never been seen to fail is
# not a gate.
#
#   RED    the allocator as of $RED_REV, before the size-binned free list. Its
#          bounded-walk steps must GROW superlinearly, i.e. exceed the slope
#          limit. If it does not, either the threshold is meaningless or the
#          revision is wrong, and this script says so and stops.
#
#   GREEN  the live ../stdlib.c. The same count at 4N must be at most
#          SLOPE_MAX x the count at N. Perfectly linear would be 4x.
#
# BOTH arms gate on the allocator's own step counter, never on wall clock. On a
# shared build host the same run varies several-fold, and a flaky gate gets
# switched off. The pre-fix revision has no such counter, so this script ADDS
# one to it with a sed, exactly as run_malloc.sh's negative control edits the
# live source; that keeps the two arms measuring the same thing. The wall-clock
# numbers are printed for information and decide nothing.
set -u

cd "$(dirname "$0")" || exit 1
LIBC=..
REPO=$LIBC/../..
N=${N:-15000}
SLOPE_MAX=${SLOPE_MAX:-6}            # linear is 4; the pre-fix allocator is ~16
RED_REV=${MALLOC_SCALE_RED_REV:-a4cc56b7}

GCCINC=$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/include 2>/dev/null | tail -1)
[ -n "$GCCINC" ] || { echo "no gcc include dir found"; exit 1; }

UUT_FLAGS="-m64 -ffreestanding -fno-builtin -nostdinc -nostdlib \
           -fno-stack-protector -mno-red-zone -Wall -Wextra -O2 -g \
           -isystem $GCCINC -I$LIBC"

WORK=$(mktemp -d) || exit 2
trap 'rm -rf "$WORK"' EXIT

build() {   # $1 = allocator source, $2 = output binary
    gcc $UUT_FLAGS -x c -c "$1" -o "$WORK/alloc_raw.o" 2>"$WORK/cc.log" || {
        echo "  compile of $1 FAILED:"; sed -n 1,25p "$WORK/cc.log"; return 2; }
    objcopy --prefix-symbols=mh_ "$WORK/alloc_raw.o" "$WORK/alloc.o" || return 2
    gcc -m64 -O2 -g -c malloc_scale_test.c -o "$WORK/t.o" 2>"$WORK/cc2.log" || {
        echo "  compile of harness FAILED:"; sed -n 1,25p "$WORK/cc2.log"; return 2; }
    gcc -m64 "$WORK/t.o" "$WORK/alloc.o" -o "$2" 2>"$WORK/ld.log" || {
        echo "  link FAILED:"; sed -n 1,25p "$WORK/ld.log"; return 2; }
    return 0
}

run_arm() {  # $1 = binary, $2 = n ; prints the line, sets STEPS and SECS
    local out
    out=$("$1" "$2" 2>&1)
    echo "$out" | sed 's/^/      /'
    STEPS=$(echo "$out" | sed -n 's/.*steps=\([0-9]*\).*/\1/p' | tail -1)
    SECS=$(echo "$out" | sed -n 's/.*secs=\([0-9.]*\).*/\1/p' | tail -1)
}

slope() { awk -v a="$1" -v b="$2" 'BEGIN{ if (a+0<=0) print 9999; else printf "%.2f", (b+0)/(a+0) }'; }

rc_all=0

# --- RED arm ---------------------------------------------------------------
echo "=== RED ARM: allocator at $RED_REV (pre-fix) must FAIL the slope gate ==="
if ! git -C "$REPO" show "$RED_REV:userland/libc/stdlib.c" > "$WORK/red_raw.c" 2>"$WORK/git.log"; then
    echo "RESULT: BROKEN - cannot fetch $RED_REV:userland/libc/stdlib.c"
    sed -n 1,5p "$WORK/git.log"
    echo "        Point MALLOC_SCALE_RED_REV at a revision that predates the"
    echo "        size-binned free list, or this gate has no RED arm."
    exit 1
fi
if grep -q 'bin_insert' "$WORK/red_raw.c"; then
    echo "RESULT: BROKEN - $RED_REV already HAS the fix, so it is not a RED arm."
    exit 1
fi
# Teach the pre-fix allocator to count the steps it already takes. Its walks use
# the same `if (++steps > limit)` form, so one sed reaches all of them.
sed -e 's/^static unsigned g_walk_reports = 0;/static unsigned g_walk_reports = 0;\nstatic uint64_t g_walk_steps = 0;\nuint64_t __malloc_walk_steps(void) { return g_walk_steps; }/' \
    -e 's/if (++steps > limit)/if (g_walk_steps++, ++steps > limit)/' \
    "$WORK/red_raw.c" > "$WORK/red.c"
RSUB=$(grep -c 'g_walk_steps++' "$WORK/red.c")
if [ "$RSUB" -lt 3 ]; then
    echo "RESULT: BROKEN - instrumented only $RSUB walk sites in $RED_REV (expected >=3)."
    echo "        The RED arm would then under-count and could pass by accident."
    exit 1
fi
if ! build "$WORK/red.c" "$WORK/t_red"; then
    echo "RESULT: BROKEN - RED arm did not build"; exit 1
fi
run_arm "$WORK/t_red" "$N";        r1=$STEPS; rs1=$SECS
run_arm "$WORK/t_red" "$((N*4))";  r4=$STEPS; rs4=$SECS
if [ -z "${r1:-}" ] || [ -z "${r4:-}" ]; then
    echo "RESULT: BROKEN - RED arm produced no measurement"; exit 1
fi
rslope=$(slope "$r1" "$r4")
echo "  RED slope (steps, 4N/N) = $rslope   [time ${rs1}s -> ${rs4}s]"
if awk -v s="$rslope" -v m="$SLOPE_MAX" 'BEGIN{exit !(s>m)}'; then
    echo "RESULT: GOOD - the pre-fix allocator is superlinear, so this gate CAN fail."
else
    echo "RESULT: BAD - the pre-fix allocator did not exceed the threshold."
    echo "        Either N is too small to leave the flat regime or SLOPE_MAX is"
    echo "        too loose. Fix that before trusting the GREEN arm."
    rc_all=1
fi

echo
# --- GREEN arm -------------------------------------------------------------
echo "=== GREEN ARM: live $LIBC/stdlib.c, bounded-walk steps must stay flat ==="
if ! build "$LIBC/stdlib.c" "$WORK/t_green"; then
    echo "RESULT: FAIL - green arm did not build"; exit 1
fi
run_arm "$WORK/t_green" "$N";       g1=$STEPS; gs1=$SECS
run_arm "$WORK/t_green" "$((N*4))"; g4=$STEPS; gs4=$SECS
if [ -z "${g1:-}" ] || [ -z "${g4:-}" ]; then
    echo "RESULT: FAIL - green arm produced no measurement (see output above)"
    exit 1
fi
# A workload that walks essentially nothing has a tiny, noisy denominator, so
# call that flat by inspection rather than dividing by it.
if [ "$g1" -lt 1000 ] && [ "$g4" -lt 1000 ]; then
    echo "  GREEN steps: N=$g1  4N=$g4   [time ${gs1}s -> ${gs4}s]"
    echo "  both under 1000 at any N: per-allocation list walking is gone"
    echo "RESULT: PASS"
else
    gslope=$(slope "$g1" "$g4")
    echo "  GREEN slope (steps, 4N/N) = $gslope  (limit $SLOPE_MAX)   [time ${gs1}s -> ${gs4}s]"
    if awk -v s="$gslope" -v m="$SLOPE_MAX" 'BEGIN{exit !(s<=m)}'; then
        echo "RESULT: PASS"
    else
        echo "RESULT: FAIL - per-allocation work grows with the live block count again."
        rc_all=1
    fi
fi

echo
[ $rc_all -eq 0 ] && echo "#245 allocator scale gate: PASS" || echo "#245 allocator scale gate: FAIL"
exit $rc_all
