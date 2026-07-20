// process.c - Process and task management implementation
#include "process.h"
#include "syscall.h"
#include "procmem.h"      // #487: per-process memory accounting (pulls mm/demand.h)
#include "../security/validate.h"   // #503: deferred-write validation (clear_child_tid)
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../cpu/gdt.h"
#include "../cpu/smp.h"
#include "../serial.h"
#include "../string.h"
#include "../cpu/isr.h"
#include "../exec/elf.h"
#include "../fs/fat.h"
#include "../gui/syslog.h"
#include "../fs/vfs.h"

// Process table
static process_t proc_table[MAX_PROCESSES];

// Current running process
static process_t *current_proc = NULL;

// Global pointer for external use (demand.c, etc.)
process_t *current_process = NULL;

// #373 heartbeat: monotonic count of real scheduler context switches, read by
// the kernel heartbeat thread (main.c) to prove scheduler liveness.
volatile uint64_t g_ctx_switches = 0;

// Ready queue (linked list of ready processes)
static process_t *ready_queue_head = NULL;
static process_t *ready_queue_tail = NULL;

// Next available PID
static uint32_t next_pid = 1;

// Preemption control
static bool preemption_enabled = false;

// Phase J: single-shot PTY binding for the next proc_create_user*() call.
// Set by proc_create_user_tty() under preemption disable, consumed by
// init_proc(). -1 means "use /dev/console as stdio", 0..7 selects /dev/pts/N.
int g_tty_bind_pts_idx = -1;

// Scheduler tick counter
static uint64_t sched_ticks = 0;
// CPU usage accounting: fraction of timer ticks NOT spent in the idle proc (pid 0).
static uint64_t g_cpu_idle_acc = 0, g_cpu_total_acc = 0;
static int g_cpu_pct = 0;
int proc_get_cpu_usage(void) { return g_cpu_pct; }

// Time slice duration (in timer ticks)
#define TIME_SLICE_TICKS    10  // ~100ms at 100Hz timer

// Forward declarations
static void idle_process(void *arg);
static void proc_wrapper(void);
static void add_to_ready_queue(process_t *proc);
static process_t *remove_from_ready_queue(void);

// ============================================================================
// Process Table Management
// ============================================================================


/**
 * Clean up resources from a previously used process slot
 * Called before reusing a slot for a new process
 */
static void cleanup_proc_slot(process_t *proc) {
    // Free kernel stack if allocated
    if (proc->stack_base) {
        kfree(proc->stack_base);
        proc->stack_base = NULL;
    }
    
    // Free user address space if this was a user process.
    // #430: a thread (shares_vm) borrows the leader's cr3 and its user stack
    // lives in the shared heap; destroying either here would corrupt/kill the
    // rest of the thread group. Only the owning task frees the address space.
    if (proc->cr3 != 0 && !proc->shares_vm) {
        // Free user stack pages first
        if (proc->user_stack_base != 0 && proc->user_stack_size != 0) {
            uint64_t stack_pages = proc->user_stack_size / VMM_PAGE_SIZE_4K;
            vmm_free_user_pages(proc->cr3, proc->user_stack_base, stack_pages);
        }
        // Then destroy the address space
        vmm_destroy_user_space(proc->cr3);
    }
    // #429: free the per-process demand-paging mm. A thread (shares_vm)
    // borrows the leader's mm, so only a non-thread owner frees it.
    if (proc->mm && !proc->shares_vm) {
        extern void mm_destroy(void *mm);
        mm_destroy(proc->mm);
    }
    proc->mm = NULL;
    proc->cr3 = 0;
    proc->shares_vm = 0;
    proc->clear_child_tid = NULL;
}

// #264: reap a specific zombie child, freeing its slot + resources. Safe on a
// non-zombie / invalid pid (no-op). rc_cmd_shell calls this so each session's
// MSH does not leak as a permanent zombie.
int proc_reap(uint32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &proc_table[i];
        if (p->state == PROC_STATE_ZOMBIE && p->pid == pid) {
            cleanup_proc_slot(p);
            p->state = PROC_STATE_UNUSED;
            return 0;
        }
    }
    return -1;
}

// #264: fallback reaper. Free ZOMBIE procs nobody will ever wait for: orphans
// (parent slot gone) and children of init/idle (pid 0/1, which never proc_wait).
// Returns the count reclaimed. Never touches idle (slot 0) or the current proc.
static int reap_orphan_zombies(void) {
    int n = 0;
    for (int i = 1; i < MAX_PROCESSES; i++) {
        process_t *z = &proc_table[i];
        if (z->state != PROC_STATE_ZOMBIE) continue;
        if (z == current_proc) continue;
        uint32_t ppid = z->ppid;
        // #430: an exited CLONE_THREAD thread is never wait()-ed for (join uses
        // the futex/clear_child_tid path), so its zombie slot can always be
        // reclaimed - treat it like an orphan so thread zombies don't leak.
        int orphan = (ppid <= 1) || z->shares_vm;
        if (!orphan) {
            int parent_alive = 0;
            for (int j = 0; j < MAX_PROCESSES; j++) {
                process_t *pp = &proc_table[j];
                if (pp->state != PROC_STATE_UNUSED &&
                    pp->state != PROC_STATE_ZOMBIE &&
                    pp->pid == ppid) { parent_alive = 1; break; }
            }
            if (!parent_alive) orphan = 1;
        }
        if (orphan) {
            cleanup_proc_slot(z);
            z->state = PROC_STATE_UNUSED;
            n++;
        }
    }
    return n;
}

/**
 * Find a free process slot
 */
static process_t *alloc_proc_slot(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state == PROC_STATE_UNUSED) {
            // Clean up any leftover resources from previous process
            cleanup_proc_slot(&proc_table[i]);
            return &proc_table[i];
        }
    }
    // #264: table full -> reclaim leaked zombies before giving up.
    if (reap_orphan_zombies() > 0) {
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (proc_table[i].state == PROC_STATE_UNUSED) {
                cleanup_proc_slot(&proc_table[i]);
                return &proc_table[i];
            }
        }
    }
    return NULL;
}

/**
 * Initialize a process structure
 */
static void init_proc(process_t *proc, const char *name, process_priority_t prio) {
    memset(proc, 0, sizeof(process_t));
    proc->pid = next_pid++;
    proc->ppid = current_proc ? current_proc->pid : 0;
    strncpy(proc->name, name, sizeof(proc->name) - 1);
    proc->state = PROC_STATE_READY;
    proc->priority = prio;
    proc->privilege = PRIV_KERNEL;  // Default to kernel mode
    proc->time_slice = TIME_SLICE_TICKS;
    proc->total_time = 0;
    // User identity: kernel processes default to root (0)
    proc->uid  = current_proc ? current_proc->uid  : 0;
    proc->gid  = current_proc ? current_proc->gid  : 0;
    proc->euid = current_proc ? current_proc->euid : 0;
    proc->egid = current_proc ? current_proc->egid : 0;
    proc->cr3 = 0;  // Will be set for user processes
    proc->user_stack_base = 0;
    proc->user_stack_size = 0;
    proc->user_rsp = 0;
    proc->user_rip = 0;
    proc->next = NULL;

    // Phase 0: default cwd for a new process is the filesystem root. Fork
    // overrides this by full-structure memcpy from the parent below.
    if (current_proc && current_proc->cwd[0]) {
        strncpy(proc->cwd, current_proc->cwd, PROC_CWD_MAX - 1);
        proc->cwd[PROC_CWD_MAX - 1] = '\0';
    } else {
        proc->cwd[0] = '/';
        proc->cwd[1] = '\0';
    }
    proc->wait_entry = NULL;

    // Phase D4: default process-group and session IDs. A brand-new process
    // inherits its parent's pgrp/session, or (for the first process) becomes
    // a self-led group in a self-led session. Fork overrides this by the
    // full-structure memcpy below, so these are only used for kernel threads
    // started via proc_create() and for the very first user process.
    if (current_proc) {
        proc->pgrp    = current_proc->pgrp ? current_proc->pgrp : proc->pid;
        proc->session = current_proc->session ? current_proc->session : proc->pid;
    } else {
        proc->pgrp    = proc->pid;
        proc->session = proc->pid;
    }

    // Phase A2: Pre-open /dev/console on fds 0, 1, 2 (stdin/stdout/stderr).
    // Phase J: if g_tty_bind_pts_idx is >= 0 (set by proc_create_user_tty()
    // under preemption disable), open /dev/pts/N instead of /dev/console and
    // consume the binding so subsequent procs revert to the console default.
    // For the idle process created before dev_init() runs, dev_open returns
    // NULL and we simply leave the slots empty; the idle thread never reads
    // or writes through fds anyway. For every subsequent process the three
    // slots hold independent struct file* pointers (separate refcounts).
    // Skipped for processes that already inherited fds (via proc_fork's
    // memcpy) or for proc_fork -- that caller does its own fd copy.
    extern struct file *dev_open(const char *name, int flags);
    if (proc->fds[0] == NULL && proc->fds[1] == NULL && proc->fds[2] == NULL) {
        const char *dev = "console";
        static const char *s_pts_names_by_idx[8] = {
            "pts/0", "pts/1", "pts/2", "pts/3",
            "pts/4", "pts/5", "pts/6", "pts/7",
        };
        if (g_tty_bind_pts_idx >= 0 && g_tty_bind_pts_idx < 8) {
            dev = s_pts_names_by_idx[g_tty_bind_pts_idx];
            g_tty_bind_pts_idx = -1;  // consume single-shot binding
        }
        // O_RDONLY=0 for stdin; O_WRONLY=1 for stdout/stderr. PTY slaves
        // treat both modes as r/w.
        struct file *fi = dev_open(dev, 0);
        struct file *fo = dev_open(dev, 1);
        struct file *fe = dev_open(dev, 1);
        if (fi) proc->fds[0] = fi;
        if (fo) proc->fds[1] = fo;
        if (fe) proc->fds[2] = fe;
    }
}

