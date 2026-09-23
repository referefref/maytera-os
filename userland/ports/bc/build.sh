#!/usr/bin/env bash
# ports/bc/build.sh - the build=script step for the bc-gh port (Tier 2 item #11
# of docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md).
#
# WHY build=script AND NOT build=objects. Two upstream steps a static `sources=`
# list cannot express:
#   1. CODEGEN. bc ships its arbitrary-precision math LIBRARY and its help text
#      as bc scripts / text (gen/lib.bc, gen/lib2.bc, gen/bc_help.txt), NOT as C.
#      The upstream tool gen/strgen.c turns each into a C string table
#      (gen/lib.c, gen/lib2.c, gen/bc_help.c). strgen must be compiled for and
#      run on the HOST (it is a build tool, not part of bc), and its output is
#      then cross-compiled. Without gen/lib.c the -l built-ins (a/s/c/l/e) simply
#      do not exist, which is why PORT's symbol check names bc_lib_name.
#   2. The generated files are not in the tarball, so they could never appear in
#      a `sources=` list (which mports verifies present in the unpacked tree).
#
# Contract from mports.sh: cwd = the unpacked, PATCHED srcdir; MPORTS_CFLAGS
# carries the userland cross-build flags PLUS this port's cflags= (-DMAYTERAOS);
# MPORTS_SRC is the unpacked srcdir; MPORTS_OUT is the install tree. After we
# return, mports looks for the archive at $(dirname MPORTS_SRC)/libbc.a and
# proves every symbol promised in PORT is defined in it.
set -euo pipefail

: "${MPORTS_CFLAGS:?mports must pass MPORTS_CFLAGS}"
: "${MPORTS_SRC:?mports must pass MPORTS_SRC}"

WORKDIR="$(dirname "$MPORTS_SRC")"   # where mports expects libbc.a
LIB="$WORKDIR/libbc.a"

# bc's own feature configuration. This is EXACTLY the set the upstream
# configure.sh emits for `--bc-only --disable-dc --disable-history --disable-nls
# --disable-man-pages` (verified by running it and reading the compile line):
# bc enabled, dc disabled, extra math (the -l library) ON, history/NLS/library
# OFF, and the default-behaviour flags for a POSIX-ish bc. strgen's output
# depends on BC_ENABLED, so the SAME set is passed to both the codegen host
# compile and the cross compile below.
BCDEFS=(
  -DBC_ENABLED=1 -DDC_ENABLED=0
  -DBUILD_TYPE=HN -DEXECPREFIX= -DMAINEXEC=bc
  -DBC_NUM_KARATSUBA_LEN=32
  -DBC_ENABLE_NLS=0 -DBC_ENABLE_EXTRA_MATH=1 -DBC_ENABLE_HISTORY=0
  -DBC_ENABLE_LIBRARY=0 -DBC_ENABLE_MEMCHECK=0
  -DBC_ENABLE_AFL=0 -DBC_ENABLE_OSSFUZZ=0
  -DBC_DEFAULT_BANNER=0
  -DBC_DEFAULT_SIGINT_RESET=1 -DBC_DEFAULT_TTY_MODE=1
  -DBC_DEFAULT_PROMPT=1 -DBC_DEFAULT_EXPR_EXIT=1 -DBC_DEFAULT_DIGIT_CLAMP=0
  -DDC_DEFAULT_SIGINT_RESET=1 -DDC_DEFAULT_TTY_MODE=0
  -DDC_DEFAULT_PROMPT=0 -DDC_DEFAULT_EXPR_EXIT=1 -DDC_DEFAULT_DIGIT_CLAMP=0
  -DNDEBUG -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
  -DBC_ENABLE_EDITLINE=0 -DBC_ENABLE_READLINE=0
)

# ---------------------------------------------------------------------------
# 1. CODEGEN: compile strgen for the HOST and regenerate the C string tables.
#    Host compile: plain gcc, NOT the cross flags. strgen is a throwaway build
#    tool that runs here on the build container; it never ships.
# ---------------------------------------------------------------------------
echo "bc/build.sh: compiling strgen for the host"
gcc -O2 -Iinclude -o "$WORKDIR/strgen" gen/strgen.c

echo "bc/build.sh: generating gen/{bc_help,lib,lib2}.c from the bc scripts"
# These three invocations are copied verbatim from the arguments the upstream
# Makefile passes strgen for a bc-only build (read off the real make output).
"$WORKDIR/strgen" gen/bc_help.txt gen/bc_help.c 0 bc_help "" BC_ENABLED 0
"$WORKDIR/strgen" gen/lib.bc  gen/lib.c  0 bc_lib  bc_lib_name  BC_ENABLED 1 "" "" 1
"$WORKDIR/strgen" gen/lib2.bc gen/lib2.c 0 bc_lib2 bc_lib2_name "BC_ENABLED && BC_ENABLE_EXTRA_MATH" 1 "" "" 1

# ---------------------------------------------------------------------------
# 2. CROSS COMPILE: every bc-only translation unit plus the three generated
#    files, into libbc.a. This is qbc_sources from the upstream build MINUS the
#    dc files (dc.c, dc_lex.c, dc_parse.c), history.c (history disabled) and
#    library.c (BC_ENABLE_LIBRARY=0). main.c IS included: its main() is what
#    userland/apps/bc links crt0 against, exactly like the darkhttpd/myman ports.
# ---------------------------------------------------------------------------
ENGINE_SRCS="args bc bc_lex bc_parse data file lang lex main num opt parse program rand read vector vm"
GEN_SRCS="bc_help lib lib2"

OBJS=()

echo "bc/build.sh: cross-compiling the bc engine (${ENGINE_SRCS})"
# -w because upstream trips -Wall/-Wextra warnings that are not ours to fix; this
# port ships the engine unaltered but for the one setjmp target patch.
for f in $ENGINE_SRCS; do
  gcc $MPORTS_CFLAGS "${BCDEFS[@]}" -Iinclude -w -c "src/$f.c" -o "$WORKDIR/bc_$f.o"
  OBJS+=("$WORKDIR/bc_$f.o")
done

echo "bc/build.sh: cross-compiling the generated tables (${GEN_SRCS})"
for f in $GEN_SRCS; do
  gcc $MPORTS_CFLAGS "${BCDEFS[@]}" -Iinclude -w -c "gen/$f.c" -o "$WORKDIR/gen_$f.o"
  OBJS+=("$WORKDIR/gen_$f.o")
done

echo "bc/build.sh: archiving libbc.a"
rm -f "$LIB"
ar rcs "$LIB" "${OBJS[@]}"

echo "bc/build.sh: done -> $LIB"
