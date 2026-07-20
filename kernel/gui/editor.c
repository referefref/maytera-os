// editor.c - Full-featured Text Editor for MayteraOS
#include "editor.h"
#include "filedialog.h"
#include "window.h"
#include "desktop.h"
#include "icons.h"
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

// Global editor for launch callback
static editor_t *g_active_editor = NULL;

// External filesystem
extern fat_fs_t g_fat_fs;

// Menu definitions
static editor_menu_item_t g_file_menu_items[] = {
    {"New",      'N', MENU_FILE_NEW},
    {"Open...",  'O', MENU_FILE_OPEN},
    {"Save",     'S', MENU_FILE_SAVE},
    {"Save As...", 0, MENU_FILE_SAVEAS},
    {NULL,        0, 0},  // Separator
    {"Exit",      0, MENU_FILE_EXIT},
    {NULL,       -1, 0}   // Terminator (shortcut -1 = end)
};

static editor_menu_item_t g_edit_menu_items[] = {
    {"Cut",       'X', MENU_EDIT_CUT},
    {"Copy",      'C', MENU_EDIT_COPY},
    {"Paste",     'V', MENU_EDIT_PASTE},
    {NULL,         0, 0},  // Separator
    {"Select All", 'A', MENU_EDIT_SELECTALL},
    {NULL,        -1, 0}   // Terminator
};

static editor_menu_item_t g_view_menu_items[] = {
    {"Word Wrap",    'W', MENU_VIEW_WORDWRAP},
    {"Line Numbers",  0, MENU_VIEW_LINENUMS},
    {NULL,           -1, 0}   // Terminator
};

static editor_menu_t g_menus[] = {
    {"File", g_file_menu_items, 6, 0, 50},
    {"Edit", g_edit_menu_items, 5, 50, 50},
    {"View", g_view_menu_items, 2, 100, 80},
    {NULL, NULL, 0, 0, 0}
};

// Line number gutter width
#define LINE_NUM_WIDTH  40

// =============================================================================
// Internal helpers
// =============================================================================

// Recalculate line start positions from buffer content
static void editor_recalc_lines(editor_t *ed) {
    if (!ed) return;

    ed->line_count = 1;
    ed->line_starts[0] = 0;

    for (uint32_t i = 0; i < ed->buffer_len && ed->line_count < EDITOR_MAX_LINES; i++) {
        if (ed->buffer[i] == '\n') {
            ed->line_starts[ed->line_count++] = i + 1;
        }
    }
}

// Update cursor line and column from cursor position
static void editor_update_cursor_pos(editor_t *ed) {
    if (!ed) return;

    ed->cursor_line = 0;
    ed->cursor_col = 0;

    for (uint32_t i = 0; i < ed->cursor_pos && i < ed->buffer_len; i++) {
        if (ed->buffer[i] == '\n') {
            ed->cursor_line++;
            ed->cursor_col = 0;
        } else {
            ed->cursor_col++;
        }
    }
}

// Get the start position of a line in the buffer
static uint32_t editor_get_line_start(editor_t *ed, uint32_t line) {
    if (!ed || line >= ed->line_count) return ed->buffer_len;
    return ed->line_starts[line];
}

// Get the length of a line (excluding newline)
static uint32_t editor_get_line_length(editor_t *ed, uint32_t line) {
    if (!ed || line >= ed->line_count) return 0;

    uint32_t start = ed->line_starts[line];
    uint32_t end;

    if (line + 1 < ed->line_count) {
        end = ed->line_starts[line + 1] - 1;  // Exclude newline
    } else {
        end = ed->buffer_len;
    }

    return end > start ? end - start : 0;
}

// Ensure cursor line is visible (adjust scroll if needed)
static void editor_ensure_visible(editor_t *ed) {
    if (!ed) return;

    // Scroll up if cursor is above visible area
    if (ed->cursor_line < ed->scroll_line) {
        ed->scroll_line = ed->cursor_line;
    }

    // Scroll down if cursor is below visible area
    if (ed->cursor_line >= ed->scroll_line + ed->visible_lines) {
        ed->scroll_line = ed->cursor_line - ed->visible_lines + 1;
    }

    // Horizontal scroll
    if (ed->cursor_col < ed->scroll_col) {
        ed->scroll_col = ed->cursor_col;
    }
    if (ed->cursor_col >= ed->scroll_col + ed->visible_cols) {
        ed->scroll_col = ed->cursor_col - ed->visible_cols + 1;
    }
}

// Check if position is in selection
static bool editor_pos_in_selection(editor_t *ed, uint32_t pos) {
    if (!ed || !ed->has_selection) return false;
    uint32_t start = ed->sel_start < ed->sel_end ? ed->sel_start : ed->sel_end;
    uint32_t end = ed->sel_start > ed->sel_end ? ed->sel_start : ed->sel_end;
    return pos >= start && pos < end;
}

// Get normalized selection bounds
static void editor_get_selection_bounds(editor_t *ed, uint32_t *start, uint32_t *end) {
    if (!ed || !ed->has_selection) {
        *start = *end = 0;
        return;
    }
    *start = ed->sel_start < ed->sel_end ? ed->sel_start : ed->sel_end;
    *end = ed->sel_start > ed->sel_end ? ed->sel_start : ed->sel_end;
}

// =============================================================================
// Drawing functions
// =============================================================================

// Draw a single character at pixel position
static void editor_draw_char_at(int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg) {
    // Draw background
    fb_fill_rect(x, y, EDITOR_CHAR_W, EDITOR_CHAR_H, bg & 0xFFFFFF);

    // Draw character
    if (c >= ' ' && c < 127) {
        const uint8_t *glyph = font_get_glyph(c);
        if (glyph) {
            for (int r = 0; r < FONT_HEIGHT && r < EDITOR_CHAR_H; r++) {
                uint8_t bits = glyph[r];
                for (int col_bit = 0; col_bit < FONT_WIDTH; col_bit++) {
                    if (bits & (0x80 >> col_bit)) {
                        fb_put_pixel(x + col_bit, y + r, fg & 0xFFFFFF);
                    }
                }
            }
        }
    }
}

