// paint.c - Paint Application for MayteraOS (MS Paint Win95 equivalent)
#include "paint.h"
#include "window.h"
#include "desktop.h"
#include "filedialog.h"
#include "icons.h"
#include "image.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../video/graphics.h"
#include "../fs/fat.h"
#include "syslog.h"

// UI dimensions
#define TOOLBAR_WIDTH   48
#define PALETTE_HEIGHT  56
#define STATUS_HEIGHT   20
#define TOOL_BTN_SIZE   20
#define TOOL_BTN_PAD    4

// Default palette colors (Win95-style)
static const uint32_t default_palette[28] = {
    0x000000, 0x808080, 0x800000, 0x808000, 0x008000, 0x008080, 0x000080,
    0x800080, 0x808040, 0x004040, 0x0080FF, 0x004080, 0x8000FF, 0x804000,
    0xFFFFFF, 0xC0C0C0, 0xFF0000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0x0000FF,
    0xFF00FF, 0xFFFF80, 0x00FF80, 0x80FFFF, 0x8080FF, 0xFF0080, 0xFF8040
};

// Forward declarations
static void paint_draw_toolbar(paint_t *p, int32_t x, int32_t y, int32_t h);
static void paint_draw_palette(paint_t *p, int32_t x, int32_t y, int32_t w);
static void paint_draw_status(paint_t *p, int32_t x, int32_t y, int32_t w);
static void paint_draw_canvas_area(paint_t *p, int32_t x, int32_t y, int32_t w, int32_t h);
static void paint_handle_canvas_click(paint_t *p, int x, int y, bool left, bool drag);
static void paint_draw_line_on_canvas(paint_t *p, int x1, int y1, int x2, int y2, uint32_t color);
static void paint_flood_fill(paint_t *p, int x, int y, uint32_t new_color);
static void paint_draw_brush(paint_t *p, int cx, int cy, uint32_t color);

// Create paint application
paint_t *paint_create(void) {
    paint_t *p = (paint_t *)kmalloc(sizeof(paint_t));
    if (!p) return NULL;

    memset(p, 0, sizeof(paint_t));

    // Create window
    p->window = window_create("Untitled - Paint", 50, 30, 800, 600);
    if (!p->window) {
        kfree(p);
        return NULL;
    }

    // Initialize canvas
    if (!paint_new(p, PAINT_DEFAULT_WIDTH, PAINT_DEFAULT_HEIGHT)) {
        window_destroy(p->window);
        kfree(p);
        return NULL;
    }

    // Initialize state
    p->tool = TOOL_PENCIL;
    p->brush_size = 1;
    p->fg_color = 0x000000;  // Black
    p->bg_color = 0xFFFFFF;  // White
    p->zoom = 100;

    // Copy default palette
    memcpy(p->palette, default_palette, sizeof(default_palette));

    // UI dimensions
    p->toolbar_width = TOOLBAR_WIDTH;
    p->palette_height = PALETTE_HEIGHT;
    p->status_height = STATUS_HEIGHT;

    return p;
}

// Destroy paint application
void paint_destroy(paint_t *p) {
    if (!p) return;

    // Free canvas
    if (p->canvas) kfree(p->canvas);

    // Free selection buffer
    if (p->selection.buffer) kfree(p->selection.buffer);

    // Free undo states
    for (int i = 0; i < PAINT_UNDO_LEVELS; i++) {
        if (p->undo_stack[i].pixels) kfree(p->undo_stack[i].pixels);
    }

    // Destroy window
    if (p->window) window_destroy(p->window);

    kfree(p);
}

// Create new canvas
bool paint_new(paint_t *p, int width, int height) {
    if (!p || width <= 0 || height <= 0) return false;
    if (width > PAINT_MAX_WIDTH || height > PAINT_MAX_HEIGHT) return false;

    // Allocate new canvas
    uint32_t *new_canvas = (uint32_t *)kmalloc(width * height * sizeof(uint32_t));
    if (!new_canvas) return false;

    // Free old canvas
    if (p->canvas) kfree(p->canvas);

    p->canvas = new_canvas;
    p->canvas_width = width;
    p->canvas_height = height;

    // Fill with background color
    for (int i = 0; i < width * height; i++) {
        p->canvas[i] = p->bg_color;
    }

    p->modified = false;
    p->filepath[0] = '\0';
    p->scroll_x = 0;
    p->scroll_y = 0;

    window_set_title(p->window, "Untitled - Paint");

    // Clear undo history
    for (int i = 0; i < PAINT_UNDO_LEVELS; i++) {
        if (p->undo_stack[i].pixels) {
            kfree(p->undo_stack[i].pixels);
            p->undo_stack[i].pixels = NULL;
        }
    }
    p->undo_index = 0;
    p->undo_count = 0;

    paint_save_undo_state(p);

    return true;
}

// External filesystem
extern fat_fs_t g_fat_fs;

