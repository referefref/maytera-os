#!/bin/bash
# run_spawn.sh - #745 test for userland/libc/spawn.c (posix_spawn).
#
# Builds the SHIPPING ../spawn.c and ../sys/wait.c with the same freestanding
# flags the libc Makefile uses, links them against tests/spawn_host.c (a model
# of kernel/proc/syscall.c spawn_impl() that really forks and execs), and runs
# the result. Nothing under test is copied, stubbed or conditionally compiled.
#
# Before any of that it cross-checks the two things a userland-only test cannot
# otherwise prove:
#
#   1. every syscall number this depends on is the SAME in userland/libc/
#      syscall.h and kernel/proc/syscall.h; and
#   2. the kernel dispatcher passes those syscalls' arguments in the order
#      spawn.c assumes.
#
# A guessed or drifted constant ships a feature that compiles, runs and does
# nothing, so both checks are hard failures, not warnings.
set -u

cd "$(dirname "$0")" || exit 1
LIBC=..
REPO=../../..
GCCINC=$(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/include 2>/dev/null | tail -1)
[ -n "$GCCINC" ] || { echo "no gcc include dir found"; exit 1; }

UH=$LIBC/syscall.h
KH=$REPO/kernel/proc/syscall.h
KC=$REPO/kernel/proc/syscall.c
for f in "$UH" "$KH" "$KC"; do
    [ -r "$f" ] || { echo "missing $f"; exit 1; }
done

rc_all=0

num_from() {   # $1 = file, $2 = macro
    grep -Eo "^#define[[:space:]]+$2[[:space:]]+[0-9]+" "$1" | head -1 | awk '{print $3}'
}

echo "=== ARM 1: syscall numbers agree between userland and kernel ==="
declare -A NUM
for m in SYS_OPEN SYS_CLOSE SYS_READ SYS_SPAWN_ARGS SYS_SPAWN_REDIR SYS_WAITPID; do
    u=$(num_from "$UH" "$m")
    k=$(num_from "$KH" "$m")
    if [ -z "$u" ] || [ -z "$k" ]; then
        echo "  BAD  $m: userland='${u:-<missing>}' kernel='${k:-<missing>}'"
        rc_all=1
        continue
    fi
    if [ "$u" != "$k" ]; then
        echo "  BAD  $m: userland=$u kernel=$k  DISAGREE"
        rc_all=1
        continue
    fi
    echo "  ok   $m = $u (both)"
    NUM[$m]=$u
done
[ $rc_all -eq 0 ] || { echo "RESULT: FAIL - syscall numbers do not agree"; exit 1; }

echo
echo "=== ARM 2: the kernel dispatcher's argument order is what spawn.c assumes ==="
check_disp() {   # $1 = description, $2 = fixed string expected in syscall.c
    if grep -qF "$2" "$KC"; then
        echo "  ok   $1"
    else
        echo "  BAD  $1"
        echo "       expected to find in kernel/proc/syscall.c:"
        echo "         $2"
        rc_all=1
    fi
}
check_disp "SYS_SPAWN_ARGS(path, argv, argc)" \
    'sys_spawn_args((const char *)arg1, (char **)arg2, (int)arg3);'
check_disp "SYS_SPAWN_REDIR(path, argv, argc, infile, outfile, append)" \
    'sys_spawn_redir((const char *)arg1, (char **)arg2, (int)arg3,'
check_disp "SYS_WAITPID(pid, status)" \
    'proc_wait((int)arg1, (int *)arg2);'
[ $rc_all -eq 0 ] || { echo "RESULT: FAIL - dispatcher shape changed"; exit 1; }

echo
echo "=== ARM 3: build and run ==="
D=$(mktemp -d) || exit 2
trap 'rm -rf "$D"' EXIT

