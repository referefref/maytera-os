#!/bin/bash
# run_cpython_alloc.sh - #245: measure the SHARED userland allocator through
# CPython, so the claim "CPython gets materially faster" is a curve and not an
# assertion.
#
# WHY A SEPARATE RIG. run_malloc_scale.sh already gates the allocator's scaling,
# deterministically, on its own step counter. That is the right gate and this is
# NOT a second one: it decides nothing and fails nothing. It answers a different
# question, which the gate cannot: how much of the allocator's cost reaches a
# real language runtime that has its OWN allocator in front of it. CPython's
# pymalloc absorbs every request <= 512 bytes into its own pools, so the answer
# turned out to be "all of it for one shape, almost none for three others", and
# that is not something you can read off the allocator's own counters.
#
# METHOD. The REAL allocator is compiled freestanding out of ../stdlib.c (or out
# of any git revision), symbol-prefixed with `objcopy --prefix-symbols=mh_`
# exactly as run_malloc_scale.sh does, and wrapped in an LD_PRELOAD shim that
# becomes malloc/free/realloc/calloc for a host CPython 3.11.
#
# Pointer ownership is decided by ADDRESS, not by a tag: the MayteraOS heap is at
# a fixed HEAP_START 0x8040000000 and nothing else is, so any pointer outside
# that window belongs to glibc and is handed straight back to it. That is what
# keeps posix_memalign/aligned_alloc (which the allocator has no entry point for,
# so they are deliberately NOT interposed) correct rather than corrupt.
#
# WHAT MAKES A RUN TRUSTWORTHY. Two things, and both are printed:
#   - the malloc/free/realloc COUNTS must be identical between the two arms. If
#     they are not, the arms are not running the same workload and no timing
#     comparison means anything.
#   - the `small` shape is a NEGATIVE CONTROL and must NOT move. It is 3-tuples,
#     entirely inside pymalloc, so it barely reaches malloc at all. A rig where
#     the control speeds up is measuring something other than the allocator.
#
# MEASURED 2026-10-02 on the build container, host CPython 3.11.2, RED=a4cc56b7 GREEN=live:
#   churn n=5000..80000   RED 0.0831 -> 47.9469 s   fitted n^2.29
#                         GREEN 0.0088 -> 0.1481 s  fitted n^1.02   (324x at 80000)
#   records/dicts/strings  under 10% apart, dicts a tie
#   small (control)        0.0071 vs 0.0072 s, unmoved
#
# Usage:  ./run_cpython_alloc.sh [RED_REV] [shape ...]
#         N_LIST="5000 10000 20000 40000" ./run_cpython_alloc.sh
set -uo pipefail

cd "$(dirname "$0")" || exit 1
LIBC=..
REPO=$LIBC/../..
RED_REV=${1:-a4cc56b7}
shift 2>/dev/null || true
SHAPES=${*:-records churn dicts strings small}
N_LIST=${N_LIST:-5000 10000 20000 40000}

PY=$(command -v python3.11 || command -v python3)
[ -n "$PY" ] || { echo "no python3 on this host; nothing to measure"; exit 1; }
case "$("$PY" -c 'import sys;print("%d.%d"%sys.version_info[:2])')" in
  3.1[0-9]) ;;
  *) echo "NOTE: host python is $("$PY" -V), not 3.11. The shipped port is 3.11.9;"
     echo "      a different minor version changes pymalloc's thresholds, so the"
     echo "      numbers stop being comparable to the ones in the header." ;;
esac

GCCINC=$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/include 2>/dev/null | tail -1)
[ -n "$GCCINC" ] || { echo "no gcc include dir found"; exit 1; }

W=$(mktemp -d) || exit 2
trap 'rm -rf "$W"' EXIT

