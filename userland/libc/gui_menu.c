// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_menu.c - MayteraOS shared menu-bar / dropdown-menu primitive.
// See gui_menu.h for the rationale (#562/#512) and usage.
#include "gui_menu.h"
#include "gui.h"       // gui_string_width, win_draw_rect/win_draw_text (via syscall.h)

#define MENU_PAD_X   10
#define MENU_GAP     16   // between label and shortcut hint
#define MENU_MIN_W   110

// --- layout ------------------------------------------------------------
void gui_menu_bar_init(gui_menu_bar_t *bar, const gui_menu_t *menus, int menu_count,
                       int x, int y, int h) {
    if (!bar) return;
    if (menu_count > GUI_MENU_MAX_TOP) menu_count = GUI_MENU_MAX_TOP;
    bar->menus = menus;
    bar->menu_count = menu_count;
    bar->x = x; bar->y = y; bar->h = h;
    bar->open = -1;
    bar->hot_top = -1;
    bar->hot_item = -1;

    int cx = x;
    for (int i = 0; i < menu_count; i++) {
        int w = gui_string_width(menus[i].label) + 2 * MENU_PAD_X;
        bar->item_x[i] = cx;
        bar->item_w[i] = w;
        cx += w;
    }
}

void gui_menu_set_palette(gui_menu_bar_t *bar, const gui_menu_palette_t *pal) {
    if (bar && pal) bar->pal = *pal;
}

// --- popup geometry (recomputed every call, like Settings' dropdown_geom) -
// Width fits the longest label+shortcut in the open menu; height shows up to
// GUI_MENU_MAX_VISIBLE rows and flips above the bar if it would run off the
// bottom of the window. Longer menus SCROLL (gui_list/gui_scroll), they do
// not truncate - that is the entire point of building this on gui_list.
// Popup height for a menu of `item_count` rows, independent of window size:
// up to GUI_MENU_MAX_VISIBLE rows, the rest reachable by scrolling. Used both
// to seed the list the instant a menu opens (so a stray wheel/key event
// before the next draw still has sane gui_scroll_max()) and by menu_geom().
static int popup_h_for(int item_count) {
    int vis = item_count < GUI_MENU_MAX_VISIBLE ? item_count : GUI_MENU_MAX_VISIBLE;
    if (vis < 1) vis = 1;
    return vis * GUI_MENU_ITEM_H + 2;
}

static void menu_geom(const gui_menu_bar_t *bar, int win_w, int win_h,
                      int *bx, int *by, int *bw, int *bh) {
    const gui_menu_t *m = &bar->menus[bar->open];
    int w = MENU_MIN_W;
    for (int i = 0; i < m->item_count; i++) {
        const gui_menu_item_t *it = &m->items[i];
        if (!it->label) continue;
        int rw = gui_string_width(it->label);
        if (it->shortcut) rw += MENU_GAP + gui_string_width(it->shortcut);
        rw += 2 * MENU_PAD_X;
        if (rw > w) w = rw;
    }
    int max_w = win_w - 8;
    if (w > max_w) w = max_w;

    int h = popup_h_for(m->item_count);

    int x = bar->item_x[bar->open];
    if (x + w > win_w - 4) x = win_w - 4 - w;
    if (x < 0) x = 0;

    int y = bar->y + bar->h;
    if (y + h > win_h - 4 && bar->y - h >= 0) y = bar->y - h;   // flip above

    *bx = x; *by = y; *bw = w; *bh = h;
}

// --- open / close --------------------------------------------------------
static void open_menu(gui_menu_bar_t *bar, int idx) {
    bar->open = idx;
    bar->hot_item = -1;
    // Seed with a real (window-size-independent) height so gui_scroll_max()
    // is already sane if a wheel/key event arrives before the next draw
    // pass calls refresh_list() with the true, window-clamped geometry.
    gui_list_config(&bar->list, 0, 0, MENU_MIN_W, popup_h_for(bar->menus[idx].item_count),
                    GUI_MENU_ITEM_H, bar->menus[idx].item_count);
}

void gui_menu_close(gui_menu_bar_t *bar) {
    if (!bar) return;
    bar->open = -1;
    bar->hot_item = -1;
}

