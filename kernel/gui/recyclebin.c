// recyclebin.c - Recycle Bin for MayteraOS
#include "recyclebin.h"
#include "window.h"
#include "icons.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "syslog.h"

// External timer
extern volatile uint64_t timer_ticks;

// Global recycle bin storage (persists even when window is closed)
static recycle_item_t g_recycle_items[RECYCLE_MAX_ITEMS];
static int g_recycle_count = 0;
static bool g_recycle_initialized = false;

// Colors
#define RB_BG_COLOR         0x1E1E2E
#define RB_HEADER_COLOR     0x2D2D3D
#define RB_SELECT_COLOR     0x3D3D5D
#define RB_TEXT_COLOR       0xFFFFFF
#define RB_TEXT_DIM_COLOR   0x808080
#define RB_BORDER_COLOR     0x404050

// UI dimensions
#define RB_ROW_HEIGHT       24
#define RB_HEADER_HEIGHT    28
#define RB_FOOTER_HEIGHT    40
#define RB_PADDING          8
#define RB_ICON_SIZE        16

// Initialize global storage
static void recycle_init_storage(void) {
    if (!g_recycle_initialized) {
        memset(g_recycle_items, 0, sizeof(g_recycle_items));
        g_recycle_count = 0;
        g_recycle_initialized = true;
    }
}

// Add item to recycle bin (global function)
bool recyclebin_add(const char *path, bool is_dir) {
    recycle_init_storage();

    if (!path || g_recycle_count >= RECYCLE_MAX_ITEMS) {
        return false;
    }

    // Find a free slot
    int slot = -1;
    for (int i = 0; i < RECYCLE_MAX_ITEMS; i++) {
        if (!g_recycle_items[i].valid) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        return false;
    }

    recycle_item_t *item = &g_recycle_items[slot];

    // Store original path
    strncpy(item->original_path, path, RECYCLE_PATH_MAX - 1);
    item->original_path[RECYCLE_PATH_MAX - 1] = '\0';

    // Extract filename from path
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    strncpy(item->deleted_name, name, 63);
    item->deleted_name[63] = '\0';

    item->file_size = 0;  // Would need FAT lookup to get actual size
    item->deleted_time = timer_ticks;
    item->is_directory = is_dir;
    item->valid = true;

    g_recycle_count++;

    kprintf("[RecycleBin] Added: %s\n", path);
    return true;
}

// Get item count
int recyclebin_get_count(void) {
    recycle_init_storage();
    return g_recycle_count;
}

// Check if empty
bool recyclebin_is_empty(void) {
    return recyclebin_get_count() == 0;
}

// Create recycle bin window
recyclebin_t *recyclebin_create(void) {
    recycle_init_storage();

    recyclebin_t *rb = (recyclebin_t *)kmalloc(sizeof(recyclebin_t));
    if (!rb) return NULL;

    memset(rb, 0, sizeof(recyclebin_t));

    // Create window
    rb->window = window_create("Recycle Bin", 180, 100, 450, 350);
    if (!rb->window) {
        kfree(rb);
        return NULL;
    }

    rb->selected = -1;
    rb->scroll_offset = 0;
    rb->last_update = timer_ticks;

    // Copy global items to local view
    rb->item_count = 0;
    for (int i = 0; i < RECYCLE_MAX_ITEMS && rb->item_count < RECYCLE_MAX_ITEMS; i++) {
        if (g_recycle_items[i].valid) {
            rb->items[rb->item_count] = g_recycle_items[i];
            rb->item_count++;
        }
    }

    return rb;
}

// Destroy recycle bin
void recyclebin_destroy(recyclebin_t *rb) {
    if (!rb) return;
    if (rb->window) {
        window_destroy(rb->window);
    }
    kfree(rb);
}

