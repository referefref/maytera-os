// ftp.c - FTP Client implementation for MayteraOS
// Uses TCP stack for FTP protocol (RFC 959)
// Supports passive mode (PASV) for data connections
// No floating point - uses integer math only

#include "ftp.h"
#include "tcp.h"
#include "ip.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"

// External declarations
extern void net_poll(void);
extern volatile uint64_t timer_ticks;

// Timeout values (in timer ticks, ~18.2 ticks/sec)
#define FTP_CONNECT_TIMEOUT     200     // ~10 seconds
#define FTP_RESPONSE_TIMEOUT    400     // ~20 seconds
#define FTP_DATA_TIMEOUT        600     // ~30 seconds

// Byte swap functions (same as in tcp.c)
static inline uint16_t htons(uint16_t h) {
    return ((h & 0xFF) << 8) | ((h >> 8) & 0xFF);
}

static inline uint16_t ntohs(uint16_t n) {
    return htons(n);
}

static inline uint32_t htonl(uint32_t h) {
    return ((h & 0xFF) << 24) | ((h & 0xFF00) << 8) |
           ((h >> 8) & 0xFF00) | ((h >> 24) & 0xFF);
}

static inline uint32_t ntohl(uint32_t n) {
    return htonl(n);
}

// Initialize FTP subsystem
void ftp_init(void) {
    kprintf("[FTP] FTP client initialized\n");
}

// Create a new FTP session
ftp_session_t *ftp_session_create(void) {
    ftp_session_t *session = (ftp_session_t *)kmalloc(sizeof(ftp_session_t));
    if (!session) {
        kprintf("[FTP] Failed to allocate session\n");
        return NULL;
    }

    memset(session, 0, sizeof(ftp_session_t));
    session->state = FTP_STATE_DISCONNECTED;
    session->ctrl_sock = -1;
    session->data_sock = -1;
    session->transfer_type = FTP_TYPE_BINARY;
    strcpy(session->cwd, "/");

    return session;
}

// Destroy FTP session
void ftp_session_destroy(ftp_session_t *session) {
    if (!session) return;

    if (session->state != FTP_STATE_DISCONNECTED) {
        ftp_disconnect(session);
    }

    kfree(session);
}

// Small delay helper
static void ftp_delay(int count) {
    for (int i = 0; i < count; i++) {
        __asm__ volatile("pause");
    }
}

// Wait for TCP connection to establish
static int ftp_wait_connect(int sock, uint64_t timeout_ticks) {
    uint64_t start = timer_ticks;

    while (!tcp_is_connected(sock)) {
        net_poll();
        tcp_timer();

        if (timer_ticks - start > timeout_ticks) {
            return -1;
        }

        tcp_state_t state = tcp_get_state(sock);
        if (state == TCP_STATE_CLOSED) {
            return -1;
        }

        ftp_delay(1000);
    }

    return 0;
}

// Send string on TCP socket
static int ftp_send_string(int sock, const char *str) {
    int len = strlen(str);
    int sent = 0;

    while (sent < len) {
        int s = tcp_send(sock, str + sent, len - sent);
        if (s < 0) {
            if (s == TCP_ERR_WOULD_BLOCK) {
                net_poll();
                tcp_timer();
                ftp_delay(500);
                continue;
            }
            return s;
        }
        sent += s;
        net_poll();
        tcp_timer();
    }

    return sent;
}

// Receive line from TCP socket (up to newline)
// Returns number of bytes received, or negative on error
static int ftp_recv_line(int sock, char *buffer, int max_len, uint64_t timeout_ticks) {
    uint64_t start = timer_ticks;
    int pos = 0;

    while (pos < max_len - 1) {
        net_poll();
        tcp_timer();

        // Check timeout
        if (timer_ticks - start > timeout_ticks) {
            kprintf("[FTP] Receive timeout\n");
            return FTP_ERR_TIMEOUT;
        }

        // Try to receive
        char c;
        int r = tcp_recv(sock, &c, 1);

        if (r > 0) {
            start = timer_ticks;  // Reset timeout on data
            if (c == '\n') {
                buffer[pos] = '\0';
                // Strip trailing CR
                if (pos > 0 && buffer[pos-1] == '\r') {
                    buffer[pos-1] = '\0';
                    pos--;
                }
                return pos;
            }
            buffer[pos++] = c;
        } else if (r < 0 && r != TCP_ERR_WOULD_BLOCK) {
            if (r == TCP_ERR_CLOSED) {
                buffer[pos] = '\0';
                return (pos > 0) ? pos : r;
            }
            return r;
        }

        // Check connection state
        tcp_state_t state = tcp_get_state(sock);
        if (state == TCP_STATE_CLOSED || state == TCP_STATE_CLOSE_WAIT) {
            buffer[pos] = '\0';
            return (pos > 0) ? pos : TCP_ERR_CLOSED;
        }

        ftp_delay(200);
    }

    buffer[max_len - 1] = '\0';
    return pos;
}

