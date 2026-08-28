// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_style.h - MayteraOS shared widget-style engine (Phase 0-2)
// A style-aware UI primitive layer used by all apps. A theme selects a
// renderer family (classic / modern / flat) plus a semantic palette; the
// primitives below render accordingly. See docs/UI_PRIMITIVES_UPLIFT_PLAN.md.
#ifndef _GUI_STYLE_H
#define _GUI_STYLE_H

#include "types.h"

// --- Renderer families -----------------------------------------------------
typedef enum {
    GUI_STYLE_CLASSIC = 0,  // CDE/Motif: beveled, square corners, no gradients
    GUI_STYLE_MODERN  = 1,  // rounded, gradient, soft elevation shadows
    GUI_STYLE_FLAT    = 2   // minimal: flat fill, thin border, no bevel/gradient
} gui_base_style_t;

// --- Button variants -------------------------------------------------------
typedef enum {
    GUI_BTN_PRIMARY   = 0,  // accent fill, contrast ink
    GUI_BTN_SECONDARY = 1,  // surface fill, ink
    GUI_BTN_GHOST     = 2,  // no fill, accent ink
    GUI_BTN_SUCCESS   = 3,  // #612: positive/update action. Fixed semantic
                            // green (like the existing OK/error colors used
                            // elsewhere), not palette-driven - added so
                            // callers with an "Update available" style action
                            // (previously a private capsule in appstore) can
                            // use the shared primitive instead of hand-rolling
                            // their own button.
    // (#appstyle) DESTRUCTIVE action. Same argument as GUI_BTN_SUCCESS above,
    // one step further: three apps had grown a private red button because the
    // engine offered no way to say "this one erases something". The installer's
    // "Erase and install" is the fourth, and the point at which a fourth
    // hand-rolled red stops being acceptable.
    //
    // WHY THIS ONE READS THE THEME AND SUCCESS DOES NOT. GUI_BTN_SUCCESS pins a
    // literal because a positive-action green is not a token any .mtheme
    // defines. Danger IS: every one of the 14 shipped themes carries
    // color_error, and they differ on purpose (0xcc0000 on retro_unix,
    // 0xD93A2F on maytera_dark, 0xff3b30 on modern_light). Pinning one here
    // would be a fourth private red wearing the engine's name, which is the
    // exact thing this variant exists to end.
    GUI_BTN_DANGER    = 4
} gui_btn_variant_t;

// --- Control states --------------------------------------------------------
typedef enum {
    GUI_ST_NORMAL   = 0,
    GUI_ST_HOVER    = 1,
    GUI_ST_PRESSED  = 2,
    GUI_ST_FOCUS    = 3,
    GUI_ST_DISABLED = 4
} gui_state_t;

// --- Design tokens ---------------------------------------------------------
// #612: buttons are square-edged house-wide (docs/UI_STYLE_GUIDE.md: 1990s
// UNIX/CDE/Motif - beveled, square, 1-2px highlight+shadow borders). The
// previous rounded "modern" capsule (radius = h/2) was the outlier, and its
// antialiased edge blended toward an ASSUMED background (p->surface) that is
// wrong wherever a button sits on anything else (a coloured card, a hero
// banner, a dialog panel) - there is no framebuffer readback, so the AA
// primitive cannot discover the real destination pixels; it can only be
// handed the truth by the caller. A square button needs no blend at all
// (gui_fill_rounded_aa's r<=0 path is an exact win_draw_rect), so this also
// permanently removes the fringe class of bug for every button, everywhere,
// with no per-caller bg bookkeeping required. See gui_button() in gui.c.
// #711: these six were compiled-in literals, which is why a corner radius or a
// padding value could not be changed without rebuilding every app that links
// libc. They now read radius.*/metric.*/type.* from the ACTIVE .mtheme file
// through SYS_THEME_METRIC, the same live kernel table SYS_THEME_COLOR reads.
// The _FALLBACK values are the pre-#711 literals and are what an older kernel
// (metric id unknown -> 0) or a theme file with no radius./metric./type. lines
// still yields, so nothing regresses.
#include "theme.h"
#define GUI_BTN_RADIUS_FALLBACK  0
#define GUI_RADIUS_FALLBACK      6
#define GUI_PAD_FALLBACK         10
#define GUI_GAP_FALLBACK         8
#define GUI_FOCUS_W_FALLBACK     2
#define GUI_TTF_SIZE_FALLBACK    14

