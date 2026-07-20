// io_ring.c - Ring buffer implementation for async I/O
// Task #42 - Low-level ring buffer operations
#include "io_ring.h"
#include "../mm/heap.h"
#include "../serial.h"

// ============================================================================
// Submission Queue Operations
// ============================================================================

int sq_ring_init(io_sq_ring_t *sq, uint32_t entries,
                 io_sqe_t *sqes, uint32_t *sq_array) {
    if (!sq || !sqes || !sq_array) {
        return AIO_ERR_INVAL;
    }

    if (!is_power_of_2(entries)) {
        return AIO_ERR_INVAL;
    }

    sq->head = 0;
    sq->tail = 0;
    sq->ring_mask = entries - 1;
    sq->ring_entries = entries;
    sq->flags = 0;
    sq->dropped = 0;

    // Initialize sq_array to sequential indices
    for (uint32_t i = 0; i < entries; i++) {
        sq_array[i] = i;
    }

    // Zero out SQE array
    for (uint32_t i = 0; i < entries; i++) {
        uint64_t *p = (uint64_t *)&sqes[i];
        for (int j = 0; j < 8; j++) p[j] = 0;
    }

    return AIO_SUCCESS;
}

io_sqe_t *sq_ring_get_sqe(io_sq_ring_t *sq, io_sqe_t *sqes, uint32_t *sq_array) {
    uint32_t head = smp_load_acquire(&sq->head);
    uint32_t tail = sq->tail;

    // Check if full
    if (ring_full(head, tail, sq->ring_mask)) {
        return NULL;
    }

    // Get the SQE at tail position
    uint32_t idx = sq_array[tail & sq->ring_mask];
    return &sqes[idx];
}

void sq_ring_submit(io_sq_ring_t *sq, uint32_t count) {
    uint32_t tail = sq->tail;
    smp_store_release(&sq->tail, tail + count);
}

uint32_t sq_ring_ready(io_sq_ring_t *sq) {
    uint32_t head = sq->head;
    uint32_t tail = smp_load_acquire(&sq->tail);
    return ring_count(head, tail, sq->ring_mask);
}

int sq_ring_consume(io_sq_ring_t *sq, io_sqe_t *sqes,
                    uint32_t *sq_array, io_sqe_t **sqe_out) {
    uint32_t head = sq->head;
    uint32_t tail = smp_load_acquire(&sq->tail);

    if (ring_empty(head, tail)) {
        *sqe_out = NULL;
        return -1;
    }

    uint32_t idx = sq_array[head & sq->ring_mask];
    *sqe_out = &sqes[idx];
    return 0;
}

void sq_ring_advance(io_sq_ring_t *sq, uint32_t count) {
    smp_store_release(&sq->head, sq->head + count);
}

// ============================================================================
// Completion Queue Operations
// ============================================================================

int cq_ring_init(io_cq_ring_t *cq, uint32_t entries, io_cqe_t *cqes) {
    if (!cq || !cqes) {
        return AIO_ERR_INVAL;
    }

    if (!is_power_of_2(entries)) {
        return AIO_ERR_INVAL;
    }

    cq->head = 0;
    cq->tail = 0;
    cq->ring_mask = entries - 1;
    cq->ring_entries = entries;
    cq->overflow = 0;
    cq->cqes = 0;  // Offset, not used in kernel mode
    cq->flags = 0;

    // Zero out CQE array
    for (uint32_t i = 0; i < entries; i++) {
        cqes[i].user_data = 0;
        cqes[i].res = 0;
        cqes[i].flags = 0;
    }

    return AIO_SUCCESS;
}

int cq_ring_post(io_cq_ring_t *cq, io_cqe_t *cqes,
                 uint64_t user_data, int32_t result, uint32_t flags) {
    uint32_t head = smp_load_acquire(&cq->head);
    uint32_t tail = cq->tail;

    // Check if full (overflow)
    if (ring_full(head, tail, cq->ring_mask)) {
        cq->overflow++;
        return -1;
    }

    // Fill in CQE
    uint32_t idx = tail & cq->ring_mask;
    cqes[idx].user_data = user_data;
    cqes[idx].res = result;
    cqes[idx].flags = flags;

    // Make visible to userspace
    smp_store_release(&cq->tail, tail + 1);

    return 0;
}

uint32_t cq_ring_ready(io_cq_ring_t *cq) {
    uint32_t head = cq->head;
    uint32_t tail = smp_load_acquire(&cq->tail);
    return ring_count(head, tail, cq->ring_mask);
}

io_cqe_t *cq_ring_peek(io_cq_ring_t *cq, io_cqe_t *cqes) {
    uint32_t head = cq->head;
    uint32_t tail = smp_load_acquire(&cq->tail);

    if (ring_empty(head, tail)) {
        return NULL;
    }

    return &cqes[head & cq->ring_mask];
}

