#!/usr/bin/env bash
#
# ecdhkat_x25519_kat.sh - build-container known-answer oracle for
# x25519_selftest() (ecdhkat).
#
# WHY THIS FILE EXISTS
# X25519 is the ECDH key agreement behind the TLS 1.3 key_share and the TLS 1.2
# x25519 ECDHE: x25519_scalar_mult() alone determines the whole session shared
# secret. A silently-wrong Montgomery ladder produces the wrong secret (every
# handshake breaks) or a weak/known one (confidentiality lost), and nothing in
# the tree proved it against a published vector. This oracle EXTRACTS the real
# Curve25519 field arithmetic + ladder + agreement span from tls13.c (the
# X25519_KAT_REGION_BEGIN..END markers) and compiles THAT, so it can never rot
# out of sync with the shipping code, then proves without a VM:
#
#   GREEN : as shipped, x25519_selftest() ACCEPTS the RFC 7748 5.2 scalar-mult
#           vectors, derives the RFC 7748 6.1 Alice/Bob shared secret K on both
#           sides (agreement), a flipped private bit CHANGES it, and a
#           small-order peer point yields all-zero -> returns 0.
#   RED   : compiled with -DX25519_KAT_FAULT (the ladder stubbed to return the
#           peer point unchanged - an identity/passthrough shared secret) the
#           SAME self-test returns non-zero, proving its known-answer, agreement
#           and negative checks actually FIRE and are not a no-op.
#
# A correct ladder plus a correct vector table is the ONLY combination that
# yields GREEN, so a passing GREEN also validates the transcribed vectors.
#
# Exit 0 iff GREEN is green AND RED is red. Run in the kernel (the build container, gcc-12)
# or userland (the build container) build container:
#     kernel/net/tls/ecdhkat_x25519_kat.sh
set -u

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SRCDIR/tls13.c"
CC="${CC:-gcc}"
[ -r "$SRC" ] || { echo "FATAL: cannot read $SRC"; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# 1) Extract EXACTLY the marked region (real shipping field arithmetic + ladder
#    + x25519_shared_secret + x25519_selftest). It carries no #include lines.
sed -n '/X25519_KAT_REGION_BEGIN/,/X25519_KAT_REGION_END/p' "$SRC" > "$WORK/region.c"
if ! grep -q x25519_selftest "$WORK/region.c"; then
    echo "FATAL: region markers not found or empty in $SRC"; exit 2
fi

# 2) Shim header: only the typedefs + prototypes the region actually needs.
cat > "$WORK/shim.h" <<'EOF'
#ifndef X25519_KAT_SHIM_H
#define X25519_KAT_SHIM_H
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned long      size_t;
typedef struct { uint8_t public_key[32]; uint8_t private_key[32]; } x25519_keypair_t;
void  *memcpy(void *, const void *, size_t);
int    memcmp(const void *, const void *, size_t);
void  *memset(void *, int, size_t);
void   csprng_bytes(void *, size_t);
int    kprintf(const char *, ...);
int    bootlog_write(const char *, ...);
#endif
EOF

# Host translation unit = shim + the extracted region.
printf '#include "shim.h"\n' > "$WORK/region_host.c"
cat "$WORK/region.c" >> "$WORK/region_host.c"

# 3) Driver: system headers only (no shim.h, so its size_t never clashes),
#    stubs for the kernel-only symbols, and a main() that returns the
#    self-test failure count as the process exit status.
cat > "$WORK/driver.c" <<'EOF'
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
/* Not exercised by the self-test (it uses fixed private keys), but the region
   contains x25519_generate_keypair which references it; keep the link resolved. */
void csprng_bytes(void *p, size_t n) { unsigned char *b = p; for (size_t i=0;i<n;i++) b[i]=0; }
int  kprintf(const char *f, ...) { va_list a; va_start(a,f); int r=vprintf(f,a); va_end(a); return r; }
int  bootlog_write(const char *f, ...) { (void)f; return 0; }
extern int x25519_selftest(void);
int main(void) {
    int fail = x25519_selftest();
    printf("HOST-KAT: x25519_selftest() returned fail=%d\n", fail);
    return fail ? 1 : 0;
}
EOF

build_and_run() {  # $1 = extra cflags, $2 = label ; returns child exit code
    local flags="$1" label="$2" bin="$WORK/kat_$2"
    if ! "$CC" -O2 -Wall -I "$WORK" $flags             "$WORK/region_host.c" "$WORK/driver.c" -o "$bin" 2> "$WORK/cc_$label.log"; then
        echo "FATAL: compile failed for $label"; cat "$WORK/cc_$label.log"; exit 2
    fi
    "$bin"; return $?
}

echo "== GREEN: unmodified x25519 ladder =="
build_and_run "" green
GREEN=$?
echo "== RED:   -DX25519_KAT_FAULT (identity/passthrough ladder) =="
build_and_run "-DX25519_KAT_FAULT" red
RED=$?

echo "------------------------------------------------------------"
rc=0
if [ "$GREEN" -eq 0 ]; then echo "GREEN ok  : ladder matches RFC7748 5.2+6.1 and negatives fire (exit 0)";
else echo "GREEN FAIL: correct ladder did not pass the KAT (exit $GREEN)"; rc=1; fi
if [ "$RED" -ne 0 ]; then echo "RED   ok  : identity-ladder is CAUGHT by the KAT (exit $RED)";
else echo "RED   FAIL: KAT did not catch an identity ladder -> it is a no-op"; rc=1; fi
echo "------------------------------------------------------------"
[ "$rc" -eq 0 ] && echo "ecdhkat_x25519_kat.sh: PASS" || echo "ecdhkat_x25519_kat.sh: FAIL"
exit $rc
