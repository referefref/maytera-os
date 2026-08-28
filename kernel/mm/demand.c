// demand.c - Demand Paging Implementation for MayteraOS
// Implements lazy allocation, page fault handling, copy-on-write, and swap

#include "demand.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "../serial.h"
#include "../string.h"
#include "../proc/process.h"
#include "../fs/fat.h"
#include "../sync/spinlock.h"   // #114: the SHARED irqsave spinlock

#ifndef USER_STACK_SIZE
#define USER_STACK_SIZE (2 * 1024 * 1024)  // 2MB default
#endif

// ============================================
// Global State
// ============================================

// Swap subsystem state
static swap_state_t swap_state = {0};

// COW reference count table
// Maps physical page number to reference count. Sized to cover the whole
// PMM identity-mapped window (2GB -> 524288 pages, 1MB table) so EVERY
// PMM-allocatable page is COW-trackable; an untrackable COW page would be
// double-freed on exit (both owners think they own it). #429.
#define COW_TABLE_SIZE      (0x80000000ULL / VMM_PAGE_SIZE_4K)  // 524288 (2GB)
static uint16_t cow_refcount[COW_TABLE_SIZE];

// #429: set by cpu_enable_nx() (mm/fault.c) once EFER.NXE is on. The demand
// paths must NOT set the PTE NX bit before this, or accesses to those pages
// raise a reserved-bit #PF.
extern int g_nx_enabled;

// Statistics
static uint64_t stat_minor_faults = 0;
static uint64_t stat_major_faults = 0;
static uint64_t stat_cow_faults = 0;
static uint64_t stat_lazy_allocs = 0;

// #114 (#75) THE COW TABLE LOCK GOES THROUGH THE SHARED PRIMITIVE.
//
// This was a private `volatile int` + __sync_lock_test_and_set spin that never
// touched RFLAGS.IF: byte for byte the shape that deadlocked two cores in #75
// (mm/pmm.c) and the shape #347 fixed in mm/heap.c. A holder with IF=1 can be
// interrupted, and cpu/idt.c wraps every ISR in bkl_acquire(), so it becomes a
// BKL WAITER WHILE STILL HOLDING THIS LOCK; any BKL owner then wanting the
// same lock closes an AB-BA cycle in which both cores spin and neither halts.
//
// HONEST SCOPE OF THIS CHANGE. Unlike pmm_lock, cow_lock was NOT exposed, and
// this commit fixes no observed deadlock. Traced at #114, every live taker is
// already safe for one of two reasons:
//   * the page-fault path runs with IF=0 (cpu/idt.c:62 registers vector 14 as
//     IDT_GATE_INTERRUPT, not a trap gate), so it cannot be interrupted; and
//   * fork (proc/process.c:4062), exit/reap (cleanup_proc_slot) and
//     brk/munmap (proc/syscall.c:4545,4637) all reach it from a SYSCALL, which
//     took the BKL at proc/syscall.asm:89.
// So only the B-half of the inversion exists today. This is REUSE hygiene, not
// a deadlock fix: it deletes a private copy of a primitive that already lives
// ten lines below in this same file (mm_lock() -> spinlock_acquire_irqsave on
// mm->vma_lock), so there is ONE implementation to fix next time.
//
// HOLD DURATION: every critical section below is one to three operations on
// cow_refcount[idx]. Nothing allocates, blocks, does I/O or takes another lock
// while this is held: the expensive work is deliberately outside it
// (vmm_free_user_page_cow releases before pmm_free_page, and demand_cow_write
// releases before pmm_alloc_page and the 4KB copy). Masking interrupts across
// a hold this short is not a latency concern.
//
// LOCK ORDER is unchanged and still mm->vma_lock -> cow_lock (see below).
static spinlock_t cow_lock = SPINLOCK_INIT;

static uint64_t cow_acquire_lock(void) {
    return spinlock_acquire_irqsave(&cow_lock);
}

static void cow_release_lock(uint64_t flags) {
    spinlock_release_irqrestore(&cow_lock, flags);
}

// ============================================
// Initialization
// ============================================

void demand_init(void) {
    kprintf("[DEMAND] Initializing demand paging subsystem...\n");

    // Clear COW reference counts
    memset(cow_refcount, 0, sizeof(cow_refcount));

    // Swap is disabled by default
    swap_state.enabled = 0;

    kprintf("[DEMAND] Demand paging initialized\n");
    kprintf("[DEMAND]   COW table size: %u entries\n", COW_TABLE_SIZE);

    // #509 keystone: prove the copy_*_user fault-fixup is TOCTOU-safe at boot.
    // Runs here because isr_init() has already registered the #PF handler.
    extern void uaccess_toctou_selftest(void);
    uaccess_toctou_selftest();
}

int swap_init(const char *swap_path, uint64_t size_bytes) {
    if (swap_state.enabled) {
        kprintf("[SWAP] Already initialized\n");
        return -1;
    }

    kprintf("[SWAP] Initializing swap: %s, size %lu MB\n",
            swap_path, size_bytes / (1024 * 1024));

    // Calculate number of slots (one per page)
    uint32_t slots = size_bytes / VMM_PAGE_SIZE_4K;
    if (slots == 0) {
        kprintf("[SWAP] Swap file too small\n");
        return -1;
    }

    // Allocate bitmap for slot tracking
    uint32_t bitmap_size = (slots + 7) / 8;
    swap_state.slot_bitmap = kmalloc(bitmap_size);
    if (!swap_state.slot_bitmap) {
        kprintf("[SWAP] Failed to allocate slot bitmap\n");
        return -1;
    }
    memset(swap_state.slot_bitmap, 0, bitmap_size);

    // Store path
    swap_state.swap_path = kmalloc(strlen(swap_path) + 1);
    if (!swap_state.swap_path) {
        kfree(swap_state.slot_bitmap);
        return -1;
    }
    strcpy(swap_state.swap_path, swap_path);

    swap_state.total_slots = slots;
    swap_state.free_slots = slots;
    swap_state.swap_reads = 0;
    swap_state.swap_writes = 0;
    swap_state.enabled = 1;

    kprintf("[SWAP] Initialized with %u slots (%lu MB)\n",
            slots, (uint64_t)slots * VMM_PAGE_SIZE_4K / (1024 * 1024));

    return 0;
}

// ============================================
// VMA Management
// ============================================

// #522 step 2: the mm's VMA-list lock.
//
// WHY THIS EXISTS NOW. Before #522 the VMA list was comparatively static:
// sys_mmap appended, and nothing ever split or removed an interior node on the
// live path (do_munmap's partial branches did not actually work, see the
// build-990 measurements). Step 1 made splitting REAL, which makes the absence
// of a lock strictly more dangerous than it was: a mutator can now kfree() a
// vma_t while another CPU is walking the same list in mm_fault(). CLONE_VM
// threads share the IDENTICAL mm_struct_t pointer (proc_clone() copies it and
// calls mm_get()), so this is two threads of one process, not an exotic case.
//
// irqsave, deliberately. mm_fault() runs from the #PF handler, which is an
// IDT_GATE_INTERRUPT and so already has IF=0; irq_save/irq_restore nest
// correctly there (restoring to 0). It also means #514's wq_assert_may_block()
// IF check can still catch an accidental block inside a critical section,
// which matters given its known gap on plain (non-irqsave) spinlock depth.
//
// LOCK ORDER, respected by every site below: mm->vma_lock -> pmm lock, and
// mm->vma_lock -> cow_lock. Nothing acquires vma_lock while holding either, so
// the order cannot invert. proc_mm_lock() -> vma_lock is the order used by the
// procmem walker.
//
// NOTHING ALLOCATES INSIDE THE CRITICAL SECTION on the munmap/mprotect/mmap
// paths: every vma_t a split might need is allocated BEFORE the lock is taken
// (see vma_node_alloc / vma_split_using) and any unused node is freed after it
// is dropped. The one documented exception is mm_dup(); see the note there.
uint64_t mm_lock(mm_struct_t *mm) {
    if (!mm) return 0;
    return spinlock_acquire_irqsave(&mm->vma_lock);
}

void mm_unlock(mm_struct_t *mm, uint64_t flags) {
    if (!mm) return;
    spinlock_release_irqrestore(&mm->vma_lock, flags);
}

// #629(c) step 3: MAKE THE LOCK RULE SELF-REPORTING.
//
// Step 2 added the lock and took it at the four sites that were known to
// matter. That is a CONVENTION, and a convention is exactly what got the VMA
// list into this state: vma_add()/vma_find() are exported in demand.h, they are
// called from proc/syscall.c, and nothing anywhere tells a caller that it must
// hold the lock first. The next person to add a caller has no way to find out
// except by reading this file.
//
// So the caller-locked primitives now CHECK. Detection is always on (the
// #514 noblock pattern: detect always, react cheaply); the reaction is a
// rate-limited serial line with the caller's return address for addr2line, not
// a panic, because a false accusation must never be able to kill a running box
// and because the check itself is one load of an already-hot cache line.
//
// LIMITATION, stated so nobody trusts this further than it goes:
// spinlock_is_locked() answers "is this lock held by SOMEONE", not "is it held
// by ME". It therefore catches the real-world mistake (a caller that never
// takes the lock at all, which is what proc/syscall.c does today) and cannot
// catch a caller holding a DIFFERENT mm's lock. A holder identity would need a
// per-CPU owner field on spinlock_t, which is a sync/ change and not mine.
#define MM_LOCK_AUDIT_MAX  16
static uint32_t mm_lock_audit_count = 0;

static void mm_require_lock(mm_struct_t *mm, const char *what, void *ret) {
    if (!mm) return;
    if (spinlock_is_locked(&mm->vma_lock)) return;
    if (mm_lock_audit_count >= MM_LOCK_AUDIT_MAX) return;
    mm_lock_audit_count++;
    kprintf("[VMALOCK] %s() on mm %p with vma_lock UNHELD, caller %p "
            "(addr2line this against kernel.elf)\n", what, (void *)mm, ret);
    if (mm_lock_audit_count == MM_LOCK_AUDIT_MAX) {
        kprintf("[VMALOCK] further reports suppressed\n");
    }
}

#define MM_REQUIRE_LOCK(mm, what) \
    mm_require_lock((mm), (what), __builtin_return_address(0))

vma_t *vma_create(uint64_t start, uint64_t end, uint32_t flags) {
    vma_t *vma = kmalloc(sizeof(vma_t));
    if (!vma) return NULL;

    memset(vma, 0, sizeof(vma_t));
    vma->start = start & ~(VMM_PAGE_SIZE_4K - 1);  // Page-align
    vma->end = (end + VMM_PAGE_SIZE_4K - 1) & ~(VMM_PAGE_SIZE_4K - 1);
    vma->flags = flags;
    vma->ref_count = 1;

    return vma;
}

// #522 step 2: a bare, uninitialised VMA node, allocated so a caller can take
// the lock and then split WITHOUT calling kmalloc under it. Returns NULL on OOM;
// callers must treat that as "cannot split" BEFORE mutating anything.
vma_t *vma_node_alloc(void) {
    vma_t *v = kmalloc(sizeof(vma_t));
    if (v) memset(v, 0, sizeof(vma_t));
    return v;
}

