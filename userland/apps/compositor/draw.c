// draw.c - Drawing primitives for the MayteraOS userland compositor
// Provides pixel, rect, circle, gradient, and text rendering.
// All operations clip to g_fb_width / g_fb_height. No malloc, no libc beyond
// what compositor.h declares.

#include "compositor.h"
// (#745) uptime_ms() lives here. draw.c did not include it before because
// nothing in it needed a clock; the glass rate limiter does. Without the
// declaration gcc assumes int, which TRUNCATES the 64-bit ms clock.
#include "../../libc/syscall.h"
// (#745 avatar AA) sqrtf() for draw_circle_filled_aa/draw_circle_ring_aa's
// per-pixel distance test. Ring-3 userland links real libgcc/libm - this is
// not the kernel's -mno-sse target, see the design doc section 11.
#include "../../libc/math.h"
#include "../../libc/dock_opacity.h"   // #132: shared DOCK_OPACITY_MIN/MAX/DEFAULT/WARN

// ============================================================================
// 8x16 bitmap font (ASCII 32-126)
// Each glyph is 16 rows of 8 bits. MSB is the leftmost pixel.
// Index 0 = ASCII 0x20 (space), index 94 = ASCII 0x7E (tilde).
// Characters outside [32,126] are rendered as space (glyph 0).
// ============================================================================

