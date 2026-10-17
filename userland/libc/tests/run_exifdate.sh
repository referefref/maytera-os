#!/bin/bash
# run_exifdate.sh - MEASURED host unit test for the #713 EXIF DateTimeOriginal
# parser (../exifdate.c). The test (exifdate_test.c) #includes ../exifdate.c in
# host-test mode and is compiled WITH AddressSanitizer, so the parser under test
# is instrumented: any out-of-bounds read of an exactly-sized heap buffer is a
# hard failure, not luck. This is the bounds-safety proof the parser must earn
# because it consumes untrusted file data.
#
# Exit 0 = every assertion held and ASan found no fault; non-zero otherwise.
set -u
cd "$(dirname "$0")" || exit 1

WORK=$(mktemp -d) || exit 2
trap 'rm -rf "$WORK"' EXIT

echo "=== building EXIF parser unit test with AddressSanitizer ==="
if ! gcc -m64 -O1 -g -std=c11 -Wall -Wextra \
        -fsanitize=address -fno-omit-frame-pointer \
        exifdate_test.c -o "$WORK/exifdate_test" 2>"$WORK/cc.log"; then
    echo "BROKEN - unit test did not build:"; sed -n 1,40p "$WORK/cc.log"; exit 1
fi
# A warning-free build is part of the contract for a security-sensitive parser.
if grep -q 'warning:' "$WORK/cc.log"; then
    echo "BROKEN - compiler warnings (treated as a defect for this parser):"
    sed -n 1,40p "$WORK/cc.log"; exit 1
fi

# #deadtests: through the SHARED runner (tools/asan/asan-run). This script had
# no DEADLYSIGNAL mitigation at all and was therefore RED on roughly 2 runs in 5
# on the the build host host, for a reason with nothing to do with the EXIF parser: the
# host has vm.mmap_rnd_bits=32 and gcc ASan assumes ~28, so ASan died in its own
# startup before main(). MEASURED 2026-10-02: 2/5 via `bash run_exifdate.sh`,
# 2/20 on the bare binary, 0/20 under setarch -R.
echo "=== running (shared asan-run; ASAN_OPTIONS abort_on_error=1) ==="
bash ../../../tools/asan/asan-run \
    --opts abort_on_error=1:detect_leaks=0 \
    --expect "#713 EXIF DateTimeOriginal parser" \
    -- "$WORK/exifdate_test"
rc=$?
if [ $rc -eq 3 ]; then
    echo "#713 EXIF parser unit test: INCONCLUSIVE - ASan never started (environment)"
    exit 3
fi
echo
[ $rc -eq 0 ] && echo "#713 EXIF parser unit test: PASS" \
              || echo "#713 EXIF parser unit test: FAIL (exit $rc)"
exit $rc