build_arm() {   # $1 = rev or LIVE, $2 = output .so
    local src="$W/$(basename "$2").c"
    if [ "$1" = LIVE ]; then
        cp "$LIBC/stdlib.c" "$src" || return 2
    else
        git -C "$REPO" show "$1:userland/libc/stdlib.c" > "$src" 2>/dev/null || {
            echo "  cannot fetch $1:userland/libc/stdlib.c"; return 2; }
    fi
    if ! grep -q 'bin_insert' "$src"; then
        # Pre-fix revisions have no step counter. Teach them to count the steps
        # they ALREADY take, with the same sed run_malloc_scale.sh uses, so both
        # arms report the same quantity.
        sed -i -e 's/^static unsigned g_walk_reports = 0;/static unsigned g_walk_reports = 0;\nstatic uint64_t g_walk_steps = 0;\nuint64_t __malloc_walk_steps(void) { return g_walk_steps; }/' \
               -e 's/if (++steps > limit)/if (g_walk_steps++, ++steps > limit)/' "$src"
        local n; n=$(grep -c 'g_walk_steps++' "$src")
        [ "$n" -ge 3 ] || { echo "  instrumented only $n walk sites in $1 (expected >=3); it would under-count"; return 1; }
    fi
    gcc -m64 -ffreestanding -fno-builtin -nostdinc -nostdlib -fno-stack-protector \
        -mno-red-zone -fPIC -O2 -g -w -isystem "$GCCINC" -I"$LIBC" \
        -x c -c "$src" -o "$W/a.o" 2>"$W/cc.log" || {
        echo "  allocator compile FAILED ($1):"; sed -n 1,20p "$W/cc.log"; return 2; }
    objcopy --prefix-symbols=mh_ "$W/a.o" "$W/ap.o" || return 2
    gcc -m64 -O2 -g -fPIC -c cpython_alloc_preload.c -o "$W/p.o" 2>"$W/cc2.log" || {
        echo "  preload compile FAILED:"; sed -n 1,20p "$W/cc2.log"; return 2; }
    gcc -shared -o "$2" "$W/p.o" "$W/ap.o" -ldl 2>"$W/ld.log" || {
        echo "  link FAILED:"; sed -n 1,20p "$W/ld.log"; return 2; }
    return 0
}

echo "=== building arms: RED=$RED_REV, GREEN=live $LIBC/stdlib.c ==="
build_arm "$RED_REV" "$W/red.so"  || { echo "RED arm did not build"; exit 1; }
build_arm LIVE       "$W/green.so" || { echo "GREEN arm did not build"; exit 1; }
if grep -q bin_insert "$W/red.so.c" 2>/dev/null; then
    echo "REFUSING: $RED_REV already HAS the size-binned free list, so it is not a RED arm."
    exit 1
fi
echo

printf '%-9s %8s  %10s %10s %7s  %14s %12s\n' shape n RED_s GREEN_s speedup RED_steps GREEN_steps
for sh in $SHAPES; do
    for n in $N_LIST; do
        ro=$(LD_PRELOAD="$W/red.so"   "$PY" cpython_alloc_workload.py "$sh" "$n" 2>&1)
        go=$(LD_PRELOAD="$W/green.so" "$PY" cpython_alloc_workload.py "$sh" "$n" 2>&1)
        rt=$(echo "$ro" | grep -v '^\[RIG\]' | awk '{print $3}')
        gt=$(echo "$go" | grep -v '^\[RIG\]' | awk '{print $3}')
        rs=$(echo "$ro" | sed -n 's/.*walk_steps=\([0-9]*\).*/\1/p')
        gs=$(echo "$go" | sed -n 's/.*walk_steps=\([0-9]*\).*/\1/p')
        rc=$(echo "$ro" | sed -n 's/^\[RIG\] \(mallocs=[0-9]* callocs=[0-9]* reallocs=[0-9]*\).*/\1/p')
        gc=$(echo "$go" | sed -n 's/^\[RIG\] \(mallocs=[0-9]* callocs=[0-9]* reallocs=[0-9]*\).*/\1/p')
        if [ -z "$rt" ] || [ -z "$gt" ]; then
            printf '%-9s %8s  MEASUREMENT FAILED\n' "$sh" "$n"
            echo "$ro" | sed 's/^/      RED   /'; echo "$go" | sed 's/^/      GREEN /'
            continue
        fi
        sp=$(awk -v a="$rt" -v b="$gt" 'BEGIN{ if (b+0<=0) print "-"; else printf "%.1fx", a/b }')
        printf '%-9s %8s  %10s %10s %7s  %14s %12s' "$sh" "$n" "$rt" "$gt" "$sp" "$rs" "$gs"
        # The arms must have run the SAME workload or the timings are not comparable.
        if [ "$rc" != "$gc" ]; then printf '   *** ARMS DIVERGED: [%s] vs [%s]' "$rc" "$gc"; fi
        printf '\n'
    done
done
echo
echo "The 'small' row is the NEGATIVE CONTROL: it must show roughly 1.0x. If it"
echo "does not, pymalloc is not absorbing it and this rig is measuring something"
echo "other than the allocator."
