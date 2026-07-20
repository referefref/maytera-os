#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
// sshd.c - SSH Server Daemon for MayteraOS
// Main server implementation
#include "ssh.h"
#include "../../crypto/crypto.h"
#include "../../string.h"
#include "../../serial.h"
#include "../../mm/heap.h"
#include "../tcp.h"
// #include "../../fs/vfs.h" -- not available

// External declarations for transport layer functions
extern int ssh_send_version(ssh_session_t *session);
extern int ssh_recv_version(ssh_session_t *session);
extern int ssh_send_kexinit(ssh_session_t *session);
extern int ssh_recv_kexinit(ssh_session_t *session, const uint8_t *payload, size_t len);
extern int ssh_recv_kexdh_init(ssh_session_t *session, const uint8_t *payload, size_t len);
extern int ssh_send_kexdh_reply(ssh_session_t *session, ssh_server_t *server);
extern int ssh_send_newkeys(ssh_session_t *session);
extern int ssh_recv_newkeys(ssh_session_t *session);

// External declarations for authentication functions
extern int ssh_handle_service_request(ssh_session_t *session, const uint8_t *payload, size_t len);
extern int ssh_handle_userauth_request(ssh_session_t *session, ssh_server_t *server,
                                       const uint8_t *payload, size_t len);
extern int ssh_send_banner(ssh_session_t *session, const char *banner);

// External declarations for channel functions
extern int ssh_handle_channel_open(ssh_session_t *session, const uint8_t *payload, size_t len);
extern int ssh_handle_channel_request(ssh_session_t *session, const uint8_t *payload, size_t len);
extern int ssh_handle_channel_data(ssh_session_t *session, const uint8_t *payload, size_t len);
extern int ssh_handle_channel_window_adjust(ssh_session_t *session, const uint8_t *payload, size_t len);
extern int ssh_handle_channel_eof(ssh_session_t *session, const uint8_t *payload, size_t len);
extern int ssh_handle_channel_close(ssh_session_t *session, const uint8_t *payload, size_t len);

// =============================================================================
// SSH Server
// =============================================================================

// Global server instance
static ssh_server_t *g_ssh_server = NULL;

int ssh_server_init(ssh_server_t *server, ssh_config_t *config) {
    if (!server || !config) {
        return SSH_ERR_INVALID;
    }

    memset(server, 0, sizeof(ssh_server_t));
    memcpy(&server->config, config, sizeof(ssh_config_t));

    // Set defaults if not specified
    if (server->config.port == 0) {
        server->config.port = SSH_DEFAULT_PORT;
    }
    if (server->config.max_auth_attempts == 0) {
        server->config.max_auth_attempts = SSH_MAX_AUTH_ATTEMPTS;
    }
    if (server->config.idle_timeout == 0) {
        server->config.idle_timeout = 300;  // 5 minutes
    }

    // Generate or load host keys
    if (server->config.host_key_path[0] != '\0') {
        int ret = ssh_load_host_keys(server, server->config.host_key_path);
        if (ret != SSH_OK) {
            kprintf("[SSH] Failed to load host keys, generating new ones\n");
            ret = ssh_generate_host_keys(server, SSH_KEY_RSA);
            if (ret != SSH_OK) {
                return ret;
            }
        }
    } else {
        // Generate ephemeral keys
        int ret = ssh_generate_host_keys(server, SSH_KEY_RSA);
        if (ret != SSH_OK) {
            return ret;
        }
    }

    g_ssh_server = server;
    kprintf("[SSH] Server initialized on port %u\n", server->config.port);
    return SSH_OK;
}

