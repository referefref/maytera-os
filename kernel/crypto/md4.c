// md4.c - MD4 Hash Implementation for NTLM Authentication
// Based on RFC 1320

#include "md4.h"
#include "../string.h"

// MD4 auxiliary functions
#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))

// Left rotate
#define ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

// Round 1
#define FF(a, b, c, d, x, s) { \
    (a) += F((b), (c), (d)) + (x); \
    (a) = ROTL((a), (s)); \
}

// Round 2
#define GG(a, b, c, d, x, s) { \
    (a) += G((b), (c), (d)) + (x) + 0x5A827999; \
    (a) = ROTL((a), (s)); \
}

// Round 3
#define HH(a, b, c, d, x, s) { \
    (a) += H((b), (c), (d)) + (x) + 0x6ED9EBA1; \
    (a) = ROTL((a), (s)); \
}

// #404 / #490 Phase H: MD4 block-compression core ported to Rust.
//
// md4_transform_rs (rustkern.rs) is a faithful, memory-safe drop-in for the pure
// 48-step (3-round) block-compression leaf below. Like the MD5/SHA ports it takes
// ONLY the 4-word state and a 64-byte block, so the md4_ctx_t struct never
// crosses the FFI boundary (init/update/final stay C and pass ctx->state). The
// live md4_transform() dispatcher routes to it under -DRUST_MD4 (Makefile CFLAGS
// strangler flag); with the flag undefined it falls straight back to
// md4_transform_c. A boot-time differential self-test (md4_rust_selftest below)
// proves md4_transform_rs == md4_transform_c on THIS build, re-runs the RFC 1320
// known-answer vectors through the live md4() API, and RDTSC-micro-benchmarks the
// two, before any consumer (NTLM auth in net/smb.c) uses it. NOTE: MD4 decodes
// the message words LITTLE-endian (like MD5); the Rust port matches the C x[i]
// decode exactly.
extern void md4_transform_rs(uint32_t *state, const uint8_t *block);

// Original C implementation, renamed md4_transform_c so BOTH the C and the Rust
// path stay in the image (differential reference + trivial rollback). Operates on
// state[4] directly (not ctx), so the leaf is struct-free across FFI.
static void md4_transform_c(uint32_t state[4], const uint8_t block[64]) {
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
    FF(a, b, c, d, x[ 0],  3);
    FF(d, a, b, c, x[ 1],  7);
    FF(c, d, a, b, x[ 2], 11);
    FF(b, c, d, a, x[ 3], 19);
    FF(a, b, c, d, x[ 4],  3);
    FF(d, a, b, c, x[ 5],  7);
    FF(c, d, a, b, x[ 6], 11);
    FF(b, c, d, a, x[ 7], 19);
    FF(a, b, c, d, x[ 8],  3);
    FF(d, a, b, c, x[ 9],  7);
    FF(c, d, a, b, x[10], 11);
    FF(b, c, d, a, x[11], 19);
    FF(a, b, c, d, x[12],  3);
    FF(d, a, b, c, x[13],  7);
    FF(c, d, a, b, x[14], 11);
    FF(b, c, d, a, x[15], 19);

    // Round 2
    GG(a, b, c, d, x[ 0],  3);
    GG(d, a, b, c, x[ 4],  5);
    GG(c, d, a, b, x[ 8],  9);
    GG(b, c, d, a, x[12], 13);
    GG(a, b, c, d, x[ 1],  3);
    GG(d, a, b, c, x[ 5],  5);
    GG(c, d, a, b, x[ 9],  9);
    GG(b, c, d, a, x[13], 13);
    GG(a, b, c, d, x[ 2],  3);
    GG(d, a, b, c, x[ 6],  5);
    GG(c, d, a, b, x[10],  9);
    GG(b, c, d, a, x[14], 13);
    GG(a, b, c, d, x[ 3],  3);
    GG(d, a, b, c, x[ 7],  5);
    GG(c, d, a, b, x[11],  9);
    GG(b, c, d, a, x[15], 13);

    // Round 3
    HH(a, b, c, d, x[ 0],  3);
    HH(d, a, b, c, x[ 8],  9);
    HH(c, d, a, b, x[ 4], 11);
    HH(b, c, d, a, x[12], 15);
    HH(a, b, c, d, x[ 2],  3);
    HH(d, a, b, c, x[10],  9);
    HH(c, d, a, b, x[ 6], 11);
    HH(b, c, d, a, x[14], 15);
    HH(a, b, c, d, x[ 1],  3);
    HH(d, a, b, c, x[ 9],  9);
    HH(c, d, a, b, x[ 5], 11);
    HH(b, c, d, a, x[13], 15);
    HH(a, b, c, d, x[ 3],  3);
    HH(d, a, b, c, x[11],  9);
    HH(c, d, a, b, x[ 7], 11);
    HH(b, c, d, a, x[15], 15);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;

    // Clear sensitive data
    memset(x, 0, sizeof(x));
}

