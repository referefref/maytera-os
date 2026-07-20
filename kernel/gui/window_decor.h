// window_decor.h - Window decoration rendering API for MayteraOS GUI
// Provides themed window decorations (title bars, borders, buttons)
#ifndef WINDOW_DECOR_H
#define WINDOW_DECOR_H

#include "../types.h"
#include "window.h"
#include "themes.h"

// ============================================================================
// Decoration Style Identifiers
// ============================================================================

#define DECOR_STYLE_RETRO_UNIX      0   // Motif/CDE style 3D bevels
#define DECOR_STYLE_MODERN_MACOS    1   // macOS traffic light buttons
#define DECOR_STYLE_MODERN_WINDOWS  2   // Windows 11 flat style
#define DECOR_STYLE_COUNT           3

// ============================================================================
// Window Button Types
// ============================================================================

#define BUTTON_TYPE_CLOSE       0
#define BUTTON_TYPE_MINIMIZE    1
#define BUTTON_TYPE_MAXIMIZE    2

// ============================================================================
// Button States
// ============================================================================

#define BUTTON_STATE_NORMAL     0
#define BUTTON_STATE_HOVER      1
#define BUTTON_STATE_PRESSED    2
#define BUTTON_STATE_INACTIVE   3

// ============================================================================
// Decoration Dimensions (may vary by style)
// ============================================================================

typedef struct {
    int32_t titlebar_height;        // Height of title bar in pixels
    int32_t border_width;           // Border thickness
    int32_t button_size;            // Size of window control buttons
    int32_t button_spacing;         // Spacing between buttons
    int32_t button_margin;          // Margin from edge to first button
    int32_t corner_radius;          // Corner radius (for modern styles)
    int32_t title_padding_left;     // Left padding for title text
    int32_t title_padding_top;      // Top padding for title text
} decor_metrics_t;

// ============================================================================
// Decoration Style Structure
// ============================================================================

// Forward declaration
struct decor_style;

// Function pointers for decoration rendering
typedef void (*decor_draw_titlebar_fn)(window_t *win, const theme_t *theme, bool active);
typedef void (*decor_draw_border_fn)(window_t *win, const theme_t *theme, bool active);
typedef void (*decor_draw_button_fn)(int type, int state, int32_t x, int32_t y,
                                      const theme_t *theme, bool active);
typedef void (*decor_get_metrics_fn)(decor_metrics_t *metrics);
typedef bool (*decor_hit_test_button_fn)(window_t *win, int32_t x, int32_t y, int *button_type);

// Decoration style definition
typedef struct decor_style {
    const char *name;                   // Style name (e.g., "Retro UNIX")
    decor_draw_titlebar_fn draw_titlebar;
    decor_draw_border_fn draw_border;
    decor_draw_button_fn draw_button;
    decor_get_metrics_fn get_metrics;
    decor_hit_test_button_fn hit_test_button;
} decor_style_t;

// ============================================================================
// Public API Functions
// ============================================================================

// Initialize the decoration system
void decor_init(void);

// Get/set the current decoration style
int decor_get_style(void);
void decor_set_style(int style_id);
const char *decor_get_style_name(int style_id);
int decor_get_style_count(void);

// Get metrics for current style
void decor_get_current_metrics(decor_metrics_t *metrics);

// Main decoration drawing functions
// These use the current decoration style
void decor_draw_titlebar(window_t *win, const theme_t *theme);
void decor_draw_border(window_t *win, const theme_t *theme);
void decor_draw_button(int type, int state, int32_t x, int32_t y, const theme_t *theme);

// Combined function to draw complete window frame
void decor_draw_window_frame(window_t *win, const theme_t *theme);

// Hit testing for buttons
// Returns true if (x,y) is on a button, sets button_type to the button type
bool decor_hit_test_button(window_t *win, int32_t x, int32_t y, int *button_type);

// Get button position for a specific button type
void decor_get_button_rect(window_t *win, int button_type, rect_t *rect);

// ============================================================================
// Helper Drawing Functions (available for use by decoration styles)
// ============================================================================

// Draw a 3D raised bevel effect (light top-left, dark bottom-right)
void decor_draw_bevel_raised(int32_t x, int32_t y, int32_t w, int32_t h,
                              uint32_t light, uint32_t dark, int32_t thickness);

// Draw a 3D sunken bevel effect (dark top-left, light bottom-right)
void decor_draw_bevel_sunken(int32_t x, int32_t y, int32_t w, int32_t h,
                              uint32_t light, uint32_t dark, int32_t thickness);

// Draw a rounded rectangle (for modern styles)
void decor_draw_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                              int32_t radius, uint32_t color);

// Draw a filled rounded rectangle
void decor_fill_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                              int32_t radius, uint32_t color);

// Draw a circular button (for macOS style traffic lights)
void decor_draw_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color);
void decor_fill_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color);

// Draw a horizontal gradient
void decor_draw_gradient_h(int32_t x, int32_t y, int32_t w, int32_t h,
                           uint32_t color1, uint32_t color2);

// Draw a vertical gradient
void decor_draw_gradient_v(int32_t x, int32_t y, int32_t w, int32_t h,
                           uint32_t color1, uint32_t color2);

// Draw a stippled (dithered) pattern for inactive windows
void decor_draw_stipple(int32_t x, int32_t y, int32_t w, int32_t h,
                        uint32_t color1, uint32_t color2);

// Color manipulation helpers
uint32_t decor_blend_colors(uint32_t c1, uint32_t c2, int alpha); // alpha 0-255
uint32_t decor_lighten_color(uint32_t color, int amount);
uint32_t decor_darken_color(uint32_t color, int amount);

// ============================================================================
// Style Registration (for decoration style modules)
// ============================================================================

// Register a decoration style
void decor_register_style(int style_id, const decor_style_t *style);

// Get a registered style by ID
const decor_style_t *decor_get_style_by_id(int style_id);

#endif // WINDOW_DECOR_H
