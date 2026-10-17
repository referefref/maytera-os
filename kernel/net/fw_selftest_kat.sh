#!/usr/bin/env bash
# kernel/net/fw_selftest_kat.sh - #kfirewall: host oracle for the packet filter's
# boot self-test.
#
# WHAT IT PROVES, and why it is worth having beside the in-kernel self-test it
# runs. `fw_selftest_rs()` already drives the real decision function over 20+
# vectors on every boot and prints `[FW] selftest: PASS (0 failures)`. The
# question this answers is the one that line cannot: CAN IT SAY ANYTHING ELSE?
#
# Until 2026-10-01 the answer was no, and not for a subtle reason. The documented
# way to fault it, `make FWTESTFAIL=1`, was accepted by make and did nothing,
# because no line in kernel/Makefile ever passed `--cfg fw_test_fail`. The
# inverted `want()` arm was never compiled on any build, ever. So the boot line
# had reported PASS for the only reason it could, for six weeks, while the
# CHANGELOG recorded having watched it go red.
#
# WHY A HOST ORACLE AND NOT ONLY A BOOT LINE. Checking the boot line costs an
# ~18-minute golden build plus a VM boot. That cost is WHY nobody checked. This
# runs in about two seconds, so editing the filter and knowing whether you broke
# it are the same action. Same argument, and same GREEN/RED shape, as the nine
# oracles in kernel/crypto and as net/tls/ecdhkat_x25519_kat.sh.
#
# HOW IT CAN DO THIS AT ALL: rustkern/fwfilter.rs depends on NOTHING but
# `core::sync::atomic` (measured: that is its only `use` line, and it has no
# extern block). So the kernel's own self-test function compiles for the host
# unmodified and is called directly. There is no second copy of anything here;
# the file under test is the file that ships.
#
# THE KERNEL BUILD REMAINS THE AUTHORITY. This cannot see the C glue, the ip.c
# hooks, or the real packet path. It tests the decision engine and its self-test.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../rustkern/fwfilter.rs"

command -v rustc >/dev/null 2>&1 || {
    echo "fw_selftest_kat: rustc not found - this oracle needs the host Rust toolchain"
    echo "(the same requirement the crypto oracles have). Run it where rustc lives."
    exit 2
}
[ -f "$SRC" ] || { echo "fw_selftest_kat: FATAL: $SRC is missing"; exit 2; }

# The red-team hook must still be IN the source. If someone deletes the inverted
# arm, this oracle would otherwise pass by having nothing to invert, which is the
# failure mode it exists to detect one level up.
grep -q 'fw_test_fail' "$SRC" || {
    echo "fw_selftest_kat: FATAL: the red-team cfg hook fw_test_fail is gone from $SRC."
    echo "Without it the self-test cannot be faulted and cannot be shown to fail."
    exit 2
}
grep -q 'maytera_sshd' "$SRC" || {
    echo "fw_selftest_kat: FATAL: the maytera_sshd derivation is gone from $SRC."
    exit 2
}

WORK=$(mktemp -d) || exit 2
trap 'rm -rf "$WORK"' EXIT
cp "$SRC" "$WORK/fwfilter.rs"
cat > "$WORK/main.rs" <<'RSEOF'
#[path = "fwfilter.rs"]
mod fwfilter;
fn main() {
    let f = unsafe { fwfilter::fw_selftest_rs() };
    println!("[FW] selftest: {} ({} failures)",
             if f == 0 { "PASS" } else { "FAIL" }, f);
    std::process::exit(if f == 0 { 0 } else { 1 });
}
RSEOF

rc=0
# $1 label, $2 want ("pass"/"fail"), $3.. cfg flags
run_case() {
    local label="$1" want="$2"; shift 2
    local out ec
    if ! out=$(cd "$WORK" && rustc --edition 2021 -A warnings "$@" -o t main.rs 2>&1); then
        echo "  $label: COMPILE FAILED"
        echo "$out" | head -5 | sed 's/^/      /'
        rc=1; return
    fi
    out=$("$WORK/t" 2>&1); ec=$?
    local got="pass"; [ "$ec" = "0" ] || got="fail"
    if [ "$got" = "$want" ]; then
        echo "  $label: $out  [wanted $want]  ok"
    else
        echo "  $label: $out  [wanted $want, got $got]  WRONG"
        rc=1
    fi
}

echo "== GREEN: the self-test as it ships =="
run_case "default kernel (no sshd)      " pass
run_case "MAYTERA_SSHD=1 kernel         " pass --cfg maytera_sshd

echo "== RED: every expectation inverted (--cfg fw_test_fail) =="
# This is the whole point. If either of these reports PASS, the knob does not
# reach the compiler and the boot line's PASS means nothing.
run_case "default kernel, faulted       " fail --cfg fw_test_fail
run_case "MAYTERA_SSHD=1 kernel, faulted" fail --cfg maytera_sshd --cfg fw_test_fail

echo "------------------------------------------------------------"
if [ "$rc" = "0" ]; then
    echo "GREEN ok  : fw_selftest_rs passes in both sshd configurations"
    echo "RED   ok  : the FWTESTFAIL knob genuinely inverts it, so the boot line CAN say FAIL"
    echo "------------------------------------------------------------"
    echo "fw_selftest_kat.sh: PASS"
else
    echo "fw_selftest_kat.sh: FAIL"
fi
exit $rc
