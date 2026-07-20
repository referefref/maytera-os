// stack_guard.h - Stack Canary Protection for MayteraOS
#ifndef SECURITY_STACK_GUARD_H
#define SECURITY_STACK_GUARD_H

#include "../types.h"

// ============================================================================
// Stack Guard Configuration
// ============================================================================

// Canary value patterns
#define STACK_CANARY_MAGIC      0xDEADBEEFCAFEBABEULL   // Fallback magic value
#define STACK_CANARY_TERMINATOR 0x00                    // Null terminator in LSB
#define STACK_CANARY_NEWLINE    0x0A                    // Newline character
#define STACK_CANARY_CR         0x0D                    // Carriage return

// Stack guard features
#define STACK_GUARD_RANDOM_CANARY   (1 << 0)    // Use random canary per thread
#define STACK_GUARD_TERMINATOR      (1 << 1)    // Include string terminators
#define STACK_GUARD_KERNEL          (1 << 2)    // Protect kernel stacks
#define STACK_GUARD_USER            (1 << 3)    // Protect user stacks

#define STACK_GUARD_DEFAULT (STACK_GUARD_RANDOM_CANARY | STACK_GUARD_TERMINATOR | \
                             STACK_GUARD_KERNEL | STACK_GUARD_USER)

// ============================================================================
// Stack Guard State
// ============================================================================

// Per-thread stack guard info
typedef struct {
    uint64_t canary;            // The canary value for this thread
    uint64_t stack_base;        // Base of the stack (highest address)
    uint64_t stack_limit;       // Bottom of the stack (lowest address)
    uint64_t guard_page;        // Guard page address (if using guard pages)
    bool guard_page_enabled;    // Whether guard page is active
} stack_guard_t;

// Stack overflow handler callback
typedef void (*stack_overflow_handler_t)(uint32_t pid, const char *name, uint64_t fault_addr);

// ============================================================================
// Stack Guard API
// ============================================================================

/**
 * Initialize the stack guard subsystem
 * Sets up the global canary and enables stack protection
 */
void stack_guard_init(void);

/**
 * Get the global canary value
 * Used by GCC's -fstack-protector
 * @return global canary value
 */
uint64_t stack_guard_get_canary(void);

/**
 * Generate a new random canary
 * @return random canary value with protective characters
 */
uint64_t stack_guard_generate_canary(void);

/**
 * Initialize stack guard for a thread
 * @param guard     pointer to thread's stack guard structure
 * @param stack_base    top of stack (highest address)
 * @param stack_size    size of stack in bytes
 */
void stack_guard_init_thread(stack_guard_t *guard, uint64_t stack_base, size_t stack_size);

/**
 * Check if the stack canary is intact
 * @param guard     pointer to thread's stack guard
 * @param canary_location   address where canary was placed
 * @return          true if canary is intact
 */
bool stack_guard_check(const stack_guard_t *guard, const uint64_t *canary_location);

/**
 * Handle stack smashing detection
 * Called when canary corruption is detected
 * This function does not return - terminates the process
 * @param fault_addr    address of corruption
 */
void stack_guard_fail(uint64_t fault_addr) __attribute__((noreturn));

/**
 * Set custom stack overflow handler
 * @param handler   callback function for stack overflow
 */
void stack_guard_set_handler(stack_overflow_handler_t handler);

/**
 * Enable/disable stack guard features
 * @param flags     bitmask of STACK_GUARD_* values
 */
void stack_guard_set_flags(uint32_t flags);

/**
 * Get current stack guard configuration
 * @return current flags
 */
uint32_t stack_guard_get_flags(void);

// ============================================================================
// Guard Page Support
// ============================================================================

/**
 * Set up a guard page at the bottom of a stack
 * @param stack_bottom  lowest stack address
 * @return              0 on success, negative on failure
 */
int stack_guard_setup_page(uint64_t stack_bottom);

/**
 * Remove a guard page
 * @param stack_bottom  address of guard page
 */
void stack_guard_remove_page(uint64_t stack_bottom);

/**
 * Check if an address is in a guard page
 * @param addr  address to check
 * @return      true if in guard page
 */
bool stack_guard_is_guard_page(uint64_t addr);

// ============================================================================
// Stack Usage Monitoring
// ============================================================================

/**
 * Get current stack usage for a thread
 * @param guard     thread's stack guard
 * @param current_sp    current stack pointer
 * @return          bytes of stack used
 */
size_t stack_guard_get_usage(const stack_guard_t *guard, uint64_t current_sp);

/**
 * Get stack usage percentage
 * @param guard     thread's stack guard
 * @param current_sp    current stack pointer
 * @return          percentage of stack used (0-100)
 */
uint32_t stack_guard_get_usage_percent(const stack_guard_t *guard, uint64_t current_sp);

/**
 * Check if stack is near overflow (> 90% used)
 * @param guard     thread's stack guard
 * @param current_sp    current stack pointer
 * @return          true if stack is nearly full
 */
bool stack_guard_near_overflow(const stack_guard_t *guard, uint64_t current_sp);

// ============================================================================
// GCC Stack Protector Support
// ============================================================================

// This symbol is referenced by GCC's -fstack-protector
// It must be defined exactly as "__stack_chk_guard"
extern uint64_t __stack_chk_guard;

// This function is called by GCC when stack smashing is detected
extern void __stack_chk_fail(void) __attribute__((noreturn));

/**
 * Print stack guard statistics
 */
void stack_guard_print_info(void);

#endif // SECURITY_STACK_GUARD_H