// ============================================================================
// Ready Queue Management
// ============================================================================

/**
 * Add a process to the ready queue (priority-based insertion)
 */
static void add_to_ready_queue(process_t *proc) {
    proc->next = NULL;
    proc->state = PROC_STATE_READY;

    if (ready_queue_head == NULL) {
        ready_queue_head = ready_queue_tail = proc;
        return;
    }

    // Simple priority queue: insert based on priority
    // Higher priority goes first
    if (proc->priority > ready_queue_head->priority) {
        proc->next = ready_queue_head;
        ready_queue_head = proc;
        return;
    }

    // Find insertion point
    process_t *prev = NULL;
    process_t *curr = ready_queue_head;
    while (curr && curr->priority >= proc->priority) {
        prev = curr;
        curr = curr->next;
    }

    if (prev) {
        proc->next = prev->next;
        prev->next = proc;
        if (prev == ready_queue_tail) {
            ready_queue_tail = proc;
        }
    }
}

/**
 * Remove and return the highest priority ready process
 */
static process_t *remove_from_ready_queue(void) {
    if (ready_queue_head == NULL) {
        return NULL;
    }

    process_t *proc = ready_queue_head;
    ready_queue_head = proc->next;
    if (ready_queue_head == NULL) {
        ready_queue_tail = NULL;
    }
    proc->next = NULL;
    return proc;
}

// ============================================================================
// Process Creation and Lifecycle
// ============================================================================

/**
 * Idle process - runs when no other processes are ready
 */
static void idle_process(void *arg) {
    extern void desktop_process_tick(void);
    extern uint32_t compositor_pid;
    (void)arg;
    while (1) {
        // Under the userland compositor (it grabs input + owns the screen) the
        // kernel desktop tick is redundant; skipping it lets the core actually
        // HLT instead of polling input every tick (#102 host-CPU).
        if (compositor_pid == 0) desktop_process_tick();
        // Atomically enable interrupts and HALT the core. sti has a 1-instr
        // delay so hlt executes before any IRQ fires - this guarantees the vCPU
        // actually halts until the next interrupt (the old hlt();proc_yield()
        // sequence ran cli() in proc_yield and let hlt return immediately,
        // pegging the host at ~100%). The timer IRQ + sched_tick reschedule us
        // to any process that becomes ready.
        __asm__ volatile("sti; hlt");
    }
}

/**
 * Process wrapper function
 * This is the actual function called when a process starts
 * It sets up the environment and calls the real entry point
 */
static void proc_wrapper(void) {
    // Get current process
    process_t *proc = current_proc;
    if (!proc || !proc->entry_point) {
        kprintf("[PROC] Error: invalid process state in wrapper\n");
        proc_exit(-1);
    }

    { extern int g_smp_bkl_full; extern void bkl_acquire(void); if (g_smp_bkl_full) bkl_acquire(); }  // #279 3b-3C: kernel threads hold BKL
    // Enable interrupts for user code
    sti();

    // Call the actual entry point
    proc->entry_point(proc->entry_arg);

    // Process returned - exit
    proc_exit(0);
}

/**
 * Initialize the process subsystem
 */
void proc_init(void) {
    kprintf("[PROC] Initializing process subsystem...\n");

    // Clear process table
    memset(proc_table, 0, sizeof(proc_table));

    // #699: also reset the ready-queue head/tail. If anything ever calls
    // proc_create() before proc_init() runs (e.g. a boot-time hardware worker
    // started during an early hardware-detect stage), add_to_ready_queue()
    // leaves these static pointers referencing a proc_table[] slot that the
    // memset() above just wiped out from under it -- every process created
    // afterward then gets linked into the ready queue relative to that
    // stale/zeroed (and possibly reused-for-idle) entry, corrupting scheduling
    // for the rest of the boot (#699: this is what broke sshd's per-connection
    // worker spawn when a new boot-time audio worker started before this
    // function ran). proc_init() is supposed to bring the whole process
    // subsystem back to a known-clean state, so the ready queue belongs here
    // too, not just the table.
    ready_queue_head = NULL;
    ready_queue_tail = NULL;

    // Create idle process (PID 0)
    process_t *idle = &proc_table[0];
    init_proc(idle, "idle", PRIO_NORMAL);  // PID 0 runs the desktop, needs normal priority
    idle->pid = 0;  // Special PID for idle
    next_pid = 1;   // Reset for next regular process

    // Allocate stack for idle process
    idle->stack_base = kmalloc(PROCESS_STACK_SIZE);
    if (!idle->stack_base) {
        kprintf("[PROC] Failed to allocate idle process stack\n");
        return;
    }
    idle->stack_size = PROCESS_STACK_SIZE;

    // Set up idle process stack
    uint64_t stack_top = (uint64_t)idle->stack_base + PROCESS_STACK_SIZE;
    stack_top &= ~0xF;  // 16-byte align

    // Set up initial context on stack
    // Layout must match what context_switch pops:
    // 1. Return address (for final ret)
    // 2. rbx, rbp, r12, r13, r14, r15 (callee-saved)
    // 3. rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11
    // 4. rflags

    // Push return address (entry point)
    stack_top -= 8;
    *(uint64_t *)stack_top = (uint64_t)idle_process;

    // Push callee-saved registers (rbx, rbp, r12-r15)
    for (int i = 0; i < 6; i++) {
        stack_top -= 8;
        *(uint64_t *)stack_top = 0;
    }

    // Push remaining registers (rax, rcx, rdx, rsi, rdi, r8-r11)
    for (int i = 0; i < 9; i++) {
        stack_top -= 8;
        *(uint64_t *)stack_top = 0;
    }

    // Push rflags (with interrupts enabled)
    stack_top -= 8;
    *(uint64_t *)stack_top = 0x202;

    // Reserve space for SSE registers (xmm0-xmm15, 16 regs * 16 bytes = 256 bytes).
    // context_switch saves/restores these below RFLAGS on the stack.
    stack_top -= 256;
    memset((void *)stack_top, 0, 256);

    idle->rsp = stack_top;
    idle->context = NULL;  // Not using struct anymore
    idle->entry_point = idle_process;
    idle->entry_arg = NULL;
    idle->state = PROC_STATE_READY;

    // Set idle as current process initially
    current_proc = idle;
    current_process = idle;
    idle->state = PROC_STATE_RUNNING;

    kprintf("[PROC] Process subsystem initialized\n");
    kprintf("[PROC] Created idle process (PID 0)\n");
}

/**
 * Create a new process
 */
int proc_create(const char *name, void (*entry)(void *), void *arg,
                process_priority_t priority) {
    return proc_create_ex(name, entry, arg, priority, PROCESS_STACK_SIZE);
}

// #264: stack-size-parameterized creator. Net workers (TLS/HTTPS) need a much
// larger kernel stack than the 16KB default or they overflow into the heap.
int proc_create_ex(const char *name, void (*entry)(void *), void *arg,
                   process_priority_t priority, uint32_t stack_size) {
    if (!name || !entry) {
        return -1;
    }
    if (stack_size < PROCESS_STACK_SIZE) stack_size = PROCESS_STACK_SIZE;

    // Disable preemption during process creation
    bool old_preempt = sched_set_preemption(false);

    // Allocate process slot
    process_t *proc = alloc_proc_slot();
    if (!proc) {
        kprintf("[PROC] No free process slots\n");
        sched_set_preemption(old_preempt);
        return -1;
    }

    // Initialize process structure
    init_proc(proc, name, priority);

    // Allocate stack
    proc->stack_base = kmalloc(stack_size);
    if (!proc->stack_base) {
        kprintf("[PROC] Failed to allocate stack for %s\n", name);
        proc->state = PROC_STATE_UNUSED;
        sched_set_preemption(old_preempt);
        return -1;
    }
    proc->stack_size = stack_size;

    // Set up stack
    uint64_t stack_top = (uint64_t)proc->stack_base + stack_size;
    stack_top &= ~0xF;  // 16-byte align

    // Set up initial context on stack
    // Layout must match what context_switch pops

    // Push return address (entry point - proc_wrapper)
    stack_top -= 8;
    *(uint64_t *)stack_top = (uint64_t)proc_wrapper;

    // Push callee-saved registers (rbx, rbp, r12-r15) - all zero
    for (int i = 0; i < 6; i++) {
        stack_top -= 8;
        *(uint64_t *)stack_top = 0;
    }

    // Push remaining registers (rax, rcx, rdx, rsi, rdi, r8-r11) - all zero
    for (int i = 0; i < 9; i++) {
        stack_top -= 8;
        *(uint64_t *)stack_top = 0;
    }

    // Push rflags (with interrupts enabled)
    stack_top -= 8;
    *(uint64_t *)stack_top = 0x202;

    // Reserve space for SSE registers (xmm0-xmm15, 16 regs * 16 bytes = 256 bytes).
    // context_switch saves/restores these below RFLAGS on the stack.
    stack_top -= 256;
    memset((void *)stack_top, 0, 256);
    proc->rsp = stack_top;
    proc->context = NULL;
    proc->entry_point = entry;
    proc->entry_arg = arg;

    // Add to ready queue
    add_to_ready_queue(proc);

    kprintf("[PROC] Created process '%s' (PID %u)\n", name, proc->pid);

    sched_set_preemption(old_preempt);
    return proc->pid;
}

