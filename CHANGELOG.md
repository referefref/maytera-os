# Changelog

This is a curated public changelog. It summarises what changed between public
releases; it is not the exhaustive internal commit log.

## v2.0.2, kernel build 2285 (2026-09-02)

The previous entry here described kernel build 1761, and the previous
**downloadable** release was v1.95.0 build 851. Those are not the same thing:
build 1761 was written up but no binary was ever published for it, and a source
push landed on 2026-08-28 without a release either. This entry therefore covers
everything back to build 1761, and this is the first published image since
build 851.

Identify this build by its **kernel build number, 2285**. Unlike previous
releases, that number is also what `MAYTERA_BUILD_NUMBER` reads in
`kernel/version.h` in this tree, because the image was built from the published
source rather than from an internal build. Earlier releases could not say that:
the internal build system stamps `max(build-state, version.h) + 1`, so the
source constant read lower than the image it produced. `/BUILDINFO.TXT` on the
image records the commit it was built from, and that commit is the identifier
to trust.

This release publishes a compressed disk image and no `.iso`. MayteraOS has no
ATAPI read path and no ISO9660 support on any boot path, so an `.iso` from this
project would be a disk image with the wrong extension: it would fail in a real
optical drive, or in a VM told to boot one, in a way that looks like a corrupt
download rather than an unsupported medium. The README names the files in the
kernel that make that true.

### Security

- **Three Ring-3 privilege-boundary fixes.** A process could read or write
  another process's legacy file descriptors across the process boundary
  (`kernel/rustkern/fdown.rs`, proven reachable at a live file descriptor); a
  non-owning process could attach to someone else's `/dev/pts/N` pseudo-terminal
  (`kernel/rustkern/ptsown.rs`); and `sys_win_blit()` handed Ring 3 an
  arbitrary kernel read with no bounds or ownership check at all
  (`kernel/rustkern/winblit.rs`). All three are now gated by explicit ownership
  and bounds checks at the syscall boundary.
- **Known follow-on regression, fixed.** The `sys_win_blit()` hardening above
  slowed the scaled-blit path enough, on a single core, to cause visible stale
  frames when a window was maximised or full-screen: the fix had replaced one
  zero-copy read with one bounds-checked copy per source row. The row copies
  are now batched into chunks of consecutive source rows instead of one row at
  a time, which reads the same total bytes in far fewer boundary crossings and
  removes the regression without reopening the original hole.
