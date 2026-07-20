// framebuffer.c - Linear framebuffer driver with double buffering
#include "framebuffer.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"

// Framebuffer state
static uint32_t *fb_front = NULL;      // Front buffer (actual display)
static uint32_t *fb_back = NULL;       // Back buffer (drawing target)
static uint32_t *fb_addr = NULL;       // Current drawing target
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch = 0;          // Bytes per line
static uint32_t fb_bpp = 0;            // Bits per pixel
static int fb_is_bgr = 0;

// Global variables for external access (fb_syscall.c)
uint64_t g_fb_phys_addr = 0;
uint32_t g_fb_width = 0;
uint32_t g_fb_height = 0;
uint32_t g_fb_pitch = 0;
uint32_t g_fb_bpp = 0;
static bool fb_double_buffered = false;
// Bochs VGA (stdvga) DISPI register definitions
#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF
#define VBE_DISPI_INDEX_XRES    0x01
#define VBE_DISPI_INDEX_YRES    0x02
#define VBE_DISPI_INDEX_BPP     0x03
#define VBE_DISPI_INDEX_ENABLE  0x04
#define VBE_DISPI_INDEX_BANK    0x05
#define VBE_DISPI_INDEX_VIRT_WIDTH  0x06
#define VBE_DISPI_INDEX_VIRT_HEIGHT 0x07
#define VBE_DISPI_INDEX_X_OFFSET    0x08
#define VBE_DISPI_INDEX_Y_OFFSET    0x09

static uint16_t bochs_vbe_read(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static void bochs_vbe_write(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

// Read and fix Bochs VGA display alignment after fb_init
static void fb_dump_vga_crtc(void);
static void fb_fix_bochs_alignment(void) {
    uint16_t xres = bochs_vbe_read(VBE_DISPI_INDEX_XRES);
    uint16_t yres = bochs_vbe_read(VBE_DISPI_INDEX_YRES);
    uint16_t bpp = bochs_vbe_read(VBE_DISPI_INDEX_BPP);
    uint16_t enable = bochs_vbe_read(VBE_DISPI_INDEX_ENABLE);
    uint16_t virt_w = bochs_vbe_read(VBE_DISPI_INDEX_VIRT_WIDTH);
    uint16_t virt_h = bochs_vbe_read(VBE_DISPI_INDEX_VIRT_HEIGHT);
    uint16_t x_off = bochs_vbe_read(VBE_DISPI_INDEX_X_OFFSET);
    uint16_t y_off = bochs_vbe_read(VBE_DISPI_INDEX_Y_OFFSET);

    kprintf("[FB] Bochs VBE: xres=%u yres=%u bpp=%u enable=0x%x\n",
            xres, yres, bpp, enable);
    kprintf("[FB] Bochs VBE: virt_w=%u virt_h=%u x_off=%u y_off=%u\n",
            virt_w, virt_h, x_off, y_off);

    // Fix: ensure virtual width matches actual width and offsets are zero
    int fixed = 0;
    if (virt_w != fb_width) {
        kprintf("[FB] Fixing virt_width: %u -> %u\n", virt_w, (unsigned)fb_width);
        bochs_vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)fb_width);
        fixed = 1;
    }
    if (x_off != 0) {
        kprintf("[FB] Fixing x_offset: %u -> 0\n", x_off);
        bochs_vbe_write(VBE_DISPI_INDEX_X_OFFSET, 0);
        fixed = 1;
    }
    if (y_off != 0) {
        kprintf("[FB] Fixing y_offset: %u -> 0\n", y_off);
        bochs_vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
        fixed = 1;
    }
    if (fixed) {
        kprintf("[FB] Bochs VBE display alignment corrected\n");
    }

    // Also check legacy VGA CRTC registers
    fb_dump_vga_crtc();
}

