# CPython port layer

`/APPS/PYTHON.ELF` on the golden is CPython 3.11.9, cross-compiled freestanding
for MayteraOS. This directory holds the MayteraOS-authored layer that makes
that possible. It is 13 files; the CPython source it sits on is
upstream 3.11.9 and is not vendored here.

| Directory | What it is |
|---|---|
| `src-cpython/` | `mos_pymain.c` (the MayteraOS launcher: sys.path onto the ext2 root, script-from-disk execution, kernel-CSPRNG hash randomisation), `compat.c` and `misc.c` (trimmed supplements standing in for POSIX pieces we do not have) |
| `src-kernel/` | The kernel-side `syscall.c/.h` state this port was built against. **Version at that time: MayteraOS 1.95.0, kernel build 686.** |
| `diffs/` | The five diffs against libc and the kernel, kept so the delta is legible rather than having to be re-derived by comparison |
| `scripts/relink.sh` | The exact link recipe that produced the shipping binary |

## Why this is here

The port existed only under `/root/cpython-port-phase2-golden` on one build
container. The 5.1 MB `PYTHON` binary shipped, but nothing that could rebuild
it was under version control, so "we have a working Python 3" was true only for
as long as that container survived. Same shape as the browser engine, fixed the
same way: commit what is ours, record how to fetch what is not.

### A note on `src-kernel/`

These files are a VERBATIM SNAPSHOT of `kernel/proc/`, kept for provenance. They
are not a build input and cannot be one: `syscall.c` still carries its original
`#include "../gui/image.h"`, `"../serial.h"`, `"../cpu/gdt.h"` and a dozen more
relative includes that resolve to nothing from this directory.

The same copy once brought a whole `version.h` along with it. That made a SIXTH
definition of `MAYTERA_VERSION_STRING` in the tree, which somebody then had to
hand-sync on every version bump, and which had already fallen out of step with
its own MAJOR/MINOR/PATCH triple. It is deleted; the one fact it carried is the
version number recorded in the table above, and `build/version-gate.sh` now
fails the build if any second definition comes back (#745, local 106).

## What is NOT here

- **CPython 3.11.9 itself** (`/root/cpython-port`, 467 MB): upstream source
  plus its build tree, re-downloadable from python.org.
- **`libpython3.11.a` and the four supplement archives** (`libpymath_supp.a`,
  `libwcharsupp.a`, `libmiscsupp.a`, `libcompatsupp.a`): build outputs.
  `relink.sh` rebuilds the last two from `src-cpython/`.
- **The binary.** Build outputs do not belong in the source of truth.

## Known gaps, stated rather than discovered later

- `relink.sh` points `L` and `U` at
  `<workspace>`, the tree CLAUDE.md calls stale
  reference only. It is kept verbatim as the historical record of what produced
  the July binary. It is NOT what builds CPython now: `scripts/relink-pie.sh`
  (#661) is, it points at the repo userland, and `build-golden.sh` section 3h
  runs it on every golden build.

### What happened to `src-libc/` (deleted 2026-10-02)

It held forked copies of `stdlib.c`, `errno.c/.h`, `posixextra.c`, `stdlib.h`,
`inttypes.h`, `time.h`, `unistd.h`, `sys/time.h` and `sys/types.h`, described
here as "the libc additions CPython forced". That was true when it was written.
It stopped being true once `errno.c` and `posixextra.c` were upstreamed into
`userland/libc` and the headers followed: MEASURED on 2026-10-02, all 292
identifiers defined or declared across those ten files existed in the shared
libc, and nothing in the tree used the directory as a build input.

What it still did was mislead. Its `stdlib.c` was a 2026-07-07 copy of the
shared allocator carrying the pre-#245 unbounded free-list walk, so a reader
comparing it against the fixed `userland/libc/stdlib.c` would reasonably
conclude that CPython does not get the #245 fix. CPython does get it:
`relink-pie.sh` links `$U/libc/libc.a`, the shared one.

`build/alloc-single-source-gate.sh` now fails the build if a second definition
of malloc/free/realloc/calloc appears anywhere under `userland/`, so this
cannot come back quietly. The deleted files are in git history if the July
state is ever needed.
- It links `user.ld`, not `user-pie.ld`, so `PYTHON.ELF` is ET_EXEC: no PIE, no
  ASLR, and outside #640's coverage. It also builds `-fno-stack-protector`, so
  #651's canary does not cover it either.
- `PYTHON.ELF` is on `KNOWN_STALE_LAUNCH` in `build/repo-guard.sh`: the golden
  ships the asset-base binary and does not rebuild it. Closing that is #650.