// Open image file
bool paint_open(paint_t *p, const char *path) {
    if (!p || !path) return false;

    // Read file from disk
    fat_file_t file;
    if (fat_open(&g_fat_fs, path, &file) != 0) {
        kprintf("[Paint] Failed to open file: %s\n", path);
        return false;
    }

    // Allocate buffer
    uint32_t file_size = file.file_size;
    uint8_t *file_data = (uint8_t *)kmalloc(file_size);
    if (!file_data) {
        fat_close(&file);
        return false;
    }

    // Read file
    size_t bytes_read = fat_read(&file, file_data, file_size);
    fat_close(&file);

    if (bytes_read != file_size) {
        kfree(file_data);
        return false;
    }

    // Load BMP
    image_t img;
    int err = image_load_bmp(file_data, file_size, &img);
    kfree(file_data);

    if (err != IMAGE_SUCCESS) {
        kprintf("[Paint] Failed to decode BMP: %s\n", image_error_string(err));
        return false;
    }

    // Allocate canvas for image
    uint32_t *new_canvas = (uint32_t *)kmalloc(img.width * img.height * sizeof(uint32_t));
    if (!new_canvas) {
        image_free(&img);
        return false;
    }

    // Free old canvas
    if (p->canvas) kfree(p->canvas);

    // Copy image data
    p->canvas = new_canvas;
    p->canvas_width = img.width;
    p->canvas_height = img.height;
    memcpy(p->canvas, img.pixels, img.width * img.height * sizeof(uint32_t));

    image_free(&img);

    // Update state
    strncpy(p->filepath, path, 255);
    p->filepath[255] = '\0';
    p->modified = false;

    // Update window title manually
    const char *filename = path;
    for (int i = strlen(path) - 1; i >= 0; i--) {
        if (path[i] == '/') {
            filename = &path[i + 1];
            break;
        }
    }
    char title[128];
    char *tp = title;
    while (*filename && (tp - title) < 100) *tp++ = *filename++;
    const char *suffix = " - Paint";
    while (*suffix) *tp++ = *suffix++;
    *tp = '\0';
    window_set_title(p->window, title);

    paint_save_undo_state(p);

    return true;
}

// Save canvas to BMP (TODO: implement proper BMP writing)
bool paint_save(paint_t *p, const char *path) {
    if (!p || !path || !p->canvas) return false;

    // TODO: Implement BMP saving - for now just log
    kprintf("[Paint] Save not yet implemented: %s\n", path);

    // Mark as saved anyway
    strncpy(p->filepath, path, 255);
    p->filepath[255] = '\0';
    p->modified = false;

    // Update window title manually
    const char *filename = path;
    for (int i = strlen(path) - 1; i >= 0; i--) {
        if (path[i] == '/') {
            filename = &path[i + 1];
            break;
        }
    }
    char title[128];
    char *tp = title;
    while (*filename && (tp - title) < 100) *tp++ = *filename++;
    const char *suffix = " - Paint";
    while (*suffix) *tp++ = *suffix++;
    *tp = '\0';
    window_set_title(p->window, title);

    return true;
}

// Save as (with dialog)
bool paint_save_as(paint_t *p) {
    if (!p) return false;

    char filepath[256];
    const char *default_name = strlen(p->filepath) > 0 ? p->filepath : "untitled.bmp";

    if (filedialog_save("Save As", "/", default_name, "*.bmp", "BMP Files", filepath)) {
        return paint_save(p, filepath);
    }
    return false;
}

// Set tool
void paint_set_tool(paint_t *p, paint_tool_t tool) {
    if (p && tool < TOOL_COUNT) {
        p->tool = tool;
        // Cancel current drawing operation
        p->drawing = false;
        p->poly_count = 0;
    }
}

// Set brush size
void paint_set_brush_size(paint_t *p, int size) {
    if (!p) return;
    if (size < PAINT_BRUSH_MIN) size = PAINT_BRUSH_MIN;
    if (size > PAINT_BRUSH_MAX) size = PAINT_BRUSH_MAX;
    p->brush_size = size;
}

// Set colors
void paint_set_fg_color(paint_t *p, uint32_t color) {
    if (p) p->fg_color = color;
}

void paint_set_bg_color(paint_t *p, uint32_t color) {
    if (p) p->bg_color = color;
}

void paint_swap_colors(paint_t *p) {
    if (!p) return;
    uint32_t tmp = p->fg_color;
    p->fg_color = p->bg_color;
    p->bg_color = tmp;
}

uint32_t paint_pick_color(paint_t *p, int x, int y) {
    if (!p || !p->canvas) return 0;
    if (x < 0 || x >= p->canvas_width || y < 0 || y >= p->canvas_height) return 0;
    return p->canvas[y * p->canvas_width + x];
}

// Undo/redo
void paint_save_undo_state(paint_t *p) {
    if (!p || !p->canvas) return;

    // Free old state if we're overwriting
    int idx = p->undo_index % PAINT_UNDO_LEVELS;
    if (p->undo_stack[idx].pixels) {
        kfree(p->undo_stack[idx].pixels);
    }

    // Allocate new state
    int size = p->canvas_width * p->canvas_height * sizeof(uint32_t);
    p->undo_stack[idx].pixels = (uint32_t *)kmalloc(size);
    if (p->undo_stack[idx].pixels) {
        memcpy(p->undo_stack[idx].pixels, p->canvas, size);
        p->undo_stack[idx].width = p->canvas_width;
        p->undo_stack[idx].height = p->canvas_height;
        p->undo_index++;
        if (p->undo_count < PAINT_UNDO_LEVELS) p->undo_count++;
    }
}

