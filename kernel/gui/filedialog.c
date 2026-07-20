// filedialog.c - Universal File/Folder Dialog for MayteraOS
#include "filedialog.h"
#include "window.h"
#include "desktop.h"
#include "../types.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../cpu/isr.h"
#include "../drivers/mouse.h"
#include "../fs/fat.h"

// External filesystem
extern fat_fs_t g_fat_fs;
extern volatile uint64_t timer_ticks;

// Forward declarations
static void fd_draw(filedialog_t *dlg);
static void fd_handle_mouse(filedialog_t *dlg, int32_t mx, int32_t my, bool left_click, bool right_click);
static void fd_handle_key(filedialog_t *dlg, char c, uint32_t keycode);
static bool fd_match_filter(const char *filename, const char *filter);
static void fd_go_up_directory(filedialog_t *dlg);

// Draw helpers
static void fd_draw_char(int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg) {
    fb_fill_rect(x, y, FD_CHAR_W, FD_CHAR_H, bg & 0xFFFFFF);
    if (c >= ' ' && c < 127) {
        const uint8_t *glyph = font_get_glyph(c);
        if (glyph) {
            for (int r = 0; r < FONT_HEIGHT && r < FD_CHAR_H; r++) {
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

static void fd_draw_string(int32_t x, int32_t y, const char *str, uint32_t fg, uint32_t bg) {
    while (*str) {
        fd_draw_char(x, y, *str, fg, bg);
        x += FD_CHAR_W;
        str++;
    }
}

static void fd_draw_string_nobg(int32_t x, int32_t y, const char *str, uint32_t fg) {
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
        x += FD_CHAR_W;
        str++;
    }
}

// Create file dialog
filedialog_t *filedialog_create(filedialog_mode_t mode, const char *title) {
    filedialog_t *dlg = (filedialog_t *)kmalloc(sizeof(filedialog_t));
    if (!dlg) return NULL;
    
    memset(dlg, 0, sizeof(filedialog_t));
    dlg->mode = mode;
    dlg->title = title ? title : "File Dialog";
    dlg->result = FILEDIALOG_RESULT_NONE;
    dlg->selected_index = -1;
    dlg->scroll_offset = 0;
    
    // Set default button labels
    switch (mode) {
        case FILEDIALOG_OPEN:
            dlg->ok_label = "Open";
            break;
        case FILEDIALOG_SAVE:
            dlg->ok_label = "Save";
            break;
        case FILEDIALOG_FOLDER:
            dlg->ok_label = "Select";
            break;
    }
    dlg->cancel_label = "Cancel";
    
    // Set default filter
    strcpy(dlg->filter, "*.*");
    dlg->filter_description = "All Files";
    
    // Set default path
    strcpy(dlg->current_path, "/");
    
    // Create window centered on screen
    int screen_w = fb_get_width();
    int screen_h = fb_get_height();
    int wx = (screen_w - FD_WIDTH) / 2;
    int wy = (screen_h - FD_HEIGHT) / 2;
    
    dlg->window = window_create(dlg->title, wx, wy, FD_WIDTH, FD_HEIGHT);
    if (!dlg->window) {
        kfree(dlg);
        return NULL;
    }
    
    // Window is not resizable by default (no WINDOW_FLAG_RESIZABLE set)
    
    return dlg;
}

// Destroy file dialog
void filedialog_destroy(filedialog_t *dlg) {
    if (!dlg) return;
    if (dlg->window) {
        window_destroy(dlg->window);
    }
    kfree(dlg);
}

// Set current path
void filedialog_set_path(filedialog_t *dlg, const char *path) {
    if (!dlg || !path) return;
    strncpy(dlg->current_path, path, FD_MAX_PATH - 1);
    dlg->current_path[FD_MAX_PATH - 1] = '\0';
}

// Set file filter
void filedialog_set_filter(filedialog_t *dlg, const char *filter, const char *desc) {
    if (!dlg) return;
    if (filter) {
        strncpy(dlg->filter, filter, FD_MAX_FILTER - 1);
        dlg->filter[FD_MAX_FILTER - 1] = '\0';
    }
    dlg->filter_description = desc;
}

// Set default filename
void filedialog_set_filename(filedialog_t *dlg, const char *filename) {
    if (!dlg) return;
    if (filename) {
        strncpy(dlg->filename, filename, FD_MAX_FILENAME - 1);
        dlg->filename[FD_MAX_FILENAME - 1] = '\0';
        dlg->filename_cursor = strlen(dlg->filename);
    }
}

// Check if filename matches filter
static bool fd_match_filter(const char *filename, const char *filter) {
    if (!filename || !filter) return true;
    if (strcmp(filter, "*.*") == 0 || strcmp(filter, "*") == 0) return true;
    
    // Simple wildcard matching for *.ext format
    if (filter[0] == '*' && filter[1] == '.') {
        const char *ext = filter + 1;  // ".ext"
        int ext_len = strlen(ext);
        int name_len = strlen(filename);
        
        if (name_len >= ext_len) {
            // Case-insensitive comparison
            const char *file_ext = filename + name_len - ext_len;
            for (int i = 0; i < ext_len; i++) {
                char c1 = file_ext[i];
                char c2 = ext[i];
                if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
                if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
                if (c1 != c2) return false;
            }
            return true;
        }
    }
    
    return true;  // Default: show all
}

// Refresh directory listing
void filedialog_refresh(filedialog_t *dlg) {
    if (!dlg) return;
    
    dlg->entry_count = 0;
    dlg->selected_index = -1;
    dlg->scroll_offset = 0;
    
    // Open directory
    fat_file_t dir;
    if (fat_open(&g_fat_fs, dlg->current_path, &dir) != 0) {
        kprintf("[FileDialog] Failed to open: %s\n", dlg->current_path);
        return;
    }
    
    if (!dir.is_dir) {
        fat_close(&dir);
        return;
    }
    
    // Add ".." entry if not at root
    if (strcmp(dlg->current_path, "/") != 0) {
        strcpy(dlg->entries[dlg->entry_count].name, "..");
        dlg->entries[dlg->entry_count].size = 0;
        dlg->entries[dlg->entry_count].is_dir = true;
        dlg->entry_count++;
    }
    
    // Read directory entries
    fat_dir_entry_t fat_entry;
    char name[256];
    
    while (fat_readdir(&dir, &fat_entry, name) == 0) {
        if (dlg->entry_count >= FD_MAX_ENTRIES) break;
        
        // Skip . and ..
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        
        // Skip hidden/system files
        if (fat_entry.attr & (FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM | FAT_ATTR_VOLUME_ID)) continue;
        
        bool is_dir = (fat_entry.attr & FAT_ATTR_DIRECTORY) != 0;
        
        // For folder selection mode, only show folders
        if (dlg->mode == FILEDIALOG_FOLDER && !is_dir) continue;
        
        // Apply file filter (only for files, not folders)
        if (!is_dir && !fd_match_filter(name, dlg->filter)) continue;
        
        strncpy(dlg->entries[dlg->entry_count].name, name, FD_MAX_FILENAME - 1);
        dlg->entries[dlg->entry_count].name[FD_MAX_FILENAME - 1] = '\0';
        dlg->entries[dlg->entry_count].size = fat_entry.file_size;
        dlg->entries[dlg->entry_count].is_dir = is_dir;
        dlg->entry_count++;
    }
    
    fat_close(&dir);
    
    // Sort: directories first, then alphabetically
    for (int i = 0; i < dlg->entry_count - 1; i++) {
        for (int j = 0; j < dlg->entry_count - i - 1; j++) {
            fd_entry_t *a = &dlg->entries[j];
            fd_entry_t *b = &dlg->entries[j + 1];
            
            bool swap = false;
            if (a->is_dir != b->is_dir) {
                swap = !a->is_dir;  // Directories first
            } else {
                // Alphabetical (case-insensitive)
                // Case-insensitive compare
                int cmp = 0;
                for (int ci = 0; a->name[ci] && b->name[ci]; ci++) {
                    char ca = a->name[ci], cb = b->name[ci];
                    if (ca >= 'a' && ca <= 'z') ca -= 32;
                    if (cb >= 'a' && cb <= 'z') cb -= 32;
                    if (ca != cb) { cmp = ca - cb; break; }
                }
                if (cmp == 0) cmp = strlen(a->name) - strlen(b->name);
                swap = cmp > 0;
            }
            
            if (swap) {
                fd_entry_t tmp = *a;
                *a = *b;
                *b = tmp;
            }
        }
    }
    
    kprintf("[FileDialog] Path: %s, mounted: %d\n", dlg->current_path, g_fat_fs.mounted);
    kprintf("[FileDialog] Loaded %d entries from %s\n", dlg->entry_count, dlg->current_path);
}

// Go up one directory
static void fd_go_up_directory(filedialog_t *dlg) {
    if (!dlg || strcmp(dlg->current_path, "/") == 0) return;
    
    // Find last slash
    int len = strlen(dlg->current_path);
    int last_slash = 0;
    for (int i = len - 1; i >= 0; i--) {
        if (dlg->current_path[i] == '/') {
            last_slash = i;
            break;
        }
    }
    
    if (last_slash == 0) {
        strcpy(dlg->current_path, "/");
    } else {
        dlg->current_path[last_slash] = '\0';
    }
    
    filedialog_refresh(dlg);
}

// Draw button
static void fd_draw_button(int32_t x, int32_t y, int32_t w, int32_t h, 
                           const char *label, bool hovered, bool pressed) {
    uint32_t bg = pressed ? FD_BUTTON_PRESS : (hovered ? FD_BUTTON_HOVER : FD_BUTTON_BG);
    
    // Background
    fb_fill_rect(x, y, w, h, bg & 0xFFFFFF);
    
    // Border
    fb_fill_rect(x, y, w, 1, 0xA0A0A0);           // Top
    fb_fill_rect(x, y + h - 1, w, 1, 0x606060);   // Bottom
    fb_fill_rect(x, y, 1, h, 0xA0A0A0);           // Left
    fb_fill_rect(x + w - 1, y, 1, h, 0x606060);   // Right
    
    // Label centered
    int label_len = strlen(label);
    int label_x = x + (w - label_len * FD_CHAR_W) / 2;
    int label_y = y + (h - FD_CHAR_H) / 2;
    fd_draw_string_nobg(label_x, label_y, label, FD_BUTTON_TEXT);
}

// Draw the dialog
static void fd_draw(filedialog_t *dlg) {
    if (!dlg || !dlg->window) return;
    
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(dlg->window, &wx, &wy, &ww, &wh);
    
    // Fill background
    fb_fill_rect(wx, wy, ww, wh, FD_BG_COLOR & 0xFFFFFF);
    
    int y = wy + FD_PADDING;
    
    // Path bar
    fb_fill_rect(wx + FD_PADDING, y, ww - 2 * FD_PADDING, FD_PATH_HEIGHT, FD_PATH_BG & 0xFFFFFF);
    fb_fill_rect(wx + FD_PADDING, y, ww - 2 * FD_PADDING, 1, FD_LIST_BORDER & 0xFFFFFF);
    fb_fill_rect(wx + FD_PADDING, y + FD_PATH_HEIGHT - 1, ww - 2 * FD_PADDING, 1, FD_LIST_BORDER & 0xFFFFFF);
    
    // Truncate path if too long
    char display_path[64];
    int max_chars = (ww - 2 * FD_PADDING - 8) / FD_CHAR_W;
    if ((int)strlen(dlg->current_path) > max_chars) {
        strcpy(display_path, "...");
        strcat(display_path, dlg->current_path + strlen(dlg->current_path) - max_chars + 3);
    } else {
        strcpy(display_path, dlg->current_path);
    }
    fd_draw_string_nobg(wx + FD_PADDING + 4, y + 3, display_path, FD_TEXT_COLOR);
    
    y += FD_PATH_HEIGHT + FD_PADDING;
    
    // File list area
    int list_h = wh - FD_PATH_HEIGHT - FD_INPUT_HEIGHT - FD_BUTTON_HEIGHT - 5 * FD_PADDING;
    if (dlg->mode != FILEDIALOG_SAVE) {
        list_h += FD_INPUT_HEIGHT + FD_PADDING;  // No filename input for Open/Folder modes
    }
    
    dlg->visible_entries = list_h / FD_ENTRY_HEIGHT;
    
    // List background
    fb_fill_rect(wx + FD_PADDING, y, ww - 2 * FD_PADDING, list_h, FD_LIST_BG & 0xFFFFFF);
    
    // List border
    fb_fill_rect(wx + FD_PADDING, y, ww - 2 * FD_PADDING, 1, FD_LIST_BORDER & 0xFFFFFF);
    fb_fill_rect(wx + FD_PADDING, y + list_h - 1, ww - 2 * FD_PADDING, 1, FD_LIST_BORDER & 0xFFFFFF);
    fb_fill_rect(wx + FD_PADDING, y, 1, list_h, FD_LIST_BORDER & 0xFFFFFF);
    fb_fill_rect(wx + ww - FD_PADDING - 1, y, 1, list_h, FD_LIST_BORDER & 0xFFFFFF);
    
    // Draw entries
    int entry_y = y + 1;
    for (int i = 0; i < dlg->visible_entries && (i + dlg->scroll_offset) < dlg->entry_count; i++) {
        int idx = i + dlg->scroll_offset;
        fd_entry_t *entry = &dlg->entries[idx];
        
        uint32_t fg = entry->is_dir ? FD_DIR_COLOR : FD_TEXT_COLOR;
        uint32_t bg = FD_LIST_BG;
        
        if (idx == dlg->selected_index) {
            fg = FD_SELECT_FG;
            bg = FD_SELECT_BG;
            fb_fill_rect(wx + FD_PADDING + 1, entry_y, ww - 2 * FD_PADDING - 2, FD_ENTRY_HEIGHT, bg & 0xFFFFFF);
        }
        
        // Icon placeholder (folder or file indicator)
        char prefix[4] = "   ";
        if (entry->is_dir) {
            prefix[0] = '[';
            prefix[1] = 'D';
            prefix[2] = ']';
        }
        
        fd_draw_string(wx + FD_PADDING + 4, entry_y + 1, prefix, fg, bg);
        fd_draw_string(wx + FD_PADDING + 4 + 4 * FD_CHAR_W, entry_y + 1, entry->name, fg, bg);
        
        entry_y += FD_ENTRY_HEIGHT;
    }
    
    y += list_h + FD_PADDING;
    
    // Filename input (Save mode only)
    if (dlg->mode == FILEDIALOG_SAVE) {
        // Label
        fd_draw_string_nobg(wx + FD_PADDING, y + 3, "Name:", FD_TEXT_COLOR);
        
        // Input field
        int input_x = wx + FD_PADDING + 6 * FD_CHAR_W;
        int input_w = ww - 2 * FD_PADDING - 6 * FD_CHAR_W;
        
        fb_fill_rect(input_x, y, input_w, FD_INPUT_HEIGHT, FD_INPUT_BG & 0xFFFFFF);
        
        // Border (blue if focused)
        uint32_t border_color = dlg->filename_focused ? FD_INPUT_FOCUS : FD_INPUT_BORDER;
        fb_fill_rect(input_x, y, input_w, 1, border_color & 0xFFFFFF);
        fb_fill_rect(input_x, y + FD_INPUT_HEIGHT - 1, input_w, 1, border_color & 0xFFFFFF);
        fb_fill_rect(input_x, y, 1, FD_INPUT_HEIGHT, border_color & 0xFFFFFF);
        fb_fill_rect(input_x + input_w - 1, y, 1, FD_INPUT_HEIGHT, border_color & 0xFFFFFF);
        
        // Filename text
        fd_draw_string_nobg(input_x + 4, y + 3, dlg->filename, FD_TEXT_COLOR);
        
        // Cursor
        if (dlg->filename_focused && ((timer_ticks / 500) % 2 == 0)) {
            int cursor_x = input_x + 4 + dlg->filename_cursor * FD_CHAR_W;
            fb_fill_rect(cursor_x, y + 3, 2, FD_CHAR_H, FD_TEXT_COLOR & 0xFFFFFF);
        }
        
        y += FD_INPUT_HEIGHT + FD_PADDING;
    }
    
    // Buttons
    int btn_y = wy + wh - FD_PADDING - FD_BUTTON_HEIGHT;
    int ok_x = wx + ww - FD_PADDING - FD_BUTTON_WIDTH;
    int cancel_x = ok_x - FD_PADDING - FD_BUTTON_WIDTH;
    
    fd_draw_button(cancel_x, btn_y, FD_BUTTON_WIDTH, FD_BUTTON_HEIGHT, 
                   dlg->cancel_label, dlg->cancel_hovered, dlg->cancel_pressed);
    fd_draw_button(ok_x, btn_y, FD_BUTTON_WIDTH, FD_BUTTON_HEIGHT,
                   dlg->ok_label, dlg->ok_hovered, dlg->ok_pressed);
}

// Handle mouse input
static void fd_handle_mouse(filedialog_t *dlg, int32_t mx, int32_t my, bool left_click, bool right_click) {
    (void)right_click;
    if (!dlg || !dlg->window) return;
    
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(dlg->window, &wx, &wy, &ww, &wh);
    
    // Calculate areas
    int path_y = wy + FD_PADDING;
    int list_y = path_y + FD_PATH_HEIGHT + FD_PADDING;
    int list_h = wh - FD_PATH_HEIGHT - FD_INPUT_HEIGHT - FD_BUTTON_HEIGHT - 5 * FD_PADDING;
    if (dlg->mode != FILEDIALOG_SAVE) {
        list_h += FD_INPUT_HEIGHT + FD_PADDING;
    }
    
    int btn_y = wy + wh - FD_PADDING - FD_BUTTON_HEIGHT;
    int ok_x = wx + ww - FD_PADDING - FD_BUTTON_WIDTH;
    int cancel_x = ok_x - FD_PADDING - FD_BUTTON_WIDTH;
    
    // Check button hover
    dlg->ok_hovered = (mx >= ok_x && mx < ok_x + FD_BUTTON_WIDTH &&
                       my >= btn_y && my < btn_y + FD_BUTTON_HEIGHT);
    dlg->cancel_hovered = (mx >= cancel_x && mx < cancel_x + FD_BUTTON_WIDTH &&
                           my >= btn_y && my < btn_y + FD_BUTTON_HEIGHT);
    
    if (left_click) {
        // Button clicks
        if (dlg->ok_hovered) {
            dlg->ok_pressed = true;
            // Build result path
            if (dlg->mode == FILEDIALOG_SAVE) {
                if (strlen(dlg->filename) > 0) {
                    strcpy(dlg->result_path, dlg->current_path);
                    if (dlg->result_path[strlen(dlg->result_path) - 1] != '/') {
                        strcat(dlg->result_path, "/");
                    }
                    strcat(dlg->result_path, dlg->filename);
                    dlg->result = FILEDIALOG_RESULT_OK;
                    dlg->running = false;
                }
            } else if (dlg->mode == FILEDIALOG_FOLDER) {
                strcpy(dlg->result_path, dlg->current_path);
                dlg->result = FILEDIALOG_RESULT_OK;
                dlg->running = false;
            } else {
                // Open mode - need a file selected
                kprintf("[FileDialog] Open pressed, selected_index=%d, entry_count=%d\n", dlg->selected_index, dlg->entry_count);
                if (dlg->selected_index >= 0 && dlg->selected_index < dlg->entry_count && 
                    !dlg->entries[dlg->selected_index].is_dir) {
                    strcpy(dlg->result_path, dlg->current_path);
                    if (dlg->result_path[strlen(dlg->result_path) - 1] != '/') {
                        strcat(dlg->result_path, "/");
                    }
                    strcat(dlg->result_path, dlg->entries[dlg->selected_index].name);
                    dlg->result = FILEDIALOG_RESULT_OK;
                    dlg->running = false;
                }
            }
            return;
        }
        
        if (dlg->cancel_hovered) {
            dlg->cancel_pressed = true;
            dlg->result = FILEDIALOG_RESULT_CANCEL;
            dlg->running = false;
            return;
        }
        
        // Filename input focus (Save mode)
        if (dlg->mode == FILEDIALOG_SAVE) {
            int input_y = list_y + list_h + FD_PADDING;
            int input_x = wx + FD_PADDING + 6 * FD_CHAR_W;
            int input_w = ww - 2 * FD_PADDING - 6 * FD_CHAR_W;
            
            if (mx >= input_x && mx < input_x + input_w &&
                my >= input_y && my < input_y + FD_INPUT_HEIGHT) {
                dlg->filename_focused = true;
            } else {
                dlg->filename_focused = false;
            }
        }
        
        // List click
        if (mx >= wx + FD_PADDING && mx < wx + ww - FD_PADDING &&
            my >= list_y && my < list_y + list_h) {
            
            int click_row = (my - list_y) / FD_ENTRY_HEIGHT;
            int idx = click_row + dlg->scroll_offset;
            
            if (idx < dlg->entry_count) {
                // Check for double-click (simplified: if same item clicked again)
                static int last_idx = -1;
                static uint64_t last_click = 0;
                
                if (idx == last_idx && (timer_ticks - last_click) < 500) {
                    // Double click - navigate or select
                    if (dlg->entries[idx].is_dir) {
                        if (strcmp(dlg->entries[idx].name, "..") == 0) {
                            fd_go_up_directory(dlg);
                        } else {
                            // Navigate into directory
                            if (dlg->current_path[strlen(dlg->current_path) - 1] != '/') {
                                strcat(dlg->current_path, "/");
                            }
                            strcat(dlg->current_path, dlg->entries[idx].name);
                            filedialog_refresh(dlg);
                        }
                    } else {
                        // Double-click on file - select and close (Open mode)
                        if (dlg->mode == FILEDIALOG_OPEN) {
                            strcpy(dlg->result_path, dlg->current_path);
                            if (dlg->result_path[strlen(dlg->result_path) - 1] != '/') {
                                strcat(dlg->result_path, "/");
                            }
                            strcat(dlg->result_path, dlg->entries[idx].name);
                            dlg->result = FILEDIALOG_RESULT_OK;
                            dlg->running = false;
                        }
                    }
                    last_idx = -1;
                } else {
                    // Single click - select
                    dlg->selected_index = idx;
                    
                    // For Save mode, populate filename field when clicking a file
                    if (dlg->mode == FILEDIALOG_SAVE && !dlg->entries[idx].is_dir) {
                        strcpy(dlg->filename, dlg->entries[idx].name);
                        dlg->filename_cursor = strlen(dlg->filename);
                    }
                    
                    last_idx = idx;
                    last_click = timer_ticks;
                }
            }
        }
    }
}

// Handle keyboard input
static void fd_handle_key(filedialog_t *dlg, char c, uint32_t keycode) {
    if (!dlg) return;
    
    // Handle filename input (Save mode)
    if (dlg->mode == FILEDIALOG_SAVE && dlg->filename_focused) {
        if (c >= 32 && c < 127) {
            // Printable character
            int len = strlen(dlg->filename);
            if (len < FD_MAX_FILENAME - 1) {
                // Insert at cursor
                for (int i = len; i >= dlg->filename_cursor; i--) {
                    dlg->filename[i + 1] = dlg->filename[i];
                }
                dlg->filename[dlg->filename_cursor] = c;
                dlg->filename_cursor++;
            }
        } else if (c == 8) {  // Backspace
            if (dlg->filename_cursor > 0) {
                dlg->filename_cursor--;
                int len = strlen(dlg->filename);
                for (int i = dlg->filename_cursor; i < len; i++) {
                    dlg->filename[i] = dlg->filename[i + 1];
                }
            }
        } else if (c == 127 || keycode == 0x53) {  // Delete
            int len = strlen(dlg->filename);
            if (dlg->filename_cursor < len) {
                for (int i = dlg->filename_cursor; i < len; i++) {
                    dlg->filename[i] = dlg->filename[i + 1];
                }
            }
        } else if (keycode == 0x4B) {  // Left arrow
            if (dlg->filename_cursor > 0) dlg->filename_cursor--;
        } else if (keycode == 0x4D) {  // Right arrow
            if (dlg->filename_cursor < (int)strlen(dlg->filename)) dlg->filename_cursor++;
        } else if (keycode == 0x47) {  // Home
            dlg->filename_cursor = 0;
        } else if (keycode == 0x4F) {  // End
            dlg->filename_cursor = strlen(dlg->filename);
        }
        return;
    }
    
    // Handle list navigation
    if (keycode == 0x48) {  // Up arrow
        if (dlg->selected_index > 0) {
            dlg->selected_index--;
            if (dlg->selected_index < dlg->scroll_offset) {
                dlg->scroll_offset = dlg->selected_index;
            }
        }
    } else if (keycode == 0x50) {  // Down arrow
        if (dlg->selected_index < dlg->entry_count - 1) {
            dlg->selected_index++;
            if (dlg->selected_index >= dlg->scroll_offset + dlg->visible_entries) {
                dlg->scroll_offset = dlg->selected_index - dlg->visible_entries + 1;
            }
        }
    } else if (c == '\r' || c == '\n') {  // Enter
        if (dlg->selected_index >= 0) {
            if (dlg->entries[dlg->selected_index].is_dir) {
                if (strcmp(dlg->entries[dlg->selected_index].name, "..") == 0) {
                    fd_go_up_directory(dlg);
                } else {
                    if (dlg->current_path[strlen(dlg->current_path) - 1] != '/') {
                        strcat(dlg->current_path, "/");
                    }
                    strcat(dlg->current_path, dlg->entries[dlg->selected_index].name);
                    filedialog_refresh(dlg);
                }
            } else if (dlg->mode == FILEDIALOG_OPEN) {
                strcpy(dlg->result_path, dlg->current_path);
                if (dlg->result_path[strlen(dlg->result_path) - 1] != '/') {
                    strcat(dlg->result_path, "/");
                }
                strcat(dlg->result_path, dlg->entries[dlg->selected_index].name);
                dlg->result = FILEDIALOG_RESULT_OK;
                dlg->running = false;
            }
        }
    } else if (c == 27) {  // Escape
        dlg->result = FILEDIALOG_RESULT_CANCEL;
        dlg->running = false;
    } else if (c == 8 && keycode == 0x0E) {  // Backspace (without filename focus) - go up
        fd_go_up_directory(dlg);
    }
}

// Run dialog and return result
filedialog_result_t filedialog_run(filedialog_t *dlg) {
    if (!dlg) return FILEDIALOG_RESULT_CANCEL;
    
    dlg->running = true;
    dlg->result = FILEDIALOG_RESULT_NONE;
    
    // Initial directory load
    filedialog_refresh(dlg);
    
    // Focus filename input in Save mode
    if (dlg->mode == FILEDIALOG_SAVE) {
        dlg->filename_focused = true;
    }
    
    // Bring dialog to front
    window_bring_to_front(dlg->window);
    window_set_focus(dlg->window);
    
    // Track button state for click detection
    static bool prev_left = false;
    
    // Main loop
    while (dlg->running) {
        // Check if window was closed
        if (!dlg->window || !(dlg->window->flags & WINDOW_FLAG_VISIBLE)) {
            dlg->result = FILEDIALOG_RESULT_CANCEL;
            break;
        }
        
        // Poll mouse state
        int32_t mx, my;
        mouse_get_position(&mx, &my);
        bool left = mouse_button_pressed(0);
        bool right = mouse_button_pressed(1);
        
        bool left_click = left && !prev_left;
        prev_left = left;
        
        // Always process mouse input for modal dialog (don't require focus)
        // Check if click is within dialog bounds
        int32_t wx = dlg->window->bounds.x;
        int32_t wy = dlg->window->bounds.y;
        int32_t ww = dlg->window->bounds.width;
        int32_t wh = dlg->window->bounds.height;

        bool mouse_in_dialog = (mx >= wx && mx < wx + ww && my >= wy && my < wy + wh);

        // Always update hover states (for button highlighting)
        fd_handle_mouse(dlg, mx, my, left_click && mouse_in_dialog, right && mouse_in_dialog);

        // Re-focus dialog if clicked inside
        if (left_click && mouse_in_dialog) {
            window_bring_to_front(dlg->window);
            window_set_focus(dlg->window);
        }

        // Handle keyboard (always - dialog is modal)
        while (keyboard_has_char()) {
            int key = keyboard_get_char();
            char c = (key < 128) ? (char)key : 0;
            uint32_t keycode = (key >= 0x100) ? (key & 0xFF) : 0;
            fd_handle_key(dlg, c, keycode);

            if (!dlg->running) break;
        }
        
        // Render full screen (same sequence as desktop main loop)
        extern void desktop_draw(void);
        extern void wm_draw_apps(void);
        
        // 1. Draw desktop background, dock, menus
        desktop_draw();
        
        // 2. Draw window frames
        wm_draw_all();
        
        // 3. Draw our dialog content (since we're not registered with wm_draw_apps)
        fd_draw(dlg);
        
        // 4. Draw cursor on top
        desktop_draw_cursor(mx, my);
        
        // 5. Swap buffers
        fb_swap_buffers();
        
        // Brief pause to reduce CPU spin
        for (int i = 0; i < 100; i++) {
            __asm__ volatile("pause");
        }
    }
    
    return dlg->result;
}

// ============================================
// Public API implementations
// ============================================

bool filedialog_open(const char *title, const char *initial_path,
                     const char *filter, const char *filter_desc,
                     char *result_path) {
    filedialog_t *dlg = filedialog_create(FILEDIALOG_OPEN, title ? title : "Open File");
    if (!dlg) return false;
    
    if (initial_path) filedialog_set_path(dlg, initial_path);
    if (filter) filedialog_set_filter(dlg, filter, filter_desc);
    
    filedialog_result_t result = filedialog_run(dlg);
    
    if (result == FILEDIALOG_RESULT_OK && result_path) {
        strcpy(result_path, dlg->result_path);
    }
    
    filedialog_destroy(dlg);
    return result == FILEDIALOG_RESULT_OK;
}

bool filedialog_save(const char *title, const char *initial_path,
                     const char *default_name,
                     const char *filter, const char *filter_desc,
                     char *result_path) {
    filedialog_t *dlg = filedialog_create(FILEDIALOG_SAVE, title ? title : "Save File");
    if (!dlg) return false;
    
    if (initial_path) filedialog_set_path(dlg, initial_path);
    if (filter) filedialog_set_filter(dlg, filter, filter_desc);
    if (default_name) filedialog_set_filename(dlg, default_name);
    
    filedialog_result_t result = filedialog_run(dlg);
    
    if (result == FILEDIALOG_RESULT_OK && result_path) {
        strcpy(result_path, dlg->result_path);
    }
    
    filedialog_destroy(dlg);
    return result == FILEDIALOG_RESULT_OK;
}

bool filedialog_select_folder(const char *title, const char *initial_path,
                              char *result_path) {
    filedialog_t *dlg = filedialog_create(FILEDIALOG_FOLDER, title ? title : "Select Folder");
    if (!dlg) return false;
    
    if (initial_path) filedialog_set_path(dlg, initial_path);
    
    filedialog_result_t result = filedialog_run(dlg);
    
    if (result == FILEDIALOG_RESULT_OK && result_path) {
        strcpy(result_path, dlg->result_path);
    }
    
    filedialog_destroy(dlg);
    return result == FILEDIALOG_RESULT_OK;
}
