# MayteraOS Self-Reprogrammable OS: Design Spec

North-star (user, 2026-06-28): ANY part of the OS EXCEPT the security guardrails can be
fundamentally redesigned/reprogrammed SAFELY by the end user (via the AI), to add features,
run software from other OSes/architectures, add device drivers, build new applications, and
redesign the UI. This spec turns that into an implementable architecture for tasks #293
(capability-token permissions), #294 (userland/whole-stack compiler), #304 (snapshot +
auto-rollback safety net), and #305 (protected immutable security core). It builds on
docs/LLM_CONTRACTS.md (the original concept) and aitools/PROTOCOL.md (the per-app tool
contracts + ReAct loop already shipped in the AI chat).

## 0. The loop (how a user reprograms the OS)

  user describes a change in chat
    -> AI proposes a concrete diff / new component (shown to the user)
      -> CONSENT gate (#293): high-risk action requires explicit user approval
        -> SNAPSHOT the affected component(s) (#304)
          -> COMPILE (#294)  [build server now; on-device toolchain later]
            -> DEPLOY + VERIFY (build ok + functional/liveness check)
              -> KEEP if good  |  AUTO-ROLLBACK if build/verify/runtime fails (#304)
  Every step is gated by a capability TOKEN and written to a tamper-evident AUDIT log.
  The guardrail core (#293 + #304 mechanism) is OFF-LIMITS to this loop (#305).

## 1. #293 Capability-token permission system

Model (from LLM_CONTRACTS.md, kernel stub kernel/rust/src/capability.rs):
- A capability TOKEN is a signed, TIME-BOUNDED grant:
  { token_id, issued_at, expires_at, capabilities[], constraints{ max_uses, allowed_paths,
    denied_commands }, audit_tag, signature }.
- Capability namespace: system.* / app.* / fs.* / media.* / build.* (the compiler verbs).
  Each capability declares a RISK level (low | medium | high).
- Enforcement point: the AI tool runtime (libc/aiclient.c + a kernel capability service).
  Before dispatching ANY tool/executor, check the active token grants every capability the
  tool declares; else return CAPABILITY_DENIED (never silently skip). Also enforce
  TOKEN_EXPIRED / TOKEN_EXHAUSTED / allowed_paths / denied_commands.
- CONSENT: any high-risk capability (fs.write/delete, terminal.execute, python.execute,
  system.network.write, system.settings.write, and all build.*) requires an explicit user
  approval prompt before first use under a token. (Interim form already in aiclient: a
  two-step confirm; replace with token issuance + a consent dialog.)
- AUDIT: every capability use appends { ts, token_id, app, tool, params_hash, result,
  audit_tag } to a tamper-evident system log. This also delivers filesystem auditing for
  free (every fs.* call is logged with its token).
Implementation order: (a) token struct + issue/check/expire/reap in the kernel (flesh out
capability.rs) + a SYS_CAP_* surface; (b) wire the aiclient dispatch to require a token;
(c) consent dialog in the compositor; (d) the audit log (append-only file + in-RAM ring).

## 2. #294 Userland (then whole-stack) compiler

Verbs (aitools/PROTOCOL.md build.*): build.compile_app(id, patch|spec), build.deploy_app(id).
Both high-risk + require_consent. Staged by blast radius:
  1. userland apps (safest, first): patch app source -> compile -> deploy /APPS -> verify.
  2. compositor / widgets / themes.
  3. device drivers (load recoverably; a faulting driver unloads, never panics).
  4. kernel (needs A/B kernel slots + boot fallback, see #304).
Compile location: build server initially (the existing remote gcc-12 path); the user wants
a real ON-DEVICE toolchain eventually (a freestanding C compiler in userland) so the OS can
rebuild itself without an external host. Promise: resolves to "component rebuilt + running"
or a typed build-failure with the compiler errors fed back so the AI iterates.
Foreign-software pillar: "run software from another OS/arch" = the Win16/DOS/Win32 +
emulation track (#289 done OLE2/COM, #194 done 386 ISA, #288 future Win32). That is the
complementary half of "reprogram any part" and reuses this same consent/snapshot/verify loop
for installs.

## 3. #304 Snapshot + auto-rollback safety net

"SAFELY" requires that no user/AI change can brick the OS. Before any self-modification:
- SNAPSHOT the affected component(s): keep the prior artifact + source.
- After compile+deploy, VERIFY: build success + a functional/liveness check (the
  2-screendump + clock-advance + ping/RC rule from verify-liveness-two-screendumps.md).
- AUTO-ROLLBACK on: build failure, verify failure, or runtime fault.
Per-tier mechanism:
  - userland app: keep the prior /APPS binary; revert on fault.
  - compositor: keep prior /APPS/COMPOSIT; revert if the desktop does not come up.
  - kernel: A/B kernel SLOTS on the ESP + a bootloader that falls back to last-good if a
    user-built kernel faults early (watchdog/boot-count).
  - driver: load in a recoverable way (fault -> unload + rollback, never panic).
Generalizes the existing timestamped-backup discipline + the themes "defaults to roll back
to" (#141) into a first-class snapshot/rollback subsystem.

## 4. #305 Protected immutable security core

The ONE thing the user cannot reprogram. The trusted core = the capability/permission/
consent/audit subsystem (#293) AND the snapshot/rollback mechanism (#304) itself. The
self-modify/compile pipeline (#294) must REFUSE to touch the files/components that make up
this core (or require an out-of-band, non-AI path with separate authority). Otherwise a
malicious or buggy change could remove its own guardrails or disable rollback. Define the
trusted-core file/module boundary explicitly; the compiler + deploy tools enforce it as a
hard denylist; the boot chain verifies the core's integrity (signature/hash) before honoring
any self-applied change.

## 5. Dependencies / sequencing

#293 (tokens+consent+audit) is the foundation; #304 (rollback) is the safety net; both must
exist before #294 (compiler) is safe for end users; #305 (protected core) gates what #294 may
touch. So: #293 -> #304 -> #305 -> #294 (userland) -> widen #294 up the stack. The AI tool
layer (#292) is already shipped and is the surface all of this plugs into. Verify everything
with the liveness rule. Keep the foreign-software (Win16/Win32) track advancing in parallel;
it shares the install/consent/snapshot loop.
