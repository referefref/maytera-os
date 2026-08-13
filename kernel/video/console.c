// console.c - Graphical text console implementation
#include "console.h"
#include "framebuffer.h"
#include "font.h"
#include "../string.h"
#include "../serial.h"
#include <stdarg.h>

// Console state
static uint32_t con_cols = 0;      // Number of columns (chars)
static uint32_t con_rows = 0;      // Number of rows (chars)
static uint32_t cursor_col = 0;    // Current cursor column
static uint32_t cursor_row = 0;    // Current cursor row
static uint32_t fg_color = FB_WHITE;
static uint32_t bg_color = FB_BLACK;
static int cursor_visible = 0;

// Draw a character at the specified character position
static void console_draw_char(uint32_t col, uint32_t row, char c, uint32_t fg, uint32_t bg) {
    uint32_t x = col * FONT_WIDTH;
    uint32_t y = row * FONT_HEIGHT;

    const uint8_t *glyph = font_get_glyph(c);

    for (int py = 0; py < FONT_HEIGHT; py++) {
        uint8_t row_data = glyph[py];
        for (int px = 0; px < FONT_WIDTH; px++) {
            // MSB is leftmost pixel
            uint32_t color = (row_data & (0x80 >> px)) ? fg : bg;
            fb_put_pixel(x + px, y + py, color);
        }
    }
}

// Erase a character cell (fill with background)
static void console_erase_char(uint32_t col, uint32_t row) {
    uint32_t x = col * FONT_WIDTH;
    uint32_t y = row * FONT_HEIGHT;
    fb_fill_rect(x, y, FONT_WIDTH, FONT_HEIGHT, bg_color);
}

// Scroll the console up by one line
static void console_scroll(void) {
    // Scroll framebuffer content up
    fb_scroll(FONT_HEIGHT, bg_color);

    // Move cursor up one row
    if (cursor_row > 0) {
        cursor_row--;
    }
}

// Handle newline
static void console_newline(void) {
    cursor_col = 0;
    cursor_row++;

    if (cursor_row >= con_rows) {
        console_scroll();
    }
}

// Draw/erase cursor at current position
static void console_draw_cursor(int draw) {
    if (!cursor_visible) return;

    uint32_t x = cursor_col * FONT_WIDTH;
    uint32_t y = cursor_row * FONT_HEIGHT + FONT_HEIGHT - 2;

    // Draw a simple underscore cursor
    for (int i = 0; i < FONT_WIDTH; i++) {
        fb_put_pixel(x + i, y, draw ? fg_color : bg_color);
        fb_put_pixel(x + i, y + 1, draw ? fg_color : bg_color);
    }
}

// Initialize console
void console_init(framebuffer_info_t *fb_info) {
    // Initialize framebuffer
    fb_init(fb_info);

    // Calculate console dimensions
    con_cols = fb_get_width() / FONT_WIDTH;
    con_rows = fb_get_height() / FONT_HEIGHT;

    // Set defaults
    cursor_col = 0;
    cursor_row = 0;
    fg_color = FB_WHITE;
    bg_color = FB_BLACK;
    cursor_visible = 1;

    // Clear screen
    console_clear();

    kprintf("[CONSOLE] Console initialized: %ux%u characters\n", con_cols, con_rows);
}

// Clear the console
void console_clear(void) {
    fb_clear(bg_color);
    cursor_col = 0;
    cursor_row = 0;
}

// Print a single character
void console_putc(char c) {
    // Erase old cursor
    console_draw_cursor(0);

    switch (c) {
        case '\n':
            console_newline();
            break;

        case '\r':
            cursor_col = 0;
            break;

        case '\t':
            // Tab to next 8-column boundary
            cursor_col = (cursor_col + 8) & ~7;
            if (cursor_col >= con_cols) {
                console_newline();
            }
            break;

        case '\b':
            // Backspace
            if (cursor_col > 0) {
                cursor_col--;
                console_erase_char(cursor_col, cursor_row);
            }
            break;

        default:
            if (c >= ' ' && c < 127) {
                console_draw_char(cursor_col, cursor_row, c, fg_color, bg_color);
                cursor_col++;

                if (cursor_col >= con_cols) {
                    console_newline();
                }
            }
            break;
    }

    // Draw new cursor
    console_draw_cursor(1);
}

// Print a string
void console_puts(const char *str) {
    if (!str) return;

    while (*str) {
        console_putc(*str++);
    }
}

// Printf-style printing
void console_printf(const char *fmt, ...) {
    // #672: was a private parser with no flag or width support. Routed through
    // the one shared parser (vsnprintf -> kvformat). Buffered, unlike kprintf,
    // because console_puts() is this console's only entry point.
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    console_puts(buf);

}

// Set text colors
void console_set_colors(uint32_t fg, uint32_t bg) {
    fg_color = fg;
    bg_color = bg;
}

// Get dimensions
uint32_t console_get_cols(void) { return con_cols; }
uint32_t console_get_rows(void) { return con_rows; }

// Cursor control
void console_set_cursor(uint32_t col, uint32_t row) {
    console_draw_cursor(0);  // Erase old
    cursor_col = col < con_cols ? col : con_cols - 1;
    cursor_row = row < con_rows ? row : con_rows - 1;
    console_draw_cursor(1);  // Draw new
}

void console_get_cursor(uint32_t *col, uint32_t *row) {
    if (col) *col = cursor_col;
    if (row) *row = cursor_row;
}

void console_show_cursor(int show) {
    console_draw_cursor(0);  // Erase if visible
    cursor_visible = show;
    console_draw_cursor(1);  // Draw if now visible
}
