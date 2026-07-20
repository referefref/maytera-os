// decor_modern.c - Modern window decorations for MayteraOS GUI
// Includes macOS-style traffic lights and Windows 11-style flat design
#include "window_decor.h"
#include "window.h"
#include "themes.h"
#include "../types.h"
#include "../serial.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "icons.h"

// ============================================================================
// Case-insensitive substring match (decor-local, freestanding).
static int decor_ci_has(const char *h, const char *n) {
    if (!h || !n) return 0;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return 1;
    }
    return 0;
}
// #136: derive a titlebar icon from the window title (kernel windows carry none).
static icon_id_t decor_title_icon(const char *t) {
    if (decor_ci_has(t, "setting"))  return ICON_COG;
    if (decor_ci_has(t, "file"))     return ICON_FOLDER;
    if (decor_ci_has(t, "terminal") || decor_ci_has(t, "console")) return ICON_TERMINAL;
    if (decor_ci_has(t, "calc"))     return ICON_CALCULATOR;
    if (decor_ci_has(t, "edit") || decor_ci_has(t, "text"))  return ICON_HIGHLIGHT;
    if (decor_ci_has(t, "paint"))    return ICON_PAINT;
    if (decor_ci_has(t, "image") || decor_ci_has(t, "photo") || decor_ci_has(t, "viewer")) return ICON_IMAGE;
    if (decor_ci_has(t, "audio") || decor_ci_has(t, "music") || decor_ci_has(t, "media") || decor_ci_has(t, "player")) return ICON_MUSIC;
    if (decor_ci_has(t, "clock") || decor_ci_has(t, "world")) return ICON_CLOCK;
    if (decor_ci_has(t, "task"))     return ICON_TASK_MANAGER;
    if (decor_ci_has(t, "log"))      return ICON_LOG_VIEWER;
    if (decor_ci_has(t, "recycle") || decor_ci_has(t, "trash")) return ICON_TRASH;
    if (decor_ci_has(t, "doom"))     return ICON_GAME_DOOM;
    if (decor_ci_has(t, "solit") || decor_ci_has(t, "freecell") || decor_ci_has(t, "card") || decor_ci_has(t, "tut")) return ICON_GAME_SOLITAIRE;
    if (decor_ci_has(t, "tetris") || decor_ci_has(t, "chip") || decor_ci_has(t, "golf") || decor_ci_has(t, "jezz") || decor_ci_has(t, "rodent") || decor_ci_has(t, "tetra") || decor_ci_has(t, "pong") || decor_ci_has(t, "game")) return ICON_GAME;
    return ICON_WINDOW;
}

// macOS Style Constants
// ============================================================================

// Traffic light button colors
#define MACOS_BTN_CLOSE_NORMAL      0x00FF5F57      // Red
#define MACOS_BTN_CLOSE_HOVER       0x00FF3B30      // Brighter red
#define MACOS_BTN_MINIMIZE_NORMAL   0x00FFBD2E      // Yellow
#define MACOS_BTN_MINIMIZE_HOVER    0x00FFCC00      // Brighter yellow
#define MACOS_BTN_MAXIMIZE_NORMAL   0x0028C840      // Green
#define MACOS_BTN_MAXIMIZE_HOVER    0x0030D158      // Brighter green
#define MACOS_BTN_INACTIVE          0x00CCCCCC      // Gray for inactive

// macOS window colors
#define MACOS_TITLEBAR_ACTIVE       0x00E8E8E8      // Light gray
#define MACOS_TITLEBAR_INACTIVE     0x00F6F6F6      // Very light gray
#define MACOS_BORDER_COLOR          0x00B0B0B0      // Subtle border

// ============================================================================
// Windows 11 Style Constants
// ============================================================================

#define WIN11_TITLEBAR_ACTIVE       0x00FFFFFF      // White
#define WIN11_TITLEBAR_INACTIVE     0x00F3F3F3      // Light gray
#define WIN11_BTN_HOVER             0x00E5E5E5      // Button hover
#define WIN11_BTN_CLOSE_HOVER       0x00C42B1C      // Red close hover
#define WIN11_BORDER_COLOR          0x00CCCCCC      // Light border

