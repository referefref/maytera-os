# Secure Multi-User Login and Lock Screen: Design

Status: PARTIALLY IMPLEMENTED. Sections 1.1 (no default credentials in a
shipping image) + 4.1 step 2 (first-boot create-account flow) are implemented in
`kernel/proc/users.c` (`users_create_first_admin`, `users_count_active`, the
`MAYTERA_SHIP_DEFAULT_ACCOUNTS`-gated `create_defaults`) and `kernel/gui/login.c`
(`LOGIN_STATE_CREATE_ACCOUNT`), verified on VM <vmid> build 855 (#568). The initial
administrator is created as uid 0 via the #566 PBKDF2 path; a fresh install with no
accounts on disk forces creation, while an internal image that ships accounts +
`/CONFIG/LOGIN.CFG autologin=<user>` still boots straight to the desktop. Other
sections (RC port 2323, kernel_shell reachability, per-user profiles, lock flow)
remain design-only.
Status of the rest: PROPOSED (design only, not yet implemented)
Author: UI/UX Specialist, 2026-07-22
Companion mockup: `docs/mockups/login-mockup.html`

This document is grounded in the actual current MayteraOS source tree (the
git source of truth). Every claim about "today the system does X" cites a
real file and line so a future implementer can verify it rather than trust
this prose. Where the design needs a kernel change, it says so explicitly.
Where it does not, it says that too.

## 0. Executive summary: where we actually are today

Read literally, MayteraOS today does **not** have a working login gate, and
has at least one hardcoded-credential remote command channel that is a worse
version of the Windows utilman.exe trick. Specifically:

- The kernel *does* run a real login screen before the desktop
  (`kernel/main.c:1580-1585` calls `login_init()`/`login_run()`, and
  `kernel/main.c:1667` only calls `desktop_run()` after it returns). This is
  the one authentication gate that is real today.
- But the userland compositor process that actually renders the desktop
  (`/APPS/COMPOSIT`) has its **own**, second, login implementation
  (`userland/apps/compositor/login.c`) that is **never called**:
  `userland/apps/compositor/main.c:1167-1173` comments out `login_run();`
  and hardcodes `g_logged_in = true; g_login_uid = 0;` ("root") every boot,
  regardless of who (if anyone) authenticated at the kernel login screen a
  moment earlier. Two parallel "am I logged in" states exist and they are
  disconnected.
- Default accounts `root`/`root` and `admin`/`admin` are auto-created on
  first boot with no forced change (`kernel/proc/users.c:363-404`).
- Password hashing is a single unsalted-in-the-cryptographic-sense SHA-256
  round (`kernel/proc/users.c:60-89`), GPU-crackable, with no rate limiting
  at the syscall layer at all.
- Autologin (`/CONFIG/LOGIN.CFG`) logs a named user in with **no password
  check whatsoever** (`kernel/gui/login.c:578-655`,
  `userland/apps/compositor/login.c:211-232`), and that config file lives on
  the world-writable-by-design FAT boot partition.
- A TCP service on port 2323 ships in every kernel build, listens the
  instant the network is up, and authenticates with a **hardcoded**
  `admin`/`maytera` credential pair unrelated to any OS account
  (`kernel/net/remote_ctrl.h:15-17`), granting a ring-0 shell with file read,
  GUI click/scroll injection, and process control.
- If the desktop process ever exits (crash, kill, anything), the kernel
  falls back to an **unauthenticated** ring-0 shell with disk/memory/network
  access (`kernel/main.c:1668-1680`).

None of this is a criticism of prior work in isolation - `login_run()` was
disabled with an honest `// TODO: Re-enable login once desktop is fully
working` comment while the desktop was being built, and RC's own comment
calls it a stopgap ("ROADMAP: User-mode version planned"). But it means
"secure by design, no bypasses" is not a small tweak on what exists - it is
the actual project. Sections 2 and 3 below treat each of these as a named
threat with a specific fix. Section 1 covers the multi-user model these
fixes sit on top of, and Section 4 is the visual/UX spec.

---

## 1. Multi-user model

### 1.1 Accounts (mostly exists; needs hardening, not invention)

MayteraOS already has a real Unix-style account system:

- `kernel/proc/users.c` / `kernel/proc/users.h`: `/CONFIG/PASSWD` (username,
  uid, gid, home, display name, shell), `/CONFIG/SHADOW` (username + password
  hash), `/CONFIG/GROUP` (groups + membership). `MAX_USERS = 32`.
- Real per-process privilege separation already exists at the kernel layer:
  `kernel/gui/desktop.c:86-93` (`desktop_set_session`/`desktop_get_session_uid`)
  sets which uid the compositor's spawned session belongs to, and
  `kernel/gui/desktop.c:142-144` sets the launched child process's real
  `uid`/`euid` to that value. Privilege checks downstream
  (`sys_delete_user` at `kernel/proc/syscall.c` checking `p->euid != 0`;
  `sys_passwd_change` at `kernel/proc/syscall.c:4741-4757` requiring the old
  password unless `euid==0`; `perms_check()` gating `sys_open()` at
  `kernel/proc/syscall.c:1891`) are enforced against this real kernel-tracked
  identity, not against anything a userland app claims about itself. This is
  the right foundation and the design keeps it as the single source of
  truth for "who is allowed to do X."
- `userland/apps/settings/main.c` already has a Users & Accounts panel:
  Add User / Remove / Change Password modals exist today (`draw_users_panel`
  around line 2959, `MODAL_ADD_USER`/`MODAL_CHANGE_PASSWORD` handling around
  lines 4225, 5693-5730), backed by real syscalls: `sys_adduser` (`SYS_ADDUSER
  = 132`, `kernel/proc/syscall.c:4786`) and `sys_passwd_change`
  (`kernel/proc/syscall.c:4741`, correctly requires the caller's own old
  password unless root).

**What the design adds on top:**

- **Per-user profile**: avatar (letter-avatar already exists -
  `draw_avatar()` in both login implementations draws the user's first
  initial in a colored circle; extend to an optional real picture from
  `<home>/PICTURES` with graceful fallback to the initial), and a per-user
  wallpaper choice, applied as the *first* visible thing after a successful
  authentication (today wallpaper is a single global set once at compositor
  boot via `wallpaper_init()`, `userland/apps/compositor/main.c` -
  `compositor_init()`; the design makes it session-scoped, read from the
  authenticated user's profile before the desktop paints).
- **No default credentials in a shipping image.** `create_defaults()`
  (`kernel/proc/users.c:363-404`) must not ship `root/root` and
  `admin/admin` as the golden's live credentials. First-boot flow instead
  forces the interactive session to *set* a password for the first
  (administrator) account before the desktop is reachable at all - the login
  screen itself becomes the "create your account" screen on a fresh install,
  the same way real consumer OSes handle first boot. This closes CVE-class
  "known default password" the same way the project's own SSH/EXTSVC
  credential-leak lessons already argue for (see `blame.md`'s incident
  history, referenced from CLAUDE.md; not re-litigated here).
- **What "a session" is.** One authenticated uid owns exactly one running
  compositor process and its child app windows at a time (this matches the
  current architecture: `kernel/gui/desktop.c:2996` spawns exactly one
  `/APPS/COMPOSIT` per `desktop_run()` call, after login). Switching users in
  v1 means: lock or log out the current session, return to the login screen,
  authenticate as the other user, and a **fresh** compositor process is
  spawned with the new user's real `euid` (same mechanism as today,
  `desktop.c:142-144`, just re-entered). This gives genuine OS-level
  isolation between users (new process, new address space, new real euid)
  rather than a cosmetic "logged in as" label. **Fast user switching** (both
  sessions kept alive, instant toggle with a hotkey) is explicitly *out of
  scope for v1*: it would require a new kernel concept of multiple
  concurrently-live sessions/compositor processes and a session list, which
  does not exist today and should not be bolted on as a side effect of a
  security feature. Flagged as a v2 idea in Section 4.3.

### 1.2 Session identity must be unified, not duplicated

Today there are two independent "am I logged in, and as whom" states: the
kernel's (real, gates `desktop_run()`) and the compositor's own
`g_logged_in`/`g_login_uid`/`g_login_username` globals
(`userland/apps/compositor/compositor.h:491-493`, defined
`userland/apps/compositor/main.c:40-42`), which are currently just
hardcoded and never actually driven by the real session. This is the same
"two parallel implementations, only one is real, the golden ships whichever
one nobody is looking at" failure mode this codebase has hit before (two
Task Managers, two `g_wallpapers[]` arrays - see project memory). The design
requires collapsing this to one authoritative implementation:

