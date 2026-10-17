#!/usr/bin/env bash
#
# sha256_twin_kat.sh - cross-check oracle for the TWO SHA-256 implementations
# that MayteraOS ships: kernel/crypto/sha256.c and userland/libc/sha256.c.
#
# WHY THIS FILE EXISTS INSTEAD OF A MERGE (backlog D1)
# ---------------------------------------------------
# D1 asked for the forked twin to be de-duplicated. It was MEASURED on
# 2026-10-01 that a single shared .c is the WORSE option here, and this oracle is
# what was built instead. The measurements, so nobody has to re-derive them:
#
#   1. The two files are not a copy-paste fork of each other's internals. After
#      stripping comments and whitespace, the K[64] round-constant table and all
#      seven bit-mixing macros (ROTR/CH/MAJ/EP0/EP1/SIG0/SIG1) are BYTE-IDENTICAL
#      between the two files. Every remaining difference in the shared core is
#      line wrapping, a redundant (size_t)/(uint64_t) cast, or one of exactly
#      three kernel-only couplings: the dlprof instrumentation inside
#      sha256_update (dp_tsc / g_dp_sha_bytes / g_dp_sha_cyc), crypto_zero vs
#      secure_zero in sha256_final, and the -DRUST_SHA256 strangler dispatcher
#      around the compression leaf. The kernel file's extra ~200 lines are extra
#      API SURFACE (HMAC-SHA256) and kernel-only plumbing (the Rust dispatcher
#      plus sha256_rust_selftest's 20003-vector boot differential), not different
#      internals and not dead code.
#
#   2. The kernel build container only ever receives the kernel tree:
#      build/build-golden.sh does `git archive $GIT_COMMIT kernel`, and
#      build/invariant-gate.sh does the same for its independent rebuild. A
#      shared file under userland/libc/ is therefore ABSENT from the kernel
#      build. The only possible shared home is kernel/crypto/, which means libc
#      compiling a file out of the kernel tree, and libc's Makefile is an
#      explicit SRCS list with a `%.o: %.c` rule that does not match a path
#      outside its own directory.
#
#   3. The decisive one. TWO existing oracles, pbkdf2_shadow_kat.sh and
#      secretuse_kat.sh, stage the REAL kernel/crypto/sha256.c by sed-ing exactly
#      its four #include lines and compiling it for the host. If the twin became
#      one file with #ifdef branches for kernel vs userland, those oracles would
#      compile the KERNEL branch only, so the userland branch would be covered by
#      NOTHING while the tree looked de-duplicated. That is backlog hazard A3
#      (a KAT that silently stops testing anything) arriving by invitation, and
#      it is strictly worse than two reviewed files with a cross-check.
#
# So the duplication stays, and it is made SAFE and VISIBLE instead: this oracle
# fails if the two implementations ever disagree on any digest, which is the
# property a merge was wanted for in the first place. It also runs
# libc_sha256_selftest(), which until now had ZERO CALLERS anywhere in the tree
# despite userland/libc/sha256.h claiming it is what catches a divergence in
# userland. It was dead. Now it runs on every land.
#
# WHAT IT PROVES
# --------------
#   GREEN : (a) both implementations reproduce the published FIPS 180-4 digests.
#               A pure differential could not establish this: the two arms share
#               the K table byte-for-byte, so a shared bug would agree with
#               itself. The published-vector arm is the independent oracle.
#           (b) both implementations produce IDENTICAL digests for every message
#               length in the battery below, including the padding boundaries
#               55/56/63/64/65/119/120 and multi-block messages.
#           (c) the one-shot and the ragged-streaming update paths agree, within
#               each arm and across the two arms.
#           (d) the two sha256_ctx_t layouts are the same size (compared to each
#               other at runtime, not against a hardcoded number).
#           (e) libc_sha256_selftest() returns 0 and is actually linked.
#
#   RED   : four mutations, each of a COPY (never of the shipping source, so
#           neither file has to carry a deliberately-wrong path to serve a host
#           test; same reasoning as sha1_hmac_kat.sh), must EACH make this
#           oracle fail:
#             (a) one corrupted round constant, K[40], in the KERNEL arm
#             (b) one rotate amount off by one, EP0's ROTR(x,13) -> ROTR(x,12),
#                 in the KERNEL arm
#             (c) one corrupted round constant, K[40], in the USERLAND arm
#             (d) the padding boundary in the USERLAND arm: `buffer_used < 56`
#                 -> `buffer_used < 57`. (d) is the one that matters. (a) to (c)
#                 break nearly every digest, so any vector at all catches them.
#                 (d) is invisible on the empty string, on "abc", on 1, 55, 63,
#                 64, 65 bytes and on every multi-block vector; it is caught ONLY
#                 because the battery deliberately covers 56 and 120. Its much
#                 smaller failing-check count below is the evidence that those
#                 boundary lengths are load-bearing rather than decoration.
#           Every mutation is VERIFIED to have changed its file, so a renamed
#           constant can never silently turn a RED run into a second GREEN run
#           that trivially "fails to fail".
#
# WHAT IT DOES NOT PROVE, stated rather than implied
# --------------------------------------------------
# This oracle compiles the kernel arm WITHOUT -DRUST_SHA256, so the leaf it
# exercises on the kernel side is sha256_transform_c. The SHIPPING kernel is
# built WITH -DRUST_SHA256 (kernel/Makefile:175), and `nm crypto/sha256.o` on a
# real build shows `U sha256_transform_rs` and `U hmac_sha256_rs`: the kernel's
# live compression and its live HMAC are RUST, not the C below. So the chain that
# makes the userland twin trustworthy against what the kernel actually runs has
# TWO links, and this oracle is only one of them:
#
#   userland C leaf  ==  kernel C leaf      <- THIS oracle, at land time
#   kernel C leaf    ==  kernel Rust leaf   <- sha256_rust_selftest() at boot,
#                                              20000 random (state, block) pairs,
#                                              reported as [RUST-DIFF] sha256 on
#                                              serial and in /BOOTLOG.TXT
#
# If the boot [RUST-DIFF] line is absent or says FAIL, this oracle passing means
# nothing about the Rust path. Teaching this oracle to build the Rust leaf too
# would close that, and is not done here: it needs the rustc target plumbing the
# three Rust oracles already carry, and it is a separate change.
#
# It also does not cover HMAC-SHA256, which exists only on the kernel side and so
# has no twin to cross-check.
#
# WHERE IT RUNS: picked up automatically by crypto/run-all-kats.sh, which globs
# *_kat.sh across the whole kernel tree. That runner is invoked from exactly one
# place, tools/worktree/mwt (the pre-land gate), which runs in a full worktree
# where userland/ is present. It is NOT invoked inside the kernel-only build
# container. If the userland file is missing this oracle FAILS LOUD rather than
# skipping: an inert gate and no gate are indistinguishable from the outside.

