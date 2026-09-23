# MayteraOS Glass Design System

The visual language of the first-boot wizard: dark glass cards on a live wallpaper,
rounded corners, teal accent. This document is the single source for building any
other form, dialog or window in that language.

Every value here is **taken from shipping code**, not from a mockup. Where a value
differs from the original design mockup, the reason is recorded, because those
differences are all deliberate corrections and re-"fixing" them re-breaks the thing
they solved. Reference implementation: `userland/apps/setup/main.rs`.

This language is **not** the retro CDE/Motif look in `docs/UI_STYLE_GUIDE.md`. That
one still governs the retro-lineage themes. Use this one for new modern surfaces,
and do not mix the two inside a single window.

**Process rule, standing:** design a new screen in HTML/CSS first, get it approved,
then port it. Do not design directly in Rust. What ports is the spec (exact geometry
plus these tokens), not the pixels.

**Sections 1-9 are the wizard's own fixed-palette recipe, verbatim from shipping
code.** Sections 10-11, added for the App Store/Task Manager/Browser-chrome glass
redesign (owner request, App Store first as the pilot), cover the two things a
*themed, multi-window* app needs that the wizard never had to solve: adapting the
palette to 14 live themes instead of one fixed dark-teal one, and the anti-flash
publish requirement every such app must meet. Read those before porting this
language into any app that is not a single fixed-theme, run-once surface.

---

## 1. Colour tokens

Copy the names as well as the values. A second name for the same hex is how two
surfaces drift apart later.

### Surface

| Token | Hex | Use |
|---|---|---|
| `WEL_BG_TOP` | `#0A1614` | Backdrop gradient, top stop |
| `WEL_BG_MID` | `#122420` | Backdrop gradient, mid stop. **Also the glass tint.** |
| `WEL_BG_BOTTOM` | `#050A09` | Backdrop gradient, bottom stop |
| `DK_CARD_FILL` | `#0E1D1B` | Nested card/result panel inside a glass card |
| `DK_THUMB_HALO` | `#050A09` | Halo behind a thumbnail so it survives any wallpaper |
| `DK_PROGRESS_TRACK` | `#16241F` | Progress bar track |
| `DK_BADGE_FILL` | `#123B32` | Badge/pill background |

### Ink

| Token | Hex | Use |
|---|---|---|
| `DK_HEADLINE` | `#F3FBF9` | Page titles, selected option labels |
| `DK_BODY` | `#A9D9CC` | Body copy, subtitles, option sub-lines |
| `DK_FINE_PRINT` | `#ADC7BF` | Fine print. Full opacity, **never** faded further |
| `DK_FIELD_LABEL` | `#8FCFC0` | Field labels (the small caps eyebrow above an input) |
| `DK_EYEBROW` | `#6AE2CF` | Section eyebrow, small caps |
| `DK_LINK` | `#C6C6C6` | Inline link text |
| `WEL_LINK` | `#AAAAAA` | Welcome-page link. True neutral grey, R=G=B |
| `DK_ERROR` | `#FFAAA2` | Validation and error messages |
| `DK_BADGE_TEXT` | `#6AE2CF` | Badge label |

### Accent and edges

| Token | Hex | Use |
|---|---|---|
| `DK_ACCENT` | `#6AE2CF` | Selection, focus, eyebrow, active indicator |
| `WEL_GLOW` | `#6AE2CF` | Backdrop glow (same hue as the accent) |
| `DK_EDGE_GLASS` | `#6FA99E` | Any stroke drawn **straight onto glass** |
| `DK_STROKE_UNSEL` | `#2C4A44` | Inner stroke on `DK_CARD_FILL` only |
| `DK_STROKE_UNSEL_B` | `#4A6B64` | Resting stroke where a card fill is behind it |
| `DK_BACK_BORDER` | `#4A6B64` | Secondary button border |
| `DK_BACK_LABEL` | `#A9D9CC` | Secondary button label |
| hairline rule | `#1E322E` | 1px horizontal divider (`dk_hr`) |

