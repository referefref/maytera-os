// font.h - Typography system for MayteraOS GUI
// Provides multi-font support, scaling, and theme-aware rendering
#ifndef GUI_FONT_H
#define GUI_FONT_H

#include "../types.h"

// ============================================================================
// Font Type Definitions
// ============================================================================

// Font identifiers for the font stack
typedef enum {
    FONT_SYSTEM = 0,    // Default UI font for labels, menus (8x16)
    FONT_TITLE,         // Bold/larger font for window titles, headings
    FONT_MONO,          // Monospace font for terminal, code
    FONT_SMALL,         // Small font for status bars, tooltips (4x8)
    FONT_ICON,          // Icon font for UI glyphs
    FONT_COUNT          // Number of font types
} font_id_t;

// Font style flags
#define FONT_STYLE_NORMAL      0x00
#define FONT_STYLE_BOLD        0x01
#define FONT_STYLE_ITALIC      0x02
#define FONT_STYLE_UNDERLINE   0x04

// Font rendering mode
typedef enum {
    FONT_RENDER_BITMAP,     // Crisp pixel-perfect (retro themes)
    FONT_RENDER_ANTIALIAS,  // Anti-aliased (modern themes)
} font_render_mode_t;

// ============================================================================
// Font Metrics Structure
// ============================================================================

typedef struct {
    uint8_t width;          // Character cell width in pixels
    uint8_t height;         // Character cell height in pixels
    uint8_t baseline;       // Baseline offset from top
    uint8_t line_height;    // Line spacing (typically height + leading)
    int8_t  ascent;         // Pixels above baseline
    int8_t  descent;        // Pixels below baseline
    uint8_t char_spacing;   // Extra spacing between characters
} font_metrics_t;

// ============================================================================
// Font Structure
// ============================================================================

typedef struct {
    const char *name;               // Font name (e.g., "System", "Mono")
    font_metrics_t metrics;         // Font metrics
    font_render_mode_t render_mode; // Rendering mode
    const uint8_t *glyph_data;      // Pointer to glyph bitmap data
    uint8_t first_char;             // First character code in font
    uint8_t last_char;              // Last character code in font
    uint16_t glyph_size;            // Size of each glyph in bytes
    bool is_loaded;                 // True if font is loaded from file
} font_t;

// ============================================================================
// Text Alignment
// ============================================================================

typedef enum {
    TEXT_ALIGN_LEFT = 0,
    TEXT_ALIGN_CENTER,
    TEXT_ALIGN_RIGHT,
} text_align_t;

typedef enum {
    TEXT_VALIGN_TOP = 0,
    TEXT_VALIGN_MIDDLE,
    TEXT_VALIGN_BOTTOM,
} text_valign_t;

// ============================================================================
// Text Drawing Options
// ============================================================================

typedef struct {
    font_id_t font;             // Which font to use
    uint32_t color;             // Text color
    uint32_t bg_color;          // Background color (0 for transparent)
    uint8_t style;              // FONT_STYLE_* flags
    text_align_t align;         // Horizontal alignment
    text_valign_t valign;       // Vertical alignment
    int32_t max_width;          // Maximum width (0 = no limit, enables wrapping)
    bool ellipsis;              // Add "..." if text is truncated
    int32_t shadow_offset_x;    // Shadow X offset (0 = no shadow)
    int32_t shadow_offset_y;    // Shadow Y offset (0 = no shadow)
    uint32_t shadow_color;      // Shadow color
} text_options_t;

// Default text options (system font, white text, no background)
#define TEXT_OPTIONS_DEFAULT { \
    .font = FONT_SYSTEM, \
    .color = 0x00000000, \
    .bg_color = 0, \
    .style = FONT_STYLE_NORMAL, \
    .align = TEXT_ALIGN_LEFT, \
    .valign = TEXT_VALIGN_TOP, \
    .max_width = 0, \
    .ellipsis = false, \
    .shadow_offset_x = 0, \
    .shadow_offset_y = 0, \
    .shadow_color = 0 \
}

// ============================================================================
// Font System API
// ============================================================================

// Initialize the typography system
void font_system_init(void);

// Get a font by ID
const font_t *font_get(font_id_t font_id);

// Get font metrics
const font_metrics_t *font_get_metrics(font_id_t font_id);

// Get the glyph bitmap for a character
// Returns pointer to glyph data, or NULL if character not in font
const uint8_t *font_get_char_glyph(font_id_t font_id, char c);

