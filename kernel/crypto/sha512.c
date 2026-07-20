// sha512.c - SHA-512 / SHA-384 and HMAC-SHA384 for MayteraOS
// FIPS 180-4. Needed for TLS 1.3 cipher suite TLS_AES_256_GCM_SHA384 (0x1302),
// which many servers (Google, CERN, ...) prefer; its key schedule, transcript
// hash and Finished MAC all use SHA-384 rather than SHA-256.

#include "crypto.h"
#include "../string.h"

// SHA-512 round constants (first 64 bits of fractional parts of cube roots of
// the first 80 primes).
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

#define ROTR64(x, n)  (((x) >> (n)) | ((x) << (64 - (n))))
#define SCH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define SMAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0_64(x)  (ROTR64(x, 28) ^ ROTR64(x, 34) ^ ROTR64(x, 39))
#define EP1_64(x)  (ROTR64(x, 14) ^ ROTR64(x, 18) ^ ROTR64(x, 41))
#define SIG0_64(x) (ROTR64(x, 1)  ^ ROTR64(x, 8)  ^ ((x) >> 7))
#define SIG1_64(x) (ROTR64(x, 19) ^ ROTR64(x, 61) ^ ((x) >> 6))

// #404 / #488 Phase F: SHA-512 block-compression core ported to Rust.
//
// sha512_transform_rs (rustkern.rs) is a faithful, memory-safe drop-in for the
// pure 80-round block-compression leaf below. Like the SHA-256 port it takes
// ONLY the 8-word state and a 128-byte block, so the sha512_ctx_t struct never
// crosses the FFI boundary (init/update/final stay C and pass ctx->state). The
// live sha512_transform() dispatcher routes to it under -DRUST_SHA512 (Makefile
// CFLAGS strangler flag); with the flag undefined it falls straight back to
// sha512_transform_c. A boot-time differential self-test (sha512_rust_selftest
// below) proves sha512_transform_rs == sha512_transform_c on THIS build, re-runs
// the NIST known-answer vectors through the live sha512() API, and RDTSC-micro-
// benchmarks the two, before any crypto (TLS 1.3 SHA-384 suite) uses it.
extern void sha512_transform_rs(uint64_t *state, const uint8_t *block);