/**
 * Terminate the current process
 */
void proc_exit(int exit_code) {
    current_proc->exit_code = exit_code;

    if (!current_proc || current_proc->pid == 0) {
        // Can't exit idle process
        kprintf("[PROC] Cannot exit idle process\n");
        return;
    }

    kprintf("[PROC] Process '%s' (PID %u) exiting\n",
            current_proc->name, current_proc->pid);

    // #430: CLONE_CHILD_CLEARTID - a thread created via clone() with a
    // clear_child_tid address must, on exit, zero that word (in the still-live
    // shared address space) and futex-wake anyone blocked on it. This is how
    // pthread_join() learns the thread has finished. Done BEFORE cli() so the
    // shared cr3 is active and the write lands on the right page.
    if (current_proc->clear_child_tid) {
        uint32_t *ctid = current_proc->clear_child_tid;
        current_proc->clear_child_tid = NULL;
        *ctid = 0;
        extern void futex_wake_addr(uint32_t *addr, int count);
        futex_wake_addr(ctid, 1);
    }

    // Disable interrupts during cleanup
    cli();

    // Phase A1: drop every open file descriptor so reference counts on
    // struct files are correct. fd_close_all operates on current_proc, and
    // we are the current process (about to become a zombie). Done under cli.
    extern void fd_close_all(void);
    fd_close_all();

    // Mark as zombie (cleanup will happen later)
    current_proc->state = PROC_STATE_ZOMBIE;

    // Clean up user windows for this process
    extern void cleanup_user_windows_for_process(uint32_t pid);
    cleanup_user_windows_for_process(current_proc->pid);

    // Tear down any Ring-3 PCM stream this process owned but never closed. The
    // music player force-kills its /APPS/MUSICPLR --play helper with SIGKILL on
    // a manual track switch, so a stream outliving its owner is a NORMAL path,
    // not an edge case: without this the next track would hit EBUSY until the
    // pump's backstop expired. Only sets flags and wakes the pump (safe under
    // cli(); the pump thread does the actual teardown and frees the ring).
    extern void audio_pcm_proc_exit(uint32_t pid);
    audio_pcm_proc_exit(current_proc->pid);

    // DO NOT free stack here - we are still running on it!
    // Stack will be freed when process slot is reused.

    // Keep as zombie - cleanup happens when slot is reused
    // (We can't free stack here since we're still running on it)

    // Schedule next process
    sched_schedule(); // Run new process

    // Should never reach here
    while (1) hlt();
}

/**
 * Yield CPU to another process
 */
void proc_yield(void) {
    cli();

    if (current_proc && current_proc->state == PROC_STATE_RUNNING) {
        current_proc->state = PROC_STATE_READY;
        add_to_ready_queue(current_proc);
    }

    sched_schedule(); // Run new process
}

/**
 * Transition a BLOCKED or SLEEPING process back to READY.
 *
 * Phase 0: used by sync/waitq.c (wake_up) and by future signal-delivery
 * code to kick a target that's interruptibly sleeping. Takes the process
 * lock implicitly by disabling interrupts; idempotent if the target is
 * already runnable (e.g., scheduler already transitioned it).
 */
void proc_wake(process_t *p) {
    if (!p) return;

    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");

    // Only unblock processes that are actually parked. A process that's
    // already RUNNING/READY/ZOMBIE is not our business.
    if (p->state == PROC_STATE_BLOCKED || p->state == PROC_STATE_SLEEPING) {
        // add_to_ready_queue sets state = READY as a side effect.
        add_to_ready_queue(p);
    }

    if (rflags & (1ULL << 9)) __asm__ volatile("sti" ::: "memory");
}

/**
 * Sleep for specified milliseconds
 */
void proc_sleep(uint32_t ms) {
    if (!current_proc || ms == 0) return;

    cli();

    // Convert ms -> timer ticks using the ACTUAL timer frequency. This was
    // hardcoded to 100Hz (10ms per tick), but the PIT runs at 250Hz, so every
    // sys_sleep slept ~40% of the requested time. That made the compositor's
    // 33ms (30 FPS) frame sleep fire at ~16ms (~60+ FPS) and pegged the CPU.
    extern uint32_t g_timer_hz;
    uint32_t _hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t ticks = ((uint64_t)ms * _hz + 999) / 1000;  // round up
    if (ticks == 0) ticks = 1;
    current_proc->wake_time = timer_ticks + ticks;
    current_proc->state = PROC_STATE_SLEEPING;

    sched_schedule(); // Run new process
}


/**
 * Wait for a child process to exit
 * @param pid   Process ID to wait for (-1 for any child)
 * @param status Pointer to store exit status (can be NULL)
 * @return      PID of exited child, or -1 on error
 */
int proc_wait(int pid, int *status) {
    process_t *current = proc_current();
    if (!current) return -1;
    
    while (1) {
        // Look for zombie children
        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t *child = &proc_table[i];
            
            if (child->state == PROC_STATE_UNUSED) continue;
            if (child->ppid != current->pid) continue;  // Not our child
            // #430: a CLONE_THREAD thread (shares_vm) is NOT a wait()-able
            // child - pthread_join() reaps it via the futex/clear_child_tid
            // path. Skipping it here stops wait() from swallowing a thread's
            // exit in place of a real fork child's.
            if (child->shares_vm) continue;
            if (pid > 0 && child->pid != (uint32_t)pid) continue;  // Wrong child
            
            if (child->state == PROC_STATE_ZOMBIE) {
                // Child has exited - reap it
                int exit_code = child->exit_code;
                if (status) *status = exit_code;
                uint32_t child_pid = child->pid;
                
                // Clean up zombie resources
                cleanup_proc_slot(child);
                child->state = PROC_STATE_UNUSED;
                
                return (int)child_pid;
            }
        }
        
        // No zombie child found, check if we have any children at all
        bool has_children = false;
        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t *child = &proc_table[i];
            if (child->state != PROC_STATE_UNUSED && child->ppid == current->pid &&
                !child->shares_vm) {  // #430: threads are not wait()-able children
                if (pid <= 0 || child->pid == (uint32_t)pid) {
                    has_children = true;
                    break;
                }
            }
        }
        if (!has_children) return -1;  // No matching children
        
        // Yield and try again
        sched_schedule();
    }
}


/**
 * Get current process
 */
process_t *proc_current(void) {
    // #279 3b-3: on SMP, return THIS cpu's current process; fall back to the
    // BSP global before per-cpu data is live or if unset.
    extern int g_smp_user_sched;
    if (g_smp_current_ready && g_smp_user_sched) {
        process_t *p = (process_t *)smp_cpu_current(smp_this_cpu());
        if (p) return p;
    }
    return current_proc;
}

/**
 * Get process by PID
 */
process_t *proc_get(uint32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state != PROC_STATE_UNUSED &&
            proc_table[i].pid == pid) {
            return &proc_table[i];
        }
    }
    return NULL;
}

/**
 * Get count of active processes
 */
uint32_t proc_count(void) {
    uint32_t count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state != PROC_STATE_UNUSED) {
            count++;
        }
    }
    return count;
}

/**
 * Get state name string
 */
static const char *state_name(process_state_t state) {
    switch (state) {
        case PROC_STATE_UNUSED:   return "UNUSED";
        case PROC_STATE_READY:    return "READY";
        case PROC_STATE_RUNNING:  return "RUNNING";
        case PROC_STATE_SLEEPING: return "SLEEPING";
        case PROC_STATE_BLOCKED:  return "BLOCKED";
        case PROC_STATE_ZOMBIE:   return "ZOMBIE";
        default:                  return "UNKNOWN";
    }
}

/**
 * Print process list
 */
void proc_print_list(void) {
    kprintf("\n[PROC] Process List:\n");
    kprintf("  PID  PPID  STATE     PRIO  CPU TIME  NAME\n");
    kprintf("  ---  ----  --------  ----  --------  ----\n");

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (proc_table[i].state != PROC_STATE_UNUSED) {
            process_t *p = &proc_table[i];
            kprintf("  %3u  %4u  %-8s  %4u  %8lu  %s%s\n",
                    p->pid, p->ppid,
                    state_name(p->state),
                    p->priority,
                    p->total_time,
                    p->name,
                    (p == current_proc) ? " *" : "");
        }
    }
    kprintf("\n");
}

// ============================================================================
// Scheduler
// ============================================================================

/**
 * Wake sleeping processes
 */
