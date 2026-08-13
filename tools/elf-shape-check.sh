#!/bin/bash
# elf-shape-check.sh - #633: does the MayteraOS kernel loader accept this binary?
#
# WHY THIS EXISTS. Three shipping apps (HELLO, INIT, SETMOUSE) were linked
# without `-T user.ld`. GNU ld then defaults to base 0x400000 and splits the
# image into four PT_LOADs, one of them read-only. Launching any of them from
# the desktop PANICKED the kernel (Ring 3 -> Ring 0 DoS, #633). The loader is
# now hardened and rejects them, but a rejected app is still a BROKEN app: the
# defect belongs at build time, and it had gone unnoticed across 43 binaries
# because nothing ever asked what shape they were.
#
# This encodes EXACTLY the rule kernel/exec/elf.c's elf_check_user_image()
# enforces, so the gate and the kernel cannot drift into disagreeing. If you
# change one, change the other, and run --self-test.
#
# Usage:
#   elf-shape-check.sh <file> [<file> ...]   # exit 0 if all acceptable
#   elf-shape-check.sh --self-test           # PROVE it goes RED on a bad shape
#
# Output is one line per file: "OK <name> <detail>" or "BAD <name> <reason>".

# This file is BOTH a CLI and a sourceable library (build/invariant-gate.sh
# sources it so the gate and this script cannot drift). Everything below the
# function definitions therefore runs only when executed directly.

# Must match kernel/exec/elf.c
IMG_MIN=$((0x80000000))       # legacy fixed-base window (ELF_USER_IMAGE_MIN/MAX)
IMG_MAX=$((0xC0000000))
MAX_SPAN=$((256*1024*1024))
# #640 stage 2: a PIE is placed by the KERNEL in the dedicated userland window
# (USER_WIN_IMAGE_BASE, mm/vmm.h), NOT at the retired 0x90000000, which is the
# live framebuffer address on the user iMac.
PIE_BASE=$((0x8000000000))    # USER_WIN_IMAGE_BASE
WIN_MIN=$((0x8000000000))     # USER_WIN_BASE
WIN_MAX=$((0xC000000000))     # USER_WIN_END

# Verdict for one file. Echoes "OK <detail>" or "BAD <reason>".
elf_shape_verdict() {
  local f="$1"
  [ -s "$f" ] || { echo "BAD empty file"; return 1; }

  # Not an ELF at all: not our business (data files live in /APPS too).
  local magic; magic=$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')
  [ "$magic" = "7f454c46" ] || { echo "SKIP not an ELF"; return 0; }

  local hdr; hdr=$(readelf -hW "$f" 2>/dev/null)
  local etype; etype=$(printf '%s' "$hdr" | awk -F: '/^  Type:/{print $2}' | awk '{print $1}')
  local entry; entry=$(printf '%s' "$hdr" | awk -F: '/^  Entry point address:/{gsub(/ /,"",$2); print $2}')
  case "$etype" in
    EXEC|DYN) : ;;
    *) echo "BAD e_type is '$etype' (kernel accepts only ET_EXEC or ET_DYN)"; return 1 ;;
  esac

  # PT_LOAD table: VirtAddr MemSiz Flags, from readelf -lW (one line per phdr).
  local loads
  loads=$(readelf -lW "$f" 2>/dev/null | awk '
    $1=="LOAD" { print $3, $6, $0 }')
  [ -n "$loads" ] || { echo "BAD no PT_LOAD segments"; return 1; }

  local nload=0 low="" high=0 x_ok=0
  local va sz line flags end
  while read -r va sz line; do
    nload=$((nload+1))
    va=$((va)); sz=$((sz))
    end=$((va+sz))
    # page-align like calculate_load_bounds()
    local va_dn=$(( va & ~4095 ))
    local end_up=$(( (end + 4095) & ~4095 ))
    if [ -z "$low" ] || [ "$va_dn" -lt "$low" ]; then low=$va_dn; fi
    if [ "$end_up" -gt "$high" ]; then high=$end_up; fi
    # entry-in-executable-segment: flags field of readelf -lW is the "R E"/"RWE"
    # column; grab everything between MemSiz and Align on the LOAD line.
    flags=$(printf '%s' "$line" | sed -E 's/.*0x[0-9a-f]+ (R?W?.?E?) +0x[0-9a-f]+$/\1/')
    case "$flags" in *E*)
      local e=$((entry))
      if [ "$e" -ge "$va" ] && [ "$e" -lt "$end" ]; then x_ok=1; fi
    ;; esac
  done <<< "$loads"

  local span=$((high-low))
  [ "$span" -le "$MAX_SPAN" ] || { echo "BAD image spans $span bytes, kernel limit is $MAX_SPAN"; return 1; }

  local base rend wmin wmax
  if [ "$etype" = "DYN" ]; then
    base=$PIE_BASE; rend=$((PIE_BASE+span)); wmin=$WIN_MIN; wmax=$WIN_MAX
  else
    base=$low; rend=$high; wmin=$IMG_MIN; wmax=$IMG_MAX
  fi
  if [ "$base" -lt "$wmin" ] || [ "$rend" -gt "$wmax" ]; then
    printf 'BAD load range 0x%X-0x%X outside the userland image window 0x%X-0x%X (linked without -T user.ld?)\n' \
           "$base" "$rend" "$wmin" "$wmax"
    return 1
  fi
  [ "$x_ok" = 1 ] || { printf 'BAD entry 0x%X is not inside an executable PT_LOAD\n' "$((entry))"; return 1; }

  printf 'OK %s %d PT_LOAD base=0x%X span=%d\n' "$etype" "$nload" "$base" "$span"
  return 0
}

