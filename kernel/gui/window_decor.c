// window_decor.c - Window decoration rendering implementation for MayteraOS GUI
#include "window_decor.h"
#include "window.h"
#include "themes.h"
#include "../types.h"
#include "../serial.h"
#include "../video/framebuffer.h"
#include "../video/font.h"

// ============================================================================
// External style declarations (defined in decor_retro.c and decor_modern.c)
// ============================================================================

extern void decor_retro_init(void);
extern void decor_modern_init(void);

// ============================================================================
// Module State
// ============================================================================

static struct {
    int current_style;
    decor_style_t styles[DECOR_STYLE_COUNT];
    bool initialized;
} decor_state;

// ============================================================================
// Helper Drawing Functions Implementation
// ============================================================================

// Blend two colors with alpha (0-255, 0=c1, 255=c2)
uint32_t decor_blend_colors(uint32_t c1, uint32_t c2, int alpha) {
    if (alpha <= 0) return c1;
    if (alpha >= 255) return c2;

    uint32_t r1 = (c1 >> 16) & 0xFF;
    uint32_t g1 = (c1 >> 8) & 0xFF;
    uint32_t b1 = c1 & 0xFF;

    uint32_t r2 = (c2 >> 16) & 0xFF;
    uint32_t g2 = (c2 >> 8) & 0xFF;
    uint32_t b2 = c2 & 0xFF;

    uint32_t r = (r1 * (255 - alpha) + r2 * alpha) / 255;
    uint32_t g = (g1 * (255 - alpha) + g2 * alpha) / 255;
    uint32_t b = (b1 * (255 - alpha) + b2 * alpha) / 255;

    return (r << 16) | (g << 8) | b;
}

// Lighten a color by amount (0-255)
uint32_t decor_lighten_color(uint32_t color, int amount) {
    uint32_t r = (color >> 16) & 0xFF;
    uint32_t g = (color >> 8) & 0xFF;
    uint32_t b = color & 0xFF;

    r = (r + amount > 255) ? 255 : r + amount;
    g = (g + amount > 255) ? 255 : g + amount;
    b = (b + amount > 255) ? 255 : b + amount;

    return (r << 16) | (g << 8) | b;
}

// Darken a color by amount (0-255)
uint32_t decor_darken_color(uint32_t color, int amount) {
    int32_t r = (color >> 16) & 0xFF;
    int32_t g = (color >> 8) & 0xFF;
    int32_t b = color & 0xFF;

    r = (r - amount < 0) ? 0 : r - amount;
    g = (g - amount < 0) ? 0 : g - amount;
    b = (b - amount < 0) ? 0 : b - amount;

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Draw a 3D raised bevel (light top-left, dark bottom-right)
void decor_draw_bevel_raised(int32_t x, int32_t y, int32_t w, int32_t h,
                              uint32_t light, uint32_t dark, int32_t thickness) {
    for (int32_t t = 0; t < thickness; t++) {
        // Top edge (light)
        fb_draw_line(x + t, y + t, x + w - 1 - t, y + t, light);
        // Left edge (light)
        fb_draw_line(x + t, y + t, x + t, y + h - 1 - t, light);
        // Bottom edge (dark)
        fb_draw_line(x + t, y + h - 1 - t, x + w - 1 - t, y + h - 1 - t, dark);
        // Right edge (dark)
        fb_draw_line(x + w - 1 - t, y + t, x + w - 1 - t, y + h - 1 - t, dark);
    }
}

// Draw a 3D sunken bevel (dark top-left, light bottom-right)
void decor_draw_bevel_sunken(int32_t x, int32_t y, int32_t w, int32_t h,
                              uint32_t light, uint32_t dark, int32_t thickness) {
    for (int32_t t = 0; t < thickness; t++) {
        // Top edge (dark)
        fb_draw_line(x + t, y + t, x + w - 1 - t, y + t, dark);
        // Left edge (dark)
        fb_draw_line(x + t, y + t, x + t, y + h - 1 - t, dark);
        // Bottom edge (light)
        fb_draw_line(x + t, y + h - 1 - t, x + w - 1 - t, y + h - 1 - t, light);
        // Right edge (light)
        fb_draw_line(x + w - 1 - t, y + t, x + w - 1 - t, y + h - 1 - t, light);
    }
}

// Draw circle outline using midpoint circle algorithm
void decor_draw_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color) {
    int32_t x = radius;
    int32_t y = 0;
    int32_t p = 1 - radius;

    while (x >= y) {
        fb_put_pixel(cx + x, cy + y, color);
        fb_put_pixel(cx - x, cy + y, color);
        fb_put_pixel(cx + x, cy - y, color);
        fb_put_pixel(cx - x, cy - y, color);
        fb_put_pixel(cx + y, cy + x, color);
        fb_put_pixel(cx - y, cy + x, color);
        fb_put_pixel(cx + y, cy - x, color);
        fb_put_pixel(cx - y, cy - x, color);

        y++;
        if (p <= 0) {
            p = p + 2 * y + 1;
        } else {
            x--;
            p = p + 2 * y - 2 * x + 1;
        }
    }
}

