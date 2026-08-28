#!/bin/sh
# Host unit test for the Terminal emulation core (term_emu.c).
#
# term_emu.c makes no syscalls and includes no GUI header, which is the whole
# reason it was split out: it compiles with a HOSTED gcc, so every escape
# sequence can be driven directly and asserted on, instead of being inferred
# from how a whole TUI looked in a VM.
#
# Exit 0 = all assertions passed.
set -e
cd "$(dirname "$0")"
gcc -O1 -Wall -Wextra -Werror -DTERM_EMU_HOSTTEST \
    -o /tmp/term_emu_test term_emu_test.c ../term_emu.c
exec /tmp/term_emu_test
