# Security Notes

MayteraOS is a hobby operating system, not a production system. Treat it accordingly.

## Threat model

**What this project is:** a teaching/learning vehicle for OS internals. The kernel boots, paints a desktop, runs Ring 3 apps, serves TLS, speaks SSH, and does enough to be fun to hack on.

**What this project is not:** audited, production-grade, or suitable for running on the public internet. Many subsystems implement "the happy path" and do not yet enforce the invariants a production kernel would. Assume that a motivated attacker with network reach or local code execution can escalate.

If you deploy this anywhere network-reachable, put it behind a firewall and treat it as permanently untrusted.

## Default credentials (READ THIS)

### SSH server (`kernel/net/ssh/`)

The built-in SSH server ships with a **hardcoded default user table** in `kernel/net/ssh/ssh_auth.c`:

| Username | Password |
|----------|----------|
| `root`   | `maytera` |
| `guest`  | `guest`  |

### Remote control shell (`kernel/net/remote_ctrl.c`, TCP 2323)

The remote control service is a second, simpler text protocol that listens on **TCP port 2323** and provides an interactive shell after login. It ships with its own hardcoded credentials in `kernel/net/remote_ctrl.h`:

| Username | Password |
|----------|----------|
| `admin`  | `maytera` |

This service is enabled by default at boot from `kernel/main.c`. It implements three-attempt rate limiting, a 2-second sleep between failed attempts, and a constant-time password comparison, but it is still a plain-text shell on a non-TLS socket. Anyone with network reach can sniff credentials and/or attach to the session.

### These exist so the services are usable out of the box during development. They are the first thing an attacker will try.

Before exposing a running MayteraOS VM to any untrusted network:

1. Edit `kernel/net/ssh/ssh_auth.c` and replace the `users[]` table with your own entries. SSH passwords are stored as SHA-256 hex. Regenerate with:
   ```bash
   printf '%s' 'your-new-password' | sha256sum
   ```
2. Edit `kernel/net/remote_ctrl.h` and change `REMOTE_CTRL_USER` / `REMOTE_CTRL_PASS`, or disable the service entirely by not calling `remote_ctrl_start()` in `kernel/main.c`.
3. Rebuild the kernel and redeploy. There is currently no runtime user database for either service; changing credentials requires a rebuild.

Alternatively, disable the SSH server and the remote control shell entirely by not calling `sshd_start()` and `remote_ctrl_start()` in `kernel/main.c`, or compile them out of the build.

## Known insecurities

This is a non-exhaustive list of things a security reviewer would flag. Some have TODO comments in the code; others don't yet.

- **No seccomp/capabilities enforcement at the syscall layer.** The `kernel/security/` directory has scaffolding (`capability.c`, `validate.c`) but the dispatcher does not consistently check caller capabilities.
- **User-copy helpers are rudimentary.** `copy_from_user` / `copy_to_user` validate page presence but do not exhaustively defend against TOCTOU races between the check and the access.
- **No W^X in all regions.** User stacks and heaps are RW, but some legacy allocations in the kernel are still RWX.
- **ASLR is implemented but the entropy source is weak** when RDRAND is not available.
- **TLS 1.3 is a from-scratch implementation.** It handles the common handshake but has not been fuzzed or tested against every extension. Do not use it for anything that matters.
- **SSH server is a from-scratch implementation.** Same caveat. Key exchange, channel handling, and auth are minimal.
- **Remote control shell on TCP 2323 is plaintext.** No transport encryption, hardcoded credentials, runs by default. See above.
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