// Restore selected item
bool recyclebin_restore(recyclebin_t *rb) {
    if (!rb || rb->selected < 0 || rb->selected >= rb->item_count) {
        return false;
    }

    recycle_item_t *item = &rb->items[rb->selected];

    // In a real implementation, we would:
    // 1. Move the file from recycle storage back to original_path
    // 2. For now, just log it
    kprintf("[RecycleBin] Restore: %s -> %s\n", item->deleted_name, item->original_path);

    // Find and remove from global storage
    for (int i = 0; i < RECYCLE_MAX_ITEMS; i++) {
        if (g_recycle_items[i].valid &&
            strcmp(g_recycle_items[i].original_path, item->original_path) == 0) {
            g_recycle_items[i].valid = false;
            g_recycle_count--;
            break;
        }
    }

    // Remove from local view
    for (int i = rb->selected; i < rb->item_count - 1; i++) {
        rb->items[i] = rb->items[i + 1];
    }
    rb->item_count--;

    if (rb->selected >= rb->item_count) {
        rb->selected = rb->item_count - 1;
    }

    return true;
}

// Permanently delete selected item
bool recyclebin_delete_permanent(recyclebin_t *rb) {
    if (!rb || rb->selected < 0 || rb->selected >= rb->item_count) {
        return false;
    }

    recycle_item_t *item = &rb->items[rb->selected];

    kprintf("[RecycleBin] Permanently deleted: %s\n", item->deleted_name);

    // Find and remove from global storage
    for (int i = 0; i < RECYCLE_MAX_ITEMS; i++) {
        if (g_recycle_items[i].valid &&
            strcmp(g_recycle_items[i].original_path, item->original_path) == 0) {
            g_recycle_items[i].valid = false;
            g_recycle_count--;
            break;
        }
    }

    // Remove from local view
    for (int i = rb->selected; i < rb->item_count - 1; i++) {
        rb->items[i] = rb->items[i + 1];
    }
    rb->item_count--;

    if (rb->selected >= rb->item_count) {
        rb->selected = rb->item_count - 1;
    }

    return true;
}

// Empty recycle bin
void recyclebin_empty(recyclebin_t *rb) {
    if (!rb) return;

    kprintf("[RecycleBin] Emptying recycle bin (%d items)\n", g_recycle_count);

    // Clear global storage
    for (int i = 0; i < RECYCLE_MAX_ITEMS; i++) {
        g_recycle_items[i].valid = false;
    }
    g_recycle_count = 0;

    // Clear local view
    rb->item_count = 0;
    rb->selected = -1;
    rb->scroll_offset = 0;
}

// Draw text helper
static void rb_draw_text(const char *text, int x, int y, uint32_t color) {
    for (int i = 0; text[i]; i++) {
        const uint8_t *glyph = font_get_glyph(text[i]);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    fb_put_pixel(x + i * 8 + col, y + row, color);
                }
            }
        }
    }
}

// Draw button
static void rb_draw_button(int x, int y, int w, int h, const char *label, bool enabled) {
    uint32_t bg = enabled ? 0x505060 : 0x303040;
    uint32_t text_color = enabled ? RB_TEXT_COLOR : RB_TEXT_DIM_COLOR;

    fb_fill_rect(x, y, w, h, bg);
    fb_draw_rect(x, y, w, h, RB_BORDER_COLOR);

    int text_len = strlen(label) * 8;
    int text_x = x + (w - text_len) / 2;
    int text_y = y + (h - 16) / 2;
    rb_draw_text(label, text_x, text_y, text_color);
}

// Format file size as string (for future use when we get real file sizes)
__attribute__((unused))
static void format_size(uint32_t size, char *buf, int buf_len __attribute__((unused))) {
    if (size < 1024) {
        // Bytes
        int i = 0;
        if (size == 0) {
            buf[i++] = '0';
        } else {
            char tmp[16];
            int j = 0;
            while (size > 0) {
                tmp[j++] = '0' + (size % 10);
                size /= 10;
            }
            while (j > 0) buf[i++] = tmp[--j];
        }
        buf[i++] = ' ';
        buf[i++] = 'B';
        buf[i] = '\0';
    } else if (size < 1024 * 1024) {
        uint32_t kb = size / 1024;
        int i = 0;
        char tmp[16];
        int j = 0;
        while (kb > 0) {
            tmp[j++] = '0' + (kb % 10);
            kb /= 10;
        }
        while (j > 0) buf[i++] = tmp[--j];
        buf[i++] = ' ';
        buf[i++] = 'K';
        buf[i++] = 'B';
        buf[i] = '\0';
    } else {
        uint32_t mb = size / (1024 * 1024);
        int i = 0;
        char tmp[16];
        int j = 0;
        while (mb > 0) {
            tmp[j++] = '0' + (mb % 10);
            mb /= 10;
        }
        while (j > 0) buf[i++] = tmp[--j];
        buf[i++] = ' ';
        buf[i++] = 'M';
        buf[i++] = 'B';
        buf[i] = '\0';
    }
}

