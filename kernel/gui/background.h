// background.h - Background system for MayteraOS
// Supports solid colors, gradients, patterns, and wallpaper images
#ifndef BACKGROUND_H
#define BACKGROUND_H

#include "../types.h"
#include "image.h"

// ============================================================================
// Background Mode Definitions
// ============================================================================

typedef enum {
    BG_SOLID,           // Solid color fill
    BG_GRADIENT_V,      // Vertical gradient (top to bottom)
    BG_GRADIENT_H,      // Horizontal gradient (left to right)
    BG_GRADIENT_RADIAL, // Radial gradient (center outward)
    BG_PATTERN,         // Procedural pattern
    BG_IMAGE_CENTER,    // Wallpaper image centered with solid border
    BG_IMAGE_TILE,      // Wallpaper image tiled
    BG_IMAGE_STRETCH,   // Wallpaper image stretched to fill
    BG_IMAGE_FIT,       // Wallpaper image fit (maintain aspect, letterbox)
    BG_IMAGE_FILL,      // Wallpaper image fill (maintain aspect, crop)
} bg_mode_t;

// Pattern types for BG_PATTERN mode
typedef enum {
    PATTERN_STIPPLE,        // CDE-style stipple/dither pattern
    PATTERN_CROSSHATCH,     // Crosshatch lines
    PATTERN_DIAGONAL,       // Diagonal lines
    PATTERN_GRID,           // Grid pattern
    PATTERN_DOTS,           // Dot matrix pattern
    PATTERN_CHECKERBOARD,   // Checkerboard pattern
    PATTERN_NOISE,          // Random noise pattern
    PATTERN_WEAVE,          // Woven texture pattern
} pattern_type_t;

// ============================================================================
// Background Configuration Structures
// ============================================================================

// Gradient configuration
typedef struct {
    uint32_t color1;        // Start color (ARGB)
    uint32_t color2;        // End color (ARGB)
    uint32_t color3;        // Optional middle color for tri-color gradients (0 = disabled)
    int angle;              // Gradient angle in degrees (for custom gradients)
} gradient_config_t;

// Pattern configuration
typedef struct {
    pattern_type_t type;    // Pattern type
    uint32_t fg_color;      // Foreground color (ARGB)
    uint32_t bg_color;      // Background color (ARGB)
    int density;            // Pattern density (1-10, meaning depends on pattern)
    int size;               // Element size in pixels (2-32)
    int spacing;            // Spacing between elements (0-32)
} pattern_config_t;

// Complete background configuration
typedef struct {
    bg_mode_t mode;         // Background mode
    uint32_t solid_color;   // For BG_SOLID mode
    gradient_config_t gradient;  // For gradient modes
    pattern_config_t pattern;    // For BG_PATTERN mode
    char wallpaper_path[64];     // For image modes
} background_config_t;

// ============================================================================
// Pre-defined Theme Backgrounds
// ============================================================================

// Retro UNIX theme background (deep teal/blue with stipple)
#define BG_RETRO_UNIX_COLOR1    0xFF1A3A4A  // Deep teal
#define BG_RETRO_UNIX_COLOR2    0xFF0A2030  // Darker blue
#define BG_RETRO_STIPPLE_FG     0xFF2A4A5A  // Lighter stipple foreground
#define BG_RETRO_STIPPLE_BG     0xFF1A3040  // Stipple background

// Modern Light theme background
#define BG_MODERN_LIGHT_COLOR1  0xFFE8F0F8  // Light sky blue
#define BG_MODERN_LIGHT_COLOR2  0xFFC8D8E8  // Slightly darker blue
#define BG_MODERN_LIGHT_ACCENT  0xFF5BADE9  // macOS Big Sur inspired blue

// Modern Dark theme background
#define BG_MODERN_DARK_COLOR1   0xFF1C1C28  // Deep dark purple-blue
#define BG_MODERN_DARK_COLOR2   0xFF0C0C18  // Even darker
#define BG_MODERN_DARK_ACCENT   0xFF3A4A6A  // Subtle accent

// Fluent Light theme background
#define BG_FLUENT_LIGHT_COLOR1  0xFFECF0F5  // Light blue-grey
#define BG_FLUENT_LIGHT_COLOR2  0xFFD4DCE4  // Slightly darker

// Fluent Dark theme background
#define BG_FLUENT_DARK_COLOR1   0xFF202428  // Dark grey-blue
#define BG_FLUENT_DARK_COLOR2   0xFF101418  // Darker

