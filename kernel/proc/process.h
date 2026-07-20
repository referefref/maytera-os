// process.h - Process and task management for MayteraOS
#ifndef PROCESS_H
#define PROCESS_H

#include "../types.h"

// Forward declarations so we can embed pointers into other subsystems in
// the PCB without dragging their headers into every caller of process.h.
struct wait_queue_entry;
struct file;

// Maximum number of processes
#define MAX_PROCESSES       64

// Maximum length (bytes) of a process's current working directory path,
// including terminating NUL. Matches MAXPATHLEN in our FAT/VFS layer.
#define PROC_CWD_MAX        256

// Maximum open file descriptors per process (Phase A1). Raised from 16
// (old kernel-wide fd_table) to 64 (per-process). Each slot holds a
// struct file* that is refcounted and shared across fork/dup.
#define MAX_FDS             64

// Process stack size (16KB per process)
#define PROCESS_STACK_SIZE  (16 * 1024)

// User mode stack size (2MB)
#define USER_STACK_SIZE     (2 * 1024 * 1024)

// Kernel stack size for user processes (8KB)
#define KERNEL_STACK_SIZE   (64 * 1024)

// Privilege levels
#define PRIV_KERNEL         0   // Ring 0 - kernel mode
#define PRIV_USER           3   // Ring 3 - user mode

// Process states
typedef enum {
    PROC_STATE_UNUSED = 0,  // Slot not in use
    PROC_STATE_READY,       // Ready to run
    PROC_STATE_RUNNING,     // Currently running
    PROC_STATE_SLEEPING,    // Waiting on timer
    PROC_STATE_BLOCKED,     // Waiting on I/O or event
    PROC_STATE_ZOMBIE       // Terminated, waiting for cleanup
} process_state_t;

// Process priority levels
typedef enum {
    PRIO_IDLE = 0,      // Idle process (lowest)
    PRIO_LOW = 1,
    PRIO_NORMAL = 2,
    PRIO_HIGH = 3,
    PRIO_REALTIME = 4   // Highest priority
} process_priority_t;

// Saved CPU context for context switching
// Must match the order pushed/popped in context_switch.asm
typedef struct {
    // General purpose registers
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;

    // Instruction pointer and flags
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) cpu_context_t;

