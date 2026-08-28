#!/bin/bash
# relink-pie.sh - relink the CPython port as a PIE, W^X-clean binary.
#
# #661. The shipped /APPS/PYTHON.ELF was a 2026-07-08 ET_EXEC with a
# writable+executable PT_LOAD, and no build step reproduced it. Its sibling
# relink.sh linked non-PIE against user.ld and against paths in the long-dead
# active-code tree, so it could not run in the current build either.
#
# This is that script pointed at the repo build tree and switched to the same
# PIE link every other userland app uses.
#
# TEXT RELOCATIONS: libpython3.11.a is prebuilt with -fno-pic -mcmodel=large,
# so the archive contains absolute relocations against .text. -z notext tells
# the linker to allow that in a PIE (it is what every other app already passes).
# The kernel loader applies the resulting R_X86_64_RELATIVE entries at load, so
# the image still relocates correctly; it just carries more of them.
set -e
U=${U:-<repo>/userland}
L=$U/libc
CP=<workspace>
S=$CP/Python-3.11.9
SH=$CP/shim
GI=/usr/lib/gcc/x86_64-linux-gnu/12/include
OUT=${OUT:-$CP/build/python-pie.elf}

[ -f "$L/libc.a" ]  || { echo "FATAL: no $L/libc.a - build userland first"; exit 1; }
[ -f "$L/crt0.o" ]  || { echo "FATAL: no $L/crt0.o"; exit 1; }
[ -f "$U/user-pie.ld" ] || { echo "FATAL: no $U/user-pie.ld"; exit 1; }
[ -f "$CP/build/libpython3.11.a" ] || { echo "FATAL: no libpython3.11.a"; exit 1; }

cd $CP/build

# -fPIE replaces -fno-pic. Everything else matches the working non-PIE recipe.
FLAGS="-ffreestanding -fno-stack-protector -fPIE -mno-red-zone -mcmodel=large \
-nostdinc -fno-builtin -O2 -std=c11 -I$SH -I$L -isystem $GI -I. -I$S/Include \
-U__linux__ -D_Py_FORCE_UTF8_LOCALE -D_SYSCALL_H -Wno-implicit-function-declaration"

echo "== rebuild trimmed supplements (PIE) =="
gcc $FLAGS -c $CP/compatsupp/compat.c -o $CP/compatsupp/compat-pie.o
ar rcs $CP/compatsupp/libcompatsupp-pie.a $CP/compatsupp/compat-pie.o
gcc $FLAGS -c $CP/miscsupp/misc.c -o $CP/miscsupp/misc-pie.o
ar rcs $CP/miscsupp/libmiscsupp-pie.a $CP/miscsupp/misc-pie.o

echo "== compile main + frozen (PIE) =="
gcc $FLAGS -c $CP/mos_pymain.c -o mos_pymain-pie.o
gcc $FLAGS -c $CP/frozen_encodings.c -o frozen_encodings-pie.o

MATH=$CP/mathsupp/libpymath_supp.a
WCH=$CP/wcharsupp/libwcharsupp.a
MISC=$CP/miscsupp/libmiscsupp-pie.a
COMPAT=$CP/compatsupp/libcompatsupp-pie.a

echo "== link as PIE =="
ld -pie --no-dynamic-linker -z notext -nostdlib -z max-page-size=0x1000 \
   -T $U/user-pie.ld --no-warn-rwx-segments -o "$OUT" \
   $L/crt0.o mos_pymain-pie.o frozen_encodings-pie.o \
   --start-group libpython3.11.a $MATH $WCH $MISC $COMPAT \
     $CP/build/Modules/_decimal/libmpdec/libmpdec.a $CP/zlib-build/libz.a $CP/bz2-build/libbz2.a $CP/lzma-build/liblzma.a \
     $CP/stublib/libm.a $CP/stublib/libcrypt.a $CP/stublib/libdl.a $CP/stublib/libnsl.a \
     $CP/stublib/libpthread.a $CP/stublib/librt.a $CP/stublib/libutil.a \
     $L/libc.a --end-group \
   2> link_pie_err.txt || true
echo "-- undefined refs --"; grep -oE "undefined reference to .[^']+." link_pie_err.txt | sort -u | head -20
echo "-- multiple defs --";  grep -i "multiple definition" link_pie_err.txt | head -10
echo "-- other errors --";   grep -iE "error" link_pie_err.txt | grep -viE "multiple|undefined" | head

if [ ! -f "$OUT" ]; then echo "LINK FAILED"; exit 1; fi
echo
echo "== resulting shape =="
readelf -h "$OUT" | grep -E 'Type:|Entry'
readelf -lW "$OUT" | grep -E '^  LOAD'
wx=$(readelf -lW "$OUT" | grep '^  LOAD' | awk '$7 ~ /W/ && $7 ~ /E/' | wc -l)
echo "W+X LOAD segments: $wx"
ls -la "$OUT"
