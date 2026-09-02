// process.h - Process and task management for MayteraOS
#ifndef PROCESS_H
#define PROCESS_H

#include "../types.h"
#include "../sync/spinlock.h"   // #SMPGLOBALS: process_t::fd_lock

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
    uint64_t wake_time;             // #483/#499: absolute mono_ms() deadline (ms) to wake a sleeper
    // #513/#499: absolute mono_ms() deadline (ms) at which to raise SIGALRM, 0 = no alarm armed. Set by
    // sys_alarm(); swept by wake_sleeping_procs() in process.c. Distinct from
    // wake_time: an alarm must fire whatever the process is doing (running,
    // ready, or blocked), whereas wake_time only ends a sleep. proc_create()
    // memsets the whole PCB, so a recycled slot starts disarmed.
    uint64_t alarm_time;

    // #wakelag: mono_ms() at which wake_sleeping_procs() moved this sleeper to
    // READY, 0 = not a pending sleep-wake. Read once at the switch commit in
    // sched_schedule() to time the SECOND stage (READY -> RUNNING) separately
    // from the first (deadline -> READY). Two stages, because "the wake is
    // late" and "the wake was on time and the dispatch is late" are different
    // defects with different fixes, and a single end-to-end number cannot tell
    // them apart.
    uint64_t wl_ready_ms;

    // ---- #254/#601 scheduler anti-starvation ----
    //
    // ready_since: the sched tick at which this PCB was last placed on the
    // ready queue. The aging sweep uses it to find processes that have been
    // passed over. prio_boost: ONE-SHOT promotion flag. While set, the
    // process's EFFECTIVE priority is priority + SCHED_BOOST (above
    // PRIO_REALTIME), so it sorts to the front of the ready queue; it is
    // cleared the instant the process is selected to run, so a boost buys
    // exactly one turn and can never become a permanent priority change.
    uint64_t ready_since;
    uint8_t  prio_boost;

    // Privilege level
    uint8_t privilege;              // PRIV_KERNEL (0) or PRIV_USER (3)

    // User identity (multi-user model)
    uint32_t uid;       // Real user ID
    uint32_t gid;       // Real group ID
    uint32_t euid;      // Effective user ID (for setuid binaries)
    uint32_t egid;      // Effective group ID

    // #745 ELEVATION (proc/elevate.h). Three fields Ring 3 has NO syscall to
    // write, which is the whole point of putting them here rather than in a
    // userland state file:
    //   elev_grant_until_ms  absolute deadline of a system-wide install grant,
    //                        0 = none. Written ONLY by SYS_ELEV_RESOLVE, which
    //                        is gated on is_compositor().
    //   elev_grant_prefix    the ONE path prefix that grant covers, a kernel
    //                        constant, never anything the requesting app said.
    //   elev_last_input_ms   when the window manager last delivered a real
    //                        input event (key down, button down/up) to a window
    //                        this process owns. A prompt may only be raised in
    //                        response to recent input; see sys_elev_request().
    // memset(proc, 0, sizeof(process_t)) in proc_create() zeroes all three, so
    // a new or re-exec'd process starts with no grant and no input credit.
    uint64_t elev_grant_until_ms;
    // TWO prefixes, because a system-wide install writes in exactly two places
    // and nowhere else: the application directory and the SYSTEM Start-menu
    // layer. Measured, not designed in the abstract: with one prefix the
    // install landed on disk and reported "(not added to the Start menu)".
    char     elev_grant_prefix[2][40];
    uint64_t elev_last_input_ms;

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
    // a new process; inherited from parent on fork. Relative paths passed to
    // sys_open / sys_stat / etc. are resolved against this by
    // sc_path_from_user() (proc/syscall_path.h) -> path_resolve_cwd_rs().
    //
    // #58: UNTIL 2026-08-20 THE SENTENCE ABOVE WAS FALSE. sys_chdir wrote this
    // field and sys_getcwd read it back, and NOTHING ELSE IN THE KERNEL EVER
    // LOOKED AT IT, so every relative path resolved against the filesystem
    // root and silently opened or created the wrong file. Two independent
    // investigations recorded it (blame.md) before it was fixed. The comment
    // describing the intent outlived the code implementing it by long enough
    // for a reader to trust it: do not reintroduce that gap by describing a
    // consumer that does not exist.
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

    // ---- #SMPGLOBALS: the fd table's lock ---------------------------------
    //
    // Guards EXACTLY this process's fds[] slots and its fd_cloexec bitmap,
    // and nothing else. The canonical shared spinlock (sync/spinlock.h); no
    // private lock type was invented for this.
    //
    // IRQSAVE, because the table is mutated with interrupts already off:
    // proc_exit() calls fd_close_all() under cli().
    //
    // LEAF: nothing is called while it is held that takes another lock or can
    // sleep. file_put() in particular is always called AFTER the release,
    // because a final put runs the description's release op, which for a
    // file-backed description writes to disk.
    //
    // Until 2026-08-30 there was no lock here at all; fs/vfs.c said so and
    // said "when SMP lands, this table needs a per-process spinlock". It was
    // safe only because the Big Kernel Lock serialised every syscall, so
    // narrowing that lock without this one would have uncovered it.
    spinlock_t fd_lock;

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

    // ---- #SMPGLOBALS: this process's signal trampoline ---------------------
    //
    // The Ring-3 address the kernel pushes as the return address a signal
    // handler will `ret` to; libc smuggles it in sigaction's __reserved field.
    //
    // WAS A SINGLE SYSTEM-WIDE GLOBAL (g_sig_trampoline), latched from the
    // FIRST sigaction call made by ANY process on the machine and then used to
    // build the frame for EVERY process. Userland is PIE, so every image is
    // loaded at a different base and one process's trampoline address is
    // meaningless in another. MEASURED on build 2284: two processes with
    // different image bases (0x802ae00000 and 0x8020400000) both took a page
    // fault at the SAME RIP 0x80372da390, which lies inside neither image,
    // because both were handed a trampoline registered by an earlier process.
    // Signal delivery was therefore broken for every process except whichever
    // one happened to install a handler first.
    uint64_t sig_trampoline;

    // ---- #SMPGLOBALS: this task's ring-0 syscall frame -------------------
    //
    // The register+IRET frame that SYSRET will pop, published by
    // syscall_check_return_work() on the way out of every syscall. It REPLACES
    // the single global g_syscall_saved_frame that proc/syscall.asm used to
    // write, which sys_rt_sigreturn() then read.
    //
    // WHY THAT GLOBAL WAS WRONG, and not only under SMP. It held the frame of
    // the LAST syscall to finish ANYWHERE in the system. Every task has its own
    // ring-0 stack, so the address differs per task; a rt_sigreturn issued by
    // task A after task B had completed a syscall read B's frame and rewrote
    // B's saved registers, RIP and user RSP with A's signal context. On one
    // core that needs only a context switch between the two syscalls; on four
    // it needs nothing at all. Per-CPU storage does not fix it either, because
    // both tasks share a core's slot on a single-CPU boot and a task may
    // migrate between the publish and the read.
    //
    // Held per TASK, which is the granularity that is actually correct: clone()
    // gives every thread its own process_t and its own ring-0 stack
    // (proc_clone(), process.c).
    uint64_t syscall_frame;

    // ---- Phase D4: process groups + sessions ----
    //
    // pgrp identifies the process group (jobs within a session); session
    // identifies the session (login + controlling terminal). On fork,
    // both are inherited. setpgid() changes pgrp, setsid() creates a
    // new session. The TTY line discipline targets SIGINT/SIGQUIT at the
    // terminal's foreground pgrp only.
    uint32_t pgrp;
    uint32_t session;

    // ---- The CONTROLLING TERMINAL, so /dev/tty can exist (#lesspipe) ----
    //
    // WHY THIS FIELD IS NEEDED AT ALL. A session had a controlling terminal in
    // NAME only: the comment above says session 'identifies the session (login
    // + controlling terminal)', but nothing anywhere recorded WHICH terminal.
    // The pts binding was a single-shot global (g_tty_bind_pts_idx) consumed by
    // init_proc() purely to pick which device fds 0/1/2 open, and then thrown
    // away. Once a process was running there was no way, even in principle, to
    // ask 'which terminal am I attached to'.
    //
    // That is exactly the question a program whose stdin is a PIPE has to ask.
    // `ls | less` gives the pager a pipe on fd 0, so reading keys from fd 0
    // consumes the piped DATA and hits EOF immediately. The conventional answer
    // is /dev/tty, and /dev/tty cannot be implemented without this field.
    //
    // -1 = no controlling terminal (the console default). 0..MAX_PTY-1 = the
    // pty slave index. Inherited by every child, because a pipeline stage must
    // reach the same terminal its shell was started on.
    int ctty;

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
    // #636: uint64_t mmap_next was DELETED here. The mmap placement cursor
    // is per-ADDRESS-SPACE, not per-thread: two CLONE_VM threads share one
    // mm_struct_t, so a per-thread cursor hands both of them the same
    // address. It lives on mm_struct_t now. This field had zero readers
    // left in kernel/, and leaving a per-thread field named exactly what
    // the bug was named is a trap for the next person.

    // ---- #429: demand-paging memory map ----
    // Per-process mm_struct (mm/demand.h). Lazily created by sys_mmap to hold
    // the VMA list for demand-paged anonymous regions; the #PF handler faults
    // pages in from it. NULL for a process that never demand-mmap'd. Duplicated
    // on fork (mm_dup), freed on exit. Typed void* to avoid dragging demand.h
    // into every process.h consumer.
    void *mm;

    // ---- #25: async HTTP fetch progress ----
    // Set by proc/syscall.c's async_fetch_worker for the duration of one
    // background page/resource fetch: points at the http_progress_t (see
    // net/http_progress.h) embedded in that fetch's job slot. https.c/wget.c
    // call net_progress_current() (which just reads this field off
    // proc_current()) to publish real phase/byte-count updates as they work,
    // with zero new parameters threaded through the connect/receive call
    // chains. NULL for every process that is not running an instrumented
    // fetch (i.e. everything except the browser's async worker thread), so
    // existing callers (HA widget, App Store, pip, sync sys_http_fetch, ...)
    // pay one NULL-check branch and nothing else. Typed void* for the same
    // reason as mm above: net/http_progress.h stays out of every process.h
    // consumer that does not need it.
    void *net_progress;

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
    // ---- #83: WHICH CORE IS THIS TASK ON ----
    // running_cpu is the core CURRENTLY executing this task, or -1 when it is
    // not executing anywhere. It is published by sched_publish_cpu()
    // (proc/process.c) at the instant of the switch, inside sched_schedule()'s
    // #610 cli() region, so the core id it stores was read with IF=0 and cannot
    // have gone stale between the read and the store. That is the invariant
    // #130 was written to establish, and it applies here for the same reason:
    // #130 was a hang whose entire signature was a core-id field reporting
    // confidently and wrongly. The OUTGOING task is cleared to -1 on the same
    // switch, so a task that is merely READY, SLEEPING or BLOCKED never keeps
    // claiming a core.
    //
    // It reads -1 for the brief mid-switch window between the publish and the
    // switch asm having saved the outgoing context. That is deliberate, and it
    // is the conservative direction: this field must never name a core the task
    // is not on. The field that makes the mid-switch window SAFE is
    // sched_on_cpu, not this one. running_cpu is OBSERVABILITY: no correctness
    // decision may be keyed on it, and none is.
    //
    // last_cpu is STICKY: the core that last ran this task, never invalidated.
    // That is what a placement hint actually wants - sched_rq_push() asks
    // "where did this last run" about a task that by definition is not running
    // now - and it is what the pre-#83 merged field was really computing while
    // calling itself running_cpu. Splitting them is most of this ticket: one
    // field cannot answer both questions, and the merged one silently answered
    // the affinity question while every reader that wanted "where is it NOW"
    // got a core id that nothing ever invalidated. The [WAKEPROBE] diagnostic
    // in proc/process.c had already drifted to printing it as `last_cpu=`,
    // which is the tell that the name and the contents had parted company.
    int running_cpu;
    int last_cpu;
    int migratable;

    // ---- #67: SMP context-switch ownership ----
    // Non-zero means "a core is between deciding to switch away from this
    // process and the switch asm having saved its context". While it is set,
    // this process_t's `rsp` still holds the value from the PREVIOUS deschedule,
    // so switching to it would put two cores on one kernel stack at two
    // different RIPs - the #421 "hand off a half-saved context" livelock.
    // SET by sched_schedule() before the switch; CLEARED by
    // context_switch/context_start themselves (proc/context_switch.asm) after
    // the rsp store, the fxsave64 and the stack change; TESTED by
    // sched_rq_pop(), which skips any queued entry that still has it set.
    // Zero on every process created by proc_create()'s memset, and explicitly
    // re-zeroed on the fork/clone children that are built by struct copy.
    volatile int32_t sched_on_cpu;

    // ---- #75: SELECTION PIN ----
    // Non-zero (cpu id + 1) from the instant a core POPS this task off a run
    // queue until it has switched to it. sched_on_cpu covers the mid-switch
    // window; this covers the window immediately BEFORE it, between selection
    // and the switch, which is where the task was being torn down under a core
    // that had already committed to running it (CANDIDATE 2, measured 4 of 4).
    //
    // Set under the run-queue lock by sched_rq_pop_locked(); cleared by the
    // selecting core once the switch is done or abandoned. Read by the EXIT
    // path, which waits for it to clear. A test cannot substitute: the state
    // being valid at the moment of selection is exactly what made this bug
    // invisible - the property decays between the check and the use, so it has
    // to be pinned rather than tested.
    volatile int32_t sched_pinned;

    // ---- #75: enqueue/pop forensics ----
    // reason 3 says the incoming task was not RUNNING at the pre-switch check,
    // and the pin proves it is not dying between pop and switch. Exactly two
    // possibilities remain and nothing measured so far separates them:
    //   (a) it was VALID at the pop and changed under us, by some path that is
    //       not exit; or
    //   (b) IT WAS ALREADY WRONG WHEN WE TOOK IT - something queued a task in a
    //       state that should never have been queueable, which would mean the
    //       run queue itself can hold invalid entries.
    // These three fields answer that in one log line, and record WHO queued it
    // so that if (b) is the answer the next question ("which enqueue site")
    // does not cost another pass.
    // #75: a wake arrived for this task while it was still executing on a core,
    // so the enqueue was REFUSED and is owed. Drained by the next scheduler
    // entry on the core that was running it, once it has actually left.
    uint8_t  rq_wanted;

    // #75 evidence: how this task's LAST enqueue reached the run queue.
    //   1 = through add_to_ready_queue(), the intended funnel
    //   2 = straight into sched_rq_push(), bypassing the funnel (a side door)
    // and whether the funnel ALLOWED it while it was still executing.
    uint8_t  enq_route;
    uint8_t  enq_allowed_hot;   // 1 = funnel let it through with sched_on_cpu set

    uint32_t sched_state_at_enq;   // state on entry to add_to_ready_queue()
    uint32_t sched_state_at_pop;   // state when a core popped it
    void    *sched_enq_ra;         // return address of add_to_ready_queue's caller

    // #75 (enqrace75b): THE RETURN ADDRESS OF THE CALL THAT ACTUALLY ENQUEUED.
    // sched_enq_ra above is stamped at funnel ENTRY, BEFORE the refusal test,
    // and is not cleared when the call is refused, so a refusal overwrites the
    // address of whatever genuinely queued the task. It names the last CALL and
    // four campaigns read it as the last ENQUEUE. This one is written only on
    // the path that reaches a run queue.
    void    *sched_enq_ra_ok;
    // #75 (enqrace75b): sched_running_owner() sampled under g_rq_lock at that
    // enqueue: cpu+1 if some core's current process WAS this task at that
    // instant, else 0. No other field on the enqueue path answers "is this task
    // executing" - sched_on_cpu answers "is it mid-switch-OUT", which is a
    // different question and is 0 for the whole of a task's timeslice.
    int32_t  enq_running_owner;
    uint8_t  enq_probe_tag;        // ENQ_PROBE_SELFTEST only: which construction

    // #smpfix (#75): WHO SAVED THIS TASK'S KERNEL rsp, AND ON WHAT STACK.
    //
    // reason 1 ("rsp outside the incoming task's kernel stack") is detected at
    // the READ side, by whichever core later selects the task. That is one
    // switch and usually one CORE too late to say who wrote the bad value, and
    // three campaigns have now read that dump without being able to name the
    // writer. These fields are stamped at sched_publish_cpu(), the last point
    // on every switch path before the switch asm executes the store of the
    // outgoing stack pointer, so
    // the value that is ABOUT to be saved (the live rsp, a few hundred bytes
    // off) is recorded together with the core and the scheduler's caller.
    uint64_t rsp_save_live;   // RSP inside sched_schedule() at the save point
    uint64_t rsp_save_ra;     // return address of sched_schedule()'s caller
    int32_t  rsp_save_cpu;    // core that performed the save, -1 = never saved
    uint32_t rsp_save_n;      // how many times this task's rsp has been saved

    // ---- #67 pass 2: run-queue membership ----
    // 1 while this PCB is linked into some core's run queue. Set and cleared
    // ONLY under g_rq_lock, and checked by sched_rq_push() before inserting.
    //
    // It exists because wake_sleeping_procs() walks the whole process table
    // WITHOUT the run-queue lock, and once two cores run the scheduler both can
    // see the same expired sleeper in the same instant and both call
    // add_to_ready_queue() on it. Inserting one PCB into two lists through a
    // single intrusive `next` pointer corrupts both queues. A state test cannot
    // substitute: add_to_ready_queue() SETS the state as a side effect, so by
    // the time the second core looks the state already says READY.
    uint8_t rq_queued;

    // 1 for a per-core idle process. The BSP's idle is pid 0 and always has
    // been; the APs' idle processes have ordinary pids, so "pid == 0" stopped
    // being the same question as "is this the idle process" the moment a second
    // core got one. Every scheduler test that means the latter now asks this.
    uint8_t is_idle;

    // ---- #430: threads (clone/futex/pthread) ----
    // A "thread" here is a process_t that shares its parent's address space
    // (same cr3). shares_vm marks such a task so cleanup_proc_slot does NOT
    // destroy the shared address space or free the shared user stack when the
    // thread exits (that memory belongs to the thread group leader).
    uint8_t   shares_vm;            // 1 = shares another proc's cr3 (a thread)

    // ---- #745 (task 37): DETACHED KERNEL WORKER ----
    // Set by proc_create_ex() on every process it creates. init_proc() takes
    // ppid from current_proc, so a kernel worker started from inside a syscall
    // (the "httpfetch"/"httppost" async transfer workers are the ones that
    // matter) became the CHILD of whichever Ring 3 process happened to enter
    // that syscall. That process never learns the worker's pid, never wait()s
    // for it, and so its zombie was immortal: 41 fetches after boot the
    // 64-entry table was full and every further fetch failed to start. See
    // rustkern/procreap.rs for the measured failure. A detached process is
    // NOT a wait()-able child (same treatment as a #430 thread), and its
    // zombie is reclaimable by proc_reap_orphans() / reap_orphan_zombies().
    uint8_t   detached;
    uint32_t  tgid;                 // thread-group id (leader pid); 0 = self
    // CLONE_CHILD_CLEARTID: on thread exit, zero *clear_child_tid and
    // futex-wake it so pthread_join() unblocks. NULL for ordinary processes.
    uint32_t *clear_child_tid;

    // #446: per-process FXSAVE image for context_switch/context_start.
    // #588 carved this 512-byte area off the kernel stack, aligned the base
    // down with "and rsp,-16" and stashed a pointer to the saved RFLAGS at
    // [base+512] so the restore side could find the GPR frame across the
    // resulting variable gap. Whenever the outgoing and incoming procs'
    // switch frames landed on the SAME kernel stack, the outgoing fxsave64
    // wrote straight through the incoming proc's stashed pointer and
    // "mov rsp,[rsp+512]" loaded XMM bytes as RSP -> double fault at
    // context_switch (~1 boot in 9). Keeping the image here removes the
    // carve, the alignment slack and the stash. MUST stay 16-byte aligned:
    // fxsave64/fxrstor64 #GP on a misaligned operand.
    //
    // #745 local 107: grown from 512/16 to FPU_AREA_SIZE/FPU_AREA_ALIGN so the
    // SAME field also holds an xsave64 image (512 legacy + 64 header + the
    // components in XCR0). fxsave64 needs 16-byte alignment, xsave64 needs 64.
    // The literals are spelled out because process.h deliberately does not
    // include cpu/sse.h; proc/process.c _Static_asserts they equal
    // FPU_AREA_SIZE / FPU_AREA_ALIGN where both headers are visible, so the
    // two can never drift apart silently.
    uint8_t fpu_area[1024] __attribute__((aligned(64)));

    // #COMPRESPAWN: WHERE THIS PROCESS'S ELF IMAGE ACTUALLY LANDED.
    //
    // Every userland binary is now PIE and elf_load_user() randomises its base
    // within a 1 GB window at 2 MB granularity (exec/elf.c, #640 stage 3). That
    // means the RIP in a crash report is MEANINGLESS on its own: to turn
    // "RIP=0x8001ebe772" into a function you must know which of the 512 ASLR
    // slots this particular run got, and nothing recorded it.
    //
    // MEASURED COST, 2026-08-25: diagnosing the owner's compositor page fault
    // required brute-forcing the slot by hand (the only slot for which
    // RIP-base lands inside .text was 15). That is not a thing anyone can do
    // on a machine they cannot attach a debugger to, and it is not a thing
    // anyone should have to do at all when the loader knew the answer.
    //
    // Recorded at spawn, printed by the fault handler. Zero for kernel threads.
    uint64_t image_base;
    uint64_t image_end;
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
    // #145: PROC_INFO_F_* . Occupies the four bytes of tail padding that were
    // already there (running_cpu ends at offset 60 and the uint64_t forces an
    // 8-byte size), so sizeof and every existing field offset are UNCHANGED and
    // the SYS_PROC_LIST ABI is byte-identical. The _Static_assert below is what
    // keeps that true if anyone reorders the struct.
    uint32_t flags;
} proc_info_t;
_Static_assert(sizeof(proc_info_t) == 64, "proc_info_t sizeof lock (userland twin + Rust ProcInfo)");

