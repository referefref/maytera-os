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

// #745 (local 102): display rotation. fb_get_width()/fb_get_height()/
// fb_get_pitch() below, and every draw primitive in this file, operate in
// LOGICAL space - the screen as the user is meant to see it, already
// rotation-corrected. Only fb_swap_buffers()/fb_swap_dirty_rects() (the
// present chokepoint) and fb_get_phys_*() below know about the raw,
// possibly-portrait GOP layout. See the block comment above fb_rotation in
// framebuffer.c for the full rationale.
typedef enum {
    FB_ROTATE_NONE = 0,
    FB_ROTATE_90   = 1,
    FB_ROTATE_180  = 2,
    FB_ROTATE_270  = 3,
} fb_rotation_t;

fb_rotation_t fb_get_rotation(void);
uint32_t fb_get_phys_width(void);
uint32_t fb_get_phys_height(void);

// Cumulative TSC-cycle cost of the rotated present copy (fb_present_rect_
// rotated in framebuffer.c). All-zero and meaningless when fb_get_rotation()
// is FB_ROTATE_NONE - there is nothing to measure on the code path every
// non-rotated machine still takes. Read by main.c's [ROTPROF] boot log line.
void fb_rotate_profile_get(uint64_t *tot_cyc, uint64_t *max_cyc,
                            uint64_t *calls, uint64_t *px_tot);

// ---------------------------------------------------------------------------
// #halfres: integer PRESENT-SCALE compositing. One owner's 3840x2160 panel
// has no 1920x1080 firmware mode (all seven GOP modes are 4:3/5:4 except
// native 4K), and he already runs the UI at 200% scale, i.e. every widget is
// already drawn at double size into 8.29 Mpx. Compositing at 1920x1080
// (scale 100%) and presenting with an EXACT integer 2x pixel replication
// gives the identical apparent picture for a quarter of the compositing
// work, with no resampling softness (unlike the general "virtual resolution"
// idea a prior investigation measured and rejected as both slower and soft).
//
// This reuses EXACTLY the logical/physical split #745's display rotation
// already built: fb_get_width()/fb_get_height() (and every draw primitive)
// stay LOGICAL - the compositor, uiscale, and every app keep thinking in
// 1920x1080 and never learn this exists - while fb_get_phys_*() is the real
// panel, and only the present chokepoint (fb_swap_buffers/fb_swap_dirty_
// rects) knows how to get from one to the other. See kernel/gui/presentscale.c
// for the config/ESP-override plumbing (mirrors uiscale.c) and
// kernel/rustkern/presentscale.rs for the pure validate-the-factor arithmetic
// (mirrors uiscale.rs). See framebuffer.c for why this does NOT reallocate
// the back buffer (kmalloc_aligned() memory here has no matching free).
//
// n=1 (the default, and what every machine that never asked for this stays
// at) is byte-identical to a kernel without this feature: fb_set_present_
// scale(1) is a no-op and fb_swap_buffers()/fb_swap_dirty_rects() take their
// UNCHANGED pre-existing code path.
bool fb_set_present_scale(int n);
int  fb_get_present_scale(void);

// Cumulative TSC-cycle cost of the present-scale replication copy
// (fb_present_rect_scaled in framebuffer.c), mirroring fb_rotate_profile_get.
// All-zero when fb_get_present_scale() == 1. Read by main.c's [SCALEPROF]
// boot log line. src_px_tot is what the compositor actually composited (the
// number this feature is meant to shrink); dst_px_tot is physical pixels
// written, which does NOT shrink - the present still touches every real
// panel pixel either way, only the SOURCE read shrinks.
void fb_scale_profile_get(uint64_t *tot_cyc, uint64_t *max_cyc,
                           uint64_t *calls, uint64_t *src_px_tot,
                           uint64_t *dst_px_tot);

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
