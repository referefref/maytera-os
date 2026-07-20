// security.c - Unified Security Module implementation for MayteraOS
#include "security.h"
#include "../serial.h"
#include "../string.h"

// ============================================================================
// Module State
// ============================================================================

static bool g_security_initialized = false;
static uint32_t g_security_features = 0;
static int g_audit_level = 2;  // Default: warnings and errors

// Audit log
#define AUDIT_LOG_SIZE 64
static struct {
    audit_event_t event;
    uint32_t pid;
    uint64_t timestamp;
    char detail[64];
} g_audit_log[AUDIT_LOG_SIZE];
static uint32_t g_audit_index = 0;
static uint64_t g_audit_count = 0;

// ============================================================================
// CPU Feature Detection
// ============================================================================

bool cpu_has_smep(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(7, &eax, &ebx, &ecx, &edx);
    return (ebx & (1 << 7)) != 0;  // SMEP bit
}

bool cpu_has_smap(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(7, &eax, &ebx, &ecx, &edx);
    return (ebx & (1 << 20)) != 0;  // SMAP bit
}

static bool cpu_has_nx(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    return (edx & (1 << 20)) != 0;  // NX bit
}

// ============================================================================
// Security Feature Initialization
// ============================================================================

void security_enable_smep(void) {
    if (!cpu_has_smep()) {
        kprintf("[SECURITY] SMEP not supported by CPU\n");
        return;
    }

    // Set SMEP bit (bit 20) in CR4
    uint64_t cr4 = read_cr4();
    cr4 |= (1ULL << 20);
    write_cr4(cr4);

    g_security_features |= SECURITY_FEATURE_SMEP;
    kprintf("[SECURITY] SMEP enabled - kernel cannot execute user code\n");
}

void security_enable_smap(void) {
    if (!cpu_has_smap()) {
        kprintf("[SECURITY] SMAP not supported by CPU\n");
        return;
    }

    // Set SMAP bit (bit 21) in CR4
    uint64_t cr4 = read_cr4();
    cr4 |= (1ULL << 21);
    write_cr4(cr4);

    g_security_features |= SECURITY_FEATURE_SMAP;
    kprintf("[SECURITY] SMAP enabled - kernel cannot access user data directly\n");
}

bool smap_disable(void) {
    if (!(g_security_features & SECURITY_FEATURE_SMAP)) {
        return false;
    }

    // Set AC flag in RFLAGS to temporarily allow user access
    __asm__ volatile("stac" ::: "cc");
    return true;
}

void smap_restore(bool previous) {
    if (previous) {
        // Clear AC flag to re-enable SMAP
        __asm__ volatile("clac" ::: "cc");
    }
}

// ============================================================================
// Unified Initialization
// ============================================================================

void security_init(void) {
    kprintf("\n");
    kprintf("============================================================\n");
    kprintf("  MayteraOS Security Subsystem v%d.%d.%d\n",
            SECURITY_VERSION_MAJOR, SECURITY_VERSION_MINOR, SECURITY_VERSION_PATCH);
    kprintf("============================================================\n");

    // Initialize ASLR
    aslr_init();
    if (aslr_enabled()) {
        g_security_features |= SECURITY_FEATURE_ASLR;
    }

    // Initialize stack guard
    stack_guard_init();
    g_security_features |= SECURITY_FEATURE_STACK_GUARD;

    // Initialize pointer validation
    validate_init();
    g_security_features |= SECURITY_FEATURE_PTR_VALIDATE;

    // Initialize overflow protection
    overflow_init();
    g_security_features |= SECURITY_FEATURE_OVERFLOW_CHECK;

    // Check and enable CPU security features
    if (cpu_has_nx()) {
        g_security_features |= SECURITY_FEATURE_NX;
        kprintf("[SECURITY] NX (No-Execute) supported\n");
    }

    // Enable SMEP if available
    if (cpu_has_smep()) {
        security_enable_smep();
    }

    // Enable SMAP if available
    if (cpu_has_smap()) {
        security_enable_smap();
    }

    g_security_initialized = true;

    kprintf("\n[SECURITY] Security subsystem initialization complete\n");
    kprintf("[SECURITY] Active features: 0x%x\n", g_security_features);
    kprintf("============================================================\n\n");
}

// ============================================================================
// Feature Queries
// ============================================================================

uint32_t security_get_features(void) {
    return g_security_features;
}

bool security_feature_enabled(uint32_t feature) {
    return (g_security_features & feature) != 0;
}

// ============================================================================
// Secure Memory Operations
// ============================================================================

// Volatile pointer prevents optimization
typedef void *(*volatile memset_ptr)(void *, int, size_t);
static memset_ptr secure_memset_ptr = memset;