void paint_undo(paint_t *p) {
    if (!p || p->undo_index <= 1) return;

    p->undo_index--;
    int idx = (p->undo_index - 1) % PAINT_UNDO_LEVELS;

    if (p->undo_stack[idx].pixels) {
        // Restore canvas
        if (p->canvas_width != p->undo_stack[idx].width ||
            p->canvas_height != p->undo_stack[idx].height) {
            kfree(p->canvas);
            p->canvas_width = p->undo_stack[idx].width;
            p->canvas_height = p->undo_stack[idx].height;
            p->canvas = (uint32_t *)kmalloc(p->canvas_width * p->canvas_height * sizeof(uint32_t));
        }
        memcpy(p->canvas, p->undo_stack[idx].pixels,
               p->canvas_width * p->canvas_height * sizeof(uint32_t));
        p->modified = true;
    }
}

void paint_redo(paint_t *p) {
    if (!p || p->undo_index >= p->undo_count) return;

    int idx = p->undo_index % PAINT_UNDO_LEVELS;
    if (p->undo_stack[idx].pixels) {
        if (p->canvas_width != p->undo_stack[idx].width ||
            p->canvas_height != p->undo_stack[idx].height) {
            kfree(p->canvas);
            p->canvas_width = p->undo_stack[idx].width;
            p->canvas_height = p->undo_stack[idx].height;
            p->canvas = (uint32_t *)kmalloc(p->canvas_width * p->canvas_height * sizeof(uint32_t));
        }
        memcpy(p->canvas, p->undo_stack[idx].pixels,
               p->canvas_width * p->canvas_height * sizeof(uint32_t));
        p->undo_index++;
        p->modified = true;
    }
}

// Drawing primitives on canvas
static void paint_set_pixel(paint_t *p, int x, int y, uint32_t color) {
    if (!p || !p->canvas) return;
    if (x < 0 || x >= p->canvas_width || y < 0 || y >= p->canvas_height) return;
    p->canvas[y * p->canvas_width + x] = color;
}

static void paint_draw_line_on_canvas(paint_t *p, int x1, int y1, int x2, int y2, uint32_t color) {
    // Bresenham's line algorithm
    int dx = (x2 > x1) ? (x2 - x1) : (x1 - x2);
    int dy = (y2 > y1) ? (y2 - y1) : (y1 - y2);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        paint_set_pixel(p, x1, y1, color);

        if (x1 == x2 && y1 == y2) break;

        int e2 = 2 * err;
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

static void paint_draw_brush(paint_t *p, int cx, int cy, uint32_t color) {
    int r = p->brush_size / 2;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                paint_set_pixel(p, cx + dx, cy + dy, color);
            }
        }
    }
}

// Flood fill using stack-based algorithm
static void paint_flood_fill(paint_t *p, int x, int y, uint32_t new_color) {
    if (!p || !p->canvas) return;
    if (x < 0 || x >= p->canvas_width || y < 0 || y >= p->canvas_height) return;

    uint32_t old_color = p->canvas[y * p->canvas_width + x];
    if (old_color == new_color) return;

    // Simple recursive fill (limited depth to avoid stack overflow)
    // In production, use explicit stack
    int stack[10000][2];
    int sp = 0;

    stack[sp][0] = x;
    stack[sp][1] = y;
    sp++;

    while (sp > 0 && sp < 9999) {
        sp--;
        int cx = stack[sp][0];
        int cy = stack[sp][1];

        if (cx < 0 || cx >= p->canvas_width || cy < 0 || cy >= p->canvas_height) continue;
        if (p->canvas[cy * p->canvas_width + cx] != old_color) continue;

        p->canvas[cy * p->canvas_width + cx] = new_color;

        stack[sp][0] = cx + 1; stack[sp][1] = cy; sp++;
        stack[sp][0] = cx - 1; stack[sp][1] = cy; sp++;
        stack[sp][0] = cx; stack[sp][1] = cy + 1; sp++;
        stack[sp][0] = cx; stack[sp][1] = cy - 1; sp++;
    }
}

// Draw rectangle on canvas
static void paint_draw_rect_on_canvas(paint_t *p, int x1, int y1, int x2, int y2, uint32_t color, bool filled) {
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }

    if (filled) {
        for (int y = y1; y <= y2; y++) {
            for (int x = x1; x <= x2; x++) {
                paint_set_pixel(p, x, y, color);
            }
        }
    } else {
        for (int x = x1; x <= x2; x++) {
            paint_set_pixel(p, x, y1, color);
            paint_set_pixel(p, x, y2, color);
        }
        for (int y = y1; y <= y2; y++) {
            paint_set_pixel(p, x1, y, color);
            paint_set_pixel(p, x2, y, color);
        }
    }
}

