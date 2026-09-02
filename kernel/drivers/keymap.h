// keymap.h - THE set-1 scancode-to-character mapping (#DOSRING3).
//
// WHY THIS FILE EXISTS. The two US set-1 tables and the modifier-aware
// translation used to live inside cpu/isr.c as file-statics. That was correct
// for the kernel - it kept the tables in the same translation unit as the ISR's
// own modifier state, so a caller could never see a keymap disagreeing with the
// one the ISR used - but it made them unreachable from anywhere that is not
// cpu/isr.c.
//
// The Ring-3 DOS host (#DOSRING3) needs exactly this mapping for the guest's
// INT 16h cooked-key path, and cannot compile cpu/isr.c. The choice was to
// re-type the tables in the shim or to share the ones that already exist.
// A re-typed table is precisely how the Ring-0 and Ring-3 DOS paths would come
// to disagree about what the user pressed, in a way no test would catch until a
// guest read the wrong character. So the tables move here and BOTH paths link
// the same object.
//
// The ISR's modifier STATE stays in cpu/isr.c: it is hardware state, not a
// mapping. Only the pure function of (scancode, modifiers) -> char is shared,
// which is what makes this safe to compile into a Ring-3 process.
#ifndef KEYMAP_H
#define KEYMAP_H

#include "../types.h"

// US set-1 scancode -> ASCII, unshifted and shifted. Index is the make code
// (0..127); 0 means "no printable character".
extern const char keymap_set1_ascii[128];
extern const char keymap_set1_ascii_shift[128];

// Translate a set-1 make code plus a KEY_MOD_* modifier word to a character.
// Returns 0 when the key has no printable form. Pure: no hardware, no state.
char keyboard_scancode_to_char(uint8_t scancode, uint32_t modifiers);

#endif // KEYMAP_H