// Draw a string at pixel position
static void editor_draw_string_at(int32_t x, int32_t y, const char *str, uint32_t fg, uint32_t bg) {
    while (*str) {
        editor_draw_char_at(x, y, *str, fg, bg);
        x += EDITOR_CHAR_W;
        str++;
    }
}

// Draw menu bar
static void editor_draw_menu_bar(editor_t *ed) {
    if (!ed || !ed->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(ed->window, &wx, &wy, &ww, &wh);

    // Menu bar background
    fb_fill_rect(wx, wy, ww, EDITOR_MENU_HEIGHT, EDITOR_MENU_BG & 0xFFFFFF);
    fb_fill_rect(wx, wy + EDITOR_MENU_HEIGHT - 1, ww, 1, 0xCCCCCC);  // Bottom border

    // Draw each menu header
    int menu_x = wx + 4;
    for (int i = 0; g_menus[i].label; i++) {
        uint32_t bg = EDITOR_MENU_BG;
        uint32_t fg = 0xFF000000;

        if (ed->menu_open == i) {
            bg = EDITOR_MENU_HOVER_BG;
            fg = 0xFFFFFFFF;
        }

        int label_width = strlen(g_menus[i].label) * EDITOR_CHAR_W + 8;
        fb_fill_rect(menu_x, wy, label_width, EDITOR_MENU_HEIGHT - 1, bg & 0xFFFFFF);
        editor_draw_string_at(menu_x + 4, wy + 3, g_menus[i].label, fg, bg);

        g_menus[i].x = menu_x;
        g_menus[i].width = label_width;
        menu_x += label_width;
    }
}

// Draw open menu dropdown
static void editor_draw_menu_dropdown(editor_t *ed) {
    if (!ed || ed->menu_open < 0) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(ed->window, &wx, &wy, &ww, &wh);
    (void)ww; (void)wh;

    editor_menu_t *menu = &g_menus[ed->menu_open];
    if (!menu->items) return;

    // Count items
    int count = 0;
    for (int i = 0; menu->items[i].shortcut != -1; i++) {
        count++;
    }

    int dropdown_x = menu->x;  // Already absolute
    int dropdown_y = wy + EDITOR_MENU_HEIGHT;
    int dropdown_w = 150;
    int dropdown_h = count * EDITOR_CHAR_H + 4;

    // Dropdown background with shadow
    fb_fill_rect(dropdown_x + 2, dropdown_y + 2, dropdown_w, dropdown_h, 0x888888);  // Shadow
    fb_fill_rect(dropdown_x, dropdown_y, dropdown_w, dropdown_h, 0xFFFFFF);
    fb_draw_rect(dropdown_x, dropdown_y, dropdown_w, dropdown_h, 0x808080);

    // Draw items
    int item_y = dropdown_y + 2;
    for (int i = 0; menu->items[i].shortcut != -1; i++) {
        if (menu->items[i].label == NULL) {
            // Separator
            fb_fill_rect(dropdown_x + 4, item_y + EDITOR_CHAR_H / 2, dropdown_w - 8, 1, 0xCCCCCC);
        } else {
            uint32_t bg = 0xFFFFFFFF;
            uint32_t fg = 0xFF000000;

            if (i == ed->menu_hover_item) {
                bg = EDITOR_MENU_HOVER_BG;
                fg = 0xFFFFFFFF;
            }

            // Check marks for toggle items
            char prefix[4] = "   ";
            if (menu->items[i].id == MENU_VIEW_WORDWRAP && ed->word_wrap) {
                prefix[0] = '*';
            }
            if (menu->items[i].id == MENU_VIEW_LINENUMS && ed->show_line_numbers) {
                prefix[0] = '*';
            }

            fb_fill_rect(dropdown_x + 2, item_y, dropdown_w - 4, EDITOR_CHAR_H, bg & 0xFFFFFF);
            editor_draw_string_at(dropdown_x + 4, item_y + 1, prefix, fg, bg);
            editor_draw_string_at(dropdown_x + 4 + 3 * EDITOR_CHAR_W, item_y + 1, menu->items[i].label, fg, bg);

            // Draw shortcut
            if (menu->items[i].shortcut && menu->items[i].shortcut != -1) {
                char shortcut_str[8] = "Ctrl+?";
                shortcut_str[5] = menu->items[i].shortcut;
                int shortcut_x = dropdown_x + dropdown_w - 6 * EDITOR_CHAR_W - 4;
                editor_draw_string_at(shortcut_x, item_y + 1, shortcut_str, 0xFF888888, bg);
            }
        }
        item_y += EDITOR_CHAR_H;
    }
}

// Draw entire editor content
static void editor_redraw(editor_t *ed) {
    if (!ed || !ed->window) return;

    // Get window content area
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(ed->window, &wx, &wy, &ww, &wh);

    // Draw menu bar
    editor_draw_menu_bar(ed);

    // Calculate edit area (below menu bar, above status bar)
    int edit_y = wy + EDITOR_MENU_HEIGHT;
    int edit_h = wh - EDITOR_MENU_HEIGHT - EDITOR_STATUS_HEIGHT;
    int gutter_w = ed->show_line_numbers ? LINE_NUM_WIDTH : 0;
    int edit_x = wx + gutter_w;
    int edit_w = ww - gutter_w;

    // Fill edit area background
    fb_fill_rect(wx, edit_y, ww, edit_h, EDITOR_BG_COLOR & 0xFFFFFF);

    // Draw line number gutter
    if (ed->show_line_numbers) {
        fb_fill_rect(wx, edit_y, gutter_w, edit_h, EDITOR_LINE_NUM_BG & 0xFFFFFF);
        fb_fill_rect(wx + gutter_w - 1, edit_y, 1, edit_h, 0xCCCCCC);  // Gutter border
    }

    // Calculate visible area
    int content_width = edit_w - 2 * EDITOR_PADDING;
    int content_height = edit_h - 2 * EDITOR_PADDING;
    ed->visible_cols = content_width / EDITOR_CHAR_W;
    ed->visible_lines = content_height / EDITOR_CHAR_H;

    // In word wrap mode, disable horizontal scrolling
    if (ed->word_wrap) {
        ed->scroll_col = 0;
    }

    // Variables for cursor position in visual coordinates
    int cursor_visual_row = -1;
    int cursor_visual_col = -1;

    if (ed->word_wrap) {
        // Word wrap mode: iterate through buffer and wrap at visible_cols
        uint32_t visual_row = 0;
        uint32_t visual_col = 0;
        uint32_t buf_line = 0;  // Current buffer line number for line numbers
        uint32_t skip_rows = ed->scroll_line;  // Rows to skip (scrolling)
        
        for (uint32_t i = 0; i < ed->buffer_len && visual_row < skip_rows + ed->visible_lines; i++) {
            char c = ed->buffer[i];
            
            // Track cursor visual position
            if (i == ed->cursor_pos) {
                cursor_visual_row = visual_row - skip_rows;
                cursor_visual_col = visual_col;
            }
            
            if (c == '\n') {
                // End of line - move to next visual row
                buf_line++;
                visual_row++;
                visual_col = 0;
                
                // Draw line number at start of new line
                if (visual_row >= skip_rows && visual_row < skip_rows + ed->visible_lines && ed->show_line_numbers) {
                    char line_num[8];
                    itoa(buf_line + 1, line_num, 10);
                    int num_x = wx + gutter_w - 8 - strlen(line_num) * EDITOR_CHAR_W;
                    int num_y = edit_y + EDITOR_PADDING + (visual_row - skip_rows) * EDITOR_CHAR_H;
                    editor_draw_string_at(num_x, num_y, line_num, EDITOR_LINE_NUM_FG, EDITOR_LINE_NUM_BG);
                }
            } else {
                // Draw line number at first char of first line
                if (visual_row == 0 && visual_col == 0 && ed->show_line_numbers) {
                    char line_num[8];
                    itoa(buf_line + 1, line_num, 10);
                    int num_x = wx + gutter_w - 8 - strlen(line_num) * EDITOR_CHAR_W;
                    int num_y = edit_y + EDITOR_PADDING;
                    editor_draw_string_at(num_x, num_y, line_num, EDITOR_LINE_NUM_FG, EDITOR_LINE_NUM_BG);
                }
                
                // Draw character if in visible area
                if (visual_row >= skip_rows && visual_row < skip_rows + ed->visible_lines) {
                    uint32_t fg = EDITOR_FG_COLOR;
                    uint32_t bg = EDITOR_BG_COLOR;
                    
                    if (editor_pos_in_selection(ed, i)) {
                        fg = EDITOR_SELECT_FG;
                        bg = EDITOR_SELECT_BG;
                    }
                    
                    int char_x = edit_x + EDITOR_PADDING + visual_col * EDITOR_CHAR_W;
                    int char_y = edit_y + EDITOR_PADDING + (visual_row - skip_rows) * EDITOR_CHAR_H;
                    editor_draw_char_at(char_x, char_y, c, fg, bg);
                }
                
                visual_col++;
                
                // Wrap to next row if at edge
                if (visual_col >= ed->visible_cols) {
                    visual_row++;
                    visual_col = 0;
                }
            }
        }
        
        // Handle cursor at end of buffer
        if (ed->cursor_pos == ed->buffer_len) {
            cursor_visual_row = visual_row - skip_rows;
            cursor_visual_col = visual_col;
        }
        
        // Draw cursor in word wrap mode
        if (ed->cursor_visible && cursor_visual_row >= 0 && 
            cursor_visual_row < (int)ed->visible_lines &&
            cursor_visual_col >= 0 && cursor_visual_col < (int)ed->visible_cols) {
            int32_t cx = edit_x + EDITOR_PADDING + cursor_visual_col * EDITOR_CHAR_W;
            int32_t cy = edit_y + EDITOR_PADDING + cursor_visual_row * EDITOR_CHAR_H;
            fb_fill_rect(cx, cy, 2, EDITOR_CHAR_H, EDITOR_CURSOR_COLOR & 0xFFFFFF);
        }
    } else {
        // Non-wrap mode: original line-based drawing
        for (uint32_t row = 0; row < ed->visible_lines; row++) {
            uint32_t line = ed->scroll_line + row;
            if (line >= ed->line_count) break;

            // Draw line number
            if (ed->show_line_numbers) {
                char line_num[8];
                itoa(line + 1, line_num, 10);
                int num_x = wx + gutter_w - 8 - strlen(line_num) * EDITOR_CHAR_W;
                int num_y = edit_y + EDITOR_PADDING + row * EDITOR_CHAR_H;
                editor_draw_string_at(num_x, num_y, line_num, EDITOR_LINE_NUM_FG, EDITOR_LINE_NUM_BG);
            }

            uint32_t line_start = editor_get_line_start(ed, line);
            uint32_t line_len = editor_get_line_length(ed, line);

            for (uint32_t col = 0; col < ed->visible_cols; col++) {
                uint32_t buf_col = ed->scroll_col + col;
                uint32_t buf_pos = line_start + buf_col;

                char c = ' ';
                if (buf_col < line_len) {
                    c = ed->buffer[buf_pos];
                }

                uint32_t fg = EDITOR_FG_COLOR;
                uint32_t bg = EDITOR_BG_COLOR;

                // Selection highlighting
                if (editor_pos_in_selection(ed, buf_pos) && buf_col < line_len) {
                    fg = EDITOR_SELECT_FG;
                    bg = EDITOR_SELECT_BG;
                }

                if (c != '\0' && c != '\n') {
                    int char_x = edit_x + EDITOR_PADDING + col * EDITOR_CHAR_W;
                    int char_y = edit_y + EDITOR_PADDING + row * EDITOR_CHAR_H;
                    editor_draw_char_at(char_x, char_y, c, fg, bg);
                }
            }
        }

        // Draw cursor in non-wrap mode
        if (ed->cursor_visible && ed->cursor_line >= ed->scroll_line &&
            ed->cursor_line < ed->scroll_line + ed->visible_lines &&
            ed->cursor_col >= ed->scroll_col &&
            ed->cursor_col < ed->scroll_col + ed->visible_cols) {

            uint32_t cursor_row = ed->cursor_line - ed->scroll_line;
            uint32_t cursor_col = ed->cursor_col - ed->scroll_col;
            int32_t cx = edit_x + EDITOR_PADDING + cursor_col * EDITOR_CHAR_W;
            int32_t cy = edit_y + EDITOR_PADDING + cursor_row * EDITOR_CHAR_H;

            // Draw a vertical bar cursor
            fb_fill_rect(cx, cy, 2, EDITOR_CHAR_H, EDITOR_CURSOR_COLOR & 0xFFFFFF);
        }
    }

    // Draw status bar at bottom
    int status_y = wy + wh - EDITOR_STATUS_HEIGHT;
    fb_fill_rect(wx, status_y, ww, EDITOR_STATUS_HEIGHT, EDITOR_STATUS_BG & 0xFFFFFF);
    fb_fill_rect(wx, status_y, ww, 1, 0xCCCCCC);  // Top border

    // Status bar: filename | Line X, Col Y | Modified
    char status[128];
    char num_buf[16];

    // Filename or "Untitled"
    if (ed->has_filename) {
        strcpy(status, ed->filename);
    } else {
        strcpy(status, "Untitled");
    }

    // Position info
    strcat(status, "  |  Ln ");
    itoa(ed->cursor_line + 1, num_buf, 10);
    strcat(status, num_buf);
    strcat(status, ", Col ");
    itoa(ed->cursor_col + 1, num_buf, 10);
    strcat(status, num_buf);

    // Modified indicator
    if (ed->modified) {
        strcat(status, "  |  Modified");
    }

    // Word wrap indicator
    if (ed->word_wrap) {
        strcat(status, "  |  Wrap");
    }

    editor_draw_string_at(wx + 4, status_y + 3, status, 0xFF000000, EDITOR_STATUS_BG);

    // Draw menu dropdown if open
    if (ed->menu_open >= 0) {
        editor_draw_menu_dropdown(ed);
    }
}

// =============================================================================
// Selection operations
// =============================================================================

void editor_select_all(editor_t *ed) {
    if (!ed) return;
    ed->has_selection = true;
    ed->sel_start = 0;
    ed->sel_end = ed->buffer_len;
    ed->cursor_pos = ed->buffer_len;
    editor_update_cursor_pos(ed);
}

void editor_clear_selection(editor_t *ed) {
    if (!ed) return;
    ed->has_selection = false;
    ed->sel_start = 0;
    ed->sel_end = 0;
}

void editor_delete_selection(editor_t *ed) {
    if (!ed || !ed->has_selection) return;

    uint32_t start, end;
    editor_get_selection_bounds(ed, &start, &end);
    uint32_t del_len = end - start;

    // Shift buffer left
    for (uint32_t i = start; i < ed->buffer_len - del_len; i++) {
        ed->buffer[i] = ed->buffer[i + del_len];
    }
    ed->buffer_len -= del_len;
    ed->buffer[ed->buffer_len] = '\0';

    // Move cursor to selection start
    ed->cursor_pos = start;
    ed->has_selection = false;
    ed->modified = true;

    editor_recalc_lines(ed);
    editor_update_cursor_pos(ed);
    editor_ensure_visible(ed);
}

// =============================================================================
// Clipboard operations
// =============================================================================

void editor_cut(editor_t *ed) {
    if (!ed || !ed->has_selection) return;

    editor_copy(ed);
    editor_delete_selection(ed);
}

void editor_copy(editor_t *ed) {
    if (!ed || !ed->has_selection) return;

    uint32_t start, end;
    editor_get_selection_bounds(ed, &start, &end);
    uint32_t copy_len = end - start;

    if (copy_len >= EDITOR_CLIPBOARD_SIZE) {
        copy_len = EDITOR_CLIPBOARD_SIZE - 1;
    }

    memcpy(ed->clipboard, &ed->buffer[start], copy_len);
    ed->clipboard[copy_len] = '\0';
    ed->clipboard_len = copy_len;

    kprintf("[Editor] Copied %u chars to clipboard\n", copy_len);
}

void editor_paste(editor_t *ed) {
    if (!ed || ed->clipboard_len == 0) return;

    // Delete selection first if any
    if (ed->has_selection) {
        editor_delete_selection(ed);
    }

    // Check if there's room
    if (ed->buffer_len + ed->clipboard_len >= EDITOR_BUFFER_SIZE) {
        kprintf("[Editor] Buffer full, cannot paste\n");
        return;
    }

    // Make room for pasted text
    for (uint32_t i = ed->buffer_len; i > ed->cursor_pos; i--) {
        ed->buffer[i + ed->clipboard_len - 1] = ed->buffer[i - 1];
    }

    // Insert clipboard content
    memcpy(&ed->buffer[ed->cursor_pos], ed->clipboard, ed->clipboard_len);
    ed->buffer_len += ed->clipboard_len;
    ed->cursor_pos += ed->clipboard_len;
    ed->buffer[ed->buffer_len] = '\0';

    ed->modified = true;
    editor_recalc_lines(ed);
    editor_update_cursor_pos(ed);
    editor_ensure_visible(ed);

    kprintf("[Editor] Pasted %u chars\n", ed->clipboard_len);
}

// =============================================================================
// File operations
// =============================================================================

void editor_new(editor_t *ed) {
    if (!ed) return;

    editor_clear(ed);
    ed->has_filename = false;
    ed->filename[0] = '\0';
    ed->modified = false;

    kprintf("[Editor] New file\n");
}

void editor_open(editor_t *ed, const char *filename) {
    if (!ed || !filename) return;

    // Try to read file from FAT filesystem
    if (!g_fat_fs.mounted) {
        kprintf("[Editor] No filesystem mounted\n");
        return;
    }

    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, filename, &size);

    if (!data) {
        kprintf("[Editor] Could not open file: %s\n", filename);
        return;
    }

    // Load into buffer
    editor_clear(ed);
    if (size >= EDITOR_BUFFER_SIZE) {
        size = EDITOR_BUFFER_SIZE - 1;
    }
    memcpy(ed->buffer, data, size);
    ed->buffer[size] = '\0';
    ed->buffer_len = size;

    kfree(data);

    // Set filename
    strncpy(ed->filename, filename, EDITOR_FILENAME_MAX - 1);
    ed->filename[EDITOR_FILENAME_MAX - 1] = '\0';
    ed->has_filename = true;
    ed->modified = false;

    editor_recalc_lines(ed);
    editor_update_cursor_pos(ed);

    kprintf("[Editor] Opened: %s (%u bytes)\n", filename, size);
}

