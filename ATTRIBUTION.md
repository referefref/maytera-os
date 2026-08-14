# Third-Party Asset Attribution

## What is NOT in the public source repository (read this before following a path below)

This file is the WHOLE-PROJECT inventory, one row per vendored COPY. The public
source repository at `github.com/referefref/maytera-os` is a CURATED SUBSET of
the internal source tree, so several components declared in this document are
deliberately absent from it. **Seeing a path in this file is not a claim that
the path exists in the public repository.** That distinction is written down
here because the opposite mistake has already been made once: a row kept
pointing at `kernel/games/doom` for months after the code moved, and nobody
could tell from the row alone.

Excluded from the public source subset by
`tools/release-gate/assemble-publish-tree.sh`, and why:

| Component | Path in the internal tree | Why it is not published as source |
|---|---|---|
| AssaultCube | `userland/apps/assaultcube` | App Store distribution. Its content licence forbids redistribution and its `vendor/` engine tree is excluded. |
| OpenArena | `userland/apps/openarena` | App Store distribution. Its `vendor/` tree carries lcc, which is explicitly not free. |
| GNU grep 2.5.4 | `userland/apps/grep-gnu` | GPLv3-or-later, and no GPLv3 licence document is tracked here yet. |
| vi (busybox + GNU regex) | `userland/apps/vi` | GPLv2-or-later combined with LGPLv3-or-later GNU regex; neither licence document is tracked here yet. |
| CuraEngine slicer | `userland/apps/curaslice` | AGPLv3, plus a Boost-licensed Clipper, with no licence documents tracked here yet. |
| Every `userland/apps/*/vendor` tree | various | Vendored upstream engine source is not republished. Where such a tree carries a notice this project owes, the notice text is carried in THIS file instead, which is why this file is itself published. ClassiCube is the worked example: its BSD-3 text is below, not in the excluded `vendor/` directory. |

These are EXCLUSIONS, not a claim that the obligations vanish. An obligation is
discharged where the component is actually distributed. Anything in this list
that later gains its authentic upstream licence text in-tree should be
re-included in the published subset rather than left out permanently.

## Icons (CoreUI Icons Free + Boxicons, current primary set, 2026-08-12, #745)

MayteraOS desktop, Start menu, dock and system-tray icons are, where a good
semantic equivalent exists, now sourced from two open icon libraries instead
of a patchwork of individually-hand-fixed glyphs. This replaced the SVG Repo
set below (now historical, see "Superseded" section) plus several of the
2026-07-20 (#562) original-artwork glyphs, specifically to eliminate a class
of bug where each icon's stroke weight/footprint had drifted independently
against an unwritten house convention (see `blame.md`, "geometry fix" entry
below and the earlier "Editor/Calculator/App Store" drift).

### CoreUI Icons Free

Source: https://github.com/coreui/coreui-icons (downloaded as
`archive/main.zip`, the `svg/free/` directory, all `cil-` prefixed files).

License: **CC BY 4.0** (https://creativecommons.org/licenses/by/4.0/), per
the repo's own `LICENSE` file: *"In the CoreUI Icons Free download, the CC BY
4.0 license applies to all icons packaged as SVG and JS file types."* This is
attribution-only, NOT share-alike, so it does not conflict with this
project's GPLv2 codebase (CC BY-SA remains banned here for exactly that
reason). Verified by reading the repo's `LICENSE` file directly, not assumed.

