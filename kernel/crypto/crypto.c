// crypto.c - Cryptographic subsystem initialization and utilities
// MayteraOS

#include "crypto.h"
#include "../string.h"
#include "../serial.h"

// Initialize crypto subsystem
void crypto_init(void) {
    kprintf("[CRYPTO] Initializing cryptographic subsystem...\n");

    // Initialize RNG first (needed by other components)
    rng_init();

    kprintf("[CRYPTO] Cryptographic subsystem initialized\n");
    kprintf("[CRYPTO] Available: SHA-256, HMAC-SHA256, AES-128/256, RNG\n");
}

// Constant-time memory comparison
int crypto_memcmp(const void *a, const void *b, size_t length) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    uint8_t diff = 0;

    for (size_t i = 0; i < length; i++) {
        diff |= pa[i] ^ pb[i];
    }

    return diff != 0;
}

// Secure memory zeroing (prevent compiler optimization)
void crypto_zero(void *ptr, size_t length) {
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (length--) {
        *p++ = 0;
    }
}

// Convert bytes to hex string
void crypto_to_hex(const uint8_t *data, size_t length, char *hex) {
    static const char hexchars[] = "0123456789abcdef";

    for (size_t i = 0; i < length; i++) {
        hex[i * 2]     = hexchars[(data[i] >> 4) & 0x0f];
        hex[i * 2 + 1] = hexchars[data[i] & 0x0f];
    }
    hex[length * 2] = '\0';
}

// Convert hex string to bytes
int crypto_from_hex(const char *hex, uint8_t *data, size_t max_length) {
    size_t hex_len = strlen(hex);
    size_t byte_len = hex_len / 2;

    if (hex_len % 2 != 0) return -1;  // Must be even
    if (byte_len > max_length) return -1;

    for (size_t i = 0; i < byte_len; i++) {
        uint8_t hi, lo;

        char c = hex[i * 2];
        if (c >= '0' && c <= '9') hi = c - '0';
        else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
        else return -1;

        c = hex[i * 2 + 1];
        if (c >= '0' && c <= '9') lo = c - '0';
        else if (c >= 'a' && c <= 'f') lo = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') lo = c - 'A' + 10;
        else return -1;

        data[i] = (hi << 4) | lo;
    }

    return (int)byte_len;
}

// =============================================================================
// AES-GCM Implementation (for TLS)
// =============================================================================

// GHASH multiplication in GF(2^128)
static void ghash_multiply(uint8_t *x, const uint8_t *h) {
    uint8_t z[16] = {0};
    uint8_t v[16];

    memcpy(v, h, 16);

    for (int i = 0; i < 128; i++) {
        // If bit i of x is set, XOR v into z
        int byte = i / 8;
        int bit = 7 - (i % 8);
        if ((x[byte] >> bit) & 1) {
            for (int j = 0; j < 16; j++) {
                z[j] ^= v[j];
            }
        }

        // Multiply v by x (shift right, with reduction)
        int carry = v[15] & 1;
        for (int j = 15; j > 0; j--) {
            v[j] = (v[j] >> 1) | (v[j-1] << 7);
        }
        v[0] >>= 1;
        if (carry) {
            v[0] ^= 0xe1;  // Reduction polynomial
        }
    }

    memcpy(x, z, 16);
}

// GHASH update
static void ghash_update(uint8_t *s, const uint8_t *h, const uint8_t *data, size_t len) {
    while (len >= 16) {
        for (int i = 0; i < 16; i++) {
            s[i] ^= data[i];
        }
        ghash_multiply(s, h);
        data += 16;
        len -= 16;
    }

    // Handle partial block
    if (len > 0) {
        for (size_t i = 0; i < len; i++) {
            s[i] ^= data[i];
        }
        ghash_multiply(s, h);
    }
}

// Increment counter (last 32 bits, big-endian)
static void inc32(uint8_t *counter) {
    for (int i = 15; i >= 12; i--) {
        if (++counter[i] != 0) break;
    }
}

// Initialize AES-GCM
int aes_gcm_init(aes_gcm_ctx_t *ctx, const uint8_t *key, int key_bits,
                 const uint8_t *iv, size_t iv_len) {
    // Set up AES key
    if (aes_set_encrypt_key(&ctx->aes, key, key_bits) != 0) {
        return -1;
    }

    // Compute H = AES(K, 0^128)
    memset(ctx->h, 0, 16);
    aes_encrypt_block(&ctx->aes, ctx->h, ctx->h);

    // Compute J0 (initial counter)
    memset(ctx->j0, 0, 16);
    if (iv_len == 12) {
        // Standard 96-bit IV
        memcpy(ctx->j0, iv, 12);
        ctx->j0[15] = 1;
    } else {
        // Hash IV to get J0
        ghash_update(ctx->j0, ctx->h, iv, iv_len);
        uint8_t len_block[16] = {0};
        uint64_t iv_bits = iv_len * 8;
        len_block[8]  = (iv_bits >> 56) & 0xff;
        len_block[9]  = (iv_bits >> 48) & 0xff;
        len_block[10] = (iv_bits >> 40) & 0xff;
        len_block[11] = (iv_bits >> 32) & 0xff;
        len_block[12] = (iv_bits >> 24) & 0xff;
        len_block[13] = (iv_bits >> 16) & 0xff;
        len_block[14] = (iv_bits >> 8) & 0xff;
        len_block[15] = iv_bits & 0xff;
        ghash_update(ctx->j0, ctx->h, len_block, 16);
    }

    // Initialize counter to J0 + 1
    memcpy(ctx->counter, ctx->j0, 16);
    inc32(ctx->counter);

    // Initialize GHASH state
    memset(ctx->s, 0, 16);
    ctx->aad_len = 0;
    ctx->cipher_len = 0;

    return 0;
}