**The edge rule that keeps being relearned:** a stroke drawn directly on glass must
use `DK_EDGE_GLASS`. `DK_STROKE_UNSEL_B` on glass measures **1.46:1** over a white
wallpaper and fails the 3:1 non-text floor outright. It is only safe over
`DK_CARD_FILL`.

### Controls

| Token | Hex | Use |
|---|---|---|
| `DK_BTN_TOP` | `#0F8068` | Primary button gradient, top |
| `DK_BTN_BOTTOM` | `#0A5D4C` | Primary button gradient, bottom |
| `DK_BTN_TEXT` | `#FFFFFF` | Primary button label |
| `DK_INPUT_FILL` | `#213B34` | Input fill |
| `DK_INPUT_BORDER` | `#4E7168` | Input border, resting |
| `DK_INPUT_FOCUS` | `#6AE2CF` | Input border, focused |
| `DK_INPUT_TEXT` | `#EAF6F2` | Input value text |
| `DK_INPUT_PH` | `#87ABA2` | Input placeholder |
| `DK_INPUT_FILL_DIS` | `#121F1C` | Input fill, disabled |
| `DK_INPUT_BORDER_DIS` | `#31504A` | Input border, disabled |
| `DK_INPUT_TEXT_DIS` | `#5C7A72` | Input text, disabled |
| `DK_TOGGLE_TRACK_OFF` | `#1C2E2A` | Toggle track, off |
| `DK_TOGGLE_THUMB_OFF` | `#7C9C94` | Toggle thumb, off |
| `DK_TOGGLE_THUMB_ON` | `#F3FBF9` | Toggle thumb, on |

---

## 2. The glass

One recipe. There must never be a second.

```
bleed        36 px
downsample   4x nearest
blur         3 separable box passes, r=3 (w=7), sigma 13.90
tint         #122420 (WEL_BG_MID) at 0.80
edge         1 px white at 0.34   (CARD_EDGE_A = 340)
highlight    top edge, white at 0.44  (CARD_HL_A = 440)
```

**Why the downsample is nearest and not a 4x4 box:** `SYS_DECODE_IMAGE` is the only
route into a userland buffer and its scaler is pure nearest-neighbour point sampling.
Measured max deviation is 2.68 levels of 255, which moves sigma from 13.90 to 13.86,
and the aliasing is destroyed by the three box passes that follow. **Do not add a
corrective pass to chase two levels.**

**Why the tint is 0.80 and not the mockup's ~0.35:** 0.80 is solved for, not chosen.
It is the lowest value at which every ink in the design clears its contrast floor
over a pure white wallpaper, including the weakest mark (an inactive step dot, a
non-text indicator needing 3:1). At 0.35, white body text measures **2.14:1** over a
white wallpaper and fails 4.5:1 by a wide margin. It still keeps 20% of the blurred
backdrop, about a 35-level swing across the shipping wallpapers, so the card reads as
a material and not a flat panel. **Do not "restore" the mockup value.**

### Sampling the backdrop

Any control that draws its own anti-aliased edge needs the colour actually behind it
at that point, because the backdrop is a live gradient plus glow, not a flat fill:

```rust
let outer = wel_composite_at(cx, cy);   // composited backdrop at a point
let c     = wel_blend_over(fg, bg, alpha_permille);  // alpha is per-mille, 0..1000
```

Pass `outer` as the AA primitive's outer colour. Skipping this is what produces a
faint square halo around a round control.

---

## 3. Corners and shadow: the contract

This is the part that took four attempts to get right, so it is written down as a
contract rather than as a technique.

| Constant | Value | Meaning |
|---|---|---|
| `CARD_R` | 16 | Card/window corner radius |
| `CORNER_BOX` | 16 | Arc bounding square. **Must equal `CARD_R`** |
| `SH_SPREAD` | 32 | Shadow spread |
| `SH_PEAK` | 89 | Shadow peak alpha |
| `SH_OFFX` | 0 | Shadow X offset |
| `SH_OFFY` | 6 | Shadow Y offset |
| `SH_R` | 16 | Shadow corner radius. **Must equal `CARD_R` and `CORNER_BOX`** |