void editor_save(editor_t *ed) {
    if (!ed) return;

    if (!ed->has_filename) {
        kprintf("[Editor] No filename - use Save As\n");
        // TODO: Show save dialog
        return;
    }

    editor_save_as(ed, ed->filename);
}

void editor_save_as(editor_t *ed, const char *filename) {
    if (!ed || !filename) return;

    // FAT write not implemented yet
    kprintf("[Editor] Save not implemented yet: %s\n", filename);
    // TODO: Implement fat_write

    // Mark as not modified (even though save failed for now)
    strncpy(ed->filename, filename, EDITOR_FILENAME_MAX - 1);
    ed->filename[EDITOR_FILENAME_MAX - 1] = '\0';
    ed->has_filename = true;
    // ed->modified = false;  // Don't clear until actually saved
}

// =============================================================================
// Editor API implementation
// =============================================================================

editor_t *editor_create(void) {
    editor_t *ed = (editor_t *)kmalloc(sizeof(editor_t));
    if (!ed) {
        kprintf("[Editor] Failed to allocate editor\n");
        return NULL;
    }

    memset(ed, 0, sizeof(editor_t));

    // Center on screen
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();
    int x = (screen_w - EDITOR_WIDTH) / 2;
    int y = (screen_h - EDITOR_HEIGHT) / 2 - 50;

    // Create window
    ed->window = window_create("Editor - Untitled", x, y, EDITOR_WIDTH, EDITOR_HEIGHT);
    if (!ed->window) {
        kprintf("[Editor] Failed to create window\n");
        kfree(ed);
        return NULL;
    }

    // Set window colors
    ed->window->bg_color = EDITOR_BG_COLOR & 0xFFFFFF;

    // Initialize state
    ed->buffer_len = 0;
    ed->cursor_pos = 0;
    ed->cursor_line = 0;
    ed->cursor_col = 0;
    ed->scroll_line = 0;
    ed->scroll_col = 0;
    ed->line_count = 1;
    ed->line_starts[0] = 0;
    ed->cursor_visible = true;
    ed->modified = false;
    ed->running = true;

    ed->has_selection = false;
    ed->sel_start = 0;
    ed->sel_end = 0;

    ed->clipboard_len = 0;
    ed->has_filename = false;
    ed->filename[0] = '\0';

    ed->word_wrap = false;
    ed->show_line_numbers = true;

    ed->menu_open = -1;
    ed->menu_hover_item = -1;

    // Calculate visible area
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(ed->window, &wx, &wy, &ww, &wh);
    int gutter_w = ed->show_line_numbers ? LINE_NUM_WIDTH : 0;
    int content_width = ww - gutter_w - 2 * EDITOR_PADDING;
    int content_height = wh - EDITOR_MENU_HEIGHT - EDITOR_STATUS_HEIGHT - 2 * EDITOR_PADDING;
    ed->visible_cols = content_width / EDITOR_CHAR_W;
    ed->visible_lines = content_height / EDITOR_CHAR_H;

    kprintf("[Editor] Created with %d cols x %d lines visible\n",
            ed->visible_cols, ed->visible_lines);

    return ed;
}

