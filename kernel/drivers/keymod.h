#ifndef DRIVERS_KEYMOD_H
#define DRIVERS_KEYMOD_H

// Keyboard MODIFIER bits, split out of drivers/keyboard.h so that cpu/isr.c,
// which owns the live modifier state, can use them by name.
//
// Why a separate header rather than just including keyboard.h: keyboard.h also
// defines KEY_UP, KEY_DOWN, KEY_F6 and friends as SCAN CODES, while cpu/isr.c
// defines the same NAMES as its own above-ASCII KEYCODES. Those are two
// different namespaces that happen to collide, so isr.c cannot include
// keyboard.h at all. Moving only the modifier bits here gives both files ONE
// definition of them without having to resolve that collision first.
#define KEY_MOD_SHIFT   0x01
#define KEY_MOD_CTRL    0x02
#define KEY_MOD_ALT     0x04
#define KEY_MOD_CAPS    0x08
#define KEY_MOD_NUM     0x10
#define KEY_MOD_SCROLL  0x20

#endif
