// hmac.c - HMAC (Hash-based Message Authentication Code)
// RFC 2104 implementation

#include "hmac.h"
#include "md5.h"
#include "crypto.h"
#include "../string.h"

#define HMAC_IPAD 0x36
#define HMAC_OPAD 0x5C
#define MD5_BLOCK_SIZE 64

// #404 / #493 Phase K: the Rust HMAC construction (rustkern.rs hmac_construct)
// stores each hash context in an OPAQUE, 16-aligned 256-byte buffer rather than
// duplicating the C struct layout. That buffer must be at least as large as any
// kernel hash context. Assert it here (all three ctx types are in scope) so a
// future struct growth trips the build instead of a silent Rust-side overflow.
_Static_assert(sizeof(md5_ctx_t)    <= 256, "md5_ctx_t exceeds Rust HMAC ctx buffer");
_Static_assert(sizeof(sha256_ctx_t) <= 256, "sha256_ctx_t exceeds Rust HMAC ctx buffer");
_Static_assert(sizeof(sha512_ctx_t) <= 256, "sha512_ctx_t exceeds Rust HMAC ctx buffer");

#ifdef RUST_HMAC
// Rust HMAC-MD5 one-shot (rustkern.rs). Same argument contract as hmac_md5_c.
extern void hmac_md5_rs(const void *key, size_t key_len,
                        const void *data, size_t data_len, uint8_t *mac);
#endif

// HMAC-MD5 one-shot, original C. Kept as hmac_md5_c for the boot-time
// differential (hmac_rust_selftest) + trivial rollback (drop -DRUST_HMAC).
void hmac_md5_c(const void *key, size_t key_len,
              const void *data, size_t data_len,
              uint8_t mac[16]) {
    uint8_t k_ipad[MD5_BLOCK_SIZE];
    uint8_t k_opad[MD5_BLOCK_SIZE];
    uint8_t tk[16];
    md5_ctx_t ctx;

    // If key is longer than block size, hash it
    if (key_len > MD5_BLOCK_SIZE) {
        md5(key, key_len, tk);
        key = tk;
        key_len = 16;
    }

    // XOR key with ipad and opad
    memset(k_ipad, HMAC_IPAD, MD5_BLOCK_SIZE);
    memset(k_opad, HMAC_OPAD, MD5_BLOCK_SIZE);

    for (size_t i = 0; i < key_len; i++) {
        k_ipad[i] ^= ((const uint8_t*)key)[i];
        k_opad[i] ^= ((const uint8_t*)key)[i];
    }

    // Inner hash: H(K XOR ipad || data)
    md5_init(&ctx);
    md5_update(&ctx, k_ipad, MD5_BLOCK_SIZE);
    md5_update(&ctx, data, data_len);
    md5_final(&ctx, mac);

    // Outer hash: H(K XOR opad || inner_hash)
    md5_init(&ctx);
    md5_update(&ctx, k_opad, MD5_BLOCK_SIZE);
    md5_update(&ctx, mac, 16);
    md5_final(&ctx, mac);

    // Clear sensitive data
    memset(k_ipad, 0, sizeof(k_ipad));
    memset(k_opad, 0, sizeof(k_opad));
    memset(tk, 0, sizeof(tk));
}

// Live HMAC-MD5 one-shot dispatcher. Under -DRUST_HMAC the construction runs in
// Rust (hmac_md5_rs); otherwise it runs in C (hmac_md5_c). Identical contract.
void hmac_md5(const void *key, size_t key_len,
              const void *data, size_t data_len,
              uint8_t mac[16]) {
#ifdef RUST_HMAC
    hmac_md5_rs(key, key_len, data, data_len, mac);
#else
    hmac_md5_c(key, key_len, data, data_len, mac);
#endif
}

// Incremental HMAC-MD5 context storage
static md5_ctx_t hmac_inner_ctx[8];
static md5_ctx_t hmac_outer_ctx[8];
static int hmac_ctx_used = 0;

