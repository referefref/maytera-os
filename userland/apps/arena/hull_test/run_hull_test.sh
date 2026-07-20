#!/bin/sh
# run_hull_test.sh - #491 Stage 2 offline known-answer hull-trace test.
# Same technique as ../bsp_test/: build the IDENTICAL Rust crate source for the
# HOST target (not the Ring-3 x86_64-unknown-none target) so it links into a
# normal Linux binary and runs directly, no VM/kernel needed for this proof.
set -e
cd "$(dirname "$0")"
python3 gen_hull_fixture.py hull_room.bsp
rustc --edition 2021 --crate-type staticlib --target x86_64-unknown-linux-gnu \
      -C panic=abort -C opt-level=2 -o libarena_rs_host.a ../arena_rs.rs
gcc -g -O1 -fsanitize=undefined hull_test.c libarena_rs_host.a -o hull_test -lm
./hull_test hull_room.bsp
