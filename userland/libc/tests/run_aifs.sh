#!/bin/bash
# run_aifs.sh - host unit test for the files.mkdir / files.move AI tools (#711).
#
# The REAL units under test (../aiclient.c executors + gate, ../aicap.c gate +
# the both-ends aicap_path_in_scope predicate) are compiled freestanding exactly
# as userland/libc/Makefile does, then symbol-prefixed with objcopy so they link
# next to glibc. The harness (aifs_test.c) backs their file + clock syscalls with
# the host and their libc primitives with host libc. Nothing reimplements an
# executor or the gate; the logic exercised is shipped code.
#
# TWO ARMS. Both must behave as stated or this script exits non-zero:
#   POSITIVE       every scenario must self-assert (exit 0): two GREEN grants
#                  (mkdir creates, move relocates bytes-identical with ZERO
#                  deletes) and three RED refusals (dest out of scope, source out
#                  of scope, mkdir out of scope) each leaving the FS unchanged.
#   NEGATIVE CTRL  recompiles ../aicap.c with constraints_ok() (the allowed_paths
#                  enforcement) forced to permit. The three RED scenarios must
#                  then FAIL (they would leak), proving they genuinely depend on
#                  the path-scope gate and are not passing for some other reason.
set -u
cd "$(dirname "$0")" || exit 1
LIBC=..
GCCINC=$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/include 2>/dev/null | tail -1)
[ -n "$GCCINC" ] || { echo "no gcc include dir found"; exit 1; }

UUT_FLAGS="-m64 -ffreestanding -fno-builtin -nostdinc -nostdlib \
           -fno-stack-protector -mno-red-zone -Wall -O1 -g \
           -isystem $GCCINC -I$LIBC"

WORK=$(mktemp -d) || exit 2
trap 'rm -rf "$WORK"' EXIT

build() {   # $1 = aicap source to use, $2 = output binary
    gcc $UUT_FLAGS -x c -c "$LIBC/aiclient.c" -o "$WORK/aiclient_raw.o" 2>"$WORK/cc1.log" || {
        echo "  compile of aiclient.c FAILED:"; sed -n 1,25p "$WORK/cc1.log"; return 2; }
    gcc $UUT_FLAGS -x c -c "$1" -o "$WORK/aicap_raw.o" 2>"$WORK/cc2.log" || {
        echo "  compile of aicap.c FAILED:"; sed -n 1,25p "$WORK/cc2.log"; return 2; }
    objcopy --prefix-symbols=ad_ "$WORK/aiclient_raw.o" "$WORK/aiclient.o" || return 2
    objcopy --prefix-symbols=ad_ "$WORK/aicap_raw.o" "$WORK/aicap.o" || return 2
    gcc -m64 -O1 -g -c aifs_test.c -o "$WORK/t.o" 2>"$WORK/cc3.log" || {
        echo "  compile of harness FAILED:"; sed -n 1,25p "$WORK/cc3.log"; return 2; }
    gcc -m64 "$WORK/t.o" "$WORK/aiclient.o" "$WORK/aicap.o" -o "$2" 2>"$WORK/ld.log" || {
        echo "  link FAILED:"; sed -n 1,25p "$WORK/ld.log"; return 2; }
    return 0
}

run_one() {  # $1 = binary, $2 = scenario ; echoes exit code
    local root; root=$(mktemp -d)
    AIFS_ROOT="$root" "$1" "$2" >"$WORK/out.$2" 2>&1
    local rc=$?
    sed 's/^/      /' "$WORK/out.$2"
    rm -rf "$root"
    return $rc
}

rc_all=0
GREEN="mkdir_grant move_grant"
RED="move_deny_dst move_deny_src mkdir_deny"

echo "=== POSITIVE: live ../aicap.c, every scenario must self-assert (exit 0) ==="
if ! build "$LIBC/aicap.c" "$WORK/t_pos"; then
    echo "RESULT: BROKEN - positive arm did not build"; exit 1
fi
pos_bad=0
for n in $GREEN $RED; do
    echo "  scenario $n:"; run_one "$WORK/t_pos" $n
    rc=$?
    if [ $rc -ne 0 ]; then echo "  -> $n FAIL (exit $rc)"; pos_bad=1
    else echo "  -> $n ok"; fi
done
if [ $pos_bad -eq 0 ]; then echo "RESULT: PASS - every scenario behaved as asserted."
else echo "RESULT: FAIL"; rc_all=1; fi

echo
echo "=== NEGATIVE CONTROL: allowed_paths gate neutered, RED scenarios must FAIL ==="
sed 's/^static int constraints_ok(const aicap_token_t \*t, const char \*cap, const char \*target) {/static int constraints_ok(const aicap_token_t *t, const char *cap, const char *target) { (void)t;(void)cap;(void)target; return 1; \/* NEGATIVE CONTROL *\//' \
    "$LIBC/aicap.c" > "$WORK/aicap_noscope.c"
NSUB=$(grep -c 'NEGATIVE CONTROL' "$WORK/aicap_noscope.c")
if [ "$NSUB" -lt 1 ]; then
    echo "RESULT: BROKEN - could not neuter constraints_ok(); it was restructured."
    echo "        Update this test before trusting it."; exit 1
fi
if ! build "$WORK/aicap_noscope.c" "$WORK/t_neg"; then
    echo "RESULT: BROKEN - negative arm did not build"; exit 1
fi
neg_bad=0
for n in $RED; do
    echo "  scenario $n (gate off):"; run_one "$WORK/t_neg" $n
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "  -> $n LEAK - refused nothing with the scope gate off, proves nothing"; neg_bad=1
    else
        echo "  -> $n correctly FAILED with the gate off (exit $rc)"
    fi
done
if [ $neg_bad -eq 0 ]; then echo "RESULT: GOOD - the RED scenarios genuinely depend on the path-scope gate."
else echo "RESULT: BAD - a RED scenario still passed with the gate disabled."; rc_all=1; fi

echo
[ $rc_all -eq 0 ] && echo "#711 files.mkdir/files.move AI-tool unit test: PASS" || echo "#711 files.mkdir/files.move AI-tool unit test: FAIL"
exit $rc_all
