// window.h - Window structures and API for MayteraOS GUI
#ifndef WINDOW_H
#define WINDOW_H

#include "../types.h"

// Forward declarations
struct widget;
struct window;

// Maximum limits
#define MAX_WINDOWS         32
#define MAX_WINDOW_TITLE    64
#define MAX_WIDGETS         64
#define MAX_APPS            16
#define EVENT_QUEUE_SIZE    64
#define MAX_DIRTY_RECTS     16

// Window flags
#define WINDOW_FLAG_VISIBLE     (1 << 0)
#define WINDOW_FLAG_FOCUSED     (1 << 1)
#define WINDOW_FLAG_DRAGGING    (1 << 2)
#define WINDOW_FLAG_MOVABLE     (1 << 3)
#define WINDOW_FLAG_CLOSABLE    (1 << 4)
#define WINDOW_FLAG_RESIZABLE   (1 << 5)
#define WINDOW_FLAG_RESIZING    (1 << 6)
#define WINDOW_FLAG_MINIMIZED   (1 << 7)
#define WINDOW_FLAG_MAXIMIZED   (1 << 8)
#define WINDOW_FLAG_NOCHROME    (1 << 9)  // #185: borderless panel (no titlebar/border/buttons)

// Default window flags
#define WINDOW_FLAGS_DEFAULT (WINDOW_FLAG_VISIBLE | WINDOW_FLAG_MOVABLE | WINDOW_FLAG_CLOSABLE)

// Title bar dimensions
#define TITLEBAR_HEIGHT     20
#define CLOSE_BUTTON_SIZE   16
#define BORDER_WIDTH        2

// Resize grip dimensions
#define RESIZE_GRIP_SIZE    10

// Minimum window size constraints
#define WINDOW_MIN_WIDTH    100
#define WINDOW_MIN_HEIGHT   50

// Window colors - Modern grey/white theme
#define COLOR_TITLEBAR_ACTIVE       0x003A3A3A  // Dark grey
#define COLOR_TITLEBAR_INACTIVE     0x002D2D2D  // Darker grey
#define COLOR_TITLEBAR_TEXT         0x00E0E0E0  // Light grey/white
#define COLOR_WINDOW_BG             0x00F5F5F5  // Very light grey (almost white)
#define COLOR_WINDOW_BORDER         0x00505050  // Medium dark grey
#define COLOR_CLOSE_BUTTON          0x00606060  // Grey close button
#define COLOR_CLOSE_BUTTON_HOVER    0x00E04040  // Subtle red on hover only
#define COLOR_MINIMIZE_BUTTON       0x00707070  // Grey
#define COLOR_MAXIMIZE_BUTTON       0x00707070  // Grey

// Title bar button spacing
#define TITLEBAR_BUTTON_SPACING     2

// Event types
typedef enum {
    EVENT_NONE = 0,
    EVENT_MOUSE_MOVE,
    EVENT_MOUSE_DOWN,
    EVENT_MOUSE_UP,
    EVENT_MOUSE_SCROLL,
    EVENT_KEY_DOWN,
    EVENT_KEY_UP,
    EVENT_WINDOW_CLOSE,
    EVENT_WINDOW_FOCUS,
    EVENT_WINDOW_BLUR,
    EVENT_BUTTON_CLICK,
    EVENT_REDRAW,
    EVENT_RESIZE
} event_type_t;

// Mouse button flags
#define MOUSE_BUTTON_LEFT   (1 << 0)
#define MOUSE_BUTTON_RIGHT  (1 << 1)
#define MOUSE_BUTTON_MIDDLE (1 << 2)

// Event structure
typedef struct {
    event_type_t type;
    uint32_t target_id;     // Window or widget ID
    int32_t mouse_x;        // Mouse X position (screen coords)
    int32_t mouse_y;        // Mouse Y position (screen coords)
    uint32_t mouse_buttons; // Mouse button state
    int8_t scroll_delta;    // Scroll wheel delta (positive=up, negative=down)
    uint32_t keycode;       // Keyboard keycode
    char key_char;          // Printable character (if any)
} gui_event_t;

// Event handler function type
typedef void (*event_handler_t)(struct window *win, gui_event_t *event);

