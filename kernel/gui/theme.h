// theme.h - Enhanced Theme Engine for MayteraOS GUI
// Supports loading themes from INI files, theme switching, and drawing helpers
#ifndef THEME_H
#define THEME_H

#include "../types.h"

// =============================================================================
// Theme Configuration Constants
// =============================================================================

#define THEME_MAX_THEMES        16      // Maximum number of loaded themes
#define THEME_NAME_LEN          64      // Maximum theme name length
#define THEME_AUTHOR_LEN        64      // Maximum author name length
#define THEME_PATH_LEN          256     // Maximum path length
#define THEME_FONT_NAME_LEN     32      // Maximum font name length

// =============================================================================
// Theme Color IDs (for theme_color() query function)
// =============================================================================

typedef enum {
    // General colors
    THEME_COLOR_BACKGROUND = 0,
    THEME_COLOR_FOREGROUND,
    THEME_COLOR_ACCENT,
    THEME_COLOR_SELECTION,
    THEME_COLOR_SELECTION_TEXT,

    // Window titlebar colors
    THEME_COLOR_TITLEBAR_ACTIVE,
    THEME_COLOR_TITLEBAR_INACTIVE,
    THEME_COLOR_TITLEBAR_TEXT,

    // Window body colors
    THEME_COLOR_WINDOW_BG,
    THEME_COLOR_WINDOW_BORDER,

    // Window control buttons
    THEME_COLOR_CLOSE_BUTTON,
    THEME_COLOR_CLOSE_BUTTON_HOVER,
    THEME_COLOR_MINIMIZE_BUTTON,
    THEME_COLOR_MAXIMIZE_BUTTON,

    // Button widget colors
    THEME_COLOR_BUTTON_FACE,
    THEME_COLOR_BUTTON_LIGHT,
    THEME_COLOR_BUTTON_SHADOW,
    THEME_COLOR_BUTTON_DARK,
    THEME_COLOR_BUTTON_TEXT,
    THEME_COLOR_BUTTON_DISABLED,

    // Text widget colors
    THEME_COLOR_LABEL_TEXT,
    THEME_COLOR_TEXTBOX_BG,
    THEME_COLOR_TEXTBOX_BORDER,
    THEME_COLOR_TEXTBOX_TEXT,
    THEME_COLOR_TEXTBOX_CURSOR,

    // Checkbox colors
    THEME_COLOR_CHECKBOX_BG,
    THEME_COLOR_CHECKBOX_BORDER,
    THEME_COLOR_CHECKBOX_CHECK,

    // Desktop colors
    THEME_COLOR_DESKTOP_BG,

    // Taskbar/Dock colors
    THEME_COLOR_TASKBAR_BG,
    THEME_COLOR_TASKBAR_HOVER,
    THEME_COLOR_TASKBAR_ACTIVE,
    THEME_COLOR_START_BUTTON,
    THEME_COLOR_GAUGE_BG,
    THEME_COLOR_GAUGE_FG,

    // Menu colors
    THEME_COLOR_MENU_BG,
    THEME_COLOR_MENU_BORDER,
    THEME_COLOR_MENU_ITEM_HOVER,
    THEME_COLOR_MENU_TEXT,
    THEME_COLOR_MENU_TEXT_DISABLED,
    THEME_COLOR_MENU_SEPARATOR,

    // Scrollbar colors
    THEME_COLOR_SCROLLBAR_BG,
    THEME_COLOR_SCROLLBAR_THUMB,
    THEME_COLOR_SCROLLBAR_THUMB_HOVER,

    // Tab colors
    THEME_COLOR_TAB_BG,
    THEME_COLOR_TAB_ACTIVE,
    THEME_COLOR_TAB_BORDER,

    // Tooltip colors
    THEME_COLOR_TOOLTIP_BG,
    THEME_COLOR_TOOLTIP_BORDER,
    THEME_COLOR_TOOLTIP_TEXT,

    // Progress bar colors
    THEME_COLOR_PROGRESS_BG,
    THEME_COLOR_PROGRESS_FG,

    THEME_COLOR_COUNT   // Total number of color IDs
} theme_color_id_t;

