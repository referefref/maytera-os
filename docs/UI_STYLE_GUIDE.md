# MayteraOS UI Style Guide

**Version:** 1.0
**Last Updated:** 2026-01-29
**Author:** UI Lead / Art Director

---

## Table of Contents

1. [Design Philosophy](#design-philosophy)
2. [Theme System Overview](#theme-system-overview)
3. [Color Palettes](#color-palettes)
4. [Typography](#typography)
5. [Spacing and Layout Grid](#spacing-and-layout-grid)
6. [Component Specifications](#component-specifications)
7. [Icon Style Guidelines](#icon-style-guidelines)
8. [Animation Guidelines](#animation-guidelines)
9. [Accessibility](#accessibility)
10. [Implementation Reference](#implementation-reference)

---

## 1. Design Philosophy

> **Corrected (docs refresh, 2026-09-23): the SHIPPING default is now the
> translucent glass theme, not Retro UNIX.** The kernel's compiled-in fallback
> theme is still index 0 (Retro UNIX), because the kernel does not read the
> userland theme selection. But the shipping golden restores a persisted
> non-retro **glass** theme at compositor startup (via `/CONFIG/THEME.CFG`), so
> the out-of-box desktop is translucent glass over an animated wallpaper, not
> CDE/Motif. Every non-retro theme now has a default window opacity of **215/255
> (~84%)**; retro themes stay fully opaque (255). A global user window-opacity
> setting takes precedence over the theme default. Retro UNIX remains a
> fully-supported, selectable, opaque theme and is the OS's signature look, but it
> is no longer the boot default. The authoritative reference for the current glass
> language is `docs/UI_GLASS_DESIGN_SYSTEM.md`; the theme engine and file format
> are in `docs/THEMES.md`. The CDE/Motif material below still describes the Retro
> UNIX theme accurately and the component specs apply to every theme.

### The Retro UNIX theme (signature, still shipped)

Retro UNIX is a **CDE/Motif-inspired** visual language reminiscent of classic UNIX workstations (CDE, NeXTSTEP, early IRIX, Solaris). This aesthetic prioritizes:

- **Functional over flashy** - Every visual element serves a purpose
- **Beveled 3D elements** - Raised buttons, sunken input fields, clear depth hierarchy
- **Muted color palettes** - Grays, tans, muted blues; no harsh saturated colors
- **Pixel-perfect borders** - Clean 1-2px borders with highlight/shadow pairs
- **Information density** - Efficient use of screen real estate
- **Predictable behavior** - Visual affordances clearly communicate interactivity

### Theme Variants

The shipping default is a non-retro glass theme (translucent, opacity 215);
Retro UNIX is the opaque signature theme and remains fully selectable:

| Theme | Inspiration | Use Case |
|-------|-------------|----------|
| **retro-unix** (opaque signature theme) | CDE/Motif/NeXTSTEP | Nostalgic, power users, low-resource systems |
| **modern-light** | macOS 11+ (Big Sur+) | Modern light environment users |
| **modern-dark** | macOS 11+ Dark Mode | Modern dark environment users |
| **fluent-light** | Windows 11 Fluent | Windows-familiar users |
| **fluent-dark** | Windows 11 Dark Mode | Windows-familiar dark mode users |

---

## 2. Theme System Overview

### Architecture

**Corrected (themes ticket, 2026-08-07): this section previously described
a directory-per-theme `theme.ini` format that has not been the live format
since #565. It described the implementation in `kernel/gui/theme.c`/
`theme_parser.c`, which still compiles but has zero live callers - see
`docs/THEMES.md`, "A dead second theme engine," for how that was confirmed.
Do not write a theme against the format that used to be documented here;
it will not load.**

Themes in MayteraOS are single flat files, `/THEMES/<slug>.mtheme`, loaded
at boot and at runtime (no reboot needed) by `kernel/gui/themes.c`. See
**`docs/THEMES.md`** for the authoritative file format, the v2 semantic
token namespace (`color.*`/`state.*`/`metric.*`/...), the runtime contrast
floor that fails a broken theme closed and visibly, and exactly how a
theme change reaches an already-open app window with no restart. This
guide covers *design* (palette, typography, spacing, components);
`docs/THEMES.md` covers the *file format and mechanism*.

### Theme File Structure

```
/THEMES/
  INDEX.TXT              one .mtheme filename per line, load order = index
  retro_unix.mtheme       one flat key=value file per theme (no subfolders)
  maytera_dark.mtheme
  ...
```

### .mtheme Format

See `docs/THEMES.md` for the complete, current specification.

---

## 3. Color Palettes

### 3.1 Retro UNIX Theme (Default)

Based on CDE's default "Crimson" and "Neptune" color schemes with influences from NeXTSTEP's grayscale elegance.

#### Primary Colors

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `base-bg` | `#AEB2C3` | 174, 178, 195 | Window backgrounds, panel backgrounds |
| `base-fg` | `#000000` | 0, 0, 0 | Primary text |
| `accent` | `#4B6983` | 75, 105, 131 | Active titlebar, selections |
| `accent-secondary` | `#8B8682` | 139, 134, 130 | Inactive elements |

#### Window Chrome

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `titlebar-active` | `#4B6983` | 75, 105, 131 | Active window titlebar |
| `titlebar-inactive` | `#8B8682` | 139, 134, 130 | Inactive window titlebar |
| `titlebar-text` | `#FFFFFF` | 255, 255, 255 | Titlebar text (active) |
| `titlebar-text-inactive` | `#D0D0D0` | 208, 208, 208 | Titlebar text (inactive) |
| `border-light` | `#DCDAD5` | 220, 218, 213 | 3D highlight (top/left) |
| `border-dark` | `#565248` | 86, 82, 72 | 3D shadow (bottom/right) |
| `border-outline` | `#000000` | 0, 0, 0 | Outer 1px border |

#### Widget Colors

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `button-bg` | `#C0C0C0` | 192, 192, 192 | Button face |
| `button-highlight` | `#FFFFFF` | 255, 255, 255 | Button 3D highlight |
| `button-shadow` | `#808080` | 128, 128, 128 | Button 3D shadow |
| `button-text` | `#000000` | 0, 0, 0 | Button label |
| `input-bg` | `#FFFFFF` | 255, 255, 255 | Text input background |
| `input-border` | `#808080` | 128, 128, 128 | Input field border |
| `scrollbar-trough` | `#A0A0A0` | 160, 160, 160 | Scrollbar track |
| `scrollbar-thumb` | `#C0C0C0` | 192, 192, 192 | Scrollbar handle |

#### Semantic Colors

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `selection-bg` | `#4B6983` | 75, 105, 131 | Selected text/items |
| `selection-fg` | `#FFFFFF` | 255, 255, 255 | Selected text color |
| `error` | `#CC0000` | 204, 0, 0 | Error states |
| `warning` | `#C4A000` | 196, 160, 0 | Warning states |
| `success` | `#4E9A06` | 78, 154, 6 | Success states |
| `disabled-fg` | `#808080` | 128, 128, 128 | Disabled text |
| `disabled-bg` | `#D4D4D4` | 212, 212, 212 | Disabled backgrounds |

#### Desktop and Taskbar

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `desktop-bg` | `#5F7B97` | 95, 123, 151 | Desktop background (if no wallpaper) |
| `taskbar-bg` | `#C0C0C0` | 192, 192, 192 | Taskbar/panel background |
| `taskbar-active` | `#4B6983` | 75, 105, 131 | Active task button |
| `start-button` | `#C0C0C0` | 192, 192, 192 | Start/menu button |

---

### 3.2 Modern Light Theme (macOS-inspired)

Clean, airy aesthetic with subtle shadows and rounded corners.

#### Primary Colors

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `base-bg` | `#FFFFFF` | 255, 255, 255 | Window backgrounds |
| `base-fg` | `#1D1D1F` | 29, 29, 31 | Primary text |
| `accent` | `#007AFF` | 0, 122, 255 | System accent (blue) |
| `accent-secondary` | `#5856D6` | 88, 86, 214 | Secondary accent (purple) |

#### Window Chrome

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `titlebar-active` | `#E8E8E8` | 232, 232, 232 | Active titlebar (translucent) |
| `titlebar-inactive` | `#F6F6F6` | 246, 246, 246 | Inactive titlebar |
| `titlebar-text` | `#1D1D1F` | 29, 29, 31 | Titlebar text |
| `window-shadow` | `rgba(0,0,0,0.2)` | - | Window drop shadow |
| `border` | `#D1D1D6` | 209, 209, 214 | Subtle border |

#### Traffic Light Buttons

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `close-button` | `#FF5F57` | 255, 95, 87 | Close button (red) |
| `minimize-button` | `#FFBD2E` | 255, 189, 46 | Minimize button (yellow) |
| `maximize-button` | `#28C840` | 40, 200, 64 | Maximize button (green) |
| `button-inactive` | `#DCDCDC` | 220, 220, 220 | Inactive window buttons |

#### Widget Colors

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `button-bg` | `#FFFFFF` | 255, 255, 255 | Button background |
| `button-border` | `#D1D1D6` | 209, 209, 214 | Button border |
| `button-hover` | `#F5F5F7` | 245, 245, 247 | Button hover |
| `button-pressed` | `#E5E5EA` | 229, 229, 234 | Button pressed |
| `input-bg` | `#FFFFFF` | 255, 255, 255 | Input background |
| `input-border` | `#D1D1D6` | 209, 209, 214 | Input border |
| `scrollbar-thumb` | `rgba(0,0,0,0.3)` | - | Scrollbar (overlay style) |

---

### 3.3 Modern Dark Theme (macOS Dark-inspired)

#### Primary Colors

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `base-bg` | `#1E1E1E` | 30, 30, 30 | Window backgrounds |
| `base-fg` | `#FFFFFF` | 255, 255, 255 | Primary text |
| `accent` | `#0A84FF` | 10, 132, 255 | System accent (blue) |
| `surface` | `#2D2D2D` | 45, 45, 45 | Elevated surfaces |

#### Window Chrome

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `titlebar-active` | `#3A3A3C` | 58, 58, 60 | Active titlebar |
| `titlebar-inactive` | `#2D2D2D` | 45, 45, 45 | Inactive titlebar |
| `titlebar-text` | `#FFFFFF` | 255, 255, 255 | Titlebar text |
| `border` | `#3D3D3D` | 61, 61, 61 | Subtle border |

#### Widget Colors

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `button-bg` | `#3A3A3C` | 58, 58, 60 | Button background |
| `button-hover` | `#48484A` | 72, 72, 74 | Button hover |
| `button-text` | `#FFFFFF` | 255, 255, 255 | Button text |
| `input-bg` | `#1C1C1E` | 28, 28, 30 | Input background |

---

### 3.4 Fluent Light Theme (Windows 11-inspired)

Microsoft Fluent Design with Mica-like materials.

#### Primary Colors

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `base-bg` | `#F3F3F3` | 243, 243, 243 | Window backgrounds |
| `base-fg` | `#1A1A1A` | 26, 26, 26 | Primary text |
| `accent` | `#0078D4` | 0, 120, 212 | System accent (blue) |
| `mica` | `rgba(255,255,255,0.7)` | - | Mica material background |

#### Window Chrome

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `titlebar-active` | `#F0F0F0` | 240, 240, 240 | Active titlebar |
| `titlebar-inactive` | `#F8F8F8` | 248, 248, 248 | Inactive titlebar |
| `titlebar-text` | `#1A1A1A` | 26, 26, 26 | Titlebar text |
| `border` | `#E5E5E5` | 229, 229, 229 | Window border |

#### Window Buttons (Right-aligned)

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `close-hover` | `#E81123` | 232, 17, 35 | Close button hover |
| `close-pressed` | `#F1707A` | 241, 112, 122 | Close button pressed |
| `control-hover` | `#E5E5E5` | 229, 229, 229 | Min/Max hover |

#### Widget Colors

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `button-bg` | `#FDFDFD` | 253, 253, 253 | Button background |
| `button-border` | `#D6D6D6` | 214, 214, 214 | Button border |
| `button-hover` | `#F9F9F9` | 249, 249, 249 | Button hover |
| `input-bg` | `#FFFFFF` | 255, 255, 255 | Input background |
| `input-border` | `#8A8A8A` | 138, 138, 138 | Input border (bottom) |

---

### 3.5 Fluent Dark Theme (Windows 11 Dark-inspired)

#### Primary Colors

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `base-bg` | `#202020` | 32, 32, 32 | Window backgrounds |
| `base-fg` | `#FFFFFF` | 255, 255, 255 | Primary text |
| `accent` | `#60CDFF` | 96, 205, 255 | System accent (light blue) |
| `surface` | `#2D2D2D` | 45, 45, 45 | Cards/elevated surfaces |

#### Window Chrome

| Token | Hex | RGB | Usage |
|-------|-----|-----|-------|
| `titlebar-active` | `#202020` | 32, 32, 32 | Active titlebar |
| `titlebar-inactive` | `#1F1F1F` | 31, 31, 31 | Inactive titlebar |
| `titlebar-text` | `#FFFFFF` | 255, 255, 255 | Titlebar text |
| `border` | `#3D3D3D` | 61, 61, 61 | Window border |

---

## 4. Typography

Owner: typography family (loop 1). Everything below is realized as `type.*`
keys in `/THEMES/<slug>.mtheme` (mtheme v2, #711), data on the ext2 root
partition, reloaded live by the compositor's ~2s poll. Nothing in this
section requires a kernel rebuild to change; if a value here does not match
what a booted system shows, the `.mtheme` file is the source of truth, not
this document.

### 4.1 Renderer facts (why the rules below are the way they are)

Text is drawn by the kernel's `stb_truetype` rasterizer (`kernel/gui/ttf.c`),
antialiased but **unhinted**, with **whole-pixel glyph positioning** (no
subpixel placement, no subpixel/ClearType-style AA). Consequences that are
not stylistic choices, they are what the rasterizer can and cannot do:

- **No UI text below 11px, no body text below 14px.** Unhinted AA glyphs
  smear below that, especially lowercase with x-height detail (e, a, s).
- **Real kerning exists** (pair kerning from the font's `kern`/GPOS-adjacent
  tables via stb_truetype); letter-spacing does **not** exist as a control,
  so it must never be specified anywhere in a theme or a spec.
- **Bold is faux unless the family ships a real Bold file.** Faux bold is a
  1px right-shift-and-blend of the rendered bitmap (`apply_bold()` in
  `ttf.c`), usable but visibly cruder than a real Bold outline, especially
  at 14px. The shipped default face (below) has a real Bold, so prefer that
  path; faux-bold is the fallback for families that lack one, never the
  first choice.
- **Faux italic is banned in UI chrome.** The renderer can shear a glyph
  buffer, but a sheared upright face reads as broken, not as italic type.
  Reserve it (if ever) for content the user typed as italic in a document,
  never for chrome, labels, or captions.
- **Text is the only antialiased thing on screen.** Every rect, line, and
  circle in this renderer is hard-edged (see the Color/Elevation sections
  for what that means for surfaces). That makes type the one place polish
  is legible pixel-by-pixel, so the size/weight steps below are kept
  deliberately decisive rather than subtle.

### 4.2 Type scale

Six roles, all whole-pixel sizes on the shared glyph-cache bucket ladder
(`{11,12,14,16,18,20,24,28,32}`; landing off this ladder thrashes the glyph
cache with a near-duplicate bucket per stray size). Line height is
`round(size * 1.4)`. Weight is `regular` or `bold` only; there is no
semi-bold, no light, no black in UI chrome regardless of what the shipped
font families technically offer (several do; using them would add hierarchy
steps this spec does not define and no widget spec accounts for).

| Role | `type.*` key | Size | Line height | Weight | Used for |
|---|---|---|---|---|---|
| Caption | `type.caption` | 11px | 15px | regular | Status bar text, tooltips, timestamps, field hints, disabled-state labels |
| Body | `type.body` | 14px | 20px | regular | Menu rows, list rows, dialog body copy, form labels, button labels |
| Body strong | `type.body_strong` | 14px | 20px | bold | Emphasized inline text, the focused/selected row's primary column, summary values ("3 items selected") |
| Title | `type.title` | 16px | 22px | regular | Window titlebars, section headers inside a panel |
| Heading | `type.heading` | 20px | 28px | bold | Dialog titles, top-level page headings (Settings category header, App Store detail header) |
| Display | `type.display` | 28px | 39px | bold | Splash/about screens, empty-state hero text, the clock widget. Rare: most windows never use this role. |

Every role above 14px steps by at least 2px from its neighbor and the
regular-to-bold roles never sit adjacent at the same size without a weight
change (body 14 regular next to body_strong 14 bold, not next to a
13-or-15px neighbor): per the brief, a 14-to-16 jump alone reads as noise
on an unhinted renderer; the decisive jumps are 14 to 20 to 28 for the
body/heading/display spine, with title (16) reserved specifically for
titlebars and body_strong (14 bold) reserved specifically for in-place
emphasis that must not reflow a row.

**`body_strong` is new this loop.** The pre-existing five-role scale
(caption/body/title/heading/display) that shipped with the #711 substrate
had no way to mark a row as "the important one" without jumping to `title`
size, which reflows list geometry. `body_strong` sits in the identical
14px/20px box as `body`, so a row can move from rest to selected/focused
and the surrounding layout does not shift, only the ink weight changes -
this is why File browser and Settings list rows should use
`type.body_strong` for the selected row's label, not `type.title`.

Full 15-key block, present in all 14 shipped `.mtheme` files (added this
loop):

```
type.caption=11
type.caption_lineheight=15
type.caption_weight=regular
type.body=14
type.body_lineheight=20
type.body_weight=regular
type.body_strong=14
type.body_strong_lineheight=20
type.body_strong_weight=bold
type.title=16
type.title_lineheight=22
type.title_weight=regular
type.heading=20
type.heading_lineheight=28
type.heading_weight=bold
type.display=28
type.display_lineheight=39
type.display_weight=bold
```

A key a theme omits falls back to the compiled default in
`kernel/gui/themes.c`'s `DEF_I(...)` calls (the same values as above); see
4.6 for the one role that does **not** yet have a compiled fallback slot.

### 4.3 Weights available, and how "bold" should resolve

Only `regular` and `bold` are meaningful values for any `type.*_weight` key.
When a role's weight is `bold`, rendering should prefer, in order:

1. A real Bold face already enrolled for the active family (e.g. the
   default `DejaVu Sans` has `DejaVu Sans Bold` enrolled from
   `/FONTS/DEJAVUB.TTF` by the time `ttf_rescan()` finishes at boot -
   confirmed present in the shipped 53-face set).
2. Faux bold (`apply_bold()`) only when no real Bold face is enrolled for
   the active family.

This ordering is a **specification for the draw-time consumer of
`type.*_weight`, not something already wired**. As of this loop,
`kernel/gui/themes.c` stores `t_*_w` in the theme table (`DEF_I(t_heading_w,
TM_TYPE_HEADING_W, 1)` etc.) but no text-drawing call site in
`kernel/gui/*.c` reads any `t_*_w` field yet, confirmed by grep with zero
matches outside the definition table. Every `type.*_weight=bold` in every
shipped theme is currently inert: headings and titles render at whatever
weight the call site hardcodes, not at the theme's stated weight. This is
scoped engineering work for whoever wires role-based text drawing (see 4.7);
it is flagged here rather than left to be discovered as a silent no-op.

### 4.4 Usage rules: which surface uses which role

| Surface | Role | Notes |
|---|---|---|
| Window titlebar | `type.title` | Both active and inactive states use the same role/size; only the color token changes (`titlebar_text` vs `titlebar_text_inactive`). |
| App menu bar and its dropdowns (`gui_menu`, Editor + Terminal) | `type.body`, read LIVE from the theme | The shared widget resolves `type.body`, `metric.menu_row_h` and `metric.gap` from the active theme in `gui_menu_bar_init()`, so this is the first SHARED widget where 4.3's acceptance test actually holds. Bar labels, popup rows and accelerators are all one size, so a dropdown reads as belonging to its bar. |
| Menu rows (start menu, context menus, window menu) | `type.body` | Selected/hover row stays `type.body`; do not bold a hovered menu row, use the `state.item_hover_*` color pair instead so hover reads as a background change, not a weight change. |
| List/table rows (Files, Settings side-lists, App Store) | `type.body`, numerals right-aligned (4.5) | The focused/selected row's primary label may step up to `type.body_strong`; other columns in that row stay `type.body`. |
| Status bar / footer text | `type.caption` | Always `on_surface_muted`, never `on_surface`: caption is explicitly the de-emphasized role. |
| Tooltips | `type.caption` | Single line preferred; if a tooltip needs two lines, that content belongs in a dialog, not a tooltip. |
| Form field labels | `type.body` | Sits above or beside the input at `metric.gap` (8px), never inside the input box. |
| Field hint / validation text | `type.caption` | Below the input, `color.danger`-toned for an error, `on_surface_muted` otherwise. |
| Dialog title | `type.heading` | Not `type.title`: a dialog is not a window titlebar, it needs the heavier role to read as a distinct level from the window that owns it. |
| Section headers inside a panel (Settings category groups) | `type.title` | One step down from a dialog heading; these are subordinate to the dialog/window, not peers of it. |
| Button labels | `type.body` | `type.body_strong` for a dialog's default/primary action button only, to give it exactly one weight step of visual priority over secondary buttons - never combine that with also changing its size. |
| Splash / about / empty-state hero text | `type.display` | The only role allowed to appear essentially alone on a screen with no `body` text beside it at the same visual weight. |

### 4.5 Truncation and alignment

- **Truncation is always ellipsis, never a mid-glyph clip.** A string that
  does not fit its box is measured against the box width and truncated at
  the last whole glyph that leaves room for a trailing `...`, not simply
  drawn and left for the renderer to clip mid-character. This applies to
  every role in every surface: window titles, list rows, menu items, tab
  labels. A clipped half-glyph is one of the fastest visual "this is
  unfinished" signals a UI can send, per the brief's own framing.
- **Labels are left-aligned.** Every `type.body`/`type.caption` label in a
  form, list, or menu starts at the surface's left content edge (offset by
  the surface's padding token); do not center labels, including dialog
  button labels' associated text, form field labels, or list-row primary
  columns.
- **Numerals are right-aligned in tabular contexts.** Any column of numbers
  meant to be compared at a glance (file sizes, counts, prices, percentages,
  durations) right-aligns so digit places stack, using a fixed-width
  numeral rendering where the family supports it (DejaVu Sans, Lato, and
  Source Sans Pro's digits are tabular by default; verify before assuming
  it for any newly-installed family via Font Book). A single non-tabular
  numeral (an inline count in a sentence, "3 items") stays inline with the
  surrounding body text and is not right-aligned.
- **Vertical centering inside a fixed-height control** (`metric.btn_h`,
  `metric.input_h`, `metric.titlebar_h`) uses the role's line height, not
  its raw pixel size, so ascenders/descenders sit optically centered rather
  than the em-box being centered and the glyphs looking low.

### 4.6 Kernel wiring gap (flagged, not fixed by this loop)

`kernel/gui/themes.h`'s `TM_TYPE_*` enum and `kernel/gui/themes.c`'s
`g_theme_fields[]` offset table have slots for caption/body/title/heading/
display (15 fields: size + lineheight + weight per role) but **no slot for
`body_strong`** yet. The key is written into every `.mtheme` file (4.2) and
will parse harmlessly today (an unrecognized key is a documented no-op for
this line-reader format), but nothing on the C side can read it back into
`theme_t` until three fields are added:

```
// themes.h, in the TM_TYPE_* enum, between TM_TYPE_BODY_W and TM_TYPE_TITLE:
TM_TYPE_BODY_STRONG, TM_TYPE_BODY_STRONG_LH, TM_TYPE_BODY_STRONG_W,

// themes.c, g_theme_fields[], same relative position as the enum entries:
{ "type.body_strong",            offsetof(theme_t, t_body_strong) },
{ "type.body_strong_lineheight", offsetof(theme_t, t_body_strong_lh) },
{ "type.body_strong_weight",     offsetof(theme_t, t_body_strong_w) },

// themes.c, DEF_I defaults block, alongside the existing t_body_* defaults:
DEF_I(t_body_strong,    TM_TYPE_BODY_STRONG,    14);
DEF_I(t_body_strong_lh, TM_TYPE_BODY_STRONG_LH, (t->t_body_strong * 14 + 5) / 10);
DEF_I(t_body_strong_w,  TM_TYPE_BODY_STRONG_W,  1);
```

Plus three new fields on `theme_t` itself. This is a mechanical, low-risk
addition (same shape as the other 15 fields, same file, same pattern) but it
is C and outside this designer's remit to edit; flagged precisely so it is
one PR, not a rediscovery.

### 4.7 Default UI face

The shipped default (`/FONT.TTF`, loaded eagerly as face 0 at boot, before
any theme or `/FONTS` scan) is **DejaVu Sans**, and this loop's
recommendation is to **keep it**, not swap it. Reasoning:

- It already has a real Bold file (`/FONTS/DEJAVUB.TTF`, part of the
  12-face DejaVu family already enrolled at boot) plus Oblique and Bold
  Oblique - satisfying the "prefer a real Bold file" rule in 4.3 with zero
  asset change.
- It is the face every other subsystem already assumes: the boot splash
  text, Font Book's default listing, and the historical '7'-glyph
  regression guard (`ttf_selfcheck_digits()`) were all measured against
  this exact file. Swapping the OS-wide default would move the one thing
  every visual regression in this codebase has been checked against.
- The mtheme v2 schema (as specified for this loop) has **no per-theme font
  family key** - `type.*` carries size/line-height/weight only, not a face
  name. So "the default UI face" is necessarily a single OS-wide choice
  (which file is `/FONT.TTF`), not a per-theme token, and changing it is an
  asset/build change, not a theme-file change. This designer's remit is
  data in `.mtheme` files; picking a different physical font file is
  flagged as a decision, not made unilaterally here.

**Location and licensing, as required to flag:** `/FONT.TTF` and every
`/FONTS/*.TTF` face live on **partition 1, the FAT ESP**, not the ext2 root
that `THEMES/`/`CONFIG/` live on (confirmed: `kernel/gui/ttf.c`'s own
comment, "the /FONTS store lives on the FAT ESP... verified on golden 859").
Practically: a `.mtheme` edit takes effect on the ~2s poll with nothing
rebuilt (4.0), but **a font swap needs a build** (`build-golden.sh` stages
`/FONTS` from `build/font-licenses` + the base asset image; there is no
live-reload path for `/FONT.TTF` itself). DejaVu Sans ships under the
Bitstream Vera license (`build/font-licenses/BitstreamVera-DejaVu.txt`,
freely redistributable, no royalty, attribution preserved in
`/FONTS/LICENSE.TXT`) - already compliant, already shipping.

If a future loop wants a distinct OS-wide identity face (the "Maytera
Light/Dark" themes were named to evoke a more contemporary system font than
DejaVu's utilitarian 2003 metrics), **Source Sans Pro** and **Lato** are
both already licensed and enrolled in `/FONTS` (SIL OFL 1.1, real Bold
files: `SRCSANB.TTF`, `LATOB.TTF`) and would need no new licensing work,
only the `/FONT.TTF` swap plus a re-verification of the '7'-glyph guard and
every pixel-metric that assumes DejaVu's advance widths (window min-sizes,
button min-width `75px`, the digit-grid guard in 4.5). Not attempted this
loop: it is a cross-cutting visual change with real regression surface, not
a typography-family-scoped one.

### 4.8 Contrast (measured this loop)

Body text (`color.on_surface` on `color.surface`), the highest-traffic
text/background pair in the system:

| Theme | Ratio | Pass/fail (WCAG AA, 4.5:1 normal text) |
|---|---|---|
| Maytera Dark | **15.6:1** | Pass (exceeds AAA 7:1) |
| Maytera Light | **16.4:1** | Pass (exceeds AAA 7:1) |
| Retro UNIX (black on `#B4B4B4`) | **10.1:1** | Pass |

Caption/secondary text (`color.on_surface_muted` on `color.surface`), the
role most likely to be under-contrasted because it is deliberately the
quiet one:

| Theme | Ratio | Pass/fail |
|---|---|---|
| Maytera Dark | **6.7:1** | Pass |
| Maytera Light | **5.6:1** | Pass |

`color.on_accent` (white) on `color.accent`, used for selected/pressed
state labels and the primary-button `body_strong` case in 4.4:

| Theme | Ratio | Pass/fail |
|---|---|---|
| Maytera Dark | **5.7:1** | Pass |
| Maytera Light | **6.7:1** | Pass |

**One defect found, already resolved by a concurrent color-family pass.**
`color.titlebar_text_inactive` in **Maytera Light** against
`color.titlebar_inactive_bottom` originally measured **3.23:1**, below the
4.5:1 floor for `type.title`'s 16px regular text (16px regular does not
qualify for the 3:1 "large text" AA exception; it would need to be 16px
bold or roughly 24px regular to qualify, and titlebars are neither). By the
time this loop finished, `titlebar_text_inactive` had already moved from
`0x007A828C` to `0x005C6269` in the working tree (the color-scheme family's
own loop-1 report independently found the same defect at 3.60:1, a measurement
difference explained by which of the two candidate background stops each
pass compared against). Re-measured against the current value: **5.12:1,
passes.** The dark-theme equivalent (`titlebar_text_inactive` on
`titlebar_inactive_bottom`) was never at risk, measuring **5.39:1** both
before and after. No shared-token change is outstanding from this family.

All ratios computed via the standard WCAG relative-luminance formula from
each theme's own hex pairs; retro/legacy themes (`light`, `dark`,
`high_contrast`, and the rest of the pre-#711 set) are not re-audited here -
they already carry `lint.baseline=legacy-v1` in the design-contract lint and
are out of this loop's scope.

### 4.9 Renderer limits this family does not get to use

Per the renderer constraints, none of the following may appear in any
`type.*` value or in any spec this family writes: letter-spacing/tracking
(no control exists), font weights beyond regular/bold (no semibold/light/
black step, however many the shipped families technically contain), faux
italic in chrome, subpixel or hinted positioning assumptions, or any
text-adjacent effect (glow, shadow, outline) since only flat glyph
antialiasing exists. Any of these would need a new renderer primitive and
is out of scope to assume free.

### 4.10 Loop 3 audit (2026-08-06)

Re-verified against the current working tree; no `.mtheme` edit was needed
this loop, the 18-key `type.*` block already matches 4.2 exactly in all 14
shipped theme files (`grep -c '^type\.' build/assets/themes/*.mtheme` = 18
everywhere, `type.body_strong` present in all 14 including `maytera_dark`/
`maytera_light`, closing the "two-file sync gap" a concurrent family's
loop-2 report flagged against this family before this loop started).

**Contrast re-measured with the CURRENT on-disk hex values** (not carried
forward from loop 2's numbers - the color family had another pass in
between):

| Pair | Dark | Light |
|---|---|---|
| `on_surface` / `surface` (type.body) | 15.61:1 | 16.44:1 |
| `on_surface_muted` / `surface` (type.caption) | 6.79:1 | 5.55:1 |
| `titlebar_text` / `titlebar_top` (type.title) | 13.50:1 | 17.17:1 |
| `titlebar_text` / `titlebar_bottom` (type.title) | 13.88:1 | 15.44:1 |
| `titlebar_text_inactive` / `titlebar_inactive_bottom` (type.title) | 5.39:1 | 5.12:1 |

All pass WCAG AA (4.5:1) for normal-size text, most clear AAA (7:1). No
regression since loop 2: the color family's loop-2 fix to
`titlebar_text_inactive` in Maytera Light (3.23:1 -> 5.12:1) held stable
through loop 3's concurrent edits.

**Escalating a wiring gap larger than the one already flagged in 4.3/4.6.**
4.3 documented that `type.*_weight` has zero draw-time consumers. Measured
this loop, the gap is bigger: `type.*` SIZE is also uncommunicated to
nearly the entire shipped UI, not just weight.

- `THEME_METRIC_TYPE_*` ids exist end-to-end (`kernel/gui/themes.c`'s
  `g_theme_fields[]`, wire syscall `SYS_THEME_METRIC`=357, userland
  `theme_metric()`/`theme_metric_or()` in `userland/libc/theme.h`) and are
  provably live: two call sites in `userland/apps/settings/main.c` use them
  (`draw_section_header()` -> `THEME_METRIC_TYPE_TITLE`, `draw_subsection()`
  -> `THEME_METRIC_TYPE_BODY`, both landed loop 2, both pass an explicit
  `FONT_STYLE_BOLD` bit and the real DejaVu Sans Bold face, not faux bold).
- Everything else in that same file goes through `win_draw_text(h,x,y,s,c)`,
  a **local macro** (`settings/main.c:22`) that expands to
  `win_draw_text_ttf(h,x,y,s,14,c)` - a compiled-in literal `14`, no theme
  read, no weight bit. `win_draw_text_small` is the same pattern at a
  literal `11`. Counted: **110 `win_draw_text(` call sites in
  settings/main.c** (excluding the macro's own definition line) route
  through this macro, i.e. hardcoded 14px, plus every `win_draw_text_small(`
  call at hardcoded 11px. These happen to numerically match `type.body`/
  `type.caption`'s compiled defaults today, which is exactly why the screen
  looks correct and the gap is invisible without grepping for it: edit
  `type.body` in any `.mtheme` file right now and Settings' labels, values,
  panel names, dialog text and the About page do not move.
- Grepped for `theme_metric(`/`theme_metric_or(`/`theme_metric_of(` across
  every other userland app (compositor taskbar/startmenu/widgets, Files,
  Editor, Terminal, IRC, Browser, App Store, ...): **zero matches**. The two
  Settings call sites above are the only place in the shipped userland
  where a `type.*` value is read live rather than compiled in.
- For scale: **317** raw `win_draw_text(` call sites exist tree-wide
  (kernel `gui/*.c` + every userland app) against **169**
  `win_draw_text_ttf`/`_ttf_ex` call sites. Not all 317 are misclassified -
  `pong`, `rogue`'s `curses.c`, `solitaire`'s board are legitimate
  fixed-grid bitmap-font uses, not label text - but it is the ceiling on how
  many sites could even structurally carry a `type.*` role today, and it
  confirms the blame.md-documented trap (`win_draw_text` = legacy 8x16
  bitmap font, no size/weight parameter exists on that path at all) is
  still the common case, not the exception.

**UPDATED 2026-08-25: now 3, and the third is a SHARED WIDGET.**
`userland/libc/gui_menu.c` resolves `type.body`, `metric.menu_row_h` and
`metric.gap` from the active theme (with clamps, because a `.mtheme` is
user-editable data), so every app that adopts the menu-bar primitive inherits
live type without its own call site. That is the shape the pass below should
take generally: wire the SHARED primitives, and the count goes up by an app at
a time instead of a call site at a time. The finding below stands for
everything else.

**UPDATED AGAIN 2026-08-25 (#appstyle): now 5, and the count moved by TWO APPS
plus a SECOND SHARED WIDGET in one pass, which is the point.**
`userland/libc/gui_list.c` was still drawing every list row with
`win_draw_text()` - the same fault `gui_menu.c` carried until #307, in a widget
that Terminal's Preferences dialog and the Disk Images picker both adopt, and
that NEITHER app could fix from its own side. It now draws with
`win_draw_text_ttf()` at `GUI_TTF_SIZE` (the active theme's `type.body`),
vertically centred on that size rather than on the old hardcoded 16, which was
the bitmap CELL height and would sit the ink a pixel high in a 24px row.
`apps/diskimg/main.c` and `apps/install/main.c` are the two apps.

The wiring lesson from the paragraph above holds and is now measured twice:
**fix the SHARED widget and the count goes up by an app at a time.** When
auditing an app for this fault, grep libc in the same pass - an app-local fix
leaves the shared widgets it calls still on the bitmap font, and a window that
is 90% converted reads as a rendering glitch rather than as an unconverted call
site.

**AND THE CONTRAST COROLLARY, MEASURED FIVE TIMES IN THAT SAME PASS.** A colour
returned by `gui_ensure_contrast(fg, bg, floor)` is guaranteed against **that
bg** and nothing else. Every failure found was a palette field walked against
one surface and then drawn on another: a `dim` guaranteed on the content
background used in the status band (4.16:1) and on the list fill (3.84:1); an
`accent` guaranteed on a card used on an inner card (3.82:1); a `danger`
guaranteed on the card used on its own soft pill (3.47:1). Any role that appears
on more than one surface needs one field PER SURFACE, named for the surface.
`gui_ensure_contrast2()` covers only the genuinely-two-backgrounds case.
`gui_ink_on()` and `lum_ink()` are threshold GUESSES, not measurements: use them
as the starting point and walk the result.

**EVERY SIZE IN THIS DOCUMENT IS A 1x DESIGN VALUE.** Since 2026-08-26 there is
a global UI scale factor (`docs/UI_SCALE.md`, `kernel/rustkern/uiscale.rs`) and
it is applied at READ time, inside `theme_get_metric_by_id()`. So `type.body =
14` is what the `.mtheme` file contains and what this guide specifies; at 150%
the accessor returns 21 and every theme-wired widget in the kernel and in every
Ring 3 app follows with no call-site change.

Three consequences for anyone writing or reviewing UI here:

1. **Do not put scaled values in a theme file.** `build/assets/theme-scale-lint.sh`
   enforces the 1x scales this guide publishes (`type.*` from {11,14,16,20,28},
   radii from {0,3,4,6,10}, spacing from {4,8,12,16,24,32}), and it is right to.
   A theme is per-LOOK; scale is per-DISPLAY; they are independent axes.
2. **App code needs no scale awareness at all.** The kernel transforms window
   coordinates at the syscall boundary, so an app draws, measures and hit-tests
   in logical pixels throughout. If you find yourself reading the scale factor
   inside an app, the boundary is not doing its job and that is the bug to file.
3. **If you add a surface whose geometry you do NOT scale, opt its text out of
   scaling too.** Scaled text inside an unscaled box overflows, and that reads as
   a rendering fault rather than as the known limitation it is. Consistently
   small is something you can write down.

**A UNIT TRAP THAT WILL BITE ANY `type.*` WIRING PASS.** The `size` argument to
`win_draw_text_ttf()` is NOT a CSS pixel size. `kernel/gui/ttf.c` scales with
`stbtt_ScaleForPixelHeight()`, which maps *ascent - descent* onto the requested
size, not the em square. Measured from the shipped `/FONT.TTF`: DejaVu Sans,
unitsPerEm 2048, ascent 1901, descent -483, so **one size unit is 0.859 em px**.
`type.body = 14` therefore renders at em 12.03px with a cap height of 8.77px.
Any size picked by eye against an HTML mock is 14% too small when ported.

**Original finding, unchanged: the test holds for `type.*` at 2 non-shared call
sites in the entire shipped UI.** This
is a bigger gap than 4.3's weight-only framing suggested, and it is not
fixable in `.mtheme` data - it needs the same class of app-code wiring pass
Substrate already ran for `metric.*`/`decor.*`/`radius.*` consumption
elsewhere in this loop, applied to `type.*` next. Flagging for the backlog:
it did not appear in this loop's Substrate priority list (director
direction section 2, items 1-9), and three loops have now passed without
it being scheduled.

**Files app: 4.5's own numeral-right-alignment rule is violated in the one
place it matters most.** `userland/apps/files/main.c`'s details-view size
column draws at a fixed left-anchored offset
(`win_draw_text_small(window_handle, lx + lw - 130, y + 7, ss, dimc)`,
details view around line 1264; `ax + aw - 70` in the Recycle Bin view,
around line 1373) rather than measuring the string and right-aligning it
the way the adjacent type column already does correctly three lines below
in the same function (`int tw = gui_ttf_width(t, 11); win_draw_text_small(
window_handle, lx + lw - 8 - tw, ...)`). Effect: "4.2 KB" and "128 B" sit
flush-left in the size column instead of flush-right, so digit places do
not stack and sizes cannot be compared at a glance, contradicting 4.5.
`gui_ttf_width()` is already in scope at the call site (used two lines
away for the type column), so the fix is mechanical:
`int sw = gui_ttf_width(ss, 11); win_draw_text_small(window_handle,
lx + lw - 8 - sw, y + 7, ss, dimc);` (and the Recycle Bin equivalent). Not
fixed here: it is app C code, outside this family's remit and this loop's
build budget; flagged with exact lines and the one-line fix for whoever
next has the Files build slot.

**Default UI face: unchanged, still correct.** `/FONT.TTF` (face 0, loaded
before any theme) is confirmed by md5 to still be DejaVu Sans Regular;
`/FONTS/DEJAVUB.TTF` (DejaVu Sans Bold, a real outline, not synthesized)
still ships alongside it - re-verified against `golden-built.img` (build
1027) rather than assumed from the loop-2 doc text. No change proposed;
4.7's reasoning stands.

No shared-token change requested of the director this loop.

---

## 5. Spacing and Layout Grid

### Base Unit

All spacing in MayteraOS is based on a **4px base unit**.

```
4px  = 1 unit  (xs)
8px  = 2 units (sm)
12px = 3 units (md)
16px = 4 units (lg)
24px = 6 units (xl)
32px = 8 units (2xl)
```

### Window Dimensions

#### Retro UNIX Theme

| Element | Value | Notes |
|---------|-------|-------|
| Titlebar height | 20px | Classic Motif height |
| Border width | 2px | 3D beveled border |
| Close button | 16x16px | Square with X glyph |
| Min/Max buttons | 16x16px | Square icons |
| Button spacing | 2px | Between titlebar buttons |
| Content padding | 8px | Inside window content area |
| Resize grip | 10x10px | Corner resize handle |

#### Modern Themes

| Element | Value | Notes |
|---------|-------|-------|
| Titlebar height | 28px | Taller for touch-friendly |
| Border width | 1px | Subtle border |
| Corner radius | 10px | Rounded corners |
| Traffic lights | 12x12px | Circular buttons |
| Button spacing | 8px | Between traffic lights |
| Content padding | 16px | More generous spacing |
| Shadow | 0 10px 30px rgba(0,0,0,0.15) | Soft drop shadow |

#### Fluent Themes

| Element | Value | Notes |
|---------|-------|-------|
| Titlebar height | 32px | Windows 11 standard |
| Border width | 1px | Thin border |
| Corner radius | 8px | Slightly rounded |
| Control buttons | 46x32px | Wide rectangular buttons |
| Content padding | 12px | Standard padding |
| Shadow | 0 8px 16px rgba(0,0,0,0.14) | Elevation shadow |

### Layout Grid

#### Desktop Grid
- Grid cell size: 80x80px (retro), 90x90px (modern)
- Icon spacing: 16px between icons
- Desktop margin: 16px from screen edges

#### Window Content Grid
- Column width: Fluid, based on window width
- Gutter: 16px between columns
- Maximum content width: None (fill available)

---

## 6. Component Specifications

### 6.1 Buttons

#### Retro UNIX (Motif-style 3D Button)

```
+------------------------------------------+
|  highlight (1px) - #FFFFFF               |
| +--------------------------------------+ |
| |                                      | |
| |        BUTTON LABEL                  | |
| |                                      | |
| +--------------------------------------+ |
|                        shadow (1px) - #808080
+------------------------------------------+
  outer shadow (1px) - #404040
```

**Specifications:**
- Height: 24px (standard), 20px (compact)
- Min width: 75px
- Padding: 8px horizontal, 4px vertical
- Border: 2px 3D beveled (highlight top/left, shadow bottom/right)
- Corner radius: 0px (square)

**States:**
| State | Background | Border | Text |
|-------|------------|--------|------|
| Normal | `#C0C0C0` | 3D raised | `#000000` |
| Hover | `#D0D0D0` | 3D raised | `#000000` |
| Pressed | `#A0A0A0` | 3D sunken (inverted) | `#000000` |
| Disabled | `#C0C0C0` | Flat gray | `#808080` |
| Focused | `#C0C0C0` | + 1px dotted inner | `#000000` |

#### Modern Themes (macOS-style)

**Specifications:**
- Height: 28px (standard), 22px (small)
- Min width: 64px
- Padding: 12px horizontal, 6px vertical
- Border: 1px solid + subtle shadow
- Corner radius: 6px

**States:**
| State | Background | Border | Shadow |
|-------|------------|--------|--------|
| Normal | `#FFFFFF` | `#D1D1D6` | `0 1px 2px rgba(0,0,0,0.05)` |
| Hover | `#F5F5F7` | `#C8C8CC` | `0 1px 3px rgba(0,0,0,0.08)` |
| Pressed | `#E5E5EA` | `#BEBEC2` | `inset 0 1px 2px rgba(0,0,0,0.05)` |
| Primary | `#007AFF` | none | `0 1px 3px rgba(0,122,255,0.3)` |

#### Fluent Themes (Windows 11-style)

**Specifications:**
- Height: 32px
- Min width: 96px
- Padding: 12px horizontal
- Border: 1px solid (bottom 1px darker)
- Corner radius: 4px

**States:**
| State | Background | Border |
|-------|------------|--------|
| Normal | `#FDFDFD` | `#D6D6D6` + bottom `#A0A0A0` |
| Hover | `#F9F9F9` | Same |
| Pressed | `#F0F0F0` | Same |
| Accent | `#0078D4` | Subtle dark |

---

### 6.2 Scrollbars

#### Retro UNIX (Always visible, substantial)

```
+---+
| ^ |  <- Arrow button (16x16)
+---+
|   |
| # |  <- Thumb (draggable)
|   |
+---+
| v |  <- Arrow button
+---+
```

**Specifications:**
- Width: 16px
- Arrow buttons: 16x16px with 3D beveling
- Thumb: Min 20px height, 3D raised
- Trough: Sunken 3D appearance

#### Modern Themes (Overlay scrollbars)

**Specifications:**
- Width: 8px (expanded on hover: 12px)
- No arrow buttons
- Thumb: Rounded, semi-transparent
- Trough: Transparent until hover

#### Fluent Themes (Thin, revealed on scroll)

**Specifications:**
- Width: 6px (expanded: 12px)
- Thumb: 2px corner radius
- Auto-hide: Fades after 2 seconds idle

---

### 6.3 Menus

#### Retro UNIX (Motif-style popup)

**Specifications:**
- Background: `#C0C0C0`
- Border: 2px 3D raised
- Item height: 20px
- Item padding: 8px horizontal
- Separator: 1px sunken line with 4px margin
- Submenu arrow: `>` character, right-aligned

**Item States:**
| State | Background | Text |
|-------|------------|------|
| Normal | Transparent | `#000000` |
| Hover | `#4B6983` | `#FFFFFF` |
| Disabled | Transparent | `#808080` |

#### Modern Themes

**Specifications:**
- Background: `#FFFFFF` with shadow
- Border: None (shadow provides edge)
- Corner radius: 6px
- Item height: 28px
- Item padding: 12px horizontal
- Icon area: 24px left margin for checkmarks/icons

#### Fluent Themes

**Specifications:**
- Background: Semi-transparent with Acrylic blur
- Border: 1px `#E5E5E5`
- Corner radius: 8px
- Item height: 36px
- Flyout animation: 100ms ease-out

---

### 6.4 Checkboxes and Radio Buttons

#### Retro UNIX

**Checkbox:**
- Size: 13x13px
- Border: 2px sunken 3D
- Checkmark: X or checkmark glyph in black
- Label spacing: 4px

**Radio Button:**
- Size: 13x13px (circular)
- Border: 2px sunken 3D
- Indicator: Filled circle 7px

#### Modern Themes

**Checkbox:**
- Size: 16x16px
- Border: 1px `#D1D1D6`
- Corner radius: 4px
- Checked: Blue fill with white checkmark

#### Fluent Themes

**Checkbox:**
- Size: 20x20px
- Border: 1px `#8A8A8A`
- Corner radius: 4px
- Checked: Accent color fill with white checkmark

---

### 6.5 Text Inputs

#### Retro UNIX

**Specifications:**
- Height: 22px
- Border: 2px sunken 3D
- Background: `#FFFFFF`
- Padding: 4px
- Cursor: Block cursor (black)

#### Modern Themes

**Specifications:**
- Height: 28px
- Border: 1px `#D1D1D6`
- Corner radius: 6px
- Background: `#FFFFFF`
- Padding: 8px
- Focus: Blue border `#007AFF`

#### Fluent Themes

**Specifications:**
- Height: 32px
- Border: 1px (bottom 2px accent on focus)
- Corner radius: 4px
- Focus: Bottom border changes to accent color

---

### 6.6 Window Title Bars

#### Retro UNIX

```
+========================================================+
| [#] Window Title                          [_] [O] [X] |
+========================================================+
```

- Height: 20px
- Gradient: None (solid color)
- Title: Left-aligned, 4px from icon
- Buttons: Right-aligned, 16x16px each
- Icon: 16x16px app icon (optional)

#### Modern Themes

```
+----------------------------------------------------------+
|  (O) (O) (O)    Window Title                             |
+----------------------------------------------------------+
```

- Height: 28px
- Buttons: Left-aligned traffic lights (macOS style)
- Title: Centered
- Background: Semi-transparent (vibrancy)

#### Fluent Themes

```
+----------------------------------------------------------+
|  [icon] Window Title                      [_] [O] [X]   |
+----------------------------------------------------------+
```

- Height: 32px
- Buttons: Right-aligned, wide buttons
- Title: Left-aligned with app icon
- Background: Mica material effect

---

## 7. Icon Style Guidelines

### Retro UNIX Icons

**Characteristics:**
- Size: 32x32px (standard), 16x16px (small)
- Color depth: 16-256 colors
- Style: Flat with minimal gradients, 1px black outline
- Perspective: Front-facing, 2D
- No drop shadows on icons

**Inspiration:** CDE icons, early GNOME, NeXTSTEP

### Modern Theme Icons

**Characteristics:**
- Size: 32x32px, 64x64px, 128x128px
- Style: SF Symbols-inspired, outlined or filled
- Stroke weight: 2px for outlined style
- Colors: Monochrome with accent color highlights
- Soft drop shadows optional

### Fluent Theme Icons

**Characteristics:**
- Size: 16x16, 20x20, 24x24, 32x32, 48x48px
- Style: Fluent System Icons - outlined by default
- Stroke weight: 1.5px
- Colors: Single color, system accent
- No gradients or shadows

### Icon Grid

All icons should be designed on a pixel grid:
- 16px icons: 14px safe area, 1px padding
- 32px icons: 28px safe area, 2px padding
- 64px icons: 56px safe area, 4px padding

---

## 8. Animation Guidelines

### General Principles

1. **Purpose over decoration** - Animations should provide feedback, not distraction
2. **Quick and responsive** - Most animations < 200ms
3. **Respect reduced motion** - Provide static alternatives

### Retro UNIX Theme

**Approach:** Minimal to no animation
- Window open/close: Instant (no animation)
- Button press: Immediate visual state change
- Menu appearance: Instant

### Modern Themes

**Timing:**
- Micro-interactions: 100-150ms
- State transitions: 150-200ms
- Page/view transitions: 250-350ms

**Easing:**
- Enter: `ease-out` (cubic-bezier(0, 0, 0.2, 1))
- Exit: `ease-in` (cubic-bezier(0.4, 0, 1, 1))
- Standard: `ease-in-out` (cubic-bezier(0.4, 0, 0.2, 1))

**Animations:**
| Action | Animation | Duration |
|--------|-----------|----------|
| Window open | Scale 0.95->1.0, fade in | 200ms |
| Window close | Scale 1.0->0.95, fade out | 150ms |
| Window minimize | Scale to taskbar position | 300ms |
| Button hover | Background fade | 100ms |
| Menu open | Fade in + slight Y translation | 150ms |

### Fluent Themes

**Timing:**
- Fast: 83ms (button hover)
- Normal: 167ms (standard transitions)
- Slow: 250ms (page transitions)

**Easing:** Windows uses custom bezier curves
- Standard: `cubic-bezier(0.8, 0, 0.2, 1)`

---

## 9. Accessibility

### Color Contrast

All themes must meet WCAG 2.1 AA standards:
- Normal text: 4.5:1 minimum contrast ratio
- Large text (18px+): 3:1 minimum contrast ratio
- UI components: 3:1 minimum contrast ratio

### High Contrast Mode

MayteraOS includes a dedicated High Contrast theme:
- Background: Pure black `#000000`
- Text: Pure white `#FFFFFF`
- Links/accents: Bright cyan `#00FFFF`
- Borders: White 2px
- No gradients or shadows

### Focus Indicators

All interactive elements must have visible focus states:
- Retro: 1px dotted outline inside component
- Modern: Blue ring shadow
- Fluent: Black 2px outline with 2px white offset

### Motion Sensitivity

- Respect OS-level "reduce motion" settings
- Provide instant alternatives for all animations
- Avoid parallax and auto-playing animations

---

## 10. Implementation Reference

**Corrected (themes ticket, 2026-08-07): this section used to give a
`[section]`-based `theme.ini` spec and an "Add theme definition to
`g_themes[]`, rebuild kernel" workflow. Neither has been true since #565 -
themes are runtime data, not compiled-in C, and the `[section]` format was
never the live one (it belongs to the confirmed-dead `kernel/gui/theme.c`/
`theme_parser.c`, see `docs/THEMES.md`). Full current spec, the v2 token
namespace, the runtime contrast floor, and the live-apply mechanism all
live in `docs/THEMES.md` now, so this guide doesn't carry two copies of a
spec that drift apart again.**

### .mtheme File Format

See `docs/THEMES.md`.

### C API Integration

The theme system integrates with the kernel via `themes.h`:

```c
// Get current theme colors
const theme_t *theme = theme_get_current();
uint32_t titlebar_color = theme->titlebar_active;

// Or use convenience macros
draw_rect(x, y, w, h, THEME_BUTTON_BG);
```

### Adding a New Theme

See `docs/THEMES.md`, "Adding a theme" - write `/THEMES/<slug>.mtheme`,
list it in `/THEMES/INDEX.TXT`, activate it. No kernel rebuild, no reboot.

---

## Appendix A: Color Conversion Reference

| Format | Example | Notes |
|--------|---------|-------|
| Hex (theme.ini) | `0x4B6983` | 0xRRGGBB format |
| Hex (CSS) | `#4B6983` | #RRGGBB format |
| RGB | `75, 105, 131` | Decimal RGB |
| ARGB | `0xFF4B6983` | With alpha channel |

---

## Appendix B: Quick Reference Card

### Retro UNIX At-a-Glance
- Titlebar: 20px, solid color
- Borders: 2px 3D beveled
- Corners: Square (0px radius)
- Buttons: 3D raised/sunken
- Shadows: None
- Animations: None

### Modern Light At-a-Glance
- Titlebar: 28px, translucent
- Borders: 1px subtle
- Corners: 10px radius
- Buttons: Flat with subtle shadow
- Shadows: Soft drop shadows
- Animations: 150-200ms ease

### Fluent Light At-a-Glance
- Titlebar: 32px, Mica material
- Borders: 1px with accent bottom
- Corners: 8px radius
- Buttons: Flat with bottom border
- Shadows: Elevation-based
- Animations: 167ms standard

---

*This style guide is a living document. Updates should be coordinated through the UI Lead.*

---

# mtheme v2: the file format the UI is actually driven by (#711)

Everything below this line is DATA on the image, not source. Changing any of it
needs no compiler, no deploy and no reboot: edit
`/THEMES/<active-slug>.mtheme` on a running system and the compositor picks the
change up within about two seconds.

This document remains human-only. Nothing in the kernel or userland reads it at
runtime or build time. The machine-checkable half of it is
`build/assets/theme-scale-lint.sh`.

## Where the files are

| What | Path on the image | Source of truth in git |
|---|---|---|
| One theme, complete | `/THEMES/<slug>.mtheme` | `build/assets/themes/<slug>.mtheme` |
| Load order (fixes the numeric id) | `/THEMES/INDEX.TXT` | `build/assets/themes/INDEX.TXT` |
| Which theme is active | `/CONFIG/THEME.CFG` -> `active=<slug>` | `build/assets/THEME.CFG` |

`INDEX.TXT` order is load-bearing: line 1 (`retro_unix.mtheme`) is theme id 0.
Append only.

`/CONFIG/THEME.CFG` has exactly ONE writer, `userland/libc/gui_theme.c`, and one
schema, `active=<slug>`. The kernel-side writer that used to emit
`theme=`/`index=` to the same filename (the #683b two-writer collision) was
deleted in #711, not extended.

## Syntax

Flat `key=value`, one per line. `#` or `;` starts a comment. Unknown keys are
ignored (forward/backward compatible). A value is always a LITERAL: an int, or
an `0x00RRGGBB` hex, or one of a tiny closed word set. There are no token
references inside a file, so the Ring 0 parser resolves nothing, recurses into
nothing and allocates nothing.

A key you omit is filled from a compiled fallback, and every fallback is
DERIVED from the 51 legacy colour keys where one carries the same meaning. That
is what makes v2 a strict superset: a pre-v2 `.mtheme` still produces a
complete, correct theme and renders exactly as it did.

## Token model

Designers write TOKEN NAMES everywhere except the two colour-scheme files. Only
the colour-scheme designer writes a hex.

### color.* (semantic)

`surface_sunken` `surface` `surface_raised` `surface_overlay` /
`on_surface` `on_surface_muted` `on_surface_disabled` / `accent` `on_accent`
`accent_hover` `accent_active` / `danger` `on_danger` / `border_subtle`
`border_strong` / `focus_ring` / `sel_bg` `sel_fg` / `titlebar_top`
`titlebar_bottom` `titlebar_inactive_top` `titlebar_inactive_bottom`
`titlebar_text` `titlebar_text_inactive`.

### state.*

Every interactive control defines exactly six states: rest, hover,
active (pressed), focus, disabled, selected. Each is an EXPLICIT token value.
No state may be specified as "the same colour at 20% alpha", because there is
no per-pixel alpha in this renderer to express that with. Families: `btn_`,
`item_`, `input_`.

### Elevation

Four levels: 0 sunken, 1 base, 2 raised, 3 overlay. Each level IS a
(surface token, border token) pair. Level 3 additionally takes
`border_strong`. In a dark scheme higher is a lighter tint; in a light scheme
higher is nearer white. **This is the entire elevation model. There are no
shadows** (removed by explicit decision, #189, and the renderer has no
soft-shadow primitive).

### Scales

- **radius**: none 0, sm 3, md 6, lg 10, pill = height/2. Window OUTER corners
  are always 0.
- **spacing** (4px grid): xs 4, sm 8, md 12, lg 16, xl 24, xxl 32. Every
  padding, gap and margin is one of those six.
- **type** (whole px, on the glyph-cache ladder): caption 11, body 14,
  body_strong 14 bold, title 16, heading 20, display 28. lineheight =
  round(size * 1.4). Weights: regular and faux-bold only.
- **metric** defaults for a modern theme: `titlebar_h=28`, `border_w=1`,
  `btn_h=28`, `input_h=28`, `focus_w=2`. `retro_unix` keeps its own values so
  the CDE look is untouched.

### decor.*

`decor.style` = `beveled` | `flat` | `gradient`; `decor.titlebar_gradient`
0/1; `decor.grip` 0/1. Before #711 the gradient-vs-flat decision was a
case-insensitive substring match on the theme's display NAME in
`kernel/gui/window.c`; it is a field now.

## What the renderer cannot do (do not specify these)

- Drop shadows of any kind.
- Blur, frosted glass, vibrancy, or any effect on live content.
- Translucent persistent surfaces or per-pixel-alpha layers. The only opacity
  is a uniform per-draw scalar. Full-screen translucent scrims are additionally
  banned: a whole-screen damage rect forces the cli-gated framebuffer copy and
  stalls input system-wide.
- Radial, multi-stop, horizontal or diagonal gradients. The only gradient is
  2-stop vertical linear; keep the stop delta subtle (max about 12% luminance).
- Antialiased lines, circles, arcs, ellipses or polygons via any public call.
  Circles are hard-edge Bresenham: use them at 14px or larger, or not at all.
- A stroked/outlined rounded rect. Fill only. A rounded border is the
  nested-fill technique: outer fill in the border colour, inner fill inset by
  the border width.
- Bilinear or any smooth bitmap scaling. Nearest-neighbour only, so icon art
  must be authored at its shipped sizes.
- Subpixel-positioned or hinted text. Whole-pixel only: no body text below
  12px, no caption below 11px.
- Font weights beyond regular and faux-bold. Avoid faux-italic in UI chrome.
- Animations or transitions. There is no timing system.

Anything needing a NEW primitive (a public antialiased line by promoting
`wdg_line`, a shared alpha blit, a radial gradient) is scoped engineering work
and must be raised as such, never assumed free.

## Checking a theme

```
build/assets/theme-scale-lint.sh --self-test   # prove the lint can fail
build/assets/theme-scale-lint.sh               # lint the shipped set
```

The twelve pre-#711 themes carry `lint.baseline=legacy-v1`, which turns their
findings into a counted, printed suppression instead of a failure (the same
device as the concurrency-lint allowlist's `[LEGACY]` tag). A NEW theme has no
such line, so it must satisfy the contract from the start.


# Window decorations: loop-1 spec (family 1)

Owner: window decorations (titlebar, close/min/max/filter buttons, active vs
inactive frame, border, resize grip, the winmenu decorator popup). This
section documents what changed this loop, in tokens, and is the spec other
designers/engineering can build against. It does not touch content-area
widgets (families 2-3), shadows/translucency (banned, see "What the renderer
cannot do" above), or the compositor-drawn taskbar/menus (family 6).

## What was already correct going in

The #711 substrate had already done most of this family's job: the winmenu
popup reads six theme fields off `theme_get_current()` instead of the old
`switch(theme_get_current_id())` eight-palette hand-copy; every shipped theme
already carries `decor.style`, `decor.titlebar_gradient`, `decor.grip`,
`metric.titlebar_h`, `metric.border_w`, `metric.winmenu_*`,
`color.titlebar_top/bottom/inactive_top/inactive_bottom/text/text_inactive`,
`color.border_subtle/strong`, `color.focus_ring`, and `type.title` (16px
regular, unchanged - the type scale is a fixed global ladder, not
per-theme). `retro_unix.mtheme` is untouched: `titlebar_h=20`, `border_w=2`,
`decor.style=beveled`. The two modern reference themes already carried the
requested spec: `titlebar_h=28`, `border_w=1`, `decor.style=gradient` with a
subtle 2-stop vertical gradient (`titlebar_top`/`titlebar_bottom`).

## Six-state gap found and closed (data side)

`window.c` draws the titlebar's filter/minimize/maximize buttons in exactly
one state (rest: `fb_fill_rect(..., tb_col)`) and the close button in two
(rest = `close_button`, hover = `close_button_hover`, both pre-#711 legacy
keys). No button tracks active/pressed. Mapped against the six-state
contract for these controls:

| State | Token | Notes |
|---|---|---|
| Rest | titlebar fill (`tb_col`, already the active/inactive gradient/flat colour) | min/max/filter have no bg of their own at rest, they read as part of the titlebar |
| Hover | `color.titlebar_btn_hover` (new, this loop) | copied verbatim from `state.btn_hover_bg` in the same file - not a new invented colour |
| Active/pressed | `color.titlebar_btn_active` (new, this loop) | copied verbatim from `state.btn_active_bg` |
| Focus | `color.focus_ring`, at the WINDOW level | a per-button focus ring on a titlebar control has no precedent in this renderer and no user benefit; focus is already expressed by the active/inactive titlebar swap. Applies to the window as a whole, see the border finding below. |
| Disabled | button simply not drawn | already the pattern for `WINDOW_FLAG_CLOSABLE`; extend the same convention rather than drawing a dead-looking control users might still click |
| Selected | not applicable | these are momentary action buttons, not toggles |

Close button additionally gets `color.titlebar_close_active` (new, this
loop): `close_button` mechanically darkened 15% per channel, the same kind
of deterministic derivation the substrate already used for the titlebar
gradient's "lift 22% toward white" - not an aesthetic pick.

All three new keys are written into all 13 files (12 shipped themes +
retro_unix carries its own reproduce-current-pixels value). **Status: data
only.** `kernel/gui/themes.h`'s `TM_*` enum and `kernel/gui/themes.c`'s
`g_theme_fields` table have no entries for `color.titlebar_btn_hover`,
`color.titlebar_btn_active`, or `color.titlebar_close_active` yet, so today's
parser reads past these lines and stores them nowhere, and `window.c` has no
per-button mouse-over/pressed tracking to consume them even once wired. This
is scoped engineering work, not assumed free: add three `offsetof` entries
+ enum values (mechanical, follows the existing pattern exactly), then track
hover/press per titlebar button the same way widgets already do in the
content area, then read `color.titlebar_btn_hover/active` in the three
`fb_fill_rect` calls that currently always pass `tb_col`, and
`color.titlebar_close_active` in the close button's press state.

## Title inset: new `metric.title_inset`

`window.c` places the title text at a hardcoded `x + BORDER_WIDTH + 4`. This
loop's brief calls for `space.md` (12px) inset on the modern spec. Added
`metric.title_inset` to all 13 files: `4` everywhere except `maytera_dark`/
`maytera_light`, which get `12`. The `4` values reproduce every other
theme's current pixel output exactly (including retro_unix, unchanged per
the brief), so wiring this key changes nothing until a theme opts in by
raising the number. **Status: data only**, same as above - no `TM_*`/
`g_theme_fields` entry exists yet, and `window.c`'s title-x calculation
would need `win_metric_or(TM_TITLE_INSET, 4)` in place of the literal `4`.

## Bug found: `color.focus_ring` is dead for window borders

Grepped `kernel/gui/window.c` for `focus_ring`/`c_focus_ring`: zero matches.
The token is parsed into `theme_t.c_focus_ring` (offset 15) and consumed by
four *other* state tokens (`s_btn_focus_ring`, `s_item_focus_ring`,
`s_input_focus_ring` derive from it), but nothing in window chrome reads it.
`win->border_color` is set once at window creation
(`THEME_WINDOW_BORDER`) and never re-evaluated on focus change - only
`win->titlebar_color` toggles between `THEME_TITLEBAR_ACTIVE`/`_INACTIVE`
(window.c:437/444). So the active-window indication this token exists for
(per this loop's brief: "`color.focus_ring` (active-window indication)")
is not visibly happening at the frame/border level today; only the
titlebar's own colour swap carries that signal. **Not fixed this loop**
(requires a `window.c` code change, and only the substrate phase may build
this loop). Spec for the fix: in `window_draw()`, select `ov_border` by
`WINDOW_FLAG_FOCUSED` - active window border = `color.focus_ring`,
inactive = `color.border_subtle` - mirroring the existing titlebar
active/inactive pattern exactly, using tokens already parsed today (no new
key needed).

## WCAG AA fix: `maytera_light` inactive titlebar text

Measured against both gradient stops (`color.titlebar_text_inactive`
`0x007A828C` on `color.titlebar_inactive_top` `0x00F5F6F8` /
`color.titlebar_inactive_bottom` `0x00E8EAEE`, standard WCAG relative-
luminance formula): **3.60:1** / **3.23:1**. Both fail the 4.5:1 floor for
16px regular text (does not qualify for the 3:1 "large text" exception,
which needs 16px+bold or ~24px+regular). This token is in this family's
"chrome group" list, so it is this family's call, not the colour-scheme
designer's. Fixed by darkening the existing colour 25% (k=0.75) while
preserving its hue (122,130,140) to (92,98,105) = `0x005C6269`: **5.70:1**
against the top stop, **5.11:1** against the bottom stop, both AA-clear with
margin. This token is already fully wired (same offset table as
`color.focus_ring`), so the fix is live the moment the file is read - no
engineering follow-up needed for this one.

`maytera_dark`'s equivalent pair measures 11.74:1 (top stop) / 13.88:1
(bottom stop) active, 5.10:1 / 5.39:1 inactive - all pass, untouched.

`retro_unix`'s inactive titlebar (white text on `0x00808080`) measures
**3.95:1**, also below 4.5:1, but is NOT fixed: the brief requires
retro_unix's values stay unchanged, and it already carries
`lint.baseline=legacy-v1` in the design-contract lint (a counted,
grandfathered suppression, not a pass).

## `radius.none` on window outer corners: compliant by construction

`window.c`'s frame draw is `fb_draw_rect(x, y, w, h, ov_border)` - a hard
rect with no radius parameter anywhere in the call path. There is nothing to
break here; no spec change needed, noted only to close out the brief's
explicit non-negotiable.

## `decor.style`'s third value is currently decorative only

`decor.style` is documented as `beveled | flat | gradient`, but `window.c`'s
only read of style is `win_modern_style()`, which is `decor.titlebar_gradient
!= 0` - a binary flag. No shipped theme uses `flat`, and no code path
distinguishes `beveled` from `flat`: both fall into the same
"else: flat fill, unchanged" branch as gradient's off-state. So today
`decor.style` itself is inert prose next to the real (binary) switch,
`decor.titlebar_gradient`. This matches the "already compiles, already
looks plausible, not the CONFIGURED output" failure class this codebase has
hit before (see `blame.md`). Not fixed this loop (would require adding an
actual bevelled-button draw path - 1px light highlight top/left + 1px dark
shadow bottom/right on the titlebar buttons, using `color.border_subtle` for
the shadow edge and a computed highlight - which is new rendering code, out
of a data-only loop). Flagged as scoped engineering work: either make
`decor.style` the single flag `win_modern_style()` reads (three-way) and
give `beveled` a real bevel-edge draw on titlebar buttons, or remove the
`beveled`/`flat` distinction from the documented value set until one exists,
so the file format doesn't promise a visual difference the renderer doesn't
deliver.

---

# Surfaces & Foundation Spec — Loop 1 (family: surfaces, #711 follow-on)

This section is the dependency root for the other UI-modernisation families:
elevation, radius, spacing, separators, gradients and the focus ring are
defined once here in TOKEN NAMES, and every other family's brief is expected
to express its own margins/padding/gaps/corners in terms of these scales
rather than choosing new numbers. It also covers the taskbar, start menu,
tray menu and notification surfaces as elevation + geometry assignments
only (no per-widget styling: that belongs to the families that own those
widgets).

Everything below is DATA on the image (`build/assets/themes/*.mtheme`), not
source, except where explicitly marked PROPOSED/not yet wired. This document
remains human-only; nothing reads it at build or run time. The
machine-checkable half is `build/assets/theme-scale-lint.sh`.

## Elevation model (four levels, no shadows)

Shadows do not exist in this system: removed by explicit decision (#189) and
the renderer has no soft-shadow primitive. Elevation is expressed ONLY as a
(surface token, border token) pair per level:

| Level | Meaning | Surface | Border | Direction |
|---|---|---|---|---|
| 0 | sunken (wells, recessed inputs) | `color.surface_sunken` | `color.border_subtle` | darkest tint (dark scheme) / nearest to background (light scheme) |
| 1 | base (window background) | `color.surface` | `color.border_subtle` | reference tint |
| 2 | raised (cards, toolbars, taskbar) | `color.surface_raised` | `color.border_subtle` | one step lighter (dark) / one step nearer white (light) |
| 3 | overlay (menus, popups, tooltips, notifications) | `color.surface_overlay` | `color.border_strong` (always) | lightest tint (dark) / nearest white, or pure white (light) |

Dark scheme: higher level is a **lighter tint**, strictly increasing:
`surface_sunken` -> `surface` -> `surface_raised` -> `surface_overlay`.
Light scheme: higher level is **nearer white**. In `maytera_light` levels 2
and 3 both land on pure white (`0xFFFFFF`); once a scheme saturates at white
there is no further tint to give, so the level 2/3 distinction is carried
entirely by the border (`border_subtle` vs `border_strong`) — the pair, not
the surface alone, is what disambiguates. This is a legitimate outcome of the
"no shadows" constraint, not a bug: document it wherever a near-white light
theme reaches the same ceiling.

Level 3 (overlay) is **always** bordered with `color.border_strong`, with no
exception, so a popup reads as clearly separated from whatever it sits over
even when its fill is close in value to level 2 beneath it.

## Radius scale

`none=0, sm=3, md=6, lg=10, pill=height/2`. Keys that exist and are read
today: `radius.btn`, `radius.input`, `radius.menu`, `radius.card` (all four
in every shipped `.mtheme`). Application:

- **sm (3)**: small discrete controls — checkboxes, small chips/tags where a
  full pill is not wanted.
- **md (6)**: the default for interactive controls at rest — buttons,
  inputs, menu rows/popups (`radius.btn`, `radius.input`, `radius.menu`).
- **lg (10)**: containers — cards, panels, notification toasts
  (`radius.card`).
- **pill**: `height/2`, a FORMULA applied by drawing code at the control's
  own height, never a stored value. Used for badges/status pills/tags. No
  `radius.pill` key exists or is needed; if a family needs a *fixed* pill
  radius independent of a control's height, that is a new primitive and
  must be flagged as scoped engineering, not assumed.
- **Window outer corners are ALWAYS `none` (0)**, unconditionally. Frames
  are kernel-drawn rects over wallpaper; there is no alpha compositing to
  round them against, and a rounded outer corner would show square wallpaper
  pixels through the "missing" triangle. This is not themeable and should
  never be requested as one.

Rounded borders use the nested-fill technique (the renderer has no stroked
rounded-rect primitive): outer fill in the border colour, inner fill inset
by `metric.border_w`. Any brief that says "1px rounded outline" is asking
for something the renderer cannot draw directly; ask for nested-fill instead.

## Spacing grid (4px base)

Six steps, `space.xs=4, sm=8, md=12, lg=16, xl=24, xxl=32`. **Every margin,
padding and gap in every brief must be one of these six numbers.** No
eyeballed pixel value anywhere.

Two of the six are already live: `metric.pad` (read by the kernel offset
table) is `space.md` (12) in the modern themes, `metric.gap` is `space.sm`
(8). Those two numbers were already being drawn from this scale before it had
a name; this loop gives the scale a name and writes the other four steps
(`space.xs/lg/xl/xxl`) into every shipped `.mtheme` as literal,
forward-compatible data (`build/assets/themes/*.mtheme`, appended after each
file's `type.*` block) — 6 keys x 14 files. **`space.xs/lg/xl/xxl` have no
reader yet**: nothing in `kernel/gui/themes.c`'s `g_theme_fields` consumes a
generic `space.*` key, by design (the format has no token-reference
mechanism, so a family that needs, say, "16px of internal card padding"
should propose a specific new key, e.g. `metric.card_pad`, with `16` as its
literal value and cite `space.lg` in its brief for why 16 and not another
number — the number is what ships, the name is how designers agree on it).
Growing the offset table to add a new `metric.*` reader is scoped
engineering, not a data change, and must be flagged as such the same way
this document flags the taskbar/notification metrics below.

Only padding/margin/gap are bound to this six-step grid. Overall panel
widths and heights (a menu's width, a dialog's default size) are content-
and layout-driven and are **not** required to sit on the 4px grid; do not
force them onto it.

## Separators

A separator is a 1px line in `color.border_subtle`. There is no separate
separator token: this is deliberate, not an oversight — a hairline divider
and the elevation border of the level it sits inside are the same visual
weight by construction, so they use the same colour. Verified already
correct in both flagship themes: `menu_separator` (legacy key) equals
`border_subtle` exactly in both `maytera_dark` (`0x2E2E37`) and
`maytera_light` (`0xE1E4E9`).

Two placements, chosen per-context by the widget that draws the separator
(not specified here beyond the rule):
- **Full-bleed**: edge-to-edge, no inset. Used where the separator marks a
  structural boundary (e.g. the taskbar's top edge against the desktop).
- **Inset**: `space.lg` (16px) from each end. Used inside a list/menu where
  the separator should visually stop short of the container's own border,
  reading as "between items" rather than "another container edge".

## Gradient policy

The only gradient this renderer has is a 2-stop vertical linear one. Rules,
all already true of the shipped `color.titlebar_top/bottom` pair and
enforced here as policy for any future gradient use:

1. **Vertical only.** No horizontal, diagonal, radial or multi-stop
   gradients: the renderer cannot draw them.
2. **Titlebars and optional button rest states only.** Not a general-purpose
   decoration.
3. **Top stop is always the lighter/nearer-white stop.** Light falls from
   above; this direction does not invert between light and dark schemes.
4. **Subtle: keep the stop delta to roughly 12% or under**, measured as HSL
   lightness delta relative to the lighter stop (`abs(L_top - L_bottom) /
   L_top`), not raw hex distance and not WCAG relative luminance (which is
   nonlinear at the dark end and overstates small dark-mode swings by a
   large factor — see the measurement below for why that distinction
   matters in practice).
5. `decor.titlebar_gradient` (0/1) is the field; `decor.style` distinguishes
   `beveled | flat | gradient` as the theme's general chrome idiom. Before
   #711 the gradient-vs-flat decision was a case-insensitive substring match
   on the theme's display NAME; it is a real field now and every theme must
   set it explicitly rather than relying on its name containing the right
   word.

**Measured this loop** (HSL lightness, `L*100`, both 0-100 scale):

| Theme | top | bottom | L(top) | L(bottom) | delta | verdict |
|---|---|---|---|---|---|---|
| `maytera_light` | `0xFAFBFD` | `0xEDEFF3` | 98.63 | 94.12 | **4.6%** | inside budget |
| `maytera_dark` | `0x30303A` | `0x24242B` | 20.78 | 15.49 | **25.5%** | **over budget** |

`maytera_dark`'s titlebar gradient is roughly double the target delta.
Direction is correct in both (top is the lighter stop). This is a
colour-scheme finding, not a metric one: `color.titlebar_top`/
`color.titlebar_bottom` are hex values, and per the token model only the
colour-scheme family writes hexes. Flagging it here rather than editing it:
the colour-scheme family should either pull `maytera_dark`'s two stops
closer together (e.g. toward the `~12%` budget) or record an explicit,
reasoned exception the way the twelve legacy themes carry
`lint.baseline=legacy-v1`.

## Focus ring

`metric.focus_w=2` (px, the ring's stroke width), colour `color.focus_ring`,
drawn as a **square-cornered rectangular outline offset 2px outside** the
control's own edge (never inset, never following the control's own corner
radius — the renderer has no stroked rounded-rect, so the ring is always a
plain rect regardless of what shape it surrounds). Present in every
interactive control's state set as `state.<family>_focus_ring` (`btn_`,
`item_`, `input_` families all define it in both flagship themes today).

**Contrast measured this loop** (focus ring colour against the level-1 base
surface behind it, the most common case — a focused control sitting on the
window background):

| Theme | ring | surface | ratio | AA (>=3:1 for a graphical/UI element) |
|---|---|---|---|---|
| `maytera_dark` | `#2563C9` | `#1A1A1F` | **3.06:1** | passes, narrowly |
| `maytera_light` | `#1856C0` | `#F5F6F8` | **6.20:1** | passes comfortably |

`maytera_dark`'s ring is right at the edge of the 3:1 floor (3.06:1). It
passes today but has almost no margin: a future colour-scheme adjustment to
either `color.focus_ring` or `color.surface` in the dark theme should re-run
this check before shipping, since a small shift either way can drop it below
3:1. Not blocking this loop; noted for the colour-scheme family.

## Body-text and semantic-colour contrast (measured this loop, WCAG AA)

Checked every `on_surface*`/elevation-surface pairing plus the accent,
selection, danger and titlebar-text pairs in both flagship themes (sRGB
relative luminance, the correct metric for WCAG contrast, as opposed to the
HSL-lightness metric used for the gradient-subtlety check above — these are
deliberately different formulas for two different questions: WCAG contrast
vs perceived gradient smoothness).

| Pair | dark | light | AA body (>=4.5:1) |
|---|---|---|---|
| `on_surface` / `surface` | 15.61:1 | 16.44:1 | pass |
| `on_surface` / `surface_raised` | 13.88:1 | 17.77:1 | pass |
| `on_surface` / `surface_overlay` | 12.45:1 | 17.77:1 | pass |
| `on_surface_muted` / `surface` | 6.66:1 | 5.61:1 | pass |
| `on_accent` / `accent` | 5.67:1 | 6.71:1 | pass |
| `sel_fg` / `sel_bg` | 5.67:1 | 6.71:1 | pass |
| `titlebar_text` / `titlebar_top` | 11.75:1 | 17.17:1 | pass |
| `titlebar_text_inactive` / `titlebar_inactive_top` | 5.10:1 | **3.60:1** | dark passes, **light fails body-text AA** (passes the 3:1 large/UI-text floor only) |
| `on_danger` / `danger` | **3.41:1** | 5.06:1 | dark **fails body-text AA** (passes 3:1 UI floor only) |

Two findings for the colour-scheme family, not fixed here (all four values
are hexes, not mine to write): `maytera_light`'s inactive titlebar text
(`#7A828C` on `#F5F6F8`) and `maytera_dark`'s `on_danger`/`danger`
(`#FFFFFF` on `#FF453A`) both clear the 3:1 floor for large/UI text (an
inactive titlebar caption and a danger button's own label both qualify as
such) but neither clears 4.5:1, so neither should be used to set body-copy
sized text at those exact sizes. Every other pairing checked passes AA
comfortably, several at AAA.

## Taskbar, start menu, tray menu, notification surfaces

Elevation assignment and geometry only; no per-widget styling (button
shapes, icon treatment etc. belong to the families that own those widgets).

| Surface | Elevation | Tokens | Radius |
|---|---|---|---|
| Taskbar (persistent dock) | 2 raised | `surface_raised` + `border_subtle` (top edge, full-bleed) | none (a bar, not a card) |
| Start menu (launcher popup) | 3 overlay | `surface_overlay` + `border_strong` | `radius.menu` |
| Tray menu (volume/network/battery popups) | 3 overlay | `surface_overlay` + `border_strong` | `radius.menu` |
| Notification toast | 3 overlay | `surface_overlay` + `border_strong` | `radius.card` |

Start menu and tray menu deliberately **reuse the existing generic menu
metrics** (`radius.menu`, `metric.menu_row_h`) rather than getting their own
`startmenu_*`/`traymenu_*` twins: structurally they are both menus, and
duplicating near-identical keys per surface is exactly the kind of
proliferation the rest of this codebase's "reuse, don't reinvent" rule
exists to prevent. No new key is needed for either.

**Colour for all four is already theme-driven** (`taskbar_bg/hover/active`,
`menu_bg/border/item_hover`, `tooltip_bg` are read today via the legacy
`THEME_*` macros in `kernel/gui/themes.h`). **Geometry is not.**

**Amended #745:** that claim was true of the BACKGROUNDS only. `taskbar_bg`
had no companion ink token, so text drawn on the bar was either derived
per-app (`readable_ink()` in the compositor) or borrowed from a token
contracted against a different background (Settings' left nav borrowed
`label_text`, whose contract is `window_bg`, and rendered dark-on-dark in
Ocean/Forest/Sunset). The bar surface now has real ink tokens, and they are
mandatory for any new theme:

| token | painted on | used by |
| --- | --- | --- |
| `taskbar_text` | `taskbar_bg`, `taskbar_hover` | Settings nav labels + icons, sidebar and modal titles |
| `taskbar_text_muted` | `taskbar_bg` | the Settings version line |
| `taskbar_selected_text` | `taskbar_active` | the selected Settings nav row |

All three are enforced twice: `theme_ensure_all_contrast()` corrects them at
load, and `build/assets/theme-scale-lint.sh` check #9 fails the build at
WCAG AA 4.5:1 (and the 60/255 runtime floor) before they can ship. The nav
ICONS need no separate 3:1 rule: `draw_mico()` tints them to their row's label
colour over the same background, so the icon pair IS the label pair.

Do not paint a foreground on `taskbar_active` in the user's accent colour.
The accent is `ACCENT_COLORS[]` in Settings, not a theme token, so no theme can
guarantee it: measured, 41 of the 112 theme x accent combinations fall below
the runtime readability floor. Express the accent with a non-text mark (the
nav's 3px indicator bar) and take the text colour from a token. Measured
this loop, the compiled constants that would need to move to data:

- `kernel/gui/desktop.h`: `TASKBAR_HEIGHT=36` (the kernel's own dead-code
  fallback taskbar; #552 moved the real one to userland, see below).
- `userland/apps/compositor/taskbar.c`: `LUMINA_MENUBAR_H=24`,
  `CLASSIC_UNIX_PANEL_H=58`, `RETRO_BENCH_BAR_H=20` (three alternate
  compiled taskbar layouts already exist), `TB_BTN_W=120`, `TRAY_ICON_W=26`.
- `userland/apps/compositor/notif.c`: `TOAST_W=320`, `TOAST_H=64`.
- `userland/apps/compositor/traymenu.c`: `TM_W=214`, `SND_W=308`,
  `SND_H=214`.
- `userland/apps/compositor/startmenu.c`: `SM_SCROLL_W=14`.

None of these are readable from a theme file today. Editing a theme value
for them does nothing until `kernel/gui/themes.c`'s `g_theme_fields` gets a
new entry and the compositor's call site reads `SYS_THEME_METRIC` instead of
the `#define`, the exact "mechanism, not instance" approach #711 already
used to convert the five window-chrome geometry macros
(`TITLEBAR_HEIGHT`/`BORDER_WIDTH`/etc). **This is scoped engineering, flagged
here and not implemented by this loop** (a designer writes data files; a
kernel/compositor change is out of this family's remit).

Proposed values (staged, PROPOSED, in `maytera_dark.mtheme` and
`maytera_light.mtheme` only, clearly marked; not added to the 12 legacy
themes to avoid pretending an untested default for styles this loop did not
review):

- `metric.taskbar_h=44` — derived from `metric.btn_h`(28) + 2 x
  `space.sm`(8), giving the bar one `space.sm` of breathing room above and
  below its own buttons; up from the compiled 36. When wired, this needs the
  same `retro ? <per-style constant> : 44` fallback pattern every other
  metric field already uses, so the three existing alternate compiled
  layouts keep their current heights until a theme opts in.
- `metric.notif_w=320`, `metric.notif_h=64` — identical to the current
  compiled `TOAST_W`/`TOAST_H`, chosen so wiring the key changes nothing
  visually until a theme actually edits it.
- `metric.notif_pad=12` (`space.md`) for the toast's internal content inset.

## Renderer limits (repeated here as a checklist, not a re-specification)

Do not ask any family's brief for: drop shadows of any kind; blur, frosted
glass, vibrancy, or any effect on live content; translucent persistent
surfaces, per-pixel alpha, or a full-screen scrim (banned outright — a
whole-screen damage rect stalls all input); radial, multi-stop, horizontal
or diagonal gradients; antialiased lines/circles/arcs/ellipses/polygons via
any public call (circles are hard-edge Bresenham, 14px+ or not at all); a
stroked/outlined rounded rect (use nested-fill); bilinear or smooth bitmap
scaling (nearest-neighbour only); subpixel-positioned or hinted text
(whole-pixel, no body below 12px, no caption below 11px); font weights
beyond regular/faux-bold; animations or transitions. A new public primitive
(an AA line, a shared alpha blit, a radial gradient) is always scoped
engineering, never assumed free.

## Summary of what this loop changed

- Added the elevation-level -> (surface, border) table above as an explicit,
  written assignment (previously only level 3's rule was stated).
- Named and completed the spacing grid: added `space.xs/sm/md/lg/xl/xxl` as
  literal data to all 14 shipped `.mtheme` files (`space.sm`/`space.md`
  already existed implicitly as `metric.gap`/`metric.pad`; the other four
  steps are new, forward-compatible, and unread pending a future reader).
- Wrote the separator, gradient-policy and focus-ring specs down as rules,
  and measured every flagship theme against them (see tables above);
  surfaced two colour-scheme findings (`maytera_dark`'s titlebar gradient
  delta and focus-ring contrast margin) and two contrast findings
  (`maytera_light` inactive titlebar text, `maytera_dark` danger text)
  without editing any hex, since hexes are the colour-scheme family's token.
- Assigned elevation levels and proposed (not yet wired) geometry for the
  taskbar, start menu, tray menu and notification surfaces, and named the
  exact compiled constants and files a future engineering pass needs to
  touch to make that geometry live.

Ran `build/assets/theme-scale-lint.sh` before and after: unchanged result
(`2 clean, 12 grandfathered, RESULT: OK`) — the new `space.*` keys are
inert to the lint as well as to the runtime, by design.

## Loop 2 (director redirect, wiring + repair): what changed and what did not

Loop 2's brief (Section A) is explicit that this is not a polish pass: close
the P0/shared-token list before anything else. This family's Loop 2 work is
entirely inside Section B (shared token decisions) and Section D1 (chrome/
elevation shadows+scrim+inactive-titlebar), since surfaces owns the radius
scale, the elevation model and the focus-ring spec. No P0 item in Section C
is assigned to this family by name; the redirect's own text confirms none of
C1-C9 says "surfaces".

**B6 (radius) - a real, LIVE value change, not a spec update.** `radius.btn`,
`radius.input` and `radius.menu` are WIRED keys (`kernel/gui/themes.c`'s
`g_theme_fields` has `TM_RADIUS_BTN`/`TM_RADIUS_INPUT`/`TM_RADIUS_MENU`
offset-table entries - confirmed by grep before editing, not assumed). They
moved 6px -> 4px in `maytera_dark.mtheme` and `maytera_light.mtheme` only
(the twelve legacy/pre-#711 themes are untouched, consistent with every
other family's "modern themes only" pattern this loop). `radius.card` stays
10: B6's own list is "buttons, inputs, dropdowns, menus, selection-row
highlights", which does not include self-contained cards.

`radius.control` (Family 3, loop 1: the checkbox box radius, `=3`) is the
**same token name** B6 uses for its settled 4px value, a genuine collision
between an earlier per-widget choice and this loop's shared decision. This
is a shared token I own the scale for, so I raised the existing key to 4px
in place rather than inventing a second name - the checkbox box corner
moves 3px -> 4px as a side effect, which is in the spirit of "one control
radius" even though checkboxes are not named in B6's list. `radius.control`
itself has no `g_theme_fields` entry yet (confirmed by grep), so this is
data ready for a reader, not a live change today, unlike the three keys
above it.

**B9 (focus ring) - width already correct, offset was never a key.**
`metric.focus_w=2` already matches B9's "2px ring" and is already WIRED
(`userland/libc/gui_style.h`'s `GUI_FOCUS_W` reads it via
`theme_metric_or(THEME_METRIC_FOCUS_W, ...)` - a genuinely live consumer,
confirmed by grep, one of the few `metric.*` fields that is). The "1px
outside the control" half of B9 had never been encoded as a key at all -
loop 1's brief described "2px offset" only in prose, never as data, so
nothing regresses by adding the real value now. Added
`metric.focus_offset=1` to both modern themes, unwired (no reader yet,
flagged, not claimed live).

**B8 (elevation: shadow bands, scrim, inactive titlebar) - investigated
against actual draw code, not restated from the brief.** My own loop-1
brief says "NO SHADOWS EVER... unrenderable" and the task's renderer-limits
section bans full-screen scrims; B8 asks for both. Rather than picking a
side by assertion, I read `kernel/gui/window.c`'s `wm_draw_winmenu()` and
`userland/apps/compositor/notif.c`'s toast draw path:

- The window-decorator popup **already ships** a hard-edged, opaque,
  offset-rect "shadow": `fb_fill_rect(x+3, y+3, w, h, 0x00101418)` drawn
  before the popup body. Zero alpha, zero new primitives - this is what
  #189 removed *soft/blurred* shadows and left standing, and it is exactly
  the "3px band" B8 describes for menus.
- The notification toast **already ships** the soft version at small
  scale, using the compositor's existing uniform per-draw alpha
  (`g_draw_blend=70` of 255, about 27%) on an offset rounded rect - within
  a rounding error of B8's stated 25%, and squarely inside my own brief's
  "uniform-opacity dim... allowed ONLY for small regions" exception.

Both techniques are proven, shipping, and require no new renderer work, so
I added the geometry as new tokens (`color.elevation_edge=0x00000000`,
`metric.elevation_edge_w_menu=3`, `metric.elevation_edge_w_modal=6`,
`metric.elevation_edge_alpha_menu=64`, `metric.elevation_edge_alpha_modal=
89`) with consumer guidance in the `.mtheme` comment: kernel draw sites
reuse the opaque `wm_draw_winmenu` technique, compositor draw sites reuse
the `g_draw_blend` technique already proven in `notif.c`. Wiring these into
the start menu / tray menu (which do not yet have either technique applied)
is scoped engineering - applying an existing, proven pattern to more call
sites, not a new primitive - and is **not done this loop**.

The **full-screen 30% scrim behind a modal is not implemented.** The
primitive exists (`kernel/gui/login.c`'s `fb_fill_rect_alpha` at full
screen size, `LOGIN_SCRIM_ALPHA=145`), so this is not a "the renderer
can't do it" refusal. The reason my brief bans a full-screen scrim is
**redraw cost**, and login.c's own comment says why it gets away with one:
"blit plus a scrim is too expensive for a per-frame redraw, #426" - true
only because the login screen is static underneath. Whether that holds for
a modal over the live desktop depends on whether opening a modal **freezes
repaint of everything below it**. I did not verify this in the
compositor's damage-rect code (not this family's code, not checked this
loop) and I am not implementing the scrim until it is answered - doing so
wrong reintroduces exactly the input-stall class #426 already paid for.
Flagged for the Substrate phase, not the director's design call to make
blind either.

**Inactive titlebar hex values (the other half of B8).** Not this family's
tokens - `color.titlebar_text_inactive`/`titlebar_inactive_top/bottom`
belong to the window-decorations family, who already ran a real AA pass on
them in loop 1 (3.23:1-3.60:1 -> 5.11:1-5.70:1, commit `a8e1bec`). B8's
literal dark target (`#1B1B20`) is a <1% luminance difference from this
file's current `titlebar_inactive_bottom` (`0x001A1A1F`) - almost
certainly the same design intent under a slightly different literal. B8's
light target ("step down from `#EDEFF3`") matches this file's current
`titlebar_inactive_bottom` (`0x00E8EAEE`) exactly one step down from the
active `titlebar_bottom` (`0x00EDEFF3`). Flagged as likely-already-done for
that family/the director to confirm; not claimed done here since these are
not my tokens to certify.

**`radius.pill` / "No pills" (B6) - a real cross-family conflict, not
resolved unilaterally.** Family 2 (buttons, loop 1) added a full
`btn.pill.*` six-state block plus `radius.pill=14`, uncommitted as of this
loop. B6 says "No pills" for Loop 2. `radius.pill` is a radius-scale token
I own; the `btn.pill.*` colours that consume it are Family 2's file section
and not mine to edit or delete. I left both as-is and flagged it in both
`.mtheme` files: either Family 2's pill variant is withdrawn from Loop 2
scope, or B6 needs an explicit carve-out for it. `radius.pill` itself is
**not** deprecated as a scale rung - `radius.scrollbar_thumb=8` (Family 3,
already shipping) is the identical "pill cap" concept applied to a
scrollbar thumb, not a button, and outside B6's "no pills" clause, which
reads as being about button shape specifically.

**B7 (spacing) - no change needed.** `space.xl=24` and `space.sm=8` already
existed from loop 1 and are exactly the two values B7 asks for (Settings
section-to-section / intra-section gaps). Confirmed, not re-specified.

**Separators - no change needed.** Loop 1's spec (1px, `color.border_subtle`,
full-bleed vs `space.lg` inset) already matches this loop's direction;
restated in the `.mtheme` files' new comment block for a reader landing at
the bottom of the file, not duplicated as a new key.

`build/assets/theme-scale-lint.sh` after all edits: unchanged,
`2 clean, 12 grandfathered, RESULT: OK` - the new geometry tokens are
outside what the lint checks (space/radius/type scale membership and
contrast pairs), same as loop 1's `space.*` additions.

### Shared-token asks for the director (this family cannot resolve these alone)

1. **`radius.pill` vs B6 "No pills"**: does Family 2's pill button variant
   stay in scope with an explicit exception, or is it withdrawn? I did not
   delete their uncommitted work to force an answer either way.
2. **Full-screen modal scrim**: does the compositor suspend repaint of
   everything under an open modal? If yes, the scrim is a cheap one-time
   draw and my brief's blanket ban can be narrowed for this one case; if
   no, the ban should stay and B8's scrim clause should be dropped or
   rewritten as the edge-band-only treatment I did implement.
3. **Inactive titlebar hex confirmation**: window-decorations family should
   confirm `titlebar_inactive_bottom` (dark `0x001A1A1F`, light
   `0x00E8EAEE`) already satisfies B8's `#1B1B20`/"-1 step from #EDEFF3"
   language, or state the precise target if not.


## Family 3 (Form Controls): checkbox, radio, dropdown, slider, field, scrollbar

Loop 1 additions to `maytera_dark.mtheme` / `maytera_light.mtheme`, geometry
identical between the two files (only colour differs by scheme, same pattern
as every other metric/radius pair). All six explicit states (rest, hover,
active/pressed, focus, disabled, selected) are covered per control below;
where a state genuinely does not apply it is documented with its fallback,
never guessed via alpha.

### What is a pure re-skin (zero new colour keys)

- **Text field**: `state.input_*` verbatim (already parsed today). Selected =
  the in-field text-selection highlight (`state.input_selected_bg/fg`).
- **Checkbox**: box states = `state.input_*` verbatim. Checked ("selected")
  = `state.input_selected_bg` (fill) / `state.input_selected_fg` (glyph
  colour). New geometry only: `metric.checkbox_box=16`, `radius.control=3`.
- **Dropdown/combobox closed box**: `state.input_*` verbatim; "selected" is
  N/A for the box itself (no checked concept), falls back to rest.
- **Dropdown popup rows**: `state.item_*` verbatim (rest/hover/active/focus/
  disabled/selected = the current choice) - this is exactly what `item_*` was
  already shaped for. Popup surface is the level-3 overlay pair
  (`color.surface_overlay` + `color.border_strong`), consumed here, not
  redefined; elevation stays this family's call, not form-controls'.
- **Radio background**: `state.input_rest_bg` (one constant fill behind the
  ring, same "recessed field" language as checkbox/text field).

### New tokens (control-specific, additive to the existing scale)

`state.field_invalid_border` / `state.field_placeholder_fg` /
`state.field_caret`; `state.radio_bg` / `state.radio_ring_{rest,hover,
active,focus_ring,disabled,selected}` / `state.radio_dot_{selected,
disabled}`; `state.slider_track` / `state.slider_fill{,_disabled}` /
`state.slider_thumb_{rest,hover,active,focus_ring,disabled}`;
`state.scrollbar_track` / `state.scrollbar_thumb_{rest,hover,active,
focus_ring,disabled}`. Every value is a literal copy of an existing
`color.*`/`state.input_*` token in the same file (see the `# = tokenname`
comment on each line in the `.mtheme` files) - nothing here is an invented
hex.

New geometry: `metric.radio_d=14` (Bresenham-circle legibility floor from the
brief, not a style choice - below it a filled circle looks ragged),
`metric.radio_dot=6`, `metric.radio_ring_w=2` (nested-fill ring stroke);
`metric.dropdown_row_h=32` (roomier than the existing `metric.menu_row_h=28`,
which window/system menus keep); `metric.slider_track_h=4`,
`metric.slider_thumb_w=12`, `metric.slider_thumb_h=18`,
`radius.slider_thumb=3`; `metric.scrollbar_thumb_min=24` (usability floor,
not style), `radius.scrollbar_thumb=8` (= `metric.scrollbar_w`/2, the `pill`
scale rule computed per-element).

Slider and scrollbar thumb are the only draggable element in each control;
their track is a static one-colour groove with no hover/press states of its
own - dragging anywhere on the track hands off straight to the thumb's
active state. "Selected" is N/A for both (continuous/no discrete-choice
concept) and aliases to rest (slider) / active (scrollbar, since dragging is
the only extra state a thumb has beyond rest/hover).

### Checkbox check-glyph pixel spec

No public call draws an antialiased line, so the check mark is a stepped 2px
polyline built from eleven 2x2 filled rects (colour
`state.input_selected_fg`), inside the 16x16 box, local coordinates, origin
at the box's top-left corner, Bresenham-plotted through the pivot at (6,10):

```
short leg (down-right): (3,7) (4,8) (5,9) (6,10)
long leg  (up-right):   (6,10) (7,9) (8,8) (9,7) (9,6) (10,5) (11,4) (12,3)
```

(6,10) is shared by both legs; each point is the top-left of a 2x2 rect.
This is deliberately a compiled constant, not a new `.mtheme` key: the
defining constraint asks for the checkbox's *design* (size, colour, border,
radius) to be data, and all of that now is; the glyph shape is treated the
same way this codebase already treats font glyphs - a small fixed bitmap
next to data-driven colour and metrics, not a case that needs its own
data format. Flagged explicitly rather than assumed: if a future loop wants
the mark itself re-skinnable without recompiling too, that needs a genuinely
new primitive (a semicolon-separated rect-list key plus a consumer that
loops a fill-rect over it) - scoped engineering work, not something this
loop's file content can deliver alone.

### Engineering asks (not deliverable as data alone)

- `kernel/gui/widget.h`'s `WIDGET_*` enum has only `LABEL`/`BUTTON`/
  `CHECKBOX`/`TEXTBOX`. Radio, slider and dropdown/combobox are not
  implemented as live, theme-reading widgets today (confirmed: zero callers
  outside `kernel/gui/theme.c` for `theme_draw_radio_button`, which is the
  same dead "engine A" `theme_save_config()` was found in at #711 - it
  compiles into the kernel binary but nothing in the live compositor path
  calls it). New widget types plus draw routines are needed to consume the
  tokens specified above.
- Checkbox and text field DO have live draw code today
  (`widget_draw_checkbox`, the textbox path), but it reads only the three
  legacy `checkbox_*` fields / four legacy `textbox_*` fields, not the six
  explicit v2 states. Repointing them at `state.input_*` (already a compiled
  `theme_t` field, no struct/ABI change) is the smallest version of this
  ask.
- Scrollbar has live legacy `scrollbar_bg/thumb/thumb_hover` fields; migrating
  its draw code to the new `state.scrollbar_*`/`radius.scrollbar_thumb`/
  `metric.scrollbar_thumb_min` keys needs new `theme_t` fields (append-only,
  same pattern as the #711 struct additions) plus draw-code changes.
- `build/assets/theme-scale-lint.sh`'s radius-scale check only iterates
  `btn input menu card`. Request to whoever owns the lint: extend that list
  with `control slider_thumb`, and treat `scrollbar_thumb` as a `pill`
  exemption (checked against `metric.scrollbar_w`/2 rather than the fixed
  0/3/6/10 set) rather than leaving these three keys unchecked by omission.

### Contrast measured (WCAG AA, 4.5:1 floor for normal text)

Computed independently with the same relative-luminance formula the lint
uses, on the actual values shipped in both files:

| Pair | maytera_dark | maytera_light |
|---|---|---|
| `state.input_rest_fg` on `state.input_rest_bg` (field body text) | 15.61:1 | 16.44:1 |
| `color.on_surface_muted` on `color.surface` (placeholder text) | 6.66:1 | 5.61:1 |
| `state.input_selected_fg` on `state.input_selected_bg` (check glyph / dropdown row selection / slider-adjacent accent text) | 5.66:1 | 6.70:1 |

All pass with margin in both schemes. `color.on_surface_disabled` on
`color.surface` (disabled field/checkbox/radio text) measures 2.86:1 dark /
similar in light - below 4.5:1 but not a defect: WCAG 1.4.3 explicitly
exempts inactive/disabled UI components from the contrast minimum, and no
disabled-state pair here is in the lint's checked list for that reason.
Border-only pairs (`color.danger` invalid-field border, `color.focus_ring`)
are not text and are not evaluated against the 4.5:1 text floor, only for
visible distinctness against `color.surface`/`color.surface_sunken`, which
both are (danger and focus_ring are saturated hues against a near-neutral
background in both schemes).

### What I did not touch

No hexes were invented (every new value is a documented copy of an existing
`color.*`/`state.input_*` token). No shared elevation, chrome or button
tokens were redefined - popups consume `color.surface_overlay`/
`color.border_strong` as already defined, buttons are untouched. Legacy
themes (the twelve pre-#711 files) were not edited; `maytera_dark.mtheme`
and `maytera_light.mtheme` remain the only two files this family's tokens
were added to, matching the "new themes are appended, never inserted"
scoping the substrate already established.

# Buttons: the shared button family (Loop 1, family: buttons, #711 follow-on)

Owner: buttons family (loop 1). This section documents the `btn.*` keys
appended to `build/assets/themes/maytera_dark.mtheme` and
`build/assets/themes/maytera_light.mtheme` this loop: standard, primary,
danger, toolbar/icon, pill, and segmented-control buttons, each with all six
states (rest/hover/active/focus/disabled/selected) as explicit token
assignments. As with every other family this loop, every value written is a
literal copy of an already-defined `color.*`/`state.*` token (see the
`# = tokenname` comment on each `.mtheme` line) - this family does not
invent hexes, and does not touch the `metric.*`/`radius.*`/`type.*` blocks
another family owns except where noted below as a flagged gap.

## Why buttons need their own namespace

Before this loop, `state.btn_*` was the *only* button spec in mtheme v2: one
push button, one set of six states. It is kept, unmodified, as the
`standard` variant below (identical literals, `btn.standard.*` just adds the
border tokens `state.btn_*` never carried). Everything else - a filled
primary action, a destructive action, a borderless icon button, a rounded
filter chip, a connected segmented control - needed its own state set
because a single generic button cannot represent "this is the default
action" or "this is destructive" through geometry alone; that distinction
has to be colour, and colour has to be data per this effort's defining
constraint.

## The nested-fill technique, used by every variant

The renderer has no stroked/outlined rounded rect - only filled rects. A
bordered button is drawn as two filled rects: an outer one in the state's
`.border` colour, then an inner one inset by `metric.border_w` (1px) on
every edge in the state's `.bg` colour. Where `.border` equals `.bg` the
button reads as flush/borderless by construction - the renderer does not
need a separate "no border" flag, it is simply two identical fills. This is
the same technique Family 3 specified for checkbox/radio rings.

Focus is drawn separately from the button's own fill: `btn.<variant>.focus
.ring` is a `metric.focus_w` (2px) rect *outline*, offset 2px *outside* the
button's bounds. Square corners are used for the ring even on the
pill/segmented variants (explicitly allowed - the renderer cannot draw an
antialiased rounded outline, and a hard-edged rounded stroke at this radius
would look worse than a square one, not better).

## The six variants

| Variant | Shape | Fill model | Label role |
|---|---|---|---|
| `btn.standard` | `radius.btn` (6) | Neutral surface, mirrors legacy `state.btn_*` | `type.body` |
| `btn.primary` | `radius.btn` (6) | Solid `color.accent` fill | `type.body_strong` |
| `btn.danger` | `radius.btn` (6) | Solid `color.danger` fill, constant across states (see below) | `type.body` |
| `btn.toolbar` | `radius.btn_toolbar` (3, new) | Flush at rest, fills on hover, sinks to elevation-0 on press | `type.body` (usually icon-only, no label) |
| `btn.pill` | `radius.pill` (14, new - see gap below) | Same neutral fill as standard, fully rounded | `type.body` |
| `btn.segmented` | `radius.btn` (6) on the group's outer ends only | Muted-unselected / accent-filled-selected, 1px dividers between segments | `type.body` |

Geometry shared by all six: height `metric.btn_h` (28), horizontal padding
`metric.btn_pad_h` (16, new = `space.lg`), icon-only padding
`metric.btn_pad_icon` (4, new = `space.xs`), gap between grouped buttons
`metric.btn_gap` (8, new, numerically identical to the existing
`metric.gap` but named explicitly so a button-group layout has a citable
key of its own rather than borrowing a generic one).

No gradient was added to the rest-state fill, even though the brief allows
an optional single 2-stop vertical one. A titlebar gradient is already this
theme's one signature gradient move (surfaces family's gradient-policy
section, above); repeating it on every button would dilute that hierarchy
rather than reinforce it, and it would double the token count per variant
(a `_bg_top`/`_bg_bottom` pair instead of one `_bg`) for a decorative gain
this loop judged not worth the added surface. Flat fills plus the nested-
fill border already carry the elevation read.

## Danger button: fill is constant, the state read comes from the border

`color.danger`/`color.on_danger` is the only fill this variant has - there
is no `color.danger_hover`/`color.danger_active` token (unlike
`color.accent`, which has both). Rather than leave hover and pressed
visually identical to rest (which the brief calls out as "the most common
way these efforts look unfinished"), the *outer* nested-fill ring moves
instead of the fill: flush at rest, `color.border_strong` (a dark defining
ring) on hover, `color.on_danger` (a bright ring) on press. All three are
genuinely distinct, named tokens - never an alpha trick - even though the
inner fill itself does not change. This is intentionally the same
mechanism used for `btn.danger`'s hover/active in both schemes, so the
button behaves identically in light and dark, only the base hue differs.

**Ask for a future loop:** `color.danger_hover` and `color.danger_active`,
matching the existing `color.accent_hover`/`accent_active` pattern, would
let a future pass give this variant a real fill-darkens-on-press treatment
instead of the ring-based one above. Not blocking - the ring technique is a
complete, working six-state spec today - but flagged as the ideal fix.

## Contrast measured this loop

`color.on_danger` (white) on `color.danger`, the button's own label/fill
pair, at every state (the fill is constant across rest/hover/active, see
above):

| Theme | Ratio | Pass/fail (WCAG AA, 4.5:1) |
|---|---|---|
| Maytera Dark | **4.58:1** | Pass (independently recomputed; matches the colour-scheme family's own 4.57:1 figure for the same pair) |
| Maytera Light | **5.07:1** | Pass |

Dark theme passes only because the colour-scheme family (Family 4)
deepened `color.danger` from `0xFF453A` to `0xD93A2F` earlier this same
loop, specifically to clear this exact floor (their own comment in
`maytera_dark.mtheme` explains the fix; 3.41:1 before, 4.58:1 after). This
family's own contrast pass would otherwise have had to flag the identical
defect - it is recorded here as confirmed-fixed, not as an open finding.

`color.on_accent` (white) on `color.accent_hover`, the tightest primary-
button pair (hover is *lighter* than rest in dark mode, which reduces
contrast versus white text - the pair most likely to fail if anything
does):

| Theme | Ratio | Pass/fail |
|---|---|---|
| Maytera Dark | **4.73:1** | Pass (tightest margin of any pair measured this loop) |
| Maytera Light | **5.06:1** | Pass |

`color.on_surface_muted` on `color.surface_raised`, the segmented control's
unselected-segment pair:

| Theme | Ratio | Pass/fail |
|---|---|---|
| Maytera Dark | **5.94:1** | Pass |
| Maytera Light | **6.07:1** | Pass |

`color.on_surface` on `color.surface_raised`, the standard/pill/segmented-
selected-off rest-state text pair, for completeness:

| Theme | Ratio | Pass/fail |
|---|---|---|
| Maytera Dark | **13.9:1** | Pass |
| Maytera Light | **17.8:1** | Pass |

Disabled-state fg/bg pairs (`on_surface_disabled` on any surface token) are
not measured against AA: per WCAG 1.4.3, inactive UI components are exempt,
and this family's disabled treatment intentionally reads as low-contrast to
signal "not interactive," consistent with every other family's disabled
state this loop.

All ratios computed via the standard WCAG relative-luminance formula from
this file's own hex pairs (independently recomputed, not copied from
another family's report, except where explicitly stated as a
cross-check above).

## Two shared-token gaps this family hit and filled, flagged for their owners

1. **`type.body_strong`** - the typography family's own doc section (above)
   states this key is "present in all 14 shipped `.mtheme` files (added
   this loop)" with value 14px/20px/bold. As measured at the time this
   family wrote its own section, it was present in the 12 pre-#711 legacy
   themes but **not yet in `maytera_dark.mtheme` or `maytera_light.mtheme`**
   - the two files every other family's new work lives in. The primary
   button's label needs it (`type.body_strong` per the brief, so the
   default action reads with one deliberate weight step over secondary
   buttons). Synced into both files at the *identical* already-established
   value - not a new number chosen by this family - so the primary button
   spec is not broken by a two-file gap. Flagged to the typography family
   to confirm/reconcile; if they resync these two files independently, the
   values should already agree bit-for-bit.
2. **`radius.pill`** - the token model's own radius scale names five rungs
   (`none/sm/md/lg/pill`), but only four had ever been instantiated as a
   concrete key anywhere (`radius.btn/input/menu/card`, realized as
   `6/6/6/10`). `pill = height/2` is a fixed formula the scale itself
   specifies, not a taste choice: at the modern default `metric.btn_h=28`,
   that is `14`. This family is the first control that is actually pill-
   shaped, so it instantiated `radius.pill=14` in both scheme files rather
   than invent a second, button-local key for the same concept. Flagged to
   whichever family owns the radius scale to confirm/adopt, so a future
   chip or tag control elsewhere reuses this exact key.

## Renderer limits this family did not use

No drop shadow on any button state (elevation is tint + border only, per
`#189`); no gradient on any button fill beyond the flat one shown above (see
"no gradient added," above, for why); no per-pixel alpha for any hover/
press feedback - every state below is a distinct named token, never a
"20% alpha" comment; no stroked rounded-rect primitive - the nested-fill
technique above is the only way this renderer draws a bordered rounded
button; no antialiased or rounded focus-ring corner - the ring is a hard-
edged square rect outline even on rounded variants.

## Engineering gap (flagged, not fixed by this loop)

Same shape as every other family's gap this loop: `kernel/gui/themes.h`'s
`TM_*` enum and `kernel/gui/themes.c`'s `g_theme_fields[]` offset table have
no entries for any `btn.<variant>.*` key, `metric.btn_pad_h`,
`metric.btn_pad_icon`, `metric.btn_gap`, or `radius.btn_toolbar` yet - the
parser reads past them and stores them nowhere, and no userland call site
in `userland/libc/gui.c`/`gui_style.h` yet draws a primary/danger/toolbar/
pill/segmented button distinctly from the single existing generic
`gui_draw_button()`. The data in both `.mtheme` files is complete and
internally consistent (lint-clean, contrast-measured, all six states
present per variant) and ready to be consumed the moment that wiring and
the per-variant draw functions exist; it changes nothing on a booted system
today beyond what `state.btn_*` already drove. This is scoped engineering
work for whoever wires per-variant button drawing, flagged precisely so it
is one PR, not a rediscovery - same as Family 1's titlebar-button states
and Family 3's form-control geometry above.

# Buttons: Loop 2 update (wiring and repair, director redirect, #711 follow-on)

Owner: buttons family (loop 2). Section A of the director's redirect measured
the defect precisely: **103 `btn.*` keys per theme file, 0 consumers** -
verified again independently this loop by grepping
`kernel/gui/themes.c`'s `g_theme_fields[]` for any `btn.<variant>` entry
(none exist). Loop 2 is scoped as "wiring and repair", not more variants, so
this loop's actual deliverable is smaller and stricter than loop 1's:

1. **Close the one real gap in the already-wired generic matrix.**
   `state.btn_*` (11 keys: rest/hover/active/disabled/selected bg+fg, plus
   `focus_ring`) has been parsed into `theme_t` since before loop 1, but it
   never carried a border colour, so the nested-fill technique (loop 1's own
   spec) had nothing to draw with for the one variant that could actually be
   wired cheaply. Six new keys close this, added directly to the existing
   `state.btn_*` block in both `.mtheme` files:
   `state.btn_rest_border`, `state.btn_hover_border`,
   `state.btn_active_border`, `state.btn_disabled_border`,
   `state.btn_selected_border`, `state.btn_selected_focus_ring`. All six are
   literal copies of already-defined `color.*` tokens (`border_strong`,
   `border_subtle`, `accent_active`, `on_accent` respectively) - no new
   hexes. This is the "minimal btn/state set" the director's B11 asks for:
   fill, text, border, for rest/hover/pressed/disabled/focus, on the ONE
   generic button, not a second pass at six variants nobody wires yet.

2. **B9 bug found and fixed in the data:** `state.btn_focus_ring` sits at
   `color.focus_ring`. In `maytera_light.mtheme`, `color.focus_ring` is
   **bit-for-bit identical** to `color.accent` (`0x1856C0` both). The
   generic matrix's `selected` state fills with `color.accent`. A selected,
   focused button would render an invisible ring: same colour as its own
   fill, in the theme most people will actually ship. Dark theme has the
   same defect at reduced severity (`focus_ring`=`0x4C8EFF` vs
   `accent`=`0x2563C9` - close enough in hue/lightness to read as barely
   distinguishable, not clearly a second ring). Per B9's settled rule ("on
   accent-filled elements the ring is `color.on_accent`"), the new
   `state.btn_selected_focus_ring=color.on_accent` (white, both schemes)
   is the ring used when the button is both selected and focused; the
   plain `state.btn_focus_ring` continues to serve rest/hover/active/
   disabled, none of which are accent-filled, where the accent-hued ring
   already reads correctly.

3. **B6 ("No pills") applied, and a real conflict with the surfaces
   family's own loop-2 note resolved.** The surfaces family found my loop-1
   `radius.pill=14` + `btn.pill.*` block sitting uncommitted in the same
   file and correctly declined to touch it ("not resolved by me... the
   director/Family 2 need to reconcile this"). Resolution: `radius.pill=14`
   is deleted outright (no button in the OS currently renders pill-shaped;
   the whole variant matrix has zero consumers either way, so deleting the
   rung costs nothing live). `radius.btn_toolbar=3` (a second per-variant
   radius rung) is deleted for the same reason B6 gives for the pill: one
   shared control radius, not one per button shape. Both keys, plus every
   `btn.<variant>.*` key from loop 1 (the six-variant matrix in full), are
   preserved but moved into a single clearly marked `DEFERRED` block at the
   tail of each `.mtheme` file, per the director's B11 instruction to
   either delete or mark-and-defer unread keys rather than ship them as
   silent dead data again. **Count: 103 `btn.*` keys deferred per file (206
   total across both schemes), plus 2 keys deleted per file (`radius.pill`,
   `radius.btn_toolbar`; 4 total) - not deferred, retired, because B6
   explicitly kills what they represented.** `type.body_strong` and
   `radius.pill`'s *scale-rung* status (as opposed to this family's now-
   deleted button-local key) are untouched; only this family's own
   instantiations were in scope.
4. **B6's actual settled token, `radius.control=4`, does not exist as a
   parsed field anywhere** (checked `kernel/gui/themes.c`'s
   `g_theme_fields[]`: only `radius.btn/input/menu/card` are wired, no
   `radius.control`). I did not invent it as a new key in either `.mtheme`
   file, because an unwired key is exactly the 103-dead-keys mistake this
   loop exists to fix. The single already-wired button radius,
   `radius.btn`, is a general-scale token this family's own loop-1 comment
   correctly marks as **not mine to edit** ("radius.btn=6 ... NOT this
   designer's family - values untouched"), so I have not changed its value
   from 6 to 4 myself. **This is the one shared-token ask for the
   director/radius-scale owner**: either retarget `radius.btn`'s value to
   4 to match B6, or add a genuinely new `radius.control` field to
   `g_theme_fields[]`/`theme_t` and point buttons at it - either way, this
   is the last piece needed before the generic button (once drawn) matches
   B6's settled corner radius.

## Contrast re-verified this loop (WCAG AA, 4.5:1 floor)

No `color.*` values changed this loop (this family does not own hexes).
Re-confirming the pairs the new border/focus-ring keys depend on, since a
border or ring is only useful if it is visible against its neighbour:

| Pair | Dark | Light |
|---|---|---|
| `color.border_strong` vs `color.surface_raised` (rest/hover/active border vs button fill) | distinguishable, non-text UI element, not an AA-gated pair | distinguishable, non-text UI element |
| `color.on_accent` (new `selected_focus_ring`) vs `color.accent` (selected fill) | **5.7:1** (Family 4/6's own measured figure for this exact pair, B1) | **6.7:1** (same source) |
| `color.on_danger` on `color.danger` (unchanged from loop 1, re-checked live) | 4.58:1 | 5.07:1 |

`theme-scale-lint.sh` (self-test proven RED-on-broken/GREEN-on-good, per
the substrate report) runs clean this loop: `2 clean, 12 grandfathered -
RESULT: OK` - no new finding introduced by any of the six added keys or the
two deletions.

## Still open, flagged again rather than silently dropped

- **Live pixel verification did not happen this loop.** Per the same
  engineering-gap finding as loop 1 (below, unchanged): no draw call site
  reads `state.btn_*` (border or otherwise) to render an actual button
  today - confirmed again by grepping `kernel/gui/*.c` and
  `userland/libc/gui.c` for any of these field names, zero hits beside the
  parser table itself. I am not fabricating a screenshot of a change that
  cannot show up on a booted system yet; the acceptance test for this
  specific data (six new keys, two deletions) is "parses clean, no
  duplicate keys, lint passes, contrast pairs check out" - all four are
  verified above - not "looks different on screen", which requires the
  wiring in the next item.
- **Wiring ask for the substrate/engine phase (C2, not this family's to
  build):** add `state.btn_rest_border` / `hover_border` / `active_border`
  / `disabled_border` / `selected_border` / `selected_focus_ring` to
  `kernel/gui/themes.h`'s `theme_t` and `kernel/gui/themes.c`'s
  `g_theme_fields[]` (six `offsetof` entries, same pattern as the eleven
  already there - purely additive, no existing offset moves), then make
  `userland/libc/gui.c`'s `gui_draw_button()` actually read
  `theme->s_btn_*` (all seventeen fields, not just the eleven already
  parsed-but-unread) instead of whatever it currently draws with. This is
  the whole C2 gap for this family: one generic, fully six-state, bordered,
  correctly-focus-ringed button, nothing more, per B11's minimal-set
  instruction.
- **`radius.control=4` (B6)**: shared-token ask to the director/radius-
  scale owner, detailed in point 4 above.

### Update (same loop, after the radius-scale owner's fix)

Point 4 above is partly resolved: the radius-scale owner raised the
existing `radius.control` line in place from 3 to 4 (see `blame.md`,
"#711 loop 2: a shared-token name can collide") rather than appending a
second definition, so the VALUE now matches B6 in both `.mtheme` files.
**The wiring gap is unchanged**: `radius.control` is still not in
`kernel/gui/themes.c`'s `g_theme_fields[]` (re-checked, 0 hits), so it
is data with a correct value and no reader yet, same class of gap as the
new `state.btn_*_border` keys above. Once C2's wiring lands, the generic
button consumer should read `radius.control`, not `radius.btn` (which
stays at 6, its own separate legacy key, untouched by this family).


# Typography: Loop 2 update (wiring and repair, director redirect, #711 follow-on)

Owner: typography family (loop 2). Per Section A, this family had no P0 item
in the director's Section C list; the assigned work is D4 ("header treatment
per B5" and "reclaim the ~150px dead whitespace in the font-picker preview
box"), gated on P0 closure elsewhere, which this loop treats as satisfied
for typography specifically since no C-item names this family. Two real
userland C fixes this loop, both verified by exact-flag `gcc -fsyntax-only`
against the project's own Makefiles (see "Verification" below) - no
`.mtheme` key added or changed, no kernel/ABI touched, no build run.

## What loop 1 already found, re-confirmed before touching anything

Loop 1's own `## 4.3`/`## 4.6` sections (above) already flagged that
`type.*_weight` is stored in `theme_t` but read by zero draw call sites, and
that `type.body_strong` has no `theme_t` field at all. Re-grepped this loop
before writing any code: **`THEME_METRIC_TYPE_CAPTION`/`_TITLE`/`_HEADING`/
`_DISPLAY` had exactly ONE consumer in the whole tree** -
`userland/libc/gui_style.h`'s `GUI_TTF_SIZE` macro, which only reads
`THEME_METRIC_TYPE_BODY` (used by `gui.c`'s core button/checkbox text plus
five apps: gallery, imageviewer, appstore, snapshot, rss). Every other role
- caption, title, heading, display - had **zero** consumers anywhere outside
the parser's own offset table, the same "163 dead keys" failure mode B11
targets, just in the `type.*` namespace instead of `btn.*`/`state.*`.

## Fix 1 (B5): Settings section/subsection headers

Found the literal defect Section B5 names ("green section headers... the
defect is that accent selection propagates to nothing except section
headers"): `userland/apps/settings/main.c`'s `draw_subsection()` - **44 call
sites, one per subsection heading across every Settings panel** - rendered
its label in `COL_ACCENT`. Its sibling `draw_section_header()` - **18 call
sites, the top-level category heading per panel** - already used
`COL_TEXT_PRIMARY` (correct colour) but both functions drew through
`win_draw_text()` (`SYS_WIN_DRAW_TEXT`), the legacy fixed-size 8x16 bitmap
console font (`kernel/video/font.h`'s `FONT_WIDTH=8`/`FONT_HEIGHT=16`), not
the antialiased TTF path this family's whole type scale describes. A bitmap
font has no size axis and no weight axis, so "bold" was structurally
impossible at that call site before this fix, independent of the colour bug.

Fixed both functions (2 functions, 62 on-screen label instances covered,
mechanism-not-instance): now call `win_draw_text_ttf_ex()` with an explicit
`FONT_STYLE_BOLD` bit (real DejaVu Sans Bold face, index 0, not faux-bold -
`/FONTS/DEJAVUB.TTF` per 4.7 above) and `COL_TEXT_PRIMARY` for both
(`draw_subsection`'s colour bug is the fix; `draw_section_header`'s colour
was already right). Sizes are read live via `theme_metric_or()`:
`THEME_METRIC_TYPE_TITLE` (16, fallback matches the pre-existing literal)
for the section header, `THEME_METRIC_TYPE_BODY` (14) for the subsection -
this is the intentional `type.body_strong` semantics (14px box, bold
weight) applied via an explicit style bit at the call site rather than a
`type.body_strong` metric read, specifically because that metric has no
`theme_t` field yet (see "Not fixed" below) - this sidesteps the ABI gap
entirely rather than depending on it.

One fortunate arithmetic match, not engineered: `type.title`'s own line
height (`round(16*1.4)=22`) is bit-for-bit the pre-existing `y + 22`
separator offset already in `draw_section_header()`, so no layout constant
needed to move to accommodate the new TTF metrics.

## Fix 2 (D4): font-picker preview whitespace

`userland/libc/gui_font.c`'s `draw_preview()` (the shared `gui_font_dialog()`
common dialog - 3 confirmed callers: `editor/main.c`'s Font menu, `paint/
ui.c`, and Settings' own system-UI-font picker) drew its big sample pinned
to a fixed top offset (`PRE_Y + 14`) at a flat `min(size, 44)` cap regardless
of how much of the 136px-tall preview box that size actually needed. At the
dialog's own default selection (14pt, `g_sizes[5]`), the glyph occupied only
its own ~14-18px near the top-left corner, leaving roughly 100px of the box
empty above the caption line - the "~150px dead whitespace" the director's
report measured (136px box, ~18px used, ~18px caption row = ~100px void;
close to their eyeballed figure, exact accounting differs by rounding).

Fixed as pure typography, not a box resize (`PRE_Y`/`PRE_H`/`M` all
unchanged - that geometry belongs to the modal/dialog family): the sample
now scales up to fill the real available band (top inset to the caption
row) using line-height math (`round(sz*1.4) <= avail_h`), **never past the
user's own selected size** (so a 14pt selection is never shown larger than
14pt - filling space is not the same as lying about the selection), then is
additionally bounded by render width (`gui_ttf_render_width()`) so a long
preview string at a large fit size cannot spill past the box's right edge,
then is **vertically centered** within the available band by its line
height. This is 4.5's own already-written rule applied to a real box:
"Vertical centering inside a fixed-height control... uses the role's line
height, not its raw pixel size, so ascenders/descenders sit optically
centered". The arbitrary `> 44 -> 44` cap is gone, replaced by the box's
actual arithmetic.

## Verification

No build was run (only the Substrate phase may build, per house rules; this
family is not Substrate). Both files were checked with `gcc -fsyntax-only`
using the **exact CFLAGS from their own Makefiles**
(`userland/apps/settings/Makefile`'s freestanding+PIE flags for `main.c`;
`userland/libc/Makefile`'s freestanding+large-model flags for `gui_font.c`),
run on the build container (the actual build host, gcc 12.2.0, matching what
`build-golden.sh` uses): **both exit 0, zero errors.** The only warnings
present are pre-existing ones unrelated to either changed function (macro
redefinitions in `theme.h`/`gui.h`, a misleading-indentation note in
`textfield.h`, unused-variable notes elsewhere in `main.c`) - confirmed by
grep that neither `draw_section_header`/`draw_subsection`/`draw_preview`
nor any of the new local variables (`sz`, `band_top`, `band_bottom`,
`avail_h`, `fit_sz`, `avail_w`, `line_h`, `text_y`) appear in the warning
output. This is a syntax/type check only, not proof of correct on-screen
pixels - no screenshot exists for this loop's typography changes. Flagging
that honestly rather than fabricating one: a Settings-panel/font-dialog
change needs a booted throwaway VM to actually verify, which needs a build,
which is Substrate's slot this loop, not this family's.

## Contrast

No new colour pair introduced. Both fixes now use `COL_TEXT_PRIMARY` (the
same `color.on_surface`-equivalent Settings already resolves elsewhere),
already measured in `## 4.8` above at **15.6:1 (Maytera Dark)** and **16.4:1
(Maytera Light)** against `color.surface` - both far past the 4.5:1 AA
floor. The defect fixed here was never a contrast number; `COL_ACCENT`
itself passes AA against the Settings background in isolation (it is a
legitimate colour, just used in the wrong semantic role per B5). The fix is
a semantic-token correction, not a contrast correction, and is reported as
such rather than padding the report with a ratio that was never the
problem.

## Not fixed, flagged again rather than re-discovered

`type.body_strong` still has no `theme_t` field / `g_theme_fields[]` entry
(unchanged since loop 1's `## 4.6`; re-confirmed this loop, still 0 grep
hits outside the definition-table comment in the buttons family's own
section above, who independently hit the identical gap for `btn.primary`'s
label). **Two families now depend on this same missing three-field addition
for two different, unrelated reasons** (a primary button's emphasized
label, and - had I chosen to read the metric instead of sidestepping it - a
theme-configurable subsection weight/size decoupled from `type.body`).
Elevating this from a single flagged note to a cross-family ask: this is a
mechanical, additive, three-field change (`TM_TYPE_BODY_STRONG[/_LH/_W]`,
same shape as the fifteen fields already there), blocking real consumers in
two families now, not a hypothetical.

`type.*_weight` remains otherwise unread by any OTHER call site in the
tree beyond this loop's two new ones - the two fixes above are additive
consumers, not a general-purpose "wire every type role everywhere" pass,
which was out of scope for a single loop against a 6700-line Settings file
alone.

## D2/D3: not applicable to this family

D2 (apply B6 radius / B7 spacing to owned surfaces) and D3 (render B9 focus
rings on owned controls): typography owns no control geometry, elevation
surface, or interactive widget - the brief explicitly prohibits this family
from specifying control geometry. Confirmed by re-reading the brief's own
boundary before writing this section rather than assumed. Nothing to report
under either item.

## What I did not touch

No `.mtheme` file, no `color.*`/`state.*`/`radius.*`/`metric.*` key, no
kernel C, no other family's app code. `kernel/gui/settings/panel_*.c` (the
dead "engine A" fallback Settings, confirmed inert unless `/APPS/COMPOSIT`
fails to launch, per the codebase's own established pattern for this class
of dead code) was not touched - the shipping Settings app is
`userland/apps/settings/main.c`, which is what was fixed.

# Family 3 (Form Controls), Loop 2: wiring and repair (#711)

Data-only change plus a code-level engineering spec; no C source touched
this loop (only the substrate/build phase may build, per house rules).

## What Loop 1 got right, and the one assumption Loop 2 found wrong

Loop 1 authored a complete six-state token model (26 `state.*` keys across
checkbox/radio/dropdown/slider/field/scrollbar) on the assumption that the
wiring target would be "one authored hex per state, per control" - the same
shape as the button family's `state.btn_*` set. Loop 2 read the actual
shipping consumer code before writing anything new, and found the widget
engine does not work that way for these controls. That finding, not more
token bookkeeping, is this loop's main output.

## The three parallel checkbox implementations (new finding)

1. **`kernel/gui/widget.c` `widget_draw_checkbox`**: reads
   `THEME_CHECKBOX_BG`/`BORDER`/`CHECK` (3 legacy keys only), `box_size`
   hardcoded `14`, checked/unchecked only - no hover/focus/disabled colour
   distinction at all. Checkmark is two raw `fb_put_pixel` loops (a 2px-ish
   diagonal tick), hard-edge, no AA - compliant with renderer limits, just
   undocumented anywhere as a spec.
2. **`userland/libc/gui.c` `gui_draw_checkbox`** (the legacy "Simple Widget
   Drawing Helpers" path): **100% hardcoded hex** - `0x00FFFFFF` box,
   `0x00404040` border, an "X" not a check - zero theme reads of any kind,
   `box_size=16` hardcoded. No state handling whatsoever.
3. **`userland/libc/gui.c` `gui_checkbox`** (the modern `gui_style.h`
   primitive, used by 6 shipping apps: files/help/mediaplayer/rss/settings/
   svcmgr, confirmed via `grep -rln "gui_checkbox("`): reads a `gui_palette_t`
   (10 fields - `surface, surface_raised, ink, ink_dim, accent, accent_hover,
   border, field_bg, field_border, track`), derives hover/pressed
   **procedurally** at draw time (`gui_lighten(base,18)`, `gui_darken(base,18)`,
   `gui_mix(x, p->surface, 130)` for disabled), and draws a real check glyph
   via two `gs_line()` calls (a plain integer Bresenham line, doubled for a
   ~2px stroke - hard-edge, renderer-limit-compliant, and already better than
   Loop 1's proposed 11-rect polyline spec, which is now superseded, not
   needed). Box radius is hardcoded `4` for the modern style family - this
   **already matches B6's settled `radius.control=4`**, coincidentally, not
   because it reads any token.

None of the three reads a single one of Loop 1's 38 form-control-specific
keys, and #2/#3 do not even read the generic `state.input_*` keys that ARE
already parsed into the kernel's `theme_t` struct. This is a deeper gap than
"the offset table has no row for this key" (Loop 1's framing): two of the
three consumer code paths do not read from the theme system's colour table
*at all* for this control, by construction, not by omission.

## Radio, dropdown, scrollbar: no shared primitive exists at all

`grep -rl` across `userland/apps/*/main.c`: 2 apps hand-roll a dropdown
inline, 1 app hand-rolls a radio button inline, 0 apps call the one
hardcoded scrollbar helper that exists (`gui_draw_scrollbar_v`) - shipping
scrollbars are drawn some other way entirely, outside this family's current
visibility. `kernel/gui/widget.h`'s `WIDGET_*` enum has only
`LABEL/BUTTON/CHECKBOX/TEXTBOX`; `kernel/gui/theme.c`'s
`theme_draw_radio_button` is dead "engine A" code with zero live callers
(same class as `theme_save_config()`, retired at #711 substrate). Every app
that needs a radio, dropdown, or scrollbar today writes its own, per the
house rule this violates directly ("reuse the existing shared primitives;
never reinvent").

## Renderer-capability correction (good news, changes the geometry spec)

Loop 1's brief assumed circles must be hard-edge Bresenham only ("do not
assume [an AA line] exists... circles are hard-edge Bresenham, use them at
14px+ or not at all"). The shipping engine already has `gui_fill_circle_aa`
(declared `gui_style.h`, implemented `gui.c`, already used by `gui_toggle`'s
knob and `gui_slider`'s thumb): an antialiased circle fill that blends the
edge toward a **caller-supplied background colour** - the exact same
no-framebuffer-readback technique `gui_fill_rounded_aa` uses for rounded
rects, and explicitly allowed by the renderer-limits section for that
reason. Radio buttons should use the **same, already-proven, already-shipping**
primitive (nested: outer `gui_fill_circle_aa` in ring colour, inner
`gui_fill_circle_aa` inset by the ring width in box-bg colour, a third
smaller `gui_fill_circle_aa` for the dot when checked) instead of Loop 1's
proposed hard-edge-Bresenham-plus-nested-fill-rect technique. Simpler spec,
better visual quality, zero new renderer capability requested.

## Architecture mismatch: procedural state derivation vs. explicit per-state tokens

`gui_checkbox`/`gui_toggle`/`gui_slider`/`gui_textfield2` derive hover/
pressed/disabled colours **procedurally** at draw time (`gui_lighten`,
`gui_darken`, `gui_mix` against a ~10-field palette), not from distinct
authored hex per state. Loop 1's design (26 explicit `state.*` hex tokens)
assumed the opposite model - the button family's model, which the button
matrix also has not gotten wired yet (see the buttons family's own report:
"0 consumers" for the full `btn.*` matrix too, before this loop's minimal
fix). Recommendation: **reconcile toward the shipping architecture rather
than rewrite it**. Extend `gui_palette_t` with a small number of new BASE
fields the procedural derivation cannot currently produce (below), instead
of wiring 26 granular hex tokens 1:1. This directly serves the redirect's
B11 "token surface diet" instruction - fewer new struct fields, same
visible states, reusing the pattern already proven for buttons/toggles.

## Minimal wiring spec (handed to whichever pass has build access)

**`gui_palette_t` additions** (4 new fields, extends the existing 10 in
`userland/libc/gui_style.h`):

```c
typedef struct {
    // ... existing 10 fields unchanged (surface, surface_raised, ink,
    // ink_dim, accent, accent_hover, border, field_bg, field_border, track)
    uint32_t danger;        // invalid-field border / destructive accents
    uint32_t placeholder;   // muted input placeholder text
    uint32_t focus_ring;    // explicit ring colour (today callers reuse
                             // p->accent - fine for accent-filled controls
                             // per B9 - a distinct field lets a future
                             // theme's ring diverge from its accent hue)
    uint32_t on_accent;     // check-glyph / selected-row ink on an accent
                             // fill (today gui_ink_on(p->accent) computes a
                             // procedural black/white pick - fine, but B1/B2
                             // name color.on_accent as an AUTHORED token)
} gui_palette_t;
```

**Single shared population helper** (this is the highest-leverage fix in
this whole spec): `gui_palette_from_theme(gui_palette_t *out)` in
`userland/libc/gui_theme.c`, filling all 14 fields from `theme_color()` /
`SYS_THEME_COLOR`, called by every app instead of each app hand-rolling its
own lookup. Confirmed this loop: `userland/apps/settings/main.c` maintains a
**private, hardcoded 12-theme colour table** (`g_th[theme_id]`,
`ACCENT_COLORS[]`) and fills `gui_palette_t` from THAT, never from
`theme_color()`. `userland/apps/files/main.c` does the same via its own
`fp_field()`/`fp_acc()`/`fp_tint()` helpers. This is structurally *why* B1's
"every hard-coded blue must be replaced with a `color.accent` read" is hard:
editing a `.mtheme` file cannot reach either app's controls no matter how
correct the data is, because the C code never asks the kernel for the
value. One shared helper, adopted by both apps, fixes this class of bug
everywhere at once instead of per-app.

**New `theme_metric_id_t` entries** (`kernel/gui/themes.h` `TM_*` enum +
`kernel/gui/themes.c` `g_theme_int_fields[]`, append-only, exact pattern
#711 already established):

```
TM_RADIUS_CONTROL      -> "radius.control"
TM_CHECKBOX_BOX        -> "metric.checkbox_box"
TM_RADIO_D             -> "metric.radio_d"
TM_RADIO_DOT           -> "metric.radio_dot"
TM_RADIO_RING_W        -> "metric.radio_ring_w"
TM_DROPDOWN_ROW_H      -> "metric.dropdown_row_h"
TM_SLIDER_TRACK_H      -> "metric.slider_track_h"
TM_SLIDER_THUMB_W      -> "metric.slider_thumb_w"
TM_SLIDER_THUMB_H      -> "metric.slider_thumb_h"
TM_SCROLLBAR_THUMB_MIN -> "metric.scrollbar_thumb_min"
```

10 entries (9 metrics + `radius.control`, which the buttons family also
needs - see the shared-token ask below). `radius.slider_thumb` and
`radius.scrollbar_thumb` are deliberately **not** requested: both thumbs are
already circular in the shipping code (`gui_fill_circle_aa`), so a
rectangular-corner radius token does not apply to either; those two `.mtheme`
keys should move to the DEFERRED marking, not get wired.

**New `gui_style.h` primitive signatures** (built entirely from EXISTING
primitives - `gui_fill_circle_aa`, `gui_fill_rounded_aa`, `gs_line`,
`win_draw_rect` - no new renderer capability requested):

```c
void gui_radio(int handle, int x, int y, int d, bool selected,
               const char *label, gui_state_t st);
void gui_dropdown_closed(int handle, int x, int y, int w, int h,
                         const char *text, bool open, gui_state_t st);
int  gui_dropdown_row_h(void);  // theme_metric_or(THEME_METRIC_DROPDOWN_ROW_H, 32)
void gui_scrollbar_v2(int handle, int x, int y, int w, int h,
                      int thumb_pos, int thumb_len, gui_state_t st);
```

`gui_radio` pixel spec: `d = theme_metric_or(THEME_METRIC_RADIO_D, 14)`. The
14px floor from Loop 1's brief stays the minimum even though the fill is now
AA - it is about the RING STROKE staying legible at a 2px inset, not the
now-obsolete hard-edge-only constraint. Ring:
`gui_fill_circle_aa(h, x, y, d, ring_color, p->surface)`, then an inset
circle `gui_fill_circle_aa(h, x+rw, y+rw, d-2*rw, p->field_bg, ring_color)`
(`rw=2` = `THEME_METRIC_RADIO_RING_W`). Dot when selected:
`gui_fill_circle_aa(h, x+(d-dot)/2, y+(d-dot)/2, dot, p->accent, p->field_bg)`
(`dot=6` = `THEME_METRIC_RADIO_DOT`). Ring colour per state: rest = the
fixed `state.input_rest_border`-equivalent value below, hover = the hover
border equivalent, active/selected = `p->accent`, focus adds the standard
2px EXTERNAL ring (`GUI_FOCUS_W`, same primitive every other control uses
per B9), disabled = the muted/disabled ink equivalent.

## Fixed this loop: `state.input_rest_border` (real defect, found and corrected)

`state.input_rest_border` (and its literal copy `state.radio_ring_rest`)
previously equalled `color.border_strong` (`0x454551` dark / `0xC3C8D0`
light), which measures only **1.98:1 dark / 1.68:1 light** against the
sunken box background these four controls (text field, checkbox,
dropdown-closed, radio ring) all rest on - under the 3:1 non-text-UI floor
(WCAG 1.4.11). Repointed at `color.on_surface_muted` instead (an existing
token in the same file, no invented hex): **7.32:1 dark / 4.97:1 light**
against `color.surface_sunken`. This value chases the colour-scheme
family's B3 token; it was re-synced once already this loop after Family 4's
B3 edit changed `color.on_surface_muted` mid-session (`0x9BA1AA` ->
`0x9AA3AF` dark, `0x5C636D` -> `0x5A6472` light) - see the blame.md entry
below, the exact stale-copy race the 5cc2512 follow-up commit already
documented once, hit again in the same loop.

Flagged, not fixed (shared token, not mine to invent a new hex for):
dark's active-state border (`state.input_active_border` = `color.accent_active`
= `0x1B4FA6`) measures only **2.42:1** against `color.surface_sunken` -
weaker than the just-fixed rest state. Its 2px external focus ring
(5.89-6.71:1, comfortably clearing 3:1) is the PRIMARY indicator for
keyboard focus, so this is not a blocking defect, but the active/pressed
border is currently the LEAST visible state in the six-state progression,
which reads backward (emphasis should increase, not decrease, on press).
Flagged to the colour-scheme family since `color.accent_active` is shared
with the button matrix's pressed state.

## Contrast, re-measured against the fixed values (WCAG relative-luminance formula)

| Pair | maytera_dark | maytera_light | Floor | Pass |
|---|---|---|---|---|
| `state.input_rest_fg` on `state.input_rest_bg` (field body text) | 16.83:1 | 17.77:1 | 4.5:1 text | Pass |
| `state.field_placeholder_fg` (`color.on_surface_muted`) on `state.input_rest_bg` | 7.32:1 | 4.97:1 | 4.5:1 text | Pass |
| `state.input_rest_border` / `state.radio_ring_rest` (fixed) on `color.surface_sunken` | 7.32:1 | 4.97:1 | 3:1 UI | Pass (was 1.98 / 1.68, fail) |
| `color.focus_ring` on `state.input_rest_bg` | 5.89:1 | 6.71:1 | 3:1 UI | Pass |
| `state.field_invalid_border` (`color.danger`) on `color.surface_sunken` | 4.09:1 | 4.19:1 | 3:1 UI | Pass |
| `state.input_selected_fg` on `state.input_selected_bg` (check glyph / selected row) | 5.66:1 | 6.70:1 | 4.5:1 text | Pass |
| `state.input_active_border` (`color.accent_active`) on `color.surface_sunken` (flagged, not fixed) | 2.42:1 | 7.41:1 | 3:1 UI | **Fail dark** |

Every pass computed independently with the same relative-luminance formula
the lint uses, against the values actually shipped in both files after this
loop's edits, not copied from another family's report.

## What I did not do

No C code touched (only the substrate/build phase may build this loop, per
house rules) - the wiring spec above is precise enough to implement without
further discovery, per the "specify and review" division of labour. Did not
touch `radius.btn` (family 2, buttons) or `radius.menu` (presumed family 6,
popups) even though B6 names "buttons... menus" alongside "inputs,
dropdowns" under one settled 4px value - those two keys belong to other
families; see the shared-token ask below rather than my editing them
myself. Did not touch `gui_palette_t`, `gui_style.h`, `kernel/gui/themes.{c,h}`,
or any app's C source. Did not delete any of Loop 1's 38 dead keys (kept and
honestly labelled per B11 - see the `.mtheme` STATUS banner added this loop)
since the geometry values and the now-fixed contrast pair are correct,
reusable targets, and re-deriving them later would waste work already
checked here. Did not add `state.danger`/`state.placeholder`/`state.focus_ring`/
`state.on_accent` colour KEYS to the `.mtheme` files for the proposed
`gui_palette_t` fields above - they already exist under other names
(`color.danger`, `state.field_placeholder_fg`, `color.focus_ring`,
`color.on_accent`), so the population helper reads existing tokens; no new
`.mtheme` keys are needed for this part of the spec, only new C struct
fields and one new reader function.

## Shared-token asks for the director

1. **Radius consolidation**: `radius.control` (mine) `=4`, `radius.btn`
   (family 2) `=6`, `radius.menu` (presumed family 6) `=6`, `radius.input`
   (unowned substrate default) `=6` - four keys where B6 names ONE settled
   value (4px) for "buttons, inputs, dropdowns, menus, selection-row
   highlights" as a single sentence. Someone needs to either consolidate
   these to one key (`radius.control`, the name B6 itself uses) or
   explicitly confirm all four keys should independently carry `4` as
   parallel, separately-maintained data. I did not rename or delete
   `radius.btn`/`radius.menu`/`radius.input` myself since they are not my
   family's keys.
2. **`color.accent_active` weak contrast** (2.42:1 dark against
   `color.surface_sunken`) - shared with the button family's pressed state,
   not mine to change; flagged above with the measurement.

## Renderer limits this family did not use

No drop shadow on any control state; no gradient anywhere in this family's
controls (all fills are the single flat colour a state resolves to); no
per-pixel alpha for any hover/press feedback (`gui_lighten`/`gui_darken`/
`gui_mix` in the procedural-derivation path all produce fully opaque,
precomputed solid colours - not a live alpha blend against the real
framebuffer, so this stays inside the "no per-pixel alpha layers" limit);
the only antialiasing anywhere in this family's spec is `gui_fill_circle_aa`/
`gui_fill_rounded_aa`, both already-shipping primitives that blend toward a
caller-supplied constant, never a framebuffer read-back; no stroked rounded
rect (the checkbox/field/dropdown box border uses the nested-fill technique,
same as buttons); no antialiased text, no font weight beyond regular/
faux-bold, no animation or transition.

## Terminal (Family 6, Loop 3): banner/prompt palette

Scope this loop, per director direction: "own the light-scheme text
palette values (section 2 item 4), verify every default output color at
4.5:1 or better against BOTH schemes' backgrounds, sampled not eyeballed."

### What the actual defect was

`userland/apps/terminal/main.c` (the shipping GUI terminal - `kernel/gui/
terminal.c` and `kernel/gui/sshterm.c` are compositor-absent kernel-side
fallbacks with zero `.mtheme` reading of any kind, see "Out of scope"
below) already routes its *default* foreground/background through
`theme_color(THEME_COLOR_TEXTBOX_BG/TEXT/CURSOR)` (main.c:185-186, 231,
244) - that part of the theme-blindness fix already happened before this
loop. The general 16-slot ANSI colour table (`ansi_colors[]`) is
deliberately left semantic per the in-source comment at main.c:180-183;
that is correct and this family did not touch it.

What was still theme-blind: two places in the SAME file pick a raw ANSI
escape code for UI-authored chrome text, not user/program output -
the welcome banner (`"\033[1;36m" "MayteraOS Terminal v1.0\n"`,
main.c:1353-1355) and the prompt (`print_prompt()`, main.c:534-538, two
segments: `\033[32m` for `user@maytera`, `\033[34m` for the cwd). Because
these are raw ANSI values picked once for a dark background, switching to
Maytera Light (background flips to `0xFFFFFF`) makes two of the three
segments fail WCAG AA badly.

### Contrast measured (WCAG relative-luminance formula, sampled against
the terminal's actual `textbox_bg` value in each theme, not eyeballed)

| segment | value | dark (0x121216) | light (0xFFFFFF) |
|---|---|---|---|
| banner (raw `\033[1;36m` = bright cyan `0x55FFFF`) | old | 15.25:1 PASS | 1.23:1 **FAIL** |
| prompt user (raw `\033[32m` = green `0x00AA00`) | old | 6.01:1 PASS | 3.11:1 **FAIL** |
| prompt path (raw `\033[34m` = blue `0x0000AA`) | old | **1.41:1 FAIL** | 13.29:1 PASS |

The third row is a real defect the director's brief did not name (it gave
two example values, banner and prompt-user, not prompt-path): the cwd
segment is functionally invisible dark-blue-on-near-black in the DARK
theme specifically, the opposite scheme from where the other two segments
break. All three needed a fix, not two.

### New tokens (data only this loop, see Wiring below)

Three new namespaced keys, `color.term_banner` / `color.term_prompt_user`
/ `color.term_prompt_path`, added to `maytera_dark.mtheme`,
`maytera_light.mtheme`, and `retro_unix.mtheme` (retro_unix has the same
white `textbox_bg` as maytera_light and the same source file drives it,
so it has the identical failure even though the director's brief named
only the two modern themes; fixed for consistency, values carried over
unchanged since they were already computed against white).

| theme | term_banner | term_prompt_user | term_prompt_path |
|---|---|---|---|
| maytera_dark | `0x55FFFF` (unchanged, 15.25:1) | `0x00AA00` (unchanged, 6.01:1) | `color.link_color` = `0x5AA9FF` (was raw `0x0000AA` at 1.41:1, now 7.61:1) |
| maytera_light | `0x0E7490` (director spec, 5.36:1) | `0x15803D` (director spec, 5.02:1) | `color.link_color` = `0x1856C0` (was already fine at 13.29:1, repointed to the semantic token anyway, 6.71:1) |
| retro_unix | `0x0E7490` (same as light, 5.36:1) | `0x15803D` (same as light, 5.02:1) | this file's own `link_color` = `0x000080` navy (was already fine at 13.29:1, repointed, 16.01:1) |

No invented hex for the path segment in any file: all three point at an
already-AA-checked existing link/navy token instead of a fourth colour.

### Wiring needed (not done this loop, flagged as scoped engineering)

Zero grep hits today for a reader: `kernel/gui/themes.h`'s `TM_*` enum and
`kernel/gui/themes.c`'s `g_theme_fields` table have no rows for these
three keys, and `userland/libc/theme.h`'s `THEME_COLOR_*` enum has no
`TERM_BANNER`/`TERM_PROMPT_USER`/`TERM_PROMPT_PATH` slots for
`SYS_THEME_COLOR` to serve. Minimal spec: add the three enum members plus
offset-table rows (same mechanism as the loop-2 titlebar-button tokens),
then in `userland/apps/terminal/main.c` replace the two hardcoded escape
writes with direct `theme_color(THEME_COLOR_TERM_*)` cell-attribute
writes instead of routing through `ansi_colors[]`. The trailing
`"\033[0m"` resets are unaffected (a reset has no colour to theme).

### Out of scope, flagged rather than silently skipped

- `kernel/gui/terminal.c` and `kernel/gui/sshterm.c`: compositor-absent
  kernel-side terminal UIs (the former a fallback if `/APPS/COMPOSIT`
  fails to launch; the latter the actual SSH client display, since
  `userland/apps/ssh/main.c` is a 107-line launcher with no rendering of
  its own). Neither reads `.mtheme` at all today - `sshterm.c` has its
  own fully-hardcoded `ST_BG`/`st_pal[16]`/cursor constants, always dark,
  with zero theme integration of any kind, a strictly bigger gap than
  banner/prompt in the userland terminal. Adding data these files cannot
  read yet would repeat the "0 grep hits for a reader = 0 keys added"
  mistake this family's own loop-2 report already named; flagged to the
  director as a real, separate finding rather than attempted this loop.
- The general `ansi_colors[16]`/`st_pal[16]` tables: confirmed correct to
  leave alone, they are the semantic ANSI palette a program's own
  `\033[3Xm` output is entitled to expect unchanged; only chrome text
  masquerading as ANSI output was in scope.
- The other 9 legacy (`lint.baseline=legacy-v1`) themes: not touched.
  Same defect likely exists wherever `textbox_bg` is light and the
  terminal is opened, but auditing all 9 is outside "own the light-scheme
  text palette values" as scoped to this loop; the three-key shape is
  established if a future pass wants to extend it.

### Renderer limits this family did not use

No shadow, no gradient, no per-pixel alpha, no antialiasing, no new
primitive: this was a same-format `color.*` key addition (nine total
lines of data across three files) plus a contrast measurement. Nothing
here needed anything the renderer cannot already do.


# Family 3 (Form Controls), Loop 3 (#711): director shared tokens + 14/14 data parity

Data-only change, no C touched (only Substrate may build). Loop 2's wiring
spec (gui_palette_t 4-field extension, gui_palette_from_theme() population
helper, 10 new TM_* metric entries, gui_radio/gui_dropdown_closed/
gui_scrollbar_v2 signatures - all above, "Minimal wiring spec") is still
correct and still unimplemented; re-confirmed this loop by re-reading the
same three checkbox call sites plus the radio/dropdown/scrollbar hand-rolls
found last loop, none of which changed. It is not part of the director's
Loop 3 Substrate priority queue (items 1-9 in the redirect cover elevation,
the start-menu search field, dialog clipping, terminal palette, the desktop
widget, live retheme, a buffer-size bug, and stale menu selection - none of
them is "wire form controls"). Flagging again, explicitly, since it remains
the single largest defect in this family: every token below, in every
theme, still has zero consumers, so nothing in this loop's work is
verifiable by screenshot on a running system - only by re-deriving the
contrast math against the literal values actually shipped, which is what
follows.

## What changed this loop

1. **`color.placeholder`** (director loop-3 shared decision, section 1): a
   new, dedicated token for text-field placeholder ink, distinct from
   `color.on_surface_muted` even though the values are close in both
   maytera themes (dark `0x9AA0AA` vs `0x9AA3AF`; light `0x6A6F78` vs
   `0x5A6472`). `state.field_placeholder_fg` now points at the new token
   instead of aliasing `on_surface_muted`. Measured (WCAG relative
   luminance, same formula the lint uses): **7.10:1 dark, 5.05:1 light**
   against `state.input_rest_bg` - both exceed the director's own "~7:1" /
   "~5:1" estimates by rounding, both clear 4.5:1 text AA though
   placeholder text is exempt from the requirement.
2. **`radius.selection` / `metric.selection_inset`**: new keys for the
   converged shared selected-row treatment (director section 1: accent
   fill, white text, radius 4, 4px inset) as it applies to THIS family's
   dropdown popup selected row. Numerically identical to the already-wired
   `radius.control` (4) and `space.xs` (4) respectively - given their own
   names because the director's spec names the concept separately from
   control-box rounding, even though the scale currently makes them
   coincide. The dropdown popup's selected row itself needed no colour
   change: `state.item_selected_bg`/`_fg` already equal `color.accent`/
   `color.on_accent`, matching the director's mandated treatment exactly,
   confirmed by reading both files rather than assumed.
3. **14/14 theme parity** (was 2/14): the 38 checkbox/radio/dropdown/
   slider/scrollbar/field extension keys from Loop 1/2 existed only in
   `maytera_dark.mtheme`/`maytera_light.mtheme`. Propagated to all 12
   legacy themes (`retro_unix`, `classic`, `dark`, `light`, `fluent_dark`,
   `fluent_light`, `forest`, `high_contrast`, `modern_dark`, `modern_light`,
   `ocean`, `sunset`), same literal-copy-of-an-existing-token discipline
   (every value traces to a "# = tokenname" comment, nothing invented).
   `radius.control`/`radius.selection`/`radius.slider_thumb`/
   `radius.scrollbar_thumb` derive from each theme's own `radius.btn`: `0`
   on the three flat/hard-edge themes (`retro_unix`, `classic`,
   `high_contrast`), matching their existing all-square-corners look, else
   the modern-default `4`/`4`/`3`/`8`. Where a legacy theme's
   `color.accent`/`accent_hover`/`accent_active` already coincide (a
   pre-#711 flat-palette limitation, grandfathered, not introduced here),
   `state.slider_thumb_hover` reuses plain `accent` rather than inventing a
   distinct hover tint the theme has no token for - documented inline per
   theme, not silently done. `--self-test` on `theme-scale-lint.sh` still
   passes (RED on broken, GREEN on good) and the full run is unchanged at
   2 clean / 12 grandfathered after the propagation, confirming the new
   keys did not trip any existing scale/contrast check (`radius.control` is
   outside the lint's checked radius set - `btn input menu card` only - so
   this is a currently-unchecked key, noted for whoever next touches the
   lint's key list, not fixed here since the lint is not this family's file
   to edit unprompted).

## A generation bug I caught and fixed before committing (worth recording as
## its own pitfall, added to blame.md): Python `str.format`'s `{x:>08}`
## zero-pads SHORT strings, silently corrupting an already-correct hex value

The first pass of the 12-theme propagation script built `state.
field_invalid_border` as the literal `"0x00" + "{danger:>08}"` where
`danger` was a bare 6-character hex string (e.g. `"CC0000"`, no `0x00`
prefix). `:>08` right-aligns a STRING to width 8 using `0` as the fill
character - since `"CC0000"` is only 6 characters, format padded it to
`"00CC0000"` before the manual `"0x00"` prefix was even added, producing
`0x0000CC0000` (10 hex digits, a corrupted colour with a phantom leading
`00` byte) in **every one of the 12 propagated files**, while every other
substitution in the same template used a helper that always returned a
10-character `"0x00RRGGBB"` string first (>= the width 8 target, so the pad
was a silent no-op there) - which is exactly why only ONE line was wrong
and it was not obvious from spot-checking a few of the other 30-plus lines
per file that looked fine. Caught by a blanket `grep -n '=0x0000'` sweep
across every theme file (not just the ones I had just written) BEFORE
staging, which also turned up ~130 legitimate pre-existing `0x0000xx`
values (colours that are genuinely dark/zero in one or two channels, e.g.
`0x000078D4`) that had to be read individually to confirm none were new
corruption - a plain `grep -c` count would not have distinguished a
one-line true bug from noise. Fixed with a second, narrowly-scoped regex
substitution (`0x0000([0-9A-Fa-f]{{6}})` -> `0x00\1`) applied only to the
exact key each theme had it on, verified by re-grepping the specific key
across all 14 files afterward, matching the value to that theme's own
`color.danger`. Lesson for the next script-generated `.mtheme` edit: never
format a raw (unprefixed) hex-digit string with a width specifier that is
smaller than or close to the string's own length - either always wrap
raw hex in a helper that returns the FULL `"0x00RRGGBB"` string before any
width formatting is applied, or drop the width spec entirely, since the
literal prefix concatenation already guarantees correct length.

## Still outstanding, not fixed this loop (shared tokens, not mine to hex)

- **`color.accent_active` fails the 3:1 UI floor in the dark theme**
  against `color.surface_sunken`: re-measured this loop at **2.42:1**
  (identical to Loop 2's figure - the value has not changed:
  `0x1B4FA6` on `0x121216`), confirming this remains open. It is the
  active/pressed border for the text field, checkbox, dropdown-closed box,
  and (via literal copy) the radio ring's active AND selected states in
  dark theme - so once wired, the radio button's "just selected" moment
  would be the least-visible frame in its own six-state progression, in
  dark theme only (light theme's same pair measures 7.41:1, comfortably
  passing). `color.accent_active` is shared with the button family's
  pressed state; still not this family's hex to invent a fix for, per
  Loop 2's own finding, restated rather than silently dropped.
- **Radius consolidation ask from Loop 2 is now resolved, confirmed**:
  `radius.btn`/`radius.input`/`radius.menu` all read `4` in both maytera
  files today (B6, settled in Loop 2), matching `radius.control=4`. No
  further action needed; noted here only so this addendum does not
  silently repeat an already-closed ask.

## Evidence limitation, stated plainly

The director's Loop 3 evidence bar asks for keyboard focus traversal
captured via screenshot, in both themes, with the ring's contrast measured
against the control and its surround. That could not be produced for this
family: as established above, nothing in the shipping engine currently
draws a focus ring (or reads any per-state token at all) for checkbox,
radio, dropdown, slider, or scrollbar - three independent checkbox
implementations were re-confirmed this loop to read at most 3 legacy keys
each, none of the six-state model. A screenshot of a keyboard-focused
checkbox on the current golden would show the SAME unchanged rest-state
pixels regardless of which `.mtheme` file is active, because the draw code
never asks. What is reported instead is the same class of evidence Loop 2
already gave: the token VALUES computed against the actual shipped hex,
with the formula stated, so whichever pass wires the spec above inherits
correct numbers rather than needing to re-derive them. `color.focus_ring`
against the field/checkbox/radio rest surface: **5.89:1 dark, 6.20-6.71:1
light** (both comfortably clear the 3:1 non-text-UI floor with margin,
computed against `state.input_rest_bg` and, for light theme, additionally
against `color.surface`/window bg since the ring sits 1px outside the
control on whichever surface it is placed on).

# Window decorations: Loop 3 elevation + outer-corner-radius spec (family 1, #711)

Owner: window decorations (titlebar, buttons, frame, border, resize grip,
winmenu). This section is the director's loop-3 redirect, section 1's radius
scale (window portion) and elevation/shadow tokens, plus Substrate priority
1 ("Elevation model in the compositor... rounded window corners and a drop
shadow... the single largest scoring item in both reviews"). Titlebar
gradients, border tokens and the six-state titlebar-button work from loops
1-2 are untouched this loop, per the director's explicit "Titlebar
gradients and border tokens are DONE; do not touch them."

## This reverses an earlier ruling of mine - stated explicitly, not silently

Loop 1's own spec (`radius.none on window outer corners: compliant by
construction`, line 1558 above) and the surfaces family's `Elevation model
(four levels, no shadows)` (line 1604 above) both stated, correctly at the
time, that window outer corners are always 0 and that shadows do not exist
in this system (#189). The director's loop-3 redirect asks for both anyway,
scored as the single largest defect in both loop-2 reviews. Neither prior
section is rewritten in place (not this addendum's paragraph to edit); both
are correct history of what loop 1/2 shipped, now superseded for the window
OUTER FRAME specifically by what follows. Every other use of `radius.none`
(menus, dropdowns already-square corners, etc.) is unaffected.

## Why this is renderable: zero new primitives, not scoped engineering

I checked before speccing rather than assuming the renderer-limits ban still
applies. It does not, for this specific case:

- `kernel/gui/window_decor.c` already ships
  `decor_fill_rounded_rect(x, y, w, h, radius, color)`: a real per-pixel
  FILLED rounded rect. The corner test is a distance-squared circle fill
  (`dist_sq <= radius*radius`), not a stroked Bresenham outline, so this is
  not the "circles below 14px look ragged" limit (that limit is about
  stroked/outlined circles specifically; a small filled quarter-disc at the
  same technique already ships in production, see next point).
- `kernel/gui/login.c` already calls it in production, twice, including the
  exact "offset opaque copy behind, real surface on top" shadow technique
  this spec asks for: `decor_fill_rounded_rect(x+2, y+3, w, h, h/2,
  LOGIN_SHADOW_COLOR)` then `decor_fill_rounded_rect(x, y, w, h, h/2,
  LOGIN_INPUT_BG)` (login.c:211-212), and again for the avatar circle
  (login.c:255-256).
- `kernel/gui/window.c`'s own `wm_draw_winmenu()` already does the same
  offset-silhouette trick, without rounding, for the popup decorator menu:
  `fb_fill_rect(x+3, y+3, w, h, 0x00101418)` then the popup body
  (window.c:1265-1266) - proof the "hard silhouette behind, real surface on
  top" shadow shape already reads correctly on a real shipped surface.
- `fb_fill_rect_alpha(x, y, w, h, color, alpha)` (framebuffer.c:559)
  already exists and is already proven at FULL-SCREEN scale by login.c's
  own scrim (`LOGIN_SCRIM_ALPHA=145`). A small per-window shadow region is
  comfortably inside the "uniform per-draw alpha allowed for small regions"
  exception in the renderer-limits list above; it is nowhere near the
  banned full-screen persistent-scrim case (that ban is about REDRAW COST
  of a whole-screen damage rect every frame, not about alpha itself - see
  the surfaces family's B8 investigation, `maytera_dark.mtheme`, for the
  full argument and the one open question it left for Substrate: does the
  compositor freeze repaint under an open modal. Still open; not this
  family's code to answer).

The only thing missing is a call site: `window.c`'s top-level frame draw
uses a plain `fb_fill_rect` with no radius parameter at all. Wiring is a
call-site change (swap that one call, add one shadow call before it, gated
by `decor.window_shadow`), not new engineering to invent a primitive.

## New tokens (all 14 shipped theme files, verified present via grep after
## writing, not assumed)

```
radius.window          window OUTER corner radius (decor_fill_rounded_rect's
                        radius arg). 6 for decor.style=gradient themes
                        (11 files: maytera_dark, maytera_light, dark,
                        fluent_dark, fluent_light, forest, light,
                        modern_dark, modern_light, ocean, sunset). 0 for
                        decor.style=beveled themes (3 files: classic,
                        high_contrast, retro_unix) - explicit 0, not an
                        unset-key default, so intent is on record.
decor.window_shadow     0/1, gates the shadow draw. 1/0 matching the same
                        beveled-vs-gradient split as radius.window above.
shadow.color            0x00000000 (pure black) in every file - not a
                        per-theme colour choice the way a surface tint is.
shadow.alpha            90 (of 255, ~35%) base/menu-level window shadow.
shadow.offset_y         4px downward offset, shared by base and modal.
shadow.blur             12px total ramp spread (NOT a convolution radius,
                        see algorithm below - this renderer has no blur).
shadow.modal_alpha      120 (of 255, ~47%), modal windows only.
shadow.modal_blur       16px ramp spread, modal windows only.
```

All eight lines are `0` in the three beveled themes (explicit off, not
absence).

## The "blur" algorithm (concrete, because this renderer cannot blur)

`shadow.blur` is not a Gaussian radius; there is no convolution primitive
here and the renderer-limits list bans one. It is the total pixel spread of
a 3-STEP RAMP, exactly the director's own "3-step darkened ramp against the
sampled backdrop is acceptable for loop 3; zero shadow is not" allowance,
turned into numbers an implementer can build directly:

1. Before drawing the window frame, call `decor_fill_rounded_rect` /
   `fb_fill_rect_alpha` three times at the window's bounds, radius
   `radius.window`, each offset `shadow.offset_y / 3` further down than the
   last (so the three steps land at roughly 1px, 3px and `shadow.offset_y`
   px of vertical offset for the base 4px case), colour `shadow.color`, and
   alpha stepping down across the three calls from `shadow.alpha` toward 0
   (e.g. `alpha`, `alpha*2/3`, `alpha/3`) so the outer edge reads softer
   than the inner one without a real blur.
2. Then draw the frame itself (rounded fill, border, titlebar, content) on
   top, exactly as today, just with `decor_fill_rounded_rect` in place of
   `fb_fill_rect` for the outer frame fill.
3. Modal windows (an existing `window_t` flag) use `shadow.modal_alpha` /
   `shadow.modal_blur` in the same algorithm instead of the base pair -
   this alone satisfies "modal shadow stronger than parent" without a
   separate code path, since it is the same three calls with two different
   numbers read off the theme.

This is data-driven per the effort's defining constraint: every number in
the algorithm above (offset, alpha steps, blur spread) comes from the
tokens, not a compiled constant, so retuning the shadow is a `.mtheme` edit,
not a rebuild.

## What I did not do, and why

- **Did not build or wire this.** Only the Substrate phase may build this
  loop, per house rules. This section is the spec Substrate implements
  against, using the exact existing functions/line numbers cited above so
  there is nothing left to invent.
- **Did not verify pixel-by-pixel.** My brief asks me to "verify
  Substrate's implementation pixel-by-pixel: corner ramp present, shadow
  gradient measurable at every window edge, modal shadow stronger than
  parent" - I cannot do that yet: as of this loop no commit exists that
  implements it (grepped `kernel/gui/window.c` for `decor_fill_rounded_rect`
  and `shadow`: zero hits before this loop's spec). This verification is
  the next step after a Substrate build lands, not something I can fabricate
  now. Stated honestly rather than claimed.
- **Did not touch `radius.control` / `radius.selection`.** Those already
  exist (Family 3's loop-1/2/3 work, `state.field_*`/checkbox/radio/
  dropdown geometry) at the same numeric value (4) the director's section 1
  asks for under the name `metric.control_radius`/`metric.selection_radius`.
  Rather than add a second, differently-named key for the same number
  (ambiguous for this line-reader parser, and outside my family - control/
  selection radius is content-area widget territory, families 2-5), I am
  flagging the naming mismatch to the director: section 1 calls these
  `metric.window_radius`/`metric.control_radius`/`metric.selection_radius`
  ("metric" namespace), but every existing corner-radius key in every
  shipped theme uses the `radius.*` namespace (`radius.btn`, `radius.input`,
  `radius.menu`, `radius.card`, `radius.control`, `radius.selection`,
  `radius.pill`, `radius.scrollbar_thumb`, `radius.slider_thumb`). I used
  `radius.window` for consistency with that established convention instead
  of introducing the only `metric.*_radius` key in the system. `radius.
  control`/`radius.selection` already carry the requested value 4 in all 14
  files (Family 3's work, not duplicated here).
- **Did not touch `color.selection_bg`/`color.selection_fg`.** Already
  satisfied: `color.sel_bg`/`color.sel_fg` have existed since the #711
  substrate and already equal `color.accent`/`0xFFFFFF` in both maytera
  themes, exactly the director's section-1 ask under a slightly different
  informal name. No new key needed; applying it to menu/list rows with
  radius 4 and a 4px inset is families 2-5's consumer-side job, not a token
  I own.

## Substrate wiring checklist (mechanical, additive to the existing pattern)

- `kernel/gui/themes.h`: append `TM_RADIUS_WINDOW`, `TM_DECOR_WINDOW_SHADOW`,
  `TM_SHADOW_COLOR`, `TM_SHADOW_ALPHA`, `TM_SHADOW_OFFSET_Y`,
  `TM_SHADOW_BLUR`, `TM_SHADOW_MODAL_ALPHA`, `TM_SHADOW_MODAL_BLUR` (appended,
  never renumbered, mirrored in `userland/libc/theme.h` per the existing ABI
  comment - same pattern `TM_TITLE_INSET` used in loop 2).
- `kernel/gui/themes.c`: eight matching `g_theme_fields`/int-fields rows,
  defaults 0 (so an unset key never shadows/rounds by surprise).
- `kernel/gui/window.c`: the frame-fill call site (currently a plain
  `fb_fill_rect`, see `window_draw()`) becomes `decor_fill_rounded_rect(...,
  win_metric_or(TM_RADIUS_WINDOW, 0), border_color)`; a new shadow block
  runs the 3-step ramp above, gated on `win_metric_or(TM_DECOR_WINDOW_SHADOW,
  0)`, before that call.


# Buttons: Loop 3 update (final loop, family: buttons, #711 follow-on)

Owner: buttons family (loop 3). This loop's director redirect did not name
buttons in the Substrate build's priority list (items 1-9), so nothing in
this family gets wired by this loop's one allowed build regardless of what
is specified here. Given that, and given Loop 1/2 already produced a
complete, contrast-checked six-variant/six-state token matrix now sitting
correctly DEFERRED in both `.mtheme` files, this loop's job was an AUDIT,
not more token surface: re-verify the existing data survives concurrent
edits and still passes contrast, and sharpen the engineering diagnosis so
the eventual wiring pass is exact rather than another rediscovery. No
`color.*`/`state.*`/`btn.*` key value changed in either `.mtheme` file this
loop; see the "Loop 3 (buttons family)" comment block appended to the tail
of `build/assets/themes/maytera_dark.mtheme` and `maytera_light.mtheme` for
the full text this section summarizes.

## What was re-verified

Independently recomputed (not re-typed from the Loop 1/2 report) WCAG
relative-luminance contrast for three representative pairs per theme:

| Pair | Dark | Light |
|---|---|---|
| `btn.danger.*` fill vs `color.on_danger` | 4.58:1 | 5.07:1 |
| `btn.primary.hover.bg` vs `color.on_accent` (tightest primary pair) | 4.73:1 | 5.06:1 |
| `btn.segmented.rest.fg` vs `.rest.bg` | 6.03:1 | 6.00:1 |

All six pass the 4.5:1 AA floor, matching (within rounding) Loop 1/2's own
figures. `theme-scale-lint.sh` re-run after this loop's append: `2 clean, 12
grandfathered - RESULT: OK`, unchanged.

**Honesty note, not smoothed over:** the first hand computation of the
segmented pair this loop produced a false **2.27:1 FAIL** from a single
decimal slip (`0.7152*0.01765` mistyped as `0.126232` instead of
`0.012624` - a factor-of-10 error propagated into the luminance sum). It
was caught by redoing the arithmetic before editing any file on its
strength, not after. No data was changed based on the wrong number. Recorded
here per this repo's own "verify the artifact, not one hand-run number"
standard - a wrong contrast claim almost got written into a design spec by
this same designer, in this same loop, and the record should say so.

## Cross-family finding: an attribution correction, not a fix

Family 3/6's own Loop 3 note in this document flags `color.accent_active`
at 2.42:1 against `color.surface_sunken` as "shared with the button
family's pressed state." Recomputed independently and confirmed exact:
**2.42:1 in `maytera_dark.mtheme`, 7.40:1 in `maytera_light.mtheme`** (a
scheme-asymmetric defect - dark fails the 3:1 non-text UI floor, light
passes comfortably). But every use of `color.accent_active` inside this
family's own `btn.*` block was checked line by line
(`btn.primary.rest/hover.border`, `btn.primary.active.bg`,
`btn.toolbar/pill/segmented.selected.border`): **none of them pair
`accent_active` against `surface_sunken`.** Button fills sit against
`surface`/`surface_raised`, or flush against their own identical border
(`bg==border`, the nested-fill technique's "no separate contrast concern"
case). The pairing that actually produces 2.42:1 is
`state.radio_ring_active` (= `accent_active`) against `state.radio_bg`
(= `surface_sunken`) - a forms-family (Family 3) control, the radio
button's active ring stroke, not any button state. Flagging the correction
so a future pass does not "fix" this against the wrong control (which would
leave the real defect - the radio ring - unfixed while touching an
uninvolved token), and does not duplicate the fix once each family finds it
independently. Not mine to fix either way: `color.accent_active` is a
shared hex this family consumes but does not own, and the radio ring is
Family 3's geometry section.

## Engineering gap: reconfirmed, and sharpened past Loop 2's "C2" spec

`kernel/gui/themes.c`'s `g_theme_fields[]` still has **0 entries** for any
`btn.<variant>.*` key or the five `state.btn_*_border`/
`selected_focus_ring` keys - re-grepped this loop, unchanged since Loop 2.

Loop 2's own wiring ask ("make `gui_draw_button()` read `theme->s_btn_*`")
turns out not to be achievable as written, and this loop traced exactly
why, reading the actual call chain rather than restating the gap:

- `theme_t` is a **kernel** struct (`kernel/gui/themes.h`). The real, live
  button primitive every app actually calls is `gui_button()` in
  `userland/libc/gui.c` - **Ring 3** code. Ring 3 has no memory visibility
  into a kernel struct; it can only reach theme colour data through a
  syscall, and the only one that exists for colour is `SYS_THEME_COLOR`,
  which dispatches through `theme_get_color_by_id()`
  (`kernel/proc/syscall.c` / `kernel/gui/themes.c`), a **closed switch of
  66 `THEME_COLOR_*` ids**. Read every case: ids 0-65 map to the 51 legacy
  fields plus a handful of v2-aliased ones (`c_accent`, `c_sel_bg/fg`,
  `c_border_strong`). **No `state.btn_*` or `btn.<variant>.*` id exists at
  any layer userland can reach today** - not "unwired," structurally
  absent from the enum.
- `gui_button()` draws from `gui_palette_t` (`userland/libc/gui_style.h`):
  10 fields - `surface`, `surface_raised`, `ink`, `ink_dim`, `accent`,
  `accent_hover`, `border`, `field_bg`, `field_border`, `track`. **No
  per-state slot, no per-state border, no `focus_ring` field at all.** It
  cannot express this family's six-state/bordered/distinct-focus-ring spec
  no matter what a `.mtheme` file says, because the C struct has nowhere to
  put the extra values.
- `gui_palette_t` is populated per-app by **25 separate
  `gui_set_palette()` call sites**, most (per Family 3's Loop 2 audit,
  reconfirmed here for buttons specifically) from a private hardcoded
  per-theme table in that app's own source, not from `SYS_THEME_COLOR` at
  all.
- `gui_style_sync_from_theme()` (the one function that DOES read live
  theme data into a global) only touches `g_style` (radius/gradient/decor
  family). **It never touches `g_pal`.** There is no
  `gui_pal_sync_from_theme()` equivalent.

**Corrected wiring spec, three necessary parts (Loop 2's spec was really
only naming part of part 1):**

1. Add new `SYS_THEME_COLOR` ids (or a new syscall) exposing
   `state.btn_*`/`btn.<variant>.*` through `theme_get_color_by_id()`'s
   switch - the same aliasing technique already used for `color.accent`
   (id 2).
2. Extend `gui_palette_t` with the per-state/border/focus-ring slots
   `gui_button()` needs.
3. Give `gui_style_sync_from_theme()` (or a new sibling call) the job of
   actually filling `gui_palette_t` from the new ids, and audit the 25
   existing `gui_set_palette()` call sites off their private tables.

Not buildable from a data-only pass (this loop has no build access;
Substrate is the only builder and buttons was not in its priority list).
Flagged precisely, with file/function names, so whichever future loop picks
this up is one engineering pass, not a third rediscovery.

## LIVE, confirmed defect: not deferred, not theoretical

Read directly off the shipping source this loop, independent of the wiring
gap above: `gui_button()`'s focus ring draws in `p->accent`
unconditionally.

```c
// userland/libc/gui.c, modern style, ~line 649:
if (g_style.base == GUI_STYLE_MODERN)
    gui_rounded_border(handle, x, y, w, h, GUI_BTN_RADIUS, p->accent);
else
    gui_draw_rect_outline(handle, x, y, w, h, p->accent);   // classic style
```

For `GUI_BTN_PRIMARY`, the button's own **fill** is also `p->accent`
(~line 611: `base = p->accent`). A focused primary/default button therefore
draws its focus ring in the exact same colour as its own fill - an
invisible ring, on every app that calls `gui_button()` with
`GUI_BTN_PRIMARY`, in both Maytera themes, on the current golden, today.

This is precisely the B9 defect Loop 2 already fixed **in the data**
(`state.btn_selected_focus_ring=color.on_accent`, both `.mtheme` files,
since Loop 2). That fix has had no way to reach the screen, because
`gui_button()` reads neither `state.btn_selected_focus_ring` nor any
per-variant token - only the 4-colour subset of `gui_palette_t` it already
has. **Top-priority item once the three-part wiring above lands:** add a
`focus_ring` field to `gui_palette_t`, and for `GUI_BTN_PRIMARY`/
`GUI_BTN_SUCCESS` use an on-fill-colour ring instead of `p->accent`,
matching what `state.btn_selected_focus_ring` has specified correctly since
Loop 2. This is also the concrete answer to this loop's evidence-bar
requirement that focus rings be verified, not assumed: I could not capture
a live screenshot of a focused button (no build access this loop), but I
can say with certainty, from source, that on every real primary button in
this OS the ring is currently unverifiable-because-invisible, which is a
stronger and more useful finding than an unmeasured screenshot would have
been.

## Shared-token asks still open (unchanged from Loop 1/2, not re-litigated)

- `color.danger_hover`/`color.danger_active` (would let `btn.danger` get a
  real fill-darkens-on-press treatment instead of the ring-moves-instead
  technique it uses today - not blocking, the ring technique is complete).
- `radius.control=4` is data-correct in both `.mtheme` files but still has
  0 entries in `kernel/gui/themes.c`'s `g_theme_fields[]` (re-checked this
  loop) - same wiring class as everything above, not a new ask.

Neither is this family's hex/scale to add; both are restated once, for the
record, not repeated as new findings.

## Renderer limits this family did not use (unchanged from Loop 1)

No drop shadow on any button state; no gradient beyond the single flat fill
already justified in Loop 1; no per-pixel alpha for any state transition -
every state is a distinct named token even where the code to read it does
not exist yet; no stroked rounded-rect primitive (nested-fill only); no
antialiased or rounded focus-ring corner.