- **Keep the kernel-side gate** (`kernel/gui/login.c`, already wired into
  `kernel/main.c`) as the thing that decides *whether* `desktop_run()` is
  reached at all, and as the source of the real `uid`/`gid` a spawned
  compositor process runs as.
- **Retire the compositor's dead second login system** as an
  authentication path. Its UI (avatar row, mouse support, themed panel -
  `userland/apps/compositor/login.c` is visually the nicer of the two) is
  worth keeping as the actual on-screen renderer, but it must not decide
  anything; it must call through to the same kernel authentication primitive
  (`sys_authenticate`) and treat its own `g_logged_in`/`g_login_uid` as a
  **display cache of a kernel-verified fact**, refreshed every time the
  authoritative state could have changed (login, lock, unlock, switch) -
  never as the fact itself. See Section 2.4 for why this distinction is load
  bearing, not stylistic.

---

## 2. Credential storage: secure by design

### 2.1 What exists today

`kernel/proc/users.c:60-89`, `compute_password_hash()`:

```c
// Compute SHA-256 hash of (password + username) and convert to hex
static void compute_password_hash(const char *password, const char *username,
                                   char *hex_out) {
    // Concatenate password + username for minimal salting
    ...
    sha256(combined, total, digest);
```

Problems, named precisely:

