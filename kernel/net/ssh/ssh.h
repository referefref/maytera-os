// ssh.h - SSH-2 Server for MayteraOS
// RFC 4253 (Transport), RFC 4252 (Authentication), RFC 4254 (Connection)
#ifndef SSH_H
#define SSH_H

#include "../../types.h"

// =============================================================================
// SSH Protocol Constants
// =============================================================================

// SSH protocol version
#define SSH_VERSION_STRING      "SSH-2.0-MayteraOS_1.0"
#define SSH_MAX_VERSION_LEN     255

// Packet limits
#define SSH_MAX_PACKET_SIZE     35000
#define SSH_MIN_PACKET_SIZE     16
#define SSH_MAX_PAYLOAD_SIZE    32768

// Buffer sizes
#define SSH_RECV_BUFFER_SIZE    65536
#define SSH_SEND_BUFFER_SIZE    65536

// Maximum concurrent sessions
#define SSH_MAX_SESSIONS        8
#define SSH_MAX_CHANNELS        16

// Authentication
#define SSH_MAX_AUTH_ATTEMPTS   6
#define SSH_MAX_USERNAME_LEN    64
#define SSH_MAX_PASSWORD_LEN    128

// Default port
#define SSH_DEFAULT_PORT        22

// =============================================================================
// SSH Message Types (RFC 4253)
// =============================================================================

// Transport layer (1-19)
#define SSH_MSG_DISCONNECT                  1
#define SSH_MSG_IGNORE                      2
#define SSH_MSG_UNIMPLEMENTED               3
#define SSH_MSG_DEBUG                       4
#define SSH_MSG_SERVICE_REQUEST             5
#define SSH_MSG_SERVICE_ACCEPT              6

// Key exchange (20-29)
#define SSH_MSG_KEXINIT                     20
#define SSH_MSG_NEWKEYS                     21

// Diffie-Hellman key exchange (30-49)
#define SSH_MSG_KEXDH_INIT                  30
#define SSH_MSG_KEXDH_REPLY                 31

// User authentication (50-79)
#define SSH_MSG_USERAUTH_REQUEST            50
#define SSH_MSG_USERAUTH_FAILURE            51
#define SSH_MSG_USERAUTH_SUCCESS            52
#define SSH_MSG_USERAUTH_BANNER             53
#define SSH_MSG_USERAUTH_PK_OK              60

// Connection protocol (80-127)
#define SSH_MSG_GLOBAL_REQUEST              80
#define SSH_MSG_REQUEST_SUCCESS             81
#define SSH_MSG_REQUEST_FAILURE             82
#define SSH_MSG_CHANNEL_OPEN                90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION   91
#define SSH_MSG_CHANNEL_OPEN_FAILURE        92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST       93
#define SSH_MSG_CHANNEL_DATA                94
#define SSH_MSG_CHANNEL_EXTENDED_DATA       95
#define SSH_MSG_CHANNEL_EOF                 96
#define SSH_MSG_CHANNEL_CLOSE               97
#define SSH_MSG_CHANNEL_REQUEST             98
#define SSH_MSG_CHANNEL_SUCCESS             99
#define SSH_MSG_CHANNEL_FAILURE             100

// =============================================================================
// SSH Disconnect Reason Codes
// =============================================================================

#define SSH_DISCONNECT_HOST_NOT_ALLOWED_TO_CONNECT      1
#define SSH_DISCONNECT_PROTOCOL_ERROR                   2
#define SSH_DISCONNECT_KEY_EXCHANGE_FAILED              3
#define SSH_DISCONNECT_RESERVED                         4
#define SSH_DISCONNECT_MAC_ERROR                        5
#define SSH_DISCONNECT_COMPRESSION_ERROR                6
#define SSH_DISCONNECT_SERVICE_NOT_AVAILABLE            7
#define SSH_DISCONNECT_PROTOCOL_VERSION_NOT_SUPPORTED   8
#define SSH_DISCONNECT_HOST_KEY_NOT_VERIFIABLE          9
#define SSH_DISCONNECT_CONNECTION_LOST                  10
#define SSH_DISCONNECT_BY_APPLICATION                   11
#define SSH_DISCONNECT_TOO_MANY_CONNECTIONS             12
#define SSH_DISCONNECT_AUTH_CANCELLED_BY_USER           13
#define SSH_DISCONNECT_NO_MORE_AUTH_METHODS_AVAILABLE   14
#define SSH_DISCONNECT_ILLEGAL_USER_NAME                15

