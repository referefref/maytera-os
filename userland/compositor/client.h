// client.h - Client Library for MayteraOS Compositor
// This library is used by userland applications to create windows and interact
// with the compositor via IPC

#ifndef COMPOSITOR_CLIENT_H
#define COMPOSITOR_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

// ============================================================================
// Window Handle
// ============================================================================

typedef struct client_window {
    uint32_t id;                    // Window ID from compositor
    int32_t x, y;                   // Current position
    int32_t width, height;          // Current size
    uint32_t flags;                 // Window flags
    
    int32_t shm_id;                 // Shared memory ID
    uint32_t *buffer;               // Window buffer (mapped shared memory)
    int32_t buf_stride;             // Bytes per row
    
    // Event callbacks
    void (*on_close)(struct client_window *);
    void (*on_resize)(struct client_window *, int32_t width, int32_t height);
    void (*on_focus)(struct client_window *, bool focused);
    void (*on_mouse_move)(struct client_window *, int32_t x, int32_t y, uint32_t buttons);
    void (*on_mouse_button)(struct client_window *, int32_t x, int32_t y, int button, bool pressed);
    void (*on_key)(struct client_window *, uint32_t keycode, bool pressed, uint32_t mods);
    
    void *user_data;                // Application data
} client_window_t;

// ============================================================================
// Connection to Compositor
// ============================================================================

typedef struct {
    int conn_id;                    // IPC connection to compositor
    uint32_t client_id;             // Assigned client ID
    int32_t screen_width;           // Screen dimensions
    int32_t screen_height;
    bool connected;
    
    // Window tracking
    client_window_t *windows[64];   // Active windows
    int num_windows;
    
    // Pending events
    uint8_t event_buffer[4096];
    int event_len;
} compositor_connection_t;

// ============================================================================
// API
// ============================================================================

/**
 * Connect to the compositor
 * @param app_name      Application name for identification
 * @return              Connection handle, or NULL on failure
 */
compositor_connection_t *compositor_connect(const char *app_name);

/**
 * Disconnect from compositor
 * All windows are automatically destroyed
 */
void compositor_disconnect(compositor_connection_t *conn);

/**
 * Create a window
 * @param conn          Compositor connection
 * @param title         Window title
 * @param x, y          Position (-1 for auto-placement)
 * @param width, height Size
 * @param flags         Window flags
 * @return              Window handle, or NULL on failure
 */
client_window_t *window_create(compositor_connection_t *conn,
                               const char *title,
                               int32_t x, int32_t y,
                               int32_t width, int32_t height,
                               uint32_t flags);

/**
 * Destroy a window
 */
void window_destroy(client_window_t *win);

/**
 * Show a window
 */
void window_show(client_window_t *win);

/**
 * Hide a window
 */
void window_hide(client_window_t *win);

/**
 * Move a window
 */
void window_move(client_window_t *win, int32_t x, int32_t y);

/**
 * Resize a window
 */
void window_resize(client_window_t *win, int32_t width, int32_t height);

/**
 * Set window title
 */
void window_set_title(client_window_t *win, const char *title);

/**
 * Request focus
 */
void window_focus(client_window_t *win);

/**
 * Mark window content as modified and request redraw
 * Call after drawing to the window buffer
 */
void window_commit(client_window_t *win);

/**
 * Mark a region as damaged
 */
void window_damage(client_window_t *win, int32_t x, int32_t y, int32_t w, int32_t h);

// ============================================================================
// Drawing Helpers
// ============================================================================

/**
 * Fill rectangle in window buffer
 */
void window_fill_rect(client_window_t *win, int32_t x, int32_t y,
                      int32_t width, int32_t height, uint32_t color);

/**
 * Draw pixel in window buffer
 */
static inline void window_put_pixel(client_window_t *win, int32_t x, int32_t y, uint32_t color) {
    if (x >= 0 && x < win->width && y >= 0 && y < win->height) {
        win->buffer[y * win->width + x] = color;
    }
}

/**
 * Clear window to color
 */
void window_clear(client_window_t *win, uint32_t color);

// ============================================================================
// Event Loop
// ============================================================================

/**
 * Process pending events (non-blocking)
 * Calls event callbacks for each pending event
 * @return              Number of events processed
 */
int compositor_process_events(compositor_connection_t *conn);

/**
 * Wait for and process events (blocking)
 * @param timeout       Timeout in milliseconds (-1 = infinite)
 * @return              Number of events processed, 0 on timeout
 */
int compositor_wait_events(compositor_connection_t *conn, int timeout);

/**
 * Run event loop until all windows closed
 */
void compositor_main_loop(compositor_connection_t *conn);

#endif // COMPOSITOR_CLIENT_H
