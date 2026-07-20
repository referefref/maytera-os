// heap.c - Kernel Heap Allocator implementation
// Simple linked-list based allocator with first-fit strategy

#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "../serial.h"
#include "../string.h"

// Heap configuration
#define HEAP_INITIAL_SIZE   (128 * MB)     // Initial heap size - needs 64MB+ for NetHack ELF (8.4MB)
#define HEAP_MAX_SIZE       (256 * MB)    // Maximum heap size
#define HEAP_BLOCK_MAGIC    0x48454150    // "HEAP"
#define HEAP_MIN_BLOCK_SIZE 32            // Minimum block size (including header)

// Heap virtual address range
// We'll use the virtual address range starting at 0x10000000 (256 MB)
#define HEAP_VIRT_BASE      0x10000000ULL

// Block header structure
typedef struct heap_block {
    uint32_t magic;              // Magic number for validation
    uint32_t flags;              // Flags (bit 0 = used)
    size_t   size;               // Size of data area (not including header)
    struct heap_block *next;     // Next block in free list (only valid if free)
    struct heap_block *prev;     // Previous block in free list
} __attribute__((packed)) heap_block_t;

#define BLOCK_HEADER_SIZE   sizeof(heap_block_t)
#define BLOCK_FLAG_USED     (1 << 0)

// Heap state
static uint64_t heap_start = 0;
static uint64_t heap_end = 0;
static uint64_t heap_size = 0;
static heap_block_t *free_list = NULL;

// Statistics
static size_t total_allocated = 0;
static size_t total_freed = 0;
static size_t allocation_count = 0;
static size_t free_count = 0;

// Spinlock for thread safety
static volatile int heap_lock = 0;

// #347: heap_lock used to be a plain busy-wait spinlock that never disabled
// interrupts. Any interrupt handler that calls kmalloc()/kfree() on the same
// CPU while foreground code already holds heap_lock (e.g. mid-split/coalesce)
// would spin on this lock from inside the ISR - forever, since the CPU that
// could release it is the one now stuck servicing the interrupt. That is a
// self-deadlock with IF effectively pinned by the interrupt gate, silently
// stopping the timer tick and every other interrupt on that core. Save/clear
// IF around the critical section (matches the irqsave pattern already used by
// sync/spinlock.h and proc_wake()) so an ISR on this core can never observe
// heap_lock held mid-update, and so the critical section itself can't be
// interrupted and re-entered.
static uint64_t heap_acquire_lock(void) {
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");
    while (__sync_lock_test_and_set(&heap_lock, 1)) {
        __asm__ volatile("pause");
    }
    return rflags;
}

