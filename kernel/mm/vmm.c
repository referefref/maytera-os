// vmm.c - Virtual Memory Manager implementation
// Implements 4-level paging for x86_64

#include "vmm.h"
#include "demand.h"   // PTE_COW_FLAG (#640: one COW rule, not two)
#include "pmm.h"
#include "../serial.h"
#include "../string.h"
#include "../gui/syslog.h"
#include "../fs/bootlog.h"   // #522 stage 0: selftest result to the durable log

// Kernel PML4 table (page map level 4)
static page_table_t kernel_pml4 __attribute__((aligned(4096)));

// Pointer to current PML4 (physical address)
static uint64_t current_pml4_phys = 0;

// Kernel CR3 for syscall handler (accessible from assembly)
uint64_t g_kernel_cr3 = 0;

// #429: COW-aware free of a user leaf page (mm/demand.c). A COW-shared page is
// only returned to the PMM when its last owner drops it; otherwise it is freed
// immediately. Routing user-page frees through this stops a fork-shared page
// from being freed out from under a sibling.
extern void vmm_free_user_page_cow(uint64_t phys_addr);

// Helper: Get or create a page table entry
static uint64_t* vmm_get_or_create_entry(uint64_t *table, int index, uint64_t flags) {
    if (!(table[index] & VMM_FLAG_PRESENT)) {
        // Allocate a new page table
        uint64_t new_table = pmm_alloc_page();
        if (new_table == 0) {
            kprintf("[VMM] ERROR: Failed to allocate page table! (pmm_alloc returned 0)\n");
            return NULL;
        }

        // Clear the new table
        memset((void*)new_table, 0, 4096);

        // Set the entry with default flags (present, writable)
        table[index] = new_table | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | (flags & VMM_FLAG_USER);
    }

    // Return pointer to the next level table
    return (uint64_t*)(table[index] & VMM_ADDR_MASK);
}

// Initialize the virtual memory manager
void vmm_init(void) {
    kprintf("[VMM] Initializing virtual memory manager...\n");

    // Get current CR3 (UEFI's page tables)
    uint64_t uefi_cr3 = read_cr3();
    kprintf("[VMM] Current CR3: 0x%lx\n", uefi_cr3);

    // For now, we'll keep using UEFI's page tables which already have
    // identity mapping set up. This is the safest approach.
    //
    // In a more complete implementation, we would:
    // 1. Copy UEFI's page tables to our own
    // 2. Modify them as needed (higher-half kernel, etc.)
    // 3. Switch to our own page tables
    //
    // For the current stage, UEFI's identity mapping is sufficient
    // and allows us to access all physical memory directly.

    current_pml4_phys = uefi_cr3;
    g_kernel_cr3 = uefi_cr3;  // Store for syscall CR3 switching

    // Clear our kernel PML4 for future use
    memset(kernel_pml4, 0, sizeof(kernel_pml4));

    kprintf("[VMM] Using UEFI page tables (identity mapped)\n");

    // #647: we just adopted memory the PMM was told is free. Take it back
    // BEFORE anything can allocate. This must be the last thing vmm_init()
    // does and the first allocation-relevant thing after pmm_init().
    vmm_reserve_boot_page_tables();

    kprintf("[VMM] Virtual memory manager initialized\n");
}

// ===========================================================================
// #647: the adopted firmware page tables were in the PMM's free list.
//
// THE BUG, end to end:
//   bootloader.c convert_memory_type() maps EfiConventionalMemory +
//   EfiBootServicesCode + EfiBootServicesData -> MEMORY_TYPE_USABLE, and
//   EfiLoaderCode + EfiLoaderData -> MEMORY_TYPE_BOOTLOADER. pmm_init() frees
//   exactly those two classes. Both halves are correct on their own: after
//   ExitBootServices those classes ARE reclaimable. They are not reclaimable
//   HERE, because vmm_init() above deliberately keeps running on the firmware's
//   page tables forever instead of building its own. CR3 therefore points into
//   memory the allocator believes it owns.
//
//   pmm_init() re-reserves only objects with a linker symbol (kernel text,
//   __data_start..__kernel_end, the bitmap) plus the hardcoded 1-4MB window. A
//   page table has no symbol. It is reachable ONLY through CR3. No amount of
//   care in pmm_init() could have caught this; the enumeration has to start
//   from the register.
//
//   Measured consequence: pmm_alloc_page() returned 0x7bc05000, which a later
//   fault identified as the live PD mapping the framebuffer. It faulted LOUDLY
//   only because the firmware write-protects its own paging structures and
//   CR0.WP was set. Where the entry is writable the same event is SILENT
//   corruption of a live translation.
//
//   Why it looks intermittent rather than constant: pmm_alloc_page() is a
//   first-fit linear scan from memory_start with no cursor, so for a FIXED
//   memory map and a FIXED allocation sequence it is perfectly deterministic
//   (hence 0x7bc05000 reproducing byte for byte). What varies is the sequence
//   (device probe order, xHCI enumeration timing, DHCP, device count) and the
//   map itself (QEMU vs the iMac vs firmware revision).
//
// THE FIX: walk the hierarchy from CR3 and hand every table page to the PMM.
// Surgical, and it keeps the rest of BootServices memory reclaimable.
//
// WHY NOT "just stop freeing EfiBootServicesData": that type does not exist in
// the kernel. The bootloader collapses three raw UEFI types into
// MEMORY_TYPE_USABLE and carries the raw value nowhere (memory_map_entry_t has
// base/length/type/attributes and `attributes` holds the EFI_MEMORY_* attribute
// bits, not the type). The coarsest thing the kernel CAN refuse to free is all
// of MEMORY_TYPE_USABLE, which is all of conventional RAM. So the surgical walk
// is not merely preferred here, it is the only fix expressible inside the
// kernel. Making the raw type available would mean changing BOOTX64.EFI, whose
// source is not even in the source-of-truth repo.
// ===========================================================================

// Rust (rustkern/ptwalk.rs): the hierarchy walk and the membership test are
// pure pointer-chasing logic over identity-mapped memory, so they are Rust per
// the Rust-first policy. The C below keeps only what must be C: the `mov %cr3`
// read (types.h read_cr3()), the PMM bitmap calls, and kprintf reporting.
extern uint32_t ptwalk_collect_rs(uint64_t cr3, uint64_t *out, uint32_t max,
                                  uint64_t phys_limit, uint32_t *overflow,
                                  uint32_t *skipped);
extern int ptwalk_contains_rs(const uint64_t *list, uint32_t n, uint64_t phys);

// A 4-level hierarchy identity-mapping a few GB with 2MB pages needs on the
// order of 1 PML4 + a handful of PDPTs + a few dozen PDs; 4KB-granular regions
// add PTs. 8192 slots (64KB of .bss) is ~an order of magnitude of headroom, and
// an overflow is DETECTED and reported rather than silently truncating.
#define VMM_PT_MAX 8192
static uint64_t g_boot_pt[VMM_PT_MAX];
static uint32_t g_boot_pt_n = 0;
static uint32_t g_boot_pt_overflow = 0;

// Exposed so the fault handler / any auditor can ask "was that a page table?".
int vmm_is_boot_page_table(uint64_t phys) {
    if (g_boot_pt_n == 0) return 0;
    return ptwalk_contains_rs(g_boot_pt, g_boot_pt_n, phys);
}
uint32_t vmm_boot_page_table_count(void) { return g_boot_pt_n; }
const uint64_t *vmm_boot_page_table_list(void) { return g_boot_pt; }

