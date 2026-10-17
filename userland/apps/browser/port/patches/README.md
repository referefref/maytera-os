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

PATCHES APPLY IN SORTED ORDER AND `0003` DEPENDS ON `0001`. `0003` corrects the
sentence in `0001`'s own comment that called it "the ONLY change in this
vendored file", which stopped being true the moment a second patch touched
`treebuilder.c`. `fetch-upstream.sh` iterates `patches/*.patch` sorted, so
`0001` is always applied first and the context matches; a `git apply --check`
of `0003` against a tree with no `0001` will fail, and that is the dependency
showing itself rather than a broken patch.

ONE MARKER PER CONCERN, NOT ONE PER FILE. `treebuilder.c` and `node.c` each
carry several of these changes now, so the markers are suffixed
(`#245 depth-cap`, `#245 scope-count`, `#245 dispatch-walk`,
`#245 ancestor-check`, `#245 entity-walk`) and the drift gate greps for each
suffix separately. A bare `MAYTERA_OS LOCAL PATCH (#245)` would be a prefix of
all of them, so any one surviving patch would vouch for all the others. The
`0001` printf gate is checked by its MECHANISM (`HUBBUB_DEBUG_MODES`) rather
than by comment text, so no patch's gate depends on another patch's prose.

| Patch | Target | Why |
|---|---|---|
| `0001-libhubbub-gate-treebuilder-mode-printf.patch` | `libhubbub/src/treebuilder/treebuilder.c` | Upstream's per-token insertion-mode `printf` is ON unless NDEBUG is defined, and no MayteraOS build defines NDEBUG. It cost 133.9 s of a 362.9 s 1MB page load (#245). Gated behind `-DHUBBUB_DEBUG_MODES`, OFF by default. |
| `0002-libdom-deep-dom-O1-insertion-paths.patch` | `libdom/src/core/node.c`, `libdom/src/events/event_target.{c,h}` | Three separate O(tree-depth) ancestor walks ran on EVERY node insertion, so building an n-deep DOM was O(n^2) from the parser and from `appendChild` alike. MEASURED at depth 32000: 1,024,192,000 iterations in the mutation-event dispatch walk (it runs twice per insertion), 512,080,000 in the inclusive-ancestor check and 512,080,000 in `_dom_node_readonly`'s entity walk. Each gets an O(1) fast path that is the same answer, not an approximation. |
| `0003-libhubbub-deep-dom-scope-count-and-depth-cap.patch` | `libhubbub/src/treebuilder/{treebuilder.c,internal.h,before_html.c}` | Two things. (a) `element_in_scope` scans the whole open-element stack whenever the sought type is absent, which every container start tag does for `p`: 512,048,001 failing iterations at depth 32000. A per-type occupancy count answers "absent" in O(1). (b) Upstream has NO nesting cap at all, so `insert_element` now clamps at `MAYTERA_DOM_MAX_DEPTH`. The cap is not a substitute for (a): libdom destroys a tree by recursing, and a 32000-deep tree segfaults on teardown with the 2 MB Ring 3 stack (MEASURED), which no complexity fix addresses. |