// =============================================================================
// Theme Font IDs (for theme_font() query function)
// =============================================================================

typedef enum {
    THEME_FONT_SYSTEM = 0,      // Default system font
    THEME_FONT_TITLE,           // Window title font
    THEME_FONT_MENU,            // Menu font
    THEME_FONT_MONOSPACE,       // Monospace/terminal font
    THEME_FONT_SMALL,           // Small text font
    THEME_FONT_LARGE,           // Large/heading font

    THEME_FONT_COUNT            // Total number of font IDs
} theme_font_id_t;

// =============================================================================
// Theme Metric IDs (for theme_metric() query function)
// =============================================================================

typedef enum {
    THEME_METRIC_BORDER_WIDTH = 0,
    THEME_METRIC_TITLEBAR_HEIGHT,
    THEME_METRIC_BUTTON_WIDTH,
    THEME_METRIC_BUTTON_HEIGHT,
    THEME_METRIC_SCROLLBAR_WIDTH,
    THEME_METRIC_MENU_ITEM_HEIGHT,
    THEME_METRIC_ICON_SIZE,
    THEME_METRIC_CORNER_RADIUS,
    THEME_METRIC_PADDING,
    THEME_METRIC_SPACING,
    THEME_METRIC_TAB_HEIGHT,

    THEME_METRIC_COUNT          // Total number of metric IDs
} theme_metric_id_t;

// =============================================================================
// Decoration Styles
// =============================================================================

typedef enum {
    THEME_STYLE_FLAT = 0,       // Flat/modern style
    THEME_STYLE_MOTIF,          // Classic Motif/CDE 3D style
    THEME_STYLE_WIN95,          // Windows 95/98 classic style
    THEME_STYLE_WIN11,          // Modern Windows 11 style with rounded corners
    THEME_STYLE_MAC,            // macOS style
    THEME_STYLE_GTK,            // GTK/GNOME style

    THEME_STYLE_COUNT
} theme_style_t;

// =============================================================================
// Button States (for drawing helpers)
// =============================================================================

typedef enum {
    BUTTON_STATE_NORMAL = 0,
    BUTTON_STATE_HOVER,
    BUTTON_STATE_PRESSED,
    BUTTON_STATE_DISABLED,
    BUTTON_STATE_FOCUSED
} button_state_t;

// =============================================================================
// Frame Styles (for drawing helpers)
// =============================================================================

typedef enum {
    FRAME_STYLE_FLAT = 0,       // No 3D effect
    FRAME_STYLE_RAISED,         // Raised 3D effect
    FRAME_STYLE_SUNKEN,         // Sunken 3D effect
    FRAME_STYLE_ETCHED,         // Etched border
    FRAME_STYLE_RIDGE           // Ridge border
} frame_style_t;

// =============================================================================
// Theme Font Definition
// =============================================================================

typedef struct {
    char name[THEME_FONT_NAME_LEN];
    int size;
    bool bold;
    bool italic;
} theme_font_def_t;

// =============================================================================
// Theme Metrics Definition
// =============================================================================

typedef struct {
    int border_width;
    int titlebar_height;
    int button_width;
    int button_height;
    int scrollbar_width;
    int menu_item_height;
    int icon_size;
    int corner_radius;
    int padding;
    int spacing;
    int tab_height;
} theme_metrics_t;

// =============================================================================
// Theme Decorations Definition
// =============================================================================

typedef struct {
    theme_style_t style;
    int corner_radius;
    bool shadow_enabled;
    int shadow_offset_x;
    int shadow_offset_y;
    int shadow_blur;
    uint32_t shadow_color;
} theme_decorations_t;

