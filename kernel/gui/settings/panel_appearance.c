// Suppress warnings
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
// Suppress warnings
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
// panel_appearance.c - Appearance Panel for MayteraOS Settings
// Implements theme selection, accent colors, fonts, icon themes, and window styles
//
// Part of the unified Settings application framework (Task #59)
//
// Features:
// - Theme Selection: Grid of theme thumbnails with instant preview
// - Accent Color: Color picker with preset palette
// - Fonts: System/Title/Monospace font selection with size slider
// - Icon Theme: Icon style and size selection
// - Window Style: Decoration style, button position, borders, shadows
// - Dark Mode: Toggle with optional auto-schedule

#include "settings_panel.h"
#include "settings_widgets.h"
#include "panel_appearance.h"
#include "../settings.h"
#include "../window.h"
#include "../desktop.h"
#include "../themes.h"
#include "../syslog.h"
#include "../icons.h"
#include "../../types.h"
#include "../../serial.h"
#include "../../mm/heap.h"
#include "../../string.h"
#include "../../video/framebuffer.h"
#include "../../video/font.h"

// ============================================================================
// Appearance Panel Configuration
// ============================================================================

// Panel dimensions and layout
#define APPEARANCE_PADDING          15
#define APPEARANCE_LINE_HEIGHT      20
#define APPEARANCE_SECTION_GAP      25
#define APPEARANCE_SUBSECTION_GAP   15

// Theme preview/selection
#define THEME_PREVIEW_WIDTH         140
#define THEME_PREVIEW_HEIGHT        90
#define THEME_PREVIEW_GAP           15
#define THEME_PREVIEW_COLS          2

// Color picker
#define COLOR_SWATCH_SIZE           28
#define COLOR_SWATCH_GAP            6
#define COLOR_PALETTE_COLS          8

// Slider dimensions
#define SLIDER_WIDTH                180
#define SLIDER_HEIGHT               8
#define SLIDER_THUMB_WIDTH          14
#define SLIDER_THUMB_HEIGHT         20

// Toggle switch
#define TOGGLE_WIDTH                44
#define TOGGLE_HEIGHT               22

// Dropdown
#define DROPDOWN_WIDTH              180
#define DROPDOWN_HEIGHT             24

// ============================================================================
// Accent Color Palette (commonly used accent colors)
// ============================================================================
static const uint32_t g_accent_palette[] = {
    0x000078D4,  // Windows Blue
    0x00007AFF,  // macOS Blue
    0x003399FF,  // Sky Blue
    0x000099CC,  // Teal
    0x00107C10,  // Green
    0x0028CA42,  // Lime Green
    0x00FF8C00,  // Orange
    0x00E81123,  // Red
    0x00C30052,  // Magenta
    0x00881798,  // Purple
    0x00744DA9,  // Lavender
    0x00018574,  // Dark Teal
    0x00486860,  // Sage
    0x00525E54,  // Moss
    0x00515C6B,  // Steel Blue
    0x005D5A58,  // Brown Gray
};
#define ACCENT_PALETTE_SIZE (sizeof(g_accent_palette) / sizeof(g_accent_palette[0]))

// ============================================================================
// Window Decoration Styles
// ============================================================================
typedef enum {
    WINDOW_STYLE_MOTIF = 0,     // Classic X11/Motif style
    WINDOW_STYLE_MACOS,         // macOS traffic light buttons
    WINDOW_STYLE_WINDOWS,       // Windows-style right buttons
    WINDOW_STYLE_COUNT
} window_decoration_style_t;

static const char *g_window_style_names[] = {
    "Motif (Classic)",
    "macOS",
    "Windows"
};

// ============================================================================
// Font Options (System fonts available)
// ============================================================================
typedef enum {
    FONT_OPTION_SYSTEM = 0,     // Default system font
    FONT_OPTION_MONO,           // Monospace
    FONT_OPTION_COUNT
} font_option_t;

static const char *g_font_option_names[] = {
    "System Default",
    "Monospace"
};

// ============================================================================
// Icon Theme Options
// ============================================================================
typedef enum {
    ICON_THEME_DEFAULT = 0,
    ICON_THEME_OUTLINE,
    ICON_THEME_FILLED,
    ICON_THEME_COUNT
} icon_theme_t;

static const char *g_icon_theme_names[] = {
    "Default",
    "Outline",
    "Filled"
};

// Icon size options
typedef enum {
    ICON_SIZE_SMALL = 0,
    ICON_SIZE_MEDIUM,
    ICON_SIZE_LARGE,
    ICON_SIZE_COUNT
} icon_size_option_t;

static const char *g_icon_size_names[] = {
    "Small (16px)",
    "Medium (24px)",
    "Large (32px)"
};

