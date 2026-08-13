# Building MayteraOS

This document describes how to build MayteraOS from this repository: prerequisites,
building the kernel, building userland, assembling the two-partition golden image,
what the required gates check, how to verify a built image, and how to write an
image to a USB device safely.

It is deliberately generic. It contains no internal hostnames, IP addresses,
container/VM identifiers, credentials, or device paths specific to any one
maintainer's infrastructure. If your build setup uses remote build hosts or
containers, substitute your own orchestration around the commands below; the
commands themselves are exactly what the scripts in `build/` run.

Everything in this file is derived from, and should stay in sync with, the actual
build scripts committed in this repository: `kernel/Makefile`,
`userland/libc/Makefile` and the per-app `userland/apps/*/Makefile`,
`build/build-golden.sh`, `build/invariant-gate.sh`, and `build/repo-guard.sh`. Where
a claim below could not be verified against one of those scripts, it says so
explicitly rather than asserting it.

## 1. Prerequisites and toolchain

Verified against the toolchain actually used to build this tree:

- **gcc 12.x** (built and gated against Debian's `gcc (Debian 12.2.0-14+deb12u1)
  12.2.0`). The kernel and userland are both freestanding builds; a materially
  different gcc major version is untested and may produce different warnings under
  `-Werror`.
- **nasm 2.16.x** (built and gated against NASM 2.16.01) for all `.asm` sources
  (kernel entry/context-switch/syscall trampolines, userland `crt0`/`syscall`
  wrappers).
- **GNU binutils** (`ld`, `objcopy`, `nm`, `size`, `strip`) matching the gcc
  toolchain.
- **rustc 1.97.0, pinned**, with the `x86_64-unknown-none` target installed, for
  in-kernel Rust (`kernel/rustkern.rs` + `kernel/rustkern/*.rs`, see
  `kernel/rust-toolchain.toml`). Install with:
  ```
  rustup toolchain install 1.97.0 --profile minimal
  rustup target add --toolchain 1.97.0 x86_64-unknown-none
  ```
  `kernel/Makefile` reads the active `rustc --version` and **fails the build** if
  it is not exactly `1.97.0`; this is a reproducibility gate, not a suggestion. No
  other Rust flags beyond `--edition 2021 --crate-type staticlib -C opt-level=2
  -C panic=abort` are required (verified in `kernel/Makefile`'s `RUSTFLAGS`).
- **python3** for the link-time gates described in section 3 (they live under
  `kernel/tools/*/` and are invoked by the Makefile with `python3`).
- Standard Linux disk/filesystem tools for assembling and verifying the golden
  image: `losetup`, `sgdisk`, `mkfs.vfat` (or equivalent FAT tooling), `mke2fs`,
  `debugfs`, `e2fsck`, `fsck.fat`, `blkid`, `blockdev`, `mount`. These are only
  needed for the image-assembly and gate steps in sections 4-5, not for compiling
  the kernel or userland.

Building the golden image and running the gates requires root (loop-device
mounting, `debugfs` writes to the ext2 root). Building the kernel or a single
userland app does not.

## 2. Repository layout relevant to a build

- `kernel/` - the freestanding C/ASM/Rust kernel, built with `kernel/Makefile`.
- `userland/libc/` - the freestanding libc every user-mode app links against
  (`libc.a` + `crt0.o`).
- `userland/apps/*/` - one directory per user-mode app, each with its own
  `Makefile` that links against `../../libc`.
- `userland/user.ld` - the user-space linker script. It intentionally forces a
  **single RWX `PT_LOAD` segment** covering the whole binary (see the comment at
  the top of the file): the kernel's ELF loader copies loadable segments in via a
  writable mapping, so a split read-only `.text` segment page-faults on load. Do
  not "fix" this by letting `ld` split segments normally.
- `build/` - `build-golden.sh` (image assembly), `invariant-gate.sh` and
  `repo-guard.sh` (the two required gates), plus supporting assets
  (`build/font-licenses/`, `build/assets/`, `build/asset-manifest.sha256`).
- `docs/` - design docs.
- `CHANGELOG.md` and `blame.md` at the repository root - **mandatory** on every
  change. `CHANGELOG.md` gets a dated, newest-first entry for every kernel,
  userland, build/deploy, or doc change, describing what changed and why.
  `blame.md` records recurring mistakes and their root causes so the next change
  does not reintroduce them; read it before non-trivial work and append to it
  when you hit a new one.

## 3. Building the kernel

```
cd kernel
make clean
make -j4
```

This produces `kernel/kernel.elf`. Useful variants:

- `make info` - dump the collected source/object lists.
- `make install` - copy the built `kernel.elf` into `../uefi/boot/`.
- `make clean` - remove `obj/`, `kernel.elf`, and the Rust static lib.

Sources are collected per subdirectory with `$(wildcard ...)` (`mm/`, `cpu/`,
`video/`, `drivers/`, `net/` + `net/tls/`, `crypto/`, `fs/`, `shell/`, `gui/`,
`exec/`, `dos/`, `proc/`, `games/`, `vfs/`, `apps/`, `bt/`, `drivers/net/wifi/`,
plus the vendored media decoders under `media/`). Adding a new `.c` file in one of
these directories is enough for the Makefile to pick it up; no source list needs
editing. Assembly objects use an `_asm.o` suffix so `foo.asm` and `foo.c` in the
same directory do not collide in `obj/`.

**The build number is not bumped by `make`.** `kernel/version.h` carries
`#define MAYTERA_BUILD_NUMBER <n>`. A plain `make` runs a target called
`increment_build`, but despite the name it does **not** increment anything: it
only checks that `version.h` has a parseable `MAYTERA_BUILD_NUMBER` define and
fails loudly if not (see `kernel/increment_build.sh` for the full history of why
this was deliberately changed from an actual increment). The build number is
owned exclusively by `build/build-golden.sh`, which computes
`max(last recorded build, version.h's current value) + 1` and patches that value
into the checkout it builds from. If you are building the kernel by hand for
development, bump `MAYTERA_BUILD_NUMBER` yourself when it matters to you; a
manual `make` will not do it, and `build-golden.sh`'s own bump does not depend on
whatever you left in your working tree.

### 3.1 Kernel target: soft-float, SSE disabled

The kernel's `CFLAGS` (see `kernel/Makefile`) include `-mno-mmx -mno-sse -mno-sse2`
alongside `-ffreestanding -fno-stack-protector -fno-pic -mno-red-zone
-mcmodel=kernel -nostdlib -nostdinc -fno-builtin -Wall -Wextra -Werror -O2`. FPU
state is not saved across a context switch, so **no code path in the kernel may
assume SSE registers survive a context switch**, and floating-point-heavy designs
are a non-starter; the vendored audio/media decoders are all fixed-point for this
reason. The one documented exception is `gui/math.o` and `gui/ttf.o`
(`stb_truetype`'s glyph rasterizer uses `double`): the Makefile re-enables SSE for
just those two translation units (`TTF_CFLAGS` strips `-mno-sse`/`-mno-sse2` and
adds `-msse -msse2`). Do not extend that exception without the same care that went
into scoping it to those two files.

Rust in the kernel targets `x86_64-unknown-none`, which is likewise soft-float by
construction and matches the C ABI (`code-model=kernel`, redzone off, `panic=abort`).
Only `core` and `alloc` are available; `std` needs an OS underneath it and is not
available in a freestanding kernel target.

**New kernel code should be Rust unless there is a stated performance reason not to
be.** This is a standing project policy, not a style preference: C is no longer the
default for new kernel subsystems, drivers, syscalls, or accessors. Where Rust
replaces existing C, the established pattern keeps the C implementation available
under a `_c` suffix, routed behind a `-DRUST_<NAME>` flag, so the two can be
differentially compared at boot rather than the C version being deleted outright.

### 3.1a Optional: building with the SSH server (`MAYTERA_SSHD`)

The in-kernel SSH server is **not** compiled in by default. To include it:

```sh
make MAYTERA_SSHD=1
```

There are **two** switches and they are not the same switch:

| | What it decides | Default |
|---|---|---|
| **Compile time**, `make MAYTERA_SSHD=1` | whether the server code is on the image at all | OFF |
| **Run time**, `/CONFIG/SSHD.CFG` with `enable=1` | whether a kernel that has the code actually listens | OFF |

`MAYTERA_SSHD=1` on its own **does not open a port**. It ships a service an
operator can then enable, without another rebuild. Both switches have to say
yes before anything listens, and an absent, malformed or credential-bearing
config means no listener.

Without the flag, `net/ssh/ssh2_server.c` is not compiled, nothing calls it,
no `sshd` service is registered, and the kernel says so at boot. The SSH
**client** always builds and is unaffected.

Because the flag changes the kernel bytes, `build/build-golden.sh` records it in
`/BUILDINFO.TXT` as `sshd=0`/`sshd=1` and `invariant-gate.sh --verify-kernel`
replays it when rebuilding to check reproducibility. See `docs/SSHD.md` for the
configuration format, the authentication model and the honest limits.

### 3.2 Gates that run on every kernel link

Five gates run as **order-only prerequisites of the final link** (`kernel.elf`
depends on them via `|`, so they run on every build without needlessly forcing a
relink when nothing changed). Each is `.PHONY`, each aborts the build loudly if
its own tool is missing rather than silently skipping, and each lives under
`kernel/tools/` (inside the kernel tree itself) so it runs wherever the kernel is
actually built:

- **`rust-symbol-gate`** - fails if the Rust static library (`librustkern.a`)
  stops exporting a symbol that `kernel/rust-symbols.manifest` declares, or
  exports one the manifest does not know about. Guards against a Rust export
  being silently deleted while nothing fails to link (most exports sit behind a
  `-DRUST_*` strangler flag, so no C caller exists to fail if the symbol vanishes).
- **`syscall-dispatch-gate`** - fails if a syscall listed in
  `kernel/syscall-dispatch.manifest` loses its `case SYS_*:` in
  `proc/syscall.c` while its number stays `#define`d in `proc/syscall.h`. A
  dropped `case` builds clean otherwise (the number, the header, and the
  implementation all still exist; the call just falls through the switch).
- **`syscall-ptr-lint`** - lints that every pointer-taking syscall validates its
  user pointer at entry.
- **`copy-user-lint`** - lints that syscalls which validate a user pointer also
  *use* `copy_*_user` rather than dereferencing the raw pointer directly
  (closing the TOCTOU gap between validation and use).
- **`concurrency-lint`** - fails the build on any new hand-rolled busy-wait /
  spin / poll loop. All waiting must go through the kernel wait-queue
  (`sync/waitq.h`) or the futex layer (`sync/futex.c`); see the project's
  waiting-primitive policy for the accepted patterns. This target is
  `--dirs kernel`-scoped in the link gate (the build environment may only have
  `kernel/` present); run `make lint` (alias `lint-all`) from a full checkout to
  also scan `userland/`.

Each of `syscall-ptr-lint`, `copy-user-lint`, and `concurrency-lint` has a
`*-selftest` Make target (`syscall-ptr-lint-selftest`,
`copy-user-lint-selftest`, `concurrency-lint-selftest`) that proves the lint goes
red on a synthetic broken fixture and green on the real tree. A lint that has
never been watched to fail is not a trustworthy lint; run the self-tests after
touching any of them.

`rust-symbol-update` and `syscall-dispatch-update` regenerate their respective
manifests from the current source, for when you are knowingly adding a new
export or syscall case and need the manifest to catch up.

## 4. Building userland

Build the shared libc first, then each app links against it:

```
cd userland/libc
make clean && make        # produces libc.a and crt0.o

cd ../apps/<app>
make clean && make        # links crt0.o + the app's own objects + ../../libc/libc.a
```

Each app `Makefile` follows the same shape (see `userland/apps/hello/Makefile` for
the simplest example): freestanding `CFLAGS` (`-ffreestanding -fno-stack-protector
-fno-pic -mno-red-zone -nostdlib -nostdinc -fno-builtin -Wall -Wextra -O2 -g`),
linked with `userland/user.ld`'s single RWX segment via `ld -nostdlib
-z max-page-size=0x1000`. There is no top-level "build all userland" target in
this repository; `build/build-golden.sh` builds each `apps/*/Makefile` in a loop
and captures whichever output file is an ELF executable (see section 5). If you
maintain your own multi-app build wrapper, that loop is the reference behavior to
replicate: `cd` into each app directory, `make clean && make`, and the resulting
ELF (not a `.o`, `.a`, or source file) is the app's shipping binary.

A handful of apps are known not to build cleanly today and are treated as an
explicit, named exception rather than a silent one: see the `KNOWN_FAIL` list near
the top of `build/build-golden.sh`'s userland stage. Any *other* app failing to
build is treated as fatal by that script, precisely so a new breakage cannot slip
in silently under cover of the existing known ones.

## 5. Producing the two-partition golden image

`build/build-golden.sh` is the reproducible build of the shipping image. It:

1. Resolves the git commit it is building from (`git rev-parse HEAD`) and refuses
   to proceed if tracked files are modified but uncommitted (an image stamped with
   a commit that does not actually contain the source it was built from defeats
   provenance). Untracked files are ignored for this check; only tracked
   modifications block.
2. Computes the next build number as `max(last recorded build, kernel/version.h's
   current value) + 1` and patches it into a disposable checkout it builds from
   (never the working tree you are editing).
3. Pins `MAYTERA_BUILD_DATE`/`MAYTERA_BUILD_TIME` (via `REPRO_DATE`/`REPRO_TIME`
   passed to `make`) instead of letting `__DATE__`/`__TIME__` embed the real wall
   clock, and stamps those same pinned values into the image's `BUILDINFO.TXT`.
   This, plus `-ffile-prefix-map` in `CFLAGS` and dropping `-g` from the assembler
   invocation, makes two builds of the same commit byte-identical regardless of
   which directory they were built in. This reproducibility is what section 6's
   kernel-rebuild verification depends on.
   Optional build flags that change the kernel bytes are stamped here too. Today
   that is `MAYTERA_SSHD` (section 3.1a), recorded as `sshd=0`/`sshd=1`, which
   section 6.1's `--verify-kernel` replays so it rebuilds the same kernel rather
   than a different one. Build a golden with the SSH server included using
   `MAYTERA_SSHD=1 build/build-golden.sh`; note this still ships **no**
   `/CONFIG/SSHD.CFG`, so the resulting image has no listener until one is
   configured.
4. Builds the kernel (section 3) and the userland app set (section 4) from a
   `git archive` of the resolved commit, not a copy of the live working tree, so
   the image the build produces is provably built from exactly that commit's
   source (no untracked or in-flight edits can sneak into a golden).
5. Assembles a **two-partition GPT image**: a FAT ESP no larger than 256 MiB
   (partition type `EF00`) plus an ext2 root (partition type `8300`). The kernel
   is copied to four boot paths on the ESP (`boot/kernel.elf`, `kernel.elf`,
   `EFI/BOOT/kernel.elf`, `KERNEL.ELF`); because FAT 8.3 names are
   case-insensitive, `kernel.elf` and `KERNEL.ELF` collapse to the same directory
   entry, so there are three *distinct* boot locations that must end up
   byte-identical. An empty `ROOTEXT2` marker file on the ESP tells the kernel to
   root-mount the ext2 partition instead of running everything off FAT. This
   script starts from an existing, boot-proven two-partition base image (an
   asset base carrying static content such as fonts, wallpapers, and
   configuration, not itself part of this repository) and overlays the freshly
   built kernel and app binaries onto it, so the image's GPT/partition structure
   is correct by construction; the invariant gate below then proves it did not
   drift.
6. Writes `/BUILDINFO.TXT` to the ESP with the building commit, the build number,
   the pinned source date/time, and the image layout description.
7. Overlays every freshly-built app binary onto its existing `/APPS` entry on the
   ext2 root (or the app's actual launch path, where that differs from `/APPS`),
   preserving the exact on-disk name the shipping UI launches. It records the md5
   of each freshly-overlaid binary in a manifest, keyed by the exact path it was
   written to; that manifest is what the freshness checks in section 6 consume.
8. Runs `build/invariant-gate.sh` and `build/repo-guard.sh` (section 6). **The
   script refuses to record a successful build unless both pass.**

You do not need `build-golden.sh` to build and test the kernel or an individual
app; you need it only to assemble the shipping two-partition image, and it
depends on a pre-existing asset-base image this repository does not carry (fonts,
wallpapers, and other static content that is either large, licensed, or
maintainer-specific). If you are setting this up from scratch, you will need to
construct an equivalent boot-proven two-partition ext2 base image yourself before
`build-golden.sh`'s overlay steps have anything to overlay onto.

## 6. The two required gates

Both gates are wired into `build/build-golden.sh` as required prerequisites: a
golden image is not recorded as built unless both exit 0. Both can be run
independently against any image, and both prove they can fail with a `--self-test`
mode that exercises a deliberately broken fixture alongside a good one.

### 6.1 `build/invariant-gate.sh` - image structure

```
build/invariant-gate.sh <image> --commit <githash> --prev <prev_build_number> \
    [--fresh-apps <manifest>] [--verify-kernel]
build/invariant-gate.sh --self-test          # prove it goes RED on a broken image, GREEN on a good one
build/invariant-gate.sh --self-test-fresh    # prove the app-freshness check goes RED on a stale binary
build/invariant-gate.sh --self-test-ext2     # prove the ext2 fsck check goes RED on a corrupt root
build/invariant-gate.sh --self-test-fat      # prove the FAT fsck check goes RED on a corrupt ESP
build/invariant-gate.sh --self-test-kernel   # prove the kernel-rebuild check goes RED on a stale kernel
```

Exit 0 = every invariant holds. Exit 1 = an invariant failed. Exit 2 = the gate
could not do its job at all (a missing tool, not running as root) and fails
closed rather than passing silently. Must run as root (`losetup`, `mount`,
`sgdisk`, `debugfs`).

Checks performed (each independently provable red/green, not merely asserted):

- Exactly 2 GPT partitions, with partition type codes `EF00` (ESP) then `8300`
  (root) in that order.
- p1 is a FAT filesystem, at most 256 MiB.
- **p1 passes `fsck.fat -n`** run on the unmounted partition. A corrupt boot
  filesystem is rejected outright rather than merely inspected for expected
  names.
- p1 carries no `/APPS` directory and none of the core app binaries at its root:
  userland lives only on the ext2 root, never on the boot ESP.
- p1 has the UEFI bootloader at `EFI/BOOT/BOOTX64.EFI` and the `ROOTEXT2` marker.
- The kernel is present at all boot paths described in section 5 and is
  byte-identical across every one of them (a stale copy at one path would boot
  instead of the fresh one).
- p1's `/FONTS` is populated, and if it carries any `.ttf`/`.otf`/`.ttc` files it
  must also carry a non-empty `LICENSE.TXT` alongside them (several bundled
  fonts are under the SIL Open Font License, which requires the license text
  ship with the font files).
- `/BUILDINFO.TXT` is present on the ESP, its stamped commit matches the
  commit passed via `--commit`, and its stamped build number is **strictly
  greater** than the number passed via `--prev`.
- p2 is ext2.
- **p2 passes `e2fsck -fn`.** Like the FAT check, this validates the filesystem
  itself, not just its type label; a filesystem that only looks right (correct
  partition type, plausible directory listing) but fails a real consistency
  check is rejected.
- p2's `/APPS` contains the full core app set: `FONTBOOK`, `SETTINGS`, `FILES`,
  `EDITOR`, `BROWSER`, `TASKMGR` (case-insensitive match). `FONTBOOK` is checked
  first deliberately, as the app that once shipped as a menu entry with no
  binary behind it.
- The ext2 root ships enough wallpaper `.BMP` files that the wallpaper picker is
  non-empty, and every `.BMP` name the shipping compositor binary references
  (found via `strings` on the binary itself) actually exists somewhere on the
  image.
- With `--fresh-apps <manifest>` (a file of `NAME MD5` lines): every named binary
  is re-read directly from the image and its md5 compared against the manifest,
  proving the *launched* app is the freshly built binary and not a stale
  asset-base copy. A fixed set of core launch targets (the compositor, Settings,
  Files, the terminal, the task manager, and the DOOM binary at its actual launch
  path) is *required* to appear and verify fresh; any of them missing from the
  manifest, unreadable, or stale fails the gate.
- With `--verify-kernel`: **rebuilds the kernel from the commit stamped in
  `/BUILDINFO.TXT`** (using the pinned `srcdate=`/`srctime=` values also stamped
  there) and requires the rebuilt binary to be byte-identical to the one shipped
  on the image. This is the only check that ties the shipped kernel *binary* to
  its claimed *source*; every other kernel-related check above is a label
  (a filename, a stamp) that a stale or hand-copied binary could also satisfy.
  An image with no `srcdate=`/`srctime=` stamp cannot be verified this way and is
  rejected rather than silently skipped, on the principle that "cannot check"
  must never be allowed to read as "checked".

### 6.2 `build/repo-guard.sh` - source integrity

```
build/repo-guard.sh <image> --commit <githash> --prev <prev_build_number> \
    --fresh-apps <manifest>
build/repo-guard.sh --self-test
```

Where the invariant gate proves the *image* is well formed, `repo-guard.sh` proves
the image came from committed, rebuildable *source*:

- The source repository has no uncommitted or untracked source files (build
  artifacts such as compiled binaries are exempt; anything matching
  `.c/.h/.rs/.asm/.S/.cpp/.hpp/.cc/.py/.ld/.mk/.sh` is not).
- The building commit is reachable on one of the long-lived branches (`dev`,
  `testing`, `stable`).
- Every app name the shipping UI can actually launch (parsed out of the
  compositor, Files, and Launcher source, plus the AI launch table and the
  file-association table) resolves to a freshly built binary in the fresh-app
  manifest, except a small, explicitly named allowlist of apps known not to
  build today. A launched name that resolves to neither is a hard failure: this
  is what closes the class of bug where the UI launches one on-disk name (say,
  an old short name) while the fresh build lands on a different one, so the
  "fix" never actually ships.
- `build/asset-manifest.sha256` exists and records a substantial number of
  freeware-tier assets; a bounded sample of assets overlaid onto the ext2 root
  is spot-checked against its recorded sha256.
- `/BUILDINFO.TXT` stamps the building commit and a build number strictly
  greater than the previous one (the same check as the invariant gate, run
  independently here against the source side).

## 7. Verifying an already-built image

You do not need to rebuild anything to check whether an image is sound; run the
gate directly against it:

```
build/invariant-gate.sh /path/to/image.img --commit <commit-it-claims> \
    --prev <build-number-before-it> --verify-kernel
```

Read the `commit=` and `build=` lines out of `/BUILDINFO.TXT` on the image's ESP
(mount partition 1) if you do not already know them. A clean exit (0) with every
line printed `ok` means the image passes every structural and provenance check
above; any `FAIL` line names exactly which invariant did not hold.

## 8. Writing an image to a USB device

This repository does not script this step; the two general engineering practices
below apply regardless of your platform and are worth stating explicitly, because
skipping either is how a host disk gets overwritten by mistake:

1. **Identify the device by content, not by guesswork, and confirm it is
   removable before writing.** On Linux:
   ```
   lsblk -o NAME,SIZE,TYPE,RM,MODEL,MOUNTPOINT
   ```
   Confirm the target shows `RM` (removable) = 1, that its `SIZE` matches your USB
   device and not a fixed internal disk, and that nothing under it is currently
   mounted (unmount any auto-mounted partitions first). Never write to a device
   node you have not just looked up this way in the same session; device letters
   shift when drives are added or removed.
2. **Write with `conv=fsync` and read the result back.** For example:
   ```
   sudo dd if=/path/to/image.img of=/dev/sdX bs=4M status=progress conv=fsync
   sync
   ```
   Then verify the write actually landed by comparing checksums between the
   source image and what is now on the device, rather than trusting `dd`'s exit
   status alone:
   ```
   md5sum /path/to/image.img
   sudo dd if=/dev/sdX bs=4M count=<image-size-in-4M-blocks> | md5sum
   ```
   A checksum mismatch after a clean `dd` exit usually means the device was still
   flushing when you disconnected it, or that you read back from the wrong node;
   treat it as a failed write, not a cosmetic discrepancy, and redo it before
   trusting the media.

## 9. Documentation obligations

`CHANGELOG.md` and `blame.md` at the repository root are mandatory. Every change
in this repository (kernel, userland, build/deploy scripts, or docs) gets a dated,
newest-entry-first line in `CHANGELOG.md` describing what changed and why, as part
of the same change, not as a follow-up. `blame.md` exists because the same
mistakes recur; read it before non-trivial work, and add to it when you hit a new
one worth remembering.

## 10. Deliberately not covered here

- **Deploying a built kernel or image to a specific test target** (VM disk paths,
  serial console access, hardware bring-up). That is infrastructure-specific by
  nature; if you maintain your own test environment, document your own deploy
  procedure separately.
- **An ISO/installer build target.** An earlier version of this project's tooling
  referenced `make iso` / `make iso-test` kernel Makefile targets; those targets
  do not exist in the current `kernel/Makefile` (verified by grepping its
  `.PHONY` targets: `all`, `dirs`, the five gates above, `flags-stamp-force`,
  `clean`, `info`, `lint`/`lint-all`, `lint-baseline`, `lint-report`, `install`).
  If installer-ISO generation is restored, this section should be updated to
  describe it rather than left claiming it exists.
- **An automated integration test suite.** This repository does not currently
  contain one; do not assume `tests/` exists here.
- **Constructing the static asset base** that `build/build-golden.sh` overlays
  onto (fonts, wallpapers, default configuration). That base is intentionally
  outside this repository (see section 5) and this document does not attempt to
  describe how to build one from scratch, since that has not been verified
  against a committed script.