**The rule:** the compositor paints a window's drop shadow onto the wallpaper
*outside* the window rect. Anything inside the window that fakes transparency by
sampling the wallpaper must apply *the same shadow* to its sample. If it samples raw
unshadowed wallpaper, there is a brightness step exactly at the window's rectangular
edge, and that step is visible as **square corners around your correctly rounded
ones**. Measured, it was a 51-level jump; applying the shadow maths inside took the
seam from 33-85 levels down to 0-6.

So: three radii must move together (`CARD_R`, `CORNER_BOX`, `SH_R`), and any
in-window backdrop sample must go through the shadow function, not around it.

---

## 4. Layout

The wizard window is **640 x 480**, borderless, centred from `SYS_FB_INFO`. Use these
as proportions, not absolutes, if your surface is a different size.

| Element | x | y | Size |
|---|---|---|---|
| Content margin | 32 | | 576 wide (32 to 608) |
| Page title | 32 | 24 | 20px bold |
| Subtitle | 32 | 50 | 12px, alpha 880 |
| First content row | 32 | ~76-84 | |
| Section eyebrow | 32 | | 10px bold small caps |
| Field label | 32 | label y | 10px, sits 14px above its input |
| Input | 32 / 336 | | 272 x 28, two per row |
| Divider (`dk_hr`) | 32 | | 576 x 1 |
| Secondary button | 32 | footer | 88 x 28 |
| Primary button | 468 | footer | 140 x 28 |
| Skip link | left | footer + 8 | 11px |

Grid pitches in use: option rows **38px** apart, input rows **56px**, thumbnail cells
**112 x 74**, theme cards **74 x 64**.

**Footer y must be one constant.** It is referenced by both buttons, the skip link,
and the focus frames on every page that can put focus on a button. Hardcoding it per
page is how buttons and their focus rings separate.

---

## 5. Typography

One family, size by role. Sizes are TTF pixel sizes.

| Role | Size | Weight |
|---|---|---|
| Hero button label | 20 | regular |
| Page title | 20 | bold |
| Option label, body, buttons | 12 | bold for labels and buttons, regular for body |
| Field label, eyebrow, sub-line, fine print | 10 | bold for eyebrows and labels |
| Skip link | 11 | regular |

**Hard constraint: the glyph cache has ten size buckets starting at 12.** Every size
below 12 renders at 12 and is then scaled. Treat 10 and 11 as the only sub-12 sizes
worth using, and never assume a 9 or an 8 will look different from a 10.

**The rasterizer's y is the top of the LINE box, not the cap-height box.** Design
specs are usually written the other way. Derive a label's y from the control it must
look centred in, rather than from a spec number, so it cannot drift if the control
moves. Measured on real pixels, the difference was 2.5px for a 20px label.

---

## 6. Controls

### Hero button (the "Get Started" pill)

```
size    246 x 51, radius 25
edge    ring-then-hole: gui_rr(x, y, w, h, 25, edge_col, pill_bg)
        then gui_rr(x+1, y+1, w-2, h-2, 24, fill_col, edge_col)
edge_col  white over the composited backdrop at CARD_EDGE_A (0.34)
fill_col  DK_ACCENT over the composited backdrop at 0.20
label   20px white, centred
arrow   stroked with dk_line, never a typed codepoint
```

**Ring-then-hole, not `frame_inward`**, because `frame_inward` puts a *square* frame
on a round pill. Same construction as `dk_radio`.

**The fill is 0.20, lower than the mockup's ~0.28.** The pill is defined by its edge,
not its fill (the fill measures only 1.48:1 against glass at either value), and a
lighter fill raises the surface the white label sits on. Lowering it *raises* label
contrast, 4.91 to 5.80.

**The arrow is stroked, not typed.** Nothing guarantees the loaded TTF carries an
arrow codepoint, and a guessed codepoint that renders as a blank box is not closer to
the spec than a drawn one.

### Primary button

