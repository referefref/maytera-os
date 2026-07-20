// async_io.c - Async I/O subsystem implementation for MayteraOS
// Task #42 - io_uring-like non-blocking I/O operations
#include "async_io.h"
#include "io_ring.h"
#include "../types.h"
#include "../mm/heap.h"
#include "../serial.h"
#include "../proc/process.h"
#include "../fs/fat.h"
#include "../string.h"

// External global filesystem
extern fat_fs_t g_fat_fs;

// External timer for timeouts
extern volatile uint64_t timer_ticks;

// ============================================================================
// Global State
// ============================================================================

// Array of all io_ring instances
static io_ring_t *g_rings[AIO_MAX_RINGS];

// Lock for ring table (simple spinlock)
static volatile uint32_t g_rings_lock = 0;

// Next ring ID
static uint32_t g_next_ring_id = 1;

// Statistics
static uint64_t g_total_rings_created = 0;
static uint64_t g_total_ops_submitted = 0;
static uint64_t g_total_ops_completed = 0;

// Subsystem initialized flag
static int g_aio_initialized = 0;

// ============================================================================
// Spinlock Helpers
// ============================================================================

static inline void spin_lock(volatile uint32_t *lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        while (*lock) {
            pause();  // CPU hint for spin wait
        }
    }
}

static inline void spin_unlock(volatile uint32_t *lock) {
    __sync_lock_release(lock);
}

// ============================================================================
// Ring Management
// ============================================================================

void aio_init(void) {
    if (g_aio_initialized) {
        return;
    }

    kprintf("[AIO] Initializing async I/O subsystem...\n");

    // Clear ring table
    for (int i = 0; i < AIO_MAX_RINGS; i++) {
        g_rings[i] = NULL;
    }

    g_next_ring_id = 1;
    g_total_rings_created = 0;
    g_total_ops_submitted = 0;
    g_total_ops_completed = 0;
    g_rings_lock = 0;

    g_aio_initialized = 1;
    kprintf("[AIO] Async I/O subsystem initialized (max rings: %d)\n", AIO_MAX_RINGS);
}

// Find free slot in ring table
static int find_free_ring_slot(void) {
    for (int i = 0; i < AIO_MAX_RINGS; i++) {
        if (g_rings[i] == NULL) {
            return i;
        }
    }
    return -1;
}

int aio_ring_create(io_setup_params_t *params, int32_t *ring_fd) {
    if (!g_aio_initialized) {
        return AIO_ERR_INVAL;
    }

    if (!params || !ring_fd) {
        return AIO_ERR_INVAL;
    }

    // Validate parameters
    uint32_t sq_entries = params->sq_entries;
    uint32_t cq_entries = params->cq_entries;

    if (sq_entries == 0) sq_entries = AIO_DEFAULT_SQ_ENTRIES;
    if (cq_entries == 0) cq_entries = AIO_DEFAULT_CQ_ENTRIES;

    // Clamp if requested
    if (params->flags & IORING_SETUP_CLAMP) {
        if (sq_entries > AIO_MAX_SQ_ENTRIES) sq_entries = AIO_MAX_SQ_ENTRIES;
        if (cq_entries > AIO_MAX_CQ_ENTRIES) cq_entries = AIO_MAX_CQ_ENTRIES;
    } else {
        if (sq_entries > AIO_MAX_SQ_ENTRIES) {
            return AIO_ERR_INVAL;
        }
        if (cq_entries > AIO_MAX_CQ_ENTRIES) {
            return AIO_ERR_INVAL;
        }
    }

    spin_lock(&g_rings_lock);

    // Find free slot
    int slot = find_free_ring_slot();
    if (slot < 0) {
        spin_unlock(&g_rings_lock);
        kprintf("[AIO] No free ring slots\n");
        return AIO_ERR_NOSPC;
    }

    // Allocate ring structure
    io_ring_t *ring = (io_ring_t *)kzalloc(sizeof(io_ring_t));
    if (!ring) {
        spin_unlock(&g_rings_lock);
        return AIO_ERR_NOMEM;
    }

    // Initialize ring buffers
    int ret = io_ring_alloc(sq_entries, cq_entries, ring);
    if (ret != AIO_SUCCESS) {
        kfree(ring);
        spin_unlock(&g_rings_lock);
        return ret;
    }

    // Set ring metadata
    ring->ring_id = g_next_ring_id++;
    ring->owner_pid = proc_current() ? proc_current()->pid : 0;
    ring->state = RING_STATE_ACTIVE;
    ring->flags = params->flags;
    ring->pending_ops = 0;
    ring->completed_ops = 0;
    ring->worker_count = 0;
    ring->total_submitted = 0;
    ring->total_completed = 0;
    ring->total_errors = 0;
    ring->next = NULL;

    // Store in table
    g_rings[slot] = ring;
    g_total_rings_created++;

    // Return slot as "file descriptor"
    *ring_fd = slot;

    spin_unlock(&g_rings_lock);

    kprintf("[AIO] Created ring %u (fd=%d, SQ=%u, CQ=%u, owner=%u)\n",
            ring->ring_id, slot, ring->sq_entries_count,
            ring->cq_entries_count, ring->owner_pid);

    return AIO_SUCCESS;
}

