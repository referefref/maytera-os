# Win16 (Windows 3.1) Apps on MayteraOS — Feasibility Assessment (#76)

> STATUS: REFUTED (2026-08-22, #242) — this assessment concluded Win16 support was "not worth implementing, won't-do"; a full NE loader + interpreter running Word 6 shipped, the exact opposite conclusion (see docs/WORD6_LOCALHEAP_PLAN.md and CHANGELOG.md's #278 Word 6 pass history).


Date: 2026-06-17. Verdict: **not worth implementing now; native ports or a DOS path are far better ROI.**

## What running a Win16 app actually requires

A Windows 3.1 application is a **16-bit NE (New Executable)** binary that runs in the
segmented 16-bit (real/protected) environment and calls the Win16 API
(`KERNEL`, `USER`, `GDI` and friends). To run one you need ALL of:

1. **NE loader** — parse the NE format, its segment table, relocations, imported-name
   table, and resource table (very different from the ELF/PE loaders MayteraOS has).
2. **16-bit execution** — Win16 code is 16-bit segmented (`CS:IP`, far calls, selectors).
   MayteraOS runs the CPU in **64-bit long mode**, where **vm86 mode does not exist** and
   16-bit protected-mode segments are heavily restricted. You would need either:
   - a software **x86-16 interpreter/JIT** (an emulator), or
   - drop a core to **real/16-bit protected mode** and thunk - incompatible with the
     long-mode kernel + SMP/paging model.
3. **A Win16 API implementation** — hundreds of functions: window manager, message loop
   (`GetMessage`/`DispatchMessage`), GDI drawing, dialog/resource engine, GlobalAlloc/
   LocalAlloc 16-bit heaps, the cooperative-multitasking scheduler, etc. This is the bulk
   of a WINE-class project, for an obsolete 16-bit target.
4. **Resource + dialog engine**, 16-bit handle tables, the `WndProc` callback ABI (far
   pointers), and message thunking between 16- and 64-bit code.

## Effort vs. value

- Effort: **very high** (a WINE-for-Win16 / emulator-grade subsystem). Months, and it
  fights the 64-bit long-mode design at every step.
- Value: **low** - Win16 software is ~30 years obsolete; almost nothing the user wants is
  Win16-only. The desktop apps people actually use are better delivered as **native
  MayteraOS apps** (which is the whole point of this OS).

## Lower-cost alternatives (recommended instead)

1. **Native ports** - the established path (DOOM, Keen, Python already ported this way).
2. **DOS layer** - MayteraOS already has `dos/`. Targeting **DOS apps** (16-bit DOS, not
   Win16) via the existing DOS emulation is far cheaper than a full Windows subsystem, and
   covers a lot of "old software" demand. Extending `dos/` is the sensible direction if
   retro-app support is wanted.
3. If Win16 is ever a hard requirement, the realistic route is an **x86 emulator app**
   (run a tiny DOS+Windows 3.1 image inside an emulator process), not a native subsystem.

## Recommendation

Mark #76 **assessed/closed as won't-do**. Invest in native apps and (if desired) the
existing DOS layer. Revisit only if a specific, irreplaceable Win16 app is required, in
which case use the emulator-app route, not a native Win16 subsystem.
