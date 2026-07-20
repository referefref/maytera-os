# Maytera Arena - GoldSrc BSP v30 import, ALL IN RUST (task #491)

Parallel track to the kernel Rust port (#404). Goal: let a user import their own
Half-Life / Counter-Strike 1.6 GoldSrc **BSP v30** maps (and WAD3 textures) into
Maytera Arena, with the parser, collision hull, and entity wiring implemented
**entirely in Rust**, linked into the Arena userland (Ring-3) ELF.

Legal model: MayteraOS ships **no** copyrighted map or texture data. The user
supplies their own `.bsp` / `.wad` files (their own legally-obtained game
content). We ship only the code that reads formats.

---

## Stage 0 - Rust userland bootstrap  [DONE - 2026-07-16, build verified]

Get rustc into the Arena app build reproducibly with **zero functional change**,
proven end to end (the exact userland analog of the kernel port's Phase A #478).

Delivered:
- `arena_rs.rs` - a `#![no_std]`, `panic=abort` staticlib with a
  `#[global_allocator]` wrapping the userland libc heap (malloc/free/realloc),
  a `#[panic_handler]` routing to the userland `abort()` (NOT kernel kpanic),
  and one FFI smoke test `arena_rs_selftest() -> u32`.
- `arena_rs.h` - the C-side declaration + expected magic.
- `rust-toolchain.toml` - pins **rustc 1.97.0** (same pin as the kernel port),
  target `x86_64-unknown-none`.
- `Makefile` - builds `libarena_rs.a` and links it into the Arena ELF, with a
  hard rustc-version gate (fails the build on drift).
- `main.c` - calls `arena_rs_selftest()` once at the top of `main()` and logs
  `[ARENA-RS] selftest=0x%08X ok` to stdout (fd 1 -> serial).

Proven (not assumed):
- Built clean on the userland build container (userland build container). Arena ELF 3,095,944 bytes;
  `arena_rs_selftest` defined at `0x80066ef0`; the Rust object leaves
  malloc/free/realloc/memcpy/abort undefined-referenced (resolved by libc).
- Ran in a VM off a copy of the golden b741 image: the selftest printed
  `[ARENA-RS] selftest=0xA55AE73B ok` (magic matched, so no_std + alloc::Vec +
  bounds-checked slice + FFI all executed in Ring-3), and Arena launched,
  rendered its main menu AND the in-game 3D "The Longest Yard" level normally,
  running stably (no crash) past 3.5 minutes uptime.
- No regression: the pre-Rust baseline ELF behaves identically (on a mismatched
  kernel both the baseline and the Rust build hit the same pre-existing Arena
  crash; on the matched golden kernel both render + play).

### The userland-Rust build recipe (reuse this for Stages 1-3)

Target: builtin `x86_64-unknown-none` (ships precompiled `core` + `alloc`, so NO
`-Zbuild-std` needed), with the code/relocation model OVERRIDDEN to match the
Arena C ABI, which is `-mcmodel=large -fno-pic -fno-pie`, statically linked at
**0x80000000** by `user.ld`:

```
rustc --edition 2021 --crate-type staticlib --target x86_64-unknown-none \
      -C opt-level=2 -C panic=abort \
      -C code-model=large -C relocation-model=static \
      -o libarena_rs.a arena_rs.rs
```

Why each flag:
- `-C code-model=large` <-> `-mcmodel=large`. The app links at 0x80000000. The
  default/small model emits `R_X86_64_32S` (signed 32-bit) relocations that
  OVERFLOW at 2^31 -> `relocation truncated to fit R_X86_64_32S`. Large uses
  movabs / `R_X86_64_64`, exactly like the C build. This is the single most
  important userland-vs-kernel difference (the kernel port needs no override
  because `x86_64-unknown-none` already matches the kernel ABI).
- `-C relocation-model=static` <-> `-fno-pic/-fno-pie` (static link, no GOT/PLT).
- `-C panic=abort` - no unwinding/eh_personality; panic -> userland `abort()`.

Allocator: `#[global_allocator]` wraps libc `malloc`/`free`/`realloc` so Rust and
C share ONE heap. malloc guarantees 16-byte alignment (stdlib.c), which covers
`Vec`/`Box` of the scalar/geometry types; the allocator has an over-alignment
fallback for align > 16 (store-base-pointer trick).

Link: `libarena_rs.a` goes AFTER the C objects (which reference the Rust symbol)
and BEFORE libgl/libc, so ld pulls the Rust member on demand and then resolves
its malloc/memcpy references against libc (last). A real C -> Rust call pulls the
member; no `-u` force-retain needed.

**Float-model caveat for Stage 1 (IMPORTANT, currently DEFERRED):** the
precompiled `core` for `x86_64-unknown-none` is **soft-float**, while the Arena C
is `-msse -msse2` (hardware float). Stage 0 crosses the FFI boundary with
INTEGERS ONLY (u32), so the ABIs agree and this is a non-issue. Stage 1 passes
`f32` map geometry across FFI, where soft-float (floats in integer regs) vs
hardware-float (floats in xmm) DISAGREE. Before Stage 1 ships, pick one:
  (a) keep the FFI integer / bit-pattern only (pass f32 as `u32` bit patterns,
      or write geometry into a caller-provided `#[repr(C)]` buffer of ints), OR
  (b) move to a **custom target JSON + `-Zbuild-std=core,alloc`** (needs the
      `rust-src` component; `RUSTC_BOOTSTRAP=1` on the pin) compiled with
      `+sse,+sse2` and `code-model=large`, so `core`/`alloc` and our crate all
      use the hardware-float ABI that matches TinyGL and the C engine.
Option (a) is lower-risk and recommended for the parser; option (b) if we ever
want to hand f32 arrays back and forth freely.

---

## Arena engine facts (the target the BSP data must fill)

Verified from `game.h` / `world.c` / `render.c`:
- **World = a set of convex axis-aligned boxes ("brushes").**
  `Brush { vec3 mins, maxs; uint32_t rgb; int is_floor; int mat; }`, `mins<maxs`.
- **Cap: `MAX_BRUSHES` = 256** boxes per level (`Level.brushes[256]`, `nbrush`).
  `MAX_ENTITIES` = 64 (players + bots + projectiles + items).
- **`Level` is data-driven:** `brushes[]/nbrush`, `spawns[]`, `items[]`,
  `props[]`, `world_mins/world_maxs` (bounds for clamp/fog). Building a level =
  filling this struct (see `levels.c`). A BSP importer just needs to POPULATE a
  `Level` (or a superset) from parsed BSP data.
- **Coordinate space: z-up**, roughly Quake units (entity "feet at pos.z bottom
  of bbox"). GoldSrc/Quake BSP is also z-up in Quake units, so the axis mapping
  is near-identity (scale/clamp to `world_mins/maxs` and the 256-brush budget).
- **Rendering: TinyGL, 6 quads per box** (`render.c` face path); textures via the
  per-level material set (`MAT_THEME` + others).
- **Collision: swept-AABB** move/slide/trace against the brush set + entities.

Implication: the axis-aligned-box world does NOT natively consume arbitrary BSP
polygon soup. Stage 1/2 must reduce GoldSrc geometry to something this engine can
render+collide, OR extend the engine with a Rust-provided face/hull path.

---

## Stage 1 - Rust BSP v30 parser + WAD3 textures + render feed  [DONE - 2026-07-16, offline-verified; VM render BLOCKED by image launch infra, see below]

Chose render option (b): a Rust-provided face list + a NEW TinyGL draw path
(`bsp_draw_faces()`), parallel to `draw_brushes`, that does NOT touch the
256-`Brush` cap. Boxes remain for the built-in levels (Stage 2 adds hull-trace
collision for imported maps).

Delivered:
- `bsp.rs` - a `mod bsp;` inside the `arena_rs` crate. Parses the BSP30 header
  (version==30) + 15-lump directory, then VERTEXES, EDGES, SURFEDGES, FACES,
  TEXINFO, TEXTURES(miptex), MODELS, and the ENTITIES text (exposed verbatim).
  Reconstructs each face polygon by walking surfedges -> edges -> vertices in
  winding order (renders MODEL 0's faces = the static world). Decodes embedded
  miptex mip-0 + palette to ARGB, and looks up external textures by name in a
  WAD3 blob. Exposes `#[repr(C)]` `BspScene`/`BspFace`/`BspTexture`/`BspVec3`
  via `bsp_parse()` / `bsp_free()`, with sizes locked by const_assert (Rust) +
  `_Static_assert` (C, in `bsp_load.h`).
- **FFI is INTEGER / BIT-PATTERN ONLY** (Stage 0 float caveat, option (a)): the
  Rust side does NO f32 arithmetic. Vertex coords and the texinfo vecs[2][4]
  cross the ABI as raw u32 IEEE-754 bit patterns; the C side (`bsp_load.c`
  `f32b()`) reinterprets them and computes per-vertex UVs with hardware SSE.
  Palette-index -> ARGB is pure integer, so texture decode stays in Rust.
- **Untrusted-file crash-safety** (the whole point of doing this in Rust): every
  lump offset+length is bounds-checked against the file size, and every edge /
  vertex / surfedge / texinfo / miptex index against its lump count, with
  `checked_add`/`checked_mul` and `slice::get()` (never a raw index on
  untrusted input). A malformed/crafted/truncated `.bsp` returns a clean
  `BspScene.error != 0` and zero counts - NEVER an out-of-bounds access. This
  is the userland analog of the kernel port's #476 ext2 lesson.
- `bsp_load.c` / `bsp_load.h` - C integration: reads the `.bsp` (+ optional
  external `.wad`) from disk, calls the Rust parser, uploads each decoded
  texture through the existing polish `img_bind_gl` path, computes UVs, and
  `bsp_draw_faces()` draws the faces as textured `GL_TRIANGLES` (cull disabled).
  `bsp_get_spawn/bounds`, entity-text `info_player_*` spawn parse (C-side).
- `main.c` / `physics.c` / `render.c` - a `/ARENA/MAP.BSP` auto-load at startup
  (+ optional `/ARENA/MAP.WAD`); press **B** from the menu/pause to enter the
  map as a noclip free-fly world (`g_arena_noclip`, phys_step free-fly branch;
  no collision this stage). `render.c` calls `bsp_draw_faces()` when a BSP is
  active. Box levels are the default and untouched (no regression). A
  bounded (`sys_sleep`, #426-safe) win_create retry handles a boot-launch race.

Proven OFFLINE on the userland build container (the REAL compiled Rust parser):
- Synthetic copyright-clean fixture `bsp_test/gen_fixture.py` -> `room.bsp`
  (a 16-textured cube room, embedded miptex + palette, info_player_start).
- Known-answer harness (`bsp_test/bsp_test.c`, linked against the identical
  Rust source built for the host target, +UBSan): 19/19 asserts PASS -
  num_verts=24, num_faces=6, num_textures=1, exact face-0 polygon coords, exact
  UVs (C-side from the bit patterns), exact decoded pixels (0xFF283CC8 /
  0xFFC82828), entity text exposed. UBSan clean.
- **Crash-safety proof**: `bsp_test/bsp_guard.c` places every fuzz input against
  a `PROT_NONE` hardware guard page (Electric-Fence style) so any read past the
  file end faults instantly. **9687 vectors** (truncations at every length,
  hostile lump ofs/len pokes, full byte-flip sweep x4 patterns) executed with
  **0 over-read faults = 0 OOB**. (ASan-instrumented linking of the no_std
  staticlib hit a runtime-ordering incompatibility with the precompiled
  `alloc` rlib; the hardware guard page is a stronger, tooling-free substitute
  for the over-read class, run on the real shipping-source parser.)
- Arena ELF builds clean on the userland build container (version-gated rustc 1.97.0): 3,136,192 bytes,
  symbols `bsp_parse`/`bsp_free`/`bsp_load_file`/`bsp_draw_faces`/
  `arena_start_bsp` all present; structurally identical single RWE PT_LOAD to
  the shipping Arena (loadable).

VM render: **UNVERIFIED / BLOCKED (honest)**. On a throwaway VM cloned from the
golden b738 (kernel b741) image, the imported map could not be launched to a
window: `sys_exec`-based launch (both `/CONFIG/AUTORUN.CFG` and the remote shell
`launch`) does NOT spawn GUI apps on this image - confirmed by the known-good
**Stage-0 ARENA and the shipped DOOM failing identically** (no `/ARENA/KEYLOG.TXT`
= never reached `win_create`). Only the compositor's start-menu/desktop-icon
`sys_spawn` reliably gives an app a window, and that needs HID input the headless
VM cannot receive (no USB HID enumerated; QEMU `sendkey`/`mouse_move` produced no
observable effect). This image's kernel also carries the parallel kernel-port
agent's WIP Rust changes ([RUST-SEC]/[RUST-DIFF] in BOOTLOG). So the render path
is compiled+wired but not yet screenshot-proven on a VM; this is a
launch/input-infra gap, NOT a demonstrated defect in the Stage-1 code.

## Stage 2 - Rust hull-trace collision  [TODO]

- Use the BSP **clipnodes / hulls** (hull 0 point, hull 1/2/3 player boxes) for
  real map collision via a Rust `hull_trace` (recursive plane split), replacing
  or augmenting the swept-AABB-vs-boxes path for imported maps.
- Keep Arena's existing swept-AABB for the built-in box levels (drop-in, no
  regression). Imported maps opt into the hull trace.
- Prove: player + bots walk/collide correctly on an imported map; no fall-through.

## Stage 3 - entities / bots on imported maps  [TODO]

- Parse the BSP `entities` lump (info_player_deathmatch spawns, item_*, light,
  func_* ignored or approximated) in Rust; map to Arena `Spawn`/`ItemDef`.
- Place bots + items from the map's own entity data; wire nav to the imported
  geometry so bots path on real maps.
- Prove: a full deathmatch runs on a user-imported CS 1.6 / HL map with bots.

---

## Reproduce the Stage 0 build

```
# on the userland build container, rustc 1.97.0 + x86_64-unknown-none:
cd userland/apps/arena
make            # builds libarena_rs.a (version-gated) then links ARENA
nm ARENA | grep arena_rs_selftest     # -> defined
```

Reproduce the Stage 1 offline proof (the userland build container):
```
cd bsp_test
python3 gen_fixture.py room.bsp
# identical Rust source built for the host target so it links with gcc:
rustc --edition 2021 --crate-type staticlib --target x86_64-unknown-linux-gnu \
      -C panic=abort -C opt-level=2 -o libarena_rs_host.a ../arena_rs.rs
gcc -g -O1 -fsanitize=undefined bsp_test.c  libarena_rs_host.a -o bsp_test  -lm  # known-answer + UBSan
gcc -g -O1                       bsp_guard.c libarena_rs_host.a -o bsp_guard -lm  # guard-page OOB proof
./bsp_test room.bsp && ./bsp_guard room.bsp
```

---

## Stage-1 changelog (repo CHANGELOG.md intentionally SKIPPED this run to avoid
## contention with the parallel kernel-port agent; recorded here instead)

- 2026-07-16 #491 Stage 1: GoldSrc BSP v30 parser + WAD3/miptex decoder ALL IN
  RUST (`bsp.rs`, a `mod bsp;` in the `arena_rs` crate), integer/bit-pattern-only
  FFI, exhaustive untrusted-input bounds checking (clean-error, never OOB). C
  integration (`bsp_load.c/.h`): texture upload via `img_bind_gl`, C-side UV
  compute, `bsp_draw_faces()` new TinyGL face path. `main.c`/`physics.c`/
  `render.c`: `/ARENA/MAP.BSP` load, press-B noclip free-fly, bounded win_create
  retry. Offline-verified on the userland build container: known-answer 19/19 PASS + UBSan clean +
  9687-vector hardware-guard-page proof (0 OOB). ARENA ELF 3,136,192 B with the
  bsp symbols. VM render UNVERIFIED (image `sys_exec` launch does not spawn GUI
  apps - Stage-0 ARENA + DOOM fail identically; input not injectable headlessly).

---

## Stage-1 pitfalls (blame.md-adjacent; recorded here to avoid CHANGELOG/blame
## contention with the kernel-port agent)

- **No f32 across the Rust<->C FFI.** The precompiled `core` for
  x86_64-unknown-none is soft-float; the Arena C is `-msse`. Passing an `f32`
  arg/return across the boundary would mismatch the ABI (xmm vs integer reg).
  Keep it integer/bit-pattern only: read f32s as raw `u32`, do ALL float math in
  C. Do not "just multiply UVs in Rust" - that reintroduces the soft-float ABI.
- **Untrusted `.bsp` = bounds-check everything.** Use `slice::get()` +
  `checked_add`/`checked_mul` on EVERY offset and EVERY index (edge/vertex/
  surfedge/texinfo/miptex). A raw `d[o]` or `arr[i]` on attacker-controlled
  input is a panic (abort) at best; funnel to a clean `error` code instead.
- **GoldSrc embedded-miptex palette location:** it is AFTER the 4 mips at
  `miptex + offsets[3] + (w/8)*(h/8)`, preceded by a 2-byte colour count, then
  256*3 RGB. `offsets[0]==0` means the pixels live in an EXTERNAL WAD3 (look up
  by the 16-byte name).
- **ASan cannot cleanly link a `#![no_std]` staticlib + precompiled `alloc`
  rlib** (runtime-ordering DEADLYSIGNAL, even with `-static-libasan` /
  `verify_asan_link_order=0`). For the over-read class, a PROT_NONE guard page
  after the input buffer is a tooling-free, hardware-enforced substitute that
  runs on the real shipping-source parser.
- **Do not put test `.c` in the arena app dir** - the Makefile globs `*.c` into
  ARENA. Keep the harness in `bsp_test/` (a subdir, not globbed).
- **This golden b738 image cannot launch GUI apps via `sys_exec`** (autorun /
  remote-shell `launch`): the window never gets created (no `/ARENA/KEYLOG.TXT`).
  Confirmed with Stage-0 ARENA AND shipped DOOM. Only the compositor's
  start-menu/icon `sys_spawn` works, and QEMU `sendkey`/`mouse_move` do not
  reach this image's compositor (no USB HID enumerated). Budget for this if a
  future stage needs a headless in-VM GUI screenshot: a matched
  compositor-launch path or working HID injection is required first.
