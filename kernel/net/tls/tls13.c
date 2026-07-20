// tls13.c - TLS 1.3 protocol implementation for MayteraOS
// Implements RFC 8446 key derivation and record layer

#include "tls13.h"
#include "../../crypto/crypto.h"
#include "../../crypto/chacha20.h"
#include "../../string.h"
#include "../../mm/heap.h"
#include "../../serial.h"

// =============================================================================
// X25519 Key Exchange (Curve25519)
// =============================================================================

// Field element: 256-bit number represented as 10 limbs of ~25.5 bits each
typedef int64_t fe[10];

// Load field element from bytes (little-endian)
// Based on SUPERCOP ref10 by Daniel J. Bernstein (public domain)
static int64_t fe_load3(const uint8_t *s) {
    return (int64_t)s[0] | ((int64_t)s[1] << 8) | ((int64_t)s[2] << 16);
}
static int64_t fe_load4(const uint8_t *s) {
    return (int64_t)s[0] | ((int64_t)s[1] << 8) |
           ((int64_t)s[2] << 16) | ((int64_t)s[3] << 24);
}

static void fe_frombytes(fe h, const uint8_t *s) {
    int64_t h0 = fe_load4(s);
    int64_t h1 = fe_load3(s + 4) << 6;
    int64_t h2 = fe_load3(s + 7) << 5;
    int64_t h3 = fe_load3(s + 10) << 3;
    int64_t h4 = fe_load3(s + 13) << 2;
    int64_t h5 = fe_load4(s + 16);
    int64_t h6 = fe_load3(s + 20) << 7;
    int64_t h7 = fe_load3(s + 23) << 5;
    int64_t h8 = fe_load3(s + 26) << 4;
    int64_t h9 = (fe_load3(s + 29) & 0x7FFFFF) << 2;

    int64_t carry;
    carry = (h9 + (1LL << 24)) >> 25; h0 += carry * 19; h9 -= carry << 25;
    carry = (h1 + (1LL << 24)) >> 25; h2 += carry; h1 -= carry << 25;
    carry = (h3 + (1LL << 24)) >> 25; h4 += carry; h3 -= carry << 25;
    carry = (h5 + (1LL << 24)) >> 25; h6 += carry; h5 -= carry << 25;
    carry = (h7 + (1LL << 24)) >> 25; h8 += carry; h7 -= carry << 25;

    carry = (h0 + (1LL << 25)) >> 26; h1 += carry; h0 -= carry << 26;
    carry = (h2 + (1LL << 25)) >> 26; h3 += carry; h2 -= carry << 26;
    carry = (h4 + (1LL << 25)) >> 26; h5 += carry; h4 -= carry << 26;
    carry = (h6 + (1LL << 25)) >> 26; h7 += carry; h6 -= carry << 26;
    carry = (h8 + (1LL << 25)) >> 26; h9 += carry; h8 -= carry << 26;

    h[0] = h0; h[1] = h1; h[2] = h2; h[3] = h3; h[4] = h4;
    h[5] = h5; h[6] = h6; h[7] = h7; h[8] = h8; h[9] = h9;
}