// Parse FTP response code from response line
static int ftp_parse_response_code(const char *line) {
    if (!line || strlen(line) < 3) return 0;

    // Response format: "NNN text" or "NNN-text" for multiline
    if (isdigit(line[0]) && isdigit(line[1]) && isdigit(line[2])) {
        return (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    }
    return 0;
}

// Receive full FTP response (handles multiline)
static int ftp_recv_response(ftp_session_t *session) {
    char line[512];
    int code = 0;
    int first_code = 0;
    int multiline = 0;

    session->response_buffer[0] = '\0';

    while (1) {
        int r = ftp_recv_line(session->ctrl_sock, line, sizeof(line), FTP_RESPONSE_TIMEOUT);
        if (r < 0) {
            session->last_error = r;
            return r;
        }

        kprintf("[FTP] < %s\n", line);

        // Append to response buffer
        int buf_len = strlen(session->response_buffer);
        int line_len = strlen(line);
        if (buf_len + line_len + 2 < FTP_RESP_BUFFER_SIZE) {
            strcat(session->response_buffer, line);
            strcat(session->response_buffer, "\n");
        }

        code = ftp_parse_response_code(line);
        if (code > 0) {
            if (first_code == 0) {
                first_code = code;
                // Check for multiline response (NNN-text)
                if (strlen(line) > 3 && line[3] == '-') {
                    multiline = 1;
                }
            } else if (code == first_code && strlen(line) > 3 && line[3] == ' ') {
                // End of multiline response
                break;
            }

            if (!multiline) {
                break;
            }
        }
    }

    session->response_code = first_code;
    return first_code;
}

// Send FTP command and get response
static int ftp_send_command(ftp_session_t *session, const char *cmd) {
    char buffer[FTP_CMD_BUFFER_SIZE];

    // Build command with CRLF
    int len = strlen(cmd);
    if (len + 3 > FTP_CMD_BUFFER_SIZE) {
        return FTP_ERR_COMMAND_FAILED;
    }

    strcpy(buffer, cmd);
    strcat(buffer, "\r\n");

    // Log command (hide password)
    if (strncmp(cmd, "PASS ", 5) == 0) {
        kprintf("[FTP] > PASS ****\n");
    } else {
        kprintf("[FTP] > %s\n", cmd);
    }

    // Send command
    if (ftp_send_string(session->ctrl_sock, buffer) < 0) {
        kprintf("[FTP] Failed to send command\n");
        return FTP_ERR_COMMAND_FAILED;
    }

    // Get response
    return ftp_recv_response(session);
}

// Parse PASV response to extract IP and port
// Response format: "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)"
static int ftp_parse_pasv(const char *response, uint32_t *ip_out, uint16_t *port_out) {
    const char *p = strchr(response, '(');
    if (!p) return -1;
    p++;

    int values[6] = {0};
    int idx = 0;
    int val = 0;

    while (*p && idx < 6) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
        } else if (*p == ',' || *p == ')') {
            values[idx++] = val;
            val = 0;
            if (*p == ')') break;
        }
        p++;
    }

    if (idx != 6) return -1;

    // IP is in host byte order (high byte = first octet)
    *ip_out = ((uint32_t)values[0] << 24) | ((uint32_t)values[1] << 16) |
              ((uint32_t)values[2] << 8) | (uint32_t)values[3];

    // Port is p1*256 + p2
    *port_out = (uint16_t)(values[4] * 256 + values[5]);

    return 0;
}

