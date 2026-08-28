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
    // (#307 terminal menu bar) Draw a check/radio mark in the row's left
    // gutter. Added because the Terminal's Settings menu lists the installed
    // colour schemes and one of them is the CURRENT one; the alternative was a
    // "* " prefix baked into every label, which is a private workaround in one
    // app for something every menu with a mutually-exclusive list needs.
    // TRAILING FIELD ON PURPOSE: every existing brace-initialised table (the
    // Editor's four) keeps compiling and zero-initialises this to false, and
    // the mark is drawn inside the existing MENU_PAD_X gutter, so no popup
    // changes width and no already-adopted menu changes appearance.
    bool        checked;
} gui_menu_item_t;

// One top-level menu ("File", "Edit", ...) and its item table.
typedef struct {
    const char            *label;
    const gui_menu_item_t *items;
    int                    item_count;
} gui_menu_t;

#define GUI_MENU_MAX_TOP      16   // top-level menu cap (File/Edit/View/...)
#define GUI_MENU_MAX_VISIBLE  12   // rows shown before the popup scrolls

// ---------------------------------------------------------------------------
// EVERY METRIC BELOW IS A FALLBACK, NOT THE VALUE. The live value comes from
// the ACTIVE THEME and is resolved once per bar in gui_menu_bar_init() (and
// again on gui_menu_sync_theme()). These constants are what a bar uses when the
// running kernel does not know the metric id at all - theme_metric() returns 0
// for an unknown id, and 0 is not a legal row height or type size.
//
// WHY THE THEME AND NOT A CONSTANT. docs/UI_STYLE_GUIDE.md 4.3's own acceptance
// test is "edit a value in a .mtheme on a booted system and see the UI change,
// having built nothing", and its audit found that test holds at exactly TWO
// call sites in the entire shipped userland, both in Settings. Every other
// surface compiles its type size in. A shared widget is the highest-leverage
// place to fix that: one read here themes every app that adopts the widget,
// including apps that do not exist yet.
// ---------------------------------------------------------------------------

// Popup row height. metric.menu_row_h: 22 on retro/modern themes, 28 on the
// maytera pair. UNIFORM within one popup, deliberately: the popup scrolls by
// indexing rows at a fixed height (gui_list), which is what stops a long menu
// silently losing entries off the bottom. Variable row heights would trade that
// guarantee for a shorter separator.
#define GUI_MENU_ITEM_H       22

// Type size for every string this widget draws: the top-level bar labels, the
// popup rows and the right-aligned accelerator hints. One size for the whole
// widget, so a dropdown reads as belonging to the bar that opened it.
// metric type.body = 14 in every shipped theme.
//
// THESE ARE RENDERER SIZE UNITS, NOT CSS PIXELS, AND THE DIFFERENCE IS 14%.
// kernel/gui/ttf.c scales with stbtt_ScaleForPixelHeight(), which maps
// (ascent - descent) onto the requested size, NOT the em square. Read from the
// shipped /FONT.TTF: DejaVu Sans has unitsPerEm 2048, ascent 1901, descent
// -483, so one size unit is 0.859 em px. Size 14 is therefore em 12.03px with a
// cap height of 8.77px - which is exactly the "font-size:12px" the terminal
// chrome spec's mockup asks for. The two house documents that looked like they
// disagreed were describing the same type in two unit systems. A constant
// picked by eye in a browser mock renders 14% small in the guest.
#define GUI_MENU_FONT_SIZE    14

// Side padding of a TOP-LEVEL label in the closed bar. metric.gap = 8 in every
// shipped theme. Nothing is ever drawn in it, so it is free to be tight, and
// tight is what a menu bar wants: it is the densest text row the OS has.
#define GUI_MENU_BAR_PAD      8


