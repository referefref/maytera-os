// rng.c - Random Number Generator for MayteraOS
// Uses RDRAND when available, with entropy pool fallback

#include "crypto.h"
#include "../string.h"
#include "../serial.h"

// External timer for entropy
extern volatile uint64_t timer_ticks;

// Entropy pool
#define POOL_SIZE 256
static uint8_t entropy_pool[POOL_SIZE];
static int pool_index = 0;
static int pool_initialized = 0;

// RDRAND availability
static int has_rdrand = 0;

// Check if RDRAND is available
static int check_rdrand(void) {
    uint32_t eax, ebx, ecx, edx;

    // CPUID with leaf 1
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));

    // RDRAND is bit 30 of ECX
    return (ecx >> 30) & 1;
}

// Get random using RDRAND (64-bit)
static int rdrand64(uint64_t *value) {
    uint64_t val;
    unsigned char success;

    // Try up to 10 times (RDRAND can fail if entropy is exhausted)
    for (int i = 0; i < 10; i++) {
        __asm__ volatile(
            "rdrand %0\n\t"
            "setc %1"
            : "=r"(val), "=qm"(success)
            :
            : "cc"
        );
        if (success) {
            *value = val;
            return 0;
        }
    }
    return -1;
}

// Get random using RDRAND (32-bit)
static int rdrand32(uint32_t *value) {
    uint32_t val;
    unsigned char success;

    for (int i = 0; i < 10; i++) {
        __asm__ volatile(
            "rdrand %0\n\t"
            "setc %1"
            : "=r"(val), "=qm"(success)
            :
            : "cc"
        );
        if (success) {
            *value = val;
            return 0;
        }
    }
    return -1;
}

// Mix entropy into pool using simple mixing function
static void mix_pool(const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        // Simple mixing: XOR with current position and rotate
        entropy_pool[pool_index] ^= data[i];
        entropy_pool[pool_index] = ((entropy_pool[pool_index] << 1) |
                                    (entropy_pool[pool_index] >> 7));

        // Also mix with neighboring bytes
        int prev = (pool_index + POOL_SIZE - 1) % POOL_SIZE;
        int next = (pool_index + 1) % POOL_SIZE;
        entropy_pool[prev] ^= entropy_pool[pool_index];
        entropy_pool[next] ^= entropy_pool[pool_index] >> 4;

        pool_index = (pool_index + 1) % POOL_SIZE;
    }
}

// Initialize RNG
void rng_init(void) {
    // Check for RDRAND support
    has_rdrand = check_rdrand();

    if (has_rdrand) {
        kprintf("[RNG] RDRAND instruction available\n");
    } else {
        kprintf("[RNG] RDRAND not available, using entropy pool\n");
    }

    // Initialize entropy pool
    memset(entropy_pool, 0, POOL_SIZE);
    pool_index = 0;

    // Seed with available entropy sources
    uint64_t seed = 0;

    // Add timer ticks
    seed ^= timer_ticks;
    mix_pool((uint8_t *)&seed, sizeof(seed));

    // Add TSC (Time Stamp Counter)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    seed = ((uint64_t)hi << 32) | lo;
    mix_pool((uint8_t *)&seed, sizeof(seed));

    // If we have RDRAND, seed from it
    if (has_rdrand) {
        for (int i = 0; i < 4; i++) {
            if (rdrand64(&seed) == 0) {
                mix_pool((uint8_t *)&seed, sizeof(seed));
            }
        }
    }

    pool_initialized = 1;
    kprintf("[RNG] Random number generator initialized\n");
}

// Add entropy to pool
void rng_add_entropy(const void *data, size_t length) {
    if (!pool_initialized) return;
    mix_pool((const uint8_t *)data, length);
}

// Extract bytes from pool with stretching
static void extract_from_pool(uint8_t *output, size_t length) {
    // Use SHA-256 to extract random bytes from pool
    sha256_ctx_t ctx;
    uint8_t hash[32];
    uint8_t counter = 0;

    while (length > 0) {
        sha256_init(&ctx);
        sha256_update(&ctx, entropy_pool, POOL_SIZE);
        sha256_update(&ctx, &counter, 1);
        sha256_final(&ctx, hash);

        size_t copy = (length < 32) ? length : 32;
        memcpy(output, hash, copy);

        // Mix hash back into pool for forward secrecy
        mix_pool(hash, 32);

        output += copy;
        length -= copy;
        counter++;
    }

    crypto_zero(hash, sizeof(hash));
}

// Get random bytes
int rng_get_bytes(void *buffer, size_t length) {
    uint8_t *p = (uint8_t *)buffer;

    if (!pool_initialized) {
        // Emergency initialization
        rng_init();
    }

    // If we have RDRAND, use it directly
    if (has_rdrand) {
        // Get 8 bytes at a time
        while (length >= 8) {
            uint64_t val;
            if (rdrand64(&val) != 0) {
                // RDRAND failed, fall back to pool
                break;
            }
            memcpy(p, &val, 8);
            p += 8;
            length -= 8;
        }

        // Get remaining bytes
        if (length > 0) {
            uint64_t val;
            if (rdrand64(&val) == 0) {
                memcpy(p, &val, length);
                return 0;
            }
        } else {
            return 0;
        }
    }

    // Fall back to entropy pool
    extract_from_pool(p, length);
    return 0;
}

// Get random 32-bit value
uint32_t rng_get_u32(void) {
    uint32_t value;

    if (has_rdrand && rdrand32(&value) == 0) {
        return value;
    }

    rng_get_bytes(&value, sizeof(value));
    return value;
}

// Get random 64-bit value
uint64_t rng_get_u64(void) {
    uint64_t value;

    if (has_rdrand && rdrand64(&value) == 0) {
        return value;
    }

    rng_get_bytes(&value, sizeof(value));
    return value;
}
