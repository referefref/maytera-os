// stack_guard.c - Stack Canary Protection implementation for MayteraOS
#include "stack_guard.h"
#include "aslr.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/vmm.h"

// ============================================================================
// Module State
// ============================================================================

// Global canary value (used by GCC's -fstack-protector)
uint64_t __stack_chk_guard = STACK_CANARY_MAGIC;

// Stack guard configuration
static uint32_t g_stack_guard_flags = STACK_GUARD_DEFAULT;
static bool g_stack_guard_initialized = false;
static stack_overflow_handler_t g_overflow_handler = NULL;

// Statistics
static uint64_t g_stack_checks_performed = 0;
static uint64_t g_stack_smashes_detected = 0;
static uint64_t g_guard_pages_allocated = 0;

// ============================================================================
// Initialization
// ============================================================================

void stack_guard_init(void) {
    kprintf("[STACK_GUARD] Initializing stack protection...\n");

    // Generate a random canary value
    uint64_t canary = stack_guard_generate_canary();
    __stack_chk_guard = canary;

    g_stack_guard_initialized = true;

    kprintf("[STACK_GUARD] Global canary set: 0x%016lx\n", __stack_chk_guard);
    kprintf("[STACK_GUARD] Stack protection enabled with flags: 0x%x\n", g_stack_guard_flags);
}

// ============================================================================
// Canary Generation
// ============================================================================

uint64_t stack_guard_generate_canary(void) {
    uint64_t canary;

    // Try to get random value from ASLR subsystem
    canary = aslr_get_random();

    // Apply protective characters in specific byte positions
    // This makes it harder to exploit with string operations

    uint8_t *bytes = (uint8_t *)&canary;

    // Include a null terminator (byte 0)
    // This stops string-based overflows
    if (g_stack_guard_flags & STACK_GUARD_TERMINATOR) {
        bytes[0] = STACK_CANARY_TERMINATOR;  // 0x00

        // Also include newline and carriage return to catch line-based overflows
        bytes[1] = STACK_CANARY_NEWLINE;     // 0x0A
        bytes[7] = STACK_CANARY_CR;          // 0x0D
    }

    return canary;
}

uint64_t stack_guard_get_canary(void) {
    return __stack_chk_guard;
}

// ============================================================================
// Thread Stack Guard
// ============================================================================

void stack_guard_init_thread(stack_guard_t *guard, uint64_t stack_base, size_t stack_size) {
    if (!guard) return;

    // Generate per-thread canary if random canaries are enabled
    if (g_stack_guard_flags & STACK_GUARD_RANDOM_CANARY) {
        guard->canary = stack_guard_generate_canary();
    } else {
        guard->canary = __stack_chk_guard;
    }

    guard->stack_base = stack_base;
    guard->stack_limit = stack_base - stack_size;
    guard->guard_page = 0;
    guard->guard_page_enabled = false;
}

bool stack_guard_check(const stack_guard_t *guard, const uint64_t *canary_location) {
    g_stack_checks_performed++;

    if (!guard || !canary_location) {
        return false;
    }

    // Check if canary matches
    if (*canary_location != guard->canary) {
        g_stack_smashes_detected++;
        return false;
    }

    return true;
}

// ============================================================================
// Stack Smashing Handler
// ============================================================================

void stack_guard_fail(uint64_t fault_addr) {
    g_stack_smashes_detected++;

    // Disable interrupts to prevent further damage
    cli();

    kprintf("\n");
    kprintf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    kprintf("!!!                    STACK SMASHING DETECTED                  !!!\n");
    kprintf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    kprintf("\n");
    kprintf("  Fault address: 0x%016lx\n", fault_addr);
    kprintf("  Canary value:  0x%016lx (expected)\n", __stack_chk_guard);
    kprintf("\n");
    kprintf("  This indicates a buffer overflow or stack corruption.\n");
    kprintf("  The system will halt to prevent further damage.\n");
    kprintf("\n");

    // Call custom handler if set
    if (g_overflow_handler) {
        g_overflow_handler(0, "unknown", fault_addr);  // TODO: get actual pid/name
    }

    // Halt the system
    kprintf("System halted.\n");
    for (;;) {
        hlt();
    }
}

