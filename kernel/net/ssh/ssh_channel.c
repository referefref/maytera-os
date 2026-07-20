#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
// ssh_channel.c - SSH Channel Management for MayteraOS
// RFC 4254 - SSH Connection Protocol
#include "ssh.h"
#include "../../crypto/crypto.h"
#include "../../string.h"
#include "../../serial.h"
#include "../../mm/heap.h"
#include "../../proc/process.h"

// =============================================================================
// Helper Functions
// =============================================================================

static void put_u32(uint8_t *buf, uint32_t val) {
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

static uint32_t get_u32(const uint8_t *buf) {
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) |
           (uint32_t)buf[3];
}

static size_t put_string(uint8_t *buf, const void *data, size_t len) {
    put_u32(buf, (uint32_t)len);
    if (len > 0 && data) {
        memcpy(buf + 4, data, len);
    }
    return 4 + len;
}

// =============================================================================
// Channel Allocation
// =============================================================================

ssh_channel_t *ssh_channel_alloc(ssh_session_t *session) {
    for (int i = 0; i < SSH_MAX_CHANNELS; i++) {
        if (session->channels[i].state == SSH_CHANNEL_UNUSED) {
            ssh_channel_t *ch = &session->channels[i];
            memset(ch, 0, sizeof(ssh_channel_t));
            ch->state = SSH_CHANNEL_OPENING;
            ch->local_id = i;
            ch->local_window = 2 * 1024 * 1024;  // 2MB window
            ch->local_max_packet = 32768;

            // Allocate I/O buffers
            ch->stdin_cap = 16384;
            ch->stdin_buf = kmalloc(ch->stdin_cap);
            ch->stdout_cap = 16384;
            ch->stdout_buf = kmalloc(ch->stdout_cap);

            if (!ch->stdin_buf || !ch->stdout_buf) {
                if (ch->stdin_buf) kfree(ch->stdin_buf);
                if (ch->stdout_buf) kfree(ch->stdout_buf);
                ch->state = SSH_CHANNEL_UNUSED;
                return NULL;
            }

            session->num_channels++;
            kprintf("[SSH] Allocated channel %d\n", i);
            return ch;
        }
    }
    return NULL;
}

void ssh_channel_free(ssh_session_t *session, ssh_channel_t *channel) {
    if (!channel || channel->state == SSH_CHANNEL_UNUSED) {
        return;
    }

    if (channel->stdin_buf) kfree(channel->stdin_buf);
    if (channel->stdout_buf) kfree(channel->stdout_buf);

    // Kill associated process if any
    if (channel->process_pid > 0) {
        // proc_kill(channel->process_pid);  // Would need to implement
    }

    uint32_t id = channel->local_id;
    memset(channel, 0, sizeof(ssh_channel_t));
    channel->state = SSH_CHANNEL_UNUSED;
    session->num_channels--;

    kprintf("[SSH] Freed channel %u\n", id);
}

ssh_channel_t *ssh_channel_find(ssh_session_t *session, uint32_t local_id) {
    if (local_id >= SSH_MAX_CHANNELS) {
        return NULL;
    }
    ssh_channel_t *ch = &session->channels[local_id];
    if (ch->state == SSH_CHANNEL_UNUSED) {
        return NULL;
    }
    return ch;
}

// =============================================================================
// Channel Open Handling
// =============================================================================

