#!/usr/bin/env bash
#
# rsa_verify_kat.sh - build-container known-answer oracle for
# rsa_verify_selftest() (secroad2).
#
# WHY THIS FILE EXISTS
# rsa_verify_pkcs1_sha256 (crypto/rsa.c) is the X.509 certificate SIGNATURE
# verifier for RSA-signed certs: net/tls/cert_store.c:rsa_verify_pkcs1() calls it
# to authenticate every RSA cert in a TLS chain, and RSA is the most common CA
# signature algorithm on the public web. It is the "is this the real server"
# trust anchor, exactly like ecdsa_verify (#659). Until secroad2 the tree had NO
# known-answer test of it and nothing proving it REJECTS a forgery (the
# [TLS1.2-SELFTEST] exercises only the PSS path, not the PKCS#1 v1.5 path real
# certs use). This oracle compiles the REAL rsa.c, byte-identical, against a tiny
# host driver and proves, without a VM:
#
#   GREEN : unmodified rsa_verify_pkcs1_sha256 ACCEPTS a genuine RSA-2048 PKCS#1
#           v1.5 SHA-256 signature (openssl-generated; cross-checked "Verified
#           OK") and REJECTS every single-bit tamper, a truncated signature, an
#           all-0xFF / all-zero signature and a flipped modulus bit ->
#           rsa_verify_selftest() returns 0.
#   RED   : compiled with -DRSA_KAT_FAULT (an unconditional-accept verifier, the
#           single most dangerous verify bug, == a cert_store stub that
#           authenticates every RSA cert) the SAME self-test returns non-zero,
#           proving its forgery-rejection checks actually FIRE and are not no-ops.
#
# The vector embeds only PUBLIC values (modulus n, exponent e, message digest,
# signature) - no private key. A correct verifier accepts iff the signature is
# valid under (n,e), so a passing GREEN run also validates the embedded vector.
#
# Exit 0 iff GREEN is green AND RED is red. Run it in the kernel (the build container,
# gcc-12) or userland (the build container) build container:
#     kernel/crypto/rsa_verify_kat.sh
set -u

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
RSC="$SRCDIR/rsa.c"
CC="${CC:-gcc}"
[ -r "$RSC" ] || { echo "FATAL: cannot read $RSC"; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/crypto" "$WORK/mm" "$WORK/fs"

# 1) Shim the kernel headers rsa.c pulls in, reduced to what the verify path
#    needs. Quote-includes resolve relative to the source file, so the real
#    rsa.c copied into $WORK/crypto/ picks these up unchanged.
cat > "$WORK/types.h" <<'EOF'
#ifndef KAT_TYPES_H
#define KAT_TYPES_H
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
cat > "$WORK/string.h" <<'EOF'
#ifndef KAT_STRING_H
#define KAT_STRING_H
#include "types.h"
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int   memcmp(const void *, const void *, size_t);
#endif
EOF
cat > "$WORK/serial.h" <<'EOF'
#ifndef KAT_SERIAL_H
#define KAT_SERIAL_H
void kprintf(const char *fmt, ...);
#endif
EOF
cat > "$WORK/mm/heap.h" <<'EOF'
#ifndef KAT_HEAP_H
#define KAT_HEAP_H
#include "../types.h"
void *kmalloc(size_t);
void  kfree(void *);
#endif
EOF
cat > "$WORK/crypto/crypto.h" <<'EOF'
#ifndef KAT_CRYPTO_H
#define KAT_CRYPTO_H
#include "../types.h"
void crypto_zero(void *ptr, size_t length);
int  crypto_memcmp(const void *a, const void *b, size_t length);
#endif
EOF
cat > "$WORK/fs/bootlog.h" <<'EOF'
#ifndef KAT_BOOTLOG_H
#define KAT_BOOTLOG_H
int bootlog_write(const char *fmt, ...);
#endif
EOF

# 2) The REAL rsa.c + its API header, byte-identical. Only kernel headers above
#    are shimmed; every function under test is unchanged.
cp "$RSC" "$WORK/crypto/rsa.c"
cp "$SRCDIR/rsa.h" "$WORK/crypto/rsa.h"

# 3) Host driver: libc-backed stubs + a main() that runs the self-test and
#    returns its failure count as the process exit status. Uses SYSTEM headers
#    only (must NOT pick up the shim types.h: its int64_t=long long would clash
#    with libc). crypto_memcmp is a real constant-time compare; emsa_pss_verify_rs
#    and rng_get_bytes are referenced only by RSA paths this KAT never calls.
cat > "$WORK/driver.c" <<'EOF'
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
void *kmalloc(size_t n) { return malloc(n); }
void  kfree(void *p) { free(p); }
void  crypto_zero(void *p, size_t n) { volatile unsigned char *q = p; while (n--) *q++ = 0; }
int   crypto_memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x=a,*y=b; int d=0; while (n--) d |= *x++ ^ *y++; return d; }
int   kprintf(const char *f, ...) { va_list a; va_start(a,f); int r=vprintf(f,a); va_end(a); return r; }
int   bootlog_write(const char *f, ...) { (void)f; return 0; }
int   rng_get_bytes(void *p, unsigned long n) { unsigned char *q=p; while (n--) *q++ = 0; return 0; }
int   emsa_pss_verify_rs(void) { return -1; }
extern int rsa_verify_selftest(void);
int main(void) {
    int fail = rsa_verify_selftest();
    printf("HOST-KAT: rsa_verify_selftest() returned fail=%d\n", fail);
    return fail ? 1 : 0;
}
EOF

build_and_run() {  # $1 = extra cflags, $2 = label ; returns child exit code
    local flags="$1" label="$2" bin="$WORK/kat_$2"
    if ! "$CC" -O2 -Wall -I "$WORK" -I "$WORK/crypto" $flags             "$WORK/crypto/rsa.c" "$WORK/driver.c"             -o "$bin" 2> "$WORK/cc_$label.log"; then
        echo "FATAL: compile failed for $label"; cat "$WORK/cc_$label.log"; exit 2
    fi
    "$bin"; return $?
}

echo "== GREEN: unmodified rsa_verify_pkcs1_sha256 =="
build_and_run "" green
GREEN=$?
echo "== RED:   -DRSA_KAT_FAULT (unconditional-accept verifier) =="
build_and_run "-DRSA_KAT_FAULT" red
RED=$?

echo "------------------------------------------------------------"
rc=0
if [ "$GREEN" -eq 0 ]; then echo "GREEN ok  : verify accepts the RSA-2048 PKCS#1 v1.5 vector and rejects all tampers (exit 0)";
else echo "GREEN FAIL: correct verify did not pass the KAT (exit $GREEN)"; rc=1; fi
if [ "$RED" -ne 0 ]; then echo "RED   ok  : always-accept verifier is CAUGHT by the KAT (exit $RED)";
else echo "RED   FAIL: KAT did not catch an always-accept verifier -> it is a no-op"; rc=1; fi
echo "------------------------------------------------------------"
[ "$rc" -eq 0 ] && echo "rsa_verify_kat.sh: PASS" || echo "rsa_verify_kat.sh: FAIL"
exit $rc
