# MayteraOS Theme Files (.mtheme)

This is the authoritative reference for the on-disk theme format, where
themes live, how a theme goes from download/edit to on-screen pixels with
no reboot, and what happens when a theme is broken. `docs/UI_STYLE_GUIDE.md`
is the authoritative *design* spec (palette, typography, spacing, component
rules); this document is the authoritative *file format and mechanism*
spec. Kernel source of truth: `kernel/gui/themes.c` /
`kernel/gui/themes.h`. Userland loader: `userland/libc/gui_theme.c` /
`gui_theme.h`.

## A note on "YAML"

The brief for this work asked for themes "defined in YAML." What ships is
not YAML: it is a flat `key=value` text format, one key per line,
`#`/`;` comments, blank lines ignored. This was a deliberate choice made
at #565 and kept here rather than replaced, for three reasons that are
still true:

1. **The parser has to run in Ring 0 on untrusted input.** A theme file is
   downloadable and user-editable by design (App Store "type=theme"
   packages, hand-edited files, both loaded straight into the kernel via
   `SYS_THEME_LOAD_FILE`). The kernel target is freestanding, `no_std`
   where Rust is used, and has no general-purpose parsing library. A real
   YAML grammar (nesting, anchors/aliases, block/flow scalars, multiple
   quoting rules) is a meaningfully larger attack surface to get right in
   that environment than a dumb bounded line reader that recognizes
   `key=value` and nothing else. The line tokenizer this format uses has a
   Rust port behind `-DRUST_THEME_PARSE` specifically because it handles
   untrusted bytes (see that file's header comment); a YAML grammar would
   need the same treatment at several times the surface.
2. **The substantive requirements are already met by this format.** Themes
   are files (`/THEMES/<slug>.mtheme`), they are downloadable (App Store
   install path), they are hand-editable in any text editor, they support
   a structured, hierarchical-*looking* key namespace via dotted prefixes
   (`color.*`, `state.*`, `metric.*`, `radius.*`, `decor.*`, `type.*`,
   `space.*`, `shadow.*` - see below), and a live edit applies with no
   reboot (see "Live apply" below). Nothing a theme author or the App
   Store client needs is blocked by the format being flat text instead of
   YAML.
3. **Every other "structured" file in this tree that is named `.yaml`/
   `.yml` (e.g. `userland/apps/compositor/TRAYMENU.YAML`) is parsed by the
   same kind of flat line reader, not a real YAML grammar.** There is no
   YAML parser anywhere in this kernel or userland tree today. Adopting
   real YAML for themes alone would mean introducing the *only* YAML
   parser in the codebase for one subsystem, which is its own maintenance
   liability without a matching benefit.

If a real YAML parser is added to this tree for other reasons in the
future, migrating `.mtheme` onto it is a reasonable follow-up. Until then,
this document treats `.mtheme` as the answer to the "YAML" requirement and
says so explicitly rather than silently reinterpreting the brief.

## Where themes live

```
/THEMES/
  INDEX.TXT              one .mtheme filename per line, in load order -
                          this ORDER is the numeric theme index every
                          syscall (SYS_SET_THEME, SYS_THEME_COLOR, ...)
                          uses, so it must agree between kernel boot and
                          the userland picker (both read this same file)
  retro_unix.mtheme
  dark.mtheme
  maytera_dark.mtheme
  ...                     one flat file per theme, no per-theme directory
```

`/CONFIG/THEME.CFG` (system default) and the per-user copy under the
`userconf` home (`THEME.CFG`, read first, falls back to the system copy -
see `userland/libc/userconf.c`) hold a single line, `active=<slug>`,
naming which theme is active for that user. This is the only piece of
theme *selection* state; the theme *data* is always the `.mtheme` file
itself.

A theme is NOT a directory with sibling wallpaper/icon/sound folders -
that was the pre-#565 design (still visible as dead, uncalled code in
`kernel/gui/theme.c`/`theme_parser.c`; see "A dead second theme engine"
below) and does not describe anything the live loader reads.

## File format

One theme = one `/THEMES/<slug>.mtheme` file. `<slug>` (the filename minus
`.mtheme`) is also what `THEME.CFG` stores and what the App Store's
`type=theme` package manifest names as its install target.

