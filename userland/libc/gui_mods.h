// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_mods.h - THE modifier-key state tracker for MayteraOS userland apps
// (#221 phase 0). ONE implementation, in libc, because Editor, Terminal, Files
// and every app with a keyboard shortcut needs it, and a private copy in one
// app is the multi-copy fault this tree keeps paying for (#188/#191/#243 were
// all one constant copied into six apps).
//
// ===========================================================================
// THE QUESTION THIS FILE ANSWERS, AND HOW IT WAS ANSWERED
// ===========================================================================
//
// "Can a userland app know whether Shift/Ctrl/Alt is held?"
//
// The tree said no. userland/apps/editor/main.c carried, in a comment, "the
// kernel gui_event_t has no shift/modifier field, so shift+arrow cannot be
// detected from userland", and built a whole Ctrl+Space selection mode around
// that belief. The first half is true - there is no modifier field on
// gui_event_t. The conclusion drawn from it is FALSE.
//
// MEASURED on VM <vmid>, golden build 2040, keystrokes injected over the #334
// serial channel into a Ring-3 probe (tools/testing/probes/keyprobe.c) that
// printed every event it received. Not read out of the source; typed:
//
//   LShift        EVENT_KEY_DOWN 0x95         EVENT_KEY_UP 0x87
//   RShift        EVENT_KEY_DOWN 0x96         EVENT_KEY_UP 0x88
//   LCtrl         EVENT_KEY_DOWN 0x99         EVENT_KEY_UP 0x84
//   Alt           EVENT_KEY_DOWN 0x9A         EVENT_KEY_UP 0x1C
//   Shift+a       LSHIFT down, 'A' (0x41), 'A' up, LSHIFT up
//   Ctrl+c        LCTRL down, 0x03, 'c' up, LCTRL up
//   Ctrl+Shift+c  LCTRL down, LSHIFT down, 'C' (0x43), 'C' up, LSHIFT up, LCTRL up
//   Shift+Up      LSHIFT down, UP (0x80), UP release, LSHIFT up
//   Super         EVENT_KEY_DOWN 0x9B, and NO release, ever
//
// So: modifiers are fully observable, and shift+arrow in particular is
// observable, which is exactly what the Editor comment said was impossible.
//
// ===========================================================================
// WHY YOU CANNOT JUST LOOK AT THE CHARACTER
// ===========================================================================
//
// Ctrl+c arrives as the control character 0x03. Ctrl+SHIFT+c arrives as an
// ordinary capital 'C' - IDENTICAL to plain Shift+c - because cpu/isr.c only
// folds Ctrl into a control character when the post-case-fold character is
// lowercase. That is the entire reason the Konsole-style `Ctrl+Shift+<letter>`
// scheme needs this file: the character alone cannot tell Ctrl+Shift+C from
// Shift+C, and only the tracked modifier state can.
//
// ===========================================================================
// WHY THE TRACKING IS EXACT, AND THE ONE PLACE IT IS NOT
// ===========================================================================
//
// EXACT: the modifier press/release events and the key they modify travel in
// ONE ordered per-window queue. When you dequeue 'C', the modifier events that
// preceded it in that queue are exactly the modifiers that were held when the
// kernel cooked it. There is no race to lose. This is why gui_mods tracks
// events rather than polling: a poll answers "is Shift down NOW", and NOW is
// not when your event was made.
//
// NOT EXACT, in exactly one situation: the kernel emits NO focus and NO blur
// event to an app. EVENT_WINDOW_BLUR appears once in the whole kernel tree and
// that one hit is the enum declaration (verified; four userland apps handle an
// event that can never arrive). So if the user holds Shift, clicks another
// window, and releases Shift there, this window never sees the release and
// would believe Shift is held forever.
//
// gui_mods_resync() closes that, using SYS_KEY_MODS (the live physical state
// the ISR itself keeps). It is safe to call ONLY at a moment when this
// window's event queue is empty, because only then can a live reading not
// contradict an event still sitting in the queue. gui_mods_next_event() finds
// that moment for you and calls it. If you use raw win_get_event() with an
// INFINITE timeout you never reach a proven-quiescent moment, so call
// gui_mods_resync() yourself at a natural idle point; otherwise a stale held
// bit can survive until the next time that modifier is pressed and released
// with this window focused.
//
// ===========================================================================
// TWO THINGS THIS FILE DELIBERATELY DOES NOT OFFER
// ===========================================================================
//
// SUPER IS NOT A TRACKABLE MODIFIER. It has a press code (0x9B) and NO release
// code at all - measured above, and cpu/isr.h says so. A held-state bit for it
// could only ever get stuck. The compositor's own Super+L uses a time window
// for this reason; copy that pattern, do not add a bit here.
//
// CAPS LOCK IS NOT AN EVENT. cpu/isr.c toggles caps_lock and pushes nothing,
// so no amount of event tracking recovers it. GUI_MOD_CAPS is therefore only
// ever set by gui_mods_resync(), which reads it from the kernel. Until the
// first resync it reads 0, which is not the same as "Caps Lock is off".
#ifndef MAYTERA_LIBC_GUI_MODS_H
#define MAYTERA_LIBC_GUI_MODS_H

