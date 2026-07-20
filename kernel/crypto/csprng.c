// csprng.c - Cryptographically-secure PRNG for MayteraOS (fix for the
// fake-audit CRITICAL: drivers/dev.c used to serve /dev/urandom from a
// deterministic xorshift64 seeded once from RDTSC; see csprng.h).
//
// Construction: HMAC-DRBG (NIST SP 800-90A section 10.1.2), built on the
// existing HMAC-SHA256 primitive (crypto/hmac.c). Two 32-byte state values
// are kept, K (the HMAC key) and V (the running value); Update() folds new
// material into both, Generate() repeatedly computes V = HMAC(K, V) to
// produce output.
//
// Entropy sources gathered at instantiate/reseed time:
//   - RDSEED (true hardware entropy source), if CPUID advertises it
//   - RDRAND (hardware DRBG), if CPUID advertises it
//   - RDTSC jitter: many small timed loops around a variable-latency port
//     read, only the low-order (noisy) bits of each delta are kept
//   - PIT tick counter (cpu/isr.c timer_ticks) and an RDTSC sample
//   - A couple of kernel addresses (stack location of a local, address of
//     a static), which vary with ASLR/KASLR-ish placement and physical
//     memory layout across boots
// All of the above are concatenated and compressed with SHA-256 (via the
// HMAC-DRBG Update step itself, which is already a hash-based extractor),
// so no single weak source can be leveraged alone: RDTSC jitter and tick
// counts alone are already enough to make output non-deterministic across
// boots even in environments (QEMU) where RDRAND/RDSEED are absent.
//
// Reseeding: automatic after RESEED_INTERVAL generate() calls, or after
// RESEED_TICKS PIT ticks have elapsed since the last reseed, whichever
// comes first. csprng_add_entropy() (wired to /dev/urandom writes) stirs
// caller-supplied bytes in immediately as HMAC-DRBG "additional_input".

#include "csprng.h"
#include "crypto.h"
#include "../string.h"
#include "../serial.h"

extern volatile uint64_t timer_ticks;   // cpu/isr.c, monotonic PIT tick

// ---------------------------------------------------------------------------
// CPU feature detection (RDRAND / RDSEED)
// ---------------------------------------------------------------------------

static int g_has_rdrand = 0;
static int g_has_rdseed = 0;
static int g_features_checked = 0;

static void detect_cpu_features(void) {
    uint32_t eax, ebx, ecx, edx;

    cpuid(1, &eax, &ebx, &ecx, &edx);
    g_has_rdrand = (ecx >> 30) & 1;          // CPUID.1:ECX.RDRAND[30]

    cpuid(7, &eax, &ebx, &ecx, &edx);
    g_has_rdseed = (ebx >> 18) & 1;          // CPUID.7.0:EBX.RDSEED[18]

    g_features_checked = 1;
}

static int rdrand64(uint64_t *out) {
    unsigned char ok;
    uint64_t val;
    for (int i = 0; i < 10; i++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(val), "=qm"(ok) :: "cc");
        if (ok) { *out = val; return 1; }
    }
    return 0;
}

static int rdseed64(uint64_t *out) {
    unsigned char ok;
    uint64_t val;
    for (int i = 0; i < 10; i++) {
        __asm__ volatile("rdseed %0; setc %1" : "=r"(val), "=qm"(ok) :: "cc");
        if (ok) { *out = val; return 1; }
    }
    return 0;
}

static inline uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// ---------------------------------------------------------------------------
// Entropy gathering
// ---------------------------------------------------------------------------

