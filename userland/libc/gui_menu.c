// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_menu.c - MayteraOS shared menu-bar / dropdown-menu primitive.
// See gui_menu.h for the rationale (#562/#512) and usage.
#include "gui_menu.h"
#include "gui.h"       // gui_ttf_width (via gui_style.h), win_draw_rect/win_draw_text_ttf
#include "theme.h"     // theme_color_of / theme_metric_or - the widget themes ITSELF

// The POPUP ROW's left gutter. Unlike the bar's padding (GUI_MENU_BAR_PAD,
// which is metric.gap) this one is NOT free and is NOT a theme read: the
// check/radio mark (gui_menu_item_t.checked) is drawn inside it, spanning
// l->x+3 .. l->x+9, so any gutter below ~10px puts the tick under the first
// glyph of the label. It is also the right inset the accelerator hint sits
// flush against. Collapsing it into the bar's padding, because the two used to
// share one constant, would look correct until the first checked row.
#define MENU_PAD_X   10
#define MENU_GAP     16   // minimum gap between a label and its accelerator
#define MENU_MIN_W   110

// Measured line height at `size`. win_draw_text_ttf()'s y is the TOP of the
// line box and the baseline is y + ascent, so centring a label in a row means
// centring (ascent - descent), not the nominal size - stbtt's pixel-height
// scale keeps those close but not equal. Same method as gui_text_ttf_centered()
// in gui.c: ask the renderer, do not assume the line is exactly `size` px tall.
static int menu_line_h(int size) {
    int m[3] = { size, 0, 0 };
    if (font_metrics(0, size, m) == 0) {
        int lh = m[0] - m[1];
        if (lh > 0) return lh;
    }
    return size;
}

// Resolve every metric the widget draws with from the ACTIVE THEME, once.
// theme_metric() returns 0 for an id this kernel does not know, and 0 is not a
// legal row height or type size, so each read carries its own fallback.
// theme_metric()/theme_metric_of() both return 0 for an id this kernel does not
// know, and 0 is not a legal size, so every read needs its own fallback. One
// helper so the index-aware and live paths cannot drift.
static int menu_metric(int idx, theme_metric_id_t id, int fallback) {
    int v = (idx < 0) ? theme_metric(id) : theme_metric_of(idx, id);
    return v > 0 ? v : fallback;
}

// The metrics follow the SAME theme index as the palette. An app whose chrome
// tracks a theme other than the live one (the Terminal does) would otherwise
// get that theme's colours with the live theme's row height.
static void menu_metrics(gui_menu_bar_t *bar, int idx) {
    bar->font_size = menu_metric(idx, THEME_METRIC_TYPE_BODY,  GUI_MENU_FONT_SIZE);
    bar->row_h     = menu_metric(idx, THEME_METRIC_MENU_ROW_H, GUI_MENU_ITEM_H);
    bar->pad_x     = menu_metric(idx, THEME_METRIC_GAP,        GUI_MENU_BAR_PAD);
    // A theme file is DATA a user can edit. Clamp to a range that still draws a
    // menu rather than trusting whatever number is in the file: an 800px row
    // height or a 2px type size is a broken theme, not a design choice.
    if (bar->font_size < 8  || bar->font_size > 32) bar->font_size = GUI_MENU_FONT_SIZE;
    if (bar->row_h    < 12 || bar->row_h    > 64) bar->row_h    = GUI_MENU_ITEM_H;
    if (bar->pad_x    < 2  || bar->pad_x    > 32) bar->pad_x    = GUI_MENU_BAR_PAD;
    bar->text_h = menu_line_h(bar->font_size);
}

