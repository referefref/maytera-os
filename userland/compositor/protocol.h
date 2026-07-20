// protocol.h - Compositor-Client IPC Protocol for MayteraOS
// Defines message types for communication between userland apps and the compositor
#ifndef COMPOSITOR_PROTOCOL_H
#define COMPOSITOR_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Protocol Version
// ============================================================================
#define COMPOSITOR_PROTOCOL_VERSION     1

// ============================================================================
// Message Types (Client -> Compositor)
// ============================================================================
#define MSG_CLIENT_CONNECT              0x0001  // Register with compositor
#define MSG_CLIENT_DISCONNECT           0x0002  // Unregister from compositor
#define MSG_WINDOW_CREATE               0x0010  // Create new window
#define MSG_WINDOW_DESTROY              0x0011  // Destroy window
#define MSG_WINDOW_SHOW                 0x0012  // Show window
#define MSG_WINDOW_HIDE                 0x0013  // Hide window
#define MSG_WINDOW_MOVE                 0x0014  // Move window
#define MSG_WINDOW_RESIZE               0x0015  // Resize window
#define MSG_WINDOW_SET_TITLE            0x0016  // Set window title
#define MSG_WINDOW_MINIMIZE             0x0017  // Minimize window
#define MSG_WINDOW_MAXIMIZE             0x0018  // Maximize window
#define MSG_WINDOW_RESTORE              0x0019  // Restore window
#define MSG_WINDOW_FOCUS                0x001A  // Request focus
#define MSG_BUFFER_ATTACH               0x0020  // Attach shared memory buffer
#define MSG_BUFFER_DAMAGE               0x0021  // Mark region as damaged (needs redraw)
#define MSG_BUFFER_COMMIT               0x0022  // Commit buffer changes
#define MSG_CURSOR_SET                  0x0030  // Set cursor image
#define MSG_CURSOR_HIDE                 0x0031  // Hide cursor

// ============================================================================
// Message Types (Compositor -> Client)
// ============================================================================
#define MSG_COMPOSITOR_WELCOME          0x1001  // Connection accepted
#define MSG_COMPOSITOR_ERROR            0x1002  // Error response
#define MSG_WINDOW_CREATED              0x1010  // Window creation result
#define MSG_WINDOW_CONFIGURE            0x1011  // Window size/position changed
#define MSG_WINDOW_CLOSE_REQUEST        0x1012  // Close button clicked
#define MSG_WINDOW_FOCUS_IN             0x1013  // Window gained focus
#define MSG_WINDOW_FOCUS_OUT            0x1014  // Window lost focus
#define MSG_INPUT_MOUSE_MOVE            0x1020  // Mouse motion
#define MSG_INPUT_MOUSE_BUTTON          0x1021  // Mouse button press/release
#define MSG_INPUT_MOUSE_SCROLL          0x1022  // Mouse scroll wheel
#define MSG_INPUT_KEY_DOWN              0x1023  // Key press
#define MSG_INPUT_KEY_UP                0x1024  // Key release
#define MSG_FRAME_DONE                  0x1030  // Frame presented, buffer available

// ============================================================================
// Error Codes
// ============================================================================
#define COMP_ERR_NONE                   0
#define COMP_ERR_NO_MEMORY              1
#define COMP_ERR_INVALID_WINDOW         2
#define COMP_ERR_INVALID_BUFFER         3
#define COMP_ERR_PERMISSION_DENIED      4
#define COMP_ERR_PROTOCOL_ERROR         5
#define COMP_ERR_DISCONNECTED           6
#define COMP_ERR_TOO_MANY_WINDOWS       7

// ============================================================================
// Window Flags
// ============================================================================
#define WIN_FLAG_VISIBLE                (1 << 0)
#define WIN_FLAG_FOCUSED                (1 << 1)
#define WIN_FLAG_MINIMIZED              (1 << 2)
#define WIN_FLAG_MAXIMIZED              (1 << 3)
#define WIN_FLAG_FULLSCREEN             (1 << 4)
#define WIN_FLAG_RESIZABLE              (1 << 5)
#define WIN_FLAG_DECORATED              (1 << 6)
#define WIN_FLAG_NO_TASKBAR             (1 << 7)  // Don't show in taskbar

// ============================================================================
// Mouse Button Masks
// ============================================================================
#define MOUSE_BTN_LEFT                  (1 << 0)
#define MOUSE_BTN_RIGHT                 (1 << 1)
#define MOUSE_BTN_MIDDLE                (1 << 2)

// ============================================================================
// Keyboard Modifier Masks
// ============================================================================
#define KEY_MOD_SHIFT                   (1 << 0)
#define KEY_MOD_CTRL                    (1 << 1)
#define KEY_MOD_ALT                     (1 << 2)
#define KEY_MOD_SUPER                   (1 << 3)
#define KEY_MOD_CAPS                    (1 << 4)
#define KEY_MOD_NUM                     (1 << 5)