// Store field element to bytes
// Based on SUPERCOP ref10 by Daniel J. Bernstein (public domain)
static void fe_tobytes(uint8_t *s, const fe h) {
    int64_t t[10];
    for (int i = 0; i < 10; i++) t[i] = h[i];

    // Full reduction mod 2^255-19
    int64_t q = (19 * t[9] + ((int64_t)1 << 24)) >> 25;
    q = (t[0] + q) >> 26;
    q = (t[1] + q) >> 25;
    q = (t[2] + q) >> 26;
    q = (t[3] + q) >> 25;
    q = (t[4] + q) >> 26;
    q = (t[5] + q) >> 25;
    q = (t[6] + q) >> 26;
    q = (t[7] + q) >> 25;
    q = (t[8] + q) >> 26;
    q = (t[9] + q) >> 25;

    t[0] += 19 * q;

    int64_t carry;
    carry = t[0] >> 26; t[1] += carry; t[0] -= carry << 26;
    carry = t[1] >> 25; t[2] += carry; t[1] -= carry << 25;
    carry = t[2] >> 26; t[3] += carry; t[2] -= carry << 26;
    carry = t[3] >> 25; t[4] += carry; t[3] -= carry << 25;
    carry = t[4] >> 26; t[5] += carry; t[4] -= carry << 26;
    carry = t[5] >> 25; t[6] += carry; t[5] -= carry << 25;
    carry = t[6] >> 26; t[7] += carry; t[6] -= carry << 26;
    carry = t[7] >> 25; t[8] += carry; t[7] -= carry << 25;
    carry = t[8] >> 26; t[9] += carry; t[8] -= carry << 26;
    carry = t[9] >> 25;                t[9] -= carry << 25;

    // Pack into bytes: 26,25,26,25,26,25,26,25,26,25 bit limbs
    // t[0] has 26 bits: positions 0-25
    // t[1] has 25 bits: positions 26-50
    // t[2] has 26 bits: positions 51-76
    // etc.
    s[0]  = (uint8_t)(t[0]);
    s[1]  = (uint8_t)(t[0] >> 8);
    s[2]  = (uint8_t)(t[0] >> 16);
    s[3]  = (uint8_t)((t[0] >> 24) | (t[1] << 2));
    s[4]  = (uint8_t)(t[1] >> 6);
    s[5]  = (uint8_t)(t[1] >> 14);
    s[6]  = (uint8_t)((t[1] >> 22) | (t[2] << 3));
    s[7]  = (uint8_t)(t[2] >> 5);
    s[8]  = (uint8_t)(t[2] >> 13);
    s[9]  = (uint8_t)((t[2] >> 21) | (t[3] << 5));
    s[10] = (uint8_t)(t[3] >> 3);
    s[11] = (uint8_t)(t[3] >> 11);
    s[12] = (uint8_t)((t[3] >> 19) | (t[4] << 6));
    s[13] = (uint8_t)(t[4] >> 2);
    s[14] = (uint8_t)(t[4] >> 10);
    s[15] = (uint8_t)(t[4] >> 18);
    s[16] = (uint8_t)(t[5]);
    s[17] = (uint8_t)(t[5] >> 8);
    s[18] = (uint8_t)(t[5] >> 16);
    s[19] = (uint8_t)((t[5] >> 24) | (t[6] << 1));
    s[20] = (uint8_t)(t[6] >> 7);
    s[21] = (uint8_t)(t[6] >> 15);
    s[22] = (uint8_t)((t[6] >> 23) | (t[7] << 3));
    s[23] = (uint8_t)(t[7] >> 5);
    s[24] = (uint8_t)(t[7] >> 13);
    s[25] = (uint8_t)((t[7] >> 21) | (t[8] << 4));
    s[26] = (uint8_t)(t[8] >> 4);
    s[27] = (uint8_t)(t[8] >> 12);
    s[28] = (uint8_t)((t[8] >> 20) | (t[9] << 6));
    s[29] = (uint8_t)(t[9] >> 2);
    s[30] = (uint8_t)(t[9] >> 10);
    s[31] = (uint8_t)(t[9] >> 18);
}

// Field operations
static void fe_0(fe h) { for (int i = 0; i < 10; i++) h[i] = 0; }
static void fe_1(fe h) { h[0] = 1; for (int i = 1; i < 10; i++) h[i] = 0; }
static void fe_copy(fe h, const fe f) { for (int i = 0; i < 10; i++) h[i] = f[i]; }

static void fe_add(fe h, const fe f, const fe g) {
    for (int i = 0; i < 10; i++) h[i] = f[i] + g[i];
}

static void fe_sub(fe h, const fe f, const fe g) {
    for (int i = 0; i < 10; i++) h[i] = f[i] - g[i];
}

