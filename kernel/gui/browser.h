// browser.h - Web Browser GUI application for MayteraOS
#ifndef BROWSER_H
#define BROWSER_H

#include "../types.h"
#include "window.h"

// Browser window dimensions
#define BROWSER_WIDTH       800
#define BROWSER_HEIGHT      600
#define BROWSER_MIN_WIDTH   400
#define BROWSER_MIN_HEIGHT  300

// Toolbar dimensions
#define BROWSER_TOOLBAR_HEIGHT  36
#define BROWSER_STATUSBAR_HEIGHT 20
#define BROWSER_BUTTON_SIZE     28
#define BROWSER_URLBAR_HEIGHT   24

// Navigation buttons
#define BROWSER_BTN_BACK        0
#define BROWSER_BTN_FORWARD     1
#define BROWSER_BTN_RELOAD      2
#define BROWSER_BTN_HOME        3
#define BROWSER_BTN_GO          4
#define BROWSER_BTN_COUNT       5

// Maximum sizes
#define BROWSER_MAX_URL         2048
#define BROWSER_MAX_TITLE       256
#define BROWSER_MAX_STATUS      256
#define BROWSER_MAX_HISTORY     64

// Browser state
typedef enum {
    BROWSER_STATE_IDLE,
    BROWSER_STATE_LOADING,
    BROWSER_STATE_COMPLETE,
    BROWSER_STATE_ERROR
} browser_state_t;

// Content type
typedef enum {
    CONTENT_TYPE_HTML,
    CONTENT_TYPE_TEXT,
    CONTENT_TYPE_IMAGE,
    CONTENT_TYPE_UNKNOWN
} content_type_t;

// Simple text line for rendering
typedef struct {
    char *text;
    int x, y;
    uint32_t color;
    int bold;
    int link;           // Is this a link
    char link_url[512]; // URL if it's a link
} text_line_t;

// Browser structure
typedef struct {
    window_t *window;

    // Navigation state
    char url[BROWSER_MAX_URL];
    char title[BROWSER_MAX_TITLE];
    char *history[BROWSER_MAX_HISTORY];
    int history_pos;
    int history_count;

    // Content
    uint8_t *page_data;
    uint32_t page_len;
    content_type_t content_type;

    // Rendered content
    text_line_t *lines;
    int line_count;
    int line_capacity;

    // View state
    int scroll_x, scroll_y;
    int content_width;
    int content_height;
    int layout_width;   // #89: window content width the page was last wrapped to (-1 = none)
    browser_state_t state;

    // URL bar
    char address_text[BROWSER_MAX_URL];
    int address_cursor;
    int address_focused;
    int address_selection_start;
    int address_selection_end;

    // Status bar
    char status[BROWSER_MAX_STATUS];

    // Button hover state
    int hover_button;

    // Window manager integration
    int app_id;
    int dock_index;

    // Key repeat filter (suppress PS/2 typematic duplicates)
    uint32_t last_key_char;
    uint64_t last_key_tick;

    // Flags
    int running;
} browser_t;

// Toolbar button definition
typedef struct {
    int x, y, w, h;
    const char *icon;   // Icon character or NULL
    const char *tooltip;
    int enabled;
} browser_button_t;

// Create browser window
browser_t *browser_create(void);

// Destroy browser
void browser_destroy(browser_t *browser);

// Navigate to URL
void browser_navigate(browser_t *browser, const char *url);

// Navigation functions
void browser_back(browser_t *browser);
void browser_forward(browser_t *browser);
void browser_reload(browser_t *browser);
void browser_stop(browser_t *browser);
void browser_home(browser_t *browser);

// Draw browser content
void browser_draw(browser_t *browser);

// Launch browser (non-blocking, registers with WM)
void browser_launch_kernel(void);

// Launch with URL
void browser_launch_url(const char *url);

// Window manager callbacks
void browser_on_event(void *app_data, gui_event_t *event);
void browser_on_draw(void *app_data);
void browser_on_destroy(void *app_data);

#endif // BROWSER_H
