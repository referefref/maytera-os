// colors.h - Semantic Color System for MayteraOS GUI
// Provides theme-independent color definitions and utilities
#ifndef COLORS_H
#define COLORS_H

#include "../types.h"
#include "themes.h"

// ============================================================================
// Semantic Color Roles
// ============================================================================

// These are the semantic color roles that every theme must define.
// Use these throughout the UI instead of hard-coded colors.
typedef enum {
    // Background colors
    COLOR_ROLE_BG_PRIMARY,          // Main content background
    COLOR_ROLE_BG_SECONDARY,        // Secondary/alternate background
    COLOR_ROLE_BG_TERTIARY,         // Tertiary/subtle background
    COLOR_ROLE_BG_ELEVATED,         // Elevated surfaces (cards, dialogs)

    // Foreground (text) colors
    COLOR_ROLE_FG_PRIMARY,          // Primary text
    COLOR_ROLE_FG_SECONDARY,        // Secondary/muted text
    COLOR_ROLE_FG_TERTIARY,         // Tertiary/placeholder text
    COLOR_ROLE_FG_DISABLED,         // Disabled text
    COLOR_ROLE_FG_INVERSE,          // Text on accent background

    // Accent colors
    COLOR_ROLE_ACCENT,              // Primary accent (buttons, links)
    COLOR_ROLE_ACCENT_HOVER,        // Accent on hover
    COLOR_ROLE_ACCENT_PRESSED,      // Accent when pressed
    COLOR_ROLE_ACCENT_SUBTLE,       // Subtle accent (hover backgrounds)

    // Selection colors
    COLOR_ROLE_SELECTION,           // Selection background
    COLOR_ROLE_SELECTION_TEXT,      // Selection text

    // Border colors
    COLOR_ROLE_BORDER,              // Default border
    COLOR_ROLE_BORDER_SUBTLE,       // Subtle/divider border
    COLOR_ROLE_BORDER_FOCUS,        // Focused element border
    COLOR_ROLE_BORDER_ERROR,        // Error state border

    // Status colors
    COLOR_ROLE_ERROR,               // Error/danger
    COLOR_ROLE_ERROR_BG,            // Error background
    COLOR_ROLE_WARNING,             // Warning
    COLOR_ROLE_WARNING_BG,          // Warning background
    COLOR_ROLE_SUCCESS,             // Success/positive
    COLOR_ROLE_SUCCESS_BG,          // Success background
    COLOR_ROLE_INFO,                // Information
    COLOR_ROLE_INFO_BG,             // Information background

    // Shadow colors
    COLOR_ROLE_SHADOW,              // Drop shadow
    COLOR_ROLE_SHADOW_STRONG,       // Strong shadow (modals)

    // Special purpose
    COLOR_ROLE_SCROLLBAR,           // Scrollbar track
    COLOR_ROLE_SCROLLBAR_THUMB,     // Scrollbar thumb
    COLOR_ROLE_TOOLTIP_BG,          // Tooltip background
    COLOR_ROLE_TOOLTIP_TEXT,        // Tooltip text

    COLOR_ROLE_COUNT                // Number of color roles
} color_role_t;

// ============================================================================
// Color Manipulation Utilities
// ============================================================================

// Color component extraction (0x00RRGGBB format)
#define COLOR_R(c)  (((c) >> 16) & 0xFF)
#define COLOR_G(c)  (((c) >> 8) & 0xFF)
#define COLOR_B(c)  ((c) & 0xFF)

// Color construction from RGB components
#define COLOR_RGB(r, g, b)  ((((uint32_t)(r) & 0xFF) << 16) | \
                             (((uint32_t)(g) & 0xFF) << 8) | \
                             ((uint32_t)(b) & 0xFF))

// Color construction from RGBA (alpha is used for blending, not stored)
#define COLOR_RGBA(r, g, b, a)  COLOR_RGB(r, g, b)

// ============================================================================
// Named Color Constants
// ============================================================================

// Grayscale
#define COLOR_BLACK         0x00000000
#define COLOR_GRAY_900      0x00111111
#define COLOR_GRAY_800      0x00222222
#define COLOR_GRAY_700      0x00333333
#define COLOR_GRAY_600      0x00555555
#define COLOR_GRAY_500      0x00777777
#define COLOR_GRAY_400      0x00999999
#define COLOR_GRAY_300      0x00BBBBBB
#define COLOR_GRAY_200      0x00DDDDDD
#define COLOR_GRAY_100      0x00EEEEEE
#define COLOR_WHITE         0x00FFFFFF