static void heap_release_lock(uint64_t rflags) {
    __sync_lock_release(&heap_lock);
    if (rflags & (1ULL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
}

// Expand the heap by allocating more pages
static int heap_expand(size_t min_size) {
    // Calculate how many pages we need
    size_t expand_size = HEAP_INITIAL_SIZE;  // Expand in large chunks
    if (min_size > expand_size) {
        expand_size = (min_size + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
    }

    // Check if we've reached max size
    if (heap_size + expand_size > HEAP_MAX_SIZE) {
        if (heap_size >= HEAP_MAX_SIZE) {
            return -1;  // Cannot expand further
        }
        expand_size = HEAP_MAX_SIZE - heap_size;
    }

    uint64_t pages_needed = expand_size / PMM_PAGE_SIZE;

    // Allocate physical pages and map them
    for (uint64_t i = 0; i < pages_needed; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            kprintf("[HEAP] ERROR: Failed to allocate physical page for heap expansion\n");
            return -1;
        }

        uint64_t virt = heap_end + i * PMM_PAGE_SIZE;
        if (vmm_map_page(virt, phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE) != 0) {
            pmm_free_page(phys);
            kprintf("[HEAP] ERROR: Failed to map heap page\n");
            return -1;
        }
    }

    // Create a new free block for the expanded area
    heap_block_t *new_block = (heap_block_t *)heap_end;
    new_block->magic = HEAP_BLOCK_MAGIC;
    new_block->flags = 0;  // Free
    new_block->size = expand_size - BLOCK_HEADER_SIZE;
    new_block->next = free_list;
    new_block->prev = NULL;

    if (free_list) {
        free_list->prev = new_block;
    }
    free_list = new_block;

    heap_end += expand_size;
    heap_size += expand_size;

    return 0;
}

// Initialize the kernel heap
void heap_init(void) {
    kprintf("[HEAP] Initializing kernel heap...\n");

    heap_start = HEAP_VIRT_BASE;
    heap_end = heap_start;
    heap_size = 0;

    // Allocate initial heap space
    if (heap_expand(HEAP_INITIAL_SIZE) != 0) {
        kprintf("[HEAP] ERROR: Failed to allocate initial heap space!\n");
        return;
    }

    kprintf("[HEAP] Heap initialized at 0x%lx - 0x%lx (%lu KB)\n",
            heap_start, heap_end, heap_size / KB);
}

// Find a suitable free block (first-fit strategy)
static heap_block_t *find_free_block(size_t size) {
    heap_block_t *block = free_list;

    while (block) {
        if (block->size >= size) {
            return block;
        }
        block = block->next;
    }

    return NULL;
}

// Remove a block from the free list
static void remove_from_free_list(heap_block_t *block) {
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        free_list = block->next;
    }

    if (block->next) {
        block->next->prev = block->prev;
    }

    block->next = NULL;
    block->prev = NULL;
}

// Add a block to the free list (at the beginning)
static void add_to_free_list(heap_block_t *block) {
    block->next = free_list;
    block->prev = NULL;

    if (free_list) {
        free_list->prev = block;
    }

    free_list = block;
}

// Split a block if it's much larger than needed
static void split_block(heap_block_t *block, size_t size) {
    // Only split when the leftover can hold a header + a minimum data block.
    // CRITICAL: guard against size_t underflow on a near-exact fit. If
    // block->size is within BLOCK_HEADER_SIZE of `size`, the old
    // `block->size - size - BLOCK_HEADER_SIZE` wrapped to a ~2^64 value, passed
    // the "large enough" test, and published a bogus giant free block. Later
    // allocations then overlapped live memory (kernel stacks, other buffers),
    // causing #GP / silent corruption (e.g. launching the 315 KB Settings ELF).
    if (block->size < size + BLOCK_HEADER_SIZE + HEAP_MIN_BLOCK_SIZE) {
        return;  // keep the whole block with the allocation (internal frag)
    }

    // remaining = data bytes left after carving `size` + one new header.
    size_t remaining = block->size - size - BLOCK_HEADER_SIZE;

    // Create new block from the remaining space
    heap_block_t *new_block = (heap_block_t *)((uint8_t *)block + BLOCK_HEADER_SIZE + size);
    new_block->magic = HEAP_BLOCK_MAGIC;
    new_block->flags = 0;  // Free
    new_block->size = remaining;   // FIX: was remaining - BLOCK_HEADER_SIZE (leaked a header each split)

    // Update original block size
    block->size = size;

    // Add new block to free list
    add_to_free_list(new_block);
}

// Allocate memory from the heap
void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    // Align size to 16 bytes
    size = (size + 15) & ~15ULL;

    uint64_t irqf = heap_acquire_lock();

    // Find a free block
    heap_block_t *block = find_free_block(size);

    // If no suitable block found, try to expand the heap
    if (!block) {
        if (heap_expand(size + BLOCK_HEADER_SIZE) != 0) {
            heap_release_lock(irqf);
            kprintf("[HEAP] ERROR: Out of memory! Requested %lu bytes\n", size);
            return NULL;
        }
        block = find_free_block(size);
        if (!block) {
            heap_release_lock(irqf);
            return NULL;
        }
    }

    // Remove from free list
    remove_from_free_list(block);

    // Split if necessary
    split_block(block, size);

    // Mark as used
    block->flags |= BLOCK_FLAG_USED;

    // Update statistics
    total_allocated += block->size;
    allocation_count++;

    heap_release_lock(irqf);

    // Return pointer to data area (after header)
    return (void *)((uint8_t *)block + BLOCK_HEADER_SIZE);
}

// Allocate aligned memory
void *kmalloc_aligned(size_t size, size_t alignment) {
    // Simple approach: allocate extra space and align within it
    if (alignment <= 16) {
        return kmalloc(size);  // Already 16-byte aligned
    }

    // Allocate extra space for alignment
    size_t extra = alignment - 1 + sizeof(void *);
    void *raw = kmalloc(size + extra);
    if (!raw) return NULL;

    // Calculate aligned address
    uintptr_t aligned = ((uintptr_t)raw + extra) & ~(alignment - 1);

    // Store original pointer just before aligned address
    ((void **)aligned)[-1] = raw;

    return (void *)aligned;
}

// Allocate zeroed memory
void *kzalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

// Allocate zeroed aligned memory
void *kzalloc_aligned(size_t size, size_t alignment) {
    void *ptr = kmalloc_aligned(size, alignment);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

// Reallocate memory
void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) {
        return kmalloc(new_size);
    }

    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    // Get block header
    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - BLOCK_HEADER_SIZE);

    // Validate block
    if (block->magic != HEAP_BLOCK_MAGIC) {
        kprintf("[HEAP] ERROR: krealloc() called on invalid pointer!\n");
        return NULL;
    }

    // If the block is already large enough, just return it
    if (block->size >= new_size) {
        return ptr;
    }

    // Allocate new block and copy data
    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) {
        return NULL;
    }

    memcpy(new_ptr, ptr, block->size);
    kfree(ptr);

    return new_ptr;
}

