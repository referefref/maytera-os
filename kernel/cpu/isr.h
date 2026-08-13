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
// held modifier). Free byte: 0x80-0x8F (special-key press range) and the rest
// of 0x90-0x9F (release range) are already fully occupied in cpu/isr.c, so this
// lives in the unused part of the release range. 0x9B is not produced by any
// other path (verified against kernel/cpu/isr.c, kernel/drivers/usb_hid.c).
#define KEY_SUPER      0x9B

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