void editor_destroy(editor_t *ed) {
    if (!ed) return;
    if (ed->window) {
        window_destroy(ed->window);
    }
    kfree(ed);
}

void editor_insert_char(editor_t *ed, char c) {
    if (!ed || ed->buffer_len >= EDITOR_BUFFER_SIZE - 1) return;

    // Delete selection first if any
    if (ed->has_selection) {
        editor_delete_selection(ed);
    }

    // Make room for new character
    for (uint32_t i = ed->buffer_len; i > ed->cursor_pos; i--) {
        ed->buffer[i] = ed->buffer[i - 1];
    }

    // Insert character
    ed->buffer[ed->cursor_pos] = c;
    ed->buffer_len++;
    ed->cursor_pos++;
    ed->buffer[ed->buffer_len] = '\0';

    ed->modified = true;

    editor_recalc_lines(ed);
    editor_update_cursor_pos(ed);
    editor_ensure_visible(ed);
}

void editor_insert_text(editor_t *ed, const char *text) {
    if (!ed || !text) return;
    while (*text) {
        editor_insert_char(ed, *text++);
    }
}

void editor_backspace(editor_t *ed) {
    if (!ed) return;

    if (ed->has_selection) {
        editor_delete_selection(ed);
        return;
    }

    if (ed->cursor_pos == 0) return;

    // Shift characters left
    ed->cursor_pos--;
    for (uint32_t i = ed->cursor_pos; i < ed->buffer_len - 1; i++) {
        ed->buffer[i] = ed->buffer[i + 1];
    }
    ed->buffer_len--;
    ed->buffer[ed->buffer_len] = '\0';

    ed->modified = true;

    editor_recalc_lines(ed);
    editor_update_cursor_pos(ed);
    editor_ensure_visible(ed);
}

