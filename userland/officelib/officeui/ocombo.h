// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// ocombo.h - THE dropdown combo for the office apps (docs/OFFICE_UI_DESIGN.md
// 3.3 / 8.3). No shared dropdown primitive exists in libc (UI_STYLE_GUIDE:
// two apps hand-roll one); this is the one the office toolbar (font family,
// font size) and every OM_COMBO modal field use. Built on gui_list.h so the
// popup scrolls with a real scrollbar, wheel and keys instead of truncating.
//
// Closed box: gui_textfield2 look, h = input_h, text body 14 at +6, a
// gui_chevron(GUI_CHEV_DOWN) centred in an 18 px cap at the right.
// Open: a popup gui_list directly below (above if it would leave the window),
// width = max(box width, 120), rows menu_row_h, max 8 visible then scrolls,
// ground menu_bg, border menu_border, hover row item_hover_bg/fg (contrast
// repaired as gui_menu_palette_theme does), the current value row checked, a
// right-aligned "N items" caption footer when scrollable. Keyboard: Up/Down,
// PgUp/PgDn, Home/End, letter jump, Enter commits, Esc reverts and closes. The
// popup closes only on Enter/Esc/row click/click on the box; a click elsewhere
// closes it WITHOUT changing the value and is consumed (-2), like gui_menu.
//
// Return contract (shared with the rest of the toolkit): >=0 a committed
// index, -1 not consumed, -2 consumed (redraw), OCOMBO_TEXT typed text was
// committed on an editable combo (c->text holds it, c->sel is -1).
//
// This header pulls in the libc widget headers it embeds (gui_list_t); it is
// a UI header for the office APPS, which already include gui.h first.
#ifndef OFFICE_OCOMBO_H
#define OFFICE_OCOMBO_H

#include "../../libc/gui.h"
#include "../../libc/gui_list.h"

#define OCOMBO_TEXT      (-3)   // editable: typed text committed (see c->text)
#define OCOMBO_MAX_ROWS  8      // visible rows before the popup scrolls
#define OCOMBO_MIN_POPW  120
#define OCOMBO_TEXT_CAP  40

typedef const char *(*ocombo_label_fn)(void *ctx, int index, char *buf, int cap);

typedef struct {
    int x, y, w, h;             // closed box rect (h = input_h)
    int count, sel;             // sel = committed index, -1 none
    int open, hot;              // popup state; hot = highlighted row
    int editable;               // 1: typed text allowed (font size)
    char text[OCOMBO_TEXT_CAP]; // closed-box text (typed, or label of sel)
    int typed;                  // 1 while `text` holds uncommitted typed input
    gui_list_t list;            // popup rows (menu_row_h), max 8 visible
    ocombo_label_fn label; void *ctx;
    int px, py, pw, ph;         // popup rect while open (list + footer)
    int footer_h;               // "N items" caption band, 0 when not scrollable
} ocombo_t;

void ocombo_init(ocombo_t *c, int x, int y, int w, int count, int sel, int editable,
                 ocombo_label_fn label, void *ctx);
// Move the closed box (the toolbar re-lays it out on resize).
void ocombo_place(ocombo_t *c, int x, int y, int w);
// Change the committed value / closed text without opening.
void ocombo_set_sel(ocombo_t *c, int sel);
void ocombo_set_text(ocombo_t *c, const char *text);
void ocombo_set_count(ocombo_t *c, int count);
// Closed box only (draw in the normal pass). `focused` adds the field's
// focus look; `disabled` dims it.
void ocombo_draw(int win, const ocombo_t *c);
void ocombo_draw_ex(int win, const ocombo_t *c, int focused, int disabled);
// Popup: call LAST in the frame (over the document). No-op when closed.
void ocombo_draw_popup(int win, ocombo_t *c, int win_w, int win_h);
// Input (window-local coordinates). See the return contract above.
int  ocombo_press(ocombo_t *c, int mx, int my, int win_w, int win_h);
int  ocombo_motion(ocombo_t *c, int mx, int my);      // 1 = redraw
int  ocombo_wheel(ocombo_t *c, int mx, int my, int delta);
void ocombo_release(ocombo_t *c);
// Keys. While open: navigation/commit/cancel. While CLOSED (a focused combo in
// a modal): Up/Down step the value (returns the new index), Space opens,
// printable/Backspace edit an editable combo's text, Enter commits typed text
// (index or OCOMBO_TEXT). Anything else: -1.
int  ocombo_key(ocombo_t *c, unsigned int keycode, char key_char, int win_w, int win_h);
void ocombo_open(ocombo_t *c, int win_w, int win_h);
void ocombo_close(ocombo_t *c);
// Hit test on the closed box.
int  ocombo_hit(const ocombo_t *c, int mx, int my);

#endif // OFFICE_OCOMBO_H