// Primary colors
#define COLOR_RED           0x00FF0000
#define COLOR_GREEN         0x0000FF00
#define COLOR_BLUE          0x000000FF
#define COLOR_CYAN          0x0000FFFF
#define COLOR_MAGENTA       0x00FF00FF
#define COLOR_YELLOW        0x00FFFF00

// Common UI colors
#define COLOR_ORANGE        0x00FF8800
#define COLOR_PURPLE        0x008800FF
#define COLOR_PINK          0x00FF88AA
#define COLOR_TEAL          0x00008888
#define COLOR_BROWN         0x00884422

// Windows-style system colors
#define COLOR_NAVY          0x00000080
#define COLOR_SILVER        0x00C0C0C0
#define COLOR_LIME          0x0000FF00
#define COLOR_AQUA          0x0000FFFF
#define COLOR_MAROON        0x00800000
#define COLOR_OLIVE         0x00808000
#define COLOR_FUCHSIA       0x00FF00FF

// ============================================================================
// Contrast Ratio Calculation
// ============================================================================

// Calculate relative luminance of a color (0.0 - 1.0)
// Uses sRGB to linear conversion per WCAG 2.1
static inline float color_luminance(uint32_t color) {
    float r = COLOR_R(color) / 255.0f;
    float g = COLOR_G(color) / 255.0f;
    float b = COLOR_B(color) / 255.0f;

    // Convert to linear RGB
    r = (r <= 0.03928f) ? r / 12.92f : ((r + 0.055f) / 1.055f);
    g = (g <= 0.03928f) ? g / 12.92f : ((g + 0.055f) / 1.055f);
    b = (b <= 0.03928f) ? b / 12.92f : ((b + 0.055f) / 1.055f);

    // Approximate power function (since we don't have pow())
    // This is a simplified version
    r = r * r * r;  // Approximation of r^2.4
    g = g * g * g;
    b = b * b * b;

    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// Calculate contrast ratio between two colors (1.0 - 21.0)
// WCAG AA: >= 4.5 for normal text, >= 3.0 for large text
// WCAG AAA: >= 7.0 for normal text, >= 4.5 for large text
static inline float color_contrast_ratio(uint32_t fg, uint32_t bg) {
    float l1 = color_luminance(fg);
    float l2 = color_luminance(bg);

    // Ensure l1 is the lighter color
    if (l2 > l1) {
        float tmp = l1;
        l1 = l2;
        l2 = tmp;
    }

    return (l1 + 0.05f) / (l2 + 0.05f);
}

// Check if contrast meets WCAG AA for normal text (4.5:1)
static inline bool color_meets_aa(uint32_t fg, uint32_t bg) {
    return color_contrast_ratio(fg, bg) >= 4.5f;
}

// Check if contrast meets WCAG AA for large text (3:1)
static inline bool color_meets_aa_large(uint32_t fg, uint32_t bg) {
    return color_contrast_ratio(fg, bg) >= 3.0f;
}

// Check if contrast meets WCAG AAA for normal text (7:1)
static inline bool color_meets_aaa(uint32_t fg, uint32_t bg) {
    return color_contrast_ratio(fg, bg) >= 7.0f;
}

// ============================================================================
// Color Modification Functions
// ============================================================================

// Lighten a color by a factor (0.0 = unchanged, 1.0 = white)
static inline uint32_t color_lighten(uint32_t color, float factor) {
    if (factor <= 0.0f) return color;
    if (factor >= 1.0f) return COLOR_WHITE;

    uint8_t r = COLOR_R(color);
    uint8_t g = COLOR_G(color);
    uint8_t b = COLOR_B(color);

    r = r + (uint8_t)((255 - r) * factor);
    g = g + (uint8_t)((255 - g) * factor);
    b = b + (uint8_t)((255 - b) * factor);

    return COLOR_RGB(r, g, b);
}

// Darken a color by a factor (0.0 = unchanged, 1.0 = black)
static inline uint32_t color_darken(uint32_t color, float factor) {
    if (factor <= 0.0f) return color;
    if (factor >= 1.0f) return COLOR_BLACK;

    uint8_t r = COLOR_R(color);
    uint8_t g = COLOR_G(color);
    uint8_t b = COLOR_B(color);

    r = (uint8_t)(r * (1.0f - factor));
    g = (uint8_t)(g * (1.0f - factor));
    b = (uint8_t)(b * (1.0f - factor));

    return COLOR_RGB(r, g, b);
}

// Blend two colors (factor 0.0 = color1, factor 1.0 = color2)
static inline uint32_t color_blend(uint32_t color1, uint32_t color2, float factor) {
    if (factor <= 0.0f) return color1;
    if (factor >= 1.0f) return color2;

    uint8_t r1 = COLOR_R(color1), r2 = COLOR_R(color2);
    uint8_t g1 = COLOR_G(color1), g2 = COLOR_G(color2);
    uint8_t b1 = COLOR_B(color1), b2 = COLOR_B(color2);

    uint8_t r = (uint8_t)(r1 + (r2 - r1) * factor);
    uint8_t g = (uint8_t)(g1 + (g2 - g1) * factor);
    uint8_t b = (uint8_t)(b1 + (b2 - b1) * factor);

    return COLOR_RGB(r, g, b);
}

// Desaturate a color (0.0 = full color, 1.0 = grayscale)
static inline uint32_t color_desaturate(uint32_t color, float factor) {
    if (factor <= 0.0f) return color;

    uint8_t r = COLOR_R(color);
    uint8_t g = COLOR_G(color);
    uint8_t b = COLOR_B(color);

    // Calculate grayscale value (simple average)
    uint8_t gray = (r + g + b) / 3;

    if (factor >= 1.0f) {
        return COLOR_RGB(gray, gray, gray);
    }

    r = (uint8_t)(r + (gray - r) * factor);
    g = (uint8_t)(g + (gray - g) * factor);
    b = (uint8_t)(b + (gray - b) * factor);

    return COLOR_RGB(r, g, b);
}

// Invert a color
static inline uint32_t color_invert(uint32_t color) {
    return COLOR_RGB(255 - COLOR_R(color),
                     255 - COLOR_G(color),
                     255 - COLOR_B(color));
}

// Adjust color opacity (simple alpha blend with background)
static inline uint32_t color_alpha_blend(uint32_t fg, uint32_t bg, uint8_t alpha) {
    if (alpha == 255) return fg;
    if (alpha == 0) return bg;

    float a = alpha / 255.0f;
    return color_blend(bg, fg, a);
}

// ============================================================================
// Semantic Color Access
// ============================================================================

// Get a semantic color from the current theme
// Maps color roles to theme colors
static inline uint32_t color_get(color_role_t role) {
    const theme_t *theme = theme_get_current();

    switch (role) {
        // Background colors
        case COLOR_ROLE_BG_PRIMARY:
            return theme->window_bg;
        case COLOR_ROLE_BG_SECONDARY:
            return theme->desktop_bg;
        case COLOR_ROLE_BG_TERTIARY:
            return theme->menu_bg;
        case COLOR_ROLE_BG_ELEVATED:
            return theme->window_bg;

        // Foreground colors
        case COLOR_ROLE_FG_PRIMARY:
            return theme->label_text;
        case COLOR_ROLE_FG_SECONDARY:
            return theme->menu_text_disabled;
        case COLOR_ROLE_FG_TERTIARY:
            return color_lighten(theme->label_text, 0.5f);
        case COLOR_ROLE_FG_DISABLED:
            return theme->button_disabled;
        case COLOR_ROLE_FG_INVERSE:
            return theme->selection_text;

        // Accent colors
        case COLOR_ROLE_ACCENT:
            return theme->selection_bg;
        case COLOR_ROLE_ACCENT_HOVER:
            return color_lighten(theme->selection_bg, 0.1f);
        case COLOR_ROLE_ACCENT_PRESSED:
            return color_darken(theme->selection_bg, 0.1f);
        case COLOR_ROLE_ACCENT_SUBTLE:
            return color_blend(theme->window_bg, theme->selection_bg, 0.1f);

        // Selection colors
        case COLOR_ROLE_SELECTION:
            return theme->selection_bg;
        case COLOR_ROLE_SELECTION_TEXT:
            return theme->selection_text;

        // Border colors
        case COLOR_ROLE_BORDER:
            return theme->window_border;
        case COLOR_ROLE_BORDER_SUBTLE:
            return theme->menu_separator;
        case COLOR_ROLE_BORDER_FOCUS:
            return theme->selection_bg;
        case COLOR_ROLE_BORDER_ERROR:
            return COLOR_RED;

        // Status colors
        case COLOR_ROLE_ERROR:
            return 0x00E04040;  // Red
        case COLOR_ROLE_ERROR_BG:
            return 0x00FFE0E0;  // Light red
        case COLOR_ROLE_WARNING:
            return 0x00E0A000;  // Orange
        case COLOR_ROLE_WARNING_BG:
            return 0x00FFF0D0;  // Light orange
        case COLOR_ROLE_SUCCESS:
            return 0x0040A040;  // Green
        case COLOR_ROLE_SUCCESS_BG:
            return 0x00E0FFE0;  // Light green
        case COLOR_ROLE_INFO:
            return 0x004080C0;  // Blue
        case COLOR_ROLE_INFO_BG:
            return 0x00E0F0FF;  // Light blue

        // Shadow colors
        case COLOR_ROLE_SHADOW:
            return 0x00404040;
        case COLOR_ROLE_SHADOW_STRONG:
            return 0x00202020;

        // Special purpose
        case COLOR_ROLE_SCROLLBAR:
            return theme->scrollbar_bg;
        case COLOR_ROLE_SCROLLBAR_THUMB:
            return theme->scrollbar_thumb;
        case COLOR_ROLE_TOOLTIP_BG:
            return 0x00FFFFD0;  // Light yellow
        case COLOR_ROLE_TOOLTIP_TEXT:
            return COLOR_BLACK;

        default:
            return theme->label_text;
    }
}

// ============================================================================
// Color Palette Generation
// ============================================================================

// Generate a color palette from a base color
// palette[0] = darkest (900), palette[9] = lightest (50)
static inline void color_generate_palette(uint32_t base, uint32_t *palette) {
    // Darken for 900-600
    palette[0] = color_darken(base, 0.7f);   // 900
    palette[1] = color_darken(base, 0.5f);   // 800
    palette[2] = color_darken(base, 0.3f);   // 700
    palette[3] = color_darken(base, 0.15f);  // 600

    // Base and slight variations
    palette[4] = base;                        // 500 (base)
    palette[5] = color_lighten(base, 0.15f); // 400

    // Lighten for 300-50
    palette[6] = color_lighten(base, 0.3f);  // 300
    palette[7] = color_lighten(base, 0.5f);  // 200
    palette[8] = color_lighten(base, 0.7f);  // 100
    palette[9] = color_lighten(base, 0.85f); // 50
}

// ============================================================================
// Predefined Color Palettes
// ============================================================================

// Material Design-inspired color indices
#define PALETTE_50    9
#define PALETTE_100   8
#define PALETTE_200   7
#define PALETTE_300   6
#define PALETTE_400   5
#define PALETTE_500   4  // Base
#define PALETTE_600   3
#define PALETTE_700   2
#define PALETTE_800   1
#define PALETTE_900   0

// ============================================================================
// Helper Macros for Theme Colors
// ============================================================================

// Quick access to common semantic colors
#define COLOR_FG          color_get(COLOR_ROLE_FG_PRIMARY)
#define COLOR_FG_MUTED    color_get(COLOR_ROLE_FG_SECONDARY)
#define COLOR_BG          color_get(COLOR_ROLE_BG_PRIMARY)
#define COLOR_BORDER      color_get(COLOR_ROLE_BORDER)
#define COLOR_ACCENT      color_get(COLOR_ROLE_ACCENT)

// Status colors
#define COLOR_ERROR       color_get(COLOR_ROLE_ERROR)
#define COLOR_WARNING     color_get(COLOR_ROLE_WARNING)
#define COLOR_SUCCESS     color_get(COLOR_ROLE_SUCCESS)
#define COLOR_INFO        color_get(COLOR_ROLE_INFO)

// ============================================================================
// Windows 95/Retro Color Constants
// ============================================================================

// Classic Windows color palette (for retro themes)
#define WIN95_DESKTOP       0x00008080  // Teal desktop
#define WIN95_TITLEBAR      0x00000080  // Navy titlebar
#define WIN95_BUTTON_FACE   0x00C0C0C0  // Silver buttons
#define WIN95_BUTTON_LIGHT  0x00FFFFFF  // Button highlight
#define WIN95_BUTTON_SHADOW 0x00808080  // Button shadow
#define WIN95_WINDOW_BG     0x00FFFFFF  // White window
#define WIN95_MENU_BG       0x00C0C0C0  // Silver menu
#define WIN95_TEXT          0x00000000  // Black text
#define WIN95_HIGHLIGHT     0x00000080  // Navy highlight
#define WIN95_HIGHLIGHT_TEXT 0x00FFFFFF // White highlight text

// CGA/EGA color palette (for ultra-retro themes)
#define CGA_BLACK           0x00000000
#define CGA_BLUE            0x000000AA
#define CGA_GREEN           0x0000AA00
#define CGA_CYAN            0x0000AAAA
#define CGA_RED             0x00AA0000
#define CGA_MAGENTA         0x00AA00AA
#define CGA_BROWN           0x00AA5500
#define CGA_LIGHT_GRAY      0x00AAAAAA
#define CGA_DARK_GRAY       0x00555555
#define CGA_LIGHT_BLUE      0x005555FF
#define CGA_LIGHT_GREEN     0x0055FF55
#define CGA_LIGHT_CYAN      0x0055FFFF
#define CGA_LIGHT_RED       0x00FF5555
#define CGA_LIGHT_MAGENTA   0x00FF55FF
#define CGA_YELLOW          0x00FFFF55
#define CGA_WHITE           0x00FFFFFF

#endif // COLORS_H