void vmm_reserve_boot_page_tables(void) {
    uint64_t cr3 = read_cr3();
    uint64_t limit = pmm_phys_limit();
    uint32_t skipped = 0;

    g_boot_pt_overflow = 0;
    g_boot_pt_n = ptwalk_collect_rs(cr3, g_boot_pt, VMM_PT_MAX, limit,
                                    &g_boot_pt_overflow, &skipped);

    kprintf("[PT647] CR3=0x%lx phys_limit=0x%lx: %u table pages, overflow=%u skipped=%u\n",
            cr3, limit, g_boot_pt_n, g_boot_pt_overflow, skipped);

    if (g_boot_pt_overflow) {
        // Do NOT continue quietly: a truncated list means unreserved live
        // tables, i.e. the bug is still armed for the pages we never saw.
        kprintf("[PT647] ERROR: table list OVERFLOWED at %u entries. "
                "Some live page tables remain in the free list. Raise VMM_PT_MAX.\n",
                VMM_PT_MAX);
    }

    // ---- STEP 1 MEASUREMENT: what TYPE is the memory the tables live in? ----
    // Tally by the memory type the bootloader assigned, and separately by
    // whether the page was actually FREE (= the allocator would have handed the
    // live table out). Those are different questions: a table above the 2GB
    // identity cap is typed USABLE but was already re-marked used, so it is
    // exposed in principle and safe in practice on this machine.
    uint64_t by_type[16];
    uint64_t free_by_type[16];
    for (int i = 0; i < 16; i++) { by_type[i] = 0; free_by_type[i] = 0; }

    uint32_t n_free = 0, n_used = 0, n_oor = 0, n_reserved = 0;
    uint64_t lowest_free = 0, highest_free = 0;
    (void)n_oor; (void)n_reserved; (void)n_free;  // one arm or the other uses each

    for (uint32_t i = 0; i < g_boot_pt_n; i++) {
        uint64_t pa = g_boot_pt[i];
        uint32_t idx = 0; uint64_t base = 0, len = 0;
        uint32_t type = pmm_mmap_type_of(pa, &idx, &base, &len);
        if (type > 15) type = 0;
        by_type[type]++;

        int was_free = pmm_page_is_free(pa);
        if (was_free) {
            free_by_type[type]++;
            if (lowest_free == 0) lowest_free = pa;
            highest_free = pa;
            // The per-page lines ARE the evidence, but there are ~74 of them and
            // a shipped kernel should not spend 74 serial lines on a fact it can
            // state in one. Full list in the diagnostic build, first few plus a
            // count otherwise.
#ifdef PTWALK_SELFTEST
            kprintf("[PT647] EXPOSED 0x%lx was FREE  mmap[%u] 0x%lx+0x%lx type=%u %s\n",
                    pa, idx, base, len, type, pmm_mmap_type_name(type));
#else
            if (n_free < 4) {
                kprintf("[PT647] EXPOSED 0x%lx was FREE  mmap[%u] 0x%lx+0x%lx type=%u %s\n",
                        pa, idx, base, len, type, pmm_mmap_type_name(type));
            }
#endif
        }

        if (was_free) n_free++;

#ifndef PTWALK_NO_RESERVE
        int r = pmm_reserve_page(pa);
        if (r == 1) n_reserved++;
        else if (r == 0) n_used++;
        else n_oor++;
#else
        // #647 NEGATIVE CONTROL (`make PTWALKNORESERVE=1`): identical build,
        // identical instrumentation, reservation REMOVED. This is the bug, kept
        // buildable, so the self-test below can be watched to FAIL. A self-test
        // that has only ever been seen to pass proves nothing.
        if (!was_free) n_used++;
#endif
    }

    for (int t = 0; t < 16; t++) {
        if (by_type[t]) {
            kprintf("[PT647]   type %d = %s : %lu table pages, %lu of them in the FREE list\n",
                    t, pmm_mmap_type_name(t), by_type[t], free_by_type[t]);
        }
    }

#ifndef PTWALK_NO_RESERVE
    kprintf("[PT647] reserved %u newly (were FREE and would have been handed out), "
            "%u already used, %u outside allocator range\n",
            n_reserved, n_used, n_oor);
#else
    kprintf("[PT647] NEGATIVE CONTROL: reservation DISABLED. %u table pages left FREE.\n",
            n_free);
#endif
    if (lowest_free) {
        kprintf("[PT647] exposed range 0x%lx .. 0x%lx\n", lowest_free, highest_free);
    }

    // INVARIANT (the direct form of the fix's claim): after this function, no
    // live table page is allocatable. Checked here, always, in every build.
    uint32_t still_free = 0;
    for (uint32_t i = 0; i < g_boot_pt_n; i++) {
        if (pmm_page_is_free(g_boot_pt[i])) still_free++;
    }
    kprintf("[PT647] INVARIANT: %u of %u live table pages still allocatable -> %s\n",
            still_free, g_boot_pt_n, still_free == 0 ? "PASS" : "FAIL");

#ifdef PTWALK_SELFTEST
    // The full boot memory map. Diagnostic builds only: it is ~110 lines, and
    // the reason to read it is descriptor ADJACENCY (see pmm_dump_mmap()), which
    // is an investigation question, not a per-boot one.
    pmm_dump_mmap();

#endif

#ifdef PTWALK_SELFTEST
    vmm_ptwalk_selftest();
#endif
}

#ifdef PTWALK_SELFTEST
// ---------------------------------------------------------------------------
// #647 END-TO-END self-test. DEBUG BUILDS ONLY (`make PTWALKTEST=1`).
// Never set for a golden.
//
// Claim under test: "no page the allocator hands out is a live page table".
// Method: drain the allocator to exhaustion and check every page it yields.
// The bitmap is snapshotted first and restored after, and the drain only
// compares addresses, never writes to the pages.
//
// Pair with `make PTWALKTEST=1 PTWALKNORESERVE=1`, which is the same build with
// the reservation removed: that arm MUST report hits. If both arms pass, the
// test is decorative and proves nothing.
// ---------------------------------------------------------------------------
struct ptw_test { uint64_t hits; uint64_t first_hit; uint64_t first_ordinal; };

static void ptw_drain_cb(uint64_t phys, uint64_t ordinal, void *ctx) {
    struct ptw_test *t = (struct ptw_test *)ctx;
    if (ptwalk_contains_rs(g_boot_pt, g_boot_pt_n, phys)) {
        if (t->hits == 0) { t->first_hit = phys; t->first_ordinal = ordinal; }
        t->hits++;
    }
}

// Part A comparator state. The drain callback signature carries a void* ctx, so
// this could be a struct; it is a small static because the callback also runs
// for every page past the compared prefix and must stay branch-cheap.
#define PTW_CMP_N 4096
static uint64_t ptw_real_seq[PTW_CMP_N];
static uint32_t ptw_real_n = 0;
static uint32_t ptw_cmp_k = 0;
static uint32_t ptw_cmp_mismatch = 0;

static void ptw_cmp_cb(uint64_t phys, uint64_t ordinal, void *ctx) {
    (void)ctx;
    if (ordinal < ptw_real_n) {
        if (ptw_real_seq[ordinal] != phys) ptw_cmp_mismatch++;
        ptw_cmp_k++;
    }
}

void vmm_ptwalk_selftest(void) {
    if (g_boot_pt_n == 0) {
        kprintf("[PT647-TEST] no table pages collected; test cannot run\n");
        return;
    }

    // ---- Part A: cross-check that the fast drain really IS the allocator ----
    // pmm_selftest_drain() claims to reproduce pmm_alloc_page()'s selection rule
    // ("the lowest-index free page") in a single pass, which is what makes the
    // full drain in Part B affordable (the real allocator restarts its scan from
    // memory_start every call, so a literal 500k-page drain is O(n^2)). Prove
    // that claim on a sample instead of asserting it: take PTW_CMP_N pages from
    // the REAL pmm_alloc_page(), restore, then compare the fast drain's prefix
    // element by element. If these ever disagree, Part B's result means nothing.
    ptw_real_n = 0;
    pmm_selftest_snapshot();
    for (uint32_t i = 0; i < PTW_CMP_N; i++) {
        uint64_t p = pmm_alloc_page();
        if (!p) break;
        ptw_real_seq[ptw_real_n++] = p;
    }
    pmm_selftest_restore();

    ptw_cmp_k = 0;
    ptw_cmp_mismatch = 0;
    pmm_selftest_snapshot();
    pmm_selftest_drain(ptw_cmp_cb, 0);
    pmm_selftest_restore();
    kprintf("[PT647-TEST] fast-drain vs real pmm_alloc_page(): compared %u pages, "
            "%u mismatches -> %s\n",
            ptw_cmp_k, ptw_cmp_mismatch,
            ptw_cmp_mismatch == 0 ? "EQUIVALENT"
                                  : "NOT EQUIVALENT (Part B result is untrustworthy)");

    // ---- Part B: the real thing. Drain everything, check every page. ----
    struct ptw_test t = {0, 0, 0};
    pmm_selftest_snapshot();
    uint64_t drained = pmm_selftest_drain(ptw_drain_cb, &t);
    pmm_selftest_restore();

    kprintf("[PT647-TEST] drained %lu pages (%lu MB); %lu of them were LIVE page tables\n",
            drained, (drained * 4096) / (1024 * 1024), t.hits);
    if (t.hits) {
        kprintf("[PT647-TEST] first live table handed out at allocation #%lu: 0x%lx\n",
                t.first_ordinal, t.first_hit);
    }
    kprintf("[PT647-TEST] RESULT: %s\n", t.hits == 0 ? "PASS (no live page table is allocatable)"
                                                     : "FAIL (allocator hands out live page tables)");

    // Arm the IN-SERVICE detector (mm/pmm.c). From here on, every real
    // allocation is checked against the live table set and the allocator's
    // high-water mark is reported as it climbs. The boot drain answers "what
    // WOULD happen at exhaustion"; this answers "what DOES happen under the
    // real workload", which is the question #606 and #483 actually pose.
    extern int g_pmm_live_detect;
    g_pmm_live_detect = 1;
    kprintf("[PT647-TEST] in-service live-page-table detector ARMED\n");
}
#endif // PTWALK_SELFTEST

// Map a virtual address to a physical address with 4KB pages
int vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    // Align addresses to page boundary
    virt_addr &= ~0xFFFULL;
    phys_addr &= ~0xFFFULL;

    // Get PML4 entry
    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    uint64_t *pdpt = vmm_get_or_create_entry(kernel_pml4, pml4_idx, flags);
    if (!pdpt) return -1;

    // Get PDPT entry
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    uint64_t *pd = vmm_get_or_create_entry(pdpt, pdpt_idx, flags);
    if (!pd) return -1;

    // Get PD entry
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);

    // Check if this is a huge page
    if (pd[pd_idx] & VMM_FLAG_HUGE) {
        // Cannot map a 4KB page over a 2MB huge page without splitting
        kprintf("[VMM] WARNING: Cannot map 4KB page over 2MB huge page at 0x%lx\n", virt_addr);
        return -1;
    }

    uint64_t *pt = vmm_get_or_create_entry(pd, pd_idx, flags);
    if (!pt) return -1;

    // Set PT entry
    uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
    pt[pt_idx] = phys_addr | flags | VMM_FLAG_PRESENT;

    // Invalidate TLB for this address
    vmm_invlpg(virt_addr);

    return 0;
}

// Map multiple contiguous pages
int vmm_map_pages(uint64_t virt_addr, uint64_t phys_addr, uint64_t count, uint64_t flags) {
    for (uint64_t i = 0; i < count; i++) {
        if (vmm_map_page(virt_addr + i * VMM_PAGE_SIZE_4K,
                         phys_addr + i * VMM_PAGE_SIZE_4K, flags) != 0) {
            return -1;
        }
    }
    return 0;
}

// Unmap a virtual address
void vmm_unmap_page(uint64_t virt_addr) {
    virt_addr &= ~0xFFFULL;

    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    if (!(kernel_pml4[pml4_idx] & VMM_FLAG_PRESENT)) return;

    uint64_t *pdpt = (uint64_t*)(kernel_pml4[pml4_idx] & VMM_ADDR_MASK);
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) return;

    uint64_t *pd = (uint64_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) return;

    // Check if huge page
    if (pd[pd_idx] & VMM_FLAG_HUGE) {
        // Clear huge page entry
        pd[pd_idx] = 0;
    } else {
        uint64_t *pt = (uint64_t*)(pd[pd_idx] & VMM_ADDR_MASK);
        uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
        pt[pt_idx] = 0;
    }

    vmm_invlpg(virt_addr);
}

