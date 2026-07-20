#!/bin/sh
# #501: host-side proof that every spawn of every built-in box level is standable.
# Compiles the REAL ../levels.c and ../world.c (not copies - see spawn_test.c).
# Not part of the shipping ARENA build (subdir, not globbed by ../Makefile).
set -e
cd "$(dirname "$0")"
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -I.. -c ../world.c  -o world.o
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -I.. -c ../levels.c -o levels.o
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -I.. spawn_test.c world.o levels.o -lm -o spawn_test
./spawn_test
