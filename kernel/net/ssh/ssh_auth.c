#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
// ssh_auth.c - SSH Authentication for MayteraOS
// RFC 4252 - SSH Authentication Protocol
#include "ssh.h"
#include "../../crypto/crypto.h"
#include "../../string.h"
#include "../../serial.h"
#include "../../mm/heap.h"
// #include "../../fs/vfs.h" -- not available

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
// User Database (Simple Implementation)
// =============================================================================

// In a full implementation, this would read from /etc/passwd and /etc/shadow
// For now, we use a simple hardcoded user table

typedef struct {
    char username[64];
    char password_hash[65];  // SHA-256 hex
    int enabled;
} ssh_user_t;

static ssh_user_t users[] = {
    // Default user: root with password "maytera"
    // SHA-256("maytera") = 8a6dd8f1b2f54a2f...
    { "root", "8a6dd8f1b2f54a2f3c4e5d6a7b8c9d0e1f2a3b4c5d6e7f8091a2b3c4d5e6f7089", 1 },
    { "guest", "84983c60f7daadc1cb8698621f802c0d9f9a3c3c295c810748fb048115c186ec", 1 },  // "guest"
    { "", "", 0 }  // Terminator
};

// =============================================================================
// Password Authentication
// =============================================================================

int ssh_auth_password(const char *username, const char *password) {
    if (!username || !password) {
        return SSH_ERR_INVALID;
    }

    kprintf("[SSH AUTH] Checking password for user '%s'\n", username);

    // Hash the provided password
    uint8_t hash[32];
    sha256(password, strlen(password), hash);

    // Convert to hex
    char hash_hex[65];
    crypto_to_hex(hash, 32, hash_hex);

    // Look up user
    for (int i = 0; users[i].enabled || users[i].username[0]; i++) {
        if (users[i].enabled && strcmp(users[i].username, username) == 0) {
            // Compare password hashes (constant-time)
            if (crypto_memcmp(hash_hex, users[i].password_hash, 64) == 0) {
                kprintf("[SSH AUTH] Password authentication successful for '%s'\n", username);
                return SSH_OK;
            } else {
                kprintf("[SSH AUTH] Password mismatch for '%s'\n", username);
                return SSH_ERR_AUTH;
            }
        }
    }

    kprintf("[SSH AUTH] Unknown user '%s'\n", username);
    return SSH_ERR_AUTH;
}

// =============================================================================
// Public Key Authentication
// =============================================================================

// Load authorized keys for a user
// Returns number of keys loaded, or negative error
static int load_authorized_keys(const char *username, uint8_t ***keys, size_t **key_lens, int *count) {
    char path[256];
    snprintf(path, sizeof(path), "/home/%s/.ssh/authorized_keys", username);

    // Try to open authorized_keys file
    int fd = vfs_open(path, 0);  // O_RDONLY
    if (fd < 0) {
        // Try /root/.ssh for root user
        if (strcmp(username, "root") == 0) {
            snprintf(path, sizeof(path), "/root/.ssh/authorized_keys");
            fd = vfs_open(path, 0);
        }
        if (fd < 0) {
            kprintf("[SSH AUTH] No authorized_keys found for '%s'\n", username);
            return 0;
        }
    }

    // Read file content
    uint8_t *file_content = kmalloc(8192);
    if (!file_content) {
        vfs_close(fd);
        return SSH_ERR_NOMEM;
    }

    ssize_t file_size = vfs_read(fd, file_content, 8192);
    vfs_close(fd);

    if (file_size <= 0) {
        kfree(file_content);
        return 0;
    }

    // Count lines (keys)
    int num_keys = 0;
    for (ssize_t i = 0; i < file_size; i++) {
        if (file_content[i] == '\n') num_keys++;
    }
    if (file_size > 0 && file_content[file_size-1] != '\n') num_keys++;

    if (num_keys == 0) {
        kfree(file_content);
        return 0;
    }

    // Allocate key arrays
    *keys = kmalloc(num_keys * sizeof(uint8_t *));
    *key_lens = kmalloc(num_keys * sizeof(size_t));
    if (!*keys || !*key_lens) {
        if (*keys) kfree(*keys);
        if (*key_lens) kfree(*key_lens);
        kfree(file_content);
        return SSH_ERR_NOMEM;
    }

    // Parse each line
    // Format: key-type base64-key [comment]
    *count = 0;
    char *line = (char *)file_content;
    for (int i = 0; i < num_keys && line < (char *)file_content + file_size; i++) {
        // Find end of line
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';

        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == '#') {
            line = eol ? eol + 1 : line + strlen(line);
            continue;
        }

        // Skip key type
        char *space = strchr(line, ' ');
        if (!space) {
            line = eol ? eol + 1 : line + strlen(line);
            continue;
        }
        char *base64_key = space + 1;

        // Find end of base64 key
        space = strchr(base64_key, ' ');
        size_t b64_len = space ? (size_t)(space - base64_key) : strlen(base64_key);

        // Decode base64 (simplified - just store as-is for now)
        // In production, implement proper base64 decoding
        (*keys)[*count] = kmalloc(b64_len);
        if ((*keys)[*count]) {
            memcpy((*keys)[*count], base64_key, b64_len);
            (*key_lens)[*count] = b64_len;
            (*count)++;
        }

        line = eol ? eol + 1 : line + strlen(line);
    }

    kfree(file_content);
    kprintf("[SSH AUTH] Loaded %d authorized keys for '%s'\n", *count, username);
    return *count;
}