static void wake_sleeping_procs(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        // #513: alarm(2) deadline sweep, folded into the table walk that was
        // already happening here rather than adding a second O(MAX_PROCESSES)
        // pass. Checked for EVERY live process, not just SLEEPING ones: SIGALRM
        // must fire whatever the target is doing, which is exactly why it needs
        // its own field and cannot ride on wake_time. Disarm BEFORE raising so
        // the one-shot cannot re-fire if sig_raise reschedules us, and so a
        // handler calling alarm() again is not immediately clobbered.
        if (proc_table[i].state != PROC_STATE_UNUSED && proc_table[i].alarm_time != 0 &&
            (int64_t)(timer_ticks - proc_table[i].alarm_time) >= 0) {   // signed: wrap-safe
            proc_table[i].alarm_time = 0;
            extern void sig_raise(process_t *target, int signo);
            sig_raise(&proc_table[i], 14 /* SIGALRM */);
        }
        if (proc_table[i].state == PROC_STATE_SLEEPING) {
            if (timer_ticks >= proc_table[i].wake_time) {
                proc_table[i].state = PROC_STATE_READY;
                add_to_ready_queue(&proc_table[i]);
            }
        }
    }
}

/**
 * Schedule next process
 * This is called from the timer interrupt or when a process yields
 */
static process_t *migq_head = NULL;
static spinlock_t migq_lock = SPINLOCK_INIT;
// #279 generalization: one-shot "launch next user proc on an AP" request.
// Set by proc_set_next_migratable() (e.g. RC launchap, Task Manager affinity)
// and consumed by proc_create_user so ANY app, not just spin*, is routed to an AP.
volatile int g_next_user_migratable = 0;
void proc_set_next_migratable(int v){ g_next_user_migratable = v ? 1 : 0; }
extern volatile int g_ap_running_user[];
extern void context_start(uint64_t *old_rsp, uint64_t new_rsp, uint64_t new_cr3);
void smp_migq_push(void *vp){ process_t *p=(process_t*)vp; spinlock_acquire(&migq_lock); p->state=PROC_STATE_READY; p->running_cpu=-1; p->next=migq_head; migq_head=p; spinlock_release(&migq_lock); { extern void smp_wake_aps(void); smp_wake_aps(); } }
void *smp_ap_take_migratable(void){ process_t *p=NULL; spinlock_acquire(&migq_lock); if(migq_head){ p=migq_head; migq_head=p->next; p->next=NULL; p->running_cpu=(int)smp_this_cpu(); p->state=PROC_STATE_RUNNING; } spinlock_release(&migq_lock); return p; }
void smp_ap_run_user(void *vp){ process_t *p=(process_t*)vp; static uint64_t ap_sched_rsp[64]; uint32_t cpu=smp_this_cpu()&63; g_ap_running_user[cpu]=1; kprintf("[SMP] CPU %u now running user proc '%s' (PID %u)\n", cpu, p->name, p->pid); smp_set_current(p); cpu_set_kernel_stack((uint64_t)p->stack_base+p->stack_size); p->total_time=1; context_start(&ap_sched_rsp[cpu], p->rsp, p->cr3); g_ap_running_user[cpu]=0; }

void sched_schedule(void) {
    if (!preemption_enabled && current_proc &&
        current_proc->state == PROC_STATE_RUNNING) {
        // Preemption disabled and current process still running
        sti();
        return;
    }

    // Wake any sleeping processes
    wake_sleeping_procs();

    // Get next process from ready queue
    process_t *next = remove_from_ready_queue();

    bool no_ready = false;
    if (!next) {
        // No ready processes - fall back to the idle slot.
        no_ready = true;
        next = &proc_table[0];  // Idle process
        if (next->state == PROC_STATE_UNUSED) {
            // Idle not ready - just return
            sti();
            return;
        }
    }

    // If same process, just continue
    if (next == current_proc) {
        current_proc->time_slice = TIME_SLICE_TICKS;
        current_proc->state = PROC_STATE_RUNNING;
        if (no_ready) {
            // Nothing else is runnable: HALT the core until the next interrupt
            // instead of busy-returning. pid 0 doubles as the desktop/idle
            // context, so without this the CPU spun at ~100% whenever every
            // process was sleeping (#180). sti;hlt is atomic (sti 1-instr delay).
            __asm__ volatile("sti; hlt");
        } else {
            sti();
        }
        return;
    }

    // Context switch needed
    // #373 heartbeat: count every real context switch so the kernel heartbeat
    // log can report scheduler liveness (alive-but-idle vs genuinely hung) on
    // real hardware where there is no serial console.
    g_ctx_switches++;
    process_t *prev = current_proc;
    current_proc = next;
    current_process = next;
    smp_set_current(next);  // #279 3b-3: mirror into this cpu's per-cpu slot
    next->state = PROC_STATE_RUNNING;
    next->time_slice = TIME_SLICE_TICKS;

    // Re-queue prev if it was RUNNING (voluntary yield). Callers that want to
    // block prev (e.g. wait-queue sleep, proc_exit) set a different state
    // before calling sched_schedule, so they are not affected.
    if (prev && prev != next && prev->state == PROC_STATE_RUNNING) {
        prev->state = PROC_STATE_READY;
        add_to_ready_queue(prev);
    }

    // Perform context switch
    // CRITICAL: Check if next process is user-space running for first time
    if (next->privilege == PRIV_USER && next->total_time == 0) {
        // First-time user process - must use context_start for Ring 0->3 transition
        // kprintf("[SCHED] Starting user process %s (PID %u) for FIRST TIME...\n",
                // next->name, next->pid);
        // kprintf("[SCHED] Entry: 0x%lx, RSP: 0x%lx, CR3: 0x%lx\n",
                // next->user_rip, next->rsp, next->cr3);

        // Set kernel stack for syscall handling
        // CRITICAL: Pass TOP of kernel stack, not IRET frame address
        uint64_t kernel_stack_top = (uint64_t)next->stack_base + next->stack_size;
        // kprintf("[SCHED] Setting kernel stack (TSS.RSP0) to 0x%lx (base=0x%lx size=0x%lx)\n",
                // kernel_stack_top, (uint64_t)next->stack_base, next->stack_size);
        cpu_set_kernel_stack(kernel_stack_top);  // Sets TSS.RSP0

        // Jump to user mode via IRET (context_start loads CR3 and switches to user mode)
        kprintf("[SCHED] IRET to %s rip=0x%lx rsp=0x%lx cr3=0x%lx\n", next->name, next->user_rip, next->user_rsp, next->cr3);
        // Validate IRET frame before context_start
        {
            uint64_t *frame = (uint64_t *)next->rsp;
            // frame layout: 15 GPRs, then RIP, CS, RFLAGS, RSP, SS
            uint64_t iret_rip = frame[15];
            uint64_t iret_cs  = frame[16];
            if (iret_rip == 0 || iret_cs == 0) {
                kprintf("[SCHED] FATAL: corrupted IRET frame! Skipping context_start.\n");
                next->state = PROC_STATE_ZOMBIE;
                return;
            }
        }
        next->total_time = 1;
        if (prev && prev->state != PROC_STATE_UNUSED) {
            { uint32_t __bd = bkl_release_all(); context_start(&prev->rsp, next->rsp, next->cr3); bkl_reacquire(__bd); }
        } else {
            static uint64_t dummy_rsp;
            { uint32_t __bd = bkl_release_all(); context_start(&dummy_rsp, next->rsp, next->cr3); bkl_reacquire(__bd); }
        }
        // kprintf("[SCHED] ERROR: context_start returned! This should never happen!\n");
    } else {
        // Normal context switch (kernel-to-kernel or already-running user process)
        // CRITICAL: Handle CR3 switching for mixed kernel/user processes
        extern uint64_t vmm_get_pml4(void);
        
        // Update TSS.RSP0 for the target process so future syscalls use its kernel stack
        if (next->privilege == PRIV_USER) {
            uint64_t kernel_stack_top = (uint64_t)next->stack_base + next->stack_size;
            cpu_set_kernel_stack(kernel_stack_top);
        }

        if (next->privilege == PRIV_USER && next->cr3 != 0) {
            // Switching to a user process - load its CR3
            __asm__ volatile("mov %0, %%cr3" : : "r"(next->cr3) : "memory");
        } else if (prev && prev->privilege == PRIV_USER && prev->cr3 != 0) {
            // Switching FROM user process TO kernel process - restore kernel CR3
            uint64_t kernel_cr3 = vmm_get_pml4();
            __asm__ volatile("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
        }

        if (prev && prev->state != PROC_STATE_UNUSED) {
            { uint32_t __bd = bkl_release_all(); context_switch(&prev->rsp, next->rsp); bkl_reacquire(__bd); }
        } else {
            static uint64_t dummy_rsp;
            { uint32_t __bd = bkl_release_all(); context_switch(&dummy_rsp, next->rsp); bkl_reacquire(__bd); }
        }
    }

    // After returning from context switch, re-enable interrupts
    sti();
}

/**
 * Timer tick handler for scheduler
 * Called from timer interrupt
 */
void sched_tick(void) {
    sched_ticks++;
    { extern void cron_tick(void); cron_tick(); }  // #265 scheduler hook
    { extern void futex_tick(void); futex_tick(); } // #430 futex timeout hook
    g_cpu_total_acc++;
    if (!current_proc || current_proc->pid == 0) g_cpu_idle_acc++;
    if (g_cpu_total_acc >= 250) {            // ~1s window at 250 Hz
        int sample = 100 - (int)(g_cpu_idle_acc * 100 / g_cpu_total_acc);
        if (sample < 0) sample = 0;
        if (sample > 100) sample = 100;
        // EMA smoothing: the taskbar gauge and Task Manager both read this single
        // value (sys_get_cpu_usage). Smoothing keeps it stable so the two displays
        // show a matching reading instead of diverging on a jittery instantaneous
        // sample (#182).
        g_cpu_pct = (g_cpu_pct * 2 + sample) / 3;
        g_cpu_idle_acc = 0; g_cpu_total_acc = 0;
        // #279: window per-core CPU% too (core 0 = this aggregate, APs measured
        // by busy-tick deltas inside the SMP work loop).
        { extern void smp_account_core_usage(int); smp_account_core_usage(g_cpu_pct); }
    }

    if (!current_proc || !preemption_enabled) {
        return;
    }

    // Update time
    current_proc->total_time++;

    // Decrement time slice
    if (current_proc->time_slice > 0) {
        current_proc->time_slice--;
    }

    // If time slice expired, reschedule
    // Allow ALL processes including PID 0 to be preempted
    if (current_proc->time_slice == 0) {

        current_proc->state = PROC_STATE_READY;
        add_to_ready_queue(current_proc);
        sched_schedule(); // Run new process
    }
}

/**
 * Enable/disable preemption
 */
bool sched_set_preemption(bool enable) {
    bool old = preemption_enabled;
    preemption_enabled = enable;
    return old;
}

/**
 * Check if preemption is enabled
 */
bool sched_preemption_enabled(void) {
    return preemption_enabled;
}

// ============================================================================
// User-mode process support
// ============================================================================

/**
 * Create a user-mode process
 * For now, this is a stub that will be completed when ELF loading is ready
 */

// ============================================================================
// setup_user_argv: write argc/argv onto a user-mode stack in a foreign
// address space (target_cr3).  Returns the new user RSP value.
//
// Uses a temporary CR3 switch to write directly to user virtual addresses.
// The kernel stack and code are in kernel upper-half mappings (PML4 256-511)
// which are shared between all address spaces, so they remain valid.
//
// Stack layout (what crt0.asm expects):
//
//   High addr:
//     <string data>        argv strings packed here (null-terminated each)
//     <padding to 8-byte>
//     NULL                  envp terminator (future)
//     NULL                  argv terminator
//     &argv[argc-1]         pointers to strings (user-space virtual addrs)
//     ...
//     &argv[0]
//     argc (uint64_t)       <-- RSP points here
//   Low addr (16-byte aligned)
// ============================================================================
static uint64_t setup_user_argv(uint64_t target_cr3, uint64_t stack_top,
                                int argc, char **argv) {
    if (argc <= 0 || !argv) {
        // No args: push argc=0, 16-byte align
        // We still need to write to the user stack, so switch CR3
        uint64_t sp = (stack_top - 8) & ~0xFULL;

        // Mask interrupts across the foreign-CR3 window (see elf_load_user).
        uint64_t old_cr3, rflags;
        __asm__ volatile("pushfq; pop %0" : "=r"(rflags) :: "memory");
        __asm__ volatile("cli");
        __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(target_cr3) : "memory");

        *(volatile uint64_t *)sp = 0;  // argc = 0

        __asm__ volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
        __asm__ volatile("push %0; popfq" : : "r"(rflags) : "cc", "memory");
        return sp;
    }

    // Clamp argc
    if (argc > 64) argc = 64;

    // Phase 1: Calculate total string bytes needed
    uint64_t total_str = 0;
    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            const char *s = argv[i];
            int len = 0;
            while (s[len]) len++;
            total_str += len + 1;
        } else {
            total_str += 1;
        }
    }

    // Strings go at the top of the stack area
    uint64_t str_area = (stack_top - total_str) & ~0x7ULL;

    // Pointer array below string data:
    // argc + argv[0..argc-1] + NULL + NULL(envp)
    uint64_t ptrs_needed = 1 + argc + 1 + 1;
    uint64_t sp = (str_area - ptrs_needed * 8) & ~0xFULL;

    // Phase 2: Switch to target address space and write everything.
    // Mask interrupts across the foreign-CR3 window (see elf_load_user): an IRQ
    // handler must never run while CR3 points at the child address space.
    uint64_t old_cr3, rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags) :: "memory");
    __asm__ volatile("cli");
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(target_cr3) : "memory");

    // Write strings and record their user-space addresses
    uint64_t str_addrs[64];
    uint64_t cur = str_area;
    for (int i = 0; i < argc; i++) {
        str_addrs[i] = cur;
        const char *s = argv[i] ? argv[i] : "";
        while (*s) {
            *(volatile uint8_t *)cur = (uint8_t)*s;
            cur++;
            s++;
        }
        *(volatile uint8_t *)cur = 0;  // null terminator
        cur++;
    }

    // Write argc
    *(volatile uint64_t *)sp = (uint64_t)argc;

    // Write argv pointers
    for (int i = 0; i < argc; i++) {
        *(volatile uint64_t *)(sp + 8 + i * 8) = str_addrs[i];
    }

    // Write argv NULL terminator
    *(volatile uint64_t *)(sp + 8 + argc * 8) = 0;

    // Write envp NULL terminator
    *(volatile uint64_t *)(sp + 8 + (argc + 1) * 8) = 0;

    // Switch back to kernel address space
    __asm__ volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
    __asm__ volatile("push %0; popfq" : : "r"(rflags) : "cc", "memory");

    return sp;
}