int aio_ring_destroy(int32_t ring_fd) {
    if (ring_fd < 0 || ring_fd >= AIO_MAX_RINGS) {
        return AIO_ERR_BADF;
    }

    spin_lock(&g_rings_lock);

    io_ring_t *ring = g_rings[ring_fd];
    if (!ring) {
        spin_unlock(&g_rings_lock);
        return AIO_ERR_BADF;
    }

    // Check ownership
    process_t *current = proc_current();
    if (current && ring->owner_pid != current->pid && ring->owner_pid != 0) {
        spin_unlock(&g_rings_lock);
        return AIO_ERR_PERM;
    }

    // Mark as shutting down
    ring->state = RING_STATE_SHUTDOWN;

    // Wait for pending operations (with timeout)
    uint32_t timeout = 1000;  // ~1 second
    while (ring->pending_ops > 0 && timeout > 0) {
        spin_unlock(&g_rings_lock);
        for (int i = 0; i < 1000; i++) io_wait();
        timeout--;
        spin_lock(&g_rings_lock);
    }

    // Free ring buffers
    io_ring_free(ring);

    // Free ring structure
    kfree(ring);

    // Remove from table
    g_rings[ring_fd] = NULL;

    spin_unlock(&g_rings_lock);

    kprintf("[AIO] Destroyed ring (fd=%d)\n", ring_fd);

    return AIO_SUCCESS;
}

io_ring_t *aio_ring_get(int32_t ring_fd) {
    if (ring_fd < 0 || ring_fd >= AIO_MAX_RINGS) {
        return NULL;
    }
    return g_rings[ring_fd];
}

// ============================================================================
// Operation Processing
// ============================================================================

// Process a read operation
static int process_op_read(io_ring_t *ring, io_sqe_t *sqe) {
    UNUSED(ring);

    // For now, we only support FAT filesystem reads
    // In a full implementation, this would use a file descriptor table

    int fd = sqe->fd;
    void *buf = sqe->addr;
    uint32_t len = sqe->len;
    uint64_t offset = sqe->off;

    UNUSED(fd);
    UNUSED(offset);

    // Placeholder: simulate read
    // In real implementation, look up fd in process file table
    // and perform the actual read operation

    if (!buf || len == 0) {
        return AIO_ERR_INVAL;
    }

    // For demonstration, we'll just zero the buffer and return length
    // A real implementation would:
    // 1. Look up fd in file descriptor table
    // 2. Seek to offset if specified
    // 3. Read data into buffer
    // 4. Return actual bytes read

    memset(buf, 0, len);

    kprintf("[AIO] Read op: fd=%d, len=%u, offset=%lu (simulated)\n",
            fd, len, offset);

    return (int)len;  // Return bytes "read"
}

// Process a write operation
static int process_op_write(io_ring_t *ring, io_sqe_t *sqe) {
    int fd = sqe->fd;
    void *buf = sqe->addr;
    uint32_t len = sqe->len;
    uint64_t offset = sqe->off;

    UNUSED(ring);
    UNUSED(fd);
    UNUSED(offset);

    if (!buf || len == 0) {
        return AIO_ERR_INVAL;
    }

    // Placeholder: simulate write
    kprintf("[AIO] Write op: fd=%d, len=%u, offset=%lu (simulated)\n",
            fd, len, offset);

    return (int)len;  // Return bytes "written"
}

// Process a NOP operation (for testing)
static int process_op_nop(io_ring_t *ring, io_sqe_t *sqe) {
    UNUSED(ring);
    UNUSED(sqe);
    return 0;
}

// Process fsync operation
static int process_op_fsync(io_ring_t *ring, io_sqe_t *sqe) {
    UNUSED(ring);

    int fd = sqe->fd;
    uint32_t flags = sqe->fsync_flags;

    UNUSED(fd);
    UNUSED(flags);

    // Placeholder: would sync file to disk
    kprintf("[AIO] Fsync op: fd=%d, flags=0x%x (simulated)\n", fd, flags);

    return 0;
}

