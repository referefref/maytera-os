// elf.c - ELF64 Parser and Loader Implementation for MayteraOS
//
// This module provides ELF64 executable loading functionality for the
// MayteraOS kernel. It supports loading standard ELF64 executables and
// position-independent executables (PIE) for x86_64 architecture.

#include "../security/aslr.h"
#include "elf.h"
#include "../types.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../string.h"
#include "../security/uaccess_smap.h"  // #19/#645: AC bracket for the two foreign-CR3 user writes
#include "fs/bootlog.h"   // #742: the owning header, NOT a private extern

// ============================================================================
// Constants
// ============================================================================

// Maximum value for uint64_t (if not defined elsewhere)
#ifndef UINT64_MAX
#define UINT64_MAX          0xFFFFFFFFFFFFFFFFULL
#endif

// Default base address for loading PIE executables
// This should be in a safe region of the kernel's address space
#define PIE_DEFAULT_BASE    0x80000000ULL  // 2GB - VMM user space range

// #427: real per-object base for PIE executables loaded into a fresh user
// address space via elf_load_user(). Deliberately inside the same 2-3GB
// PDPT[2] window that vmm_create_user_space() clears for user mappings and
// that every fixed-base (non-PIE) app already uses successfully at
// 0x80000000 - just offset from it so a PIE image's own layout is visibly
// distinct. See the elf_load_user() comment at its use site for why the
// previous choice (USER_SPACE_START, 4MB) silently faulted on first write.
#define PIE_USER_BASE_LEGACY 0x90000000ULL  // 2.25GB - within PDPT[2]. RETIRED, see below.

// ===========================================================================
// #640 stage 2: PIE images load in the DEDICATED USERLAND WINDOW, at
// USER_WIN_IMAGE_BASE (mm/vmm.h) = 512GB, NOT at the 0x90000000 above.
//
// 0x90000000 was never a safe address and the comment above it, written for
// #427, is now known to be wrong in the one way that matters. MEASURED on the
// user's iMac14,4: the firmware's GOP framebuffer is at 0x90000000. That single
// address was simultaneously this PIE base AND libc's old HEAP_START, which is
// most of what #522/#650 are about. The 2-3GB window holds device MMIO whose
// address is chosen by FIRMWARE, so no layout that assumes any address in it is
// correct; #511 also pre-fills that window with identity huge pages, which is
// its own hazard for anything mapped there.
//
// USER_WIN_IMAGE_BASE is the stage-5 slot of the window #522 stage 0 created,
// in PML4[1], which the kernel's own address space leaves ABSENT. Stages 0, 2
// and 3 (window, mmap arena, libc heap) are already shipped and proven, so this
// is moving into a room that is already furnished, not opening a new one.
// USER_WIN_HEAP_BASE sits 1GB above the image base; the largest image in the
// tree is ~13MB, and ELF_USER_IMAGE_MAX_SPAN caps it at 256MB, so they cannot
// collide.
// ===========================================================================
#define PIE_USER_BASE       USER_WIN_IMAGE_BASE

// Page size for alignment calculations
#define ELF_PAGE_SIZE       4096ULL

// ===========================================================================
// #633: the address window a FIXED-BASE (ET_EXEC) user image may declare.
//
// USER_SPACE_START (4MB) / USER_SPACE_END (128TB) are the bounds of the whole
// lower half. They are NOT a policy: an ET_EXEC that declares p_vaddr =
// 0x400000 (what `ld` emits when an app Makefile forgets -T user.ld) passed
// that check and then took the loader into memory the fresh address space
// still carries KERNEL mappings for, which is how an unprivileged Ring 3
// launch could panic Ring 0.
//
// The real policy is narrow and is what every correctly-linked app already
// satisfies: the image lives in the userland window user.ld targets. MEASURED
// 2026-08-03 on the repo tree: 42 of 43 built app binaries are single-PT_LOAD
// ET_EXEC at exactly 0x80000000; the one exception (hello) is the #633 bug
// itself. Nothing legitimate is anywhere else, so this window costs nothing.
//
// #640 stage 3 tightens this further: once apps are PIE the kernel picks the
// base and a self-declared fixed base is refused outright, because honouring
// one would leave binaries in the identity/MMIO region we are vacating.
// ===========================================================================
// The LEGACY window: where every fixed-base (ET_EXEC) app still lives. This is
// also the reserved identity/MMIO window, which is exactly why #640 is moving
// userland out of it. Once the fleet is PIE, a self-declared base in here is
// REJECTED outright (stage 3) rather than merely tolerated, because honouring
// one would leave binaries in the region being vacated.
#define ELF_USER_IMAGE_MIN  0x80000000ULL   // 2GB, the user.ld base
#define ELF_USER_IMAGE_MAX  0xC0000000ULL   // 3GB, end of the PDPT[2] window

// The DESTINATION window for position-independent images (#640 stage 2).
#define ELF_USER_WIN_MIN    USER_WIN_BASE   // 512GB, PML4[1]
#define ELF_USER_WIN_MAX    USER_WIN_END    // 768GB

// Largest total virtual span (lowest..highest PT_LOAD page) a single user
// image may declare. Without this, an ELF with two PT_LOADs a terabyte apart
// asks the loader for a terabyte of page tables: an unprivileged OOM/DoS.
// The largest real image in the tree is ASSAULTCUBE at ~13MB.
#define ELF_USER_IMAGE_MAX_SPAN (256ULL * 1024 * 1024)

// ============================================================================
// Error Messages
// ============================================================================

static const char *elf_error_messages[] = {
    "Success",                                  // ELF_SUCCESS (0)
    "NULL pointer passed",                      // ELF_ERR_NULL_PTR (-1)
    "Data too small for ELF header",            // ELF_ERR_TOO_SMALL (-2)
    "Invalid ELF magic number",                 // ELF_ERR_BAD_MAGIC (-3)
    "Not a 64-bit ELF file",                    // ELF_ERR_NOT_64BIT (-4)
    "Not little-endian encoding",               // ELF_ERR_NOT_LE (-5)
    "Invalid ELF version",                      // ELF_ERR_BAD_VERSION (-6)
    "Not x86_64 architecture",                  // ELF_ERR_NOT_X86_64 (-7)
    "Not an executable or PIE",                 // ELF_ERR_NOT_EXEC (-8)
    "No program headers",                       // ELF_ERR_NO_PHDR (-9)
    "Program header offset/size overflow",      // ELF_ERR_PHDR_OVERFLOW (-10)
    "Segment offset/size overflow",             // ELF_ERR_SEGMENT_OVERFLOW (-11)
    "Memory allocation failed",                 // ELF_ERR_ALLOC_FAILED (-12)
    "Failed to load segment",                   // ELF_ERR_LOAD_FAILED (-13)
    "Load address outside the userland image window",  // ELF_ERR_BAD_LOAD_ADDR (-14)
    "Load destination not mapped writable",     // ELF_ERR_DEST_NOT_WRITABLE (-15)
};