int ssh_handle_channel_open(ssh_session_t *session, const uint8_t *payload, size_t len) {
    size_t pos = 0;

    // Parse channel type
    if (pos + 4 > len) return SSH_ERR_PROTOCOL;
    uint32_t type_len = get_u32(payload + pos);
    pos += 4;
    if (pos + type_len > len || type_len > 30) return SSH_ERR_PROTOCOL;

    char channel_type[32];
    memcpy(channel_type, payload + pos, type_len);
    channel_type[type_len] = '\0';
    pos += type_len;

    // Parse sender channel
    if (pos + 4 > len) return SSH_ERR_PROTOCOL;
    uint32_t sender_channel = get_u32(payload + pos);
    pos += 4;

    // Parse initial window size
    if (pos + 4 > len) return SSH_ERR_PROTOCOL;
    uint32_t initial_window = get_u32(payload + pos);
    pos += 4;

    // Parse maximum packet size
    if (pos + 4 > len) return SSH_ERR_PROTOCOL;
    uint32_t max_packet = get_u32(payload + pos);
    pos += 4;

    kprintf("[SSH] Channel open request: type='%s', sender=%u, window=%u, max_packet=%u\n",
            channel_type, sender_channel, initial_window, max_packet);

    // Only support "session" channels for now
    if (strcmp(channel_type, "session") != 0) {
        // Send CHANNEL_OPEN_FAILURE
        uint8_t failure[256];
        size_t failure_len = 0;
        put_u32(failure + failure_len, sender_channel);
        failure_len += 4;
        put_u32(failure + failure_len, SSH_OPEN_UNKNOWN_CHANNEL_TYPE);
        failure_len += 4;
        failure_len += put_string(failure + failure_len, "Unknown channel type", 20);
        failure_len += put_string(failure + failure_len, "", 0);  // language tag

        ssh_send_packet(session, SSH_MSG_CHANNEL_OPEN_FAILURE, failure, failure_len);
        return SSH_OK;
    }

    // Allocate channel
    ssh_channel_t *channel = ssh_channel_alloc(session);
    if (!channel) {
        // Send CHANNEL_OPEN_FAILURE
        uint8_t failure[256];
        size_t failure_len = 0;
        put_u32(failure + failure_len, sender_channel);
        failure_len += 4;
        put_u32(failure + failure_len, SSH_OPEN_RESOURCE_SHORTAGE);
        failure_len += 4;
        failure_len += put_string(failure + failure_len, "No channels available", 21);
        failure_len += put_string(failure + failure_len, "", 0);

        ssh_send_packet(session, SSH_MSG_CHANNEL_OPEN_FAILURE, failure, failure_len);
        return SSH_OK;
    }

    // Store channel info
    strncpy(channel->type, channel_type, sizeof(channel->type) - 1);
    channel->remote_id = sender_channel;
    channel->remote_window = initial_window;
    channel->remote_max_packet = max_packet;
    channel->state = SSH_CHANNEL_OPEN;

    // Send CHANNEL_OPEN_CONFIRMATION
    uint8_t confirm[256];
    size_t confirm_len = 0;
    put_u32(confirm + confirm_len, channel->remote_id);
    confirm_len += 4;
    put_u32(confirm + confirm_len, channel->local_id);
    confirm_len += 4;
    put_u32(confirm + confirm_len, channel->local_window);
    confirm_len += 4;
    put_u32(confirm + confirm_len, channel->local_max_packet);
    confirm_len += 4;

    ssh_send_packet(session, SSH_MSG_CHANNEL_OPEN_CONFIRMATION, confirm, confirm_len);

    session->state = SSH_STATE_CHANNEL_OPEN;
    kprintf("[SSH] Channel %u opened (remote %u)\n", channel->local_id, channel->remote_id);
    return SSH_OK;
}

// =============================================================================
// Channel Request Handling
// =============================================================================