// Process timeout operation
static int process_op_timeout(io_ring_t *ring, io_sqe_t *sqe) {
    UNUSED(ring);

    // Get timeout value from sqe
    uint64_t timeout_ns = sqe->off;  // Timeout stored in offset field
    uint32_t flags = sqe->timeout_flags;

    UNUSED(flags);

    // Convert to ticks (assuming 1000 ticks/second = 1ms per tick)
    uint64_t timeout_ticks = timeout_ns / 1000000;  // ns to ms

    kprintf("[AIO] Timeout op: %lu ms (simulated)\n", timeout_ticks);

    // In real implementation, this would:
    // 1. Add to timer queue
    // 2. Complete when timeout expires or is cancelled

    return 0;
}

// Process poll_add operation
static int process_op_poll_add(io_ring_t *ring, io_sqe_t *sqe) {
    UNUSED(ring);

    int fd = sqe->fd;
    uint32_t events = sqe->poll_events;

    kprintf("[AIO] Poll add: fd=%d, events=0x%x (simulated)\n", fd, events);

    // In real implementation, this would:
    // 1. Add fd to poll list
    // 2. Complete when events occur

    return 0;
}

int aio_process_sqe(io_ring_t *ring, io_sqe_t *sqe) {
    if (!ring || !sqe) {
        return AIO_ERR_INVAL;
    }

    int result;

    // Dispatch based on opcode
    switch (sqe->opcode) {
        case IORING_OP_NOP:
            result = process_op_nop(ring, sqe);
            break;

        case IORING_OP_READ:
            result = process_op_read(ring, sqe);
            break;

        case IORING_OP_WRITE:
            result = process_op_write(ring, sqe);
            break;

        case IORING_OP_FSYNC:
            result = process_op_fsync(ring, sqe);
            break;

        case IORING_OP_TIMEOUT:
            result = process_op_timeout(ring, sqe);
            break;

        case IORING_OP_POLL_ADD:
            result = process_op_poll_add(ring, sqe);
            break;

        case IORING_OP_READV:
        case IORING_OP_WRITEV:
            // Vectored I/O not yet implemented
            kprintf("[AIO] Vectored I/O not implemented\n");
            result = AIO_ERR_INVAL;
            break;

        default:
            kprintf("[AIO] Unknown opcode: %u\n", sqe->opcode);
            result = AIO_ERR_INVAL;
            break;
    }

    return result;
}

int aio_complete(io_ring_t *ring, uint64_t user_data, int32_t result, uint32_t flags) {
    if (!ring || !ring->cq_ring || !ring->cq_entries) {
        return AIO_ERR_INVAL;
    }

    int ret = cq_ring_post(ring->cq_ring, ring->cq_entries,
                           user_data, result, flags);

    if (ret == 0) {
        ring->completed_ops++;
        ring->total_completed++;
        g_total_ops_completed++;
    } else {
        ring->total_errors++;
        return AIO_ERR_OVERFLOW;
    }

    return AIO_SUCCESS;
}

// ============================================================================
// Submission and Waiting
// ============================================================================

int aio_submit(io_ring_t *ring, uint32_t count) {
    if (!ring || ring->state != RING_STATE_ACTIVE) {
        return AIO_ERR_INVAL;
    }

    uint32_t submitted = 0;

    // Process up to 'count' SQEs
    while (submitted < count) {
        io_sqe_t *sqe = NULL;

        // Get next SQE from queue
        if (sq_ring_consume(ring->sq_ring, ring->sq_entries,
                           ring->sq_array, &sqe) != 0) {
            break;  // Queue empty
        }

        // Track pending
        ring->pending_ops++;

        // Process the operation
        int result = aio_process_sqe(ring, sqe);

        // Complete immediately (synchronous path for now)
        // In a full async implementation, this would be deferred
        aio_complete(ring, sqe->user_data, result, 0);

        ring->pending_ops--;

        // Advance SQ head
        sq_ring_advance(ring->sq_ring, 1);

        submitted++;
        ring->total_submitted++;
        g_total_ops_submitted++;
    }

    return (int)submitted;
}

int aio_wait(io_ring_t *ring, io_wait_params_t *params) {
    if (!ring || ring->state != RING_STATE_ACTIVE) {
        return AIO_ERR_INVAL;
    }

    uint32_t min_complete = params ? params->min_complete : 1;
    uint32_t timeout_ms = params ? params->timeout_ms : 0;

    uint64_t start_ticks = timer_ticks;
    uint64_t timeout_ticks = timeout_ms;  // Assuming ~1ms per tick

    while (1) {
        // Check for completions
        uint32_t ready = cq_ring_ready(ring->cq_ring);
        if (ready >= min_complete) {
            return (int)ready;
        }

        // Check timeout
        if (timeout_ms > 0) {
            uint64_t elapsed = timer_ticks - start_ticks;
            if (elapsed >= timeout_ticks) {
                return ready > 0 ? (int)ready : AIO_ERR_TIMEOUT;
            }
        }

        // If no timeout and no completions needed, return immediately
        if (timeout_ms == 0 && min_complete == 0) {
            return (int)ready;
        }

        // Yield CPU
        hlt();
    }
}