// theme_metric() returns 0 for an id this kernel does not know; radius 0 is a
// LEGAL value, so radius uses theme_metric() directly (0 means square, which
// is what a retro theme wants) while the strictly-positive metrics use
// theme_metric_or() to keep their fallback.
#define GUI_BTN_RADIUS   theme_metric(THEME_METRIC_RADIUS_BTN)
#define GUI_RADIUS       theme_metric(THEME_METRIC_RADIUS_CARD)
#define GUI_PAD          theme_metric_or(THEME_METRIC_PAD,       GUI_PAD_FALLBACK)
#define GUI_GAP          theme_metric_or(THEME_METRIC_GAP,       GUI_GAP_FALLBACK)
// (#745) KNOWN GAP, DELIBERATELY NOT FIXED HERE. GUI_FOCUS_W has ZERO
// CONSUMERS: nothing in the tree reads it, so every focus ring is 1px
// regardless of what a theme asks for, and all 14 themes ask for 2. #745 fixed
// the ring's COLOUR (it was drawn from `accent` at 2.89:1); its WIDTH is still
// ignored. Wiring it is a real improvement and a real visual-weight change to
// every button in every app, so it wants its own screenshot pass rather than
// being smuggled in behind a contrast fix. Do not read this macro's existence
// as evidence that focus width is themeable: it is not, yet.
#define GUI_FOCUS_W      theme_metric_or(THEME_METRIC_FOCUS_W,   GUI_FOCUS_W_FALLBACK)
#define GUI_TTF_SIZE     theme_metric_or(THEME_METRIC_TYPE_BODY, GUI_TTF_SIZE_FALLBACK)

// --- Semantic palette (filled by the app from its active theme) ------------
typedef struct {
    uint32_t surface;        // window / content background
    uint32_t surface_raised; // card / panel background
    uint32_t ink;            // primary text
    uint32_t ink_dim;        // secondary text
    uint32_t accent;         // accent / selection
    uint32_t accent_hover;   // accent, hovered
    uint32_t border;         // outlines
    uint32_t field_bg;       // input background
    uint32_t field_border;   // input outline
    uint32_t track;          // slider/scrollbar track
    // (#745) ENGINE-OWNED. gui_set_palette() OVERWRITES both of these with a
    // derived, floor-guaranteed value and never reads what the caller left
    // here. That is deliberate and load-bearing: every app in the tree
    // declares its palette as a bare `gui_palette_t p;` on the stack and
    // assigns the ten fields above one by one, so on entry these two hold
    // STACK GARBAGE. A "0 means unset" sentinel would therefore have been an
    // uninitialised read in 22 apps. Deriving unconditionally makes the
    // half-migrated state inexpressible instead of merely discouraged.
    uint32_t focus;          // keyboard focus ring   (>= 3:1 vs surface(_raised))
    uint32_t edge_strong;    // boundary-carrying outline (>= 3:1 vs surface)
} gui_palette_t;

// --- Active style descriptor ----------------------------------------------
typedef struct {
    gui_base_style_t base;
    int  radius;       // corner radius for modern/flat (0 = square)
    bool gradients;
    bool shadows;
} ui_style_t;

// Style + palette management (one source of truth in libc).
void        gui_set_style(gui_base_style_t base);   // sensible defaults per family

// #711: pull radius/gradient/shadow from the ACTIVE theme FILE (decor.style +
// radius.card). Call it once at app start, or after a theme change, instead of
// hardcoding a family. gui_active_style() then reports what the file asked for.
void        gui_style_sync_from_theme(void);
ui_style_t  gui_active_style(void);
void        gui_set_palette(const gui_palette_t *p);
gui_palette_t *gui_pal(void);

// --- Foundation drawing helpers -------------------------------------------
uint32_t gui_mix(uint32_t a, uint32_t b, int t);     // blend a->b, t in 0..255
uint32_t gui_lighten(uint32_t c, int amt);
uint32_t gui_darken(uint32_t c, int amt);
uint32_t gui_ink_on(uint32_t bg);                    // black/white for contrast
int      gui_ttf_width(const char *s, int size);
int      gui_ttf_render_width(const char *s, int size);   // #B3: matches win_draw_text_ttf() exactly (no kerning) - see gui.c