void editor_delete(editor_t *ed) {
    if (!ed) return;

    if (ed->has_selection) {
        editor_delete_selection(ed);
        return;
    }

    if (ed->cursor_pos >= ed->buffer_len) return;

    // Shift characters left
    for (uint32_t i = ed->cursor_pos; i < ed->buffer_len - 1; i++) {
        ed->buffer[i] = ed->buffer[i + 1];
    }
    ed->buffer_len--;
    ed->buffer[ed->buffer_len] = '\0';

    ed->modified = true;

    editor_recalc_lines(ed);
}

void editor_cursor_left(editor_t *ed) {
    if (!ed || ed->cursor_pos == 0) return;
    editor_clear_selection(ed);
    ed->cursor_pos--;
    editor_update_cursor_pos(ed);
    editor_ensure_visible(ed);
}

void editor_cursor_right(editor_t *ed) {
    if (!ed || ed->cursor_pos >= ed->buffer_len) return;
    editor_clear_selection(ed);
    ed->cursor_pos++;
    editor_update_cursor_pos(ed);
    editor_ensure_visible(ed);
}

void editor_cursor_up(editor_t *ed) {
    if (!ed || ed->cursor_line == 0) return;
    editor_clear_selection(ed);

    uint32_t target_line = ed->cursor_line - 1;
    uint32_t target_start = editor_get_line_start(ed, target_line);
    uint32_t target_len = editor_get_line_length(ed, target_line);

    uint32_t target_col = ed->cursor_col;
    if (target_col > target_len) {
        target_col = target_len;
    }

    ed->cursor_pos = target_start + target_col;
    editor_update_cursor_pos(ed);
    editor_ensure_visible(ed);
}

