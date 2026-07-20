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
static per_cpu_t per_cpu_data[SMP_MAX_CPUS] __attribute__((aligned(4096)));

// Number of CPUs detected
static uint32_t cpu_count = 0;
volatile unsigned long g_ap_heartbeat[SMP_MAX_CPUS];  // #279 stage1 proof-of-life

// Number of CPUs online
static volatile uint32_t cpus_online = 0;

// Global kernel lock
spinlock_t kernel_lock = SPINLOCK_INIT;

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
static cpu_local_t cpu_local[SMP_MAX_CPUS] __attribute__((aligned(64)));

#define MSR_GS_BASE 0xC0000101

// Point this CPU's GS base at its cpu_local slot. Call AFTER the final gdt_load
// on the CPU (gdt_load reloads the gs selector, zeroing GS_BASE).
// #279 3b-3: set once per-CPU GS base is live so proc_current() may use the
// fast per-CPU path (before this, callers fall back to the BSP global).
volatile int g_smp_current_ready = 0;
volatile int g_ap_running_user[SMP_MAX_CPUS];
int g_smp_user_sched = 1;  // #279: SMP scheduling ENABLED (restoring b390 verified state; livelock was a screensaver misdiagnosis)
// #279 3b-3C: whole-kernel Big Kernel Lock so APs can run SYSCALL-making apps
// (BSP kernel code holds the BKL too, serializing against AP syscalls).
int g_smp_bkl_full = 1;     // #279: whole-kernel BKL ENABLED
extern void *smp_ap_take_migratable(void);
extern void smp_ap_run_user(void *);

