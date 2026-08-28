// keys.h - THE keycode table for MayteraOS userland apps (#191).
//
// WHAT BROKE, twice, and why this file exists.
//
// An app receives keys as gui_event_t.keycode. That value is NOT a PS/2
// scancode. kernel/cpu/isr.c cooks the scancode first: the four ARROW keys are
// REMAPPED (E0 48/50/4B/4D -> 0x80/0x81/0x82/0x83), while Home/End/Delete/
// PgUp/PgDn/Insert are forwarded as their raw make codes and ordinary keys
// arrive as their ASCII byte. So an app that matches 0x48 for "up" cannot
// ever see an arrow press. Worse, it is not merely dead: 0x48 is ASCII 'H',
// so shift-H silently moves the selection instead.
//
// #188 found the Task Manager had matched 0x48/0x50 since the day it was
// written - arrow navigation had NEVER ONCE WORKED - under a comment claiming
// that handler was what made the app keyboard-navigable. #191 then found the
// same copy in five more apps, one of them (midiplay) with the comment "Taken
// from the same values taskmanager uses". That is the whole mechanism of the
// defect: the constants were declared per-app, so a wrong one propagated by
// being copied, and every copy carried a confident comment.
//
// THE MECHANISM is that there is now exactly ONE declaration of these values
// in userland, here, and build/keycode-gate.sh FAILS the golden build if any
// app declares its own arrow constant or compares a keycode against a raw
// arrow scancode. Divergence is caught by construction, not by review.
//
// Rust apps read the SAME table from userland/libc/keys.rs, which the gate
// checks value-for-value against this file. Do not add a third spelling.
//
// Every value below was read out of kernel/cpu/isr.c + isr.h and out of
// SYS_INJECT_KEY in kernel/proc/syscall.c, not assumed.
#ifndef MAYTERA_LIBC_KEYS_H
#define MAYTERA_LIBC_KEYS_H

// ---------------------------------------------------------------------------
// REMAPPED keys. The kernel replaces the scancode with these codes, so the
// scancode value NEVER reaches an app. Matching the scancode is always a bug.
// ---------------------------------------------------------------------------
#define GUI_KEY_UP     0x80   // NOT 0x48 (0x48 is ASCII 'H')
#define GUI_KEY_DOWN   0x81   // NOT 0x50 (0x50 is ASCII 'P')
#define GUI_KEY_LEFT   0x82   // NOT 0x4B (0x4B is ASCII 'K')
#define GUI_KEY_RIGHT  0x83   // NOT 0x4D (0x4D is ASCII 'M')

// F-keys, also remapped. The numbering is deliberately NOT contiguous: the
// gaps and the out-of-order F10 are historical de-collision moves recorded in
// kernel/cpu/isr.h. Copy them, do not "tidy" them.
#define GUI_KEY_F1     0x88
#define GUI_KEY_F2     0x89
#define GUI_KEY_F3     0x8B
#define GUI_KEY_F4     0x8C
#define GUI_KEY_F5     0x84
#define GUI_KEY_F6     0x8A
#define GUI_KEY_F7     0x8D
#define GUI_KEY_F8     0x8E
#define GUI_KEY_F9     0x8F
#define GUI_KEY_F10    0x87
#define GUI_KEY_F11    0x85
#define GUI_KEY_F12    0x86

// Modifier / system presses. These live at or above 0x90 because 0x80-0x8F is
// fully occupied above; SYS_INJECT_KEY has an explicit case for each so they
// arrive as EVENT_KEY_DOWN rather than being mistaken for a release.
#define GUI_KEY_LSHIFT 0x95
#define GUI_KEY_RSHIFT 0x96
#define GUI_KEY_LCTRL  0x99
#define GUI_KEY_ALT    0x9A
#define GUI_KEY_SUPER  0x9B
#define GUI_KEY_PRTSC  0x9D

// RELEASE codes for the modifiers and arrows. The kernel emits these when the
// key goes UP; they are IRREGULAR (the arrows are press+0x10, the modifiers are
// not) so they cannot be computed and must be declared. Read out of
// kernel/cpu/isr.h, which the gate checks this file against.
//
// #243: these are here because userland genuinely needs them -
// apps/compositor/vnc.c injects a release when an RFB viewer lifts a key, and
// it had been computing them as `press + 0x10`, which is only true for arrows.
#define GUI_KEY_UP_REL     0x90
#define GUI_KEY_DOWN_REL   0x91
#define GUI_KEY_LEFT_REL   0x92
#define GUI_KEY_RIGHT_REL  0x93
#define GUI_KEY_LCTRL_UP   0x94
#define GUI_KEY_LSHIFT_UP  0x97
#define GUI_KEY_RSHIFT_UP  0x98
#define GUI_KEY_ALT_UP     0x9C

