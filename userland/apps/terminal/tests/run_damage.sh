#!/bin/sh
# Build and run the Terminal damage-tracking measurement harness.
#
#   run_damage.sh                 measure every trace under traces/
#   run_damage.sh --self-test     PROVE the harness can go RED
#
# The harness runs the PRODUCTION renderer (term_render.c and friends) with the
# window syscalls replaced by counting stubs and a plain pixel array, over a
# byte stream captured from a real pty. For every trace it runs the SAME stream
# twice in one process: once with damage tracking off (which is the pre-change
# renderer exactly, since term_shadow == NULL is already the repaint-everything
# path) and once with it on. It reports the work saved AND compares the two
# emulated windows after EVERY frame.
#
# Exit 0 = every frame of every trace was pixel-identical between the two arms.
set -e
cd "$(dirname "$0")"

SRC="term_damage_test.c term_damage_hostio.c \
     ../term_emu.c ../term_grid.c ../term_parse.c ../term_scrollback.c \
     ../term_render.c ../term_util.c ../../../libc/gui_scroll.c"
BIN=/tmp/term_damage_test

build() { gcc -O1 -w $1 -o $BIN $SRC; }

if [ "$1" = "--self-test" ]; then
    echo "--- ARM 1: the comparison deliberately broken (must report MISMATCH)"
    build -DTERM_DAMAGE_SELFTEST_BLIND
    bad=0
    for t in traces/*.trace; do
        b=$(basename "$t" .trace)
        [ -f "traces/$b.frames" ] || continue
        if $BIN "$t" "traces/$b.frames" 80 24 > /tmp/st.$b.txt; then
            echo "  $b: STILL GREEN - the harness cannot detect a stale cell. BROKEN."
            bad=1
        else
            echo "  $b: RED, as required ($(grep 'frames that differ' /tmp/st.$b.txt | tr -s ' '))"
        fi
    done
    echo "--- ARM 2: the real comparison (must report IDENTICAL)"
    build
    for t in traces/*.trace; do
        b=$(basename "$t" .trace)
        [ -f "traces/$b.frames" ] || continue
        if $BIN "$t" "traces/$b.frames" 80 24 > /tmp/ok.$b.txt; then
            echo "  $b: GREEN, as required"
        else
            echo "  $b: RED - damage tracking does NOT reproduce the full repaint."
            bad=1
        fi
    done
    [ $bad -eq 0 ] || { echo "SELF-TEST FAILED"; exit 1; }
    echo "SELF-TEST PASSED: red when broken, green when correct."
    exit 0
fi

build
rc=0
for t in traces/*.trace; do
    b=$(basename "$t" .trace)
    [ -f "traces/$b.frames" ] || continue
    echo "=========================================================== $b"
    $BIN "$t" "traces/$b.frames" 80 24 || rc=1
done
exit $rc