// #204: general-purpose word-wrap + ellipsize helper for any app window.
// Greedy word-wrap up to `max_lines` lines that each fit `max_w` pixels at
// TTF size `size` (measured with gui_ttf_width - the real glyph metrics, a
// proportional TTF has no fixed char width), hard-breaking a single word
// wider than max_w so it can never leave the box, and ellipsizing (real-
// measured "...", not a fixed suffix count - the same convention notif.c/
// taskbar.c/widgets.c already use) whatever text remains once max_lines is
// reached. Mirrors the compositor's wrap_text_ttf() (notif.c, #762)
// algorithm exactly - duplicated, not shared source, only because the
// compositor and an app are different processes with different draw
// primitives (the accepted compositor/libc split this whole confirm
// component already uses - see confirmdialog.h). gui_confirm_open() below
// uses this so every app-window confirm (Files, Task Manager, ...) wraps
// for real instead of overflowing its card.
#define GUI_WRAP_COL 120
int      gui_wrap_text_ttf(const char *body, int size, int max_w, int max_lines,
                           char out[][GUI_WRAP_COL]);
void     gui_text_ttf_centered(int handle, int x, int y, int w, int h,
                               const char *s, uint32_t color, int size);
void     gui_fill_rounded(int handle, int x, int y, int w, int h, int r, uint32_t color);
void     gui_fill_rounded_grad(int handle, int x, int y, int w, int h, int r,
                               uint32_t top, uint32_t bottom);
void     gui_rounded_border(int handle, int x, int y, int w, int h, int r, uint32_t color);

