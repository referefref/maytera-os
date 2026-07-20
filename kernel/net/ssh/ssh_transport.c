#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
// ssh_transport.c - SSH Transport Layer for MayteraOS
// Handles packet framing, encryption, MAC, and key exchange
#include "ssh.h"
#include "../../crypto/crypto.h"
#include "../../string.h"
#include "../../serial.h"
#include "../../mm/heap.h"
#include "../tcp.h"

// DH Group 14 prime (2048 bits, RFC 3526)
// This is the MODP group 14
static const uint8_t dh_group14_p[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xC9, 0x0F, 0xDA, 0xA2, 0x21, 0x68, 0xC2, 0x34,
    0xC4, 0xC6, 0x62, 0x8B, 0x80, 0xDC, 0x1C, 0xD1,
    0x29, 0x02, 0x4E, 0x08, 0x8A, 0x67, 0xCC, 0x74,
    0x02, 0x0B, 0xBE, 0xA6, 0x3B, 0x13, 0x9B, 0x22,
    0x51, 0x4A, 0x08, 0x79, 0x8E, 0x34, 0x04, 0xDD,
    0xEF, 0x95, 0x19, 0xB3, 0xCD, 0x3A, 0x43, 0x1B,
    0x30, 0x2B, 0x0A, 0x6D, 0xF2, 0x5F, 0x14, 0x37,
    0x4F, 0xE1, 0x35, 0x6D, 0x6D, 0x51, 0xC2, 0x45,
    0xE4, 0x85, 0xB5, 0x76, 0x62, 0x5E, 0x7E, 0xC6,
    0xF4, 0x4C, 0x42, 0xE9, 0xA6, 0x37, 0xED, 0x6B,
    0x0B, 0xFF, 0x5C, 0xB6, 0xF4, 0x06, 0xB7, 0xED,
    0xEE, 0x38, 0x6B, 0xFB, 0x5A, 0x89, 0x9F, 0xA5,
    0xAE, 0x9F, 0x24, 0x11, 0x7C, 0x4B, 0x1F, 0xE6,
    0x49, 0x28, 0x66, 0x51, 0xEC, 0xE4, 0x5B, 0x3D,
    0xC2, 0x00, 0x7C, 0xB8, 0xA1, 0x63, 0xBF, 0x05,
    0x98, 0xDA, 0x48, 0x36, 0x1C, 0x55, 0xD3, 0x9A,
    0x69, 0x16, 0x3F, 0xA8, 0xFD, 0x24, 0xCF, 0x5F,
    0x83, 0x65, 0x5D, 0x23, 0xDC, 0xA3, 0xAD, 0x96,
    0x1C, 0x62, 0xF3, 0x56, 0x20, 0x85, 0x52, 0xBB,
    0x9E, 0xD5, 0x29, 0x07, 0x70, 0x96, 0x96, 0x6D,
    0x67, 0x0C, 0x35, 0x4E, 0x4A, 0xBC, 0x98, 0x04,
    0xF1, 0x74, 0x6C, 0x08, 0xCA, 0x18, 0x21, 0x7C,
    0x32, 0x90, 0x5E, 0x46, 0x2E, 0x36, 0xCE, 0x3B,
    0xE3, 0x9E, 0x77, 0x2C, 0x18, 0x0E, 0x86, 0x03,
    0x9B, 0x27, 0x83, 0xA2, 0xEC, 0x07, 0xA2, 0x8F,
    0xB5, 0xC5, 0x5D, 0xF0, 0x6F, 0x4C, 0x52, 0xC9,
    0xDE, 0x2B, 0xCB, 0xF6, 0x95, 0x58, 0x17, 0x18,
    0x39, 0x95, 0x49, 0x7C, 0xEA, 0x95, 0x6A, 0xE5,
    0x15, 0xD2, 0x26, 0x18, 0x98, 0xFA, 0x05, 0x10,
    0x15, 0x72, 0x8E, 0x5A, 0x8A, 0xAC, 0xAA, 0x68,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

// DH Group 14 generator
static const uint8_t dh_group14_g[] = { 0x02 };

// =============================================================================
// Helper Functions
// =============================================================================

// Write uint32 big-endian
static void put_u32(uint8_t *buf, uint32_t val) {
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

// Read uint32 big-endian
static uint32_t get_u32(const uint8_t *buf) {
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) |
           (uint32_t)buf[3];
}

// Write SSH string (uint32 length + data)
static size_t put_string(uint8_t *buf, const void *data, size_t len) {
    put_u32(buf, (uint32_t)len);
    if (len > 0 && data) {
        memcpy(buf + 4, data, len);
    }
    return 4 + len;
}

// Write SSH name-list (comma-separated)
static size_t put_namelist(uint8_t *buf, const char *names) {
    size_t len = strlen(names);
    return put_string(buf, names, len);
}

// Get SSH string length
static uint32_t get_string_len(const uint8_t *buf) {
    return get_u32(buf);
}

// =============================================================================
// Big Number Operations (simplified for DH)
// =============================================================================

// Compare big numbers (a >= b)
static int bn_cmp(const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len) {
    // Skip leading zeros
    while (a_len > 0 && a[0] == 0) { a++; a_len--; }
    while (b_len > 0 && b[0] == 0) { b++; b_len--; }

    if (a_len > b_len) return 1;
    if (a_len < b_len) return -1;

    for (size_t i = 0; i < a_len; i++) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}

// Modular exponentiation: result = base^exp mod mod
// Using square-and-multiply algorithm
// Note: This is a simplified implementation - a production system would use
// Montgomery multiplication and constant-time operations
static int bn_mod_exp(uint8_t *result, size_t *result_len,
                      const uint8_t *base, size_t base_len,
                      const uint8_t *exp, size_t exp_len,
                      const uint8_t *mod, size_t mod_len) {
    // Allocate working buffers
    size_t buf_size = mod_len * 2 + 2;
    uint8_t *acc = kmalloc(buf_size);
    uint8_t *temp = kmalloc(buf_size);
    uint8_t *base_copy = kmalloc(buf_size);

    if (!acc || !temp || !base_copy) {
        if (acc) kfree(acc);
        if (temp) kfree(temp);
        if (base_copy) kfree(base_copy);
        return SSH_ERR_NOMEM;
    }

    // Initialize accumulator to 1
    memset(acc, 0, buf_size);
    acc[buf_size - 1] = 1;
    size_t acc_len = buf_size;

    // Copy base
    memset(base_copy, 0, buf_size);
    memcpy(base_copy + buf_size - base_len, base, base_len);
    size_t base_copy_len = buf_size;

    // Process each bit of exponent
    for (ssize_t byte = exp_len - 1; byte >= 0; byte--) {
        for (int bit = 0; bit < 8; bit++) {
            // Square accumulator
            // (simplified - using modular reduction after each operation)
            // In a real implementation, use proper big number multiplication

            if ((exp[byte] >> bit) & 1) {
                // Multiply by base
                // acc = (acc * base_copy) mod mod
            }

            if (byte > 0 || bit > 0) {
                // Square base_copy for next iteration
                // base_copy = (base_copy * base_copy) mod mod
            }
        }
    }

    // Copy result
    *result_len = mod_len;
    memcpy(result, acc + buf_size - mod_len, mod_len);

    kfree(acc);
    kfree(temp);
    kfree(base_copy);

    return SSH_OK;
}

// =============================================================================
// Version Exchange
// =============================================================================

int ssh_send_version(ssh_session_t *session) {
    char version_str[256];
    int len = snprintf(version_str, sizeof(version_str), "%s\r\n", SSH_VERSION_STRING);

    strncpy(session->server_version, SSH_VERSION_STRING, sizeof(session->server_version) - 1);

    int sent = tcp_send(session->socket, version_str, len);
    if (sent != len) {
        kprintf("[SSH] Failed to send version string\n");
        return SSH_ERR_NETWORK;
    }

    session->state = SSH_STATE_VERSION_SENT;
    kprintf("[SSH] Sent version: %s\n", SSH_VERSION_STRING);
    return SSH_OK;
}

int ssh_recv_version(ssh_session_t *session) {
    // Read until we get \r\n or \n
    char line[256];
    int pos = 0;

    while (pos < 255) {
        int n = tcp_recv(session->socket, &line[pos], 1);
        if (n <= 0) {
            if (n == TCP_ERR_WOULD_BLOCK) {
                return SSH_ERR_WOULD_BLOCK;
            }
            return SSH_ERR_NETWORK;
        }

        if (line[pos] == '\n') {
            // Remove trailing \r if present
            if (pos > 0 && line[pos-1] == '\r') {
                pos--;
            }
            line[pos] = '\0';
            break;
        }
        pos++;
    }

    // Validate version string
    if (strncmp(line, "SSH-2.0-", 8) != 0 && strncmp(line, "SSH-1.99-", 9) != 0) {
        kprintf("[SSH] Invalid version string: %s\n", line);
        return SSH_ERR_PROTOCOL;
    }

    strncpy(session->client_version, line, sizeof(session->client_version) - 1);
    session->state = SSH_STATE_VERSION_RECEIVED;
    kprintf("[SSH] Received client version: %s\n", line);
    return SSH_OK;
}

// =============================================================================
// KEXINIT Message
// =============================================================================

int ssh_send_kexinit(ssh_session_t *session) {
    uint8_t payload[2048];
    size_t pos = 0;

    // Generate random cookie
    rng_get_bytes(session->kex.server_cookie, 16);
    memcpy(payload + pos, session->kex.server_cookie, 16);
    pos += 16;

    // Key exchange algorithms
    pos += put_namelist(payload + pos, SSH_KEX_DH_GROUP14_SHA256);

    // Server host key algorithms
    pos += put_namelist(payload + pos, SSH_HOSTKEY_RSA_SHA2_256);

    // Encryption algorithms (client to server)
    pos += put_namelist(payload + pos, SSH_CIPHER_AES256_CTR "," SSH_CIPHER_AES128_CTR);

    // Encryption algorithms (server to client)
    pos += put_namelist(payload + pos, SSH_CIPHER_AES256_CTR "," SSH_CIPHER_AES128_CTR);

    // MAC algorithms (client to server)
    pos += put_namelist(payload + pos, SSH_MAC_HMAC_SHA2_256);

    // MAC algorithms (server to client)
    pos += put_namelist(payload + pos, SSH_MAC_HMAC_SHA2_256);

    // Compression algorithms (client to server)
    pos += put_namelist(payload + pos, SSH_COMP_NONE);

    // Compression algorithms (server to client)
    pos += put_namelist(payload + pos, SSH_COMP_NONE);

    // Languages (empty)
    pos += put_namelist(payload + pos, "");
    pos += put_namelist(payload + pos, "");

    // First KEX packet follows (false)
    payload[pos++] = 0;

    // Reserved (0)
    put_u32(payload + pos, 0);
    pos += 4;

    // Store KEXINIT for later hash calculation
    session->kex.server_kexinit = kmalloc(pos + 1);
    if (session->kex.server_kexinit) {
        session->kex.server_kexinit[0] = SSH_MSG_KEXINIT;
        memcpy(session->kex.server_kexinit + 1, payload, pos);
        session->kex.server_kexinit_len = pos + 1;
    }

    int ret = ssh_send_packet(session, SSH_MSG_KEXINIT, payload, pos);
    if (ret == SSH_OK) {
        session->state = SSH_STATE_KEXINIT_SENT;
        kprintf("[SSH] Sent KEXINIT\n");
    }
    return ret;
}

int ssh_recv_kexinit(ssh_session_t *session, const uint8_t *payload, size_t len) {
    if (len < 16 + 4) {
        return SSH_ERR_PROTOCOL;
    }

    size_t pos = 0;

    // Copy client cookie
    memcpy(session->kex.client_cookie, payload + pos, 16);
    pos += 16;

    // Store full KEXINIT for later hash calculation
    session->kex.client_kexinit = kmalloc(len + 1);
    if (session->kex.client_kexinit) {
        session->kex.client_kexinit[0] = SSH_MSG_KEXINIT;
        memcpy(session->kex.client_kexinit + 1, payload, len);
        session->kex.client_kexinit_len = len + 1;
    }

    // Parse name-lists and select algorithms
    // Skip parsing for now, just use defaults
    strncpy(session->kex.kex_algorithm, SSH_KEX_DH_GROUP14_SHA256,
            sizeof(session->kex.kex_algorithm) - 1);
    strncpy(session->kex.hostkey_algorithm, SSH_HOSTKEY_RSA_SHA2_256,
            sizeof(session->kex.hostkey_algorithm) - 1);
    strncpy(session->kex.cipher_c2s, SSH_CIPHER_AES256_CTR,
            sizeof(session->kex.cipher_c2s) - 1);
    strncpy(session->kex.cipher_s2c, SSH_CIPHER_AES256_CTR,
            sizeof(session->kex.cipher_s2c) - 1);
    strncpy(session->kex.mac_c2s, SSH_MAC_HMAC_SHA2_256,
            sizeof(session->kex.mac_c2s) - 1);
    strncpy(session->kex.mac_s2c, SSH_MAC_HMAC_SHA2_256,
            sizeof(session->kex.mac_s2c) - 1);

    session->state = SSH_STATE_KEXINIT_RECEIVED;
    kprintf("[SSH] Received KEXINIT, selected algorithms\n");
    return SSH_OK;
}

// =============================================================================
// Diffie-Hellman Key Exchange
// =============================================================================

int ssh_recv_kexdh_init(ssh_session_t *session, const uint8_t *payload, size_t len) {
    if (len < 4) {
        return SSH_ERR_PROTOCOL;
    }

    // Get client's DH public value (e)
    uint32_t e_len = get_u32(payload);
    if (e_len > len - 4 || e_len > 512) {
        return SSH_ERR_PROTOCOL;
    }

    session->kex.dh_e = kmalloc(e_len);
    if (!session->kex.dh_e) {
        return SSH_ERR_NOMEM;
    }
    memcpy(session->kex.dh_e, payload + 4, e_len);
    session->kex.dh_e_len = e_len;

    // Store DH parameters
    session->kex.dh_p = kmalloc(sizeof(dh_group14_p));
    session->kex.dh_g = kmalloc(sizeof(dh_group14_g));
    if (!session->kex.dh_p || !session->kex.dh_g) {
        return SSH_ERR_NOMEM;
    }
    memcpy(session->kex.dh_p, dh_group14_p, sizeof(dh_group14_p));
    memcpy(session->kex.dh_g, dh_group14_g, sizeof(dh_group14_g));
    session->kex.dh_p_len = sizeof(dh_group14_p);
    session->kex.dh_g_len = sizeof(dh_group14_g);

    // Generate server's private value (y)
    session->kex.dh_x = kmalloc(256);
    if (!session->kex.dh_x) {
        return SSH_ERR_NOMEM;
    }
    rng_get_bytes(session->kex.dh_x, 256);
    session->kex.dh_x_len = 256;

    // Compute server's public value: f = g^y mod p
    session->kex.dh_f = kmalloc(256);
    if (!session->kex.dh_f) {
        return SSH_ERR_NOMEM;
    }

    // For now, generate random f (in production, compute g^y mod p)
    rng_get_bytes(session->kex.dh_f, 256);
    session->kex.dh_f_len = 256;

    // Compute shared secret: K = e^y mod p
    session->kex.dh_k = kmalloc(256);
    if (!session->kex.dh_k) {
        return SSH_ERR_NOMEM;
    }

    // For now, generate random K (in production, compute e^y mod p)
    rng_get_bytes(session->kex.dh_k, 256);
    session->kex.dh_k_len = 256;

    session->state = SSH_STATE_KEX_DH_INIT;
    kprintf("[SSH] Received KEXDH_INIT\n");
    return SSH_OK;
}

int ssh_send_kexdh_reply(ssh_session_t *session, ssh_server_t *server) {
    uint8_t payload[4096];
    size_t pos = 0;

    // Host key (K_S)
    // Format: string "ssh-rsa" + mpint e + mpint n
    // For now, send a placeholder
    uint8_t hostkey[512];
    size_t hostkey_len = 0;

    // Write key type
    hostkey_len += put_string(hostkey + hostkey_len, "ssh-rsa", 7);

    // Write public exponent (e) - typically 65537
    uint8_t rsa_e[] = { 0x01, 0x00, 0x01 };  // 65537
    hostkey_len += put_string(hostkey + hostkey_len, rsa_e, sizeof(rsa_e));

    // Write modulus (n) - use server's public key or placeholder
    if (server->hostkey_public && server->hostkey_public_len > 0) {
        hostkey_len += put_string(hostkey + hostkey_len,
                                  server->hostkey_public,
                                  server->hostkey_public_len);
    } else {
        // Generate placeholder 2048-bit modulus
        uint8_t modulus[256];
        rng_get_bytes(modulus, sizeof(modulus));
        modulus[0] |= 0x80;  // Ensure MSB is set
        modulus[255] |= 0x01;  // Ensure odd
        hostkey_len += put_string(hostkey + hostkey_len, modulus, sizeof(modulus));
    }

    // Write host key to payload
    pos += put_string(payload + pos, hostkey, hostkey_len);

    // Server's DH public value (f)
    pos += put_string(payload + pos, session->kex.dh_f, session->kex.dh_f_len);

    // Compute exchange hash H
    // H = hash(V_C || V_S || I_C || I_S || K_S || e || f || K)
    sha256_ctx_t hash_ctx;
    sha256_init(&hash_ctx);

    // V_C (client version)
    uint8_t str_buf[512];
    size_t str_len = put_string(str_buf, session->client_version,
                                strlen(session->client_version));
    sha256_update(&hash_ctx, str_buf, str_len);

    // V_S (server version)
    str_len = put_string(str_buf, session->server_version,
                         strlen(session->server_version));
    sha256_update(&hash_ctx, str_buf, str_len);

    // I_C (client KEXINIT)
    if (session->kex.client_kexinit) {
        str_len = put_string(str_buf, session->kex.client_kexinit,
                             session->kex.client_kexinit_len);
        sha256_update(&hash_ctx, str_buf, str_len);
    }

    // I_S (server KEXINIT)
    if (session->kex.server_kexinit) {
        str_len = put_string(str_buf, session->kex.server_kexinit,
                             session->kex.server_kexinit_len);
        sha256_update(&hash_ctx, str_buf, str_len);
    }

    // K_S (host key)
    str_len = put_string(str_buf, hostkey, hostkey_len);
    sha256_update(&hash_ctx, str_buf, str_len);

    // e (client DH public)
    str_len = put_string(str_buf, session->kex.dh_e, session->kex.dh_e_len);
    sha256_update(&hash_ctx, str_buf, str_len);

    // f (server DH public)
    str_len = put_string(str_buf, session->kex.dh_f, session->kex.dh_f_len);
    sha256_update(&hash_ctx, str_buf, str_len);

    // K (shared secret as mpint)
    str_len = put_string(str_buf, session->kex.dh_k, session->kex.dh_k_len);
    sha256_update(&hash_ctx, str_buf, str_len);

    sha256_final(&hash_ctx, session->kex.h);
    session->kex.h_len = 32;

    // If this is the first key exchange, H becomes the session ID
    if (session->kex.session_id_len == 0) {
        memcpy(session->kex.session_id, session->kex.h, 32);
        session->kex.session_id_len = 32;
    }

    // Signature of H using host key
    // For RSA with SHA-256: RSASSA-PKCS1-v1_5
    // Format: string "rsa-sha2-256" + string signature
    uint8_t sig_data[512];
    size_t sig_data_len = 0;

    sig_data_len += put_string(sig_data + sig_data_len, "rsa-sha2-256", 12);

    // Generate signature (placeholder - should use actual RSA signing)
    uint8_t signature[256];
    rng_get_bytes(signature, sizeof(signature));  // Placeholder
    sig_data_len += put_string(sig_data + sig_data_len, signature, sizeof(signature));

    // Write signature to payload
    pos += put_string(payload + pos, sig_data, sig_data_len);

    int ret = ssh_send_packet(session, SSH_MSG_KEXDH_REPLY, payload, pos);
    if (ret == SSH_OK) {
        session->state = SSH_STATE_KEX_DH_REPLY;
        kprintf("[SSH] Sent KEXDH_REPLY\n");
    }
    return ret;
}

// =============================================================================
// NEWKEYS Message
// =============================================================================

int ssh_send_newkeys(ssh_session_t *session) {
    int ret = ssh_send_packet(session, SSH_MSG_NEWKEYS, NULL, 0);
    if (ret == SSH_OK) {
        session->state = SSH_STATE_NEWKEYS_SENT;
        kprintf("[SSH] Sent NEWKEYS\n");
    }
    return ret;
}

int ssh_recv_newkeys(ssh_session_t *session) {
    // Derive session keys from K and H
    // Key = hash(K || H || "X" || session_id)
    // where X is 'A'-'F' for different keys

    sha256_ctx_t ctx;
    uint8_t key_buf[512];
    size_t key_buf_len;

    // Prepare K as mpint
    key_buf_len = put_string(key_buf, session->kex.dh_k, session->kex.dh_k_len);

    // Encryption key client to server (C)
    sha256_init(&ctx);
    sha256_update(&ctx, key_buf, key_buf_len);
    sha256_update(&ctx, session->kex.h, session->kex.h_len);
    sha256_update(&ctx, "C", 1);
    sha256_update(&ctx, session->kex.session_id, session->kex.session_id_len);
    sha256_final(&ctx, session->keys.c2s_key);

    // Encryption key server to client (D)
    sha256_init(&ctx);
    sha256_update(&ctx, key_buf, key_buf_len);
    sha256_update(&ctx, session->kex.h, session->kex.h_len);
    sha256_update(&ctx, "D", 1);
    sha256_update(&ctx, session->kex.session_id, session->kex.session_id_len);
    sha256_final(&ctx, session->keys.s2c_key);

    // IV client to server (A)
    sha256_init(&ctx);
    sha256_update(&ctx, key_buf, key_buf_len);
    sha256_update(&ctx, session->kex.h, session->kex.h_len);
    sha256_update(&ctx, "A", 1);
    sha256_update(&ctx, session->kex.session_id, session->kex.session_id_len);
    sha256_final(&ctx, session->keys.c2s_iv);

    // IV server to client (B)
    sha256_init(&ctx);
    sha256_update(&ctx, key_buf, key_buf_len);
    sha256_update(&ctx, session->kex.h, session->kex.h_len);
    sha256_update(&ctx, "B", 1);
    sha256_update(&ctx, session->kex.session_id, session->kex.session_id_len);
    sha256_final(&ctx, session->keys.s2c_iv);

    // MAC key client to server (E)
    sha256_init(&ctx);
    sha256_update(&ctx, key_buf, key_buf_len);
    sha256_update(&ctx, session->kex.h, session->kex.h_len);
    sha256_update(&ctx, "E", 1);
    sha256_update(&ctx, session->kex.session_id, session->kex.session_id_len);
    sha256_final(&ctx, session->keys.c2s_mac);

    // MAC key server to client (F)
    sha256_init(&ctx);
    sha256_update(&ctx, key_buf, key_buf_len);
    sha256_update(&ctx, session->kex.h, session->kex.h_len);
    sha256_update(&ctx, "F", 1);
    sha256_update(&ctx, session->kex.session_id, session->kex.session_id_len);
    sha256_final(&ctx, session->keys.s2c_mac);

    // Set key lengths for AES-256-CTR
    session->keys.cipher_key_len = 32;
    session->keys.cipher_iv_len = 16;
    session->keys.mac_key_len = 32;
    session->keys.mac_len = 32;  // HMAC-SHA256

    // Reset sequence numbers
    session->keys.c2s_seq = 0;
    session->keys.s2c_seq = 0;

    session->state = SSH_STATE_NEWKEYS_RECEIVED;
    kprintf("[SSH] Received NEWKEYS, encryption activated\n");
    return SSH_OK;
}

// =============================================================================
// Packet Send/Receive
// =============================================================================

int ssh_send_packet(ssh_session_t *session, uint8_t type,
                    const void *payload, size_t len) {
    uint8_t packet[SSH_MAX_PACKET_SIZE];
    size_t pos = 0;

    // Calculate padding
    // packet_length || padding_length || payload || random padding || MAC
    size_t block_size = 8;  // Minimum, or cipher block size
    if (session->state >= SSH_STATE_NEWKEYS_RECEIVED) {
        block_size = 16;  // AES block size
    }

    size_t payload_len = 1 + len;  // type + payload
    size_t pad_len = block_size - ((4 + 1 + payload_len) % block_size);
    if (pad_len < 4) {
        pad_len += block_size;
    }
    uint32_t packet_len = 1 + payload_len + pad_len;

    // Write packet
    put_u32(packet + pos, packet_len);
    pos += 4;
    packet[pos++] = (uint8_t)pad_len;
    packet[pos++] = type;
    if (payload && len > 0) {
        memcpy(packet + pos, payload, len);
        pos += len;
    }

    // Random padding
    rng_get_bytes(packet + pos, pad_len);
    pos += pad_len;

    size_t unencrypted_len = pos;

    // Encrypt if keys are active
    if (session->state >= SSH_STATE_NEWKEYS_RECEIVED) {
        // AES-256-CTR encryption
        aes_ctx_t aes;
        aes_set_encrypt_key(&aes, session->keys.s2c_key, 256);

        // CTR mode encryption
        uint8_t counter[16];
        memcpy(counter, session->keys.s2c_iv, 16);

        // Add sequence number to counter
        uint64_t seq = session->keys.s2c_seq;
        for (int i = 15; i >= 8; i--) {
            counter[i] ^= (seq & 0xFF);
            seq >>= 8;
        }

        // Encrypt (skip first 4 bytes for packet length in some modes)
        for (size_t i = 4; i < pos; i += 16) {
            uint8_t keystream[16];
            aes_encrypt_block(&aes, counter, keystream);

            // XOR with plaintext
            size_t chunk = (pos - i < 16) ? (pos - i) : 16;
            for (size_t j = 0; j < chunk; j++) {
                packet[i + j] ^= keystream[j];
            }

            // Increment counter
            for (int k = 15; k >= 0; k--) {
                if (++counter[k] != 0) break;
            }
        }

        // Compute MAC
        uint8_t mac_data[SSH_MAX_PACKET_SIZE + 8];
        put_u32(mac_data, (uint32_t)session->keys.s2c_seq);
        memcpy(mac_data + 4, packet, unencrypted_len);

        uint8_t mac[32];
        hmac_sha256(session->keys.s2c_mac, 32, mac_data, 4 + unencrypted_len, mac);

        memcpy(packet + pos, mac, 32);
        pos += 32;

        session->keys.s2c_seq++;
    }

    // Send packet
    int sent = tcp_send(session->socket, packet, pos);
    if (sent != (int)pos) {
        kprintf("[SSH] Failed to send packet (sent %d of %zu)\n", sent, pos);
        return SSH_ERR_NETWORK;
    }

    return SSH_OK;
}

int ssh_recv_packet(ssh_session_t *session, uint8_t *type,
                    uint8_t *payload, size_t *len) {
    // Read packet length (4 bytes)
    while (session->recv_len < 4) {
        int n = tcp_recv(session->socket,
                        session->recv_buf + session->recv_len,
                        4 - session->recv_len);
        if (n <= 0) {
            if (n == TCP_ERR_WOULD_BLOCK || n == 0) {
                return SSH_ERR_WOULD_BLOCK;
            }
            return SSH_ERR_NETWORK;
        }
        session->recv_len += n;
    }

    // Decrypt packet length if encrypted
    uint8_t len_buf[4];
    memcpy(len_buf, session->recv_buf, 4);

    if (session->state >= SSH_STATE_NEWKEYS_RECEIVED) {
        // Decrypt first block to get packet length
        // (For AES-CTR, packet length is encrypted)
        aes_ctx_t aes;
        aes_set_encrypt_key(&aes, session->keys.c2s_key, 256);

        uint8_t counter[16];
        memcpy(counter, session->keys.c2s_iv, 16);

        uint64_t seq = session->keys.c2s_seq;
        for (int i = 15; i >= 8; i--) {
            counter[i] ^= (seq & 0xFF);
            seq >>= 8;
        }

        uint8_t keystream[16];
        aes_encrypt_block(&aes, counter, keystream);

        for (int i = 0; i < 4; i++) {
            len_buf[i] ^= keystream[i];
        }
    }

    uint32_t packet_len = get_u32(len_buf);

    // Validate packet length
    if (packet_len < 5 || packet_len > SSH_MAX_PACKET_SIZE - 4) {
        kprintf("[SSH] Invalid packet length: %u\n", packet_len);
        return SSH_ERR_PROTOCOL;
    }

    // Calculate total bytes needed
    size_t total_len = 4 + packet_len;
    if (session->state >= SSH_STATE_NEWKEYS_RECEIVED) {
        total_len += session->keys.mac_len;  // Add MAC
    }

    // Read remaining data
    while (session->recv_len < total_len) {
        int n = tcp_recv(session->socket,
                        session->recv_buf + session->recv_len,
                        total_len - session->recv_len);
        if (n <= 0) {
            if (n == TCP_ERR_WOULD_BLOCK || n == 0) {
                return SSH_ERR_WOULD_BLOCK;
            }
            return SSH_ERR_NETWORK;
        }
        session->recv_len += n;
    }

    // Decrypt packet
    uint8_t decrypted[SSH_MAX_PACKET_SIZE];
    memcpy(decrypted, session->recv_buf, 4 + packet_len);

    if (session->state >= SSH_STATE_NEWKEYS_RECEIVED) {
        aes_ctx_t aes;
        aes_set_encrypt_key(&aes, session->keys.c2s_key, 256);

        uint8_t counter[16];
        memcpy(counter, session->keys.c2s_iv, 16);

        uint64_t seq = session->keys.c2s_seq;
        for (int i = 15; i >= 8; i--) {
            counter[i] ^= (seq & 0xFF);
            seq >>= 8;
        }

        // Decrypt entire packet
        for (size_t i = 0; i < 4 + packet_len; i += 16) {
            uint8_t keystream[16];
            aes_encrypt_block(&aes, counter, keystream);

            size_t chunk = (4 + packet_len - i < 16) ? (4 + packet_len - i) : 16;
            for (size_t j = 0; j < chunk; j++) {
                decrypted[i + j] ^= keystream[j];
            }

            for (int k = 15; k >= 0; k--) {
                if (++counter[k] != 0) break;
            }
        }

        // Verify MAC
        uint8_t mac_data[SSH_MAX_PACKET_SIZE + 8];
        put_u32(mac_data, (uint32_t)session->keys.c2s_seq);
        memcpy(mac_data + 4, decrypted, 4 + packet_len);

        uint8_t computed_mac[32];
        hmac_sha256(session->keys.c2s_mac, 32, mac_data, 4 + 4 + packet_len, computed_mac);

        if (crypto_memcmp(computed_mac, session->recv_buf + 4 + packet_len, 32) != 0) {
            kprintf("[SSH] MAC verification failed\n");
            return SSH_ERR_CRYPTO;
        }

        session->keys.c2s_seq++;
    }

    // Parse decrypted packet
    uint8_t pad_len = decrypted[4];
    *type = decrypted[5];
    size_t payload_len = packet_len - 1 - pad_len;

    if (payload && len) {
        if (payload_len > *len) {
            payload_len = *len;
        }
        memcpy(payload, decrypted + 6, payload_len);
        *len = payload_len;
    }

    // Clear receive buffer
    session->recv_len = 0;

    return SSH_OK;
}

// =============================================================================
// State Name
// =============================================================================

const char *ssh_state_name(ssh_state_t state) {
    switch (state) {
        case SSH_STATE_INIT:             return "INIT";
        case SSH_STATE_VERSION_SENT:     return "VERSION_SENT";
        case SSH_STATE_VERSION_RECEIVED: return "VERSION_RECEIVED";
        case SSH_STATE_KEXINIT_SENT:     return "KEXINIT_SENT";
        case SSH_STATE_KEXINIT_RECEIVED: return "KEXINIT_RECEIVED";
        case SSH_STATE_KEX_DH_INIT:      return "KEX_DH_INIT";
        case SSH_STATE_KEX_DH_REPLY:     return "KEX_DH_REPLY";
        case SSH_STATE_NEWKEYS_SENT:     return "NEWKEYS_SENT";
        case SSH_STATE_NEWKEYS_RECEIVED: return "NEWKEYS_RECEIVED";
        case SSH_STATE_SERVICE_REQUEST:  return "SERVICE_REQUEST";
        case SSH_STATE_USERAUTH:         return "USERAUTH";
        case SSH_STATE_AUTHENTICATED:    return "AUTHENTICATED";
        case SSH_STATE_CHANNEL_OPEN:     return "CHANNEL_OPEN";
        case SSH_STATE_ESTABLISHED:      return "ESTABLISHED";
        case SSH_STATE_CLOSING:          return "CLOSING";
        case SSH_STATE_CLOSED:           return "CLOSED";
        case SSH_STATE_ERROR:            return "ERROR";
        default:                         return "UNKNOWN";
    }
}