// #145: this row is a per-core IDLE process, i.e. CPU capacity that nothing
// asked for. It is NOT a consumer and must never be ranked as one. Exported as
// a kernel-authoritative BIT rather than left to userland string-matching
// "idle"/"idleN", because a user process may legitimately be called that and
// this project has been bitten by name-matching before (COMPOSIT vs
// COMPOSITOR). See proc_snapshot().
#define PROC_INFO_F_IDLE   0x00000001u

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

// #446: seed an FXSAVE area with a sane default FPU env (FCW=0x037F,
// MXCSR=0x1F80). fpu_area_init() takes a raw FPU_AREA_ALIGN-aligned,
// FPU_AREA_SIZE-byte buffer
// so threads and per-CPU scratch areas share the one implementation.
void fpu_area_init(void *area);
void proc_init_fpu_area(struct process *p);

// #110: capture the CALLER'S LIVE FPU/SSE(/AVX) registers into `area`
// (FPU_AREA_ALIGN-aligned, FPU_AREA_SIZE bytes), using the same
// xsave64/fxsave64 selection context_switch uses. fpu_save_live() is the raw
// asm (proc/context_switch.asm); fpu_capture_live() is the zero-then-save
// wrapper every caller should use. This is what fork()/clone() give the child,
// instead of the architectural default fpu_area_init() produces: the child of
// a fork inherits the parent's floating-point environment.
extern void fpu_save_live(void *area);   // proc/context_switch.asm
void fpu_capture_live(void *area);
void proc_capture_fpu_from_current(struct process *p);

