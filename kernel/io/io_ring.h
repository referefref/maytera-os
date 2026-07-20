// io_ring.h - Ring buffer structures and operations for async I/O
// Task #42 - Low-level ring buffer management
#ifndef IO_RING_H
#define IO_RING_H

#include "../types.h"
#include "async_io.h"

// ============================================================================
// Memory Barriers
// ============================================================================

// Compiler barrier (prevent reordering)
#define compiler_barrier() __asm__ volatile("" ::: "memory")

// Store fence (ensure all stores are visible)
#define smp_store_release(p, v) do {    \
    compiler_barrier();                  \
    *(volatile typeof(*(p)) *)(p) = (v); \
} while (0)

// Load fence (ensure all prior loads complete)
#define smp_load_acquire(p) ({          \
    typeof(*(p)) __v = *(volatile typeof(*(p)) *)(p); \
    compiler_barrier();                  \
    __v;                                 \
})

// Full memory barrier
#define smp_mb() __asm__ volatile("mfence" ::: "memory")

// Write memory barrier
#define smp_wmb() __asm__ volatile("sfence" ::: "memory")

// Read memory barrier
#define smp_rmb() __asm__ volatile("lfence" ::: "memory")

// ============================================================================
// Ring Buffer Helper Macros
// ============================================================================

// Check if ring is empty
#define ring_empty(head, tail) ((head) == (tail))

// Check if ring is full
#define ring_full(head, tail, mask) (((tail) - (head)) > (mask))

// Get number of entries in ring
#define ring_count(head, tail, mask) (((tail) - (head)) & (mask))

// Get available space in ring
#define ring_space(head, tail, mask) ((mask) + 1 - ring_count(head, tail, mask))

// Advance index with wrap
#define ring_advance(idx, mask) (((idx) + 1) & (mask))

// ============================================================================
// Submission Queue Operations
// ============================================================================

/**
 * Initialize submission queue ring
 * @param sq        Submission queue ring structure
 * @param entries   Number of entries (must be power of 2)
 * @param sqes      Array of SQE entries
 * @param sq_array  Array of SQE indices
 * @return          0 on success, negative error on failure
 */
int sq_ring_init(io_sq_ring_t *sq, uint32_t entries,
                 io_sqe_t *sqes, uint32_t *sq_array);

/**
 * Get next SQE slot for submission (userspace side)
 * @param sq        Submission queue ring
 * @param sqes      SQE array
 * @param sq_array  Index array
 * @return          Pointer to SQE, or NULL if full
 */
io_sqe_t *sq_ring_get_sqe(io_sq_ring_t *sq, io_sqe_t *sqes, uint32_t *sq_array);

/**
 * Submit entries to kernel (userspace side)
 * Advances tail to make entries visible to kernel
 * @param sq        Submission queue ring
 * @param count     Number of entries to submit
 */
void sq_ring_submit(io_sq_ring_t *sq, uint32_t count);

/**
 * Get number of entries ready for kernel processing
 * @param sq        Submission queue ring
 * @return          Number of entries
 */
uint32_t sq_ring_ready(io_sq_ring_t *sq);

/**
 * Consume SQE from kernel side
 * @param sq        Submission queue ring
 * @param sqes      SQE array
 * @param sq_array  Index array
 * @param sqe_out   Output: pointer to SQE
 * @return          0 on success, -1 if empty
 */
int sq_ring_consume(io_sq_ring_t *sq, io_sqe_t *sqes,
                    uint32_t *sq_array, io_sqe_t **sqe_out);

/**
 * Advance SQ head after kernel has processed entries
 * @param sq        Submission queue ring
 * @param count     Number of entries processed
 */
void sq_ring_advance(io_sq_ring_t *sq, uint32_t count);

// ============================================================================
// Completion Queue Operations
// ============================================================================

/**
 * Initialize completion queue ring
 * @param cq        Completion queue ring structure
 * @param entries   Number of entries (must be power of 2)
 * @param cqes      Array of CQE entries
 * @return          0 on success, negative error on failure
 */
int cq_ring_init(io_cq_ring_t *cq, uint32_t entries, io_cqe_t *cqes);

