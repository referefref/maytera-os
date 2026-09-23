# SLIDES toolbar icons (native office suite)

Original MayteraOS artwork (no third-party paths; nothing to attribute beyond
this repo). Twenty toolbar glyphs for the SLIDES presentation app, drawn as
one family so they sit next to the CoreUI-derived Files/Settings toolbar
glyphs (`CHEVL`/`CHEVR`/`CHEVU`, `COPY`, `PASTE`, ...) without a visible
change of hand: same 32-unit line weight in a 512 viewBox, white on
transparent, no background tile.

## Mechanism this drops into

Toolbars in this OS do not load SVGs. The Files app pattern
(`userland/apps/files/main.c`, `files_icon_btn()` -> `draw_mico()`) opens
`/ICONS/<NAME>.ICN`, a 64x64 MICO raster (`assets/icons/README.md`), and
nearest-samples it down to 16px inside a 26x24 `gui_button`, using the
white glyph's luminance times alpha as coverage and tinting it with the
toolbar's ink colour. So the shipped artefact is the `.ICN`, and the SVGs
here are the masters it is regenerated from.

Because the downscale is a nearest sample (one source pixel per target
pixel, no filtering), every line in these masters is exactly one 32-unit
cell wide on a 16x16 grid (centre at 32k+16, ends on multiples of 32, butt
caps, mitre joins). At 16px that lands one fully-covered pixel per line,
which is what keeps the set crisp instead of dropping strokes. At 20-24px
lines alternate 1-2px, the same behaviour the existing CoreUI glyphs have at
those sizes. `gen_icons.py` is the geometry source; edit it and re-run
rather than nudging path data by hand, so the grid stays intact.

## Files

| Master (SVG)         | `.ICN` (deploy to `/ICONS/`) | Shared with Writer/Calc? |
|----------------------|------------------------------|--------------------------|
| new.svg              | `OFNEW.ICN`                  | yes (page + plus)        |
| open.svg             | `OFOPEN.ICN`                 | yes                      |
| save.svg             | `OFSAVE.ICN`                 | yes                      |
| new-slide.svg        | `SLNEW.ICN`                  | no                       |
| duplicate-slide.svg  | `SLDUP.ICN`                  | no                       |
| delete-slide.svg     | `SLDEL.ICN`                  | no                       |
| slide-layout.svg     | `SLLAYOUT.ICN`               | no                       |
| text-box.svg         | `OFTEXTBX.ICN`               | yes                      |
| insert-image.svg     | `OFIMAGE.ICN`                | yes                      |
| insert-shape.svg     | `OFSHAPE.ICN`                | yes                      |
| bold.svg             | `OFBOLD.ICN`                 | yes                      |
| italic.svg           | `OFITALIC.ICN`               | yes                      |
| align-left.svg       | `OFALIGNL.ICN`               | yes                      |
| align-center.svg     | `OFALIGNC.ICN`               | yes                      |
| align-right.svg      | `OFALIGNR.ICN`               | yes                      |
| bullet-list.svg      | `OFBULLET.ICN`               | yes                      |
| prev-slide.svg       | `SLPREV.ICN`                 | no                       |
| next-slide.svg       | `SLNEXT.ICN`                 | no                       |
| present-play.svg     | `SLPLAY.ICN`                 | no                       |
| notes.svg            | `SLNOTES.ICN`                | no                       |

Prefix rule: `OF*` = generic office glyph any suite app may reuse, `SL*` =
slide-specific. All names are 8.3 uppercase like the rest of `/ICONS/`, and
none collides with an existing icon (`NOTES.ICN` is the Notes app's icon,
which is why speaker notes is `SLNOTES`).

## Regenerating

```
assets/office-icons/slides/build.sh     # -> assets/office-icons/slides/icn/*.ICN
```

`rsvg-convert -w 64 -h 64 --background-color=none` straight from each SVG,
then `tools/icons/png2icn.py` packs the PNG into MICO. The script checks
every output is exactly 12 + 64*64*4 bytes with the `MICO` magic.

## Deploying

The build does not copy icons from the repo: `/ICONS/` lives on the ext2
root partition (p2) of the static asset base
(`/ssdmirror/maytera-2part-ext2-b850.img`, see `assets/icons/README.md`,
`build/gen-asset-manifest.sh`). Copy `icn/*.ICN` into that image's `/ICONS/`
(loop-mount p2, `cp`, `umount`, `losetup -d`) and the next golden carries
them. Until then an app that asks `draw_mico("SLNEW", ...)` gets a 0 return
and should fall back to a label, exactly as Files falls back to
`draw_arrow()`.

## Judging changes

Always look at the result through the real downscale, never at 64px:
render the `.ICN`/PNG with the nearest-sample + luminance-tint algorithm
from `draw_mico()` at 16px and 24px, zoomed 8x with no smoothing, side by
side with `CHEVR`/`NOTES` from `assets/icons/svg/` as weight references.