// Carry-reduce after multiplication
static void fe_carry(fe h) {
    int64_t carry;
    carry = h[0] >> 26; h[1] += carry; h[0] -= carry << 26;
    carry = h[1] >> 25; h[2] += carry; h[1] -= carry << 25;
    carry = h[2] >> 26; h[3] += carry; h[2] -= carry << 26;
    carry = h[3] >> 25; h[4] += carry; h[3] -= carry << 25;
    carry = h[4] >> 26; h[5] += carry; h[4] -= carry << 26;
    carry = h[5] >> 25; h[6] += carry; h[5] -= carry << 25;
    carry = h[6] >> 26; h[7] += carry; h[6] -= carry << 26;
    carry = h[7] >> 25; h[8] += carry; h[7] -= carry << 25;
    carry = h[8] >> 26; h[9] += carry; h[8] -= carry << 26;
    carry = h[9] >> 25; h[0] += carry * 19; h[9] -= carry << 25;
    carry = h[0] >> 26; h[1] += carry; h[0] -= carry << 26;
}

static void fe_mul(fe h, const fe f, const fe g) {
    // Schoolbook multiplication with delayed carry
    int64_t f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    int64_t f5 = f[5], f6 = f[6], f7 = f[7], f8 = f[8], f9 = f[9];
    int64_t g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
    int64_t g5 = g[5], g6 = g[6], g7 = g[7], g8 = g[8], g9 = g[9];
    
    int64_t g1_19 = g1 * 19, g2_19 = g2 * 19, g3_19 = g3 * 19, g4_19 = g4 * 19;
    int64_t g5_19 = g5 * 19, g6_19 = g6 * 19, g7_19 = g7 * 19, g8_19 = g8 * 19, g9_19 = g9 * 19;
    
    h[0] = f0*g0 + 2*(f1*g9_19 + f3*g7_19 + f5*g5_19 + f7*g3_19 + f9*g1_19) + f2*g8_19 + f4*g6_19 + f6*g4_19 + f8*g2_19;
    h[1] = f0*g1 + f1*g0 + f2*g9_19 + f3*g8_19 + f4*g7_19 + f5*g6_19 + f6*g5_19 + f7*g4_19 + f8*g3_19 + f9*g2_19;
    h[2] = f0*g2 + 2*(f1*g1 + f3*g9_19 + f5*g7_19 + f7*g5_19 + f9*g3_19) + f2*g0 + f4*g8_19 + f6*g6_19 + f8*g4_19;
    h[3] = f0*g3 + f1*g2 + f2*g1 + f3*g0 + f4*g9_19 + f5*g8_19 + f6*g7_19 + f7*g6_19 + f8*g5_19 + f9*g4_19;
    h[4] = f0*g4 + 2*(f1*g3 + f3*g1 + f5*g9_19 + f7*g7_19 + f9*g5_19) + f2*g2 + f4*g0 + f6*g8_19 + f8*g6_19;
    h[5] = f0*g5 + f1*g4 + f2*g3 + f3*g2 + f4*g1 + f5*g0 + f6*g9_19 + f7*g8_19 + f8*g7_19 + f9*g6_19;
    h[6] = f0*g6 + 2*(f1*g5 + f3*g3 + f5*g1 + f7*g9_19 + f9*g7_19) + f2*g4 + f4*g2 + f6*g0 + f8*g8_19;
    h[7] = f0*g7 + f1*g6 + f2*g5 + f3*g4 + f4*g3 + f5*g2 + f6*g1 + f7*g0 + f8*g9_19 + f9*g8_19;
    h[8] = f0*g8 + 2*(f1*g7 + f3*g5 + f5*g3 + f7*g1 + f9*g9_19) + f2*g6 + f4*g4 + f6*g2 + f8*g0;
    h[9] = f0*g9 + f1*g8 + f2*g7 + f3*g6 + f4*g5 + f5*g4 + f6*g3 + f7*g2 + f8*g1 + f9*g0;
    
    fe_carry(h);
}

static void fe_sq(fe h, const fe f) {
    fe_mul(h, f, f);
}

