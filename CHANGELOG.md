# Changelog

This is a curated public changelog. It summarises what changed between public
releases; it is not the exhaustive internal commit log.

## v1.95.0, kernel build 1761 (2026-08-08)

Previous public release: v1.95.0, kernel build 851. That is 910 kernel builds
and roughly three weeks of development.

The version string is unchanged from the previous release because the internal
version number was not bumped for this build. Identify this release by its
**kernel build number, 1761**, not by the version string.

### Security

- **TLS 1.3 now validates both the certificate chain and the signature.**
  Skipping signature validation was a real gap and it is closed.
- **Userland PIE with load-base randomisation.** The kernel randomises the load
  base of every PIE user image from its HMAC-DRBG.
- **Stack canaries are live in the kernel**, built with
  `-fstack-protector-strong -mstack-protector-guard=global`, with
  `__stack_chk_guard` seeded from real entropy at boot rather than left
  constant.
- **An ELF-shape / W^X gate now runs at build time**, encoding exactly the rule
  the kernel loader enforces so the two cannot drift apart. It ships as
  `tools/elf-shape-check.sh` with a self-test that proves it goes red on a bad
  shape. This closed a Ring 3 to Ring 0 denial of service where three shipped
  apps were linked without the user linker script.
- **Honest correction, not a new feature:** a per-process ASLR API that
  advertised stack, heap and mmap randomisation had **zero callers** and could
  never have worked as written (its mmap base sat outside the reachable user
  window). It has been deleted rather than left standing as an implied feature.
  `kernel/security/aslr.c` now reports the true state at boot.
- **KASLR is still not implemented.** The kernel base is fixed by the linker
  script and nothing relocates it. Do not assume otherwise.
- **The non-root desktop is still in progress and does not ship.** The desktop
  runs as root in this release.

### Kernel

- A concurrency lint now **fails the kernel build** on any new busy-wait or
  poll loop, with a self-test proving it goes red on a synthetic spin and green
  on correct wait-queue code. Existing sites are baselined and each baseline
  entry must carry a justification or the lint fails.
- A blocking-context assertion fires at the single chokepoint every
  `wait_event` form reaches before sleeping, catching attempts to block with
  interrupts off or before the scheduler is live. Known gap, stated plainly:
  holding a plain non-`irqsave` spinlock is **not** detected.
- Continued incremental Rust port. Live in Rust today: the IP/TCP/UDP
  checksums, the ext2 directory parser, the ICMP/ARP/DNS/DHCP/URL parsers, the
  ELF and PE loaders, several hashes and ciphers, the syscall argument table
  and the clipboard. Each keeps its C original for rollback behind a
  differential test. `kernel/RUST_PORT_LEDGER.md` records the per-component
  status including where a result was weaker than it first looked.
- Filesystem correctness: opening a file `O_RDWR` no longer truncates it,
  `rename` relinks instead of copying, and a failed FAT write now surfaces at
  `close()` instead of being swallowed.
- ext2 is supported as the root filesystem, opt-in via a `/ROOTEXT2` marker on
  the EFI System Partition.

### Desktop and applications

- A theme format (`.mtheme`) with a documented on-disk spec and a design
  contract lint; thirteen themes ship under `disk/THEMES`.
- Start-menu content is now data as well as code: drop-in `.MENU` files under
  `disk/CONFIG/STARTMENU/SYSTEM.D` are merged at runtime.
- Notification service with a tray bell, toasts and per-app control.
- DOOM now builds as a **separate userland ELF** rather than being compiled
  into the kernel binary. Besides being the right structure, this removes a
  licence conflict where DOOM shared one binary with GPLv2 media decoders.
- Font licences now ship next to the fonts under `disk/FONT-LICENSES`, as the
  SIL Open Font License requires.

### Removed from this repository

Five in-development ports are **absent on purpose**: `assaultcube`,
`openarena`, `vi`, `grep-gnu` and `curaslice`. Each depends on upstream code
under GPLv3, AGPLv3 or a non-free licence, and shipping them without their
licence texts would be a violation. They return when that compliance pass is
done properly. See the Licensing section of the README.

`docs/LICENSES.md` has also been removed: it was dated January 2026, described
the kernel as "Proprietary", and contradicted the GPL v2 licence this project
is released under. It needs a rewrite, not a copy.

### Known gaps

- `browser` does not build from a clean clone: it needs a NetSurf port and a
  Duktape build that are not in this repository. `ipc_test` fails on a missing
  `-isystem` flag. Those are the only two failures out of 145 apps, down from
  seven in the previous release. `build.sh` reports them at the end rather than
  hiding the failure.
- The browser's JavaScript is a ported Duktape: ES5.1-era, no event loop.
- The Nova prompt-injection guard is a keyword ruleset, not a semantic model.
