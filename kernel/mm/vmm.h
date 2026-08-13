// vmm.h - Virtual Memory Manager (Paging)
#ifndef VMM_H
#define VMM_H

#include "../types.h"

// Page table entry flags
#define VMM_FLAG_PRESENT    (1ULL << 0)   // Page is present in memory
#define VMM_FLAG_WRITABLE   (1ULL << 1)   // Page is writable
#define VMM_FLAG_USER       (1ULL << 2)   // Page is accessible from user mode
#define VMM_FLAG_PWT        (1ULL << 3)   // Page-level write-through
#define VMM_FLAG_PCD        (1ULL << 4)   // Page-level cache disable
#define VMM_FLAG_ACCESSED   (1ULL << 5)   // Page has been accessed
#define VMM_FLAG_DIRTY      (1ULL << 6)   // Page has been written to
#define VMM_FLAG_HUGE       (1ULL << 7)   // Huge page (2MB/1GB)
#define VMM_FLAG_GLOBAL     (1ULL << 8)   // Global page (not flushed on CR3 switch)
#define VMM_FLAG_NX         (1ULL << 63)  // No-execute (NX bit)

// ===========================================================================
// #642: PAT (Page Attribute Table) memory types.
//
// WHAT WAS HERE: this header carried, TWICE, the line
//
//     #define VMM_WRITE_COMBINING (VMM_FLAG_PWT)
//
// PWT on its own selects PAT slot 1, and slot 1 holds WT (write-THROUGH) at
// power-on. The macro was named for a memory type its value did not produce. It
// also had ZERO callers anywhere in the kernel, and one of the two copies was
// dead on arrival: it shared a physical line with a `//` comment containing a
// literal backslash-n, so the preprocessor never saw the directive at all. On
// top of all that, NO PAT or MTRR programming existed anywhere in the tree, so
// there was no write-combining type to select even if something had tried.
// See blame.md.
//
// The macro's VALUE is now correct, and it is important to understand that this
// is not a vindication of the original. PWT means WC only because
// vmm_pat_init() rewrites slot 1 to hold the WC encoding. Without that MSR
// write the value still means write-through.
//
// WHY SLOT 1 (this is the same slot Linux uses for WC, and for the same
// reasons):
//   1. Slot 1 is selected by PWT=1, PCD=0, PAT=0, so no PAT bit is set at all.
//      The PAT bit lives at bit 7 in a 4KB PTE but bit 12 in a 2MB/1GB entry;
//      not needing it makes that whole class of bit-position bug impossible.
//   2. It sidesteps the errata Linux documents in which the high PAT bit is
//      ignored, which would silently redirect a slot-4..7 access to slot 0..3.
//   3. NOTHING in this kernel sets PWT (verified: the only PWT/PCD users are
//      ahci.c and e1000.c, and both set PCD alone = slot 2 = UC-), so rewriting
//      slot 1 changes the meaning of exactly zero existing mappings.
//   4. Best failure mode. PAT is PER-LOGICAL-CPU. An AP that never runs
//      vmm_pat_init() still sees slot 1 at its power-on default of WT, and
//      WT combined with the firmware's UC MTRR is UC: correct, merely slow. Had
//      WC gone in slot 4, that same AP would have seen slot 4's default of WB,
//      i.e. write-BACK caching of an MMIO framebuffer BAR, which is a coherency
//      hazard rather than a performance one.
//
// Slots 0, 2 and 3 keep their architectural power-on values (WB, UC-, UC), so
// this cannot alter the caching of any memory that was not explicitly re-typed.
//
// THE KEY ARCHITECTURAL FACT, and the reason no MTRR programming is needed:
// per the SDM effective-memory-type table (Vol 3A, "Effective Page-Level Memory
// Types", Table 12-7 in current editions) and AMD APM Vol 2 Table 7-9, an MTRR
// type of UC combined with a PAT type of WB yields UC, but an MTRR type of UC
// combined with a PAT type of WC yields WC. WC in the PAT is the one type never
// downgraded by any MTRR type. That asymmetry is why PAT exists, and is what
// Linux's ioremap_wc() relies on: arch_phys_wc_add() is documented as "a no-op
// on PAT enabled systems".
// ===========================================================================