// Initialize framebuffer
void fb_init(framebuffer_info_t *info) {
    if (!info || info->address == 0) {
        kprintf("[FB] ERROR: Invalid framebuffer info\n");
        return;
    }

    fb_front = (uint32_t *)info->address;
    fb_width = info->width;
    fb_height = info->height;
    fb_pitch = info->pitch;
    fb_bpp = info->bpp;
    fb_is_bgr = (info->pixel_format == PIXEL_FORMAT_BGR);

    // Set global variables for external access
    g_fb_phys_addr = info->address;
    g_fb_width = fb_width;
    g_fb_height = fb_height;
    g_fb_pitch = fb_pitch;
    g_fb_bpp = fb_bpp;

    // Allocate back buffer for double buffering
    uint32_t buffer_size = fb_height * fb_pitch;
    fb_back = (uint32_t *)kmalloc_aligned(buffer_size, 4096);  // page-align so sys_fb_map mapping matches exactly (#72)

    if (fb_back) {
        // Explicitly zero the back buffer to avoid garbage data
        memset(fb_back, 0, buffer_size);
        fb_double_buffered = true;
        fb_addr = fb_back;  // Draw to back buffer
        kprintf("[FB] Double buffering enabled (%u KB back buffer)\n", buffer_size / 1024);

        // Also clear the front buffer (hardware framebuffer) to remove UEFI graphics
        memset(fb_front, 0, buffer_size);
    } else {
        fb_double_buffered = false;
        fb_addr = fb_front;  // Fall back to single buffer
        kprintf("[FB] Warning: Could not allocate back buffer, using single buffer\n");
    }

    kprintf("[FB] Framebuffer initialized: %ux%u pitch=%u ppsl=%u @ 0x%lx\n",
            fb_width, fb_height, fb_pitch, fb_pitch/4, (uint64_t)fb_front);

    // Fix Bochs VGA display alignment (prevents left-side content appearing on right)
    fb_fix_bochs_alignment();
}

// Getters
uint32_t fb_get_width(void) { return fb_width; }
uint32_t fb_get_height(void) { return fb_height; }
uint32_t fb_get_pitch(void) { return fb_pitch; }
uint32_t *fb_get_back_buffer(void) { return fb_back; }
uint32_t fb_get_bpp(void) { return fb_bpp; }

// Put a pixel at (x, y)
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_addr || x >= fb_width || y >= fb_height) return;

    // Calculate offset (pitch is in bytes, we're using 32-bit pointer)
    uint32_t *pixel = (uint32_t *)((uint8_t *)fb_addr + y * fb_pitch + x * 4);
    *pixel = color;
}

// Write a horizontal run of `count` pixels at (x,y) into the active buffer with
// a single bounds-clamp + memcpy, instead of `count` fb_put_pixel() calls. Used
// by row-oriented blitters (scaled image blit, etc.) for a big speedup.
void fb_put_row(uint32_t x, uint32_t y, uint32_t count, const uint32_t *pixels) {
    if (!fb_addr || !pixels || y >= fb_height || x >= fb_width) return;
    if (x + count > fb_width) count = fb_width - x;
    uint32_t *dst = (uint32_t *)((uint8_t *)fb_addr + y * fb_pitch + x * 4);
    memcpy(dst, pixels, (size_t)count * 4);
}

// Get pixel at (x, y)
uint32_t fb_get_pixel(uint32_t x, uint32_t y) {
    if (!fb_addr || x >= fb_width || y >= fb_height) return 0;

    uint32_t *pixel = (uint32_t *)((uint8_t *)fb_addr + y * fb_pitch + x * 4);
    return *pixel;
}

// Clear entire screen
void fb_clear(uint32_t color) {
    if (!fb_addr) return;

    for (uint32_t y = 0; y < fb_height; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)fb_addr + y * fb_pitch);
        for (uint32_t x = 0; x < fb_width; x++) {
            row[x] = color;
        }
    }
}

// Fill a rectangle
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb_addr) return;

    // Handle negative coordinates (signed int32_t cast to uint32_t)
    int32_t sx = (int32_t)x, sy = (int32_t)y;
    int32_t sw = (int32_t)w, sh = (int32_t)h;
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sw <= 0 || sh <= 0) return;
    x = (uint32_t)sx; y = (uint32_t)sy;
    w = (uint32_t)sw; h = (uint32_t)sh;

    // Clip to screen
    if (x >= fb_width || y >= fb_height) return;
    if (x + w > fb_width) w = fb_width - x;
    if (y + h > fb_height) h = fb_height - y;

    for (uint32_t row = y; row < y + h; row++) {
        uint32_t *pixel = (uint32_t *)((uint8_t *)fb_addr + row * fb_pitch + x * 4);
        for (uint32_t col = 0; col < w; col++) {
            pixel[col] = color;
        }
    }
}

