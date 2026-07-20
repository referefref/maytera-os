// open_with_dialog.h - "Open With" dialog for MayteraOS
// Displays a dialog to choose an application to open a file
#ifndef OPEN_WITH_DIALOG_H
#define OPEN_WITH_DIALOG_H

#include "../types.h"
#include "../gui/window.h"
#include "associations.h"

// Dialog dimensions
#define OPENWITH_WIDTH          350
#define OPENWITH_HEIGHT         400
#define OPENWITH_ITEM_HEIGHT    36
#define OPENWITH_PADDING        10
#define OPENWITH_CHECKBOX_SIZE  16
#define OPENWITH_BUTTON_WIDTH   80
#define OPENWITH_BUTTON_HEIGHT  28

// Dialog colors
#define OPENWITH_BG             0xFFF0F0F0
#define OPENWITH_ITEM_BG        0xFFFFFFFF
#define OPENWITH_ITEM_HOVER     0xFFE0E8FF
#define OPENWITH_ITEM_SELECTED  0xFFCCDDFF
#define OPENWITH_TEXT           0xFF222222
#define OPENWITH_TEXT_DESC      0xFF666666
#define OPENWITH_BORDER         0xFFCCCCCC
#define OPENWITH_BUTTON_BG      0xFFE0E0E0
#define OPENWITH_BUTTON_HOVER   0xFFD0D0FF
#define OPENWITH_BUTTON_TEXT    0xFF222222
#define OPENWITH_CHECKBOX_BG    0xFFFFFFFF
#define OPENWITH_CHECKBOX_CHECK 0xFF0078D7

// Maximum number of apps to display
#define OPENWITH_MAX_APPS       16

// Forward declaration
struct open_with_dialog;

// Dialog structure
typedef struct open_with_dialog {
    window_t *window;               // GUI window
    char filepath[256];             // File to open
    char mime_type[64];             // MIME type of file
    
    // App list
    assoc_app_t *apps[OPENWITH_MAX_APPS];
    int app_count;
    int selected_index;             // Selected app (-1 if none)
    int hover_index;                // Hovered app (-1 if none)
    int scroll_offset;              // Scroll position
    
    // Checkbox state
    bool always_use;                // "Always use this application" checkbox
    bool checkbox_hover;            // Checkbox is hovered
    
    // Button hover states
    bool ok_hover;
    bool cancel_hover;
    bool browse_hover;
    
    // Callback
    open_with_callback_t callback;
    
    // Running state
    bool running;
    
    // Window manager integration
    int app_id;
} open_with_dialog_t;

// ============================================================================
// Dialog API
// ============================================================================

/**
 * Create and show the "Open With" dialog
 * @param filepath Path to the file to open
 * @param callback Function to call when user makes a selection (can be NULL)
 * @return Dialog instance
 */
open_with_dialog_t *open_with_dialog_create(const char *filepath, 
                                             open_with_callback_t callback);

/**
 * Destroy the dialog
 * @param dialog Dialog instance
 */
void open_with_dialog_destroy(open_with_dialog_t *dialog);

/**
 * Run the dialog main loop (blocking until closed)
 * @param dialog Dialog instance
 */
void open_with_dialog_run(open_with_dialog_t *dialog);

/**
 * Draw the dialog
 * @param dialog Dialog instance
 */
void open_with_dialog_draw(open_with_dialog_t *dialog);

// Window manager callbacks
void open_with_dialog_on_event(void *app_data, gui_event_t *event);
void open_with_dialog_on_draw(void *app_data);
void open_with_dialog_on_destroy(void *app_data);

#endif // OPEN_WITH_DIALOG_H