// The PAT bit. Not needed to select the WC slot (see above), but defined
// correctly because vmm_set_memtype_range() must translate it when it splits a
// large page. Its POSITION DIFFERS BY PAGE LEVEL, and mixing the two up
// silently sets PS/HUGE or a reserved bit instead:
//   4KB PTE            -> bit 7  (bit 7 has no other meaning at the leaf level)
//   2MB PD / 1GB PDPT  -> bit 12 (bit 7 is PS/HUGE there)
#define VMM_FLAG_PAT        (1ULL << 7)    // 4KB PTE ONLY
#define VMM_FLAG_PAT_LARGE  (1ULL << 12)   // 2MB PD / 1GB PDPT entry ONLY

// Memory-type encodings as stored in an IA32_PAT slot byte.
// 0x02 and 0x03 are RESERVED; writing either causes a #GP on WRMSR, as does a
// nonzero value in any slot's upper 5 bits.
#define VMM_PAT_UC          0x00   // uncacheable (strong, not MTRR-overridable)
#define VMM_PAT_WC          0x01   // write-combining
#define VMM_PAT_WT          0x04   // write-through
#define VMM_PAT_WP          0x05   // write-protected
#define VMM_PAT_WB          0x06   // write-back
#define VMM_PAT_UC_MINUS    0x07   // UC-, overridable by a WC MTRR

// PAT slot indices. index = (PAT << 2) | (PCD << 1) | PWT.
// Slot 1 is the only one vmm_pat_init() rewrites.
#define VMM_PAT_IDX_WB       0
#define VMM_PAT_IDX_WC       1     // WT at power-on, WC after vmm_pat_init()
#define VMM_PAT_IDX_UC_MINUS 2
#define VMM_PAT_IDX_UC       3

// Leaf-PTE bits that select the write-combining slot.
// VALID ONLY AFTER vmm_pat_init() HAS RETURNED 1. Before that these bits still
// mean write-through, which is exactly why fb_init() sequences the PAT write
// BEFORE the framebuffer remap. Same bits at every page level, since no PAT bit
// is involved.
#define VMM_WRITE_COMBINING  VMM_FLAG_PWT

// #642: program IA32_PAT so slot 1 holds a genuine WC type.
// Checks CPUID.01H:EDX[16] first (WRMSR 0x277 on a CPU without PAT is a #GP),
// then performs the SDM cache-disable / WBINVD / TLB-flush sequence, then reads
// the MSR back and VERIFIES slot 1 before reporting success. Returns 1 if WC is
// genuinely available afterwards, 0 if not. Safe to call repeatedly; later calls
// are no-ops returning the same answer.
//
// PER-LOGICAL-CPU. An AP must call this itself before it can see WC on any page
// mapped with VMM_WRITE_COMBINING. Not doing so is safe (that AP sees WT, which
// against a UC MTRR is UC) but forfeits the speedup on that CPU. Wiring it into
// AP bring-up belongs in the SMP path, which this change does not touch.
int vmm_pat_init(void);

// 1 if vmm_pat_init() succeeded and VMM_WRITE_COMBINING really means WC.
int vmm_pat_available(void);

// #642: change the MEMORY TYPE of an ALREADY-MAPPED range in place, without
// changing which physical pages it maps and WITHOUT ALLOCATING ANYTHING.
// pat_index is one of the VMM_PAT_IDX_* values.
//
// Large pages are re-typed IN PLACE rather than split, which is possible only
// because WC lives in slot 1 and PWT is bit 3 at every page level. The trade is
// granularity: a 2MB or 1GB entry is re-typed only when the WHOLE granule falls
// inside the requested range, so a partially-covered granule at either end
// keeps its original type. 4KB leaves are always exact.
//
// RETURNS THE NUMBER OF BYTES ACTUALLY RE-TYPED, or -1 if the range is not
// fully mapped. Callers should report that figure rather than assume full
// coverage. See the long comment at the definition for why splitting was
// abandoned (it page-faulted on a UEFI read-only page handed out by the PMM).
int64_t vmm_set_memtype_range(uint64_t pml4_phys, uint64_t virt_addr,
                              uint64_t size, int pat_index);

