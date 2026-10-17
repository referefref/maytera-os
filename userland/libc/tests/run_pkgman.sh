#!/bin/bash
# run_pkgman.sh - battery for userland/libc/pkgman.c, the .mpkg INSTALL manifest
# resolver that gained TREE (directory prefix) rules.
#
# FIVE ARMS. All must behave as stated or this script exits non-zero:
#
#   1 NEGATIVE CONTROL  resolves the same hostile vectors with a naive prefix
#                       match plus concatenation and REQUIRES them to ESCAPE the
#                       destination tree, judged by an independent path resolver
#                       in the test. A vector that does not escape the naive
#                       version is not hostile and proves nothing.
#   2 POSITIVE          the real pkgman_dest_all(): hostile refused, benign
#                       mapped exactly, fan-out honoured, over-wide fan-out
#                       refused rather than truncated, every malformed rule
#                       named with its line number.
#   3 CHAINED           pkgman_dest_all() then pkgdest_confine(), the order the
#                       App Store actually uses.
#   4 CORPUS            the real INSTALL manifest and full member list of all 24
#                       packages published on the repo host, with the OLD
#                       exact-match resolver embedded as the oracle: every
#                       already-published package must resolve byte-identically.
#   5 MIN-CLIENT        the "#!mpkg-min-client <n>" declaration: read correctly
#                       when it is well formed, and REFUSED (never read as "no
#                       requirement") when it is not. "I could not read the
#                       requirement" and "there is no requirement" lead to
#                       opposite actions, so they must not share an answer.
#
# The units under test are compiled freestanding with the same flags
# userland/libc/Makefile uses, so what is tested is the shipping translation
# unit and not a host-libc-flavoured variant. The harness itself is an ordinary
# host program.
set -u

cd "$(dirname "$0")" || exit 1
LIBC=..
GCCINC=$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/include 2>/dev/null | tail -1)
[ -n "$GCCINC" ] || { echo "no gcc include dir found"; exit 1; }

# Mirrors userland/libc/Makefile's CFLAGS for the units under test.
UUT_FLAGS="-m64 -ffreestanding -fno-builtin -nostdinc -fno-stack-protector \
           -mno-red-zone -Wall -Wextra -Werror -O2 -g -isystem $GCCINC -I$LIBC"
HARNESS_FLAGS="-m64 -Wall -Wextra -O1 -g -I$LIBC"

CORPUS=fixtures/mpkg-install-corpus.txt
[ -f "$CORPUS" ] || { echo "FAIL: missing $CORPUS"; exit 1; }

rc=0
d=$(mktemp -d) || exit 2
trap 'rm -rf "$d"' EXIT

gcc $UUT_FLAGS -c "$LIBC/pkgman.c"  -o "$d/pkgman.o"  || {
    echo "FAIL: pkgman.c does not compile with the real libc flags"; exit 1; }
gcc $UUT_FLAGS -c "$LIBC/pkgdest.c" -o "$d/pkgdest.o" || {
    echo "FAIL: pkgdest.c does not compile with the real libc flags"; exit 1; }

echo "=== arm 1: NEGATIVE CONTROL ==="
gcc $HARNESS_FLAGS -DPKGMAN_NAIVE -c pkgman_test.c -o "$d/t_naive.o" || exit 2
gcc "$d/t_naive.o" "$d/pkgman.o" "$d/pkgdest.o" -o "$d/naive" || exit 2
"$d/naive" "$CORPUS"; n=$?
if [ $n -ne 0 ]; then echo "NEGATIVE ARM FAILED: the vectors do not demonstrate an escape"; rc=1; fi

echo
echo "=== arms 2-5: POSITIVE, CHAINED, CORPUS, MIN-CLIENT ==="
gcc $HARNESS_FLAGS -c pkgman_test.c -o "$d/t_real.o" || exit 2
gcc "$d/t_real.o" "$d/pkgman.o" "$d/pkgdest.o" -o "$d/real" || exit 2
"$d/real" "$CORPUS"; p=$?
if [ $p -ne 0 ]; then echo "POSITIVE ARMS FAILED"; rc=1; fi

echo
if [ $rc -eq 0 ]; then echo "run_pkgman.sh: PASS (all five arms)"; else echo "run_pkgman.sh: FAIL"; fi
exit $rc
