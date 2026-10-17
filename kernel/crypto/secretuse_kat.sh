#!/bin/bash
# secretuse_kat.sh - #secretuse: known-answer + refusal oracle for the TOTP
# proxy's arithmetic and for the SECRET TIER TABLE (rustkern/totp.rs).
#
# WHAT IS UNDER TEST, and why each RED exists rather than just one.
#
# rustkern/totp.rs is the whole algorithmic surface of SYS_SECRET_USE: RFC 4648
# base32 decode over text a user pasted, RFC 4226 dynamic truncation, RFC 6238
# time stepping, the parser that turns decrypted store bytes into fixed-size
# records, and the table that says which KEY SOURCE opens which class of secret.
# proc/secretuse.c around it holds no algorithm at all.
#
# A round-trip test proves almost nothing here, which is the reason for four REDs:
#
#   GREEN : the shipping rustkern/totp.rs + rustkern/sha1.rs + rustkern/hmac.rs,
#           linked against the REAL crypto/sha256.c, reproduce RFC 6238
#           Appendix B (SHA1 and SHA256 arms, all six published times each) and
#           RFC 4648 section 10, and every refusal fires.
#           totp_selftest_rs() returns 0 and reports a NON-ZERO check count.
#   RED 1 : --cfg totp_kat_fault pins the dynamic-truncation offset to 0. The
#           codes are still 8 digits, still change every 30 seconds, and still
#           round-trip against themselves. They are wrong against every real
#           issuer, and ONLY a published vector notices. This is the concrete
#           reason "it produced six digits" is not evidence of a TOTP.
#   RED 2 : the base32 digit range is shifted by one ('2'..'7' -> 25 not 26).
#           Every secret a user pastes decodes to different bytes, silently.
#   RED 3 : the digits-in-{6,8} policy check in the store parser is disabled, to
#           prove the REFUSAL cases are not no-ops.
#   RED 4 : the tier table's fall-through is changed from SECRET_KEYSRC_NONE to
#           SECRET_KEYSRC_LOGIN, so an UNDECLARED secret class would inherit the
#           login key instead of being refused. This is the one that matters most
#           for the design: the whole fail-closed claim in docs/SECRET_STORE.md
#           rests on that single `_ =>` arm, and this RED is the evidence that
#           the claim is enforced rather than merely written down.
#
# A note on why the check COUNT is asserted and not only the return code. The
# contract work this week found a verifier reporting promise=fulfilled with
# paths=0/0: a verifier that asks nothing answers yes. totp_selftest_rs() reports
# how many checks it ran, and this oracle fails if that number is zero even when
# the return code is 0.
#
# Exit 0 iff GREEN is green AND all four REDs are red. Exit 2 means the ORACLE is
# broken (unreadable source, compile failure, a mutation that matched nothing),
# which is deliberately distinct from a test failure. Run it in the kernel
# (the build container) or userland (the build container) build container; run-all-kats.sh picks it up from
# the *_kat.sh glob with no wiring.
set -u

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL="$(cd "$SRCDIR/.." && pwd)"
TOTP_RS="$KERNEL/rustkern/totp.rs"
SHA1_RS="$KERNEL/rustkern/sha1.rs"
HMAC_RS="$KERNEL/rustkern/hmac.rs"
SHA256_C="$SRCDIR/sha256.c"
CC="${CC:-gcc}"
RUSTC="${RUSTC:-rustc}"

for f in "$TOTP_RS" "$SHA1_RS" "$HMAC_RS" "$SHA256_C"; do
    [ -r "$f" ] || { echo "FATAL: cannot read $f"; exit 2; }
done
command -v "$RUSTC" >/dev/null 2>&1 || { echo "FATAL: no rustc on PATH (set RUSTC=)"; exit 2; }