// ---------------------------------------------------------------------------
// #221 phase 0: THE VALUES A MODIFIER RELEASE ACTUALLY ARRIVES WITH.
//
// THE FOUR CONSTANTS ABOVE ARE NOT WHAT AN APP SEES. They are what cpu/isr.c
// PUSHES into the cooked ring. SYS_INJECT_KEY (kernel/proc/syscall.c) rewrites
// the value on the way into gui_event_t, so the release an app receives is a
// different number from the one declared above, and matching GUI_KEY_LSHIFT_UP
// against gui_event_t.keycode never fires. Use the four names below.
//
// #232 CORRECTION - THE COLLISION IS GONE, AND THIS ENTRY USED TO DOCUMENT IT
// AS A HAZARD TO WORK AROUND RATHER THAN A BUG TO FIX.
//
// It said, correctly and with a real measurement behind it, that a Right Shift
// RELEASE was delivered as keycode 0x88, which is GUI_KEY_F1's PRESS code; that
// a Left Shift release was 0x87 == GUI_KEY_F10; and that a Left Ctrl release
// was 0x84 == GUI_KEY_F5. It then concluded "that is survivable only because it
// is the TYPE that disambiguates ... ALWAYS match the event type as well as the
// keycode". Survivable is not the same as correct: it made every consumer in
// the tree responsible, forever, for a check whose omission fails SILENTLY as a
// phantom F1/F5/F10 press on an unrelated key release. That is a convention,
// and this project has a long ledger of conventions failing (see
// userland/apps/ide/main.c, whose K_F5 "Run" shortcut sits on exactly the byte
// a Ctrl release used to deliver).
//
// SYS_INJECT_KEY now maps each modifier release back to its OWN press code, so
// EVENT_KEY_UP can no longer carry any F-key's press value at all. The four
// names below keep their _DELIVERED_REL spelling (nothing that matches them
// breaks) and simply equal the press codes now.
//
// MEASURED on VM <vmid> / golden build 2040 with tools/testing/probes/keyprobe.c
// BEFORE the change:
//     press  LShift -> EVENT_KEY_DOWN 0x95   release -> EVENT_KEY_UP 0x87 (== F10!)
//     press  RShift -> EVENT_KEY_DOWN 0x96   release -> EVENT_KEY_UP 0x88 (== F1!)
//     press  LCtrl  -> EVENT_KEY_DOWN 0x99   release -> EVENT_KEY_UP 0x84 (== F5!)
//     press  Alt    -> EVENT_KEY_DOWN 0x9A   release -> EVENT_KEY_UP 0x1C
// AFTER the change every release carries its own press code, as below.
//
// Matching the event type is still GOOD PRACTICE and gui_mods.c still does it;
// it is simply no longer the only thing standing between you and a phantom
// keystroke.
//
// Use libc/gui_mods.h rather than these directly unless you are writing a
// second modifier tracker, in which case do not: there is one.
// ---------------------------------------------------------------------------
#define GUI_KEY_LSHIFT_DELIVERED_REL  0x95  // EVENT_KEY_UP. == GUI_KEY_LSHIFT (#232)
#define GUI_KEY_RSHIFT_DELIVERED_REL  0x96  // EVENT_KEY_UP. == GUI_KEY_RSHIFT (#232)
#define GUI_KEY_LCTRL_DELIVERED_REL   0x99  // EVENT_KEY_UP. == GUI_KEY_LCTRL  (#232)
#define GUI_KEY_ALT_DELIVERED_REL     0x9A  // EVENT_KEY_UP. == GUI_KEY_ALT    (#232)

// ---------------------------------------------------------------------------
// NAVIGATION / EDITING keys. Also REMAPPED, into a block above the byte.
//
// #243, AND THE COMMENT THAT USED TO BE HERE. This block previously read
// "PASS-THROUGH keys. isr.c forwards these as their raw set-1 make code on
// purpose (all below 0x80, so they cannot collide with the block above) ...
// Here the scancode value IS correct - which is exactly what makes the arrow
// case easy to get wrong." Every sentence of that was accurate and the
// conclusion was still false, because "cannot collide with the 0x80 block" is
// not "cannot collide". 0x47 is ASCII 'G'. 0x4F is 'O'. 0x49 is 'I'. 0x51 is
// 'Q'. 0x52 is 'R'. 0x53 is 'S'. So these six keys and those six capital
// letters were THE SAME VALUE, and every app had to pick which one to break:
//   * `echo GIOQ hello` displayed `echo  hello` - the terminal's Home/PgUp/
//     End/PgDn scrollback bindings ate the letters.
//   * `vi /BUILDINFO.TXT` opened /BULDNF.TXT.
//   * PgUp inside vi entered INSERT mode (the pty was handed the letter 'i').
//   * Word 6 could not type a capital G, because exec/win16api.c had resolved
//     the same ambiguity the other way and mapped 0x47 to VK_HOME.
// This is the SAME defect class #188/#191 fixed for the arrows. It survived
// because the byte namespace was full and a comment argued the leftovers were
// safe. build/keycode-gate.sh now FAILS on any non-character key whose value
// lands in printable ASCII, so no future key can be parked there again.
//
// These values come from kernel/cpu/isr.h (KEY_HOME..KEY_DEL) and the gate
// checks this file against that one.
// ---------------------------------------------------------------------------
#define GUI_KEY_HOME   0x100  // NOT 0x47 (0x47 is ASCII 'G')
#define GUI_KEY_END    0x101  // NOT 0x4F (0x4F is ASCII 'O')
#define GUI_KEY_PGUP   0x102  // NOT 0x49 (0x49 is ASCII 'I')
#define GUI_KEY_PGDN   0x103  // NOT 0x51 (0x51 is ASCII 'Q')
#define GUI_KEY_INS    0x104  // NOT 0x52 (0x52 is ASCII 'R')
#define GUI_KEY_DEL    0x105  // NOT 0x53 (0x53 is ASCII 'S')

// ---------------------------------------------------------------------------
// ORDINARY keys arrive as their ASCII byte in BOTH keycode and key_char (see
// the final else in SYS_INJECT_KEY). Match key_char for those; there is no
// scancode involved and none should appear. In particular Escape is 0x1B and
// Enter is 0x0A - NOT the 0x01/0x1C scancodes.
// ---------------------------------------------------------------------------
#define GUI_KEY_ESC    0x1B
#define GUI_KEY_ENTER  0x0A
#define GUI_KEY_TAB    0x09
#define GUI_KEY_BKSP   0x08

#endif // MAYTERA_LIBC_KEYS_H
