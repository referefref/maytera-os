// thread.c - Thread management implementation for MayteraOS
// Part of Task #25 (Threading with clone() syscall)

#include "thread.h"
#include "process.h"
#include "../mm/heap.h"
#include "../security/validate.h"   // #503: deferred-write validation (clear_child_tid)
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../cpu/gdt.h"
#include "../serial.h"
#include "../string.h"
#include "../sync/spinlock.h"

// ============================================================================
// Global State
// ============================================================================

// Thread table (all threads in system)
#define MAX_THREADS 256
static thread_t thread_table[MAX_THREADS];

// Thread table lock
static spinlock_t thread_table_lock = SPINLOCK_INIT;

// Current running thread (per-CPU in SMP, single for now)
static thread_t *current_thread = NULL;

// Ready queue
static thread_t *ready_queue_head = NULL;
static thread_t *ready_queue_tail = NULL;
static spinlock_t ready_queue_lock = SPINLOCK_INIT;

// Thread ID counter
static uint32_t next_tid = 1;

// Scheduler tick counter
static uint64_t thread_sched_ticks = 0;

// Time slice duration
#define THREAD_TIME_SLICE 10  // ~100ms at 100Hz

// External timer ticks
extern volatile uint64_t timer_ticks;

// ============================================================================
// Internal Functions
// ============================================================================

/**
 * Allocate a thread slot from the table
 */
static thread_t *alloc_thread_slot(void) {
    spinlock_acquire(&thread_table_lock);

    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state == THREAD_STATE_UNUSED) {
            thread_table[i].state = THREAD_STATE_READY;  // Mark as used
            spinlock_release(&thread_table_lock);
            return &thread_table[i];
        }
    }

    spinlock_release(&thread_table_lock);
    return NULL;
}

/**
 * Free a thread slot
 */
static void free_thread_slot(thread_t *thread) {
    if (!thread) return;

    spinlock_acquire(&thread_table_lock);
    thread->state = THREAD_STATE_UNUSED;
    spinlock_release(&thread_table_lock);
}

/**
 * Initialize a thread structure
 */
static void init_thread(thread_t *thread, process_t *proc, process_priority_t prio) {
    memset(thread, 0, sizeof(thread_t));

    thread->tid = next_tid++;
    thread->tgid = proc ? proc->pid : 0;
    thread->process = proc;
    thread->state = THREAD_STATE_READY;
    thread->priority = prio;
    thread->time_slice = THREAD_TIME_SLICE;
    thread->total_time = 0;
    thread->detached = false;
    thread->next = NULL;
    thread->group_next = NULL;
    thread->group_prev = NULL;
}

/**
 * Add thread to ready queue (priority-based insertion)
 */
static void add_to_ready_queue_internal(thread_t *thread) {
    thread->next = NULL;
    thread->state = THREAD_STATE_READY;

    if (ready_queue_head == NULL) {
        ready_queue_head = ready_queue_tail = thread;
        return;
    }

    // Insert based on priority (higher priority first)
    if (thread->priority > ready_queue_head->priority) {
        thread->next = ready_queue_head;
        ready_queue_head = thread;
        return;
    }

    // Find insertion point
    thread_t *prev = NULL;
    thread_t *curr = ready_queue_head;
    while (curr && curr->priority >= thread->priority) {
        prev = curr;
        curr = curr->next;
    }

    if (prev) {
        thread->next = prev->next;
        prev->next = thread;
        if (prev == ready_queue_tail) {
            ready_queue_tail = thread;
        }
    }
}

/**
 * Remove and return highest priority ready thread
 */
static thread_t *remove_from_ready_queue_internal(void) {
    if (ready_queue_head == NULL) {
        return NULL;
    }

    thread_t *thread = ready_queue_head;
    ready_queue_head = thread->next;
    if (ready_queue_head == NULL) {
        ready_queue_tail = NULL;
    }
    thread->next = NULL;
    return thread;
}

/**
 * Thread wrapper function - called when a thread starts
 */
