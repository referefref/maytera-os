// background.c - Background system implementation for MayteraOS
// Supports solid colors, gradients, patterns, and wallpaper images

#include "background.h"
#include "themes.h"
#include "../mm/heap.h"
#include "../serial.h"
#include "../string.h"
#include "../video/framebuffer.h"
#include "../fs/fat.h"

// External filesystem
extern fat_fs_t g_fat_fs;

// Current background configuration
static background_config_t g_current_bg = {
    .mode = BG_SOLID,
    .solid_color = 0xFF2E5A88,  // Default pleasant blue
    .gradient = {0},
    .pattern = {0},
    .wallpaper_path = {0}
};

// Simple pseudo-random number generator for patterns
static uint32_t g_rand_seed = 12345;

static uint32_t pattern_rand(void) {
    g_rand_seed = g_rand_seed * 1103515245 + 12345;
    return (g_rand_seed >> 16) & 0x7FFF;
}

static void pattern_srand(uint32_t seed) {
    g_rand_seed = seed;
}

// ============================================================================
// Color Utilities
// ============================================================================

// Linear interpolation between two colors
static uint32_t color_lerp(uint32_t c1, uint32_t c2, int t, int max_t) {
    if (t <= 0) return c1;
    if (t >= max_t) return c2;

    uint8_t a1 = (c1 >> 24) & 0xFF;
    uint8_t r1 = (c1 >> 16) & 0xFF;
    uint8_t g1 = (c1 >> 8) & 0xFF;
    uint8_t b1 = c1 & 0xFF;

    uint8_t a2 = (c2 >> 24) & 0xFF;
    uint8_t r2 = (c2 >> 16) & 0xFF;
    uint8_t g2 = (c2 >> 8) & 0xFF;
    uint8_t b2 = c2 & 0xFF;

    uint8_t a = a1 + ((a2 - a1) * t) / max_t;
    uint8_t r = r1 + ((r2 - r1) * t) / max_t;
    uint8_t g = g1 + ((g2 - g1) * t) / max_t;
    uint8_t b = b1 + ((b2 - b1) * t) / max_t;

    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// Convert ARGB to framebuffer format (BGRA)
static uint32_t argb_to_fb(uint32_t argb) {
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >> 8) & 0xFF;
    uint8_t b = argb & 0xFF;
    return (b << 16) | (g << 8) | r;
}

// Integer square root approximation
static uint32_t isqrt(uint32_t n) {
    if (n == 0) return 0;
    uint32_t x = n;
    uint32_t y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

// ============================================================================
// Pattern Generation Functions
// ============================================================================

void pattern_stipple(uint32_t *pixels, uint32_t width, uint32_t height,
                     uint32_t fg_color, uint32_t bg_color, int density) {
    if (!pixels || width == 0 || height == 0) return;

    // Convert to framebuffer format
    uint32_t fg = argb_to_fb(fg_color);
    uint32_t bg = argb_to_fb(bg_color);

    // Clamp density to 1-10
    if (density < 1) density = 1;
    if (density > 10) density = 10;

    // CDE-style 4x4 stipple patterns based on density
    // Higher density = more foreground pixels
    static const uint8_t stipple_masks[11][2] = {
        {0x00, 0x00},  // 0: all background (not used)
        {0x88, 0x00},  // 1: very sparse
        {0x88, 0x22},  // 2: sparse
        {0xAA, 0x00},  // 3: light
        {0xAA, 0x55},  // 4: medium-light (classic CDE)
        {0xAA, 0x55},  // 5: medium
        {0xEE, 0x55},  // 6: medium-heavy
        {0xEE, 0xBB},  // 7: heavy
        {0xFF, 0xAA},  // 8: dense
        {0xFF, 0xEE},  // 9: very dense
        {0xFF, 0xFF},  // 10: all foreground
    };

    uint8_t mask_even = stipple_masks[density][0];
    uint8_t mask_odd = stipple_masks[density][1];

    for (uint32_t y = 0; y < height; y++) {
        uint8_t row_mask = (y & 1) ? mask_odd : mask_even;
        for (uint32_t x = 0; x < width; x++) {
            // Check if this pixel should be foreground based on 4-pixel repeat
            int bit_pos = x & 3;  // 0-3 for each 4-pixel group
            bool is_fg = (row_mask >> (7 - bit_pos * 2)) & 1;
            pixels[y * width + x] = is_fg ? fg : bg;
        }
    }
}

void pattern_crosshatch(uint32_t *pixels, uint32_t width, uint32_t height,
                        uint32_t fg_color, uint32_t bg_color, int spacing) {
    if (!pixels || width == 0 || height == 0) return;

    uint32_t fg = argb_to_fb(fg_color);
    uint32_t bg = argb_to_fb(bg_color);

    if (spacing < 4) spacing = 4;
    if (spacing > 64) spacing = 64;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            // Draw line if on horizontal or vertical grid line
            bool on_line = ((x % spacing) == 0) || ((y % spacing) == 0);
            pixels[y * width + x] = on_line ? fg : bg;
        }
    }
}

