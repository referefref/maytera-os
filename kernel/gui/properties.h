// properties.h - File Properties Dialog for MayteraOS
// Displays file information and extended attributes
#ifndef PROPERTIES_H
#define PROPERTIES_H

#include "../types.h"
#include "window.h"
#include "../fs/fat.h"
#include "../fs/xattr.h"

// Dialog dimensions
#define PROP_DIALOG_WIDTH       420
#define PROP_DIALOG_HEIGHT      480
#define PROP_TAB_HEIGHT         26
#define PROP_PADDING            12
#define PROP_LINE_HEIGHT        22
#define PROP_LABEL_WIDTH        100
#define PROP_VALUE_WIDTH        280
#define PROP_CHAR_W             8
#define PROP_CHAR_H             16
#define PROP_BTN_WIDTH          75
#define PROP_BTN_HEIGHT         26

// Colors
#define PROP_BG_COLOR           0xFFF0F0F0
#define PROP_TAB_BG             0xFFE0E0E0
#define PROP_TAB_ACTIVE         0xFFFFFFFF
#define PROP_TAB_BORDER         0xFFA0A0A0
#define PROP_TEXT_COLOR         0xFF000000
#define PROP_LABEL_COLOR        0xFF404040
#define PROP_VALUE_COLOR        0xFF000000
#define PROP_BTN_BG             0xFFE0E0E0
#define PROP_BTN_HOVER          0xFFD0D0FF
#define PROP_BTN_BORDER         0xFF808080
#define PROP_INPUT_BG           0xFFFFFFFF
#define PROP_INPUT_BORDER       0xFFA0A0A0
#define PROP_HEADER_COLOR       0xFF0078D7
#define PROP_ICON_BG            0xFFFFFFFF
#define PROP_SEPARATOR_COLOR    0xFFC0C0C0
#define PROP_LIST_SELECTED      0xFF0078D7
#define PROP_LIST_HOVER         0xFFE0E0FF

// Tab indices
#define PROP_TAB_GENERAL        0
#define PROP_TAB_ATTRIBUTES     1
#define PROP_TAB_COUNT          2

// Maximum xattr entries displayed
#define PROP_MAX_XATTRS         32
#define PROP_XATTR_NAME_MAX     64
#define PROP_XATTR_VALUE_MAX    256

// Button indices
#define PROP_BTN_OK             0
#define PROP_BTN_CANCEL         1
#define PROP_BTN_APPLY          2
#define PROP_BTN_ADD_ATTR       3
#define PROP_BTN_EDIT_ATTR      4
#define PROP_BTN_DEL_ATTR       5
#define PROP_BTN_COUNT          6

// Result codes
typedef enum {
    PROP_RESULT_NONE = 0,
    PROP_RESULT_OK,
    PROP_RESULT_CANCEL
} properties_result_t;

// Extended attribute entry for display
typedef struct {
    char name[PROP_XATTR_NAME_MAX];
    char value[PROP_XATTR_VALUE_MAX];
    bool modified;
    bool deleted;
    bool is_new;
} prop_xattr_entry_t;

// Button state
typedef struct {
    int32_t x, y, w, h;
    const char *label;
    bool enabled;
    bool hovered;
    bool pressed;
    bool visible;
} prop_button_t;

// Properties dialog structure
typedef struct {
    window_t *window;
    
    // File information
    char filepath[512];             // Full path to file
    char filename[256];             // Just the filename
    char location[256];             // Parent directory
    uint32_t file_size;             // File size in bytes
    uint16_t mod_date;              // Modification date (FAT format)
    uint16_t mod_time;              // Modification time (FAT format)
    uint8_t attributes;             // FAT attributes
    uint8_t file_type;              // File type (from filebrowser)
    bool is_directory;              // Is this a directory?
    
    // Extended attributes
    prop_xattr_entry_t xattrs[PROP_MAX_XATTRS];
    int xattr_count;
    int xattr_selected;             // Selected attribute index (-1 if none)
    int xattr_scroll;               // Scroll offset for attribute list
    
    // Standard MayteraOS xattrs
    char mime_type[64];             // user.mime_type
    char description[256];          // user.description
    char tags[256];                 // user.tags
    char open_with[256];            // user.open_with
    
    // Dialog state
    int active_tab;
    properties_result_t result;
    bool running;
    bool has_changes;
    
    // UI elements
    prop_button_t buttons[PROP_BTN_COUNT];
    
    // Input field editing
    int editing_field;              // Which field is being edited (-1 if none)
    char edit_buffer[256];          // Edit buffer
    int edit_cursor;                // Cursor position
    
    // Window manager
    int app_id;
} properties_dialog_t;

// Create and show properties dialog
properties_dialog_t *properties_create(const char *filepath);

// Destroy properties dialog
void properties_destroy(properties_dialog_t *dlg);

// Run dialog (blocking until closed)
properties_result_t properties_run(properties_dialog_t *dlg);

// Load file information
void properties_load_file_info(properties_dialog_t *dlg);

// Load extended attributes
void properties_load_xattrs(properties_dialog_t *dlg);

// Save extended attributes (apply changes)
int properties_save_xattrs(properties_dialog_t *dlg);

// Drawing functions
void properties_draw(properties_dialog_t *dlg);
void properties_draw_tabs(properties_dialog_t *dlg);
void properties_draw_general_tab(properties_dialog_t *dlg);
void properties_draw_attributes_tab(properties_dialog_t *dlg);
void properties_draw_buttons(properties_dialog_t *dlg);

// Event handlers
void properties_handle_mouse(properties_dialog_t *dlg, int32_t x, int32_t y, bool left_down, bool left_click);
void properties_handle_key(properties_dialog_t *dlg, char c, uint32_t keycode);

// Window manager callbacks
void properties_on_event(void *app_data, gui_event_t *event);
void properties_on_draw(void *app_data);
void properties_on_destroy(void *app_data);

// Helper functions
const char *properties_format_size(uint32_t size);
const char *properties_format_date(uint16_t date, uint16_t time);
const char *properties_get_type_name(uint8_t file_type, bool is_dir);

#endif // PROPERTIES_H
