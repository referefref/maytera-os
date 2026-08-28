# Icon assets

`.ICN` files are the compositor's MICO format: magic `MICO` + u32 width +
u32 height + width*height ARGB(0xAARRGGBB) little-endian pixels. They deploy to
`/ICONS/*.ICN` on the boot disk and are loaded by `icon_load_color()`.

**`.ICN` binaries are not in this git repo.** They live in the static asset
base image (out of git by design, see the top-level `CLAUDE.md`/`BUILD.md`);
`build/build-golden.sh` overlays freshly-built binaries onto that base but does
not regenerate icons from `assets/icons/svg/*.svg` on every build. If you
change an SVG here, you must also rasterize it and write the resulting
`.ICN` into the asset base's `/ICONS/` yourself (see "Regenerating an icon"
below), or the fix only ever lands in the next hand-built golden, not
automatically.

## CANONICAL TREATMENT (rewritten 2026-08-12, #745 - supersedes the
"stroke-width 6/5/4.5" convention below, which described the OLD generated
set and no longer applies to most of the icon set)

**As of 2026-08-12, most app/dock/tray icons are sourced from CoreUI Icons
Free (`cil-` prefix, CC BY 4.0) and one from Boxicons (`bx-wrench`, CC BY
4.0) - see `../../ATTRIBUTION.md` for the full per-icon source table and the
reasoning for every icon that was deliberately kept bespoke instead.**

**DO NOT retrofit CoreUI/Boxicons icons to the old hand-authored house
stroke convention (stroke-width 6 primary / 5 secondary / 4.5 fine-accent).
That convention was a rule this project invented for HAND-DRAWN glyphs to
keep them internally consistent with each other; it has no meaning applied
to a professionally-designed library icon, and editing a CoreUI path's
stroke geometry would both violate "recolor only, geometry unmodified in
form" (the same principle this project has followed for third-party SVGs
since the original SVG Repo batch) and reintroduce, by hand, the exact
per-icon drift bug this swap exists to eliminate.**

The new rule is simpler:

- **Take the upstream SVG's native path geometry and viewBox unmodified.**
  CoreUI ships at `viewBox="0 0 512 512"`; Boxicons ships at
  `viewBox="0 0 24 24"`. Do not rescale or redraw the path data to force a
  64x64 viewBox - it is unnecessary. Rasterization (see below) scales
  proportionally to the target pixel size regardless of the source viewBox,
  so this is a single clean scale at import, never a second lossy pass.
- **Recolor to solid white only.** CoreUI paths use
  `fill="var(--ci-primary-color, currentcolor)"`; replace that exact string
  with `fill="#ffffff"`. Boxicons paths have no fill attribute (default
  black); add `fill="#ffffff"` to the root `<svg>` element so it is
  inherited. Do not touch stroke-width, since these icon families are
  constructed as filled outline shapes (no literal `stroke` attribute) -
  there is no stroke-width to tune in the first place.
  White-on-transparent only, no background badge/tile (unchanged rule from
  the old convention, and still correct - a background tile is what made the
  2026-07-11 AI-generated Paint icon stand out as wrong against the rest of
  the set; see `../../ATTRIBUTION.md`).
- **Rasterize ONCE, straight from the vector source to the exact 64x64
  target canvas** (`rsvg-convert -w 64 -h 64 --background-color=none`),
  never from an intermediate raster at a different size. This is the
  platform's general rule (scale once at import, never per-frame, never
  re-derived from a differently-sized raster) and is exactly what the
  documented 72->24 downscale defect violated historically.
- **A small number of icons are still original MayteraOS hand-drawn line
  art and are NOT being converted**: the per-game icons (`ARENA.svg`,
  `CHESS.svg`, `SQUADRON.svg`, `GLCUBE.svg`, `GLMATRIX.svg`) and
  `PRINT3D.svg`. For THESE FILES ONLY, the old hand-authored convention still
  applies and is preserved below for reference. Do not apply it to any
  CoreUI/Boxicons-sourced file.
- **Judge every icon at the real on-screen size, not at 1:1.** The
  compositor draws favorites-dock icons at 40px (`XFCE_DOCK_ICON`,
  `userland/apps/compositor/taskbar.c`) using its own nearest-sample scaler
  (`icon_draw_scaled`/`color_icon_blit` in
  `userland/apps/compositor/icons.c`); render through that same algorithm
  (or the genuine compositor, via a throwaway VM) at 8-10x zoom before
  judging any icon change. This is unchanged from the old convention and is
  still the single most important rule in this file - it is what caught
  the 2026-08-12 Editor/Calculator/App Store stroke-drift bug in the first
  place, and it is what proved the CoreUI replacements read correctly at
  true size before they shipped.

