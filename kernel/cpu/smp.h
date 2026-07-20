// smp.h - Symmetric Multi-Processing Support for MayteraOS
// Part of Task #41 (SMP Support)
//
// This module handles multi-CPU initialization and management:
// - Application Processor (AP) startup using INIT/SIPI sequence
// - Per-CPU data structures (GDT, TSS, IDT, stack)
// - CPU identification and enumeration
// - SMP-safe kernel synchronization

#ifndef SMP_H
#define SMP_H

#include "../types.h"
#include "../sync/spinlock.h"

// ============================================================================
// Configuration
// ============================================================================

// Maximum number of CPUs supported
#define SMP_MAX_CPUS        256

// Per-CPU stack size (16KB per CPU)
#define SMP_STACK_SIZE      (16 * 1024)

// AP startup code location (must be below 1MB for real mode)
// We use 0x8000 (32KB) as it's typically free after BIOS
#define AP_TRAMPOLINE_ADDR  0x8000

// ============================================================================
// CPU State
// ============================================================================

// CPU state flags
#define CPU_STATE_OFFLINE   0   // CPU not started
#define CPU_STATE_STARTING  1   // CPU is booting
#define CPU_STATE_ONLINE    2   // CPU is running
#define CPU_STATE_IDLE      3   // CPU is in idle loop
#define CPU_STATE_HALTED    4   // CPU halted (panic or shutdown)

// Per-CPU data structure
typedef struct {
    // Identification
    uint32_t cpu_id;            // Logical CPU ID (0 = BSP)
    uint32_t apic_id;           // APIC ID
    uint8_t is_bsp;             // Bootstrap Processor flag
    
    // State
    volatile uint8_t state;     // Current state (CPU_STATE_*)
    volatile uint8_t should_halt; // Set to 1 to halt this CPU
    
    // Per-CPU memory
    void *stack_base;           // Kernel stack base
    uint64_t stack_top;         // Kernel stack top (high address)
    
    // Per-CPU GDT and TSS
    void *gdt;                  // GDT for this CPU
    void *tss;                  // TSS for this CPU
    
    // Scheduling
    void *current_process;      // Currently running process
    void *idle_process;         // Idle process for this CPU
    uint64_t idle_time;         // Total time spent idle (ticks)
    uint64_t running_time;      // Total time running (ticks)
    
    // Statistics
    uint64_t ipi_received;      // IPIs received
    uint64_t timer_ticks;       // Timer ticks handled
    uint64_t syscalls;          // Syscalls handled
    
    // Padding to cache line boundary (64 bytes)
    uint8_t pad[16];
} __attribute__((aligned(64))) per_cpu_t;

// ============================================================================
// SMP Initialization
// ============================================================================

// Initialize SMP subsystem (called by BSP during boot)
// Detects available CPUs and prepares for AP startup
int smp_init(void);

// Start all Application Processors
// Returns number of CPUs successfully started
int smp_start_aps(void);

// Wait for all APs to reach a specific state
void smp_wait_for_aps(uint8_t state);

// ============================================================================
// CPU Identification
// ============================================================================

// Get current CPU ID (0 = BSP)
uint32_t smp_get_cpu_id(void);

// Get current CPU APIC ID
uint32_t smp_get_apic_id(void);

// Get total number of CPUs (online + offline)
uint32_t smp_get_cpu_count(void);

// Get number of online CPUs
uint32_t smp_get_online_count(void);

// Get per-CPU data structure for current CPU
per_cpu_t *smp_get_current_cpu(void);

// Get per-CPU data structure by CPU ID
per_cpu_t *smp_get_cpu(uint32_t cpu_id);

// Get per-CPU data structure by APIC ID
per_cpu_t *smp_get_cpu_by_apic(uint32_t apic_id);

// Check if running on BSP
static inline bool smp_is_bsp(void) {
    return smp_get_cpu_id() == 0;
}

// ============================================================================
// Per-CPU Data Access
// ============================================================================

// Set current process for this CPU
void smp_set_current_process(void *process);

// Get current process for this CPU
void *smp_get_current_process(void);

// ============================================================================
// Inter-Processor Interrupts
// ============================================================================

// Send reschedule IPI to a specific CPU
void smp_send_reschedule(uint32_t cpu_id);

// Send reschedule IPI to all other CPUs
void smp_send_reschedule_all(void);

// Send TLB shootdown IPI (invalidate TLB entry on all CPUs)
void smp_tlb_shootdown(uint64_t virt_addr);

