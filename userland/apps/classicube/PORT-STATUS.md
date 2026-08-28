# ClassiCube on MayteraOS (#28) - build wiring, libc gap analysis, verification plan

Upstream: https://github.com/UnknownShadow200/ClassiCube, BSD-3-Clause.
Pinned at `4016a0918ba5c127d5203a4940e76b79b229d51f` (2026-08-09) by
`fetch-engine.sh`. `license.txt` is vendored alongside the source.

This file covers the build-wiring lane only. The platform layer
(`Platform_*.c`, `Window_*.c`, `Graphics_*.c`, `Http`/`Socket_*`, `Audio_*.c`)
is owned by other agents.

---

## 1. THE LIBC GAP IS ZERO. Nobody is blocked on libc.

**Measured, not estimated.** All 101 upstream `src/*.c` translation units were
compiled against `userland/libc` with the real app flag set, and the undefined
symbols of the resulting objects were diffed against `nm -g --defined-only` of
`libc.a` (556 symbols) and `libgl.a` (317 symbols).

    COMPILE: ok=101 fail=0
    GENUINE libc gap: 0

Reproduce it at any time with `make gap` in this directory.

**ClassiCube references exactly three external symbols from our libc across the
entire portable core (~450 KB of text):**

| Symbol              | Status in `userland/libc`                                    |
|---------------------|--------------------------------------------------------------|
| `sqrtf`             | present, `math.c:18`, a single hardware `sqrtss` instruction  |
| `__stack_chk_fail`  | present, `stack_guard.c`                                      |
| `__stack_chk_guard` | present, `stack_guard.c`                                      |

That is the whole list. **No math functions are missing.** The `sinf`, `cosf`,
`atan2f`, `fmodf`, `powf` family that the port was expected to need is not
needed at all: ClassiCube ships its own `ExtMath.c`, written for consoles with
no libm, which implements `Math_Sin`/`Math_Cos`/`Math_Exp`/`Math_Log`/
`Math_Atan2`/`Math_Pow` in portable C. `Math_SqrtF` is the single function it
delegates, and our `sqrtf` is one `sqrtss`, so it is IEEE-correct by
construction rather than by an approximation that would need a tolerance test.

No string, stdio, or stdlib symbol is missing either, because ClassiCube routes
every allocation and memory operation through its own `Mem_*` platform
interface and every string operation through its own `cc_string` in `String.c`.
Our libc is reached only through the platform layer the other agents are
writing, and everything that layer will plausibly call (`malloc`, `free`,
`memcpy`, `memset`, `open`, `read`, `write`, `pthread_*`, `socket`, ...) is
already present in the 556-symbol set.

**Consequence for the other four agents: do not add libc functions for this
port, and do not fork private copies of libc routines into a platform file.**
If a genuinely missing symbol appears later it will show up in `make gap` under
"GENUINE libc gap"; fix it in `userland/libc` so every app benefits, per the
shared-primitives rule in CLAUDE.md.

## 1b. Two real libc defects, found and fixed in the shared libc

Zero MISSING symbols does not mean zero BROKEN ones. Two functions the port
depends on were present, linked, and silently wrong. Both are fixed in
`userland/libc/pthread.c`; see the CHANGELOG entry for the full reasoning.

* **`pthread_cond_timedwait()` treated a POSIX absolute deadline as a relative
  timeout**, turning any real `abstime` into a **20,679-day (about 56-year)**
  wait, measured. It never returned an error and never timed out. Now converted
  against a millisecond wall clock, with an explicit already-expired
  `ETIMEDOUT` because 0 means WAIT FOREVER to `futex_wait()`.
* **Every thread leaked about 64 KB**, joined or detached, not just detached as
  first reported: `pthread_join()` freed only the 4-byte join word, while the
  64 KB stack and the `thread_start_t` were unreachable. Threads now carry a
  record that owns all three, with a lazy reaper for detached threads.

Anything relying on `pthread_detach()` or `pthread_cond_timedwait()` should be
rebuilt against the fixed libc rather than working around the old behaviour.

## 2. Float verdict: userland is HARDWARE float (SSE2). Write normal C.

The kernel is soft-float with SSE disabled. **Userland is not, and this was
measured rather than assumed:**

* `gcc -Q --help=target -O2` in the build container reports `-msse [enabled]`,
  `-msse2 [enabled]`, `-mfpmath=sse`. SSE2 is the x86-64 baseline and nothing
  in the userland flag set turns it off. Only the kernel passes `-mno-sse
  -mno-sse2`.
