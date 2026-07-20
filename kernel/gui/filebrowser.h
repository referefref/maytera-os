// filebrowser.h - GUI File Browser application for MayteraOS
#ifndef FILEBROWSER_H
#define FILEBROWSER_H

#include "../types.h"
#include "window.h"
#include "../fs/fat.h"

// File browser dimensions
#define FB_WINDOW_WIDTH     600
#define FB_WINDOW_HEIGHT    450
#define FB_PATH_BAR_HEIGHT  24
#define FB_ENTRY_HEIGHT     20
#define FB_CHAR_W           8
#define FB_CHAR_H           16

// Toolbar dimensions
#define FB_TOOLBAR_HEIGHT   28
#define FB_TOOLBAR_BTN_W    24
#define FB_TOOLBAR_BTN_H    22
#define FB_TOOLBAR_PADDING  4
#define FB_TOOLBAR_SEP_W    8

// Status bar dimensions
#define FB_STATUSBAR_HEIGHT 20

// Breadcrumb dimensions
#define FB_BREADCRUMB_HEIGHT 22
#define FB_BREADCRUMB_SEP_W  12

// Context menu dimensions
#define FB_MENU_ITEM_HEIGHT 20
#define FB_MENU_ITEM_PADDING 8
#define FB_MENU_MIN_WIDTH   120

// Preview pane dimensions
#define FB_PREVIEW_WIDTH    200
#define FB_PREVIEW_PADDING  8
#define FB_PREVIEW_THUMB_MAX 180   // Max thumbnail size (scaled to fit)
#define FB_PREVIEW_TEXT_LINES 10   // Max lines of text preview
#define FB_PREVIEW_MAX_FILE_SIZE (64 * 1024)  // Max file size to preview (64KB)

// Maximum entries that can be displayed
#define FB_MAX_ENTRIES      256
#define FB_MAX_PATH         256
#define FB_NAME_MAX         128
#define FB_MAX_TOOLBAR_BTNS 12
#define FB_MAX_MENU_ITEMS   12
#define FB_MAX_BREADCRUMBS  16

// Colors
#define FB_BG_COLOR         0xFFFFFFFF  // White background
#define FB_TEXT_COLOR       0xFF000000  // Black text
#define FB_DIR_COLOR        0xFF0000FF  // Blue for directories
#define FB_PATH_BG_COLOR    0xFFE0E0E0  // Light gray for path bar
#define FB_SELECTED_BG      0xFF0078D7  // Blue selection
#define FB_SELECTED_FG      0xFFFFFFFF  // White text when selected
#define FB_SCROLL_BG        0xFFC0C0C0  // Scrollbar background
#define FB_SCROLL_FG        0xFF808080  // Scrollbar foreground
#define FB_PREVIEW_BG       0xFFF0F0F0  // Preview pane background
#define FB_PREVIEW_BORDER   0xFFCCCCCC  // Preview pane border
#define FB_TOOLBAR_BG       0xFFF0F0F0  // Toolbar background
#define FB_TOOLBAR_BTN_BG   0xFFE0E0E0  // Toolbar button background
#define FB_TOOLBAR_BTN_HOVER 0xFFD0D0FF // Toolbar button hover
#define FB_TOOLBAR_BTN_PRESS 0xFFA0A0FF // Toolbar button pressed
#define FB_STATUSBAR_BG     0xFFF0F0F0  // Status bar background
#define FB_STATUSBAR_TEXT   0xFF404040  // Status bar text
#define FB_MENU_BG          0xFFFFFFFF  // Menu background
#define FB_MENU_BORDER      0xFF808080  // Menu border
#define FB_MENU_HOVER       0xFF0078D7  // Menu item hover
#define FB_MENU_DISABLED    0xFFA0A0A0  // Disabled menu item text
#define FB_BREADCRUMB_SEP   0xFF808080  // Breadcrumb separator color
#define FB_BREADCRUMB_HOVER 0xFFD0D0FF  // Breadcrumb hover color

// Toolbar icon IDs
#define FB_ICON_BACK        0
#define FB_ICON_FORWARD     1
#define FB_ICON_UP          2
#define FB_ICON_REFRESH     3
#define FB_ICON_NEWFOLDER   4
#define FB_ICON_DELETE      5
#define FB_ICON_VIEW        6
#define FB_ICON_SEPARATOR   255