// Internal, deliberately unaudited: for an mm that is NOT yet reachable by any
// other CPU (a half-built fork/clone destination). Taking a lock nobody can
// contend, purely to satisfy an assertion, would be cargo cult.
static int vma_add_nolock(mm_struct_t *mm, vma_t *vma) {
    if (!mm || !vma) return -1;

    // Insert in sorted order by start address
    vma_t *prev = NULL;
    vma_t *curr = mm->vma_list;

    while (curr && curr->start < vma->start) {
        prev = curr;
        curr = curr->next;
    }

    // Check for overlap
    if (prev && prev->end > vma->start) {
        kprintf("[VMA] Overlap with previous VMA: 0x%lx-0x%lx vs 0x%lx-0x%lx\n",
                prev->start, prev->end, vma->start, vma->end);
        return -1;
    }
    if (curr && vma->end > curr->start) {
        kprintf("[VMA] Overlap with next VMA: 0x%lx-0x%lx vs 0x%lx-0x%lx\n",
                vma->start, vma->end, curr->start, curr->end);
        return -1;
    }

    // Insert
    vma->prev = prev;
    vma->next = curr;

    if (prev) {
        prev->next = vma;
    } else {
        mm->vma_list = vma;
    }

    if (curr) {
        curr->prev = vma;
    }

    mm->vma_count++;
    mm->total_mapped += (vma->end - vma->start);

    return 0;
}

// CALLER MUST HOLD mm_lock(mm). See mm_require_lock() above.
int vma_add(mm_struct_t *mm, vma_t *vma) {
    MM_REQUIRE_LOCK(mm, "vma_add");
    return vma_add_nolock(mm, vma);
}

// CALLER MUST HOLD mm_lock(mm), AND must keep holding it for as long as it uses
// the returned pointer: a concurrent CLONE_VM sibling in do_munmap() can kfree()
// this exact node the moment the lock is dropped.
vma_t *vma_find(mm_struct_t *mm, uint64_t addr) {
    MM_REQUIRE_LOCK(mm, "vma_find");
    if (!mm) return NULL;

    vma_t *vma = mm->vma_list;
    while (vma) {
        if (addr >= vma->start && addr < vma->end) {
            return vma;
        }
        if (addr < vma->start) {
            // Past where it could be (list is sorted)
            break;
        }
        vma = vma->next;
    }

    return NULL;
}

// Internal, unaudited (see vma_add_nolock).
static vma_t *vma_find_range_nolock(mm_struct_t *mm, uint64_t start, uint64_t end) {
    if (!mm) return NULL;

    vma_t *vma = mm->vma_list;
    while (vma) {
        // Check for any overlap
        if (start < vma->end && end > vma->start) {
            return vma;
        }
        if (start >= vma->end) {
            vma = vma->next;
            continue;
        }
        break;
    }

    return NULL;
}

// CALLER MUST HOLD mm_lock(mm).
vma_t *vma_find_range(mm_struct_t *mm, uint64_t start, uint64_t end) {
    MM_REQUIRE_LOCK(mm, "vma_find_range");
    return vma_find_range_nolock(mm, start, end);
}

// #522 step 2: split using a CALLER-SUPPLIED node, so the split itself performs
// no allocation and can run inside the mm lock. `node` must come from
// vma_node_alloc() and is consumed on success; on failure the caller still owns
// it and must free it.
int vma_split_using(mm_struct_t *mm, vma_t *vma, uint64_t addr, vma_t *node) {
    MM_REQUIRE_LOCK(mm, "vma_split_using");
    if (!mm || !vma || !node) return -1;
    if (addr <= vma->start || addr >= vma->end) return -1;

    node->start     = addr;
    node->end       = vma->end;
    node->flags     = vma->flags;
    node->prot      = vma->prot;
    node->ref_count = 1;

    if (vma->flags & VMA_FILE) {
        node->file        = vma->file;
        node->file_offset = vma->file_offset + (addr - vma->start);
        node->file_size   = vma->file_size - (addr - vma->start);
        if (node->file_size > vma->end - addr) node->file_size = vma->end - addr;
    }

    vma->end = addr;

    node->prev = vma;
    node->next = vma->next;
    if (vma->next) vma->next->prev = node;
    vma->next = node;

    mm->vma_count++;
    return 0;
}

// CALLER MUST HOLD mm_lock(mm). NOTE this variant kmalloc()s INSIDE the
// critical section the caller is holding. kmalloc() does not block (heap.c
// takes an irqsave spinlock, #347), so it is safe, but it lengthens the
// critical section; do_munmap()/do_mprotect() deliberately use the
// vma_node_alloc() + vma_split_using() pair instead.
int vma_split(mm_struct_t *mm, vma_t *vma, uint64_t addr) {
    MM_REQUIRE_LOCK(mm, "vma_split");
    if (!mm || !vma) return -1;
    if (addr <= vma->start || addr >= vma->end) return -1;

    // Create new VMA for upper half
    vma_t *upper = vma_create(addr, vma->end, vma->flags);
    if (!upper) return -1;

    // Copy file info if applicable
    if (vma->flags & VMA_FILE) {
        upper->file = vma->file;
        upper->file_offset = vma->file_offset + (addr - vma->start);
        upper->file_size = vma->file_size - (addr - vma->start);
        if (upper->file_size > vma->end - addr) {
            upper->file_size = vma->end - addr;
        }
    }

    // Adjust original VMA
    vma->end = addr;

    // Insert upper half after vma
    upper->prev = vma;
    upper->next = vma->next;
    if (vma->next) {
        vma->next->prev = upper;
    }
    vma->next = upper;

    mm->vma_count++;

    return 0;
}

// CALLER MUST HOLD mm_lock(mm).
//
// DANGER, and the reason this has no callers: on the merge-with-previous path
// this function kfree()s the vma_t THE CALLER PASSED IN and returns 0. The
// caller's pointer is dangling on return. Any future caller must treat `vma` as
// consumed and re-look-up whatever it needs. Left as-is rather than "improved"
// into returning the survivor, because changing the signature of an
// exported-but-unused function is churn; the contract is now at least written
// down where a caller will read it.
int vma_merge(mm_struct_t *mm, vma_t *vma) {
    MM_REQUIRE_LOCK(mm, "vma_merge");
    if (!mm || !vma) return -1;

    // Try to merge with next VMA
    vma_t *next = vma->next;
    if (next && vma->end == next->start && vma->flags == next->flags) {
        // Check file backing compatibility
        if ((vma->flags & VMA_FILE) && (next->flags & VMA_FILE)) {
            if (vma->file != next->file) return 0;  // Different files
            if (vma->file_offset + (vma->end - vma->start) != next->file_offset) {
                return 0;  // Non-contiguous file regions
            }
        }

        // Merge
        vma->end = next->end;
        vma->next = next->next;
        if (next->next) {
            next->next->prev = vma;
        }

        kfree(next);
        mm->vma_count--;
    }

    // Try to merge with previous VMA
    vma_t *prev = vma->prev;
    if (prev && prev->end == vma->start && prev->flags == vma->flags) {
        if ((vma->flags & VMA_FILE) && (prev->flags & VMA_FILE)) {
            if (vma->file != prev->file) return 0;
            if (prev->file_offset + (prev->end - prev->start) != vma->file_offset) {
                return 0;
            }
        }

        prev->end = vma->end;
        prev->next = vma->next;
        if (vma->next) {
            vma->next->prev = prev;
        }

        kfree(vma);
        mm->vma_count--;
    }

    return 0;
}

// CALLER MUST HOLD mm_lock(mm).
void vma_remove(mm_struct_t *mm, vma_t *vma) {
    MM_REQUIRE_LOCK(mm, "vma_remove");
    if (!mm || !vma) return;

    // Unlink from list
    if (vma->prev) {
        vma->prev->next = vma->next;
    } else {
        mm->vma_list = vma->next;
    }

    if (vma->next) {
        vma->next->prev = vma->prev;
    }

    mm->vma_count--;
    mm->total_mapped -= (vma->end - vma->start);

    kfree(vma);
}

void vma_free_all(mm_struct_t *mm) {
    if (!mm) return;

    vma_t *vma = mm->vma_list;
    while (vma) {
        vma_t *next = vma->next;
        kfree(vma);
        vma = next;
    }

    mm->vma_list = NULL;
    mm->vma_count = 0;
    mm->total_mapped = 0;
}

// ============================================
// Page Fault Handling
// ============================================

// #630: demand_page_fault() DELETED (was here).
//
// It was a SECOND page-fault entry point with ZERO callers, and it resolved
// every process's faults against ONE `static mm_struct_t default_mm` shared
// process-wide, with hardcoded brk/stack/mmap constants that match nothing this
// kernel actually uses. The live path is mm/fault.c -> mm_fault(), which is
// per-process. Leaving a plausible-looking, wrong, zero-caller fault handler
// next to the real one is a trap for the next reader, not a fallback.
//
// #630: handle_file_fault() DELETED too (was below). It was unreachable
// (VMA_FILE was only ever set by do_mmap(), which itself had zero callers), it
// was hardcoded to fat_seek()/fat_read() while the shipping root has been ext2
// since #365, and it performed BLOCKING DISK I/O from the #PF handler - which
// runs on an IDT_GATE_INTERRUPT with interrupts disabled, exactly what
// wq_assert_may_block() (#514) exists to catch. File-backed mmap needs a VFS
// path, I/O outside the fault handler, and a page cache; each is its own
// ticket. mm_fault() now refuses VMA_FILE explicitly instead.

int handle_lazy_fault(mm_struct_t *mm, vma_t *vma, uint64_t fault_addr) {
    // #745 (local 75) CLASS FIX: the faulting address space belongs to the
    // task running on THIS cpu. Through the BSP-published global, a fault
    // taken on an AP walked and edited the BSP task's PML4.
    extern process_t *proc_current(void);
    process_t *me = proc_current();
    uint64_t pml4 = me->cr3;

    // Allocate physical page
    uint64_t phys_page = pmm_alloc_page();
    if (phys_page == 0) {
        kprintf("[DEMAND] Out of memory for lazy allocation at 0x%lx\n", fault_addr);
        // TODO: Try to swap out pages
        return -1;
    }

    // Zero the page (security and for BSS)
    memset((void*)phys_page, 0, VMM_PAGE_SIZE_4K);

    // Build page flags
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (vma->flags & VMA_WRITE) {
        flags |= VMM_FLAG_WRITABLE;
    }
    if (g_nx_enabled && !(vma->flags & VMA_EXEC)) {
        flags |= VMM_FLAG_NX;   // #429 W^X: writable data pages are no-execute
    }

    // Map the page
    uint64_t page_addr = fault_addr & ~(VMM_PAGE_SIZE_4K - 1);
    if (vmm_map_page_in(pml4, page_addr, phys_page, flags) != 0) {
        pmm_free_page(phys_page);
        kprintf("[DEMAND] Failed to map lazy page at 0x%lx\n", page_addr);
        return -1;
    }

    mm->resident_pages++;
    mm->lazy_pages--;
    stat_lazy_allocs++;
    stat_minor_faults++;

    return 0;
}

