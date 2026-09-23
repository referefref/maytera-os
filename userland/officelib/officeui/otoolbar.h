// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// otoolbar.h - the office toolbar/ribbon widget (docs/OFFICE_UI_DESIGN.md
// 3.1 / 3.2 / 8.2). An item table (icon button, toggle, separator, combo slot,
// spacer), hit-testing, press/release/motion/leave/tick, whole-group overflow
// into a MORE control, checked-toggle rendering (pressed bevel + accent bar),
// disabled state, hover tooltip after 600 ms.
//
// Geometry (retro values): bar height = btn_h + 16 (40); controls at
// y = bar_y + 8; icon button 26 x btn_h with a 16 px glyph centred; 2 px
// between buttons in a group; group separator = 8 gap + 1 px rule + 8 gap
// (17); combos are input_h tall and get 4 px of clear either side. Left/right
// inset 8. When the required width exceeds the bar, whole groups are hidden
// from the RIGHT and a MORE icon button is appended whose popup is a gui_list
// of the hidden items by label. Never draw a control that does not fit.
//
// Face: gui_button(GUI_BTN_SECONDARY) in the matching gui_state_t (exactly
// files_icon_btn()), glyph via oicon_draw() tinted to on_surface floored at
// 3:1 against the face; missing icon = first letter of the tooltip, bold.
//
// Return contract: >=0 the activated item id, -1 not consumed, -2 consumed
// (redraw). For a combo activation the committed index is in tb->last_value
// (or -1 with the typed text in the combo's text[] when it was typed).
#ifndef OFFICE_OTOOLBAR_H
#define OFFICE_OTOOLBAR_H

#include "ocombo.h"

typedef enum { OTB_BUTTON = 0, OTB_TOGGLE, OTB_SEPARATOR, OTB_COMBO, OTB_SPACER } otb_kind_t;

typedef struct {
    otb_kind_t  kind;
    int         id;          // app-defined, >0; delivered as on_event(app, OAPP_EVENT_TOOL, id, value)
    const char *icon;        // /ICONS basename, NULL for OTB_COMBO/SEPARATOR
    const char *tooltip;     // also the fallback letter source; NULL for SEPARATOR
    int         w;           // OTB_COMBO/OTB_SPACER width in px; 0 = default (120 / 8)
    int         group;       // toggles sharing a group>0 behave as radio buttons
} otb_item_t;

#define OTB_MAX_ITEMS   40
#define OTB_MAX_COMBOS  4
#define OTB_BTN_W       26
#define OTB_GLYPH       16
#define OTB_TOOLTIP_MS  600

typedef struct otb_toolbar {
    const otb_item_t *items; int n;
    int x, y, w, h;                      // bar rect (h = otb_height())
    int item_x[OTB_MAX_ITEMS], item_w[OTB_MAX_ITEMS], visible[OTB_MAX_ITEMS];
    unsigned char enabled[OTB_MAX_ITEMS], checked[OTB_MAX_ITEMS];
    int combo_slot[OTB_MAX_ITEMS];       // index into combos[] for OTB_COMBO items, else -1
    ocombo_t combos[OTB_MAX_COMBOS]; int ncombos;
    int hot, pressed;                    // hovered / mouse-down item index, -1 none
    int overflow_from;                   // first hidden item index, -1 none
    int more_x;                          // MORE button x when overflowing, else -1
    int more_hot, more_pressed;          // MORE button hover / press
    int more_open;                       // MORE popup open
    gui_list_t more_list; int more_row_hot;
    int more_map[OTB_MAX_ITEMS]; int more_n;   // popup row -> item index
    int tooltip_item; unsigned long long hover_since_ms; int tooltip_shown;
    int focus;                           // keyboard-focused item, -1 (Alt+T walk)
    int open_combo;                      // item index whose popup is open, -1
    int last_value;                      // value of the last activation (see header)
    int focus_pressed;                   // Space held on the focused item
} otb_toolbar_t;

int  otb_height(void);                                   // theme btn_h + 16
void otb_init(otb_toolbar_t *tb, const otb_item_t *items, int n, int x, int y, int w);
void otb_layout(otb_toolbar_t *tb, int w);               // on resize; recomputes overflow
void otb_move(otb_toolbar_t *tb, int x, int y, int w);   // reposition + layout (the shell uses it)
void otb_set_enabled(otb_toolbar_t *tb, int id, int enabled);
void otb_set_checked(otb_toolbar_t *tb, int id, int checked); // radio groups uncheck siblings
int  otb_is_checked(const otb_toolbar_t *tb, int id);
void otb_set_combo_text(otb_toolbar_t *tb, int id, const char *text);
// The ocombo_t behind an OTB_COMBO item, for the app to ocombo_init()'s
// count/sel/label/editable (its rect is owned by the toolbar's layout).
ocombo_t *otb_combo(otb_toolbar_t *tb, int id);
int  otb_item_rect(const otb_toolbar_t *tb, int id, int *x, int *y, int *w, int *h);
void otb_draw(int win, otb_toolbar_t *tb);                // bar + items
void otb_draw_overlay(int win, otb_toolbar_t *tb, int win_w, int win_h); // tooltip, MORE popup, open combo popup: call LAST
// Input. Returns id (>0) on activation, -1 not consumed, -2 consumed (redraw).
int  otb_press(otb_toolbar_t *tb, int mx, int my, int win_w, int win_h);
int  otb_release(otb_toolbar_t *tb, int mx, int my);     // activation happens here
int  otb_motion(otb_toolbar_t *tb, int mx, int my);      // 1 = redraw (hover/tooltip)
int  otb_wheel(otb_toolbar_t *tb, int mx, int my, int delta);
int  otb_key(otb_toolbar_t *tb, unsigned int keycode, char key_char, int win_w, int win_h);
void otb_focus_enter(otb_toolbar_t *tb);                 // Alt+T: keyboard focus onto the bar
void otb_leave(otb_toolbar_t *tb);                       // on EVENT_WINDOW_BLUR
int  otb_tick(otb_toolbar_t *tb, unsigned long long now_ms); // 1 when the tooltip should appear
// 1 while a popup (MORE or a combo) is open and must swallow input.
int  otb_popup_open(const otb_toolbar_t *tb);
// Milliseconds until the tooltip timer fires, or -1 when no timer is armed
// (the shell uses this as its event-wait timeout: no polling).
int  otb_timer_ms(const otb_toolbar_t *tb, unsigned long long now_ms);

#endif // OFFICE_OTOOLBAR_H
