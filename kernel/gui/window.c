// window.c - Window manager implementation for MayteraOS GUI
#include "window.h"
#include "widget.h"
#include "themes.h"
#include "../types.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "icons.h"
#include "ttf.h"
#include "../fs/fat.h"

// #165: window transparency/options context menu state + forward decls. The
// state lives here so wm_handle_mouse_down (above the menu helpers) can see it.
static struct { int open; window_t *win; int32_t x, y, w, h; } g_winmenu;
static int winmenu_handle_click(int32_t x, int32_t y);
void wm_draw_winmenu(void);

// Kernel time source for title-bar double-click detection.
extern volatile uint64_t timer_ticks;
extern uint32_t pit_get_frequency(void);

// Window manager state
static struct {
    window_t *window_list;      // Linked list of all windows (sorted by z-order)
    window_t *focused_window;   // Currently focused window
    uint32_t next_window_id;    // Next window ID to assign
    uint32_t highest_z_order;   // Highest z-order value

    // Mouse state
    int32_t mouse_x;
    int32_t mouse_y;
    uint32_t mouse_buttons;

    // Drag state
    window_t *dragging_window;

    // Resize state
    window_t *resizing_window;
    int32_t resize_start_x;     // Mouse X when resize started
    int32_t resize_start_y;     // Mouse Y when resize started
    int32_t resize_orig_x;      // Original window X position
    int32_t resize_orig_y;      // Original window Y position
    int32_t resize_orig_w;      // Original window width
    int32_t resize_orig_h;      // Original window height
    uint32_t resize_edge;       // Which edge(s) are being resized

    // Event queue for centralized event handling
    event_queue_t event_queue;

    // App registrations
    app_registration_t apps[MAX_APPS];
    int next_app_id;

    // Dirty region tracking
    dirty_region_t dirty;

    // Desktop running flag
    bool running;

    // Title-bar double-click tracking (toggles maximize/restore)
    uint64_t last_tb_click_tick;
    window_t *last_tb_click_win;
} wm_state;

// Initialize the window manager
void wm_init(void) {
    wm_state.window_list = NULL;
    wm_state.focused_window = NULL;
    wm_state.next_window_id = 1;
    wm_state.highest_z_order = 0;

    wm_state.mouse_x = 0;
    wm_state.mouse_y = 0;
    wm_state.mouse_buttons = 0;
    wm_state.dragging_window = NULL;

    wm_state.resizing_window = NULL;
    wm_state.resize_start_x = 0;
    wm_state.resize_start_y = 0;
    wm_state.resize_orig_x = 0;
    wm_state.resize_orig_y = 0;
    wm_state.resize_orig_w = 0;
    wm_state.resize_orig_h = 0;
    wm_state.resize_edge = 0;

    // Initialize event queue
    wm_state.event_queue.head = 0;
    wm_state.event_queue.tail = 0;
    wm_state.event_queue.count = 0;

    // Initialize app registrations
    for (int i = 0; i < MAX_APPS; i++) {
        wm_state.apps[i].active = false;
        wm_state.apps[i].window = NULL;
        wm_state.apps[i].app_data = NULL;
        wm_state.apps[i].on_event = NULL;
        wm_state.apps[i].on_draw = NULL;
        wm_state.apps[i].on_destroy = NULL;
    }
    wm_state.next_app_id = 0;

    // Initialize dirty region
    wm_state.dirty.count = 0;
    wm_state.dirty.full_redraw = true;  // Initial full redraw

    // Desktop running
    wm_state.running = true;

    kprintf("Window manager initialized\n");
}

// Exclusive mode state: when true, the WM skips normal compositing
static bool g_exclusive_mode = false;

// Enter exclusive fullscreen mode (e.g. for DOOM)
// The caller draws directly to the framebuffer; WM stops compositing.
void wm_enter_exclusive_mode(void) {
    g_exclusive_mode = true;
}

// Exit exclusive fullscreen mode and resume normal WM compositing
void wm_exit_exclusive_mode(void) {
    g_exclusive_mode = false;
    wm_invalidate_all();
}

// Check if a fullscreen/exclusive mode app (e.g. DOOM) is active
bool wm_is_exclusive_mode(void) {
    return g_exclusive_mode;
}

// Create a new window
window_t *window_create(const char *title, int32_t x, int32_t y, int32_t width, int32_t height) {
    window_t *win = (window_t *)kzalloc(sizeof(window_t));
    if (!win) {
        kprintf("Failed to allocate window\n");
        return NULL;
    }

    win->id = wm_state.next_window_id++;
    strncpy(win->title, title, MAX_WINDOW_TITLE - 1);
    win->title[MAX_WINDOW_TITLE - 1] = '\0';

    win->bounds.x = x;
    win->bounds.y = y;
    win->bounds.width = width;
    win->bounds.height = height;

    win->flags = WINDOW_FLAGS_DEFAULT | WINDOW_FLAG_RESIZABLE;
    win->z_order = ++wm_state.highest_z_order;

    win->bg_color = THEME_WINDOW_BG;
    win->border_color = THEME_WINDOW_BORDER;
    win->titlebar_color = THEME_TITLEBAR_INACTIVE;

    win->widgets = NULL;
    win->widget_count = 0;

    win->on_close = NULL;
    win->on_click = NULL;
    win->on_key = NULL;
    win->user_data = NULL;

    win->drag_offset_x = 0;
    win->drag_offset_y = 0;

    win->resize_edge = 0;
    win->opacity = g_default_window_opacity;
    win->locked = 0;
    win->theme_override = 0;     // follow system theme by default
    win->scale_pct = 100;       // 100% content scale by default
    win->scale_base_w = 0;
    win->scale_base_h = 0;

    // Add to window list (sorted by z-order, highest first)
    if (!wm_state.window_list) {
        wm_state.window_list = win;
        win->next = NULL;
        win->prev = NULL;
    } else {
        // Insert at head (new windows go on top)
        win->next = wm_state.window_list;
        win->prev = NULL;
        wm_state.window_list->prev = win;
        wm_state.window_list = win;
    }

    kprintf("Created window '%s' at (%d, %d) size %dx%d\n", title, x, y, width, height);
    return win;
}

// Destroy a window
void window_destroy(window_t *win) {
    if (!win) return;

    // Destroy all widgets first
    widgets_destroy_all(win);

    // Remove from window list
    if (win->prev) {
        win->prev->next = win->next;
    } else {
        wm_state.window_list = win->next;
    }
    if (win->next) {
        win->next->prev = win->prev;
    }

    // Clear focus if this was the focused window
    if (wm_state.focused_window == win) {
        wm_state.focused_window = wm_state.window_list;
    }

    // Stop dragging if we were dragging this window
    if (wm_state.dragging_window == win) {
        wm_state.dragging_window = NULL;
    }

    // Stop resizing if we were resizing this window
    if (wm_state.resizing_window == win) {
        wm_state.resizing_window = NULL;
    }

    kprintf("Destroyed window '%s'\n", win->title);
    kfree(win);
}

// Show a window
void window_show(window_t *win) {
    if (win) {
        win->flags |= WINDOW_FLAG_VISIBLE;
    }
}

// Hide a window
void window_hide(window_t *win) {
    if (win) {
        win->flags &= ~WINDOW_FLAG_VISIBLE;
    }
}

// Minimize a window
void window_minimize(window_t *win) {
    if (!win) return;

    // If already minimized, do nothing
    if (win->flags & WINDOW_FLAG_MINIMIZED) return;

    // Set minimized flag and hide the window
    win->flags |= WINDOW_FLAG_MINIMIZED;
    win->flags &= ~WINDOW_FLAG_VISIBLE;

    kprintf("Minimized window '%s'\n", win->title);
}

// Maximize a window
void window_maximize(window_t *win) {
    if (!win) return;

    // If already maximized, do nothing
    if (win->flags & WINDOW_FLAG_MAXIMIZED) return;

    // Store current bounds for restore
    win->stored_bounds.x = win->bounds.x;
    win->stored_bounds.y = win->bounds.y;
    win->stored_bounds.width = win->bounds.width;
    win->stored_bounds.height = win->bounds.height;

    // Get actual screen dimensions from framebuffer
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();

    // Leave some space for dock at bottom (80 pixels)
    win->bounds.x = 0;
    win->bounds.y = 0;
    win->bounds.width = screen_w;
    win->bounds.height = screen_h - 80;

    win->flags |= WINDOW_FLAG_MAXIMIZED;

    // Notify the owning user-mode process so it can resize its content buffer
    // and reflow (e.g. terminal recomputes columns/rows for the new size).
    user_window_handle_resize(win);

    kprintf("Maximized window '%s' to %ux%u\n", win->title, win->bounds.width, win->bounds.height);
}

// Restore a window from minimized or maximized state
// Toggle maximize/restore of the currently focused window. Used by the
// F11 hotkey (kernel desktop mode) and SYS_WM_MAXIMIZE_WINDOW (compositor).
void wm_toggle_maximize_focused(void) {
    window_t *w = wm_state.focused_window;
    if (!w) return;
    if (w->flags & WINDOW_FLAG_MAXIMIZED) window_restore(w);
    else window_maximize(w);
}