set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
KROOT=$(cd "$HERE/.." && pwd)          # kernel/
REPO=$(cd "$KROOT/.." && pwd)          # repo root

K_SHA="$KROOT/crypto/sha256.c"
U_SHA="$REPO/userland/libc/sha256.c"
U_HDR="$REPO/userland/libc/sha256.h"

CC=${CC:-gcc}
command -v "$CC" >/dev/null 2>&1 || { echo "FATAL: no $CC on PATH"; exit 2; }

for f in "$K_SHA" "$U_SHA" "$U_HDR"; do
  if [ ! -f "$f" ]; then
    echo "FATAL: missing $f"
    echo "       This oracle cross-checks the kernel and userland SHA-256 twins,"
    echo "       so it needs BOTH trees. It is reached from tools/worktree/mwt in a"
    echo "       full worktree. If you are running it inside the kernel-only build"
    echo "       container (git archive <commit> kernel), that is the bug, not this."
    exit 2
  fi
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------------------
# Shims. TWO files, and the split is load-bearing: both kernel/crypto/crypto.h
# and userland/libc/sha256.h define their own `typedef struct {...} sha256_ctx_t`,
# which are distinct anonymous struct types. A single shim carrying the kernel
# typedef, included alongside the REAL userland header, is a typedef redefinition
# and does not compile. So shimtypes.h carries only what BOTH arms need (the
# integer typedefs and the libc declarations) and shim.h adds the kernel-only
# crypto surface on top. Everything that decides a DIGEST comes from the real
# staged sources either way.
# ---------------------------------------------------------------------------
cat > "$WORK/shimtypes.h" <<'EOF'
#ifndef SHA256_TWIN_KAT_SHIMTYPES_H
#define SHA256_TWIN_KAT_SHIMTYPES_H
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned long      size_t;

