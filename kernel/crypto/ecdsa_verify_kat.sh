#!/usr/bin/env bash
#
# ecdsa_verify_kat.sh - build-container known-answer oracle for
# ecdsa_verify_selftest() (#659, crypkat).
#
# WHY THIS FILE EXISTS
# ecdsa_verify (crypto/ecdsa.c) is the X.509 certificate SIGNATURE verifier:
# net/tls/cert_store.c calls it to authenticate every ECDSA-signed cert in a TLS
# chain. It is the "is this the real server" trust anchor, and it REPLACED a
# cert_store.c stub that literally "return 0"-ed and so accepted every ECDSA
# certificate. Until #659 the tree had NO known-answer test of it, and nothing
# proving it REJECTS a forgery. This oracle compiles the REAL ecdsa.c + rsa.c
# (the shared bignum math it builds on), byte-identical, against a tiny host
# driver and proves, without a VM:
#
#   GREEN : unmodified ecdsa_verify ACCEPTS the RFC 6979 A.2.5 P-256/SHA-256
#           vectors and REJECTS every single-bit tamper and the r=0/s=0/r=n
#           degenerate signatures -> ecdsa_verify_selftest() returns 0.
#   RED   : compiled with -DECDSA_KAT_FAULT (an unconditional-accept verifier,
#           the single most dangerous verify bug, == the old cert_store.c stub)
#           the SAME self-test returns non-zero, proving its forgery-rejection
#           checks actually FIRE and are not a no-op.
#
# The KAT vectors need no external oracle: a correct verifier plus a correct
# vector table is the ONLY combination that yields GREEN, so a passing GREEN run
# also validates the transcribed RFC 6979 vectors.
#
# Exit 0 iff GREEN is green AND RED is red. Run it in the kernel (the build container,
# gcc-12) or userland (the build container) build container:
#     kernel/crypto/ecdsa_verify_kat.sh
set -u

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
ECC="$SRCDIR/ecdsa.c"
RSC="$SRCDIR/rsa.c"
CC="${CC:-gcc}"
[ -r "$ECC" ] || { echo "FATAL: cannot read $ECC"; exit 2; }
[ -r "$RSC" ] || { echo "FATAL: cannot read $RSC"; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/crypto" "$WORK/mm"

# 1) Shim the kernel headers ecdsa.c/rsa.c pull in, reduced to what the verify
#    path actually needs. Quote-includes resolve relative to the source file, so
#    the real ecdsa.c/rsa.c copied into $WORK/crypto/ pick these up unchanged.
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
#endif
EOF
cat > "$WORK/crypto/csprng.h" <<'EOF'
#ifndef KAT_CSPRNG_H
#define KAT_CSPRNG_H
#include "../types.h"
void csprng_bytes(uint8_t *out, size_t len);
#endif
EOF

# 2) The REAL ecdsa.c, rsa.c and their API headers, byte-identical. Only the
#    kernel headers above are shimmed; every function under test is unchanged.
# ecdsa.c is byte-identical EXCEPT the fs/bootlog.h include is stripped (that
# header pulls the FAT/VFS stack a host build has no business linking); the
# driver stubs bootlog_write. Same technique as ed25519_verify_kat.sh.
sed -E -e 's|^#include "fs/bootlog.h".*||' "$ECC" > "$WORK/crypto/ecdsa.c"
cp "$RSC" "$WORK/crypto/rsa.c"
cp "$SRCDIR/ecdsa.h" "$WORK/crypto/ecdsa.h"
cp "$SRCDIR/rsa.h"   "$WORK/crypto/rsa.h"

# 3) Host driver: libc-backed stubs + a main() that runs the self-test and
#    returns its failure count as the process exit status.
#    NOTE: the driver uses SYSTEM headers only and must NOT pick up the shim
#    types.h/string.h (their int64_t = long long would clash with libc's). It is
#    therefore compiled with system includes ahead of $WORK, via a plain filename
#    that does not exist in $WORK.
cat > "$WORK/driver.c" <<'EOF'
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
void *kmalloc(size_t n) { return malloc(n); }
void  kfree(void *p) { free(p); }
void  crypto_zero(void *p, size_t n) { volatile unsigned char *q = p; while (n--) *q++ = 0; }
void  csprng_bytes(unsigned char *o, size_t n) { while (n--) *o++ = 0; } /* verify path unused */
int   kprintf(const char *f, ...) { va_list a; va_start(a,f); int r=vprintf(f,a); va_end(a); return r; }
void  bootlog_write(const char *f, ...) { (void)f; }
/* rsa.c (compiled for its shared bignum math) references these in RSA-only
   paths the ECDSA self-test never calls; stub them so the link resolves. */
int   rng_get_bytes(void *p, unsigned long n) { unsigned char *q=p; while (n--) *q++ = 0; return 0; }
int   crypto_memcmp(const void *a, const void *b, unsigned long n) {
    const unsigned char *x=a,*y=b; int d=0; while (n--) d |= *x++ ^ *y++; return d; }
void  sha256(void *o, const void *i, unsigned long n) { (void)i;(void)n; unsigned char *p=o; for(int k=0;k<32;k++)p[k]=0; }
void  sha384(void *o, const void *i, unsigned long n) { (void)i;(void)n; unsigned char *p=o; for(int k=0;k<48;k++)p[k]=0; }
void  sha512(void *o, const void *i, unsigned long n) { (void)i;(void)n; unsigned char *p=o; for(int k=0;k<64;k++)p[k]=0; }
int   emsa_pss_verify_rs(void) { return 0; }
extern int ecdsa_verify_selftest(void);
int main(void) {
    int fail = ecdsa_verify_selftest();
    printf("HOST-KAT: ecdsa_verify_selftest() returned fail=%d\n", fail);
    return fail ? 1 : 0;
}
EOF

build_and_run() {  # $1 = extra cflags, $2 = label ; returns child exit code
    local flags="$1" label="$2" bin="$WORK/kat_$2"
    if ! "$CC" -O2 -Wall -I "$WORK" -I "$WORK/crypto" $flags \
            "$WORK/crypto/ecdsa.c" "$WORK/crypto/rsa.c" "$WORK/driver.c" \
            -o "$bin" 2> "$WORK/cc_$label.log"; then
        echo "FATAL: compile failed for $label"; cat "$WORK/cc_$label.log"; exit 2
    fi
    "$bin"; return $?
}

echo "== GREEN: unmodified ecdsa_verify =="
build_and_run "" green
GREEN=$?
echo "== RED:   -DECDSA_KAT_FAULT (unconditional-accept verifier) =="
build_and_run "-DECDSA_KAT_FAULT" red
RED=$?

echo "------------------------------------------------------------"
rc=0
if [ "$GREEN" -eq 0 ]; then echo "GREEN ok  : verify accepts RFC6979 vectors and rejects all tampers (exit 0)";
else echo "GREEN FAIL: correct verify did not pass the KAT (exit $GREEN)"; rc=1; fi
if [ "$RED" -ne 0 ]; then echo "RED   ok  : always-accept verifier is CAUGHT by the KAT (exit $RED)";
else echo "RED   FAIL: KAT did not catch an always-accept verifier -> it is a no-op"; rc=1; fi
echo "------------------------------------------------------------"
[ "$rc" -eq 0 ] && echo "ecdsa_verify_kat.sh: PASS" || echo "ecdsa_verify_kat.sh: FAIL"
exit $rc
