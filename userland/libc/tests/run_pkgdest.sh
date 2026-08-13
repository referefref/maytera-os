#!/bin/bash
# run_pkgdest.sh - #745 containment battery for userland/libc/pkgdest.c.
#
# TWO ARMS. Both must behave as stated or this script exits non-zero:
#
#   NEGATIVE CONTROL  builds the same table against a NAIVE confinement (plain
#                     prefix concatenation, no canonicalization) and REQUIRES
#                     every ".."-bearing vector to ESCAPE the sandbox, judged
#                     by an independent path resolver in the test, not by
#                     pkgdest.c. If a vector does not escape the naive join it
#                     is not hostile and proves nothing, so that is a failure.
#
#   POSITIVE          builds the real ../pkgdest.c and requires every hostile
#                     vector REFUSED, every benign vector rewritten to exactly
#                     the expected path, and every root-session ("/") vector
#                     left byte-identical.
#
# The unit under test is compiled freestanding, with the same flags
# userland/libc/Makefile uses, so what is tested is the shipping translation
# unit and not a host-libc-flavoured variant of it. The harness itself is an
# ordinary host program.
set -u

cd "$(dirname "$0")" || exit 1
LIBC=..
GCCINC=$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/include 2>/dev/null | tail -1)
[ -n "$GCCINC" ] || { echo "no gcc include dir found"; exit 1; }

# Mirrors userland/libc/Makefile's CFLAGS for the unit under test.
UUT_FLAGS="-m64 -ffreestanding -fno-builtin -nostdinc -fno-stack-protector \
           -mno-red-zone -Wall -Wextra -Werror -O2 -g -isystem $GCCINC -I$LIBC"
HARNESS_FLAGS="-m64 -Wall -Wextra -O1 -g -I$LIBC"

rc=0
d=$(mktemp -d) || exit 2
trap 'rm -rf "$d"' EXIT

gcc $UUT_FLAGS -c "$LIBC/pkgdest.c" -o "$d/pkgdest.o" || {
    echo "FAIL: pkgdest.c does not compile with the real libc flags"; exit 1; }

echo "--- arm 1/2: NEGATIVE CONTROL (naive prefix join must escape) ---"
gcc $HARNESS_FLAGS -DPKGDEST_NAIVE -c pkgdest_test.c -o "$d/t_naive.o" || exit 2
gcc "$d/t_naive.o" "$d/pkgdest.o" -o "$d/naive" || exit 2
"$d/naive"; n=$?
if [ $n -ne 0 ]; then echo "NEGATIVE ARM FAILED: the vectors do not demonstrate an escape"; rc=1; fi

echo
echo "--- arm 2/2: POSITIVE (pkgdest_confine) ---"
gcc $HARNESS_FLAGS -c pkgdest_test.c -o "$d/t_real.o" || exit 2
gcc "$d/t_real.o" "$d/pkgdest.o" -o "$d/real" || exit 2
"$d/real"; p=$?
if [ $p -ne 0 ]; then echo "POSITIVE ARM FAILED"; rc=1; fi

echo
if [ $rc -eq 0 ]; then echo "run_pkgdest.sh: PASS (both arms)"; else echo "run_pkgdest.sh: FAIL"; fi
exit $rc