* `userland/apps/glcube/Makefile` passes `-msse -msse2` explicitly. This
  Makefile does the same, so the choice is visible in the file.
* Disassembling the compiled objects shows real scalar-float instructions:
  `ExtMath.o` contains 102 `mulsd`, 35 `addsd`, 15 `mulss`, 12 `subss`,
  4 `cvtsi2ss`, 2 `divss`; our own `libc/math.o` contains 117 `mulsd`,
  56 `divsd`, 9 `sqrtsd`, 1 `sqrtss`.
* There are **zero** libgcc soft-float libcalls (`__mulsf3`, `__adddf3`,
  `__floatsidf`, ...) undefined in either the ClassiCube objects or `math.o`.

So math may be written as ordinary hardware-float C. Fixed-point is not
required and would be a pessimisation.

**Related, and CLAUDE.md is stale on it:** CLAUDE.md states that `sse_save` and
`sse_restore` have zero callers "so FPU state is never saved across a context
switch". The zero-callers half is still true of those two *functions*, but the
conclusion is false. `kernel/proc/context_switch.asm` does `fxsave64` /
`fxrstor64` inline against a per-process `fpu_area` (#446/#588), seeded with
`FCW=0x037F` / `MXCSR=0x1F80` in `process.c`. Ring-3 SSE state is preserved, so
a float-heavy userland app is safe.

## 3. Graphics backend viability, measured

Every upstream platform `.c` is wrapped in an `#if` on a backend id from
`Core.h`, so the entire `src/*.c` wildcard is compiled and non-matching files
collapse to empty translation units. That is why the Makefile has no source
list. Selecting a backend is a `-D`, and the ids are **grepped from `Core.h`,
never guessed**: an earlier pass of this analysis "tested GL1" by passing 1,
which is actually `SOFTGPU`, and produced a clean result that meant nothing.

Real ids: `SOFTGPU 1, GL1 2, GL2 3, D3D9 4, D3D11 5, VULKAN 6, GL11 7,
SOFTMIN 8, SOFTFP 9`.

| Backend      | Compiles | Unresolved beyond the platform layer | Verdict |
|--------------|----------|--------------------------------------|---------|
| SOFTGPU (1)  | 101/101  | 0                                    | **viable, zero extra dependencies** |
| GL1 (2)      | 101/101  | 8, all `GLContext_*`                 | **viable against our existing TinyGL `libgl.a`** |
| GL11 (7)     | 101/101  | 38 `gl*` (display lists, fog) + `GLContext_*` | not viable |
| GL2 (3)      | 101/101  | 49 `gl*` (shader entry points)       | not viable |
| SOFTMIN (8)  | 101/101  | 0                                    | viable |
| SOFTFP (9)   | 100/101  | `Graphics_SoftFP.c` fails: its `limits.h` include_next chain breaks under `-nostdinc` | avoid |

The GL1 result is the notable one: **TinyGL already covers ClassiCube's entire
GL1 feature usage.** Every `gl*` call resolves against `libgl.a`; the only
things left are the eight `GLContext_*` window-glue functions, which the Window
agent has to write for any GL backend anyway. Both SOFTGPU and GL1 are real
options; the Graphics agent chooses.

Default in this Makefile is `GFX_BACKEND=1` (SOFTGPU) because it needs nothing
outside the tree. Override with `make GFX_BACKEND=2`.

## 4. Platform interface still to implement (123 symbols)

`make gap` prints the current list. It is stable across backends apart from the
`GLContext_*` group. Grouped by owner:

* **Platform / process / time / memory** (Platform agent):
  `Platform_Init` group (`Platform_Log`..`Platform_Log4`, `Platform_LogConst`,
  `Platform_Flags`, `Platform_DescribeError`, `Platform_Encrypt`,
  `Platform_Decrypt`, `Platform_EncodePath`, `Platform_DecodePath`,
  `Platform_AppNameSuffix`, `Platform_ReadonlyFilesystem`),
  `Mem_*` (11), `File_*` (11), `Directory_*` (3), `Process_*` (4),
  `DateTime_*` (2), `Stopwatch_*` (3), `Thread_*` (4), `Mutex_*` (3),
  `Waitable_*` (5), `DynamicLib_DescribeError`, `CrashHandler_DumpRegisters`,
  `Updater_*` (6), `ReturnCode_*` (7).
* **Window / input** (Window agent): `Window_*` (19), `WindowInfo`,
  `DisplayInfo`, `Cursor_SetPosition`, `Clipboard_*` (2),
  `OnscreenKeyboard_*` (3), `Gamepads_Process`, and `GLContext_*` (8) if a GL
  backend is chosen.
* **Networking** (Http agent): `Socket_*` (8), `SockAddr_ToString`,
  `DirectUrl_*` (2), `Resume_Parse`. Note that **HTTP itself is portable**:
  `Http_Worker.c` implements the client over `Socket_*` under
  `CC_NET_BACKEND_BUILTIN`, so the Http agent's real job is sockets plus,
  optionally, an SSL backend.
* **Audio** (Audio agent): nothing is currently unresolved, because the default
  is `CC_AUD_BACKEND_NULL` and upstream's `Audio_Null.c` satisfies it. Flip
  `AUD_BACKEND` when a real backend exists.

### The one that is easy to miss: `main()`

`main()` is **not** in the portable core. Upstream defines it inside the
platform file (`Platform_Posix.c`, `Platform_Windows.c`), so it never appears in
the undefined-symbol list above (nothing in the tree references it; only `crt0`
does) and the link fails at the very end with a bare
``undefined reference to `main'``. **`Platform_Maytera.c` must define it**, and
the body is fixed:

    #include "main_impl.h"          /* provides static SetupProgram/RunProgram */

    int main(int argc, char** argv) {
        cc_result res;
        SetupProgram(argc, argv);
        do { res = RunProgram(argc, argv); }
        while (Platform_IsSingleProcess() && Window_Main.Exists);
        Window_Free();
        Process_Exit(res);
        return res;
    }

## 5. Build wiring

* **Install name: `/APPS/CLASSICUBE`.** Derived, not guessed.
  `build/build-golden.sh` resolves an app's `/APPS` entry as: exact match to
  the built BINARY name, else case-insensitive to the binary name, else
  case-insensitive to the source DIRECTORY name, else it creates the entry
  using the binary name. This app is new: the shipped image's `/APPS` has 164
  entries and **none matches `classicube` or `CLASSICUBE` in any case**
  (checked with `debugfs -R "ls /APPS"` against `the golden image` p2;
  `GLCUBE` exists and does not collide). So the fall-through applies and the
  entry is created as the Makefile's `TARGET`, which is `CLASSICUBE`. Changing
  `TARGET` changes the shipped path, and anything that launches the app must
  use the same string. This is the `/APPS/COMPOSIT` vs `COMPOSITOR` failure
  mode, and the reason the name is derived from the binary rather than chosen.
  `/APPS` is on ext2, which is case-sensitive and not limited to 8.3, so the
  10-character name is fine (`ASSAULTCUBE`, 11 characters, already ships).
* PIE, not the old fixed base. `-fPIE` + `-pie --no-dynamic-linker -z notext
  -z max-page-size=0x1000 -T ../../user-pie.ld`, matching `glcube` and
  `assaultcube`. Userland moved to ET_DYN in #640; `-mcmodel=large` is
  consequently not needed.
* Objects go to `build/`, never the app's top level, because `build-golden.sh`
  stages **the first top-level ELF file it finds** in an app directory. Any
  stray ELF at the top level would be shipped as `/APPS/CLASSICUBE`.
* `Core.h` is patched at build time by `engine-patches/coreh-maytera.py`, which
  adds a `PLAT_MAYTERA` branch in upstream's own `#elif` style and one new
  `CC_WIN_BACKEND_MAYTERA` id (8, proven free by parsing the id table rather
  than assuming). It **asserts every anchor** and is idempotent. The assertions
  already earned their keep: they caught that upstream's `Core.h` uses CRLF
  line endings, which would otherwise have produced a silent no-op patch.

### Current build state

`make` currently FAILS at the link step, on the 123 platform symbols plus
`main`. That is the expected state until the other four lanes land, and
`classicube` is listed in `build/userland-expected-fail.allow` with that
reason. **Delete that line in the same commit that makes the port link** - the
gate aborts the build if a listed app builds successfully, so it cannot go
stale.

`make linkcheck` proves the wiring end to end without waiting for them: it
links the real objects against auto-generated stubs and checks the ELF shape.
Measured on both viable backends:

| Backend | rc | Type | LOAD segments | Size |
|---------|----|------|---------------|------|
| SOFTGPU | 0  | DYN (PIE) | `R E` 0xc2ffc + `RW` filesz 0x6dc0 / memsz 0x1f2328 | 3,321,632 |
| GL1     | 0  | DYN (PIE) | `R E` 0xf3b74 + `RW` filesz 0x7a98 / memsz 0x1f3e98 | 4,032,192 |

Both are ET_DYN with the W^X two-segment layout `user-pie.ld` produces, and no
`PT_INTERP`. `.bss` is about 2 MB, comfortably inside the loader's 256 MB
`ELF_USER_IMAGE_MAX_SPAN`, so no large-`.bss` workaround is needed; the 2 MB
user stack (`USER_STACK_SIZE`) and 512 MB max heap (`HEAP_MAX_SIZE`) are the
limits that actually matter for world allocation.

---

## 6. Verification plan: what "ClassiCube boots to its menu on MayteraOS" means

Prose is not evidence. Each level below is a claim with a stated proof, and a
level may only be claimed if its proof was produced against the ARTIFACT.

### L0 - Build integrity (this lane; DONE)
1. `make probe` compiles 101/101 vendor TUs plus every platform `.c` in this
   directory, zero errors.
2. `make gap` reports **0** under "GENUINE libc gap".
3. `make linkcheck` exits 0 and `readelf -h` reports `DYN`.

### L1 - It links and is the right artifact
1. `make` exits 0 with no stubs. **`build/stubs.c` must not exist**; a link
   that only succeeds against stubs is not a port.
2. `readelf -h CLASSICUBE` reports `Type: DYN`, `Machine: X86-64`.
3. `readelf -lW CLASSICUBE` shows no `PT_INTERP`.
4. `nm -u CLASSICUBE` lists no undefined symbol other than
   `_GLOBAL_OFFSET_TABLE_`.
5. Delete the `classicube` line from `build/userland-expected-fail.allow` in
   the same commit.

### L2 - It reaches the image under the right name
1. A golden build logs `/APPS/CLASSICUBE   <- classicube/CLASSICUBE`.
2. `md5sum` of the freshly built binary equals the entry recorded in the
   build's fresh-manifest for `/APPS/CLASSICUBE`, i.e. the launched binary is
   this build. This is the `#537` check, and it is the only defence against the
   `/APPS/COMPOSIT` class of failure.
3. `debugfs -R "ls -l /APPS" <p2>` shows `CLASSICUBE` with a non-zero size.

### L3 - It starts as a process
On a THROWAWAY VM booting the NEW golden (never an old base image: an old
kernel's ELF loader faults on today's PIE binaries and the symptom is
indistinguishable from a compositor hang), launched from the serial console:
1. Serial shows the process spawning and does **not** show a page fault, `#GP`,
   `[WQBLOCK]`, or a kernel panic.
2. The process appears in the task list with a non-zero runtime, i.e. it is
   scheduled, not merely created.
3. It survives 60 seconds without exiting.

### L4 - It boots to its menu (the actual goal)
"Boots to its menu" means the ClassiCube **launcher** screen is drawn and
responsive. Proof, all four required:
1. **Two screendumps at least 5 seconds apart** showing the launcher, plus a
   moving or changing element between them. One screendump of a wedged UI looks
   identical to a live one, which is why one is not enough.
2. Text is legible: the built-in bitmap font renders (this port sets
   `#undef CC_BUILD_FREETYPE`, so `Drawer2D`'s default font is the path
   exercised). A menu of blank boxes is a FAIL, not a cosmetic issue.
3. **Input is proven by a state change, not by a screenshot**: send a keystroke
   over serial and show the resulting screen differs (e.g. a focused field or a
   changed selection). Do not attempt GUI mouse clicks; pointer injection does
   not land reliably (#334).
4. Serial is clean over the whole run: no panic, no `[WQBLOCK]`, no repeated
   fault spam.

### L5 - It is a game, not a menu
1. Singleplayer generates a world and renders it (two screendumps with camera
   movement between them).
2. Frame rate is reported and non-zero.
3. Ten minutes without a crash, and no unbounded memory growth in the task
   list.

### Standing constraints on whoever verifies
* Throwaway VMs only, destroyed with their LVs. Never VM <vmid> or 2281.
* Never test new userland on an old base image.
* Prove claims against the artifact: `nm`, `objdump`, `readelf`, `md5sum`.
  Never `grep` a binary.
