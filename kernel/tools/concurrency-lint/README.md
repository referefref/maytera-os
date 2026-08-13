# concurrency-lint (#426)

Enforces the mandatory CLAUDE.md rule **"Reuse the existing shared primitives;
NEVER reinvent them"** for the waiting/blocking family. It statically scans the
MayteraOS kernel and userland C sources and FAILS the build on any **new**
hand-rolled busy-wait / spin / poll loop, the bug class that produced the
freeze/hang tickets **#211 #212 #230 #231 #347 #381 #419 #420**.

## The rule it enforces

All sleeping/waiting must go through the shared primitives:

- **Wait queue** `kernel/sync/waitq.h`: `wait_event(wq, cond)` /
  `wait_event_interruptible(wq, cond)`. A producer flips the condition and calls
  `wake_up(&wq)`. This is the canonical block-until-condition mechanism.
- **Futex** `kernel/sync/futex.c` (`futex_wait` / `futex_wake`) for userland and
  fast-path in-address-space waiting.

You must **NEVER** hand-roll:

- a busy-wait: `while (!ready) ;` / `for (;;) {}` spinning on shared state,
- a `proc_yield()` spin loop: `while (cond) proc_yield();`,
- a short-sleep poll loop: `while (cond) proc_sleep(1);` / `msleep(small)`,
- blocking/sleeping in a **no-block context** (IRQ/ISR handler or the compositor
  draw thread). Those must never block; do async-fetch-then-cache instead.

If a needed primitive is missing or too weak, **improve the shared one** so
everyone benefits. Do not fork a private copy into your subsystem.

## What it flags

| Rule | Pattern |
|------|---------|
| `SPIN_BUSYWAIT` | empty-bodied loop that spins the CPU: `while(<pure read>) ;`, `for(;;) ;`, or a calibrated `for(volatile ...)` delay loop |
| `YIELD_SPIN`    | a loop whose only wait mechanism is `proc_yield()` (no shared primitive in the body) |
| `SLEEP_POLL`    | a loop containing `proc_sleep(N)` / `msleep(N)` with a small `N`, not routed through a blocking primitive |
| `NOBLOCK_BLOCK` | a sleep/yield/block call inside an IRQ/ISR handler or a compositor draw callback |

A loop whose body already uses a canonical primitive (`wait_event`,
`futex_wait`, `wake_up`, ...) is considered correctly routed and is NOT flagged.

Comments and string literals are stripped before scanning, `do { } while(cond);`
tails are not mistaken for empty loops, and vendored third-party trees
(`media/opus`, `media/tremor`, `media/faad2`, `grep-gnu`, `micropython`,
`rogue`, DOOM) plus the build `obj/` and the primitives' own `sync/` dir are
excluded.

## Baseline / allowlist

`allowlist.txt` records every site that already existed when the lint landed, so
the build is green today while any NEW spin/poll fails. Each entry is keyed by a
**fingerprint** `path|rule|sha1(normalized-snippet)[:12]`, which is stable
across line-number drift: unrelated edits that move a line do not spuriously
trip the check, but a genuinely new loop (new path or new code) does.

- Fix a site -> delete its allowlist line.
- Knowingly accept a reviewed exception -> add its fingerprint line with a real
  justification note (do not just re-baseline to silence it).

## Usage

```sh
# from the kernel/ dir. The BUILD GATE is kernel-scoped and runs automatically
# as an order-only prerequisite of the link, so a normal `make` fails on any
# NEW spin/poll loop:
make                        # concurrency-lint runs; a new violation stops the link
make concurrency-lint       # run the gate on its own
make concurrency-lint-selftest   # PROVE it: RED on synthetic spins, GREEN on clean code

# whole-tree sweep (kernel + userland). Needs a FULL checkout; the kernel build
# container only receives kernel/, which is why the gate itself is kernel-only:
make lint-all               # (`make lint` is the historical alias)
make lint-report            # full categorized backlog of every hit
make lint-baseline          # re-baseline, writes TODO notes the lint then REJECTS

# or directly:
kernel/tools/concurrency-lint/concurrency-lint --root <source-root> [--dirs kernel]
```

