// graphics.h - Graphics primitives
#ifndef GRAPHICS_H
#define GRAPHICS_H

#include "../types.h"
#include "framebuffer.h"

// Block-mosaic a rectangular region of the active draw buffer in place: sample
// one pixel per BLOCKxBLOCK cell and fill the cell with it. A cheap, integer,
// SSE-free "frosted glass" approximation of a blur (see the login/lock screens,
// #567). Operates through fb_get_pixel/fb_put_pixel so it is correct in both
// double- and single-buffered modes. Intended for one-shot backdrop prep, not
// per-frame work.
void gfx_mosaic_region(int32_t x, int32_t y, int32_t w, int32_t h, int32_t block);

// Draw a filled circle
void gfx_fill_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color);

// Draw a circle outline
void gfx_draw_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color);

// Draw a filled triangle
void gfx_fill_triangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                       int32_t x3, int32_t y3, uint32_t color);

// Draw gradient rectangle
void gfx_gradient_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint32_t color1, uint32_t color2, int horizontal);

// Blend two colors (alpha 0-255)
uint32_t gfx_blend(uint32_t color1, uint32_t color2, uint8_t alpha);

// Draw a simple test pattern
void gfx_test_pattern(void);

// #569: boot-splash framebuffer ownership. The splash-drawing calls below
// (progress/status/log/spinner/refresh) only touch the framebuffer while the
// splash OWNS the display. login_init()/desktop_run() release it so late
// background logging (e.g. the periodic xHCI rescan worker) can never repaint
// the boot log over a live UI; an explicit splash call re-acquires it.
void gfx_boot_acquire_display(void);
void gfx_boot_release_display(void);
bool gfx_boot_owns_display(void);

// Boot splash screen
void gfx_boot_splash(void);
void gfx_boot_progress(int percent);
void gfx_boot_status(const char *status);

// Boot log (scrolling dmesg-style output below loading bar)
void gfx_boot_log(const char *message);
// #610: rewrite the LAST boot-log line in place (live progress readouts).
void gfx_boot_log_replace(const char *message);
void gfx_boot_log_clear(void);

// Draw boot image (centered, scaled) - uses BOOT.BMP if loaded, gradient otherwise
void gfx_draw_boot_image(void);

// Load boot image from disk (call after filesystem is mounted)
// Returns 0 on success, -1 on failure
int gfx_load_boot_image_from_disk(void);

// Check if boot image was loaded from disk
bool gfx_has_disk_boot_image(void);

// Boot spinner (shown before filesystem is mounted)
// Shows spinning animation with BOOT.BMP background (or gradient fallback)
void gfx_boot_spinner(int frame);

// Simple boot screen (before filesystem)
// Uses BOOT.BMP if loaded, gradient background otherwise
void gfx_boot_simple(void);

// Refresh boot screen after BOOT.BMP is loaded
// Call this after gfx_load_boot_image_from_disk() succeeds to update display
void gfx_boot_refresh(void);

#endif // GRAPHICS_H