### Legacy hand-authored convention (applies ONLY to `ARENA.svg`, `CHESS.svg`,
`SQUADRON.svg`, `GLCUBE.svg`, `GLMATRIX.svg`, `PRINT3D.svg` - do not apply to
CoreUI/Boxicons-sourced files)

- Canvas: **64x64** px (three unrelated small utility glyphs -
  `BROWSER.ICN`, `IRC.ICN`, `NETWORK.ICN` - shipped at 48x48 before this
  task's CoreUI swap replaced all three; if you touch a still-bespoke icon
  that predates this note, re-measure rather than assume 64x64). **UPDATE
  (2026-08-12, same day): `BROWSER.ICN` was reverted (see
  `../../ATTRIBUTION.md`'s "Reverted" section) and is 48x48 again -
  `IRC.ICN`/`NETWORK.ICN` were NOT reverted and stay the new 64x64 CoreUI
  rasters. Do not assume all three still match each other's dimensions.
- SVG `viewBox="0 0 64 64"`, authored directly at target size.
- Primary stroke: `stroke-width="6"`, `stroke="#fff"` (or `#ffffff`),
  `fill="none"`, `stroke-linecap="round"`, `stroke-linejoin="round"`.
- Secondary/interior lines: `stroke-width="5"`.
- Fine accents (checkmarks, short ticks): `stroke-width="4.5"`.
- Small filled accent shapes (dots, tabs, digit keys): solid `fill="#fff"`,
  no stroke.
- Content generally spans about 75-85% of the canvas on its longest axis,
  roughly centered.

## Regenerating a CoreUI/Boxicons-sourced icon

```
# 1. Recolor (see rules above - CoreUI shown; Boxicons needs the <svg fill=...> form instead)
sed 's/fill="var(--ci-primary-color, currentcolor)"/fill="#ffffff"/' cil-name.svg > assets/icons/svg/NAME.svg

# 2. Rasterize straight to the 64x64 target, transparent background, single scale
rsvg-convert -w 64 -h 64 --background-color=none assets/icons/svg/NAME.svg -o g.png

# 3. Pack g.png (RGBA) -> MICO: magic 'MICO' + u32 width (LE) + u32 height (LE) +
#    width*height pixels, each a little-endian u32 0xAARRGGBB (on-disk byte
#    order B,G,R,A - matches icons.c's icon_load_color()/color_icon_blit()
#    exactly; verify against that C code before trusting a byte-order
#    assumption, do not re-derive it from prose alone)
```

## Mirroring an existing icon (#123)

When the ONLY change wanted is "the same icon, the other way round", do NOT
re-rasterize. A horizontal mirror of a raster is a pure permutation of its
pixels: every antialiased edge value is carried across unchanged, so the
result has exactly the weight and coverage the shipped icon had.
Re-rasterizing instead puts the glyph through whatever SVG renderer happens
to be installed on the machine doing the work, which is NOT the renderer that
produced the shipped set, and silently re-derives every edge. For a change
whose entire content is a mirror, that is a needless risk.

```
tools/icons/mirror_icn.py <asset-base>/ICONS/NAME.ICN /tmp/NAME.mirrored.ICN
```

It self-checks that mirroring twice is the identity, so a format or indexing
mistake fails loudly instead of shipping a subtly wrong icon nobody looks at
closely.

Mirror the SVG source in the SAME change so a future full regeneration lands
in the same place: wrap the untouched path in a
`<g transform="translate(W,0) scale(-1,1)">` (W = the viewBox width) rather
than rewriting any coordinate, which keeps the "upstream path geometry
unmodified" rule above intact. If the source is third-party, record the
modification in `../../ATTRIBUTION.md`: CC BY 4.0 requires that changes be
indicated. `APLAYER.svg` / `APLAYER.ICN` is the worked example (#123: the
music note pointed the wrong way).

## Regenerating a bespoke (original-artwork) icon

Pipeline (white glyph, transparent bg; the compositor adds the drop shadow):
```
rsvg-convert -w 64 -h 64 in.svg -o g.png
convert g.png -channel RGB -evaluate set 100% +channel w.png
# pack w.png (RGBA) -> MICO, same format as above
```
Use **lineart (stroke, fill=none)** SVGs; solid-silhouette SVGs become white
blobs with no internal detail. `recycle.svg` in this directory briefly went
UNUSED when 2026-08-12's CoreUI swap pointed `RECYCLE.ICN` at `cil-trash.svg`
instead, then went back to being the live source for `RECYCLE.ICN` the same
day when Computer/Browser/Recycle Bin were reverted per user preference (see
`../../ATTRIBUTION.md`'s "Reverted" section) - it is, and has always been,
the original custom hand-drawn trash can, not third-party. See
`../../ATTRIBUTION.md` for the full source table of every currently-shipped
icon, third-party or original.
