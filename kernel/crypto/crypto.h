// crypto.h - Cryptographic primitives for MayteraOS
#ifndef CRYPTO_H
#define CRYPTO_H

#include "../types.h"

// Initialize crypto subsystem (RNG, etc.)
void crypto_init(void);

// =============================================================================
// Random Number Generation
// =============================================================================

// Initialize RNG (called by crypto_init)
void rng_init(void);

// Get random bytes
// Returns: 0 on success, -1 on failure
int rng_get_bytes(void *buffer, size_t length);

// Get random 32-bit value
uint32_t rng_get_u32(void);

// Get random 64-bit value
uint64_t rng_get_u64(void);

// Add entropy to pool
void rng_add_entropy(const void *data, size_t length);

// =============================================================================
// SHA-256
// =============================================================================

#define SHA256_BLOCK_SIZE   64
#define SHA256_DIGEST_SIZE  32

typedef struct {
    uint32_t state[8];          // Hash state
    uint64_t count;             // Number of bits processed
    uint8_t buffer[64];         // Input buffer
} sha256_ctx_t;

// Initialize SHA-256 context
void sha256_init(sha256_ctx_t *ctx);

// Update hash with data
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t length);

// Finalize and get digest
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32]);

// One-shot hash
void sha256(const void *data, size_t length, uint8_t digest[32]);

// =============================================================================
// SHA-512 / SHA-384
// =============================================================================

#define SHA512_BLOCK_SIZE   128
#define SHA512_DIGEST_SIZE  64
#define SHA384_DIGEST_SIZE  48

typedef struct {
    uint64_t state[8];          // Hash state
    uint64_t count;             // Number of bits processed (low 64 bits)
    uint8_t buffer[128];        // Input buffer
} sha512_ctx_t;

void sha512_init(sha512_ctx_t *ctx);
void sha384_init(sha512_ctx_t *ctx);
void sha512_update(sha512_ctx_t *ctx, const void *data, size_t length);
void sha512_final(sha512_ctx_t *ctx, uint8_t digest[64]);
void sha384_final(sha512_ctx_t *ctx, uint8_t digest[48]);
void sha384(const void *data, size_t length, uint8_t digest[48]);

// =============================================================================
// HMAC-SHA384
// =============================================================================

#define HMAC_SHA384_SIZE  48

typedef struct {
    sha512_ctx_t inner;
    sha512_ctx_t outer;
    uint8_t key_block[SHA512_BLOCK_SIZE];
} hmac_sha384_ctx_t;

void hmac_sha384_init(hmac_sha384_ctx_t *ctx, const void *key, size_t key_len);
void hmac_sha384_update(hmac_sha384_ctx_t *ctx, const void *data, size_t length);
void hmac_sha384_final(hmac_sha384_ctx_t *ctx, uint8_t mac[48]);
void hmac_sha384(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 uint8_t mac[48]);

// =============================================================================
// HMAC-SHA256
// =============================================================================

#define HMAC_SHA256_SIZE  32

typedef struct {
    sha256_ctx_t inner;
    sha256_ctx_t outer;
    uint8_t key_block[SHA256_BLOCK_SIZE];
} hmac_sha256_ctx_t;

// Initialize HMAC context with key
void hmac_sha256_init(hmac_sha256_ctx_t *ctx, const void *key, size_t key_len);

// Update HMAC with data
void hmac_sha256_update(hmac_sha256_ctx_t *ctx, const void *data, size_t length);

// Finalize and get MAC
void hmac_sha256_final(hmac_sha256_ctx_t *ctx, uint8_t mac[32]);

// One-shot HMAC
void hmac_sha256(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 uint8_t mac[32]);

// =============================================================================
// AES-128/256
// =============================================================================

#define AES_BLOCK_SIZE  16
#define AES128_KEY_SIZE 16
#define AES256_KEY_SIZE 32

typedef struct {
    uint32_t rk[60];            // Round keys (max for AES-256)
    int nr;                     // Number of rounds (10, 12, or 14)
} aes_ctx_t;

// Set encryption key
// key_bits: 128, 192, or 256
int aes_set_encrypt_key(aes_ctx_t *ctx, const uint8_t *key, int key_bits);

// Set decryption key
int aes_set_decrypt_key(aes_ctx_t *ctx, const uint8_t *key, int key_bits);

// Encrypt single block (16 bytes)
void aes_encrypt_block(const aes_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]);

// Decrypt single block (16 bytes)
void aes_decrypt_block(const aes_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]);

// =============================================================================
// AES-CBC Mode
// =============================================================================

typedef struct {
    aes_ctx_t aes;
    uint8_t iv[16];
} aes_cbc_ctx_t;

// Initialize AES-CBC for encryption
int aes_cbc_encrypt_init(aes_cbc_ctx_t *ctx, const uint8_t *key, int key_bits,
                         const uint8_t iv[16]);

// Initialize AES-CBC for decryption
int aes_cbc_decrypt_init(aes_cbc_ctx_t *ctx, const uint8_t *key, int key_bits,
                         const uint8_t iv[16]);

// Encrypt data (must be multiple of 16 bytes)
void aes_cbc_encrypt(aes_cbc_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t length);

// Decrypt data (must be multiple of 16 bytes)
void aes_cbc_decrypt(aes_cbc_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t length);

// =============================================================================
// AES-GCM Mode (for TLS 1.2/1.3)
// =============================================================================

typedef struct {
    aes_ctx_t aes;
    uint8_t h[16];              // Hash subkey
    uint8_t j0[16];             // Pre-counter block
    uint8_t s[16];              // GHASH state
    uint8_t counter[16];        // Counter block
    uint64_t aad_len;           // AAD length in bits
    uint64_t cipher_len;        // Ciphertext length in bits
} aes_gcm_ctx_t;

// Initialize AES-GCM
int aes_gcm_init(aes_gcm_ctx_t *ctx, const uint8_t *key, int key_bits,
                 const uint8_t *iv, size_t iv_len);

// Add additional authenticated data
void aes_gcm_aad(aes_gcm_ctx_t *ctx, const uint8_t *aad, size_t aad_len);

// Encrypt and authenticate
void aes_gcm_encrypt(aes_gcm_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t length);

// Decrypt and verify
int aes_gcm_decrypt(aes_gcm_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t length);

// Get authentication tag
void aes_gcm_final(aes_gcm_ctx_t *ctx, uint8_t *tag, size_t tag_len);

// Verify authentication tag
int aes_gcm_verify(aes_gcm_ctx_t *ctx, const uint8_t *tag, size_t tag_len);

// =============================================================================
// Utility Functions
// =============================================================================

// Constant-time memory comparison (prevents timing attacks)
int crypto_memcmp(const void *a, const void *b, size_t length);

// Secure memory zeroing
void crypto_zero(void *ptr, size_t length);

// Convert bytes to hex string
void crypto_to_hex(const uint8_t *data, size_t length, char *hex);

// Convert hex string to bytes
int crypto_from_hex(const char *hex, uint8_t *data, size_t max_length);

#endif // CRYPTO_H