// Fill a circle
void decor_fill_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color) {
    for (int32_t y = -radius; y <= radius; y++) {
        for (int32_t x = -radius; x <= radius; x++) {
            if (x * x + y * y <= radius * radius) {
                fb_put_pixel(cx + x, cy + y, color);
            }
        }
    }
}

// Draw rounded rectangle outline
void decor_draw_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                              int32_t radius, uint32_t color) {
    if (radius <= 0) {
        fb_draw_rect(x, y, w, h, color);
        return;
    }

    // Clamp radius to half the smallest dimension
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    // Draw straight edges
    fb_draw_line(x + radius, y, x + w - 1 - radius, y, color);              // Top
    fb_draw_line(x + radius, y + h - 1, x + w - 1 - radius, y + h - 1, color); // Bottom
    fb_draw_line(x, y + radius, x, y + h - 1 - radius, color);              // Left
    fb_draw_line(x + w - 1, y + radius, x + w - 1, y + h - 1 - radius, color); // Right

    // Draw rounded corners using circle algorithm
    int32_t px = radius;
    int32_t py = 0;
    int32_t p = 1 - radius;

    while (px >= py) {
        // Top-left corner
        fb_put_pixel(x + radius - px, y + radius - py, color);
        fb_put_pixel(x + radius - py, y + radius - px, color);

        // Top-right corner
        fb_put_pixel(x + w - 1 - radius + px, y + radius - py, color);
        fb_put_pixel(x + w - 1 - radius + py, y + radius - px, color);

        // Bottom-left corner
        fb_put_pixel(x + radius - px, y + h - 1 - radius + py, color);
        fb_put_pixel(x + radius - py, y + h - 1 - radius + px, color);

        // Bottom-right corner
        fb_put_pixel(x + w - 1 - radius + px, y + h - 1 - radius + py, color);
        fb_put_pixel(x + w - 1 - radius + py, y + h - 1 - radius + px, color);

        py++;
        if (p <= 0) {
            p = p + 2 * py + 1;
        } else {
            px--;
            p = p + 2 * py - 2 * px + 1;
        }
    }
}

// Fill rounded rectangle
void decor_fill_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                              int32_t radius, uint32_t color) {
    if (radius <= 0) {
        fb_fill_rect(x, y, w, h, color);
        return;
    }

    // Clamp radius
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    // Fill main rectangles
    fb_fill_rect(x + radius, y, w - 2 * radius, h, color);           // Center vertical
    fb_fill_rect(x, y + radius, radius, h - 2 * radius, color);      // Left strip
    fb_fill_rect(x + w - radius, y + radius, radius, h - 2 * radius, color); // Right strip

    // Fill corners
    for (int32_t dy = 0; dy < radius; dy++) {
        for (int32_t dx = 0; dx < radius; dx++) {
            int32_t dist_sq = (radius - dx) * (radius - dx) + (radius - dy) * (radius - dy);
            if (dist_sq <= radius * radius) {
                // Top-left
                fb_put_pixel(x + dx, y + dy, color);
                // Top-right
                fb_put_pixel(x + w - 1 - dx, y + dy, color);
                // Bottom-left
                fb_put_pixel(x + dx, y + h - 1 - dy, color);
                // Bottom-right
                fb_put_pixel(x + w - 1 - dx, y + h - 1 - dy, color);
            }
        }
    }
}

// ============================================================================
// #745 ANTIALIASED rounded rectangles. INTEGER FIXED POINT ONLY, no float.
//
// decor_fill_rounded_rect()/decor_draw_rounded_rect() above are 100%-or-0%
// integer boundary tests: a corner pixel is either painted in full or not
// painted at all. That is the stairstep a user reported on the kernel login
// gate ("the rounded corners of the text input box look pixelated and not
// antialiased"). These variants sit ALONGSIDE the originals rather than
// replacing them, so every existing caller keeps its current behaviour and can
// migrate independently; this is the shared primitive improved in place, not a
// private copy forked into one subsystem.
//
// WHY NO FLOAT: the kernel is built -mno-sse -mno-sse2 (soft float), so a float
// coverage path would put libgcc soft-float calls in a per-pixel drawing loop.
// Coverage is computed in 1/16 px fixed point with a Newton's-method integer
// sqrt and composited through fb_get_pixel + decor_blend_colors, which is
// exactly how ttf_draw_char() already antialiases text onto this same
// photographic backdrop a few hundred lines away in this binary.
// ============================================================================