void pattern_diagonal(uint32_t *pixels, uint32_t width, uint32_t height,
                      uint32_t fg_color, uint32_t bg_color, int spacing, int direction) {
    if (!pixels || width == 0 || height == 0) return;

    uint32_t fg = argb_to_fb(fg_color);
    uint32_t bg = argb_to_fb(bg_color);

    if (spacing < 4) spacing = 4;
    if (spacing > 64) spacing = 64;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            bool on_line = false;

            // Forward slash / diagonal
            if (direction == 0 || direction == 2) {
                if (((x + y) % spacing) == 0) on_line = true;
            }
            // Backslash \ diagonal
            if (direction == 1 || direction == 2) {
                if (((x + height - y) % spacing) == 0) on_line = true;
            }

            pixels[y * width + x] = on_line ? fg : bg;
        }
    }
}

void pattern_grid(uint32_t *pixels, uint32_t width, uint32_t height,
                  uint32_t fg_color, uint32_t bg_color, int spacing) {
    // Grid is just crosshatch with thicker lines
    pattern_crosshatch(pixels, width, height, fg_color, bg_color, spacing);
}

void pattern_dots(uint32_t *pixels, uint32_t width, uint32_t height,
                  uint32_t fg_color, uint32_t bg_color, int size, int spacing) {
    if (!pixels || width == 0 || height == 0) return;

    uint32_t fg = argb_to_fb(fg_color);
    uint32_t bg = argb_to_fb(bg_color);

    if (size < 2) size = 2;
    if (size > 16) size = 16;
    if (spacing < 4) spacing = 4;
    if (spacing > 64) spacing = 64;

    int total = size + spacing;
    int radius = size / 2;
    int r_sq = radius * radius;

    // Fill background first
    for (uint32_t i = 0; i < width * height; i++) {
        pixels[i] = bg;
    }

    // Draw dots at grid intersections
    for (uint32_t cy = radius; cy < height; cy += total) {
        for (uint32_t cx = radius; cx < width; cx += total) {
            // Draw filled circle at (cx, cy)
            for (int dy = -radius; dy <= radius; dy++) {
                for (int dx = -radius; dx <= radius; dx++) {
                    if (dx * dx + dy * dy <= r_sq) {
                        int px = cx + dx;
                        int py = cy + dy;
                        if (px >= 0 && px < (int)width && py >= 0 && py < (int)height) {
                            pixels[py * width + px] = fg;
                        }
                    }
                }
            }
        }
    }
}

