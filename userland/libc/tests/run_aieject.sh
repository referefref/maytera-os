#!/bin/bash
# run_aieject.sh - host unit test for the FIRST real AI DEVICE executor (#708):
# device.list + device.block.eject.
#
# The REAL units under test (../aiclient.c exec_device_list/exec_device_eject +
# the run_action device routing, ../aidev.c manifest gate, ../aicap.c token+
# consent+audit gate) are compiled freestanding exactly as userland/libc/Makefile
# does, then symbol-prefixed with objcopy so they link next to glibc. The harness
# (aieject_test.c) backs file+clock syscalls with the host, the THREE volume
# syscalls with an in-memory mock removable-device table, and libc primitives
# with host libc. Nothing reimplements an executor or a gate; the logic exercised
# is shipped code.
#
# TWO ARMS. Both must behave as stated or this script exits non-zero:
#   POSITIVE      every scenario must self-assert (exit 0): a manifest-ALLOW eject
#                 flushes+unmounts+stops the stick; a consent grant flows through
#                 to the executor; no consent => refused; a FORBIDden verb (format)
#                 is refused by the manifest; a BUSY device is refused (not forced);
#                 an unknown device is a clean no-op; a NON-removable/system disk is
#                 never a target; device.list shows only removable volumes.
#   NEGATIVE CTRL recompiles ../aiclient.c with (a) the busy guard neutered and
#                 (b) the removable guard neutered. The busy and system-disk
#                 scenarios must then FAIL (they would leak: eject a busy or a
#                 non-removable device), proving those guards are load-bearing.
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

# $1 = aiclient.c source, $2 = output binary
build() {
    gcc $UUT_FLAGS -x c -c "$1" -o "$WORK/aiclient_raw.o" 2>"$WORK/cc1.log" || {
        echo "  compile of aiclient.c FAILED:"; sed -n 1,25p "$WORK/cc1.log"; return 2; }
    gcc $UUT_FLAGS -x c -c "$LIBC/aidev.c" -o "$WORK/aidev_raw.o" 2>"$WORK/cc2.log" || {
        echo "  compile of aidev.c FAILED:"; sed -n 1,25p "$WORK/cc2.log"; return 2; }
    gcc $UUT_FLAGS -x c -c "$LIBC/aicap.c" -o "$WORK/aicap_raw.o" 2>"$WORK/cc3.log" || {
        echo "  compile of aicap.c FAILED:"; sed -n 1,25p "$WORK/cc3.log"; return 2; }
    objcopy --prefix-symbols=ad_ "$WORK/aiclient_raw.o" "$WORK/aiclient.o" || return 2
    objcopy --prefix-symbols=ad_ "$WORK/aidev_raw.o"   "$WORK/aidev.o"   || return 2
    objcopy --prefix-symbols=ad_ "$WORK/aicap_raw.o"   "$WORK/aicap.o"   || return 2
    gcc -m64 -O1 -g -c aieject_test.c -o "$WORK/t.o" 2>"$WORK/cc4.log" || {
        echo "  compile of harness FAILED:"; sed -n 1,25p "$WORK/cc4.log"; return 2; }
    gcc -m64 "$WORK/t.o" "$WORK/aiclient.o" "$WORK/aidev.o" "$WORK/aicap.o" -o "$2" 2>"$WORK/ld.log" || {
        echo "  link FAILED:"; sed -n 1,25p "$WORK/ld.log"; return 2; }
    return 0
}

run_one() {  # $1 = binary, $2 = scenario ; returns the scenario exit code
    local root; root=$(mktemp -d)
    AIEJ_ROOT="$root" "$1" "$2" >"$WORK/out.$2" 2>&1
    local rc=$?
    sed 's/^/      /' "$WORK/out.$2"
    rm -rf "$root"
    return $rc
}

rc_all=0
ALL="eject_allow eject_consent eject_consent_deny format_forbid eject_busy eject_notfound eject_sysdisk list"

echo "=== POSITIVE: live ../aiclient.c, every scenario must self-assert (exit 0) ==="
if ! build "$LIBC/aiclient.c" "$WORK/t_pos"; then
    echo "RESULT: BROKEN - positive arm did not build"; exit 1
fi
pos_bad=0
for n in $ALL; do
    echo "  scenario $n:"; run_one "$WORK/t_pos" $n
    rc=$?
    if [ $rc -ne 0 ]; then echo "  -> $n FAIL (exit $rc)"; pos_bad=1
    else echo "  -> $n ok"; fi
done
if [ $pos_bad -eq 0 ]; then echo "RESULT: PASS - every scenario behaved as asserted."
else echo "RESULT: FAIL"; rc_all=1; fi

echo
echo "=== NEGATIVE CONTROL A: BUSY guard neutered, eject_busy must FAIL (leak) ==="
sed 's/if (busy > 0) {/if (busy > 999999) { \/* NEG CTRL busy-guard off *\//' \
    "$LIBC/aiclient.c" > "$WORK/aiclient_nobusy.c"
NSUB=$(grep -c 'NEG CTRL busy-guard off' "$WORK/aiclient_nobusy.c")
if [ "$NSUB" -ne 1 ]; then
    echo "RESULT: BROKEN - could not neuter the busy guard ($NSUB matches); it was restructured."; exit 1
fi
if ! build "$WORK/aiclient_nobusy.c" "$WORK/t_nobusy"; then
    echo "RESULT: BROKEN - busy-neg arm did not build"; exit 1
fi
echo "  scenario eject_busy (busy guard off):"; run_one "$WORK/t_nobusy" eject_busy
if [ $? -eq 0 ]; then
    echo "  -> LEAK: a busy device was ejected with the guard off, proves nothing"; rc_all=1
else
    echo "  -> correctly FAILED with the busy guard off (the guard is load-bearing)"
fi

echo
echo "=== NEGATIVE CONTROL B: REMOVABLE guard neutered, eject_sysdisk must FAIL ==="
sed 's@if (!(vols\[i\].flags & MOSVOL_REMOVABLE)) continue;   // never a system/boot disk@if (0) continue; /* NEG CTRL removable-guard off */@' \
    "$LIBC/aiclient.c" > "$WORK/aiclient_norem.c"
NSUB=$(grep -c 'NEG CTRL removable-guard off' "$WORK/aiclient_norem.c")
if [ "$NSUB" -ne 1 ]; then
    echo "RESULT: BROKEN - could not neuter the removable guard ($NSUB matches); it was restructured."; exit 1
fi
if ! build "$WORK/aiclient_norem.c" "$WORK/t_norem"; then
    echo "RESULT: BROKEN - removable-neg arm did not build"; exit 1
fi
echo "  scenario eject_sysdisk (removable guard off):"; run_one "$WORK/t_norem" eject_sysdisk
if [ $? -eq 0 ]; then
    echo "  -> LEAK: a non-removable disk was ejected with the guard off, proves nothing"; rc_all=1
else
    echo "  -> correctly FAILED with the removable guard off (the guard is load-bearing)"
fi

echo
[ $rc_all -eq 0 ] && echo "#708 AI safe-eject device-executor unit test: PASS" || echo "#708 AI safe-eject device-executor unit test: FAIL"
exit $rc_all