int handle_cow_fault(mm_struct_t *mm, vma_t *vma, uint64_t fault_addr) {
    // #745 (local 75) CLASS FIX: the faulting address space belongs to the
    // task running on THIS cpu. Through the BSP-published global, a fault
    // taken on an AP walked and edited the BSP task's PML4.
    extern process_t *proc_current(void);
    process_t *me = proc_current();
    uint64_t pml4 = me->cr3;

    uint64_t page_addr = fault_addr & ~(VMM_PAGE_SIZE_4K - 1);

    // Get current physical page
    uint64_t old_phys = vmm_get_physical_in(pml4, page_addr);
    if (old_phys == 0) {
        // Page not present - just allocate
        return handle_lazy_fault(mm, vma, fault_addr);
    }

    // Check if we're the only reference
    uint64_t cowfl = cow_acquire_lock();
    uint32_t page_index = old_phys / VMM_PAGE_SIZE_4K;
    if (page_index < COW_TABLE_SIZE && cow_refcount[page_index] <= 1) {
        // We're the only reference - just make writable
        cow_release_lock(cowfl);

        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE;
        // #629: this was the ONE NX site in this file missing the g_nx_enabled
        // guard every other one has. Setting the NX bit before EFER.NXE is on
        // makes bit 63 a RESERVED bit, and the next access to the page takes a
        // reserved-bit #PF instead of succeeding. It is currently defused one
        // layer down (#640 made vmm_map_page_in() strip NX when NXE is clear),
        // but relying on that leaves this site wrong on its own terms.
        if (g_nx_enabled && !(vma->flags & VMA_EXEC)) {
            flags |= VMM_FLAG_NX;
        }

        if (vmm_map_page_in(pml4, page_addr, old_phys, flags) != 0) {
            return -1;
        }

        // Clear COW flag from VMA if all pages are copied
        // (simplified - in reality we'd track per-page)
        stat_cow_faults++;
        return 0;
    }
    cow_release_lock(cowfl);

    // Multiple references - need to copy
    uint64_t new_phys = pmm_alloc_page();
    if (new_phys == 0) {
        kprintf("[DEMAND] Out of memory for COW at 0x%lx\n", fault_addr);
        return -1;
    }

    // Copy page contents
    memcpy((void*)new_phys, (void*)old_phys, VMM_PAGE_SIZE_4K);

    // Map new page as writable
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE;
    if (g_nx_enabled && !(vma->flags & VMA_EXEC)) {
        flags |= VMM_FLAG_NX;   // #429 W^X: writable data pages are no-execute
    }

    if (vmm_map_page_in(pml4, page_addr, new_phys, flags) != 0) {
        pmm_free_page(new_phys);
        return -1;
    }

    // Decrement old page reference
    cow_page_unref(old_phys);

    mm->cow_pages--;
    mm->resident_pages++;
    stat_cow_faults++;

    return 0;
}

int handle_swap_fault(mm_struct_t *mm, vma_t *vma, uint64_t fault_addr) {
    // #745 (local 75) CLASS FIX: the faulting address space belongs to the
    // task running on THIS cpu. Through the BSP-published global, a fault
    // taken on an AP walked and edited the BSP task's PML4.
    extern process_t *proc_current(void);
    process_t *me = proc_current();
    uint64_t pml4 = me->cr3;

    if (!swap_state.enabled) {
        kprintf("[DEMAND] Swap fault but swap disabled at 0x%lx\n", fault_addr);
        return -1;
    }

    uint64_t page_addr = fault_addr & ~(VMM_PAGE_SIZE_4K - 1);

    // Get swap slot from PTE
    uint32_t slot = pte_get_swap_slot(pml4, page_addr);
    if (slot == (uint32_t)-1) {
        kprintf("[DEMAND] Invalid swap slot at 0x%lx\n", fault_addr);
        return -1;
    }

    // Read page from swap
    uint64_t phys_page = swap_in_page(slot);
    if (phys_page == 0) {
        kprintf("[DEMAND] Failed to swap in page at 0x%lx\n", fault_addr);
        return -1;
    }

    // Build page flags
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (vma->flags & VMA_WRITE) {
        flags |= VMM_FLAG_WRITABLE;
    }
    if (g_nx_enabled && !(vma->flags & VMA_EXEC)) {
        flags |= VMM_FLAG_NX;   // #429 W^X: writable data pages are no-execute
    }

    // Map the page
    if (vmm_map_page_in(pml4, page_addr, phys_page, flags) != 0) {
        pmm_free_page(phys_page);
        return -1;
    }

    // Free swap slot
    swap_free_slot(slot);

    mm->resident_pages++;
    mm->swapped_pages--;
    stat_major_faults++;

    return 0;
}

// ============================================
// Memory Mapping (mmap)
// ============================================

// #636: the anonymous-mmap placement ARENA.
//
// These are NOT new limits and they do NOT widen anything: the cursor already
// started at USER_WIN_MMAP_BASE (proc/syscall.c DEFAULT_MMAP_START) and the
// next arena above it is the stage-4 stack. What is new is that the arena has
// an END at all. `mmap_next += length` had no ceiling, so a process that
// mmap'd 4GB in total (or asked for one absurd length) walked the cursor
// straight out of the arena, and because vma_add() can only see VMAs - and the
// ELF image, the brk heap and the user stack have NO VMAs in this kernel - the
// insert SUCCEEDED and mmap returned an address aliasing another live region.
//
// Deliberately NOT touched: the 2-3GB legacy window and its MMIO. Nothing here
// places anything there.
#define MM_ANON_ARENA_BASE  USER_WIN_MMAP_BASE
#define MM_ANON_ARENA_END   USER_WIN_STACK_TOP

// #629(a): sanity bounds for an EXPLICIT (caller-supplied) mmap address.
// The libc heap grows by mmap()ing at an explicit address (userland
// stdlib.c ensure_mapped()), so explicit addresses MUST keep working; what
// must not keep working is an explicit address that is not a user address at
// all. An address in the kernel half, or above the user window, previously
// went straight through to vma_add() + vmm_punch_demand_range(), and the punch
// clears exactly the PRESENT-and-not-USER leaves that the kernel's own
// identity mappings are made of.
//
// HONEST LIMIT: this cannot protect the low 2-3GB, because in this kernel
// physical == virtual and user space genuinely starts at 4MB, on top of the
// same identity map the kernel runs from (see the #511 writeup in vmm.c).
// PROC_DEFAULT_BRK_START is 5MB. A range check there would be structurally
// wrong, not merely strict. This check therefore rejects the two cases that
// are unambiguously never user memory: below USER_SPACE_START, and at or above
// USER_WIN_END.
#define MM_USER_ADDR_MIN    USER_SPACE_START
#define MM_USER_ADDR_MAX    USER_WIN_END

// First-fit gap scan from `from`, bounded by the arena. CALLER HOLDS THE LOCK.
// Returns 0 when no gap fits, which callers must treat as failure (0 is not a
// legal mapping address here: the arena base is 512GB+4GB).
static uint64_t mm_scan_gap_locked(mm_struct_t *mm, uint64_t from, uint64_t length) {
    uint64_t cand = from;
    if (cand < MM_ANON_ARENA_BASE) cand = MM_ANON_ARENA_BASE;

    for (vma_t *v = mm->vma_list; v; v = v->next) {
        if (v->end <= cand) continue;              // entirely below the candidate
        if (v->start >= cand + length) break;      // the gap before v is big enough
        cand = v->end;                             // pushed past this VMA
    }

    if (cand + length < cand) return 0;                 // overflow
    if (cand + length > MM_ANON_ARENA_END) return 0;    // arena exhausted
    return cand;
}

// #636: pick an address for an anonymous mapping. CALLER HOLDS THE LOCK, and
// the caller inserts the VMA before dropping it, so placement and insertion are
// one atomic step. That is the whole point: the old code advanced the cursor in
// one critical section and inserted in a SECOND one, which leaked address space
// on every failed insert and let an explicit-address mapping land inside a
// range the cursor had already promised to someone else.
static uint64_t mm_place_anon_locked(mm_struct_t *mm, uint64_t length) {
    if (length == 0 || length > (MM_ANON_ARENA_END - MM_ANON_ARENA_BASE)) return 0;

    uint64_t cursor = mm->mmap_next;
    if (cursor < MM_ANON_ARENA_BASE || cursor >= MM_ANON_ARENA_END) {
        cursor = MM_ANON_ARENA_BASE;
    }

    uint64_t addr = mm_scan_gap_locked(mm, cursor, length);
    if (addr == 0 && cursor != MM_ANON_ARENA_BASE) {
        // One wrap. Without this the arena is a one-way ratchet: munmap()ed
        // space below the cursor was never reusable and a long-running process
        // died of address-space exhaustion with almost nothing mapped.
        addr = mm_scan_gap_locked(mm, MM_ANON_ARENA_BASE, length);
    }
    return addr;
}