void window_restore(window_t *win) {
    if (!win) return;

    // Restore from minimized
    if (win->flags & WINDOW_FLAG_MINIMIZED) {
        win->flags &= ~WINDOW_FLAG_MINIMIZED;
        win->flags |= WINDOW_FLAG_VISIBLE;
        kprintf("Restored minimized window '%s'\n", win->title);
    }

    // Restore from maximized
    if (win->flags & WINDOW_FLAG_MAXIMIZED) {
        win->bounds.x = win->stored_bounds.x;
        win->bounds.y = win->stored_bounds.y;
        win->bounds.width = win->stored_bounds.width;
        win->bounds.height = win->stored_bounds.height;

        win->flags &= ~WINDOW_FLAG_MAXIMIZED;

        // Notify the owning user-mode process so it can resize its content
        // buffer and reflow back to the restored dimensions.
        user_window_handle_resize(win);

        kprintf("Restored maximized window '%s'\n", win->title);
    }
}

// Move a window
void window_move(window_t *win, int32_t x, int32_t y) {
    if (win) {
        win->bounds.x = x;
        win->bounds.y = y;
    }
}

// Resize a window
void window_resize(window_t *win, int32_t width, int32_t height) {
    if (win) {
        win->bounds.width = width;
        win->bounds.height = height;

        // Notify user-mode process that owns this window (if any)
        user_window_handle_resize(win);
    }
}

// Set window title
void window_set_title(window_t *win, const char *title) {
    if (win && title) {
        strncpy(win->title, title, MAX_WINDOW_TITLE - 1);
        win->title[MAX_WINDOW_TITLE - 1] = '\0';
    }
}

// Bring window to front
void window_bring_to_front(window_t *win) {
    if (!win) return;

    // Already at front?
    if (wm_state.window_list == win) return;

    // Remove from current position
    if (win->prev) {
        win->prev->next = win->next;
    }
    if (win->next) {
        win->next->prev = win->prev;
    }

    // Insert at head
    win->next = wm_state.window_list;
    win->prev = NULL;
    if (wm_state.window_list) {
        wm_state.window_list->prev = win;
    }
    wm_state.window_list = win;

    // Update z-order
    win->z_order = ++wm_state.highest_z_order;
}

// Send window to back
void window_send_to_back(window_t *win) {
    if (!win) return;

    // Remove from current position
    if (win->prev) {
        win->prev->next = win->next;
    } else {
        wm_state.window_list = win->next;
    }
    if (win->next) {
        win->next->prev = win->prev;
    }

    // Find the end of the list
    window_t *last = wm_state.window_list;
    while (last && last->next) {
        last = last->next;
    }

    // Insert at end
    if (last) {
        last->next = win;
        win->prev = last;
        win->next = NULL;
    } else {
        wm_state.window_list = win;
        win->prev = NULL;
        win->next = NULL;
    }

    // Update z-order to minimum
    win->z_order = 0;
}

// Get window at point (returns topmost window)
window_t *window_get_at_point(int32_t x, int32_t y) {
    // Iterate from front to back (head of list is front)
    window_t *win = wm_state.window_list;
    while (win) {
        if ((win->flags & WINDOW_FLAG_VISIBLE) && rect_contains_point(&win->bounds, x, y)) {
            return win;
        }
        win = win->next;
    }
    return NULL;
}

// Set focus to window
void window_set_focus(window_t *win) {
    if (wm_state.focused_window == win) return;

    // Remove focus from old window
    if (wm_state.focused_window) {
        wm_state.focused_window->flags &= ~WINDOW_FLAG_FOCUSED;
        wm_state.focused_window->titlebar_color = THEME_TITLEBAR_INACTIVE;
    }

    // Set focus to new window
    wm_state.focused_window = win;
    if (win) {
        win->flags |= WINDOW_FLAG_FOCUSED;
        win->titlebar_color = THEME_TITLEBAR_ACTIVE;
    }
}

// Get focused window
window_t *window_get_focused(void) {
    return wm_state.focused_window;
}

// Set event handlers
void window_set_close_handler(window_t *win, event_handler_t handler) {
    if (win) win->on_close = handler;
}

void window_set_click_handler(window_t *win, event_handler_t handler) {
    if (win) win->on_click = handler;
}

void window_set_key_handler(window_t *win, event_handler_t handler) {
    if (win) win->on_key = handler;
}

// Utility: check if point is in rectangle
bool rect_contains_point(const rect_t *rect, int32_t x, int32_t y) {
    return (x >= rect->x && x < rect->x + rect->width &&
            y >= rect->y && y < rect->y + rect->height);
}

// Utility: check if rectangles intersect
bool rect_intersects(const rect_t *a, const rect_t *b) {
    return !(a->x + a->width <= b->x || b->x + b->width <= a->x ||
             a->y + a->height <= b->y || b->y + b->height <= a->y);
}

// Get client rect (content area without title bar and borders)
rect_t window_get_client_rect(const window_t *win) {
    rect_t client;
    if (win->flags & WINDOW_FLAG_NOCHROME) {
        client.x = win->bounds.x; client.y = win->bounds.y;
        client.width = win->bounds.width; client.height = win->bounds.height;
        return client;
    }
    client.x = win->bounds.x + BORDER_WIDTH;
    client.y = win->bounds.y + TITLEBAR_HEIGHT + BORDER_WIDTH;
    client.width = win->bounds.width - 2 * BORDER_WIDTH;
    client.height = win->bounds.height - TITLEBAR_HEIGHT - 2 * BORDER_WIDTH;
    return client;
}

// Get content bounds (for internal use)
void window_get_content_bounds(const window_t *win, int32_t *x, int32_t *y, int32_t *w, int32_t *h) {
    if (win->flags & WINDOW_FLAG_NOCHROME) {
        *x = win->bounds.x; *y = win->bounds.y;
        *w = win->bounds.width; *h = win->bounds.height;
        return;
    }
    *x = win->bounds.x + BORDER_WIDTH;
    *y = win->bounds.y + TITLEBAR_HEIGHT + BORDER_WIDTH;
    *w = win->bounds.width - 2 * BORDER_WIDTH;
    *h = win->bounds.height - TITLEBAR_HEIGHT - 2 * BORDER_WIDTH;
}

// Check if point is in title bar
static bool is_in_titlebar(window_t *win, int32_t x, int32_t y) {
    if (win->flags & WINDOW_FLAG_NOCHROME) return false;
    return (x >= win->bounds.x && x < win->bounds.x + win->bounds.width &&
            y >= win->bounds.y && y < win->bounds.y + TITLEBAR_HEIGHT);
}

// Check if point is on close button
static bool is_on_close_button(window_t *win, int32_t x, int32_t y) {
    int32_t btn_x = win->bounds.x + win->bounds.width - CLOSE_BUTTON_SIZE - 2;
    int32_t btn_y = win->bounds.y + 2;
    return (x >= btn_x && x < btn_x + CLOSE_BUTTON_SIZE &&
            y >= btn_y && y < btn_y + CLOSE_BUTTON_SIZE);
}
// Global default window opacity (new windows inherit this).
uint8_t g_default_window_opacity = 255;

// Settings cog button: 4th button, left of [minimize][maximize][close].
static bool is_on_cog_button(window_t *win, int32_t x, int32_t y) {
    int32_t btn_x = win->bounds.x + win->bounds.width - 4 * CLOSE_BUTTON_SIZE - 2 - 3 * TITLEBAR_BUTTON_SPACING;
    int32_t btn_y = win->bounds.y + 2;
    return (x >= btn_x && x < btn_x + CLOSE_BUTTON_SIZE &&
            y >= btn_y && y < btn_y + CLOSE_BUTTON_SIZE);
}

void wm_set_default_opacity(int opacity) {
    if (opacity < 40) opacity = 40;        // never fully invisible
    if (opacity > 255) opacity = 255;
    g_default_window_opacity = (uint8_t)opacity;
    for (window_t *w = wm_state.window_list; w; w = w->next) w->opacity = (uint8_t)opacity;
    wm_invalidate_all();
}

int wm_get_default_opacity(void) {
    return (int)g_default_window_opacity;
}

// Resize edge flags
#define RESIZE_EDGE_NONE    0
#define RESIZE_EDGE_LEFT    (1 << 0)
#define RESIZE_EDGE_RIGHT   (1 << 1)
#define RESIZE_EDGE_TOP     (1 << 2)
#define RESIZE_EDGE_BOTTOM  (1 << 3)
#define RESIZE_EDGE_TL      (RESIZE_EDGE_TOP | RESIZE_EDGE_LEFT)
#define RESIZE_EDGE_TR      (RESIZE_EDGE_TOP | RESIZE_EDGE_RIGHT)
#define RESIZE_EDGE_BL      (RESIZE_EDGE_BOTTOM | RESIZE_EDGE_LEFT)
#define RESIZE_EDGE_BR      (RESIZE_EDGE_BOTTOM | RESIZE_EDGE_RIGHT)

// Check if point is on any resize grip (all 4 corners)
// Returns the edge flags for which edge(s) to resize, or 0 if not on a grip
static uint32_t get_resize_edge(window_t *win, int32_t x, int32_t y) {
    if (win->flags & WINDOW_FLAG_NOCHROME) {
        return RESIZE_EDGE_NONE;
    }
    if (!(win->flags & WINDOW_FLAG_RESIZABLE)) {
        return RESIZE_EDGE_NONE;
    }

    int32_t wx = win->bounds.x;
    int32_t wy = win->bounds.y;
    int32_t ww = win->bounds.width;
    int32_t wh = win->bounds.height;

    // Check if in any corner grip area
    bool on_left = (x >= wx && x < wx + RESIZE_GRIP_SIZE);
    bool on_right = (x >= wx + ww - RESIZE_GRIP_SIZE && x < wx + ww);
    bool on_top = (y >= wy && y < wy + RESIZE_GRIP_SIZE);
    bool on_bottom = (y >= wy + wh - RESIZE_GRIP_SIZE && y < wy + wh);

    // Must be within window bounds
    if (x < wx || x >= wx + ww || y < wy || y >= wy + wh) {
        return RESIZE_EDGE_NONE;
    }

    // Check corners first (corners take priority over edges)
    if (on_top && on_left) return RESIZE_EDGE_TL;
    if (on_top && on_right) return RESIZE_EDGE_TR;
    if (on_bottom && on_left) return RESIZE_EDGE_BL;
    if (on_bottom && on_right) return RESIZE_EDGE_BR;

    return RESIZE_EDGE_NONE;
}


