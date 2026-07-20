# curaslice port status (Phase P1: vendoring + toolchain)

Port of CuraEngine 15.04 (legacy) into the MayteraOS freestanding userland as
`userland/apps/curaslice`. This document records what was vendored, every local
modification to upstream files, and open risks. See CURAENGINE_PORT_PLAN.md for
the overall plan and BUILD-NOTES.md for how to build and iterate.

Writing-style rule: no em-dashes anywhere in this repo's generated docs.

## Status

- P1 vendoring and C++ toolchain scaffolding: DONE (this change).
- Building: NOT attempted here. Builds happen on the remote build server where
  `../../libc/libc.a` and `crt0.o` exist. Expect an undefined-symbol iteration
  loop against `cxxsupp.cpp` (BUILD-NOTES.md).

## What was vendored

Source: `git clone --depth 1 -b legacy https://github.com/Ultimaker/CuraEngine`
(branch `legacy`, commit aca2c17). Exactly the CURAENGINE_PORT_PLAN.md section 6
file list, layout preserved:

- `src/` (23 files): main.cpp, fffProcessor.h, settings.cpp/.h, slicer.cpp/.h,
  layerPart.cpp/.h, inset.cpp/.h, skin.cpp/.h, infill.cpp/.h, skirt.cpp/.h,
  raft.cpp/.h, support.cpp/.h, bridge.cpp/.h, comb.cpp/.h,
  pathOrderOptimizer.cpp/.h, polygonOptimizer.cpp/.h, optimizedModel.cpp/.h,
  sliceDataStorage.h, multiVolumes.h, gcodeExport.cpp/.h, timeEstimate.cpp/.h.
- `src/modelFile/` (2): modelFile.cpp/.h (binary + ASCII STL loader).
- `src/utils/` (12): intpoint.h, floatpoint.h, polygon.h, polygondebug.h,
  string.h, gettime.cpp/.h, logoutput.cpp/.h, socket.cpp/.h (STUBBED).
- `libs/clipper/` (2): clipper.cpp, clipper.hpp (sole third-party dep, bundled).

Total vendored C++ files: 51. NOT vendored (per plan): Conan/Arcus/protobuf/
CMake, front-end comms, tests.

License: upstream `LICENSE` (GNU AGPLv3) copied verbatim to `LICENSE.curaengine`.
CuraEngine 15.04 is AGPLv3; the bundled Clipper library carries its own Boost-
style license inside `libs/clipper` upstream (not vendored into this subset since
plan section 6 lists only clipper.cpp/.hpp; the clipper source headers retain
their in-file copyright notices). Any MayteraOS distribution that ships curaslice
must honor AGPLv3 for the CuraEngine portion.

## New MayteraOS files (not from upstream)

- `cxxsupp.cpp` - C++ runtime shim: operator new/new[]/delete/delete[] (+ sized +
  nothrow) onto libc malloc/free; `__cxa_pure_virtual`, `__cxa_atexit` (no-op
  returns 0), `__dso_handle`; `std::terminate`,
  `__gnu_cxx::__verbose_terminate_handler`; the `std::__throw_*` family
  (length_error, out_of_range, out_of_range_fmt, bad_alloc, logic_error,
  bad_function_call, invalid_argument, runtime_error, bad_cast); a `setpriority`
  no-op. All fatal paths route through `curaslice_die()` which does libc `printf`
  then `sys_exit(1)`.
- `mstring.h` - header-only malloc-backed `std::string` replacement class
  `mstring`, implementing exactly the surface the vendored code uses (ctors,
  operator=, c_str/data, size/length, empty, clear, operator[], append,
  operator+= / operator+, operator== / !=, find, find_first_of, substr, erase,
  static npos).
- `platform.h` - pulls MayteraOS libc headers, includes `mstring.h`, defines
  `typedef mstring pstring;` and an M_PI fallback. Included by the files that use
  strings.
- `presets.h` - built-in fixed profile: 0.2 mm layers, 2 walls, 20 percent grid
  infill, no support, expressed as `key=value` strings for
  `ConfigSettings::setSetting`.
- `Makefile` - g++ C++ build per plan 3.1; keeps libstdc++ headers, links
  `-nostdlib -T ../../user.ld` with crt0 + app objects + libc.a.
- `BUILD-NOTES.md`, `PORT-STATUS.md` - this documentation.

## Local modifications to upstream files (file by file)

Every upstream file is byte-identical to the `legacy` checkout EXCEPT:

1. `src/utils/socket.h` - REWRITTEN as a no-op stub interface. Removed
   `#include <string>`; `connectTo` now takes `const char*` instead of
   `std::string`. Reason: CLI-only, no networking on freestanding userland
   (plan 3.5). Class shape (method names/order) preserved so callers in
   `fffProcessor.h` still compile.