// ============================================================================
// Message Structures
// ============================================================================

// Base message header (all messages start with this)
typedef struct {
    uint16_t type;          // Message type
    uint16_t flags;         // Message flags
    uint32_t size;          // Total message size including header
    uint32_t window_id;     // Window ID (0 for global messages)
    uint32_t seq;           // Sequence number for request/response matching
} __attribute__((packed)) msg_header_t;

// Client connection request
typedef struct {
    msg_header_t header;
    uint32_t protocol_version;
    char app_name[64];      // Application name for identification
} __attribute__((packed)) msg_client_connect_t;

// Compositor welcome response
typedef struct {
    msg_header_t header;
    uint32_t client_id;         // Assigned client ID
    uint32_t screen_width;      // Screen dimensions
    uint32_t screen_height;
    uint32_t compositor_channel; // Channel ID to use for communication
} __attribute__((packed)) msg_compositor_welcome_t;

// Window creation request
typedef struct {
    msg_header_t header;
    int32_t x, y;           // Requested position (-1 for auto-placement)
    int32_t width, height;  // Requested size
    uint32_t flags;         // Window flags
    char title[128];        // Window title
} __attribute__((packed)) msg_window_create_t;

// Window created response
typedef struct {
    msg_header_t header;
    uint32_t error;         // Error code (0 = success)
    int32_t x, y;           // Actual position
    int32_t width, height;  // Actual size
    int32_t shm_id;         // Shared memory ID for window buffer
    uint64_t buffer_addr;   // Virtual address of mapped buffer
} __attribute__((packed)) msg_window_created_t;

// Window move/resize
typedef struct {
    msg_header_t header;
    int32_t x, y;
    int32_t width, height;
} __attribute__((packed)) msg_window_geometry_t;

// Buffer attachment (shared memory)
typedef struct {
    msg_header_t header;
    int32_t shm_id;         // Shared memory region ID
    int32_t width, height;  // Buffer dimensions
    int32_t stride;         // Bytes per row
    uint32_t format;        // Pixel format (ARGB8888 = 0)
} __attribute__((packed)) msg_buffer_attach_t;

// Buffer damage notification
typedef struct {
    msg_header_t header;
    int32_t x, y;           // Damaged region start
    int32_t width, height;  // Damaged region size
} __attribute__((packed)) msg_buffer_damage_t;

// Window configure event (compositor -> client)
typedef struct {
    msg_header_t header;
    int32_t x, y;
    int32_t width, height;
    uint32_t flags;         // New window flags
} __attribute__((packed)) msg_window_configure_t;

// Mouse motion event
typedef struct {
    msg_header_t header;
    int32_t x, y;           // Position relative to window
    int32_t dx, dy;         // Delta from last position
    uint32_t buttons;       // Button mask
} __attribute__((packed)) msg_mouse_move_t;

// Mouse button event
typedef struct {
    msg_header_t header;
    int32_t x, y;           // Position relative to window
    uint32_t button;        // Which button
    uint32_t state;         // 1 = pressed, 0 = released
    uint32_t modifiers;     // Keyboard modifiers held
} __attribute__((packed)) msg_mouse_button_t;

// Mouse scroll event
typedef struct {
    msg_header_t header;
    int32_t x, y;           // Position relative to window
    int32_t scroll_x;       // Horizontal scroll
    int32_t scroll_y;       // Vertical scroll
} __attribute__((packed)) msg_mouse_scroll_t;

// Keyboard event
typedef struct {
    msg_header_t header;
    uint32_t keycode;       // Hardware keycode
    uint32_t scancode;      // Scancode
    uint32_t modifiers;     // Modifier mask
    char utf8[8];           // UTF-8 representation (for key down)
} __attribute__((packed)) msg_key_event_t;

// Error response
typedef struct {
    msg_header_t header;
    uint32_t error_code;
    char message[128];
} __attribute__((packed)) msg_error_t;

// ============================================================================
// Buffer Formats
// ============================================================================
#define BUFFER_FORMAT_ARGB8888          0   // 32-bit ARGB (default)
#define BUFFER_FORMAT_XRGB8888          1   // 32-bit RGB (ignore alpha)
#define BUFFER_FORMAT_RGB888            2   // 24-bit RGB

// ============================================================================
// Well-known Channel IDs
// ============================================================================
#define COMPOSITOR_CHANNEL_ID           1   // Main compositor channel

// ============================================================================
// Utility Macros
// ============================================================================
#define MSG_INIT_HEADER(msg, msg_type, win_id) do {     (msg)->header.type = (msg_type);     (msg)->header.flags = 0;     (msg)->header.size = sizeof(*(msg));     (msg)->header.window_id = (win_id);     (msg)->header.seq = 0; } while(0)

#endif // COMPOSITOR_PROTOCOL_H