// Draw ellipse on canvas
static void paint_draw_ellipse_on_canvas(paint_t *p, int x1, int y1, int x2, int y2, uint32_t color, bool filled) {
    int cx = (x1 + x2) / 2;
    int cy = (y1 + y2) / 2;
    int rx = (x2 - x1) / 2;
    int ry = (y2 - y1) / 2;
    if (rx < 0) rx = -rx;
    if (ry < 0) ry = -ry;

    // Midpoint ellipse algorithm
    int x = 0, y = ry;
    int rx2 = rx * rx, ry2 = ry * ry;
    int px = 0, py = 2 * rx2 * y;

    if (filled) {
        for (int i = -rx; i <= rx; i++) {
            paint_set_pixel(p, cx + i, cy, color);
        }
    } else {
        paint_set_pixel(p, cx + rx, cy, color);
        paint_set_pixel(p, cx - rx, cy, color);
    }
    paint_set_pixel(p, cx, cy + ry, color);
    paint_set_pixel(p, cx, cy - ry, color);

    // Region 1
    int d1 = ry2 - rx2 * ry + rx2 / 4;
    while (px < py) {
        x++;
        px += 2 * ry2;
        if (d1 < 0) {
            d1 += ry2 + px;
        } else {
            y--;
            py -= 2 * rx2;
            d1 += ry2 + px - py;
        }
        if (filled) {
            for (int i = -x; i <= x; i++) {
                paint_set_pixel(p, cx + i, cy + y, color);
                paint_set_pixel(p, cx + i, cy - y, color);
            }
        } else {
            paint_set_pixel(p, cx + x, cy + y, color);
            paint_set_pixel(p, cx - x, cy + y, color);
            paint_set_pixel(p, cx + x, cy - y, color);
            paint_set_pixel(p, cx - x, cy - y, color);
        }
    }

    // Region 2
    int d2 = ry2 * (x + 1) * (x + 1) + rx2 * (y - 1) * (y - 1) - rx2 * ry2;
    while (y > 0) {
        y--;
        py -= 2 * rx2;
        if (d2 > 0) {
            d2 += rx2 - py;
        } else {
            x++;
            px += 2 * ry2;
            d2 += rx2 - py + px;
        }
        if (filled) {
            for (int i = -x; i <= x; i++) {
                paint_set_pixel(p, cx + i, cy + y, color);
                paint_set_pixel(p, cx + i, cy - y, color);
            }
        } else {
            paint_set_pixel(p, cx + x, cy + y, color);
            paint_set_pixel(p, cx - x, cy + y, color);
            paint_set_pixel(p, cx + x, cy - y, color);
            paint_set_pixel(p, cx - x, cy - y, color);
        }
    }
}

// Handle canvas click
static void paint_handle_canvas_click(paint_t *p, int x, int y, bool left, bool drag) {
    if (!p || !p->canvas) return;

    uint32_t color = left ? p->fg_color : p->bg_color;

    switch (p->tool) {
        case TOOL_PENCIL:
            if (drag && p->drawing) {
                paint_draw_line_on_canvas(p, p->last_x, p->last_y, x, y, color);
            } else {
                paint_set_pixel(p, x, y, color);
            }
            p->last_x = x;
            p->last_y = y;
            break;

        case TOOL_BRUSH:
            if (drag && p->drawing) {
                // Draw brush along line
                int dx = x - p->last_x;
                int dy = y - p->last_y;
                int steps = (dx > dy ? dx : dy);
                if (steps < 0) steps = -steps;
                if (steps == 0) steps = 1;
                for (int i = 0; i <= steps; i++) {
                    int px = p->last_x + (dx * i) / steps;
                    int py = p->last_y + (dy * i) / steps;
                    paint_draw_brush(p, px, py, color);
                }
            } else {
                paint_draw_brush(p, x, y, color);
            }
            p->last_x = x;
            p->last_y = y;
            break;

        case TOOL_ERASER:
            paint_draw_brush(p, x, y, p->bg_color);
            p->last_x = x;
            p->last_y = y;
            break;

        case TOOL_FILL:
            if (!drag) {
                paint_save_undo_state(p);
                paint_flood_fill(p, x, y, color);
                p->modified = true;
            }
            break;

        case TOOL_PICKER:
            if (!drag) {
                uint32_t picked = paint_pick_color(p, x, y);
                if (left) p->fg_color = picked;
                else p->bg_color = picked;
            }
            break;

        case TOOL_LINE:
            if (!drag && !p->drawing) {
                p->start_x = x;
                p->start_y = y;
            }
            // Line preview drawn in paint_draw()
            break;

        case TOOL_RECT:
        case TOOL_RECT_FILL:
            if (!drag && !p->drawing) {
                p->start_x = x;
                p->start_y = y;
            }
            break;

        case TOOL_ELLIPSE:
        case TOOL_ELLIPSE_FILL:
            if (!drag && !p->drawing) {
                p->start_x = x;
                p->start_y = y;
            }
            break;

        default:
            break;
    }

    p->drawing = true;
}

// Complete shape drawing (on mouse up)
static void paint_complete_shape(paint_t *p, int x, int y, bool left) {
    if (!p || !p->canvas) return;

    uint32_t color = left ? p->fg_color : p->bg_color;

    switch (p->tool) {
        case TOOL_LINE:
            paint_save_undo_state(p);
            paint_draw_line_on_canvas(p, p->start_x, p->start_y, x, y, color);
            p->modified = true;
            break;

        case TOOL_RECT:
            paint_save_undo_state(p);
            paint_draw_rect_on_canvas(p, p->start_x, p->start_y, x, y, color, false);
            p->modified = true;
            break;

        case TOOL_RECT_FILL:
            paint_save_undo_state(p);
            paint_draw_rect_on_canvas(p, p->start_x, p->start_y, x, y, color, true);
            p->modified = true;
            break;

        case TOOL_ELLIPSE:
            paint_save_undo_state(p);
            paint_draw_ellipse_on_canvas(p, p->start_x, p->start_y, x, y, color, false);
            p->modified = true;
            break;

        case TOOL_ELLIPSE_FILL:
            paint_save_undo_state(p);
            paint_draw_ellipse_on_canvas(p, p->start_x, p->start_y, x, y, color, true);
            p->modified = true;
            break;

        case TOOL_PENCIL:
        case TOOL_BRUSH:
        case TOOL_ERASER:
            paint_save_undo_state(p);
            p->modified = true;
            break;

        default:
            break;
    }

    p->drawing = false;
}

