#!/usr/bin/env bash
#
# chacha20_poly1305_kat.sh - build-container known-answer oracle for
# chacha20_poly1305_selftest() (aeadkat).
#
# WHY THIS FILE EXISTS
# The boot-time chacha20_rust_selftest() proves ONLY the ChaCha20 keystream
# BLOCK (RFC 8439 section 2.3.2). It is BLIND to the AEAD: nothing tests whether
# Poly1305, the length/pad blocks, or the tag COMPARISON are correct, and above
# all nothing tests whether chacha20_poly1305_open() REJECTS a forged record.
# That AEAD is the live TLS 1.3 data plane for TLS13_CHACHA20_POLY1305_SHA256
# (net/tls/tls13.c). A broken tag check silently accepts attacker-modified TLS
# records. AES-GCM already has its tamper-reject KAT (ghash_rust_selftest, NIST
# TC4 + 3 negatives); this is the matching proof for the ChaCha20-Poly1305 suite.
#
# This oracle compiles the REAL crypto/chacha20.c (kernel headers shimmed,
# functions byte-identical) against a tiny host driver and proves, without a VM:
#
#   GREEN : the unmodified AEAD ENCRYPTS the RFC 8439 2.8.2 vector to the known
#           ciphertext+tag, DECRYPTS it back (ACCEPT), and REJECTS a flipped
#           ciphertext byte, a flipped tag byte, a flipped AAD byte, and a
#           truncated tag; the Poly1305 2.5.2 MAC matches -> selftest returns 0.
#   RED   : compiled with -DAEADKAT_FAULT (an unconditional-accept tag check, the
#           single most dangerous AEAD bug) the SAME self-test returns non-zero,
#           proving its tamper-rejection checks actually FIRE and are not no-ops.
#
# A correct AEAD plus a correct vector table is the ONLY combination that yields
# GREEN, so a passing GREEN run also validates the transcribed RFC 8439 vectors.
#
# Exit 0 iff GREEN is green AND RED is red. Run it in the kernel (the build container,
# gcc-12) or userland (the build container) build container:
#     kernel/crypto/chacha20_poly1305_kat.sh
set -u

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
CCFILE="$SRCDIR/chacha20.c"
CC="${CC:-gcc}"
[ -r "$CCFILE" ] || { echo "FATAL: cannot read $CCFILE"; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# 1) Shim headers. chacha20.c includes chacha20.h, crypto.h, ../string.h and
#    fs/bootlog.h. We keep chacha20.h (its only include is ../types.h, shimmed)
#    and swap the other three for a tiny shim providing just the typedefs and
#    prototypes the AEAD path references.
cat > "$WORK/types.h" <<'EOF'
#ifndef AEADKAT_TYPES_H
#define AEADKAT_TYPES_H
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
#ifndef AEADKAT_CRYPTO_H
#define AEADKAT_CRYPTO_H
#include "types.h"
int  crypto_memcmp(const void *a, const void *b, size_t length);
void crypto_zero(void *ptr, size_t length);
#endif
EOF

cat > "$WORK/string.h" <<'EOF'
#ifndef AEADKAT_STRING_H
#define AEADKAT_STRING_H
#include "types.h"
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int   memcmp(const void *, const void *, size_t);
#endif
EOF

cat > "$WORK/kshim.h" <<'EOF'
#ifndef AEADKAT_KSHIM_H
#define AEADKAT_KSHIM_H
int  kprintf(const char *, ...);
void bootlog_write(const char *, ...);
#endif
EOF

# The kernel chacha20.h #include "../types.h"; point it at our shim.
cp "$SRCDIR/chacha20.h" "$WORK/chacha20.h.orig"
sed -E 's|^#include "\.\./types\.h"|#include "types.h"|' \
    "$WORK/chacha20.h.orig" > "$WORK/chacha20.h"

# 2) Host copy of chacha20.c with the kernel includes swapped for the shims.
#    Only the #include lines change; every function under test is byte-identical.
sed -E \
  -e 's|^#include "\.\./string\.h"|#include "string.h"|' \
  -e 's|^#include "fs/bootlog\.h".*|#include "kshim.h"|' \
  "$CCFILE" > "$WORK/chacha20_host.c"

# 3) Host driver: stubs for everything chacha20.c's static call graph references
#    but that is not on the AEAD path, plus a main() that runs the self-test and
#    returns its failure count as the process exit status.
cat > "$WORK/driver.c" <<'EOF'
/* System headers only; does NOT include the shim (its int64_t = long long would
   clash with <stdlib.h>'s int64_t = long int). size_t via stddef. */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

/* string helpers the crypto file calls */
/* memcpy/memset come from <string.h> */

/* crypto.c helpers, reimplemented faithfully for the host (constant-timeness is
   irrelevant to correctness; 0 iff equal). */
int crypto_memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b; unsigned char d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned char)(x[i] ^ y[i]);
    return d ? 1 : 0;
}
void crypto_zero(void *p, size_t n) { volatile unsigned char *q = p; while (n--) *q++ = 0; }