1. **The "salt" is the username.** A salt must be secret-until-used and
   ideally random; the username is neither. Two installs with an account
   named `admin` produce the *same* hash for the same password. This is a
   fixed, guessable pepper, not a salt - it defeats rainbow tables only in
   the weakest sense (per-username tables, not per-install).
2. **One SHA-256 round.** SHA-256 is designed to be fast, which is exactly
   the wrong property for password hashing: a modern GPU computes billions
   of SHA-256 hashes per second, so any password not already
   high-entropy is crackable offline in a practical amount of time the
   instant `/CONFIG/SHADOW` is read.
3. **No rate limiting at the syscall layer.** `sys_authenticate()`
   (`kernel/proc/syscall.c:278-283`) is a plain syscall available to *any*
   Ring-3 process, with zero attempt counting. The only throttling that
   exists is a **UI-layer** 1200ms sleep-and-retry in the compositor's login
   loop (`userland/apps/compositor/login.c:267-276`), which only slows down
   someone clicking through the actual login screen - it does nothing for a
   program that calls `sys_authenticate()` directly in a tight loop.
4. **Non-constant-time compare.** `user_verify_password()`
   (`kernel/proc/users.c:497-524`) uses plain `strcmp()` on the computed vs.
   stored hash - a byte-at-a-time early-exit compare is a textbook timing
   oracle. The codebase already has the fix, just not applied here: the RC
   remote shell's own auth uses a real constant-time compare,
   `rc_streq()` (`kernel/net/remote_ctrl.c`, "Constant-time string comparison
   (prevents timing side-channels)"). This must be promoted to a shared
   primitive and used everywhere a secret is compared, including password
   hash comparison.

### 2.2 What's actually good and should be kept

- `/CONFIG/SHADOW` is written with mode 0600 owned by uid/gid 0
  (`kernel/proc/users.c:323-326`, `perms_set("/CONFIG/SHADOW", 0, 0, 0600)`),
  and this is **actually enforced**, not decorative: `perms_check()` is
  called from the real `sys_open()` path (`kernel/proc/syscall.c:1891`), so
  a non-root Ring-3 process's attempt to `open("/CONFIG/SHADOW")` is denied
  at the kernel layer before any bytes are returned. Verified by reading the
  call site, not assumed.
- `sys_passwd_change()` (`kernel/proc/syscall.c:4741-4757`) already gets
  authorization right: non-root callers must supply their *own* current
  password and can only target their own account; root can reset anyone's.
  This is the correct model and the login/lock design should route all
  password changes through this exact syscall, unchanged.
- The kernel already has real cryptographic primitives to build a proper KDF
  from, so nothing new needs inventing from scratch:
  `kernel/crypto/csprng.c`/`rng.c` (RDRAND-backed CSPRNG with entropy-pool
  fallback), `kernel/crypto/hmac.c`, `kernel/crypto/sha256.c`.

### 2.3 Proposed fix

- **Real per-account random salt.** At account creation (and at any future
  password change), generate 16 bytes from the existing CSPRNG
  (`kernel/crypto/csprng.h`) and store it alongside the hash in `/CONFIG/SHADOW`
  (new field; format becomes `username:salt_hex:hash_hex:iterations`, still a
  simple flat file, no new file format invented).
- **A real KDF, built from existing primitives.** Implement PBKDF2-HMAC-SHA256
  (composable directly from the existing `hmac.c` + `sha256.c`; no new
  primitive, just an iteration loop) with a substantial iteration count.
  Password hashing runs once per login attempt, not in a hot path, so even
  ~100-300ms of deliberate CPU cost is imperceptible to a user and a real
  cost to an offline attacker. **Open item, needs real-hardware
  measurement**: the target is soft-float/no-SSE
  (`x86_64-unknown-none`, `-mno-sse -mno-sse2` per project convention),
  so the exact iteration count that lands around ~150ms must be benchmarked
  on the actual iMac14,4 target, not assumed from a desktop-class number.
  Per the project's Rust mandate, this is new kernel code and should be
  written in Rust (`no_std`, `core`+`alloc`), FFI'd to the existing C
  `sha256()`/HMAC the same strangler pattern already used elsewhere
  (`RUST_PORT_LEDGER.md`).
- **Rate limiting inside `sys_authenticate()` itself**, not just the UI: a
  small per-account failed-attempt counter (in-RAM, persisted alongside
  `/CONFIG/SHADOW` on `users_sync()`) with escalating lockout (for example:
  5 failures -> 30s lockout, 10 -> 5 minutes, reset on a successful auth).
  Because this lives in the syscall, it protects *every* caller uniformly:
  the compositor's login screen, the lock screen, and (once fixed per
  Section 3.1) the RC remote shell, which today has its own separate
  3-attempt-then-disconnect limit (`REMOTE_CTRL_MAX_ATTEMPTS = 3`,
  `kernel/net/remote_ctrl.h:17`) that would otherwise be a second,
  inconsistent policy.
