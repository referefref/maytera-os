// security.h - Unified Security Module for MayteraOS
#ifndef SECURITY_H
#define SECURITY_H

#include "../types.h"
#include "aslr.h"
#include "validate.h"
#include "stack_guard.h"
#include "overflow.h"

// ============================================================================
// Security Module Version
// ============================================================================

#define SECURITY_VERSION_MAJOR  1
#define SECURITY_VERSION_MINOR  0
#define SECURITY_VERSION_PATCH  0

// ============================================================================
// Unified Security Initialization
// ============================================================================

/**
 * Initialize all security subsystems
 * Call this during kernel initialization
 */
void security_init(void);

/**
 * Print security status summary
 */
void security_print_status(void);

// ============================================================================
// Security Feature Flags
// ============================================================================

#define SECURITY_FEATURE_ASLR           (1 << 0)    // Address randomization
#define SECURITY_FEATURE_STACK_GUARD    (1 << 1)    // Stack canaries
#define SECURITY_FEATURE_PTR_VALIDATE   (1 << 2)    // Pointer validation
#define SECURITY_FEATURE_OVERFLOW_CHECK (1 << 3)    // Integer overflow checks
#define SECURITY_FEATURE_GUARD_PAGES    (1 << 4)    // Stack guard pages
#define SECURITY_FEATURE_NX             (1 << 5)    // No-execute pages
#define SECURITY_FEATURE_SMEP           (1 << 6)    // Supervisor Mode Exec Prevention
#define SECURITY_FEATURE_SMAP           (1 << 7)    // Supervisor Mode Access Prevention

#define SECURITY_FEATURE_ALL            0xFF

/**
 * Get enabled security features
 * @return bitmask of enabled features
 */
uint32_t security_get_features(void);

/**
 * Check if a security feature is enabled
 * @param feature   feature flag to check
 * @return          true if feature is enabled
 */
bool security_feature_enabled(uint32_t feature);

// ============================================================================
// CPU Security Features
// ============================================================================

/**
 * Check if SMEP is supported by CPU
 * @return true if supported
 */
bool cpu_has_smep(void);

/**
 * Check if SMAP is supported by CPU
 * @return true if supported
 */
bool cpu_has_smap(void);

/**
 * Enable SMEP (Supervisor Mode Execution Prevention)
 * Prevents kernel from executing user-space code
 */
void security_enable_smep(void);

/**
 * Enable SMAP (Supervisor Mode Access Prevention)
 * Prevents kernel from accessing user-space data
 */
void security_enable_smap(void);

/**
 * Temporarily disable SMAP for intentional user access
 * @return previous SMAP state
 */
bool smap_disable(void);

/**
 * Re-enable SMAP after intentional user access
 * @param previous  previous state from smap_disable()
 */
void smap_restore(bool previous);

// ============================================================================
// Secure Memory Operations
// ============================================================================

/**
 * Securely zero memory (not optimized away)
 * @param ptr   memory to zero
 * @param size  bytes to zero
 */
void secure_zero(void *ptr, size_t size);

/**
 * Compare memory in constant time
 * Prevents timing side-channel attacks
 * @param a     first buffer
 * @param b     second buffer
 * @param size  bytes to compare
 * @return      0 if equal, non-zero if different
 */
int secure_compare(const void *a, const void *b, size_t size);

// ============================================================================
// Process Security
// ============================================================================

// Forward declaration (defined in proc/process.h)
struct process;

/**
 * Initialize security state for a new process
 * Sets up ASLR, stack canaries, etc.
 * @param proc  process to initialize
 */
void security_init_process(struct process *proc);

/**
 * Clean up security state when process exits
 * @param proc  process exiting
 */
void security_cleanup_process(struct process *proc);

// ============================================================================
// Security Audit Logging
// ============================================================================

// Audit event types
typedef enum {
    AUDIT_SYSCALL_FAIL,         // Syscall validation failure
    AUDIT_STACK_SMASH,          // Stack smashing detected
    AUDIT_PTR_INVALID,          // Invalid pointer access
    AUDIT_OVERFLOW,             // Integer overflow detected
    AUDIT_PERMISSION_DENIED,    // Permission denied
    AUDIT_EXEC_VIOLATION,       // Execute permission violation
    AUDIT_MEMORY_VIOLATION,     // Memory access violation
} audit_event_t;

/**
 * Log a security audit event
 * @param event     event type
 * @param pid       process ID (0 for kernel)
 * @param detail    additional detail string
 */
void security_audit(audit_event_t event, uint32_t pid, const char *detail);

/**
 * Set audit logging level
 * @param level     0=off, 1=errors only, 2=warnings, 3=all
 */
void security_set_audit_level(int level);

#endif // SECURITY_H