// Live dispatcher. Callers pass ctx->state. Under -DRUST_MD4 the compression
// runs in Rust (md4_transform_rs); otherwise it runs in C. Same 4-word state +
// 64-byte block leaf either way, so the md4_ctx_t struct never crosses FFI.
static inline void md4_transform(uint32_t state[4], const uint8_t block[64]) {
#ifdef RUST_MD4
    md4_transform_rs(state, block);
#else
    md4_transform_c(state, block);
#endif
}

// Initialize MD4 context
void md4_init(md4_ctx_t *ctx) {
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->count = 0;
}

// Update hash with data
void md4_update(md4_ctx_t *ctx, const void *data, size_t length) {
    const uint8_t *input = (const uint8_t *)data;
    size_t index = (ctx->count / 8) % 64;

    ctx->count += (uint64_t)length * 8;

    size_t partial = 64 - index;
    size_t i = 0;

    if (length >= partial) {
        memcpy(&ctx->buffer[index], input, partial);
        md4_transform(ctx->state, ctx->buffer);

        for (i = partial; i + 63 < length; i += 64) {
            md4_transform(ctx->state, &input[i]);
        }

        index = 0;
    }

    memcpy(&ctx->buffer[index], &input[i], length - i);
}

// Finalize and get digest
void md4_final(md4_ctx_t *ctx, uint8_t digest[16]) {
    static const uint8_t padding[64] = { 0x80 };
    uint8_t bits[8];

    // Save bit count (little-endian)
    for (int i = 0; i < 8; i++) {
        bits[i] = (ctx->count >> (i * 8)) & 0xFF;
    }

    // Pad to 56 mod 64
    size_t index = (ctx->count / 8) % 64;
    size_t padlen = (index < 56) ? (56 - index) : (120 - index);
    md4_update(ctx, padding, padlen);

    // Append length
    md4_update(ctx, bits, 8);

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
void md4(const void *data, size_t length, uint8_t digest[16]) {
    md4_ctx_t ctx;
    md4_init(&ctx);
    md4_update(&ctx, data, length);
    md4_final(&ctx, digest);
}

// =============================================================================
// Boot-time differential self-test + RDTSC micro-benchmark (#404 / #490 Phase H).
//
// Three bounded, run-once proofs (no busy-wait / #426):
//   (1) RFC 1320 known-answer vectors hashed through the LIVE md4 init/update/
//       final path, which under -DRUST_MD4 compresses via md4_transform_rs.
//       Proves the Rust transform is correct end-to-end in the real hash path,
//       not just against the C twin.
//   (2) A direct differential of md4_transform_rs vs md4_transform_c over a large
//       corpus of random (state[4], block[64]) pairs. This is why the transform
//       is a byte-identical drop-in and not merely KAT-correct.
//   (3) An RDTSC micro-benchmark of both transforms over a fixed block (both are
//       unrolled the same 48-step way, so parity is the honest expectation).
// One [RUST-DIFF] md4 line + one [RUST-PERF] md4 line are logged to serial
// (kprintf) and /BOOTLOG.TXT (bootlog_write). Any mismatch => M > 0 => FAIL (the
// orchestrator leaves RUST_MD4 OFF). md4_transform_rs is referenced here so its
// Rust archive member is always pulled into the link regardless of the flag.

static uint64_t md4diff_rng(uint64_t *s) {
    // xorshift64*: deterministic, decent spread for test coverage.
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static int md4diff_digcmp(const uint8_t *d, const char *hex, int n) {
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
static inline uint64_t md4_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

void md4_rust_selftest(void) {
    extern int kprintf(const char *fmt, ...);
    extern void bootlog_write(const char *fmt, ...);

    uint32_t vectors = 0;
    uint32_t mismatches = 0;

    // --- Part 1: RFC 1320 known-answer vectors through the LIVE md4 path. ---
    struct { const char *msg; size_t len; const char *hex; } kat[3] = {
        { "", 0, "31d6cfe0d16ae931b73c59d7e0c089c0" },
        { "abc", 3, "a448017aaf21d8525fc10ae87aa6729d" },
        { "message digest", 14, "d9130a8164549fe818874806e1c7014b" },
    };
    int kat_fail = 0;
    for (int k = 0; k < 3; k++) {
        uint8_t dig[16];
        md4(kat[k].msg, kat[k].len, dig);
        vectors++;
        if (md4diff_digcmp(dig, kat[k].hex, 16) != 0) {
            mismatches++;
            kat_fail++;
        }
    }

    // --- Part 2: direct md4_transform_rs vs md4_transform_c differential over
    // random (state, block) pairs. Bounded loop, deterministic PRNG. ---
    uint64_t seed = 0x4d44340000000000ULL; // 'MD4\0...'
    for (int n = 0; n < 20000; n++) {
        uint32_t st0[4];
        uint8_t blk[64];
        for (int i = 0; i < 4; i++) st0[i] = (uint32_t)md4diff_rng(&seed);
        for (int i = 0; i < 64; i += 8) {
            uint64_t r = md4diff_rng(&seed);
            for (int j = 0; j < 8; j++) blk[i + j] = (uint8_t)(r >> (8 * j));
        }

        uint32_t sc[4], sr[4];
        for (int i = 0; i < 4; i++) { sc[i] = st0[i]; sr[i] = st0[i]; }
        md4_transform_c(sc, blk);
        md4_transform_rs(sr, blk);
        vectors++;
        for (int i = 0; i < 4; i++) {
            if (sc[i] != sr[i]) { mismatches++; break; }
        }
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] md4: %u vectors, %u mismatches -> %s (KAT %s)\n",
            vectors, mismatches, verdict, kat_fail ? "FAIL" : "OK");
    bootlog_write("[RUST-DIFF] md4: %u vectors, %u mismatches -> %s (KAT %s)",
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
        for (int i = 0; i < 200; i++) md4_transform_c(sc, fixed);
        for (int i = 0; i < 200; i++) md4_transform_rs(sr, fixed);

        uint64_t t0 = md4_tsc_serialized();
        for (int i = 0; i < iters; i++) md4_transform_c(sc, fixed);
        uint64_t t1 = md4_tsc_serialized();
        for (int i = 0; i < iters; i++) md4_transform_rs(sr, fixed);
        uint64_t t2 = md4_tsc_serialized();

        uint64_t c_cyc = (t1 - t0) / (uint64_t)iters;
        uint64_t r_cyc = (t2 - t1) / (uint64_t)iters;
        // ratio = RS / C, fixed-point x100 (integer-only, no FP on -mno-sse).
        uint64_t ratio100 = (c_cyc != 0) ? (r_cyc * 100ULL / c_cyc) : 0;
        // Fold a state word into the log so the benchmark work is observably used.
        uint64_t sink = (uint64_t)(sc[0] ^ sr[0]);

        kprintf("[RUST-PERF] md4: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu (sink=%llx)\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100),
                (unsigned long long)sink);
        bootlog_write("[RUST-PERF] md4: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}
