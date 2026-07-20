// aes.c - AES implementation for MayteraOS
// Based on FIPS 197

#include "crypto.h"
#include "../string.h"

// AES S-box
static const uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5,
    0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
    0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a,
    0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b,
    0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85,
    0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17,
    0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9,
    0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
    0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94,
    0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68,
    0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

// Inverse S-box
static const uint8_t inv_sbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38,
    0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87,
    0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d,
    0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2,
    0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16,
    0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda,
    0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a,
    0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02,
    0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea,
    0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85,
    0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89,
    0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20,
    0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31,
    0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d,
    0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0,
    0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26,
    0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d
};

// Round constants
static const uint8_t rcon[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

// Multiply by 2 in GF(2^8)
static inline uint8_t xtime(uint8_t x) {
    return ((x << 1) ^ (((x >> 7) & 1) * 0x1b));
}

// Multiply in GF(2^8)
static inline uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

// Key expansion
static void aes_key_expansion(aes_ctx_t *ctx, const uint8_t *key, int nk) {
    int i;
    uint8_t temp[4];
    int nr = ctx->nr;

    // First nk words are the key itself
    for (i = 0; i < nk; i++) {
        ctx->rk[i] = ((uint32_t)key[4*i] << 24) |
                     ((uint32_t)key[4*i+1] << 16) |
                     ((uint32_t)key[4*i+2] << 8) |
                     ((uint32_t)key[4*i+3]);
    }

    // Generate remaining round keys
    for (i = nk; i < 4 * (nr + 1); i++) {
        uint32_t w = ctx->rk[i - 1];

        if (i % nk == 0) {
            // RotWord + SubWord + Rcon
            temp[0] = sbox[(w >> 16) & 0xff] ^ rcon[i / nk];
            temp[1] = sbox[(w >> 8) & 0xff];
            temp[2] = sbox[w & 0xff];
            temp[3] = sbox[(w >> 24) & 0xff];
            w = ((uint32_t)temp[0] << 24) | ((uint32_t)temp[1] << 16) |
                ((uint32_t)temp[2] << 8) | temp[3];
        } else if (nk > 6 && i % nk == 4) {
            // Extra SubWord for AES-256
            temp[0] = sbox[(w >> 24) & 0xff];
            temp[1] = sbox[(w >> 16) & 0xff];
            temp[2] = sbox[(w >> 8) & 0xff];
            temp[3] = sbox[w & 0xff];
            w = ((uint32_t)temp[0] << 24) | ((uint32_t)temp[1] << 16) |
                ((uint32_t)temp[2] << 8) | temp[3];
        }

        ctx->rk[i] = ctx->rk[i - nk] ^ w;
    }
}

// Set encryption key
int aes_set_encrypt_key(aes_ctx_t *ctx, const uint8_t *key, int key_bits) {
    int nk;

    switch (key_bits) {
        case 128: ctx->nr = 10; nk = 4; break;
        case 192: ctx->nr = 12; nk = 6; break;
        case 256: ctx->nr = 14; nk = 8; break;
        default: return -1;
    }

    aes_key_expansion(ctx, key, nk);
    return 0;
}

// Set decryption key (same as encryption key for our implementation)
int aes_set_decrypt_key(aes_ctx_t *ctx, const uint8_t *key, int key_bits) {
    return aes_set_encrypt_key(ctx, key, key_bits);
}

// Add round key
static void add_round_key(uint8_t state[16], const uint32_t *rk) {
    for (int i = 0; i < 4; i++) {
        uint32_t k = rk[i];
        state[4*i]     ^= (k >> 24) & 0xff;
        state[4*i + 1] ^= (k >> 16) & 0xff;
        state[4*i + 2] ^= (k >> 8) & 0xff;
        state[4*i + 3] ^= k & 0xff;
    }
}

// SubBytes
static void sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++) {
        state[i] = sbox[state[i]];
    }
}

// InvSubBytes
static void inv_sub_bytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++) {
        state[i] = inv_sbox[state[i]];
    }
}

// ShiftRows
static void shift_rows(uint8_t state[16]) {
    uint8_t temp;

    // Row 1: shift left by 1
    temp = state[1];
    state[1] = state[5];
    state[5] = state[9];
    state[9] = state[13];
    state[13] = temp;

    // Row 2: shift left by 2
    temp = state[2];
    state[2] = state[10];
    state[10] = temp;
    temp = state[6];
    state[6] = state[14];
    state[14] = temp;

    // Row 3: shift left by 3 (= shift right by 1)
    temp = state[15];
    state[15] = state[11];
    state[11] = state[7];
    state[7] = state[3];
    state[3] = temp;
}

