#!/bin/bash
# run_ctpromise.sh - MEASURED host test that the ESCROW LAYER CONSUMES a per-item
# contract promise (#<config-ref>, task 2 of docs/CONTRACT_COVERAGE_PLAN.md).
# Same rig as run_escrow.sh: the SHIPPED units (../contract.c, ../ctpromise.c,
# ../escrow.c, ../aicap.c) are compiled freestanding exactly as
# userland/libc/Makefile does, symbol-prefixed with objcopy so they link next to
# glibc, and driven by ctpromise_test.c which backs their syscalls with the host.
#
# FIVE POSITIVE SCENARIOS plus ONE NEGATIVE ARM, all of which must behave as
# stated or this script exits non-zero:
#
#   fulfilled / unmet / vacuous / nobackend / moved / timing / describe
#                                                    (see ctpromise_test.c)
#
#   NEG-ORACLE  recompiles ../escrow.c with escrow_verify() forced to report
#               ESCROW_FULFILLED unconditionally. The `unmet` scenario must then
#               FAIL, because a lying action would be reported as a success. That
#               is what proves the verdict on the reply line is genuinely the
#               ESCROW ORACLE's finding and not something contract.c computed for
#               itself: without this arm, a green run would be consistent with a
#               second, parallel promise check, which is precisely the mistake
#               task 2 says not to make.
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

build() {   # $1 = escrow source to use, $2 = output binary
    for u in contract ctpromise escrow aicap; do
        local src="$LIBC/$u.c"
        [ "$u" = escrow ] && src="$1"
        gcc $UUT_FLAGS -x c -c "$src" -o "$WORK/${u}_raw.o" 2>"$WORK/cc.$u.log" || {
            echo "  compile of $u.c FAILED:"; sed -n 1,25p "$WORK/cc.$u.log"; return 2; }
        objcopy --prefix-symbols=ad_ "$WORK/${u}_raw.o" "$WORK/$u.o" || return 2
    done
    # The harness is HOST code (glibc headers). Do NOT add -I$LIBC: <sys/stat.h>
    # would resolve to MayteraOS's freestanding headers (a different struct stat)
    # while host stat() fills the glibc layout. contract.h and escrow.h come in
    # through relative quote includes and need no -I.
    gcc -m64 -O1 -g -c ctpromise_test.c -o "$WORK/t.o" 2>"$WORK/cc.t.log" || {
        echo "  compile of harness FAILED:"; sed -n 1,25p "$WORK/cc.t.log"; return 2; }
    gcc -m64 "$WORK/t.o" "$WORK/contract.o" "$WORK/ctpromise.o" \
        "$WORK/escrow.o" "$WORK/aicap.o" -o "$2" 2>"$WORK/ld.log" || {
        echo "  link FAILED:"; sed -n 1,25p "$WORK/ld.log"; return 2; }
    return 0
}

run_one() {  # $1 = binary, $2 = scenario
    local root; root=$(mktemp -d)
    AIFS_ROOT="$root" "$1" "$2" >"$WORK/out.$2" 2>&1
    local rc=$?
    sed 's/^/      /' "$WORK/out.$2"
    rm -rf "$root"
    return $rc
}

rc_all=0
POS="fulfilled unmet vacuous nobackend moved timing describe"

echo "=== POSITIVE: live ../escrow.c, every scenario must self-assert (exit 0) ==="
if ! build "$LIBC/escrow.c" "$WORK/t_pos"; then
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
echo "=== NEG-ORACLE: escrow_verify() forced FULFILLED, 'unmet' must FAIL ==="
sed 's/^int escrow_verify(escrow_contract_t \*c, escrow_verify_t \*out) {/int escrow_verify(escrow_contract_t *c, escrow_verify_t *out) { (void)c; if(out){ __builtin_memset(out,0,sizeof(*out)); } return ESCROW_FULFILLED; \/* NEG-ORACLE *\//' \
    "$LIBC/escrow.c" > "$WORK/escrow_blind.c"
if [ "$(grep -c 'NEG-ORACLE' "$WORK/escrow_blind.c")" -lt 1 ]; then
    echo "RESULT: BROKEN - could not neuter escrow_verify(); update this test."; exit 1
fi
if ! build "$WORK/escrow_blind.c" "$WORK/t_no"; then
    echo "RESULT: BROKEN - neg-oracle arm did not build"; exit 1
fi
echo "  scenario unmet (escrow oracle blinded):"; run_one "$WORK/t_no" unmet
if [ $? -eq 0 ]; then
    echo "  -> LEAK: 'unmet' passed with the escrow oracle blinded, so the"
    echo "     verdict on the reply line is NOT the escrow's finding."; rc_all=1
else
    echo "RESULT: GOOD - the call outcome genuinely depends on escrow_verify()."
fi

echo
[ $rc_all -eq 0 ] && echo "contract per-item promise end-to-end test: PASS" \
                  || echo "contract per-item promise end-to-end test: FAIL"
exit $rc_all
