// Suppress warnings
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
// Suppress warnings
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
// panel_desktop.c - Desktop Settings Panel for MayteraOS Settings App
// Part of the unified Settings application framework (Task #59)
//
// Handles wallpaper selection, screensaver settings, hot corners, and desktop icons
#include "panel_desktop.h"
#include "settings_widgets.h"
#include "../window.h"
#include "../desktop.h"
#include "../screensaver.h"
#include "../themes.h"
#include "../image.h"
#include "../icons.h"
#include "../../types.h"
#include "../../serial.h"
#include "../../mm/heap.h"
#include "../../string.h"
#include "../../video/framebuffer.h"
#include "../../video/font.h"
#include "../../fs/fat.h"

// External filesystem
extern fat_fs_t g_fat_fs;

// ============================================================================
// Constants
// ============================================================================

#define DESKTOP_PANEL_PADDING       16
#define DESKTOP_SECTION_SPACING     24
#define DESKTOP_LINE_HEIGHT         24
#define DESKTOP_BUTTON_HEIGHT       28
#define DESKTOP_BUTTON_WIDTH        100
#define DESKTOP_CHECKBOX_SIZE       16

// Wallpaper thumbnail dimensions
#define WALLPAPER_THUMB_WIDTH       80
#define WALLPAPER_THUMB_HEIGHT      50
#define WALLPAPER_THUMB_SPACING     12
#define WALLPAPER_THUMBS_PER_ROW    4
#define WALLPAPER_THUMB_BORDER      2

// ============================================================================
// Wallpaper Entries
// ============================================================================

static const char *g_wallpaper_files[] = {
    "BACK.BMP",
    "EBERG01.BMP", "EBERG02.BMP", "EBERG03.BMP", "EBERG04.BMP", "EBERG05.BMP",
    "EBERG06.BMP", "EBERG07.BMP", "EBERG08.BMP", "EBERG09.BMP", "EBERG10.BMP",
    "OCEAN01.BMP", "OCEAN02.BMP", "OCEAN03.BMP", "OCEAN04.BMP", "OCEAN05.BMP",
    "MACRO01.BMP", "MACRO02.BMP", "MACRO03.BMP", "MACRO04.BMP", "MACRO05.BMP",
    NULL
};

static const char *g_position_names[] = {
    "Center", "Tile", "Stretch", "Fit", "Fill", NULL
};

static const char *g_hot_corner_actions[] = {
    "None", "Show Desktop", "Start Screensaver", "Disable Screensaver", "Lock Screen", NULL
};

// ============================================================================
// Panel State
// ============================================================================

typedef struct {
    bool initialized;

    // Wallpaper settings
    int current_wallpaper;
    int position_mode;
    bool use_solid_color;
    uint32_t solid_color;
    bool use_gradient;
    uint32_t gradient_color1;
    uint32_t gradient_color2;
    int gradient_direction;

    // Screensaver settings
    int screensaver_type;
    int screensaver_timeout;
    bool password_on_wake;

    // Hot corners
    int hot_corner_tl;
    int hot_corner_tr;
    int hot_corner_bl;
    int hot_corner_br;

    // Desktop icons
    bool show_icons;
    int icon_size;
    bool auto_arrange;
    bool grid_alignment;

    // UI state
    int scroll_offset;
    int max_scroll;
    int selected_section;
    int hover_wallpaper;
    bool preview_visible;

    // Thumbnail cache
    uint32_t *thumbnails[32];
    bool thumbnails_loaded[32];
} desktop_panel_state_t;

static desktop_panel_state_t g_state = {0};

// ============================================================================
// Helper Drawing Functions
// ============================================================================

static void draw_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    while (*text) {
        const uint8_t *glyph = font_get_glyph(*text);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(x + col, y + row, color);
                    }
                }
            }
        }
        x += FONT_WIDTH;
        text++;
    }
}