// #629/#636: anonymous mmap. ONE critical section covers placement, the overlap
// decision, the VMA insert, the identity punch and the cursor update.
//
// Uses the CURRENT process's cr3 for the punch, exactly as do_munmap() does:
// there is no cr3 in mm_struct_t, and every live caller is operating on its own
// address space (a CLONE_VM sibling shares the same cr3, which is precisely the
// case that matters here).
//
// FAILS rather than overwrites. There is no eager fallback and no MAP_FIXED
// replace path: if the requested range is occupied or out of bounds, the call
// returns -1 having changed nothing. The one thing this must never do is what
// the pre-#629 syscall path did, which was to map fresh zero pages over a live
// range and report success.
uint64_t do_mmap(mm_struct_t *mm, uint64_t addr, uint64_t length,
                 uint32_t prot, uint32_t flags, void *file, uint64_t offset) {
    (void)offset;
    if (!mm || length == 0) return (uint64_t)-1;

    // File-backed mapping is not implemented (#630 deleted the fault handler).
    // Creating a VMA_FILE VMA would produce a region every access to which
    // mm_fault() refuses, i.e. a guaranteed SIGSEGV dressed up as a successful
    // mmap. Refuse at the door instead.
    if (file) return (uint64_t)-1;

    uint64_t len = (length + VMM_PAGE_SIZE_4K - 1) & ~(VMM_PAGE_SIZE_4K - 1);
    if (len < length) return (uint64_t)-1;      // rounding overflowed

    process_t *cur = proc_current();
    uint64_t pml4 = cur ? cur->cr3 : 0;
    if (pml4 == 0) return (uint64_t)-1;

    // Protection -> VMA flags. Mirrors what the syscall path has always done,
    // including "no PROT bits at all means anonymous read/write", which is what
    // the userland libc relies on.
    uint32_t vflags = VMA_LAZY | VMA_READ | VMA_ANONYMOUS;
    if (prot & 0x2) vflags |= VMA_WRITE;        // PROT_WRITE
    if (prot & 0x4) vflags |= VMA_EXEC;         // PROT_EXEC
    if ((prot & 0x7) == 0) vflags |= VMA_WRITE;
    vflags |= (flags & 0x1) ? VMA_SHARED : VMA_PRIVATE;   // MAP_SHARED

    // Allocate the node BEFORE the lock: nothing allocates inside it.
    vma_t *node = vma_node_alloc();
    if (!node) return (uint64_t)-1;

    uint64_t irq = mm_lock(mm);

    uint64_t start;
    if (addr == 0) {
        start = mm_place_anon_locked(mm, len);
    } else {
        start = addr & ~(VMM_PAGE_SIZE_4K - 1);
        if (start < MM_USER_ADDR_MIN || start + len <= start ||
            start + len > MM_USER_ADDR_MAX) {
            start = 0;                                  // not a user range
        } else if (vma_find_range_nolock(mm, start, start + len)) {
            start = 0;                                  // occupied: do NOT overwrite
        }
    }

    if (start == 0) {
        mm_unlock(mm, irq);
        kfree(node);
        return (uint64_t)-1;
    }

    node->start     = start;
    node->end       = start + len;
    node->flags     = vflags;
    node->prot      = prot;
    node->ref_count = 1;

    if (vma_add_nolock(mm, node) != 0) {
        mm_unlock(mm, irq);
        kfree(node);
        return (uint64_t)-1;
    }

    // #511: strip identity backing so the first touch (from Ring 3 OR from a
    // Ring-0 copy_to_user) takes a real fault and gets a real USER page.
    vmm_punch_demand_range(pml4, start, len);

    mm->lazy_pages += len / VMM_PAGE_SIZE_4K;

    // Advance the cursor ONLY on success, and only for a mapping that actually
    // lands in the arena. An explicit-address mapping inside the arena also
    // advances it, so a later cursor allocation cannot be handed a range an
    // explicit mapping already owns.
    if (start >= MM_ANON_ARENA_BASE && start + len <= MM_ANON_ARENA_END &&
        start + len > mm->mmap_next) {
        mm->mmap_next = start + len;
    }

    mm_unlock(mm, irq);
    return start;
}

// #522: tear down the page-table backing for [from, to) in `pml4`.
//
// TWO invariants the original open-coded loops got wrong:
//   1. #628 COW SAFETY. A page may be shared with another address space (fork
//      marks the PTE, not the VMA, so `vma->flags & VMA_COW` is ALWAYS false on
//      the live path and the old code always took the raw pmm_free_page()
//      branch). Every other user-page free in this tree goes through
//      vmm_free_user_page_cow(), which drops one reference and frees only at
//      zero. PROVEN LIVE, not theorised: mmtest (D) on build 990 had a child
//      munmap() a COW-shared region, and because pmm_alloc_page() is first-fit
//      the child's very next mmap() got the parent's physical pages straight
//      back; the parent then read the child's pattern in all 4096 words.
//   2. NEVER FREE A FRAME WE DO NOT OWN. A page inside a VMA that was never
//      faulted in can still be PRESENT as leftover kernel IDENTITY backing (the
//      2-3GB window is pre-filled with identity huge pages, #511). Those frames
//      belong to the identity map, not to this process. Drop the mapping, but
//      handing one to the PMM would corrupt unrelated memory. The USER bit is
//      what distinguishes a real demand-allocated user page from identity
//      backing, so it gates the free.
static void vma_teardown_pages(mm_struct_t *mm, uint64_t pml4,
                               uint64_t from, uint64_t to) {
    for (uint64_t page = from; page < to; page += VMM_PAGE_SIZE_4K) {
        uint64_t pte = vmm_get_pte_in(pml4, page);
        if (!(pte & VMM_FLAG_PRESENT)) continue;

        uint64_t phys = pte & VMM_ADDR_MASK;
        int owned = (pte & VMM_FLAG_USER) != 0;

        vmm_unmap_page_in(pml4, page);
        if (owned) {
            vmm_free_user_page_cow(phys);      // COW-aware; frees only at refcount 0
            if (mm->resident_pages) mm->resident_pages--;
        }
    }
}

// #522/#628/#629: correct-by-construction reference implementation of munmap.
//
// Rewritten rather than patched, because a differential against the ORIGINAL
// would have inherited its defects instead of catching them (the #433 lesson:
// a differential cannot catch a bug both arms share). The original had four:
//   (a) #628 raw pmm_free_page() on COW-shared pages -> cross-process
//       use-after-free. See vma_teardown_pages() above for the live evidence.
//   (b) #629 it started at vma_find(mm, start), which returns NULL when `start`
//       lands in a HOLE. A munmap spanning a gap and then a real mapping
//       unmapped NOTHING and still returned 0.
//   (c) it ignored vma_split() failure (kmalloc exhaustion), reporting success
//       having done nothing, or worse having done half.
//   (d) its "unmap start of VMA" branch iterated `page >= start` over a range
//       it had already split, and its "unmap end" branch had a provably dead
//       else-arm. Both are gone in favour of one clip-then-remove shape.
//
// POSIX semantics kept deliberately: unmapping a range that is wholly or partly
// unmapped SUCCEEDS. A partial unmap inside a VMA splits it; it never frees the
// whole thing.
int do_munmap(mm_struct_t *mm, uint64_t addr, uint64_t length) {
    if (!mm || length == 0) return -1;

    process_t *cur = proc_current();
    uint64_t pml4 = cur ? cur->cr3 : 0;
    if (pml4 == 0) return -1;

    uint64_t start = addr & ~(VMM_PAGE_SIZE_4K - 1);
    uint64_t end   = (addr + length + VMM_PAGE_SIZE_4K - 1) & ~(VMM_PAGE_SIZE_4K - 1);
    if (end <= start) return -1;      // zero-length after rounding, or overflow

    // #522 step 2: at most TWO splits can be needed for any range, however many
    // VMAs it spans: one at `start` (in the first overlapping VMA) and one at
    // `end` (in the last). Allocate both BEFORE taking the lock so the critical
    // section performs no allocation, and free whatever went unused after.
    vma_t *nodeA = vma_node_alloc();
    vma_t *nodeB = vma_node_alloc();
    if (!nodeA || !nodeB) {
        if (nodeA) kfree(nodeA);
        if (nodeB) kfree(nodeB);
        return -1;
    }

    int rc = 0;
    uint64_t irq = mm_lock(mm);

    vma_t *vma = mm->vma_list;
    while (vma) {
        vma_t *next = vma->next;

        if (vma->end <= start) { vma = next; continue; }   // entirely below
        if (vma->start >= end) break;                      // list is sorted: done

        // Clip this VMA to the requested range, splitting so that whatever
        // survives outside [start, end) keeps its own identity and flags.
        if (vma->start < start) {
            if (!nodeA || vma_split_using(mm, vma, start, nodeA) != 0) { rc = -1; break; }
            nodeA = NULL;                // consumed
            vma  = vma->next;            // the piece beginning at `start`
            next = vma->next;
        }
        if (vma->end > end) {
            if (!nodeB || vma_split_using(mm, vma, end, nodeB) != 0) { rc = -1; break; }
            nodeB = NULL;                // consumed
            next = vma->next;            // the survivor above `end`
        }

        // `vma` is now exactly the overlap with [start, end).
        vma_remove(mm, vma);
        vma = next;
    }

    // #629(b) step 3: tear the PAGES down over the WHOLE requested range, not
    // just the parts a VMA covered.
    //
    // The caller (proc/syscall.c sys_munmap) chose between two half-correct
    // implementations on the value of vma_find(mm, addr), and BOTH halves are
    // wrong for a range that mixes mapped and unmapped parts:
    //
    //   addr lands in a VMA  -> this function ran, and (before this change) it
    //       tore down only the pages VMAs covered. The ELF image, the brk heap
    //       and the user stack have NO VMAs in this kernel, so any part of the
    //       range backed by one of those kept its pages while munmap returned 0.
    //
    //   addr lands in a HOLE -> vma_find() returns NULL, so this function was
    //       never called at all and the caller fell back to
    //       vmm_free_user_pages() over the raw range. That frees PTEs but does
    //       not touch the VMA list, so every VMA inside the range SURVIVED. The
    //       call returned 0 having unmapped nothing in the only sense that
    //       lasts: the next touch of the "unmapped" address faults, finds the
    //       surviving VMA, and is satisfied with a fresh demand-zero page.
    //
    // Reproduce the second one in three calls: p = mmap(0, 8 pages);
    // munmap(p, 2 pages) (clips the VMA to [p+2, p+8)); munmap(p, 8 pages) -
    // now `p` is a hole, so the whole range takes the fallback and the VMA
    // [p+2, p+8) is still in the list afterwards.
    //
    // vma_teardown_pages() is the right tool for the whole range because it is
    // COW-aware (#628) and refuses to hand the PMM a frame this address space
    // does not own (identity backing has no USER bit). The old fallback,
    // vmm_free_user_pages(), is COW-aware but is NOT USER-gated, so it could
    // free kernel identity frames.
    //
    // Done AFTER the VMA walk and only when it succeeded: unmapping is allowed
    // to fail, but it must not destroy pages and then report failure.
    //
    // BEHAVIOUR CHANGE, stated plainly: munmap() now really does unmap
    // everything in [addr, addr+len), including parts backed by no VMA. That is
    // POSIX, and it is what the sys_munmap fallback already did for the
    // hole-start case, but it means a caller that passes an over-long length
    // now destroys more than it used to. Requires the sys_munmap patch (see the
    // report) to be reachable for a hole-start range.
    if (rc == 0) {
        vma_teardown_pages(mm, pml4, start, end);
    }

    mm_unlock(mm, irq);

    if (nodeA) kfree(nodeA);
    if (nodeB) kfree(nodeB);
    return rc;
}