// --- (#745) Contrast primitives. ONE definition, shared, never hand-rolled --
// WCAG 2.1 relative luminance and contrast, in INTEGER arithmetic only: the
// kernel target is soft-float with SSE disabled and libc must stay linkable
// into anything, so the sRGB transfer curve is a 256-entry table rather than
// a powf(). gui_luma_wcag() returns 0..65535.
//
// FLOORS. These are the two WCAG thresholds and they are NOT interchangeable:
//   GUI_FLOOR_NONTEXT (3.0:1, WCAG 1.4.11 Non-text Contrast) applies to a
//     boundary: a control outline, a focus ring, the extent of a track. Using
//     the 4.5:1 text floor on a hairline is over-correction and produces a
//     garish, high-key UI; it is not "safer".
//   GUI_FLOOR_TEXT (4.5:1, WCAG 1.4.3) applies to glyphs only.
#define GUI_FLOOR_NONTEXT  300
#define GUI_FLOOR_TEXT     450
//
// AIM vs FLOOR (#745, local queue item 77). A FLOOR is what the pixels must
// clear. An AIM is what you ASK gui_ensure_contrast() for in order to clear it
// with margin, and the two are not the same number, because that function walks
// the mix in 8/255 steps and returns the FIRST step that clears what it was
// given. Ask for exactly the floor and you land ON the floor, in [floor,
// floor+one step), with nothing left for a theme edit, a rounding difference,
// or a background that turns out not to be the one you measured against. The
// About/Credits pass hit this first and asked 5.00 for the 4.50 text floor.
// GUI_AIM_NONTEXT is the same idea for the 3:1 non-text floor: +10%. MEASURED,
// with tools/contrast/scrollbar-contrast.sh over the 14 shipped themes, on the
// scrollbar thumb: asking for exactly 3.00 lands the thumb at 3.00 to 3.20, and
// four themes (dark 3.00, maytera_dark 3.01, maytera_light 3.01, light 3.02)
// land within 0.02 of the floor, which is the no-headroom state this comment is
// about. Asking 3.30 lands them at 3.30 to 3.56, so the aim buys 0.30 to 0.56.
// One 8/255 mix step is worth 0.29 to 0.38 in that range, so the headroom is
// about one step: enough that a re-measure cannot land under the floor, without
// over-correcting into a garish thumb that is no longer the theme's colour.
#define GUI_AIM_NONTEXT    330
// A hover/drag state has to clear the floor AND still read as a change. Both
// states get repaired toward the same corner of the colour space, so on a theme
// whose two thumb tokens are close (retro_unix ships 0xc8c8c8 and 0xd4d4d4, one
// 8/255 step apart) repairing each independently can land them on the SAME
// colour and silently delete the hover affordance. GUI_HOVER_STEP is the extra
// contrast the hover state is required to gain over the repaired rest state, so
// the direction of the change survives the repair.
#define GUI_HOVER_STEP      40
// And the acceptance test for that: the hover thumb must differ from the rest
// thumb by at least this ratio or it is not a state change at all. 1.10:1 is a
// visible shade step on a solid 10px block, and it sits below what aiming
// +GUI_HOVER_STEP actually produces (measured 1.12 to 1.20 on the 13 themes
// where that aim is reachable), so the aim is what normally happens and this is
// the acceptance test underneath it. high_contrast is the one theme that cannot
// reach it, by construction: its rest thumb is pure white on pure black, 20.99:1,
// the maximum any colour can score, so every possible hover colour is LOWER
// contrast. There the fallback keeps the theme's authored yellow (1.07:1 against
// white), and the hover cue is the hue change, which a luminance-only metric
// cannot score and this one does not pretend to.
#define GUI_HOVER_MIN      110
uint32_t gui_luma_wcag(uint32_t rgb);
int      gui_contrast_x100(uint32_t a, uint32_t b);
// Return `fg` unchanged if it already clears `min_x100` against `bg`; else the
// nearest colour along the toward-white or toward-black axis (hue preserved as
// far as a linear mix allows) that does. Always terminates: on any background
// at least one of pure white / pure black reaches 4.58:1.
uint32_t gui_ensure_contrast(uint32_t fg, uint32_t bg, int min_x100);
// Same, but the result must clear the floor against BOTH backgrounds (a focus
// ring can land on `surface` or on a card's `surface_raised`). Best-effort if
// the two backgrounds are so far apart that no colour can satisfy both.
uint32_t gui_ensure_contrast2(uint32_t fg, uint32_t bg1, uint32_t bg2, int min_x100);
// (#117) Two-tone bevel pair (shadow/highlight) for a sunken or raised edge:
// walks `base` independently toward black and white until each side clears
// GUI_AIM_NONTEXT against `base` itself, so a bevel's two sides land on
// opposite, forced directions instead of gui_ensure_contrast()'s single
// caller-agnostic pick. Lifted out of gui_scroll_trough_bevel() (#96, the
// scrollbar trough), which is now a one-line wrapper around this; #117 moved
// gui_checkbox(), gui_textfield2(), gui_card() and gui_button()'s CLASSIC
// bevels onto it too, replacing four copies of a fixed darken/lighten
// magnitude that measured below the 3:1 non-text floor on shipped themes.
// See gui.c for the full rationale and gui_scroll.h for the original.
void gui_bevel_pair(uint32_t base, uint32_t *shadow_out, uint32_t *highlight_out);
void     gui_soft_shadow(int handle, int x, int y, int w, int h, int r, uint32_t bg);
// Antialiased rounded fill / circle: edge pixels blend the fill color toward the
// caller-supplied background (no framebuffer read-back). r==0 falls back to a rect.
void     gui_fill_rounded_aa(int handle, int x, int y, int w, int h, int r,
                             uint32_t color, uint32_t bg);
void     gui_fill_circle_aa(int handle, int x, int y, int d, uint32_t color, uint32_t bg);
// Star rating icon (#B2): one star in a d x d box, fill_pct (0..100) of its
// width painted fill_color, the rest empty_color, blended toward bg at the
// polygon edge. See gui.c for the rationale (shared so no app hand-rolls a
// star shape for its own rating UI).
void     gui_fill_star_aa(int handle, int x, int y, int d, int fill_pct,
                          uint32_t fill_color, uint32_t empty_color, uint32_t bg);
// #306: arbitrary-angle line stroke (Bresenham), promoted out of gui.c's
// private gs_line() so any app can draw a non-axis-aligned mark (a checkmark,
// an X, a diagonal divider) without hand-rolling its own line rasterizer.
// gui_thick_line() bundles parallel copies for a small approximate thickness
// that holds up at any angle (see gui.c for why a single-axis offset does not).
void     gui_line(int handle, int x0, int y0, int x1, int y1, uint32_t col);