// =============================================================================
// Complete Theme Definition Structure
// =============================================================================

typedef struct theme {
    // Metadata
    char name[THEME_NAME_LEN];
    char author[THEME_AUTHOR_LEN];
    char version[16];
    char path[THEME_PATH_LEN];      // Path to theme file (if loaded from file)
    bool is_builtin;                // True for built-in themes

    // All colors indexed by theme_color_id_t
    uint32_t colors[THEME_COLOR_COUNT];

    // Font definitions
    theme_font_def_t fonts[THEME_FONT_COUNT];

    // Metrics
    theme_metrics_t metrics;

    // Decorations
    theme_decorations_t decorations;

} theme_t;

// =============================================================================
// Theme Engine API - Initialization and Lifecycle
// =============================================================================

// Initialize the theme system
// Returns 0 on success, -1 on failure
int theme_engine_init(void);

// Shutdown the theme system and free resources
void theme_shutdown(void);

// =============================================================================
// Theme Engine API - Loading and Switching
// =============================================================================

// Load a theme from an INI file
// theme_name: Just the name (will look in /System/Themes/<name>/theme.ini)
// Returns theme index on success, -1 on failure
int theme_load(const char *theme_name);

// Load a theme from a specific path
// Returns theme index on success, -1 on failure
int theme_load_from_path(const char *path);

// Set the active theme by name
// Returns 0 on success, -1 if theme not found
int theme_set_active(const char *theme_name);

// Set the active theme by index
// Returns 0 on success, -1 if invalid index
int theme_set_active_by_index(int index);

// =============================================================================
// Theme Engine API - Query Functions
// =============================================================================

// Get the currently active theme
theme_t *theme_get_active(void);

// Get a theme by name
// Returns NULL if not found
theme_t *theme_get_by_name(const char *name);

// Get a theme by index
// Returns NULL if invalid index
theme_t *theme_get_by_index(int index);

// Get the number of loaded themes
int theme_get_count(void);

// Get the active theme index
int theme_get_active_index(void);

// Get list of theme names (returns pointer to static array)
const char **theme_get_names(int *count);

// =============================================================================
// Theme Engine API - Color, Font, and Metric Queries
// =============================================================================

// Get a color from the active theme by ID
uint32_t theme_color(theme_color_id_t id);

// Get a color with a custom alpha blend
// alpha: 0-255 (0=transparent, 255=opaque)
uint32_t theme_color_alpha(theme_color_id_t id, uint8_t alpha);

// Blend two colors
// ratio: 0.0 = pure c1, 1.0 = pure c2
uint32_t theme_blend_colors(uint32_t c1, uint32_t c2, float ratio);

// Get font definition from the active theme by ID
const theme_font_def_t *theme_font(theme_font_id_t id);

// Get a metric from the active theme by ID
int theme_metric(theme_metric_id_t id);

// Get the decoration style
theme_style_t theme_get_style(void);

// =============================================================================
// Theme Engine API - Drawing Helpers
// =============================================================================

// Draw a themed button
// state: BUTTON_STATE_NORMAL, BUTTON_STATE_HOVER, etc.
void theme_draw_button(int x, int y, int w, int h, button_state_t state, const char *text);

// Draw a themed frame/border
// style: FRAME_STYLE_FLAT, FRAME_STYLE_RAISED, etc.
void theme_draw_frame(int x, int y, int w, int h, frame_style_t style);

// Draw a themed 3D border (for window decorations)
void theme_draw_3d_border(int x, int y, int w, int h, bool raised);

// Draw a themed window titlebar
void theme_draw_titlebar(int x, int y, int w, int h, const char *title, bool active);

// Draw a themed scrollbar thumb
void theme_draw_scrollbar_thumb(int x, int y, int w, int h, bool horizontal, bool hovered);

// Draw a themed progress bar
void theme_draw_progress_bar(int x, int y, int w, int h, int percent);

