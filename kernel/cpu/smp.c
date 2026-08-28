// smp.c - Symmetric Multi-Processing Support Implementation
// Part of Task #41 (SMP Support)

#include "smp.h"
#include "apic.h"
#include "gdt.h"
#include "idt.h"
#include "sse.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sync/spinlock.h"
#include "../drivers/acpi_madt.h"

// External trampoline code (from trampoline.asm)
extern uint8_t ap_trampoline_start[];
extern uint8_t ap_trampoline_end[];

// IDT vector used to wake idle (HLT'd) APs when new work is submitted (defined
// here so smp_init can register the handler before the work-pool section).
#define SMP_WAKE_VECTOR 240
static void smp_wake_handler(interrupt_frame_t *frame);

// ============================================================================
// Global State
// ============================================================================

// Per-CPU data for all CPUs
static per_cpu_t per_cpu_data[MAYTERA_MAX_CPUS] __attribute__((aligned(4096)));

// Number of CPUs detected
static uint32_t cpu_count = 0;
volatile unsigned long g_ap_heartbeat[MAYTERA_MAX_CPUS];  // #279 stage1 proof-of-life

// Number of CPUs online
static volatile uint32_t cpus_online = 0;

// 2026-08-23: "spinlock_t kernel_lock" and its kernel_lock_acquire()/
// kernel_lock_release() wrappers were DELETED from here. They had zero
// callers, and cpu/smp.h advertised them as "the big kernel lock for
// coarse-grained synchronization" while THE ACTUAL Big Kernel Lock is
// bkl_word/bkl_owner/bkl_depth below. A dead lock that names itself after
// the live one is worse than no lock: calling it would have given the
// caller confident, total, and entirely imaginary protection. Use
// bkl_acquire()/bkl_release() (recursive, owner-tracked, IRQ-friendly,
// instrumented) instead.

// ---- Per-CPU data accessed via GS base (#279 3b-1.5) -------------------
// GS_BASE is set (permanently, no swapgs) to &cpu_local[cpu] on every CPU so
// the SYSCALL entry stub can load THIS cpu's current-process kernel stack from
// gs:[0] and use gs:[8]/gs:[16] as per-cpu scratch. Layout MUST match the
// offsets hard-coded in proc/syscall.asm.
typedef struct {
    uint64_t kernel_rsp;   // gs:0  current proc ring-0 stack top for this CPU
    uint64_t user_rsp;     // gs:8  scratch: user RSP saved at syscall entry
    uint64_t scratch;      // gs:16 scratch: syscall number across stack switch
    uint64_t cpu_id;       // gs:24
} cpu_local_t;
static cpu_local_t cpu_local[MAYTERA_MAX_CPUS] __attribute__((aligned(64)));

#include "mono.h"   // #67 pass 5: mono_us() for BKL hold times
#define MSR_GS_BASE 0xC0000101

// Point this CPU's GS base at its cpu_local slot. Call AFTER the final gdt_load
// on the CPU (gdt_load reloads the gs selector, zeroing GS_BASE).
// #279 3b-3: set once per-CPU GS base is live so proc_current() may use the
// fast per-CPU path (before this, callers fall back to the BSP global).
volatile int g_smp_current_ready = 0;
volatile int g_ap_running_user[MAYTERA_MAX_CPUS];
int g_smp_user_sched = 0;  // #67: DEFAULT OFF. AP user-process scheduling.
//
// ---------------------------------------------------------------------------
// #67 (2026-08-12): READ THIS BEFORE CHANGING THE DEFAULT.
//
// WHAT THIS FLAG ACTUALLY DOES TODAY, MEASURED, not inherited from the note it
// replaces: it gates THE ENTIRE SMP BRING-UP. main.c only calls
// smp_start_aps() when this flag is set, so on the shipping golden NO
// APPLICATION PROCESSOR IS EVER STARTED and the whole OS - kernel jobs
// included - runs on the BSP. The previous version of this comment claimed
// "APs still run kernel jobs (smp_work_*) in parallel, so SMP is not disabled,
// only concurrent USER scheduling is". THAT IS FALSE and had been false since
// the flag was set to 0: kernel/tools/concurrency-lint/allowlist.txt already
// recorded the same finding from the other direction ("unreachable today:
// main.c gates smp_start_aps() on g_smp_user_sched, which is 0"). This is why
// a 2-vCPU VM sits at exactly 50% host CPU for hours: one core saturated, one
// core never started.
//
// THE ORIGINAL DEFECT (#421 phase 7), which was real and is NOT the
// screensaver misdiagnosis an earlier re-enable blamed it on: with this =1, an
// AP that took a user proc drove the GLOBAL ready_queue and current_proc
// concurrently with the BSP. Two things were broken, independently:
//   1. the ready queue had NO LOCK - only the #610 cli(), which masks the
//      LOCAL timer and is not a lock at all against another core; and
//   2. the scheduler published `prev` on the ready queue BEFORE
//      context_switch had saved prev->rsp, and released the BKL across the
//      switch (bkl_release_all), so another core could pop prev and switch to
//      a stale rsp. Two cores, one kernel stack, two RIPs.
// The result was a context-switch storm livelock that SILENTLY wedged the box:
// heartbeat dead, no panic, no log line. Build 912 hung; even with the
// per-cpu-current hardening (sched_cpu_current(), process.c) one boot survived
// and the next hung, so the window was narrowed, not closed.
//
// WHAT #67 CHANGED (all of it inert while this flag is 0):
//   * proc/context_switch.asm now CLEARS process_t::sched_on_cpu itself, after
//     the rsp store and after the core has left the outgoing stack. That is
//     the safe handoff: a queued entry whose flag is set is skipped, never
//     switched to. It has to live in the asm because context_start never
//     returns to its caller, so no C hook can observe "the save is done" for
//     the first-entry path.
//   * proc/process.c grew PER-CPU RUN QUEUES behind a real spinlock
//     (g_rq_lock, irqsave), with placement/steal policy in
//     rustkern/schedwatch.rs.
//   * a CONTEXT-SWITCH STORM DETECTOR that is NOT gated and runs on every
//     build, because the recorded failure mode is silence. It prints
//     [SCHEDSTORM] with the core, the rate and the processes; `make
//     SCHEDSTORMPANIC=1` turns it into a kpanic, because a panic with state
//     beats a hang. [SCHEDCORE] reports the per-core split every ~4 s.
//
// DO NOT FLIP THIS DEFAULT casually. The bar is the one #421 failed: not one
// clean boot, but many runs of a real multi-process load with no [SCHEDSTORM]
// and no wedge. Until then it is enabled per-boot, with no rebuild, by putting
// an empty /SMPSCHED.TXT at the root of the FAT ESP (main.c), so the SAME
// kernel binary can be tested both ways.
// ---------------------------------------------------------------------------
void smp_user_sched_enable(int on) { g_smp_user_sched = on ? 1 : 0; }

// #279 3b-3C: whole-kernel Big Kernel Lock so APs can run SYSCALL-making apps
// (BSP kernel code holds the BKL too, serializing against AP syscalls).
int g_smp_bkl_full = 1;     // #279: whole-kernel BKL ENABLED
extern void *smp_ap_take_migratable(void);
extern void smp_ap_run_user(void *);

void smp_cpu_local_init(uint32_t cpu) {
    if (cpu >= MAYTERA_MAX_CPUS) return;
    cpu_local[cpu].cpu_id = cpu;
    wrmsr(MSR_GS_BASE, (uint64_t)&cpu_local[cpu]);
    g_smp_current_ready = 1;
}

// Fast current-CPU id read from the GS-based per-cpu block (gs:24). Only valid
// after smp_cpu_local_init for this CPU (g_smp_current_ready).
uint32_t smp_this_cpu(void) {
    uint32_t id;
    __asm__ volatile("mov %%gs:24, %0" : "=r"(id));
    return id;
}

// Per-CPU current process (used by proc_current for SMP correctness).
void  smp_set_current(void *p) { uint32_t c = smp_this_cpu(); if (c < MAYTERA_MAX_CPUS) per_cpu_data[c].current_process = p; }
void *smp_cpu_current(uint32_t cpu) { return (cpu < MAYTERA_MAX_CPUS) ? per_cpu_data[cpu].current_process : 0; }

// Set the ring-0 stack used for the next user->kernel entry on THIS cpu:
// gs:[0] for SYSCALL, and the loaded TSS.rsp0 for interrupts/exceptions.
void cpu_set_kernel_stack(uint64_t top) {
    uint32_t cpu = smp_get_cpu_id();
    if (cpu < MAYTERA_MAX_CPUS) cpu_local[cpu].kernel_rsp = top;
    extern void gdt_set_kernel_stack(uint64_t);
    extern void gdt_set_kernel_stack_cpu(uint32_t, uint64_t);
    if (cpu == 0) gdt_set_kernel_stack(top);
    else gdt_set_kernel_stack_cpu(cpu, top);
}

// CPU ID lookup by APIC ID (for fast lookup)
static uint32_t apic_to_cpu[256];

// ============================================================================
// CPU Identification
// ============================================================================

// Get current CPU's APIC ID
uint32_t smp_get_apic_id(void) {
    return lapic_get_id();
}

// Get current CPU ID (logical ID, 0 = BSP)
uint32_t smp_get_cpu_id(void) {
    uint32_t apic_id = lapic_get_id();
    return apic_to_cpu[apic_id & 0xFF];
}

// Get total CPU count
uint32_t smp_get_cpu_count(void) {
    return cpu_count;
}

// Get online CPU count
uint32_t smp_get_online_count(void) {
    return cpus_online;
}

// Get per-CPU data for current CPU
per_cpu_t *smp_get_current_cpu(void) {
    return &per_cpu_data[smp_get_cpu_id()];
}

// Get per-CPU data by CPU ID
per_cpu_t *smp_get_cpu(uint32_t cpu_id) {
    if (cpu_id >= cpu_count) return NULL;
    return &per_cpu_data[cpu_id];
}

// Get per-CPU data by APIC ID
per_cpu_t *smp_get_cpu_by_apic(uint32_t apic_id) {
    uint32_t cpu_id = apic_to_cpu[apic_id & 0xFF];
    if (cpu_id >= cpu_count) return NULL;
    return &per_cpu_data[cpu_id];
}

// ============================================================================
// Per-CPU Data Access
// ============================================================================

void smp_set_current_process(void *process) {
    smp_get_current_cpu()->current_process = process;
}

void *smp_get_current_process(void) {
    return smp_get_current_cpu()->current_process;
}

// ============================================================================
// SMP Initialization
// ============================================================================

