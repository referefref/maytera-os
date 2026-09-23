# MayteraOS Office app icons (Writer, Sheets, Slides) + suite mark

Original MayteraOS artwork, drawn for the native office suite's three
launcher entries. No third-party geometry: nothing here needs attribution
beyond this repo's own licence (recorded in `../../../ATTRIBUTION.md`).

| App (Start-menu label, binary) | SVG master (this dir) | Raster shipped | Colour |
|---|---|---|---|
| Writer, `/APPS/WRITER` | `WRITER.svg` | `/ICONS/WRITER.ICN` | blue `#4da3ff`, text lines |
| Sheets, `/APPS/SHEETS` (the suite's spreadsheet; the brief calls it "Calc", but `CALC.ICN` already belongs to the Calculator) | `SHEETS.svg` | `/ICONS/SHEETS.ICN` | green `#3fd17a`, header row + cell lattice |
| Slides, `/APPS/SLIDES` | `SLIDES.svg` | `/ICONS/SLIDES.ICN` | orange `#ff9f40`, solid slide on easel legs |
| Office suite mark (family badge, not a launcher) | `OFFICE.svg` | `/ICONS/OFFICE.ICN` | the three colours stacked |

`*.png` next to each SVG is the 64x64 raster the `.ICN` was packed from, kept
here so the set can be reviewed in any image viewer. The `.ICN` binaries are
NOT in this repo (same rule as every other icon, see `../../icons/README.md`):
they live in the asset base.

## Family design

- **Sheet outline**: white, 4px stroke on the 64px canvas. That is the
  CoreUI weight the rest of the launcher set uses (CoreUI draws 32/512, which
  is 4/64), so the three sit next to Notes, Calculator, Editor as one set.
  No background tile (the rule that made the old AI-generated Paint icon
  stand out as wrong).
- **Shared suite cue**: the Maytera mountain-M, the two peaks of the logo
  mark, drawn as a 3px white polyline across the top of the sheet. Identical
  on all four files; it is what makes them read as a family rather than
  three unrelated document icons.
- **Colour only on the content**, never on the outline. This matters for
  the Marble dock: `icon_draw_dock_icon()` (`userland/apps/compositor/icons.c`)
  recolours every icon to a single ink using the source's alpha-weighted
  LUMINANCE as coverage, so a white outline keeps full weight in the dock
  while the blue/green/orange content drops to roughly 55-70% ink. The three
  colours were picked with that in mind (luminance 147/155/178 of 255), so
  they land at similar strength rather than one app fading out.
- **Judged at real size**, not 1:1: the four were rendered through a
  box-filter downsample (the same sampling `color_icon_sample_box()` does)
  at 40px (dock, `XFCE_DOCK_ICON`), 20px (Start menu default `icon_size`) and
  16px (`TB_ICON_SZ`, taskbar and title bar) on dark and light grounds. The
  Slides glyph is a SOLID slide for exactly that reason: an outlined slide
  at 3px collapsed into a blob at 16px. Its title bar is an evenodd cut-out,
  not a painted dark bar, so it works on any backdrop and in the tint path.

## Pipeline mechanism matched (how launcher icons actually work here)

There is no per-app icon file lookup. The compositor loads a fixed table of
64x64 MICO rasters at startup and every surface draws by icon id:

1. `assets/icons/svg/NAME.svg` is the pipeline's SVG location; the four
   files there are symlinks to the masters in this directory, so there is
   one source. Rasterize ONCE, straight to the 64x64 target:
   `rsvg-convert -w 64 -h 64 --background-color=none NAME.svg -o NAME.png`
   then pack with `tools/icons/pack_icn.py NAME.png NAME.ICN --expect 64x64`
   (stdlib-only; magic `MICO`, u32 LE width, u32 LE height, then BGRA
   pixels, matching `icon_load_color()` in `icons.c`).
2. The `.ICN` goes to ext2 `/ICONS/NAME.ICN` on the boot volume (the
   image's ext2 ROOT partition p2, not the FAT ESP). Two copies of record:
   - the private git-LFS asset repo `the build container:<workspace>`
   - the b850 asset base image `the build host:/ssdmirror/maytera-2part-ext2-b850.img`
     (p2, `/ICONS/`), which `build/build-golden.sh` copies and overlays
     binaries onto. `build-golden.sh` does NOT regenerate icons from SVG,
     so a new `.ICN` must be written into the base by hand (done for these
     four on 2026-09-07) or it never ships.
   - `build/asset-manifest.sha256` carries their sha256 as FREEWARE rows.
3. `userland/apps/compositor/main.c` calls
   `icon_load_color(ICON_<ID>, "/ICONS/NAME.ICN")` once per id.
4. Start menu entries name the icon by a string: `icon=<name>` in
   `build/assets/startmenu/system.d/02-accessories.MENU` (the "Office"
   category), mapped by `sm_icon_by_name()` in
   `userland/apps/compositor/startmenu.c`.
5. Taskbar buttons, the dock and the window title bar do NOT need their own
   wiring: `tb_icon_for_window()` (`taskbar.c`) resolves a running window's
   kernel `app_id` (binary basename) back to its Start-menu entry via
   `startmenu_find_by_app_id()` and uses that entry's `icon_id`.

## Wiring these icons in (for the implementation agents; not done here)

Pure asset delivery: no C or `.MENU` edit was made in this change. To make
the three entries use the new art:

- `userland/apps/compositor/compositor.h`, enum `icon_id_t`: APPEND
  `ICON_WRITER, ICON_SHEETS, ICON_SLIDES, ICON_OFFICE` after
  `ICON_PLANETARIUM` and before `ICON_COUNT`. Append, never insert: the
  numeric values are persisted on disk (desktop icon keys, UIPROFIL.YML).
- `userland/apps/compositor/main.c`, next to the other `icon_load_color()`
  calls:
  `icon_load_color(ICON_WRITER, "/ICONS/WRITER.ICN");`
  `icon_load_color(ICON_SHEETS, "/ICONS/SHEETS.ICN");`
  `icon_load_color(ICON_SLIDES, "/ICONS/SLIDES.ICN");`
  `icon_load_color(ICON_OFFICE, "/ICONS/OFFICE.ICN");`
- `userland/apps/compositor/startmenu.c`, `sm_icon_by_name()`: add
  `"writer"`, `"sheets"`, `"slides"` (and `"office"`) returning those ids.
- `build/assets/startmenu/system.d/02-accessories.MENU`, category Office:
  change `icon=notes` / `icon=calculator` / `icon=highlight` on the Writer /
  Sheets / Slides lines to `icon=writer` / `icon=sheets` / `icon=slides`.
- Nothing else: taskbar, dock and title bar follow the Start-menu entry.

If a golden is built from a base that lacks the `.ICN` files, the ids fall
back to nothing being drawn (`icon_draw_scaled()` returns for a color-only
id with no raster, the #pubboot fix), never a crash; the entries would show
no icon until the asset base carries them.

## Splash / wordmark

Skipped deliberately. The office shell (`userland/officelib/include/officeui.h`)
has no splash, logo or about-box hook, and no shipped userland app draws a
splash, so there is no pipeline for one to drop into. If a suite-level
about box is added later, `OFFICE.svg` is the mark to use.