// ============================================================================
// Appearance Panel State
// ============================================================================
typedef struct appearance_panel {
    // Current section being viewed
    int current_subsection;     // 0=Themes, 1=Colors, 2=Fonts, 3=Icons, 4=Windows

    // Selected values
    int selected_theme;
    uint32_t accent_color;
    int font_size_percent;      // 80-150%
    bool font_antialiasing;

    font_option_t system_font;
    font_option_t title_font;
    font_option_t mono_font;

    icon_theme_t icon_theme;
    icon_size_option_t icon_size;

    window_decoration_style_t window_style;
    bool titlebar_buttons_left;
    int border_thickness;       // 1-4 pixels
    bool window_shadows;

    bool dark_mode;
    bool dark_mode_auto;        // Auto switch by time
    int dark_mode_start_hour;   // 18 (6 PM)
    int dark_mode_end_hour;     // 6 (6 AM)

    // UI state
    int hover_theme;
    int hover_accent;
    bool slider_dragging;
    int scroll_offset;

} appearance_panel_t;

// Global panel state (singleton for now)
static appearance_panel_t *g_appearance_panel = NULL;

// ============================================================================
// Drawing Helper Functions
// ============================================================================

// Draw text at position (panel-relative coordinates, converted to screen)
static void appearance_draw_text(int32_t panel_x, int32_t panel_y, int32_t x, int32_t y,
                                  const char *text, uint32_t color) {
    int32_t px = panel_x + x;
    int32_t py = panel_y + y;

    while (*text) {
        const uint8_t *glyph = font_get_glyph(*text);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(px + col, py + row, color);
                    }
                }
            }
        }
        px += FONT_WIDTH;
        text++;
    }
}

// Draw a section header with separator line
static int appearance_draw_section_header(int32_t panel_x, int32_t panel_y,
                                           int y, const char *title, int panel_width) {
    // Draw title
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         title, 0x000066CC);
    y += APPEARANCE_LINE_HEIGHT;

    // Draw separator line
    fb_draw_line(panel_x + APPEARANCE_PADDING, panel_y + y,
                 panel_x + panel_width - APPEARANCE_PADDING, panel_y + y,
                 0x00C0C0C0);
    y += 8;

    return y;
}

// Draw a filled rounded rectangle (simplified - uses regular rect)
// Marked unused to suppress warnings - may be used in future for rounded UI elements
__attribute__((unused))
static void draw_rounded_rect_filled(int32_t x, int32_t y, int32_t w, int32_t h,
                                     uint32_t fill_color, uint32_t border_color) {
    fb_fill_rect(x, y, w, h, fill_color);
    fb_draw_rect(x, y, w, h, border_color);
}

// Draw a toggle switch
static void draw_toggle_switch(int32_t x, int32_t y, bool enabled, bool hovered) {
    uint32_t track_color = enabled ? 0x00007AFF : 0x00C0C0C0;
    uint32_t thumb_color = 0x00FFFFFF;
    uint32_t border_color = hovered ? 0x00808080 : 0x00A0A0A0;

    // Draw track
    fb_fill_rect(x, y, TOGGLE_WIDTH, TOGGLE_HEIGHT, track_color);
    fb_draw_rect(x, y, TOGGLE_WIDTH, TOGGLE_HEIGHT, border_color);

    // Draw thumb
    int thumb_x = enabled ? (x + TOGGLE_WIDTH - TOGGLE_HEIGHT + 2) : (x + 2);
    int thumb_y = y + 2;
    int thumb_size = TOGGLE_HEIGHT - 4;

    fb_fill_rect(thumb_x, thumb_y, thumb_size, thumb_size, thumb_color);
    fb_draw_rect(thumb_x, thumb_y, thumb_size, thumb_size, 0x00C0C0C0);
}

// Draw a horizontal slider
static void draw_slider(int32_t x, int32_t y, int value, int min_val, int max_val) {
    // Calculate thumb position
    float ratio = (float)(value - min_val) / (float)(max_val - min_val);
    int thumb_x = x + (int)(ratio * (SLIDER_WIDTH - SLIDER_THUMB_WIDTH));

    // Draw track background
    fb_fill_rect(x, y + (SLIDER_THUMB_HEIGHT - SLIDER_HEIGHT) / 2,
                 SLIDER_WIDTH, SLIDER_HEIGHT, 0x00D0D0D0);

    // Draw filled portion
    fb_fill_rect(x, y + (SLIDER_THUMB_HEIGHT - SLIDER_HEIGHT) / 2,
                 thumb_x - x + SLIDER_THUMB_WIDTH / 2, SLIDER_HEIGHT, 0x00007AFF);

    // Draw thumb
    fb_fill_rect(thumb_x, y, SLIDER_THUMB_WIDTH, SLIDER_THUMB_HEIGHT, 0x00FFFFFF);
    fb_draw_rect(thumb_x, y, SLIDER_THUMB_WIDTH, SLIDER_THUMB_HEIGHT, 0x00808080);
}

