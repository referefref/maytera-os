#!/usr/bin/env bash
#
# pbkdf2_shadow_kat.sh - build-container known-answer oracle for
# pbkdf2_shadow_selftest() (authkat) and, since #secretuse, for the PROMOTED
# crypto/pbkdf2.c.
#
# WHY THIS FILE EXISTS
# verify_against_record() in kernel/proc/users.c is the password trust anchor for
# local login and the ext2 /CONFIG SHADOW file (root/admin asset accounts, and
# any account whose password was set). A passing kernel build with real callers
# proves NOTHING about whether it (a) computes PBKDF2-HMAC-SHA256 correctly or
# (b) REJECTS a wrong password / truncated record / flipped-byte record. This
# oracle compiles the REAL verify code (extracted verbatim between the
# PBKDF2_SHADOW_KAT_REGION_BEGIN/END markers in users.c, so it cannot drift from
# what ships) plus the REAL crypto/pbkdf2.c and the REAL crypto/sha256.c HMAC
# primitive, and proves, without a VM:
#
#   GREEN : the unmodified verify computes the RFC-style PBKDF2-HMAC-SHA256
#           vectors correctly AND accepts the right password AND rejects every
#           tamper  -> pbkdf2_shadow_selftest() returns 0.
#   RED 1 : compiled with -DPBKDF2_KAT_FAULT (an unconditional-accept verifier,
#           the single most dangerous auth bug and exactly what an unguarded
#           trust anchor must never be) the SAME self-test returns non-zero,
#           proving its reject checks actually FIRE and are not a no-op.
#   RED 2 : compiled with -DPBKDF2_CORE_FAULT, which collapses the ITERATION LOOP
#           inside crypto/pbkdf2.c to a single HMAC, the SAME self-test returns
#           non-zero.
#
# ===========================================================================
# WHY RED 2 EXISTS, AND IT IS THE REASON THIS FILE CHANGED IN #secretuse.
# ===========================================================================
# pbkdf2_hmac_sha256() used to be file-static INSIDE the marked region. It was
# promoted to crypto/pbkdf2.c so the encrypted secret store could derive a key
# from the same login password without a second PBKDF2 implementation.
#
# That move is exactly the shape of failure this repository has already suffered:
# a KAT that quietly stops covering the thing it is named after. The region
# extraction would still have compiled, this oracle would still have printed
# PASS, and the promoted KDF would have been tested by nothing at all. RED 1
# would NOT have noticed, because it faults the verifier in users.c and fires
# whether or not crypto/pbkdf2.c is even on the compile line.
#
# So RED 2 faults the loop IN THE PROMOTED FILE. Its fault is chosen to be
# invisible to everything except a known-answer vector: a single HMAC still
# returns a perfectly well-formed 32 bytes, still verifies against itself, and
# still lets every password in the system work. Only the RFC vectors disagree.
#
# GREEN proves the vectors match. RED 2 proves the vectors are computed BY THAT
# FILE. Together they are the evidence that the promotion did not silently end
# the coverage; separately, neither is.
#
# There is also a structural guard below: if the marked region ever RE-DEFINES
# pbkdf2_hmac_sha256 locally, the oracle exits 2 rather than testing two
# implementations and reporting on whichever one won the link.
#
# The KAT vectors (P="password", S="salt", c in {1,2,4096}, dkLen=32) were
# independently reproduced with Python hashlib.pbkdf2_hmac; a correct verifier
# plus a correct vector table is the ONLY combination that yields GREEN, so a
# passing GREEN run also validates the transcribed vectors.
#
# Exit 0 iff GREEN is green AND both REDs are red. Run in the kernel build
# container (the build container, gcc-12) or userland (the build container):
#     kernel/crypto/pbkdf2_shadow_kat.sh
set -u

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
SHA="$SRCDIR/sha256.c"
PBK_C="$SRCDIR/pbkdf2.c"
PBK_H="$SRCDIR/pbkdf2.h"
USERS="$SRCDIR/../proc/users.c"
CC="${CC:-gcc}"
for f in "$SHA" "$PBK_C" "$PBK_H" "$USERS"; do
  [ -r "$f" ] || { echo "FATAL: cannot read $f"; exit 2; }
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------------------
# 0) STRUCTURAL GUARDS. Each of these, if it trips, means the oracle would have
#    reported on something other than what ships. Exit 2 (oracle broken), which
#    is deliberately distinct from exit 1 (test failed).
# ---------------------------------------------------------------------------
grep -q 'void pbkdf2_hmac_sha256' "$PBK_C" || {
  echo "FATAL: crypto/pbkdf2.c does not define pbkdf2_hmac_sha256 (moved again?)"; exit 2; }
grep -q 'PBKDF2_CORE_FAULT' "$PBK_C" || {
  echo "FATAL: the RED-2 hook PBKDF2_CORE_FAULT is gone from crypto/pbkdf2.c."
  echo "       Without it this oracle cannot prove it still covers the promoted KDF."; exit 2; }
grep -q 'PBKDF2_KAT_FAULT' "$USERS" || {
  echo "FATAL: the RED-1 hook PBKDF2_KAT_FAULT is gone from proc/users.c"; exit 2; }
# If the region defines the KDF again, there would be TWO implementations and the
# link would silently pick one. Refuse rather than report on a coin toss.
if awk '/PBKDF2_SHADOW_KAT_REGION_BEGIN/{f=1;next} /PBKDF2_SHADOW_KAT_REGION_END/{f=0} f' \
      "$USERS" | grep -qE '^[[:space:]]*(static[[:space:]]+)?void[[:space:]]+pbkdf2_hmac_sha256'; then
  echo "FATAL: the users.c marked region DEFINES pbkdf2_hmac_sha256 again."
  echo "       It was promoted to crypto/pbkdf2.c in #secretuse. Two copies of a KDF"
  echo "       is the exact drift that promotion removed; pick one."
  exit 2
