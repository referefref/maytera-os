# diaglog-gate

FAILS THE KERNEL BUILD when a boot-path diagnostic can only be read over a
serial port.

## The rule

A function that `kernel/main.c` calls, whose name ends in
`_report` / `_selftest` / `_stats` / `_summary` / `_diag` / `_dump`, and which
emits anything at all, must reach a durable sink:

    bootlog_write()        the ordinary one. Safe from the FIRST LINE of main(),
                           long before any filesystem exists: it buffers in RAM
                           and bootlog_arm() replays the whole buffer to
                           /BOOTLOG.TXT once the root volume is writable. It
                           mirrors every line to serial anyway, so choosing it
                           costs nothing and loses nothing.
    bootlog_fault_write()  the no-lock, no-allocation, no-filesystem sibling.
                           Use it from an ISR, an exception handler, or anywhere
                           ON THE STORAGE PATH, where bootlog_write()'s flush
                           would re-enter the code that called it.
    usblog_write() / audiolog_write() / bootlog_heartbeat() / panic_log_write()

or be listed in `allowlist.txt` with `[LEGIT]` / `[DEBT]` / `[LEGACY]` and a
real justification. A stale entry also fails, so the baseline cannot rot.

## Why it exists

Serial is silent in GUI mode, and the two machines we most need evidence from,
the owner's ASUS laptop and the iMac14,4, have no serial port at all.

MEASURED 2026-08-26 against commit `3983a768` (golden build 2219, booted by the
owner on his laptop, stick recovered, `/BOOTLOG.TXT` 383 KB and healthy):

* `blk_stage_report()` was the ONLY evidence that the block write-staging
  corruption fix works on real hardware. Its author intended it to print on
  every boot INCLUDING the golden, for the right reason: an exclusive claim
  makes the fault it removes invisible, so a silent green line would prove
  nothing. It used `kprintf()`. `grep -aic blkstage` over the recovered log: 0.
* The `[BLKSTAGE] CORRUPTION IN PROGRESS` alarm, which names the victim LBAs
  while a filesystem's bytes are being replaced under it, had the same sink. On
  the machine class that lost a root filesystem to that bug, it could not have
  been read.
* A sweep of every boot-path report/selftest function `main.c` calls found 18
  more in the same shape. They are the baseline in `allowlist.txt`.

The mechanism was never missing. `bootlog_write()` has done exactly the right
thing for a long time, and the diagnostics that DO work in the field
(`[BSTAGE]`, `[VIDEOMODE]`, `[USERSPACE]`) all use it. What was missing was
anything that made the wrong choice visible, and `kprintf()` is both easier to
type and what the surrounding code already used.

## Scope, deliberately narrow

Not "every kprintf in the kernel". There are 545 distinct bracketed tags in the
tree; a gate that fires on ordinary tracing gets disabled, and then it is #514
all over again. The scope is boot-path summary functions, which is the exact
shape of the measured failures.

## Prove it

    make diaglog-gate-selftest

RED on a synthetic serial-only boot diagnostic, GREEN when the same fixture uses
`bootlog_write()`, plus the allowlist suppressing a known one and a stale
allowlist entry failing. The fixture is self-contained, so the proof does not
depend on the state of any real kernel file, and cannot go green because
somebody happened to fix the function it was written against.

Exit 0 clean, 1 violation, 2 cannot run (fail closed).
