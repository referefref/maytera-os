// thread.h - Thread management for MayteraOS
// Part of Task #25 (Threading with clone() syscall)
//
// Threads share the same address space (process) but have independent:
// - Stack
// - Register state (including instruction pointer)
// - Thread-local storage (TLS)
// - Thread ID (TID)
//
// Threading model follows Linux clone() semantics with CLONE_* flags.

#ifndef THREAD_H
#define THREAD_H

#include "../types.h"
#include "process.h"
#include "../sync/spinlock.h"

// ============================================================================
// Clone Flags (compatible with Linux)
// ============================================================================

#define CLONE_VM        0x00000100  // Share virtual memory (address space)
#define CLONE_FS        0x00000200  // Share filesystem info (cwd, root)
#define CLONE_FILES     0x00000400  // Share file descriptor table
#define CLONE_SIGHAND   0x00000800  // Share signal handlers
#define CLONE_THREAD    0x00010000  // Same thread group (share PID)
#define CLONE_SETTLS    0x00080000  // Set thread-local storage
#define CLONE_PARENT_SETTID 0x00100000  // Store TID in parent
#define CLONE_CHILD_CLEARTID 0x00200000 // Clear TID when thread exits
#define CLONE_CHILD_SETTID  0x01000000  // Store TID in child

// Combined flag for typical pthread_create semantics
#define CLONE_PTHREAD   (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | \
                         CLONE_THREAD | CLONE_SETTLS | CLONE_PARENT_SETTID | \
                         CLONE_CHILD_CLEARTID)

// ============================================================================
// Thread Constants
// ============================================================================

#define MAX_THREADS_PER_PROCESS 64  // Maximum threads per process
#define THREAD_STACK_SIZE       (64 * 1024)  // 64KB per thread stack
#define DEFAULT_TLS_SIZE        4096  // Default TLS area size

// Thread states
typedef enum {
    THREAD_STATE_UNUSED = 0,    // Slot not in use
    THREAD_STATE_READY,         // Ready to run
    THREAD_STATE_RUNNING,       // Currently running
    THREAD_STATE_SLEEPING,      // Waiting on timer
    THREAD_STATE_BLOCKED,       // Blocked on futex or I/O
    THREAD_STATE_ZOMBIE,        // Exited, waiting for join
    THREAD_STATE_DEAD           // Fully cleaned up
} thread_state_t;

// ============================================================================
// Thread-Local Storage (TLS)
// ============================================================================

// TLS descriptor for GDT/FS base setup
typedef struct {
    uint64_t base;          // TLS base address
    uint32_t size;          // TLS area size
    uint32_t flags;         // TLS flags
} tls_desc_t;

// ============================================================================
// Thread Control Block (TCB)
// ============================================================================

typedef struct thread {
    // Thread identification
    uint32_t tid;               // Thread ID (unique system-wide)
    uint32_t tgid;              // Thread group ID (= main thread's TID = PID)

    // Parent process reference
    struct process *process;    // Owning process

    // State and scheduling
    thread_state_t state;       // Current state
    process_priority_t priority; // Thread priority (inherited from process)
    uint64_t time_slice;        // Remaining time slice
    uint64_t total_time;        // Total CPU time used
    uint64_t wake_time;         // Tick count to wake (for sleeping)

    // CPU context (saved on context switch)
    uint64_t rsp;               // Saved stack pointer
    uint64_t rip;               // Saved instruction pointer (for debug)

    // Stack
    void *stack_base;           // Thread stack base (kernel-allocated)
    uint64_t stack_size;        // Thread stack size
    uint64_t user_stack_base;   // User-mode stack base (if applicable)
    uint64_t user_stack_size;   // User-mode stack size
    uint64_t user_rsp;          // User-mode stack pointer

    // Thread-local storage
    tls_desc_t tls;             // TLS descriptor
    void *tls_base;             // TLS memory base (kernel-allocated)

    // Futex support (clear_child_tid)
    uint32_t *clear_child_tid;  // Address to clear on exit (CLONE_CHILD_CLEARTID)
    uint32_t *set_child_tid;    // Address to set TID (CLONE_CHILD_SETTID)

    // Exit status
    int exit_code;              // Exit code (for joining)
    void *exit_value;           // Return value from thread function

    // Join synchronization
    struct thread *joining_thread; // Thread waiting to join this one
    bool detached;              // If true, no join needed

    // Entry point (for new threads)
    void (*entry_fn)(void *);   // Thread function
    void *entry_arg;            // Argument to thread function

    // Linked list for scheduler
    struct thread *next;        // Next in ready/wait queue

    // Thread group list
    struct thread *group_next;  // Next thread in same process
    struct thread *group_prev;  // Previous thread in same process
} thread_t;

// ============================================================================
// Thread Group (extends process_t)
// ============================================================================

// Maximum threads in a single thread group
#define MAX_THREAD_GROUP_SIZE 64