// #522: apply a VMA's protection to the already-present pages of [from, to).
//
// Pages that are NOT present are deliberately left alone: they are lazy, and
// handle_lazy_fault() will read the (now updated) VMA flags when it faults them
// in, so the new protection applies there too without touching a PTE here.
static void vma_reprotect_pages(mm_struct_t *mm, uint64_t pml4,
                                uint64_t from, uint64_t to,
                                uint32_t vflags) {
    int any_access = (vflags & (VMA_READ | VMA_WRITE | VMA_EXEC)) != 0;

    for (uint64_t page = from; page < to; page += VMM_PAGE_SIZE_4K) {
        uint64_t pte = vmm_get_pte_in(pml4, page);
        if (!(pte & VMM_FLAG_PRESENT)) continue;
        if (!(pte & VMM_FLAG_USER))    continue;   // identity backing, not ours

        uint64_t phys = pte & VMM_ADDR_MASK;

        // PROT_NONE has no PTE encoding that keeps the page reachable, so drop
        // the mapping. The VMA stays, and because it now grants no access,
        // mm_fault() rejects any touch and the process gets SIGSEGV. That half
        // was always correct and is proven by /APPS/MMTEST subtest (9).
        //
        // #404 CORRECTION, and the comment that used to be here was CONFIDENTLY
        // WRONG IN BOTH DIRECTIONS. It claimed "the frame is NOT freed:
        // mprotect(PROT_NONE) must not destroy data, and a later mprotect back
        // to PROT_READ must find it". Neither half held, and nobody had noticed
        // because do_mprotect() had ZERO CALLERS from the day it was written
        // until the syscall was wired up: this was unverified code that read
        // like verified code.
        //
        //   * The data was destroyed anyway. vmm_unmap_page_in() zeroes the
        //     PTE, which is the only record of the physical frame. Nothing
        //     anywhere remembered it, so a later mprotect back to PROT_READ
        //     took the lazy path and got a FRESH ZEROED page. The round trip
        //     silently returned zeros.
        //   * And the frame LEAKED, permanently. With the PTE zeroed, the frame
        //     is unreachable, and vma_teardown_pages() skips anything that is
        //     not PRESENT, so process exit did not reclaim it either. Every
        //     mprotect(PROT_NONE) burned one page per page until reboot, which
        //     an unprivileged app can do in a loop.
        //
        // So free it, COW-aware, exactly as vma_teardown_pages() does. Every
        // page reaching this line is already known PRESENT and USER (both are
        // checked above), so it is genuinely ours to free.
        //
        // BEHAVIOUR, STATED PLAINLY BECAUSE IT DIVERGES FROM POSIX:
        // mprotect(PROT_NONE) now DISCARDS the contents of the range. POSIX
        // preserves them. This is deliberate, it is documented in the libc
        // header, and it is the honest version of what the code already did:
        // the previous behaviour also lost the data, it just leaked the frame
        // as well. Preserving contents needs the frame parked somewhere the
        // PTE can no longer hold it (a software-bit "parked" PTE encoding, with
        // matching cases in vma_teardown_pages() and the fault path) and that
        // is its own ticket, not a side effect of wiring up a syscall. Nothing
        // in this tree calls mprotect at all today, so nothing regresses; the
        // uses this primitive exists for (guard pages, poisoning a freed
        // region, W^X) do not read the range back.
        if (!any_access) {
            vmm_unmap_page_in(pml4, page);
            vmm_free_user_page_cow(phys);
            if (mm && mm->resident_pages) mm->resident_pages--;
            continue;
        }

        uint64_t nf = VMM_FLAG_PRESENT | VMM_FLAG_USER;

        // #522 COW INTERACTION. A fork-shared page must STAY read-only with its
        // COW bit intact even when the new protection is writable. Granting
        // WRITABLE here would un-share the page WITHOUT copying it, so the
        // writer's stores would land in the frame the other process is still
        // reading - silently reintroducing exactly the cross-process corruption
        // #628 is about, by a different route. Leave it read-only and let
        // demand_cow_write() do the copy on the resulting fault; that path is
        // reached precisely BECAUSE we did not set WRITABLE.
        if (pte & PTE_COW_FLAG) {
            nf |= PTE_COW_FLAG;
        } else if (vflags & VMA_WRITE) {
            nf |= VMM_FLAG_WRITABLE;
        }
        if (g_nx_enabled && !(vflags & VMA_EXEC)) nf |= VMM_FLAG_NX;

        vmm_map_page_in(pml4, page, phys, nf);
    }
}

// #522: real mprotect. The original required the whole range to sit inside ONE
// VMA and could not split, so it could not change the protection of a
// sub-range at all - which is the only interesting case, and the one the ticket
// asks for. It also rewrote every PTE as PRESENT|USER|WRITABLE without
// preserving PTE_COW_FLAG, so an mprotect over a post-fork region silently
// made shared pages writable.
//
// POSIX semantics: if any part of [addr, addr+len) is not mapped, the call
// FAILS and changes NOTHING. That is why the coverage scan below runs to
// completion before a single VMA is touched.
int do_mprotect(mm_struct_t *mm, uint64_t addr, uint64_t length, uint32_t prot) {
    if (!mm || length == 0) return -1;

    process_t *cur = proc_current();
    uint64_t pml4 = cur ? cur->cr3 : 0;
    if (pml4 == 0) return -1;

    uint64_t start = addr & ~(VMM_PAGE_SIZE_4K - 1);
    uint64_t end   = (addr + length + VMM_PAGE_SIZE_4K - 1) & ~(VMM_PAGE_SIZE_4K - 1);
    if (end <= start) return -1;

    uint32_t vbits = 0;
    if (prot & 0x1) vbits |= VMA_READ;
    if (prot & 0x2) vbits |= VMA_WRITE;
    if (prot & 0x4) vbits |= VMA_EXEC;

    // Same two-node pre-allocation as do_munmap: at most one split at `start`
    // and one at `end`, both allocated outside the lock.
    vma_t *nodeA = vma_node_alloc();
    vma_t *nodeB = vma_node_alloc();
    if (!nodeA || !nodeB) {
        if (nodeA) kfree(nodeA);
        if (nodeB) kfree(nodeB);
        return -1;
    }

    int rc = 0;
    uint64_t irq = mm_lock(mm);

    // Whole range must be mapped, with no holes, BEFORE anything changes. This
    // scan is inside the lock so the coverage it proves is the same state the
    // mutation below then acts on.
    uint64_t covered = start;
    for (vma_t *v = mm->vma_list; v && covered < end; v = v->next) {
        if (v->end <= covered) continue;
        if (v->start > covered) break;      // hole: fail without side effects
        covered = v->end;
    }

    if (covered < end) {
        rc = -1;
    } else {
        vma_t *vma = mm->vma_list;
        while (vma) {
            vma_t *next = vma->next;

            if (vma->end <= start) { vma = next; continue; }
            if (vma->start >= end) break;

            if (vma->start < start) {
                if (!nodeA || vma_split_using(mm, vma, start, nodeA) != 0) { rc = -1; break; }
                nodeA = NULL;
                vma  = vma->next;
                next = vma->next;
            }
            if (vma->end > end) {
                if (!nodeB || vma_split_using(mm, vma, end, nodeB) != 0) { rc = -1; break; }
                nodeB = NULL;
                next = vma->next;
            }

            vma->flags = (vma->flags & ~(VMA_READ | VMA_WRITE | VMA_EXEC)) | vbits;
            vma->prot  = prot;
            vma_reprotect_pages(mm, pml4, vma->start, vma->end, vma->flags);

            vma = next;
        }
    }

    mm_unlock(mm, irq);

    if (nodeA) kfree(nodeA);
    if (nodeB) kfree(nodeB);
    return rc;
}

int do_msync(mm_struct_t *mm, uint64_t addr, uint64_t length, int flags) {
    // TODO: Implement file sync for memory-mapped files
    (void)mm;
    (void)addr;
    (void)length;
    (void)flags;
    return 0;
}

// ============================================
// Heap Management
// ============================================

// #629: VMA-backed brk.
//
// Three defects fixed here, all of the same family as the munmap ones:
//   (a) SHRINK CALLED pmm_free_page() RAW. Every other user-page free in this
//       tree goes through the COW-aware path, because after fork() the heap
//       pages are shared with the child. A shrinking brk handed frames the
//       child still maps straight back to the allocator; the next allocation in
//       ANY process got them, and the child's heap changed under it. That is
//       #628 again, in brk instead of munmap. It also never checked the USER
//       bit, so a page inside the heap range that was still leftover kernel
//       identity backing (the 2-3GB window is pre-filled, #511) was donated to
//       the PMM. vma_teardown_pages() gets both right.
//   (b) EXPAND WROTE heap_vma->end = new_brk WITH NO OVERLAP CHECK. The list is
//       kept sorted and non-overlapping by vma_add(); extending an existing
//       node's end past the next node's start breaks that invariant directly.
//       After it, vma_find() resolves addresses inside the NEXT mapping to the
//       heap VMA, so faults there are satisfied with heap protections and a
//       munmap of the heap tears down pages belonging to the other mapping.
//   (c) NO LOCK, on a list a CLONE_VM sibling can be splitting concurrently.
//
// NOTE this function has no callers today: sys_brk() (proc/syscall.c) does its
// own eager vmm_alloc_user_pages() and never touches the mm. That is a
// SEPARATE defect (the brk heap has no VMA, which is half of why munmap of it
// used to be a no-op), and fixing it is a proc/ change, not an mm/ one.
uint64_t do_brk(mm_struct_t *mm, uint64_t addr) {
    if (!mm) return (uint64_t)-1;

    process_t *cur = proc_current();
    uint64_t pml4 = cur ? cur->cr3 : 0;
    if (pml4 == 0) return (uint64_t)-1;

    // Return current brk if addr is 0
    if (addr == 0) {
        return mm->brk_current;
    }

    // Align to page boundary
    uint64_t new_brk = (addr + VMM_PAGE_SIZE_4K - 1) & ~(VMM_PAGE_SIZE_4K - 1);
    if (new_brk < addr) return (uint64_t)-1;        // rounding overflowed

    // Check bounds
    if (new_brk < mm->brk_start) {
        return (uint64_t)-1;
    }

    // Pre-allocate outside the lock; freed below if the heap VMA already exists.
    vma_t *node = (new_brk > mm->brk_current) ? vma_node_alloc() : NULL;
    if (new_brk > mm->brk_current && !node) return (uint64_t)-1;

    int rc = 0;
    uint64_t irq = mm_lock(mm);

    if (new_brk > mm->brk_current) {
        vma_t *heap_vma = vma_find(mm, mm->brk_start);
        if (!heap_vma) {
            node->start     = mm->brk_start;
            node->end       = new_brk;
            node->flags     = VMA_READ | VMA_WRITE | VMA_HEAP | VMA_LAZY;
            node->ref_count = 1;
            if (vma_add_nolock(mm, node) != 0) rc = -1;
            else node = NULL;                       // consumed
        } else if (heap_vma->next && new_brk > heap_vma->next->start) {
            rc = -1;                                // would overlap the next VMA
        } else if (new_brk > heap_vma->end) {
            mm->total_mapped += (new_brk - heap_vma->end);
            heap_vma->end = new_brk;
        }

        if (rc == 0) {
            mm->lazy_pages += (new_brk - mm->brk_current) / VMM_PAGE_SIZE_4K;
        }
    } else if (new_brk < mm->brk_current) {
        // COW-aware, USER-gated teardown of the pages being given back.
        vma_teardown_pages(mm, pml4, new_brk, mm->brk_current);

        vma_t *heap_vma = vma_find(mm, mm->brk_start);
        if (heap_vma && heap_vma->end > new_brk && heap_vma->start <= new_brk) {
            mm->total_mapped -= (heap_vma->end - new_brk);
            heap_vma->end = new_brk;
        }
    }

    if (rc == 0) mm->brk_current = new_brk;
    uint64_t result = rc == 0 ? new_brk : (uint64_t)-1;

    mm_unlock(mm, irq);
    if (node) kfree(node);
    return result;
}

