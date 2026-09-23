#!/bin/bash
# run_escrow.sh - MEASURED host end-to-end test for the AI escrow/promise
# capability (#712), same rig as run_aifs.sh. The REAL units under test
# (../escrow.c, ../photorg.c, ../aiclient.c executors + gate, ../aicap.c token
# layer + the #712 capability denylist) are compiled freestanding exactly as
# userland/libc/Makefile does, then symbol-prefixed with objcopy so they link
# next to glibc. The harness (escrow_test.c) backs their file/dir/clock syscalls
# with the host and their libc primitives with host libc. Nothing reimplements
# the escrow, the organiser or the gate; the logic exercised is shipped code.
#
# THREE ARMS. All must behave as stated or this script exits non-zero:
#   POSITIVE      the owner's scenario end to end: organise photos with varied
#                 mtimes into date folders under an escrow contract and assert
#                 correct folders, bytes-intact moves, ZERO deletes, promise
#                 FULFILLED, token early-closed; the chatbot tool path; the
#                 mid-contract gate (in-scope allowed, delete + out-of-scope
#                 refused); the NEGATIVE promise (PARTIAL, grant retained); and
#                 that the no-delete policy overrides a delete consent.
#   NEG-SCOPE     recompiles ../aicap.c with constraints_ok() forced to permit.
#                 The guards scenario must then FAIL (out-of-scope refusals leak),
#                 proving they genuinely depend on the path-scope gate.
#   NEG-DELETE    recompiles ../aicap.c with the #712 cap_is_denied() forced to
#                 0. The deny_ctrl scenario must then FAIL (a delete consent leaks
#                 through), proving the no-delete policy is load-bearing.
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
    for u in aiclient aicap escrow photorg exifdate; do
        local src="$LIBC/$u.c"
        [ "$u" = aicap ] && src="$1"
        gcc $UUT_FLAGS -x c -c "$src" -o "$WORK/${u}_raw.o" 2>"$WORK/cc.$u.log" || {
            echo "  compile of $u.c FAILED:"; sed -n 1,25p "$WORK/cc.$u.log"; return 2; }
        objcopy --prefix-symbols=ad_ "$WORK/${u}_raw.o" "$WORK/$u.o" || return 2
    done
    # NOTE: the harness is HOST code (glibc headers). Do NOT add -I$LIBC here or
    # <sys/stat.h> etc. would resolve to MayteraOS's freestanding headers (a
    # different struct stat layout) while host stat() fills the glibc layout,
    # making S_ISDIR read garbage. escrow.h is pulled in via a relative quote
    # include ("../escrow.h") and needs no -I.
    gcc -m64 -O1 -g -c escrow_test.c -o "$WORK/t.o" 2>"$WORK/cc.t.log" || {
        echo "  compile of harness FAILED:"; sed -n 1,25p "$WORK/cc.t.log"; return 2; }
    gcc -m64 "$WORK/t.o" "$WORK/aiclient.o" "$WORK/aicap.o" "$WORK/escrow.o" \
        "$WORK/photorg.o" "$WORK/exifdate.o" -o "$2" 2>"$WORK/ld.log" || {
        echo "  link FAILED:"; sed -n 1,25p "$WORK/ld.log"; return 2; }
    return 0
}

run_one() {  # $1 = binary, $2 = scenario ; returns the scenario exit code
    local root; root=$(mktemp -d)
    AIFS_ROOT="$root" "$1" "$2" >"$WORK/out.$2" 2>&1
    local rc=$?
    sed 's/^/      /' "$WORK/out.$2"
    rm -rf "$root"
    return $rc
}

rc_all=0
POS="organize tool guards partial deny_ctrl exif"

echo "=== POSITIVE: live ../aicap.c, every scenario must self-assert (exit 0) ==="
if ! build "$LIBC/aicap.c" "$WORK/t_pos"; then
    echo "RESULT: BROKEN - positive arm did not build"; exit 1
fi
pos_bad=0
for n in $POS; do
    echo "  scenario $n:"; run_one "$WORK/t_pos" $n
    rc=$?
    if [ $rc -ne 0 ]; then echo "  -> $n FAIL (exit $rc)"; pos_bad=1
    else echo "  -> $n ok"; fi
done
if [ $pos_bad -eq 0 ]; then echo "RESULT: PASS - every scenario behaved as asserted."
else echo "RESULT: FAIL"; rc_all=1; fi

echo
echo "=== NEG-SCOPE: allowed_paths gate neutered, guards must FAIL ==="
sed 's/^static int constraints_ok(const aicap_token_t \*t, const char \*cap, const char \*target) {/static int constraints_ok(const aicap_token_t *t, const char *cap, const char *target) { (void)t;(void)cap;(void)target; return 1; \/* NEG-SCOPE *\//' \
    "$LIBC/aicap.c" > "$WORK/aicap_noscope.c"
if [ "$(grep -c 'NEG-SCOPE' "$WORK/aicap_noscope.c")" -lt 1 ]; then
    echo "RESULT: BROKEN - could not neuter constraints_ok(); update this test."; exit 1
fi
if ! build "$WORK/aicap_noscope.c" "$WORK/t_ns"; then
    echo "RESULT: BROKEN - neg-scope arm did not build"; exit 1
fi
echo "  scenario guards (scope gate off):"; run_one "$WORK/t_ns" guards
if [ $? -eq 0 ]; then echo "  -> LEAK: guards passed with the scope gate off, proves nothing"; rc_all=1
else echo "RESULT: GOOD - out-of-scope refusals genuinely depend on the path-scope gate."; fi

echo
echo "=== NEG-DELETE: #712 denylist neutered, deny_ctrl must FAIL ==="
sed 's/^static int cap_is_denied(const char \*cap) {/static int cap_is_denied(const char *cap) { (void)cap; return 0; \/* NEG-DELETE *\//' \
    "$LIBC/aicap.c" > "$WORK/aicap_nodeny.c"
if [ "$(grep -c 'NEG-DELETE' "$WORK/aicap_nodeny.c")" -lt 1 ]; then
    echo "RESULT: BROKEN - could not neuter cap_is_denied(); update this test."; exit 1
fi
if ! build "$WORK/aicap_nodeny.c" "$WORK/t_nd"; then
    echo "RESULT: BROKEN - neg-delete arm did not build"; exit 1
fi
echo "  scenario deny_ctrl (no-delete policy off):"; run_one "$WORK/t_nd" deny_ctrl
if [ $? -eq 0 ]; then echo "  -> LEAK: deny_ctrl passed with the no-delete policy off, proves nothing"; rc_all=1
else echo "RESULT: GOOD - the no-delete policy genuinely gates fs.delete."; fi

echo
[ $rc_all -eq 0 ] && echo "#712 AI escrow/promise end-to-end test: PASS" || echo "#712 AI escrow/promise end-to-end test: FAIL"
exit $rc_all
