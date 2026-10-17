#!/bin/bash
# run_vma_scale.sh - #245 regression gate: the kernel's per-mmap and per-fault
# VMA work must not grow with the number of mappings already established.
#
# TWO ARMS, and the RED one uses the REAL pre-fix mm/demand.c pulled out of git,
# not a hand-written imitation of it. A gate that has never been seen to fail is
# not a gate. This is deliberately the same shape as
# userland/libc/tests/run_malloc_scale.sh, which gates the first half of #245.
#
#   RED    mm/demand.c as of $RED_REV, before do_mmap() coalesced. Its VMA
#          node-visit count must GROW superlinearly with heap size, i.e. exceed
#          the slope limit. If it does not, either the threshold is meaningless
#          or the revision is wrong, and this script says so and stops.
#
#   GREEN  the live ../demand.c. The same count at 4S must be at most SLOPE_MAX
#          x the count at S. Perfectly linear would be 4x.
#
# BOTH arms gate on a DETERMINISTIC node-visit counter, never on wall clock: on
# this shared build host the same run varies several-fold and a flaky gate gets
# switched off. Neither revision carries that counter, so this script INJECTS
# the identical counter into both with the same four seds, and REFUSES to run if
# any of the four walk functions came out uninstrumented. The wall-clock numbers
# are printed for information and decide nothing.
set -u

cd "$(dirname "$0")" || exit 1
MM=..
REPO=$MM/../..
MB=${MB:-8}                           # heap size, MB, for the small point
SLOPE_MAX=${SLOPE_MAX:-6}             # linear is 4; the pre-fix kernel is ~16
RED_REV=${VMA_SCALE_RED_REV:-555ee521}

WORK=$(mktemp -d) || exit 2
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------- extraction
# Pull a function out of a demand.c VERBATIM: from its signature line to the
# first line that is exactly "}". Nothing in this file has a brace in column 0
# inside a function body.
grab_fn() {  # $1=file  $2=anchored signature regex
    awk -v s="$2" 'BEGIN{on=0} !on && $0 ~ s {on=1} on{print} on && /^}$/{exit}' "$1"
}

