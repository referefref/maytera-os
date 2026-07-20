// graphics.c - Graphics primitives implementation
#include "graphics.h"
#include "font.h"
#include "boot_image.h"
#include "../fs/fat.h"
#include "../gui/image.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../version.h"

// Stringify the numeric build number so it concatenates into the version string.
#define GFX_STR2(x) #x
#define GFX_STR(x)  GFX_STR2(x)

// Loaded boot image from disk (if available)
static image_t g_disk_boot_image = {0};
static bool g_disk_boot_image_loaded = false;

// Track if boot screen needs refresh after image loads
static bool g_boot_screen_active = false;

// External filesystem
extern fat_fs_t g_fat_fs;

// Load boot image from disk (call after filesystem is mounted)
int gfx_load_boot_image_from_disk(void) {
    if (g_disk_boot_image_loaded) {
        return 0;  // Already loaded
    }

    // Check if filesystem is mounted
    if (!g_fat_fs.mounted && g_fat_fs.bytes_per_sector == 0) {
        kprintf("[GFX] Filesystem not mounted, cannot load boot image\n");
        return -1;
    }

    // Try to load BOOT.BMP from disk - check /BOOT/ directory first
    const char *boot_files[] = {"/BOOT/BOOT.BMP", "BOOT.BMP", "/BOOT.BMP", "boot.bmp", NULL};

    for (int i = 0; boot_files[i] != NULL; i++) {
        kprintf("[GFX] Trying to load boot image: %s\n", boot_files[i]);
        uint32_t size = 0;
        void *data = fat_read_file(&g_fat_fs, boot_files[i], &size);

        if (data && size > 54) {
            kprintf("[GFX] Loading boot image from %s (%u bytes)\n", boot_files[i], size);

            int result = image_load_bmp(data, size, &g_disk_boot_image);
            if (result == IMAGE_SUCCESS) {
                g_disk_boot_image_loaded = true;
                kprintf("[GFX] Boot image loaded: %ux%u\n",
                        g_disk_boot_image.width, g_disk_boot_image.height);
                kfree(data);
                return 0;
            } else {
                kprintf("[GFX] Failed to parse BMP: error %d\n", result);
            }
            kfree(data);
        }
    }

    kprintf("[GFX] No boot image found on disk, using gradient background\n");
    return -1;
}

// Check if boot image was loaded from disk
bool gfx_has_disk_boot_image(void) {
    return g_disk_boot_image_loaded;
}

// Draw a single character at pixel position
static void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = font_get_glyph(c);
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                fb_put_pixel(x + col, y + row, fg);
            } else if (bg != 0) {
                fb_put_pixel(x + col, y + row, bg);
            }
        }
    }
}

