// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_scroll.h - MayteraOS shared scrollable-viewport primitive
//
// WHY THIS EXISTS (#291 / #261 / #438 / #386):
// The compositor has routed EVENT_MOUSE_SCROLL OS-wide since #291, but there was
// no shared scrollable-list widget, so every app hand-rolled its own list state,
// its own scrollbar geometry, and its own wheel handling. The results diverged:
//   * Files scrolled the wheel BACKWARDS relative to every other app (it treated
//     a negative delta as "up"; the OS convention, set by the kernel terminal, is
//     positive = up / back toward the content start).
//   * Settings' sidebar had no scroll at all, so a window too short to show all
//     17 panels simply clipped them with no way to reach the rest.
//   * Several lists implement the wheel but not the keyboard, which makes them
//     unusable wherever the pointer's wheel is unavailable (the Apple Magic
//     Mouse on the iMac14,4 target, #438) or where the mouse is dead entirely.
// One model, one convention, one look. Adopt this instead of adding a copy.
//
// CONVENTION: a POSITIVE scroll_delta scrolls UP (toward the start of the
// content). This matches gui/terminal.c ("Positive scroll_delta = scroll up")
// and the kernel's own wheel documentation.
//
// MODEL: the viewport is a pixel window onto a taller content. Item lists set
// content_px = item_count * row_h and step_px = row_h; the first visible row is
// then gui_scroll_first_item(). Pixel-scrolled panels use content_px directly.
//
// The app owns layout and drawing of the CONTENT. This primitive owns the scroll
// offset, the clamping, the scrollbar geometry, the input handling, and the
// scrollbar's themed appearance.
#ifndef _GUI_SCROLL_H
#define _GUI_SCROLL_H

// The compositor cannot include libc's types.h: compositor.h carries its own
// `typedef int bool;` and types.h typedefs bool to _Bool, which is a hard
// conflict. It still needs the scrollbar CONTRAST RULE declared at the bottom
// of this header, and a second declaration of that rule living in the
// compositor is exactly the divergence this header exists to prevent. Defining
// GUI_SCROLL_STDINT_ONLY before including takes the stdint route instead, so
// the tree still holds exactly ONE declaration of these functions.
#ifdef GUI_SCROLL_STDINT_ONLY
#include <stdint.h>
#else
#include "types.h"
#endif

// Width of the scrollbar gutter, in pixels. Matches the Files/Settings design
// language (a 14px gutter with a 10px thumb inset 2px each side).
#define GUI_SCROLL_W       14
#define GUI_SCROLL_MIN_TH  24   // smallest the thumb is allowed to get

// Keycodes as delivered in gui_event_t.keycode. These USED to be declared
// right here, which made a scrollbar header the de-facto owner of the whole
// keyboard table, and every app that did not include it open-coded its own
// copy instead. Six of those copies had the ARROWS wrong (#188, #191). The
// table now lives in ONE place and this header just re-exports it, so the
// GUI_KEY_* spelling every existing caller uses is unchanged.
#include "keys.h"

typedef struct {
    // --- Viewport rect, window-local. Set via gui_scroll_config() each layout.
    int x, y, w, h;

    int content_px;   // total content height in px
    int step_px;      // wheel / arrow-key step (typically the row height)
    int offset;       // current scroll offset in px, always in [0, max]

    // Set to 1 for a list of fixed-height rows: every offset then snaps to a
    // whole multiple of step_px (the end stop excepted), so a row is never left
    // half-drawn across the top edge of the viewport. Leave 0 for pixel-scrolled
    // content (documents, images, wrapped text). Set it once after the first
    // gui_scroll_config(); config() does not clear it.
    int snap;

    // --- Internal input state; do not poke directly.
    int drag;         // 1 while the thumb is being dragged
    int drag_grab;    // pointer offset within the thumb when the drag began
    int hover;        // 1 when the pointer is over the thumb
} gui_scroll_t;