// InvShiftRows
static void inv_shift_rows(uint8_t state[16]) {
    uint8_t temp;

    // Row 1: shift right by 1
    temp = state[13];
    state[13] = state[9];
    state[9] = state[5];
    state[5] = state[1];
    state[1] = temp;

    // Row 2: shift right by 2
    temp = state[2];
    state[2] = state[10];
    state[10] = temp;
    temp = state[6];
    state[6] = state[14];
    state[14] = temp;

    // Row 3: shift right by 3 (= shift left by 1)
    temp = state[3];
    state[3] = state[7];
    state[7] = state[11];
    state[11] = state[15];
    state[15] = temp;
}

// MixColumns
static void mix_columns(uint8_t state[16]) {
    for (int i = 0; i < 4; i++) {
        uint8_t a = state[4*i];
        uint8_t b = state[4*i + 1];
        uint8_t c = state[4*i + 2];
        uint8_t d = state[4*i + 3];

        state[4*i]     = xtime(a) ^ xtime(b) ^ b ^ c ^ d;
        state[4*i + 1] = a ^ xtime(b) ^ xtime(c) ^ c ^ d;
        state[4*i + 2] = a ^ b ^ xtime(c) ^ xtime(d) ^ d;
        state[4*i + 3] = xtime(a) ^ a ^ b ^ c ^ xtime(d);
    }
}

// InvMixColumns
static void inv_mix_columns(uint8_t state[16]) {
    for (int i = 0; i < 4; i++) {
        uint8_t a = state[4*i];
        uint8_t b = state[4*i + 1];
        uint8_t c = state[4*i + 2];
        uint8_t d = state[4*i + 3];

        state[4*i]     = gf_mul(a, 0x0e) ^ gf_mul(b, 0x0b) ^ gf_mul(c, 0x0d) ^ gf_mul(d, 0x09);
        state[4*i + 1] = gf_mul(a, 0x09) ^ gf_mul(b, 0x0e) ^ gf_mul(c, 0x0b) ^ gf_mul(d, 0x0d);
        state[4*i + 2] = gf_mul(a, 0x0d) ^ gf_mul(b, 0x09) ^ gf_mul(c, 0x0e) ^ gf_mul(d, 0x0b);
        state[4*i + 3] = gf_mul(a, 0x0b) ^ gf_mul(b, 0x0d) ^ gf_mul(c, 0x09) ^ gf_mul(d, 0x0e);
    }
}

// #404 / #492 Phase J: AES block encrypt/decrypt cores ported to Rust.
//
// aes_encrypt_block_rs / aes_decrypt_block_rs (rustkern.rs) are faithful,
// memory-safe drop-ins for the pure per-block cores below. Like the SHA/MD/
// ChaCha20 ports, ONLY flat data crosses FFI: the expanded round-key array
// (ctx->rk, big-endian uint32 words), the round count ctx->nr, and the 16-byte
// in/out blocks; the aes_ctx_t struct never crosses the boundary, so the key
// schedule (aes_set_encrypt_key) stays in C. The live aes_encrypt_block() /
// aes_decrypt_block() dispatchers below route to Rust under -DRUST_AES (Makefile
// CFLAGS strangler flag); with the flag undefined they fall straight back to the
// C cores (aes_encrypt_block_c / aes_decrypt_block_c, kept verbatim = differential
// reference + trivial rollback). A boot-time differential self-test
// (aes_rust_selftest below) proves rs == c on THIS build, re-runs the FIPS-197
// AES-128 (+ AES-256 Appendix-C) KAT through the live AES API, and RDTSC-micro-
// benchmarks the two, before any consumer (TLS AES-GCM, SSH aes-ctr) uses it.
extern void aes_encrypt_block_rs(const uint32_t *rk, int nr, const uint8_t in[16], uint8_t out[16]);
extern void aes_decrypt_block_rs(const uint32_t *rk, int nr, const uint8_t in[16], uint8_t out[16]);

