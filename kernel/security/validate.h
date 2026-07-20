// validate.h - Pointer and Memory Validation for MayteraOS
#ifndef SECURITY_VALIDATE_H
#define SECURITY_VALIDATE_H

#include "../types.h"

// ============================================================================
// Memory Region Definitions
// These are defined here but can be overridden by vmm.h if included first
// ============================================================================

// Canonical address check - x86-64 uses 48-bit virtual addresses
// Bits 48-63 must be all 0 (user) or all 1 (kernel)
#ifndef CANONICAL_USER_MAX
#define CANONICAL_USER_MAX      0x00007FFFFFFFFFFFULL   // Highest user address
#endif

#ifndef CANONICAL_KERNEL_MIN
#define CANONICAL_KERNEL_MIN    0xFFFF800000000000ULL   // Lowest kernel address
#endif

// User space boundaries (use vmm.h values if available)
#ifndef USER_SPACE_START
#define USER_SPACE_START        0x0000000000400000ULL   // Match vmm.h (4MB)
#endif

#ifndef USER_SPACE_END
#define USER_SPACE_END          0x00007FFFFFFFFFFFULL   // Top of user space
#endif

// Kernel space boundaries
#ifndef KERNEL_SPACE_START
#define KERNEL_SPACE_START      0xFFFF800000000000ULL   // Start of kernel space
#endif

#ifndef KERNEL_SPACE_END
#define KERNEL_SPACE_END        0xFFFFFFFFFFFFFFFFULL   // End of address space
#endif

// Special regions (should not be accessed by user)
#ifndef NULL_PAGE_END
#define NULL_PAGE_END           0x0000000000001000ULL   // Null pointer guard page
#endif

#ifndef MMIO_REGION_START
#define MMIO_REGION_START       0x00000000FE000000ULL   // Memory-mapped I/O
#endif

#ifndef MMIO_REGION_END
#define MMIO_REGION_END         0x0000000100000000ULL
#endif

// ============================================================================
// Access Flags
// ============================================================================

#define ACCESS_READ             (1 << 0)    // Read access
#define ACCESS_WRITE            (1 << 1)    // Write access
#define ACCESS_EXEC             (1 << 2)    // Execute access
#define ACCESS_USER             (1 << 3)    // User-mode access (not kernel)
#define ACCESS_KERNEL           (1 << 4)    // Kernel-mode access

// Combined access patterns
#define ACCESS_READ_USER        (ACCESS_READ | ACCESS_USER)
#define ACCESS_WRITE_USER       (ACCESS_WRITE | ACCESS_USER)
#define ACCESS_RW_USER          (ACCESS_READ | ACCESS_WRITE | ACCESS_USER)
#define ACCESS_EXEC_USER        (ACCESS_EXEC | ACCESS_USER)

// ============================================================================
// Validation Error Codes
// ============================================================================

typedef enum {
    VALIDATE_OK = 0,            // Pointer is valid
    VALIDATE_NULL,              // Null pointer
    VALIDATE_UNALIGNED,         // Misaligned pointer
    VALIDATE_NON_CANONICAL,     // Non-canonical address
    VALIDATE_KERNEL_SPACE,      // Pointer in kernel space (from user)
    VALIDATE_UNMAPPED,          // Address not mapped
    VALIDATE_NO_READ,           // Page not readable
    VALIDATE_NO_WRITE,          // Page not writable
    VALIDATE_NO_EXEC,           // Page not executable
    VALIDATE_NO_USER,           // Page not user-accessible
    VALIDATE_OVERFLOW,          // Range overflows address space
    VALIDATE_MMIO,              // Pointer to MMIO region
    VALIDATE_STRING_UNTERMINATED, // String not null-terminated within limit
    VALIDATE_ARRAY_TOO_LARGE,   // Array size exceeds limit
} validate_error_t;

// ============================================================================
// Pointer Validation API
// ============================================================================

/**
 * Initialize the validation subsystem
 */
void validate_init(void);

/**
 * Validate a user-space pointer
 * @param ptr       pointer to validate
 * @param size      size of memory region in bytes
 * @param access    access flags (ACCESS_READ, ACCESS_WRITE, etc.)
 * @return          VALIDATE_OK if valid, error code otherwise
 */
validate_error_t validate_user_ptr(const void *ptr, size_t size, uint32_t access);

/**
 * Validate a user-space string
 * @param str       string pointer to validate
 * @param max_len   maximum string length (including null terminator)
 * @return          VALIDATE_OK if valid, error code otherwise
 */
