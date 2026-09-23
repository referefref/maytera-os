#!/usr/bin/env bash
# imgoob-oracle.sh - build-container regression oracle for out-of-bounds reads
# in the BMP image decoder (kernel/gui/image.c, bmp_decode_c). NO VM, NO golden:
# pure host user-space.
#
# It extracts the REAL, shipping bmp_decode_c() out of kernel/gui/image.c and
# compiles it as the GREEN arm alongside a faithful pre-hardening UNGUARDED BMP
# decode (RED arm), then runs both over one valid and four crafted-malicious
# BMP files (pixel-data offset past EOF, truncated pixel data, dimension
# overflow, absurd height). It PASSES only when the RED decode demonstrably
# over-reads the source buffer on every crafted BMP AND the real shipping
# decoder stays in bounds AND rejects every one AND still decodes the valid BMP
# to the exact expected pixels (so the bounds/overflow guards are proven
# load-bearing, not no-ops). See imgoob_oracle.c.
#
# Because the GREEN arm is EXTRACTED from image.c at run time, weakening the
# shipping guards makes this oracle go RED. Run it after any change to the BMP
# decoder or its bounds/overflow checks.
#
# Usage: kernel/gui/imgoob/imgoob-oracle.sh              (guard-page backing)
#        kernel/gui/imgoob/imgoob-oracle.sh --self-test  (same; explicit)
# Exit 0 = the guards hold. Exit 1 = a decode path over-reads or a guard regressed.
#
# No em-dashes by house style.

set -u
# This oracle lives in kernel/gui/imgoob/ (a SUBDIR, so it stays out of the
# non-recursive $(wildcard gui/*.c) that builds the freestanding kernel: a
# host-only .c directly in gui/ would be swept in and break the build). The code
# under test, image.c, is therefore one directory up.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_C="$SCRIPT_DIR/../image.c"
ORACLE_C="$SCRIPT_DIR/imgoob_oracle.c"
CC="${CC:-gcc}"

if [ ! -f "$IMAGE_C" ];  then echo "FATAL: $IMAGE_C not found";  exit 2; fi
if [ ! -f "$ORACLE_C" ]; then echo "FATAL: $ORACLE_C not found"; exit 2; fi

BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

# ---- extract the real bmp_decode_c() verbatim -----------------------------
# The signature spans three lines starting "int bmp_decode_c(", and the only
# column-0 "}" inside the function is its final closing brace (the for/if bodies
# close with indented braces), so the awk range is exact.
awk '/^int bmp_decode_c\(/{p=1} p{print} p&&/^}/{exit}' \
    "$IMAGE_C" > "$BUILD/bmp_decode_c_real.inc"

LINES=$(wc -l < "$BUILD/bmp_decode_c_real.inc")
if [ "$LINES" -lt 30 ]; then
    echo "FATAL: could not extract bmp_decode_c() from image.c (got $LINES lines)"
    exit 2
fi
echo "Extracted the shipping bmp_decode_c() ($LINES lines) from kernel/gui/image.c"
echo "SHA of shipping image.c: $(sha256sum "$IMAGE_C" | cut -c1-16)"
echo

cp "$ORACLE_C" "$BUILD/imgoob_oracle.c"

RC_TOTAL=0

echo "############################################################"
echo "# Backing: mmap PROT_NONE guard page (deterministic wall)"
echo "############################################################"
if "$CC" -O0 -g -Wall -Wextra \
        -I"$BUILD" "$BUILD/imgoob_oracle.c" -o "$BUILD/oracle_guard"; then
    "$BUILD/oracle_guard"
    RC1=$?
    echo "(guard-page arm exit=$RC1)"
    RC_TOTAL=$((RC_TOTAL + RC1))
else
    echo "FATAL: guard-page arm failed to compile"
    RC_TOTAL=$((RC_TOTAL + 1))
fi
echo

# NOTE on why a PROT_NONE guard page and not AddressSanitizer: for THIS test a
# guard page is strictly stronger and DETERMINISTIC. It is a hard wall at exactly
# byte len, so a source over-read of ANY distance faults at the same spot every
# run. ASan's malloc redzone is a fixed, small band a far read can clear into
# adjacent mapped heap undetected, and its fault address is allocator/ASLR
# dependent. The same reasoning retired the ASan arm in the ext2oob oracle.

echo "############################################################"
if [ "$RC_TOTAL" -eq 0 ]; then
    echo "# BMP over-read ORACLE: PASS (shipping decode safe, guards load-bearing)"
    echo "############################################################"
    exit 0
else
    echo "# BMP over-read ORACLE: FAIL (rc_total=$RC_TOTAL)"
    echo "############################################################"
    exit 1
fi