// Draw rectangle outline
void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb_addr || w == 0 || h == 0) return;

    // Use signed coords to handle negative positions
    int32_t sx = (int32_t)x, sy = (int32_t)y;
    int32_t sw = (int32_t)w, sh = (int32_t)h;

    int32_t x_start = sx < 0 ? 0 : sx;
    int32_t x_end = sx + sw;
    if (x_end > (int32_t)fb_width) x_end = (int32_t)fb_width;

    // Top and bottom lines
    for (int32_t i = x_start; i < x_end; i++) {
        if (sy >= 0 && sy < (int32_t)fb_height)
            fb_put_pixel(i, sy, color);
        int32_t bottom = sy + sh - 1;
        if (bottom >= 0 && bottom < (int32_t)fb_height)
            fb_put_pixel(i, bottom, color);
    }

    // Left and right lines
    int32_t y_start = sy < 0 ? 0 : sy;
    int32_t y_end = sy + sh;
    if (y_end > (int32_t)fb_height) y_end = (int32_t)fb_height;

    for (int32_t i = y_start; i < y_end; i++) {
        if (sx >= 0 && sx < (int32_t)fb_width)
            fb_put_pixel(sx, i, color);
        int32_t right = sx + sw - 1;
        if (right >= 0 && right < (int32_t)fb_width)
            fb_put_pixel(right, i, color);
    }
}

// Draw line using Bresenham's algorithm
void fb_draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color) {
    if (!fb_addr) return;

    int32_t dx = x2 > x1 ? x2 - x1 : x1 - x2;
    int32_t dy = y2 > y1 ? y2 - y1 : y1 - y2;
    int32_t sx = x1 < x2 ? 1 : -1;
    int32_t sy = y1 < y2 ? 1 : -1;
    int32_t err = dx - dy;

    while (1) {
        if (x1 >= 0 && x1 < (int32_t)fb_width &&
            y1 >= 0 && y1 < (int32_t)fb_height) {
            fb_put_pixel(x1, y1, color);
        }

        if (x1 == x2 && y1 == y2) break;

        int32_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x1 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y1 += sy;
        }
    }
}

// Scroll screen up by n lines (in pixels)
void fb_scroll(uint32_t lines, uint32_t bg_color) {
    if (!fb_addr || lines == 0 || lines >= fb_height) {
        fb_clear(bg_color);
        return;
    }

    // Move screen content up
    uint32_t bytes_to_copy = (fb_height - lines) * fb_pitch;
    memmove(fb_addr, (uint8_t *)fb_addr + lines * fb_pitch, bytes_to_copy);

    // Clear the bottom area
    fb_fill_rect(0, fb_height - lines, fb_width, lines, bg_color);
}

// Blit raw pixel data
void fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *data) {
    if (!fb_addr || !data) return;

    // Handle negative coordinates (signed int32_t cast to uint32_t)
    int32_t sx = (int32_t)x, sy = (int32_t)y;
    int32_t sw = (int32_t)w, sh = (int32_t)h;
    int32_t src_x = 0, src_y = 0;

    if (sx < 0) { src_x = -sx; sw += sx; sx = 0; }
    if (sy < 0) { src_y = -sy; sh += sy; sy = 0; }
    if (sw <= 0 || sh <= 0) return;

    for (int32_t row = 0; row < sh && sy + row < (int32_t)fb_height; row++) {
        for (int32_t col = 0; col < sw && sx + col < (int32_t)fb_width; col++) {
            fb_put_pixel(sx + col, sy + row, data[(src_y + row) * (int32_t)w + (src_x + col)]);
        }
    }
}

// Swap back buffer to front buffer (present frame)
void fb_swap_buffers(void) {
    if (!fb_double_buffered || !fb_back || !fb_front) {
        return;  // Nothing to swap in single buffer mode
    }

    // Copy back buffer to front buffer
    uint32_t buffer_size = fb_height * fb_pitch;
    memcpy(fb_front, fb_back, buffer_size);
}

