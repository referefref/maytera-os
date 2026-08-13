// crypto.c - Cryptographic subsystem initialization and utilities
// MayteraOS

#include "crypto.h"
#include "../string.h"
#include "../serial.h"
#include "../cpu/dlprof.h"
#include "fs/bootlog.h"   // #742: the owning header, NOT a private extern

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

// #615 Rust seam. GHASH is the AES-GCM bottleneck: the bit-serial multiply
// below is 128 iterations of a 16-byte loop per 16 bytes of record, measured at
// ~8,500 cycles per block (~530 cycles/BYTE) and at 36-40% of one core for the
// entire App Store download (build 975 [DLPROF]). rustkern/ghash.rs implements
// the standard 4-bit table method instead. The C stays as ghash_multiply_c /
// ghash_update_c: it is the differential reference for ghash_rust_selftest()
// and the rollback path (drop -DRUST_GHASH).
extern void ghash_key_init_rs(ghash_key_t *key, const uint8_t *h);
extern void ghash_mul_rs(const ghash_key_t *key, uint8_t *x);
extern void ghash_update_rs(const ghash_key_t *key, uint8_t *s,
                            const uint8_t *data, size_t len);

// GHASH multiplication in GF(2^128)
static void ghash_multiply_c(uint8_t *x, const uint8_t *h) {
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
static void ghash_update_c(uint8_t *s, const uint8_t *h, const uint8_t *data, size_t len) {
    while (len >= 16) {
        for (int i = 0; i < 16; i++) {
            s[i] ^= data[i];
        }
        ghash_multiply_c(s, h);
        data += 16;
        len -= 16;
    }

    // Handle partial block
    if (len > 0) {
        for (size_t i = 0; i < len; i++) {
            s[i] ^= data[i];
        }
        ghash_multiply_c(s, h);
    }
}

// Live dispatchers. Everything below calls THESE, never the raw C, so a single
// -DRUST_GHASH switch moves the whole AES-GCM MAC path between the two.
static inline void gcm_ghash_mul(aes_gcm_ctx_t *ctx, uint8_t *x) {
#ifdef RUST_GHASH
    ghash_mul_rs(&ctx->ghk, x);
#else
    ghash_multiply_c(x, ctx->h);
#endif
}
static inline void gcm_ghash_update(aes_gcm_ctx_t *ctx, uint8_t *s,
                                    const uint8_t *data, size_t len) {
#ifdef RUST_GHASH
    ghash_update_rs(&ctx->ghk, s, data, len);
#else
    ghash_update_c(s, ctx->h, data, len);
#endif
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

    // #615: derive the nibble tables for this H BEFORE any GHASH runs below.
#ifdef RUST_GHASH
    ghash_key_init_rs(&ctx->ghk, ctx->h);
#endif

    // Compute J0 (initial counter)
    memset(ctx->j0, 0, 16);
    if (iv_len == 12) {
        // Standard 96-bit IV
        memcpy(ctx->j0, iv, 12);
        ctx->j0[15] = 1;
    } else {
        // Hash IV to get J0
        gcm_ghash_update(ctx, ctx->j0, iv, iv_len);
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
        gcm_ghash_update(ctx, ctx->j0, len_block, 16);
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
    gcm_ghash_update(ctx, ctx->s, aad, aad_len);
    ctx->aad_len += aad_len * 8;
}

// Encrypt
void aes_gcm_encrypt(aes_gcm_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t length) {
    uint8_t keystream[16];
    uint64_t _dp_t0 = dp_tsc(); g_dp_gcm_bytes += length;

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
            gcm_ghash_mul(ctx, ctx->s);
        } else {
            for (size_t i = 0; i < block_len; i++) {
                ctx->s[i] ^= out[i];
            }
            gcm_ghash_mul(ctx, ctx->s);
        }

        ctx->cipher_len += block_len * 8;
        in += block_len;
        out += block_len;
        length -= block_len;
    }

    g_dp_gcm_cyc += dp_tsc() - _dp_t0;
    crypto_zero(keystream, sizeof(keystream));
}

