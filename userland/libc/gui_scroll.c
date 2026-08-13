// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_scroll.c - MayteraOS shared scrollable-viewport primitive.
// See gui_scroll.h for the rationale and the wheel-direction convention.
#include "gui_scroll.h"
#include "syscall.h"   // win_draw_rect
#include "theme.h"     // theme_color / THEME_COLOR_SCROLLBAR_*
#include "gui_style.h" // GUI_FLOOR_NONTEXT / GUI_AIM_NONTEXT, gui_ensure_contrast2
// NOTE: deliberately does NOT include gui.h. gui.h and theme.h both define
// BTN_COLOR_* / DISPLAY_BG (gui.h as literals, theme.h as theme lookups), so
// including both produces macro-redefinition warnings. Everything needed here
// is in syscall.h (win_draw_rect) and theme.h (theme_color). gui_style.h is the
// contrast layer on its own (it declares gui_ensure_contrast*() and defines the
// floors) and pulls in only theme.h, so it can be included here without
// dragging gui.h's literal palette in with it.

// --- small local helpers ---------------------------------------------------
static int clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Thumb geometry for the current state. Returns 0 when no scrollbar is needed
// (in which case *ty/*th are untouched).
static int thumb_geom(const gui_scroll_t *s, int *ty, int *th) {
    int max = gui_scroll_max(s);
    if (max <= 0 || s->h <= 0) return 0;

    // Thumb is sized by the visible fraction of the content, floored so it stays
    // grabbable on very long lists.
    int t = (int)(((long)s->h * (long)s->h) / (long)s->content_px);
    if (t < GUI_SCROLL_MIN_TH) t = GUI_SCROLL_MIN_TH;
    if (t > s->h) t = s->h;

    int travel = s->h - t;
    int off = clamp_i(s->offset, 0, max);
    *ty = s->y + (travel > 0 ? (int)(((long)travel * (long)off) / (long)max) : 0);
    *th = t;
    return 1;
}

// --- configuration ---------------------------------------------------------
void gui_scroll_config(gui_scroll_t *s, int x, int y, int w, int h,
                       int content_px, int step_px) {
    if (!s) return;
    s->x = x; s->y = y; s->w = w; s->h = h;
    s->content_px = content_px < 0 ? 0 : content_px;
    s->step_px = step_px > 0 ? step_px : 16;
    // Re-clamp: the content may have shrunk (a filtered list, a resized window)
    // and an offset past the new end would strand the view on empty space.
    s->offset = clamp_i(s->offset, 0, gui_scroll_max(s));
}

int gui_scroll_max(const gui_scroll_t *s) {
    if (!s) return 0;
    int m = s->content_px - s->h;
    return m > 0 ? m : 0;
}

int gui_scroll_needed(const gui_scroll_t *s) {
    return gui_scroll_max(s) > 0;
}

int gui_scroll_bar_x(const gui_scroll_t *s) {
    return s ? (s->x + s->w - GUI_SCROLL_W) : 0;
}

// --- movement --------------------------------------------------------------
int gui_scroll_set(gui_scroll_t *s, int offset) {
    if (!s) return 0;
    int max = gui_scroll_max(s);
    int n = clamp_i(offset, 0, max);
    if (s->snap && s->step_px > 0) {
        // Round to the NEAREST row so a thumb drag settles on the row the user
        // is closest to rather than always rounding down. The end stop is exempt:
        // when max is not a whole number of rows, snapping up would put the last
        // row permanently out of reach.
        int snapped = ((n + s->step_px / 2) / s->step_px) * s->step_px;
        if (snapped > max) snapped = max;
        n = snapped;
    }
    if (n == s->offset) return 0;
    s->offset = n;
    return 1;
}

int gui_scroll_by(gui_scroll_t *s, int delta_px) {
    if (!s) return 0;
    return gui_scroll_set(s, s->offset + delta_px);
}

// Positive delta scrolls UP (toward the content start). This is the OS-wide
// convention (kernel gui/terminal.c). Do not invert it per-app.
int gui_scroll_wheel(gui_scroll_t *s, int scroll_delta) {
    if (!s || scroll_delta == 0) return 0;
    // Three rows per notch is the established feel (Files, recycle bin).
    return gui_scroll_by(s, -scroll_delta * s->step_px * 3);
}

int gui_scroll_key(gui_scroll_t *s, uint32_t keycode) {
    if (!s) return 0;
    switch (keycode) {
        case GUI_KEY_UP:   return gui_scroll_by(s, -s->step_px);
        case GUI_KEY_DOWN: return gui_scroll_by(s,  s->step_px);
        case GUI_KEY_PGUP: return gui_scroll_by(s, -s->h);
        case GUI_KEY_PGDN: return gui_scroll_by(s,  s->h);
        case GUI_KEY_HOME: return gui_scroll_set(s, 0);
        case GUI_KEY_END:  return gui_scroll_set(s, gui_scroll_max(s));
        default:           return 0;
    }
}

