# CPython port layer

`/APPS/PYTHON.ELF` on the golden is CPython 3.11.9, cross-compiled freestanding
for MayteraOS. This directory holds the MayteraOS-authored layer that makes
that possible. It is 23 files and ~348 KB; the CPython source it sits on is
upstream 3.11.9 and is not vendored here.

| Directory | What it is |
|---|---|
| `src-cpython/` | `mos_pymain.c` (the MayteraOS launcher: sys.path onto the ext2 root, script-from-disk execution, kernel-CSPRNG hash randomisation), `compat.c` and `misc.c` (trimmed supplements standing in for POSIX pieces we do not have) |
| `src-libc/` | The libc additions CPython forced: `posixextra.c`, real `errno.c/.h`, plus the headers CPython expects (`unistd.h`, `stdlib.h`, `time.h`, `inttypes.h`, `sys/time.h`, `sys/types.h`) |
| `src-kernel/` | The kernel-side `syscall.c/.h` state this port was built against. **Version at that time: MayteraOS 1.95.0, kernel build 686.** |
| `diffs/` | The five diffs against libc and the kernel, kept so the delta is legible rather than having to be re-derived by comparison |
| `scripts/relink.sh` | The exact link recipe that produced the shipping binary |

## Why this is here

The port existed only under `<workspace>` on one build
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

- **CPython 3.11.9 itself** (`<workspace>`, 467 MB): upstream source
  plus its build tree, re-downloadable from python.org.
- **`libpython3.11.a` and the four supplement archives** (`libpymath_supp.a`,
  `libwcharsupp.a`, `libmiscsupp.a`, `libcompatsupp.a`): build outputs.
  `relink.sh` rebuilds the last two from `src-cpython/`.
- **The binary.** Build outputs do not belong in the source of truth.

## Known gaps, stated rather than discovered later

- `relink.sh` points `L` and `U` at
  `<workspace>`, the tree CLAUDE.md calls stale
  reference only. It is recorded verbatim because it is what actually produced
  the shipping binary, and rewriting it without rebuilding and testing would
  substitute a guess for a fact. Repointing it at the repo libc is part of
  #650, not of committing the source.
- It links `user.ld`, not `user-pie.ld`, so `PYTHON.ELF` is ET_EXEC: no PIE, no
  ASLR, and outside #640's coverage. It also builds `-fno-stack-protector`, so
  #651's canary does not cover it either.
- `PYTHON.ELF` is on `KNOWN_STALE_LAUNCH` in `build/repo-guard.sh`: the golden
  ships the asset-base binary and does not rebuild it. Closing that is #650.