int proc_create_user(const char *name, void *elf_data, uint64_t elf_size,
                     char **argv, char **envp) {
    (void)envp;

    if (!name || !elf_data || elf_size == 0) {
        LOG_ERROR("[Process] Invalid parameters to proc_create_user");
        return -1;
    }

    // Disable preemption during process creation
    bool old_preempt = sched_set_preemption(false);

    // Allocate process slot
    process_t *proc = alloc_proc_slot();
    if (!proc) {
        kprintf("[PROC] No free process slots for user process\n");
        LOG_ERROR("[Process] No free process slots");
        sched_set_preemption(old_preempt);
        return -1;
    }

    // Initialize process structure
    init_proc(proc, name, PRIO_NORMAL);
    proc->privilege = PRIV_USER;

    // Create user address space
    proc->cr3 = vmm_create_user_space();
    if (proc->cr3 == 0) {
        kprintf("[PROC] Failed to create address space for %s\n", name);
        LOG_ERROR("[Process] Failed to create address space");
        proc->state = PROC_STATE_UNUSED;
        sched_set_preemption(old_preempt);
        return -1;
    }

    // Allocate kernel stack (used during syscalls/interrupts)
    proc->stack_base = kmalloc(KERNEL_STACK_SIZE);
    if (!proc->stack_base) {
        kprintf("[PROC] Failed to allocate kernel stack for %s\n", name);
        LOG_ERROR("[Process] Failed to allocate kernel stack");
        vmm_destroy_user_space(proc->cr3);
        proc->state = PROC_STATE_UNUSED;
        sched_set_preemption(old_preempt);
        return -1;
    }
    proc->stack_size = KERNEL_STACK_SIZE;

    // Allocate user stack in user address space
    proc->user_stack_base = USER_STACK_TOP - USER_STACK_SIZE;
    proc->user_stack_size = USER_STACK_SIZE;

    // Map user stack pages
    uint64_t stack_pages = USER_STACK_SIZE / VMM_PAGE_SIZE_4K;
    if (vmm_alloc_user_pages(proc->cr3, proc->user_stack_base, stack_pages,
                             VMM_USER_RW) != 0) {
        kprintf("[PROC] Failed to allocate user stack for %s\n", name);
        LOG_ERROR("[Process] Failed to allocate user stack");
        kfree(proc->stack_base);
        vmm_destroy_user_space(proc->cr3);
        proc->state = PROC_STATE_UNUSED;
        sched_set_preemption(old_preempt);
        return -1;
    }

    // Load ELF binary into user address space
    uint64_t entry_point, load_base, load_end;
    int elf_result = elf_load_user(elf_data, elf_size, proc->cr3,
                                   &entry_point, &load_base, &load_end);
    if (elf_result != ELF_SUCCESS) {
        kprintf("[PROC] Failed to load ELF for %s: %s\n", name, elf_strerror(elf_result));
        LOG_ERROR("[Process] Failed to load ELF into user space");
        vmm_free_user_pages(proc->cr3, proc->user_stack_base, stack_pages);
        kfree(proc->stack_base);
        vmm_destroy_user_space(proc->cr3);
        proc->state = PROC_STATE_UNUSED;
        sched_set_preemption(old_preempt);
        return -1;
    }

    kprintf("[PROC] Loaded ELF: entry=0x%lx, base=0x%lx, end=0x%lx\n",
            entry_point, load_base, load_end);

    // Set up user mode entry point and stack
    proc->user_rip = entry_point;

    // Set up user stack with argc/argv
    uint64_t user_sp;
    if (argv) {
        // Count argc from argv array
        int argc = 0;
        while (argv[argc]) argc++;
        user_sp = setup_user_argv(proc->cr3, USER_STACK_TOP, argc, argv);
    } else {
        // No argv: push argc=0
        user_sp = setup_user_argv(proc->cr3, USER_STACK_TOP, 0, NULL);
    }

    proc->user_rsp = user_sp;

    // Set up kernel stack for initial IRET to user mode
    uint64_t kstack_top = (uint64_t)proc->stack_base + KERNEL_STACK_SIZE;
    kstack_top &= ~0xF;  // 16-byte align

    // Push interrupt frame for IRET to user mode:
    // SS, RSP, RFLAGS, CS, RIP (in reverse order on stack)
    kstack_top -= 8;
    *(uint64_t *)kstack_top = GDT_USER_DATA_RPL3;  // SS
    kstack_top -= 8;
    *(uint64_t *)kstack_top = proc->user_rsp;      // RSP
    kstack_top -= 8;
    *(uint64_t *)kstack_top = 0x202;               // RFLAGS (IF enabled)
    kstack_top -= 8;
    *(uint64_t *)kstack_top = GDT_USER_CODE_RPL3;  // CS
    kstack_top -= 8;
    *(uint64_t *)kstack_top = proc->user_rip;      // RIP

    // Push general purpose registers (all zero for new process)
    for (int i = 0; i < 15; i++) {  // 15 GPRs
        kstack_top -= 8;
        *(uint64_t *)kstack_top = 0;
    }

    proc->rsp = kstack_top;
    proc->context = NULL;
    proc->entry_point = NULL;  // Not used for user processes
    proc->entry_arg = NULL;

    proc->running_cpu = -1;
    int __mig = g_next_user_migratable ||
        (proc->name[0]=='s'&&proc->name[1]=='p'&&proc->name[2]=='i'&&proc->name[3]=='n');
    g_next_user_migratable = 0;  // one-shot
    if (__mig) { proc->migratable=1; smp_migq_push(proc); }
    else { proc->migratable=0; add_to_ready_queue(proc); }

    kprintf("[PROC] Created user process '%s' (PID %u) CR3=0x%lx\n",
            name, proc->pid, proc->cr3);

    sched_set_preemption(old_preempt);

    // Let the timer-driven preemptive scheduler handle context switching.
    // Calling sched_schedule() here would context_switch from the kernel
    // main thread (running as PID 0 / idle) which corrupts the stack
    // because the idle process's saved RSP points into the boot stack
    // rather than its own allocated stack.

    return proc->pid;
}