// Allocate per-CPU stack
static void *allocate_cpu_stack(void) {
    // Allocate pages for stack
    uint64_t pages = (SMP_STACK_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (phys == 0) {
        return NULL;
    }
    // Return pointer to stack base (stack grows down)
    return (void *)phys;
}

// Initialize BSP's per-CPU data
static void init_bsp_per_cpu(void) {
    per_cpu_t *cpu = &per_cpu_data[0];
    
    memset(cpu, 0, sizeof(*cpu));
    
    cpu->cpu_id = 0;
    cpu->apic_id = lapic_get_id();
    cpu->is_bsp = 1;
    cpu->state = CPU_STATE_ONLINE;
    
    // BSP already has its stack set up
    // Just record it
    cpu->stack_base = NULL;  // Will be set later if needed
    cpu->stack_top = 0;
    
    // GDT/TSS already initialized by gdt_init()
    cpu->gdt = NULL;  // Using global GDT for BSP
    cpu->tss = NULL;
    
    // Update lookup table
    apic_to_cpu[cpu->apic_id & 0xFF] = 0;
    
    cpus_online = 1;
    
    kprintf("[SMP] BSP initialized: CPU 0, APIC ID %u\n", cpu->apic_id);
}

// Initialize per-CPU data for an AP
static int init_ap_per_cpu(uint32_t cpu_id, uint32_t apic_id) {
    per_cpu_t *cpu = &per_cpu_data[cpu_id];
    
    memset(cpu, 0, sizeof(*cpu));
    
    cpu->cpu_id = cpu_id;
    cpu->apic_id = apic_id;
    cpu->is_bsp = 0;
    cpu->state = CPU_STATE_OFFLINE;
    
    // Allocate stack
    cpu->stack_base = allocate_cpu_stack();
    if (!cpu->stack_base) {
        kprintf("[SMP] Error: Failed to allocate stack for CPU %u\n", cpu_id);
        return -1;
    }
    cpu->stack_top = (uint64_t)cpu->stack_base + SMP_STACK_SIZE;
    
    // Update lookup table
    apic_to_cpu[apic_id & 0xFF] = cpu_id;
    
    kprintf("[SMP] AP initialized: CPU %u, APIC ID %u, stack 0x%lx\n",
            cpu_id, apic_id, cpu->stack_top);
    
    return 0;
}

// Copy trampoline code to low memory
static void setup_trampoline(void) {
    // Calculate trampoline size
    uint64_t trampoline_size = (uint64_t)(ap_trampoline_end - ap_trampoline_start);
    
    kprintf("[SMP] Trampoline: 0x%lx bytes, copying to 0x%x\n",
            trampoline_size, AP_TRAMPOLINE_ADDR);
    
    // Copy trampoline code to low memory
    memcpy((void *)(uint64_t)AP_TRAMPOLINE_ADDR, ap_trampoline_start, trampoline_size);

    // #279 CRITICAL: the AP enables paging (CR0.PG) while executing from the
    // trampoline at 0x8000 using the BSP page tables (CR3). If that low page is
    // not identity-mapped present+writable+executable, the very next instruction
    // fetch after PG=1 faults -> triple fault -> VM reset (was dying at marker 3).
    // Force-identity-map the low 1 MiB so the trampoline + its stack + GDTs are
    // guaranteed reachable under paging.
    for (uint64_t a = 0; a < 0x100000; a += 0x1000) {
        vmm_map_page(a, a, VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE);
    }
    kprintf("[SMP] low-1MiB identity-mapped; 0x8000 mapped=%d phys=0x%lx cr3=0x%lx\n",
            vmm_is_mapped(AP_TRAMPOLINE_ADDR),
            vmm_get_physical(AP_TRAMPOLINE_ADDR), read_cr3());
}

// Initialize SMP subsystem
int smp_init(void) {
    kprintf("[SMP] Initializing Symmetric Multi-Processing...\n");
    
    // Initialize APIC ID lookup table
    memset(apic_to_cpu, 0xFF, sizeof(apic_to_cpu));
    
    // Check if MADT is available
    if (!madt_is_initialized()) {
        kprintf("[SMP] Warning: MADT not initialized, assuming single CPU\n");
        cpu_count = 1;
        init_bsp_per_cpu();
        return 0;
    }
    
    // Get CPU count from MADT
    uint32_t madt_cpus = madt_get_enabled_cpu_count();
    if (madt_cpus == 0) {
        kprintf("[SMP] Warning: MADT reports 0 enabled CPUs, assuming single CPU\n");
        cpu_count = 1;
        init_bsp_per_cpu();
        return 0;
    }
    
    cpu_count = madt_cpus;
    if (cpu_count > MAYTERA_MAX_CPUS) {
        kprintf("[SMP] Warning: %u CPUs detected, limiting to %u\n",
                cpu_count, MAYTERA_MAX_CPUS);
        cpu_count = MAYTERA_MAX_CPUS;
    }
    
    kprintf("[SMP] Detected %u CPUs in MADT\n", cpu_count);
    
    // Initialize Local APIC (BSP)
    if (lapic_init() != 0) {
        kprintf("[SMP] Error: Failed to initialize Local APIC\n");
        cpu_count = 1;
        init_bsp_per_cpu();
        return -1;
    }
    
    // Initialize I/O APIC
    if (ioapic_init() != 0) {
        kprintf("[SMP] Warning: Failed to initialize I/O APIC\n");
        // Continue anyway - we can still use SMP without I/O APIC routing
    }
    
    // Initialize BSP per-CPU data
    init_bsp_per_cpu();

    // Register the AP wake-IPI handler so idle APs can HLT and be kicked awake
    // when work is submitted (vector SMP_WAKE_VECTOR in the shared IDT).
    idt_register_handler(SMP_WAKE_VECTOR, smp_wake_handler);

    // Initialize AP per-CPU data
    uint32_t bsp_apic = madt_get_bsp_apic_id();
    uint32_t cpu_id = 1;
    
    for (uint32_t i = 0; i < madt_get_cpu_count(); i++) {
        cpu_info_t *madt_cpu = madt_get_cpu(i);
        if (!madt_cpu || !madt_cpu->is_enabled) continue;
        
        // Skip BSP (already initialized)
        if (madt_cpu->apic_id == bsp_apic) continue;
        
        if (cpu_id < cpu_count) {
            if (init_ap_per_cpu(cpu_id, madt_cpu->apic_id) != 0) {
                kprintf("[SMP] Warning: Failed to initialize CPU %u\n", cpu_id);
            }
            cpu_id++;
        }
    }
    
    // Setup trampoline code
    setup_trampoline();
    
    kprintf("[SMP] SMP initialization complete: %u CPUs configured\n", cpu_count);
    return 0;
}

// ============================================================================
// AP Startup
// ============================================================================

// Start a single AP
static int start_ap(uint32_t cpu_id) {
    per_cpu_t *cpu = &per_cpu_data[cpu_id];
    
    if (cpu->state != CPU_STATE_OFFLINE) {
        return -1;  // Already started
    }
    
    kprintf("[SMP] Starting CPU %u (APIC ID %u)...\n", cpu_id, cpu->apic_id);
    
    cpu->state = CPU_STATE_STARTING;
    
    // Setup trampoline data
    trampoline_data_t *tdata = (trampoline_data_t *)(uint64_t)(AP_TRAMPOLINE_ADDR + TRAMPOLINE_DATA_OFFSET);
    
    tdata->pml4_phys = read_cr3();
    tdata->stack_top = cpu->stack_top;
    tdata->ap_entry_addr = (uint64_t)ap_entry;
    tdata->cpu_id = cpu_id;
    tdata->apic_id = cpu->apic_id;
    tdata->started = 0;
    
    // Memory barrier to ensure trampoline data is visible
    memory_barrier();
    
    // Send INIT IPI
    lapic_send_init(cpu->apic_id);
    
    // Wait 10ms
    for (volatile int i = 0; i < 10000000; i++) pause();
    
    // Send STARTUP IPI (twice, as per Intel spec)
    uint8_t sipi_vector = (AP_TRAMPOLINE_ADDR >> 12) & 0xFF;
    
    lapic_send_startup(cpu->apic_id, sipi_vector);
    
    // Wait 200us
    for (volatile int i = 0; i < 200000; i++) pause();
    
    // Check if AP started
    if (tdata->started == 0) {
        // Try again
        lapic_send_startup(cpu->apic_id, sipi_vector);
        
        // Wait for AP to start (timeout after ~1 second)
        for (int timeout = 0; timeout < 1000000 && tdata->started == 0; timeout++) {
            pause();
        }
    }
    
    if (tdata->started != 0) {
        kprintf("[SMP] CPU %u started successfully\n", cpu_id);
        return 0;
    }
    
    kprintf("[SMP] Error: CPU %u failed to start\n", cpu_id);
    cpu->state = CPU_STATE_OFFLINE;
    return -1;
}

// Start all APs
int smp_start_aps(void) {
    if (cpu_count <= 1) {
        kprintf("[SMP] Single CPU system, no APs to start\n");
        return 0;
    }
    
    kprintf("[SMP] Starting %u Application Processors...\n", cpu_count - 1);
    
    int started = 0;
    for (uint32_t i = 1; i < cpu_count; i++) {
        if (start_ap(i) == 0) {
            started++;
        }
    }
    
    kprintf("[SMP] %u/%u APs started successfully\n", started, cpu_count - 1);
    
    return started;
}

// Wait for APs to reach a state
void smp_wait_for_aps(uint8_t state) {
    for (uint32_t i = 1; i < cpu_count; i++) {
        while (per_cpu_data[i].state != state &&
               per_cpu_data[i].state != CPU_STATE_HALTED) {
            pause();
        }
    }
}

// ============================================================================
// SMP Parallel Work Pool (#279 stage 2)
// ============================================================================
// A shared, spinlock-protected ring of run-to-completion kernel jobs. Online
// APs spin-pull jobs and run them in parallel with the BSP. The BSP can also
// drain the ring via smp_work_run_one(), so jobs complete even on 1 CPU.

#define SMP_WORK_RING_SIZE 256   // power of two

static smp_job_t      smp_work_ring[SMP_WORK_RING_SIZE];
static volatile uint32_t smp_work_head = 0;   // next slot to pop
static volatile uint32_t smp_work_tail = 0;   // next slot to push
static spinlock_t     smp_work_lock = SPINLOCK_INIT;
static volatile uint64_t smp_jobs_done = 0;

int smp_work_submit(void (*fn)(void *), void *arg, volatile uint32_t *done) {
    if (!fn) return -1;
    spinlock_acquire(&smp_work_lock);
    uint32_t next = (smp_work_tail + 1) & (SMP_WORK_RING_SIZE - 1);
    if (next == smp_work_head) {            // ring full
        spinlock_release(&smp_work_lock);
        return -1;
    }
    smp_work_ring[smp_work_tail].fn   = fn;
    smp_work_ring[smp_work_tail].arg  = arg;
    smp_work_ring[smp_work_tail].done = done;
    smp_work_tail = next;
    spinlock_release(&smp_work_lock);
    // Kick any idle (HLT'd) APs so they wake up and grab the new work.
    if (cpu_count > 1) lapic_send_ipi_all_excluding_self(SMP_WAKE_VECTOR);
    return 0;
}

// Wake-IPI handler: an idle AP was HLT'd; this fires to wake it. Nothing to do
// but acknowledge the interrupt; the AP re-polls the work ring on return.
static void smp_wake_handler(interrupt_frame_t *frame) {
    (void)frame;
    per_cpu_t *cpu = smp_get_current_cpu();
    if (cpu) cpu->ipi_received++;
    lapic_eoi();
}

// Pop one job under the lock; returns 1 + fills *out, or 0 if empty.
static int smp_work_pop(smp_job_t *out) {
    int got = 0;
    spinlock_acquire(&smp_work_lock);
    if (smp_work_head != smp_work_tail) {
        *out = smp_work_ring[smp_work_head];
        smp_work_head = (smp_work_head + 1) & (SMP_WORK_RING_SIZE - 1);
        got = 1;
    }
    spinlock_release(&smp_work_lock);
    return got;
}

// Execute a popped job on the calling CPU and signal completion.
static void smp_run_job(const smp_job_t *j) {
    j->fn(j->arg);
    atomic_inc64(&smp_jobs_done);
    if (j->done) atomic_store32((volatile uint32_t *)j->done, 1);
}

// #279 3b-3: kick HLT'd APs so they re-check the migratable queue.
void smp_wake_aps(void) { if (cpu_count > 1) lapic_send_ipi_all_excluding_self(SMP_WAKE_VECTOR); }

// #67 pass 8: DIRECTED wake, to ONE core.
//
// smp_wake_aps() broadcasts to every other core. That was harmless when the APs
// only drained a job ring, and became a feedback loop once they run the
// scheduler: each core's scheduling activity interrupts every other core, whose
// wake handler takes the BKL (idt.c wraps every ISR in bkl_acquire), whose
// scheduling then interrupts the first core back. MEASURED on build 254 with
// broadcast wakes: a single BKL hold of 2,886,374 us attributed to vector 0xF0 -
// this very handler - alongside 111,884,639 spin iterations in one window. 2.9
// seconds is far longer than the handler runs, so it is a NEST: the hold is
// measured from depth 0->1 to 1->0, and IPIs arriving faster than the nest
// unwinds keep the depth off zero for seconds at a time.
//
// Waking exactly the core that has work, and only when it is actually halted,
// removes the loop at its source.
void smp_wake_cpu(uint32_t cpu) {
    if (cpu_count <= 1 || cpu >= cpu_count) return;
    if (cpu == smp_this_cpu()) return;          // never IPI yourself
    lapic_send_ipi(per_cpu_data[cpu].apic_id, SMP_WAKE_VECTOR);
}

int smp_work_run_one(void) {
    smp_job_t j;
    if (smp_work_pop(&j)) { smp_run_job(&j); return 1; }
    return 0;
}

uint64_t smp_work_pending(void) {
    spinlock_acquire(&smp_work_lock);
    uint32_t h = smp_work_head, t = smp_work_tail;
    spinlock_release(&smp_work_lock);
    return (uint64_t)((t - h) & (SMP_WORK_RING_SIZE - 1));
}

uint64_t smp_work_completed(void) { return smp_jobs_done; }

// ---- smp_parallel_for: split [start,end) into per-core chunks, run them on the
// pool + the caller, wait for all. The range function MUST only touch KERNEL
// memory: APs run on the kernel address space and do NOT have the caller
// process's user mappings, so user buffers must be staged through kmalloc'd
// kernel memory. (#279 stage 3a work offload.) smp_range_fn is declared in smp.h.
struct smp_chunk { smp_range_fn fn; void *ctx; int s, e; };
static void smp_chunk_run(void *p) { struct smp_chunk *c = p; c->fn(c->s, c->e, c->ctx); }

void smp_parallel_for(int start, int end, smp_range_fn fn, void *ctx) {
    if (!fn || end <= start) return;
    int n = (int)cpu_count;
    if (n < 1) n = 1;
    // #143: was a bare literal 16 here and in the two array bounds below, a
    // third independent answer to "how many CPUs" that agreed with neither the
    // scheduler cap of 8 nor the per-CPU array cap of 256. On a 20-core machine
    // it silently fanned out to 16. Routed through the one cap.
    if (n > MAYTERA_MAX_CPUS) n = MAYTERA_MAX_CPUS;
    int total = end - start;
    if (n <= 1 || total < n) { fn(start, end, ctx); return; }   // not worth splitting

    struct smp_chunk ch[MAYTERA_MAX_CPUS];
    volatile uint32_t done[MAYTERA_MAX_CPUS];
    int per = (total + n - 1) / n;
    int nj = 0;
    for (int i = 0; i < n; i++) {
        int s = start + i * per;
        if (s >= end) break;
        int e = s + per;
        if (e > end) e = end;
        ch[nj].fn = fn; ch[nj].ctx = ctx; ch[nj].s = s; ch[nj].e = e;
        done[nj] = 0;
        nj++;
    }
    // Submit all chunks but the first to the pool; run the first on this CPU.
    for (int i = 1; i < nj; i++)
        while (smp_work_submit(smp_chunk_run, &ch[i], &done[i]) != 0)
            smp_work_run_one();
    smp_chunk_run(&ch[0]);
    done[0] = 1;
    // Help drain the queue and wait for every chunk to finish.
    int all = 0;
    while (!all) {
        smp_work_run_one();
        all = 1;
        for (int i = 0; i < nj; i++)
            if (!done[i]) { all = 0; break; }
    }
}

// ---- per-core CPU utilization accounting (#279 per-core meters) ----
// APs get no timer tick (they HLT), so we measure each AP's BUSY time by the
// global timer_ticks elapsed while it runs a job. The BSP windows this once per
// ~1s (from sched_tick) into a smoothed 0-100% per core. Core 0 (BSP) uses the
// existing aggregate CPU% (proc_get_cpu_usage), which already measures the BSP.
static volatile uint64_t g_core_busy_ticks[MAYTERA_MAX_CPUS];   // cumulative AP busy ticks
static uint64_t g_core_busy_last[MAYTERA_MAX_CPUS];             // snapshot at last window
static uint64_t g_core_win_last = 0;                        // timer_ticks at last window
static int g_core_pct[MAYTERA_MAX_CPUS];                        // smoothed 0-100 per core

// Called from sched_tick once per CPU% window (BSP). bsp_pct = aggregate CPU%.
void smp_account_core_usage(int bsp_pct) {
    extern volatile uint64_t timer_ticks;
    uint64_t now = timer_ticks;
    uint64_t win = now - g_core_win_last;
    g_core_win_last = now;
    if (win == 0) win = 1;

    g_core_pct[0] = bsp_pct;   // BSP measured by the existing aggregate meter
    for (uint32_t i = 1; i < cpu_count && i < MAYTERA_MAX_CPUS; i++) {
        uint64_t cur = g_core_busy_ticks[i];
        uint64_t db = cur - g_core_busy_last[i];
        g_core_busy_last[i] = cur;
        int pct = (int)(db * 100 / win);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        g_core_pct[i] = (g_core_pct[i] * 2 + pct) / 3;   // EMA smoothing
        if (i < MAYTERA_MAX_CPUS && g_ap_running_user[i]) g_core_pct[i] = 100;
    }
}

int smp_get_core_count(void) { return (int)cpu_count; }
int smp_get_core_pct(uint32_t cpu_id) {
    return (cpu_id < MAYTERA_MAX_CPUS) ? g_core_pct[cpu_id] : 0;
}

// ---- built-in parallel self-test ----
static volatile uint64_t g_smp_test_cpu_jobs[MAYTERA_MAX_CPUS];
static volatile uint64_t g_smp_test_results[64];

static void smp_test_job(void *arg) {
    uint64_t idx = (uint64_t)arg;
    uint32_t cpu = smp_get_cpu_id();
    if (cpu < MAYTERA_MAX_CPUS) atomic_inc64(&g_smp_test_cpu_jobs[cpu]);
    // A chunk of real integer compute so the job lasts long enough that more
    // than one CPU gets to participate.
    volatile uint64_t acc = 0;
    for (uint64_t i = 0; i < 60000000UL; i++) acc += (i ^ idx) + (i * 3u);
    g_smp_test_results[idx & 63] = acc;
}

void smp_selftest(void) {
    if (cpu_count <= 1) {
        kprintf("[SMP] selftest skipped (single CPU)\n");
        return;
    }
    const int N = 16;
    volatile uint32_t done[16];
    for (int i = 0; i < N; i++) done[i] = 0;
    for (uint32_t c = 0; c < MAYTERA_MAX_CPUS; c++) g_smp_test_cpu_jobs[c] = 0;

    kprintf("[SMP] selftest: dispatching %d parallel compute jobs across %u CPUs...\n",
            N, cpu_count);

    for (uint64_t i = 0; i < (uint64_t)N; i++) {
        // If the ring is momentarily full, run one here to make room.
        while (smp_work_submit(smp_test_job, (void *)i, &done[i]) != 0) {
            smp_work_run_one();
        }
    }

    // BSP helps drain the queue, then waits for all jobs to finish.
    int all_done = 0;
    while (!all_done) {
        smp_work_run_one();
        all_done = 1;
        for (int i = 0; i < N; i++) {
            if (!done[i]) { all_done = 0; break; }
        }
    }

    kprintf("[SMP] selftest complete: %lu jobs done. Per-CPU breakdown:\n",
            smp_work_completed());
    for (uint32_t c = 0; c < cpu_count; c++) {
        kprintf("[SMP]   CPU %u executed %lu job(s)%s\n",
                c, g_smp_test_cpu_jobs[c], (c != 0 && g_smp_test_cpu_jobs[c] > 0)
                ? "  <-- AP ran work in parallel!" : "");
    }
}

// ============================================================================
// AP Entry Point
// ============================================================================

// C entry point for APs (called from trampoline)
void ap_entry(void) {
    // Get our CPU info from trampoline data
    trampoline_data_t *tdata = (trampoline_data_t *)(uint64_t)(AP_TRAMPOLINE_ADDR + TRAMPOLINE_DATA_OFFSET);
    
    uint32_t cpu_id = tdata->cpu_id;
    per_cpu_t *cpu = &per_cpu_data[cpu_id];
    
    // Signal that we reached C code
    tdata->started = 1;
    memory_barrier();
    
    // Initialize this CPU's Local APIC
    lapic_init_ap();

    // #279 stage 1: load proper descriptors so this core can take
    // exceptions/interrupts without triple-faulting (it had none).
    gdt_init_ap(cpu_id, cpu->stack_top);  // #279 3b-1: per-CPU TSS+GDT (was gdt_load_ap)
    smp_cpu_local_init(cpu_id);           // #279 3b-1.5: GS base for this AP
    { extern void syscall_init(void); syscall_init(); }  // #279 3b-3C: per-CPU SYSCALL MSRs so this AP can execute the syscall instruction
    idt_load_ap();

    // Enable SSE/FXSR on this AP (CR0/CR4 are per-CPU) so any kernel code a job
    // calls that touches XMM (memcpy fast paths, etc.) does not #UD.
    sse_init();

    // #429: enable EFER.NXE on this AP too. EFER is per-CPU; a page whose PTE
    // has NX=1 would raise a reserved-bit #PF on any core that lacks NXE.
    { extern void cpu_enable_nx(void); cpu_enable_nx(); }

    // #19/#645/#624: CR4 IS PER-CPU, AND SO ARE SMEP AND SMAP.
    //
    // security_init() runs once, on the BSP, and sets CR4.SMEP|CR4.SMAP there.
    // Every AP came up with its own CR4 and had NEITHER bit, so on CPU 1..N the
    // kernel could execute AND read/write user pages freely while the boot
    // banner said "SMEP: ENABLED" and "SMAP: ENABLED". Half the cores were
    // unprotected and nothing said so.
    //
    // MEASURED, not inferred: `make SMAPTEST=1` arms a deliberate unsanctioned
    // Ring-0 read of a user page in exec/elf.c. With SMAP armed and this block
    // absent, that read SUCCEEDED and printed
    //   "[SMAPTEST] FAILED: read returned 0xA5"
    // because the ELF load happened to run on the AP. That is the whole reason
    // a negative control exists: every positive result before it was consistent
    // with the guard being off.
    //
    // The bits are taken from the BSP's OWN live CR4 rather than re-derived
    // from CPUID and policy, so an AP can never end up with a bit the BSP
    // declined (the /NOSMEP.TXT and /NOSMAP.TXT escape hatches and the CR4
    // readback that gates them keep working, unchanged, for every core).
    // Read back and report, because a silently-dropped CR4 write on one core is
    // exactly the failure this comment exists about.
    {
        extern uint64_t sec_cr4_read(void);
        extern void     sec_cr4_write(uint64_t v);
        extern uint64_t g_bsp_cr4_secbits;          // security.c, BSP-owned
        const uint64_t want = g_bsp_cr4_secbits;    // (CR4_SMEP|CR4_SMAP) & bsp
        if (want) {
            uint64_t cr4 = sec_cr4_read();
            sec_cr4_write(cr4 | want);
            uint64_t got = sec_cr4_read() & want;
            if (got != want) {
                kprintf("[SECURITY] CPU %u: CR4 security bits NOT taken "
                        "(wanted 0x%llx, got 0x%llx) - this core runs WITHOUT "
                        "SMEP/SMAP\n", cpu_id,
                        (unsigned long long)want, (unsigned long long)got);
            } else {
                kprintf("[SECURITY] CPU %u: CR4 SMEP/SMAP armed (0x%llx)\n",
                        cpu_id, (unsigned long long)got);
            }
        }
    }

    // Set CPU state to online
    cpu->state = CPU_STATE_ONLINE;
    atomic_inc32(&cpus_online);

    // #169: ARM THIS CORE'S OWN PREEMPTION TICK, before interrupts are enabled.
    //
    // Armed HERE, for every AP and for its whole life, rather than inside
    // sched_ap_enter(): smp_ap_run_user() (the one-shot migration path) also
    // runs a Ring-3 process on an AP WITHOUT going through sched_ap_enter(), so
    // arming at the scheduler entry point would leave that path cooperative and
    // produce exactly the "some cores preempt, some do not" split that is
    // harder to diagnose than a uniform miss.
    //
    // COST ON A CORE WITH NOTHING TO RUN: one interrupt per tick that reaches
    // sched_tick_ap(), finds this core's idle process, credits a counter and
    // returns. It is not free - an idle AP no longer sleeps until an IPI - but
    // it BUYS a redundant always-armed wake source: the idle loop's hlt returns
    // every tick and re-polls its run queue, so a lost wake IPI now costs one
    // tick instead of stranding the queue for the rest of the boot (the exact
    // shape MEASURED at #67 pass 11). CLAUDE.md's preference order calls that
    // option 1, and prefers it to a timeout.
    { extern int tick_ap_arm(void); (void)tick_ap_arm(); }

    // Enable interrupts
    sti();

    // #279 stage 2: SMP parallel work loop. This AP pulls run-to-completion
    // kernel jobs off the shared ring and executes them in parallel with the
    // BSP. When the ring is empty it pause-spins (instant pickup; the power-
    // saving IPI/timer-wake idle is a follow-up). Scales to N cores: every AP
    // runs this same loop.
    kprintf("[SMP] CPU %u online, entering SMP work loop\n", cpu_id);
    g_ap_heartbeat[cpu_id] = 1;
    extern volatile uint64_t timer_ticks;
    while (!cpu->should_halt) {
        smp_job_t j;
        if (smp_work_pop(&j)) {
            uint64_t t0 = timer_ticks;        // measure busy time for per-core meter
            smp_run_job(&j);
            g_core_busy_ticks[cpu_id] += timer_ticks - t0;
            cpu->running_time++;
            g_ap_heartbeat[cpu_id]++;
        } else {
            if (g_smp_user_sched) {
                // #67 pass 2: become a real scheduler consumer and never come
                // back. Before this, an AP only ever ran smp_work_* jobs and the
                // one-shot migq, so anything the scheduler placed on this core's
                // run queue was never popped and the process silently never ran.
                // sched_ap_enter() creates this core's own idle process (the
                // global proc_table[0] fallback is unsafe for two cores),
                // publishes it, and runs the idle loop.
                //
                // #169 CORRECTION: this comment used to end "the timer tick on
                // this core preempts it into work exactly as on the BSP". THERE
                // WAS NO TIMER TICK ON THIS CORE - that is the whole of #169,
                // and sched_ap_enter()'s own comment said so twenty lines away.
                // A comment asserting the mechanism a ticket exists because it
                // is missing is worse than none: it is what a reader checks
                // FIRST. There is one now (tick_ap_arm(), vector 0x42, armed
                // below), and the idle loop drives itself regardless.
                { extern void sched_ap_enter(uint32_t cpu); sched_ap_enter(cpu_id); }
                // Only reached if this core could not become a consumer; fall
                // through to the kernel-job-only loop rather than spinning.
            }
            // No work: sleep at ~0% CPU until a wake IPI (sent by
            // smp_work_submit) kicks us. cli + re-check closes the lost-wakeup
            // race; the STI before HLT has a 1-instruction grace so a pending
            // wake IPI is taken right after HLT, then we loop and re-poll.
            cpu->idle_time++;
            __asm__ volatile("cli");
            if (smp_work_pending() == 0 && !cpu->should_halt) {
                __asm__ volatile("sti; hlt");
            } else {
                __asm__ volatile("sti");
            }
        }
    }
    ap_idle();
}

// AP idle loop
void ap_idle(void) {
    per_cpu_t *cpu = smp_get_current_cpu();
    cpu->state = CPU_STATE_IDLE;
    
    while (!cpu->should_halt) {
        // Enable interrupts and halt until next interrupt
        __asm__ volatile(
            "sti\n\t"
            "hlt\n\t"
            "cli"
        );
        
        cpu->idle_time++;
        
        // Check if we should run something
        if (cpu->current_process) {
            cpu->state = CPU_STATE_ONLINE;
            // Schedule would run here
            cpu->state = CPU_STATE_IDLE;
        }
    }
    
    cpu->state = CPU_STATE_HALTED;
    
    // Halt forever
    cli();
    while (1) {
        hlt();
    }
}

// ============================================================================
// Inter-Processor Interrupts
// ============================================================================

void smp_send_reschedule(uint32_t cpu_id) {
    per_cpu_t *cpu = smp_get_cpu(cpu_id);
    if (cpu && cpu->state == CPU_STATE_ONLINE) {
        lapic_send_ipi(cpu->apic_id, IPI_VECTOR_RESCHEDULE);
    }
}

void smp_send_reschedule_all(void) {
    lapic_send_ipi_all_excluding_self(IPI_VECTOR_RESCHEDULE);
}

// ############################################################################
// # DANGER - THIS FUNCTION HAS ZERO CALLERS AND DOES NOT DO WHAT IT IS NAMED. #
// ############################################################################
//
// Audited #404 (2026-08-23). It is harmless TODAY only because g_smp_user_sched
// is 0 (smp.c above), which gates ALL AP bring-up, so no other CPU is ever
// started and the broadcast below reaches nobody. Whoever enables AP scheduling
// inherits stale-TLB memory corruption unless cross-CPU invalidation exists
// FIRST. Do NOT wire this in as it stands. Three things are missing, measured:
//
//  1. THERE IS NO RECEIVER. IPI_VECTOR_TLB is 0xF2 (242) and cpu/idt.c never
//     registers a gate for it: idt_init() memsets the table and then installs
//     specific vectors only (240, 0x41, 0x42, 0x50, 0x51). Delivering 242 to a
//     live AP hits a not-present IDT entry and raises #GP, error 0x792. This is
//     the exact failure idt.c documents for vector 0x41.
//     Same hole: IPI_VECTOR_CALL (0xF1) and IPI_VECTOR_STOP (0xF3) have no
//     gates either. Only IPI_VECTOR_RESCHEDULE (0xF0) lands on a registered
//     gate, and it is worth checking whether that is intentional.
//  2. THE ADDRESS IS NEVER TRANSMITTED. An IPI carries a vector and nothing
//     else. Even with a handler, the receiving CPU cannot know which page to
//     invalidate: the "request structure" of step 1 in the comment below was
//     never written.
//  3. THERE IS NO ACKNOWLEDGEMENT. Step 4 was never written either, and a
//     shootdown without quiescence is worse than none. vma_teardown_pages()
//     (mm/demand.c) unmaps and then immediately returns the frame to the PMM;
//     a peer still holding a stale TLB entry writes through it into whatever
//     that frame gets reallocated to. That is the #628 corruption class.
//
// A CORRECT VERSION IS NOT A SMALL PATCH, and it cannot be shaped like this:
// callers of this would be under mm_lock(), an irqsave spinlock, so interrupts
// are OFF. Spinning there for a peer acknowledgement deadlocks if the peer is
// itself waiting on that same mm_lock or on the BKL, and the ack spin would
// also be a new concurrency-lint violation that cannot use wait_event() because
// wq_assert_may_block() correctly refuses an interrupts-off context. It also
// needs a per-mm cpumask: lapic_send_ipi_all_excluding_self() hits every core
// whether or not it is running this address space.
//
// BOTTOM LINE, worth more than the code: THIS KERNEL HAS NO CROSS-CPU TLB
// INVALIDATION OF ANY KIND. That is a hard prerequisite for flipping
// g_smp_user_sched, and single-CPU correctness today rests on
// vmm_space_is_live() + vmm_invlpg() in mm/vmm.c, which are BSP-local.
void smp_tlb_shootdown(uint64_t virt_addr) {
    // For full implementation, we would:
    // 1. Set up TLB shootdown request structure
    // 2. Send IPI to all other CPUs
    // 3. Each CPU invalidates the page
    // 4. Wait for acknowledgment
    
    // Simple version: just send IPI
    lapic_send_ipi_all_excluding_self(IPI_VECTOR_TLB);
    
    // Invalidate locally
    vmm_invlpg(virt_addr);
}

void smp_halt_all(void) {
    // Mark all CPUs for halt
    for (uint32_t i = 1; i < cpu_count; i++) {
        per_cpu_data[i].should_halt = 1;
    }
    
    // Send stop IPI
    lapic_send_ipi_all_excluding_self(IPI_VECTOR_STOP);
}

// ============================================================================
// Kernel Lock
// ============================================================================

// ---- Recursive Big Kernel Lock (#279 stage 3b-2) --------------------------
// Owner-aware, re-entrant giant lock. A CPU that already holds it (e.g. a timer
// IRQ nested inside a syscall on the same CPU) just bumps a depth counter instead
// of deadlocking. Used by stage 3b-3 to serialize kernel execution across cores
// when APs run user processes. The scheduler must RELEASE this around
// context_switch (a hold belongs to the CPU, not the process) - see 3b-3.
static volatile uint32_t bkl_word = 0;     // 0 = free, 1 = held
static volatile int32_t  bkl_owner = -1;   // owning cpu id, -1 = none
static volatile uint32_t bkl_depth = 0;    // recursion depth

// #67 pass 3: BKL CONTENTION INSTRUMENTATION.
//
// Making an AP a real scheduler consumer made the whole guest about 5x slower
// (MEASURED: build 247, AP started but not scheduling, reached 233 s of guest
// uptime in a 240 s capture; build 249, AP scheduling, reached 48 s). The
// obvious suspect is this lock: with g_smp_bkl_full = 1 every kernel entry on
// either core serialises here, and a second core that actually runs the
// scheduler enters the kernel constantly. That is a HYPOTHESIS, and #67 has
// already cost one pass to a plausible-but-wrong story, so it gets measured
// before anything is narrowed. These counters are reported in [SCHEDCORE].
//
// #67 pass 5: THE COUNTERS ARE NOW PER-CPU. Pass 4 already caught the spin
// counter being 93% of its own reading; the acquire counter had the same defect
// in a quieter form, because g_bkl_acquires++ ran on EVERY kernel entry on both
// cores - a read-modify-write on one shared cacheline, bouncing it between cores
// thousands of times a second. Per-CPU cells are summed only when the report
// prints, so the measured path touches nothing another core is touching.
//
// g_bkl_hold_max/reason answer the question the pass-4 numbers raised but could
// not settle: ~68 contended acquisitions per window burning ~7.5M pause
// iterations is roughly 110k spins per wait, which is milliseconds. That is not
// "the lock is a bit hot", it is "somebody holds it for a whole time slice".
// This records the LONGEST hold in each window together with a tag saying what
// was running, so the narrowing can be aimed instead of guessed.
//
// #166: ONE definition of how many CPUs, taken from cpu/cpumax.h. This constant
// was the literal `8` while MAYTERA_MAX_CPUS was 32 - the SIXTH independent
// answer to "how many CPUs" in this tree, and the one #143 did not find when it
// consolidated the other five.
//
// Every WRITE below was range-checked against the 8, so no store ever went out
// of bounds. What the mismatch produced instead were two SILENT faults that
// together made this instrument print arithmetic garbage on any machine with
// more than eight cores:
//
//  (1) TRUNCATION. On a 12-vCPU boot, cores 8..11 recorded NOTHING - no
//      acquires, no contention, no hold time, no reason tag, because
//      bkl_set_reason(), bkl_set_syscall(), bkl_irq_mark() and every counter
//      store return early for cpu >= BKL_STAT_CPUS. Every BKL total was LOW by
//      a third and no line said so. This is exactly the failure cpumax.h's own
//      header comment describes and was written to end.
//
//  (2) AN OUT-OF-BOUNDS READ, AND AN OUT-OF-BOUNDS WRITE, from the reader.
//      sched_smp_report() summed `i < sched_rq_ncpu()`, a bound derived from
//      MAYTERA_MAX_CPUS, over arrays sized by THIS constant. MEASURED .bss
//      layout of the pre-fix build (dev 7ed21fc7), in address order:
//
//        g_bkl_long  g_bkl_hold_sum  g_bkl_hold_reason  g_bkl_hold_max
//        g_bkl_spin_pc  g_bkl_con_pc  g_bkl_acq_pc
//
//      so at n=12 the report read g_bkl_hold_sum[8..11] out of
//      g_bkl_hold_reason[0..7] (two 32-bit tags packed per u64),
//      g_bkl_con_pc[8..11] out of g_bkl_acq_pc[0..3], g_bkl_long[8..15] out of
//      g_bkl_hold_sum[0..7] - and WROTE ZERO into g_bkl_spin_pc[0..3] through
//      `g_bkl_hold_max[i] = 0` every window, destroying the spin counters it
//      then printed a delta of.
//
//      That is the whole of #166's headline number. The reported
//      held=18446743107341936826 us decomposes EXACTLY as -(225 << 32) + 26810:
//      26810 us of real forward hold movement, minus 225 in the HIGH 32 bits of
//      a packed reason pair, i.e. one g_bkl_hold_reason[] entry changing by 225
//      (0x0201 -> 0x0120 is such a change: syscall 1 to timer vector 0x20).
//      A microsecond accumulator does not move in units of 2^32. A mis-read tag
//      word does. Reproduced on a 12-vCPU boot of the unmodified tree before
//      this change: held=18446744073709550388us, bkl=3949/8795c (contended
//      exceeding acquires), maxhold=143860845us (143 seconds inside a 4-second
//      window), long=3878413 (millions of over-1ms holds in the same window).
#define BKL_STAT_CPUS MAYTERA_MAX_CPUS
_Static_assert(BKL_STAT_CPUS == MAYTERA_MAX_CPUS,
    "#166: the per-CPU BKL counters must be sized by cpu/cpumax.h and by "
    "nothing else. A second, independent answer to 'how many CPUs' is what "
    "produced the out-of-bounds summation this constant now prevents.");
volatile uint64_t g_bkl_acq_pc[BKL_STAT_CPUS];    // acquires, per cpu
volatile uint64_t g_bkl_con_pc[BKL_STAT_CPUS];    // contended, per cpu
// #166: RECURSIVE takes - a core asking for a lock it already owns, which just
// bumps bkl_depth. These cannot contend, so counting them in g_bkl_acq_pc made
// the denominator of the contention ratio a different quantity from what the
// numerator counted. Their own counter, so nothing is lost by moving them out.
volatile uint64_t g_bkl_rec_pc[BKL_STAT_CPUS];    // re-entrant takes, per cpu
volatile uint64_t g_bkl_spin_pc[BKL_STAT_CPUS];   // pause iterations, per cpu
volatile uint64_t g_bkl_hold_max[BKL_STAT_CPUS];  // longest hold this window, us
volatile uint32_t g_bkl_hold_reason[BKL_STAT_CPUS]; // tag of that longest hold
volatile uint64_t g_bkl_hold_sum[BKL_STAT_CPUS];  // total us held, per cpu
volatile uint64_t g_bkl_long[BKL_STAT_CPUS];      // holds over 1 ms, per cpu

// #67 pass 9: WHICH CORE held the lock longest, and did that hold begin on the
// far side of a context switch.
//
// Pass 8 measured a 2.9-second hold tagged to the SMP wake vector and I very
// nearly acted on it. No interrupt handler runs for 2.9 seconds; an absurd
// reading is an instrument fault first and a discovery second. The tag was only
// ever "whatever entry point was on the stack", which does not identify the
// core, and the core is the whole question: the BSP has a 250 Hz PIT tick that
// bounds any hold to a time slice, and an AP has NO periodic timer at all, so a
// process holding the lock there runs until it voluntarily blocks.
//
// hold_from_switch records whether the hold began in bkl_reacquire() (i.e. the
// far side of a context switch) rather than at a genuine kernel entry, which is
// exactly the conflation the previous instrument could not express.
volatile uint32_t g_bkl_hold_cpu = 0;
volatile uint32_t g_bkl_hold_from_switch = 0;
static uint8_t bkl_hold_is_switch[BKL_STAT_CPUS];

// What this cpu is currently doing inside the kernel. Set by the two entry
// points that take the BKL (the ISR wrapper and the syscall dispatcher) so a
// long hold can be attributed to a VECTOR or a SYSCALL NUMBER rather than to a
// return address that is always one of the same two wrappers.
//   0x0100 | vector   - interrupt
//   0x0200 | syscall  - syscall
//   0x0300            - kernel thread body / other
volatile uint32_t g_bkl_reason[BKL_STAT_CPUS];
void bkl_set_reason(uint32_t r) {
    uint32_t c = smp_this_cpu();
    if (c < BKL_STAT_CPUS) g_bkl_reason[c] = r;
}

// Hold-time bookkeeping, per cpu, touched only on the 0->1 and 1->0 edges.
static uint64_t bkl_hold_start[BKL_STAT_CPUS];
// #143 re-measure: THE PID THAT OWNED THE HOLD, SNAPSHOT AT ACQUIRE.
//
// THIS ARRAY EXISTS BECAUSE THE OBVIOUS VERSION WAS WRONG, AND MEASURABLY SO.
// The first cut read the current pid in bkl_hold_account(), i.e. at RELEASE.
// proc/process.c publishes the incoming task into the per-cpu current slot at
// line 4084 (smp_set_current(next), current_proc = next), which is BEFORE the
// bkl_release_all() at :4190/:4220 that ends the outgoing task's hold. So every
// hold was credited to the thread that came NEXT, not the one that held it.
//
// It did not look broken. It produced a plausible, confidently wrong answer:
// on a 4-vCPU boot whose CPU profile was top=dos:51, the "top BKL holders" came
// out as xhci_evt 78s / netpump 25s / haservice 13s, with the dos thread absent
// from the list entirely - because dos's holds were being credited to whichever
// service thread the scheduler picked after it. Sampled at ACQUIRE the owner is
// unambiguous: bkl_take_locked() runs before any publish for the next switch,
// and for a reacquire it runs on the far side of context_switch(), where the
// resumed thread is already current.
static uint32_t bkl_hold_pid[BKL_STAT_CPUS];

// ===========================================================================
// #118: NAME THE HOLDER, AND PROVE IT WAS RUNNING.
//
// Two things the previous instrument could not do, and both of them are why
// "maxhold=446195us@0x120" was unactionable.
//
// (1) WHO. The @0xNNN tag is read at RELEASE out of g_bkl_reason[cpu], which
//     every ISR and syscall ENTRY overwrites and nobody restores. Any hold
//     longer than one timer period therefore reports 0x120 (vector 0x20, the
//     timer), because at 250 Hz the timer is simply what ticked most recently.
//     That is every hold over 4 ms - exactly the ones worth reporting. The
//     depth-0 acquire's return address, already recorded for the #75 halt
//     forensics, does name the holder and cannot be clobbered by a nested IRQ.
//
// (2) WHETHER IT WAS EVEN EXECUTING. mono_us() is TSC-backed real time, so it
//     keeps advancing while this core is halted or while the hypervisor has
//     descheduled the vCPU. A "hold" measured across either is elapsed time
//     the lock was owned but nothing was running - very different from a long
//     critical section, and it needs a completely different fix. With IF set
//     the timer alone guarantees an interrupt entry every 4 ms, so the worst
//     gap between interrupt entries DURING a hold separates the two cases: a
//     gap near the tick period means the core really was executing throughout;
//     a gap near the hold length means it was not running at all.
// ===========================================================================
static uint64_t bkl_irq_last[BKL_STAT_CPUS];    // mono_us() at the last IRQ entry
static uint64_t bkl_irq_gapmax[BKL_STAT_CPUS];  // worst such gap during THIS hold
// WHICH SYSCALL. g_bkl_reason[] cannot answer this: cpu/idt.c overwrites it on
// every interrupt entry, which is exactly the defect this ticket is about. This
// one is written ONLY by syscall_dispatch(), so an interrupt arriving mid-hold
// cannot touch it, and for a hold taken by proc/syscall.asm it names the
// syscall that was running.
volatile uint64_t g_bkl_syscall[BKL_STAT_CPUS];
void bkl_set_syscall(uint64_t n) {
    uint32_t c = smp_this_cpu();
    if (c < BKL_STAT_CPUS) g_bkl_syscall[c] = n;
}

// Mirrors rustkern/bklsite.rs BklSite. Locked so the FFI cannot drift.
typedef struct { uint64_t ra, count, total_us, max_us, max_gap_us, worst_syscall; } bklsite_t;
_Static_assert(sizeof(bklsite_t) == 48, "bklsite: FFI struct layout");
#define BKLSITE_N 48u
bklsite_t g_bkl_sites[BKLSITE_N];
volatile uint64_t g_bklsite_drops;
// #143 re-measure: THE SAME TABLE, KEYED ON THE PROCESS INSTEAD OF THE ADDRESS.
//
// WHY A SECOND TABLE AND NOT A SECOND INSTRUMENT. Measured on build 1972, the
// g_bkl_sites table answers "which call site" with 99% of ALL hold time at ONE
// address: proc/process.c:4220, the bkl_reacquire() on the far side of
// context_switch(). That is a true answer and a useless one. Every kernel
// thread takes the BKL in proc_wrapper() and never drops it, so the scheduler's
// retake covers the resumed thread's ENTIRE residency; the call site is
// therefore "whoever is running", by construction, for any workload.
//
// The holder that a narrowing effort needs named is the PROCESS. Keying the
// identical accounting on the pid answers that, and #118's table already does
// the merging, the ranking and the overflow reporting, with a self-test. A
// private copy of that logic would be the forked-primitive mistake this project
// has a standing rule against; this is one line of new state.
//
// KEY = pid + 1. bklsite_add() reserves 0 as its free-slot marker and folds a
// zero key into bucket 1, so a raw pid 0 would be indistinguishable from an
// unattributed sample. The report subtracts the 1 back.
bklsite_t g_bkl_pids[BKLSITE_N];
volatile uint64_t g_bklpid_drops;
extern int      bklsite_add(bklsite_t *tab, uint32_t n, uint64_t ra, uint64_t us,
                            uint64_t gap_us, uint64_t syscall);

// Called from the ISR wrapper (cpu/idt.c) on every interrupt entry, after the
// BKL is (re)acquired. One per-cpu store, no shared cacheline, no I/O.
void bkl_irq_mark(void) {
    uint32_t c = smp_this_cpu();
    if (c >= BKL_STAT_CPUS) return;
    if (!bkl_hold_start[c]) return;      // no hold in progress: nothing to time
    uint64_t now  = mono_us();
    uint64_t prev = bkl_irq_last[c];
    if (prev && now > prev) {
        uint64_t gap = now - prev;
        if (gap > bkl_irq_gapmax[c]) bkl_irq_gapmax[c] = gap;
    }
    bkl_irq_last[c] = now;
}

// #121: how many times a BKL hold was BROKEN by a context switch, per cpu.
// A syscall's DURATION and its syscall_entry HOLD are the same interval only
// when this does not move during it, and nothing in the tree could tell those
// two apart: proc/process.c drops the lock across every switch, so a BLOCKING
// syscall ends its syscall_entry hold at the first switch and the remainder is
// charged to sched_schedule's reacquire instead. cpu/scprof.c samples this
// across every syscall so a long syscall and a long hold stop being confused.
volatile uint64_t g_bkl_brk[BKL_STAT_CPUS];

// Kept as aggregate names so existing readers still link; summed at report time.
volatile uint64_t g_bkl_acquires   = 0;
volatile uint64_t g_bkl_contended  = 0;
volatile uint64_t g_bkl_spins      = 0;

// ===========================================================================
// #745 (#75) BKL-HALT FORENSICS. Diagnostic only; changes no policy.
//
// The open defect is a core that HALTS while it owns the Big Kernel Lock: the
// other core then spins in bkl_take_locked() for ever and nothing can become
// runnable, so the halted core is never woken. An address alone says WHERE the
// core was, not WHICH acquire left the lock held, and this ticket has already
// lost three passes to inferring that. So record it: for every depth level,
// the return address of the call that took it and which entry point that was.
//
// v1 = bkl_acquire()   (idt.c ISR wrapper, proc_wrapper kernel-thread entry,
//                       syscall.asm, serial.c drain retake)
// v2 = bkl_reacquire() (the scheduler switch sites, sched_smp_report,
//                       sched_storm_note, audio.c)
//
// Costs one store per acquire on a per-cpu line; no shared cacheline, no I/O.
// ===========================================================================
#define BKL_RA_MAX 8
static void    *g_bkl_ra[BKL_STAT_CPUS][BKL_RA_MAX];
static uint8_t  g_bkl_via[BKL_STAT_CPUS][BKL_RA_MAX];

static inline void bkl_ra_set(int cpu, uint32_t depth, void *ra, uint8_t via) {
    if ((uint32_t)cpu >= BKL_STAT_CPUS) return;
    if (depth == 0 || depth > BKL_RA_MAX) return;
    g_bkl_ra[cpu][depth - 1]  = ra;
    g_bkl_via[cpu][depth - 1] = via;
}
// bkl_take_locked() publishes a whole depth at once (a reacquire restores N),
// so every level below it is attributed to the same call.
static inline void bkl_ra_fill(int cpu, uint32_t depth, void *ra, uint8_t via) {
    for (uint32_t i = 1; i <= depth && i <= BKL_RA_MAX; i++)
        bkl_ra_set(cpu, i, ra, via);
}

// Returns THIS core's BKL depth, or 0 if this core is not the owner, and
// copies out the recorded acquire sites. Reads with IRQs masked so a nested
// handler cannot change the depth under the read.
uint32_t bkl_self_forensics(void **ra_out, uint8_t *via_out, uint32_t max) {
    unsigned long fl; __asm__ volatile("pushfq; pop %0" : "=r"(fl));
    __asm__ volatile("cli");
    int cpu = (int)smp_this_cpu();
    uint32_t d = 0;
    if (bkl_owner == cpu) {
        d = bkl_depth;
        if ((uint32_t)cpu < BKL_STAT_CPUS) {
            for (uint32_t i = 0; i < max && i < d && i < BKL_RA_MAX; i++) {
                if (ra_out)  ra_out[i]  = g_bkl_ra[cpu][i];
                if (via_out) via_out[i] = g_bkl_via[cpu][i];
            }
        }
    }
    if (fl & (1UL << 9)) __asm__ volatile("sti");
    return d;
}

// #118: ONE implementation of hold accounting. This block existed VERBATIM in
// both bkl_release() and bkl_release_all(). A second copy of the same logic is
// this tree's defining defect (see the note below on what a desynchronised copy
// of bkl_acquire cost #67), and here it also meant the new instrumentation had
// to be written twice to be correct. Now it is written once.
//
// Caller must hold the lock, be the owner, and have interrupts masked.
static inline void bkl_hold_account(int cpu) {
    if ((uint32_t)cpu >= BKL_STAT_CPUS || !bkl_hold_start[cpu]) return;
    uint64_t held = mono_us() - bkl_hold_start[cpu];
    uint64_t gap  = bkl_irq_gapmax[cpu];
    bkl_hold_start[cpu] = 0;
    g_bkl_hold_sum[cpu] += held;
    if (held > 1000) g_bkl_long[cpu]++;
    if (bklsite_add(g_bkl_sites, BKLSITE_N,
                    (uint64_t)(uintptr_t)g_bkl_ra[cpu][0], held, gap,
                    g_bkl_syscall[cpu]) == 2)
        g_bklsite_drops++;          // 2 == BKLSITE_FULL: sample dropped, said so
    // #143 re-measure: the same hold, attributed to the process that held it.
    //
    // IN C, DELIBERATELY, and this is the stated reason the all-new-code-in-Rust
    // rule asks for: this runs inside bkl_release()/bkl_release_all() between an
    // inline-asm cli and sti, with interrupts masked, on every kernel entry on
    // every core. It is the same entanglement split #166 already made and
    // documented for the counter stores beside it: C for the masked store, Rust
    // for the accounting that decides anything. Every DECISION here (which slot,
    // merge or claim, ranking, overflow) is in rustkern/bklsite.rs and is
    // covered by its self-test; the new C is a key computation and a call.
    //
    // The pid was snapshot at ACQUIRE (bkl_hold_pid[], see its comment): reading
    // it here would name the INCOMING thread, which is the bug that array exists
    // to record. sched_cpu_current_pid() is lock-free by construction and MUST
    // stay that way; it is called from bkl_take_locked() with interrupts masked.
    if (bklsite_add(g_bkl_pids, BKLSITE_N,
                    (uint64_t)bkl_hold_pid[cpu] + 1, held, gap,
                    g_bkl_syscall[cpu]) == 2)
        g_bklpid_drops++;
    if (held > g_bkl_hold_max[cpu]) {
        g_bkl_hold_max[cpu]    = held;
        g_bkl_hold_reason[cpu] = g_bkl_reason[cpu];
        g_bkl_hold_cpu         = (uint32_t)cpu;
        g_bkl_hold_from_switch = bkl_hold_is_switch[cpu];
    }
}

// #67 pass 10: ONE implementation of "wait for the lock and publish ownership".
//
// THE BUG THIS DELETES. bkl_acquire() and bkl_reacquire() were near-identical
// copies of the same wait-and-publish sequence. In #67 pass 5 a mechanical
// edit that was adding per-cpu counters replaced the contended block in
// bkl_reacquire() and DROPPED ITS `for (;;)` LOOP. What was left ran the CAS
// once and then fell straight through to `bkl_owner = cpu; bkl_depth = depth;`
// - so when the compare-and-swap FAILED, meaning another core held the lock,
// this core published itself as the owner anyway. bkl_reacquire() is the
// function every context uses to get the BKL back after a context switch, so
// the Big Kernel Lock stopped being a lock.
//
// It built clean: the `spins` local was still summed, so nothing was unused,
// and the comment still said "Contended: IRQ-friendly spin", which is why the
// missing loop read as present for four passes.
//
// OBSERVED, and every one of these is this defect: two cores publishing as
// owner, so bkl_word sticks at 1 and the victim spins in bkl_acquire() forever
// (~110-115 MILLION pause iterations and a ~3.0 second "hold" in one window of
// EVERY gate-ON boot, measured identically with the console synchronous and
// asynchronous, so the console was never involved); ONE PROCESS RUNNING ON TWO
// CORES from a single proc_create(), its output interleaved character by
// character with itself; an Invalid Opcode panic at RIP=0x45 with two kfree()s
// on invalid pointers; and roughly half of all gate-ON boots failing to
// complete.
//
// The fix is not to re-add the loop to the copy. It is to have ONE copy, so a
// future edit cannot desynchronise them again - blame.md's "a duplicated
// implementation is usually the INTERFACE's fault, not the author's".
//
// ===========================================================================
// #130 A CPU ID IS ONLY VALID WHILE INTERRUPTS ARE MASKED.
//
// These two counters exist because the failure they describe is otherwise
// completely silent until the machine is dead, and because a fix nobody can
// see working is a fix nobody can trust.
//
//   g_bkl_mig_n    a contended acquire that ENTERED the wait on one core and
//                  finished it on another. This is not an error any more: it
//                  is the event the fix below handles correctly, and a non-zero
//                  count is the PROOF that the fix is being exercised rather
//                  than merely compiled in. Before the fix, every one of these
//                  published a false owner and killed the machine.
//   g_bkl_self_n   a core that entered the wait for a lock IT ALREADY OWNS.
//                  That can never complete. bkl_reacquire() is the only way in,
//                  because it is the one entry point with no owner check, so a
//                  non-zero count means a release_all/reacquire pair is
//                  mismatched. Diagnostic only; no policy is changed here.
//
// Both are on the CONTENDED path only, so the uncontended fast path is
// untouched. Recorded in memory before being printed, because a machine in
// this state usually cannot get a line out to the console.
// ===========================================================================
volatile uint64_t g_bkl_mig_n;
volatile int32_t  g_bkl_mig_from = -1;
volatile int32_t  g_bkl_mig_to   = -1;
volatile uint32_t g_bkl_mig_via;
volatile uint64_t g_bkl_mig_ra;
volatile uint64_t g_bkl_self_n;
volatile uint64_t g_bkl_self_ra;

// #130 THE CORE ID IS READ HERE, NOT PASSED IN, AND ONLY WITH IF=0.
//
// This function used to take `cpu` as a parameter. Its callers read
// smp_this_cpu() BEFORE the wait below, and the wait deliberately runs with
// interrupts ENABLED (see the #279 note inside it). So the timer can preempt a
// waiter mid-spin; cpu/idt.c wraps that ISR in bkl_acquire(), sched_tick() ->
// sched_schedule() switches the waiter out from inside it, and sched_rq_pop()
// STEALS ACROSS CORES with no affinity of any kind - so the waiter can be
// resumed ON A DIFFERENT CORE. It then finishes the spin, wins the CAS, and
// publishes `bkl_owner = <the core it is no longer running on>`.
//
// Both releases (bkl_release, bkl_release_all) early-return unless
// bkl_owner == smp_this_cpu(). So that hold can NEVER be released: bkl_word is
// stuck at 1, every core piles into the loop below, and the machine is dead
// with every core at 100% and a silent console. The recorded owner is an
// innocent core which is itself spinning - which is the contradiction that
// identified this bug (#130: owner=cpu2, and cpu2's own RIP inside this spin).
//
// The fix is NOT a test for that case. It is to stop the stale value existing:
// the id is read here, once, with interrupts masked and after the last point
// at which this context could have changed core. A value that is never carried
// across an `sti` cannot go stale.
//
// Returns with the lock held, bkl_owner == this core and bkl_depth == depth,
// IRQs masked. The caller restores its own IF.
volatile int      g_bkl_inv_cpu  = -1;
volatile int      g_bkl_inv_line = 0;
volatile uint64_t g_bkl_inv_n    = 0;

static void bkl_take_locked(uint32_t depth, uint8_t from_switch,
                            void *ra, uint8_t via) {
    int cpu;                    // #130: read below, with IF=0. Never earlier.
    int was_contended = 0;      // #166: counted with the acquire, at the bottom
    if (atomic_cas32(&bkl_word, 0, 1) != 0) {
        was_contended = 1;
        // Contended. CRITICAL (#279): we may have been entered from interrupt
        // context (idt.c wraps every ISR in bkl_acquire) with IF=0, or from a
        // Ring-3 syscall/timer with IF=1. Spinning with interrupts masked blocks
        // ALL IRQ delivery on this CPU, which deadlocks against a holder that is
        // waiting on an interrupt this CPU would service. So enable interrupts
        // WHILE waiting, and re-mask before the CAS so the word=1/owner=unset
        // window can never be observed by a handler on this CPU.
        // #130: valid only until the sti below; kept solely so the migration
        // counter has something to compare against.
        int entry_cpu = (int)smp_this_cpu();
        // #166: the contention used to be counted HERE, against entry_cpu. It
        // is now counted at the bottom of this function against the core the
        // take actually finished on, together with the acquisition. See there.
        // #130 SELF-DEADLOCK GUARD (diagnostic). Waiting for a lock this core
        // already owns can never succeed, and bkl_owner is only ever written by
        // the core it names, so this is not a racy read in the direction that
        // matters. bkl_reacquire() is the only entry point with no owner check,
        // so reaching here means a bkl_release_all()/bkl_reacquire() pair is
        // mismatched. Say so; the alternative is an unattributable wedge.
        if (bkl_owner == entry_cpu) {
            g_bkl_self_ra = (uint64_t)(uintptr_t)ra;
            g_bkl_self_n++;
            if (g_bkl_self_n == 1)
                kprintf("[BKL] SELF-WAIT: cpu %d is waiting for the BKL it "
                        "already owns (depth %u, via %u, ra=0x%lx). This can "
                        "never complete; see #130.\n",
                        entry_cpu, bkl_depth, (unsigned)via,
                        (unsigned long)(uintptr_t)ra);
        }
        // #130 INSTRUMENT: the loop below re-enables interrupts (sti) while
        // waiting for the BKL, which revokes the contract of every irqsave
        // spinlock held across this point: a timer ISR can fire on THIS core,
        // idt.c wraps every ISR in bkl_acquire(), and that scheduler path takes
        // g_rq_lock - a lock this core may already hold. Record, do not print:
        // a kprintf from inside a contended lock wait can itself take locks and
        // change the behaviour being measured.
        {
            extern volatile int g_rq_owner_cpu;
            extern volatile int g_rq_owner_line;
            int me_now = (int)smp_this_cpu();
            if (g_rq_owner_cpu == me_now) {
                g_bkl_inv_cpu  = me_now;
                g_bkl_inv_line = g_rq_owner_line;
                g_bkl_inv_n++;
            }
        }
        uint64_t spins = 0;                      // LOCAL: a shared counter here
        for (;;) {                               // would be most of its own cost
            __asm__ volatile("sti");
            while (bkl_word) { pause(); spins++; }
            __asm__ volatile("cli");
            if (atomic_cas32(&bkl_word, 0, 1) == 0) break;
        }
        // #130: WE HOLD THE LOCK AND IF IS 0, so this core cannot change under
        // the read. This is the only id that may be published.
        cpu = (int)smp_this_cpu();
        if ((uint32_t)cpu < BKL_STAT_CPUS) g_bkl_spin_pc[cpu] += spins;
        if (cpu != entry_cpu) {
            // Handled correctly now, and counted so that "the fix is exercised"
            // is a measurement rather than an assertion. One line per boot: a
            // log that carries it AND goes on to a working desktop is the whole
            // proof, because before this change every one of these events
            // published an owner that was not the core running the holder, and
            // no release could then ever match. Measured: every hung boot in the
            // #130 control arm had at least one.
            g_bkl_mig_from = entry_cpu; g_bkl_mig_to = cpu;
            g_bkl_mig_via  = via;       g_bkl_mig_ra = (uint64_t)(uintptr_t)ra;
            g_bkl_mig_n++;
            if (g_bkl_mig_n == 1)
                kprintf("[BKLMIG] a BKL waiter entered on cpu %d and finished "
                        "on cpu %d (via %u, ra=0x%lx). The owner published is "
                        "cpu %d, the core actually running; #130.\n",
                        entry_cpu, cpu, (unsigned)via,
                        (unsigned long)(uintptr_t)ra, cpu);
        }
    } else {
        // Uncontended. The caller masked interrupts before calling us and we
        // have not re-enabled them, so this context cannot have moved core.
        cpu = (int)smp_this_cpu();
    }

    // THEFT DETECTOR. Reaching here means OUR compare-and-swap moved bkl_word
    // 0 -> 1, so no other core can be the owner. If one is, the lock has been
    // taken without waiting - exactly the defect above - and continuing means
    // two cores in the kernel believing they are alone. Say so loudly: this
    // failure is otherwise silent until something far away corrupts.
    if (bkl_owner != -1 && bkl_owner != cpu) {
        kprintf("[BKL] THEFT: cpu %d took the lock while cpu %d still owns it "
                "(depth %u). The BKL is not locking; see #67 pass 10.\n",
                cpu, (int)bkl_owner, bkl_depth);
    }

    // #166: THE ACQUISITION AND ITS CONTENTION, COUNTED TOGETHER, ONCE, AGAINST
    // ONE CORE INDEX, UNDER ONE RANGE CHECK.
    //
    // `contended <= acquires` is now true BY CONSTRUCTION, per core and in
    // total: there is no path that reaches this line having contended without
    // also being an acquisition, and no future edit can separate them without
    // deleting both. That is the point - the invariant is not restored by a
    // clamp on the print, it is made unbreakable at the only place either
    // number is produced.
    //
    // IT WAS FALSE BEFORE, for two INDEPENDENT reasons, and both are #166:
    //
    //  * bkl_acquire() counted the acquire; bkl_take_locked() counted the
    //    contention. But bkl_reacquire() reaches bkl_take_locked() WITHOUT
    //    going through bkl_acquire(), and bkl_reacquire() is the scheduler's
    //    retake on the far side of EVERY context switch (proc/process.c), plus
    //    sched_smp_report()'s own retake, sched_storm_note() and audio.c. So
    //    every contended reacquire incremented `contended` and NOTHING
    //    incremented `acquires`. Under load the reacquires dominate. This needs
    //    no out-of-bounds read and no unusual core count: it is why contended
    //    exceeded acquires at 4 vCPU as well as at 12.
    //
    //  * the contention was credited to entry_cpu and the acquisition to the
    //    core the caller occupied before the wait. #130 established that those
    //    differ in practice, not in theory: the wait below runs with interrupts
    //    ENABLED and the waiter can be resumed on a different core, which
    //    g_bkl_mig_n counts. Per core the two counters then described events
    //    that happened on different cores.
    if ((uint32_t)cpu < BKL_STAT_CPUS) {
        g_bkl_acq_pc[cpu]++;
        if (was_contended) g_bkl_con_pc[cpu]++;
    }

    bkl_owner = cpu;                             // published with IF=0
    bkl_depth = depth;
    bkl_ra_fill(cpu, depth, ra, via);            // #75 forensics
    if ((uint32_t)cpu < BKL_STAT_CPUS) {
        bkl_hold_start[cpu]    = mono_us();
        { extern uint32_t sched_cpu_current_pid(void);
          bkl_hold_pid[cpu] = sched_cpu_current_pid(); }   // #143 re-measure
        bkl_hold_is_switch[cpu] = from_switch;
        bkl_irq_last[cpu]       = bkl_hold_start[cpu];   // #118
        bkl_irq_gapmax[cpu]     = 0;
    }
}

void bkl_acquire(void) {
    // #264: mask IRQs for the whole acquire so the word=1/owner=unset window can
    // never be observed by a nested IRQ on this CPU (which would deadlock with
    // bkl_word stuck at 1 and bkl_owner left at -1). We restore the caller IF at
    // the end. Re-entrant callers (already the owner) are cheap and skip this.
    unsigned long fl; __asm__ volatile("pushfq; pop %0" : "=r"(fl));
    __asm__ volatile("cli");
    // #130: AFTER the cli, not before. This read used to sit above it, so a
    // preemption in the two-instruction window could migrate this context and
    // leave the re-entrancy test below comparing bkl_owner against a core we
    // are no longer on - which decides whether we enter the critical section
    // WITHOUT the lock. Under the mask the answer cannot go stale.
    int cpu = (int)smp_this_cpu();
    if (bkl_owner == cpu) {
        // #166: a recursive take is not an acquisition of the lock and cannot
        // contend for it. It is counted separately (see g_bkl_rec_pc) so the
        // contention ratio divides by the number of takes that COULD have
        // contended. The acquire itself is counted inside bkl_take_locked().
        if ((uint32_t)cpu < BKL_STAT_CPUS) g_bkl_rec_pc[cpu]++;
        bkl_depth++;
        bkl_ra_set(cpu, bkl_depth, __builtin_return_address(0), 1);   // #75 forensics
        if (fl & (1UL << 9)) __asm__ volatile("sti");
        return;
    }
    bkl_take_locked(1, 0, __builtin_return_address(0), 1);
    if (fl & (1UL << 9)) __asm__ volatile("sti");
}

void bkl_release(void) {
    // #264: mask IRQs so the owner-read / depth-dec / word-clear is atomic w.r.t.
    // a nested handler on this CPU (which could otherwise observe a torn state).
    unsigned long fl; __asm__ volatile("pushfq; pop %0" : "=r"(fl));
    __asm__ volatile("cli");
    int cpu = (int)smp_this_cpu();
    if (bkl_owner != cpu) { if (fl & (1UL << 9)) __asm__ volatile("sti"); return; }
    if (--bkl_depth == 0) {
        bkl_hold_account(cpu);          // #118: one implementation, was inline here
        bkl_owner = -1;
        atomic_store32(&bkl_word, 0);
    }
    if (fl & (1UL << 9)) __asm__ volatile("sti");
}

// Force-drop the lock regardless of depth, returning the saved depth; used by the
// scheduler to release across a context switch. Re-take with bkl_reacquire(n).
uint32_t bkl_release_all(void) {
    // #264: atomic w.r.t. nested IRQ on this CPU (see bkl_release).
    unsigned long fl; __asm__ volatile("pushfq; pop %0" : "=r"(fl));
    __asm__ volatile("cli");
    int cpu = (int)smp_this_cpu();
    if (bkl_owner != cpu) { if (fl & (1UL << 9)) __asm__ volatile("sti"); return 0; }
    uint32_t d = bkl_depth;
    if ((uint32_t)cpu < BKL_STAT_CPUS) g_bkl_brk[cpu]++;   // #121
    bkl_hold_account(cpu);              // #118: one implementation, was inline here
    bkl_depth = 0; bkl_owner = -1;
    atomic_store32(&bkl_word, 0);
    if (fl & (1UL << 9)) __asm__ volatile("sti");
    return d;
}
void bkl_reacquire(uint32_t depth) {
    if (depth == 0) return;
    // #130: no cpu id is read here at all. bkl_take_locked() reads its own,
    // with IF=0, on the far side of the wait; anything read here would be a
    // value from before a preemption point.
    // #264: same publish-window guard as bkl_acquire. Mask IRQs across the take
    // so no nested handler can see word=1 with owner unset.
    unsigned long fl; __asm__ volatile("pushfq; pop %0" : "=r"(fl));
    __asm__ volatile("cli");
    // #67 pass 10: this WAITS now. It used to run the CAS once and publish
    // ownership regardless of the result. See bkl_take_locked().
    // from_switch=1: this hold began on the far side of a context switch, which
    // is what the /swN field in [SCHEDCORE] reports.
    bkl_take_locked(depth, 1, __builtin_return_address(0), 2);
    if (fl & (1UL << 9)) __asm__ volatile("sti");
}

// ===========================================================================
// #166: SNAPSHOT THE PER-CPU COUNTERS. See the bkl_totals_t comment in smp.h
// for why this function exists at all rather than the caller looping.
//
// The static assertion is the load-bearing part. It is not decoration: it fails
// the BUILD if any of these arrays ever stops matching the one whose sizeof
// supplies the loop bound, which is precisely the drift that produced #166. An
// assertion that can only be true is worthless; this one has already been false
// in this tree, in effect, for the whole time BKL_STAT_CPUS was 8.
// ===========================================================================
void bkl_stat_totals(bkl_totals_t *out) {
    if (!out) return;
    _Static_assert(sizeof(g_bkl_acq_pc)  == sizeof(g_bkl_con_pc)   &&
                   sizeof(g_bkl_acq_pc)  == sizeof(g_bkl_rec_pc)   &&
                   sizeof(g_bkl_acq_pc)  == sizeof(g_bkl_spin_pc)  &&
                   sizeof(g_bkl_acq_pc)  == sizeof(g_bkl_hold_sum) &&
                   sizeof(g_bkl_acq_pc)  == sizeof(g_bkl_hold_max) &&
                   sizeof(g_bkl_acq_pc)  == sizeof(g_bkl_long),
        "#166: every per-CPU BKL counter array must have the same extent as the "
        "one whose sizeof bounds this loop. If you add an array here, add it "
        "above, or the summation walks off the end of it - which is #166.");
    const uint32_t n = (uint32_t)(sizeof(g_bkl_acq_pc) / sizeof(g_bkl_acq_pc[0]));
    memset(out, 0, sizeof(*out));
    out->ncpu = n;
    for (uint32_t i = 0; i < n; i++) {
        out->acquires   += g_bkl_acq_pc[i];
        out->contended  += g_bkl_con_pc[i];
        out->recursive  += g_bkl_rec_pc[i];
        out->spins      += g_bkl_spin_pc[i];
        out->held_us    += g_bkl_hold_sum[i];
        out->long_holds += g_bkl_long[i];
        if (g_bkl_hold_max[i] > out->max_us) {
            out->max_us     = g_bkl_hold_max[i];
            out->max_cpu    = i;
            out->max_reason = g_bkl_hold_reason[i];
        }
        g_bkl_hold_max[i] = 0;   // per-window maximum, reset IN RANGE
    }
    out->max_from_switch = g_bkl_hold_from_switch;
}

// #67 pass 9: PROVE THE PROBE READS A KNOWN DURATION.
//
// Three instruments in this ticket have misled me: a spin counter on a shared
// cacheline that was 93% of its own reading, an acquire counter with the same
// defect, and a hold timer that could not say which core it was timing. The rule
// now is that a measurement gets validated before it gets believed.
//
// This takes the BKL, busies for a known number of microseconds against the
// SAME clock the probe uses, releases, and prints what the probe recorded. If
// the reported hold is not close to the requested one the probe is broken and
// says so on the console, at boot, rather than being trusted for another pass.
// Runs once, on the BSP, before any AP is scheduling, so it cannot contend.
void bkl_probe_selftest(void) {
    const uint64_t want = 5000;   // 5 ms, far above any real hold at boot
    uint32_t c = smp_this_cpu();
    if (c >= BKL_STAT_CPUS) { kprintf("[BKLPROBE] SKIPPED (cpu %u out of range)\n", c); return; }
    uint64_t save_max = g_bkl_hold_max[c];
    g_bkl_hold_max[c] = 0;
    bkl_acquire();
    bkl_set_reason(0x0300u);      // "kernel thread body / other"
    { uint64_t t0 = mono_us(); while (mono_us() - t0 < want) { /* bounded by the clock */ } }
    bkl_release();
    uint64_t got = g_bkl_hold_max[c];
    // Generous band: this is a correctness check on the probe, not a benchmark.
    if (got >= (want * 8) / 10 && got <= want * 3) {
        kprintf("[BKLPROBE] OK: asked %lu us, probe recorded %lu us on cpu %u "
                "(from_switch=%u reason=0x%x)\n", (unsigned long)want,
                (unsigned long)got, g_bkl_hold_cpu, g_bkl_hold_from_switch,
                g_bkl_hold_reason[c]);
    } else {
        kprintf("[BKLPROBE] FAILED: asked %lu us, probe recorded %lu us. The BKL "
                "hold-time probe is WRONG; do not trust maxhold readings from "
                "this build.\n", (unsigned long)want, (unsigned long)got);
    }
    g_bkl_hold_max[c] = save_max;
}

// ============================================================================
// Debug/Status
// ============================================================================

const char *smp_state_string(uint8_t state) {
    switch (state) {
        case CPU_STATE_OFFLINE:  return "offline";
        case CPU_STATE_STARTING: return "starting";
        case CPU_STATE_ONLINE:   return "online";
        case CPU_STATE_IDLE:     return "idle";
        case CPU_STATE_HALTED:   return "halted";
        default:                 return "unknown";
    }
}

void smp_print_status(void) {
    kprintf("\n[SMP] ====== CPU Status ======\n");
    kprintf("[SMP] Total: %u CPUs, Online: %u\n", cpu_count, cpus_online);
    kprintf("[SMP] %-4s  %-8s  %-3s  %-10s  %-16s\n",
            "CPU", "APIC ID", "BSP", "State", "Stack Top");
    kprintf("[SMP] ----  --------  ---  ----------  ----------------\n");
    
    for (uint32_t i = 0; i < cpu_count; i++) {
        per_cpu_t *cpu = &per_cpu_data[i];
        kprintf("[SMP] %-4u  %-8u  %-3s  %-10s  0x%lx\n",
                cpu->cpu_id,
                cpu->apic_id,
                cpu->is_bsp ? "yes" : "no",
                smp_state_string(cpu->state),
                cpu->stack_top);
    }
    
    kprintf("[SMP] ==========================\n\n");
}