// ============================================================================
// Background API
// ============================================================================

// Initialize the background system
void background_init(void);

// Set background mode and configuration
void background_set_config(const background_config_t *config);

// Get current background configuration
const background_config_t *background_get_config(void);

// Convenience functions for common background types
void background_set_solid(uint32_t color);
void background_set_gradient_v(uint32_t top_color, uint32_t bottom_color);
void background_set_gradient_h(uint32_t left_color, uint32_t right_color);
void background_set_gradient_radial(uint32_t center_color, uint32_t edge_color);
void background_set_pattern(pattern_type_t type, uint32_t fg, uint32_t bg, int density);
void background_set_wallpaper(const char *path, bg_mode_t mode);

// Generate background to an image buffer
// Returns newly allocated image_t with background pixels
// Caller must free with image_free()
image_t *background_generate(uint32_t width, uint32_t height);

// Generate background directly to framebuffer (faster, no allocation)
void background_render(uint32_t width, uint32_t height);

// ============================================================================
// Pattern Generation Functions
// ============================================================================

// Generate a stipple pattern (CDE style)
// density: 1 (sparse) to 10 (dense)
void pattern_stipple(uint32_t *pixels, uint32_t width, uint32_t height,
                     uint32_t fg_color, uint32_t bg_color, int density);

// Generate a crosshatch pattern
// spacing: pixels between lines (4-32 typical)
void pattern_crosshatch(uint32_t *pixels, uint32_t width, uint32_t height,
                        uint32_t fg_color, uint32_t bg_color, int spacing);

// Generate diagonal lines pattern
// spacing: pixels between lines
// direction: 0 = forward slash /, 1 = backslash \, 2 = both
void pattern_diagonal(uint32_t *pixels, uint32_t width, uint32_t height,
                      uint32_t fg_color, uint32_t bg_color, int spacing, int direction);

// Generate a grid pattern
void pattern_grid(uint32_t *pixels, uint32_t width, uint32_t height,
                  uint32_t fg_color, uint32_t bg_color, int spacing);

// Generate a dot matrix pattern
void pattern_dots(uint32_t *pixels, uint32_t width, uint32_t height,
                  uint32_t fg_color, uint32_t bg_color, int size, int spacing);

// Generate a checkerboard pattern
void pattern_checkerboard(uint32_t *pixels, uint32_t width, uint32_t height,
                          uint32_t color1, uint32_t color2, int size);

// Generate noise pattern (pseudo-random)
void pattern_noise(uint32_t *pixels, uint32_t width, uint32_t height,
                   uint32_t base_color, int intensity);

// Generate woven texture pattern
void pattern_weave(uint32_t *pixels, uint32_t width, uint32_t height,
                   uint32_t fg_color, uint32_t bg_color, int size);

// ============================================================================
// Gradient Generation Functions
// ============================================================================

// Generate vertical gradient (top to bottom)
void gradient_vertical(uint32_t *pixels, uint32_t width, uint32_t height,
                       uint32_t top_color, uint32_t bottom_color);

// Generate horizontal gradient (left to right)
void gradient_horizontal(uint32_t *pixels, uint32_t width, uint32_t height,
                         uint32_t left_color, uint32_t right_color);

// Generate radial gradient (center to edge)
void gradient_radial(uint32_t *pixels, uint32_t width, uint32_t height,
                     uint32_t center_color, uint32_t edge_color);

// Generate diagonal gradient
void gradient_diagonal(uint32_t *pixels, uint32_t width, uint32_t height,
                       uint32_t start_color, uint32_t end_color);

// ============================================================================
// Theme-Specific Default Backgrounds
// ============================================================================

// Generate default background for a theme
// Returns newly allocated image_t
image_t *background_generate_for_theme(int theme_id, uint32_t width, uint32_t height);

// Get default background configuration for a theme
void background_get_theme_default(int theme_id, background_config_t *config);

// ============================================================================
// Wallpaper File I/O
// ============================================================================

// Generate a BMP file from a background configuration
// Returns 0 on success, -1 on failure
// Creates the BMP file at the specified path
int background_save_bmp(const char *path, const background_config_t *config,
                        uint32_t width, uint32_t height);

// Load a BMP wallpaper and apply it
int background_load_wallpaper(const char *path, bg_mode_t mode);

#endif // BACKGROUND_H