// #446: stamp a proc's kernel stack with its own pid so the scheduler can
// detect two live procs sharing one kernel stack.
void proc_stack_tag(struct process *p);

// #446: FXSAVE scratch used when there is no outgoing proc to save into.
extern uint8_t g_dummy_fpu_area[1024];

// #421 phase 5 (AssaultCube port): SMP process-teardown-vs-snapshot race fix.
//
// BACKGROUND: proc_snapshot() (and proc_mem_info(), procmem.c) read a live
// process's p->mm and then walk mm->vma_list via proc_mem_fill_in() +
// proc_mem_account() with NO locking. cleanup_proc_slot() (process.c), which
// runs when a zombie process's slot is reclaimed (proc_reap /
// reap_orphan_zombies / proc_wait), calls mm_destroy(proc->mm) to free that
// exact vma_list and then sets proc->mm = NULL. On SMP these can run on
// different cores at the same instant: proc_snapshot() captures a valid
// mm/vma_head pointer, cleanup_proc_slot() frees the vma nodes out from under
// it on another core, and the walk dereferences freed memory. This is exactly
// what killed a whole VM during AssaultCube phase 4 bring-up: a recoverable
// user-process GPF was correctly caught and the process killed, but the
// heartbeat's concurrent proc_snapshot() then panicked for real inside
// proc_mem_account_rs (kernel/proc/procmem.c), turning one crashing app into
// a full system halt.
//
// FIX: a dedicated spinlock (g_proc_mm_lock, process.c, static) serializes
// the two sides of this race:
//   - cleanup_proc_slot() holds it across "read proc->mm, mm_destroy it if
//     owned, then null it" (process.c);
//   - proc_snapshot() and proc_mem_info() hold it across
//     "proc_mem_fill_in() + proc_mem_account()" for a single process, i.e.
//     across the entire capture-and-walk of that process's mm (process.c,
//     procmem.c).
// Neither side can observe (or free) a vma_list the other side is using.
// Both critical sections are short and non-sleeping (bounded VMA walk,
// bounded VMA free, no allocation that can block), so holding a plain
// spinlock across them is safe under the "never hold a lock across
// something that might sleep" rule. This is intentionally a NEW, narrowly
// scoped lock. It was chosen over the `kernel_lock` that used to sit in
// cpu/smp.h, which this comment already recorded as dead (zero real callers
// besides its own accessors); that lock has since been DELETED outright
// (2026-08-23) for exactly that reason, and it was never the Big Kernel Lock
// its name and its header comment claimed it was. The real BKL is
// bkl_acquire()/bkl_release() in cpu/smp.c, and this narrow lock is still the
// right choice over it: entangling this fix with a much coarser-grained
// whole-kernel mechanism is what was being avoided.
//
// Exposed as two plain functions rather than `extern spinlock_t
// g_proc_mm_lock`, historically because process.h (included very widely:
// fs/, gui/, drivers/, io/, ipc/, net/...) could not pull in
// sync/spinlock.h: io/io_ring.h #defined its own zero-arg
// `compiler_barrier()` macro, which collided with spinlock.h's real
// `compiler_barrier(void)` inline of the same name.
//
// THAT OBSTACLE IS GONE (#SMPGLOBALS, 2026-08-30). The private macro was a
// forked one-line copy of the shared primitive; it has been deleted and
// io_ring.h now includes sync/spinlock.h like everyone else, so process.h
// includes it too and process_t carries a real spinlock_t (see fd_lock
// above). These two functions are kept as-is because the mm lock is a single
// global, not a per-PCB field, and nothing is improved by exposing it.
void proc_mm_lock(void);
void proc_mm_unlock(void);