void pattern_checkerboard(uint32_t *pixels, uint32_t width, uint32_t height,
                          uint32_t color1, uint32_t color2, int size) {
    if (!pixels || width == 0 || height == 0) return;

    uint32_t c1 = argb_to_fb(color1);
    uint32_t c2 = argb_to_fb(color2);

    if (size < 2) size = 2;
    if (size > 64) size = 64;

    for (uint32_t y = 0; y < height; y++) {
        int row_phase = (y / size) & 1;
        for (uint32_t x = 0; x < width; x++) {
            int col_phase = (x / size) & 1;
            pixels[y * width + x] = (row_phase ^ col_phase) ? c1 : c2;
        }
    }
}

void pattern_noise(uint32_t *pixels, uint32_t width, uint32_t height,
                   uint32_t base_color, int intensity) {
    if (!pixels || width == 0 || height == 0) return;

    if (intensity < 1) intensity = 1;
    if (intensity > 50) intensity = 50;

    uint8_t base_r = (base_color >> 16) & 0xFF;
    uint8_t base_g = (base_color >> 8) & 0xFF;
    uint8_t base_b = base_color & 0xFF;

    pattern_srand(42);  // Fixed seed for reproducibility

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            // Random offset for each channel
            int offset = (int)(pattern_rand() % (intensity * 2 + 1)) - intensity;

            int r = base_r + offset;
            int g = base_g + offset;
            int b = base_b + offset;

            // Clamp to valid range
            if (r < 0) r = 0;
            if (r > 255) r = 255;
            if (g < 0) g = 0;
            if (g > 255) g = 255;
            if (b < 0) b = 0;
            if (b > 255) b = 255;

            // Store in framebuffer format (BGR)
            pixels[y * width + x] = (b << 16) | (g << 8) | r;
        }
    }
}

void pattern_weave(uint32_t *pixels, uint32_t width, uint32_t height,
                   uint32_t fg_color, uint32_t bg_color, int size) {
    if (!pixels || width == 0 || height == 0) return;

    uint32_t fg = argb_to_fb(fg_color);
    uint32_t bg = argb_to_fb(bg_color);

    if (size < 4) size = 4;
    if (size > 32) size = 32;

    // Create a woven/interlaced pattern
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            int cell_x = x / size;
            int cell_y = y / size;
            int in_x = x % size;
            int in_y = y % size;

            // Alternate horizontal and vertical stripes in checkerboard pattern
            bool vert_stripe = (cell_x + cell_y) & 1;
            bool on_stripe;

            if (vert_stripe) {
                // Vertical stripe: draw if in middle half vertically
                on_stripe = (in_x >= size / 4 && in_x < 3 * size / 4);
            } else {
                // Horizontal stripe: draw if in middle half horizontally
                on_stripe = (in_y >= size / 4 && in_y < 3 * size / 4);
            }

            pixels[y * width + x] = on_stripe ? fg : bg;
        }
    }
}

// ============================================================================
// Gradient Generation Functions
// ============================================================================

void gradient_vertical(uint32_t *pixels, uint32_t width, uint32_t height,
                       uint32_t top_color, uint32_t bottom_color) {
    if (!pixels || width == 0 || height == 0) return;

    for (uint32_t y = 0; y < height; y++) {
        uint32_t color = color_lerp(top_color, bottom_color, y, height - 1);
        uint32_t fb_color = argb_to_fb(color);

        for (uint32_t x = 0; x < width; x++) {
            pixels[y * width + x] = fb_color;
        }
    }
}

void gradient_horizontal(uint32_t *pixels, uint32_t width, uint32_t height,
                         uint32_t left_color, uint32_t right_color) {
    if (!pixels || width == 0 || height == 0) return;

    for (uint32_t x = 0; x < width; x++) {
        uint32_t color = color_lerp(left_color, right_color, x, width - 1);
        uint32_t fb_color = argb_to_fb(color);

        for (uint32_t y = 0; y < height; y++) {
            pixels[y * width + x] = fb_color;
        }
    }
}

