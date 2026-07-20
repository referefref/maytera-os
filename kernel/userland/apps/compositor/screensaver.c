// screensaver.c - Screensaver effects for MayteraOS userland compositor.
// Supports blank, starfield, bouncing lines, and expanding bubbles modes.
// No malloc. All state is static. Idle detection uses sys_clock() at 100 Hz.

#include "compositor.h"
#include "../../libc/syscall.h"

// ============================================================================
// Static state
// ============================================================================

static screensaver_type_t g_ss_type    = SS_STARFIELD;
static int                g_ss_timeout = SS_DEFAULT_TIMEOUT; // seconds
static uint32_t           g_ss_frame;
static uint32_t           g_ss_seed    = 12345;

// Starfield
static ss_star_t   g_stars[SS_MAX_STARS];

// Lines
static ss_line_t   g_lines[SS_MAX_LINES];

// Bubbles
static ss_bubble_t g_bubbles[SS_MAX_BUBBLES];

// ============================================================================
// PRNG
// ============================================================================

static uint32_t ss_rand(void) {
    g_ss_seed = g_ss_seed * 1103515245 + 12345;
    return (g_ss_seed >> 16) & 0x7FFF;
}

// ============================================================================
// Helpers
// ============================================================================

// Draw a line using Bresenham's algorithm.
static void ss_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                         uint32_t color) {
    int32_t dx  =  (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int32_t dy  = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int32_t sx  =  (x0 < x1) ? 1 : -1;
    int32_t sy  =  (y0 < y1) ? 1 : -1;
    int32_t err =  dx + dy;

    while (1) {
        draw_putpixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Draw the outline of a circle using the midpoint circle algorithm.
static void ss_draw_circle_outline(int32_t cx, int32_t cy, int32_t r,
                                   uint32_t color) {
    if (r <= 0) return;
    int32_t x = 0;
    int32_t y = r;
    int32_t d = 3 - 2 * r;

    while (x <= y) {
        draw_putpixel(cx + x, cy + y, color);
        draw_putpixel(cx - x, cy + y, color);
        draw_putpixel(cx + x, cy - y, color);
        draw_putpixel(cx - x, cy - y, color);
        draw_putpixel(cx + y, cy + x, color);
        draw_putpixel(cx - y, cy + x, color);
        draw_putpixel(cx + y, cy - x, color);
        draw_putpixel(cx - y, cy - x, color);
        if (d < 0) {
            d += 4 * x + 6;
        } else {
            d += 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

// ============================================================================
// screensaver_init
// ============================================================================

void screensaver_init(void) {
    int32_t i;

    // Starfield: spread stars across the virtual 3-D volume.
    for (i = 0; i < SS_MAX_STARS; i++) {
        g_stars[i].x = (int32_t)(ss_rand() % (uint32_t)g_fb_width);
        g_stars[i].y = (int32_t)(ss_rand() % (uint32_t)g_fb_height);
        g_stars[i].z = (int32_t)((ss_rand() % 256) + 1);
    }

    // Lines: random start and end points with random bounce velocities.
    for (i = 0; i < SS_MAX_LINES; i++) {
        g_lines[i].x1 = (int32_t)(ss_rand() % (uint32_t)g_fb_width);
        g_lines[i].y1 = (int32_t)(ss_rand() % (uint32_t)g_fb_height);
        g_lines[i].x2 = (int32_t)(ss_rand() % (uint32_t)g_fb_width);
        g_lines[i].y2 = (int32_t)(ss_rand() % (uint32_t)g_fb_height);

        // Velocities in range [-4, 4], excluding 0.
        int32_t d1x = (int32_t)(ss_rand() % 4) + 1;
        int32_t d1y = (int32_t)(ss_rand() % 4) + 1;
        int32_t d2x = (int32_t)(ss_rand() % 4) + 1;
        int32_t d2y = (int32_t)(ss_rand() % 4) + 1;
        g_lines[i].dx1 = (ss_rand() & 1) ? d1x : -d1x;
        g_lines[i].dy1 = (ss_rand() & 1) ? d1y : -d1y;
        g_lines[i].dx2 = (ss_rand() & 1) ? d2x : -d2x;
        g_lines[i].dy2 = (ss_rand() & 1) ? d2y : -d2y;

        // Random saturated color with full alpha.
        uint32_t r = (ss_rand() % 128) + 64;
        uint32_t g = (ss_rand() % 128) + 64;
        uint32_t b = (ss_rand() % 128) + 64;
        g_lines[i].color = 0xFF000000 | (r << 16) | (g << 8) | b;
    }

    // Bubbles: random center, radius starting at 1, random max and color.
    for (i = 0; i < SS_MAX_BUBBLES; i++) {
        g_bubbles[i].x          = (int32_t)(ss_rand() % (uint32_t)g_fb_width);
        g_bubbles[i].y          = (int32_t)(ss_rand() % (uint32_t)g_fb_height);
        g_bubbles[i].radius     = (int32_t)((ss_rand() % 30) + 1);
        g_bubbles[i].max_radius = (int32_t)((ss_rand() % 80) + 20);
        g_bubbles[i].dr         = 1; // growing

        uint32_t r = (ss_rand() % 128) + 64;
        uint32_t g = (ss_rand() % 128) + 64;
        uint32_t b = (ss_rand() % 128) + 64;
        g_bubbles[i].color = 0xFF000000 | (r << 16) | (g << 8) | b;
    }

    g_ss_frame = 0;
}

// ============================================================================
// screensaver_render
// ============================================================================

void screensaver_render(void) {
    int32_t i;

    switch (g_ss_type) {

    // -----------------------------------------------------------------------
    case SS_NONE:
        // No screensaver, nothing to draw.
        break;

    // -----------------------------------------------------------------------
    case SS_BLANK:
        draw_fill_rect(0, 0, g_fb_width, g_fb_height, 0xFF000000);
        break;

    // -----------------------------------------------------------------------
    case SS_STARFIELD: {
        draw_fill_rect(0, 0, g_fb_width, g_fb_height, 0xFF000000);

        int32_t cx = g_fb_width  / 2;
        int32_t cy = g_fb_height / 2;

        for (i = 0; i < SS_MAX_STARS; i++) {
            // Move star closer to the camera (lower z = nearer).
            g_stars[i].z -= 2;

            // Recycle stars that fly past the camera.
            if (g_stars[i].z <= 0) {
                g_stars[i].x = (int32_t)(ss_rand() % (uint32_t)g_fb_width);
                g_stars[i].y = (int32_t)(ss_rand() % (uint32_t)g_fb_height);
                g_stars[i].z = 255;
            }

            int32_t z = g_stars[i].z;

            // Project onto the 2-D screen.
            int32_t sx = cx + (g_stars[i].x - cx) * 256 / z;
            int32_t sy = cy + (g_stars[i].y - cy) * 256 / z;

            // Clip to framebuffer bounds before drawing.
            if (sx < 0 || sx >= g_fb_width || sy < 0 || sy >= g_fb_height) {
                continue;
            }

            // Brightness: stars with small z are close, therefore bright.
            // Map z in [1,255] linearly to brightness in [255,20].
            uint32_t bright = (uint32_t)(255 - (z * 235 / 255));
            uint32_t color  = 0xFF000000 | (bright << 16) | (bright << 8) | bright;

            if (z < 64) {
                // Draw a 2x2 block for nearby stars.
                draw_putpixel(sx,     sy,     color);
                draw_putpixel(sx + 1, sy,     color);
                draw_putpixel(sx,     sy + 1, color);
                draw_putpixel(sx + 1, sy + 1, color);
            } else {
                draw_putpixel(sx, sy, color);
            }
        }
        break;
    }

    // -----------------------------------------------------------------------
    case SS_LINES:
        draw_fill_rect(0, 0, g_fb_width, g_fb_height, 0xFF000000);

        for (i = 0; i < SS_MAX_LINES; i++) {
            // Update endpoints and bounce off screen edges.
            g_lines[i].x1 += g_lines[i].dx1;
            g_lines[i].y1 += g_lines[i].dy1;
            g_lines[i].x2 += g_lines[i].dx2;
            g_lines[i].y2 += g_lines[i].dy2;

            if (g_lines[i].x1 < 0 || g_lines[i].x1 >= g_fb_width) {
                g_lines[i].dx1 = -g_lines[i].dx1;
                g_lines[i].x1 += g_lines[i].dx1 * 2;
            }
            if (g_lines[i].y1 < 0 || g_lines[i].y1 >= g_fb_height) {
                g_lines[i].dy1 = -g_lines[i].dy1;
                g_lines[i].y1 += g_lines[i].dy1 * 2;
            }
            if (g_lines[i].x2 < 0 || g_lines[i].x2 >= g_fb_width) {
                g_lines[i].dx2 = -g_lines[i].dx2;
                g_lines[i].x2 += g_lines[i].dx2 * 2;
            }
            if (g_lines[i].y2 < 0 || g_lines[i].y2 >= g_fb_height) {
                g_lines[i].dy2 = -g_lines[i].dy2;
                g_lines[i].y2 += g_lines[i].dy2 * 2;
            }

            ss_draw_line(g_lines[i].x1, g_lines[i].y1,
                         g_lines[i].x2, g_lines[i].y2,
                         g_lines[i].color);
        }
        break;

    // -----------------------------------------------------------------------
    case SS_BUBBLES:
        draw_fill_rect(0, 0, g_fb_width, g_fb_height, 0xFF000000);

        for (i = 0; i < SS_MAX_BUBBLES; i++) {
            // Grow or shrink the radius.
            g_bubbles[i].radius += g_bubbles[i].dr;

            if (g_bubbles[i].radius >= g_bubbles[i].max_radius) {
                // Reached maximum: start shrinking.
                g_bubbles[i].dr = -1;
            } else if (g_bubbles[i].radius <= 0) {
                // Collapsed: respawn at a new position with a new max radius.
                g_bubbles[i].x          = (int32_t)(ss_rand() % (uint32_t)g_fb_width);
                g_bubbles[i].y          = (int32_t)(ss_rand() % (uint32_t)g_fb_height);
                g_bubbles[i].max_radius = (int32_t)((ss_rand() % 80) + 20);
                g_bubbles[i].radius     = 1;
                g_bubbles[i].dr         = 1;

                uint32_t r = (ss_rand() % 128) + 64;
                uint32_t g = (ss_rand() % 128) + 64;
                uint32_t b = (ss_rand() % 128) + 64;
                g_bubbles[i].color = 0xFF000000 | (r << 16) | (g << 8) | b;
            }

            ss_draw_circle_outline(g_bubbles[i].x, g_bubbles[i].y,
                                   g_bubbles[i].radius, g_bubbles[i].color);
        }
        break;

    // -----------------------------------------------------------------------
    case SS_MATRIX:
        // Matrix mode is not implemented; fall back to blank.
        draw_fill_rect(0, 0, g_fb_width, g_fb_height, 0xFF000000);
        break;
    }

    g_ss_frame++;
}

// ============================================================================
// screensaver_on_input
// ============================================================================

void screensaver_on_input(void) {
    // Reset the idle timer to the current clock tick.
    g_idle_ticks = (uint64_t)sys_clock();

    // Dismiss an active screensaver on any user input.
    if (g_screensaver_active) {
        g_screensaver_active = false;
        g_needs_redraw       = true;
    }
}

// ============================================================================
// screensaver_check_timeout
// ============================================================================

bool screensaver_check_timeout(void) {
    // Already active: keep it running.
    if (g_screensaver_active) {
        return true;
    }

    // SS_NONE means the feature is disabled entirely.
    if (g_ss_type == SS_NONE) {
        return false;
    }

    // Convert elapsed ticks to seconds. The kernel timer runs at 100 Hz.
    uint64_t now     = (uint64_t)sys_clock();
    uint64_t elapsed = (now - g_idle_ticks) / 100;

    if (elapsed >= (uint64_t)g_ss_timeout) {
        g_screensaver_active = true;
        return true;
    }

    return false;
}
