#!/bin/bash
# run_aidev.sh - host unit test for the per-device AI capability manifest (owner req #6).
#
# The REAL units under test (../aidev.c and ../aicap.c) are compiled freestanding
# exactly as userland/libc/Makefile does, then symbol-prefixed with objcopy so
# they can be linked next to glibc. The harness (aidev_test.c) backs their file
# and clock syscalls with the host and their libc primitives with host libc.
# Nothing reimplements the manifest or the gate; the decision logic is shipped
# code.
#
# TWO ARMS. Both must behave as stated or this script exits non-zero:
#
#   POSITIVE  runs every scenario against the live ../aidev.c. Each scenario
#             self-asserts its expected ALLOW/DENY (RED deny arms 2,4,5,7 and
#             GREEN allow arms 1,3,6, plus logic arm 8) and exits 0 only on a
#             match. Any non-zero scenario fails the run.
#
#   NEGATIVE CONTROL  recompiles ../aidev.c with the manifest FORBID gate
#             neutered (policy forced to ALLOW). Scenarios 4 and 5, which require
#             a FORBID verb to be DENIED, must then FAIL (exit non-zero). If they
#             still pass with the gate disabled, the test is not exercising the
#             gate and proves nothing, so that is a failure too.
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

build() {   # $1 = aidev source to use, $2 = output binary
    gcc $UUT_FLAGS -x c -c "$1" -o "$WORK/aidev_raw.o" 2>"$WORK/cc1.log" || {
        echo "  compile of $1 FAILED:"; sed -n 1,25p "$WORK/cc1.log"; return 2; }
    gcc $UUT_FLAGS -x c -c "$LIBC/aicap.c" -o "$WORK/aicap_raw.o" 2>"$WORK/cc2.log" || {
        echo "  compile of aicap.c FAILED:"; sed -n 1,25p "$WORK/cc2.log"; return 2; }
    objcopy --prefix-symbols=ad_ "$WORK/aidev_raw.o" "$WORK/aidev.o" || return 2
    objcopy --prefix-symbols=ad_ "$WORK/aicap_raw.o" "$WORK/aicap.o" || return 2
    gcc -m64 -O1 -g -c aidev_test.c -o "$WORK/t.o" 2>"$WORK/cc3.log" || {
        echo "  compile of harness FAILED:"; sed -n 1,25p "$WORK/cc3.log"; return 2; }
    gcc -m64 "$WORK/t.o" "$WORK/aidev.o" "$WORK/aicap.o" -o "$2" 2>"$WORK/ld.log" || {
        echo "  link FAILED:"; sed -n 1,25p "$WORK/ld.log"; return 2; }
    return 0
}

run_one() {  # $1 = binary, $2 = scenario number ; echoes exit code
    local root; root=$(mktemp -d)
    AIDEV_ROOT="$root" "$1" "$2" >"$WORK/out.$2" 2>&1
    local rc=$?
    sed 's/^/      /' "$WORK/out.$2"
    rm -rf "$root"
    return $rc
}

rc_all=0

echo "=== POSITIVE: live ../aidev.c, every scenario must self-assert (exit 0) ==="
if ! build "$LIBC/aidev.c" "$WORK/t_pos"; then
    echo "RESULT: BROKEN - positive arm did not build"; exit 1
fi
pos_bad=0
for n in 1 2 3 4 5 6 7 8; do
    run_one "$WORK/t_pos" $n
    rc=$?
    if [ $rc -ne 0 ]; then echo "  scenario $n: FAIL (exit $rc)"; pos_bad=1
    else echo "  scenario $n: ok"; fi
done
if [ $pos_bad -eq 0 ]; then echo "RESULT: PASS - every scenario behaved as asserted."
else echo "RESULT: FAIL"; rc_all=1; fi

echo
echo "=== NEGATIVE CONTROL: FORBID gate neutered, deny scenarios must FAIL ==="
# Force the manifest policy to ALLOW so FORBID can never fire. Everything else
# (override parse, class table, consent deferral) stays; the point is that the
# forbid decision specifically is what the deny scenarios depend on.
sed 's/    int pol = aidev_verb_policy(dev_class, dev_id, verb);/    int pol = AIDEV_ALLOW; (void)aidev_verb_policy;  \/* NEGATIVE CONTROL *\//' \
    "$LIBC/aidev.c" > "$WORK/aidev_noforbid.c"
NSUB=$(grep -c 'NEGATIVE CONTROL' "$WORK/aidev_noforbid.c")
if [ "$NSUB" -lt 1 ]; then
    echo "RESULT: BROKEN - could not neuter the FORBID gate; the checkpoint was"
    echo "        restructured. Update this test before trusting it."
    exit 1
fi
if ! build "$WORK/aidev_noforbid.c" "$WORK/t_neg"; then
    echo "RESULT: BROKEN - negative arm did not build"; exit 1
fi
neg_bad=0
for n in 4 5; do
    run_one "$WORK/t_neg" $n
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "  scenario $n: LEAK - denied nothing with the gate off, proves nothing"; neg_bad=1
    else
        echo "  scenario $n: correctly FAILED with the gate off (exit $rc)"
    fi
done
if [ $neg_bad -eq 0 ]; then echo "RESULT: GOOD - the deny scenarios genuinely depend on the FORBID gate."
else echo "RESULT: BAD - a deny scenario still passed with the gate disabled."; rc_all=1; fi

echo
[ $rc_all -eq 0 ] && echo "owner req #6 device-manifest unit test: PASS" || echo "owner req #6 device-manifest unit test: FAIL"
exit $rc_all