// Encrypt single block (original C core, renamed aes_encrypt_block_c). Kept
// verbatim so both the C and Rust path stay in the image.
static void aes_encrypt_block_c(const aes_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[16];
    int nr = ctx->nr;

    memcpy(state, in, 16);

    // Initial round key addition
    add_round_key(state, &ctx->rk[0]);

    // Main rounds
    for (int round = 1; round < nr; round++) {
        sub_bytes(state);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, &ctx->rk[4 * round]);
    }

    // Final round (no MixColumns)
    sub_bytes(state);
    shift_rows(state);
    add_round_key(state, &ctx->rk[4 * nr]);

    memcpy(out, state, 16);
}

// Decrypt single block (original C core, renamed aes_decrypt_block_c). Kept
// verbatim so both the C and Rust path stay in the image.
static void aes_decrypt_block_c(const aes_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t state[16];
    int nr = ctx->nr;

    memcpy(state, in, 16);

    // Initial round key addition
    add_round_key(state, &ctx->rk[4 * nr]);

    // Main rounds (in reverse)
    for (int round = nr - 1; round > 0; round--) {
        inv_shift_rows(state);
        inv_sub_bytes(state);
        add_round_key(state, &ctx->rk[4 * round]);
        inv_mix_columns(state);
    }

    // Final round
    inv_shift_rows(state);
    inv_sub_bytes(state);
    add_round_key(state, &ctx->rk[0]);

    memcpy(out, state, 16);
}

// Live public block-core dispatchers (keep the crypto.h symbols
// aes_encrypt_block / aes_decrypt_block so every caller - AES-CBC, AES-GCM in
// crypto.c, SSH aes-ctr in net/ssh/* - is unchanged). Under -DRUST_AES the block
// runs in Rust; otherwise in C. Same (ctx, in[16], out[16]) contract either way;
// the Rust leaf takes ctx->rk + ctx->nr so the struct never crosses FFI.
void aes_encrypt_block(const aes_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]) {
#ifdef RUST_AES
    aes_encrypt_block_rs(ctx->rk, ctx->nr, in, out);
#else
    aes_encrypt_block_c(ctx, in, out);
#endif
}

void aes_decrypt_block(const aes_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]) {
#ifdef RUST_AES
    aes_decrypt_block_rs(ctx->rk, ctx->nr, in, out);
#else
    aes_decrypt_block_c(ctx, in, out);
#endif
}

// =============================================================================
// AES-CBC Mode
// =============================================================================

int aes_cbc_encrypt_init(aes_cbc_ctx_t *ctx, const uint8_t *key, int key_bits,
                         const uint8_t iv[16]) {
    int ret = aes_set_encrypt_key(&ctx->aes, key, key_bits);
    if (ret == 0) {
        memcpy(ctx->iv, iv, 16);
    }
    return ret;
}

int aes_cbc_decrypt_init(aes_cbc_ctx_t *ctx, const uint8_t *key, int key_bits,
                         const uint8_t iv[16]) {
    int ret = aes_set_decrypt_key(&ctx->aes, key, key_bits);
    if (ret == 0) {
        memcpy(ctx->iv, iv, 16);
    }
    return ret;
}

void aes_cbc_encrypt(aes_cbc_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t length) {
    uint8_t block[16];

    while (length >= 16) {
        // XOR with IV/previous ciphertext
        for (int i = 0; i < 16; i++) {
            block[i] = in[i] ^ ctx->iv[i];
        }

        aes_encrypt_block(&ctx->aes, block, out);

        // Update IV
        memcpy(ctx->iv, out, 16);

        in += 16;
        out += 16;
        length -= 16;
    }
}

void aes_cbc_decrypt(aes_cbc_ctx_t *ctx, const uint8_t *in, uint8_t *out, size_t length) {
    uint8_t block[16];
    uint8_t next_iv[16];

    while (length >= 16) {
        // Save ciphertext for next IV
        memcpy(next_iv, in, 16);

        aes_decrypt_block(&ctx->aes, in, block);

        // XOR with IV/previous ciphertext
        for (int i = 0; i < 16; i++) {
            out[i] = block[i] ^ ctx->iv[i];
        }

        // Update IV
        memcpy(ctx->iv, next_iv, 16);

        in += 16;
        out += 16;
        length -= 16;
    }
}

