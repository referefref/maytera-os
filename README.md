# MayteraOS

**MayteraOS is an LLM-first operating system, built entirely from scratch.**

The AI layer is not an app bolted on top, it is a first-class part of the
system. Every application publishes a machine-readable **tool contract**, and a
built-in LLM reads those contracts to operate the desktop and, with your
explicit consent, to **write, compile and run new applications, widgets and
drivers on the running machine**, every action scoped by a capability token,
gated on consent, and written to an audit trail.

Underneath that sits a complete, hand-built OS: its own UEFI bootloader, a
freestanding C and Rust kernel (SMP, demand paging, COW), a compositor desktop,
an in-house TCP/IP + TLS 1.3 stack, ext2/FAT filesystems, and a suite of native
applications. It boots on real x86-64 hardware and in virtual machines.

> Status: an ambitious, experimental research OS under active development. It
> boots to a usable desktop, but it is experimental software. Do not run it on
> production hardware or a network you care about.

![MayteraOS desktop](screenshots/desktop-clean.png)

**This release: version 1.95.0, kernel build 1761.**

## Screenshots

| | |
|---|---|
| ![Desktop](screenshots/desktop.jpg) | ![Settings](screenshots/settings.jpg) |
| The desktop: wallpaper, icons, taskbar gauges, widgets | The Settings app (style engine + TTF) |
| ![Widgets](screenshots/widgets.png) | ![Browser](screenshots/browser.png) |
| Desktop widgets (clock, calendar, weather, monitors) | The web browser with a TLS 1.3 client |
| ![DOOM](screenshots/doom.jpg) | ![Maytera Studio](screenshots/studio-filters.png) |
| DOOM, running on the MayteraOS platform layer | Maytera Studio: the Texturizer filter dialog |

## Highlights

- **LLM-first architecture** - every app publishes a YAML **tool contract**; a
  built-in LLM reads them to drive the desktop and, on your consent, generate,
  compile and run new apps/widgets/drivers on the live system. Every action is
  scoped by a **capability token**, gated on consent, and written to an audit
  trail. A prompt-injection keyword guard (the Nova ruleset) ships in the
  kernel. See the honest description of its limits under Security below.
- **Kernel** - long-mode paging with demand paging and copy-on-write, a
  preemptive scheduler with SMP, ELF and PE loaders, signals, futexes, and a
  wait-queue-based blocking layer.
- **Graphics + desktop** - a framebuffer compositor with damage-tracked
  redraw, drop shadows, TTF text, a themeable style engine, taskbar, start
  menu, and desktop widgets.
- **Applications** - Files, Terminal, Settings, a text editor, calculator,
  image viewer, an IRC client, a web browser, a music player, Task Manager, and
  **Maytera Studio**, an image editor with layers, masks, blend modes,
  channels, paths and a large filter set.
- **Rust in the kernel** - an incremental port. Live today: the IP, TCP and UDP
  checksums, the ext2 directory parser, the ICMP/ARP/DNS/DHCP/URL parsers, the
  ELF and PE loaders, several hashes and ciphers, the syscall argument table,
  and the clipboard. Each ported piece keeps its C original for rollback and is
  checked against it with a differential test. See `kernel/RUST_PORT_LEDGER.md`,
  which records the honest per-component status including where a result was
  weaker than it first looked.
- **Networking + filesystems** - an in-house TCP/IP stack (ARP, IP, UDP, TCP,
  DHCP, DNS), TLS 1.3 with certificate and signature validation, SSH
  client/server; ext2 and FAT32 (VFAT long names) behind a VFS.
- **Compatibility layers** - a Win16 (Windows 3.x NE) interpreter and a DOS
  layer. These are *runtimes*, not bundled software: they run applications you
  supply yourself. **No third-party application ships with this repository.**

Where things are partial, they are marked partial rather than dressed up. The
browser's JavaScript is a ported Duktape, so it is ES5.1-era with no event
loop: it evaluates scripts but does not drive an interactive, event-driven page.

## Security

This section states what is actually implemented, and what is not. Several
neighbouring features are easy to confuse with each other, so they are listed
separately rather than summarised as "hardened".

**Implemented and live:**

- **Userland PIE with load-base randomisation.** `kernel/exec/elf.c`
  randomises the load base of every PIE user image, drawing from the kernel
  HMAC-DRBG.
- **Stack canaries.** The kernel builds with `-fstack-protector-strong
  -mstack-protector-guard=global`, and `__stack_chk_guard` is seeded with a
  real random value at boot rather than left at a constant.
- **An image-wide W^X / ELF-shape gate.** Every launched binary is checked
  against exactly the rule the kernel's own loader enforces, encoded once in
  `tools/elf-shape-check.sh` so the gate and the loader cannot drift apart. It
  has a self-test that proves it goes red on a bad shape.
- **TLS 1.3 with certificate *and* signature validation.** Skipping either was
  a real gap once; it is closed.
- **Capability tokens, consent gating and an append-only audit trail** for
  AI-initiated actions.

**Explicitly NOT implemented. Do not assume otherwise:**

- **KASLR. The kernel base is fixed.** `linker.ld` pins `KERNEL_PHYS_BASE` and
  nothing relocates it. `kernel/security/aslr.c` reports this honestly at boot
  ("Kernel (KASLR): not implemented").