Only the `svg/free/` (CC BY 4.0) icon set was taken. The repo's `svg/brand/`
(828 files) and `svg/flag/` (198 files) directories were explicitly EXCLUDED:
brand icons are third-party trademarks under their own separate terms
("Please do not use brand logos for any purpose except to represent the
company, product, or service to which they refer" per the repo's own
LICENSE), and flags carry their own provenance; neither was needed here.
CoreUI Icons **Pro** (which adds Solid and Duo-Tone styles) was NOT used and
was not adopted on this task's own initiative - it is a paid tier requiring
the user's explicit approval, which was not sought because it was not
needed: the free download's icons are a single style (**Linear/outline**,
`cil-` prefix, 562 icons total), so no per-style picker was built (see
CHANGELOG #745 entry for the full reasoning).

The SVGs were recolored solid white (`fill="#ffffff"`, replacing CoreUI's
`fill="var(--ci-primary-color, currentcolor)"`) and rendered to the internal
`.ICN` (MICO) icon format used by the compositor; path geometry is untouched
(native 512x512 viewBox preserved in the committed source SVG - CC BY 4.0
explicitly permits recoloring/adaptation, and this project's own convention,
set by the SVG Repo batch below, has always been "recolor only, geometry
unmodified in form").

**Computer, Browser and Recycle Bin were swapped to CoreUI on 2026-08-12 and
REVERTED the same day (task #745/#63, user preference: "return the computer,
recycle bin and browser icons to their previous icons ... i prefer those").**
They are NOT part of the current CoreUI set - see "Reverted: Computer,
Browser, Recycle Bin" below for their actual current sourcing. Left out of
the table below entirely (rather than listed and marked reverted in place)
so this table is an accurate list of what CoreUI assets currently ship, with
no entry a reader has to cross-reference elsewhere to find out doesn't apply.

| Use | `.ICN` | CoreUI source file |
|-----|--------|---------------------|
| Terminal | `TERMINAL.ICN` | `cil-terminal.svg` |
| Settings | `SETTINGS.ICN` | `cil-settings.svg` |
| Generic game (fallback) | `GAME.ICN` | `cil-gamepad.svg` |
| IRC | `IRC.ICN` | `cil-comment-square.svg` |
| Editor | `EDITOR.ICN` | `cil-pencil.svg` |
| Calculator | `CALC.ICN` | `cil-calculator.svg` |
| Image Viewer | `IMGVIEW.ICN` | `cil-image.svg` |
| Audio Player | `APLAYER.ICN` | `cil-music-note.svg` |
| Media Player | `MPLAYER.ICN` | `cil-video.svg` |
| Clock | `CLOCK.ICN` | `cil-clock.svg` |
| Files | `FILES.ICN` | `cil-folder.svg` |
| Network | `NETWORK.ICN` | `cil-lan.svg` |
| Paint | `PAINT.ICN` | `cil-color-palette.svg` |
| Sliders (tray quick-settings glyph) | `SLIDERS.ICN` | `cil-equalizer.svg` |
| Chevron (down) | `CHEVD.ICN` | `cil-chevron-bottom.svg` |
| Chevron (right) | `CHEVR.ICN` | `cil-chevron-right.svg` |
| AI Chat | `CHATBUB.ICN` | `cil-chat-bubble.svg` |
| Weather | `WEATHER.ICN` | `cil-sun.svg` |
| Gallery | `GALLERY.ICN` | `cil-grid.svg` |
| Notes | `NOTES.ICN` | `cil-notes.svg` |
| Font Book | `BOOK.ICN` | `cil-book.svg` |
| Converter | `CONVERT.ICN` | `cil-swap-horizontal.svg` |
| Timers | `TIMERS.ICN` | `cil-av-timer.svg` |
| Python | `PYTHON.ICN` | `cil-code.svg` (still a generic `</>` glyph, deliberately not the trademarked Python logo) |
| Authenticator | `AUTH.ICN` | `cil-lock-locked.svg` |
| Help | `HELP.ICN` | `cil-life-ring.svg` |
| Launcher | `TILE.ICN` | `cil-apps.svg` |
| Task Switcher | `WINSWTCH.ICN` | `cil-layers.svg` |
| App Store | `APPSTORE.ICN` | `cil-cart.svg` |
| System Monitor | `MONITOR.ICN` | `cil-speedometer.svg` |
| Feeds | `RSS.ICN` | `cil-rss.svg` |
| Snapshot | `SNAPSHOT.ICN` | `cil-camera.svg` |

If any icon above is republished, retain attribution to CoreUI
(https://coreui.io/icons/) and the CC BY 4.0 license linked above.

### Boxicons (one icon, supplementary)

Source: https://github.com/atisawd/boxicons (`svg/regular/`, `bx-` prefixed,
the outline/"regular" style only - never the `bxs-` solid style, to stay
consistent with the CoreUI Linear style chosen above).

License: **CC BY 4.0** for the icon SVGs specifically, per the repo's own
`README.md`: *"The icons (.svg) files are free to download and are licensed
under CC 4.0 By... Attribution is not required but is appreciated."* (The
repo's top-level `LICENSE` file is MIT, but that MIT grant covers, in the
README's own words, "files which are not fonts or icons" - the icon SVGs
carry the separate CC BY 4.0 grant quoted above.) Verified by reading both
the repo's `LICENSE` file and `README.md` `## License` section directly.
Despite the README's "not required" wording, this project attributes it
anyway per CC BY 4.0's own terms and this project's standing rule that
attribution is a binding obligation, not a formality.

Used for exactly one icon, where neither CoreUI's free set nor the prior
in-house art had a glyph visually distinct from `SETTINGS.ICN` (both would
otherwise have rendered as near-identical cogs - a real, pre-existing defect
in the shipped set that this swap fixes rather than reproduces):

| Use | `.ICN` | Boxicons source file |
|-----|--------|------------------------|
| Services | `GEAR.ICN` | `bx-wrench.svg` |

### Icons kept bespoke (no CoreUI/Boxicons swap - #745)

Per the explicit "where possible" scope of this task, the following were
evaluated and deliberately NOT swapped, because forcing a generic library
icon onto them would misrepresent or genericize something the icon is
specifically supposed to identify:

| Use | `.ICN` | Reason kept |
|-----|--------|-------------|
| DOOM launcher | `DOOM.ICN` | id Software's own DOOM trademark/logo - a brand mark, not ours to swap; see the DOOM licensing note below. |
| Maytera Arena | `ARENA.ICN` | MayteraOS's own original game; the icon IS that game's product identity, not a generic app glyph. |
| Maytera Chess | `CHESS.ICN` | Same reasoning as Arena. |
| Maytera Squadron | `SQUADRON.ICN` | Same reasoning as Arena. |
| GL Cube | `GLCUBE.ICN` | Same reasoning as Arena (original MayteraOS tech demo). |
| GL Matrix | `GLMATRIX.ICN` | Same reasoning as Arena. |
| Solitaire | `SOLITR.ICN` | Represents a specific bundled game's identity; no suitable playing-card glyph exists in either free set anyway. |
| Win16 games category | `WIN3X.ICN` | MayteraOS-specific concept (the Win16 compatibility layer's game category) with no equivalent in a general-purpose icon library. |
| DOS games category | `DOSAPP.ICN` | Same reasoning as Win3x (the DOS compatibility layer). |
| 3D Print | `PRINT3D.ICN` | Neither library has an actual 3D-printer glyph, only flat 2D office-printer icons (`cil-print`/`bx-printer`); using one would misrepresent the feature - a wrong-but-generic icon is worse than keeping the existing bespoke one. |
| Desktop pet ("sheep") | n/a | Not an SVG/icon asset at all - drawn procedurally from primitives by the compositor (see "Desktop pet" section below); the user supplied/approved this art and has strong opinions about it regardless. |

`WIN3X.ICN`, `DOSAPP.ICN`, `SOLITR.ICN` and `DOOM.ICN` have no SVG source
committed in `assets/icons/svg/` (predates any source being committed for
them - a pre-existing gap, not introduced by this task; noted in
`assets/icons/README.md`).

### Reverted: Computer, Browser, Recycle Bin (2026-08-12, task #745/#63)

Swapped to CoreUI earlier the same day, then reverted a few hours later per
explicit user preference ("return the computer, recycle bin and browser
icons to their previous icons in place of the new ones as i prefer those").
`.ICN` bytes were restored byte-for-byte from a pre-swap asset-base backup
(`maytera-2part-ext2-b850.img.bak-20260811_045011`, sha256-verified against
the live asset base after restore and against `build/asset-manifest.sha256`,
which had never been updated for the CoreUI swap in the first place - see
CHANGELOG for that finding), not regenerated from a re-typed source, so this
is an exact restoration, not a re-creation.

**These three are NOT "one restore, one shape": Computer and Browser return
to SVG Repo (CC BY) sourcing, Recycle Bin returns to original MayteraOS
artwork - do not describe all three as "original artwork" or as "CC BY",
only Recycle Bin is the former and only Computer/Browser are the latter.**
The task brief that requested this revert assumed all three had been
original artwork before the CoreUI swap; that was true only for Recycle Bin
- checked against this file's own pre-swap content (`git show 8ae060a^:ATTRIBUTION.md`),
not assumed.

| Use | `.ICN` | Source | License |
|-----|--------|--------|---------|
| Computer | `COMPUTER.ICN` | SVG Repo ID 533134, "monitor-alt-4" (https://www.svgrepo.com) | CC BY (https://www.svgrepo.com/page/licensing/#CC%20Attribution) |
| Browser | `BROWSER.ICN` | SVG Repo ID 443597, "browser-general" (https://www.svgrepo.com) | CC BY (https://www.svgrepo.com/page/licensing/#CC%20Attribution) |
| Recycle Bin | `RECYCLE.ICN` | Custom hand-drawn lineart trash can, `assets/icons/recycle.svg` | Original MayteraOS artwork, not third-party |

No SVG source is committed for the restored `COMPUTER.ICN`/`BROWSER.ICN` -
same pre-existing gap `WIN3X.ICN`/`DOSAPP.ICN`/`SOLITR.ICN`/`DOOM.ICN` above
already have (these three predate any SVG-Repo source ever being committed
to this tree; only the compiled `.ICN` ever shipped). `RECYCLE.ICN`'s source
IS committed, unchanged throughout: `assets/icons/recycle.svg` (see
`assets/icons/README.md`, which no longer describes it as unused).

### Superseded: SVG Repo (Creative Commons Attribution) - historical, mostly no longer shipped

Before 2026-08-12, the icons listed in the CoreUI table above (Terminal,
Settings, IRC, Editor, Calculator, Image Viewer, Audio Player, Media Player,
Clock, Files, Network) plus Computer and Browser (see "Reverted" above -
these two DO still ship, from this same SVG Repo source, as of the revert
documented above) were derived from SVG icons obtained from **SVG Repo**
(https://www.svgrepo.com), distributed under the **Creative Commons
Attribution (CC BY)** license (license reference:
https://www.svgrepo.com/page/licensing/#CC%20Attribution). Every icon in
this section OTHER than Computer/Browser was swapped to CoreUI Icons Free
(also CC BY) on 2026-08-12, #745, and stayed swapped - this note is retained
for provenance only for those. `Recycle Bin` was, before that same swap, a
custom hand-drawn lineart trash can (`assets/icons/recycle.svg`, not
third-party); it is back to that same artwork per the revert above, not
superseded.

### Original icons added 2026-07-20 (#562) - PARTIALLY SUPERSEDED 2026-08-12

`ICON_GAME` (the generic game glyph, `main.c`) used to load `DOOM.ICN` directly,
so every app that fell back to it (Maytera Arena, Maytera Chess, Maytera
Squadron, GL Cube, GL Matrix) rendered the DOOM logo instead of its own icon.
Fixing that, plus a wider pass giving every app its own icon instead of sharing
one, needed 17 new glyphs, hand-drawn as **original MayteraOS artwork**
(`assets/icons/svg/*.svg`), not sourced from SVG Repo or any third party.

**As of 2026-08-12 (#745), most of that batch has been superseded by the
CoreUI/Boxicons swap documented above** (see that section's tables for the
current source of each). The ones still shipping as original MayteraOS
artwork, unchanged, are `GAME.ICN`'s per-game siblings and `PRINT3D.ICN` -
listed in the "kept bespoke" table above with the reason each was kept.
`GAME.ICN` itself (the generic fallback glyph) WAS swapped to CoreUI's
`cil-gamepad.svg`.

Superseded from this batch: `WEATHER.ICN`, `GALLERY.ICN`, `SNAPSHOT.ICN`,
`NOTES.ICN`, `CONVERT.ICN`, `TIMERS.ICN`, `PYTHON.ICN`, `AUTH.ICN`,
`WINSWTCH.ICN`, `APPSTORE.ICN`, and (from the "seven more apps" list just
below) `CHATBUB.ICN`, `RSS.ICN`, `BOOK.ICN`, `HELP.ICN`, `MONITOR.ICN`,
`TILE.ICN` (all now CoreUI) and `GEAR.ICN` (now Boxicons). This paragraph is
kept for provenance history; do not treat any filename mentioned here as
current without checking the CoreUI/Boxicons tables above first.

Seven more apps that were sharing another app's icon at the time pointed at an
**existing, previously-unwired** icon file already in the asset set (not new
art, just newly loaded via `icon_load_color()`): AI Chat -> `CHATBUB.ICN`,
Feeds -> `RSS.ICN`, Font Book -> `BOOK.ICN`, Help -> `HELP.ICN`, System
Monitor -> `MONITOR.ICN`, Services -> `GEAR.ICN`, Launcher -> `TILE.ICN`. (All
seven now point at the CoreUI/Boxicons-sourced files per the table above; this
paragraph documents what app each icon belongs to, which did not change.)

### Geometry fix + Paint replacement, 2026-08-12 (#745) - SUPERSEDED SAME DAY

The below describes a same-day, earlier fix (hand-adjusting stroke widths on
the SVG-Repo-derived Editor/Calculator/App Store icons, and replacing Paint
with a new bespoke glyph) that has since been **superseded by the CoreUI
swap** in the section above: Editor, Calculator, App Store and Paint are now
all CoreUI-sourced (`cil-pencil`, `cil-calculator`, `cil-cart`,
`cil-color-palette`), which is precisely the "one professionally-designed,
internally-consistent set" fix that made the hand-tuning below unnecessary
going forward. Kept for provenance/history, not as a description of what
currently ships.

USER-REPORTED: the Editor, App Store and Calculator icons did not match the
square format the rest of the dock used, and the Paint icon was full color
while the rest of the set is monochrome.

Measured the canon by decoding every shipped `.ICN` (12-byte MICO header +
ARGB pixels) and by reading every committed source SVG: canvas is 64x64 for
the whole set (Editor/Calculator/App Store were already 64x64 too, so the
"format" complaint was not the container size), and the established stroke
convention across the SVG-Repo-derived and original-artwork icons alike is
`stroke-width="6"` for primary lines (`stroke-linecap="round"`,
`stroke-linejoin="round"`), 5 for secondary interior lines, and 4.5 for fine
accents, solid `#fff` fill for small accent shapes, white-on-transparent, no
background badge. Rendering every icon through the compositor's own
nearest-sample scaler (`icon_draw_scaled`/`color_icon_blit` in
`userland/apps/compositor/icons.c`) at the real on-screen dock size (40px,
`XFCE_DOCK_ICON`) showed the actual defect: `EDITOR.svg` and `CALC.svg` used
`stroke-width="3"`/`"4"`, roughly half the family's weight, so their linework
rendered visibly thinner and greyer next to the rest of the dock at true size.
`APPSTORE.svg` already matched the stroke convention (6 / 4.5) but its
clipboard glyph was scaled smaller within the canvas than its neighbors
(content span ~69% of the long axis vs. the family's ~75-85%).

**Editor and Calculator** (`assets/icons/svg/EDITOR.svg`, `CALC.svg`) remain
the same SVG-Repo-derived CC BY icons listed above (`file-pencil-alt` /
`calculator`); only the stroke widths were brought up to the house 6/5
convention and Editor's pencil was resized to match, per CC BY's explicit
permission to adapt. **App Store** (`APPSTORE.svg`) is unchanged art, scaled
up ~15% around its own center to bring its footprint into the family range;
still `APPSTORE.ICN` white lineart, unrelated to the AI-generated one noted
above.

**Paint** (`PAINT.ICN`) previously carried the full-color glossy
OpenAI-gpt-image-2-generated palette tile from the 2026-07-11 Maytera Studio
work (see CHANGELOG). That tile is now removed from the shipping icon set and
replaced with `PAINT.svg`, an **original MayteraOS line-art glyph** (palette
outline with a thumb-hole notch, 4 filled paint-dot accents, one brush-handle
line) drawn in the same house style as the `#562` original-artwork batch
above (stroke-width 6 primary / 5 accent, `#fff` fill dots, white-on-transparent,
64x64 canvas), not sourced from any third party. This keeps the Paint glyph
on the same original-work footing as Game/Chess/Squadron/Weather/etc., with
no license question at all (own original creation) rather than trying to
match a crowd-sourced third-party icon's exact license tag against this
repo's public/CC0-only bar for new sourced assets.

## Desktop pet

The "sheep" desktop pet in MayteraOS is drawn procedurally from primitives by
the compositor; it does not embed third-party sprite artwork.

## Fonts

`FONT.TTF` on the boot image is **DejaVu Sans** (from the DejaVu fonts project,
derived from Bitstream Vera). Bitstream Vera is distributed under the Bitstream
Vera license (a permissive, redistributable license); DejaVu's own changes are
released into the public domain. Both permit redistribution.

The `/FONTS` directory on the shipped image carries 53 TrueType faces from 14
families in total (DejaVu/Bitstream Vera above, plus Lato, Noto, Inconsolata,
Catamaran, IBM Plex Mono, Libertinus Serif, Source Sans/Serif/Code Pro under
the SIL Open Font License 1.1, and Symbola under public domain). Every
family's full license text is reproduced in `build/font-licenses/LICENSE.TXT`,
which ships on-image at `/FONT-LICENSES` (see
`tools/release/assemble-publish-tree.sh`, `INCLUDE_BUILD`). **Liberation is
not among the shipped fonts** - checked directly against
`build/font-licenses/` and the on-image font set, 2026-08-12 (#745, local queue item 57);
there is therefore no exposure to Liberation's 1.x-vs-2.x license split
(GPL+exception for 1.x, OFL for 2.x only).

## Vendored open-source libraries

MayteraOS adapts the open-source projects listed below.

**Read the Location column as "one row per COPY, not one row per project."**
Until 2026-08-13 this table had one row per project, and that shape is wrong
for this tree: components here get vendored more than once (GNU regex lives in
two directories, DOOM lived in two, apps carried their own `cxxsupp.cpp`,
nine directories carry a private `limits.h`). A single row per project makes
the second copy invisible, and that is precisely how
`userland/apps/vi/vendor/gnuregex` and `userland/apps/vi/vendor/busybox`
shipped with no entry at all while `userland/apps/grep-gnu/lib` had one. Every
copy now gets its own row, even when two rows name the same upstream project.

`tools/license-audit/vendor-attribution-check.sh` enforces this table
mechanically; see "How this table is kept honest" below.

| Component | Location (one row per copy) | License | License text in tree |
|-----------|-----------------------------|---------|----------------------|
| libmad (MP3 decode) | `kernel/media/libmad` | GPLv2+ | per-file headers + `COPYING` |
| faad2 (AAC decode) | `kernel/media/faad2` | GPLv2 | `COPYING` + `AUTHORS` |
| Tremor / libogg (Vorbis) | `kernel/media/tremor` | BSD-style (Xiph) | `COPYING`(+`.libogg`) |
| Opus | `kernel/media/opus` | BSD-style (Xiph) | `COPYING` + `AUTHORS` |
| dr_flac | `kernel/media/dr_flac` | public domain / MIT-0 | `COPYING` |
| stb_truetype (Sean Barrett / RAD Game Tools) | `kernel/gui/stb_truetype.h` | public domain, or MIT at your option (dual, per the file's own tail) | in-file header |
| Ed25519 verification, derived from TweetNaCl | `kernel/crypto/ed25519.c` | public domain (TweetNaCl) | in-file note naming the TweetNaCl authors |
| Nova prompt-injection ruleset | `kernel/security/nova.c` | MIT | in-file note |
| Mozilla CA bundle | `kernel/fs/CERTS/ca-bundle.crt` | MPL 2.0 (claimed; see the note below) | **none: the promised in-file note does not exist** |
| Realtek 88x2bu register tables | `kernel/drivers/net/wifi` | GPL (register facts) | in-file note |
| MayteraOS libc (first party, listed so the table is a complete inventory) | `userland/libc` | MIT | `LICENSE` + per-file SPDX headers |
| TinyGL | `userland/libgl` | zlib-style, **mandatory in-product acknowledgment** (see note below) | `src/LICENSE` |
| MicroPython port glue | `userland/python/micropython/ports/maytera` | MIT (port glue only; upstream MicroPython is itself MIT) | upstream |
| Duktape (JS engine core) | `userland/apps/browser/port/duktape` holds MayteraOS glue only; the engine is fetched, not tracked (see note below) | MIT (upstream) | not tracked |
| NetSurf core libs (libwapcaplet, libparserutils, libhubbub, libcss, libdom) | `userland/apps/browser/port/netsurf` holds MayteraOS glue only; the libraries are fetched at pinned revisions, not tracked (see note below) | MIT (upstream NetSurf core libs) | not tracked |
| DOOM (id Software) | `userland/apps/doom` | GPLv2 (id relicensed the DOOM source in 1999; the stale per-file headers still name the 1997 DOOM Source Code License) | `userland/apps/doom/LICENSE.TXT`, fetched verbatim from id's own upstream release, plus `userland/apps/doom/LICENSING.md` recording provenance and the header discrepancy |
| Rogue 5.4.4 (Michael Toy, Ken Arnold, Glenn Wichman) | `userland/apps/rogue` | BSD 3-clause | `LICENSE.TXT` |
| GNU grep (installs as `/APPS/GREP`, the only grep on the image since #745 local 98) | `userland/apps/grep-gnu/src` | GPLv3-or-later | none tracked **NOT IN THIS REPOSITORY** |
| GNU regex + gnulib, **copy 1 of 2** | `userland/apps/grep-gnu/lib` | LGPLv3-or-later | per-file headers **NOT IN THIS REPOSITORY** |
| busybox `vi.c` 1.36.1 | `userland/apps/vi/vendor/busybox` | GPLv2-or-later | none tracked; `userland/apps/vi/PROVENANCE.md` pins the upstream tarball and md5 **NOT IN THIS REPOSITORY** |
| GNU regex, **copy 2 of 2** | `userland/apps/vi/vendor/gnuregex` | LGPLv3-or-later | per-file headers **NOT IN THIS REPOSITORY** |
| CuraEngine | `userland/apps/curaslice/src` | AGPLv3 | `userland/apps/curaslice/LICENSE.curaengine`, which sits at the app root rather than beside the code it covers **NOT IN THIS REPOSITORY** |
| Clipper (Angus Johnson) | `userland/apps/curaslice/libs/clipper` | Boost Software License 1.0 | none tracked; the file header cites the Boost licence by URL **NOT IN THIS REPOSITORY** |
| ClassiCube engine | `userland/apps/classicube/vendor/ClassiCube` | modified BSD 3-clause | `license.txt` |
| FreeType, bundled *inside* ClassiCube | `userland/apps/classicube/vendor/ClassiCube/src/freetype` | FreeType Project License, or GPLv2, at your option | reproduced inside ClassiCube's own `license.txt` |
| AssaultCube / Cube engine (Wouter van Oortmerssen and the AssaultCube team) | `userland/apps/assaultcube/vendor/AC` | zlib-like Cube licence (zlib plus an extra clause) | `source/README_CUBEENGINE.txt` **NOT IN THIS REPOSITORY** |
| ENet (Lee Salzman) | `userland/apps/assaultcube/vendor/AC/source/enet` | MIT | `LICENSE` beside the code, plus a second copy of the same text at `userland/apps/assaultcube/vendor/enet_LICENSE.txt` **NOT IN THIS REPOSITORY** |
| SDL2, zlib, libogg, libvorbis, OpenAL and GL headers bundled by AssaultCube | `userland/apps/assaultcube/vendor/AC/source/include` | zlib (SDL2, zlib) and BSD-style (Xiph) per file | per-file headers + `SDL_copying.h` + `README_jpeg.txt` **NOT IN THIS REPOSITORY** |
| OpenArena / ioquake3 engine | `userland/apps/openarena/vendor/OA` | GPLv2 | `source/COPYING.txt` **NOT IN THIS REPOSITORY** |
| libjpeg 8c (Independent JPEG Group) bundled by ioquake3 | `userland/apps/openarena/vendor/OA/source/code/jpeg-8c` | IJG licence | `README` **NOT IN THIS REPOSITORY** |
| lcc retargetable C compiler bundled by ioquake3 | `userland/apps/openarena/vendor/OA/source/code/tools/lcc` | **NOT free**: "you may not sell lcc or any product derived from it" | `COPYRIGHT` **NOT IN THIS REPOSITORY** |
| Freedoom-derived sprite and weapon art | `userland/apps/arena/assets` | BSD 3-clause | `FREEDOOM-COPYING.txt` (three copies, one per asset directory) |
| Reproduced licence texts for the 14 shipped font families | `disk/FONT-LICENSES` (this repository; `build/font-licenses` in the internal tree) | OFL 1.1, Bitstream Vera, public domain, per family | the directory IS the licence text; ships on-image at `/FONT-LICENSES` |
| zlib 1.3.1 (Jean-loup Gailly, Mark Adler) | `userland/ports/zlib` holds the mports recipe only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | zlib licence | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| PCRE2 10.45 (Philip Hazel; University of Cambridge) | `userland/ports/pcre2` holds the mports recipe and one patch only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | BSD 3-clause **with the PCRE2 binary-package exemption** | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |

**TinyGL's acknowledgment clause is stricter than the standard zlib license.**
`userland/libgl/src/LICENSE` reads: "If you use this software in a product, an
acknowledgment in the product and its documentation *is* required" (emphasis
in the original) - not the usual zlib "would be appreciated but is not
required" wording. This file (documentation) now satisfies the documentation
half. **The product half does not yet exist**: there is no in-product
About/Credits screen anywhere in the shipped OS. Checked 2026-08-12 (#745 task
#57) by grepping every app for an "about"/"credits" screen; none exists. Not
fixed by this task: the natural home for it is a Settings "About" panel, and
`userland/apps/settings/main.c` was under active concurrent work at the time.
Flagged here as an open compliance item for a follow-up ticket, not silently
closed.

**Duktape's core engine (`duktape.c`/`duk_config.h`) is not tracked in this
git repository at all.** `duktape/build.sh` reads it from a host-local path
(`<workspace>`) at build time; only the MayteraOS-authored glue
(`userland/apps/browser/port/duktape/duk_dom.c`, `duk_support.c`) is tracked
in-tree. Duktape itself is MIT-licensed upstream
(https://duktape.org, Sami Vaarala and contributors) and MIT permits
binary-only redistribution without republishing source, so this is a
**source-completeness gap, not by itself a license violation**, as long as
this attribution entry (which does ship in the public repo) travels with any
distributed binary that links the compiled engine. Found 2026-08-12 (#745,
local queue item 57) while building the per-component license table; not fixed here,
since moving ~750KB of vendored upstream source into git is a build-topology
change outside this task's docs/license scope.

> **Scope note (2026-08-14): the next two sections describe components that are
> NOT in this repository.** `userland/apps/vi`, `userland/apps/grep-gnu` and
> `userland/apps/curaslice` are absent from the public source subset (verified
> with `find`; see the exclusion table at the top of this file). The analysis is
> kept because it is the whole-project record and because the obligations are
> real wherever those binaries are actually distributed. It is **not** a claim
> that this repository ships `/APPS/VI`, `/APPS/GREP` or `/APPS/CURASLIC`, and
> nothing you obtain from this repository carries the GPLv3 or AGPLv3
> obligations described below.

**The `VI` binary must be offered as GPLv3, not GPLv2. Stating it, not leaving
it to be derived.** `/APPS/VI` links busybox `vi.c` (GPLv2-**or-later**) and
the GNU regex copy at `userland/apps/vi/vendor/gnuregex` (LGPLv3-or-later) into
ONE executable. LGPLv3 is not compatible with GPLv2, so that combination is
lawful only by exercising busybox's "or later" option and distributing the
result under GPLv3. That election is a decision, not a footnote, and it must be
written down: relying on an "or later" posture without ever naming the version
you elected is the thing that is not a plan. Anyone redistributing the VI binary
therefore takes on GPLv3 obligations for it, including the corresponding-source
obligation for both components. The same reasoning applies to the OS's own
GPLv2-or-later code the moment it is combined with LGPLv3 material.

**This is a NOTICE analysis based on reading licence headers and the link
graph, not legal advice.** `grep` and `vi` are standalone binaries: neither is
linked into the kernel nor into each other, and the libc they link is MIT and
imposes nothing. Shipping them alongside the GPLv2-or-later OS is aggregation
of separately-licensed programs on one medium, not a combined work. An earlier
framing that called the LGPLv3 regex a GPLv2 incompatibility requiring
replacement with PCRE2 overstated it: PCRE2 remains a worthwhile simplification,
it is not a compliance necessity.

**DOOM moved and this file did not notice, corrected 2026-08-13 (#745, local
queue item 87).** The table used to point DOOM at `kernel/games/doom` and cite
`kernel/games/doom/DOOMLICENSE.md`. Neither exists: the in-kernel DOOM was
deleted in #703 (commit 748bbc7), `kernel/games` has zero tracked files, and no
file named `DOOMLICENSE*` is tracked anywhere in the repository. The live copy
is `userland/apps/doom` (134 tracked files), it builds `DOOM.ELF`, and
`build/build-golden.sh` installs it at `/GAMES/DOOM/DOOM.ELF`. Every one of
those files carries id Software's own header ("This source is available for
distribution and/or modification only under the terms of the DOOM Source Code
License"), which is the retained-notice half of the obligation.

**That owner action is now discharged, and the licence conclusion above was
wrong (2026-08-14).** The action item recorded here was to drop the authentic
`DOOMLICENSE` text into `userland/apps/doom/`. Doing it revealed that the
premise was mistaken. id Software's own upstream release
(`github.com/id-Software/DOOM`) ships its licence document as `LICENSE.TXT`,
and that document is the **GNU GPL v2**, not the 1997 DOOM Source Code License.
id relicensed the DOOM source under GPLv2 in 1999 and never went back to
rewrite the per-file banners, which is why the headers and the licence document
disagree upstream. Reading the headers and not the document is what produced
the earlier "NOT the GPL" entry.

`userland/apps/doom/LICENSE.TXT` now holds that document, fetched verbatim and
committed unmodified (git blob sha1 `d60c31a97a544b53039088d14fe9114583c0efc3`,
sha256 `32b1062f7da84967e7019d01ab805935caa7ab7321a7ced0e30ebe75e5df1670`,
17992 bytes). `userland/apps/doom/LICENSING.md` records the provenance, the
verification command, and the header discrepancy so the next reader does not
repeat the mistake. DOOM is therefore GPLv2, the same licence as MayteraOS
itself, and no compatibility question arises.

**No game data is tracked and none may be added.** The port is 63 `.c`, 70
`.h`, one `Makefile`, `LICENSE.TXT` and `LICENSING.md`. Users supply their own
WAD through `DOOM_WAD=` in `stage-disk.sh`.

**The CA bundle's "in-file note" was checked and is not there (2026-08-13,
local queue item 87).** This table and `docs/LICENSES.md` both recorded
`kernel/fs/CERTS/ca-bundle.crt` as MPL 2.0 with the licence recorded in an
in-file note. The file's entire header is three comment lines: a title, "Contains
major trusted root certificates for HTTPS connections", and a date. It names no
upstream, no version and no licence. So the MPL 2.0 claim cannot be checked
against the artifact, and the file does not identify where its certificates came
from. MPL 2.0 asks that recipients be given the licence text or a URI for it,
and neither travels with this file today. **Owner decision needed:** either
re-derive the bundle from a named Mozilla release and stamp the provenance and
licence URI into its header, or correct this row to whatever the real provenance
is. This task did not edit the file, because changing bytes in a trust anchor is
a security change and not a documentation one.

**mports ports keep their upstream OUT of this repository, so this document is
the only notice that exists for them.** A port (`userland/ports/<name>/`) tracks
a `PORT` manifest and a patch series; `userland/ports/mports.sh` fetches the
upstream tarball named in the manifest and refuses to build unless its sha256
equals the pin. Nothing under `userland/ports/` therefore carries the upstream
licence text, and the COVERAGE scan described below, which finds third-party
code by reading tracked files for a licence grant, cannot see a port at all.
That gap is closed by a third check in
`tools/license-audit/vendor-attribution-check.sh` (its PORTS check) which reads
the recipes as data and fails the build if a port has no `components.tsv` row,
or if the recipe's licence and the row's licence disagree. It also refuses
CC BY-SA outright, as does the driver, because a licence banned by policy should
not depend on somebody running a build to be caught.

Because the upstream text is not in the tree, it is reproduced here in full.

**zlib 1.3.1** (`userland/ports/zlib`), from the upstream `LICENSE` file of
`zlib-1.3.1.tar.gz`, sha256
`9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23`:

```
Copyright notice:

 (C) 1995-2022 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu
```

Restriction 2 ("altered source versions must be plainly marked") is why the
mports design keeps upstream unmodified and expresses every MayteraOS delta as
a patch file under `userland/ports/<name>/patches/`, listed by name in the
manifest. The zlib recipe currently carries NO patches: the 1.3.1 sources
compile unmodified against this userland's freestanding toolchain, so what
ships is unaltered upstream. Restriction 1's acknowledgment is "appreciated but
not required", and this entry provides it.

**PCRE2 10.45** (`userland/ports/pcre2`), from the upstream `LICENCE.md` file of
`pcre2-10.45.tar.gz`, sha256
`0e138387df7835d7403b8351e2226c1377da804e0737db0e071b48f07c9d12ee`, reproduced
in full and verbatim:

```
PCRE2 License
=============

| SPDX-License-Identifier: | BSD-3-Clause WITH PCRE2-exception |
|---------|-------|

PCRE2 is a library of functions to support regular expressions whose syntax
and semantics are as close as possible to those of the Perl 5 language.

Releases 10.00 and above of PCRE2 are distributed under the terms of the "BSD"
licence, as specified below, with one exemption for certain binary
redistributions. The documentation for PCRE2, supplied in the "doc" directory,
is distributed under the same terms as the software itself. The data in the
testdata directory is not copyrighted and is in the public domain.

The basic library functions are written in C and are freestanding. Also
included in the distribution is a just-in-time compiler that can be used to
optimize pattern matching. This is an optional feature that can be omitted when
the library is built.


COPYRIGHT
---------

### The basic library functions

    Written by:       Philip Hazel
    Email local part: Philip.Hazel
    Email domain:     gmail.com

    Retired from University of Cambridge Computing Service,
    Cambridge, England.

    Copyright (c) 1997-2007 University of Cambridge
    Copyright (c) 2007-2024 Philip Hazel
    All rights reserved.

### PCRE2 Just-In-Time compilation support

    Written by:       Zoltan Herczeg
    Email local part: hzmester
    Email domain:     freemail.hu

    Copyright (c) 2010-2024 Zoltan Herczeg
    All rights reserved.

### Stack-less Just-In-Time compiler

    Written by:       Zoltan Herczeg
    Email local part: hzmester
    Email domain:     freemail.hu

    Copyright (c) 2009-2024 Zoltan Herczeg
    All rights reserved.

### All other contributions

Many other contributors have participated in the authorship of PCRE2. As PCRE2
has never required a Contributor Licensing Agreement, or other copyright
assignment agreement, all contributions have copyright retained by each
original contributor or their employer.


THE "BSD" LICENCE
-----------------

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notices,
  this list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright
  notices, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

* Neither the name of the University of Cambridge nor the names of any
  contributors may be used to endorse or promote products derived from this
  software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.


EXEMPTION FOR BINARY LIBRARY-LIKE PACKAGES
------------------------------------------

The second condition in the BSD licence (covering binary redistributions) does
not apply all the way down a chain of software. If binary package A includes
PCRE2, it must respect the condition, but if package B is software that
includes package A, the condition is not imposed on package B unless it uses
PCRE2 independently.

End
```

Three notes on the PCRE2 entry specifically.

**The exemption RELAXES an obligation; it never adds one.** It says the binary
condition does not propagate down a chain of packages. We do not rely on it:
this document reproduces the copyright notices, the conditions and the
disclaimer in full, which discharges plain BSD-3-Clause, and plain
BSD-3-Clause is strictly the harder of the two. The `components.tsv` row and
the recipe both say `BSD-3-Clause-WITH-PCRE2-exception` so that the exemption
is recorded rather than quietly dropped, and the two strings must match
exactly or the attribution gate fails.

**The JIT copyrights are reproduced even though the JIT is not built.** PCRE2's
licence text carries separate copyright blocks for Zoltan Herczeg's JIT and for
sljit. `userland/ports/pcre2` sets `SUPPORT_JIT` nowhere, and
`src/pcre2_jit_compile.c` therefore compiles to stubs with no code generator
(see the reasoning in `patches/0001-config-h-maytera-settings.patch`), so no
sljit code is in the shipped archive at all. The blocks stay because they are
part of the verbatim text, and editing a licence to match what you happen to
ship is exactly the kind of paraphrase this document refuses to make.

**Our delta is one patch, and it is not a code change.**
`userland/ports/pcre2/patches/0001-config-h-maytera-settings.patch` appends a
settings block to the `src/config.h` that upstream's own
`NON-AUTOTOOLS-BUILD` procedure tells you to create from `config.h.generic`.
No PCRE2 source file is modified. That satisfies "altered source versions must
be plainly marked" in spirit as well as letter: the alteration is one
reviewable hunk in a file upstream expects the packager to write.

### How this table is kept honest

`tools/license-audit/vendor-attribution-check.sh` fails when this file and the
tree disagree. It reads `tools/license-audit/components.tsv` (one row per
vendored copy) and enforces two things a reader cannot:

1. **Row integrity.** Every declared path must still exist, and must be named
   literally in this file. A component that is moved or deleted turns the check
   red instead of leaving a row pointing at nothing, which is exactly what the
   DOOM row did for months.
2. **Coverage.** Every tracked TEXT file carrying a third-party licence grant
   must fall under some declared component. A newly vendored tree therefore
   cannot land silently; it arrives unclassified and the check goes red.

Run `--self-test` to see it go red on each failure shape and green once fixed.

**Its honest limits, so nobody over-reads it.** It reads text files, so licence
obligations carried inside BINARY assets (fonts, images, WADs, prebuilt blobs)
are invisible to it and remain hand-maintained in this file's asset sections. It
matches licence-grant wording, so third-party code vendored with no licence
header at all is invisible to it. It verifies that a path is NAMED here; it
cannot verify that what this file SAYS about that path is true. It is a
bookkeeping gate, not legal advice.

The AI layer's LLM prompt-injection protection uses the **Nova** open ruleset by
**Thomas Roccia** ([@fr0gger_](https://github.com/fr0gger/nova-framework)),
(c) 2025, MIT License. The keyword layer is adapted from Nova's
`llm01_promptinject`, `jailbreak` and `injection` rules; retain this credit if
you redistribute `kernel/security/nova.c`.

Because the kernel statically links GPLv2 components (libmad, faad2), the
combined MayteraOS KERNEL binary is distributed under **GPLv2-or-later**. The
permissively-licensed components above remain under their own terms as source.

That sentence covers the kernel and nothing else. **The ported userland
applications are separate executables and each carries its own licence**, which
is why the table above lists a licence per copy: `/APPS/VI` is GPLv3 (see
above), `/APPS/GREP` is GPLv3-or-later, `/APPS/CURASLIC` is AGPLv3,
`/GAMES/DOOM/DOOM.ELF` is GPLv2, and `/APPS/CLASSICUBE` is
BSD 3-clause. Shipping them on one image is aggregation, not a combined work;
none of them makes any other one copyleft, and none of them is covered by the
kernel's blanket.

**`userland/libc/` is the one first-party exception to the GPLv2-or-later
blanket.** It is deliberately **MIT**-licensed (`userland/libc/LICENSE`; every
`.c`, `.h`, `.asm` and `.S` under the directory carries an
`SPDX-License-Identifier: MIT` header, 114 files, added 2026-08-12 and extended
to the assembly sources 2026-08-13, #745 local queue item 57). The per-file
headers exist because a directory-level license file has repeatedly failed to
travel with code that gets copied out of its directory.

**What that means if you are porting or writing software for MayteraOS:**

> **Statically linking `userland/libc/libc.a` does NOT make your application
> GPL.** Your application keeps whatever license you give it, including
> Apache-2.0, BSD, MIT, GPLv3, or a proprietary license. MayteraOS's
> GPLv2-or-later terms cover MayteraOS's own kernel and OS code; they do not
> reach across the static link into an application whose connection to the OS
> is that it calls the C library. MIT's only obligation is to preserve the
> copyright and permission notice.

That exemption is the libc specifically, not a general exemption: linking some
other GPL component of ours, or vendoring a third-party GPL library into your
app, is governed by that component's own terms. See `docs/LICENSES.md` for the
full policy and `docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md` section 7.2
for the 2,666-package analysis that led to this decision.

The in-house decoders (`jpeg.c`, `png.c`, `webp.c`, `wav.c`, `mpeg.c`) and the
archiver (`userland/libarchive`) are original MayteraOS code.

> **DOOM (id Software):** the DOOM engine source under `userland/apps/doom`
> (the `d_*/i_*/r_*/p_*/w_*/z_*` files) is licensed under the **GNU GPL v2**,
> the same licence as MayteraOS itself. The authoritative text is
> `userland/apps/doom/LICENSE.TXT`, committed verbatim from id Software's own
> upstream release.
>
> **Do not conclude otherwise from the per-file headers.** Every engine file
> still carries a 1997 banner naming the *DOOM Source Code License*, because id
> relicensed the source under GPLv2 in 1999 and never rewrote the banners. The
> licence document, not the stale header, is controlling. An earlier revision of
> this paragraph read those headers, called DOOM "a separate license, not the
> GPL", and carved it out of this repository's blanket; that was wrong. See the
> DOOM correction above, and `userland/apps/doom/LICENSING.md`.
>
> No DOOM game data is in this repository. The port needs a WAD you supply.

## Freedoom (Maytera Arena character and weapon sprites)

`userland/apps/arena/assets/spr/*.BMP` (enemy/character sprites - zombie,
shotgun zombie, serpentipede skins) and `userland/apps/arena/assets/wpn/*.BMP`
(weapon viewmodel sprites) are extracted from the **Freedoom** project's
`freedoom2.wad`, release v0.13.0 (https://github.com/freedoom/freedoom),
converted from Doom picture format to BMP. See the header comments in
`userland/apps/arena/characters.c` and `userland/apps/arena/weapons_art.c`
for the exact Freedoom lump prefixes each set was extracted from.

License: **BSD 3-clause** ("Copyright 2001-2024 Contributors to the Freedoom
project. All rights reserved."). Full text: `FREEDOOM-COPYING.txt`, present
alongside both `assets/spr/` and `assets/wpn/` and at the `assets/` parent
directory (added 2026-08-12, #745, local queue item 57 - previously a copy existed only
next to `assets/wpn/`, even though `assets/spr/` is the larger Freedoom-
derived set with over 220 sprite files and had no license file of its own).

**This entry did not exist in `ATTRIBUTION.md` before 2026-08-12 (#745 task
#57), and `userland/apps/arena/CREDITS.TXT` did not mention Freedoom at all**
- it credited only the (correctly CC0, no-attribution-required) OpenGameArt
textures. Both are corrected as of this task; see `CHANGELOG.md`.

### ClassiCube

`/APPS/CLASSICUBE` is a port of **ClassiCube**, an open-source Minecraft Classic
compatible game engine written in C by **UnknownShadow200** and contributors
(https://github.com/UnknownShadow200/ClassiCube). The engine source is vendored
unmodified at pinned upstream commit
`4016a0918ba5c127d5203a4940e76b79b229d51f` under
`userland/apps/classicube/vendor/ClassiCube/`. MayteraOS supplies only the
platform backends (`Platform_`, `Window_`, `Http_`, `Socket_`, `Audio_Maytera.c`),
which are MayteraOS code and sit outside the vendored tree.

ClassiCube is licensed under the **modified (3-clause) BSD licence**:

> Copyright (c) 2014 - 2024, UnknownShadow200. All rights reserved.
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are met:
>
> 1. Redistributions of source code must retain the above copyright notice,
>    this list of conditions and the following disclaimer.
> 2. Redistributions in binary form must reproduce the above copyright notice,
>    this list of conditions and the following disclaimer in the documentation
>    and/or other materials provided with the distribution.
> 3. Neither the name of ClassiCube nor the names of its contributors may be
>    used to endorse or promote products derived from this software without
>    specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
> AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
> IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
> ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
> LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
> CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
> SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
> INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
> CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
> ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
> POSSIBILITY OF SUCH DAMAGE.

Condition 2 is why this entry exists: a golden image carrying
`/APPS/CLASSICUBE` is a binary redistribution, and the notice above is the
"documentation or other materials provided with the distribution".

Condition 3 is a NAMING restriction, not a redistribution restriction. The
Start-menu entry may say "ClassiCube" as a factual identification of what the
program is; MayteraOS marketing must not imply that ClassiCube or its authors
endorse MayteraOS.

**Do not split up `vendor/ClassiCube/license.txt`.** Beyond the BSD text above
it aggregates upstream's own third-party notices, which travel with the engine
and are covered by the same obligation: the **OpenTK** MIT licence (and the
Mono class-library portions it carries), the Emscripten licence, the **BearSSL**
licence, and the public-domain / unlicensed notices covering the ray-box
intersection, voxel-traversal and frustum-culling algorithms. The file is
vendored byte-identical to upstream (md5 `a7c4a780e01e1bfa1883428c39f8dda4`);
`userland/apps/classicube/fetch-upstream.sh verify` fails if any vendored byte,
that file included, stops matching the pinned commit.

## Photography (boot splash / wallpapers)

The boot-splash background (`kernel/boot.bmp`, `kernel/boot_splash.jpg`,
`kernel/video/boot_image_data.c`) and the bundled wallpapers are edited from
**Pexels** stock photography, used under the [Pexels License](https://www.pexels.com/license/)
(free for commercial and non-commercial use, no attribution required). The
MayteraOS lighthouse mark composited onto the splash is original artwork.

If you redistribute any component, retain its in-tree license file and this
attribution.