#define ELF_ERROR_COUNT (sizeof(elf_error_messages) / sizeof(elf_error_messages[0]))

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Check if a value would overflow when added to another, i.e. "is a + b > max?"
 *
 * #489 C-FALLBACK HARDENING (#404 3-way drift audit, 2026-07-16). The original
 * body was:
 *     return (a > max - b);
 * which UNDERFLOWS when b > max: `max - b` wraps to a huge unsigned value, `a >
 * huge` is false, and the helper reports "no overflow" for a sum that massively
 * exceeds max. That is MAYTERA-SEC-2026-0003's root cause: an ELF whose
 * p_filesz > size passed calculate_load_bounds()'s file-bounds guard and
 * elf_load()'s copy loop then memcpy'd p_filesz bytes out of a `size`-byte image
 * (witnessed under ASan: READ of size 4728, 4,216 bytes past EOF).
 *
 * Until now the kernel was safe ONLY because -DRUST_ELF routes elf_validate() to
 * the Rust seam, which rejects such images before the loader runs. That made the
 * documented one-line rollback ("drop -DRUST_ELF and you are back to the previous
 * behavior") genuinely UNSAFE: it restored the OOB read AND the subsequent write.
 * The C is now sound on its own, independent of the flag.
 *
 * ZERO REGRESSION, by construction: when b <= max the added disjunct is false and
 * the expression reduces to EXACTLY the original `a > max - b`, so no image that
 * loads today can change verdict. The behavior differs only when b > max, where
 * a + b > max is unconditionally true (a is unsigned), so reporting overflow is
 * the correct answer and the old answer was simply wrong. Fail-closed.
 *
 * NOTE: elf_validate_full_c() deliberately does NOT use this hardened helper; it
 * keeps the original underflowing arithmetic so it stays a faithful reference of
 * the pre-port verdicts. See check_overflow_add_verbatim() below.
 */
static inline bool check_overflow_add(uint64_t a, uint64_t b, uint64_t max) {
    return (b > max) || (a > max - b);
}

/**
 * The ORIGINAL, UNDERFLOWING check_overflow_add, retained VERBATIM and used ONLY
 * by elf_validate_full_c(). Not a mistake and not dead code: elf_validate_full_c
 * is the #404 rollback/differential reference whose entire job is to reproduce the
 * pre-port C's verdicts, INCLUDING gap (1). If it silently inherited the #489
 * hardening it would stop being a reference, the boot differential would stop
 * showing the Rust's confinement, and the [RUST-SEC] claim would become false.
 * The LOADER path (calculate_load_bounds/elf_load/elf_parse_dynamic) uses the
 * hardened helper above, so the C is sound even when the reference accepts.
 */
static inline bool check_overflow_add_verbatim(uint64_t a, uint64_t b, uint64_t max) {
    return (a > max - b);
}

/**
 * Align a value up to the nearest boundary
 */
static inline uint64_t align_up(uint64_t value, uint64_t alignment) {
    if (alignment == 0) {
        return value;
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

/**
 * Align a value down to the nearest boundary
 */
static inline uint64_t align_down(uint64_t value, uint64_t alignment) {
    if (alignment == 0) {
        return value;
    }
    return value & ~(alignment - 1);
}

// Defined with the self-tests further down; declared here so the loader can
// time itself with the SAME primitive the benchmarks use rather than growing a
// second copy of rdtsc.
static inline uint64_t elf_tsc_serialized(void);

// ===========================================================================
// #633: the loader dereferences user virtual addresses at CPL 0 with CR3
// temporarily pointed at the target address space (segment copies, relocation
// fix-ups). With CR0.WP set, a supervisor write to a page that is not mapped
// WRITABLE is a #PF, and a #PF taken in that window is a KERNEL page fault:
// the process cannot be blamed, so it panics. That is how a Ring 3 launch of a
// malformed ELF took down Ring 0.
//
// Returns true only if EVERY page of [va, va+len) is present, user-accessible
// AND writable in the address space rooted at pml4_phys. vmm_get_effective_flags_in
// (not vmm_get_physical_in) is used deliberately: it ANDs U/S and R/W across
// all four levels, which is the only computation that matches what the CPU
// will do when the write actually happens.
// ===========================================================================
static bool elf_user_range_writable(uint64_t pml4_phys, uint64_t va, uint64_t len) {
    if (len == 0) {
        return true;
    }
    if (va + len < va) {            // wrap
        return false;
    }
    const uint64_t need = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE;
    uint64_t page = align_down(va, ELF_PAGE_SIZE);
    uint64_t last = align_down(va + len - 1, ELF_PAGE_SIZE);
    for (;;) {
        if ((vmm_get_effective_flags_in(pml4_phys, page) & need) != need) {
            return false;
        }
        if (page == last) {
            break;
        }
        page += ELF_PAGE_SIZE;
    }
    return true;
}

// ============================================================================
// Dynamic Relocation Engine (#427 FAKE-audit fix)
//
// The previous "PIE" path only ever added a flat relocation_offset to each
// PT_LOAD segment's vaddr and to the entry point. It never looked at
// PT_DYNAMIC and applied zero actual relocations. That happened to boot
// because every PIE binary built so far stores no absolute pointers that
// need runtime fixups (or got lucky with RIP-relative codegen for the few
// paths actually exercised); any real -fPIC/-pie object with a global
// function-pointer table, vtable, or string-pointer array would silently
// read garbage/zero instead of a fixed-up address. This blocks real
// dlopen()-style loading and CPython C extensions (#359), which both rely on
// R_X86_64_RELATIVE/GLOB_DAT/JUMP_SLOT fixups actually being applied.
//
// This section implements a real (if minimal) relocation engine: it walks
// PT_DYNAMIC, finds DT_RELA/DT_REL/DT_JMPREL, and applies each entry against
// either kernel-owned memory (elf_load/elf_load_full) or a foreign address
// space reached via a temporary CR3 switch (elf_load_user, the path actually
// used by proc_create_user() to launch real user processes).
// ============================================================================

/**
 * Translate a link-time (unrelocated) virtual address to a pointer into the
 * ELF file buffer, by finding the PT_LOAD segment that covers it. Used to
 * read the dynamic tables (.dynsym/.dynstr/.rela.dyn/.rela.plt), which are
 * addressed by their link-time vaddr in the DT_* entries.
 *
 * Returns NULL if the address/length isn't covered by a load segment's
 * file-backed region (e.g. it falls in the BSS tail, or is out of bounds).
 */
static const uint8_t *elf_vaddr_to_fileptr(const void *elf_data, uint32_t size,
                                            const Elf64_Ehdr *ehdr,
                                            uint64_t vaddr, uint64_t need_len) {
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = elf_get_phdr(elf_data, i);
        if (!phdr || phdr->p_type != PT_LOAD) {
            continue;
        }
        if (vaddr < phdr->p_vaddr) {
            continue;
        }
        uint64_t seg_off = vaddr - phdr->p_vaddr;
        if (seg_off >= phdr->p_filesz) {
            continue;   // not file-backed (e.g. lands in the BSS tail)
        }
        if (check_overflow_add(seg_off, need_len, phdr->p_filesz)) {
            continue;
        }
        uint64_t file_off = phdr->p_offset + seg_off;
        if (file_off > size || check_overflow_add(file_off, need_len, size)) {
            continue;
        }
        return (const uint8_t *)elf_data + file_off;
    }
    return NULL;
}

// Parsed PT_DYNAMIC tag values we care about (all link-time vaddrs/sizes).
typedef struct {
    uint64_t rela_off, rela_size, rela_ent, rela_count;
    uint64_t rel_off, rel_size, rel_ent;
    uint64_t jmprel_off, pltrelsz, pltrel_type;
    uint64_t symtab_off, strtab_off, syment;
} elf_dyn_tables_t;

static int elf_parse_dynamic(const void *elf_data, uint32_t size,
                              const Elf64_Phdr *dyn_phdr, elf_dyn_tables_t *out) {
    memset(out, 0, sizeof(*out));
    out->syment = sizeof(Elf64_Sym);

    if (dyn_phdr->p_offset > size ||
        check_overflow_add(dyn_phdr->p_offset, dyn_phdr->p_filesz, size)) {
        kprintf("[ELF] Error: PT_DYNAMIC out of bounds\n");
        return ELF_ERR_SEGMENT_OVERFLOW;
    }

    const Elf64_Dyn *dyn = (const Elf64_Dyn *)((const uint8_t *)elf_data + dyn_phdr->p_offset);
    uint64_t count = dyn_phdr->p_filesz / sizeof(Elf64_Dyn);

    for (uint64_t i = 0; i < count; i++) {
        int64_t tag = dyn[i].d_tag;
        if (tag == DT_NULL) {
            break;
        }
        uint64_t val = dyn[i].d_un.d_val;
        switch (tag) {
            case DT_RELA:       out->rela_off    = val; break;
            case DT_RELASZ:     out->rela_size   = val; break;
            case DT_RELAENT:    out->rela_ent    = val; break;
            case DT_RELACOUNT:  out->rela_count  = val; break;
            case DT_REL:        out->rel_off     = val; break;
            case DT_RELSZ:      out->rel_size    = val; break;
            case DT_RELENT:     out->rel_ent     = val; break;
            case DT_JMPREL:     out->jmprel_off  = val; break;
            case DT_PLTRELSZ:   out->pltrelsz    = val; break;
            case DT_PLTREL:     out->pltrel_type = val; break;
            case DT_SYMTAB:     out->symtab_off  = val; break;
            case DT_STRTAB:     out->strtab_off  = val; break;
            case DT_SYMENT:     out->syment      = val; break;
            default: break;
        }
    }

    return ELF_SUCCESS;
}

/**
 * Resolve a dynamic symbol table entry to its link-time value (S). Since this
 * loader has no dynamic linker / shared library resolution, only locally
 * DEFINED symbols (st_shndx != SHN_UNDEF) can be resolved; an undefined
 * (imported) symbol cannot be satisfied yet and is reported via *resolved.
 */
static uint64_t elf_resolve_symbol(const void *elf_data, uint32_t size,
                                    const Elf64_Ehdr *ehdr,
                                    const elf_dyn_tables_t *dyn,
                                    uint32_t sym_index, bool *resolved) {
    *resolved = false;
    if (dyn->symtab_off == 0) {
        return 0;
    }
    uint64_t syment = dyn->syment ? dyn->syment : sizeof(Elf64_Sym);

    const uint8_t *sym0 = elf_vaddr_to_fileptr(elf_data, size, ehdr, dyn->symtab_off, syment);
    if (!sym0) {
        return 0;
    }
    // Re-resolve at the indexed offset (bounds-checked independently so a
    // huge sym_index can't walk off the segment we validated sym0 against).
    uint64_t sym_vaddr = dyn->symtab_off + (uint64_t)sym_index * syment;
    const uint8_t *symp = elf_vaddr_to_fileptr(elf_data, size, ehdr, sym_vaddr, syment);
    if (!symp) {
        return 0;
    }

    const Elf64_Sym *sym = (const Elf64_Sym *)symp;
    if (sym->st_shndx == SHN_UNDEF) {
        return 0;   // external/imported symbol - no dynamic linker yet
    }

    *resolved = true;
    return sym->st_value;
}

// Where fixed-up values get written: either into a foreign user address
// space (via a temporary, interrupt-safe CR3 switch, same technique used for
// segment copies in elf_load_user), or directly into kernel-owned memory.
typedef struct {
    bool is_user;
    uint64_t pml4_phys;    // valid if is_user
    uint8_t *kernel_base;  // valid if !is_user: backing memory for the image
    uint64_t kernel_size;  // valid if !is_user: byte size of kernel_base (#633)
} elf_reloc_target_t;

// ===========================================================================
// #640 stage 2b: BATCHED relocation writes.
//
// These two writers used to do, PER 8-BYTE STORE: pushfq, cli, save CR3, load
// the target CR3, store, restore CR3, popfq. Writing CR3 flushes the entire
// TLB, so that is TWO FULL TLB FLUSHES PER RELOCATION.
//
// MEASURED (VM <vmid>, golden 998, kernel-only replacement): a PIE HEAPTEST with
// 743 relocations spent ~6.6M cycles in the relocation phase against ~2.1M for
// the same app built fixed-base with zero relocations, i.e. ~6,100 cycles to
// write eight bytes, 1,486 TLB flushes for one 53KB utility. The #633
// writability walk was suspected and exonerated: removing it entirely produced
// a number INSIDE the run-to-run spread of the unmodified build.
//
// The fix is the pattern the segment copier twenty lines away has always used:
// ONE interrupt-safe foreign-CR3 window around a BOUNDED batch of work, not one
// window per unit. Values are computed and destinations verified with the
// normal CR3 in place; then a single window applies up to ELF_RELOC_BATCH
// stores. Interrupt latency stays bounded by the batch size, exactly as the
// copier bounds it by the 64KB chunk.
//
// SAFETY OF REORDERING: batching would be wrong if one relocation's result were
// an input to another's. It is not. Every value comes from the FILE (r_addend,
// or for Elf64_Rel the implicit addend read from the file image via
// elf_vaddr_to_fileptr) plus the symbol table, never from the loaded image, so
// no entry reads a location another entry writes. Two entries targeting the
// same address still resolve in table order within a batch.
// ===========================================================================
// 64 entries * 24 bytes = 1.5KB of kernel stack. The binding constraint is NOT
// interrupt latency (64 stores is far shorter than the 64KB segment-copy window
// next door) but PROCESS_STACK_SIZE, which is 16KB: a 256-entry batch would be
// 6KB, over a third of the kernel stack, on a path that already has frames
// beneath it. 64 still removes 96% of the CR3 switches (743 relocations goes
// from 1,486 TLB flushes to 24), so the extra risk buys almost nothing.
#define ELF_RELOC_BATCH 64

typedef struct {
    uint64_t r_offset;   // link-time vaddr of the location to fix up
    uint64_t value;      // value to store
    bool     is32;       // 4-byte store instead of 8
} elf_reloc_pending_t;

// Apply a queued batch. Returns how many were actually written; the caller
// counts the remainder as skipped.
//
// #633 is preserved and, if anything, cheaper: the destination-writability walk
// happens HERE, before the CR3 window opens, so a page that is not
// present+user+writable in the target is dropped rather than turned into a
// kernel #PF with CR3 pointing at a foreign address space.
static uint32_t elf_reloc_flush(const elf_reloc_target_t *tgt,
                                elf_reloc_pending_t *batch, uint32_t n,
                                uint64_t relocation_offset, uint64_t low_addr) {
    if (n == 0) {
        return 0;
    }

    if (!tgt->is_user) {
        // Kernel-owned buffer: no address space to switch to, so there was
        // never a per-write cost here. Bounds-check and store.
        uint32_t done = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t r_offset = batch[i].r_offset;
            uint64_t width = batch[i].is32 ? sizeof(uint32_t) : sizeof(uint64_t);
            uint64_t dest_off = r_offset - low_addr;
            if (r_offset < low_addr || dest_off + width > tgt->kernel_size) {
                kprintf("[ELF] Warning: relocation offset 0x%llX past the %llu-byte "
                        "load buffer; skipping\n", r_offset, tgt->kernel_size);
                continue;
            }
            if (batch[i].is32) {
                *(uint32_t *)(tgt->kernel_base + dest_off) = (uint32_t)batch[i].value;
            } else {
                *(uint64_t *)(tgt->kernel_base + dest_off) = batch[i].value;
            }
            done++;
        }
        return done;
    }

    // Phase 1, normal CR3: verify every destination and compact the survivors
    // to the front of the batch, so the interrupts-off window below contains
    // stores and nothing else.
    uint32_t good = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t dest_vaddr = batch[i].r_offset + relocation_offset;
        uint64_t width = batch[i].is32 ? sizeof(uint32_t) : sizeof(uint64_t);
        if (!elf_user_range_writable(tgt->pml4_phys, dest_vaddr, width)) {
            kprintf("[ELF] Warning: relocation target 0x%llX is not mapped "
                    "writable in the target address space; skipping\n", dest_vaddr);
            continue;
        }
        batch[good++] = batch[i];
    }
    if (good == 0) {
        return 0;
    }

    // Phase 2: ONE foreign-CR3 window for the whole batch.
    {
        uint64_t old_cr3, rflags;
        __asm__ volatile("pushfq; pop %0" : "=r"(rflags) :: "memory");
        __asm__ volatile("cli");
        __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(tgt->pml4_phys) : "memory");

        // #19/#645: every store below lands on a USER page (U/S=1), so with
        // CR4.SMAP set each one is a #PF without AC. The bracket is around the
        // STORE LOOP and nothing else: phase 1 above already did the walking and
        // validation under the normal CR3, so this window is stores only.
        uaccess_ac_t __ac = uaccess_begin();
        for (uint32_t i = 0; i < good; i++) {
            uint64_t dest_vaddr = batch[i].r_offset + relocation_offset;
            if (batch[i].is32) {
                *(volatile uint32_t *)dest_vaddr = (uint32_t)batch[i].value;
            } else {
                *(volatile uint64_t *)dest_vaddr = batch[i].value;
            }
        }
        uaccess_end(__ac);

        __asm__ volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
        __asm__ volatile("push %0; popfq" : : "r"(rflags) : "cc", "memory");
    }
    return good;
}

/**
 * Apply every entry of one relocation table (DT_RELA/DT_REL/DT_JMPREL).
 *
 * table_off/table_size/entsize are as read from PT_DYNAMIC (link-time vaddr
 * + byte size). is_rela selects Elf64_Rela (explicit addend) vs Elf64_Rel
 * (implicit addend read from the original file bytes at r_offset).
 *
 * bounds_low/bounds_high restrict which r_offset values are accepted (the
 * aggregate PT_LOAD vaddr range computed by calculate_load_bounds) so a
 * corrupt/hostile relocation entry can't be used to write outside the
 * image's own memory.
 */