# The oracle must be testing the real thing. If a symbol or the RED hook has been
# renamed, fail LOUDLY rather than silently testing nothing: that is the
# ecdsa_verify_kat.sh lesson recorded in run-all-kats.sh's header.
grep -q 'totp_selftest_rs'            "$TOTP_RS" || { echo "FATAL: totp_selftest_rs gone from $TOTP_RS"; exit 2; }
grep -q 'totp_kat_fault'              "$TOTP_RS" || { echo "FATAL: RED hook totp_kat_fault gone from $TOTP_RS"; exit 2; }
grep -q 'secret_keysrc_for_class_rs'  "$TOTP_RS" || { echo "FATAL: secret_keysrc_for_class_rs gone from $TOTP_RS"; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------------------
# 1) Shim headers so the REAL crypto/sha256.c compiles on the host. Only
#    #include lines change; every hashing function is byte-identical to what
#    ships. SHA-256 is real here and not stubbed, because the RFC 6238 SHA256 arm
#    is half the known answers.
# ---------------------------------------------------------------------------
cat > "$WORK/crypto.h" <<'EOF'
#ifndef SECRETUSE_KAT_CRYPTO_H
#define SECRETUSE_KAT_CRYPTO_H
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned long      size_t;
#define SHA256_BLOCK_SIZE   64
#define SHA256_DIGEST_SIZE  32
typedef struct { uint32_t state[8]; uint64_t count; uint8_t buffer[64]; } sha256_ctx_t;
typedef struct { sha256_ctx_t inner; sha256_ctx_t outer; uint8_t key_block[SHA256_BLOCK_SIZE]; } hmac_sha256_ctx_t;
void sha256_init(sha256_ctx_t *);
void sha256_update(sha256_ctx_t *, const void *, size_t);
void sha256_final(sha256_ctx_t *, uint8_t *);
void sha256(const void *, size_t, uint8_t *);
void hmac_sha256_init(hmac_sha256_ctx_t *, const void *, size_t);
void hmac_sha256_update(hmac_sha256_ctx_t *, const void *, size_t);
void hmac_sha256_final(hmac_sha256_ctx_t *, uint8_t *);
void hmac_sha256(const void *, size_t, const void *, size_t, uint8_t *);
void crypto_zero(void *, size_t);
#endif
EOF

cat > "$WORK/string.h" <<'EOF'
#ifndef SECRETUSE_KAT_STRING_H
#define SECRETUSE_KAT_STRING_H
#include "crypto.h"
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int   memcmp(const void *, const void *, size_t);
#endif
EOF

cat > "$WORK/kshim.h" <<'EOF'
#ifndef SECRETUSE_KAT_KSHIM_H
#define SECRETUSE_KAT_KSHIM_H
#include "crypto.h"
static inline uint64_t dp_tsc(void) { return 0; }
static volatile uint64_t g_dp_sha_cyc, g_dp_sha_bytes;
int  kprintf(const char *, ...);
void bootlog_write(const char *, ...);
#endif
EOF

sed -E \
  -e 's|^#include "crypto\.h"|#include "crypto.h"|' \
  -e 's|^#include "\.\./string\.h".*|#include "string.h"|' \
  -e 's|^#include "\.\./cpu/dlprof\.h".*|#include "kshim.h"|' \
  -e 's|^#include "fs/bootlog\.h".*|#include "kshim.h"|' \
  "$SHA256_C" > "$WORK/sha256_host.c"

# Host-side C stubs for the handful of kernel symbols the hashing TU references.
# A stub cannot hide a failure here: if the SHA-256 path ever reached one, every
# RFC 6238 SHA256 vector below would be wrong.
cat > "$WORK/stubs.c" <<'EOF'
#include <stdio.h>
#include <stdarg.h>
void crypto_zero(void *p, unsigned long n) { volatile unsigned char *q = p; while (n--) *q++ = 0; }
int  kprintf(const char *f, ...) { va_list a; va_start(a,f); int r = vprintf(f,a); va_end(a); return r; }
void bootlog_write(const char *f, ...) { (void)f; }
/* referenced by sha256.c's differential self-test, which this build never runs */
void sha256_transform_rs(unsigned int *s, const unsigned char *b) { (void)s; (void)b; }
EOF

# ---------------------------------------------------------------------------
# 2) Host driver. `#[path]` pulls in the three real modules VERBATIM; only the
#    hash triples hmac.rs also declares but this oracle does not exercise are
#    stubbed. If HMAC-SHA1 or HMAC-SHA256 ever reached a stub, every vector below
#    would be wrong, so the stubs cannot mask anything.
# ---------------------------------------------------------------------------
cat > "$WORK/driver.rs" <<'EOF'
#![allow(dead_code)]

#[path = "sha1_host.rs"]
mod sha1m;
#[path = "hmac_host.rs"]
mod hmacm;
#[path = "totp_host.rs"]
mod totpm;

mod stubs {
    #[no_mangle] pub extern "C" fn sha384_init(_c: *mut u8) {}
    #[no_mangle] pub extern "C" fn sha512_update(_c: *mut u8, _d: *const u8, _l: usize) {}
    #[no_mangle] pub extern "C" fn sha384_final(_c: *mut u8, _o: *mut u8) {}
    #[no_mangle] pub extern "C" fn md5_init(_c: *mut u8) {}
    #[no_mangle] pub extern "C" fn md5_update(_c: *mut u8, _d: *const u8, _l: usize) {}
    #[no_mangle] pub extern "C" fn md5_final(_c: *mut u8, _o: *mut u8) {}
}

fn main() {
    let mut checks: u32 = 0;
    let rc = totpm::totp_selftest_rs(&mut checks as *mut u32);
    println!("HOST-KAT: totp_selftest_rs() = {} after {} checks", rc, checks);
    if rc != 0 {
        println!("HOST-KAT: FAILED at case {}", rc);
        std::process::exit(1);
    }
    // A self-test that ran ZERO checks and returned 0 is VACUOUS. The contract
    // work found exactly that shape this week (promise=fulfilled, paths=0/0), so
    // the count is asserted rather than trusted.
    if checks == 0 {
        println!("HOST-KAT: VACUOUS - returned 0 having run no checks at all");
        std::process::exit(1);
    }
    println!("HOST-KAT: PASS");
    std::process::exit(0);
}
EOF

cp "$SHA1_RS" "$WORK/sha1_host.rs"
cp "$HMAC_RS" "$WORK/hmac_host.rs"
cp "$TOTP_RS" "$WORK/totp_shipped.rs"

# Compile the C half once; it is identical for every variant.
if ! "$CC" -O2 -Wall -Wno-unused-variable -Wno-unused-function -I "$WORK" \
        -c "$WORK/sha256_host.c" -o "$WORK/sha256.o" 2> "$WORK/cc_sha256.log"; then
    echo "FATAL: compile of crypto/sha256.c failed"; cat "$WORK/cc_sha256.log"; exit 2
fi
if ! "$CC" -O2 -Wall -c "$WORK/stubs.c" -o "$WORK/stubs.o" 2> "$WORK/cc_stubs.log"; then
    echo "FATAL: compile of the host stubs failed"; cat "$WORK/cc_stubs.log"; exit 2
fi

build_and_run() {   # $1 = totp source to compile, $2 = label, $3 = extra rustc flags
    local src="$1" label="$2" extra="${3:-}"
    cp "$src" "$WORK/totp_host.rs"
    if ! "$RUSTC" --edition 2021 -O -A warnings $extra \
            -C link-arg="$WORK/sha256.o" -C link-arg="$WORK/stubs.o" \
            -o "$WORK/kat_$label" "$WORK/driver.rs" 2> "$WORK/rs_$label.log"; then
        echo "FATAL: compile failed for $label"; cat "$WORK/rs_$label.log"; exit 2
    fi
    "$WORK/kat_$label"
    return $?
}

mutate() {   # $1 = sed expression, $2 = output file, $3 = description
    sed "$1" "$WORK/totp_shipped.rs" > "$2"
    if cmp -s "$WORK/totp_shipped.rs" "$2"; then
        echo "FATAL: mutation '$3' changed NOTHING (the expression matched no line)."
        echo "       The RED run would then be a second GREEN run and would prove nothing."
        exit 2
    fi
}

echo "== GREEN: rustkern/totp.rs + sha1.rs + hmac.rs + real crypto/sha256.c =="
build_and_run "$WORK/totp_shipped.rs" green ""
GREEN=$?

echo
echo "== RED 1: --cfg totp_kat_fault (dynamic-truncation offset pinned to 0) =="
build_and_run "$WORK/totp_shipped.rs" red1 "--cfg totp_kat_fault" > "$WORK/red1.out" 2>&1
RED1=$?
sed 's/^/    /' "$WORK/red1.out"

echo
echo "== RED 2: base32 digit range shifted by one ('2'..'7' -> 25, not 26) =="
mutate 's/ as i32 + 26/ as i32 + 25/' "$WORK/totp_red2.rs" "base32 digit range"
build_and_run "$WORK/totp_red2.rs" red2 "" > "$WORK/red2.out" 2>&1
RED2=$?
sed 's/^/    /' "$WORK/red2.out"

echo
echo "== RED 3: the digits-in-{6,8} store-parser policy check disabled =="
mutate 's/if dig != 6 \&\& dig != 8 {/if false {/' "$WORK/totp_red3.rs" "digits policy check"
build_and_run "$WORK/totp_red3.rs" red3 "" > "$WORK/red3.out" 2>&1
RED3=$?
sed 's/^/    /' "$WORK/red3.out"

echo
echo "== RED 4: the tier table falls through to LOGIN instead of NONE =="
mutate 's/_ => SECRET_KEYSRC_NONE,/_ => SECRET_KEYSRC_LOGIN,/' "$WORK/totp_red4.rs" \
       "tier-table fall-through"
build_and_run "$WORK/totp_red4.rs" red4 "" > "$WORK/red4.out" 2>&1
RED4=$?
sed 's/^/    /' "$WORK/red4.out"

echo
echo "------------------------------------------------------------"
rc=0
if [ "$GREEN" -eq 0 ]; then
    echo "GREEN ok  : RFC 6238 (SHA1+SHA256, 12 vectors) + RFC 4648 + every refusal (exit 0)"
else
    echo "GREEN FAIL: correct code did not pass the KAT (exit $GREEN)"; rc=1
fi
red_check() {  # $1 = code, $2 = label, $3 = what it proves
    if [ "$1" -ne 0 ]; then
        echo "RED $2 ok  : $3 is CAUGHT (exit $1)"
    else
        echo "RED $2 FAIL: the KAT did not catch $3 -> that case is a no-op"; rc=1
    fi
}
red_check "$RED1" 1 "a truncation offset pinned to zero"
red_check "$RED2" 2 "a base32 alphabet off by one"
red_check "$RED3" 3 "a disabled digits policy check"
red_check "$RED4" 4 "an undeclared secret class inheriting the login key"
echo "------------------------------------------------------------"
[ "$rc" -eq 0 ] && echo "secretuse_kat.sh: PASS" || echo "secretuse_kat.sh: FAIL"
exit $rc
