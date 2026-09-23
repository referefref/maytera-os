#!/usr/bin/env bash
#
# pbkdf2_shadow_kat.sh - build-container known-answer oracle for
# pbkdf2_shadow_selftest() (authkat).
#
# WHY THIS FILE EXISTS
# verify_against_record() in kernel/proc/users.c is the password trust anchor for
# local login and the ext2 /CONFIG SHADOW file (root/admin asset accounts, and
# any account whose password was set). A passing kernel build with real callers
# proves NOTHING about whether it (a) computes PBKDF2-HMAC-SHA256 correctly or
# (b) REJECTS a wrong password / truncated record / flipped-byte record. This
# oracle compiles the REAL PBKDF2 + verify code (extracted verbatim between the
# PBKDF2_SHADOW_KAT_REGION_BEGIN/END markers in users.c, so it cannot drift from
# what ships) against the REAL crypto/sha256.c HMAC primitive, and proves,
# without a VM:
#
#   GREEN : the unmodified verify computes the RFC-style PBKDF2-HMAC-SHA256
#           vectors correctly AND accepts the right password AND rejects every
#           tamper  -> pbkdf2_shadow_selftest() returns 0.
#   RED   : compiled with -DPBKDF2_KAT_FAULT (an unconditional-accept verifier,
#           the single most dangerous auth bug and exactly what an unguarded
#           trust anchor must never be) the SAME self-test returns non-zero,
#           proving its reject checks actually FIRE and are not a no-op.
#
# The KAT vectors (P="password", S="salt", c in {1,2,4096}, dkLen=32) were
# independently reproduced with Python hashlib.pbkdf2_hmac; a correct verifier
# plus a correct vector table is the ONLY combination that yields GREEN, so a
# passing GREEN run also validates the transcribed vectors.
#
# Exit 0 iff GREEN is green AND RED is red. Run in the kernel build container
# (the build container, gcc-12) or userland (the build container):
#     kernel/crypto/pbkdf2_shadow_kat.sh
set -u

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
SHA="$SRCDIR/sha256.c"
USERS="$SRCDIR/../proc/users.c"
CC="${CC:-gcc}"
[ -r "$SHA" ]   || { echo "FATAL: cannot read $SHA"; exit 2; }
[ -r "$USERS" ] || { echo "FATAL: cannot read $USERS"; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# 1) Shim header: the kernel typedefs, crypto ctx types/constants and PBKDF2
#    constants the region + sha256.c need, reproduced from kernel/crypto/crypto.h
#    and kernel/proc/users.{c,h}. libc prototypes are resolved against the system
#    libc at link time.
cat > "$WORK/shim.h" <<'EOF'
#ifndef PBKDF2_KAT_SHIM_H
#define PBKDF2_KAT_SHIM_H
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned long      size_t;

/* from kernel/crypto/crypto.h */
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

/* from kernel/cpu/dlprof.h (measurement profiler; no-op in the host oracle) */
static inline uint64_t dp_tsc(void) { return 0; }
static volatile uint64_t g_dp_sha_cyc, g_dp_sha_bytes;

/* from kernel/proc/users.{c,h} */
#define PBKDF2_ITERATIONS 50000u
#define PBKDF2_SALT_LEN   16
#define PBKDF2_DK_LEN     32
#define PASSWORD_HASH_SIZE 160

/* libc used by the region + sha256.c (resolved against system libc) */
void  *memcpy(void *, const void *, size_t);
void  *memset(void *, int, size_t);
size_t strlen(const char *);
int    strncmp(const char *, const char *, size_t);
int    snprintf(char *, size_t, const char *, ...);
int    kprintf(const char *, ...);
void   bootlog_write(const char *, ...);
#endif
EOF

# 2) Host copy of the REAL crypto/sha256.c with its kernel includes swapped for
#    the shim. Only #include lines change; every hashing function is byte-identical.
sed -E \
  -e 's|^#include "crypto.h"|#include "shim.h"|' \
  -e 's|^#include "\.\./string.h".*||' \
  -e 's|^#include "\.\./cpu/dlprof.h".*||' \
  -e 's|^#include "fs/bootlog.h".*||' \
  "$SHA" > "$WORK/sha256_host.c"

# 3) Host copy of the REAL PBKDF2/verify/self-test region, extracted VERBATIM
#    between the markers in users.c so it cannot drift from what ships.
{
  echo '#include "shim.h"'
  awk '/PBKDF2_SHADOW_KAT_REGION_BEGIN/{f=1;next} /PBKDF2_SHADOW_KAT_REGION_END/{f=0} f' "$USERS"
} > "$WORK/region_host.c"

LINES=$(grep -c ';' "$WORK/region_host.c" || true)
if ! grep -q 'pbkdf2_shadow_selftest' "$WORK/region_host.c"; then
  echo "FATAL: region extraction found no pbkdf2_shadow_selftest (markers moved?)"; exit 2
fi

# 4) Host driver: stubs for the symbols the compiled TUs reference but that live
#    outside the crypto/auth path, plus a main() that runs the self-test and
#    returns its failure count as the process exit status. Uses system headers
#    only, so its libc types never clash with the shim.
cat > "$WORK/driver.c" <<'EOF'
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
/* crypto_zero: real behaviour (constant-time-ish memset), never optimised out */
void crypto_zero(void *p, unsigned long n) { volatile unsigned char *q = p; while (n--) *q++ = 0; }
/* referenced by sha256.c's (uncalled here) rust differential self-test */
void sha256_transform_rs(unsigned int *s, const unsigned char *b) { (void)s; (void)b; }
int  kprintf(const char *f, ...) { va_list a; va_start(a,f); int r=vprintf(f,a); va_end(a); return r; }
void bootlog_write(const char *f, ...) { (void)f; }
extern int pbkdf2_shadow_selftest(void);
int main(void) {
    int fail = pbkdf2_shadow_selftest();
    printf("HOST-KAT: pbkdf2_shadow_selftest() returned fail=%d\n", fail);
    return fail ? 1 : 0;
}
EOF

build_and_run() {  # $1 = extra cflags, $2 = label ; returns process exit code
    local flags="$1" label="$2" bin="$WORK/kat_$2"
    if ! "$CC" -O2 -Wall -Wno-array-parameter -Wno-unused-variable -Wno-unused-function -I "$WORK" $flags \
            "$WORK/region_host.c" "$WORK/sha256_host.c" "$WORK/driver.c" \
            -o "$bin" 2> "$WORK/cc_$label.log"; then
        echo "FATAL: compile failed for $label"; cat "$WORK/cc_$label.log"; exit 2
    fi
    "$bin"; return $?
}

echo "== GREEN: unmodified verify_against_record + real sha256.c HMAC =="
build_and_run "" green
GREEN=$?
echo "== RED:   -DPBKDF2_KAT_FAULT (unconditional-accept verifier) =="
build_and_run "-DPBKDF2_KAT_FAULT" red
RED=$?

echo "------------------------------------------------------------"
rc=0
if [ "$GREEN" -eq 0 ]; then echo "GREEN ok  : PBKDF2 vectors match AND verify accepts+rejects correctly (exit 0)";
else echo "GREEN FAIL: correct code did not pass the KAT (exit $GREEN)"; rc=1; fi
if [ "$RED" -ne 0 ]; then echo "RED   ok  : always-accept verifier is CAUGHT by the KAT (exit $RED)";
else echo "RED   FAIL: KAT did not catch an always-accept verifier -> it is a no-op"; rc=1; fi
echo "------------------------------------------------------------"
[ "$rc" -eq 0 ] && echo "pbkdf2_shadow_kat.sh: PASS" || echo "pbkdf2_shadow_kat.sh: FAIL"
exit $rc