static void draw_section_header(int32_t x, int32_t y, int32_t w, const char *title, bool expanded) {
    uint32_t bg_color = 0x4A90C2;
    uint32_t text_color = 0xFFFFFF;

    fb_fill_rect(x, y, w, 24, bg_color);

    const char *indicator = expanded ? "[-]" : "[+]";
    draw_text(x + 8, y + 5, indicator, text_color);
    draw_text(x + 36, y + 5, title, text_color);
}

static void draw_checkbox(int32_t x, int32_t y, const char *label, bool checked) {
    fb_fill_rect(x, y, 16, 16, 0xFFFFFF);
    fb_draw_rect(x, y, 16, 16, THEME_MENU_TEXT_DISABLED);

    if (checked) {
        for (int i = 0; i < 4; i++) {
            fb_put_pixel(x + 3 + i, y + 7 + i, THEME_LABEL_TEXT);
            fb_put_pixel(x + 3 + i + 1, y + 7 + i, THEME_LABEL_TEXT);
        }
        for (int i = 0; i < 6; i++) {
            fb_put_pixel(x + 6 + i, y + 10 - i, THEME_LABEL_TEXT);
            fb_put_pixel(x + 6 + i + 1, y + 10 - i, THEME_LABEL_TEXT);
        }
    }

    draw_text(x + 24, y + 2, label, THEME_LABEL_TEXT);
}

static void draw_dropdown(int32_t x, int32_t y, int32_t w, const char *value) {
    fb_fill_rect(x, y, w, DESKTOP_BUTTON_HEIGHT, 0xFFFFFF);
    fb_draw_rect(x, y, w, DESKTOP_BUTTON_HEIGHT, THEME_MENU_TEXT_DISABLED);
    draw_text(x + 8, y + 7, value, THEME_LABEL_TEXT);

    int arrow_x = x + w - 18;
    int arrow_y = y + 12;
    for (int i = 0; i < 5; i++) {
        fb_put_pixel(arrow_x + i, arrow_y + i/2, 0x404040);
        fb_put_pixel(arrow_x + 8 - i, arrow_y + i/2, 0x404040);
    }
}

static void draw_button(int32_t x, int32_t y, int32_t w, int32_t h,
                       const char *text, bool pressed, bool enabled) {
    uint32_t bg_color = !enabled ? 0xA0A0A0 : (pressed ? 0x808080 : 0xC0C0C0);
    uint32_t border_color = !enabled ? 0x808080 : THEME_MENU_TEXT_DISABLED;
    uint32_t text_color = !enabled ? THEME_MENU_TEXT_DISABLED : THEME_LABEL_TEXT;

    fb_fill_rect(x, y, w, h, bg_color);
    fb_draw_rect(x, y, w, h, border_color);

    int text_len = strlen(text);
    int text_x = x + (w - text_len * FONT_WIDTH) / 2;
    int text_y = y + (h - FONT_HEIGHT) / 2;
    draw_text(text_x, text_y, text, text_color);
}

static void draw_color_swatch(int32_t x, int32_t y, int32_t size, uint32_t color) {
    fb_fill_rect(x, y, size, size, color);
    fb_draw_rect(x, y, size, size, 0x404040);
}

// ============================================================================
// Section Drawing Functions
// ============================================================================

