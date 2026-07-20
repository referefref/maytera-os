// async_io.h - Async I/O subsystem for MayteraOS (io_uring-like interface)
// Task #42 - Implements non-blocking I/O operations with ring buffers
#ifndef ASYNC_IO_H
#define ASYNC_IO_H

#include "../types.h"

// ============================================================================
// Configuration Constants
// ============================================================================

// Maximum number of io_ring instances system-wide
#define AIO_MAX_RINGS           64

// Default/maximum entries in submission queue
#define AIO_DEFAULT_SQ_ENTRIES  128
#define AIO_MAX_SQ_ENTRIES      4096

// Default/maximum entries in completion queue (usually 2x SQ)
#define AIO_DEFAULT_CQ_ENTRIES  256
#define AIO_MAX_CQ_ENTRIES      8192

// Maximum number of kernel worker threads per ring
#define AIO_MAX_WORKERS         4

// Operation timeout (ms) - 0 means no timeout
#define AIO_DEFAULT_TIMEOUT     0

// ============================================================================
// Operation Types (similar to Linux io_uring opcodes)
// ============================================================================

typedef enum {
    IORING_OP_NOP = 0,          // No operation (for testing)
    IORING_OP_READ,             // Read from file descriptor
    IORING_OP_WRITE,            // Write to file descriptor
    IORING_OP_READV,            // Vectored read (scatter)
    IORING_OP_WRITEV,           // Vectored write (gather)
    IORING_OP_FSYNC,            // Sync file to disk
    IORING_OP_POLL_ADD,         // Add poll event
    IORING_OP_POLL_REMOVE,      // Remove poll event
    IORING_OP_TIMEOUT,          // Add timeout
    IORING_OP_TIMEOUT_REMOVE,   // Remove timeout
    IORING_OP_ACCEPT,           // Accept socket connection
    IORING_OP_CONNECT,          // Connect socket
    IORING_OP_SEND,             // Send to socket
    IORING_OP_RECV,             // Receive from socket
    IORING_OP_OPENAT,           // Open file (async)
    IORING_OP_CLOSE,            // Close file descriptor
    IORING_OP_STATX,            // Get file status (async)
    IORING_OP_LAST              // Marker for last valid opcode
} io_op_t;

// ============================================================================
// Flags for Submission Queue Entries
// ============================================================================

// SQE flags
#define IOSQE_FIXED_FILE    (1 << 0)    // Use registered file descriptor
#define IOSQE_IO_DRAIN      (1 << 1)    // Wait for prior operations
#define IOSQE_IO_LINK       (1 << 2)    // Link with next SQE
#define IOSQE_IO_HARDLINK   (1 << 3)    // Hard link (continue on error)
#define IOSQE_ASYNC         (1 << 4)    // Force async execution
#define IOSQE_BUFFER_SELECT (1 << 5)    // Select buffer from pool

// ============================================================================
// Submission Queue Entry (SQE) - Request to perform I/O
// ============================================================================

// I/O vector for scatter/gather operations
typedef struct {
    void    *iov_base;          // Base address of buffer
    size_t   iov_len;           // Length of buffer
} io_vec_t;

// Submission Queue Entry (64 bytes, cache-line aligned)
typedef struct {
    uint8_t     opcode;         // Operation type (io_op_t)
    uint8_t     flags;          // IOSQE_* flags
    uint16_t    ioprio;         // I/O priority
    int32_t     fd;             // File descriptor

    union {
        uint64_t    off;        // Offset for read/write
        uint64_t    addr2;      // Secondary address
    };

    union {
        void       *addr;       // Buffer address
        io_vec_t   *iovecs;     // Vector array for readv/writev
        uint64_t    splice_off_in;
    };

    uint32_t    len;            // Buffer length or vector count

    union {
        uint32_t    rw_flags;   // Read/write flags
        uint32_t    fsync_flags;// Fsync flags
        uint32_t    poll_events;// Poll events to watch
        uint32_t    sync_range_flags;
        uint32_t    msg_flags;  // Send/recv flags
        uint32_t    timeout_flags;
        uint32_t    accept_flags;
        uint32_t    open_flags; // Open flags for openat
        uint32_t    statx_flags;
    };

    uint64_t    user_data;      // User data (passed through to CQE)

    // Padding to 64 bytes
    uint8_t     __pad[16];
} __attribute__((packed, aligned(64))) io_sqe_t;

// ============================================================================
// Completion Queue Entry (CQE) - Result of I/O operation
// ============================================================================

// Completion Queue Entry (16 bytes)
typedef struct {
    uint64_t    user_data;      // Copied from SQE
    int32_t     res;            // Result (bytes transferred or negative errno)
    uint32_t    flags;          // Completion flags
} __attribute__((packed)) io_cqe_t;