// Swap only dirty rectangles (optimized partial update)
void fb_swap_dirty_rects(const void *dirty_rects, uint32_t count, bool full_redraw) {
    if (!fb_double_buffered || !fb_back || !fb_front) {
        return;
    }
    
    // If full redraw requested or no dirty rects, do full swap
    if (full_redraw || count == 0 || dirty_rects == NULL) {
        uint32_t buffer_size = fb_height * fb_pitch;
        memcpy(fb_front, fb_back, buffer_size);
        return;
    }
    
    // Cast to rect structure (x, y, width, height as int32_t)
    typedef struct { int32_t x, y, width, height; } rect_t;
    const rect_t *rects = (const rect_t *)dirty_rects;
    
    // Copy only the dirty rectangles
    for (uint32_t i = 0; i < count; i++) {
        int32_t x = rects[i].x;
        int32_t y = rects[i].y;
        int32_t w = rects[i].width;
        int32_t h = rects[i].height;
        
        // Clamp to screen bounds
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > (int32_t)fb_width) w = fb_width - x;
        if (y + h > (int32_t)fb_height) h = fb_height - y;
        
        // Skip invalid rectangles
        if (w <= 0 || h <= 0) continue;
        
        // Copy each row of the dirty rectangle
        uint32_t bytes_per_row = w * 4;  // 4 bytes per pixel (BGRA)
        for (int32_t row = 0; row < h; row++) {
            uint32_t offset = ((y + row) * fb_pitch) + (x * 4);
            memcpy((uint8_t*)fb_front + offset, (uint8_t*)fb_back + offset, bytes_per_row);
        }
    }
}

// Set direct mode - draw directly to front buffer (for boot splash)
void fb_set_direct_mode(bool direct) {
    // Note: Direct mode was found to not work reliably. Use fb_swap_buffers() instead.
    // This function is kept for backward compatibility but does nothing useful.
    if (direct) {
        fb_addr = fb_front;
    } else {
        if (fb_double_buffered && fb_back) {
            fb_addr = fb_back;
        }
    }
}

// ============================================================================
// Alpha Blending Operations
// ============================================================================

// Blend a pixel with the background using alpha (0-255, 0=transparent, 255=opaque)
void fb_blend_pixel(uint32_t x, uint32_t y, uint32_t color, uint8_t alpha) {
    if ((int32_t)x < 0 || (int32_t)y < 0) return;
    if (x >= fb_width || y >= fb_height) return;
    if (alpha == 255) {
        fb_put_pixel(x, y, color);
        return;
    }
    if (alpha == 0) return;  // Fully transparent
    
    // Get existing pixel
    uint32_t bg = fb_get_pixel(x, y);
    
    // Extract RGB components
    uint32_t bg_r = (bg >> 16) & 0xFF;
    uint32_t bg_g = (bg >> 8) & 0xFF;
    uint32_t bg_b = bg & 0xFF;
    
    uint32_t fg_r = (color >> 16) & 0xFF;
    uint32_t fg_g = (color >> 8) & 0xFF;
    uint32_t fg_b = color & 0xFF;
    
    // Blend: result = bg * (255-alpha)/255 + fg * alpha/255
    uint32_t inv_alpha = 255 - alpha;
    uint32_t r = (bg_r * inv_alpha + fg_r * alpha) / 255;
    uint32_t g = (bg_g * inv_alpha + fg_g * alpha) / 255;
    uint32_t b = (bg_b * inv_alpha + fg_b * alpha) / 255;
    
    fb_put_pixel(x, y, (r << 16) | (g << 8) | b);
}