static int draw_wallpaper_section(int32_t x, int32_t y, int32_t w) {
    int32_t cy = y;

    draw_text(x, cy, "Wallpaper", 0x0066CC);
    cy += DESKTOP_LINE_HEIGHT + 8;

    // Wallpaper thumbnail grid
    int idx = 0;
    while (g_wallpaper_files[idx] && idx < 20) {
        int col = idx % WALLPAPER_THUMBS_PER_ROW;
        int row = idx / WALLPAPER_THUMBS_PER_ROW;

        int tx = x + col * (WALLPAPER_THUMB_WIDTH + WALLPAPER_THUMB_SPACING);
        int ty = cy + row * (WALLPAPER_THUMB_HEIGHT + WALLPAPER_THUMB_SPACING + 16);

        // Border (blue if selected)
        uint32_t border_color = (idx == g_state.current_wallpaper) ? 0x0066CC : 0x808080;
        int border_w = (idx == g_state.current_wallpaper) ? 2 : 1;
        fb_draw_rect(tx - border_w, ty - border_w,
                    WALLPAPER_THUMB_WIDTH + border_w * 2,
                    WALLPAPER_THUMB_HEIGHT + border_w * 2, border_color);
        if (idx == g_state.current_wallpaper) {
            fb_draw_rect(tx - 1, ty - 1, WALLPAPER_THUMB_WIDTH + 2,
                        WALLPAPER_THUMB_HEIGHT + 2, border_color);
        }

        // Placeholder (would show actual thumbnail)
        uint32_t placeholder = 0x2E5A88 + idx * 0x111111;
        fb_fill_rect(tx, ty, WALLPAPER_THUMB_WIDTH, WALLPAPER_THUMB_HEIGHT, placeholder);

        // Filename
        draw_text(tx, ty + WALLPAPER_THUMB_HEIGHT + 2, g_wallpaper_files[idx], THEME_LABEL_TEXT);

        idx++;
    }

    int rows = (idx + WALLPAPER_THUMBS_PER_ROW - 1) / WALLPAPER_THUMBS_PER_ROW;
    cy += rows * (WALLPAPER_THUMB_HEIGHT + WALLPAPER_THUMB_SPACING + 16);
    cy += DESKTOP_SECTION_SPACING;

    // Position dropdown
    draw_text(x, cy + 5, "Position:", THEME_LABEL_TEXT);
    draw_dropdown(x + 80, cy, 120, g_position_names[g_state.position_mode]);
    cy += DESKTOP_LINE_HEIGHT + 10;

    // Solid color option
    draw_checkbox(x, cy, "Use solid color", g_state.use_solid_color);
    draw_color_swatch(x + 160, cy - 2, 22, g_state.solid_color);
    cy += DESKTOP_LINE_HEIGHT + 8;

    // Gradient option
    draw_checkbox(x, cy, "Use gradient", g_state.use_gradient);
    draw_color_swatch(x + 160, cy - 2, 22, g_state.gradient_color1);
    draw_text(x + 186, cy + 2, "to", THEME_MENU_TEXT_DISABLED);
    draw_color_swatch(x + 210, cy - 2, 22, g_state.gradient_color2);

    return cy - y + DESKTOP_LINE_HEIGHT;
}

static int draw_screensaver_section(int32_t x, int32_t y, int32_t w) {
    int32_t cy = y;

    draw_text(x, cy, "Screensaver", 0x0066CC);
    cy += DESKTOP_LINE_HEIGHT + 8;

    // Type dropdown
    draw_text(x, cy + 5, "Type:", THEME_LABEL_TEXT);
    const char *ss_name = screensaver_get_type_name(g_state.screensaver_type);
    draw_dropdown(x + 80, cy, 150, ss_name);
    draw_button(x + 240, cy, 80, DESKTOP_BUTTON_HEIGHT, "Preview", false, true);
    cy += DESKTOP_LINE_HEIGHT + 12;

    // Timeout
    draw_text(x, cy + 5, "Wait:", THEME_LABEL_TEXT);
    char timeout_str[32];
    if (g_state.screensaver_timeout == 0) {
        snprintf(timeout_str, sizeof(timeout_str), "Never");
    } else {
        snprintf(timeout_str, sizeof(timeout_str), "%d minutes", g_state.screensaver_timeout);
    }
    draw_dropdown(x + 80, cy, 120, timeout_str);
    cy += DESKTOP_LINE_HEIGHT + 12;

    // Password option
    draw_checkbox(x, cy, "Require password on wake", g_state.password_on_wake);

    return cy - y + DESKTOP_LINE_HEIGHT;
}