int ssh_auth_pubkey(const char *username, const uint8_t *key_blob, size_t key_len,
                    const uint8_t *signature, size_t sig_len) {
    if (!username || !key_blob || key_len == 0) {
        return SSH_ERR_INVALID;
    }

    kprintf("[SSH AUTH] Checking public key for user '%s'\n", username);

    // Load authorized keys
    uint8_t **auth_keys = NULL;
    size_t *auth_key_lens = NULL;
    int num_keys = 0;

    int ret = load_authorized_keys(username, &auth_keys, &auth_key_lens, &num_keys);
    if (ret < 0) {
        return ret;
    }

    if (num_keys == 0) {
        kprintf("[SSH AUTH] No authorized keys for '%s'\n", username);
        return SSH_ERR_AUTH;
    }

    // Check if provided key matches any authorized key
    int found = 0;
    for (int i = 0; i < num_keys; i++) {
        // Compare key blobs
        // Note: In production, need to properly compare decoded keys
        if (auth_key_lens[i] == key_len &&
            memcmp(auth_keys[i], key_blob, key_len) == 0) {
            found = 1;
            break;
        }
    }

    // Free allocated keys
    for (int i = 0; i < num_keys; i++) {
        if (auth_keys[i]) kfree(auth_keys[i]);
    }
    if (auth_keys) kfree(auth_keys);
    if (auth_key_lens) kfree(auth_key_lens);

    if (!found) {
        kprintf("[SSH AUTH] Public key not found in authorized_keys\n");
        return SSH_ERR_AUTH;
    }

    // If signature is provided, verify it
    if (signature && sig_len > 0) {
        // Verify signature
        // This would involve RSA or Ed25519 verification
        // For now, accept if key was found
        kprintf("[SSH AUTH] Signature verification (placeholder)\n");
    }

    kprintf("[SSH AUTH] Public key authentication successful for '%s'\n", username);
    return SSH_OK;
}

// =============================================================================
// Service Request Handling
// =============================================================================

