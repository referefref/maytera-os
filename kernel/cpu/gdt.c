// gdt.c - Global Descriptor Table implementation for x86_64
#include "gdt.h"
#include "../serial.h"
#include "../string.h"

// GDT entries: NULL, kernel code, kernel data, user code, user data, TSS
// TSS takes 2 entries (16 bytes) in 64-bit mode
#define GDT_ENTRIES 7

// GDT and TSS structures
static gdt_entry_t gdt[GDT_ENTRIES] __attribute__((aligned(16)));
tss_t tss __attribute__((aligned(16)));  // Made global for syscall access
static gdt_ptr_t gdt_ptr;

// Interrupt stack (separate from main kernel stack)
static uint8_t interrupt_stack[8192] __attribute__((aligned(16)));

// Set a GDT entry
static void gdt_set_entry(int index, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags) {
    gdt[index].limit_low    = limit & 0xFFFF;
    gdt[index].base_low     = base & 0xFFFF;
    gdt[index].base_middle  = (base >> 16) & 0xFF;
    gdt[index].access       = access;
    gdt[index].flags_limit  = ((limit >> 16) & 0x0F) | (flags << 4);
    gdt[index].base_high    = (base >> 24) & 0xFF;
}

// Set TSS entry in GDT (64-bit TSS uses 16 bytes)
static void gdt_set_tss(int index, uint64_t base, uint32_t limit) {
    tss_entry_t *entry = (tss_entry_t *)&gdt[index];

    entry->limit_low    = limit & 0xFFFF;
    entry->base_low     = base & 0xFFFF;
    entry->base_middle1 = (base >> 16) & 0xFF;
    entry->access       = 0x89;  // Present, TSS (busy=0), ring 0
    entry->flags_limit  = ((limit >> 16) & 0x0F);
    entry->base_middle2 = (base >> 24) & 0xFF;
    entry->base_high    = (base >> 32) & 0xFFFFFFFF;
    entry->reserved     = 0;
}

// ---------------------------------------------------------------------------
// Per-CPU TSS + GDT for SMP application processors (#279 stage 3b-1).
// Each AP loads its OWN GDT (a copy of the BSP GDT but with a TSS descriptor
// pointing at that AP's own TSS) and does LTR, so a Ring 3 -> Ring 0 transition
// (interrupt/exception from a user process running on that AP) switches to that
// AP's kernel stack via its TSS.rsp0 instead of clobbering the BSP's. This is
// inert until APs actually run user processes (stage 3b-3); building/loading it
// now is safe (the descriptors are valid; nothing references the new rsp0 yet).
#define MAX_SMP_CPUS 16
static tss_t       ap_tss[MAX_SMP_CPUS] __attribute__((aligned(16)));
static gdt_entry_t ap_gdt[MAX_SMP_CPUS][GDT_ENTRIES] __attribute__((aligned(16)));
static uint8_t     ap_int_stack[MAX_SMP_CPUS][8192] __attribute__((aligned(16)));

// Write a 64-bit TSS descriptor (16 bytes, 2 GDT slots) into an arbitrary GDT.
static void gdt_set_tss_in(gdt_entry_t *g, int index, uint64_t base, uint32_t limit) {
    tss_entry_t *entry = (tss_entry_t *)&g[index];
    entry->limit_low    = limit & 0xFFFF;
    entry->base_low     = base & 0xFFFF;
    entry->base_middle1 = (base >> 16) & 0xFF;
    entry->access       = 0x89;  // Present, 64-bit TSS (available), ring 0
    entry->flags_limit  = ((limit >> 16) & 0x0F);
    entry->base_middle2 = (base >> 24) & 0xFF;
    entry->base_high    = (base >> 32) & 0xFFFFFFFF;
    entry->reserved     = 0;
}

// Per-CPU TSS.rsp0 setter (the SMP analogue of gdt_set_kernel_stack). Called on
// context switch when a user process is scheduled onto this AP.
void gdt_set_kernel_stack_cpu(uint32_t cpu, uint64_t stack) {
    if (cpu < MAX_SMP_CPUS) ap_tss[cpu].rsp0 = stack;
}