/* libc the staged sources call, resolved against the system libc */
void  *memcpy(void *, const void *, size_t);
void  *memset(void *, int, size_t);
int    memcmp(const void *, const void *, size_t);
void   secure_zero(void *, size_t);
#endif
EOF

cat > "$WORK/shim.h" <<'EOF'
#ifndef SHA256_TWIN_KAT_SHIM_H
#define SHA256_TWIN_KAT_SHIM_H
#include "shimtypes.h"

#define SHA256_BLOCK_SIZE   64
#define SHA256_DIGEST_SIZE  32

/* kernel/crypto/crypto.h */
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

/* kernel/cpu/dlprof.h (measurement profiler; inert in the host oracle) */
static inline uint64_t dp_tsc(void) { return 0; }
static volatile uint64_t g_dp_sha_cyc, g_dp_sha_bytes;

int    kprintf(const char *, ...);
void   bootlog_write(const char *, ...);
#endif
EOF

# ---------------------------------------------------------------------------
# Stage the KERNEL arm: the REAL crypto/sha256.c, only its #include lines
# changed. Every hashing function is byte-identical to what ships.
# ---------------------------------------------------------------------------
stage_kernel() {   # $1 = destination .c
  sed -E \
    -e 's|^#include "crypto.h"|#include "shim.h"|' \
    -e 's|^#include "\.\./string\.h".*||' \
    -e 's|^#include "\.\./cpu/dlprof\.h".*||' \
    -e 's|^#include "fs/bootlog\.h".*||' \
    "$K_SHA" > "$1"
  # Export the ctx size so the driver can compare the two layouts to each other.
  echo 'unsigned long k_ctx_size(void) { return sizeof(sha256_ctx_t); }' >> "$1"
}

# ---------------------------------------------------------------------------
# Stage the USERLAND arm: the REAL userland/libc/sha256.c and its REAL header,
# with the userland includes pointed at the shim. Its externally visible symbols
# are renamed to u_* on the command line so both arms link into one binary; the
# preprocessor only rewrites whole identifiers, so -Dsha256=u_sha256 does not
# touch sha256_init / sha256_update / sha256_final.
# ---------------------------------------------------------------------------
U_RENAME='-Dsha256_init=u_sha256_init -Dsha256_update=u_sha256_update
          -Dsha256_final=u_sha256_final -Dsha256=u_sha256
          -Dlibc_sha256_selftest=u_libc_sha256_selftest'

stage_user() {     # $1 = destination .c
  sed -E 's|^#include "types\.h"|#include "shimtypes.h"|' "$U_HDR" > "$WORK/u_sha256.h"
  grep -q 'sha256_ctx_t' "$WORK/u_sha256.h" || {
    echo "FATAL: staged userland sha256.h has no sha256_ctx_t (did the header move?)"; exit 2; }
  sed -E \
    -e 's|^#include "sha256\.h"|#include "u_sha256.h"|' \
    -e 's|^#include "string\.h".*||' \
    "$U_SHA" > "$1"
  echo 'unsigned long u_ctx_size(void) { return sizeof(sha256_ctx_t); }' >> "$1"
}

# Sanity: the staging must have actually found the includes it claims to rewrite,
# or a silently-unrewritten include would surface as a confusing compile error
# and, worse, a future include could slip through unnoticed.
stage_kernel "$WORK/k_sha256.c"
stage_user   "$WORK/u_sha256.c"
if grep -qE '^#include "(crypto|\.\./string|\.\./cpu/dlprof|fs/bootlog)' "$WORK/k_sha256.c"; then
  echo "FATAL: kernel arm still has an unrewritten kernel include:"
  grep -nE '^#include' "$WORK/k_sha256.c"; exit 2
fi
if grep -qE '^#include "(sha256|string|types)\.h"' "$WORK/u_sha256.c"; then
  echo "FATAL: userland arm still has an unrewritten userland include:"
  grep -nE '^#include' "$WORK/u_sha256.c"; exit 2
fi
# The userland arm must still DEFINE the selftest, or check (e) below is vacuous.
grep -q 'libc_sha256_selftest' "$WORK/u_sha256.c" || {
  echo "FATAL: the userland arm no longer defines libc_sha256_selftest, so this"
  echo "       oracle would not be running it at all."; exit 2; }