// Draw a dropdown (closed state)
static void draw_dropdown(int32_t x, int32_t y, const char *selected_text, bool hovered) {
    uint32_t bg_color = hovered ? 0x00E8E8E8 : 0x00FFFFFF;
    uint32_t border_color = hovered ? 0x00808080 : 0x00C0C0C0;

    // Draw background
    fb_fill_rect(x, y, DROPDOWN_WIDTH, DROPDOWN_HEIGHT, bg_color);
    fb_draw_rect(x, y, DROPDOWN_WIDTH, DROPDOWN_HEIGHT, border_color);

    // Draw text
    int text_x = x + 8;
    int text_y = y + (DROPDOWN_HEIGHT - FONT_HEIGHT) / 2;

    // Draw using direct pixel manipulation
    const char *text = selected_text;
    while (*text) {
        const uint8_t *glyph = font_get_glyph(*text);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(text_x + col, text_y + row, 0x00000000);
                    }
                }
            }
        }
        text_x += FONT_WIDTH;
        text++;
    }

    // Draw dropdown arrow
    int arrow_x = x + DROPDOWN_WIDTH - 16;
    int arrow_y = y + DROPDOWN_HEIGHT / 2 - 2;

    // Simple triangle arrow
    for (int i = 0; i < 5; i++) {
        fb_draw_line(arrow_x + i, arrow_y + i / 2,
                     arrow_x + 8 - i, arrow_y + i / 2, 0x00606060);
    }
}

// Draw a color swatch
static void draw_color_swatch(int32_t x, int32_t y, uint32_t color,
                               bool selected, bool hovered) {
    // Draw outer border if selected
    if (selected) {
        fb_draw_rect(x - 2, y - 2, COLOR_SWATCH_SIZE + 4, COLOR_SWATCH_SIZE + 4, 0x00000000);
        fb_draw_rect(x - 1, y - 1, COLOR_SWATCH_SIZE + 2, COLOR_SWATCH_SIZE + 2, 0x00FFFFFF);
    }

    // Draw swatch
    fb_fill_rect(x, y, COLOR_SWATCH_SIZE, COLOR_SWATCH_SIZE, color);

    // Draw border
    uint32_t border = hovered ? 0x00404040 : 0x00808080;
    fb_draw_rect(x, y, COLOR_SWATCH_SIZE, COLOR_SWATCH_SIZE, border);
}

// ============================================================================
// Theme Preview Drawing
// ============================================================================

// Draw a miniature theme preview
static void draw_theme_preview(int32_t x, int32_t y, int theme_id,
                                bool selected, bool hovered) {
    const theme_t *theme = theme_get_by_id(theme_id);
    if (!theme) return;

    // Selection/hover border
    uint32_t border_color = selected ? 0x00007AFF : (hovered ? 0x00808080 : 0x00C0C0C0);
    int border_width = selected ? 3 : 1;

    // Draw outer border
    for (int i = 0; i < border_width; i++) {
        fb_draw_rect(x - i, y - i,
                     THEME_PREVIEW_WIDTH + 2*i, THEME_PREVIEW_HEIGHT + 2*i,
                     border_color);
    }

    // Draw desktop area
    fb_fill_rect(x, y, THEME_PREVIEW_WIDTH, THEME_PREVIEW_HEIGHT, theme->desktop_bg);

    // Draw a mini window in the preview
    int win_x = x + 15;
    int win_y = y + 15;
    int win_w = 70;
    int win_h = 45;

    // Window titlebar
    fb_fill_rect(win_x, win_y, win_w, 12, theme->titlebar_active);

    // Window close button (tiny)
    fb_fill_rect(win_x + win_w - 10, win_y + 3, 6, 6, theme->close_button);

    // Window body
    fb_fill_rect(win_x, win_y + 12, win_w, win_h - 12, theme->window_bg);

    // Window border
    fb_draw_rect(win_x, win_y, win_w, win_h, theme->window_border);

    // Draw mini taskbar
    int taskbar_y = y + THEME_PREVIEW_HEIGHT - 12;
    fb_fill_rect(x, taskbar_y, THEME_PREVIEW_WIDTH, 12, theme->taskbar_bg);

    // Draw selection highlight in preview
    fb_fill_rect(x + THEME_PREVIEW_WIDTH - 30, taskbar_y + 2,
                 8, 8, theme->selection_bg);

    // Draw theme name below preview
    const char *name = theme->name;
    int name_len = strlen(name);
    int name_x = x + (THEME_PREVIEW_WIDTH - name_len * FONT_WIDTH) / 2;
    int name_y = y + THEME_PREVIEW_HEIGHT + 4;

    uint32_t name_color = selected ? 0x00007AFF : 0x00333333;

    while (*name) {
        const uint8_t *glyph = font_get_glyph(*name);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(name_x + col, name_y + row, name_color);
                    }
                }
            }
        }
        name_x += FONT_WIDTH;
        name++;
    }
}

// ============================================================================
// Panel Section Drawing Functions
// ============================================================================