// #421 phase 5 FOLLOW-UP: the g_proc_mm_lock fix above was verified
// correct-as-far-as-it-goes (it does serialize a single process_t's mm
// against its own teardown) but was NOT sufficient on its own: it still
// panicked at the exact same proc_mem_account_rs address on a real,
// reproduced AssaultCube crash. Root cause of the remaining gap: a CLONE_VM
// thread (shares_vm=1, process.c proc_clone()) shares its group leader's mm
// pointer, but cleanup_proc_slot() decided "am I the one who frees this mm"
// from a single process_t's OWN shares_vm flag, not from whether anyone else
// was still using it. When the LEADER crashed and was reaped while a
// just-cloned sibling thread was still alive and running, the leader's
// cleanup (shares_vm==0, so "yes, free it") freed the shared mm out from
// under the still-live sibling, whose own p->mm field kept pointing at now-
// freed memory; the next heartbeat's proc_snapshot() walked THAT sibling and
// panicked. The lock above did not help because each side's critical
// section was individually correct, just about the wrong condition: "am I
// shares_vm" says nothing about whether anyone else is still referencing the
// same mm.
//
// REAL FIX: `mm_users` (mm/demand.h), a plain refcount on the mm itself, not
// on any one process_t. mm_create() seeds it at 1; proc_clone() calls
// mm_get() (under this same lock) when a new thread starts sharing an
// existing mm; cleanup_proc_slot() calls mm_put() (also under this lock)
// instead of mm_destroy() directly, which only actually frees once the count
// reaches 0, i.e. once EVERY process_t that ever referenced this mm -
// leader or thread, in whatever order they exit - has released it. A still-
// running sibling can never be left holding a dangling pointer no matter
// which member of the group dies first. See demand.h for the field and
// mm_get()/mm_put() themselves.
//
// Verification: reproduced the panic on the pre-refcount tree (crash
// AssaultCube -> its own recoverable GPF -> a SECOND, fatal kernel GPF at
// the same proc_mem_account_rs RIP, same throwaway VM <vmid>, build 910). See
// PORT-STATUS.md / CHANGELOG.md for the measured before/after boot logs
// proving whether mm_users actually closes the gap.

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

