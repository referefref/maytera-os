// ttf.h - TrueType font renderer for MayteraOS
// Uses stb_truetype to render TTF fonts loaded from FAT32 disk.
#ifndef TTF_H
#define TTF_H

#include "../types.h"

// Font styles (can be OR'd together)
#define TTF_STYLE_NORMAL  0
#define TTF_STYLE_BOLD    1
#define TTF_STYLE_ITALIC  2

// Pre-defined font sizes
#define TTF_SIZE_SMALL   12
#define TTF_SIZE_NORMAL  16
#define TTF_SIZE_LARGE   24
#define TTF_SIZE_XLARGE  32

// Rendered glyph info
typedef struct {
    uint8_t *bitmap;     // Grayscale bitmap (cached, do not free)
    int width;           // Bitmap width in pixels
    int height;          // Bitmap height in pixels
    int xoff;            // X offset from cursor to top-left of bitmap
    int yoff;            // Y offset from cursor to top-left of bitmap
    int advance;         // How much to advance cursor after this glyph
} ttf_glyph_t;

// Initialize TTF system. Call after FAT32 is mounted.
// Returns 0 on success, -1 on failure (falls back to bitmap fonts).
int ttf_init(void);

// Check if TTF system is initialized and ready
int ttf_is_ready(void);

// ---- Multi-face registry (OS-wide fonts) ----
// Number of installed faces (>=1; face 0 is the default UI font).
int ttf_face_count(void);
// Copy face `idx`'s typographic family name into buf; returns length.
// Returns 0 for a removed/empty slot: indices are stable and may contain holes,
// so callers must enumerate 0..ttf_face_count()-1 and skip zero-length entries.
int ttf_face_name(int idx, char *buf, int cap);
// Copy face `idx`'s subfamily ("Regular"/"Bold"/"Semibold Italic"); returns length.
int ttf_face_style(int idx, char *buf, int cap);
// Face registered from `path`, or -1.
int ttf_face_by_path(const char *path);
// Re-walk /FONTS; registers newly-installed fonts without a reboot. Additive
// only (see ttf.c). Returns the number of faces added.
int ttf_rescan(void);
// Hide a face from enumeration (uninstall). Face 0 cannot be removed.
int ttf_face_remove(int idx);
// Set the active face used by the legacy (non-_f) API; returns previous index.
int ttf_set_active_face(int idx);
// Current active face (the system UI font).
int ttf_get_active_face(void);

// Face-explicit variants (used by the font syscalls / Font Browser / Studio).
ttf_glyph_t *ttf_get_glyph_f(int face, int codepoint, int size, int style);
void ttf_get_metrics_f(int face, int size, int *ascent, int *descent, int *line_gap);
int  ttf_get_advance_f(int face, int codepoint, int size);
int  ttf_get_kerning_f(int face, int cp1, int cp2, int size);

// Get a rendered glyph for a codepoint at a given size and style (active face).
// Returns pointer to cached glyph info, or NULL on failure.
ttf_glyph_t *ttf_get_glyph(int codepoint, int size, int style);

// Get vertical font metrics for a given size
void ttf_get_metrics(int size, int *ascent, int *descent, int *line_gap);

// Get advance width for a character
int ttf_get_advance(int codepoint, int size);

// Get kerning adjustment between two characters
int ttf_get_kerning(int cp1, int cp2, int size);

// Draw a string using TTF rendering (convenience function)
// Renders anti-aliased text by alpha-blending glyph bitmaps onto the framebuffer.
void ttf_draw_string(int x, int y, const char *str, int size, uint32_t color);

// Measure the width of a string in pixels
int ttf_measure_string(const char *str, int size);

// Draw a single character with anti-aliasing. Returns advance width.
int ttf_draw_char(int x, int y, int codepoint, int size, int style, uint32_t color);

// #302 self-check: render digits 0-9 at several sizes and log PASS/FAIL to
// serial. Catches a silent regression in digit (esp. '7') rendering.
void ttf_selfcheck_digits(void);

#endif
