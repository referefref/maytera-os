#!/bin/sh
# run_side1_test.sh - #568 regression test + negative control, same technique
# as run_dust2_test.sh's phase 2: build the test against BOTH a pre-fix and
# the real post-fix bsp.rs (identical Rust source, host target) and require
# the pre-fix build to FAIL this test and the post-fix build to PASS it. If
# the pre-fix build also passed, the test would be proving nothing.
#
# The pre-fix source is derived ON THE FLY from the real ../bsp.rs (never a
# frozen duplicate file living in the tree - that would bit-rot the moment
# bsp.rs changes again and silently stop proving anything). It works by
# textually reverting the exact fixed block back to the original one-line bug
# and ERRORS OUT if that anchor text is not found, rather than silently
# testing nothing.
set -e
cd "$(dirname "$0")"
python3 gen_side1_fixture.py side1_floor.bsp

echo "=============================================================="
echo " Deriving the PRE-FIX source (negative control) from ../bsp.rs"
echo "=============================================================="
python3 - <<'PYEOF'
import sys
src = open("../bsp.rs").read()
FIXED = """    let n = plane_normal(plane);
    st.normal = if side == 0 { n } else { [-n[0], -n[1], -n[2]] };"""
BUGGY = "    st.normal = plane_normal(plane);"
if FIXED not in src:
    sys.exit("ERROR: the #568 fixed block was not found verbatim in ../bsp.rs "
             "(it moved or changed) - refusing to fabricate a fake negative "
             "control. Update this anchor to match the current fix.")
prefix_src = src.replace(FIXED, BUGGY, 1)
open("/tmp/bsp_prefix_568.rs", "w").write(prefix_src)
print("wrote /tmp/bsp_prefix_568.rs (pre-fix: side==1 normal negation reverted)")
PYEOF

# arena_rs.rs unchanged either way (mod bsp; the fix is entirely inside bsp.rs),
# but rustc needs both files under the SAME directory since bsp.rs is `mod bsp;`.
mkdir -p /tmp/bsp_prefix_568_dir
cp /tmp/bsp_prefix_568.rs /tmp/bsp_prefix_568_dir/bsp.rs
cp ../arena_rs.rs /tmp/bsp_prefix_568_dir/arena_rs.rs

echo "=============================================================="
echo " PRE-FIX (negative control): must FAIL"
echo "=============================================================="
rustc --edition 2021 --crate-type staticlib --target x86_64-unknown-linux-gnu \
      -C panic=abort -C opt-level=2 -o libarena_rs_prefix.a \
      /tmp/bsp_prefix_568_dir/arena_rs.rs
gcc -g -O1 side1_test.c libarena_rs_prefix.a -o side1_test_prefix -lm
PRE_RC=0
./side1_test_prefix side1_floor.bsp || PRE_RC=$?

echo
echo "=============================================================="
echo " POST-FIX (the real shipping source): must PASS"
echo "=============================================================="
rustc --edition 2021 --crate-type staticlib --target x86_64-unknown-linux-gnu \
      -C panic=abort -C opt-level=2 -o libarena_rs_host.a ../arena_rs.rs
gcc -g -O1 -fsanitize=undefined side1_test.c libarena_rs_host.a -o side1_test -lm
POST_RC=0
./side1_test side1_floor.bsp || POST_RC=$?

echo
echo "=============================================================="
echo " VERDICT"
echo "=============================================================="
echo "  pre-fix  (should FAIL, non-zero) exit=$PRE_RC"
echo "  post-fix (should PASS, zero)     exit=$POST_RC"
rm -rf /tmp/bsp_prefix_568.rs /tmp/bsp_prefix_568_dir
if [ "$PRE_RC" -ne 0 ] && [ "$POST_RC" -eq 0 ]; then
    echo "  RESULT: OK - the fix is real and the test provably detects its absence."
    exit 0
fi
echo "  RESULT: BAD - either the fix did not take, or the test is vacuous."
exit 1