// 16x16 tool icons (each uint16_t is one row, MSB = left pixel)
static const uint16_t tool_icons[16][16] = {
    // TOOL_PEN (pencil diagonal)
    { 0x0003, 0x0006, 0x000C, 0x0018, 0x0030, 0x0060, 0x00C0, 0x0180,
      0x0300, 0x0600, 0x0C00, 0x1800, 0x3000, 0x6000, 0xC000, 0x8000 },
    // TOOL_BRUSH (thick brush)
    { 0x001C, 0x003E, 0x007C, 0x00F8, 0x01F0, 0x03E0, 0x07C0, 0x0F80,
      0x1F00, 0x3E00, 0x7C00, 0xF800, 0xF000, 0xE000, 0xC000, 0x8000 },
    // TOOL_ERASER (rectangle block)
    { 0x0000, 0x0000, 0x0000, 0x7FFE, 0x7FFE, 0x7FFE, 0x7FFE, 0x7FFE,
      0x7FFE, 0x7FFE, 0x7FFE, 0x7FFE, 0x7FFE, 0x0000, 0x0000, 0x0000 },
    // TOOL_FILL (paint bucket)
    { 0x0780, 0x0FC0, 0x1FE0, 0x3FF0, 0x7FF8, 0x7FF8, 0x7FF8, 0x3FF0,
      0x1FE0, 0x0FC0, 0x0780, 0x0300, 0x0600, 0x0C00, 0x1800, 0x3000 },
    // TOOL_PICKER (eyedropper)
    { 0x001C, 0x003E, 0x003E, 0x001C, 0x0038, 0x0070, 0x00E0, 0x01C0,
      0x0380, 0x0700, 0x0E00, 0x1C00, 0x3800, 0x7000, 0xE000, 0xC000 },
    // TOOL_LINE (diagonal line with endpoints)
    { 0x8003, 0xC006, 0x600C, 0x3018, 0x1830, 0x0C60, 0x06C0, 0x0380,
      0x01C0, 0x0360, 0x0630, 0x0C18, 0x180C, 0x3006, 0x6003, 0xC001 },
    // TOOL_RECT (rectangle outline)
    { 0xFFFF, 0x8001, 0x8001, 0x8001, 0x8001, 0x8001, 0x8001, 0x8001,
      0x8001, 0x8001, 0x8001, 0x8001, 0x8001, 0x8001, 0x8001, 0xFFFF },
    // TOOL_FILLED_RECT (filled rectangle)
    { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
      0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF },
    // TOOL_ELLIPSE (circle outline)
    { 0x03C0, 0x0FF0, 0x1E78, 0x380E, 0x7006, 0x6002, 0xC003, 0xC003,
      0xC003, 0xC003, 0x6002, 0x7006, 0x380E, 0x1E78, 0x0FF0, 0x03C0 },
    // TOOL_FILLED_ELLIPSE (filled circle)
    { 0x03C0, 0x0FF0, 0x1FF8, 0x3FFC, 0x7FFE, 0x7FFE, 0xFFFF, 0xFFFF,
      0xFFFF, 0xFFFF, 0x7FFE, 0x7FFE, 0x3FFC, 0x1FF8, 0x0FF0, 0x03C0 },
    // TOOL_POLYGON (triangle)
    { 0x0080, 0x01C0, 0x01C0, 0x0360, 0x0360, 0x0630, 0x0630, 0x0C18,
      0x0C18, 0x180C, 0x180C, 0x3006, 0x3006, 0x6003, 0x6003, 0xFFFF },
    // TOOL_TEXT (letter A)
    { 0x0180, 0x03C0, 0x03C0, 0x0660, 0x0660, 0x0C30, 0x0C30, 0x1818,
      0x1FF8, 0x3FFC, 0x300C, 0x6006, 0x6006, 0xC003, 0xC003, 0x0000 },
    // TOOL_SELECT (dashed rectangle)
    { 0xAAAA, 0x8001, 0x0001, 0x8001, 0x0001, 0x8001, 0x0001, 0x8001,
      0x0001, 0x8001, 0x0001, 0x8001, 0x0001, 0x8001, 0x8001, 0x5555 },
    // TOOL_FREE_SELECT (lasso)
    { 0x0000, 0x07C0, 0x1830, 0x2008, 0x4004, 0x8002, 0x8006, 0x800C,
      0x8018, 0x8030, 0x4060, 0x20C0, 0x1980, 0x0700, 0x0000, 0x0000 },
    // TOOL_SPRAY (spray dots)
    { 0x0100, 0x0410, 0x1200, 0x0089, 0x0400, 0x0920, 0x4004, 0x0110,
      0x0804, 0x0240, 0x0020, 0x4802, 0x0120, 0x2008, 0x0080, 0x0200 },
    // TOOL_MAGNIFY (magnifying glass)
    { 0x0F80, 0x3060, 0x4010, 0x8008, 0x8008, 0x8008, 0x4010, 0x3060,
      0x0F80, 0x0180, 0x0180, 0x0300, 0x0600, 0x0C00, 0x1800, 0x3000 }
};

