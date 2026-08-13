#!/bin/bash
# Freestanding cross-compile helper for MayteraOS NetSurf port.
# Uses -mcmodel=large because user apps load at 0x80000000 (2GB); the default
# (small) code model emits 32-bit relocations that overflow at that address and
# fault at runtime. -D_ALIGNED= neutralises libcss's struct-attr macro (undefined
# in this freestanding config, otherwise it creates a bogus global tentative def).
# -DWITHOUT_ICONV_FILTER selects the built-in charset codecs (no iconv).
# The active-code tree is STALE reference only (CLAUDE.md); its syscall.h
# already differs from the repo's. Default to the source of truth, and stay
# overridable so a checkout elsewhere can point at its own libc.
LIBC=${LIBC:-<repo>/userland/libc}
SHIM=<workspace>
GCCINC=/usr/lib/gcc/x86_64-linux-gnu/12/include
exec gcc -m64 -ffreestanding -fno-builtin -nostdinc -nostdlib \
  -fno-stack-protector -mno-red-zone -mcmodel=large -fno-pic -O2 \
  -D_ALIGNED= -DWITHOUT_ICONV_FILTER \
  -isystem $GCCINC \
  -I$SHIM -I$LIBC \
  "$@"