// Draw Theme Selection section
static int appearance_draw_themes_section(int32_t panel_x, int32_t panel_y,
                                           int panel_width, int y,
                                           appearance_panel_t *panel) {
    y = appearance_draw_section_header(panel_x, panel_y, y, "Theme Selection", panel_width);

    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "Choose a color theme for your desktop:", 0x00333333);
    y += APPEARANCE_LINE_HEIGHT + 8;

    // Draw theme previews in a grid
    int theme_count = theme_get_count();
    int col = 0;
    (void)y; // Suppress unused warning - y is modified in loop

    for (int i = 0; i < theme_count; i++) {
        int preview_x = panel_x + APPEARANCE_PADDING +
                        col * (THEME_PREVIEW_WIDTH + THEME_PREVIEW_GAP);
        int preview_y = panel_y + y;

        bool selected = (i == panel->selected_theme);
        bool hovered = (i == panel->hover_theme);

        draw_theme_preview(preview_x, preview_y, i, selected, hovered);

        col++;
        if (col >= THEME_PREVIEW_COLS) {
            col = 0;
            y += THEME_PREVIEW_HEIGHT + APPEARANCE_LINE_HEIGHT + THEME_PREVIEW_GAP;
        }
    }

    // Finish last row if incomplete
    if (col != 0) {
        y += THEME_PREVIEW_HEIGHT + APPEARANCE_LINE_HEIGHT + THEME_PREVIEW_GAP;
    }

    y += APPEARANCE_SECTION_GAP;

    // Draw Apply button
    int btn_x = panel_x + panel_width - 100 - APPEARANCE_PADDING;
    int btn_y = panel_y + y;

    // Button background
    fb_fill_rect(btn_x, btn_y, 100, 28, 0x00007AFF);
    fb_draw_rect(btn_x, btn_y, 100, 28, 0x00005A9E);

    // Button text
    const char *btn_text = "Apply Theme";
    int btn_text_x = btn_x + (100 - strlen(btn_text) * FONT_WIDTH) / 2;
    int btn_text_y = btn_y + (28 - FONT_HEIGHT) / 2;

    const char *t = btn_text;
    while (*t) {
        const uint8_t *glyph = font_get_glyph(*t);
        if (glyph) {
            for (int r = 0; r < FONT_HEIGHT; r++) {
                uint8_t bits = glyph[r];
                for (int c = 0; c < FONT_WIDTH; c++) {
                    if (bits & (0x80 >> c)) {
                        fb_put_pixel(btn_text_x + c, btn_text_y + r, 0x00FFFFFF);
                    }
                }
            }
        }
        btn_text_x += FONT_WIDTH;
        t++;
    }

    y += 40;

    return y;
}

// Draw Accent Color section
static int appearance_draw_accent_section(int32_t panel_x, int32_t panel_y,
                                           int panel_width, int y,
                                           appearance_panel_t *panel) {
    y = appearance_draw_section_header(panel_x, panel_y, y, "Accent Color", panel_width);

    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "Select an accent color for highlights and buttons:", 0x00333333);
    y += APPEARANCE_LINE_HEIGHT + 8;

    // Draw color palette
    for (int i = 0; i < (int)ACCENT_PALETTE_SIZE; i++) {
        int col = i % COLOR_PALETTE_COLS;
        int row = i / COLOR_PALETTE_COLS;

        int swatch_x = panel_x + APPEARANCE_PADDING +
                       col * (COLOR_SWATCH_SIZE + COLOR_SWATCH_GAP);
        int swatch_y = panel_y + y + row * (COLOR_SWATCH_SIZE + COLOR_SWATCH_GAP);

        bool selected = (g_accent_palette[i] == panel->accent_color);
        bool hovered = (i == panel->hover_accent);

        draw_color_swatch(swatch_x, swatch_y, g_accent_palette[i], selected, hovered);
    }

    // Calculate height used by palette
    int rows = (ACCENT_PALETTE_SIZE + COLOR_PALETTE_COLS - 1) / COLOR_PALETTE_COLS;
    y += rows * (COLOR_SWATCH_SIZE + COLOR_SWATCH_GAP);

    y += APPEARANCE_SUBSECTION_GAP;

    // Show current color
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "Current accent:", 0x00333333);

    // Draw current color preview
    fb_fill_rect(panel_x + APPEARANCE_PADDING + 120, panel_y + y - 2,
                 60, FONT_HEIGHT + 4, panel->accent_color);
    fb_draw_rect(panel_x + APPEARANCE_PADDING + 120, panel_y + y - 2,
                 60, FONT_HEIGHT + 4, 0x00808080);

    y += APPEARANCE_SECTION_GAP;

    return y;
}

