// themes.h - Color theme system for MayteraOS GUI
// Provides comprehensive theming for all UI elements including colors and typography
#ifndef THEMES_H
#define THEMES_H

#include "../types.h"

// Maximum number of built-in themes
#define MAX_THEMES  12

// ============================================================================
// Theme Style Categories
// ============================================================================

// Font rendering style (bitmap for retro, antialiased for modern)
typedef enum {
    THEME_STYLE_RETRO,      // Crisp bitmap fonts, 3D beveled UI
    THEME_STYLE_MODERN,     // Anti-aliased fonts, flat/minimal UI
    THEME_STYLE_MIXED,      // Mix of both styles
} theme_style_t;

// Theme identifiers
// THEME_DEFAULT (0) is the Retro UNIX/CDE theme - the signature MayteraOS look
#define THEME_DEFAULT       0   // Retro UNIX (CDE/Motif-inspired) - MayteraOS signature theme
#define THEME_DARK          1
#define THEME_LIGHT         2
#define THEME_HIGH_CONTRAST 3
#define THEME_CLASSIC       4   // Windows 95/98 style
#define THEME_OCEAN         5
#define THEME_SUNSET        6
#define THEME_FOREST        7
#define THEME_MODERN_LIGHT  8   // macOS Big Sur light mode
#define THEME_MODERN_DARK   9   // macOS Big Sur dark mode
#define THEME_FLUENT_LIGHT  10  // Windows 11 Fluent Design - Light Mode
#define THEME_FLUENT_DARK   11  // Windows 11 Fluent Design - Dark Mode

// Theme name max length
#define THEME_NAME_LEN      32

// Theme color structure - contains all UI colors
// All colors are in 0x00RRGGBB format (no alpha for simplicity)
typedef struct {
    char name[THEME_NAME_LEN];      // Theme display name
    theme_style_t style;            // Visual style category

    // Window titlebar colors
    uint32_t titlebar_active;       // Active window titlebar
    uint32_t titlebar_inactive;     // Inactive window titlebar
    uint32_t titlebar_text;         // Titlebar text color

    // Window body colors
    uint32_t window_bg;             // Window background (content area)
    uint32_t window_border;         // Window border color

    // Window control buttons
    uint32_t close_button;          // Close button color
    uint32_t close_button_hover;    // Close button hover color
    uint32_t minimize_button;       // Minimize button color
    uint32_t maximize_button;       // Maximize button color

    // Button widget colors
    uint32_t button_bg;             // Normal button background
    uint32_t button_bg_hover;       // Button hover background
    uint32_t button_bg_pressed;     // Button pressed background
    uint32_t button_border;         // Button border
    uint32_t button_text;           // Button text color
    uint32_t button_disabled;       // Disabled button color

    // Text widget colors
    uint32_t label_text;            // Label text color
    uint32_t textbox_bg;            // Textbox background
    uint32_t textbox_border;        // Textbox border
    uint32_t textbox_text;          // Textbox text
    uint32_t textbox_cursor;        // Textbox cursor

    // Checkbox colors
    uint32_t checkbox_bg;           // Checkbox background
    uint32_t checkbox_border;       // Checkbox border
    uint32_t checkbox_check;        // Checkmark color

    // Desktop colors
    uint32_t desktop_bg;            // Default desktop background

    // Taskbar/Dock colors
    uint32_t taskbar_bg;            // Taskbar background
    uint32_t taskbar_hover;         // Taskbar item hover
    uint32_t taskbar_active;        // Active taskbar item
    uint32_t start_button;          // Start button color
    uint32_t gauge_bg;              // Resource gauge background
    uint32_t gauge_fg;              // Resource gauge fill

    // Menu colors
    uint32_t menu_bg;               // Menu background
    uint32_t menu_border;           // Menu border
    uint32_t menu_item_hover;       // Menu item hover highlight
    uint32_t menu_text;             // Menu text color
    uint32_t menu_text_disabled;    // Disabled menu item text
    uint32_t menu_separator;        // Menu separator line

    // Scrollbar colors
    uint32_t scrollbar_bg;          // Scrollbar track
    uint32_t scrollbar_thumb;       // Scrollbar thumb
    uint32_t scrollbar_thumb_hover; // Scrollbar thumb hover

    // Selection colors
    uint32_t selection_bg;          // Selected item background
    uint32_t selection_text;        // Selected item text

    // Status/alert colors
    uint32_t color_error;           // Error/danger color
    uint32_t color_warning;         // Warning color
    uint32_t color_success;         // Success/positive color
    uint32_t color_info;            // Information color

    // Link colors
    uint32_t link_color;            // Hyperlink color
    uint32_t link_visited;          // Visited link color
    uint32_t link_hover;            // Link hover color

    // Tooltip colors
    uint32_t tooltip_bg;            // Tooltip background
    uint32_t tooltip_text;          // Tooltip text
    uint32_t tooltip_border;        // Tooltip border

} theme_t;

// Get the currently active theme
const theme_t *theme_get_current(void);

// Get a specific theme by ID
const theme_t *theme_get_by_id(int theme_id);

// Get the current theme ID
int theme_get_current_id(void);

// (#285) Resolve userland theme_color_id_t -> active theme color
uint32_t theme_get_color_by_id(int theme_id, int color_id);