// Check if point is on minimize button (leftmost of the three buttons)
// Button layout: [minimize][maximize][close] on right side of title bar
static bool is_on_minimize_button(window_t *win, int32_t x, int32_t y) {
    // Close is at: width - CLOSE_BUTTON_SIZE - 2
    // Maximize is at: width - 2*CLOSE_BUTTON_SIZE - 2 - TITLEBAR_BUTTON_SPACING
    // Minimize is at: width - 3*CLOSE_BUTTON_SIZE - 2 - 2*TITLEBAR_BUTTON_SPACING
    int32_t btn_x = win->bounds.x + win->bounds.width - 3 * CLOSE_BUTTON_SIZE - 2 - 2 * TITLEBAR_BUTTON_SPACING;
    int32_t btn_y = win->bounds.y + 2;
    return (x >= btn_x && x < btn_x + CLOSE_BUTTON_SIZE &&
            y >= btn_y && y < btn_y + CLOSE_BUTTON_SIZE);
}

// Check if point is on maximize button (middle of the three buttons)
static bool is_on_maximize_button(window_t *win, int32_t x, int32_t y) {
    // Maximize is at: width - 2*CLOSE_BUTTON_SIZE - 2 - TITLEBAR_BUTTON_SPACING
    int32_t btn_x = win->bounds.x + win->bounds.width - 2 * CLOSE_BUTTON_SIZE - 2 - TITLEBAR_BUTTON_SPACING;
    int32_t btn_y = win->bounds.y + 2;
    return (x >= btn_x && x < btn_x + CLOSE_BUTTON_SIZE &&
            y >= btn_y && y < btn_y + CLOSE_BUTTON_SIZE);
}

// Handle mouse move
void wm_handle_mouse_move(int32_t x, int32_t y) {
    int32_t old_x = wm_state.mouse_x;
    int32_t old_y = wm_state.mouse_y;
    wm_state.mouse_x = x;
    wm_state.mouse_y = y;

    // Handle window resizing
    if (wm_state.resizing_window) {
        window_t *win = wm_state.resizing_window;

        // Calculate mouse movement from resize start
        int32_t dx = x - wm_state.resize_start_x;
        int32_t dy = y - wm_state.resize_start_y;

        int32_t new_x = wm_state.resize_orig_x;
        int32_t new_y = wm_state.resize_orig_y;
        int32_t new_width = wm_state.resize_orig_w;
        int32_t new_height = wm_state.resize_orig_h;

        uint32_t edge = wm_state.resize_edge;

        // Handle horizontal resizing
        if (edge & RESIZE_EDGE_LEFT) {
            new_x = wm_state.resize_orig_x + dx;
            new_width = wm_state.resize_orig_w - dx;
        } else if (edge & RESIZE_EDGE_RIGHT) {
            new_width = wm_state.resize_orig_w + dx;
        }

        // Handle vertical resizing
        if (edge & RESIZE_EDGE_TOP) {
            new_y = wm_state.resize_orig_y + dy;
            new_height = wm_state.resize_orig_h - dy;
        } else if (edge & RESIZE_EDGE_BOTTOM) {
            new_height = wm_state.resize_orig_h + dy;
        }

        // Apply minimum size constraints
        if (new_width < WINDOW_MIN_WIDTH) {
            if (edge & RESIZE_EDGE_LEFT) {
                new_x = wm_state.resize_orig_x + wm_state.resize_orig_w - WINDOW_MIN_WIDTH;
            }
            new_width = WINDOW_MIN_WIDTH;
        }
        if (new_height < WINDOW_MIN_HEIGHT) {
            if (edge & RESIZE_EDGE_TOP) {
                new_y = wm_state.resize_orig_y + wm_state.resize_orig_h - WINDOW_MIN_HEIGHT;
            }
            new_height = WINDOW_MIN_HEIGHT;
        }

        // Invalidate old window area
        wm_invalidate_rect(&win->bounds);

        // Apply new position and size
        window_move(win, new_x, new_y);
        window_resize(win, new_width, new_height);

        // Invalidate new window area
        wm_invalidate_rect(&win->bounds);
    }

    // Handle window dragging
    if (wm_state.dragging_window) {
        window_t *win = wm_state.dragging_window;

        // Invalidate old position
        wm_invalidate_rect(&win->bounds);

        window_move(win, x - win->drag_offset_x, y - win->drag_offset_y);

        // Invalidate new position
        wm_invalidate_rect(&win->bounds);
    }

    // Update widget hover states
    window_t *win = window_get_at_point(x, y);
    if (win) {
        rect_t client = window_get_client_rect(win);
        int32_t local_x = x - client.x;
        int32_t local_y = y - client.y;

        // Update hover state for all widgets
        widget_t *widget = win->widgets;
        while (widget) {
            if (widget->flags & WIDGET_FLAG_VISIBLE) {
                bool was_hovered = (widget->flags & WIDGET_FLAG_HOVERED) != 0;
                bool is_hovered = rect_contains_point(&widget->bounds, local_x, local_y);

                if (is_hovered) {
                    widget->flags |= WIDGET_FLAG_HOVERED;
                } else {
                    widget->flags &= ~WIDGET_FLAG_HOVERED;
                }

                if (was_hovered != is_hovered) {
                    // Invalidate widget area
                    rect_t abs_bounds = {
                        client.x + widget->bounds.x,
                        client.y + widget->bounds.y,
                        widget->bounds.width,
                        widget->bounds.height
                    };
                    wm_invalidate_rect(&abs_bounds);
                }
            }
            widget = widget->next;
        }
    }

    // NOTE: Don't queue mouse move events here - they are queued by desktop_run()
    // This function is called to HANDLE the event, not to create new ones

    // Mark cursor region as dirty (old and new positions)
    if (old_x != x || old_y != y) {
        rect_t old_cursor = { old_x, old_y, 16, 20 };
        rect_t new_cursor = { x, y, 16, 20 };
        wm_invalidate_rect(&old_cursor);
        wm_invalidate_rect(&new_cursor);
    }
}