// ============================================================================
// Window Manager Event Queue and App Registration
// ============================================================================

// Event queue for centralized event handling
typedef struct {
    gui_event_t events[EVENT_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} event_queue_t;

// App callback function types
typedef void (*app_event_handler_t)(void *app_data, gui_event_t *event);
typedef void (*app_draw_handler_t)(void *app_data);
typedef void (*app_destroy_handler_t)(void *app_data);

// Rectangle structure (must be defined before dirty_region_t)
typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} rect_t;

// App registration structure
typedef struct {
    struct window *window;          // Associated window
    void *app_data;                 // Application-specific data
    app_event_handler_t on_event;   // Event callback
    app_draw_handler_t on_draw;     // Draw callback
    app_destroy_handler_t on_destroy; // Destroy callback
    bool active;                    // Is this registration active
} app_registration_t;

// Dirty region tracking for efficient redraws
typedef struct {
    rect_t rects[MAX_DIRTY_RECTS];
    uint32_t count;
    bool full_redraw;               // If true, redraw entire screen
} dirty_region_t;

// Window structure
typedef struct window {
    uint32_t id;                        // Unique window ID
    char title[MAX_WINDOW_TITLE];       // Window title
    rect_t bounds;                      // Position and size (including title bar)
    uint32_t flags;                     // Window flags
    uint32_t z_order;                   // Z-ordering (higher = on top)

    // Colors (customizable per window)
    uint32_t bg_color;                  // Background color
    uint32_t border_color;              // Border color
    uint32_t titlebar_color;            // Title bar color

    // Widgets
    struct widget *widgets;             // Linked list of widgets
    uint32_t widget_count;              // Number of widgets

    // Event handling
    event_handler_t on_close;           // Close handler
    event_handler_t on_click;           // Click handler
    event_handler_t on_key;             // Key handler
    void *user_data;                    // User-defined data

    // Drag state
    int32_t drag_offset_x;
    int32_t drag_offset_y;

    // Resize state
    uint32_t resize_edge;           // Which edge(s) are being resized

    // Stored bounds for restore from maximize
    rect_t stored_bounds;           // Original bounds before maximize

    // Transparency (0-255, 255=opaque) + lock-in-place
    uint8_t opacity;
    uint8_t locked;
    // Per-window decorator state (Task A redesign).
    uint8_t theme_override;   // 0 = follow system, 1 = force dark, 2 = force light
    uint8_t scale_pct;        // window content scale percent (50..200, default 100)
    int32_t scale_base_w;     // natural width captured for scale baseline (0 = uncaptured)
    int32_t scale_base_h;     // natural height captured for scale baseline

    // Linked list pointers
    struct window *next;
    struct window *prev;
} window_t;

// Window manager functions

// Initialize the window manager
void wm_init(void);

// Create a new window
window_t *window_create(const char *title, int32_t x, int32_t y, int32_t width, int32_t height);
void wm_set_default_opacity(int opacity);  // global default + apply to all windows
extern uint8_t g_default_window_opacity;

// Destroy a window
void window_destroy(window_t *win);

// Show/hide a window
void window_show(window_t *win);
void window_hide(window_t *win);

// Minimize/maximize a window
void window_minimize(window_t *win);
void window_maximize(window_t *win);
void window_restore(window_t *win);
void wm_toggle_maximize_focused(void);

// Move a window
void window_move(window_t *win, int32_t x, int32_t y);

// Resize a window
void window_resize(window_t *win, int32_t width, int32_t height);

// Set window title
void window_set_title(window_t *win, const char *title);

// Z-ordering
void window_bring_to_front(window_t *win);
void window_send_to_back(window_t *win);
window_t *window_get_at_point(int32_t x, int32_t y);

// Focus management
void window_set_focus(window_t *win);
window_t *window_get_focused(void);

// Event handling
void window_set_close_handler(window_t *win, event_handler_t handler);
void window_set_click_handler(window_t *win, event_handler_t handler);
void window_set_key_handler(window_t *win, event_handler_t handler);

// Process events (call this from main loop)
void wm_process_events(void);

