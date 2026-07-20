// ssh_crypto.c - Cryptographic operations for SSH
// Provides wrappers and additional crypto primitives
#include "ssh.h"
#include "../../crypto/crypto.h"
#include "../../string.h"
#include "../../serial.h"
#include "../../mm/heap.h"

// =============================================================================
// AES-CTR Mode (Counter Mode)
// =============================================================================

typedef struct {
    aes_ctx_t aes;
    uint8_t counter[16];
    uint8_t keystream[16];
    int keystream_pos;
} aes_ctr_ctx_t;

int aes_ctr_init(aes_ctr_ctx_t *ctx, const uint8_t *key, int key_bits,
                 const uint8_t *iv) {
    int ret = aes_set_encrypt_key(&ctx->aes, key, key_bits);
    if (ret != 0) {
        return ret;
    }
    memcpy(ctx->counter, iv, 16);
    ctx->keystream_pos = 16;  // Force regeneration on first use
    return 0;
}

static void aes_ctr_increment(uint8_t *counter) {
    for (int i = 15; i >= 0; i--) {
        if (++counter[i] != 0) {
            break;
        }
    }
}

void aes_ctr_crypt(aes_ctr_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (ctx->keystream_pos >= 16) {
            aes_encrypt_block(&ctx->aes, ctx->counter, ctx->keystream);
            aes_ctr_increment(ctx->counter);
            ctx->keystream_pos = 0;
        }
        out[i] = in[i] ^ ctx->keystream[ctx->keystream_pos++];
    }
}

// =============================================================================
// HKDF (HMAC-based Key Derivation Function)
// RFC 5869 - Used for SSH key derivation
// =============================================================================

// HKDF-Extract: PRK = HMAC-Hash(salt, IKM)
static void hkdf_extract(const uint8_t *salt, size_t salt_len,
                  const uint8_t *ikm, size_t ikm_len,
                  uint8_t prk[32]) {
    if (salt == NULL || salt_len == 0) {
        // Use all-zero salt
        uint8_t zero_salt[32] = {0};
        hmac_sha256(zero_salt, 32, ikm, ikm_len, prk);
    } else {
        hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
    }
}

// HKDF-Expand: OKM = HMAC-Hash(PRK, T(1) || info || 0x01) || ...
void hkdf_expand(const uint8_t prk[32],
                 const uint8_t *info, size_t info_len,
                 uint8_t *okm, size_t okm_len) {
    uint8_t t[32] = {0};
    size_t t_len = 0;
    uint8_t counter = 1;
    size_t offset = 0;

    while (offset < okm_len) {
        // T(N) = HMAC-Hash(PRK, T(N-1) || info || N)
        hmac_sha256_ctx_t ctx;
        hmac_sha256_init(&ctx, prk, 32);

        if (t_len > 0) {
            hmac_sha256_update(&ctx, t, t_len);
        }
        if (info_len > 0) {
            hmac_sha256_update(&ctx, info, info_len);
        }
        hmac_sha256_update(&ctx, &counter, 1);
        hmac_sha256_final(&ctx, t);
        t_len = 32;

        // Copy to output
        size_t copy_len = (okm_len - offset < 32) ? (okm_len - offset) : 32;
        memcpy(okm + offset, t, copy_len);
        offset += copy_len;
        counter++;
    }
}

// Full HKDF
void hkdf(const uint8_t *salt, size_t salt_len,
          const uint8_t *ikm, size_t ikm_len,
          const uint8_t *info, size_t info_len,
          uint8_t *okm, size_t okm_len) {
    uint8_t prk[32];
    hkdf_extract(salt, salt_len, ikm, ikm_len, prk);
    hkdf_expand(prk, info, info_len, okm, okm_len);
    crypto_zero(prk, sizeof(prk));
}

// =============================================================================
// SSH Key Derivation
// =============================================================================

// Derive key material from shared secret K and exchange hash H
// key = HASH(K || H || X || session_id)
// where X is a single character 'A'-'F' identifying the key type
void ssh_derive_key(const uint8_t *k, size_t k_len,
                    const uint8_t *h, size_t h_len,
                    char x,
                    const uint8_t *session_id, size_t session_id_len,
                    uint8_t *key, size_t key_len) {
    sha256_ctx_t ctx;
    uint8_t hash[32];

    // First hash: K || H || X || session_id
    sha256_init(&ctx);

    // K as mpint (4-byte length prefix + data)
    uint8_t k_mpint[4];
    k_mpint[0] = (k_len >> 24) & 0xFF;
    k_mpint[1] = (k_len >> 16) & 0xFF;
    k_mpint[2] = (k_len >> 8) & 0xFF;
    k_mpint[3] = k_len & 0xFF;
    sha256_update(&ctx, k_mpint, 4);
    sha256_update(&ctx, k, k_len);

    sha256_update(&ctx, h, h_len);
    sha256_update(&ctx, &x, 1);
    sha256_update(&ctx, session_id, session_id_len);
    sha256_final(&ctx, hash);

    // If we need more than 32 bytes, extend
    if (key_len <= 32) {
        memcpy(key, hash, key_len);
    } else {
        memcpy(key, hash, 32);
        size_t offset = 32;

        while (offset < key_len) {
            // K(n) = HASH(K || H || K(1) || ... || K(n-1))
            sha256_init(&ctx);
            sha256_update(&ctx, k_mpint, 4);
            sha256_update(&ctx, k, k_len);
            sha256_update(&ctx, h, h_len);
            sha256_update(&ctx, key, offset);
            sha256_final(&ctx, hash);

            size_t copy = (key_len - offset < 32) ? (key_len - offset) : 32;
            memcpy(key + offset, hash, copy);
            offset += copy;
        }
    }

    crypto_zero(hash, sizeof(hash));
}

