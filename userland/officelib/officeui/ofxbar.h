// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// ofxbar.h - the Calc formula/name bar (docs/OFFICE_UI_DESIGN.md 2.2 / 3.8 / 8.7).
//
// Strip h = input_h + 4 (28 retro) on surface_raised with a 1 px rule at the
// bottom. Name box at x+8, w 88, h input_h, gui_textfield2 look, shows "A1"
// (or "A1:C4"), editable (typing a ref + Enter jumps). SCFX icon button at
// x+100 (26 x btn_h). Formula field from x+130 to w-8, gui_textfield_tf with
// the cell's raw content; Enter commits and moves down, Tab commits and moves
// right, Esc reverts.
#ifndef OFFICE_OFXBAR_H
#define OFFICE_OFXBAR_H

#include "../../libc/gui.h"

#define OFX_NAME_W   88
#define OFX_FX_X     100
#define OFX_FIELD_X  130
#define OFX_FOCUS_NONE 0
#define OFX_FOCUS_NAME 1
#define OFX_FOCUS_FX   2
#define OFX_PRESS_FXBTN 3

typedef struct {
    int x, y, w, h;                     // strip rect, h = input_h + 4
    textfield_t name_tf; char name_buf[24];
    textfield_t fx_tf;   char fx_buf[512];
    int focus;                          // OFX_FOCUS_NONE / _NAME / _FX
    int fx_hot, fx_pressed;             // SCFX button states
    char fx_revert[512];                // formula as set, for Esc
} ofxbar_t;

int  ofxbar_height(void);               // input_h + 4
void ofxbar_init(ofxbar_t *f, int x, int y, int w);
void ofxbar_move(ofxbar_t *f, int x, int y, int w);
void ofxbar_set(ofxbar_t *f, const char *ref, const char *formula);   // from the app (selection moved)
void ofxbar_draw(int win, ofxbar_t *f);
int  ofxbar_press(ofxbar_t *f, int mx, int my);   // 1 name, 2 field, 3 SCFX button (on press), -1 miss
int  ofxbar_release(ofxbar_t *f, int mx, int my); // 3 when the SCFX button is activated, else 0
int  ofxbar_motion(ofxbar_t *f, int mx, int my);  // 1 = redraw
// Keys while focus != NONE: 1 changed, 2 commit (Enter), 3 commit+right (Tab),
// 4 revert (Esc, focus dropped), 0 not consumed. For the name box, "commit"
// means "jump to name_buf".
int  ofxbar_key(ofxbar_t *f, const gui_event_t *ev);
void ofxbar_blur(ofxbar_t *f);

#endif // OFFICE_OFXBAR_H
