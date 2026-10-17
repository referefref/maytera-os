#!/usr/bin/env bash
#
# ntlm_nthash_kat.sh - known-answer oracle for the SMB NTLM key derivation.
#
# WHY THIS FILE EXISTS
# kernel/net/smb.c authenticates every SMB mount with NTLMv2. A passing kernel
# build proves NOTHING about whether the derivation is CORRECT: a wrong NT hash
# or a wrong NTOWFv2 compiles perfectly and simply fails every login, and a
# refactor of this path (#smbnt moved it from holding a cleartext password to
# holding the 16-byte NT hash) can be behaviour-changing without a single
# warning. So this oracle extracts the REAL derivation VERBATIM from smb.c
# between the NTLM_KAT_REGION/NTLM_KAT_HELPER markers and compiles it against
# the REAL crypto/md4.c and crypto/hmac.c, without a VM.
#
# THE VECTORS ARE PUBLISHED, NOT SELF-GENERATED. MS-NLMP section 4.2.4 gives
# User="User", Domain="Domain", Password="Password":
#     NTOWFv1 (the NT hash) = a4f49c406510bdcab6824ee7c30fd852
#     NTOWFv2               = 0c868a403bfd7a93a3001ef22ef02e3f
# A self-generated vector proves only that the code agrees with itself, which is
# exactly the failure this repo keeps re-learning. These come from Microsoft's
# own specification, so GREEN means our bytes match a third party's.
#
#   GREEN : unmodified code reproduces BOTH published vectors.
#   RED   : compiled with -DNTLM_KAT_FAULT (the uppercase step skipped, a
#           plausible real bug that leaves the NT hash correct and breaks only
#           NTOWFv2) the SAME test must FAIL, proving the comparison fires
#           rather than being a no-op.
#
# Exit 0 iff GREEN is green AND RED is red.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$HERE/.." && pwd)
SMB="$K/net/smb.c"
WORK=$(mktemp -d) || exit 1
trap 'rm -rf "$WORK"' EXIT

[ -f "$SMB" ] || { echo "FAIL: $SMB not found"; exit 1; }

extract() { # $1=begin marker  $2=end marker
    awk -v b="$1" -v e="$2" '$0 ~ b {f=1; next} $0 ~ e {f=0} f' "$SMB"
}

HELPER=$(extract 'NTLM_KAT_HELPER_BEGIN' 'NTLM_KAT_HELPER_END')
REGION=$(extract 'NTLM_KAT_REGION_BEGIN' 'NTLM_KAT_REGION_END')
[ -n "$HELPER" ] || { echo "FAIL: could not extract the ascii_to_unicode helper"; exit 1; }
[ -n "$REGION" ] || { echo "FAIL: could not extract the NTLM region"; exit 1; }
echo "$REGION" | grep -q 'ntlm_v2_hash' || { echo "FAIL: extracted region has no ntlm_v2_hash"; exit 1; }

cat > "$WORK/kat.c" <<CEOF
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void md4(const uint8_t *data, uint32_t len, uint8_t *digest);
void hmac_md5(const uint8_t *key, uint32_t key_len,
              const uint8_t *data, uint32_t data_len, uint8_t *out);

$HELPER

$REGION

static int cmp(const char *what, const uint8_t *got, const char *want_hex) {
    char hex[33];
    for (int i = 0; i < 16; i++) sprintf(hex + i * 2, "%02x", got[i]);
    hex[32] = 0;
    int ok = (strcmp(hex, want_hex) == 0);
    printf("  %-10s %s  got=%s want=%s\n", what, ok ? "PASS" : "FAIL", hex, want_hex);
    return ok;
}

int main(void) {
    uint8_t nt[16], v2[16];
    int ok = 1;
    ntlm_hash("Password", nt);
    ok &= cmp("NTOWFv1", nt, "a4f49c406510bdcab6824ee7c30fd852");
    ntlm_v2_hash("Domain", "User", nt, v2);
    ok &= cmp("NTOWFv2", v2, "0c868a403bfd7a93a3001ef22ef02e3f");
    return ok ? 0 : 1;
}
CEOF

# A fault build must break NTOWFv2 only. Injected by rewriting the uppercase
# step in the EXTRACTED copy, never in smb.c itself.
sed 's/? username\[i\] - 32 : username\[i\];/? username[i] : username[i];/' \
    "$WORK/kat.c" > "$WORK/kat_fault.c"
if cmp -s "$WORK/kat.c" "$WORK/kat_fault.c"; then
    echo "FAIL: fault injection changed nothing, so RED could never be red"
    exit 1
fi

CC=${CC:-gcc}
# The crypto sources need the KERNEL headers; the test file must NOT have them
# on its include path, or kernel/string.h shadows the host <string.h> and
# kernel/types.h collides with the host stdint.h. Separate translation units,
# separate include paths, then link.
$CC -O1 -w -ffunction-sections -fdata-sections -c "$K/crypto/md4.c"  -I"$K" -I"$K/crypto" -o "$WORK/md4.o"  2>"$WORK/crypto.log" || {
    echo "FAIL: could not build md4.c"; head -10 "$WORK/crypto.log"; exit 1; }
$CC -O1 -w -ffunction-sections -fdata-sections -c "$K/crypto/hmac.c" -I"$K" -I"$K/crypto" -o "$WORK/hmac.o" 2>>"$WORK/crypto.log" || {
    echo "FAIL: could not build hmac.c"; head -10 "$WORK/crypto.log"; exit 1; }
$CC -O1 -w -ffunction-sections -fdata-sections -c "$K/crypto/md5.c"  -I"$K" -I"$K/crypto" -o "$WORK/md5.o"  2>>"$WORK/crypto.log" || {
    echo "FAIL: could not build md5.c"; head -10 "$WORK/crypto.log"; exit 1; }

# --gc-sections drops the crypto files' own self-test functions, which drag in a
# long tail of kernel-only symbols this oracle neither has nor needs. Discarding
# them is safer than stubbing them: a stub could silently stand in for something
# the REAL derivation calls, whereas an unreferenced section simply disappears.
build() { $CC -O1 -w -ffunction-sections -fdata-sections -Wl,--gc-sections \
            -o "$WORK/$1" "$WORK/$2" "$WORK/md4.o" "$WORK/hmac.o" "$WORK/md5.o" 2>"$WORK/$1.log"; }

rc=0
echo "GREEN (unmodified smb.c):"
if build kat kat.c; then
    "$WORK/kat"; g=$?
    [ $g -eq 0 ] || { echo "  -> GREEN FAILED"; rc=1; }
else
    echo "  build failed:"; head -15 "$WORK/kat.log"; rc=1
fi

echo "RED (-DNTLM_KAT_FAULT equivalent: uppercase step removed):"
if build katf kat_fault.c; then
    "$WORK/katf" >/dev/null 2>&1; r=$?
    if [ $r -eq 0 ]; then echo "  -> RED PASSED, so the comparison is a NO-OP"; rc=1
    else echo "  correctly FAILED (exit $r), the comparison fires"; fi
else
    echo "  fault build failed:"; head -15 "$WORK/katf.log"; rc=1
fi

echo
[ $rc -eq 0 ] && echo "ntlm_nthash_kat: PASS" || echo "ntlm_nthash_kat: FAIL"
exit $rc