static int draw_hot_corners_section(int32_t x, int32_t y, int32_t w) {
    int32_t cy = y;

    draw_text(x, cy, "Hot Corners", 0x0066CC);
    cy += DESKTOP_LINE_HEIGHT + 8;

    // Screen preview
    int px = x + 60;
    int py = cy;
    int pw = 160;
    int ph = 100;

    fb_draw_rect(px, py, pw, ph, 0x808080);
    fb_fill_rect(px + 1, py + 1, pw - 2, ph - 2, 0xE0E0E0);

    // Corner indicators
    uint32_t corner_color = 0x4A90C2;
    int cs = 16;
    fb_fill_rect(px, py, cs, cs, corner_color);
    fb_fill_rect(px + pw - cs, py, cs, cs, corner_color);
    fb_fill_rect(px, py + ph - cs, cs, cs, corner_color);
    fb_fill_rect(px + pw - cs, py + ph - cs, cs, cs, corner_color);

    cy = py + ph + 16;

    // Corner action dropdowns
    draw_text(x, cy + 5, "Top-Left:", THEME_LABEL_TEXT);
    draw_dropdown(x + 100, cy, 160, g_hot_corner_actions[g_state.hot_corner_tl]);
    cy += DESKTOP_LINE_HEIGHT + 8;

    draw_text(x, cy + 5, "Top-Right:", THEME_LABEL_TEXT);
    draw_dropdown(x + 100, cy, 160, g_hot_corner_actions[g_state.hot_corner_tr]);
    cy += DESKTOP_LINE_HEIGHT + 8;

    draw_text(x, cy + 5, "Bottom-Left:", THEME_LABEL_TEXT);
    draw_dropdown(x + 100, cy, 160, g_hot_corner_actions[g_state.hot_corner_bl]);
    cy += DESKTOP_LINE_HEIGHT + 8;

    draw_text(x, cy + 5, "Bottom-Right:", THEME_LABEL_TEXT);
    draw_dropdown(x + 100, cy, 160, g_hot_corner_actions[g_state.hot_corner_br]);

    return cy - y + DESKTOP_LINE_HEIGHT;
}

static int draw_icons_section(int32_t x, int32_t y, int32_t w) {
    int32_t cy = y;

    draw_text(x, cy, "Desktop Icons", 0x0066CC);
    cy += DESKTOP_LINE_HEIGHT + 8;

    draw_checkbox(x, cy, "Show desktop icons", g_state.show_icons);
    cy += DESKTOP_LINE_HEIGHT + 8;

    draw_text(x, cy + 5, "Icon size:", THEME_LABEL_TEXT);
    char size_str[16];
    snprintf(size_str, sizeof(size_str), "%d pixels", g_state.icon_size);
    draw_dropdown(x + 100, cy, 100, size_str);
    cy += DESKTOP_LINE_HEIGHT + 12;

    draw_checkbox(x, cy, "Auto-arrange icons", g_state.auto_arrange);
    cy += DESKTOP_LINE_HEIGHT + 8;

    draw_checkbox(x, cy, "Align icons to grid", g_state.grid_alignment);

    return cy - y + DESKTOP_LINE_HEIGHT;
}

// ============================================================================
// Public API - Framework Callbacks
// ============================================================================

static void desktop_panel_init_internal(settings_panel_t *panel) {
    desktop_panel_init();
}

static void desktop_panel_draw_internal(settings_panel_t *panel, int32_t x, int32_t y,
                                        int32_t w, int32_t h) {
    desktop_panel_draw(x, y, w, h);
}

static void desktop_panel_event_internal(settings_panel_t *panel, gui_event_t *event) {
    if (event->type == EVENT_MOUSE_UP) {
        desktop_panel_handle_click(panel->content_x, panel->content_y,
                                   panel->content_width, panel->content_height_visible,
                                   event->mouse_x, event->mouse_y);
    } else if (event->type == EVENT_MOUSE_MOVE) {
        desktop_panel_handle_mouse_move(panel->content_x, panel->content_y,
                                        panel->content_width, panel->content_height_visible,
                                        event->mouse_x, event->mouse_y);
    } else if (event->type == EVENT_MOUSE_SCROLL) {
        desktop_panel_handle_scroll(event->scroll_delta);
    }
}

