# volume-route-lint (#246)

Fails the kernel build when a call hands the ROOT filesystem handle
(`&g_fat_fs`) a path argument the tool cannot prove is a fixed root path.

## The bug class

`g_fat_fs` routes the ext2 root, the FAT ESP, `/SMB`, `/NFS` and a mounted disk
image. It does **not** route a hot-plugged removable volume; only
`hotplug_fs_or_root()` and `hotplug_volume_for_path()`
(`kernel/drivers/hotplug.h`) do. Code that hands it a path under a mount point
like `/USB0` is told a file that is plainly there is **absent**, and then does
something wrong with a straight face: an existence test takes a permission
gate's weaker branch, a read fails, a write lands on a different file of the
same spelling in the root namespace.

In two days the same fault was found **four separate times** in four unrelated
subsystems, every time by somebody tripping over a symptom: the Recycle-Bin
capture (`[escrowred]`), four of five non-delete capture hooks (`[usbescrow]`),
`rename`/`mkdir`/`rmdir`/`unlink` in `proc/syscall.c` (`[renamevol]`), and one
of three ownership-stamping sites. The sweep that followed
(`[volaudit]`) found eight more with a user-visible or security consequence.

## Usage

```
volume-route-lint [--dirs kernel] [--report] [--update-allowlist]
volume-route-lint --self-test
```

Exit 0 = no new violations. Exit 1 = a new violation, or a rotted allowlist.

`make volume-route-lint` runs it; it is an order-only prerequisite of the link
in `kernel/Makefile`, so a normal `make` runs it and a new violation stops the
build before `kernel.elf` is relinked. `make volume-route-lint-selftest` proves
it goes RED on the real pre-fix `sys_mkdir` taken from `716367de~1` and GREEN
on the fixed form.

## What it claims, exactly

It does **not** claim "this call is a bug". It cannot: whether a path can ever
name a removable volume depends on where the string came from, and that is
interprocedural taint. `sc_path_is_dir(kpath)` was blind, but its parameter is
tainted two frames up in `sys_chdir`, and no regex follows that.

It claims the checkable thing: **this call hands the root handle a path
argument the tool cannot PROVE is a fixed root path.** Three proof rules keep
it quiet on everything it can establish:

1. a string literal starting with `/`;
2. an identifier resolving to such a literal through a `#define` or a
   `static const char[]`;
3. a buffer whose **every** write inside the enclosing function is a
   `snprintf`/`sprintf`/`strcpy`/`strncpy` from a format or source literal
   starting with `/`.

Those take 507 candidate path arguments in the kernel down to 190 unprovable
ones (168 distinct fingerprints).

It strips comments before matching. That is not cosmetic: this tree's house
style quotes the defective code inside the comment that explains the fix
(`// [renamevol] #246: This was fat_mkdir(&g_fat_fs, path)`), so a lint that
reads comments reports every well-documented fix as a fresh bug.

## Why this is the SECONDARY defence

The primary defence for this class is the **runtime detector** in
`kernel/fs/volblind.c` + `kernel/rustkern/volblind.rs`. It fires if and only if
a path that genuinely lies under a genuinely mounted removable volume reaches
the root handle, so its false-alarm rate is zero by construction, and it sees
the routes no static tool can follow (a path built at runtime, a path arriving
through the DOS layer, a path a GUI app took from a directory listing).

Their blind spots are opposite and neither is sufficient alone. The lint stops
a new instance being committed, in the shapes it can read, on a machine with
nothing plugged in. The detector catches any instance at all, but only one that
is **exercised**, on a machine with a volume present.

## Allowlist discipline

Same rules as `kernel/tools/concurrency-lint`, deliberately, because a second
pattern for the same job is the fault this ticket exists to stop.

* Fingerprint = `path|function|sha1(enclosing_fn#argindex:expression)[:12]`.
  Keyed on the expression and the enclosing function, **not** the line number,
  so unrelated edits above a site do not trip the lint, while a genuinely new
  site in the same file does.
* Every row must carry `[LEGIT]`, `[DEBT]` or `[LEGACY]` **plus a real
  justification**, or the lint fails.
* A row whose fingerprint matches nothing in the tree fails too: a list of dead
  hashes is not a baseline.
* `[DEBT]` rows print as a loud ledger on every build.
* `[LEGACY]` means bulk-baselined at #246 and **not** individually reviewed.
  Treat as a backlog. Never add one by hand.
* `--update-allowlist` writes `TODO:` placeholders that the lint then rejects
  until a human fills them in.

Current baseline: 37 `[LEGIT]`, 44 `[DEBT]`, 87 `[LEGACY]`.

## Adding a covered function

`PATH_FN` in the script lists the `fat_*` functions whose path arguments this
watches, derived from the declarations in `kernel/fs/fat.h` that take
`(fat_fs_t *fs, const char *path, ...)`. `--self-test` case C re-derives that
set from `fat.h` and FAILS if one is missing, so the table cannot silently stop
covering something.