// Page sizes
#define VMM_PAGE_SIZE_4K    4096ULL
#define VMM_PAGE_SIZE_2M    (2ULL * 1024 * 1024)
#define VMM_PAGE_SIZE_1G    (1ULL * 1024 * 1024 * 1024)

// Address masks
#define VMM_ADDR_MASK       0x000FFFFFFFFFF000ULL  // Physical address mask
#define VMM_PML4_INDEX(a)   (((a) >> 39) & 0x1FF)
#define VMM_PDPT_INDEX(a)   (((a) >> 30) & 0x1FF)
#define VMM_PD_INDEX(a)     (((a) >> 21) & 0x1FF)
#define VMM_PT_INDEX(a)     (((a) >> 12) & 0x1FF)
#define VMM_PAGE_OFFSET(a)  ((a) & 0xFFF)

// Page table structure (512 entries of 8 bytes each = 4KB)
typedef uint64_t page_table_t[512] __attribute__((aligned(4096)));

// Initialize virtual memory manager
void vmm_init(void);

// Map a virtual address to a physical address
int vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);

// Map multiple contiguous pages
int vmm_map_pages(uint64_t virt_addr, uint64_t phys_addr, uint64_t count, uint64_t flags);

// Unmap a virtual address
void vmm_unmap_page(uint64_t virt_addr);

// Get the physical address for a virtual address (returns 0 if not mapped)
uint64_t vmm_get_physical(uint64_t virt_addr);

// Check if a virtual address is mapped
int vmm_is_mapped(uint64_t virt_addr);