// #COMPRESPAWN: sweep every ZOMBIE nobody will ever wait for, returning their
// slots, kernel stacks and address spaces NOW rather than at the next
// proc_create(). Call this BEFORE allocating for a relaunch, never with
// g_proc_table_lock held. See the definition in process.c.
int proc_reap_orphans(void);

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

// #230: wake every parent parked in proc_wait(). Called at every transition to
// PROC_STATE_ZOMBIE and, redundantly, from sched_tick(). Safe from IRQ context
// and with interrupts already off.
void proc_child_exit_notify(void);

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
// #58: cwd of the calling process, or NULL if there is no current process.
const char *proc_cwd(void);

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
// #171: sticky "the scheduler has started". See the long comment at
// sched_set_preemption() for why this is NOT the same question.
bool sched_is_live(void);

/**
 * Handle scheduler timer tick
 * Called from timer interrupt handler
 */
void sched_tick(void);
// #169: the PER-CORE scheduler tick, taken on an Application Processor from its
// own LAPIC timer (vector 0x42, armed by tick_ap_arm() in cpu/isr.c).
//
// NOT a variant of sched_tick() and not interchangeable with it. sched_tick()
// owns GLOBAL state - timer_ticks' consumer sched_ticks, g_sched_tick_samples,
// the g_cpu_pct aggregate, and the cron/futex/child-exit/mm-watchdog hooks -
// all of which must be advanced by exactly ONE core or they run N times too
// fast. sched_tick_ap() touches NONE of them: it does per-core preemption and
// per-core accounting only. Never call it on the BSP.
void sched_tick_ap(void);

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