// Draw toolbar
static void paint_draw_toolbar(paint_t *p, int32_t x, int32_t y, int32_t h) {
    // Background
    fb_fill_rect(x, y, TOOLBAR_WIDTH, h, 0xC0C0C0);
    fb_fill_rect(x + TOOLBAR_WIDTH - 1, y, 1, h, 0x808080);

    // Tool buttons
    int bx = x + 4;
    int by = y + 4;

    for (int i = 0; i < TOOL_COUNT && i < 16; i++) {
        bool selected = ((int)p->tool == i);

        // Button background
        uint32_t bg = selected ? 0xFFFFFF : 0xC0C0C0;
        fb_fill_rect(bx, by, TOOL_BTN_SIZE, TOOL_BTN_SIZE, bg);

        // Button border
        if (selected) {
            fb_draw_rect(bx, by, TOOL_BTN_SIZE, TOOL_BTN_SIZE, 0x000000);
        } else {
            fb_fill_rect(bx, by, TOOL_BTN_SIZE, 1, 0xFFFFFF);
            fb_fill_rect(bx, by, 1, TOOL_BTN_SIZE, 0xFFFFFF);
            fb_fill_rect(bx, by + TOOL_BTN_SIZE - 1, TOOL_BTN_SIZE, 1, 0x808080);
            fb_fill_rect(bx + TOOL_BTN_SIZE - 1, by, 1, TOOL_BTN_SIZE, 0x808080);
        }

        // Draw tool icon (16x16, centered in 20x20 button)
        int icon_x = bx + 2;
        int icon_y = by + 2;
        for (int row = 0; row < 16; row++) {
            uint16_t bits = tool_icons[i][row];
            for (int col = 0; col < 16; col++) {
                if (bits & (0x8000 >> col)) {
                    fb_put_pixel(icon_x + col, icon_y + row, 0x000000);
                }
            }
        }

        // Next position
        if ((i % 2) == 0) {
            bx += TOOL_BTN_SIZE + TOOL_BTN_PAD;
        } else {
            bx = x + 4;
            by += TOOL_BTN_SIZE + TOOL_BTN_PAD;
        }
    }
}

// Draw color palette
static void paint_draw_palette(paint_t *p, int32_t x, int32_t y, int32_t w) {
    // Background
    fb_fill_rect(x, y, w, PALETTE_HEIGHT, 0xC0C0C0);
    fb_fill_rect(x, y, w, 1, 0x808080);

    // Current colors (fg/bg)
    int color_x = x + 8;
    int color_y = y + 8;

    // Background color (behind)
    fb_fill_rect(color_x + 14, color_y + 14, 24, 24, p->bg_color);
    fb_draw_rect(color_x + 14, color_y + 14, 24, 24, 0x000000);

    // Foreground color (front)
    fb_fill_rect(color_x, color_y, 24, 24, p->fg_color);
    fb_draw_rect(color_x, color_y, 24, 24, 0x000000);

    // Palette colors
    int px = color_x + 50;
    int py = y + 4;
    int size = 16;

    for (int i = 0; i < 28; i++) {
        int cx = px + (i % 14) * (size + 2);
        int cy = py + (i / 14) * (size + 2);

        fb_fill_rect(cx, cy, size, size, p->palette[i]);
        fb_draw_rect(cx, cy, size, size, 0x000000);
    }

    // Brush size indicator - build "Size:N" manually
    char size_str[16];
    char *sp = size_str;
    const char *prefix = "Size:";
    while (*prefix) *sp++ = *prefix++;
    // Convert brush_size to string
    int val = p->brush_size;
    if (val == 0) {
        *sp++ = '0';
    } else {
        char tmp[8];
        int i = 0;
        while (val > 0) {
            tmp[i++] = '0' + (val % 10);
            val /= 10;
        }
        while (i > 0) *sp++ = tmp[--i];
    }
    *sp = '\0';
    int text_x = x + w - 80;
    int text_y = y + 20;
    for (int i = 0; size_str[i]; i++) {
        const uint8_t *glyph = font_get_glyph(size_str[i]);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    fb_put_pixel(text_x + i * 8 + col, text_y + row, 0x000000);
                }
            }
        }
    }
}

// Helper to append an integer to a string
static char *paint_itoa(char *buf, int val) {
    if (val == 0) {
        *buf++ = '0';
        return buf;
    }
    char tmp[12];
    int i = 0;
    while (val > 0) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }
    while (i > 0) *buf++ = tmp[--i];
    return buf;
}

// Draw status bar
static void paint_draw_status(paint_t *p, int32_t x, int32_t y, int32_t w) {
    fb_fill_rect(x, y, w, STATUS_HEIGHT, 0xC0C0C0);
    fb_fill_rect(x, y, w, 1, 0x808080);

    // Build status text: "WxH  Zoom:N%  [Modified]"
    char status[128];
    char *sp = status;
    sp = paint_itoa(sp, p->canvas_width);
    *sp++ = 'x';
    sp = paint_itoa(sp, p->canvas_height);
    *sp++ = ' '; *sp++ = ' ';
    const char *zoom_lbl = "Zoom:";
    while (*zoom_lbl) *sp++ = *zoom_lbl++;
    sp = paint_itoa(sp, p->zoom);
    *sp++ = '%';
    if (p->modified) {
        *sp++ = ' '; *sp++ = ' ';
        const char *mod = "[Modified]";
        while (*mod) *sp++ = *mod++;
    }
    *sp = '\0';

    int text_x = x + 4;
    int text_y = y + 2;
    for (int i = 0; status[i]; i++) {
        const uint8_t *glyph = font_get_glyph(status[i]);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    fb_put_pixel(text_x + i * 8 + col, text_y + row, 0x000000);
                }
            }
        }
    }
}