// ============================================================================
// Forward Declarations
// ============================================================================

static void macos_get_metrics(decor_metrics_t *metrics);
static void macos_draw_button_impl(int type, int state, int32_t x, int32_t y,
                                    const theme_t *theme, bool active,
                                    const decor_metrics_t *metrics);

static void win11_get_metrics(decor_metrics_t *metrics);
static void win11_draw_button_impl(int type, int state, int32_t x, int32_t y,
                                    const theme_t *theme, bool active,
                                    const decor_metrics_t *metrics);

// ============================================================================
// macOS Style Implementation
// ============================================================================

static void macos_get_metrics(decor_metrics_t *metrics) {
    metrics->titlebar_height = 28;          // macOS standard (works with 16px font)
    metrics->border_width = 1;              // Thin border
    metrics->button_size = 12;              // Traffic light size
    metrics->button_spacing = 8;            // Spacing between buttons
    metrics->button_margin = 10;            // Left margin for buttons
    metrics->corner_radius = 10;            // Rounded corners
    metrics->title_padding_left = 0;        // Title is centered
    metrics->title_padding_top = 6;
}

static void macos_draw_button_impl(int type, int state, int32_t x, int32_t y,
                                    const theme_t *theme, bool active,
                                    const decor_metrics_t *metrics) {
    (void)theme;
    (void)metrics;

    int32_t radius = 6;  // Button radius
    int32_t cx = x + radius;
    int32_t cy = y + radius;

    // Determine button color based on type and state
    uint32_t color;

    if (!active || state == BUTTON_STATE_INACTIVE) {
        // All buttons gray when inactive
        color = MACOS_BTN_INACTIVE;
    } else {
        switch (type) {
            case BUTTON_TYPE_CLOSE:
                color = (state == BUTTON_STATE_HOVER || state == BUTTON_STATE_PRESSED)
                        ? MACOS_BTN_CLOSE_HOVER : MACOS_BTN_CLOSE_NORMAL;
                break;
            case BUTTON_TYPE_MINIMIZE:
                color = (state == BUTTON_STATE_HOVER || state == BUTTON_STATE_PRESSED)
                        ? MACOS_BTN_MINIMIZE_HOVER : MACOS_BTN_MINIMIZE_NORMAL;
                break;
            case BUTTON_TYPE_MAXIMIZE:
                color = (state == BUTTON_STATE_HOVER || state == BUTTON_STATE_PRESSED)
                        ? MACOS_BTN_MAXIMIZE_HOVER : MACOS_BTN_MAXIMIZE_NORMAL;
                break;
            default:
                color = MACOS_BTN_INACTIVE;
                break;
        }
    }

    // Draw filled circle
    decor_fill_circle(cx, cy, radius, color);

    // Draw darker border around the circle
    uint32_t border_color = decor_darken_color(color, 40);
    decor_draw_circle(cx, cy, radius, border_color);

    // Draw icons when hovering (macOS shows icons on hover)
    if (active && (state == BUTTON_STATE_HOVER || state == BUTTON_STATE_PRESSED)) {
        uint32_t icon_color = decor_darken_color(color, 100);

        switch (type) {
            case BUTTON_TYPE_CLOSE:
                // Draw X
                for (int i = -2; i <= 2; i++) {
                    fb_put_pixel(cx + i, cy + i, icon_color);
                    fb_put_pixel(cx + i, cy - i, icon_color);
                }
                break;

            case BUTTON_TYPE_MINIMIZE:
                // Draw minus
                for (int i = -3; i <= 3; i++) {
                    fb_put_pixel(cx + i, cy, icon_color);
                }
                break;

            case BUTTON_TYPE_MAXIMIZE:
                // Draw two small triangles (fullscreen icon)
                fb_put_pixel(cx - 2, cy - 2, icon_color);
                fb_put_pixel(cx - 1, cy - 2, icon_color);
                fb_put_pixel(cx - 2, cy - 1, icon_color);
                fb_put_pixel(cx + 2, cy + 2, icon_color);
                fb_put_pixel(cx + 1, cy + 2, icon_color);
                fb_put_pixel(cx + 2, cy + 1, icon_color);
                break;
        }
    }
}

