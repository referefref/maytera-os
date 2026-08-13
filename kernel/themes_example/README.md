# DO NOT USE THIS DIRECTORY AS A TEMPLATE (#711)

The four `theme.ini` files here are `[Section]`-INI samples for
`kernel/gui/theme.c`, the OLD theme engine. **Nothing in the live golden reads
them.** That engine only runs on the kernel's fallback desktop, which is only
reached if `/APPS/COMPOSIT` fails to launch, and even then it reads
`/THEMES/<name>/theme.ini`, a path no shipped image contains.

A designer who finds this directory first will author the wrong format. That is
why this file exists.

## The real theme format is mtheme v2

- **Files:** `build/assets/themes/<slug>.mtheme`, one file per theme, one file
  IS the complete theme. Flat `key=value`, no sections, no nesting, no quoting.
- **Order:** `build/assets/themes/INDEX.TXT`, one filename per line. **The order
  is load-bearing** (it assigns each theme's numeric id). `retro_unix.mtheme`
  must stay line 1 = index 0. Append, never insert.
- **Active theme:** `/CONFIG/THEME.CFG`, `active=<slug>`, written by userland
  (`userland/libc/gui_theme.c`) and by nothing else.
- **Readers:** kernel `gui/themes.c` (one parser, colours + metrics) feeding
  both the kernel window decorator and, over `SYS_THEME_COLOR` /
  `SYS_THEME_METRIC`, every userland app. There is no second format.
- **Contract check:** `build/assets/theme-scale-lint.sh` (and
  `--self-test`). Commit-time only; nothing at runtime reads it.
- **Spec:** `docs/UI_STYLE_GUIDE.md`, section "mtheme v2".

To add a theme: copy `build/assets/themes/maytera_dark.mtheme`, edit it, append
its filename to `INDEX.TXT`, and run the lint.