// Enter passive mode and establish data connection
static int ftp_enter_pasv(ftp_session_t *session) {
    // Send PASV command
    int resp = ftp_send_command(session, "PASV");
    if (resp < 0) {
        return resp;
    }

    if (resp != FTP_RESP_PASV_MODE) {
        kprintf("[FTP] PASV failed: %d\n", resp);
        return FTP_ERR_PASV_FAILED;
    }

    // Parse response
    if (ftp_parse_pasv(session->response_buffer, &session->data_ip, &session->data_port) < 0) {
        kprintf("[FTP] Failed to parse PASV response\n");
        return FTP_ERR_INVALID_RESPONSE;
    }

    kprintf("[FTP] PASV: ");
    ip_print(session->data_ip);
    kprintf(":%d\n", session->data_port);

    // Create data socket
    session->data_sock = tcp_socket();
    if (session->data_sock < 0) {
        kprintf("[FTP] Failed to create data socket\n");
        return FTP_ERR_NO_MEMORY;
    }

    // Connect to data port
    int conn = tcp_connect(session->data_sock, session->data_ip, session->data_port);
    if (conn != TCP_ERR_IN_PROGRESS && conn < 0) {
        kprintf("[FTP] Data connection failed: %d\n", conn);
        tcp_close(session->data_sock);
        session->data_sock = -1;
        return FTP_ERR_CONNECT_FAILED;
    }

    // Wait for connection
    if (ftp_wait_connect(session->data_sock, FTP_CONNECT_TIMEOUT) < 0) {
        kprintf("[FTP] Data connection timeout\n");
        tcp_close(session->data_sock);
        session->data_sock = -1;
        return FTP_ERR_TIMEOUT;
    }

    kprintf("[FTP] Data connection established\n");
    return FTP_SUCCESS;
}

// Close data connection
static void ftp_close_data(ftp_session_t *session) {
    if (session->data_sock >= 0) {
        tcp_close(session->data_sock);
        session->data_sock = -1;
    }
}

// Connect to FTP server
int ftp_connect(ftp_session_t *session, uint32_t server_ip, uint16_t port) {
    if (!session) return FTP_ERR_NO_MEMORY;

    if (session->state != FTP_STATE_DISCONNECTED) {
        return FTP_ERR_ALREADY_CONNECTED;
    }

    // Check network
    if (ip_get_address() == 0) {
        kprintf("[FTP] No network configured\n");
        return FTP_ERR_NO_NETWORK;
    }

    session->server_ip = server_ip;
    session->server_port = port;

    kprintf("[FTP] Connecting to ");
    ip_print(server_ip);
    kprintf(":%d\n", port);

    // Create control socket
    session->ctrl_sock = tcp_socket();
    if (session->ctrl_sock < 0) {
        kprintf("[FTP] Failed to create socket\n");
        return FTP_ERR_NO_MEMORY;
    }

    session->state = FTP_STATE_CONNECTING;

    // Connect
    int conn = tcp_connect(session->ctrl_sock, server_ip, port);
    if (conn != TCP_ERR_IN_PROGRESS && conn < 0) {
        kprintf("[FTP] Connection failed: %d\n", conn);
        tcp_close(session->ctrl_sock);
        session->ctrl_sock = -1;
        session->state = FTP_STATE_DISCONNECTED;
        return FTP_ERR_CONNECT_FAILED;
    }

    // Wait for connection
    if (ftp_wait_connect(session->ctrl_sock, FTP_CONNECT_TIMEOUT) < 0) {
        kprintf("[FTP] Connection timeout\n");
        tcp_close(session->ctrl_sock);
        session->ctrl_sock = -1;
        session->state = FTP_STATE_DISCONNECTED;
        return FTP_ERR_TIMEOUT;
    }

    kprintf("[FTP] Connected, waiting for welcome message...\n");

    // Read welcome message
    int resp = ftp_recv_response(session);
    if (resp < 0) {
        kprintf("[FTP] No welcome message\n");
        tcp_close(session->ctrl_sock);
        session->ctrl_sock = -1;
        session->state = FTP_STATE_DISCONNECTED;
        return resp;
    }

    if (resp != FTP_RESP_READY && resp != FTP_RESP_WELCOME) {
        kprintf("[FTP] Unexpected response: %d\n", resp);
        tcp_close(session->ctrl_sock);
        session->ctrl_sock = -1;
        session->state = FTP_STATE_DISCONNECTED;
        return FTP_ERR_CONNECT_FAILED;
    }

    session->state = FTP_STATE_CONNECTED;
    kprintf("[FTP] Connection established\n");
    return FTP_SUCCESS;
}

