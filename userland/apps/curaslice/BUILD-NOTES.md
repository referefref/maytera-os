# curaslice build notes

curaslice is the CuraEngine 15.04 (legacy) slicer ported to the MayteraOS
freestanding userland. It is the first C++ application in the tree. This file is
the build and iteration procedure for Phase P1 (cross-compile to a working ELF).

Writing-style rule: no em-dashes anywhere in this repo's generated docs.

## Where to build

C++ builds, like all kernel/userland builds, happen on the remote build server,
NOT locally. The userland libc archive (`../../libc/libc.a`) and `crt0.o` are
produced there by the libc Makefile; they do not exist in a fresh checkout. Sync
this `curaslice/` directory (and the `../../libc`, `../../user.ld` it references)
to the build host, then build there with gcc/g++ 12.

Do a first cross-compile and a host-side run/diff on the build server BEFORE
going on-device (plan P1 step 5).

## Build command

From `userland/apps/curaslice/` on the build server:

```
make clean && make
```

That compiles 23 translation units with `g++` and links a single `curaslice`
ELF with `ld -nostdlib -T ../../user.ld`. Key flags (see the Makefile and
CURAENGINE_PORT_PLAN.md section 3.1):

- `-std=gnu++11 -ffreestanding -fno-exceptions -fno-rtti`
- `-fno-threadsafe-statics -fno-stack-protector -fno-pic -mno-red-zone`
- `-mcmodel=large -fno-builtin -O2 -g`
- `-I../../libc -Ilibs -Isrc`
- NO `-nostdinc` and NO `-nostdinc++`: the libstdc++ template headers
  (`<vector>`, `<algorithm>`, `<map>`, `<set>`, `<limits>`, `<cmath>`) MUST stay
  on the include path. `-I../../libc` is first so `<stdio.h>`, `<string.h>`,
  `<math.h>` resolve to the MayteraOS libc rather than glibc.
- `-nostdlib` is a LINK flag only.

If `gcc .../11/include` shows up in the compile lines you are building on the
wrong (local) machine; the build server is gcc-12. Stop and fix that first.

## Iteration protocol (the core P1 loop)

The C++ ABI shim in `cxxsupp.cpp` is a STARTING set, not a proven-complete one.
Expect the first link to fail with undefined symbols. The loop is:

1. `make` and read the `ld` "undefined reference to ..." list.
2. For each symbol decide which bucket it is:
   - `operator new`/`operator delete` variant: already covered; if a new
     mangling appears (e.g. aligned new `_ZnwmSt11align_val_t`), add it to the
     new/delete block in `cxxsupp.cpp`.
   - `std::__throw_*` / `std::terminate` / `__gnu_cxx::__verbose_terminate_handler`:
     add the missing one to the `__throw_*` block, matching the
     `<bits/functexcept.h>` signature, routing through `curaslice_die`.
   - `__cxa_*` (`__cxa_pure_virtual`, `__cxa_atexit`, `__cxa_guard_*`,
     `__cxa_throw`, `__cxa_begin_catch`): add an abort-style or no-op stub. Note
     `__cxa_guard_*` should NOT appear because `-fno-threadsafe-statics` is set;
     if it does, confirm that flag reached every object.
   - A libc function genuinely missing (e.g. a math or string routine): prefer
     adding it to the shared libc (`../../libc`) so every app benefits, per the
     repo "reuse the shared primitives" rule. Do not fork a private copy.
   - A libstdc++ runtime symbol that implies a banned facility leaked in
     (`std::ios_base::Init`, `std::basic_string<...>`, locale, `std::__cxx11`):
     do NOT stub it. Find the `.cpp` that pulled in `<iostream>`/`<sstream>`/
     `<fstream>` or an out-of-line `std::string`, and remove/replace that usage
     (this is the mstring / banned-header policy). A `basic_string` symbol means
     an `std::string` slipped past the `pstring` substitution.
3. Rebuild. Repeat until zero undefined symbols.
4. Record every symbol you added and why in `PORT-STATUS.md`.

## Compile-time (not link-time) risks to watch on the first pass

- `<cmath>`/`<cctype>` over the MayteraOS libc `math.h`/`ctype.h`: libstdc++'s
  `<cmath>` expects a glibc-shaped `math.h` and does `using ::fn`. If libc lacks
  a declaration `<cmath>` references, compilation fails inside the header. Fix by
  adding the missing prototype to libc `math.h`, not by hacking `<cmath>`.
- Host headers pulled by `main.cpp`'s `__linux__` block (`<execinfo.h>`,
  `<sys/resource.h>`) and by `modelFile.cpp` (`<strings.h>`): these are
  include-only. The only missing SYMBOL is `setpriority`, stubbed in
  `cxxsupp.cpp`. `backtrace` is not called.

## On-device proof (P1 step 5)

Slice a small cube STL (a few KB, under the #416 1 MB read cap) to `out.gcode`
on device via `mdev pushrun`, then bytewise-diff the first N layers against host
CuraEngine 15.04 run with the same `-s` settings. Identical int64-micron math
should match closely; pin `%f` precision if float formatting diverges.

## CLI contract (unchanged from upstream)

```
curaslice [-v] [-p] [-s key=value]... -o out.gcode model.stl
```

`-c <configfile>` is accepted but is now a no-op (config-file reading is
disabled; see PORT-STATUS.md). Supply settings with `-s` or the built-in preset
in `presets.h`.
