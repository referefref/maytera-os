// overflow.h - Integer Overflow Protection for MayteraOS
#ifndef SECURITY_OVERFLOW_H
#define SECURITY_OVERFLOW_H

#include "../types.h"

// ============================================================================
// Safe Arithmetic Macros
// ============================================================================

// These macros perform arithmetic with overflow checking
// They return true if overflow occurred, false otherwise
// The result is stored in the pointed-to variable

/**
 * Safe addition (a + b) with overflow check
 * @param a         first operand
 * @param b         second operand
 * @param result    pointer to store result
 * @return          true if overflow occurred
 */
#define SAFE_ADD(a, b, result) \
    __builtin_add_overflow((a), (b), (result))

/**
 * Safe subtraction (a - b) with underflow check
 * @param a         first operand
 * @param b         second operand
 * @param result    pointer to store result
 * @return          true if underflow occurred
 */
#define SAFE_SUB(a, b, result) \
    __builtin_sub_overflow((a), (b), (result))

/**
 * Safe multiplication (a * b) with overflow check
 * @param a         first operand
 * @param b         second operand
 * @param result    pointer to store result
 * @return          true if overflow occurred
 */
#define SAFE_MUL(a, b, result) \
    __builtin_mul_overflow((a), (b), (result))

// ============================================================================
// Safe Arithmetic Functions
// ============================================================================

/**
 * Safe 64-bit addition
 * @param a         first operand
 * @param b         second operand
 * @param result    pointer to store result
 * @return          true if overflow occurred
 */
static inline bool safe_add64(uint64_t a, uint64_t b, uint64_t *result) {
    return __builtin_add_overflow(a, b, result);
}

/**
 * Safe 64-bit subtraction
 * @param a         first operand
 * @param b         second operand
 * @param result    pointer to store result
 * @return          true if underflow occurred
 */
static inline bool safe_sub64(uint64_t a, uint64_t b, uint64_t *result) {
    return __builtin_sub_overflow(a, b, result);
}

/**
 * Safe 64-bit multiplication
 * @param a         first operand
 * @param b         second operand
 * @param result    pointer to store result
 * @return          true if overflow occurred
 */
static inline bool safe_mul64(uint64_t a, uint64_t b, uint64_t *result) {
    return __builtin_mul_overflow(a, b, result);
}

/**
 * Safe 32-bit addition
 * @param a         first operand
 * @param b         second operand
 * @param result    pointer to store result
 * @return          true if overflow occurred
 */
static inline bool safe_add32(uint32_t a, uint32_t b, uint32_t *result) {
    return __builtin_add_overflow(a, b, result);
}

/**
 * Safe 32-bit subtraction
 * @param a         first operand
 * @param b         second operand
 * @param result    pointer to store result
 * @return          true if underflow occurred
 */
static inline bool safe_sub32(uint32_t a, uint32_t b, uint32_t *result) {
    return __builtin_sub_overflow(a, b, result);
}

/**
 * Safe 32-bit multiplication
 * @param a         first operand
 * @param b         second operand
 * @param result    pointer to store result
 * @return          true if overflow occurred
 */
static inline bool safe_mul32(uint32_t a, uint32_t b, uint32_t *result) {
    return __builtin_mul_overflow(a, b, result);
}

/**
 * Safe signed 64-bit addition
 * @param a         first operand
 * @param b         second operand
 * @param result    pointer to store result
 * @return          true if overflow occurred
 */
static inline bool safe_add_s64(int64_t a, int64_t b, int64_t *result) {
    return __builtin_add_overflow(a, b, result);
}

/**
 * Safe signed 64-bit subtraction
 * @param a         first operand
 * @param b         second operand
 * @param result    pointer to store result
 * @return          true if overflow occurred
 */
static inline bool safe_sub_s64(int64_t a, int64_t b, int64_t *result) {
    return __builtin_sub_overflow(a, b, result);
}

/**
 * Safe signed 64-bit multiplication
 * @param a         first operand
 * @param b         second operand
 * @param result    pointer to store result
 * @return          true if overflow occurred
 */
static inline bool safe_mul_s64(int64_t a, int64_t b, int64_t *result) {
    return __builtin_mul_overflow(a, b, result);
}

// ============================================================================
// Size/Dimension Validation
// ============================================================================

// Maximum reasonable values for various dimensions
#define MAX_WINDOW_WIDTH        16384       // 16K pixels
#define MAX_WINDOW_HEIGHT       16384       // 16K pixels
#define MAX_BUFFER_SIZE         (256 * MB)  // 256 MB
#define MAX_ARRAY_ELEMENTS      (1024 * 1024 * 1024)  // 1 billion elements
#define MAX_STRING_LENGTH       (16 * MB)   // 16 MB string

/**
 * Validate window dimensions
 * @param width     window width
 * @param height    window height
 * @param bpp       bits per pixel (8, 16, 24, 32)
 * @return          true if dimensions are valid
 */