// Handle mouse button down
void wm_handle_mouse_down(int32_t x, int32_t y, uint32_t button) {
    wm_state.mouse_buttons |= button;

    // #165: if the window context menu is open, it gets first crack at the click.
    // A click inside it is consumed; a click outside closes it and falls through.
    if (g_winmenu.open) {
        if (winmenu_handle_click(x, y)) return;
    }

    window_t *win = window_get_at_point(x, y);

    // NOTE: Don't queue mouse down events here - they are queued by desktop_run()
    // This function is called to HANDLE the event, not to create new ones

    if (!win) return;

    // Bring window to front and focus it
    window_bring_to_front(win);
    window_set_focus(win);
    wm_invalidate_all();  // Focus change requires full redraw

    // Borderless (NOCHROME) windows have NO kernel-drawn titlebar buttons, so
    // their cog/minimize/maximize/close hit-tests are phantom zones. Skip the
    // whole titlebar-button block for them: those clicks fall through to the
    // app (its on_click) instead of triggering kernel maximise/minimize/close.
    // Chromed windows (no NOCHROME flag) keep their button hit-tests unchanged.
    if (!(win->flags & WINDOW_FLAG_NOCHROME)) {
        // #165: transparency (Filter) button -> open the window context menu under
        // it (Lock/Unlock + Opacity). Toggle if already open for this window.
        if (is_on_cog_button(win, x, y)) {
            int32_t bx = win->bounds.x + win->bounds.width - 4 * CLOSE_BUTTON_SIZE - 2 - 3 * TITLEBAR_BUTTON_SPACING;
            if (g_winmenu.open && g_winmenu.win == win) {
                g_winmenu.open = 0;
            } else {
                g_winmenu.open = 1; g_winmenu.win = win;
                g_winmenu.x = bx; g_winmenu.y = win->bounds.y + TITLEBAR_HEIGHT + BORDER_WIDTH;
            }
            wm_invalidate_all();
            return;
        }

        // Check minimize button
        if (is_on_minimize_button(win, x, y)) {
            window_minimize(win);
            wm_invalidate_all();
            return;
        }

        // Check maximize button
        if (is_on_maximize_button(win, x, y)) {
            if (win->flags & WINDOW_FLAG_MAXIMIZED) {
                window_restore(win);
            } else {
                window_maximize(win);
            }
            wm_invalidate_all();
            return;
        }

        // Check close button
        if ((win->flags & WINDOW_FLAG_CLOSABLE) && is_on_close_button(win, x, y)) {
            // Queue close event for app
            gui_event_t close_event = {0};
            close_event.type = EVENT_WINDOW_CLOSE;
            close_event.target_id = win->id;
            close_event.mouse_x = x;
            close_event.mouse_y = y;
            close_event.mouse_buttons = button;
            wm_queue_event(&close_event);

            if (win->on_close) {
                win->on_close(win, &close_event);
            } else {
                // Default behavior: hide the window
                window_hide(win);
                wm_invalidate_all();
            }
            return;
        }
    }

    // Check resize grip (any corner). A locked window ignores resize (Task A).
    uint32_t resize_edge = win->locked ? RESIZE_EDGE_NONE : get_resize_edge(win, x, y);
    if (resize_edge != RESIZE_EDGE_NONE) {
        win->flags |= WINDOW_FLAG_RESIZING;
        win->resize_edge = resize_edge;
        wm_state.resizing_window = win;
        wm_state.resize_edge = resize_edge;
        wm_state.resize_start_x = x;
        wm_state.resize_start_y = y;
        wm_state.resize_orig_x = win->bounds.x;
        wm_state.resize_orig_y = win->bounds.y;
        wm_state.resize_orig_w = win->bounds.width;
        wm_state.resize_orig_h = win->bounds.height;
        return;
    }

    // Check title bar for dragging (button checks above already returned, so a
    // click that reaches here is on the title bar but not on any button).
    if (is_in_titlebar(win, x, y)) {
        // Double-click on the title bar toggles maximize/restore. This gives a
        // large, safe target for un-maximizing a window, instead of the tiny
        // restore button that sits right next to close in the screen corner.
        uint64_t now = timer_ticks;
        uint32_t freq = pit_get_frequency();
        uint64_t dbl_window = freq ? (uint64_t)(freq / 2) : 125;  // ~0.5s
        if (win == wm_state.last_tb_click_win &&
            (now - wm_state.last_tb_click_tick) < dbl_window) {
            if (win->flags & WINDOW_FLAG_MAXIMIZED) {
                window_restore(win);
            } else {
                window_maximize(win);
            }
            wm_invalidate_all();
            wm_state.last_tb_click_win = NULL;
            wm_state.last_tb_click_tick = 0;
            return;
        }
        wm_state.last_tb_click_win = win;
        wm_state.last_tb_click_tick = now;

        // Single click: begin dragging (if the window is movable and not locked).
        if ((win->flags & WINDOW_FLAG_MOVABLE) && !win->locked) {
            win->flags |= WINDOW_FLAG_DRAGGING;
            win->drag_offset_x = x - win->bounds.x;
            win->drag_offset_y = y - win->bounds.y;
            wm_state.dragging_window = win;
        }
        return;
    }

    // Check widgets in content area
    rect_t client = window_get_client_rect(win);
    int32_t local_x = x - client.x;
    int32_t local_y = y - client.y;

    widget_t *widget = widget_find_at_point(win, local_x, local_y);
    if (widget && (widget->flags & WIDGET_FLAG_ENABLED)) {
        widget->flags |= WIDGET_FLAG_PRESSED;
        widget_handle_mouse_down(widget, local_x, local_y, button);

        // Invalidate widget area
        rect_t abs_bounds = {
            client.x + widget->bounds.x,
            client.y + widget->bounds.y,
            widget->bounds.width,
            widget->bounds.height
        };
        wm_invalidate_rect(&abs_bounds);
    } else if (win->on_click) {
        // No widget clicked, call window click handler
        gui_event_t click_event = {0};
        click_event.type = EVENT_MOUSE_DOWN;
        click_event.target_id = win->id;
        click_event.mouse_x = local_x;
        click_event.mouse_y = local_y;
        click_event.mouse_buttons = button;
        win->on_click(win, &click_event);
    }
}

// Handle mouse button up
void wm_handle_mouse_up(int32_t x, int32_t y, uint32_t button) {
    wm_state.mouse_buttons &= ~button;

    window_t *win = window_get_at_point(x, y);

    // NOTE: Don't queue mouse up events here - they are queued by desktop_run()
    // This function is called to HANDLE the event, not to create new ones

    // Stop resizing
    if (wm_state.resizing_window) {
        wm_state.resizing_window->flags &= ~WINDOW_FLAG_RESIZING;
        wm_state.resizing_window->resize_edge = 0;
        wm_state.resizing_window = NULL;
        wm_state.resize_edge = 0;
        wm_invalidate_all();
    }

    // Stop dragging
    if (wm_state.dragging_window) {
        wm_state.dragging_window->flags &= ~WINDOW_FLAG_DRAGGING;
        wm_state.dragging_window = NULL;
        wm_invalidate_all();
    }

    // Handle widget mouse up
    if (win) {
        rect_t client = window_get_client_rect(win);
        int32_t local_x = x - client.x;
        int32_t local_y = y - client.y;

        widget_t *widget = win->widgets;
        while (widget) {
            if (widget->flags & WIDGET_FLAG_PRESSED) {
                widget->flags &= ~WIDGET_FLAG_PRESSED;
                widget_handle_mouse_up(widget, local_x, local_y, button);

                // Invalidate widget area
                rect_t abs_bounds = {
                    client.x + widget->bounds.x,
                    client.y + widget->bounds.y,
                    widget->bounds.width,
                    widget->bounds.height
                };
                wm_invalidate_rect(&abs_bounds);
            }
            widget = widget->next;
        }
    }
}

// Handle key down
void wm_handle_key_down(uint32_t keycode, char key_char) {
    window_t *win = wm_state.focused_window;

    // F11: toggle maximize/restore of the focused window (global WM hotkey).
    // Goes through window_maximize/window_restore, which call
    // user_window_handle_resize() so the owning app reflows to the new size.
    // Intercepted here so F11 is not also acted on by the app.
    if (keycode == 0x85 && win) {  // 0x85 = F11 (translated special-key code)
        if (win->flags & WINDOW_FLAG_MAXIMIZED) window_restore(win);
        else window_maximize(win);
        return;
    }

    // NOTE: Don't queue key events here - they are queued by desktop_run()
    // This function is called to HANDLE the event, not to create new ones

    // Call the direct handler for backwards compatibility
    if (win && win->on_key) {
        gui_event_t event = {0};
        event.type = EVENT_KEY_DOWN;
        event.keycode = keycode;
        event.key_char = key_char;
        if (win) event.target_id = win->id;
        win->on_key(win, &event);
    }
}

// Handle key up
void wm_handle_key_up(uint32_t keycode) {
    window_t *win = wm_state.focused_window;

    // NOTE: Don't queue key events here - they are queued by desktop_run()
    // This function is called to HANDLE the event, not to create new ones

    // Call the direct handler for backwards compatibility
    if (win && win->on_key) {
        gui_event_t event = {0};
        event.type = EVENT_KEY_UP;
        event.keycode = keycode;
        if (win) event.target_id = win->id;
        win->on_key(win, &event);
    }
}

// Draw a single character
static void draw_char(int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = font_get_glyph(c);
    if (!glyph) return;

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            fb_put_pixel(x + col, y + row, color);
        }
    }
}

// Draw text at position
__attribute__((unused))
static void draw_text(int32_t x, int32_t y, const char *text, uint32_t fg, uint32_t bg) {
    while (*text) {
        draw_char(x, y, *text, fg, bg);
        x += FONT_WIDTH;
        text++;
    }
}

// Draw text (no background - transparent)
static void draw_text_transparent(int32_t x, int32_t y, const char *text, uint32_t fg) {
    while (*text) {
        const uint8_t *glyph = font_get_glyph(*text);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(x + col, y + row, fg);
                    }
                }
            }
        }
        x += FONT_WIDTH;
        text++;
    }
}

// Readable title-bar ink derived from the bar bg: near-black on light bars,
// warm off-white (beige) on dark bars. Keeps the heading legible whatever the
// titlebar colour becomes (see #140).
static uint32_t win_title_ink(uint32_t bg) {
    uint8_t r = (bg >> 16) & 0xFF, g = (bg >> 8) & 0xFF, b = bg & 0xFF;
    uint32_t luma = (r * 77 + g * 150 + b * 29) >> 8;
    return (luma >= 140) ? 0x00232018 : 0x00EDE4D0;
}
// Case-insensitive substring match (freestanding).
static int win_ci_has(const char *h, const char *n) {
    if (!h || !n) return 0;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return 1;
    }
    return 0;
}
// Phase 4: the modern window dressing (gradient titlebar) applies to every theme
// except the Classic (CDE/Motif) family, mirroring the Settings app's
// classic-vs-modern split so window chrome and app chrome agree.
static int win_modern_style(void) {
    const char *n = theme_get_name(theme_get_current_id());
    if (!n) return 1;
    if (win_ci_has(n, "classic") || win_ci_has(n, "retro") ||
        win_ci_has(n, "cde") || win_ci_has(n, "motif")) return 0;
    return 1;
}
// #136: map a window title to a titlebar icon (windows carry no icon id).
static icon_id_t window_title_icon(const char *t) {
    if (win_ci_has(t, "setting"))  return ICON_COG;
    if (win_ci_has(t, "file"))     return ICON_FOLDER;
    if (win_ci_has(t, "terminal") || win_ci_has(t, "console")) return ICON_TERMINAL;
    if (win_ci_has(t, "calc"))     return ICON_CALCULATOR;
    if (win_ci_has(t, "edit") || win_ci_has(t, "text"))  return ICON_HIGHLIGHT;
    if (win_ci_has(t, "paint"))    return ICON_PAINT;
    if (win_ci_has(t, "image") || win_ci_has(t, "photo") || win_ci_has(t, "viewer")) return ICON_IMAGE;
    if (win_ci_has(t, "audio") || win_ci_has(t, "music") || win_ci_has(t, "media") || win_ci_has(t, "player")) return ICON_MUSIC;
    if (win_ci_has(t, "clock") || win_ci_has(t, "world")) return ICON_CLOCK;
    if (win_ci_has(t, "task"))     return ICON_TASK_MANAGER;
    if (win_ci_has(t, "log"))      return ICON_LOG_VIEWER;
    if (win_ci_has(t, "recycle") || win_ci_has(t, "trash")) return ICON_TRASH;
    if (win_ci_has(t, "doom"))     return ICON_GAME_DOOM;
    if (win_ci_has(t, "solit") || win_ci_has(t, "freecell") || win_ci_has(t, "card") || win_ci_has(t, "tut")) return ICON_GAME_SOLITAIRE;
    if (win_ci_has(t, "tetris") || win_ci_has(t, "chip") || win_ci_has(t, "golf") || win_ci_has(t, "jezz") || win_ci_has(t, "rodent") || win_ci_has(t, "tetra") || win_ci_has(t, "pong") || win_ci_has(t, "game") || win_ci_has(t, "irc")) return ICON_GAME;
    return ICON_WINDOW;
}

