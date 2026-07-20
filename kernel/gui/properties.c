#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#pragma GCC diagnostic ignored "-Wunused-variable"
// properties.c - File Properties Dialog Implementation for MayteraOS
#include "properties.h"
#include "window.h"
#include "desktop.h"
#include "filebrowser.h"
#include "../types.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../cpu/isr.h"
#include "../drivers/mouse.h"
#include "../fs/fat.h"
#include "../fs/xattr.h"

// External resources
extern fat_fs_t g_fat_fs;
extern volatile uint64_t timer_ticks;

// Static buffers for formatting
static char prop_size_buf[32];
static char prop_date_buf[32];

// Forward declarations
static void prop_draw_char(int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg);
static void prop_draw_string(int32_t x, int32_t y, const char *str, uint32_t fg, uint32_t bg);
static void prop_draw_string_nobg(int32_t x, int32_t y, const char *str, uint32_t fg);
static void prop_draw_icon(properties_dialog_t *dlg, int32_t x, int32_t y);
static void prop_init_buttons(properties_dialog_t *dlg);
static void prop_add_xattr(properties_dialog_t *dlg);
static void prop_edit_xattr(properties_dialog_t *dlg);
static void prop_delete_xattr(properties_dialog_t *dlg);

// ============================================================================
// Drawing Helpers
// ============================================================================