static void macos_draw_titlebar(window_t *win, const theme_t *theme, bool active) {
    decor_metrics_t metrics;
    macos_get_metrics(&metrics);

    int32_t x = win->bounds.x;
    int32_t y = win->bounds.y;
    int32_t w = win->bounds.width;
    int32_t tb_h = metrics.titlebar_height;

    // Titlebar background color
    uint32_t tb_bg = active ? MACOS_TITLEBAR_ACTIVE : MACOS_TITLEBAR_INACTIVE;
    if (active && theme->titlebar_active) {
        tb_bg = theme->titlebar_active;
    } else if (!active && theme->titlebar_inactive) {
        tb_bg = theme->titlebar_inactive;
    }
    // #140: light-theme active titlebar -> taskbar color (not glaring white).
    if (active) {
        uint8_t rr = (tb_bg >> 16) & 0xFF, gg = (tb_bg >> 8) & 0xFF, bb = tb_bg & 0xFF;
        uint32_t lm = (rr * 77 + gg * 150 + bb * 29) >> 8;
        if (lm >= 200 && theme->taskbar_bg) tb_bg = theme->taskbar_bg;
    }

    // Draw titlebar with rounded top corners
    // For now, just draw a rectangle (rounded corners require more complex rendering)
    fb_fill_rect(x, y, w, tb_h, tb_bg);

    // Draw subtle bottom border
    fb_draw_line(x, y + tb_h - 1, x + w - 1, y + tb_h - 1, MACOS_BORDER_COLOR);

    // Draw traffic light buttons on the LEFT side (macOS style)
    int32_t btn_y = y + (tb_h - metrics.button_size) / 2;
    int32_t btn_x = x + metrics.button_margin;

    // Close button (leftmost)
    macos_draw_button_impl(BUTTON_TYPE_CLOSE, active ? BUTTON_STATE_NORMAL : BUTTON_STATE_INACTIVE,
                           btn_x, btn_y, theme, active, &metrics);
    btn_x += metrics.button_size + metrics.button_spacing;

    // Minimize button
    macos_draw_button_impl(BUTTON_TYPE_MINIMIZE, active ? BUTTON_STATE_NORMAL : BUTTON_STATE_INACTIVE,
                           btn_x, btn_y, theme, active, &metrics);
    btn_x += metrics.button_size + metrics.button_spacing;

    // Maximize button
    macos_draw_button_impl(BUTTON_TYPE_MAXIMIZE, active ? BUTTON_STATE_NORMAL : BUTTON_STATE_INACTIVE,
                           btn_x, btn_y, theme, active, &metrics);

    // Draw centered title
    uint32_t mt_r = (tb_bg >> 16) & 0xFF, mt_g = (tb_bg >> 8) & 0xFF, mt_b = tb_bg & 0xFF;
    uint32_t mt_luma = (mt_r * 77 + mt_g * 150 + mt_b * 29) >> 8;
    uint32_t modern_title_ink = (mt_luma >= 140) ? 0x00232018 : 0x00EDE4D0;
    uint32_t title_color = active ? modern_title_ink : decor_lighten_color(modern_title_ink, 50);

    // Calculate title position (centered)
    int title_len = 0;
    const char *t = win->title;
    while (*t++) title_len++;

    int32_t title_width = title_len * FONT_WIDTH;  // Using FONT_WIDTH from font.h
    int32_t isz = 16, igap = 4;
    int32_t group_w = isz + igap + title_width;
    int32_t group_x = x + (w - group_w) / 2;
    int32_t title_x = group_x + isz + igap;
    int32_t title_y = y + (tb_h - FONT_HEIGHT) / 2;
    icon_draw_scaled(decor_title_icon(win->title), group_x, y + (tb_h - isz) / 2, isz, title_color);

    // Draw title
    const char *title = win->title;
    int32_t curr_x = title_x;
    while (*title) {
        const uint8_t *glyph = font_get_glyph(*title);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(curr_x + col, title_y + row, title_color);
                    }
                }
            }
        }
        curr_x += FONT_WIDTH;
        title++;
    }
}