/**
 * Post completion to CQ (kernel side)
 * @param cq        Completion queue ring
 * @param cqes      CQE array
 * @param user_data User data to copy to CQE
 * @param result    Result value
 * @param flags     CQE flags
 * @return          0 on success, -1 if full (overflow)
 */
int cq_ring_post(io_cq_ring_t *cq, io_cqe_t *cqes,
                 uint64_t user_data, int32_t result, uint32_t flags);

/**
 * Get number of completions available (userspace side)
 * @param cq        Completion queue ring
 * @return          Number of completions
 */
uint32_t cq_ring_ready(io_cq_ring_t *cq);

/**
 * Peek at next CQE without consuming (userspace side)
 * @param cq        Completion queue ring
 * @param cqes      CQE array
 * @return          Pointer to CQE, or NULL if empty
 */
io_cqe_t *cq_ring_peek(io_cq_ring_t *cq, io_cqe_t *cqes);

/**
 * Consume CQE (userspace side)
 * @param cq        Completion queue ring
 * @param cqes      CQE array
 * @param cqe_out   Output: completion entry
 * @return          0 on success, -1 if empty
 */
int cq_ring_consume(io_cq_ring_t *cq, io_cqe_t *cqes, io_cqe_t *cqe_out);

/**
 * Advance CQ head after processing completions
 * @param cq        Completion queue ring
 * @param count     Number of entries processed
 */
void cq_ring_advance(io_cq_ring_t *cq, uint32_t count);

/**
 * Check if CQ has overflowed
 * @param cq        Completion queue ring
 * @return          Overflow count
 */
uint32_t cq_ring_overflow(io_cq_ring_t *cq);

// ============================================================================
// Ring Memory Management
// ============================================================================

/**
 * Calculate memory required for io_ring
 * @param sq_entries    Number of SQ entries
 * @param cq_entries    Number of CQ entries
 * @return              Total bytes needed
 */
size_t io_ring_calc_size(uint32_t sq_entries, uint32_t cq_entries);

/**
 * Allocate and initialize all ring structures
 * @param sq_entries    Number of SQ entries (power of 2)
 * @param cq_entries    Number of CQ entries (power of 2)
 * @param ring          Output: initialized ring
 * @return              0 on success, negative error on failure
 */
int io_ring_alloc(uint32_t sq_entries, uint32_t cq_entries, io_ring_t *ring);

/**
 * Free all ring structures
 * @param ring          Ring to free
 */
void io_ring_free(io_ring_t *ring);

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Round up to next power of 2
 * @param n     Value to round
 * @return      Next power of 2 >= n
 */
static inline uint32_t next_power_of_2(uint32_t n) {
    if (n == 0) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

/**
 * Check if value is power of 2
 * @param n     Value to check
 * @return      1 if power of 2, 0 otherwise
 */
static inline int is_power_of_2(uint32_t n) {
    return n && !(n & (n - 1));
}

/**
 * Prepare an SQE (zero and set opcode)
 * @param sqe       SQE to prepare
 * @param opcode    Operation type
 * @param fd        File descriptor
 * @param addr      Buffer address
 * @param len       Buffer length
 * @param offset    File offset
 */
static inline void io_sqe_prep(io_sqe_t *sqe, uint8_t opcode, int32_t fd,
                               void *addr, uint32_t len, uint64_t offset) {
    // Zero the structure
    uint64_t *p = (uint64_t *)sqe;
    for (int i = 0; i < 8; i++) p[i] = 0;

    sqe->opcode = opcode;
    sqe->fd = fd;
    sqe->addr = addr;
    sqe->len = len;
    sqe->off = offset;
}

/**
 * Set user data on SQE
 * @param sqe       SQE to modify
 * @param data      User data value
 */
static inline void io_sqe_set_data(io_sqe_t *sqe, uint64_t data) {
    sqe->user_data = data;
}

/**
 * Set flags on SQE
 * @param sqe       SQE to modify
 * @param flags     IOSQE_* flags
 */
static inline void io_sqe_set_flags(io_sqe_t *sqe, uint8_t flags) {
    sqe->flags = flags;
}

#endif // IO_RING_H