static void elf_apply_reloc_table(const void *elf_data, uint32_t size, const Elf64_Ehdr *ehdr,
                                   const elf_dyn_tables_t *dyn,
                                   uint64_t table_off, uint64_t table_size, uint64_t entsize,
                                   bool is_rela,
                                   uint64_t relocation_offset, uint64_t low_addr, uint64_t high_addr,
                                   const elf_reloc_target_t *tgt,
                                   uint32_t *applied_count, uint32_t *skipped_count) {
    if (table_off == 0 || table_size == 0) {
        return;
    }

    uint64_t default_ent = is_rela ? sizeof(Elf64_Rela) : sizeof(Elf64_Rel);
    uint64_t ent = entsize ? entsize : default_ent;
    if (ent == 0) {
        return;
    }
    uint64_t count = table_size / ent;

    // On the stack, not static: this function is re-entered per relocation
    // table (DT_RELA, DT_REL, DT_JMPREL). See ELF_RELOC_BATCH for the size.
    elf_reloc_pending_t batch[ELF_RELOC_BATCH];
    uint32_t nbatch = 0;

    const uint8_t *table = elf_vaddr_to_fileptr(elf_data, size, ehdr, table_off, table_size);
    if (!table) {
        kprintf("[ELF] Warning: relocation table at 0x%llX (%llu bytes) not "
                "resolvable to a file offset; skipping %llu entries\n",
                table_off, table_size, count);
        if (skipped_count) *skipped_count += (uint32_t)count;
        return;
    }

    for (uint64_t i = 0; i < count; i++) {
        uint64_t r_offset, r_info;
        int64_t r_addend;

        if (is_rela) {
            const Elf64_Rela *r = (const Elf64_Rela *)(table + i * ent);
            r_offset = r->r_offset;
            r_info = r->r_info;
            r_addend = r->r_addend;
        } else {
            const Elf64_Rel *r = (const Elf64_Rel *)(table + i * ent);
            r_offset = r->r_offset;
            r_info = r->r_info;
            // Elf64_Rel has no addend field; the ABI convention is that the
            // addend is whatever was already stored at the relocated
            // location in the file (implicit addend).
            const uint8_t *addend_src = elf_vaddr_to_fileptr(elf_data, size, ehdr,
                                                               r_offset, sizeof(uint64_t));
            r_addend = addend_src ? (int64_t)(*(const uint64_t *)addend_src) : 0;
        }

        // Reject anything outside this image's own load range: defends the
        // loader against a corrupt or hostile r_offset writing outside the
        // memory we allocated/mapped for it.
        if (r_offset < low_addr || r_offset >= high_addr) {
            kprintf("[ELF] Warning: relocation r_offset=0x%llX outside load "
                    "range [0x%llX, 0x%llX); skipping\n", r_offset, low_addr, high_addr);
            if (skipped_count) (*skipped_count)++;
            continue;
        }

        uint32_t type = (uint32_t)ELF64_R_TYPE(r_info);
        uint32_t symidx = (uint32_t)ELF64_R_SYM(r_info);
        uint64_t value = 0;
        bool have_value = false;
        bool is_32bit = false;

        switch (type) {
            case R_X86_64_RELATIVE:
                // B + A: pure base-relocation, no symbol lookup needed.
                value = relocation_offset + (uint64_t)r_addend;
                have_value = true;
                break;

            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT: {
                bool resolved;
                uint64_t s = elf_resolve_symbol(elf_data, size, ehdr, dyn, symidx, &resolved);
                if (resolved) {
                    value = s + relocation_offset;
                    have_value = true;
                } else {
                    kprintf("[ELF] Warning: unresolved symbol %u for %s "
                            "(external symbols not supported)\n", symidx,
                            type == R_X86_64_GLOB_DAT ? "GLOB_DAT" : "JUMP_SLOT");
                }
                break;
            }

            case R_X86_64_64: {
                bool resolved;
                uint64_t s = elf_resolve_symbol(elf_data, size, ehdr, dyn, symidx, &resolved);
                if (resolved) {
                    value = s + relocation_offset + (uint64_t)r_addend;
                    have_value = true;
                } else {
                    kprintf("[ELF] Warning: unresolved symbol %u for R_X86_64_64\n", symidx);
                }
                break;
            }

            case R_X86_64_DTPMOD64:
                // Single-module simplification: this loader doesn't maintain
                // a real TLS module registry, so every object is "module 1".
                value = 1;
                have_value = true;
                break;

            case R_X86_64_DTPOFF64: {
                bool resolved;
                uint64_t s = elf_resolve_symbol(elf_data, size, ehdr, dyn, symidx, &resolved);
                value = (resolved ? s : 0) + (uint64_t)r_addend;
                have_value = true;
                break;
            }

            case R_X86_64_TPOFF64:
                // Needs a real per-thread TLS block (%fs base) that this
                // loader does not set up yet; applying a wrong value would be
                // worse than leaving it, so just report it.
                kprintf("[ELF] Warning: R_X86_64_TPOFF64 seen (symbol %u) but "
                        "this loader has no TLS block yet; skipping\n", symidx);
                break;

            case R_X86_64_PC32:
            case R_X86_64_PLT32: {
                bool resolved;
                uint64_t s = elf_resolve_symbol(elf_data, size, ehdr, dyn, symidx, &resolved);
                if (resolved) {
                    uint64_t p = r_offset + relocation_offset;
                    uint64_t s_runtime = s + relocation_offset;
                    value = (uint32_t)(s_runtime + (uint64_t)r_addend - p);
                    have_value = true;
                    is_32bit = true;
                } else {
                    kprintf("[ELF] Warning: unresolved symbol %u for PC32/PLT32\n", symidx);
                }
                break;
            }

            case R_X86_64_32:
            case R_X86_64_32S: {
                bool resolved;
                uint64_t s = elf_resolve_symbol(elf_data, size, ehdr, dyn, symidx, &resolved);
                if (resolved) {
                    value = (uint32_t)(s + relocation_offset + (uint64_t)r_addend);
                    have_value = true;
                    is_32bit = true;
                } else {
                    kprintf("[ELF] Warning: unresolved symbol %u for R_X86_64_32/32S\n", symidx);
                }
                break;
            }

            case R_X86_64_NONE:
                break;

            default:
                kprintf("[ELF] Warning: unsupported relocation type %u at "
                        "r_offset=0x%llX; skipping\n", type, r_offset);
                break;
        }

        if (!have_value) {
            if (skipped_count) (*skipped_count)++;
            continue;
        }

        // #633: the [low_addr, high_addr) test above accepts an r_offset in the
        // LAST bytes of the image, where an 8-byte store still runs past the
        // end. Re-check with the actual access width now that it is known.
        uint64_t acc = is_32bit ? sizeof(uint32_t) : sizeof(uint64_t);
        if (r_offset + acc > high_addr) {
            kprintf("[ELF] Warning: %llu-byte relocation at r_offset=0x%llX "
                    "straddles the end of the load range 0x%llX; skipping\n",
                    acc, r_offset, high_addr);
            if (skipped_count) (*skipped_count)++;
            continue;
        }

        // #640 stage 2b: queue instead of writing. The batch is flushed inside
        // ONE foreign-CR3 window rather than one window per store.
        batch[nbatch].r_offset = r_offset;
        batch[nbatch].value    = value;
        batch[nbatch].is32     = is_32bit;
        nbatch++;
        if (nbatch == ELF_RELOC_BATCH) {
            uint32_t w = elf_reloc_flush(tgt, batch, nbatch, relocation_offset, low_addr);
            if (applied_count) (*applied_count) += w;
            if (skipped_count) (*skipped_count) += (nbatch - w);
            nbatch = 0;
        }
    }

    // Flush the remainder.
    if (nbatch) {
        uint32_t w = elf_reloc_flush(tgt, batch, nbatch, relocation_offset, low_addr);
        if (applied_count) (*applied_count) += w;
        if (skipped_count) (*skipped_count) += (nbatch - w);
        nbatch = 0;
    }
}

/**
 * Find PT_DYNAMIC (if any) and apply all of DT_RELA / DT_REL / DT_JMPREL
 * against the already-loaded image. Safe to call on a static (non-dynamic)
 * ELF: it just finds no PT_DYNAMIC and does nothing.
 */
static void elf_apply_all_relocations(const void *elf_data, uint32_t size, const Elf64_Ehdr *ehdr,
                                       uint64_t relocation_offset, uint64_t low_addr, uint64_t high_addr,
                                       const elf_reloc_target_t *tgt) {
    const Elf64_Phdr *dyn_phdr = NULL;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *p = elf_get_phdr(elf_data, i);
        if (p && p->p_type == PT_DYNAMIC) {
            dyn_phdr = p;
            break;
        }
    }
    if (!dyn_phdr) {
        kprintf("[ELF] No PT_DYNAMIC segment; nothing to relocate\n");
        return;
    }

    elf_dyn_tables_t dyn;
    if (elf_parse_dynamic(elf_data, size, dyn_phdr, &dyn) != ELF_SUCCESS) {
        kprintf("[ELF] Warning: failed to parse PT_DYNAMIC; skipping relocations\n");
        return;
    }

    uint32_t applied = 0, skipped = 0;

    if (dyn.rela_off) {
        kprintf("[ELF] Applying DT_RELA: %llu bytes @ link-vaddr 0x%llX "
                "(entsize=%llu, RELACOUNT hint=%llu)\n",
                dyn.rela_size, dyn.rela_off, dyn.rela_ent, dyn.rela_count);
        elf_apply_reloc_table(elf_data, size, ehdr, &dyn, dyn.rela_off, dyn.rela_size,
                               dyn.rela_ent, true, relocation_offset, low_addr, high_addr,
                               tgt, &applied, &skipped);
    }

    if (dyn.rel_off) {
        kprintf("[ELF] Applying DT_REL: %llu bytes @ link-vaddr 0x%llX (entsize=%llu)\n",
                dyn.rel_size, dyn.rel_off, dyn.rel_ent);
        elf_apply_reloc_table(elf_data, size, ehdr, &dyn, dyn.rel_off, dyn.rel_size,
                               dyn.rel_ent, false, relocation_offset, low_addr, high_addr,
                               tgt, &applied, &skipped);
    }

    if (dyn.jmprel_off) {
        bool jmp_is_rela = (dyn.pltrel_type != DT_REL);   // x86-64 ABI mandates RELA
        kprintf("[ELF] Applying DT_JMPREL (PLT): %llu bytes @ link-vaddr 0x%llX\n",
                dyn.pltrelsz, dyn.jmprel_off);
        elf_apply_reloc_table(elf_data, size, ehdr, &dyn, dyn.jmprel_off, dyn.pltrelsz,
                               jmp_is_rela ? sizeof(Elf64_Rela) : sizeof(Elf64_Rel),
                               jmp_is_rela, relocation_offset, low_addr, high_addr,
                               tgt, &applied, &skipped);
    }

    kprintf("[ELF] Relocations: %u applied, %u skipped/unresolved\n", applied, skipped);
}

// ============================================================================
// ELF Validation
// ============================================================================

// FFI lock: ElfValidated (rustkern.rs) mirrors this byte-for-byte.
_Static_assert(sizeof(elf_validated_t) == 40, "elf_validated_t must be 40 bytes for the Rust FFI");

// ============================================================================
// #404 / #499 Phase Q: pre-map validation seam (elf_validate_full_c / _rs).
//
// elf_validate_full_c is the VERBATIM reference: the ehdr + phdr-table checks of
// the original elf_validate() (same order + same ELF_ERR_* codes) followed by
// the PT_LOAD file-bounds walk lifted verbatim from calculate_load_bounds(). It
// is PURE (no map/alloc/CR3/copy) and SILENT (no kprintf) so it can run in the
// boot differential without flooding serial. It HONESTLY retains all three of
// the C loader's gaps on the untrusted header:
//   (1) the underflowing check_overflow_add(p_offset, p_filesz, size) - an
//       oversized p_filesz (> size) passes, and elf_load()'s memcpy then OOBs;
//   (2) no e_phentsize >= sizeof(Elf64_Phdr) check - elf_get_phdr() over-reads
//       when e_phentsize < 56;
//   (3) no p_memsz >= p_filesz check - the segment copy can write past memsz.
// elf_validate_full_rs (rustkern.rs) is the same validation with those three
// gaps CLOSED by construction (slice of exactly `size` bytes, no-underflow
// bounds math, phentsize/memsz gates). See rustkern.rs for the full write-up.
// ============================================================================
int elf_validate_full_c(const void *elf_data, uint32_t size, elf_validated_t *out) {
    if (out) {
        out->e_type = 0; out->e_machine = 0; out->e_phnum = 0; out->e_phentsize = 0;
        out->n_load = 0; out->e_entry = 0; out->e_phoff = 0; out->first_load_vaddr = 0;
    }
    if (elf_data == NULL) return ELF_ERR_NULL_PTR;
    if (size < sizeof(Elf64_Ehdr)) return ELF_ERR_TOO_SMALL;

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)elf_data;

    if (ehdr->e_ident[EI_MAG0] != ELF_MAGIC_0 || ehdr->e_ident[EI_MAG1] != ELF_MAGIC_1 ||
        ehdr->e_ident[EI_MAG2] != ELF_MAGIC_2 || ehdr->e_ident[EI_MAG3] != ELF_MAGIC_3)
        return ELF_ERR_BAD_MAGIC;
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) return ELF_ERR_NOT_64BIT;
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) return ELF_ERR_NOT_LE;
    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT || ehdr->e_version != EV_CURRENT)
        return ELF_ERR_BAD_VERSION;
    if (ehdr->e_machine != EM_X86_64) return ELF_ERR_NOT_X86_64;
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) return ELF_ERR_NOT_EXEC;
    if (ehdr->e_phnum == 0) return ELF_ERR_NO_PHDR;

    uint64_t phdr_end = ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize;
    if (ehdr->e_phoff > size || phdr_end > size) return ELF_ERR_PHDR_OVERFLOW;

    // PT_LOAD file-bounds walk (verbatim calculate_load_bounds bounds logic,
    // incl. the underflowing check_overflow_add and elf_get_phdr's 56-byte read).
    // check_overflow_add_verbatim is the ORIGINAL underflowing helper, used here
    // ON PURPOSE: this function is the reference and must keep reproducing the
    // pre-port verdicts (gap (1)) even though the LOADER path is now hardened
    // under #489. Do NOT "fix" this call site; fixing it would silently turn the
    // rollback reference into a different function from the one it references.
    uint32_t nload = 0; uint64_t first_v = 0; bool found = false;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = elf_get_phdr(elf_data, i);
        if (phdr == NULL || phdr->p_type != PT_LOAD) continue;
        if (phdr->p_offset > size ||
            check_overflow_add_verbatim(phdr->p_offset, phdr->p_filesz, size)) {
            return ELF_ERR_SEGMENT_OVERFLOW;
        }
        if (!found) { first_v = phdr->p_vaddr; found = true; }
        nload++;
    }
    if (!found) return ELF_ERR_NO_PHDR;

    if (out) {
        out->e_type = ehdr->e_type; out->e_machine = ehdr->e_machine;
        out->e_phnum = ehdr->e_phnum; out->e_phentsize = ehdr->e_phentsize;
        out->n_load = nload; out->e_entry = ehdr->e_entry;
        out->e_phoff = ehdr->e_phoff; out->first_load_vaddr = first_v;
    }
    return ELF_SUCCESS;
}

// Live dispatcher. Keeps the original int(void*,uint32_t) signature every caller
// uses. With -DRUST_ELF (set in the Makefile) the untrusted-image validation
// runs in Rust (elf_validate_full_rs, rustkern.rs) - which additionally CONFINES
// the three OOB classes above before elf_load() maps/copies anything. A one-line
// summary is logged to preserve the original diagnostics without the walk
// flooding serial.
//
// READ THIS BEFORE DROPPING -DRUST_ELF (#404 3-way drift audit, 2026-07-16).
// Dropping the flag is NOT a behavior rollback to the pre-port kernel. It is a
// rollback to elf_validate_full_c, which differs from the pre-b808 C in ONE
// deliberate way and is NOT a bug:
//
//   * CONTRACT CHANGE (deliberate, fail-closed, reaches 8 callers). The Phase Q
//     merge moved the PT_LOAD file-bounds check and the "no PT_LOAD" check out of
//     calculate_load_bounds() and INTO elf_validate(). Both _c and _rs carry them,
//     so BOTH flag states are stricter than the pre-b808 elf_validate(). 48,130 of
//     438,747 vectors change verdict, all in the reject direction (0 the other
//     way). Fully documented at elf_validate()'s declaration in elf.h, where the
//     callers can actually see it. The flag does not restore the old contract; it
//     only chooses which implementation enforces the new one.
//
// What dropping the flag DOES still restore is the three OOB GAPS in the
// reference (the underflowing bounds math via check_overflow_add_verbatim, no
// e_phentsize gate, no p_memsz gate). That used to make the C path unsound: an
// image with p_filesz > size was accepted here and elf_load()'s copy loop then
// read past EOF (advisory 0003). As of #489 that is no longer reachable: the
// LOADER path (calculate_load_bounds/elf_vaddr_to_fileptr/elf_parse_dynamic) uses
// the hardened check_overflow_add and rejects the image before any copy, so the C
// fallback is sound on its own even though this reference still accepts. The
// reference keeps the gaps on purpose so the differential can still demonstrate
// what the Rust confines.
int elf_validate(const void *elf_data, uint32_t size) {
    // #489 HARDENING (2026-07-26): close the reachable OOB in the -DRUST_ELF-off
    // (C fallback) path BEFORE dispatching. elf_validate_full_c is DELIBERATELY
    // kept as the verbatim differential reference (it retains the three header
    // gaps on purpose - see its banner), so the fix lives HERE in the shared
    // dispatcher. This gate rejects an undersized e_phentsize, which otherwise
    // makes elf_get_phdr() (called by both elf_validate_full_c AND the loader's
    // calculate_load_bounds/elf_load) read a full 56-byte program header past the
    // validated table and past EOF (reachable OOB read; matches
    // elf_validate_full_rs confinement (2)). Gated on a plausible ELF64 header so
    // non-ELF / wrong-class inputs still get their exact ELF_ERR_* code below. On
    // every real ELF (e_phentsize == 56) this is a no-op. Confinements (1)
    // oversized p_filesz and (3) p_memsz < p_filesz are enforced in
    // calculate_load_bounds() before any allocation/copy (see there).
    if (elf_data != NULL && size >= sizeof(Elf64_Ehdr)) {
        const Elf64_Ehdr *eh = (const Elf64_Ehdr *)elf_data;
        if (eh->e_ident[EI_MAG0] == ELF_MAGIC_0 && eh->e_ident[EI_MAG1] == ELF_MAGIC_1 &&
            eh->e_ident[EI_MAG2] == ELF_MAGIC_2 && eh->e_ident[EI_MAG3] == ELF_MAGIC_3 &&
            eh->e_ident[EI_CLASS] == ELFCLASS64 &&
            eh->e_phnum != 0 && (size_t)eh->e_phentsize < sizeof(Elf64_Phdr)) {
            kprintf("[ELF] Validation failed: %s (%d)\n",
                    elf_strerror(ELF_ERR_PHDR_OVERFLOW), ELF_ERR_PHDR_OVERFLOW);
            return ELF_ERR_PHDR_OVERFLOW;
        }
    }
    elf_validated_t v;
#ifdef RUST_ELF
    int r = elf_validate_full_rs((const uint8_t *)elf_data, (uint64_t)size, &v);
#else
    int r = elf_validate_full_c(elf_data, size, &v);
#endif
    if (r == ELF_SUCCESS) {
        kprintf("[ELF] Validation passed: %s, %d program headers, %u PT_LOAD\n",
                v.e_type == ET_EXEC ? "executable" : "PIE/shared",
                v.e_phnum, v.n_load);
    } else {
        kprintf("[ELF] Validation failed: %s (%d)\n", elf_strerror(r), r);
    }
    return r;
}