- **Password policy, enforced at one chokepoint instead of one screen.**
  Previously the only rule anywhere in the system was a 6-character minimum
  applied at first-boot account creation. Every path that can set a credential
  (`passwd`, Settings' Add User, the first-boot wizard, the login screen) now
  goes through `user_set_password()` in `kernel/proc/users.c`, which enforces
  an 8-127 byte length rule, rejects a password containing the username,
  rejects low-entropy and keyboard-run patterns, and rejects any password on a
  local breached-password list (the top 50,000 RockYou passwords, stored as a
  66 KB table of truncated hashes, not plaintext, checked with no network
  call). There is deliberately no character-class composition rule, following
  NIST SP 800-63B: composition rules push users toward predictable,
  breach-list-friendly transforms.
- **An experimental, opt-in sandbox for legacy DOS and Win16 titles.** The DOS
  interpreter can now run as an ordinary unprivileged Ring 3 process
  (`/APPS/DOSUSER`, armed via `/CONFIG/DOSRING3.CFG`) instead of inside the
  kernel. Measured directly: the host was refused every one of five tried
  credential files and two directory-escape attempts, with positive controls
  proving the probe could read what it should. Structurally this closes a real
  gap: the in-kernel DOS filesystem path enforced credentials at 15
  hand-placed checks across 94 file-open call sites, any of which could be
  missed by a future change; the Ring-3 host enforces at the syscall boundary
  for all 94, by construction. This is not yet the default: the in-kernel DOS
  path still runs every title unless the config file above is present.

### Kernel

- **The kernel is single-threaded by construction, and it is now measured.**
  With multi-core scheduling on, the Big Kernel Lock is held 96% of wall clock
  at 4 vCPUs, and 85-88% of that hold time is one call site:
  `syscall_entry` takes the lock before dispatch and releases it after, so it
  wraps the entire body of every syscall. One process observed holding it for
  157 of a 230-second run, in 41,722 acquires. A fair ticket-lock
  implementation of the same primitive is measurably fairer but changes
  present rate, latency, and host CPU by nothing at all, confirming the lock's
  granularity, not its fairness, was the problem; it ships as an opt-in
  experiment (`/BKLFAIR.TXT`), default off.
- **The first real narrowing, and it is now the default.** `sys_win_blit()`'s
  per-row copy loop now runs with the Big Kernel Lock dropped, protected
  instead by a per-window pin that defers (rather than blocks on) freeing the
  buffer it is reading, and revalidates every cached pointer after
  reacquiring the lock. Combined with letting a waiter on the lock park
  instead of spin, measured at 4 vCPUs: compositor present rate rose from 8.5
  to 30.5-31.5 per second, host CPU usage fell 43%, and input-to-photon
  latency fell from 32.8 ms to 8.2 ms. Both changes are now default on
  (`/NOBLITNARROW.TXT` and `/NOBKLPARK.TXT` are the opt-out controls); zero
  frames were dropped by the narrowing across roughly 130,000 unlocked blits
  measured.
- **This is a first step, not a fix.** Even narrowed, the lock still occupies
  roughly 71-73% of wall clock at 4 vCPUs. `sys_win_draw_image()`, which shares
  the same shape, is the next identified candidate and is not yet narrowed.
- **Every timed sleep in the kernel had 40 ms real resolution, not the 4 ms
  the 250 Hz tick rate implies.** The only code path that wakes a sleeping
  process ran solely on time-slice expiry (40 ms), not on every tick, so a
  `sys_sleep(8)` call on an idle machine actually returned after roughly
  40 ms. Fixed by expiring the current time slice early when a sleeper's
  deadline has already passed; measured to roughly double the single-threaded
  compositor's present rate (about 20 to 43-46 per second) at unchanged host
  CPU, with no measurable cost when the machine is idle.
- Continued incremental Rust port under the existing strangler pattern:
  new components this cycle include the audio-underrun accounting described
  below, the password-policy engine, the DOS launch-routing policy matcher,
  and the three privilege-boundary checks above. Each keeps its C original
  behind a build flag for rollback.

### Drivers and audio

- **A real audio dropout counter that had always read zero.** On at least one
  real laptop, audio audibly stuttered while the underrun counter reported
  nothing, because the code that would have incremented it ran the DMA-ring
  repair first and never recorded that a repair had been necessary. Fixed:
  playback now prefills the ring before starting the engine and paces itself
  against actual buffer occupancy instead of a fixed clock interval, and the
  counter is proven to fire (not just read as zero) under a deliberately
  induced 1.5-second dropout. A regression introduced by an early version of
  this fix, where the first playback chunk of every file could be silently
  discarded and produce an audible skip, was caught in review before shipping
  and fixed by checking for ring space before decoding rather than after.
- The Ring-3 DOS host above now reaches real digitised audio and Sound
  Blaster FM synthesis, verified against captured waveforms compared to the
  in-kernel reference path. A related bug where closing a DOS guest's window
  did not stop its FM synthesiser, leaving it consuming a full CPU core after
  the window was gone, is fixed.

### Networking

- **A single unreachable background address could take the whole machine's
  outbound HTTP offline, indefinitely.** The connectivity circuit breaker that
  protects against retry storms had no path for a client to notice it had
  recovered: once six consecutive failures tripped it, the browser, the
  weather widget, and every subsequent request stayed refused, while `ping`
  kept working the whole time because ICMP was never gated. The state was
  self-latching because nothing ever took the one recovery probe the breaker
  held open every 30 seconds. Fixed: the two background services that could
  get stuck now make their own bounded recovery attempts, and the breaker
  records and logs which host tripped it. `ping` now accepts a hostname
  (previously only a literal address), and a new `nslookup` app ships so a DNS
  problem can be told apart from this breaker without a serial console.

### Desktop and applications

- **Alt+Tab task switching is wired to the keyboard.** The switcher
  application itself already shipped; it had no keyboard chord bound to it.
  Two real cycling bugs were fixed at the same time: repeated Tab presses
  during a held Alt did nothing past the first press, and Shift+Tab always
  cycled forward instead of reversing.
- Terminal rendering is now damage-tracked instead of repainting the entire
  grid on every 10 ms input tick; a full 170x52 terminal previously issued on
  the order of 8,800 draw calls per repaint regardless of how little had
  changed.
- Fixed a compositor input bug where a fast burst of keystrokes into a
  compositor-native text field silently collapsed to only the last key typed.

### Building from a clean clone

Two defects that only ever showed up in a clone of THIS repository, never in
internal builds, are fixed here. Both were found by building the published tree
rather than by reading it.

- **`userland/ports/mports.sh` poisoned its own compiler flags** whenever it was
  reached from a parent `make`, which is exactly how `./build.sh` reaches it.
  It captures `make` output into a variable that becomes `CFLAGS`; `MAKEFLAGS`
  inherited from the parent re-enabled make's "Entering directory" messages,
  overriding the local `-s`, and those words were then passed to gcc as input
  filenames. Every ports-based app failed, `terminal`, `less`, `sed`, `lua` and
  `zlibtest` among them. Fixed with `--no-print-directory`.
- **`kernel/rustkern/pwbreach.bin` was missing from the previous source push.**
  The publish tooling had cleared it, and then the repository's own `.gitignore`
  rule for `*.bin` silently dropped it from the push. It is source, not a build
  output: `kernel/rustkern/pwpolicy.rs` includes it and `kernel/Makefile` names
  it, so a clean clone could not build the Rust kernel library. The ignore rule
  now has an explicit exception for it.

### Known gaps

- **Multi-core scheduling (SMP) remains implemented but off by default**
  (`/SMPSCHED.TXT`). Even with this release's Big Kernel Lock work, enabling
  it still costs several times the host CPU for less throughput on the
  workloads measured, because the lock now covers a smaller region rather
  than none. It should stay off until more of the lock's remaining footprint
  is narrowed.
- The Ring-3 DOS/Win16 sandbox described above is real and measured, but it
  is opt-in. The in-kernel DOS interpreter, with its narrower and
  hand-maintained set of credential checks, remains the default execution
  path for every DOS and Win16 title.
- The Big Kernel Lock still holds roughly 71-73% of wall clock at 4 vCPUs
  after this release's first narrowing; one syscall was addressed, at least
  one comparable one was not.

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
