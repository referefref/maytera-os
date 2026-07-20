// compositor.h - Userland Compositor for MayteraOS
// Manages all windows and compositing in Ring 3
#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

// ============================================================================
// Constants
// ============================================================================
#define MAX_COMPOSITOR_WINDOWS      64
#define MAX_COMPOSITOR_CLIENTS      32
#define COMPOSITOR_FRAME_RATE       60
#define COMPOSITOR_FRAME_MS         (1000 / COMPOSITOR_FRAME_RATE)

// ============================================================================
// Window State (server-side)
// ============================================================================
typedef struct comp_window {
    uint32_t id;                    // Unique window ID
    uint32_t client_id;             // Owning client
    char title[128];                // Window title
    
    // Geometry
    int32_t x, y;                   // Position
    int32_t width, height;          // Size
    int32_t stored_x, stored_y;     // For restore from maximize
    int32_t stored_w, stored_h;
    
    // State
    uint32_t flags;                 // Window flags
    uint32_t z_order;               // Stacking order
    
    // Buffer
    int32_t shm_id;                 // Shared memory ID
    uint32_t *buffer;               // Mapped buffer pointer
    int32_t buf_width, buf_height;
    int32_t buf_stride;
    
    // Decoration
    bool decorated;                 // Has window decorations
    int32_t titlebar_height;
    int32_t border_width;
    
    // Linked list
    struct comp_window *next;
    struct comp_window *prev;
} comp_window_t;

// ============================================================================
// Client State
// ============================================================================
typedef struct {
    uint32_t id;                    // Client ID
    int conn_id;                    // IPC connection ID
    char name[64];                  // Application name
    bool connected;                 // Is connected
    int num_windows;                // Number of windows owned
} comp_client_t;

// ============================================================================
// Compositor State
// ============================================================================
typedef struct {
    // Screen info
    int32_t screen_width;
    int32_t screen_height;
    uint32_t *framebuffer;          // Direct framebuffer access
    
    // Windows
    comp_window_t *windows;         // Linked list (z-order)
    comp_window_t *focused;         // Focused window
    uint32_t next_window_id;
    uint32_t highest_z;
    
    // Clients
    comp_client_t clients[MAX_COMPOSITOR_CLIENTS];
    uint32_t next_client_id;
    
    // Input state
    int32_t mouse_x, mouse_y;
    uint32_t mouse_buttons;
    
    // Drag state
    comp_window_t *drag_window;
    int32_t drag_offset_x, drag_offset_y;
    
    // Resize state
    comp_window_t *resize_window;
    int32_t resize_edge;
    int32_t resize_orig_x, resize_orig_y;
    int32_t resize_orig_w, resize_orig_h;
    
    // IPC
    int channel_id;                 // Compositor channel
    
    // Flags
    bool running;
    bool needs_redraw;
    
    // Stats
    uint64_t frame_count;
    uint64_t last_frame_time;
} compositor_t;

// ============================================================================
// Compositor API
// ============================================================================

// Initialize the compositor
int compositor_init(void);

// Run compositor main loop
void compositor_run(void);

// Shutdown compositor
void compositor_shutdown(void);

// ============================================================================
// Window Management
// ============================================================================

// Create a window for a client
comp_window_t *compositor_window_create(uint32_t client_id, const char *title,
                                         int32_t x, int32_t y,
                                         int32_t width, int32_t height,
                                         uint32_t flags);

// Destroy a window
void compositor_window_destroy(comp_window_t *win);

// Get window by ID
comp_window_t *compositor_window_get(uint32_t window_id);

// Focus a window
void compositor_window_focus(comp_window_t *win);

// Bring window to front
void compositor_window_raise(comp_window_t *win);

// Move window
void compositor_window_move(comp_window_t *win, int32_t x, int32_t y);

// Resize window
void compositor_window_resize(comp_window_t *win, int32_t width, int32_t height);

// ============================================================================
// Client Management
// ============================================================================

// Register a new client
uint32_t compositor_client_register(int conn_id, const char *name);

// Unregister a client
void compositor_client_unregister(uint32_t client_id);

// ============================================================================
// Rendering
// ============================================================================

// Composite all windows to framebuffer
void compositor_render(void);

// Mark region as damaged
void compositor_damage(int32_t x, int32_t y, int32_t width, int32_t height);

// Mark full screen as damaged
void compositor_damage_all(void);

// ============================================================================
// Input Handling
// ============================================================================

// Handle mouse input
void compositor_handle_mouse(int32_t x, int32_t y, uint32_t buttons);

// Handle keyboard input
void compositor_handle_key(uint32_t keycode, uint32_t scancode, bool pressed, uint32_t modifiers);

// ============================================================================
// Internal Helpers
// ============================================================================

// Find window at coordinates
comp_window_t *compositor_window_at(int32_t x, int32_t y);

// Check if point is in titlebar
bool compositor_in_titlebar(comp_window_t *win, int32_t x, int32_t y);

// Check if point is on resize edge
int compositor_resize_edge_at(comp_window_t *win, int32_t x, int32_t y);

#endif // COMPOSITOR_H