// Add additional authenticated data
void aes_gcm_aad(aes_gcm_ctx_t *ctx, const uint8_t *aad, size_t aad_len) {
    ghash_update(ctx->s, ctx->h, aad, aad_len);
    ctx->aad_len += aad_len * 8;
}

// Encrypt
void aes_gcm_encrypt(aes_gcm_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t length) {
    uint8_t keystream[16];

    while (length > 0) {
        // Generate keystream block
        aes_encrypt_block(&ctx->aes, ctx->counter, keystream);
        inc32(ctx->counter);

        // XOR with plaintext
        size_t block_len = (length < 16) ? length : 16;
        for (size_t i = 0; i < block_len; i++) {
            out[i] = in[i] ^ keystream[i];
        }

        // Update GHASH with ciphertext
        if (block_len == 16) {
            for (int i = 0; i < 16; i++) {
                ctx->s[i] ^= out[i];
            }
            ghash_multiply(ctx->s, ctx->h);
        } else {
            for (size_t i = 0; i < block_len; i++) {
                ctx->s[i] ^= out[i];
            }
            ghash_multiply(ctx->s, ctx->h);
        }

        ctx->cipher_len += block_len * 8;
        in += block_len;
        out += block_len;
        length -= block_len;
    }

    crypto_zero(keystream, sizeof(keystream));
}

// Decrypt
int aes_gcm_decrypt(aes_gcm_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t length) {
    uint8_t keystream[16];

    while (length > 0) {
        // Update GHASH with ciphertext first
        size_t block_len = (length < 16) ? length : 16;
        for (size_t i = 0; i < block_len; i++) {
            ctx->s[i] ^= in[i];
        }
        if (block_len == 16 || length == block_len) {
            ghash_multiply(ctx->s, ctx->h);
        }

        // Generate keystream block
        aes_encrypt_block(&ctx->aes, ctx->counter, keystream);
        inc32(ctx->counter);

        // XOR with ciphertext
        for (size_t i = 0; i < block_len; i++) {
            out[i] = in[i] ^ keystream[i];
        }

        ctx->cipher_len += block_len * 8;
        in += block_len;
        out += block_len;
        length -= block_len;
    }

    crypto_zero(keystream, sizeof(keystream));
    return 0;
}

// Get authentication tag
void aes_gcm_final(aes_gcm_ctx_t *ctx, uint8_t *tag, size_t tag_len) {
    uint8_t len_block[16];
    uint8_t final_tag[16];

    // Append lengths (in bits) to GHASH
    len_block[0]  = (ctx->aad_len >> 56) & 0xff;
    len_block[1]  = (ctx->aad_len >> 48) & 0xff;
    len_block[2]  = (ctx->aad_len >> 40) & 0xff;
    len_block[3]  = (ctx->aad_len >> 32) & 0xff;
    len_block[4]  = (ctx->aad_len >> 24) & 0xff;
    len_block[5]  = (ctx->aad_len >> 16) & 0xff;
    len_block[6]  = (ctx->aad_len >> 8) & 0xff;
    len_block[7]  = ctx->aad_len & 0xff;
    len_block[8]  = (ctx->cipher_len >> 56) & 0xff;
    len_block[9]  = (ctx->cipher_len >> 48) & 0xff;
    len_block[10] = (ctx->cipher_len >> 40) & 0xff;
    len_block[11] = (ctx->cipher_len >> 32) & 0xff;
    len_block[12] = (ctx->cipher_len >> 24) & 0xff;
    len_block[13] = (ctx->cipher_len >> 16) & 0xff;
    len_block[14] = (ctx->cipher_len >> 8) & 0xff;
    len_block[15] = ctx->cipher_len & 0xff;

    for (int i = 0; i < 16; i++) {
        ctx->s[i] ^= len_block[i];
    }
    ghash_multiply(ctx->s, ctx->h);

    // Compute tag = GHASH XOR AES(J0)
    aes_encrypt_block(&ctx->aes, ctx->j0, final_tag);
    for (int i = 0; i < 16; i++) {
        final_tag[i] ^= ctx->s[i];
    }

    // Copy requested tag length
    if (tag_len > 16) tag_len = 16;
    memcpy(tag, final_tag, tag_len);

    crypto_zero(final_tag, sizeof(final_tag));
}

// Verify authentication tag
int aes_gcm_verify(aes_gcm_ctx_t *ctx, const uint8_t *tag, size_t tag_len) {
    uint8_t computed_tag[16];
    aes_gcm_final(ctx, computed_tag, tag_len);
    int result = crypto_memcmp(computed_tag, tag, tag_len);
    crypto_zero(computed_tag, sizeof(computed_tag));
    return result;  // 0 if equal, non-zero if different
}