_elfshape_main() {
if [ "${1:-}" = "--self-test" ]; then
  # Prove RED on a bad shape and GREEN on a good one. Without this the gate is
  # just an assertion that has never been observed to fire.
  td=$(mktemp -d); trap 'rm -rf "$td"' EXIT
  cat > "$td/t.c" <<'EOF'
void _start(void){ for(;;); }
EOF
  gcc -ffreestanding -nostdlib -fno-pic -mno-red-zone -mcmodel=large -c "$td/t.c" -o "$td/t.o" 2>/dev/null || {
    echo "SELF-TEST INCONCLUSIVE: no gcc"; exit 2; }
  # GOOD: single RWX PT_LOAD at 0x80000000, the user.ld shape.
  cat > "$td/good.ld" <<'EOF'
ENTRY(_start)
PHDRS { load PT_LOAD FLAGS(7); }
SECTIONS { . = 0x80000000; .text : { *(.text*) } :load .data : { *(.data*) } :load }
EOF
  ld -nostdlib -z max-page-size=0x1000 -T "$td/good.ld" -o "$td/good" "$td/t.o" 2>/dev/null
  # BAD: exactly the #633 shape - default ld base 0x400000, no user.ld.
  ld -nostdlib -z max-page-size=0x1000 -o "$td/bad" "$td/t.o" 2>/dev/null

  rc=0
  g=$(elf_shape_verdict "$td/good"); case "$g" in OK*) echo "  self-test GOOD -> $g";; *) echo "  self-test GOOD MISJUDGED -> $g"; rc=1;; esac
  b=$(elf_shape_verdict "$td/bad");  case "$b" in BAD*) echo "  self-test BAD  -> $b";; *) echo "  self-test BAD NOT CAUGHT -> $b"; rc=1;; esac
  if [ "$rc" = 0 ]; then echo "elf-shape-check self-test: PASS (green on good, RED on the real #633 shape)";
  else echo "elf-shape-check self-test: FAIL"; fi
  exit $rc
fi

[ $# -ge 1 ] || { echo "usage: $0 <elf> [...] | --self-test" >&2; exit 2; }
local rc=0 f v
for f in "$@"; do
  v=$(elf_shape_verdict "$f") || rc=1
  echo "$(basename "$f"): $v"
done
exit $rc
}

# Executed directly (not sourced)?
if [ "${BASH_SOURCE[0]:-$0}" = "$0" ]; then
  _elfshape_main "$@"
fi