// INCLUDE gui.h, NOT THIS FILE. gui_event_t is an anonymous-struct typedef in
// gui.h, so it cannot be forward-declared here; gui.h includes this header
// immediately after defining it, which is why every app that includes gui.h
// (or maytera.h) already has the whole API below. Including gui_mods.h on its
// own will not compile, and that is the honest failure rather than a second
// declaration of gui_event_t that could drift from the kernel's.
#include "types.h"

// ---------------------------------------------------------------------------
// The bitmask. The low four bits are DELIBERATELY the same values as the
// kernel's KEY_MOD_SHIFT/CTRL/ALT/CAPS (kernel/drivers/keymod.h), so
// gui_mods_resync() needs no translation table and no translation table can
// therefore drift. The two above them have no kernel twin: the kernel keeps
// one shift flag, not two.
// ---------------------------------------------------------------------------
#define GUI_MOD_SHIFT   0x01u   // EITHER shift. Use this one.
#define GUI_MOD_CTRL    0x02u   // either Ctrl (both keys map to one code)
#define GUI_MOD_ALT     0x04u   // either Alt (both keys map to one code)
#define GUI_MOD_CAPS    0x08u   // LATCHED, not held. Only ever from a resync.
#define GUI_MOD_LSHIFT  0x10u   // best effort: a resync cannot tell L from R
#define GUI_MOD_RSHIFT  0x20u   // best effort: a resync cannot tell L from R
// THESE TWO OVERLAP KEY_MOD_NUM (0x10) AND KEY_MOD_SCROLL (0x20) IN THE
// KERNEL HEADER. Nothing in the kernel ever sets those two bits today, and
// gui_mods_resync() masks the syscall result down to SHIFT|CTRL|ALT|CAPS
// before using it, so the overlap is inert BY THAT MASK and by nothing else.
// If you ever widen that mask, move these two bits first.

// The three bits a keyboard shortcut is allowed to be about.
#define GUI_MOD_CHORD   (GUI_MOD_SHIFT | GUI_MOD_CTRL | GUI_MOD_ALT)

// The modifier state as of the last event fed to the tracker.
unsigned int gui_mods_get(void);

// Feed ONE event. Call this for EVERY event you dequeue, in order, or the
// state is meaningless. Returns 1 if the event was purely a modifier
// transition (so there is nothing else to do with it), 0 otherwise.
int gui_mods_feed(const gui_event_t *ev);

// win_get_event() + gui_mods_feed() + a resync at proven quiescence. This is
// the supported way to drive the tracker. Same return convention as
// win_get_event(): >0 event type, 0 no event, <0 error.
int gui_mods_next_event(int handle, gui_event_t *ev, int timeout);

// Overwrite the tracked state from the kernel's LIVE physical state. Only
// correct at a moment when this window's event queue is empty; see the header
// note. Cheap (one argument-free syscall), never blocks.
void gui_mods_resync(void);

// Forget everything. For an app that knows it has just lost the keyboard.
void gui_mods_clear(void);

// Does the current chord state EXACTLY equal `want`? Exact, not subset:
// gui_mods_is(GUI_MOD_CTRL) is false while Shift is also held, which is what
// keeps Ctrl+C and Ctrl+Shift+C apart. CAPS and the L/R bits are ignored.
int gui_mods_is(unsigned int want);

// The letter the user PHYSICALLY pressed, lowercased, or 0 if this event is
// not a letter. This undoes the kernel's folding, which differs per chord and
// is the second half of the trap:
//     Ctrl+c        key_char 0x03  -> 'c'
//     Ctrl+Shift+c  key_char 'C'   -> 'c'
//     Shift+c       key_char 'C'   -> 'c'
//     Alt+f         key_char 'f'   -> 'f'
// Pair it with gui_mods_is() and a shortcut table reads the way it should:
//     if (gui_mods_is(GUI_MOD_CTRL|GUI_MOD_SHIFT) && gui_mods_letter(&ev) == 't')
int gui_mods_letter(const gui_event_t *ev);

#endif // MAYTERA_LIBC_GUI_MODS_H
