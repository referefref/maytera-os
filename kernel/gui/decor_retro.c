// decor_retro.c - Retro UNIX (Motif/CDE) window decorations for MayteraOS GUI
// Features classic 3D beveled look with stippled inactive windows
#include "window_decor.h"
#include "window.h"
#include "themes.h"
#include "../types.h"
#include "../serial.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "icons.h"

// ============================================================================
// Retro UNIX Style Constants
// ============================================================================

// Classic Motif/CDE color palette
#define RETRO_BEVEL_LIGHT       0x00FFFFFF      // White highlight
#define RETRO_BEVEL_DARK        0x00404040      // Dark shadow
#define RETRO_BEVEL_MID         0x00808080      // Mid shadow
#define RETRO_BASE_GRAY         0x00C0C0C0      // Classic Windows gray

// Titlebar colors for active/inactive (Motif style uses blue for active)
#define RETRO_TITLEBAR_ACTIVE   0x00000080      // Navy blue
#define RETRO_TITLEBAR_INACTIVE 0x00808080      // Gray

// ============================================================================
// Forward Declarations
// ============================================================================

static void retro_get_metrics(decor_metrics_t *metrics);
static void retro_draw_button_impl(int type, int state, int32_t x, int32_t y,
                                    const theme_t *theme, bool active,
                                    const decor_metrics_t *metrics);

// ============================================================================
// Metrics for Retro Style
// ============================================================================

static void retro_get_metrics(decor_metrics_t *metrics) {
    metrics->titlebar_height = 24;          // Accommodate 16px font + padding
    metrics->border_width = 4;              // Thick 3D border
    metrics->button_size = 20;              // Square buttons (slightly larger)
    metrics->button_spacing = 2;
    metrics->button_margin = 2;
    metrics->corner_radius = 0;             // No rounded corners (square look)
    metrics->title_padding_left = 6;
    metrics->title_padding_top = 4;
}

// ============================================================================
// Draw Button Implementation (needed by titlebar)
// ============================================================================

static void retro_draw_button_impl(int type, int state, int32_t x, int32_t y,
                                    const theme_t *theme, bool active,
                                    const decor_metrics_t *metrics) {
    // Suppress unused parameter warnings
    (void)theme;
    (void)active;

    int32_t size = metrics->button_size;

    // Button base color
    uint32_t btn_bg = RETRO_BASE_GRAY;
    uint32_t btn_text = 0x00000000;  // Black

    // Adjust colors based on state
    switch (state) {
        case BUTTON_STATE_HOVER:
            btn_bg = decor_lighten_color(btn_bg, 20);
            break;
        case BUTTON_STATE_PRESSED:
            btn_bg = decor_darken_color(btn_bg, 20);
            break;
        case BUTTON_STATE_INACTIVE:
            btn_bg = RETRO_BEVEL_MID;
            btn_text = 0x00606060;
            break;
        default:
            break;
    }

    // Fill button background
    fb_fill_rect(x, y, size, size, btn_bg);

    // Draw bevel (raised for normal/hover, sunken for pressed)
    if (state == BUTTON_STATE_PRESSED) {
        decor_draw_bevel_sunken(x, y, size, size, RETRO_BEVEL_LIGHT, RETRO_BEVEL_DARK, 2);
    } else {
        decor_draw_bevel_raised(x, y, size, size, RETRO_BEVEL_LIGHT, RETRO_BEVEL_DARK, 2);
    }

    // Draw button symbol
    int32_t cx = x + size / 2;
    int32_t cy = y + size / 2;
    int offset = (state == BUTTON_STATE_PRESSED) ? 1 : 0;

    switch (type) {
        case BUTTON_TYPE_CLOSE:
            // Draw X symbol
            for (int i = -3; i <= 3; i++) {
                fb_put_pixel(cx + i + offset, cy + i + offset, btn_text);
                fb_put_pixel(cx + i + offset, cy - i + offset, btn_text);
                // Make it thicker
                fb_put_pixel(cx + i + offset + 1, cy + i + offset, btn_text);
                fb_put_pixel(cx + i + offset + 1, cy - i + offset, btn_text);
            }
            break;

        case BUTTON_TYPE_MINIMIZE:
            // Draw horizontal line (minus sign)
            for (int i = -4; i <= 4; i++) {
                fb_put_pixel(cx + i + offset, cy + 2 + offset, btn_text);
                fb_put_pixel(cx + i + offset, cy + 3 + offset, btn_text);
            }
            break;

        case BUTTON_TYPE_MAXIMIZE:
            // Draw square outline (window symbol)
            for (int i = -4; i <= 4; i++) {
                fb_put_pixel(cx + i + offset, cy - 4 + offset, btn_text);  // Top
                fb_put_pixel(cx + i + offset, cy + 4 + offset, btn_text);  // Bottom
                fb_put_pixel(cx - 4 + offset, cy + i + offset, btn_text);  // Left
                fb_put_pixel(cx + 4 + offset, cy + i + offset, btn_text);  // Right
            }
            // Top bar (thicker to indicate title)
            for (int i = -4; i <= 4; i++) {
                fb_put_pixel(cx + i + offset, cy - 3 + offset, btn_text);
            }
            break;
    }
}