// Event handling
void recyclebin_handle_event(recyclebin_t *rb, gui_event_t *event) {
    if (!rb || !event) return;

    switch (event->type) {
        case EVENT_KEY_DOWN:
            switch (event->keycode) {
                case 0x48:  // Up arrow
                    if (rb->selected > 0) rb->selected--;
                    break;
                case 0x50:  // Down arrow
                    if (rb->selected < rb->item_count - 1) rb->selected++;
                    break;
                case 'r':
                case 'R':
                    recyclebin_restore(rb);
                    break;
                case 0x7F:  // Delete
                case 'd':
                case 'D':
                    recyclebin_delete_permanent(rb);
                    break;
                case 'e':
                case 'E':
                    recyclebin_empty(rb);
                    break;
            }
            break;

        case EVENT_MOUSE_DOWN:
            {
                int32_t wx, wy, ww, wh;
                window_get_content_bounds(rb->window, &wx, &wy, &ww, &wh);

                int mx = event->mouse_x;
                int my = event->mouse_y;

                // Check list area
                int list_y = wy + RB_HEADER_HEIGHT;
                int list_h = wh - RB_HEADER_HEIGHT - RB_FOOTER_HEIGHT;

                if (my >= list_y && my < list_y + list_h) {
                    int clicked_idx = (my - list_y) / RB_ROW_HEIGHT + rb->scroll_offset;
                    if (clicked_idx >= 0 && clicked_idx < rb->item_count) {
                        rb->selected = clicked_idx;
                    }
                }

                // Check buttons
                int footer_y = wy + wh - RB_FOOTER_HEIGHT;
                int btn_y = footer_y + 8;
                int btn_h = 24;

                // Restore button
                int restore_btn_x = wx + RB_PADDING;
                if (mx >= restore_btn_x && mx < restore_btn_x + 70 &&
                    my >= btn_y && my < btn_y + btn_h) {
                    recyclebin_restore(rb);
                }

                // Delete button
                int delete_btn_x = wx + RB_PADDING + 75;
                if (mx >= delete_btn_x && mx < delete_btn_x + 70 &&
                    my >= btn_y && my < btn_y + btn_h) {
                    recyclebin_delete_permanent(rb);
                }

                // Empty button
                int empty_btn_x = wx + ww - RB_PADDING - 90;
                if (mx >= empty_btn_x && mx < empty_btn_x + 90 &&
                    my >= btn_y && my < btn_y + btn_h) {
                    recyclebin_empty(rb);
                }
            }
            break;

        default:
            break;
    }
}

