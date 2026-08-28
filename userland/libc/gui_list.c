// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_list.c - MayteraOS shared scrollable listbox primitive.
// See gui_list.h for the rationale (#512) and the box model.
#include "gui_list.h"
#include "syscall.h"   // win_draw_rect / win_draw_text_ttf
#include "gui_style.h" // GUI_TTF_SIZE: the ACTIVE theme's type.body
// NOTE: deliberately does NOT include gui.h or theme.h (see gui_scroll.c's
// note: both define BTN_COLOR_*/DISPLAY_BG and collide). Colors come in as
// plain uint32_t params from the caller, which already resolved them from
// its own theme/palette source, exactly like gui_scroll_draw().

void gui_list_config(gui_list_t *l, int x, int y, int w, int h, int row_h, int count) {
    if (!l) return;
    l->x = x; l->y = y; l->w = w; l->h = h;
    l->row_h = row_h > 0 ? row_h : 16;
    l->count = count < 0 ? 0 : count;
    gui_scroll_config(&l->scroll, x + 1, y + 1, w - 2, h - 2,
                      l->count * l->row_h, l->row_h);
    l->scroll.snap = 1;
}

int gui_list_first(const gui_list_t *l) {
    return l ? gui_scroll_first_item(&l->scroll) : 0;
}

int gui_list_span(const gui_list_t *l) {
    return l ? gui_scroll_visible_items(&l->scroll) : 0;
}

int gui_list_row_y(const gui_list_t *l, int index, int *out_y) {
    if (!l || index < 0 || index >= l->count) return 0;
    int first = gui_list_first(l);
    int ry = l->y + 1 + (index - first) * l->row_h - (l->scroll.offset - first * l->row_h);
    // Clip to the row content area [y+1, y+h-1). This is the generalized
    // #533 fix: gui_list_span() intentionally returns one extra (partially
    // visible trailing) row by contract, and rounding/offset math can also
    // place a row one pixel shy of the box on either edge - without this
    // check a caller draws outside the list, exactly as the size list did.
    if (ry + l->row_h <= l->y + 1 || ry >= l->y + l->h - 1) return 0;
    if (out_y) *out_y = ry;
    return 1;
}

int gui_list_row_w(const gui_list_t *l) {
    if (!l) return 0;
    return gui_scroll_needed(&l->scroll) ? l->w - 2 - GUI_SCROLL_W : l->w - 2;
}

int gui_list_row_at(const gui_list_t *l, int mx, int my) {
    if (!l || l->count <= 0) return -1;
    if (mx < l->x + 1 || mx >= l->x + gui_list_row_w(l) + 1) return -1;
    if (my < l->y + 1 || my >= l->y + l->h - 1) return -1;
    // screen_y(idx) = (y+1) + idx*row_h - offset, so idx = (my-(y+1)+offset)/row_h.
    // my and offset are both >= 0 here (bounds checked above), so plain integer
    // division floors correctly; no need to route through gui_list_first().
    int idx = (my - (l->y + 1) + l->scroll.offset) / l->row_h;
    if (idx < 0 || idx >= l->count) return -1;
    return idx;
}

int gui_list_hit(const gui_list_t *l, int mx, int my) {
    if (!l) return 0;
    return mx >= l->x && mx < l->x + l->w && my >= l->y && my < l->y + l->h;
}

