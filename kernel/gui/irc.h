// irc.h - IRC Client GUI application for MayteraOS
// Simple IRC client using TCP socket API
#ifndef IRC_H
#define IRC_H

#include "../types.h"
#include "window.h"

// IRC Window dimensions
#define IRC_WIDTH           640
#define IRC_HEIGHT          480

// Chat display area
#define IRC_CHAT_X          10
#define IRC_CHAT_Y          10
#define IRC_CHAT_WIDTH      480
#define IRC_CHAT_HEIGHT     400

// User list area
#define IRC_USERLIST_X      500
#define IRC_USERLIST_Y      10
#define IRC_USERLIST_WIDTH  130
#define IRC_USERLIST_HEIGHT 400

// Input area
#define IRC_INPUT_X         10
#define IRC_INPUT_Y         420
#define IRC_INPUT_WIDTH     620
#define IRC_INPUT_HEIGHT    24

// Status bar
#define IRC_STATUS_Y        450
#define IRC_STATUS_HEIGHT   20

// Buffer sizes
#define IRC_MAX_INPUT       256
#define IRC_MAX_MESSAGE     512
#define IRC_MAX_NICKNAME    32
#define IRC_MAX_CHANNEL     64
#define IRC_MAX_SERVER      128

// Chat history
#define IRC_CHAT_LINES      50
#define IRC_MAX_LINE_LEN    100

// User list
#define IRC_MAX_USERS       32
#define IRC_MAX_USERNAME    32

// IRC protocol constants
#define IRC_DEFAULT_PORT    6667
#define IRC_RECV_BUFFER     2048

// IRC colors (ANSI-inspired)
#define IRC_COLOR_BG        0xFF1E1E1E  // Dark background
#define IRC_COLOR_TEXT      0xFFE0E0E0  // Light grey text
#define IRC_COLOR_NICK      0xFF00AAFF  // Blue for nicknames
#define IRC_COLOR_CHANNEL   0xFF00FF00  // Green for channel names
#define IRC_COLOR_SERVER    0xFFFF8800  // Orange for server messages
#define IRC_COLOR_ERROR     0xFFFF4444  // Red for errors
#define IRC_COLOR_ACTION    0xFFFF00FF  // Magenta for actions
#define IRC_COLOR_HIGHLIGHT 0xFFFFFF00  // Yellow for highlights
#define IRC_COLOR_INPUT_BG  0xFF2D2D2D  // Input field background
#define IRC_COLOR_USERLIST  0xFF252525  // User list background

// IRC connection states
typedef enum {
    IRC_STATE_DISCONNECTED = 0,
    IRC_STATE_CONNECTING,
    IRC_STATE_REGISTERING,
    IRC_STATE_CONNECTED,
    IRC_STATE_ERROR
} irc_state_t;

// Chat line with color
typedef struct {
    char text[IRC_MAX_LINE_LEN + 1];
    uint32_t color;
    bool active;                    // Is this line in use?
} irc_chat_line_t;

// IRC client state
typedef struct {
    window_t *window;               // IRC window

    // Connection
    int socket;                     // TCP socket
    irc_state_t state;              // Connection state
    char server[IRC_MAX_SERVER];    // Server hostname/IP
    uint16_t port;                  // Server port

    // User info
    char nickname[IRC_MAX_NICKNAME];
    char channel[IRC_MAX_CHANNEL];  // Current channel

    // Chat display
    irc_chat_line_t chat[IRC_CHAT_LINES];
    int chat_scroll;                // Scroll offset
    int chat_count;                 // Number of lines

    // User list
    char users[IRC_MAX_USERS][IRC_MAX_USERNAME];
    int user_count;

    // Input
    char input[IRC_MAX_INPUT];
    int input_pos;
    int input_cursor;               // Cursor position in input

    // Receive buffer for partial messages
    char recv_buffer[IRC_RECV_BUFFER];
    int recv_pos;

    // Status
    char status[128];               // Status message
    bool running;

    // Window manager integration
    int app_id;
    int dock_index;

    // Dialog mode for connection
    int dialog_mode;                // 0=chat, 1=server input, 2=nick input, 3=channel input
    char dialog_input[IRC_MAX_SERVER];
    int dialog_input_pos;
} irc_client_t;

// ============================================================================
// IRC Client API
// ============================================================================

// Create IRC client window
irc_client_t *irc_create(void);

// Destroy IRC client
void irc_destroy(irc_client_t *irc);

// Connect to server (IP address as uint32_t, port)
int irc_connect(irc_client_t *irc, uint32_t ip, uint16_t port);

// Disconnect from server
void irc_disconnect(irc_client_t *irc);

// Set nickname
void irc_set_nickname(irc_client_t *irc, const char *nick);

// Join a channel
int irc_join(irc_client_t *irc, const char *channel);

// Leave a channel
int irc_part(irc_client_t *irc, const char *channel);

// Send a message to current channel
int irc_send_message(irc_client_t *irc, const char *message);

// Send a private message
int irc_send_privmsg(irc_client_t *irc, const char *target, const char *message);

// Process incoming data (call from main loop)
void irc_process(irc_client_t *irc);

// Add a line to chat display
void irc_add_chat_line(irc_client_t *irc, const char *text, uint32_t color);

// Draw IRC client content
void irc_draw(irc_client_t *irc);

// Launch callback for dock (non-blocking, registers with WM)
extern void irc_launch(void);

// ============================================================================
// Window Manager Callbacks (internal use)
// ============================================================================

void irc_on_event(void *app_data, gui_event_t *event);
void irc_on_draw(void *app_data);
void irc_on_destroy(void *app_data);

// ============================================================================
// IRC Protocol Helpers
// ============================================================================

// Send raw IRC command
int irc_send_raw(irc_client_t *irc, const char *command);

// Parse IP address string to uint32_t (e.g., "192.0.2.1")
uint32_t irc_parse_ip(const char *ip_str);

// Periodic processing function - call from main loop to process network data
void irc_tick(void);

#endif // IRC_H