static void thread_wrapper(void) {
    thread_t *self = current_thread;
    if (!self || !self->entry_fn) {
        kprintf("[THREAD] Error: invalid thread state in wrapper\n");
        thread_exit(-1);
    }

    // Enable interrupts for thread execution
    sti();

    // Call the actual thread function
    self->entry_fn(self->entry_arg);

    // Thread returned - exit normally
    thread_exit(0);
}

/**
 * Add a thread to its process's thread group list
 */
static void add_to_thread_group(thread_t *thread, process_t *proc) {
    if (!thread || !proc) return;

    // For now, just link to process
    // Future: implement proper thread_group_t management
    thread->process = proc;
}

/**
 * Remove a thread from its process's thread group list
 */
static void remove_from_thread_group(thread_t *thread) {
    if (!thread) return;

    // Unlink from group list
    if (thread->group_prev) {
        thread->group_prev->group_next = thread->group_next;
    }
    if (thread->group_next) {
        thread->group_next->group_prev = thread->group_prev;
    }
    thread->group_next = NULL;
    thread->group_prev = NULL;
}

// ============================================================================
// Public API Implementation
// ============================================================================

void thread_init(void) {
    kprintf("[THREAD] Initializing thread subsystem...\n");

    // Clear thread table
    memset(thread_table, 0, sizeof(thread_table));

    // Initialize locks
    spinlock_init(&thread_table_lock);
    spinlock_init(&ready_queue_lock);

    // Reset counters
    next_tid = 1;
    thread_sched_ticks = 0;

    // Create initial thread for current execution context
    // This wraps the existing kernel execution as "thread 0"
    thread_t *init_thread = &thread_table[0];
    init_thread->tid = 0;
    init_thread->tgid = 0;
    init_thread->state = THREAD_STATE_RUNNING;
    init_thread->priority = PRIO_NORMAL;
    init_thread->time_slice = THREAD_TIME_SLICE;
    current_thread = init_thread;

    kprintf("[THREAD] Thread subsystem initialized\n");
    kprintf("[THREAD] Max threads: %d, Stack size: %d KB\n",
            MAX_THREADS, THREAD_STACK_SIZE / 1024);
}

int thread_create(uint32_t flags, void *stack, uint32_t *parent_tid,
                  uint32_t *child_tid, void *tls) {
    // Get current process
    process_t *proc = proc_current();
    if (!proc) {
        kprintf("[THREAD] Cannot create thread: no current process\n");
        return -1;
    }

    // Allocate thread slot
    thread_t *thread = alloc_thread_slot();
    if (!thread) {
        kprintf("[THREAD] No free thread slots\n");
        return -1;
    }

    // Initialize thread
    init_thread(thread, proc, proc->priority);

    // Handle clone flags
    if (flags & CLONE_VM) {
        // Share address space - use same CR3
        // Thread inherits process's address space
    }

    if (flags & CLONE_THREAD) {
        // Same thread group
        thread->tgid = proc->pid;
    } else {
        // New thread group (fork-like behavior)
        thread->tgid = thread->tid;
    }

    // Allocate kernel stack
    thread->stack_base = kmalloc(THREAD_STACK_SIZE);
    if (!thread->stack_base) {
        kprintf("[THREAD] Failed to allocate stack for TID %u\n", thread->tid);
        free_thread_slot(thread);
        return -1;
    }
    thread->stack_size = THREAD_STACK_SIZE;

    // Set up user stack if provided
    if (stack) {
        thread->user_stack_base = (uint64_t)stack - THREAD_STACK_SIZE;
        thread->user_stack_size = THREAD_STACK_SIZE;
        thread->user_rsp = (uint64_t)stack;
    }

    // Set up TLS if requested
    if ((flags & CLONE_SETTLS) && tls) {
        tls_desc_t *tls_desc = (tls_desc_t *)tls;
        thread->tls.base = tls_desc->base;
        thread->tls.size = tls_desc->size;
        thread->tls.flags = tls_desc->flags;

        // Allocate kernel-side TLS storage if needed
        if (tls_desc->size > 0) {
            thread->tls_base = kmalloc(tls_desc->size);
            if (thread->tls_base) {
                memset(thread->tls_base, 0, tls_desc->size);
            }
        }
    }

    // Set up TID addresses
    if ((flags & CLONE_PARENT_SETTID) && parent_tid) {
        *parent_tid = thread->tid;
    }

    if (flags & CLONE_CHILD_SETTID) {
        thread->set_child_tid = child_tid;
    }

    if (flags & CLONE_CHILD_CLEARTID) {
        thread->clear_child_tid = child_tid;
    }

    // Set up kernel stack for initial context switch
    uint64_t stack_top = (uint64_t)thread->stack_base + THREAD_STACK_SIZE;
    stack_top &= ~0xF;  // 16-byte align

    // Push return address (thread_wrapper)
    stack_top -= 8;
    *(uint64_t *)stack_top = (uint64_t)thread_wrapper;

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

    thread->rsp = stack_top;

    // Add to thread group
    add_to_thread_group(thread, proc);

    // Add to ready queue
    spinlock_acquire(&ready_queue_lock);
    add_to_ready_queue_internal(thread);
    spinlock_release(&ready_queue_lock);

    kprintf("[THREAD] Created thread TID %u (TGID %u) flags=0x%x\n",
            thread->tid, thread->tgid, flags);

    return thread->tid;
}