// Get physical address for a virtual address
uint64_t vmm_get_physical(uint64_t virt_addr) {
    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    if (!(kernel_pml4[pml4_idx] & VMM_FLAG_PRESENT)) return 0;

    uint64_t *pdpt = (uint64_t*)(kernel_pml4[pml4_idx] & VMM_ADDR_MASK);
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) return 0;

    // Check for 1GB huge page
    if (pdpt[pdpt_idx] & VMM_FLAG_HUGE) {
        return (pdpt[pdpt_idx] & VMM_ADDR_MASK) | (virt_addr & 0x3FFFFFFF);
    }

    uint64_t *pd = (uint64_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) return 0;

    // Check for 2MB huge page
    if (pd[pd_idx] & VMM_FLAG_HUGE) {
        return (pd[pd_idx] & VMM_ADDR_MASK) | (virt_addr & 0x1FFFFF);
    }

    uint64_t *pt = (uint64_t*)(pd[pd_idx] & VMM_ADDR_MASK);
    uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
    if (!(pt[pt_idx] & VMM_FLAG_PRESENT)) return 0;

    return (pt[pt_idx] & VMM_ADDR_MASK) | VMM_PAGE_OFFSET(virt_addr);
}

// Check if a virtual address is mapped
int vmm_is_mapped(uint64_t virt_addr) {
    return vmm_get_physical(virt_addr) != 0;
}

// Flush entire TLB
void vmm_flush_tlb(void) {
    write_cr3(read_cr3());
}

// Get current PML4 physical address
uint64_t vmm_get_pml4(void) {
    return current_pml4_phys;
}

// Switch to a different address space
void vmm_switch_pml4(uint64_t pml4_phys) {
    current_pml4_phys = pml4_phys;
    write_cr3(pml4_phys);
}

// ============================================
// User-space memory management
// ============================================

// Helper: Get or create a page table entry in a specific address space
static uint64_t* vmm_get_or_create_entry_in(uint64_t *table, int index, uint64_t flags) {
    if (!(table[index] & VMM_FLAG_PRESENT)) {
        // Allocate a new page table
        uint64_t new_table = pmm_alloc_page();
        if (new_table == 0) {
            kprintf("[VMM] ERROR: Failed to allocate page table! (pmm_alloc returned 0)\n");
            return NULL;
        }

        // Clear the new table
        memset((void*)new_table, 0, 4096);

        // Set the entry with default flags
        table[index] = new_table | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | (flags & VMM_FLAG_USER);
    }

    // Return pointer to the next level table
    return (uint64_t*)(table[index] & VMM_ADDR_MASK);
}

// Create a new address space for a user process
// #522 stage 0: prove the new user window works BEFORE anything moves into it.
//
// Deliberately checks the FREE path, not just the map path. Mapping a page at a
// fresh PML4 slot was always going to work (vmm_get_or_create_entry_in builds
// the hierarchy on demand and propagates the USER bit); what was broken, and
// what a map-only test would have happily reported as a pass, is that
// vmm_destroy_user_space did not recognise the slot as owned and leaked every
// page under it. So the assertion that carries the weight is the PMM free-page
// count returning to its baseline after the address space is destroyed.
int vmm_user_window_selftest(void) {
    uint64_t free_before = pmm_get_free_pages();

    uint64_t sp = vmm_create_user_space();
    if (!sp) { kprintf("[UWIN] SELFTEST FAIL: no address space\n"); return 0; }

    // Map a handful of pages spread across the window's arenas.
    const uint64_t vas[4] = {
        USER_WIN_BASE, USER_WIN_HEAP_BASE, USER_WIN_MMAP_BASE, USER_WIN_STACK_TOP - 0x1000ULL
    };
    int ok = 1;
    uint64_t phys[4] = {0,0,0,0};
    for (int i = 0; i < 4; i++) {
        phys[i] = pmm_alloc_page();
        if (!phys[i]) { ok = 0; break; }
        if (vmm_map_page_in(sp, vas[i], phys[i], VMM_USER_RW) != 0) { ok = 0; break; }
    }

    // Every mapping must be present, USER and writable, at the right frame.
    if (ok) {
        for (int i = 0; i < 4; i++) {
            uint64_t eff = vmm_get_effective_flags_in(sp, vas[i]);
            uint64_t got = vmm_get_physical_in(sp, vas[i]);
            if (!(eff & VMM_FLAG_PRESENT) || !(eff & VMM_FLAG_USER) ||
                !(eff & VMM_FLAG_WRITABLE) || got != phys[i]) {
                kprintf("[UWIN] SELFTEST FAIL: va=0x%lx eff=0x%lx phys=0x%lx want 0x%lx\n",
                        vas[i], eff, got, phys[i]);
                ok = 0;
            }
        }
    }

    // A user VA in the window must NOT be reachable in the KERNEL address
    // space: that separation is the entire point of moving out of PDPT[2].
    if (ok && current_pml4_phys) {
        if (vmm_get_physical_in(current_pml4_phys, USER_WIN_BASE) != 0) {
            kprintf("[UWIN] SELFTEST FAIL: window is mapped in the kernel space too\n");
            ok = 0;
        }
    }

    vmm_destroy_user_space(sp);

    uint64_t free_after = pmm_get_free_pages();
    int64_t leaked = (int64_t)free_before - (int64_t)free_after;
    if (leaked != 0) {
        kprintf("[UWIN] SELFTEST FAIL: LEAKED %ld page(s) (before=%lu after=%lu)\n",
                (long)leaked, free_before, free_after);
        ok = 0;
    }

    kprintf("[UWIN] window 0x%lx-0x%lx pml4[%d] map/flags/isolation/free: %s "
            "(pages before=%lu after=%lu)\n",
            USER_WIN_BASE, USER_WIN_END, USER_WIN_PML4_INDEX,
            ok ? "PASS" : "***FAIL***", free_before, free_after);
    bootlog_write("[UWIN] selftest %s leaked=%ld", ok ? "PASS" : "FAIL", (long)leaked);
    return ok;
}

uint64_t vmm_create_user_space(void) {
    // #522 stage 0: run the window self-test once, on the first user address
    // space ever created. The guard is set BEFORE the call because the test
    // itself calls back into this function.
    static int selftest_done = 0;
    if (!selftest_done) {
        selftest_done = 1;
        vmm_user_window_selftest();
    }

    // Allocate PML4
    uint64_t pml4_phys = pmm_alloc_page();
    if (pml4_phys == 0) {
        kprintf("[VMM] Failed to allocate PML4 for user space\n");
        return 0;
    }

    uint64_t *pml4 = (uint64_t*)pml4_phys;
    memset(pml4, 0, 4096);

    // Copy ONLY kernel mappings from current address space (upper half)
    // PML4 entries 256-511 are for kernel space (0xFFFF800000000000+)
    // DO NOT copy lower half - it contains 2MB huge pages that conflict with user 4KB pages
    uint64_t *current_pml4 = (uint64_t*)current_pml4_phys;

    // Only copy kernel entries (256-511), leave user space (0-255) empty
    // User pages will be mapped with 4KB granularity
    for (int i = 256; i < 512; i++) {
        pml4[i] = current_pml4[i];
    }
    
    // CRITICAL: Deep copy PML4[0] with new PDPT to avoid huge page conflicts
    // The kernel's PML4[0] contains a PDPT that has 2MB huge pages at 2GB range
    // We need to map 4KB user pages at 2GB, which conflicts with huge pages
    // Solution: Create a NEW PDPT, copy all entries EXCEPT entry 2 (2GB-3GB range)
    if (current_pml4[0] != 0) {
        uint64_t new_pdpt_phys = pmm_alloc_page();
        if (new_pdpt_phys == 0) {
            kprintf("[VMM] Failed to allocate new PDPT\n");
            pmm_free_page(pml4_phys);
            return 0;
        }
        
        uint64_t *new_pdpt = (uint64_t*)new_pdpt_phys;
        memset(new_pdpt, 0, 4096);
        
        uint64_t *kernel_pdpt = (uint64_t*)(current_pml4[0] & 0xFFFFFFFFFFFFF000ULL);
        
        // Copy PDPT entries EXCEPT entries 0-2
        // Keep entries 0-1 for kernel stack access during context switch
        // Clear only entry 2 (2-3GB) for user code/data
        // Entries 3+ are framebuffer/MMIO (keep from kernel)
        // We clear these to use fresh page tables (allocated from identity-mapped memory)
        // Entries 3+ point to framebuffer/MMIO which we can keep
        for (int i = 0; i < 512; i++) {
            if (i == 2) {
                // PDPT[2] covers the 2-3GB virtual/physical range.
                // Always create a fresh PD with 2MB huge pages for the identity
                // mapping. This avoids inheriting kernel PT pointers that may
                // reference physical pages above the PMM 2GB limit (which are
                // not writable via the kernel identity mapping).
                // When elf_load_user maps 4KB user pages here, vmm_map_page_in
                // will break individual 2MB entries into fresh PT pages.
                uint64_t new_pd_phys = pmm_alloc_page();
                if (new_pd_phys) {
                    uint64_t *new_pd = (uint64_t*)new_pd_phys;
                    for (int j = 0; j < 512; j++) {
                        new_pd[j] = (0x80000000ULL + (uint64_t)j * 0x200000ULL)
                            | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_HUGE;
                    }
                    new_pdpt[2] = new_pd_phys | VMM_FLAG_PRESENT
                        | VMM_FLAG_WRITABLE | VMM_FLAG_USER;
                } else {
                    new_pdpt[2] = 0;
                }
            } else {
                new_pdpt[i] = kernel_pdpt[i];
            }
        }
        
        // CRITICAL: Set PML4[0] with USER flag so user-mode can access
        pml4[0] = new_pdpt_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;
        kprintf("[VMM] Deep copied PML4[0], cleared PDPT[2] for user space at 2GB\n");
    }

    kprintf("[VMM] Created user address space: PML4=0x%lx\n", pml4_phys);
    return pml4_phys;
}

