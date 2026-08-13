# Local patches to the vendored NetSurf trees

The five NetSurf core libraries are fetched pristine at pinned commits by
`../fetch-upstream.sh`; they are not vendored into this repo. Anything we must
change in them lives here as a patch, and `fetch-upstream.sh` applies every
`*.patch` in this directory (sorted) after cloning.

A patch here must be:

- **minimal** and confined to one concern,
- **marked in the source** with a `MAYTERAOS LOCAL PATCH (#NNN)` comment, so the
  edit is obvious to anyone reading the vendored file,
- **checked by the build**. `build/browser-port-drift.sh` asserts that the live
  tree actually carries each marker; without that, a re-fetch would silently
  revert the patch and the golden would ship the unpatched engine, which is the
  same failure shape (#386/#650/#659) that script exists to prevent.

| Patch | Target | Why |
|---|---|---|
| `0001-libhubbub-gate-treebuilder-mode-printf.patch` | `libhubbub/src/treebuilder/treebuilder.c` | Upstream's per-token insertion-mode `printf` is ON unless NDEBUG is defined, and no MayteraOS build defines NDEBUG. It cost 133.9 s of a 362.9 s 1MB page load (#245). Gated behind `-DHUBBUB_DEBUG_MODES`, OFF by default. |