int aio_get_cqe(io_ring_t *ring, io_cqe_t *cqe) {
    if (!ring || !cqe) {
        return AIO_ERR_INVAL;
    }

    if (cq_ring_consume(ring->cq_ring, ring->cq_entries, cqe) != 0) {
        return AIO_ERR_AGAIN;
    }

    return AIO_SUCCESS;
}

void aio_cq_advance(io_ring_t *ring, uint32_t count) {
    if (ring && ring->cq_ring) {
        cq_ring_advance(ring->cq_ring, count);
    }
}

// ============================================================================
// Syscall Interface
// ============================================================================

int64_t sys_io_setup(uint32_t entries, uint32_t cq_entries) {
    io_setup_params_t params = {
        .sq_entries = entries,
        .cq_entries = cq_entries,
        .flags = IORING_SETUP_CLAMP
    };

    int32_t ring_fd;
    int ret = aio_ring_create(&params, &ring_fd);

    if (ret != AIO_SUCCESS) {
        return ret;
    }

    return ring_fd;
}

int64_t sys_io_submit(int32_t ring_fd, uint32_t sqe_count) {
    io_ring_t *ring = aio_ring_get(ring_fd);
    if (!ring) {
        return AIO_ERR_BADF;
    }

    return aio_submit(ring, sqe_count);
}

int64_t sys_io_wait(int32_t ring_fd, uint32_t min_complete, uint32_t timeout_ms) {
    io_ring_t *ring = aio_ring_get(ring_fd);
    if (!ring) {
        return AIO_ERR_BADF;
    }

    io_wait_params_t params = {
        .min_complete = min_complete,
        .timeout_ms = timeout_ms,
        .flags = timeout_ms > 0 ? IORING_WAIT_TIMEOUT : 0
    };

    return aio_wait(ring, &params);
}

int64_t sys_io_destroy(int32_t ring_fd) {
    return aio_ring_destroy(ring_fd);
}

int64_t sys_io_cancel(int32_t ring_fd, uint64_t user_data) {
    io_ring_t *ring = aio_ring_get(ring_fd);
    if (!ring) {
        return AIO_ERR_BADF;
    }

    UNUSED(user_data);

    // Cancellation not yet fully implemented
    // Would search pending operations and cancel matching user_data
    kprintf("[AIO] Cancel not implemented: user_data=%lu\n", user_data);

    return AIO_ERR_INVAL;
}

// ============================================================================
// Statistics and Debugging
// ============================================================================

int aio_get_stats(io_ring_t *ring, aio_stats_t *stats) {
    if (!ring || !stats) {
        return AIO_ERR_INVAL;
    }

    stats->total_submitted = ring->total_submitted;
    stats->total_completed = ring->total_completed;
    stats->total_errors = ring->total_errors;
    stats->pending_ops = ring->pending_ops;

    if (ring->sq_ring) {
        stats->sq_entries_used = sq_ring_ready(ring->sq_ring);
    } else {
        stats->sq_entries_used = 0;
    }

    if (ring->cq_ring) {
        stats->cq_entries_used = cq_ring_ready(ring->cq_ring);
    } else {
        stats->cq_entries_used = 0;
    }

    return AIO_SUCCESS;
}

void aio_print_status(void) {
    kprintf("\n[AIO] Async I/O Subsystem Status\n");
    kprintf("  Initialized:      %s\n", g_aio_initialized ? "Yes" : "No");
    kprintf("  Total rings:      %lu\n", g_total_rings_created);
    kprintf("  Ops submitted:    %lu\n", g_total_ops_submitted);
    kprintf("  Ops completed:    %lu\n", g_total_ops_completed);
    kprintf("\n");

    kprintf("  Active Rings:\n");
    int active_count = 0;
    for (int i = 0; i < AIO_MAX_RINGS; i++) {
        io_ring_t *ring = g_rings[i];
        if (ring) {
            active_count++;
            const char *state_str = "Unknown";
            switch (ring->state) {
                case RING_STATE_FREE: state_str = "Free"; break;
                case RING_STATE_ACTIVE: state_str = "Active"; break;
                case RING_STATE_DRAINING: state_str = "Draining"; break;
                case RING_STATE_SHUTDOWN: state_str = "Shutdown"; break;
            }
            kprintf("    [%d] Ring %u: state=%s, owner=%u, pending=%u\n",
                    i, ring->ring_id, state_str, ring->owner_pid, ring->pending_ops);
            kprintf("         SQ: %u/%u, CQ: %u/%u\n",
                    sq_ring_ready(ring->sq_ring), ring->sq_entries_count,
                    cq_ring_ready(ring->cq_ring), ring->cq_entries_count);
        }
    }

    if (active_count == 0) {
        kprintf("    (none)\n");
    }
}
