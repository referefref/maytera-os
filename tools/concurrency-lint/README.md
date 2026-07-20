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
# from the kernel/ dir (wired into the Makefile, no kernel rebuild):
make lint            # exit 1 on any NEW violation
make lint-report     # full categorized backlog of every hit
make lint-baseline   # re-baseline the allowlist (use sparingly)

# or directly:
tools/concurrency-lint/concurrency-lint --root <source-root>
```

## CI / pre-commit

Run `make lint` (or `tools/concurrency-lint/concurrency-lint --root .`) in CI or
a pre-commit hook and treat a non-zero exit as a failure. It is fast, needs only
`python3`, and touches no build artifacts.

## Phase 2 (not yet done): runtime assertion

The companion kernel-side guard `wq_assert_may_block()` (panic if a no-block
context ever actually blocks at runtime) is a separate kernel code change and
build, deferred so it does not contend with an in-flight kernel build. It should
be added to the wait-queue / futex entry points and asserted in IRQ and
compositor-draw contexts.