int thread_create_kernel(const char *name, void (*entry)(void *), void *arg,
                         process_priority_t priority) {
    (void)name;  // For future use in debugging

    // Allocate thread slot
    thread_t *thread = alloc_thread_slot();
    if (!thread) {
        kprintf("[THREAD] No free thread slots for kernel thread\n");
        return -1;
    }

    // Initialize as kernel thread (no process)
    init_thread(thread, NULL, priority);
    thread->entry_fn = entry;
    thread->entry_arg = arg;

    // Allocate kernel stack
    thread->stack_base = kmalloc(THREAD_STACK_SIZE);
    if (!thread->stack_base) {
        kprintf("[THREAD] Failed to allocate stack for kernel thread\n");
        free_thread_slot(thread);
        return -1;
    }
    thread->stack_size = THREAD_STACK_SIZE;

    // Set up kernel stack for initial context switch
    uint64_t stack_top = (uint64_t)thread->stack_base + THREAD_STACK_SIZE;
    stack_top &= ~0xF;  // 16-byte align

    // Push return address (thread_wrapper)
    stack_top -= 8;
    *(uint64_t *)stack_top = (uint64_t)thread_wrapper;

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

    thread->rsp = stack_top;

    // Add to ready queue
    spinlock_acquire(&ready_queue_lock);
    add_to_ready_queue_internal(thread);
    spinlock_release(&ready_queue_lock);

    kprintf("[THREAD] Created kernel thread TID %u\n", thread->tid);

    return thread->tid;
}