// ---------------------------------------------------------------------------
// #692: THE IDENTITY A NEW RING-3 PROCESS RUNS AS.
//
// There is no default and no silent inheritance. proc_create_user() used to
// give the child whatever uid/gid init_proc() had copied from proc_current(),
// and at three of the eight user-spawn sites the "parent" is a KERNEL THREAD
// whose uid is 0 by definition (the cron worker, and the two in-kernel
// launchers in apps/). While the desktop autologs in as root that grants
// nothing; the moment the session is not root, a USER-CREATED CRON JOB RUNS AS
// ROOT. The session flip does not merely fail to prevent that, it creates it,
// by being the thing that makes root mean something.
//
// So this is not three patched call sites. proc_create_user() is DELETED, and
// its replacement takes a proc_ident_t that cannot be omitted:
//
//   * a forgotten spawn site is a COMPILE ERROR, not a uid-0 child;
//   * PROC_AS_INVALID is 0, so a zero-initialised or memset identity is
//     REFUSED by the resolver rather than meaning root;
//   * PROC_AS_CALLER is refused unless the caller really is a Ring-3 process,
//     so "inherit" can never again silently mean "inherit from a kernel
//     thread".
//
// The policy itself lives in rustkern/spawnid.rs (new kernel code, so Rust per
// the 2026-07-16 rule), including the single gid-follows-uid rule that fixes
// the incoherent uid 0 / gid 1000 pair services.c used to produce.
// ---------------------------------------------------------------------------
#define PROC_AS_INVALID  0u   // never valid; the resolver refuses it
#define PROC_AS_CALLER   1u   // run as the CALLING Ring-3 process (exec)
#define PROC_AS_SESSION  2u   // run as the logged-in desktop session user
#define PROC_AS_UID      3u   // run as an explicit uid; gid from /CONFIG/PASSWD