// Draw Font Settings section
static int appearance_draw_fonts_section(int32_t panel_x, int32_t panel_y,
                                          int panel_width, int y,
                                          appearance_panel_t *panel) {
    y = appearance_draw_section_header(panel_x, panel_y, y, "Font Settings", panel_width);

    // System font dropdown
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "System Font:", 0x00333333);
    draw_dropdown(panel_x + APPEARANCE_PADDING + 120, panel_y + y - 2,
                  g_font_option_names[panel->system_font], false);
    y += APPEARANCE_LINE_HEIGHT + 10;

    // Title font dropdown
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "Title Font:", 0x00333333);
    draw_dropdown(panel_x + APPEARANCE_PADDING + 120, panel_y + y - 2,
                  g_font_option_names[panel->title_font], false);
    y += APPEARANCE_LINE_HEIGHT + 10;

    // Monospace font dropdown
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "Monospace Font:", 0x00333333);
    draw_dropdown(panel_x + APPEARANCE_PADDING + 120, panel_y + y - 2,
                  g_font_option_names[panel->mono_font], false);
    y += APPEARANCE_LINE_HEIGHT + 15;

    // Font size slider
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "Font Size:", 0x00333333);

    draw_slider(panel_x + APPEARANCE_PADDING + 120, panel_y + y - 2,
                panel->font_size_percent, 80, 150);

    // Show percentage value
    char size_str[16];
    snprintf(size_str, sizeof(size_str), "%d%%", panel->font_size_percent);
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING + 120 + SLIDER_WIDTH + 10, y,
                         size_str, 0x00333333);
    y += APPEARANCE_LINE_HEIGHT + 15;

    // Font antialiasing toggle
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "Font Smoothing:", 0x00333333);
    draw_toggle_switch(panel_x + APPEARANCE_PADDING + 150, panel_y + y - 2,
                       panel->font_antialiasing, false);

    const char *aa_status = panel->font_antialiasing ? "On" : "Off";
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING + 200, y,
                         aa_status, 0x00666666);

    y += APPEARANCE_SECTION_GAP;

    return y;
}

// Draw Icon Theme section
static int appearance_draw_icons_section(int32_t panel_x, int32_t panel_y,
                                          int panel_width, int y,
                                          appearance_panel_t *panel) {
    y = appearance_draw_section_header(panel_x, panel_y, y, "Icon Theme", panel_width);

    // Icon theme dropdown
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "Icon Style:", 0x00333333);
    draw_dropdown(panel_x + APPEARANCE_PADDING + 120, panel_y + y - 2,
                  g_icon_theme_names[panel->icon_theme], false);
    y += APPEARANCE_LINE_HEIGHT + 10;

    // Icon size dropdown
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "Icon Size:", 0x00333333);
    draw_dropdown(panel_x + APPEARANCE_PADDING + 120, panel_y + y - 2,
                  g_icon_size_names[panel->icon_size], false);
    y += APPEARANCE_LINE_HEIGHT + 15;

    // Icon preview
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "Preview:", 0x00333333);
    y += APPEARANCE_LINE_HEIGHT + 5;

    // Draw some sample icons
    int preview_x = panel_x + APPEARANCE_PADDING;
    int icon_spacing = 40;

    // Determine size based on setting
    int size = (panel->icon_size == ICON_SIZE_SMALL) ? 16 :
               (panel->icon_size == ICON_SIZE_MEDIUM) ? 24 : 32;

    icon_draw_scaled(ICON_FOLDER, preview_x, panel_y + y, size, panel->accent_color);
    icon_draw_scaled(ICON_FILE, preview_x + icon_spacing, panel_y + y, size, panel->accent_color);
    icon_draw_scaled(ICON_COG, preview_x + icon_spacing * 2, panel_y + y, size, panel->accent_color);
    icon_draw_scaled(ICON_TERMINAL, preview_x + icon_spacing * 3, panel_y + y, size, panel->accent_color);
    icon_draw_scaled(ICON_IMAGE, preview_x + icon_spacing * 4, panel_y + y, size, panel->accent_color);

    y += size + APPEARANCE_SECTION_GAP;

    return y;
}

// Draw Window Style section
static int appearance_draw_window_section(int32_t panel_x, int32_t panel_y,
                                           int panel_width, int y,
                                           appearance_panel_t *panel) {
    y = appearance_draw_section_header(panel_x, panel_y, y, "Window Style", panel_width);

    // Decoration style dropdown
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "Decoration Style:", 0x00333333);
    draw_dropdown(panel_x + APPEARANCE_PADDING + 150, panel_y + y - 2,
                  g_window_style_names[panel->window_style], false);
    y += APPEARANCE_LINE_HEIGHT + 10;

    // Title bar button position toggle
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "Title Buttons:", 0x00333333);

    const char *pos_text = panel->titlebar_buttons_left ? "Left" : "Right";
    draw_dropdown(panel_x + APPEARANCE_PADDING + 150, panel_y + y - 2,
                  pos_text, false);
    y += APPEARANCE_LINE_HEIGHT + 10;

    // Border thickness slider
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "Border Width:", 0x00333333);
    draw_slider(panel_x + APPEARANCE_PADDING + 150, panel_y + y - 2,
                panel->border_thickness, 1, 4);

    char border_str[16];
    snprintf(border_str, sizeof(border_str), "%dpx", panel->border_thickness);
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING + 150 + SLIDER_WIDTH + 10, y,
                         border_str, 0x00333333);
    y += APPEARANCE_LINE_HEIGHT + 10;

    // Window shadows toggle
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "Window Shadows:", 0x00333333);
    draw_toggle_switch(panel_x + APPEARANCE_PADDING + 150, panel_y + y - 2,
                       panel->window_shadows, false);

    const char *shadow_status = panel->window_shadows ? "On" : "Off";
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING + 200, y,
                         shadow_status, 0x00666666);

    y += APPEARANCE_SECTION_GAP;

    return y;
}