void thread_exit(int exit_code) {
    thread_t *self = current_thread;
    if (!self) {
        kprintf("[THREAD] thread_exit called with no current thread\n");
        while (1) hlt();
    }

    cli();

    kprintf("[THREAD] Thread TID %u exiting with code %d\n", self->tid, exit_code);

    // Store exit code
    self->exit_code = exit_code;
    self->state = THREAD_STATE_ZOMBIE;

    // Clear TID at specified address if CLONE_CHILD_CLEARTID was set.
    //
    // #503 / MAYTERA-SEC-2026-0016: this is a DEFERRED ARBITRARY WRITE. Ring 3
    // hands the kernel an address via SYS_SET_TID_ADDRESS / SYS_CLONE and the
    // kernel writes 4 zero bytes to it HERE, at thread exit, arbitrarily far
    // from any dispatcher check. The syscall argtab cannot cover this shape: a
    // descriptor proves the pointer valid AT DISPATCH, and this write happens
    // much later, by which time the caller may have unmapped or remapped the
    // page. So it is validated at both ends: at store time (thread_set_tid_
    // address / proc_set_tid_address, which refuse to record a non-user-writable
    // pointer) and again HERE, immediately before the write, which is the check
    // that actually holds because it is the one with no window after it.
    //
    // We are on the exiting thread with its own CR3 still loaded, so
    // validate_user_ptr() walks the right address space. It only walks page
    // tables (no locks, no allocation), so it is safe under cli().
    if (self->clear_child_tid) {
        if (validate_user_ptr(self->clear_child_tid, sizeof(uint32_t),
                              ACCESS_RW_USER) == VALIDATE_OK) {
            *self->clear_child_tid = 0;

            // Wake any threads waiting on futex at this address
            // (futex_wake will be called from futex.c)
            extern void futex_wake_addr(uint32_t *addr, int count);
            futex_wake_addr(self->clear_child_tid, 1);
        } else {
            // Not a user-writable address any more (or never was). Dropping the
            // write is the only safe action: performing it would be a Ring-0
            // write to an address Ring 3 chose.
            kprintf("[THREAD] TID %u: clear_child_tid %p not user-writable at exit; write dropped\n",
                    self->tid, (void *)self->clear_child_tid);
        }
    }

    // Wake any thread waiting to join this one
    if (self->joining_thread) {
        spinlock_acquire(&ready_queue_lock);
        self->joining_thread->state = THREAD_STATE_READY;
        add_to_ready_queue_internal(self->joining_thread);
        spinlock_release(&ready_queue_lock);
    }

    // If detached, clean up immediately
    if (self->detached) {
        // Free resources
        if (self->stack_base) {
            kfree(self->stack_base);
            self->stack_base = NULL;
        }
        if (self->tls_base) {
            kfree(self->tls_base);
            self->tls_base = NULL;
        }

        remove_from_thread_group(self);
        self->state = THREAD_STATE_DEAD;
        free_thread_slot(self);
    }

    // Schedule next thread
    thread_schedule();

    // Should never reach here
    while (1) hlt();
    __builtin_unreachable();
}

int thread_join(uint32_t tid, int *exit_code) {
    thread_t *target = thread_get(tid);
    if (!target) {
        return -1;  // Thread not found
    }

    if (target->detached) {
        return -1;  // Cannot join detached thread
    }

    thread_t *self = current_thread;
    if (!self || self == target) {
        return -1;  // Cannot join self
    }

    // Wait for thread to exit
    cli();

    if (target->state != THREAD_STATE_ZOMBIE && target->state != THREAD_STATE_DEAD) {
        // Block until target exits
        self->state = THREAD_STATE_BLOCKED;
        target->joining_thread = self;

        thread_schedule();

        // Woken up - target should be zombie now
    }

    // Get exit code
    if (exit_code) {
        *exit_code = target->exit_code;
    }

    // Clean up target thread
    if (target->stack_base) {
        kfree(target->stack_base);
        target->stack_base = NULL;
    }
    if (target->tls_base) {
        kfree(target->tls_base);
        target->tls_base = NULL;
    }

    remove_from_thread_group(target);
    target->state = THREAD_STATE_DEAD;
    free_thread_slot(target);

    sti();
    return 0;
}

int thread_detach(uint32_t tid) {
    thread_t *target = thread_get(tid);
    if (!target) {
        return -1;
    }

    target->detached = true;

    // If already zombie, clean up now
    if (target->state == THREAD_STATE_ZOMBIE) {
        if (target->stack_base) {
            kfree(target->stack_base);
            target->stack_base = NULL;
        }
        if (target->tls_base) {
            kfree(target->tls_base);
            target->tls_base = NULL;
        }

        remove_from_thread_group(target);
        target->state = THREAD_STATE_DEAD;
        free_thread_slot(target);
    }

    return 0;
}

thread_t *thread_current(void) {
    return current_thread;
}

thread_t *thread_get(uint32_t tid) {
    spinlock_acquire(&thread_table_lock);

    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state != THREAD_STATE_UNUSED &&
            thread_table[i].tid == tid) {
            spinlock_release(&thread_table_lock);
            return &thread_table[i];
        }
    }

    spinlock_release(&thread_table_lock);
    return NULL;
}

uint32_t thread_gettid(void) {
    return current_thread ? current_thread->tid : 0;
}

