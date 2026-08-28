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
| `DELAY_SPIN`    | a loop whose body is nothing but hardware delay calls (`io_wait`/`pause`/`udelay`), which pegs a core for the whole wait |
| `PRIVATE_SPINLOCK_IRQON` | **(#114)** a hand-rolled spinlock acquire loop that never masks `RFLAGS.IF`. This is the #75 AB-BA deadlock shape |
| `PRIVATE_SPINLOCK` | **(#114)** a hand-rolled spinlock acquire loop that does mask `RFLAGS.IF`: not the deadlock shape, but still a private copy of `sync/spinlock.h` |

A loop whose body already uses a canonical primitive (`wait_event`,
`futex_wait`, `wake_up`, ...) is considered correctly routed and is NOT flagged.

Comments and string literals are stripped before scanning, `do { } while(cond);`
tails are not mistaken for empty loops, and vendored third-party trees
(`media/opus`, `media/tremor`, `media/faad2`, `grep-gnu`, `micropython`,
`rogue`, DOOM) plus the build `obj/` and the primitives' own `sync/` dir are
excluded.

## The `PRIVATE_SPINLOCK` family (#114), and why it is a rule and not a patch

`mm/heap.c` had a private `volatile int` + test-and-set lock that never touched
`RFLAGS.IF`. #347 fixed it. `mm/pmm.c`, **in the same directory**, had the
identical defect and was not fixed at the same time. It survived for months and
took a bespoke instrument plus a QMP dump of guest memory to find again.

What #75 measured, six samples 30 seconds apart with every value identical: a
core took `pmm_lock` with IF=1 and **without** the Big Kernel Lock, was
interrupted, and `cpu/idt.c` wraps every ISR in `bkl_acquire()`, so the holder
became a **BKL waiter while still holding its own lock**. A second core inside a
`SYSCALL` (which takes the BKL at entry) then wanted `pmm_lock`. AB-BA. Both
cores spun with IF=1 and neither halted, which is why "the owner stops taking
interrupts" and "a core halted holding the lock" were both the wrong picture.

Fixing three or four instances leaves the fifth free to appear next month, which
is exactly how `pmm.c` survived #347. Hence a rule.

**What it detects.** A loop (`while`, `for`, **or `do`/`while`**, whose body
precedes its condition and which the ordinary header scan skips) that retries an
atomic lock acquire. The acquire may be spelled with any of the `__sync_*`
builtins, any of the `__atomic_*` builtins, our own `atomic_xchg*` /
`atomic_cas*` helpers, **or raw inline asm** (`xchg`, `lock cmpxchg`,
`lock bts`). The asm branch scans a comment-stripped but string-PRESERVING copy
of the source: the instruction only ever appears inside an asm string literal,
which the normal stripper blanks out, while prose about `xchg` (`fs/blockdev.c`
and `exec/x86_16.c` both discuss it in comments) must not count. Spelling-breadth is load-bearing: the sweep that opened
#114 grepped only for `__sync_lock_test_and_set` and therefore **missed
`net/net_perf.c` entirely**, which uses `__atomic_test_and_set`. There is a
self-test case pinning that specific spelling, so narrowing the rule back to one
builtin fails the gate.

**What it deliberately does NOT flag.**

* A **trylock that gives up** (`if (test_and_set(&L,1)) return BUSY;`) is not a
  spin: a BKL owner that fails it returns instead of spinning, so the second
  half of the inversion cannot form. `net/sntp.c` and `drivers/xhci.c` are this
  shape.
* An acquire loop that **parks on the shared wait queue** (`__wait_prepare` +
  `__wait_event_wait_deadline`, the `fs/fat.c` `fat_lock()` and
  `drivers/usb_msc.c` `msc_cmd_lock()` shape) is correctly routed. Flagging it
  would push authors back towards a raw spin. There is a GREEN self-test control
  for this.
* Taking the **shared** `sync/spinlock.h` primitive. There is also a GREEN
  control for that, because a rule that flags the fix it is demanding gets muted.

**Kernel-only by construction.** `RFLAGS.IF` and the BKL exist only in Ring 0.

**Measured coverage** (`--self-test` pins each of these; the table is the
output of an adversarial probe run at #114, not an aspiration):

| Shape | Result |
|---|---|
| `while (__sync_lock_test_and_set(&L,1))` | caught |
| `while (__atomic_test_and_set(&L,...))` | caught |
| `do { } while (__sync_lock_test_and_set(&L,1));` | caught |
| `for(;;){ if(tas(&L,1)==0) break; }` | caught |
| `while(1){ if(!tas(&L,1)) return; }` | caught |
| `while (atomic_xchg32(&L,1) != 0)` / `atomic_cas32` | caught |
| `__atomic_exchange_n`, `__sync_bool_compare_and_swap` | caught |
| raw inline asm `xchgl` acquire | caught |
| plain non-atomic `while (L) { }` flag loop | caught, by `SPIN_BUSYWAIT` |
| **acquire hidden behind a called helper** | **NOT caught** |
| trylock that gives up | not flagged, by design |
| acquire loop that parks on the wait queue | not flagged, by design |
| `spinlock_acquire_irqsave` (the fix) | not flagged, by design |

**Known blind spot, stated rather than discovered later.** Detection is textual
and per-loop, so an acquire hidden behind a *called* helper is invisible: the
`for(...)` in `fs/fat.c` `fat_lock_noblock()` retries via `fat_lock_try()`, and
no atomic appears in the loop text. `find_delay_wrappers()` solves the analogous
problem for sleeps, scoped to one file; the same treatment for lock acquires is
not implemented. Judge the backlog by reading code, not by the hit count.

**IF-masking is judged on the enclosing function's RAW text**, not the
comment/string-stripped copy, because `cli` and `pushfq` only ever appear inside
asm string literals which the stripper blanks out. A consequence: if the `cli`
lives in the *caller* rather than in the acquire helper, the site is reported as
`PRIVATE_SPINLOCK_IRQON`. That is a false positive by construction and is
allowlistable, but the better answer is almost always to put the masking inside
the helper, which is what `spinlock_acquire_irqsave()` already does.

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
