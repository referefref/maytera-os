#!/usr/bin/env bash
# ports/libsodium/build.sh - the build=script step for the libsodium port.
#
# WHY build=script AND NOT build=objects (docs/MPORTS.md). Two structural reasons
# a flat sources= list cannot express:
#
#  1. GENERATED HEADER. sodium/version.h does not exist in the tarball; it is
#     produced from sodium/version.h.in by ./configure. We substitute the four
#     @...@ tokens as DATA below (version 1.0.20; SONAME 26.2 from configure.ac)
#     instead of running the autotools.
#
#  2. PER-FILE ARCH FLAGS AVOIDED BY CONSTRUCTION. libsodium ships ~19 SIMD
#     variants (*_avx2/avx512/sse2/ssse3/sse41/aesni/armcrypto) that upstream
#     compiles with per-file -mavx2 etc. behind runtime cpuid. We build the
#     PORTABLE reference implementation only: every one of those files is wholly
#     wrapped in `#if defined(HAVE_AVX2INTRIN_H) && ...`, and we define NONE of
#     those HAVE_*INTRIN_H macros, so each compiles to an empty translation unit.
#     We also skip them explicitly. The result is ONE uniform C build with no
#     per-file flags: correctness over speed for the first port, exactly as the
#     task scoped it. crypto_secretbox (xsalsa20poly1305), crypto_box
#     (curve25519xsalsa20poly1305) and crypto_sign (ed25519 ref10) are all
#     portable C and unaffected.
#
# We also skip randombytes/internal/ entirely: it is a Linux-getrandom/salsa20
# alternative RNG (it #includes <sys/syscall.h>), and this port ships exactly
# ONE randombytes backend, the kernel CSPRNG, per the never-a-second-RNG rule.
#
# THE RNG IS WIRED TO OUR KERNEL CSPRNG. Upstream's sysrandom TU is NOT compiled;
# ports/libsodium/randombytes_maytera.c is compiled in its place, keeping the
# public symbol randombytes_sysrandom_implementation but sourcing bytes from
# getrandom() (SYS_GETRANDOM -> kernel crypto/csprng.c). So the DEFAULT libsodium
# RNG is the kernel CSPRNG with no randombytes_set_implementation() needed.
#
# Contract from mports.sh: cwd = unpacked srcdir; MPORTS_CFLAGS = userland cross
# flags + this port's cflags=; MPORTS_SRC = srcdir; MPORTS_OUT = install tree.
# After we return mports looks for the archive at $(dirname MPORTS_SRC)/<lib> and
# proves every promised symbol is defined in it.
set -euo pipefail

: "${MPORTS_CFLAGS:?mports must pass MPORTS_CFLAGS}"
: "${MPORTS_SRC:?mports must pass MPORTS_SRC}"
: "${MPORTS_OUT:?mports must pass MPORTS_OUT}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKDIR="$(dirname "$MPORTS_SRC")"   # where mports expects libsodium.a
LIB="$WORKDIR/libsodium.a"
INC="src/libsodium/include"
CF="$MPORTS_CFLAGS -I$INC -I$INC/sodium"

echo "libsodium/build.sh: generating sodium/version.h from version.h.in"
sed -e 's/@VERSION@/1.0.20/' \
    -e 's/@SODIUM_LIBRARY_VERSION_MAJOR@/26/' \
    -e 's/@SODIUM_LIBRARY_VERSION_MINOR@/2/' \
    -e 's/@SODIUM_LIBRARY_MINIMAL_DEF@//' \
    "$INC/sodium/version.h.in" > "$INC/sodium/version.h"

echo "libsodium/build.sh: installing MayteraOS randombytes backend (getrandom -> kernel CSPRNG)"
cp "$SCRIPT_DIR/randombytes_maytera.c" src/libsodium/randombytes/randombytes_maytera.c

echo "libsodium/build.sh: compiling the portable reference set (SIMD variants skipped)"
OBJS=()
while IFS= read -r f; do
  case "$f" in
    */sysrandom/randombytes_sysrandom.c) continue ;;   # replaced by our backend
    */randombytes/internal/*) continue ;;              # Linux-syscall alternative RNG; we ship exactly one backend (the kernel CSPRNG)
    *avx2*|*avx512*|*sse2*|*ssse3*|*sse41*|*aesni*|*armcrypto*) continue ;;
  esac
  o="${f%.c}.o"
  gcc $CF -c "$f" -o "$o"
  OBJS+=("$o")
done < <(find src/libsodium -name '*.c' | sort)

gcc $CF -c src/libsodium/randombytes/randombytes_maytera.c \
    -o src/libsodium/randombytes/randombytes_maytera.o
OBJS+=(src/libsodium/randombytes/randombytes_maytera.o)

echo "libsodium/build.sh: archiving ${#OBJS[@]} objects into libsodium.a"
rm -f "$LIB"
ar rcs "$LIB" "${OBJS[@]}"

echo "libsodium/build.sh: installing nested public headers to out/include/sodium"
mkdir -p "$MPORTS_OUT/include/sodium"
cp -R "$INC/sodium/." "$MPORTS_OUT/include/sodium/"
rm -f "$MPORTS_OUT/include/sodium/version.h.in"

echo "libsodium/build.sh: done ($(ar t "$LIB" | wc -l) members)"