int ssh_handle_channel_request(ssh_session_t *session, const uint8_t *payload, size_t len) {
    size_t pos = 0;

    // Parse recipient channel
    if (pos + 4 > len) return SSH_ERR_PROTOCOL;
    uint32_t recipient_channel = get_u32(payload + pos);
    pos += 4;

    // Find channel
    ssh_channel_t *channel = ssh_channel_find(session, recipient_channel);
    if (!channel) {
        kprintf("[SSH] Channel request for unknown channel %u\n", recipient_channel);
        return SSH_ERR_PROTOCOL;
    }

    // Parse request type
    if (pos + 4 > len) return SSH_ERR_PROTOCOL;
    uint32_t type_len = get_u32(payload + pos);
    pos += 4;
    if (pos + type_len > len || type_len > 64) return SSH_ERR_PROTOCOL;

    char request_type[65];
    memcpy(request_type, payload + pos, type_len);
    request_type[type_len] = '\0';
    pos += type_len;

    // Parse want_reply
    if (pos + 1 > len) return SSH_ERR_PROTOCOL;
    uint8_t want_reply = payload[pos++];

    kprintf("[SSH] Channel %u request: '%s', want_reply=%d\n",
            recipient_channel, request_type, want_reply);

    int success = 0;

    // Handle different request types
    if (strcmp(request_type, "pty-req") == 0) {
        // PTY request
        // Parse TERM
        if (pos + 4 > len) goto send_reply;
        uint32_t term_len = get_u32(payload + pos);
        pos += 4;
        if (pos + term_len > len || term_len > 30) goto send_reply;
        memcpy(channel->term, payload + pos, term_len);
        channel->term[term_len] = '\0';
        pos += term_len;

        // Parse dimensions
        if (pos + 16 > len) goto send_reply;
        channel->term_width = get_u32(payload + pos); pos += 4;
        channel->term_height = get_u32(payload + pos); pos += 4;
        channel->term_width_px = get_u32(payload + pos); pos += 4;
        channel->term_height_px = get_u32(payload + pos); pos += 4;

        // Skip terminal modes
        channel->pty_allocated = 1;
        success = 1;

        kprintf("[SSH] PTY allocated: TERM=%s, %ux%u\n",
                channel->term, channel->term_width, channel->term_height);

    } else if (strcmp(request_type, "shell") == 0) {
        // Shell request
        if (!channel->shell_started) {
            // Start shell process
            // In a full implementation, fork and exec /bin/sh
            // For now, mark shell as started and handle I/O directly
            channel->shell_started = 1;
            success = 1;

            kprintf("[SSH] Shell started on channel %u\n", channel->local_id);

            // Send welcome message
            const char *welcome = "\r\nWelcome to MayteraOS SSH Server\r\n$ ";
            ssh_channel_send(session, channel, welcome, strlen(welcome));
        }

    } else if (strcmp(request_type, "exec") == 0) {
        // Execute command
        if (pos + 4 > len) goto send_reply;
        uint32_t cmd_len = get_u32(payload + pos);
        pos += 4;
        if (pos + cmd_len > len || cmd_len > 1024) goto send_reply;

        char command[1025];
        memcpy(command, payload + pos, cmd_len);
        command[cmd_len] = '\0';

        kprintf("[SSH] Exec request: '%s'\n", command);

        // Execute command
        // In a full implementation, fork and exec the command
        // For now, just echo it back
        char response[1280];
        int resp_len = snprintf(response, sizeof(response),
                               "Executing: %s\r\n(Command execution not yet implemented)\r\n",
                               command);
        ssh_channel_send(session, channel, response, resp_len);

        // Send exit status
        ssh_channel_send_exit_status(session, channel, 0);
        ssh_channel_send_eof(session, channel);
        success = 1;

    } else if (strcmp(request_type, "subsystem") == 0) {
        // Subsystem request (e.g., sftp)
        if (pos + 4 > len) goto send_reply;
        uint32_t subsys_len = get_u32(payload + pos);
        pos += 4;
        if (pos + subsys_len > len || subsys_len > 64) goto send_reply;

        char subsystem[65];
        memcpy(subsystem, payload + pos, subsys_len);
        subsystem[subsys_len] = '\0';

        kprintf("[SSH] Subsystem request: '%s'\n", subsystem);

        if (strcmp(subsystem, "sftp") == 0) {
            // SFTP subsystem
            // Would need to implement SFTP protocol handler
            success = 0;  // Not yet implemented
        }

    } else if (strcmp(request_type, "env") == 0) {
        // Environment variable
        if (pos + 4 > len) goto send_reply;
        uint32_t name_len = get_u32(payload + pos);
        pos += 4;
        if (pos + name_len > len) goto send_reply;
        pos += name_len;

        if (pos + 4 > len) goto send_reply;
        uint32_t value_len = get_u32(payload + pos);
        pos += 4;
        if (pos + value_len > len) goto send_reply;

        success = 1;  // Accept but ignore

    } else if (strcmp(request_type, "window-change") == 0) {
        // Terminal window size change
        if (pos + 16 > len) goto send_reply;
        channel->term_width = get_u32(payload + pos); pos += 4;
        channel->term_height = get_u32(payload + pos); pos += 4;
        channel->term_width_px = get_u32(payload + pos); pos += 4;
        channel->term_height_px = get_u32(payload + pos); pos += 4;

        success = 1;
        kprintf("[SSH] Window changed to %ux%u\n", channel->term_width, channel->term_height);

    } else if (strcmp(request_type, "signal") == 0) {
        // Signal
        success = 1;  // Accept but ignore

    } else if (strcmp(request_type, "exit-status") == 0) {
        // Exit status (sent by client, rare)
        success = 1;
    }

send_reply:
    if (want_reply) {
        if (success) {
            ssh_send_packet(session, SSH_MSG_CHANNEL_SUCCESS, &recipient_channel, 4);
        } else {
            ssh_send_packet(session, SSH_MSG_CHANNEL_FAILURE, &recipient_channel, 4);
        }
    }

    if (success) {
        session->state = SSH_STATE_ESTABLISHED;
    }

    return SSH_OK;
}