// Merge adjacent free blocks (forward AND backward).
//
// #347: this used to only merge `block` with its next physical neighbor.
// Blocks freed in an order that leaves them physically adjacent to a free
// block that comes BEFORE them in memory never got merged with that
// predecessor, so long-running processes with steady alloc/free churn
// (observed live: a Home Assistant polling client doing an HTTP GET every
// few seconds, hours of uptime) fragment the heap into many small
// unmergeable free blocks. That reproduces exactly what we saw on a live
// serial capture: sustained "[HEAP] ERROR: Out of memory! Requested 1048576
// bytes" (and even 4096-byte) failures despite the heap having plenty of
// free bytes in aggregate - just never as one contiguous run. Worse, any
// caller of kmalloc() that doesn't check for NULL (this codebase has had at
// least one prior heap-corruption bug of this shape - see the split_block()
// underflow comment above, which caused "#GP / silent corruption") turns
// that OOM into a kernel-mode NULL/bad-pointer dereference, which is caught
// by the fault handler and converted into a permanent cli+hlt of that core
// (cpu/idt.c isr_handler_impl) - fatal because interrupts (the timer tick)
// never resume, and the whole box appears to "hang idle forever".
//
// There's no back-pointer to the physical predecessor in heap_block_t (that
// would need a footer/boundary tag, a bigger layout change), so scan the
// free list for a free block whose end address equals `block`'s start. This
// is O(free-list length) but only runs on the kfree() path, and correctness
// against fragmentation matters far more than micro-optimizing free().
// Returns the block that should actually be added to the free list (it may
// be the absorbing predecessor rather than `block` itself).
static heap_block_t *coalesce_blocks(heap_block_t *block) {
    // Try to merge with the next physical block.
    heap_block_t *next = (heap_block_t *)((uint8_t *)block + BLOCK_HEADER_SIZE + block->size);
    if ((uint64_t)next < heap_end &&
        next->magic == HEAP_BLOCK_MAGIC &&
        !(next->flags & BLOCK_FLAG_USED)) {
        remove_from_free_list(next);
        block->size += BLOCK_HEADER_SIZE + next->size;
        next->magic = 0;
    }

    // Try to merge with the previous physical block.
    for (heap_block_t *prev = free_list; prev; prev = prev->next) {
        if (prev == block) continue;
        uint64_t prev_end = (uint64_t)prev + BLOCK_HEADER_SIZE + prev->size;
        if (prev_end == (uint64_t)block) {
            remove_from_free_list(prev);
            prev->size += BLOCK_HEADER_SIZE + block->size;
            block->magic = 0;
            return prev;
        }
    }

    return block;
}

// Free allocated memory
void kfree(void *ptr) {
    if (!ptr) return;

    uint64_t irqf = heap_acquire_lock();

    // Get block header
    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - BLOCK_HEADER_SIZE);

    // Validate block
    if (block->magic != HEAP_BLOCK_MAGIC) {
        heap_release_lock(irqf);
        kprintf("[HEAP] ERROR: kfree() called on invalid pointer 0x%p!\n", ptr);
        return;
    }

    if (!(block->flags & BLOCK_FLAG_USED)) {
        heap_release_lock(irqf);
        kprintf("[HEAP] ERROR: Double free detected at 0x%p!\n", ptr);
        return;
    }

    // Mark as free
    block->flags &= ~BLOCK_FLAG_USED;

    // Update statistics
    total_freed += block->size;
    free_count++;

    // Try to coalesce with adjacent blocks (forward and backward)
    block = coalesce_blocks(block);

    // Add to free list
    add_to_free_list(block);

    heap_release_lock(irqf);
}

// Get heap statistics
size_t heap_get_total_size(void) {
    return heap_size;
}

size_t heap_get_used_size(void) {
    return total_allocated - total_freed;
}

size_t heap_get_free_size(void) {
    return heap_size - heap_get_used_size();
}

// Print heap statistics
void heap_print_stats(void) {
    kprintf("[HEAP] Heap Statistics:\n");
    kprintf("  Total size:  %lu KB\n", heap_size / KB);
    kprintf("  Used:        %lu KB\n", heap_get_used_size() / KB);
    kprintf("  Free:        %lu KB\n", heap_get_free_size() / KB);
    kprintf("  Allocations: %lu\n", allocation_count);
    kprintf("  Frees:       %lu\n", free_count);
}