static void refresh_list(gui_menu_bar_t *bar, int win_w, int win_h) {
    int bx, by, bw, bh;
    menu_geom(bar, win_w, win_h, &bx, &by, &bw, &bh);
    gui_list_config(&bar->list, bx, by, bw, bh, GUI_MENU_ITEM_H,
                    bar->menus[bar->open].item_count);
}

// --- drawing ---------------------------------------------------------------
void gui_menu_bar_draw(int win, gui_menu_bar_t *bar) {
    if (!bar || bar->menu_count <= 0) return;
    int bar_w = bar->item_x[bar->menu_count - 1] + bar->item_w[bar->menu_count - 1] - bar->x;
    win_draw_rect(win, bar->x, bar->y, bar_w, bar->h, bar->pal.bar_bg);
    for (int i = 0; i < bar->menu_count; i++) {
        bool hi = (i == bar->open) || (i == bar->hot_top && bar->open < 0);
        if (hi) {
            win_draw_rect(win, bar->item_x[i], bar->y, bar->item_w[i], bar->h,
                         bar->pal.bar_hover_bg);
        }
        int ty = bar->y + (bar->h - 16) / 2;
        win_draw_text(win, bar->item_x[i] + MENU_PAD_X, ty, bar->menus[i].label,
                      bar->pal.bar_text);
    }
}

void gui_menu_popup_draw(int win, gui_menu_bar_t *bar, int win_w, int win_h) {
    if (!bar || bar->open < 0) return;
    refresh_list(bar, win_w, win_h);
    const gui_menu_t *m = &bar->menus[bar->open];
    gui_list_t *l = &bar->list;

    win_draw_rect(win, l->x, l->y, l->w, l->h, bar->pal.popup_bg);
    win_draw_rect(win, l->x, l->y, l->w, 1, bar->pal.popup_border);
    win_draw_rect(win, l->x, l->y + l->h - 1, l->w, 1, bar->pal.popup_border);
    win_draw_rect(win, l->x, l->y, 1, l->h, bar->pal.popup_border);
    win_draw_rect(win, l->x + l->w - 1, l->y, 1, l->h, bar->pal.popup_border);

    int first = gui_list_first(l);
    int span  = gui_list_span(l);
    int rw    = gui_list_row_w(l);
    for (int r = 0; r < span; r++) {
        int idx = first + r;
        if (idx >= m->item_count) break;
        int ry;
        if (!gui_list_row_y(l, idx, &ry)) continue;   // never draw off the popup
        const gui_menu_item_t *it = &m->items[idx];

        if (!it->label) {   // separator: thin rule at row mid-height
            win_draw_rect(win, l->x + 1 + 6, ry + GUI_MENU_ITEM_H / 2, rw - 12, 1,
                         bar->pal.separator);
            continue;
        }

        bool hot = (idx == bar->hot_item) && it->enabled;
        if (hot) win_draw_rect(win, l->x + 1, ry, rw, GUI_MENU_ITEM_H, bar->pal.item_hover_bg);

        uint32_t ink = !it->enabled ? bar->pal.item_text_disabled
                     : hot          ? bar->pal.item_hover_text
                                    : bar->pal.item_text;
        int ty = ry + (GUI_MENU_ITEM_H - 16) / 2;
        win_draw_text(win, l->x + 1 + MENU_PAD_X, ty, it->label, ink);

        if (it->shortcut) {
            int sw = gui_string_width(it->shortcut);
            win_draw_text(win, l->x + 1 + rw - MENU_PAD_X - sw, ty, it->shortcut,
                         !it->enabled ? bar->pal.item_text_disabled : bar->pal.shortcut_text);
        }
    }
    // The popup's own fill is the surface behind this gutter, not the
    // window's (#745 item 77): a dropdown floats over whatever is beneath it.
    gui_scroll_draw_on(win, &l->scroll, bar->pal.popup_bg);
}

// --- input -----------------------------------------------------------------
static int hit_top(const gui_menu_bar_t *bar, int mx, int my) {
    if (my < bar->y || my >= bar->y + bar->h) return -1;
    for (int i = 0; i < bar->menu_count; i++)
        if (mx >= bar->item_x[i] && mx < bar->item_x[i] + bar->item_w[i]) return i;
    return -1;
}