// Modular inversion using Fermat's little theorem: a^(-1) = a^(p-2) mod p
static void fe_invert(fe out, const fe z) {
    fe t0, t1, t2, t3;
    int i;
    
    // Compute z^(2^255-21) using addition chain
    fe_sq(t0, z);
    fe_sq(t1, t0);
    fe_sq(t1, t1);
    fe_mul(t1, z, t1);
    fe_mul(t0, t0, t1);
    fe_sq(t2, t0);
    fe_mul(t1, t1, t2);
    fe_sq(t2, t1);
    for (i = 1; i < 5; i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (i = 1; i < 10; i++) fe_sq(t2, t2);
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (i = 1; i < 20; i++) fe_sq(t3, t3);
    fe_mul(t2, t3, t2);
    fe_sq(t2, t2);
    for (i = 1; i < 10; i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (i = 1; i < 50; i++) fe_sq(t2, t2);
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (i = 1; i < 100; i++) fe_sq(t3, t3);
    fe_mul(t2, t3, t2);
    fe_sq(t2, t2);
    for (i = 1; i < 50; i++) fe_sq(t2, t2);
    fe_mul(t1, t2, t1);
    fe_sq(t1, t1);
    for (i = 1; i < 5; i++) fe_sq(t1, t1);
    fe_mul(out, t1, t0);
}

// X25519 scalar multiplication using Montgomery ladder
static void x25519_scalar_mult(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t e[32];
    memcpy(e, scalar, 32);
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;
    
    fe x1, x2, z2, x3, z3, tmp0, tmp1;
    
    fe_frombytes(x1, point);
    fe_1(x2);
    fe_0(z2);
    fe_copy(x3, x1);
    fe_1(z3);
    
    int swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        int b = (e[pos / 8] >> (pos % 8)) & 1;
        swap ^= b;
        
        // Conditional swap
        if (swap) {
            fe t;
            fe_copy(t, x2); fe_copy(x2, x3); fe_copy(x3, t);
            fe_copy(t, z2); fe_copy(z2, z3); fe_copy(z3, t);
        }
        swap = b;
        
        fe_sub(tmp0, x3, z3);
        fe_sub(tmp1, x2, z2);
        fe_add(x2, x2, z2);
        fe_add(z2, x3, z3);
        fe_mul(z3, tmp0, x2);
        fe_mul(z2, z2, tmp1);
        fe_sq(tmp0, tmp1);
        fe_sq(tmp1, x2);
        fe_add(x3, z3, z2);
        fe_sub(z2, z3, z2);
        fe_mul(x2, tmp1, tmp0);
        fe_sub(tmp1, tmp1, tmp0);
        fe_sq(z2, z2);
        
        // Multiply by 121666 = (A+2)/4 for curve25519
        fe tmp2;
        for (int i = 0; i < 10; i++) tmp2[i] = tmp1[i] * 121666;
        fe_carry(tmp2);
        
        fe_sq(x3, x3);
        fe_add(tmp2, tmp2, tmp0);
        fe_mul(z3, x1, z2);
        fe_mul(z2, tmp1, tmp2);
    }
    
    if (swap) {
        fe t;
        fe_copy(t, x2); fe_copy(x2, x3); fe_copy(x3, t);
        fe_copy(t, z2); fe_copy(z2, z3); fe_copy(z3, t);
    }
    
    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);
}

// Base point for X25519 (u = 9)
static const uint8_t x25519_basepoint[32] = {9};

void x25519_generate_keypair(x25519_keypair_t *kp) {
    // Generate random private key
    rng_get_bytes(kp->private_key, 32);
    
    // Clamp private key
    kp->private_key[0] &= 248;
    kp->private_key[31] &= 127;
    kp->private_key[31] |= 64;
    
    // Compute public key
    x25519_scalar_mult(kp->public_key, kp->private_key, x25519_basepoint);
}

void x25519_shared_secret(const uint8_t *their_public,
                          const uint8_t *my_private,
                          uint8_t *shared) {
    x25519_scalar_mult(shared, my_private, their_public);
}

// =============================================================================
// HKDF (RFC 5869)
// =============================================================================

void hkdf_extract(const uint8_t *salt, size_t salt_len,
                  const uint8_t *ikm, size_t ikm_len,
                  uint8_t *prk, size_t hash_len) {
    // If salt not provided, use hash_len zeros
    uint8_t default_salt[48] = {0};
    if (!salt || salt_len == 0) {
        salt = default_salt;
        salt_len = hash_len;
    }
    
    // PRK = HMAC-Hash(salt, IKM); SHA-384 for the AES-256-GCM-SHA384 suite
    // (hash_len 48), SHA-256 otherwise.
    if (hash_len == 48) {
        hmac_sha384(salt, salt_len, ikm, ikm_len, prk);
    } else {
        hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
    }
}