// Draw window
// ---- Kernel MICO .ICN loader (titlebar button glyphs, #165) --------------
// MICO: 12-byte header ('MICO' + w u32LE + h u32LE) then w*h*4 BGRA. White line
// art with coverage in the alpha channel; we tint it to the button ink and
// alpha-blend over the button background. Small fixed cache.
extern fat_fs_t g_fat_fs;
typedef struct { char name[16]; int w, h, ok; uint8_t px[64 * 64 * 4]; } kicon_t;
static kicon_t g_kicons[8];
static int g_kicon_n = 0;
static kicon_t *kicon_get(const char *name) {
    for (int i = 0; i < g_kicon_n; i++)
        if (strcmp(g_kicons[i].name, name) == 0) return &g_kicons[i];
    if (g_kicon_n >= 8) return NULL;
    kicon_t *ic = &g_kicons[g_kicon_n++];
    int n = 0; while (name[n] && n < 15) { ic->name[n] = name[n]; n++; } ic->name[n] = 0;
    ic->ok = 0; ic->w = ic->h = 0;
    char path[40]; int l = 0; const char *p = "/ICONS/";
    while (*p) path[l++] = *p++;
    int k = 0; while (name[k]) path[l++] = name[k++];
    const char *e = ".ICN"; while (*e) path[l++] = *e++; path[l] = 0;
    uint32_t sz = 0; uint8_t *d = (uint8_t *)fat_read_file(&g_fat_fs, path, &sz);
    if (!d) return ic;
    if (sz >= 12 && d[0]=='M' && d[1]=='I' && d[2]=='C' && d[3]=='O') {
        int w = d[4] | (d[5]<<8) | (d[6]<<16) | (d[7]<<24);
        int h = d[8] | (d[9]<<8) | (d[10]<<16) | (d[11]<<24);
        if (w > 0 && h > 0 && w <= 64 && h <= 64 && sz >= 12 + (uint32_t)w*h*4) {
            ic->w = w; ic->h = h;
            for (int i = 0; i < w*h*4; i++) ic->px[i] = d[12 + i];
            ic->ok = 1;
        }
    }
    kfree(d);
    return ic;
}
// Blit a MICO icon scaled (area-averaged) to sz x sz at (x,y), tinted to `ink`,
// alpha-blended over solid `bg`. Returns 0 if the icon is missing (caller draws
// its hand-drawn fallback).
static int kicon_blit(const char *name, int x, int y, int sz, uint32_t ink, uint32_t bg) {
    kicon_t *ic = kicon_get(name);
    if (!ic || !ic->ok) return 0;
    int ir = (ink>>16)&0xFF, ig = (ink>>8)&0xFF, ib = ink&0xFF;
    int br = (bg>>16)&0xFF, bgc = (bg>>8)&0xFF, bb = bg&0xFF;
    for (int dy = 0; dy < sz; dy++) {
        for (int dx = 0; dx < sz; dx++) {
            int sx0 = dx*ic->w/sz, sx1 = (dx+1)*ic->w/sz; if (sx1 <= sx0) sx1 = sx0+1;
            int sy0 = dy*ic->h/sz, sy1 = (dy+1)*ic->h/sz; if (sy1 <= sy0) sy1 = sy0+1;
            int asum = 0, cnt = 0;
            for (int sy = sy0; sy < sy1 && sy < ic->h; sy++)
                for (int sx = sx0; sx < sx1 && sx < ic->w; sx++) {
                    asum += ic->px[(sy*ic->w + sx)*4 + 3]; cnt++;
                }
            int a = cnt ? asum/cnt : 0;
            if (a <= 4) continue;
            int rr = (ir*a + br*(255-a))/255;
            int gg = (ig*a + bgc*(255-a))/255;
            int bbp = (ib*a + bb*(255-a))/255;
            fb_put_pixel(x+dx, y+dy, ((uint32_t)rr<<16)|((uint32_t)gg<<8)|(uint32_t)bbp);
        }
    }
    return 1;
}

// ---- Window decorator popup (#165, Task A redesign) ----------------------
// Opened by the titlebar decorator button. Anchored under that button and drawn
// ON TOP of all window content (see wm_draw_decor_popup() call after
// wm_draw_apps()). Themed via switch(theme_get_current_id()) colour literals
// (theme_color() is mis-indexed; we use literals per the project theme pattern).
// TTF text matches the title-bar text path. Rows:
//   0  Lock / Unlock (toggle)
//   1  Theme:  System | Dark | Light   ( [<] cycle [>] )
//   2  Scale:  NNN%                     ( [-] [+] )
//   3  Opacity: NNN%                    ( [-] [+] )
#define WINMENU_ROWH   26
#define WINMENU_HDR    22
#define WINMENU_W      210
#define WINMENU_ROWS   4

// Per-window decorator palette (bg / text / dim text / border / accent).
typedef struct { uint32_t bg, text, dim, border, accent, rowhi; } decor_pal_t;
static decor_pal_t decor_palette(void) {
    decor_pal_t p;
    switch (theme_get_current_id()) {
        case THEME_LIGHT: case THEME_MODERN_LIGHT: case THEME_FLUENT_LIGHT:
            p.bg=0x00F4F6F9; p.text=0x00202428; p.dim=0x00707880;
            p.border=0x00A8B0BA; p.accent=0x00216FDB; p.rowhi=0x00E1E8F2; break;
        case THEME_DARK: case THEME_MODERN_DARK: case THEME_FLUENT_DARK:
            p.bg=0x001E2228; p.text=0x00E8ECF1; p.dim=0x008A929C;
            p.border=0x003A424C; p.accent=0x004C8DF0; p.rowhi=0x002A3038; break;
        case THEME_OCEAN:
            p.bg=0x00102733; p.text=0x00DCEEF5; p.dim=0x007FA6B5;
            p.border=0x002A4D5E; p.accent=0x0030B0C8; p.rowhi=0x001A3A48; break;
        case THEME_FOREST:
            p.bg=0x00152019; p.text=0x00E0EAD8; p.dim=0x008AA088;
            p.border=0x00324A38; p.accent=0x005CB86A; p.rowhi=0x00203328; break;
        case THEME_SUNSET:
            p.bg=0x002A1822; p.text=0x00F4E2DC; p.dim=0x00B58A93;
            p.border=0x00553040; p.accent=0x00E8794C; p.rowhi=0x003A2230; break;
        case THEME_HIGH_CONTRAST:
            p.bg=0x00000000; p.text=0x00FFFFFF; p.dim=0x00C0C0C0;
            p.border=0x00FFFFFF; p.accent=0x0000FF00; p.rowhi=0x00303030; break;
        case THEME_CLASSIC:
            p.bg=0x00C0C0C0; p.text=0x00000000; p.dim=0x00606060;
            p.border=0x00404040; p.accent=0x00000080; p.rowhi=0x00D8D8D8; break;
        default: /* THEME_DEFAULT retro UNIX/CDE */
            p.bg=0x00222A33; p.text=0x00E8EEF4; p.dim=0x0090A0B0;
            p.border=0x00708090; p.accent=0x0066B2FF; p.rowhi=0x002E3742; break;
    }
    return p;
}

// Draw decorator text via the same TTF path as the title bar, bitmap fallback.
static void decor_text(int32_t x, int32_t y, const char *s, uint32_t col) {
    if (ttf_is_ready()) ttf_draw_string(x, y, s, 13, col);
    else draw_text_transparent(x, y, (s), col);
}
static const char *decor_theme_name(uint8_t ov) {
    return ov == 1 ? "Dark" : ov == 2 ? "Light" : "System";
}
// Append "label NNN%" into buf.
static void decor_pct(char *buf, const char *label, int pct) {
    int i = 0; while (label[i]) { buf[i] = label[i]; i++; }
    if (pct >= 100) { buf[i++]='0'+(pct/100)%10; }
    if (pct >= 10)  { buf[i++]='0'+(pct/10)%10; }
    buf[i++]='0'+pct%10; buf[i++]='%'; buf[i]=0;
}