// --- pointer ---------------------------------------------------------------
int gui_scroll_press(gui_scroll_t *s, int mx, int my) {
    if (!s) return 0;
    int ty, th;
    if (!thumb_geom(s, &ty, &th)) return 0;   // nothing to scroll: not ours

    int bx = gui_scroll_bar_x(s);
    if (mx < bx || mx >= bx + GUI_SCROLL_W) return 0;
    if (my < s->y || my >= s->y + s->h)     return 0;

    if (my >= ty && my < ty + th) {
        // Grab the thumb. Remembering the grab offset is what stops the thumb
        // from jumping so its centre snaps under the pointer.
        s->drag = 1;
        s->drag_grab = my - ty;
    } else {
        // Click in the track pages toward the pointer, like every other OS.
        gui_scroll_by(s, my < ty ? -s->h : s->h);
    }
    return 1;
}

int gui_scroll_motion(gui_scroll_t *s, int mx, int my) {
    if (!s) return 0;
    int ty, th;
    if (!thumb_geom(s, &ty, &th)) {
        int was = s->hover; s->hover = 0; s->drag = 0;
        return was != 0;
    }

    if (s->drag) {
        int travel = s->h - th;
        if (travel <= 0) return 0;
        // Invert the thumb-position mapping to recover the offset.
        int rel = my - s->y - s->drag_grab;
        int off = (int)(((long)rel * (long)gui_scroll_max(s)) / (long)travel);
        return gui_scroll_set(s, off);
    }

    int bx = gui_scroll_bar_x(s);
    int hov = (mx >= bx && mx < bx + GUI_SCROLL_W && my >= ty && my < ty + th);
    if (hov != s->hover) { s->hover = hov; return 1; }
    return 0;
}

void gui_scroll_release(gui_scroll_t *s) {
    if (s) s->drag = 0;
}

// --- item helpers ----------------------------------------------------------
int gui_scroll_first_item(const gui_scroll_t *s) {
    if (!s || s->step_px <= 0) return 0;
    return s->offset / s->step_px;
}

int gui_scroll_visible_items(const gui_scroll_t *s) {
    if (!s || s->step_px <= 0) return 0;
    // +1 so a partially visible trailing row is still drawn (and clipped by the
    // caller's viewport) rather than popping in only once fully scrolled.
    return (s->h + s->step_px - 1) / s->step_px + 1;
}

int gui_scroll_reveal(gui_scroll_t *s, int top_px, int h_px) {
    if (!s) return 0;
    if (top_px < s->offset)                    // above the view: align to top
        return gui_scroll_set(s, top_px);
    if (top_px + h_px > s->offset + s->h)      // below: align to bottom
        return gui_scroll_set(s, top_px + h_px - s->h);
    return 0;
}

// --- drawing ---------------------------------------------------------------
// (#745, local queue item 77) READING THE THEME IS NOT THE SAME AS BEING
// READABLE. This widget always resolved its colours from the active theme at
// draw time, which is why the comment below used to claim it was "correct on
// light and dark themes". It was not. Measured with the shipped WCAG code
// (tools/contrast/scrollbar-contrast.sh), scrollbar_thumb against scrollbar_bg
// was below the 3:1 non-text floor on 13 of the 14 shipped themes, and on
// retro_unix, the theme the machine actually ships on, it was 1.23:1: a thumb
// you cannot see. A scrollbar thumb is the ONLY affordance telling a user that
// there is more content and where they are in it, so an invisible one makes a
// truncated list look complete. Trusting a theme file to be accessible is the
// same mistake gui_set_palette() already stopped making for the focus ring.
//
// A THUMB HAS TWO BACKGROUNDS. It sits in the trough, and the whole gutter sits
// on the app's surface, and those are different colours on most themes. On
// retro_unix they are the SAME colour (scrollbar_bg == window_bg == 0xb4b4b4),
// so the trough is invisible and the thumb's real background is the panel. A
// repair measured against the trough alone would therefore have been measured
// against a colour nobody can see. Both are passed in, and the repair has to
// clear the floor against BOTH. That is also what makes an invisible trough
// harmless rather than a problem moved one step sideways: the thumb is legible
// whether or not the trough behind it is.
//
// WHICH BACKGROUNDS ACTUALLY APPLY. Requiring the floor against the trough AND
// the surface unconditionally is too strong, and measurement showed it: on
// ocean, sunset and forest the Settings left nav is a DARK bar (taskbar_bg
// 0x204060) while the trough is a LIGHT strip (scrollbar_bg 0x d0e0f0), and no
// single colour clears 3.3:1 against both, because it would have to be dark to
// read on the trough and light to read on the bar. Those themes were not the
// problem. The rule was. The trough surrounds the thumb on all four sides, so
// the trough is ALWAYS a background; the surface is a background only when the
// trough fails to read as a container in its own right, in which case what the
// user actually sees behind the thumb is the surface. That test is the same 3:1
// figure, applied to the trough, and it is stated ONCE here so the widget and
// tools/contrast/scrollbar-contrast.sh cannot disagree about it.
int gui_scroll_surface_is_bg(uint32_t track, uint32_t surface) {
    return gui_contrast_x100(track, surface) < GUI_FLOOR_NONTEXT;
}