// Fills `out[0..len)` with fresh, mixed entropy from every source we have.
// Not itself cryptographically "smoothed" (that's the DRBG's job); this is
// just a wide net that raw HMAC-DRBG Update() folds down via SHA-256.
static void gather_entropy(uint8_t *out, size_t len) {
    if (!g_features_checked) detect_cpu_features();

    memset(out, 0, len);

    // 1) RDSEED - true hardware entropy source, XORed across the whole
    //    buffer a handful of times if the CPU advertises it.
    if (g_has_rdseed) {
        for (int i = 0; i < 4; i++) {
            uint64_t v;
            if (rdseed64(&v)) {
                for (size_t j = 0; j < len; j++) out[j] ^= ((uint8_t *)&v)[j % 8];
            }
        }
    }

    // 2) RDRAND - hardware DRBG, one word per 8-byte slice if available.
    if (g_has_rdrand) {
        for (size_t i = 0; i < len; i += 8) {
            uint64_t v;
            if (rdrand64(&v)) {
                size_t n = (len - i < 8) ? (len - i) : 8;
                for (size_t j = 0; j < n; j++) out[i + j] ^= ((uint8_t *)&v)[j];
            }
        }
    }

    // 3) RDTSC jitter: time a variable-latency operation (PIT status port
    //    read, port 0x61 on all PC-compatible platforms including QEMU) many
    //    times; the low bits of each delta are unpredictable "jitter" even
    //    with no RDRAND/RDSEED (e.g. plain QEMU/TCG).
    uint64_t jitter_acc = rdtsc64();
    for (int round = 0; round < 64; round++) {
        uint64_t t0 = rdtsc64();
        __asm__ volatile ("inb $0x61, %%al" ::: "al"); // cheap, variable-latency I/O
        uint64_t t1 = rdtsc64();
        uint64_t delta = t1 - t0;
        jitter_acc = (jitter_acc << 13) ^ (jitter_acc >> 7) ^ delta ^ t1;
        out[round % len] ^= (uint8_t)(jitter_acc & 0xff);
        out[(round * 7 + 3) % len] ^= (uint8_t)((jitter_acc >> 8) & 0xff);
    }

    // 4) PIT tick counter + a fresh RDTSC sample.
    uint64_t ticks = timer_ticks;
    uint64_t tsc = rdtsc64();
    for (size_t i = 0; i < 8 && i < len; i++) out[i] ^= ((uint8_t *)&ticks)[i];
    for (size_t i = 0; i < 8 && i < len; i++) out[(i + 8) % len] ^= ((uint8_t *)&tsc)[i];

    // 5) Kernel addresses (stack + static data) - varies with each boot's
    //    physical memory layout / stack depth at call time.
    int stack_var;
    uintptr_t stack_addr = (uintptr_t)&stack_var;
    uintptr_t static_addr = (uintptr_t)&g_has_rdrand;
    for (size_t i = 0; i < sizeof(stack_addr) && i < len; i++)
        out[(i * 3) % len] ^= ((uint8_t *)&stack_addr)[i];
    for (size_t i = 0; i < sizeof(static_addr) && i < len; i++)
        out[(i * 5 + 1) % len] ^= ((uint8_t *)&static_addr)[i];
}

// ---------------------------------------------------------------------------
// HMAC-DRBG core (SP 800-90A section 10.1.2)
// ---------------------------------------------------------------------------

#define DRBG_OUT 32   // HMAC-SHA256 output size

static uint8_t  g_K[DRBG_OUT];
static uint8_t  g_V[DRBG_OUT];
static int      g_instantiated = 0;
static uint64_t g_reseed_counter = 0;
static uint64_t g_last_reseed_tick = 0;

#define RESEED_INTERVAL   4096            // generate() calls between reseeds
#define RESEED_TICKS      (250ULL * 300)  // ~5 min at the kernel's 250 Hz tick