// =============================================================================
// RSA Operations (Minimal Implementation)
// =============================================================================

// Note: This is a simplified implementation for demonstration.
// A production SSH server would use a proper bignum library.

// RSA public key structure
typedef struct {
    uint8_t *n;     // Modulus
    size_t n_len;
    uint8_t *e;     // Public exponent
    size_t e_len;
} rsa_public_key_t;

// RSA private key structure
typedef struct {
    uint8_t *n;     // Modulus
    size_t n_len;
    uint8_t *e;     // Public exponent
    size_t e_len;
    uint8_t *d;     // Private exponent
    size_t d_len;
    uint8_t *p;     // Prime factor p
    size_t p_len;
    uint8_t *q;     // Prime factor q
    size_t q_len;
} rsa_private_key_t;

// PKCS#1 v1.5 signature padding for SHA-256
// DigestInfo for SHA-256
static const uint8_t sha256_digest_info[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
    0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
    0x00, 0x04, 0x20
};

// Create PKCS#1 v1.5 signature padding
// EM = 0x00 || 0x01 || PS || 0x00 || T
// where T = DigestInfo || Hash
int rsa_pkcs1_pad_sign(const uint8_t *hash, size_t hash_len,
                       uint8_t *em, size_t em_len) {
    if (em_len < 11 + sizeof(sha256_digest_info) + hash_len) {
        return -1;
    }

    size_t t_len = sizeof(sha256_digest_info) + hash_len;
    size_t ps_len = em_len - 3 - t_len;

    em[0] = 0x00;
    em[1] = 0x01;
    memset(em + 2, 0xFF, ps_len);
    em[2 + ps_len] = 0x00;
    memcpy(em + 3 + ps_len, sha256_digest_info, sizeof(sha256_digest_info));
    memcpy(em + 3 + ps_len + sizeof(sha256_digest_info), hash, hash_len);

    return 0;
}

// Verify PKCS#1 v1.5 signature padding
int rsa_pkcs1_verify_sign(const uint8_t *em, size_t em_len,
                          const uint8_t *hash, size_t hash_len) {
    if (em_len < 11 + sizeof(sha256_digest_info) + hash_len) {
        return -1;
    }

    // Check format
    if (em[0] != 0x00 || em[1] != 0x01) {
        return -1;
    }

    // Find 0x00 separator
    size_t i = 2;
    while (i < em_len && em[i] == 0xFF) {
        i++;
    }
    if (i < 10 || i >= em_len || em[i] != 0x00) {
        return -1;
    }
    i++;

    // Check DigestInfo
    if (i + sizeof(sha256_digest_info) + hash_len > em_len) {
        return -1;
    }
    if (memcmp(em + i, sha256_digest_info, sizeof(sha256_digest_info)) != 0) {
        return -1;
    }
    i += sizeof(sha256_digest_info);

    // Compare hash
    if (crypto_memcmp(em + i, hash, hash_len) != 0) {
        return -1;
    }

    return 0;
}

// =============================================================================
// SSH Host Key Blob Encoding
// =============================================================================

