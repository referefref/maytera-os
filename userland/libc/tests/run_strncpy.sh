#!/bin/bash
# run_strncpy.sh - #231 regression test for the userland libc strncpy()/
# strncat() off-by-one (userland/libc/string.c).
#
# TWO ARMS. Both must behave as stated or this script exits non-zero:
#
#   NEGATIVE CONTROL  builds the same test against fixtures/string.c.prefix231,
#                     a frozen copy of the PRE-FIX string.c, and REQUIRES
#                     AddressSanitizer to report a stack-buffer-overflow. If
#                     the negative arm ever comes out clean, the test has
#                     stopped exercising the bug and proves nothing - exactly
#                     how #223's first two rounds mis-attributed this to path
#                     length instead of the shared strncpy() primitive.
#
#   POSITIVE          builds against the live ../string.c and requires a clean
#                     ASan run with zero test failures.
#
# The libc TU is compiled freestanding (-nostdinc against ../..) exactly as
# the real build does, with its public symbols renamed to mos_* so it can be
# linked next to glibc in this same host binary (see printf_float_test.c /
# run.sh for the precedent this follows).
set -u

cd "$(dirname "$0")" || exit 1
LIBC=..
GCCINC=$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/include 2>/dev/null | tail -1)
[ -n "$GCCINC" ] || { echo "no gcc include dir found"; exit 1; }

RENAME="-Dstrncpy=mos_strncpy -Dstrncat=mos_strncat"

# Freestanding flags mirroring userland/libc/Makefile, plus ASan.
# memcpy_fast/memset_fast/memmove_fast come from strncpy_test.c's own stub
# definitions (see that file), not from this compile, so no -D needed for
# them here - only the two functions under test are renamed.
UUT_FLAGS="-m64 -ffreestanding -fno-builtin -nostdinc -fno-stack-protector \
           -mno-red-zone -Wall -Wextra -O1 -g -fsanitize=address \
           -isystem $GCCINC -I$LIBC $RENAME"

build_and_run() {   # $1 = string.c source, $2 = output tag
    local src="$1" tag="$2" d
    d=$(mktemp -d) || return 2
    gcc $UUT_FLAGS -x c -c "$src" -o "$d/string.o" 2>"$d/cc1.log" || {
        echo "  compile of $src FAILED:"; sed -n 1,20p "$d/cc1.log"; rm -rf "$d"; return 2; }
    gcc -m64 -O1 -g -fsanitize=address -c strncpy_test.c -o "$d/t.o" \
        2>"$d/cc2.log" || {
        echo "  compile of harness FAILED:"; sed -n 1,20p "$d/cc2.log"; rm -rf "$d"; return 2; }
    gcc -m64 -fsanitize=address "$d/t.o" "$d/string.o" -o "$d/t" 2>"$d/ld.log" || {
        echo "  link FAILED:"; sed -n 1,20p "$d/ld.log"; rm -rf "$d"; return 2; }
    # ASan reserves a very large shadow mapping. On a loaded build host that
    # can fail at startup, which shows up as repeated "DEADLYSIGNAL" with no
    # test output at all. That is an environment flake, NOT a result, and must
    # never be reported as either arm passing or failing. Retry a few times and
    # return 3 if it never gets off the ground.
    local rc=0 try
    for try in 1 2 3; do
        ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 "$d/t" >"$d/$tag.out" 2>&1
        rc=$?
        if grep -q "#231 strncpy" "$d/$tag.out"; then break; fi
        if ! grep -q "DEADLYSIGNAL" "$d/$tag.out"; then break; fi
        rc=3
        sleep 2
    done
    cp "$d/$tag.out" "./$tag.out"
    rm -rf "$d"
    return $rc
}

rc_all=0

echo "=== ARM 1: NEGATIVE CONTROL (pre-fix string.c, must smash the stack) ==="
build_and_run fixtures/string.c.prefix231 negative
neg_rc=$?
if [ $neg_rc -eq 2 ]; then
    echo "RESULT: BROKEN - negative arm did not build"; rc_all=1
elif [ $neg_rc -eq 3 ]; then
    echo "RESULT: INCONCLUSIVE - ASan could not start (host memory pressure)"; rc_all=1
elif grep -q "stack-buffer-overflow" negative.out; then
    echo "RESULT: GOOD - ASan caught it on the pre-fix code:"
    grep -m3 -E "ERROR: AddressSanitizer|stack-buffer-overflow|#1 .*string" negative.out | sed 's/^/    /'
else
    echo "RESULT: BAD - pre-fix code ran CLEAN, so this test proves nothing."
    echo "        (exit $neg_rc; see negative.out)"
    rc_all=1
fi

echo
echo "=== ARM 2: POSITIVE (live ../string.c, must be clean) ==="
build_and_run "$LIBC/string.c" positive
pos_rc=$?
if [ $pos_rc -eq 2 ]; then
    echo "RESULT: FAIL - positive arm did not build"; rc_all=1
elif [ $pos_rc -eq 3 ]; then
    echo "RESULT: INCONCLUSIVE - ASan could not start (host memory pressure)"; rc_all=1
elif grep -q "AddressSanitizer" positive.out; then
    echo "RESULT: FAIL - ASan still reports an overflow:"
    grep -m3 -E "ERROR: AddressSanitizer|stack-buffer-overflow" positive.out | sed 's/^/    /'
    rc_all=1
elif [ $pos_rc -ne 0 ]; then
    echo "RESULT: FAIL - test reported failures (exit $pos_rc):"
    grep -E "^FAIL" positive.out | sed 's/^/    /'
    rc_all=1
else
    echo "RESULT: PASS - no ASan report, all checks agree."
    tail -2 positive.out | sed 's/^/    /'
fi

echo
[ $rc_all -eq 0 ] && echo "#231 regression test: PASS" || echo "#231 regression test: FAIL"
exit $rc_all