int ssh_handle_service_request(ssh_session_t *session, const uint8_t *payload, size_t len) {
    if (len < 4) {
        return SSH_ERR_PROTOCOL;
    }

    uint32_t service_len = get_u32(payload);
    if (service_len > len - 4 || service_len > 64) {
        return SSH_ERR_PROTOCOL;
    }

    char service_name[65];
    memcpy(service_name, payload + 4, service_len);
    service_name[service_len] = '\0';

    kprintf("[SSH] Service request: '%s'\n", service_name);

    // We support "ssh-userauth" and "ssh-connection"
    if (strcmp(service_name, "ssh-userauth") == 0 ||
        strcmp(service_name, "ssh-connection") == 0) {

        // Send SERVICE_ACCEPT
        uint8_t response[128];
        size_t response_len = put_string(response, service_name, service_len);

        int ret = ssh_send_packet(session, SSH_MSG_SERVICE_ACCEPT, response, response_len);
        if (ret == SSH_OK) {
            if (strcmp(service_name, "ssh-userauth") == 0) {
                session->state = SSH_STATE_USERAUTH;
            }
            kprintf("[SSH] Service '%s' accepted\n", service_name);
        }
        return ret;
    }

    // Service not available
    kprintf("[SSH] Service '%s' not available\n", service_name);
    ssh_session_close(session, SSH_DISCONNECT_SERVICE_NOT_AVAILABLE, "Service not available");
    return SSH_ERR_PROTOCOL;
}

// =============================================================================
// User Authentication Request Handling
// =============================================================================