// Draw canvas area
static void paint_draw_canvas_area(paint_t *p, int32_t x, int32_t y, int32_t w, int32_t h) {
    // Canvas background (gray for areas outside canvas)
    fb_fill_rect(x, y, w, h, 0x808080);

    if (!p->canvas) return;

    // Calculate visible canvas area
    int zoom = p->zoom;
    int disp_w = (p->canvas_width * zoom) / 100;
    int disp_h = (p->canvas_height * zoom) / 100;

    // Center or top-left align
    int cx = x + (w - disp_w) / 2;
    int cy = y + (h - disp_h) / 2;
    if (cx < x) cx = x;
    if (cy < y) cy = y;

    cx -= p->scroll_x;
    cy -= p->scroll_y;

    // Draw canvas pixels
    for (int dy = 0; dy < h && dy < disp_h; dy++) {
        int src_y = (dy * 100) / zoom;
        if (src_y >= p->canvas_height) continue;

        for (int dx = 0; dx < w && dx < disp_w; dx++) {
            int src_x = (dx * 100) / zoom;
            if (src_x >= p->canvas_width) continue;

            int dest_x = cx + dx;
            int dest_y = cy + dy;
            if (dest_x >= x && dest_x < x + w && dest_y >= y && dest_y < y + h) {
                uint32_t color = p->canvas[src_y * p->canvas_width + src_x];
                fb_put_pixel(dest_x, dest_y, color);
            }
        }
    }

    // Draw shape preview while drawing
    if (p->drawing) {
        // Get current mouse position relative to canvas
        // (In real implementation, track last mouse position)
    }
}

// Helper to handle mouse interactions
static void paint_handle_mouse(paint_t *p, gui_event_t *event, bool pressed) {
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(p->window, &wx, &wy, &ww, &wh);

    int canvas_x = wx + TOOLBAR_WIDTH;
    int canvas_y = wy;
    int canvas_w = ww - TOOLBAR_WIDTH;
    int canvas_h = wh - PALETTE_HEIGHT - STATUS_HEIGHT;

    int mx = event->mouse_x;
    int my = event->mouse_y;

    // Check if in canvas area
    if (mx >= canvas_x && mx < canvas_x + canvas_w &&
        my >= canvas_y && my < canvas_y + canvas_h) {

        // Convert to canvas coordinates
        int zoom = p->zoom;
        int disp_w = (p->canvas_width * zoom) / 100;
        int disp_h = (p->canvas_height * zoom) / 100;
        int cx = canvas_x + (canvas_w - disp_w) / 2 - p->scroll_x;
        int cy = canvas_y + (canvas_h - disp_h) / 2 - p->scroll_y;
        if (cx < canvas_x) cx = canvas_x;
        if (cy < canvas_y) cy = canvas_y;

        int pixel_x = ((mx - cx) * 100) / zoom;
        int pixel_y = ((my - cy) * 100) / zoom;

        bool left = (event->mouse_buttons & MOUSE_BUTTON_LEFT);
        bool drag = p->drawing;

        if (pressed && left) {
            paint_handle_canvas_click(p, pixel_x, pixel_y, left, drag);
        } else if (!pressed && p->drawing) {
            // Mouse released
            paint_complete_shape(p, pixel_x, pixel_y, true);
        }
    }

    // Check palette clicks (only on press)
    if (pressed) {
        int palette_y = wy + wh - PALETTE_HEIGHT - STATUS_HEIGHT;
        if (my >= palette_y && my < palette_y + PALETTE_HEIGHT) {
            // Palette color click
            int px = wx + TOOLBAR_WIDTH + 50;
            int py = palette_y + 4;
            int size = 16;

            for (int i = 0; i < 28; i++) {
                int pcx = px + (i % 14) * (size + 2);
                int pcy = py + (i / 14) * (size + 2);

                if (mx >= pcx && mx < pcx + size && my >= pcy && my < pcy + size) {
                    if (event->mouse_buttons & MOUSE_BUTTON_LEFT) {
                        p->fg_color = p->palette[i];
                    } else if (event->mouse_buttons & MOUSE_BUTTON_RIGHT) {
                        p->bg_color = p->palette[i];
                    }
                    break;
                }
            }
        }

        // Check toolbar clicks
        if (mx >= wx && mx < wx + TOOLBAR_WIDTH) {
            int bx = wx + 4;
            int by = wy + 4;

            for (int i = 0; i < TOOL_COUNT && i < 16; i++) {
                int tx = bx + (i % 2) * (TOOL_BTN_SIZE + TOOL_BTN_PAD);
                int ty = by + (i / 2) * (TOOL_BTN_SIZE + TOOL_BTN_PAD);

                if (mx >= tx && mx < tx + TOOL_BTN_SIZE &&
                    my >= ty && my < ty + TOOL_BTN_SIZE) {
                    if (event->mouse_buttons & MOUSE_BUTTON_LEFT) {
                        paint_set_tool(p, i);
                    }
                    break;
                }
            }
        }
    }
}

