#!/bin/bash
# secstore_kat.sh - #secretuse: known-answer + tamper-reject oracle for the
# encrypted secret-store container (rustkern/secstore.rs).
#
# WHAT IS UNDER TEST, and why it needs its own oracle.
#
# secstore.rs frames a secret as header || ciphertext || Poly1305 tag, feeding
# the WHOLE 64-byte header to ChaCha20-Poly1305 as additional authenticated
# data. That AAD binding is the entire tamper-evidence property: without it the
# container still encrypts, still decrypts, and still round-trips perfectly,
# while an attacker can rewrite the KDF iteration count to 1, swap the salt, or
# restate the plaintext length, and the file still opens. A round-trip test
# passes a container with that bug. Only the tamper cases catch it.
#
# So this oracle is deliberately asymmetric: ONE case proves the round trip and
# FOURTEEN prove refusals. secstore_selftest_rs() returns 0 or the CASE NUMBER
# that failed, so a failure names itself.
#
#   GREEN : the unmodified secstore.rs, linked against the real
#           crypto/chacha20.c AEAD, passes all 15 cases -> selftest returns 0.
#   RED   : compiled with --cfg secstore_kat_fault, which drops the header from
#           the AAD and is the exact bug described above, the SAME self-test
#           returns non-zero, proving the tamper cases are not a no-op.
#
# This is the first Rust-hosted oracle in this directory. It works because
# secstore.rs needs no kernel services: it is pure framing plus two extern "C"
# calls into the AEAD, so it compiles for the host as an ordinary staticlib and
# links against a host build of the real chacha20.c. The AEAD staging below is
# lifted from chacha20_poly1305_kat.sh, which already solved it.
#
# Exit 0 iff GREEN is green AND RED is red. Exit 2 means the ORACLE is broken
# (unreadable source, compile failure, markers moved), which is deliberately
# distinct from a test failure. Run it in the kernel (the build container) or userland
# (the build container) build container.
set -u

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
CCFILE="$SRCDIR/chacha20.c"
RSFILE="$SRCDIR/../rustkern/secstore.rs"
CC="${CC:-gcc}"
RUSTC="${RUSTC:-rustc}"

[ -r "$CCFILE" ] || { echo "FATAL: cannot read $CCFILE"; exit 2; }
[ -r "$RSFILE" ] || { echo "FATAL: cannot read $RSFILE"; exit 2; }
command -v "$RUSTC" >/dev/null 2>&1 || { echo "FATAL: no rustc on PATH"; exit 2; }

# The oracle must be testing the real thing. If the self-test or the AAD hook
# has been renamed or removed, fail LOUDLY rather than silently testing nothing:
# that is the ecdsa_verify_kat.sh lesson (it was broken for an unknown period
# because the source moved out from under it).
grep -q 'secstore_selftest_rs' "$RSFILE" || {
    echo "FATAL: secstore_selftest_rs not found in $RSFILE (renamed?)"; exit 2; }
grep -q 'secstore_kat_fault' "$RSFILE" || {
    echo "FATAL: the RED-team cfg hook secstore_kat_fault is gone from $RSFILE"; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------------------
# 1) Shim headers so the real chacha20.c compiles on the host. Only #include
#    lines change; every function under test is byte-identical to what ships.
# ---------------------------------------------------------------------------
cat > "$WORK/types.h" <<'EOF'
#ifndef SECSTOREKAT_TYPES_H
#define SECSTOREKAT_TYPES_H
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned long      size_t;
#endif
EOF

cat > "$WORK/crypto.h" <<'EOF'
#ifndef SECSTOREKAT_CRYPTO_H
#define SECSTOREKAT_CRYPTO_H
#include "types.h"
int  crypto_memcmp(const void *a, const void *b, size_t length);
void crypto_zero(void *ptr, size_t length);
#endif
EOF

cat > "$WORK/string.h" <<'EOF'
#ifndef SECSTOREKAT_STRING_H
#define SECSTOREKAT_STRING_H
#include "types.h"
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int   memcmp(const void *, const void *, size_t);
#endif
EOF

cat > "$WORK/kshim.h" <<'EOF'
#ifndef SECSTOREKAT_KSHIM_H
#define SECSTOREKAT_KSHIM_H
int  kprintf(const char *, ...);
void bootlog_write(const char *, ...);
#endif
EOF

sed -E 's|^#include "\.\./types\.h"|#include "types.h"|' \
    "$SRCDIR/chacha20.h" > "$WORK/chacha20.h"

sed -E \
  -e 's|^#include "\.\./string\.h"|#include "string.h"|' \
  -e 's|^#include "fs/bootlog\.h".*|#include "kshim.h"|' \
  "$CCFILE" > "$WORK/chacha20_host.c"

# ---------------------------------------------------------------------------
# 2) The module under test, VERBATIM. secstore.rs is a rustkern module, so it
#    carries no crate-level attributes of its own; compiled directly as a
#    staticlib crate root it needs none. No sed, no edits: if this file needs
#    changing to be testable, the test is no longer testing what ships.
# ---------------------------------------------------------------------------
cp "$RSFILE" "$WORK/secstore.rs"

# ---------------------------------------------------------------------------
# 3) Host driver. System headers only, and it must NOT see the shim (the shim's
#    int64_t = long long clashes with <stdlib.h>'s int64_t = long int).
# ---------------------------------------------------------------------------
cat > "$WORK/driver.c" <<'EOF'
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

/* crypto.c helpers, reimplemented faithfully for the host. Constant-timeness is
   irrelevant to correctness here; 0 iff equal is the whole contract. */
int crypto_memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b; unsigned char d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned char)(x[i] ^ y[i]);
    return d ? 1 : 0;
}
void crypto_zero(void *p, size_t n) {
    volatile unsigned char *q = p; while (n--) *q++ = 0;
}
int kprintf(const char *f, ...) {
    va_list a; va_start(a, f); int r = vprintf(f, a); va_end(a); return r;
}
void bootlog_write(const char *f, ...) { (void)f; }

