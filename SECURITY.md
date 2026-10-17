# Security Notes

MayteraOS is a hobby operating system, not a production system. Treat it accordingly.

## Threat model

**What this project is:** a teaching/learning vehicle for OS internals. The kernel boots, paints a desktop, runs Ring 3 apps, serves TLS, speaks SSH, and does enough to be fun to hack on.

**What this project is not:** audited, production-grade, or suitable for running on the public internet. Many subsystems implement "the happy path" and do not yet enforce the invariants a production kernel would. Assume that a motivated attacker with network reach or local code execution can escalate.

If you deploy this anywhere network-reachable, put it behind a firewall and treat it as permanently untrusted.

## Credentials

**MayteraOS ships with no default login credentials.** There is no shipped
`root` or `guest` password and no always-on network login. On first boot, the
setup wizard (OOBE, `userland/apps/setup`) has you create the first
administrator account and set its password; account records live on the ext2
root under `/CONFIG` (the SHADOW store), not baked into the kernel.

### SSH server (`kernel/net/ssh/`)

The from-scratch SSH server is **compiled out unless the kernel is built with
`MAYTERA_SSHD=1`, and it is off by default.** A stock build does not listen for
SSH at all (`kernel/main.c` prints "not built into this kernel" when it is not
compiled in). If you do build it in, it is a minimal, unhardened implementation:
configure your own accounts and put it behind a firewall before exposing the VM
to any untrusted network. Do not rely on it for anything that matters.

## What we found and fixed

This project publishes its own security defects, including the ones nobody reported
and the ones that were our own fault. Two places carry that record:

- **`kernel/SECURITY_ADVISORIES.md`** in this repo: the issuing ledger. One entry per
  advisory, in discovery order, with the affected component, the CWE class, the
  reachability (remote versus local), what the fix actually changes, and the build
  range. It also keeps **withdrawn** ids rather than reusing them, so an id always
  means one thing.
- **<https://maytera.net/security>**: the same set presented for reading, grouped by
  class, with the patched build for each.

Three classes are held to different evidence standards, deliberately, because they
are not the same kind of claim:

| class | what it means | what counts as evidence |
| --- | --- | --- |
| memory safety | a crafted input makes the code over-read or over-write | usually AddressSanitizer, and the entry says where that is insufficient |
| access control | a missing authorization check, nothing corrupted | the caller could ask for something it should never have been able to ask for |
| availability | a crafted input causes grossly disproportionate work | a **measured** before-and-after on the running system, not a reasoned argument |

Two habits in that ledger are worth stating, because they are the point of keeping it:
an advisory is only filed if the defect is **reachable**, so defence-in-depth
hardening of already-correct code does not inflate the count; and where an entry was
later found to be wrong, it is **corrected in place and the correction is visible**
rather than quietly rewritten.

## Known insecurities

This is a non-exhaustive list of things a security reviewer would flag. Some have TODO comments in the code; others don't yet.

- **Capability enforcement exists but is NOT comprehensive.** This bullet used to say the dispatcher does not check caller capabilities at all. That is no longer true: `syscall_cap_check()` is a chokepoint in the syscall dispatcher (`kernel/proc/syscall.c`) and gates a named set of capabilities, currently `screen.capture`, `screen.stream`, `serial.port`, `input.inject`, `input.observe`, `net.connect`, `net.listen` and `device.eject`, each obtainable only through a compositor-drawn consent prompt and recorded as a journalled grant. **Most syscalls are still ungated**, and a capability a program already holds is time-bounded but broad, so treat this as a working mechanism covering specific high-risk surfaces rather than a sandbox. The advisories for the gaps this closed are in the ledger linked above.
- **User-copy helpers are rudimentary.** `copy_from_user` / `copy_to_user` validate page presence but do not exhaustively defend against TOCTOU races between the check and the access.
- **No W^X in all regions.** User stacks and heaps are RW, but some legacy allocations in the kernel are still RWX.
- **ASLR is implemented but the entropy source is weak** when RDRAND is not available.
- **TLS 1.3 is a from-scratch implementation.** It handles the common handshake but has not been fuzzed or tested against every extension. Do not use it for anything that matters.
- **SSH server is a from-scratch implementation.** Same caveat. Key exchange, channel handling, and auth are minimal. It is off by default (compiled out unless built with `MAYTERA_SSHD=1`).
- **No memory randomization on the kernel heap.**
- **`kernel/dos/` is a tiny DOS emulation layer.** It exists to run a few 16-bit toys and should not be considered sandboxed.
- **DOOM is compiled with `-w`** (all warnings suppressed). This is intentional (legacy code, thousands of implicit-int warnings) but means undefined behavior in that subtree won't be caught by the build.

## Reporting issues

This is a hobby repo without a formal disclosure process. If you find a serious issue, open a GitHub issue (or, for things you'd rather not say in public, contact the maintainer via the profile linked on the repo).

## Credentials / infrastructure this repo does NOT contain

For the record, the following were stripped before publication and should never reappear in the repo history:

- Any hardcoded SSH passwords for build/deploy infrastructure.
- Any deployment scripts that bake in a remote host IP or credential.
- Any internal IP address for the primary development build server.
- Any `apitoken`, `osbuilderkey`, or similar credential files.
- Any backup files (`*.bak`, `*.buildserver`, `*.broken`, etc.) that might contain older versions of the above.
- The `DOOM.WAD` file (not GPL, not redistributable).

If you spot any of these leaking into a commit, please open an issue immediately so history can be scrubbed.

## Code intentionally excluded from this distribution

- **DOOM WAD files.** Only the GPLv2 DOOM source is included (in `userland/apps/doom/`). You must supply your own WAD. The shareware `DOOM1.WAD` is legal to download; commercial WADs require a licensed copy of the game.