static void macos_draw_border(window_t *win, const theme_t *theme, bool active) {
    decor_metrics_t metrics;
    macos_get_metrics(&metrics);

    (void)theme;
    (void)active;

    int32_t x = win->bounds.x;
    int32_t y = win->bounds.y;
    int32_t w = win->bounds.width;
    int32_t h = win->bounds.height;

    // Draw thin border around the entire window
    fb_draw_rect(x, y, w, h, MACOS_BORDER_COLOR);

    // Draw window content background
    int32_t content_y = y + metrics.titlebar_height;
    int32_t content_h = h - metrics.titlebar_height;

    if (content_h > 0) {
        fb_fill_rect(x + 1, content_y, w - 2, content_h - 1, 0x00FFFFFF);
    }
}

static void macos_draw_button(int type, int state, int32_t x, int32_t y,
                               const theme_t *theme, bool active) {
    decor_metrics_t metrics;
    macos_get_metrics(&metrics);
    macos_draw_button_impl(type, state, x, y, theme, active, &metrics);
}

static bool macos_hit_test_button(window_t *win, int32_t x, int32_t y, int *button_type) {
    decor_metrics_t metrics;
    macos_get_metrics(&metrics);

    int32_t tb_h = metrics.titlebar_height;
    int32_t btn_size = metrics.button_size;

    // Check if in titlebar
    if (y < win->bounds.y || y >= win->bounds.y + tb_h) {
        return false;
    }

    int32_t btn_y = win->bounds.y + (tb_h - btn_size) / 2;
    if (y < btn_y || y >= btn_y + btn_size) {
        return false;
    }

    // Buttons are on the LEFT side
    int32_t base_x = win->bounds.x + metrics.button_margin;

    // Close button
    if (x >= base_x && x < base_x + btn_size) {
        *button_type = BUTTON_TYPE_CLOSE;
        return true;
    }
    base_x += btn_size + metrics.button_spacing;

    // Minimize button
    if (x >= base_x && x < base_x + btn_size) {
        *button_type = BUTTON_TYPE_MINIMIZE;
        return true;
    }
    base_x += btn_size + metrics.button_spacing;

    // Maximize button
    if (x >= base_x && x < base_x + btn_size) {
        *button_type = BUTTON_TYPE_MAXIMIZE;
        return true;
    }

    return false;
}

// ============================================================================
// Windows 11 Style Implementation
// ============================================================================

static void win11_get_metrics(decor_metrics_t *metrics) {
    metrics->titlebar_height = 32;          // Win11 standard (works with 16px font)
    metrics->border_width = 1;              // Very thin border
    metrics->button_size = 46;              // Wide buttons (46x32)
    metrics->button_spacing = 0;            // No spacing
    metrics->button_margin = 0;             // Buttons touch edge
    metrics->corner_radius = 8;             // Rounded corners
    metrics->title_padding_left = 12;
    metrics->title_padding_top = 8;
}