// =============================================================================
// Channel Data Handling
// =============================================================================

int ssh_handle_channel_data(ssh_session_t *session, const uint8_t *payload, size_t len) {
    size_t pos = 0;

    // Parse recipient channel
    if (pos + 4 > len) return SSH_ERR_PROTOCOL;
    uint32_t recipient_channel = get_u32(payload + pos);
    pos += 4;

    // Find channel
    ssh_channel_t *channel = ssh_channel_find(session, recipient_channel);
    if (!channel) {
        kprintf("[SSH] Data for unknown channel %u\n", recipient_channel);
        return SSH_ERR_PROTOCOL;
    }

    // Parse data
    if (pos + 4 > len) return SSH_ERR_PROTOCOL;
    uint32_t data_len = get_u32(payload + pos);
    pos += 4;
    if (pos + data_len > len) return SSH_ERR_PROTOCOL;

    const uint8_t *data = payload + pos;

    // Adjust window
    if (data_len > channel->local_window) {
        kprintf("[SSH] Window overflow on channel %u\n", recipient_channel);
        // Could close channel here
    } else {
        channel->local_window -= data_len;
    }

    // Process data
    if (channel->shell_started) {
        // Echo data back (simple shell emulation)
        for (size_t i = 0; i < data_len; i++) {
            uint8_t c = data[i];

            if (c == '\r' || c == '\n') {
                // Newline - process command
                channel->stdin_buf[channel->stdin_len] = '\0';

                // Echo newline
                ssh_channel_send(session, channel, "\r\n", 2);

                // Process simple commands
                if (channel->stdin_len > 0) {
                    char *cmd = (char *)channel->stdin_buf;

                    if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
                        ssh_channel_send(session, channel, "Goodbye!\r\n", 10);
                        ssh_channel_send_exit_status(session, channel, 0);
                        ssh_channel_send_eof(session, channel);
                        ssh_channel_close(session, channel);
                        return SSH_OK;
                    } else if (strcmp(cmd, "help") == 0) {
                        const char *help =
                            "Available commands:\r\n"
                            "  help    - Show this help\r\n"
                            "  whoami  - Show current user\r\n"
                            "  uname   - Show system info\r\n"
                            "  date    - Show current date\r\n"
                            "  exit    - Exit shell\r\n";
                        ssh_channel_send(session, channel, help, strlen(help));
                    } else if (strcmp(cmd, "whoami") == 0) {
                        char resp[128];
                        int resp_len = snprintf(resp, sizeof(resp), "%s\r\n", session->username);
                        ssh_channel_send(session, channel, resp, resp_len);
                    } else if (strcmp(cmd, "uname") == 0 || strncmp(cmd, "uname ", 6) == 0) {
                        ssh_channel_send(session, channel, "MayteraOS 1.0\r\n", 15);
                    } else if (strcmp(cmd, "date") == 0) {
                        ssh_channel_send(session, channel, "Time not implemented\r\n", 22);
                    } else if (strcmp(cmd, "pwd") == 0) {
                        ssh_channel_send(session, channel, "/home/", 6);
                        ssh_channel_send(session, channel, session->username, strlen(session->username));
                        ssh_channel_send(session, channel, "\r\n", 2);
                    } else if (cmd[0] != '\0') {
                        char resp[256];
                        int resp_len = snprintf(resp, sizeof(resp),
                                               "sh: %s: command not found\r\n", cmd);
                        ssh_channel_send(session, channel, resp, resp_len);
                    }
                }

                // Send prompt
                ssh_channel_send(session, channel, "$ ", 2);
                channel->stdin_len = 0;

            } else if (c == 127 || c == 8) {
                // Backspace
                if (channel->stdin_len > 0) {
                    channel->stdin_len--;
                    ssh_channel_send(session, channel, "\b \b", 3);
                }
            } else if (c == 3) {
                // Ctrl+C
                ssh_channel_send(session, channel, "^C\r\n$ ", 6);
                channel->stdin_len = 0;
            } else if (c == 4) {
                // Ctrl+D (EOF)
                if (channel->stdin_len == 0) {
                    ssh_channel_send(session, channel, "\r\nlogout\r\n", 10);
                    ssh_channel_send_exit_status(session, channel, 0);
                    ssh_channel_send_eof(session, channel);
                    ssh_channel_close(session, channel);
                    return SSH_OK;
                }
            } else if (c >= 32 && c < 127) {
                // Printable character
                if (channel->stdin_len < channel->stdin_cap - 1) {
                    channel->stdin_buf[channel->stdin_len++] = c;
                    ssh_channel_send(session, channel, &c, 1);  // Echo
                }
            }
        }
    }

    // Send window adjust if needed
    if (channel->local_window < 1024 * 1024) {
        uint32_t adjust = 2 * 1024 * 1024 - channel->local_window;
        channel->local_window += adjust;

        uint8_t window_adjust[8];
        put_u32(window_adjust, channel->remote_id);
        put_u32(window_adjust + 4, adjust);
        ssh_send_packet(session, SSH_MSG_CHANNEL_WINDOW_ADJUST, window_adjust, 8);
    }

    return SSH_OK;
}