// Fill a rectangle with alpha blending
void fb_fill_rect_alpha(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color, uint8_t alpha) {
    if (alpha == 255) {
        fb_fill_rect(x, y, w, h, color);
        return;
    }
    if (alpha == 0) return;  // Fully transparent
    
    // Handle negative coordinates (signed int32_t cast to uint32_t)
    {
        int32_t sx = (int32_t)x, sy = (int32_t)y;
        int32_t sw = (int32_t)w, sh = (int32_t)h;
        if (sx < 0) { sw += sx; sx = 0; }
        if (sy < 0) { sh += sy; sy = 0; }
        if (sw <= 0 || sh <= 0) return;
        x = (uint32_t)sx; y = (uint32_t)sy;
        w = (uint32_t)sw; h = (uint32_t)sh;
    }

    // Clip to framebuffer bounds
    if (x >= fb_width || y >= fb_height) return;
    if (x + w > fb_width) w = fb_width - x;
    if (y + h > fb_height) h = fb_height - y;
    
    // Extract foreground RGB
    uint32_t fg_r = (color >> 16) & 0xFF;
    uint32_t fg_g = (color >> 8) & 0xFF;
    uint32_t fg_b = color & 0xFF;
    uint32_t inv_alpha = 255 - alpha;
    
    // Pre-multiply foreground
    fg_r = fg_r * alpha;
    fg_g = fg_g * alpha;
    fg_b = fg_b * alpha;
    
    uint32_t *fb = fb_back;  // Use back buffer
    for (uint32_t dy = 0; dy < h; dy++) {
        uint32_t *row = fb + (y + dy) * (fb_pitch / 4) + x;
        for (uint32_t dx = 0; dx < w; dx++) {
            uint32_t bg = row[dx];
            uint32_t bg_r = (bg >> 16) & 0xFF;
            uint32_t bg_g = (bg >> 8) & 0xFF;
            uint32_t bg_b = bg & 0xFF;
            
            uint32_t r = (bg_r * inv_alpha + fg_r) / 255;
            uint32_t g = (bg_g * inv_alpha + fg_g) / 255;
            uint32_t b = (bg_b * inv_alpha + fg_b) / 255;
            
            row[dx] = (r << 16) | (g << 8) | b;
        }
    }
}


// Debug: read and log VGA CRTC registers that affect display width
static void fb_dump_vga_crtc(void) {
    // Read CRTC Offset register (index 0x13) - logical line width
    outb(0x3D4, 0x13);
    uint8_t crtc_offset = inb(0x3D5);

    // Read CRTC Start Address High (0x0C) and Low (0x0D)
    outb(0x3D4, 0x0C);
    uint8_t start_hi = inb(0x3D5);
    outb(0x3D4, 0x0D);
    uint8_t start_lo = inb(0x3D5);
    uint16_t start_addr = (uint16_t)((start_hi << 8) | start_lo);

    // Read Attribute Controller Horizontal Pixel Panning (index 0x13)
    // Must reset flip-flop first by reading 0x3DA
    inb(0x3DA);
    outb(0x3C0, 0x13 | 0x20);  // Index 0x13, keep palette on
    uint8_t pixel_pan = inb(0x3C1);

    // Read CRTC H Total (0x00), H Display End (0x01)
    outb(0x3D4, 0x00);
    uint8_t h_total = inb(0x3D5);
    outb(0x3D4, 0x01);
    uint8_t h_disp_end = inb(0x3D5);

    // Read CRTC H Blank Start (0x02)
    outb(0x3D4, 0x02);
    uint8_t h_blank_start = inb(0x3D5);

    kprintf("[FB] VGA CRTC: offset=0x%02x start_addr=0x%04x pixel_pan=%u\n",
            crtc_offset, start_addr, pixel_pan);
    kprintf("[FB] VGA CRTC: h_total=%u h_disp_end=%u h_blank_start=%u\n",
            h_total, h_disp_end, h_blank_start);

    // If pixel panning is non-zero, zero it out
    if (pixel_pan != 0) {
        kprintf("[FB] Fixing pixel panning: %u -> 0\n", pixel_pan);
        inb(0x3DA);
        outb(0x3C0, 0x13 | 0x20);
        outb(0x3C0, 0x00);
    }

    // If start address is non-zero, zero it out
    if (start_addr != 0) {
        kprintf("[FB] Fixing start address: 0x%04x -> 0\n", start_addr);
        outb(0x3D4, 0x0C);
        outb(0x3D5, 0x00);
        outb(0x3D4, 0x0D);
        outb(0x3D5, 0x00);
    }
}