validate_error_t validate_user_string(const char *str, size_t max_len);

/**
 * Validate a user-space array
 * @param arr       array pointer
 * @param count     number of elements
 * @param elem_size size of each element
 * @param access    access flags
 * @return          VALIDATE_OK if valid, error code otherwise
 */
validate_error_t validate_user_array(const void *arr, size_t count, size_t elem_size, uint32_t access);

/**
 * Validate a kernel pointer (for internal checks)
 * @param ptr       pointer to validate
 * @param size      size of memory region
 * @return          VALIDATE_OK if valid, error code otherwise
 */
validate_error_t validate_kernel_ptr(const void *ptr, size_t size);

/**
 * Check if an address is in user space
 * @param addr      address to check
 * @return          true if in user space
 */
bool is_user_address(uint64_t addr);

/**
 * Check if an address is in kernel space
 * @param addr      address to check
 * @return          true if in kernel space
 */
bool is_kernel_address(uint64_t addr);

/**
 * Check if an address is canonical
 * @param addr      address to check
 * @return          true if canonical
 */
bool is_canonical_address(uint64_t addr);

/**
 * Get error string for validation error code
 * @param error     error code
 * @return          human-readable error string
 */
const char *validate_error_string(validate_error_t error);

// ============================================================================
// Safe Copy Functions (copy_from_user / copy_to_user)
// ============================================================================

/**
 * Safely copy data from user space to kernel space
 * @param dest      kernel destination buffer
 * @param src       user source buffer
 * @param size      number of bytes to copy
 * @return          0 on success, negative error code on failure
 */
int copy_from_user(void *dest, const void *src, size_t size);

/**
 * Safely copy data from kernel space to user space
 * @param dest      user destination buffer
 * @param src       kernel source buffer
 * @param size      number of bytes to copy
 * @return          0 on success, negative error code on failure
 */
int copy_to_user(void *dest, const void *src, size_t size);

/**
 * Safely copy a string from user space
 * @param dest      kernel destination buffer
 * @param src       user source string
 * @param max_len   maximum length to copy (including null)
 * @return          actual string length on success, negative on failure
 */
ssize_t strncpy_from_user(char *dest, const char *src, size_t max_len);

/**
 * Get string length from user space
 * @param str       user string
 * @param max_len   maximum length to scan
 * @return          string length on success, negative on failure
 */
ssize_t strnlen_user(const char *str, size_t max_len);

/**
 * Clear user memory (set to zero)
 * @param dest      user destination buffer
 * @param size      number of bytes to clear
 * @return          0 on success, negative on failure
 */
int clear_user(void *dest, size_t size);

// ============================================================================
// Alignment Validation
// ============================================================================

/**
 * Check pointer alignment
 * @param ptr       pointer to check
 * @param alignment required alignment (must be power of 2)
 * @return          true if properly aligned
 */
static inline bool is_aligned(const void *ptr, size_t alignment) {
    return ((uint64_t)ptr & (alignment - 1)) == 0;
}

/**
 * Validate pointer alignment for specific type
 * @param ptr       pointer to check
 * @param type_size size of type (for alignment)
 * @return          VALIDATE_OK if aligned, VALIDATE_UNALIGNED otherwise
 */
validate_error_t validate_alignment(const void *ptr, size_t type_size);

// ============================================================================
// Convenience Macros for Syscall Validation
// ============================================================================

// Validate read-only user pointer, return -EFAULT on failure
#define VALIDATE_USER_READ(ptr, size) do { \
    validate_error_t _err = validate_user_ptr((ptr), (size), ACCESS_READ_USER); \
    if (_err != VALIDATE_OK) { \
        return -14; /* EFAULT */ \
    } \
} while (0)

// Validate read-write user pointer, return -EFAULT on failure
#define VALIDATE_USER_WRITE(ptr, size) do { \
    validate_error_t _err = validate_user_ptr((ptr), (size), ACCESS_RW_USER); \
    if (_err != VALIDATE_OK) { \
        return -14; /* EFAULT */ \
    } \
} while (0)

// Validate user string, return -EFAULT on failure
#define VALIDATE_USER_STRING(str, max) do { \
    validate_error_t _err = validate_user_string((str), (max)); \
    if (_err != VALIDATE_OK) { \
        return -14; /* EFAULT */ \
    } \
} while (0)

// Validate pointer is not null
#define VALIDATE_NOT_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        return -14; /* EFAULT */ \
    } \
} while (0)

#endif // SECURITY_VALIDATE_H