static void prop_draw_char(int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg) {
    fb_fill_rect(x, y, PROP_CHAR_W, PROP_CHAR_H, bg & 0xFFFFFF);
    if (c >= ' ' && c < 127) {
        const uint8_t *glyph = font_get_glyph(c);
        if (glyph) {
            for (int r = 0; r < FONT_HEIGHT && r < PROP_CHAR_H; r++) {
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

static void prop_draw_string(int32_t x, int32_t y, const char *str, uint32_t fg, uint32_t bg) {
    while (*str) {
        prop_draw_char(x, y, *str, fg, bg);
        x += PROP_CHAR_W;
        str++;
    }
}

static void prop_draw_string_nobg(int32_t x, int32_t y, const char *str, uint32_t fg) {
    while (*str) {
        if (*str >= ' ' && *str < 127) {
            const uint8_t *glyph = font_get_glyph(*str);
            if (glyph) {
                for (int r = 0; r < FONT_HEIGHT; r++) {
                    uint8_t bits = glyph[r];
                    for (int col = 0; col < FONT_WIDTH; col++) {
                        if (bits & (0x80 >> col)) {
                            fb_put_pixel(x + col, y + r, fg & 0xFFFFFF);
                        }
                    }
                }
            }
        }
        x += PROP_CHAR_W;
        str++;
    }
}

// Draw a 48x48 file icon based on type
static void prop_draw_icon(properties_dialog_t *dlg, int32_t x, int32_t y) {
    // Draw icon background
    fb_fill_rect(x, y, 48, 48, PROP_ICON_BG & 0xFFFFFF);
    fb_draw_rect(x, y, 48, 48, PROP_TAB_BORDER & 0xFFFFFF);
    
    // Draw simplified file icon
    if (dlg->is_directory) {
        // Folder icon
        fb_fill_rect(x + 8, y + 12, 32, 28, 0xFFD4A017 & 0xFFFFFF);
        fb_fill_rect(x + 8, y + 8, 16, 8, 0xFFD4A017 & 0xFFFFFF);
    } else {
        // File icon
        fb_fill_rect(x + 12, y + 6, 24, 36, 0xFFFFFFFF & 0xFFFFFF);
        fb_draw_rect(x + 12, y + 6, 24, 36, 0xFF808080 & 0xFFFFFF);
        // Document lines
        fb_fill_rect(x + 16, y + 14, 16, 2, 0xFFC0C0C0 & 0xFFFFFF);
        fb_fill_rect(x + 16, y + 20, 16, 2, 0xFFC0C0C0 & 0xFFFFFF);
        fb_fill_rect(x + 16, y + 26, 12, 2, 0xFFC0C0C0 & 0xFFFFFF);
    }
}

// ============================================================================
// Formatting Helpers
// ============================================================================

const char *properties_format_size(uint32_t size) {
    if (size < 1024) {
        int i = 0;
        if (size == 0) {
            prop_size_buf[0] = '0';
            prop_size_buf[1] = '\0';
        } else {
            char tmp[16];
            int j = 0;
            while (size > 0) {
                tmp[j++] = '0' + (size % 10);
                size /= 10;
            }
            while (j > 0) prop_size_buf[i++] = tmp[--j];
            prop_size_buf[i] = '\0';
        }
        strcat(prop_size_buf, " bytes");
    } else if (size < 1024 * 1024) {
        uint32_t kb = size / 1024;
        char tmp[16];
        int i = 0, j = 0;
        while (kb > 0) {
            tmp[j++] = '0' + (kb % 10);
            kb /= 10;
        }
        while (j > 0) prop_size_buf[i++] = tmp[--j];
        prop_size_buf[i] = '\0';
        strcat(prop_size_buf, " KB");
    } else {
        uint32_t mb = size / (1024 * 1024);
        char tmp[16];
        int i = 0, j = 0;
        while (mb > 0) {
            tmp[j++] = '0' + (mb % 10);
            mb /= 10;
        }
        while (j > 0) prop_size_buf[i++] = tmp[--j];
        prop_size_buf[i] = '\0';
        strcat(prop_size_buf, " MB");
    }
    return prop_size_buf;
}

const char *properties_format_date(uint16_t date, uint16_t time) {
    int year = ((date >> 9) & 0x7F) + 1980;
    int month = (date >> 5) & 0x0F;
    int day = date & 0x1F;
    int hour = (time >> 11) & 0x1F;
    int minute = (time >> 5) & 0x3F;
    
    // Format: YYYY-MM-DD HH:MM
    int i = 0;
    prop_date_buf[i++] = '0' + (year / 1000);
    prop_date_buf[i++] = '0' + ((year / 100) % 10);
    prop_date_buf[i++] = '0' + ((year / 10) % 10);
    prop_date_buf[i++] = '0' + (year % 10);
    prop_date_buf[i++] = '-';
    prop_date_buf[i++] = '0' + (month / 10);
    prop_date_buf[i++] = '0' + (month % 10);
    prop_date_buf[i++] = '-';
    prop_date_buf[i++] = '0' + (day / 10);
    prop_date_buf[i++] = '0' + (day % 10);
    prop_date_buf[i++] = ' ';
    prop_date_buf[i++] = '0' + (hour / 10);
    prop_date_buf[i++] = '0' + (hour % 10);
    prop_date_buf[i++] = ':';
    prop_date_buf[i++] = '0' + (minute / 10);
    prop_date_buf[i++] = '0' + (minute % 10);
    prop_date_buf[i] = '\0';
    
    return prop_date_buf;
}

const char *properties_get_type_name(uint8_t file_type, bool is_dir) {
    if (is_dir) return "Folder";
    
    switch (file_type) {
        case FB_TYPE_TEXT: return "Text Document";
        case FB_TYPE_IMAGE: return "Image File";
        case FB_TYPE_EXECUTABLE: return "Application";
        case FB_TYPE_DOCUMENT: return "Document";
        case FB_TYPE_AUDIO: return "Audio File";
        case FB_TYPE_ARCHIVE: return "Archive";
        case FB_TYPE_VIDEO: return "Video File";
        default: return "File";
    }
}

// ============================================================================
// Button Initialization
// ============================================================================

static void prop_init_buttons(properties_dialog_t *dlg) {
    int btn_y = PROP_DIALOG_HEIGHT - 40;
    int btn_x = PROP_DIALOG_WIDTH - PROP_PADDING - PROP_BTN_WIDTH;
    
    // Cancel button (rightmost)
    dlg->buttons[PROP_BTN_CANCEL].x = btn_x;
    dlg->buttons[PROP_BTN_CANCEL].y = btn_y;
    dlg->buttons[PROP_BTN_CANCEL].w = PROP_BTN_WIDTH;
    dlg->buttons[PROP_BTN_CANCEL].h = PROP_BTN_HEIGHT;
    dlg->buttons[PROP_BTN_CANCEL].label = "Cancel";
    dlg->buttons[PROP_BTN_CANCEL].enabled = true;
    dlg->buttons[PROP_BTN_CANCEL].visible = true;
    
    // OK button
    btn_x -= PROP_BTN_WIDTH + 8;
    dlg->buttons[PROP_BTN_OK].x = btn_x;
    dlg->buttons[PROP_BTN_OK].y = btn_y;
    dlg->buttons[PROP_BTN_OK].w = PROP_BTN_WIDTH;
    dlg->buttons[PROP_BTN_OK].h = PROP_BTN_HEIGHT;
    dlg->buttons[PROP_BTN_OK].label = "OK";
    dlg->buttons[PROP_BTN_OK].enabled = true;
    dlg->buttons[PROP_BTN_OK].visible = true;
    
    // Apply button
    btn_x -= PROP_BTN_WIDTH + 8;
    dlg->buttons[PROP_BTN_APPLY].x = btn_x;
    dlg->buttons[PROP_BTN_APPLY].y = btn_y;
    dlg->buttons[PROP_BTN_APPLY].w = PROP_BTN_WIDTH;
    dlg->buttons[PROP_BTN_APPLY].h = PROP_BTN_HEIGHT;
    dlg->buttons[PROP_BTN_APPLY].label = "Apply";
    dlg->buttons[PROP_BTN_APPLY].enabled = false;
    dlg->buttons[PROP_BTN_APPLY].visible = true;
    
    // Attribute buttons (shown only on attributes tab)
    int attr_btn_x = PROP_DIALOG_WIDTH - PROP_PADDING - 60;
    int attr_btn_y = PROP_TAB_HEIGHT + PROP_PADDING + 130;
    
    dlg->buttons[PROP_BTN_ADD_ATTR].x = attr_btn_x;
    dlg->buttons[PROP_BTN_ADD_ATTR].y = attr_btn_y;
    dlg->buttons[PROP_BTN_ADD_ATTR].w = 50;
    dlg->buttons[PROP_BTN_ADD_ATTR].h = 22;
    dlg->buttons[PROP_BTN_ADD_ATTR].label = "Add";
    dlg->buttons[PROP_BTN_ADD_ATTR].enabled = true;
    dlg->buttons[PROP_BTN_ADD_ATTR].visible = false;
    
    dlg->buttons[PROP_BTN_EDIT_ATTR].x = attr_btn_x;
    dlg->buttons[PROP_BTN_EDIT_ATTR].y = attr_btn_y + 26;
    dlg->buttons[PROP_BTN_EDIT_ATTR].w = 50;
    dlg->buttons[PROP_BTN_EDIT_ATTR].h = 22;
    dlg->buttons[PROP_BTN_EDIT_ATTR].label = "Edit";
    dlg->buttons[PROP_BTN_EDIT_ATTR].enabled = false;
    dlg->buttons[PROP_BTN_EDIT_ATTR].visible = false;
    
    dlg->buttons[PROP_BTN_DEL_ATTR].x = attr_btn_x;
    dlg->buttons[PROP_BTN_DEL_ATTR].y = attr_btn_y + 52;
    dlg->buttons[PROP_BTN_DEL_ATTR].w = 50;
    dlg->buttons[PROP_BTN_DEL_ATTR].h = 22;
    dlg->buttons[PROP_BTN_DEL_ATTR].label = "Delete";
    dlg->buttons[PROP_BTN_DEL_ATTR].enabled = false;
    dlg->buttons[PROP_BTN_DEL_ATTR].visible = false;
}

// ============================================================================
// Create/Destroy
// ============================================================================

properties_dialog_t *properties_create(const char *filepath) {
    if (!filepath) return NULL;
    
    properties_dialog_t *dlg = (properties_dialog_t *)kmalloc(sizeof(properties_dialog_t));
    if (!dlg) return NULL;
    
    memset(dlg, 0, sizeof(properties_dialog_t));
    
    // Copy filepath
    strncpy(dlg->filepath, filepath, sizeof(dlg->filepath) - 1);
    
    // Extract filename from path
    const char *name = filepath;
    const char *p = filepath;
    while (*p) {
        if (*p == '/') name = p + 1;
        p++;
    }
    strncpy(dlg->filename, name, sizeof(dlg->filename) - 1);
    
    // Extract location (parent directory)
    size_t path_len = name - filepath;
    if (path_len > 0 && path_len < sizeof(dlg->location)) {
        strncpy(dlg->location, filepath, path_len);
        dlg->location[path_len - 1] = '\0';  // Remove trailing /
        if (dlg->location[0] == '\0') {
            strcpy(dlg->location, "/");
        }
    } else {
        strcpy(dlg->location, "/");
    }
    
    // Initialize state
    dlg->active_tab = PROP_TAB_GENERAL;
    dlg->result = PROP_RESULT_NONE;
    dlg->running = true;
    dlg->xattr_selected = -1;
    dlg->editing_field = -1;
    
    // Initialize buttons
    prop_init_buttons(dlg);
    
    // Load file information
    properties_load_file_info(dlg);
    
    // Load extended attributes
    properties_load_xattrs(dlg);
    
    // Create window
    char title[300];
    strcpy(title, dlg->filename);
    strcat(title, " Properties");
    
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();
    int32_t wx = (screen_w - PROP_DIALOG_WIDTH) / 2;
    int32_t wy = (screen_h - PROP_DIALOG_HEIGHT) / 2;
    
    dlg->window = window_create(title, wx, wy, PROP_DIALOG_WIDTH, PROP_DIALOG_HEIGHT);
    if (!dlg->window) {
        kfree(dlg);
        return NULL;
    }
    
    kprintf("[Properties] Created dialog for: %s\n", filepath);
    return dlg;
}

void properties_destroy(properties_dialog_t *dlg) {
    if (!dlg) return;
    
    if (dlg->window) {
        window_destroy(dlg->window);
    }
    
    kfree(dlg);
}

// ============================================================================
// Load File Information
// ============================================================================

void properties_load_file_info(properties_dialog_t *dlg) {
    if (!dlg) return;
    
    fat_file_t file;
    if (fat_open(&g_fat_fs, dlg->filepath, &file) == 0) {
        dlg->file_size = fat_size(&file);
        dlg->attributes = file.attr;
        dlg->is_directory = (file.attr & FAT_ATTR_DIRECTORY) != 0;
        dlg->mod_date = 0;
        dlg->mod_time = 0;
        fat_close(&file);
    } else {
        kprintf("[Properties] Failed to open file: %s\n", dlg->filepath);
    }
    
    // Determine file type
    dlg->file_type = fb_get_file_type(dlg->filename);
}

// ============================================================================
// Extended Attributes Management
// ============================================================================

void properties_load_xattrs(properties_dialog_t *dlg) {
    if (!dlg) return;
    
    dlg->xattr_count = 0;
    memset(dlg->xattrs, 0, sizeof(dlg->xattrs));
    memset(dlg->mime_type, 0, sizeof(dlg->mime_type));
    memset(dlg->description, 0, sizeof(dlg->description));
    memset(dlg->tags, 0, sizeof(dlg->tags));
    memset(dlg->open_with, 0, sizeof(dlg->open_with));
    
    // Get list of attributes
    char list_buf[4096];
    ssize_t list_size = xattr_list(dlg->filepath, list_buf, sizeof(list_buf));
    
    if (list_size <= 0) {
        kprintf("[Properties] No xattrs for %s\n", dlg->filepath);
        return;
    }
    
    // Parse list and load values
    char *ptr = list_buf;
    while (ptr < list_buf + list_size && dlg->xattr_count < PROP_MAX_XATTRS) {
        if (*ptr == '\0') {
            ptr++;
            continue;
        }
        
        prop_xattr_entry_t *entry = &dlg->xattrs[dlg->xattr_count];
        strncpy(entry->name, ptr, PROP_XATTR_NAME_MAX - 1);
        
        // Get value
        char value_buf[PROP_XATTR_VALUE_MAX];
        ssize_t val_size = xattr_get(dlg->filepath, ptr, value_buf, sizeof(value_buf) - 1);
        if (val_size > 0) {
            value_buf[val_size] = '\0';
            strncpy(entry->value, value_buf, PROP_XATTR_VALUE_MAX - 1);
        }
        
        // Check for standard attributes
        if (strcmp(ptr, XATTR_USER_MIME_TYPE) == 0) {
            strncpy(dlg->mime_type, entry->value, sizeof(dlg->mime_type) - 1);
        } else if (strcmp(ptr, XATTR_USER_DESCRIPTION) == 0) {
            strncpy(dlg->description, entry->value, sizeof(dlg->description) - 1);
        } else if (strcmp(ptr, XATTR_USER_TAGS) == 0) {
            strncpy(dlg->tags, entry->value, sizeof(dlg->tags) - 1);
        } else if (strcmp(ptr, XATTR_USER_OPEN_WITH) == 0) {
            strncpy(dlg->open_with, entry->value, sizeof(dlg->open_with) - 1);
        }
        
        dlg->xattr_count++;
        ptr += strlen(ptr) + 1;
    }
    
    kprintf("[Properties] Loaded %d xattrs\n", dlg->xattr_count);
}

int properties_save_xattrs(properties_dialog_t *dlg) {
    if (!dlg || !dlg->has_changes) return 0;
    
    int saved = 0;
    
    // Save modified attributes
    for (int i = 0; i < dlg->xattr_count; i++) {
        prop_xattr_entry_t *entry = &dlg->xattrs[i];
        
        if (entry->deleted) {
            if (xattr_remove(dlg->filepath, entry->name) == 0) {
                kprintf("[Properties] Removed xattr: %s\n", entry->name);
                saved++;
            }
        } else if (entry->modified || entry->is_new) {
            if (xattr_set(dlg->filepath, entry->name, entry->value, 
                         strlen(entry->value), 0) == 0) {
                kprintf("[Properties] Saved xattr: %s = %s\n", entry->name, entry->value);
                saved++;
                entry->modified = false;
                entry->is_new = false;
            }
        }
    }
    
    // Save standard attributes if modified
    if (dlg->mime_type[0]) {
        xattr_set(dlg->filepath, XATTR_USER_MIME_TYPE, dlg->mime_type, 
                 strlen(dlg->mime_type), 0);
    }
    if (dlg->description[0]) {
        xattr_set(dlg->filepath, XATTR_USER_DESCRIPTION, dlg->description,
                 strlen(dlg->description), 0);
    }
    if (dlg->tags[0]) {
        xattr_set(dlg->filepath, XATTR_USER_TAGS, dlg->tags,
                 strlen(dlg->tags), 0);
    }
    if (dlg->open_with[0]) {
        xattr_set(dlg->filepath, XATTR_USER_OPEN_WITH, dlg->open_with,
                 strlen(dlg->open_with), 0);
    }
    
    dlg->has_changes = false;
    dlg->buttons[PROP_BTN_APPLY].enabled = false;
    
    return saved;
}

// ============================================================================
// Drawing Functions
// ============================================================================

void properties_draw(properties_dialog_t *dlg) {
    if (!dlg || !dlg->window) return;
    
    int32_t x = dlg->window->bounds.x;
    int32_t y = dlg->window->bounds.y;
    int32_t w = PROP_DIALOG_WIDTH;
    int32_t h = PROP_DIALOG_HEIGHT;
    
    // Draw background
    fb_fill_rect(x, y, w, h, PROP_BG_COLOR & 0xFFFFFF);
    
    // Draw tabs
    properties_draw_tabs(dlg);
    
    // Draw content based on active tab
    if (dlg->active_tab == PROP_TAB_GENERAL) {
        properties_draw_general_tab(dlg);
    } else {
        properties_draw_attributes_tab(dlg);
    }
    
    // Draw bottom buttons
    properties_draw_buttons(dlg);
}

void properties_draw_tabs(properties_dialog_t *dlg) {
    int32_t x = dlg->window->bounds.x;
    int32_t y = dlg->window->bounds.y;
    
    // Tab bar background
    fb_fill_rect(x, y, PROP_DIALOG_WIDTH, PROP_TAB_HEIGHT, PROP_TAB_BG & 0xFFFFFF);
    
    // Draw tabs
    const char *tab_names[] = { "General", "Attributes" };
    int tab_x = x + PROP_PADDING;
    
    for (int i = 0; i < PROP_TAB_COUNT; i++) {
        int tab_w = strlen(tab_names[i]) * PROP_CHAR_W + 16;
        
        if (i == dlg->active_tab) {
            // Active tab
            fb_fill_rect(tab_x, y + 2, tab_w, PROP_TAB_HEIGHT - 2, PROP_TAB_ACTIVE & 0xFFFFFF);
            fb_draw_rect(tab_x, y + 2, tab_w, PROP_TAB_HEIGHT - 1, PROP_TAB_BORDER & 0xFFFFFF);
            prop_draw_string(tab_x + 8, y + 6, tab_names[i], PROP_TEXT_COLOR, PROP_TAB_ACTIVE);
        } else {
            // Inactive tab
            prop_draw_string_nobg(tab_x + 8, y + 6, tab_names[i], PROP_LABEL_COLOR);
        }
        
        tab_x += tab_w + 4;
    }
    
    // Separator line
    fb_fill_rect(x, y + PROP_TAB_HEIGHT - 1, PROP_DIALOG_WIDTH, 1, PROP_TAB_BORDER & 0xFFFFFF);
}

void properties_draw_general_tab(properties_dialog_t *dlg) {
    int32_t x = dlg->window->bounds.x + PROP_PADDING;
    int32_t y = dlg->window->bounds.y + PROP_TAB_HEIGHT + PROP_PADDING;
    int32_t w = PROP_DIALOG_WIDTH - PROP_PADDING * 2;
    
    // Draw file icon
    prop_draw_icon(dlg, x, y);
    
    // Draw filename next to icon
    prop_draw_string_nobg(x + 60, y + 10, dlg->filename, PROP_TEXT_COLOR);
    prop_draw_string_nobg(x + 60, y + 28, properties_get_type_name(dlg->file_type, dlg->is_directory), 
                          PROP_LABEL_COLOR);
    
    y += 60;
    
    // Separator
    fb_fill_rect(x, y, w, 1, PROP_SEPARATOR_COLOR & 0xFFFFFF);
    y += PROP_PADDING;
    
    // File information
    int32_t label_x = x;
    int32_t value_x = x + PROP_LABEL_WIDTH;
    
    // Location
    prop_draw_string_nobg(label_x, y, "Location:", PROP_LABEL_COLOR);
    prop_draw_string_nobg(value_x, y, dlg->location, PROP_VALUE_COLOR);
    y += PROP_LINE_HEIGHT;
    
    // Size
    prop_draw_string_nobg(label_x, y, "Size:", PROP_LABEL_COLOR);
    prop_draw_string_nobg(value_x, y, properties_format_size(dlg->file_size), PROP_VALUE_COLOR);
    y += PROP_LINE_HEIGHT;
    
    // Modified
    prop_draw_string_nobg(label_x, y, "Modified:", PROP_LABEL_COLOR);
    prop_draw_string_nobg(value_x, y, properties_format_date(dlg->mod_date, dlg->mod_time), PROP_VALUE_COLOR);
    y += PROP_LINE_HEIGHT;
    
    // Separator
    y += PROP_PADDING / 2;
    fb_fill_rect(x, y, w, 1, PROP_SEPARATOR_COLOR & 0xFFFFFF);
    y += PROP_PADDING;
    
    // Standard Extended Attributes Section
    prop_draw_string_nobg(x, y, "Extended Attributes", PROP_HEADER_COLOR);
    y += PROP_LINE_HEIGHT + 4;
    
    // MIME Type
    prop_draw_string_nobg(label_x, y, "MIME Type:", PROP_LABEL_COLOR);
    fb_fill_rect(value_x - 2, y - 2, PROP_VALUE_WIDTH, PROP_CHAR_H + 4, PROP_INPUT_BG & 0xFFFFFF);
    fb_draw_rect(value_x - 2, y - 2, PROP_VALUE_WIDTH, PROP_CHAR_H + 4, PROP_INPUT_BORDER & 0xFFFFFF);
    if (dlg->mime_type[0]) {
        prop_draw_string_nobg(value_x, y, dlg->mime_type, PROP_VALUE_COLOR);
    }
    y += PROP_LINE_HEIGHT + 4;
    
    // Description
    prop_draw_string_nobg(label_x, y, "Description:", PROP_LABEL_COLOR);
    fb_fill_rect(value_x - 2, y - 2, PROP_VALUE_WIDTH, PROP_CHAR_H + 4, PROP_INPUT_BG & 0xFFFFFF);
    fb_draw_rect(value_x - 2, y - 2, PROP_VALUE_WIDTH, PROP_CHAR_H + 4, PROP_INPUT_BORDER & 0xFFFFFF);
    if (dlg->description[0]) {
        prop_draw_string_nobg(value_x, y, dlg->description, PROP_VALUE_COLOR);
    }
    y += PROP_LINE_HEIGHT + 4;
    
    // Tags
    prop_draw_string_nobg(label_x, y, "Tags:", PROP_LABEL_COLOR);
    fb_fill_rect(value_x - 2, y - 2, PROP_VALUE_WIDTH, PROP_CHAR_H + 4, PROP_INPUT_BG & 0xFFFFFF);
    fb_draw_rect(value_x - 2, y - 2, PROP_VALUE_WIDTH, PROP_CHAR_H + 4, PROP_INPUT_BORDER & 0xFFFFFF);
    if (dlg->tags[0]) {
        prop_draw_string_nobg(value_x, y, dlg->tags, PROP_VALUE_COLOR);
    }
    y += PROP_LINE_HEIGHT + 4;
    
    // Open With
    prop_draw_string_nobg(label_x, y, "Open With:", PROP_LABEL_COLOR);
    fb_fill_rect(value_x - 2, y - 2, PROP_VALUE_WIDTH, PROP_CHAR_H + 4, PROP_INPUT_BG & 0xFFFFFF);
    fb_draw_rect(value_x - 2, y - 2, PROP_VALUE_WIDTH, PROP_CHAR_H + 4, PROP_INPUT_BORDER & 0xFFFFFF);
    if (dlg->open_with[0]) {
        prop_draw_string_nobg(value_x, y, dlg->open_with, PROP_VALUE_COLOR);
    }
}

void properties_draw_attributes_tab(properties_dialog_t *dlg) {
    int32_t x = dlg->window->bounds.x + PROP_PADDING;
    int32_t y = dlg->window->bounds.y + PROP_TAB_HEIGHT + PROP_PADDING;
    int32_t w = PROP_DIALOG_WIDTH - PROP_PADDING * 2 - 70;  // Leave room for buttons
    int32_t h = 280;
    
    // Title
    prop_draw_string_nobg(x, y, "All Extended Attributes", PROP_HEADER_COLOR);
    y += PROP_LINE_HEIGHT;
    
    // List header
    fb_fill_rect(x, y, w, 20, PROP_TAB_BG & 0xFFFFFF);
    prop_draw_string_nobg(x + 4, y + 2, "Name", PROP_LABEL_COLOR);
    prop_draw_string_nobg(x + 160, y + 2, "Value", PROP_LABEL_COLOR);
    y += 20;
    
    // List background
    fb_fill_rect(x, y, w, h, PROP_INPUT_BG & 0xFFFFFF);
    fb_draw_rect(x, y, w, h, PROP_INPUT_BORDER & 0xFFFFFF);
    
    // Draw attribute list
    int visible_count = h / PROP_LINE_HEIGHT;
    int32_t item_y = y + 2;
    
    for (int i = dlg->xattr_scroll; i < dlg->xattr_count && i < dlg->xattr_scroll + visible_count; i++) {
        prop_xattr_entry_t *entry = &dlg->xattrs[i];
        
        if (entry->deleted) continue;
        
        // Highlight selected
        if (i == dlg->xattr_selected) {
            fb_fill_rect(x + 1, item_y, w - 2, PROP_LINE_HEIGHT, PROP_LIST_SELECTED & 0xFFFFFF);
            prop_draw_string_nobg(x + 4, item_y + 3, entry->name, PROP_TAB_ACTIVE);
            prop_draw_string_nobg(x + 160, item_y + 3, entry->value, PROP_TAB_ACTIVE);
        } else {
            prop_draw_string_nobg(x + 4, item_y + 3, entry->name, PROP_TEXT_COLOR);
            prop_draw_string_nobg(x + 160, item_y + 3, entry->value, PROP_VALUE_COLOR);
        }
        
        // Show modified indicator
        if (entry->modified || entry->is_new) {
            prop_draw_string_nobg(x + w - 12, item_y + 3, "*", PROP_HEADER_COLOR);
        }
        
        item_y += PROP_LINE_HEIGHT;
    }
    
    // Show attribute buttons
    dlg->buttons[PROP_BTN_ADD_ATTR].visible = true;
    dlg->buttons[PROP_BTN_EDIT_ATTR].visible = true;
    dlg->buttons[PROP_BTN_EDIT_ATTR].enabled = (dlg->xattr_selected >= 0);
    dlg->buttons[PROP_BTN_DEL_ATTR].visible = true;
    dlg->buttons[PROP_BTN_DEL_ATTR].enabled = (dlg->xattr_selected >= 0);
}

void properties_draw_buttons(properties_dialog_t *dlg) {
    for (int i = 0; i < PROP_BTN_COUNT; i++) {
        prop_button_t *btn = &dlg->buttons[i];
        if (!btn->visible) continue;
        
        int32_t x = dlg->window->bounds.x + btn->x;
        int32_t y = dlg->window->bounds.y + btn->y;
        
        uint32_t bg_color = PROP_BTN_BG;
        if (!btn->enabled) {
            bg_color = PROP_TAB_BG;
        } else if (btn->pressed) {
            bg_color = PROP_LIST_SELECTED;
        } else if (btn->hovered) {
            bg_color = PROP_BTN_HOVER;
        }
        
        fb_fill_rect(x, y, btn->w, btn->h, bg_color & 0xFFFFFF);
        fb_draw_rect(x, y, btn->w, btn->h, PROP_BTN_BORDER & 0xFFFFFF);
        
        // Center text
        int text_x = x + (btn->w - strlen(btn->label) * PROP_CHAR_W) / 2;
        int text_y = y + (btn->h - PROP_CHAR_H) / 2;
        
        uint32_t text_color = btn->enabled ? PROP_TEXT_COLOR : PROP_LABEL_COLOR;
        if (btn->pressed) text_color = PROP_TAB_ACTIVE;
        
        prop_draw_string_nobg(text_x, text_y, btn->label, text_color);
    }
}

// ============================================================================
// Event Handlers
// ============================================================================

void properties_handle_mouse(properties_dialog_t *dlg, int32_t mx, int32_t my, bool left_down, bool left_click) {
    if (!dlg) return;
    
    int32_t x = mx - dlg->window->bounds.x;
    int32_t y = my - dlg->window->bounds.y;
    
    // Reset button hover states
    for (int i = 0; i < PROP_BTN_COUNT; i++) {
        dlg->buttons[i].hovered = false;
        dlg->buttons[i].pressed = false;
    }
    
    // Check tab clicks
    if (y >= 0 && y < PROP_TAB_HEIGHT && left_click) {
        int tab_x = PROP_PADDING;
        const char *tab_names[] = { "General", "Attributes" };
        
        for (int i = 0; i < PROP_TAB_COUNT; i++) {
            int tab_w = strlen(tab_names[i]) * PROP_CHAR_W + 16;
            if (x >= tab_x && x < tab_x + tab_w) {
                dlg->active_tab = i;
                // Update attribute button visibility
                bool show_attr_btns = (i == PROP_TAB_ATTRIBUTES);
                dlg->buttons[PROP_BTN_ADD_ATTR].visible = show_attr_btns;
                dlg->buttons[PROP_BTN_EDIT_ATTR].visible = show_attr_btns;
                dlg->buttons[PROP_BTN_DEL_ATTR].visible = show_attr_btns;
                break;
            }
            tab_x += tab_w + 4;
        }
    }
    
    // Check button clicks
    for (int i = 0; i < PROP_BTN_COUNT; i++) {
        prop_button_t *btn = &dlg->buttons[i];
        if (!btn->visible || !btn->enabled) continue;
        
        if (x >= btn->x && x < btn->x + btn->w &&
            y >= btn->y && y < btn->y + btn->h) {
            btn->hovered = true;
            btn->pressed = left_down;
            
            if (left_click) {
                switch (i) {
                    case PROP_BTN_OK:
                        if (dlg->has_changes) {
                            properties_save_xattrs(dlg);
                        }
                        dlg->result = PROP_RESULT_OK;
                        dlg->running = false;
                        break;
                    case PROP_BTN_CANCEL:
                        dlg->result = PROP_RESULT_CANCEL;
                        dlg->running = false;
                        break;
                    case PROP_BTN_APPLY:
                        properties_save_xattrs(dlg);
                        break;
                    case PROP_BTN_ADD_ATTR:
                        prop_add_xattr(dlg);
                        break;
                    case PROP_BTN_EDIT_ATTR:
                        prop_edit_xattr(dlg);
                        break;
                    case PROP_BTN_DEL_ATTR:
                        prop_delete_xattr(dlg);
                        break;
                }
            }
        }
    }
    
    // Check attribute list clicks (on attributes tab)
    if (dlg->active_tab == PROP_TAB_ATTRIBUTES) {
        int32_t list_x = PROP_PADDING;
        int32_t list_y = PROP_TAB_HEIGHT + PROP_PADDING + PROP_LINE_HEIGHT + 20;
        int32_t list_w = PROP_DIALOG_WIDTH - PROP_PADDING * 2 - 70;
        int32_t list_h = 280;
        
        if (x >= list_x && x < list_x + list_w &&
            y >= list_y && y < list_y + list_h && left_click) {
            int idx = (y - list_y) / PROP_LINE_HEIGHT + dlg->xattr_scroll;
            if (idx >= 0 && idx < dlg->xattr_count) {
                dlg->xattr_selected = idx;
            }
        }
    }
}

void properties_handle_key(properties_dialog_t *dlg, char c, uint32_t keycode) {
    if (!dlg) return;
    
    // Escape to close
    if (keycode == 0x01 || c == 27) {  // Escape
        dlg->result = PROP_RESULT_CANCEL;
        dlg->running = false;
        return;
    }
    
    // Enter to confirm
    if (c == '\n' || c == '\r') {
        if (dlg->has_changes) {
            properties_save_xattrs(dlg);
        }
        dlg->result = PROP_RESULT_OK;
        dlg->running = false;
        return;
    }
}

// ============================================================================
// Attribute Operations
// ============================================================================

static void prop_add_xattr(properties_dialog_t *dlg) {
    if (dlg->xattr_count >= PROP_MAX_XATTRS) return;
    
    prop_xattr_entry_t *entry = &dlg->xattrs[dlg->xattr_count];
    strcpy(entry->name, "user.custom");
    strcpy(entry->value, "");
    entry->is_new = true;
    entry->modified = true;
    
    dlg->xattr_count++;
    dlg->xattr_selected = dlg->xattr_count - 1;
    dlg->has_changes = true;
    dlg->buttons[PROP_BTN_APPLY].enabled = true;
}

static void prop_edit_xattr(properties_dialog_t *dlg) {
    // TODO: Implement inline editing or popup editor
    if (dlg->xattr_selected < 0) return;
    kprintf("[Properties] Edit xattr: %s\n", dlg->xattrs[dlg->xattr_selected].name);
}

static void prop_delete_xattr(properties_dialog_t *dlg) {
    if (dlg->xattr_selected < 0 || dlg->xattr_selected >= dlg->xattr_count) return;
    
    dlg->xattrs[dlg->xattr_selected].deleted = true;
    dlg->has_changes = true;
    dlg->buttons[PROP_BTN_APPLY].enabled = true;
    dlg->xattr_selected = -1;
}

// ============================================================================
// Window Manager Callbacks
// ============================================================================

void properties_on_event(void *app_data, gui_event_t *event) {
    properties_dialog_t *dlg = (properties_dialog_t *)app_data;
    if (!dlg || !event) return;
    
    switch (event->type) {
        case EVENT_MOUSE_MOVE:
        case EVENT_MOUSE_DOWN:
        case EVENT_MOUSE_UP:
            properties_handle_mouse(dlg, event->mouse_x, event->mouse_y,
                                   event->type == EVENT_MOUSE_DOWN,
                                   event->type == EVENT_MOUSE_DOWN);
            break;
        case EVENT_KEY_DOWN:
            properties_handle_key(dlg, event->key_char, event->keycode);
            break;
        default:
            break;
    }
}

void properties_on_draw(void *app_data) {
    properties_dialog_t *dlg = (properties_dialog_t *)app_data;
    if (dlg) {
        properties_draw(dlg);
    }
}

void properties_on_destroy(void *app_data) {
    properties_dialog_t *dlg = (properties_dialog_t *)app_data;
    if (dlg) {
        dlg->running = false;
    }
}

// ============================================================================
// Run Dialog
// ============================================================================

properties_result_t properties_run(properties_dialog_t *dlg) {
    if (!dlg) return PROP_RESULT_CANCEL;
    
    // Register with window manager
    // TODO: Integrate with wm_register_app when available
    
    // Initial draw
    properties_draw(dlg);
    fb_swap_buffers();
    
    // Main loop
    while (dlg->running) {
        // Get mouse state
        int32_t mx, my;
        bool left_down = false, right_down = false;
        
        extern void mouse_get_position(int32_t *x, int32_t *y);
        extern uint8_t mouse_get_buttons(void);
        
        mouse_get_position(&mx, &my);
        uint8_t btns = mouse_get_buttons();
        left_down = (btns & 1) != 0;
        
        static bool prev_left = false;
        bool left_click = left_down && !prev_left;
        prev_left = left_down;
        
        // Handle mouse
        properties_handle_mouse(dlg, mx, my, left_down, left_click);
        
        // Check for keyboard input
        extern int keyboard_get_char(void);
        
        int c = keyboard_get_char();
        if (c != 0) {
            
            
            properties_handle_key(dlg, (char)c, c);
        }
        
        // Redraw
        properties_draw(dlg);
        fb_swap_buffers();
        
        // Small delay
        for (volatile int i = 0; i < 100000; i++);
    }
    
    return dlg->result;
}