// Initialize HMAC-MD5 context
void hmac_md5_init(hmac_md5_ctx_t *ctx, const void *key, size_t key_len) {
    uint8_t tk[16];
    int slot = hmac_ctx_used++ % 8;

    ctx->inner_ctx = &hmac_inner_ctx[slot];
    ctx->outer_ctx = &hmac_outer_ctx[slot];

    // If key is longer than block size, hash it
    if (key_len > MD5_BLOCK_SIZE) {
        md5(key, key_len, tk);
        key = tk;
        key_len = 16;
    }

    // Prepare key blocks
    memset(ctx->key_block, 0, MD5_BLOCK_SIZE);
    memcpy(ctx->key_block, key, key_len);

    // Prepare ipad and opad
    uint8_t k_ipad[MD5_BLOCK_SIZE];
    uint8_t k_opad[MD5_BLOCK_SIZE];

    for (int i = 0; i < MD5_BLOCK_SIZE; i++) {
        k_ipad[i] = ctx->key_block[i] ^ HMAC_IPAD;
        k_opad[i] = ctx->key_block[i] ^ HMAC_OPAD;
    }

    // Initialize inner hash
    md5_init((md5_ctx_t*)ctx->inner_ctx);
    md5_update((md5_ctx_t*)ctx->inner_ctx, k_ipad, MD5_BLOCK_SIZE);

    // Store opad for later
    memcpy(ctx->key_block, k_opad, MD5_BLOCK_SIZE);

    memset(k_ipad, 0, sizeof(k_ipad));
    memset(k_opad, 0, sizeof(k_opad));
    memset(tk, 0, sizeof(tk));
}

// Update HMAC with data
void hmac_md5_update(hmac_md5_ctx_t *ctx, const void *data, size_t length) {
    md5_update((md5_ctx_t*)ctx->inner_ctx, data, length);
}

// Finalize HMAC
void hmac_md5_final(hmac_md5_ctx_t *ctx, uint8_t mac[16]) {
    uint8_t inner_hash[16];

    // Finalize inner hash
    md5_final((md5_ctx_t*)ctx->inner_ctx, inner_hash);

    // Outer hash: H(K XOR opad || inner_hash)
    md5_init((md5_ctx_t*)ctx->outer_ctx);
    md5_update((md5_ctx_t*)ctx->outer_ctx, ctx->key_block, MD5_BLOCK_SIZE);
    md5_update((md5_ctx_t*)ctx->outer_ctx, inner_hash, 16);
    md5_final((md5_ctx_t*)ctx->outer_ctx, mac);

    // Clear sensitive data
    memset(inner_hash, 0, sizeof(inner_hash));
    memset(ctx->key_block, 0, sizeof(ctx->key_block));
}

// =============================================================================
// Boot-time HMAC differential self-test + RDTSC micro-benchmark
// (#404 / #493 Phase K).
//
// Three bounded, run-once proofs (no busy-wait / #426), one [RUST-DIFF] hmac +
// one [RUST-PERF] hmac line each to serial (kprintf) + /BOOTLOG (bootlog_write):
//   (1) KNOWN-ANSWER TESTS through the LIVE hmac_sha256 / hmac_sha384 / hmac_md5
//       API (which routes to the Rust construction under -DRUST_HMAC): RFC 4231
//       HMAC-SHA256 TC1/TC2 + HMAC-SHA384 TC1/TC2, RFC 2202 HMAC-MD5 TC1/TC2.
//       This proves the Rust construction is correct end-to-end in the real
//       crypto path, not just against the C twin.
//   (2) DIFFERENTIAL hmac_*_rs vs hmac_*_c over a large corpus of random
//       (key, msg) pairs for all three variants (the C twin uses the kernel's
//       INCREMENTAL init/update/final HMAC, an INDEPENDENT construction from the
//       Rust one-shot, so a shared-logic bug cannot hide). Any mismatch => the
//       orchestrator leaves -DRUST_HMAC off.
//   (3) RDTSC micro-benchmark of hmac_sha256_rs vs hmac_sha256_c over a fixed
//       small message (~50k iters). HMAC ~= 2x the hash; expect near-parity to
//       the already-Rust SHA-256 (ratio ~1.0), reported honestly.
// The *_rs symbols are called unconditionally here so their Rust archive members
// are always pulled into the link regardless of the -DRUST_HMAC strangler flag.

