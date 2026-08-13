// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sha256.c - SHA-256 for MayteraOS userland (#559).
// Ported verbatim in behaviour from kernel crypto/sha256.c (FIPS 180-4).
// See sha256.h for why this port exists rather than a syscall.

#include "sha256.h"
#include "string.h"

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

#define ROTR(x, n)  (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)  (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x)  (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        W[i] = ((uint32_t)block[i * 4]     << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] <<  8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++)
        W[i] = SIG1(W[i - 2]) + W[i - 7] + SIG0(W[i - 15]) + W[i - 16];

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + K[i] + W[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256_init(sha256_ctx_t *ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

void sha256_update(sha256_ctx_t *ctx, const void *data, size_t length) {
    const uint8_t *p = (const uint8_t *)data;
    size_t buffer_space, buffer_used;

    if (length == 0) return;

    buffer_used  = (size_t)((ctx->count >> 3) & 63);
    buffer_space = 64 - buffer_used;

    ctx->count += (uint64_t)length << 3;

    if (buffer_used && length >= buffer_space) {
        memcpy(ctx->buffer + buffer_used, p, buffer_space);
        sha256_transform(ctx->state, ctx->buffer);
        p += buffer_space;
        length -= buffer_space;
        buffer_used = 0;
    }

    while (length >= 64) {
        sha256_transform(ctx->state, p);
        p += 64;
        length -= 64;
    }

    if (length > 0)
        memcpy(ctx->buffer + buffer_used, p, length);
}

void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32]) {
    uint8_t padding[64];
    uint64_t bits = ctx->count;
    size_t pad_len;
    size_t buffer_used = (size_t)((ctx->count >> 3) & 63);

    pad_len = (buffer_used < 56) ? (56 - buffer_used) : (120 - buffer_used);

    memset(padding, 0, sizeof(padding));
    padding[0] = 0x80;
    sha256_update(ctx, padding, pad_len);

    padding[0] = (uint8_t)(bits >> 56); padding[1] = (uint8_t)(bits >> 48);
    padding[2] = (uint8_t)(bits >> 40); padding[3] = (uint8_t)(bits >> 32);
    padding[4] = (uint8_t)(bits >> 24); padding[5] = (uint8_t)(bits >> 16);
    padding[6] = (uint8_t)(bits >>  8); padding[7] = (uint8_t)(bits);
    sha256_update(ctx, padding, 8);

    for (int i = 0; i < 8; i++) {
        digest[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >>  8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }

    memset(ctx, 0, sizeof(*ctx));
}

void sha256(const void *data, size_t length, uint8_t digest[32]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, length);
    sha256_final(&ctx, digest);
}

// ---- Known-answer self-test (FIPS 180-4 published vectors) ----------------
// Independent oracle, not a differential against our own second copy.
int libc_sha256_selftest(void) {
    // "abc"
    static const uint8_t want_abc[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    // "" (empty)
    static const uint8_t want_empty[32] = {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55
    };
    // 448-bit two-block message: "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
    static const char msg2[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const uint8_t want_msg2[32] = {
        0x24,0x8d,0x6a,0x61,0xd2,0x06,0x38,0xb8,0xe5,0xc0,0x26,0x93,0x0c,0x3e,0x60,0x39,
        0xa3,0x3c,0xe4,0x59,0x64,0xff,0x21,0x67,0xf6,0xec,0xed,0xd4,0x19,0xdb,0x06,0xc1
    };

    uint8_t d[32];

    sha256("abc", 3, d);
    if (memcmp(d, want_abc, 32) != 0) return -1;

    sha256("", 0, d);
    if (memcmp(d, want_empty, 32) != 0) return -1;

    sha256(msg2, sizeof(msg2) - 1, d);
    if (memcmp(d, want_msg2, 32) != 0) return -1;

    // Multi-block streaming path: 1,000,000 'a' would be slow here; use a
    // 200-block streamed update instead to exercise buffer_used != 0 and the
    // whole-block loop together, cross-checked against the one-shot API.
    {
        static uint8_t big[12800];
        uint8_t one[32], streamed[32];
        sha256_ctx_t c;
        memset(big, 'a', sizeof(big));
        sha256(big, sizeof(big), one);
        sha256_init(&c);
        // deliberately ragged chunk sizes to hit the partial-buffer branches
        size_t off = 0; size_t step = 1;
        while (off < sizeof(big)) {
            size_t n = step; if (off + n > sizeof(big)) n = sizeof(big) - off;
            sha256_update(&c, big + off, n);
            off += n; step = (step * 7 + 13) % 300 + 1;
        }
        sha256_final(&c, streamed);
        if (memcmp(one, streamed, 32) != 0) return -1;
    }

    return 0;
}