static void desktop_panel_apply_internal(settings_panel_t *panel) {
    // Apply screensaver settings
    screensaver_set_type(g_state.screensaver_type);
    screensaver_set_timeout(g_state.screensaver_timeout * 60);
    screensaver_set_enabled(g_state.screensaver_timeout > 0);

    kprintf("[DesktopPanel] Settings applied\n");
}

static void desktop_panel_cleanup_internal(settings_panel_t *panel) {
    desktop_panel_cleanup();
}

// Panel definition
static settings_panel_def_t g_desktop_panel_def = {
    .name = "Desktop",
    .icon = "desktop",
    .category = SETTINGS_CAT_APPEARANCE,
    .priority = 20,
    .init = desktop_panel_init_internal,
    .draw = desktop_panel_draw_internal,
    .handle_event = desktop_panel_event_internal,
    .apply = desktop_panel_apply_internal,
    .cleanup = desktop_panel_cleanup_internal
};

// ============================================================================
// Public API - Direct Functions
// ============================================================================

void desktop_panel_init(void) {
    if (g_state.initialized) return;

    g_state.current_wallpaper = 0;
    g_state.position_mode = WP_POSITION_STRETCH;
    g_state.use_solid_color = false;
    g_state.solid_color = 0x2E5A88;
    g_state.use_gradient = false;
    g_state.gradient_color1 = 0x1E3A58;
    g_state.gradient_color2 = 0x4E7A98;
    g_state.gradient_direction = 1;

    g_state.screensaver_type = SCREENSAVER_STARFIELD;
    g_state.screensaver_timeout = 5;
    g_state.password_on_wake = false;

    g_state.hot_corner_tl = HOT_CORNER_NONE;
    g_state.hot_corner_tr = HOT_CORNER_NONE;
    g_state.hot_corner_bl = HOT_CORNER_SHOW_DESKTOP;
    g_state.hot_corner_br = HOT_CORNER_START_SS;

    g_state.show_icons = true;
    g_state.icon_size = 48;
    g_state.auto_arrange = true;
    g_state.grid_alignment = true;

    g_state.scroll_offset = 0;
    g_state.selected_section = 0;
    g_state.hover_wallpaper = -1;

    for (int i = 0; i < 32; i++) {
        g_state.thumbnails[i] = NULL;
        g_state.thumbnails_loaded[i] = false;
    }

    g_state.initialized = true;
    kprintf("[DesktopPanel] Initialized\n");
}

void desktop_panel_draw(int32_t panel_x, int32_t panel_y,
                        int32_t panel_width, int32_t panel_height) {
    if (!g_state.initialized) desktop_panel_init();

    int32_t cy = panel_y + DESKTOP_PANEL_PADDING;
    int32_t cx = panel_x + DESKTOP_PANEL_PADDING;
    int32_t cw = panel_width - DESKTOP_PANEL_PADDING * 2;

    // Section 1: Wallpaper
    draw_section_header(cx, cy, cw, "Wallpaper", g_state.selected_section == 0);
    cy += 26;
    if (g_state.selected_section == 0) {
        cy += draw_wallpaper_section(cx + 8, cy, cw - 16);
    }
    cy += DESKTOP_SECTION_SPACING;

    // Section 2: Screensaver
    draw_section_header(cx, cy, cw, "Screensaver", g_state.selected_section == 1);
    cy += 26;
    if (g_state.selected_section == 1) {
        cy += draw_screensaver_section(cx + 8, cy, cw - 16);
    }
    cy += DESKTOP_SECTION_SPACING;

    // Section 3: Hot Corners
    draw_section_header(cx, cy, cw, "Hot Corners", g_state.selected_section == 2);
    cy += 26;
    if (g_state.selected_section == 2) {
        cy += draw_hot_corners_section(cx + 8, cy, cw - 16);
    }
    cy += DESKTOP_SECTION_SPACING;

    // Section 4: Desktop Icons
    draw_section_header(cx, cy, cw, "Desktop Icons", g_state.selected_section == 3);
    cy += 26;
    if (g_state.selected_section == 3) {
        cy += draw_icons_section(cx + 8, cy, cw - 16);
    }

    g_state.max_scroll = (cy - panel_y > panel_height) ? (cy - panel_y - panel_height) : 0;
}