// Draw Dark Mode section
static int appearance_draw_darkmode_section(int32_t panel_x, int32_t panel_y,
                                             int panel_width, int y,
                                             appearance_panel_t *panel) {
    y = appearance_draw_section_header(panel_x, panel_y, y, "Dark Mode", panel_width);

    // Dark mode toggle
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "Dark Mode:", 0x00333333);
    draw_toggle_switch(panel_x + APPEARANCE_PADDING + 120, panel_y + y - 2,
                       panel->dark_mode, false);

    const char *mode_text = panel->dark_mode ? "On" : "Off";
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING + 170, y,
                         mode_text, 0x00666666);
    y += APPEARANCE_LINE_HEIGHT + 10;

    // Auto-schedule toggle
    appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING, y,
                         "Auto Schedule:", 0x00333333);
    draw_toggle_switch(panel_x + APPEARANCE_PADDING + 120, panel_y + y - 2,
                       panel->dark_mode_auto, false);

    if (panel->dark_mode_auto) {
        char schedule_str[32];
        snprintf(schedule_str, sizeof(schedule_str), "%d:00 - %d:00",
                 panel->dark_mode_start_hour, panel->dark_mode_end_hour);
        appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING + 170, y,
                             schedule_str, 0x00666666);
    }
    y += APPEARANCE_LINE_HEIGHT + 10;

    // Description text
    if (panel->dark_mode_auto) {
        appearance_draw_text(panel_x, panel_y, APPEARANCE_PADDING + 20, y,
                             "Dark mode will activate automatically at sunset.", 0x00808080);
    }

    y += APPEARANCE_SECTION_GAP;

    return y;
}

// ============================================================================
// Main Panel Drawing Function
// ============================================================================

// Draw the complete appearance panel
void appearance_panel_draw(int32_t panel_x, int32_t panel_y,
                            int32_t panel_width, int32_t panel_height) {
    if (!g_appearance_panel) {
        // Initialize panel state on first draw
        g_appearance_panel = (appearance_panel_t *)kmalloc(sizeof(appearance_panel_t));
        if (!g_appearance_panel) {
            kprintf("[Appearance] Failed to allocate panel state\n");
            return;
        }

        // Initialize with current theme settings
        g_appearance_panel->current_subsection = 0;
        g_appearance_panel->selected_theme = theme_get_current_id();
        g_appearance_panel->accent_color = 0x00007AFF;  // Default macOS blue
        g_appearance_panel->font_size_percent = 100;
        g_appearance_panel->font_antialiasing = true;
        g_appearance_panel->system_font = FONT_OPTION_SYSTEM;
        g_appearance_panel->title_font = FONT_OPTION_SYSTEM;
        g_appearance_panel->mono_font = FONT_OPTION_MONO;
        g_appearance_panel->icon_theme = ICON_THEME_DEFAULT;
        g_appearance_panel->icon_size = ICON_SIZE_MEDIUM;
        g_appearance_panel->window_style = WINDOW_STYLE_WINDOWS;
        g_appearance_panel->titlebar_buttons_left = false;
        g_appearance_panel->border_thickness = 1;
        g_appearance_panel->window_shadows = true;
        g_appearance_panel->dark_mode = (theme_get_current_id() == THEME_DARK ||
                                          theme_get_current_id() == THEME_MODERN_DARK ||
                                          theme_get_current_id() == THEME_FLUENT_DARK);
        g_appearance_panel->dark_mode_auto = false;
        g_appearance_panel->dark_mode_start_hour = 18;
        g_appearance_panel->dark_mode_end_hour = 6;
        g_appearance_panel->hover_theme = -1;
        g_appearance_panel->hover_accent = -1;
        g_appearance_panel->slider_dragging = false;
        g_appearance_panel->scroll_offset = 0;

        kprintf("[Appearance] Panel initialized with theme %d\n",
                g_appearance_panel->selected_theme);
    }

    // Draw background
    fb_fill_rect(panel_x, panel_y, panel_width, panel_height, 0x00F8F8F8);

    // Draw all sections
    int y = APPEARANCE_PADDING;

    y = appearance_draw_themes_section(panel_x, panel_y, panel_width, y, g_appearance_panel);
    y = appearance_draw_accent_section(panel_x, panel_y, panel_width, y, g_appearance_panel);
    y = appearance_draw_darkmode_section(panel_x, panel_y, panel_width, y, g_appearance_panel);
    y = appearance_draw_fonts_section(panel_x, panel_y, panel_width, y, g_appearance_panel);
    y = appearance_draw_icons_section(panel_x, panel_y, panel_width, y, g_appearance_panel);
    y = appearance_draw_window_section(panel_x, panel_y, panel_width, y, g_appearance_panel);
}

// ============================================================================
// Event Handling
// ============================================================================