typedef struct thread_group {
    spinlock_t lock;            // Protect thread list
    uint32_t tgid;              // Thread group ID (= main thread TID)
    thread_t *leader;           // Thread group leader (main thread)
    thread_t *threads;          // Linked list of all threads
    uint32_t thread_count;      // Number of active threads
    uint32_t exit_code;         // Group exit code
    bool exiting;               // Group is exiting (no new threads allowed)
} thread_group_t;

// ============================================================================
// Thread Management API
// ============================================================================

/**
 * Initialize the thread subsystem
 */
void thread_init(void);

/**
 * Create a new thread via clone()
 *
 * @param flags     Clone flags (CLONE_VM, CLONE_THREAD, etc.)
 * @param stack     User-provided stack pointer (or NULL for kernel allocation)
 * @param parent_tid Pointer to store TID in parent (CLONE_PARENT_SETTID)
 * @param child_tid  Pointer to store TID in child / clear on exit
 * @param tls       TLS descriptor (CLONE_SETTLS)
 * @return          Thread ID on success, -1 on failure
 */
int thread_create(uint32_t flags, void *stack, uint32_t *parent_tid,
                  uint32_t *child_tid, void *tls);

/**
 * Create a kernel thread
 *
 * @param name      Thread name
 * @param entry     Entry function
 * @param arg       Argument to entry function
 * @param priority  Thread priority
 * @return          Thread ID on success, -1 on failure
 */
int thread_create_kernel(const char *name, void (*entry)(void *), void *arg,
                         process_priority_t priority);

/**
 * Exit the current thread
 *
 * @param exit_code Exit code
 */
void thread_exit(int exit_code) __attribute__((noreturn));

/**
 * Wait for a thread to exit
 *
 * @param tid       Thread ID to wait for
 * @param exit_code Pointer to store exit code (can be NULL)
 * @return          0 on success, -1 on failure
 */
int thread_join(uint32_t tid, int *exit_code);

/**
 * Detach a thread (no join needed)
 *
 * @param tid       Thread ID to detach
 * @return          0 on success, -1 on failure
 */
int thread_detach(uint32_t tid);

/**
 * Get current thread
 *
 * @return          Pointer to current thread, or NULL
 */
thread_t *thread_current(void);

/**
 * Get thread by TID
 *
 * @param tid       Thread ID
 * @return          Pointer to thread, or NULL if not found
 */
thread_t *thread_get(uint32_t tid);

/**
 * Get current thread ID
 *
 * @return          Current thread ID, or 0 if not in a thread
 */
uint32_t thread_gettid(void);

/**
 * Set the clear_child_tid address for the current thread
 *
 * @param tidptr    Address to clear on thread exit
 * @return          Current thread ID
 */
uint32_t thread_set_tid_address(uint32_t *tidptr);

/**
 * Yield the current thread's timeslice
 */
void thread_yield(void);

/**
 * Put the current thread to sleep
 *
 * @param ms        Milliseconds to sleep
 */
void thread_sleep(uint32_t ms);

// ============================================================================
// Thread-Local Storage API
// ============================================================================

/**
 * Set up TLS for the current thread
 *
 * @param base      TLS base address
 * @param size      TLS size
 * @return          0 on success, -1 on failure
 */
int thread_set_tls(uint64_t base, uint32_t size);

/**
 * Get the TLS base address for the current thread
 *
 * @return          TLS base address, or 0 if none
 */
uint64_t thread_get_tls(void);

// ============================================================================
// Thread Scheduler Integration
// ============================================================================

/**
 * Add a thread to the ready queue
 *
 * @param thread    Thread to add
 */
void thread_enqueue(thread_t *thread);

/**
 * Remove and return the next thread to run
 *
 * @return          Next thread, or NULL if queue empty
 */
thread_t *thread_dequeue(void);

/**
 * Handle scheduler tick for threads
 * Called from timer interrupt
 */
void thread_sched_tick(void);

/**
 * Schedule the next thread to run
 * May context switch to a different thread
 */
void thread_schedule(void);

// ============================================================================
// Thread Context Switching (implemented in assembly)
// ============================================================================

/**
 * Switch from one thread to another
 * Saves current state, loads new state
 *
 * @param old_rsp   Pointer to store current RSP
 * @param new_rsp   New RSP to load
 */
extern void thread_switch(uint64_t *old_rsp, uint64_t new_rsp);

/**
 * Start a new thread (first time)
 * Sets up stack and jumps to entry point
 *
 * @param rsp       Stack pointer
 * @param tls_base  TLS base address (for FS segment)
 */
extern void thread_start(uint64_t rsp, uint64_t tls_base) __attribute__((noreturn));

// ============================================================================
// Debug Functions
// ============================================================================

/**
 * Print thread information
 *
 * @param thread    Thread to print (or NULL for current)
 */
void thread_print(thread_t *thread);

/**
 * Print all threads in the system
 */
void thread_print_all(void);

/**
 * Get thread statistics
 *
 * @param total     Total threads created
 * @param active    Currently active threads
 * @param ready     Threads in ready queue
 */
void thread_get_stats(uint32_t *total, uint32_t *active, uint32_t *ready);

#endif // THREAD_H