// Original C implementation, renamed sha512_transform_c so BOTH the C and the
// Rust path stay in the image (differential reference + trivial rollback). Now
// operates on state[8] directly (not ctx) so the leaf is struct-free across FFI.
static void sha512_transform_c(uint64_t state[8], const uint8_t block[128]) {
    uint64_t W[80];
    uint64_t a, b, c, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        W[i] = ((uint64_t)block[i * 8] << 56) |
               ((uint64_t)block[i * 8 + 1] << 48) |
               ((uint64_t)block[i * 8 + 2] << 40) |
               ((uint64_t)block[i * 8 + 3] << 32) |
               ((uint64_t)block[i * 8 + 4] << 24) |
               ((uint64_t)block[i * 8 + 5] << 16) |
               ((uint64_t)block[i * 8 + 6] << 8) |
               ((uint64_t)block[i * 8 + 7]);
    }
    for (i = 16; i < 80; i++) {
        W[i] = SIG1_64(W[i - 2]) + W[i - 7] + SIG0_64(W[i - 15]) + W[i - 16];
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 80; i++) {
        t1 = h + EP1_64(e) + SCH(e, f, g) + K512[i] + W[i];
        t2 = EP0_64(a) + SMAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

// Live dispatcher. Callers pass ctx->state. Under -DRUST_SHA512 the compression
// runs in Rust (sha512_transform_rs); otherwise it runs in C. Same 8-word
// (uint64_t) in/out contract either way.
static inline void sha512_transform(uint64_t state[8], const uint8_t block[128]) {
#ifdef RUST_SHA512
    sha512_transform_rs(state, block);
#else
    sha512_transform_c(state, block);
#endif
}

void sha512_init(sha512_ctx_t *ctx) {
    ctx->state[0] = 0x6a09e667f3bcc908ULL; ctx->state[1] = 0xbb67ae8584caa73bULL;
    ctx->state[2] = 0x3c6ef372fe94f82bULL; ctx->state[3] = 0xa54ff53a5f1d36f1ULL;
    ctx->state[4] = 0x510e527fade682d1ULL; ctx->state[5] = 0x9b05688c2b3e6c1fULL;
    ctx->state[6] = 0x1f83d9abfb41bd6bULL; ctx->state[7] = 0x5be0cd19137e2179ULL;
    ctx->count = 0;
}

void sha384_init(sha512_ctx_t *ctx) {
    ctx->state[0] = 0xcbbb9d5dc1059ed8ULL; ctx->state[1] = 0x629a292a367cd507ULL;
    ctx->state[2] = 0x9159015a3070dd17ULL; ctx->state[3] = 0x152fecd8f70e5939ULL;
    ctx->state[4] = 0x67332667ffc00b31ULL; ctx->state[5] = 0x8eb44a8768581511ULL;
    ctx->state[6] = 0xdb0c2e0d64f98fa7ULL; ctx->state[7] = 0x47b5481dbefa4fa4ULL;
    ctx->count = 0;
}

void sha512_update(sha512_ctx_t *ctx, const void *data, size_t length) {
    const uint8_t *p = (const uint8_t *)data;
    size_t buffered = (size_t)((ctx->count / 8) % 128);
    ctx->count += (uint64_t)length * 8;

    while (length > 0) {
        size_t space = 128 - buffered;
        size_t take = (length < space) ? length : space;
        memcpy(ctx->buffer + buffered, p, take);
        buffered += take;
        p += take;
        length -= take;
        if (buffered == 128) {
            sha512_transform(ctx->state, ctx->buffer);
            buffered = 0;
        }
    }
}

static void sha512_do_final(sha512_ctx_t *ctx, uint8_t *digest, int out_bytes) {
    uint64_t bitlen = ctx->count;
    size_t buffered = (size_t)((ctx->count / 8) % 128);

    ctx->buffer[buffered++] = 0x80;
    if (buffered > 112) {
        while (buffered < 128) ctx->buffer[buffered++] = 0;
        sha512_transform(ctx->state, ctx->buffer);
        buffered = 0;
    }
    while (buffered < 112) ctx->buffer[buffered++] = 0;

    // 128-bit length, big-endian; high 64 bits are 0 for our message sizes.
    for (int i = 0; i < 8; i++) ctx->buffer[112 + i] = 0;
    for (int i = 0; i < 8; i++) {
        ctx->buffer[120 + i] = (uint8_t)(bitlen >> (56 - 8 * i));
    }
    sha512_transform(ctx->state, ctx->buffer);

    int words = out_bytes / 8;
    for (int i = 0; i < words; i++) {
        for (int j = 0; j < 8; j++) {
            digest[i * 8 + j] = (uint8_t)(ctx->state[i] >> (56 - 8 * j));
        }
    }
}

void sha512_final(sha512_ctx_t *ctx, uint8_t digest[64]) {
    sha512_do_final(ctx, digest, 64);
}

void sha384_final(sha512_ctx_t *ctx, uint8_t digest[48]) {
    sha512_do_final(ctx, digest, 48);
}

void sha384(const void *data, size_t length, uint8_t digest[48]) {
    sha512_ctx_t ctx;
    sha384_init(&ctx);
    sha512_update(&ctx, data, length);
    sha384_final(&ctx, digest);
}

// =============================================================================
// HMAC-SHA384 (RFC 2104), block size 128, output 48
// =============================================================================

#define HMAC_SHA384_IPAD 0x36
#define HMAC_SHA384_OPAD 0x5C

void hmac_sha384_init(hmac_sha384_ctx_t *ctx, const void *key, size_t key_len) {
    uint8_t k_ipad[SHA512_BLOCK_SIZE];
    uint8_t tk[48];

    if (key_len > SHA512_BLOCK_SIZE) {
        sha384(key, key_len, tk);
        key = tk;
        key_len = 48;
    }

    memset(ctx->key_block, 0, SHA512_BLOCK_SIZE);
    memcpy(ctx->key_block, key, key_len);

    for (int i = 0; i < SHA512_BLOCK_SIZE; i++) {
        k_ipad[i] = ctx->key_block[i] ^ HMAC_SHA384_IPAD;
    }

    sha384_init(&ctx->inner);
    sha512_update(&ctx->inner, k_ipad, SHA512_BLOCK_SIZE);

    // Stash opad-mixed key for finalize.
    for (int i = 0; i < SHA512_BLOCK_SIZE; i++) {
        ctx->key_block[i] ^= HMAC_SHA384_OPAD;
    }

    memset(k_ipad, 0, sizeof(k_ipad));
    memset(tk, 0, sizeof(tk));
}

void hmac_sha384_update(hmac_sha384_ctx_t *ctx, const void *data, size_t length) {
    sha512_update(&ctx->inner, data, length);
}

void hmac_sha384_final(hmac_sha384_ctx_t *ctx, uint8_t mac[48]) {
    uint8_t inner_hash[48];
    sha384_final(&ctx->inner, inner_hash);

    sha384_init(&ctx->outer);
    sha512_update(&ctx->outer, ctx->key_block, SHA512_BLOCK_SIZE);
    sha512_update(&ctx->outer, inner_hash, 48);
    sha384_final(&ctx->outer, mac);

    memset(inner_hash, 0, sizeof(inner_hash));
    memset(ctx->key_block, 0, sizeof(ctx->key_block));
}

// HMAC-SHA384 one-shot, original C. Kept as hmac_sha384_c for the boot-time
// differential (hmac_rust_selftest in crypto/hmac.c) + trivial rollback.
void hmac_sha384_c(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 uint8_t mac[48]) {
    hmac_sha384_ctx_t ctx;
    hmac_sha384_init(&ctx, key, key_len);
    hmac_sha384_update(&ctx, data, data_len);
    hmac_sha384_final(&ctx, mac);
}

#ifdef RUST_HMAC
// #404 / #493 Phase K: Rust HMAC-SHA384 one-shot construction (rustkern.rs).
// Reaches the (already-Rust under -DRUST_SHA512) compression via the C sha384
// init / sha512_update / sha384_final glue; only the ipad/opad wrapper is Rust.
extern void hmac_sha384_rs(const void *key, size_t key_len,
                           const void *data, size_t data_len, uint8_t *mac);
#endif

// Live HMAC-SHA384 one-shot dispatcher. Under -DRUST_HMAC the construction runs
// in Rust (hmac_sha384_rs); otherwise in C (hmac_sha384_c). TLS 1.3 SHA-384
// cipher-suite Finished key schedule calls this symbol.
void hmac_sha384(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 uint8_t mac[48]) {
#ifdef RUST_HMAC
    hmac_sha384_rs(key, key_len, data, data_len, mac);
#else
    hmac_sha384_c(key, key_len, data, data_len, mac);
#endif
}

// =============================================================================
// Boot-time differential self-test + RDTSC micro-benchmark (#404 / #488 Phase F).
//
// Three bounded, run-once proofs (no busy-wait / #426):
//   (1) NIST known-answer vectors hashed through the LIVE sha512 init/update/
//       final path, which under -DRUST_SHA512 compresses via sha512_transform_rs.
//       Proves the Rust transform is correct end-to-end in the real crypto path,
//       not just against the C twin.
//   (2) A direct differential of sha512_transform_rs vs sha512_transform_c over a
//       large corpus of random (state[8], block[128]) pairs. This is why the
//       transform is a byte-identical drop-in and not merely KAT-correct.
//   (3) An RDTSC micro-benchmark of both transforms over a fixed block (parity is
//       the honest expectation for a 64-bit soft-ALU hash on this -mno-sse build).
// One [RUST-DIFF] sha512 line + one [RUST-PERF] sha512 line are logged to serial
// (kprintf) and /BOOTLOG.TXT (bootlog_write). Any mismatch => M > 0 => FAIL (the
// orchestrator leaves RUST_SHA512 OFF). sha512_transform_rs is referenced here so
// its Rust archive member is always pulled into the link regardless of the flag.

static uint64_t sha512diff_rng(uint64_t *s) {
    // xorshift64*: deterministic, decent spread for test coverage.
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static int sha512diff_digcmp(const uint8_t *d, const char *hex, int n) {
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
static inline uint64_t sha512_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

void sha512_rust_selftest(void) {
    extern int kprintf(const char *fmt, ...);
    extern void bootlog_write(const char *fmt, ...);

    uint32_t vectors = 0;
    uint32_t mismatches = 0;

    // --- Part 1: NIST known-answer vectors through the LIVE sha512 path. ---
    struct { const char *msg; size_t len; const char *hex; } kat[3] = {
        { "abc", 3,
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f" },
        { "", 0,
          "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
          "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e" },
        { "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
          "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", 112,
          "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
          "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909" },
    };
    int kat_fail = 0;
    for (int k = 0; k < 3; k++) {
        uint8_t dig[64];
        sha512_ctx_t c;
        sha512_init(&c);
        sha512_update(&c, kat[k].msg, kat[k].len);
        sha512_final(&c, dig);
        vectors++;
        if (sha512diff_digcmp(dig, kat[k].hex, 64) != 0) {
            mismatches++;
            kat_fail++;
        }
    }

    // --- Part 2: direct sha512_transform_rs vs sha512_transform_c differential
    // over random (state, block) pairs. Bounded loop, deterministic PRNG. ---
    uint64_t seed = 0x5348413531320000ULL; // 'SHA512\0\0'
    for (int n = 0; n < 20000; n++) {
        uint64_t st0[8];
        uint8_t blk[128];
        for (int i = 0; i < 8; i++) st0[i] = sha512diff_rng(&seed);
        for (int i = 0; i < 128; i += 8) {
            uint64_t r = sha512diff_rng(&seed);
            for (int j = 0; j < 8; j++) blk[i + j] = (uint8_t)(r >> (8 * j));
        }

        uint64_t sc[8], sr[8];
        for (int i = 0; i < 8; i++) { sc[i] = st0[i]; sr[i] = st0[i]; }
        sha512_transform_c(sc, blk);
        sha512_transform_rs(sr, blk);
        vectors++;
        for (int i = 0; i < 8; i++) {
            if (sc[i] != sr[i]) { mismatches++; break; }
        }
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] sha512: %u vectors, %u mismatches -> %s (KAT %s)\n",
            vectors, mismatches, verdict, kat_fail ? "FAIL" : "OK");
    bootlog_write("[RUST-DIFF] sha512: %u vectors, %u mismatches -> %s (KAT %s)",
                  vectors, mismatches, verdict, kat_fail ? "FAIL" : "OK");

    // --- Part 3: RDTSC micro-benchmark, C vs Rust transform over a fixed block.
    // ~100k iterations each; the state is fed back in-place every iteration so
    // the compiler cannot elide the loop, and its final word is folded into the
    // log so the result is observably used. Parity is the honest expectation. ---
    {
        const int iters = 100000;
        uint8_t fixed[128];
        for (int i = 0; i < 128; i++) fixed[i] = (uint8_t)(i * 7 + 1);

        uint64_t sc[8], sr[8];
        for (int i = 0; i < 8; i++) {
            sc[i] = 0x0123456789abcdefULL ^ ((uint64_t)i << 32);
            sr[i] = sc[i];
        }

        // Warm the I-cache/branch predictors for both paths (untimed).
        for (int i = 0; i < 200; i++) sha512_transform_c(sc, fixed);
        for (int i = 0; i < 200; i++) sha512_transform_rs(sr, fixed);

        uint64_t t0 = sha512_tsc_serialized();
        for (int i = 0; i < iters; i++) sha512_transform_c(sc, fixed);
        uint64_t t1 = sha512_tsc_serialized();
        for (int i = 0; i < iters; i++) sha512_transform_rs(sr, fixed);
        uint64_t t2 = sha512_tsc_serialized();

        uint64_t c_cyc = (t1 - t0) / (uint64_t)iters;
        uint64_t r_cyc = (t2 - t1) / (uint64_t)iters;
        // ratio = RS / C, fixed-point x100 (integer-only, no FP on -mno-sse).
        uint64_t ratio100 = (c_cyc != 0) ? (r_cyc * 100ULL / c_cyc) : 0;
        // Fold a state word into the log so the benchmark work is observably used.
        uint64_t sink = sc[0] ^ sr[0];

        kprintf("[RUST-PERF] sha512: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu (sink=%llx)\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100),
                (unsigned long long)sink);
        bootlog_write("[RUST-PERF] sha512: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}