// GCC stack protector failure function
void __stack_chk_fail(void) {
    // Get approximate fault address from stack
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));

    stack_guard_fail(rsp);
}

// ============================================================================
// Configuration
// ============================================================================

void stack_guard_set_handler(stack_overflow_handler_t handler) {
    g_overflow_handler = handler;
}

void stack_guard_set_flags(uint32_t flags) {
    g_stack_guard_flags = flags;
}

uint32_t stack_guard_get_flags(void) {
    return g_stack_guard_flags;
}

// ============================================================================
// Guard Pages
// ============================================================================

int stack_guard_setup_page(uint64_t stack_bottom) {
    // The guard page is at the very bottom of the stack
    // We mark it as not present so any access causes a page fault
    uint64_t guard_addr = ALIGN_DOWN(stack_bottom, PAGE_SIZE);

    // Unmap the guard page (or mark as not present)
    vmm_unmap_page(guard_addr);

    g_guard_pages_allocated++;

    kprintf("[STACK_GUARD] Guard page set up at 0x%lx\n", guard_addr);
    return 0;
}

void stack_guard_remove_page(uint64_t stack_bottom) {
    // Would need to remap the page - typically not done
    // as guard pages remain until thread exits
    (void)stack_bottom;
}

bool stack_guard_is_guard_page(uint64_t addr) {
    // Check if address is in an unmapped guard page region
    // This would need to track allocated guard pages
    // For now, check if address is unmapped and in stack region
    uint64_t page = ALIGN_DOWN(addr, PAGE_SIZE);
    return !vmm_is_mapped(page);
}

// ============================================================================
// Stack Usage Monitoring
// ============================================================================

size_t stack_guard_get_usage(const stack_guard_t *guard, uint64_t current_sp) {
    if (!guard) return 0;

    // Stack grows down, so usage = base - current_sp
    if (current_sp >= guard->stack_base) {
        return 0;  // Stack pointer above base (shouldn't happen)
    }
    if (current_sp < guard->stack_limit) {
        return guard->stack_base - guard->stack_limit;  // Overflow
    }

    return guard->stack_base - current_sp;
}

uint32_t stack_guard_get_usage_percent(const stack_guard_t *guard, uint64_t current_sp) {
    if (!guard) return 0;

    size_t total = guard->stack_base - guard->stack_limit;
    if (total == 0) return 0;

    size_t used = stack_guard_get_usage(guard, current_sp);

    return (uint32_t)((used * 100) / total);
}

bool stack_guard_near_overflow(const stack_guard_t *guard, uint64_t current_sp) {
    return stack_guard_get_usage_percent(guard, current_sp) >= 90;
}

// ============================================================================
// Debug / Info
// ============================================================================

void stack_guard_print_info(void) {
    kprintf("[STACK_GUARD] Stack Protection Status:\n");
    kprintf("  Initialized:      %s\n", g_stack_guard_initialized ? "Yes" : "No");
    kprintf("  Global canary:    0x%016lx\n", __stack_chk_guard);
    kprintf("  Flags:            0x%x\n", g_stack_guard_flags);
    kprintf("    Random canary:  %s\n", (g_stack_guard_flags & STACK_GUARD_RANDOM_CANARY) ? "Yes" : "No");
    kprintf("    Terminator:     %s\n", (g_stack_guard_flags & STACK_GUARD_TERMINATOR) ? "Yes" : "No");
    kprintf("    Kernel stacks:  %s\n", (g_stack_guard_flags & STACK_GUARD_KERNEL) ? "Yes" : "No");
    kprintf("    User stacks:    %s\n", (g_stack_guard_flags & STACK_GUARD_USER) ? "Yes" : "No");
    kprintf("  Statistics:\n");
    kprintf("    Checks:         %lu\n", g_stack_checks_performed);
    kprintf("    Smashes:        %lu\n", g_stack_smashes_detected);
    kprintf("    Guard pages:    %lu\n", g_guard_pages_allocated);
}