void gui_menu_palette_theme(gui_menu_palette_t *out, int idx) {
    if (!out) return;
    uint32_t bg     = (idx < 0) ? theme_color(THEME_COLOR_MENU_BG)
                                : theme_color_of(idx, THEME_COLOR_MENU_BG);
    uint32_t text   = (idx < 0) ? theme_color(THEME_COLOR_MENU_TEXT)
                                : theme_color_of(idx, THEME_COLOR_MENU_TEXT);
    uint32_t border = (idx < 0) ? theme_color(THEME_COLOR_MENU_BORDER)
                                : theme_color_of(idx, THEME_COLOR_MENU_BORDER);
    uint32_t hover  = (idx < 0) ? theme_color(THEME_COLOR_MENU_ITEM_HOVER)
                                : theme_color_of(idx, THEME_COLOR_MENU_ITEM_HOVER);
    uint32_t dim    = (idx < 0) ? theme_color(THEME_COLOR_MENU_TEXT_DISABLED)
                                : theme_color_of(idx, THEME_COLOR_MENU_TEXT_DISABLED);
    uint32_t sep    = (idx < 0) ? theme_color(THEME_COLOR_MENU_SEPARATOR)
                                : theme_color_of(idx, THEME_COLOR_MENU_SEPARATOR);
    uint32_t accent = (idx < 0) ? theme_color(THEME_COLOR_ACCENT)
                                : theme_color_of(idx, THEME_COLOR_ACCENT);

    out->bar_bg             = bg;
    out->bar_text           = gui_ensure_contrast(text, bg, GUI_FLOOR_TEXT);
    out->bar_hover_bg       = hover;
    out->bar_open_bg        = accent;
    out->bar_open_text      = gui_ensure_contrast(text, accent, GUI_FLOOR_TEXT);
    out->popup_bg           = bg;
    out->popup_border       = border;
    out->item_text          = out->bar_text;
    out->item_text_disabled = dim;
    out->item_hover_bg      = hover;
    out->item_hover_text    = gui_ensure_contrast(text, hover, GUI_FLOOR_TEXT);
    out->shortcut_text      = dim;
    out->separator          = sep;
}

// ONE place measures and ONE place draws, both at bar->font_size, so a label's
// box can never disagree with the glyphs inside it. (The old code measured with
// gui_string_width(), a strlen * 8 fixed-cell estimate that was only ever right
// for the 8x16 bitmap font it drew with.)
static int menu_text_w(const gui_menu_bar_t *bar, const char *s) {
    return gui_ttf_width(s, bar->font_size);
}

// Lay the top-level label boxes out. Split out of gui_menu_bar_init() so a live
// theme change can re-lay them without resetting the bar's open/hot state.
static void menu_layout(gui_menu_bar_t *bar) {
    int cx = bar->x;
    for (int i = 0; i < bar->menu_count; i++) {
        int w = menu_text_w(bar, bar->menus[i].label) + 2 * bar->pad_x;
        bar->item_x[i] = cx;
        bar->item_w[i] = w;
        cx += w;
    }
}

void gui_menu_sync_theme(gui_menu_bar_t *bar, int theme_index) {
    if (!bar || bar->menu_count <= 0) return;
    menu_metrics(bar, theme_index);
    gui_menu_palette_theme(&bar->pal, theme_index);
    menu_layout(bar);
}

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

    // A bar is themed by DEFAULT. An app that declares its menus and calls
    // nothing else gets correct, contrast-checked, theme-tracking colours -
    // which is the whole point of it being a primitive. gui_menu_set_palette()
    // remains the override for an app with its own chrome identity (Editor).
    menu_metrics(bar, -1);
    gui_menu_palette_theme(&bar->pal, -1);
    menu_layout(bar);
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
static int popup_h_for(const gui_menu_bar_t *bar, int item_count) {
    int vis = item_count < GUI_MENU_MAX_VISIBLE ? item_count : GUI_MENU_MAX_VISIBLE;
    if (vis < 1) vis = 1;
    return vis * bar->row_h + 2;
}