# Pull a typedef struct out of demand.h verbatim, by its closing line.
grab_struct() {  # $1=file  $2=closing line, e.g. "} vma_t;"
    awk -v cl="$2" '
        /^typedef struct( vma)? \{$/ { buf=""; inb=1 }
        inb { buf = buf $0 "\n" }
        inb && $0 == cl { printf "%s", buf; exit }
    ' "$1"
}

build_defs() {  # $1 = demand.h to read
    {
        grep -E '^#define (VMA_|PF_|PAGE_STATE_)' "$1"
        echo
        grab_struct "$1" "} vma_t;"
        echo
        grab_struct "$1" "} mm_struct_t;"
    } > "$WORK/vma_defs.inc"
    grep -q 'vma_t;' "$WORK/vma_defs.inc" && grep -q 'mm_struct_t;' "$WORK/vma_defs.inc"
}

# The four list walks that decide the scaling, in dependency order.
FNS=(
    '^vma_t \*vma_node_alloc\('
    '^vma_t \*vma_find\('
    '^static vma_t \*vma_find_range_nolock\('
    '^static int vma_add_nolock\('
    '^int vma_merge\('
    '^static uint64_t mm_scan_gap_locked\('
    '^static uint64_t mm_place_anon_locked\('
    '^uint64_t do_mmap\('
)
# Functions that MUST end up carrying the instrument, with the name used in the
# diagnostic. Checked after instrumentation; a miss is BROKEN, not a pass.
WALKFNS=(
    '^vma_t \*vma_find\('
    '^static vma_t \*vma_find_range_nolock\('
    '^static int vma_add_nolock\('
    '^static uint64_t mm_scan_gap_locked\('
)

instrument() {  # $1 = demand.c in, $2 = demand.c out
    sed -e 's/^        prev = curr;$/        VMAW_STEP(); prev = curr;/' \
        -e 's/^        vma = vma->next;$/        VMAW_STEP(); vma = vma->next;/' \
        -e 's/^            vma = vma->next;$/            VMAW_STEP(); vma = vma->next;/' \
        -e 's/for (vma_t \*v = mm->vma_list; v; v = v->next) {/for (vma_t *v = mm->vma_list; v; v = v->next) { VMAW_STEP();/' \
        "$1" > "$2"
    local f missing=""
    for f in "${WALKFNS[@]}"; do
        if ! grab_fn "$2" "$f" | grep -q 'VMAW_STEP'; then
            missing="$missing $f"
        fi
    done
    if [ -n "$missing" ]; then
        echo "  INSTRUMENTATION MISS:$missing"
        echo "  The walk loops in this revision do not match the seds in this"
        echo "  script, so the arm would UNDER-COUNT and could pass by accident."
        return 1
    fi
    return 0
}

build() {  # $1 = demand.c (already instrumented), $2 = output binary
    : > "$WORK/vma_scale_body.inc"
    local sig
    for sig in "${FNS[@]}"; do
        local body
        body=$(grab_fn "$1" "$sig")
        if [ -z "$body" ]; then
            echo "  EXTRACTION FAILED for $sig"; return 2
        fi
        printf '%s\n\n' "$body" >> "$WORK/vma_scale_body.inc"
    done
    cp vma_scale_shim.h "$WORK/" || return 2
    gcc -m64 -O2 -g -Wall -Wextra -Wno-unused-function \
        -I"$WORK" -I. vma_scale_test.c -o "$2" 2>"$WORK/cc.log" || {
        echo "  compile FAILED:"; sed -n 1,30p "$WORK/cc.log"; return 2; }
    return 0
}

run_arm() {  # $1 = binary, $2 = MB ; prints the line, sets STEPS/VMAS/SPP/SECS
    local out
    out=$("$1" "$2" 2>&1)
    echo "$out" | sed 's/^/      /'
    STEPS=$(echo "$out" | sed -n 's/.*steps=\([0-9]*\).*/\1/p' | tail -1)
    VMAS=$(echo "$out"  | sed -n 's/.*vmas=\([0-9]*\).*/\1/p'  | tail -1)
    SPP=$(echo "$out"   | sed -n 's/.*spp=\([0-9]*\).*/\1/p'   | tail -1)
    SECS=$(echo "$out"  | sed -n 's/.*secs=\([0-9.]*\).*/\1/p' | tail -1)
}

slope() { awk -v a="$1" -v b="$2" 'BEGIN{ if (a+0<=0) print 9999; else printf "%.2f", (b+0)/(a+0) }'; }

rc_all=0

# --- RED arm ---------------------------------------------------------------
echo "=== RED ARM: mm/demand.c at $RED_REV (pre-fix) must FAIL the slope gate ==="
if ! git -C "$REPO" show "$RED_REV:kernel/mm/demand.c" > "$WORK/red_raw.c" 2>"$WORK/git.log"; then
    echo "RESULT: BROKEN - cannot fetch $RED_REV:kernel/mm/demand.c"
    sed -n 1,5p "$WORK/git.log"
    exit 1
fi
if ! git -C "$REPO" show "$RED_REV:kernel/mm/demand.h" > "$WORK/red_raw.h" 2>>"$WORK/git.log"; then
    echo "RESULT: BROKEN - cannot fetch $RED_REV:kernel/mm/demand.h"; exit 1
fi
if grab_fn "$WORK/red_raw.c" '^uint64_t do_mmap\(' | grep -q 'vma_merge'; then
    echo "RESULT: BROKEN - do_mmap() at $RED_REV already coalesces, so it is not a RED arm."
    exit 1
fi
build_defs "$WORK/red_raw.h" || { echo "RESULT: BROKEN - no vma_t/mm_struct_t in $RED_REV demand.h"; exit 1; }
instrument "$WORK/red_raw.c" "$WORK/red.c" || { echo "RESULT: BROKEN - RED arm uninstrumented"; exit 1; }
build "$WORK/red.c" "$WORK/t_red" || { echo "RESULT: BROKEN - RED arm did not build"; exit 1; }
run_arm "$WORK/t_red" "$MB";          r1=$STEPS; rv1=$VMAS; rp1=$SPP; rs1=$SECS
run_arm "$WORK/t_red" "$((MB*4))";    r4=$STEPS; rv4=$VMAS; rp4=$SPP; rs4=$SECS
if [ -z "${r1:-}" ] || [ -z "${r4:-}" ]; then
    echo "RESULT: BROKEN - RED arm produced no measurement"; exit 1
fi
rslope=$(slope "$r1" "$r4")
echo "  RED  slope (steps, 4S/S) = $rslope   vmas ${rv1} -> ${rv4}   steps/page ${rp1} -> ${rp4}   [time ${rs1}s -> ${rs4}s]"
if awk -v s="$rslope" -v m="$SLOPE_MAX" 'BEGIN{exit !(s>m)}'; then
    echo "RESULT: GOOD - the pre-fix kernel is superlinear, so this gate CAN fail."
else
    echo "RESULT: BAD - the pre-fix kernel did not exceed the threshold."
    echo "        Either MB is too small to leave the flat regime or SLOPE_MAX is"
    echo "        too loose. Fix that before trusting the GREEN arm."
    rc_all=1
fi

echo
# --- GREEN arm -------------------------------------------------------------
echo "=== GREEN ARM: live $MM/demand.c, VMA node visits must stay linear ==="
build_defs "$MM/demand.h" || { echo "RESULT: FAIL - no vma_t/mm_struct_t in live demand.h"; exit 1; }
instrument "$MM/demand.c" "$WORK/green.c" || { echo "RESULT: FAIL - GREEN arm uninstrumented"; exit 1; }
build "$WORK/green.c" "$WORK/t_green" || { echo "RESULT: FAIL - green arm did not build"; exit 1; }
run_arm "$WORK/t_green" "$MB";        g1=$STEPS; gv1=$VMAS; gp1=$SPP; gs1=$SECS
run_arm "$WORK/t_green" "$((MB*4))";  g4=$STEPS; gv4=$VMAS; gp4=$SPP; gs4=$SECS
if [ -z "${g1:-}" ] || [ -z "${g4:-}" ]; then
    echo "RESULT: FAIL - green arm produced no measurement (see output above)"; exit 1
fi
gslope=$(slope "$g1" "$g4")
echo "  GREEN slope (steps, 4S/S) = $gslope  (limit $SLOPE_MAX)   vmas ${gv1} -> ${gv4}   steps/page ${gp1} -> ${gp4}   [time ${gs1}s -> ${gs4}s]"
if awk -v s="$gslope" -v m="$SLOPE_MAX" 'BEGIN{exit !(s<=m)}'; then
    echo "RESULT: PASS"
else
    echo "RESULT: FAIL - per-mmap/per-fault work grows with the mapping count again."
    rc_all=1
fi

echo
[ $rc_all -eq 0 ] && echo "#245 kernel VMA scale gate: PASS" || echo "#245 kernel VMA scale gate: FAIL"
exit $rc_all