void *do_sbrk(mm_struct_t *mm, int64_t increment) {
    if (!mm) return (void*)-1;

    uint64_t old_brk = mm->brk_current;
    uint64_t new_brk = old_brk + increment;

    if (do_brk(mm, new_brk) == (uint64_t)-1) {
        return (void*)-1;
    }

    return (void*)old_brk;
}

// ============================================
// Copy-on-Write Support
// ============================================

// #629(c): walks AND mutates (vma->flags |= VMA_COW) the list, so it takes the
// lock. Bounded work inside: page-table writes and cow_page_ref(), which takes
// cow_lock, which is below vma_lock in the documented order.
int cow_mark_all(mm_struct_t *mm, uint64_t pml4_phys) {
    if (!mm) return -1;

    uint64_t irq = mm_lock(mm);
    vma_t *vma = mm->vma_list;
    while (vma) {
        // Skip shared mappings and non-writable regions
        if ((vma->flags & VMA_SHARED) || !(vma->flags & VMA_WRITE)) {
            vma = vma->next;
            continue;
        }

        // Mark pages as COW (read-only)
        for (uint64_t addr = vma->start; addr < vma->end; addr += VMM_PAGE_SIZE_4K) {
            uint64_t phys = vmm_get_physical_in(pml4_phys, addr);
            if (phys) {
                pte_mark_cow(pml4_phys, addr);
                cow_page_ref(phys);
                mm->cow_pages++;
            }
        }

        // Mark VMA as COW
        vma->flags |= VMA_COW;
        vma = vma->next;
    }

    mm_unlock(mm, irq);
    return 0;
}

mm_struct_t *mm_clone_cow(mm_struct_t *src) {
    if (!src) return NULL;

    mm_struct_t *dst = mm_create();
    if (!dst) return NULL;

    // Copy basic fields. #629(c): everything that reads or mutates src's VMA
    // list happens under src's lock (it mutates src too: it sets VMA_COW on the
    // SOURCE VMAs). dst is not reachable by any other CPU yet, so it needs no
    // lock and uses the _nolock inserter.
    dst->brk_start = src->brk_start;
    dst->brk_current = src->brk_current;
    dst->stack_start = src->stack_start;
    dst->stack_end = src->stack_end;
    dst->mmap_next = src->mmap_next;    // #636: the child inherits the cursor

    // Clone VMAs (share pages with COW)
    uint64_t sirq = mm_lock(src);
    vma_t *src_vma = src->vma_list;
    while (src_vma) {
        vma_t *dst_vma = vma_create(src_vma->start, src_vma->end, src_vma->flags);
        if (!dst_vma) {
            mm_unlock(src, sirq);
            mm_destroy(dst);
            return NULL;
        }

        dst_vma->prot = src_vma->prot;
        dst_vma->file = src_vma->file;
        dst_vma->file_offset = src_vma->file_offset;
        dst_vma->file_size = src_vma->file_size;

        // For writable private mappings, enable COW
        if ((src_vma->flags & VMA_WRITE) && !(src_vma->flags & VMA_SHARED)) {
            dst_vma->flags |= VMA_COW;
            src_vma->flags |= VMA_COW;
        }

        if (vma_add_nolock(dst, dst_vma) != 0) {
            kfree(dst_vma);
            mm_unlock(src, sirq);
            mm_destroy(dst);
            return NULL;
        }

        src_vma = src_vma->next;
    }
    mm_unlock(src, sirq);

    // Copy statistics
    dst->resident_pages = src->resident_pages;
    dst->cow_pages = src->cow_pages;
    dst->lazy_pages = src->lazy_pages;

    return dst;
}

void cow_page_ref(uint64_t phys_addr) {
    uint32_t page_index = phys_addr / VMM_PAGE_SIZE_4K;
    if (page_index >= COW_TABLE_SIZE) return;

    uint64_t cowfl = cow_acquire_lock();
    if (cow_refcount[page_index] < 0xFFFF) {
        cow_refcount[page_index]++;
    }
    cow_release_lock(cowfl);
}

void cow_page_unref(uint64_t phys_addr) {
    uint32_t page_index = phys_addr / VMM_PAGE_SIZE_4K;
    if (page_index >= COW_TABLE_SIZE) return;

    uint64_t cowfl = cow_acquire_lock();
    if (cow_refcount[page_index] > 0) {
        cow_refcount[page_index]--;
        if (cow_refcount[page_index] == 0) {
            cow_release_lock(cowfl);
            pmm_free_page(phys_addr);
            return;
        }
    }
    cow_release_lock(cowfl);
}

int cow_page_shared(uint64_t phys_addr) {
    uint32_t page_index = phys_addr / VMM_PAGE_SIZE_4K;
    if (page_index >= COW_TABLE_SIZE) return 0;

    uint64_t cowfl = cow_acquire_lock();
    int shared = (cow_refcount[page_index] > 1);
    cow_release_lock(cowfl);
    return shared;
}

// ============================================
// Swap Operations
// ============================================

static uint32_t swap_alloc_slot(void) {
    if (!swap_state.enabled || swap_state.free_slots == 0) {
        return (uint32_t)-1;
    }

    for (uint32_t i = 0; i < swap_state.total_slots; i++) {
        uint32_t byte = i / 8;
        uint32_t bit = i % 8;
        if (!(swap_state.slot_bitmap[byte] & (1 << bit))) {
            swap_state.slot_bitmap[byte] |= (1 << bit);
            swap_state.free_slots--;
            return i;
        }
    }

    return (uint32_t)-1;
}

uint32_t swap_out_page(uint64_t phys_addr __attribute__((unused))) {
    if (!swap_state.enabled) return (uint32_t)-1;

    uint32_t slot = swap_alloc_slot();
    if (slot == (uint32_t)-1) return (uint32_t)-1;

    // TODO: Write page to swap file
    // For now, we just track the slot
    // uint64_t offset = (uint64_t)slot * VMM_PAGE_SIZE_4K;
    // fat_seek(swap_state.swap_file, offset);
    // fat_write(swap_state.swap_file, (void*)phys_addr, VMM_PAGE_SIZE_4K);

    swap_state.swap_writes++;
    return slot;
}

uint64_t swap_in_page(uint32_t slot_index) {
    if (!swap_state.enabled || slot_index >= swap_state.total_slots) {
        return 0;
    }

    // Allocate physical page
    uint64_t phys_page = pmm_alloc_page();
    if (phys_page == 0) return 0;

    // TODO: Read page from swap file
    // uint64_t offset = (uint64_t)slot_index * VMM_PAGE_SIZE_4K;
    // fat_seek(swap_state.swap_file, offset);
    // fat_read(swap_state.swap_file, (void*)phys_page, VMM_PAGE_SIZE_4K);

    swap_state.swap_reads++;
    return phys_page;
}

void swap_free_slot(uint32_t slot_index) {
    if (!swap_state.enabled || slot_index >= swap_state.total_slots) {
        return;
    }

    uint32_t byte = slot_index / 8;
    uint32_t bit = slot_index % 8;
    if (swap_state.slot_bitmap[byte] & (1 << bit)) {
        swap_state.slot_bitmap[byte] &= ~(1 << bit);
        swap_state.free_slots++;
    }
}

int swap_enabled(void) {
    return swap_state.enabled;
}

void swap_get_stats(uint32_t *total, uint32_t *free, uint64_t *reads, uint64_t *writes) {
    if (total) *total = swap_state.total_slots;
    if (free) *free = swap_state.free_slots;
    if (reads) *reads = swap_state.swap_reads;
    if (writes) *writes = swap_state.swap_writes;
}

// ============================================
// Memory Map Management
// ============================================

mm_struct_t *mm_create(void) {
    mm_struct_t *mm = kmalloc(sizeof(mm_struct_t));
    if (!mm) return NULL;

    memset(mm, 0, sizeof(mm_struct_t));

    // #522 step 2: every mm gets its own VMA-list lock. mm_dup() builds the
    // child through mm_create(), so a forked mm gets a FRESH, unlocked lock
    // rather than inheriting the parent's (possibly held) state.
    spinlock_init_named(&mm->vma_lock, "mm.vma");

    // #636: ONE placement cursor, seeded to the arena base.
    //
    // mm_struct_t used to carry a SECOND placement base, `mmap_base`, seeded
    // here to 0x20000000 (512MB) - an address in no arena this kernel uses -
    // and do_mmap() allocated from it while the syscall path allocated from
    // mmap_next. Two independent next-free pointers over one address space is
    // the same defect #636 is about (a per-thread cursor over a per-mm
    // resource), just with the second copy named differently instead of living
    // somewhere else. The field is gone; there is now exactly one cursor.
    mm->mmap_next = MM_ANON_ARENA_BASE;

    // #421 phase 5 follow-up: the creator is the first (and, for the common
    // never-shared case, only) reference. See demand.h's mm_users comment.
    mm->mm_users = 1;

    return mm;
}

void mm_destroy(mm_struct_t *mm) {
    if (!mm) return;

    // Free all VMAs
    vma_free_all(mm);

    kfree(mm);
}

// #421 phase 5 follow-up: see demand.h for the full writeup of the bug this
// fixes (a crashed thread-group leader's cleanup freeing an mm a sibling
// CLONE_VM thread was still actively using, because the old code decided
// "who frees it" from a single process_t's own shares_vm flag instead of
// counting real references). Neither function takes a lock: every existing
// caller (process.c's proc_clone() and cleanup_proc_slot()) already holds
// g_proc_mm_lock across the whole read-modify-(maybe-free) sequence.
void mm_get(mm_struct_t *mm) {
    if (!mm) return;
    mm->mm_users++;
}

void mm_put(mm_struct_t *mm) {
    if (!mm) return;
    if (mm->mm_users > 0) mm->mm_users--;
    if (mm->mm_users == 0) mm_destroy(mm);
}

// #421 phase 7: decrement mm_users and RETURN the mm that should now be
// destroyed (refcount reached 0), or NULL, WITHOUT freeing it. This lets
// cleanup_proc_slot() (process.c) do the refcount step inside its short,
// non-preemptible g_proc_mm_lock critical section and then run the actual
// (unbounded: vma_free_all + kfree) teardown OUTSIDE the lock. Holding the
// lock across that free is what let a preempted holder wedge the whole box in
// an AB-BA cycle with the BKL (silent both-cpus-halted hang past AssaultCube
// map load); see process.c for the full writeup.
void *mm_put_detach(mm_struct_t *mm) {
    if (!mm) return NULL;
    if (mm->mm_users > 0) mm->mm_users--;
    return (mm->mm_users == 0) ? (void *)mm : NULL;
}

mm_struct_t *mm_clone(mm_struct_t *src) {
    return mm_clone_cow(src);
}

void mm_print(mm_struct_t *mm) {
    if (!mm) {
        kprintf("[MM] NULL memory map\n");
        return;
    }

    kprintf("[MM] Memory Map:\n");
    kprintf("  Heap: 0x%lx - 0x%lx\n", mm->brk_start, mm->brk_current);
    kprintf("  Stack: 0x%lx - 0x%lx\n", mm->stack_end, mm->stack_start);
    kprintf("  mmap cursor: 0x%lx\n", mm->mmap_next);
    kprintf("  VMAs: %u, Total mapped: %lu bytes\n", mm->vma_count, mm->total_mapped);
    kprintf("  Resident: %lu, COW: %lu, Lazy: %lu, Swapped: %lu\n",
            mm->resident_pages, mm->cow_pages, mm->lazy_pages, mm->swapped_pages);
}

