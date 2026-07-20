// chacha20.c - ChaCha20-Poly1305 AEAD implementation for MayteraOS
// RFC 8439 compliant implementation

#include "chacha20.h"
#include "crypto.h"
#include "../string.h"

// =============================================================================
// ChaCha20 Core
// =============================================================================

// ChaCha20 quarter round
#define QUARTERROUND(a, b, c, d) \
    a += b; d ^= a; d = (d << 16) | (d >> 16); \
    c += d; b ^= c; b = (b << 12) | (b >> 20); \
    a += b; d ^= a; d = (d << 8) | (d >> 24); \
    c += d; b ^= c; b = (b << 7) | (b >> 25);

// Load 32-bit word (little-endian)
static uint32_t load32_le(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Store 32-bit word (little-endian)
static void store32_le(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

// #404 / #491 Phase I: ChaCha20 block core ported to Rust.
//
// chacha20_block_rs (rustkern.rs) is a faithful, memory-safe drop-in for the
// pure 20-round block function below. Like the SHA/MD ports it takes ONLY the
// 16-word input state and writes the 64-byte serialized keystream block, so the
// chacha20_ctx_t struct never crosses the FFI boundary (init/setkey/counter/
// XOR-stream stay C and pass ctx->state). The live block-core dispatcher routes
// to it under -DRUST_CHACHA20 (Makefile CFLAGS strangler flag); with the flag
// undefined it falls straight back to chacha20_block_c. A boot-time differential
// self-test (chacha20_rust_selftest below) proves chacha20_block_rs ==
// chacha20_block_c on THIS build, re-runs the RFC 8439 section 2.3.2 known-answer
// keystream vector through the live chacha20 API, and RDTSC-micro-benchmarks the
// two, before any consumer (TLS ChaCha20-Poly1305) uses it.
extern void chacha20_block_rs(const uint32_t input[16], uint8_t out[64]);

// Original C implementation, renamed chacha20_block_c so BOTH the C and the Rust
// path stay in the image (differential reference + trivial rollback). Combines
// the former chacha20_block_internal 20-round core with the little-endian
// serialization to 64 bytes, so it operates on the 16-word state directly (not
// ctx) and the leaf is struct-free across FFI.
static void chacha20_block_c(const uint32_t input[16], uint8_t out[64]) {
    uint32_t output[16];

    // Copy input to output
    for (int i = 0; i < 16; i++) {
        output[i] = input[i];
    }

    // 20 rounds (10 double rounds)
    for (int i = 0; i < 10; i++) {
        // Column rounds
        QUARTERROUND(output[0], output[4], output[8],  output[12]);
        QUARTERROUND(output[1], output[5], output[9],  output[13]);
        QUARTERROUND(output[2], output[6], output[10], output[14]);
        QUARTERROUND(output[3], output[7], output[11], output[15]);

        // Diagonal rounds
        QUARTERROUND(output[0], output[5], output[10], output[15]);
        QUARTERROUND(output[1], output[6], output[11], output[12]);
        QUARTERROUND(output[2], output[7], output[8],  output[13]);
        QUARTERROUND(output[3], output[4], output[9],  output[14]);
    }

    // Add original input
    for (int i = 0; i < 16; i++) {
        output[i] += input[i];
    }

    // Serialize output (little-endian)
    for (int i = 0; i < 16; i++) {
        store32_le(out + i * 4, output[i]);
    }
}

// Live block-core dispatcher. Callers pass ctx->state and a 64-byte keystream
// buffer. Under -DRUST_CHACHA20 the block runs in Rust (chacha20_block_rs);
// otherwise it runs in C. Same 16-word-in / 64-byte-out contract either way.
static inline void chacha20_block_core(const uint32_t input[16], uint8_t out[64]) {
#ifdef RUST_CHACHA20
    chacha20_block_rs(input, out);
#else
    chacha20_block_c(input, out);
#endif
}

// Initialize ChaCha20 context
void chacha20_init(chacha20_ctx_t *ctx, const uint8_t key[32],
                   const uint8_t nonce[12], uint32_t counter) {
    // "expand 32-byte k" constant
    ctx->state[0] = 0x61707865;
    ctx->state[1] = 0x3320646e;
    ctx->state[2] = 0x79622d32;
    ctx->state[3] = 0x6b206574;
    
    // Key (8 words)
    for (int i = 0; i < 8; i++) {
        ctx->state[4 + i] = load32_le(key + i * 4);
    }
    
    // Counter
    ctx->state[12] = counter;
    
    // Nonce (3 words)
    ctx->state[13] = load32_le(nonce);
    ctx->state[14] = load32_le(nonce + 4);
    ctx->state[15] = load32_le(nonce + 8);
    
    ctx->counter = counter;
    ctx->keystream_pos = 64;  // Force keystream generation on first use
}

// Generate keystream block
void chacha20_block(chacha20_ctx_t *ctx, uint8_t output[64]) {
    // 20-round core + little-endian serialize (C or Rust per -DRUST_CHACHA20).
    chacha20_block_core(ctx->state, output);

    // Increment counter
    ctx->state[12]++;
}

// Encrypt/decrypt data
void chacha20_crypt(chacha20_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t len) {
    while (len > 0) {
        // Generate new keystream if needed
        if (ctx->keystream_pos >= 64) {
            chacha20_block(ctx, ctx->keystream);
            ctx->keystream_pos = 0;
        }
        
        // XOR with keystream
        size_t available = 64 - ctx->keystream_pos;
        size_t use = (len < available) ? len : available;
        
        for (size_t i = 0; i < use; i++) {
            out[i] = in[i] ^ ctx->keystream[ctx->keystream_pos + i];
        }
        
        ctx->keystream_pos += use;
        in += use;
        out += use;
        len -= use;
    }
}

// =============================================================================
// Poly1305 MAC
// =============================================================================

// Poly1305 uses 130-bit arithmetic
// We use 5 26-bit limbs for the accumulator

// Clamp key r according to RFC 8439
static void poly1305_clamp(uint8_t r[16]) {
    r[3] &= 0x0f;
    r[7] &= 0x0f;
    r[11] &= 0x0f;
    r[15] &= 0x0f;
    r[4] &= 0xfc;
    r[8] &= 0xfc;
    r[12] &= 0xfc;
}

void poly1305_init(poly1305_ctx_t *ctx, const uint8_t key[32]) {
    uint8_t r[16];
    memcpy(r, key, 16);
    poly1305_clamp(r);
    
    // Load r into 26-bit limbs
    ctx->r[0] = (load32_le(r + 0)) & 0x3ffffff;
    ctx->r[1] = (load32_le(r + 3) >> 2) & 0x3ffffff;
    ctx->r[2] = (load32_le(r + 6) >> 4) & 0x3ffffff;
    ctx->r[3] = (load32_le(r + 9) >> 6) & 0x3ffffff;
    ctx->r[4] = (load32_le(r + 12) >> 8) & 0x0fffff;
    
    // Initialize accumulator
    ctx->h[0] = 0;
    ctx->h[1] = 0;
    ctx->h[2] = 0;
    ctx->h[3] = 0;
    ctx->h[4] = 0;
    
    // Load pad (second half of key)
    ctx->pad[0] = load32_le(key + 16);
    ctx->pad[1] = load32_le(key + 20);
    ctx->pad[2] = load32_le(key + 24);
    ctx->pad[3] = load32_le(key + 28);
    
    ctx->buffer_len = 0;
    ctx->total_len = 0;
}

// Process a 16-byte block
static void poly1305_block(poly1305_ctx_t *ctx, const uint8_t block[16], int final) {
    // Load block into 26-bit limbs
    uint32_t h0 = ctx->h[0] + ((load32_le(block + 0)) & 0x3ffffff);
    uint32_t h1 = ctx->h[1] + ((load32_le(block + 3) >> 2) & 0x3ffffff);
    uint32_t h2 = ctx->h[2] + ((load32_le(block + 6) >> 4) & 0x3ffffff);
    uint32_t h3 = ctx->h[3] + ((load32_le(block + 9) >> 6) & 0x3ffffff);
    uint32_t h4 = ctx->h[4] + ((load32_le(block + 12) >> 8) | (final ? 0 : (1 << 24)));
    
    // Multiply h by r
    uint64_t r0 = ctx->r[0];
    uint64_t r1 = ctx->r[1];
    uint64_t r2 = ctx->r[2];
    uint64_t r3 = ctx->r[3];
    uint64_t r4 = ctx->r[4];
    
    uint64_t s1 = r1 * 5;
    uint64_t s2 = r2 * 5;
    uint64_t s3 = r3 * 5;
    uint64_t s4 = r4 * 5;
    
    uint64_t d0 = h0*r0 + h1*s4 + h2*s3 + h3*s2 + h4*s1;
    uint64_t d1 = h0*r1 + h1*r0 + h2*s4 + h3*s3 + h4*s2;
    uint64_t d2 = h0*r2 + h1*r1 + h2*r0 + h3*s4 + h4*s3;
    uint64_t d3 = h0*r3 + h1*r2 + h2*r1 + h3*r0 + h4*s4;
    uint64_t d4 = h0*r4 + h1*r3 + h2*r2 + h3*r1 + h4*r0;
    
    // Carry propagation
    uint32_t c;
    c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff; d1 += c;
    c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff; d2 += c;
    c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff; d3 += c;
    c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff; d4 += c;
    c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    
    ctx->h[0] = h0;
    ctx->h[1] = h1;
    ctx->h[2] = h2;
    ctx->h[3] = h3;
    ctx->h[4] = h4;
}

void poly1305_update(poly1305_ctx_t *ctx, const uint8_t *data, size_t len) {
    ctx->total_len += len;
    
    // Handle buffered data
    if (ctx->buffer_len > 0) {
        size_t need = 16 - ctx->buffer_len;
        if (len < need) {
            memcpy(ctx->buffer + ctx->buffer_len, data, len);
            ctx->buffer_len += len;
            return;
        }
        
        memcpy(ctx->buffer + ctx->buffer_len, data, need);
        poly1305_block(ctx, ctx->buffer, 0);
        data += need;
        len -= need;
        ctx->buffer_len = 0;
    }
    
    // Process full blocks
    while (len >= 16) {
        poly1305_block(ctx, data, 0);
        data += 16;
        len -= 16;
    }
    
    // Buffer remaining data
    if (len > 0) {
        memcpy(ctx->buffer, data, len);
        ctx->buffer_len = len;
    }
}

void poly1305_final(poly1305_ctx_t *ctx, uint8_t tag[16]) {
    // Process final partial block
    if (ctx->buffer_len > 0) {
        ctx->buffer[ctx->buffer_len] = 0x01;
        memset(ctx->buffer + ctx->buffer_len + 1, 0, 16 - ctx->buffer_len - 1);
        poly1305_block(ctx, ctx->buffer, 1);
    }
    
    // Fully reduce h
    uint32_t h0 = ctx->h[0];
    uint32_t h1 = ctx->h[1];
    uint32_t h2 = ctx->h[2];
    uint32_t h3 = ctx->h[3];
    uint32_t h4 = ctx->h[4];
    
    uint32_t c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    
    // Compute h - p
    uint32_t g0 = h0 + 5;
    c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1 << 26);
    
    // Select h or h - p based on carry
    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;
    
    // h = h + pad
    uint64_t f;
    f = (uint64_t)h0 + ctx->pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + ctx->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + ctx->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + ctx->pad[3] + (f >> 32); h3 = (uint32_t)f;
    
    // Convert to bytes
    h0 = h0 | (h1 << 26);
    h1 = (h1 >> 6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 << 8);
    
    store32_le(tag, h0);
    store32_le(tag + 4, h1);
    store32_le(tag + 8, h2);
    store32_le(tag + 12, h3);
    
    // Clear sensitive data
    crypto_zero(ctx, sizeof(*ctx));
}

void poly1305(const uint8_t key[32], const uint8_t *msg, size_t msg_len, uint8_t tag[16]) {
    poly1305_ctx_t ctx;
    poly1305_init(&ctx, key);
    poly1305_update(&ctx, msg, msg_len);
    poly1305_final(&ctx, tag);
}

// =============================================================================
// ChaCha20-Poly1305 AEAD
// =============================================================================

void chacha20_poly1305_init(chacha20_poly1305_ctx_t *ctx,
                            const uint8_t key[32],
                            const uint8_t nonce[12]) {
    // Generate Poly1305 key from first ChaCha20 block
    uint8_t poly_key[64];
    chacha20_init(&ctx->chacha, key, nonce, 0);
    chacha20_block(&ctx->chacha, poly_key);
    
    poly1305_init(&ctx->poly, poly_key);
    crypto_zero(poly_key, sizeof(poly_key));
    
    // Reset ChaCha20 counter to 1 for encryption
    ctx->chacha.state[12] = 1;
    ctx->chacha.keystream_pos = 64;
    
    ctx->aad_len = 0;
    ctx->cipher_len = 0;
}

void chacha20_poly1305_aad(chacha20_poly1305_ctx_t *ctx,
                           const uint8_t *aad, size_t len) {
    poly1305_update(&ctx->poly, aad, len);
    ctx->aad_len += len;
}

// Pad to 16-byte boundary
static void pad16(poly1305_ctx_t *poly, size_t len) {
    if (len % 16 != 0) {
        static const uint8_t zeros[16] = {0};
        poly1305_update(poly, zeros, 16 - (len % 16));
    }
}

void chacha20_poly1305_encrypt(chacha20_poly1305_ctx_t *ctx,
                               const uint8_t *plaintext,
                               uint8_t *ciphertext,
                               size_t len) {
    // Pad AAD before first ciphertext
    if (ctx->cipher_len == 0 && ctx->aad_len > 0) {
        pad16(&ctx->poly, ctx->aad_len);
    }
    
    chacha20_crypt(&ctx->chacha, plaintext, ciphertext, len);
    poly1305_update(&ctx->poly, ciphertext, len);
    ctx->cipher_len += len;
}

void chacha20_poly1305_decrypt(chacha20_poly1305_ctx_t *ctx,
                               const uint8_t *ciphertext,
                               uint8_t *plaintext,
                               size_t len) {
    // Pad AAD before first ciphertext
    if (ctx->cipher_len == 0 && ctx->aad_len > 0) {
        pad16(&ctx->poly, ctx->aad_len);
    }
    
    poly1305_update(&ctx->poly, ciphertext, len);
    chacha20_crypt(&ctx->chacha, ciphertext, plaintext, len);
    ctx->cipher_len += len;
}

void chacha20_poly1305_final(chacha20_poly1305_ctx_t *ctx, uint8_t tag[16]) {
    // Pad ciphertext
    pad16(&ctx->poly, ctx->cipher_len);
    
    // Append lengths
    uint8_t len_block[16];
    store32_le(len_block, (uint32_t)ctx->aad_len);
    store32_le(len_block + 4, (uint32_t)(ctx->aad_len >> 32));
    store32_le(len_block + 8, (uint32_t)ctx->cipher_len);
    store32_le(len_block + 12, (uint32_t)(ctx->cipher_len >> 32));
    poly1305_update(&ctx->poly, len_block, 16);
    
    poly1305_final(&ctx->poly, tag);
}

int chacha20_poly1305_verify(chacha20_poly1305_ctx_t *ctx, const uint8_t tag[16]) {
    uint8_t computed[16];
    chacha20_poly1305_final(ctx, computed);
    
    int result = crypto_memcmp(computed, tag, 16);
    crypto_zero(computed, 16);
    
    return result;
}

// One-shot seal
int chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *plaintext, size_t plaintext_len,
                           uint8_t *ciphertext) {
    chacha20_poly1305_ctx_t ctx;
    chacha20_poly1305_init(&ctx, key, nonce);
    
    if (aad && aad_len > 0) {
        chacha20_poly1305_aad(&ctx, aad, aad_len);
    }
    
    chacha20_poly1305_encrypt(&ctx, plaintext, ciphertext, plaintext_len);
    chacha20_poly1305_final(&ctx, ciphertext + plaintext_len);
    
    return 0;
}