typedef struct {
    uint32_t kind;   // PROC_AS_*
    uint32_t uid;    // PROC_AS_UID only; ignored otherwise
} proc_ident_t;

// FFI layout lock: rustkern/spawnid.rs takes these two fields as scalars.
_Static_assert(sizeof(proc_ident_t) == 8, "proc_ident_t must stay 2x u32 for the Rust FFI");

// Constructors. Call sites read as an English sentence, which is the point:
// the identity is visible at the spawn, not buried in a post-hoc fixup.
static inline proc_ident_t proc_as_caller(void)  { proc_ident_t i = { PROC_AS_CALLER,  0 };   return i; }
static inline proc_ident_t proc_as_session(void) { proc_ident_t i = { PROC_AS_SESSION, 0 };   return i; }
static inline proc_ident_t proc_as_uid(uint32_t uid) { proc_ident_t i = { PROC_AS_UID, uid }; return i; }

/**
 * Create a user-mode process from an ELF binary, running as `ident`.
 * @param name      Process name
 * @param elf_data  Pointer to ELF binary data
 * @param elf_size  Size of ELF binary
 * @param argv      Command line arguments (NULL-terminated array)
 * @param envp      Environment variables (NULL-terminated array)
 * @param ident     MANDATORY identity; an unresolvable one fails the spawn
 * @return          Process ID, or -1 on failure
 */