// Clone an address space (for fork)
uint64_t vmm_clone_user_space(uint64_t src_pml4_phys) {
    if (src_pml4_phys == 0) {
        return vmm_create_user_space();
    }

    // Allocate new PML4
    uint64_t dst_pml4_phys = pmm_alloc_page();
    if (dst_pml4_phys == 0) {
        return 0;
    }

    uint64_t *src_pml4 = (uint64_t*)src_pml4_phys;
    uint64_t *dst_pml4 = (uint64_t*)dst_pml4_phys;

    // CRITICAL: Zero the freshly allocated PML4. pmm_alloc_page() returns
    // pages with arbitrary stale contents (often the contents of a recently
    // freed page). The loops below only WRITE entries where src has a PRESENT
    // mapping, so any stale PRESENT-looking garbage in entries 0-255 (user
    // half) would survive into the child's PML4. vmm_destroy_user_space then
    // walks those entries, dereferences the stale non-canonical addresses, and
    // #GPs. Zero up front so only entries we explicitly copy are live.
    memset(dst_pml4, 0, 4096);

    // For now, do a simple deep copy of all user-space pages
    // TODO: Implement copy-on-write for efficiency
    for (int i = 0; i < 256; i++) {  // Only user space (lower half)
        if (src_pml4[i] & VMM_FLAG_PRESENT) {
            // Allocate new PDPT
            uint64_t *src_pdpt = (uint64_t*)(src_pml4[i] & VMM_ADDR_MASK);
            uint64_t new_pdpt_phys = pmm_alloc_page();
            if (!new_pdpt_phys) goto fail;

            uint64_t *dst_pdpt = (uint64_t*)new_pdpt_phys;
            memset(dst_pdpt, 0, 4096);

            for (int j = 0; j < 512; j++) {
                if (src_pdpt[j] & VMM_FLAG_PRESENT) {
                    if (src_pdpt[j] & VMM_FLAG_HUGE) {
                        // 1GB page - copy reference (kernel pages)
                        dst_pdpt[j] = src_pdpt[j];
                    } else {
                        // Need to copy PD
                        uint64_t *src_pd = (uint64_t*)(src_pdpt[j] & VMM_ADDR_MASK);
                        uint64_t new_pd_phys = pmm_alloc_page();
                        if (!new_pd_phys) goto fail;

                        uint64_t *dst_pd = (uint64_t*)new_pd_phys;
                        memset(dst_pd, 0, 4096);

                        for (int k = 0; k < 512; k++) {
                            if (src_pd[k] & VMM_FLAG_PRESENT) {
                                if (src_pd[k] & VMM_FLAG_HUGE) {
                                    // 2MB page - for user pages, allocate new
                                    if (src_pd[k] & VMM_FLAG_USER) {
                                        uint64_t new_page = pmm_alloc_pages(512);  // 2MB
                                        if (!new_page) goto fail;
                                        memcpy((void*)new_page,
                                               (void*)(src_pd[k] & VMM_ADDR_MASK),
                                               VMM_PAGE_SIZE_2M);
                                        dst_pd[k] = new_page | (src_pd[k] & 0xFFF);
                                    } else {
                                        dst_pd[k] = src_pd[k];  // Kernel page
                                    }
                                } else {
                                    // Need to copy PT
                                    uint64_t *src_pt = (uint64_t*)(src_pd[k] & VMM_ADDR_MASK);
                                    uint64_t new_pt_phys = pmm_alloc_page();
                                    if (!new_pt_phys) goto fail;

                                    uint64_t *dst_pt = (uint64_t*)new_pt_phys;
                                    memset(dst_pt, 0, 4096);

                                    for (int l = 0; l < 512; l++) {
                                        if (src_pt[l] & VMM_FLAG_PRESENT) {
                                            if (src_pt[l] & VMM_FLAG_USER) {
                                                // User page - allocate new
                                                uint64_t new_page = pmm_alloc_page();
                                                if (!new_page) goto fail;
                                                memcpy((void*)new_page,
                                                       (void*)(src_pt[l] & VMM_ADDR_MASK),
                                                       VMM_PAGE_SIZE_4K);
                                                dst_pt[l] = new_page | (src_pt[l] & 0xFFF);
                                            } else {
                                                dst_pt[l] = src_pt[l];  // Kernel page
                                            }
                                        }
                                    }

                                    dst_pd[k] = new_pt_phys | (src_pd[k] & 0xFFF);
                                }
                            }
                        }

                        dst_pdpt[j] = new_pd_phys | (src_pdpt[j] & 0xFFF);
                    }
                }
            }

            dst_pml4[i] = new_pdpt_phys | (src_pml4[i] & 0xFFF);
        }
    }

    // Copy kernel space entries (upper half, entries 256-511)
    for (int i = 256; i < 512; i++) {
        dst_pml4[i] = src_pml4[i];
    }

    return dst_pml4_phys;

fail:
    kprintf("[VMM] Failed to clone user space\n");
    vmm_destroy_user_space(dst_pml4_phys);
    return 0;
}

// #429: real copy-on-write clone for fork. Mirrors vmm_clone_user_space's
// page-table copy but at the leaf shares the physical page read-only with the
// COW bit set in BOTH parent and child; the #PF handler copies on first write.
uint64_t vmm_clone_user_space_cow(uint64_t src_pml4_phys) {
    // Declared in mm/demand.c.
    extern int  cow_trackable(uint64_t phys);
    extern void cow_fork_share(uint64_t phys);
    const uint64_t VMM_COW_BIT = (1ULL << 9);  // #429 leaf COW marker

    if (src_pml4_phys == 0) return vmm_create_user_space();

    uint64_t dst_pml4_phys = pmm_alloc_page();
    if (dst_pml4_phys == 0) return 0;

    uint64_t *src_pml4 = (uint64_t*)src_pml4_phys;
    uint64_t *dst_pml4 = (uint64_t*)dst_pml4_phys;
    memset(dst_pml4, 0, 4096);
    int parent_pte_changed = 0;

    for (int i = 0; i < 256; i++) {  // user half only
        if (!(src_pml4[i] & VMM_FLAG_PRESENT)) continue;

        uint64_t *src_pdpt = (uint64_t*)(src_pml4[i] & VMM_ADDR_MASK);
        uint64_t new_pdpt_phys = pmm_alloc_page();
        if (!new_pdpt_phys) goto fail;
        uint64_t *dst_pdpt = (uint64_t*)new_pdpt_phys;
        memset(dst_pdpt, 0, 4096);

        for (int j = 0; j < 512; j++) {
            if (!(src_pdpt[j] & VMM_FLAG_PRESENT)) continue;
            if (src_pdpt[j] & VMM_FLAG_HUGE) {
                dst_pdpt[j] = src_pdpt[j];   // 1GB kernel page, share by ref
                continue;
            }
            uint64_t *src_pd = (uint64_t*)(src_pdpt[j] & VMM_ADDR_MASK);
            uint64_t new_pd_phys = pmm_alloc_page();
            if (!new_pd_phys) goto fail;
            uint64_t *dst_pd = (uint64_t*)new_pd_phys;
            memset(dst_pd, 0, 4096);

            for (int k = 0; k < 512; k++) {
                if (!(src_pd[k] & VMM_FLAG_PRESENT)) continue;
                if (src_pd[k] & VMM_FLAG_HUGE) {
                    if (src_pd[k] & VMM_FLAG_USER) {
                        // 2MB user huge page: eager-copy (COW refcount is
                        // per-4KB; keep it simple and correct).
                        uint64_t np = pmm_alloc_pages(512);
                        if (!np) goto fail;
                        memcpy((void*)np, (void*)(src_pd[k] & VMM_ADDR_MASK),
                               VMM_PAGE_SIZE_2M);
                        dst_pd[k] = np | (src_pd[k] & 0xFFF);
                    } else {
                        dst_pd[k] = src_pd[k];   // kernel huge page
                    }
                    continue;
                }
                uint64_t *src_pt = (uint64_t*)(src_pd[k] & VMM_ADDR_MASK);
                uint64_t new_pt_phys = pmm_alloc_page();
                if (!new_pt_phys) goto fail;
                uint64_t *dst_pt = (uint64_t*)new_pt_phys;
                memset(dst_pt, 0, 4096);

                for (int l = 0; l < 512; l++) {
                    if (!(src_pt[l] & VMM_FLAG_PRESENT)) continue;
                    if (!(src_pt[l] & VMM_FLAG_USER)) {
                        dst_pt[l] = src_pt[l];   // kernel leaf, share by ref
                        continue;
                    }
                    uint64_t phys = src_pt[l] & VMM_ADDR_MASK;
                    if (cow_trackable(phys)) {
                        // Share read-only + COW in both address spaces.
                        uint64_t cow = (src_pt[l] & ~VMM_FLAG_WRITABLE) | VMM_COW_BIT;
                        dst_pt[l] = cow;
                        if (src_pt[l] != cow) {
                            src_pt[l] = cow;     // demote parent to read-only
                            parent_pte_changed = 1;
                        }
                        cow_fork_share(phys);
                    } else {
                        // Untrackable page: fall back to an eager private copy.
                        uint64_t np = pmm_alloc_page();
                        if (!np) goto fail;
                        memcpy((void*)np, (void*)phys, VMM_PAGE_SIZE_4K);
                        dst_pt[l] = np | (src_pt[l] & 0xFFF);
                    }
                }
                dst_pd[k] = new_pt_phys | (src_pd[k] & 0xFFF);
            }
            dst_pdpt[j] = new_pd_phys | (src_pdpt[j] & 0xFFF);
        }
        dst_pml4[i] = new_pdpt_phys | (src_pml4[i] & 0xFFF);
    }

    // Kernel half: share by value.
    for (int i = 256; i < 512; i++) dst_pml4[i] = src_pml4[i];

    // The parent's PTEs were demoted to read-only; flush the TLB so the
    // demotion takes effect before the parent runs again. fork() always runs in
    // the parent's context, so the live hardware CR3 (read_cr3()) IS the
    // parent's; flush unconditionally rather than trusting the software
    // current_pml4_phys shadow, which can lag the scheduler's context switches.
    // Without this the parent keeps a stale writable TLB entry, writes its
    // fork() return value straight into the still-shared page, and the child
    // then reads a NON-ZERO fork() return (runs the parent branch). #429.
    if (parent_pte_changed) {
        vmm_flush_tlb();
    }
    return dst_pml4_phys;

fail:
    kprintf("[VMM] COW clone failed\n");
    vmm_destroy_user_space(dst_pml4_phys);
    return 0;
}