// File type constants
#define FB_TYPE_FOLDER      0
#define FB_TYPE_FOLDER_OPEN 1
#define FB_TYPE_TEXT        2
#define FB_TYPE_IMAGE       3
#define FB_TYPE_EXECUTABLE  4
#define FB_TYPE_DOCUMENT    5
#define FB_TYPE_AUDIO       6
#define FB_TYPE_ARCHIVE     7
#define FB_TYPE_VIDEO       8
#define FB_TYPE_UNKNOWN     9

// Icon size for file browser (16x16 pixels)
#define FB_ICON_SIZE        16
#define FB_ICON_LARGE       32

// Forward declaration
struct filebrowser;

// Toolbar button structure
typedef struct {
    int32_t x, y, w, h;
    const char *tooltip;
    uint8_t icon_id;
    void (*action)(struct filebrowser *fb);
    bool enabled;
    bool hovered;
    bool pressed;
} fb_toolbar_btn_t;

// Context menu item structure
typedef struct {
    const char *label;
    void (*action)(struct filebrowser *fb);
    bool enabled;
    bool separator_after;
} fb_menu_item_t;

// Breadcrumb segment structure
typedef struct {
    char name[64];
    char path[FB_MAX_PATH];
    int32_t x, w;
} fb_breadcrumb_t;

// View modes
typedef enum {
    FB_VIEW_LIST,
    FB_VIEW_DETAILS,
    FB_VIEW_ICONS,
    FB_VIEW_THUMBNAILS
} fb_view_mode_t;

// Sort columns for details view
typedef enum {
    FB_SORT_NAME,
    FB_SORT_SIZE,
    FB_SORT_TYPE,
    FB_SORT_DATE
} fb_sort_column_t;

// Details view column widths
#define FB_COL_NAME_WIDTH   200
#define FB_COL_SIZE_WIDTH   80
#define FB_COL_TYPE_WIDTH   80
#define FB_COL_DATE_WIDTH   120
#define FB_HEADER_HEIGHT    20

// Icon view dimensions (64x64 icon area + name below)
#define FB_ICON_VIEW_SIZE   64
#define FB_ICON_CELL_W      80
#define FB_ICON_CELL_H      90
#define FB_ICON_TEXT_H      20

// Thumbnail view dimensions (96x96 for thumbnail area)
#define FB_THUMB_SIZE       96
#define FB_THUMB_CELL_W     110
#define FB_THUMB_CELL_H     130
#define FB_THUMB_TEXT_H     24

// Header colors
#define FB_HEADER_BG        0xFFF0F0F0
#define FB_HEADER_BORDER    0xFFD0D0D0
#define FB_ICON_BG          0xFFE8E8E8

// Extension max length
#define FB_EXT_MAX          8

// File entry structure
typedef struct {
    char name[FB_NAME_MAX];
    char extension[FB_EXT_MAX];     // File extension (jpg, txt, etc.)
    uint32_t size;
    uint8_t attr;
    uint8_t file_type;              // FB_TYPE_FOLDER, FB_TYPE_TEXT, FB_TYPE_IMAGE, etc.
    bool is_dir;
    uint16_t mod_date;              // FAT date format
    uint16_t mod_time;              // FAT time format
} fb_entry_t;

// Clipboard operation types
typedef enum {
    CLIPBOARD_NONE = 0,
    CLIPBOARD_COPY,
    CLIPBOARD_CUT
} clipboard_op_t;

// Clipboard structure for copy/cut/paste operations
typedef struct {
    char path[FB_MAX_PATH];           // Full path of source file/folder
    char name[FB_NAME_MAX];           // Name of the file/folder
    bool is_dir;                      // Is it a directory?
    clipboard_op_t operation;         // Copy or cut?
} clipboard_entry_t;

// Preview type enumeration
typedef enum {
    PREVIEW_NONE = 0,
    PREVIEW_IMAGE,
    PREVIEW_TEXT,
    PREVIEW_DIRECTORY
} preview_type_t;