// Set the active theme by ID
void theme_set(int theme_id);

// Get the number of available themes
int theme_get_count(void);

// Get theme name by ID
const char *theme_get_name(int theme_id);

// Initialize the theme system
void theme_init(void);

// ============================================================================
// Convenience macros to access current theme colors
// ============================================================================

#define THEME_TITLEBAR_ACTIVE       (theme_get_current()->titlebar_active)
#define THEME_TITLEBAR_INACTIVE     (theme_get_current()->titlebar_inactive)
#define THEME_TITLEBAR_TEXT         (theme_get_current()->titlebar_text)
#define THEME_WINDOW_BG             (theme_get_current()->window_bg)
#define THEME_WINDOW_BORDER         (theme_get_current()->window_border)
#define THEME_CLOSE_BUTTON          (theme_get_current()->close_button)
#define THEME_CLOSE_BUTTON_HOVER    (theme_get_current()->close_button_hover)
#define THEME_MINIMIZE_BUTTON       (theme_get_current()->minimize_button)
#define THEME_MAXIMIZE_BUTTON       (theme_get_current()->maximize_button)
#define THEME_BUTTON_BG             (theme_get_current()->button_bg)
#define THEME_BUTTON_BG_HOVER       (theme_get_current()->button_bg_hover)
#define THEME_BUTTON_BG_PRESSED     (theme_get_current()->button_bg_pressed)
#define THEME_BUTTON_BORDER         (theme_get_current()->button_border)
#define THEME_BUTTON_TEXT           (theme_get_current()->button_text)
#define THEME_BUTTON_DISABLED       (theme_get_current()->button_disabled)
#define THEME_LABEL_TEXT            (theme_get_current()->label_text)
#define THEME_TEXTBOX_BG            (theme_get_current()->textbox_bg)
#define THEME_TEXTBOX_BORDER        (theme_get_current()->textbox_border)
#define THEME_TEXTBOX_TEXT          (theme_get_current()->textbox_text)
#define THEME_TEXTBOX_CURSOR        (theme_get_current()->textbox_cursor)
#define THEME_CHECKBOX_BG           (theme_get_current()->checkbox_bg)
#define THEME_CHECKBOX_BORDER       (theme_get_current()->checkbox_border)
#define THEME_CHECKBOX_CHECK        (theme_get_current()->checkbox_check)
#define THEME_DESKTOP_BG            (theme_get_current()->desktop_bg)
#define THEME_TASKBAR_BG            (theme_get_current()->taskbar_bg)
#define THEME_TASKBAR_HOVER         (theme_get_current()->taskbar_hover)
#define THEME_TASKBAR_ACTIVE        (theme_get_current()->taskbar_active)
#define THEME_START_BUTTON          (theme_get_current()->start_button)
#define THEME_GAUGE_BG              (theme_get_current()->gauge_bg)
#define THEME_GAUGE_FG              (theme_get_current()->gauge_fg)
#define THEME_MENU_BG               (theme_get_current()->menu_bg)
#define THEME_MENU_BORDER           (theme_get_current()->menu_border)
#define THEME_MENU_ITEM_HOVER       (theme_get_current()->menu_item_hover)
#define THEME_MENU_TEXT             (theme_get_current()->menu_text)
#define THEME_MENU_TEXT_DISABLED    (theme_get_current()->menu_text_disabled)
#define THEME_MENU_SEPARATOR        (theme_get_current()->menu_separator)
#define THEME_SCROLLBAR_BG          (theme_get_current()->scrollbar_bg)
#define THEME_SCROLLBAR_THUMB       (theme_get_current()->scrollbar_thumb)
#define THEME_SCROLLBAR_THUMB_HOVER (theme_get_current()->scrollbar_thumb_hover)
#define THEME_SELECTION_BG          (theme_get_current()->selection_bg)
#define THEME_SELECTION_TEXT        (theme_get_current()->selection_text)

// Status colors
#define THEME_COLOR_ERROR           (theme_get_current()->color_error)
#define THEME_COLOR_WARNING         (theme_get_current()->color_warning)
#define THEME_COLOR_SUCCESS         (theme_get_current()->color_success)
#define THEME_COLOR_INFO            (theme_get_current()->color_info)

// Link colors
#define THEME_LINK_COLOR            (theme_get_current()->link_color)
#define THEME_LINK_VISITED          (theme_get_current()->link_visited)
#define THEME_LINK_HOVER            (theme_get_current()->link_hover)

// Tooltip colors
#define THEME_TOOLTIP_BG            (theme_get_current()->tooltip_bg)
#define THEME_TOOLTIP_TEXT          (theme_get_current()->tooltip_text)
#define THEME_TOOLTIP_BORDER        (theme_get_current()->tooltip_border)

// Style access
#define THEME_STYLE                 (theme_get_current()->style)
#define THEME_IS_RETRO              (theme_get_current()->style == THEME_STYLE_RETRO)
#define THEME_IS_MODERN             (theme_get_current()->style == THEME_STYLE_MODERN)

// ============================================================================
// Typography Integration
// ============================================================================

// Notify the font system when theme changes (for render mode updates)
void theme_notify_font_system(void);

#endif // THEMES_H