// =============================================================================
// SSH Channel Open Failure Reason Codes
// =============================================================================

#define SSH_OPEN_ADMINISTRATIVELY_PROHIBITED    1
#define SSH_OPEN_CONNECT_FAILED                 2
#define SSH_OPEN_UNKNOWN_CHANNEL_TYPE           3
#define SSH_OPEN_RESOURCE_SHORTAGE              4

// =============================================================================
// SSH Extended Data Type Codes
// =============================================================================

#define SSH_EXTENDED_DATA_STDERR    1

// =============================================================================
// Cryptographic Algorithm Identifiers
// =============================================================================

// Key exchange algorithms
#define SSH_KEX_DH_GROUP14_SHA256       "diffie-hellman-group14-sha256"
#define SSH_KEX_DH_GROUP16_SHA512       "diffie-hellman-group16-sha512"
#define SSH_KEX_CURVE25519_SHA256       "curve25519-sha256"

// Host key algorithms
#define SSH_HOSTKEY_RSA_SHA2_256        "rsa-sha2-256"
#define SSH_HOSTKEY_RSA_SHA2_512        "rsa-sha2-512"
#define SSH_HOSTKEY_SSH_RSA             "ssh-rsa"
#define SSH_HOSTKEY_SSH_ED25519         "ssh-ed25519"

// Encryption algorithms
#define SSH_CIPHER_AES256_CTR           "aes256-ctr"
#define SSH_CIPHER_AES128_CTR           "aes128-ctr"
#define SSH_CIPHER_AES256_GCM           "aes256-gcm@openssh.com"
#define SSH_CIPHER_CHACHA20_POLY1305    "chacha20-poly1305@openssh.com"

// MAC algorithms
#define SSH_MAC_HMAC_SHA2_256           "hmac-sha2-256"
#define SSH_MAC_HMAC_SHA2_512           "hmac-sha2-512"

// Compression algorithms
#define SSH_COMP_NONE                   "none"

// =============================================================================
// SSH Key Types
// =============================================================================

typedef enum {
    SSH_KEY_NONE = 0,
    SSH_KEY_RSA,
    SSH_KEY_ED25519
} ssh_key_type_t;

// =============================================================================
// SSH Session States
// =============================================================================

typedef enum {
    SSH_STATE_INIT = 0,             // Initial state
    SSH_STATE_VERSION_SENT,         // Version string sent
    SSH_STATE_VERSION_RECEIVED,     // Version string received
    SSH_STATE_KEXINIT_SENT,         // KEXINIT sent
    SSH_STATE_KEXINIT_RECEIVED,     // KEXINIT received
    SSH_STATE_KEX_DH_INIT,          // DH key exchange in progress
    SSH_STATE_KEX_DH_REPLY,         // DH reply sent
    SSH_STATE_NEWKEYS_SENT,         // NEWKEYS sent
    SSH_STATE_NEWKEYS_RECEIVED,     // NEWKEYS received (keys activated)
    SSH_STATE_SERVICE_REQUEST,      // Service request received
    SSH_STATE_USERAUTH,             // User authentication in progress
    SSH_STATE_AUTHENTICATED,        // User authenticated
    SSH_STATE_CHANNEL_OPEN,         // Channel opened
    SSH_STATE_ESTABLISHED,          // Session fully established
    SSH_STATE_CLOSING,              // Session closing
    SSH_STATE_CLOSED,               // Session closed
    SSH_STATE_ERROR                 // Error state
} ssh_state_t;

// =============================================================================
// SSH Channel States
// =============================================================================

typedef enum {
    SSH_CHANNEL_UNUSED = 0,
    SSH_CHANNEL_OPENING,
    SSH_CHANNEL_OPEN,
    SSH_CHANNEL_EOF_SENT,
    SSH_CHANNEL_EOF_RECEIVED,
    SSH_CHANNEL_CLOSING,
    SSH_CHANNEL_CLOSED
} ssh_channel_state_t;

// =============================================================================
// Forward Declarations
// =============================================================================

struct ssh_session;
struct ssh_channel;
struct ssh_kex;
struct ssh_keys;

// =============================================================================
// SSH Key Exchange State
// =============================================================================