// Rust one-shot constructions (rustkern.rs), always present in the image.
extern void hmac_sha256_rs(const void *key, size_t key_len,
                           const void *data, size_t data_len, uint8_t *mac);
extern void hmac_sha384_rs(const void *key, size_t key_len,
                           const void *data, size_t data_len, uint8_t *mac);
extern void hmac_md5_rs(const void *key, size_t key_len,
                        const void *data, size_t data_len, uint8_t *mac);
// C twins (independent incremental-HMAC references).
extern void hmac_sha256_c(const void *key, size_t key_len,
                          const void *data, size_t data_len, uint8_t mac[32]);
extern void hmac_sha384_c(const void *key, size_t key_len,
                          const void *data, size_t data_len, uint8_t mac[48]);
extern void hmac_md5_c(const void *key, size_t key_len,
                       const void *data, size_t data_len, uint8_t mac[16]);

static uint32_t hmacdiff_rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static int hmacdiff_hexcmp(const uint8_t *d, const char *hex, int n) {
    for (int i = 0; i < n; i++) {
        uint8_t hi = (uint8_t)hex[i * 2];
        uint8_t lo = (uint8_t)hex[i * 2 + 1];
        hi = (hi >= 'a') ? (uint8_t)(hi - 'a' + 10) : (uint8_t)(hi - '0');
        lo = (lo >= 'a') ? (uint8_t)(lo - 'a' + 10) : (uint8_t)(lo - '0');
        if (d[i] != (uint8_t)((hi << 4) | lo)) return 1;
    }
    return 0;
}

// Serialized TSC read (cpuid fence), unaffected by -mno-sse.
static inline uint64_t hmac_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