// Configure the viewport rect and content extent. Safe to call every draw; the
// current offset is re-clamped so a shrinking content never strands the view.
void gui_scroll_config(gui_scroll_t *s, int x, int y, int w, int h,
                       int content_px, int step_px);

// Largest legal offset (0 when everything fits).
int  gui_scroll_max(const gui_scroll_t *s);
// 1 when the content overflows the viewport (i.e. a scrollbar is warranted).
int  gui_scroll_needed(const gui_scroll_t *s);

// Absolute / relative movement. Both clamp. Return 1 if the offset changed.
int  gui_scroll_set(gui_scroll_t *s, int offset);
int  gui_scroll_by(gui_scroll_t *s, int delta_px);

// --- Input -----------------------------------------------------------------
// Wheel. Pass gui_event_t.scroll_delta straight in. Positive = up. Returns 1 if
// the offset changed (i.e. the app should redraw).
int  gui_scroll_wheel(gui_scroll_t *s, int scroll_delta);

// Keyboard. Pass gui_event_t.keycode. Handles Up/Down/PageUp/PageDown/Home/End.
// Returns 1 if the key was consumed AND the offset changed. A key this does not
// own returns 0, so the caller can fall through to its own handling.
int  gui_scroll_key(gui_scroll_t *s, uint32_t keycode);

// Scrollbar hit-testing and drag. Coordinates are window-local, matching what
// user_window_event_handler delivers to the app.
// gui_scroll_press:   press anywhere in the gutter. Grabs the thumb if hit,
//                     otherwise pages toward the click. Returns 1 if consumed.
// gui_scroll_motion:  call on every EVENT_MOUSE_MOVE. Returns 1 if the offset
//                     changed. Cheap no-op unless a drag is in progress.
// gui_scroll_release: call on EVENT_MOUSE_UP.
int  gui_scroll_press(gui_scroll_t *s, int mx, int my);
int  gui_scroll_motion(gui_scroll_t *s, int mx, int my);
void gui_scroll_release(gui_scroll_t *s);

// --- Item-list helpers -----------------------------------------------------
// For the common "list of fixed-height rows" case.
int  gui_scroll_first_item(const gui_scroll_t *s);   // index of first visible row
int  gui_scroll_visible_items(const gui_scroll_t *s);// rows that fit (partial counted)
// Scroll the minimum distance needed to bring [top_px, top_px+h_px) into view.
// Use this to keep a keyboard-selected row on screen. Returns 1 if changed.
int  gui_scroll_reveal(gui_scroll_t *s, int top_px, int h_px);

// --- Drawing ---------------------------------------------------------------
// Draw the themed scrollbar in the gutter at the right edge of the viewport.
// Draws nothing when the content fits, so the gutter is only spent when needed.
// Colors come from theme_color(THEME_COLOR_SCROLLBAR_*) at draw time AND are
// raised to the 3:1 non-text contrast floor against both the trough and the
// surface, because 13 of the 14 shipped themes author a thumb below it (#745,
// local queue item 77; see gui_scroll.c for the measurement and the rule).
// This form assumes the gutter sits on THEME_COLOR_WINDOW_BG.
void gui_scroll_draw(int handle, const gui_scroll_t *s);
// Same, for a gutter on any other surface: menu_bg in a popup, textbox_bg in a
// field-style list, taskbar_bg in the Settings left nav. Pass the colour the
// caller actually painted behind the gutter; the repair needs it, and only the
// caller knows it.
void gui_scroll_draw_on(int handle, const gui_scroll_t *s, uint32_t surface);

