// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// omodal.h - declarative modal dialogs for the office apps
// (docs/OFFICE_UI_DESIGN.md 3.4 / 8.4).
//
// Same overlay technique and card grammar as the shipped gui_confirm card and
// the shell's Open/Save dialog: never a separate window, never click-away
// dismissable, drawn over the interlaced-scanline scrim of the app's own
// window, centred within it. BLOCKING: each call runs its own nested event
// loop on the app window (repainting the app underneath via the shell's last
// on_draw) and returns when the user resolves it.
//
// Geometry (fixed, from gui_confirm.c so both card families are one family):
// card width S 360 / M 480 / L 560; height = 73 + body_h + 16 + 60; title
// heading 20 bold at (16,16); rule at y=56; body origin (16,73); label column
// 120 px, field column from x=144 to w-16; row pitch 32; footer band 60 with
// buttons h 28, min w 96, pad 16, gap 8, right inset 16, order [third]
// Cancel OK; OK is GUI_BTN_PRIMARY with a bold label (GUI_BTN_DANGER when
// ok_is_danger, which then never has initial focus). Focus ring 2 px at 2 px.
//
// Behaviour: initial focus on the first field (OK when there are none, Cancel
// when OK is danger); Tab/Shift+Tab cycle fields then [third] Cancel OK; Enter
// = OK unless a button has focus (then that button); Esc = Cancel; Space
// toggles a check/radio, opens a combo, activates a focused button; Up/Down
// and Left/Right walk a radio group, a list or a closed combo; settle 250 ms;
// a NUMBER outside [min,max] turns its border danger, shows "min..max" and
// disables OK; window close while open = Cancel; resize recentres.
//
// VALUES: fields are edited IN PLACE (the caller's buf/*sel/*checked) so
// on_change()/preview() see the live values; on Cancel the entry values are
// restored, so the caller's view is "out-pointers written on OK, untouched
// on Cancel".
//
// Header is self-sufficient (raw C types only).
#ifndef OFFICE_OMODAL_H
#define OFFICE_OMODAL_H

#include "../include/officeui.h"

typedef enum { OM_TEXT = 0, OM_NUMBER, OM_COMBO, OM_CHECK, OM_RADIO, OM_LIST, OM_LABEL } om_kind_t;

typedef struct {
    om_kind_t   kind;
    const char *label;                 // left column, body 14; NULL = field spans both columns.
                                       // OM_CHECK: drawn INLINE beside the box (field column).
                                       // OM_LABEL: the left-column caption; `buf` is the right-column text.
    char *buf; int cap;                // OM_TEXT/OM_NUMBER: caller buffer (in/out); OM_LABEL: text
    int  min, max;                     // OM_NUMBER range (validated live)
    const char *const *options; int n_options; int *sel;   // OM_COMBO/OM_RADIO/OM_LIST (in/out)
    int *checked;                      // OM_CHECK (in/out)
    int  list_rows;                    // OM_LIST visible rows (default 6)
    int  half;                         // 1: share the row with the next field (two-column row)
} om_field_t;

#define OM_SIZE_S 360
#define OM_SIZE_M 480
#define OM_SIZE_L 560
#define OM_MAX_FIELDS 24

typedef struct {
    const char *title;
    int size;                          // OM_SIZE_S / OM_SIZE_M / OM_SIZE_L (a raw width is accepted)
    const om_field_t *fields; int n_fields;
    const char *ok_label;              // NULL = "OK"; drawn body_strong, GUI_BTN_PRIMARY
    const char *cancel_label;          // NULL = "Cancel"
    const char *third_label;           // optional tertiary (e.g. "Save"), GUI_BTN_SECONDARY, left of Cancel
    int  ok_is_danger;                 // 1: OK is GUI_BTN_DANGER and never has initial focus
    int  preview_h;                    // 0 = none; else a pane below the rows
    void (*preview)(int win, int x, int y, int w, int h, void *ctx);   // draws the pane
    void (*on_change)(void *ctx);      // called after any field edit (live preview)
    void *ctx;
} om_spec_t;

#define OM_CANCEL 0
#define OM_OK     1
#define OM_THIRD  2

int  omodal_run(office_app *a, const om_spec_t *spec);

// Free-form body (Slide Layout picker): the toolkit owns card, title, footer,
// buttons, focus ring and keys; the app draws and hit-tests the body rect
// (bx,by,bw,bh in window coordinates). key() sees every key first and returns
// 1 to consume it; Tab/Enter/Esc then fall to the buttons.
typedef struct {
    void (*draw)(int win, int bx, int by, int bw, int bh, void *ctx);
    int  (*press)(int bx, int by, int mx, int my, void *ctx);          // 1 = redraw
    int  (*key)(unsigned int keycode, char key_char, void *ctx);       // 1 = consumed
    void *ctx;
} om_custom_t;
int  omodal_run_custom(office_app *a, const char *title, int size, int body_h,
                       const char *ok_label, const char *cancel_label, const om_custom_t *body);

// Three-button unsaved-changes prompt (Save / Discard / Cancel) in the same
// card grammar as gui_confirm; body is wrapped by measurement. Initial focus:
// Cancel. Returns OM_THIRD for Save, OM_OK for Discard (danger), OM_CANCEL.
int  omodal_confirm3(office_app *a, const char *title, const char *body,
                     const char *save_label, const char *discard_label);

#endif // OFFICE_OMODAL_H