// ============================================
// Page Table Manipulation
// ============================================

// We use special bits in the PTE for demand paging:
// - Bit 9 (AVL): COW flag
// - Bit 10 (AVL): Swapped flag
// - Bits 12-51: Swap slot index (when swapped)

#define PTE_COW_BIT     (1ULL << 9)
#define PTE_SWAP_BIT    (1ULL << 10)

int pte_mark_cow(uint64_t pml4_phys, uint64_t virt_addr) {
    // Read the whole PTE, not just the frame: the flags we must PRESERVE live
    // in it. #629/#640: this used to rebuild the entry as
    // PRESENT|USER|COW, which DROPS NX. Marking a page copy-on-write is
    // supposed to remove write permission, not grant execute permission, but on
    // a kernel that now enforces W^X (#640 made VMM_USER_RW carry NX) that is
    // exactly what it did: every writable data page in the address space came
    // out of fork() executable. Keep NX; clearing WRITABLE is the only change.
    uint64_t pte = vmm_get_pte_in(pml4_phys, virt_addr);
    if (!(pte & VMM_FLAG_PRESENT)) return -1;

    uint64_t phys  = pte & VMM_ADDR_MASK;
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | PTE_COW_BIT |
                     (pte & VMM_FLAG_NX);
    return vmm_map_page_in(pml4_phys, virt_addr, phys, flags);
}

int pte_mark_lazy(uint64_t pml4_phys, uint64_t virt_addr) {
    // Simply unmap - the VMA will handle the fault
    vmm_unmap_page_in(pml4_phys, virt_addr);
    return 0;
}

int pte_mark_swapped(uint64_t pml4_phys, uint64_t virt_addr, uint32_t swap_slot) {
    // Create a special PTE with swap slot encoded
    uint64_t pte = PTE_SWAP_BIT | ((uint64_t)swap_slot << 12);
    return vmm_map_page_in(pml4_phys, virt_addr, pte, 0);
}

int pte_get_state(uint64_t pml4_phys, uint64_t virt_addr) {
    uint64_t phys = vmm_get_physical_in(pml4_phys, virt_addr);

    if (phys == 0) {
        return PAGE_STATE_UNMAPPED;
    }

    // Check for swap marker
    if (phys & PTE_SWAP_BIT) {
        return PAGE_STATE_SWAPPED;
    }

    // Check for COW
    // Note: This is simplified - in reality we'd need to read the actual PTE
    return PAGE_STATE_PRESENT;
}

uint32_t pte_get_swap_slot(uint64_t pml4_phys, uint64_t virt_addr) {
    uint64_t phys = vmm_get_physical_in(pml4_phys, virt_addr);
    if (!(phys & PTE_SWAP_BIT)) {
        return (uint32_t)-1;
    }
    return (uint32_t)(phys >> 12);
}

// ============================================
// Statistics and Debugging
// ============================================

void demand_get_stats(uint64_t *minor_faults, uint64_t *major_faults,
                      uint64_t *cow_faults, uint64_t *lazy_allocs) {
    if (minor_faults) *minor_faults = stat_minor_faults;
    if (major_faults) *major_faults = stat_major_faults;
    if (cow_faults) *cow_faults = stat_cow_faults;
    if (lazy_allocs) *lazy_allocs = stat_lazy_allocs;
}

void demand_print_stats(void) {
    kprintf("[DEMAND] Statistics:\n");
    kprintf("  Minor faults: %lu\n", stat_minor_faults);
    kprintf("  Major faults: %lu\n", stat_major_faults);
    kprintf("  COW faults: %lu\n", stat_cow_faults);
    kprintf("  Lazy allocations: %lu\n", stat_lazy_allocs);

    if (swap_state.enabled) {
        kprintf("[SWAP] Statistics:\n");
        kprintf("  Total slots: %u\n", swap_state.total_slots);
        kprintf("  Free slots: %u\n", swap_state.free_slots);
        kprintf("  Pages read: %lu\n", swap_state.swap_reads);
        kprintf("  Pages written: %lu\n", swap_state.swap_writes);
    }
}

void demand_dump_vmas(mm_struct_t *mm) {
    if (!mm) return;

    // #629(c): a debug dump that walks an unlocked list is still a list walk;
    // a concurrent munmap frees nodes under it. kprintf() to the serial port
    // inside an irqsave critical section is slow but does not block (#426), and
    // this is a debug-only path.
    uint64_t irq = mm_lock(mm);
    kprintf("[DEMAND] VMA List:\n");
    vma_t *vma = mm->vma_list;
    int i = 0;
    while (vma) {
        kprintf("  [%d] 0x%lx - 0x%lx (%lu KB) flags=0x%x",
                i++, vma->start, vma->end,
                (vma->end - vma->start) / 1024, vma->flags);

        if (vma->flags & VMA_READ) kprintf(" R");
        if (vma->flags & VMA_WRITE) kprintf("W");
        if (vma->flags & VMA_EXEC) kprintf("X");
        if (vma->flags & VMA_SHARED) kprintf(" shared");
        if (vma->flags & VMA_PRIVATE) kprintf(" private");
        if (vma->flags & VMA_ANONYMOUS) kprintf(" anon");
        if (vma->flags & VMA_FILE) kprintf(" file");
        if (vma->flags & VMA_STACK) kprintf(" stack");
        if (vma->flags & VMA_HEAP) kprintf(" heap");
        if (vma->flags & VMA_COW) kprintf(" cow");
        if (vma->flags & VMA_LAZY) kprintf(" lazy");

        kprintf("\n");
        vma = vma->next;
    }
    mm_unlock(mm, irq);
}

// ============================================
// #429: per-process fault resolver + real COW
// ============================================

int cow_trackable(uint64_t phys_addr) {
    return (phys_addr / VMM_PAGE_SIZE_4K) < COW_TABLE_SIZE;
}

// Record that one more address space now shares `phys` (called once per shared
// page as fork marks it COW). The first time a page becomes shared its count
// jumps to 2 (the parent that already mapped it + the new child).
void cow_fork_share(uint64_t phys_addr) {
    uint32_t idx = phys_addr / VMM_PAGE_SIZE_4K;
    if (idx >= COW_TABLE_SIZE) return;
    uint64_t cowfl = cow_acquire_lock();
    if (cow_refcount[idx] == 0) cow_refcount[idx] = 2;
    else if (cow_refcount[idx] < 0xFFFF) cow_refcount[idx]++;
    cow_release_lock(cowfl);
}

// COW-aware free of a user leaf page (see demand.h).
void vmm_free_user_page_cow(uint64_t phys_addr) {
    uint32_t idx = phys_addr / VMM_PAGE_SIZE_4K;
    if (idx < COW_TABLE_SIZE) {
        uint64_t cowfl = cow_acquire_lock();
        if (cow_refcount[idx] > 0) {
            cow_refcount[idx]--;
            int last = (cow_refcount[idx] == 0);
            cow_release_lock(cowfl);
            if (last) pmm_free_page(phys_addr);
            return;
        }
        cow_release_lock(cowfl);
    }
    pmm_free_page(phys_addr);
}

// Copy-on-write a single page on a write fault.
int demand_cow_write(struct process *p, uint64_t page_addr) {
    uint64_t pml4 = p->cr3;
    uint64_t pte = vmm_get_pte_in(pml4, page_addr);
    if (!(pte & VMM_FLAG_PRESENT)) return -1;

    uint64_t old_phys = pte & VMM_ADDR_MASK;
    uint64_t keep_nx  = pte & VMM_FLAG_NX;   // preserve W^X on the fresh copy
    uint32_t idx = old_phys / VMM_PAGE_SIZE_4K;

    int shared = 0;
    uint64_t cowfl = cow_acquire_lock();
    if (idx < COW_TABLE_SIZE && cow_refcount[idx] > 1) shared = 1;
    cow_release_lock(cowfl);

    uint64_t new_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE | keep_nx;

    if (!shared) {
        // Sole owner: drop the COW bit and re-enable write in place.
        if (vmm_map_page_in(pml4, page_addr, old_phys, new_flags) != 0) return -1;
        if (idx < COW_TABLE_SIZE) {
            cowfl = cow_acquire_lock();
            if (cow_refcount[idx] == 1) cow_refcount[idx] = 0;  // now private
            cow_release_lock(cowfl);
        }
        stat_cow_faults++;
        return 0;
    }

    // Shared: allocate a private copy.
    uint64_t new_phys = pmm_alloc_page();
    if (new_phys == 0) {
        kprintf("[DEMAND] COW OOM at 0x%lx\n", page_addr);
        return -1;
    }
    memcpy((void *)new_phys, (void *)old_phys, VMM_PAGE_SIZE_4K);
    if (vmm_map_page_in(pml4, page_addr, new_phys, new_flags) != 0) {
        pmm_free_page(new_phys);
        return -1;
    }
    // This address space no longer shares the old page. new_phys is private
    // (refcount stays 0). Drop one reference on the old page.
    cow_page_unref(old_phys);
    stat_cow_faults++;
    return 0;
}