// =============================================================================
// Boot-time differential self-test + RDTSC micro-benchmark (#404 / #492 Phase J).
//
// Bounded, run-once proofs (no busy-wait / #426):
//   (1) FIPS-197 known-answer test through the LIVE AES API, which under
//       -DRUST_AES runs the block cores in Rust. AES-128 (FIPS-197 C.1):
//       key    = 000102030405060708090a0b0c0d0e0f
//       plain  = 00112233445566778899aabbccddeeff
//       cipher = 69c4e0d86a7b0430d8cdb78070b4c55a
//       plus the decrypt back to the plaintext. AND AES-256 (FIPS-197 C.3):
//       key    = 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
//       plain  = 00112233445566778899aabbccddeeff
//       cipher = 8ea2b7ca516745bfeafc49904b496089
//       plus its decrypt round-trip.
//   (2) A direct differential of aes_encrypt_block_rs vs aes_encrypt_block_c and
//       aes_decrypt_block_rs vs aes_decrypt_block_c over a large corpus of random
//       (16-byte state, round-key array, Nr in {10,12,14}) - the pure block-core
//       leaf, so this is why it is a byte-identical drop-in and not merely KAT-OK.
//   (3) An RDTSC micro-benchmark of both encrypt block cores over a fixed AES-128
//       schedule (~100k iters each).
// One [RUST-DIFF] aes line + one [RUST-PERF] aes line to serial (kprintf) and
// /BOOTLOG.TXT (bootlog_write). Any mismatch => M > 0 => FAIL (the orchestrator
// leaves RUST_AES OFF). aes_encrypt_block_rs / aes_decrypt_block_rs are referenced
// here so their Rust archive members are always pulled into the link regardless of
// the flag.