// =============================================================================
// Channel Send Functions
// =============================================================================

int ssh_channel_send(ssh_session_t *session, ssh_channel_t *channel,
                     const void *data, size_t len) {
    if (!channel || channel->state != SSH_CHANNEL_OPEN) {
        return SSH_ERR_INVALID;
    }

    const uint8_t *ptr = (const uint8_t *)data;

    while (len > 0) {
        // Check window
        if (channel->remote_window == 0) {
            // Need to wait for window adjust
            kprintf("[SSH] Channel %u: waiting for window\n", channel->local_id);
            return SSH_ERR_WOULD_BLOCK;
        }

        // Calculate chunk size
        size_t chunk = len;
        if (chunk > channel->remote_window) {
            chunk = channel->remote_window;
        }
        if (chunk > channel->remote_max_packet) {
            chunk = channel->remote_max_packet;
        }

        // Build packet
        uint8_t packet[SSH_MAX_PACKET_SIZE];
        size_t packet_len = 0;
        put_u32(packet + packet_len, channel->remote_id);
        packet_len += 4;
        packet_len += put_string(packet + packet_len, ptr, chunk);

        // Send
        int ret = ssh_send_packet(session, SSH_MSG_CHANNEL_DATA, packet, packet_len);
        if (ret != SSH_OK) {
            return ret;
        }

        channel->remote_window -= chunk;
        ptr += chunk;
        len -= chunk;
    }

    return SSH_OK;
}

int ssh_channel_send_extended(ssh_session_t *session, ssh_channel_t *channel,
                              uint32_t data_type, const void *data, size_t len) {
    if (!channel || channel->state != SSH_CHANNEL_OPEN) {
        return SSH_ERR_INVALID;
    }

    // Build packet
    uint8_t packet[SSH_MAX_PACKET_SIZE];
    size_t packet_len = 0;
    put_u32(packet + packet_len, channel->remote_id);
    packet_len += 4;
    put_u32(packet + packet_len, data_type);
    packet_len += 4;
    packet_len += put_string(packet + packet_len, data, len);

    return ssh_send_packet(session, SSH_MSG_CHANNEL_EXTENDED_DATA, packet, packet_len);
}