/**
 * Phase J: create a user process with /dev/pts/N wired to fds 0/1/2.
 *
 * `pts_idx` is the slave index returned by TIOCGPTN on a master opened
 * via dev_open("ptmx", ...). Preemption is held across the whole call so
 * the new process cannot be scheduled between init_proc() (which consults
 * g_tty_bind_pts_idx) and the ready-queue insertion inside
 * proc_create_user().
 *
 * Returns the PID on success, -1 on failure.
 */
int proc_create_user_tty(const char *name, void *elf_data, uint64_t elf_size,
                         int pts_idx) {
    if (pts_idx < 0 || pts_idx >= 8) return -1;
    bool old = sched_set_preemption(false);
    g_tty_bind_pts_idx = pts_idx;
    int pid = proc_create_user(name, elf_data, elf_size, NULL, NULL);
    g_tty_bind_pts_idx = -1;  // defensive: ensure not leaked if init_proc skipped
    sched_set_preemption(old);
    return pid;
}

/**
 * Fork child return trampoline.
 * After the child's first context_switch, it lands here.
 * Sets RAX=0 (child return value) and returns through the syscall path.
 */
extern void syscall_return_path(void);  // From syscall.asm

__attribute__((noreturn))
static void fork_child_return(void) {
    // Re-enable preemption (was disabled in the parent's proc_fork)
    sched_set_preemption(true);

    // Update TSS.RSP0 so future syscalls from this child use the correct stack
    process_t *me = proc_current();
    uint64_t my_stack_top = (uint64_t)me->stack_base + me->stack_size;
    cpu_set_kernel_stack(my_stack_top);

    // The child's kernel stack has a copy of the parent's syscall entry frame,
    // pushed from my_stack_top downward by syscall.asm. The layout:
    //   [stack_top - 1*8]  SS (0x1B)
    //   [stack_top - 2*8]  user RSP
    //   [stack_top - 3*8]  user RFLAGS (R11)
    //   [stack_top - 4*8]  CS (0x23)
    //   [stack_top - 5*8]  user RIP (RCX)
    //   [stack_top - 6*8]  RAX (syscall number, will become return value)
    //   ... 14 more GPRs ...
    //   [stack_top - 20*8] R15
    //   [stack_top - 21*8] alignment padding (0)
    //   [stack_top - 22*8] arg6
    //
    // syscall_return_path expects RSP at [stack_top - 22*8] and does:
    //   add rsp, 16 -> skip arg6+padding
    //   mov [rsp+14*8], rax -> store return value
    //   pop all GPRs, restore user state, sysret

    uint64_t return_rsp = my_stack_top - 22 * 8;

    __asm__ volatile(
        "xor %%eax, %%eax\n"      // RAX = 0 (fork child return value)
        "mov %0, %%rsp\n"         // Set stack to syscall return position
        "jmp *%1\n"               // Jump to syscall return path in syscall.asm
        :
        : "r"(return_rsp), "r"((uint64_t)syscall_return_path)
        : "memory", "rax"
    );
    __builtin_unreachable();
}

/**
 * Fork the current process
 *
 * Creates a child process that is a copy of the current (parent) process.
 * The parent receives the child's PID as return value.
 * The child receives 0 as return value (via fork_child_return trampoline).
 */
int proc_fork(void) {
    if (!current_proc) {
        return -1;
    }

    // Disable preemption during fork
    bool old_preempt = sched_set_preemption(false);

    // Allocate child process slot
    process_t *child = alloc_proc_slot();
    if (!child) {
        sched_set_preemption(old_preempt);
        return -1;
    }

    // Copy parent process structure
    memcpy(child, current_proc, sizeof(process_t));
    child->pid = next_pid++;
    child->ppid = current_proc->pid;
    child->state = PROC_STATE_READY;
    child->next = NULL;
    // The parent's wait_entry (if non-NULL) lives on the parent's kernel
    // stack, not the child's, so it is meaningless in the child. Clear it.
    child->wait_entry = NULL;
    // cwd[] was copied by the memcpy above, which is what POSIX fork()
    // requires: child inherits the parent's working directory.

    // Phase A3: fork() copies the fd table with refcount bumps; CLOEXEC is
    // NOT cleared here (per POSIX, fork inherits CLOEXEC; only execve closes
    // the CLOEXEC fds). The fd_cloexec bitmap was already copied by the
    // memcpy above.
    extern void file_get(struct file *f);
    for (int __fd = 0; __fd < MAX_FDS; __fd++) {
        if (child->fds[__fd]) file_get(child->fds[__fd]);
    }

    // Phase D1: POSIX fork() inherits signal handlers and the blocked-signal
    // mask, but clears pending signals and any return-work state in the
    // child. The memcpy above already copied handlers+mask; we only need
    // to zero the pending set and return_work bitmap.
    child->sig_pending = 0;
    child->return_work = 0;

    // #429: give the child its own demand-paging mm. Physical COW page sharing
    // is done by vmm_clone_user_space_cow() below; this duplicates the VMA
    // metadata so the child can still fault in inherited lazy mmap regions.
    {
        extern void *mm_dup(void *src);
        child->mm = current_proc->mm ? mm_dup(current_proc->mm) : (void *)0;
    }

    // Clone address space (copy-on-write share of all user pages)
    if (current_proc->privilege == PRIV_USER && current_proc->cr3 != 0) {
        child->cr3 = vmm_clone_user_space_cow(current_proc->cr3);  // #429 COW fork
        if (child->cr3 == 0) {
            child->state = PROC_STATE_UNUSED;
            sched_set_preemption(old_preempt);
            return -1;
        }
    }

    // Allocate new kernel stack for child
    child->stack_base = kmalloc(current_proc->stack_size);
    if (!child->stack_base) {
        if (child->cr3) vmm_destroy_user_space(child->cr3);
        child->state = PROC_STATE_UNUSED;
        sched_set_preemption(old_preempt);
        return -1;
    }
    child->stack_size = current_proc->stack_size;

    // Copy parent's kernel stack (includes the syscall entry frame)
    memcpy(child->stack_base, current_proc->stack_base, current_proc->stack_size);

    // Build a synthetic context_switch frame on the child's kernel stack.
    // When the scheduler picks the child and does context_switch(&prev->rsp, child->rsp),
    // context_switch will popfq + pop 15 GPRs + ret, landing in fork_child_return.
    //
    // context_switch frame layout (from saved RSP, matching pop order):
    //   [rsp + 0*8]  RFLAGS
    //   [rsp + 1*8]  R11
    //   [rsp + 2*8]  R10
    //   [rsp + 3*8]  R9
    //   [rsp + 4*8]  R8
    //   [rsp + 5*8]  RDI
    //   [rsp + 6*8]  RSI
    //   [rsp + 7*8]  RDX
    //   [rsp + 8*8]  RCX
    //   [rsp + 9*8]  RAX
    //   [rsp + 10*8] R15
    //   [rsp + 11*8] R14
    //   [rsp + 12*8] R13
    //   [rsp + 13*8] R12
    //   [rsp + 14*8] RBP
    //   [rsp + 15*8] RBX
    //   [rsp + 16*8] return address (popped by ret)
    //
    // Place the frame well below the syscall entry frame area (which uses
    // the top 22 qwords of the stack).

    uint64_t child_stack_top = (uint64_t)child->stack_base + child->stack_size;

    // 17 qwords: RFLAGS + 15 GPRs + return address
    // context_switch frame layout (from saved RSP, matching pop order):
    //   [rsp + 0*8]  RFLAGS
    //   [rsp + 1*8]  R11
    //   [rsp + 2*8]  R10
    //   [rsp + 3*8]  R9
    //   [rsp + 4*8]  R8
    //   [rsp + 5*8]  RDI
    //   [rsp + 6*8]  RSI
    //   [rsp + 7*8]  RDX
    //   [rsp + 8*8]  RCX
    //   [rsp + 9*8]  RAX
    //   [rsp + 10*8] R15
    //   [rsp + 11*8] R14
    //   [rsp + 12*8] R13
    //   [rsp + 13*8] R12
    //   [rsp + 14*8] RBP
    //   [rsp + 15*8] RBX
    //   [rsp + 16*8] return address (popped by ret)
    // SSE (256 bytes = 32 qwords) + RFLAGS + 15 GPRs + return address = 49 qwords
    uint64_t *frame = (uint64_t *)(child_stack_top - 72 * 8);
    memset(frame, 0, 49 * 8);

    // SSE area: frame[0..31] = 256 bytes of zeros (already zeroed by memset)
    frame[32] = 0x202;                          // RFLAGS: IF set, reserved bit 1 set
    frame[41] = 0;                              // RAX = 0 (child fork return value)
    frame[48] = (uint64_t)fork_child_return;    // Return address

    child->rsp = (uint64_t)frame;

    // Ensure child takes the context_switch path (not context_start/IRET)
    if (child->total_time == 0) child->total_time = 1;

    // Add child to ready queue
    add_to_ready_queue(child);

    kprintf("[PROC] Forked '%s' (PID %u -> child PID %u)\n",
            child->name, current_proc->pid, child->pid);

    sched_set_preemption(old_preempt);
    return child->pid;
}

