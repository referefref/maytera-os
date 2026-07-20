// compositor_client.h - Window compositor IPC client for MayteraOS
// Provides IPC-based window protocol for user-mode applications
//
// This client connects to a user-mode compositor via message passing
// and uses shared memory for efficient buffer transfer.
//
// Protocol Messages (client -> compositor):
//   MSG_CREATE_WINDOW   - Create a new window
//   MSG_DESTROY_WINDOW  - Destroy a window
//   MSG_PRESENT_BUFFER  - Present framebuffer to screen
//   MSG_SET_TITLE       - Change window title
//   MSG_RESIZE          - Resize window
//   MSG_MOVE            - Move window position
//   MSG_SET_FLAGS       - Set window flags (visible, resizable, etc.)
//
// Protocol Events (compositor -> client):
//   EVENT_INPUT         - Keyboard/mouse input
//   EVENT_WINDOW_CLOSE  - Window close request
//   EVENT_RESIZE        - Window was resized
//   EVENT_FOCUS         - Window gained/lost focus
//   EVENT_EXPOSE        - Window needs redraw

#ifndef _COMPOSITOR_CLIENT_H
#define _COMPOSITOR_CLIENT_H

#include "types.h"
#include "ipc.h"

// ============================================================================
// Compositor Protocol Constants
// ============================================================================

// Well-known channel ID for compositor service
// In production, this would be discovered via a name service
#define COMPOSITOR_CHANNEL_ID   100

// Maximum title length
#define COMP_MAX_TITLE          64

// Maximum shared memory regions per window
#define COMP_MAX_BUFFERS        2

// Default double-buffering
#define COMP_DEFAULT_BUFFERS    2

// ============================================================================
// Protocol Message Types (Client -> Compositor)
// ============================================================================

typedef enum {
    MSG_NONE = 0,
    
    // Window management
    MSG_CREATE_WINDOW       = 1,    // Create new window
    MSG_DESTROY_WINDOW      = 2,    // Destroy window
    MSG_SET_TITLE           = 3,    // Change window title
    MSG_RESIZE              = 4,    // Resize window
    MSG_MOVE                = 5,    // Move window
    MSG_SET_FLAGS           = 6,    // Set window flags
    MSG_SHOW                = 7,    // Show window
    MSG_HIDE                = 8,    // Hide window
    MSG_MINIMIZE            = 9,    // Minimize window
    MSG_MAXIMIZE            = 10,   // Maximize window
    MSG_RESTORE             = 11,   // Restore from minimize/maximize
    
    // Buffer management
    MSG_CREATE_BUFFER       = 20,   // Create shared memory buffer
    MSG_DESTROY_BUFFER      = 21,   // Destroy buffer
    MSG_PRESENT_BUFFER      = 22,   // Present buffer to screen
    MSG_DAMAGE_RECT         = 23,   // Mark region as damaged
    
    // Input focus
    MSG_REQUEST_FOCUS       = 30,   // Request input focus
    MSG_RELEASE_FOCUS       = 31,   // Release input focus
    
    // Clipboard
    MSG_CLIPBOARD_COPY      = 40,   // Copy to clipboard
    MSG_CLIPBOARD_PASTE     = 41,   // Paste from clipboard
    
    // Session
    MSG_CONNECT             = 50,   // Initial connection handshake
    MSG_DISCONNECT          = 51,   // Graceful disconnect
    MSG_PING                = 52,   // Keep-alive ping
    MSG_PONG                = 53,   // Keep-alive response
    
} comp_msg_type_t;

// ============================================================================
// Protocol Event Types (Compositor -> Client)
// ============================================================================