// Preview data structure
typedef struct {
    preview_type_t type;
    uint8_t *data;              // Raw file data (for image) or text content
    uint32_t data_size;         // Size of the data
    uint32_t img_width;         // Original image width (if image)
    uint32_t img_height;        // Original image height (if image)
    uint32_t *pixels;           // Decoded pixel data (if image)
    char text_lines[FB_PREVIEW_TEXT_LINES][64];  // Text preview lines
    int text_line_count;        // Number of text lines loaded
} preview_data_t;

// File browser structure
typedef struct filebrowser {
    window_t *window;
    char current_path[FB_MAX_PATH];
    fb_entry_t entries[FB_MAX_ENTRIES];
    int entry_count;
    int selected_index;
    int scroll_offset;
    int visible_entries;
    bool running;

    // Clipboard for copy/cut/paste operations
    clipboard_entry_t clipboard;

    // Preview pane state
    bool show_preview;          // Is preview pane visible?
    preview_data_t preview;     // Cached preview data
    int preview_entry_index;    // Index of entry being previewed (-1 if none)

    // Toolbar state
    fb_toolbar_btn_t toolbar_btns[FB_MAX_TOOLBAR_BTNS];
    int toolbar_btn_count;

    // Breadcrumb navigation
    fb_breadcrumb_t breadcrumbs[FB_MAX_BREADCRUMBS];
    int breadcrumb_count;
    int breadcrumb_hover;       // Index of hovered breadcrumb (-1 if none)

    // Context menu state
    fb_menu_item_t menu_items[FB_MAX_MENU_ITEMS];
    int menu_item_count;
    bool menu_visible;
    int32_t menu_x, menu_y;
    int32_t menu_width, menu_height;
    int menu_hover_index;

    // View mode
    fb_view_mode_t view_mode;
    fb_sort_column_t sort_column;       // Current sort column
    bool sort_ascending;                // Sort direction
    int items_per_row;                  // For icon/thumbnail views

    // Keyboard modifiers
    bool ctrl_held;
    bool shift_held;

    // Window manager integration
    int app_id;                 // WM app registration ID
    int dock_index;             // Dock/taskbar index
} filebrowser_t;

// Create and show a file browser window
filebrowser_t *filebrowser_create(void);

// Destroy file browser
void filebrowser_destroy(filebrowser_t *fb);

// Run file browser main loop (returns when closed)
void filebrowser_run(filebrowser_t *fb);

// Navigate to a directory
int filebrowser_navigate(filebrowser_t *fb, const char *path);

// Refresh current directory listing
void filebrowser_refresh(filebrowser_t *fb);

// Draw file browser content
void filebrowser_draw(filebrowser_t *fb);

// Launch callback for dock (non-blocking)
void filebrowser_launch(void);

// Window manager callbacks
void filebrowser_on_event(void *app_data, gui_event_t *event);
void filebrowser_on_draw(void *app_data);
void filebrowser_on_destroy(void *app_data);

// ============================================
// File Operations
// ============================================

int filebrowser_create_folder(filebrowser_t *fb, const char *name);
int filebrowser_create_file(filebrowser_t *fb, const char *name);
int filebrowser_delete_selected(filebrowser_t *fb);
int filebrowser_rename_selected(filebrowser_t *fb, const char *new_name);
int filebrowser_copy_selected(filebrowser_t *fb);
int filebrowser_cut_selected(filebrowser_t *fb);
int filebrowser_paste(filebrowser_t *fb);
bool filebrowser_has_clipboard(filebrowser_t *fb);
fb_entry_t *filebrowser_get_selected(filebrowser_t *fb);

// ============================================
// File Type Icons
// ============================================

uint8_t fb_get_file_type(const char *filename);
void fb_draw_file_icon(int32_t x, int32_t y, uint8_t file_type, bool is_selected);
void fb_draw_file_icon_large(int32_t x, int32_t y, uint8_t file_type, bool is_selected);

// ============================================
// Preview Pane Functions
// ============================================

void fb_toggle_preview(filebrowser_t *fb);
void fb_load_preview(filebrowser_t *fb, fb_entry_t *entry);
void fb_clear_preview(filebrowser_t *fb);
void fb_draw_preview_pane(filebrowser_t *fb);
bool fb_is_image_file(const char *filename);
bool fb_is_text_file(const char *filename);

// ============================================
// Toolbar Functions
// ============================================