// Handle mouse events
void wm_handle_mouse_move(int32_t x, int32_t y);
void wm_handle_mouse_down(int32_t x, int32_t y, uint32_t button);
void wm_handle_mouse_up(int32_t x, int32_t y, uint32_t button);

// Handle keyboard events
void wm_handle_key_down(uint32_t keycode, char key_char);
void wm_handle_key_up(uint32_t keycode);

// Drawing
void window_draw(window_t *win);
void window_invalidate(window_t *win);
void wm_draw_all(void);
void wm_draw_winmenu(void);   // decorator popup, drawn on top after wm_draw_apps()
void wm_redraw_screen(void);

// Utility functions
bool rect_contains_point(const rect_t *rect, int32_t x, int32_t y);
bool rect_intersects(const rect_t *a, const rect_t *b);
rect_t window_get_client_rect(const window_t *win);

// Get the content area (excluding title bar and borders)
void window_get_content_bounds(const window_t *win, int32_t *x, int32_t *y, int32_t *w, int32_t *h);

// Draw text in window coordinates
void window_draw_text(window_t *win, int32_t x, int32_t y, const char *text, uint32_t color);

// ============================================================================
// Event Queue Functions
// ============================================================================

// Queue an event for later processing
void wm_queue_event(gui_event_t *event);

// Poll the next event from the queue (returns false if empty)
bool wm_poll_event(gui_event_t *event);

// Clear all queued events
void wm_clear_event_queue(void);

// ============================================================================
// App Registration Functions
// ============================================================================

// Register an app with the window manager
// Returns app_id (>= 0) on success, -1 on failure
int wm_register_app(window_t *win, void *app_data,
                    app_event_handler_t on_event,
                    app_draw_handler_t on_draw,
                    app_destroy_handler_t on_destroy);

// Unregister an app
void wm_unregister_app(int app_id);

// Get app registration by window
app_registration_t *wm_get_app_by_window(window_t *win);

// Get app registration by id
app_registration_t *wm_get_app_by_id(int app_id);

// Get all windows (for task manager, etc.)
// Returns count, fills array pointer
int wm_get_window_count(void);
window_t *wm_get_window_at_index(int index);

// Focus a window (brings to front and sets focus)
void wm_focus_window(window_t *win);

// Close a window through app registration (triggers proper cleanup)
void window_close(window_t *win);

// ============================================================================
// Dirty Region Tracking
// ============================================================================

// Mark a rectangle as needing redraw
void wm_invalidate_rect(const rect_t *rect);

// Mark entire screen as needing redraw
void wm_invalidate_all(void);

// Check if any region needs redraw
bool wm_is_dirty(void);

// Clear dirty state after redraw
void wm_clear_dirty(void);

// Get dirty region info
const dirty_region_t *wm_get_dirty_region(void);

// ============================================================================
// Main Frame Processing
// ============================================================================

// Process one frame: dispatch events to apps, render if dirty
// Returns false if desktop should exit
bool wm_process_frame(void);

// Dispatch a single event to the appropriate app
void wm_dispatch_event(gui_event_t *event);

// Draw all registered app contents (call after wm_draw_all)
void wm_draw_apps(void);

// Exclusive fullscreen mode (used by DOOM and other full-screen apps)
void wm_enter_exclusive_mode(void);
void wm_exit_exclusive_mode(void);
bool wm_is_exclusive_mode(void);

// Notify user-mode apps when their window is resized (called from window_resize)
void user_window_handle_resize(window_t *win);


// Window info for userland compositor query (sys_wm_get_windows)
typedef struct {
    int     id;
    int     x, y, width, height;
    int     visible;
    int     minimized;
    int     focused;
    char    title[64];
} wm_window_info_t;

// Syscall: fill buf with info about up to max_count windows
// Returns number of windows filled in
int64_t sys_wm_get_windows(wm_window_info_t *buf, int max_count);
int64_t sys_wm_focus_window(int id);
int64_t sys_wm_minimize_window(int id);
void wm_inject_app_mouse(int32_t x, int32_t y, int32_t type, uint32_t button);
void wm_inject_app_scroll(int32_t x, int32_t y, int32_t delta);

#endif // WINDOW_H
