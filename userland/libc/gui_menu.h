// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_menu.h - MayteraOS shared menu-bar / dropdown-menu primitive (#562/#512).
//
// WHY THIS EXISTS:
// Every app with a menu bar (Editor, and whichever came before it) hand-rolled
// its own fixed-x-coordinate label row plus a hand-rolled click hit-test - and
// the hit-test drifted out of sync with the draw. The Editor shipped drawing
// FOUR menus (File, Edit, Search, Font) but only hit-testing TWO (Search and
// Font): File and Edit were pure decoration with zero click handling (#562).
// That was not a one-line miss, it was the predictable result of there being
// no shared menu widget to reach for - fixing it "locally" would have meant
// inventing a fourth bespoke dropdown in the same file that already had three
// slightly different ones.
//
// This widget is built ON TOP of gui_list.h (#512): the open popup is a
// gui_list_t, so a menu with more items than fit on screen SCROLLS (real
// scrollbar, wheel, keys) instead of silently losing items off the bottom -
// the same bug class as the Settings DD_VISIBLE truncation, killed here too.
//
// USAGE (see userland/apps/editor/main.c for the adopted example):
//
//     static const gui_menu_item_t FILE_ITEMS[] = {
//         { "New",  "Ctrl+N", ID_FILE_NEW,  true },
//         { "Save", "Ctrl+S", ID_FILE_SAVE, true },
//         { NULL, NULL, 0, false },                    // separator
//         { "Exit", NULL,     ID_FILE_EXIT, true },
//     };
//     static const gui_menu_t MENUS[] = {
//         { "File", FILE_ITEMS, 4 },
//         { "Edit", EDIT_ITEMS, EDIT_N },
//     };
//     static gui_menu_bar_t g_bar;
//     gui_menu_bar_init(&g_bar, MENUS, 2, 0, 0, MENU_HEIGHT);
//     gui_menu_set_palette(&g_bar, &pal);   // call again whenever the app's
//                                            // theme changes live
//
//     // draw pass:
//     gui_menu_bar_draw(win, &g_bar);              // the closed bar
//     ... rest of the window ...
//     gui_menu_popup_draw(win, &g_bar, win_w, win_h);  // LAST: overlays on top
//
//     // input:
//     int id = gui_menu_bar_click(&g_bar, mx, my, win_w, win_h);
//     if (id >= 0) { handle_action(id); redraw(); }
//     else if (id == -2) { redraw(); }              // menu opened/closed/moved
//     else { /* not consumed: fall through to normal window click handling */ }
#ifndef _GUI_MENU_H
#define _GUI_MENU_H

#include "types.h"
#include "gui_list.h"

// A single row in an open menu. label == NULL marks a separator: a thin
// rule with no hit target (id/enabled are ignored). A disabled row is drawn
// dimmed and absorbs clicks/keys without activating or closing the menu,
// exactly like a native menu.
typedef struct {
    const char *label;
    const char *shortcut;   // right-aligned hint, e.g. "Ctrl+S"; NULL for none
    int         id;         // returned to the caller on activation (app-defined)
    bool        enabled;
} gui_menu_item_t;

// One top-level menu ("File", "Edit", ...) and its item table.
typedef struct {
    const char            *label;
    const gui_menu_item_t *items;
    int                    item_count;
} gui_menu_t;

#define GUI_MENU_MAX_TOP      16   // top-level menu cap (File/Edit/View/...)
#define GUI_MENU_ITEM_H       22   // matches THEME_METRIC_MENU_ITEM_HEIGHT
#define GUI_MENU_MAX_VISIBLE  12   // rows shown before the popup scrolls

// Caller-resolved colors (from the app's own theme/palette source - this
// widget does not assume the kernel THEME_COLOR_* system, so it can match
// an app's existing bespoke chrome, e.g. the Editor's VSCode-style palette,
// exactly as it looked before adoption).
typedef struct {
    uint32_t bar_bg;
    uint32_t bar_text;
    uint32_t bar_hover_bg;        // top label: hovered OR its menu is open
    uint32_t popup_bg;
    uint32_t popup_border;
    uint32_t item_text;
    uint32_t item_text_disabled;
    uint32_t item_hover_bg;
    uint32_t item_hover_text;
    uint32_t shortcut_text;
    uint32_t separator;
} gui_menu_palette_t;

typedef struct {
    const gui_menu_t *menus;
    int menu_count;
    int x, y, h;                       // bar rect (width = sum of item_w)
    int item_x[GUI_MENU_MAX_TOP];
    int item_w[GUI_MENU_MAX_TOP];
    gui_menu_palette_t pal;

    int open;                          // index into menus[], -1 = closed
    int hot_top;                       // hovered top label while closed, -1 = none
    int hot_item;                      // hovered/keyboard-selected popup row, -1 = none
    gui_list_t list;                   // popup scroller (persists offset while open)
} gui_menu_bar_t;

// Lay out the bar (measures label widths) and reset to closed. `menus` must
// outlive the bar (it is typically a static const table). Call again if the
// label set itself changes; safe to call once at startup otherwise.
void gui_menu_bar_init(gui_menu_bar_t *bar, const gui_menu_t *menus, int menu_count,
                       int x, int y, int h);

// Update colors. Call whenever the app's live theme changes (the palette is
// copied in, not referenced).
void gui_menu_set_palette(gui_menu_bar_t *bar, const gui_menu_palette_t *pal);

// Draw the closed bar. Call every normal draw pass.
void gui_menu_bar_draw(int win, gui_menu_bar_t *bar);

// Draw the open popup, if any (no-op when closed). Call LAST in the draw
// pass so it overlays the rest of the window, same convention as Settings'
// dropdown_render().
void gui_menu_popup_draw(int win, gui_menu_bar_t *bar, int win_w, int win_h);

// Route a mouse-down at window-local (mx,my).
//   -1  not consumed: click was outside the bar and outside any open popup;
//       the caller should fall through to its own click handling.
//   -2  consumed, no item activated (opened/closed/switched a menu, or hit
//       a disabled row/separator). Caller should just redraw.
//   >=0 the id of the activated item. The menu is now closed.
int gui_menu_bar_click(gui_menu_bar_t *bar, int mx, int my, int win_w, int win_h);

// Route motion/wheel while a menu is open. Return 1 if a redraw is needed.
// Cheap no-ops when closed.
int gui_menu_motion(gui_menu_bar_t *bar, int mx, int my, int win_w, int win_h);
// Takes the pointer position (and win_w/win_h, to refresh the popup geometry
// first, matching gui_menu_bar_click/gui_menu_motion) so the wheel is gated
// on gui_list_hit() - anywhere over the open popup's full box, gutter
// included - instead of applying to the popup no matter where the pointer
// actually is in the window.
int gui_menu_wheel(gui_menu_bar_t *bar, int mx, int my, int win_w, int win_h,
                   int scroll_delta);
void gui_menu_release(gui_menu_bar_t *bar);

// Keyboard while open: Escape closes; Left/Right switch the open top menu;
// Up/Down move the highlighted row (skipping separators); Enter/Space
// activates it. Returns the same contract as gui_menu_bar_click (-1 not
// consumed / not open, -2 consumed no activation, >=0 activated id).
int gui_menu_key(gui_menu_bar_t *bar, uint32_t keycode, char key_char);

void gui_menu_close(gui_menu_bar_t *bar);
static inline bool gui_menu_is_open(const gui_menu_bar_t *bar) {
    return bar && bar->open >= 0;
}

#endif // _GUI_MENU_H