static inline bool validate_window_dimensions(uint32_t width, uint32_t height, uint32_t bpp) {
    // Check individual dimension limits
    if (width == 0 || height == 0) return false;
    if (width > MAX_WINDOW_WIDTH) return false;
    if (height > MAX_WINDOW_HEIGHT) return false;
    if (bpp == 0 || bpp > 32) return false;

    // Check for multiplication overflow in buffer size calculation
    uint64_t row_bytes;
    uint64_t total_bytes;

    // bytes per row = width * (bpp / 8)
    if (safe_mul64(width, (bpp + 7) / 8, &row_bytes)) return false;

    // total bytes = row_bytes * height
    if (safe_mul64(row_bytes, height, &total_bytes)) return false;

    // Check against maximum buffer size
    if (total_bytes > MAX_BUFFER_SIZE) return false;

    return true;
}

/**
 * Validate buffer allocation size
 * @param count     number of elements
 * @param elem_size size of each element
 * @param total     pointer to store total size
 * @return          true if allocation size is valid
 */
static inline bool validate_alloc_size(size_t count, size_t elem_size, size_t *total) {
    // Check for zero
    if (count == 0 || elem_size == 0) {
        if (total) *total = 0;
        return true;  // Zero allocation is valid
    }

    // Check element count
    if (count > MAX_ARRAY_ELEMENTS) return false;

    // Check for multiplication overflow
    size_t result;
    if (safe_mul64(count, elem_size, &result)) return false;

    // Check against maximum buffer size
    if (result > MAX_BUFFER_SIZE) return false;

    if (total) *total = result;
    return true;
}

/**
 * Validate array index
 * @param index     array index
 * @param count     array element count
 * @return          true if index is valid
 */
static inline bool validate_index(size_t index, size_t count) {
    return index < count;
}

/**
 * Validate range within buffer
 * @param offset    start offset
 * @param length    length of range
 * @param total     total buffer size
 * @return          true if range is valid
 */
static inline bool validate_range(size_t offset, size_t length, size_t total) {
    size_t end;
    if (safe_add64(offset, length, &end)) return false;
    return end <= total;
}

// ============================================================================
// Rectangle/Region Validation
// ============================================================================

// Rectangle structure for validation
typedef struct {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} rect_t;

/**
 * Validate a rectangle is within bounds
 * @param rect      rectangle to validate
 * @param max_width maximum allowed width (e.g., screen width)
 * @param max_height maximum allowed height
 * @return          true if rectangle is valid and within bounds
 */
static inline bool validate_rect(const rect_t *rect, uint32_t max_width, uint32_t max_height) {
    if (!rect) return false;

    // Check for negative coordinates that would wrap
    if (rect->x < 0 || rect->y < 0) return false;

    // Check dimensions
    if (rect->width == 0 || rect->height == 0) return false;
    if (rect->width > max_width || rect->height > max_height) return false;

    // Check that rectangle doesn't overflow
    uint32_t right, bottom;
    if (safe_add32((uint32_t)rect->x, rect->width, &right)) return false;
    if (safe_add32((uint32_t)rect->y, rect->height, &bottom)) return false;

    // Check bounds
    if (right > max_width || bottom > max_height) return false;

    return true;
}

/**
 * Validate rectangle intersection calculation won't overflow
 * @param r1        first rectangle
 * @param r2        second rectangle
 * @return          true if intersection can be safely calculated
 */
static inline bool validate_rect_intersect(const rect_t *r1, const rect_t *r2) {
    if (!r1 || !r2) return false;

    // Just check that both rectangles have valid non-overflow dimensions
    uint32_t dummy;
    if (safe_add32((uint32_t)r1->x, r1->width, &dummy)) return false;
    if (safe_add32((uint32_t)r1->y, r1->height, &dummy)) return false;
    if (safe_add32((uint32_t)r2->x, r2->width, &dummy)) return false;
    if (safe_add32((uint32_t)r2->y, r2->height, &dummy)) return false;

    return true;
}

// ============================================================================
// Convenience Macros for Syscall Size Validation
// ============================================================================

// Validate size parameter, return -EINVAL on failure
#define VALIDATE_SIZE(size, max) do { \
    if ((size) > (max)) { \
        return -22; /* EINVAL */ \
    } \
} while (0)

// Validate allocation size, return -ENOMEM on overflow
#define VALIDATE_ALLOC(count, elem_size, total_ptr) do { \
    if (!validate_alloc_size((count), (elem_size), (total_ptr))) { \
        return -12; /* ENOMEM */ \
    } \
} while (0)

// Validate window dimensions, return -EINVAL on failure
#define VALIDATE_WINDOW_DIM(width, height, bpp) do { \
    if (!validate_window_dimensions((width), (height), (bpp))) { \
        return -22; /* EINVAL */ \
    } \
} while (0)

// Validate range within buffer, return -EINVAL on failure
#define VALIDATE_RANGE(offset, length, total) do { \
    if (!validate_range((offset), (length), (total))) { \
        return -22; /* EINVAL */ \
    } \
} while (0)

// ============================================================================
// Overflow Statistics
// ============================================================================

/**
 * Initialize overflow protection subsystem
 */
void overflow_init(void);

/**
 * Report an overflow detection (for statistics)
 * @param location  string describing where overflow was detected
 */
void overflow_report(const char *location);

/**
 * Get count of detected overflows
 * @return number of overflows detected
 */
uint64_t overflow_get_count(void);

/**
 * Print overflow protection statistics
 */
void overflow_print_info(void);

#endif // SECURITY_OVERFLOW_H