cp "$WORK/k_sha256.c" "$WORK/k_sha256.orig.c"
cp "$WORK/u_sha256.c" "$WORK/u_sha256.orig.c"

# ---------------------------------------------------------------------------
# Driver: the published-vector arm, the cross-arm differential, the streaming
# cross-check and the layout comparison. Exit status is the failing-check count.
# ---------------------------------------------------------------------------
cat > "$WORK/driver.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* Symbols the staged kernel arm references but that live outside the hash path. */
void crypto_zero(void *p, unsigned long n) { volatile unsigned char *q = p; while (n--) *q++ = 0; }
void secure_zero(void *p, unsigned long n) { volatile unsigned char *q = p; while (n--) *q++ = 0; }
void sha256_transform_rs(unsigned int *s, const unsigned char *b) { (void)s; (void)b; }
int  kprintf(const char *f, ...) { (void)f; return 0; }
void bootlog_write(const char *f, ...) { (void)f; }

/* Both arms. The layouts are compared at runtime rather than assumed. */
typedef struct { unsigned int state[8]; unsigned long long count; unsigned char buffer[64]; } ctx_t;

void sha256_init(ctx_t *);
void sha256_update(ctx_t *, const void *, unsigned long);
void sha256_final(ctx_t *, unsigned char *);
void sha256(const void *, unsigned long, unsigned char *);
unsigned long k_ctx_size(void);

void u_sha256_init(ctx_t *);
void u_sha256_update(ctx_t *, const void *, unsigned long);
void u_sha256_final(ctx_t *, unsigned char *);
void u_sha256(const void *, unsigned long, unsigned char *);
int  u_libc_sha256_selftest(void);
unsigned long u_ctx_size(void);

static int fails = 0, checks = 0;

static void hex(const unsigned char *d, char *out) {
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
    out[64] = 0;
}
static void bad(const char *what, const char *detail) {
    fails++;
    printf("  FAIL %-34s %s\n", what, detail);
}

/* ---- (a) published FIPS 180-4 vectors, both arms independently ------------
 * A differential cannot establish correctness when both arms share the K table
 * byte-for-byte, so this is the independent oracle, not decoration.          */