// ============================================================================
// ELF Loading
// ============================================================================

/**
 * Count the number of PT_LOAD segments
 */
static uint32_t count_load_segments(const void *elf_data) {
    const Elf64_Ehdr *ehdr = elf_get_header(elf_data);
    uint32_t count = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = elf_get_phdr(elf_data, i);
        if (phdr && phdr->p_type == PT_LOAD) {
            count++;
        }
    }

    return count;
}

/**
 * Calculate the total memory required for all PT_LOAD segments
 * Returns the lowest and highest virtual addresses needed
 */
static int calculate_load_bounds(const void *elf_data, uint32_t size,
                                  uint64_t *low_addr, uint64_t *high_addr) {
    const Elf64_Ehdr *ehdr = elf_get_header(elf_data);
    uint64_t lowest = UINT64_MAX;
    uint64_t highest = 0;
    bool found_load = false;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = elf_get_phdr(elf_data, i);
        if (phdr == NULL) {
            continue;
        }

        if (phdr->p_type != PT_LOAD) {
            continue;
        }

        // Validate segment bounds within file
        if (phdr->p_offset > size ||
            check_overflow_add(phdr->p_offset, phdr->p_filesz, size)) {
            kprintf("[ELF] Error: Segment %d file bounds overflow\n", i);
            return ELF_ERR_SEGMENT_OVERFLOW;
        }

        // #489 HARDENING (2026-07-26): reject p_memsz < p_filesz (matches
        // elf_validate_full_rs confinement (3)). The elf_load copy writes
        // p_filesz bytes into a region sized from p_memsz; an inverted pair is a
        // write-overflow past the load allocation. This runs before any alloc/copy
        // in elf_load/elf_load_full/elf_load_user. Real ELFs always have
        // p_memsz >= p_filesz, so this never fires on valid input.
        if (phdr->p_memsz < phdr->p_filesz) {
            kprintf("[ELF] Error: Segment %d memsz < filesz\n", i);
            return ELF_ERR_SEGMENT_OVERFLOW;
        }

        // Calculate aligned boundaries
        uint64_t seg_start = align_down(phdr->p_vaddr, ELF_PAGE_SIZE);
        uint64_t seg_end = align_up(phdr->p_vaddr + phdr->p_memsz, ELF_PAGE_SIZE);

        if (seg_start < lowest) {
            lowest = seg_start;
        }
        if (seg_end > highest) {
            highest = seg_end;
        }

        found_load = true;

        kprintf("[ELF]   Segment %d: vaddr=0x%llX, memsz=0x%llX, filesz=0x%llX, flags=%c%c%c\n",
                i, phdr->p_vaddr, phdr->p_memsz, phdr->p_filesz,
                (phdr->p_flags & PF_R) ? 'R' : '-',
                (phdr->p_flags & PF_W) ? 'W' : '-',
                (phdr->p_flags & PF_X) ? 'X' : '-');
    }

    if (!found_load) {
        kprintf("[ELF] Error: No PT_LOAD segments found\n");
        return ELF_ERR_NO_PHDR;
    }

    *low_addr = lowest;
    *high_addr = highest;

    kprintf("[ELF] Load bounds: 0x%llX - 0x%llX (size: 0x%llX)\n",
            lowest, highest, highest - lowest);

    return ELF_SUCCESS;
}

int elf_load(void *elf_data, uint32_t size, uint64_t *entry_point) {
    // Validate parameters
    if (entry_point == NULL) {
        kprintf("[ELF] Error: entry_point pointer is NULL\n");
        return ELF_ERR_NULL_PTR;
    }

    // Validate ELF file
    int result = elf_validate(elf_data, size);
    if (result != ELF_SUCCESS) {
        return result;
    }

    const Elf64_Ehdr *ehdr = elf_get_header(elf_data);
    bool is_pie = (ehdr->e_type == ET_DYN);

    kprintf("[ELF] Loading %s executable, entry=0x%llX\n",
            is_pie ? "PIE" : "static", ehdr->e_entry);

    // Calculate memory bounds for all PT_LOAD segments
    uint64_t low_addr, high_addr;
    result = calculate_load_bounds(elf_data, size, &low_addr, &high_addr);
    if (result != ELF_SUCCESS) {
        return result;
    }

    uint64_t total_size = high_addr - low_addr;

    // For PIE executables, we load at our chosen base address
    // For static executables, we load at the specified virtual addresses
    uint64_t load_base;
    uint64_t relocation_offset;

    if (is_pie) {
        // PIE: allocate memory and load at our chosen base
        load_base = PIE_DEFAULT_BASE;
        relocation_offset = load_base - low_addr;
        kprintf("[ELF] PIE relocation: base=0x%llX, offset=0x%llX\n",
                load_base, relocation_offset);
    } else {
        // Static executable: load at specified addresses
        load_base = low_addr;
        relocation_offset = 0;
    }

    // Allocate memory for loading
    // For a bare-metal kernel, we use kmalloc_aligned to get page-aligned memory
    void *load_mem = kmalloc_aligned(total_size, ELF_PAGE_SIZE);
    if (load_mem == NULL) {
        kprintf("[ELF] Error: Failed to allocate %llu bytes for loading\n", total_size);
        return ELF_ERR_ALLOC_FAILED;
    }

    kprintf("[ELF] Allocated memory at %p (size: 0x%llX)\n", load_mem, total_size);

    // Zero out the allocated memory (for BSS sections)
    memset(load_mem, 0, total_size);

    // Load each PT_LOAD segment
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = elf_get_phdr(elf_data, i);
        if (phdr == NULL || phdr->p_type != PT_LOAD) {
            continue;
        }

        // Calculate destination address in our allocated memory
        // For PIE: use relative offset from low_addr
        // For static: also use relative offset from low_addr (since we allocated from there)
        uint64_t dest_offset = phdr->p_vaddr - low_addr;
        uint8_t *dest = (uint8_t *)load_mem + dest_offset;

        // Copy file data (p_filesz bytes)
        if (phdr->p_filesz > 0) {
            const uint8_t *src = (const uint8_t *)elf_data + phdr->p_offset;
            memcpy(dest, src, phdr->p_filesz);
            kprintf("[ELF] Loaded segment %d: %llu bytes to offset 0x%llX\n",
                    i, phdr->p_filesz, dest_offset);
        }

        // Note: BSS (p_memsz - p_filesz) is already zeroed by memset above
        if (phdr->p_memsz > phdr->p_filesz) {
            kprintf("[ELF] Segment %d: BSS %llu bytes (already zeroed)\n",
                    i, phdr->p_memsz - phdr->p_filesz);
        }
    }

    // Apply PT_DYNAMIC relocations (R_X86_64_RELATIVE/GLOB_DAT/JUMP_SLOT/64/...)
    // against the kernel-owned buffer we just populated. #427 fix: this used
    // to be entirely skipped for PIE/dynamic images.
    {
        elf_reloc_target_t tgt = { .is_user = false, .pml4_phys = 0,
                                    .kernel_base = (uint8_t *)load_mem,
                                    .kernel_size = total_size };
        elf_apply_all_relocations(elf_data, size, ehdr, relocation_offset, low_addr, high_addr, &tgt);
    }

    // Calculate the entry point
    // For PIE: add relocation offset
    // For static: use as-is (but our memory is at load_mem)
    if (is_pie) {
        *entry_point = ehdr->e_entry + relocation_offset;
    } else {
        // For static executables loaded at low_addr, calculate offset into load_mem
        *entry_point = (uint64_t)load_mem + (ehdr->e_entry - low_addr);
    }

    kprintf("[ELF] Load complete. Entry point: 0x%llX\n", *entry_point);

    return ELF_SUCCESS;
}

int elf_load_full(void *elf_data, uint32_t size, Elf64_LoadResult *result) {
    // Validate parameters
    if (result == NULL) {
        kprintf("[ELF] Error: result pointer is NULL\n");
        return ELF_ERR_NULL_PTR;
    }

    // Initialize result structure
    memset(result, 0, sizeof(Elf64_LoadResult));

    // Validate ELF file
    int ret = elf_validate(elf_data, size);
    if (ret != ELF_SUCCESS) {
        return ret;
    }

    const Elf64_Ehdr *ehdr = elf_get_header(elf_data);
    result->is_pie = (ehdr->e_type == ET_DYN);

    kprintf("[ELF] Loading %s executable (full), entry=0x%llX\n",
            result->is_pie ? "PIE" : "static", ehdr->e_entry);

    // Count PT_LOAD segments
    uint32_t load_count = count_load_segments(elf_data);
    if (load_count == 0) {
        kprintf("[ELF] Error: No PT_LOAD segments\n");
        return ELF_ERR_NO_PHDR;
    }

    // Allocate segment info array
    result->segments = (Elf64_LoadedSeg *)kmalloc(load_count * sizeof(Elf64_LoadedSeg));
    if (result->segments == NULL) {
        kprintf("[ELF] Error: Failed to allocate segment info array\n");
        return ELF_ERR_ALLOC_FAILED;
    }
    result->segment_count = load_count;

    // Calculate memory bounds
    uint64_t low_addr, high_addr;
    ret = calculate_load_bounds(elf_data, size, &low_addr, &high_addr);
    if (ret != ELF_SUCCESS) {
        kfree(result->segments);
        result->segments = NULL;
        return ret;
    }

    uint64_t total_size = high_addr - low_addr;
    result->load_size = total_size;

    // Determine load base and relocation
    uint64_t relocation_offset;
    if (result->is_pie) {
        result->base_addr = PIE_DEFAULT_BASE;
        relocation_offset = result->base_addr - low_addr;
    } else {
        result->base_addr = low_addr;
        relocation_offset = 0;
    }

    // Allocate memory for loading
    result->load_base = kmalloc_aligned(total_size, ELF_PAGE_SIZE);
    if (result->load_base == NULL) {
        kprintf("[ELF] Error: Failed to allocate %llu bytes\n", total_size);
        kfree(result->segments);
        result->segments = NULL;
        return ELF_ERR_ALLOC_FAILED;
    }

    kprintf("[ELF] Allocated memory at %p (size: 0x%llX)\n",
            result->load_base, total_size);

    // Zero out allocated memory
    memset(result->load_base, 0, total_size);

    // Load segments and fill segment info
    uint32_t seg_idx = 0;
    for (uint16_t i = 0; i < ehdr->e_phnum && seg_idx < load_count; i++) {
        const Elf64_Phdr *phdr = elf_get_phdr(elf_data, i);
        if (phdr == NULL || phdr->p_type != PT_LOAD) {
            continue;
        }

        // Fill segment info
        result->segments[seg_idx].vaddr = phdr->p_vaddr + relocation_offset;
        result->segments[seg_idx].memsz = phdr->p_memsz;
        result->segments[seg_idx].flags = phdr->p_flags;

        // Calculate destination and copy data
        uint64_t dest_offset = phdr->p_vaddr - low_addr;
        uint8_t *dest = (uint8_t *)result->load_base + dest_offset;

        if (phdr->p_filesz > 0) {
            const uint8_t *src = (const uint8_t *)elf_data + phdr->p_offset;
            memcpy(dest, src, phdr->p_filesz);
        }

        kprintf("[ELF] Loaded segment %d: vaddr=0x%llX, memsz=0x%llX, flags=%c%c%c\n",
                seg_idx, result->segments[seg_idx].vaddr,
                result->segments[seg_idx].memsz,
                (phdr->p_flags & PF_R) ? 'R' : '-',
                (phdr->p_flags & PF_W) ? 'W' : '-',
                (phdr->p_flags & PF_X) ? 'X' : '-');

        seg_idx++;
    }

    // Apply PT_DYNAMIC relocations against the kernel-owned buffer. #427 fix:
    // this used to be entirely skipped for PIE/dynamic images.
    {
        elf_reloc_target_t tgt = { .is_user = false, .pml4_phys = 0,
                                    .kernel_base = (uint8_t *)result->load_base,
                                    .kernel_size = result->load_size };
        elf_apply_all_relocations(elf_data, size, ehdr, relocation_offset, low_addr, high_addr, &tgt);
    }

    // Calculate entry point
    if (result->is_pie) {
        result->entry_point = ehdr->e_entry + relocation_offset;
    } else {
        result->entry_point = (uint64_t)result->load_base + (ehdr->e_entry - low_addr);
    }

    kprintf("[ELF] Load complete. Entry point: 0x%llX, %d segments loaded\n",
            result->entry_point, result->segment_count);

    return ELF_SUCCESS;
}