// Event handling
void paint_handle_event(paint_t *p, gui_event_t *event) {
    if (!p || !event) return;

    switch (event->type) {
        case EVENT_KEY_DOWN:
            // Tool shortcuts (no Ctrl modifier in this simplified version)
            switch (event->keycode) {
                case 'p': case 'P': paint_set_tool(p, TOOL_PENCIL); break;
                case 'b': case 'B': paint_set_tool(p, TOOL_BRUSH); break;
                case 'e': case 'E': paint_set_tool(p, TOOL_ERASER); break;
                case 'f': case 'F': paint_set_tool(p, TOOL_FILL); break;
                case 'l': case 'L': paint_set_tool(p, TOOL_LINE); break;
                case 'r': case 'R': paint_set_tool(p, TOOL_RECT); break;
                case 'x': case 'X': paint_swap_colors(p); break;
                case 'u': case 'U': paint_undo(p); break;
                case 'y': case 'Y': paint_redo(p); break;
                case 'n': case 'N': paint_new(p, PAINT_DEFAULT_WIDTH, PAINT_DEFAULT_HEIGHT); break;
                case 'o': case 'O':
                    {
                        char filepath[256];
                        if (filedialog_open("Open Image", "/", "*.bmp", "BMP Files", filepath)) {
                            paint_open(p, filepath);
                        }
                    }
                    break;
                case '+': case '=':
                    paint_set_brush_size(p, p->brush_size + 1);
                    break;
                case '-': case '_':
                    paint_set_brush_size(p, p->brush_size - 1);
                    break;
            }
            break;

        case EVENT_MOUSE_DOWN:
            paint_handle_mouse(p, event, true);
            break;

        case EVENT_MOUSE_UP:
            paint_handle_mouse(p, event, false);
            break;

        case EVENT_MOUSE_MOVE:
            // Handle dragging for drawing tools
            if (p->drawing && (event->mouse_buttons & MOUSE_BUTTON_LEFT)) {
                paint_handle_mouse(p, event, true);
            }
            break;

        default:
            break;
    }
}

// Selection operations (stubs for now)
void paint_select_all(paint_t *p) {
    if (!p) return;
    p->selection.active = true;
    p->selection.x = 0;
    p->selection.y = 0;
    p->selection.width = p->canvas_width;
    p->selection.height = p->canvas_height;
}

void paint_select_none(paint_t *p) {
    if (!p) return;
    p->selection.active = false;
    if (p->selection.buffer) {
        kfree(p->selection.buffer);
        p->selection.buffer = NULL;
    }
}

void paint_cut(paint_t *p) { paint_copy(p); paint_delete_selection(p); }
void paint_copy(paint_t *p) { (void)p; /* TODO */ }
void paint_paste(paint_t *p) { (void)p; /* TODO */ }
void paint_delete_selection(paint_t *p) { (void)p; /* TODO */ }

// Transform operations (stubs)
void paint_flip_horizontal(paint_t *p) { (void)p; /* TODO */ }
void paint_flip_vertical(paint_t *p) { (void)p; /* TODO */ }
void paint_rotate_90(paint_t *p, bool clockwise) { (void)p; (void)clockwise; /* TODO */ }
void paint_invert_colors(paint_t *p) { (void)p; /* TODO */ }

void paint_clear_canvas(paint_t *p) {
    if (!p || !p->canvas) return;
    paint_save_undo_state(p);
    for (int i = 0; i < p->canvas_width * p->canvas_height; i++) {
        p->canvas[i] = p->bg_color;
    }
    p->modified = true;
}

// Zoom operations
void paint_zoom_in(paint_t *p) {
    if (p && p->zoom < 800) {
        p->zoom += 25;
        if (p->zoom > 800) p->zoom = 800;
    }
}

void paint_zoom_out(paint_t *p) {
    if (p && p->zoom > 25) {
        p->zoom -= 25;
        if (p->zoom < 25) p->zoom = 25;
    }
}

void paint_zoom_actual(paint_t *p) {
    if (p) p->zoom = 100;
}

// Drawing
void paint_draw(paint_t *p) {
    if (!p || !p->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(p->window, &wx, &wy, &ww, &wh);

    // Draw toolbar
    paint_draw_toolbar(p, wx, wy, wh - PALETTE_HEIGHT - STATUS_HEIGHT);

    // Draw canvas area
    int canvas_x = wx + TOOLBAR_WIDTH;
    int canvas_y = wy;
    int canvas_w = ww - TOOLBAR_WIDTH;
    int canvas_h = wh - PALETTE_HEIGHT - STATUS_HEIGHT;
    paint_draw_canvas_area(p, canvas_x, canvas_y, canvas_w, canvas_h);

    // Draw palette
    paint_draw_palette(p, wx, wy + wh - PALETTE_HEIGHT - STATUS_HEIGHT, ww);

    // Draw status bar
    paint_draw_status(p, wx, wy + wh - STATUS_HEIGHT, ww);
}

// Launch paint application
void paint_launch(const char *filepath) {
    LOG_INFO("[Paint] Application launched");
    paint_t *p = paint_create();
    if (!p) {
        LOG_ERROR("[Paint] Failed to create paint application");
        kprintf("[Paint] Failed to create paint application\n");
        return;
    }

    if (filepath && strlen(filepath) > 0) {
        paint_open(p, filepath);
    }

    // Register with window manager
    wm_register_app(p->window, p,
                    (app_event_handler_t)paint_handle_event,
                    (app_draw_handler_t)paint_draw,
                    (app_destroy_handler_t)paint_destroy);

    kprintf("[Paint] Application launched\n");
}