// Destroy a user address space and free all associated pages
void vmm_destroy_user_space(uint64_t pml4_phys) {
    if (pml4_phys == 0 || pml4_phys == current_pml4_phys) {
        return;  // Don't destroy current or null address space
    }

    uint64_t *pml4 = (uint64_t*)pml4_phys;

    // Reference PDPT for PML4[0] taken from the CURRENT (kernel/parent) address
    // space. vmm_create_user_space copies the kernel's PDPT entries BY VALUE for
    // every slot except slot 2 (the 2-3GB user region), which it allocates
    // fresh. The copied slots therefore point at page tables that are SHARED
    // with the kernel and every other process. Freeing them here returns live
    // kernel/sibling page-table pages to the PMM, which then get handed back out
    // and corrupt unrelated memory (e.g. another process's stack). So we only
    // free page-table structures this address space actually OWNS: a PDPT slot
    // is owned when its value differs from the reference address space.
    uint64_t *ref_pml4 = (uint64_t*)current_pml4_phys;

    // Free user space pages only (lower half, entries 0-255)
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & VMM_FLAG_PRESENT)) continue;

        uint64_t *pdpt = (uint64_t*)(pml4[i] & VMM_ADDR_MASK);

        // #522 stage 0: the ownership rule, GENERALISED from "slot 0 only".
        //
        // It used to derive a reference PDPT for i == 0 ALONE, so for every
        // other PML4 slot `owned` collapsed to (i == 0 && j == 2), i.e. FALSE.
        // That was harmless while PML4[0] was the only slot a user address
        // space ever populated, and becomes a silent whole-window LEAK the
        // moment userland lives anywhere else: every user page, PD and PT under
        // the new slot would be skipped here while the PDPT page below is freed
        // regardless. MEASURED by vmm_user_window_selftest(), which fails on
        // the un-generalised rule.
        //
        // The correct rule is two-tier, and it hinges on whether the REFERENCE
        // address space has this PML4 slot at all:
        //   - reference HAS it  -> the tables under it may be SHARED with the
        //     kernel (this is PML4[0], whose PDPT slots are copied by value in
        //     vmm_create_user_space). Keep the per-slot comparison; freeing a
        //     shared kernel table would hand live page tables to the PMM.
        //   - reference LACKS it -> nothing outside this address space can be
        //     pointing into it, so every table beneath is exclusively ours.
        //     That is precisely the case for the new user window, because the
        //     kernel has no PML4[USER_WIN_PML4_INDEX].
        uint64_t *ref_pdpt = NULL;
        int slot_absent_in_ref = 1;
        if (ref_pml4 && (ref_pml4[i] & VMM_FLAG_PRESENT)) {
            slot_absent_in_ref = 0;
            ref_pdpt = (uint64_t*)(ref_pml4[i] & VMM_ADDR_MASK);
        }

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & VMM_FLAG_PRESENT)) continue;

            // Determine ownership. With a reference PDPT, a slot is owned if its
            // value differs from the reference (shared kernel slots match). With
            // no reference available, only the fresh 2-3GB user slot (PML4[0],
            // PDPT[2]) is known to be owned.
            int owned;
            if (slot_absent_in_ref) {
                owned = 1;                        // whole slot is exclusively ours
            } else if (ref_pdpt) {
                owned = (pdpt[j] != ref_pdpt[j]);
            } else {
                owned = (i == 0 && j == 2);
            }
            if (!owned) continue;

            if (pdpt[j] & VMM_FLAG_HUGE) continue;  // 1GB huge: nothing to walk

            uint64_t *pd = (uint64_t*)(pdpt[j] & VMM_ADDR_MASK);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & VMM_FLAG_PRESENT)) continue;
                if (!(pd[k] & VMM_FLAG_HUGE)) {
                    uint64_t *pt = (uint64_t*)(pd[k] & VMM_ADDR_MASK);
                    for (int l = 0; l < 512; l++) {
                        if ((pt[l] & VMM_FLAG_PRESENT) &&
                            (pt[l] & VMM_FLAG_USER)) {
                            vmm_free_user_page_cow(pt[l] & VMM_ADDR_MASK);  // #429 COW-aware
                        }
                    }
                    pmm_free_page((uint64_t)pt);
                } else if (pd[k] & VMM_FLAG_USER) {
                    pmm_free_pages(pd[k] & VMM_ADDR_MASK, 512);
                }
            }

            pmm_free_page((uint64_t)pd);
        }

        // The PDPT page itself is freshly allocated per address space in
        // vmm_create_user_space, so it is always owned and safe to free.
        pmm_free_page((uint64_t)pdpt);
    }

    pmm_free_page(pml4_phys);
}

// Map a page in a specific address space

// #522/#629: TLB-flush liveness test. `current_pml4_phys` is a SOFTWARE SHADOW
// updated only by vmm_switch_address_space(); the scheduler's context switches
// can leave it lagging the hardware CR3 (the same trap #429 hit, see the
// comment at vmm_clone_user_space_cow()). Gating an invlpg on the stale shadow
// means a PTE we just cleared keeps a live TLB entry, so an unmapped page stays
// readable. MEASURED, not theorised: with the shadow-only test, mmtest (2)
// reported fault[p0..p3]=0000 after munmap()ing pages 1 and 2, i.e. NEITHER
// unmapped page faulted. Ask the hardware, and keep the shadow as a fallback.
static inline int vmm_space_is_live(uint64_t pml4_phys) {
    return pml4_phys == (read_cr3() & VMM_ADDR_MASK) || pml4_phys == current_pml4_phys;
}

int vmm_map_page_in(uint64_t pml4_phys, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    // #640 leg 4 step 2: NXE guard, at the ONE place every mapping passes.
    // Bit 63 is the NX bit only while EFER.NXE is set; with NXE clear it is a
    // RESERVED bit, and a PTE with a reserved bit set raises a reserved-bit
    // page fault on first touch rather than doing nothing. Callers should be
    // able to ask for VMM_FLAG_NX without each one re-deriving whether NXE
    // happens to be on, so the check lives here instead of in every caller.
    // On a CPU or boot path where NXE never came up, this silently degrades to
    // the old executable mapping, which is exactly the pre-#640 behaviour.
    {
        extern int g_nx_enabled;   // mm/fault.c, set by cpu_enable_nx()
        if (!g_nx_enabled) flags &= ~VMM_FLAG_NX;
    }

    if (pml4_phys == 0) {
        return -1;
    }

    uint64_t *pml4 = (uint64_t*)pml4_phys;

    virt_addr &= ~0xFFFULL;
    phys_addr &= ~0xFFFULL;

    // Get or create PML4 entry
    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    uint64_t *pdpt = vmm_get_or_create_entry_in(pml4, pml4_idx, flags);
    if (!pdpt) { kprintf("[VMM] Failed at PDPT creation, pml4_idx=%d\n", (int)pml4_idx); return -1; }
    if (!pdpt) return -1;

    // Get or create PDPT entry
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    uint64_t *pd = vmm_get_or_create_entry_in(pdpt, pdpt_idx, flags);
    if (!pd) { kprintf("[VMM] Failed at PD creation, pdpt_idx=%d\n", (int)pdpt_idx); return -1; }
    if (!pd) return -1;

    // Get or create PD entry
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    if (pd[pd_idx] & VMM_FLAG_HUGE) {
        // Break 2MB huge page into 512 individual 4KB pages
        uint64_t huge_phys = pd[pd_idx] & VMM_ADDR_MASK;
        uint64_t huge_flags = pd[pd_idx] & 0xFFFULL & ~VMM_FLAG_HUGE;
        uint64_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return -1;
        uint64_t *pt_new = (uint64_t *)pt_phys;
        for (int j = 0; j < 512; j++) {
            pt_new[j] = (huge_phys + j * 4096) | huge_flags | VMM_FLAG_PRESENT;
        }
        pd[pd_idx] = pt_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | (flags & VMM_FLAG_USER);
    }
    uint64_t *pt = vmm_get_or_create_entry_in(pd, pd_idx, flags);
    if (!pt) { kprintf("[VMM] Failed at PT creation, pd_idx=%d\n", (int)pd_idx); return -1; }
    if (!pt) return -1;

    // Set PT entry
    uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
    pt[pt_idx] = phys_addr | flags | VMM_FLAG_PRESENT;

    // Invalidate TLB if this is the current address space
    if (vmm_space_is_live(pml4_phys)) {
        vmm_invlpg(virt_addr);
    }

    return 0;
}

// Unmap a page in a specific address space
void vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt_addr) {
    if (pml4_phys == 0) return;

    uint64_t *pml4 = (uint64_t*)pml4_phys;
    virt_addr &= ~0xFFFULL;

    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    if (!(pml4[pml4_idx] & VMM_FLAG_PRESENT)) return;

    uint64_t *pdpt = (uint64_t*)(pml4[pml4_idx] & VMM_ADDR_MASK);
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) return;

    uint64_t *pd = (uint64_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) return;

    if (pd[pd_idx] & VMM_FLAG_HUGE) {
        pd[pd_idx] = 0;
    } else {
        uint64_t *pt = (uint64_t*)(pd[pd_idx] & VMM_ADDR_MASK);
        uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
        pt[pt_idx] = 0;
    }

    if (vmm_space_is_live(pml4_phys)) {
        vmm_invlpg(virt_addr);
    }
}

