// help_ui.h - GUI help primitives for MayteraOS apps (task #267).
//
// Self-contained helpers an app drives from its own event loop:
//   * Hover tooltips: register help text for screen-relative rects, then call
//     help_ui_tick() with the mouse position each frame and help_ui_draw() to
//     paint a styled tooltip after a ~600 ms hover.
//   * A "?" question-mark icon widget with hit testing, so an app can open
//     context help when it is clicked.
//   * A Help-menu helper and an F1 helper that launch the help viewer app on a
//     given topic.
//
// These build on libc gui.h / gui_style.h and the libhelp doc model. No kernel
// changes are required: timing uses uptime_ms() from libc.
//
// THREE-LINE ADOPTION PATTERN for any app (documented in detail in the help
// app README at apps/help):
//   1) #include "../../libhelp/help_ui.h"     // (adjust relative path)
//   2) help_ui_register(win, x, y, w, h, "tooltip text");  // per widget, once
//   3) in your event loop:
//        on mouse move:  help_ui_tick(ev.mouse_x - win_origin_x,
//                                     ev.mouse_y - win_origin_y, uptime_ms());
//        each redraw:    help_ui_draw(win);
//        on F1 keydown:  help_ui_open_topic("/HELP/SYSTEM.MHLP", "desktop");
//        "?" icon:       if (help_ui_question_hit(...)) help_ui_open_topic(...);

#ifndef _LIBHELP_UI_H
#define _LIBHELP_UI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Default F1 keycode delivered in gui_event_t.keycode for MayteraOS windows.
// The kernel keyboard driver maps F1 (scancode 0x3B) to KEY_F1 == 0x3B. Apps
// may pass their own keycode to help_ui_is_f1() if this ever changes.
#define HELP_UI_KEY_F1 0x3B

// Hover delay before a tooltip appears, in milliseconds.
#define HELP_UI_HOVER_MS 600

// Maximum tooltip registrations per app (kept small and static; no malloc).
#define HELP_UI_MAX_TIPS 64

// ---------------------------------------------------------------------------
// Tooltip registration. Coordinates are WINDOW-RELATIVE (same space you draw
// in). Calling register with the same rect twice updates the text. Returns the
// slot index or -1 if full. Re-registering the SAME (x,y,w,h) replaces text.
// ---------------------------------------------------------------------------
int  help_ui_register(int win, int x, int y, int w, int h, const char *text);

// Remove all registered tooltips (call when your layout changes wholesale).
void help_ui_clear(void);

// Feed the current mouse position (window-relative) and a monotonic timestamp
// in ms (use uptime_ms()). Tracks hover dwell time internally. Cheap; call it
// on every mouse-move event and once per frame.
void help_ui_tick(int mouse_x, int mouse_y, unsigned long now_ms);

// Draw the active tooltip (if any) into the window. Call at the END of your
// redraw so it paints on top. No-op when no tooltip is due.
void help_ui_draw(int win);

// ---------------------------------------------------------------------------
// "?" question-mark help icon. Draws a small round beveled icon at (x,y) with
// the given diameter. help_ui_question_hit() returns true if (mx,my) is inside
// the icon, so the caller can open context help on a click.
// ---------------------------------------------------------------------------
void help_ui_question_icon(int win, int x, int y, int diameter);
bool help_ui_question_hit(int x, int y, int diameter, int mx, int my);

// ---------------------------------------------------------------------------
// Help-menu / F1 helpers.
// ---------------------------------------------------------------------------

// Returns true if a key event keycode is F1 (uses HELP_UI_KEY_F1).
bool help_ui_is_f1(unsigned int keycode);

// Launch the help viewer app (/APPS/HELP) with the given help file and, if non-
// NULL, an initial topic id. Non-blocking; returns 0 on spawn, <0 on error.
// If help_file is NULL it defaults to the system help "/HELP/SYSTEM.MHLP".
int  help_ui_open_topic(const char *help_file, const char *topic_id);

// Convenience: open the system help at its home topic.
int  help_ui_open_system(void);

#ifdef __cplusplus
}
#endif

#endif // _LIBHELP_UI_H