// ============================================================================
// Text Measurement
// ============================================================================

// Measure the width of a string in pixels
int32_t font_measure_string(font_id_t font_id, const char *str);

// Measure the width of a string up to max_chars
int32_t font_measure_string_n(font_id_t font_id, const char *str, int max_chars);

// Get the height needed for wrapped text at a given width
int32_t font_measure_wrapped_height(font_id_t font_id, const char *str, int32_t max_width);

// Calculate how many characters fit in a given width
int font_chars_that_fit(font_id_t font_id, const char *str, int32_t max_width);

// ============================================================================
// Text Drawing Functions
// ============================================================================

// Draw a single character at (x, y)
void font_draw_char(int32_t x, int32_t y, char c, font_id_t font_id, uint32_t color);

// Draw a single character with background
void font_draw_char_bg(int32_t x, int32_t y, char c, font_id_t font_id,
                       uint32_t fg_color, uint32_t bg_color);

// Draw a string at (x, y) with default options
void font_draw_string(int32_t x, int32_t y, const char *str,
                      font_id_t font_id, uint32_t color);

// Draw a string at (x, y) with background
void font_draw_string_bg(int32_t x, int32_t y, const char *str,
                         font_id_t font_id, uint32_t fg_color, uint32_t bg_color);

// Draw text with full options
void font_draw_text(int32_t x, int32_t y, const char *str, const text_options_t *options);

// Draw text within a bounding box with alignment
void font_draw_text_box(int32_t x, int32_t y, int32_t width, int32_t height,
                        const char *str, const text_options_t *options);

// Draw text with word wrapping
void font_draw_text_wrapped(int32_t x, int32_t y, int32_t max_width,
                            const char *str, font_id_t font_id, uint32_t color);

// ============================================================================
// Styled Text (simple markup)
// ============================================================================

// Draw text with inline style codes:
// ^B = toggle bold, ^I = toggle italic, ^U = toggle underline
// ^Cxxxxxx = set color (hex RGB), ^R = reset to default
void font_draw_styled_text(int32_t x, int32_t y, const char *str,
                           font_id_t base_font, uint32_t default_color);

// ============================================================================
// Font Loading (for file-based fonts)
// ============================================================================

// Font file format identifiers
#define FONT_FILE_MAGIC     0x464E5446  // "FNTF" - Font File
#define FONT_FILE_VERSION   1

// Font file header structure (stored in .fnt files)
typedef struct {
    uint32_t magic;         // FONT_FILE_MAGIC
    uint16_t version;       // File format version
    uint16_t flags;         // Font flags
    uint8_t  width;         // Glyph width
    uint8_t  height;        // Glyph height
    uint8_t  first_char;    // First character code
    uint8_t  last_char;     // Last character code
    char     name[24];      // Font name (null-terminated)
    // Followed by glyph data: (last_char - first_char + 1) * (width * height / 8) bytes
} font_file_header_t;

// Load a font from a file path (e.g., "/FONTS/mono.fnt")
// Returns true on success
bool font_load_from_file(font_id_t slot, const char *path);

// Unload a loaded font, restoring the default
void font_unload(font_id_t slot);

// ============================================================================
// Theme Integration
// ============================================================================

// Set font rendering mode based on current theme
// Called automatically when theme changes
void font_set_render_mode(font_render_mode_t mode);

// Get current rendering mode
font_render_mode_t font_get_render_mode(void);

// ============================================================================
// Convenience Macros
// ============================================================================

// Standard font dimensions for layout calculations
#define FONT_SYSTEM_WIDTH   8
#define FONT_SYSTEM_HEIGHT  16
#define FONT_SMALL_WIDTH    4
#define FONT_SMALL_HEIGHT   8
#define FONT_TITLE_WIDTH    8
#define FONT_TITLE_HEIGHT   16
#define FONT_MONO_WIDTH     8
#define FONT_MONO_HEIGHT    16

// Legacy compatibility - map to video/font.h dimensions
#define FONT_WIDTH  FONT_SYSTEM_WIDTH
#define FONT_HEIGHT FONT_SYSTEM_HEIGHT

// Quick measurement macros
#define TEXT_WIDTH(str, font)  font_measure_string((font), (str))
#define TEXT_HEIGHT(font)      (font_get_metrics(font)->line_height)

#endif // GUI_FONT_H