static void win11_draw_button_impl(int type, int state, int32_t x, int32_t y,
                                    const theme_t *theme, bool active,
                                    const decor_metrics_t *metrics) {
    (void)theme;
    (void)active;

    int32_t w = 46;   // Win11 button width
    int32_t h = 32;   // Win11 button height (same as titlebar)

    // Determine background color based on state and type
    uint32_t bg_color = 0;  // Transparent by default

    if (state == BUTTON_STATE_HOVER || state == BUTTON_STATE_PRESSED) {
        if (type == BUTTON_TYPE_CLOSE) {
            bg_color = WIN11_BTN_CLOSE_HOVER;
        } else {
            bg_color = WIN11_BTN_HOVER;
        }
        fb_fill_rect(x, y, w, h, bg_color);
    }

    // Icon color
    uint32_t icon_color;
    if (type == BUTTON_TYPE_CLOSE && (state == BUTTON_STATE_HOVER || state == BUTTON_STATE_PRESSED)) {
        icon_color = 0x00FFFFFF;  // White on red
    } else if (state == BUTTON_STATE_INACTIVE) {
        icon_color = 0x00A0A0A0;  // Gray when inactive
    } else {
        icon_color = 0x00000000;  // Black
    }

    // Calculate center
    int32_t cx = x + w / 2;
    int32_t cy = y + h / 2;
    (void)metrics;

    switch (type) {
        case BUTTON_TYPE_CLOSE:
            // Draw X (thin lines, Win11 style)
            for (int i = -4; i <= 4; i++) {
                fb_put_pixel(cx + i, cy + i, icon_color);
                fb_put_pixel(cx + i, cy - i, icon_color);
            }
            break;

        case BUTTON_TYPE_MINIMIZE:
            // Draw horizontal line (minus)
            for (int i = -5; i <= 5; i++) {
                fb_put_pixel(cx + i, cy, icon_color);
            }
            break;

        case BUTTON_TYPE_MAXIMIZE:
            // Draw square outline
            for (int i = -4; i <= 4; i++) {
                fb_put_pixel(cx + i, cy - 4, icon_color);
                fb_put_pixel(cx + i, cy + 4, icon_color);
                fb_put_pixel(cx - 4, cy + i, icon_color);
                fb_put_pixel(cx + 4, cy + i, icon_color);
            }
            break;
    }
}

static void win11_draw_titlebar(window_t *win, const theme_t *theme, bool active) {
    decor_metrics_t metrics;
    win11_get_metrics(&metrics);

    int32_t x = win->bounds.x;
    int32_t y = win->bounds.y;
    int32_t w = win->bounds.width;
    int32_t tb_h = metrics.titlebar_height;

    // Titlebar background
    uint32_t tb_bg = active ? WIN11_TITLEBAR_ACTIVE : WIN11_TITLEBAR_INACTIVE;
    if (active && theme->titlebar_active) {
        tb_bg = theme->titlebar_active;
    } else if (!active && theme->titlebar_inactive) {
        tb_bg = theme->titlebar_inactive;
    }
    // #140: light-theme active titlebar -> taskbar color (not glaring white).
    if (active) {
        uint8_t rr = (tb_bg >> 16) & 0xFF, gg = (tb_bg >> 8) & 0xFF, bb = tb_bg & 0xFF;
        uint32_t lm = (rr * 77 + gg * 150 + bb * 29) >> 8;
        if (lm >= 200 && theme->taskbar_bg) tb_bg = theme->taskbar_bg;
    }

    fb_fill_rect(x, y, w, tb_h, tb_bg);

    // Draw title (left aligned with icon space)
    uint32_t title_color = active ? 0x00000000 : 0x00808080;
    if (theme->titlebar_text) {
        title_color = active ? theme->titlebar_text : decor_lighten_color(theme->titlebar_text, 60);
    }

    int32_t title_x = x + metrics.title_padding_left;
    int32_t title_y = y + (tb_h - FONT_HEIGHT) / 2;
    {
        int32_t isz = 16;
        icon_draw_scaled(decor_title_icon(win->title), title_x, y + (tb_h - isz) / 2, isz, title_color);
        title_x += isz + 4;
    }

    // Draw title text
    const char *title = win->title;
    int32_t curr_x = title_x;
    int32_t max_x = x + w - 3 * 46 - 10;  // Leave space for buttons

    while (*title && curr_x < max_x) {
        const uint8_t *glyph = font_get_glyph(*title);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(curr_x + col, title_y + row, title_color);
                    }
                }
            }
        }
        curr_x += FONT_WIDTH;
        title++;
    }

    // Draw buttons on the RIGHT side (Win11 style)
    int32_t btn_x = x + w - 46;  // Start from right edge

    // Close button (rightmost)
    if (win->flags & WINDOW_FLAG_CLOSABLE) {
        win11_draw_button_impl(BUTTON_TYPE_CLOSE, active ? BUTTON_STATE_NORMAL : BUTTON_STATE_INACTIVE,
                               btn_x, y, theme, active, &metrics);
    }
    btn_x -= 46;

    // Maximize button
    win11_draw_button_impl(BUTTON_TYPE_MAXIMIZE, active ? BUTTON_STATE_NORMAL : BUTTON_STATE_INACTIVE,
                           btn_x, y, theme, active, &metrics);
    btn_x -= 46;

    // Minimize button
    win11_draw_button_impl(BUTTON_TYPE_MINIMIZE, active ? BUTTON_STATE_NORMAL : BUTTON_STATE_INACTIVE,
                           btn_x, y, theme, active, &metrics);
}