- **Stack, heap and mmap randomisation.** Only the PIE image base is
  randomised. The per-process randomisation API that used to imply otherwise
  had zero callers and has been deleted rather than left standing as an implied
  feature.
- **A non-root desktop.** Work is in progress; this release still runs the
  desktop as root. The default accounts are `root`/`root` and `admin`/`admin`,
  created on first boot. Change them.

**About the Nova prompt-injection guard:** it is a **keyword and pattern
ruleset**, not a semantic model. It raises the cost of the obvious injection
strings. It does not understand intent and must not be treated as a boundary
you can rely on. The capability token, the consent prompt and the audit trail
are the real controls.

Please report security issues as described in `SECURITY.md`.

## Repository layout

```
kernel/      Freestanding kernel: mm/ cpu/ proc/ exec/ fs/ net/ crypto/
             video/ drivers/ gui/ security/ rustkern/ and kernel/tools/
             (the build-time link gates)
userland/    libc/ (freestanding C library + crt0), libgl/ (TinyGL),
             user.ld linker script, and apps/ (each builds a Ring 3 ELF)
boot/uefi/   UEFI bootloader source (BOOTX64.EFI)
disk/        Disk template: CONFIG (incl. start-menu drop-ins and THEME.CFG),
             THEMES, FONT-LICENSES
docs/        Design documents and the generated Win16 API reference
tools/       Build-time helpers (ELF shape gate, theme lint)
tests/       Serial-driven integration test framework
screenshots/ Images used by this README
```

## Building

MayteraOS builds with a GCC cross toolchain targeting freestanding x86-64.

```sh
sudo apt install build-essential nasm gnu-efi mtools dosfstools gdisk
```

`gnu-efi` provides the `efi.h` headers the UEFI bootloader needs. A few
userland apps are written in Rust and need a pinned `rustc` (see their
`rust-toolchain.toml`); `build.sh --all-apps` reports those as failures at the
end and carries on, so the rest of the system still builds without a Rust
toolchain.

```sh
git clone https://github.com/referefref/maytera-os.git
cd maytera-os
./build.sh --all-apps          # libc, then libgl, then kernel, bootloader, apps
sudo SIZE_MB=512 IMG=/tmp/maytera.img ./stage-disk.sh
```

`build.sh` builds `userland/libgl` (TinyGL) before the app loop on purpose: the
3D apps link `../../libgl/libgl.a`.

**Not everything builds from a clean clone, and the script tells you so.**
`build.sh` lists the optional apps that failed at the end rather than failing
the whole build or hiding it. Known gaps:

In the build that produced this release, exactly **two** of the 145 apps
failed, measured on Debian 12 with gcc 12 and rustc 1.97.0:

| App | Why |
|---|---|
| `browser` | Needs a NetSurf port and a Duktape build that are not in this repository. Its Makefile points at absolute paths outside the repo. Known gap. |
| `ipc_test` | Its Makefile is missing an `-isystem` flag and fails on `stddef.h`. A real bug in the test app, not a missing prerequisite. |

`stage-disk.sh` installs whatever ELF executables it finds, so an app that
failed to build is simply absent from the image rather than silently replaced
by a stale copy.

### Assembling a bootable image

```sh
sudo ./stage-disk.sh                 # writes ./boot_disk.img
```

Boot it with:

```sh
qemu-system-x86_64 -machine pc,accel=kvm -cpu kvm64 -m 2G \
    -bios /usr/share/OVMF/OVMF_CODE.fd \
    -drive file=boot_disk.img,format=raw,if=ide -serial stdio
```

Use `-cpu kvm64`, **not** `-cpu host`. Passing through the host CPU exposes AVX,
which this kernel does not save or restore across a context switch (it builds
soft-float with SSE disabled), and the compositor crashes.

Two categories of content are deliberately **not** in this repository, and the
image is usable without either:

- **Large binary assets** (wallpapers, the full font set, boot art). Point the
  script at your own with `WALLPAPERS_DIR=/path/to/bmps`. The kernel falls back
  to a gradient desktop when a wallpaper is absent.
- **Third-party software and game data** (DOOM's `DOOM1.WAD`, any Windows or
  DOS application). These are other people's copyrighted work and are not ours
  to redistribute. Supply your own, for example
  `DOOM_WAD=/path/to/DOOM1.WAD ./stage-disk.sh`.

## Licensing

MayteraOS is released under the GPL v2 (see `LICENSE` and `COPYING`).
Third-party components and their licences are listed in `ATTRIBUTION.md`.

Some in-development ports are **not** included in this repository because their
third-party licence obligations have not yet been discharged. They are absent
on purpose, not by oversight: `assaultcube`, `openarena`, `vi`, `grep-gnu` and
`curaslice`. Each depends on upstream code under GPLv3, AGPLv3, or a non-free
licence, and shipping them without their licence texts would be a violation.
They will return once that compliance pass is done properly.

Font licences ship with the fonts, under `disk/FONT-LICENSES`, as the SIL Open
Font License requires.

## Releases

Bootable images are published under this repository's
[Releases](https://github.com/referefref/maytera-os/releases).

### Verify what you downloaded

```sh
sha256sum maytera-os.iso
```

The expected checksum is published in the release notes. Check it before you
write the image to anything.
