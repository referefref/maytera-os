// md5.c - MD5 Hash Implementation
// Based on RFC 1321

#include "md5.h"
#include "../string.h"

// MD5 auxiliary functions
#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | (~z)))

// Left rotate
#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

// Round operations
#define FF(a, b, c, d, x, s, ac) { \
    (a) += F((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTL((a), (s)); \
    (a) += (b); \
}

#define GG(a, b, c, d, x, s, ac) { \
    (a) += G((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTL((a), (s)); \
    (a) += (b); \
}

#define HH(a, b, c, d, x, s, ac) { \
    (a) += H((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTL((a), (s)); \
    (a) += (b); \
}

#define II(a, b, c, d, x, s, ac) { \
    (a) += I((b), (c), (d)) + (x) + (uint32_t)(ac); \
    (a) = ROTL((a), (s)); \
    (a) += (b); \
}

// #404 / #489 Phase G: MD5 block-compression core ported to Rust.
//
// md5_transform_rs (rustkern.rs) is a faithful, memory-safe drop-in for the pure
// 64-round block-compression leaf below. Like the SHA-256/512 ports it takes
// ONLY the 4-word state and a 64-byte block, so the md5_ctx_t struct never
// crosses the FFI boundary (init/update/final stay C and pass ctx->state). The
// live md5_transform() dispatcher routes to it under -DRUST_MD5 (Makefile CFLAGS
// strangler flag); with the flag undefined it falls straight back to
// md5_transform_c. A boot-time differential self-test (md5_rust_selftest below)
// proves md5_transform_rs == md5_transform_c on THIS build, re-runs the RFC 1321
// known-answer vectors through the live md5() API, and RDTSC-micro-benchmarks the
// two, before any consumer (HMAC-MD5 / NTLM in net/smb.c, crypto/hmac.c) uses it.
// NOTE: MD5 decodes the message words LITTLE-endian (unlike the big-endian SHA
// schedule); the Rust port matches the C x[i] decode exactly.
extern void md5_transform_rs(uint32_t *state, const uint8_t *block);

// Original C implementation, renamed md5_transform_c so BOTH the C and the Rust
// path stay in the image (differential reference + trivial rollback). Already
// operates on state[4] directly (not ctx), so the leaf is struct-free across FFI.
static void md5_transform_c(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a, b, c, d;
    uint32_t x[16];

    // Copy block to x (little-endian)
    for (int i = 0; i < 16; i++) {
        x[i] = ((uint32_t)block[i * 4]) |
               ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) |
               ((uint32_t)block[i * 4 + 3] << 24);
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];

    // Round 1
    FF(a, b, c, d, x[ 0],  7, 0xd76aa478);
    FF(d, a, b, c, x[ 1], 12, 0xe8c7b756);
    FF(c, d, a, b, x[ 2], 17, 0x242070db);
    FF(b, c, d, a, x[ 3], 22, 0xc1bdceee);
    FF(a, b, c, d, x[ 4],  7, 0xf57c0faf);
    FF(d, a, b, c, x[ 5], 12, 0x4787c62a);
    FF(c, d, a, b, x[ 6], 17, 0xa8304613);
    FF(b, c, d, a, x[ 7], 22, 0xfd469501);
    FF(a, b, c, d, x[ 8],  7, 0x698098d8);
    FF(d, a, b, c, x[ 9], 12, 0x8b44f7af);
    FF(c, d, a, b, x[10], 17, 0xffff5bb1);
    FF(b, c, d, a, x[11], 22, 0x895cd7be);
    FF(a, b, c, d, x[12],  7, 0x6b901122);
    FF(d, a, b, c, x[13], 12, 0xfd987193);
    FF(c, d, a, b, x[14], 17, 0xa679438e);
    FF(b, c, d, a, x[15], 22, 0x49b40821);

    // Round 2
    GG(a, b, c, d, x[ 1],  5, 0xf61e2562);
    GG(d, a, b, c, x[ 6],  9, 0xc040b340);
    GG(c, d, a, b, x[11], 14, 0x265e5a51);
    GG(b, c, d, a, x[ 0], 20, 0xe9b6c7aa);
    GG(a, b, c, d, x[ 5],  5, 0xd62f105d);
    GG(d, a, b, c, x[10],  9, 0x02441453);
    GG(c, d, a, b, x[15], 14, 0xd8a1e681);
    GG(b, c, d, a, x[ 4], 20, 0xe7d3fbc8);
    GG(a, b, c, d, x[ 9],  5, 0x21e1cde6);
    GG(d, a, b, c, x[14],  9, 0xc33707d6);
    GG(c, d, a, b, x[ 3], 14, 0xf4d50d87);
    GG(b, c, d, a, x[ 8], 20, 0x455a14ed);
    GG(a, b, c, d, x[13],  5, 0xa9e3e905);
    GG(d, a, b, c, x[ 2],  9, 0xfcefa3f8);
    GG(c, d, a, b, x[ 7], 14, 0x676f02d9);
    GG(b, c, d, a, x[12], 20, 0x8d2a4c8a);

    // Round 3
    HH(a, b, c, d, x[ 5],  4, 0xfffa3942);
    HH(d, a, b, c, x[ 8], 11, 0x8771f681);
    HH(c, d, a, b, x[11], 16, 0x6d9d6122);
    HH(b, c, d, a, x[14], 23, 0xfde5380c);
    HH(a, b, c, d, x[ 1],  4, 0xa4beea44);
    HH(d, a, b, c, x[ 4], 11, 0x4bdecfa9);
    HH(c, d, a, b, x[ 7], 16, 0xf6bb4b60);
    HH(b, c, d, a, x[10], 23, 0xbebfbc70);
    HH(a, b, c, d, x[13],  4, 0x289b7ec6);
    HH(d, a, b, c, x[ 0], 11, 0xeaa127fa);
    HH(c, d, a, b, x[ 3], 16, 0xd4ef3085);
    HH(b, c, d, a, x[ 6], 23, 0x04881d05);
    HH(a, b, c, d, x[ 9],  4, 0xd9d4d039);
    HH(d, a, b, c, x[12], 11, 0xe6db99e5);
    HH(c, d, a, b, x[15], 16, 0x1fa27cf8);
    HH(b, c, d, a, x[ 2], 23, 0xc4ac5665);

    // Round 4
    II(a, b, c, d, x[ 0],  6, 0xf4292244);
    II(d, a, b, c, x[ 7], 10, 0x432aff97);
    II(c, d, a, b, x[14], 15, 0xab9423a7);
    II(b, c, d, a, x[ 5], 21, 0xfc93a039);
    II(a, b, c, d, x[12],  6, 0x655b59c3);
    II(d, a, b, c, x[ 3], 10, 0x8f0ccc92);
    II(c, d, a, b, x[10], 15, 0xffeff47d);
    II(b, c, d, a, x[ 1], 21, 0x85845dd1);
    II(a, b, c, d, x[ 8],  6, 0x6fa87e4f);
    II(d, a, b, c, x[15], 10, 0xfe2ce6e0);
    II(c, d, a, b, x[ 6], 15, 0xa3014314);
    II(b, c, d, a, x[13], 21, 0x4e0811a1);
    II(a, b, c, d, x[ 4],  6, 0xf7537e82);
    II(d, a, b, c, x[11], 10, 0xbd3af235);
    II(c, d, a, b, x[ 2], 15, 0x2ad7d2bb);
    II(b, c, d, a, x[ 9], 21, 0xeb86d391);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;

    // Clear sensitive data
    memset(x, 0, sizeof(x));
}

// Live dispatcher. Callers pass ctx->state. Under -DRUST_MD5 the compression
// runs in Rust (md5_transform_rs); otherwise it runs in C. Same 4-word
// (uint32_t) in/out contract either way.
static inline void md5_transform(uint32_t state[4], const uint8_t block[64]) {
#ifdef RUST_MD5
    md5_transform_rs(state, block);
#else
    md5_transform_c(state, block);
#endif
}

// Initialize MD5 context
void md5_init(md5_ctx_t *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->count = 0;
}

// Update hash with data
void md5_update(md5_ctx_t *ctx, const void *data, size_t length) {
    const uint8_t *input = (const uint8_t *)data;
    size_t index = (ctx->count / 8) % 64;

    ctx->count += (uint64_t)length * 8;

    size_t partial = 64 - index;
    size_t i = 0;

    if (length >= partial) {
        memcpy(&ctx->buffer[index], input, partial);
        md5_transform(ctx->state, ctx->buffer);

        for (i = partial; i + 63 < length; i += 64) {
            md5_transform(ctx->state, &input[i]);
        }

        index = 0;
    }

    memcpy(&ctx->buffer[index], &input[i], length - i);
}

// Finalize and get digest
void md5_final(md5_ctx_t *ctx, uint8_t digest[16]) {
    static const uint8_t padding[64] = { 0x80 };
    uint8_t bits[8];

    // Save bit count (little-endian)
    for (int i = 0; i < 8; i++) {
        bits[i] = (ctx->count >> (i * 8)) & 0xFF;
    }

    // Pad to 56 mod 64
    size_t index = (ctx->count / 8) % 64;
    size_t padlen = (index < 56) ? (56 - index) : (120 - index);
    md5_update(ctx, padding, padlen);

    // Append length
    md5_update(ctx, bits, 8);

    // Output digest (little-endian)
    for (int i = 0; i < 4; i++) {
        digest[i * 4]     = (ctx->state[i]) & 0xFF;
        digest[i * 4 + 1] = (ctx->state[i] >> 8) & 0xFF;
        digest[i * 4 + 2] = (ctx->state[i] >> 16) & 0xFF;
        digest[i * 4 + 3] = (ctx->state[i] >> 24) & 0xFF;
    }

    // Clear context
    memset(ctx, 0, sizeof(*ctx));
}

// One-shot hash
void md5(const void *data, size_t length, uint8_t digest[16]) {
    md5_ctx_t ctx;
    md5_init(&ctx);
    md5_update(&ctx, data, length);
    md5_final(&ctx, digest);
}

// =============================================================================
// Boot-time differential self-test + RDTSC micro-benchmark (#404 / #489 Phase G).
//
// Three bounded, run-once proofs (no busy-wait / #426):
//   (1) RFC 1321 known-answer vectors hashed through the LIVE md5 init/update/
//       final path, which under -DRUST_MD5 compresses via md5_transform_rs.
//       Proves the Rust transform is correct end-to-end in the real hash path,
//       not just against the C twin.
//   (2) A direct differential of md5_transform_rs vs md5_transform_c over a large
//       corpus of random (state[4], block[64]) pairs. This is why the transform
//       is a byte-identical drop-in and not merely KAT-correct.
//   (3) An RDTSC micro-benchmark of both transforms over a fixed block (parity is
//       the honest expectation for a 32-bit soft-ALU hash on this -mno-sse build).
// One [RUST-DIFF] md5 line + one [RUST-PERF] md5 line are logged to serial
// (kprintf) and /BOOTLOG.TXT (bootlog_write). Any mismatch => M > 0 => FAIL (the
// orchestrator leaves RUST_MD5 OFF). md5_transform_rs is referenced here so its
// Rust archive member is always pulled into the link regardless of the flag.

static uint64_t md5diff_rng(uint64_t *s) {
    // xorshift64*: deterministic, decent spread for test coverage.
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static int md5diff_digcmp(const uint8_t *d, const char *hex, int n) {
    // hex is 2*n lowercase hex chars. Return 0 if equal.
    for (int i = 0; i < n; i++) {
        uint8_t hi = (uint8_t)hex[i * 2];
        uint8_t lo = (uint8_t)hex[i * 2 + 1];
        hi = (hi >= 'a') ? (uint8_t)(hi - 'a' + 10) : (uint8_t)(hi - '0');
        lo = (lo >= 'a') ? (uint8_t)(lo - 'a' + 10) : (uint8_t)(lo - '0');
        if (d[i] != (uint8_t)((hi << 4) | lo)) return 1;
    }
    return 0;
}

// Serialized TSC read: cpuid drains the pipeline so prior work retires before the
// rdtsc, giving a stable start/end fence around the measured loop. cpuid/rdtsc are
// always available on x86-64 and are unaffected by -mno-sse (they are not SSE).
static inline uint64_t md5_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

void md5_rust_selftest(void) {
    extern int kprintf(const char *fmt, ...);
    extern void bootlog_write(const char *fmt, ...);

    uint32_t vectors = 0;
    uint32_t mismatches = 0;

    // --- Part 1: RFC 1321 known-answer vectors through the LIVE md5 path. ---
    struct { const char *msg; size_t len; const char *hex; } kat[3] = {
        { "", 0, "d41d8cd98f00b204e9800998ecf8427e" },
        { "abc", 3, "900150983cd24fb0d6963f7d28e17f72" },
        { "message digest", 14, "f96b697d7cb7938d525a2f31aaf161d0" },
    };
    int kat_fail = 0;
    for (int k = 0; k < 3; k++) {
        uint8_t dig[16];
        md5(kat[k].msg, kat[k].len, dig);
        vectors++;
        if (md5diff_digcmp(dig, kat[k].hex, 16) != 0) {
            mismatches++;
            kat_fail++;
        }
    }

    // --- Part 2: direct md5_transform_rs vs md5_transform_c differential over
    // random (state, block) pairs. Bounded loop, deterministic PRNG. ---
    uint64_t seed = 0x4d44350000000000ULL; // 'MD5\0...'
    for (int n = 0; n < 20000; n++) {
        uint32_t st0[4];
        uint8_t blk[64];
        for (int i = 0; i < 4; i++) st0[i] = (uint32_t)md5diff_rng(&seed);
        for (int i = 0; i < 64; i += 8) {
            uint64_t r = md5diff_rng(&seed);
            for (int j = 0; j < 8; j++) blk[i + j] = (uint8_t)(r >> (8 * j));
        }

        uint32_t sc[4], sr[4];
        for (int i = 0; i < 4; i++) { sc[i] = st0[i]; sr[i] = st0[i]; }
        md5_transform_c(sc, blk);
        md5_transform_rs(sr, blk);
        vectors++;
        for (int i = 0; i < 4; i++) {
            if (sc[i] != sr[i]) { mismatches++; break; }
        }
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] md5: %u vectors, %u mismatches -> %s (KAT %s)\n",
            vectors, mismatches, verdict, kat_fail ? "FAIL" : "OK");
    bootlog_write("[RUST-DIFF] md5: %u vectors, %u mismatches -> %s (KAT %s)",
                  vectors, mismatches, verdict, kat_fail ? "FAIL" : "OK");

    // --- Part 3: RDTSC micro-benchmark, C vs Rust transform over a fixed block.
    // ~100k iterations each; the state is fed back in-place every iteration so
    // the compiler cannot elide the loop, and its final word is folded into the
    // log so the result is observably used. Parity is the honest expectation. ---
    {
        const int iters = 100000;
        uint8_t fixed[64];
        for (int i = 0; i < 64; i++) fixed[i] = (uint8_t)(i * 7 + 1);

        uint32_t sc[4], sr[4];
        for (int i = 0; i < 4; i++) {
            sc[i] = 0x01234567u ^ ((uint32_t)i << 8);
            sr[i] = sc[i];
        }

        // Warm the I-cache/branch predictors for both paths (untimed).
        for (int i = 0; i < 200; i++) md5_transform_c(sc, fixed);
        for (int i = 0; i < 200; i++) md5_transform_rs(sr, fixed);

        uint64_t t0 = md5_tsc_serialized();
        for (int i = 0; i < iters; i++) md5_transform_c(sc, fixed);
        uint64_t t1 = md5_tsc_serialized();
        for (int i = 0; i < iters; i++) md5_transform_rs(sr, fixed);
        uint64_t t2 = md5_tsc_serialized();

        uint64_t c_cyc = (t1 - t0) / (uint64_t)iters;
        uint64_t r_cyc = (t2 - t1) / (uint64_t)iters;
        // ratio = RS / C, fixed-point x100 (integer-only, no FP on -mno-sse).
        uint64_t ratio100 = (c_cyc != 0) ? (r_cyc * 100ULL / c_cyc) : 0;
        // Fold a state word into the log so the benchmark work is observably used.
        uint64_t sink = (uint64_t)(sc[0] ^ sr[0]);

        kprintf("[RUST-PERF] md5: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu (sink=%llx)\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100),
                (unsigned long long)sink);
        bootlog_write("[RUST-PERF] md5: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}
