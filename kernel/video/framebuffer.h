// framebuffer.h - Framebuffer driver
#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include "../types.h"
#include "../boot_info.h"

// Color macros (32-bit BGRA)
#define FB_COLOR(r, g, b) (((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

// Common colors
#define FB_BLACK        FB_COLOR(0, 0, 0)
#define FB_WHITE        FB_COLOR(255, 255, 255)
#define FB_RED          FB_COLOR(255, 0, 0)
#define FB_GREEN        FB_COLOR(0, 255, 0)
#define FB_BLUE         FB_COLOR(0, 0, 255)
#define FB_YELLOW       FB_COLOR(255, 255, 0)
#define FB_CYAN         FB_COLOR(0, 255, 255)
#define FB_MAGENTA      FB_COLOR(255, 0, 255)
#define FB_GRAY         FB_COLOR(128, 128, 128)
#define FB_DARK_GRAY    FB_COLOR(64, 64, 64)
#define FB_LIGHT_GRAY   FB_COLOR(192, 192, 192)

// Initialize framebuffer from boot info
void fb_init(framebuffer_info_t *info);

// Get framebuffer dimensions
uint32_t fb_get_width(void);
uint32_t fb_get_height(void);
uint32_t fb_get_pitch(void);
uint32_t *fb_get_back_buffer(void);
uint32_t fb_get_bpp(void);

// Write a horizontal run of pixels at (x,y) in one memcpy (row-oriented blit).
void fb_put_row(uint32_t x, uint32_t y, uint32_t count, const uint32_t *pixels);

// Basic operations
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t fb_get_pixel(uint32_t x, uint32_t y);
void fb_clear(uint32_t color);

// Optimized operations
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color);
void fb_scroll(uint32_t lines, uint32_t bg_color);

// Bitmap operations
void fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *data);

// Alpha blending operations
void fb_blend_pixel(uint32_t x, uint32_t y, uint32_t color, uint8_t alpha);
void fb_fill_rect_alpha(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color, uint8_t alpha);

// Double buffering (optional)
void fb_swap_buffers(void);
void fb_swap_dirty_rects(const void *dirty_rects, uint32_t count, bool full_redraw);

// Control double buffering (for boot splash to draw directly to screen)
void fb_set_direct_mode(bool direct);  // true = draw to front buffer directly

#endif // FRAMEBUFFER_H
