// sha256.c - SHA-256 implementation for MayteraOS
// Based on FIPS 180-4

#include "crypto.h"
#include "../string.h"

// SHA-256 constants (first 32 bits of fractional parts of cube roots of first 64 primes)
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

// Bit manipulation macros
#define ROTR(x, n)  (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)  (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x)  (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

// #404 / #487 Phase E: SHA-256 block compression core ported to Rust.
//
// sha256_transform_rs (rustkern.rs) is a faithful, memory-safe drop-in for the
// pure 64-round block-compression leaf below. It takes ONLY the 8-word state
// and a 64-byte block, so the sha256_ctx_t struct never crosses the FFI
// boundary (init/update/final stay C and just pass ctx->state). The live
// sha256_transform() dispatcher routes to it under -DRUST_SHA256 (Makefile
// CFLAGS strangler flag); with the flag undefined it falls straight back to
// sha256_transform_c. A boot-time differential self-test (sha256_rust_selftest
// below) proves sha256_transform_rs == sha256_transform_c on THIS build and
// re-runs the NIST known-answer vectors through the live sha256() API before
// any crypto (TLS/HMAC) uses it.
extern void sha256_transform_rs(uint32_t *state, const uint8_t *block);

// Original C implementation, renamed sha256_transform_c so BOTH the C and the
// Rust path stay in the image (differential reference + trivial rollback). Now
// operates on state[8] directly (not ctx) so the leaf is struct-free across FFI.
static void sha256_transform_c(uint32_t state[8], const uint8_t block[64]) {
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t t1, t2;
    int i;

    // Prepare message schedule
    for (i = 0; i < 16; i++) {
        W[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        W[i] = SIG1(W[i - 2]) + W[i - 7] + SIG0(W[i - 15]) + W[i - 16];
    }

    // Initialize working variables
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];

    // Main loop
    for (i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + K[i] + W[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    // Update state
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

// Live dispatcher. Callers pass ctx->state. Under -DRUST_SHA256 the compression
// runs in Rust (sha256_transform_rs); otherwise it runs in C. Same 8-word
// in/out contract either way.
static inline void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
#ifdef RUST_SHA256
    sha256_transform_rs(state, block);
#else
    sha256_transform_c(state, block);
#endif
}

// Initialize SHA-256 context
void sha256_init(sha256_ctx_t *ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

// Update hash with data
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t length) {
    const uint8_t *p = (const uint8_t *)data;
    size_t buffer_space;
    size_t buffer_used;

    if (length == 0) return;

    buffer_used = (ctx->count >> 3) & 63;
    buffer_space = 64 - buffer_used;

    ctx->count += length << 3;

    // If we have buffered data and new data fills the buffer
    if (buffer_used && length >= buffer_space) {
        memcpy(ctx->buffer + buffer_used, p, buffer_space);
        sha256_transform(ctx->state, ctx->buffer);
        p += buffer_space;
        length -= buffer_space;
        buffer_used = 0;
    }

    // Process complete blocks
    while (length >= 64) {
        sha256_transform(ctx->state, p);
        p += 64;
        length -= 64;
    }

    // Buffer remaining data
    if (length > 0) {
        memcpy(ctx->buffer + buffer_used, p, length);
    }
}

// Finalize and get digest
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32]) {
    uint8_t padding[64];
    uint64_t bits = ctx->count;
    size_t pad_len;
    size_t buffer_used = (ctx->count >> 3) & 63;

    // Padding: 1 bit, then zeros, then 64-bit length
    pad_len = (buffer_used < 56) ? (56 - buffer_used) : (120 - buffer_used);

    memset(padding, 0, sizeof(padding));
    padding[0] = 0x80;

    sha256_update(ctx, padding, pad_len);

    // Append length in bits (big-endian)
    padding[0] = (uint8_t)(bits >> 56);
    padding[1] = (uint8_t)(bits >> 48);
    padding[2] = (uint8_t)(bits >> 40);
    padding[3] = (uint8_t)(bits >> 32);
    padding[4] = (uint8_t)(bits >> 24);
    padding[5] = (uint8_t)(bits >> 16);
    padding[6] = (uint8_t)(bits >> 8);
    padding[7] = (uint8_t)(bits);

    sha256_update(ctx, padding, 8);

    // Output digest (big-endian)
    for (int i = 0; i < 8; i++) {
        digest[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }

    // Clear sensitive data
    crypto_zero(ctx, sizeof(*ctx));
}

// One-shot hash
void sha256(const void *data, size_t length, uint8_t digest[32]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, length);
    sha256_final(&ctx, digest);
}