void secure_zero(void *ptr, size_t size) {
    // Use volatile pointer to prevent compiler from optimizing away
    secure_memset_ptr(ptr, 0, size);

    // Memory barrier to ensure write completes
    __asm__ volatile("" ::: "memory");
}

int secure_compare(const void *a, const void *b, size_t size) {
    const volatile uint8_t *pa = (const volatile uint8_t *)a;
    const volatile uint8_t *pb = (const volatile uint8_t *)b;
    volatile uint8_t result = 0;

    // Compare all bytes, accumulating differences
    // This prevents early-exit timing attacks
    for (size_t i = 0; i < size; i++) {
        result |= pa[i] ^ pb[i];
    }

    return result;
}

// ============================================================================
// Process Security
// ============================================================================

void security_init_process(struct process *proc) {
    (void)proc;  // TODO: integrate with process structure

    // When process structure is extended:
    // 1. Initialize ASLR state
    // 2. Generate stack canary
    // 3. Set up guard pages
}

void security_cleanup_process(struct process *proc) {
    (void)proc;  // TODO: integrate with process structure

    // When process structure is extended:
    // 1. Clear sensitive data
    // 2. Remove guard pages
}

// ============================================================================
// Security Audit Logging
// ============================================================================

// External declaration for timer ticks
extern volatile uint64_t timer_ticks;

void security_audit(audit_event_t event, uint32_t pid, const char *detail) {
    if (g_audit_level == 0) return;

    // Log to buffer
    uint32_t idx = g_audit_index;
    g_audit_log[idx].event = event;
    g_audit_log[idx].pid = pid;
    g_audit_log[idx].timestamp = timer_ticks;

    if (detail) {
        strncpy(g_audit_log[idx].detail, detail, sizeof(g_audit_log[idx].detail) - 1);
        g_audit_log[idx].detail[sizeof(g_audit_log[idx].detail) - 1] = '\0';
    } else {
        g_audit_log[idx].detail[0] = '\0';
    }

    g_audit_index = (g_audit_index + 1) % AUDIT_LOG_SIZE;
    g_audit_count++;

    // Print based on audit level
    const char *event_name;
    int severity;

    switch (event) {
        case AUDIT_STACK_SMASH:
        case AUDIT_MEMORY_VIOLATION:
        case AUDIT_EXEC_VIOLATION:
            event_name = "CRITICAL";
            severity = 1;
            break;
        case AUDIT_SYSCALL_FAIL:
        case AUDIT_PTR_INVALID:
        case AUDIT_PERMISSION_DENIED:
            event_name = "WARNING";
            severity = 2;
            break;
        case AUDIT_OVERFLOW:
        default:
            event_name = "INFO";
            severity = 3;
            break;
    }

    if (severity <= g_audit_level) {
        kprintf("[AUDIT] %s: pid=%u", event_name, pid);
        if (detail) {
            kprintf(" - %s", detail);
        }
        kprintf("\n");
    }
}

void security_set_audit_level(int level) {
    g_audit_level = level;
}

// ============================================================================
// Status Reporting
// ============================================================================

void security_print_status(void) {
    kprintf("\n");
    kprintf("============================================================\n");
    kprintf("  MayteraOS Security Status\n");
    kprintf("============================================================\n");
    kprintf("\n");

    kprintf("Security Features:\n");
    kprintf("  ASLR:             %s\n", (g_security_features & SECURITY_FEATURE_ASLR) ? "Enabled" : "Disabled");
    kprintf("  Stack Guard:      %s\n", (g_security_features & SECURITY_FEATURE_STACK_GUARD) ? "Enabled" : "Disabled");
    kprintf("  Ptr Validation:   %s\n", (g_security_features & SECURITY_FEATURE_PTR_VALIDATE) ? "Enabled" : "Disabled");
    kprintf("  Overflow Check:   %s\n", (g_security_features & SECURITY_FEATURE_OVERFLOW_CHECK) ? "Enabled" : "Disabled");
    kprintf("  NX (No-Execute):  %s\n", (g_security_features & SECURITY_FEATURE_NX) ? "Supported" : "Not Available");
    kprintf("  SMEP:             %s\n", (g_security_features & SECURITY_FEATURE_SMEP) ? "Enabled" : "Not Available");
    kprintf("  SMAP:             %s\n", (g_security_features & SECURITY_FEATURE_SMAP) ? "Enabled" : "Not Available");
    kprintf("\n");

    // Print subsystem details
    aslr_print_info();
    kprintf("\n");
    stack_guard_print_info();
    kprintf("\n");
    overflow_print_info();

    kprintf("\nAudit Statistics:\n");
    kprintf("  Total events:     %lu\n", g_audit_count);
    kprintf("  Audit level:      %d\n", g_audit_level);

    kprintf("\n============================================================\n");
}
