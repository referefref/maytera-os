// overflow.c - Integer Overflow Protection implementation for MayteraOS
#include "overflow.h"
#include "../serial.h"

// ============================================================================
// Module State
// ============================================================================

static bool g_overflow_initialized = false;
static uint64_t g_overflow_count = 0;

// Track last few overflow locations for debugging
#define OVERFLOW_LOG_SIZE 16
static const char *g_overflow_log[OVERFLOW_LOG_SIZE];
static uint32_t g_overflow_log_index = 0;

// ============================================================================
// Initialization
// ============================================================================

void overflow_init(void) {
    g_overflow_initialized = true;
    g_overflow_count = 0;
    g_overflow_log_index = 0;

    // Clear log
    for (int i = 0; i < OVERFLOW_LOG_SIZE; i++) {
        g_overflow_log[i] = NULL;
    }

    kprintf("[OVERFLOW] Integer overflow protection initialized\n");
}

// ============================================================================
// Overflow Reporting
// ============================================================================

void overflow_report(const char *location) {
    g_overflow_count++;

    // Log the location
    g_overflow_log[g_overflow_log_index] = location;
    g_overflow_log_index = (g_overflow_log_index + 1) % OVERFLOW_LOG_SIZE;

    // Print warning
    kprintf("[OVERFLOW] WARNING: Integer overflow detected");
    if (location) {
        kprintf(" at %s", location);
    }
    kprintf(" (total: %lu)\n", g_overflow_count);
}

uint64_t overflow_get_count(void) {
    return g_overflow_count;
}

// ============================================================================
// Debug / Info
// ============================================================================

void overflow_print_info(void) {
    kprintf("[OVERFLOW] Integer Overflow Protection Status:\n");
    kprintf("  Initialized:        %s\n", g_overflow_initialized ? "Yes" : "No");
    kprintf("  Overflows detected: %lu\n", g_overflow_count);
    kprintf("  Max window size:    %dx%d\n", MAX_WINDOW_WIDTH, MAX_WINDOW_HEIGHT);
    kprintf("  Max buffer size:    %lu MB\n", MAX_BUFFER_SIZE / MB);
    kprintf("  Max array elements: %lu\n", MAX_ARRAY_ELEMENTS);

    if (g_overflow_count > 0) {
        kprintf("  Recent overflow locations:\n");
        for (int i = 0; i < OVERFLOW_LOG_SIZE; i++) {
            int idx = (g_overflow_log_index + i) % OVERFLOW_LOG_SIZE;
            if (g_overflow_log[idx]) {
                kprintf("    - %s\n", g_overflow_log[idx]);
            }
        }
    }
}
