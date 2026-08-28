// confirmdialog.h - THE shared system-modal confirm/notice card.
//
// docs/CONFIRM_MODAL_DESIGN.html (86f3cea): the audit that produced this file
// found SIX independent hand-rolled "are you sure" dialogs in the tree and no
// shared component behind any of them - see the design doc section 1. This is
// that shared component's compositor-side half (system-modal: the compositor
// draws it straight to the framebuffer and owns the whole screen, the same
// way lock_enter()/elevate.c's prompt do). The app-window half (Files, Task
// Manager, Settings, ...) is userland/libc/gui_style.h's gui_confirm_t -
// same geometry and token names, different backend, because an app cannot
// grab the whole screen (see the design doc section 6).
//
// This file replaces startmenu.c's old sm_confirm_rect/sm_power_confirm_yes
// hand-rolled box (~1044-1145) and is now also used by taskbar.c's Force Quit
// confirm. Two real defects that shipped in that old code are fixed here,
// not restyled around:
//   1. No focus concept at all -> Enter unconditionally fired the destructive
//      action. Fixed: a real focus field, initial focus on Cancel for the
//      destructive variant (elevate.c's proven rule - a buffered Enter before
//      the dialog opens can only cancel, never approve).
//   2. No input-settle timer -> a buffered/fast Enter could chain straight
//      into confirming. Fixed: CONFIRM_SETTLE_MS, verbatim copy of
//      elevate.c's ELEV_SETTLE_MS=250 mechanism.
#ifndef CONFIRMDIALOG_H
#define CONFIRMDIALOG_H

#include <stdint.h>
#include "compositor.h"   // #204: UI_WRAP_COL (CONFIRM_LINE_MAX aliases it below).
                          // Guarded by COMPOSITOR_H, safe if the includer already has it.

// The three content variants the audit found a real need for (design doc
// section 2.2). Do not add a fourth without updating the design doc first -
// "do not invent variants nothing uses" is the doc's own rule.
typedef enum {
    CONFIRM_DESTRUCTIVE = 0,   // Cancel=standard, Action=danger. Initial focus: Cancel.
    CONFIRM_NEUTRAL     = 1,   // Cancel=standard, Action=primary. Initial focus: Action.
    CONFIRM_NOTICE      = 2    // Single "OK"=primary, no Cancel. Initial focus: OK.
} confirm_variant_t;

#define CONFIRM_MAX_LINES 3
// #204: must equal compositor.h's UI_WRAP_COL - this is the shared
// wrap_text_ttf() buffer stride (see confirmdialog.c), not an independent
// number, so there is nothing left to drift out of sync.
#define CONFIRM_LINE_MAX  UI_WRAP_COL

typedef struct {
    int open;
    confirm_variant_t variant;
    char title[64];
    char lines[CONFIRM_MAX_LINES][CONFIRM_LINE_MAX];
    int  n_lines;              // 1..CONFIRM_MAX_LINES, the REAL wrapped count
    char cancel_label[24];
    char action_label[24];     // also the NOTICE dialog's single button label
    int  focus;                 // 0 = Cancel/left, 1 = Action/right (NOTICE: unused, always resolves the one control)
    unsigned long long shown_ms;
} confirm_dialog_t;

// #204 CORRECTED: this used to say body copy is "pre-wrapped by the CALLER
// ... author discipline, not a runtime wrap rule; every call site in this
// port has short, known-ahead-of-time copy" - that was never actually true.
// All 7 real call sites (Shut Down/Restart/Log Out/Lock, Force Quit, Recycle
// Bin Delete/Empty, End Task/Kill) passed ONE whole unwrapped sentence as a
// single n_lines=1 line, measured at 474-727px against this card's 328px
// body width (DejaVu Sans body text, 14px - docs/CONFIRM_MODAL_DESIGN.html
// section 8) - i.e. every one of them always overflowed the card, with or
// without a variable name in it. confirm_dialog_open() now wraps `lines`/
// `n_lines` FOR REAL (joins them with a space, then reuses notif.c's proven
// wrap_text_ttf(), #762), hard-breaking a single too-wide "word" (e.g. an
// app name with no spaces) and ellipsizing whatever still doesn't fit in
// CONFIRM_MAX_LINES. Callers no longer need to size or split anything
// themselves; passing one long sentence as `lines[0]`/`n_lines=1` (what
// every caller already does) is now the CORRECT usage, not the bug.
void confirm_dialog_open(confirm_dialog_t *d, confirm_variant_t variant,
                         const char *title,
                         const char *const *lines, int n_lines,
                         const char *cancel_label, const char *action_label);
void confirm_dialog_close(confirm_dialog_t *d);
static inline int confirm_dialog_is_open(const confirm_dialog_t *d) { return d->open; }

// Draws the one-time full-screen scrim (design doc section 6: drawn once at
// open, not per frame - safe because the caller already suspends repaint of
// everything under an open system-modal, same as lock screen/elevate.c) plus
// the card. Call once per frame while d->open.
void confirm_dialog_scrim(void);
void confirm_dialog_render(const confirm_dialog_t *d);

// Returns 0 = no decision yet, 1 = Cancel/dismiss, 2 = Action/OK. A non-zero
// result has already closed *d (matches elevate.c's elev_close()-on-decision
// pattern) so the caller only needs to act on the return value.
int confirm_dialog_handle_key(confirm_dialog_t *d, int key);
int confirm_dialog_handle_mouse(confirm_dialog_t *d, int32_t x, int32_t y, int clicked);

#endif // CONFIRMDIALOG_H