- **Constant-time compare everywhere a secret is checked.** Promote
  `rc_streq()` out of `remote_ctrl.c` into a shared string/crypto utility and
  call it from `user_verify_password()` too.
- **No default credentials in the golden.** First boot forces password
  creation for the initial administrator account (Section 1.1); no
  `create_defaults()` password is ever a real login credential in a shipped
  image.

---

## 3. Threat model: bypass classes designed out

Each subsection: the attack, grounded in real code where it exists today,
and the specific design decision that closes it.

### 3.1 The Windows utilman.exe / sticky-keys equivalent

**The attack (classic Windows)**: from the lock/login screen, trigger an
"accessibility" helper that Windows will run with SYSTEM privilege before
login, and swap it for `cmd.exe`.

**MayteraOS's actual equivalent, and it is worse**: `kernel/net/remote_ctrl.c`
listens on TCP port 2323 the instant the network stack is up
(`remote_ctrl_init()` called at `kernel/main.c:1413`, which is *before*
`login_run()` at `kernel/main.c:1582` - it starts regardless of whether
anyone has ever logged in). It authenticates with credentials **hardcoded
in a header** and unrelated to any real OS account:

```c
// kernel/net/remote_ctrl.h:15-17
#define REMOTE_CTRL_USER    "admin"
#define REMOTE_CTRL_PASS    "maytera"
```

Once "authenticated" (with a credential every install shares), the session
gets a ring-0 command shell: `cat` (arbitrary file read, including anything
`perms_check` would otherwise deny to a real Ring-3 process, since this
path is kernel code, not a syscall caller), `shell`/`run` (execute
commands), `launch`/`launchap`/`click`/`scroll` (drive the GUI directly -
this can literally click through a lock screen), `reboot`/`shutdown`,
`install`/`uninstall`. No local disk access, no binary swap needed - just
network reachability. This needs no accessibility-app substitution at all;
it's a standing, documented backdoor.

**Design decision:**
- **Delete the hardcoded credential.** `rc_authenticate()` already has the
  right shape (prompts for username/password, has a constant-time compare,
  has an attempt limit) - point it at the real `user_verify_password()`
  against `/CONFIG/PASSWD`/`/CONFIG/SHADOW` instead of the two `#define`s, so
  RC access requires a real, per-install, per-user OS credential, subject to
  the same lockout policy as every other auth path (Section 2.3).
- **RC must not be able to touch a locked session.** Even with real
  credentials, an RC session authenticated as user A must not be able to
  send `click`/`scroll`/`launchap` input to a *locked* desktop (whether
  locked as A or as a different user B) without separately re-entering that
  session's unlock credential. A locked screen is a stronger boundary than
  "some valid account is connected over the network"; RC's GUI-injection
  path must check compositor lock state (Section 3.2) before relaying, the
  same way it already gates on authentication before dispatching any
  command at all.
