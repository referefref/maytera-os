# MayteraOS Win16 (Windows 3.x) compatibility layer — design

Goal: run ANY Windows 3.x application and installer on MayteraOS via a general
Win16 -> MayteraOS emulation/translation layer, not per-app patches.

Reference (for understanding ONLY — do NOT copy code; clean-room implementation):
- https://osfree.org/doku/doku.php?id=en:docs:win16
- https://github.com/osfree-project/WIN16

## Current foundation (builds 11-14)
- `kernel/exec/x86_16.{c,h}` — clean-room real-mode 8086/186 interpreter (1 MiB image).
- `kernel/exec/ne.{c,h}` — `win16_run_file(path)`: NE + DOS .COM loader. As of build 14 it
  correctly parses the NE header (CS:IP HIWORD=segment), loads ALL segments at consecutive
  real-mode paragraphs, sets cs/ds/ss/sp. INT 21h (DOS) + INT 80h (MVP thunk) handled.
- Verified: TETRIS.EXE (MS Entertainment Pack) loads (2 segs, entry 1000:6cf1) but runs into the
  instruction cap because relocations/imports are not yet applied.

## Architecture (phased)

### Phase 1 (#131) — NE relocations + imports + API trap/dispatch + logging
- Parse module-reference table, imported-names table, entry table, and per-segment relocation
  records (NE segment flag 0x0100 = has relocations; records are 8 bytes each after the segment
  data: addr-type, reloc-type+flags, src chain offset, target).
- Apply relocations:
  - INTERNAL ref  -> patch with the loaded paragraph of the target segment + offset.
  - IMPORT (ord/name) -> patch with a far pointer into a synthetic "Win16 thunk" segment
    (e.g. 0xF000), offset = a unique import id. Keep id -> (module, ordinal/name) table.
  - Handle the additive/chained fixup list (each source offset heads a chain terminated by 0xFFFF).
- Interpreter trap: when a far CALL targets the thunk segment, invoke a host
  `win16_api(cpu, id)` dispatcher (PASCAL convention: args pushed L->R on the stack, callee
  cleans up; return value in AX, or DX:AX for 32-bit). Dispatcher LOGS module+name for every call
  so any app reveals exactly which APIs it needs.

### Phase 2 (#132) — KERNEL / USER / GDI translation layer
- Implement the API surface incrementally (driven by Phase-1 logs). Map each Win16 HWND to a
  MayteraOS window; GDI calls render into its content buffer. Implement `x86_16_call_far` so
  DispatchMessage can call back into the 16-bit window procedure. Feed MayteraOS input as Win16
  messages (WM_PAINT/WM_KEYDOWN/WM_LBUTTONDOWN/WM_TIMER/...).

### Phase 3 (#133) — Win3.x environment
- C: drive mapped to a MayteraOS path (e.g. /WIN31 == C:\WINDOWS). INT 21h DOS file I/O against
  the FAT VFS. INI store (WIN.INI/SYSTEM.INI/PROGMAN.INI + GetPrivateProfile*/WriteProfile*),
  minimal registry, env vars (PATH/WINDIR/TEMP). DOS<->MayteraOS path translation.

### Phase 4 (#134) — installers + PROGMAN -> Start menu
- Run Win3.x SETUP via the layer (and/or a native installer reading SETUP.LST/.INF + floppy
  images). PROGMAN group creation (CreateGroup/AddItem) -> PROGMAN.INI [Groups]. Surface PROGMAN
  groups as nested folders in the MayteraOS Start menu. Floppy .img (FAT12) mount + MS SZDD
  (.EX_/.DL_) expansion already understood (host-side: msexpand).

## Test corpus
- MS Best of Entertainment Pack (winworldpc) — Disk01.img: TETRIS, CHIPS (Chip's Challenge),
  FREECELL, GOLF, JEZZBALL, TETRAVEX, RODENT, TUTSTOMB + SETUP.EXE. Games are SZDD-compressed
  (.EX_); expand with `msexpand < X.EX_ > X.EXE`. All verified to be valid NE Win3.x executables.

## Scope reality
This is a large, multi-build subsystem (effectively a focused 16-bit Wine + software GDI). Built
incrementally; each phase is independently useful (Phase 1 makes any app's API usage visible).
