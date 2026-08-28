# skipgate

FAILS THE KERNEL BUILD when a self-test can decline to run and says so in a line
that reads like ordinary boot noise.

## The defect it was written for

`kernel/fs/perms.c` `perms_selftest()` printed this on every boot:

    [PERMS-SELFTEST] SKIP traversal vectors (/HOME/ADMIN not 1000:0750)

That reads as "this image has not been provisioned yet". It is not what it
means. The first-boot wizard lets the owner NAME the account, `proc/users.c`
derives the home from that name, so an owner called "james" gets `/HOME/JAMES`
and NEVER gets `/HOME/ADMIN`. The line therefore printed forever, on a
CORRECTLY PROVISIONED machine, and the directory-traversal half of the
permission model (the half that stops one account reading another's home) had
probably never been exercised on any real user's machine.

MEASURED 2026-08-27 on golden build 2234: the shipped `/CONFIG/PERMS.DB` holds
three entries (`/CONFIG/SHADOW`, `/HOME`, `/CONFIG`) and `/CONFIG/PASSWD` is
EMPTY, so the vectors could not run before provisioning either. Neither before
nor after. Never.

This is the project's most-repeated shape: a harness armed by a marker no image
carries. The concurrency lint that could not fail (#514). The invariant gate
that was documented but never wired (#514). `wq_assert_may_block()`, described
as shipped for a month before it was written (#514). `img_shadow_selftest()` and
`diskimg_boot_harness()`, which `main.c`'s own comment records as "compiled,
linked and CALLED every boot" and returning at their first line. Every one of
them passes every check that asks whether the code is REACHED, because it is.

## The rule

Inside a function whose name ends in `_selftest` (or `_selftest_worker` /
`_selftest_start` / `_selftest_report`), a printed line whose text says
SKIP / SKIPPED / skipping / "not run" / "not built" / "aborting self-test" /
"no checks" must be produced by

    selftest_notrun(group, reason)      kernel/security/selftest_registry.h

which does two things a bare `kprintf` cannot:

* it writes through `bootlog_write()`, so the line survives into
  `/BOOTLOG.TXT` and can be read off a returned USB stick. The two machines
  whose evidence matters (the owner's ASUS laptop, the iMac14,4) have NO SERIAL
  PORT, so a `kprintf` skip notice produces literally nothing there; and
* it REGISTERS the group, so `main.c`'s end-of-boot `selftest_notrun_report()`
  prints one durable summary line. A reader greps for one tag instead of
  knowing in advance which subsystem went quiet:

      [SELFTEST] 14 group(s) ran, 0 declined.
      [SELFTEST] *** 1 GROUP(S) DID NOT RUN *** (13 ran). ...
      [SELFTEST]   1/1 perms/traversal(session): the session user's home has NO entry ...

Anything that genuinely must print its own goes in `allowlist.txt` with
`[LEGIT]`/`[DEBT]`/`[LEGACY]` and a real justification. A stale entry also
fails, so the baseline cannot rot.

## Scope, deliberately narrow

Not "every function that can print the word SKIP". A gate that fires on
ordinary tracing gets switched off, and then it is #514 all over again. Only
`*_selftest`-shaped functions, which is the exact shape of the measured
failure. The `selftest_` PREFIX form is explicitly NOT matched, because it
caught the register's own `selftest_notrun()`: a gate that fires on its own
remedy teaches people to disable it.

## What this gate does NOT claim

It is a TEXT rule, not a semantic one. Two things it cannot see:

1. **A self-test that stops testing without saying anything at all.**
   `img_shadow_selftest()`'s bare `return;` on a missing marker file is
   invisible to it.
2. **A self-test whose assertions are structurally incapable of failing.**
   The example this section was written around was `ext2_selftest()`, whose
   three read probes were each `if (ino) { ... }` with no `else`, pinned to
   dev-image fixtures that ship on no image, while the function still ended
   with "self-test complete". **That one is FIXED** (#EXT2SELFTEST, 2026-08-27):
   it was retargeted at the mounted root, its probes are discovered rather than
   named, and every did-not-run goes through `selftest_notrun()`. It is no
   longer on the allowlist. The BLIND SPOT, however, is unchanged, so the
   example is kept here as the shape to watch for.

Both need the RUNTIME half: the `selftest_ran()` / `selftest_notrun()` counts in
`/BOOTLOG.TXT`, where "13 ran, 1 did not" is a number a reader can act on.
Neither half alone is enough, and neither is claimed to be.

### Would a text VERDICT rule close blind spot 2? MEASURED: no.

The obvious extension is "a `*_selftest` function must reach a verdict: it must
call `selftest_ran()`/`selftest_notrun()`, or emit a literal containing
PASS/FAIL/OK/MATCH/MISMATCH". It WOULD have caught `ext2_selftest`, which
printed "=== self-test complete ===" and no verdict token at all.

Measured over the kernel tree on 2026-08-27: **124** `*_selftest`-shaped
definitions exist and **30** of them would fail that rule. They are not 30
defects. Roughly half are `*_start_*` launchers and `*_worker` bodies that emit
nothing because they delegate, and several of the rest DO state a verdict in a
vocabulary the token list does not cover (`fold_selftest` prints "SELFTEST
FAILED", `usbvol_selftest` prints "%u checks, %d failure(s)"). Shipping it would
mean a thirty-entry bulk `[LEGACY]` baseline nobody had read, which is the exact
thing this allowlist's header records as having been refused, and the exact
#514 shape the gate exists to prevent.

So the rule is NOT added, and the honest position is recorded instead: the
tractable mechanism is the RUNTIME half, and the number that matters is its
ADOPTION. On golden build 2245 the end-of-boot summary read `6 group(s) ran`.
Converting this ONE function took it to `12 ran, 2 declined`, i.e. the register
covered six groups out of 124 self-test-shaped functions before and twelve
after. The gap to close is adoption, one conversion at a time, and the
reviewable scope for a future gate is the ~17 functions `main.c` calls on the
boot path (the diaglog-gate's scope), not all 124.

## Prove it

    make skipgate-selftest

Five cases, all synthetic and self-contained so the proof does not depend on the
state of any real kernel file:

* RED on a `*_selftest` printing its own SKIP;
* GREEN on the same fixture using `selftest_notrun()`;
* RED on UNDERCOUNT, one branch registered correctly and a second still printing
  its own. This case caught the gate's FIRST rule ("at least as many
  `selftest_notrun` calls as skip messages") going green on a real violation;
* the allowlist actually suppressing;
* a stale allowlist entry failing.

Exit 0 clean, 1 violation, 2 cannot run (fail closed).