// --- The contrast rule, for apps that draw their own gutter ----------------
// Several apps predate this widget and own their scrollbar geometry. They must
// NOT restate the rule below; they call these, so there is one definition.
// gui_scroll_colors(): the themed (track, thumb) pair for a gutter on `surface`,
//   already repaired. `hot` selects the hover/drag thumb.
// gui_scroll_thumb_ink(): the rule alone, for a drawer whose track and thumb
//   come from tokens other than THEME_COLOR_SCROLLBAR_* (the compositor's start
//   menu paints its gutter from the menu palette).
// gui_scroll_hover_ink(): the hover thumb, guaranteed to read as a change
//   against the already-repaired rest ink rather than collapsing onto it.
void     gui_scroll_colors(int hot, uint32_t surface,
                           uint32_t *track_out, uint32_t *thumb_out);
uint32_t gui_scroll_thumb_ink(uint32_t thumb, uint32_t track, uint32_t surface);
uint32_t gui_scroll_hover_ink(uint32_t hover, uint32_t rest_ink,
                              uint32_t track, uint32_t surface);

// (#96) The TROUGH's own boundary. #745 floored the thumb against both the
// trough and the surface and explicitly left the trough's fill unrepaired,
// reasoning that an invisible trough is harmless once the thumb reads on its
// own. Measured with tools/contrast/scrollbar-contrast.sh, the trough fill is
// 1.00-1.42:1 against its surface on ALL 14 shipped themes (retro_unix,
// classic, fluent_dark and high_contrast are exactly 1.00:1: literally the same
// colour), so a user who is not looking directly at the thumb has no way to
// tell a region is scrollable at all, which is a real, reported affordance gap
// independent of thumb legibility (#96).
// docs/UI_STYLE_GUIDE.md 6.2 calls the retro-unix trough a "Sunken 3D
// appearance", the same shadow-top/left + highlight-bottom/right convention
// gui_checkbox()/gui_textfield2() use for their own sunken wells in gui.c.
// Each side is walked independently toward black/white until it clears
// GUI_AIM_NONTEXT against `surface`. Where one direction cannot clear the
// floor at all (only possible when `surface` is already at that extreme,
// e.g. high_contrast's pure-black window_bg), BOTH sides fall back to the
// other direction, which degrades to a uniform border -- exactly the
// "Borders: White 2px" docs/UI_STYLE_GUIDE.md section 9 already specifies
// for High Contrast mode, not a coincidence.
// (#117) At the time this comment was written, gui_checkbox()/gui_textfield2()
// did NOT share this repair: they drew their sunken well with a fixed
// darken(70)/lighten(80) step regardless of `surface`, measured 1.82:1 and
// 1.27:1 respectively against retro_unix's window_bg, below GUI_FLOOR_NONTEXT.
// #117 moved the walk itself into the general-purpose gui_bevel_pair()
// (gui.c/gui_style.h) and put THIS function, gui_checkbox(), gui_textfield2()
// and gui_card()/gui_button()'s CLASSIC bevels all on top of it, so there is
// now one walk instead of four fixed-magnitude copies. This function's own
// signature and behaviour are unchanged; see gui_scroll.c and gui.c.
void gui_scroll_trough_bevel(uint32_t surface, uint32_t *shadow_out, uint32_t *highlight_out);

// (#117) The trough BOUNDARY draw, not just its colours: draws the same
// CLASSIC two-tone bevel / MODERN-FLAT single ring gui_scroll_draw_on() draws
// around its own trough, at the caller's OWN geometry (x,y,w,h). For the apps
// that own their scrollbar geometry and call gui_scroll_colors() for colour
// only (Files' two lists, the browser page bar, the recycle bin): before
// this they drew colour with no edge at all, so their troughs stayed
// borderless even after #96 fixed the shared widget's. `track` is the
// trough's own fill (from gui_scroll_colors()); `surface` is what the gutter
// sits on, same meaning as gui_scroll_colors()'s `surface` argument.
void gui_scroll_trough_border(int handle, int x, int y, int w, int h,
                              uint32_t track, uint32_t surface);

// X of the scrollbar gutter (right-aligned inside the viewport). Useful when the
// app needs to keep its content clear of the gutter.
int  gui_scroll_bar_x(const gui_scroll_t *s);

#endif // _GUI_SCROLL_H