// Disconnect from FTP server
void ftp_disconnect(ftp_session_t *session) {
    if (!session) return;

    if (session->state >= FTP_STATE_CONNECTED) {
        // Send QUIT command (don't wait for response)
        ftp_send_string(session->ctrl_sock, "QUIT\r\n");
        kprintf("[FTP] > QUIT\n");

        // Brief wait for response
        uint64_t start = timer_ticks;
        while (timer_ticks - start < 20) {  // ~1 second
            net_poll();
            tcp_timer();
            ftp_delay(500);
        }
    }

    ftp_close_data(session);

    if (session->ctrl_sock >= 0) {
        tcp_close(session->ctrl_sock);
        session->ctrl_sock = -1;
    }

    session->state = FTP_STATE_DISCONNECTED;
    session->logged_in = 0;
    kprintf("[FTP] Disconnected\n");
}

// Login to FTP server
int ftp_login(ftp_session_t *session, const char *username, const char *password) {
    if (!session || session->state < FTP_STATE_CONNECTED) {
        return FTP_ERR_NOT_CONNECTED;
    }

    // Store credentials
    strncpy(session->username, username, sizeof(session->username) - 1);
    strncpy(session->password, password, sizeof(session->password) - 1);

    // Send USER command
    char cmd[128];
    strcpy(cmd, "USER ");
    strcat(cmd, username);
    int resp = ftp_send_command(session, cmd);

    if (resp < 0) return resp;

    if (resp == FTP_RESP_LOGGED_IN) {
        // Logged in without password
        session->logged_in = 1;
        session->state = FTP_STATE_LOGGED_IN;
        kprintf("[FTP] Logged in (no password required)\n");
        return FTP_SUCCESS;
    }

    if (resp != FTP_RESP_NEED_PASSWORD) {
        kprintf("[FTP] USER failed: %d\n", resp);
        return FTP_ERR_LOGIN_FAILED;
    }

    // Send PASS command
    strcpy(cmd, "PASS ");
    strcat(cmd, password);
    resp = ftp_send_command(session, cmd);

    if (resp < 0) return resp;

    if (resp != FTP_RESP_LOGGED_IN) {
        kprintf("[FTP] PASS failed: %d\n", resp);
        return FTP_ERR_LOGIN_FAILED;
    }

    session->logged_in = 1;
    session->state = FTP_STATE_LOGGED_IN;
    kprintf("[FTP] Logged in as %s\n", username);

    // Set binary mode by default
    ftp_set_type(session, FTP_TYPE_BINARY);

    return FTP_SUCCESS;
}

// Anonymous login
int ftp_login_anonymous(ftp_session_t *session) {
    return ftp_login(session, "anonymous", "ftp@mayteraos.local");
}

// Change directory
int ftp_cd(ftp_session_t *session, const char *path) {
    if (!session || !session->logged_in) {
        return FTP_ERR_NOT_CONNECTED;
    }

    char cmd[FTP_PATH_MAX + 8];
    strcpy(cmd, "CWD ");
    strcat(cmd, path);

    int resp = ftp_send_command(session, cmd);
    if (resp < 0) return resp;

    if (resp != FTP_RESP_FILE_ACTION_OK) {
        kprintf("[FTP] CWD failed: %d\n", resp);
        return FTP_ERR_COMMAND_FAILED;
    }

    // Update stored cwd
    if (path[0] == '/') {
        strncpy(session->cwd, path, FTP_PATH_MAX - 1);
    } else {
        // Relative path - simple append (not fully correct but works)
        if (strcmp(session->cwd, "/") != 0) {
            strcat(session->cwd, "/");
        }
        strncat(session->cwd, path, FTP_PATH_MAX - strlen(session->cwd) - 1);
    }

    return FTP_SUCCESS;
}

// Change to parent directory
int ftp_cdup(ftp_session_t *session) {
    if (!session || !session->logged_in) {
        return FTP_ERR_NOT_CONNECTED;
    }

    int resp = ftp_send_command(session, "CDUP");
    if (resp < 0) return resp;

    if (resp != FTP_RESP_FILE_ACTION_OK && resp != FTP_RESP_OK) {
        return FTP_ERR_COMMAND_FAILED;
    }

    return FTP_SUCCESS;
}