```
size    140 x 28, radius 5
fill    vertical gradient DK_BTN_TOP -> DK_BTN_BOTTOM
label   12px bold, DK_BTN_TEXT, centred
disabled  blend both stops and the label over the backdrop at alpha 420
```

### Secondary button

```
size    88 x 28, square (frame_inward, 1px)
border  DK_BACK_BORDER
label   12px bold, DK_BACK_LABEL, centred
```

### Radio

13px overall. Ring, then hole, then dot:

```rust
gui_fill_circle_aa(win, cx, cy, 6, ring, outer);      // ring
gui_fill_circle_aa(win, cx, cy, 5, outer, ring);      // hole
if selected { gui_fill_circle_aa(win, cx, cy, 3, DK_ACCENT, outer); }
```

`ring` is `DK_ACCENT` when selected, `DK_EDGE_GLASS` otherwise. Selected also sets
the label bold and near-white. A radio has exactly **two** appearances: moving into a
radio group selects immediately, so there is no browse-without-committing state for a
third appearance to show.

### Toggle

```
track   34 x 17, radius 9
on      gradient DK_BTN_TOP -> DK_BTN_BOTTOM
off     DK_TOGGLE_TRACK_OFF
thumb   r=6 AA circle, x+25 when on, x+8 when off
```

### Input field

```
size    272 x 28 typical, radius 4
fill    DK_INPUT_FILL, border 1px DK_INPUT_BORDER
focus   keeps the 1px border, adds the focus ring (below)
disabled  DK_INPUT_FILL_DIS / DK_INPUT_BORDER_DIS / DK_INPUT_TEXT_DIS
```

A focused field keeps its **normal 1px border**. It does not get a 2px inward border:
that is the grammar grids use for *selection*, and conflating the two is exactly what
the focus ring exists to prevent.

### Focus ring

```rust
frame_inward(win, x - 4, y - 4, w + 8, h + 8, 2, DK_ACCENT);
```

2px accent, 2px outside the control edge. It is shared, so every focusable control in
the surface moves together rather than one page at a time.

**Focus and selection are the same hex on purpose.** A second focus hue would be an
unmeasured colour. They are told apart by **geometry**: focus sits 2px *outside* the
edge; selection is a 3px border *inset* plus a 15% fill. A control that is both shows
both, nested, and still reads correctly.

Use a **group** focus frame (around a whole radio group) where a per-control frame
would be meaningless. Measured 9.36:1 and 8.99:1 against the composited backdrop.

### Checkbox

Unchecked is a hollow 2px inward frame with **no fill**. The obvious
`DK_INPUT_FILL`/`DK_INPUT_BORDER` pair measures 1.43:1 and 1.44:1 against glass and
fails. Do not put a card fill behind an unchecked item to make it visible.

### Nested card

```
radius  6
fill    DK_CARD_FILL
edge    frame_inward 1px DK_EDGE_GLASS
```

Use for read-only result panels. Show **values**, not disabled inputs: a disabled
input says "you could have edited this", a value says "this is what it is".

---

## 7. Contrast floors

Non-negotiable, and the reason most of the odd-looking values above exist.

- **4.5:1** for text
- **3:1** for non-text marks (indicators, strokes, icons)

Two facts that keep being got wrong:

1. Contrast against a fixed colour is **monotonic in backdrop luminance**. So black
   and white bound a **single** mark: test both and you have covered every wallpaper.
2. A **two-member pair** (a mark that must be distinguishable from its own other
   state) has its worst case **in the middle**, not at the extremes. Testing black and
   white alone will pass a pair that fails on a mid-grey wallpaper.

Measure on the **rendered pixel**, against the **right reference**. An anti-aliasing
"residual" of 29.7 levels was once entirely an artifact of comparing against an
unblended surface; the true value was 0.24.

---

## 8. Rules that are not obvious from the code

**Do not grey a control that still does something.** Greying is truthful when the
value is genuinely used by nothing (a static IP field under DHCP). It is a lie when
the setting still applies in some other context, and a false disabled state is worse
than a less guided one. Prefer a line of text saying *when* the setting applies.

