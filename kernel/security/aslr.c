// aslr.c - Address Space Layout Randomization implementation for MayteraOS
#include "aslr.h"
#include "../serial.h"
#include "../string.h"

// ============================================================================
// ASLR State
// ============================================================================

// Global ASLR configuration
static uint32_t g_aslr_flags = ASLR_FLAG_ALL;
static aslr_entropy_source_t g_entropy_source = ASLR_ENTROPY_NONE;
static bool g_aslr_initialized = false;

// Entropy pool for mixing
#define ENTROPY_POOL_SIZE 256
static uint8_t g_entropy_pool[ENTROPY_POOL_SIZE];
static uint32_t g_entropy_index = 0;
static uint64_t g_entropy_counter = 0;

// ============================================================================
// CPU Feature Detection
// ============================================================================

// Check if RDRAND is supported
static bool cpu_has_rdrand(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ecx & (1 << 30)) != 0;  // RDRAND bit
}

// Check if RDSEED is supported
static bool cpu_has_rdseed(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(7, &eax, &ebx, &ecx, &edx);
    return (ebx & (1 << 18)) != 0;  // RDSEED bit
}

// ============================================================================
// Hardware Random Number Generation
// ============================================================================

// Get random value using RDRAND (Intel Ivy Bridge+)
static bool rdrand64(uint64_t *value) {
    uint64_t result;
    uint8_t success;

    // Try up to 10 times (RDRAND can fail if entropy depleted)
    for (int i = 0; i < 10; i++) {
        __asm__ volatile(
            "rdrand %0\n\t"
            "setc %1"
            : "=r"(result), "=qm"(success)
            :
            : "cc"
        );

        if (success) {
            *value = result;
            return true;
        }

        // Small delay before retry
        for (int j = 0; j < 100; j++) {
            __asm__ volatile("pause");
        }
    }

    return false;
}

// Get random value using RDSEED (Intel Broadwell+)
static bool rdseed64(uint64_t *value) {
    uint64_t result;
    uint8_t success;

    // RDSEED is slower, try fewer times
    for (int i = 0; i < 5; i++) {
        __asm__ volatile(
            "rdseed %0\n\t"
            "setc %1"
            : "=r"(result), "=qm"(success)
            :
            : "cc"
        );

        if (success) {
            *value = result;
            return true;
        }

        // Longer delay for RDSEED
        for (int j = 0; j < 1000; j++) {
            __asm__ volatile("pause");
        }
    }

    return false;
}

// Read Time Stamp Counter
static uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// ============================================================================
// Entropy Pool
// ============================================================================

// Simple mixing function (based on xorshift)
static uint64_t mix64(uint64_t x) {
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    return x * 0x2545F4914F6CDD1DULL;
}

// Update entropy pool with new data
void aslr_add_entropy(const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;

    for (size_t i = 0; i < size; i++) {
        // XOR with pool and rotate
        g_entropy_pool[g_entropy_index] ^= bytes[i];
        g_entropy_pool[g_entropy_index] ^= (uint8_t)(g_entropy_counter >> (i % 8));

        // Advance index with mixing
        g_entropy_index = (g_entropy_index + 1 + bytes[i]) % ENTROPY_POOL_SIZE;
        g_entropy_counter++;
    }
}

// Extract random value from entropy pool
static uint64_t entropy_pool_extract(void) {
    uint64_t result = 0;

    // Mix TSC into pool first
    uint64_t tsc = read_tsc();
    aslr_add_entropy(&tsc, sizeof(tsc));

    // Extract 8 bytes from pool with mixing
    for (int i = 0; i < 8; i++) {
        uint32_t idx = (g_entropy_index + i * 31) % ENTROPY_POOL_SIZE;
        result = (result << 8) | g_entropy_pool[idx];

        // Update pool as we extract
        g_entropy_pool[idx] ^= (uint8_t)(g_entropy_counter >> i);
    }

    // Additional mixing
    result = mix64(result ^ g_entropy_counter);
    g_entropy_counter++;

    return result;
}

// ============================================================================
// ASLR Initialization
// ============================================================================

