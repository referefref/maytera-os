// md4.h - MD4 Hash Implementation for NTLM Authentication
// MD4 is required for NTLM password hashing (RFC 1320)
#ifndef MD4_H
#define MD4_H

#include "../types.h"

#define MD4_BLOCK_SIZE   64
#define MD4_DIGEST_SIZE  16

typedef struct {
    uint32_t state[4];          // Hash state (A, B, C, D)
    uint64_t count;             // Number of bits processed
    uint8_t buffer[64];         // Input buffer
} md4_ctx_t;

// Initialize MD4 context
void md4_init(md4_ctx_t *ctx);

// Update hash with data
void md4_update(md4_ctx_t *ctx, const void *data, size_t length);

// Finalize and get digest
void md4_final(md4_ctx_t *ctx, uint8_t digest[16]);

// One-shot hash
void md4(const void *data, size_t length, uint8_t digest[16]);

#endif // MD4_H
