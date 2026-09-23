#!/usr/bin/env bash
#
# ed25519_verify_kat.sh - build-container known-answer oracle for
# ed25519_verify_selftest() (#658 precursor, secroadmap).
#
# WHY THIS FILE EXISTS
# The boot-time ed25519_decode_selftest() proves ONLY the point-DECODE
# differential (unpack25519 rs==c). It cannot see a bug in the full verify: a
# byte-identical decoder says nothing about whether ed25519_verify rejects a
# forgery. ed25519_verify is the trust anchor for SSH client-key auth, App Store
# signing (#559) and the planned Secure Boot (#658). This oracle compiles the
# REAL ed25519.c (kernel headers shimmed, functions byte-identical) against a
# tiny host driver and proves, without a VM:
#
#   GREEN : the unmodified verify ACCEPTS the RFC 8032 vectors and REJECTS every
#           single-bit tamper  -> ed25519_verify_selftest() returns 0.
#   RED   : compiled with -DED25519_KAT_FAULT (an unconditional-accept verifier,
#           the single most dangerous verify bug) the SAME self-test returns
#           non-zero, proving its forgery-rejection checks actually FIRE and are
#           not a no-op.
#
# The KAT vectors themselves need no external oracle: a correct verifier plus a
# correct vector table is the ONLY combination that yields GREEN, so a passing
# GREEN run also validates the transcribed RFC 8032 vectors.
#
# Exit 0 iff GREEN is green AND RED is red. Run it in the kernel (the build container,
# gcc-12) or userland (the build container) build container:
#     kernel/crypto/ed25519_verify_kat.sh
set -u

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
EDC="$SRCDIR/ed25519.c"
CC="${CC:-gcc}"
[ -r "$EDC" ] || { echo "FATAL: cannot read $EDC"; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# 1) Shim header: the four kernel includes ed25519.c pulls in, reduced to the
#    handful of typedefs and prototypes the verify path actually needs.
cat > "$WORK/shim.h" <<'EOF'
#ifndef ED25519_KAT_SHIM_H
#define ED25519_KAT_SHIM_H
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned long      size_t;
void  *kmalloc(size_t);
void   kfree(void *);
void  *memcpy(void *, const void *, size_t);
int    kprintf(const char *, ...);
void   bootlog_write(const char *, ...);
#endif
EOF

# 2) Host copy of ed25519.c with the kernel includes swapped for the shim. Only
#    the #include lines change; every function under test is byte-identical.
sed -E \
  -e 's|^#include "ed25519.h"|#include "shim.h"|' \
  -e 's|^#include "\.\./string.h"||' \
  -e 's|^#include "\.\./mm/heap.h"||' \
  -e 's|^#include "fs/bootlog.h".*||' \
  "$EDC" > "$WORK/ed25519_host.c"

# 3) Host driver: stubs + a main() that runs the self-test and returns its
#    failure count as the process exit status.
cat > "$WORK/driver.c" <<'EOF'
/* Driver uses system headers only; it does NOT include shim.h, so its int64_t
   (from <stdlib.h>) never clashes with the shim's. size_t comes from stddef. */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
void *kmalloc(size_t n) { return malloc(n); }
void  kfree(void *p) { free(p); }
int   kprintf(const char *f, ...) { va_list a; va_start(a,f); int r=vprintf(f,a); va_end(a); return r; }
void  bootlog_write(const char *f, ...) { (void)f; }
/* referenced by the (uncalled) decode self-test; stub keeps the link resolved */
void  unpack25519_rs(long long *o, const unsigned char *n) { (void)o; (void)n; }
extern int ed25519_verify_selftest(void);
int main(void) {
    int fail = ed25519_verify_selftest();
    printf("HOST-KAT: ed25519_verify_selftest() returned fail=%d\n", fail);
    return fail ? 1 : 0;
}
EOF

build_and_run() {  # $1 = extra cflags, $2 = label ; echoes exit code
    local flags="$1" label="$2" bin="$WORK/kat_$2"
    if ! "$CC" -O2 -Wall -I "$WORK" $flags \
            "$WORK/ed25519_host.c" "$WORK/driver.c" -o "$bin" 2> "$WORK/cc_$label.log"; then
        echo "FATAL: compile failed for $label"; cat "$WORK/cc_$label.log"; exit 2
    fi
    "$bin"; return $?
}

echo "== GREEN: unmodified ed25519_verify =="
build_and_run "" green
GREEN=$?
echo "== RED:   -DED25519_KAT_FAULT (unconditional-accept verifier) =="
build_and_run "-DED25519_KAT_FAULT" red
RED=$?

echo "------------------------------------------------------------"
rc=0
if [ "$GREEN" -eq 0 ]; then echo "GREEN ok  : verify accepts RFC8032 vectors and rejects all tampers (exit 0)";
else echo "GREEN FAIL: correct verify did not pass the KAT (exit $GREEN)"; rc=1; fi
if [ "$RED" -ne 0 ]; then echo "RED   ok  : always-accept verifier is CAUGHT by the KAT (exit $RED)";
else echo "RED   FAIL: KAT did not catch an always-accept verifier -> it is a no-op"; rc=1; fi
echo "------------------------------------------------------------"
[ "$rc" -eq 0 ] && echo "ed25519_verify_kat.sh: PASS" || echo "ed25519_verify_kat.sh: FAIL"
exit $rc