void wm_draw_winmenu(void) {
    if (!g_winmenu.open || !g_winmenu.win) return;
    extern int g_win_blit_suppressed;
    if (g_win_blit_suppressed) return;
    window_t *win = g_winmenu.win;
    decor_pal_t p = decor_palette();
    int32_t x = g_winmenu.x, y = g_winmenu.y;
    int32_t w = WINMENU_W, h = WINMENU_HDR + WINMENU_ROWS*WINMENU_ROWH + 4;
    // keep the popup on screen horizontally
    extern uint32_t fb_get_width(void);
    int sw = (int)fb_get_width();
    if (sw > 0 && x + w > sw - 2) x = sw - 2 - w;
    if (x < 2) x = 2;
    g_winmenu.x = x; g_winmenu.w = w; g_winmenu.h = h;

    // body + border + drop shadow
    fb_fill_rect(x+3, y+3, w, h, 0x00101418);
    fb_fill_rect(x, y, w, h, p.bg);
    fb_draw_rect(x, y, w, h, p.border);
    // header bar
    fb_fill_rect(x+1, y+1, w-2, WINMENU_HDR-1, p.rowhi);
    fb_fill_rect(x+1, y+WINMENU_HDR, w-2, 1, p.accent);
    decor_text(x+8, y+5, "Window Options", p.accent);

    char buf[24];
    for (int i = 0; i < WINMENU_ROWS; i++) {
        int32_t ry = y + WINMENU_HDR + 2 + i*WINMENU_ROWH;
        int32_t ty = ry + (WINMENU_ROWH-13)/2;
        if (i == 0) {
            const char *lab = win->locked ? "Unlock window" : "Lock window";
            decor_text(x+10, ty, lab, p.text);
            // state pill on the right
            const char *pill = win->locked ? "LOCKED" : "free";
            decor_text(x+w-58, ty, pill, win->locked ? p.accent : p.dim);
        } else {
            // value rows with [-]/[<] and [+]/[>] steppers on the right
            const char *lab; int istheme = 0;
            if (i == 1) { lab = "Theme"; istheme = 1; }
            else if (i == 2) lab = "Scale: ";
            else lab = "Opacity: ";
            decor_text(x+10, ty, lab, p.text);
            if (istheme) {
                decor_text(x+58, ty, decor_theme_name(win->theme_override), p.accent);
            } else {
                int v = (i==2) ? win->scale_pct
                              : (int)((win->opacity * 100 + 127) / 255);
                decor_pct(buf, "", v);
                decor_text(x+66, ty, buf, p.accent);
            }
            // steppers: [-]/[<] on the left of the zone, [+]/[>] on the right
            decor_text(x+w-44, ty, istheme ? "<" : "-", p.text);
            decor_text(x+w-22, ty, istheme ? ">" : "+", p.text);
        }
    }
}

// Returns 1 if the click was consumed by the menu (open/route/close).
static int winmenu_handle_click(int32_t x, int32_t y) {
    if (!g_winmenu.open) return 0;
    int32_t mx = g_winmenu.x, my = g_winmenu.y, mw = g_winmenu.w, mh = g_winmenu.h;
    if (x >= mx && x < mx+mw && y >= my && y < my+mh) {
        window_t *w = g_winmenu.win;
        int row = (y - my - WINMENU_HDR - 2) / WINMENU_ROWH;
        if (w && y >= my + WINMENU_HDR && row >= 0 && row < WINMENU_ROWS) {
            int right = (x >= mx + mw - 56);   // stepper zone on the right
            int plus  = (x >= mx + mw - 28);   // the [+]/[>] vs [-]/[<] split
            if (row == 0) {
                w->locked = !w->locked;
            } else if (row == 1) {
                // theme override cycle: System -> Dark -> Light -> System
                if (right) {
                    if (plus) w->theme_override = (uint8_t)((w->theme_override + 1) % 3);
                    else      w->theme_override = (uint8_t)((w->theme_override + 2) % 3);
                } else {
                    w->theme_override = (uint8_t)((w->theme_override + 1) % 3);
                }
            } else if (row == 2) {
                // scale 50..200 in 10% steps; resize window from captured baseline
                if (w->scale_base_w <= 0) {
                    w->scale_base_w = w->bounds.width;
                    w->scale_base_h = w->bounds.height;
                }
                int s = w->scale_pct ? w->scale_pct : 100;
                if (right) s += plus ? 10 : -10; else s += 10;
                if (s < 50) s = 50;
                if (s > 200) s = 200;
                w->scale_pct = (uint8_t)s;
                int nw = w->scale_base_w * s / 100;
                int nh = w->scale_base_h * s / 100;
                if (nw < 120) nw = 120;
                if (nh < 80) nh = 80;
                window_resize(w, nw, nh);
            } else if (row == 3) {
                int o = w->opacity;
                if (right) o += plus ? 25 : -25; else o += 25;
                if (o < 40) o = 40;
                if (o > 255) o = 255;
                w->opacity = (uint8_t)o;
            }
        }
        wm_invalidate_all();
        return 1;   // keep menu open for repeated taps
    }
    // click outside -> close, and let the click fall through to normal handling
    g_winmenu.open = 0;
    wm_invalidate_all();
    return 0;
}