```ini
name=Retro UNIX
author=MayteraOS
version=1
style=retro
dark=0
titlebar_active=0x00336666
window_bg=0x00b4b4b4
label_text=0x00000000
... (51 legacy color.* keys total, see kernel/gui/themes.c g_theme_fields[])

# --- mtheme v2 (#711) namespaced tokens ---
color.surface=0x00B4B4B4
color.on_surface=0x00000000
color.accent=0x00336666
state.btn_rest_bg=0x00C8C8C8
state.btn_rest_fg=0x00000000
state.item_selected_bg=0x00336666
state.item_selected_fg=0x00FFFFFF
metric.pad=10
radius.btn=0
decor.style=beveled
type.body=14
type.body_lineheight=20
type.body_weight=regular
space.md=12
shadow.blur=12
lint.baseline=legacy-v1   # only on themes that predate v2, see below
```

Rules:

- One `key=value` per line. `#` or `;` starts a comment (whole-line or
  trailing). Blank lines ignored. No quoting, no escaping, no multi-line
  values.
- Colors are `0xRRGGBB` or `0x00RRGGBB` (the leading `00` byte is
  accepted and ignored; there is no alpha channel).
- Integers (`metric.*`, `radius.*`, `type.*` sizes) are bounded signed
  decimals, or one of a small fixed word set (`beveled`/`flat`/`gradient`
  for `decor.style`; `regular`/`bold` for `*_weight`; `yes`/`no`/`on`/
  `off`/`true`/`false` for a handful of boolean-ish fields).
- **Unknown keys are ignored**, not an error. This is what keeps an old
  theme file forward-compatible with a newer kernel that understands more
  keys, and a theme written for a newer kernel loadable (with the new
  keys silently dropped) on an older one.
- **A key a file does not set is not zero and not undefined.** See "Two
  layers of defaulting" below.

### Two layers of defaulting

A `.mtheme` file only has to specify the 51 legacy color keys to render
completely. Everything else is filled in, in order:

1. **Baseline.** Parsing starts from a copy of the built-in, known-good
   Retro UNIX palette (`g_fallback_theme`), not zeroed memory. A minimal
   or truncated file that sets almost nothing still renders as a complete,
   readable theme (Retro UNIX's colors) rather than black-on-black.
2. **v2 derivation.** Any `color.*`/`state.*`/`metric.*`/... key the file
   does not set is derived from the closest legacy field with the same
   meaning (`theme_fill_v2_defaults()` in `kernel/gui/themes.c` - e.g.
   `color.surface` defaults to `window_bg`, `state.btn_rest_fg` defaults
   to `button_text`). A file written against the pre-v2 (#565) format
   still produces a complete, correct v2 theme with zero edits.

### The v2 namespace, by prefix

| Prefix | What | Consumer today |
|---|---|---|
| `color.*` | Semantic surface/accent/border/titlebar tokens | kernel window chrome, `theme_get_color_by_id` |
| `state.*` | Six explicit states (rest/hover/active/focus/disabled/selected) per control family (btn/item/input) | partially wired, see `docs/UI_STYLE_GUIDE.md` for per-family status |
| `metric.*` | Geometry in px (was `#define`s in `window.h`/`gui_style.h`) | kernel window chrome, `SYS_THEME_METRIC` |
| `radius.*` | Corner radius, fixed scale `0/3/4/6/10` | window/button/input/menu/card decoration |
| `decor.*` | `beveled\|flat\|gradient` style + gradient/shadow/grip flags | window chrome |
| `type.*` | Type scale (`11/14/16/20/28`), lineheight, weight | text rendering (partial - see UI_STYLE_GUIDE 4.6) |
| `space.*` | 4px spacing grid (`4/8/12/16/24/32`) | partial, `metric.pad`/`metric.gap` are the wired subset |
| `shadow.*` | Window/modal elevation shadow | window chrome (gradient-style themes only) |
| `lint.baseline` | `legacy-v1` marks a pre-#711 theme as grandfathered from the design-contract lint below | `build/assets/theme-scale-lint.sh` only, not read at runtime |

## Two different checks, two different jobs - do not confuse them

**`build/assets/theme-scale-lint.sh`** is a commit-time, offline check
against the 14 themes shipped in `build/assets/themes/`. It does real
gamma-corrected WCAG relative-luminance math (needs floating point, which
is why it is a host-side script and not kernel code - see the kernel
"soft-float, SSE disabled" hard limit in `CLAUDE.md`) and enforces AA
4.5:1 on a fixed list of pairs, plus the radius/spacing/type scales.
Nothing on a running MayteraOS image reads this script or depends on it;
it exists so the themes this project ships don't quietly drift off their
own published scales. It cannot see a theme that arrives later (App Store
download, hand-edited file) at all.

**The runtime contrast floor** (`theme_ensure_all_contrast()` /
`theme_ensure_v2_contrast()` in `kernel/gui/themes.c`) runs on *every*
theme parse, unconditionally - built-in themes at boot and any theme
loaded at runtime via `SYS_THEME_LOAD_FILE` (Settings' "browse a custom
theme", the App Store install path, and a live file edit picked up by the
~2s poll - see "Live apply" below). It is integer-only (Rec. 601 luma,
`(R*299+G*587+B*114)/1000`), a coarse proxy for WCAG rather than the real
formula, and deliberately generous (a 60/255 luma-delta floor, ~24%) so it
never overrides a theme author's deliberate, readable-but-subtle contrast
choice - it only catches a genuine near-collision (black-on-black,
white-on-white, and everything close enough to read as one of those). If
a foreground fails against its paired background, the foreground - never
the background - is force-set to whichever of pure black or white is
farther from that background, and the correction is logged
(`kprintf("[Themes] contrast fix: ...")`) and counted
(`theme_t.contrast_corrected`, readable via
`SYS_THEME_CONTRAST_CORRECTIONS`).

This is the answer to "should the engine refuse a bad theme or derive a
readable foreground": **derive**, not refuse, for a contrast failure -
refusing would mean an App Store theme with one slightly-too-subtle pair
never installs at all, which is a worse outcome than one auto-corrected
color. **Refuse** (see below) is reserved for a file that cannot be read
or parsed at all.

Coverage: every legacy fg/bg pair a theme realistically drives text
against (`label_text`/`window_bg`, `button_text`/`button_bg`,
`textbox_text`/`textbox_bg`, `menu_text`/`menu_bg`,
`selection_text`/`selection_bg`, `titlebar_text`/`titlebar_active`,
`tooltip_text`/`tooltip_bg`, `gauge_fg`/`gauge_bg`,
`checkbox_check`/`checkbox_bg`), checked and corrected *before* v2
defaults are derived (so a v2 token that copies a corrected legacy field
gets the corrected value, not the original bad one); then every v2
rest/hover/active/selected pair across the btn/item/input state families,
`color.surface`/`on_surface`, `color.accent`/`on_accent`, `color.sel_bg`/
`sel_fg`, and both titlebar text roles against both of their gradient
stops, checked *after* v2 defaults are filled (so an explicitly-set v2
value that never touches a legacy field - e.g. a downloaded theme setting
`state.item_selected_fg`/`bg` directly - is still covered). Disabled/muted
pairs are deliberately excluded: reduced contrast there is intentional
design, not a defect, and WCAG AA does not require it either.

## Fail-closed: a file that cannot be read or parsed

If `SYS_THEME_LOAD_FILE` is pointed at a path that doesn't exist, isn't
readable, or produces a parse the kernel treats as an outright failure,
`theme_load_file_runtime()` returns -1 and **does not touch the live theme
table at all** - the previously active theme keeps running, byte for
byte. This applies uniformly at boot too: `theme_init()` skips any listed
`/THEMES/*.mtheme` file it can't load and falls back to the single
hardcoded fallback palette only if *every* listed file fails.

Both userland call sites that can trigger a load now surface this
visibly, not just to serial:

- `gui_theme_activate_path()` (Settings' theme picker, the App Store
  install path) posts a `NOTIFY_ERROR` toast, "Theme could not be
  applied," on failure, and a `NOTIFY_WARNING` toast naming how many
  color pairs were auto-corrected when the contrast floor fired.
- `gui_theme_poll_reload()` (the live-file-edit path, see below) posts the
  same two toasts on the same two conditions when an *already-applied*
  theme file is edited on disk and the edit breaks it or needs
  correcting.

A malformed or partial theme therefore cannot produce an unreadable
desktop, and cannot fail silently: either it doesn't apply (previous
theme visibly kept, error toast) or it applies with a toast naming exactly
how many pairs needed fixing.

## Live apply: no reboot, no restart

Two independent triggers, one mechanism:

1. **Explicit switch.** Settings calls `gui_theme_activate()` ->
   `theme_load_file()` (`SYS_THEME_LOAD_FILE`) -> `theme_set_active()`
   (`SYS_SET_THEME`). Every process's next `theme_color()`/
   `theme_color_of()` call (i.e. every syscall-backed color/metric read)
   sees the new palette immediately - there is no per-process cache to
   invalidate, the kernel table itself changed.
2. **Live file edit.** A theme file can be hand-edited (or overwritten by
   a tool) while it is the active theme. `gui_theme_poll_reload()` is
   polled by both the compositor and Settings roughly every 2s (a
   content-hash compare, not an mtime compare - this filesystem's writer
   does not reliably move mtime), and re-loads + re-activates the file
   when its bytes actually changed.

Neither trigger, by itself, used to reach an *already-open* app window's
content: `theme_set_active()`/a file reload only flip the kernel's live
table and (for the compositor's own chrome) recompute cached chrome
colors. An open app's window kept showing stale colors until it was
resized or recreated (`redraw_pending` was only ever armed at window
create/resize). `SYS_WM_FORCE_REDRAW_ALL` (#704) closes that gap: the
compositor calls it once per detected theme change (an edge, not a level -
never from a per-frame path, so it cannot reintroduce the earlier #564
redraw ping-pong), which arms `redraw_pending` on every open app window.
That flows through the same coalesced "queue at most one EVENT_REDRAW"
path window creation already used, so every open app - not just the
compositor's own taskbar/menus - repaints with the new palette on its very
next frame, with no resize, no restart, no reboot.

## Adding a theme

1. Write `/THEMES/<slug>.mtheme` (see format above - copy an existing file
   as a starting point; the 51 legacy keys are enough for a complete,
   readable theme, everything else defaults sanely).
2. Add `<slug>.mtheme` to `/THEMES/INDEX.TXT` (or let
   `gui_theme_index_append()` do it - the App Store install path calls
   this automatically after writing a new theme file).
3. Activate it: `gui_theme_activate("<slug>")` (Settings' picker does
   this), or just wait ~2s if it's already the active slug and you're
   iterating on it live.

No kernel rebuild, no reboot, no restart of any open app.

## A dead second theme engine (do not build on this)

`kernel/gui/theme.c` (1710 lines), `kernel/gui/theme.h`,
`kernel/gui/theme_parser.c`, and `kernel/gui/theme_line.h` are a complete,
independent theme engine - own `theme_t` struct, own `sys_theme_get_active`/
`sys_theme_get_color`/`sys_theme_set_active` syscall implementations, own
`[section]`-based INI parser, own `theme_engine_init()`/`theme_load()`/
`theme_switch_to()` - that still compiles into the kernel but has **zero
live callers**: nothing in `proc/syscall.c`'s dispatch table calls any
`sys_theme_*` function defined in `theme.c` (the live dispatch calls
`theme_get_color_by_id()`/`theme_load_file_runtime()` etc. from
`themes.c`, the file this document describes), and `gui/theme.h` is
`#include`d nowhere outside `theme.c`/`theme_parser.c` themselves. It is
the implementation of the OLD `/THEMES/<name>/theme.ini` per-directory
format that `docs/UI_STYLE_GUIDE.md` §2/§10 used to describe before this
pass corrected them (see that file's history) - superseded whole by #565,
never removed. It has already caused one documented false lead
(`userland/libc/gui_theme.c`'s own comments note a since-resolved
"kernel/gui/theme.c writes a DIFFERENT THEME.CFG schema" concern that
turned out to reference this dead file). Confirmed dead by exhaustive
grep across the kernel tree, not inferred; removing it is a reasonable,
low-risk follow-up but is out of scope for this pass (touches build flags
and the Rust self-test wiring for its equally-dead `-DRUST_THEME_PARSE`
seam - see `blame.md`).
