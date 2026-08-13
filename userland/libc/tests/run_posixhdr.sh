#!/bin/bash
# run_posixhdr.sh - #745 (local 72) battery for the tier-1 POSIX headers added
# to userland/libc: getopt, libgen, ftw, sys/uio, sys/utsname, sys/file, utime,
# endian, byteswap, sys/param, alloca.
#
# WHAT IS ACTUALLY UNDER TEST. Every unit is compiled with the SHIPPING flags
# from userland/libc/Makefile (freestanding, -nostdinc, no host headers) and
# linked into the harness. There is no second copy of the code in the positive
# arm, so a green run is a statement about the objects that go into libc.a.
#
# TWO ARMS. Both must behave as stated or this script exits non-zero:
#
#   NEGATIVE CONTROL  swaps getopt.c for fixtures/getopt.c.no-ddash, which is
#                     the same file with the "--" end-of-options block removed
#                     and nothing else changed. That is the single most
#                     damaging way a getopt goes wrong: it stops honouring "--"
#                     and starts parsing the words after it, so a filename
#                     called "-rf" becomes options. The arm REQUIRES that build
#                     to FAIL the battery. A battery a broken getopt can pass
#                     is not a battery.
#
#   POSITIVE          builds the real sources and requires zero failures.
set -u

cd "$(dirname "$0")" || exit 1
LIBC=..
GCCINC=$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/include 2>/dev/null | tail -1)
[ -n "$GCCINC" ] || { echo "no gcc include dir found"; exit 1; }

# Mirrors userland/libc/Makefile CFLAGS for the units under test. -fno-pic and
# -mcmodel=large are dropped: this harness links as an ordinary host executable
# and the large code model needs host startup objects built the same way.
# Nothing under test here is code-model sensitive.
UUT_FLAGS="-m64 -ffreestanding -fno-builtin -nostdinc -fno-stack-protector \
           -mno-red-zone -Wall -Wextra -Werror -O2 -g -isystem $GCCINC -I$LIBC"

OTHER_UNITS="$LIBC/libgen.c $LIBC/ftw.c $LIBC/utime.c \
             $LIBC/sys/uio.c $LIBC/sys/utsname.c $LIBC/sys/file.c"

rc=0
d=$(mktemp -d) || exit 2
trap 'rm -rf "$d"' EXIT

build() {   # $1 = getopt source, $2 = tag, $3 = output binary
    local gsrc="$1" tag="$2" out="$3" objs="" src o i=0
    gcc $UUT_FLAGS -x c -c "$gsrc" -o "$d/$tag-getopt.o" || return 2
    objs="$d/$tag-getopt.o"
    for src in $OTHER_UNITS; do
        i=$((i + 1))
        o="$d/$tag-u$i.o"
        gcc $UUT_FLAGS -c "$src" -o "$o" || return 2
        objs="$objs $o"
    done
    gcc $UUT_FLAGS -c posixhdr_test.c -o "$d/$tag-harness.o" || return 2
    gcc -m64 -g "$d/$tag-harness.o" $objs -o "$out" || return 2
    return 0
}

echo "=== ARM 1: NEGATIVE CONTROL (getopt without \"--\" handling, must FAIL) ==="
if ! build "fixtures/getopt.c.no-ddash" neg "$d/broken"; then
    echo "RESULT: BROKEN - negative arm did not build"; rc=1
else
    "$d/broken" > "$d/neg.out" 2>&1
    n=$?
    cp "$d/neg.out" ./posixhdr_negative.out
    if [ $n -eq 0 ]; then
        echo "RESULT: BAD - a getopt that does not honour \"--\" PASSED the battery."
        echo "        The battery proves nothing. See posixhdr_negative.out."
        rc=1
    else
        echo "RESULT: GOOD - the broken getopt is caught. Failures it produced:"
        grep -m6 "^FAIL" "$d/neg.out" | sed 's/^/    /'
    fi
fi

echo
echo "=== ARM 2: POSITIVE (the real sources, must PASS) ==="
if ! build "$LIBC/getopt.c" pos "$d/real"; then
    echo "RESULT: FAIL - positive arm did not build"; rc=1
else
    "$d/real" > "$d/pos.out" 2>&1
    p=$?
    cp "$d/pos.out" ./posixhdr_positive.out
    cat "$d/pos.out"
    if [ $p -ne 0 ]; then
        echo "RESULT: FAIL - the battery reported failures"; rc=1
    else
        echo "RESULT: PASS"
    fi
fi

echo
if [ $rc -eq 0 ]; then echo "run_posixhdr.sh: PASS (both arms)"; else echo "run_posixhdr.sh: FAIL"; fi
exit $rc
