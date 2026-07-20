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

#include "types.h"

// Width of the scrollbar gutter, in pixels. Matches the Files/Settings design
// language (a 14px gutter with a 10px thumb inset 2px each side).
#define GUI_SCROLL_W       14
#define GUI_SCROLL_MIN_TH  24   // smallest the thumb is allowed to get

// Keycodes as delivered in gui_event_t.keycode (scancode-derived; see the
// keyboard driver). These were previously open-coded as magic numbers in every
// app that handled them.
#define GUI_KEY_HOME   0x47
#define GUI_KEY_PGUP   0x49
#define GUI_KEY_END    0x4F
#define GUI_KEY_PGDN   0x51
#define GUI_KEY_UP     0x80
#define GUI_KEY_DOWN   0x81
#define GUI_KEY_LEFT   0x82
#define GUI_KEY_RIGHT  0x83

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
// Colors come from theme_color(THEME_COLOR_SCROLLBAR_*) at draw time, so a new
// theme needs no change here and both light and dark themes are correct.
void gui_scroll_draw(int handle, const gui_scroll_t *s);

// X of the scrollbar gutter (right-aligned inside the viewport). Useful when the
// app needs to keep its content clear of the gutter.
int  gui_scroll_bar_x(const gui_scroll_t *s);

#endif // _GUI_SCROLL_H