void gradient_radial(uint32_t *pixels, uint32_t width, uint32_t height,
                     uint32_t center_color, uint32_t edge_color) {
    if (!pixels || width == 0 || height == 0) return;

    int cx = width / 2;
    int cy = height / 2;

    // Calculate maximum distance (corner to center)
    uint32_t max_dist = isqrt(cx * cx + cy * cy);
    if (max_dist == 0) max_dist = 1;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            int dx = x - cx;
            int dy = y - cy;
            uint32_t dist = isqrt(dx * dx + dy * dy);

            uint32_t color = color_lerp(center_color, edge_color, dist, max_dist);
            pixels[y * width + x] = argb_to_fb(color);
        }
    }
}

void gradient_diagonal(uint32_t *pixels, uint32_t width, uint32_t height,
                       uint32_t start_color, uint32_t end_color) {
    if (!pixels || width == 0 || height == 0) return;

    uint32_t max_dist = width + height;

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t dist = x + y;
            uint32_t color = color_lerp(start_color, end_color, dist, max_dist);
            pixels[y * width + x] = argb_to_fb(color);
        }
    }
}

// ============================================================================
// Background API Implementation
// ============================================================================

void background_init(void) {
    // Set default configuration
    memset(&g_current_bg, 0, sizeof(g_current_bg));
    g_current_bg.mode = BG_SOLID;
    g_current_bg.solid_color = 0xFF2E5A88;  // Pleasant blue default

    kprintf("[Background] Background system initialized\n");
}

void background_set_config(const background_config_t *config) {
    if (!config) return;
    memcpy(&g_current_bg, config, sizeof(background_config_t));
    kprintf("[Background] Configuration updated, mode=%d\n", config->mode);
}

const background_config_t *background_get_config(void) {
    return &g_current_bg;
}

void background_set_solid(uint32_t color) {
    g_current_bg.mode = BG_SOLID;
    g_current_bg.solid_color = color;
}

void background_set_gradient_v(uint32_t top_color, uint32_t bottom_color) {
    g_current_bg.mode = BG_GRADIENT_V;
    g_current_bg.gradient.color1 = top_color;
    g_current_bg.gradient.color2 = bottom_color;
}

void background_set_gradient_h(uint32_t left_color, uint32_t right_color) {
    g_current_bg.mode = BG_GRADIENT_H;
    g_current_bg.gradient.color1 = left_color;
    g_current_bg.gradient.color2 = right_color;
}

void background_set_gradient_radial(uint32_t center_color, uint32_t edge_color) {
    g_current_bg.mode = BG_GRADIENT_RADIAL;
    g_current_bg.gradient.color1 = center_color;
    g_current_bg.gradient.color2 = edge_color;
}

void background_set_pattern(pattern_type_t type, uint32_t fg, uint32_t bg, int density) {
    g_current_bg.mode = BG_PATTERN;
    g_current_bg.pattern.type = type;
    g_current_bg.pattern.fg_color = fg;
    g_current_bg.pattern.bg_color = bg;
    g_current_bg.pattern.density = density;
    g_current_bg.pattern.size = 8;
    g_current_bg.pattern.spacing = 8;
}

void background_set_wallpaper(const char *path, bg_mode_t mode) {
    if (!path) return;
    g_current_bg.mode = mode;
    strncpy(g_current_bg.wallpaper_path, path, sizeof(g_current_bg.wallpaper_path) - 1);
    g_current_bg.wallpaper_path[sizeof(g_current_bg.wallpaper_path) - 1] = '\0';
}