// ============================================================================
// Draw Title Bar - Motif/CDE Style
// ============================================================================

// Legible title-text ink derived from the titlebar background: a warm off-white
// (beige) on dark titlebars, near-black on light ones. (replaces harsh pure white)
static uint32_t retro_title_ink(uint32_t bg) {
    uint8_t r = (bg >> 16) & 0xFF, g = (bg >> 8) & 0xFF, b = bg & 0xFF;
    uint32_t luma = (r * 77 + g * 150 + b * 29) >> 8;
    return (luma >= 140) ? 0x00232018 : 0x00EDE4D0;
}

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

// #136: kernel windows carry no icon id, so derive a titlebar icon from the
// window title the same way the taskbar maps app names to icons.
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

static void retro_draw_titlebar(window_t *win, const theme_t *theme, bool active) {
    decor_metrics_t metrics;
    retro_get_metrics(&metrics);

    int32_t x = win->bounds.x;
    int32_t y = win->bounds.y;
    int32_t w = win->bounds.width;
    int32_t tb_h = metrics.titlebar_height;
    int32_t border = metrics.border_width;

    // Calculate titlebar inner area (inside the outer border)
    int32_t tb_x = x + border;
    int32_t tb_y = y + border;
    int32_t tb_w = w - 2 * border;

    // Determine colors based on active state
    uint32_t tb_bg, tb_text;
    if (active) {
        // Use theme colors or fall back to classic Motif blue
        tb_bg = theme->titlebar_active ? theme->titlebar_active : RETRO_TITLEBAR_ACTIVE;
        // #140: in light themes the active titlebar would be near-white; use the
        // taskbar color instead so the window heading is cohesive, not glaring.
        {
            uint8_t rr = (tb_bg >> 16) & 0xFF, gg = (tb_bg >> 8) & 0xFF, bb = tb_bg & 0xFF;
            uint32_t lm = (rr * 77 + gg * 150 + bb * 29) >> 8;
            if (lm >= 200 && theme->taskbar_bg) tb_bg = theme->taskbar_bg;
        }
        tb_text = retro_title_ink(tb_bg);
    } else {
        // For inactive: use gray with stipple pattern
        tb_bg = theme->titlebar_inactive ? theme->titlebar_inactive : RETRO_TITLEBAR_INACTIVE;
        tb_text = decor_darken_color(retro_title_ink(tb_bg), 45);
    }

    // Fill titlebar background
    if (!active) {
        // Draw stippled pattern for inactive windows (classic Motif look)
        decor_draw_stipple(tb_x, tb_y, tb_w, tb_h, tb_bg, decor_darken_color(tb_bg, 30));
    } else {
        // Solid fill for active window
        fb_fill_rect(tb_x, tb_y, tb_w, tb_h, tb_bg);
    }

    // Draw inner bevel on titlebar (raised effect)
    uint32_t light = decor_lighten_color(tb_bg, 60);
    uint32_t dark = decor_darken_color(tb_bg, 60);
    decor_draw_bevel_raised(tb_x, tb_y, tb_w, tb_h, light, dark, 1);

    // Draw title text (centered vertically)
    int32_t text_y = tb_y + (tb_h - FONT_HEIGHT) / 2;  // Using FONT_HEIGHT from font.h
    int32_t text_x = tb_x + metrics.title_padding_left;

    // Account for buttons on the right (3 buttons)
    int32_t btn_area = 3 * (metrics.button_size + metrics.button_spacing) + metrics.button_margin;
    int32_t max_title_width = tb_w - metrics.title_padding_left - btn_area;

    // #136: app icon at the left of the titlebar (tinted to the title ink).
    {
        int32_t isz = 16;
        int32_t iy = tb_y + (tb_h - isz) / 2;
        icon_draw_scaled(decor_title_icon(win->title), text_x, iy, isz, tb_text);
        text_x += isz + 4;
        max_title_width -= (isz + 4);
        if (max_title_width < 0) max_title_width = 0;
    }

    // Draw title (with simple clipping)
    const char *title = win->title;
    int32_t curr_x = text_x;
    while (*title && curr_x < text_x + max_title_width - FONT_WIDTH) {
        // Draw character
        const uint8_t *glyph = font_get_glyph(*title);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(curr_x + col, text_y + row, tb_text);
                    }
                }
            }
        }
        curr_x += FONT_WIDTH;
        title++;
    }

    // Draw window control buttons
    int32_t btn_y = tb_y + (tb_h - metrics.button_size) / 2;

    // Minimize button (leftmost of the three)
    int32_t min_x = x + w - border - 3 * metrics.button_size - 2 * metrics.button_spacing - metrics.button_margin;
    retro_draw_button_impl(BUTTON_TYPE_MINIMIZE, active ? BUTTON_STATE_NORMAL : BUTTON_STATE_INACTIVE,
                           min_x, btn_y, theme, active, &metrics);

    // Maximize button (middle)
    int32_t max_x = x + w - border - 2 * metrics.button_size - metrics.button_spacing - metrics.button_margin;
    retro_draw_button_impl(BUTTON_TYPE_MAXIMIZE, active ? BUTTON_STATE_NORMAL : BUTTON_STATE_INACTIVE,
                           max_x, btn_y, theme, active, &metrics);

    // Close button (rightmost)
    int32_t close_x = x + w - border - metrics.button_size - metrics.button_margin;
    if (win->flags & WINDOW_FLAG_CLOSABLE) {
        retro_draw_button_impl(BUTTON_TYPE_CLOSE, active ? BUTTON_STATE_NORMAL : BUTTON_STATE_INACTIVE,
                               close_x, btn_y, theme, active, &metrics);
    }
}

