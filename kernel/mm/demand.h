// demand.h - Demand Paging Support for MayteraOS
// Implements lazy allocation, page fault handling, and copy-on-write
#ifndef DEMAND_H
#define DEMAND_H

#include "../types.h"
#include "../sync/spinlock.h"   // #522 step 2: mm_struct_t::vma_lock

// Page fault error codes (from CPU)
#define PF_PRESENT      (1 << 0)    // Page was present (protection violation)
#define PF_WRITE        (1 << 1)    // Write access caused the fault
#define PF_USER         (1 << 2)    // Fault occurred in user mode
#define PF_RESERVED     (1 << 3)    // Reserved bit violation
#define PF_INSTRUCTION  (1 << 4)    // Instruction fetch (NX violation)

// Demand paging flags for VMAs (Virtual Memory Areas)
#define VMA_READ        (1 << 0)    // Readable
#define VMA_WRITE       (1 << 1)    // Writable
#define VMA_EXEC        (1 << 2)    // Executable
#define VMA_SHARED      (1 << 3)    // Shared mapping
#define VMA_PRIVATE     (1 << 4)    // Private mapping (copy-on-write)
#define VMA_ANONYMOUS   (1 << 5)    // Anonymous mapping (not file-backed)
#define VMA_FILE        (1 << 6)    // File-backed mapping
#define VMA_STACK       (1 << 7)    // Stack region (grows down)
#define VMA_HEAP        (1 << 8)    // Heap region (grows up)
#define VMA_COW         (1 << 9)    // Copy-on-write (waiting to be copied)
#define VMA_LAZY        (1 << 10)   // Lazy allocation (not yet allocated)

// Page states in the page table
#define PAGE_STATE_UNMAPPED     0   // Not in page table
#define PAGE_STATE_PRESENT      1   // Present and valid
#define PAGE_STATE_SWAPPED      2   // Swapped to disk
#define PAGE_STATE_COW          3   // Copy-on-write (shared read-only)
#define PAGE_STATE_LAZY         4   // Lazy allocation (fault will allocate)
#define PAGE_STATE_FILE         5   // File-backed (fault will load)

// Virtual Memory Area (VMA) - describes a contiguous region of virtual memory
typedef struct vma {
    uint64_t start;             // Start virtual address (page-aligned)
    uint64_t end;               // End virtual address (exclusive, page-aligned)
    uint32_t flags;             // VMA flags (VMA_*)
    uint32_t prot;              // Protection flags for mmap compatibility

    // File backing (if VMA_FILE is set)
    void *file;                 // File handle (fat_file_t*)
    uint64_t file_offset;       // Offset in file
    uint64_t file_size;         // Size of file mapping

    // COW reference counting
    uint32_t ref_count;         // Number of processes sharing this VMA

    struct vma *next;           // Next VMA in process's list
    struct vma *prev;           // Previous VMA in process's list
} vma_t;

