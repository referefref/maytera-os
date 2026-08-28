// isr.h - Interrupt Service Routines
#ifndef ISR_H
#define ISR_H

#include "idt.h"

// Special key codes (above ASCII range, returned by keyboard_get_char)
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83
#define KEY_F11     0x85
#define KEY_F12     0x86
// (Ctrl+Home wedge, Word6 divergence catalog #1) KEY_LCTRL used to be 0x84,
// which COLLIDES with cpu/isr.c's own local `#define KEY_F5 0x84` (the F5
// press branch, scancode 0x3F, pushes the SAME byte). Any consumer reading
// the cooked keyboard_buffer (e.g. exec/win16api.c's kernel_key_to_vk) could
// not tell a bare Ctrl-press from an F5-press: pressing Ctrl alone produced
// code 0x84, which win16api.c's switch mapped to VK_F5; so Ctrl+Home in
// Word 6 silently fired Word's real F5 "Go To" command (opened invisible,
// wedging input) instead of ever reaching Home. Moved to 0x99, a byte proven
// unused by grepping isr.h/isr.c/win16api.c/DOOM's i_maytera.h for both the
// press (0x99) and this release (0x94, unchanged, was already unique).
#define KEY_LCTRL   0x99
// (Word6 divergence catalog #2, Alt-menu) KEY_LSHIFT/KEY_RSHIFT/KEY_ALT used
// to be 0x87/0x88/0x89, which COLLIDE with cpu/isr.c's own local F-key press
// defines at the SAME bytes: `KEY_F10 0x87`, `KEY_F1 0x88`, `KEY_F2 0x89`
// (scancodes 0x44/0x3B/0x3C). This is the exact same collision class as the
// KEY_LCTRL/KEY_F5 bug fixed above (Word6 divergence catalog #1, 6a848ae):
// exec/win16api.c's kernel_key_to_vk switch matches on the cooked BYTE VALUE,
// not on which key produced it, so pressing bare Left Shift (code 0x87) hit
// the `case 0x87: return VK_F10` arm and pressing bare Right Shift (code
// 0x88) hit `case 0x88: return VK_F1` -- i.e. every Shift press ALSO misfired
// a Help (F1) or menu-activate (F10) keystroke into whatever app had focus.
// KEY_ALT/0x89 additionally collided with KEY_F2, and Alt was never even
// produced as a cooked code before this fix (see cpu/isr.c), so pressing F2
// silently doubled as "Alt" wherever a consumer (DOOM's MKEY_LALT) happened
// to read that byte. Moved to bytes proven unused by grepping isr.h/isr.c/
// win16api.c/DOOM's i_maytera.h/i_video_maytera.c/syscall.c/usb_hid.c/
// textfield.h for 0x95, 0x96, 0x9A, 0x9C (all four, press AND the new Alt
// release code). isr.c's local KEY_F10/KEY_F1/KEY_F2 stay at 0x87/0x88/0x89
// unchanged (now unambiguous), matching how KEY_F5 stayed at 0x84 when
// KEY_LCTRL moved off it.
#define KEY_LSHIFT  0x95
#define KEY_RSHIFT  0x96
#define KEY_ALT     0x9A
#define KEY_F6      0x8A
// Key release codes (0x90+ range)
#define KEY_UP_REL     0x90
#define KEY_DOWN_REL   0x91
#define KEY_LEFT_REL   0x92
#define KEY_RIGHT_REL  0x93
#define KEY_LCTRL_UP   0x94
#define KEY_LSHIFT_UP  0x97
#define KEY_RSHIFT_UP  0x98
// (Word6 divergence catalog #2) Alt release had NO cooked code at all before
// this fix (cpu/isr.c's Alt-release branch only cleared a private, never-read
// `alt_pressed` flag), so no consumer could ever see Alt go up. New, unused
// byte; see the KEY_LSHIFT/KEY_RSHIFT/KEY_ALT comment above for how the free
// bytes in this range were verified.
#define KEY_ALT_UP     0x9C

// #552: Super/GUI/Windows/Command key (Left or Right). One code for both,
// since it is used only to toggle the compositor start menu (a press, not a
// held modifier). Free byte: the 0x80-0x8F special-key press range is fully
// occupied, so this lives in the release range at a byte no PS/2 make/break
// code or USB HID usage produces (verified against kernel/cpu/isr.c,
// kernel/drivers/usb_hid.c). NOTE: 0x90-0x9C of the release range are fully
// occupied (see the codes above), but 0x9D-0x9F were NOT: KEY_PRINTSCREEN
// below claims the first of them. Re-verify before adding another.
#define KEY_SUPER      0x9B