// =============================================================================
// HMAC-SHA256
// =============================================================================

void hmac_sha256_init(hmac_sha256_ctx_t *ctx, const void *key, size_t key_len) {
    uint8_t key_hash[32];
    const uint8_t *key_ptr;
    uint8_t ipad[64];
    uint8_t opad[64];
    size_t i;

    // If key is longer than block size, hash it first
    if (key_len > 64) {
        sha256(key, key_len, key_hash);
        key_ptr = key_hash;
        key_len = 32;
    } else {
        key_ptr = (const uint8_t *)key;
    }

    // Prepare key block (pad with zeros)
    memset(ctx->key_block, 0, 64);
    memcpy(ctx->key_block, key_ptr, key_len);

    // Compute ipad and opad
    for (i = 0; i < 64; i++) {
        ipad[i] = ctx->key_block[i] ^ 0x36;
        opad[i] = ctx->key_block[i] ^ 0x5c;
    }

    // Initialize inner hash
    sha256_init(&ctx->inner);
    sha256_update(&ctx->inner, ipad, 64);

    // Store outer hash state
    sha256_init(&ctx->outer);
    sha256_update(&ctx->outer, opad, 64);

    crypto_zero(ipad, sizeof(ipad));
    crypto_zero(opad, sizeof(opad));
    crypto_zero(key_hash, sizeof(key_hash));
}

void hmac_sha256_update(hmac_sha256_ctx_t *ctx, const void *data, size_t length) {
    sha256_update(&ctx->inner, data, length);
}

void hmac_sha256_final(hmac_sha256_ctx_t *ctx, uint8_t mac[32]) {
    uint8_t inner_hash[32];

    // Finalize inner hash
    sha256_final(&ctx->inner, inner_hash);

    // Compute outer hash: H(opad || inner_hash)
    sha256_update(&ctx->outer, inner_hash, 32);
    sha256_final(&ctx->outer, mac);

    crypto_zero(inner_hash, sizeof(inner_hash));
    crypto_zero(ctx, sizeof(*ctx));
}

// HMAC-SHA256 one-shot, original C. Kept as hmac_sha256_c for the boot-time
// differential (hmac_rust_selftest in crypto/hmac.c) + trivial rollback.
void hmac_sha256_c(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 uint8_t mac[32]) {
    hmac_sha256_ctx_t ctx;
    hmac_sha256_init(&ctx, key, key_len);
    hmac_sha256_update(&ctx, data, data_len);
    hmac_sha256_final(&ctx, mac);
}

#ifdef RUST_HMAC
// #404 / #493 Phase K: Rust HMAC-SHA256 one-shot construction (rustkern.rs).
// Reaches the (already-Rust under -DRUST_SHA256) compression via the C sha256
// init/update/final glue; only the ipad/opad HMAC wrapper is the new Rust logic.
extern void hmac_sha256_rs(const void *key, size_t key_len,
                           const void *data, size_t data_len, uint8_t *mac);
#endif

// Live HMAC-SHA256 one-shot dispatcher. Under -DRUST_HMAC the construction runs
// in Rust (hmac_sha256_rs); otherwise in C (hmac_sha256_c). TLS 1.3 Finished key
// schedule + CSPRNG HMAC-DRBG one-shot call this symbol.
void hmac_sha256(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 uint8_t mac[32]) {
#ifdef RUST_HMAC
    hmac_sha256_rs(key, key_len, data, data_len, mac);
#else
    hmac_sha256_c(key, key_len, data, data_len, mac);
#endif
}