void elf_unload(Elf64_LoadResult *result) {
    if (result == NULL) {
        return;
    }

    kprintf("[ELF] Unloading executable (base=%p, size=0x%llX)\n",
            result->load_base, result->load_size);

    if (result->segments != NULL) {
        kfree(result->segments);
        result->segments = NULL;
    }

    if (result->load_base != NULL) {
        kfree(result->load_base);
        result->load_base = NULL;
    }

    result->entry_point = 0;
    result->base_addr = 0;
    result->load_size = 0;
    result->segment_count = 0;
    result->is_pie = false;
}

// ============================================================================
// User-Space ELF Loading
// ============================================================================

// ===========================================================================
// #633: PRE-FLIGHT. Everything an untrusted image can say about where it wants
// to live is checked HERE, before a single page is allocated, mapped or
// written. Ring 3 chooses this data (any user can drop a file in /APPS and
// launch it), so a violation must produce an ELF_ERR_* the caller reports with
// the app's name, never a fault in a foreign-CR3 window.
//
// This is deliberately a separate, pure function: it can be reasoned about and
// tested without a live address space, and it runs to completion so the log
// names every problem, not just the first.
// ===========================================================================
static int elf_check_user_image(const void *elf_data, const Elf64_Ehdr *ehdr,
                                uint64_t reloc_off, uint64_t low_addr, uint64_t high_addr,
                                const char *name) {
    if (!name) name = "<unnamed>";
    if (high_addr <= low_addr) {
        kprintf("[ELF] Reject '%s': empty load range 0x%llX-0x%llX\n", name, low_addr, high_addr);
        return ELF_ERR_BAD_LOAD_ADDR;
    }
    if (high_addr - low_addr > ELF_USER_IMAGE_MAX_SPAN) {
        kprintf("[ELF] Reject '%s': image spans 0x%llX bytes, limit is 0x%llX\n",
                name, high_addr - low_addr, ELF_USER_IMAGE_MAX_SPAN);
        return ELF_ERR_BAD_LOAD_ADDR;
    }

    uint64_t base = low_addr + reloc_off;
    uint64_t end  = high_addr + reloc_off;
    if (end < base) {
        kprintf("[ELF] Reject '%s': load range wraps (base=0x%llX end=0x%llX)\n",
                name, base, end);
        return ELF_ERR_BAD_LOAD_ADDR;
    }

    // #640 stage 2: a PIE is relocated by the KERNEL into the dedicated
    // userland window, so its permitted range is that window, not the legacy
    // one. A fixed-base ET_EXEC keeps the legacy window until the fleet is
    // converted (stage 3 then rejects a declared base in there outright).
    bool is_dyn = (ehdr->e_type == ET_DYN);
    uint64_t win_min = is_dyn ? ELF_USER_WIN_MIN : ELF_USER_IMAGE_MIN;
    uint64_t win_max = is_dyn ? ELF_USER_WIN_MAX : ELF_USER_IMAGE_MAX;

    // THE #633 CHECK. A fixed-base ET_EXEC at 0x400000 (an app linked without
    // -T user.ld) lands here. Previously it passed, because the only test was
    // against USER_SPACE_START/END, i.e. the entire 128TB lower half.
    if (base < win_min || end > win_max) {
        kprintf("[ELF] Reject '%s': load range 0x%llX-0x%llX is outside the "
                "permitted %s window 0x%llX-0x%llX. A fixed-base user binary "
                "must be linked with -T user.ld (without it ld defaults to "
                "0x400000, which is kernel-mapped in every address space); a "
                "PIE is placed by the kernel and declares no base.\n",
                name, base, end, is_dyn ? "PIE" : "legacy fixed-base",
                win_min, win_max);
        return ELF_ERR_BAD_LOAD_ADDR;
    }

    // Per-segment: no arithmetic wrap, and every segment inside the aggregate
    // range calculate_load_bounds reported (which is what the relocation
    // engine bounds-checks against, so a segment outside it would be a hole in
    // that guarantee).
    int rc = ELF_SUCCESS;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = elf_get_phdr(elf_data, i);
        if (phdr == NULL || phdr->p_type != PT_LOAD) {
            continue;
        }
        uint64_t v = phdr->p_vaddr + reloc_off;
        if (v < phdr->p_vaddr || v + phdr->p_memsz < v) {
            kprintf("[ELF] Reject '%s': segment %d vaddr/memsz wraps\n", name, i);
            rc = ELF_ERR_BAD_LOAD_ADDR;
            continue;
        }
        if (phdr->p_vaddr < low_addr || phdr->p_vaddr + phdr->p_memsz > high_addr) {
            kprintf("[ELF] Reject '%s': segment %d [0x%llX,0x%llX) outside computed "
                    "bounds [0x%llX,0x%llX)\n", name, i, phdr->p_vaddr,
                    phdr->p_vaddr + phdr->p_memsz, low_addr, high_addr);
            rc = ELF_ERR_BAD_LOAD_ADDR;
        }
    }
    if (rc != ELF_SUCCESS) {
        return rc;
    }

    // The entry point must land in a segment we are actually going to map and
    // that is executable. A bad entry only kills the process (Ring 3 fault, not
    // a panic), but rejecting it here turns a mystery crash into a message.
    uint64_t entry = ehdr->e_entry;
    bool entry_ok = false;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = elf_get_phdr(elf_data, i);
        if (phdr == NULL || phdr->p_type != PT_LOAD || !(phdr->p_flags & PF_X)) {
            continue;
        }
        if (entry >= phdr->p_vaddr && entry < phdr->p_vaddr + phdr->p_memsz) {
            entry_ok = true;
            break;
        }
    }
    if (!entry_ok) {
        kprintf("[ELF] Reject '%s': entry 0x%llX is not inside any executable "
                "PT_LOAD segment\n", name, entry);
        return ELF_ERR_BAD_LOAD_ADDR;
    }

    return ELF_SUCCESS;
}

// #640: the name is for DIAGNOSTICS ONLY and may be NULL. It exists because a
// rejection that says "load range 0x400000-0x40F000 is outside the permitted
// window" is a puzzle, and one that says which app it was is a work item. The
// original 6-argument form is kept for the callers (and self-tests) that have
// no name to give.
int elf_load_user(void *elf_data, uint32_t size, uint64_t pml4_phys,
                  uint64_t *entry_point, uint64_t *load_base, uint64_t *load_end) {
    return elf_load_user_named(elf_data, size, pml4_phys, entry_point, load_base,
                               load_end, NULL);
}