// #148: PrintScreen. Real PS/2 hardware sends it as a 4-byte set-1 sequence
// (E0 2A E0 37 make, E0 B7 E0 AA break) that does not fit the (code,
// extended) shape every other key uses; cpu/isr.c's keyboard_process_scancode
// tracks the two extended-prefixed halves of the MAKE sequence itself and
// synthesizes this cooked code once both have arrived (the BREAK sequence is
// left unhandled: a screenshot fires once per press, like KEY_SUPER, F1 and
// F11, so no release code is needed). USB HID reports PrintScreen as a plain,
// unambiguous single usage (0x46) with no such multi-byte mess, so
// drivers/usb_hid.c (and bt/hid.c) push this code directly via
// keyboard_push_cooked_key() instead of forcing it through the PS/2 set-1
// encoding just to have keyboard_process_scancode decode it straight back
// out again. One cooked code, two transports, two different but honest
// routes to it. Free byte: see the KEY_SUPER note above.
#define KEY_PRINTSCREEN 0x9D

// ---------------------------------------------------------------------------
// #243: NAVIGATION / EDITING KEYS. Codes of their own, above the byte.
//
// WHAT BROKE. These six were forwarded as their raw set-1 make codes, and
// 0x47/0x4F/0x49/0x51/0x52/0x53 are the ASCII codes for G O I Q R S. So Home
// and a capital G were THE SAME VALUE, and no consumer could tell them apart.
// Measured on a booted machine: `echo GIOQ hello` displayed `echo  hello`
// (the terminal's scrollback bindings swallowed the letters), `vi
// /BUILDINFO.TXT` opened /BULDNF.TXT, and PgUp inside vi entered INSERT mode
// because the pty received the letter 'i'. exec/win16api.c had already
// resolved the same ambiguity the OTHER way for Word 6 (0x47 -> VK_HOME), so
// Word could not type a capital G. One ambiguity, and every consumer had to
// pick which key to break.
//
// WHY IT SURVIVED #188/#191, which fixed exactly this class for the arrows.
// The byte namespace was FULL: 0x00-0x7F ASCII, 0x80-0x8F arrows and F-keys,
// 0x90-0x9D releases and modifiers, 0xA0-0xFE `printable | 0x80` releases.
// There was no free byte, so the pass-through was rationalised in a comment
// arguing these values are safe because they are "below 0x80" and so cannot
// collide with the KEY_* range. True of the 0x80 block, and false of ASCII.
// A comment asserting safety is not a check; build/keycode-gate.sh now checks.
//
// THE MECHANISM. cpu/isr.c's cooked ring is uint16_t (#243), so this block is
// out of reach of every 8-bit encoding above it: no ASCII character, no
// `| 0x80` release and no modifier code can produce a value >= 0x100. The
// collision is gone by construction, not by everyone agreeing to be careful.
//
// PRESS-ONLY (no release code), like KEY_SUPER and KEY_PRINTSCREEN: isr.c's
// extended-RELEASE switch has never emitted anything for these six.
// userland/libc/keys.h and keys.rs mirror these values and the gate checks
// all three files against one another.
// ---------------------------------------------------------------------------
#define KEY_HOME    0x100
#define KEY_END     0x101
#define KEY_PGUP    0x102
#define KEY_PGDN    0x103
#define KEY_INS     0x104
#define KEY_DEL     0x105

// #148: push a cooked key code directly into the keyboard ring, bypassing
// scancode translation entirely. For a transport (USB/Bluetooth HID) whose
// report is a clean, unambiguous key that has no honest single-(code,
// extended) PS/2 set-1 encoding (KEY_PRINTSCREEN; see above), this is the
// shared entry point, so no caller re-implements the ring-buffer push. Safe
// to call from any context that already calls keyboard_process_scancode
// (thread or IRQ; never touches the PIC/EOI).
void keyboard_push_cooked_key(uint16_t code);   // #243: uint16_t, see KEY_HOME above

// Initialize interrupt handlers
void isr_init(void);

// Timer tick count
extern volatile uint64_t timer_ticks;

// Interrupt flag (set by Ctrl+C)
extern volatile int interrupt_requested;

// Clear interrupt flag
void clear_interrupt(void);

// Check if interrupt was requested
int check_interrupt(void);

// Keyboard handler (returns last key pressed, or 0 if none)
// Returns int to handle special keys like KEY_UP (0x80+)
int keyboard_get_char(void);

// Check if a key is available
int keyboard_has_char(void);

// #307: shared scancode processor (fed by both the PS/2 IRQ and USB HID).
// Takes a PS/2 set-1 scancode byte; does not touch the PIC/EOI.
void keyboard_process_scancode(uint8_t scancode);

#endif // ISR_H