void aslr_init(void) {
    kprintf("[ASLR] Initializing Address Space Layout Randomization...\n");

    // Detect best available entropy source
    if (cpu_has_rdseed()) {
        g_entropy_source = ASLR_ENTROPY_RDSEED;
        kprintf("[ASLR] Using RDSEED for entropy (best quality)\n");
    } else if (cpu_has_rdrand()) {
        g_entropy_source = ASLR_ENTROPY_RDRAND;
        kprintf("[ASLR] Using RDRAND for entropy\n");
    } else {
        g_entropy_source = ASLR_ENTROPY_TSC;
        kprintf("[ASLR] Using TSC + mixing for entropy (fallback)\n");
    }

    // Initialize entropy pool with multiple sources

    // 1. TSC (time stamp counter)
    uint64_t tsc = read_tsc();
    aslr_add_entropy(&tsc, sizeof(tsc));

    // 2. Stack address (varies by execution)
    uint64_t stack_addr = (uint64_t)&tsc;
    aslr_add_entropy(&stack_addr, sizeof(stack_addr));

    // 3. Code address
    uint64_t code_addr = (uint64_t)&aslr_init;
    aslr_add_entropy(&code_addr, sizeof(code_addr));

    // 4. Hardware RNG if available
    if (g_entropy_source == ASLR_ENTROPY_RDRAND || g_entropy_source == ASLR_ENTROPY_RDSEED) {
        uint64_t hw_random;
        for (int i = 0; i < 4; i++) {
            if (g_entropy_source == ASLR_ENTROPY_RDSEED) {
                if (rdseed64(&hw_random)) {
                    aslr_add_entropy(&hw_random, sizeof(hw_random));
                }
            } else {
                if (rdrand64(&hw_random)) {
                    aslr_add_entropy(&hw_random, sizeof(hw_random));
                }
            }
        }
    }

    // 5. More TSC samples (with delays for variation)
    for (int i = 0; i < 8; i++) {
        tsc = read_tsc();
        aslr_add_entropy(&tsc, sizeof(tsc));
        for (int j = 0; j < 100; j++) {
            __asm__ volatile("pause");
        }
    }

    g_aslr_initialized = true;
    kprintf("[ASLR] Entropy pool initialized (%u bytes)\n", ENTROPY_POOL_SIZE);
    kprintf("[ASLR] ASLR enabled with flags: 0x%x\n", g_aslr_flags);
}

// ============================================================================
// ASLR Configuration
// ============================================================================

bool aslr_enabled(void) {
    return (g_aslr_flags & ASLR_FLAG_ENABLED) != 0;
}

void aslr_set_enabled(bool enable) {
    if (enable) {
        g_aslr_flags |= ASLR_FLAG_ENABLED;
    } else {
        g_aslr_flags &= ~ASLR_FLAG_ENABLED;
    }
}

uint32_t aslr_get_flags(void) {
    return g_aslr_flags;
}

void aslr_set_flags(uint32_t flags) {
    g_aslr_flags = flags;
}

aslr_entropy_source_t aslr_get_entropy_source(void) {
    return g_entropy_source;
}

// ============================================================================
// Random Number Generation
// ============================================================================

uint64_t aslr_get_random(void) {
    uint64_t value;

    // Try hardware RNG first
    if (g_entropy_source == ASLR_ENTROPY_RDSEED) {
        if (rdseed64(&value)) {
            return value;
        }
    }

    if (g_entropy_source == ASLR_ENTROPY_RDRAND || g_entropy_source == ASLR_ENTROPY_RDSEED) {
        if (rdrand64(&value)) {
            return value;
        }
    }

    // Fall back to entropy pool
    return entropy_pool_extract();
}