- **This is a kernel-and-userland change**: the credential fix and lockout
  are kernel (`remote_ctrl.c`); the "refuse to inject input while locked"
  check needs the compositor to expose its lock state to the kernel (a new,
  small, kernel-owned flag - see 3.4 for why it must be kernel-owned, not a
  value RC reads out of the compositor's own memory).

### 3.2 Lock screen that leaks input

**The attack**: a lock overlay is drawn, but keystrokes or clicks still
reach the desktop/apps underneath - the classic "the lock is just a picture"
bug.

**Grounded in the real input path**: the compositor already gets exclusive
input from the kernel - `grab_input(1)` at compositor init
(`userland/apps/compositor/main.c:177`) calls `sys_grab_input()`
(`kernel/gui/fb_syscall.c:391`), which calls `wm_enter_exclusive_mode()` so
the kernel's own desktop input loop stops running; the compositor becomes
the sole recipient of raw input from the kernel. Good foundation. But
*within* the compositor process, `process_input()` unconditionally forwards
every key to whatever app window has focus:

```c
// userland/apps/compositor/main.c, process_input()
sys_inject_key(key);   // forwarded to app windows, no lock-state check today
```

There is no lock state to check today because there is no lock. The design
adds one, and the compositor already has the right *pattern* to copy: it
has existing true modal-input-capture surfaces. The command launcher /
Spotlight is documented as swallowing "every other key while open (modal)"
(`userland/apps/compositor/launcher.c:132`), and the HA/widget settings
dialog is documented as "Modal: clicks outside the panel do nothing (close
only via X / Save & Close / ESC)" (`userland/apps/compositor/widgets.c:1879`,
with the wheel-scroll gate at `main.c:637-639` explicitly there "so [the
scroll] does not reach the window manager underneath the modal"). A recent
real incident (see project memory: "dead-leftclick-was-modal-grab") also
confirms this modal-grab mechanism actually works in practice, not just in
comments - it was strong enough to make the entire desktop appear
unresponsive to clicks when a debug line opened it unconditionally.

**Design decision**: the lock overlay is a modal exactly like these, at
compositor scope, with one added rule that is the entire point: while
locked, `process_input()` must **not** call `sys_inject_key()` /
`sys_inject_mouse()` toward any app window, must not run desktop icon /
taskbar / start-menu hit-testing, and must route 100% of key and mouse
events to the lock overlay's own handler (unlock password field, the
power-button row, nothing else). This is a pure userland/compositor change
- the kernel primitive it needs (`grab_input`, exclusive routing) already
exists and does not need to change. What does need adding at the kernel
level is a small, kernel-owned "this session is locked" flag (Section 3.4)
so other subsystems (RC, IPC to app windows) can be gated on it without
trusting the compositor's own process memory.

### 3.3 Console/debug escapes

Three distinct escape hatches, each different:

**(a) `kernel_shell()` - unauthenticated ring-0 shell, reachable by crashing
the desktop.** `kernel/main.c:1668-1680`:

```c
// If GUI exits, fall back to shell
kprintf("[GUI] Desktop exited, falling back to shell\n");
...
kernel_shell();
```

`kernel_shell()` (`kernel/main.c:1813` onward) requires **no credential at
all** and offers memory dump, disk read, network control, process listing,
`shutdown`/`reboot`, and a `gui` command to relaunch the desktop. Today,
since there is no working login gate, this is moot (everything is already
open); but the moment login/lock exist, this becomes the single most
dangerous bypass: crash or kill the authenticated compositor process (a
memory-safety bug anywhere in `/APPS/COMPOSIT`, or literally sending it a
signal if one exists) and land in a full ring-0 shell with zero
authentication, worse than an unlocked desktop because it has no privilege
boundary at all.

*Design decision*: once a session has successfully authenticated, the
kernel must never silently fall through to `kernel_shell()` again for that
boot. If `desktop_run()` returns unexpectedly post-login, the kernel must
return to `login_init()`/`login_run()` (the mandatory gate), not to the
open shell. `kernel_shell()` remains reachable only pre-login (no session
has yet authenticated this boot) - and even that pre-login reachability
should be reconsidered; arguably `kernel_shell()` should also require
`sys_authenticate()` once any account exists, falling back to fully open
only in the narrow pre-account first-boot-setup window. **This is a kernel
change** (`kernel/main.c`'s boot sequence and its "fall back to shell"
branch).

**(b) Serial console.** `kernel_shell()` is also what a serial connection
reaches (per the project's own documented workflow,
`socat - UNIX-CONNECT:/var/run/qemu-server/<id>.serial0`, landing at the
`maytera>` prompt). This is a physical/hypervisor-console channel, the same
trust class as BIOS/GRUB access on any other OS: whoever has the serial
cable (or the hypervisor console) already has an out-of-band path to the
machine. The design does **not** try to eliminate this - it is an accepted,
documented trust boundary, not a bypass to "fix" - but it must not be
promoted to something network-reachable (see 3.3c), and the design doc
should say plainly: physical/hypervisor console access is root-equivalent
by design, same as it would be on Linux/Windows/macOS.

