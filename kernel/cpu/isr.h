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
#define KEY_LCTRL   0x84
#define KEY_LSHIFT  0x87
#define KEY_RSHIFT  0x88
#define KEY_ALT     0x89
#define KEY_F6      0x8A
// Key release codes (0x90+ range)
#define KEY_UP_REL     0x90
#define KEY_DOWN_REL   0x91
#define KEY_LEFT_REL   0x92
#define KEY_RIGHT_REL  0x93
#define KEY_LCTRL_UP   0x94
#define KEY_LSHIFT_UP  0x97
#define KEY_RSHIFT_UP  0x98

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