uint32_t thread_set_tid_address(uint32_t *tidptr) {
    if (current_thread) {
        // #503: refuse to RECORD an address the kernel would not be allowed to
        // write. NULL is legal and means "clear it" (POSIX/Linux semantics), so
        // it is stored as-is. Anything non-NULL that is not user-writable is
        // dropped rather than stored: see the deferred-write note in
        // thread_exit(). The tid is still returned, because set_tid_address()
        // does not have a failure return and callers do not check one.
        if (tidptr && validate_user_ptr(tidptr, sizeof(uint32_t),
                                        ACCESS_RW_USER) != VALIDATE_OK) {
            kprintf("[THREAD] TID %u: set_tid_address(%p) rejected (not user-writable)\n",
                    current_thread->tid, (void *)tidptr);
            current_thread->clear_child_tid = NULL;
            return current_thread->tid;
        }
        current_thread->clear_child_tid = tidptr;
        return current_thread->tid;
    }
    return 0;
}

void thread_yield(void) {
    cli();

    if (current_thread && current_thread->state == THREAD_STATE_RUNNING) {
        spinlock_acquire(&ready_queue_lock);
        current_thread->state = THREAD_STATE_READY;
        add_to_ready_queue_internal(current_thread);
        spinlock_release(&ready_queue_lock);
    }

    thread_schedule();
}

void thread_sleep(uint32_t ms) {
    if (!current_thread || ms == 0) return;

    cli();

    // Calculate wake time (assuming 100Hz timer)
    uint64_t ticks = (ms + 9) / 10;  // Round up
    current_thread->wake_time = timer_ticks + ticks;
    current_thread->state = THREAD_STATE_SLEEPING;

    thread_schedule();
}

// ============================================================================
// Thread-Local Storage
// ============================================================================

int thread_set_tls(uint64_t base, uint32_t size) {
    if (!current_thread) return -1;

    current_thread->tls.base = base;
    current_thread->tls.size = size;

    // Set FS base register for TLS access
    // Use WRFSBASE if available, otherwise use MSR
    extern void cpu_set_fs_base(uint64_t base);
    cpu_set_fs_base(base);

    return 0;
}

uint64_t thread_get_tls(void) {
    return current_thread ? current_thread->tls.base : 0;
}

// ============================================================================
// Scheduler Integration
// ============================================================================

void thread_enqueue(thread_t *thread) {
    if (!thread) return;

    spinlock_acquire(&ready_queue_lock);
    add_to_ready_queue_internal(thread);
    spinlock_release(&ready_queue_lock);
}

thread_t *thread_dequeue(void) {
    spinlock_acquire(&ready_queue_lock);
    thread_t *thread = remove_from_ready_queue_internal();
    spinlock_release(&ready_queue_lock);
    return thread;
}

void thread_sched_tick(void) {
    thread_sched_ticks++;

    if (!current_thread) return;

    // Update total time
    current_thread->total_time++;

    // Check for sleeping threads to wake
    spinlock_acquire(&thread_table_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *t = &thread_table[i];
        if (t->state == THREAD_STATE_SLEEPING && t->wake_time <= timer_ticks) {
            t->state = THREAD_STATE_READY;
            t->wake_time = 0;

            spinlock_acquire(&ready_queue_lock);
            add_to_ready_queue_internal(t);
            spinlock_release(&ready_queue_lock);
        }
    }
    spinlock_release(&thread_table_lock);

    // Decrement time slice
    if (current_thread->time_slice > 0) {
        current_thread->time_slice--;
    }

    // Preempt if time slice expired
    if (current_thread->time_slice == 0) {
        current_thread->time_slice = THREAD_TIME_SLICE;

        // Put current thread back in ready queue and schedule
        spinlock_acquire(&ready_queue_lock);
        if (current_thread->state == THREAD_STATE_RUNNING) {
            current_thread->state = THREAD_STATE_READY;
            add_to_ready_queue_internal(current_thread);
        }
        spinlock_release(&ready_queue_lock);

        thread_schedule();
    }
}