fi

# ---------------------------------------------------------------------------
# 1) Shim header: the kernel typedefs, crypto ctx types/constants the region +
#    sha256.c + pbkdf2.c need, reproduced from kernel/crypto/crypto.h. The PBKDF2
#    constants are deliberately NOT reproduced here: the REAL crypto/pbkdf2.h is
#    staged below and supplies them, so a change to the shipping work factor
#    reaches this oracle instead of being shadowed by a stale copy.
# ---------------------------------------------------------------------------
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

# 1b) The REAL crypto/pbkdf2.h, with only its kernel include swapped for the
#     shim. The work factor and dkLen under test are therefore the shipping ones.
sed -E 's|^#include "\.\./types\.h"|#include "shim.h"|' "$PBK_H" > "$WORK/pbkdf2.h"
grep -q 'PBKDF2_ITERATIONS' "$WORK/pbkdf2.h" || {
  echo "FATAL: staged pbkdf2.h has no PBKDF2_ITERATIONS (did the constants move again?)"; exit 2; }

# 2) Host copy of the REAL crypto/sha256.c with its kernel includes swapped for
#    the shim. Only #include lines change; every hashing function is byte-identical.
sed -E \
  -e 's|^#include "crypto.h"|#include "shim.h"|' \
  -e 's|^#include "\.\./string.h".*||' \
  -e 's|^#include "\.\./cpu/dlprof.h".*||' \
  -e 's|^#include "fs/bootlog.h".*||' \
  "$SHA" > "$WORK/sha256_host.c"

# 2b) Host copy of the REAL crypto/pbkdf2.c, same treatment. This is the file the
#     promotion created, and RED 2 faults it.
sed -E \
  -e 's|^#include "crypto.h"|#include "shim.h"|' \
  -e 's|^#include "\.\./string.h".*||' \
  "$PBK_C" > "$WORK/pbkdf2_host.c"

# 3) Host copy of the REAL verify/self-test region, extracted VERBATIM between
#    the markers in users.c so it cannot drift from what ships. It now includes
#    the staged pbkdf2.h for the KDF it calls, exactly as users.c does.
{
  echo '#include "shim.h"'
  echo '#include "pbkdf2.h"'
  awk '/PBKDF2_SHADOW_KAT_REGION_BEGIN/{f=1;next} /PBKDF2_SHADOW_KAT_REGION_END/{f=0} f' "$USERS"
} > "$WORK/region_host.c"

if ! grep -q 'pbkdf2_shadow_selftest' "$WORK/region_host.c"; then
  echo "FATAL: region extraction found no pbkdf2_shadow_selftest (markers moved?)"; exit 2
fi
# The region must still CALL the promoted KDF, or GREEN would be testing a
# verifier that no longer uses it and RED 2 would be unreachable.
if ! grep -q 'pbkdf2_hmac_sha256' "$WORK/region_host.c"; then
  echo "FATAL: the extracted region never calls pbkdf2_hmac_sha256, so this"
  echo "       oracle would not be exercising crypto/pbkdf2.c at all."; exit 2
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
            "$WORK/region_host.c" "$WORK/pbkdf2_host.c" "$WORK/sha256_host.c" "$WORK/driver.c" \
            -o "$bin" 2> "$WORK/cc_$label.log"; then
        echo "FATAL: compile failed for $label"; cat "$WORK/cc_$label.log"; exit 2
    fi
    "$bin"; return $?
}

echo "== GREEN: unmodified verify_against_record + crypto/pbkdf2.c + real sha256.c =="
build_and_run "" green
GREEN=$?
echo "== RED 1: -DPBKDF2_KAT_FAULT  (unconditional-accept verifier, users.c) =="
build_and_run "-DPBKDF2_KAT_FAULT" red1
RED1=$?
echo "== RED 2: -DPBKDF2_CORE_FAULT (iteration loop collapsed, crypto/pbkdf2.c) =="
build_and_run "-DPBKDF2_CORE_FAULT" red2
RED2=$?

echo "------------------------------------------------------------"
rc=0
if [ "$GREEN" -eq 0 ]; then echo "GREEN ok  : PBKDF2 vectors match AND verify accepts+rejects correctly (exit 0)";
else echo "GREEN FAIL: correct code did not pass the KAT (exit $GREEN)"; rc=1; fi
if [ "$RED1" -ne 0 ]; then echo "RED 1 ok  : always-accept verifier is CAUGHT by the KAT (exit $RED1)";
else echo "RED 1 FAIL: KAT did not catch an always-accept verifier -> it is a no-op"; rc=1; fi
if [ "$RED2" -ne 0 ]; then echo "RED 2 ok  : a faulted loop in the PROMOTED crypto/pbkdf2.c is CAUGHT (exit $RED2)";
else
  echo "RED 2 FAIL: KAT did not catch a faulted iteration loop in crypto/pbkdf2.c."
  echo "            That means this oracle is no longer covering the promoted KDF,"
  echo "            which is the precise failure #secretuse added RED 2 to prevent."
  rc=1
fi
echo "------------------------------------------------------------"
[ "$rc" -eq 0 ] && echo "pbkdf2_shadow_kat.sh: PASS" || echo "pbkdf2_shadow_kat.sh: FAIL"
exit $rc