// Get physical address in a specific address space
// #500 / MAYTERA-SEC-2026-0016. See the contract note in vmm.h.
//
// This mirrors the CPU's own permission computation. For a Ring-3 access the
// hardware permits it only if the U/S bit is set at EVERY level of the walk,
// and permits a write only if R/W is set at every level. So we AND those bits
// down the walk and OR the NX bits. Checking only the leaf would be wrong in
// both directions, and checking only presence (as vmm_get_physical_in does) is
// what let a user pointer naming kernel memory pass validation.
uint64_t vmm_get_effective_flags_in(uint64_t pml4_phys, uint64_t virt_addr) {
    if (pml4_phys == 0) return 0;

    // Start permissive and AND the restrictions in, level by level.
    uint64_t eff_user = VMM_FLAG_USER;
    uint64_t eff_write = VMM_FLAG_WRITABLE;
    uint64_t eff_nx = 0;

    uint64_t *pml4 = (uint64_t *)pml4_phys;
    uint64_t e = pml4[VMM_PML4_INDEX(virt_addr)];
    if (!(e & VMM_FLAG_PRESENT)) return 0;
    eff_user &= e; eff_write &= e; eff_nx |= (e & VMM_FLAG_NX);

    uint64_t *pdpt = (uint64_t *)(e & VMM_ADDR_MASK);
    e = pdpt[VMM_PDPT_INDEX(virt_addr)];
    if (!(e & VMM_FLAG_PRESENT)) return 0;
    eff_user &= e; eff_write &= e; eff_nx |= (e & VMM_FLAG_NX);
    if (e & VMM_FLAG_HUGE) {   // 1GB page: the walk ends here
        return VMM_FLAG_PRESENT | eff_user | eff_write | eff_nx;
    }

    uint64_t *pd = (uint64_t *)(e & VMM_ADDR_MASK);
    e = pd[VMM_PD_INDEX(virt_addr)];
    if (!(e & VMM_FLAG_PRESENT)) return 0;
    eff_user &= e; eff_write &= e; eff_nx |= (e & VMM_FLAG_NX);
    if (e & VMM_FLAG_HUGE) {   // 2MB page: the walk ends here
        return VMM_FLAG_PRESENT | eff_user | eff_write | eff_nx;
    }

    uint64_t *pt = (uint64_t *)(e & VMM_ADDR_MASK);
    e = pt[VMM_PT_INDEX(virt_addr)];
    if (!(e & VMM_FLAG_PRESENT)) return 0;
    eff_user &= e; eff_write &= e; eff_nx |= (e & VMM_FLAG_NX);

    return VMM_FLAG_PRESENT | eff_user | eff_write | eff_nx;
}

uint64_t vmm_get_physical_in(uint64_t pml4_phys, uint64_t virt_addr) {
    if (pml4_phys == 0) return 0;

    uint64_t *pml4 = (uint64_t*)pml4_phys;

    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    if (!(pml4[pml4_idx] & VMM_FLAG_PRESENT)) return 0;

    uint64_t *pdpt = (uint64_t*)(pml4[pml4_idx] & VMM_ADDR_MASK);
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) return 0;

    if (pdpt[pdpt_idx] & VMM_FLAG_HUGE) {
        return (pdpt[pdpt_idx] & VMM_ADDR_MASK) | (virt_addr & 0x3FFFFFFF);
    }

    uint64_t *pd = (uint64_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) return 0;

    if (pd[pd_idx] & VMM_FLAG_HUGE) {
        return (pd[pd_idx] & VMM_ADDR_MASK) | (virt_addr & 0x1FFFFF);
    }

    uint64_t *pt = (uint64_t*)(pd[pd_idx] & VMM_ADDR_MASK);
    uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
    if (!(pt[pt_idx] & VMM_FLAG_PRESENT)) return 0;

    return (pt[pt_idx] & VMM_ADDR_MASK) | VMM_PAGE_OFFSET(virt_addr);
}

// #429: return the raw leaf PTE (flags + phys) mapping virt_addr, or 0 if there
// is no present 4KB leaf. Huge (2MB/1GB) mappings return 0 (no 4KB leaf).
uint64_t vmm_get_pte_in(uint64_t pml4_phys, uint64_t virt_addr) {
    if (pml4_phys == 0) return 0;
    uint64_t *pml4 = (uint64_t*)pml4_phys;

    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    if (!(pml4[pml4_idx] & VMM_FLAG_PRESENT)) return 0;

    uint64_t *pdpt = (uint64_t*)(pml4[pml4_idx] & VMM_ADDR_MASK);
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) return 0;
    if (pdpt[pdpt_idx] & VMM_FLAG_HUGE) return 0;

    uint64_t *pd = (uint64_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) return 0;
    if (pd[pd_idx] & VMM_FLAG_HUGE) return 0;

    uint64_t *pt = (uint64_t*)(pd[pd_idx] & VMM_ADDR_MASK);
    uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
    if (!(pt[pt_idx] & VMM_FLAG_PRESENT)) return 0;
    return pt[pt_idx];
}

// #511 ROOT FIX: punch a DEMAND (VMA-backed) range to genuinely NOT-PRESENT in
// a user address space, so a first access by EITHER ring (Ring 3 OR Ring 0)
// takes a real #PF and is resolved through mm_fault() -> handle_lazy_fault()
// onto a proper USER page.
//
// WHY THIS EXISTS. vmm_create_user_space() pre-populates PDPT[2] (the 2-3GB
// user window) with identity 2MB huge pages that are PRESENT|WRITABLE but carry
// NO USER bit. That identity map is LOAD-BEARING and MUST stay: device MMIO BARs
// live in this same physical/virtual window (the QEMU std-VGA framebuffer at
// 0x80000000, the E1000 NIC registers at 0x81600000, ...), and the kernel reaches
// them through this identity map from WHATEVER address space is current when an
// IRQ or a syscall touches the device. Removing it panics the kernel the moment
// a NIC IRQ (e1000_read) fires while a user CR3 is loaded. So we cannot simply
// leave the whole window unmapped.
//
// But a lazy/anonymous VMA region (the userland heap at 0x90000000, mmap at
// 0xA0000000) is NOT a device: it is ordinary demand-zero memory that must end
// up as a private USER page. While it still sits under the identity huge page it
// is PRESENT+WRITABLE+SUPERVISOR, so a Ring-0 write into it (copy_to_user, the
// sys_read memcpy, any kernel->user copy that spans >1 page) does NOT fault - a
// supervisor access ignores the U/S bit - and the write silently lands on
// identity physical memory, then vanishes the instant Ring 3 first touches the
// page and takes ITS own #PF into a fresh demand-zeroed frame. Net effect: only
// already-present pages got the kernel's data; the rest were lost (#416 ">1MB
// read returns 0", the sys_read zero-fill class).
//
// The fix is to remove ONLY the demand region's identity backing, leaving the
// surrounding identity map (and all device MMIO, which has no VMA and is never
// punched) intact. Called from sys_mmap() right after a lazy VMA is registered,
// so from then on the fault handler is load-bearing for that region for BOTH
// rings and copy_to_user "just works" across page boundaries with no per-caller
// prefault. mm_prefault_range() (#510) is left as a cheap optimization only.
//
// Idempotent and conservative: only PRESENT, non-USER (i.e. still-identity)
// leaves are cleared. A page already faulted in as a real USER page, or already
// not-present, is left untouched, so re-mmap / overlap never destroys live data.
void vmm_punch_demand_range(uint64_t pml4_phys, uint64_t start, uint64_t len) {
    if (pml4_phys == 0 || len == 0) return;

    uint64_t va  = start & ~(VMM_PAGE_SIZE_4K - 1);
    uint64_t end = (start + len + VMM_PAGE_SIZE_4K - 1) & ~(VMM_PAGE_SIZE_4K - 1);
    if (end <= va) return;   // overflow / bogus range: do nothing

    uint64_t *pml4 = (uint64_t*)pml4_phys;

    for (; va < end; va += VMM_PAGE_SIZE_4K) {
        uint64_t pml4_idx = VMM_PML4_INDEX(va);
        if (!(pml4[pml4_idx] & VMM_FLAG_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t*)(pml4[pml4_idx] & VMM_ADDR_MASK);
        uint64_t pdpt_idx = VMM_PDPT_INDEX(va);
        if (!(pdpt[pdpt_idx] & VMM_FLAG_PRESENT)) continue;
        if (pdpt[pdpt_idx] & VMM_FLAG_HUGE) continue;   // 1GB huge (not user window)

        uint64_t *pd = (uint64_t*)(pdpt[pdpt_idx] & VMM_ADDR_MASK);
        uint64_t pd_idx = VMM_PD_INDEX(va);
        if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) continue;

        if (pd[pd_idx] & VMM_FLAG_HUGE) {
            // Split the 2MB identity huge page into 512 identity 4KB PTEs so we
            // can clear a single page without disturbing the rest of the 2MB
            // (which may be other identity / device memory). Mirrors the split
            // in vmm_map_page_in(): siblings keep PRESENT|WRITABLE, no USER.
            uint64_t huge_phys  = pd[pd_idx] & VMM_ADDR_MASK;
            uint64_t huge_flags = pd[pd_idx] & 0xFFFULL & ~VMM_FLAG_HUGE;
            uint64_t pt_phys = pmm_alloc_page();
            if (!pt_phys) return;   // OOM: best-effort, leave the rest identity
            uint64_t *pt_new = (uint64_t*)pt_phys;
            for (int j = 0; j < 512; j++) {
                pt_new[j] = (huge_phys + (uint64_t)j * 4096) | huge_flags | VMM_FLAG_PRESENT;
            }
            pd[pd_idx] = pt_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;
        }
        if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) continue;

        uint64_t *pt = (uint64_t*)(pd[pd_idx] & VMM_ADDR_MASK);
        uint64_t pt_idx = VMM_PT_INDEX(va);
        uint64_t e = pt[pt_idx];
        // Only clear a still-identity (present, no USER bit) leaf. Never clear a
        // real USER page (already faulted in) or an already-not-present slot.
        if ((e & VMM_FLAG_PRESENT) && !(e & VMM_FLAG_USER)) {
            pt[pt_idx] = 0;
            if (vmm_space_is_live(pml4_phys)) vmm_invlpg(va);
        }
    }
}

