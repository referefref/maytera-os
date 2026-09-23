# Contracts, capabilities, and manifests: which doc is which

The contracts/capabilities/manifests area accumulated many documents at
different stages of reality. This index says, for each, whether it describes
something CANONICAL (implemented and current), DESIGN (agreed but unbuilt),
ASPIRATIONAL (a concept, largely unbuilt), or HISTORICAL (a record of something
removed or superseded). Every status below was verified against code on `dev`,
because in this area some documents' own banners disagree with the code.

## Read this first: there are three substrates, one of them dead

1. **Kernel system capability API** (`SYS_CAP_*`, syscall numbers 429 to 440).
   REAL and kernel-enforced for three capability classes. This is what actually
   gates screen capture, the serial gateway, and input injection today.
2. **Kernel-enforced escrow** (`escrow_fs_guard`, `SYS_ESCROW_*`). REAL kernel
   code, wired into the filesystem mutation syscalls, but entered ONLY by test
   apps. No production or AI flow enters the kernel escrow.
3. **Per-app `manifest.json`.** DEAD. The 27 files were fabricated and were
   deleted in #235; nothing ever read them. There is no static per-app manifest
   read by the kernel or compositor.

## What the REAL current mechanism is

There is no static per-app YAML/JSON capability manifest. The real mechanisms are
all runtime and three-layered:

1. **Static contract description.** A `ct_contract_t` compiled into an app's own
   source, answered over a `--contract` CLI flag (`userland/libc/contract.c`).
   Only `calc` and `settings` implement one. The app IS its own contract; there
   is no separate file.
2. **Runtime capability consent.** `SYS_CAP_REQUEST` opens a request, the
   compositor draws the consent prompt via `SYS_CAP_VIEW` / `SYS_CAP_RESOLVE`,
   and a time-boxed, scoped grant is minted in the kernel (`rustkern/caps.rs`).
   Enforced at the `syscall_cap_check` chokepoint (`kernel/proc/syscall.c:1889`,
   backed by `cap_required_for_syscall()` in `rustkern/caps.rs`) for
   `screen.capture`, `serial.port`, and `input.inject`. See
   `docs/INPUT_SCREENSHOT_API.md`.
3. **AI tool layer.** `userland/libc/aicap.c` holds tokens plus a consent and
   audit trail (`/CONFIG/AICAPS.CFG`, `/CONFIG/AIAUDIT.LOG`), extended to a
   per-device tier by `userland/libc/aidev.c` reading `/CONFIG/AIDEVCAP.CFG`.
   This layer is USERLAND-advisory: a uid-0 Ring-3 process can bypass it.

## Per-document status