// Draw a themed menu item
void theme_draw_menu_item(int x, int y, int w, int h, const char *text, bool selected, bool disabled);

// Draw a themed checkbox
void theme_draw_checkbox(int x, int y, bool checked, bool hovered, bool disabled);

// Draw a themed radio button
void theme_draw_radio_button(int x, int y, bool selected, bool hovered, bool disabled);

// Draw a themed tab
void theme_draw_tab(int x, int y, int w, int h, const char *text, bool active);

// Draw themed tooltip background
void theme_draw_tooltip(int x, int y, int w, int h);

// =============================================================================
// Theme Engine API - Color Utilities
// =============================================================================

// Parse a hex color string (#RRGGBB or #RGB)
// Returns the color value, or 0 on parse error
uint32_t theme_parse_color(const char *str);

// #404 batch-1: boot self-test for the theme-file line tokenizer Rust seam
// (theme_parse_line_rs vs theme_parse_line_c). Bounded, once (#426).
void theme_parse_rust_selftest(void);

// Convert color to hex string (returns static buffer)
const char *theme_color_to_string(uint32_t color);

// Lighten a color by a percentage (0-100)
uint32_t theme_lighten(uint32_t color, int percent);

// Darken a color by a percentage (0-100)
uint32_t theme_darken(uint32_t color, int percent);

// =============================================================================
// Compatibility Macros (for migration from old themes.h)
// =============================================================================

// These macros provide backwards compatibility with the old theme system
#define THEME_TITLEBAR_ACTIVE       theme_color(THEME_COLOR_TITLEBAR_ACTIVE)
#define THEME_TITLEBAR_INACTIVE     theme_color(THEME_COLOR_TITLEBAR_INACTIVE)
#define THEME_TITLEBAR_TEXT         theme_color(THEME_COLOR_TITLEBAR_TEXT)
#define THEME_WINDOW_BG             theme_color(THEME_COLOR_WINDOW_BG)
#define THEME_WINDOW_BORDER         theme_color(THEME_COLOR_WINDOW_BORDER)
#define THEME_CLOSE_BUTTON          theme_color(THEME_COLOR_CLOSE_BUTTON)
#define THEME_CLOSE_BUTTON_HOVER    theme_color(THEME_COLOR_CLOSE_BUTTON_HOVER)
#define THEME_MINIMIZE_BUTTON       theme_color(THEME_COLOR_MINIMIZE_BUTTON)
#define THEME_MAXIMIZE_BUTTON       theme_color(THEME_COLOR_MAXIMIZE_BUTTON)
#define THEME_BUTTON_BG             theme_color(THEME_COLOR_BUTTON_FACE)
#define THEME_BUTTON_BG_HOVER       theme_color(THEME_COLOR_BUTTON_LIGHT)
#define THEME_BUTTON_BG_PRESSED     theme_color(THEME_COLOR_BUTTON_SHADOW)
#define THEME_BUTTON_BORDER         theme_color(THEME_COLOR_BUTTON_DARK)
#define THEME_BUTTON_TEXT           theme_color(THEME_COLOR_BUTTON_TEXT)
#define THEME_BUTTON_DISABLED       theme_color(THEME_COLOR_BUTTON_DISABLED)
#define THEME_LABEL_TEXT            theme_color(THEME_COLOR_LABEL_TEXT)
#define THEME_TEXTBOX_BG            theme_color(THEME_COLOR_TEXTBOX_BG)
#define THEME_TEXTBOX_BORDER        theme_color(THEME_COLOR_TEXTBOX_BORDER)
#define THEME_TEXTBOX_TEXT          theme_color(THEME_COLOR_TEXTBOX_TEXT)
#define THEME_TEXTBOX_CURSOR        theme_color(THEME_COLOR_TEXTBOX_CURSOR)
#define THEME_CHECKBOX_BG           theme_color(THEME_COLOR_CHECKBOX_BG)
#define THEME_CHECKBOX_BORDER       theme_color(THEME_COLOR_CHECKBOX_BORDER)
#define THEME_CHECKBOX_CHECK        theme_color(THEME_COLOR_CHECKBOX_CHECK)
#define THEME_DESKTOP_BG            theme_color(THEME_COLOR_DESKTOP_BG)
#define THEME_TASKBAR_BG            theme_color(THEME_COLOR_TASKBAR_BG)
#define THEME_TASKBAR_HOVER         theme_color(THEME_COLOR_TASKBAR_HOVER)
#define THEME_TASKBAR_ACTIVE        theme_color(THEME_COLOR_TASKBAR_ACTIVE)
#define THEME_START_BUTTON          theme_color(THEME_COLOR_START_BUTTON)
#define THEME_GAUGE_BG              theme_color(THEME_COLOR_GAUGE_BG)
#define THEME_GAUGE_FG              theme_color(THEME_COLOR_GAUGE_FG)
#define THEME_MENU_BG               theme_color(THEME_COLOR_MENU_BG)
#define THEME_MENU_BORDER           theme_color(THEME_COLOR_MENU_BORDER)
#define THEME_MENU_ITEM_HOVER       theme_color(THEME_COLOR_MENU_ITEM_HOVER)
#define THEME_MENU_TEXT             theme_color(THEME_COLOR_MENU_TEXT)
#define THEME_MENU_TEXT_DISABLED    theme_color(THEME_COLOR_MENU_TEXT_DISABLED)
#define THEME_MENU_SEPARATOR        theme_color(THEME_COLOR_MENU_SEPARATOR)
#define THEME_SCROLLBAR_BG          theme_color(THEME_COLOR_SCROLLBAR_BG)
#define THEME_SCROLLBAR_THUMB       theme_color(THEME_COLOR_SCROLLBAR_THUMB)
#define THEME_SCROLLBAR_THUMB_HOVER theme_color(THEME_COLOR_SCROLLBAR_THUMB_HOVER)
#define THEME_SELECTION_BG          theme_color(THEME_COLOR_SELECTION)
#define THEME_SELECTION_TEXT        theme_color(THEME_COLOR_SELECTION_TEXT)