// Build + load this AP's GDT and TSS. kstack_top = the AP's initial ring-0 stack.
void gdt_init_ap(uint32_t cpu, uint64_t kstack_top) {
    if (cpu == 0 || cpu >= MAX_SMP_CPUS) {
        // cpu 0 is the BSP (uses the global gdt/tss); out-of-range -> fall back.
        gdt_load_ap();
        return;
    }
    // Copy the BSP segment descriptors (null/kcode/kdata/udata/ucode) verbatim.
    memcpy(ap_gdt[cpu], gdt, sizeof(gdt_entry_t) * GDT_ENTRIES);
    // This AP's own TSS.
    memset(&ap_tss[cpu], 0, sizeof(tss_t));
    ap_tss[cpu].rsp0        = kstack_top;
    ap_tss[cpu].ist1        = (uint64_t)&ap_int_stack[cpu][sizeof(ap_int_stack[cpu])];
    ap_tss[cpu].iopb_offset = sizeof(tss_t);
    // TSS descriptor occupies GDT entries 5-6 (selector 0x28), same as the BSP.
    gdt_set_tss_in(ap_gdt[cpu], 5, (uint64_t)&ap_tss[cpu], sizeof(tss_t) - 1);
    // Load this AP's GDT and reload segment registers, then LTR our TSS.
    gdt_ptr_t p;
    p.limit = sizeof(gdt_entry_t) * GDT_ENTRIES - 1;
    p.base  = (uint64_t)&ap_gdt[cpu][0];
    gdt_load(&p, GDT_KERNEL_CODE, GDT_KERNEL_DATA);
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS));
}

// Initialize the GDT
void gdt_load_ap(void) {
    // Application processor: load the shared GDT and reload segment
    // registers. We intentionally do NOT ltr (the TSS busy bit would
    // fault a second ltr); ring0-only AP work does not need a TSS yet.
    gdt_load(&gdt_ptr, GDT_KERNEL_CODE, GDT_KERNEL_DATA);
}

void gdt_init(void) {
    kprintf("[GDT] Initializing Global Descriptor Table...\n");

    // Clear GDT
    memset(gdt, 0, sizeof(gdt));

    // Entry 0: Null descriptor (required)
    gdt_set_entry(0, 0, 0, 0, 0);

    // Entry 1: Kernel code segment (selector 0x08)
    // 64-bit code segment: Present, DPL=0, Executable, Readable
    gdt_set_entry(1, 0, 0xFFFFF,
                  GDT_PRESENT | GDT_DPL_RING0 | GDT_SEGMENT | GDT_EXECUTABLE | GDT_RW,
                  GDT_LONG_MODE | GDT_GRANULARITY);

    // Entry 2: Kernel data segment (selector 0x10)
    // 64-bit data segment: Present, DPL=0, Writable
    gdt_set_entry(2, 0, 0xFFFFF,
                  GDT_PRESENT | GDT_DPL_RING0 | GDT_SEGMENT | GDT_RW,
                  GDT_GRANULARITY);

    // CRITICAL FIX Build 103: Swap order for SYSRET compatibility
    // SYSRET loads SS from (STAR[63:48] + 8), CS from (STAR[63:48] + 16)
    // So user data must be BEFORE user code in GDT!
    
    // Entry 3: User data segment (selector 0x18)
    // 64-bit data segment: Present, DPL=3, Writable
    gdt_set_entry(3, 0, 0xFFFFF,
                  GDT_PRESENT | GDT_DPL_RING3 | GDT_SEGMENT | GDT_RW,
                  GDT_GRANULARITY);

    // Entry 4: User code segment (selector 0x20)
    // 64-bit code segment: Present, DPL=3, Executable, Readable
    gdt_set_entry(4, 0, 0xFFFFF,
                  GDT_PRESENT | GDT_DPL_RING3 | GDT_SEGMENT | GDT_EXECUTABLE | GDT_RW,
                  GDT_LONG_MODE | GDT_GRANULARITY);

    // Initialize TSS
    memset(&tss, 0, sizeof(tss));
    tss.rsp0 = (uint64_t)&interrupt_stack[sizeof(interrupt_stack)];
    tss.ist1 = (uint64_t)&interrupt_stack[sizeof(interrupt_stack)];  // IST1 for NMI/DF
    tss.iopb_offset = sizeof(tss);

    // Entry 5-6: TSS (selector 0x28, takes 16 bytes = 2 entries)
    gdt_set_tss(5, (uint64_t)&tss, sizeof(tss) - 1);

    // Set up GDT pointer
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint64_t)&gdt;

    // Load GDT
    kprintf("[GDT] Loading GDT at 0x%lx (limit %u)\n", gdt_ptr.base, gdt_ptr.limit);
    gdt_load(&gdt_ptr, GDT_KERNEL_CODE, GDT_KERNEL_DATA);

    // Load TSS
    kprintf("[GDT] Loading TSS (selector 0x%x)\n", GDT_TSS);
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS));

    kprintf("[GDT] GDT and TSS initialized\n");
}

// Set kernel stack pointer in TSS
void gdt_set_kernel_stack(uint64_t stack) {
    tss.rsp0 = stack;
}