## Where this tool lives, and why (#514)

It used to live at `<repo>/tools/concurrency-lint/` and be invoked from
`kernel/Makefile` as `../tools/concurrency-lint/concurrency-lint`.
`build/build-golden.sh` ships **only** `git archive <commit> kernel` to the
kernel build container, so on the machine where the kernel is actually built
that path did not exist and `make lint` died with `No such file or directory`.
It was also never a prerequisite of any build target, so a normal `make` never
invoked it at all. Two independent reasons the documented guardrail could not
fail. The tool now travels inside `kernel/`, exactly like `rust-symbol-gate`,
`syscall-dispatch-gate`, `syscall-ptr-lint` and `copy-user-lint`.

## The `==` bug (#514)

`COND_PROGRESS_RE` decides whether a loop condition lets the loop make its own
progress; if it does, an empty body is not treated as a spin. The old pattern
was `\+\+|--|[-+*/|&^]?=(?!=)`, whose `=(?!=)` branch matched the **second**
`=` of a `==`, because that `=` is not followed by another `=`. So

```c
while (*flag == 0) ;      /* the single most canonical busy-wait we have */
```

was classified as "makes progress" and never flagged. Fixing it (plus detecting
polls hidden behind one-line delay wrappers such as `ftp_delay()`) took the
kernel hit count from 12 to 179. The rule existed, ran, and could not fire on
its headline case, which is why `--self-test` now exists: every rule has a
synthetic RED fixture that must actually go red.

## Allowlist categories

Every line needs a category and a real justification, or the lint fails:

| Category   | Meaning |
|------------|---------|
| `[LEGIT]`  | Individually reviewed, correct by design (pre-scheduler code with no scheduler to wait on, real-time pacing sleeps). Do not "fix" these. |
| `[DEBT]`   | Individually reviewed, accepted for now, should become a `wait_event`. Printed as a loud ledger on every build. |
| `[LEGACY]` | Bulk-baselined at #514, **not** individually reviewed. A backlog, not an endorsement. Never add one by hand. |

A stale entry (fingerprint matching nothing in the tree) also fails the lint, so
fixing a site forces deleting its allowlist line in the same commit and the file
cannot rot into a list of dead hashes.

## Phase 2 (DONE 2026-08-02): the runtime assertion

`wq_assert_may_block()` now EXISTS, in `kernel/sync/noblock.{h,c}`, and it runs in
the shipping kernel. It refuses to let a no-block context block, where "no-block"
means: the scheduler is not live, `proc_current()` is NULL, or `RFLAGS.IF` is clear
(an ISR or a `cli`+spinlock section). It is a generalisation of the private
`xhci_may_block()` (#614/#615/#616), which is now a one-line alias for it.

- It lives at the CHOKEPOINT: `__wait_prepare()` (which every `wait_event*()` macro
  passes through) and `futex_wait()` entry, before either takes a lock.
- Detection is always on; only the reaction is gated. Default = a rate-limited,
  per-call-site de-duplicated `[WQBLOCK]` line on serial with the caller address for
  `addr2line`. `make NOBLOCKPANIC=1` = `kpanic()` on the first offence.
- `make NOBLOCKTEST=1` compiles in the DELIBERATE VIOLATION (a `wait_event()` with
  interrupts off) that proves it fires. It has been watched firing on serial.

Known gap, stated rather than hidden: holding a plain (non-`irqsave`) spinlock is
NOT detected. See the long comment in `noblock.h` for why a global lock-depth
counter would be unsound and what a sound one would cost.

**And a caveat about THIS tool that Phase 2 exposed:** the lint does not contain
either of the two spins that had tickets. `proc_wait()` (#230) escapes `YIELD_SPIN`
because its body also scans a table, and `ata_dma_wait()` (#287) escapes
`COND_PROGRESS` because `timeout--` looks like progress. The allowlist size is
therefore NOT a measure of how much spin backlog exists.