typedef enum {
    COMP_EVENT_NONE = 0,
    
    // Input events
    COMP_EVENT_KEY_DOWN     = 100,
    COMP_EVENT_KEY_UP       = 101,
    COMP_EVENT_MOUSE_MOVE   = 102,
    COMP_EVENT_MOUSE_DOWN   = 103,
    COMP_EVENT_MOUSE_UP     = 104,
    COMP_EVENT_MOUSE_SCROLL = 105,
    
    // Window events
    COMP_EVENT_CLOSE        = 110,  // Window close requested
    COMP_EVENT_RESIZE       = 111,  // Window was resized
    COMP_EVENT_MOVE         = 112,  // Window was moved
    COMP_EVENT_FOCUS        = 113,  // Window gained focus
    COMP_EVENT_BLUR         = 114,  // Window lost focus
    COMP_EVENT_EXPOSE       = 115,  // Window needs redraw
    
    // Buffer events
    COMP_EVENT_BUFFER_RELEASE = 120, // Buffer released, can reuse
    
    // Connection events
    COMP_EVENT_CONNECTED    = 130,  // Connection established
    COMP_EVENT_DISCONNECTED = 131,  // Connection lost
    
} comp_event_type_t;

// ============================================================================
// Window Flags
// ============================================================================

#define COMP_FLAG_VISIBLE       (1 << 0)
#define COMP_FLAG_RESIZABLE     (1 << 1)
#define COMP_FLAG_MOVABLE       (1 << 2)
#define COMP_FLAG_CLOSABLE      (1 << 3)
#define COMP_FLAG_MINIMIZABLE   (1 << 4)
#define COMP_FLAG_MAXIMIZABLE   (1 << 5)
#define COMP_FLAG_DECORATED     (1 << 6)    // Has title bar/borders
#define COMP_FLAG_ALWAYS_ON_TOP (1 << 7)
#define COMP_FLAG_FULLSCREEN    (1 << 8)
#define COMP_FLAG_TRANSPARENT   (1 << 9)

// Default window flags
#define COMP_FLAGS_DEFAULT  (COMP_FLAG_VISIBLE | COMP_FLAG_RESIZABLE | \
                             COMP_FLAG_MOVABLE | COMP_FLAG_CLOSABLE | \
                             COMP_FLAG_MINIMIZABLE | COMP_FLAG_MAXIMIZABLE | \
                             COMP_FLAG_DECORATED)

// ============================================================================
// Protocol Message Structures
// ============================================================================

// Base message header
typedef struct {
    comp_msg_type_t type;       // Message type
    uint32_t        window_id;  // Window ID (0 for session messages)
    uint32_t        seq;        // Sequence number for request/response matching
    uint32_t        size;       // Total message size including header
} comp_msg_header_t;

// MSG_CONNECT request
typedef struct {
    comp_msg_header_t header;
    uint32_t protocol_version;  // Protocol version (1)
    char client_name[32];       // Client application name
} comp_msg_connect_t;

// MSG_CONNECT response
typedef struct {
    comp_msg_header_t header;
    uint32_t client_id;         // Assigned client ID
    uint32_t screen_width;      // Screen dimensions
    uint32_t screen_height;
    uint32_t pixel_format;      // Pixel format (ARGB32, etc.)
} comp_msg_connect_response_t;

// MSG_CREATE_WINDOW request
typedef struct {
    comp_msg_header_t header;
    int32_t  x, y;              // Initial position (-1 for auto)
    uint32_t width, height;     // Initial size
    uint32_t flags;             // Window flags
    char     title[COMP_MAX_TITLE];
} comp_msg_create_window_t;

// MSG_CREATE_WINDOW response
typedef struct {
    comp_msg_header_t header;
    uint32_t window_id;         // Assigned window ID
    int32_t  result;            // 0 = success, negative = error
} comp_msg_create_window_response_t;

// MSG_SET_TITLE request
typedef struct {
    comp_msg_header_t header;
    char title[COMP_MAX_TITLE];
} comp_msg_set_title_t;

// MSG_RESIZE request
typedef struct {
    comp_msg_header_t header;
    uint32_t width, height;
} comp_msg_resize_t;

// MSG_MOVE request
typedef struct {
    comp_msg_header_t header;
    int32_t x, y;
} comp_msg_move_t;

// MSG_SET_FLAGS request
typedef struct {
    comp_msg_header_t header;
    uint32_t flags;
} comp_msg_set_flags_t;

// MSG_CREATE_BUFFER request
typedef struct {
    comp_msg_header_t header;
    uint32_t width, height;     // Buffer dimensions
    uint32_t format;            // Pixel format
    uint32_t shm_id;            // Shared memory region ID (created by client)
} comp_msg_create_buffer_t;