2. `src/utils/socket.cpp` - REWRITTEN as no-op stubs. All BSD-socket code
   removed; sends drop, `recvNr` returns 0, `recvAll` zero-fills. Reason: same
   as above. The original networking implementation remains in the scratchpad
   upstream clone for reference.

3. `src/main.cpp` - three changes:
   - Added `#include "../platform.h"`.
   - `std::string` -> `pstring` (two `std::vector<std::string>` sites).
   - Removed the three `try { ... } catch(...) { logError; exit(1); }` guards,
     leaving the bodies inline. Reason: `-fno-exceptions` makes try/catch a
     compile error; ClipperLib throw sites are compiled out and, if reached,
     abort via the `__throw_*` stubs. The CLI contract is unchanged.

4. `src/settings.h` - added `#include "../platform.h"`; `std::string` ->
   `pstring` (four member declarations).

5. `src/settings.cpp` - changes:
   - Added `#include "../platform.h"`; removed `#include <fstream>`.
   - `std::string` -> `pstring` throughout.
   - `readSettings(const char* path)` body REPLACED with `(void)path; return
     false;`. Reason: the original used `std::ifstream` + `std::getline`
     (`<fstream>`), which drags libstdc++ iostream/locale static init that
     cannot link `-nostdlib`. Config-file reading is disabled; settings come from
     `presets.h` and `-s` CLI args. The now-unused LTRIM/RTRIM macros and
     `CONFIG_MULTILINE_SEPARATOR` are left in place (harmless).

6. `src/gcodeExport.h` - added `#include "../platform.h"`; `std::string` ->
   `pstring` (member + method signature).

7. `src/gcodeExport.cpp` - added `#include "../platform.h"`; `std::string` ->
   `pstring` (setSwitchExtruderCode parameters).

8. `src/fffProcessor.h` - added `#include "../platform.h"`; `std::string` ->
   `pstring` (two `std::vector<std::string>` parameter types).

NOT modified (byte-identical), notably:
- `libs/clipper/clipper.cpp` and `clipper.hpp` - untouched. `clipper.hpp` still
  declares `clipperException : std::exception` with a `std::string m_descr` and
  three `std::ostream& operator<<` DECLARATIONS. These are never instantiated or
  called in the vendored subset, so no libstdc++ string/ostream symbols are
  referenced. The `throw()` exception specifications compile fine under
  `-fno-exceptions`. `<ostream>` is header-only (it does not create the iostream
  static initializer that `<iostream>` does), so it stays.
- All geometry/slicing sources (slicer, inset, skin, infill, comb, etc.) and the
  STL loader `modelFile.cpp` are unmodified. `modelFile.cpp` still `#include
  <strings.h>` (host header, harmless: it calls `stringcasecompare` from
  `utils/string.h`, not `strcasecmp`).

## Open risks (biggest first)

1. `<cmath>`/`<cctype>` over MayteraOS libc headers (COMPILE risk). libstdc++'s
   `<cmath>` does `using ::sqrt`/`::atan2`/etc. against whatever `math.h` is on
   the path, which is our libc `math.h`. If any declaration `<cmath>` expects is
   absent, compilation fails inside the header. `intpoint.h` uses `std::atan2`;
   `skin/support/gcodeExport` use `cos/tan/M_PI`. First build will reveal gaps;
   fix by extending libc `math.h`, not by editing `<cmath>`.

2. C++ ABI link gaps (LINK risk). `cxxsupp.cpp` is a starting set. The first link
   will almost certainly report additional undefined symbols (extra new/delete
   manglings, a `__throw_*` not yet listed). This is the expected P1 loop; drive
   it from the undefined-symbol list (BUILD-NOTES.md).

3. libstdc++ string/iostream leak (LINK risk). If any `std::basic_string`,
   `std::__cxx11`, `std::ios_base::Init`, or locale symbol appears undefined, an
   `std::string` or a banned `<iostream>`/`<sstream>`/`<fstream>` slipped past
   the substitution. Do not stub these; fix the offending source.

4. `-mcmodel=large` coverage and big C++ statics/vtables (blame #444). The flag
   is on every object incl. clipper. Watch for `R_X86_64_32S` relocation-
   truncated errors at link; if seen, confirm no object was compiled without it.

5. Global C++ constructors. The vendored subset uses only POD-initialized
   statics (`ConfigSettings::config = NULL`, `binaryMeshBlob = nullptr`), so no
   `.init_array` run is required. If future code adds a global object with a
   non-trivial constructor, verify crt0 runs `.init_array`, or the ctor will not
   fire.

6. Memory footprint and recursion (RUNTIME, later phases). 256 MB demand-paged
   heap; deep walks in optimizedModel/comb/Clipper. Verify the pthread/worker
   stack size before slicing large models (plan risks 4, 5).

7. Float determinism vs host CuraEngine (RUNTIME). g-code number formatting uses
   `sprintf("%f")` via libc `vsnprintf`; diff early and pin precision (plan
   risk 9).