// #430: CLONE_* flags we honor (subset of Linux, matching userland thread.h).
#define CLONE_VM            0x00000100
#define CLONE_THREAD        0x00010000
#define CLONE_SETTLS        0x00080000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID  0x01000000

/**
 * #430: clone() - create a thread sharing the caller's address space.
 *
 * This is proc_fork() with three changes: (1) with CLONE_VM the child SHARES
 * the parent's cr3 instead of deep-copying it; (2) the child returns onto a
 * caller-supplied user stack (stack_top) rather than the parent's; (3) TID
 * bookkeeping (CLONE_*_SETTID / CLONE_CHILD_CLEARTID) is honored so pthread
 * join/detach work. The child returns 0, the parent returns the new tid, and
 * both resume at the instruction after the SYSCALL - exactly the contract the
 * userland pthread_create()/clone() wrapper relies on.
 */
int proc_clone(uint32_t flags, void *user_stack, uint32_t *parent_tid,
               uint32_t *child_tid, void *tls) {
    (void)tls;  // CLONE_SETTLS: FS-base switching per task is not wired yet
                // (pthread_self() uses SYS_GETTID, TSD uses arrays), so a
                // NULL/zero tls is the only case the current libc exercises.
    if (!current_proc) return -1;
    // A raw clone with no shared stack is just fork(); route it there.
    if (!(flags & CLONE_VM) || user_stack == NULL) {
        return proc_fork();
    }

    bool old_preempt = sched_set_preemption(false);

    process_t *child = alloc_proc_slot();
    if (!child) {
        sched_set_preemption(old_preempt);
        return -1;
    }

    // Copy parent structure, then fix up the thread-specific fields.
    memcpy(child, current_proc, sizeof(process_t));
    child->pid = next_pid++;
    child->ppid = current_proc->pid;
    child->state = PROC_STATE_READY;
    child->next = NULL;
    child->wait_entry = NULL;
    child->sig_pending = 0;
    child->return_work = 0;

    // Thread group: leader is the caller's tgid (or the caller itself).
    child->tgid = current_proc->tgid ? current_proc->tgid : current_proc->pid;

    // Bump refcounts on inherited fds (CLONE_FILES sharing is approximated as
    // fork-style inheritance; adequate for the current libc).
    extern void file_get(struct file *f);
    for (int __fd = 0; __fd < MAX_FDS; __fd++) {
        if (child->fds[__fd]) file_get(child->fds[__fd]);
    }

    // (1) SHARE the address space - do NOT clone it. Mark the child so its
    // cleanup never destroys the shared cr3 or frees the shared user stack.
    child->cr3 = current_proc->cr3;
    child->shares_vm = 1;
    child->user_stack_base = 0;   // owned by the leader, not this thread
    child->user_stack_size = 0;

    // Allocate + copy a fresh kernel stack (the child needs its own kernel
    // stack for syscalls; the copy carries the parent's syscall-entry frame).
    child->stack_base = kmalloc(current_proc->stack_size);
    if (!child->stack_base) {
        child->state = PROC_STATE_UNUSED;
        child->cr3 = 0; child->shares_vm = 0;
        sched_set_preemption(old_preempt);
        return -1;
    }
    child->stack_size = current_proc->stack_size;
    memcpy(child->stack_base, current_proc->stack_base, current_proc->stack_size);

    // (2) The copied kernel stack holds a duplicate of the parent's syscall
    // entry frame at its top. syscall.asm pushed, from the very top down:
    //   [top-1*8]=SS  [top-2*8]=userRSP  [top-3*8]=RFLAGS  [top-4*8]=CS
    //   [top-5*8]=RIP  [top-6*8..top-20*8]=15 GPRs
    // Overwrite the saved user RSP so the child returns onto its own stack.
    uint64_t child_stack_top = (uint64_t)child->stack_base + child->stack_size;
    *(uint64_t *)(child_stack_top - 2 * 8) = (uint64_t)user_stack;

    // Build the synthetic context_switch frame (identical layout to fork) so
    // the scheduler lands in fork_child_return, which sets RAX=0 and jumps to
    // syscall_return_path (SYSRET onto the new user stack, same user RIP).
    uint64_t *frame = (uint64_t *)(child_stack_top - 72 * 8);
    memset(frame, 0, 49 * 8);
    frame[32] = 0x202;                          // RFLAGS
    frame[41] = 0;                              // RAX = 0 (child return value)
    frame[48] = (uint64_t)fork_child_return;    // return address
    child->rsp = (uint64_t)frame;

    // Take the context_switch (not context_start/IRET) path.
    if (child->total_time == 0) child->total_time = 1;

    // (3) TID bookkeeping. Parent and child share the address space, so these
    // user-memory writes are valid from the parent's context here.
    if ((flags & CLONE_PARENT_SETTID) && parent_tid) *parent_tid = child->pid;
    if ((flags & CLONE_CHILD_SETTID) && child_tid)   *child_tid  = child->pid;
    child->clear_child_tid =
        (flags & CLONE_CHILD_CLEARTID) ? child_tid : NULL;

    add_to_ready_queue(child);

    kprintf("[PROC] Cloned thread of '%s' (PID %u -> tid %u) flags=0x%x\n",
            child->name, current_proc->pid, child->pid, flags);

    sched_set_preemption(old_preempt);
    return (int)child->pid;
}

uint32_t proc_gettid(void) {
    process_t *p = proc_current();
    return p ? p->pid : 0;
}

uint32_t proc_set_tid_address(uint32_t *tidptr) {
    process_t *p = proc_current();
    if (!p) return 0;
    // #503 / MAYTERA-SEC-2026-0016: do not RECORD an address the kernel would
    // not be allowed to write. What is stored here is written (as 4 zero bytes)
    // at thread exit, far from any dispatcher check, so an unvalidated store is
    // a deferred arbitrary write. NULL is legal and means "clear it".
    // See the matching checks in proc/thread.c (store + immediately-before-write).
    if (tidptr && validate_user_ptr(tidptr, sizeof(uint32_t),
                                    ACCESS_RW_USER) != VALIDATE_OK) {
        kprintf("[PROC] pid %u: set_tid_address(%p) rejected (not user-writable)\n",
                p->pid, (void *)tidptr);
        p->clear_child_tid = NULL;
        return p->pid;
    }
    p->clear_child_tid = tidptr;
    return p->pid;
}

/**
 * Execute a new program (placeholder)
 */
int proc_exec(const char *path, char **argv, char **envp) {
    (void)path;
    (void)argv;
    (void)envp;

    // TODO: Implement exec when ELF loader is ready
    kprintf("[PROC] exec() not yet implemented\n");
    return -1;
}

// ============================================================================
// Phase G: real execve
// ============================================================================
//
// proc_execve_arm() loads a new image into a freshly-allocated address space
// and stashes the new cr3/rip/rsp on the PCB, armed by RETURN_WORK_EXECPENDING.
// The old address space is NOT destroyed here; we need it to survive until
// the syscall finishes (since we're still executing on code mapped in it).
// proc_execve_finalize() at syscall-return swaps CR3 and tears down the old
// address space.

extern fat_fs_t g_fat_fs;
extern void fd_close_cloexec(void);

#ifndef USER_STACK_TOP
#define USER_STACK_TOP  0x00007FFFFFFFE000ULL
#endif