int gui_menu_bar_click(gui_menu_bar_t *bar, int mx, int my, int win_w, int win_h) {
    if (!bar) return -1;

    if (bar->open >= 0) {
        refresh_list(bar, win_w, win_h);
        gui_list_t *l = &bar->list;
        bool in_popup = (mx >= l->x && mx < l->x + l->w && my >= l->y && my < l->y + l->h);
        if (in_popup) {
            if (gui_scroll_press(&l->scroll, mx, my)) return -2;   // scrollbar drag/page
            int idx = gui_list_row_at(l, mx, my);
            const gui_menu_t *m = &bar->menus[bar->open];
            if (idx >= 0 && idx < m->item_count && m->items[idx].label && m->items[idx].enabled) {
                int id = m->items[idx].id;
                gui_menu_close(bar);
                return id;
            }
            return -2;   // separator/disabled/miss inside the box: stays open
        }
        int top = hit_top(bar, mx, my);
        if (top >= 0 && top != bar->open) { open_menu(bar, top); return -2; }
        gui_menu_close(bar);
        return -2;   // click outside: closes, consumed (matches dropdown_click)
    }

    int top = hit_top(bar, mx, my);
    if (top >= 0) { open_menu(bar, top); return -2; }
    return -1;   // not our click at all
}

int gui_menu_motion(gui_menu_bar_t *bar, int mx, int my, int win_w, int win_h) {
    if (!bar) return 0;
    if (bar->open >= 0) {
        refresh_list(bar, win_w, win_h);
        gui_list_t *l = &bar->list;
        int changed = gui_list_motion(l, mx, my);
        int idx = gui_list_row_at(l, mx, my);
        if (idx != bar->hot_item) { bar->hot_item = idx; changed = 1; }
        // Hovering a different top label while a menu is open switches to it
        // (standard menu-bar behavior).
        int top = hit_top(bar, mx, my);
        if (top >= 0 && top != bar->open) { open_menu(bar, top); changed = 1; }
        return changed;
    }
    int top = hit_top(bar, mx, my);
    if (top != bar->hot_top) { bar->hot_top = top; return 1; }
    return 0;
}

int gui_menu_wheel(gui_menu_bar_t *bar, int mx, int my, int win_w, int win_h,
                   int scroll_delta) {
    if (!bar || bar->open < 0) return 0;
    // Refresh first, same as gui_menu_bar_click/gui_menu_motion: the popup
    // can flip above the bar or resize its width, and gui_list_wheel's hit
    // test needs this frame's real box, not whatever it was last drawn at.
    refresh_list(bar, win_w, win_h);
    return gui_list_wheel(&bar->list, mx, my, scroll_delta);
}

void gui_menu_release(gui_menu_bar_t *bar) {
    if (bar && bar->open >= 0) gui_list_release(&bar->list);
}

int gui_menu_key(gui_menu_bar_t *bar, uint32_t keycode, char key_char) {
    if (!bar || bar->open < 0) return -1;
    const gui_menu_t *m = &bar->menus[bar->open];

    if (key_char == 27) { gui_menu_close(bar); return -2; }               // Esc

    if (keycode == GUI_KEY_LEFT || keycode == GUI_KEY_RIGHT) {
        if (bar->menu_count > 1) {
            int n = bar->open + (keycode == GUI_KEY_RIGHT ? 1 : -1);
            if (n < 0) n = bar->menu_count - 1;
            if (n >= bar->menu_count) n = 0;
            open_menu(bar, n);
        }
        return -2;
    }

    if (keycode == GUI_KEY_UP || keycode == GUI_KEY_DOWN) {
        int step = (keycode == GUI_KEY_DOWN) ? 1 : -1;
        int idx = (bar->hot_item < 0) ? (step > 0 ? -1 : m->item_count) : bar->hot_item;
        for (int tries = 0; tries < m->item_count; tries++) {
            idx += step;
            if (idx < 0) idx = m->item_count - 1;
            if (idx >= m->item_count) idx = 0;
            if (m->items[idx].label) break;   // skip separators
        }
        bar->hot_item = idx;
        gui_list_move_sel(&bar->list, &bar->hot_item, 0);   // reveal, no move
        return -2;
    }

    if (key_char == '\n' || key_char == '\r' || key_char == ' ') {
        if (bar->hot_item >= 0 && bar->hot_item < m->item_count) {
            const gui_menu_item_t *it = &m->items[bar->hot_item];
            if (it->label && it->enabled) {
                int id = it->id;
                gui_menu_close(bar);
                return id;
            }
        }
        return -2;
    }

    return -1;
}
