// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// otabs.h - the Calc sheet-tab strip (docs/OFFICE_UI_DESIGN.md 2.2 / 3.5 / 8.5).
//
// 26 px (TAB_H, the shipped height) on surface_raised with a 1 px rule on top.
// Tabs from x+8: each = text width + 24 (12 pad a side), min 64, max 140
// (ellipsis by measurement), h 26. Active: surface_sunken fill, 2 px accent
// bar along its TOP edge, on_surface label; inactive: surface_raised, muted
// floored label, 1 px border_subtle right edge; hover: btn_hover_bg fill.
// After the last tab an add button 26x26 with the PLUS icon at 16 px. When
// the tabs overflow, CHEVL/CHEVR 26x26 scroll buttons appear at the right end;
// the strip never truncates a tab silently. A rename in progress replaces the
// label with a gui_textfield_tf of the same rect.
//
// Return contract (press/key): >=0 a tab index, OTAB_ADD the add button,
// OTAB_PREV/OTAB_NEXT (Ctrl+PgUp/PgDn), OTAB_RENAMED (rename committed, name
// in rename_buf, tab in `renaming_tab`), -1 not consumed, -2 consumed (redraw).
#ifndef OFFICE_OTABS_H
#define OFFICE_OTABS_H

#include "../../libc/gui.h"

#define OTAB_H        26
#define OTAB_ADD      (-3)
#define OTAB_PREV     (-4)
#define OTAB_NEXT     (-5)
#define OTAB_RENAMED  (-6)
#define OTAB_MAX      64
#define OTAB_NAME_MAX 32

typedef const char *(*otabs_label_fn)(void *ctx, int index);

typedef struct {
    int x, y, w, h;                 // h = OTAB_H
    int count, active, hot;         // hot: hovered tab, OTAB_ADD, or -1
    int first_visible;              // overflow scroll
    int tab_x[OTAB_MAX], tab_w[OTAB_MAX];
    int last_visible;               // last tab index that fits (inclusive)
    int add_x;                      // add button x, -1 when it does not fit
    int chev_x;                     // CHEVL x when overflowing (CHEVR at +26), else -1
    int renaming;                   // 1 while a rename field is live
    int renaming_tab;
    textfield_t rename_tf; char rename_buf[OTAB_NAME_MAX];
    int pressed;                    // pressed target (tab index / OTAB_ADD / -10 prev / -11 next), -1
} otabs_t;

void otabs_init(otabs_t *t, int x, int y, int w);
void otabs_move(otabs_t *t, int x, int y, int w);
// Lay the tabs out (measures labels). Call when count/active/labels change and
// on resize; safe every frame.
void otabs_layout(otabs_t *t, int count, int active, otabs_label_fn label, void *ctx);
void otabs_draw(int win, otabs_t *t, otabs_label_fn label, void *ctx);
int  otabs_press(otabs_t *t, int mx, int my);      // >=0 tab, OTAB_ADD, -1 miss, -2 consumed (scroll chevrons)
int  otabs_release(otabs_t *t, int mx, int my);
int  otabs_dblclick(otabs_t *t, int mx, int my);   // >=0 starts a rename of that tab
int  otabs_motion(otabs_t *t, int mx, int my);     // 1 = redraw
void otabs_leave(otabs_t *t);
// Keys: Ctrl+PgUp/PgDn -> OTAB_PREV/OTAB_NEXT (mods = GUI_MOD_* held); while
// renaming: editing keys, Enter -> OTAB_RENAMED, Esc cancels (-2).
int  otabs_key(otabs_t *t, unsigned int keycode, char key_char, int mods);
// Start a rename of `index` programmatically (Format > Rename Sheet...).
void otabs_begin_rename(otabs_t *t, int index, const char *current);
void otabs_cancel_rename(otabs_t *t);

#endif // OFFICE_OTABS_H