**(c) The RC `click`/`scroll`/`launchap` GUI-injection commands are the
network-reachable version of (b), and unlike the compositor's own
`testhook.c` they are not compile-gated.** `testhook.c` gets this right and
should be the model: its own header comment states plainly "SECURITY: this
is an attack surface... it must not exist in a shipping build. Enforcement
is COMPILE-TIME... never how `build/build-golden.sh` or a developer's plain
`make`/`make install` builds `COMPOSIT`." RC's GUI-injection surface has no
equivalent gate; it ships in every kernel build by default and is reachable
the instant the network is up, regardless of `TESTHOOK=1` or not. *Design
decision*: RC's GUI-injection commands (`click`, `scroll`, `launchap`) get
the same treatment as 3.1 (require real per-user OS auth, refuse while
locked) as the primary fix; the invariant gate
(`build/invariant-gate.sh`) should also gain a check that a golden's
`COMPOSIT` has no `MAYTERA_TESTHOOK` symbols, closing the loophole where a
stray test build reaches production undetected (the exact class of bug this
project's own `blame.md` already documents for the `COMPOSIT`/`COMPOSITOR`
naming mismatch).

### 3.4 Ring-3 forging the "unlocked" state

**The attack**: an app (or an exploited bug in the compositor itself) writes
directly to whatever variable means "session is unlocked," skipping
authentication entirely.