**A hidden control must have a dead hit-test.** If you stop drawing something, remove
its click region and its keyboard stop in the same change. A radio pair that was no
longer drawn kept firing on click and silently rewrote a setting with nothing on
screen changing to say so.

**One definition for anything three places agree about.** Page size, grid pitch,
footer y, corner radius. Three literal copies of the same number is how a pager ends
up skipping a row.

**A control that does nothing must not be drawn.** A pill with no target behind it is
worse than no pill. Omit it; the layout should not depend on it.

**Anti-aliased geometry needs the backdrop.** Pass `wel_composite_at()` output to
every AA primitive. See section 2.

**Judge small artwork at 8x-10x nearest-neighbour zoom.** Artwork under ~24px cannot
be assessed at 1:1. Several icons shipped twice because they were judged from 1:1
screenshots.

---

## 9. Available primitives

Use these. Do not re-implement them per surface.

| Primitive | Purpose |
|---|---|
| `gui_rr` | Rounded rect, AA, over a known outer colour |
| `gui_fill_rounded_grad` | Rounded rect with a vertical gradient |
| `gui_fill_circle_aa` | AA circle |
| `frame_inward` | Square frame, inward from the given rect |
| `dk_line` | Stroked line (there is no diagonal primitive; lines are stamped rects) |
| `dk_tri` | Small triangle, stamped as three rows |
| `dk_checkmark` | Check glyph |
| `wel_composite_at` | Composited backdrop colour at a point |
| `wel_blend_over` | Alpha blend, per-mille |
| `card_cov` | Rounded-rect coverage 0..255 for a point |
| `shadow_alpha_win` | Shadow alpha at a point, for in-window backdrop samples |

If a primitive is missing or too weak, **improve the shared one**. Do not fork a
private copy into your surface.

---

## 10. Applying this language to a themed, multi-window app (the App Store pilot)

The wizard is a single fixed-theme surface: always dark, always teal
(`DK_ACCENT #6AE2CF`, `WEL_BG_MID #122420`). App Repo (`userland/apps/appstore/`,
display name "App Repo", component name `appstore`), Task Manager, and the browser
chrome are not - they render under all 14 shipping themes including light ones, and
the owner's build order is App Store first, confirmed on hardware, before this
section's guidance is applied to the other two. This section is the same reasoning
`docs/GLASS_MODALS_AND_POPOUTS.html` already applied to the confirm dialog and the
CPU/RAM/DSK/NET pop-out, generalised to an app window rather than compositor chrome.

**What's reused (the pattern, not the hex).** The corner-radius contract (a card's
fill radius, its bounding box, and its shadow radius move together, section 3); the
edge/highlight/shadow layering grammar (section 2); the typography scale and control
specs (sections 5-6); the contrast-floor discipline (section 7, including the two
subtleties: a fixed-colour comparison is monotonic in backdrop luminance, so black
and white bound it, but a **two-member pair** - a hovered vs. unhovered row, a
selected vs. unselected category - has its worst case **in the middle**, so testing
only the extremes is not enough).

**What's different: colour comes from the theme, not from a literal.** App Store
already derives its palette live at startup from `theme_color()`
(`userland/apps/appstore/main.c:449-475`):

```c
uint32_t wbg  = theme_color(THEME_COLOR_WINDOW_BG);
uint32_t ink  = theme_color(THEME_COLOR_LABEL_TEXT);
uint32_t acc  = theme_color(THEME_COLOR_ACCENT);
uint32_t bord = theme_color(THEME_COLOR_WINDOW_BORDER);
C_surface = wbg;
C_panel   = dark ? gui_lighten(wbg, 8)  : gui_darken(wbg, 6);
C_card    = dark ? gui_lighten(wbg, 16) : 0xFFFFFF;
C_hero1   = gui_mix(acc, wbg, 60);
C_hero2   = gui_mix(acc, wbg, 140);
```