static void win11_draw_border(window_t *win, const theme_t *theme, bool active) {
    decor_metrics_t metrics;
    win11_get_metrics(&metrics);

    (void)theme;
    (void)active;

    int32_t x = win->bounds.x;
    int32_t y = win->bounds.y;
    int32_t w = win->bounds.width;
    int32_t h = win->bounds.height;

    // Very subtle border
    fb_draw_rect(x, y, w, h, WIN11_BORDER_COLOR);

    // Content area
    int32_t content_y = y + metrics.titlebar_height;
    int32_t content_h = h - metrics.titlebar_height - 1;

    if (content_h > 0) {
        fb_fill_rect(x + 1, content_y, w - 2, content_h, 0x00FFFFFF);
    }
}

static void win11_draw_button(int type, int state, int32_t x, int32_t y,
                               const theme_t *theme, bool active) {
    decor_metrics_t metrics;
    win11_get_metrics(&metrics);
    win11_draw_button_impl(type, state, x, y, theme, active, &metrics);
}

static bool win11_hit_test_button(window_t *win, int32_t x, int32_t y, int *button_type) {
    decor_metrics_t metrics;
    win11_get_metrics(&metrics);

    int32_t tb_h = metrics.titlebar_height;

    // Check if in titlebar
    if (y < win->bounds.y || y >= win->bounds.y + tb_h) {
        return false;
    }

    int32_t wx = win->bounds.x;
    int32_t ww = win->bounds.width;
    int32_t btn_w = 46;

    // Buttons are on the right
    // Close button
    int32_t close_x = wx + ww - btn_w;
    if (x >= close_x && x < close_x + btn_w && (win->flags & WINDOW_FLAG_CLOSABLE)) {
        *button_type = BUTTON_TYPE_CLOSE;
        return true;
    }

    // Maximize button
    int32_t max_x = wx + ww - 2 * btn_w;
    if (x >= max_x && x < max_x + btn_w) {
        *button_type = BUTTON_TYPE_MAXIMIZE;
        return true;
    }

    // Minimize button
    int32_t min_x = wx + ww - 3 * btn_w;
    if (x >= min_x && x < min_x + btn_w) {
        *button_type = BUTTON_TYPE_MINIMIZE;
        return true;
    }

    return false;
}

// ============================================================================
// Style Registration
// ============================================================================

static const decor_style_t macos_style = {
    .name = "Modern macOS",
    .draw_titlebar = macos_draw_titlebar,
    .draw_border = macos_draw_border,
    .draw_button = macos_draw_button,
    .get_metrics = macos_get_metrics,
    .hit_test_button = macos_hit_test_button,
};

static const decor_style_t win11_style = {
    .name = "Modern Windows",
    .draw_titlebar = win11_draw_titlebar,
    .draw_border = win11_draw_border,
    .draw_button = win11_draw_button,
    .get_metrics = win11_get_metrics,
    .hit_test_button = win11_hit_test_button,
};

void decor_modern_init(void) {
    decor_register_style(DECOR_STYLE_MODERN_MACOS, &macos_style);
    decor_register_style(DECOR_STYLE_MODERN_WINDOWS, &win11_style);
    kprintf("[Decor] Modern macOS style initialized\n");
    kprintf("[Decor] Modern Windows style initialized\n");
}