// K = HMAC(K, V || 0x00/0x01 || data); V = HMAC(K, V)
static void drbg_update(const uint8_t *data, size_t data_len) {
    hmac_sha256_ctx_t ctx;
    uint8_t sep;

    sep = 0x00;
    hmac_sha256_init(&ctx, g_K, DRBG_OUT);
    hmac_sha256_update(&ctx, g_V, DRBG_OUT);
    hmac_sha256_update(&ctx, &sep, 1);
    if (data_len) hmac_sha256_update(&ctx, data, data_len);
    hmac_sha256_final(&ctx, g_K);

    hmac_sha256(g_K, DRBG_OUT, g_V, DRBG_OUT, g_V);

    if (data_len == 0) return;

    sep = 0x01;
    hmac_sha256_init(&ctx, g_K, DRBG_OUT);
    hmac_sha256_update(&ctx, g_V, DRBG_OUT);
    hmac_sha256_update(&ctx, &sep, 1);
    hmac_sha256_update(&ctx, data, data_len);
    hmac_sha256_final(&ctx, g_K);

    hmac_sha256(g_K, DRBG_OUT, g_V, DRBG_OUT, g_V);
}

static void drbg_instantiate(const uint8_t *seed, size_t seed_len) {
    memset(g_K, 0x00, DRBG_OUT);
    memset(g_V, 0x01, DRBG_OUT);
    drbg_update(seed, seed_len);
    g_reseed_counter = 1;
    g_instantiated = 1;
    g_last_reseed_tick = timer_ticks;
}

static void drbg_reseed(const uint8_t *seed, size_t seed_len) {
    drbg_update(seed, seed_len);
    g_reseed_counter = 1;
    g_last_reseed_tick = timer_ticks;
}

static void drbg_generate(uint8_t *out, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        hmac_sha256(g_K, DRBG_OUT, g_V, DRBG_OUT, g_V);
        size_t n = (len - pos < DRBG_OUT) ? (len - pos) : DRBG_OUT;
        memcpy(out + pos, g_V, n);
        pos += n;
    }
    drbg_update(NULL, 0);
    g_reseed_counter++;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void csprng_init(void) {
    detect_cpu_features();

    // Gather two independent 32-byte samples: one used as "entropy_input",
    // one as the "nonce" the DRBG spec calls for. gather_entropy() re-times
    // fresh RDTSC/RDRAND/RDSEED/tick state each call so the two are not the
    // same bytes.
    uint8_t entropy_input[32];
    uint8_t nonce[32];
    gather_entropy(entropy_input, sizeof(entropy_input));
    gather_entropy(nonce, sizeof(nonce));

    uint8_t seed_material[64];
    memcpy(seed_material, entropy_input, 32);
    memcpy(seed_material + 32, nonce, 32);

    drbg_instantiate(seed_material, sizeof(seed_material));

    crypto_zero(entropy_input, sizeof(entropy_input));
    crypto_zero(nonce, sizeof(nonce));
    crypto_zero(seed_material, sizeof(seed_material));

    kprintf("[CSPRNG] HMAC-DRBG initialized (RDSEED=%s RDRAND=%s)\n",
            g_has_rdseed ? "yes" : "no", g_has_rdrand ? "yes" : "no");
}

void csprng_reseed(void) {
    if (!g_instantiated) { csprng_init(); return; }

    uint8_t entropy_input[32];
    gather_entropy(entropy_input, sizeof(entropy_input));
    drbg_reseed(entropy_input, sizeof(entropy_input));
    crypto_zero(entropy_input, sizeof(entropy_input));
}

static void maybe_reseed(void) {
    if (!g_instantiated) {
        csprng_init();
        return;
    }
    if (g_reseed_counter >= RESEED_INTERVAL ||
        (timer_ticks - g_last_reseed_tick) >= RESEED_TICKS) {
        csprng_reseed();
    }
}

void csprng_bytes(void *buf, size_t len) {
    maybe_reseed();
    drbg_generate((uint8_t *)buf, len);
}

void csprng_add_entropy(const void *data, size_t len) {
    if (!g_instantiated) csprng_init();
    if (!data || !len) return;
    // Additional caller-supplied input, folded in immediately. This is what
    // makes write()s to /dev/urandom actually stir the pool (previously
    // silently discarded).
    drbg_update((const uint8_t *)data, len);
}