void gui_list_draw(int win, gui_list_t *l, int sel, uint32_t bg, uint32_t border,
                   uint32_t text, uint32_t sel_bg, uint32_t sel_text,
                   gui_list_label_fn label_of, void *ctx) {
    if (!l) return;
    win_draw_rect(win, l->x, l->y, l->w, l->h, bg);
    // 1px frame outline.
    win_draw_rect(win, l->x, l->y, l->w, 1, border);
    win_draw_rect(win, l->x, l->y + l->h - 1, l->w, 1, border);
    win_draw_rect(win, l->x, l->y, 1, l->h, border);
    win_draw_rect(win, l->x + l->w - 1, l->y, 1, l->h, border);

    int first = gui_list_first(l);
    int span  = gui_list_span(l);
    int rw    = gui_list_row_w(l);
    for (int r = 0; r < span; r++) {
        int idx = first + r;
        if (idx >= l->count) break;
        int ry;
        if (!gui_list_row_y(l, idx, &ry)) continue;   // clip: never draw off-box
        char buf[96];
        const char *lbl = label_of ? label_of(ctx, idx, buf, sizeof(buf)) : "";
        // (#appstyle) ANTIALIASED TRUETYPE AT THE THEME'S type.body, not the
        // kernel's 8x16 bitmap font.
        //
        // This is the same fault gui_menu.c carried until #307, in the same
        // shape and for the same reason: win_draw_text() is SYS_WIN_DRAW_TEXT,
        // which walks a 16-byte glyph array and advances cx += 8 with no size
        // argument anywhere on that path. A SHARED widget drawing with it means
        // every app that adopts the widget inherits a typeface the rest of the
        // OS does not use - Terminal's Preferences lists and Disk Images'
        // picker were both rendering blocky monospace rows inside otherwise
        // TrueType windows, and neither app could fix it from its own side.
        //
        // Centring is on GUI_TTF_SIZE (the size argument IS the ascent-descent
        // extent, by stbtt_ScaleForPixelHeight's definition), not on the old
        // hardcoded 16, which was the bitmap cell height and would now sit the
        // ink one pixel high in a 24px row and two in a 22px one.
        int ty = ry + (l->row_h - GUI_TTF_SIZE) / 2;
        if (idx == sel) {
            win_draw_rect(win, l->x + 1, ry, rw, l->row_h, sel_bg);
            win_draw_text_ttf(win, l->x + 6, ty, lbl, GUI_TTF_SIZE, sel_text);
        } else {
            win_draw_text_ttf(win, l->x + 6, ty, lbl, GUI_TTF_SIZE, text);
        }
    }
    // `bg` is this list's own fill, which is what the gutter sits on; a
    // field-style list is textbox_bg, not window_bg (#745 item 77).
    gui_scroll_draw_on(win, &l->scroll, bg);
}

int gui_list_wheel(gui_list_t *l, int mx, int my, int scroll_delta) {
    // #root-cause fix: gate on the FULL outer box (gui_list_hit), not on
    // row_at()-style row content only. Wheel over the scrollbar gutter, or
    // over empty box space below a short list's last row, is still "the
    // pointer is over this list" and must scroll it - only a miss outside
    // the box entirely is a no-op. Previously this took no position at all,
    // which forced every caller to invent its own (too-narrow, or missing)
    // gate; see gui_list.h.
    if (!l || !gui_list_hit(l, mx, my)) return 0;
    return gui_scroll_wheel(&l->scroll, scroll_delta);
}

int gui_list_key(gui_list_t *l, uint32_t keycode) {
    return l ? gui_scroll_key(&l->scroll, keycode) : 0;
}

int gui_list_press(gui_list_t *l, int mx, int my) {
    if (!l) return -1;
    if (gui_scroll_press(&l->scroll, mx, my)) return -1;   // scrollbar took it
    return gui_list_row_at(l, mx, my);
}

int gui_list_motion(gui_list_t *l, int mx, int my) {
    return l ? gui_scroll_motion(&l->scroll, mx, my) : 0;
}

void gui_list_release(gui_list_t *l) {
    if (l) gui_scroll_release(&l->scroll);
}

int gui_list_move_sel(gui_list_t *l, int *sel, int delta) {
    if (!l || !sel || l->count <= 0) return 0;
    int n = *sel + delta;
    if (n < 0) n = 0;
    if (n >= l->count) n = l->count - 1;
    int changed = (n != *sel);
    *sel = n;
    gui_scroll_reveal(&l->scroll, n * l->row_h, l->row_h);
    return changed;
}