// Decrypt
int aes_gcm_decrypt(aes_gcm_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t length) {
    uint8_t keystream[16];
    uint64_t _dp_t0 = dp_tsc(); g_dp_gcm_bytes += length;

    while (length > 0) {
        // Update GHASH with ciphertext first
        size_t block_len = (length < 16) ? length : 16;
        for (size_t i = 0; i < block_len; i++) {
            ctx->s[i] ^= in[i];
        }
        if (block_len == 16 || length == block_len) {
            gcm_ghash_mul(ctx, ctx->s);
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

    g_dp_gcm_cyc += dp_tsc() - _dp_t0;
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
    gcm_ghash_mul(ctx, ctx->s);

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

// =============================================================================
// #615 GHASH Rust seam: boot-time differential + known-answer + benchmark.
//
// The Rust 4-bit table method (rustkern/ghash.rs) is an INDEPENDENT algorithm,
// not a transliteration of the bit-serial C, so this differential is a real
// cross-check rather than the both-arms-share-the-bug trap. Bounded, runs once
// at boot before TLS uses AES-GCM, prints one [RUST-DIFF] and one [RUST-PERF]
// line to serial + /BOOTLOG. No waiting of any kind (#426).
// =============================================================================

static uint64_t ghdiff_rng(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *s = x;
    return x;
}

static int gh_hex2bytes(const char *hex, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int hi = hex[2*i], lo = hex[2*i + 1];
        hi = (hi >= '0' && hi <= '9') ? hi - '0' : ((hi | 32) - 'a' + 10);
        lo = (lo >= '0' && lo <= '9') ? lo - '0' : ((lo | 32) - 'a' + 10);
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

void ghash_rust_selftest(void) {
    unsigned vectors = 0, mismatches = 0, kat_fail = 0;
    uint64_t seed = 0x4748415348000615ULL;   // 'GHASH' + task number

    // --- Part 1: single-block multiply differential over random (H, X). ---
    for (int n = 0; n < 4096; n++) {
        uint8_t h[16], x[16], a[16], b[16];
        for (int i = 0; i < 16; i++) h[i] = (uint8_t)ghdiff_rng(&seed);
        for (int i = 0; i < 16; i++) x[i] = (uint8_t)ghdiff_rng(&seed);

        memcpy(a, x, 16);
        ghash_multiply_c(a, h);

        ghash_key_t k;
        ghash_key_init_rs(&k, h);
        memcpy(b, x, 16);
        ghash_mul_rs(&k, b);

        vectors++;
        if (memcmp(a, b, 16) != 0) mismatches++;
    }

    // --- Part 2: streaming update differential across EVERY length 0..80, so
    // the zero-padded partial tail (the case a table port is most likely to get
    // wrong) is covered at every offset. ---
    {
        uint8_t data[80];
        for (int len = 0; len <= 80; len++) {
            uint8_t h[16], s0[16], a[16], b[16];
            for (int i = 0; i < 16; i++) h[i] = (uint8_t)ghdiff_rng(&seed);
            for (int i = 0; i < 16; i++) s0[i] = (uint8_t)ghdiff_rng(&seed);
            for (int i = 0; i < len; i++) data[i] = (uint8_t)ghdiff_rng(&seed);

            memcpy(a, s0, 16);
            ghash_update_c(a, h, data, (size_t)len);

            ghash_key_t k;
            ghash_key_init_rs(&k, h);
            memcpy(b, s0, 16);
            ghash_update_rs(&k, b, data, (size_t)len);

            vectors++;
            if (memcmp(a, b, 16) != 0) mismatches++;
        }
    }

    // --- Part 3: NIST GCM AES-128 Test Case 4 through the LIVE aes_gcm_* API
    // (which runs the Rust GHASH under -DRUST_GHASH). 60 bytes of plaintext
    // and 20 bytes of AAD, so both the ciphertext tail AND the AAD tail are
    // partial blocks. Proves the whole GCM construction, not just the leaf. ---
    {
        uint8_t key[16], iv[12], aad[20], pt[60], ct[60], tag[16];
        uint8_t out[60], mac[16];
        gh_hex2bytes("feffe9928665731c6d6a8f9467308308", key, 16);
        gh_hex2bytes("cafebabefacedbaddecaf888", iv, 12);
        gh_hex2bytes("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad, 20);
        gh_hex2bytes("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
                     "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", pt, 60);
        gh_hex2bytes("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
                     "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091", ct, 60);
        gh_hex2bytes("5bc94fbc3221a5db94fae95ae7121a47", tag, 16);

        aes_gcm_ctx_t g;
        aes_gcm_init(&g, key, 128, iv, 12);
        aes_gcm_aad(&g, aad, 20);
        aes_gcm_encrypt(&g, pt, out, 60);
        aes_gcm_final(&g, mac, 16);
        vectors += 2;
        if (memcmp(out, ct, 60) != 0) { mismatches++; kat_fail++; }
        if (memcmp(mac, tag, 16) != 0) { mismatches++; kat_fail++; }

        // and the decrypt direction, tag-verified
        aes_gcm_ctx_t d;
        uint8_t back[60];
        aes_gcm_init(&d, key, 128, iv, 12);
        aes_gcm_aad(&d, aad, 20);
        aes_gcm_decrypt(&d, ct, back, 60);
        vectors += 2;
        if (memcmp(back, pt, 60) != 0) { mismatches++; kat_fail++; }
        if (aes_gcm_verify(&d, tag, 16) != 0) { mismatches++; kat_fail++; }

        // --- NEGATIVE ORACLE. GHASH IS the AES-GCM authenticator, so a port
        // that only ever ACCEPTS the right tag is not proven: a GHASH stuck at
        // a constant would pass every positive vector above. These two cases
        // must REFUSE, and they are counted as failures if they do not. This is
        // the tamper check for the code this change actually touches. ---
        uint8_t bad[16];
        memcpy(bad, tag, 16);
        bad[7] ^= 0x01;                        // one bit of the tag
        aes_gcm_ctx_t n1;
        aes_gcm_init(&n1, key, 128, iv, 12);
        aes_gcm_aad(&n1, aad, 20);
        aes_gcm_decrypt(&n1, ct, back, 60);
        vectors++;
        if (aes_gcm_verify(&n1, bad, 16) == 0) { mismatches++; kat_fail++; }

        uint8_t ctmod[60];
        memcpy(ctmod, ct, 60);
        ctmod[59] ^= 0x80;                     // one bit of the ciphertext tail
        aes_gcm_ctx_t n2;
        aes_gcm_init(&n2, key, 128, iv, 12);
        aes_gcm_aad(&n2, aad, 20);
        aes_gcm_decrypt(&n2, ctmod, back, 60);
        vectors++;
        if (aes_gcm_verify(&n2, tag, 16) == 0) { mismatches++; kat_fail++; }

        uint8_t aadmod[20];
        memcpy(aadmod, aad, 20);
        aadmod[19] ^= 0x40;                    // one bit of the AAD tail
        aes_gcm_ctx_t n3;
        aes_gcm_init(&n3, key, 128, iv, 12);
        aes_gcm_aad(&n3, aadmod, 20);
        aes_gcm_decrypt(&n3, ct, back, 60);
        vectors++;
        if (aes_gcm_verify(&n3, tag, 16) == 0) { mismatches++; kat_fail++; }
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] ghash: %u vectors, %u mismatches -> %s (GCM KAT %s)\n",
            vectors, mismatches, verdict, kat_fail ? "FAIL" : "OK");
    bootlog_write("[RUST-DIFF] ghash: %u vectors, %u mismatches -> %s (GCM KAT %s)",
                  vectors, mismatches, verdict, kat_fail ? "FAIL" : "OK");

    // --- Part 4: RDTSC benchmark of the leaf both ways. This is the whole point
    // of the change, so the number is in the boot log rather than in prose. ---
    {
        const int iters = 20000;
        uint8_t h[16], xc[16], xr[16];
        for (int i = 0; i < 16; i++) { h[i] = (uint8_t)(0x31 * i + 7); xc[i] = (uint8_t)(0x17 * i + 1); xr[i] = xc[i]; }
        ghash_key_t k;
        ghash_key_init_rs(&k, h);

        for (int i = 0; i < 200; i++) ghash_multiply_c(xc, h);
        for (int i = 0; i < 200; i++) ghash_mul_rs(&k, xr);

        uint64_t t0 = dp_tsc();
        for (int i = 0; i < iters; i++) ghash_multiply_c(xc, h);
        uint64_t t1 = dp_tsc();
        for (int i = 0; i < iters; i++) ghash_mul_rs(&k, xr);
        uint64_t t2 = dp_tsc();

        uint64_t c_cyc = (t1 - t0) / (uint64_t)iters;
        uint64_t r_cyc = (t2 - t1) / (uint64_t)iters;
        uint64_t speedup100 = (r_cyc != 0) ? (c_cyc * 100ULL / r_cyc) : 0;
        kprintf("[RUST-PERF] ghash: C=%llu cyc/block RS=%llu cyc/block "
                "speedup=%llu.%02llux (sink=%02x%02x)\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(speedup100 / 100),
                (unsigned long long)(speedup100 % 100), xc[0], xr[0]);
        bootlog_write("[RUST-PERF] ghash: C=%llu cyc/block RS=%llu cyc/block speedup=%llu.%02llux",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(speedup100 / 100),
                      (unsigned long long)(speedup100 % 100));
    }
}