typedef struct ssh_kex {
    // Algorithm selections
    char kex_algorithm[64];
    char hostkey_algorithm[64];
    char cipher_c2s[64];            // Client to server cipher
    char cipher_s2c[64];            // Server to client cipher
    char mac_c2s[64];               // Client to server MAC
    char mac_s2c[64];               // Server to client MAC

    // Client KEXINIT cookie and data
    uint8_t client_cookie[16];
    uint8_t *client_kexinit;
    size_t client_kexinit_len;

    // Server KEXINIT cookie and data
    uint8_t server_cookie[16];
    uint8_t *server_kexinit;
    size_t server_kexinit_len;

    // DH parameters (for DH-group14-sha256)
    uint8_t *dh_p;                  // Prime
    size_t dh_p_len;
    uint8_t *dh_g;                  // Generator
    size_t dh_g_len;
    uint8_t *dh_x;                  // Server private key
    size_t dh_x_len;
    uint8_t *dh_e;                  // Client public value
    size_t dh_e_len;
    uint8_t *dh_f;                  // Server public value
    size_t dh_f_len;
    uint8_t *dh_k;                  // Shared secret
    size_t dh_k_len;

    // Exchange hash
    uint8_t h[64];                  // SHA-256 or SHA-512
    size_t h_len;
    uint8_t session_id[64];
    size_t session_id_len;
} ssh_kex_t;

// =============================================================================
// SSH Encryption Keys
// =============================================================================

typedef struct ssh_keys {
    // Client to server keys
    uint8_t c2s_key[32];            // Encryption key
    uint8_t c2s_iv[16];             // Initial vector
    uint8_t c2s_mac[32];            // MAC key
    uint64_t c2s_seq;               // Sequence number

    // Server to client keys
    uint8_t s2c_key[32];            // Encryption key
    uint8_t s2c_iv[16];             // Initial vector
    uint8_t s2c_mac[32];            // MAC key
    uint64_t s2c_seq;               // Sequence number

    // Key sizes
    int cipher_key_len;
    int cipher_iv_len;
    int mac_key_len;
    int mac_len;

    // Encryption state
    void *c2s_cipher_ctx;
    void *s2c_cipher_ctx;
} ssh_keys_t;

// =============================================================================
// SSH Channel
// =============================================================================

typedef struct ssh_channel {
    ssh_channel_state_t state;

    uint32_t local_id;              // Our channel ID
    uint32_t remote_id;             // Remote channel ID
    uint32_t local_window;          // Our receive window
    uint32_t remote_window;         // Remote receive window
    uint32_t local_max_packet;      // Our max packet size
    uint32_t remote_max_packet;     // Remote max packet size

    char type[32];                  // Channel type (session, etc.)

    // For session channels
    int pty_allocated;              // PTY allocated?
    int shell_started;              // Shell started?
    int process_pid;                // Associated process PID

    // PTY settings
    char term[32];                  // TERM environment variable
    uint32_t term_width;
    uint32_t term_height;
    uint32_t term_width_px;
    uint32_t term_height_px;

    // I/O buffers
    uint8_t *stdin_buf;
    size_t stdin_len;
    size_t stdin_cap;
    uint8_t *stdout_buf;
    size_t stdout_len;
    size_t stdout_cap;
} ssh_channel_t;

// =============================================================================
// SSH Session
// =============================================================================

typedef struct ssh_session {
    ssh_state_t state;

    // Socket
    int socket;
    uint32_t client_ip;
    uint16_t client_port;

    // Version strings
    char client_version[256];
    char server_version[256];

    // Key exchange state
    ssh_kex_t kex;

    // Session keys (active)
    ssh_keys_t keys;

    // Next keys (pending after NEWKEYS)
    ssh_keys_t next_keys;

    // Authentication state
    char username[SSH_MAX_USERNAME_LEN];
    int auth_attempts;
    int authenticated;

    // Channels
    ssh_channel_t channels[SSH_MAX_CHANNELS];
    int num_channels;

    // Receive buffer
    uint8_t recv_buf[SSH_RECV_BUFFER_SIZE];
    size_t recv_len;

    // Send buffer
    uint8_t send_buf[SSH_SEND_BUFFER_SIZE];
    size_t send_len;

    // Packet assembly buffer
    uint8_t packet_buf[SSH_MAX_PACKET_SIZE];
    size_t packet_len;

    // Error info
    int last_error;
    uint32_t disconnect_reason;
    char error_msg[256];
} ssh_session_t;

// =============================================================================
// SSH Server Configuration
// =============================================================================