void smp_cpu_local_init(uint32_t cpu) {
    if (cpu >= SMP_MAX_CPUS) return;
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
void  smp_set_current(void *p) { uint32_t c = smp_this_cpu(); if (c < SMP_MAX_CPUS) per_cpu_data[c].current_process = p; }
void *smp_cpu_current(uint32_t cpu) { return (cpu < SMP_MAX_CPUS) ? per_cpu_data[cpu].current_process : 0; }

// Set the ring-0 stack used for the next user->kernel entry on THIS cpu:
// gs:[0] for SYSCALL, and the loaded TSS.rsp0 for interrupts/exceptions.
void cpu_set_kernel_stack(uint64_t top) {
    uint32_t cpu = smp_get_cpu_id();
    if (cpu < SMP_MAX_CPUS) cpu_local[cpu].kernel_rsp = top;
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
    if (cpu_count > SMP_MAX_CPUS) {
        kprintf("[SMP] Warning: %u CPUs detected, limiting to %u\n",
                cpu_count, SMP_MAX_CPUS);
        cpu_count = SMP_MAX_CPUS;
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
    if (n > 16) n = 16;
    int total = end - start;
    if (n <= 1 || total < n) { fn(start, end, ctx); return; }   // not worth splitting

    struct smp_chunk ch[16];
    volatile uint32_t done[16];
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
static volatile uint64_t g_core_busy_ticks[SMP_MAX_CPUS];   // cumulative AP busy ticks
static uint64_t g_core_busy_last[SMP_MAX_CPUS];             // snapshot at last window
static uint64_t g_core_win_last = 0;                        // timer_ticks at last window
static int g_core_pct[SMP_MAX_CPUS];                        // smoothed 0-100 per core

// Called from sched_tick once per CPU% window (BSP). bsp_pct = aggregate CPU%.
void smp_account_core_usage(int bsp_pct) {
    extern volatile uint64_t timer_ticks;
    uint64_t now = timer_ticks;
    uint64_t win = now - g_core_win_last;
    g_core_win_last = now;
    if (win == 0) win = 1;

    g_core_pct[0] = bsp_pct;   // BSP measured by the existing aggregate meter
    for (uint32_t i = 1; i < cpu_count && i < SMP_MAX_CPUS; i++) {
        uint64_t cur = g_core_busy_ticks[i];
        uint64_t db = cur - g_core_busy_last[i];
        g_core_busy_last[i] = cur;
        int pct = (int)(db * 100 / win);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        g_core_pct[i] = (g_core_pct[i] * 2 + pct) / 3;   // EMA smoothing
        if (i < SMP_MAX_CPUS && g_ap_running_user[i]) g_core_pct[i] = 100;
    }
}

int smp_get_core_count(void) { return (int)cpu_count; }
int smp_get_core_pct(uint32_t cpu_id) {
    return (cpu_id < SMP_MAX_CPUS) ? g_core_pct[cpu_id] : 0;
}

// ---- built-in parallel self-test ----
static volatile uint64_t g_smp_test_cpu_jobs[SMP_MAX_CPUS];
static volatile uint64_t g_smp_test_results[64];

static void smp_test_job(void *arg) {
    uint64_t idx = (uint64_t)arg;
    uint32_t cpu = smp_get_cpu_id();
    if (cpu < SMP_MAX_CPUS) atomic_inc64(&g_smp_test_cpu_jobs[cpu]);
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
    for (uint32_t c = 0; c < SMP_MAX_CPUS; c++) g_smp_test_cpu_jobs[c] = 0;

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

    // Set CPU state to online
    cpu->state = CPU_STATE_ONLINE;
    atomic_inc32(&cpus_online);

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
                void *up = smp_ap_take_migratable();
                if (up) { smp_ap_run_user(up); continue; }
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

void bkl_acquire(void) {
    int cpu = (int)smp_this_cpu();
    // #264: mask IRQs for the whole acquire so the word=1/owner=unset window can
    // never be observed by a nested IRQ on this CPU (which would deadlock with
    // bkl_word stuck at 1 and bkl_owner left at -1). We restore the caller IF at
    // the end. Re-entrant callers (already the owner) are cheap and skip this.
    unsigned long fl; __asm__ volatile("pushfq; pop %0" : "=r"(fl));
    __asm__ volatile("cli");
    if (bkl_owner == cpu) { bkl_depth++; if (fl & (1UL << 9)) __asm__ volatile("sti"); return; }
    // Fast path: take it uncontended. IRQs are masked, so publishing owner/depth
    // right after the CAS is atomic w.r.t. any handler on this CPU.
    if (atomic_cas32(&bkl_word, 0, 1) == 0) {
        bkl_owner = cpu; bkl_depth = 1;
        if (fl & (1UL << 9)) __asm__ volatile("sti");
        return;
    }
    // Contended. CRITICAL (#279): we may have been entered from interrupt
    // context (idt.c wraps every ISR in bkl_acquire) with IF=0, or from a Ring-3
    // syscall/timer with IF=1. Spinning with interrupts masked blocks ALL IRQ
    // delivery on this CPU, which deadlocks against a holder that is (directly or
    // indirectly) waiting on an interrupt this CPU would service. So enable
    // interrupts WHILE waiting, but re-mask + publish owner BEFORE we ever leave
    // the word=1/owner=unset window, then restore caller IF.
    for (;;) {
        __asm__ volatile("sti");
        while (bkl_word) pause();                     // spin-read with IRQs live
        __asm__ volatile("cli");
        if (atomic_cas32(&bkl_word, 0, 1) == 0) break;
    }
    bkl_owner = cpu;                                  // published with IF=0
    bkl_depth = 1;
    if (fl & (1UL << 9)) __asm__ volatile("sti");     // restore caller IF
}

void bkl_release(void) {
    // #264: mask IRQs so the owner-read / depth-dec / word-clear is atomic w.r.t.
    // a nested handler on this CPU (which could otherwise observe a torn state).
    unsigned long fl; __asm__ volatile("pushfq; pop %0" : "=r"(fl));
    __asm__ volatile("cli");
    int cpu = (int)smp_this_cpu();
    if (bkl_owner != cpu) { if (fl & (1UL << 9)) __asm__ volatile("sti"); return; }
    if (--bkl_depth == 0) {
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
    bkl_depth = 0; bkl_owner = -1;
    atomic_store32(&bkl_word, 0);
    if (fl & (1UL << 9)) __asm__ volatile("sti");
    return d;
}
void bkl_reacquire(uint32_t depth) {
    if (depth == 0) return;
    int cpu = (int)smp_this_cpu();
    // #264: same publish-window guard as bkl_acquire. Mask IRQs across the take
    // so no nested handler can see word=1 with owner unset.
    unsigned long fl; __asm__ volatile("pushfq; pop %0" : "=r"(fl));
    __asm__ volatile("cli");
    if (atomic_cas32(&bkl_word, 0, 1) == 0) {
        bkl_owner = cpu; bkl_depth = depth;
        if (fl & (1UL << 9)) __asm__ volatile("sti");
        return;
    }
    // Contended: IRQ-friendly spin (see bkl_acquire note).
    for (;;) {
        __asm__ volatile("sti");
        while (bkl_word) pause();
        __asm__ volatile("cli");
        if (atomic_cas32(&bkl_word, 0, 1) == 0) break;
    }
    bkl_owner = cpu;                                  // published with IF=0
    bkl_depth = depth;
    if (fl & (1UL << 9)) __asm__ volatile("sti");
}

void kernel_lock_acquire(void) {
    spinlock_acquire(&kernel_lock);
}

void kernel_lock_release(void) {
    spinlock_release(&kernel_lock);
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
