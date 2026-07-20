// editor.h - Full-featured Text Editor for MayteraOS
#ifndef EDITOR_H
#define EDITOR_H

#include "../types.h"
#include "window.h"

// Editor dimensions
#define EDITOR_WIDTH        700
#define EDITOR_HEIGHT       500
#define EDITOR_CHAR_W       8   // FONT_WIDTH
#define EDITOR_CHAR_H       16  // FONT_HEIGHT
#define EDITOR_PADDING      4
#define EDITOR_MENU_HEIGHT  20
#define EDITOR_STATUS_HEIGHT 18

// Text buffer limits
#define EDITOR_BUFFER_SIZE  65536   // 64KB max text
#define EDITOR_MAX_LINES    4096    // Maximum number of lines tracked
#define EDITOR_CLIPBOARD_SIZE 4096  // Clipboard buffer size
#define EDITOR_FILENAME_MAX 128     // Max filename length

// Editor colors
#define EDITOR_FG_COLOR       0xFF000000  // Black text
#define EDITOR_BG_COLOR       0xFFFFFFFF  // White background
#define EDITOR_CURSOR_COLOR   0xFF000000  // Black cursor
#define EDITOR_SELECT_BG      0xFF3399FF  // Blue selection background
#define EDITOR_SELECT_FG      0xFFFFFFFF  // White selection text
#define EDITOR_STATUS_BG      0xFFE0E0E0  // Light gray status bar
#define EDITOR_MENU_BG        0xFFF0F0F0  // Menu bar background
#define EDITOR_MENU_HOVER_BG  0xFF3399FF  // Menu hover
#define EDITOR_LINE_NUM_BG    0xFFF5F5F5  // Line number background
#define EDITOR_LINE_NUM_FG    0xFF888888  // Line number text

// Menu item structure
typedef struct {
    const char *label;
    char shortcut;          // Ctrl+? shortcut (0 = none)
    int id;                 // Menu item ID
} editor_menu_item_t;

// Menu structure
typedef struct {
    const char *label;
    editor_menu_item_t *items;
    int item_count;
    int x;                  // Menu position
    int width;
} editor_menu_t;

// Menu item IDs
#define MENU_FILE_NEW       1
#define MENU_FILE_OPEN      2
#define MENU_FILE_SAVE      3
#define MENU_FILE_SAVEAS    4
#define MENU_FILE_EXIT      5
#define MENU_EDIT_UNDO      10
#define MENU_EDIT_CUT       11
#define MENU_EDIT_COPY      12
#define MENU_EDIT_PASTE     13
#define MENU_EDIT_SELECTALL 14
#define MENU_VIEW_WORDWRAP  20
#define MENU_VIEW_LINENUMS  21

// Editor structure
typedef struct {
    window_t *window;

    // Text buffer
    char buffer[EDITOR_BUFFER_SIZE];
    uint32_t buffer_len;            // Current text length

    // Cursor position
    uint32_t cursor_pos;            // Position in buffer
    uint32_t cursor_line;           // Line number (0-indexed)
    uint32_t cursor_col;            // Column number (0-indexed)

    // Selection
    bool has_selection;
    uint32_t sel_start;             // Selection start position
    uint32_t sel_end;               // Selection end position

    // Scrolling
    uint32_t scroll_line;           // First visible line
    uint32_t scroll_col;            // First visible column (horizontal scroll)
    uint32_t visible_lines;         // Number of visible lines
    uint32_t visible_cols;          // Number of visible columns

    // Line tracking (start positions of each line in buffer)
    uint32_t line_starts[EDITOR_MAX_LINES];
    uint32_t line_count;            // Total number of lines

    // Clipboard
    char clipboard[EDITOR_CLIPBOARD_SIZE];
    uint32_t clipboard_len;

    // File info
    char filename[EDITOR_FILENAME_MAX];
    bool has_filename;

    // View options
    bool word_wrap;
    bool show_line_numbers;

    // State
    bool cursor_visible;
    bool modified;                  // Has content been modified?
    bool running;

    // Menu state
    int menu_open;                  // Which menu is open (-1 = none)
    int menu_hover_item;            // Hovered menu item

    // Window manager integration
    int app_id;                     // WM app registration ID
    int dock_index;                 // Dock/taskbar index
} editor_t;

// Create and show an editor window
editor_t *editor_create(void);

// Destroy editor
void editor_destroy(editor_t *ed);

// Run editor main loop (returns when closed)
void editor_run(editor_t *ed);

// Text operations
void editor_insert_char(editor_t *ed, char c);
void editor_insert_text(editor_t *ed, const char *text);
void editor_backspace(editor_t *ed);
void editor_delete(editor_t *ed);
void editor_delete_selection(editor_t *ed);

// Cursor movement
void editor_cursor_left(editor_t *ed);
void editor_cursor_right(editor_t *ed);
void editor_cursor_up(editor_t *ed);
void editor_cursor_down(editor_t *ed);
void editor_cursor_home(editor_t *ed);
void editor_cursor_end(editor_t *ed);
void editor_page_up(editor_t *ed);
void editor_page_down(editor_t *ed);
void editor_cursor_doc_start(editor_t *ed);
void editor_cursor_doc_end(editor_t *ed);

// Selection operations
void editor_select_all(editor_t *ed);
void editor_clear_selection(editor_t *ed);

// Clipboard operations
void editor_cut(editor_t *ed);
void editor_copy(editor_t *ed);
void editor_paste(editor_t *ed);

// File operations
void editor_new(editor_t *ed);
void editor_open(editor_t *ed, const char *filename);
void editor_save(editor_t *ed);
void editor_save_as(editor_t *ed, const char *filename);

// Clear editor content
void editor_clear(editor_t *ed);

// Set editor content
void editor_set_text(editor_t *ed, const char *text);

// Get editor content (returns internal buffer pointer)
const char *editor_get_text(editor_t *ed);

// Draw editor content
void editor_draw(editor_t *ed);

// Launch callback for dock (non-blocking)
void editor_launch(void);

// Window manager callbacks
void editor_on_event(void *app_data, gui_event_t *event);
void editor_on_draw(void *app_data);
void editor_on_destroy(void *app_data);

#endif // EDITOR_H