void editor_cursor_down(editor_t *ed) {
    if (!ed || ed->cursor_line >= ed->line_count - 1) return;
    editor_clear_selection(ed);

    uint32_t target_line = ed->cursor_line + 1;
    uint32_t target_start = editor_get_line_start(ed, target_line);
    uint32_t target_len = editor_get_line_length(ed, target_line);

    uint32_t target_col = ed->cursor_col;
    if (target_col > target_len) {
        target_col = target_len;
    }

    ed->cursor_pos = target_start + target_col;
    editor_update_cursor_pos(ed);
    editor_ensure_visible(ed);
}

void editor_cursor_home(editor_t *ed) {
    if (!ed) return;
    editor_clear_selection(ed);
    ed->cursor_pos = editor_get_line_start(ed, ed->cursor_line);
    editor_update_cursor_pos(ed);
    editor_ensure_visible(ed);
}

void editor_cursor_end(editor_t *ed) {
    if (!ed) return;
    editor_clear_selection(ed);
    uint32_t line_start = editor_get_line_start(ed, ed->cursor_line);
    uint32_t line_len = editor_get_line_length(ed, ed->cursor_line);
    ed->cursor_pos = line_start + line_len;
    editor_update_cursor_pos(ed);
    editor_ensure_visible(ed);
}

void editor_page_up(editor_t *ed) {
    if (!ed) return;
    editor_clear_selection(ed);

    uint32_t page = ed->visible_lines > 1 ? ed->visible_lines - 1 : 1;
    if (ed->cursor_line > page) {
        ed->cursor_line -= page;
    } else {
        ed->cursor_line = 0;
    }

    uint32_t target_start = editor_get_line_start(ed, ed->cursor_line);
    uint32_t target_len = editor_get_line_length(ed, ed->cursor_line);
    if (ed->cursor_col > target_len) {
        ed->cursor_col = target_len;
    }
    ed->cursor_pos = target_start + ed->cursor_col;

    if (ed->scroll_line > page) {
        ed->scroll_line -= page;
    } else {
        ed->scroll_line = 0;
    }

    editor_ensure_visible(ed);
}

void editor_page_down(editor_t *ed) {
    if (!ed) return;
    editor_clear_selection(ed);

    uint32_t page = ed->visible_lines > 1 ? ed->visible_lines - 1 : 1;
    ed->cursor_line += page;
    if (ed->cursor_line >= ed->line_count) {
        ed->cursor_line = ed->line_count - 1;
    }

    uint32_t target_start = editor_get_line_start(ed, ed->cursor_line);
    uint32_t target_len = editor_get_line_length(ed, ed->cursor_line);
    if (ed->cursor_col > target_len) {
        ed->cursor_col = target_len;
    }
    ed->cursor_pos = target_start + ed->cursor_col;

    ed->scroll_line += page;
    if (ed->scroll_line + ed->visible_lines > ed->line_count) {
        ed->scroll_line = ed->line_count > ed->visible_lines ? ed->line_count - ed->visible_lines : 0;
    }

    editor_ensure_visible(ed);
}

void editor_cursor_doc_start(editor_t *ed) {
    if (!ed) return;
    editor_clear_selection(ed);
    ed->cursor_pos = 0;
    ed->scroll_line = 0;
    ed->scroll_col = 0;
    editor_update_cursor_pos(ed);
}

void editor_cursor_doc_end(editor_t *ed) {
    if (!ed) return;
    editor_clear_selection(ed);
    ed->cursor_pos = ed->buffer_len;
    editor_update_cursor_pos(ed);
    editor_ensure_visible(ed);
}

void editor_clear(editor_t *ed) {
    if (!ed) return;
    memset(ed->buffer, 0, EDITOR_BUFFER_SIZE);
    ed->buffer_len = 0;
    ed->cursor_pos = 0;
    ed->cursor_line = 0;
    ed->cursor_col = 0;
    ed->scroll_line = 0;
    ed->scroll_col = 0;
    ed->line_count = 1;
    ed->line_starts[0] = 0;
    ed->has_selection = false;
    ed->modified = false;
}

void editor_set_text(editor_t *ed, const char *text) {
    if (!ed || !text) return;

    editor_clear(ed);

    size_t len = strlen(text);
    if (len >= EDITOR_BUFFER_SIZE) {
        len = EDITOR_BUFFER_SIZE - 1;
    }

    memcpy(ed->buffer, text, len);
    ed->buffer[len] = '\0';
    ed->buffer_len = len;

    editor_recalc_lines(ed);
    ed->modified = false;
}

const char *editor_get_text(editor_t *ed) {
    if (!ed) return NULL;
    return ed->buffer;
}

void editor_run(editor_t *ed) {
    // This is the old blocking run loop - not used in WM mode
    (void)ed;
}

