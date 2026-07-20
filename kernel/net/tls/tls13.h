// tls13.h - TLS 1.3 protocol support for MayteraOS
// Implements RFC 8446
#ifndef TLS13_H
#define TLS13_H

#include "tls.h"
#include "cert_store.h"

// TLS 1.3 version
#define TLS_VERSION_1_3     0x0304

// TLS 1.3 cipher suites
#define TLS13_AES_128_GCM_SHA256        0x1301
#define TLS13_AES_256_GCM_SHA384        0x1302
#define TLS13_CHACHA20_POLY1305_SHA256  0x1303

// TLS 1.3 signature algorithms
#define TLS13_SIG_RSA_PKCS1_SHA256      0x0401
#define TLS13_SIG_RSA_PKCS1_SHA384      0x0501
#define TLS13_SIG_ECDSA_SECP256R1_SHA256 0x0403
#define TLS13_SIG_ECDSA_SECP384R1_SHA384 0x0503
#define TLS13_SIG_RSA_PSS_SHA256        0x0804
#define TLS13_SIG_RSA_PSS_SHA384        0x0805

// TLS 1.3 named groups (for key exchange)
#define TLS13_GROUP_SECP256R1   0x0017
#define TLS13_GROUP_SECP384R1   0x0018
#define TLS13_GROUP_X25519      0x001D

// TLS 1.3 extension types
#define TLS_EXT_SERVER_NAME             0
#define TLS_EXT_SUPPORTED_GROUPS        10
#define TLS_EXT_SIGNATURE_ALGORITHMS    13
#define TLS_EXT_KEY_SHARE               51
#define TLS_EXT_SUPPORTED_VERSIONS      43
#define TLS_EXT_PSK_KEY_EXCHANGE_MODES  45

// TLS 1.3 handshake types
#define TLS13_HANDSHAKE_ENCRYPTED_EXTENSIONS  8
#define TLS13_HANDSHAKE_NEW_SESSION_TICKET    4

// TLS 1.3 key derivation labels
#define TLS13_LABEL_DERIVED         "derived"
#define TLS13_LABEL_CLIENT_HANDSHAKE "c hs traffic"
#define TLS13_LABEL_SERVER_HANDSHAKE "s hs traffic"
#define TLS13_LABEL_CLIENT_APP       "c ap traffic"
#define TLS13_LABEL_SERVER_APP       "s ap traffic"
#define TLS13_LABEL_KEY              "key"
#define TLS13_LABEL_IV               "iv"
#define TLS13_LABEL_FINISHED         "finished"

// TLS 1.3 key schedule context
typedef struct {
    uint8_t early_secret[48];
    uint8_t handshake_secret[48];
    uint8_t master_secret[48];
    
    uint8_t client_handshake_traffic[48];
    uint8_t server_handshake_traffic[48];
    uint8_t client_app_traffic[48];
    uint8_t server_app_traffic[48];
    
    // Derived keys
    uint8_t client_write_key[32];
    uint8_t server_write_key[32];
    uint8_t client_write_iv[12];
    uint8_t server_write_iv[12];
    
    int hash_len;  // 32 for SHA-256, 48 for SHA-384
} tls13_key_schedule_t;

// X25519 key pair
typedef struct {
    uint8_t public_key[32];
    uint8_t private_key[32];
} x25519_keypair_t;

// =============================================================================
// TLS 1.3 Key Exchange
// =============================================================================

// Generate X25519 key pair
void x25519_generate_keypair(x25519_keypair_t *kp);

// Compute X25519 shared secret
void x25519_shared_secret(const uint8_t *their_public, 
                          const uint8_t *my_private,
                          uint8_t *shared);

// =============================================================================
// TLS 1.3 Key Derivation
// =============================================================================

// HKDF-Extract: output = HMAC-Hash(salt, IKM)
void hkdf_extract(const uint8_t *salt, size_t salt_len,
                  const uint8_t *ikm, size_t ikm_len,
                  uint8_t *prk, size_t hash_len);

// HKDF-Expand-Label for TLS 1.3
void hkdf_expand_label(const uint8_t *secret, size_t secret_len,
                       const char *label,
                       const uint8_t *context, size_t context_len,
                       uint8_t *output, size_t output_len,
                       size_t hash_len);

// Derive TLS 1.3 secrets from shared secret
void tls13_derive_secrets(tls13_key_schedule_t *ks,
                          const uint8_t *shared_secret, size_t secret_len,
                          const uint8_t *hello_hash, size_t hash_len);

// Derive handshake traffic keys
void tls13_derive_handshake_keys(tls13_key_schedule_t *ks, uint16_t cipher_suite);

// Derive application traffic keys
void tls13_derive_app_keys(tls13_key_schedule_t *ks,
                           const uint8_t *handshake_hash,
                           uint16_t cipher_suite);

// =============================================================================
// TLS 1.3 Record Layer
// =============================================================================

// Encrypt TLS 1.3 record
int tls13_encrypt_record(const uint8_t *key, const uint8_t *iv,
                         uint64_t seq_num, uint8_t content_type,
                         const uint8_t *plaintext, size_t plaintext_len,
                         uint8_t *ciphertext, size_t *ciphertext_len,
                         uint16_t cipher_suite);

// Decrypt TLS 1.3 record
int tls13_decrypt_record(const uint8_t *key, const uint8_t *iv,
                         uint64_t seq_num,
                         const uint8_t *ciphertext, size_t ciphertext_len,
                         uint8_t *plaintext, size_t *plaintext_len,
                         uint8_t *content_type,
                         uint16_t cipher_suite);

#endif // TLS13_H