// Draw a filled circle using midpoint circle algorithm
void gfx_fill_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color) {
    if (radius <= 0) return;

    int32_t x = 0;
    int32_t y = radius;
    int32_t d = 1 - radius;

    while (x <= y) {
        // Draw horizontal lines to fill the circle
        for (int32_t i = cx - y; i <= cx + y; i++) {
            fb_put_pixel(i, cy + x, color);
            fb_put_pixel(i, cy - x, color);
        }
        for (int32_t i = cx - x; i <= cx + x; i++) {
            fb_put_pixel(i, cy + y, color);
            fb_put_pixel(i, cy - y, color);
        }

        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

// Draw a circle outline
void gfx_draw_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color) {
    if (radius <= 0) return;

    int32_t x = 0;
    int32_t y = radius;
    int32_t d = 1 - radius;

    while (x <= y) {
        fb_put_pixel(cx + x, cy + y, color);
        fb_put_pixel(cx - x, cy + y, color);
        fb_put_pixel(cx + x, cy - y, color);
        fb_put_pixel(cx - x, cy - y, color);
        fb_put_pixel(cx + y, cy + x, color);
        fb_put_pixel(cx - y, cy + x, color);
        fb_put_pixel(cx + y, cy - x, color);
        fb_put_pixel(cx - y, cy - x, color);

        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

// Draw a filled triangle using scanline algorithm
void gfx_fill_triangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                       int32_t x3, int32_t y3, uint32_t color) {
    // Sort vertices by y coordinate
    if (y1 > y2) { int32_t tx=x1,ty=y1; x1=x2; y1=y2; x2=tx; y2=ty; }
    if (y2 > y3) { int32_t tx=x2,ty=y2; x2=x3; y2=y3; x3=tx; y3=ty; }
    if (y1 > y2) { int32_t tx=x1,ty=y1; x1=x2; y1=y2; x2=tx; y2=ty; }

    // Check for degenerate triangle
    if (y1 == y3) {
        int32_t minx = x1 < x2 ? (x1 < x3 ? x1 : x3) : (x2 < x3 ? x2 : x3);
        int32_t maxx = x1 > x2 ? (x1 > x3 ? x1 : x3) : (x2 > x3 ? x2 : x3);
        fb_draw_line(minx, y1, maxx, y1, color);
        return;
    }

    // Draw using scanline
    for (int32_t y = y1; y <= y3; y++) {
        int32_t xa, xb;

        if (y < y2) {
            // Upper part
            xa = x1 + (x2 - x1) * (y - y1) / (y2 - y1);
            xb = x1 + (x3 - x1) * (y - y1) / (y3 - y1);
        } else {
            // Lower part
            if (y2 != y3) {
                xa = x2 + (x3 - x2) * (y - y2) / (y3 - y2);
            } else {
                xa = x2;
            }
            xb = x1 + (x3 - x1) * (y - y1) / (y3 - y1);
        }

        if (xa > xb) { int32_t t = xa; xa = xb; xb = t; }

        for (int32_t x = xa; x <= xb; x++) {
            fb_put_pixel(x, y, color);
        }
    }
}

// Draw gradient rectangle
void gfx_gradient_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint32_t color1, uint32_t color2, int horizontal) {
    uint8_t r1 = (color1 >> 0) & 0xFF;
    uint8_t g1 = (color1 >> 8) & 0xFF;
    uint8_t b1 = (color1 >> 16) & 0xFF;

    uint8_t r2 = (color2 >> 0) & 0xFF;
    uint8_t g2 = (color2 >> 8) & 0xFF;
    uint8_t b2 = (color2 >> 16) & 0xFF;

    uint32_t steps = horizontal ? w : h;
    if (steps == 0) return;

    for (uint32_t i = 0; i < steps; i++) {
        uint8_t r = r1 + (r2 - r1) * i / steps;
        uint8_t g = g1 + (g2 - g1) * i / steps;
        uint8_t b = b1 + (b2 - b1) * i / steps;
        uint32_t color = FB_COLOR(r, g, b);

        if (horizontal) {
            for (uint32_t j = 0; j < h; j++) {
                fb_put_pixel(x + i, y + j, color);
            }
        } else {
            for (uint32_t j = 0; j < w; j++) {
                fb_put_pixel(x + j, y + i, color);
            }
        }
    }
}

// Blend two colors
uint32_t gfx_blend(uint32_t color1, uint32_t color2, uint8_t alpha) {
    uint8_t r1 = (color1 >> 0) & 0xFF;
    uint8_t g1 = (color1 >> 8) & 0xFF;
    uint8_t b1 = (color1 >> 16) & 0xFF;

    uint8_t r2 = (color2 >> 0) & 0xFF;
    uint8_t g2 = (color2 >> 8) & 0xFF;
    uint8_t b2 = (color2 >> 16) & 0xFF;

    uint8_t r = (r1 * (255 - alpha) + r2 * alpha) / 255;
    uint8_t g = (g1 * (255 - alpha) + g2 * alpha) / 255;
    uint8_t b = (b1 * (255 - alpha) + b2 * alpha) / 255;

    return FB_COLOR(r, g, b);
}

// Font data for large text (8x8 bitmap font for A-Z, 0-9, and common symbols)
static const uint8_t large_font[][8] = {
    ['A' - 'A'] = {0x38, 0x44, 0x82, 0x82, 0xFE, 0x82, 0x82, 0x82},
    ['B' - 'A'] = {0xFC, 0x82, 0x82, 0xFC, 0x82, 0x82, 0x82, 0xFC},
    ['C' - 'A'] = {0x7C, 0x82, 0x80, 0x80, 0x80, 0x80, 0x82, 0x7C},
    ['D' - 'A'] = {0xF8, 0x84, 0x82, 0x82, 0x82, 0x82, 0x84, 0xF8},
    ['E' - 'A'] = {0xFE, 0x80, 0x80, 0xF8, 0x80, 0x80, 0x80, 0xFE},
    ['F' - 'A'] = {0xFE, 0x80, 0x80, 0xF8, 0x80, 0x80, 0x80, 0x80},
    ['G' - 'A'] = {0x7C, 0x82, 0x80, 0x80, 0x9E, 0x82, 0x82, 0x7C},
    ['H' - 'A'] = {0x82, 0x82, 0x82, 0xFE, 0x82, 0x82, 0x82, 0x82},
    ['I' - 'A'] = {0x7C, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x7C},
    ['J' - 'A'] = {0x3E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x84, 0x78},
    ['K' - 'A'] = {0x82, 0x84, 0x88, 0xF0, 0x88, 0x84, 0x82, 0x82},
    ['L' - 'A'] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xFE},
    ['M' - 'A'] = {0x82, 0xC6, 0xAA, 0x92, 0x82, 0x82, 0x82, 0x82},
    ['N' - 'A'] = {0x82, 0xC2, 0xA2, 0x92, 0x8A, 0x86, 0x82, 0x82},
    ['O' - 'A'] = {0x7C, 0x82, 0x82, 0x82, 0x82, 0x82, 0x82, 0x7C},
    ['P' - 'A'] = {0xFC, 0x82, 0x82, 0xFC, 0x80, 0x80, 0x80, 0x80},
    ['Q' - 'A'] = {0x7C, 0x82, 0x82, 0x82, 0x82, 0x8A, 0x84, 0x7A},
    ['R' - 'A'] = {0xFC, 0x82, 0x82, 0xFC, 0x88, 0x84, 0x82, 0x82},
    ['S' - 'A'] = {0x7C, 0x82, 0x80, 0x7C, 0x02, 0x02, 0x82, 0x7C},
    ['T' - 'A'] = {0xFE, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10},
    ['U' - 'A'] = {0x82, 0x82, 0x82, 0x82, 0x82, 0x82, 0x82, 0x7C},
    ['V' - 'A'] = {0x82, 0x82, 0x82, 0x44, 0x44, 0x28, 0x28, 0x10},
    ['W' - 'A'] = {0x82, 0x82, 0x82, 0x82, 0x92, 0xAA, 0xC6, 0x82},
    ['X' - 'A'] = {0x82, 0x44, 0x28, 0x10, 0x28, 0x44, 0x82, 0x82},
    ['Y' - 'A'] = {0x82, 0x44, 0x28, 0x10, 0x10, 0x10, 0x10, 0x10},
    ['Z' - 'A'] = {0xFE, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0xFE},
};

