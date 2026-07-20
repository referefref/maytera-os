// filedialog.h - Universal File/Folder Dialog for MayteraOS
#ifndef FILEDIALOG_H
#define FILEDIALOG_H

#include "../types.h"
#include "window.h"
#include "../fs/fat.h"

// Dialog modes
typedef enum {
    FILEDIALOG_OPEN,        // Open existing file
    FILEDIALOG_SAVE,        // Save file (allows new filename)
    FILEDIALOG_FOLDER       // Select folder only
} filedialog_mode_t;

// Dialog result
typedef enum {
    FILEDIALOG_RESULT_NONE,     // Dialog still open
    FILEDIALOG_RESULT_OK,       // User clicked OK/Open/Save
    FILEDIALOG_RESULT_CANCEL    // User clicked Cancel or closed
} filedialog_result_t;

// Dialog dimensions
#define FD_WIDTH            450
#define FD_HEIGHT           350
#define FD_PADDING          8
#define FD_ENTRY_HEIGHT     18
#define FD_BUTTON_WIDTH     70
#define FD_BUTTON_HEIGHT    24
#define FD_INPUT_HEIGHT     22
#define FD_PATH_HEIGHT      22
#define FD_CHAR_W           8
#define FD_CHAR_H           16

// Maximum entries and path length
#define FD_MAX_ENTRIES      128
#define FD_MAX_PATH         256
#define FD_MAX_FILENAME     128
#define FD_MAX_FILTER       64

// Colors
#define FD_BG_COLOR         0xFFF0F0F0
#define FD_LIST_BG          0xFFFFFFFF
#define FD_LIST_BORDER      0xFFA0A0A0
#define FD_TEXT_COLOR       0xFF000000
#define FD_DIR_COLOR        0xFF0066CC
#define FD_SELECT_BG        0xFF0078D7
#define FD_SELECT_FG        0xFFFFFFFF
#define FD_INPUT_BG         0xFFFFFFFF
#define FD_INPUT_BORDER     0xFF808080
#define FD_INPUT_FOCUS      0xFF0078D7
#define FD_BUTTON_BG        0xFFE0E0E0
#define FD_BUTTON_HOVER     0xFFD0D0D0
#define FD_BUTTON_PRESS     0xFFC0C0C0
#define FD_BUTTON_TEXT      0xFF000000
#define FD_PATH_BG          0xFFE8E8E8

// File entry for dialog
typedef struct {
    char name[FD_MAX_FILENAME];
    uint32_t size;
    bool is_dir;
} fd_entry_t;

// File dialog structure
typedef struct {
    window_t *window;
    filedialog_mode_t mode;
    filedialog_result_t result;
    
    // Current directory and entries
    char current_path[FD_MAX_PATH];
    fd_entry_t entries[FD_MAX_ENTRIES];
    int entry_count;
    int selected_index;
    int scroll_offset;
    int visible_entries;
    
    // Filename input (for Save mode)
    char filename[FD_MAX_FILENAME];
    int filename_cursor;
    bool filename_focused;
    
    // File filter (e.g., "*.txt" or "*.*")
    char filter[FD_MAX_FILTER];
    const char *filter_description;  // e.g., "Text Files (*.txt)"
    
    // Result path (full path to selected file/folder)
    char result_path[FD_MAX_PATH];
    
    // Dialog title
    const char *title;
    
    // Button labels
    const char *ok_label;       // "Open", "Save", "Select"
    const char *cancel_label;   // "Cancel"
    
    // Internal state
    bool running;
    bool ok_hovered;
    bool cancel_hovered;
    bool ok_pressed;
    bool cancel_pressed;
} filedialog_t;

// ============================================
// Public API
// ============================================

/**
 * Show an Open File dialog
 * @param title         Dialog title (e.g., "Open File")
 * @param initial_path  Starting directory (NULL for root)
 * @param filter        File filter (e.g., "*.txt", "*.*" for all)
 * @param filter_desc   Filter description (e.g., "Text Files")
 * @param result_path   Buffer to store selected path (FD_MAX_PATH size)
 * @return              true if file selected, false if cancelled
 */
bool filedialog_open(const char *title, const char *initial_path,
                     const char *filter, const char *filter_desc,
                     char *result_path);

/**
 * Show a Save File dialog
 * @param title         Dialog title (e.g., "Save File")
 * @param initial_path  Starting directory (NULL for root)
 * @param default_name  Default filename (can be NULL)
 * @param filter        File filter (e.g., "*.txt")
 * @param filter_desc   Filter description
 * @param result_path   Buffer to store selected path (FD_MAX_PATH size)
 * @return              true if path selected, false if cancelled
 */
bool filedialog_save(const char *title, const char *initial_path,
                     const char *default_name,
                     const char *filter, const char *filter_desc,
                     char *result_path);

/**
 * Show a Folder Selection dialog
 * @param title         Dialog title (e.g., "Select Folder")
 * @param initial_path  Starting directory (NULL for root)
 * @param result_path   Buffer to store selected path (FD_MAX_PATH size)
 * @return              true if folder selected, false if cancelled
 */
bool filedialog_select_folder(const char *title, const char *initial_path,
                              char *result_path);

// ============================================
// Internal functions (used by implementation)
// ============================================

filedialog_t *filedialog_create(filedialog_mode_t mode, const char *title);
void filedialog_destroy(filedialog_t *dlg);
void filedialog_set_path(filedialog_t *dlg, const char *path);
void filedialog_set_filter(filedialog_t *dlg, const char *filter, const char *desc);
void filedialog_set_filename(filedialog_t *dlg, const char *filename);
filedialog_result_t filedialog_run(filedialog_t *dlg);
void filedialog_refresh(filedialog_t *dlg);

#endif // FILEDIALOG_H