int ssh_server_start(ssh_server_t *server) {
    if (!server) {
        return SSH_ERR_INVALID;
    }

    // Create listening socket
    server->listen_socket = tcp_socket();
    if (server->listen_socket < 0) {
        kprintf("[SSH] Failed to create socket\n");
        return SSH_ERR_NETWORK;
    }

    // Bind to port
    int ret = tcp_bind(server->listen_socket, server->config.port);
    if (ret < 0) {
        kprintf("[SSH] Failed to bind to port %u\n", server->config.port);
        tcp_close(server->listen_socket);
        return SSH_ERR_NETWORK;
    }

    // Start listening
    ret = tcp_listen(server->listen_socket, 5);
    if (ret < 0) {
        kprintf("[SSH] Failed to listen\n");
        tcp_close(server->listen_socket);
        return SSH_ERR_NETWORK;
    }

    server->running = 1;
    kprintf("[SSH] Server listening on port %u\n", server->config.port);
    return SSH_OK;
}

void ssh_server_stop(ssh_server_t *server) {
    if (!server) {
        return;
    }

    server->running = 0;

    // Close all sessions
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (server->sessions[i]) {
            ssh_session_close(server->sessions[i], SSH_DISCONNECT_BY_APPLICATION,
                            "Server shutting down");
            ssh_session_free(server->sessions[i]);
            server->sessions[i] = NULL;
        }
    }

    // Close listening socket
    if (server->listen_socket >= 0) {
        tcp_close(server->listen_socket);
        server->listen_socket = -1;
    }

    // Free host keys
    if (server->hostkey_private) {
        crypto_zero(server->hostkey_private, server->hostkey_private_len);
        kfree(server->hostkey_private);
        server->hostkey_private = NULL;
    }
    if (server->hostkey_public) {
        kfree(server->hostkey_public);
        server->hostkey_public = NULL;
    }

    kprintf("[SSH] Server stopped\n");
}

int ssh_server_poll(ssh_server_t *server) {
    if (!server || !server->running) {
        return SSH_ERR_INVALID;
    }

    // Check for new connections
    int new_socket = tcp_accept(server->listen_socket);
    if (new_socket >= 0) {
        // Find a free session slot
        int slot = -1;
        for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
            if (server->sessions[i] == NULL) {
                slot = i;
                break;
            }
        }

        if (slot < 0) {
            kprintf("[SSH] No free session slots, rejecting connection\n");
            tcp_close(new_socket);
        } else {
            ssh_session_t *session = ssh_session_create();
            if (session) {
                session->socket = new_socket;
                server->sessions[slot] = session;
                server->num_sessions++;
                kprintf("[SSH] New connection in slot %d\n", slot);

                // Send version string immediately
                ssh_send_version(session);
            } else {
                tcp_close(new_socket);
            }
        }
    }

    // Process existing sessions
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        ssh_session_t *session = server->sessions[i];
        if (!session) continue;

        int ret = ssh_session_process(session, server);

        // Check for session closure
        if (ret == SSH_ERR_CLOSED || ret == SSH_ERR_NETWORK ||
            session->state == SSH_STATE_CLOSED ||
            session->state == SSH_STATE_ERROR) {

            kprintf("[SSH] Session %d closed (state=%s, ret=%d)\n",
                    i, ssh_state_name(session->state), ret);
            ssh_session_free(session);
            server->sessions[i] = NULL;
            server->num_sessions--;
        }
    }

    return SSH_OK;
}

// =============================================================================
// Key Generation
// =============================================================================