// One-shot open
int chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *ciphertext, size_t ciphertext_len,
                           uint8_t *plaintext) {
    if (ciphertext_len < 16) return -1;
    
    size_t message_len = ciphertext_len - 16;
    const uint8_t *tag = ciphertext + message_len;
    
    chacha20_poly1305_ctx_t ctx;
    chacha20_poly1305_init(&ctx, key, nonce);
    
    if (aad && aad_len > 0) {
        chacha20_poly1305_aad(&ctx, aad, aad_len);
    }
    
    chacha20_poly1305_decrypt(&ctx, ciphertext, plaintext, message_len);
    
    if (chacha20_poly1305_verify(&ctx, tag) != 0) {
        crypto_zero(plaintext, message_len);
        return -1;
    }

    return 0;
}

// =============================================================================
// Boot-time differential self-test + RDTSC micro-benchmark (#404 / #491 Phase I).
//
// Three bounded, run-once proofs (no busy-wait / #426):
//   (1) The RFC 8439 section 2.3.2 known-answer keystream block hashed through
//       the LIVE chacha20_init/chacha20_block path, which under -DRUST_CHACHA20
//       generates the block via chacha20_block_rs. Proves the Rust block core is
//       correct end-to-end in the real cipher path, not just against the C twin.
//       Key = 00 01 .. 1f, nonce = 00 00 00 09 00 00 00 4a 00 00 00 00, block
//       counter = 1; the keystream starts 10 f1 e7 e4 d1 3b 59 15 50 0f dd 1f
//       a3 20 71 c4 (RFC 8439 section 2.3.2 serialized block).
//   (2) A direct differential of chacha20_block_rs vs chacha20_block_c over a
//       large corpus of random 16-word input states. This is why the block core
//       is a byte-identical drop-in and not merely KAT-correct.
//   (3) An RDTSC micro-benchmark of both block cores over a fixed state.
// One [RUST-DIFF] chacha20 line + one [RUST-PERF] chacha20 line are logged to
// serial (kprintf) and /BOOTLOG.TXT (bootlog_write). Any mismatch => M > 0 =>
// FAIL (the orchestrator leaves RUST_CHACHA20 OFF). chacha20_block_rs is
// referenced here so its Rust archive member is always pulled into the link
// regardless of the flag.