// Encode RSA public key as SSH blob
// Format: string "ssh-rsa" + mpint e + mpint n
size_t ssh_encode_rsa_pubkey(const uint8_t *e, size_t e_len,
                             const uint8_t *n, size_t n_len,
                             uint8_t *blob, size_t blob_max) {
    size_t pos = 0;

    // "ssh-rsa"
    const char *type = "ssh-rsa";
    size_t type_len = 7;

    if (pos + 4 + type_len > blob_max) return 0;
    blob[pos++] = (type_len >> 24) & 0xFF;
    blob[pos++] = (type_len >> 16) & 0xFF;
    blob[pos++] = (type_len >> 8) & 0xFF;
    blob[pos++] = type_len & 0xFF;
    memcpy(blob + pos, type, type_len);
    pos += type_len;

    // mpint e
    // Skip leading zeros, but ensure positive
    while (e_len > 1 && e[0] == 0) { e++; e_len--; }
    int e_pad = (e[0] & 0x80) ? 1 : 0;

    if (pos + 4 + e_pad + e_len > blob_max) return 0;
    blob[pos++] = ((e_len + e_pad) >> 24) & 0xFF;
    blob[pos++] = ((e_len + e_pad) >> 16) & 0xFF;
    blob[pos++] = ((e_len + e_pad) >> 8) & 0xFF;
    blob[pos++] = (e_len + e_pad) & 0xFF;
    if (e_pad) blob[pos++] = 0x00;
    memcpy(blob + pos, e, e_len);
    pos += e_len;

    // mpint n
    while (n_len > 1 && n[0] == 0) { n++; n_len--; }
    int n_pad = (n[0] & 0x80) ? 1 : 0;

    if (pos + 4 + n_pad + n_len > blob_max) return 0;
    blob[pos++] = ((n_len + n_pad) >> 24) & 0xFF;
    blob[pos++] = ((n_len + n_pad) >> 16) & 0xFF;
    blob[pos++] = ((n_len + n_pad) >> 8) & 0xFF;
    blob[pos++] = (n_len + n_pad) & 0xFF;
    if (n_pad) blob[pos++] = 0x00;
    memcpy(blob + pos, n, n_len);
    pos += n_len;

    return pos;
}

// Decode RSA public key from SSH blob
int ssh_decode_rsa_pubkey(const uint8_t *blob, size_t blob_len,
                          uint8_t **e, size_t *e_len,
                          uint8_t **n, size_t *n_len) {
    size_t pos = 0;

    // Skip type string
    if (pos + 4 > blob_len) return -1;
    uint32_t type_len = ((uint32_t)blob[pos] << 24) |
                        ((uint32_t)blob[pos+1] << 16) |
                        ((uint32_t)blob[pos+2] << 8) |
                        (uint32_t)blob[pos+3];
    pos += 4;
    if (pos + type_len > blob_len) return -1;
    // Could verify type is "ssh-rsa"
    pos += type_len;

    // Read e
    if (pos + 4 > blob_len) return -1;
    *e_len = ((uint32_t)blob[pos] << 24) |
             ((uint32_t)blob[pos+1] << 16) |
             ((uint32_t)blob[pos+2] << 8) |
             (uint32_t)blob[pos+3];
    pos += 4;
    if (pos + *e_len > blob_len) return -1;
    *e = (uint8_t *)(blob + pos);
    pos += *e_len;

    // Read n
    if (pos + 4 > blob_len) return -1;
    *n_len = ((uint32_t)blob[pos] << 24) |
             ((uint32_t)blob[pos+1] << 16) |
             ((uint32_t)blob[pos+2] << 8) |
             (uint32_t)blob[pos+3];
    pos += 4;
    if (pos + *n_len > blob_len) return -1;
    *n = (uint8_t *)(blob + pos);

    return 0;
}

// =============================================================================
// SSH Signature Encoding
// =============================================================================

// Encode RSA signature as SSH blob
// Format: string algorithm + string signature
size_t ssh_encode_rsa_signature(const char *algorithm,
                                const uint8_t *sig, size_t sig_len,
                                uint8_t *blob, size_t blob_max) {
    size_t pos = 0;
    size_t algo_len = strlen(algorithm);

    // Algorithm name
    if (pos + 4 + algo_len > blob_max) return 0;
    blob[pos++] = (algo_len >> 24) & 0xFF;
    blob[pos++] = (algo_len >> 16) & 0xFF;
    blob[pos++] = (algo_len >> 8) & 0xFF;
    blob[pos++] = algo_len & 0xFF;
    memcpy(blob + pos, algorithm, algo_len);
    pos += algo_len;

    // Signature data
    if (pos + 4 + sig_len > blob_max) return 0;
    blob[pos++] = (sig_len >> 24) & 0xFF;
    blob[pos++] = (sig_len >> 16) & 0xFF;
    blob[pos++] = (sig_len >> 8) & 0xFF;
    blob[pos++] = sig_len & 0xFF;
    memcpy(blob + pos, sig, sig_len);
    pos += sig_len;

    return pos;
}

// =============================================================================
// Random Number Generation for Crypto
// =============================================================================

// Generate cryptographically secure random bytes
int ssh_random_bytes(uint8_t *buf, size_t len) {
    return rng_get_bytes(buf, len);
}

// Generate random mpint (big integer) of specified bit length
int ssh_random_mpint(uint8_t *buf, size_t *len, int bits) {
    int bytes = (bits + 7) / 8;
    if (bytes > (int)*len) {
        return -1;
    }

    rng_get_bytes(buf, bytes);

    // Set high bit to ensure full length
    buf[0] |= 0x80;

    // Ensure positive (clear MSB if needed for mpint format)
    // Actually for DH private key we want it to be full range

    *len = bytes;
    return 0;
}