// Invalidate TLB entry for a virtual address
static inline void vmm_invlpg(uint64_t virt_addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

// Flush entire TLB (reload CR3)
void vmm_flush_tlb(void);

// Get current PML4 physical address
uint64_t vmm_get_pml4(void);

// Switch to a different address space (CR3)
void vmm_switch_pml4(uint64_t pml4_phys);

// ============================================
// User-space memory management
// ============================================

// User-space virtual address ranges
#define USER_SPACE_START    0x0000000000400000ULL  // Start at 4MB
#define USER_SPACE_END      0x00007FFFFFFFFFFFULL  // End of user space (lower half)
// ===========================================================================
// #522 stage 0: THE DEDICATED USERLAND WINDOW.
//
// Userland and device MMIO were put in the SAME 1GB of virtual address space
// (PDPT[2], 0x80000000-0xC0000000). That one decision produced three separate
// bills: #511 (the identity pre-fill under demand regions), the compositor
// present's forced CR3 switch (the physical framebuffer's address is also a
// live userland virtual address), and a 1GB budget that the app image, the
// libc heap, the mmap arena and the stack already over-subscribe.
//
// It is not fixable by carving PDPT[2] more carefully, because the framebuffer
// address is chosen by FIRMWARE, not by us. MEASURED on the user's iMac14,4:
// GOP base 0x90000000, 8MB - which is simultaneously PIE_USER_BASE (exec/elf.c)
// and libc's HEAP_START (userland/libc/stdlib.c). QEMU cannot reproduce this
// because its std-VGA lands at 0x80000000, exactly where the code assumed. No
// layout that assumes ANY framebuffer address or size is correct.
//
// So userland moves OUT, into its own PML4 entry the kernel identity map never
// touches. Precedent: sys_fb_map() has mapped the compositor's back buffer at
// 0x0000600000000000 for a long time, so user-visible memory at a high virtual
// address is proven in this kernel, not speculative.
//
// PML4 index 1 == virtual 512GB. Chosen because the kernel's own PML4[1] is
// ABSENT, which is exactly what makes the window unambiguously owned by each
// address space (see the ownership rule in vmm_destroy_user_space).
// ===========================================================================
#define USER_WIN_PML4_INDEX 1
#define USER_WIN_BASE       0x0000008000000000ULL   // 512 GB
#define USER_WIN_END        0x000000C000000000ULL   // 768 GB (256 GB of room)

// Arena bases within the window, declared up front but adopted ONE PER STAGE
// (see the #522 stage table in CHANGELOG.md).
#define USER_WIN_IMAGE_BASE (USER_WIN_BASE + 0x0000000000ULL)  // stage 5
#define USER_WIN_HEAP_BASE  (USER_WIN_BASE + 0x0040000000ULL)  // stage 3, +1GB
#define USER_WIN_MMAP_BASE  (USER_WIN_BASE + 0x0100000000ULL)  // stage 2, +4GB
#define USER_WIN_STACK_TOP  (USER_WIN_BASE + 0x0200000000ULL)  // stage 4, +8GB

// #522 stage 0 self-test: proves a page can be mapped, located AND (the part
// that actually mattered) FREED in the new window, before anything moves into
// it. Returns 1 on pass, 0 on fail.
int vmm_user_window_selftest(void);

#define USER_STACK_TOP      0x00000000BFFF0000ULL  // User stack at top of 2-3GB range
#define USER_STACK_SIZE     (2 * 1024 * 1024)       // 2MB default stack

// Common user-space flag combinations
#define VMM_USER_RO         (VMM_FLAG_PRESENT | VMM_FLAG_USER)
// #640 leg 4 step 2. This macro is used for the user STACK, the brk HEAP, shm
// and the mapped framebuffer, and until now it produced writable-AND-executable
// pages: the name said RW, the PTE said RWX, because nothing ever set NX. Those
// four regions are data. None of them has any reason to be executable, and a
// writable+executable stack is the single most useful thing an attacker can be
// handed. NX is safe to name here unconditionally because vmm_map_page_in()
// drops it when EFER.NXE is off.
//
// If a future JIT genuinely needs W and X in the same region (#526), it must
// ask for it explicitly at its own mapping site, so the exception is visible
// and greppable rather than being the default everyone inherits.
#define VMM_USER_RW         (VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE | VMM_FLAG_NX)
#define VMM_USER_RX         (VMM_FLAG_PRESENT | VMM_FLAG_USER)
// VMM_USER_RWX was DELETED at #640 leg 4 step 2. It had zero callers and
// its definition was character-for-character VMM_USER_RW, so it documented
// a distinction the kernel did not make. A caller that truly needs an
// executable writable mapping should spell the flags out in place.

// #642: the write-combining flags used to be defined a SECOND time here,
// identically wrong. See the PAT block near the top of this header.

// Create a new address space for a user process
// Returns physical address of new PML4, or 0 on failure
// The new address space includes kernel mappings in upper half
uint64_t vmm_create_user_space(void);

// Clone an address space (for fork)
// Creates copy-on-write mappings where possible
uint64_t vmm_clone_user_space(uint64_t src_pml4_phys);

// #429: real copy-on-write clone for fork. Instead of deep-copying every user
// page, the parent and child share the same physical pages marked read-only
// with the COW bit; the #PF handler copies a page on the first write from
// either side. The page-table STRUCTURE (PDPT/PD/PT pages) is still copied so
// the two address spaces are independent. Returns the child PML4, or 0.
uint64_t vmm_clone_user_space_cow(uint64_t src_pml4_phys);

// #429: return the raw leaf PTE (including flags such as PRESENT, WRITABLE, NX
// and the COW bit) mapping virt_addr in the given address space, or 0 if there
// is no present 4KB leaf entry. Used by the fault handler to distinguish a COW
// write from a genuine protection fault.
uint64_t vmm_get_pte_in(uint64_t pml4_phys, uint64_t virt_addr);

// #511: punch a demand (VMA-backed) range to NOT-PRESENT so a first access by
// EITHER ring faults into mm_fault(). Keeps device MMIO (no VMA) identity-mapped.
void vmm_punch_demand_range(uint64_t pml4_phys, uint64_t start, uint64_t len);

// Destroy a user address space and free all associated pages
void vmm_destroy_user_space(uint64_t pml4_phys);

// Map a page in a specific address space (not necessarily current)
int vmm_map_page_in(uint64_t pml4_phys, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);

// Unmap a page in a specific address space
void vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt_addr);

// Get physical address in a specific address space
uint64_t vmm_get_physical_in(uint64_t pml4_phys, uint64_t virt_addr);

