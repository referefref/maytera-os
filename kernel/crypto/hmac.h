// hmac.h - HMAC (Hash-based Message Authentication Code)
// Used for HMAC-MD5 in NTLM authentication
#ifndef HMAC_H
#define HMAC_H

#include "../types.h"

// HMAC-MD5 (for NTLM authentication)
void hmac_md5(const void *key, size_t key_len,
              const void *data, size_t data_len,
              uint8_t mac[16]);

// HMAC-MD5 incremental interface
typedef struct {
    uint8_t key_block[64];
    void *inner_ctx;    // md5_ctx_t*
    void *outer_ctx;    // md5_ctx_t*
} hmac_md5_ctx_t;

void hmac_md5_init(hmac_md5_ctx_t *ctx, const void *key, size_t key_len);
void hmac_md5_update(hmac_md5_ctx_t *ctx, const void *data, size_t length);
void hmac_md5_final(hmac_md5_ctx_t *ctx, uint8_t mac[16]);

#endif // HMAC_H
