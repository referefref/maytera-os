// keymap.c - THE set-1 scancode-to-character mapping. See keymap.h for why it
// is here rather than inside cpu/isr.c.
#include "keymap.h"
#include "keyboard.h"   // KEY_MOD_*

// US keyboard scancode to ASCII mapping (set 1)
const char keymap_set1_ascii[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*',  0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,   0,
};

// Shifted characters
const char keymap_set1_ascii_shift[128] = {
    0,    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*',  0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   '-', 0,   0,   0,   '+', 0,   0,
    0,    0,   0,   0,   0,   0,   0,   0,   0,
};

char keyboard_scancode_to_char(uint8_t scancode, uint32_t modifiers) {
    if (scancode >= 128) return 0;
    char c = (modifiers & KEY_MOD_SHIFT) ? keymap_set1_ascii_shift[scancode]
                                         : keymap_set1_ascii[scancode];
    if ((modifiers & KEY_MOD_CAPS) && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    else if ((modifiers & KEY_MOD_CAPS) && (modifiers & KEY_MOD_SHIFT) &&
             c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');
    if (modifiers & KEY_MOD_CTRL) {
        if (c >= 'a' && c <= 'z')      c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
    }
    return c;
}