// Process memory map (collection of VMAs)
typedef struct {
    vma_t *vma_list;            // Linked list of VMAs
    uint32_t vma_count;         // Number of VMAs

    // #522 step 2: guards vma_list / vma_count against concurrent mutation.
    // MANDATORY, not advisory: CLONE_VM threads share this exact struct, and
    // since step 1 made splitting real a mutator can kfree() a vma_t while
    // another CPU walks the list in mm_fault(). Every reader and writer of
    // vma_list takes it via mm_lock()/mm_unlock() below. See demand.c for the
    // lock order and the no-allocation-inside rule.
    spinlock_t vma_lock;

    // #421 phase 5 follow-up: how many process_t's currently hold this mm.
    // mm_create() sets this to 1 (the creator). A CLONE_VM thread (process.c
    // proc_clone(), shares_vm=1) does NOT get its own mm; it shares this
    // exact pointer, and proc_clone() calls mm_get() to bump this before the
    // new thread is runnable. cleanup_proc_slot() (process.c) calls mm_put()
    // instead of mm_destroy() directly; mm_put() only actually frees when
    // the count reaches 0. Without this, whichever process_t happened to be
    // !shares_vm (the thread GROUP LEADER) freed the mm unconditionally on
    // its own exit/cleanup, even while a shares_vm sibling thread was still
    // alive and holding the SAME pointer in its own p->mm - a real,
    // reproduced use-after-free (a crashed AssaultCube leader was reaped
    // while its just-cloned worker thread was still live; the next
    // heartbeat's proc_snapshot() walked the sibling's now-dangling p->mm
    // and panicked the kernel inside proc_mem_account_rs). All mutations of
    // this field happen while the caller holds process.c's g_proc_mm_lock
    // (proc_mm_lock()/proc_mm_unlock()), so it needs no lock of its own.
    uint32_t mm_users;

    uint64_t brk_start;         // Start of heap (after data segment)
    uint64_t brk_current;       // Current heap end

    uint64_t stack_start;       // Stack bottom (highest address)
    uint64_t stack_end;         // Stack top (lowest address, grows down)

    // #636 step 3: `mmap_base` was DELETED from here. It was a SECOND
    // next-free-address base for the same address space, seeded to 0x20000000
    // (512MB, an address in no arena this kernel uses) and consulted only by
    // do_mmap(), while the syscall path allocated from mmap_next below. Two
    // cursors over one address space is the #636 defect wearing a different
    // name, so there is now exactly one.

    // #522 step 2: the anonymous-mmap placement cursor. This lives in the mm,
    // NOT in process_t, because the ADDRESS SPACE is what it allocates from.
    // It used to be process_t::mmap_next, which is per-THREAD: proc_clone()
    // memcpy's the parent process_t, so every CLONE_VM thread started with its
    // own copy of the cursor over the SAME address space and they immediately
    // diverged. Two threads mmap()ing concurrently were handed the SAME
    // address. Before #522 step 1 that aliased silently (vma_add rejected the
    // overlap and the eager fallback mapped over it anyway); after step 1 it
    // became an outright mmap failure. Either way it is wrong, and it is only
    // correct to fix here because allocation now happens under vma_lock.
    //
    // #636 step 3: it is now also only ever READ AND WRITTEN inside the SAME
    // critical section that inserts the resulting VMA (do_mmap()), so placement
    // and insertion are atomic with respect to each other. Advancing it in one
    // critical section and inserting in a second (what the syscall path did)
    // still leaked address space on every failed insert, and still let an
    // explicit-address mapping land inside a range the cursor had already
    // handed out. It is bounded by the arena: see MM_ANON_ARENA_* in demand.c.
    uint64_t mmap_next;

    // Statistics
    uint64_t total_mapped;      // Total bytes mapped
    uint64_t resident_pages;    // Pages currently in memory
    uint64_t swapped_pages;     // Pages swapped to disk
    uint64_t cow_pages;         // Pages marked copy-on-write
    uint64_t lazy_pages;        // Pages with lazy allocation

    // Page fault statistics
    uint64_t minor_faults;      // Faults satisfied without disk I/O
    uint64_t major_faults;      // Faults requiring disk I/O
    uint64_t cow_faults;        // Copy-on-write faults
} mm_struct_t;

// Swap slot descriptor
typedef struct {
    uint32_t slot_index;        // Index in swap file/partition
    uint32_t ref_count;         // Reference count for COW
    uint64_t page_hash;         // Hash for verification
} swap_slot_t;

// Swap subsystem state
typedef struct {
    int enabled;                // Swap is enabled
    char *swap_path;            // Path to swap file/partition
    void *swap_file;            // File handle if file-backed
    uint32_t total_slots;       // Total swap slots
    uint32_t free_slots;        // Available swap slots
    uint8_t *slot_bitmap;       // Bitmap of used slots
    uint64_t swap_reads;        // Total pages read from swap
    uint64_t swap_writes;       // Total pages written to swap
} swap_state_t;

// ============================================
// Initialization
// ============================================

// Initialize demand paging subsystem
void demand_init(void);

// Initialize swap support (optional)
// Returns 0 on success, -1 on failure
int swap_init(const char *swap_path, uint64_t size_bytes);

// ============================================
// Page Fault Handling
// ============================================