static uint64_t aesdiff_rng(uint64_t *s) {
    // xorshift64*: deterministic, decent spread for test coverage.
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

// Serialized TSC read: cpuid drains the pipeline so prior work retires before the
// rdtsc. cpuid/rdtsc are always available on x86-64 and unaffected by -mno-sse.
static inline uint64_t aes_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

// Parse a lowercase hex string into bytes (test helper; fixed-length vectors).
static void aes_hex2bytes(const char *hex, uint8_t *out, int nbytes) {
    for (int i = 0; i < nbytes; i++) {
        uint8_t hi = 0, lo = 0;
        char ch = hex[2 * i];
        char cl = hex[2 * i + 1];
        hi = (ch >= '0' && ch <= '9') ? (uint8_t)(ch - '0') : (uint8_t)(ch - 'a' + 10);
        lo = (cl >= '0' && cl <= '9') ? (uint8_t)(cl - '0') : (uint8_t)(cl - 'a' + 10);
        out[i] = (uint8_t)((hi << 4) | lo);
    }
}

void aes_rust_selftest(void) {
    extern int kprintf(const char *fmt, ...);
    extern void bootlog_write(const char *fmt, ...);

    uint32_t vectors = 0;
    uint32_t mismatches = 0;
    int kat_fail = 0;

    // --- Part 1: FIPS-197 KAT through the LIVE AES API (Rust under -DRUST_AES). ---
    {
        // AES-128 (FIPS-197 Appendix C.1).
        uint8_t key[16], pt[16], ct[16], enc[16], dec[16];
        aes_hex2bytes("000102030405060708090a0b0c0d0e0f", key, 16);
        aes_hex2bytes("00112233445566778899aabbccddeeff", pt, 16);
        aes_hex2bytes("69c4e0d86a7b0430d8cdb78070b4c55a", ct, 16);

        aes_ctx_t ctx;
        aes_set_encrypt_key(&ctx, key, 128);
        aes_encrypt_block(&ctx, pt, enc);
        vectors++;
        if (memcmp(enc, ct, 16) != 0) { mismatches++; kat_fail++; }
        aes_decrypt_block(&ctx, ct, dec);
        vectors++;
        if (memcmp(dec, pt, 16) != 0) { mismatches++; kat_fail++; }
    }
    {
        // AES-256 (FIPS-197 Appendix C.3).
        uint8_t key[32], pt[16], ct[16], enc[16], dec[16];
        aes_hex2bytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, 32);
        aes_hex2bytes("00112233445566778899aabbccddeeff", pt, 16);
        aes_hex2bytes("8ea2b7ca516745bfeafc49904b496089", ct, 16);

        aes_ctx_t ctx;
        aes_set_encrypt_key(&ctx, key, 256);
        aes_encrypt_block(&ctx, pt, enc);
        vectors++;
        if (memcmp(enc, ct, 16) != 0) { mismatches++; kat_fail++; }
        aes_decrypt_block(&ctx, ct, dec);
        vectors++;
        if (memcmp(dec, pt, 16) != 0) { mismatches++; kat_fail++; }
    }

    // --- Part 2: direct aes_*_block_rs vs aes_*_block_c differential over random
    // (state, round-keys, Nr). Bounded loop, deterministic PRNG. The block cores
    // are pure functions of (rk, nr, in), so fully-random round-key arrays are a
    // valid and broad test of the leaf independent of any real key schedule. ---
    uint64_t seed = 0x4145530000000000ULL; // 'AES\0...'
    static const int nr_tab[3] = { 10, 12, 14 };
    for (int n = 0; n < 20000; n++) {
        aes_ctx_t ctx;
        ctx.nr = nr_tab[(int)(aesdiff_rng(&seed) % 3)];
        // Fill only the words this Nr uses (4*(nr+1)); the rest are irrelevant.
        int nwords = 4 * (ctx.nr + 1);
        for (int i = 0; i < nwords; i++) ctx.rk[i] = (uint32_t)aesdiff_rng(&seed);

        uint8_t in[16];
        for (int i = 0; i < 16; i++) in[i] = (uint8_t)aesdiff_rng(&seed);

        uint8_t ec[16], er[16], dc[16], dr[16];
        aes_encrypt_block_c(&ctx, in, ec);
        aes_encrypt_block_rs(ctx.rk, ctx.nr, in, er);
        vectors++;
        if (memcmp(ec, er, 16) != 0) mismatches++;

        aes_decrypt_block_c(&ctx, in, dc);
        aes_decrypt_block_rs(ctx.rk, ctx.nr, in, dr);
        vectors++;
        if (memcmp(dc, dr, 16) != 0) mismatches++;
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] aes: %u vectors, %u mismatches -> %s (KAT %s)\n",
            vectors, mismatches, verdict, kat_fail ? "FAIL" : "OK");
    bootlog_write("[RUST-DIFF] aes: %u vectors, %u mismatches -> %s (KAT %s)",
                  vectors, mismatches, verdict, kat_fail ? "FAIL" : "OK");

    // --- Part 3: RDTSC micro-benchmark, C vs Rust encrypt block core over a fixed
    // AES-128 schedule. ~100k iterations each; the produced block's first byte is
    // fed back into the input every iteration so the loop cannot be elided, and its
    // final value is folded into the log so the result is observably used. ---
    {
        const int iters = 100000;
        uint8_t key[16];
        aes_hex2bytes("000102030405060708090a0b0c0d0e0f", key, 16);
        aes_ctx_t ctx;
        aes_set_encrypt_key(&ctx, key, 128);

        uint8_t inc[16], inr[16], oc[16], orr[16];
        for (int i = 0; i < 16; i++) { inc[i] = (uint8_t)(0x11 * i); inr[i] = inc[i]; }

        // Warm caches / predictors (untimed).
        for (int i = 0; i < 200; i++) aes_encrypt_block_c(&ctx, inc, oc);
        for (int i = 0; i < 200; i++) aes_encrypt_block_rs(ctx.rk, ctx.nr, inr, orr);

        uint64_t t0 = aes_tsc_serialized();
        for (int i = 0; i < iters; i++) {
            aes_encrypt_block_c(&ctx, inc, oc);
            inc[0] ^= oc[0];
        }
        uint64_t t1 = aes_tsc_serialized();
        for (int i = 0; i < iters; i++) {
            aes_encrypt_block_rs(ctx.rk, ctx.nr, inr, orr);
            inr[0] ^= orr[0];
        }
        uint64_t t2 = aes_tsc_serialized();

        uint64_t c_cyc = (t1 - t0) / (uint64_t)iters;
        uint64_t r_cyc = (t2 - t1) / (uint64_t)iters;
        uint64_t ratio100 = (c_cyc != 0) ? (r_cyc * 100ULL / c_cyc) : 0;
        uint64_t sink = (uint64_t)(inc[0] ^ inr[0]);

        kprintf("[RUST-PERF] aes: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu (sink=%llx)\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100),
                (unsigned long long)sink);
        bootlog_write("[RUST-PERF] aes: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}