| Document | Status | Verified reality |
|----------|--------|------------------|
| `SYSTEM_CAPABILITY_API.md` | **CANONICAL for Stages 0 to 3** (banner is STALE, see flag below) | Syscalls 429 to 440 implemented and dispatched (`kernel/proc/syscall.c:2637-2680`); enforced at `syscall_cap_check`. Stage 1 `screen.capture` (`SYS_SCREENSHOT_REQUEST` 435), Stage 2 `serial.port` (`SYS_SERIAL_OPEN` 438), Stage 3 `input.inject` (`SYS_CAP_INJECT_KEY` 439 / `MOUSE` 440) are kernel-enforced. Audio, socket, and video-out classes are not yet built. |
| `CONTRACT_API.md` | **CANONICAL-IMPLEMENTED** (banner honest) | `ct_contract_t` compiled into an app, answered via `--contract` (`userland/libc/contract.c`). The "two apps wired" are `calc` and `settings`. This is a static description surface, not broad enforcement. |
| `CONTRACT_ENFORCEMENT_PLAN.md` | **MECHANISM IMPLEMENTED, NOT IN PRODUCTION USE** (banner OVERSTATES reach, see flag below) | `escrow_fs_guard()` is wired into the real FS syscalls (`kernel/proc/syscall.c:10710` mkdir, `:10748`/`:10781` unlink/rmdir, `:10834` rename) and a non-FS effect guard at `:12554` (device eject). Device scope (`kernel/fs/escrow_device.c`), rollback/abort, and the no-delete invariant are all present. But the only callers of `escrow_enter()` are test apps (`ESCU6`, `ESCROWT`, `ESCROW2T`); nothing a real user does enters the kernel escrow. |
| `CONTRACT_ARCHITECTURE.md` | **DESIGN-ONLY** (banner honest) | Self-describes: "ARCHITECTURE, agreed 2026-08-05. Nothing below is implemented yet." The aspirational escrow/isolation vision (#680). |
| `AI_ACTION_CAPABILITY_BINDING.md` | **PARTIAL** (banner honest) | The Tier-2 live-app action wire is real for paint (`userland/libc/ctlive.c`, `userland/apps/paint/contract.c`, the `app.action` AI tool). The broad "drive any app" claim is still design. |
| `AI_DEVICE_CAPABILITY_MANIFEST.md` | **PARTIAL / userland-advisory** (banner honest: DEFERRED) | The per-device manifest is real userland code (`userland/libc/aidev.c`, override file `/CONFIG/AIDEVCAP.CFG`), a two-gate model (manifest verdict then `aicap_authorize`). Enforcement is userland and uid-0-bypassable. Only the EJECT verb has a real executor (`SYS_VOL_EJECT`, #708); format/partition have none. |
| `AI_ESCROW_PROMISE.md` | **IMPLEMENTED as userland-advisory** (banner honest, #712) | The production AI photo-organize path uses THIS advisory escrow: `aiclient.c` mints scoped fs.write/move/mkdir grants (no delete) via `aicap.c`. It correctly disclaims the kernel-enforced version (#246/#305) as the follow-on. |
| `ESCROW_ROLLBACK_SEMANTICS.md` | **MECHANISM IMPLEMENTED, test-only exercise** | Rollback logic real (`kernel/fs/escrow_undo.c`, `kernel/rustkern/escrowundo.rs`, `kernel/fs/escrow_guard.c:555-600`). Same caveat as the enforcement plan: real code, only test apps enter escrow. |
| `GRAPHFS_DESIGN.md` | **PARTIAL** (banner honest, self-flagging: DEFERRED) | Journal/fold/blob slices shipped and VM-verified (`kernel/rustkern/gfsjournal.rs`, `gfsfold.rs`, `kernel/fs/graphfs/{journal,fold,blob}.c`); `gfs_grant_check()` is used by the escrow guard. Versions, revert, signed-seal, and the syscall chokepoint are unbuilt (headers only). |
| `LLM_CONTRACTS.md` | **ASPIRATIONAL** (banner honest) | "aspirational / original concept, largely unbuilt." The per-app `manifest.json` files it implies were removed as fabricated (#235). |
| `APP_CAPABILITY_WISHLIST.md` | **HISTORICAL** (banner honest) | Explicitly a recovered wishlist, not a description of what exists. Reiterates the #235 deletion and the 80%-fictional-token audit. |

## The manifest reconciliation (#235)

The per-app capability `manifest.json` story is closed. Twenty-seven orphaned
`userland/apps/*/manifest.json` files were fabricated: an audit found 150 stated
capability tokens, of which about 120 (80%) were fictional. They were all
DELETED in #235, and `contract-lint` fails the build if any reappear. Every
surviving `manifest.json` reference in the tree is the App Store repo catalog,
which is unrelated. The lesson recorded in CHANGELOG for #235: every description
of an app's capabilities is now either true or absent, never invented.

## Documents whose own banner contradicts the code (flags for the owner)

1. **`SYSTEM_CAPABILITY_API.md` banner is STALE and FALSE.** Its header reads
   "DESIGN. Nothing below is implemented." (measured at `bbebc5a0`, 2026-09-04),
   but Stages 0 to 3 landed and are kernel-enforced (`screen.capture`,
   `serial.port`, `input.inject`). This is the most misleading banner in the set.
   The document's technical content is correct and current; only its status line
   is wrong. Treat it as CANONICAL for Stages 0 to 3.
2. **`CONTRACT_ENFORCEMENT_PLAN.md` banner OVERSTATES reach.** Its header reads
   "IMPLEMENTED (all six stages)." The mechanism is genuinely implemented and its
   self-tests are green, but no production path enters it: only test apps call
   `escrow_enter()`. Read "implemented" as "the mechanism exists and passes its
   own tests," not "the AI runs inside it."