// #630: demand_page_fault() was DELETED. It was a second, zero-caller page-fault
// entry point that resolved every process against one shared `static mm_struct_t`.
// The live handler is mm/fault.c -> mm_fault() below.

// #429: per-process page-fault resolver used by the real #PF handler
// (mm/fault.c). Resolves a COW write, a demand-zero / lazy mmap page or a
// file-backed page for process `p`. Returns 0 if handled, -1 if the fault is
// invalid and the process should get SIGSEGV. Works for user- and kernel-mode
// faults (the latter only for a write to a user COW page: copy_to_user).
struct process;
int mm_fault(struct process *p, uint64_t fault_addr, uint64_t error_code);

// #510/#511: proactively resolve every page in [addr, addr+len) for process
// `p` through the SAME resolver mm_fault() uses, before a Ring-0 syscall
// handler memcpy()s real data into that user destination range. Fixes the
// sys_read-into-a-fresh-buffer silent-zero-fill bug (see demand.c for the
// full writeup) at its root: call this before the copy, not after. Best
// effort / never makes a bad pointer worse - see demand.c.
void mm_prefault_range(struct process *p, uint64_t addr, uint64_t len, int for_write);

// #429: copy-on-write a single page on a write fault. Copies (or, if this is
// the last owner, simply re-enables write on) the page mapped at page_addr in
// p's address space. Returns 0 on success, -1 on OOM.
int demand_cow_write(struct process *p, uint64_t page_addr);

// #429: fork COW bookkeeping. cow_trackable() reports whether a physical page
// falls inside the COW refcount table (all PMM-allocatable pages do).
// cow_fork_share() records that one more address space now shares `phys`.
int  cow_trackable(uint64_t phys_addr);
void cow_fork_share(uint64_t phys_addr);

// #429: COW-aware free of a user leaf page. If the page is COW-shared this
// decrements its refcount and frees only when it reaches zero; otherwise it
// frees immediately. vmm_destroy_user_space()/vmm_free_user_pages() route
// every user leaf page through this so a shared page is not freed out from
// under a sibling that still references it.
void vmm_free_user_page_cow(uint64_t phys_addr);

// #429: duplicate a memory map's VMA list for fork (shallow copy, no COW
// mutation - the physical COW is handled by the page-table clone). Returns a
// new mm or NULL.
mm_struct_t *mm_dup(mm_struct_t *src);

// #429: COW flag stored in the available bits of a leaf PTE (bit 9).
#define PTE_COW_FLAG   (1ULL << 9)

// Handle different types of page faults
int handle_lazy_fault(mm_struct_t *mm, vma_t *vma, uint64_t fault_addr);
int handle_cow_fault(mm_struct_t *mm, vma_t *vma, uint64_t fault_addr);
// #630: handle_file_fault() was DELETED (unreachable, hardcoded to FAT while the
// root is ext2, and it did blocking disk I/O from the interrupts-disabled #PF
// handler). mm_fault() now refuses a VMA_FILE mapping explicitly.
int handle_swap_fault(mm_struct_t *mm, vma_t *vma, uint64_t fault_addr);

// ============================================
// VMA Management
// ============================================
//
// #629(c) LOCKING CONTRACT, and it is a CONTRACT, not a suggestion.
//
// EVERY function in this section below vma_create() operates directly on
// mm->vma_list and REQUIRES the caller to already hold mm_lock(mm). They do not
// take the lock themselves, because a caller almost always needs several of
// them to be atomic with respect to each other (find-then-split-then-remove),
// and because the pointer a lookup returns is only valid while the lock is
// held: a CLONE_VM sibling in do_munmap() can kfree() that exact node the
// instant it is dropped.
//
// They now CHECK, and report an unheld lock on serial with the caller's return
// address (see mm_require_lock() in demand.c). The check is best-effort:
// spinlock_is_locked() cannot tell "held by me" from "held by someone".
//
// The do_*() entry points below (do_mmap/do_munmap/do_mprotect/do_brk) take the
// lock THEMSELVES and must NOT be called with it held.

