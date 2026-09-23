// heap.c - Kernel Heap Allocator implementation
// Simple linked-list based allocator with first-fit strategy

#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "../serial.h"
#include "mmlog.h"   // #dosmem: mm anomalies must reach a serial-less machine
#include "../string.h"
#include "../sync/spinlock.h"   // #114/#75: the SHARED irqsave spinlock (was a private copy)

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

// #137 (see heap_expand below)
static void heap_expand_rollback(uint64_t mapped);
static int  heap_expand_once(size_t expand_size);
extern uint64_t vmm_get_physical(uint64_t virt_addr);
extern void     vmm_unmap_page(uint64_t virt_addr);
static heap_block_t *free_list = NULL;

// Statistics
static size_t total_allocated = 0;
static size_t total_freed = 0;
static size_t allocation_count = 0;
static size_t free_count = 0;

// Spinlock for thread safety.
//
// #347: heap_lock used to be a plain busy-wait spinlock that never disabled
// interrupts. Any interrupt handler that calls kmalloc()/kfree() on the same
// CPU while foreground code already holds heap_lock (e.g. mid-split/coalesce)
// would spin on this lock from inside the ISR - forever, since the CPU that
// could release it is the one now stuck servicing the interrupt. That is a
// self-deadlock with IF effectively pinned by the interrupt gate, silently
// stopping the timer tick and every other interrupt on that core. The fix was
// to save/clear IF around the critical section so an ISR on this core can never
// observe heap_lock held mid-update, and so the critical section itself can't
// be interrupted and re-entered.
//
// #114/#75: that fix was a hand-rolled irqsave lock, a PRIVATE copy of a
// primitive that already exists in sync/spinlock.h. The identical private lock
// ten files away in mm/pmm.c was not converted alongside it and went on to
// deadlock two cores at #75. So heap_lock is now the SHARED spinlock, exactly
// as mm/pmm.c, fs/blockdev.c, fs/ext2.c and drivers/ata.c use it. The IF-masking
// semantics #347 established are preserved unchanged: spinlock_acquire_irqsave()
// does pushfq; popq; cli (identical to the old acquire), and
// spinlock_release_irqrestore() does popfq, which restores IF to its saved value
// (same net effect as the old conditional sti). The critical sections, their
// scope, and the returned-flags convention are all unchanged; only the mechanism
// is now the one shared implementation. spinlock_acquire_irqsave() itself does no
// allocation, so there is no kmalloc recursion.
static spinlock_t heap_lock = SPINLOCK_INIT_NAMED("heap");

static uint64_t heap_acquire_lock(void) {
    return spinlock_acquire_irqsave(&heap_lock);
}

static void heap_release_lock(uint64_t rflags) {
    spinlock_release_irqrestore(&heap_lock, rflags);
}