// #500 / MAYTERA-SEC-2026-0016.
// Return the EFFECTIVE access rights of virt_addr in the address space rooted
// at pml4_phys, computed the way the hardware computes them: the U/S and R/W
// bits are ANDed across ALL four levels, and NX is ORed. Returns 0 if the page
// is not present at any level; otherwise VMM_FLAG_PRESENT plus the effective
// VMM_FLAG_USER / VMM_FLAG_WRITABLE / VMM_FLAG_NX bits.
//
// vmm_get_physical_in() is NOT a substitute: it reports only presence, so it
// says "mapped" for kernel memory that Ring 3 can never touch. MayteraOS
// identity-maps the kernel into the LOWER half (KERNEL_PHYS_BASE = 0x400000)
// and copies PML4[0] into every user address space, so a presence-only check
// cannot tell user memory from kernel memory. The U/S bit can; that is the
// only thing that can. Any user-pointer check MUST use this, not the other.
uint64_t vmm_get_effective_flags_in(uint64_t pml4_phys, uint64_t virt_addr);

// Allocate and map user pages at a virtual address
// Allocates physical memory and maps it with user-accessible flags
int vmm_alloc_user_pages(uint64_t pml4_phys, uint64_t virt_addr, uint64_t count, uint64_t flags);

// Free user pages and their physical memory
void vmm_free_user_pages(uint64_t pml4_phys, uint64_t virt_addr, uint64_t count);

// #640 leg 4 step 2: change the protection of an EXISTING user mapping without
// touching the physical frame behind it. This is the "protect" half of the only
// order that can work for W^X on a loaded image:
//
//     map writable  ->  copy contents  ->  apply relocations  ->  PROTECT
//
// The loader writes segment bytes through this mapping at CPL 0 with CR0.WP
// set, so the destination MUST be writable while it is being written (#633).
// Enforcing the ELF's own PF_W/PF_X before the copy is a kernel panic, not a
// hardening win; enforcing it after is W^X.
//
// The keep argument names the bits to KEEP; absent bits are CLEARED.
// VMM_FLAG_PRESENT and VMM_FLAG_USER are always kept. Pass VMM_FLAG_WRITABLE
// to leave the range writable, VMM_FLAG_NX to make it non-executable.
//
// Semantics deliberately shared with mprotect (mm/demand.c):
//   - a not-present or non-USER page is skipped (it is not ours to reprotect)
//   - PTE_COW_FLAG is preserved and, when set, WRITABLE is NOT granted, so a
//     fork-shared frame keeps taking the copy-on-write fault (#628)
//   - VMM_FLAG_NX is only ever set when EFER.NXE is on, because bit 63 is a
//     RESERVED bit without it and would raise a reserved-bit #PF (#429)
//
// Returns the number of pages whose PTE was rewritten. TLB shootdown is the
// caller's business. Both current callers are safe because a CR3 load (the
// loader's foreign-address-space switch, or context_start entering the new
// process) flushes every non-global entry.
uint64_t vmm_protect_user_range(uint64_t pml4_phys, uint64_t virt_addr,
                                uint64_t count, uint64_t keep);

// ---------------------------------------------------------------------------
// #647: vmm_init() ADOPTS the firmware's page tables and never switches away
// from them, but the bootloader types the memory those tables live in as
// MEMORY_TYPE_USABLE / MEMORY_TYPE_BOOTLOADER, which is exactly what pmm_init()
// FREES. CR3 therefore points into the PMM's free list. Page tables have no
// linker symbol, so pmm_init()'s hardcoded re-reservations cannot reach them;
// the enumeration has to start from the register.
//
// Called at the end of vmm_init(), BEFORE anything can allocate.
// ---------------------------------------------------------------------------
void vmm_reserve_boot_page_tables(void);

// Is this physical address one of the live boot page tables? (1/0)
int vmm_is_boot_page_table(uint64_t phys);
uint32_t vmm_boot_page_table_count(void);
const uint64_t *vmm_boot_page_table_list(void);

#ifdef PTWALK_SELFTEST
// #647 end-to-end self-test, DEBUG BUILDS ONLY (`make PTWALKTEST=1`).
void vmm_ptwalk_selftest(void);
#endif

#endif // VMM_H
