// filebrowser.c - GUI File Browser application for MayteraOS
#include "filebrowser.h"
#include "window.h"
#include "desktop.h"
#include "image.h"
#include "syslog.h"
#include "../types.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../cpu/isr.h"
#include "../drivers/mouse.h"
#include "../fs/fat.h"
#include "properties.h"
#include "../fs/xattr.h"
#include "thumbnailer.h"

// Global FAT filesystem (from main kernel)
extern fat_fs_t g_fat_fs;

// Global file browser for launch callback
static filebrowser_t *g_active_filebrowser = NULL;

// Forward declarations
static void fb_draw_content(filebrowser_t *fb);
static void fb_redraw(filebrowser_t *fb);
static void fb_handle_click(filebrowser_t *fb, int32_t x, int32_t y);
static void fb_go_up_directory(filebrowser_t *fb);

// Number to string helper
static void fb_itoa(uint32_t num, char *buf) {
    char tmp[16];
    int i = 0;
    if (num == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (num > 0) {
        tmp[i++] = '0' + (num % 10);
        num /= 10;
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

// Format file size (KB, MB, etc.)
static void fb_format_size(uint32_t size, char *buf) {
    if (size < 1024) {
        fb_itoa(size, buf);
        strcat(buf, " B");
    } else if (size < 1024 * 1024) {
        fb_itoa(size / 1024, buf);
        strcat(buf, " KB");
    } else {
        fb_itoa(size / (1024 * 1024), buf);
        strcat(buf, " MB");
    }
}

// Draw a single character at position
static void fb_draw_char(int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg) {
    // Draw background
    fb_fill_rect(x, y, FB_CHAR_W, FB_CHAR_H, bg & 0xFFFFFF);

    // Draw character
    if (c >= ' ' && c < 127) {
        const uint8_t *glyph = font_get_glyph(c);
        if (glyph) {
            for (int r = 0; r < FONT_HEIGHT && r < FB_CHAR_H; r++) {
                uint8_t bits = glyph[r];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(x + col, y + r, fg & 0xFFFFFF);
                    }
                }
            }
        }
    }
}

// Draw a string at position
static void fb_draw_string(int32_t x, int32_t y, const char *str, uint32_t fg, uint32_t bg) {
    while (*str) {
        fb_draw_char(x, y, *str, fg, bg);
        x += FB_CHAR_W;
        str++;
    }
}

// Draw string without background (transparent)
static void fb_draw_string_nobg(int32_t x, int32_t y, const char *str, uint32_t fg) {
    while (*str) {
        if (*str >= ' ' && *str < 127) {
            const uint8_t *glyph = font_get_glyph(*str);
            if (glyph) {
                for (int r = 0; r < FONT_HEIGHT && r < FB_CHAR_H; r++) {
                    uint8_t bits = glyph[r];
                    for (int col = 0; col < FONT_WIDTH; col++) {
                        if (bits & (0x80 >> col)) {
                            fb_put_pixel(x + col, y + r, fg & 0xFFFFFF);
                        }
                    }
                }
            }
        }
        x += FB_CHAR_W;
        str++;
    }
}

// ============================================
// Toolbar Implementation
// ============================================

// Toolbar action functions
static void tb_action_back(filebrowser_t *fb) {
    fb_go_up_directory(fb);
}

static void tb_action_up(filebrowser_t *fb) {
    fb_go_up_directory(fb);
}

static void tb_action_refresh(filebrowser_t *fb) {
    filebrowser_refresh(fb);
}

static void tb_action_newfolder(filebrowser_t *fb) {
    // Create a folder with a default name
    filebrowser_create_folder(fb, "New Folder");
}

static void tb_action_delete(filebrowser_t *fb) {
    filebrowser_delete_selected(fb);
}

static void tb_action_view(filebrowser_t *fb) {
    // Cycle through view modes
    fb->view_mode = (fb->view_mode + 1) % 3;
    kprintf("[FileBrowser] View mode changed to %d\n", fb->view_mode);
}

// Draw toolbar icon (simple 16x16 pixel icons)
static void fb_draw_toolbar_icon(int32_t x, int32_t y, uint8_t icon_id, uint32_t color) {
    switch (icon_id) {
        case FB_ICON_BACK:
            // Left arrow
            for (int i = 0; i < 8; i++) {
                fb_put_pixel(x + 4 + i, y + 8, color);
                if (i < 5) {
                    fb_put_pixel(x + 4 + i, y + 8 - i, color);
                    fb_put_pixel(x + 4 + i, y + 8 + i, color);
                }
            }
            break;

        case FB_ICON_UP:
            // Up arrow
            for (int i = 0; i < 8; i++) {
                fb_put_pixel(x + 8, y + 4 + i, color);
                if (i < 5) {
                    fb_put_pixel(x + 8 - i, y + 4 + i, color);
                    fb_put_pixel(x + 8 + i, y + 4 + i, color);
                }
            }
            break;

        case FB_ICON_REFRESH:
            // Circular arrow
            for (int i = 0; i < 6; i++) {
                fb_put_pixel(x + 5 + i, y + 4, color);
                fb_put_pixel(x + 5 + i, y + 12, color);
            }
            for (int i = 0; i < 6; i++) {
                fb_put_pixel(x + 4, y + 5 + i, color);
                fb_put_pixel(x + 12, y + 5 + i, color);
            }
            // Arrow tip
            fb_put_pixel(x + 10, y + 3, color);
            fb_put_pixel(x + 11, y + 4, color);
            fb_put_pixel(x + 12, y + 5, color);
            break;

        case FB_ICON_NEWFOLDER:
            // Folder with plus
            fb_fill_rect(x + 3, y + 5, 10, 8, color);
            fb_fill_rect(x + 3, y + 4, 5, 2, color);
            // Plus sign
            fb_fill_rect(x + 7, y + 7, 3, 1, 0xFFFFFF);
            fb_fill_rect(x + 8, y + 6, 1, 3, 0xFFFFFF);
            break;

        case FB_ICON_DELETE:
            // X mark
            for (int i = 0; i < 8; i++) {
                fb_put_pixel(x + 4 + i, y + 4 + i, color);
                fb_put_pixel(x + 11 - i, y + 4 + i, color);
            }
            break;

        case FB_ICON_VIEW:
            // Grid pattern
            for (int row = 0; row < 3; row++) {
                for (int col = 0; col < 3; col++) {
                    fb_fill_rect(x + 4 + col * 4, y + 4 + row * 4, 3, 3, color);
                }
            }
            break;

        default:
            break;
    }
}

// Initialize toolbar buttons
void fb_init_toolbar(filebrowser_t *fb) {
    if (!fb) return;

    int btn_x = FB_TOOLBAR_PADDING;
    int btn_idx = 0;

    // Back button
    fb->toolbar_btns[btn_idx].x = btn_x;
    fb->toolbar_btns[btn_idx].y = FB_TOOLBAR_PADDING;
    fb->toolbar_btns[btn_idx].w = FB_TOOLBAR_BTN_W;
    fb->toolbar_btns[btn_idx].h = FB_TOOLBAR_BTN_H;
    fb->toolbar_btns[btn_idx].tooltip = "Back";
    fb->toolbar_btns[btn_idx].icon_id = FB_ICON_BACK;
    fb->toolbar_btns[btn_idx].action = tb_action_back;
    fb->toolbar_btns[btn_idx].enabled = true;
    fb->toolbar_btns[btn_idx].hovered = false;
    fb->toolbar_btns[btn_idx].pressed = false;
    btn_x += FB_TOOLBAR_BTN_W + 2;
    btn_idx++;

    // Up button
    fb->toolbar_btns[btn_idx].x = btn_x;
    fb->toolbar_btns[btn_idx].y = FB_TOOLBAR_PADDING;
    fb->toolbar_btns[btn_idx].w = FB_TOOLBAR_BTN_W;
    fb->toolbar_btns[btn_idx].h = FB_TOOLBAR_BTN_H;
    fb->toolbar_btns[btn_idx].tooltip = "Up";
    fb->toolbar_btns[btn_idx].icon_id = FB_ICON_UP;
    fb->toolbar_btns[btn_idx].action = tb_action_up;
    fb->toolbar_btns[btn_idx].enabled = true;
    fb->toolbar_btns[btn_idx].hovered = false;
    fb->toolbar_btns[btn_idx].pressed = false;
    btn_x += FB_TOOLBAR_BTN_W + 2;
    btn_idx++;

    // Separator
    btn_x += FB_TOOLBAR_SEP_W;

    // Refresh button
    fb->toolbar_btns[btn_idx].x = btn_x;
    fb->toolbar_btns[btn_idx].y = FB_TOOLBAR_PADDING;
    fb->toolbar_btns[btn_idx].w = FB_TOOLBAR_BTN_W;
    fb->toolbar_btns[btn_idx].h = FB_TOOLBAR_BTN_H;
    fb->toolbar_btns[btn_idx].tooltip = "Refresh";
    fb->toolbar_btns[btn_idx].icon_id = FB_ICON_REFRESH;
    fb->toolbar_btns[btn_idx].action = tb_action_refresh;
    fb->toolbar_btns[btn_idx].enabled = true;
    fb->toolbar_btns[btn_idx].hovered = false;
    fb->toolbar_btns[btn_idx].pressed = false;
    btn_x += FB_TOOLBAR_BTN_W + 2;
    btn_idx++;

    // Separator
    btn_x += FB_TOOLBAR_SEP_W;

    // New Folder button
    fb->toolbar_btns[btn_idx].x = btn_x;
    fb->toolbar_btns[btn_idx].y = FB_TOOLBAR_PADDING;
    fb->toolbar_btns[btn_idx].w = FB_TOOLBAR_BTN_W;
    fb->toolbar_btns[btn_idx].h = FB_TOOLBAR_BTN_H;
    fb->toolbar_btns[btn_idx].tooltip = "New Folder";
    fb->toolbar_btns[btn_idx].icon_id = FB_ICON_NEWFOLDER;
    fb->toolbar_btns[btn_idx].action = tb_action_newfolder;
    fb->toolbar_btns[btn_idx].enabled = true;
    fb->toolbar_btns[btn_idx].hovered = false;
    fb->toolbar_btns[btn_idx].pressed = false;
    btn_x += FB_TOOLBAR_BTN_W + 2;
    btn_idx++;

    // Delete button
    fb->toolbar_btns[btn_idx].x = btn_x;
    fb->toolbar_btns[btn_idx].y = FB_TOOLBAR_PADDING;
    fb->toolbar_btns[btn_idx].w = FB_TOOLBAR_BTN_W;
    fb->toolbar_btns[btn_idx].h = FB_TOOLBAR_BTN_H;
    fb->toolbar_btns[btn_idx].tooltip = "Delete";
    fb->toolbar_btns[btn_idx].icon_id = FB_ICON_DELETE;
    fb->toolbar_btns[btn_idx].action = tb_action_delete;
    fb->toolbar_btns[btn_idx].enabled = true;
    fb->toolbar_btns[btn_idx].hovered = false;
    fb->toolbar_btns[btn_idx].pressed = false;
    btn_x += FB_TOOLBAR_BTN_W + 2;
    btn_idx++;

    // Separator
    btn_x += FB_TOOLBAR_SEP_W;

    // View mode button
    fb->toolbar_btns[btn_idx].x = btn_x;
    fb->toolbar_btns[btn_idx].y = FB_TOOLBAR_PADDING;
    fb->toolbar_btns[btn_idx].w = FB_TOOLBAR_BTN_W;
    fb->toolbar_btns[btn_idx].h = FB_TOOLBAR_BTN_H;
    fb->toolbar_btns[btn_idx].tooltip = "View Mode";
    fb->toolbar_btns[btn_idx].icon_id = FB_ICON_VIEW;
    fb->toolbar_btns[btn_idx].action = tb_action_view;
    fb->toolbar_btns[btn_idx].enabled = true;
    fb->toolbar_btns[btn_idx].hovered = false;
    fb->toolbar_btns[btn_idx].pressed = false;
    btn_idx++;

    fb->toolbar_btn_count = btn_idx;
}

// Draw the toolbar
void fb_draw_toolbar(filebrowser_t *fb) {
    if (!fb || !fb->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(fb->window, &wx, &wy, &ww, &wh);

    // Fill toolbar background
    fb_fill_rect(wx, wy, ww, FB_TOOLBAR_HEIGHT, FB_TOOLBAR_BG & 0xFFFFFF);

    // Draw each toolbar button
    for (int i = 0; i < fb->toolbar_btn_count; i++) {
        fb_toolbar_btn_t *btn = &fb->toolbar_btns[i];

        int32_t btn_x = wx + btn->x;
        int32_t btn_y = wy + btn->y;

        // Determine button color based on state
        uint32_t bg_color = FB_TOOLBAR_BTN_BG;
        if (btn->pressed) {
            bg_color = FB_TOOLBAR_BTN_PRESS;
        } else if (btn->hovered) {
            bg_color = FB_TOOLBAR_BTN_HOVER;
        }

        // Draw button background
        fb_fill_rect(btn_x, btn_y, btn->w, btn->h, bg_color & 0xFFFFFF);

        // Draw 3D border effect
        uint32_t light = 0xFFFFFF;
        uint32_t dark = 0x808080;
        if (btn->pressed) {
            uint32_t tmp = light;
            light = dark;
            dark = tmp;
        }

        // Top and left edges (light)
        for (int j = 0; j < btn->w; j++) fb_put_pixel(btn_x + j, btn_y, light);
        for (int j = 0; j < btn->h; j++) fb_put_pixel(btn_x, btn_y + j, light);

        // Bottom and right edges (dark)
        for (int j = 0; j < btn->w; j++) fb_put_pixel(btn_x + j, btn_y + btn->h - 1, dark);
        for (int j = 0; j < btn->h; j++) fb_put_pixel(btn_x + btn->w - 1, btn_y + j, dark);

        // Draw icon
        uint32_t icon_color = btn->enabled ? FB_TEXT_COLOR : FB_MENU_DISABLED;
        fb_draw_toolbar_icon(btn_x + 4, btn_y + 3, btn->icon_id, icon_color & 0xFFFFFF);
    }

    // Draw separator line at bottom
    fb_fill_rect(wx, wy + FB_TOOLBAR_HEIGHT - 1, ww, 1, 0x808080);
}

// Handle toolbar click
bool fb_handle_toolbar_click(filebrowser_t *fb, int32_t x, int32_t y) {
    if (!fb || !fb->window) return false;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(fb->window, &wx, &wy, &ww, &wh);

    // Check if click is in toolbar area
    if (y < wy || y >= wy + FB_TOOLBAR_HEIGHT) return false;

    // Check each button
    for (int i = 0; i < fb->toolbar_btn_count; i++) {
        fb_toolbar_btn_t *btn = &fb->toolbar_btns[i];
        int32_t btn_x = wx + btn->x;
        int32_t btn_y = wy + btn->y;

        if (x >= btn_x && x < btn_x + btn->w &&
            y >= btn_y && y < btn_y + btn->h) {
            if (btn->enabled && btn->action) {
                btn->action(fb);
                return true;
            }
        }
    }
    return false;
}

// Update toolbar hover state
void fb_update_toolbar_hover(filebrowser_t *fb, int32_t x, int32_t y) {
    if (!fb || !fb->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(fb->window, &wx, &wy, &ww, &wh);

    for (int i = 0; i < fb->toolbar_btn_count; i++) {
        fb_toolbar_btn_t *btn = &fb->toolbar_btns[i];
        int32_t btn_x = wx + btn->x;
        int32_t btn_y = wy + btn->y;

        btn->hovered = (x >= btn_x && x < btn_x + btn->w &&
                        y >= btn_y && y < btn_y + btn->h);
    }
}

// ============================================
// Breadcrumb Navigation Implementation
// ============================================

// Update breadcrumb segments from current path
void fb_update_breadcrumbs(filebrowser_t *fb) {
    if (!fb) return;

    fb->breadcrumb_count = 0;
    fb->breadcrumb_hover = -1;

    // Always add root
    strcpy(fb->breadcrumbs[0].name, "/");
    strcpy(fb->breadcrumbs[0].path, "/");
    fb->breadcrumbs[0].x = 0;
    fb->breadcrumbs[0].w = FB_CHAR_W + FB_BREADCRUMB_SEP_W;
    fb->breadcrumb_count = 1;

    if (strcmp(fb->current_path, "/") == 0) {
        return;
    }

    // Parse path into segments
    char path_copy[FB_MAX_PATH];
    strncpy(path_copy, fb->current_path, FB_MAX_PATH - 1);
    path_copy[FB_MAX_PATH - 1] = '\0';

    char built_path[FB_MAX_PATH] = "";
    char *token = path_copy;
    char *next;

    // Skip leading slash
    if (*token == '/') token++;

    while (*token && fb->breadcrumb_count < FB_MAX_BREADCRUMBS) {
        // Find next slash
        next = token;
        while (*next && *next != '/') next++;

        // Temporarily null-terminate
        char saved = *next;
        *next = '\0';

        // Build the full path up to this segment
        strcat(built_path, "/");
        strcat(built_path, token);

        // Add breadcrumb
        strncpy(fb->breadcrumbs[fb->breadcrumb_count].name, token, 63);
        fb->breadcrumbs[fb->breadcrumb_count].name[63] = '\0';
        strncpy(fb->breadcrumbs[fb->breadcrumb_count].path, built_path, FB_MAX_PATH - 1);
        fb->breadcrumbs[fb->breadcrumb_count].path[FB_MAX_PATH - 1] = '\0';
        fb->breadcrumbs[fb->breadcrumb_count].w = strlen(token) * FB_CHAR_W + FB_BREADCRUMB_SEP_W;
        fb->breadcrumb_count++;

        // Restore and move to next
        *next = saved;
        if (saved == '\0') break;
        token = next + 1;
    }

    // Calculate x positions
    int32_t x_pos = FB_TOOLBAR_PADDING;
    for (int i = 0; i < fb->breadcrumb_count; i++) {
        fb->breadcrumbs[i].x = x_pos;
        x_pos += fb->breadcrumbs[i].w;
    }
}

// Draw the breadcrumb navigation bar
void fb_draw_breadcrumbs(filebrowser_t *fb) {
    if (!fb || !fb->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(fb->window, &wx, &wy, &ww, &wh);

    int32_t bar_y = wy + FB_TOOLBAR_HEIGHT;

    // Fill breadcrumb bar background
    fb_fill_rect(wx, bar_y, ww, FB_BREADCRUMB_HEIGHT, FB_PATH_BG_COLOR & 0xFFFFFF);

    // Draw each breadcrumb segment
    for (int i = 0; i < fb->breadcrumb_count; i++) {
        fb_breadcrumb_t *bc = &fb->breadcrumbs[i];
        int32_t bc_x = wx + bc->x;
        int32_t text_y = bar_y + (FB_BREADCRUMB_HEIGHT - FB_CHAR_H) / 2;

        // Draw hover background
        if (i == fb->breadcrumb_hover) {
            fb_fill_rect(bc_x, bar_y + 2, bc->w - FB_BREADCRUMB_SEP_W + 4, FB_BREADCRUMB_HEIGHT - 4,
                        FB_BREADCRUMB_HOVER & 0xFFFFFF);
        }

        // Draw segment name
        fb_draw_string_nobg(bc_x + 2, text_y, bc->name, FB_TEXT_COLOR & 0xFFFFFF);

        // Draw separator ">" after each segment except the last
        if (i < fb->breadcrumb_count - 1) {
            int32_t sep_x = bc_x + bc->w - FB_BREADCRUMB_SEP_W + 4;
            fb_draw_string_nobg(sep_x, text_y, ">", FB_BREADCRUMB_SEP & 0xFFFFFF);
        }
    }

    // Draw separator line at bottom
    fb_fill_rect(wx, bar_y + FB_BREADCRUMB_HEIGHT - 1, ww, 1, 0x808080);
}

// Handle breadcrumb click
bool fb_handle_breadcrumb_click(filebrowser_t *fb, int32_t x, int32_t y) {
    if (!fb || !fb->window) return false;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(fb->window, &wx, &wy, &ww, &wh);

    int32_t bar_y = wy + FB_TOOLBAR_HEIGHT;

    // Check if click is in breadcrumb area
    if (y < bar_y || y >= bar_y + FB_BREADCRUMB_HEIGHT) return false;

    // Check each breadcrumb
    for (int i = 0; i < fb->breadcrumb_count; i++) {
        fb_breadcrumb_t *bc = &fb->breadcrumbs[i];
        int32_t bc_x = wx + bc->x;

        if (x >= bc_x && x < bc_x + bc->w) {
            filebrowser_navigate(fb, bc->path);
            return true;
        }
    }
    return false;
}

// Update breadcrumb hover state
void fb_update_breadcrumb_hover(filebrowser_t *fb, int32_t x, int32_t y) {
    if (!fb || !fb->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(fb->window, &wx, &wy, &ww, &wh);

    int32_t bar_y = wy + FB_TOOLBAR_HEIGHT;

    fb->breadcrumb_hover = -1;

    if (y < bar_y || y >= bar_y + FB_BREADCRUMB_HEIGHT) return;

    for (int i = 0; i < fb->breadcrumb_count; i++) {
        fb_breadcrumb_t *bc = &fb->breadcrumbs[i];
        int32_t bc_x = wx + bc->x;

        if (x >= bc_x && x < bc_x + bc->w) {
            fb->breadcrumb_hover = i;
            return;
        }
    }
}

// ============================================
// Status Bar Implementation
// ============================================

// Draw the status bar
void fb_draw_statusbar(filebrowser_t *fb) {
    if (!fb || !fb->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(fb->window, &wx, &wy, &ww, &wh);

    int32_t bar_y = wy + wh - FB_STATUSBAR_HEIGHT;

    // Fill status bar background
    fb_fill_rect(wx, bar_y, ww, FB_STATUSBAR_HEIGHT, FB_STATUSBAR_BG & 0xFFFFFF);

    // Draw separator line at top
    fb_fill_rect(wx, bar_y, ww, 1, 0xC0C0C0);

    // Build status text
    char status[128];
    int text_y = bar_y + (FB_STATUSBAR_HEIGHT - FB_CHAR_H) / 2 + 1;

    // Count items
    fb_itoa(fb->entry_count, status);
    strcat(status, " items");

    // Add selection info if something is selected
    if (fb->selected_index >= 0 && fb->selected_index < fb->entry_count) {
        fb_entry_t *entry = &fb->entries[fb->selected_index];
        strcat(status, " | Selected: ");
        strcat(status, entry->name);

        if (!entry->is_dir) {
            strcat(status, " (");
            char size_buf[32];
            fb_format_size(entry->size, size_buf);
            strcat(status, size_buf);
            strcat(status, ")");
        }
    }

    fb_draw_string_nobg(wx + 4, text_y, status, FB_STATUSBAR_TEXT & 0xFFFFFF);
}

// ============================================
// Context Menu Implementation
// ============================================

// Context menu action functions
static void menu_action_open(filebrowser_t *fb) {
    if (fb->selected_index >= 0) {
        fb_entry_t *entry = &fb->entries[fb->selected_index];
        if (entry->is_dir) {
            char new_path[FB_MAX_PATH];
            if (strcmp(entry->name, "..") == 0) {
                fb_go_up_directory(fb);
            } else {
                strcpy(new_path, fb->current_path);
                int len = strlen(new_path);
                if (len > 0 && new_path[len - 1] != '/') {
                    strcat(new_path, "/");
                }
                strcat(new_path, entry->name);
                filebrowser_navigate(fb, new_path);
            }
        }
    }
}

static void menu_action_cut(filebrowser_t *fb) {
    filebrowser_cut_selected(fb);
}

static void menu_action_copy(filebrowser_t *fb) {
    filebrowser_copy_selected(fb);
}

static void menu_action_paste(filebrowser_t *fb) {
    filebrowser_paste(fb);
}

static void menu_action_delete(filebrowser_t *fb) {
    filebrowser_delete_selected(fb);
}

static void menu_action_newfolder(filebrowser_t *fb) {
    filebrowser_create_folder(fb, "New Folder");
}

// Open file/folder properties dialog
static void menu_action_properties(filebrowser_t *fb) {
    if (!fb) return;
    
    fb_entry_t *entry = filebrowser_get_selected(fb);
    if (!entry || strcmp(entry->name, "..") == 0) return;
    
    // Build full path
    char filepath[FB_MAX_PATH];
    strcpy(filepath, fb->current_path);
    if (filepath[strlen(filepath) - 1] != '/')
        strcat(filepath, "/");
    strcat(filepath, entry->name);
    
    kprintf("[FileBrowser] Opening properties for: %s\n", filepath);
    
    // Create and show properties dialog
    properties_dialog_t *dlg = properties_create(filepath);
    if (dlg) {
        properties_run(dlg);
        properties_destroy(dlg);
        // Refresh to show any changes
        filebrowser_refresh(fb);
    }
}

static void menu_action_refresh(filebrowser_t *fb) {
    filebrowser_refresh(fb);
}

// Initialize context menu items
void fb_init_context_menu(filebrowser_t *fb) {
    if (!fb) return;

    int idx = 0;

    fb->menu_items[idx].label = "Open";
    fb->menu_items[idx].action = menu_action_open;
    fb->menu_items[idx].enabled = true;
    fb->menu_items[idx].separator_after = true;
    idx++;

    fb->menu_items[idx].label = "Cut";
    fb->menu_items[idx].action = menu_action_cut;
    fb->menu_items[idx].enabled = true;
    fb->menu_items[idx].separator_after = false;
    idx++;

    fb->menu_items[idx].label = "Copy";
    fb->menu_items[idx].action = menu_action_copy;
    fb->menu_items[idx].enabled = true;
    fb->menu_items[idx].separator_after = false;
    idx++;

    fb->menu_items[idx].label = "Paste";
    fb->menu_items[idx].action = menu_action_paste;
    fb->menu_items[idx].enabled = false;  // Enabled when clipboard has content
    fb->menu_items[idx].separator_after = true;
    idx++;

    fb->menu_items[idx].label = "Delete";
    fb->menu_items[idx].action = menu_action_delete;
    fb->menu_items[idx].enabled = true;
    fb->menu_items[idx].separator_after = true;
    idx++;

    fb->menu_items[idx].label = "New Folder";
    fb->menu_items[idx].action = menu_action_newfolder;
    fb->menu_items[idx].enabled = true;
    fb->menu_items[idx].separator_after = false;
    idx++;

    fb->menu_items[idx].label = "Refresh";
    fb->menu_items[idx].action = menu_action_refresh;
    fb->menu_items[idx].enabled = true;
    fb->menu_items[idx].separator_after = false;
    idx++;


    fb->menu_items[idx].label = "Properties";
    fb->menu_items[idx].action = menu_action_properties;
    fb->menu_items[idx].enabled = true;
    fb->menu_items[idx].separator_after = false;
    idx++;
    fb->menu_item_count = idx;
    fb->menu_visible = false;
    fb->menu_hover_index = -1;
}

// Show context menu at position
void fb_show_context_menu(filebrowser_t *fb, int32_t x, int32_t y) {
    if (!fb) return;

    // Update menu item states
    bool has_selection = (fb->selected_index >= 0 && fb->selected_index < fb->entry_count);
    bool is_parent = has_selection && strcmp(fb->entries[fb->selected_index].name, "..") == 0;

    // Open - enabled if something is selected
    fb->menu_items[0].enabled = has_selection;

    // Cut/Copy - enabled if something is selected and not ".."
    fb->menu_items[1].enabled = has_selection && !is_parent;  // Cut
    fb->menu_items[2].enabled = has_selection && !is_parent;  // Copy

    // Paste - enabled if clipboard has content
    fb->menu_items[3].enabled = filebrowser_has_clipboard(fb);

    // Delete - enabled if something is selected and not ".."

    // Properties - enabled if something is selected and not ".."
    fb->menu_items[7].enabled = has_selection && !is_parent;
    fb->menu_items[4].enabled = has_selection && !is_parent;

    // Calculate menu dimensions
    int32_t max_width = FB_MENU_MIN_WIDTH;
    for (int i = 0; i < fb->menu_item_count; i++) {
        int label_width = strlen(fb->menu_items[i].label) * FB_CHAR_W + FB_MENU_ITEM_PADDING * 2;
        if (label_width > max_width) max_width = label_width;
    }

    fb->menu_width = max_width;
    fb->menu_height = fb->menu_item_count * FB_MENU_ITEM_HEIGHT + 4;

    // Add height for separators
    for (int i = 0; i < fb->menu_item_count; i++) {
        if (fb->menu_items[i].separator_after) {
            fb->menu_height += 4;
        }
    }

    // Position menu (ensure it stays on screen)
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();

    fb->menu_x = x;
    fb->menu_y = y;

    if (fb->menu_x + fb->menu_width > (int32_t)screen_w) {
        fb->menu_x = screen_w - fb->menu_width;
    }
    if (fb->menu_y + fb->menu_height > (int32_t)screen_h) {
        fb->menu_y = screen_h - fb->menu_height;
    }

    fb->menu_visible = true;
    fb->menu_hover_index = -1;
}

// Hide the context menu
void fb_hide_context_menu(filebrowser_t *fb) {
    if (!fb) return;
    fb->menu_visible = false;
    fb->menu_hover_index = -1;
}

// Draw the context menu
void fb_draw_context_menu(filebrowser_t *fb) {
    if (!fb || !fb->menu_visible) return;

    // Draw menu background
    fb_fill_rect(fb->menu_x, fb->menu_y, fb->menu_width, fb->menu_height, FB_MENU_BG & 0xFFFFFF);

    // Draw border
    fb_draw_rect(fb->menu_x, fb->menu_y, fb->menu_width, fb->menu_height, FB_MENU_BORDER & 0xFFFFFF);

    // Draw menu items
    int32_t item_y = fb->menu_y + 2;
    for (int i = 0; i < fb->menu_item_count; i++) {
        fb_menu_item_t *item = &fb->menu_items[i];

        // Draw hover highlight
        if (i == fb->menu_hover_index && item->enabled) {
            fb_fill_rect(fb->menu_x + 2, item_y, fb->menu_width - 4, FB_MENU_ITEM_HEIGHT,
                        FB_MENU_HOVER & 0xFFFFFF);
        }

        // Draw item text
        uint32_t text_color = item->enabled ? FB_TEXT_COLOR : FB_MENU_DISABLED;
        if (i == fb->menu_hover_index && item->enabled) {
            text_color = FB_SELECTED_FG;
        }

        fb_draw_string_nobg(fb->menu_x + FB_MENU_ITEM_PADDING, item_y + 3,
                           item->label, text_color & 0xFFFFFF);

        item_y += FB_MENU_ITEM_HEIGHT;

        // Draw separator if needed
        if (item->separator_after) {
            fb_fill_rect(fb->menu_x + 4, item_y, fb->menu_width - 8, 1, FB_MENU_BORDER & 0xFFFFFF);
            item_y += 4;
        }
    }
}

// Handle context menu click
bool fb_handle_context_menu_click(filebrowser_t *fb, int32_t x, int32_t y) {
    if (!fb || !fb->menu_visible) return false;

    // Check if click is outside menu
    if (x < fb->menu_x || x >= fb->menu_x + fb->menu_width ||
        y < fb->menu_y || y >= fb->menu_y + fb->menu_height) {
        fb_hide_context_menu(fb);
        return true;  // Click was handled (menu was closed)
    }

    // Check which item was clicked
    int32_t item_y = fb->menu_y + 2;
    for (int i = 0; i < fb->menu_item_count; i++) {
        fb_menu_item_t *item = &fb->menu_items[i];

        if (y >= item_y && y < item_y + FB_MENU_ITEM_HEIGHT) {
            if (item->enabled && item->action) {
                fb_hide_context_menu(fb);
                item->action(fb);
                return true;
            }
            fb_hide_context_menu(fb);
            return true;
        }

        item_y += FB_MENU_ITEM_HEIGHT;
        if (item->separator_after) {
            item_y += 4;
        }
    }

    return false;
}

// Update context menu hover state
void fb_update_context_menu_hover(filebrowser_t *fb, int32_t x, int32_t y) {
    if (!fb || !fb->menu_visible) return;

    fb->menu_hover_index = -1;

    // Check if mouse is over menu
    if (x < fb->menu_x || x >= fb->menu_x + fb->menu_width ||
        y < fb->menu_y || y >= fb->menu_y + fb->menu_height) {
        return;
    }

    // Find which item is hovered
    int32_t item_y = fb->menu_y + 2;
    for (int i = 0; i < fb->menu_item_count; i++) {
        fb_menu_item_t *item = &fb->menu_items[i];

        if (y >= item_y && y < item_y + FB_MENU_ITEM_HEIGHT) {
            fb->menu_hover_index = i;
            return;
        }

        item_y += FB_MENU_ITEM_HEIGHT;
        if (item->separator_after) {
            item_y += 4;
        }
    }
}

// ============================================
// Keyboard Shortcut Handling
// ============================================

// Handle keyboard shortcuts
bool fb_handle_keyboard(filebrowser_t *fb, uint32_t keycode, char c) {
    if (!fb) return false;

    // Hide context menu on any key press
    if (fb->menu_visible) {
        fb_hide_context_menu(fb);
    }

    // Backspace - Go up one directory
    if (c == 8 || c == 127) {  // Backspace or DEL
        // Check if Ctrl is held for delete
        if (fb->ctrl_held) {
            // Ctrl+Backspace = Delete
            if (filebrowser_delete_selected(fb) == 0) {
                return true;
            }
        } else {
            // Plain Backspace = Go up
            fb_go_up_directory(fb);
            return true;
        }
    }

    // F5 - Refresh (F5 scancode is typically 0x3F, but we might receive it as a char)
    if (keycode == 0x3F || c == 'r' || c == 'R') {
        filebrowser_refresh(fb);
        return true;
    }

    // Ctrl+N - New folder
    if (c == 14 || (fb->ctrl_held && (c == 'n' || c == 'N'))) {
        filebrowser_create_folder(fb, "New Folder");
        return true;
    }

    // Ctrl+C - Copy
    if (c == 3 || (fb->ctrl_held && (c == 'c' || c == 'C'))) {
        filebrowser_copy_selected(fb);
        return true;
    }

    // Ctrl+X - Cut
    if (c == 24 || (fb->ctrl_held && (c == 'x' || c == 'X'))) {
        filebrowser_cut_selected(fb);
        return true;
    }

    // Ctrl+V - Paste
    if (c == 22 || (fb->ctrl_held && (c == 'v' || c == 'V'))) {
        filebrowser_paste(fb);
        return true;
    }

    // Home - Go to root
    // Home key often sends different codes, check common ones
    if (keycode == 0x47) {  // Home scancode
        filebrowser_navigate(fb, "/");
        return true;
    }

    // ESC - Close file browser
    if (c == 27) {
        fb->running = false;
        return true;
    }

    // Enter - Open selected
    if (c == '\n' || c == '\r') {
        if (fb->selected_index >= 0 && fb->selected_index < fb->entry_count) {
            fb_entry_t *entry = &fb->entries[fb->selected_index];
            if (entry->is_dir) {
                if (strcmp(entry->name, "..") == 0) {
                    fb_go_up_directory(fb);
                } else {
                    char new_path[FB_MAX_PATH];
                    strcpy(new_path, fb->current_path);
                    int len = strlen(new_path);
                    if (len > 0 && new_path[len - 1] != '/') {
                        strcat(new_path, "/");
                    }
                    strcat(new_path, entry->name);
                    filebrowser_navigate(fb, new_path);
                }
            }
        }
        return true;
    }

    // Arrow keys for navigation
    if (keycode == 0x48) {  // Up arrow
        if (fb->selected_index > 0) {
            fb->selected_index--;
            if (fb->selected_index < fb->scroll_offset) {
                fb->scroll_offset = fb->selected_index;
            }
        }
        return true;
    }

    if (keycode == 0x50) {  // Down arrow
        if (fb->selected_index < fb->entry_count - 1) {
            fb->selected_index++;
            if (fb->selected_index >= fb->scroll_offset + fb->visible_entries) {
                fb->scroll_offset = fb->selected_index - fb->visible_entries + 1;
            }
        }
        return true;
    }

    // View mode shortcuts: 1/2/3/4 or Ctrl+1/2/3/4
    if (c == '1') {
        filebrowser_set_view_mode(fb, FB_VIEW_LIST);
        return true;
    }
    if (c == '2') {
        filebrowser_set_view_mode(fb, FB_VIEW_DETAILS);
        return true;
    }
    if (c == '3') {
        filebrowser_set_view_mode(fb, FB_VIEW_ICONS);
        return true;
    }
    if (c == '4') {
        filebrowser_set_view_mode(fb, FB_VIEW_THUMBNAILS);
        return true;
    }

    // V key - cycle through view modes
    if (c == 'v' || c == 'V') {
        filebrowser_cycle_view_mode(fb);
        return true;
    }

    // P key - toggle preview pane
    if (c == 'p' || c == 'P') {
        fb_toggle_preview(fb);
        return true;
    }

    return false;
}

// ============================================
// Helper Functions
// ============================================

// Go up one directory
static void fb_go_up_directory(filebrowser_t *fb) {
    if (!fb) return;

    if (strcmp(fb->current_path, "/") == 0) {
        return;  // Already at root
    }

    char new_path[FB_MAX_PATH];
    strcpy(new_path, fb->current_path);

    // Find last slash
    int len = strlen(new_path);
    if (len > 1) {
        // Remove trailing slash if present
        if (new_path[len - 1] == '/') {
            new_path[len - 1] = '\0';
            len--;
        }
        // Find previous slash
        while (len > 0 && new_path[len - 1] != '/') {
            len--;
        }
        if (len <= 1) {
            strcpy(new_path, "/");
        } else {
            new_path[len] = '\0';
        }
    }

    filebrowser_navigate(fb, new_path);
}

// ============================================
// Draw Functions (Updated)
// ============================================

// Draw file list content area
static void fb_draw_content(filebrowser_t *fb) {
    if (!fb || !fb->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(fb->window, &wx, &wy, &ww, &wh);

    // Content area starts below toolbar and breadcrumbs
    int32_t content_y = wy + FB_TOOLBAR_HEIGHT + FB_BREADCRUMB_HEIGHT;
    int32_t content_h = wh - FB_TOOLBAR_HEIGHT - FB_BREADCRUMB_HEIGHT - FB_STATUSBAR_HEIGHT;

    // Adjust width if preview pane is visible
    int32_t list_width = ww;
    if (fb->show_preview) {
        list_width = ww - FB_PREVIEW_WIDTH - 2;  // 2px for separator
    }

    // Fill content area with background
    fb_fill_rect(wx, content_y, list_width, content_h, FB_BG_COLOR & 0xFFFFFF);

    // Draw based on view mode
    switch (fb->view_mode) {
        case FB_VIEW_LIST:
            fb_draw_list_view(fb, wx, content_y, list_width, content_h);
            break;
        case FB_VIEW_DETAILS:
            fb_draw_details_view(fb, wx, content_y, list_width, content_h);
            break;
        case FB_VIEW_ICONS:
            fb_draw_icon_view(fb, wx, content_y, list_width, content_h);
            break;
        case FB_VIEW_THUMBNAILS:
            fb_draw_thumbnail_view(fb, wx, content_y, list_width, content_h);
            break;
        default:
            fb_draw_list_view(fb, wx, content_y, list_width, content_h);
            break;
    }
}

// Full redraw of file browser
static void fb_redraw(filebrowser_t *fb) {
    if (!fb || !fb->window) return;

    // Draw window frame
    window_draw(fb->window);

    // Draw toolbar
    fb_draw_toolbar(fb);

    // Draw breadcrumbs
    fb_draw_breadcrumbs(fb);

    // Draw content
    fb_draw_content(fb);

    // Draw status bar
    fb_draw_statusbar(fb);

    // Draw preview pane if visible
    if (fb->show_preview) {
        fb_draw_preview_pane(fb);
    }

    // Draw context menu if visible
    if (fb->menu_visible) {
        fb_draw_context_menu(fb);
    }
}

// Compare function for sorting entries (directories first, then alphabetical)
static int fb_compare_entries(const fb_entry_t *a, const fb_entry_t *b) {
    // ".." always comes first
    if (strcmp(a->name, "..") == 0) return -1;
    if (strcmp(b->name, "..") == 0) return 1;

    // Directories before files
    if (a->is_dir && !b->is_dir) return -1;
    if (!a->is_dir && b->is_dir) return 1;

    // Alphabetical order (case insensitive)
    const char *pa = a->name;
    const char *pb = b->name;
    while (*pa && *pb) {
        char ca = (*pa >= 'A' && *pa <= 'Z') ? *pa + 32 : *pa;
        char cb = (*pb >= 'A' && *pb <= 'Z') ? *pb + 32 : *pb;
        if (ca != cb) return ca - cb;
        pa++;
        pb++;
    }
    return *pa - *pb;
}

// Simple bubble sort for entries
static void fb_sort_entries(filebrowser_t *fb) {
    for (int i = 0; i < fb->entry_count - 1; i++) {
        for (int j = 0; j < fb->entry_count - i - 1; j++) {
            if (fb_compare_entries(&fb->entries[j], &fb->entries[j + 1]) > 0) {
                fb_entry_t tmp = fb->entries[j];
                fb->entries[j] = fb->entries[j + 1];
                fb->entries[j + 1] = tmp;
            }
        }
    }
}

// Navigate to a directory
int filebrowser_navigate(filebrowser_t *fb, const char *path) {
    if (!fb || !path) return -1;

    kprintf("[FileBrowser] Navigating to: %s\n", path);

    // Check if filesystem is mounted
    if (!g_fat_fs.mounted) {
        kprintf("[FileBrowser] Filesystem not mounted\n");
        return -1;
    }

    // Open the directory
    fat_file_t dir;
    if (fat_open(&g_fat_fs, path, &dir) != 0) {
        kprintf("[FileBrowser] Failed to open directory: %s\n", path);
        return -1;
    }

    if (!dir.is_dir) {
        kprintf("[FileBrowser] Path is not a directory: %s\n", path);
        fat_close(&dir);
        return -1;
    }

    // Clear current entries
    fb->entry_count = 0;
    fb->selected_index = -1;
    fb->scroll_offset = 0;

    // Update current path
    strncpy(fb->current_path, path, FB_MAX_PATH - 1);
    fb->current_path[FB_MAX_PATH - 1] = '\0';

    // Update breadcrumbs
    fb_update_breadcrumbs(fb);

    // Add ".." entry if not at root
    if (strcmp(path, "/") != 0) {
        strcpy(fb->entries[fb->entry_count].name, "..");
        fb->entries[fb->entry_count].size = 0;
        fb->entries[fb->entry_count].attr = FAT_ATTR_DIRECTORY;
        fb->entries[fb->entry_count].is_dir = true;
        fb->entry_count++;
    }

    // Read directory entries
    fat_dir_entry_t fat_entry;
    char name[256];

    while (fat_readdir(&dir, &fat_entry, name) == 0) {
        if (fb->entry_count >= FB_MAX_ENTRIES) {
            kprintf("[FileBrowser] Entry limit reached\n");
            break;
        }

        // Skip "." entry
        if (strcmp(name, ".") == 0) continue;

        // Skip ".." entry (we added it above)
        if (strcmp(name, "..") == 0) continue;

        // Skip hidden and system files
        if (fat_entry.attr & (FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM)) continue;

        // Skip volume label
        if (fat_entry.attr & FAT_ATTR_VOLUME_ID) continue;

        // Add entry
        strncpy(fb->entries[fb->entry_count].name, name, FB_NAME_MAX - 1);
        fb->entries[fb->entry_count].name[FB_NAME_MAX - 1] = '\0';
        fb->entries[fb->entry_count].size = fat_entry.file_size;
        fb->entries[fb->entry_count].attr = fat_entry.attr;
        fb->entries[fb->entry_count].is_dir = (fat_entry.attr & FAT_ATTR_DIRECTORY) != 0;
        fb->entry_count++;
    }

    fat_close(&dir);

    // Sort entries (directories first, then alphabetical)
    fb_sort_entries(fb);

    kprintf("[FileBrowser] Loaded %d entries\n", fb->entry_count);
    return 0;
}

// Refresh current directory listing
void filebrowser_refresh(filebrowser_t *fb) {
    if (!fb) return;
    filebrowser_navigate(fb, fb->current_path);
}

// Handle click on an entry
static void fb_handle_click(filebrowser_t *fb, int32_t x, int32_t y) {
    if (!fb || !fb->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(fb->window, &wx, &wy, &ww, &wh);

    // Check if click is in content area
    int32_t content_y = wy + FB_TOOLBAR_HEIGHT + FB_BREADCRUMB_HEIGHT;
    int32_t content_h = wh - FB_TOOLBAR_HEIGHT - FB_BREADCRUMB_HEIGHT - FB_STATUSBAR_HEIGHT;

    // Adjust width if preview pane is visible
    int32_t list_width = ww;
    if (fb->show_preview) {
        list_width = ww - FB_PREVIEW_WIDTH - 2;
    }

    if (y < content_y || y >= content_y + content_h) return;
    if (x < wx || x >= wx + list_width) return;  // Click is outside file list area

    // Calculate which entry was clicked
    int click_row = (y - content_y) / FB_ENTRY_HEIGHT;
    int entry_index = click_row + fb->scroll_offset;

    if (entry_index >= fb->entry_count) {
        fb->selected_index = -1;
        return;
    }

    // If clicking on already selected entry, navigate into it
    if (entry_index == fb->selected_index) {
        fb_entry_t *entry = &fb->entries[entry_index];

        if (entry->is_dir) {
            // Navigate into directory
            char new_path[FB_MAX_PATH];

            if (strcmp(entry->name, "..") == 0) {
                // Go up one level
                fb_go_up_directory(fb);
            } else {
                // Navigate into subdirectory
                strcpy(new_path, fb->current_path);
                int len = strlen(new_path);

                // Add trailing slash if needed
                if (len > 0 && new_path[len - 1] != '/') {
                    strcat(new_path, "/");
                }
                strcat(new_path, entry->name);
                filebrowser_navigate(fb, new_path);
            }
        }
    } else {
        // Select the entry
        fb->selected_index = entry_index;

        // Load preview for the newly selected entry
        if (fb->show_preview && entry_index >= 0 && entry_index < fb->entry_count) {
            fb_load_preview(fb, &fb->entries[entry_index]);
        }
    }
}

// Create and show a file browser window
filebrowser_t *filebrowser_create(void) {
    filebrowser_t *fb = (filebrowser_t *)kmalloc(sizeof(filebrowser_t));
    if (!fb) {
        kprintf("[FileBrowser] Failed to allocate file browser\n");
        return NULL;
    }

    memset(fb, 0, sizeof(filebrowser_t));

    // Center on screen
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();
    int x = (screen_w - FB_WINDOW_WIDTH) / 2;
    int y = (screen_h - FB_WINDOW_HEIGHT) / 2 - 50;

    // Create window
    fb->window = window_create("Files", x, y, FB_WINDOW_WIDTH, FB_WINDOW_HEIGHT);
    if (!fb->window) {
        kprintf("[FileBrowser] Failed to create window\n");
        kfree(fb);
        return NULL;
    }

    // Set window colors
    fb->window->bg_color = FB_BG_COLOR & 0xFFFFFF;

    fb->selected_index = -1;
    fb->scroll_offset = 0;
    fb->running = true;

    // Initialize clipboard
    fb->clipboard.operation = CLIPBOARD_NONE;
    fb->clipboard.path[0] = '\0';
    fb->clipboard.name[0] = '\0';
    fb->clipboard.is_dir = false;

    // Initialize preview pane state
    fb->show_preview = true;  // Preview pane visible by default
    fb->preview_entry_index = -1;
    memset(&fb->preview, 0, sizeof(fb->preview));

    // Initialize toolbar
    fb_init_toolbar(fb);

    // Initialize context menu
    fb_init_context_menu(fb);

    // Initialize view mode
    fb->view_mode = FB_VIEW_LIST;

    // Initialize keyboard modifiers
    fb->ctrl_held = false;
    fb->shift_held = false;

    // Set initial path
    strcpy(fb->current_path, "/");

    // Load initial directory
    if (g_fat_fs.mounted) {
        filebrowser_navigate(fb, "/");
    } else {
        fb->entry_count = 0;
        fb_update_breadcrumbs(fb);
        kprintf("[FileBrowser] Warning: Filesystem not mounted\n");
    }

    return fb;
}

// Destroy file browser
void filebrowser_destroy(filebrowser_t *fb) {
    if (!fb) return;

    // Clear preview data
    fb_clear_preview(fb);

    if (fb->window) {
        window_destroy(fb->window);
    }
    kfree(fb);
}

// Run file browser main loop
void filebrowser_run(filebrowser_t *fb) {
    if (!fb) return;

    kprintf("[FileBrowser] Running file browser...\n");

    // Draw initial state
    fb_redraw(fb);

    // Track mouse state for dragging
    int32_t last_mouse_x = 0, last_mouse_y = 0;
    uint8_t last_buttons = 0;
    mouse_get_position(&last_mouse_x, &last_mouse_y);

    while (fb->running) {
        // Poll mouse for window dragging
        mouse_poll();
        int32_t mouse_x, mouse_y;
        uint8_t buttons;
        mouse_get_position(&mouse_x, &mouse_y);
        buttons = mouse_get_buttons();

        // Handle mouse button press
        if ((buttons & MOUSE_LEFT_BTN) && !(last_buttons & MOUSE_LEFT_BTN)) {
            // Left click - handle context menu first
            if (fb->menu_visible) {
                if (fb_handle_context_menu_click(fb, mouse_x, mouse_y)) {
                    fb_redraw(fb);
                }
            } else {
                // Check if click is on window title bar
                if (mouse_y >= fb->window->bounds.y &&
                    mouse_y < fb->window->bounds.y + TITLEBAR_HEIGHT &&
                    mouse_x >= fb->window->bounds.x &&
                    mouse_x < fb->window->bounds.x + fb->window->bounds.width) {
                    // Start dragging
                    wm_handle_mouse_down(mouse_x, mouse_y, MOUSE_LEFT_BTN);
                } else {
                    // Check toolbar
                    if (fb_handle_toolbar_click(fb, mouse_x, mouse_y)) {
                        fb_redraw(fb);
                    }
                    // Check breadcrumbs
                    else if (fb_handle_breadcrumb_click(fb, mouse_x, mouse_y)) {
                        fb_redraw(fb);
                    }
                    // Handle click in content area
                    else {
                        fb_handle_click(fb, mouse_x, mouse_y);
                        fb_redraw(fb);
                    }
                }
            }
        }

        // Handle right-click for context menu
        if ((buttons & MOUSE_RIGHT_BTN) && !(last_buttons & MOUSE_RIGHT_BTN)) {
            // Show context menu
            fb_show_context_menu(fb, mouse_x, mouse_y);
            fb_redraw(fb);
        }

        // Handle mouse movement
        if (mouse_x != last_mouse_x || mouse_y != last_mouse_y) {
            wm_handle_mouse_move(mouse_x, mouse_y);

            // Update hover states
            fb_update_toolbar_hover(fb, mouse_x, mouse_y);
            fb_update_breadcrumb_hover(fb, mouse_x, mouse_y);
            if (fb->menu_visible) {
                fb_update_context_menu_hover(fb, mouse_x, mouse_y);
            }

            // Always redraw to prevent cursor trails
            desktop_draw();
            fb_redraw(fb);
            desktop_draw_cursor(mouse_x, mouse_y);
            last_mouse_x = mouse_x;
            last_mouse_y = mouse_y;
        }

        // Handle mouse button release
        if (!(buttons & MOUSE_LEFT_BTN) && (last_buttons & MOUSE_LEFT_BTN)) {
            wm_handle_mouse_up(mouse_x, mouse_y, MOUSE_LEFT_BTN);
        }

        last_buttons = buttons;

        // Check for keyboard input
        if (keyboard_has_char()) {
            char c = keyboard_get_char();
            
            // Use the keyboard handler
            if (fb_handle_keyboard(fb, 0, c)) {
                fb_redraw(fb);
            }
        }

        // Draw cursor
        static const uint8_t cursor[] = {
            0b10000000, 0b00000000, 0b11000000, 0b00000000,
            0b11100000, 0b00000000, 0b11110000, 0b00000000,
            0b11111000, 0b00000000, 0b11111100, 0b00000000,
            0b11111110, 0b00000000, 0b11111111, 0b00000000,
            0b11111111, 0b10000000, 0b11111111, 0b11000000,
            0b11111100, 0b00000000, 0b11101100, 0b00000000,
            0b11000110, 0b00000000, 0b10000110, 0b00000000,
            0b00000011, 0b00000000, 0b00000011, 0b00000000,
        };
        for (int row = 0; row < 16; row++) {
            uint16_t bits = (cursor[row*2] << 8) | cursor[row*2 + 1];
            for (int col = 0; col < 12; col++) {
                if (bits & (0x8000 >> col)) {
                    fb_put_pixel(mouse_x + col, mouse_y + row, 0xFFFFFF);
                    fb_put_pixel(mouse_x + col + 1, mouse_y + row, 0x000000);
                }
            }
        }

        // Small delay
        for (int i = 0; i < 1000; i++) {
            __asm__ volatile("pause");
        }
    }

    kprintf("[FileBrowser] File browser closed\n");
}

// Public draw function
void filebrowser_draw(filebrowser_t *fb) {
    if (!fb || !fb->window) return;
    window_draw(fb->window);
    fb_redraw(fb);
}

// ============================================================================
// Window Manager Callback Functions (non-blocking model)
// ============================================================================

void filebrowser_on_event(void *app_data, gui_event_t *event) {
    filebrowser_t *fb = (filebrowser_t *)app_data;
    if (!fb || !fb->window || !event) return;

    // Get window content bounds
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(fb->window, &wx, &wy, &ww, &wh);
    int32_t local_x = event->mouse_x - wx;
    int32_t local_y = event->mouse_y - wy;

    switch (event->type) {
        case EVENT_MOUSE_MOVE:
            // Update toolbar hover
            fb_update_toolbar_hover(fb, local_x, local_y);
            fb_update_breadcrumb_hover(fb, local_x, local_y);
            if (fb->menu_visible) {
                fb_update_context_menu_hover(fb, local_x, local_y);
            }
            wm_invalidate_rect(&fb->window->bounds);
            break;

        case EVENT_MOUSE_DOWN:
            if (event->mouse_buttons & MOUSE_BUTTON_RIGHT) {
                fb_show_context_menu(fb, local_x, local_y);
                wm_invalidate_rect(&fb->window->bounds);
            }
            break;

        case EVENT_MOUSE_UP:
            if (event->mouse_buttons & MOUSE_BUTTON_LEFT) {
                // Check toolbar click
                if (fb_handle_toolbar_click(fb, local_x, local_y)) {
                    wm_invalidate_rect(&fb->window->bounds);
                    break;
                }
                // Check breadcrumb click
                if (fb_handle_breadcrumb_click(fb, local_x, local_y)) {
                    wm_invalidate_rect(&fb->window->bounds);
                    break;
                }
                // Check context menu click
                if (fb->menu_visible && fb_handle_context_menu_click(fb, local_x, local_y)) {
                    wm_invalidate_rect(&fb->window->bounds);
                    break;
                }
                // Check file entry click
                int entry_idx = fb_get_entry_at_point(fb, event->mouse_x, event->mouse_y);
                if (entry_idx >= 0 && entry_idx < fb->entry_count) {
                    if (entry_idx == fb->selected_index) {
                        // Double-click: open directory or file
                        fb_entry_t *entry = &fb->entries[entry_idx];
                        if (entry->is_dir) {
                            char new_path[FB_MAX_PATH];
                            strcpy(new_path, fb->current_path);
                            strcat(new_path, "/");
                            strcat(new_path, entry->name);
                            filebrowser_navigate(fb, new_path);
                        }
                    } else {
                        fb->selected_index = entry_idx;
                        if (fb->show_preview) {
                            fb_load_preview(fb, &fb->entries[entry_idx]);
                        }
                    }
                    wm_invalidate_rect(&fb->window->bounds);
                }
            }
            break;

        case EVENT_KEY_DOWN:
            {
                char c = event->key_char;
                if (c == 27) {  // ESC - close filebrowser
                    kprintf("[FileBrowser] ESC pressed, closing\n");
                    wm_unregister_app(fb->app_id);
                    if (fb->dock_index >= 0) {
                        dock_remove_app(fb->dock_index);
                    }
                    if (g_active_filebrowser == fb) {
                        g_active_filebrowser = NULL;
                    }
                    window_hide(fb->window);
                    wm_invalidate_all();
                    return;
                }
                // Handle keyboard shortcuts
                fb_handle_keyboard(fb, event->keycode, c);
                wm_invalidate_rect(&fb->window->bounds);
            }
            break;

        case EVENT_WINDOW_CLOSE:
            kprintf("[FileBrowser] Close button clicked\n");
            wm_unregister_app(fb->app_id);
            if (fb->dock_index >= 0) {
                dock_remove_app(fb->dock_index);
            }
            if (g_active_filebrowser == fb) {
                g_active_filebrowser = NULL;
            }
            window_hide(fb->window);
            wm_invalidate_all();
            break;

        default:
            break;
    }
}

void filebrowser_on_draw(void *app_data) {
    filebrowser_t *fb = (filebrowser_t *)app_data;
    if (fb) {
        filebrowser_draw(fb);
    }
}

void filebrowser_on_destroy(void *app_data) {
    filebrowser_t *fb = (filebrowser_t *)app_data;
    if (fb) {
        kprintf("[FileBrowser] Destroying filebrowser instance\n");
        if (g_active_filebrowser == fb) {
            g_active_filebrowser = NULL;
        }
        filebrowser_destroy(fb);
    }
}

// Launch callback for dock (non-blocking)
void filebrowser_launch(void) {
    LOG_INFO("[Files] Application launched");
    kprintf("[FileBrowser] Launching file browser (non-blocking)...\n");

    filebrowser_t *fb = filebrowser_create();
    if (!fb) {
        LOG_ERROR("[Files] Failed to create window");
        kprintf("[FileBrowser] Failed to create file browser\n");
        return;
    }

    // Initialize WM integration fields
    fb->app_id = -1;
    fb->dock_index = -1;

    // Add to taskbar
    fb->dock_index = dock_add_app("Files", DOCK_ICON_FILES, NULL);

    // Register with window manager
    fb->app_id = wm_register_app(
        fb->window,
        fb,
        filebrowser_on_event,
        filebrowser_on_draw,
        filebrowser_on_destroy
    );

    if (fb->app_id < 0) {
        kprintf("[FileBrowser] Failed to register with window manager\n");
        if (fb->dock_index >= 0) {
            dock_remove_app(fb->dock_index);
        }
        filebrowser_destroy(fb);
        return;
    }

    g_active_filebrowser = fb;
    wm_invalidate_all();

    kprintf("[FileBrowser] File browser registered as app %d\n", fb->app_id);
}

// ============================================
// File Operations Implementation
// ============================================

// Helper function to build a full path from current directory and name
static void fb_build_path(const char *dir, const char *name, char *out, int max_len) {
    int dir_len = strlen(dir);

    // Start with directory
    strncpy(out, dir, max_len - 1);
    out[max_len - 1] = '\0';

    // Add separator if needed
    if (dir_len > 0 && dir[dir_len - 1] != '/' && dir_len < max_len - 1) {
        out[dir_len] = '/';
        out[dir_len + 1] = '\0';
        dir_len++;
    }

    // Append name
    int name_len = strlen(name);
    if (dir_len + name_len < max_len) {
        strcat(out, name);
    }
}

// Create a new folder in the current directory
int filebrowser_create_folder(filebrowser_t *fb, const char *name) {
    if (!fb || !name || strlen(name) == 0) {
        kprintf("[FileBrowser] create_folder: Invalid parameters\n");
        return -1;
    }

    // Check for invalid characters in name
    const char *p = name;
    while (*p) {
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' ||
            *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') {
            kprintf("[FileBrowser] create_folder: Invalid character in name\n");
            return -1;
        }
        p++;
    }

    // Build full path
    char full_path[FB_MAX_PATH];
    fb_build_path(fb->current_path, name, full_path, FB_MAX_PATH);

    kprintf("[FileBrowser] Creating folder: %s\n", full_path);

    // Check if already exists
    if (fat_exists(&g_fat_fs, full_path) == 1) {
        kprintf("[FileBrowser] create_folder: Path already exists\n");
        return -1;
    }

    // Call filesystem function
    int result = fat_mkdir(&g_fat_fs, full_path);

    if (result == 0) {
        // Refresh directory listing
        filebrowser_refresh(fb);
        kprintf("[FileBrowser] Folder created successfully\n");
    } else {
        kprintf("[FileBrowser] Failed to create folder\n");
    }

    return result;
}

// Create a new empty file in the current directory
int filebrowser_create_file(filebrowser_t *fb, const char *name) {
    if (!fb || !name || strlen(name) == 0) {
        kprintf("[FileBrowser] create_file: Invalid parameters\n");
        return -1;
    }

    // Check for invalid characters in name
    const char *p = name;
    while (*p) {
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' ||
            *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') {
            kprintf("[FileBrowser] create_file: Invalid character in name\n");
            return -1;
        }
        p++;
    }

    // Build full path
    char full_path[FB_MAX_PATH];
    fb_build_path(fb->current_path, name, full_path, FB_MAX_PATH);

    kprintf("[FileBrowser] Creating file: %s\n", full_path);

    // Check if already exists
    if (fat_exists(&g_fat_fs, full_path) == 1) {
        kprintf("[FileBrowser] create_file: Path already exists\n");
        return -1;
    }

    // Call filesystem function
    int result = fat_create(&g_fat_fs, full_path);

    if (result == 0) {
        // Refresh directory listing
        filebrowser_refresh(fb);
        kprintf("[FileBrowser] File created successfully\n");
    } else {
        kprintf("[FileBrowser] Failed to create file\n");
    }

    return result;
}

// Delete the currently selected file or folder
int filebrowser_delete_selected(filebrowser_t *fb) {
    if (!fb) {
        kprintf("[FileBrowser] delete_selected: Invalid file browser\n");
        return -1;
    }

    // Check if something is selected
    if (fb->selected_index < 0 || fb->selected_index >= fb->entry_count) {
        kprintf("[FileBrowser] delete_selected: No item selected\n");
        return -1;
    }

    fb_entry_t *entry = &fb->entries[fb->selected_index];

    // Don't allow deleting ".."
    if (strcmp(entry->name, "..") == 0) {
        kprintf("[FileBrowser] delete_selected: Cannot delete parent directory reference\n");
        return -1;
    }

    // Build full path
    char full_path[FB_MAX_PATH];
    fb_build_path(fb->current_path, entry->name, full_path, FB_MAX_PATH);

    kprintf("[FileBrowser] Deleting: %s (is_dir=%d)\n", full_path, entry->is_dir);

    // Call filesystem function
    int result = fat_delete(&g_fat_fs, full_path);

    if (result == 0) {
        // Clear selection and refresh
        fb->selected_index = -1;
        filebrowser_refresh(fb);
        kprintf("[FileBrowser] Deleted successfully\n");
    } else {
        kprintf("[FileBrowser] Failed to delete\n");
    }

    return result;
}

// Rename the currently selected file or folder
int filebrowser_rename_selected(filebrowser_t *fb, const char *new_name) {
    if (!fb || !new_name || strlen(new_name) == 0) {
        kprintf("[FileBrowser] rename_selected: Invalid parameters\n");
        return -1;
    }

    // Check if something is selected
    if (fb->selected_index < 0 || fb->selected_index >= fb->entry_count) {
        kprintf("[FileBrowser] rename_selected: No item selected\n");
        return -1;
    }

    fb_entry_t *entry = &fb->entries[fb->selected_index];

    // Don't allow renaming ".."
    if (strcmp(entry->name, "..") == 0) {
        kprintf("[FileBrowser] rename_selected: Cannot rename parent directory reference\n");
        return -1;
    }

    // Check for invalid characters in new name
    const char *p = new_name;
    while (*p) {
        if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' ||
            *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') {
            kprintf("[FileBrowser] rename_selected: Invalid character in new name\n");
            return -1;
        }
        p++;
    }

    // Build full paths
    char old_path[FB_MAX_PATH];
    char new_path[FB_MAX_PATH];
    fb_build_path(fb->current_path, entry->name, old_path, FB_MAX_PATH);
    fb_build_path(fb->current_path, new_name, new_path, FB_MAX_PATH);

    kprintf("[FileBrowser] Renaming: %s -> %s\n", old_path, new_path);

    // Check if new name already exists
    if (fat_exists(&g_fat_fs, new_path) == 1) {
        kprintf("[FileBrowser] rename_selected: Target name already exists\n");
        return -1;
    }

    // Call filesystem function
    int result = fat_rename(&g_fat_fs, old_path, new_path);

    if (result == 0) {
        // Refresh directory listing
        filebrowser_refresh(fb);
        kprintf("[FileBrowser] Renamed successfully\n");
    } else {
        kprintf("[FileBrowser] Failed to rename\n");
    }

    return result;
}

// Copy the currently selected file/folder to clipboard
int filebrowser_copy_selected(filebrowser_t *fb) {
    if (!fb) {
        kprintf("[FileBrowser] copy_selected: Invalid file browser\n");
        return -1;
    }

    // Check if something is selected
    if (fb->selected_index < 0 || fb->selected_index >= fb->entry_count) {
        kprintf("[FileBrowser] copy_selected: No item selected\n");
        return -1;
    }

    fb_entry_t *entry = &fb->entries[fb->selected_index];

    // Don't allow copying ".."
    if (strcmp(entry->name, "..") == 0) {
        kprintf("[FileBrowser] copy_selected: Cannot copy parent directory reference\n");
        return -1;
    }

    // Store in clipboard
    fb_build_path(fb->current_path, entry->name, fb->clipboard.path, FB_MAX_PATH);
    strncpy(fb->clipboard.name, entry->name, FB_NAME_MAX - 1);
    fb->clipboard.name[FB_NAME_MAX - 1] = '\0';
    fb->clipboard.is_dir = entry->is_dir;
    fb->clipboard.operation = CLIPBOARD_COPY;

    kprintf("[FileBrowser] Copied to clipboard: %s\n", fb->clipboard.path);
    return 0;
}

// Cut the currently selected file/folder (for move operation)
int filebrowser_cut_selected(filebrowser_t *fb) {
    if (!fb) {
        kprintf("[FileBrowser] cut_selected: Invalid file browser\n");
        return -1;
    }

    // Check if something is selected
    if (fb->selected_index < 0 || fb->selected_index >= fb->entry_count) {
        kprintf("[FileBrowser] cut_selected: No item selected\n");
        return -1;
    }

    fb_entry_t *entry = &fb->entries[fb->selected_index];

    // Don't allow cutting ".."
    if (strcmp(entry->name, "..") == 0) {
        kprintf("[FileBrowser] cut_selected: Cannot cut parent directory reference\n");
        return -1;
    }

    // Store in clipboard
    fb_build_path(fb->current_path, entry->name, fb->clipboard.path, FB_MAX_PATH);
    strncpy(fb->clipboard.name, entry->name, FB_NAME_MAX - 1);
    fb->clipboard.name[FB_NAME_MAX - 1] = '\0';
    fb->clipboard.is_dir = entry->is_dir;
    fb->clipboard.operation = CLIPBOARD_CUT;

    kprintf("[FileBrowser] Cut to clipboard: %s\n", fb->clipboard.path);
    return 0;
}

// Paste from clipboard to current directory
int filebrowser_paste(filebrowser_t *fb) {
    if (!fb) {
        kprintf("[FileBrowser] paste: Invalid file browser\n");
        return -1;
    }

    // Check if clipboard has content
    if (fb->clipboard.operation == CLIPBOARD_NONE) {
        kprintf("[FileBrowser] paste: Clipboard is empty\n");
        return -1;
    }

    // Build destination path
    char dst_path[FB_MAX_PATH];
    fb_build_path(fb->current_path, fb->clipboard.name, dst_path, FB_MAX_PATH);

    kprintf("[FileBrowser] Pasting: %s -> %s (op=%d)\n",
            fb->clipboard.path, dst_path, fb->clipboard.operation);

    // Check if source still exists
    if (fat_exists(&g_fat_fs, fb->clipboard.path) != 1) {
        kprintf("[FileBrowser] paste: Source no longer exists\n");
        fb->clipboard.operation = CLIPBOARD_NONE;
        return -1;
    }

    // Check if destination already exists
    if (fat_exists(&g_fat_fs, dst_path) == 1) {
        kprintf("[FileBrowser] paste: Destination already exists\n");
        return -1;
    }

    // Check if trying to paste into itself (for directories)
    if (fb->clipboard.is_dir) {
        int src_len = strlen(fb->clipboard.path);
        if (strncmp(fb->current_path, fb->clipboard.path, src_len) == 0 &&
            (fb->current_path[src_len] == '/' || fb->current_path[src_len] == '\0')) {
            kprintf("[FileBrowser] paste: Cannot paste directory into itself\n");
            return -1;
        }
    }

    int result;
    if (fb->clipboard.operation == CLIPBOARD_COPY) {
        // Copy operation
        result = fat_copy(&g_fat_fs, fb->clipboard.path, dst_path);
    } else {
        // Cut/Move operation
        result = fat_move(&g_fat_fs, fb->clipboard.path, dst_path);

        // Clear clipboard after successful move
        if (result == 0) {
            fb->clipboard.operation = CLIPBOARD_NONE;
        }
    }

    if (result == 0) {
        // Refresh directory listing
        filebrowser_refresh(fb);
        kprintf("[FileBrowser] Pasted successfully\n");
    } else {
        kprintf("[FileBrowser] Failed to paste\n");
    }

    return result;
}

// Check if clipboard has content
bool filebrowser_has_clipboard(filebrowser_t *fb) {
    if (!fb) return false;
    return fb->clipboard.operation != CLIPBOARD_NONE;
}

// Get the selected entry (or NULL if none selected)
fb_entry_t *filebrowser_get_selected(filebrowser_t *fb) {
    if (!fb) return NULL;
    if (fb->selected_index < 0 || fb->selected_index >= fb->entry_count) {
        return NULL;
    }
    return &fb->entries[fb->selected_index];
}

// ============================================
// Preview Pane Stubs (if not implemented elsewhere)
// ============================================

// Toggle preview pane visibility
void fb_toggle_preview(filebrowser_t *fb) {
    if (!fb) return;
    fb->show_preview = !fb->show_preview;
    kprintf("[FileBrowser] Preview pane %s\n", fb->show_preview ? "shown" : "hidden");
}

// Clear/free cached preview data
void fb_clear_preview(filebrowser_t *fb) {
    if (!fb) return;

    if (fb->preview.data) {
        kfree(fb->preview.data);
        fb->preview.data = NULL;
    }
    if (fb->preview.pixels) {
        kfree(fb->preview.pixels);
        fb->preview.pixels = NULL;
    }
    fb->preview.type = PREVIEW_NONE;
    fb->preview.data_size = 0;
    fb->preview.img_width = 0;
    fb->preview.img_height = 0;
    fb->preview.text_line_count = 0;
    fb->preview_entry_index = -1;
}

// Load preview data for the selected file
void fb_load_preview(filebrowser_t *fb, fb_entry_t *entry) {
    if (!fb || !entry) return;

    // Check if we already have preview for this entry
    int entry_idx = (int)(entry - fb->entries);
    if (entry_idx == fb->preview_entry_index) {
        return;  // Already loaded
    }

    // Clear existing preview
    fb_clear_preview(fb);
    fb->preview_entry_index = entry_idx;

    if (entry->is_dir) {
        fb->preview.type = PREVIEW_DIRECTORY;
        kprintf("[Preview] Directory: %s\n", entry->name);
        return;
    }

    // Check file size - skip if too large
    if (entry->size > FB_PREVIEW_MAX_FILE_SIZE) {
        kprintf("[Preview] File too large to preview: %u bytes\n", entry->size);
        fb->preview.type = PREVIEW_NONE;
        return;
    }

    // Build full path for the file
    char full_path[FB_MAX_PATH];
    fb_build_path(fb->current_path, entry->name, full_path, FB_MAX_PATH);

    // Determine preview type and load content based on file extension
    if (fb_is_image_file(entry->name)) {
        // Load image preview (only BMP supported)
        kprintf("[Preview] Loading image: %s\n", full_path);

        uint32_t size;
        void *data = fat_read_file(&g_fat_fs, full_path, &size);
        if (data && size > 0) {
            // Try to load as BMP
            image_t img;
            int result = image_load(data, size, &img);

            if (result == IMAGE_SUCCESS) {
                fb->preview.type = PREVIEW_IMAGE;
                fb->preview.img_width = img.width;
                fb->preview.img_height = img.height;
                fb->preview.pixels = img.pixels;  // Take ownership of pixel data
                kprintf("[Preview] Image loaded: %ux%u\n", img.width, img.height);
            } else {
                kprintf("[Preview] Failed to load BMP: %s\n", image_error_string(result));
                fb->preview.type = PREVIEW_NONE;
            }

            // Free the raw file data (pixels are kept if successfully loaded)
            kfree(data);
        } else {
            fb->preview.type = PREVIEW_NONE;
        }
    } else if (fb_is_text_file(entry->name)) {
        // Load text preview
        kprintf("[Preview] Loading text: %s\n", full_path);

        uint32_t size;
        void *data = fat_read_file(&g_fat_fs, full_path, &size);
        if (data && size > 0) {
            fb->preview.type = PREVIEW_TEXT;
            fb->preview.text_line_count = 0;

            // Parse first few lines of text
            char *p = (char *)data;
            char *line_start = p;
            int col = 0;

            while (*p && fb->preview.text_line_count < FB_PREVIEW_TEXT_LINES) {
                if (*p == '\n' || *p == '\r') {
                    // End of line
                    int line_len = (col < 63) ? col : 63;
                    if (line_len > 0) {
                        memcpy(fb->preview.text_lines[fb->preview.text_line_count], line_start, line_len);
                    }
                    fb->preview.text_lines[fb->preview.text_line_count][line_len] = '\0';
                    fb->preview.text_line_count++;

                    // Skip \r\n or \n\r
                    if ((*p == '\r' && *(p+1) == '\n') || (*p == '\n' && *(p+1) == '\r')) {
                        p++;
                    }
                    p++;
                    line_start = p;
                    col = 0;
                } else {
                    col++;
                    p++;
                }
            }

            // Handle last line if no trailing newline
            if (col > 0 && fb->preview.text_line_count < FB_PREVIEW_TEXT_LINES) {
                int line_len = (col < 63) ? col : 63;
                memcpy(fb->preview.text_lines[fb->preview.text_line_count], line_start, line_len);
                fb->preview.text_lines[fb->preview.text_line_count][line_len] = '\0';
                fb->preview.text_line_count++;
            }

            kprintf("[Preview] Text loaded: %d lines\n", fb->preview.text_line_count);
            kfree(data);
        } else {
            fb->preview.type = PREVIEW_NONE;
        }
    } else {
        // Unknown file type - no preview
        kprintf("[Preview] Unknown file type: %s\n", entry->name);
        fb->preview.type = PREVIEW_NONE;
    }
}

// Draw a scaled image using nearest-neighbor interpolation for preview
static void fb_preview_draw_scaled(uint32_t *pixels, uint32_t src_w, uint32_t src_h,
                                    int32_t dx, int32_t dy, uint32_t dw, uint32_t dh) {
    if (!pixels || src_w == 0 || src_h == 0 || dw == 0 || dh == 0) return;

    uint32_t x_ratio = ((src_w << 16) / dw);
    uint32_t y_ratio = ((src_h << 16) / dh);

    for (uint32_t y = 0; y < dh; y++) {
        uint32_t src_y = (y * y_ratio) >> 16;
        if (src_y >= src_h) src_y = src_h - 1;
        uint32_t *src_row = pixels + src_y * src_w;

        for (uint32_t x = 0; x < dw; x++) {
            uint32_t src_x = (x * x_ratio) >> 16;
            if (src_x >= src_w) src_x = src_w - 1;
            fb_put_pixel(dx + x, dy + y, src_row[src_x]);
        }
    }
}

// Draw the preview pane
void fb_draw_preview_pane(filebrowser_t *fb) {
    if (!fb || !fb->window || !fb->show_preview) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(fb->window, &wx, &wy, &ww, &wh);

    // Preview pane is on the right side
    int32_t preview_x = wx + ww - FB_PREVIEW_WIDTH;
    int32_t preview_y = wy + FB_TOOLBAR_HEIGHT + FB_BREADCRUMB_HEIGHT;
    int32_t preview_h = wh - FB_TOOLBAR_HEIGHT - FB_BREADCRUMB_HEIGHT - FB_STATUSBAR_HEIGHT;

    // Draw separator line
    fb_fill_rect(preview_x - 2, preview_y, 2, preview_h, FB_PREVIEW_BORDER & 0xFFFFFF);

    // Draw preview pane background
    fb_fill_rect(preview_x, preview_y, FB_PREVIEW_WIDTH, preview_h, FB_PREVIEW_BG & 0xFFFFFF);

    // Draw preview content based on type
    int32_t text_y = preview_y + FB_PREVIEW_PADDING;

    if (fb->selected_index >= 0 && fb->selected_index < fb->entry_count) {
        fb_entry_t *entry = &fb->entries[fb->selected_index];

        // Draw filename (truncate if too long)
        char name_disp[26];
        int nlen = strlen(entry->name);
        if (nlen > 24) {
            strncpy(name_disp, entry->name, 21);
            name_disp[21] = '.';
            name_disp[22] = '.';
            name_disp[23] = '.';
            name_disp[24] = '\0';
        } else {
            strcpy(name_disp, entry->name);
        }
        fb_draw_string_nobg(preview_x + FB_PREVIEW_PADDING, text_y, name_disp, FB_TEXT_COLOR & 0xFFFFFF);
        text_y += FB_CHAR_H + 4;

        // Draw separator
        fb_fill_rect(preview_x + FB_PREVIEW_PADDING, text_y, FB_PREVIEW_WIDTH - FB_PREVIEW_PADDING * 2, 1, FB_PREVIEW_BORDER & 0xFFFFFF);
        text_y += 8;

        // Draw file type
        const char *type_str = "File";
        if (entry->is_dir) type_str = "Folder";
        else if (fb_is_image_file(entry->name)) type_str = "Image";
        else if (fb_is_text_file(entry->name)) type_str = "Text";

        fb_draw_string_nobg(preview_x + FB_PREVIEW_PADDING, text_y, "Type: ", FB_STATUSBAR_TEXT & 0xFFFFFF);
        fb_draw_string_nobg(preview_x + FB_PREVIEW_PADDING + 48, text_y, type_str, FB_TEXT_COLOR & 0xFFFFFF);
        text_y += FB_CHAR_H + 2;

        // Show file size (only for files)
        if (!entry->is_dir) {
            char size_buf[32];
            fb_format_size(entry->size, size_buf);
            fb_draw_string_nobg(preview_x + FB_PREVIEW_PADDING, text_y, "Size: ", FB_STATUSBAR_TEXT & 0xFFFFFF);
            fb_draw_string_nobg(preview_x + FB_PREVIEW_PADDING + 48, text_y, size_buf, FB_TEXT_COLOR & 0xFFFFFF);
            text_y += FB_CHAR_H + 2;
        }

        text_y += 8;  // Extra spacing

        // Calculate content area
        int32_t content_y = text_y;
        int32_t content_h = preview_y + preview_h - content_y - FB_PREVIEW_PADDING;
        int32_t content_w = FB_PREVIEW_WIDTH - (2 * FB_PREVIEW_PADDING);

        // Draw preview based on loaded data
        switch (fb->preview.type) {
            case PREVIEW_IMAGE:
                if (fb->preview.pixels && fb->preview.img_width > 0 && fb->preview.img_height > 0) {
                    uint32_t max_w = (content_w > FB_PREVIEW_THUMB_MAX) ? FB_PREVIEW_THUMB_MAX : (uint32_t)content_w;
                    uint32_t max_h = (content_h - FB_CHAR_H - 4 > FB_PREVIEW_THUMB_MAX) ? FB_PREVIEW_THUMB_MAX : (uint32_t)(content_h - FB_CHAR_H - 4);

                    uint32_t scaled_w, scaled_h;
                    if (fb->preview.img_width <= max_w && fb->preview.img_height <= max_h) {
                        scaled_w = fb->preview.img_width;
                        scaled_h = fb->preview.img_height;
                    } else {
                        uint32_t sx = (max_w * 1000) / fb->preview.img_width;
                        uint32_t sy = (max_h * 1000) / fb->preview.img_height;
                        uint32_t sc = (sx < sy) ? sx : sy;
                        scaled_w = (fb->preview.img_width * sc) / 1000;
                        scaled_h = (fb->preview.img_height * sc) / 1000;
                        if (scaled_w == 0) scaled_w = 1;
                        if (scaled_h == 0) scaled_h = 1;
                    }

                    int32_t img_x = preview_x + FB_PREVIEW_PADDING + (content_w - (int32_t)scaled_w) / 2;
                    int32_t img_y = content_y + (content_h - (int32_t)scaled_h - FB_CHAR_H - 4) / 2;

                    fb_preview_draw_scaled(fb->preview.pixels, fb->preview.img_width, fb->preview.img_height,
                                           img_x, img_y, scaled_w, scaled_h);

                    // Draw dimensions
                    char dim[32];
                    fb_itoa(fb->preview.img_width, dim);
                    strcat(dim, "x");
                    char hs[16];
                    fb_itoa(fb->preview.img_height, hs);
                    strcat(dim, hs);
                    strcat(dim, " px");
                    int dlen = strlen(dim);
                    fb_draw_string_nobg(preview_x + (FB_PREVIEW_WIDTH - dlen * FB_CHAR_W) / 2,
                                        preview_y + preview_h - FB_PREVIEW_PADDING - FB_CHAR_H,
                                        dim, FB_STATUSBAR_TEXT & 0xFFFFFF);
                }
                break;

            case PREVIEW_TEXT:
                for (int i = 0; i < fb->preview.text_line_count && content_y < preview_y + preview_h - FB_CHAR_H; i++) {
                    char line[26];
                    int llen = strlen(fb->preview.text_lines[i]);
                    if (llen > 24) {
                        strncpy(line, fb->preview.text_lines[i], 21);
                        line[21] = '.';
                        line[22] = '.';
                        line[23] = '.';
                        line[24] = '\0';
                    } else {
                        strcpy(line, fb->preview.text_lines[i]);
                    }
                    fb_draw_string_nobg(preview_x + FB_PREVIEW_PADDING, content_y, line, FB_TEXT_COLOR & 0xFFFFFF);
                    content_y += FB_CHAR_H;
                }
                if (fb->preview.text_line_count >= FB_PREVIEW_TEXT_LINES) {
                    fb_draw_string_nobg(preview_x + FB_PREVIEW_PADDING, content_y, "...", FB_STATUSBAR_TEXT & 0xFFFFFF);
                }
                break;

            case PREVIEW_DIRECTORY:
                fb_draw_string_nobg(preview_x + FB_PREVIEW_PADDING, content_y, "(Folder)", FB_STATUSBAR_TEXT & 0xFFFFFF);
                break;

            default:
                fb_draw_string_nobg(preview_x + FB_PREVIEW_PADDING, content_y, "(No preview)", FB_STATUSBAR_TEXT & 0xFFFFFF);
                break;
        }
    } else {
        // No selection
        fb_draw_string_nobg(preview_x + FB_PREVIEW_PADDING, text_y, "No file selected", FB_STATUSBAR_TEXT & 0xFFFFFF);
    }
}

// Check if a file extension indicates an image file
bool fb_is_image_file(const char *filename) {
    if (!filename) return false;

    // Find extension
    const char *ext = filename;
    const char *last_dot = NULL;
    while (*ext) {
        if (*ext == '.') last_dot = ext;
        ext++;
    }

    if (!last_dot) return false;
    last_dot++;  // Skip the dot

    // Check common image extensions (case insensitive)
    if ((last_dot[0] == 'b' || last_dot[0] == 'B') &&
        (last_dot[1] == 'm' || last_dot[1] == 'M') &&
        (last_dot[2] == 'p' || last_dot[2] == 'P') &&
        last_dot[3] == '\0') return true;

    if ((last_dot[0] == 'p' || last_dot[0] == 'P') &&
        (last_dot[1] == 'n' || last_dot[1] == 'N') &&
        (last_dot[2] == 'g' || last_dot[2] == 'G') &&
        last_dot[3] == '\0') return true;

    if ((last_dot[0] == 'j' || last_dot[0] == 'J') &&
        (last_dot[1] == 'p' || last_dot[1] == 'P') &&
        (last_dot[2] == 'g' || last_dot[2] == 'G') &&
        last_dot[3] == '\0') return true;

    if ((last_dot[0] == 'j' || last_dot[0] == 'J') &&
        (last_dot[1] == 'p' || last_dot[1] == 'P') &&
        (last_dot[2] == 'e' || last_dot[2] == 'E') &&
        (last_dot[3] == 'g' || last_dot[3] == 'G') &&
        last_dot[4] == '\0') return true;

    // WebP
    if ((last_dot[0] == 'w' || last_dot[0] == 'W') &&
        (last_dot[1] == 'e' || last_dot[1] == 'E') &&
        (last_dot[2] == 'b' || last_dot[2] == 'B') &&
        (last_dot[3] == 'p' || last_dot[3] == 'P') &&
        last_dot[4] == '\0') return true;

    return false;
}

// Check if a file extension indicates a text file
bool fb_is_text_file(const char *filename) {
    if (!filename) return false;

    // Find extension
    const char *ext = filename;
    const char *last_dot = NULL;
    while (*ext) {
        if (*ext == '.') last_dot = ext;
        ext++;
    }

    if (!last_dot) return false;
    last_dot++;  // Skip the dot

    // Check common text extensions (case insensitive)
    if ((last_dot[0] == 't' || last_dot[0] == 'T') &&
        (last_dot[1] == 'x' || last_dot[1] == 'X') &&
        (last_dot[2] == 't' || last_dot[2] == 'T') &&
        last_dot[3] == '\0') return true;

    if ((last_dot[0] == 'c' || last_dot[0] == 'C') &&
        last_dot[1] == '\0') return true;

    if ((last_dot[0] == 'h' || last_dot[0] == 'H') &&
        last_dot[1] == '\0') return true;

    if ((last_dot[0] == 'm' || last_dot[0] == 'M') &&
        (last_dot[1] == 'd' || last_dot[1] == 'D') &&
        last_dot[2] == '\0') return true;

    if ((last_dot[0] == 'c' || last_dot[0] == 'C') &&
        (last_dot[1] == 'f' || last_dot[1] == 'F') &&
        (last_dot[2] == 'g' || last_dot[2] == 'G') &&
        last_dot[3] == '\0') return true;

    if ((last_dot[0] == 'i' || last_dot[0] == 'I') &&
        (last_dot[1] == 'n' || last_dot[1] == 'N') &&
        (last_dot[2] == 'i' || last_dot[2] == 'I') &&
        last_dot[3] == '\0') return true;

    return false;
}

// Determine file type from filename extension
uint8_t fb_get_file_type(const char *filename) {
    if (!filename) return FB_TYPE_UNKNOWN;

    if (fb_is_image_file(filename)) return FB_TYPE_IMAGE;
    if (fb_is_text_file(filename)) return FB_TYPE_TEXT;

    // Find extension
    const char *ext = filename;
    const char *last_dot = NULL;
    while (*ext) {
        if (*ext == '.') last_dot = ext;
        ext++;
    }

    if (!last_dot) return FB_TYPE_UNKNOWN;
    last_dot++;

    // Check for executable
    if ((last_dot[0] == 'e' || last_dot[0] == 'E') &&
        (last_dot[1] == 'x' || last_dot[1] == 'X') &&
        (last_dot[2] == 'e' || last_dot[2] == 'E') &&
        last_dot[3] == '\0') return FB_TYPE_EXECUTABLE;

    if ((last_dot[0] == 'c' || last_dot[0] == 'C') &&
        (last_dot[1] == 'o' || last_dot[1] == 'O') &&
        (last_dot[2] == 'm' || last_dot[2] == 'M') &&
        last_dot[3] == '\0') return FB_TYPE_EXECUTABLE;

    // Check for archive
    if ((last_dot[0] == 'z' || last_dot[0] == 'Z') &&
        (last_dot[1] == 'i' || last_dot[1] == 'I') &&
        (last_dot[2] == 'p' || last_dot[2] == 'P') &&
        last_dot[3] == '\0') return FB_TYPE_ARCHIVE;

    if ((last_dot[0] == 't' || last_dot[0] == 'T') &&
        (last_dot[1] == 'a' || last_dot[1] == 'A') &&
        (last_dot[2] == 'r' || last_dot[2] == 'R') &&
        last_dot[3] == '\0') return FB_TYPE_ARCHIVE;

    if ((last_dot[0] == 'g' || last_dot[0] == 'G') &&
        (last_dot[1] == 'z' || last_dot[1] == 'Z') &&
        last_dot[2] == '\0') return FB_TYPE_ARCHIVE;

    if ((last_dot[0] == '7' ) &&
        (last_dot[1] == 'z' || last_dot[1] == 'Z') &&
        last_dot[2] == '\0') return FB_TYPE_ARCHIVE;

    if ((last_dot[0] == 'r' || last_dot[0] == 'R') &&
        (last_dot[1] == 'a' || last_dot[1] == 'A') &&
        (last_dot[2] == 'r' || last_dot[2] == 'R') &&
        last_dot[3] == '\0') return FB_TYPE_ARCHIVE;

    // Check for audio files
    if ((last_dot[0] == 'm' || last_dot[0] == 'M') &&
        (last_dot[1] == 'p' || last_dot[1] == 'P') &&
        last_dot[2] == '3' &&
        last_dot[3] == '\0') return FB_TYPE_AUDIO;

    if ((last_dot[0] == 'w' || last_dot[0] == 'W') &&
        (last_dot[1] == 'a' || last_dot[1] == 'A') &&
        (last_dot[2] == 'v' || last_dot[2] == 'V') &&
        last_dot[3] == '\0') return FB_TYPE_AUDIO;

    if ((last_dot[0] == 'o' || last_dot[0] == 'O') &&
        (last_dot[1] == 'g' || last_dot[1] == 'G') &&
        (last_dot[2] == 'g' || last_dot[2] == 'G') &&
        last_dot[3] == '\0') return FB_TYPE_AUDIO;

    if ((last_dot[0] == 'f' || last_dot[0] == 'F') &&
        (last_dot[1] == 'l' || last_dot[1] == 'L') &&
        (last_dot[2] == 'a' || last_dot[2] == 'A') &&
        (last_dot[3] == 'c' || last_dot[3] == 'C') &&
        last_dot[4] == '\0') return FB_TYPE_AUDIO;

    // Check for video files
    if ((last_dot[0] == 'm' || last_dot[0] == 'M') &&
        (last_dot[1] == 'p' || last_dot[1] == 'P') &&
        last_dot[2] == '4' &&
        last_dot[3] == '\0') return FB_TYPE_VIDEO;

    if ((last_dot[0] == 'a' || last_dot[0] == 'A') &&
        (last_dot[1] == 'v' || last_dot[1] == 'V') &&
        (last_dot[2] == 'i' || last_dot[2] == 'I') &&
        last_dot[3] == '\0') return FB_TYPE_VIDEO;

    if ((last_dot[0] == 'm' || last_dot[0] == 'M') &&
        (last_dot[1] == 'k' || last_dot[1] == 'K') &&
        (last_dot[2] == 'v' || last_dot[2] == 'V') &&
        last_dot[3] == '\0') return FB_TYPE_VIDEO;

    if ((last_dot[0] == 'w' || last_dot[0] == 'W') &&
        (last_dot[1] == 'm' || last_dot[1] == 'M') &&
        (last_dot[2] == 'v' || last_dot[2] == 'V') &&
        last_dot[3] == '\0') return FB_TYPE_VIDEO;

    if ((last_dot[0] == 'm' || last_dot[0] == 'M') &&
        (last_dot[1] == 'o' || last_dot[1] == 'O') &&
        (last_dot[2] == 'v' || last_dot[2] == 'V') &&
        last_dot[3] == '\0') return FB_TYPE_VIDEO;

    // Check for document files
    if ((last_dot[0] == 'p' || last_dot[0] == 'P') &&
        (last_dot[1] == 'd' || last_dot[1] == 'D') &&
        (last_dot[2] == 'f' || last_dot[2] == 'F') &&
        last_dot[3] == '\0') return FB_TYPE_DOCUMENT;

    if ((last_dot[0] == 'd' || last_dot[0] == 'D') &&
        (last_dot[1] == 'o' || last_dot[1] == 'O') &&
        (last_dot[2] == 'c' || last_dot[2] == 'C') &&
        last_dot[3] == '\0') return FB_TYPE_DOCUMENT;

    if ((last_dot[0] == 'd' || last_dot[0] == 'D') &&
        (last_dot[1] == 'o' || last_dot[1] == 'O') &&
        (last_dot[2] == 'c' || last_dot[2] == 'C') &&
        (last_dot[3] == 'x' || last_dot[3] == 'X') &&
        last_dot[4] == '\0') return FB_TYPE_DOCUMENT;

    if ((last_dot[0] == 'x' || last_dot[0] == 'X') &&
        (last_dot[1] == 'l' || last_dot[1] == 'L') &&
        (last_dot[2] == 's' || last_dot[2] == 'S') &&
        last_dot[3] == '\0') return FB_TYPE_DOCUMENT;

    if ((last_dot[0] == 'x' || last_dot[0] == 'X') &&
        (last_dot[1] == 'l' || last_dot[1] == 'L') &&
        (last_dot[2] == 's' || last_dot[2] == 'S') &&
        (last_dot[3] == 'x' || last_dot[3] == 'X') &&
        last_dot[4] == '\0') return FB_TYPE_DOCUMENT;

    if ((last_dot[0] == 'p' || last_dot[0] == 'P') &&
        (last_dot[1] == 'p' || last_dot[1] == 'P') &&
        (last_dot[2] == 't' || last_dot[2] == 'T') &&
        last_dot[3] == '\0') return FB_TYPE_DOCUMENT;

    if ((last_dot[0] == 'p' || last_dot[0] == 'P') &&
        (last_dot[1] == 'p' || last_dot[1] == 'P') &&
        (last_dot[2] == 't' || last_dot[2] == 'T') &&
        (last_dot[3] == 'x' || last_dot[3] == 'X') &&
        last_dot[4] == '\0') return FB_TYPE_DOCUMENT;

    if ((last_dot[0] == 'o' || last_dot[0] == 'O') &&
        (last_dot[1] == 'd' || last_dot[1] == 'D') &&
        (last_dot[2] == 't' || last_dot[2] == 'T') &&
        last_dot[3] == '\0') return FB_TYPE_DOCUMENT;

    return FB_TYPE_UNKNOWN;
}

// Draw a file type icon at the specified position (16x16)
void fb_draw_file_icon(int32_t x, int32_t y, uint8_t file_type, bool is_selected) {
    uint32_t primary_color;
    uint32_t secondary_color = is_selected ? 0xFFFFFF : 0x000000;

    switch (file_type) {
        case FB_TYPE_FOLDER:
        case FB_TYPE_FOLDER_OPEN:
            primary_color = 0xE6B422;  // Yellow/Gold
            // Draw folder shape
            fb_fill_rect(x + 1, y + 4, 14, 10, primary_color);
            fb_fill_rect(x + 1, y + 3, 6, 2, primary_color);
            break;

        case FB_TYPE_TEXT:
            primary_color = 0x4080FF;  // Blue
            // Draw document shape
            fb_fill_rect(x + 3, y + 1, 10, 14, 0xFFFFFF);
            fb_draw_rect(x + 3, y + 1, 10, 14, primary_color);
            // Text lines
            for (int i = 0; i < 4; i++) {
                fb_fill_rect(x + 5, y + 4 + i * 3, 6, 1, 0x808080);
            }
            break;

        case FB_TYPE_IMAGE:
            primary_color = 0x40C040;  // Green
            fb_fill_rect(x + 2, y + 2, 12, 12, 0xFFFFFF);
            fb_draw_rect(x + 2, y + 2, 12, 12, primary_color);
            // Simple mountain/sun icon
            fb_fill_rect(x + 10, y + 4, 2, 2, 0xFFFF00);  // Sun
            for (int i = 0; i < 5; i++) {
                fb_fill_rect(x + 4 + i, y + 10 - i, 1, i + 1, 0x40A040);
            }
            break;

        case FB_TYPE_EXECUTABLE:
            primary_color = 0x808080;  // Gray
            fb_fill_rect(x + 3, y + 2, 10, 12, primary_color);
            fb_fill_rect(x + 5, y + 5, 6, 6, 0x404040);
            break;

        case FB_TYPE_DOCUMENT:
            primary_color = 0x2060C0;  // Dark blue
            // Draw document with folded corner
            fb_fill_rect(x + 3, y + 1, 10, 14, 0xFFFFFF);
            fb_draw_rect(x + 3, y + 1, 10, 14, primary_color);
            // Folded corner triangle
            fb_fill_rect(x + 10, y + 1, 3, 3, primary_color);
            // Text lines
            for (int i = 0; i < 3; i++) {
                fb_fill_rect(x + 5, y + 6 + i * 3, 6, 1, 0x606060);
            }
            break;

        case FB_TYPE_AUDIO:
            primary_color = 0xFF6040;  // Orange/Red
            // Draw music note shape
            fb_fill_rect(x + 4, y + 2, 8, 10, 0xFFFFFF);
            fb_draw_rect(x + 4, y + 2, 8, 10, primary_color);
            // Music note symbol
            fb_fill_rect(x + 6, y + 4, 2, 6, primary_color);  // Note stem
            fb_fill_rect(x + 5, y + 9, 3, 2, primary_color);  // Note head
            fb_fill_rect(x + 8, y + 4, 2, 2, primary_color);  // Flag
            break;

        case FB_TYPE_ARCHIVE:
            primary_color = 0x8B4513;  // Brown
            // Draw box/archive shape
            fb_fill_rect(x + 2, y + 3, 12, 10, primary_color);
            fb_fill_rect(x + 4, y + 5, 8, 2, 0xD2691E);  // Lighter band
            fb_fill_rect(x + 6, y + 4, 4, 4, 0xFFD700);  // Clasp
            break;

        case FB_TYPE_VIDEO:
            primary_color = 0x8040C0;  // Purple
            // Draw film/video shape
            fb_fill_rect(x + 3, y + 3, 10, 10, 0x202020);  // Dark background
            fb_draw_rect(x + 3, y + 3, 10, 10, primary_color);
            // Play triangle
            fb_fill_rect(x + 6, y + 5, 1, 6, 0xFFFFFF);
            fb_fill_rect(x + 7, y + 6, 1, 4, 0xFFFFFF);
            fb_fill_rect(x + 8, y + 7, 1, 2, 0xFFFFFF);
            break;

        default:
            primary_color = 0xC0C0C0;  // Light gray
            fb_fill_rect(x + 3, y + 1, 10, 14, 0xFFFFFF);
            fb_draw_rect(x + 3, y + 1, 10, 14, primary_color);
            break;
    }
    (void)secondary_color;  // Suppress unused warning
}

// Draw a large (32x32) file type icon
void fb_draw_file_icon_large(int32_t x, int32_t y, uint8_t file_type, bool is_selected) {
    // Scale up the icon drawing
    uint32_t primary_color;

    switch (file_type) {
        case FB_TYPE_FOLDER:
        case FB_TYPE_FOLDER_OPEN:
            primary_color = 0xE6B422;
            fb_fill_rect(x + 2, y + 8, 28, 20, primary_color);
            fb_fill_rect(x + 2, y + 6, 12, 4, primary_color);
            break;

        case FB_TYPE_TEXT:
            primary_color = 0x4080FF;
            fb_fill_rect(x + 6, y + 2, 20, 28, 0xFFFFFF);
            fb_draw_rect(x + 6, y + 2, 20, 28, primary_color);
            for (int i = 0; i < 6; i++) {
                fb_fill_rect(x + 10, y + 8 + i * 4, 12, 2, 0x808080);
            }
            break;

        case FB_TYPE_IMAGE:
            primary_color = 0x40C040;  // Green
            fb_fill_rect(x + 4, y + 4, 24, 24, 0xFFFFFF);
            fb_draw_rect(x + 4, y + 4, 24, 24, primary_color);
            // Sun
            fb_fill_rect(x + 20, y + 8, 4, 4, 0xFFFF00);
            // Mountain
            for (int i = 0; i < 10; i++) {
                fb_fill_rect(x + 8 + i, y + 20 - i, 2, i * 2 + 2, 0x40A040);
            }
            break;

        case FB_TYPE_EXECUTABLE:
            primary_color = 0x808080;  // Gray
            fb_fill_rect(x + 6, y + 4, 20, 24, primary_color);
            fb_fill_rect(x + 10, y + 10, 12, 12, 0x404040);
            break;

        case FB_TYPE_DOCUMENT:
            primary_color = 0x2060C0;  // Dark blue
            fb_fill_rect(x + 6, y + 2, 20, 28, 0xFFFFFF);
            fb_draw_rect(x + 6, y + 2, 20, 28, primary_color);
            // Folded corner
            fb_fill_rect(x + 20, y + 2, 6, 6, primary_color);
            // Text lines
            for (int i = 0; i < 5; i++) {
                fb_fill_rect(x + 10, y + 12 + i * 4, 12, 2, 0x606060);
            }
            break;

        case FB_TYPE_AUDIO:
            primary_color = 0xFF6040;  // Orange/Red
            fb_fill_rect(x + 8, y + 4, 16, 20, 0xFFFFFF);
            fb_draw_rect(x + 8, y + 4, 16, 20, primary_color);
            // Music note
            fb_fill_rect(x + 12, y + 8, 4, 12, primary_color);
            fb_fill_rect(x + 10, y + 18, 6, 4, primary_color);
            fb_fill_rect(x + 16, y + 8, 4, 4, primary_color);
            break;

        case FB_TYPE_ARCHIVE:
            primary_color = 0x8B4513;  // Brown
            fb_fill_rect(x + 4, y + 6, 24, 20, primary_color);
            fb_fill_rect(x + 8, y + 10, 16, 4, 0xD2691E);
            fb_fill_rect(x + 12, y + 8, 8, 8, 0xFFD700);
            break;

        case FB_TYPE_VIDEO:
            primary_color = 0x8040C0;  // Purple
            fb_fill_rect(x + 6, y + 6, 20, 20, 0x202020);
            fb_draw_rect(x + 6, y + 6, 20, 20, primary_color);
            // Play triangle (larger)
            fb_fill_rect(x + 12, y + 10, 2, 12, 0xFFFFFF);
            fb_fill_rect(x + 14, y + 12, 2, 8, 0xFFFFFF);
            fb_fill_rect(x + 16, y + 14, 2, 4, 0xFFFFFF);
            break;

        default:
            fb_fill_rect(x + 6, y + 2, 20, 28, 0xFFFFFF);
            fb_draw_rect(x + 6, y + 2, 20, 28, 0xC0C0C0);
            break;
    }
    (void)is_selected;  // Suppress unused warning
}

// ============================================
// View Mode Functions
// ============================================

// Set the view mode for the file browser
void filebrowser_set_view_mode(filebrowser_t *fb, fb_view_mode_t mode) {
    if (!fb) return;
    fb->view_mode = mode;
    kprintf("[FileBrowser] View mode set to %d\n", mode);
}

// Get the current view mode
fb_view_mode_t filebrowser_get_view_mode(filebrowser_t *fb) {
    if (!fb) return FB_VIEW_LIST;
    return fb->view_mode;
}

// Cycle to the next view mode
void filebrowser_cycle_view_mode(filebrowser_t *fb) {
    if (!fb) return;
    fb->view_mode = (fb->view_mode + 1) % 4;
    kprintf("[FileBrowser] View mode cycled to %d\n", fb->view_mode);
}

// Set the sort column for details view
void filebrowser_set_sort_column(filebrowser_t *fb, fb_sort_column_t column) {
    if (!fb) return;
    if (fb->sort_column == column) {
        // Toggle direction if same column
        fb->sort_ascending = !fb->sort_ascending;
    } else {
        fb->sort_column = column;
        fb->sort_ascending = true;
    }
    // Re-sort entries would go here
}

// Toggle sort direction
void filebrowser_toggle_sort_direction(filebrowser_t *fb) {
    if (!fb) return;
    fb->sort_ascending = !fb->sort_ascending;
}

// Get file type string for display
const char *fb_get_type_string(uint8_t file_type) {
    switch (file_type) {
        case FB_TYPE_FOLDER: return "Folder";
        case FB_TYPE_FOLDER_OPEN: return "Folder";
        case FB_TYPE_TEXT: return "Text";
        case FB_TYPE_IMAGE: return "Image";
        case FB_TYPE_EXECUTABLE: return "Program";
        case FB_TYPE_DOCUMENT: return "Document";
        case FB_TYPE_AUDIO: return "Audio";
        case FB_TYPE_ARCHIVE: return "Archive";
        default: return "File";
    }
}

// Extract file extension from filename
void fb_extract_extension(const char *filename, char *ext_buf) {
    if (!filename || !ext_buf) return;
    ext_buf[0] = '\0';

    const char *last_dot = NULL;
    const char *p = filename;
    while (*p) {
        if (*p == '.') last_dot = p;
        p++;
    }

    if (last_dot && last_dot[1]) {
        strncpy(ext_buf, last_dot + 1, FB_EXT_MAX - 1);
        ext_buf[FB_EXT_MAX - 1] = '\0';
        // Convert to uppercase
        for (int i = 0; ext_buf[i]; i++) {
            if (ext_buf[i] >= 'a' && ext_buf[i] <= 'z') {
                ext_buf[i] -= 32;
            }
        }
    }
}

// Format FAT date/time for display
void fb_format_datetime(uint16_t date, uint16_t time, char *buf) {
    if (!buf) return;

    // FAT date format: bits 15-9 = year (0-127, relative to 1980), bits 8-5 = month (1-12), bits 4-0 = day (1-31)
    // FAT time format: bits 15-11 = hours (0-23), bits 10-5 = minutes (0-59), bits 4-0 = seconds/2 (0-29)

    int year = ((date >> 9) & 0x7F) + 1980;
    int month = (date >> 5) & 0x0F;
    int day = date & 0x1F;

    int hour = (time >> 11) & 0x1F;
    int minute = (time >> 5) & 0x3F;

    // Format as "YYYY-MM-DD HH:MM"
    char tmp[8];
    fb_itoa(year, buf);
    strcat(buf, "-");
    if (month < 10) strcat(buf, "0");
    fb_itoa(month, tmp);
    strcat(buf, tmp);
    strcat(buf, "-");
    if (day < 10) strcat(buf, "0");
    fb_itoa(day, tmp);
    strcat(buf, tmp);
    strcat(buf, " ");
    if (hour < 10) strcat(buf, "0");
    fb_itoa(hour, tmp);
    strcat(buf, tmp);
    strcat(buf, ":");
    if (minute < 10) strcat(buf, "0");
    fb_itoa(minute, tmp);
    strcat(buf, tmp);
}

// Draw list view (simple name-only list) - uses file type icons
void fb_draw_list_view(filebrowser_t *fb, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!fb) return;

    fb->visible_entries = h / FB_ENTRY_HEIGHT;

    for (int i = 0; i < fb->visible_entries && (i + fb->scroll_offset) < fb->entry_count; i++) {
        int entry_index = i + fb->scroll_offset;
        int32_t entry_y = y + (i * FB_ENTRY_HEIGHT);
        fb_entry_t *entry = &fb->entries[entry_index];
        bool is_selected = (entry_index == fb->selected_index);

        uint32_t bg_color = FB_BG_COLOR;
        uint32_t fg_color = entry->is_dir ? FB_DIR_COLOR : FB_TEXT_COLOR;

        if (is_selected) {
            bg_color = FB_SELECTED_BG;
            fg_color = FB_SELECTED_FG;
        }

        // Fill entry background
        fb_fill_rect(x, entry_y, w, FB_ENTRY_HEIGHT, bg_color & 0xFFFFFF);

        // Draw file type icon (16x16, vertically centered in 20px row)
        uint8_t file_type = entry->is_dir ? FB_TYPE_FOLDER : fb_get_file_type(entry->name);
        fb_draw_file_icon(x + 2, entry_y + 2, file_type, is_selected);

        // Draw file name (icon is 16px + 4px padding = 20px offset)
        fb_draw_string(x + 22, entry_y + 2, entry->name, fg_color, bg_color);

        // Draw file size on the right (only for files)
        if (!entry->is_dir) {
            char size_buf[32];
            fb_format_size(entry->size, size_buf);
            int size_len = strlen(size_buf);
            fb_draw_string(x + w - (size_len * FB_CHAR_W) - 8, entry_y + 2, size_buf, fg_color, bg_color);
        }
    }

    if (fb->entry_count == 0) {
        fb_draw_string(x + 4, y + 4, "(Empty directory)", 0x808080, FB_BG_COLOR);
    }
}

// Draw details view (multi-column with sortable headers) - uses file type icons
void fb_draw_details_view(filebrowser_t *fb, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!fb) return;

    // Draw column headers
    fb_draw_column_headers(fb, x, y, w);

    int32_t content_y = y + FB_HEADER_HEIGHT;
    int32_t content_h = h - FB_HEADER_HEIGHT;
    fb->visible_entries = content_h / FB_ENTRY_HEIGHT;

    for (int i = 0; i < fb->visible_entries && (i + fb->scroll_offset) < fb->entry_count; i++) {
        int entry_index = i + fb->scroll_offset;
        int32_t entry_y = content_y + (i * FB_ENTRY_HEIGHT);
        fb_entry_t *entry = &fb->entries[entry_index];
        bool is_selected = (entry_index == fb->selected_index);

        uint32_t bg_color = FB_BG_COLOR;
        uint32_t fg_color = entry->is_dir ? FB_DIR_COLOR : FB_TEXT_COLOR;

        if (is_selected) {
            bg_color = FB_SELECTED_BG;
            fg_color = FB_SELECTED_FG;
        }

        fb_fill_rect(x, entry_y, w, FB_ENTRY_HEIGHT, bg_color & 0xFFFFFF);

        // Name column with icon
        uint8_t file_type = entry->is_dir ? FB_TYPE_FOLDER : fb_get_file_type(entry->name);
        fb_draw_file_icon(x + 2, entry_y + 2, file_type, is_selected);
        fb_draw_string(x + 22, entry_y + 2, entry->name, fg_color, bg_color);

        // Size column
        if (!entry->is_dir) {
            char size_buf[32];
            fb_format_size(entry->size, size_buf);
            fb_draw_string(x + FB_COL_NAME_WIDTH + 4, entry_y + 2, size_buf, fg_color, bg_color);
        }

        // Type column
        const char *type_str = entry->is_dir ? "Folder" : fb_get_type_string(entry->file_type);
        fb_draw_string(x + FB_COL_NAME_WIDTH + FB_COL_SIZE_WIDTH + 4, entry_y + 2, type_str, fg_color, bg_color);
    }
}

// Draw icon view (grid of icons with names)
void fb_draw_icon_view(filebrowser_t *fb, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!fb) return;

    fb->items_per_row = w / FB_ICON_CELL_W;
    if (fb->items_per_row < 1) fb->items_per_row = 1;

    int visible_rows = h / FB_ICON_CELL_H;
    fb->visible_entries = fb->items_per_row * visible_rows;

    fb_fill_rect(x, y, w, h, FB_BG_COLOR & 0xFFFFFF);

    for (int i = 0; i < fb->visible_entries && (i + fb->scroll_offset) < fb->entry_count; i++) {
        int entry_index = i + fb->scroll_offset;
        fb_entry_t *entry = &fb->entries[entry_index];

        int col = i % fb->items_per_row;
        int row = i / fb->items_per_row;

        int32_t cell_x = x + col * FB_ICON_CELL_W;
        int32_t cell_y = y + row * FB_ICON_CELL_H;

        // Highlight selected
        if (entry_index == fb->selected_index) {
            fb_fill_rect(cell_x, cell_y, FB_ICON_CELL_W, FB_ICON_CELL_H, FB_SELECTED_BG & 0xFFFFFF);
        }

        // Draw icon
        uint8_t icon_type = entry->is_dir ? FB_TYPE_FOLDER : entry->file_type;
        fb_draw_file_icon_large(cell_x + (FB_ICON_CELL_W - FB_ICON_LARGE) / 2, cell_y + 4, icon_type, entry_index == fb->selected_index);

        // Draw name (truncated if necessary)
        uint32_t text_color = (entry_index == fb->selected_index) ? FB_SELECTED_FG : FB_TEXT_COLOR;
        int name_len = strlen(entry->name);
        int max_chars = FB_ICON_CELL_W / FB_CHAR_W;
        if (name_len > max_chars) {
            char truncated[16];
            strncpy(truncated, entry->name, max_chars - 3);
            truncated[max_chars - 3] = '\0';
            strcat(truncated, "...");
            fb_draw_string_nobg(cell_x + 2, cell_y + FB_ICON_LARGE + 8, truncated, text_color & 0xFFFFFF);
        } else {
            fb_draw_string_nobg(cell_x + (FB_ICON_CELL_W - name_len * FB_CHAR_W) / 2, cell_y + FB_ICON_LARGE + 8, entry->name, text_color & 0xFFFFFF);
        }
    }
}

// Draw thumbnail view with actual thumbnail generation
void fb_draw_thumbnail_view(filebrowser_t *fb, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!fb) return;

    // Initialize thumbnail cache if needed
    thumb_cache_init(0);

    fb->items_per_row = w / FB_THUMB_CELL_W;
    if (fb->items_per_row < 1) fb->items_per_row = 1;

    int visible_rows = (h + FB_THUMB_CELL_H - 1) / FB_THUMB_CELL_H;
    fb->visible_entries = fb->items_per_row * visible_rows;

    // Clear background
    fb_fill_rect(x, y, w, h, FB_BG_COLOR & 0xFFFFFF);

    // Draw grid of thumbnails
    for (int i = 0; i < fb->visible_entries && (i + fb->scroll_offset) < fb->entry_count; i++) {
        int entry_index = i + fb->scroll_offset;
        fb_entry_t *entry = &fb->entries[entry_index];

        int col = i % fb->items_per_row;
        int row = i / fb->items_per_row;

        int32_t cell_x = x + col * FB_THUMB_CELL_W;
        int32_t cell_y = y + row * FB_THUMB_CELL_H;

        bool is_selected = (entry_index == fb->selected_index);

        // Draw selection highlight
        if (is_selected) {
            fb_fill_rect(cell_x + 2, cell_y + 2, FB_THUMB_CELL_W - 4, FB_THUMB_CELL_H - 4,
                        FB_SELECTED_BG & 0xFFFFFF);
        }

        // Calculate thumbnail area
        int32_t thumb_x = cell_x + (FB_THUMB_CELL_W - FB_THUMB_SIZE) / 2;
        int32_t thumb_y = cell_y + 4;

        // Draw thumbnail background
        fb_fill_rect(thumb_x, thumb_y, FB_THUMB_SIZE, FB_THUMB_SIZE, FB_ICON_BG & 0xFFFFFF);
        fb_draw_rect(thumb_x, thumb_y, FB_THUMB_SIZE, FB_THUMB_SIZE, 0xC0C0C0);

        // Try to get thumbnail from cache
        bool drew_thumbnail = false;
        if (!entry->is_dir) {
            // Build full path
            char full_path[FB_MAX_PATH];
            strcpy(full_path, fb->current_path);
            int plen = strlen(full_path);
            if (plen > 0 && full_path[plen - 1] != '/') {
                strcat(full_path, "/");
            }
            strcat(full_path, entry->name);

            thumbnail_t *thumb = thumb_cache_get(full_path, THUMB_MEDIUM);

            if (thumb && thumb->pixels && thumb->width > 0 && thumb->height > 0) {
                // Draw actual thumbnail
                int32_t draw_x = thumb_x + (FB_THUMB_SIZE - (int32_t)thumb->width) / 2;
                int32_t draw_y = thumb_y + (FB_THUMB_SIZE - (int32_t)thumb->height) / 2;

                for (uint32_t py = 0; py < thumb->height; py++) {
                    for (uint32_t px = 0; px < thumb->width; px++) {
                        if (draw_x + (int32_t)px >= thumb_x &&
                            draw_x + (int32_t)px < thumb_x + FB_THUMB_SIZE &&
                            draw_y + (int32_t)py >= thumb_y &&
                            draw_y + (int32_t)py < thumb_y + FB_THUMB_SIZE) {
                            fb_put_pixel(draw_x + px, draw_y + py,
                                        thumb->pixels[py * thumb->width + px]);
                        }
                    }
                }
                drew_thumbnail = true;
            } else if (thumb && thumb->type == THUMB_TYPE_TEXT) {
                // Draw text preview thumbnail
                fb_fill_rect(thumb_x + 1, thumb_y + 1, FB_THUMB_SIZE - 2, FB_THUMB_SIZE - 2, 0xFFFFFF);
                for (int line = 0; line < thumb->text_line_count && line < 6; line++) {
                    int line_y = thumb_y + 8 + line * 12;
                    int line_w = strlen(thumb->text_lines[line]);
                    if (line_w > 10) line_w = 10;
                    fb_fill_rect(thumb_x + 8, line_y, line_w * 6, 2, 0x606060);
                }
                drew_thumbnail = true;
            }
        }

        if (!drew_thumbnail) {
            // Draw file type icon
            uint8_t icon_type = entry->is_dir ? FB_TYPE_FOLDER : fb_get_file_type(entry->name);
            fb_draw_file_icon_large(thumb_x + (FB_THUMB_SIZE - FB_ICON_LARGE) / 2,
                                   thumb_y + (FB_THUMB_SIZE - FB_ICON_LARGE) / 2,
                                   icon_type, is_selected);
        }

        // Draw filename below thumbnail
        uint32_t text_color = is_selected ? (FB_SELECTED_FG & 0xFFFFFF) : (FB_TEXT_COLOR & 0xFFFFFF);

        // Truncate filename if too long
        char display_name[16];
        int max_chars = (FB_THUMB_CELL_W - 8) / FB_CHAR_W;
        if (max_chars > 14) max_chars = 14;

        int name_len = strlen(entry->name);
        if (name_len > max_chars) {
            strncpy(display_name, entry->name, max_chars - 3);
            display_name[max_chars - 3] = '\0';
            strcat(display_name, "...");
        } else {
            strcpy(display_name, entry->name);
        }

        // Center the name
        int text_x = cell_x + (FB_THUMB_CELL_W - strlen(display_name) * FB_CHAR_W) / 2;
        int text_y = cell_y + FB_THUMB_SIZE + 8;

        // Draw text background for better readability
        if (is_selected) {
            fb_fill_rect(text_x - 2, text_y - 1,
                        strlen(display_name) * FB_CHAR_W + 4, FB_CHAR_H + 2,
                        FB_SELECTED_BG & 0xFFFFFF);
        }

        fb_draw_string_nobg(text_x, text_y, display_name, text_color);
    }

    // Draw empty message if no entries
    if (fb->entry_count == 0) {
        fb_draw_string_nobg(x + 4, y + 4, "(Empty directory)", 0x808080);
    }
}


// Draw column headers for details view
void fb_draw_column_headers(filebrowser_t *fb, int32_t x, int32_t y, int32_t w) {
    // Header background
    fb_fill_rect(x, y, w, FB_HEADER_HEIGHT, FB_HEADER_BG & 0xFFFFFF);
    fb_fill_rect(x, y + FB_HEADER_HEIGHT - 1, w, 1, FB_HEADER_BORDER & 0xFFFFFF);

    // Column labels
    fb_draw_string_nobg(x + 4, y + 2, "Name", FB_TEXT_COLOR & 0xFFFFFF);
    fb_draw_string_nobg(x + FB_COL_NAME_WIDTH + 4, y + 2, "Size", FB_TEXT_COLOR & 0xFFFFFF);
    fb_draw_string_nobg(x + FB_COL_NAME_WIDTH + FB_COL_SIZE_WIDTH + 4, y + 2, "Type", FB_TEXT_COLOR & 0xFFFFFF);

    // Column separators
    fb_fill_rect(x + FB_COL_NAME_WIDTH, y, 1, FB_HEADER_HEIGHT, FB_HEADER_BORDER & 0xFFFFFF);
    fb_fill_rect(x + FB_COL_NAME_WIDTH + FB_COL_SIZE_WIDTH, y, 1, FB_HEADER_HEIGHT, FB_HEADER_BORDER & 0xFFFFFF);
    (void)fb;  // Suppress unused warning
}

// Get entry index at a given screen point (view-mode aware)
int fb_get_entry_at_point(filebrowser_t *fb, int32_t x, int32_t y) {
    if (!fb) return -1;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(fb->window, &wx, &wy, &ww, &wh);

    int32_t content_y = wy + FB_TOOLBAR_HEIGHT + FB_BREADCRUMB_HEIGHT;
    int32_t content_h = wh - FB_TOOLBAR_HEIGHT - FB_BREADCRUMB_HEIGHT - FB_STATUSBAR_HEIGHT;
    int32_t list_width = ww;
    if (fb->show_preview) {
        list_width = ww - FB_PREVIEW_WIDTH - 2;
    }

    if (y < content_y || y >= content_y + content_h) return -1;
    if (x < wx || x >= wx + list_width) return -1;

    switch (fb->view_mode) {
        case FB_VIEW_LIST:
        case FB_VIEW_DETAILS: {
            int click_row = (y - content_y) / FB_ENTRY_HEIGHT;
            if (fb->view_mode == FB_VIEW_DETAILS) {
                click_row = (y - content_y - FB_HEADER_HEIGHT) / FB_ENTRY_HEIGHT;
                if (y < content_y + FB_HEADER_HEIGHT) return -1;
            }
            int entry_index = click_row + fb->scroll_offset;
            if (entry_index >= 0 && entry_index < fb->entry_count) {
                return entry_index;
            }
            break;
        }

        case FB_VIEW_ICONS:
        case FB_VIEW_THUMBNAILS: {
            int cell_w = (fb->view_mode == FB_VIEW_ICONS) ? FB_ICON_CELL_W : FB_THUMB_CELL_W;
            int cell_h = (fb->view_mode == FB_VIEW_ICONS) ? FB_ICON_CELL_H : FB_THUMB_CELL_H;
            int items_per_row = list_width / cell_w;
            if (items_per_row < 1) items_per_row = 1;

            int col = (x - wx) / cell_w;
            int row = (y - content_y) / cell_h;
            int entry_index = row * items_per_row + col + fb->scroll_offset;

            if (entry_index >= 0 && entry_index < fb->entry_count) {
                return entry_index;
            }
            break;
        }
    }

    return -1;
}