int cq_ring_consume(io_cq_ring_t *cq, io_cqe_t *cqes, io_cqe_t *cqe_out) {
    uint32_t head = cq->head;
    uint32_t tail = smp_load_acquire(&cq->tail);

    if (ring_empty(head, tail)) {
        return -1;
    }

    // Copy CQE
    uint32_t idx = head & cq->ring_mask;
    *cqe_out = cqes[idx];

    // Advance head
    smp_store_release(&cq->head, head + 1);

    return 0;
}

void cq_ring_advance(io_cq_ring_t *cq, uint32_t count) {
    smp_store_release(&cq->head, cq->head + count);
}

uint32_t cq_ring_overflow(io_cq_ring_t *cq) {
    return cq->overflow;
}

// ============================================================================
// Ring Memory Management
// ============================================================================

size_t io_ring_calc_size(uint32_t sq_entries, uint32_t cq_entries) {
    size_t size = 0;

    // SQ ring structure
    size += sizeof(io_sq_ring_t);

    // CQ ring structure
    size += sizeof(io_cq_ring_t);

    // SQE array (64 bytes each, aligned)
    size += sq_entries * sizeof(io_sqe_t);

    // SQ index array
    size += sq_entries * sizeof(uint32_t);

    // CQE array (16 bytes each)
    size += cq_entries * sizeof(io_cqe_t);

    // Add padding for alignment
    size = ALIGN_UP(size, 64);

    return size;
}

int io_ring_alloc(uint32_t sq_entries, uint32_t cq_entries, io_ring_t *ring) {
    if (!ring) {
        return AIO_ERR_INVAL;
    }

    // Validate and adjust sizes
    if (sq_entries < 2) sq_entries = 2;
    if (sq_entries > AIO_MAX_SQ_ENTRIES) sq_entries = AIO_MAX_SQ_ENTRIES;
    sq_entries = next_power_of_2(sq_entries);

    if (cq_entries < sq_entries) cq_entries = sq_entries * 2;
    if (cq_entries > AIO_MAX_CQ_ENTRIES) cq_entries = AIO_MAX_CQ_ENTRIES;
    cq_entries = next_power_of_2(cq_entries);

    // Allocate SQ ring
    ring->sq_ring = (io_sq_ring_t *)kzalloc_aligned(sizeof(io_sq_ring_t), 64);
    if (!ring->sq_ring) {
        goto err_sq_ring;
    }

    // Allocate CQ ring
    ring->cq_ring = (io_cq_ring_t *)kzalloc_aligned(sizeof(io_cq_ring_t), 64);
    if (!ring->cq_ring) {
        goto err_cq_ring;
    }

    // Allocate SQE array (must be cache-line aligned)
    ring->sq_entries = (io_sqe_t *)kzalloc_aligned(sq_entries * sizeof(io_sqe_t), 64);
    if (!ring->sq_entries) {
        goto err_sqes;
    }

    // Allocate SQ index array
    ring->sq_array = (uint32_t *)kzalloc(sq_entries * sizeof(uint32_t));
    if (!ring->sq_array) {
        goto err_sq_array;
    }

    // Allocate CQE array
    ring->cq_entries = (io_cqe_t *)kzalloc_aligned(cq_entries * sizeof(io_cqe_t), 64);
    if (!ring->cq_entries) {
        goto err_cqes;
    }

    // Initialize rings
    if (sq_ring_init(ring->sq_ring, sq_entries, ring->sq_entries, ring->sq_array) != 0) {
        goto err_init;
    }

    if (cq_ring_init(ring->cq_ring, cq_entries, ring->cq_entries) != 0) {
        goto err_init;
    }

    // Set counts
    ring->sq_entries_count = sq_entries;
    ring->cq_entries_count = cq_entries;

    kprintf("[AIO] Ring allocated: SQ=%u entries, CQ=%u entries\n",
            sq_entries, cq_entries);

    return AIO_SUCCESS;

err_init:
    kfree(ring->cq_entries);
err_cqes:
    kfree(ring->sq_array);
err_sq_array:
    kfree(ring->sq_entries);
err_sqes:
    kfree(ring->cq_ring);
err_cq_ring:
    kfree(ring->sq_ring);
err_sq_ring:
    kprintf("[AIO] Ring allocation failed: out of memory\n");
    return AIO_ERR_NOMEM;
}

void io_ring_free(io_ring_t *ring) {
    if (!ring) return;

    if (ring->cq_entries) {
        kfree(ring->cq_entries);
        ring->cq_entries = NULL;
    }

    if (ring->sq_array) {
        kfree(ring->sq_array);
        ring->sq_array = NULL;
    }

    if (ring->sq_entries) {
        kfree(ring->sq_entries);
        ring->sq_entries = NULL;
    }

    if (ring->cq_ring) {
        kfree(ring->cq_ring);
        ring->cq_ring = NULL;
    }

    if (ring->sq_ring) {
        kfree(ring->sq_ring);
        ring->sq_ring = NULL;
    }

    ring->sq_entries_count = 0;
    ring->cq_entries_count = 0;

    kprintf("[AIO] Ring freed\n");
}