void proc_set_next_migratable(int v);  // #279: route next launched user proc to an AP
int proc_create_user_as(const char *name, void *elf_data, uint64_t elf_size,
                        char **argv, char **envp, proc_ident_t ident);

/**
 * Phase J: create a user process with /dev/pts/N pre-wired as stdio.
 *
 * Spawns the ELF with fds 0/1/2 bound to the pty slave at index `pts_idx`
 * instead of /dev/console. The caller must have opened /dev/ptmx first
 * (so the slave slot exists). Returns PID on success, -1 on failure.
 * `ident` is mandatory, exactly as for proc_create_user_as().
 */
int proc_create_user_tty_as(const char *name, void *elf_data, uint64_t elf_size,
                            int pts_idx, proc_ident_t ident);

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
extern void context_switch(uint64_t *old_rsp, uint64_t new_rsp,
                           void *old_fpu, void *new_fpu,
                           volatile int32_t *old_release,
                           volatile int32_t *new_unpin);

/**
 * Start running a new process for the first time
 * Called after creating a process to jump into it
 */
extern void context_start(uint64_t *old_rsp, uint64_t new_rsp, uint64_t new_cr3,
                          void *old_fpu, volatile int32_t *old_release,
                          volatile int32_t *new_unpin);

// #67 SMP scheduler diagnostic + policy self-test (proc/process.c).
void sched_smp_selftest(void);

// #67 pass 2: turn THIS application processor into a real scheduler consumer.
// Creates the core's own idle process, publishes it as the core's current
// process, advertises the core in g_rq_consumers, and never returns: the core
// runs the idle loop and is preempted into work by its own timer tick, exactly
// like the BSP. Called once per AP from the SMP work loop.
void sched_ap_enter(uint32_t cpu);

// #75: remove a process from every run queue. Call before it stops being a
// thing that may be run. See the comment on the definition.
void sched_rq_remove(void *vp);

// #75: report whether this task is already SELECTED by some core. Call from the
// exit path before the task stops being runnable.
void sched_note_exit(void *vp);

// #75: mark THIS task as about to stop running. Arms scheduler ownership and
// writes the new state as ONE operation, in that order, so no other core can
// observe a blocked/sleeping task that is still executing. Use these instead of
// assigning to ->state directly in any path where a task blocks itself.
void sched_self_block(void *vp, uint32_t new_state);
void sched_self_running(void *vp);

// #67 pass 2: cross-core preemption request. Marks `cpu` as needing to
// reschedule and pokes it with an IPI so a halted core wakes; the target
// consumes the flag in its own sched_tick().
void sched_request_resched(uint32_t cpu);

/* (rakbd) Live per-core busy-tick counter (BSP sched_tick + AP
   sched_tick_ap), in BSP-tick units. See the definition in process.c
   for why cpu/smp.c's own g_core_busy_ticks[] is not sufficient. */
uint64_t sched_core_busy_ticks(uint32_t cpu);

#endif // PROCESS_H