# Helper binaries. They must be real ELFs: the model validates ELF magic before
# creating a child, exactly as spawn_impl() does, so a shell script would be
# rejected and the test would be testing the wrong thing.
cat > "$D/helper_exit7.c" <<'EOF'
int main(void) { return 7; }
EOF
cat > "$D/helper_exit3.c" <<'EOF'
int main(void) { return 3; }
EOF
cat > "$D/helper_say.c" <<'EOF'
#include <unistd.h>
#include <string.h>
int main(void) { const char *s = "hello-from-spawn\n";
                 return write(1, s, strlen(s)) < 0; }
EOF
cat > "$D/helper_cat.c" <<'EOF'
#include <unistd.h>
#include <string.h>
int main(void) {
    char b[256]; ssize_t n = read(0, b, sizeof(b) - 1);
    if (n < 0) n = 0;
    while (n > 0 && b[n-1] == '\n') n--;
    b[n] = 0;
    char out[300]; strcpy(out, "got:"); strcat(out, b); strcat(out, "\n");
    return write(1, out, strlen(out)) < 0;
}
EOF
for h in helper_exit7 helper_exit3 helper_say helper_cat; do
    gcc -O1 -o "$D/$h" "$D/$h.c" 2>"$D/cc.$h.log" || {
        echo "  helper $h failed to build:"; sed -n 1,10p "$D/cc.$h.log"; exit 2; }
done
cp "$D/helper_exit3" "$D/HELPER.ELF"          # only the UPPER + .ELF form exists
printf 'not an elf at all' > "$D/notanelf.bin"
printf 'from-the-input-file\n' > "$D/input.txt"

# Freestanding flags mirroring userland/libc/Makefile. -mcmodel=large, -fno-pic
# and the stack protector are dropped so the objects link into a hosted test
# binary; this is the same adjustment tests/run.sh already makes.
UUT_FLAGS="-m64 -ffreestanding -fno-builtin -nostdinc -fno-stack-protector \
           -mno-red-zone -Wall -Wextra -Werror -O1 -g -isystem $GCCINC -I$LIBC"

DEFS=""
for m in SYS_OPEN SYS_CLOSE SYS_READ SYS_SPAWN_ARGS SYS_SPAWN_REDIR SYS_WAITPID; do
    DEFS="$DEFS -DM_${m}=${NUM[$m]}"
done

gcc $UUT_FLAGS -c "$LIBC/spawn.c" -o "$D/spawn.o" 2>"$D/cc1.log" || {
    echo "  compile of spawn.c FAILED:"; sed -n 1,30p "$D/cc1.log"; exit 2; }
# waitpid/wait are renamed so the model can call glibc's wait4 without the
# shipping waitpid shadowing it and recursing back into the model.
gcc $UUT_FLAGS -Dwaitpid=mos_waitpid -Dwait=mos_wait \
    -c "$LIBC/sys/wait.c" -o "$D/wait.o" 2>"$D/cc2.log" || {
    echo "  compile of sys/wait.c FAILED:"; sed -n 1,30p "$D/cc2.log"; exit 2; }
gcc $UUT_FLAGS -c spawn_test.c -o "$D/test.o" 2>"$D/cc3.log" || {
    echo "  compile of spawn_test.c FAILED:"; sed -n 1,30p "$D/cc3.log"; exit 2; }
gcc -m64 -Wall -Wextra -O1 -g $DEFS -c spawn_host.c -o "$D/host.o" 2>"$D/cc4.log" || {
    echo "  compile of spawn_host.c FAILED:"; sed -n 1,30p "$D/cc4.log"; exit 2; }
gcc -m64 -no-pie -o "$D/t" "$D/host.o" "$D/test.o" "$D/spawn.o" "$D/wait.o" \
    2>"$D/ld.log" || {
    echo "  link FAILED:"; sed -n 1,30p "$D/ld.log"; exit 2; }

echo "  built; symbols under test, straight out of the shipping objects:"
nm --defined-only "$D/spawn.o" | grep -E ' T posix_spawn' | sed 's/^/    /'

echo
SPAWNTEST_DIR="$D" "$D/t"
rc=$?

echo
if [ $rc -eq 0 ]; then
    echo "#745 posix_spawn test: PASS"
else
    echo "#745 posix_spawn test: FAIL (exit $rc)"
fi
exit $rc