static void menu_geom(const gui_menu_bar_t *bar, int win_w, int win_h,
                      int *bx, int *by, int *bw, int *bh) {
    const gui_menu_t *m = &bar->menus[bar->open];
    int w = MENU_MIN_W;
    for (int i = 0; i < m->item_count; i++) {
        const gui_menu_item_t *it = &m->items[i];
        if (!it->label) continue;
        int rw = menu_text_w(bar, it->label);
        if (it->shortcut) rw += MENU_GAP + menu_text_w(bar, it->shortcut);
        rw += 2 * MENU_PAD_X;
        if (rw > w) w = rw;
    }
    int max_w = win_w - 8;
    if (w > max_w) w = max_w;

    int h = popup_h_for(bar, m->item_count);

    int x = bar->item_x[bar->open];
    if (x + w > win_w - 4) x = win_w - 4 - w;
    if (x < 0) x = 0;

    int y = bar->y + bar->h;
    if (y + h > win_h - 4 && bar->y - h >= 0) y = bar->y - h;   // flip above

    // (#307) THE WINDOW CAN BE SHORTER THAN THE POPUP, and for a bar at y == 0
    // - which is every menu bar in this tree - the flip above is arithmetically
    // impossible (bar->y - h is always negative), so there was no second
    // chance. MEASURED on golden 2045 with the Terminal dragged down to a
    // ~135px content area: the View popup's lower rows were drawn past the
    // window's bottom edge, clipped by the compositor, and simply unreachable.
    //
    // Clipping is the one outcome this widget exists to prevent. gui_menu is
    // built on gui_list precisely so that a menu with more rows than fit
    // SCROLLS (see this file's header); popup_h_for() just did not know about
    // the window. Clamp the height to what the window can actually show and
    // hand THAT to gui_list_config(), which then reports fewer visible rows and
    // draws its own scrollbar. Never below one row plus the border, or the
    // popup becomes a box with nothing in it.
    int avail = win_h - 4 - y;
    if (h > avail) h = avail;
    if (h < bar->row_h + 2) h = bar->row_h + 2;

    *bx = x; *by = y; *bw = w; *bh = h;
}

// --- open / close --------------------------------------------------------
static void open_menu(gui_menu_bar_t *bar, int idx) {
    bar->open = idx;
    bar->hot_item = -1;
    // Seed with a real (window-size-independent) height so gui_scroll_max()
    // is already sane if a wheel/key event arrives before the next draw
    // pass calls refresh_list() with the true, window-clamped geometry.
    gui_list_config(&bar->list, 0, 0, MENU_MIN_W, popup_h_for(bar, bar->menus[idx].item_count),
                    bar->row_h, bar->menus[idx].item_count);
}

int gui_menu_alt_open(gui_menu_bar_t *bar, int letter) {
    if (!bar || bar->menu_count <= 0) return 0;
    int want = (letter >= 'A' && letter <= 'Z') ? letter - 'A' + 'a' : letter;
    for (int i = 0; i < bar->menu_count; i++) {
        const char *lb = bar->menus[i].label;
        if (!lb || !lb[0]) continue;
        int c = lb[0];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        if (c == want) { open_menu(bar, i); return 1; }
    }
    return 0;
}

void gui_menu_close(gui_menu_bar_t *bar) {
    if (!bar) return;
    bar->open = -1;
    bar->hot_item = -1;
}

void gui_menu_leave(gui_menu_bar_t *bar) {
    if (!bar) return;
    gui_menu_close(bar);
    bar->hot_top = -1;   // see gui_menu.h: no motion arrives once the pointer
                         // is in another window, so nothing else would clear it
}

static void refresh_list(gui_menu_bar_t *bar, int win_w, int win_h) {
    int bx, by, bw, bh;
    menu_geom(bar, win_w, win_h, &bx, &by, &bw, &bh);
    gui_list_config(&bar->list, bx, by, bw, bh, bar->row_h,
                    bar->menus[bar->open].item_count);
}