// Sub-pixel units per pixel. 16 gives the 1px falloff band 16 discrete steps,
// which is smooth at every radius this tree draws (6 to 21px) with no banding.
#define DECOR_AA_SUB 16

// Newton's-method integer square root, same shape as the userland compositor's
// dr_isqrt() but with int64 headroom so the DECOR_AA_SUB scaling cannot
// overflow for any radius a caller might pass.
static int32_t decor_isqrt(int64_t n) {
    if (n <= 0) return 0;
    int64_t x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return (int32_t)x;
}

// Coverage 0..255 for one pixel of a corner box. dx,dy are the pixel's offset
// from the corner CENTRE in whole pixels: the identical inputs the hard-edge
// test in decor_fill_rounded_rect() uses, so an AA and a non-AA rounded rect of
// the same radius describe the same circle and agree at the seam. The ramp is
// 1px wide and centred on the exact arc: fully inside 0.5px in, fully outside
// 0.5px out.
static uint8_t decor_corner_coverage(int32_t dx, int32_t dy, int32_t radius) {
    int64_t sdx = (int64_t)dx * DECOR_AA_SUB;
    int64_t sdy = (int64_t)dy * DECOR_AA_SUB;
    int32_t dist = decor_isqrt(sdx * sdx + sdy * sdy);   // distance, 1/16 px
    int32_t d    = dist - radius * DECOR_AA_SUB;         // signed dist to arc
    int32_t cov  = (DECOR_AA_SUB / 2) - d;               // clamp(0.5 - d, 0, 1)
    if (cov <= 0) return 0;
    if (cov >= DECOR_AA_SUB) return 255;
    return (uint8_t)(cov * 255 / DECOR_AA_SUB);
}

// Coverage 0..255 for one pixel of a 1px-wide STROKE whose outer edge sits on
// the same arc the fill's outer edge does, so a stroked and a filled rounded
// rect of the same radius line up. Triangular profile of total width 2px and
// total ink 1px, centred on radius-0.5.
static uint8_t decor_ring_coverage(int32_t dx, int32_t dy, int32_t radius) {
    int64_t sdx = (int64_t)dx * DECOR_AA_SUB;
    int64_t sdy = (int64_t)dy * DECOR_AA_SUB;
    int32_t dist = decor_isqrt(sdx * sdx + sdy * sdy);
    int32_t rc   = radius * DECOR_AA_SUB - (DECOR_AA_SUB / 2);
    int32_t off  = dist - rc;
    if (off < 0) off = -off;
    int32_t cov = DECOR_AA_SUB - off;
    if (cov <= 0) return 0;
    if (cov >= DECOR_AA_SUB) return 255;
    return (uint8_t)(cov * 255 / DECOR_AA_SUB);
}

// Composite `color` at cov/255 over what is already in the framebuffer. The
// read-blend-write triplet is the whole point: these controls float directly on
// a wallpaper with no chrome box behind them, so a fractional edge pixel has to
// be mixed with the photograph, not with a guessed background colour.
static void decor_blend_pixel(int32_t x, int32_t y, uint32_t color, uint8_t cov) {
    if (cov == 0 || x < 0 || y < 0) return;
    if (cov >= 255) { fb_put_pixel((uint32_t)x, (uint32_t)y, color); return; }
    uint32_t bg = fb_get_pixel((uint32_t)x, (uint32_t)y);
    fb_put_pixel((uint32_t)x, (uint32_t)y, decor_blend_colors(bg, color, cov));
}