bool desktop_panel_handle_click(int32_t panel_x, int32_t panel_y,
                                int32_t panel_width, int32_t panel_height,
                                int32_t click_x, int32_t click_y) {
    int32_t local_x = click_x - panel_x;
    int32_t local_y = click_y - panel_y;

    int32_t cy = DESKTOP_PANEL_PADDING;
    int32_t cx = DESKTOP_PANEL_PADDING;
    int32_t cw = panel_width - DESKTOP_PANEL_PADDING * 2;

    // Check section header clicks
    // Section 0
    if (local_y >= cy && local_y < cy + 26) {
        g_state.selected_section = (g_state.selected_section == 0) ? -1 : 0;
        return true;
    }
    cy += 26 + (g_state.selected_section == 0 ? 300 : 0) + DESKTOP_SECTION_SPACING;

    // Section 1
    if (local_y >= cy && local_y < cy + 26) {
        g_state.selected_section = (g_state.selected_section == 1) ? -1 : 1;
        return true;
    }
    cy += 26 + (g_state.selected_section == 1 ? 120 : 0) + DESKTOP_SECTION_SPACING;

    // Section 2
    if (local_y >= cy && local_y < cy + 26) {
        g_state.selected_section = (g_state.selected_section == 2) ? -1 : 2;
        return true;
    }
    cy += 26 + (g_state.selected_section == 2 ? 220 : 0) + DESKTOP_SECTION_SPACING;

    // Section 3
    if (local_y >= cy && local_y < cy + 26) {
        g_state.selected_section = (g_state.selected_section == 3) ? -1 : 3;
        return true;
    }

    return false;
}

void desktop_panel_handle_mouse_move(int32_t panel_x, int32_t panel_y,
                                     int32_t panel_width, int32_t panel_height,
                                     int32_t mouse_x, int32_t mouse_y) {
    // Track hover state for wallpaper thumbnails
    g_state.hover_wallpaper = -1;
}

void desktop_panel_handle_scroll(int32_t delta) {
    g_state.scroll_offset -= delta * 20;
    if (g_state.scroll_offset < 0) g_state.scroll_offset = 0;
    if (g_state.scroll_offset > g_state.max_scroll) g_state.scroll_offset = g_state.max_scroll;
}

void desktop_panel_cleanup(void) {
    for (int i = 0; i < 32; i++) {
        if (g_state.thumbnails[i]) {
            kfree(g_state.thumbnails[i]);
            g_state.thumbnails[i] = NULL;
        }
    }
    g_state.initialized = false;
}

// State getters
int desktop_get_selected_wallpaper(void) { return g_state.current_wallpaper; }
int desktop_get_position_mode(void) { return g_state.position_mode; }
uint32_t desktop_get_solid_color(void) { return g_state.solid_color; }
bool desktop_is_solid_color_enabled(void) { return g_state.use_solid_color; }
int desktop_get_screensaver_type(void) { return g_state.screensaver_type; }
int desktop_get_screensaver_timeout(void) { return g_state.screensaver_timeout; }
int desktop_get_hot_corner_action(int corner) {
    switch (corner) {
        case 0: return g_state.hot_corner_tl;
        case 1: return g_state.hot_corner_tr;
        case 2: return g_state.hot_corner_bl;
        case 3: return g_state.hot_corner_br;
        default: return HOT_CORNER_NONE;
    }
}
bool desktop_get_show_icons(void) { return g_state.show_icons; }
int desktop_get_icon_size(void) { return g_state.icon_size; }

int desktop_panel_register(void) {
    return settings_register_panel(&g_desktop_panel_def);
}

const settings_panel_def_t *desktop_panel_get_def(void) {
    return &g_desktop_panel_def;
}