image_t *background_generate(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return NULL;

    // Allocate image structure
    image_t *img = (image_t *)kmalloc(sizeof(image_t));
    if (!img) {
        kprintf("[Background] Failed to allocate image structure\n");
        return NULL;
    }

    // Allocate pixel buffer
    size_t pixel_size = width * height * sizeof(uint32_t);
    img->pixels = (uint32_t *)kmalloc(pixel_size);
    if (!img->pixels) {
        kprintf("[Background] Failed to allocate pixel buffer (%u bytes)\n", pixel_size);
        kfree(img);
        return NULL;
    }

    img->width = width;
    img->height = height;

    // Generate based on mode
    switch (g_current_bg.mode) {
        case BG_SOLID: {
            uint32_t fb_color = argb_to_fb(g_current_bg.solid_color);
            for (uint32_t i = 0; i < width * height; i++) {
                img->pixels[i] = fb_color;
            }
            break;
        }

        case BG_GRADIENT_V:
            gradient_vertical(img->pixels, width, height,
                             g_current_bg.gradient.color1,
                             g_current_bg.gradient.color2);
            break;

        case BG_GRADIENT_H:
            gradient_horizontal(img->pixels, width, height,
                               g_current_bg.gradient.color1,
                               g_current_bg.gradient.color2);
            break;

        case BG_GRADIENT_RADIAL:
            gradient_radial(img->pixels, width, height,
                           g_current_bg.gradient.color1,
                           g_current_bg.gradient.color2);
            break;

        case BG_PATTERN:
            switch (g_current_bg.pattern.type) {
                case PATTERN_STIPPLE:
                    pattern_stipple(img->pixels, width, height,
                                   g_current_bg.pattern.fg_color,
                                   g_current_bg.pattern.bg_color,
                                   g_current_bg.pattern.density);
                    break;
                case PATTERN_CROSSHATCH:
                    pattern_crosshatch(img->pixels, width, height,
                                      g_current_bg.pattern.fg_color,
                                      g_current_bg.pattern.bg_color,
                                      g_current_bg.pattern.spacing);
                    break;
                case PATTERN_DIAGONAL:
                    pattern_diagonal(img->pixels, width, height,
                                    g_current_bg.pattern.fg_color,
                                    g_current_bg.pattern.bg_color,
                                    g_current_bg.pattern.spacing, 2);
                    break;
                case PATTERN_GRID:
                    pattern_grid(img->pixels, width, height,
                                g_current_bg.pattern.fg_color,
                                g_current_bg.pattern.bg_color,
                                g_current_bg.pattern.spacing);
                    break;
                case PATTERN_DOTS:
                    pattern_dots(img->pixels, width, height,
                                g_current_bg.pattern.fg_color,
                                g_current_bg.pattern.bg_color,
                                g_current_bg.pattern.size,
                                g_current_bg.pattern.spacing);
                    break;
                case PATTERN_CHECKERBOARD:
                    pattern_checkerboard(img->pixels, width, height,
                                        g_current_bg.pattern.fg_color,
                                        g_current_bg.pattern.bg_color,
                                        g_current_bg.pattern.size);
                    break;
                case PATTERN_NOISE:
                    pattern_noise(img->pixels, width, height,
                                 g_current_bg.pattern.bg_color,
                                 g_current_bg.pattern.density * 5);
                    break;
                case PATTERN_WEAVE:
                    pattern_weave(img->pixels, width, height,
                                 g_current_bg.pattern.fg_color,
                                 g_current_bg.pattern.bg_color,
                                 g_current_bg.pattern.size);
                    break;
            }
            break;

        default:
            // For wallpaper modes, generate solid color as fallback
            // Actual wallpaper loading should be done separately
            {
                uint32_t fb_color = argb_to_fb(g_current_bg.solid_color);
                for (uint32_t i = 0; i < width * height; i++) {
                    img->pixels[i] = fb_color;
                }
            }
            break;
    }

    return img;
}

void background_render(uint32_t width, uint32_t height) {
    // Generate to temporary buffer then blit to framebuffer
    image_t *img = background_generate(width, height);
    if (!img) return;

    // Blit to framebuffer
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            fb_put_pixel(x, y, img->pixels[y * width + x]);
        }
    }

    // Free temporary buffer
    kfree(img->pixels);
    kfree(img);
}

// ============================================================================
// Theme-Specific Backgrounds
// ============================================================================

