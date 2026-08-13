// font.h - Bitmap font for console
#ifndef FONT_H
#define FONT_H

#include "../types.h"

// Font dimensions
#define FONT_WIDTH  8
#define FONT_HEIGHT 16

// Get font bitmap for a character
// Returns pointer to 16 bytes (one byte per row)
const uint8_t *font_get_glyph(char c);

// Get the bitmap for a CP437 (IBM OEM) code point, 0x00-0xFF. Same 8x16 cell.
// font_get_glyph() is ASCII-only and returns a BLANK for anything above 0x7F,
// which is wrong for DOS text: box-drawing and block-shading characters live in
// 0xB0-0xDF. Use this for DOS/OEM text; use font_get_glyph() for GUI strings.
const uint8_t *font_get_glyph_cp437(unsigned char c);

#endif // FONT_H

// Small font dimensions (half size)
#define FONT_SMALL_WIDTH  4
#define FONT_SMALL_HEIGHT 8

// Draw character at half scale
void draw_char_small(int32_t x, int32_t y, char c, uint32_t color);

// Draw string at half scale
void draw_string_small(int32_t x, int32_t y, const char *str, uint32_t color);