static uint32_t scroll_ink_aim(uint32_t c, uint32_t track, uint32_t surface, int aim) {
    return gui_scroll_surface_is_bg(track, surface)
             ? gui_ensure_contrast2(c, track, surface, aim)
             : gui_ensure_contrast(c, track, aim);
}

uint32_t gui_scroll_thumb_ink(uint32_t thumb, uint32_t track, uint32_t surface) {
    return scroll_ink_aim(thumb, track, surface, GUI_AIM_NONTEXT);
}

// The hover/drag thumb. It must clear the floor like the rest state, and it
// must also still LOOK like a change: repairing both states independently pushes
// them toward the same corner of the colour space, and on a theme whose two
// tokens are a single 8/255 step apart (retro_unix: 0xc8c8c8 / 0xd4d4d4) that
// lands them on the same colour and deletes the hover feedback. Requiring the
// hover state to gain GUI_HOVER_STEP over the REPAIRED rest state keeps the
// direction of the change intact, on every theme, without inventing a colour.
uint32_t gui_scroll_hover_ink(uint32_t hover, uint32_t rest_ink,
                              uint32_t track, uint32_t surface) {
    int aim = gui_contrast_x100(rest_ink, track);
    if (gui_scroll_surface_is_bg(track, surface)) {
        int b = gui_contrast_x100(rest_ink, surface);
        if (b < aim) aim = b;
    }
    aim += GUI_HOVER_STEP;
    if (aim < GUI_AIM_NONTEXT) aim = GUI_AIM_NONTEXT;
    uint32_t hi = scroll_ink_aim(hover, track, surface, aim);
    if (gui_contrast_x100(hi, rest_ink) >= GUI_HOVER_MIN) return hi;
    // The gain was unreachable, and chasing it has collapsed the hover onto the
    // rest colour: high_contrast's rest thumb is already pure white on near
    // black at 20.99:1, so there is nothing above it to move to and the mix ends
    // at white, which is where the rest state already is. Fall back to the
    // theme's OWN hover token raised to the plain floor. On that theme it is
    // cyan, and a hue change is a perfectly good hover cue; what is not
    // acceptable is a hover state that renders identically to rest.
    uint32_t lo = scroll_ink_aim(hover, track, surface, GUI_AIM_NONTEXT);
    return gui_contrast_x100(lo, rest_ink) > gui_contrast_x100(hi, rest_ink) ? lo : hi;
}

// Resolve the pair a scrollbar draws with, from the active theme, repaired.
// Exposed because several apps predate this widget and draw their own gutter
// geometry (Files' two lists, the browser page bar, the recycle bin, the
// installer, the compositor's start menu, which has no window handle to hand a
// widget). Those keep their geometry and call THIS for their colours, so the
// contrast rule has exactly one definition instead of one per app, which is how
// the OS ended up with 13 failing themes in the first place.
// `surface` is the colour the gutter sits ON, which the caller knows and this
// code cannot: it is window_bg in a panel, menu_bg in a popup, textbox_bg in a
// field-style list, taskbar_bg in the Settings left nav.
void gui_scroll_colors(int hot, uint32_t surface,
                       uint32_t *track_out, uint32_t *thumb_out) {
    uint32_t track = theme_color(THEME_COLOR_SCROLLBAR_BG);
    uint32_t rest  = gui_scroll_thumb_ink(theme_color(THEME_COLOR_SCROLLBAR_THUMB),
                                          track, surface);
    if (track_out) *track_out = track;
    if (!thumb_out) return;
    *thumb_out = hot ? gui_scroll_hover_ink(theme_color(THEME_COLOR_SCROLLBAR_THUMB_HOVER),
                                            rest, track, surface)
                     : rest;
}

void gui_scroll_draw_on(int handle, const gui_scroll_t *s, uint32_t surface) {
    if (!s) return;
    int ty, th;
    if (!thumb_geom(s, &ty, &th)) return;   // fits: spend no pixels on chrome

    int bx = gui_scroll_bar_x(s);
    uint32_t track, thumb;
    gui_scroll_colors(s->drag || s->hover, surface, &track, &thumb);

    win_draw_rect(handle, bx, s->y, GUI_SCROLL_W, s->h, track);
    win_draw_rect(handle, bx + 2, ty, GUI_SCROLL_W - 4, th, thumb);
}

// Convenience form for the common case: the gutter sits on a window panel.
// Every existing caller keeps working unchanged and gets the repair; the ones
// that sit on a different surface say so with gui_scroll_draw_on().
void gui_scroll_draw(int handle, const gui_scroll_t *s) {
    gui_scroll_draw_on(handle, s, theme_color(THEME_COLOR_WINDOW_BG));
}