// Create a new VMA. Allocates; safe to call with the lock NOT held (and
// preferable, so the critical section does no allocation).
vma_t *vma_create(uint64_t start, uint64_t end, uint32_t flags);

// Add VMA to process memory map. CALLER HOLDS mm_lock(mm).
int vma_add(mm_struct_t *mm, vma_t *vma);

// Find VMA containing the given address. CALLER HOLDS mm_lock(mm), and must
// keep holding it while it uses the result.
vma_t *vma_find(mm_struct_t *mm, uint64_t addr);

// Find VMA for a range (checks overlap). CALLER HOLDS mm_lock(mm).
vma_t *vma_find_range(mm_struct_t *mm, uint64_t start, uint64_t end);

// Split VMA at the given address. CALLER HOLDS mm_lock(mm). Allocates inside
// the critical section; prefer vma_node_alloc() + vma_split_using().
int vma_split(mm_struct_t *mm, vma_t *vma, uint64_t addr);

// #522 step 2: allocation-free split for use INSIDE the mm lock. `node` must
// come from vma_node_alloc(); it is consumed on success, still owned by the
// caller on failure.
vma_t *vma_node_alloc(void);
int vma_split_using(mm_struct_t *mm, vma_t *vma, uint64_t addr, vma_t *node);

// #522 step 2: acquire/release the mm's VMA-list lock (irqsave). EVERY read or
// write of mm->vma_list must be inside one of these. mm_lock() returns the
// saved interrupt flags to hand back to mm_unlock().
uint64_t mm_lock(mm_struct_t *mm);
void mm_unlock(mm_struct_t *mm, uint64_t flags);

// Merge adjacent VMAs if possible. CALLER HOLDS mm_lock(mm).
// WARNING: on the merge-with-previous path this FREES the vma_t you passed in
// and still returns 0. Treat `vma` as consumed on return.
int vma_merge(mm_struct_t *mm, vma_t *vma);

// Remove and free VMA. CALLER HOLDS mm_lock(mm).
void vma_remove(mm_struct_t *mm, vma_t *vma);

// Free all VMAs in memory map. Teardown only: called from mm_destroy() on an
// mm whose refcount has already reached 0, i.e. one no other CPU can reach.
void vma_free_all(mm_struct_t *mm);

// ============================================
// Memory Mapping (mmap)
// ============================================

// Map an ANONYMOUS region of virtual memory. TAKES mm_lock() ITSELF; do not
// call with it held. Returns the start address, or (uint64_t)-1 on failure.
//
// addr == 0  -> placed from the per-mm cursor inside the anonymous arena.
// addr != 0  -> honoured at that address if it is a user address and the range
//               is free. NEVER maps over an existing mapping: an occupied or
//               out-of-bounds range FAILS. (`flags` MAP_FIXED is not
//               distinguished; the userland libc heap relies on a plain
//               explicit-address mmap being honoured.)
// file != NULL -> refused: file-backed mmap is not implemented (#630), and a
//               VMA_FILE VMA would be a region every access to which faults.
//
// Uses the CURRENT process's cr3 to punch identity backing out of the range.
uint64_t do_mmap(mm_struct_t *mm, uint64_t addr, uint64_t length,
                 uint32_t prot, uint32_t flags, void *file, uint64_t offset);

// Unmap a region of virtual memory. TAKES mm_lock() ITSELF.
// Clips/removes every VMA overlapping the range AND tears down the page tables
// across the WHOLE range, including parts no VMA covered (the ELF image, the
// brk heap and the user stack have no VMAs in this kernel). COW-aware, and
// never hands the PMM a frame this address space does not own.
int do_munmap(mm_struct_t *mm, uint64_t addr, uint64_t length);

// Change protection of a memory region. TAKES mm_lock() ITSELF.
int do_mprotect(mm_struct_t *mm, uint64_t addr, uint64_t length, uint32_t prot);

// Synchronize memory-mapped file
int do_msync(mm_struct_t *mm, uint64_t addr, uint64_t length, int flags);

// ============================================
// Heap Management (brk/sbrk)
// ============================================

