// aio.h - User-space async I/O wrapper for MayteraOS
// Task #42 - Provides convenient interface for io_uring-like operations
#ifndef _LIBC_AIO_H
#define _LIBC_AIO_H

// Standard types (assuming userspace has these defined elsewhere,
// otherwise include appropriate headers)
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef long long          int64_t;
typedef int                int32_t;
typedef unsigned long      size_t;

// ============================================================================
// Operation Types
// ============================================================================

#define IORING_OP_NOP           0
#define IORING_OP_READ          1
#define IORING_OP_WRITE         2
#define IORING_OP_READV         3
#define IORING_OP_WRITEV        4
#define IORING_OP_FSYNC         5
#define IORING_OP_POLL_ADD      6
#define IORING_OP_POLL_REMOVE   7
#define IORING_OP_TIMEOUT       8
#define IORING_OP_TIMEOUT_REMOVE 9
#define IORING_OP_ACCEPT        10
#define IORING_OP_CONNECT       11
#define IORING_OP_SEND          12
#define IORING_OP_RECV          13
#define IORING_OP_OPENAT        14
#define IORING_OP_CLOSE         15
#define IORING_OP_STATX         16

// ============================================================================
// SQE Flags
// ============================================================================

#define IOSQE_FIXED_FILE    (1 << 0)
#define IOSQE_IO_DRAIN      (1 << 1)
#define IOSQE_IO_LINK       (1 << 2)
#define IOSQE_IO_HARDLINK   (1 << 3)
#define IOSQE_ASYNC         (1 << 4)
#define IOSQE_BUFFER_SELECT (1 << 5)

// ============================================================================
// CQE Flags
// ============================================================================

#define IOCQE_F_BUFFER      (1 << 0)
#define IOCQE_F_MORE        (1 << 1)

// ============================================================================
// Setup Flags
// ============================================================================

#define IORING_SETUP_IOPOLL     (1 << 0)
#define IORING_SETUP_SQPOLL     (1 << 1)
#define IORING_SETUP_SQ_AFF     (1 << 2)
#define IORING_SETUP_CQSIZE     (1 << 3)
#define IORING_SETUP_CLAMP      (1 << 4)

// ============================================================================
// Error Codes
// ============================================================================

#define AIO_SUCCESS         0
#define AIO_ERR_NOMEM      -1
#define AIO_ERR_INVAL      -2
#define AIO_ERR_NOSPC      -3
#define AIO_ERR_NOENT      -4
#define AIO_ERR_BUSY       -5
#define AIO_ERR_PERM       -6
#define AIO_ERR_BADF       -7
#define AIO_ERR_AGAIN      -8
#define AIO_ERR_INTR       -9
#define AIO_ERR_TIMEOUT    -10
#define AIO_ERR_OVERFLOW   -11
#define AIO_ERR_SHUTDOWN   -12

// ============================================================================
// Structures
// ============================================================================

// I/O vector for scatter/gather
typedef struct {
    void    *iov_base;
    size_t   iov_len;
} aio_iovec_t;

// Submission Queue Entry (matches kernel io_sqe_t)
typedef struct {
    uint8_t     opcode;
    uint8_t     flags;
    uint16_t    ioprio;
    int32_t     fd;

    union {
        uint64_t    off;
        uint64_t    addr2;
    };

    union {
        void       *addr;
        aio_iovec_t *iovecs;
        uint64_t    splice_off_in;
    };

    uint32_t    len;

    union {
        uint32_t    rw_flags;
        uint32_t    fsync_flags;
        uint32_t    poll_events;
        uint32_t    sync_range_flags;
        uint32_t    msg_flags;
        uint32_t    timeout_flags;
        uint32_t    accept_flags;
        uint32_t    open_flags;
        uint32_t    statx_flags;
    };

    uint64_t    user_data;
    uint8_t     __pad[16];
} __attribute__((packed, aligned(64))) aio_sqe_t;

// Completion Queue Entry (matches kernel io_cqe_t)
typedef struct {
    uint64_t    user_data;
    int32_t     res;
    uint32_t    flags;
} __attribute__((packed)) aio_cqe_t;

// Ring context (opaque handle for userspace)
typedef struct aio_ring {
    int32_t     fd;             // Ring file descriptor
    uint32_t    sq_entries;     // Number of SQ entries
    uint32_t    cq_entries;     // Number of CQ entries

    // These would be mmap'd in a full implementation
    // For now, they're placeholders
    volatile uint32_t *sq_head;
    volatile uint32_t *sq_tail;
    volatile uint32_t *cq_head;
    volatile uint32_t *cq_tail;
    aio_sqe_t   *sqes;
    aio_cqe_t   *cqes;
    uint32_t    *sq_array;

    // Local tracking
    uint32_t    sq_mask;
    uint32_t    cq_mask;
} aio_ring_t;

// ============================================================================
// Ring Management
// ============================================================================

