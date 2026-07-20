// chacha20.h - ChaCha20-Poly1305 AEAD cipher for MayteraOS
// Used in TLS 1.2 and TLS 1.3
#ifndef CHACHA20_H
#define CHACHA20_H

#include "../types.h"

// ChaCha20 constants
#define CHACHA20_KEY_SIZE    32
#define CHACHA20_NONCE_SIZE  12
#define CHACHA20_BLOCK_SIZE  64

// Poly1305 constants
#define POLY1305_KEY_SIZE    32
#define POLY1305_TAG_SIZE    16

// =============================================================================
// ChaCha20 Stream Cipher
// =============================================================================

typedef struct {
    uint32_t state[16];
    uint32_t working[16];
    uint8_t keystream[64];
    size_t keystream_pos;
    uint32_t counter;
} chacha20_ctx_t;

// Initialize ChaCha20 context
void chacha20_init(chacha20_ctx_t *ctx, const uint8_t key[32], 
                   const uint8_t nonce[12], uint32_t counter);

// Encrypt/decrypt data (XOR with keystream)
void chacha20_crypt(chacha20_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len);

// Generate keystream block
void chacha20_block(chacha20_ctx_t *ctx, uint8_t output[64]);

// =============================================================================
// Poly1305 MAC
// =============================================================================

typedef struct {
    uint32_t r[5];      // Clamped key portion
    uint32_t h[5];      // Accumulator
    uint32_t pad[4];    // Final addition key
    uint8_t buffer[16];
    size_t buffer_len;
    size_t total_len;
} poly1305_ctx_t;

// Initialize Poly1305 with key
void poly1305_init(poly1305_ctx_t *ctx, const uint8_t key[32]);

// Update with data
void poly1305_update(poly1305_ctx_t *ctx, const uint8_t *data, size_t len);

// Finalize and get tag
void poly1305_final(poly1305_ctx_t *ctx, uint8_t tag[16]);

// One-shot MAC
void poly1305(const uint8_t key[32], const uint8_t *msg, size_t msg_len, uint8_t tag[16]);

// =============================================================================
// ChaCha20-Poly1305 AEAD
// =============================================================================

typedef struct {
    chacha20_ctx_t chacha;
    poly1305_ctx_t poly;
    uint64_t aad_len;
    uint64_t cipher_len;
} chacha20_poly1305_ctx_t;

// Initialize for encryption/decryption
void chacha20_poly1305_init(chacha20_poly1305_ctx_t *ctx,
                            const uint8_t key[32],
                            const uint8_t nonce[12]);

// Add additional authenticated data (call before encrypt/decrypt)
void chacha20_poly1305_aad(chacha20_poly1305_ctx_t *ctx,
                           const uint8_t *aad, size_t len);

// Encrypt data
void chacha20_poly1305_encrypt(chacha20_poly1305_ctx_t *ctx,
                               const uint8_t *plaintext,
                               uint8_t *ciphertext,
                               size_t len);

// Decrypt data
void chacha20_poly1305_decrypt(chacha20_poly1305_ctx_t *ctx,
                               const uint8_t *ciphertext,
                               uint8_t *plaintext,
                               size_t len);

// Finalize and get authentication tag
void chacha20_poly1305_final(chacha20_poly1305_ctx_t *ctx, uint8_t tag[16]);

// Verify authentication tag (returns 0 on success)
int chacha20_poly1305_verify(chacha20_poly1305_ctx_t *ctx, const uint8_t tag[16]);

// =============================================================================
// One-shot AEAD Functions
// =============================================================================

// Encrypt and authenticate (ciphertext must have room for len + 16 bytes)
int chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *plaintext, size_t plaintext_len,
                           uint8_t *ciphertext);

// Decrypt and verify (returns 0 on success, -1 on auth failure)
int chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *ciphertext, size_t ciphertext_len,
                           uint8_t *plaintext);

#endif // CHACHA20_H
