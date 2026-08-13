# convert - changelog

## 2026-07-12 - initial release
- New utility app: Unit Converter. Fills a real gap: calc and bc do
  arithmetic only; the OS had no way to convert between units.
- Eight categories: Length (8 units), Mass (7), Temperature (C/F/K),
  Data (bit..TB, binary multiples), Speed (5), Time (6), Area (7),
  Volume (7, US customary). 49 units total.
- All conversions are scale+offset to a per-category base unit, so
  temperature (affine) shares the same code path as everything else.
- Layout: category sidebar, From/To value cards with live result and a
  "1 from = X to" rate hint, swap button, two unit-selection columns,
  and an on-screen keypad (digits, dot, sign, clear, backspace, swap)
  so the app is fully usable with mouse only.
- Full keyboard input: digits and '.', Backspace, C or Esc clear,
  '-' toggles sign, S or Tab swaps units, Up/Down change category.
- Freestanding: carries its own small decimal parser and formatter
  (no libm, no %f in the userland printf), same approach as apps/calc.
  Up to 11 significant digits, scientific notation for extremes.
- Styled per docs/UI_STYLE_GUIDE.md: theme-following palette (Dark,
  Light, Classic, Ocean, Nord), shared style-engine widgets, antialiased
  TTF text, live resize reflow, hover states everywhere.
- No new kernel work needed; event loop blocks on win_get_event.
