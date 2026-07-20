// md5.h - MD5 Hash Implementation
// Required for HMAC-MD5 in NTLM authentication
#ifndef MD5_H
#define MD5_H

#include "../types.h"

#define MD5_BLOCK_SIZE   64
#define MD5_DIGEST_SIZE  16

typedef struct {
    uint32_t state[4];          // Hash state (A, B, C, D)
    uint64_t count;             // Number of bits processed
    uint8_t buffer[64];         // Input buffer
} md5_ctx_t;

// Initialize MD5 context
void md5_init(md5_ctx_t *ctx);

// Update hash with data
void md5_update(md5_ctx_t *ctx, const void *data, size_t length);

// Finalize and get digest
void md5_final(md5_ctx_t *ctx, uint8_t digest[16]);

// One-shot hash
void md5(const void *data, size_t length, uint8_t digest[16]);

#endif // MD5_H