struct pub { const char *msg; unsigned long len; const char *hex; const char *name; };
static const struct pub PUB[] = {
    { "", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "FIPS empty" },
    { "abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "FIPS abc" },
    { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "FIPS 448-bit (56B)" },
    { "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", 112,
      "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1", "FIPS 896-bit (112B)" },
};

/* ---- (b) the length battery. Padding boundaries are load-bearing: 56 and 120
 * are the only lengths that catch a `< 56` -> `< 57` boundary bug.           */
static const unsigned long LENS[] = {
    0, 1, 2, 3, 54, 55, 56, 57, 63, 64, 65, 119, 120, 121, 127, 128, 129,
    191, 192, 255, 256, 1000, 4096, 12800
};

int main(void) {
    char h1[72], h2[72], detail[256];

    /* (d) layout comparison, each arm reporting its own sizeof. */
    checks++;
    if (k_ctx_size() != u_ctx_size()) {
        snprintf(detail, sizeof detail, "kernel sizeof(ctx)=%lu userland=%lu",
                 k_ctx_size(), u_ctx_size());
        bad("ctx layout", detail);
    }
    checks++;
    if (k_ctx_size() != sizeof(ctx_t)) {
        snprintf(detail, sizeof detail, "arm sizeof(ctx)=%lu driver=%lu",
                 k_ctx_size(), (unsigned long)sizeof(ctx_t));
        bad("ctx layout vs driver", detail);
    }

    /* (a) published vectors, both arms. */
    for (unsigned i = 0; i < sizeof PUB / sizeof PUB[0]; i++) {
        unsigned char dk[32], du[32];
        sha256(PUB[i].msg, PUB[i].len, dk);
        u_sha256(PUB[i].msg, PUB[i].len, du);
        checks++; hex(dk, h1);
        if (strcmp(h1, PUB[i].hex)) {
            snprintf(detail, sizeof detail, "%s: kernel got %s want %s", PUB[i].name, h1, PUB[i].hex);
            bad("published vector (kernel)", detail);
        }
        checks++; hex(du, h2);
        if (strcmp(h2, PUB[i].hex)) {
            snprintf(detail, sizeof detail, "%s: userland got %s want %s", PUB[i].name, h2, PUB[i].hex);
            bad("published vector (userland)", detail);
        }
    }

    /* (b) + (c) the battery: cross-arm one-shot equality, and for each arm the
     * ragged-streaming path against its own one-shot, then across the arms.   */
    static unsigned char buf[12800];
    for (unsigned long i = 0; i < sizeof buf; i++) buf[i] = (unsigned char)(i * 31 + (i >> 5));

    for (unsigned i = 0; i < sizeof LENS / sizeof LENS[0]; i++) {
        unsigned long n = LENS[i];
        unsigned char dk[32], du[32], sk[32], su[32];

        sha256(buf, n, dk);
        u_sha256(buf, n, du);
        checks++;
        if (memcmp(dk, du, 32)) {
            hex(dk, h1); hex(du, h2);
            snprintf(detail, sizeof detail, "len=%lu kernel=%s userland=%s", n, h1, h2);
            bad("cross-arm one-shot", detail);
        }

        /* ragged chunk sizes, so buffer_used != 0 and the whole-block loop are
         * both exercised, and the two arms are fed the identical chunking.    */
        ctx_t ck, cu;
        sha256_init(&ck); u_sha256_init(&cu);
        unsigned long off = 0, step = 1;
        while (off < n) {
            unsigned long c = step; if (off + c > n) c = n - off;
            sha256_update(&ck, buf + off, c);
            u_sha256_update(&cu, buf + off, c);
            off += c; step = (step * 7 + 13) % 300 + 1;
        }
        sha256_final(&ck, sk); u_sha256_final(&cu, su);

        checks++;
        if (memcmp(sk, dk, 32)) {
            snprintf(detail, sizeof detail, "len=%lu kernel streamed != kernel one-shot", n);
            bad("streaming (kernel)", detail);
        }
        checks++;
        if (memcmp(su, du, 32)) {
            snprintf(detail, sizeof detail, "len=%lu userland streamed != userland one-shot", n);
            bad("streaming (userland)", detail);
        }
        checks++;
        if (memcmp(sk, su, 32)) {
            snprintf(detail, sizeof detail, "len=%lu streamed digests differ across arms", n);
            bad("cross-arm streaming", detail);
        }
    }

    /* (e) libc_sha256_selftest: it is LINKED here and it RUNS here. Until this
     * oracle existed it had zero callers anywhere in the tree.               */
    checks++;
    int st = u_libc_sha256_selftest();
    if (st != 0) {
        snprintf(detail, sizeof detail, "returned %d, expected 0", st);
        bad("libc_sha256_selftest()", detail);
    }

    printf("  %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
EOF

# The arms are compiled in SEPARATE gcc invocations and then linked. They cannot
# share one command line: -D applies to the whole compilation regardless of where
# it sits among the inputs, so a single invocation renames the KERNEL arm's
# symbols to u_* as well and the link fails with "multiple definition of
# u_sha256_init". Measured, not guessed: that was the first attempt.
CFLAGS_KAT="-O2 -Wall -Wno-array-parameter -Wno-unused-function -Wno-unused-variable"

build_and_run() {  # $1 = label, $2 = kernel arm .c, $3 = userland arm .c
  local label="$1" karm="$2" uarm="$3" bin="$WORK/kat_$1" log="$WORK/cc_$1.log"
  : > "$log"
  if ! "$CC" $CFLAGS_KAT -I "$WORK" -c "$karm"          -o "$WORK/k_$label.o" 2>>"$log" ||
     ! "$CC" $CFLAGS_KAT -I "$WORK" $U_RENAME -c "$uarm" -o "$WORK/u_$label.o" 2>>"$log" ||
     ! "$CC" $CFLAGS_KAT -I "$WORK" -c "$WORK/driver.c"  -o "$WORK/d_$label.o" 2>>"$log" ||
     ! "$CC" -o "$bin" "$WORK/k_$label.o" "$WORK/u_$label.o" "$WORK/d_$label.o" 2>>"$log"; then
    echo "FATAL: compile/link failed for $label"; cat "$log"; exit 2
  fi
  "$bin"; return $?
}

# A mutation that did not change its file would silently turn a RED run into a
# second GREEN run, so every one is verified to have altered the bytes.
mutate() {  # $1 = file, $2 = sed expr, $3 = description
  local f="$1" before after
  before=$(md5sum "$f" | cut -d' ' -f1)
  sed -i -E "$2" "$f"
  after=$(md5sum "$f" | cut -d' ' -f1)
  if [ "$before" = "$after" ]; then
    echo "FATAL: mutation '$3' changed nothing in $f."
    echo "       The RED arm would be a second GREEN run. Fix the pattern."
    exit 2
  fi
}

echo "== GREEN: real kernel/crypto/sha256.c vs real userland/libc/sha256.c =="
build_and_run green "$WORK/k_sha256.orig.c" "$WORK/u_sha256.orig.c"
GREEN=$?

echo "== RED 1: KERNEL arm, one corrupted round constant K[40] (0xa2bfe8a1) =="
cp "$WORK/k_sha256.orig.c" "$WORK/k_red1.c"
mutate "$WORK/k_red1.c" 's/0xa2bfe8a1/0xa2bfe8a2/' 'K[40] corrupt (kernel)'
build_and_run red1 "$WORK/k_red1.c" "$WORK/u_sha256.orig.c"
RED1=$?

echo "== RED 2: KERNEL arm, EP0 rotate off by one (ROTR(x, 13) -> ROTR(x, 12)) =="
cp "$WORK/k_sha256.orig.c" "$WORK/k_red2.c"
# ROTR(x, 13) appears exactly once in the file: EP0. (EP1 uses 6/11/25, SIG0
# uses 7/18/3, SIG1 uses 17/19/10.) So this one substitution is EP0's rotate.
mutate "$WORK/k_red2.c" 's/ROTR\(x, 13\)/ROTR(x, 12)/' 'EP0 rotate off by one (kernel)'
build_and_run red2 "$WORK/k_red2.c" "$WORK/u_sha256.orig.c"
RED2=$?

echo "== RED 3: USERLAND arm, one corrupted round constant K[40] (0xa2bfe8a1) =="
cp "$WORK/u_sha256.orig.c" "$WORK/u_red3.c"
mutate "$WORK/u_red3.c" 's/0xa2bfe8a1/0xa2bfe8a2/' 'K[40] corrupt (userland)'
build_and_run red3 "$WORK/k_sha256.orig.c" "$WORK/u_red3.c"
RED3=$?

echo "== RED 4: USERLAND arm, padding boundary (buffer_used < 56) -> (< 57) =="
cp "$WORK/u_sha256.orig.c" "$WORK/u_red4.c"
mutate "$WORK/u_red4.c" 's/\(buffer_used < 56\)/(buffer_used < 57)/' 'padding boundary (userland)'
build_and_run red4 "$WORK/k_sha256.orig.c" "$WORK/u_red4.c"
RED4=$?

echo "------------------------------------------------------------"
rc=0
if [ "$GREEN" -eq 0 ]; then
  echo "GREEN ok  : both implementations match the published FIPS vectors AND each"
  echo "            other on every length in the battery (exit 0)"
else
  echo "GREEN FAIL: the two shipping SHA-256 implementations DISAGREE, or one of"
  echo "            them disagrees with FIPS 180-4 (exit $GREEN). This is the"
  echo "            divergence this oracle exists to catch. Read the FAIL lines."
  rc=1
fi
for pair in "1:$RED1:corrupted round constant in the KERNEL arm" \
            "2:$RED2:an off-by-one rotate in the KERNEL arm" \
            "3:$RED3:a corrupted round constant in the USERLAND arm" \
            "4:$RED4:a padding-boundary bug in the USERLAND arm"; do
  n=${pair%%:*}; rest=${pair#*:}; code=${rest%%:*}; what=${rest#*:}
  if [ "$code" -ne 0 ]; then
    echo "RED $n ok  : $what is CAUGHT (exit $code)"
  else
    echo "RED $n FAIL: this oracle did NOT catch $what, so it is a no-op."
    rc=1
  fi
done
echo "------------------------------------------------------------"
[ "$rc" -eq 0 ] && echo "sha256_twin_kat.sh: PASS" || echo "sha256_twin_kat.sh: FAIL"
exit $rc