// Get current working directory
int ftp_pwd(ftp_session_t *session, char *path_out, int max_len) {
    if (!session || !session->logged_in) {
        return FTP_ERR_NOT_CONNECTED;
    }

    int resp = ftp_send_command(session, "PWD");
    if (resp < 0) return resp;

    if (resp != FTP_RESP_PATHNAME) {
        return FTP_ERR_COMMAND_FAILED;
    }

    // Parse response: "257 "/path" ..."
    const char *p = strchr(session->response_buffer, '"');
    if (p) {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < max_len - 1) {
            path_out[i++] = *p++;
        }
        path_out[i] = '\0';

        // Update stored cwd
        strncpy(session->cwd, path_out, FTP_PATH_MAX - 1);
    } else {
        strncpy(path_out, session->cwd, max_len - 1);
    }

    return FTP_SUCCESS;
}

// List directory
int ftp_ls(ftp_session_t *session, const char *path, ftp_list_callback_t callback, void *user_data) {
    if (!session || !session->logged_in) {
        return FTP_ERR_NOT_CONNECTED;
    }

    // Enter passive mode
    int ret = ftp_enter_pasv(session);
    if (ret < 0) {
        return ret;
    }

    // Send LIST command
    char cmd[FTP_PATH_MAX + 8];
    if (path && *path) {
        strcpy(cmd, "LIST ");
        strcat(cmd, path);
    } else {
        strcpy(cmd, "LIST");
    }

    int resp = ftp_send_command(session, cmd);
    if (resp < 0) {
        ftp_close_data(session);
        return resp;
    }

    if (resp != FTP_RESP_FILE_OK && resp != FTP_RESP_DATA_CONN_OPEN) {
        kprintf("[FTP] LIST failed: %d\n", resp);
        ftp_close_data(session);
        return FTP_ERR_COMMAND_FAILED;
    }

    // Receive listing data
    char line[512];
    int line_pos = 0;
    uint64_t start = timer_ticks;

    while (1) {
        net_poll();
        tcp_timer();

        if (timer_ticks - start > FTP_DATA_TIMEOUT) {
            kprintf("[FTP] Data receive timeout\n");
            break;
        }

        char c;
        int r = tcp_recv(session->data_sock, &c, 1);

        if (r > 0) {
            start = timer_ticks;

            if (c == '\n') {
                line[line_pos] = '\0';
                // Strip CR
                if (line_pos > 0 && line[line_pos - 1] == '\r') {
                    line[line_pos - 1] = '\0';
                }

                if (callback) {
                    callback(line, user_data);
                } else {
                    kprintf("%s\n", line);
                }

                line_pos = 0;
            } else if (line_pos < (int)sizeof(line) - 1) {
                line[line_pos++] = c;
            }
        } else if (r < 0 && r != TCP_ERR_WOULD_BLOCK) {
            break;
        }

        tcp_state_t state = tcp_get_state(session->data_sock);
        if (state == TCP_STATE_CLOSED || state == TCP_STATE_CLOSE_WAIT ||
            state == TCP_STATE_TIME_WAIT) {
            // Output any remaining data
            if (line_pos > 0) {
                line[line_pos] = '\0';
                if (callback) {
                    callback(line, user_data);
                } else {
                    kprintf("%s\n", line);
                }
            }
            break;
        }

        ftp_delay(100);
    }

    ftp_close_data(session);

    // Get transfer complete response
    resp = ftp_recv_response(session);
    if (resp != FTP_RESP_TRANSFER_OK) {
        kprintf("[FTP] Warning: unexpected response after LIST: %d\n", resp);
    }

    return FTP_SUCCESS;
}