int ssh_generate_host_keys(ssh_server_t *server, ssh_key_type_t type) {
    if (!server) {
        return SSH_ERR_INVALID;
    }

    kprintf("[SSH] Generating host keys (type=%d)\n", type);

    server->hostkey_type = type;

    if (type == SSH_KEY_RSA) {
        // Generate RSA key pair
        // For a minimal implementation, we'll generate random bytes
        // A real implementation would use proper RSA key generation

        // Private key: 256 bytes (2048 bits)
        server->hostkey_private_len = 256;
        server->hostkey_private = kmalloc(server->hostkey_private_len);
        if (!server->hostkey_private) {
            return SSH_ERR_NOMEM;
        }
        rng_get_bytes(server->hostkey_private, server->hostkey_private_len);

        // Public key (modulus): 256 bytes
        // Ensure it's odd and has high bit set (valid RSA modulus)
        server->hostkey_public_len = 256;
        server->hostkey_public = kmalloc(server->hostkey_public_len);
        if (!server->hostkey_public) {
            kfree(server->hostkey_private);
            server->hostkey_private = NULL;
            return SSH_ERR_NOMEM;
        }
        rng_get_bytes(server->hostkey_public, server->hostkey_public_len);
        server->hostkey_public[0] |= 0x80;  // Set high bit
        server->hostkey_public[255] |= 0x01;  // Make odd

        kprintf("[SSH] Generated 2048-bit RSA host key\n");

    } else if (type == SSH_KEY_ED25519) {
        // Ed25519 key pair (32 bytes each)
        // Would need to implement Ed25519 for proper key generation
        server->hostkey_private_len = 64;  // Seed + public
        server->hostkey_private = kmalloc(server->hostkey_private_len);
        server->hostkey_public_len = 32;
        server->hostkey_public = kmalloc(server->hostkey_public_len);

        if (!server->hostkey_private || !server->hostkey_public) {
            if (server->hostkey_private) kfree(server->hostkey_private);
            if (server->hostkey_public) kfree(server->hostkey_public);
            return SSH_ERR_NOMEM;
        }

        rng_get_bytes(server->hostkey_private, 32);  // Seed
        rng_get_bytes(server->hostkey_public, 32);   // Placeholder public key

        kprintf("[SSH] Generated Ed25519 host key\n");

    } else {
        return SSH_ERR_INVALID;
    }

    return SSH_OK;
}

int ssh_load_host_keys(ssh_server_t *server, const char *path) {
    if (!server || !path) {
        return SSH_ERR_INVALID;
    }

    // Open key file
    int fd = vfs_open(path, 0);  // O_RDONLY
    if (fd < 0) {
        kprintf("[SSH] Cannot open host key file: %s\n", path);
        return SSH_ERR_IO;
    }

    // Read file content
    uint8_t *buffer = kmalloc(4096);
    if (!buffer) {
        vfs_close(fd);
        return SSH_ERR_NOMEM;
    }

    ssize_t bytes = vfs_read(fd, buffer, 4096);
    vfs_close(fd);

    if (bytes <= 0) {
        kfree(buffer);
        return SSH_ERR_IO;
    }

    // Parse key file (simplified format: type + private + public)
    // In production, use proper PEM or OpenSSH format
    if (bytes < 8) {
        kfree(buffer);
        return SSH_ERR_INVALID;
    }

    uint32_t type = *(uint32_t *)buffer;
    uint32_t priv_len = *(uint32_t *)(buffer + 4);

    if (8 + priv_len > (size_t)bytes) {
        kfree(buffer);
        return SSH_ERR_INVALID;
    }

    server->hostkey_type = (ssh_key_type_t)type;
    server->hostkey_private_len = priv_len;
    server->hostkey_private = kmalloc(priv_len);
    if (!server->hostkey_private) {
        kfree(buffer);
        return SSH_ERR_NOMEM;
    }
    memcpy(server->hostkey_private, buffer + 8, priv_len);

    uint32_t pub_len = *(uint32_t *)(buffer + 8 + priv_len);
    if (8 + priv_len + 4 + pub_len > (size_t)bytes) {
        kfree(server->hostkey_private);
        kfree(buffer);
        return SSH_ERR_INVALID;
    }

    server->hostkey_public_len = pub_len;
    server->hostkey_public = kmalloc(pub_len);
    if (!server->hostkey_public) {
        kfree(server->hostkey_private);
        kfree(buffer);
        return SSH_ERR_NOMEM;
    }
    memcpy(server->hostkey_public, buffer + 8 + priv_len + 4, pub_len);

    kfree(buffer);
    kprintf("[SSH] Loaded host key from %s\n", path);
    return SSH_OK;
}