// CQE flags
#define IOCQE_F_BUFFER      (1 << 0)    // Buffer ID set in flags
#define IOCQE_F_MORE        (1 << 1)    // More completions coming

// ============================================================================
// Ring Buffer Structures
// ============================================================================

// Submission queue head/tail (shared with userspace)
typedef struct {
    volatile uint32_t   head;           // Producer index (userspace)
    volatile uint32_t   tail;           // Consumer index (kernel)
    volatile uint32_t   ring_mask;      // entries - 1
    volatile uint32_t   ring_entries;   // Total entries
    volatile uint32_t   flags;          // Runtime flags
    volatile uint32_t   dropped;        // Dropped submissions
} io_sq_ring_t;

// Completion queue head/tail (shared with userspace)
typedef struct {
    volatile uint32_t   head;           // Consumer index (userspace)
    volatile uint32_t   tail;           // Producer index (kernel)
    volatile uint32_t   ring_mask;      // entries - 1
    volatile uint32_t   ring_entries;   // Total entries
    volatile uint32_t   overflow;       // Overflow count
    volatile uint32_t   cqes;           // Inline CQEs offset
    volatile uint32_t   flags;          // Runtime flags
} io_cq_ring_t;

// ============================================================================
// IO Ring Instance
// ============================================================================

// Ring state
typedef enum {
    RING_STATE_FREE = 0,        // Not allocated
    RING_STATE_ACTIVE,          // Ready for use
    RING_STATE_DRAINING,        // Completing pending ops
    RING_STATE_SHUTDOWN         // Being destroyed
} ring_state_t;

// IO Ring instance (kernel-side structure)
typedef struct io_ring {
    uint32_t        ring_id;            // Unique ring identifier
    uint32_t        owner_pid;          // Process that created this ring
    ring_state_t    state;              // Current state

    // Ring buffers
    io_sq_ring_t   *sq_ring;            // Submission queue ring
    io_cq_ring_t   *cq_ring;            // Completion queue ring
    io_sqe_t       *sq_entries;         // SQE array
    io_cqe_t       *cq_entries;         // CQE array
    uint32_t       *sq_array;           // SQE index array

    // Configuration
    uint32_t        sq_entries_count;   // Number of SQ entries
    uint32_t        cq_entries_count;   // Number of CQ entries
    uint32_t        flags;              // Ring flags

    // Processing state
    volatile uint32_t pending_ops;      // Operations in flight
    volatile uint32_t completed_ops;    // Total completed operations

    // Worker threads (if async processing enabled)
    uint32_t        worker_count;
    // Worker thread IDs would go here in a full implementation

    // Statistics
    uint64_t        total_submitted;
    uint64_t        total_completed;
    uint64_t        total_errors;

    // Linked list for ring management
    struct io_ring *next;
} io_ring_t;

// ============================================================================
// Syscall Structures
// ============================================================================

// Parameters for sys_io_setup
typedef struct {
    uint32_t    sq_entries;     // Desired SQ size (power of 2)
    uint32_t    cq_entries;     // Desired CQ size (power of 2, >= sq_entries)
    uint32_t    flags;          // Setup flags
} io_setup_params_t;

// Setup flags
#define IORING_SETUP_IOPOLL     (1 << 0)    // Use I/O polling
#define IORING_SETUP_SQPOLL     (1 << 1)    // Kernel-side SQ polling
#define IORING_SETUP_SQ_AFF     (1 << 2)    // SQ polling CPU affinity
#define IORING_SETUP_CQSIZE     (1 << 3)    // Custom CQ size
#define IORING_SETUP_CLAMP      (1 << 4)    // Clamp SQ/CQ sizes

// Wait parameters for sys_io_wait
typedef struct {
    uint32_t    min_complete;   // Minimum completions to wait for
    uint32_t    timeout_ms;     // Timeout in milliseconds (0 = no timeout)
    uint32_t    flags;          // Wait flags
} io_wait_params_t;

// Wait flags
#define IORING_WAIT_TIMEOUT     (1 << 0)    // Use timeout
#define IORING_WAIT_INTERRUPTIBLE (1 << 1)  // Can be interrupted

// ============================================================================
// Error Codes
// ============================================================================

#define AIO_SUCCESS         0       // Operation successful
#define AIO_ERR_NOMEM      -1       // Out of memory
#define AIO_ERR_INVAL      -2       // Invalid argument
#define AIO_ERR_NOSPC      -3       // No space (rings full)
#define AIO_ERR_NOENT      -4       // No such entry/ring
#define AIO_ERR_BUSY       -5       // Ring busy
#define AIO_ERR_PERM       -6       // Permission denied
#define AIO_ERR_BADF       -7       // Bad file descriptor
#define AIO_ERR_AGAIN      -8       // Try again (would block)
#define AIO_ERR_INTR       -9       // Interrupted
#define AIO_ERR_TIMEOUT    -10      // Operation timed out
#define AIO_ERR_OVERFLOW   -11      // Queue overflow
#define AIO_ERR_SHUTDOWN   -12      // Ring is shutting down