// ============================================================================
// Draw Border - Motif/CDE Style 3D Frame
// ============================================================================

static void retro_draw_border(window_t *win, const theme_t *theme, bool active) {
    decor_metrics_t metrics;
    retro_get_metrics(&metrics);

    int32_t x = win->bounds.x;
    int32_t y = win->bounds.y;
    int32_t w = win->bounds.width;
    int32_t h = win->bounds.height;
    int32_t border = metrics.border_width;

    // Suppress unused parameter warning
    (void)active;

    // Base color for the frame
    uint32_t base = RETRO_BASE_GRAY;
    if (theme->window_border) {
        base = theme->window_border;
    }

    // Draw outer raised bevel (makes window pop out)
    decor_draw_bevel_raised(x, y, w, h, RETRO_BEVEL_LIGHT, RETRO_BEVEL_DARK, 2);

    // Draw the frame sides
    // Top frame
    fb_fill_rect(x + 2, y + 2, w - 4, border - 2, base);
    // Left frame
    fb_fill_rect(x + 2, y + 2, border - 2, h - 4, base);
    // Right frame
    fb_fill_rect(x + w - border, y + 2, border - 2, h - 4, base);
    // Bottom frame
    fb_fill_rect(x + 2, y + h - border, w - 4, border - 2, base);

    // Draw inner sunken bevel around the content area
    int32_t content_x = x + border;
    int32_t content_y = y + border + metrics.titlebar_height;
    int32_t content_w = w - 2 * border;
    int32_t content_h = h - 2 * border - metrics.titlebar_height;

    if (content_h > 0 && content_w > 0) {
        decor_draw_bevel_sunken(content_x - 1, content_y - 1, content_w + 2, content_h + 2,
                                 RETRO_BEVEL_LIGHT, RETRO_BEVEL_DARK, 1);
    }

    // Draw resize grips in corners (small triangular patterns)
    if (win->flags & WINDOW_FLAG_RESIZABLE) {
        // Bottom-right corner grip
        int32_t grip_x = x + w - border - 8;
        int32_t grip_y = y + h - border - 8;

        for (int i = 0; i < 3; i++) {
            int offset = i * 3;
            // Draw small raised bumps
            fb_put_pixel(grip_x + 6 - offset, grip_y + 6, RETRO_BEVEL_LIGHT);
            fb_put_pixel(grip_x + 7 - offset, grip_y + 6, RETRO_BEVEL_DARK);
            fb_put_pixel(grip_x + 6 - offset, grip_y + 7, RETRO_BEVEL_DARK);

            fb_put_pixel(grip_x + 6, grip_y + 6 - offset, RETRO_BEVEL_LIGHT);
            fb_put_pixel(grip_x + 7, grip_y + 6 - offset, RETRO_BEVEL_DARK);
            fb_put_pixel(grip_x + 6, grip_y + 7 - offset, RETRO_BEVEL_DARK);
        }
    }
}