static const uint8_t font_data[128][16] = {
    // 0x00 - NUL (blank)
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x01
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x02
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x03
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x04
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x05
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x06
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x07
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x08
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x09
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x0A
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x0B
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x0C
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x0D
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x0E
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x0F
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x10
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x11
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x12
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x13
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x14
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x15
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x16
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x17
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x18
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x19
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x1A
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x1B
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x1C
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x1D
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x1E
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x1F
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },

    // 0x20 ' ' space
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x21 '!'
    { 0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00 },
    // 0x22 '"'
    { 0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x23 '#'
    { 0x00,0x36,0x36,0x7F,0x36,0x36,0x36,0x7F,0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x24 '$'
    { 0x00,0x18,0x3C,0x7E,0x78,0x3C,0x1E,0x7E,0x3C,0x18,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x25 '%'
    { 0x00,0x62,0x66,0x0C,0x18,0x30,0x66,0x46,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x26 '&'
    { 0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x27 "'"
    { 0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x28 '('
    { 0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x29 ')'
    { 0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x2A '*'
    { 0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x2B '+'
    { 0x00,0x00,0x18,0x18,0x18,0xFF,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x2C ','
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00,0x00 },
    // 0x2D '-'
    { 0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x2E '.'
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x2F '/'
    { 0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x30 '0'
    { 0x00,0x3C,0x66,0xC3,0xC3,0xDB,0xC3,0xC3,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x31 '1'
    { 0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x32 '2'
    { 0x00,0x3C,0x66,0xC3,0x06,0x0C,0x18,0x30,0x63,0xFF,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x33 '3'
    { 0x00,0x3C,0x66,0xC3,0x06,0x1C,0x06,0xC3,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x34 '4'
    { 0x00,0x06,0x0E,0x1E,0x36,0x66,0xFF,0x06,0x06,0x06,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x35 '5'
    { 0x00,0xFF,0xC0,0xC0,0xFC,0x06,0x06,0xC3,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x36 '6'
    { 0x00,0x3C,0x66,0xC0,0xFC,0xC6,0xC6,0xC6,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x37 '7'
    { 0x00,0xFF,0x03,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x38 '8'
    { 0x00,0x3C,0x66,0xC3,0x66,0x3C,0x66,0xC3,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x39 '9'
    { 0x00,0x3C,0x66,0xC6,0xC6,0xC6,0x7E,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x3A ':'
    { 0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x3B ';'
    { 0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00 },
    // 0x3C '<'
    { 0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x3D '='
    { 0x00,0x00,0x00,0x00,0xFF,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x3E '>'
    { 0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x3F '?'
    { 0x00,0x3C,0x66,0xC3,0x06,0x0C,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x40 '@'
    { 0x00,0x3C,0x66,0xC3,0xDB,0xDB,0xDB,0xC0,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x41 'A'
    { 0x00,0x18,0x3C,0x66,0xC3,0xC3,0xFF,0xC3,0xC3,0xC3,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x42 'B'
    { 0x00,0xFC,0x66,0x63,0x66,0x7C,0x66,0x63,0x66,0xFC,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x43 'C'
    { 0x00,0x3C,0x66,0xC3,0xC0,0xC0,0xC0,0xC3,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x44 'D'
    { 0x00,0xF8,0x6C,0x66,0x63,0x63,0x63,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x45 'E'
    { 0x00,0xFF,0x60,0x60,0x60,0x7C,0x60,0x60,0x60,0xFF,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x46 'F'
    { 0x00,0xFF,0x60,0x60,0x60,0x7C,0x60,0x60,0x60,0x60,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x47 'G'
    { 0x00,0x3C,0x66,0xC3,0xC0,0xCF,0xC3,0xC3,0x67,0x3B,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x48 'H'
    { 0x00,0xC3,0xC3,0xC3,0xC3,0xFF,0xC3,0xC3,0xC3,0xC3,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x49 'I'
    { 0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x4A 'J'
    { 0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x4B 'K'
    { 0x00,0xC6,0xCC,0xD8,0xF0,0xE0,0xF0,0xD8,0xCC,0xC6,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x4C 'L'
    { 0x00,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0xFF,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x4D 'M'
    { 0x00,0xC3,0xE7,0xFF,0xDB,0xC3,0xC3,0xC3,0xC3,0xC3,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x4E 'N'
    { 0x00,0xC3,0xE3,0xF3,0xDB,0xCF,0xC7,0xC3,0xC3,0xC3,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x4F 'O'
    { 0x00,0x3C,0x66,0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x50 'P'
    { 0x00,0xFC,0x66,0x63,0x63,0x7E,0x60,0x60,0x60,0x60,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x51 'Q'
    { 0x00,0x3C,0x66,0xC3,0xC3,0xC3,0xDB,0xCF,0x66,0x3C,0x03,0x00,0x00,0x00,0x00,0x00 },
    // 0x52 'R'
    { 0x00,0xFC,0x66,0x63,0x63,0x7E,0x6C,0x66,0x63,0x63,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x53 'S'
    { 0x00,0x3C,0x66,0xC3,0x60,0x3C,0x06,0xC3,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x54 'T'
    { 0x00,0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x55 'U'
    { 0x00,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x56 'V'
    { 0x00,0xC3,0xC3,0xC3,0xC3,0xC3,0x66,0x66,0x3C,0x18,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x57 'W'
    { 0x00,0xC3,0xC3,0xC3,0xC3,0xDB,0xFF,0xE7,0xC3,0xC3,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x58 'X'
    { 0x00,0xC3,0xC3,0x66,0x3C,0x18,0x3C,0x66,0xC3,0xC3,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x59 'Y'
    { 0x00,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x5A 'Z'
    { 0x00,0xFF,0x03,0x06,0x0C,0x18,0x30,0x60,0xC0,0xFF,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x5B '['
    { 0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x5C '\'
    { 0x00,0x00,0x80,0xC0,0x60,0x30,0x18,0x0C,0x06,0x03,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x5D ']'
    { 0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x5E '^'
    { 0x00,0x18,0x3C,0x66,0xC3,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x5F '_'
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x60 '`'
    { 0x00,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x61 'a'
    { 0x00,0x00,0x00,0x00,0x3C,0x06,0x3E,0x66,0x66,0x3B,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x62 'b'
    { 0x00,0x60,0x60,0x60,0x7C,0x66,0x63,0x63,0x66,0x7C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x63 'c'
    { 0x00,0x00,0x00,0x00,0x3C,0x66,0xC0,0xC0,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x64 'd'
    { 0x00,0x03,0x03,0x03,0x3F,0x63,0xC3,0xC3,0x63,0x3F,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x65 'e'
    { 0x00,0x00,0x00,0x00,0x3C,0x66,0xFF,0xC0,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x66 'f'
    { 0x00,0x1E,0x30,0x30,0x7E,0x30,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x67 'g'
    { 0x00,0x00,0x00,0x00,0x3B,0x66,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00,0x00,0x00,0x00 },
    // 0x68 'h'
    { 0x00,0x60,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x69 'i'
    { 0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x6A 'j'
    { 0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x66,0x3C,0x00,0x00,0x00,0x00 },
    // 0x6B 'k'
    { 0x00,0x60,0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x63,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x6C 'l'
    { 0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x6D 'm'
    { 0x00,0x00,0x00,0x00,0xE6,0xFF,0xDB,0xC3,0xC3,0xC3,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x6E 'n'
    { 0x00,0x00,0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x6F 'o'
    { 0x00,0x00,0x00,0x00,0x3C,0x66,0xC3,0xC3,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x70 'p'
    { 0x00,0x00,0x00,0x00,0x7C,0x66,0x63,0x63,0x66,0x7C,0x60,0x60,0x00,0x00,0x00,0x00 },
    // 0x71 'q'
    { 0x00,0x00,0x00,0x00,0x3E,0x66,0xC6,0xC6,0x66,0x3E,0x06,0x06,0x00,0x00,0x00,0x00 },
    // 0x72 'r'
    { 0x00,0x00,0x00,0x00,0x6C,0x76,0x60,0x60,0x60,0x60,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x73 's'
    { 0x00,0x00,0x00,0x00,0x3C,0x60,0x3C,0x06,0x06,0x7C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x74 't'
    { 0x00,0x30,0x30,0x30,0x7E,0x30,0x30,0x30,0x30,0x1C,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x75 'u'
    { 0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x3B,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x76 'v'
    { 0x00,0x00,0x00,0x00,0xC3,0xC3,0x66,0x66,0x3C,0x18,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x77 'w'
    { 0x00,0x00,0x00,0x00,0xC3,0xC3,0xDB,0xFF,0xE7,0xC3,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x78 'x'
    { 0x00,0x00,0x00,0x00,0xC3,0x66,0x3C,0x3C,0x66,0xC3,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x79 'y'
    { 0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x06,0x0C,0x78,0x00,0x00,0x00,0x00 },
    // 0x7A 'z'
    { 0x00,0x00,0x00,0x00,0xFF,0x0C,0x18,0x30,0x60,0xFF,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x7B '{'
    { 0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x7C '|'
    { 0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x7D '}'
    { 0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x7E '~'
    { 0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    // 0x7F (DEL, blank)
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
};

// ============================================================================
// Internal helpers
// ============================================================================

// Extract an 8-bit channel from a packed ARGB word.
#define CHAN_R(c) (((c) >> 16) & 0xFF)
#define CHAN_G(c) (((c) >>  8) & 0xFF)
#define CHAN_B(c) (((c)      ) & 0xFF)
#define CHAN_A(c) (((c) >> 24) & 0xFF)

// Pack R,G,B into full-alpha ARGB.
#define PACK_RGB(r,g,b) (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

// Integer absolute value (avoids a libm dependency).
static inline int32_t iabs32(int32_t v) { return v < 0 ? -v : v; }

// ============================================================================
// Pixel and basic shapes
// ============================================================================

// Optional alpha-blend for compositor-drawn elements (e.g. transparent desktop
// widgets). 255 = opaque; <255 blends the drawn color over the existing pixel.
int g_draw_blend = 255;
static inline uint32_t draw_blend(uint32_t dst, uint32_t src, int op) {
    int inv = 255 - op;
    uint32_t r = (((src >> 16) & 0xFF) * op + ((dst >> 16) & 0xFF) * inv) / 255;
    uint32_t g = (((src >> 8)  & 0xFF) * op + ((dst >> 8)  & 0xFF) * inv) / 255;
    uint32_t b = (( src        & 0xFF) * op + ( dst        & 0xFF) * inv) / 255;
    return (r << 16) | (g << 8) | b;
}

// ============================================================================
// Dirty-rectangle compositing (#102/#379): a global clip rectangle bounds every
// primitive so the idle compositor can recomposite only the regions that
// changed. Default clip is the full screen, so any code path that does not set
// a clip behaves exactly as before. All bulk pixel loops below clamp their
// bounds to the intersection of the screen and this clip; per-pixel writers
// (putpixel, char, icon blit) test each pixel with draw_pt_in_clip().
// ============================================================================
int g_clip_x0 = 0, g_clip_y0 = 0, g_clip_x1 = 1 << 24, g_clip_y1 = 1 << 24;

void draw_set_clip(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > g_fb_width)  x1 = g_fb_width;
    if (y1 > g_fb_height) y1 = g_fb_height;
    g_clip_x0 = x; g_clip_y0 = y; g_clip_x1 = x1; g_clip_y1 = y1;
}

void draw_clear_clip(void)
{
    g_clip_x0 = 0; g_clip_y0 = 0;
    g_clip_x1 = (g_fb_width  > 0) ? g_fb_width  : (1 << 24);
    g_clip_y1 = (g_fb_height > 0) ? g_fb_height : (1 << 24);
}

// (#745) See the comment on the declaration in compositor.h: a nestable
// clip, built on top of the existing global (draw_set_clip/draw_clear_clip
// are left exactly as they were - every existing caller keeps working). Depth
// 4 comfortably covers every nesting this renderer does; deeper than that
// falls back to a plain draw_set_clip() rather than corrupting the stack.
#define DRAW_CLIP_STACK_MAX 4
static int32_t g_clip_stack_x0[DRAW_CLIP_STACK_MAX];
static int32_t g_clip_stack_y0[DRAW_CLIP_STACK_MAX];
static int32_t g_clip_stack_x1[DRAW_CLIP_STACK_MAX];
static int32_t g_clip_stack_y1[DRAW_CLIP_STACK_MAX];
static int     g_clip_stack_n = 0;

void draw_push_clip(int x, int y, int w, int h)
{
    if (g_clip_stack_n >= DRAW_CLIP_STACK_MAX) { draw_set_clip(x, y, w, h); return; }
    g_clip_stack_x0[g_clip_stack_n] = g_clip_x0;
    g_clip_stack_y0[g_clip_stack_n] = g_clip_y0;
    g_clip_stack_x1[g_clip_stack_n] = g_clip_x1;
    g_clip_stack_y1[g_clip_stack_n] = g_clip_y1;
    g_clip_stack_n++;

    int32_t x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < g_clip_x0) x0 = g_clip_x0;
    if (y0 < g_clip_y0) y0 = g_clip_y0;
    if (x1 > g_clip_x1) x1 = g_clip_x1;
    if (y1 > g_clip_y1) y1 = g_clip_y1;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    draw_set_clip(x0, y0, x1 - x0, y1 - y0);
}

void draw_pop_clip(void)
{
    if (g_clip_stack_n <= 0) { draw_clear_clip(); return; }
    g_clip_stack_n--;
    g_clip_x0 = g_clip_stack_x0[g_clip_stack_n];
    g_clip_y0 = g_clip_stack_y0[g_clip_stack_n];
    g_clip_x1 = g_clip_stack_x1[g_clip_stack_n];
    g_clip_y1 = g_clip_stack_y1[g_clip_stack_n];
}

// Intersect a rect with the current clip AND the screen (loop-bound clipping).
static inline void clamp_draw_area(int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    int32_t x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < g_clip_x0) x0 = g_clip_x0;
    if (y0 < g_clip_y0) y0 = g_clip_y0;
    if (x1 > g_clip_x1) x1 = g_clip_x1;
    if (y1 > g_clip_y1) y1 = g_clip_y1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_fb_width)  x1 = g_fb_width;
    if (y1 > g_fb_height) y1 = g_fb_height;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}

// ============================================================================
// Damage list (#102/#379): the set of rectangles that changed this frame. The
// idle compositor recomposites (and presents) only these. Overlapping rects are
// merged; if the list overflows it collapses to a single bounding rect.
// ============================================================================
#define DAMAGE_MAX 24
typedef struct { int32_t x, y, w, h; } damage_rect_t;
static damage_rect_t g_damage[DAMAGE_MAX];
static int g_damage_n = 0;

void damage_reset(void) { g_damage_n = 0; }
int  damage_count(void) { return g_damage_n; }

int damage_get(int i, int *x, int *y, int *w, int *h)
{
    if (i < 0 || i >= g_damage_n) return 0;
    if (x) *x = g_damage[i].x; if (y) *y = g_damage[i].y;
    if (w) *w = g_damage[i].w; if (h) *h = g_damage[i].h;
    return 1;
}

static int rects_overlap(const damage_rect_t *a, int32_t x, int32_t y, int32_t w, int32_t h)
{
    return !(x >= a->x + a->w || x + w <= a->x || y >= a->y + a->h || y + h <= a->y);
}

void damage_add(int x, int y, int w, int h)
{
    // Clamp to the screen; drop empty rects.
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (g_fb_width  > 0 && x + w > g_fb_width)  w = g_fb_width  - x;
    if (g_fb_height > 0 && y + h > g_fb_height) h = g_fb_height - y;
    if (w <= 0 || h <= 0) return;

    // Merge into any rect it already overlaps or touches (grow to the union) so
    // adjacent widget updates do not fragment the list.
    for (int i = 0; i < g_damage_n; i++) {
        damage_rect_t *r = &g_damage[i];
        if (rects_overlap(r, x - 2, y - 2, w + 4, h + 4)) {
            int32_t nx0 = (r->x < x) ? r->x : x;
            int32_t ny0 = (r->y < y) ? r->y : y;
            int32_t nx1 = (r->x + r->w > x + w) ? r->x + r->w : x + w;
            int32_t ny1 = (r->y + r->h > y + h) ? r->y + r->h : y + h;
            r->x = nx0; r->y = ny0; r->w = nx1 - nx0; r->h = ny1 - ny0;
            return;
        }
    }

    if (g_damage_n < DAMAGE_MAX) {
        g_damage[g_damage_n].x = x; g_damage[g_damage_n].y = y;
        g_damage[g_damage_n].w = w; g_damage[g_damage_n].h = h;
        g_damage_n++;
        return;
    }

    // Overflow: collapse everything (existing + new) into one bounding rect.
    int32_t bx0 = x, by0 = y, bx1 = x + w, by1 = y + h;
    for (int i = 0; i < g_damage_n; i++) {
        if (g_damage[i].x < bx0) bx0 = g_damage[i].x;
        if (g_damage[i].y < by0) by0 = g_damage[i].y;
        if (g_damage[i].x + g_damage[i].w > bx1) bx1 = g_damage[i].x + g_damage[i].w;
        if (g_damage[i].y + g_damage[i].h > by1) by1 = g_damage[i].y + g_damage[i].h;
    }
    g_damage[0].x = bx0; g_damage[0].y = by0;
    g_damage[0].w = bx1 - bx0; g_damage[0].h = by1 - by0;
    g_damage_n = 1;
}

void draw_putpixel(int32_t x, int32_t y, uint32_t color)
{
    if (x < 0 || y < 0 || x >= g_fb_width || y >= g_fb_height)
        return;
    if (!draw_pt_in_clip(x, y)) return;
    if (g_draw_blend < 255) color = draw_blend(g_fb[y * g_fb_pitch + x], color, g_draw_blend);
    g_fb[y * g_fb_pitch + x] = color;
}

void draw_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    clamp_draw_area(&x, &y, &w, &h);
    if (w <= 0 || h <= 0) return;

    for (int32_t row = 0; row < h; row++) {
        uint32_t *dst = g_fb + (y + row) * g_fb_pitch + x;
        for (int32_t col = 0; col < w; col++)
            dst[col] = (g_draw_blend < 255) ? draw_blend(dst[col], color, g_draw_blend) : color;
    }
}

void draw_hline(int32_t x, int32_t y, int32_t w, uint32_t color)
{
    int32_t h = 1;
    clamp_draw_area(&x, &y, &w, &h);
    if (w <= 0 || h <= 0) return;
    uint32_t *dst = g_fb + y * g_fb_pitch + x;
    for (int32_t i = 0; i < w; i++)
        dst[i] = (g_draw_blend < 255) ? draw_blend(dst[i], color, g_draw_blend) : color;
}

void draw_vline(int32_t x, int32_t y, int32_t h, uint32_t color)
{
    int32_t w = 1;
    clamp_draw_area(&x, &y, &w, &h);
    if (w <= 0 || h <= 0) return;
    for (int32_t i = 0; i < h; i++) {
        uint32_t *dp = &g_fb[(y + i) * g_fb_pitch + x];
        *dp = (g_draw_blend < 255) ? draw_blend(*dp, color, g_draw_blend) : color;
    }
}

// #745: one horizontal run of `color` with a PER-PIXEL alpha taken from a
// caller-supplied table. Added to draw.c rather than open-coded in main.c
// because it is the generic primitive the file was missing: draw_hline() can
// only do ONE alpha for the whole run (g_draw_blend), so a gradient edge had no
// choice but a per-pixel draw_putpixel() call, and 44k function calls per frame
// is most of the cost of a soft shadow. `astep` is the alpha-table stride:
// +1 walks it left to right, -1 right to left, 0 holds one value (equivalent to
// draw_hline at that blend). Honors the active clip, including advancing the
// alpha table when the left edge is clipped, so a clipped partial repaint draws
// the SAME pixels as the unclipped one - that identity is what keeps the
// dirty-rect paths from disagreeing with the full composite.
void draw_hspan_alpha(int32_t x, int32_t y, int32_t w,
                      uint32_t color, const uint8_t *alpha, int astep)
{
    if (!g_fb || w <= 0 || !alpha) return;
    if (y < g_clip_y0 || y >= g_clip_y1) return;
    int32_t x0 = x, x1 = x + w;
    const uint8_t *a = alpha;
    if (x0 < g_clip_x0) { a += (int)(g_clip_x0 - x0) * astep; x0 = g_clip_x0; }
    if (x1 > g_clip_x1) x1 = g_clip_x1;
    if (x0 >= x1) return;
    uint32_t *dst = &g_fb[y * g_fb_pitch];
    // NOTE, because this looked like free money and is not: the obvious
    // optimisation here is the packed trick, blending red and blue together in
    // one 32-bit multiply since they sit in separate 16-bit lanes. It was
    // written, and it was WRONG: that trick is only valid when the reduction is
    // a SHIFT. `(packed * inv) / 255` is a division across the whole 32-bit
    // value, so the remainder of the red lane leaks into blue - the lanes are
    // independent under >>8 and are not under /255. The correct packed form
    // (x + 0x00800080 + ((x >> 8) & 0x00FF00FF)) >> 8 computes ROUND(x/255)
    // rather than the FLOOR this loop computes, i.e. it would shift the whole
    // measured falloff by up to a level. It is worth about 0.17 ms per pass on
    // the 688x616 OOBE card (0.52 -> ~0.35 measured on the broken version, so
    // treat that figure as indicative only). Not taken: the falloff has been
    // measured against the spec at <=1.23 levels of deviation with THIS
    // arithmetic, and re-defining the rounding to save a fraction of a
    // millisecond on a path that costs zero when nothing is animating is a bad
    // trade. If someone wants it later, change it and RE-MEASURE the ramp.
    uint32_t sr = (color >> 16) & 0xFF, sg = (color >> 8) & 0xFF, sb = color & 0xFF;
    for (int32_t px = x0; px < x1; px++, a += astep) {
        uint32_t al = *a;
        if (al == 0) continue;
        if (al >= 255) { dst[px] = color; continue; }
        uint32_t d = dst[px];
        uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
        uint32_t inv = 255 - al;
        dst[px] = 0xFF000000u
                | (((dr * inv + sr * al) / 255) << 16)
                | (((dg * inv + sg * al) / 255) << 8)
                |  ((db * inv + sb * al) / 255);
    }
}

void draw_rect_outline(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    draw_hline(x,         y,         w, color);
    draw_hline(x,         y + h - 1, w, color);
    draw_vline(x,         y,         h, color);
    draw_vline(x + w - 1, y,         h, color);
}

// ============================================================================
// Circle helpers (midpoint / Bresenham)
// ============================================================================

// Plot four symmetric horizontal spans for a filled circle.
static void circle_fill_spans(int32_t cx, int32_t cy, int32_t dx, int32_t dy, uint32_t color)
{
    // Top and bottom spans from the current point.
    draw_hline(cx - dx, cy - dy, dx * 2 + 1, color);
    if (dy != 0)
        draw_hline(cx - dx, cy + dy, dx * 2 + 1, color);
}

// Plot eight symmetric points for an outline circle.
static void circle_plot8(int32_t cx, int32_t cy, int32_t dx, int32_t dy, uint32_t color)
{
    draw_putpixel(cx + dx, cy + dy, color);
    draw_putpixel(cx - dx, cy + dy, color);
    draw_putpixel(cx + dx, cy - dy, color);
    draw_putpixel(cx - dx, cy - dy, color);
    draw_putpixel(cx + dy, cy + dx, color);
    draw_putpixel(cx - dy, cy + dx, color);
    draw_putpixel(cx + dy, cy - dx, color);
    draw_putpixel(cx - dy, cy - dx, color);
}

void draw_circle_filled(int32_t cx, int32_t cy, int32_t r, uint32_t color)
{
    if (r <= 0) return;
    int32_t x = 0, y = r, d = 1 - r;
    while (x <= y) {
        circle_fill_spans(cx, cy, x, y, color);
        circle_fill_spans(cx, cy, y, x, color);
        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

void draw_circle_outline(int32_t cx, int32_t cy, int32_t r, uint32_t color)
{
    if (r <= 0) return;
    int32_t x = 0, y = r, d = 1 - r;
    while (x <= y) {
        circle_plot8(cx, cy, x, y, color);
        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

// ============================================================================
// Antialiased circle primitives (#745 avatar polish port,
// docs/LOGIN_AVATARS_AND_PROFILE.html section 11)
// ============================================================================
// The midpoint rasterizers above plot every pixel at 100% color or 0% - there
// is no fractional edge anywhere on the boundary (measured: the ring pixel
// equals the token exactly, same finding as draw_rounded_rect before #745).
// These two build a coverage-based edge instead: per scanline, every pixel's
// alpha is a linear function of its TRUE distance to the circle boundary
// (`coverage = clamp(0.5 - d, 0, 1)`, a 1px-wide falloff straddling the exact
// edge), then the whole row is composited in one draw_hspan_alpha() call -
// the same per-pixel-alpha primitive the #745 shadow pass added, so this
// honors the active clip and the existing blend path rather than a new one.
//
// Float is fine here: userland/apps/compositor/ is Ring-3, built with
// hardware SSE2 (the ABI baseline), not the kernel's -mno-sse target. A 64px
// avatar's bounding box is at most ~70x70px and there are never more than a
// handful on screen at once, so one sqrtf() per pixel is a trivial per-frame
// cost - see the design doc section 11 for the same reasoning.
//
// Row buffer capped at 256px: no avatar-scale caller comes close, and this
// avoids a VLA in the render path.
#define DRAW_AA_ROW_MAX 256

void draw_circle_filled_aa(int32_t cx, int32_t cy, float r, uint32_t color, int alpha)
{
    if (r <= 0.0f || alpha <= 0) return;
    int32_t r0 = (int32_t)r + 2;
    int32_t x0 = cx - r0, x1 = cx + r0;
    int32_t w = x1 - x0 + 1;
    if (w <= 0 || w > DRAW_AA_ROW_MAX) return;
    uint8_t row[DRAW_AA_ROW_MAX];

    for (int32_t y = cy - r0; y <= cy + r0; y++) {
        if (y < g_clip_y0 || y >= g_clip_y1) continue;
        float dy = ((float)y + 0.5f) - (float)cy;
        float dy2 = dy * dy;
        float reach = r + 1.0f;
        if (dy2 > reach * reach) continue;
        for (int32_t i = 0; i < w; i++) {
            float dx = ((float)(x0 + i) + 0.5f) - (float)cx;
            float dist = sqrtf(dx * dx + dy2) - r;
            float cov = 0.5f - dist;
            if (cov < 0.0f) cov = 0.0f;
            if (cov > 1.0f) cov = 1.0f;
            int32_t a = (int32_t)(cov * (float)alpha + 0.5f);
            row[i] = (uint8_t)(a > 255 ? 255 : a);
        }
        draw_hspan_alpha(x0, y, w, color, row, 1);
    }
}

// Coverage against TWO boundaries (r - stroke_w/2, r + stroke_w/2): a pixel's
// coverage is "inside the outer edge" times "outside the inner edge", each
// with the same 1px falloff. This is what lets the selected-state ring go
// from 2px to 3px without a second function (stroke_w is a parameter, not
// baked into the rasterizer the way the midpoint outline's 1px stroke is).
void draw_circle_ring_aa(int32_t cx, int32_t cy, float r, float stroke_w, uint32_t color, int alpha)
{
    if (r <= 0.0f || stroke_w <= 0.0f || alpha <= 0) return;
    float r_out = r + stroke_w * 0.5f;
    float r_in  = r - stroke_w * 0.5f;
    if (r_in < 0.0f) r_in = 0.0f;
    int32_t r0 = (int32_t)r_out + 2;
    int32_t x0 = cx - r0, x1 = cx + r0;
    int32_t w = x1 - x0 + 1;
    if (w <= 0 || w > DRAW_AA_ROW_MAX) return;
    uint8_t row[DRAW_AA_ROW_MAX];

    for (int32_t y = cy - r0; y <= cy + r0; y++) {
        if (y < g_clip_y0 || y >= g_clip_y1) continue;
        float dy = ((float)y + 0.5f) - (float)cy;
        float dy2 = dy * dy;
        float reach = r_out + 1.0f;
        if (dy2 > reach * reach) continue;
        for (int32_t i = 0; i < w; i++) {
            float dx = ((float)(x0 + i) + 0.5f) - (float)cx;
            float dist = sqrtf(dx * dx + dy2);

            float d_out = dist - r_out;
            float cov_out = 0.5f - d_out;
            if (cov_out < 0.0f) cov_out = 0.0f;
            if (cov_out > 1.0f) cov_out = 1.0f;

            float d_in = dist - r_in;
            float cov_hole = 0.5f - d_in;
            if (cov_hole < 0.0f) cov_hole = 0.0f;
            if (cov_hole > 1.0f) cov_hole = 1.0f;

            float cov = cov_out * (1.0f - cov_hole);
            int32_t a = (int32_t)(cov * (float)alpha + 0.5f);
            row[i] = (uint8_t)(a > 255 ? 255 : a);
        }
        draw_hspan_alpha(x0, y, w, color, row, 1);
    }
}

// (#745 follow-up) Antialiased line segment: this renderer had NO AA-line
// primitive anywhere, so every diagonal glyph (the bluetooth rune, the
// checkmark glyph) was hard-edge Bresenham "by precedent" - a precedent that
// was really just an absence. Same technique as the two functions above:
// per scanline, every pixel's alpha is a coverage function of its TRUE
// distance to the segment (clamped to its two endpoints), composited through
// draw_hspan_alpha(). `width` is the full stroke width (a 1.0-1.6px width is
// typical for a small glyph; the coverage falloff itself is always a fixed
// 1px band straddling the true edge, exactly like the circle pair, so a
// thicker line does not get a softer edge, only a wider solid core).
void draw_line_aa(int32_t x0, int32_t y0, int32_t x1, int32_t y1, float width, uint32_t color, int alpha)
{
    if (alpha <= 0) return;
    if (width < 0.5f) width = 0.5f;
    float fx0 = (float)x0, fy0 = (float)y0, fx1 = (float)x1, fy1 = (float)y1;
    float ddx = fx1 - fx0, ddy = fy1 - fy0;
    float len2 = ddx * ddx + ddy * ddy;
    float halfw = width * 0.5f;
    int32_t pad = (int32_t)(halfw + 2.0f);

    int32_t bx0 = ((x0 < x1) ? x0 : x1) - pad, bx1 = ((x0 < x1) ? x1 : x0) + pad;
    int32_t by0 = ((y0 < y1) ? y0 : y1) - pad, by1 = ((y0 < y1) ? y1 : y0) + pad;
    int32_t w = bx1 - bx0 + 1;
    if (w <= 0 || w > DRAW_AA_ROW_MAX) return;
    uint8_t row[DRAW_AA_ROW_MAX];

    for (int32_t y = by0; y <= by1; y++) {
        if (y < g_clip_y0 || y >= g_clip_y1) continue;
        float py = (float)y + 0.5f;
        for (int32_t i = 0; i < w; i++) {
            float px = (float)(bx0 + i) + 0.5f;
            float t;
            if (len2 < 1.0e-6f) {
                t = 0.0f;
            } else {
                t = ((px - fx0) * ddx + (py - fy0) * ddy) / len2;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
            }
            float cxp = fx0 + t * ddx, cyp = fy0 + t * ddy;
            float dx = px - cxp, dy = py - cyp;
            float dist = sqrtf(dx * dx + dy * dy);
            float cov = 0.5f + halfw - dist;
            if (cov < 0.0f) cov = 0.0f;
            if (cov > 1.0f) cov = 1.0f;
            int32_t a = (int32_t)(cov * (float)alpha + 0.5f);
            row[i] = (uint8_t)(a > 255 ? 255 : a);
        }
        draw_hspan_alpha(bx0, y, w, color, row, 1);
    }
}

// ============================================================================
// Rounded rectangle
// ============================================================================

// Integer square root (Newton) for the per-row corner inset below.
static int32_t dr_isqrt(int32_t n)
{
    if (n <= 0) return 0;
    int32_t x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

void draw_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color)
{
    if (w <= 0 || h <= 0) return;
    // Clamp radius so it fits inside the box.
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) {
        draw_fill_rect(x, y, w, h, color);
        return;
    }

    // Per-row fill: every scanline is ONE contiguous span [x+inset, x+w-inset),
    // where inset follows the corner circle in the top/bottom r-row bands and is
    // 0 in the straight middle. Because each row is a single rect there can be no
    // interior coverage gap. (The old midpoint-circle fill left a triangular
    // "notch" just inside each corner where its steep-octant spans ended one
    // pixel short of the centre column.)
    for (int32_t j = 0; j < h; j++) {
        int32_t d = -1;
        if (j < r)           d = j;          // top corner band (0..r-1)
        else if (j >= h - r) d = h - 1 - j;  // bottom corner band
        int32_t inset = 0;
        if (d >= 0) {
            int32_t dy = r - 1 - d;
            inset = r - dr_isqrt(r * r - dy * dy);
            if (inset < 0) inset = 0;
        }
        int32_t rw = w - 2 * inset;
        if (rw > 0) draw_fill_rect(x + inset, y + j, rw, 1, color);
    }
}

// ============================================================================
// Gradient
// ============================================================================

void draw_gradient_v(int32_t x, int32_t y, int32_t w, int32_t h,
                     uint32_t top, uint32_t bot)
{
    if (w <= 0 || h <= 0) return;

    int32_t tr = (int32_t)CHAN_R(top), tg = (int32_t)CHAN_G(top), tb = (int32_t)CHAN_B(top);
    int32_t br = (int32_t)CHAN_R(bot), bg = (int32_t)CHAN_G(bot), bb = (int32_t)CHAN_B(bot);

    for (int32_t row = 0; row < h; row++) {
        int32_t abs_y = y + row;
        if (abs_y < 0 || abs_y >= g_fb_height) continue;

        // Linear interpolation: t = row / (h-1) in fixed-point (256 steps).
        int32_t t   = (h > 1) ? (row * 256) / (h - 1) : 128;
        int32_t inv = 256 - t;
        uint8_t r   = (uint8_t)((tr * inv + br * t) >> 8);
        uint8_t g_c = (uint8_t)((tg * inv + bg * t) >> 8);
        uint8_t b   = (uint8_t)((tb * inv + bb * t) >> 8);
        uint32_t color = PACK_RGB(r, g_c, b);

        draw_hline(x, abs_y, w, color);
    }
}

// ============================================================================
// Text / font rendering
// ============================================================================

void draw_char(int32_t x, int32_t y, char c, uint32_t color)
{
    int idx = (unsigned char)c;
    if (idx < 0 || idx > 127) idx = 0x20;  // fallback to space

    const uint8_t *glyph = font_data[idx];

    // #uiscale: raw bitmap glyph dims (indexes font_data[128][16], must stay
    // the true 8x16 source size, NOT the scaled FONT_CHAR_W/H used elsewhere
    // for TTF layout math - see compositor.h).
    for (int row = 0; row < FONT_CHAR_H_RAW; row++) {
        int32_t py = y + row;
        if (py < 0 || py >= g_fb_height) continue;
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_CHAR_W_RAW; col++) {
            if (bits & (0x80 >> col)) {
                int32_t px = x + col;
                if (px >= 0 && px < g_fb_width && draw_pt_in_clip(px, py))
                    g_fb[py * g_fb_pitch + px] = color;
            }
        }
    }
}

// Bitmap (8x16) text - kept for anything that needs the fixed-width font.
void draw_text_bitmap(int32_t x, int32_t y, const char *text, uint32_t color)
{
    if (!text) return;
    int32_t cx = x;
    while (*text) {
        draw_char(cx, y, *text, color);
        cx += FONT_CHAR_W_RAW;   // #uiscale: raw glyph advance, see draw_char() above
        text++;
    }
}

// Default UI text now renders in the antialiased TrueType font, so the whole
// desktop chrome (widgets, start menu, taskbar, tray, login) matches the
// desktop icon labels. UI_TTF_SIZE is tuned to the ~16px chrome rows.
#define UI_TTF_SIZE 15
void draw_text(int32_t x, int32_t y, const char *text, uint32_t color)
{
    if (!text) return;
    draw_text_ttf(x, y, text, UI_TTF_SIZE, color);
}

void draw_text_centered(int32_t cx, int32_t y, const char *text, uint32_t color)
{
    if (!text) return;
    int32_t tw = text_width(text);
    draw_text(cx - tw / 2, y, text, color);
}

void draw_text_shadow(int32_t x, int32_t y, const char *text,
                      uint32_t fg, uint32_t shadow)
{
    draw_text(x + 1, y + 1, text, shadow);
    draw_text(x,     y,     text, fg);
}

// ----------------------------------------------------------------------------
// Contrast helpers (#128): pick legible text colors from a known background so
// text stays readable across light AND dark themes instead of hardcoding grays.
// ----------------------------------------------------------------------------
uint32_t draw_luminance(uint32_t c)
{
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    // Rec.601 luma, fixed point: 0.299R + 0.587G + 0.114B
    return (r * 77 + g * 150 + b * 29) >> 8;
}

// Primary readable ink for the given background: near-black on light, near-white on dark.
uint32_t readable_ink(uint32_t bg)
{
    return (draw_luminance(bg) >= 140) ? 0xFF1A1A1A : 0xFFF0F0F0;
}

// Muted secondary ink: the primary ink blended `mix` percent toward the
// background. (#745) The mix used to be hardcoded at 35%, which is right on an
// OPAQUE chrome surface but not on GLASS: the effective background there is the
// tint blended over an arbitrary backdrop, so a 35% mix lands at 3.18:1 in the
// worst case, under the 3.0 floor once the backdrop moves. 22% raises the same
// worst case to 3.83:1. One implementation, one parameter, so the two call
// families cannot drift apart the way two copied functions would.
uint32_t readable_ink_dim_mix(uint32_t bg, int mix)
{
    if (mix < 0)   mix = 0;
    if (mix > 100) mix = 100;
    uint32_t keep = (uint32_t)(100 - mix), m = (uint32_t)mix;
    uint32_t ink = readable_ink(bg);
    uint32_t ir = (ink >> 16) & 0xFF, ig = (ink >> 8) & 0xFF, ib = ink & 0xFF;
    uint32_t br = (bg  >> 16) & 0xFF, bgc= (bg  >> 8) & 0xFF, bb = bg  & 0xFF;
    uint32_t r = (ir * keep + br  * m) / 100;
    uint32_t g = (ig * keep + bgc * m) / 100;
    uint32_t b = (ib * keep + bb  * m) / 100;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

// Muted secondary ink on an OPAQUE surface (the historical 35% mix).
uint32_t readable_ink_dim(uint32_t bg)
{
    return readable_ink_dim_mix(bg, 35);
}

// Keep a colored accent's hue but guarantee it is legible on the given background.
// Dark theme: vivid colors are fine, returned unchanged. Light theme: a too-light
// accent (e.g. gold/cyan/pale-green widget headers) is scaled down in luminance so
// it stays the same hue but reads clearly on a light surface. (#130)
uint32_t readable_accent(uint32_t color, uint32_t bg)
{
    if (draw_luminance(bg) < 140) return color;     // dark surface: leave vivid
    uint32_t lum = draw_luminance(color);
    if (lum <= 110) return color;                   // already dark enough
    uint32_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    uint32_t factor = (100u * 256u) / (lum ? lum : 1);   // target luma ~100, fixed point
    r = (r * factor) >> 8; g = (g * factor) >> 8; b = (b * factor) >> 8;
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

// ----------------------------------------------------------------------------
// #127/#128: ONE shared "tray/system popup" panel primitive, so every popup
// hung off the tray (notifications center, quick-control popups, the AI
// launcher, per-widget menus) renders the SAME chrome instead of each file
// hand-rolling its own rect+border+shadow combination with its own literal
// colors - which is exactly how #128 got filed (traymenu.c's control popups
// used flat hardcoded 0x00-prefixed literals that never tracked the active
// theme at all, while notif.c/launcher.c already used CLR_MENU_* + a rounded
// panel + drop shadow). Matches launcher.c's pre-existing panel treatment
// (the AI prompt panel) and widgets.c's card treatment (the weather widget),
// the two surfaces #128 names as the reference look: rounded rect body in
// CLR_MENU_BG, a soft 2-layer offset drop shadow, a CLR_MENU_BORDER hairline,
// and a 1px translucent top highlight for depth. Callers own their own
// content; this only draws the surface underneath it.
void draw_popup_panel(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius)
{
    int ob = g_draw_blend;
    g_draw_blend = 40;
    draw_rounded_rect(x - 2, y + 6, w + 4, h + 4, radius + 2, 0xFF000000);
    draw_rounded_rect(x,     y + 3, w,     h + 6, radius,     0xFF000000);
    g_draw_blend = 255;
    draw_rounded_rect(x, y, w, h, radius, CLR_MENU_BG);
    g_draw_blend = 30;
    draw_hline(x + radius, y + 1, w - radius * 2, 0xFFFFFFFF);
    g_draw_blend = ob;
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
}

// #127: a NEUTRAL popup action button (e.g. "Clear all") - fill is CLR_MENU_BG
// tinted toward its own ink, text is the same readable_ink() used for every
// other label in a CLR_MENU_BG-family surface. Deliberately NOT a saturated
// accent color: notif.c used to fill this exact button with sev_color(INFO)
// (a fixed blue) under hardcoded white text, which measures 3.68:1 against
// white - below the WCAG AA 4.5:1 text floor on EVERY theme, because the
// severity blue was designed to be used as a small accent (bar/icon/dot via
// readable_accent()), not as a button SURFACE with fixed white text sitting
// on it (a different contrast contract nothing was checking). This
// construction is provably >=7.6:1 on all 14 shipping themes (measured via a
// host-side port of this exact math against every /THEMES/*.mtheme menu_bg -
// see docs/TRAY_POPUP_DESIGN_LANGUAGE.md's contrast table), because both the
// fill and the ink are derived from the SAME per-theme CLR_MENU_BG token
// instead of a fixed literal that has to independently work everywhere.
// Text is centered using the REAL measured glyph width (text_width_ttf), not
// a guessed inset - the bug that made "Clear all" look nudged/off-center.
void draw_popup_button_centered(int32_t x, int32_t y, int32_t w, int32_t h,
                                int32_t radius, const char *label, int ttf_size)
{
    uint32_t fill = readable_ink_dim_mix(CLR_MENU_BG, 88);   // subtle wash, bg-dominant
    uint32_t ink  = readable_ink(CLR_MENU_BG);
    draw_rounded_rect(x, y, w, h, radius, fill);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    int tw = text_width_ttf(label, ttf_size);
    int fm[3]; int th = ttf_size;
    // #uiscale: font_metrics() is a direct kernel call (libc/syscall.h,
    // off-limits here), not routed through draw_text_ttf()/text_width_ttf()
    // above, so it needs its own ui_px() to stay proportional to what those
    // two actually draw/measure at this same logical ttf_size.
    if (font_metrics(0, ui_px(ttf_size), fm) == 0) th = fm[0] - fm[1];   // {ascent,descent,line_gap}
    int tx = x + (w - tw) / 2;
    int ty = y + (h - th) / 2;
    draw_text_ttf(tx, ty, label, ttf_size, ink);
}

// ============================================================================
// Scaled character rendering
// ============================================================================

void draw_char_large(int32_t x, int32_t y, char c, uint32_t color, int scale)
{
    if (scale <= 0) scale = 1;
    int idx = (unsigned char)c;
    if (idx < 0 || idx > 127) idx = 0x20;

    const uint8_t *glyph = font_data[idx];

    // #uiscale: raw bitmap glyph dims, same reasoning as draw_char() above.
    for (int row = 0; row < FONT_CHAR_H_RAW; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_CHAR_W_RAW; col++) {
            if (bits & (0x80 >> col)) {
                int32_t bx = x + col * scale;
                int32_t by = y + row * scale;
                // Draw a scale x scale block for each set bit.
                for (int sy = 0; sy < scale; sy++) {
                    int32_t py = by + sy;
                    if (py < 0 || py >= g_fb_height) continue;
                    for (int sx = 0; sx < scale; sx++) {
                        int32_t px = bx + sx;
                        if (px >= 0 && px < g_fb_width && draw_pt_in_clip(px, py))
                            g_fb[py * g_fb_pitch + px] = color;
                    }
                }
            }
        }
    }
}

void draw_text_large(int32_t x, int32_t y, const char *text, uint32_t color, int scale)
{
    if (!text || scale <= 0) return;
    int32_t cx = x;
    while (*text) {
        draw_char_large(cx, y, *text, color, scale);
        cx += FONT_CHAR_W_RAW * scale;   // #uiscale: raw glyph advance, see draw_char() above
        text++;
    }
}

// ============================================================================
// Text width helpers
// ============================================================================

int text_width(const char *text)
{
    if (!text) return 0;
    return text_width_ttf(text, UI_TTF_SIZE);   // match the TTF draw_text
}

int text_width_large(const char *text, int scale)
{
    if (!text || scale <= 0) return 0;
    return text_width(text) * scale;
}


// ============================================================================
// #745 GLASS: translucent chrome surfaces with a blurred backdrop.
// Ported from docs/DOCK_XFCE_GLASS.html sections 4, 6 and 7. That document is
// the authority for every constant here; nothing below was re-derived.
//
// There is no GPU, no shader and no backdrop-filter in this system, so the
// "blur(14px)" in the design is produced on the CPU by an integer-only,
// no-FPU, no-allocation pipeline:
//
//   0. source   g_fb, already holding wallpaper + windows, BEFORE any chrome
//   1. inflate  the surface rect by GLASS_BLEED on every side that is not a
//               screen edge (clamping at the edge is what makes a screen-edge
//               side get no bleed, which is exactly the spec's rule)
//   2. downsample x4 into three uint16 planes (R, G, B)
//   3. blur     3 separable box passes at r=3 (w=7), running sum, and a
//               reciprocal multiply instead of a divide
//   4. upscale  bilinear x4, then tint through the EXISTING draw_blend()
//   5. store    into the surface's cache strip, then blit that strip to g_fb
//
// A box of width w has variance (w^2-1)/12, so w=7 gives 4 per pass in
// quarter-pixel units; three passes give 12, which is 12*16 = 192 in
// full-resolution pixels. The 4x4 downsample box itself contributes
// (16-1)/12 = 1.25. Total variance 193.25, sigma 13.90, i.e. blur(14px).
//
// THE ONE RULE THAT SILENTLY CORRUPTS THE EFFECT IF BROKEN: step 0 may read
// g_fb ONLY during a full render_frame(). The partial paths
// (render_frame_cursor / render_frame_idle) recomposite a CLIPPED region only,
// so under a chrome surface g_fb is stale or half-written; blurring it would
// feed chrome back into the glass and compound every frame. That rule is
// enforced structurally below rather than by comment: g_glass_live is tested
// ONCE, at the top of glass_render(), and every g_fb read lives inside that
// branch. A partial pass can only ever blit an existing strip.
// ============================================================================

#define GLASS_DS                4      // downsample factor, both axes (power of 2)
#define GLASS_RADIUS            3      // box radius at 1/4 scale -> w = 2r+1 = 7
#define GLASS_PASSES            3      // H+V pairs
#define GLASS_RECIP             9363   // (1<<16)/7: sum*9363>>16 replaces the divide
#define GLASS_BLEED             36     // 2.6 sigma, rounded to a multiple of GLASS_DS
#define GLASS_BLUR_SKIP_ABOVE   96     // above this opacity the backdrop swings
                                       // only 10/255: skip the blur entirely
#define GLASS_MIN_RECOMPUTE_MS  100    // at most 10 recomputes/second per surface
#define GLASS_DIM_MIX           22     // readable_ink_dim mix on glass (vs 35 opaque)

// The user preference (percent OPAQUE), NOT a theme key.
// DOCK_OPACITY_MIN..DOCK_OPACITY_MAX (dock_opacity.h), default DOCK_OPACITY_DEFAULT.
// Owned here because the glass composite is its only consumer; main.c drives
// the live-apply file and profile.c the persistence.
//
// (#745 dockgrey, 2026-08-12) Was 60..100 default 90 (an earlier explicit user
// decision, see blame.md). USER-REPORTED: "on the marble dock, there's not
// enough transparency ... the same is true for the taskbar - matching say the
// weather widget", which supersedes that decision for the glass-enabled
// surfaces (Marble/DOCK_XFCE panel+dock, the classic DOCK_DEFAULT taskbar,
// Lumina, the start menu). The floor moved from 60 to 70 in the SAME change,
// not independently: lightening CLR_GLASS_TINT's dark-theme derivation
// (glass_theme_apply() in main.c, 58% -> 78%) trades away low-opacity margin
// against a bright wallpaper, and a full black/white backdrop sweep (see
// glass_render()'s floor comment below) showed the OLD 60 floor was already
// under the 4.5:1 WCAG text floor for every dark-bar glass theme once you
// actually sweep to a white backdrop instead of trusting the single number
// that had been quoted for it - not a regression this change introduces, a
// pre-existing gap this change's own measurement pass happened to expose.
//
// (#132) The 70 THAT USED TO LIVE HERE was a hard clamp: a value below it was
// silently raised back to 70 at render time, which is why the owner reported
// none of his below-70 opacity requests appeared to do anything. The 4.62:1
// (Ocean)/4.59:1 (retro_unix flat path) measurements below are still true and
// still the reason DOCK_OPACITY_WARN in dock_opacity.h is 73, not 0 - they are
// now a WARNING threshold in Settings, not an enforced floor. Anything down to
// DOCK_OPACITY_MIN renders exactly as set; contrast below DOCK_OPACITY_WARN is
// the user's explicit trade-off.
int g_dock_opacity = DOCK_OPACITY_DEFAULT;

// Set to 1 by render_frame() around the full-frame body, and by nothing else.
// See the rule above.
int g_glass_live = 0;

// Tier-2 auto-downgrade threshold in MEASURED milliseconds for one cold
// recompute. A variable rather than a #define so a verification build can set
// it to 0 and prove the downgrade actually fires, instead of asserting that it
// would (see #745 report).
int g_glass_downgrade_ms = 4;

// Monotonic backdrop epoch. Bumped once per full frame by render_frame(); the
// per-surface rate limit is what bounds the cost, and a cheap sparse signature
// of the source rect is what avoids recomputing when nothing actually moved.
uint64_t g_backdrop_epoch = 0;

void glass_bump_epoch(void) { g_backdrop_epoch++; }

// --- scratch: 1/4-scale planes -------------------------------------------
// Worst case is the start menu at its 420px clamp on a tall screen:
// (420+72)/4 = 123 wide by (1080+72)/4 = 288 high = 35424. The default taskbar
// at 1920 wide is (1920)/4 = 480 by (36+36)/4 = 18 = 8640.
#define GLASS_SCRATCH_PX   40000
#define GLASS_ROW_MAX      512         // max 1/4-scale width: (1920+72)/4 = 498
static uint16_t g_gs_r[GLASS_SCRATCH_PX];
static uint16_t g_gs_g[GLASS_SCRATCH_PX];
static uint16_t g_gs_b[GLASS_SCRATCH_PX];
static uint16_t g_gs_t[GLASS_SCRATCH_PX];     // ping-pong temp, one plane at a time

// Per-output-column bilinear indices, rebuilt only when the surface width or
// origin changes. Hoisted out of the pixel loop: the same triple applies to
// every row, so recomputing it per pixel was pure waste.
static int32_t  g_gs_q0[MAX_SCREEN_W];
static int32_t  g_gs_q1[MAX_SCREEN_W];
static uint8_t  g_gs_fx[MAX_SCREEN_W];

// --- per-surface cache ----------------------------------------------------
// Each surface owns a full-resolution, ALREADY-TINTED ARGB strip, so the hit
// path is a pure copy with no arithmetic. Fixed slices rather than a bump
// allocator: a bump allocator's failure mode depends on which surface happens
// to draw first, which is exactly the kind of order-dependent bug this tree
// has been bitten by before.
#define GLASS_PANEL_PX   (MAX_SCREEN_W * 40)    // taskbar 36 / xfce panel 30
// (#123 item 2) Was MAX_SCREEN_W * 72 for a fixed 64px dock. The marble dock's
// height is a user preference now (44..96, taskbar.c XFCE_DOCK_H_MAX), and
// glass_render() SILENTLY falls back to a flat tint for any surface whose
// w*h exceeds its cache slice - so a slice sized for 72 would have made the
// dock lose its blur, with no error anywhere, for every height above 72. Sized
// to the ceiling plus the same 8px of slack the old value carried over 64.
#define GLASS_DOCK_PX    (MAX_SCREEN_W * 104)   // xfce dock, max height 96
#define GLASS_MENU_PX    (432 * 1080)           // start menu, 420 clamp + slack
// (#glassmodal) Shared by the confirm/shutdown modal card AND the CPU/RAM/
// DSK/NET perf pop-out (GLASS_SURF_MODAL, compositor.h) - the two are never
// open at once in practice, so one slot covers both. Computed worst case:
// confirm dialog at 200% UI scale is roughly CARD_W=ui_px(360) wide by
// card_h() tall (grows with wrapped line count, capped at CONFIRM_MAX_LINES)
// - about 720 x 280 at 2x; the perf pop-out is a fixed, currently-unscaled
// 252 x 230. 900 x 500 = 450000 covers both with real margin. If this is
// ever wrong, glass_render()'s own existing guard (w*h > s->cap -> flat tint
// fallback, never a crash - see the "op > GLASS_BLUR_SKIP_ABOVE || s->tier
// >= 3 || (int32_t)(w * h) > s->cap" check above) degrades gracefully rather
// than corrupting anything - verified that guard is still exactly this
// shape at the time this slot was added.
#define GLASS_MODAL_PX   (900 * 500)
static uint32_t g_glass_panel[GLASS_PANEL_PX];
static uint32_t g_glass_dock [GLASS_DOCK_PX];
static uint32_t g_glass_menu [GLASS_MENU_PX];
static uint32_t g_glass_modal[GLASS_MODAL_PX];

typedef struct {
    uint32_t *buf;
    int32_t   cap;
    int32_t   x, y, w, h;      // geometry the strip was built for
    uint32_t  tint;
    int32_t   alpha;
    uint32_t  sig;             // sparse signature of the source rect
    uint64_t  epoch;           // g_backdrop_epoch the strip was built at
    uint64_t  last_ms;         // uptime_ms() of the last recompute
    int       valid;
    int       tier;            // 1 = live backdrop, 2 = downgraded
    // instrumentation (see glass_perf_get)
    uint32_t  cold_n, cold_ms, cold_worst;
    uint32_t  hit_n;
} glass_surface_t;

static glass_surface_t g_glass[GLASS_SURF_COUNT] = {
    { g_glass_panel, GLASS_PANEL_PX, 0,0,0,0, 0,0,0,0,0, 0, 1, 0,0,0, 0 },
    { g_glass_dock,  GLASS_DOCK_PX,  0,0,0,0, 0,0,0,0,0, 0, 1, 0,0,0, 0 },
    { g_glass_menu,  GLASS_MENU_PX,  0,0,0,0, 0,0,0,0,0, 0, 1, 0,0,0, 0 },
    { g_glass_modal, GLASS_MODAL_PX, 0,0,0,0, 0,0,0,0,0, 0, 1, 0,0,0, 0 },
};

void glass_invalidate_all(void)
{
    for (int i = 0; i < GLASS_SURF_COUNT; i++) {
        g_glass[i].valid = 0;
        g_glass[i].tier  = 1;
    }
}

int glass_perf_get(int surf, uint32_t *cold_n, uint32_t *cold_ms,
                   uint32_t *cold_worst, uint32_t *hit_n, int *tier)
{
    if (surf < 0 || surf >= GLASS_SURF_COUNT) return 0;
    const glass_surface_t *s = &g_glass[surf];
    if (cold_n)     *cold_n     = s->cold_n;
    if (cold_ms)    *cold_ms    = s->cold_ms;
    if (cold_worst) *cold_worst = s->cold_worst;
    if (hit_n)      *hit_n      = s->hit_n;
    if (tier)       *tier       = s->tier;
    return 1;
}

// --- step 1: source rect --------------------------------------------------
// Inflate by GLASS_BLEED and clamp to the screen. Clamping is what implements
// "bleed on every side that is not a screen edge": a side flush with the edge
// clamps back to itself and gets none. SKIPPING THE BLEED IS THE SINGLE MOST
// LIKELY PORTING BUG: without it the blur near the surface's inner edge sees
// only surface-region pixels and produces a dark vignette along that edge.
static void glass_source_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                              int32_t *sx0, int32_t *sy0, int32_t *sw, int32_t *sh)
{
    int32_t x0 = x - GLASS_BLEED, y0 = y - GLASS_BLEED;
    int32_t x1 = x + w + GLASS_BLEED, y1 = y + h + GLASS_BLEED;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_fb_width)  x1 = g_fb_width;
    if (y1 > g_fb_height) y1 = g_fb_height;
    *sx0 = x0; *sy0 = y0; *sw = x1 - x0; *sh = y1 - y0;
}

// A cheap FNV-1a over a sparse sample of the source rect (every 4th row, every
// 16th column: ~1/64 of the pixels). This is the "did the backdrop actually
// change" test. Without it, a desktop with one window open takes a full frame
// every tick and would recompute at the 10Hz rate limit forever even though
// nothing behind the panel moved.
static uint32_t glass_sig(int32_t sx0, int32_t sy0, int32_t sw, int32_t sh)
{
    uint32_t hsh = 2166136261u;
    for (int32_t yy = 0; yy < sh; yy += 4) {
        const uint32_t *row = g_fb + (int32_t)(sy0 + yy) * g_fb_pitch + sx0;
        for (int32_t xx = 0; xx < sw; xx += 16)
            hsh = (hsh ^ (row[xx] & 0x00FFFFFFu)) * 16777619u;
    }
    return hsh;
}

// --- step 2: downsample x4 into three uint16 planes -----------------------
static void glass_downsample(int32_t sx0, int32_t sy0, int32_t sw, int32_t sh,
                             int32_t dw, int32_t dh)
{
    for (int32_t by = 0; by < dh; by++) {
        int32_t y0 = by * GLASS_DS, y1 = y0 + GLASS_DS;
        if (y1 > sh) y1 = sh;
        for (int32_t bx = 0; bx < dw; bx++) {
            int32_t x0 = bx * GLASS_DS, x1 = x0 + GLASS_DS;
            if (x1 > sw) x1 = sw;
            uint32_t ar = 0, ag = 0, ab = 0, n = 0;
            for (int32_t yy = y0; yy < y1; yy++) {
                const uint32_t *row = g_fb + (int32_t)(sy0 + yy) * g_fb_pitch + sx0;
                for (int32_t xx = x0; xx < x1; xx++) {
                    uint32_t c = row[xx];
                    ar += (c >> 16) & 0xFF;
                    ag += (c >>  8) & 0xFF;
                    ab +=  c        & 0xFF;
                }
            }
            n = (uint32_t)((y1 - y0) * (x1 - x0));
            int32_t i = by * dw + bx;
            if (n == 16) {                    // the overwhelmingly common case
                g_gs_r[i] = (uint16_t)(ar >> 4);
                g_gs_g[i] = (uint16_t)(ag >> 4);
                g_gs_b[i] = (uint16_t)(ab >> 4);
            } else if (n) {
                g_gs_r[i] = (uint16_t)(ar / n);
                g_gs_g[i] = (uint16_t)(ag / n);
                g_gs_b[i] = (uint16_t)(ab / n);
            } else {
                g_gs_r[i] = g_gs_g[i] = g_gs_b[i] = 0;
            }
        }
    }
}

// --- step 3: separable box blur, running sum, clamp-to-edge ---------------
// sum maxes at 7*255 = 1785; 1785*9363 = 16712955, which is < 2^32 and
// >> 16 == 255, so the reciprocal neither overflows nor clips low.
static void glass_box_h(const uint16_t *src, uint16_t *dst, int32_t dw, int32_t dh)
{
    const int32_t r = GLASS_RADIUS;
    for (int32_t y = 0; y < dh; y++) {
        const uint16_t *s = src + y * dw;
        uint16_t *d = dst + y * dw;
        uint32_t sum = (uint32_t)s[0] * (uint32_t)(r + 1);
        for (int32_t i = 1; i <= r; i++) sum += s[i < dw ? i : dw - 1];
        for (int32_t x = 0; x < dw; x++) {
            d[x] = (uint16_t)((sum * GLASS_RECIP) >> 16);
            int32_t ai = x + r + 1; if (ai > dw - 1) ai = dw - 1;
            int32_t si = x - r;     if (si < 0)      si = 0;
            sum += s[ai];
            sum -= s[si];
        }
    }
}

static void glass_box_v(const uint16_t *src, uint16_t *dst, int32_t dw, int32_t dh)
{
    const int32_t r = GLASS_RADIUS;
    for (int32_t x = 0; x < dw; x++) {
        const uint16_t *s = src + x;
        uint16_t *d = dst + x;
        uint32_t sum = (uint32_t)s[0] * (uint32_t)(r + 1);
        for (int32_t i = 1; i <= r; i++) sum += s[(int32_t)(i < dh ? i : dh - 1) * dw];
        for (int32_t y = 0; y < dh; y++) {
            d[y * dw] = (uint16_t)((sum * GLASS_RECIP) >> 16);
            int32_t ai = y + r + 1; if (ai > dh - 1) ai = dh - 1;
            int32_t si = y - r;     if (si < 0)      si = 0;
            sum += s[ai * dw];
            sum -= s[si * dw];
        }
    }
}

static void glass_blur_plane(uint16_t *plane, int32_t dw, int32_t dh)
{
    for (int p = 0; p < GLASS_PASSES; p++) {
        glass_box_h(plane, g_gs_t, dw, dh);
        glass_box_v(g_gs_t, plane, dw, dh);
    }
}

// --- step 4/5: bilinear upscale + tint into the cache strip ---------------
// Plane cell p covers source pixels [4p, 4p+3], so its centre is at source
// coordinate 4p + 1.5. The inverse, for source pixel s, is (s - 1.5)/4, which
// in 8.8 fixed point is s*64 - 96.
static void glass_tint_into(glass_surface_t *s, int32_t sx0, int32_t sy0,
                            int32_t dw, int32_t dh, uint32_t tint, int32_t alpha)
{
    static uint16_t rowr[GLASS_ROW_MAX], rowg[GLASS_ROW_MAX], rowb[GLASS_ROW_MAX];
    int32_t w = s->w, h = s->h;

    // Per-column indices, once for the whole surface.
    for (int32_t ox = 0; ox < w; ox++) {
        int32_t sx  = (s->x + ox) - sx0;
        int32_t pos = sx * 64 - 96;
        int32_t q0, fx;
        if (pos < 0) { q0 = 0; fx = 0; }
        else { q0 = pos >> 8; fx = pos & 255; }
        if (q0 >= dw - 1) { q0 = dw - 1; fx = 0; }
        g_gs_q0[ox] = q0;
        g_gs_q1[ox] = (q0 + 1 < dw) ? q0 + 1 : q0;
        g_gs_fx[ox] = (uint8_t)fx;
    }

    for (int32_t oy = 0; oy < h; oy++) {
        int32_t sy  = (s->y + oy) - sy0;
        int32_t pos = sy * 64 - 96;
        int32_t p0, fy;
        if (pos < 0) { p0 = 0; fy = 0; }
        else { p0 = pos >> 8; fy = pos & 255; }
        if (p0 >= dh - 1) { p0 = dh - 1; fy = 0; }
        int32_t p1 = (p0 + 1 < dh) ? p0 + 1 : p0;
        int32_t ify = 256 - fy;

        // Vertical lerp of the two plane rows once, reused by every column.
        const uint16_t *a_r = g_gs_r + p0 * dw, *b_r = g_gs_r + p1 * dw;
        const uint16_t *a_g = g_gs_g + p0 * dw, *b_g = g_gs_g + p1 * dw;
        const uint16_t *a_b = g_gs_b + p0 * dw, *b_b = g_gs_b + p1 * dw;
        for (int32_t i = 0; i < dw; i++) {
            rowr[i] = (uint16_t)(((uint32_t)a_r[i] * ify + (uint32_t)b_r[i] * fy) >> 8);
            rowg[i] = (uint16_t)(((uint32_t)a_g[i] * ify + (uint32_t)b_g[i] * fy) >> 8);
            rowb[i] = (uint16_t)(((uint32_t)a_b[i] * ify + (uint32_t)b_b[i] * fy) >> 8);
        }

        uint32_t *out = s->buf + oy * w;
        for (int32_t ox = 0; ox < w; ox++) {
            int32_t q0 = g_gs_q0[ox], q1 = g_gs_q1[ox];
            uint32_t fx = g_gs_fx[ox], ifx = 256u - fx;
            uint32_t r = ((uint32_t)rowr[q0] * ifx + (uint32_t)rowr[q1] * fx) >> 8;
            uint32_t g = ((uint32_t)rowg[q0] * ifx + (uint32_t)rowg[q1] * fx) >> 8;
            uint32_t b = ((uint32_t)rowb[q0] * ifx + (uint32_t)rowb[q1] * fx) >> 8;
            uint32_t blurred = (r << 16) | (g << 8) | b;
            // Tint OVER the blurred backdrop, through the one blend this file
            // already has. Tinting first and blurring after would blur the tint
            // into the backdrop and desaturate the result.
            out[ox] = 0xFF000000u | draw_blend(blurred, tint, alpha);
        }
    }
}

// Copy a finished strip into g_fb, clamped to the screen AND to the current
// clip rect, like every other primitive in this file.
static void glass_blit(const glass_surface_t *s)
{
    int32_t x0 = s->x, y0 = s->y, x1 = s->x + s->w, y1 = s->y + s->h;
    if (x0 < g_clip_x0) x0 = g_clip_x0;
    if (y0 < g_clip_y0) y0 = g_clip_y0;
    if (x1 > g_clip_x1) x1 = g_clip_x1;
    if (y1 > g_clip_y1) y1 = g_clip_y1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_fb_width)  x1 = g_fb_width;
    if (y1 > g_fb_height) y1 = g_fb_height;
    for (int32_t y = y0; y < y1; y++) {
        const uint32_t *src = s->buf + (int32_t)(y - s->y) * s->w + (x0 - s->x);
        uint32_t *dst = g_fb + (int32_t)y * g_fb_pitch + x0;
        for (int32_t x = 0; x < x1 - x0; x++) dst[x] = src[x];
    }
}

// Tier 3 / skip-blur path: the tint alone, over whatever is behind, through
// the existing blend. Still translucent, still reads as a material, costs one
// blended fill.
static void glass_flat(int32_t x, int32_t y, int32_t w, int32_t h,
                       uint32_t tint, int32_t alpha)
{
    int ob = g_draw_blend;
    g_draw_blend = (alpha >= 255) ? 255 : (int)alpha;
    draw_fill_rect(x, y, w, h, tint);
    g_draw_blend = ob;
}

// ---------------------------------------------------------------------------
// glass_render: the single entry point. `surf` selects the cache slot.
// ---------------------------------------------------------------------------
void glass_render(int32_t x, int32_t y, int32_t w, int32_t h,
                  uint32_t tint, int surf)
{
    if (w <= 0 || h <= 0) return;
    if (surf < 0 || surf >= GLASS_SURF_COUNT) { glass_flat(x, y, w, h, tint, 255); return; }

    int op = g_dock_opacity;
    // (#745 dockgrey, 2026-08-12) Floor raised 60 -> 70 in the SAME change that
    // lightened CLR_GLASS_TINT's dark-branch derivation (58% -> 78%, main.c
    // glass_theme_apply()). The floor is DERIVED from the tint, not independent
    // of it: a lighter tint at a low opacity lets more of a bright wallpaper
    // wash through, which is exactly what a black/white backdrop sweep (the
    // ink is the single fixed mark here, so the two extremes bound the whole
    // range - see blame.md) has to catch. Measured worst case at op=70 with the
    // new tint, across all 11 glass-enabled shipped themes: 4.62:1 (Ocean,
    // white backdrop), which clears the WCAG AA 4.5:1 text floor with a small
    // margin. The OLD floor's own "4.75:1" claim did NOT hold under this same
    // sweep against the OLD tint either (Ocean measured 3.74:1, Maytera Dark
    // 4.16:1 at op=60 against a white backdrop) - it had evidently only ever
    // been checked against one backdrop, not swept. Re-run
    // /tmp/dockgrey_harness.py (or its equivalent) before ever changing either
    // number again; do not hand-recompute this ratio.
    //
    // (#132) THIS USED TO BE A HARD CLAMP TO 70. The 4.62:1 measurement above
    // is real and is why DOCK_OPACITY_WARN (dock_opacity.h) is 73, but forcing
    // every value below it back up to 70 is exactly why the owner's "opacity
    // below 70%" requests never visibly did anything - this was the one place
    // that silently won the argument every time. The owner's standing decision
    // is a warning, not a second clamp: honour anything down to
    // DOCK_OPACITY_MIN, and let Settings carry the contrast risk in its label.
    if (op < DOCK_OPACITY_MIN) op = DOCK_OPACITY_MIN;
    if (op > DOCK_OPACITY_MAX) op = DOCK_OPACITY_MAX;
    // ROUND, do not truncate. op*255/100 truncates: 90% gives 229 where the
    // spec's alpha ladder says 230, because 90*255/100 is 229.5 exactly. The
    // difference is one level and it never threatens the floor, but rounding
    // is what the spec states and it costs nothing.
    int32_t alpha = (int32_t)((op * 255 + 50) / 100);

    glass_surface_t *s = &g_glass[surf];

    // Above GLASS_BLUR_SKIP_ABOVE the backdrop swings 10 levels out of 255,
    // which is not distinguishable from a flat fill, so the whole pipeline
    // disappears for anyone who nudges the slider toward opaque. Same for a
    // surface that has been downgraded past tier 2, and for a surface too big
    // for its cache slice.
    if (op > GLASS_BLUR_SKIP_ABOVE || s->tier >= 3 || (int32_t)(w * h) > s->cap) {
        glass_flat(x, y, w, h, tint, alpha);
        return;
    }

    int geom_same = (s->valid && s->x == x && s->y == y && s->w == w && s->h == h &&
                     s->tint == tint && s->alpha == alpha);

    // ---- THE HARD RULE, ENFORCED STRUCTURALLY --------------------------
    // Everything below that reads g_fb is inside this branch. A clipped pass
    // (render_frame_cursor / render_frame_idle) can only blit an existing
    // strip; if it has none, it falls back to the flat tint for that frame
    // and the next full frame builds the strip properly.
    if (!g_glass_live) {
        if (geom_same) { s->hit_n++; glass_blit(s); }
        else           glass_flat(x, y, w, h, tint, alpha);
        return;
    }

    int32_t sx0, sy0, sw, sh;
    glass_source_rect(x, y, w, h, &sx0, &sy0, &sw, &sh);
    if (sw <= 0 || sh <= 0) { glass_flat(x, y, w, h, tint, alpha); return; }

    int32_t dw = (sw + GLASS_DS - 1) / GLASS_DS;
    int32_t dh = (sh + GLASS_DS - 1) / GLASS_DS;
    if (dw < 2) dw = 2;
    if (dh < 2) dh = 2;
    if (dw > GLASS_ROW_MAX || dw * dh > GLASS_SCRATCH_PX) {
        glass_flat(x, y, w, h, tint, alpha);
        return;
    }

    uint32_t sig = glass_sig(sx0, sy0, sw, sh);
    if (geom_same && sig == s->sig) {       // backdrop genuinely unchanged
        s->hit_n++;
        glass_blit(s);
        return;
    }

    // Rate limit. Even with a video playing behind the panel, a surface
    // recomputes at most ten times a second and reuses its stale strip in
    // between; glass lagging 100ms behind moving content is not perceptible,
    // and this is what bounds the worst case instead of leaving it open-ended.
    // Tier 2 (auto-downgraded) uses a 50x longer interval, so a slow machine
    // gets a flat-costed panel rather than a slow desktop.
    uint64_t now = uptime_ms();
    uint64_t interval = (s->tier >= 2) ? (GLASS_MIN_RECOMPUTE_MS * 50)
                                       : GLASS_MIN_RECOMPUTE_MS;
    if (geom_same && s->last_ms != 0 && (now - s->last_ms) < interval) {
        s->hit_n++;
        glass_blit(s);
        return;
    }

    // ---- cold recompute ------------------------------------------------
    s->x = x; s->y = y; s->w = w; s->h = h;
    s->tint = tint; s->alpha = alpha;

    glass_downsample(sx0, sy0, sw, sh, dw, dh);
    glass_blur_plane(g_gs_r, dw, dh);
    glass_blur_plane(g_gs_g, dw, dh);
    glass_blur_plane(g_gs_b, dw, dh);
    glass_tint_into(s, sx0, sy0, dw, dh, tint, alpha);

    uint64_t took = uptime_ms() - now;
    s->cold_n++;
    s->cold_ms += (uint32_t)took;
    if ((uint32_t)took > s->cold_worst) s->cold_worst = (uint32_t)took;

    s->sig     = sig;
    s->epoch   = g_backdrop_epoch;
    s->last_ms = now;
    s->valid   = 1;

    // A measured cold recompute over budget drops this surface to tier 2 for
    // the rest of the session. Compared against a variable, not a constant, so
    // a verification build can force it and prove it fires.
    if (s->tier == 1 && (int)took >= g_glass_downgrade_ms)
        s->tier = 2;

    glass_blit(s);
}

// ============================================================================
// (#glassmodal) Shared glass/chrome helpers, promoted from taskbar.c where
// they used to be `static` (~lines 130-195 there, next to xfce_dock_paint_
// rounded()'s neighbors). Moved here, next to glass_render(), so
// confirmdialog.c and taskbar.c's draw_perf_popup() can call the SAME
// functions instead of hand-rolling third copies (CLAUDE.md: improve the
// shared primitive, do not fork). No behavior change: every existing
// taskbar.c call site resolves identically, just via compositor.h's
// prototype instead of a file-local static. CLR_GLASS_TINT, CLR_TASKBAR_BG,
// g_glass_enable, g_dock_opacity and DOCK_OPACITY_MIN/MAX were already
// extern-shared globals before this move, so this is mechanical.
// ============================================================================

// (#123 item 4) THE FLAT-PATH OPACITY FLOOR. See taskbar.c's original
// comment (still there, this is only the definition move) for the full
// measurement history: 73 was the smallest value where every flat-lineage
// theme cleared the WCAG 4.5:1 text floor against a worst-case backdrop; it
// is no longer an enforced clamp (DOCK_OPACITY_WARN carries the warning in
// Settings instead), so this only floors to DOCK_OPACITY_MIN/MAX.
int flat_chrome_alpha(void)
{
    int op = g_dock_opacity;
    if (op < DOCK_OPACITY_MIN) op = DOCK_OPACITY_MIN;
    if (op > DOCK_OPACITY_MAX) op = DOCK_OPACITY_MAX;
    return (op * 255 + 50) / 100;   // ROUND, matching glass_render()
}

// (#745) tb_lighten_argb() stays local to taskbar.c (it is used only by the
// flat fallback bands below and by nothing this move needs to share); reused
// here via its taskbar.c definition would create a cross-file static call,
// so the three-band flat fallback is inlined with the same math instead of
// importing that helper. Percentage lighten toward white, matching
// tb_lighten_argb()'s exact formula.
static uint32_t glass_flat_lighten(uint32_t c, int pct)
{
    int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
    r += (255 - r) * pct / 100;
    g += (255 - g) * pct / 100;
    b += (255 - b) * pct / 100;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void glass_or_flat(int32_t x, int32_t y, int32_t w, int32_t h, int surf)
{
    if (g_glass_enable) { glass_render(x, y, w, h, CLR_GLASS_TINT, surf); return; }
    // Tier 4 (opaque fallback) only: a subtle 3-band top-light gradient
    // instead of one dead-flat fill, each band a PERCENTAGE lighten of the
    // theme's own taskbar_bg - see docs/TASKBAR_AND_TRAY.html section 4.
    int ob = g_draw_blend;
    g_draw_blend = flat_chrome_alpha();
    if (h >= 8) {
        draw_fill_rect(x, y,     w, 4, glass_flat_lighten(CLR_TASKBAR_BG, 10));
        draw_fill_rect(x, y + 4, w, 4, glass_flat_lighten(CLR_TASKBAR_BG, 4));
        draw_fill_rect(x, y + 8, w, h - 8, CLR_TASKBAR_BG);
    } else {
        draw_fill_rect(x, y, w, h, CLR_TASKBAR_BG);
    }
    g_draw_blend = ob;
}

// One hairline of the surface edge, at alpha 128 when the surface is glass and
// fully opaque when it is not.
void glass_edge_h(int32_t x, int32_t y, int32_t w, uint32_t c)
{
    int ob = g_draw_blend;
    if (g_glass_enable) g_draw_blend = 128;
    draw_hline(x, y, w, c);
    g_draw_blend = ob;
}
void glass_edge_v(int32_t x, int32_t y, int32_t h, uint32_t c)
{
    int ob = g_draw_blend;
    if (g_glass_enable) g_draw_blend = 128;
    draw_vline(x, y, h, c);
    g_draw_blend = ob;
}
// Inner highlight: white at alpha 26 on a dark material, 140 on a light one.
void glass_highlight_h(int32_t x, int32_t y, int32_t w)
{
    if (!g_glass_enable) return;
    int ob = g_draw_blend;
    g_draw_blend = (draw_luminance(CLR_GLASS_TINT) >= 140) ? 140 : 26;
    draw_hline(x, y, w, 0xFFFFFFFF);
    g_draw_blend = ob;
}

// ============================================================================
// (#glassmodal) AA corner rounding for a FLOATING rect, any corner mask.
//
// taskbar.c's xfce_dock_paint_rounded()/xfce_corner_coverage() already prove
// this technique for a rect flush to the screen edge (only its top two
// corners are ever rounded there). This generalizes the same capture/paint/
// restore sequence to all four corners for a panel with no flush edge - the
// confirm/shutdown modal and the perf pop-out both need that.
//
// corner_coverage_aa() is a DELIBERATE small duplication of taskbar.c's
// xfce_corner_coverage() (same four-line formula), not a shared call,
// specifically so this file's corner primitive has no new cross-file
// dependency on taskbar.c while the concurrent chromescale pass is also
// editing that file's geometry (per this task's scope brief). If the two
// ever need to change, change both; they are proven identical as of this
// commit.
// ============================================================================
static float corner_coverage_aa(int32_t dist_x, int32_t dist_y, int32_t r)
{
    float fdx = (float)(r - dist_x);
    float fdy = (float)(r - dist_y);
    float dist = sqrtf(fdx * fdx + fdy * fdy);
    float cov = 0.5f - (dist - (float)r);
    if (cov < 0.0f) cov = 0.0f;
    if (cov > 1.0f) cov = 1.0f;
    return cov;
}

// Corner index convention: 0=TL, 1=TR, 2=BL, 3=BR (matches CORNER_TL=1<<0,
// CORNER_TR=1<<1, CORNER_BL=1<<2, CORNER_BR=1<<3 in compositor.h - checking
// `mask & (1 << c)` selects the right corner for index c). `right`/`bottom`
// pick which edge of the panel each corner's box is measured from, so the
// SAME per-pixel address formula is used in both capture and restore and the
// two can never disagree about which physical pixel index (xx, yy) means -
// exactly the discipline xfce_dock_paint_rounded()'s #104 fix comment
// documents for the two-corner case.
void draw_round_corners_capture(corner_capture_t *cap, int32_t x, int32_t y,
                                int32_t w, int32_t h, int32_t r, int mask)
{
    if (r > CORNER_CAP_MAXR) r = CORNER_CAP_MAXR;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    cap->x = x; cap->y = y; cap->w = w; cap->h = h; cap->r = r; cap->mask = mask;
    if (r <= 0) return;

    for (int c = 0; c < 4; c++) {
        if (!(mask & (1 << c))) continue;
        int right  = (c == 1 || c == 3);   // TR, BR
        int bottom = (c == 2 || c == 3);   // BL, BR
        for (int32_t yy = 0; yy < r; yy++) {
            int32_t py = bottom ? (y + h - 1 - yy) : (y + yy);
            for (int32_t xx = 0; xx < r; xx++) {
                int32_t px = right ? (x + w - 1 - xx) : (x + xx);
                cap->px[c][yy][xx] = (py >= 0 && py < g_fb_height && px >= 0 && px < g_fb_width)
                                    ? g_fb[py * g_fb_pitch + px] : 0;
            }
        }
    }
}

void draw_round_corners_restore(const corner_capture_t *cap)
{
    int32_t r = cap->r;
    if (r <= 0) return;
    int ob = g_draw_blend;
    for (int c = 0; c < 4; c++) {
        if (!(cap->mask & (1 << c))) continue;
        int right  = (c == 1 || c == 3);
        int bottom = (c == 2 || c == 3);
        for (int32_t yy = 0; yy < r; yy++) {
            int32_t py = bottom ? (cap->y + cap->h - 1 - yy) : (cap->y + yy);
            if (py < 0 || py >= g_fb_height) continue;
            for (int32_t xx = 0; xx < r; xx++) {
                int32_t px = right ? (cap->x + cap->w - 1 - xx) : (cap->x + xx);
                if (px < 0 || px >= g_fb_width) continue;
                // xx, yy are already "distance from THIS corner" on both
                // axes by construction of the px/py formula above, so they
                // feed corner_coverage_aa() directly - no further mirroring
                // needed here (unlike the dock's two-corner special case,
                // there is only one formula for all four corners).
                float cov = corner_coverage_aa(xx, yy, r);
                int restore_a = (int)((1.0f - cov) * 255.0f + 0.5f);
                if (restore_a == 0) continue;
                g_draw_blend = restore_a;
                draw_putpixel(px, py, cap->px[c][yy][xx]);
            }
        }
    }
    g_draw_blend = ob;
}

// ============================================================================
// (#73) LOGIN/LOCK SHARED DRAWING PRIMITIVES
//
// These three used to live in login.c, next to the compositor's OWN login
// screen. That screen was deleted in #73: it was dead code that was still
// wired into the key path and still accumulated typed characters into a
// plaintext password buffer (see CHANGELOG). Its renderers were NOT dead -
// draw_bullet() is used by lockscreen.c and elevate.c, draw_button() by
// confirmdialog.c - so they move HERE, into the shared drawing module every
// other primitive already lives in, rather than keeping a mostly-empty
// login.c alive as their host.
//
// draw_password_field() did NOT come with them: it had zero callers (both
// credential surfaces use lock_draw_pill()) and it was the dead layer's own
// password renderer. Deleted with the rest of it.
// ============================================================================

// Draw a single bullet dot representing one masked password character.
// cx, cy is the center of the dot.
// #566: shared with lockscreen.c (prototyped in compositor.h) - not static.
void draw_bullet(int32_t cx, int32_t cy)
{
    draw_circle_filled(cx, cy, 4, CLR_LOGIN_TEXT);
}

// Draw a button rectangle with centered text.
// #566: shared with lockscreen.c (prototyped in compositor.h) - not static.
void draw_button(int32_t x, int32_t y, int32_t w, int32_t h,
                 const char *label, uint32_t bg)
{
    draw_rounded_rect(x, y, w, h, 4, bg);
    draw_rect_outline(x, y, w, h, CLR_LOGIN_BORDER);
    int lw = text_width(label);
    draw_text(x + (w - lw) / 2, y + (h - FONT_CHAR_H) / 2, label, CLR_LOGIN_TEXT);
}

// #745 identity dot palette: reused VERBATIM from Settings'
// avatar_palette (userland/apps/settings/main.c users_refresh()), same 8
// hexes, same uid%8 indexing (section 8.2 of the design doc) - kept as an
// independent literal copy rather than a shared header because the two
// apps are separate binaries on separate toolkits (draw.c vs the gui_*
// handle-based one) with no common color-token module between them; a
// mismatch here would only ever be cosmetic (which of 8 hues), never a
// correctness bug, and grep is the check that keeps them in sync.
static const uint32_t g_avatar_id_palette[8] = {
    0xFF569CD6, 0xFF66BB66, 0xFFCC8844, 0xFFAA66CC,
    0xFFCC6666, 0xFF44AAAA, 0xFF888888, 0xFFBBAA44
};

// Draw the user avatar: an antialiased "glass" disc (#745 port of
// docs/LOGIN_AVATARS_AND_PROFILE.html) with the user's initial inside.
//
// GLASS, HONESTLY: the real glass_render() backdrop blur cannot reach a 64px
// circle (no circular mask primitive, no spare cache surface, and its own
// bleed would wash a disc this small to near-flat anyway - design doc
// section 4) and in any case this panel is a flat opaque fill, not glass, so
// there is no backdrop behind the avatar to blur. This draws a translucent
// tint fill plus a soft highlight patch (upper-left) and shade patch
// (lower-right), the two patches sized/offset so they stay fully inside the
// fill disc - no circular clip needed. It reads as a lit, translucent
// material; it does not claim to show blurred content, because there is none
// to show.
//
// CONTRAST BY CONSTRUCTION: the fill is now the SAME token in every state
// (CLR_LOGIN_AVATAR_FILL) - the old code swapped to a blue fill on
// "selected", which put the fixed-color initial at 2.71:1, under the 4.5:1
// text floor (design doc section 3). Selection is communicated by the ring
// alone, which is why `state` only ever changes the ring here.
//
// #566: shared with lockscreen.c (prototyped in compositor.h) - not static.
// (lockscreen.c does not currently call it - #745 removed its own avatar
// disc as a considered "username only" decision - but the prototype stays
// shared in case that decision is revisited.)
void draw_avatar(int32_t cx, int32_t cy, const char *username, unsigned int uid, int state)
{
    float r = (float)(LOGIN_AVATAR_SIZE / 2);

    // Base glass fill, uniform across all states.
    draw_circle_filled_aa(cx, cy, r, CLR_LOGIN_AVATAR_FILL, 255);
    // Soft highlight patch, upper-left (specular glint).
    draw_circle_filled_aa(cx - (int32_t)(r * 0.30f), cy - (int32_t)(r * 0.32f),
                          r * 0.50f, CLR_LOGIN_AVATAR_HI, 90);
    // Soft shade patch, lower-right.
    draw_circle_filled_aa(cx + (int32_t)(r * 0.30f), cy + (int32_t)(r * 0.32f),
                          r * 0.45f, CLR_LOGIN_AVATAR_SHADE, 80);

    // Ring: the only state-dependent boundary.
    uint32_t ring_color;
    float stroke;
    if (state == AVATAR_ST_SELECTED) {
        ring_color = CLR_LOGIN_AVATAR_RING_SEL;
        stroke = 3.0f;
        // 1px soft outer glow (design doc 7.1).
        draw_circle_ring_aa(cx, cy, r + 4.5f, 2.0f, CLR_LOGIN_AVATAR_RING_SEL, 45);
    } else if (state == AVATAR_ST_HOVER) {
        ring_color = CLR_LOGIN_AVATAR_RING_HOVER;
        stroke = 2.0f;
    } else {
        ring_color = CLR_LOGIN_AVATAR_RING;
        stroke = 2.0f;
    }
    draw_circle_ring_aa(cx, cy, r + stroke * 0.5f, stroke, ring_color, 255);

    // Identity dot (decorative only - design doc section 7.3, exempt from the
    // 3:1 boundary floor: removing it changes nothing about which account is
    // which). 12px, 2px panel-color border, bottom-right.
    int32_t dot_r = 6;
    int32_t dot_cx = cx + (int32_t)(r * 0.74f);
    int32_t dot_cy = cy + (int32_t)(r * 0.74f);
    draw_circle_filled_aa(dot_cx, dot_cy, (float)(dot_r + 2), CLR_LOGIN_PANEL, 255);
    draw_circle_filled_aa(dot_cx, dot_cy, (float)dot_r, g_avatar_id_palette[uid % 8], 255);

    // Draw the first letter of the username, scaled up, centered. One ink,
    // every state (5.58:1 against the fill - design doc 7.2) - no longer
    // needs to change with the fill because the fill no longer changes.
    char letter[2] = { username[0], '\0' };
    if (letter[0] >= 'a' && letter[0] <= 'z') {
        letter[0] -= 32; // uppercase
    }
    int lw = text_width_large(letter, 2);
    // #uiscale: this positions a draw_char_large() BITMAP glyph (raw 16px
    // cell x explicit scale=2), not TTF text, so it uses the raw constant -
    // see the FONT_CHAR_H comment in compositor.h. The avatar initial itself
    // does not grow with the UI scale factor (known limitation, draw_char_large
    // has no scale-factor awareness); it stays a crisp fixed-size bitmap glyph.
    draw_text_large(cx - lw / 2, cy - FONT_CHAR_H_RAW, letter, CLR_LOGIN_TEXT, 2);
}