// Process Control Block (PCB)
typedef struct process {
    // Process identification
    uint32_t pid;                   // Process ID
    uint32_t ppid;                  // Parent process ID
    int exit_code;                  // Exit code (for parent to retrieve)
    char name[32];                  // Process name

    // State and scheduling
    process_state_t state;          // Current state
    process_priority_t priority;    // Priority level
    uint64_t time_slice;            // Remaining time slice (in ticks)
    uint64_t total_time;            // Total CPU time used
    uint64_t wake_time;             // Tick count to wake up (for sleeping)
    // #513: absolute tick at which to raise SIGALRM, 0 = no alarm armed. Set by
    // sys_alarm(); swept by wake_sleeping_procs() in process.c. Distinct from
    // wake_time: an alarm must fire whatever the process is doing (running,
    // ready, or blocked), whereas wake_time only ends a sleep. proc_create()
    // memsets the whole PCB, so a recycled slot starts disarmed.
    uint64_t alarm_time;

    // Privilege level
    uint8_t privilege;              // PRIV_KERNEL (0) or PRIV_USER (3)

    // User identity (multi-user model)
    uint32_t uid;       // Real user ID
    uint32_t gid;       // Real group ID
    uint32_t euid;      // Effective user ID (for setuid binaries)
    uint32_t egid;      // Effective group ID

    // Memory - kernel mode
    void *stack_base;               // Kernel stack base address
    uint64_t stack_size;            // Kernel stack size
    uint64_t rsp;                   // Saved stack pointer (kernel RSP)

    // Memory - user mode (only for user processes)
    uint64_t cr3;                   // Page table root (PML4 physical address)
    uint64_t user_stack_base;       // User stack virtual address (base)
    uint64_t user_stack_size;       // User stack size
    uint64_t user_rsp;              // User stack pointer (saved)
    uint64_t user_rip;              // User instruction pointer (entry)

    // Context
    cpu_context_t *context;         // Saved CPU context (on stack)

    // Function pointer for kernel processes
    void (*entry_point)(void *);    // Entry point function (kernel mode)
    void *entry_arg;                // Argument to entry point

    // Linked list for scheduler queue
    struct process *next;           // Next in ready queue

    // ---- POSIX-ish additions (Phase 0) ----

    // Current working directory, absolute path. Initialized to "/" for
    // a new process; inherited from parent on fork. Relative paths passed
    // to sys_open / sys_stat / etc. are resolved against this.
    char cwd[PROC_CWD_MAX];

    // If this process is sleeping interruptibly on a wait_queue_head_t
    // (see sync/waitq.h), wait_entry points at the entry on that queue.
    // Signal delivery uses this to kick the process awake with -EINTR.
    // NULL for running/ready/zombie processes.
    struct wait_queue_entry *wait_entry;

    // ---- Phase A1: per-process file descriptor table ----

    // Open file descriptors. Each slot is either NULL or points to a
    // refcounted struct file (see fs/vfs.h). Slot 0..2 are reserved for
    // stdin/stdout/stderr (Phase A2 pre-opens them on /dev/console).
    struct file *fds[MAX_FDS];

    // FD_CLOEXEC bitmap (Phase A3): bit i is set if fd i should be closed
    // by exec(). Currently unused by any consumer; Phase A3 adds
    // fcntl(F_SETFD, FD_CLOEXEC) and makes execve honor it.
    uint64_t fd_cloexec;

    // ---- Phase D1: signal state ----
    //
    // sig_pending: bitmask of signals queued for delivery to this process.
    //              Bit (signo - 1) is set when a signal is queued.
    // sig_mask:    bitmask of signals currently blocked (sigprocmask).
    //              A pending signal is not delivered while it is masked.
    // sig_handlers[i] is the userland handler for signal (i+1). NULL means
    //              default action (usually terminate). SIG_IGN is encoded
    //              as (void*)1.
    // return_work: bitmap of work items to perform at syscall return. Bit
    //              0 = signals pending, bit 1 = exec pending (Phase G).
    //              Checked by the assembly syscall_return_path; when set,
    //              return_work_handler() is called before SYSRET/IRETQ.
    uint64_t sig_pending;
    uint64_t sig_mask;
    void    *sig_handlers[64];
    uint64_t sig_flags[64];         // SA_* flags per signal (D2)
    uint64_t sig_handler_mask[64];  // mask applied during handler run (D2)
    uint32_t return_work;

    // ---- Phase D4: process groups + sessions ----
    //
    // pgrp identifies the process group (jobs within a session); session
    // identifies the session (login + controlling terminal). On fork,
    // both are inherited. setpgid() changes pgrp, setsid() creates a
    // new session. The TTY line discipline targets SIGINT/SIGQUIT at the
    // terminal's foreground pgrp only.
    uint32_t pgrp;
    uint32_t session;

    // ---- Phase G: real execve ----
    //
    // When sys_execve validates and loads a new image, it stashes the
    // freshly-prepared state here and arms RETURN_WORK_EXECPENDING. At the
    // next syscall return the return_work_handler:
    //   1. Writes exec_new_cr3 into CR3.
    //   2. Destroys exec_old_cr3 (the PML4 + user pages of the exiting image).
    //   3. Rewrites the saved user RIP/RSP to exec_new_rip/exec_new_rsp.
    //   4. Resets signal handlers to SIG_DFL (preserving SIG_IGN).
    //   5. Closes all FD_CLOEXEC file descriptors.
    uint64_t exec_new_cr3;
    uint64_t exec_new_rip;
    uint64_t exec_new_rsp;
    uint64_t exec_old_cr3;
    uint64_t exec_new_user_stack_base;
    uint64_t exec_new_user_stack_size;

    // ---- Per-process heap / mmap state ----
    uint64_t brk;                   // Current program break (heap top)
    uint64_t mmap_next;             // Next anonymous mmap address

    // ---- #429: demand-paging memory map ----
    // Per-process mm_struct (mm/demand.h). Lazily created by sys_mmap to hold
    // the VMA list for demand-paged anonymous regions; the #PF handler faults
    // pages in from it. NULL for a process that never demand-mmap'd. Duplicated
    // on fork (mm_dup), freed on exit. Typed void* to avoid dragging demand.h
    // into every process.h consumer.
    void *mm;

    // ---- #95: Background services subsystem ----
    //
    // is_service is 1 only for processes launched by the service manager
    // (proc/services.c); init_proc's memset leaves it 0 for every ordinary
    // process, so the permission gates keyed on it are a strict no-op for
    // normal apps and never change their behavior.
    //
    // svc_perms is a bitmask of SVC_PERM_* capabilities (see proc/services.h)
    // granted to a service via its service account. When is_service is set,
    // selected syscalls (fs-write, spawn, ...) are denied unless the matching
    // permission bit is present. svc_perms is ignored when is_service is 0.
    uint8_t  is_service;
    uint32_t svc_perms;
    int running_cpu;
    int migratable;

    // ---- #430: threads (clone/futex/pthread) ----
    // A "thread" here is a process_t that shares its parent's address space
    // (same cr3). shares_vm marks such a task so cleanup_proc_slot does NOT
    // destroy the shared address space or free the shared user stack when the
    // thread exits (that memory belongs to the thread group leader).
    uint8_t   shares_vm;            // 1 = shares another proc's cr3 (a thread)
    uint32_t  tgid;                 // thread-group id (leader pid); 0 = self
    // CLONE_CHILD_CLEARTID: on thread exit, zero *clear_child_tid and
    // futex-wake it so pthread_join() unblocks. NULL for ordinary processes.
    uint32_t *clear_child_tid;
} process_t;