// Handle mouse click in appearance panel
bool appearance_panel_handle_click(int32_t panel_x, int32_t panel_y,
                                    int32_t panel_width, int32_t panel_height,
                                    int32_t click_x, int32_t click_y) {
    if (!g_appearance_panel) return false;

    // Convert to panel-relative coordinates
    int32_t rel_x = click_x - panel_x;
    int32_t rel_y = click_y - panel_y;

    // Check if click is within panel bounds
    if (rel_x < 0 || rel_x >= panel_width || rel_y < 0 || rel_y >= panel_height) {
        return false;
    }

    // Calculate Y positions for each section
    int theme_section_start = APPEARANCE_PADDING;
    int theme_section_end = theme_section_start + 40 +
                            ((theme_get_count() + 1) / 2) * (THEME_PREVIEW_HEIGHT + APPEARANCE_LINE_HEIGHT + THEME_PREVIEW_GAP);

    // Check theme preview clicks
    if (rel_y >= theme_section_start + 40 && rel_y < theme_section_end - 60) {
        int local_y = rel_y - (theme_section_start + 40);
        int row = local_y / (THEME_PREVIEW_HEIGHT + APPEARANCE_LINE_HEIGHT + THEME_PREVIEW_GAP);
        int local_x = rel_x - APPEARANCE_PADDING;
        int col = local_x / (THEME_PREVIEW_WIDTH + THEME_PREVIEW_GAP);

        if (col >= 0 && col < THEME_PREVIEW_COLS) {
            int theme_index = row * THEME_PREVIEW_COLS + col;
            if (theme_index >= 0 && theme_index < theme_get_count()) {
                g_appearance_panel->selected_theme = theme_index;
                kprintf("[Appearance] Theme selected: %s\n", theme_get_name(theme_index));
                return true;
            }
        }
    }

    // Check Apply Theme button click (estimate position)
    int btn_y = theme_section_end - 35;
    int btn_x = panel_width - 100 - APPEARANCE_PADDING;

    if (rel_x >= btn_x && rel_x < btn_x + 100 &&
        rel_y >= btn_y && rel_y < btn_y + 28) {
        // Apply the selected theme
        theme_set(g_appearance_panel->selected_theme);
        g_appearance_panel->dark_mode = (g_appearance_panel->selected_theme == THEME_DARK ||
                                          g_appearance_panel->selected_theme == THEME_MODERN_DARK ||
                                          g_appearance_panel->selected_theme == THEME_FLUENT_DARK);
        kprintf("[Appearance] Theme applied: %s\n",
                theme_get_name(g_appearance_panel->selected_theme));
        wm_invalidate_all();
        return true;
    }

    // Check accent color palette clicks
    int accent_section_start = theme_section_end + APPEARANCE_LINE_HEIGHT + 8;

    for (int i = 0; i < (int)ACCENT_PALETTE_SIZE; i++) {
        int col = i % COLOR_PALETTE_COLS;
        int row = i / COLOR_PALETTE_COLS;

        int swatch_x = APPEARANCE_PADDING + col * (COLOR_SWATCH_SIZE + COLOR_SWATCH_GAP);
        int swatch_y = accent_section_start + APPEARANCE_LINE_HEIGHT + 8 +
                       row * (COLOR_SWATCH_SIZE + COLOR_SWATCH_GAP);

        if (rel_x >= swatch_x && rel_x < swatch_x + COLOR_SWATCH_SIZE &&
            rel_y >= swatch_y && rel_y < swatch_y + COLOR_SWATCH_SIZE) {
            g_appearance_panel->accent_color = g_accent_palette[i];
            kprintf("[Appearance] Accent color selected: 0x%06X\n", g_accent_palette[i]);
            return true;
        }
    }

    // Check toggle switches
    // (Simplified - would need exact Y position calculations for full implementation)

    return false;
}

// Handle mouse move for hover effects
void appearance_panel_handle_mouse_move(int32_t panel_x, int32_t panel_y,
                                         int32_t panel_width, int32_t panel_height,
                                         int32_t mouse_x, int32_t mouse_y) {
    if (!g_appearance_panel) return;

    int32_t rel_x = mouse_x - panel_x;
    int32_t rel_y = mouse_y - panel_y;

    // Reset hover states
    g_appearance_panel->hover_theme = -1;
    g_appearance_panel->hover_accent = -1;

    // Check if mouse is within panel
    if (rel_x < 0 || rel_x >= panel_width || rel_y < 0 || rel_y >= panel_height) {
        return;
    }

    // Check theme hover
    int theme_section_start = APPEARANCE_PADDING + 40;

    if (rel_y >= theme_section_start) {
        int local_y = rel_y - theme_section_start;
        int row = local_y / (THEME_PREVIEW_HEIGHT + APPEARANCE_LINE_HEIGHT + THEME_PREVIEW_GAP);
        int local_x = rel_x - APPEARANCE_PADDING;
        int col = local_x / (THEME_PREVIEW_WIDTH + THEME_PREVIEW_GAP);

        if (col >= 0 && col < THEME_PREVIEW_COLS) {
            int theme_index = row * THEME_PREVIEW_COLS + col;
            if (theme_index >= 0 && theme_index < theme_get_count()) {
                g_appearance_panel->hover_theme = theme_index;
            }
        }
    }
}