// --- (#307 follow-up) THE small filled chevron. ONE definition, shared. ------
// A 7px base x 4px deep solid triangle: the OS's mark for "there is more this
// way" (dropdown affordance, scroll cue, collapse/expand control).
//
// WHY IT MOVED INTO libc. It was a private static PAIR in
// apps/settings/main.c, and the pair had a real bug that a second reader would
// never look for: draw_chevron_up() and draw_chevron_down() emitted the
// IDENTICAL pixel set. down() walked r=0..3 laying rows of width 7,5,3,1 at
// y=cy+r; up() walked the same rows in the reverse ORDER (w=1+2r at y=cy+3-r)
// and so painted {cy:7, cy+1:5, cy+2:3, cy+3:1} - the same map, apex down. The
// loop looked mirrored; the output was not. Settings' "more items above"
// scroll cue has therefore been drawing a DOWN arrow. Proven by evaluating
// both loops, not by reading them.
//
// (cx, cy) is the CENTRE of the mark, not its top-left. The old private
// version anchored on the top row, which is why its call sites carried
// unexplained +11 / +4 fudges; anchoring on the centre means a caller centres
// a chevron in a box by handing it the box's centre and nothing else.
#define GUI_CHEV_UP     0
#define GUI_CHEV_DOWN   1
#define GUI_CHEV_LEFT   2
#define GUI_CHEV_RIGHT  3
void     gui_chevron(int handle, int cx, int cy, int dir, uint32_t col);
void     gui_thick_line(int handle, int x0, int y0, int x1, int y1,
                        int thickness, uint32_t col);

// --- Style-aware primitives (4 states each) -------------------------------
void gui_button(int handle, int x, int y, int w, int h, const char *label,
                gui_btn_variant_t variant, gui_state_t st);
void gui_checkbox(int handle, int x, int y, int sz, bool checked,
                  const char *label, gui_state_t st);
void gui_toggle(int handle, int x, int y, int w, int h, bool on, gui_state_t st);
void gui_slider(int handle, int x, int y, int w, int value, int max_val, gui_state_t st);
void gui_textfield2(int handle, int x, int y, int w, int h, const char *text, bool focused);
// (#appstyle) The TTF half of the above, driven by a textfield_t so a field
// gets its caret, its selection highlight and its placeholder from ONE call.
//
// WHY IT HAD TO EXIST. textfield.h already ships gui_draw_textfield_tf(), and
// its own header comment says so plainly: it is the MONOSPACE case, it draws
// with win_draw_text() and steps the caret by a constant FONT_WIDTH, and
// "apps with their own (e.g. TrueType) layout draw the selection themselves".
// Every app that wanted a themed TTF field therefore had to hand-roll caret
// placement from gui_ttf_*_width() - which the installer did, badly (no
// selection rendering at all, and a caret measured with gui_ttf_width() while
// the text beside it was drawn by win_draw_text_ttf(), which uses no kerning,
// so caret and glyphs drifted apart on any string with a kerned pair).
//
// MEASURE WITH gui_ttf_render_width(), NOT gui_ttf_width(). The chrome and the
// text below come from gui_textfield2()'s own path (win_draw_text_ttf), and
// gui_ttf_render_width() is documented as the function that matches it exactly.
// This is the one place where picking the "obvious" width function puts the
// caret in the wrong place on some strings and the right place on others.
//
// `placeholder` is drawn in a dimmed ink when the field is empty, and is
// ignored when it is not. Pass NULL for none.
//
// PRIMITIVES ONLY, deliberately, for the same reason gui_confirm_open_s()
// below takes primitives: textfield_t is an ANONYMOUS typedef'd struct, so it
// cannot be forward-declared, and pulling textfield.h into gui_style.h would
// put a ~2.7 KiB header-only widget (with its own undo history) into the
// include graph of every app that wants a button. Callers pass
// tf->buf, tf->len, tf->cursor, tf->sel_anchor. `sel_anchor` < 0 means no
// selection, matching textfield.h's own convention exactly.
void gui_textfield_tf(int handle, int x, int y, int w, int h,
                      const char *text, int len, int cursor, int sel_anchor,
                      bool focused, const char *placeholder);
void gui_progress(int handle, int x, int y, int w, int h, int pct);
void gui_card(int handle, int x, int y, int w, int h);