void hkdf_expand_label(const uint8_t *secret, size_t secret_len,
                       const char *label,
                       const uint8_t *context, size_t context_len,
                       uint8_t *output, size_t output_len,
                       size_t hash_len) {
    // Build HkdfLabel structure:
    // struct {
    //     uint16 length = Length;
    //     opaque label<7..255> = "tls13 " + Label;
    //     opaque context<0..255> = Context;
    // } HkdfLabel;
    
    const char *prefix = "tls13 ";
    size_t prefix_len = 6;
    size_t label_len = strlen(label);
    size_t total_label_len = prefix_len + label_len;
    
    uint8_t hkdf_label[256 + 3 + 256];
    size_t pos = 0;
    
    // Length
    hkdf_label[pos++] = (output_len >> 8) & 0xff;
    hkdf_label[pos++] = output_len & 0xff;
    
    // Label
    hkdf_label[pos++] = (uint8_t)total_label_len;
    memcpy(hkdf_label + pos, prefix, prefix_len);
    pos += prefix_len;
    memcpy(hkdf_label + pos, label, label_len);
    pos += label_len;
    
    // Context
    hkdf_label[pos++] = (uint8_t)context_len;
    if (context_len > 0) {
        memcpy(hkdf_label + pos, context, context_len);
        pos += context_len;
    }
    
    // HKDF-Expand
    size_t done = 0;
    uint8_t counter = 1;
    uint8_t block[64];  // Max hash output
    uint8_t prev[64] = {0};
    size_t prev_len = 0;
    
    while (done < output_len) {
        // T(i) = HMAC-Hash(PRK, T(i-1) | info | counter)
        if (hash_len == 48) {
            hmac_sha384_ctx_t ctx;
            hmac_sha384_init(&ctx, secret, secret_len);
            if (prev_len > 0) {
                hmac_sha384_update(&ctx, prev, prev_len);
            }
            hmac_sha384_update(&ctx, hkdf_label, pos);
            hmac_sha384_update(&ctx, &counter, 1);
            hmac_sha384_final(&ctx, block);
        } else {
            hmac_sha256_ctx_t ctx;
            hmac_sha256_init(&ctx, secret, secret_len);
            if (prev_len > 0) {
                hmac_sha256_update(&ctx, prev, prev_len);
            }
            hmac_sha256_update(&ctx, hkdf_label, pos);
            hmac_sha256_update(&ctx, &counter, 1);
            hmac_sha256_final(&ctx, block);
        }

        size_t copy = (output_len - done < hash_len) ? (output_len - done) : hash_len;
        memcpy(output + done, block, copy);
        done += copy;
        
        memcpy(prev, block, hash_len);
        prev_len = hash_len;
        counter++;
    }
}

// =============================================================================
// TLS 1.3 Key Schedule
// =============================================================================

void tls13_derive_secrets(tls13_key_schedule_t *ks,
                          const uint8_t *shared_secret, size_t secret_len,
                          const uint8_t *hello_hash, size_t hash_len) {
    uint8_t zero_ikm[48] = {0};
    uint8_t empty_hash[48];
    if (hash_len == 48) {
        sha384((const uint8_t *)"", 0, empty_hash);
    } else {
        sha256((const uint8_t *)"", 0, empty_hash);
    }
    uint8_t derived[48];

    ks->hash_len = (hash_len == 48) ? 48 : 32;
    
    // Early Secret
    hkdf_extract(NULL, 0, zero_ikm, hash_len, ks->early_secret, hash_len);
    
    // Derive-Secret for handshake
    hkdf_expand_label(ks->early_secret, hash_len, "derived",
                      empty_hash, hash_len,
                      derived, hash_len, hash_len);
    
    // Handshake Secret
    hkdf_extract(derived, hash_len, shared_secret, secret_len,
                 ks->handshake_secret, hash_len);
    
    // Client/Server Handshake Traffic Secrets
    hkdf_expand_label(ks->handshake_secret, hash_len, "c hs traffic",
                      hello_hash, hash_len,
                      ks->client_handshake_traffic, hash_len, hash_len);
    
    hkdf_expand_label(ks->handshake_secret, hash_len, "s hs traffic",
                      hello_hash, hash_len,
                      ks->server_handshake_traffic, hash_len, hash_len);
}