// MSG_CREATE_BUFFER response
typedef struct {
    comp_msg_header_t header;
    uint32_t buffer_id;         // Assigned buffer ID
    int32_t  result;            // 0 = success, negative = error
} comp_msg_create_buffer_response_t;

// MSG_PRESENT_BUFFER request
typedef struct {
    comp_msg_header_t header;
    uint32_t buffer_id;         // Buffer to present
    int32_t  x, y;              // Offset within window (usually 0,0)
    uint32_t width, height;     // Region to present
} comp_msg_present_buffer_t;

// MSG_DAMAGE_RECT request
typedef struct {
    comp_msg_header_t header;
    int32_t  x, y;
    uint32_t width, height;
} comp_msg_damage_rect_t;

// ============================================================================
// Protocol Event Structures
// ============================================================================

// Base event header
typedef struct {
    comp_event_type_t type;
    uint32_t window_id;
    uint64_t timestamp;         // Event timestamp (ms since boot)
} comp_event_header_t;

// Keyboard event
typedef struct {
    comp_event_header_t header;
    uint32_t keycode;           // Hardware keycode
    uint32_t keysym;            // Translated keysym
    char     character;         // UTF-8 character (if printable)
    uint8_t  modifiers;         // Shift, Ctrl, Alt, etc.
    uint8_t  _pad[2];
} comp_event_key_t;

// Mouse event
typedef struct {
    comp_event_header_t header;
    int32_t  x, y;              // Position relative to window
    int32_t  screen_x, screen_y; // Absolute screen position
    uint32_t buttons;           // Button state
    int8_t   scroll_x, scroll_y; // Scroll deltas
    uint8_t  _pad[2];
} comp_event_mouse_t;

// Window event (resize, move, focus, etc.)
typedef struct {
    comp_event_header_t header;
    int32_t  x, y;              // New position (for move)
    uint32_t width, height;     // New size (for resize)
    uint32_t flags;             // Additional flags
} comp_event_window_t;

// Buffer release event
typedef struct {
    comp_event_header_t header;
    uint32_t buffer_id;
} comp_event_buffer_t;

// Union of all event types
typedef union {
    comp_event_header_t header;
    comp_event_key_t    key;
    comp_event_mouse_t  mouse;
    comp_event_window_t window;
    comp_event_buffer_t buffer;
} comp_event_t;

// ============================================================================
// Client State Structures
// ============================================================================

// Window buffer for double-buffering
typedef struct {
    uint32_t buffer_id;         // Compositor buffer ID
    int      shm_id;            // Shared memory region ID
    uint32_t *pixels;           // Mapped pixel buffer
    uint32_t width, height;     // Buffer dimensions
    size_t   size;              // Buffer size in bytes
    bool     in_use;            // Currently being displayed
} comp_buffer_t;

// Forward declaration for event handler
struct comp_window;

// Event handler function type
typedef void (*comp_event_handler_t)(struct comp_window *win, comp_event_t *event);

// Client window state
typedef struct comp_window {
    uint32_t    window_id;      // Compositor window ID
    int32_t     x, y;           // Window position
    uint32_t    width, height;  // Window dimensions
    uint32_t    flags;          // Window flags
    char        title[COMP_MAX_TITLE];
    
    // Double-buffering
    comp_buffer_t buffers[COMP_MAX_BUFFERS];
    int           buffer_count;
    int           front_buffer; // Index of buffer being displayed
    int           back_buffer;  // Index of buffer being drawn to
    
    // Event callback
    comp_event_handler_t event_handler;
    void *user_data;
} comp_window_t;

// Client connection state
typedef struct {
    int         conn_id;        // IPC connection ID
    uint32_t    client_id;      // Assigned client ID
    uint32_t    screen_width;   // Screen dimensions
    uint32_t    screen_height;
    uint32_t    pixel_format;
    uint32_t    seq;            // Sequence number counter
    bool        connected;
} comp_client_t;

// ============================================================================
// Client API Functions
// ============================================================================