MEASURED: for the shipping `modern_dark.mtheme` (`window_bg=#1e1e1e`,
`accent=#0a84ff`, `window_border=#3d3d3f`, `label_text=#f5f5f7`) and
`modern_light.mtheme` (`window_bg=#ffffff`, `window_border=#d1d1d6`), these are the
**exact same hex** this document's own §1 dark/light reference tables use for
`WEL_BG_TOP`-family surfaces and `DK_ACCENT`/accent-blue - the wizard's own
`WIZ_LIGHT`/`WIZ_DARK` tables (`main.rs:510-521`) already mirror the shipping
`modern_light`/`modern_dark` themes hex-for-hex. So "make App Store's glass
theme-aware" is not a new palette to invent: it is the SAME derivation the app
already has, with a glass LAYERING applied on top (gradient, edge stroke, highlight
line, soft shadow, rounded corners) rather than the current flat fill plus one
hairline. No new colour token is required for the pilot.

**Two-tier build recommendation for App Store's header and sidebar** (the two
surfaces the owner named):

- **Tier 1, recommended for the pilot: flat glass.** A translucent-*reading* panel
  built from `C_panel`/`C_surface` (already computed) through a vertical tint
  gradient (`gui_fill_rounded_grad`, already used by `draw_header()`) plus a 1px
  edge stroke, a 1px top highlight at the theme's own luminance-appropriate alpha
  (140 on a light backdrop, 26 on a dark one - same rule as `glass_highlight_h()`,
  section 2's "why the tint is 0.80" logic applied to whichever surface is behind),
  and the 3-band soft shadow already specified in `GLASS_MODALS_AND_POPOUTS.html`
  (offsets +2/+4/+6, radius +2/+4/+6, alpha 46/28/14). No real backdrop blur. This is
  the majority of the visual language (rounded depth, layered chrome, theme-correct
  accent) and needs **no kernel or libc change** - every primitive it calls
  (`gui_fill_rounded_grad`, `gui_soft_shadow`, `gui_rounded_border`) already exists
  and is already called by `draw_header()`/`draw_sidebar()` today, just arranged
  differently.
- **Tier 2, stretch, not required to ship the pilot: real blur-of-own-content.**
  Header and sidebar sample and box-blur the row of already-composed card content
  that scrolls underneath them, using the *identical* constants
  `glass_render()` (`userland/apps/compositor/draw.c`) already uses -
  `GLASS_DS=4`, `GLASS_RADIUS=3`, `GLASS_PASSES=3`, `GLASS_RECIP=9363` - promoted to
  a shared libc primitive so App Store does not fork a private copy (the standing
  "improve the shared one, do not fork" rule). This needs the app to hold its own
  off-screen ARGB copy of its content, which §11 below requires anyway for the
  anti-flash fix, so Tier 2 becomes a comparatively cheap follow-up once Tier 1's
  off-screen model ships - not a blocker for the pilot.

**Geometry stays exactly as measured**, nothing here moves a hit-test rect:
`HEADER_H` 58, `SIDEBAR_W` 186, `NAV_ROW_H` 34 / `NAV_HIT_H` 28 / `NAV_CAT_GAP` 8,
card corner radius 12 (`gui_fill_rounded_aa`, unchanged), `CARD_H` 132, `CARD_GAP`
16, grid columns computed as `content_w() / (CARD_W + CARD_GAP)` (2 columns at the
app's default 980x680 size, cards stretched to fill the column, not fixed at
`CARD_W`). Only how the surface *under* the existing header/sidebar/card content is
painted changes.

---

## 11. Anti-flash rendering requirement (hard requirement for any app using this language)

**The rule, stated once so it can be quoted:** a surface using this language
publishes a complete frame in one atomic step. A partially drawn frame must never
become visible, no matter how the redraw was triggered (a mouse move, a scroll, a
background fetch completing).

**The mechanism already exists; the bug is misusing it, not its absence.**
`kernel/proc/syscall.c`'s `uw_commit_content()` (`:7036`) is the atomic publish: one
`content_buffer -> content_presented` copy under a seqlock (`content_seq`), so a
concurrent compositor read always sees either the old complete frame or the new
complete one, never a mix. Per its own comment (`:7017-7024`), it is called **only**
by `sys_win_invalidate()`, `win16_host_invalidate()`, and the two syscalls that
**self**-invalidate, `sys_win_blit()` and `sys_win_draw_image()` - **never** by the
plain per-primitive draw syscalls (`draw_rect`/`draw_pixel`/`draw_text*`). That
design is correct: a burst of plain draws can accumulate in `content_buffer` with
nothing published, and exactly one commit at the end makes the whole burst atomic.

**MEASURED root cause of the App Store flash** (userland/apps/appstore/main.c):
`draw_card()` (`:2506`) calls `win_draw_image()` to paint a card's fetched
thumbnail; `draw_hero()` and `draw_detail()` have equivalent image draws for the
hero tile and the screenshot gallery. `draw_all()` (`:3160-3177`) clears the whole
window, draws content **first** (cards/hero, including these image calls), then
draws sidebar and header **last**, followed by a single `win_invalidate()`
(`:3217`). Because `win_draw_image()` self-invalidates, **every thumbnail paint
mid-sequence fires its own `uw_commit_content()` before the header and sidebar have
been redrawn into `content_buffer` for that frame** - publishing a real, complete,
but momentarily *wrong* frame (full-window fill, partial card content, stale or
absent header/sidebar chrome) that the compositor's independent draw thread can
sample and present. This is a real intermediate frame, not tearing, which is why it
reads as a flash rather than a garbled image, and why it settles once mouse movement
stops: the last redraw in a burst is the one whose thumbnail draws (if any) still
run mid-sequence, but by construction the header/sidebar redraw that follows always
completes and its own final `win_invalidate()` leaves the window correct - the flash
is the *transient* frames a fast burst produces, not a permanently wrong end state.

**Hard requirements for the port (App Store first, then Task Manager, then Browser
chrome):**

1. No self-invalidating primitive (`win_draw_image`, `win_blit`) may run in the
   middle of a multi-step repaint. Reordering draw calls does not fix this - it only
   relocates which chrome flashes - because the primitive itself is what commits.
2. **Recommended fix shape:** give the app its own off-screen ARGB buffer
   (`content_width x content_height`, owned by the app in its own memory, not the
   kernel's `content_buffer`), draw every element (header, sidebar, hero, cards,
   thumbnails composited by a plain memory write, not `win_draw_image`) into that
   local buffer with zero syscalls per element, then publish the **entire** frame
   with exactly **one** `win_blit()` (or one `win_draw_image()` covering the whole
   window) per repaint. One call = one `uw_commit_content()` = one atomic publish -
   architecturally the same buffer §10's Tier 2 blur needs, so building it once pays
   for both.
3. **Alternative/complementary fix, already proven in this tree:**
   `userland/apps/terminal/term_render.c`'s damage-diff model (its `#damage`
   section) keeps a shadow copy of what was last actually drawn (per cell there; per
   widget here) and repaints only the entries that differ, never a whole-header or
   whole-sidebar repaint for a hover that changed nothing. App Store's own
   `hover_signature()` (`:3570`) already computes "what is hovered right now"
   correctly and gates whether `draw_all()` runs at all on `EVENT_MOUSE_MOVE`
   (`:3795`) - the remaining gap is that `draw_all()` itself is still an
   all-or-nothing repaint once it does run. The two fixes compose: `hover_signature`
   keeps deciding **whether** to redraw; the off-screen-buffer-plus-single-blit
   decides **how** that redraw gets published, so a legitimate redraw can never
   flash either.
4. This is part of the design language documented here, not an App-Store-specific
   patch: it applies to Task Manager and Browser chrome when their turn comes, and
   to any future surface built in this language.

**Effort note (App Store pilot):** Tier-1 flat glass plus the off-screen-buffer/
single-blit anti-flash fix is a moderate, self-contained App Store change - no
kernel or libc change is required, since `win_blit()`/`win_draw_image()`/
`win_invalidate()` already exist and already behave as needed; the fix is entirely
in how `appstore/main.c` structures its own redraw. Real backdrop blur (Tier 2) is
additional and separable, and is not needed to resolve the reported flashing.
