# Browser engine port

The MayteraOS browser is not a hand-written renderer. It links:

| Component | Origin | In this repo? |
|---|---|---|
| libwapcaplet, libparserutils, libhubbub, libcss, libdom | NetSurf, pristine upstream | No, fetched at pinned commits |
| duktape.c / duktape.h / duk_config.h | Duktape 2.7.0, pristine upstream | No, fetched by release |
| `dom_hubbub_bind.c`, `css_select_bind.c`, `layout.c` | MayteraOS, original | **Yes** |
| `duk_dom.c`, `duk_support.c` | MayteraOS, original | **Yes** |
| `shim-include/`, `shim/` | MayteraOS, original | **Yes** |
| `cc.sh`, `build-all.sh`, `recompile.sh` | MayteraOS, original | **Yes** |

## Why the split

Vendoring 48MB of unmodified NetSurf plus 3.7MB of unmodified Duktape would
add nothing recoverable: both are re-fetchable, and neither carries a MayteraOS
patch. What was genuinely irreplaceable was the ~97KB of glue above, which
existed only on one build container, in no repository. A `git archive` of the
source of truth could not rebuild the browser. That is what this directory
fixes.

`fetch-upstream.sh` re-creates the third-party trees at the exact pinned
revisions the shipping browser was built against, then copies this glue over
them. The pins matter: the shim headers and the bindings track these APIs, and
upstream HEAD may not compile.

## Build

```
./fetch-upstream.sh [dest]        # default dest: <workspace>
<workspace>
```

`build-all.sh` needs host `perl`, `python3`, `gperf` and a host `cc`: NetSurf
generates several sources (charset aliases, HTML entity tables, an element-type
perfect hash, the libcss select/computed tables and the per-property parsers)
before anything can be cross-compiled. It then produces the five `.a` archives
and the three MayteraOS objects the browser Makefile links.

## Known issues in this layer

- **`cc.sh` builds `-fno-pic`.** Every object here therefore carries text
  relocations (941 to 2640 per app in the measured fleet). The browser itself
  is now PIE (`-pie`, `user-pie.ld`), so the loader applies those relocations
  at load time; they work, but they are what blocks W^X on the text segment
  (#640 leg 4). Rebuilding these libraries `-fPIE` is a prerequisite for
  read-only browser text.
- **`cc.sh` builds `-fno-stack-protector`.** The 141-app stack-canary rollout
  (#651) does not cover this code.
- **CORRECTED 2026-10-01 (#245 renderhang). This entry used to read "These
  objects are not rebuilt by the golden build", and it is no longer true.** It
  said a "Freshly-built BROWSER" meant a fresh `main.o` over pre-built engine
  objects, which would mean a fix to `layout.c` or `cssvar.c` could be committed
  here and never reach a golden. Since 2026-09-30 `build/build-golden.sh` copies
  the pristine third-party trees to a PRIVATE per-build root, overlays the
  COMMIT's glue from `userland/apps/browser/port/` on top, and then DELETES
  every object derived from an overlaid `.c` (it derives that list from the
  overlay directories rather than hardcoding it, so adding a file here cannot
  leave a stale object behind). The committed copy of the MayteraOS-authored
  glue is therefore what a golden compiles and ships. The five third-party `.a`
  archives are still pre-built and still not rebuilt by a golden; that half of
  the old claim stands. Verified by reading `build-golden.sh` (the overlay and
  the object sweep) and by a browser built from this tree.

  **What is still true, and bites during development:** a `make` in
  `userland/apps/browser` compiles `$(NS)/cssvar.o` from `$(NS)/cssvar.c`, where
  `$(NS)` is the PORT ROOT, not this repo directory. Editing the copy here alone
  changes nothing you can run. Copy it into your port root (use a private one:
  `cp -a <workspace> <mine>/nsport` and
  `MAYTERA_NS_ROOT=<mine>/nsport make`) and `build/browser-port-drift.sh` keeps
  the two from silently diverging.
