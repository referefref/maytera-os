// aslr.c - address-space randomisation support for MayteraOS.
// See aslr.h for the measured statement of what is live and what is not (#646).
#include "aslr.h"
#include "../serial.h"
#include "../string.h"
#include "../crypto/csprng.h"

// ============================================================================
// ASLR State
// ============================================================================

static bool g_aslr_enabled = true;
static aslr_entropy_source_t g_entropy_source = ASLR_ENTROPY_NONE;
static bool g_aslr_initialized = false;

// ============================================================================
// CPU Feature Detection (for REPORTING the seed quality, not for the draws)
// ============================================================================

static bool cpu_has_rdrand(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ecx & (1 << 30)) != 0;  // RDRAND bit
}

static bool cpu_has_rdseed(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(7, &eax, &ebx, &ecx, &edx);
    return (ebx & (1 << 18)) != 0;  // RDSEED bit
}

static uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// ============================================================================
// Entropy
// ============================================================================

// #654: the private 256-byte xorshift pool that used to live here is GONE.
// It was a mixer, not a CSPRNG, and it fed the two consumers that most needed
// a real one: every user-image ASLR base and the kernel stack canary. All
// randomness now comes from crypto/csprng.c (HMAC-DRBG), which is the same
// source /dev/urandom and the password salts already use. One implementation,
// one place to audit, one place to fix.
void aslr_add_entropy(const void *data, size_t size) {
    csprng_add_entropy(data, size);
}

void aslr_init(void) {
    // #654: this used to prime a private entropy pool with TSC/stack/code
    // addresses and a few RDRAND draws. That pool is gone; crypto/csprng.c
    // gathers the same classes of entropy (RDSEED/RDRAND, RDTSC jitter, PIT
    // ticks) into an HMAC-DRBG and reseeds on a policy, which the pool never
    // did. All that is left here is detection for REPORTING, and offering the
    // boot-time samples to the real pool.
    if (cpu_has_rdseed()) {
        g_entropy_source = ASLR_ENTROPY_RDSEED;
    } else if (cpu_has_rdrand()) {
        g_entropy_source = ASLR_ENTROPY_RDRAND;
    } else {
        g_entropy_source = ASLR_ENTROPY_TSC;
    }

    // Boot-time samples still worth offering: they are cheap and they vary.
    uint64_t tsc = read_tsc();
    aslr_add_entropy(&tsc, sizeof(tsc));
    uint64_t stack_addr = (uint64_t)&tsc;
    aslr_add_entropy(&stack_addr, sizeof(stack_addr));
    uint64_t code_addr = (uint64_t)&aslr_init;
    aslr_add_entropy(&code_addr, sizeof(code_addr));
    for (int i = 0; i < 8; i++) {
        tsc = read_tsc();
        aslr_add_entropy(&tsc, sizeof(tsc));
        for (int j = 0; j < 100; j++) __asm__ volatile("pause");
    }

    g_aslr_initialized = true;
    kprintf("[ASLR] entropy: crypto/csprng.c HMAC-DRBG (hw seed: %s)\n",
            g_entropy_source == ASLR_ENTROPY_RDSEED ? "RDSEED" :
            g_entropy_source == ASLR_ENTROPY_RDRAND ? "RDRAND" : "none (TSC jitter)");
}

// ============================================================================
// Policy
// ============================================================================

bool aslr_enabled(void) { return g_aslr_enabled; }
void aslr_set_enabled(bool enable) { g_aslr_enabled = enable; }

// ============================================================================
// Random Number Generation
// ============================================================================

uint64_t aslr_get_random(void) {
    // #654: draw from the shared HMAC-DRBG. csprng_bytes() self-instantiates
    // on first use (maybe_reseed -> csprng_init), which matters because
    // security_init() runs long before dev_init() calls csprng_init().
    uint64_t value;
    csprng_bytes(&value, sizeof(value));
    return value;
}

uint64_t aslr_get_random_range(uint64_t max) {
    if (max == 0) return 0;
    if (max == 1) return 0;

    // Rejection sampling to avoid modulo bias
    uint64_t mask = max - 1;
    mask |= mask >> 1;
    mask |= mask >> 2;
    mask |= mask >> 4;
    mask |= mask >> 8;
    mask |= mask >> 16;
    mask |= mask >> 32;

    uint64_t value;
    do {
        value = aslr_get_random() & mask;
    } while (value >= max);

    return value;
}

// ============================================================================
// Reporting
// ============================================================================

void aslr_print_info(void) {
    const char *source_name;
    switch (g_entropy_source) {
        case ASLR_ENTROPY_RDSEED: source_name = "RDSEED (hardware)"; break;
        case ASLR_ENTROPY_RDRAND: source_name = "RDRAND (hardware)"; break;
        case ASLR_ENTROPY_TSC:    source_name = "TSC jitter only"; break;
        default:                  source_name = "not detected (aslr_init not run)"; break;
    }

    kprintf("[ASLR] Randomisation posture (measured, #646):\n");
    kprintf("  Entropy seeded:   %s\n", g_aslr_initialized ? "Yes" : "No");
    kprintf("  Hardware seed:    %s\n", source_name);
    kprintf("  Bytes from:       crypto/csprng.c HMAC-DRBG\n");
    kprintf("  Policy knob:      %s\n", g_aslr_enabled ? "enabled" : "disabled");
    kprintf("  User image (PIE): RANDOMISED by exec/elf.c, 2MB granularity,\n");
    kprintf("                    up to 512 slots (~9 bits) in the 1GB image window.\n");
    kprintf("                    NOTE: exec/elf.c does NOT consult the policy knob.\n");
    kprintf("  User stack:       not randomised\n");
    kprintf("  User heap:        not randomised\n");
    kprintf("  mmap region:      not randomised\n");
    kprintf("  Kernel (KASLR):   not implemented\n");
    // Deliberately NOT printing sample draws. The previous version emitted two
    // raw 64-bit CSPRNG outputs to the serial console, which bootlog_arm()
    // persists to /BOOTLOG.TXT on the FAT ESP - the same DRBG that produces the
    // stack canary and the password salts. Free output of a security PRNG into
    // a file on disk is not something a status printer should do.
}