// Download file
int ftp_get(ftp_session_t *session, const char *remote_path, uint8_t **data_out, uint32_t *size_out) {
    if (!session || !session->logged_in) {
        return FTP_ERR_NOT_CONNECTED;
    }

    if (!data_out || !size_out) {
        return FTP_ERR_COMMAND_FAILED;
    }

    *data_out = NULL;
    *size_out = 0;

    // Enter passive mode
    int ret = ftp_enter_pasv(session);
    if (ret < 0) {
        return ret;
    }

    // Send RETR command
    char cmd[FTP_PATH_MAX + 8];
    strcpy(cmd, "RETR ");
    strcat(cmd, remote_path);

    int resp = ftp_send_command(session, cmd);
    if (resp < 0) {
        ftp_close_data(session);
        return resp;
    }

    if (resp != FTP_RESP_FILE_OK && resp != FTP_RESP_DATA_CONN_OPEN) {
        kprintf("[FTP] RETR failed: %d\n", resp);
        ftp_close_data(session);
        return FTP_ERR_TRANSFER_FAILED;
    }

    // Allocate initial buffer
    uint32_t buffer_size = FTP_DATA_BUFFER_SIZE;
    uint8_t *buffer = (uint8_t *)kmalloc(buffer_size);
    if (!buffer) {
        ftp_close_data(session);
        return FTP_ERR_NO_MEMORY;
    }

    uint32_t received = 0;
    uint64_t start = timer_ticks;

    // Receive file data
    while (1) {
        net_poll();
        tcp_timer();

        if (timer_ticks - start > FTP_DATA_TIMEOUT) {
            kprintf("[FTP] Data receive timeout\n");
            break;
        }

        // Expand buffer if needed
        if (received + 1024 > buffer_size) {
            uint32_t new_size = buffer_size * 2;
            uint8_t *new_buf = (uint8_t *)kmalloc(new_size);
            if (!new_buf) {
                kprintf("[FTP] Out of memory during transfer\n");
                break;
            }
            memcpy(new_buf, buffer, received);
            kfree(buffer);
            buffer = new_buf;
            buffer_size = new_size;
        }

        int r = tcp_recv(session->data_sock, buffer + received, buffer_size - received);

        if (r > 0) {
            start = timer_ticks;
            received += r;
            // Progress indication
            if ((received % 4096) == 0) {
                kprintf("[FTP] Received %u bytes...\n", received);
            }
        } else if (r < 0 && r != TCP_ERR_WOULD_BLOCK) {
            break;
        }

        tcp_state_t state = tcp_get_state(session->data_sock);
        if (state == TCP_STATE_CLOSED || state == TCP_STATE_CLOSE_WAIT ||
            state == TCP_STATE_TIME_WAIT) {
            break;
        }

        ftp_delay(100);
    }

    ftp_close_data(session);

    // Get transfer complete response
    resp = ftp_recv_response(session);
    if (resp != FTP_RESP_TRANSFER_OK) {
        kprintf("[FTP] Warning: unexpected response after RETR: %d\n", resp);
    }

    *data_out = buffer;
    *size_out = received;

    kprintf("[FTP] Download complete: %u bytes\n", received);
    return FTP_SUCCESS;
}

// Upload file
int ftp_put(ftp_session_t *session, const char *remote_path, const uint8_t *data, uint32_t size) {
    if (!session || !session->logged_in) {
        return FTP_ERR_NOT_CONNECTED;
    }

    if (!data || size == 0) {
        return FTP_ERR_COMMAND_FAILED;
    }

    // Enter passive mode
    int ret = ftp_enter_pasv(session);
    if (ret < 0) {
        return ret;
    }

    // Send STOR command
    char cmd[FTP_PATH_MAX + 8];
    strcpy(cmd, "STOR ");
    strcat(cmd, remote_path);

    int resp = ftp_send_command(session, cmd);
    if (resp < 0) {
        ftp_close_data(session);
        return resp;
    }

    if (resp != FTP_RESP_FILE_OK && resp != FTP_RESP_DATA_CONN_OPEN) {
        kprintf("[FTP] STOR failed: %d\n", resp);
        ftp_close_data(session);
        return FTP_ERR_TRANSFER_FAILED;
    }

    // Send file data
    uint32_t sent = 0;
    uint64_t start = timer_ticks;

    while (sent < size) {
        net_poll();
        tcp_timer();

        if (timer_ticks - start > FTP_DATA_TIMEOUT) {
            kprintf("[FTP] Data send timeout\n");
            ftp_close_data(session);
            return FTP_ERR_TIMEOUT;
        }

        int chunk = size - sent;
        if (chunk > 1024) chunk = 1024;

        int s = tcp_send(session->data_sock, data + sent, chunk);

        if (s > 0) {
            start = timer_ticks;
            sent += s;
            // Progress indication
            if ((sent % 4096) == 0) {
                kprintf("[FTP] Sent %u/%u bytes...\n", sent, size);
            }
        } else if (s < 0 && s != TCP_ERR_WOULD_BLOCK) {
            kprintf("[FTP] Send error: %d\n", s);
            ftp_close_data(session);
            return FTP_ERR_TRANSFER_FAILED;
        }

        ftp_delay(100);
    }

    ftp_close_data(session);

    // Get transfer complete response
    resp = ftp_recv_response(session);
    if (resp != FTP_RESP_TRANSFER_OK) {
        kprintf("[FTP] Warning: unexpected response after STOR: %d\n", resp);
    }

    kprintf("[FTP] Upload complete: %u bytes\n", sent);
    return FTP_SUCCESS;
}

