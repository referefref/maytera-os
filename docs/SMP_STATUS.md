# SMP Status: what has actually landed

This is the current-state record of MayteraOS symmetric multiprocessing (SMP).
It is distinct from the plan docs (`docs/BKL_DECOMPOSITION_PLAN.md`,
`docs/BRANCH_ASSESSMENT_SSE151_SMPSCHED.md`), which describe intended work. Every
row below was verified against the code on `dev`, not against doc prose.

## The one-line summary

SMP is default-ON: multiple CPUs come up and user processes are scheduled across
them. The big kernel lock (BKL) is still a single global lock held around every
syscall body, every ISR, and every context switch, with only two GUI hot-loops
narrowed out from under it. The correctness machinery that keeps SMP honest
(concurrency-lint, the no-block assert, per-switch FPU save, the TTF glyph-cache
race fix) is genuinely landed.

## Owner's hard rule (do not relitigate)

**SMP is a requirement, not a setting.** A benchmark showing SMP slower than
single-core is a BKL bug report, not a reason to disable SMP. A fair lock alone
buys nothing; the BKL is held roughly 96% of wall-clock and most of that is
inside syscall bodies, so the game is NARROWING the syscall body, not making the
one lock fairer. Do not "fix" a regression by turning SMP off.

## SMP fixes and machinery