// Drawing
void recyclebin_draw(recyclebin_t *rb) {
    if (!rb || !rb->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(rb->window, &wx, &wy, &ww, &wh);

    // Background
    fb_fill_rect(wx, wy, ww, wh, RB_BG_COLOR);

    // Header
    fb_fill_rect(wx, wy, ww, RB_HEADER_HEIGHT, RB_HEADER_COLOR);

    // Draw trash icon in header
    icon_draw(rb->item_count > 0 ? ICON_TRASH_FULL : ICON_TRASH,
              wx + RB_PADDING, wy + 2, RB_TEXT_COLOR);

    rb_draw_text("Recycle Bin", wx + RB_PADDING + 28, wy + 6, RB_TEXT_COLOR);

    // Item count on right
    char count_str[32];
    int val = rb->item_count;
    char *cp = count_str;
    if (val == 0) {
        *cp++ = '0';
    } else {
        char tmp[8];
        int i = 0;
        while (val > 0) {
            tmp[i++] = '0' + (val % 10);
            val /= 10;
        }
        while (i > 0) *cp++ = tmp[--i];
    }
    const char *suffix = " items";
    while (*suffix) *cp++ = *suffix++;
    *cp = '\0';

    int count_w = strlen(count_str) * 8;
    rb_draw_text(count_str, wx + ww - RB_PADDING - count_w, wy + 6, RB_TEXT_DIM_COLOR);

    // Separator
    fb_fill_rect(wx, wy + RB_HEADER_HEIGHT, ww, 1, RB_BORDER_COLOR);

    // Item list
    int list_y = wy + RB_HEADER_HEIGHT + 1;
    int list_h = wh - RB_HEADER_HEIGHT - RB_FOOTER_HEIGHT - 1;
    int visible_rows = list_h / RB_ROW_HEIGHT;

    if (rb->item_count == 0) {
        // Empty message
        rb_draw_text("Recycle Bin is empty", wx + ww / 2 - 80, list_y + list_h / 2 - 8, RB_TEXT_DIM_COLOR);
    } else {
        for (int i = 0; i < visible_rows && i + rb->scroll_offset < rb->item_count; i++) {
            int idx = i + rb->scroll_offset;
            recycle_item_t *item = &rb->items[idx];

            int row_y = list_y + i * RB_ROW_HEIGHT;

            // Selection highlight
            if (idx == rb->selected) {
                fb_fill_rect(wx, row_y, ww, RB_ROW_HEIGHT, RB_SELECT_COLOR);
            }

            // Icon
            icon_draw(item->is_directory ? ICON_FOLDER : ICON_FILE,
                      wx + RB_PADDING, row_y + 4, RB_TEXT_COLOR);

            // Filename
            rb_draw_text(item->deleted_name, wx + RB_PADDING + 20, row_y + 4, RB_TEXT_COLOR);

            // Original path (dimmed, right side)
            int path_x = wx + ww / 2;
            int max_chars = (ww - ww / 2 - RB_PADDING) / 8;
            char short_path[64];
            strncpy(short_path, item->original_path, max_chars > 63 ? 63 : max_chars);
            short_path[max_chars > 63 ? 63 : max_chars] = '\0';
            rb_draw_text(short_path, path_x, row_y + 4, RB_TEXT_DIM_COLOR);
        }
    }

    // Footer separator
    int footer_y = wy + wh - RB_FOOTER_HEIGHT;
    fb_fill_rect(wx, footer_y, ww, 1, RB_BORDER_COLOR);

    // Footer
    fb_fill_rect(wx, footer_y + 1, ww, RB_FOOTER_HEIGHT - 1, RB_HEADER_COLOR);

    // Buttons
    bool has_selection = (rb->selected >= 0 && rb->selected < rb->item_count);

    // Restore button
    rb_draw_button(wx + RB_PADDING, footer_y + 8, 70, 24, "Restore", has_selection);

    // Delete button
    rb_draw_button(wx + RB_PADDING + 75, footer_y + 8, 70, 24, "Delete", has_selection);

    // Empty button
    rb_draw_button(wx + ww - RB_PADDING - 90, footer_y + 8, 90, 24, "Empty All", rb->item_count > 0);
}

// Launch recycle bin
void recyclebin_launch(void) {
    LOG_INFO("[RecycleBin] Application launched");
    recyclebin_t *rb = recyclebin_create();
    if (!rb) {
        LOG_ERROR("[RecycleBin] Failed to create recycle bin");
        kprintf("[RecycleBin] Failed to create recycle bin\n");
        return;
    }

    // Register with window manager
    wm_register_app(rb->window, rb,
                    (app_event_handler_t)recyclebin_handle_event,
                    (app_draw_handler_t)recyclebin_draw,
                    (app_destroy_handler_t)recyclebin_destroy);

    kprintf("[RecycleBin] Launched\n");
}