int ssh_channel_send_eof(ssh_session_t *session, ssh_channel_t *channel) {
    if (!channel) {
        return SSH_ERR_INVALID;
    }

    if (channel->state == SSH_CHANNEL_EOF_SENT ||
        channel->state == SSH_CHANNEL_CLOSING ||
        channel->state == SSH_CHANNEL_CLOSED) {
        return SSH_OK;
    }

    uint8_t packet[4];
    put_u32(packet, channel->remote_id);

    int ret = ssh_send_packet(session, SSH_MSG_CHANNEL_EOF, packet, 4);
    if (ret == SSH_OK) {
        channel->state = SSH_CHANNEL_EOF_SENT;
        kprintf("[SSH] Sent EOF on channel %u\n", channel->local_id);
    }
    return ret;
}

int ssh_channel_send_exit_status(ssh_session_t *session, ssh_channel_t *channel,
                                 uint32_t exit_status) {
    if (!channel) {
        return SSH_ERR_INVALID;
    }

    uint8_t packet[256];
    size_t packet_len = 0;
    put_u32(packet + packet_len, channel->remote_id);
    packet_len += 4;
    packet_len += put_string(packet + packet_len, "exit-status", 11);
    packet[packet_len++] = 0;  // want_reply = false
    put_u32(packet + packet_len, exit_status);
    packet_len += 4;

    return ssh_send_packet(session, SSH_MSG_CHANNEL_REQUEST, packet, packet_len);
}

int ssh_channel_close(ssh_session_t *session, ssh_channel_t *channel) {
    if (!channel) {
        return SSH_ERR_INVALID;
    }

    if (channel->state == SSH_CHANNEL_CLOSING ||
        channel->state == SSH_CHANNEL_CLOSED) {
        return SSH_OK;
    }

    uint8_t packet[4];
    put_u32(packet, channel->remote_id);

    int ret = ssh_send_packet(session, SSH_MSG_CHANNEL_CLOSE, packet, 4);
    if (ret == SSH_OK) {
        channel->state = SSH_CHANNEL_CLOSING;
        kprintf("[SSH] Closing channel %u\n", channel->local_id);
    }
    return ret;
}

// =============================================================================
// Channel Event Handlers
// =============================================================================

int ssh_handle_channel_window_adjust(ssh_session_t *session, const uint8_t *payload, size_t len) {
    if (len < 8) return SSH_ERR_PROTOCOL;

    uint32_t recipient_channel = get_u32(payload);
    uint32_t bytes_to_add = get_u32(payload + 4);

    ssh_channel_t *channel = ssh_channel_find(session, recipient_channel);
    if (!channel) {
        kprintf("[SSH] Window adjust for unknown channel %u\n", recipient_channel);
        return SSH_ERR_PROTOCOL;
    }

    channel->remote_window += bytes_to_add;
    kprintf("[SSH] Channel %u window increased by %u (now %u)\n",
            recipient_channel, bytes_to_add, channel->remote_window);

    return SSH_OK;
}

int ssh_handle_channel_eof(ssh_session_t *session, const uint8_t *payload, size_t len) {
    if (len < 4) return SSH_ERR_PROTOCOL;

    uint32_t recipient_channel = get_u32(payload);

    ssh_channel_t *channel = ssh_channel_find(session, recipient_channel);
    if (!channel) {
        return SSH_ERR_PROTOCOL;
    }

    kprintf("[SSH] Received EOF on channel %u\n", recipient_channel);
    channel->state = SSH_CHANNEL_EOF_RECEIVED;

    return SSH_OK;
}

int ssh_handle_channel_close(ssh_session_t *session, const uint8_t *payload, size_t len) {
    if (len < 4) return SSH_ERR_PROTOCOL;

    uint32_t recipient_channel = get_u32(payload);

    ssh_channel_t *channel = ssh_channel_find(session, recipient_channel);
    if (!channel) {
        return SSH_ERR_PROTOCOL;
    }

    kprintf("[SSH] Received CLOSE on channel %u\n", recipient_channel);

    // Send CLOSE back if we haven't already
    if (channel->state != SSH_CHANNEL_CLOSING && channel->state != SSH_CHANNEL_CLOSED) {
        uint8_t close_packet[4];
        put_u32(close_packet, channel->remote_id);
        ssh_send_packet(session, SSH_MSG_CHANNEL_CLOSE, close_packet, 4);
    }

    ssh_channel_free(session, channel);
    return SSH_OK;
}