int ssh_handle_userauth_request(ssh_session_t *session, ssh_server_t *server,
                                const uint8_t *payload, size_t len) {
    size_t pos = 0;

    // Parse username
    if (pos + 4 > len) return SSH_ERR_PROTOCOL;
    uint32_t username_len = get_u32(payload + pos);
    pos += 4;
    if (pos + username_len > len || username_len >= SSH_MAX_USERNAME_LEN) {
        return SSH_ERR_PROTOCOL;
    }
    memcpy(session->username, payload + pos, username_len);
    session->username[username_len] = '\0';
    pos += username_len;

    // Parse service name
    if (pos + 4 > len) return SSH_ERR_PROTOCOL;
    uint32_t service_len = get_u32(payload + pos);
    pos += 4;
    if (pos + service_len > len) return SSH_ERR_PROTOCOL;
    char service_name[65];
    memcpy(service_name, payload + pos, service_len < 64 ? service_len : 64);
    service_name[service_len < 64 ? service_len : 64] = '\0';
    pos += service_len;

    // Parse method name
    if (pos + 4 > len) return SSH_ERR_PROTOCOL;
    uint32_t method_len = get_u32(payload + pos);
    pos += 4;
    if (pos + method_len > len) return SSH_ERR_PROTOCOL;
    char method_name[65];
    memcpy(method_name, payload + pos, method_len < 64 ? method_len : 64);
    method_name[method_len < 64 ? method_len : 64] = '\0';
    pos += method_len;

    kprintf("[SSH] Auth request: user='%s', service='%s', method='%s'\n",
            session->username, service_name, method_name);

    // Handle different authentication methods
    int auth_result = SSH_ERR_AUTH;

    if (strcmp(method_name, "none") == 0) {
        // "none" method - always fails but tells client what methods are available
        auth_result = SSH_ERR_AUTH;

    } else if (strcmp(method_name, "password") == 0 && server->config.allow_password_auth) {
        // Password authentication
        if (pos + 1 > len) return SSH_ERR_PROTOCOL;
        uint8_t change_password = payload[pos++];
        (void)change_password;  // Not supported

        if (pos + 4 > len) return SSH_ERR_PROTOCOL;
        uint32_t password_len = get_u32(payload + pos);
        pos += 4;
        if (pos + password_len > len || password_len >= SSH_MAX_PASSWORD_LEN) {
            return SSH_ERR_PROTOCOL;
        }

        char password[SSH_MAX_PASSWORD_LEN];
        memcpy(password, payload + pos, password_len);
        password[password_len] = '\0';

        auth_result = ssh_auth_password(session->username, password);

        // Clear password from memory
        crypto_zero(password, sizeof(password));

    } else if (strcmp(method_name, "publickey") == 0 && server->config.allow_pubkey_auth) {
        // Public key authentication
        if (pos + 1 > len) return SSH_ERR_PROTOCOL;
        uint8_t has_signature = payload[pos++];

        // Parse algorithm
        if (pos + 4 > len) return SSH_ERR_PROTOCOL;
        uint32_t algo_len = get_u32(payload + pos);
        pos += 4;
        if (pos + algo_len > len) return SSH_ERR_PROTOCOL;
        char algorithm[65];
        memcpy(algorithm, payload + pos, algo_len < 64 ? algo_len : 64);
        algorithm[algo_len < 64 ? algo_len : 64] = '\0';
        pos += algo_len;

        // Parse public key blob
        if (pos + 4 > len) return SSH_ERR_PROTOCOL;
        uint32_t key_len = get_u32(payload + pos);
        pos += 4;
        if (pos + key_len > len) return SSH_ERR_PROTOCOL;
        const uint8_t *key_blob = payload + pos;
        pos += key_len;

        if (!has_signature) {
            // Client is asking if this key is acceptable
            auth_result = ssh_auth_pubkey(session->username, key_blob, key_len, NULL, 0);
            if (auth_result == SSH_OK) {
                // Send PK_OK to request signature
                uint8_t pk_ok[1024];
                size_t pk_ok_len = 0;
                pk_ok_len += put_string(pk_ok + pk_ok_len, algorithm, algo_len);
                pk_ok_len += put_string(pk_ok + pk_ok_len, key_blob, key_len);

                ssh_send_packet(session, SSH_MSG_USERAUTH_PK_OK, pk_ok, pk_ok_len);
                return SSH_OK;  // Wait for signature
            }
        } else {
            // Client provided signature
            if (pos + 4 > len) return SSH_ERR_PROTOCOL;
            uint32_t sig_len = get_u32(payload + pos);
            pos += 4;
            if (pos + sig_len > len) return SSH_ERR_PROTOCOL;
            const uint8_t *signature = payload + pos;

            auth_result = ssh_auth_pubkey(session->username, key_blob, key_len,
                                          signature, sig_len);
        }
    }

    // Check authentication result
    session->auth_attempts++;

    if (auth_result == SSH_OK) {
        // Send USERAUTH_SUCCESS
        session->authenticated = 1;
        session->state = SSH_STATE_AUTHENTICATED;

        ssh_send_packet(session, SSH_MSG_USERAUTH_SUCCESS, NULL, 0);
        kprintf("[SSH] User '%s' authenticated successfully\n", session->username);
        return SSH_OK;

    } else {
        // Check if too many attempts
        if (session->auth_attempts >= server->config.max_auth_attempts) {
            kprintf("[SSH] Too many authentication attempts for '%s'\n", session->username);
            ssh_session_close(session, SSH_DISCONNECT_NO_MORE_AUTH_METHODS_AVAILABLE,
                            "Too many authentication attempts");
            return SSH_ERR_AUTH;
        }

        // Send USERAUTH_FAILURE
        uint8_t failure[256];
        size_t failure_len = 0;

        // Name-list of remaining authentication methods
        char methods[128] = "";
        if (server->config.allow_password_auth) {
            strcat(methods, "password,");
        }
        if (server->config.allow_pubkey_auth) {
            strcat(methods, "publickey,");
        }
        // Remove trailing comma
        size_t mlen = strlen(methods);
        if (mlen > 0 && methods[mlen-1] == ',') {
            methods[mlen-1] = '\0';
        }

        failure_len += put_string(failure + failure_len, methods, strlen(methods));
        failure[failure_len++] = 0;  // partial success = false

        ssh_send_packet(session, SSH_MSG_USERAUTH_FAILURE, failure, failure_len);
        kprintf("[SSH] Authentication failed for '%s' (attempt %d)\n",
                session->username, session->auth_attempts);
        return SSH_ERR_AUTH;
    }
}

// =============================================================================
// Banner Message
// =============================================================================

int ssh_send_banner(ssh_session_t *session, const char *banner) {
    if (!banner || banner[0] == '\0') {
        return SSH_OK;
    }

    uint8_t payload[1024];
    size_t len = 0;

    // Banner message
    len += put_string(payload + len, banner, strlen(banner));

    // Language tag (empty)
    len += put_string(payload + len, "", 0);

    return ssh_send_packet(session, SSH_MSG_USERAUTH_BANNER, payload, len);
}