// Initialize the compositor client
// Returns: 0 on success, negative on error
int comp_init(void);

// Shutdown the compositor client
void comp_shutdown(void);

// Check if connected to compositor
bool comp_is_connected(void);

// Get screen dimensions
void comp_get_screen_size(uint32_t *width, uint32_t *height);

// ============================================================================
// Window Management
// ============================================================================

// Create a window
// title: Window title
// x, y: Initial position (-1 for auto-placement)
// width, height: Window dimensions
// flags: Window flags (COMP_FLAG_*)
// Returns: Window pointer on success, NULL on failure
comp_window_t *comp_window_create(const char *title, int x, int y, 
                                   uint32_t width, uint32_t height,
                                   uint32_t flags);

// Destroy a window
void comp_window_destroy(comp_window_t *win);

// Set window title
int comp_window_set_title(comp_window_t *win, const char *title);

// Move window
int comp_window_move(comp_window_t *win, int x, int y);

// Resize window
int comp_window_resize(comp_window_t *win, uint32_t width, uint32_t height);

// Show/hide window
int comp_window_show(comp_window_t *win);
int comp_window_hide(comp_window_t *win);

// Minimize/maximize/restore
int comp_window_minimize(comp_window_t *win);
int comp_window_maximize(comp_window_t *win);
int comp_window_restore(comp_window_t *win);

// Request input focus
int comp_window_request_focus(comp_window_t *win);

// Set window flags
int comp_window_set_flags(comp_window_t *win, uint32_t flags);

// ============================================================================
// Buffer Management
// ============================================================================

// Get the back buffer for drawing
// Returns pixel buffer pointer (ARGB32 format)
uint32_t *comp_window_get_buffer(comp_window_t *win);

// Get buffer dimensions
void comp_window_get_buffer_size(comp_window_t *win, uint32_t *width, uint32_t *height);

// Present the back buffer to screen (swaps buffers)
int comp_window_present(comp_window_t *win);

// Present only a damaged rectangle
int comp_window_present_rect(comp_window_t *win, int x, int y, 
                              uint32_t width, uint32_t height);

// Mark a rectangle as damaged (needs redraw)
int comp_window_damage(comp_window_t *win, int x, int y,
                        uint32_t width, uint32_t height);

// ============================================================================
// Event Handling
// ============================================================================

// Set event handler callback
void comp_window_set_handler(comp_window_t *win,
                              comp_event_handler_t handler,
                              void *user_data);

// Poll for events (non-blocking)
// Returns: event type, or COMP_EVENT_NONE if no events
int comp_poll_event(comp_event_t *event);

// Wait for events (blocking)
// timeout: 0 = non-blocking, >0 = wait up to timeout ms, -1 = wait forever
// Returns: event type, or COMP_EVENT_NONE on timeout
int comp_wait_event(comp_event_t *event, int timeout);

// Dispatch event to window handler
void comp_dispatch_event(comp_event_t *event);

// Main event loop (blocks until all windows closed)
void comp_main_loop(void);

// Request exit from main loop
void comp_exit_loop(void);

// ============================================================================
// Drawing Helpers (operate on back buffer)
// ============================================================================

// Clear window with color
void comp_window_clear(comp_window_t *win, uint32_t color);

// Fill rectangle
void comp_window_fill_rect(comp_window_t *win, int x, int y,
                            uint32_t width, uint32_t height, uint32_t color);

// Draw rectangle outline
void comp_window_draw_rect(comp_window_t *win, int x, int y,
                            uint32_t width, uint32_t height, uint32_t color);

// Draw pixel
void comp_window_draw_pixel(comp_window_t *win, int x, int y, uint32_t color);

// Draw horizontal line
void comp_window_draw_hline(comp_window_t *win, int x, int y, 
                             uint32_t width, uint32_t color);

// Draw vertical line
void comp_window_draw_vline(comp_window_t *win, int x, int y,
                             uint32_t height, uint32_t color);

// Blit image data
void comp_window_blit(comp_window_t *win, int x, int y,
                       uint32_t width, uint32_t height,
                       const uint32_t *pixels, uint32_t stride);

#endif // _COMPOSITOR_CLIENT_H