// Set transfer type
int ftp_set_type(ftp_session_t *session, ftp_transfer_type_t type) {
    if (!session || !session->logged_in) {
        return FTP_ERR_NOT_CONNECTED;
    }

    char cmd[16];
    strcpy(cmd, "TYPE ");
    cmd[5] = (char)type;
    cmd[6] = '\0';

    int resp = ftp_send_command(session, cmd);
    if (resp < 0) return resp;

    if (resp == FTP_RESP_OK) {
        session->transfer_type = type;
        return FTP_SUCCESS;
    }

    return FTP_ERR_COMMAND_FAILED;
}

// Get file size
int ftp_size(ftp_session_t *session, const char *path, uint32_t *size_out) {
    if (!session || !session->logged_in) {
        return FTP_ERR_NOT_CONNECTED;
    }

    char cmd[FTP_PATH_MAX + 8];
    strcpy(cmd, "SIZE ");
    strcat(cmd, path);

    int resp = ftp_send_command(session, cmd);
    if (resp < 0) return resp;

    if (resp == 213) {  // SIZE response code
        // Parse size from response "213 size"
        const char *p = session->response_buffer + 4;
        while (*p == ' ') p++;
        *size_out = 0;
        while (*p >= '0' && *p <= '9') {
            *size_out = *size_out * 10 + (*p - '0');
            p++;
        }
        return FTP_SUCCESS;
    }

    return FTP_ERR_COMMAND_FAILED;
}

// Delete file
int ftp_delete(ftp_session_t *session, const char *path) {
    if (!session || !session->logged_in) {
        return FTP_ERR_NOT_CONNECTED;
    }

    char cmd[FTP_PATH_MAX + 8];
    strcpy(cmd, "DELE ");
    strcat(cmd, path);

    int resp = ftp_send_command(session, cmd);
    if (resp < 0) return resp;

    if (resp == FTP_RESP_FILE_ACTION_OK) {
        return FTP_SUCCESS;
    }

    return FTP_ERR_COMMAND_FAILED;
}

// Create directory
int ftp_mkdir(ftp_session_t *session, const char *path) {
    if (!session || !session->logged_in) {
        return FTP_ERR_NOT_CONNECTED;
    }

    char cmd[FTP_PATH_MAX + 8];
    strcpy(cmd, "MKD ");
    strcat(cmd, path);

    int resp = ftp_send_command(session, cmd);
    if (resp < 0) return resp;

    if (resp == FTP_RESP_PATHNAME) {
        return FTP_SUCCESS;
    }

    return FTP_ERR_COMMAND_FAILED;
}

// Remove directory
int ftp_rmdir(ftp_session_t *session, const char *path) {
    if (!session || !session->logged_in) {
        return FTP_ERR_NOT_CONNECTED;
    }

    char cmd[FTP_PATH_MAX + 8];
    strcpy(cmd, "RMD ");
    strcat(cmd, path);

    int resp = ftp_send_command(session, cmd);
    if (resp < 0) return resp;

    if (resp == FTP_RESP_FILE_ACTION_OK) {
        return FTP_SUCCESS;
    }

    return FTP_ERR_COMMAND_FAILED;
}