// Caller-resolved colors (from the app's own theme/palette source - this
// widget does not assume the kernel THEME_COLOR_* system, so it can match
// an app's existing bespoke chrome, e.g. the Editor's VSCode-style palette,
// exactly as it looked before adoption).
typedef struct {
    uint32_t bar_bg;
    uint32_t bar_text;
    // HOVER AND OPEN ARE DIFFERENT FACTS AND USED TO SHARE ONE TOKEN. A label
    // under the pointer and a label whose menu is currently down looked
    // identical, which is the one distinction a user reads constantly while
    // walking a menu bar. bar_hover_bg is now the closed-bar hover only;
    // bar_open_bg/bar_open_text are the open state and default to the theme
    // accent with contrast-corrected ink.
    uint32_t bar_hover_bg;        // top label: hovered, no menu open
    uint32_t bar_open_bg;         // top label: ITS menu is open
    uint32_t bar_open_text;
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

    // --- resolved from the ACTIVE THEME, once, in gui_menu_bar_init() -------
    // Cached rather than read per frame: a theme_metric() is a syscall, the
    // draw path touches these once per label per frame, and none of them can
    // change while the bar exists. gui_menu_sync_theme() re-reads them (and the
    // palette) when the app's theme changes live.
    int font_size;                     // type.body
    int row_h;                         // metric.menu_row_h
    int pad_x;                         // metric.gap - closed-bar label padding
    int text_h;                        // MEASURED (ascent - descent) at
                                       // font_size. win_draw_text_ttf()'s y is
                                       // the TOP of the line and the baseline
                                       // is y + ascent, so centring a label
                                       // means centring ascent-descent, not the
                                       // nominal size.

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

// Fill `out` from a theme's own menu tokens, every ink already passed through
// gui_ensure_contrast() against the surface it will actually sit on.
// theme_index < 0 means the LIVE active theme; pass a real index for an app
// that tracks a theme other than the live one (the Terminal does).
//
// CONTRAST CORRECTION IS NOT DEFENSIVE PROGRAMMING HERE, A SHIPPED THEME NEEDS
// IT: modern_light.mtheme sets state.item_hover_bg = #007AFF with
// state.item_hover_fg = #1D1D1F, i.e. near-black on saturated blue. Any widget
// that trusts that pair renders an unreadable hovered row. Doing it once, in
// the widget, means no adopting app has to know.
void gui_menu_palette_theme(gui_menu_palette_t *out, int theme_index);

// Re-read BOTH the metrics and the palette after a live theme change, keeping
// the bar's rect, its menus and its open/hot state. Re-lays the label boxes,
// because the type size may have moved. Call this instead of a second
// gui_menu_bar_init() on a theme change.
void gui_menu_sync_theme(gui_menu_bar_t *bar, int theme_index);

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

// (#307) Open the top-level menu whose label STARTS WITH `letter`
// (case-insensitive), the classic Alt+F / Alt+E menu-bar mnemonic. Returns 1
// if a menu was opened (the caller should redraw and must swallow the key), 0
// if no label matches, in which case nothing changed and the key is still the
// caller's.
//
// WHY THE FIRST LETTER AND NOT AN "&F" MNEMONIC MARKER: an ampersand marker
// would have to be stripped at draw time in the widget AND understood by every
// existing caller's label strings, and nothing in this tree draws underlined
// mnemonics today. First-letter matching needs no change to any existing table
// and is unambiguous for every menu bar currently shipped (File/Edit/Search/
// Font in Editor; File/Edit/View/Bookmarks/Settings/Help in Terminal). If a
// future bar has two menus starting with the same letter, the FIRST wins and
// that is the point at which a real mnemonic marker earns its cost.
int gui_menu_alt_open(gui_menu_bar_t *bar, int letter);

void gui_menu_close(gui_menu_bar_t *bar);

// The pointer has left this window, or the window lost focus. Closes any open
// menu AND clears the hover highlight.
//
// WHY IT IS NEEDED AT ALL: a window stops receiving motion events the moment
// the pointer leaves it, so `hot_top` keeps whatever it held when the pointer
// crossed the edge and the bar goes on drawing a hovered label indefinitely.
// MEASURED: with the pointer moved onto the Terminal Profiles dialog (a
// different window), the terminal's menu bar kept a highlight under the last
// label the pointer had touched. There is no mouse-leave event in this
// protocol; EVENT_WINDOW_BLUR is the available signal, and an app should call
// this from it. Motion inside the window already clears it correctly.
void gui_menu_leave(gui_menu_bar_t *bar);
static inline bool gui_menu_is_open(const gui_menu_bar_t *bar) {
    return bar && bar->open >= 0;
}

#endif // _GUI_MENU_H