int elf_load_user_named(void *elf_data, uint32_t size, uint64_t pml4_phys,
                        uint64_t *entry_point, uint64_t *load_base, uint64_t *load_end,
                        const char *name) {
    // #640: cost instrumentation. #633 made every relocation write verify its
    // destination with a four-level page walk, and a PIE has far more
    // relocations than the zero every ET_EXEC in this tree has, so the cost of
    // the migration has to be a measurement and not an opinion. Cycles, not
    // microseconds: there is no calibrated TSC frequency here, and cycles are
    // exactly comparable between two boots of the same VM, which is the only
    // comparison being made.
    uint64_t t_enter = elf_tsc_serialized();
    uint64_t t_reloc = 0;

    // Validate parameters
    if (entry_point == NULL || load_base == NULL || load_end == NULL) {
        kprintf("[ELF] Error: output pointer is NULL\n");
        return ELF_ERR_NULL_PTR;
    }

    if (pml4_phys == 0) {
        kprintf("[ELF] Error: invalid PML4 address\n");
        return ELF_ERR_NULL_PTR;
    }

    // Validate ELF file
    int result = elf_validate(elf_data, size);
    if (result != ELF_SUCCESS) {
        return result;
    }

    const Elf64_Ehdr *ehdr = elf_get_header(elf_data);
    bool is_pie = (ehdr->e_type == ET_DYN);

    kprintf("[ELF] Loading %s executable to user space, entry=0x%llX\n",
            is_pie ? "PIE" : "static", ehdr->e_entry);

    // Calculate memory bounds for all PT_LOAD segments
    uint64_t low_addr, high_addr;
    result = calculate_load_bounds(elf_data, size, &low_addr, &high_addr);
    if (result != ELF_SUCCESS) {
        return result;
    }

    // For PIE executables, relocate to a real per-object base.
    // For static executables, use their specified addresses.
    //
    // #427 fix note: this used to relocate to USER_SPACE_START (0x400000 /
    // 4MB). That address is inside the range the deep-copied PML4[0] still
    // carries pre-existing (kernel, read-only) mappings for low memory - a
    // fresh vmm_alloc_user_pages() there does not actually get a clean,
    // writable page, and the very first write into the segment takes a
    // supervisor-write page fault (CR2 in the 0xBFC0xxxx range: the same
    // failure signature already documented above for raw-physical-address
    // segment copies). PIE_USER_BASE instead reuses the 2-3GB PDPT[2] window
    // that vmm_create_user_space() explicitly clears for user mappings and
    // that every existing (non-PIE, fixed 0x80000000) app already loads into
    // successfully - just at a distinct offset (0x90000000) so a PIE image's
    // layout doesn't coincide with the fixed-base apps' addresses, matching
    // the "real per-object base" this loader owes the dynamic linking path
    // (dlopen / CPython C-extensions, #359).
    uint64_t relocation_offset = 0;
    uint64_t actual_base = low_addr;

    if (is_pie) {
        // #640 stage 3: ASLR. The image slot runs from USER_WIN_IMAGE_BASE up to
        // USER_WIN_HEAP_BASE (mm/vmm.h) = 1GB of room. Randomize the base within
        // it at 2MB granularity: 2MB alignment keeps the mapping huge-page
        // friendly, and 1GB/2MB = up to 512 slots (~9 bits). Modest, but it is
        // the honest maximum this layout allows without moving the heap arena.
        //
        // The image must still fit, so the slot count is derived from the ACTUAL
        // image span: a large image gets fewer slots, and an image that fills the
        // window gets slot 0 and loads exactly where it did before ASLR.
        actual_base = PIE_USER_BASE;

        uint64_t span      = (high_addr > low_addr) ? (high_addr - low_addr) : 0;
        uint64_t slot_size = 0x200000ULL;                       // 2MB
        uint64_t room      = USER_WIN_HEAP_BASE - USER_WIN_IMAGE_BASE;
        // #646: this did not consult aslr_enabled(), so the one on/off switch
        // in the tree did not switch its one live consumer. A policy knob that
        // controls nothing is worse than no knob: it invites the belief that
        // randomisation was turned off when it was not.
        if (aslr_enabled() && span < room) {
            uint64_t slots = (room - span) / slot_size;
            if (slots > 1) {
                // aslr_get_random_range() does rejection sampling, so this is
                // free of the modulo bias a plain % would introduce.
                uint64_t slot = aslr_get_random_range(slots);
                actual_base = PIE_USER_BASE + slot * slot_size;
            }
        }

        relocation_offset = actual_base - low_addr;
        kprintf("[ELF] PIE relocation: base=0x%llX, offset=0x%llX (ASLR span=0x%llX)\n",
                actual_base, relocation_offset, span);
    }

    uint64_t actual_end = high_addr + relocation_offset;

    // #633: full pre-flight on the untrusted image BEFORE anything is mapped.
    // This replaces the old USER_SPACE_START/END test, which spanned the whole
    // 128TB lower half and therefore accepted a 0x400000-based ET_EXEC.
#ifndef ELF633_UNSAFE
    result = elf_check_user_image(elf_data, ehdr, relocation_offset, low_addr, high_addr, name);
    if (result != ELF_SUCCESS) {
        return result;
    }
#else
    (void)elf_check_user_image;   // -DELF633_UNSAFE: the "prove it can fail" build
#endif
    if (actual_base < USER_SPACE_START || actual_end > USER_SPACE_END) {
        kprintf("[ELF] Error: Load addresses 0x%llX-0x%llX outside user space\n",
                actual_base, actual_end);
        return ELF_ERR_LOAD_FAILED;
    }

    // Load each PT_LOAD segment
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = elf_get_phdr(elf_data, i);
        if (phdr == NULL || phdr->p_type != PT_LOAD) {
            continue;
        }

        // Calculate relocated virtual address
        uint64_t seg_vaddr = phdr->p_vaddr + relocation_offset;
        uint64_t seg_start = align_down(seg_vaddr, ELF_PAGE_SIZE);
        uint64_t seg_end = align_up(seg_vaddr + phdr->p_memsz, ELF_PAGE_SIZE);
        uint64_t num_pages = (seg_end - seg_start) / ELF_PAGE_SIZE;

        // #633: the load destination is ALWAYS mapped writable, whatever the
        // segment's PF_W says, because the loader itself writes the segment
        // contents through this mapping at CPL 0 with CR0.WP set. Mapping a
        // read-only PT_LOAD read-only and then memcpy'ing into it is a
        // supervisor write fault, i.e. a kernel panic. That is precisely what
        // an app linked without -T user.ld produced: its first PT_LOAD is
        // R-only (MEASURED on hello: LOAD 0x400000 flags "R").
        //
        // This is NOT a W^X regression: the mapping was already RWX for every
        // real app (user.ld forces a single PF_R|PF_W|PF_X PT_LOAD, and NX has
        // never been set here). #526 owns making segments read-only/NX AFTER
        // the copy completes, which is the only order that can work.
#if defined(ELF633_UNSAFE) || defined(ELF633_UNSAFE_WP)
        uint64_t vmm_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        if (phdr->p_flags & PF_W) vmm_flags |= VMM_FLAG_WRITABLE;
#else
        uint64_t vmm_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITABLE;
#endif

        kprintf("[ELF] Segment %d: vaddr=0x%llX, pages=%llu, flags=0x%llX\n",
                i, seg_vaddr, num_pages, vmm_flags);

        // Allocate and map pages in user address space
        if (vmm_alloc_user_pages(pml4_phys, seg_start, num_pages, vmm_flags) != 0) {
            kprintf("[ELF] Error: Failed to allocate pages for segment %d\n", i);
            // TODO: cleanup already allocated pages
            return ELF_ERR_ALLOC_FAILED;
        }

        // Copy segment file data into the freshly mapped user pages.
        //
        // We briefly switch CR3 to the target address space and copy to the
        // segment's *user virtual* address (mapped RW there) instead of
        // writing through the kernel identity map at the raw physical page.
        // The old direct-physical write (dst = (uint8_t*)phys) faults when the
        // allocator hands out a physical page that is mapped read-only (or not
        // writable) in the kernel identity map: observed as a supervisor write
        // fault at pages in the 2-3GB window (e.g. 0xBFC03000) when loading a
        // service binary. Copying via the target's user VA is immune to which
        // physical page backs it.
        //
        // Safety: kernel code, the source ELF buffer and the kernel stack all
        // live below 2GB, which the new address space retains via the deep
        // copy of PML4[0] (only PDPT[2], the 2-3GB user window, is cleared), so
        // every address the copy touches stays mapped across the switch.
        // proc_create_user() holds preemption disabled around this call, the
        // same context setup_user_argv() relies on for its own CR3 switch.
        if (phdr->p_filesz > 0) {
            const uint8_t *src = (const uint8_t *)elf_data + phdr->p_offset;

            // #633 BACKSTOP. The pre-flight says the image is well-formed and
            // the mapping call above says it succeeded; this asks the page
            // tables THEMSELVES whether the exact bytes we are about to write
            // are present, user and writable in the target. If any answer is
            // no, fail the load instead of taking a kernel #PF with CR3
            // pointing at a foreign address space. Cost is one 4-level walk
            // per page (~400 for the largest app), paid once per launch.
#if !defined(ELF633_UNSAFE) && !defined(ELF633_UNSAFE_WP)
            if (!elf_user_range_writable(pml4_phys, seg_vaddr, phdr->p_filesz)) {
                kprintf("[ELF] Error: segment %d destination 0x%llX+0x%llX is not "
                        "mapped writable in the target address space; refusing to "
                        "copy\n", i, seg_vaddr, phdr->p_filesz);
                return ELF_ERR_DEST_NOT_WRITABLE;
            }
#endif

            // Copy segment data into the child address space via a temporary
            // CR3 switch. CRITICAL: hardware interrupts MUST be masked while
            // CR3 points at the child, otherwise an IRQ handler runs against
            // the wrong address space. Preemption-disable (held by the caller)
            // only stops the scheduler, NOT interrupt delivery. This window
            // scales with segment size, so a large binary (e.g. DOOM ~1.6MB)
            // held it open long enough that a stray IRQ produced rare,
            // nondeterministic memory corruption / page faults in the calling
            // process. Copy in bounded chunks so interrupt latency stays small
            // while each chunk's foreign-CR3 window is fully interrupt-safe.
            uint64_t copied = 0;
            const uint64_t CHUNK = 64 * 1024;
            while (copied < phdr->p_filesz) {
                uint64_t n = phdr->p_filesz - copied;
                if (n > CHUNK) n = CHUNK;

                uint64_t old_cr3, rflags;
                __asm__ volatile("pushfq; pop %0" : "=r"(rflags) :: "memory");
                __asm__ volatile("cli");
                __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));
                __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");

                // #19/#645: the DESTINATION is a user page in the child address
                // space (U/S=1), so under CR4.SMAP this memcpy is a #PF without
                // AC. Measured: this exact instruction is where an armed kernel
                // first died (RIP in memcpy_fast, CR2=0x80000000, err=0x3). The
                // bracket covers the one copy, not the surrounding loop
                // bookkeeping or the CR3 switch.
                uaccess_ac_t __ac = uaccess_begin();
                memcpy((void *)(seg_vaddr + copied), src + copied, n);
                uaccess_end(__ac);

#ifdef SMAPTEST
                // #19/#645 THE NEGATIVE HALF OF THE PROOF, and it must never
                // ship: `make SMAPTEST=1` arms ONE deliberate, unsanctioned
                // Ring-0 read of a user page, right here where a correct one
                // just happened inside a bracket. With CR4.SMAP armed this MUST
                // take a #PF with err bit P set, S set, W clear, and CR2 equal
                // to the address printed just below; if the kernel sails past
                // it, the guard is not doing anything and every green boot
                // above this line means nothing. Same discipline as
                // `make NOBLOCKTEST=1` (kernel/sync/noblock.c), for the same
                // reason: an assertion you have not watched fire is not an
                // assertion.
                {
                    static int fired = 0;
                    if (!fired) {
                        fired = 1;
                        extern uint64_t sec_cr4_read(void);
                        extern volatile uint8_t g_smap_active;
                        uint64_t __fl; __asm__ __volatile__("pushfq\n\tpopq %0" : "=r"(__fl));
                        uint64_t __eff = vmm_get_effective_flags_in(pml4_phys,
                                             (uint64_t)seg_vaddr & ~0xFFFULL);
                        kprintf("[SMAPTEST] cr4=0x%llx (SMAP bit21=%d) "
                                "rflags.AC=%d g_smap_active=%u eff=0x%llx "
                                "(USER bit2=%d)\n",
                                (unsigned long long)sec_cr4_read(),
                                (int)((sec_cr4_read() >> 21) & 1),
                                (int)((__fl >> 18) & 1),
                                (unsigned)g_smap_active,
                                (unsigned long long)__eff,
                                (int)((__eff >> 2) & 1));
                        kprintf("[SMAPTEST] about to read user VA 0x%llX from "
                                "Ring 0 with NO stac; SMAP must #PF here\n",
                                (unsigned long long)seg_vaddr);
                        volatile uint8_t probe = *(volatile uint8_t *)seg_vaddr;
                        kprintf("[SMAPTEST] FAILED: read returned 0x%02X, so "
                                "SMAP did NOT trap an unsanctioned user access\n",
                                (unsigned)probe);
                    }
                }
#endif

                __asm__ volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
                __asm__ volatile("push %0; popfq" : : "r"(rflags) : "cc", "memory");

                copied += n;
            }

            kprintf("[ELF] Loaded segment %d: %llu bytes at 0x%llX\n",
                    i, phdr->p_filesz, seg_vaddr);
        }

        // BSS (memsz - filesz) is already zeroed by vmm_alloc_user_pages
        if (phdr->p_memsz > phdr->p_filesz) {
            kprintf("[ELF] Segment %d: BSS %llu bytes\n",
                    i, phdr->p_memsz - phdr->p_filesz);
        }
    }

    // Apply PT_DYNAMIC relocations (R_X86_64_RELATIVE/GLOB_DAT/JUMP_SLOT/64/...)
    // into the target user address space via the same temporary, interrupt-safe
    // CR3-switch technique used above for segment data.
    //
    // #427 FAKE-audit CRITICAL fix: this used to be entirely absent. Every PIE
    // process was loaded with only a flat address shift applied to segments
    // and the entry point; any absolute pointer baked into .data/.data.rel.ro
    // (function-pointer tables, vtables, string-pointer arrays, the GOT
    // itself) was left as whatever the static linker wrote for its assumed
    // link-time base, not the real runtime base. This is the prerequisite for
    // a real dlopen()-style loader and CPython C-extension loading (#359).
    {
        elf_reloc_target_t tgt = { .is_user = true, .pml4_phys = pml4_phys,
                                    .kernel_base = NULL, .kernel_size = 0 };
        uint64_t t_r0 = elf_tsc_serialized();
#ifndef PIE_NORELOC
        elf_apply_all_relocations(elf_data, size, ehdr, relocation_offset, low_addr, high_addr, &tgt);
#else
        // #640 stage 1 CONTROL BUILD. -DPIE_NORELOC skips relocation for the
        // USER path only. It exists to answer the question this engine could
        // never answer before: does it actually DO anything? Before #640 no
        // binary in the tree had a PT_DYNAMIC, so "relocations applied: 0" was
        // indistinguishable from a broken engine. With /APPS/PIETEST loaded,
        // the fixed kernel prints "[PIETEST] RESULT: PASS" and this build
        // prints "[PIETEST] RESULT: FAIL (relocations not applied)" with the
        // unrelocated link-time pointer values. Never ship this.
        (void)tgt;
        kprintf("[ELF] PIE_NORELOC control build: skipping user relocations\n");
#endif
        t_reloc = elf_tsc_serialized() - t_r0;
    }

    // ------------------------------------------------------------------
    // #640 leg 4 step 2: THE PROTECT PASS. Everything above deliberately ran
    // with the whole image mapped writable, because the loader writes segment
    // bytes and then relocations through these very mappings at CPL 0 with
    // CR0.WP set (#633: enforcing PF_W any earlier is a kernel panic, not a
    // hardening win). Now that no further kernel write to the image is
    // pending, downgrade each page to what the ELF actually asked for.
    //
    // PER-PAGE UNION, not per-segment. A page can be covered by two PT_LOADs
    // when the link did not page-align the split (every binary built before
    // user-pie.ld gained its ALIGN, and any third-party ELF). Applying the
    // segments in order would let the LAST one win, so a shared text/data page
    // would end up writable+NX and the tail of .text would stop executing.
    // Taking the union instead can only ever grant a permission, never remove
    // one that some covering segment needs, so a badly-split image keeps
    // working and merely gets weaker W^X on the one straddling page.
    //
    // Consequence worth stating plainly: an image whose single PT_LOAD is
    // RWX (the pre-#640 layout, and the 30 carryover binaries still shipping)
    // gets exactly what it asks for, which is no protection at all. The
    // counters below make that visible per launch rather than assumed.
#ifndef ELF_NO_WX_PROTECT
    {
        uint64_t img_lo = align_down(low_addr + relocation_offset, ELF_PAGE_SIZE);
        uint64_t img_hi = align_up(high_addr + relocation_offset, ELF_PAGE_SIZE);
        uint64_t n_ro = 0, n_nx = 0, n_wx = 0, n_pages = 0;

        for (uint64_t pg = img_lo; pg < img_hi; pg += ELF_PAGE_SIZE) {
            int any_w = 0, any_x = 0, covered = 0;

            for (uint16_t k = 0; k < ehdr->e_phnum; k++) {
                const Elf64_Phdr *ph = elf_get_phdr(elf_data, k);
                if (ph == NULL || ph->p_type != PT_LOAD) continue;

                uint64_t s0 = align_down(ph->p_vaddr + relocation_offset, ELF_PAGE_SIZE);
                uint64_t s1 = align_up(ph->p_vaddr + relocation_offset + ph->p_memsz,
                                       ELF_PAGE_SIZE);
                if (pg < s0 || pg >= s1) continue;

                covered = 1;
                if (ph->p_flags & PF_W) any_w = 1;
                if (ph->p_flags & PF_X) any_x = 1;
            }
            if (!covered) continue;

            uint64_t keep = 0;
            if (any_w)  keep |= VMM_FLAG_WRITABLE;
            if (!any_x) keep |= VMM_FLAG_NX;

            vmm_protect_user_range(pml4_phys, pg, 1, keep);

            n_pages++;
            if (!any_w)          n_ro++;
            if (!any_x)          n_nx++;
            if (any_w && any_x)  n_wx++;
        }

        // One line, and it reports the ARTIFACT rather than the intent: how
        // many pages of THIS process actually came out read-only, no-execute,
        // and still-both. n_wx > 0 means this binary defeats W^X by its own
        // program headers; that is a property of the binary, not of the kernel.
        kprintf("[WX] %s: pages=%llu ro=%llu nx=%llu wx=%llu%s\n",
                name ? name : "?", n_pages, n_ro, n_nx, n_wx,
                (n_wx > 0) ? "  <-- RWX segment, W^X not enforced for this image" : "");
    }
#else
    // -DELF_NO_WX_PROTECT: the control build. Loads exactly as before the
    // protect pass existed, so "does the protect pass do anything?" is a
    // differential question with an answer, not an assumption.
    kprintf("[WX] %s: protect pass DISABLED (ELF_NO_WX_PROTECT control build)\n",
            name ? name : "?");
#endif

    // Set output values
    *entry_point = ehdr->e_entry + relocation_offset;
    *load_base = actual_base;
    *load_end = actual_end;

    {
        uint64_t total = elf_tsc_serialized() - t_enter;
        kprintf("[ELF] User load complete: entry=0x%llX, base=0x%llX, end=0x%llX\n",
                *entry_point, *load_base, *load_end);
        // #640 stage 2b, and read this before quoting any number it prints:
        // THIS LINE COSTS MORE THAN THE WORK IT REPORTS. kprintf goes to a
        // synchronous 115200-baud UART and is echoed again by the timestamped
        // logger, so ONE line is ~1.9M cycles, while applying 743 relocations is
        // ~52k. Every figure below therefore includes the cost of the loader's
        // own logging (this line is outside the timed region, but the ~8 other
        // [ELF] lines per load are inside it). To measure anything in here,
        // silence the loader's kprintfs first and compare against a control that
        // does the same; a serial-logging build cannot resolve anything smaller
        // than a log line.
        kprintf("[ELFCOST] %s %s total=%llu cyc reloc=%llu cyc (%llu%% of load) "
                "image=0x%llX bytes\n",
                name ? name : "<unnamed>", is_pie ? "PIE" : "fixed",
                total, t_reloc, total ? (t_reloc * 100ULL / total) : 0ULL,
                high_addr - low_addr);
    }

    return ELF_SUCCESS;
}

// ============================================================================
// Error Handling
// ============================================================================

const char *elf_strerror(int error) {
    if (error > 0) {
        return "Unknown error (positive value)";
    }

    int index = -error;
    if ((uint32_t)index >= ELF_ERROR_COUNT) {
        return "Unknown error code";
    }

    return elf_error_messages[index];
}

// ============================================================================
// Debug Functions
// ============================================================================