// Send halt IPI to all CPUs (for panic)
void smp_halt_all(void);

// ============================================================================
// Synchronization Helpers
// ============================================================================

// Global kernel lock (big kernel lock for coarse-grained synchronization)
extern spinlock_t kernel_lock;

// Acquire kernel lock
void kernel_lock_acquire(void);

// Release kernel lock
void kernel_lock_release(void);

// #279 3b-1.5: per-CPU GS-base data + ring-0 stack setter.
void smp_cpu_local_init(uint32_t cpu);
void cpu_set_kernel_stack(uint64_t top);

// #279 3b-2 recursive Big Kernel Lock
void bkl_acquire(void);
void bkl_release(void);
uint32_t bkl_release_all(void);
void bkl_reacquire(uint32_t depth);

// #279 3b-3 per-CPU current process
extern volatile int g_smp_current_ready;
uint32_t smp_this_cpu(void);
void  smp_set_current(void *p);
void *smp_cpu_current(uint32_t cpu);

// ============================================================================
// SMP Parallel Work Pool (#279 stage 2)
// ============================================================================
// APs pull run-to-completion kernel jobs (Ring 0) off a shared, spinlock-
// protected ring and execute them in parallel with the BSP. Scales to N cores:
// every online AP runs the same work loop. The BSP can also drain the queue via
// smp_work_run_one(), so submitted work always completes even on a 1-CPU system.

// A unit of parallel work. fn(arg) runs to completion on whichever CPU pops it;
// if done != NULL it is set to 1 when the job finishes.
typedef struct {
    void (*fn)(void *);
    void *arg;
    volatile uint32_t *done;
} smp_job_t;

// Submit a job. Returns 0 on success, -1 if the queue is full (caller should
// either retry or call smp_work_run_one() to make room).
int smp_work_submit(void (*fn)(void *), void *arg, volatile uint32_t *done);

// Pop and run one queued job on the calling CPU. Returns 1 if a job ran, 0 if
// the queue was empty. Lets the BSP help drain the pool while waiting.
int smp_work_run_one(void);

// Number of jobs currently queued (approximate snapshot).
uint64_t smp_work_pending(void);

// Total jobs completed since boot (across all CPUs).
uint64_t smp_work_completed(void);

// Run the built-in parallel self-test (distributes compute jobs across all
// CPUs and prints per-CPU job counts proving the AP executed real work).
void smp_selftest(void);

// Per-core CPU utilization (#279 per-core meters). smp_account_core_usage() is
// called once per CPU% window from sched_tick (bsp_pct = aggregate CPU% for
// core 0). smp_get_core_count()/smp_get_core_pct() expose the smoothed 0-100%.
void smp_account_core_usage(int bsp_pct);
int  smp_get_core_count(void);
int  smp_get_core_pct(uint32_t cpu_id);

// Parallel-for: split [start,end) across all cores. The range function must only
// touch KERNEL memory (APs lack the caller's user mappings). (#279 stage 3a.)
typedef void (*smp_range_fn)(int start, int end, void *ctx);
void smp_parallel_for(int start, int end, smp_range_fn fn, void *ctx);

// ============================================================================
// AP Entry Points
// ============================================================================

// AP C entry point (called after trampoline brings AP to long mode)
void ap_entry(void);

// AP idle loop
void ap_idle(void);

// ============================================================================
// Internal Data Structures
// ============================================================================

// Trampoline data structure (placed at known address for AP startup)
typedef struct {
    // These are set by BSP before SIPI, read by trampoline code
    uint64_t pml4_phys;         // Page table root
    uint64_t stack_top;         // Stack pointer for this AP
    uint64_t ap_entry_addr;     // Address of ap_entry() function
    uint32_t cpu_id;            // CPU ID being started
    uint32_t apic_id;           // APIC ID of this CPU
    volatile uint32_t started;  // Set by AP when it reaches C code
    
    // GDT pointer for AP
    uint16_t gdt_limit;
    uint64_t gdt_base;
} __attribute__((packed)) trampoline_data_t;

// Trampoline data is placed right after trampoline code
#define TRAMPOLINE_DATA_OFFSET  0xE00  // moved past trampoline code (ends 0x200) - was 0x100 and CLOBBERED the post-longmode code

// ============================================================================
// Debug/Status Functions
// ============================================================================

// Print SMP status (CPU states, etc.)
void smp_print_status(void);

// Get CPU state as string
const char *smp_state_string(uint8_t state);

#endif // SMP_H