int proc_execve_arm(const char *path, char **argv, char **envp) {
    (void)argv;  // argv/envp construction is deferred; MVP runs static ELFs.
    (void)envp;

    if (!path) return -1;
    process_t *cur = proc_current();
    if (!cur || cur->privilege != PRIV_USER) return -1;

    if (!g_fat_fs.mounted) return -1;

    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, path, &size);
    if (!data || size == 0) return -1;

    if (elf_validate(data, size) != 0) {
        kfree(data);
        return -1;
    }

    // Build the new address space.
    uint64_t new_cr3 = vmm_create_user_space();
    if (new_cr3 == 0) { kfree(data); return -1; }

    // Allocate a fresh user stack.
    uint64_t stack_base  = USER_STACK_TOP - USER_STACK_SIZE;
    uint64_t stack_pages = USER_STACK_SIZE / VMM_PAGE_SIZE_4K;
    if (vmm_alloc_user_pages(new_cr3, stack_base, stack_pages,
                             VMM_USER_RW) != 0) {
        vmm_destroy_user_space(new_cr3);
        kfree(data);
        return -1;
    }

    // Load the ELF into it.
    uint64_t entry = 0, base = 0, end = 0;
    if (elf_load_user(data, size, new_cr3, &entry, &base, &end) != 0) {
        vmm_destroy_user_space(new_cr3);
        kfree(data);
        return -1;
    }
    kfree(data);

    // Stash for proc_execve_finalize.
    cur->exec_new_cr3 = new_cr3;
    cur->exec_new_rip = entry;
    cur->exec_new_rsp = (USER_STACK_TOP - 16) & ~0xFULL;
    cur->exec_old_cr3 = cur->cr3;
    cur->exec_new_user_stack_base = stack_base;
    cur->exec_new_user_stack_size = USER_STACK_SIZE;

    cur->return_work |= RETURN_WORK_EXECPENDING;
    return 0;
}

// The in-assembly saved frame layout that signal.c also uses. We redefine
// it locally so process.c doesn't need to include signal.c's header.
typedef struct exec_saved_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip;       // saved user RIP that will IRETQ
    uint64_t cs;
    uint64_t rflags;
    uint64_t user_rsp;  // saved user RSP
    uint64_t ss;
} exec_saved_frame_t;

void proc_execve_finalize(void *user_frame) {
    exec_saved_frame_t *sf = (exec_saved_frame_t *)user_frame;
    process_t *p = proc_current();
    if (!p) return;
    if (!(p->return_work & RETURN_WORK_EXECPENDING)) return;
    if (p->exec_new_cr3 == 0) {
        p->return_work &= ~RETURN_WORK_EXECPENDING;
        return;
    }

    uint64_t old_cr3 = p->exec_old_cr3;
    uint64_t new_cr3 = p->exec_new_cr3;

    // Swap address space. The kernel stack is kmalloc'd (kernel mapping)
    // so it survives the CR3 change. Our own code pages are mapped in both
    // old and new because they're kernel mappings (upper half PML4 entries
    // 256..511 are shared). After this MOV we can no longer touch the old
    // user-mode mappings.
    __asm__ volatile ("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
    p->cr3 = new_cr3;

    // Rewrite saved IRET frame so SYSRET/IRETQ lands at the new entry.
    sf->rip      = p->exec_new_rip;
    sf->user_rsp = p->exec_new_rsp;

    // Update PCB user-mode bookkeeping.
    p->user_rip        = p->exec_new_rip;
    p->user_rsp        = p->exec_new_rsp;
    p->user_stack_base = p->exec_new_user_stack_base;
    p->user_stack_size = p->exec_new_user_stack_size;

    // Destroy the old address space now that nothing references its user
    // mappings.
    if (old_cr3 && old_cr3 != new_cr3) {
        vmm_destroy_user_space(old_cr3);
    }

    // POSIX: execve resets all signal handlers to SIG_DFL unless they were
    // SIG_IGN, in which case they stay ignored. Pending signals are cleared.
    // Mask is preserved.
    for (int i = 0; i < 64; i++) {
        if (p->sig_handlers[i] != (void *)1ULL) {  // SIG_IGN
            p->sig_handlers[i] = 0;  // SIG_DFL
        }
        p->sig_flags[i] = 0;
        p->sig_handler_mask[i] = 0;
    }
    p->sig_pending = 0;

    // Close all fds with FD_CLOEXEC set.
    fd_close_cloexec();

    // Clear the arm bits.
    p->exec_new_cr3 = 0;
    p->exec_new_rip = 0;
    p->exec_new_rsp = 0;
    p->exec_old_cr3 = 0;
    p->return_work &= ~RETURN_WORK_EXECPENDING;
}

/**
 * Enter user mode via IRET
 * This is typically called after setting up the stack frame
 */
void proc_enter_usermode(uint64_t entry_rip, uint64_t user_rsp) {
    // Set up TSS kernel stack for this process
    uint64_t kstack = (uint64_t)current_proc->stack_base + current_proc->stack_size;
    cpu_set_kernel_stack(kstack);

    // Switch to user address space
    if (current_proc->cr3 != 0) {
        vmm_switch_pml4(current_proc->cr3);
    }

    // Build IRET frame on stack and execute IRET
    // This is done in assembly for precise control
    __asm__ volatile(
        "cli\n"
        "mov %0, %%rax\n"           // User RIP
        "mov %1, %%rcx\n"           // User RSP
        "mov %2, %%rdx\n"           // User CS
        "mov %3, %%rbx\n"           // User SS

        "push %%rbx\n"              // Push SS
        "push %%rcx\n"              // Push RSP
        "pushf\n"                   // Push RFLAGS
        "pop %%r8\n"
        "or $0x200, %%r8\n"         // Enable interrupts
        "push %%r8\n"
        "push %%rdx\n"              // Push CS
        "push %%rax\n"              // Push RIP

        "xor %%rax, %%rax\n"        // Clear registers
        "xor %%rbx, %%rbx\n"
        "xor %%rcx, %%rcx\n"
        "xor %%rdx, %%rdx\n"
        "xor %%rsi, %%rsi\n"
        "xor %%rdi, %%rdi\n"
        "xor %%rbp, %%rbp\n"
        "xor %%r8, %%r8\n"
        "xor %%r9, %%r9\n"
        "xor %%r10, %%r10\n"
        "xor %%r11, %%r11\n"
        "xor %%r12, %%r12\n"
        "xor %%r13, %%r13\n"
        "xor %%r14, %%r14\n"
        "xor %%r15, %%r15\n"

        "iretq\n"
        :
        : "r"(entry_rip),
          "r"(user_rsp),
          "r"((uint64_t)GDT_USER_CODE_RPL3),
          "r"((uint64_t)GDT_USER_DATA_RPL3)
        : "memory", "rax"
    );

    // Never reached
    __builtin_unreachable();
}

/**
 * Check if current process is user mode
 */
bool proc_is_usermode(void) {
    return current_proc && current_proc->privilege == PRIV_USER;
}

/**
 * Get current process CR3
 */
uint64_t proc_get_cr3(void) {
    return current_proc ? current_proc->cr3 : 0;
}

// Return current process name (for debugging)
const char *proc_current_name(void) {
    if (current_process && current_process->name[0])
        return current_process->name;
    return "kernel";
}


// #487: narrow pid accessor (see process.h). Returns 0 when there is no current
// process, which callers treat as "unowned".
uint32_t proc_current_pid(void) {
    process_t *p = proc_current();
    return p ? p->pid : 0;
}

// SYS_PROC_LIST backend: snapshot the live process table for Task Manager.
int proc_snapshot(proc_info_t *out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < MAX_PROCESSES && n < max; i++) {
        process_t *p = &proc_table[i];
        if (p->state == PROC_STATE_UNUSED) continue;
        out[n].pid  = p->pid;
        out[n].ppid = p->ppid;
        int j = 0;
        for (; j < 31 && p->name[j]; j++) out[n].name[j] = p->name[j];
        out[n].name[j] = 0;
        out[n].state = (uint32_t)p->state;
        out[n].cpu_ticks = p->total_time;
        // #487: mem_kb used to be p->user_stack_size alone, so EVERY user
        // process reported a flat ~2 MB (USER_STACK_SIZE) and the Task Manager
        // Memory column was decorative. It is now the real working set
        // (resident demand-paged frames + the committed user stack), computed
        // by the Rust seam under -DRUST_PROC_MEM. Same field, same width, same
        // syscall ABI (proc_info_t is shared with userland libc/syscall.h):
        // only the SEMANTICS improve, so existing userland consumers keep
        // working and simply get a truthful number.
        proc_mem_in_t mi;
        proc_mem_out_t mo;
        proc_mem_fill_in(p, &mi);
        out[n].mem_kb = (proc_mem_account(&mi, &mo) == 1) ? mo.working_set_kb : 0;
        out[n].running_cpu = p->running_cpu;
        n++;
    }
    return n;
}

// #404/#349 Task Manager: count the threads in a process's thread group. In
// this kernel a thread is a process_t with shares_vm=1 whose tgid names the
// group leader; the leader itself has shares_vm=0 and pid==tgid. Count the
// leader plus every thread that names this pid as its tgid. A normal
// single-threaded process returns 1. Read-only; safe to call from the
// compositor task-manager draw path (no allocation, bounded by MAX_PROCESSES).
uint32_t proc_thread_count(uint32_t pid) {
    uint32_t count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &proc_table[i];
        if (p->state == PROC_STATE_UNUSED) continue;
        if (p->pid == pid || (p->shares_vm && p->tgid == pid)) count++;
    }
    return count ? count : 1;
}