// Snapshot record for SYS_PROC_LIST (Task Manager). Layout MUST match the
// userland proc_info_t in libc/syscall.h.
typedef struct {
    uint32_t pid;
    uint32_t ppid;
    char     name[32];
    uint32_t state;       // process_state_t value
    uint32_t mem_kb;      // committed user memory (KB)
    uint64_t cpu_ticks;   // total CPU ticks consumed (total_time)
    int32_t  running_cpu; // #279: AP id this proc is pinned to, or -1 (BSP/normal)
} proc_info_t;

// Base of the user heap (the value p->brk is initialized to on the first
// sys_brk call). #487: centralized here because per-process memory accounting
// (proc/procmem.c) needs the heap base to size the heap, and mm->brk_start is
// NOT usable for it: mm_create() memsets the mm to zero and nothing ever
// assigns brk_start, so it reads 0 for every process. sys_brk keeps p->brk
// authoritative instead. Keep this the single definition.
//
// Spelled as a literal because process.h deliberately does not pull in mm/vmm.h
// (include-graph hygiene); proc/procmem.c _Static_asserts it equals
// USER_SPACE_START + 0x100000 where both headers are visible, so the two can
// never drift apart silently.
#define PROC_DEFAULT_BRK_START   0x0000000000500000ULL

// Fill `out` (up to `max` entries) with a snapshot of all live processes.
// Returns the number of entries written. Used by SYS_PROC_LIST.
int proc_snapshot(proc_info_t *out, int max);

// #404/#349 Task Manager: number of threads in `pid`'s thread group (>=1).
uint32_t proc_thread_count(uint32_t pid);

// #487: pid of the running process, or 0 if there is none. A narrow accessor so
// callers that only need the pid (net/tcp.c stamping socket ownership) do not
// have to pull the whole PCB definition into their layer.
uint32_t proc_current_pid(void);


// Return-work bits used by syscall_return_path and return_work_handler().
#define RETURN_WORK_SIGPENDING   (1u << 0)
#define RETURN_WORK_EXECPENDING  (1u << 1)

// ============================================================================
// Process Management API
// ============================================================================

/**
 * Initialize the process subsystem
 * Creates the idle process and initializes the scheduler
 */
void proc_init(void);

/**
 * Create a new process
 * @param name      Process name
 * @param entry     Entry point function
 * @param arg       Argument passed to entry function
 * @param priority  Process priority
 * @return          Process ID, or -1 on failure
 */
int proc_create(const char *name, void (*entry)(void *), void *arg,
                process_priority_t priority);
// #264: like proc_create but with an explicit kernel-stack size (clamped to at
// least the default). Net/TLS workers need a large stack.
int proc_create_ex(const char *name, void (*entry)(void *), void *arg,
                   process_priority_t priority, uint32_t stack_size);
// #264: reap a specific zombie child by pid (frees its slot). Returns 0 / -1.
int proc_reap(uint32_t pid);

/**
 * Terminate the current process
 * @param exit_code Exit code (stored for parent to retrieve)
 */
void proc_exit(int exit_code);

/**
 * Wait for child process to exit
 * @param pid    Process ID to wait for (-1 for any child)
 * @param status Pointer to store exit status (can be NULL)
 * @return       PID of exited child, or -1 on error
 */
int proc_wait(int pid, int *status);

/**
 * Yield CPU to another process
 * Voluntarily gives up the remaining time slice
 */
void proc_yield(void);

/**
 * Sleep for a specified number of milliseconds
 * @param ms    Milliseconds to sleep
 */
void proc_sleep(uint32_t ms);

/**
 * Get current process
 * @return      Pointer to current process PCB
 */
process_t *proc_current(void);

/**
 * Get process by PID
 * @param pid   Process ID
 * @return      Pointer to process PCB, or NULL if not found
 */
process_t *proc_get(uint32_t pid);

/**
 * Get count of active processes
 * @return      Number of processes (excluding unused slots)
 */
uint32_t proc_count(void);

// ============================================================================
// Scheduler API
// ============================================================================

/**
 * Schedule next process to run
 * Called by timer interrupt handler
 */
void sched_schedule(void);