void background_get_theme_default(int theme_id, background_config_t *config) {
    if (!config) return;

    memset(config, 0, sizeof(background_config_t));

    switch (theme_id) {
        case THEME_DEFAULT:
            // Modern dark grey with subtle vertical gradient
            config->mode = BG_GRADIENT_V;
            config->gradient.color1 = 0xFF4A90C2;  // Sky blue top
            config->gradient.color2 = 0xFF1E5A8A;  // Ocean blue bottom
            break;

        case THEME_DARK:
            // Deep dark blue gradient
            config->mode = BG_GRADIENT_V;
            config->gradient.color1 = 0xFF101820;  // Very dark blue
            config->gradient.color2 = 0xFF080C10;  // Almost black
            break;

        case THEME_LIGHT:
            // Light sky gradient
            config->mode = BG_GRADIENT_V;
            config->gradient.color1 = 0xFFE8F0F8;  // Very light blue
            config->gradient.color2 = 0xFFC8D8E8;  // Slightly darker blue
            break;

        case THEME_HIGH_CONTRAST:
            // Solid black
            config->mode = BG_SOLID;
            config->solid_color = 0xFF000000;
            break;

        case THEME_CLASSIC:
            // Classic teal (Windows 95 style)
            config->mode = BG_SOLID;
            config->solid_color = 0xFF008080;
            break;

        case THEME_OCEAN:
            // Deep ocean gradient
            config->mode = BG_GRADIENT_V;
            config->gradient.color1 = 0xFF306090;  // Ocean blue
            config->gradient.color2 = 0xFF102040;  // Deep ocean
            break;

        case THEME_SUNSET:
            // Warm sunset gradient
            config->mode = BG_GRADIENT_V;
            config->gradient.color1 = 0xFFD06830;  // Sunset orange
            config->gradient.color2 = 0xFF401810;  // Dark brown
            break;

        case THEME_FOREST:
            // Forest green gradient
            config->mode = BG_GRADIENT_V;
            config->gradient.color1 = 0xFF306830;  // Forest green
            config->gradient.color2 = 0xFF102010;  // Very dark green
            break;

        case THEME_MODERN_LIGHT:
            // macOS Big Sur light - Sky blue gradient
            config->mode = BG_GRADIENT_RADIAL;
            config->gradient.color1 = 0xFF8BC8F2;  // Light blue center
            config->gradient.color2 = 0xFF5BADE9;  // Deeper blue edges
            break;

        case THEME_MODERN_DARK:
            // macOS Big Sur dark - Deep purple-blue
            config->mode = BG_GRADIENT_RADIAL;
            config->gradient.color1 = 0xFF2C2C40;  // Dark purple center
            config->gradient.color2 = 0xFF1C1C28;  // Darker edges
            break;

        case THEME_FLUENT_LIGHT:
            // Windows 11 light - Subtle blue-grey
            config->mode = BG_GRADIENT_V;
            config->gradient.color1 = 0xFFD8E8F8;  // Light blue-grey
            config->gradient.color2 = 0xFFA8C8E8;  // Slightly darker
            break;

        case THEME_FLUENT_DARK:
            // Windows 11 dark - Dark mode gradient
            config->mode = BG_GRADIENT_V;
            config->gradient.color1 = 0xFF202428;  // Dark grey-blue
            config->gradient.color2 = 0xFF101418;  // Darker
            break;

        default:
            // Fallback to solid blue
            config->mode = BG_SOLID;
            config->solid_color = 0xFF2E5A88;
            break;
    }
}

image_t *background_generate_for_theme(int theme_id, uint32_t width, uint32_t height) {
    background_config_t config;
    background_get_theme_default(theme_id, &config);

    // Temporarily save and restore current config
    background_config_t saved = g_current_bg;
    g_current_bg = config;

    image_t *img = background_generate(width, height);

    g_current_bg = saved;

    return img;
}

// ============================================================================
// BMP File Generation
// ============================================================================