/* Rust FFI symbol referenced by chacha20.c, only used under -DRUST_CHACHA20,
   which this host build does NOT define. The stub keeps the link resolved; it is
   never called on the AEAD path this oracle exercises. Same stub, same reason,
   as chacha20_poly1305_kat.sh. */
void chacha20_block_rs(const unsigned int input[16], unsigned char out[64]) {
    (void)input; (void)out;
}

extern int secstore_selftest_rs(void);

int main(void) {
    int c = secstore_selftest_rs();
    if (c == 0) printf("HOST-KAT: secstore_selftest_rs() = 0 (all 15 cases pass)\n");
    else        printf("HOST-KAT: secstore_selftest_rs() FAILED at case %d\n", c);
    return c ? 1 : 0;
}
EOF

# ---------------------------------------------------------------------------
# 4) Build and run, parameterised on the rustc cfg. Returns the process code.
# ---------------------------------------------------------------------------
build_and_run() {   # $1 = extra rustc flags, $2 = label
    local rsflags="$1" label="$2"
    local rslib="$WORK/libsecstore_$label.a"
    local bin="$WORK/kat_$label"

    if ! "$RUSTC" --edition 2021 -O --crate-type=staticlib \
            --crate-name "secstore_$label" $rsflags \
            "$WORK/secstore.rs" -o "$rslib" 2> "$WORK/rs_$label.log"; then
        echo "FATAL: rustc failed for $label"; cat "$WORK/rs_$label.log"; exit 2
    fi
    # A warning is fine; a missing symbol is not. Check the export is really there.
    if ! nm "$rslib" 2>/dev/null | grep -q ' T secstore_selftest_rs'; then
        echo "FATAL: secstore_selftest_rs not exported from $rslib for $label"; exit 2
    fi

    # The crypto TU needs the shim headers; the DRIVER must not see them, because
    # the shim's int64_t is `long long` and <stdlib.h>'s is `long int`. Compile
    # them separately and link. chacha20_poly1305_kat.sh solves it the same way.
    if ! "$CC" -O2 -Wall -I "$WORK" -c "$WORK/chacha20_host.c" \
            -o "$WORK/chacha20_$label.o" 2> "$WORK/cc_$label.log"; then
        echo "FATAL: compile of chacha20.c failed for $label"
        cat "$WORK/cc_$label.log"; exit 2
    fi
    if ! "$CC" -O2 -Wall -c "$WORK/driver.c" \
            -o "$WORK/driver_$label.o" 2>> "$WORK/cc_$label.log"; then
        echo "FATAL: compile of driver.c failed for $label"
        cat "$WORK/cc_$label.log"; exit 2
    fi
    if ! "$CC" "$WORK/chacha20_$label.o" "$WORK/driver_$label.o" "$rslib" \
            -o "$bin" 2>> "$WORK/cc_$label.log"; then
        echo "FATAL: link failed for $label"; cat "$WORK/cc_$label.log"; exit 2
    fi
    "$bin"; return $?
}

echo "== GREEN: unmodified secstore.rs + real crypto/chacha20.c AEAD =="
build_and_run "" green
GREEN=$?

echo "== RED:   --cfg secstore_kat_fault (header dropped from the AAD) =="
build_and_run "--cfg secstore_kat_fault" red
RED=$?

echo "------------------------------------------------------------"
rc=0
if [ "$GREEN" -eq 0 ]; then
    echo "GREEN ok  : round trip correct AND all 14 refusals fire (exit 0)"
else
    echo "GREEN FAIL: correct code did not pass the KAT (exit $GREEN)"; rc=1
fi
if [ "$RED" -ne 0 ]; then
    echo "RED   ok  : an unauthenticated header is CAUGHT by the KAT (exit $RED)"
else
    echo "RED   FAIL: KAT did not catch a header excluded from the AAD -> it is a no-op"; rc=1
fi
echo "------------------------------------------------------------"
[ "$rc" -eq 0 ] && echo "secstore_kat.sh: PASS" || echo "secstore_kat.sh: FAIL"
exit $rc