void decor_fill_rounded_rect_aa(int32_t x, int32_t y, int32_t w, int32_t h,
                                int32_t radius, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius <= 0) { fb_fill_rect(x, y, w, h, color); return; }

    // Interior and straight edges carry no coverage question: flat fills, the
    // same three the non-AA version does. Only the four radius x radius corner
    // boxes route through the per-pixel path.
    fb_fill_rect(x + radius, y, w - 2 * radius, h, color);
    fb_fill_rect(x, y + radius, radius, h - 2 * radius, color);
    fb_fill_rect(x + w - radius, y + radius, radius, h - 2 * radius, color);

    for (int32_t dy = 0; dy < radius; dy++) {
        for (int32_t dx = 0; dx < radius; dx++) {
            uint8_t cov = decor_corner_coverage(radius - dx, radius - dy, radius);
            if (cov == 0) continue;
            decor_blend_pixel(x + dx,         y + dy,         color, cov);
            decor_blend_pixel(x + w - 1 - dx, y + dy,         color, cov);
            decor_blend_pixel(x + dx,         y + h - 1 - dy, color, cov);
            decor_blend_pixel(x + w - 1 - dx, y + h - 1 - dy, color, cov);
        }
    }
}

void decor_draw_rounded_rect_aa(int32_t x, int32_t y, int32_t w, int32_t h,
                                int32_t radius, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius <= 0) { fb_draw_rect(x, y, w, h, color); return; }

    // Straight edges stay byte-identical to decor_draw_rounded_rect(): an
    // axis-aligned 1px line has no antialiasing question.
    fb_draw_line(x + radius, y, x + w - 1 - radius, y, color);
    fb_draw_line(x + radius, y + h - 1, x + w - 1 - radius, y + h - 1, color);
    fb_draw_line(x, y + radius, x, y + h - 1 - radius, color);
    fb_draw_line(x + w - 1, y + radius, x + w - 1, y + h - 1 - radius, color);

    for (int32_t dy = 0; dy < radius; dy++) {
        for (int32_t dx = 0; dx < radius; dx++) {
            uint8_t cov = decor_ring_coverage(radius - dx, radius - dy, radius);
            if (cov == 0) continue;
            decor_blend_pixel(x + dx,         y + dy,         color, cov);
            decor_blend_pixel(x + w - 1 - dx, y + dy,         color, cov);
            decor_blend_pixel(x + dx,         y + h - 1 - dy, color, cov);
            decor_blend_pixel(x + w - 1 - dx, y + h - 1 - dy, color, cov);
        }
    }
}

// Draw horizontal gradient
void decor_draw_gradient_h(int32_t x, int32_t y, int32_t w, int32_t h,
                           uint32_t color1, uint32_t color2) {
    for (int32_t dx = 0; dx < w; dx++) {
        int alpha = (dx * 255) / (w - 1);
        uint32_t color = decor_blend_colors(color1, color2, alpha);
        for (int32_t dy = 0; dy < h; dy++) {
            fb_put_pixel(x + dx, y + dy, color);
        }
    }
}

// Draw vertical gradient
void decor_draw_gradient_v(int32_t x, int32_t y, int32_t w, int32_t h,
                           uint32_t color1, uint32_t color2) {
    for (int32_t dy = 0; dy < h; dy++) {
        int alpha = (dy * 255) / (h - 1);
        uint32_t color = decor_blend_colors(color1, color2, alpha);
        for (int32_t dx = 0; dx < w; dx++) {
            fb_put_pixel(x + dx, y + dy, color);
        }
    }
}

// Draw stippled/dithered pattern (for inactive windows in retro style)
void decor_draw_stipple(int32_t x, int32_t y, int32_t w, int32_t h,
                        uint32_t color1, uint32_t color2) {
    for (int32_t dy = 0; dy < h; dy++) {
        for (int32_t dx = 0; dx < w; dx++) {
            uint32_t color = ((dx + dy) % 2 == 0) ? color1 : color2;
            fb_put_pixel(x + dx, y + dy, color);
        }
    }
}

// ============================================================================
// Style Management
// ============================================================================

void decor_register_style(int style_id, const decor_style_t *style) {
    if (style_id >= 0 && style_id < DECOR_STYLE_COUNT && style) {
        decor_state.styles[style_id] = *style;
        kprintf("[Decor] Registered style %d: %s\n", style_id, style->name);
    }
}

const decor_style_t *decor_get_style_by_id(int style_id) {
    if (style_id >= 0 && style_id < DECOR_STYLE_COUNT) {
        return &decor_state.styles[style_id];
    }
    return NULL;
}

// ============================================================================
// Public API Implementation
// ============================================================================

void decor_init(void) {
    if (decor_state.initialized) return;

    // Clear state
    decor_state.current_style = DECOR_STYLE_RETRO_UNIX;
    for (int i = 0; i < DECOR_STYLE_COUNT; i++) {
        decor_state.styles[i].name = NULL;
        decor_state.styles[i].draw_titlebar = NULL;
        decor_state.styles[i].draw_border = NULL;
        decor_state.styles[i].draw_button = NULL;
        decor_state.styles[i].get_metrics = NULL;
        decor_state.styles[i].hit_test_button = NULL;
    }

    // Initialize built-in styles
    decor_retro_init();
    decor_modern_init();

    decor_state.initialized = true;
    kprintf("[Decor] Window decoration system initialized\n");
}