uint64_t aslr_get_random_range(uint64_t max) {
    if (max == 0) return 0;
    if (max == 1) return 0;

    // Use rejection sampling to avoid modulo bias
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

void aslr_get_random_bytes(void *buffer, size_t size) {
    uint8_t *bytes = (uint8_t *)buffer;

    while (size >= 8) {
        uint64_t value = aslr_get_random();
        memcpy(bytes, &value, 8);
        bytes += 8;
        size -= 8;
    }

    if (size > 0) {
        uint64_t value = aslr_get_random();
        memcpy(bytes, &value, size);
    }
}

// ============================================================================
// Process ASLR State
// ============================================================================

void aslr_init_process(aslr_process_state_t *state) {
    if (!state) return;

    // Clear state first
    memset(state, 0, sizeof(aslr_process_state_t));

    if (!aslr_enabled()) {
        // No randomization - use fixed bases
        state->stack_base = ASLR_STACK_BASE;
        state->heap_base = ASLR_HEAP_BASE;
        state->mmap_base = ASLR_MMAP_BASE;
        state->exec_base = ASLR_EXEC_BASE;
        return;
    }

    // Generate random offsets for each region

    // Stack randomization (ASLR_STACK_BITS worth of pages)
    if (g_aslr_flags & ASLR_FLAG_STACK) {
        uint64_t max_offset = (1ULL << ASLR_STACK_BITS) * ASLR_STACK_ALIGN;
        state->stack_offset = aslr_get_random_range(max_offset);
        state->stack_offset = ALIGN_DOWN(state->stack_offset, ASLR_STACK_ALIGN);
    }
    state->stack_base = ASLR_STACK_BASE - state->stack_offset;

    // Heap randomization
    if (g_aslr_flags & ASLR_FLAG_HEAP) {
        uint64_t max_offset = (1ULL << ASLR_HEAP_BITS) * ASLR_PAGE_ALIGN;
        state->heap_offset = aslr_get_random_range(max_offset);
        state->heap_offset = ALIGN_DOWN(state->heap_offset, ASLR_PAGE_ALIGN);
    }
    state->heap_base = ASLR_HEAP_BASE + state->heap_offset;

    // mmap randomization
    if (g_aslr_flags & ASLR_FLAG_MMAP) {
        uint64_t max_offset = (1ULL << ASLR_MMAP_BITS) * ASLR_PAGE_ALIGN;
        state->mmap_offset = aslr_get_random_range(max_offset);
        state->mmap_offset = ALIGN_DOWN(state->mmap_offset, ASLR_PAGE_ALIGN);
    }
    state->mmap_base = ASLR_MMAP_BASE + state->mmap_offset;

    // Executable (PIE) randomization
    if (g_aslr_flags & ASLR_FLAG_EXEC) {
        uint64_t max_offset = (1ULL << ASLR_EXEC_BITS) * ASLR_PAGE_ALIGN;
        state->exec_offset = aslr_get_random_range(max_offset);
        state->exec_offset = ALIGN_DOWN(state->exec_offset, ASLR_PAGE_ALIGN);
    }
    state->exec_base = ASLR_EXEC_BASE + state->exec_offset;
}

uint64_t aslr_get_stack_base(const aslr_process_state_t *state) {
    if (!state) return ASLR_STACK_BASE;
    return state->stack_base;
}

uint64_t aslr_get_heap_base(const aslr_process_state_t *state) {
    if (!state) return ASLR_HEAP_BASE;
    return state->heap_base;
}

uint64_t aslr_get_mmap_base(const aslr_process_state_t *state) {
    if (!state) return ASLR_MMAP_BASE;
    return state->mmap_base;
}

uint64_t aslr_get_exec_base(const aslr_process_state_t *state) {
    if (!state) return ASLR_EXEC_BASE;
    return state->exec_base;
}

uint64_t aslr_randomize_stack_pointer(uint64_t sp) {
    if (!aslr_enabled() || !(g_aslr_flags & ASLR_FLAG_STACK)) {
        return sp;
    }

    // Add sub-page randomization (0-4080 bytes, aligned to 16)
    uint64_t sub_page_offset = aslr_get_random_range(256) * 16;  // 0-4080, 16-byte aligned

    // Subtract offset (stack grows down)
    return sp - sub_page_offset;
}

// ============================================================================
// Kernel ASLR
// ============================================================================

// Kernel slide (set at boot if KASLR is active)
static uint64_t g_kernel_slide = 0;

uint64_t aslr_get_kernel_offset(void) {
    return g_kernel_slide;
}

// ============================================================================
// Debug / Info
// ============================================================================

void aslr_print_info(void) {
    kprintf("[ASLR] Configuration:\n");
    kprintf("  Initialized:    %s\n", g_aslr_initialized ? "Yes" : "No");
    kprintf("  Enabled:        %s\n", aslr_enabled() ? "Yes" : "No");
    kprintf("  Flags:          0x%x\n", g_aslr_flags);
    kprintf("    Stack:        %s\n", (g_aslr_flags & ASLR_FLAG_STACK) ? "Yes" : "No");
    kprintf("    Heap:         %s\n", (g_aslr_flags & ASLR_FLAG_HEAP) ? "Yes" : "No");
    kprintf("    mmap:         %s\n", (g_aslr_flags & ASLR_FLAG_MMAP) ? "Yes" : "No");
    kprintf("    Exec (PIE):   %s\n", (g_aslr_flags & ASLR_FLAG_EXEC) ? "Yes" : "No");

    const char *source_name;
    switch (g_entropy_source) {
        case ASLR_ENTROPY_RDSEED: source_name = "RDSEED (hardware)"; break;
        case ASLR_ENTROPY_RDRAND: source_name = "RDRAND (hardware)"; break;
        case ASLR_ENTROPY_TSC:    source_name = "TSC + mixing"; break;
        case ASLR_ENTROPY_POOL:   source_name = "Entropy pool"; break;
        default:                  source_name = "None"; break;
    }
    kprintf("  Entropy:        %s\n", source_name);

    kprintf("  Randomization bits:\n");
    kprintf("    Stack:        %d bits (%lu MB range)\n",
            ASLR_STACK_BITS, ((1ULL << ASLR_STACK_BITS) * ASLR_STACK_ALIGN) / MB);
    kprintf("    Heap:         %d bits (%lu MB range)\n",
            ASLR_HEAP_BITS, ((1ULL << ASLR_HEAP_BITS) * ASLR_PAGE_ALIGN) / MB);
    kprintf("    mmap:         %d bits (%lu GB range)\n",
            ASLR_MMAP_BITS, ((1ULL << ASLR_MMAP_BITS) * ASLR_PAGE_ALIGN) / GB);
    kprintf("    Exec (PIE):   %d bits (%lu MB range)\n",
            ASLR_EXEC_BITS, ((1ULL << ASLR_EXEC_BITS) * ASLR_PAGE_ALIGN) / MB);

    // Sample random values
    kprintf("  Sample random:  0x%lx\n", aslr_get_random());
    kprintf("  Sample random:  0x%lx\n", aslr_get_random());
}