// Rename file
int ftp_rename(ftp_session_t *session, const char *from, const char *to) {
    if (!session || !session->logged_in) {
        return FTP_ERR_NOT_CONNECTED;
    }

    // RNFR (rename from)
    char cmd[FTP_PATH_MAX + 8];
    strcpy(cmd, "RNFR ");
    strcat(cmd, from);

    int resp = ftp_send_command(session, cmd);
    if (resp < 0) return resp;

    if (resp != FTP_RESP_PENDING) {
        return FTP_ERR_COMMAND_FAILED;
    }

    // RNTO (rename to)
    strcpy(cmd, "RNTO ");
    strcat(cmd, to);

    resp = ftp_send_command(session, cmd);
    if (resp < 0) return resp;

    if (resp == FTP_RESP_FILE_ACTION_OK) {
        return FTP_SUCCESS;
    }

    return FTP_ERR_COMMAND_FAILED;
}

// Get system type
int ftp_syst(ftp_session_t *session, char *syst_out, int max_len) {
    if (!session || session->state < FTP_STATE_CONNECTED) {
        return FTP_ERR_NOT_CONNECTED;
    }

    int resp = ftp_send_command(session, "SYST");
    if (resp < 0) return resp;

    if (resp == FTP_RESP_SYSTEM) {
        // Copy response after code
        const char *p = session->response_buffer + 4;
        int i = 0;
        while (*p && *p != '\n' && i < max_len - 1) {
            syst_out[i++] = *p++;
        }
        syst_out[i] = '\0';
        return FTP_SUCCESS;
    }

    return FTP_ERR_COMMAND_FAILED;
}

// Send NOOP
int ftp_noop(ftp_session_t *session) {
    if (!session || session->state < FTP_STATE_CONNECTED) {
        return FTP_ERR_NOT_CONNECTED;
    }

    int resp = ftp_send_command(session, "NOOP");
    if (resp < 0) return resp;

    if (resp == FTP_RESP_OK) {
        return FTP_SUCCESS;
    }

    return FTP_ERR_COMMAND_FAILED;
}

// Send raw command
int ftp_command(ftp_session_t *session, const char *cmd) {
    if (!session || session->state < FTP_STATE_CONNECTED) {
        return FTP_ERR_NOT_CONNECTED;
    }

    return ftp_send_command(session, cmd);
}

// Get last response code
int ftp_get_response_code(ftp_session_t *session) {
    return session ? session->response_code : 0;
}

// Get last response message
const char *ftp_get_response_msg(ftp_session_t *session) {
    return session ? session->response_buffer : "";
}

// Check if connected
int ftp_is_connected(ftp_session_t *session) {
    return session && session->state >= FTP_STATE_CONNECTED;
}

// Check if logged in
int ftp_is_logged_in(ftp_session_t *session) {
    return session && session->logged_in;
}

// Parse IP address
uint32_t ftp_parse_ip(const char *ip_str) {
    uint8_t octets[4] = {0};
    int octet_idx = 0;
    int value = 0;

    for (const char *c = ip_str; octet_idx < 4; c++) {
        if (*c >= '0' && *c <= '9') {
            value = value * 10 + (*c - '0');
            if (value > 255) return 0;
        } else if (*c == '.' || *c == '\0' || *c == '/' || *c == ':') {
            octets[octet_idx++] = (uint8_t)value;
            value = 0;
            if (*c != '.') break;
        } else {
            return 0;
        }
    }

    if (octet_idx != 4) return 0;

    return ((uint32_t)octets[0] << 24) | ((uint32_t)octets[1] << 16) |
           ((uint32_t)octets[2] << 8) | (uint32_t)octets[3];
}

// Get error string
const char *ftp_strerror(int error) {
    switch (error) {
        case FTP_SUCCESS:               return "Success";
        case FTP_ERR_NO_MEMORY:         return "Out of memory";
        case FTP_ERR_CONNECT_FAILED:    return "Connection failed";
        case FTP_ERR_TIMEOUT:           return "Connection timeout";
        case FTP_ERR_NOT_CONNECTED:     return "Not connected";
        case FTP_ERR_LOGIN_FAILED:      return "Login failed";
        case FTP_ERR_COMMAND_FAILED:    return "Command failed";
        case FTP_ERR_TRANSFER_FAILED:   return "Transfer failed";
        case FTP_ERR_PASV_FAILED:       return "Passive mode failed";
        case FTP_ERR_NO_NETWORK:        return "No network configured";
        case FTP_ERR_INVALID_RESPONSE:  return "Invalid server response";
        case FTP_ERR_FILE_ERROR:        return "File error";
        case FTP_ERR_ALREADY_CONNECTED: return "Already connected";
        default:                        return "Unknown error";
    }
}