static uint64_t cc20diff_rng(uint64_t *s) {
    // xorshift64*: deterministic, decent spread for test coverage.
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

// Serialized TSC read: cpuid drains the pipeline so prior work retires before the
// rdtsc, giving a stable start/end fence around the measured loop. cpuid/rdtsc are
// always available on x86-64 and are unaffected by -mno-sse (they are not SSE).
static inline uint64_t cc20_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

void chacha20_rust_selftest(void) {
    extern int kprintf(const char *fmt, ...);
    extern void bootlog_write(const char *fmt, ...);

    uint32_t vectors = 0;
    uint32_t mismatches = 0;
    int kat_fail = 0;

    // --- Part 1: RFC 8439 section 2.3.2 known-answer block via the LIVE path. ---
    {
        uint8_t key[32];
        for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
        static const uint8_t nonce[12] = { 0,0,0,9, 0,0,0,0x4a, 0,0,0,0 };
        static const uint8_t expect[16] = {
            0x10,0xf1,0xe7,0xe4, 0xd1,0x3b,0x59,0x15,
            0x50,0x0f,0xdd,0x1f, 0xa3,0x20,0x71,0xc4,
        };
        chacha20_ctx_t ctx;
        chacha20_init(&ctx, key, nonce, 1); // block counter = 1
        uint8_t ks[64];
        chacha20_block(&ctx, ks);
        vectors++;
        for (int i = 0; i < 16; i++) {
            if (ks[i] != expect[i]) { mismatches++; kat_fail++; break; }
        }
    }

    // --- Part 2: direct chacha20_block_rs vs chacha20_block_c differential over
    // random 16-word input states. Bounded loop, deterministic PRNG. Compares the
    // full 64-byte serialized keystream block. ---
    uint64_t seed = 0x4343320000000000ULL; // 'CC2\0...'
    for (int n = 0; n < 20000; n++) {
        uint32_t st0[16];
        for (int i = 0; i < 16; i++) st0[i] = (uint32_t)cc20diff_rng(&seed);

        uint8_t oc[64], orr[64];
        chacha20_block_c(st0, oc);
        chacha20_block_rs(st0, orr);
        vectors++;
        for (int i = 0; i < 64; i++) {
            if (oc[i] != orr[i]) { mismatches++; break; }
        }
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] chacha20: %u vectors, %u mismatches -> %s (KAT %s)\n",
            vectors, mismatches, verdict, kat_fail ? "FAIL" : "OK");
    bootlog_write("[RUST-DIFF] chacha20: %u vectors, %u mismatches -> %s (KAT %s)",
                  vectors, mismatches, verdict, kat_fail ? "FAIL" : "OK");

    // --- Part 3: RDTSC micro-benchmark, C vs Rust block core over a fixed state.
    // ~100k iterations each; the produced block's first word is fed back into the
    // input state every iteration so the compiler cannot elide the loop, and its
    // final value is folded into the log so the result is observably used. ---
    {
        const int iters = 100000;
        uint32_t stc[16], str[16];
        for (int i = 0; i < 16; i++) {
            stc[i] = 0x01234567u ^ ((uint32_t)i << 8);
            str[i] = stc[i];
        }
        uint8_t oc[64], orr[64];

        // Warm the I-cache/branch predictors for both paths (untimed).
        for (int i = 0; i < 200; i++) chacha20_block_c(stc, oc);
        for (int i = 0; i < 200; i++) chacha20_block_rs(str, orr);

        uint64_t t0 = cc20_tsc_serialized();
        for (int i = 0; i < iters; i++) {
            chacha20_block_c(stc, oc);
            stc[0] ^= (uint32_t)oc[0];
        }
        uint64_t t1 = cc20_tsc_serialized();
        for (int i = 0; i < iters; i++) {
            chacha20_block_rs(str, orr);
            str[0] ^= (uint32_t)orr[0];
        }
        uint64_t t2 = cc20_tsc_serialized();

        uint64_t c_cyc = (t1 - t0) / (uint64_t)iters;
        uint64_t r_cyc = (t2 - t1) / (uint64_t)iters;
        // ratio = RS / C, fixed-point x100 (integer-only, no FP on -mno-sse).
        uint64_t ratio100 = (c_cyc != 0) ? (r_cyc * 100ULL / c_cyc) : 0;
        // Fold a state word into the log so the benchmark work is observably used.
        uint64_t sink = (uint64_t)(stc[0] ^ str[0]);

        kprintf("[RUST-PERF] chacha20: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu (sink=%llx)\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100),
                (unsigned long long)sink);
        bootlog_write("[RUST-PERF] chacha20: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}
