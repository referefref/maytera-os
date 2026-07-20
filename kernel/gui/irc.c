// irc.c - IRC Client GUI application for MayteraOS
// Simple IRC client using TCP socket API
#include "irc.h"
#include "window.h"
#include "desktop.h"
#include "../types.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../net/tcp.h"
#include "../net/net.h"
#include "syslog.h"

// Global IRC client for launch callback
static irc_client_t *g_active_irc = NULL;

// Forward declarations
static void irc_handle_message(irc_client_t *irc, const char *line);
static void irc_draw_chat(irc_client_t *irc);
static void irc_draw_input(irc_client_t *irc);
static void irc_draw_userlist(irc_client_t *irc);
static void irc_draw_status(irc_client_t *irc);
static void irc_draw_dialog(irc_client_t *irc);
static void irc_process_input(irc_client_t *irc);

// Character dimensions
#define CHAR_W 8
#define CHAR_H 16

// ============================================================================
// Utility Functions
// ============================================================================

// Simple string copy with length limit
static void safe_strcpy(char *dest, const char *src, size_t max) {
    size_t i;
    for (i = 0; i < max - 1 && src[i]; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

// Parse IP address string to uint32_t
uint32_t irc_parse_ip(const char *ip_str) {
    uint32_t ip = 0;
    uint8_t octets[4] = {0};
    int octet_idx = 0;
    int value = 0;

    for (const char *c = ip_str; *c && octet_idx < 4; c++) {
        if (*c >= '0' && *c <= '9') {
            value = value * 10 + (*c - '0');
            if (value > 255) return 0;
        } else if (*c == '.') {
            octets[octet_idx++] = (uint8_t)value;
            value = 0;
        } else if (*c == ' ' || *c == '\0' || *c == ':') {
            break;
        } else {
            return 0;  // Invalid character
        }
    }

    if (octet_idx < 4) {
        octets[octet_idx] = (uint8_t)value;
        octet_idx++;
    }

    if (octet_idx != 4) return 0;

    // Build IP in host byte order (high byte = first octet)
    ip = ((uint32_t)octets[0] << 24) | ((uint32_t)octets[1] << 16) |
         ((uint32_t)octets[2] << 8) | (uint32_t)octets[3];

    return ip;
}

// Format IP address to string (static buffer)
static const char *ip_to_str(uint32_t ip) {
    static char buf[16];
    int pos = 0;
    for (int i = 3; i >= 0; i--) {
        uint8_t octet = (ip >> (i * 8)) & 0xFF;
        if (octet >= 100) buf[pos++] = '0' + (octet / 100);
        if (octet >= 10) buf[pos++] = '0' + ((octet / 10) % 10);
        buf[pos++] = '0' + (octet % 10);
        if (i > 0) buf[pos++] = '.';
    }
    buf[pos] = '\0';
    return buf;
}

// Integer to string (no floating point)
static void int_to_str(int value, char *buf) {
    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }

    int neg = 0;
    if (value < 0) {
        neg = 1;
        value = -value;
    }

    char tmp[16];
    int i = 0;
    while (value > 0) {
        tmp[i++] = '0' + (value % 10);
        value /= 10;
    }

    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

// ============================================================================
// IRC Client Creation/Destruction
// ============================================================================

irc_client_t *irc_create(void) {
    irc_client_t *irc = (irc_client_t *)kmalloc(sizeof(irc_client_t));
    if (!irc) {
        kprintf("[IRC] Failed to allocate IRC client\n");
        return NULL;
    }

    memset(irc, 0, sizeof(irc_client_t));

    // Calculate window size
    int width = IRC_WIDTH + 10;
    int height = IRC_HEIGHT + TITLEBAR_HEIGHT + 10;

    // Center on screen
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();
    int x = (screen_w - width) / 2;
    int y = (screen_h - height) / 2 - 50;

    // Create window
    irc->window = window_create("IRC Client", x, y, width, height);
    if (!irc->window) {
        kprintf("[IRC] Failed to create window\n");
        kfree(irc);
        return NULL;
    }

    // Set window background
    irc->window->bg_color = IRC_COLOR_BG & 0xFFFFFF;

    // Initialize state
    irc->socket = -1;
    irc->state = IRC_STATE_DISCONNECTED;
    irc->port = IRC_DEFAULT_PORT;
    irc->running = true;
    irc->app_id = -1;
    irc->dock_index = -1;
    irc->dialog_mode = 1;  // Start with server input dialog

    // Default nickname
    safe_strcpy(irc->nickname, "MayteraUser", IRC_MAX_NICKNAME);

    // Initial status
    safe_strcpy(irc->status, "Disconnected - Press F1 to connect", 128);

    // Welcome message
    irc_add_chat_line(irc, "Welcome to MayteraOS IRC Client!", IRC_COLOR_SERVER);
    irc_add_chat_line(irc, "Press F1 to connect to a server", IRC_COLOR_TEXT);
    irc_add_chat_line(irc, "Commands: /nick, /join, /part, /quit, /msg", IRC_COLOR_TEXT);

    kprintf("[IRC] IRC client created\n");
    return irc;
}

void irc_destroy(irc_client_t *irc) {
    if (!irc) return;

    if (irc->socket >= 0) {
        irc_disconnect(irc);
    }

    if (irc->window) {
        window_destroy(irc->window);
    }

    kfree(irc);
    kprintf("[IRC] IRC client destroyed\n");
}

// ============================================================================
// Chat Display
// ============================================================================

void irc_add_chat_line(irc_client_t *irc, const char *text, uint32_t color) {
    if (!irc || !text) return;

    // Shift lines up if at capacity
    if (irc->chat_count >= IRC_CHAT_LINES) {
        for (int i = 0; i < IRC_CHAT_LINES - 1; i++) {
            memcpy(&irc->chat[i], &irc->chat[i + 1], sizeof(irc_chat_line_t));
        }
        irc->chat_count = IRC_CHAT_LINES - 1;
    }

    // Add new line
    irc_chat_line_t *line = &irc->chat[irc->chat_count];
    safe_strcpy(line->text, text, IRC_MAX_LINE_LEN);
    line->color = color;
    line->active = true;
    irc->chat_count++;

    kprintf("[IRC] Chat: %s\n", text);
}

// ============================================================================
// IRC Connection
// ============================================================================

int irc_connect(irc_client_t *irc, uint32_t ip, uint16_t port) {
    if (!irc) return -1;

    if (irc->socket >= 0) {
        irc_disconnect(irc);
    }

    irc_add_chat_line(irc, "Connecting...", IRC_COLOR_SERVER);
    safe_strcpy(irc->status, "Connecting...", 128);

    // Create TCP socket
    irc->socket = tcp_socket();
    if (irc->socket < 0) {
        irc_add_chat_line(irc, "Error: Failed to create socket", IRC_COLOR_ERROR);
        safe_strcpy(irc->status, "Error: Failed to create socket", 128);
        return -1;
    }

    kprintf("[IRC] Created socket %d, connecting to ", irc->socket);
    kprintf("%s:%d\n", ip_to_str(ip), port);

    // Initiate connection
    int result = tcp_connect(irc->socket, ip, port);
    if (result < 0 && result != TCP_ERR_IN_PROGRESS) {
        irc_add_chat_line(irc, "Error: Connection failed", IRC_COLOR_ERROR);
        safe_strcpy(irc->status, "Error: Connection failed", 128);
        tcp_close(irc->socket);
        irc->socket = -1;
        return -1;
    }

    irc->state = IRC_STATE_CONNECTING;
    irc->port = port;

    // Store server IP for display
    safe_strcpy(irc->server, ip_to_str(ip), IRC_MAX_SERVER);

    return 0;
}

void irc_disconnect(irc_client_t *irc) {
    if (!irc) return;

    if (irc->socket >= 0) {
        // Send QUIT message
        irc_send_raw(irc, "QUIT :Leaving\r\n");
        tcp_close(irc->socket);
        irc->socket = -1;
    }

    irc->state = IRC_STATE_DISCONNECTED;
    irc->channel[0] = '\0';
    irc->user_count = 0;
    irc->recv_pos = 0;

    safe_strcpy(irc->status, "Disconnected", 128);
    irc_add_chat_line(irc, "Disconnected from server", IRC_COLOR_SERVER);
}

// ============================================================================
// IRC Protocol
// ============================================================================

int irc_send_raw(irc_client_t *irc, const char *command) {
    if (!irc || irc->socket < 0 || !command) return -1;

    int len = strlen(command);
    int sent = tcp_send(irc->socket, command, len);

    if (sent < 0) {
        kprintf("[IRC] Send failed: %d\n", sent);
        return -1;
    }

    kprintf("[IRC] Sent: %s", command);
    return sent;
}

void irc_set_nickname(irc_client_t *irc, const char *nick) {
    if (!irc || !nick) return;

    safe_strcpy(irc->nickname, nick, IRC_MAX_NICKNAME);

    if (irc->state == IRC_STATE_CONNECTED) {
        char cmd[128];
        safe_strcpy(cmd, "NICK ", 128);
        strcat(cmd, nick);
        strcat(cmd, "\r\n");
        irc_send_raw(irc, cmd);
    }
}

int irc_join(irc_client_t *irc, const char *channel) {
    if (!irc || !channel || irc->state != IRC_STATE_CONNECTED) return -1;

    char cmd[128];
    safe_strcpy(cmd, "JOIN ", 128);
    strcat(cmd, channel);
    strcat(cmd, "\r\n");

    int result = irc_send_raw(irc, cmd);
    if (result >= 0) {
        safe_strcpy(irc->channel, channel, IRC_MAX_CHANNEL);
    }

    return result;
}

int irc_part(irc_client_t *irc, const char *channel) {
    if (!irc || !channel || irc->state != IRC_STATE_CONNECTED) return -1;

    char cmd[128];
    safe_strcpy(cmd, "PART ", 128);
    strcat(cmd, channel);
    strcat(cmd, "\r\n");

    int result = irc_send_raw(irc, cmd);
    if (result >= 0 && strcmp(irc->channel, channel) == 0) {
        irc->channel[0] = '\0';
        irc->user_count = 0;
    }

    return result;
}

int irc_send_message(irc_client_t *irc, const char *message) {
    if (!irc || !message || irc->channel[0] == '\0') return -1;

    return irc_send_privmsg(irc, irc->channel, message);
}

int irc_send_privmsg(irc_client_t *irc, const char *target, const char *message) {
    if (!irc || !target || !message || irc->state != IRC_STATE_CONNECTED) return -1;

    char cmd[IRC_MAX_MESSAGE];
    safe_strcpy(cmd, "PRIVMSG ", IRC_MAX_MESSAGE);
    strcat(cmd, target);
    strcat(cmd, " :");
    strcat(cmd, message);
    strcat(cmd, "\r\n");

    int result = irc_send_raw(irc, cmd);

    // Show our own message in chat
    if (result >= 0) {
        char display[IRC_MAX_LINE_LEN];
        safe_strcpy(display, "<", IRC_MAX_LINE_LEN);
        strcat(display, irc->nickname);
        strcat(display, "> ");
        strcat(display, message);
        irc_add_chat_line(irc, display, IRC_COLOR_TEXT);
    }

    return result;
}

// ============================================================================
// IRC Message Parsing
// ============================================================================

// Parse and handle a single IRC message line
static void irc_handle_message(irc_client_t *irc, const char *line) {
    if (!irc || !line || !line[0]) return;

    kprintf("[IRC] Recv: %s\n", line);

    // Parse IRC message format: [:prefix] command [params] [:trailing]
    const char *prefix = NULL;
    const char *command = NULL;
    const char *params = NULL;
    const char *trailing = NULL;

    const char *p = line;

    // Skip leading spaces
    while (*p == ' ') p++;

    // Extract prefix (starts with :)
    if (*p == ':') {
        p++;
        prefix = p;
        while (*p && *p != ' ') p++;
        if (*p == ' ') p++;
    }

    // Extract command
    command = p;
    while (*p && *p != ' ') p++;

    // Create null-terminated command copy
    char cmd[32];
    int cmd_len = p - command;
    if (cmd_len >= 32) cmd_len = 31;
    memcpy(cmd, command, cmd_len);
    cmd[cmd_len] = '\0';

    // Skip space after command
    if (*p == ' ') p++;

    // Extract parameters (everything after command)
    params = p;

    // Find trailing (starts with :)
    const char *t = p;
    while (*t) {
        if (*t == ':' && (t == params || *(t-1) == ' ')) {
            trailing = t + 1;
            break;
        }
        t++;
    }

    // Handle PING
    if (strcmp(cmd, "PING") == 0) {
        char pong[128];
        safe_strcpy(pong, "PONG ", 128);
        if (trailing) {
            strcat(pong, ":");
            strcat(pong, trailing);
        } else if (params && params[0]) {
            strcat(pong, params);
        }
        strcat(pong, "\r\n");
        irc_send_raw(irc, pong);
        return;
    }

    // Handle numeric replies
    if (cmd[0] >= '0' && cmd[0] <= '9') {
        int numeric = atoi(cmd);

        switch (numeric) {
            case 1:   // RPL_WELCOME
            case 2:   // RPL_YOURHOST
            case 3:   // RPL_CREATED
            case 4:   // RPL_MYINFO
                if (irc->state != IRC_STATE_CONNECTED) {
                    irc->state = IRC_STATE_CONNECTED;
                    safe_strcpy(irc->status, "Connected to ", 128);
                    strcat(irc->status, irc->server);
                }
                if (trailing) {
                    irc_add_chat_line(irc, trailing, IRC_COLOR_SERVER);
                }
                break;

            case 353: // RPL_NAMREPLY - Names list
                if (trailing) {
                    // Parse user list
                    irc->user_count = 0;
                    const char *u = trailing;
                    while (*u && irc->user_count < IRC_MAX_USERS) {
                        // Skip leading spaces and mode prefixes (@, +)
                        while (*u == ' ' || *u == '@' || *u == '+' || *u == '%' || *u == '&' || *u == '~') u++;
                        if (!*u) break;

                        // Copy username
                        char *dest = irc->users[irc->user_count];
                        int i = 0;
                        while (*u && *u != ' ' && i < IRC_MAX_USERNAME - 1) {
                            dest[i++] = *u++;
                        }
                        dest[i] = '\0';
                        if (i > 0) irc->user_count++;
                    }
                }
                break;

            case 366: // RPL_ENDOFNAMES
                {
                    char msg[64];
                    safe_strcpy(msg, "Users in channel: ", 64);
                    char num[8];
                    int_to_str(irc->user_count, num);
                    strcat(msg, num);
                    irc_add_chat_line(irc, msg, IRC_COLOR_CHANNEL);
                }
                break;

            case 372: // RPL_MOTD
            case 375: // RPL_MOTDSTART
            case 376: // RPL_ENDOFMOTD
                if (trailing) {
                    irc_add_chat_line(irc, trailing, IRC_COLOR_SERVER);
                }
                break;

            case 433: // ERR_NICKNAMEINUSE
                irc_add_chat_line(irc, "Error: Nickname already in use", IRC_COLOR_ERROR);
                // Try alternate nick
                strcat(irc->nickname, "_");
                {
                    char nickcmd[64];
                    safe_strcpy(nickcmd, "NICK ", 64);
                    strcat(nickcmd, irc->nickname);
                    strcat(nickcmd, "\r\n");
                    irc_send_raw(irc, nickcmd);
                }
                break;

            default:
                // Display other numerics
                if (trailing) {
                    irc_add_chat_line(irc, trailing, IRC_COLOR_TEXT);
                }
                break;
        }
        return;
    }

    // Handle PRIVMSG
    if (strcmp(cmd, "PRIVMSG") == 0) {
        // Extract sender nick from prefix
        char sender[IRC_MAX_NICKNAME];
        sender[0] = '\0';
        if (prefix) {
            int i = 0;
            while (prefix[i] && prefix[i] != '!' && prefix[i] != ' ' && i < IRC_MAX_NICKNAME - 1) {
                sender[i] = prefix[i];
                i++;
            }
            sender[i] = '\0';
        }

        // Format message
        char display[IRC_MAX_LINE_LEN];
        if (trailing && trailing[0] == '\001') {
            // CTCP/ACTION
            if (strncmp(trailing, "\001ACTION ", 8) == 0) {
                safe_strcpy(display, "* ", IRC_MAX_LINE_LEN);
                strcat(display, sender);
                strcat(display, " ");
                // Remove ACTION and trailing \001
                const char *action = trailing + 8;
                int alen = strlen(action);
                char action_text[IRC_MAX_LINE_LEN];
                memcpy(action_text, action, alen > IRC_MAX_LINE_LEN - 1 ? IRC_MAX_LINE_LEN - 1 : alen);
                if (alen > 0 && action_text[alen - 1] == '\001') action_text[alen - 1] = '\0';
                else action_text[alen] = '\0';
                strcat(display, action_text);
                irc_add_chat_line(irc, display, IRC_COLOR_ACTION);
            }
        } else if (trailing) {
            safe_strcpy(display, "<", IRC_MAX_LINE_LEN);
            strcat(display, sender);
            strcat(display, "> ");
            strcat(display, trailing);

            // Check for highlight (our nick mentioned)
            uint32_t color = IRC_COLOR_TEXT;
            if (strstr(trailing, irc->nickname)) {
                color = IRC_COLOR_HIGHLIGHT;
            }
            irc_add_chat_line(irc, display, color);
        }
        return;
    }

    // Handle JOIN
    if (strcmp(cmd, "JOIN") == 0) {
        char sender[IRC_MAX_NICKNAME];
        sender[0] = '\0';
        if (prefix) {
            int i = 0;
            while (prefix[i] && prefix[i] != '!' && prefix[i] != ' ' && i < IRC_MAX_NICKNAME - 1) {
                sender[i] = prefix[i];
                i++;
            }
            sender[i] = '\0';
        }

        const char *chan = trailing ? trailing : params;
        char display[IRC_MAX_LINE_LEN];
        safe_strcpy(display, "-> ", IRC_MAX_LINE_LEN);
        strcat(display, sender);
        strcat(display, " has joined ");
        if (chan) strcat(display, chan);
        irc_add_chat_line(irc, display, IRC_COLOR_CHANNEL);

        // Add to user list if not us
        if (strcmp(sender, irc->nickname) != 0 && irc->user_count < IRC_MAX_USERS) {
            safe_strcpy(irc->users[irc->user_count], sender, IRC_MAX_USERNAME);
            irc->user_count++;
        }
        return;
    }

    // Handle PART
    if (strcmp(cmd, "PART") == 0) {
        char sender[IRC_MAX_NICKNAME];
        sender[0] = '\0';
        if (prefix) {
            int i = 0;
            while (prefix[i] && prefix[i] != '!' && prefix[i] != ' ' && i < IRC_MAX_NICKNAME - 1) {
                sender[i] = prefix[i];
                i++;
            }
            sender[i] = '\0';
        }

        char display[IRC_MAX_LINE_LEN];
        safe_strcpy(display, "<- ", IRC_MAX_LINE_LEN);
        strcat(display, sender);
        strcat(display, " has left");
        if (trailing) {
            strcat(display, " (");
            strcat(display, trailing);
            strcat(display, ")");
        }
        irc_add_chat_line(irc, display, IRC_COLOR_CHANNEL);

        // Remove from user list
        for (int i = 0; i < irc->user_count; i++) {
            if (strcmp(irc->users[i], sender) == 0) {
                for (int j = i; j < irc->user_count - 1; j++) {
                    safe_strcpy(irc->users[j], irc->users[j + 1], IRC_MAX_USERNAME);
                }
                irc->user_count--;
                break;
            }
        }
        return;
    }

    // Handle QUIT
    if (strcmp(cmd, "QUIT") == 0) {
        char sender[IRC_MAX_NICKNAME];
        sender[0] = '\0';
        if (prefix) {
            int i = 0;
            while (prefix[i] && prefix[i] != '!' && prefix[i] != ' ' && i < IRC_MAX_NICKNAME - 1) {
                sender[i] = prefix[i];
                i++;
            }
            sender[i] = '\0';
        }

        char display[IRC_MAX_LINE_LEN];
        safe_strcpy(display, "<- ", IRC_MAX_LINE_LEN);
        strcat(display, sender);
        strcat(display, " has quit");
        if (trailing) {
            strcat(display, " (");
            strcat(display, trailing);
            strcat(display, ")");
        }
        irc_add_chat_line(irc, display, IRC_COLOR_CHANNEL);

        // Remove from user list
        for (int i = 0; i < irc->user_count; i++) {
            if (strcmp(irc->users[i], sender) == 0) {
                for (int j = i; j < irc->user_count - 1; j++) {
                    safe_strcpy(irc->users[j], irc->users[j + 1], IRC_MAX_USERNAME);
                }
                irc->user_count--;
                break;
            }
        }
        return;
    }

    // Handle NICK change
    if (strcmp(cmd, "NICK") == 0) {
        char sender[IRC_MAX_NICKNAME];
        sender[0] = '\0';
        if (prefix) {
            int i = 0;
            while (prefix[i] && prefix[i] != '!' && prefix[i] != ' ' && i < IRC_MAX_NICKNAME - 1) {
                sender[i] = prefix[i];
                i++;
            }
            sender[i] = '\0';
        }

        const char *newnick = trailing ? trailing : params;
        char display[IRC_MAX_LINE_LEN];
        safe_strcpy(display, "* ", IRC_MAX_LINE_LEN);
        strcat(display, sender);
        strcat(display, " is now known as ");
        if (newnick) strcat(display, newnick);
        irc_add_chat_line(irc, display, IRC_COLOR_ACTION);

        // Update user list
        for (int i = 0; i < irc->user_count; i++) {
            if (strcmp(irc->users[i], sender) == 0) {
                if (newnick) safe_strcpy(irc->users[i], newnick, IRC_MAX_USERNAME);
                break;
            }
        }
        return;
    }

    // Handle NOTICE
    if (strcmp(cmd, "NOTICE") == 0) {
        if (trailing) {
            char display[IRC_MAX_LINE_LEN];
            safe_strcpy(display, "-Notice- ", IRC_MAX_LINE_LEN);
            strcat(display, trailing);
            irc_add_chat_line(irc, display, IRC_COLOR_SERVER);
        }
        return;
    }

    // Handle ERROR
    if (strcmp(cmd, "ERROR") == 0) {
        if (trailing) {
            irc_add_chat_line(irc, trailing, IRC_COLOR_ERROR);
        }
        irc_disconnect(irc);
        return;
    }
}

// ============================================================================
// IRC Processing
// ============================================================================

void irc_process(irc_client_t *irc) {
    if (!irc || irc->socket < 0) return;

    // Check connection state
    tcp_state_t state = tcp_get_state(irc->socket);

    if (irc->state == IRC_STATE_CONNECTING) {
        if (state == TCP_STATE_ESTABLISHED) {
            kprintf("[IRC] Connection established, registering...\n");
            irc->state = IRC_STATE_REGISTERING;

            // Send registration (NICK and USER commands)
            char cmd[256];

            // NICK command
            safe_strcpy(cmd, "NICK ", 256);
            strcat(cmd, irc->nickname);
            strcat(cmd, "\r\n");
            irc_send_raw(irc, cmd);

            // USER command
            safe_strcpy(cmd, "USER ", 256);
            strcat(cmd, irc->nickname);
            strcat(cmd, " 0 * :MayteraOS User\r\n");
            irc_send_raw(irc, cmd);

            irc_add_chat_line(irc, "Registering with server...", IRC_COLOR_SERVER);
        } else if (state == TCP_STATE_CLOSED) {
            irc_add_chat_line(irc, "Connection closed by remote", IRC_COLOR_ERROR);
            irc->state = IRC_STATE_DISCONNECTED;
            irc->socket = -1;
            safe_strcpy(irc->status, "Disconnected", 128);
        }
        return;
    }

    if (state != TCP_STATE_ESTABLISHED && state != TCP_STATE_CLOSE_WAIT) {
        if (irc->state == IRC_STATE_CONNECTED || irc->state == IRC_STATE_REGISTERING) {
            irc_add_chat_line(irc, "Connection lost", IRC_COLOR_ERROR);
            irc->state = IRC_STATE_DISCONNECTED;
            irc->socket = -1;
            safe_strcpy(irc->status, "Disconnected", 128);
        }
        return;
    }

    // Receive data
    char buf[512];
    int received = tcp_recv(irc->socket, buf, sizeof(buf) - 1);

    if (received > 0) {
        buf[received] = '\0';

        // Append to receive buffer
        for (int i = 0; i < received && irc->recv_pos < IRC_RECV_BUFFER - 1; i++) {
            irc->recv_buffer[irc->recv_pos++] = buf[i];
        }
        irc->recv_buffer[irc->recv_pos] = '\0';

        // Process complete lines (ending with \r\n or \n)
        char *line_start = irc->recv_buffer;
        char *p = irc->recv_buffer;

        while (*p) {
            if (*p == '\n') {
                // Found end of line
                *p = '\0';
                // Remove trailing \r if present
                if (p > line_start && *(p - 1) == '\r') {
                    *(p - 1) = '\0';
                }

                // Handle this line
                if (line_start[0]) {
                    irc_handle_message(irc, line_start);
                }

                line_start = p + 1;
            }
            p++;
        }

        // Move remaining partial data to beginning
        int remaining = irc->recv_buffer + irc->recv_pos - line_start;
        if (remaining > 0 && line_start != irc->recv_buffer) {
            memmove(irc->recv_buffer, line_start, remaining);
            irc->recv_pos = remaining;
        } else if (line_start == irc->recv_buffer + irc->recv_pos) {
            irc->recv_pos = 0;
        }
        irc->recv_buffer[irc->recv_pos] = '\0';
    } else if (received < 0 && received != TCP_ERR_WOULD_BLOCK) {
        // Connection error
        kprintf("[IRC] Receive error: %d\n", received);
    }
}

// ============================================================================
// Input Processing
// ============================================================================

static void irc_process_input(irc_client_t *irc) {
    if (!irc || irc->input[0] == '\0') return;

    char *input = irc->input;

    // Check for commands (start with /)
    if (input[0] == '/') {
        const char *cmd = input + 1;

        if (strncmp(cmd, "nick ", 5) == 0) {
            const char *newnick = cmd + 5;
            while (*newnick == ' ') newnick++;
            if (*newnick) {
                irc_set_nickname(irc, newnick);
                char msg[64];
                safe_strcpy(msg, "Nickname changed to: ", 64);
                strcat(msg, newnick);
                irc_add_chat_line(irc, msg, IRC_COLOR_ACTION);
            }
        } else if (strncmp(cmd, "join ", 5) == 0) {
            const char *chan = cmd + 5;
            while (*chan == ' ') chan++;
            if (*chan) {
                irc_join(irc, chan);
            }
        } else if (strncmp(cmd, "part", 4) == 0) {
            const char *chan = cmd + 4;
            while (*chan == ' ') chan++;
            if (*chan) {
                irc_part(irc, chan);
            } else if (irc->channel[0]) {
                irc_part(irc, irc->channel);
            }
        } else if (strncmp(cmd, "quit", 4) == 0) {
            irc_disconnect(irc);
        } else if (strncmp(cmd, "msg ", 4) == 0) {
            const char *target = cmd + 4;
            while (*target == ' ') target++;
            const char *msg = target;
            while (*msg && *msg != ' ') msg++;
            if (*msg == ' ') {
                char target_buf[64];
                int len = msg - target;
                if (len > 63) len = 63;
                memcpy(target_buf, target, len);
                target_buf[len] = '\0';
                msg++;
                while (*msg == ' ') msg++;
                if (*msg) {
                    irc_send_privmsg(irc, target_buf, msg);
                }
            }
        } else if (strncmp(cmd, "me ", 3) == 0) {
            const char *action = cmd + 3;
            if (irc->channel[0] && *action) {
                char ctcp[256];
                safe_strcpy(ctcp, "\001ACTION ", 256);
                strcat(ctcp, action);
                strcat(ctcp, "\001");
                irc_send_privmsg(irc, irc->channel, ctcp);

                // Show locally
                char display[IRC_MAX_LINE_LEN];
                safe_strcpy(display, "* ", IRC_MAX_LINE_LEN);
                strcat(display, irc->nickname);
                strcat(display, " ");
                strcat(display, action);
                irc_add_chat_line(irc, display, IRC_COLOR_ACTION);
            }
        } else if (strncmp(cmd, "raw ", 4) == 0) {
            const char *raw = cmd + 4;
            char rawcmd[256];
            safe_strcpy(rawcmd, raw, 256);
            strcat(rawcmd, "\r\n");
            irc_send_raw(irc, rawcmd);
        } else if (strcmp(cmd, "help") == 0) {
            irc_add_chat_line(irc, "Commands:", IRC_COLOR_SERVER);
            irc_add_chat_line(irc, "  /nick <name> - Change nickname", IRC_COLOR_TEXT);
            irc_add_chat_line(irc, "  /join #chan  - Join channel", IRC_COLOR_TEXT);
            irc_add_chat_line(irc, "  /part [#chan]- Leave channel", IRC_COLOR_TEXT);
            irc_add_chat_line(irc, "  /msg <to> <msg> - Private message", IRC_COLOR_TEXT);
            irc_add_chat_line(irc, "  /me <action> - Send action", IRC_COLOR_TEXT);
            irc_add_chat_line(irc, "  /quit        - Disconnect", IRC_COLOR_TEXT);
            irc_add_chat_line(irc, "  F1           - Connect dialog", IRC_COLOR_TEXT);
        } else {
            char errmsg[64];
            safe_strcpy(errmsg, "Unknown command: /", 64);
            strcat(errmsg, cmd);
            irc_add_chat_line(irc, errmsg, IRC_COLOR_ERROR);
        }
    } else {
        // Regular message to current channel
        if (irc->channel[0]) {
            irc_send_message(irc, input);
        } else {
            irc_add_chat_line(irc, "Not in a channel. Use /join #channel", IRC_COLOR_ERROR);
        }
    }

    // Clear input
    irc->input[0] = '\0';
    irc->input_pos = 0;
    irc->input_cursor = 0;
}

// ============================================================================
// Drawing Functions
// ============================================================================

static void irc_draw_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    while (*text) {
        if (*text >= ' ' && *text < 127) {
            const uint8_t *glyph = font_get_glyph(*text);
            if (glyph) {
                for (int r = 0; r < FONT_HEIGHT && r < CHAR_H; r++) {
                    uint8_t bits = glyph[r];
                    for (int c = 0; c < FONT_WIDTH; c++) {
                        if (bits & (0x80 >> c)) {
                            fb_put_pixel(x + c, y + r, color & 0xFFFFFF);
                        }
                    }
                }
            }
        }
        x += CHAR_W;
        text++;
    }
}

static void irc_draw_chat(irc_client_t *irc) {
    if (!irc || !irc->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(irc->window, &wx, &wy, &ww, &wh);

    // Chat area background
    fb_fill_rect(wx + IRC_CHAT_X, wy + IRC_CHAT_Y,
                 IRC_CHAT_WIDTH, IRC_CHAT_HEIGHT, IRC_COLOR_BG & 0xFFFFFF);

    // Calculate visible lines
    int visible_lines = IRC_CHAT_HEIGHT / CHAR_H;
    int start_line = 0;
    if (irc->chat_count > visible_lines) {
        start_line = irc->chat_count - visible_lines + irc->chat_scroll;
        if (start_line < 0) start_line = 0;
        if (start_line > irc->chat_count - visible_lines)
            start_line = irc->chat_count - visible_lines;
    }

    // Draw chat lines
    int y = wy + IRC_CHAT_Y;
    for (int i = start_line; i < irc->chat_count && (y - wy - IRC_CHAT_Y) < IRC_CHAT_HEIGHT - CHAR_H; i++) {
        if (irc->chat[i].active) {
            irc_draw_text(wx + IRC_CHAT_X + 2, y, irc->chat[i].text, irc->chat[i].color);
        }
        y += CHAR_H;
    }
}

static void irc_draw_input(irc_client_t *irc) {
    if (!irc || !irc->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(irc->window, &wx, &wy, &ww, &wh);

    // Input field background
    fb_fill_rect(wx + IRC_INPUT_X, wy + IRC_INPUT_Y,
                 IRC_INPUT_WIDTH, IRC_INPUT_HEIGHT, IRC_COLOR_INPUT_BG & 0xFFFFFF);

    // Draw border
    fb_draw_rect(wx + IRC_INPUT_X, wy + IRC_INPUT_Y,
                 IRC_INPUT_WIDTH, IRC_INPUT_HEIGHT, 0x505050);

    // Draw input text
    int text_y = wy + IRC_INPUT_Y + (IRC_INPUT_HEIGHT - CHAR_H) / 2;
    irc_draw_text(wx + IRC_INPUT_X + 4, text_y, irc->input, IRC_COLOR_TEXT);

    // Draw cursor
    int cursor_x = wx + IRC_INPUT_X + 4 + irc->input_cursor * CHAR_W;
    fb_fill_rect(cursor_x, text_y, 2, CHAR_H, 0x00FF00);
}

static void irc_draw_userlist(irc_client_t *irc) {
    if (!irc || !irc->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(irc->window, &wx, &wy, &ww, &wh);

    // User list background
    fb_fill_rect(wx + IRC_USERLIST_X, wy + IRC_USERLIST_Y,
                 IRC_USERLIST_WIDTH, IRC_USERLIST_HEIGHT, IRC_COLOR_USERLIST & 0xFFFFFF);

    // Draw border
    fb_draw_rect(wx + IRC_USERLIST_X, wy + IRC_USERLIST_Y,
                 IRC_USERLIST_WIDTH, IRC_USERLIST_HEIGHT, 0x404040);

    // Draw header
    irc_draw_text(wx + IRC_USERLIST_X + 4, wy + IRC_USERLIST_Y + 2, "Users", IRC_COLOR_CHANNEL);

    // Draw separator
    fb_fill_rect(wx + IRC_USERLIST_X, wy + IRC_USERLIST_Y + CHAR_H + 4,
                 IRC_USERLIST_WIDTH, 1, 0x404040);

    // Draw users
    int y = wy + IRC_USERLIST_Y + CHAR_H + 8;
    for (int i = 0; i < irc->user_count && y < wy + IRC_USERLIST_Y + IRC_USERLIST_HEIGHT - CHAR_H; i++) {
        irc_draw_text(wx + IRC_USERLIST_X + 4, y, irc->users[i], IRC_COLOR_NICK);
        y += CHAR_H;
    }
}

static void irc_draw_status(irc_client_t *irc) {
    if (!irc || !irc->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(irc->window, &wx, &wy, &ww, &wh);

    // Status bar background
    fb_fill_rect(wx, wy + IRC_STATUS_Y, ww, IRC_STATUS_HEIGHT, 0x252525);

    // Draw channel name
    if (irc->channel[0]) {
        irc_draw_text(wx + 4, wy + IRC_STATUS_Y + 2, irc->channel, IRC_COLOR_CHANNEL);
    }

    // Draw status message
    int status_x = wx + 150;
    irc_draw_text(status_x, wy + IRC_STATUS_Y + 2, irc->status, IRC_COLOR_TEXT);
}

static void irc_draw_dialog(irc_client_t *irc) {
    if (!irc || !irc->window || irc->dialog_mode == 0) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(irc->window, &wx, &wy, &ww, &wh);

    // Dialog box dimensions
    int dialog_w = 300;
    int dialog_h = 100;
    int dialog_x = wx + (ww - dialog_w) / 2;
    int dialog_y = wy + (wh - dialog_h) / 2;

    // Draw dialog background
    fb_fill_rect(dialog_x, dialog_y, dialog_w, dialog_h, 0x3A3A3A);
    fb_draw_rect(dialog_x, dialog_y, dialog_w, dialog_h, 0x505050);

    // Title
    const char *title = NULL;
    const char *prompt = NULL;
    switch (irc->dialog_mode) {
        case 1:
            title = "Connect to Server";
            prompt = "Enter server IP:";
            break;
        case 2:
            title = "Set Nickname";
            prompt = "Enter nickname:";
            break;
        case 3:
            title = "Join Channel";
            prompt = "Enter channel:";
            break;
        default:
            return;
    }

    irc_draw_text(dialog_x + (dialog_w - strlen(title) * CHAR_W) / 2, dialog_y + 10, title, IRC_COLOR_CHANNEL);
    irc_draw_text(dialog_x + 10, dialog_y + 35, prompt, IRC_COLOR_TEXT);

    // Input field
    fb_fill_rect(dialog_x + 10, dialog_y + 55, dialog_w - 20, 24, IRC_COLOR_INPUT_BG & 0xFFFFFF);
    fb_draw_rect(dialog_x + 10, dialog_y + 55, dialog_w - 20, 24, 0x505050);

    irc_draw_text(dialog_x + 14, dialog_y + 59, irc->dialog_input, IRC_COLOR_TEXT);

    // Cursor
    int cursor_x = dialog_x + 14 + irc->dialog_input_pos * CHAR_W;
    fb_fill_rect(cursor_x, dialog_y + 59, 2, CHAR_H, 0x00FF00);

    // Help text
    irc_draw_text(dialog_x + 10, dialog_y + 82, "Enter=OK  ESC=Cancel", 0x808080);
}

void irc_draw(irc_client_t *irc) {
    if (!irc || !irc->window) return;

    // Draw main areas
    irc_draw_chat(irc);
    irc_draw_input(irc);
    irc_draw_userlist(irc);
    irc_draw_status(irc);

    // Draw dialog if active
    if (irc->dialog_mode > 0) {
        irc_draw_dialog(irc);
    }
}

// ============================================================================
// Window Manager Callbacks
// ============================================================================

void irc_on_event(void *app_data, gui_event_t *event) {
    irc_client_t *irc = (irc_client_t *)app_data;
    if (!irc || !irc->window || !event) return;

    switch (event->type) {
        case EVENT_KEY_DOWN:
            {
                int c = (unsigned char)event->key_char;
                int keycode = event->keycode;

                // Handle dialog mode
                if (irc->dialog_mode > 0) {
                    if (c == 27) {  // ESC - cancel dialog
                        irc->dialog_mode = 0;
                        irc->dialog_input[0] = '\0';
                        irc->dialog_input_pos = 0;
                    } else if (c == '\n' || c == '\r') {  // Enter - confirm
                        if (irc->dialog_input[0]) {
                            switch (irc->dialog_mode) {
                                case 1:  // Server
                                    {
                                        uint32_t ip = irc_parse_ip(irc->dialog_input);
                                        if (ip != 0) {
                                            // Store the IP string BEFORE clearing the
                                            // input buffer, otherwise irc->server ends
                                            // up empty and the nick dialog re-parses it
                                            // as 0, so the connection is never started.
                                            safe_strcpy(irc->server, irc->dialog_input, IRC_MAX_SERVER);
                                            irc->dialog_mode = 2;  // Move to nick dialog
                                            irc->dialog_input[0] = '\0';
                                            irc->dialog_input_pos = 0;
                                        } else {
                                            irc_add_chat_line(irc, "Invalid IP address format", IRC_COLOR_ERROR);
                                        }
                                    }
                                    break;
                                case 2:  // Nickname
                                    safe_strcpy(irc->nickname, irc->dialog_input, IRC_MAX_NICKNAME);
                                    {
                                        uint32_t ip = irc_parse_ip(irc->server);
                                        if (ip != 0) {
                                            irc_connect(irc, ip, IRC_DEFAULT_PORT);
                                        }
                                    }
                                    irc->dialog_mode = 0;
                                    irc->dialog_input[0] = '\0';
                                    irc->dialog_input_pos = 0;
                                    break;
                                case 3:  // Channel
                                    if (irc->state == IRC_STATE_CONNECTED) {
                                        irc_join(irc, irc->dialog_input);
                                    }
                                    irc->dialog_mode = 0;
                                    irc->dialog_input[0] = '\0';
                                    irc->dialog_input_pos = 0;
                                    break;
                            }
                        }
                    } else if (c == '\b') {  // Backspace
                        if (irc->dialog_input_pos > 0) {
                            irc->dialog_input_pos--;
                            irc->dialog_input[irc->dialog_input_pos] = '\0';
                        }
                    } else if (c >= ' ' && c < 127 && irc->dialog_input_pos < IRC_MAX_SERVER - 1) {
                        irc->dialog_input[irc->dialog_input_pos++] = c;
                        irc->dialog_input[irc->dialog_input_pos] = '\0';
                    }
                    wm_invalidate_rect(&irc->window->bounds);
                    return;
                }

                // Normal mode key handling
                if (c == 27) {  // ESC - close IRC client
                    kprintf("[IRC] ESC pressed, closing IRC client\n");
                    wm_unregister_app(irc->app_id);
                    if (irc->dock_index >= 0) {
                        dock_remove_app(irc->dock_index);
                    }
                    if (g_active_irc == irc) {
                        g_active_irc = NULL;
                    }
                    window_hide(irc->window);
                    wm_invalidate_all();
                    return;
                } else if (keycode == 0x3B) {  // F1 - Connect dialog
                    irc->dialog_mode = 1;
                    irc->dialog_input[0] = '\0';
                    irc->dialog_input_pos = 0;
                } else if (keycode == 0x3C && irc->state == IRC_STATE_CONNECTED) {  // F2 - Join dialog
                    irc->dialog_mode = 3;
                    irc->dialog_input[0] = '\0';
                    irc->dialog_input_pos = 0;
                } else if (c == '\n' || c == '\r') {  // Enter - send message
                    irc_process_input(irc);
                } else if (c == '\b') {  // Backspace
                    if (irc->input_pos > 0) {
                        irc->input_pos--;
                        irc->input[irc->input_pos] = '\0';
                        irc->input_cursor = irc->input_pos;
                    }
                } else if (c >= ' ' && c < 127 && irc->input_pos < IRC_MAX_INPUT - 1) {
                    irc->input[irc->input_pos++] = c;
                    irc->input[irc->input_pos] = '\0';
                    irc->input_cursor = irc->input_pos;
                }

                wm_invalidate_rect(&irc->window->bounds);
            }
            break;

        case EVENT_WINDOW_CLOSE:
            kprintf("[IRC] Close button clicked\n");
            wm_unregister_app(irc->app_id);
            if (irc->dock_index >= 0) {
                dock_remove_app(irc->dock_index);
            }
            if (g_active_irc == irc) {
                g_active_irc = NULL;
            }
            window_hide(irc->window);
            wm_invalidate_all();
            break;

        default:
            break;
    }
}

void irc_on_draw(void *app_data) {
    irc_client_t *irc = (irc_client_t *)app_data;
    if (irc && irc->window) {
        window_draw(irc->window);
        irc_draw(irc);
    }
}

void irc_on_destroy(void *app_data) {
    irc_client_t *irc = (irc_client_t *)app_data;
    if (irc) {
        kprintf("[IRC] Destroying IRC client instance\n");
        if (g_active_irc == irc) {
            g_active_irc = NULL;
        }
        irc_destroy(irc);
    }
}

// ============================================================================
// Launch Function
// ============================================================================

void irc_launch(void) {
    LOG_INFO("[IRC] Application launched");
    kprintf("[IRC] Launching IRC client...\n");

    irc_client_t *irc = irc_create();
    if (!irc) {
        LOG_ERROR("[IRC] Failed to create IRC client");
        kprintf("[IRC] Failed to create IRC client\n");
        return;
    }

    // Add to taskbar
    irc->dock_index = dock_add_app("IRC", DOCK_ICON_TERMINAL, NULL);

    // Register with window manager
    irc->app_id = wm_register_app(
        irc->window,
        irc,
        irc_on_event,
        irc_on_draw,
        irc_on_destroy
    );

    if (irc->app_id < 0) {
        kprintf("[IRC] Failed to register with window manager\n");
        if (irc->dock_index >= 0) {
            dock_remove_app(irc->dock_index);
        }
        irc_destroy(irc);
        return;
    }

    g_active_irc = irc;
    wm_invalidate_all();

    kprintf("[IRC] IRC client registered as app %d\n", irc->app_id);
}

// ============================================================================
// Periodic Processing (call from main loop)
// ============================================================================

// This function should be called periodically to process incoming IRC data
void irc_tick(void) {
    if (g_active_irc && g_active_irc->socket >= 0) {
        // Poll network for any incoming packets
        extern void net_poll(void);
        net_poll();

        // Process TCP timer for retransmissions
        extern void tcp_timer(void);
        tcp_timer();

        // Process IRC messages
        irc_process(g_active_irc);
    }
}
