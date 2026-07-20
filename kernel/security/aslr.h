// aslr.h - Address Space Layout Randomization for MayteraOS
#ifndef SECURITY_ASLR_H
#define SECURITY_ASLR_H

#include "../types.h"

// ASLR randomization bits for each region
#define ASLR_STACK_BITS     16      // 16 bits = 64K pages = 256MB range
#define ASLR_HEAP_BITS      16      // 16 bits of randomization
#define ASLR_MMAP_BITS      20      // 20 bits for mmap region
#define ASLR_EXEC_BITS      16      // 16 bits for PIE executables

// Base addresses for randomizable regions (before randomization)
#define ASLR_STACK_BASE     0x00007FFFFF000000ULL   // User stack region (grows down)
#define ASLR_HEAP_BASE      0x0000600000000000ULL   // User heap region
#define ASLR_MMAP_BASE      0x0000700000000000ULL   // mmap region
#define ASLR_EXEC_BASE      0x0000000000400000ULL   // Executable base (PIE)

// Alignment requirements (must be page-aligned)
#define ASLR_PAGE_ALIGN     0x1000ULL               // 4KB page alignment
#define ASLR_STACK_ALIGN    0x10000ULL              // 64KB stack alignment

// ASLR configuration flags
#define ASLR_FLAG_ENABLED   (1 << 0)    // ASLR globally enabled
#define ASLR_FLAG_STACK     (1 << 1)    // Stack randomization enabled
#define ASLR_FLAG_HEAP      (1 << 2)    // Heap randomization enabled
#define ASLR_FLAG_MMAP      (1 << 3)    // mmap randomization enabled
#define ASLR_FLAG_EXEC      (1 << 4)    // PIE executable randomization enabled
#define ASLR_FLAG_ALL       (ASLR_FLAG_ENABLED | ASLR_FLAG_STACK | \
                             ASLR_FLAG_HEAP | ASLR_FLAG_MMAP | ASLR_FLAG_EXEC)

// ASLR entropy source types
typedef enum {
    ASLR_ENTROPY_NONE = 0,      // No hardware RNG available
    ASLR_ENTROPY_RDRAND,        // Intel RDRAND instruction
    ASLR_ENTROPY_RDSEED,        // Intel RDSEED instruction
    ASLR_ENTROPY_TSC,           // Fallback to TSC + mixing
    ASLR_ENTROPY_POOL           // Entropy pool accumulation
} aslr_entropy_source_t;

// Per-process ASLR state
typedef struct {
    uint64_t stack_offset;      // Random stack offset
    uint64_t heap_offset;       // Random heap offset
    uint64_t mmap_offset;       // Random mmap offset
    uint64_t exec_offset;       // Random executable offset (PIE)
    uint64_t stack_base;        // Computed stack base (after randomization)
    uint64_t heap_base;         // Computed heap base
    uint64_t mmap_base;         // Computed mmap base
    uint64_t exec_base;         // Computed exec base
} aslr_process_state_t;

// ============================================================================
// ASLR API Functions
// ============================================================================

/**
 * Initialize the ASLR subsystem
 * Detects available entropy sources and seeds the entropy pool
 */
void aslr_init(void);

/**
 * Check if ASLR is enabled
 * @return true if ASLR is globally enabled
 */
bool aslr_enabled(void);

/**
 * Enable or disable ASLR globally
 * @param enable    true to enable, false to disable
 */
void aslr_set_enabled(bool enable);

/**
 * Get current ASLR configuration flags
 * @return bitmask of ASLR_FLAG_* values
 */
uint32_t aslr_get_flags(void);

/**
 * Set ASLR configuration flags
 * @param flags     bitmask of ASLR_FLAG_* values
 */
void aslr_set_flags(uint32_t flags);

/**
 * Get detected entropy source
 * @return entropy source type
 */
aslr_entropy_source_t aslr_get_entropy_source(void);

// ============================================================================
// Random Number Generation
// ============================================================================

/**
 * Get a random 64-bit value
 * Uses best available hardware RNG, falls back to software
 * @return random 64-bit value
 */
uint64_t aslr_get_random(void);

/**
 * Get a random value within a range
 * @param max   maximum value (exclusive)
 * @return random value in [0, max)
 */
uint64_t aslr_get_random_range(uint64_t max);

/**
 * Get random bytes
 * @param buffer    destination buffer
 * @param size      number of bytes to generate
 */
void aslr_get_random_bytes(void *buffer, size_t size);

/**
 * Add entropy to the pool
 * @param data      entropy data
 * @param size      size of entropy data
 */
void aslr_add_entropy(const void *data, size_t size);

// ============================================================================
// Address Randomization
// ============================================================================

/**
 * Initialize ASLR state for a new process
 * Generates random offsets for all regions
 * @param state     pointer to process ASLR state to initialize
 */
void aslr_init_process(aslr_process_state_t *state);

/**
 * Get randomized stack base for a process
 * @param state     process ASLR state
 * @return randomized stack base address (top of stack)
 */
uint64_t aslr_get_stack_base(const aslr_process_state_t *state);

/**
 * Get randomized heap base for a process
 * @param state     process ASLR state
 * @return randomized heap base address
 */
uint64_t aslr_get_heap_base(const aslr_process_state_t *state);

/**
 * Get randomized mmap base for a process
 * @param state     process ASLR state
 * @return randomized mmap region base
 */
uint64_t aslr_get_mmap_base(const aslr_process_state_t *state);

/**
 * Get randomized executable base for PIE
 * @param state     process ASLR state
 * @return randomized executable load address
 */
uint64_t aslr_get_exec_base(const aslr_process_state_t *state);

/**
 * Randomize a stack pointer within page
 * Adds sub-page randomization to stack pointer
 * @param sp        initial stack pointer
 * @return randomized stack pointer (aligned to 16 bytes)
 */
uint64_t aslr_randomize_stack_pointer(uint64_t sp);

// ============================================================================
// Kernel ASLR (KASLR)
// ============================================================================

/**
 * Get kernel ASLR offset (if compiled with KASLR support)
 * @return kernel slide offset, or 0 if KASLR not active
 */
uint64_t aslr_get_kernel_offset(void);

/**
 * Print ASLR statistics and configuration
 */
void aslr_print_info(void);

#endif // SECURITY_ASLR_H