void editor_draw(editor_t *ed) {
    if (!ed || !ed->window) return;
    window_draw(ed->window);
    editor_redraw(ed);
}

// =============================================================================
// Menu handling
// =============================================================================

static void editor_handle_menu_action(editor_t *ed, int action_id) {
    if (!ed) return;

    switch (action_id) {
        case MENU_FILE_NEW:
            editor_new(ed);
            window_set_title(ed->window, "Editor - Untitled");
            break;
        case MENU_FILE_OPEN:
            {
                char filepath[FD_MAX_PATH];
                if (filedialog_open("Open File", "/", "*.txt", "Text Files", filepath)) {
                    editor_open(ed, filepath);
                }
            }
            break;
        case MENU_FILE_SAVE:
            editor_save(ed);
            break;
        case MENU_FILE_SAVEAS:
            {
                char filepath[FD_MAX_PATH];
                const char *default_name = ed->has_filename ? ed->filename : "untitled.txt";
                if (filedialog_save("Save As", "/", default_name, "*.*", "All Files", filepath)) {
                    editor_save_as(ed, filepath);
                }
            }
            break;
        case MENU_FILE_EXIT:
            kprintf("[Editor] Exit from menu\n");
            wm_unregister_app(ed->app_id);
            if (ed->dock_index >= 0) {
                dock_remove_app(ed->dock_index);
            }
            if (g_active_editor == ed) {
                g_active_editor = NULL;
            }
            window_hide(ed->window);
            wm_invalidate_all();
            break;
        case MENU_EDIT_CUT:
            editor_cut(ed);
            break;
        case MENU_EDIT_COPY:
            editor_copy(ed);
            break;
        case MENU_EDIT_PASTE:
            editor_paste(ed);
            break;
        case MENU_EDIT_SELECTALL:
            editor_select_all(ed);
            break;
        case MENU_VIEW_WORDWRAP:
            ed->word_wrap = !ed->word_wrap;
            kprintf("[Editor] Word wrap: %s\n", ed->word_wrap ? "ON" : "OFF");
            break;
        case MENU_VIEW_LINENUMS:
            ed->show_line_numbers = !ed->show_line_numbers;
            kprintf("[Editor] Line numbers: %s\n", ed->show_line_numbers ? "ON" : "OFF");
            break;
    }

    ed->menu_open = -1;
    wm_invalidate_rect(&ed->window->bounds);
}

// =============================================================================
// Window Manager Callback Functions (non-blocking model)
// =============================================================================