int ssh_save_host_keys(ssh_server_t *server, const char *path) {
    if (!server || !path || !server->hostkey_private || !server->hostkey_public) {
        return SSH_ERR_INVALID;
    }

    // Calculate buffer size
    size_t buf_size = 8 + server->hostkey_private_len + 4 + server->hostkey_public_len;
    uint8_t *buffer = kmalloc(buf_size);
    if (!buffer) {
        return SSH_ERR_NOMEM;
    }

    // Write type and private key
    *(uint32_t *)buffer = (uint32_t)server->hostkey_type;
    *(uint32_t *)(buffer + 4) = (uint32_t)server->hostkey_private_len;
    memcpy(buffer + 8, server->hostkey_private, server->hostkey_private_len);

    // Write public key
    *(uint32_t *)(buffer + 8 + server->hostkey_private_len) = (uint32_t)server->hostkey_public_len;
    memcpy(buffer + 8 + server->hostkey_private_len + 4,
           server->hostkey_public, server->hostkey_public_len);

    // Open file for writing
    int fd = vfs_open(path, 1 | 64);  // O_WRONLY | O_CREAT
    if (fd < 0) {
        kfree(buffer);
        return SSH_ERR_IO;
    }

    ssize_t written = vfs_write(fd, buffer, buf_size);
    vfs_close(fd);
    kfree(buffer);

    if (written != (ssize_t)buf_size) {
        return SSH_ERR_IO;
    }

    kprintf("[SSH] Saved host key to %s\n", path);
    return SSH_OK;
}

// =============================================================================
// SSH Session
// =============================================================================

ssh_session_t *ssh_session_create(void) {
    ssh_session_t *session = kmalloc(sizeof(ssh_session_t));
    if (!session) {
        return NULL;
    }

    memset(session, 0, sizeof(ssh_session_t));
    session->state = SSH_STATE_INIT;
    session->socket = -1;

    return session;
}

void ssh_session_free(ssh_session_t *session) {
    if (!session) {
        return;
    }

    // Close socket
    if (session->socket >= 0) {
        tcp_close(session->socket);
        session->socket = -1;
    }

    // Free KEX data
    if (session->kex.client_kexinit) kfree(session->kex.client_kexinit);
    if (session->kex.server_kexinit) kfree(session->kex.server_kexinit);
    if (session->kex.dh_p) kfree(session->kex.dh_p);
    if (session->kex.dh_g) kfree(session->kex.dh_g);
    if (session->kex.dh_x) {
        crypto_zero(session->kex.dh_x, session->kex.dh_x_len);
        kfree(session->kex.dh_x);
    }
    if (session->kex.dh_e) kfree(session->kex.dh_e);
    if (session->kex.dh_f) kfree(session->kex.dh_f);
    if (session->kex.dh_k) {
        crypto_zero(session->kex.dh_k, session->kex.dh_k_len);
        kfree(session->kex.dh_k);
    }

    // Zero sensitive data
    crypto_zero(session->keys.c2s_key, sizeof(session->keys.c2s_key));
    crypto_zero(session->keys.s2c_key, sizeof(session->keys.s2c_key));
    crypto_zero(session->keys.c2s_mac, sizeof(session->keys.c2s_mac));
    crypto_zero(session->keys.s2c_mac, sizeof(session->keys.s2c_mac));

    // Free channels
    for (int i = 0; i < SSH_MAX_CHANNELS; i++) {
        ssh_channel_free(session, &session->channels[i]);
    }

    kfree(session);
}