void hmac_rust_selftest(void) {
    extern int kprintf(const char *fmt, ...);
    extern void bootlog_write(const char *fmt, ...);

    uint32_t vectors = 0;
    uint32_t mismatches = 0;
    int kat_fail = 0;

    // --- Part 1: RFC 4231 / RFC 2202 KATs through the LIVE hmac_* API. ---
    {
        uint8_t key20[20]; memset(key20, 0x0b, 20);
        uint8_t key16[16]; memset(key16, 0x0b, 16);
        const char *jefe = "Jefe";
        const char *hi = "Hi There";
        const char *wd = "what do ya want for nothing?"; // 28 bytes

        uint8_t m32[32], m48[48], m16[16];

        // HMAC-SHA256 TC1 / TC2
        hmac_sha256(key20, 20, hi, 8, m32); vectors++;
        if (hmacdiff_hexcmp(m32,
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32)) { mismatches++; kat_fail++; }
        hmac_sha256(jefe, 4, wd, 28, m32); vectors++;
        if (hmacdiff_hexcmp(m32,
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32)) { mismatches++; kat_fail++; }

        // HMAC-SHA384 TC1 / TC2
        hmac_sha384(key20, 20, hi, 8, m48); vectors++;
        if (hmacdiff_hexcmp(m48,
            "afd03944d84895626b0825f4ab46907f15f9dadbe4101ec682aa034c7cebc59cfaea9ea9076ede7f4af152e8b2fa9cb6", 48)) { mismatches++; kat_fail++; }
        hmac_sha384(jefe, 4, wd, 28, m48); vectors++;
        if (hmacdiff_hexcmp(m48,
            "af45d2e376484031617f78d2b58a6b1b9c7ef464f5a01b47e42ec3736322445e8e2240ca5e69e2c78b3239ecfab21649", 48)) { mismatches++; kat_fail++; }

        // HMAC-MD5 TC1 / TC2 (RFC 2202)
        hmac_md5(key16, 16, hi, 8, m16); vectors++;
        if (hmacdiff_hexcmp(m16, "9294727a3638bb1c13f48ef8158bfc9d", 16)) { mismatches++; kat_fail++; }
        hmac_md5(jefe, 4, wd, 28, m16); vectors++;
        if (hmacdiff_hexcmp(m16, "750c783e6ab0b503eaa86e310a5db738", 16)) { mismatches++; kat_fail++; }
    }

    // --- Part 2: direct hmac_*_rs vs hmac_*_c differential over random
    // (key, msg). Bounded loop, deterministic PRNG. 7000 vectors x 3 variants. ---
    {
        uint32_t seed = 0x484d4143; // 'HMAC'
        uint8_t key[200], msg[300];
        uint8_t a[48], b[48];
        for (int n = 0; n < 7000; n++) {
            size_t kl = hmacdiff_rng(&seed) % 200;
            size_t ml = hmacdiff_rng(&seed) % 300;
            for (size_t i = 0; i < kl; i++) key[i] = (uint8_t)hmacdiff_rng(&seed);
            for (size_t i = 0; i < ml; i++) msg[i] = (uint8_t)hmacdiff_rng(&seed);

            // HMAC-SHA256
            hmac_sha256_rs(key, kl, msg, ml, a);
            hmac_sha256_c(key, kl, msg, ml, b);
            vectors++;
            if (memcmp(a, b, 32) != 0) mismatches++;

            // HMAC-SHA384
            hmac_sha384_rs(key, kl, msg, ml, a);
            hmac_sha384_c(key, kl, msg, ml, b);
            vectors++;
            if (memcmp(a, b, 48) != 0) mismatches++;

            // HMAC-MD5
            hmac_md5_rs(key, kl, msg, ml, a);
            hmac_md5_c(key, kl, msg, ml, b);
            vectors++;
            if (memcmp(a, b, 16) != 0) mismatches++;
        }
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] hmac: %u vectors, %u mismatches -> %s (KAT %s: SHA256/384 RFC4231 + MD5 RFC2202)\n",
            vectors, mismatches, verdict, kat_fail ? "FAIL" : "OK");
    bootlog_write("[RUST-DIFF] hmac: %u vectors, %u mismatches -> %s (KAT %s)",
                  vectors, mismatches, verdict, kat_fail ? "FAIL" : "OK");

    // --- Part 3: RDTSC micro-benchmark, HMAC-SHA256 C vs Rust over a fixed
    // small (32-byte key, 32-byte msg) input, ~50k iters each. The mac's first
    // word is folded into a sink so the work is observably used. ---
    {
        const int iters = 50000;
        uint8_t key[32], msg[32], mac[32];
        for (int i = 0; i < 32; i++) { key[i] = (uint8_t)(i * 3 + 1); msg[i] = (uint8_t)(i * 7 + 5); }

        // Warm I-cache / predictors (untimed).
        for (int i = 0; i < 200; i++) hmac_sha256_c(key, 32, msg, 32, mac);
        for (int i = 0; i < 200; i++) hmac_sha256_rs(key, 32, msg, 32, mac);

        uint64_t sink = 0;
        uint64_t t0 = hmac_tsc_serialized();
        for (int i = 0; i < iters; i++) { hmac_sha256_c(key, 32, msg, 32, mac); sink ^= mac[0]; }
        uint64_t t1 = hmac_tsc_serialized();
        for (int i = 0; i < iters; i++) { hmac_sha256_rs(key, 32, msg, 32, mac); sink ^= mac[0]; }
        uint64_t t2 = hmac_tsc_serialized();

        uint64_t c_cyc = (t1 - t0) / (uint64_t)iters;
        uint64_t r_cyc = (t2 - t1) / (uint64_t)iters;
        uint64_t ratio100 = (c_cyc != 0) ? (r_cyc * 100ULL / c_cyc) : 0;

        kprintf("[RUST-PERF] hmac: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu (HMAC-SHA256, sink=%llx)\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100),
                (unsigned long long)sink);
        bootlog_write("[RUST-PERF] hmac: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}