// =============================================================================
// Theme Persistence and Runtime Switching (Added by P14 - Theme Specialist)
// =============================================================================

// Save current theme selection to /CONFIG/THEME.CFG
int theme_save_config(void);

// Load theme selection from config file
int theme_load_config(void);

// Switch to a theme by index and notify all callbacks (with persistence)
int theme_switch_to(int index);

// Callback type for theme change notification
typedef void (*theme_change_callback_t)(int new_theme_index);

// Register a callback to be notified when theme changes
int theme_register_callback(theme_change_callback_t callback);

// =============================================================================
// Theme Syscall Implementations (for userland access)
// =============================================================================

int64_t sys_theme_get_active(void);
int64_t sys_theme_get_color(uint64_t color_id);
int64_t sys_theme_get_count(void);
int64_t sys_theme_set_active(uint64_t theme_index);
int64_t sys_theme_get_name(uint64_t theme_index, char *buf, uint64_t buf_size);
int64_t sys_theme_get_metric(uint64_t metric_id);

// =============================================================================
// Legacy API Compatibility (wrapper functions)
// =============================================================================

// These functions maintain compatibility with the old themes.h API
static inline const theme_t *theme_get_current(void) {
    return theme_get_active();
}

static inline void theme_set(int theme_id) {
    theme_switch_to(theme_id);  // Use switch_to for persistence
}

static inline int theme_get_current_id(void) {
    return theme_get_active_index();
}

static inline const char *theme_get_name(int theme_id) {
    theme_t *t = theme_get_by_index(theme_id);
    return t ? t->name : "Unknown";
}

#endif // THEME_H