void window_draw(window_t *win) {
    if (!win || !(win->flags & WINDOW_FLAG_VISIBLE)) return;
    {
        extern int g_win_blit_suppressed;
        if (g_win_blit_suppressed) return;   // screensaver owns the FB; suppress frames + decor
    }

    int32_t x = win->bounds.x;
    int32_t y = win->bounds.y;
    int32_t w = win->bounds.width;
    int32_t h = win->bounds.height;

    // Task A: per-window theme override recolours just this window's chrome
    // (titlebar/border/content fill) without changing the global theme.
    uint32_t ov_titlebar = win->titlebar_color;
    uint32_t ov_border   = win->border_color;
    uint32_t ov_bg       = win->bg_color;
    if (win->theme_override == 1) {          // force Dark
        ov_titlebar = 0x002A3038; ov_border = 0x00485058; ov_bg = 0x001E2228;
    } else if (win->theme_override == 2) {   // force Light
        ov_titlebar = 0x00DDE3EA; ov_border = 0x00A8B0BA; ov_bg = 0x00F4F6F9;
    }

    // #185: borderless panel. Skip all chrome (border/titlebar/buttons/grips)
    // and let the app own the entire window rectangle. Content == full bounds.
    if (win->flags & WINDOW_FLAG_NOCHROME) {
        int32_t cx, cy, cw, ch;
        window_get_content_bounds(win, &cx, &cy, &cw, &ch);
        if (win->opacity >= 255)
            fb_fill_rect(cx, cy, cw, ch, ov_bg);
        widget_t *wd = win->widgets;
        while (wd) { widget_draw(wd); wd = wd->next; }
        return;
    }

    // Draw border
    fb_draw_rect(x, y, w, h, ov_border);

    // #140: recolour a near-white active titlebar to the taskbar colour so the
    // window heading matches the taskbar rather than glaring white.
    uint32_t tb_col = ov_titlebar;
    {
        uint8_t tr = (tb_col >> 16) & 0xFF, tg = (tb_col >> 8) & 0xFF, tbb = tb_col & 0xFF;
        uint32_t tl = (tr * 77 + tg * 150 + tbb * 29) >> 8;
        if (tl >= 200) tb_col = THEME_TASKBAR_BG;
    }
    uint32_t tb_ink = win_title_ink(tb_col);

    // Phase 4: modern themes get a subtle vertical gradient titlebar (lighter at
    // the top for a raised feel); Classic keeps the flat fill it always had.
    // Opaque fill within the (always-redrawn) titlebar region, so it cannot
    // accumulate or trail like an alpha effect would under partial redraws.
    int32_t tbx = x + BORDER_WIDTH, tby = y + BORDER_WIDTH;
    int32_t tbw = w - 2 * BORDER_WIDTH;
    if (win_modern_style()) {
        uint8_t r = (tb_col >> 16) & 0xFF, g = (tb_col >> 8) & 0xFF, b = tb_col & 0xFF;
        // top shade = tb_col lifted ~22% toward white
        int tr = r + (255 - r) * 22 / 100;
        int tg = g + (255 - g) * 22 / 100;
        int tb2 = b + (255 - b) * 22 / 100;
        int span = TITLEBAR_HEIGHT > 1 ? TITLEBAR_HEIGHT - 1 : 1;
        for (int32_t j = 0; j < TITLEBAR_HEIGHT; j++) {
            int rr = tr + (r - tr) * j / span;
            int gg = tg + (g - tg) * j / span;
            int bb = tb2 + (b - tb2) * j / span;
            fb_fill_rect(tbx, tby + j, tbw, 1,
                         ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb);
        }
    } else {
        // Classic: flat titlebar (unchanged).
        fb_fill_rect(tbx, tby, tbw, TITLEBAR_HEIGHT, tb_col);
    }

    // #136: app icon then title text, both in the readable ink.
    int32_t title_x = x + BORDER_WIDTH + 4;
    int32_t title_y = y + BORDER_WIDTH + (TITLEBAR_HEIGHT - FONT_HEIGHT) / 2;
    {
        int32_t isz = 16;
        int32_t iy = y + BORDER_WIDTH + (TITLEBAR_HEIGHT - isz) / 2;
        icon_draw_scaled(window_title_icon(win->title), title_x, iy, isz, tb_ink);
        title_x += isz + 3;
    }
    // #162: crisp TTF title (smaller/lighter than the chunky bitmap font), with a
    // bitmap fallback if TTF is not ready.
    if (ttf_is_ready()) {
        int32_t tty = y + BORDER_WIDTH + (TITLEBAR_HEIGHT - 14) / 2;
        ttf_draw_string(title_x, tty, win->title, 14, tb_ink);
    } else {
        draw_text_transparent(title_x, title_y, win->title, tb_ink);
    }

    // Draw title bar buttons: [minimize][maximize][close]
    // All buttons are drawn for all windows
    int32_t btn_y = y + 2;

    // #165: transparency button (Zest Filter icon) - far left of the button group.
    {
        int32_t btn_x = x + w - 4 * CLOSE_BUTTON_SIZE - 2 - 3 * TITLEBAR_BUTTON_SPACING;
        fb_fill_rect(btn_x, btn_y, CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE, tb_col);
        if (!kicon_blit("FILTER", btn_x + 2, btn_y + 2, 12, tb_ink, tb_col)) {
            // fallback: a small funnel/triangle
            int32_t cx = btn_x + CLOSE_BUTTON_SIZE / 2;
            int32_t cy = btn_y + CLOSE_BUTTON_SIZE / 2;
            for (int i = 0; i < 5; i++) fb_put_pixel(cx - 4 + i, cy - 3 + i, tb_ink);
            for (int i = 0; i < 5; i++) fb_put_pixel(cx + 4 - i, cy - 3 + i, tb_ink);
            for (int i = 0; i < 3; i++) fb_put_pixel(cx, cy + 1 + i, tb_ink);
        }
    }

    // #165: minimize button (Zest minus icon).
    {
        int32_t btn_x = x + w - 3 * CLOSE_BUTTON_SIZE - 2 - 2 * TITLEBAR_BUTTON_SPACING;
        fb_fill_rect(btn_x, btn_y, CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE, tb_col);
        if (!kicon_blit("MINUS", btn_x + 2, btn_y + 2, 12, tb_ink, tb_col)) {
            int32_t cx = btn_x + CLOSE_BUTTON_SIZE / 2;
            int32_t cy = btn_y + CLOSE_BUTTON_SIZE / 2;
            for (int i = -4; i <= 4; i++) fb_put_pixel(cx + i, cy, tb_ink);
        }
    }

    // Draw maximize button - middle
    {
        int32_t btn_x = x + w - 2 * CLOSE_BUTTON_SIZE - 2 - TITLEBAR_BUTTON_SPACING;
        fb_fill_rect(btn_x, btn_y, CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE, tb_col);

        // Draw square outline on maximize button
        int32_t cx = btn_x + CLOSE_BUTTON_SIZE / 2;
        int32_t cy = btn_y + CLOSE_BUTTON_SIZE / 2;
        // Draw a small square (4x4 outline)
        for (int i = -3; i <= 3; i++) {
            fb_put_pixel(cx + i, cy - 3, tb_ink);  // Top
            fb_put_pixel(cx + i, cy + 3, tb_ink);  // Bottom
            fb_put_pixel(cx - 3, cy + i, tb_ink);  // Left
            fb_put_pixel(cx + 3, cy + i, tb_ink);  // Right
        }
    }

    // Draw close button - rightmost
    if (win->flags & WINDOW_FLAG_CLOSABLE) {
        int32_t btn_x = x + w - CLOSE_BUTTON_SIZE - 2;
        uint32_t close_bg = THEME_CLOSE_BUTTON;
        fb_fill_rect(btn_x, btn_y, CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE, close_bg);
        // #165: Zest X (xmark) icon, tinted white over the red close button.
        if (!kicon_blit("XMARK", btn_x + 2, btn_y + 2, 12, 0x00FFFFFF, close_bg)) {
            int32_t cx = btn_x + CLOSE_BUTTON_SIZE / 2;
            int32_t cy = btn_y + CLOSE_BUTTON_SIZE / 2;
            for (int i = -4; i <= 4; i++) {
                fb_put_pixel(cx + i, cy + i, tb_ink);
                fb_put_pixel(cx + i, cy - i, tb_ink);
            }
        }
    }

    // Draw window content area
    int32_t content_x, content_y, content_w, content_h;
    window_get_content_bounds(win, &content_x, &content_y, &content_w, &content_h);
    // For a transparent window, do NOT pre-fill the content with the opaque bg
    // color: leave whatever is behind the window (wallpaper / lower windows) so
    // the content blit can alpha-blend against it (true see-through, not a
    // washed-out blend against bg_color).
    if (win->opacity >= 255)
        fb_fill_rect(content_x, content_y, content_w, content_h, ov_bg);

    // Draw all widgets
    widget_t *widget = win->widgets;
    while (widget) {
        widget_draw(widget);
        widget = widget->next;
    }

    // Draw resize grips (small triangular indicators in all 4 corners)
    if (win->flags & WINDOW_FLAG_RESIZABLE) {
        // Bottom-right corner (3 diagonal lines)
        int32_t grip_x = x + w - RESIZE_GRIP_SIZE;
        int32_t grip_y = y + h - RESIZE_GRIP_SIZE;
        for (int i = 0; i < 3; i++) {
            fb_put_pixel(grip_x + 7 + i, grip_y + 7 - i, 0x00FFFFFF);
            fb_put_pixel(grip_x + 8 + i, grip_y + 8 - i, 0x00808080);
        }
        for (int i = 0; i < 5; i++) {
            fb_put_pixel(grip_x + 4 + i, grip_y + 7 - i, 0x00FFFFFF);
            fb_put_pixel(grip_x + 5 + i, grip_y + 8 - i, 0x00808080);
        }
        for (int i = 0; i < 7; i++) {
            fb_put_pixel(grip_x + 1 + i, grip_y + 7 - i, 0x00FFFFFF);
            fb_put_pixel(grip_x + 2 + i, grip_y + 8 - i, 0x00808080);
        }

        // Top-left corner (small triangle)
        for (int i = 0; i < 4; i++) {
            fb_put_pixel(x + 2 + i, y + TITLEBAR_HEIGHT + 2 + i, 0x00808080);
            fb_put_pixel(x + 2, y + TITLEBAR_HEIGHT + 2 + i, 0x00808080);
        }

        // Top-right corner (small triangle)
        for (int i = 0; i < 4; i++) {
            fb_put_pixel(x + w - 3 - i, y + TITLEBAR_HEIGHT + 2 + i, 0x00808080);
            fb_put_pixel(x + w - 3, y + TITLEBAR_HEIGHT + 2 + i, 0x00808080);
        }

        // Bottom-left corner (small triangle)
        for (int i = 0; i < 4; i++) {
            fb_put_pixel(x + 2 + i, y + h - 3 - i, 0x00808080);
            fb_put_pixel(x + 2, y + h - 3 - i, 0x00808080);
        }
    }
}

// Invalidate window (mark for redraw)
void window_invalidate(window_t *win) {
    // For now, just redraw immediately
    window_draw(win);
}

// Draw all windows (back to front)
void wm_draw_all(void) {
    extern int g_win_blit_suppressed;
    if (g_win_blit_suppressed) return;   // screensaver owns the FB; don't draw window frames
    // Find the last window (lowest z-order)
    window_t *win = wm_state.window_list;
    while (win && win->next) {
        win = win->next;
    }

    // Draw from back to front
    while (win) {
        window_draw(win);
        win = win->prev;
    }
    // Task A: the decorator popup is NOT drawn here. It must render on top of
    // app content, so desktop_run() calls wm_draw_winmenu() after wm_draw_apps().
}

// Redraw entire screen
void wm_redraw_screen(void) {
    // Clear background
    fb_clear(0x00008080);  // Teal desktop background

    // Draw all windows
    wm_draw_all();
}

// Draw text in window coordinates (relative to content area)
void window_draw_text(window_t *win, int32_t x, int32_t y, const char *text, uint32_t color) {
    if (!win || !text) return;

    int32_t content_x, content_y, content_w, content_h;
    window_get_content_bounds(win, &content_x, &content_y, &content_w, &content_h);

    draw_text_transparent(content_x + x, content_y + y, text, color);
}

// Process events (placeholder - integrate with actual input system)
void wm_process_events(void) {
    // This function should be called from the main loop
    // It would poll the mouse/keyboard state and call the appropriate handlers
    // For now, it's a placeholder that applications can call
}

// ============================================================================
// Event Queue Implementation
// ============================================================================

void wm_queue_event(gui_event_t *event) {
    if (!event) return;

    event_queue_t *q = &wm_state.event_queue;

    // Check if queue is full
    if (q->count >= EVENT_QUEUE_SIZE) {
        // Drop oldest event to make room
        q->head = (q->head + 1) % EVENT_QUEUE_SIZE;
        q->count--;
    }

    // Add event to tail
    q->events[q->tail] = *event;
    q->tail = (q->tail + 1) % EVENT_QUEUE_SIZE;
    q->count++;
}

// Build a content mouse event from primitive args and dispatch it to the
// window under the cursor (its app on_event -> per-window queue). Used by the
// compositor's sys_inject_mouse so userland apps receive content clicks while
// the kernel desktop loop is idle in exclusive mode.
void wm_inject_app_mouse(int32_t x, int32_t y, int32_t type, uint32_t button) {
    gui_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.mouse_x = x;
    ev.mouse_y = y;
    ev.mouse_buttons = (button == 2) ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_LEFT;
    if      (type == 0) ev.type = EVENT_MOUSE_MOVE;
    else if (type == 1) ev.type = EVENT_MOUSE_DOWN;
    else if (type == 2) ev.type = EVENT_MOUSE_UP;
    else return;
    wm_dispatch_event(&ev);
}

// OS-wide mouse wheel: dispatch an EVENT_MOUSE_SCROLL to the window under the
// cursor. The user_window_event_handler translates mouse_x/y to content coords.
void wm_inject_app_scroll(int32_t x, int32_t y, int32_t delta) {
    if (delta == 0) return;
    gui_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EVENT_MOUSE_SCROLL;
    ev.mouse_x = x;
    ev.mouse_y = y;
    ev.scroll_delta = (int8_t)delta;
    wm_dispatch_event(&ev);
}

bool wm_poll_event(gui_event_t *event) {
    if (!event) return false;

    event_queue_t *q = &wm_state.event_queue;

    if (q->count == 0) {
        return false;
    }

    // Get event from head
    *event = q->events[q->head];
    q->head = (q->head + 1) % EVENT_QUEUE_SIZE;
    q->count--;

    return true;
}

void wm_clear_event_queue(void) {
    wm_state.event_queue.head = 0;
    wm_state.event_queue.tail = 0;
    wm_state.event_queue.count = 0;
}