int decor_get_style(void) {
    return decor_state.current_style;
}

void decor_set_style(int style_id) {
    if (style_id >= 0 && style_id < DECOR_STYLE_COUNT) {
        if (decor_state.styles[style_id].name != NULL) {
            decor_state.current_style = style_id;
            kprintf("[Decor] Style changed to: %s\n", decor_state.styles[style_id].name);
        } else {
            kprintf("[Decor] Error: Style %d not registered\n", style_id);
        }
    }
}

const char *decor_get_style_name(int style_id) {
    if (style_id >= 0 && style_id < DECOR_STYLE_COUNT && decor_state.styles[style_id].name) {
        return decor_state.styles[style_id].name;
    }
    return "Unknown";
}

int decor_get_style_count(void) {
    return DECOR_STYLE_COUNT;
}

void decor_get_current_metrics(decor_metrics_t *metrics) {
    if (!metrics) return;

    const decor_style_t *style = &decor_state.styles[decor_state.current_style];
    if (style->get_metrics) {
        style->get_metrics(metrics);
    } else {
        // Default metrics
        metrics->titlebar_height = TITLEBAR_HEIGHT;
        metrics->border_width = BORDER_WIDTH;
        metrics->button_size = CLOSE_BUTTON_SIZE;
        metrics->button_spacing = TITLEBAR_BUTTON_SPACING;
        metrics->button_margin = 2;
        metrics->corner_radius = 0;
        metrics->title_padding_left = 4;
        metrics->title_padding_top = 2;
    }
}

void decor_draw_titlebar(window_t *win, const theme_t *theme) {
    if (!win || !theme) return;

    const decor_style_t *style = &decor_state.styles[decor_state.current_style];
    bool active = (win->flags & WINDOW_FLAG_FOCUSED) != 0;

    if (style->draw_titlebar) {
        style->draw_titlebar(win, theme, active);
    }
}

void decor_draw_border(window_t *win, const theme_t *theme) {
    if (!win || !theme) return;

    const decor_style_t *style = &decor_state.styles[decor_state.current_style];
    bool active = (win->flags & WINDOW_FLAG_FOCUSED) != 0;

    if (style->draw_border) {
        style->draw_border(win, theme, active);
    }
}

void decor_draw_button(int type, int state, int32_t x, int32_t y, const theme_t *theme) {
    if (!theme) return;

    const decor_style_t *style = &decor_state.styles[decor_state.current_style];
    bool active = (state != BUTTON_STATE_INACTIVE);

    if (style->draw_button) {
        style->draw_button(type, state, x, y, theme, active);
    }
}

void decor_draw_window_frame(window_t *win, const theme_t *theme) {
    if (!win || !theme) return;

    // Draw border first (it may extend under the titlebar)
    decor_draw_border(win, theme);

    // Draw titlebar on top
    decor_draw_titlebar(win, theme);
}

bool decor_hit_test_button(window_t *win, int32_t x, int32_t y, int *button_type) {
    if (!win || !button_type) return false;

    const decor_style_t *style = &decor_state.styles[decor_state.current_style];

    if (style->hit_test_button) {
        return style->hit_test_button(win, x, y, button_type);
    }

    return false;
}

void decor_get_button_rect(window_t *win, int button_type, rect_t *rect) {
    if (!win || !rect) return;

    decor_metrics_t metrics;
    decor_get_current_metrics(&metrics);

    int32_t btn_size = metrics.button_size;
    int32_t btn_y = win->bounds.y + metrics.button_margin;

    // Buttons are arranged right-to-left: close, maximize, minimize
    int btn_index;
    switch (button_type) {
        case BUTTON_TYPE_CLOSE:    btn_index = 0; break;
        case BUTTON_TYPE_MAXIMIZE: btn_index = 1; break;
        case BUTTON_TYPE_MINIMIZE: btn_index = 2; break;
        default: return;
    }

    int32_t btn_x = win->bounds.x + win->bounds.width - metrics.button_margin
                    - (btn_index + 1) * btn_size - btn_index * metrics.button_spacing;

    rect->x = btn_x;
    rect->y = btn_y;
    rect->width = btn_size;
    rect->height = btn_size;
}