// Number font data
static const uint8_t number_font[][8] = {
    [0] = {0x7C, 0x82, 0x8A, 0x92, 0xA2, 0x82, 0x82, 0x7C},
    [1] = {0x10, 0x30, 0x10, 0x10, 0x10, 0x10, 0x10, 0x38},
    [2] = {0x7C, 0x82, 0x02, 0x0C, 0x30, 0x40, 0x80, 0xFE},
    [3] = {0x7C, 0x82, 0x02, 0x1C, 0x02, 0x02, 0x82, 0x7C},
    [4] = {0x08, 0x18, 0x28, 0x48, 0x88, 0xFE, 0x08, 0x08},
    [5] = {0xFE, 0x80, 0x80, 0xFC, 0x02, 0x02, 0x82, 0x7C},
    [6] = {0x7C, 0x82, 0x80, 0xFC, 0x82, 0x82, 0x82, 0x7C},
    [7] = {0xFE, 0x02, 0x04, 0x08, 0x10, 0x20, 0x20, 0x20},
    [8] = {0x7C, 0x82, 0x82, 0x7C, 0x82, 0x82, 0x82, 0x7C},
    [9] = {0x7C, 0x82, 0x82, 0x7E, 0x02, 0x02, 0x82, 0x7C},
};

// Draw large character with scale
static void draw_large_char(int x, int y, char c, int scale, uint32_t color) {
    const uint8_t *bits = NULL;

    if (c >= 'A' && c <= 'Z') {
        bits = large_font[c - 'A'];
    } else if (c >= 'a' && c <= 'z') {
        bits = large_font[c - 'a'];
    } else if (c >= '0' && c <= '9') {
        bits = number_font[c - '0'];
    } else if (c == '.') {
        // Draw a dot
        fb_fill_rect(x + 3 * scale, y + 6 * scale, scale * 2, scale * 2, color);
        return;
    } else if (c == ' ') {
        return;  // Space - just skip
    } else {
        return;  // Unsupported character
    }

    if (!bits) return;

    for (int row = 0; row < 8; row++) {
        uint8_t b = bits[row];
        for (int col = 0; col < 8; col++) {
            if (b & (0x80 >> col)) {
                fb_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

// Draw text string with large font
__attribute__((unused))
static void draw_large_text(int x, int y, const char *text, int scale, uint32_t color) {
    int char_width = 8 * scale + scale;  // Character width plus spacing
    for (int i = 0; text[i]; i++) {
        draw_large_char(x + i * char_width, y, text[i], scale, color);
    }
}

// Calculate text width for centering
__attribute__((unused))
static int calc_text_width(const char *text, int scale) {
    int len = 0;
    while (text[len]) len++;
    return len * (8 * scale + scale);
}

// MayteraOS version string
// Version now comes from version.h

// One-way latch: once the graphical boot splash (BOOT.BMP) has been shown via
// ANY boot path, the loading spinner must never draw again over it. See the
// "second splash then no spinner" UX fix.
static int g_boot_splash_shown = 0;

// Draw MayteraOS boot splash
void gfx_boot_splash(void) {
    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();

    g_boot_splash_shown = 1;

    kprintf("[GFX] Boot splash: screen %ux%u\n", w, h);

    // Draw the boot image (handles background and centered image)
    gfx_draw_boot_image();
    kprintf("[GFX] Boot image drawn\n");

    // The boot image now has the MayteraOS logo embedded, so we no longer draw a
    // "MayteraOS" title. Version + subtitle are placed ~20% of the screen height
    // lower than before so they sit below the embedded logo, not over it.
    const char *version_label = "Version ";
    const char *version_num = MAYTERA_VERSION_STRING "  build " GFX_STR(MAYTERA_BUILD_NUMBER);
    // (raised so the "Loading..."/bar sit higher and leave room for a taller log)
    int version_y = h * 55 / 100;

    // Center "Version " + version_num based on their real combined length.
    int vnlen = 0; while (version_num[vnlen]) vnlen++;
    int version_x = (w - (8 + vnlen) * 8) / 2;
    for (int i = 0; version_label[i]; i++) {
        gfx_draw_char(version_x + i * 8 + 1, version_y + 1, version_label[i], FB_COLOR(0, 0, 0), 0);
        gfx_draw_char(version_x + i * 8, version_y, version_label[i], FB_COLOR(220, 220, 240), 0);
    }

    // Draw version number in accent color with shadow
    int num_x = version_x + 8 * 8;
    for (int i = 0; version_num[i]; i++) {
        gfx_draw_char(num_x + i * 8 + 1, version_y + 1, version_num[i], FB_COLOR(0, 0, 0), 0);
        gfx_draw_char(num_x + i * 8, version_y, version_num[i], FB_COLOR(100, 200, 255), 0);
    }

    // Calculate bar position. Raised to 62% (was 70%) so the "Loading..." label
    // and bar sit higher, leaving room for a taller boot-log console below.
    int bar_y = h * 62 / 100;
    int bar_w = w / 3;  // 1/3 screen width
    int bar_h = 8;      // Thinner, more elegant bar
    int bar_x = (w - bar_w) / 2;

    // Bar background (dark semi-transparent look) - no black box around it
    fb_fill_rect(bar_x, bar_y, bar_w, bar_h, FB_COLOR(30, 35, 50));
    // Bar border
    fb_draw_rect(bar_x - 1, bar_y - 1, bar_w + 2, bar_h + 2, FB_COLOR(80, 90, 120));

    // "Loading..." text above the bar with shadow
    const char *loading = "Loading...";
    int load_len = 10;
    int load_x = (w - load_len * 8) / 2;
    int load_y = bar_y - 20;
    for (int i = 0; loading[i]; i++) {
        gfx_draw_char(load_x + i * 8 + 1, load_y + 1, loading[i], FB_COLOR(0, 0, 0), 0);
        gfx_draw_char(load_x + i * 8, load_y, loading[i], FB_COLOR(220, 230, 250), 0);
    }

    // Swap buffers to display the splash screen
    fb_swap_buffers();
}

// Helper function to get bar Y position consistently
static int get_boot_bar_y(void) {
    uint32_t h = fb_get_height();
    // Raised to 62% (matches gfx_boot_splash) so the progress bar + boot-log
    // console sit higher and the log has room for more lines.
    return h * 62 / 100;
}

// Update boot splash progress (0-100)
void gfx_boot_progress(int percent) {
    uint32_t w = fb_get_width();

    int bar_y = get_boot_bar_y();
    int bar_w = w / 3;
    int bar_h = 8;
    int bar_x = (w - bar_w) / 2;

    // Clamp percent
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    int fill_w = (bar_w * percent) / 100;

    // Draw progress fill with gradient effect
    // Blue to cyan gradient for the fill
    for (int x = 0; x < fill_w; x++) {
        int ratio = x * 255 / bar_w;
        uint8_t r = 50 + ratio / 4;
        uint8_t g = 150 + ratio / 3;
        uint8_t b = 255;
        for (int dy = 0; dy < bar_h; dy++) {
            fb_put_pixel(bar_x + x, bar_y + dy, FB_COLOR(r, g, b));
        }
    }

    // Swap buffers to display progress update
    fb_swap_buffers();
}

// Update boot status text (shown below loading bar)
void gfx_boot_status(const char *status) {
    uint32_t w = fb_get_width();

    int bar_y = get_boot_bar_y();

    // Status text position - below the loading bar
    int status_y = bar_y + 25;

    // Clear previous status line with black background
    int clear_w = w * 2 / 3;
    int clear_x = (w - clear_w) / 2;
    fb_fill_rect(clear_x, status_y, clear_w, 20, FB_COLOR(0, 0, 0));

    // Calculate text length and center it
    int len = 0;
    while (status[len]) len++;

    int text_x = (w - len * 8) / 2;

    // Draw status text
    for (int i = 0; status[i]; i++) {
        gfx_draw_char(text_x + i * 8, status_y, status[i], FB_COLOR(200, 210, 230), 0);
    }
}

// Draw a test pattern
void gfx_test_pattern(void) {
    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();

    // Color bars
    uint32_t colors[] = {
        FB_WHITE, FB_YELLOW, FB_CYAN, FB_GREEN,
        FB_MAGENTA, FB_RED, FB_BLUE, FB_BLACK
    };
    uint32_t bar_width = w / 8;

    for (int i = 0; i < 8; i++) {
        fb_fill_rect(i * bar_width, 0, bar_width, h * 2 / 3, colors[i]);
    }

    // Gradient bar at bottom
    gfx_gradient_rect(0, h * 2 / 3, w, h / 6, FB_BLACK, FB_WHITE, 1);

    // Draw some shapes
    gfx_fill_circle(w / 4, h * 5 / 6, 30, FB_RED);
    gfx_draw_circle(w / 2, h * 5 / 6, 30, FB_GREEN);
    gfx_fill_triangle(w * 3 / 4 - 30, h * 5 / 6 + 30,
                      w * 3 / 4, h * 5 / 6 - 30,
                      w * 3 / 4 + 30, h * 5 / 6 + 30, FB_BLUE);
}

// Boot log state
#define BOOT_LOG_MAX_LINES 16
#define BOOT_LOG_MAX_CHARS 80
static char boot_log_lines[BOOT_LOG_MAX_LINES][BOOT_LOG_MAX_CHARS];
static int boot_log_count = 0;

// Clear boot log
void gfx_boot_log_clear(void) {
    boot_log_count = 0;
    for (int i = 0; i < BOOT_LOG_MAX_LINES; i++) {
        boot_log_lines[i][0] = '\0';
    }
}

// Draw a rounded rectangle (simple version with corner radius)
static void draw_rounded_rect_filled(int x, int y, int w, int h, int radius, uint32_t color) {
    // Draw main body rectangles
    fb_fill_rect(x + radius, y, w - 2 * radius, h, color);  // Center
    fb_fill_rect(x, y + radius, radius, h - 2 * radius, color);  // Left
    fb_fill_rect(x + w - radius, y + radius, radius, h - 2 * radius, color);  // Right

    // Draw corners using filled circles (quarter circles)
    for (int cy = 0; cy < radius; cy++) {
        for (int cx = 0; cx < radius; cx++) {
            int dx = radius - cx - 1;
            int dy = radius - cy - 1;
            if (dx * dx + dy * dy <= radius * radius) {
                // Top-left corner
                fb_put_pixel(x + cx, y + cy, color);
                // Top-right corner
                fb_put_pixel(x + w - 1 - cx, y + cy, color);
                // Bottom-left corner
                fb_put_pixel(x + cx, y + h - 1 - cy, color);
                // Bottom-right corner
                fb_put_pixel(x + w - 1 - cx, y + h - 1 - cy, color);
            }
        }
    }
}

// Add message to boot log (scrolling dmesg-style)
void gfx_boot_log(const char *message) {
    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();

    // Shift lines up if full
    if (boot_log_count >= BOOT_LOG_MAX_LINES) {
        for (int i = 0; i < BOOT_LOG_MAX_LINES - 1; i++) {
            for (int j = 0; j < BOOT_LOG_MAX_CHARS; j++) {
                boot_log_lines[i][j] = boot_log_lines[i + 1][j];
            }
        }
        boot_log_count = BOOT_LOG_MAX_LINES - 1;
    }

    // Copy message to current line
    int len = 0;
    while (message[len] && len < BOOT_LOG_MAX_CHARS - 1) {
        boot_log_lines[boot_log_count][len] = message[len];
        len++;
    }
    boot_log_lines[boot_log_count][len] = '\0';
    boot_log_count++;

    // Calculate log area position (below loading bar)
    int bar_y = get_boot_bar_y();
    int log_y = bar_y + 20;  // Below bar
    int log_h = BOOT_LOG_MAX_LINES * 16 + 16;  // Extra padding
    int log_w = w * 3 / 4;
    int log_x = (w - log_w) / 2;

    // Clamp log area to screen
    if (log_y + log_h > (int)h - 10) {
        log_h = h - log_y - 10;
    }

    // Draw rounded rectangle background for log area (semi-transparent effect)
    int corner_radius = 12;
    // Use a semi-transparent dark background
    uint32_t log_bg_color = FB_COLOR(0, 0, 0);  // Will overlay on boot image
    draw_rounded_rect_filled(log_x, log_y, log_w, log_h, corner_radius, log_bg_color);

    // Draw all log lines
    for (int i = 0; i < boot_log_count; i++) {
        int text_x = log_x + 12;
        int text_y = log_y + 8 + i * 16;
        if (text_y + 16 > (int)h - 10) break;  // Don't draw off screen
        for (int j = 0; boot_log_lines[i][j]; j++) {
            gfx_draw_char(text_x + j * 8, text_y, boot_log_lines[i][j],
                          FB_COLOR(180, 200, 230), 0);
        }
    }

    // Swap buffers to display log update
    fb_swap_buffers();
}

// Draw boot image (BOOT.BMP from disk, or gradient fallback)
// This is the SINGLE source of truth for boot background
void gfx_draw_boot_image(void) {
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();

    // If we have BOOT.BMP loaded from disk, use it fullscreen
    if (g_disk_boot_image_loaded && g_disk_boot_image.pixels) {
        // Use image_blit_scaled to draw fullscreen
        image_blit_scaled(&g_disk_boot_image, 0, 0, screen_w, screen_h);
        return;
    }

    // Fallback (before BOOT.BMP is loaded from disk): the embedded boot splash
    // image (boot-splash-1), scaled fullscreen with nearest-neighbor. This is the
    // very first screen, shown before the filesystem is mounted.
    if (boot_image_size >= (uint32_t)(BOOT_IMAGE_WIDTH * BOOT_IMAGE_HEIGHT * 3)) {
        for (uint32_t y = 0; y < screen_h; y++) {
            uint32_t sy = y * BOOT_IMAGE_HEIGHT / screen_h;
            const uint8_t *srow = boot_image_data + (uint64_t)sy * BOOT_IMAGE_WIDTH * 3;
            for (uint32_t x = 0; x < screen_w; x++) {
                uint32_t sx = x * BOOT_IMAGE_WIDTH / screen_w;
                const uint8_t *p = srow + (uint64_t)sx * 3;
                fb_put_pixel(x, y, FB_COLOR(p[0], p[1], p[2]));
            }
        }
        return;
    }
    // Last-resort gradient if the embedded image is somehow unavailable.
    for (uint32_t y = 0; y < screen_h; y++) {
        uint8_t r = 15 + (y * 25 / screen_h);
        uint8_t g = 20 + (y * 15 / screen_h);
        uint8_t b = 35 + (y * 25 / screen_h);
        for (uint32_t x = 0; x < screen_w; x++) {
            fb_put_pixel(x, y, FB_COLOR(r, g, b));
        }
    }
}

// Simple boot screen shown before filesystem is mounted
// Shows BOOT.BMP (or gradient) with title, version, and spinner
// This matches gfx_boot_splash() style for seamless transition
void gfx_boot_simple(void) {
    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();

    g_boot_screen_active = true;

    // Draw the boot image (BOOT.BMP if loaded, gradient otherwise)
    gfx_draw_boot_image();

    // First boot screen: embedded graphic + spinner only. The version/build
    // string is intentionally NOT drawn here; it appears on the second splash
    // (gfx_boot_splash) once the filesystem has mounted.

    // Draw spinner at 70% height (same as progress bar position in gfx_boot_splash)
    int cx = w / 2;
    int cy = h * 70 / 100;
    int radius = 30;
    int dot_radius = 6;

    static const int cos_table[8] = {100, 71, 0, -71, -100, -71, 0, 71};
    static const int sin_table[8] = {0, 71, 100, 71, 0, -71, -100, -71};

    for (int i = 0; i < 8; i++) {
        int dot_x = cx + (cos_table[i] * radius) / 100;
        int dot_y = cy + (sin_table[i] * radius) / 100;
        int brightness = 255 - (i * 28);
        if (brightness < 60) brightness = 60;
        uint32_t color = FB_COLOR(brightness, brightness, brightness);  // white dots, trailing fade
        gfx_fill_circle(dot_x, dot_y, dot_radius, color);
    }

    fb_swap_buffers();
}

// Refresh boot screen after BOOT.BMP is loaded
// Call this after gfx_load_boot_image_from_disk() succeeds
void gfx_boot_refresh(void) {
    if (!g_boot_screen_active) return;

    // Redraw the entire boot screen with BOOT.BMP background
    gfx_boot_simple();
}

// Draw spinning dots animation at bottom of screen
// Uses BOOT.BMP as background (same as gfx_boot_simple)
void gfx_boot_spinner(int frame) {
    // Once the graphical boot splash has been shown, never redraw the spinner
    // over it (the progress bar + log text remain fine).
    if (g_boot_splash_shown) return;

    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();

    // Spinner position - 70% down (same as gfx_boot_simple)
    int cx = w / 2;
    int cy = h * 70 / 100;
    int radius = 30;
    int dot_radius = 6;
    int num_dots = 8;

    // Clear spinner area by redrawing boot image section
    int clear_x1 = cx - radius - dot_radius - 5;
    int clear_y1 = cy - radius - dot_radius - 5;
    int clear_w = (radius + dot_radius) * 2 + 10;
    int clear_h = (radius + dot_radius) * 2 + 10;

    // Redraw boot image in spinner area (BOOT.BMP or gradient)
    if (g_disk_boot_image_loaded && g_disk_boot_image.pixels) {
        // Sample from BOOT.BMP (disk boot image)
        for (int y = 0; y < clear_h; y++) {
            int screen_y = clear_y1 + y;
            if (screen_y < 0 || screen_y >= (int)h) continue;
            int img_y = (screen_y * g_disk_boot_image.height) / h;

            for (int x = 0; x < clear_w; x++) {
                int screen_x = clear_x1 + x;
                if (screen_x < 0 || screen_x >= (int)w) continue;
                int img_x = (screen_x * g_disk_boot_image.width) / w;

                uint32_t *pixels = (uint32_t *)g_disk_boot_image.pixels;
                uint32_t color = pixels[img_y * g_disk_boot_image.width + img_x];
                fb_put_pixel(screen_x, screen_y, color);
            }
        }
    } else {
        // Redraw from the embedded boot splash so the spinner shows the actual
        // image behind it (transparent look) instead of a gradient patch.
        for (int y = clear_y1; y < clear_y1 + clear_h && y < (int)h; y++) {
            if (y < 0) continue;
            uint32_t sy = (uint32_t)y * BOOT_IMAGE_HEIGHT / h;
            const uint8_t *srow = boot_image_data + (uint64_t)sy * BOOT_IMAGE_WIDTH * 3;
            for (int x = clear_x1; x < clear_x1 + clear_w && x < (int)w; x++) {
                if (x < 0) continue;
                uint32_t sx = (uint32_t)x * BOOT_IMAGE_WIDTH / w;
                const uint8_t *p = srow + (uint64_t)sx * 3;
                fb_put_pixel(x, y, FB_COLOR(p[0], p[1], p[2]));
            }
        }
    }

    // Draw spinning dots
    static const int cos_table[8] = {100, 71, 0, -71, -100, -71, 0, 71};
    static const int sin_table[8] = {0, 71, 100, 71, 0, -71, -100, -71};

    for (int i = 0; i < num_dots; i++) {
        int dot_x = cx + (cos_table[i] * radius) / 100;
        int dot_y = cy + (sin_table[i] * radius) / 100;

        // Brightness based on position relative to leading dot
        int leading = frame % num_dots;
        int dist = (leading - i + num_dots) % num_dots;
        int brightness = 255 - (dist * 28);
        if (brightness < 60) brightness = 60;

        uint32_t color = FB_COLOR(brightness, brightness, brightness);  // white dots, trailing fade
        gfx_fill_circle(dot_x, dot_y, dot_radius, color);
    }

    fb_swap_buffers();
}