// Allocate and map user pages at a virtual address
int vmm_alloc_user_pages(uint64_t pml4_phys, uint64_t virt_addr, uint64_t count, uint64_t flags) {
    virt_addr &= ~0xFFFULL;

    for (uint64_t i = 0; i < count; i++) {
        uint64_t page = pmm_alloc_page();
        if (page == 0) {
            kprintf("[VMM] alloc_user_pages: pmm_alloc_page failed at page %llu\n", i);
            LOG_ERROR("[VMM] Out of physical memory for user pages");
            // Failed - free what we allocated
            vmm_free_user_pages(pml4_phys, virt_addr, i);
            return -1;
        }

        memset((void*)page, 0, VMM_PAGE_SIZE_4K);

        if (vmm_map_page_in(pml4_phys, virt_addr + i * VMM_PAGE_SIZE_4K,
                           page, flags | VMM_FLAG_USER) != 0) {
            kprintf("[VMM] alloc_user_pages: vmm_map_page_in failed for vaddr=0x%llX\n",
                    virt_addr + i * VMM_PAGE_SIZE_4K);
            LOG_ERROR("[VMM] Failed to map page in user address space");
            pmm_free_page(page);
            vmm_free_user_pages(pml4_phys, virt_addr, i);
            return -1;
        }
    }

    return 0;
}

// #640 leg 4 step 2. See the contract in vmm.h. This is the ONLY place in the
// kernel that rewrites the protection bits of an existing user PTE; mprotect's
// vma_reprotect_pages() delegates here rather than keeping a second copy, so
// the COW and NXE rules cannot drift between the two paths.
uint64_t vmm_protect_user_range(uint64_t pml4_phys, uint64_t virt_addr,
                                uint64_t count, uint64_t keep) {
    extern int g_nx_enabled;   // mm/fault.c, set by cpu_enable_nx()

    virt_addr &= ~0xFFFULL;
    uint64_t changed = 0;

    for (uint64_t i = 0; i < count; i++) {
        uint64_t page = virt_addr + i * VMM_PAGE_SIZE_4K;
        uint64_t pte  = vmm_get_pte_in(pml4_phys, page);

        if (!(pte & VMM_FLAG_PRESENT)) continue;
        if (!(pte & VMM_FLAG_USER))    continue;   // identity backing, not ours

        uint64_t phys = pte & VMM_ADDR_MASK;
        uint64_t nf   = VMM_FLAG_PRESENT | VMM_FLAG_USER;

        // COW wins over a writable request: granting WRITABLE on a fork-shared
        // frame un-shares it WITHOUT copying, so stores would land in the frame
        // the other process is still reading. Leave it read-only and let the
        // fault path do the copy. This is the #628 rule, stated once.
        if (pte & PTE_COW_FLAG) {
            nf |= PTE_COW_FLAG;
        } else if (keep & VMM_FLAG_WRITABLE) {
            nf |= VMM_FLAG_WRITABLE;
        }

        if ((keep & VMM_FLAG_NX) && g_nx_enabled) {
            nf |= VMM_FLAG_NX;
        }

        if (nf != (pte & (VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE |
                          VMM_FLAG_NX | PTE_COW_FLAG))) {
            changed++;
        }
        vmm_map_page_in(pml4_phys, page, phys, nf);
    }

    return changed;
}

// Free user pages and their physical memory
void vmm_free_user_pages(uint64_t pml4_phys, uint64_t virt_addr, uint64_t count) {
    virt_addr &= ~0xFFFULL;

    for (uint64_t i = 0; i < count; i++) {
        uint64_t addr = virt_addr + i * VMM_PAGE_SIZE_4K;
        uint64_t phys = vmm_get_physical_in(pml4_phys, addr);
        if (phys) {
            vmm_unmap_page_in(pml4_phys, addr);
            vmm_free_user_page_cow(phys);  // #429 COW-aware
        }
    }
}

// ============================================================================
// DEBUG instrumentation: use-after-free detector for user pages.
// Scans the CURRENTLY-LIVE user address space (current_pml4_phys) for a
// USER-flagged mapping of `phys`. Returns the VA it is mapped at, or 0 if not
// mapped. pmm_free_page() calls this to catch a page that is still live in the
// running process (e.g. the terminal) being wrongly returned to the physical
// allocator during another process's teardown. Only walks PML4[0] -> PDPT[2]
// (the 2-3GB user region) where all user code/data/stack live, so it is cheap.
// ============================================================================
uint64_t vmm_dbg_user_va_for_phys(uint64_t phys) {
    uint64_t live = current_pml4_phys;
    if (live == 0) return 0;
    phys &= VMM_ADDR_MASK;

    // Structural pages of the live address space. Freeing any of these
    // corrupts the running process even though no leaf scan would find it.
    // Sentinels: 0xT1=PML4 page, 0xT2=PDPT page, 0xT3=PD page, 0xT4=PT page.
    if ((live & VMM_ADDR_MASK) == phys) return 0xD1000000000ULL; /* PML4 page */

    uint64_t *pml4 = (uint64_t*)live;
    if (!(pml4[0] & VMM_FLAG_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t*)(pml4[0] & VMM_ADDR_MASK);
    if ((pml4[0] & VMM_ADDR_MASK) == phys) return 0xD2000000000ULL; /* PDPT page */

    if (!(pdpt[2] & VMM_FLAG_PRESENT)) return 0;
    if (pdpt[2] & VMM_FLAG_HUGE) return 0;
    uint64_t *pd = (uint64_t*)(pdpt[2] & VMM_ADDR_MASK);
    if ((pdpt[2] & VMM_ADDR_MASK) == phys) return 0xD3000000000ULL; /* PD page */

    for (int k = 0; k < 512; k++) {
        if (!(pd[k] & VMM_FLAG_PRESENT)) continue;
        if (pd[k] & VMM_FLAG_HUGE) {
            if ((pd[k] & VMM_FLAG_USER) &&
                (pd[k] & VMM_ADDR_MASK) <= phys &&
                phys < ((pd[k] & VMM_ADDR_MASK) + 0x200000ULL)) {
                return 0x80000000ULL | ((uint64_t)k << 21);
            }
            continue;
        }
        uint64_t *pt = (uint64_t*)(pd[k] & VMM_ADDR_MASK);
        if ((pd[k] & VMM_ADDR_MASK) == phys) return 0xD4000000000ULL | ((uint64_t)k << 21); /* PT page */
        for (int l = 0; l < 512; l++) {
            if (!(pt[l] & VMM_FLAG_PRESENT)) continue;
            if (!(pt[l] & VMM_FLAG_USER)) continue;
            if ((pt[l] & VMM_ADDR_MASK) == phys) {
                return 0x80000000ULL | ((uint64_t)k << 21) | ((uint64_t)l << 12);
            }
        }
    }
    return 0;
}

// ===========================================================================
// #642: Page Attribute Table - a genuine write-combining memory type
//
// The framebuffer present (fb_swap_buffers / fb_swap_dirty_rects, called from
// sys_fb_flip with interrupts off) copies the back buffer, which is ordinary
// WB kernel RAM, to the front buffer, which on real hardware is the GOP
// framebuffer BAR. Firmware maps that BAR UC in the MTRRs. A UC page table
// entry plus a UC MTRR is UC, so every 16-byte movdqu store in memcpy_fast
// became its own uncached transaction across PCIe.
//
// Write-combining collects those stores in a WC buffer and issues one 64-byte
// burst per filled line. memcpy_fast's inner loop is four consecutive movdqu
// stores covering exactly 64 bytes, which is the ideal WC fill pattern, so the
// copy itself needs no change: only the memory type, and a fence at the end.
//
// This code is C rather than Rust because it is WRMSR, CR0/CR3/CR4 writes,
// WBINVD and raw page-table entry bit manipulation: inline asm and pointer
// arithmetic over hardware structures with no safe-Rust representation. The
// POLICY that uses it (which range, which type) lives in the caller.
// ===========================================================================

#define IA32_PAT_MSR   0x277
#define CR0_NW_BIT     (1ULL << 29)
#define CR0_CD_BIT     (1ULL << 30)
#define CR4_PGE_BIT    (1ULL << 7)
#define CR0_WP_BIT     (1ULL << 16)   // supervisor write protect

static int      g_pat_ok      = 0;   // 1 once slot 4 verifiably holds WC
static int      g_pat_tried   = 0;
static uint64_t g_pat_value   = 0;   // the value we installed, as read back

int vmm_pat_available(void) { return g_pat_ok; }

int vmm_pat_init(void) {
    if (g_pat_tried) return g_pat_ok;
    g_pat_tried = 1;

    // CPUID.01H:EDX[16] = PAT. A WRMSR to 0x277 without it is a #GP, which at
    // this point in boot is an unrecoverable triple fault, so this check is not
    // optional politeness.
    uint32_t a = 1, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(d & (1u << 16))) {
        kprintf("[PAT] CPUID.01H:EDX[16] clear: no PAT on this CPU, "
                "framebuffer keeps its firmware memory type\n");
        return 0;
    }

    uint64_t old = rdmsr(IA32_PAT_MSR);

    // Rewrite slot 1 (bits 15:8) to WC and leave every other slot exactly as
    // found. Nothing in this kernel sets PWT, so slot 1 is unreachable by every
    // existing mapping and this write cannot change the memory type of anything
    // already mapped. See the slot-choice rationale in vmm.h.
    uint64_t neu = (old & ~(0xFFULL << 8)) | ((uint64_t)VMM_PAT_WC << 8);

    // SDM Vol 3A "Programming the PAT" / MTRR-change sequence. Interrupts off,
    // caches disabled and written back, TLB and global pages flushed around the
    // WRMSR, so no line can survive with the old type attached.
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
    __asm__ volatile("cli");

    uint64_t cr0_saved, cr4_saved;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0_saved));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4_saved));

    // CD=1, NW=0: no-fill cache mode.
    __asm__ volatile("mov %0, %%cr0" :: "r"((cr0_saved | CR0_CD_BIT) & ~CR0_NW_BIT) : "memory");
    __asm__ volatile("wbinvd" ::: "memory");

    // Global pages survive a plain CR3 reload, so PGE must come off first.
    int had_pge = (cr4_saved & CR4_PGE_BIT) ? 1 : 0;
    if (had_pge) __asm__ volatile("mov %0, %%cr4" :: "r"(cr4_saved & ~CR4_PGE_BIT) : "memory");
    vmm_flush_tlb();

    wrmsr(IA32_PAT_MSR, neu);

    vmm_flush_tlb();
    __asm__ volatile("wbinvd" ::: "memory");
    if (had_pge) __asm__ volatile("mov %0, %%cr4" :: "r"(cr4_saved) : "memory");
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0_saved) : "memory");

    if (rflags & (1ULL << 9)) __asm__ volatile("sti");

    // VERIFY, do not assume. A WRMSR that silently did not take would leave
    // slot 1 at WT, and the framebuffer remap would then quietly produce a
    // write-THROUGH mapping: exactly the bug this task exists to fix, wearing
    // the name of the fix. Report the readback either way.
    g_pat_value = rdmsr(IA32_PAT_MSR);
    uint8_t slot1 = (uint8_t)((g_pat_value >> 8) & 0xFF);
    g_pat_ok = (slot1 == VMM_PAT_WC);

    kprintf("[PAT] IA32_PAT 0x%lx -> 0x%lx (slot1=0x%x %s)\n",
            old, g_pat_value, slot1, g_pat_ok ? "WC OK" : "NOT WC");
    if (!g_pat_ok) {
        kprintf("[PAT] ERROR: slot 1 did not take WC; refusing to re-type anything\n");
    }
    return g_pat_ok;
}