void editor_on_event(void *app_data, gui_event_t *event) {
    editor_t *ed = (editor_t *)app_data;
    if (!ed || !ed->window || !event) return;

    switch (event->type) {
        case EVENT_KEY_DOWN:
            {
                int c = event->key_char;
                uint32_t keycode = event->keycode;

                // Close menu on any key if open
                if (ed->menu_open >= 0 && c == 27) {
                    ed->menu_open = -1;
                    wm_invalidate_rect(&ed->window->bounds);
                    return;
                }

                // Handle Ctrl+key shortcuts
                if (c >= 1 && c <= 26) {
                    char ctrl_key = 'A' + (c - 1);
                    switch (ctrl_key) {
                        case 'N':
                            editor_handle_menu_action(ed, MENU_FILE_NEW);
                            return;
                        case 'O':
                            editor_handle_menu_action(ed, MENU_FILE_OPEN);
                            return;
                        case 'S':
                            editor_handle_menu_action(ed, MENU_FILE_SAVE);
                            return;
                        case 'X':
                            editor_cut(ed);
                            wm_invalidate_rect(&ed->window->bounds);
                            return;
                        case 'C':
                            editor_copy(ed);
                            return;
                        case 'V':
                            editor_paste(ed);
                            wm_invalidate_rect(&ed->window->bounds);
                            return;
                        case 'A':
                            editor_select_all(ed);
                            wm_invalidate_rect(&ed->window->bounds);
                            return;
                        case 'W':
                            ed->word_wrap = !ed->word_wrap;
                            wm_invalidate_rect(&ed->window->bounds);
                            return;
                    }
                }

                if (c == 27) {  // ESC - close editor
                    kprintf("[Editor] ESC pressed, closing\n");
                    wm_unregister_app(ed->app_id);
                    if (ed->dock_index >= 0) {
                        dock_remove_app(ed->dock_index);
                    }
                    if (g_active_editor == ed) {
                        g_active_editor = NULL;
                    }
                    window_hide(ed->window);
                    wm_invalidate_all();
                    return;
                }

                // Handle control characters
                if (c == '\b' || c == 127) {  // Backspace
                    editor_backspace(ed);
                } else if (c == '\n' || c == '\r') {
                    editor_insert_char(ed, '\n');
                } else if (c == '\t') {
                    // Insert 4 spaces for tab
                    editor_insert_text(ed, "    ");
                } else if (c >= ' ' && c < 127) {
                    editor_insert_char(ed, c);
                }

                // Arrow keys (using our custom key codes)
                if (keycode == KEY_LEFT || keycode == 0x4B) editor_cursor_left(ed);
                if (keycode == KEY_RIGHT || keycode == 0x4D) editor_cursor_right(ed);
                if (keycode == KEY_UP || keycode == 0x48) editor_cursor_up(ed);
                if (keycode == KEY_DOWN || keycode == 0x50) editor_cursor_down(ed);

                // Home/End
                if (keycode == 0x47) editor_cursor_home(ed);  // Home
                if (keycode == 0x4F) editor_cursor_end(ed);   // End

                // Page Up/Down
                if (keycode == 0x49) editor_page_up(ed);
                if (keycode == 0x51) editor_page_down(ed);

                wm_invalidate_rect(&ed->window->bounds);
            }
            break;

        case EVENT_MOUSE_DOWN:
            {
                int32_t mx = event->mouse_x;
                int32_t my = event->mouse_y;

                // Check if click is on menu bar
                int32_t wx, wy, ww, wh;
                window_get_content_bounds(ed->window, &wx, &wy, &ww, &wh);
                (void)ww; (void)wh;

                if (my >= wy && my < wy + EDITOR_MENU_HEIGHT) {
                    // Check which menu was clicked
                    for (int i = 0; g_menus[i].label; i++) {
                        int menu_x = g_menus[i].x;  // Already absolute from drawing
                        int menu_w = g_menus[i].width;
                        if (mx >= menu_x && mx < menu_x + menu_w) {
                            if (ed->menu_open == i) {
                                ed->menu_open = -1;  // Close if already open
                            } else {
                                ed->menu_open = i;
                                ed->menu_hover_item = -1;
                            }
                            wm_invalidate_rect(&ed->window->bounds);
                            return;
                        }
                    }
                }

                // Check if click is in menu dropdown
                if (ed->menu_open >= 0) {
                    editor_menu_t *menu = &g_menus[ed->menu_open];
                    int dropdown_x = menu->x;  // Already absolute
                    int dropdown_y = wy + EDITOR_MENU_HEIGHT;
                    int dropdown_w = 150;
                    int count = 0;
                    for (int i = 0; menu->items[i].shortcut != -1; i++) count++;
                    int dropdown_h = count * EDITOR_CHAR_H + 4;

                    if (mx >= dropdown_x && mx < dropdown_x + dropdown_w &&
                        my >= dropdown_y && my < dropdown_y + dropdown_h) {
                        // Find which item was clicked
                        int item_idx = (my - dropdown_y - 2) / EDITOR_CHAR_H;
                        if (item_idx >= 0 && item_idx < count && menu->items[item_idx].label) {
                            editor_handle_menu_action(ed, menu->items[item_idx].id);
                        }
                        return;
                    }

                    // Click outside menu - close it
                    ed->menu_open = -1;
                    wm_invalidate_rect(&ed->window->bounds);
                }

                // Click in editor area - position cursor
                int gutter_w = ed->show_line_numbers ? LINE_NUM_WIDTH : 0;
                int edit_x = wx + gutter_w;
                int edit_y = wy + EDITOR_MENU_HEIGHT;

                if (mx >= edit_x + EDITOR_PADDING && my >= edit_y + EDITOR_PADDING) {
                    int click_col = (mx - edit_x - EDITOR_PADDING) / EDITOR_CHAR_W;
                    int click_row = (my - edit_y - EDITOR_PADDING) / EDITOR_CHAR_H;

                    uint32_t target_line = ed->scroll_line + click_row;
                    if (target_line >= ed->line_count) {
                        target_line = ed->line_count - 1;
                    }

                    uint32_t target_col = ed->scroll_col + click_col;
                    uint32_t line_len = editor_get_line_length(ed, target_line);
                    if (target_col > line_len) {
                        target_col = line_len;
                    }

                    uint32_t target_start = editor_get_line_start(ed, target_line);
                    ed->cursor_pos = target_start + target_col;
                    editor_clear_selection(ed);
                    editor_update_cursor_pos(ed);
                    wm_invalidate_rect(&ed->window->bounds);
                }
            }
            break;

        case EVENT_MOUSE_MOVE:
            // Handle menu hover
            if (ed->menu_open >= 0) {
                int32_t my = event->mouse_y;

                int32_t wx, wy, ww, wh;
                window_get_content_bounds(ed->window, &wx, &wy, &ww, &wh);
                (void)wx; (void)ww; (void)wh;

                editor_menu_t *menu = &g_menus[ed->menu_open];
                int dropdown_y = wy + EDITOR_MENU_HEIGHT;
                int count = 0;
                for (int i = 0; menu->items[i].shortcut != -1; i++) count++;

                int old_hover = ed->menu_hover_item;
                int item_idx = (my - dropdown_y - 2) / EDITOR_CHAR_H;
                if (item_idx >= 0 && item_idx < count && menu->items[item_idx].label) {
                    ed->menu_hover_item = item_idx;
                } else {
                    ed->menu_hover_item = -1;
                }

                if (old_hover != ed->menu_hover_item) {
                    wm_invalidate_rect(&ed->window->bounds);
                }
            }
            break;

        case EVENT_WINDOW_CLOSE:
            kprintf("[Editor] Close button clicked\n");
            wm_unregister_app(ed->app_id);
            if (ed->dock_index >= 0) {
                dock_remove_app(ed->dock_index);
            }
            if (g_active_editor == ed) {
                g_active_editor = NULL;
            }
            window_hide(ed->window);
            wm_invalidate_all();
            break;

        default:
            break;
    }
}

void editor_on_draw(void *app_data) {
    editor_t *ed = (editor_t *)app_data;
    if (ed) {
        editor_draw(ed);
    }
}

void editor_on_destroy(void *app_data) {
    editor_t *ed = (editor_t *)app_data;
    if (ed) {
        kprintf("[Editor] Destroying editor instance\n");
        if (g_active_editor == ed) {
            g_active_editor = NULL;
        }
        editor_destroy(ed);
    }
}

// Launch callback for dock (non-blocking)
void editor_launch(void) {
    LOG_INFO("[Editor] Application launched");
    kprintf("[Editor] Launching editor (non-blocking)...\n");

    editor_t *ed = editor_create();
    if (!ed) {
        LOG_ERROR("[Editor] Failed to create window");
        kprintf("[Editor] Failed to create editor\n");
        return;
    }

    // Initialize WM integration fields
    ed->app_id = -1;
    ed->dock_index = -1;

    // Add to taskbar
    ed->dock_index = dock_add_app_with_icon("Editor", 0xFFE67E22, ICON_HIGHLIGHT, NULL);

    // Register with window manager
    ed->app_id = wm_register_app(
        ed->window,
        ed,
        editor_on_event,
        editor_on_draw,
        editor_on_destroy
    );

    if (ed->app_id < 0) {
        kprintf("[Editor] Failed to register with window manager\n");
        if (ed->dock_index >= 0) {
            dock_remove_app(ed->dock_index);
        }
        editor_destroy(ed);
        return;
    }

    g_active_editor = ed;
    wm_invalidate_all();

    kprintf("[Editor] Editor registered as app %d\n", ed->app_id);
}