/**
 * Create a new async I/O ring
 * @param entries       Number of submission queue entries
 * @param cq_entries    Number of completion queue entries (0 = auto)
 * @param ring          Output: ring context
 * @return              0 on success, negative error code on failure
 */
static inline int aio_queue_init(uint32_t entries, uint32_t cq_entries,
                                 aio_ring_t *ring) {
    if (!ring) return AIO_ERR_INVAL;

    // Syscall to create ring (would be implemented via syscall instruction)
    // For now, this is a placeholder that shows the intended API
    int64_t fd;

    // In real implementation:
    // fd = syscall(SYS_IO_SETUP, entries, cq_entries);
    (void)entries;
    (void)cq_entries;
    fd = -1;  // Placeholder

    if (fd < 0) {
        return (int)fd;
    }

    ring->fd = (int32_t)fd;
    ring->sq_entries = entries;
    ring->cq_entries = cq_entries > 0 ? cq_entries : entries * 2;
    ring->sq_mask = ring->sq_entries - 1;
    ring->cq_mask = ring->cq_entries - 1;

    // In real implementation, mmap the ring buffers here
    ring->sq_head = (void*)0;
    ring->sq_tail = (void*)0;
    ring->cq_head = (void*)0;
    ring->cq_tail = (void*)0;
    ring->sqes = (void*)0;
    ring->cqes = (void*)0;
    ring->sq_array = (void*)0;

    return AIO_SUCCESS;
}

/**
 * Destroy an async I/O ring
 * @param ring          Ring to destroy
 */
static inline void aio_queue_exit(aio_ring_t *ring) {
    if (!ring || ring->fd < 0) return;

    // Syscall to destroy ring
    // syscall(SYS_IO_DESTROY, ring->fd);

    ring->fd = -1;
}

// ============================================================================
// SQE Preparation Helpers
// ============================================================================

/**
 * Get next available SQE
 * @param ring          Ring context
 * @return              Pointer to SQE, or NULL if queue full
 */
static inline aio_sqe_t *aio_get_sqe(aio_ring_t *ring) {
    if (!ring || !ring->sqes) return (void*)0;

    // In real implementation, check if queue is full
    // uint32_t head = *ring->sq_head;
    // uint32_t tail = *ring->sq_tail;
    // if ((tail - head) > ring->sq_mask) return NULL;

    // Return next SQE slot
    // uint32_t idx = ring->sq_array[tail & ring->sq_mask];
    // return &ring->sqes[idx];

    return (void*)0;  // Placeholder
}

/**
 * Zero an SQE
 */
static inline void aio_sqe_zero(aio_sqe_t *sqe) {
    uint64_t *p = (uint64_t *)sqe;
    for (int i = 0; i < 8; i++) p[i] = 0;
}

/**
 * Prepare a read operation
 */
static inline void aio_prep_read(aio_sqe_t *sqe, int fd, void *buf,
                                 uint32_t len, uint64_t offset) {
    aio_sqe_zero(sqe);
    sqe->opcode = IORING_OP_READ;
    sqe->fd = fd;
    sqe->addr = buf;
    sqe->len = len;
    sqe->off = offset;
}

/**
 * Prepare a write operation
 */
static inline void aio_prep_write(aio_sqe_t *sqe, int fd, const void *buf,
                                  uint32_t len, uint64_t offset) {
    aio_sqe_zero(sqe);
    sqe->opcode = IORING_OP_WRITE;
    sqe->fd = fd;
    sqe->addr = (void *)buf;
    sqe->len = len;
    sqe->off = offset;
}

/**
 * Prepare a vectored read operation
 */
static inline void aio_prep_readv(aio_sqe_t *sqe, int fd, aio_iovec_t *iov,
                                  uint32_t iovcnt, uint64_t offset) {
    aio_sqe_zero(sqe);
    sqe->opcode = IORING_OP_READV;
    sqe->fd = fd;
    sqe->iovecs = iov;
    sqe->len = iovcnt;
    sqe->off = offset;
}

/**
 * Prepare a vectored write operation
 */
static inline void aio_prep_writev(aio_sqe_t *sqe, int fd, aio_iovec_t *iov,
                                   uint32_t iovcnt, uint64_t offset) {
    aio_sqe_zero(sqe);
    sqe->opcode = IORING_OP_WRITEV;
    sqe->fd = fd;
    sqe->iovecs = iov;
    sqe->len = iovcnt;
    sqe->off = offset;
}

/**
 * Prepare an fsync operation
 */
static inline void aio_prep_fsync(aio_sqe_t *sqe, int fd, uint32_t flags) {
    aio_sqe_zero(sqe);
    sqe->opcode = IORING_OP_FSYNC;
    sqe->fd = fd;
    sqe->fsync_flags = flags;
}

/**
 * Prepare a poll operation
 */
static inline void aio_prep_poll_add(aio_sqe_t *sqe, int fd, uint32_t events) {
    aio_sqe_zero(sqe);
    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = fd;
    sqe->poll_events = events;
}