void tls13_derive_handshake_keys(tls13_key_schedule_t *ks, uint16_t cipher_suite) {
    size_t key_len = (cipher_suite == TLS13_AES_256_GCM_SHA384) ? 32 : 16;
    size_t iv_len = 12;
    size_t hash_len = ks->hash_len;
    
    // Client handshake key/iv
    hkdf_expand_label(ks->client_handshake_traffic, hash_len, "key",
                      NULL, 0, ks->client_write_key, key_len, hash_len);
    hkdf_expand_label(ks->client_handshake_traffic, hash_len, "iv",
                      NULL, 0, ks->client_write_iv, iv_len, hash_len);
    
    // Server handshake key/iv
    hkdf_expand_label(ks->server_handshake_traffic, hash_len, "key",
                      NULL, 0, ks->server_write_key, key_len, hash_len);
    hkdf_expand_label(ks->server_handshake_traffic, hash_len, "iv",
                      NULL, 0, ks->server_write_iv, iv_len, hash_len);
}

void tls13_derive_app_keys(tls13_key_schedule_t *ks,
                           const uint8_t *handshake_hash,
                           uint16_t cipher_suite) {
    size_t key_len = (cipher_suite == TLS13_AES_256_GCM_SHA384) ? 32 : 16;
    size_t iv_len = 12;
    size_t hash_len = ks->hash_len;
    uint8_t derived[48];
    uint8_t zero_ikm[48] = {0};
    uint8_t empty_hash[48];
    if (hash_len == 48) {
        sha384((const uint8_t *)"", 0, empty_hash);
    } else {
        sha256((const uint8_t *)"", 0, empty_hash);
    }

    // Derive Master Secret
    hkdf_expand_label(ks->handshake_secret, hash_len, "derived",
                      empty_hash, hash_len,
                      derived, hash_len, hash_len);
    
    hkdf_extract(derived, hash_len, zero_ikm, hash_len,
                 ks->master_secret, hash_len);
    
    // Application Traffic Secrets
    hkdf_expand_label(ks->master_secret, hash_len, "c ap traffic",
                      handshake_hash, hash_len,
                      ks->client_app_traffic, hash_len, hash_len);
    
    hkdf_expand_label(ks->master_secret, hash_len, "s ap traffic",
                      handshake_hash, hash_len,
                      ks->server_app_traffic, hash_len, hash_len);
    
    // Derive application keys
    hkdf_expand_label(ks->client_app_traffic, hash_len, "key",
                      NULL, 0, ks->client_write_key, key_len, hash_len);
    hkdf_expand_label(ks->client_app_traffic, hash_len, "iv",
                      NULL, 0, ks->client_write_iv, iv_len, hash_len);
    
    hkdf_expand_label(ks->server_app_traffic, hash_len, "key",
                      NULL, 0, ks->server_write_key, key_len, hash_len);
    hkdf_expand_label(ks->server_app_traffic, hash_len, "iv",
                      NULL, 0, ks->server_write_iv, iv_len, hash_len);
}

// =============================================================================
// TLS 1.3 Record Encryption/Decryption
// =============================================================================

// Build nonce by XORing IV with sequence number
static void tls13_build_nonce(const uint8_t iv[12], uint64_t seq_num, uint8_t nonce[12]) {
    memcpy(nonce, iv, 12);
    
    // XOR sequence number into rightmost bytes
    for (int i = 0; i < 8; i++) {
        nonce[11 - i] ^= (seq_num >> (i * 8)) & 0xff;
    }
}

