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
| DOOM (id Software) | `userland/apps/doom` | Distributed through the MayteraOS App Store rather than as repository source. The per-file id Software notices are intact in the source; the full upstream licence DOCUMENT is not yet tracked here, and it will be fetched from the authentic upstream release rather than retyped from memory. |
| AssaultCube | `userland/apps/assaultcube` | App Store distribution. Its content licence forbids redistribution and its `vendor/` engine tree is excluded. |
| OpenArena | `userland/apps/openarena` | App Store distribution. Its `vendor/` tree carries lcc, which is explicitly not free. |
| GNU grep 2.5.4 | `userland/apps/grep-gnu` | GPLv3-or-later, and no GPLv3 licence document is tracked here yet. |
| vi (busybox) | `userland/apps/vi` | GPLv2-or-later; no GPLv2 licence document is tracked here yet. (Until #745 local 97 this row also said "combined with LGPLv3-or-later GNU regex". That copy has been deleted; the engine is now the MIT mports port at `userland/ports/musl-regex`.) |
| CuraEngine slicer | `userland/apps/curaslice` | AGPLv3, plus a Boost-licensed Clipper, with no licence documents tracked here yet. |
| Every `userland/apps/*/vendor` tree | various | Vendored upstream engine source is not republished. Where such a tree carries a notice this project owes, the notice text is carried in THIS file instead, which is why this file is itself published. ClassiCube is the worked example: its BSD-3 text is below, not in the excluded `vendor/` directory. |

These are EXCLUSIONS, not a claim that the obligations vanish. An obligation is
discharged where the component is actually distributed. Anything in this list
that later gains its authentic upstream licence text in-tree should be
re-included in the published subset rather than left out permanently.

## DOS-guest media on the internal asset base (2026-08-25, #234f/#234g/#234h)

**None of this is in git, none of it reaches the public repository, and none of
it is ours.** It is recorded here because until this entry existed the file said
nothing at all about the sixteen third-party DOS programs the internal golden has
been shipping, and an inventory that is silent about a whole class of payload is
not an inventory. The precedent that made the silence look acceptable was itself
the bug: this file's own opening paragraph records a row that pointed at
`kernel/games/doom` for months after the code moved, and the lesson taken from it
was "say where things actually are", not "say nothing about the things that are
awkward to place".

### Where these live, and why the path is not a repository path

The binaries live ONLY on the internal static asset base,
`maytera-2part-ext2-b850.img` on the build host, under `/DOS/<NAME>` and
`/GAMES/<NAME>` on its ext2 root. That image is deliberately outside git
(`build/build-golden.sh` overlays freshly-compiled binaries onto it; it is never
committed), which is exactly why `tools/license-audit/vendor-attribution-check.sh`
cannot see any of this: that linter reads tracked TEXT files, and none of these
are tracked or text. **This section is therefore hand-maintained and
unenforced.** Treat that as a known weakness of the check, not as permission to
let it drift.

`tools/release-gate/assemble-publish-tree.sh` publishes SOURCE, and there is no
source here to exclude; the operative protection is that the golden image itself
is never published, for the credential reasons recorded in `CLAUDE.md` and
`blame.md`. A commercial game on an internal-only image is the same category of
decision as the live credentials that image carries by design.

### Added 2026-08-25

| Item | What it is | Where it came from | Licence status |
|---|---|---|---|
| SimCity Classic (1994, MS-DOS) | Maxis city-building simulation. Plain 16-bit real-mode MZ; runs on the `x86_16.c` interpreter. Installed tree at `/DOS/SIMCITY`. | `archive.org/download/msdos_SimCity_Classic_1994` (Internet Archive MS-DOS software collection), `SimCity_Classic_1994.zip`, 5,972,434 bytes. | **Commercial, copyright Maxis Software (now Electronic Arts). NOT freeware, NOT abandonware in any legal sense.** Distributed by the Internet Archive under its software-preservation programme. No redistribution right is claimed or implied. Internal image only. |
| PYRO! 2.01 for DOS (1992) | Fifth Generation Systems screen-saver suite, a DOS TSR. Shipped as a 3.5" 720 KB floppy IMAGE at `/IMAGES/PYRO.IMG`, not as an installed tree; it is mounted on A: by `/APPS/DISKIMG`. | `winworldpc.com/download/a1ce9c95-ee4a-11e7-a562-fa163e9022f0` (WinWorld). 7-Zip archive, 1,282,765 bytes, SHA1 `f861e0debe77776c848325ec698876f3af821300`, verified against the checksum WinWorld publishes on the download page. | **Commercial, copyright Fifth Generation Systems, Inc. (defunct; assets passed to Symantec).** Preserved and distributed by WinWorld as abandonware. No redistribution right is claimed or implied. Internal image only. |
| SimCity Classic install floppy 1 | The vendor's own 720 KB install disk from the same archive, kept at `/IMAGES/SCINST1.IMA` purely as a SECOND floppy-image test fixture, so the floppy path is proven on more than one disk. | Same zip as SimCity Classic above, `SimCityC/floppy/simcityc_dos_35a.IMA`. | As SimCity Classic above. |
| The Dig (1995, LucasArts) | SCUMM adventure, DOS/4GW protected-mode. **Deliberately NOT on the asset base**, see below. | `archive.org/download/DigTheUSARerelease`, `Dig, The (USA) (Rerelease).zip`, 622,278,884 bytes, containing a single-track `MODE1/2352` BIN plus a CUE. | **Commercial, copyright LucasArts (now Disney).** Supplied by the project owner from his own copy. Internal use only; not on any shipped image. |

### The Dig is not shipped, and that is a deliberate decision

It was converted to a plain 2048-byte-per-sector ISO (681,037,824 bytes, volume
identifier `THEDIG`) because the mount path accepts ISO 9660 only, and used to
exercise the CD-ROM path on a throwaway test image. It was **not** added to the
asset base: at 650 MB it is larger than everything else on this list combined, it
would be copied on every golden build for ever, and putting a commercial CD image
into the shared asset base is a payload decision for the project owner to take
explicitly rather than a side effect of a test. The conversion recipe is in
`CHANGELOG.md`; the image is not in the repo.

### Already shipping, previously unrecorded

For completeness, and so this section is a real inventory rather than a record of
one day's additions. All are third-party, all commercial or shareware, all
internal-image-only, none in git. Recorded from the asset base listing rather
than from memory:

`/DOS`: ALADDIN, JOUST, KEEN5, KEEN7, MONKEY, NETHACK, ROGUE, SKYROADS, STUNTS,
TIM. `/GAMES`: BATS, KEEN4, KEEN6, PRINCE.

NetHack is the one item in that list that is genuinely free software (NetHack
General Public Licence); it is a DJGPP build and its status differs from its
neighbours. The rest are commercial or shareware releases preserved by third
parties, and carry no redistribution right. DOOM under `/GAMES/DOOM` is covered
by its own row in this file and is a userland ELF port, not a DOS guest.

## Office app icons (Writer, Sheets, Slides, suite mark): original MayteraOS artwork (2026-09-07)

`WRITER.ICN`, `SHEETS.ICN`, `SLIDES.ICN` and `OFFICE.ICN` (masters in
`assets/office-icons/apps/*.svg`, symlinked from `assets/icons/svg/`) are
original MayteraOS artwork drawn for this project: a white sheet outline at
the same 4px-on-64 weight as the CoreUI launcher set, the Maytera mountain-M
peaks as the shared suite cue, and per-app colour on the content only
(Writer blue, Sheets green, Slides orange). No CoreUI, Boxicons or SVG Repo
path data was used or adapted, so no third-party attribution applies; they
are covered by this repository's own licence like every other bespoke icon
listed here (`ARENA.svg`, `CHESS.svg`, `SQUADRON.svg`, `recycle.svg`).

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
| Audio Player | `APLAYER.ICN` | `cil-music-note.svg` (MIRRORED horizontally, #123 - see note below) |
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
| Wizard power corner: Shut Down | `POWER.ICN` | `cil-power-standby.svg` |
| Wizard power corner: Restart | `RESTART.ICN` | `cil-reload.svg` |

If any icon above is republished, retain attribution to CoreUI
(https://coreui.io/icons/) and the CC BY 4.0 license linked above.

**Modification note (#123, 2026-08-14).** `APLAYER.ICN` is CoreUI's
`cil-music-note` MIRRORED HORIZONTALLY, at the user's request: the upstream
glyph puts the note head at the bottom right with the flag reaching up and to
the left, which is the reverse of the conventional quaver every other music
glyph on this desktop uses. CC BY 4.0 permits modification provided the
change is indicated, which is what this paragraph does. No other geometry was
altered: `assets/icons/svg/APLAYER.svg` still carries CoreUI's path data
verbatim inside a wrapping `<g transform="translate(512,0) scale(-1,1)">`,
and the shipped raster was produced by mirroring the existing `.ICN` pixel
for pixel (`tools/icons/mirror_icn.py`), not by re-rasterizing.

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

### Original icons: CALC (spreadsheet) toolbar set, 2026-09-07 (officeiconscalc)

`assets/office-icons/calc/svg/*.svg` (23 masters) and the 24x24 MICO
rasters under `assets/office-icons/calc/icn/` (`SCNEW.ICN` ... `SCSORTD.ICN`)
are **original MayteraOS artwork**: hand-authored stroked paths on a 24px
grid, no CoreUI/Boxicons/SVG Repo paths, and no font glyph outlines (the
B, I, U, $, %, sigma, fx and # glyphs are drawn as strokes, not extracted
from a typeface). Nothing to credit beyond this repository. See that
directory's `README.md` for the mechanism they target.
### Office suite toolbar icons: original artwork (2026-09-07, officeiconsslides)

The twenty SLIDES toolbar glyphs under `assets/office-icons/slides/`
(`OFNEW`, `OFOPEN`, `OFSAVE`, `SLNEW`, `SLDUP`, `SLDEL`, `SLLAYOUT`,
`OFTEXTBX`, `OFIMAGE`, `OFSHAPE`, `OFBOLD`, `OFITALIC`, `OFALIGNL`,
`OFALIGNC`, `OFALIGNR`, `OFBULLET`, `SLPREV`, `SLNEXT`, `SLPLAY`,
`SLNOTES`, all `.ICN` in `/ICONS/`) are original MayteraOS artwork drawn
from scratch on a 32-unit grid (`gen_icons.py` in that directory is the
geometry source). No third-party path data was copied or traced; they only
share the CoreUI set's 32/512 line weight so the two families sit together
in one toolbar. Nothing to attribute; nothing that restricts republishing.

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
| CPython 3.11.9 (the shipping Python interpreter, `/APPS/PYTHON.ELF`) | `userland/apps/python/port` holds MayteraOS port glue only (MIT); CPython 3.11.9 itself is fetched at build time, not vendored in this repo | Python Software Foundation License (PSF), upstream python.org | upstream (python.org); the port-glue files carry MIT SPDX headers |
| Duktape (JS engine core) | `userland/apps/browser/port/duktape` holds MayteraOS glue only; the engine is fetched, not tracked (see note below) | MIT (upstream) | not tracked |
| NetSurf core libs (libwapcaplet, libparserutils, libhubbub, libcss, libdom) | `userland/apps/browser/port/netsurf` holds MayteraOS glue only; the libraries are fetched at pinned revisions, not tracked (see note below) | MIT (upstream NetSurf core libs) | not tracked |
| DOOM (id Software) | `userland/apps/doom` | id Software DOOM Source Code License (NOT the GPL) | per-file headers only; **no licence document is in the tree, see the DOOM note below** |
| Rogue 5.4.4 (Michael Toy, Ken Arnold, Glenn Wichman) | `userland/apps/rogue` | BSD 3-clause | `LICENSE.TXT` |
| GNU grep (installs as `/APPS/GREP`, the only grep on the image since #745 local 98) | `userland/apps/grep-gnu/src` | GPLv3-or-later | none tracked |
| gnulib support files for grep | `userland/apps/grep-gnu/lib` | GPLv3-or-later | per-file headers |
| gnulib `obstack` **(LGPL that is still shipped)** | `userland/apps/grep-gnu/lib/obstack.c`, `userland/apps/grep-gnu/lib/obstack.h` | LGPLv3-or-later (upstream header says "GNU Library General Public License ... version 3", gnulib's mechanical version bump of the old LGPL name) | per-file headers |
| gnulib `error()` **(LGPL that is still shipped)** | `userland/apps/grep-gnu/lib/error.c` | LGPLv3-or-later, same wording as obstack | per-file headers |
| busybox `vi.c` 1.36.1 | `userland/apps/vi/vendor/busybox` | GPLv2-or-later | none tracked; `userland/apps/vi/PROVENANCE.md` pins the upstream tarball and md5 |
| CuraEngine | `userland/apps/curaslice/src` | AGPLv3 | `userland/apps/curaslice/LICENSE.curaengine`, which sits at the app root rather than beside the code it covers |
| Clipper (Angus Johnson) | `userland/apps/curaslice/libs/clipper` | Boost Software License 1.0 | none tracked; the file header cites the Boost licence by URL |
| ClassiCube engine | `userland/apps/classicube/vendor/ClassiCube` | modified BSD 3-clause | `license.txt` |
| FreeType, bundled *inside* ClassiCube | `userland/apps/classicube/vendor/ClassiCube/src/freetype` | FreeType Project License, or GPLv2, at your option | reproduced inside ClassiCube's own `license.txt` |
| AssaultCube / Cube engine (Wouter van Oortmerssen and the AssaultCube team) | `userland/apps/assaultcube/vendor/AC` | zlib-like Cube licence (zlib plus an extra clause) | `source/README_CUBEENGINE.txt` |
| ENet (Lee Salzman) | `userland/apps/assaultcube/vendor/AC/source/enet` | MIT | `LICENSE` beside the code, plus a second copy of the same text at `userland/apps/assaultcube/vendor/enet_LICENSE.txt` |
| SDL2, zlib, libogg, libvorbis, OpenAL and GL headers bundled by AssaultCube | `userland/apps/assaultcube/vendor/AC/source/include` | zlib (SDL2, zlib) and BSD-style (Xiph) per file | per-file headers + `SDL_copying.h` + `README_jpeg.txt` |
| OpenArena / ioquake3 engine | `userland/apps/openarena/vendor/OA` | GPLv2 | `source/COPYING.txt` |
| libjpeg 8c (Independent JPEG Group) bundled by ioquake3 | `userland/apps/openarena/vendor/OA/source/code/jpeg-8c` | IJG licence | `README` |
| lcc retargetable C compiler bundled by ioquake3 | `userland/apps/openarena/vendor/OA/source/code/tools/lcc` | **NOT free**: "you may not sell lcc or any product derived from it" | `COPYRIGHT` |
| Freedoom-derived sprite and weapon art | `userland/apps/arena/assets` | BSD 3-clause | `FREEDOOM-COPYING.txt` (three copies, one per asset directory) |
| Reproduced licence texts for the 14 shipped font families | `build/font-licenses` | OFL 1.1, Bitstream Vera, public domain, per family | the directory IS the licence text; ships on-image at `/FONT-LICENSES` |
| zlib 1.3.1 (Jean-loup Gailly, Mark Adler) | `userland/ports/zlib` holds the mports recipe only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | zlib licence | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| PCRE2 10.45 (Philip Hazel; University of Cambridge) | `userland/ports/pcre2` holds the mports recipe and one patch only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | BSD 3-clause **with the PCRE2 binary-package exemption** | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| Lua 5.4.8 (Lua.org, PUC-Rio) | `userland/ports/lua` holds the mports recipe and one patch only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | MIT | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| musl 1.2.5 POSIX regex, TRE-derived (Rich Felker; Ville Laurikari) | `userland/ports/musl-regex` holds the mports recipe and five patches only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | MIT (musl), with the regex files additionally carrying TRE's 2-clause BSD notice | not tracked; **both texts reproduced in the mports note below**, which is the only place they exist in this repository |
| ncurses 6.5 (Thomas E. Dickey; Free Software Foundation) | `userland/ports/ncurses` holds the mports recipe, one patch, and the hand-written `terminfo/maytera.terminfo` only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | X11 (MIT-style permissive) | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| SQLite 3.53.4 (D. Richard Hipp) | `userland/ports/sqlite` holds the mports recipe, our own `maytera_vfs.c`, and `build.sh` only; the upstream amalgamation is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | blessing (public domain; SQLite disclaimer) | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| jq 1.8.1 (Stephen Dolan and jq contributors) with bundled Oniguruma 6.9.10 (K. Kosako) and IBM decNumber | `userland/ports/jq` holds the mports recipe and `build.sh` only; the upstream tarball (which itself bundles Oniguruma and decNumber) is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | MIT (jq), BSD 2-clause (Oniguruma), ICU licence (decNumber) | not tracked; **full texts reproduced in the mports note below**, which is the only place they exist in this repository |
| cJSON 1.7.18 (Dave Gamble and cJSON contributors) | `userland/ports/cjson` holds the mports recipe only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | MIT | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| Expat 2.7.1 (Thai Open Source Software Center, Clark Cooper, Expat maintainers) | `userland/ports/expat` holds the mports recipe and one patch only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | MIT | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| libyaml 0.2.5 (Ingy döt Net; Kirill Simonov) | `userland/ports/libyaml` holds the mports recipe and one patch only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | MIT | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| LZ4 1.10.0, library only (Yann Collet) | `userland/ports/lz4` holds the mports recipe only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below). The library (lib/) is BSD-2-Clause; the GPLv2 programs are not built | BSD 2-clause (library) | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| Zstandard (zstd) 1.5.6, library only (Meta Platforms, Inc.) | `userland/ports/zstd` holds the mports recipe only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below). zstd is dual BSD-3 / GPLv2; this port takes the BSD grant and builds only the library | BSD 3-clause (chosen from the dual grant) | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| MD4C 0.5.2 (Martin Mitáš) | `userland/ports/md4c` holds the mports recipe only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | MIT | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| libpng 1.6.44 (Cosmin Truta; Glenn Randers-Pehrson; the PNG Reference Library Authors) | `userland/ports/libpng` holds the mports recipe only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below). needs=zlib (also ported) | libpng-2.0 (PNG Reference Library License v2) | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| libarchive 3.8.9 (Tim Kientzle and contributors) | `userland/ports/libarchive` holds the mports recipe and one patch only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | BSD-2-Clause (with documented per-file exceptions: a 3-clause file, a public-domain file, and a triple-licensed pair, all noted below) | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| darkhttpd 1.16 (Emil Mikulic) | `userland/ports/darkhttpd` holds the mports recipe and one patch only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | ISC | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| libedit 20260512-3.1 (portable editline; Christos Zoulas; NetBSD; Regents of the University of California) | `userland/ports/libedit` holds the mports recipe and one patch only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | BSD-3-Clause | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| Angband 4.2.6 (Angband contributors) | `userland/ports/angband` holds the mports recipe, one patch, and one first-party MayteraOS glue file (`src/mayteraos-curses-compat.c`, added by the patch) only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | GPL-2.0-only | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| myman 0.7.0 (Benjamin C. Wiley Sittler) | `userland/ports/myman` holds the mports recipe, one patch and the `install_sources` list only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | MIT | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| ninvaders 0.1.1 (Dettus) | `userland/ports/ninvaders` holds the mports recipe and three patches only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | GPL-2.0-or-later | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| moon-buggy 1.0.51 (Jochen Voss) | `userland/ports/moonbuggy` holds the mports recipe and three patches only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | GPL-2.0-or-later | not tracked; the GPLv2 text is the same one reproduced verbatim for ninvaders in the mports note below |
| QuickJS-ng 0.16.2 (Fabrice Bellard; Charlie Gordon; Ben Noordhuis; Saul Ibarra Corretge) | `userland/ports/quickjs` holds the mports recipe, one patch, our own `maytera_compat.c`, and `build.sh` only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | MIT | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| bc-gh 7.0.3 (Gavin D. Howard and contributors) | `userland/ports/bc` holds the mports recipe, one patch, and `build.sh` only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | BSD-2-Clause | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| TinyCC 0.9.27 (Fabrice Bellard) | `userland/ports/tcc` holds the mports recipe, one patch (`tcc-native-guard.patch`), our own `maytera_compat.c`, and `build.sh` only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked | LGPL-2.1-or-later | not tracked; the LGPL-2.1 grant is `COPYING` inside the pinned `tcc-0.9.27.tar.gz`, verified present by the mports licence-files check at build time |
| libsodium 1.0.20 (Frank Denis) | `userland/ports/libsodium` holds the mports recipe, our own `randombytes_maytera.c` (the RNG backend), and `build.sh` only; the upstream tarball is fetched at build time against a sha256 pin and is not tracked (see the mports note below) | ISC | not tracked; **full text reproduced in the mports note below**, which is the only place it exists in this repository |
| XaoS 3.6 (Jan Hubicka; Thomas Marsh; the XaoS project) | `userland/apps/xaos/src` holds the vendored upstream source (engine, filter, ui, ui-hlp, util, include; release-3.6 tag), tracked in git like DOOM rather than fetched at build time; the only new code is `userland/apps/xaos/src/ui/ui-drv/maytera/ui_maytera.c`, `userland/apps/xaos/xaos_compat.c`, and a one-line `#ifndef USE_CLOCK` patch noted in-file in `userland/apps/xaos/src/util/timers.c` | GPL-2.0-or-later | `userland/apps/xaos/COPYING`, copied verbatim from the release-3.6 tarball |

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

**The `VI` binary no longer has to be offered as GPLv3 (#745 local 97), and
`GREP` still carries LGPLv3 for a DIFFERENT reason. Both halves stated, because
only one of them changed.**

This paragraph used to say that `/APPS/VI` must be distributed under GPLv3,
because it linked busybox `vi.c` (GPLv2-**or-later**) together with the GNU
regex copy at `userland/apps/vi/vendor/gnuregex` (LGPLv3-or-later), and LGPLv3
is not compatible with GPLv2, so the combination was lawful only by exercising
busybox's "or later" option. That election is no longer forced: the GNU regex
copy is deleted and the engine is now `userland/ports/musl-regex`, which is
MIT and imposes nothing. MEASURED on the built binary rather than inferred:
`/APPS/VI` contains none of the twelve GNU-regex-only error strings, and its
link line is busybox `vi.c` + MayteraOS compat code + `libregex.a` + `libc.a`.
VI is therefore GPLv2-or-later, and the "or later" option stays an option.

`/APPS/GREP` is a different case and the honest answer is that LGPLv3 has NOT
left it. GNU regex is gone from it too, but two gnulib files it links,
`lib/obstack.c` (used by `src/kwset.c`, the fixed-string matcher) and
`lib/error.c`, carry the LGPL themselves. That was never a compatibility
problem, because grep 2.5.4 as vendored here is GPLv3-or-later and LGPLv3
combines with GPLv3 freely; it is a notice obligation, and it is recorded in
the table above with its own rows rather than folded into the directory row.
Retiring it is a separate, smaller piece of work than this one was.

The general rule stands: the OS's own GPLv2-or-later code would face the same
election the moment it is combined with LGPLv3 material.

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
License"), which is the retained-notice half of the obligation. **The full
licence document is missing from the tree and this task did not write one**: a
licence text must be reproduced from the authentic upstream document, never
retyped from memory. **Owner action needed** before the next public push: drop
the authentic `DOOMLICENSE` text into `userland/apps/doom/`.

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

**ncurses 6.5** (`userland/ports/ncurses`), from `COPYING` of `ncurses-6.5.tar.gz`, sha256 `136d91bc269a9a5785e5f9e980bc76ab57428f604ce3e5a5a90cebc767971cc6`, reproduced in full and verbatim. It is the X11-style (MIT-derived) permissive licence, compatible with the GPLv2 base:

```
Copyright 2018-2023,2024 Thomas E. Dickey
Copyright 1998-2017,2018 Free Software Foundation, Inc.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, distribute with modifications, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE ABOVE COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name(s) of the above copyright
holders shall not be used in advertising or otherwise to promote the
sale, use or other dealings in this Software without prior written
authorization.
```

The compiled terminfo database this port ships (`/TERMINFO` on the image) is built by `tic` from this same tarball's `misc/terminfo.src` (ansi, vt100, linux, xterm, xterm-256color) plus this repository's own `userland/ports/ncurses/terminfo/maytera.terminfo`; both are covered by the grant above and this repository's MIT licence respectively.

**Lua 5.4.8** (`userland/ports/lua`), from the end of the upstream `src/lua.h`
of `lua-5.4.8.tar.gz`, sha256
`4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae`, reproduced
in full and verbatim (the same text appears in the "License" section of the
tarball's `doc/readme.html`; the tarball ships no separate `LICENSE` file, which
is why the recipe's `licence_files` names those two paths):

```
Copyright (C) 1994-2025 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

Two notes on the Lua entry.

**MIT requires the notice above to be included in all copies, and this document
is where that happens.** The Lua code ships inside `/APPS/LUA` (the interpreter)
and inside any future app that links `liblua.a`; none of those binaries carries
the text, so this file is the notice for every one of them. The upstream project
additionally asks - as a request, not a condition - that products using Lua
credit it somewhere; this entry is that credit.

**Our delta is one patch, and it changes no Lua source file.**
`userland/ports/lua/patches/0001-luaconf-maytera-module-paths.patch` rewrites
the `LUA_ROOT` / `LUA_PATH_DEFAULT` / `LUA_CPATH_DEFAULT` block of
`src/luaconf.h`, which is the header upstream documents as the place to change
for a non-conventional directory hierarchy. `luac.c` and `lua.c` are excluded
from the library by the recipe; `lua.c` is compiled unmodified into
`/APPS/LUA` by `userland/apps/lua`.

### musl 1.2.5 (`userland/ports/musl-regex`)

Reproduced verbatim from `COPYRIGHT` in the sha256-pinned upstream tarball
(`musl-1.2.5.tar.gz`, sha256
`a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`, measured on
the build host). musl as a whole is MIT:

```
musl as a whole is licensed under the following standard MIT license:

----------------------------------------------------------------------
Copyright © 2005-2020 Rich Felker, et al.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
----------------------------------------------------------------------
```

The same file records the provenance of exactly the files this port builds:

```
The TRE regular expression implementation (src/regex/reg* and
src/regex/tre*) is Copyright © 2001-2008 Ville Laurikari and licensed
under a 2-clause BSD license (license text in the source files). The
included version has been heavily modified by Rich Felker in 2012, in
the interests of size, simplicity, and namespace cleanliness.
```

That in-source 2-clause BSD text, which heads `src/regex/regcomp.c`,
`regexec.c` and `tre.h`, is:

```
  Copyright (c) 2001-2009 Ville Laurikari <vl@iki.fi>
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS
  ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
  HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

Only `src/regex/{regcomp,regexec,regerror,tre-mem}.c` are built; the rest of
musl is not compiled, linked or shipped. `gnu_compat.c` and `gnuregex.h`, added
by patch 0004, are first-party MayteraOS code under the MIT licence in
`userland/libc/LICENSE`.

**SQLite 3.53.4** (`userland/ports/sqlite`), from the header comment at the top
of the upstream `sqlite3.h` of `sqlite-autoconf-3530400.tar.gz`, sha256
`0e9483900e92cd5de8fd48d16bf9200145a61f7fd5be542a5ac81d8a9516eb9c` (measured on
the build host). SQLite is dedicated to the public domain; the tarball ships no
`LICENSE` file, so the recipe's `licence_files` names `sqlite3.h`, where the
disclaimer and "blessing" live:

```
The author disclaims copyright to this source code.  In place of
a legal notice, here is a blessing:

   May you do good and not evil.
   May you find forgiveness for yourself and forgive others.
   May you share freely, never taking more than you give.
```

Two notes on the SQLite entry.

**Public domain imposes no notice obligation, and this entry is a courtesy, not
a requirement.** The SQLite code ships inside any app that links `libsqlite3.a`;
none of those binaries needs to carry the text, and neither would this file. It
is here because this document records provenance for every third-party copy
regardless of licence.

**Our delta is zero patches to upstream `sqlite3.c`.** The MayteraOS adaptation
is (a) the compile-time option set in `userland/ports/sqlite/build.sh`
(`SQLITE_OS_OTHER=1` and the rest, which SQLite is explicitly designed to be
configured by) and (b) a first-party `sqlite3_vfs` in
`userland/ports/sqlite/maytera_vfs.c`, which is MayteraOS code under the MIT
licence in `userland/libc/LICENSE`. The upstream amalgamation ships
byte-for-byte unaltered.

**jq 1.8.1** (`userland/ports/jq`), from the upstream `COPYING` of
`jq-1.8.1.tar.gz`, sha256
`2be64e7129cecb11d5906290eba10af694fb9e3e7f9fc208a311dc33ca837eb0` (measured on
the build host). This one recipe compiles THREE third-party works into one
`libjq.a`, so all three grants are recorded here.

jq itself is MIT (only the software grant is reproduced; jq's `docs/` are CC BY
3.0 upstream but no jq documentation is shipped by this port):

```
jq is copyright (C) 2012 Stephen Dolan

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

**Oniguruma 6.9.10** is jq's bundled regex engine;
`userland/ports/jq/build.sh` compiles `vendor/oniguruma` into the same archive.
From that tree's `COPYING`, it is 2-clause BSD:

```
Oniguruma LICENSE
-----------------

Copyright (c) 2002-2021  K.Kosako  <kkosako0@gmail.com>
All rights reserved.
### libedit 20260512-3.1 (`userland/ports/libedit`)

**libedit 20260512-3.1** (`userland/ports/libedit`), from `COPYING` of
`libedit-20260512-3.1.tar.gz`, sha256
`432d5e7ea8b0116dd39f2eca7bc11d0eed77faa6b77ea526ace89907c23ea4a0` (measured on
the build host), reproduced in full and verbatim. It is the BSD 3-clause
licence, compatible with the GPLv2 base (permissive, non-copyleft). This
release carries NO 4-clause advertising files (measured: zero files match
"advertising materials"), which is one reason the port pins the current
wide-char release rather than an older narrow-char one:

```
Copyright (c) 1992, 1993
 The Regents of the University of California.  All rights reserved.

This code is derived from software contributed to Berkeley by
Christos Zoulas of Cornell University.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
3. Neither the name of the University nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.
```

**IBM decNumber** is jq's bundled arbitrary-precision decimal
(`vendor/decNumber`, linked because this port sets `USE_DECNUM` for exact
big-literal preservation). It ships no separate LICENSE file; the grant is the
header comment at the top of `vendor/decNumber/decNumber.c`, the ICU licence:

```
------------------------------------------------------------------
Decimal Number arithmetic module
------------------------------------------------------------------
Copyright (c) IBM Corporation, 2000, 2009.  All rights reserved.
This software is made available under the terms of the
ICU License -- ICU 1.8.1 and later.
```

**Our delta is zero patches to upstream.** The jq, Oniguruma and decNumber
sources ship byte-for-byte unaltered. The MayteraOS adaptation is entirely (a)
the `-D` option set recorded in `userland/ports/jq/PORT` (which jq is designed
to be configured by) and (b) the small Oniguruma `config.h` plus the generated
`builtin.inc`/`config_opts.inc` that `userland/ports/jq/build.sh` writes as
data. Building it also added four genuinely missing functions to the shared
libc rather than forking private copies: `realpath()`, `isnormal()`, and
`tzset()`/`tzname[]` (the last two were declared in `time.h` since #359 but
never defined).
**darkhttpd 1.16** (`userland/ports/darkhttpd`), from `COPYING` of
`darkhttpd-1.16.tar.gz`, sha256
`ab97ea3404654af765f78282aa09cfe4226cb007d2fcc59fe1a475ba0fef1981` (measured on
the build host, reproducible across independent downloads of the GitHub
release-tag archive). darkhttpd is copyright (c) 2003-2024 Emil Mikulic and is
distributed under the ISC licence, reproduced in full and verbatim:

```
Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the
above copyright notice and this permission notice appear in all
copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.
```

The ISC licence is the plain-language permissive licence functionally equivalent
to two-clause BSD / MIT, and is GPLv2-compatible. **Our delta is one patch**
(`patches/0001-getsockname-nonfatal.patch`): MayteraOS's kernel exposes no
`getsockname()`, so the libc stub returns `-1`/`ENOSYS` and the patch makes
darkhttpd's single startup call tolerate exactly that, keeping the address it
already bound with. The server otherwise ships unaltered; the `-DNO_IPV6` build
option and the `-U__linux` flags that select its generic-POSIX branch are
compile-time configuration, not source edits.

**libarchive 3.8.9** (`userland/ports/libarchive`), from the upstream `COPYING`
file of `libarchive-3.8.9.tar.gz`, sha256
`f5a6539059cf5e597dbeda37bfa4874b1e8dea063c8d93bf85a2b44af90a5bd4`. The
distribution as a whole is **BSD-2-Clause**; its verbatim notice is:

```
Copyright (c) 2003-2018 <author(s)>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer
   in this position and unchanged.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

**Per-file exceptions, as declared in the upstream `COPYING` and honoured by the
files we build.** These are documented rather than glossed because two of the
three are in the archive we ship: `libarchive/archive_read_support_filter_compress.c`
(the `.Z`/LZW reader, which IS built) is additionally subject to a 3-clause UC
Regents copyright; `libarchive/archive_parse_date.c` (built) is in the public
domain; and `libarchive/archive_blake2s_ref.c` / `archive_blake2sp_ref.c` (built)
are triple-licensed with a choice of CC0-1.0, OpenSSL, or Apache-2.0. None of
these adds a copyleft obligation; the strictest is the 3-clause notice, which is
satisfied by reproducing the notice above and this paragraph. The upstream
build scripts carry varying terms but are not shipped: mports fetches the
tarball and compiles a fixed, enumerated source list.

**Our delta is one patch and it adds a first-party file, not a change to
upstream code.** `userland/ports/libarchive/patches/0001-add-maytera-config-h.patch`
creates `libarchive/config.h` (MIT, MayteraOS, the licence in
`userland/libc/LICENSE`), which stands in for the header upstream `configure`
would generate. Every upstream `.c` we compile ships byte-for-byte unaltered.

**What this port supports, measured, so the App Store and Files app claims are
honest.** READING is the target and is proven against real archives created by
the host `tar`/`zip`/`cpio`: tar (+gzip via the ported zlib), zip (DEFLATE via
zlib), and cpio all list entries and extract bytes that match the originals
exactly. Also compiled as functional readers: `ar`, `mtree`, `iso9660`, `raw`,
`empty`, `warc`, `cab`, `lha`, `rar`, and `rar5`. `7zip` reads store/DEFLATE
(and PPMd) entries; 7z streams compressed with LZMA/LZMA2 do NOT decompress,
because liblzma is not bundled. The archive-CREATION (write) path is also built
for the mainstream formats (ustar/pax/gnutar, cpio variants, zip, ar, shar,
mtree, raw, warc) with the gzip/none/compress/uuencode/b64 write filters.

**What is deliberately DISABLED, and why (owner rule: do not own a dependency
tree you did not intend to).** Compression backends needing a heavy external
library are OFF: `bzip2`, `xz`/`lzma`, `lz4`, `zstd`, `lzop`, `grzip`, `lrzip`.
libarchive still registers them, but through its external-helper mechanism,
which reports a clean error on MayteraOS because those helper programs are not
installed. `xar` needs libxml2/expat plus liblzma and therefore registers as an
honest "Xar not supported on this platform" stub. Extraction directly to the
real filesystem (`archive_write_disk`, and the `archive_read_extract` helpers
that drive it) is NOT built: it needs `mknod`/`chown`/`symlink`/`lchown`/
`mkfifo`/`futimens` surface the libc does not yet expose; a consumer extracts by
reading data blocks and writing files itself. This is a documented follow-up.

**This port does NOT replace the OS's own `userland/libarchive/arc.c`.** That
limited archiver stays; libarchive is added ALONGSIDE it for broad read-format
coverage. The two libc additions this port needed (`div`/`ldiv`/`lldiv`,
`strtoimax`/`strtoumax`, the standard `<sys/stat.h>` mode bits, and a WEAK
`timezone` global; `tzset` is provided by the jq port's tz.c) are described in the CHANGELOG entry for #745.
**Our delta is one patch.** `userland/ports/libedit/patches/0001-drop-unused-langinfo-include.patch`
removes a single unused `#include <langinfo.h>` from `src/el.c` (nothing in
libedit calls `nl_langinfo`). **CORRECTED by the Angband port below:**
MayteraOS has one locale (`userland/libc/locale.h` is explicit about that) but
DOES now have `<langinfo.h>`/`nl_langinfo()`, added to the shared libc for
Angband's `setlocale()`+`nl_langinfo(CODESET)` UTF-8-detection idiom; it was
genuinely absent when this libedit sentence was written.
Everything else libedit needed was ADDED TO THE SHARED LIBC, not patched into
upstream: the wide-char string/conversion layer, `<strings.h>`,
`<sys/ttydefaults.h>`, `SA_ONSTACK`, the `S_I*` mode bits, the BSD `u_*` types,
and `fseeko`/`ftello`/`mkstemp`/`execlp`/`getpwent`. libedit links against the
ncurses termcap (`tgetent`/`tgetstr`) and the musl-regex POSIX regex it needs;
`/APPS/MSH` (the Terminal's shell) and `/APPS/ELDEMO` are the consumers.

**Angband 4.2.6** (`userland/ports/angband`), the git tag source archive
`angband-4.2.6.tar.gz` fetched from
`https://github.com/angband/angband/archive/refs/tags/4.2.6.tar.gz`, sha256
`64091eb98e1b08c4d69a9ca94802ea797aef09daaaf335e450bc64f80ee56911` (measured on
the build host; 25,964,779 bytes). **There is no standalone COPYING/LICENSE
file upstream** (confirmed against both the release tarball's file listing and
GitHub's own license-detection API, which returns 404/"Not Found" for this
repository); every source file instead carries an identical dual-licence
header, reproduced here verbatim from `src/main.c`:

```
This work is free software; you can redistribute it and/or modify it
under the terms of either:

a) the GNU General Public License as published by the Free Software
   Foundation, version 2, or

b) the "Angband licence":
   This software may be copied and distributed for educational, research,
   and not for profit purposes provided that this copyright and statement
   are included in all such copies.  Other copyrights may also apply.
```

We redistribute under option (a), **GPL-2.0-only**: the clear, well-understood
choice, matching `docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md`'s own
classification of this candidate. Option (b), the "Angband licence", is a
non-standard, non-OSI-approved grant (it restricts to "educational, research,
and not for profit purposes", which GPL-2.0 does not) and was not chosen.
`/APPS/ANGBAND` ships as a standalone binary with its own corresponding-source
obligation (this PORT recipe, its one patch, and the exact upstream tarball
pin ARE the source); it links `userland/libc` (MIT) and `userland/ports/ncurses`
(X11-style permissive), neither copyleft, so nothing here makes any OTHER
MayteraOS binary GPL.

**Our delta is one patch, and it ADDS a first-party file rather than changing
upstream code**, the same shape as `userland/ports/musl-regex/patches/0004`'s
`gnuregex.h`/`gnu_compat.c`. `main-gcu.c`'s only glyph-drawing routine,
`Term_text_gcu()`, calls `mvwaddnwstr()` unconditionally; that is an ncursesw
(wide-character) entry point, and `userland/ports/ncurses` is deliberately
built `--disable-widec` (8-bit/UTF-8-passthrough terminal, not wide tile
graphics). `patches/0001-mvwaddnwstr-narrow-curses-compat.patch` adds
`src/mayteraos-curses-compat.{h,c}` (SPDX MIT, MayteraOS contributors copyright,
narrow-`waddch()`-based implementation) and one `#include` line in
`src/main-gcu.c`. Every upstream `.c` we compile otherwise ships byte-for-byte
unaltered; only the GCU frontend is built (`-DUSE_GCU`), with X11/SDL/SDL2/
stats/spoil/win left out entirely, per section 2.6's "no X11, GTK, Qt, or
SDL".

**Three genuine libc gaps this port closed, added to the SHARED libc per
docs/MPORTS.md owner rule 1** (documented in full in `userland/ports/angband/PORT`'s
header comment and in the CHANGELOG entry for this work): `<langinfo.h>` +
`nl_langinfo()` (`main.c`'s standard `setlocale()`+`nl_langinfo(CODESET)`
UTF-8-detection idiom; answered honestly as "UTF-8", the terminal's real
transport encoding, not a locale claim), `<fcntl.h>`'s `struct flock` and the
`F_RDLCK`/`F_WRLCK`/`F_UNLCK`/`F_GETLK`/`F_SETLK`/`F_SETLKW` constants
(`z-file.c`'s advisory `file_lock()`/`file_unlock()`, source-compatibility only:
the kernel does not implement record locking and every caller already ignores
the return value, the same honest single-writer contract as the SQLite port's
VFS), and `<sys/stat.h>`'s `umask()` (process-local bookkeeping only, since
MayteraOS's `mkdir()`/`open(O_CREAT)` apply the caller's explicit mode with no
mask consulted anywhere in the kernel path).


### myman 0.7.0 (`userland/ports/myman`)

**myman 0.7.0** (`userland/ports/myman`), from `LICENSE` of `myman-0.7.0.tar.gz`,
sha256 `31a94b2c8949a35d18eed35e8f685801fbfeb71418a3c01eafbf710ce95a292b`
(measured on the build host), reproduced in full and verbatim. It is the MIT
licence, compatible with the GPLv2 base (permissive, non-copyleft):

```
myman - the MyMan video game
Copyright 1997-2008, Benjamin C. Wiley Sittler <bsittler@gmail.com>

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

Author contact information:

        Benjamin C. W. Sittler <bsittler@gmail.com>
        2806 Foothill Boulevard
        Oakland, California 94601
        U.S.A.
```

**Our delta is one patch.**
`userland/ports/myman/patches/0001-drop-unused-langinfo-include.patch` removes
a single unused `#include <langinfo.h>` from `src/myman.c` (its one caller,
`nl_langinfo(CODESET)`, is already guarded by `#ifdef CODESET`, so the removal
just leaves that branch permanently unused; the same gap and the same fix as
`userland/ports/libedit`). This port also does not build myman's own
self-hosting `configure`/Makefile system (which can cross-generate a
no-runtime-files "builtin data" binary by running myman.c twice); instead it
compiles `src/myman.c` directly with the `-DMYMANSIZE`/`-DTILEFILE`/
`-DSPRITEFILE` overrides upstream's own `simple.mk` documents for exactly this
case, and ships the three small data files the "square" variant reads at
runtime (`chr/khr1.txt`, `spr/spr1.txt`, `lvl/maze.txt`) via `install_sources`,
copied out of this same sha256-pinned tarball, not vendored into git. See
`userland/ports/myman/PORT`'s header comment for the full reasoning.
`/APPS/MYMAN` is the consumer.

### ninvaders 0.1.1 (`userland/ports/ninvaders`)

**ninvaders 0.1.1** (`userland/ports/ninvaders`), from `gpl.txt` of
`ninvaders-0.1.1.tar.gz`, sha256
`bfbc5c378704d9cf5e7fed288dac88859149bee5ed0850175759d310b61fd30b` (measured on
the build host). Every source file's header reads "either version 2 of the
License, or (at your option) any later version", so this component is
recorded as **GPL-2.0-or-later**, reproduced in full and verbatim:

```
		    GNU GENERAL PUBLIC LICENSE
		       Version 2, June 1991

 Copyright (C) 1989, 1991 Free Software Foundation, Inc.
                       59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 Everyone is permitted to copy and distribute verbatim copies
 of this license document, but changing it is not allowed.

			    Preamble

  The licenses for most software are designed to take away your
freedom to share and change it.  By contrast, the GNU General Public
License is intended to guarantee your freedom to share and change free
software--to make sure the software is free for all its users.  This
General Public License applies to most of the Free Software
Foundation's software and to any other program whose authors commit to
using it.  (Some other Free Software Foundation software is covered by
the GNU Library General Public License instead.)  You can apply it to
your programs, too.

  When we speak of free software, we are referring to freedom, not
price.  Our General Public Licenses are designed to make sure that you
have the freedom to distribute copies of free software (and charge for
this service if you wish), that you receive source code or can get it
if you want it, that you can change the software or use pieces of it
in new free programs; and that you know you can do these things.

  To protect your rights, we need to make restrictions that forbid
anyone to deny you these rights or to ask you to surrender the rights.
These restrictions translate to certain responsibilities for you if you
distribute copies of the software, or if you modify it.

  For example, if you distribute copies of such a program, whether
gratis or for a fee, you must give the recipients all the rights that
you have.  You must make sure that they, too, receive or can get the
source code.  And you must show them these terms so they know their
rights.

  We protect your rights with two steps: (1) copyright the software, and
(2) offer you this license which gives you legal permission to copy,
distribute and/or modify the software.

  Also, for each author's protection and ours, we want to make certain
that everyone understands that there is no warranty for this free
software.  If the software is modified by someone else and passed on, we
want its recipients to know that what they have is not the original, so
that any problems introduced by others will not reflect on the original
authors' reputations.

  Finally, any free program is threatened constantly by software
patents.  We wish to avoid the danger that redistributors of a free
program will individually obtain patent licenses, in effect making the
program proprietary.  To prevent this, we have made it clear that any
patent must be licensed for everyone's free use or not licensed at all.

  The precise terms and conditions for copying, distribution and
modification follow.

		    GNU GENERAL PUBLIC LICENSE
   TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION

  0. This License applies to any program or other work which contains
a notice placed by the copyright holder saying it may be distributed
under the terms of this General Public License.  The "Program", below,
refers to any such program or work, and a "work based on the Program"
means either the Program or any derivative work under copyright law:
that is to say, a work containing the Program or a portion of it,
either verbatim or with modifications and/or translated into another
language.  (Hereinafter, translation is included without limitation in
the term "modification".)  Each licensee is addressed as "you".

Activities other than copying, distribution and modification are not
covered by this License; they are outside its scope.  The act of
running the Program is not restricted, and the output from the Program
is covered only if its contents constitute a work based on the
Program (independent of having been made by running the Program).
Whether that is true depends on what the Program does.

  1. You may copy and distribute verbatim copies of the Program's
source code as you receive it, in any medium, provided that you
conspicuously and appropriately publish on each copy an appropriate
copyright notice and disclaimer of warranty; keep intact all the
notices that refer to this License and to the absence of any warranty;
and give any other recipients of the Program a copy of this License
along with the Program.

You may charge a fee for the physical act of transferring a copy, and
you may at your option offer warranty protection in exchange for a fee.

  2. You may modify your copy or copies of the Program or any portion
of it, thus forming a work based on the Program, and copy and
distribute such modifications or work under the terms of Section 1
above, provided that you also meet all of these conditions:

    a) You must cause the modified files to carry prominent notices
    stating that you changed the files and the date of any change.

    b) You must cause any work that you distribute or publish, that in
    whole or in part contains or is derived from the Program or any
    part thereof, to be licensed as a whole at no charge to all third
    parties under the terms of this License.

    c) If the modified program normally reads commands interactively
    when run, you must cause it, when started running for such
    interactive use in the most ordinary way, to print or display an
    announcement including an appropriate copyright notice and a
    notice that there is no warranty (or else, saying that you provide
    a warranty) and that users may redistribute the program under
    these conditions, and telling the user how to view a copy of this
    License.  (Exception: if the Program itself is interactive but
    does not normally print such an announcement, your work based on
    the Program is not required to print an announcement.)

These requirements apply to the modified work as a whole.  If
identifiable sections of that work are not derived from the Program,
and can be reasonably considered independent and separate works in
themselves, then this License, and its terms, do not apply to those
sections when you distribute them as separate works.  But when you
distribute the same sections as part of a whole which is a work based
on the Program, the distribution of the whole must be on the terms of
this License, whose permissions for other licensees extend to the
entire whole, and thus to each and every part regardless of who wrote it.

Thus, it is not the intent of this section to claim rights or contest
your rights to work written entirely by you; rather, the intent is to
exercise the right to control the distribution of derivative or
collective works based on the Program.

In addition, mere aggregation of another work not based on the Program
with the Program (or with a work based on the Program) on a volume of
a storage or distribution medium does not bring the other work under
the scope of this License.

  3. You may copy and distribute the Program (or a work based on it,
under Section 2) in object code or executable form under the terms of
Sections 1 and 2 above provided that you also do one of the following:

    a) Accompany it with the complete corresponding machine-readable
    source code, which must be distributed under the terms of Sections
    1 and 2 above on a medium customarily used for software interchange; or,

    b) Accompany it with a written offer, valid for at least three
    years, to give any third party, for a charge no more than your
    cost of physically performing source distribution, a complete
    machine-readable copy of the corresponding source code, to be
    distributed under the terms of Sections 1 and 2 above on a medium
    customarily used for software interchange; or,

    c) Accompany it with the information you received as to the offer
    to distribute corresponding source code.  (This alternative is
    allowed only for noncommercial distribution and only if you
    received the program in object code or executable form with such
    an offer, in accord with Subsection b above.)

The source code for a work means the preferred form of the work for
making modifications to it.  For an executable work, complete source
code means all the source code for all modules it contains, plus any
associated interface definition files, plus the scripts used to
control compilation and installation of the executable.  However, as a
special exception, the source code distributed need not include
anything that is normally distributed (in either source or binary
form) with the major components (compiler, kernel, and so on) of the
operating system on which the executable runs, unless that component
itself accompanies the executable.

If distribution of executable or object code is made by offering
access to copy from a designated place, then offering equivalent
access to copy the source code from the same place counts as
distribution of the source code, even though third parties are not
compelled to copy the source along with the object code.

  4. You may not copy, modify, sublicense, or distribute the Program
except as expressly provided under this License.  Any attempt
otherwise to copy, modify, sublicense or distribute the Program is
void, and will automatically terminate your rights under this License.
However, parties who have received copies, or rights, from you under
this License will not have their licenses terminated so long as such
parties remain in full compliance.

  5. You are not required to accept this License, since you have not
signed it.  However, nothing else grants you permission to modify or
distribute the Program or its derivative works.  These actions are
prohibited by law if you do not accept this License.  Therefore, by
modifying or distributing the Program (or any work based on the
Program), you indicate your acceptance of this License to do so, and
all its terms and conditions for copying, distributing or modifying
the Program or works based on it.

  6. Each time you redistribute the Program (or any work based on the
Program), the recipient automatically receives a license from the
original licensor to copy, distribute or modify the Program subject to
these terms and conditions.  You may not impose any further
restrictions on the recipients' exercise of the rights granted herein.
You are not responsible for enforcing compliance by third parties to
this License.

  7. If, as a consequence of a court judgment or allegation of patent
infringement or for any other reason (not limited to patent issues),
conditions are imposed on you (whether by court order, agreement or
otherwise) that contradict the conditions of this License, they do not
excuse you from the conditions of this License.  If you cannot
distribute so as to satisfy simultaneously your obligations under this
License and any other pertinent obligations, then as a consequence you
may not distribute the Program at all.  For example, if a patent
license would not permit royalty-free redistribution of the Program by
all those who receive copies directly or indirectly through you, then
the only way you could satisfy both it and this License would be to
refrain entirely from distribution of the Program.

If any portion of this section is held invalid or unenforceable under
any particular circumstance, the balance of the section is intended to
apply and the section as a whole is intended to apply in other
circumstances.

It is not the purpose of this section to induce you to infringe any
patents or other property right claims or to contest validity of any
such claims; this section has the sole purpose of protecting the
integrity of the free software distribution system, which is
implemented by public license practices.  Many people have made
generous contributions to the wide range of software distributed
through that system in reliance on consistent application of that
system; it is up to the author/donor to decide if he or she is willing
to distribute software through any other system and a licensee cannot
impose that choice.

This section is intended to make thoroughly clear what is believed to
be a consequence of the rest of this License.

  8. If the distribution and/or use of the Program is restricted in
certain countries either by patents or by copyrighted interfaces, the
original copyright holder who places the Program under this License
may add an explicit geographical distribution limitation excluding
those countries, so that distribution is permitted only in or among
countries not thus excluded.  In such case, this License incorporates
the limitation as if written in the body of this License.

  9. The Free Software Foundation may publish revised and/or new versions
of the General Public License from time to time.  Such new versions will
be similar in spirit to the present version, but may differ in detail to
address new problems or concerns.

Each version is given a distinguishing version number.  If the Program
specifies a version number of this License which applies to it and "any
later version", you have the option of following the terms and conditions
either of that version or of any later version published by the Free
Software Foundation.  If the Program does not specify a version number of
this License, you may choose any version ever published by the Free Software
Foundation.

  10. If you wish to incorporate parts of the Program into other free
programs whose distribution conditions are different, write to the author
to ask for permission.  For software which is copyrighted by the Free
Software Foundation, write to the Free Software Foundation; we sometimes
make exceptions for this.  Our decision will be guided by the two goals
of preserving the free status of all derivatives of our free software and
of promoting the sharing and reuse of software generally.

			    NO WARRANTY

  11. BECAUSE THE PROGRAM IS LICENSED FREE OF CHARGE, THERE IS NO WARRANTY
FOR THE PROGRAM, TO THE EXTENT PERMITTED BY APPLICABLE LAW.  EXCEPT WHEN
OTHERWISE STATED IN WRITING THE COPYRIGHT HOLDERS AND/OR OTHER PARTIES
PROVIDE THE PROGRAM "AS IS" WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESSED
OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.  THE ENTIRE RISK AS
TO THE QUALITY AND PERFORMANCE OF THE PROGRAM IS WITH YOU.  SHOULD THE
PROGRAM PROVE DEFECTIVE, YOU ASSUME THE COST OF ALL NECESSARY SERVICING,
REPAIR OR CORRECTION.

  12. IN NO EVENT UNLESS REQUIRED BY APPLICABLE LAW OR AGREED TO IN WRITING
WILL ANY COPYRIGHT HOLDER, OR ANY OTHER PARTY WHO MAY MODIFY AND/OR
REDISTRIBUTE THE PROGRAM AS PERMITTED ABOVE, BE LIABLE TO YOU FOR DAMAGES,
INCLUDING ANY GENERAL, SPECIAL, INCIDENTAL OR CONSEQUENTIAL DAMAGES ARISING
OUT OF THE USE OR INABILITY TO USE THE PROGRAM (INCLUDING BUT NOT LIMITED
TO LOSS OF DATA OR DATA BEING RENDERED INACCURATE OR LOSSES SUSTAINED BY
YOU OR THIRD PARTIES OR A FAILURE OF THE PROGRAM TO OPERATE WITH ANY OTHER
PROGRAMS), EVEN IF SUCH HOLDER OR OTHER PARTY HAS BEEN ADVISED OF THE
POSSIBILITY OF SUCH DAMAGES.

		     END OF TERMS AND CONDITIONS
```

(the "How to Apply These Terms" boilerplate that follows the terms in
`gpl.txt` is the standard GPLv2 template text, not part of the licence grant
itself, and is omitted here as it is in this repository's own root `COPYING`.)

**THE COPYLEFT ANSWER (MPORTS.md): combined, not aggregate.** ninvaders'
object files are statically linked with the permissively-licensed ncurses and
`userland/libc` into ONE executable, `/APPS/NINVADERS`. This is the same
"linked into a work we distribute, and then its terms flow into that work"
shape as `/APPS/VI` (busybox GPLv2 + LGPLv3 GNU regex, "must therefore be
offered as GPLv3"): the combined `/APPS/NINVADERS` binary as a whole must be
offered under GPL-2.0-or-later. Neither ncurses nor `userland/libc` is thereby
made GPL; each keeps its own permissive licence as source and neither is
modified by this port (the MPORTS.md libc note that statically linking MIT
libc does not make an application GPL holds the same way in this direction:
permissive code linked BY a GPL program does not itself become GPL).
Corresponding source for `/APPS/NINVADERS` is the pinned
`ninvaders-0.1.1.tar.gz`, the three patches below, and this repository's own
(already open) ncurses and libc source used to build it.

**Our delta is three patches**, each documented at the top of its own file
under `userland/ports/ninvaders/patches/`:

1. `0001-ncurses-header-to-curses-h.patch` - `view.h` includes `<ncurses.h>`
   by name; this tree's `userland/ports/ncurses` port installs only
   `curses.h` (see that port's `headers=`), so the include is retargeted.
2. `0002-random-to-rand.patch` - `aliens.c`/`ufo.c` call the BSD `random()`,
   which `userland/libc` does not provide (only ISO C `rand()`/`srand()` do);
   both call sites only ever use `% N`, so the distribution/period difference
   between the two generators is immaterial here.
3. `0003-ncurses-timeout-instead-of-setitimer.patch` - upstream drives its
   1/FPS game tick from `setitimer(ITIMER_REAL, ...)` firing `SIGALRM`, both
   of which need libc surface MayteraOS does not provide (measured: no
   `setitimer()`/`ITIMER_REAL`; only the one-second-granularity `alarm()`
   exists) and a signal-delivery guarantee (interrupting a blocked `getch()`
   on the compositor pty) this port does not want to depend on. Patched to
   drive the same tick synchronously off ncurses' own `timeout()`/`getch()`
   polling idiom instead, matching docs/MPORTS.md's own pointer that "ncurses
   already provides `napms()` for timing". See the patch file for the full
   reasoning inline.

`/APPS/NINVADERS` is the consumer.
### moon-buggy 1.0.51 (`userland/ports/moonbuggy`)

**moon-buggy 1.0.51** (`userland/ports/moonbuggy`), from `COPYING` of
`moon-buggy-1.0.51.tar.gz`, sha256
`352dc16ccae4c66f1e87ab071e6a4ebeb94ff4e4f744ce1b12a769d02fe5d23f` (measured on
the build host). moon-buggy is Jochen Voss's ncurses lunar side-scroller
(https://www.seehuhn.de/pages/moon-buggy). Its source files carry a bare
`Copyright Jochen Voss` header and `README`/`moon-buggy.texi` state generically
"under the terms of the GNU General Public License"; the shipped `COPYING` is
the GPLv2 text whose recommended notice reads "version 2 of the License, or (at
your option) any later version", and Debian has recorded moon-buggy as GPL-2+
throughout its life, so this component is recorded as **GPL-2.0-or-later**. The
GPLv2 text is byte-for-byte the same one reproduced verbatim in the ninvaders
mports note above (`COPYING` of both tarballs is the standard GNU GPL v2), and
is not duplicated here.

The mports recipe fetches the tarball at build time and compiles its 26
translation units into `libmoonbugy.a`, consumed by `/APPS/MOONBUGY`. Upstream
is autoconf; the recipe drives the sources directly (`build=script`), writing a
small honest `config.h` and running upstream's own two `sed` scripts to
regenerate `copying.h` (the in-game copyright screen, from `COPYING`) and
`buggy.h` (the buggy sprites, from `car.img`). Three patches under
`userland/ports/moonbuggy/patches/` adapt it to MayteraOS, each documented in
its own file and in `CHANGELOG.md`:

* `0001-queue-ncurses-timeout-instead-of-select.patch` replaces the main
  loop's `select(2)`-on-stdin wait with ncurses' own `wtimeout()`/`wgetch()`
  timing (MayteraOS's `select()` is network-oriented and does not wake on a
  Terminal-pty keypress). The same idiom the ninvaders port used.
* `0002-highscore-advisory-lock-best-effort.patch` makes the score-file
  `fcntl(F_SETLKW)` advisory lock best-effort (MayteraOS has no multi-process
  record locking; single writer) instead of fatal.
* `0003-terminal-drop-mesg-fchmod.patch` removes the "mesg n" `fchmod()` path
  (not needed on the single-user Terminal, and `fchmod()` is not in this libc).

**THE COPYLEFT ANSWER (MPORTS.md): combined, not aggregate.** moon-buggy's
object files are statically linked with the permissively-licensed
`libncurses.a` and `userland/libc` into ONE executable, `/APPS/MOONBUGY` (the
same shape as `/APPS/NINVADERS`). The combined binary as a whole must therefore
be offered under **GPL-2.0-or-later**; this does not make ncurses or
`userland/libc` GPL (neither is modified, each keeps its own permissive terms).
Corresponding source for `/APPS/MOONBUGY` is the pinned `moon-buggy-1.0.51.tar.gz`,
the three patches above, and this repository's own ncurses/libc source.
`/APPS/MOONBUGY` is the consumer.

### QuickJS-ng 0.16.2 (`userland/ports/quickjs`)

From the root `LICENSE` of `quickjs-ng-0.16.2.tar.gz`, sha256
`97c80625b26775a4c7ca618c004d4ea24cf99cbf867e4eba78bd927a8b23d106`. The mports
recipe fetches this tarball at build time and compiles its engine (dtoa.c,
libregexp.c, libunicode.c, quickjs.c) into `libquickjs.a`, consumed by
`/APPS/QJS`. QuickJS-ng is the actively maintained community fork of Fabrice
Bellard's QuickJS; both are MIT. This port does not build `quickjs-libc.c` (the
std/os OS-binding module), keeping the shipped library pure computation.

```
The MIT License (MIT)

Copyright (c) 2017-2026 Fabrice Bellard
Copyright (c) 2017-2024 Charlie Gordon
Copyright (c) 2023-2026 Ben Noordhuis
Copyright (c) 2023-2026 Saul Ibarra Corretge

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

Our delta beyond the recipe is one patch, `cutils-maytera.patch` (a two-hunk
target adaptation so cutils.h does not `#include <malloc.h>` on the MayteraOS
freestanding libc), and one first-party file, `maytera_compat.c` (a single WEAK
`pthread_condattr_setclock` the engine references but libc does not define). The
upstream engine source is otherwise byte-for-byte unaltered. `/APPS/QJS`
(`userland/apps/qjs`) is the consumer.

### bc-gh 7.0.3 (`userland/ports/bc`)

From the root `LICENSE.md` of `bc-7.0.3.tar.gz`, sha256
`3c625e8034ef47c9ae11a1ed3dcc22314bf374144decf546647b9a722f7313d6`. The mports
recipe fetches this tarball at build time and compiles the bc-only engine
(seventeen `src/*.c` translation units plus the strgen-generated `gen/lib.c`,
`gen/lib2.c` and `gen/bc_help.c`) into `libbc.a`, linked by `/APPS/bc`. bc-gh is
the bc/dc implementation Gavin Howard maintains, shipped as the system bc by
several BSDs and chosen here over GNU bc, which is GPL-3.0-or-later and blocked
against the GPLv2 base. This port builds bc only (dc is not built) with history
and NLS disabled, so `src/dc*.c`, `src/history.c` and `src/library.c` are not
compiled.

```
Copyright (c) 2018-2024 Gavin D. Howard <gavin@gavinhoward.com>

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

`LICENSE.md` also carries a separate BSD-2-Clause grant (Salvatore Sanfilippo;
Pieter Noordhuis) covering `src/history.c` / `include/history.h`, the linenoise-
derived line editor. This port disables history, so neither file is compiled and
that grant does not apply to anything shipped. Our delta beyond the recipe is one
patch, `status-maytera-sigjmp.patch` (a target adaptation mapping
`sigsetjmp`/`siglongjmp`/`sigjmp_buf` to the plain `setjmp`/`longjmp`/`jmp_buf`
the freestanding libc provides, under `-DMAYTERAOS`, matching upstream's own
`_WIN32` branch), plus `build.sh` (the codegen-then-cross-compile step). The
upstream engine source is otherwise byte-for-byte unaltered. `/APPS/bc`
(`userland/apps/bc`) is the consumer, replacing the earlier integer-only
homegrown bc.

### XaoS 3.6 (`userland/apps/xaos`)

Tier 3 item #16 of `docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md`: a
real-time fractal zoomer, described there as "class D, matches the DOOM
precedent closely." That precedent is followed literally: this is NOT an
`mports` recipe (XaoS is a complete application, not a library another app
links against), it is vendored source under `userland/apps/xaos/src`, built
by its own `userland/apps/xaos/Makefile` into `/APPS/XAOS`, the same shape as
`userland/apps/doom`.

Pinned to the `release-3.6` tag of `github.com/xaos-project/XaoS`
(`XaoS-release-3.6.tar.gz`, fetched from
`https://github.com/xaos-project/XaoS/archive/refs/tags/release-3.6.tar.gz`,
sha256 `857c4ec71d0a25075a7af6255f5c36515d4d330f074f5aaf4da6fbe410e4e1f8`).
3.6 (2007) was chosen over the actively-maintained 4.3.x line because 4.x is
GTK-only; 3.6 is the last release with XaoS's own pluggable `struct
ui_driver` abstraction (`src/include/ui.h`) and a documented
`src/ui/ui-drv/template/ui_template.c` worked example, which is exactly the
seam a new platform port needs and is the same reason DOOM's own upstream
was pinned to a specific source drop rather than "whatever is newest".

**What is vendored, and what is not.** `userland/apps/xaos/src/{engine,
filter,ui,ui-hlp,util,include}` is upstream source, copied over UNMODIFIED
with two exceptions: `src/ui/drivers.c` (one clearly-marked hunk registering
the new driver, exactly like every other `*_DRIVER` already in that file) and
`src/util/timers.c` (one clearly-marked `#ifndef USE_CLOCK` line fixing a
stale `#error` guard that predates its own `USE_CLOCK` timing path). The
`sffe/` optional formula-parser subtree and `util/png.c` (needs libpng/zlib)
are deliberately not vendored at all: `SFFE_USING` is never defined, so
XaoS's normal built-in formula table (including the default Mandelbrot
`z^2+c`) is what runs, and nothing on the render path needs PNG export.
`src/include/config.h`, `src/include/aconfig.h` and `src/include/version.h`
are hand-written in place of the autoconf-generated files a real
`./configure` would produce (there is no configure in this build); each
carries an in-file note on what it replaces and why. The only genuinely new
code is `src/ui/ui-drv/maytera/ui_maytera.c` (the MayteraOS platform driver,
modelled on upstream's own `ui_win32.c`) and `xaos_compat.c` (three small
`#pragma weak` libc-gap fills: `putc`, `sincos`, a stub `writepng`;
see that file's own header for why each exists and its header note on `nm -u`
having been run against `libc.a` before landing, per the port hazard rule).

XaoS is GPL-2.0-or-later (every source file header: "either version 2 of the
License, or (at your option) any later version"); `userland/apps/xaos/COPYING`
is the real GPLv2 text copied verbatim out of the `release-3.6` tarball
(matching the sha256 above), not retyped from memory.


### cJSON 1.7.18 (`userland/ports/cjson`)

`userland/ports/cjson` holds the mports recipe only; the upstream source is
fetched at build time from `cJSON-1.7.18.tar.gz`, sha256
`3aa806844a03442c00769b83e99970be70fbef03735ff898f4811dd03b9f5ee5`, and is not
tracked. Because the upstream text is not in the tree, cJSON's `LICENSE` file
is reproduced here in full and verbatim:

```
Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

Our delta is zero patches to upstream: cJSON 1.7.18's `cJSON.c` compiles
unmodified against this userland's freestanding SSE2 toolchain, so what ships
is unaltered upstream. Only the core `cJSON.c` is built (the optional
`cJSON_Utils.c` JSON-Patch/Pointer layer is excluded by name in the recipe).

### Expat 2.7.1 (`userland/ports/expat`)

`userland/ports/expat` holds the mports recipe and one patch only; the upstream
source is fetched at build time from `expat-2.7.1.tar.gz`, sha256
`0cce2e6e69b327fc607b8ff264f4b66bdf71ead55a87ffd5f3143f535f15cfa2`, and is not
tracked. Because the upstream text is not in the tree, Expat's `COPYING` file is
reproduced here in full and verbatim:

```
Copyright (c) 1998-2000 Thai Open Source Software Center Ltd and Clark Cooper
Copyright (c) 2001-2025 Expat maintainers

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

Our delta is ONE patch, `0001-maytera-expat-config.patch`, which adds a
first-party `lib/expat_config.h` (expat's sources include it unconditionally and
upstream's generated copy assumes Linux getrandom/arc4random/mmap/dlfcn). That
header carries an `SPDX-License-Identifier: MIT` tag and the MayteraOS copyright
line, so `vendor-attribution-check.sh` classifies it as first-party rather than
unattributed third-party code. It selects `XML_POOR_ENTROPY` for the hash
secret salt: MayteraOS userland has no getrandom/arc4random and the kernel
entropy source (#654) is not yet surfaced to Ring 3, so expat derives the salt
from `clock() ^ getpid()`. That salt is a hash-flooding MITIGATION, not a
cryptographic key, and this is the documented upstream fallback; it is
appropriate for parsing trusted on-device XML and is NOT a general-purpose RNG.

### libyaml 0.2.5 (`userland/ports/libyaml`)

`userland/ports/libyaml` holds the mports recipe and one patch only; the
upstream source is fetched at build time from `yaml-0.2.5.tar.gz`, sha256
`c642ae9b75fee120b2d96c712538bd2cf283228d2337df2cf2988e3c02678ef4`, and is not
tracked. Because the upstream text is not in the tree, libyaml's `License` file
is reproduced here in full and verbatim:

```
Copyright (c) 2017-2020 Ingy döt Net
Copyright (c) 2006-2016 Kirill Simonov

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Our delta is ONE patch, `0001-maytera-libyaml-config.patch`, which adds a
first-party `src/config.h` supplying the four `YAML_VERSION_*` constants that
`src/api.c` reads (upstream generates them via autotools; we do not run
configure). It carries an `SPDX-License-Identifier: MIT` tag and the MayteraOS
copyright line, so `vendor-attribution-check.sh` classifies it as first-party.
The include is gated by `-DHAVE_CONFIG_H=1` in the recipe cflags.

### LZ4 1.10.0, library only (`userland/ports/lz4`)

`userland/ports/lz4` holds the mports recipe only; the upstream source is
fetched at build time from `lz4-1.10.0.tar.gz`, sha256
`537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b`, and is not
tracked. The LZ4 repository is dual-licensed: the command-line PROGRAMS are
GPLv2, and the LIBRARY (`lib/`) is BSD-2-Clause. This port builds ONLY the
library and takes the BSD grant; the GPLv2 programs are not built. Because the
upstream text is not in the tree, `lib/LICENSE` (the library grant) is reproduced
here in full and verbatim:

```
LZ4 Library
Copyright (c) 2011-2020, Yann Collet
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

Our delta is zero patches: the four library TUs (`lz4.c`, `lz4hc.c`,
`lz4frame.c`, `xxhash.c`) compile unmodified against this userland's freestanding
toolchain. `lz4file.c`, the FILE*-based wrapper, is excluded by name.

### Zstandard (zstd) 1.5.6, library only, BSD grant (`userland/ports/zstd`)

`userland/ports/zstd` holds the mports recipe only; the upstream source is
fetched at build time from `zstd-1.5.6.tar.gz`, sha256
`8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1`, and is not
tracked. zstd is DUAL-LICENSED: BSD-3-Clause (`LICENSE` at the repo root) OR
GPLv2 (`COPYING`). This port TAKES THE BSD GRANT and incurs no GPL obligation;
only the library is built (not the GPLv2 CLI programs). The build is
single-threaded (ZSTD_MULTITHREAD is not defined). Because the upstream text is
not in the tree, the root `LICENSE` (the BSD grant) is reproduced here in full
and verbatim:

```
BSD License

For Zstandard software

Copyright (c) Meta Platforms, Inc. and affiliates. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name Facebook, nor Meta, nor the names of its contributors may
   be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

Our delta is zero patches: the 25 library TUs (common + compress + decompress)
compile unmodified. legacy/, deprecated/ and dictBuilder/ are not built, and the
x86-64 BMI2 assembly is disabled via -DZSTD_DISABLE_ASM (the C Huffman-decode
path is used instead). The bundled xxHash is namespaced with -DXXH_NAMESPACE=ZSTD_
(upstream's own default) so it never clashes with LZ4's xxHash in a consumer.

### MD4C 0.5.2 (`userland/ports/md4c`)

`userland/ports/md4c` holds the mports recipe only; the upstream source is
fetched at build time from `md4c-0.5.2.tar.gz` (GitHub tag release-0.5.2),
sha256 `55d0111d48fb11883aaee91465e642b8b640775a4d6993c2d0e7a8092758ef21`, and is
not tracked. Because the upstream text is not in the tree, MD4C's `LICENSE.md`
is reproduced here in full and verbatim:

```
# The MIT License (MIT)

Copyright © 2016-2024 Martin Mitáš

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the “Software”),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.
```

Our delta is zero patches: `md4c.c`, `md4c-html.c` and `entity.c` compile
unmodified against this userland's freestanding toolchain. The default encoding
is UTF-8 (no MD4C_USE_* flag set). Only the parser + HTML renderer are built;
there is no CLI in the tarball's `src/`.

### libpng 1.6.44 (`userland/ports/libpng`)

`userland/ports/libpng` holds the mports recipe only; the upstream source is
fetched at build time from `libpng-1.6.44.tar.gz`, sha256
`8c25a7792099a0089fa1cc76c94260d0bb3f1ec52b93671b572f8bb61577b732`, and is not
tracked. It is the first port with a real cross-port dependency: `needs=libc
zlib`, so mports builds the already-ported zlib first and libpng links against
it. libpng 1.6.44 is distributed under the PNG Reference Library License version
2 (SPDX `libpng-2.0`), reproduced here in full and verbatim from the upstream
`LICENSE` (the file additionally retains the historical version-1 license text
and contributor list for libpng 0.5 through 1.6.35, not reproduced here because
version 2 is the operative grant for 1.6.44):

```
PNG Reference Library License version 2
---------------------------------------

 * Copyright (c) 1995-2024 The PNG Reference Library Authors.
 * Copyright (c) 2018-2024 Cosmin Truta.
 * Copyright (c) 2000-2002, 2004, 2006-2018 Glenn Randers-Pehrson.
 * Copyright (c) 1996-1997 Andreas Dilger.
 * Copyright (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc.

The software is supplied "as is", without warranty of any kind,
express or implied, including, without limitation, the warranties
of merchantability, fitness for a particular purpose, title, and
non-infringement.  In no event shall the Copyright owners, or
anyone distributing the software, be liable for any damages or
other liability, whether in contract, tort or otherwise, arising
from, out of, or in connection with the software, or the use or
other dealings in the software, even if advised of the possibility
of such damage.

Permission is hereby granted to use, copy, modify, and distribute
this software, or portions hereof, for any purpose, without fee,
subject to the following restrictions:

 1. The origin of this software must not be misrepresented; you
    must not claim that you wrote the original software.  If you
    use this software in a product, an acknowledgment in the product
    documentation would be appreciated, but is not required.

 2. Altered source versions must be plainly marked as such, and must
    not be misrepresented as being the original software.

 3. This Copyright notice may not be removed or altered from any
    source or altered source distribution.
```

Restriction 2 (altered source must be plainly marked) is why the mports design
keeps upstream unmodified and expresses any delta as a patch series. This recipe
carries NO patches: it copies upstream's own `scripts/pnglibconf.h.prebuilt` to
`pnglibconf.h` (the pre-configure step upstream documents for a non-autotools
build) and compiles the fifteen core sources unaltered. The CPU-SIMD sources are
not built (the prebuilt config enables no SIMD path). Restriction 1's
acknowledgment is "appreciated but not required", and this entry provides it.
### libsodium 1.0.20 (`userland/ports/libsodium`)

`userland/ports/libsodium` holds the mports recipe, our own RNG backend
`randombytes_maytera.c`, and `build.sh` only; the upstream source is fetched at
build time from `libsodium-1.0.20.tar.gz`, sha256
`ebb65ef6ca439333c2bb41a0c1990587288da07f6c7fd07cb3a18cc18d30ce19`, and is not
tracked. Only the PORTABLE reference implementation is built (no SIMD variant is
compiled), so it is one uniform C build with no per-file architecture flags.
libsodium is distributed under the ISC License, reproduced here in full and
verbatim from the upstream `LICENSE`:

```
ISC License

Copyright (c) 2013-2024
Frank Denis <j at pureftpd dot org>

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

THE RNG IS THE KERNEL CSPRNG. libsodium's default entropy source is not upstream
code here: upstream's `randombytes_sysrandom.c` is NOT compiled, and
`userland/ports/libsodium/randombytes_maytera.c` (MayteraOS-authored, not part of
the ISC upstream) is compiled in its place. It keeps the public symbol
`randombytes_sysrandom_implementation` that libsodium resolves its default to, so
`sodium_init()`/`randombytes_buf()` draw from the kernel HMAC-DRBG
(`crypto/csprng.c`) through the `SYS_GETRANDOM` syscall via the libc `getrandom()`
wrapper, with no `randombytes_set_implementation()` call required. It fails closed:
if `getrandom()` ever errors it `abort()`s rather than returning non-random bytes.

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
`/GAMES/DOOM/DOOM.ELF` is under id's own licence, and `/APPS/CLASSICUBE` is
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
> (the `d_*/i_*/r_*/p_*/w_*/z_*` files) is covered by id Software's own *DOOM
> Source Code License*, carried in every source-file header. It is a **separate
> license, not the GPL**, and permits non-commercial redistribution under id's
> terms. It is carved out of this repository's blanket GPLv2-or-later;
> MayteraOS's own code remains GPL. **The path in this paragraph used to read
> `kernel/games/doom` and to cite a `DOOMLICENSE.md` that no longer exists
> anywhere in the tree**; see the DOOM correction above for what happened and
> what the owner still needs to do.

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

#### ClassiCube's preloaded default texture pack is MayteraOS-original, NOT Minecraft's

**#28, 2026-08-17.** Upstream ClassiCube does not bundle its own default
texture pack. At first run it downloads and patches Mojang's official
"classic jar" and "1.6.2 jar" client archives over HTTP
(`vendor/ClassiCube/src/Resources.c`, the `defaultZipSources_0030_0023[]`
table, `http://launcher.mojang.com/mc/game/...`) to build `terrain.png`,
`char.png`, the mob skins, and the rest of `defaultZipEntries[]`. Those
textures are Mojang/Microsoft copyrighted; MayteraOS never had a licence to
redistribute them and does not do so here.

The owner's real iMac14,4 target has no working network path at all (no NIC
this tree drives is present; the onboard Broadcom is unsupported), so that
runtime fetch can never succeed there, and previously left the game silent
about textures or exited early trying. `/GAMES/CLASSICUBE/texpacks/default.zip`
and `/GAMES/CLASSICUBE/texpacks/classicube.zip`, shipped on the ext2 root
partition (see `build/build-golden.sh`), replace that runtime fetch with an
**original, MayteraOS-authored placeholder pack**, generated procedurally by
`userland/apps/classicube/assets/gen-default-texpack.py`:

* `terrain.png`, `particles.png`, `clouds.png`, `rain.png`, `snow.png`,
  `char.png`, the seven mob skins, `icons.png`, `gui.png`, `gui_classic.png`
  and `animations.png` are flat colours, simple gradients and a deterministic
  HSV-wheel palette computed by the script. No pixel is sampled, traced, or
  derived from any Minecraft or Mojang asset, real or reconstructed.
* `default.png` (the bitmap chat/UI font) is the one exception worth calling
  out explicitly: its glyph shapes are re-rasterised straight out of
  `vendor/ClassiCube/src/SystemFonts.c`'s own `font_bitmap[][8]` table, i.e.
  ClassiCube's **own** BSD-3-Clause fallback font data (credited in that file
  as "Goodly's texture pack for ClassiCube"), reformatted from a bit array
  into a PNG atlas. This is a format change of code already covered by the
  BSD-3 licence quoted above, not new third-party content, and it is why chat
  and menu text stays legible with the placeholder pack loaded.
* `animations.txt`'s content is copied verbatim from the `ANIMS_TXT` string
  constant already embedded in `vendor/ClassiCube/src/Resources.c`, which is
  ClassiCube's own file-format documentation, again BSD-3.
* Sound and music are not addressed by this pack: upstream's default sound
  and music assets are extracted from the same Mojang jar as the textures,
  and there is no in-tree fallback asset comparable to `font_bitmap[]` to
  substitute. `engine-patches/coreh-maytera.py` instead defines
  `CC_BUILD_NOMUSIC` and `CC_BUILD_NOSOUNDS` for the MayteraOS platform
  branch, so the resource fetcher never lists audio as missing and the game
  runs silently rather than shipping or faking Mojang audio.

**Licence of the generated placeholder pixel data: CC0 / public domain**,
the same terms as other MayteraOS-authored placeholder assets in this
repository (e.g. `userland/apps/setup/assets/WORLDMAP.BMP`). See
`userland/apps/classicube/assets/README.md` for the full provenance note and
`gen-default-texpack.py` to regenerate it. If a user's machine has working
network access and deletes or replaces these files, ClassiCube's own
unmodified resource-fetch code (untouched by this change) takes over exactly
as upstream intended, subject to Mojang's own terms for that download, which
are between the user and Mojang, not MayteraOS.

## Photography (boot splash / wallpapers)

The boot-splash background (`kernel/boot.bmp`, `kernel/boot_splash.jpg`,
`kernel/video/boot_image_data.c`) and the bundled wallpapers are edited from
**Pexels** stock photography, used under the [Pexels License](https://www.pexels.com/license/)
(free for commercial and non-commercial use, no attribution required). The
MayteraOS lighthouse mark composited onto the splash is original artwork.

If you redistribute any component, retain its in-tree license file and this
attribution.

## Solar System Scope planet/Moon/Sun textures (planetarium, 2026-09-06)

Source: https://www.solarsystemscope.com/textures - 2k equirectangular maps for
the Sun, Moon, Mercury, Venus, Mars, Jupiter, Saturn, Uranus and Neptune.

License: **Creative Commons Attribution 4.0 International (CC BY 4.0)**
(https://creativecommons.org/licenses/by/4.0/), per the Solar System Scope
textures page. Attribution-only, NOT ShareAlike, so it is compatible with this
project's GPLv2 codebase and its CC-BY-SA ban (see the Icons section).

Where these live: NOT as image files. Each map is sphere-mapped offline to a
48x48 RGBA disk and embedded as generated Rust data in
`userland/apps/stellarium/src/solar_tex.rs`; the planetarium draws each body as
a textured disk. The original 2k maps are not redistributed - the embedded disks
are a derivative work under the same CC-BY 4.0 terms, attributed here.

Deliberately NOT used: Stellarium's own planet textures were sourced first, but
its Sun (CREDITS 4.3o) and Jupiter (4.3a) maps are CC-BY-SA, which this project
bans. The Solar System Scope set is CC-BY 4.0 throughout, which is why it was
chosen instead.


## IAU Catalog of Star Names (planetarium star labels, 2026-09-06)

Source: IAU Division C Working Group on Star Names (WGSN), IAU-CSN
(https://www.pas.rochester.edu/~emamajek/WGSN/IAU-CSN.txt). ~442 official proper
star names with coordinates and magnitudes.

License: the IAU-CSN is a factual catalogue of official star names; the names
themselves and their coordinates are not copyrightable. Used as data only,
generated into userland/apps/stellarium/src/star_names.rs (name + RA/Dec/Vmag).

## sdl12-compat SDL 1.2 public headers (userland/libSDL, 2026-09-07, #745)

Covered paths: `userland/libSDL/include/SDL` (the Zlib SDL 1.2 headers) and `userland/libSDL/COPYING` (the SDL 1.2 Zlib licence text). The libSDL src/*.c implementation is first-party MayteraOS code.

Source: github.com/libsdl-org/sdl12-compat, tag `release-1.2.76`, the
`include/SDL/` directory only (SDL.h, SDL_video.h, SDL_events.h,
SDL_keysym.h, SDL_audio.h, and the rest of the public API headers, plus
begin_code.h/close_code.h). These are Sam Lantinga's own clean-room
re-release of the SDL 1.2 public API surface, written to let a project build
against SDL 1.2 headers without needing the original (LGPL 2.1) SDL 1.2
source tree. Copied verbatim (git tag pin, no modifications) except two
files replaced with MayteraOS-specific equivalents, both original to this
project and documented as such in their own header comments:

- `SDL_config.h`: sdl12-compat's own generic config (which probes for
  iconv.h and X11) replaced with one stating exactly what userland/libc
  provides on this OS.
- `SDL_opengl.h`: sdl12-compat's file is a 7000+ line copy of the full
  Khronos GL/GLext headers for a real desktop GL driver; MayteraOS has no
  such driver; this file is a two-line redirect to the TinyGL
  (userland/libgl) GL/gl.h every other TinyGL-backed app already builds
  against, so "what a game can call" and "what TinyGL implements" are the
  same list by construction.

Licence: Zlib (LICENSE.txt at the sdl12-compat repo root; the same terms
are also reproduced verbatim at the top of every copied header). The
project's dr_mp3.h (public domain/MIT-0) is NOT vendored here (it lives in
sdl12-compat's own src/, which this project does not use).

Everything else in userland/libSDL/src/*.c (the backend implementation
itself: SDL_SetVideoMode, the software blit engine, the event pump, threads,
audio, etc.) is original MayteraOS code written against these headers, not
copied from sdl12-compat's own SDL2-backed implementation.

## bsd-games 2.17 terminal games: hangman, worm, robots, snake (tuigames batch, 2026-09-18)

### hangman, worm, robots, snake, from bsd-games 2.17 (`userland/ports/hangman`, `userland/ports/worm`, `userland/ports/robots`, `userland/ports/snake`)

**hangman**, **worm**, **robots** and **snake**, all four from the single shared
`bsdgames_2.17.orig.tar.gz` (Debian's `bsd-games` source package, itself a
repackaging of the NetBSD-derived per-game trees; verified by diffing against
NetBSD src's own `games/hangman` etc. - identical code and header), sha256
`066f924aef6c1c5ea946f588e36f303021f5dfc093944738f025d8edbc6fff60` (measured on
the build host, fetched from `https://snapshot.debian.org/file/8e48a9808908e2898126fdd285196ab71c9a59c0`).
Every source file in these four directories carries the 4.3BSD "Regents of the
University of California" 1983/1993 header, the permissive 3-clause BSD text,
recorded as **BSD-3-Clause**, reproduced in full and verbatim from the
tarball's top-level `COPYING` (the section covering these four games, not the
whole file - `COPYING` also covers other bsd-games titles under other
licences, e.g. GPL, which this batch does not touch):

```
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. Neither the name of the University nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.
```

Copyright (c) 1980-1993 The Regents of the University of California. All
rights reserved.

Ships as `/APPS/HANGMAN`, `/APPS/WORM`, `/APPS/ROBOTS`, `/APPS/SNAKE`. Each
statically combines this BSD-3-Clause code with the permissively-licensed
ncurses port and userland/libc; BSD-3-Clause is permissive, so none of the
four binaries carries a copyleft obligation. Per-port adaptation notes
(setgid-drop removal, built-in word list replacing hangman's on-disk
dictionary, `/HOME`-rooted scorefiles for robots/snake) are documented in each
port's own `userland/ports/<name>/PORT` file and patches.

## 2048 (mevdschee/2048.c) (tuigames batch, 2026-09-18)

### 2048 (`userland/ports/2048`)

**2048** (`userland/ports/2048`), Maurits van der Schee's single-file
terminal 2048, from `LICENSE` of the GitHub commit archive pinned at
`afc8898691f54d43309497f4c32682fe90bb5f57`, sha256
`7fcd88c09c38cbecdf6cbd47ef88787d0a42b2f7e36ad97a6bfbf3ae114db38c` (measured
on the build host). **MIT License**, reproduced in full and verbatim:

```
The MIT License (MIT)

Copyright (c) 2024 Maurits van der Schee

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Ships as `/APPS/TUI2048`. No patches: it compiled against userland/libc
unmodified. MIT is permissive; no copyleft obligation.

## Frotz 2.44, dumb/terminal interface only (tuigames batch, 2026-09-18)

### Frotz 2.44 (`userland/ports/frotz`)

**Frotz 2.44** (`userland/ports/frotz`), the David Griffith-maintained
Z-machine interpreter, from `COPYING` of the tag-2.44 GitHub archive
tarball, sha256
`dbb5eb3bc95275dcb984c4bdbaea58bc1f1b085b20092ce6e86d9f0bf3ba858f` (measured
on the build host). **GPL-2.0-or-later**, same text as reproduced above for
ninvaders (this tarball's own `COPYING` is the identical GNU GPL v2 text).

Ships as `/APPS/FROTZ`, built from only `src/common` (the portable Z-machine
core) and `src/dumb` (the plain-text terminal frontend) - `src/curses`,
`src/sdl`, `src/blorb`, `src/x` and `src/dos` are not built. No story file is
bundled (matches the `userland/apps/gbemu` no-ROM precedent): the interpreter
takes a story file path on argv. Statically combines this GPL-2.0-or-later
code with permissively-licensed userland/libc; the combined `/APPS/FROTZ`
binary as a whole is offered under GPL-2.0-or-later, the same shape as the
ninvaders/moon-buggy precedents above. userland/libc is not thereby made GPL.

## open-adventure (Colossal Cave Adventure) (tuigames batch, 2026-09-18)

### open-adventure (`userland/ports/openadv`)

**open-adventure** (`userland/ports/openadv`), Eric S. Raymond's modernised
C99 forward-port of the original Crowther/Woods Adventure 2.5 ("the last
version in the main line of Adventure development written by the original
authors", who gave permission and encouragement for this release - per
upstream's own README), from `COPYING` of the GitLab commit archive pinned at
`e6160529e70a8c7a06b9a5f190d69aa35c4c62f5` (project `gitlab.com/esr/open-adventure`,
id 3339205), sha256
`7997b8cc8150ff8a9ae4cfa72c199bbeea9ac799a9992055af05fb57ca754ab1` (measured
on the build host). **BSD-2-Clause**, reproduced in full and verbatim:

```
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

Ships as `/APPS/ADVENTURE`. The puzzle/movement database (`adventure.yaml`) is
compiled to `dungeon.c`/`dungeon.h` at build time by upstream's own
`make_dungeon.py` (needs PyYAML on the build host - a build-tool dependency,
not linked into the target binary). Statically combines this BSD-2-Clause
code with the permissively-licensed libedit, musl-regex and ncurses ports and
userland/libc; BSD-2-Clause is permissive, so the combined `/APPS/ADVENTURE`
binary carries no copyleft obligation. One patch renames a static verb-handler
function (`read` -> `verb_read` in `actions.c`) that collided with
userland/libc's own `read()` declaration; documented in the port's own PORT
file and patch.

## NetHack (verified present, not re-ported) (tuigames batch, 2026-09-18)

NetHack ships already, as a DOS binary run under the DOS/4GW bridge
(`/DOS/NETHACK/NETHACK.EXE`, `build/assets/nethack`, menu entry in
`build/assets/startmenu/system.d/03-games.MENU`). Verified present before
starting this batch (per the batch's own instructions: ground first, land
nothing that already exists). Not re-ported; no changes made to it.
