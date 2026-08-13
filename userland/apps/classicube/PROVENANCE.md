# ClassiCube vendoring provenance (task #28)

## Upstream

| Field | Value |
|---|---|
| Project | ClassiCube (a Minecraft Classic clone written in C) |
| Upstream URL | https://github.com/UnknownShadow200/ClassiCube |
| Pinned commit | `4016a0918ba5c127d5203a4940e76b79b229d51f` |
| Commit date | 2026-08-09T22:36:56+10:00 |
| Commit subject | Windows: try to support icons on ARM builds too |
| Vendored on | 2026-08-10 |
| Vendored by | MayteraOS task #28, agent lane "platform layer" |

`fetch-upstream.sh` re-creates the exact staged clone from that pinned commit, so
the provenance is recorded as CODE and not only as prose. The buildable source is
ALSO vendored in-repo (under `src/`), so a `git archive` of `maytera-src` can
rebuild ClassiCube with no network access, matching the convention already used
by `userland/apps/assaultcube/`.

## Licence

**ClassiCube is licensed under the modified (3-clause) BSD licence.** The full,
byte-identical upstream licence text is vendored alongside this file as
`license.txt` (md5 `a7c4a780e01e1bfa1883428c39f8dda4`), together with the
upstream `credits.txt`.

The licence explicitly permits redistribution in source and binary form,
provided that:

1. source redistributions retain the copyright notice, the conditions and the
   disclaimer (satisfied by `license.txt` staying in this directory, next to the
   source it covers);
2. binary redistributions reproduce the same notice in the documentation or
   other materials shipped with the distribution (satisfied by
   `ATTRIBUTION.md` at the repository root, which must carry a ClassiCube
   entry before any image containing this app is published);
3. the ClassiCube name and its contributors' names are not used to endorse or
   promote derived products without prior written permission.

Condition 3 is a naming restriction, not a redistribution restriction. It means
the MayteraOS Start-menu entry may say "ClassiCube" as a factual identification
of what the program IS, but MayteraOS marketing must not imply that ClassiCube
or its authors endorse MayteraOS.

**Conclusion: redistribution is permitted; vendoring proceeded.**

`license.txt` additionally carries the third-party notices that upstream
aggregates there (OpenTK MIT licence, and the public-domain / permissive
notices covering the ray-box intersection, voxel traversal and frustum-culling
algorithms). Those travel with the file; do not split them out.

## What was vendored, and what was not

Vendored verbatim from the pinned commit:

* `src/` in full, upstream layout preserved. This includes every per-platform
  subdirectory (`src/psp/`, `src/dreamcast/`, ... ) and `src/freetype/`. They
  are not built by MayteraOS today, but keeping the tree whole is what makes
  the next `fetch-upstream.sh` a no-op diff instead of an argument about what
  is missing.
* `license.txt`, `credits.txt`, `upstream-readme.md` (upstream `readme.md`,
  renamed only so it does not shadow a MayteraOS-authored readme).

Deliberately NOT vendored:

* `third_party/` (BearSSL, tinyalloc, citro3d). BearSSL is only needed for the
  HTTPS/multiplayer path, which this port does not build (see
  "Networking" in the port notes). Nothing under `src/` that MayteraOS compiles
  references it.
* `misc/` (per-platform makefiles, icons, packaging). MayteraOS supplies its own
  build rules; upstream's makefiles assume toolchains that do not exist here.
* `doc/`.

## MayteraOS-authored files in this directory

Everything below is MayteraOS code, NOT upstream, and is not covered by the
vendoring above:

* `src/maytera/` - the MayteraOS platform backend. Upstream's own convention is
  one directory per platform (`src/psp/Platform_PSP.c`,
  `src/dreamcast/Platform_Dreamcast.c`, ...), so a new platform lands here and
  nowhere else.
* `engine-patches/` - the ONE patch applied to vendored source, expressed as an
  idempotent, anchor-asserting Python script rather than a hand edit. Re-running
  `engine-patches/apply-all.sh` after a re-fetch reproduces the tree exactly.
* `Makefile.platform`, `PROVENANCE.md`, this file's siblings.

## The single vendored-source modification

`engine-patches/0001-core-platform-detect.py` inserts a `PLAT_MAYTERA` branch at
the TOP of the platform-detection chain in `src/Core.h`.

It has to be first in the chain, and this is not a style preference: the
MayteraOS userland cross-build uses the build host's gcc, which predefines
`__linux__` even for this freestanding `-nostdinc -nostdlib` target (the same
fact already documented in `userland/libc/time.h` for CPython). Without the new
branch, `Core.h` silently selects `CC_BUILD_LINUX` + `CC_BUILD_POSIX`, and the
build then tries to compile `Platform_Posix.c` against `<sys/socket.h>`,
`<netdb.h>`, `<dlfcn.h>` and friends, none of which exist in this libc. That
failure mode is loud; the worse one is a future `#elif` reshuffle silently
demoting MayteraOS back to the Linux branch, which is why the patch asserts its
anchors and exits non-zero on a miss.