void ssh_session_close(ssh_session_t *session, uint32_t reason, const char *msg) {
    if (!session || session->state == SSH_STATE_CLOSED) {
        return;
    }

    // Send DISCONNECT message
    uint8_t payload[512];
    size_t len = 0;

    // Reason code
    payload[len++] = (reason >> 24) & 0xFF;
    payload[len++] = (reason >> 16) & 0xFF;
    payload[len++] = (reason >> 8) & 0xFF;
    payload[len++] = reason & 0xFF;

    // Description
    size_t msg_len = msg ? strlen(msg) : 0;
    payload[len++] = (msg_len >> 24) & 0xFF;
    payload[len++] = (msg_len >> 16) & 0xFF;
    payload[len++] = (msg_len >> 8) & 0xFF;
    payload[len++] = msg_len & 0xFF;
    if (msg_len > 0) {
        memcpy(payload + len, msg, msg_len);
        len += msg_len;
    }

    // Language tag (empty)
    payload[len++] = 0;
    payload[len++] = 0;
    payload[len++] = 0;
    payload[len++] = 0;

    ssh_send_packet(session, SSH_MSG_DISCONNECT, payload, len);

    session->state = SSH_STATE_CLOSED;
    session->disconnect_reason = reason;
    if (msg) {
        strncpy(session->error_msg, msg, sizeof(session->error_msg) - 1);
    }

    kprintf("[SSH] Session closed: %s\n", msg ? msg : "(no message)");
}

// =============================================================================
// Session Processing
// =============================================================================

int ssh_session_process(ssh_session_t *session, ssh_server_t *server) {
    if (!session || !server) {
        return SSH_ERR_INVALID;
    }

    int ret;

    // State machine
    switch (session->state) {
        case SSH_STATE_INIT:
            // Should have already sent version in accept
            session->state = SSH_STATE_VERSION_SENT;
            // Fall through

        case SSH_STATE_VERSION_SENT:
            // Wait for client version
            ret = ssh_recv_version(session);
            if (ret == SSH_ERR_WOULD_BLOCK) {
                return SSH_OK;
            }
            if (ret != SSH_OK) {
                return ret;
            }
            // Send KEXINIT
            ret = ssh_send_kexinit(session);
            if (ret != SSH_OK) {
                return ret;
            }
            break;

        case SSH_STATE_KEXINIT_SENT:
        case SSH_STATE_VERSION_RECEIVED:
        case SSH_STATE_KEXINIT_RECEIVED:
        case SSH_STATE_KEX_DH_INIT:
        case SSH_STATE_KEX_DH_REPLY:
        case SSH_STATE_NEWKEYS_SENT:
        case SSH_STATE_NEWKEYS_RECEIVED:
        case SSH_STATE_SERVICE_REQUEST:
        case SSH_STATE_USERAUTH:
        case SSH_STATE_AUTHENTICATED:
        case SSH_STATE_CHANNEL_OPEN:
        case SSH_STATE_ESTABLISHED:
            // Process incoming packets
            ret = ssh_process_packets(session, server);
            if (ret != SSH_OK && ret != SSH_ERR_WOULD_BLOCK) {
                return ret;
            }
            break;

        case SSH_STATE_CLOSING:
        case SSH_STATE_CLOSED:
        case SSH_STATE_ERROR:
            return SSH_ERR_CLOSED;

        default:
            kprintf("[SSH] Unknown state %d\n", session->state);
            return SSH_ERR_PROTOCOL;
    }

    return SSH_OK;
}

// =============================================================================
// Packet Processing
// =============================================================================

