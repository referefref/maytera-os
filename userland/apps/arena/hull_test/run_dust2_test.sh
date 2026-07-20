#!/bin/sh
# run_dust2_test.sh - #491 Stage 2 REAL-MAP boundary test + negative control.
#
# Builds the IDENTICAL Rust crate source for the HOST target (same technique as
# run_hull_test.sh / ../bsp_test/) and runs it against the REAL de_dust2 BSP.
#
# Two phases, and BOTH must come out as stated or the run is not evidence:
#   PHASE 1 (real trace)      -> must PASS
#   PHASE 2 (negative control)-> must FAIL
# Phase 2 relinks the SAME test against a stub bsp_hull_trace that always reports
# "never hit" (frac=1.0), which is EXACTLY the Stage-1 behaviour the user hit
# ("no ground clipping so i can fly through walls and floor"). If the test still
# passed against that stub it would be proving nothing, so phase 2 failing is a
# required part of the result, not a bonus.
set -e
cd "$(dirname "$0")"
MAP="${1:-/root/MAP.BSP}"

rustc --edition 2021 --crate-type staticlib --target x86_64-unknown-linux-gnu \
      -C panic=abort -C opt-level=2 -o libarena_rs_host.a ../arena_rs.rs

echo "=============================================================="
echo " PHASE 1: REAL Rust hull trace vs real de_dust2 (must PASS)"
echo "=============================================================="
gcc -g -O1 dust2_test.c libarena_rs_host.a -o dust2_test -lm
REAL_RC=0
./dust2_test "$MAP" || REAL_RC=$?

echo
echo "=============================================================="
echo " PHASE 1b: the OLD Stage-1 spawn adjust (+24, a free-fly camera"
echo "           lift) measured against the same 40 real CS spawns,"
echo "           for comparison with the GoldSrc-correct -36."
echo "=============================================================="
gcc -g -O1 -DSPAWN_Z_ADJ=24.0f dust2_test.c libarena_rs_host.a -o dust2_test_oldspawn -lm
./dust2_test_oldspawn "$MAP" 2>&1 | sed -n '/1b\. INTEGRATION/,/^$/p' || true

echo
echo "=============================================================="
echo " PHASE 2: NEGATIVE CONTROL - stub trace that never hits."
echo "          This is the Stage-1 bug. The test MUST FAIL here;"
echo "          if it passes, the test is vacuous and proves nothing."
echo "=============================================================="
cat > /tmp/neverhit_stub.c <<'EOF'
#include <stdint.h>
#include <string.h>
typedef struct { uint32_t x, y, z; } BspVec3;
typedef struct { uint32_t frac; BspVec3 end; BspVec3 normal; uint32_t start_solid, all_solid; } HullTrace;
/* The pre-fix world: nothing is ever solid, every sweep is a clean miss. */
int32_t bsp_hull_trace(const void *scene, int32_t hull,
                       uint32_t p1x, uint32_t p1y, uint32_t p1z,
                       uint32_t p2x, uint32_t p2y, uint32_t p2z, HullTrace *out) {
    union { float f; uint32_t u; } one; one.f = 1.0f;
    (void)scene; (void)hull; (void)p1x; (void)p1y; (void)p1z;
    memset(out, 0, sizeof(*out));
    out->frac = one.u;
    out->end.x = p2x; out->end.y = p2y; out->end.z = p2z;
    return 0;
}
EOF
gcc -g -O1 -c /tmp/neverhit_stub.c -o /tmp/neverhit_stub.o
gcc -g -O1 /tmp/neverhit_stub.o dust2_test.c libarena_rs_host.a \
    -Wl,--allow-multiple-definition -o dust2_test_neg -lm
NEG_RC=0
./dust2_test_neg "$MAP" || NEG_RC=$?

echo
echo "=============================================================="
echo " VERDICT"
echo "=============================================================="
echo "  phase 1 (real trace)       exit=$REAL_RC  (want 0 = PASS)"
echo "  phase 2 (never-hit stub)   exit=$NEG_RC  (want non-zero = FAIL)"
if [ "$REAL_RC" -eq 0 ] && [ "$NEG_RC" -ne 0 ]; then
    echo "  RESULT: OK - real trace collides, and the test provably detects its absence."
    exit 0
fi
echo "  RESULT: BAD - either the real trace is broken, or the test is vacuous."
exit 1