// --- drawing ---------------------------------------------------------------
void gui_menu_bar_draw(int win, gui_menu_bar_t *bar) {
    if (!bar || bar->menu_count <= 0) return;
    int bar_w = bar->item_x[bar->menu_count - 1] + bar->item_w[bar->menu_count - 1] - bar->x;
    win_draw_rect(win, bar->x, bar->y, bar_w, bar->h, bar->pal.bar_bg);
    for (int i = 0; i < bar->menu_count; i++) {
        bool is_open  = (i == bar->open);
        bool is_hover = (i == bar->hot_top && bar->open < 0);
        uint32_t ink = bar->pal.bar_text;
        if (is_open) {
            win_draw_rect(win, bar->item_x[i], bar->y, bar->item_w[i], bar->h,
                         bar->pal.bar_open_bg);
            ink = bar->pal.bar_open_text;
        } else if (is_hover) {
            win_draw_rect(win, bar->item_x[i], bar->y, bar->item_w[i], bar->h,
                         bar->pal.bar_hover_bg);
        }
        int lh = bar->text_h > 0 ? bar->text_h : bar->font_size;
        int ty = bar->y + (bar->h - lh) / 2;
        // ARGUMENT ORDER: win_draw_text_ttf(h, x, y, s, SIZE, COLOR). Both
        // trailing args are ints and -Wconversion is off, so swapping them
        // compiles silently and renders the label at (colour & 0xFF) points -
        // that is how a 113pt badge shipped elsewhere in this tree. Read
        // syscall.h, do not pattern-match gui_text_ttf_centered(), whose
        // trailing pair is the other way round (colour, size).
        win_draw_text_ttf(win, bar->item_x[i] + bar->pad_x, ty,
                          bar->menus[i].label, bar->font_size, ink);
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
            // Inset 8px each side (metric.gap), centred in the row. The row
            // is a FULL row: see gui_menu.h on why the popup's height model is
            // uniform.
            win_draw_rect(win, l->x + 1 + 8, ry + bar->row_h / 2, rw - 16, 1,
                         bar->pal.separator);
            continue;
        }

        bool hot = (idx == bar->hot_item) && it->enabled;
        if (hot) win_draw_rect(win, l->x + 1, ry, rw, bar->row_h, bar->pal.item_hover_bg);

        uint32_t ink = !it->enabled ? bar->pal.item_text_disabled
                     : hot          ? bar->pal.item_hover_text
                                    : bar->pal.item_text;
        int lh = bar->text_h > 0 ? bar->text_h : bar->font_size;
        int ty = ry + (bar->row_h - lh) / 2;
        // The check mark lives INSIDE the existing MENU_PAD_X gutter, so a
        // menu that uses it is exactly as wide as one that does not and the
        // label x is unchanged for every already-adopted caller.
        if (it->checked) {
            int cx = l->x + 2, cy = ry + bar->row_h / 2;
            for (int t = 0; t < 2; t++) {
                gui_line(win, cx + 1, cy + t,     cx + 3, cy + 2 + t, ink);
                gui_line(win, cx + 3, cy + 2 + t, cx + 7, cy - 3 + t, ink);
            }
        }
        win_draw_text_ttf(win, l->x + 1 + MENU_PAD_X, ty, it->label,
                          bar->font_size, ink);

        if (it->shortcut) {
            int sw = menu_text_w(bar, it->shortcut);
            win_draw_text_ttf(win, l->x + 1 + rw - MENU_PAD_X - sw, ty, it->shortcut,
                              bar->font_size,
                              !it->enabled ? bar->pal.item_text_disabled
                                           : bar->pal.shortcut_text);
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

    // hot_top IS TRACKED IN BOTH BRANCHES, and it used to be tracked only in
    // the closed one. MEASURED on the uplift build: with File open, moving the
    // pointer onto Bookmarks and clicking left the bar drawing a highlight
    // under FILE while the pointer sat over Bookmarks. The motion switched the
    // open menu (below) but never moved hot_top, so the instant the menu closed
    // the closed-bar draw fell back to a hot_top from before the walk started.
    //
    // It was invisible until this pass only because hover and open shared one
    // colour, so a stale highlight looked like "a menu is up". Splitting them
    // is what made a pre-existing state bug legible. Fix the state, not the
    // colours.
    int top = hit_top(bar, mx, my);
    int changed = 0;
    if (top != bar->hot_top) { bar->hot_top = top; changed = 1; }

    if (bar->open >= 0) {
        refresh_list(bar, win_w, win_h);
        gui_list_t *l = &bar->list;
        if (gui_list_motion(l, mx, my)) changed = 1;
        int idx = gui_list_row_at(l, mx, my);
        if (idx != bar->hot_item) { bar->hot_item = idx; changed = 1; }
        // Hovering a different top label while a menu is open switches to it
        // (standard menu-bar behavior).
        if (top >= 0 && top != bar->open) { open_menu(bar, top); changed = 1; }
    }
    return changed;
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