// =============================================================================
// Boot-time differential self-test (#404 / #487 Phase E).
//
// Two independent proofs, bounded, run once at boot (no busy-wait / #426):
//   (1) NIST known-answer vectors hashed through the LIVE sha256() one-shot API,
//       which now compresses via sha256_transform (Rust under -DRUST_SHA256).
//       This proves the Rust transform is correct end-to-end in the real crypto
//       path, not just against the C twin.
//   (2) A direct differential of sha256_transform_rs vs sha256_transform_c over
//       a large corpus of random (state[8], block[64]) pairs. This is why the
//       transform is a byte-identical drop-in and not just KAT-correct.
// One [RUST-DIFF] sha256 line is logged to serial (kprintf) + /BOOTLOG.TXT
// (bootlog_write). Any mismatch => M > 0 => FAIL (the orchestrator leaves
// RUST_SHA256 OFF). sha256_transform_rs is referenced here so its Rust archive
// member is always pulled into the link regardless of the strangler flag.
static inline uint32_t sha256diff_rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static int sha256diff_digcmp(const uint8_t *d, const char *hex) {
    // hex is 64 lowercase hex chars (32 bytes). Return 0 if equal.
    for (int i = 0; i < 32; i++) {
        uint8_t hi = (uint8_t)hex[i * 2];
        uint8_t lo = (uint8_t)hex[i * 2 + 1];
        hi = (hi >= 'a') ? (uint8_t)(hi - 'a' + 10) : (uint8_t)(hi - '0');
        lo = (lo >= 'a') ? (uint8_t)(lo - 'a' + 10) : (uint8_t)(lo - '0');
        if (d[i] != (uint8_t)((hi << 4) | lo)) return 1;
    }
    return 0;
}

void sha256_rust_selftest(void) {
    extern int kprintf(const char *fmt, ...);
    extern void bootlog_write(const char *fmt, ...);

    uint32_t vectors = 0;
    uint32_t mismatches = 0;

    // --- Part 1: NIST known-answer vectors through the LIVE sha256() API. ---
    struct { const char *msg; size_t len; const char *hex; } kat[3] = {
        { "abc", 3,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "", 0,
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
    };
    int kat_fail = 0;
    for (int k = 0; k < 3; k++) {
        uint8_t dig[32];
        sha256(kat[k].msg, kat[k].len, dig);
        vectors++;
        if (sha256diff_digcmp(dig, kat[k].hex) != 0) {
            mismatches++;
            kat_fail++;
        }
    }

    // --- Part 2: direct sha256_transform_rs vs sha256_transform_c differential
    // over random (state, block) pairs. Bounded loop, deterministic PRNG. ---
    uint32_t seed = 0x53484132; // 'SHA2'
    for (int n = 0; n < 20000; n++) {
        uint32_t st0[8];
        uint8_t blk[64];
        for (int i = 0; i < 8; i++) st0[i] = sha256diff_rng(&seed);
        for (int i = 0; i < 64; i++) blk[i] = (uint8_t)(sha256diff_rng(&seed) & 0xFF);

        uint32_t sc[8], sr[8];
        for (int i = 0; i < 8; i++) { sc[i] = st0[i]; sr[i] = st0[i]; }
        sha256_transform_c(sc, blk);
        sha256_transform_rs(sr, blk);
        vectors++;
        for (int i = 0; i < 8; i++) {
            if (sc[i] != sr[i]) { mismatches++; break; }
        }
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] sha256: %u vectors, %u mismatches -> %s (KAT %s)\n",
            vectors, mismatches, verdict, kat_fail ? "FAIL" : "OK");
    bootlog_write("[RUST-DIFF] sha256: %u vectors, %u mismatches -> %s (KAT %s)",
                  vectors, mismatches, verdict, kat_fail ? "FAIL" : "OK");
}