typedef struct ssh_config {
    uint16_t port;                  // Listen port (default 22)
    char host_key_path[256];        // Path to host key file
    char authorized_keys_path[256]; // Path to authorized_keys
    int allow_password_auth;        // Allow password authentication
    int allow_pubkey_auth;          // Allow public key authentication
    int max_auth_attempts;          // Maximum authentication attempts
    uint32_t idle_timeout;          // Idle timeout in seconds
    char banner[512];               // Pre-authentication banner
} ssh_config_t;

// =============================================================================
// SSH Server State
// =============================================================================

typedef struct ssh_server {
    int running;
    int listen_socket;
    ssh_config_t config;

    // Host keys
    ssh_key_type_t hostkey_type;
    uint8_t *hostkey_private;
    size_t hostkey_private_len;
    uint8_t *hostkey_public;
    size_t hostkey_public_len;

    // Active sessions
    ssh_session_t *sessions[SSH_MAX_SESSIONS];
    int num_sessions;
} ssh_server_t;

// =============================================================================
// SSH Server API
// =============================================================================

// Initialize SSH server
int ssh_server_init(ssh_server_t *server, ssh_config_t *config);

// Start SSH server (begins listening)
int ssh_server_start(ssh_server_t *server);

// Stop SSH server
void ssh_server_stop(ssh_server_t *server);

// Process SSH server events (call from main loop)
int ssh_server_poll(ssh_server_t *server);

// Generate host keys
int ssh_generate_host_keys(ssh_server_t *server, ssh_key_type_t type);

// Load host keys from file
int ssh_load_host_keys(ssh_server_t *server, const char *path);

// Save host keys to file
int ssh_save_host_keys(ssh_server_t *server, const char *path);

// =============================================================================
// SSH Session API
// =============================================================================

// Create new session
ssh_session_t *ssh_session_create(void);

// Free session
void ssh_session_free(ssh_session_t *session);

// Accept connection
int ssh_session_accept(ssh_session_t *session, int listen_socket);

// Process session (handle I/O)
int ssh_session_process(ssh_session_t *session, ssh_server_t *server);

// Close session
void ssh_session_close(ssh_session_t *session, uint32_t reason, const char *msg);

// Get session state name
const char *ssh_state_name(ssh_state_t state);

// =============================================================================
// SSH Channel API
// =============================================================================

// Allocate channel
ssh_channel_t *ssh_channel_alloc(ssh_session_t *session);

// Free channel
void ssh_channel_free(ssh_session_t *session, ssh_channel_t *channel);

// Find channel by local ID
ssh_channel_t *ssh_channel_find(ssh_session_t *session, uint32_t local_id);

// Send data to channel
int ssh_channel_send(ssh_session_t *session, ssh_channel_t *channel,
                     const void *data, size_t len);

// Send EOF on channel
int ssh_channel_send_eof(ssh_session_t *session, ssh_channel_t *channel);

// Close channel
int ssh_channel_close(ssh_session_t *session, ssh_channel_t *channel);

// =============================================================================
// SSH Packet API (Internal)
// =============================================================================

// Build and send packet
int ssh_send_packet(ssh_session_t *session, uint8_t type,
                    const void *payload, size_t len);

// Read and parse packet
int ssh_recv_packet(ssh_session_t *session, uint8_t *type,
                    uint8_t *payload, size_t *len);

// =============================================================================
// SSH Authentication API (Internal)
// =============================================================================

// Check password authentication
int ssh_auth_password(const char *username, const char *password);

// Check public key authentication
int ssh_auth_pubkey(const char *username, const uint8_t *key_blob, size_t key_len,
                    const uint8_t *signature, size_t sig_len);

// Load authorized keys for user
int ssh_load_authorized_keys(const char *username);

// =============================================================================
// Error Codes
// =============================================================================

#define SSH_OK                  0
#define SSH_ERR_NOMEM           -1
#define SSH_ERR_INVALID         -2
#define SSH_ERR_NETWORK         -3
#define SSH_ERR_PROTOCOL        -4
#define SSH_ERR_AUTH            -5
#define SSH_ERR_TIMEOUT         -6
#define SSH_ERR_CLOSED          -7
#define SSH_ERR_CRYPTO          -8
#define SSH_ERR_KEYGEN          -9
#define SSH_ERR_IO              -10
#define SSH_ERR_WOULD_BLOCK     -11

#endif // SSH_H