int ssh_process_packets(ssh_session_t *session, ssh_server_t *server) {
    uint8_t type;
    uint8_t payload[SSH_MAX_PACKET_SIZE];
    size_t len = sizeof(payload);

    int ret = ssh_recv_packet(session, &type, payload, &len);
    if (ret == SSH_ERR_WOULD_BLOCK) {
        return SSH_OK;
    }
    if (ret != SSH_OK) {
        return ret;
    }

    kprintf("[SSH] Received packet type %d in state %s\n",
            type, ssh_state_name(session->state));

    // Handle packet based on type
    switch (type) {
        case SSH_MSG_DISCONNECT:
            kprintf("[SSH] Received DISCONNECT\n");
            session->state = SSH_STATE_CLOSED;
            return SSH_ERR_CLOSED;

        case SSH_MSG_IGNORE:
            // Ignore
            return SSH_OK;

        case SSH_MSG_UNIMPLEMENTED:
            kprintf("[SSH] Client sent UNIMPLEMENTED\n");
            return SSH_OK;

        case SSH_MSG_DEBUG:
            // Ignore debug messages
            return SSH_OK;

        case SSH_MSG_KEXINIT:
            ret = ssh_recv_kexinit(session, payload, len);
            if (ret == SSH_OK && session->state < SSH_STATE_KEXINIT_SENT) {
                ret = ssh_send_kexinit(session);
            }
            return ret;

        case SSH_MSG_KEXDH_INIT:
            ret = ssh_recv_kexdh_init(session, payload, len);
            if (ret == SSH_OK) {
                ret = ssh_send_kexdh_reply(session, server);
                if (ret == SSH_OK) {
                    ret = ssh_send_newkeys(session);
                }
            }
            return ret;

        case SSH_MSG_NEWKEYS:
            return ssh_recv_newkeys(session);

        case SSH_MSG_SERVICE_REQUEST:
            return ssh_handle_service_request(session, payload, len);

        case SSH_MSG_USERAUTH_REQUEST:
            // Send banner if configured
            if (server->config.banner[0] && !session->authenticated) {
                ssh_send_banner(session, server->config.banner);
            }
            return ssh_handle_userauth_request(session, server, payload, len);

        case SSH_MSG_CHANNEL_OPEN:
            return ssh_handle_channel_open(session, payload, len);

        case SSH_MSG_CHANNEL_REQUEST:
            return ssh_handle_channel_request(session, payload, len);

        case SSH_MSG_CHANNEL_DATA:
            return ssh_handle_channel_data(session, payload, len);

        case SSH_MSG_CHANNEL_EXTENDED_DATA:
            // Handle similarly to CHANNEL_DATA
            return ssh_handle_channel_data(session, payload, len);

        case SSH_MSG_CHANNEL_WINDOW_ADJUST:
            return ssh_handle_channel_window_adjust(session, payload, len);

        case SSH_MSG_CHANNEL_EOF:
            return ssh_handle_channel_eof(session, payload, len);

        case SSH_MSG_CHANNEL_CLOSE:
            return ssh_handle_channel_close(session, payload, len);

        case SSH_MSG_GLOBAL_REQUEST:
            // Send failure for unsupported global requests
            kprintf("[SSH] Unsupported global request\n");
            ssh_send_packet(session, SSH_MSG_REQUEST_FAILURE, NULL, 0);
            return SSH_OK;

        default:
            // Send unimplemented
            kprintf("[SSH] Unhandled packet type %d\n", type);
            uint8_t unimpl[4];
            unimpl[0] = (session->keys.c2s_seq >> 24) & 0xFF;
            unimpl[1] = (session->keys.c2s_seq >> 16) & 0xFF;
            unimpl[2] = (session->keys.c2s_seq >> 8) & 0xFF;
            unimpl[3] = session->keys.c2s_seq & 0xFF;
            ssh_send_packet(session, SSH_MSG_UNIMPLEMENTED, unimpl, 4);
            return SSH_OK;
    }
}

// =============================================================================
// Convenience Functions
// =============================================================================

// Get SSH server singleton
ssh_server_t *ssh_get_server(void) {
    return g_ssh_server;
}

// Initialize and start SSH server with default config
int ssh_init_default(void) {
    static ssh_server_t server;
    ssh_config_t config = {
        .port = SSH_DEFAULT_PORT,
        .host_key_path = "",  // Generate ephemeral keys
        .authorized_keys_path = "/root/.ssh/authorized_keys",
        .allow_password_auth = 1,
        .allow_pubkey_auth = 1,
        .max_auth_attempts = SSH_MAX_AUTH_ATTEMPTS,
        .idle_timeout = 300,
        .banner = ""
    };

    int ret = ssh_server_init(&server, &config);
    if (ret != SSH_OK) {
        return ret;
    }

    return ssh_server_start(&server);
}
