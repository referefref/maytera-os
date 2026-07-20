// console.h - Graphical text console
#ifndef CONSOLE_H
#define CONSOLE_H

#include "../types.h"
#include "../boot_info.h"

// Initialize console with framebuffer info
void console_init(framebuffer_info_t *fb_info);

// Clear the console
void console_clear(void);

// Print a character
void console_putc(char c);

// Print a string
void console_puts(const char *str);

// Printf-style printing
void console_printf(const char *fmt, ...);

// Set text colors
void console_set_colors(uint32_t fg, uint32_t bg);

// Get console dimensions (in characters)
uint32_t console_get_cols(void);
uint32_t console_get_rows(void);

// Cursor control
void console_set_cursor(uint32_t col, uint32_t row);
void console_get_cursor(uint32_t *col, uint32_t *row);
void console_show_cursor(int show);

#endif // CONSOLE_H