void elf_print_header(const void *elf_data) {
    if (elf_data == NULL) {
        kprintf("[ELF] Cannot print header: NULL pointer\n");
        return;
    }

    const Elf64_Ehdr *ehdr = elf_get_header(elf_data);

    kprintf("\n=== ELF64 Header ===\n");
    kprintf("Magic:      %02X %02X %02X %02X\n",
            ehdr->e_ident[EI_MAG0], ehdr->e_ident[EI_MAG1],
            ehdr->e_ident[EI_MAG2], ehdr->e_ident[EI_MAG3]);
    kprintf("Class:      %s\n",
            ehdr->e_ident[EI_CLASS] == ELFCLASS64 ? "ELF64" :
            ehdr->e_ident[EI_CLASS] == ELFCLASS32 ? "ELF32" : "Unknown");
    kprintf("Encoding:   %s\n",
            ehdr->e_ident[EI_DATA] == ELFDATA2LSB ? "Little-endian" :
            ehdr->e_ident[EI_DATA] == ELFDATA2MSB ? "Big-endian" : "Unknown");
    kprintf("Version:    %d\n", ehdr->e_ident[EI_VERSION]);
    kprintf("OS/ABI:     %d\n", ehdr->e_ident[EI_OSABI]);

    const char *type_str;
    switch (ehdr->e_type) {
        case ET_NONE: type_str = "None"; break;
        case ET_REL:  type_str = "Relocatable"; break;
        case ET_EXEC: type_str = "Executable"; break;
        case ET_DYN:  type_str = "Shared/PIE"; break;
        case ET_CORE: type_str = "Core"; break;
        default:      type_str = "Unknown"; break;
    }
    kprintf("Type:       %s (%d)\n", type_str, ehdr->e_type);

    const char *machine_str;
    switch (ehdr->e_machine) {
        case EM_NONE:   machine_str = "None"; break;
        case EM_386:    machine_str = "i386"; break;
        case EM_X86_64: machine_str = "x86_64"; break;
        default:        machine_str = "Other"; break;
    }
    kprintf("Machine:    %s (%d)\n", machine_str, ehdr->e_machine);

    kprintf("Entry:      0x%llX\n", ehdr->e_entry);
    kprintf("PH offset:  0x%llX\n", ehdr->e_phoff);
    kprintf("SH offset:  0x%llX\n", ehdr->e_shoff);
    kprintf("Flags:      0x%X\n", ehdr->e_flags);
    kprintf("EH size:    %d bytes\n", ehdr->e_ehsize);
    kprintf("PH entries: %d x %d bytes\n", ehdr->e_phnum, ehdr->e_phentsize);
    kprintf("SH entries: %d x %d bytes\n", ehdr->e_shnum, ehdr->e_shentsize);
    kprintf("SH strndx:  %d\n", ehdr->e_shstrndx);
    kprintf("====================\n\n");
}

void elf_print_phdrs(const void *elf_data) {
    if (elf_data == NULL) {
        kprintf("[ELF] Cannot print program headers: NULL pointer\n");
        return;
    }

    const Elf64_Ehdr *ehdr = elf_get_header(elf_data);

    kprintf("\n=== ELF64 Program Headers ===\n");
    kprintf("%-8s %-10s %-18s %-18s %-10s %-10s %-5s %-8s\n",
            "Index", "Type", "VirtAddr", "PhysAddr", "FileSz", "MemSz", "Flags", "Align");

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr *phdr = elf_get_phdr(elf_data, i);
        if (phdr == NULL) {
            continue;
        }

        const char *type_str;
        switch (phdr->p_type) {
            case PT_NULL:         type_str = "NULL"; break;
            case PT_LOAD:         type_str = "LOAD"; break;
            case PT_DYNAMIC:      type_str = "DYNAMIC"; break;
            case PT_INTERP:       type_str = "INTERP"; break;
            case PT_NOTE:         type_str = "NOTE"; break;
            case PT_SHLIB:        type_str = "SHLIB"; break;
            case PT_PHDR:         type_str = "PHDR"; break;
            case PT_TLS:          type_str = "TLS"; break;
            case PT_GNU_EH_FRAME: type_str = "GNU_EH"; break;
            case PT_GNU_STACK:    type_str = "GNU_STK"; break;
            case PT_GNU_RELRO:    type_str = "GNU_RO"; break;
            default:              type_str = "OTHER"; break;
        }

        char flags_str[4] = "---";
        if (phdr->p_flags & PF_R) flags_str[0] = 'R';
        if (phdr->p_flags & PF_W) flags_str[1] = 'W';
        if (phdr->p_flags & PF_X) flags_str[2] = 'X';

        kprintf("%-8d %-10s 0x%016llX 0x%016llX 0x%08llX 0x%08llX %s   0x%llX\n",
                i, type_str, phdr->p_vaddr, phdr->p_paddr,
                phdr->p_filesz, phdr->p_memsz, flags_str, phdr->p_align);
    }
    kprintf("=============================\n\n");
}

// ============================================================================
// #404 / #499 Phase Q boot-time self-test: prove elf_validate_full_rs (Rust,
// live under -DRUST_ELF) == elf_validate_full_c (verbatim reference) on the
// agreement domain (well-formed synthetic ELF64 images + shared ehdr/phdr-table
// reject mutations, incl. real-app-shaped headers), characterize the SECURITY
// divergence classes HONESTLY (the C's reachable OOBs vs the Rust confinement),
// and micro-benchmark both. LIGHT (#426, bounded, runs once): ~256 differential
// vectors + a small security sweep + a ~5k-iter RDTSC bench. The heavy work
// (2,000,000-vector differential + [RUST-SEC] characterization + ASan-proven
// reachable heap-buffer-overflow in the C reference) runs as the OFFLINE
// pre-flight. One [RUST-DIFF] elf, one [RUST-SEC] elf, one [RUST-PERF] elf line
// to serial + /BOOTLOG.
//
// SAFETY of running the C reference over MALFORMED vectors at boot: the crafted
// small-e_phentsize case makes elf_validate_full_c's elf_get_phdr read a full
// 56-byte phdr past the logical image length. All vectors live in a 4096-byte
// static buffer and the logical `size` we pass is always <= 1024, so a bounded
// over-read stays inside the backing buffer (never a live OOB). The oversized-
// p_filesz case only makes the C ACCEPT (its own reads stay in-buffer; the OOB
// is in elf_load()'s memcpy, which the self-test never calls).

static uint32_t elfdiff_rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

static inline uint64_t elf_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static int elf_validated_eq(int rc_a, const elf_validated_t *a, int rc_b, const elf_validated_t *b) {
    if (rc_a != rc_b) return 1;
    if (rc_a != ELF_SUCCESS) return 0;   // both rejected identically: fields N/A
    return !(a->e_type == b->e_type && a->e_machine == b->e_machine &&
             a->e_phnum == b->e_phnum && a->e_phentsize == b->e_phentsize &&
             a->n_load == b->n_load && a->e_entry == b->e_entry &&
             a->e_phoff == b->e_phoff && a->first_load_vaddr == b->first_load_vaddr);
}

// Build a well-formed ELF64 image (ehdr + N phdrs, >=1 PT_LOAD, phentsize=56,
// memsz>=filesz, in-bounds segments) into buf. Returns the logical length. This
// is a structurally-real ELF header (identical shape to every /apps binary).
static uint32_t elf_build_wellformed(uint8_t *buf, uint32_t cap, uint32_t *seed) {
    for (uint32_t i = 0; i < cap; i++) buf[i] = 0;
    uint16_t nph = 1 + (elfdiff_rng(seed) % 4);
    uint16_t phent = 56;
    uint64_t phoff = 64;
    uint32_t need = 64 + (uint32_t)nph * phent + 256;
    if (need > cap) need = cap;
    Elf64_Ehdr *e = (Elf64_Ehdr *)buf;
    e->e_ident[EI_MAG0] = ELF_MAGIC_0; e->e_ident[EI_MAG1] = ELF_MAGIC_1;
    e->e_ident[EI_MAG2] = ELF_MAGIC_2; e->e_ident[EI_MAG3] = ELF_MAGIC_3;
    e->e_ident[EI_CLASS] = ELFCLASS64; e->e_ident[EI_DATA] = ELFDATA2LSB;
    e->e_ident[EI_VERSION] = EV_CURRENT;
    e->e_type = (elfdiff_rng(seed) & 1) ? ET_EXEC : ET_DYN;
    e->e_machine = EM_X86_64; e->e_version = EV_CURRENT;
    e->e_entry = 0x400000 + (elfdiff_rng(seed) % 0x1000);
    e->e_phoff = phoff; e->e_phentsize = phent; e->e_phnum = nph;
    for (uint16_t i = 0; i < nph; i++) {
        Elf64_Phdr *p = (Elf64_Phdr *)(buf + phoff + (uint64_t)i * phent);
        int load = (i == 0) || (elfdiff_rng(seed) & 1);
        p->p_type = load ? PT_LOAD : PT_NULL;
        if (load) {
            uint32_t room = (need > 128) ? 64 : 0;
            p->p_offset = 64 + (room ? (elfdiff_rng(seed) % room) : 0);
            uint64_t maxf = need - p->p_offset;
            p->p_filesz = maxf ? (elfdiff_rng(seed) % (maxf + 1)) : 0;
            p->p_memsz = p->p_filesz + (elfdiff_rng(seed) % 64);
            p->p_vaddr = 0x400000 + (uint64_t)i * 0x1000;
        }
    }
    return need;
}

// ===========================================================================
// #633 boot self-test. Builds synthetic user images with the exact shapes that
// killed the kernel, loads each into a THROWAWAY user address space through the
// live elf_load_user(), and asserts the verdict.
//
// PROVING IT CAN FAIL (both halves of the fix, MEASURED 2026-08-03 on VM <vmid>
// booting golden 998 with only the kernel replaced):
//
//   -DELF633_UNSAFE     removes the pre-flight AND the writable mapping. Vector
//                       A panics: [KERNEL PANIC] Page Fault RIP=0x457be3
//                       CR2=0x7bc03010 err=0x3 (P W S), DESKTOP_READY=0. Note
//                       WHERE: the fault is inside vmm_alloc_user_pages, not the
//                       copy. Mapping a page at 0x400000 has to write a PTE into
//                       the page table the deep-copied PML4[0] still shares with
//                       the FIRMWARE's identity map, and that table (physical
//                       0x7bc03000) is READ-ONLY. This is why the check has to
//                       run BEFORE the first mapping call: no amount of care
//                       inside the copy loop could have caught it. The reported
//                       #633 crash (RIP=0x455e83 CR2=0x7bc03010) is the same
//                       fault, same CR2, from a different build.
//
//   -DELF633_UNSAFE_WP  keeps the pre-flight, restores only the old "map with
//                       the segment's own PF_W" behaviour. Vector C then panics
//                       with a WP write fault at the segment's own address:
//                       proof that the forced-writable mapping is load-bearing
//                       on its own, and not just belt-and-braces.
//
// A test that cannot be made to fail proves nothing, so the two failing builds
// are the evidence that the passing one means something.
// ===========================================================================

// One synthetic user image. base/entry/flags are the knobs the attacker (or a
// broken Makefile) controls. Returns the logical image length.
static uint32_t elf633_build(uint8_t *buf, uint32_t cap, uint64_t base,
                             uint64_t entry_off, uint32_t seg_flags,
                             uint64_t second_seg_gap, uint16_t e_type) {
    for (uint32_t i = 0; i < cap; i++) buf[i] = 0;
    uint16_t nph = second_seg_gap ? 2 : 1;
    Elf64_Ehdr *e = (Elf64_Ehdr *)buf;
    e->e_ident[EI_MAG0] = ELF_MAGIC_0; e->e_ident[EI_MAG1] = ELF_MAGIC_1;
    e->e_ident[EI_MAG2] = ELF_MAGIC_2; e->e_ident[EI_MAG3] = ELF_MAGIC_3;
    e->e_ident[EI_CLASS] = ELFCLASS64; e->e_ident[EI_DATA] = ELFDATA2LSB;
    e->e_ident[EI_VERSION] = EV_CURRENT;
    e->e_type = e_type; e->e_machine = EM_X86_64; e->e_version = EV_CURRENT;
    e->e_entry = base + entry_off;
    e->e_phoff = 64; e->e_phentsize = 56; e->e_phnum = nph;

    Elf64_Phdr *p = (Elf64_Phdr *)(buf + 64);
    p->p_type = PT_LOAD; p->p_flags = seg_flags;
    p->p_offset = 512; p->p_vaddr = base; p->p_paddr = base;
    p->p_filesz = 256; p->p_memsz = 4096; p->p_align = 4096;
    for (uint32_t i = 0; i < 256; i++) buf[512 + i] = (uint8_t)(i ^ 0xA5);

    if (second_seg_gap) {
        Elf64_Phdr *p2 = (Elf64_Phdr *)(buf + 64 + 56);
        p2->p_type = PT_LOAD; p2->p_flags = PF_R | PF_W;
        p2->p_offset = 768; p2->p_vaddr = base + second_seg_gap;
        p2->p_paddr = p2->p_vaddr;
        p2->p_filesz = 64; p2->p_memsz = 4096; p2->p_align = 4096;
    }
    return 1024;
}