// Set program break (heap end). TAKES mm_lock() ITSELF.
// Returns new break on success, (uint64_t)-1 on failure.
// NOTE: no caller today. proc/syscall.c's sys_brk() does its own eager
// allocation and never touches the mm, which is why the brk heap has no VMA.
uint64_t do_brk(mm_struct_t *mm, uint64_t addr);

// Increment program break
void *do_sbrk(mm_struct_t *mm, int64_t increment);

// ============================================
// Copy-on-Write Support
// ============================================

// Mark all user pages as COW for fork
int cow_mark_all(mm_struct_t *mm, uint64_t pml4_phys);

// Clone memory map with COW
mm_struct_t *mm_clone_cow(mm_struct_t *src);

// Increment COW reference count for a page
void cow_page_ref(uint64_t phys_addr);

// Decrement COW reference count, free if zero
void cow_page_unref(uint64_t phys_addr);

// Check if page is COW-shared
int cow_page_shared(uint64_t phys_addr);

// ============================================
// Swap Operations
// ============================================

// Write page to swap
// Returns swap slot index on success, (uint32_t)-1 on failure
uint32_t swap_out_page(uint64_t phys_addr);

// Read page from swap
// Returns physical address on success, 0 on failure
uint64_t swap_in_page(uint32_t slot_index);

// Free a swap slot
void swap_free_slot(uint32_t slot_index);

// Check if swap is enabled
int swap_enabled(void);

// Get swap statistics
void swap_get_stats(uint32_t *total, uint32_t *free, uint64_t *reads, uint64_t *writes);

// ============================================
// Memory Map Management
// ============================================

// Create new memory map structure
mm_struct_t *mm_create(void);

// Destroy memory map and free all resources. Unconditional: only safe to call
// on an mm no other process_t has ever seen (fresh, still-being-built, or a
// failed clone). Real teardown of a possibly-shared, already-live mm goes
// through mm_put() below instead.
void mm_destroy(mm_struct_t *mm);

// #421 phase 5 follow-up: refcounted alternative to calling mm_destroy()
// directly, for an mm a process_t may be SHARING (see mm_users above).
// mm_get() bumps the count when a new process_t starts sharing an existing
// mm (proc_clone()'s CLONE_VM thread path). mm_put() drops the count and
// only calls mm_destroy() once it reaches 0, i.e. once the LAST process_t
// referencing this mm has released it, regardless of which one (leader or
// thread) happens to exit last. Callers are expected to hold whatever lock
// guards concurrent mm teardown/lookup (process.c's g_proc_mm_lock, via
// proc_mm_lock()/proc_mm_unlock()); neither function takes a lock itself.
void mm_get(mm_struct_t *mm);
void mm_put(mm_struct_t *mm);
// #421 phase 7: refcount-decrement-and-detach; see demand.c.
void *mm_put_detach(mm_struct_t *mm);

// Clone memory map (for fork)
mm_struct_t *mm_clone(mm_struct_t *src);

// Print memory map for debugging
void mm_print(mm_struct_t *mm);

// ============================================
// Page Table Manipulation
// ============================================

// Mark page as COW (read-only, will copy on write)
int pte_mark_cow(uint64_t pml4_phys, uint64_t virt_addr);

// Mark page as lazy (not present, will allocate on access)
int pte_mark_lazy(uint64_t pml4_phys, uint64_t virt_addr);

// Mark page as swapped (store swap slot in PTE)
int pte_mark_swapped(uint64_t pml4_phys, uint64_t virt_addr, uint32_t swap_slot);

// Get page state from PTE
int pte_get_state(uint64_t pml4_phys, uint64_t virt_addr);

// Get swap slot from swapped PTE
uint32_t pte_get_swap_slot(uint64_t pml4_phys, uint64_t virt_addr);

// ============================================
// Statistics and Debugging
// ============================================

// Get demand paging statistics
void demand_get_stats(uint64_t *minor_faults, uint64_t *major_faults,
                      uint64_t *cow_faults, uint64_t *lazy_allocs);

// Print demand paging statistics
void demand_print_stats(void);

// Dump VMA list for debugging
void demand_dump_vmas(mm_struct_t *mm);

#endif // DEMAND_H