int background_save_bmp(const char *path, const background_config_t *config,
                        uint32_t width, uint32_t height) {
    if (!path || !config || width == 0 || height == 0) return -1;

    // BMP file structure:
    // - File header (14 bytes)
    // - Info header (40 bytes)
    // - Pixel data (width * height * 3 bytes for 24-bit)

    uint32_t row_stride = (width * 3 + 3) & ~3;  // Rows must be 4-byte aligned
    uint32_t pixel_data_size = row_stride * height;
    uint32_t file_size = 14 + 40 + pixel_data_size;

    // Allocate file buffer
    uint8_t *bmp_data = (uint8_t *)kmalloc(file_size);
    if (!bmp_data) {
        kprintf("[Background] Failed to allocate BMP buffer\n");
        return -1;
    }

    memset(bmp_data, 0, file_size);

    // Generate the background image
    background_config_t saved = g_current_bg;
    g_current_bg = *config;
    image_t *img = background_generate(width, height);
    g_current_bg = saved;

    if (!img) {
        kfree(bmp_data);
        return -1;
    }

    // BMP File Header (14 bytes)
    bmp_data[0] = 'B';
    bmp_data[1] = 'M';
    *(uint32_t *)&bmp_data[2] = file_size;
    *(uint32_t *)&bmp_data[6] = 0;  // Reserved
    *(uint32_t *)&bmp_data[10] = 14 + 40;  // Data offset

    // BMP Info Header (BITMAPINFOHEADER - 40 bytes)
    *(uint32_t *)&bmp_data[14] = 40;  // Header size
    *(int32_t *)&bmp_data[18] = width;
    *(int32_t *)&bmp_data[22] = height;  // Positive = bottom-up
    *(uint16_t *)&bmp_data[26] = 1;  // Planes
    *(uint16_t *)&bmp_data[28] = 24;  // Bits per pixel
    *(uint32_t *)&bmp_data[30] = 0;  // Compression (none)
    *(uint32_t *)&bmp_data[34] = pixel_data_size;  // Image size
    *(int32_t *)&bmp_data[38] = 2835;  // X pixels per meter (~72 DPI)
    *(int32_t *)&bmp_data[42] = 2835;  // Y pixels per meter
    *(uint32_t *)&bmp_data[46] = 0;  // Colors used
    *(uint32_t *)&bmp_data[50] = 0;  // Important colors

    // Pixel data (bottom-up, BGR order)
    uint8_t *pixel_ptr = bmp_data + 54;
    for (int y = height - 1; y >= 0; y--) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t pixel = img->pixels[y * width + x];
            // Framebuffer is BGRA, BMP expects BGR
            *pixel_ptr++ = (pixel >> 16) & 0xFF;  // B
            *pixel_ptr++ = (pixel >> 8) & 0xFF;   // G
            *pixel_ptr++ = pixel & 0xFF;          // R
        }
        // Pad row to 4-byte boundary
        while ((pixel_ptr - (bmp_data + 54)) % 4 != 0) {
            *pixel_ptr++ = 0;
        }
    }

    // Write to filesystem (if available)
    // Note: This requires filesystem write support which may not be implemented
    kprintf("[Background] BMP generation complete: %s (%ux%u, %u bytes)\n",
            path, width, height, file_size);

    // Clean up
    kfree(img->pixels);
    kfree(img);
    kfree(bmp_data);

    return 0;
}

int background_load_wallpaper(const char *path, bg_mode_t mode) {
    if (!path) return -1;

    if (!g_fat_fs.mounted) {
        kprintf("[Background] Filesystem not mounted\n");
        return -1;
    }

    g_current_bg.mode = mode;
    strncpy(g_current_bg.wallpaper_path, path, sizeof(g_current_bg.wallpaper_path) - 1);

    kprintf("[Background] Wallpaper set: %s, mode=%d\n", path, mode);
    return 0;
}