**Where this stands today**: `g_logged_in`/`g_login_uid`/`g_login_username`
(`userland/apps/compositor/compositor.h:491-493`) are plain process-local
static globals inside `/APPS/COMPOSIT`'s own address space. No *other*
Ring-3 process has a syscall or shared-memory path to write them directly
today - but that "safety" is incidental, not designed: it holds only as
long as nothing exposes a write primitive into the compositor's own data
segment, and the project's own security audit (`docs/SECURITY_AUDIT.md`,
Finding #003, "No User/Kernel Address Space Separation") plus more recent
internal findings (project memory: "VMM TRUTH... kernel_pml4 is zeroed and
never in CR3... range checks are structurally wrong") indicate the
Ring-3/Ring-0 isolation this whole model depends on is not as solid as it
should be. **This document does not assume that guarantee holds**; it is
called out explicitly as a prerequisite that needs independent
verification/hardening by the kernel/security specialists, not something a
UI design can paper over.

Given that, the design deliberately does **not** put the security decision
in a plain global bool at all:

- **The kernel remains the sole authority for "is this uid's session
  unlocked."** Concretely: extend the same mechanism that already correctly
  tracks session identity at the kernel layer
  (`desktop_set_session()`/`desktop_get_session_uid()`,
  `kernel/gui/desktop.c:86-93`) with a companion "locked" bit, set only by
  a syscall that itself calls `user_verify_password()` / the new rate-limited
  `sys_authenticate()` path - never settable by a bare write. The
  compositor's own `g_logged_in`-style globals become a **display cache**,
  re-read from the kernel every frame (or on every state-transition event),
  never the fact itself. A forged compositor-local flag, even if some future
  bug made that possible, would not change what the kernel enforces for
  `perms_check()`, `sys_delete_user`, `sys_passwd_change`, or RC's
  input-injection gate (3.1).
- **Audit trail on every transition.** `login.c` already logs authenticate
  success/failure via `sys_bootlog()` (`userland/apps/compositor/login.c:98-111`);
  extend the same logging to lock/unlock transitions, so a forged unlock (if
  the isolation assumption above ever failed) leaves a visible trace instead
  of a silent one.
- This item is explicitly **kernel work**, not something the compositor can
  self-certify.

### 3.5 Boot-time bypass via ESP config (autologin)

**The attack**: skip login entirely by editing a file on the boot partition.

**This one already exists, fully working, today**: `/CONFIG/LOGIN.CFG`'s
`autologin=<username>` line logs that user in with **an empty password**
and no check at all:

```c
// userland/apps/compositor/login.c:220-222
if (extract_after(buf, "autologin=", autologin_user, 64)) {
    int uid = sys_authenticate(autologin_user, "");
```

The kernel-side implementation (`kernel/gui/login.c:578-655`,
`login_check_autologin()`) is even more direct - it does not call any
password-verification function at all, it just looks the named user up and
logs them in if found. Since the ESP is a FAT32 partition that exists
precisely to be written by boot tooling (and is not protected by the same
`perms_check()` model that guards the ext2 root - FAT has no POSIX
permission bits), **anyone with write access to the boot partition can name
`root` in this file and boot straight to an authenticated root desktop with
zero credential check.**

**Design decision**: autologin, if kept at all, must become an explicit,
off-by-default, *admin-set-from-within-an-already-authenticated-Settings-session*
option - never a bare config file an attacker can plant by writing to the
ESP. It must never be usable to autologin as an administrator/root-
equivalent account, and the login screen must visibly indicate "autologin
enabled for USER" (a security-relevant fact should never be silent). Given
the severity of the current implementation, the recommendation is to
**remove file-driven `/CONFIG/LOGIN.CFG` autologin entirely** from a
"secure by design" build and replace it, if the kiosk use case is still
wanted, with a Settings-managed flag stored in the same protected location
as other session policy (see 3.4) - this is called out as an **open
question for the user** in Section 5, since it is a real feature removal,
not a bug fix.

### 3.6 Screenshot / DoS of the lock screen, and reboot-to-unlocked

- **Screenshot/liveness**: the lock overlay must keep rendering, including a
  visibly ticking clock, under all conditions - project experience
  specifically warns that "a single screendump of a wedged UI looks alive"
  (project memory: "verify liveness: 2 screendumps + clock") and that GUI
  automation should be corroborated (see docs/GUI_TEST_INPUT.md: #334 is the
  WORKING deterministic testinput channel, COM1, ACKd) so that verification of a
  shipped lock screen should capture two screenshots with a visible clock
  between them, not one. This is a verification-methodology note as much as
  a design one - flagged in Section 6.
- **Notification leakage over the lock**: today, notification toasts have no
  concept of a locked state (`notif_init()`/`notify_post()`,
  `userland/apps/compositor/main.c:1179-1181`, fire unconditionally). A
  background service posting a toast with sensitive content while the screen
  is locked would render it on top of the lock overlay exactly like the
  modal patterns in 3.2 draw on top of everything else. Design decision:
  either suppress toast rendering entirely while locked, or render only a
  redacted "N notifications" count, consistent with how most modern lock
  screens handle this.
- **Reboot-to-unlocked**: this is the one place the current code already
  gets the right property, *if* the two-login-implementations problem
  (Section 1.2) is fixed: `kernel/main.c` unconditionally calls
  `login_init()`/`login_run()` before `desktop_run()` on every boot
  (`kernel/main.c:1580-1585`), so a hard reboot lands back at authentication,
  not a resumed unlocked desktop. If the compositor's dead second
  `login_run()` call (`main.c:1167-1169`) were ever re-enabled by accident
  without retiring it as a decision-maker, the result would be a confusing
  double-login prompt, not a bypass - a UX bug, not a security one, but
  another argument for collapsing to one implementation (1.2) rather than
  leaving two.

---

## 4. UX spec

### 4.1 Login flow

1. Boot reaches the kernel login gate (`kernel/gui/login.c`'s rendering
   logic, kept, restyled per the mockup).
2. If zero accounts exist (genuine first boot), show account-creation
   instead of sign-in: username, display name, password + confirm, avatar
   pick. This account becomes the administrator. No default credentials are
   ever live in a shipped golden (Section 1.1).
3. Otherwise: avatar row (existing pattern in both login implementations,
   kept and restyled), click or press 1-9 to pick a user (existing behavior,
   `login_handle_key()`/`login_handle_mouse()` in
   `userland/apps/compositor/login.c:404-525`), then a password field.
   `Enter` or the Sign In button submits; `Esc` or "Back" returns to the
   avatar row (existing behavior, kept).
4. On failure: existing error-message-then-clear behavior is kept
   (`LOGIN_STATE_ERROR`, `userland/apps/compositor/login.c:267-277`), plus
   the new rate-limit/lockout messaging from Section 2.3 ("Too many
   attempts. Try again in Ns.").
5. On success: apply the authenticated user's own wallpaper/theme (Section
   1.1) before the desktop paints, play the existing startup sound
   (`sys_play_wav("/SOUNDS/STARTUP.WAV")`, already wired), then
   `desktop_run()` spawns `/APPS/COMPOSIT` with the real kernel-tracked
   `uid`/`euid` (existing mechanism, kept, `desktop.c:142-144`).

### 4.2 Lock flow

Three triggers, one overlay:

- **Explicit**: a new "Lock" item in the Start Menu's power section,
  alongside the existing Restart/Shutdown buttons
  (`userland/apps/compositor/startmenu.c`, power section around lines
  558-590). Neither "Lock" nor "Log Out" exist in the menu today (verified:
  no match for either string in `startmenu.c`/`desktop.c`) - this is a real
  gap, not a rename.
- **Idle timeout**: reuse the exact idle-tracking primitive the screensaver
  already uses (`g_idle_ms`/`uptime_ms()`,
  `userland/apps/compositor/main.c:45,184,403,1176,1292`,
  `screensaver_on_input()`), with its own configurable "lock after N minutes"
  in Settings, independent of (but able to compose with) the screensaver
  timeout.
- **Hotkey**: Super+L, reusing the existing `KEY_SUPER` handling already
  present for the Start menu (`main.c`'s `s_last_key` tracking of `0x9B`).

While locked: full input grab per Section 3.2 (no key/click reaches any app
window or desktop chrome); power controls (Restart/Shutdown, mirroring the
existing Start Menu power buttons) remain available without authenticating,
matching every mainstream OS's lock screen; a "switch user" affordance
returns to the avatar row without ending the other user's session state (v1:
this still means fully suspending/logging out the current session per
Section 1.1's stated v1 scope - "switch user" here means "go to login
screen," not "keep both alive").

### 4.3 Visual language

See `docs/mockups/login-mockup.html` for the concrete rendering. Direction:
full-bleed blurred desktop wallpaper backdrop (a real blur of the actual
wallpaper, not the current flat two-color gradient in
`CLR_LOGIN_BG_TOP`/`CLR_LOGIN_BG_BOT`), a glassy centered panel, a large
live clock and date above the panel (doubles as the liveness indicator
called out in Section 3.6), the user's avatar and name, a masked password
field with the existing bullet-dot pattern
(`draw_password_field()`/`draw_bullet()`,
`userland/apps/compositor/login.c:118-147`, kept), and power controls in a
corner. Typography and spacing follow `docs/UI_STYLE_GUIDE.md`'s Modern
Dark palette/type scale as the primary direction (this screen is the user's
first and last impression of the OS per session, so it leans into the
"very very pretty" ask rather than the information-dense retro-UNIX
density), while remaining themeable: the existing `CLR_LOGIN_*` token set
(`userland/apps/compositor/compositor.h:74-86`) is kept as the integration
point so retro-unix/modern-light/fluent installs get a coherent, not
mismatched, lock/login screen. Light and dark contrast are both verified in
the mockup.

**v2 idea, explicitly out of scope for this design**: fast user switching
(multiple live sessions, instant toggle) - flagged in Section 1.1, would need
a new kernel-level multi-session concept.

---

## 5. Open questions for the user

1. **Autologin removal.** Section 3.5 recommends removing file-driven
   `/CONFIG/LOGIN.CFG` autologin entirely rather than hardening it, since its
   current form has no safe middle ground (empty-password auth is not
   fixable in place). Confirm this is acceptable, or specify the kiosk use
   case it needs to keep serving so a Settings-gated replacement can be
   scoped.
2. **RC (port 2323) fate.** Section 3.1 proposes real per-user auth plus a
   lock-state gate. An alternative, more conservative option: disable RC
   entirely in golden/production builds (compile-gated like `testhook.c`)
   and keep it only for development images. Which posture is wanted for the
   public golden?
3. **`kernel_shell()` pre-login reachability.** Section 3.3a proposes it stay
   reachable only pre-authentication this boot. Confirm whether even that
   is acceptable, or whether it should require credentials the moment any
   account exists at all (i.e., even on a fresh unconfigured machine, once
   accounts.json/PASSWD is non-empty).
4. **PBKDF2 iteration count** needs real-iMac14,4 benchmarking before being
   fixed (Section 2.3) - this is implementation work, not a decision to make
   now, but the target latency (~100-300ms) should be confirmed as
   acceptable.
5. **Ring-3/Ring-0 isolation** (Section 3.4) is a prerequisite this design
   leans on but does not itself fix; recommend the kernel/security
   specialists independently confirm current isolation strength (given
   `docs/SECURITY_AUDIT.md` Finding #003 and related project findings)
   before the "Ring-3 cannot forge unlock state" guarantee is treated as
   airtight rather than "best effort, kernel-enforced where the isolation
   holds."

---

## 6. Verification plan (once built)

Mouse-driven compositor panels ARE drivable headlessly via the #334 testinput
channel (COM1, deterministic, ACKd; see docs/GUI_TEST_INPUT.md). This passage
previously claimed the opposite, which is FALSE and has misled several passes.
For belt-and-braces liveness, verification of the actual
shipped login/lock screen should be:

- Two screenshots with a visibly different clock reading between them (not
  one - a single screendump of a wedged UI looks alive; see project memory),
  taken via a throwaway auto-open build or a real-mouse pass, not
  `testhook.c` (which is explicitly compile-gated out of production and
  should stay that way per 3.3c).
- An explicit negative test: attempt to reach an app window's input while
  the lock overlay is up (real keyboard/mouse, not `sys_inject_key` called
  directly, which would trivially "pass" without proving the compositor's
  own gating logic works).
- An explicit RC test, post-fix: connect to port 2323 while a session is
  locked, authenticate with valid OS credentials for the locked user's own
  account, and confirm `click`/`scroll`/`launchap` are refused rather than
  silently accepted.
- Confirm `kernel_shell()` is not reached by killing/crashing the
  compositor process post-login (Section 3.3a's core claim).

None of this has been run - it is a plan for whoever implements the design,
not a report of results.