static void elf_load_user_selftest(void) {
    static uint8_t img[4096];
    uint32_t pass = 0, total = 0;
    const uint64_t GOOD = 0x80000000ULL;   // the user.ld base every real app uses

    struct { const char *name; uint64_t base; uint64_t entry_off; uint32_t flags;
             uint64_t gap; uint16_t etype; int want; uint64_t want_base; } v[] = {
        // A: THE #633 BUG. An app linked without -T user.ld: 0x400000, and its
        // first PT_LOAD is R-only (MEASURED on the shipping HELLO binary).
        { "unlinked-base-0x400000", 0x400000ULL, 0x130, PF_R,          0, ET_EXEC, ELF_ERR_BAD_LOAD_ADDR, 0 },
        // B: a correctly-linked app must still load. If this ever fails the gate
        // has become a blanket reject and every app is broken.
        { "well-formed-RWX",        GOOD,        0x100, PF_R|PF_W|PF_X, 0, ET_EXEC, ELF_SUCCESS, GOOD },
        // C: correctly BASED but R-only. Pre-#633 this mapped the destination
        // read-only and the CPL-0 copy then took a WP fault = panic. Must load.
        { "read-only-segment",      GOOD,        0x100, PF_R|PF_X,      0, ET_EXEC, ELF_SUCCESS, GOOD },
        // D: entry outside any executable segment.
        { "entry-not-in-text",      GOOD,        0x9000, PF_R|PF_W|PF_X, 0, ET_EXEC, ELF_ERR_BAD_LOAD_ADDR, 0 },
        // E: two PT_LOADs 320MB apart: a page-table exhaustion DoS.
        { "absurd-span",            GOOD,        0x100, PF_R|PF_W|PF_X,
                                    320ULL*1024*1024, ET_EXEC, ELF_ERR_BAD_LOAD_ADDR, 0 },
        // F (#640 stage 2): a PIE declares base 0 and MUST land in the dedicated
        // userland window, NOT at the old 0x90000000 (which is the iMac's live
        // framebuffer address). want_base is checked, because "it loaded" and
        // "it loaded where we intended" are different claims and only the
        // second one retires the #522/#650 hazard.
        { "pie-lands-in-user-window", 0,           0x100, PF_R|PF_W|PF_X, 0,
                                    ET_DYN,  ELF_SUCCESS, USER_WIN_IMAGE_BASE },
        // G: a PIE whose span would run past the end of the window.
        { "pie-absurd-span",          0,           0x100, PF_R|PF_W|PF_X,
                                    320ULL*1024*1024, ET_DYN, ELF_ERR_BAD_LOAD_ADDR, 0 },
    };

    for (uint32_t i = 0; i < sizeof(v)/sizeof(v[0]); i++) {
        uint64_t cr3 = vmm_create_user_space();
        if (cr3 == 0) {
            kprintf("[ELF633] vector %s: could not create address space; skipped\n", v[i].name);
            continue;
        }
        uint32_t len = elf633_build(img, sizeof(img), v[i].base, v[i].entry_off,
                                    v[i].flags, v[i].gap, v[i].etype);
        uint64_t entry = 0, lb = 0, le = 0;
        int rc = elf_load_user_named(img, len, cr3, &entry, &lb, &le, v[i].name);
        total++;
        if (rc != v[i].want) {
            kprintf("[ELF633] vector %s: got %d (%s), want %d\n",
                    v[i].name, rc, elf_strerror(rc), v[i].want);
        } else if (v[i].want == ELF_SUCCESS && v[i].want_base && lb != v[i].want_base) {
            kprintf("[ELF633] vector %s: loaded at 0x%llX, expected base 0x%llX\n",
                    v[i].name, lb, v[i].want_base);
        } else {
            pass++;
        }
        vmm_destroy_user_space(cr3);
    }

    kprintf("[ELF633] malformed-ELF loader gate: %u/%u vectors -> %s\n",
            pass, total, (pass == total && total > 0) ? "PASS" : "FAIL");
    bootlog_write("[ELF633] malformed-ELF loader gate: %u/%u -> %s",
                  pass, total, (pass == total && total > 0) ? "PASS" : "FAIL");
}

void elf_rust_selftest(void) {
    static uint8_t buf[4096];
    uint32_t seed = 0x7e1f0404u;
    uint32_t vectors = 0, mismatches = 0;
    int first_bad = -1;

    // Force-reference the Rust symbol so its archive member is always linked
    // (matches the icmp/arp/dns/dhcp/url pattern), regardless of -DRUST_ELF.
    { elf_validated_t t; elf_build_wellformed(buf, sizeof(buf), &seed);
      elf_validate_full_rs(buf, 512, &t); }

    // Part 1: agreement domain (~256 vectors: well-formed + shared ehdr/phdr-
    // table reject mutations). Both sides must agree exactly (int + fields).
    seed = 0x7e1f0404u;
    for (uint32_t iter = 0; iter < 256; iter++) {
        elf_validated_t vc, vr;
        uint32_t len = elf_build_wellformed(buf, sizeof(buf), &seed);
        uint32_t plen = len;
        uint32_t klass = elfdiff_rng(&seed) % 12;
        Elf64_Ehdr *e = (Elf64_Ehdr *)buf;
        switch (klass) {
            case 0: break;                                   // pristine well-formed
            case 1: buf[1] ^= 0xFF; break;                   // bad magic
            case 2: buf[EI_CLASS] = 1; break;                // not 64-bit
            case 3: buf[EI_DATA] = 2; break;                 // not LE
            case 4: e->e_machine = 0x3E ^ 0xFF; break;       // bad machine
            case 5: e->e_type = 9; break;                    // bad type
            case 6: e->e_phnum = 0; break;                   // no phdr
            case 7: e->e_phoff = len + 100; break;           // phoff past EOF
            case 8: e->e_phnum = 5000; break;                // phdr table past EOF
            case 9: plen = 40; break;                        // truncated header
            case 10: plen = 0; break;                        // zero filelen
            case 11: buf[EI_VERSION] = 9; break;             // bad EI_VERSION
        }
        int rc = elf_validate_full_c(buf, plen, &vc);
        int rr = elf_validate_full_rs(buf, (uint64_t)plen, &vr);
        vectors++;
        if (elf_validated_eq(rc, &vc, rr, &vr)) {
            mismatches++;
            if (first_bad < 0) first_bad = (int)iter;
        }
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] elf: %u vectors, %u mismatches -> %s\n", vectors, mismatches, verdict);
    bootlog_write("[RUST-DIFF] elf: %u vectors, %u mismatches -> %s", vectors, mismatches, verdict);
    if (mismatches != 0) {
        kprintf("[RUST-DIFF] elf FIRST MISMATCH iter=%d\n", first_bad);
        bootlog_write("[RUST-DIFF] elf FIRST MISMATCH iter=%d", first_bad);
    }

    // Part 2: SECURITY posture (HONEST + REACHABLE, unlike arp/dns/url). The
    // verbatim C has three gaps on the untrusted header; sweep each crafted class
    // and count how often C accepts/over-reads while Rust confines.
    {
        uint32_t n_filesz = 0, c_filesz = 0;   // oversized p_filesz (underflow bypass)
        uint32_t n_phent = 0,  c_phent = 0;    // e_phentsize < 56 (elf_get_phdr over-read)
        uint32_t n_memsz = 0,  c_memsz = 0;    // p_memsz < p_filesz (write-overflow)
        uint32_t s2 = 0x5ec0de11u;
        for (uint32_t r = 0; r < 300; r++) {
            elf_validated_t vc, vr;
            uint32_t len = elf_build_wellformed(buf, sizeof(buf), &s2);
            if (len > 1024) len = 1024;   // keep any C over-read inside buf[4096]
            Elf64_Ehdr *e = (Elf64_Ehdr *)buf;
            Elf64_Phdr *p0 = (Elf64_Phdr *)(buf + e->e_phoff);   // first phdr (PT_LOAD)
            uint32_t klass = r % 3;
            int rc, rr;
            if (klass == 0) {
                p0->p_offset = 8; p0->p_filesz = (uint64_t)len + 1 + (elfdiff_rng(&s2) % 512);
                p0->p_memsz = p0->p_filesz;
                rc = elf_validate_full_c(buf, len, &vc);
                rr = elf_validate_full_rs(buf, (uint64_t)len, &vr);
                n_filesz++;
                if (rc == ELF_SUCCESS && rr == ELF_ERR_SEGMENT_OVERFLOW) c_filesz++;
            } else if (klass == 1) {
                uint16_t small = 8 + (uint16_t)(elfdiff_rng(&s2) % 40);   // 8..47 < 56
                e->e_phentsize = small; e->e_phnum = 1; e->e_phoff = 64;
                if (64u + small > len) len = 64u + small;
                rc = elf_validate_full_c(buf, len, &vc);
                rr = elf_validate_full_rs(buf, (uint64_t)len, &vr);
                n_phent++;
                if (rc == ELF_SUCCESS && rr == ELF_ERR_PHDR_OVERFLOW) c_phent++;
            } else {
                if (p0->p_filesz < 8) p0->p_filesz = 8;
                if (p0->p_offset + p0->p_filesz > len) { p0->p_offset = 8; p0->p_filesz = (len > 16) ? 16 : 8; }
                p0->p_memsz = 0;
                rc = elf_validate_full_c(buf, len, &vc);
                rr = elf_validate_full_rs(buf, (uint64_t)len, &vr);
                n_memsz++;
                if (rc == ELF_SUCCESS && rr == ELF_ERR_SEGMENT_OVERFLOW) c_memsz++;
            }
        }
        kprintf("[RUST-SEC] elf: REACHABLE OOBs in the C confined by Rust - oversized-p_filesz(underflow bypass, OOB r/w) %u/%u; "
                "small-e_phentsize(over-read) %u/%u; p_memsz<p_filesz(write-overflow) %u/%u\n",
                c_filesz, n_filesz, c_phent, n_phent, c_memsz, n_memsz);
        bootlog_write("[RUST-SEC] elf: C oversized-filesz %u/%u + small-phentsize %u/%u + memsz<filesz %u/%u confined by Rust (2 ASan-proven reachable OOBs offline)",
                      c_filesz, n_filesz, c_phent, n_phent, c_memsz, n_memsz);
    }

    // Part 2b: #489 HARDENING PROOF (2026-07-26). Part 2 above exercises the raw
    // elf_validate_full_c REFERENCE, which KEEPS its three header gaps on purpose.
    // The C FALLBACK the loader actually uses is elf_validate() (the dispatcher,
    // now with the e_phentsize gate) + calculate_load_bounds() (now with the
    // no-underflow file-bounds check and the p_memsz>=p_filesz check). This part
    // feeds the SAME three crafted classes through those live loader-path
    // functions and asserts each is REJECTED before any allocation/copy, i.e. the
    // C fallback is now sound whether or not -DRUST_ELF is set. Vectors stay in
    // buf[4096] with size <= 1024 so no craft can OOB the self-test itself.
    {
        uint32_t n_phent2 = 0,  ok_phent2 = 0;    // small e_phentsize -> elf_validate rejects
        uint32_t n_filesz2 = 0, ok_filesz2 = 0;   // oversized p_filesz -> calculate_load_bounds rejects
        uint32_t n_memsz2 = 0,  ok_memsz2 = 0;    // p_memsz<p_filesz  -> calculate_load_bounds rejects
        uint32_t s2b = 0x489c0de1u;
        for (uint32_t r = 0; r < 300; r++) {
            uint32_t len = elf_build_wellformed(buf, sizeof(buf), &s2b);
            if (len > 1024) len = 1024;
            Elf64_Ehdr *e = (Elf64_Ehdr *)buf;
            Elf64_Phdr *p0 = (Elf64_Phdr *)(buf + e->e_phoff);
            uint32_t klass = r % 3;
            if (klass == 0) {
                // small-e_phentsize: the dispatcher gate must reject (PHDR_OVERFLOW)
                uint16_t small = 8 + (uint16_t)(elfdiff_rng(&s2b) % 40);   // 8..47 < 56
                e->e_phentsize = small; e->e_phnum = 1; e->e_phoff = 64;
                if (64u + small > len) len = 64u + small;
                n_phent2++;
                if (elf_validate(buf, len) == ELF_ERR_PHDR_OVERFLOW) ok_phent2++;
            } else if (klass == 1) {
                // oversized p_filesz: calculate_load_bounds must reject (no underflow)
                p0->p_offset = 8; p0->p_filesz = (uint64_t)len + 1 + (elfdiff_rng(&s2b) % 512);
                p0->p_memsz = p0->p_filesz;
                uint64_t lo = 0, hi = 0;
                n_filesz2++;
                if (calculate_load_bounds(buf, len, &lo, &hi) == ELF_ERR_SEGMENT_OVERFLOW) ok_filesz2++;
            } else {
                // p_memsz < p_filesz: calculate_load_bounds must reject
                if (p0->p_filesz < 8) p0->p_filesz = 8;
                if (p0->p_offset + p0->p_filesz > len) { p0->p_offset = 8; p0->p_filesz = (len > 16) ? 16 : 8; }
                p0->p_memsz = 0;
                uint64_t lo = 0, hi = 0;
                n_memsz2++;
                if (calculate_load_bounds(buf, len, &lo, &hi) == ELF_ERR_SEGMENT_OVERFLOW) ok_memsz2++;
            }
        }
        int pass = (ok_phent2 == n_phent2) && (ok_filesz2 == n_filesz2) && (ok_memsz2 == n_memsz2);
        kprintf("[RUST-SEC-C] elf: #489 C loader path REJECTS crafted OOBs - "
                "small-phentsize %u/%u, oversized-filesz %u/%u, memsz<filesz %u/%u -> %s\n",
                ok_phent2, n_phent2, ok_filesz2, n_filesz2, ok_memsz2, n_memsz2,
                pass ? "PASS" : "FAIL");
        bootlog_write("[RUST-SEC-C] elf: #489 C loader rejects phent %u/%u filesz %u/%u memsz %u/%u -> %s",
                      ok_phent2, n_phent2, ok_filesz2, n_filesz2, ok_memsz2, n_memsz2,
                      pass ? "PASS" : "FAIL");
    }

    // Part 3: RDTSC micro-benchmark over a fixed well-formed ELF. LIGHT: 5k.
    {
        const int iters = 5000;
        elf_validated_t o;
        uint32_t s3 = 0x1234abcd;
        uint32_t len = elf_build_wellformed(buf, sizeof(buf), &s3);

        for (int i = 0; i < 300; i++) {
            elf_validate_full_c(buf, len, &o);
            elf_validate_full_rs(buf, (uint64_t)len, &o);
        }
        uint64_t t0 = elf_tsc_serialized();
        for (int i = 0; i < iters; i++) elf_validate_full_c(buf, len, &o);
        uint64_t t1 = elf_tsc_serialized();
        for (int i = 0; i < iters; i++) elf_validate_full_rs(buf, (uint64_t)len, &o);
        uint64_t t2 = elf_tsc_serialized();

        uint64_t c_cyc = (t1 - t0) / iters;
        uint64_t r_cyc = (t2 - t1) / iters;
        uint64_t ratio100 = (c_cyc != 0) ? (r_cyc * 100ULL / c_cyc) : 0;
        kprintf("[RUST-PERF] elf: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
        bootlog_write("[RUST-PERF] elf: C=%llu RS=%llu ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }

    // Part 4: #633 - Ring 3 must never be able to panic Ring 0 with a malformed
    // ELF. This is NOT a parser differential: it drives the REAL loader
    // (elf_load_user) against a REAL, freshly created user address space, which
    // is the only thing that exercises the mapping and the foreign-CR3 copy,
    // i.e. the code that actually took the kernel down.
    elf_load_user_selftest();
}