int tls13_encrypt_record(const uint8_t *key, const uint8_t *iv,
                         uint64_t seq_num, uint8_t content_type,
                         const uint8_t *plaintext, size_t plaintext_len,
                         uint8_t *ciphertext, size_t *ciphertext_len,
                         uint16_t cipher_suite) {
    // TLS 1.3 inner plaintext: data + content_type
    size_t inner_len = plaintext_len + 1;
    uint8_t *inner = kmalloc(inner_len);
    if (!inner) return -1;
    
    memcpy(inner, plaintext, plaintext_len);
    inner[plaintext_len] = content_type;
    
    // Build nonce
    uint8_t nonce[12];
    tls13_build_nonce(iv, seq_num, nonce);
    
    // Build AAD: record header
    // TLS 1.3 records always claim to be application_data (0x17) and TLS 1.2 (0x0303)
    uint8_t aad[5];
    aad[0] = TLS_CONTENT_APPLICATION;
    aad[1] = 0x03;
    aad[2] = 0x03;
    // Length = inner_len + tag_len
    size_t record_len = inner_len + 16;  // 16-byte tag
    aad[3] = (record_len >> 8) & 0xff;
    aad[4] = record_len & 0xff;
    
    // Encrypt based on cipher suite
    if (cipher_suite == TLS13_CHACHA20_POLY1305_SHA256) {
        chacha20_poly1305_seal(key, nonce, aad, 5, inner, inner_len, ciphertext);
    } else {
        // AES-GCM
        aes_gcm_ctx_t gcm;
        int key_bits = (cipher_suite == TLS13_AES_256_GCM_SHA384) ? 256 : 128;
        aes_gcm_init(&gcm, key, key_bits, nonce, 12);
        aes_gcm_aad(&gcm, aad, 5);
        aes_gcm_encrypt(&gcm, inner, ciphertext, inner_len);
        aes_gcm_final(&gcm, ciphertext + inner_len, 16);
    }
    
    *ciphertext_len = inner_len + 16;
    
    kfree(inner);
    return 0;
}

int tls13_decrypt_record(const uint8_t *key, const uint8_t *iv,
                         uint64_t seq_num,
                         const uint8_t *ciphertext, size_t ciphertext_len,
                         uint8_t *plaintext, size_t *plaintext_len,
                         uint8_t *content_type,
                         uint16_t cipher_suite) {
    if (ciphertext_len < 17) return -1;  // At least 1 byte + 16-byte tag
    
    size_t inner_len = ciphertext_len - 16;
    
    // Build nonce
    uint8_t nonce[12];
    tls13_build_nonce(iv, seq_num, nonce);
    
    // Build AAD
    uint8_t aad[5];
    aad[0] = TLS_CONTENT_APPLICATION;
    aad[1] = 0x03;
    aad[2] = 0x03;
    aad[3] = (ciphertext_len >> 8) & 0xff;
    aad[4] = ciphertext_len & 0xff;
    
    // Decrypt based on cipher suite
    uint8_t *inner = kmalloc(inner_len);
    if (!inner) return -1;
    
    int ret;
    if (cipher_suite == TLS13_CHACHA20_POLY1305_SHA256) {
        ret = chacha20_poly1305_open(key, nonce, aad, 5, 
                                      ciphertext, ciphertext_len, inner);
    } else {
        // AES-GCM
        aes_gcm_ctx_t gcm;
        int key_bits = (cipher_suite == TLS13_AES_256_GCM_SHA384) ? 256 : 128;
        aes_gcm_init(&gcm, key, key_bits, nonce, 12);
        aes_gcm_aad(&gcm, aad, 5);
        aes_gcm_decrypt(&gcm, ciphertext, inner, inner_len);
        ret = aes_gcm_verify(&gcm, ciphertext + inner_len, 16);
    }
    
    if (ret != 0) {
        kprintf("[TLS1.3] GCM verify failed, nonce: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                nonce[0], nonce[1], nonce[2], nonce[3], nonce[4], nonce[5],
                nonce[6], nonce[7], nonce[8], nonce[9], nonce[10], nonce[11]);
        kprintf("[TLS1.3] AAD: %02x %02x %02x %02x %02x (ct_len=%u)\n",
                aad[0], aad[1], aad[2], aad[3], aad[4], (unsigned)ciphertext_len);
        kfree(inner);
        return -1;
    }
    
    // Remove padding and extract content type
    // Inner plaintext ends with content_type, possibly preceded by zeros
    size_t i = inner_len - 1;
    while (i > 0 && inner[i] == 0) i--;
    
    *content_type = inner[i];
    *plaintext_len = i;
    memcpy(plaintext, inner, i);
    
    kfree(inner);
    return 0;
}
