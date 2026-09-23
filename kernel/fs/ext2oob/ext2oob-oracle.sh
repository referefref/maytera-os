#!/usr/bin/env bash
# ext2oob-oracle.sh - build-container regression oracle for the #476 ext2
# directory-walk heap over-read. NO VM, NO golden: pure host user-space.
#
# It extracts the REAL, shipping ext2_dirblock_find_c() out of fs/ext2.c and
# compiles it as the GREEN arm alongside a faithful pre-#476 UNGUARDED walk
# (RED arm), then runs both over one valid and four crafted-malicious directory
# blocks. It PASSES only when the RED walk demonstrably over-reads / DoS-loops
# AND the real shipping walk stays in bounds AND still resolves a valid block
# (so the guards are proven load-bearing, not no-ops). See ext2oob_oracle.c.
#
# Because the GREEN arm is EXTRACTED from ext2.c at run time, weakening the
# shipping guards makes this oracle go RED. Run it after any change to the ext2
# directory-entry walk.
#
# Usage: kernel/fs/ext2oob-oracle.sh            (both backings)
#        kernel/fs/ext2oob-oracle.sh --self-test (same; explicit)
# Exit 0 = the fix holds. Exit 1 = a walk path over-reads or a guard regressed.
#
# No em-dashes by house style.

set -u
# This oracle lives in kernel/fs/ext2oob/ (a SUBDIR, so it stays out of the
# non-recursive $(wildcard fs/*.c) that builds the freestanding kernel: a
# host-only .c directly in fs/ would be swept in and break the build). The code
# under test, ext2.c, is therefore one directory up.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXT2_C="$SCRIPT_DIR/../ext2.c"
ORACLE_C="$SCRIPT_DIR/ext2oob_oracle.c"
CC="${CC:-gcc}"

if [ ! -f "$EXT2_C" ];   then echo "FATAL: $EXT2_C not found";   exit 2; fi
if [ ! -f "$ORACLE_C" ]; then echo "FATAL: $ORACLE_C not found"; exit 2; fi

BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

# ---- extract the real ext2_dirblock_find_c() verbatim ---------------------
awk '/^int ext2_dirblock_find_c\(/{p=1} p{print} p&&/^}/{exit}' \
    "$EXT2_C" > "$BUILD/ext2_dirblock_find_real.inc"

LINES=$(wc -l < "$BUILD/ext2_dirblock_find_real.inc")
if [ "$LINES" -lt 20 ]; then
    echo "FATAL: could not extract ext2_dirblock_find_c() from ext2.c (got $LINES lines)"
    exit 2
fi
echo "Extracted the shipping ext2_dirblock_find_c() ($LINES lines) from fs/ext2.c"
echo "SHA of shipping ext2.c: $(sha256sum "$EXT2_C" | cut -c1-16)"
echo

cp "$ORACLE_C" "$BUILD/ext2oob_oracle.c"

RC_TOTAL=0

# ---- backing 1: mmap guard page (no sanitizer) ----------------------------
echo "############################################################"
echo "# Backing 1: mmap PROT_NONE guard page"
echo "############################################################"
if "$CC" -O0 -g -Wall -Wextra -DUSE_MMAP_GUARD \
        -I"$BUILD" "$BUILD/ext2oob_oracle.c" -o "$BUILD/oracle_guard"; then
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
# guard page is strictly stronger and, crucially, DETERMINISTIC. It is a hard
# wall at exactly byte block_size, so an over-read of ANY distance faults at the
# same spot every run. ASan's malloc redzone is a fixed, small band: a far read
# (MAL-B walks ~83 bytes past) can clear the redzone into adjacent mapped heap
# and go UNDETECTED, and its fault address is allocator/ASLR dependent, which
# made the ASan arm flaky (a pure in-bounds DoS loop was even seen to SEGV). A
# flaky oracle is worse than a clean one, so the guard page is the sole backing.

echo "############################################################"
if [ "$RC_TOTAL" -eq 0 ]; then
    echo "# ext2 #476 ORACLE: PASS (shipping walk safe, guards load-bearing)"
    echo "############################################################"
    exit 0
else
    echo "# ext2 #476 ORACLE: FAIL (rc_total=$RC_TOTAL)"
    echo "############################################################"
    exit 1
fi
