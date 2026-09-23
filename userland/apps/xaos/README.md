# XaoS port (#745, docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md Tier 3 #16)

XaoS is a real-time fractal zoomer, GPL-2.0-or-later. Pinned to upstream tag
`release-3.6` (github.com/xaos-project/XaoS), sha256
`857c4ec71d0a25075a7af6255f5c36515d4d330f074f5aaf4da6fbe410e4e1f8` for
`XaoS-release-3.6.tar.gz`. See ATTRIBUTION.md for the full licence writeup and
why 3.6 (not the GTK-only 4.3.x line) was chosen.

## Why a userland app, not an mports recipe

`userland/ports/` (see `docs/MPORTS.md`) exists for LIBRARIES another app
links against; the two owner rules there are "never reinvent a wheel" and
"the adoption test: does this thing want to USE a platform layer, or BE
one?" XaoS fails the adoption test on purpose - it wants to own `main()`,
the event loop and the window, exactly like DOOM. So this port follows the
DOOM precedent literally: vendored upstream source under `src/`, built by
this directory's own `Makefile` into `/APPS/XAOS`, no mports recipe at all.

## The one file that is genuinely new: the platform driver

XaoS 3.6 already has a pluggable driver abstraction
(`src/include/ui.h`'s `struct ui_driver`, the same seam upstream's own X11,
GTK, SVGAlib, win32 and BeOS ports each implement) plus a worked example at
`src/ui/ui-drv/template/ui_template.c`. `src/ui/ui-drv/maytera/ui_maytera.c`
implements that same contract against MayteraOS's compositor: `win_create`/
`win_destroy` for the window, `SYS_WIN_BLIT` (syscall 35, called directly -
no higher-level wrapper exists in userland/libc yet, same as
`userland/apps/doom/i_video.c`) to present each rendered frame, and
`win_get_event`/`gui_event_t` for input. It was modelled most closely on
upstream's own `src/ui/ui-drv/win32/ui_win32.c` - a single bitmap-blit
true-colour window is the closest of upstream's seven drivers to what a
compositor window actually is.

**Keyboard-only zoom/pan, for verification.** Real XaoS is mouse-first (hold
left button to zoom in toward the cursor, right button to zoom out, drag to
pan) - `BUTTON1`/`BUTTON2`/`BUTTON3` (`ui.h`) are exactly the bits
`ui_mouse()` (`src/ui/ui.c`) reads every frame regardless of where they come
from. `ui_maytera.c` tracks a synthetic cursor position driven by the arrow
keys and synthesizes `BUTTON1`/`BUTTON3` while 'Z'/'X' are held, so the same
mouse-zoom code path upstream already has is reachable with no real mouse -
useful in general (this OS's compositor sends keyboard to the focused
window and mouse to whatever is under the pointer) and specifically because
this project's real-mouse injection in test VMs is unreliable (#334).

## What is vendored, what is excluded, and the two patches

`src/{engine,filter,ui,ui-hlp,util,include}` is upstream source, copied over
UNMODIFIED except:

- `src/ui/drivers.c`: one hunk registering `maytera_driver`, exactly like
  every other `*_DRIVER` already in that file.
- `src/util/timers.c`: one `#ifndef USE_CLOCK` line. The file's own
  `#error "I am unable to get time in milisecond..."` guard predates its own
  `USE_CLOCK` timing path (the one this port uses, since userland/libc has
  `clock()`/`CLOCKS_PER_SEC` but no `gettimeofday()`/`uclock()`) and never
  learned about it; this line teaches it.

Excluded entirely:

- `sffe/` (Malczak's optional textual-formula parser add-on). `SFFE_USING`
  is never defined anywhere in this port, so XaoS's normal built-in formula
  table runs (including the default Mandelbrot `z^2+c`); sffe is opt-in
  upstream too (needs `--enable-sffe`).
- `util/png.c` (needs libpng/zlib, no mports recipe for either yet). Nothing
  on the render path needs it; only "Save image"/"Render animation" do, and
  `xaos_compat.c`'s stub `writepng()` fails those cleanly instead of link
  error or crash.
- The `catalogs/`, `help/` and `tutorial/` upstream data trees. XaoS
  degrades gracefully with none of them present (`util/xstdio.c`'s
  `xio_get*` search functions return "not found", not a crash), and nothing
  on the render/zoom path reads them.

`src/include/config.h`, `src/include/aconfig.h` and `src/include/version.h`
are hand-written, in place of the autoconf-generated files a real
`./configure` would produce (there is no configure step in this build).
`config.h` is modelled on upstream's own `src/include/config/config.autoconf`
(a real Linux build's generated output, checked into the upstream tree as a
reference) rather than the older `config.std` fallback, because `config.std`
is missing `INLINE`/`CONST`, which `src/engine/formulas.c` needs
unconditionally.

## libc adaptation (the hazard this task called out)

No `userland/libc` header was touched. `xaos_compat.c` supplies three
`#pragma weak` functions the vendored source calls that userland/libc does
not provide: `putc` (userland/libc/stdio.h has `fputc` but not the
C-standard-required `putc` alias), `sincos` (a GNU libm extension; plain
`sin()`+`cos()` calls are correctness-preserving), and a stub `writepng`
(see above). `#pragma weak` means a future real userland/libc definition
of any of these silently wins instead of colliding. `usleep()` is
deliberately NOT here: `userland/libc/unistd.c` already has a real one
(checked with `nm` against `libc.a`, not assumed - see the comment at the
top of `xaos_compat.c`), so `HAVE_USLEEP` in `aconfig.h` is defined for that
reason. `nm -u xaos_compat.o` shows only libc's own `cos`/`sin`/`fputc`/
`__stack_chk_fail`/`__stack_chk_guard` as its remaining undefined symbols,
none of which is a `types.h`-leak risk (they are plain `double`/`FILE*`/`int`
signatures, no legacy compat typedefs involved).

Also added: `src/include/malloc.h` (forwards to `<stdlib.h>`; ~20 upstream
files `#include <malloc.h>`, a glibc-ism userland/libc does not have, since
it declares malloc/free in `<stdlib.h>` per ISO C placement).

MayteraOS userland is hardware-float (SSE2); nothing in this port needed
fixed-point, and `FPOINT_TYPE` is plain `double` (see `config.h`'s note -
userland/libc/math.h has no long-double function family, so
`HAVE_LONG_DOUBLE` is left undefined and XaoS's own config logic drops
`FPOINT_TYPE` from `long double` to `double` exactly as it would on any
platform lacking that feature).

## Build

```
cd userland/apps/xaos && make
```

Produces `XAOS.ELF`, statically linked (`static-pie`, zero dynamic symbol
imports - confirmed with `readelf -d` and `nm -D`), against
`userland/user-pie.ld` at the standard user-space PIE base, same as every
other userland app.