// Per-process page-fault resolver (see demand.h). Called by the #PF handler.
int mm_fault(struct process *p, uint64_t fault_addr, uint64_t error_code) {
    if (!p || p->cr3 == 0) return -1;

    uint64_t page_addr = fault_addr & ~(VMM_PAGE_SIZE_4K - 1);
    uint64_t pml4 = p->cr3;
    uint64_t pte = vmm_get_pte_in(pml4, page_addr);

    if (pte & VMM_FLAG_PRESENT) {
        // Recoverable case 1: a write to a read-only COW page (works for user-
        // AND kernel-mode writes so copy_to_user into a fork-shared buffer is
        // handled too).
        if ((error_code & PF_WRITE) && (pte & PTE_COW_FLAG) &&
            !(pte & VMM_FLAG_WRITABLE)) {
            // #522: consult the VMA before honouring the PTE's COW bit.
            // Honouring it unconditionally is an mprotect BYPASS: a page that
            // mprotect() made read-only still carries the COW bit set by fork,
            // so the first write took the COW path and came back WRITABLE,
            // silently undoing the protection. It is the same hole for W^X
            // (#526), which will express "no write" the same way. Only a VMA
            // that actually permits writing may take the COW copy.
            //
            // Deliberately NOT rejected when there is no covering VMA: an
            // ELF-loaded region has no VMA at all in this kernel, and a
            // kernel-mode copy_to_user() into a fork-shared buffer there is a
            // legitimate COW write that must still resolve.
            mm_struct_t *cmm = (mm_struct_t *)p->mm;
            int deny = 0;
            if (cmm) {
                uint64_t cirq = mm_lock(cmm);
                vma_t *cv = vma_find(cmm, fault_addr);
                deny = (cv && !(cv->flags & VMA_WRITE));
                mm_unlock(cmm, cirq);
            }
            if (deny) return -1;
            return demand_cow_write(p, page_addr);
        }
        // Recoverable case 2: a USER access faulted on a page that is present
        // but lacks the USER bit. This happens inside a demand region when an
        // earlier fault in the same 2MB span split a kernel identity huge page:
        // the sibling 4KB entries inherit PRESENT|WRITABLE but not USER. If a
        // demand VMA covers the address, map a real user page over the stale
        // kernel entry.
        //
        // IMPORTANT (found and reverted in the same pass that added it, see
        // mm_prefault_range() below for the real fix): an earlier version of
        // this branch tried to "preserve" whatever was already at the PRESENT
        // physical frame here (on the theory that a prior Ring-0 write, e.g.
        // sys_read's kernel-mode memcpy, might already have put real data
        // there) by re-mapping the SAME physical page with the USER bit added
        // instead of calling handle_lazy_fault(). That is wrong: the PRESENT
        // frame at this point is not evidence of anything written on
        // purpose. It is inherited from splitting a KERNEL IDENTITY huge page
        // (see the comment above), i.e. it is whatever physical memory
        // happens to sit at that address, not memory ever allocated for this
        // VMA. For the (far more common) case where Ring 3 touches the page
        // FIRST, before any kernel-side write ever targets it (a plain
        // malloc'd buffer the app itself writes), "preserving" that content
        // leaks stale/unrelated bytes into a supposedly fresh anonymous page
        // instead of zeroing it, corrupting anything that assumed demand-zero
        // semantics. Reproduced live: AssaultCube's very first heap-arena
        // allocation (its addcommand() console-command pool, touched by a
        // C++ global constructor before main() even runs) came back as
        // garbage under that version of this branch and crashed with an
        // Invalid Opcode a few instructions later, decoding whatever noise
        // was inherited at that physical address as code operands. Reverted
        // to the original, always-safe handle_lazy_fault() (zero a genuinely
        // FRESH page) for this branch; the actual root-cause fix for the
        // #510/#511 sys_read-zero-fill bug is mm_prefault_range() below,
        // called PROACTIVELY by sys_read() before its memcpy, so the
        // destination page is correctly demand-zeroed and marked present
        // BEFORE the kernel writes real data into it, rather than trying to
        // retroactively guess whether stale PRESENT content was meaningful.
        if ((error_code & PF_USER) && !(pte & VMM_FLAG_USER)) {
            mm_struct_t *mm2 = (mm_struct_t *)p->mm;
            if (mm2) {
                uint64_t irq2 = mm_lock(mm2);
                vma_t *v2 = vma_find(mm2, fault_addr);
                if (v2 && !(v2->flags & VMA_FILE)) {
                    int r2;
                    if ((error_code & PF_WRITE) && !(v2->flags & VMA_WRITE)) r2 = -1;
                    else r2 = handle_lazy_fault(mm2, v2, fault_addr);
                    mm_unlock(mm2, irq2);
                    return r2;
                }
                mm_unlock(mm2, irq2);
            }
        }
        return -1;  // NX exec fault, or a genuine protection violation
    }

    // Not present: satisfy from the process's VMA list (demand-zero / lazy
    // mmap or file-backed). Requires a per-process mm.
    mm_struct_t *mm = (mm_struct_t *)p->mm;
    if (!mm) return -1;

    // #522 step 2: the lookup AND the handler run under the mm lock. Splitting
    // this into "find under the lock, then drop it and use the pointer" would
    // be exactly the use-after-free the lock exists to prevent: a concurrent
    // munmap on a CLONE_VM sibling can kfree() this vma_t between the two.
    // handle_lazy_fault() is bounded (one pmm_alloc_page, one 4KB memset, one
    // page-table write) and takes only the pmm lock, which is strictly below
    // this one in the lock order, so holding it here cannot invert.
    uint64_t irq = mm_lock(mm);

    vma_t *vma = vma_find(mm, fault_addr);
    if (!vma) { mm_unlock(mm, irq); return -1; }

    // A user access must be to a page the VMA actually permits.
    if ((error_code & PF_WRITE) && !(vma->flags & VMA_WRITE)) { mm_unlock(mm, irq); return -1; }
    if ((error_code & PF_USER) &&
        !(vma->flags & (VMA_READ | VMA_WRITE | VMA_EXEC))) { mm_unlock(mm, irq); return -1; }

    // #630: file-backed mapping is NOT implemented. VMA_FILE can only be set by
    // a caller that supplies a file handle, and #522 rejects that at the mmap
    // syscall, so this is unreachable today; it is an explicit refusal rather
    // than a silent demand-zero, so a future file-mmap cannot quietly hand an
    // app a page of zeros where it asked for file contents. See the deletion
    // note above handle_lazy_fault() for why the old handler had to go.
    if (vma->flags & VMA_FILE) { mm_unlock(mm, irq); return -1; }

    // Anonymous / lazy: demand-zero a fresh page.
    int r = handle_lazy_fault(mm, vma, fault_addr);
    mm_unlock(mm, irq);
    return r;
}

// #510/#511 ROOT-CAUSE FIX. Ring-0 code (a syscall handler) that is about to
// memcpy() real data into a user-supplied destination buffer must NOT rely on
// the CPU to fault the destination in for it: a Ring-0 access ignores the U/S
// PTE bit entirely, so a page that is PRESENT but not yet USER (the common
// "inherited from a split kernel identity huge page" state a fresh heap-growth
// region starts in, see the case-2 comment in mm_fault() above) never faults
// at all when the KERNEL writes to it. The write silently "succeeds" against
// whatever physical frame happens to be there. Nothing is wrong yet from the
// syscall's point of view (it returns the correct byte count) - the bug only
// surfaces the FIRST time Ring 3 (the app) touches that same page: THAT access
// is gated on the missing USER bit, so it takes the real #PF -> mm_fault()
// path, which (correctly, for a plain never-touched page) hands back a FRESH,
// ZEROED, otherwise-identical page, discarding whatever the kernel had just
// written. Net effect: sys_read() reports the real length, but the app reads
// back zeros, exactly the #510 finding from Arena's de_dust2.bsp load and the
// #421 AssaultCube config/font.cfg finding (both plain fopen()+fread() into a
// freshly malloc'd libc stdio buffer).
//
// The fix is to make the KERNEL invoke the SAME fault resolver Ring 3 would
// have hit, PROACTIVELY, for every destination page, BEFORE doing the real
// write. That guarantees the destination is already backed by a genuinely
// fresh (or already-valid) page in exactly the state a natural Ring-3 first
// touch would have produced, so the kernel's subsequent write lands in the
// page the app will actually see - no separate "preserve the old frame"
// logic is needed (and, per the case-2 revert above, that approach is
// actively wrong for the common not-yet-written-by-anyone case).
//
// Best-effort by design: called from an unvalidated-argument syscall path
// (SYS_READ has no syscall_argtab entry) where the destination pointer has
// not been range/permission-checked yet either. This function only ever
// LOOSENS a fault that would otherwise have to be taken anyway (or is a
// harmless no-op if the range is already fully backed), so it cannot make an
// invalid destination valid: a genuinely bad pointer (unmapped entirely, or
// with no covering VMA) still fails exactly as before when the real memcpy
// touches it, moving the observable failure earlier and identically.
// #522 step 2 NOTE: this must NOT take the mm lock. It calls mm_fault() in a
// loop, and mm_fault() takes the lock itself; the mm lock is a plain spinlock,
// not recursive, so acquiring it here would self-deadlock on the first page.
void mm_prefault_range(struct process *p, uint64_t addr, uint64_t len, int for_write) {
    if (!p || p->cr3 == 0 || len == 0) return;

    uint64_t start = addr & ~(VMM_PAGE_SIZE_4K - 1);
    uint64_t end = (addr + len + VMM_PAGE_SIZE_4K - 1) & ~(VMM_PAGE_SIZE_4K - 1);
    // Guard against addr+len overflow wrapping end below start (a bad
    // syscall argument): bail out rather than loop on a bogus range. The
    // real memcpy will fault (or the caller's own bounds checks will catch
    // it) exactly as it would have without this prefault step.
    if (end <= start && len != 0) return;

    uint32_t want = for_write ? (PF_USER | PF_WRITE) : PF_USER;

    for (uint64_t page_addr = start; page_addr < end; page_addr += VMM_PAGE_SIZE_4K) {
        uint64_t eff = vmm_get_effective_flags_in(p->cr3, page_addr);
        if ((eff & VMM_FLAG_PRESENT) && (eff & VMM_FLAG_USER) &&
            (!for_write || (eff & VMM_FLAG_WRITABLE))) {
            continue;  // already exactly as backed as a real touch would leave it
        }
        // Ignore the result: this is best-effort. If mm_fault() cannot
        // resolve it (no covering VMA, genuinely invalid address), the
        // upcoming real memcpy fails/faults exactly as it would have without
        // this call - nothing is made WORSE by trying first.
        mm_fault(p, page_addr, want);
    }
}

// Duplicate a memory map's VMA list for fork (shallow; physical COW handled
// by the page-table clone). File-backed VMAs share the underlying handle.
mm_struct_t *mm_dup(mm_struct_t *src) {
    if (!src) return NULL;
    mm_struct_t *dst = mm_create();
    if (!dst) return NULL;

    dst->brk_start   = src->brk_start;
    dst->brk_current = src->brk_current;
    dst->stack_start = src->stack_start;
    dst->stack_end   = src->stack_end;
    // #636: the child inherits the PARENT'S cursor. Without this the child
    // restarted placement at the arena base with the parent's VMAs already
    // copied in over it. The gap scan copes, but only because it scans; before
    // there was a scan, the child's first mmap collided with an inherited VMA.
    dst->mmap_next   = src->mmap_next;

    // #522 step 2: snapshot the source list under the SOURCE mm's lock, so a
    // CLONE_VM sibling of the forking thread cannot split or free a node
    // mid-copy. This is the ONE documented site that allocates inside the lock:
    // fork must copy a CONSISTENT list, and counting first then allocating
    // outside would need a retry loop for a count that can change under us.
    // kmalloc() itself takes an irqsave spinlock (heap.c, #347) and never
    // blocks, so this can only lengthen the critical section, never deadlock.
    uint64_t sirq = mm_lock(src);
    for (vma_t *sv = src->vma_list; sv; sv = sv->next) {
        vma_t *dv = vma_create(sv->start, sv->end, sv->flags);
        if (!dv) { mm_unlock(src, sirq); mm_destroy(dst); return NULL; }
        dv->prot        = sv->prot;
        dv->file        = sv->file;
        dv->file_offset = sv->file_offset;
        dv->file_size   = sv->file_size;
        // dst is still private to this caller: _nolock, not a lock nobody can contend.
        if (vma_add_nolock(dst, dv) != 0) { kfree(dv); mm_unlock(src, sirq); mm_destroy(dst); return NULL; }
    }
    mm_unlock(src, sirq);
    return dst;
}