// ============================================================================
// Kernel API - Ring Management
// ============================================================================

/**
 * Initialize the async I/O subsystem
 * Must be called during kernel initialization
 */
void aio_init(void);

/**
 * Create a new io_ring instance
 * @param params    Setup parameters
 * @param ring_fd   Output: ring file descriptor
 * @return          0 on success, negative error code on failure
 */
int aio_ring_create(io_setup_params_t *params, int32_t *ring_fd);

/**
 * Destroy an io_ring instance
 * @param ring_fd   Ring file descriptor
 * @return          0 on success, negative error code on failure
 */
int aio_ring_destroy(int32_t ring_fd);

/**
 * Get ring by file descriptor
 * @param ring_fd   Ring file descriptor
 * @return          Pointer to ring, or NULL if not found
 */
io_ring_t *aio_ring_get(int32_t ring_fd);

// ============================================================================
// Kernel API - Submission/Completion
// ============================================================================

/**
 * Submit operations from the submission queue
 * @param ring      IO ring instance
 * @param count     Maximum number of SQEs to submit
 * @return          Number of operations submitted, or negative error
 */
int aio_submit(io_ring_t *ring, uint32_t count);

/**
 * Wait for completions
 * @param ring      IO ring instance
 * @param params    Wait parameters
 * @return          Number of completions available, or negative error
 */
int aio_wait(io_ring_t *ring, io_wait_params_t *params);

/**
 * Get next completion entry (if available)
 * @param ring      IO ring instance
 * @param cqe       Output: completion entry
 * @return          0 if entry available, AIO_ERR_AGAIN if empty
 */
int aio_get_cqe(io_ring_t *ring, io_cqe_t *cqe);

/**
 * Advance CQ head (after processing completions)
 * @param ring      IO ring instance
 * @param count     Number of entries to advance
 */
void aio_cq_advance(io_ring_t *ring, uint32_t count);

// ============================================================================
// Kernel API - Operation Processing
// ============================================================================

/**
 * Process a single submission queue entry
 * Called by worker threads or polling loop
 * @param ring      IO ring instance
 * @param sqe       Submission queue entry to process
 * @return          0 on success, negative error code on failure
 */
int aio_process_sqe(io_ring_t *ring, io_sqe_t *sqe);

/**
 * Complete an operation and post to CQ
 * @param ring      IO ring instance
 * @param user_data User data from SQE
 * @param result    Result value (bytes or negative error)
 * @param flags     CQE flags
 * @return          0 on success, negative error code on failure
 */
int aio_complete(io_ring_t *ring, uint64_t user_data, int32_t result, uint32_t flags);

// ============================================================================
// Syscall Interface (for userspace)
// ============================================================================

/**
 * sys_io_setup - Create io_ring instance
 * @param entries       Number of SQ entries
 * @param cq_entries    Number of CQ entries (0 = auto)
 * @return              Ring file descriptor, or negative error
 */
int64_t sys_io_setup(uint32_t entries, uint32_t cq_entries);

/**
 * sys_io_submit - Submit operations
 * @param ring_fd       Ring file descriptor
 * @param sqe_count     Number of SQEs to submit
 * @return              Number submitted, or negative error
 */
int64_t sys_io_submit(int32_t ring_fd, uint32_t sqe_count);

/**
 * sys_io_wait - Wait for completions
 * @param ring_fd       Ring file descriptor
 * @param min_complete  Minimum completions to wait for
 * @param timeout_ms    Timeout in milliseconds (0 = no wait)
 * @return              Number of completions, or negative error
 */
int64_t sys_io_wait(int32_t ring_fd, uint32_t min_complete, uint32_t timeout_ms);

/**
 * sys_io_destroy - Destroy io_ring instance
 * @param ring_fd       Ring file descriptor
 * @return              0 on success, negative error
 */
int64_t sys_io_destroy(int32_t ring_fd);

/**
 * sys_io_cancel - Cancel pending operation
 * @param ring_fd       Ring file descriptor
 * @param user_data     User data of operation to cancel
 * @return              0 on success, negative error
 */
int64_t sys_io_cancel(int32_t ring_fd, uint64_t user_data);

// ============================================================================
// Statistics and Debugging
// ============================================================================

/**
 * Get statistics for an io_ring
 */
typedef struct {
    uint64_t    total_submitted;
    uint64_t    total_completed;
    uint64_t    total_errors;
    uint32_t    pending_ops;
    uint32_t    sq_entries_used;
    uint32_t    cq_entries_used;
} aio_stats_t;

int aio_get_stats(io_ring_t *ring, aio_stats_t *stats);

/**
 * Print async I/O subsystem status
 */
void aio_print_status(void);

#endif // ASYNC_IO_H