// ============================================================================
// Panel Registration / Cleanup
// ============================================================================

// Initialize appearance panel
void appearance_panel_init(void) {
    kprintf("[Appearance] Panel module initialized\n");
    // Panel state is created on first draw
}

// Cleanup appearance panel
void appearance_panel_cleanup(void) {
    if (g_appearance_panel) {
        kfree(g_appearance_panel);
        g_appearance_panel = NULL;
    }
    kprintf("[Appearance] Panel module cleaned up\n");
}

// Get current selected theme
int appearance_get_selected_theme(void) {
    return g_appearance_panel ? g_appearance_panel->selected_theme : theme_get_current_id();
}

// Get current accent color
uint32_t appearance_get_accent_color(void) {
    return g_appearance_panel ? g_appearance_panel->accent_color : 0x00007AFF;
}

// Check if dark mode is enabled
bool appearance_is_dark_mode(void) {
    return g_appearance_panel ? g_appearance_panel->dark_mode : false;
}

// ============================================================================
// Settings Panel Framework Integration
// ============================================================================

// Panel initialization callback (for settings_panel_def_t)
static void appearance_panel_init_callback(settings_panel_t *panel) {
    if (!panel) return;

    // Initialize panel state if not already done
    if (!g_appearance_panel) {
        appearance_panel_init();
    }

    // Store panel pointer for callbacks
    panel->user_data = g_appearance_panel;
    panel->initialized = true;

    kprintf("[Appearance] Panel initialized via framework\n");
}

// Panel draw callback (for settings_panel_def_t)
static void appearance_panel_draw_callback(settings_panel_t *panel, int32_t x, int32_t y,
                                            int32_t width, int32_t height) {
    // Draw the panel content
    appearance_panel_draw(x, y, width, height);
}

// Panel event callback (for settings_panel_def_t)
static void appearance_panel_event_callback(settings_panel_t *panel, gui_event_t *event) {
    if (!panel || !event) return;

    // Handle mouse events
    if (event->type == EVENT_MOUSE_UP) {
        if (event->mouse_buttons & MOUSE_BUTTON_LEFT) {
            bool handled = appearance_panel_handle_click(
                panel->content_x, panel->content_y,
                panel->content_width, panel->content_height_visible,
                event->mouse_x, event->mouse_y
            );
            if (handled) {
                panel->dirty = true;
            }
        }
    } else if (event->type == EVENT_MOUSE_MOVE) {
        appearance_panel_handle_mouse_move(
            panel->content_x, panel->content_y,
            panel->content_width, panel->content_height_visible,
            event->mouse_x, event->mouse_y
        );
    }
}

// Panel apply callback (for settings_panel_def_t)
static void appearance_panel_apply_callback(settings_panel_t *panel) {
    if (!panel || !g_appearance_panel) return;

    // Apply selected theme
    if (g_appearance_panel->selected_theme != theme_get_current_id()) {
        theme_set(g_appearance_panel->selected_theme);
        kprintf("[Appearance] Theme applied: %s\n",
                theme_get_name(g_appearance_panel->selected_theme));
    }

    // Update dark mode state based on applied theme
    g_appearance_panel->dark_mode = (g_appearance_panel->selected_theme == THEME_DARK ||
                                      g_appearance_panel->selected_theme == THEME_MODERN_DARK ||
                                      g_appearance_panel->selected_theme == THEME_FLUENT_DARK);

    // Mark as not dirty since changes are applied
    panel->dirty = false;

    // Force full UI redraw
    wm_invalidate_all();

    kprintf("[Appearance] Settings applied\n");
}

// Panel cleanup callback (for settings_panel_def_t)
static void appearance_panel_cleanup_callback(settings_panel_t *panel) {
    if (panel) {
        panel->user_data = NULL;
        panel->initialized = false;
    }
    appearance_panel_cleanup();
}

// Panel definition for registration
static const settings_panel_def_t g_appearance_panel_def = {
    .name = "Appearance",
    .icon = "palette",
    .category = SETTINGS_CAT_APPEARANCE,
    .priority = 0,  // First panel in Appearance category
    .init = appearance_panel_init_callback,
    .draw = appearance_panel_draw_callback,
    .handle_event = appearance_panel_event_callback,
    .apply = appearance_panel_apply_callback,
    .cleanup = appearance_panel_cleanup_callback
};

// Register the appearance panel with the settings framework
int appearance_panel_register(void) {
    int result = settings_register_panel(&g_appearance_panel_def);
    if (result >= 0) {
        kprintf("[Appearance] Panel registered at index %d\n", result);
    } else {
        kprintf("[Appearance] Failed to register panel\n");
    }
    return result;
}

// Get the panel definition (for external access)
const settings_panel_def_t *appearance_panel_get_def(void) {
    return &g_appearance_panel_def;
}