// index = (PAT << 2) | (PCD << 1) | PWT. With WC in slot 1 the PAT bit is never
// needed, so `large` only matters if a future caller asks for slots 4-7.
static uint64_t vmm_pat_bits(int idx, int large) {
    uint64_t f = 0;
    if (idx & 1) f |= VMM_FLAG_PWT;
    if (idx & 2) f |= VMM_FLAG_PCD;
    if (idx & 4) f |= large ? VMM_FLAG_PAT_LARGE : VMM_FLAG_PAT;
    return f;
}

#define VMM_PAT_MASK_4K    (VMM_FLAG_PWT | VMM_FLAG_PCD | VMM_FLAG_PAT)
#define VMM_PAT_MASK_LARGE (VMM_FLAG_PWT | VMM_FLAG_PCD | VMM_FLAG_PAT_LARGE)

// ---------------------------------------------------------------------------
// TWO THINGS THIS FUNCTION LEARNED THE HARD WAY, both found by page-faulting on
// the first two boots. Neither is caused by #642; both are pre-existing
// properties of this kernel that #642 was simply the first code to touch.
//
// (1) THE UEFI PAGE TABLES WE RUN ON ARE MAPPED READ-ONLY.
//     [EXCEPTION] Page Fault err=0x3 (P W S) CR2=0x7bc05000, RIP inside this
//     function, writing pd[i]. A SUPERVISOR WRITE TO A PRESENT PAGE faults only
//     when that page is read-only and CR0.WP is set. vmm_init() adopts the
//     firmware's tables wholesale ("we'll keep using UEFI's page tables"), and
//     UEFI write-protects its own paging structures. The rest of the kernel
//     never noticed because it only ever writes into page-table pages IT
//     allocated: vmm_create_user_space() deep-copies PML4[0] into fresh pages.
//     Modifying the INHERITED tables in place, which is exactly what re-typing
//     the identity-mapped framebuffer means, is the unusual act. So the writes
//     below run with CR0.WP cleared and interrupts masked.
//
// (2) THE PMM HANDS OUT LIVE UEFI PAGE-TABLE PAGES.
//     The first version of this function split large pages instead of re-typing
//     them, and faulted writing the table it had just allocated. That allocation
//     returned 0x7bc05000, which the second fault then identified as the LIVE PD
//     mapping the framebuffer: pmm_alloc_page() had handed back a paging
//     structure the CPU was actively walking. It is read-only, so it faulted
//     instead of silently corrupting the address space, which is the only reason
//     this has not already caused an unexplained crash. That is a PMM bug and it
//     is logged in blame.md; it is not fixed here.
//
// Between them they are why this re-types large pages IN PLACE and allocates
// NOTHING. That is possible only because WC lives in PAT slot 1, so selecting it
// needs only PWT, which is bit 3 at EVERY page level. No split, no allocation,
// no dependence on the PMM being trustworthy this early in boot.
//
// The cost is granularity: a large page is re-typed only when the WHOLE granule
// lies inside the requested range, so a partially-covered granule at either end
// keeps its original type. Returns the number of BYTES actually re-typed, which
// lets the caller report real coverage instead of assuming 100%.
// ---------------------------------------------------------------------------
int64_t vmm_set_memtype_range(uint64_t pml4_phys, uint64_t virt_addr,
                              uint64_t size, int pat_index) {
    if (!pml4_phys || !size) return -1;
    if (pat_index < 0 || pat_index > 7) return -1;

    uint64_t start = virt_addr & ~0xFFFULL;
    uint64_t end   = (virt_addr + size + 0xFFF) & ~0xFFFULL;
    uint64_t *pml4 = (uint64_t *)pml4_phys;
    int live = vmm_space_is_live(pml4_phys);

    uint64_t bits_4k    = vmm_pat_bits(pat_index, 0);
    uint64_t bits_large = vmm_pat_bits(pat_index, 1);
    uint64_t done = 0, skipped = 0;

    // See note (1) above. Interrupts off for the whole walk so that nothing else
    // can run while supervisor write protection is off, and CR0 is restored on
    // every exit path including the early returns.
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
    __asm__ volatile("cli");
    uint64_t cr0_saved;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0_saved));
    if (cr0_saved & CR0_WP_BIT)
        __asm__ volatile("mov %0, %%cr0" :: "r"(cr0_saved & ~CR0_WP_BIT) : "memory");

#define VMM_MEMTYPE_UNWIND()                                                   \
    do {                                                                       \
        __asm__ volatile("mov %0, %%cr0" :: "r"(cr0_saved) : "memory");        \
        if (rflags & (1ULL << 9)) __asm__ volatile("sti");                     \
    } while (0)

    for (uint64_t va = start; va < end; ) {
        if (!(pml4[VMM_PML4_INDEX(va)] & VMM_FLAG_PRESENT)) {
            kprintf("[PAT] set_memtype: 0x%lx not mapped (PML4)\n", va);
            VMM_MEMTYPE_UNWIND();
            return -1;
        }
        uint64_t *pdpt = (uint64_t *)(pml4[VMM_PML4_INDEX(va)] & VMM_ADDR_MASK);
        uint64_t pdpt_i = VMM_PDPT_INDEX(va);
        if (!(pdpt[pdpt_i] & VMM_FLAG_PRESENT)) {
            kprintf("[PAT] set_memtype: 0x%lx not mapped (PDPT)\n", va);
            VMM_MEMTYPE_UNWIND();
            return -1;
        }

        // 1GB leaf.
        if (pdpt[pdpt_i] & VMM_FLAG_HUGE) {
            uint64_t g0 = va & ~(VMM_PAGE_SIZE_1G - 1);
            uint64_t g1 = g0 + VMM_PAGE_SIZE_1G;
            if (g0 >= start && g1 <= end) {
                pdpt[pdpt_i] = (pdpt[pdpt_i] & ~VMM_PAT_MASK_LARGE) | bits_large;
                if (live) vmm_invlpg(g0);
                done += VMM_PAGE_SIZE_1G;
            } else {
                skipped += (g1 < end ? g1 : end) - va;
            }
            va = g1;
            continue;
        }

        uint64_t *pd = (uint64_t *)(pdpt[pdpt_i] & VMM_ADDR_MASK);
        uint64_t pd_i = VMM_PD_INDEX(va);
        if (!(pd[pd_i] & VMM_FLAG_PRESENT)) {
            kprintf("[PAT] set_memtype: 0x%lx not mapped (PD)\n", va);
            VMM_MEMTYPE_UNWIND();
            return -1;
        }

        // 2MB leaf.
        if (pd[pd_i] & VMM_FLAG_HUGE) {
            uint64_t g0 = va & ~(VMM_PAGE_SIZE_2M - 1);
            uint64_t g1 = g0 + VMM_PAGE_SIZE_2M;
            if (g0 >= start && g1 <= end) {
                pd[pd_i] = (pd[pd_i] & ~VMM_PAT_MASK_LARGE) | bits_large;
                if (live) vmm_invlpg(g0);
                done += VMM_PAGE_SIZE_2M;
            } else {
                skipped += (g1 < end ? g1 : end) - va;
            }
            va = g1;
            continue;
        }

        // 4KB leaf: exact, no granularity loss.
        uint64_t *pt = (uint64_t *)(pd[pd_i] & VMM_ADDR_MASK);
        uint64_t pt_i = VMM_PT_INDEX(va);
        if (pt[pt_i] & VMM_FLAG_PRESENT) {
            pt[pt_i] = (pt[pt_i] & ~VMM_PAT_MASK_4K) | bits_4k;
            if (live) vmm_invlpg(va);
            done += VMM_PAGE_SIZE_4K;
        } else {
            skipped += VMM_PAGE_SIZE_4K;
        }
        va += VMM_PAGE_SIZE_4K;
    }

    VMM_MEMTYPE_UNWIND();
#undef VMM_MEMTYPE_UNWIND

    kprintf("[PAT] 0x%lx..0x%lx -> slot %d: %lu KB re-typed, %lu KB left as-is\n",
            start, end, pat_index, done / 1024, skipped / 1024);
    return (int64_t)done;
}