// ============================================================================
// App Registration Implementation
// ============================================================================

int wm_register_app(window_t *win, void *app_data,
                    app_event_handler_t on_event,
                    app_draw_handler_t on_draw,
                    app_destroy_handler_t on_destroy) {
    // Find a free slot
    int app_id = -1;
    for (int i = 0; i < MAX_APPS; i++) {
        if (!wm_state.apps[i].active) {
            app_id = i;
            break;
        }
    }

    if (app_id < 0) {
        kprintf("[WM] Error: No free app slots\n");
        return -1;
    }

    app_registration_t *app = &wm_state.apps[app_id];
    app->window = win;
    app->app_data = app_data;
    app->on_event = on_event;
    app->on_draw = on_draw;
    app->on_destroy = on_destroy;
    app->active = true;

    kprintf("[WM] Registered app %d with window '%s'\n", app_id, win ? win->title : "NULL");
    return app_id;
}

void wm_unregister_app(int app_id) {
    if (app_id < 0 || app_id >= MAX_APPS) return;

    app_registration_t *app = &wm_state.apps[app_id];
    if (!app->active) return;

    kprintf("[WM] Unregistering app %d\n", app_id);

    // Call destroy handler if set
    if (app->on_destroy && app->app_data) {
        app->on_destroy(app->app_data);
    }

    // Clear the registration
    app->window = NULL;
    app->app_data = NULL;
    app->on_event = NULL;
    app->on_draw = NULL;
    app->on_destroy = NULL;
    app->active = false;

    // Mark screen as dirty to remove the window
    wm_invalidate_all();
}

app_registration_t *wm_get_app_by_window(window_t *win) {
    if (!win) return NULL;

    for (int i = 0; i < MAX_APPS; i++) {
        if (wm_state.apps[i].active && wm_state.apps[i].window == win) {
            return &wm_state.apps[i];
        }
    }
    return NULL;
}

app_registration_t *wm_get_app_by_id(int app_id) {
    if (app_id < 0 || app_id >= MAX_APPS) return NULL;
    if (!wm_state.apps[app_id].active) return NULL;
    return &wm_state.apps[app_id];
}

// Get count of all windows
int wm_get_window_count(void) {
    int count = 0;
    window_t *win = wm_state.window_list;
    while (win) {
        count++;
        win = win->next;
    }
    return count;
}

// Get window at index (0 = front/top, higher = back)
window_t *wm_get_window_at_index(int index) {
    if (index < 0) return NULL;

    window_t *win = wm_state.window_list;
    int i = 0;
    while (win) {
        if (i == index) return win;
        i++;
        win = win->next;
    }
    return NULL;
}

// Focus a window (brings to front and sets focus)
void wm_focus_window(window_t *win) {
    if (!win) return;
    // Fast-path no-op: if this window is ALREADY focused and ALREADY frontmost,
    // there is nothing to change. A fullscreen app (Arena / Squadron) re-asserts
    // focus every frame so a background window-create can never steal its keyboard
    // focus and so it stays above overlay docks (the AI Chat edge dock). Without
    // this guard, wm_invalidate_all() would force a full recomposite every frame
    // on slow hardware (iMac); with it, the per-frame call costs nothing until
    // focus/z-order actually changes.
    if (wm_state.focused_window == win && wm_state.window_list == win) return;
    window_bring_to_front(win);
    window_set_focus(win);
    wm_invalidate_all();
}

// Close a window through app registration (triggers proper cleanup)
void window_close(window_t *win) {
    if (!win) return;

    // Find app registration for this window
    for (int i = 0; i < MAX_APPS; i++) {
        if (wm_state.apps[i].active && wm_state.apps[i].window == win) {
            // Unregister app (will call destroy handler)
            wm_unregister_app(i);
            return;
        }
    }

    // No app registration found, just destroy the window directly
    window_destroy(win);
}

// ============================================================================
// Dirty Region Implementation
// ============================================================================

void wm_invalidate_rect(const rect_t *rect) {
    if (!rect) return;

    dirty_region_t *d = &wm_state.dirty;

    // If already full redraw, no need to track individual rects
    if (d->full_redraw) return;

    // If we've reached max rects, just do a full redraw
    if (d->count >= MAX_DIRTY_RECTS) {
        d->full_redraw = true;
        return;
    }

    // Add the rect
    d->rects[d->count++] = *rect;
}

void wm_invalidate_all(void) {
    wm_state.dirty.full_redraw = true;
}

bool wm_is_dirty(void) {
    return wm_state.dirty.full_redraw || wm_state.dirty.count > 0;
}

void wm_clear_dirty(void) {
    wm_state.dirty.count = 0;
    wm_state.dirty.full_redraw = false;
}

const dirty_region_t *wm_get_dirty_region(void) {
    return &wm_state.dirty;
}

// ============================================================================
// Event Dispatch Implementation
// ============================================================================

void wm_dispatch_event(gui_event_t *event) {
    if (!event) return;

    // Find target window
    window_t *target = NULL;

    if (event->type == EVENT_MOUSE_MOVE ||
        event->type == EVENT_MOUSE_DOWN ||
        event->type == EVENT_MOUSE_UP ||
        event->type == EVENT_MOUSE_SCROLL) {
        // For mouse events, find window at mouse position
        target = window_get_at_point(event->mouse_x, event->mouse_y);
    } else if (event->type == EVENT_KEY_DOWN ||
               event->type == EVENT_KEY_UP) {
        // For keyboard events, send to focused window
        target = wm_state.focused_window;
    } else if (event->target_id != 0) {
        // Find window by ID
        window_t *win = wm_state.window_list;
        while (win) {
            if (win->id == event->target_id) {
                target = win;
                break;
            }
            win = win->next;
        }
    }

    // Find registered app for this window
    if (target) {
        app_registration_t *app = wm_get_app_by_window(target);
        if (app && app->on_event) {
            app->on_event(app->app_data, event);
        }
    }
}

// ============================================================================
// Main Frame Processing
// ============================================================================

bool wm_process_frame(void) {
    // Process all queued events
    gui_event_t event;
    while (wm_poll_event(&event)) {
        // First let window manager handle it (for dragging, focus, etc)
        switch (event.type) {
            case EVENT_MOUSE_DOWN:
                wm_handle_mouse_down(event.mouse_x, event.mouse_y, event.mouse_buttons);
                break;
            case EVENT_MOUSE_UP:
                wm_handle_mouse_up(event.mouse_x, event.mouse_y, event.mouse_buttons);
                break;
            case EVENT_MOUSE_MOVE:
                wm_handle_mouse_move(event.mouse_x, event.mouse_y);
                break;
            case EVENT_KEY_DOWN:
                wm_handle_key_down(event.keycode, event.key_char);
                break;
            case EVENT_KEY_UP:
                wm_handle_key_up(event.keycode);
                break;
            default:
                break;
        }

        // Then dispatch to the app
        wm_dispatch_event(&event);
    }

    // Call draw handlers for all active apps if dirty (in z-order)
    if (wm_is_dirty()) {
        // Draw all app windows in z-order (back to front)
        wm_draw_apps();
    }

    return wm_state.running;
}

// Draw all registered app contents (in z-order, back to front)
void wm_draw_apps(void) {
    // Draw all windows back to front, with both decorations and app content
    // per window. This ensures the focused (front) window decorations are
    // never obscured by a background window app content.
    window_t *win = wm_state.window_list;
    while (win && win->next) {
        win = win->next;
    }

    while (win) {
        if (win->flags & WINDOW_FLAG_VISIBLE) {
            // Draw decorations (border, title bar, buttons, content bg)
            window_draw(win);
            // Draw app content on top
            app_registration_t *app = wm_get_app_by_window(win);
            if (app && app->on_draw) {
                app->on_draw(app->app_data);
            }
        }
        win = win->prev;
    }
}


// ============================================================================
// Compositor syscall: enumerate visible windows
// ============================================================================

int64_t sys_wm_focus_window(int id) {
    window_t *win = wm_state.window_list;
    while (win) {
        if ((int)win->id == id) {
            if (win->flags & WINDOW_FLAG_MINIMIZED) window_restore(win);
            wm_focus_window(win);
            return 0;
        }
        win = win->next;
    }
    return -1;
}

int64_t sys_wm_minimize_window(int id) {
    window_t *win = wm_state.window_list;
    while (win) {
        if ((int)win->id == id) {
            window_minimize(win);
            return 0;
        }
        win = win->next;
    }
    return -1;
}

int64_t sys_wm_get_windows(wm_window_info_t *buf, int max_count) {
    if (!buf || max_count <= 0) return -1;
    int n = 0;
    window_t *win = wm_state.window_list;
    while (win && n < max_count) {
        buf[n].id      = (int)win->id;
        buf[n].x       = (int)win->bounds.x;
        buf[n].y       = (int)win->bounds.y;
        buf[n].width   = (int)win->bounds.width;
        buf[n].height  = (int)win->bounds.height;
        buf[n].visible   = (win->flags & WINDOW_FLAG_VISIBLE)   ? 1 : 0;
        buf[n].minimized = (win->flags & WINDOW_FLAG_MINIMIZED) ? 1 : 0;
        buf[n].focused   = (win->flags & WINDOW_FLAG_FOCUSED)   ? 1 : 0;
        int ti = 0;
        const char *src = win->title;
        while (ti < 63 && src[ti]) { buf[n].title[ti] = src[ti]; ti++; }
        buf[n].title[ti] = '\0';
        n++;
        win = win->next;
    }
    return (int64_t)n;
}