// Expand the heap by allocating more pages.
//
// #137: this used to LEAK EVERY PAGE IT HAD ALREADY TAKEN whenever it gave up
// partway. The loop below allocates and maps one page at a time, and both
// failure exits simply `return -1`: the pages taken on earlier iterations were
// never unmapped and never handed back to the PMM, and because heap_end and
// heap_size were only advanced AFTER the loop, the heap did not know about them
// either. Nothing in the system could ever reclaim them. A failure at the last
// page of a 128 MB expansion orphaned 128 MB of RAM, and the failure is
// REACHABLE FROM RING 3 (an unvalidated win_create() size, see
// rustkern/winbuf.rs), so an unprivileged app could drain physical memory until
// every kernel allocation failed - which presents as the machine stopping with
// no panic, because the thing that fails next is whatever asks for memory next.
//
// Two changes:
//   1. ROLL BACK on failure. Unmap and free exactly the pages this call took,
//      leaving the PMM and the page tables as they were found. A failed
//      expansion is now genuinely free of side effects.
//   2. DO NOT OVER-ASK. The old code always tried for HEAP_INITIAL_SIZE
//      (128 MB) even to satisfy a 4 KB allocation, so one large-ish request on
//      a machine with less than 128 MB free failed even though the memory it
//      actually needed was there. Try the big chunk first (large chunks keep
//      the free list short, which matters because find_free_block is O(n)),
//      then fall back to the smallest expansion that would actually satisfy
//      the caller before reporting failure.
static int heap_expand_once(size_t expand_size) {
    if (expand_size == 0) return -1;
    if (heap_size + expand_size > HEAP_MAX_SIZE) {
        if (heap_size >= HEAP_MAX_SIZE) {
            return -1;  // Cannot expand further
        }
        expand_size = HEAP_MAX_SIZE - heap_size;
    }
    expand_size &= ~((size_t)PMM_PAGE_SIZE - 1);
    if (expand_size < PMM_PAGE_SIZE) return -1;

    uint64_t pages_needed = expand_size / PMM_PAGE_SIZE;

    // Allocate physical pages and map them
    for (uint64_t i = 0; i < pages_needed; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            heap_expand_rollback(i);
            return -1;
        }

        uint64_t virt = heap_end + i * PMM_PAGE_SIZE;
        if (vmm_map_page(virt, phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE) != 0) {
            pmm_free_page(phys);
            heap_expand_rollback(i);
            MM_ANOMALY("[HEAP] failed to MAP a kernel heap page: the physical page was "
                       "obtained but vmm_map_page_in refused it");
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

// #137: undo a partial expansion. `mapped` is the number of pages this call
// had already mapped at [heap_end, heap_end + mapped*PAGE). heap_end has NOT
// been advanced yet, so this range is not part of the heap and nothing else can
// be looking at it; unmapping and freeing it is safe and is the only way those
// pages ever get back to the PMM.
static void heap_expand_rollback(uint64_t mapped) {
    for (uint64_t j = 0; j < mapped; j++) {
        uint64_t virt = heap_end + j * PMM_PAGE_SIZE;
        uint64_t phys = vmm_get_physical(virt);
        vmm_unmap_page(virt);
        if (phys) pmm_free_page(phys);
    }
}

// Expand the heap for a caller that needs at least `min_size` contiguous bytes.
// Prefers a large chunk (short free list) but never fails while a smaller
// expansion would have worked.
static int heap_expand(size_t min_size) {
    size_t want = (min_size + BLOCK_HEADER_SIZE + PMM_PAGE_SIZE - 1)
                  & ~((size_t)PMM_PAGE_SIZE - 1);
    size_t big  = HEAP_INITIAL_SIZE;
    if (want > big) big = want;

    if (heap_expand_once(big) == 0) return 0;
    if (want < big && heap_expand_once(want) == 0) return 0;

    kprintf("[HEAP] ERROR: expansion for %lu bytes failed (heap %lu of %lu KB)\n",
            (unsigned long)min_size, (unsigned long)(heap_size / KB),
            (unsigned long)(HEAP_MAX_SIZE / KB));
    return -1;
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
            MM_ANOMALY("[HEAP] kmalloc(%lu) FAILED: the kernel heap is exhausted. Every "
                   "caller that does not check its return now writes through NULL.", size);
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

// ===========================================================================
// #dosmem: THE ALIGNED ALLOCATOR HAD NO FREE, AND TWENTY CALL SITES.
//
// FOUND BY MEASUREMENT, not by reading. Promoting mm/'s anomaly lines to
// /BOOTLOG.TXT put this on disk on a HEALTHY 4 GB boot of build 2287:
//
//   [MM#1] [HEAP] kfree() on an INVALID pointer 0x11395300
//          (magic=0x52415453 want 0x48454150) from ra=0x478cdd
//   [MM#2] [HEAP] kfree() on an INVALID pointer 0x11395780
//          (magic=0x0 want 0x48454150) from ra=0x478cf9
//
// addr2line: drivers/hda.c:1015 and :1019, i.e. hda_free_dma_buffers()'s
// kfree(hda_state.bdl) and kfree(hda_state.dma_buffer). Both were allocated
// with kzalloc_aligned(). 0x52415453 is not a corrupted magic, it is the ASCII
// "STAR" - the four bytes that happen to live 16 bytes below an address in the
// MIDDLE of somebody else's allocation, which is exactly where an aligned
// pointer points.
//
// THE MECHANISM: kmalloc_aligned() returned an offset INTO a kmalloc block and
// stashed the real base at aligned[-1], and NOTHING IN THE TREE EVER READ THAT
// BACK. There was no kfree_aligned(), in this header or anywhere else. So every
// one of the twenty kmalloc_aligned/kzalloc_aligned sites (drivers/hda.c,
// drivers/intel_hda.c, drivers/ac97.c, drivers/intel_gpu.c, io/io_ring.c,
// exec/elf.c, video/framebuffer.c) that is ever freed leaks its whole block,
// including a PAGE_SIZE-aligned DMA buffer per audio teardown.
//
// It was ALSO silently inconsistent: `alignment <= 16` returned a plain
// kmalloc() pointer, which kfree() frees correctly. So whether kfree(p) worked
// depended on an alignment argument at a completely different call site. Half
// the callers were right by accident.
//
// THE FIX IS IN THE MECHANISM, NOT THE TWENTY INSTANCES. Adding kfree_aligned()
// and editing twenty call sites is a fix that can be half-applied, and the next
// person to add a kmalloc_aligned() has no way to learn the rule. Instead the
// aligned block now carries a TAG immediately below the pointer it hands out,
// and kfree() itself recognises it. Every existing kfree(aligned_ptr) in the
// tree becomes correct with no caller change, and so does every future one.
// kfree_aligned() exists as an explicit spelling for new code; it is a direct
// alias, not a second implementation.
//
// WHY THE TAG IS SAFE TO TRUST. kfree() consults it ONLY after the ordinary
// block magic has already failed, i.e. only on a pointer it was about to
// reject outright. It then requires all three of: a 64-bit magic, a recorded
// base inside the live heap, and that base's OWN block header carrying
// HEAP_BLOCK_MAGIC. A wild pointer that satisfies all three is not
// distinguishable from a real aligned allocation by any means available here,
// and the probability is negligible; a wild pointer that satisfies none is
// reported exactly as it was before.
// ===========================================================================
#define HEAP_ALIGN_TAG_MAGIC  0x4B414C4E5F544147ULL   // "KALN_TAG"

typedef struct {
    uint64_t magic;     // HEAP_ALIGN_TAG_MAGIC
    void    *raw;       // the kmalloc() base this aligned pointer sits inside
} heap_align_tag_t;

// Sits immediately below the returned pointer. 16 bytes, so an aligned pointer
// stays aligned for every alignment this allocator supports (>= 16).
#define HEAP_ALIGN_TAG_SIZE  ((size_t)sizeof(heap_align_tag_t))

// Allocate aligned memory. See the block comment above for why the result is
// tagged and why kfree() knows about it.
void *kmalloc_aligned(size_t size, size_t alignment) {
    // <= 16 is the natural alignment of every kmalloc() result (BLOCK_HEADER_SIZE
    // and the size rounding are both multiples of 16), so a plain block is
    // already correct AND is already freeable by kfree() with no tag.
    if (alignment <= 16) {
        return kmalloc(size);
    }
    // Reject a non-power-of-two alignment rather than computing nonsense with
    // the ~(alignment-1) mask below. Previously this produced a wrong address
    // silently.
    if (alignment & (alignment - 1)) return NULL;

    // Room to slide up to `alignment-1` bytes AND to carry the tag underneath.
    size_t extra = alignment - 1 + HEAP_ALIGN_TAG_SIZE;
    if (size + extra < size) return NULL;          // overflow
    void *raw = kmalloc(size + extra);
    if (!raw) return NULL;

    uintptr_t aligned = ((uintptr_t)raw + extra) & ~(uintptr_t)(alignment - 1);
    // aligned >= raw + HEAP_ALIGN_TAG_SIZE by construction (the mask can move
    // the address down by at most alignment-1), so the tag is inside our block.
    heap_align_tag_t *tag = (heap_align_tag_t *)(aligned - HEAP_ALIGN_TAG_SIZE);
    tag->magic = HEAP_ALIGN_TAG_MAGIC;
    tag->raw   = raw;

    return (void *)aligned;
}

// Explicit spelling for new code. kfree() handles an aligned pointer on its
// own, so this is an alias and NOT a second implementation: a private copy is
// exactly how this defect would come back.
void kfree_aligned(void *ptr) { kfree(ptr); }

// Recover the kmalloc base from an aligned pointer, or NULL if `ptr` does not
// carry a valid tag. Called ONLY from kfree(), and only after the ordinary
// block magic has failed. Must not fault: it reads at most 16 bytes below a
// pointer the caller already dereferenced (kfree read its block header there),
// and validates everything it finds before returning it.
static void *heap_align_tag_base(void *ptr) {
    if (!ptr) return NULL;
    uintptr_t a = (uintptr_t)ptr;
    if (a < heap_start + HEAP_ALIGN_TAG_SIZE) return NULL;
    if (a >= heap_start + heap_size) return NULL;

    const heap_align_tag_t *tag = (const heap_align_tag_t *)(a - HEAP_ALIGN_TAG_SIZE);
    if (tag->magic != HEAP_ALIGN_TAG_MAGIC) return NULL;

    uintptr_t raw = (uintptr_t)tag->raw;
    if (raw < heap_start + BLOCK_HEADER_SIZE) return NULL;
    if (raw >= a) return NULL;                       // the base must be BELOW us
    if (raw >= heap_start + heap_size) return NULL;

    // Third and strongest check: the recorded base must itself be a live
    // kmalloc block. A wild pointer does not get to free arbitrary memory.
    const heap_block_t *rb = (const heap_block_t *)(raw - BLOCK_HEADER_SIZE);
    if (rb->magic != HEAP_BLOCK_MAGIC) return NULL;
    if (!(rb->flags & BLOCK_FLAG_USED)) return NULL;  // already free: not ours to free again

    return (void *)raw;
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
        MM_ANOMALY("[HEAP] krealloc() on an INVALID pointer: heap metadata corruption "
                   "or a wild pointer");
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
        // #dosmem: before calling this a bad pointer, ask whether it is an
        // ALIGNED one. See the block comment above kmalloc_aligned(). The lock
        // is already released, so the recursive kfree() below is a normal
        // acquire, not a re-entry.
        {
            void *base = heap_align_tag_base(ptr);
            if (base) {
                static int reported = 0;
                if (!reported) {
                    reported = 1;
                    MM_ANOMALY("[HEAP] aligned-free path ENGAGED: kfree(%p) recovered the "
                               "kmalloc base %p from its tag and freed that instead. Before "
                               "this existed the whole block LEAKED and the call was "
                               "reported as an invalid free. Reported once per boot.",
                               ptr, base);
                }
                kfree(base);
                return;
            }
        }
        // #dosmem: NAME THE CALLER. "There is an invalid free somewhere" is not
        // actionable; an image offset an addr2line can resolve is. This line is
        // MEASURED to fire twice on a HEALTHY 4 GB boot of golden 2286 + this
        // instrument (/BOOTLOG.TXT: 0x11201a00 and 0x11201f00, 1280 bytes
        // apart), which means the kernel heap has a live pointer defect that
        // nobody could see, because kprintf goes to serial and the owner's two
        // machines have no serial port. The magic word actually found is
        // printed too: 0 says the header was zeroed (a double free that already
        // coalesced, or a freed-then-reused block), anything else says the
        // header was overwritten with data, and those are different bugs.
        MM_ANOMALY("[HEAP] kfree() on an INVALID pointer %p (magic=0x%x want 0x%x) "
                   "from ra=%p - addr2line this against kernel.dbg.elf",
                   ptr, block->magic, HEAP_BLOCK_MAGIC,
                   __builtin_return_address(0));
        kprintf("[HEAP] ERROR: kfree() called on invalid pointer 0x%p!\n", ptr);
        return;
    }

    if (!(block->flags & BLOCK_FLAG_USED)) {
        heap_release_lock(irqf);
        MM_ANOMALY("[HEAP] DOUBLE FREE at %p (size=%lu) from ra=%p - addr2line "
                   "this against kernel.dbg.elf", ptr, (unsigned long)block->size,
                   __builtin_return_address(0));
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