void fb_init_toolbar(filebrowser_t *fb);
void fb_draw_toolbar(filebrowser_t *fb);
bool fb_handle_toolbar_click(filebrowser_t *fb, int32_t x, int32_t y);
void fb_update_toolbar_hover(filebrowser_t *fb, int32_t x, int32_t y);

// ============================================
// Breadcrumb Navigation Functions
// ============================================

void fb_update_breadcrumbs(filebrowser_t *fb);
void fb_draw_breadcrumbs(filebrowser_t *fb);
bool fb_handle_breadcrumb_click(filebrowser_t *fb, int32_t x, int32_t y);
void fb_update_breadcrumb_hover(filebrowser_t *fb, int32_t x, int32_t y);

// ============================================
// Status Bar Functions
// ============================================

void fb_draw_statusbar(filebrowser_t *fb);

// ============================================
// Context Menu Functions
// ============================================

void fb_init_context_menu(filebrowser_t *fb);
void fb_show_context_menu(filebrowser_t *fb, int32_t x, int32_t y);
void fb_hide_context_menu(filebrowser_t *fb);
void fb_draw_context_menu(filebrowser_t *fb);
bool fb_handle_context_menu_click(filebrowser_t *fb, int32_t x, int32_t y);
void fb_update_context_menu_hover(filebrowser_t *fb, int32_t x, int32_t y);

// ============================================
// Keyboard Shortcut Functions
// ============================================

bool fb_handle_keyboard(filebrowser_t *fb, uint32_t keycode, char c);

// ============================================
// View Mode Functions
// ============================================

/**
 * Set the view mode for the file browser
 * @param fb    File browser instance
 * @param mode  View mode to set
 */
void filebrowser_set_view_mode(filebrowser_t *fb, fb_view_mode_t mode);

/**
 * Get the current view mode
 * @param fb  File browser instance
 * @return    Current view mode
 */
fb_view_mode_t filebrowser_get_view_mode(filebrowser_t *fb);

/**
 * Cycle to the next view mode
 * @param fb  File browser instance
 */
void filebrowser_cycle_view_mode(filebrowser_t *fb);

/**
 * Set the sort column for details view
 * @param fb      File browser instance
 * @param column  Column to sort by
 */
void filebrowser_set_sort_column(filebrowser_t *fb, fb_sort_column_t column);

/**
 * Toggle sort direction (ascending/descending)
 * @param fb  File browser instance
 */
void filebrowser_toggle_sort_direction(filebrowser_t *fb);

/**
 * Get file type string for display
 * @param file_type  File type constant
 * @return           String description of file type
 */
const char *fb_get_type_string(uint8_t file_type);

/**
 * Extract file extension from filename
 * @param filename   The filename to analyze
 * @param ext_buf    Buffer to store extension (should be FB_EXT_MAX size)
 */
void fb_extract_extension(const char *filename, char *ext_buf);

/**
 * Format FAT date/time for display
 * @param date    FAT date value
 * @param time    FAT time value
 * @param buf     Buffer to store formatted string (at least 20 chars)
 */
void fb_format_datetime(uint16_t date, uint16_t time, char *buf);

/**
 * Draw list view (simple name-only list)
 */
void fb_draw_list_view(filebrowser_t *fb, int32_t x, int32_t y, int32_t w, int32_t h);

/**
 * Draw details view (multi-column with sortable headers)
 */
void fb_draw_details_view(filebrowser_t *fb, int32_t x, int32_t y, int32_t w, int32_t h);

/**
 * Draw icon view (grid of icons with names)
 */
void fb_draw_icon_view(filebrowser_t *fb, int32_t x, int32_t y, int32_t w, int32_t h);

/**
 * Draw thumbnail view (larger grid for image previews)
 */
void fb_draw_thumbnail_view(filebrowser_t *fb, int32_t x, int32_t y, int32_t w, int32_t h);

/**
 * Draw column headers for details view
 */
void fb_draw_column_headers(filebrowser_t *fb, int32_t x, int32_t y, int32_t w);

/**
 * Get entry index at a given screen point (view-mode aware)
 */
int fb_get_entry_at_point(filebrowser_t *fb, int32_t x, int32_t y);

#endif // FILEBROWSER_H