void thread_schedule(void) {
    // Get next thread from ready queue
    spinlock_acquire(&ready_queue_lock);
    thread_t *next = remove_from_ready_queue_internal();
    spinlock_release(&ready_queue_lock);

    // If no thread ready, use idle thread (TID 0)
    if (!next) {
        next = &thread_table[0];  // Idle thread
    }

    // If same thread, just continue
    if (next == current_thread) {
        if (current_thread->state == THREAD_STATE_READY) {
            current_thread->state = THREAD_STATE_RUNNING;
        }
        sti();
        return;
    }

    // Perform context switch
    thread_t *old = current_thread;
    current_thread = next;
    next->state = THREAD_STATE_RUNNING;

    // Set up TLS if needed
    if (next->tls.base != 0) {
        extern void cpu_set_fs_base(uint64_t base);
        cpu_set_fs_base(next->tls.base);
    }

    // If switching to a user thread, update TSS and CR3
    if (next->process && next->process->privilege == PRIV_USER) {
        uint64_t kstack = (uint64_t)next->stack_base + next->stack_size;
        cpu_set_kernel_stack(kstack);

        if (next->process->cr3 != 0) {
            vmm_switch_pml4(next->process->cr3);
        }
    }

    // Context switch
    if (old) {
        extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
        context_switch(&old->rsp, next->rsp);
    } else {
        // First thread - save old state and switch to new context
        extern void context_start(uint64_t *old_rsp, uint64_t new_rsp, uint64_t new_cr3);
        static uint64_t dummy_rsp;
        context_start(&dummy_rsp, next->rsp, next->process ? next->process->cr3 : 0);
    }

    // Returned from context switch - re-enable interrupts
    sti();
}

// ============================================================================
// Debug Functions
// ============================================================================

void thread_print(thread_t *thread) {
    if (!thread) thread = current_thread;
    if (!thread) {
        kprintf("[THREAD] No current thread\n");
        return;
    }

    static const char *state_names[] = {
        "UNUSED", "READY", "RUNNING", "SLEEPING", "BLOCKED", "ZOMBIE", "DEAD"
    };

    kprintf("Thread TID=%u TGID=%u State=%s Prio=%d\n",
            thread->tid, thread->tgid,
            state_names[thread->state], thread->priority);
    kprintf("  Stack: base=0x%lx size=%lu\n",
            (uint64_t)thread->stack_base, thread->stack_size);
    kprintf("  Time: slice=%lu total=%lu\n",
            thread->time_slice, thread->total_time);
    if (thread->tls.base) {
        kprintf("  TLS: base=0x%lx size=%u\n",
                thread->tls.base, thread->tls.size);
    }
}

void thread_print_all(void) {
    kprintf("\n=== Thread List ===\n");

    spinlock_acquire(&thread_table_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state != THREAD_STATE_UNUSED) {
            thread_print(&thread_table[i]);
        }
    }
    spinlock_release(&thread_table_lock);

    kprintf("==================\n\n");
}

void thread_get_stats(uint32_t *total, uint32_t *active, uint32_t *ready) {
    uint32_t t = 0, a = 0, r = 0;

    spinlock_acquire(&thread_table_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i].state != THREAD_STATE_UNUSED) {
            t++;
            if (thread_table[i].state == THREAD_STATE_RUNNING ||
                thread_table[i].state == THREAD_STATE_READY) {
                a++;
            }
            if (thread_table[i].state == THREAD_STATE_READY) {
                r++;
            }
        }
    }
    spinlock_release(&thread_table_lock);

    if (total) *total = t;
    if (active) *active = a;
    if (ready) *ready = r;
}

// ============================================================================
// CPU FS Base Helper (for TLS)
// ============================================================================

// Set FS base register for thread-local storage
// Uses WRFSBASE if available (FSGSBASE feature), otherwise MSR
void cpu_set_fs_base(uint64_t base) {
    // Use MSR 0xC0000100 (IA32_FS_BASE)
    wrmsr(0xC0000100, base);
}

// Weak stub for futex_wake_addr - will be overridden by futex.c
__attribute__((weak))
void futex_wake_addr(uint32_t *addr, int count) {
    (void)addr;
    (void)count;
    // Stub - actual implementation in futex.c
}