/**
 * Enable/disable preemption
 * @param enable    true to enable, false to disable
 * @return          Previous preemption state
 */
bool sched_set_preemption(bool enable);

/**
 * Check if preemption is enabled
 * @return          true if enabled
 */
bool sched_preemption_enabled(void);

/**
 * Handle scheduler timer tick
 * Called from timer interrupt handler
 */
void sched_tick(void);

/**
 * Print process list
 */
void proc_print_list(void);

/**
 * Transition a BLOCKED or SLEEPING process to READY and add it to the ready
 * queue. Safe to call with interrupts disabled; idempotent for processes
 * that are already runnable or terminated.
 *
 * Used by the wait-queue implementation (sync/waitq.c) and by future
 * signal-delivery code to unblock interruptibly-sleeping targets.
 */
void proc_wake(struct process *p);

// ============================================================================
// User-mode process support
// ============================================================================

/**
 * Create a user-mode process from an ELF binary
 * @param name      Process name
 * @param elf_data  Pointer to ELF binary data
 * @param elf_size  Size of ELF binary
 * @param argv      Command line arguments (NULL-terminated array)
 * @param envp      Environment variables (NULL-terminated array)
 * @return          Process ID, or -1 on failure
 */
void proc_set_next_migratable(int v);  // #279: route next launched user proc to an AP
int proc_create_user(const char *name, void *elf_data, uint64_t elf_size,
                     char **argv, char **envp);

/**
 * Phase J: create a user process with /dev/pts/N pre-wired as stdio.
 *
 * Spawns the ELF with fds 0/1/2 bound to the pty slave at index `pts_idx`
 * instead of /dev/console. The caller must have opened /dev/ptmx first
 * (so the slave slot exists). Returns PID on success, -1 on failure.
 */
int proc_create_user_tty(const char *name, void *elf_data, uint64_t elf_size,
                         int pts_idx);

/**
 * Fork the current process
 * @return          Child PID in parent, 0 in child, -1 on failure
 */
int proc_fork(void);

/**
 * #430: clone(2) - create a thread that shares the caller's address space.
 * Modeled on proc_fork() but shares cr3 (CLONE_VM), runs on a caller-supplied
 * user stack, and returns 0 in the child / new-thread tid in the parent.
 * flags use the CLONE_* constants in thread.h.
 */
int proc_clone(uint32_t flags, void *user_stack, uint32_t *parent_tid,
               uint32_t *child_tid, void *tls);

/** #430: return the current task's thread id (== its pid in this model). */
uint32_t proc_gettid(void);

/** #430: set the CLONE_CHILD_CLEARTID address for the current task. */
uint32_t proc_set_tid_address(uint32_t *tidptr);

/**
 * Execute a new program in the current process
 * @param path      Path to ELF binary
 * @param argv      Command line arguments
 * @param envp      Environment variables
 * @return          -1 on failure (never returns on success)
 */
int proc_exec(const char *path, char **argv, char **envp);

/**
 * Phase G: real execve. Load the ELF at `path` into a fresh address space
 * for the current process, stash the new cr3/rip/rsp into the PCB, and arm
 * RETURN_WORK_EXECPENDING. The actual cr3 swap and IRET-frame rewrite
 * happen at syscall return via return_work_handler().
 * @return 0 if armed; -1 if load failed (current image untouched).
 */
int proc_execve_arm(const char *path, char **argv, char **envp);

/**
 * Phase G: finalize a pending exec at syscall return time. Called by
 * return_work_handler. Swaps CR3 to the new address space, destroys the
 * old one, rewrites the saved IRET frame, resets signal handlers and
 * CLOEXEC fds. Never returns failure; the image is already armed.
 */
void proc_execve_finalize(void *user_frame);

/**
 * Jump to user mode (called after setting up user process)
 * @param entry_rip User-mode entry point
 * @param user_rsp  User-mode stack pointer
 * Never returns.
 */
void proc_enter_usermode(uint64_t entry_rip, uint64_t user_rsp);

/**
 * Check if current process is running in user mode
 * @return          true if user mode, false if kernel mode
 */
bool proc_is_usermode(void);

/**
 * Get the current process's address space (CR3)
 * @return          PML4 physical address
 */
uint64_t proc_get_cr3(void);

// ============================================================================
// Low-level context switching (implemented in assembly)
// ============================================================================

/**
 * Switch from one process to another
 * Saves current context to old_rsp, loads new context from new_rsp
 */
extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp);

/**
 * Start running a new process for the first time
 * Called after creating a process to jump into it
 */
extern void context_start(uint64_t *old_rsp, uint64_t new_rsp, uint64_t new_cr3);

#endif // PROCESS_H
