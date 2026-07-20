// gui_scroll.c - MayteraOS shared scrollable-viewport primitive.
// See gui_scroll.h for the rationale and the wheel-direction convention.
#include "gui_scroll.h"
#include "syscall.h"   // win_draw_rect
#include "theme.h"     // theme_color / THEME_COLOR_SCROLLBAR_*
// NOTE: deliberately does NOT include gui.h. gui.h and theme.h both define
// BTN_COLOR_* / DISPLAY_BG (gui.h as literals, theme.h as theme lookups), so
// including both produces macro-redefinition warnings. Everything needed here
// is in syscall.h (win_draw_rect) and theme.h (theme_color).

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
void gui_scroll_draw(int handle, const gui_scroll_t *s) {
    if (!s) return;
    int ty, th;
    if (!thumb_geom(s, &ty, &th)) return;   // fits: spend no pixels on chrome

    int bx = gui_scroll_bar_x(s);

    // Colors are resolved from the ACTIVE theme at draw time, never hardcoded,
    // so this is correct on light and dark themes and on themes added later.
    uint32_t track = theme_color(THEME_COLOR_SCROLLBAR_BG);
    uint32_t thumb = s->drag || s->hover
                       ? theme_color(THEME_COLOR_SCROLLBAR_THUMB_HOVER)
                       : theme_color(THEME_COLOR_SCROLLBAR_THUMB);

    win_draw_rect(handle, bx, s->y, GUI_SCROLL_W, s->h, track);
    win_draw_rect(handle, bx + 2, ty, GUI_SCROLL_W - 4, th, thumb);
}