/**
 * Prepare a timeout operation
 */
static inline void aio_prep_timeout(aio_sqe_t *sqe, uint64_t timeout_ns,
                                    uint32_t flags) {
    aio_sqe_zero(sqe);
    sqe->opcode = IORING_OP_TIMEOUT;
    sqe->off = timeout_ns;
    sqe->timeout_flags = flags;
}

/**
 * Prepare a NOP operation (for testing)
 */
static inline void aio_prep_nop(aio_sqe_t *sqe) {
    aio_sqe_zero(sqe);
    sqe->opcode = IORING_OP_NOP;
}

/**
 * Set user data on SQE
 */
static inline void aio_sqe_set_data(aio_sqe_t *sqe, uint64_t data) {
    sqe->user_data = data;
}

/**
 * Set flags on SQE
 */
static inline void aio_sqe_set_flags(aio_sqe_t *sqe, uint8_t flags) {
    sqe->flags = flags;
}

// ============================================================================
// Submission and Completion
// ============================================================================

/**
 * Submit pending SQEs to the kernel
 * @param ring          Ring context
 * @return              Number of submissions, or negative error
 */
static inline int aio_submit(aio_ring_t *ring) {
    if (!ring || ring->fd < 0) return AIO_ERR_INVAL;

    // Syscall to submit
    // return syscall(SYS_IO_SUBMIT, ring->fd, count);
    return 0;  // Placeholder
}

/**
 * Submit pending SQEs and wait for at least one completion
 * @param ring          Ring context
 * @param cqe_ptr       Output: pointer to CQE
 * @return              0 on success, negative error
 */
static inline int aio_submit_and_wait(aio_ring_t *ring, aio_cqe_t **cqe_ptr) {
    if (!ring || ring->fd < 0 || !cqe_ptr) return AIO_ERR_INVAL;

    // Submit
    int ret = aio_submit(ring);
    if (ret < 0) return ret;

    // Wait for completion
    // ret = syscall(SYS_IO_WAIT, ring->fd, 1, 0);
    // if (ret < 0) return ret;

    // Get CQE
    // *cqe_ptr = &ring->cqes[*ring->cq_head & ring->cq_mask];

    *cqe_ptr = (void*)0;  // Placeholder
    return AIO_SUCCESS;
}

/**
 * Wait for at least min_complete completions
 * @param ring          Ring context
 * @param min_complete  Minimum completions to wait for
 * @param timeout_ms    Timeout in milliseconds (0 = no timeout)
 * @return              Number of completions, or negative error
 */
static inline int aio_wait(aio_ring_t *ring, uint32_t min_complete,
                          uint32_t timeout_ms) {
    if (!ring || ring->fd < 0) return AIO_ERR_INVAL;

    // Syscall to wait
    // return syscall(SYS_IO_WAIT, ring->fd, min_complete, timeout_ms);
    (void)min_complete;
    (void)timeout_ms;
    return 0;  // Placeholder
}

/**
 * Peek at next CQE without consuming
 * @param ring          Ring context
 * @param cqe_ptr       Output: pointer to CQE
 * @return              0 if CQE available, AIO_ERR_AGAIN if empty
 */
static inline int aio_peek_cqe(aio_ring_t *ring, aio_cqe_t **cqe_ptr) {
    if (!ring || !cqe_ptr) return AIO_ERR_INVAL;

    // uint32_t head = *ring->cq_head;
    // uint32_t tail = *ring->cq_tail;
    // if (head == tail) return AIO_ERR_AGAIN;
    // *cqe_ptr = &ring->cqes[head & ring->cq_mask];

    *cqe_ptr = (void*)0;  // Placeholder
    return AIO_ERR_AGAIN;
}

/**
 * Advance CQ head after processing completions
 * @param ring          Ring context
 * @param count         Number of CQEs processed
 */
static inline void aio_cq_advance(aio_ring_t *ring, uint32_t count) {
    if (!ring || !ring->cq_head) return;

    // *ring->cq_head += count;
    (void)count;
}

/**
 * Get user data from CQE
 */
static inline uint64_t aio_cqe_get_data(aio_cqe_t *cqe) {
    return cqe ? cqe->user_data : 0;
}

// ============================================================================
// Convenience Macros
// ============================================================================

// Iterate over available CQEs
// Usage:
//   aio_cqe_t *cqe;
//   uint32_t head;
//   aio_for_each_cqe(ring, head, cqe) {
//       // Process cqe
//   }
//   aio_cq_advance(ring, head);

#define aio_for_each_cqe(ring, head, cqe) \
    for (head = 0; aio_peek_cqe(ring, &cqe) == 0; head++)

// Check if CQE indicates error
#define aio_cqe_is_error(cqe) ((cqe)->res < 0)

// Get CQE result (bytes transferred or error code)
#define aio_cqe_get_res(cqe) ((cqe)->res)

#endif // _LIBC_AIO_H