// --- (docs/CONFIRM_MODAL_DESIGN.html, 86f3cea) Shared window-modal confirm/
// notice card -----------------------------------------------------------
// The app-window half of the same component confirmdialog.c is on the
// compositor side (system-modal: draws straight to the framebuffer). This
// half is for any app that owns a window but cannot grab the whole screen -
// Files' Recycle Bin, Task Manager's End Task/Kill, and any future Settings/
// App Store/Studio confirm. Same geometry, same token names, same three
// variants and same two safety properties (real keyboard focus so Enter
// never unconditionally fires the destructive action; a settle timer so a
// buffered keystroke cannot land on open) - see confirmdialog.h for the full
// rationale, which applies here unchanged.
typedef enum {
    GUI_CONFIRM_DESTRUCTIVE = 0,   // Cancel=standard, Action=danger. Initial focus: Cancel.
    GUI_CONFIRM_NEUTRAL     = 1,   // Cancel=standard, Action=primary. Initial focus: Action.
    GUI_CONFIRM_NOTICE      = 2    // Single "OK"=primary, no Cancel. Initial focus: OK.
} gui_confirm_variant_t;

#define GUI_CONFIRM_MAX_LINES 3
// #204: must equal GUI_WRAP_COL above - this is gui_wrap_text_ttf()'s output
// buffer stride, not an independent number, so there is nothing to drift.
#define GUI_CONFIRM_LINE_MAX  GUI_WRAP_COL

typedef struct {
    int open;
    gui_confirm_variant_t variant;
    char title[64];
    char lines[GUI_CONFIRM_MAX_LINES][GUI_CONFIRM_LINE_MAX];
    int  n_lines;
    char cancel_label[24];
    char action_label[24];
    int  focus;
    unsigned long long shown_ms;
} gui_confirm_t;

// #204 CORRECTED: this used to say body copy is "pre-wrapped by the caller
// ... same author-discipline rule as the compositor-side confirmdialog.h" -
// that was never actually true (see confirmdialog.h's header comment for
// the measured overflow at all 7 real call sites). gui_confirm_open() now
// wraps `lines`/`n_lines` for real via gui_wrap_text_ttf() above. Callers
// may keep passing one whole sentence as `lines[0]`/`n_lines=1` - that is
// now the CORRECT usage, not the bug.
void gui_confirm_open(gui_confirm_t *c, gui_confirm_variant_t variant,
                      const char *title, const char *const *lines, int n_lines,
                      const char *cancel_label, const char *action_label);
void gui_confirm_close(gui_confirm_t *c);
static inline int gui_confirm_is_open(const gui_confirm_t *c) { return c->open; }
// Draws the interlaced-scanline scrim confined to (0,0)-(win_w,win_h) - i.e.
// the app's own window client area only, never system-wide - then the card,
// screen-centered within that same rect. Call every time the app redraws
// while c->open.
void gui_confirm_render(int handle, const gui_confirm_t *c, int win_w, int win_h);
// Returns 0 = no decision, 1 = Cancel/dismiss, 2 = Action. Callers must still
// swallow every key/click while c->open regardless of this return value -
// see confirmdialog.h's identical note. win_w/win_h must be the SAME values
// passed to gui_confirm_render() - the card is screen-centered within them,
// so a hit-test without them would be guessing the geometry it is testing.
int  gui_confirm_handle_key(gui_confirm_t *c, int key);
int  gui_confirm_handle_mouse(gui_confirm_t *c, int32_t x, int32_t y, int clicked,
                              int win_w, int win_h);

// --- FFI-safe singleton wrapper ------------------------------------------
// A Rust caller (task #487 Task Manager) would otherwise have to mirror
// gui_confirm_t's exact C layout - enum width, char-array padding, the
// alignment gap gcc inserts before the trailing uint64_t - across the FFI
// boundary by hand, which is exactly the "guessed layout, no diagnostic
// until it corrupts something" class of bug already recorded in blame.md
// for this codebase's Rust userland port. Every function below takes only
// primitives (int/char*/int32_t), the same shape as the gui_card()/
// gui_button()/gui_set_palette() calls Task Manager already makes, so no new
// struct needs to cross the boundary at all. One dialog open at a time is
// already the only case any caller needs (Files, Task Manager alike).
void gui_confirm_open_s(int variant, const char *title,
                        const char *line0, const char *line1, const char *line2, int n_lines,
                        const char *cancel_label, const char *action_label);
int  gui_confirm_singleton_is_open(void);
void gui_confirm_singleton_render(int handle, int win_w, int win_h);
int  gui_confirm_singleton_handle_key(int key);
int  gui_confirm_singleton_handle_mouse(int32_t x, int32_t y, int clicked);

#endif // _GUI_STYLE_H
