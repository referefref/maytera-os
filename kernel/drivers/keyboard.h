// keyboard.h - Keyboard driver interface for MayteraOS
#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "../types.h"

// Keyboard modifier keys
#define KEY_MOD_SHIFT   0x01
#define KEY_MOD_CTRL    0x02
#define KEY_MOD_ALT     0x04
#define KEY_MOD_CAPS    0x08
#define KEY_MOD_NUM     0x10
#define KEY_MOD_SCROLL  0x20

// Special key codes
#define KEY_ESCAPE      0x01
#define KEY_BACKSPACE   0x0E
#define KEY_TAB         0x0F
#define KEY_ENTER       0x1C
#define KEY_CTRL        0x1D
#define KEY_LSHIFT      0x2A
#define KEY_RSHIFT      0x36
#define KEY_ALT         0x38
#define KEY_SPACE       0x39
#define KEY_CAPS        0x3A
#define KEY_F1          0x3B
#define KEY_F2          0x3C
#define KEY_F3          0x3D
#define KEY_F4          0x3E
#define KEY_F5          0x3F
#define KEY_F6          0x40
#define KEY_F7          0x41
#define KEY_F8          0x42
#define KEY_F9          0x43
#define KEY_F10         0x44
#define KEY_NUM         0x45
#define KEY_SCROLL      0x46
#define KEY_HOME        0x47
#define KEY_UP          0x48
#define KEY_PGUP        0x49
#define KEY_LEFT        0x4B
#define KEY_RIGHT       0x4D
#define KEY_END         0x4F
#define KEY_DOWN        0x50
#define KEY_PGDOWN      0x51
#define KEY_INSERT      0x52
#define KEY_DELETE      0x53
#define KEY_F11         0x57
#define KEY_F12         0x58

// Initialize keyboard driver
void keyboard_init(void);

// Get current modifier state
uint32_t keyboard_get_modifiers(void);

// Check if a key is currently pressed
int keyboard_is_pressed(uint8_t scancode);

// Get character from scancode (0 if special key)
char keyboard_scancode_to_char(uint8_t scancode, uint32_t modifiers);

// Raw keyboard polling (non-blocking)
int keyboard_poll(uint8_t *scancode, int *pressed);

#endif // KEYBOARD_H
