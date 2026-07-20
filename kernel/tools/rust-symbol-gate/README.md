# rust-symbol-gate (#404 / #526)

Fails the build when the Rust kernel crate stops exporting a symbol the C side
depends on, or starts exporting one nobody declared.

## Why this exists

`rustkern.rs` reached **9,566 lines with 83 `#[no_mangle]` exports**, edited
concurrently by four agents in two days. There was no guard at all: **a silently
deleted export builds clean.** The archive just gets smaller and nothing fails.

The near-misses are real and are recorded in `blame.md`:

* an agent was **one command from pushing `rustkern.rs` wholesale**, which would
  have silently deleted all six of #502's live TLS 1.2 functions that
  `net/tls/tls.h` declares `extern`. It was caught only because that agent
  happened to md5 the remote against its base and diff the symbol lists by hand.
* a `mono_*` block **appeared to be clobbered** mid-task. It turned out to be an
  edit racing a serial rebuild, and the *only* tell was that
  `nm librustkern.a | grep mono` showed 5 symbols while `grep mono rustkern.rs`
  showed 0: **the artifact and the source disagreed.**

In both cases the thing standing between the tree and a lost subsystem was a
human remembering to run `nm`. That is not a control.

### "Wouldn't the linker catch it?"

Only sometimes, which is worse than never, because it breeds false confidence.
`ld` fails on an undefined reference **only if a caller is compiled into that
build**. Most of these exports sit behind a `-DRUST_*` strangler flag, and a
flag that is off compiles no caller. Delete such an export and the build is
clean, the kernel boots, and the subsystem is simply gone. This gate does not
depend on a caller existing.

## What it checks

Two mechanical questions against the **build artifact**, not the source. (The
source is exactly what lies to you when another agent is mid-write; the mono
incident above is the proof.)

| check | meaning | result |
|---|---|---|
| `MISSING` | a symbol in `rust-symbols.manifest` is not a defined global text symbol in `librustkern.a` | build FAILS |
| `UNDECLARED` | the rustkern crate exports a symbol the manifest does not list | build FAILS |

`UNDECLARED` exists so the manifest cannot silently rot. Adding an export means
adding a line, which is also where you record who owns the symbol.

It is deliberately **structural, not heuristic** (the shape #503's
`syscall-ptr-lint` established): it does not parse Rust and does not guess what
"should" be exported. `nm` is the oracle.

## Anti-vacuity

`blame.md` records ten false PASSes in one session, several of them harnesses
that measured nothing and printed green. This gate is fail-closed:

* an empty or unreadable manifest is an **ERROR**, not "0 symbols, all present";
* an archive with **no rustkern codegen units at all** is an ERROR, not
  "0 exports, therefore nothing undeclared" — that is the case that would
  otherwise pass while measuring literally nothing (e.g. if the crate were
  renamed and the member filter silently matched zero members);
* `nm` failing is an ERROR, not a skip;
* a missing gate script is a **build failure**, not a skip (see the Makefile).

### Proven in both directions

A gate that has never been shown to fail has proven nothing. Both were
demonstrated on the real tree at b843/b844:

```
# deleted the mono_ms_rs block (the exact symbol from the real incident)
rust-symbol-gate: FAILED
    MISSING  mono_ms_rs
make: *** [Makefile:939: rust-symbol-gate] Error 1     # kernel never linked

# added an export with no manifest entry
rust-symbol-gate: FAILED
    UNDECLARED  gate_negative_control_rs
make: *** [Makefile:939: rust-symbol-gate] Error 1
```

and it passes clean on the unmodified tree:

```
rust-symbol-gate: OK - 83 declared symbols all exported by librustkern.a; no undeclared exports.
```

## How it is wired

```make
$(TARGET): $(OBJECTS) $(RUST_LIB) | rust-symbol-gate
```

An **order-only** prerequisite (`|`) of the link:

* the target is `.PHONY`, so it runs on **every** build. A normal (non-phony)
  prerequisite would be skipped once `librustkern.a` was up to date, which is
  precisely the staleness hole that lets a deletion through.
* being order-only, it never marks `kernel.elf` out of date, so it does not
  force a needless 15 MB relink.
* if it fails, `make` stops before linking.

## Why the tool lives in `kernel/tools/` and not the repo-root `tools/`

`make lint` invokes `../tools/concurrency-lint/concurrency-lint`, which resolves
to `source/tools/` on the build container. **That directory does not exist
there**, so that lint cannot run where the kernel is actually built. The
repo-root `tools/` and the kernel tree are not the same shape on the build host
as they are locally.

A gate that silently skips is worse than no gate, so this one travels with the
Makefile that invokes it, and the Makefile **fails loudly** if the script is
missing rather than passing over it.

## Usage

```bash
make rust-symbol-gate     # run the gate (runs automatically on every build)
make rust-symbol-update   # report manifest drift without failing
```

`--update` deliberately does **not** rewrite the manifest: the manifest records
ownership per symbol, which the tool cannot infer. It prints what to add, and
you add it under the right section by hand.

## Files

* `rust-symbol-gate` - the gate.
* `../../rust-symbols.manifest` - the declared FFI surface (83 symbols).
* `split-rustkern.py` - the one-shot splitter that produced `rustkern/*.rs` from
  the original 9,566-line file. Kept as the record of exactly how the split was
  performed, and so the boundaries can be re-derived if ever questioned. It is
  not part of the build.