// ============================================================================
// Public Button Drawing Function
// ============================================================================

static void retro_draw_button(int type, int state, int32_t x, int32_t y,
                               const theme_t *theme, bool active) {
    decor_metrics_t metrics;
    retro_get_metrics(&metrics);
    retro_draw_button_impl(type, state, x, y, theme, active, &metrics);
}

// ============================================================================
// Hit Test - Check if point is on a button
// ============================================================================

static bool retro_hit_test_button(window_t *win, int32_t x, int32_t y, int *button_type) {
    decor_metrics_t metrics;
    retro_get_metrics(&metrics);

    int32_t border = metrics.border_width;
    int32_t btn_size = metrics.button_size;
    int32_t tb_h = metrics.titlebar_height;

    // Check if in titlebar area
    int32_t tb_y = win->bounds.y + border;
    if (y < tb_y || y >= tb_y + tb_h) {
        return false;
    }

    int32_t btn_y = tb_y + (tb_h - btn_size) / 2;
    if (y < btn_y || y >= btn_y + btn_size) {
        return false;
    }

    // Check each button from right to left
    int32_t wx = win->bounds.x;
    int32_t ww = win->bounds.width;

    // Close button (rightmost)
    int32_t close_x = wx + ww - border - btn_size - metrics.button_margin;
    if (x >= close_x && x < close_x + btn_size && (win->flags & WINDOW_FLAG_CLOSABLE)) {
        *button_type = BUTTON_TYPE_CLOSE;
        return true;
    }

    // Maximize button
    int32_t max_x = wx + ww - border - 2 * btn_size - metrics.button_spacing - metrics.button_margin;
    if (x >= max_x && x < max_x + btn_size) {
        *button_type = BUTTON_TYPE_MAXIMIZE;
        return true;
    }

    // Minimize button
    int32_t min_x = wx + ww - border - 3 * btn_size - 2 * metrics.button_spacing - metrics.button_margin;
    if (x >= min_x && x < min_x + btn_size) {
        *button_type = BUTTON_TYPE_MINIMIZE;
        return true;
    }

    return false;
}

// ============================================================================
// Style Registration
// ============================================================================

static const decor_style_t retro_style = {
    .name = "Retro UNIX (Motif/CDE)",
    .draw_titlebar = retro_draw_titlebar,
    .draw_border = retro_draw_border,
    .draw_button = retro_draw_button,
    .get_metrics = retro_get_metrics,
    .hit_test_button = retro_hit_test_button,
};

void decor_retro_init(void) {
    decor_register_style(DECOR_STYLE_RETRO_UNIX, &retro_style);
    kprintf("[Decor] Retro UNIX style initialized\n");
}
