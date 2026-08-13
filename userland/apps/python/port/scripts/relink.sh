#!/bin/bash
set -e
L=<workspace>
U=<workspace>
SH=/root/cpython-port/shim
GI=/usr/lib/gcc/x86_64-linux-gnu/12/include
S=/root/cpython-port/Python-3.11.9
CP=/root/cpython-port
cd $CP/build

FLAGS="-ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mcmodel=large -nostdinc -fno-builtin -O2 -std=c11 -I$SH -I$L -isystem $GI -I. -I$S/Include -U__linux__ -D_Py_FORCE_UTF8_LOCALE -D_SYSCALL_H -Wno-implicit-function-declaration"

echo "== rebuild trimmed supplements =="
gcc $FLAGS -c $CP/compatsupp/compat.c -o $CP/compatsupp/compat.o
ar rcs $CP/compatsupp/libcompatsupp.a $CP/compatsupp/compat.o
gcc $FLAGS -c $CP/miscsupp/misc.c -o $CP/miscsupp/misc.o
ar rcs $CP/miscsupp/libmiscsupp.a $CP/miscsupp/misc.o

echo "== compile main + frozen =="
gcc $FLAGS -c $CP/mos_pymain.c -o mos_pymain.o
gcc $FLAGS -c $CP/frozen_encodings.c -o frozen_encodings.o

MATH=$CP/mathsupp/libpymath_supp.a
WCH=$CP/wcharsupp/libwcharsupp.a
MISC=$CP/miscsupp/libmiscsupp.a
COMPAT=$CP/compatsupp/libcompatsupp.a

echo "== link (no --wrap; kernel fcntl + libc errno-open now real) =="
ld -nostdlib -z max-page-size=0x1000 -T $U/user.ld --no-warn-rwx-segments -o python.elf \
   $L/crt0.o mos_pymain.o frozen_encodings.o \
   --start-group libpython3.11.a $MATH $WCH $MISC $COMPAT $L/libc.a --end-group 2> link_err.txt
echo "LD exit: $?"
echo "-- undefined refs --"; grep -oE "undefined reference to .[^']+." link_err.txt | sort -u | head -40
echo "-- multiple defs --"; grep -i "multiple definition" link_err.txt | head -20
echo "-- other errors --"; grep -iE "error" link_err.txt | grep -viE "multiple|undefined" | head
ls -la python.elf 2>/dev/null && file python.elf 2>/dev/null