/* logging */
int  kprintf(const char *f, ...) { va_list a; va_start(a,f); int r=vprintf(f,a); va_end(a); return r; }
void bootlog_write(const char *f, ...) { (void)f; }

/* Rust FFI symbol referenced by chacha20.c (only used under -DRUST_CHACHA20,
   which this host build does NOT define; the stub keeps the link resolved). */
void chacha20_block_rs(const unsigned int input[16], unsigned char out[64]) {
    (void)input; (void)out;
}

extern int chacha20_poly1305_selftest(void);
int main(void) {
    int fail = chacha20_poly1305_selftest();
    printf("HOST-KAT: chacha20_poly1305_selftest() returned fail=%d\n", fail);
    return fail ? 1 : 0;
}
EOF

build_and_run() {  # $1 = extra cflags, $2 = label ; returns process exit code
    local flags="$1" label="$2" bin="$WORK/kat_$2"
    # The crypto file compiles against the shim (-I WORK). The driver compiles
    # against SYSTEM headers only (NO -I WORK): its <string.h>/<stdlib.h> must be
    # the real ones, and the shim's int64_t = long long would clash otherwise.
    if ! "$CC" -O2 -Wall -I "$WORK" $flags -c \
            "$WORK/chacha20_host.c" -o "$WORK/chacha20_$label.o" 2> "$WORK/cc_$label.log"; then
        echo "FATAL: compile failed for $label (crypto)"; cat "$WORK/cc_$label.log"; exit 2
    fi
    if ! "$CC" -O2 -Wall -c \
            "$WORK/driver.c" -o "$WORK/driver_$label.o" 2>> "$WORK/cc_$label.log"; then
        echo "FATAL: compile failed for $label (driver)"; cat "$WORK/cc_$label.log"; exit 2
    fi
    if ! "$CC" "$WORK/chacha20_$label.o" "$WORK/driver_$label.o" -o "$bin" 2>> "$WORK/cc_$label.log"; then
        echo "FATAL: link failed for $label"; cat "$WORK/cc_$label.log"; exit 2
    fi
    "$bin"; return $?
}

echo "== GREEN: unmodified chacha20_poly1305 AEAD =="
build_and_run "" green
GREEN=$?
echo "== RED:   -DAEADKAT_FAULT (unconditional-accept tag check) =="
build_and_run "-DAEADKAT_FAULT" red
RED=$?

echo "------------------------------------------------------------"
rc=0
if [ "$GREEN" -eq 0 ]; then echo "GREEN ok  : AEAD matches RFC8439 2.8.2 ct+tag, accepts valid, rejects every tamper (exit 0)";
else echo "GREEN FAIL: correct AEAD did not pass the KAT (exit $GREEN)"; rc=1; fi
if [ "$RED" -ne 0 ]; then echo "RED   ok  : always-accept tag check is CAUGHT by the KAT (exit $RED)";
else echo "RED   FAIL: KAT did not catch an always-accept tag check -> it is a no-op"; rc=1; fi
echo "------------------------------------------------------------"
[ "$rc" -eq 0 ] && echo "chacha20_poly1305_kat.sh: PASS" || echo "chacha20_poly1305_kat.sh: FAIL"
exit $rc