| Item | Where (file:line / commit) | Status |
|------|----------------------------|--------|
| SMP user scheduling default ON | `g_smp_user_sched = 1` at `kernel/cpu/smp.c:261`; flip commit `09c4e59d` (#67, 2026-09-02) | LANDED |
| Build stages SMP on | `build/build-golden.sh:1030` writes `/SMPSCHED.TXT`; kernel reads presence only | LANDED |
| Disable path (opt-out) | `/NOSMPSCHED.TXT` on the ESP; gate is inverted (default ON) at `kernel/main.c:2905-2927`, confirmed `main.c:3213` | LANDED |
| AP bring-up (INIT-SIPI, trampoline, LAPIC) | `kernel/cpu/smp.c` (`smp_init()` :573, `lapic_init()` :614, `setup_trampoline()` :547-556, `lapic_send_init()` :698) + `kernel/cpu/trampoline.asm` + `kernel/cpu/apic.c` | LANDED |
| User procs scheduled across CPUs | per-CPU run queues `kernel/proc/process.c:1060`; APs pull user procs via `sched_ap_enter()` (`process.c:1975-1978`); directed AP kick `smp_wake_aps()` `main.c:4314-4318` | LANDED |
| #75: APs reached scheduler before `proc_init()` and never retried (secretly single-core) | fix at `main.c:4293` (build 1876), directed AP kick after the process table is up | LANDED |
| Per-switch FPU/SSE save | `kernel/proc/context_switch.asm`: `fxsave64` :136, `fxrstor64` :175 (fork path :323/:247); `xsave64` selected when `g_fpu_use_xsave` for AVX (#446/#588) | LANDED |
| noblock assert `wq_assert_may_block()` | `kernel/sync/noblock.c:120`; called from the wait path at `kernel/sync/waitq.c:79`; `[WQBLOCK]` message `noblock.c:141` | LANDED |
| concurrency-lint fails build on new spin/poll | `kernel/tools/concurrency-lint/`; order-only link prereq `kernel/Makefile:2336`; `make concurrency-lint-selftest` (`Makefile:2218`) | LANDED |
| TTF shared-glyph-cache SMP race PANIC fix | `g_ttf_lock` spinlock `kernel/gui/ttf.c:171`, `spinlock_acquire_irqsave` :687; stb glyph-size bound :738-742; commit `6d598675` (2026-09-19) | LANDED |
| BKL is a single global lock, default ON | `g_smp_bkl_full = 1` at `kernel/cpu/smp.c:370`; primitives `bkl_acquire`/`bkl_release` `smp.c:236` | STILL GLOBAL |
| Syscall body under the BKL | `kernel/proc/syscall.asm:138-145` acquire, `:197-200` release; `syscall.c:4359` "this whole syscall body runs with the BKL held" | STILL GLOBAL |
| Every ISR + context switch under the BKL | `smp.c:890` "idt.c wraps every ISR in bkl_acquire"; released across a switch via `bkl_release_all()` | STILL GLOBAL |
| BKL narrowing Stage 1: `SYS_WIN_BLIT` row loop | commit `037526a3` (#168 step 3); control arm `/NOBLITNARROW.TXT`; counters `syscall.c:518-524`; measured 8.5 to 30.5 fps at 4 vCPU | LANDED (default) |
| BKL narrowing Stage 2: `SYS_WIN_INVALIDATE` / `uw_commit_content()` memcpy | commit `fd6deada` (#168 stage 2); control arm `/NOINVNARROW.TXT`; counters `syscall.c:507-515` | LANDED (default) |
| BKL narrowing Stages 3 to 8 (and deleting the BKL) | `docs/BKL_DECOMPOSITION_PLAN.md` | NOT DONE |
| Scheduler BKL serialization | `g_sched_bkl_serialize` (#75, commit `30050aa9`); control arm `/NOSCHEDBKL.TXT` | LANDED (default) |
| Fair-ticket BKL experiment | `g_bkl_fair` DEFAULT OFF (`main.c:3147-3170`); opt-in `/BKLFAIR.TXT` | OFF by default |

## The BKL, plainly

The BKL is still one recursive, owner-tracked global lock, ON by default. It is
taken on every syscall entry, wrapped around every ISR, and held across context
switches. Two GUI hot-loops (the `SYS_WIN_BLIT` per-row blit and the
`SYS_WIN_INVALIDATE` content-commit memcpy) now drop the lock for their copy
body and retake it, which is where the measured framerate gains come from. That
is the entire extent of the narrowing that has landed. The plan's later stages,
including removing the BKL, are not done. Judge the backlog by reading the code,
not by the plan's stage list.

## Latent hazard to remember when narrowing further

`csprng_bytes()` mutates the global DRBG state without its own lock. This is safe
today ONLY because syscall bodies run under the BKL (CHANGELOG, around line
1039). If the BKL is ever narrowed off syscall bodies, `csprng_bytes()` needs its
own lock first. This is the shape of every "safe because the BKL serializes it"
assumption: each one becomes a race the moment the body it lives in leaves the
lock.

## Known correctness gaps (documented, not fixed)

- **Plain (non-irqsave) spinlocks are NOT detected by the no-block assert.** A
  timer IRQ can preempt a plain-spinlock holder and switch to an innocent thread
  that would then be falsely accused, so a global lock-depth counter would be
  unsound. This is written down as a known gap at `kernel/sync/noblock.h:45-51`,
  not fixed. A sound version needs the depth on `process_t`.
- **The concurrency-lint allowlist is not a measure of the spin backlog.**
  `kernel/tools/concurrency-lint/allowlist.txt` is 211 lines (151 `[LEGIT]`, 13
  `[LEGACY]`, 5 `[DEBT]`). `[LEGACY]` entries were bulk-baselined and not
  individually reviewed; treat them as backlog. Some real spins are invisible to
  the lint (a loop body that also scans a table reads as progress), so the
  allowlist size can stay flat while a real spin is fixed.

## Doc that contradicts the code (flagged)

`docs/BRANCH_ASSESSMENT_SSE151_SMPSCHED.md` (around lines 61-66) is STALE and now
FALSE on its central SMP claim. It states `g_smp_user_sched = 0` at
`kernel/cpu/smp.c:65` and concludes "DO NOT build a gate-ON golden". The code now
has `g_smp_user_sched = 1` at `smp.c:261`, the gate is inverted to opt-out via
`/NOSMPSCHED.TXT`, and gate-ON is the shipping default. That document was last
committed 2026-08-22, ten days before the flip commit `09c4e59d` (2026-09-02).
Its unrelated SSE-in-MMIO / KVM analysis is not contradicted; only its SMP
default claim is. `docs/BKL_DECOMPOSITION_PLAN.md`, by contrast, holds up: it
marks Stages 1 and 2 done (matching commits `037526a3` / `fd6deada`) and is
honest that the later stages are not.
