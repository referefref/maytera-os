// process.c - Process and task management implementation
#include "process.h"
#include "../fs/bootlog.h"   // #134: persistent process-exit record (owning header)
#include "schedrace.h"   // #75: SMP context-corruption reproducer
#include "syscall.h"
#include "signal.h"      // #161: SIGCHLD / SIG_IGN for the no-cldwait reap rule
#include "users.h"       // #692: user_entry_t for the gid-follows-uid shim
#include "procmem.h"      // #487: per-process memory accounting (pulls mm/demand.h)
#include "../security/validate.h"   // #503: deferred-write validation (clear_child_tid)
#include "../security/uaccess_smap.h"  // #19/#645: AC brackets for the user-stack writes
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../cpu/gdt.h"
#include "../cpu/smp.h"
#include "../cpu/scprof.h"   // #121: spawn-path phase attribution
#include "../serial.h"
#include "../string.h"
#include "../cpu/isr.h"
#include "../exec/elf.h"
#include "../fs/fat.h"
#include "../gui/syslog.h"
#include "../fs/vfs.h"
#include "../cpu/mono.h"   // #421 phase 7: mono_us() for the mm-lock watchdog
#include "../cpu/sse.h"    // #588: fxsave_area_t for the FPU-frame init
#include "../sync/waitq.h"  // #230: the child-exit wait queue

// Process table
static process_t proc_table[MAX_PROCESSES];

// #230: the child-exit wait queue. Declared up here because proc_init() (which
// initialises it) runs long before proc_wait() (which is defined further down
// and documents the whole design).
static wait_queue_head_t g_child_exit_wq;
static int g_child_exit_wq_ready = 0;

// #421 phase 5: guards p->mm across cleanup_proc_slot() (mm_destroy + null)
// vs proc_snapshot()/proc_mem_info() (fill_in + walk). See process.h for the
// full race writeup. Kept `static` + accessed by other files ONLY through
// proc_mm_lock()/proc_mm_unlock() below, so process.h does not need to
// expose spinlock_t (see process.h comment: that broke the build via an
// io_ring.h macro collision).
static spinlock_t g_proc_mm_lock = SPINLOCK_INIT;

// #421 phase 7: the g_proc_mm_lock hold MUST be non-preemptible. It is a
// SECOND lock, separate from the BKL (cpu/smp.c). The scheduler releases only
// the BKL across a context switch (bkl_release_all()), never this lock. So if a
// thread holding g_proc_mm_lock is PREEMPTED (timer -> sched_tick ->
// sched_schedule), this lock travels with the switched-out thread while the BKL
// is dropped, letting ANOTHER cpu take the BKL and then spin on g_proc_mm_lock:
// a classic AB-BA deadlock between the BKL and this lock that wedges the WHOLE
// kernel with no panic (a lock cycle, not a fault). That is exactly the silent
// both-cpus-halted hang AssaultCube hit a few seconds past map load, where the
// heavy pthread churn + Task Manager/heartbeat proc_snapshot() polling makes the
// "preempted mid-critical-section" window get hit. Fix: hold this lock with
// interrupts disabled on the local cpu (irqsave), so the holder can never be
// preempted, so the lock is never held across a context switch. The holder
// identity + hold-age are recorded so any future stall names who holds it
// (proc_mm_lock_watchdog(), below) instead of presenting as a silent halt.
//
// Single-holder invariant: this lock is never taken recursively (no
// g_proc_mm_lock critical section calls another), and while it is held IF=0 on
// the holding cpu prevents same-cpu re-entry, so a SINGLE global saved-flags
// slot is sufficient and correct.
static uint64_t   g_proc_mm_lock_flags = 0;         // saved RFLAGS for the holder
volatile int      g_proc_mm_lock_owner_cpu = -1;    // -1 = free, else holding cpu
volatile uint64_t g_proc_mm_lock_site = 0;          // caller return address
volatile uint64_t g_proc_mm_lock_since_us = 0;      // mono_us() at acquire

// #421 phase 5/7: public wrappers so procmem.c/procinfo.c (and any future
// caller) can take part in the same critical section without process.h pulling
// in sync/spinlock.h. Now irqsave (see the invariant note above).
void proc_mm_lock(void) {
    uint64_t fl = spinlock_acquire_irqsave(&g_proc_mm_lock);
    g_proc_mm_lock_flags = fl;
    g_proc_mm_lock_owner_cpu = (int)smp_this_cpu();
    g_proc_mm_lock_site = (uint64_t)__builtin_return_address(0);
    g_proc_mm_lock_since_us = mono_us();
}
void proc_mm_unlock(void) {
    uint64_t fl = g_proc_mm_lock_flags;
    g_proc_mm_lock_owner_cpu = -1;
    g_proc_mm_lock_site = 0;
    g_proc_mm_lock_since_us = 0;
    spinlock_release_irqrestore(&g_proc_mm_lock, fl);
}

// #421 phase 7 watchdog: every g_proc_mm_lock hold site is microsecond-scale.
// If the lock stays held for seconds, a holder wedged (or a deadlock this fix
// missed). Name it ONCE on serial so a future silent halt becomes a named
// holder + site instead of a byte-identical screendump. Called from sched_tick()
// (already in the timer ISR under the BKL); reads only, never takes the lock.
void proc_mm_lock_watchdog(void) {
    extern process_t *current_process;   // defined below; forward-declared here
    int cpu = g_proc_mm_lock_owner_cpu;
    if (cpu < 0) return;                            // free
    uint64_t since = g_proc_mm_lock_since_us;
    if (since == 0) return;
    uint64_t now = mono_us();
    if (now <= since) return;
    uint64_t held = now - since;
    static uint64_t warned_site = 0;
    if (held > 2000000ULL && g_proc_mm_lock_site != warned_site) {
        warned_site = g_proc_mm_lock_site;
        kprintf("[MMWD] g_proc_mm_lock held %lu us by cpu%d site=0x%lx cur=%s "
                "-- possible mm-lock wedge (see #421 phase 7)\n",
                (unsigned long)held, cpu, (unsigned long)g_proc_mm_lock_site,
                current_process ? current_process->name : "?");
    }
}

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
extern char kernel_stack_bottom[];   // #446: entry.asm boot stack (64 KB)

// #167 / defect (c): DO NOT FREE A KERNEL STACK A CORE IS STILL STANDING ON.
//
// proc_exit() sets state = ZOMBIE and calls sched_schedule(). Its own comment
// is explicit that the task is STILL RUNNING ON ITS KERNEL STACK at that point
// ("DO NOT free stack here - we are still running on it!"), and it stays there
// right up to the `mov [rdi], rsp` inside the switch asm. sched_on_cpu is the
// flag that says exactly this, and the asm clears it once the save is done.
//
// On another core, proc_create() calls reap_orphan_zombies(), which sees
// state == ZOMBIE and calls this function, which kfree()s stack_base. Neither
// the reaper nor this function consulted sched_on_cpu, so the stack a live core
// is executing on was handed back to the heap and re-issued.
//
// MEASURED, this pass, and it is what the fault looks like from the inside. The
// #167 exit-churn reproducer (proc/wakeloss.c, ~9,400 sleep-then-exit threads
// per boot, recycling 64 PCB slots continuously) produced 9 to 28 panics per
// boot under SCHEDRACE=1, all of them the wild RIP #75 first reported:
//
//   [KERNEL PANIC] Invalid Opcode at RIP=0x45  RSP=0x11310638
//   [rsp+0x00] = 0x44            [rsp+0x28] = 0x5ac460 *   (proc_current)
//   [rsp+0x30] = "e 0\n[PRO"     [rsp+0x58] = "g, code "
//   [rsp+0x60] = "0\n[PROC]"     [rsp+0x68] = " 'wlshor"
//   [rsp+0x70] = "t' (PID "      [rsp+0x78] = "138) exi"
//
// The stack being executed from CONTAINS THE TEXT of another thread's exit log
// line and a "HEAP" tag. It is not a corrupted stack, it is somebody else's
// memory. The same logs carry "[HEAP] ERROR: kfree() called on invalid pointer",
// which is the same block coming back a second time.
//
// This also explains why proc/schedrace.c's pre-switch validator never fired on
// any of them: that validator checks the INCOMING context at a switch, and this
// is the OUTGOING context's stack being freed underneath it. No switch-time
// check can see it.
//
// RETURNS 0 and cleans NOTHING if the task is still on a core, so the caller
// must not mark the slot UNUSED either; the next reap sweep will take it, by
// which time sched_on_cpu is clear. Losing one reap cycle costs a slot for a
// few milliseconds. Freeing a live stack costs the machine.
volatile uint64_t g_reap_deferred = 0;   // reaps refused because a core was on it
int g_exit_stack_guard = 1;              // cleared by /NOSTACKGUARD.TXT (control arm)

static int cleanup_proc_slot(process_t *proc) {
    if (g_exit_stack_guard && proc && proc->sched_on_cpu != 0) {
        g_reap_deferred++;
        return 0;
    }
    // Free kernel stack if allocated.
    // #446: pid 0's stack_base points at the STATIC boot stack in entry.asm,
    // which must never be handed to kfree().
    if (proc->stack_base && proc->stack_base != (void *)kernel_stack_bottom) {
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
    // #429: free the per-process demand-paging mm.
    //
    // #421 phase 5: held under g_proc_mm_lock for the ENTIRE release-and-null
    // sequence, not just the null assignment. proc_snapshot()/proc_mem_info()
    // hold the same lock across their own capture-and-walk of this exact mm
    // (see process.h), so a concurrent snapshot on another core either runs
    // to completion entirely before this releases anything, or is blocked
    // until this finishes and then correctly observes proc->mm == NULL. There
    // is no window where a snapshot can hold a pointer into a vma_list this
    // is simultaneously freeing.
    //
    // #421 phase 5 FOLLOW-UP: this used to be "if (proc->mm && !proc->shares_vm)
    // mm_destroy(...)", i.e. it decided who frees the mm from a single
    // process_t's OWN shares_vm flag. That is wrong for a thread GROUP: it
    // let the group leader free the mm unconditionally on its own
    // exit/cleanup even while a shares_vm sibling thread was still alive and
    // holding the exact same pointer in its own p->mm - a real, reproduced
    // use-after-free (see demand.h's mm_users comment for the full incident:
    // a crashed AssaultCube leader was reaped while its just-cloned worker
    // thread was still running; the next heartbeat's proc_snapshot() walked
    // the sibling's now-dangling p->mm and panicked the kernel inside
    // proc_mem_account_rs). mm_put() replaces the shares_vm check with a real
    // refcount (bumped by proc_clone() for every thread that starts sharing
    // an mm): it only actually frees once every process_t referencing this
    // mm - leader or thread, in whatever order they happen to exit - has
    // released it, so a still-running sibling is never left holding a
    // dangling pointer no matter which one dies first.
    // #421 phase 7: decrement the refcount and DETACH proc->mm under the lock
    // (short, non-preemptible), but perform the ACTUAL teardown (vma_free_all +
    // kfree, potentially many VMAs for a heavy app like AssaultCube) OUTSIDE the
    // lock with interrupts restored. Once proc->mm is NULL'd under the lock, no
    // new proc_snapshot()/proc_mem_info()/sys_proc_detail can obtain this
    // pointer, and any walk already in flight held the same lock so it fully
    // completed before we acquired it. This keeps the interrupts-disabled window
    // tiny (a refcount dec + a pointer store) instead of spanning an unbounded
    // free, while still closing the use-after-free the refcount was added for.
    {
        // #421 phase 7: mm_put_detach() (demand.c) does the refcount decrement
        // and RETURNS the mm to destroy (or NULL) WITHOUT freeing it, so the
        // actual teardown runs OUTSIDE the lock with interrupts restored. void*
        // here because process.c deliberately does not include mm/demand.h,
        // matching the existing extern-void* mm_get()/mm_dup() pattern below.
        extern void *mm_put_detach(void *mm);
        extern void mm_destroy(void *mm);
        void *dead = NULL;
        proc_mm_lock();
        if (proc->mm) dead = mm_put_detach(proc->mm);
        proc->mm = NULL;
        proc_mm_unlock();
        if (dead) mm_destroy(dead);
    }
    proc->cr3 = 0;
    proc->shares_vm = 0;
    proc->clear_child_tid = NULL;
    return 1;
}

// #264: reap a specific zombie child, freeing its slot + resources. Safe on a
// non-zombie / invalid pid (no-op). rc_cmd_shell calls this so each session's
// MSH does not leak as a permanent zombie.
int proc_reap(uint32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &proc_table[i];
        if (p->state == PROC_STATE_ZOMBIE && p->pid == pid) {
            if (!cleanup_proc_slot(p)) continue;   // #167(c): still on a core
            p->state = PROC_STATE_UNUSED;
            return 0;
        }
    }
    return -1;
}

// #745 (task 37): FFI mirror of rustkern/procreap.rs's ProcReapEnt. The reclaim
// POLICY lives in Rust (per the 2026-07-16 rule; it is new logic, so there is
// no C twin to strangle - the same call the fetchown seam made); this file
// keeps the table walk and the teardown, which touch kernel stacks, page
// tables and the mm refcount.
typedef struct {
    uint32_t pid;
    uint32_t ppid;
    uint32_t state;
    uint32_t flags;
} proc_reap_ent_t;

#define PROC_REAP_F_SHARES_VM  (1u << 0)
#define PROC_REAP_F_DETACHED   (1u << 1)
// #161: the zombie's PARENT has SIGCHLD set to SIG_IGN, i.e. it has declared
// (the POSIX way) that it will never wait() for its children. See the flag's
// use in reap_orphan_zombies() below and the policy in rustkern/procreap.rs.
#define PROC_REAP_F_NOCLDWAIT  (1u << 2)

_Static_assert(sizeof(proc_reap_ent_t) == 16,
               "proc_reap_ent_t must match ProcReapEnt in rustkern/procreap.rs");
_Static_assert(MAX_PROCESSES <= 64,
               "proc_reap_scan_rs returns a u64 slot bitmask; MAX_PROCESSES cannot exceed 64");
_Static_assert(PROC_STATE_UNUSED == 0 && PROC_STATE_ZOMBIE == 5,
               "rustkern/procreap.rs mirrors these two PROC_STATE_* values");

extern uint64_t proc_reap_scan_rs(const proc_reap_ent_t *ents, uint32_t n, int skip);
extern uint32_t proc_reap_selftest_rs(void);

// #264/#745: reaper for ZOMBIE procs nobody will ever wait for. The decision of
// WHICH ones those are is rustkern/procreap.rs (orphans, #430 threads, and
// - the task 37 fix - detached kernel workers). Returns the count reclaimed.
// Never touches idle (slot 0) or the current proc.
static int reap_orphan_zombies(void) {
    // #745 (local 75) CLASS FIX: the task to SKIP is the one running on
    // THIS cpu. Through the BSP-published global, a reap running on an AP
    // protected the BSP's task and left its own caller eligible.
    process_t *me = proc_current();

    proc_reap_ent_t ents[MAX_PROCESSES];
    int skip = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &proc_table[i];
        ents[i].pid   = p->pid;
        ents[i].ppid  = p->ppid;
        ents[i].state = (uint32_t)p->state;
        ents[i].flags = (p->shares_vm ? PROC_REAP_F_SHARES_VM : 0u) |
                        (p->detached  ? PROC_REAP_F_DETACHED  : 0u);
        // #161: #745 task 37 fixed this leak for KERNEL WORKERS by marking them
        // detached, and its brief said to check every other spawner. The Ring 3
        // spawn path has the identical defect and was not checked: an app
        // launched from the dock or the start menu is a child of the
        // COMPOSITOR, which never calls wait() and never exits, so the policy's
        // "no live parent" rule can never fire and EVERY app the user opens and
        // closes leaks a permanent zombie slot. The table is 64 entries, so
        // that is the same slow denial of service the fetch workers caused,
        // reached by opening and closing apps.
        //
        // The mechanism, rather than a per-caller guess: POSIX already has the
        // declaration this needs. A parent that sets SIGCHLD to SIG_IGN is
        // saying its children must not become zombies, and wait() is then
        // required to fail rather than hand back a status. That is exactly the
        // compositor's situation, it is explicit (no heuristic about whether a
        // parent "looks like" it will wait), and any future long-lived spawner
        // opts in with one line. msh, which DOES wait, sets nothing and keeps
        // POSIX zombie semantics untouched.
        //
        // Evaluated only for zombies: this is the one branch that costs a
        // parent lookup, and a live process's slot is not a candidate anyway.
        if (p->state == PROC_STATE_ZOMBIE) {
            process_t *par = proc_get(p->ppid);
            if (par && par->sig_handlers[SIGCHLD - 1] == SIG_IGN)
                ents[i].flags |= PROC_REAP_F_NOCLDWAIT;
        }
        if (p == me) skip = i;
    }
    uint64_t mask = proc_reap_scan_rs(ents, (uint32_t)MAX_PROCESSES, skip);
    int n = 0;
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (!(mask & (1ull << i))) continue;
        if (!cleanup_proc_slot(&proc_table[i])) continue;   // #167(c)
        proc_table[i].state = PROC_STATE_UNUSED;
        n++;
    }
    return n;
}

// #COMPRESPAWN: THE SAME SWEEP, CALLABLE FROM OUTSIDE THIS FILE.
//
// reap_orphan_zombies() is static and its only caller is alloc_proc_slot(),
// which means a dead process's resources come back at the moment the NEXT
// process is created - and, critically, AFTER anything that caller allocated
// on the way in. gui/desktop.c's launch_userspace_app() reads the ~1 MB ELF
// with kmalloc FIRST and calls proc_create_user_as() SECOND, so when the
// compositor dies and is relaunched, the 1 MB read is asked for while the dead
// compositor still holds its process slot, its 64 KB kernel stack and its
// entire ~29 MB address space. The reap it needed had not run yet.
//
// This is the ordinary "free before you allocate" rule, and the only reason it
// was violated is that the reap was invisible from where the allocation
// happens. Exported rather than duplicated, per the shared-primitive rule.
//
// Safe from any ordinary kernel thread: it takes no lock itself and
// cleanup_proc_slot() kfree()s, so it must NOT be called with
// g_proc_table_lock held (alloc_proc_slot() calls it before taking the lock,
// for exactly this reason).
int proc_reap_orphans(void) {
    return reap_orphan_zombies();
}

// #COMPRESPAWN: FAULT-SAFE "which image is running here?" snapshot.
//
// cpu/idt.c deliberately does not include proc/process.h, and the exception
// handler must not dereference anything it has not proved. This reads four
// scalars from the current PCB and nothing else: no lock, no allocation, no
// call that can fault. Returns 1 for a Ring-3 process with a recorded image,
// 0 otherwise (kernel thread, no current process, or a pre-#COMPRESPAWN
// spawn path that never recorded a base).
int proc_current_image(uint32_t *pid, uint64_t *base, uint64_t *end,
                       uint64_t *cr3) {
    process_t *p = proc_current();
    if (!p) return 0;
    if (pid)  *pid  = p->pid;
    if (base) *base = p->image_base;
    if (end)  *end  = p->image_end;
    if (cr3)  *cr3  = p->cr3;
    return (p->privilege == PRIV_USER && p->image_base != 0) ? 1 : 0;
}

/**
 * Find a free process slot
 */
// #446: FXSAVE scratch for switches with no outgoing proc to save.
uint8_t g_dummy_fpu_area[1024] __attribute__((aligned(64)));

// #745 local 107: process.h/thread.h spell the size and alignment as literals
// (they do not include cpu/sse.h). Lock them to the one definition here, where
// both headers are visible.
_Static_assert(sizeof(((process_t *)0)->fpu_area) == FPU_AREA_SIZE,
               "#745 local 107: process_t::fpu_area must be FPU_AREA_SIZE");
_Static_assert(sizeof(g_dummy_fpu_area) == FPU_AREA_SIZE,
               "#745 local 107: g_dummy_fpu_area must be FPU_AREA_SIZE");
_Static_assert(__alignof__(((process_t *)0)->fpu_area) == FPU_AREA_ALIGN,
               "#745 local 107: process_t::fpu_area must be FPU_AREA_ALIGN aligned");
_Static_assert(FPU_AREA_SIZE >= 576,
               "#745 local 107: an xsave64 area is at least 512 legacy + 64 header");

// #446: seed an FXSAVE image with the architectural default FPU environment.
// An all-zero image is NOT a valid thing to fxrstor64: FCW=0 leaves every x87
// exception unmasked at 24-bit precision, and MXCSR=0 leaves every SSE
// exception unmasked, so the first FP/SSE op after such a restore traps
// (#MF / #XM, or #UD on a CPU whose CR4.OSXMMEXCPT is clear). EVERY path that
// produces a runnable rsp must seed its area through here; the seeding is
// therefore ALSO done centrally in init_proc() and alloc_proc_slot() so a
// future frame builder cannot silently miss it.
void fpu_area_init(void *area) {
    memset(area, 0, FPU_AREA_SIZE);
    ((fxsave_area_t *)area)->fcw   = 0x037F;
    ((fxsave_area_t *)area)->mxcsr = 0x1F80;
    // #745 local 107: the 64-byte XSAVE header at offset 512 stays all-zero.
    // XSTATE_BV = 0 makes xrstor64 load each component's architectural INIT
    // state (x87 FCW=0x037F, XMM=0, YMM_Hi128=0) instead of reading it out of
    // the image, which is exactly what a fresh task wants. XCOMP_BV = 0 is
    // also REQUIRED: bit 63 set would mean the compacted format, which
    // xrstor64 (as opposed to xrstors) will not accept.
    //
    // MXCSR is the exception. xrstor64 loads MXCSR from the legacy area
    // whenever SSE or AVX is in the restore mask, REGARDLESS of XSTATE_BV, so
    // the 0x1F80 seeded above is load-bearing for the xsave path too - a
    // zeroed image would restore MXCSR=0 and unmask every SSE exception,
    // which is #446 lesson 4 in a new costume.
}

void proc_init_fpu_area(process_t *p) { fpu_area_init(p->fpu_area); }

// #110: fork()/clone() must hand the child the PARENT'S floating-point state,
// not a freshly initialized one. POSIX requires the child of fork() to inherit
// the parent's floating-point environment, and a cloned thread to inherit the
// creating thread's; the previous code called proc_init_fpu_area(child) on
// both paths, so every child started with FCW=0x037F / MXCSR=0x1F80 and zeroed
// XMM/YMM/x87 registers no matter what the parent was doing.
//
// WHERE THE PARENT'S STATE ACTUALLY IS. It is NOT in me->fpu_area. That field
// is written only by context_switch/context_start, i.e. the last time the
// parent was descheduled, which can be an arbitrary amount of user execution
// ago. Copying it would swap one wrong answer for another (a stale one).
// The parent's real state is live in the physical registers: the kernel is
// built -mno-sse -mno-sse2 and never touches them, and any interrupt that
// descheduled the parent on the way in restored them on the way back. So the
// correct source is a LIVE save taken here, in the parent's own syscall
// context, straight into the child's area.
//
// This covers MXCSR and the x87 control word, not just the data registers: a
// copy that got XMM0-15 but reset the rounding mode or the exception masks
// would still be wrong, and would still pass a naive register-only test.
//
// AVX: fpu_save_live() branches on g_fpu_use_xsave exactly as the context
// switch does (#107), so on a machine where AVX is enabled the child inherits
// YMM0-15 in full, and on one where it is not, CR4.OSXSAVE is clear and there
// is no YMM state in existence to inherit. fork therefore cannot be narrower
// than the switch, which is the drift this comment exists to prevent.
void fpu_capture_live(void *area) {
    // xsave64 ORs into the existing XSTATE_BV and leaves bits outside the RFBM
    // alone, so a stale header would survive into the image and make the
    // matching xrstor64 #GP. Zero first. fork/clone are not hot paths; this is
    // one FPU_AREA_SIZE memset per process creation.
    memset(area, 0, FPU_AREA_SIZE);
    fpu_save_live(area);
}

void proc_capture_fpu_from_current(process_t *p) { fpu_capture_live(p->fpu_area); }

// #446 diagnostics. These are cheap (a few loads on the switch path) and only
// print when something is already wrong; they exist because #446's fatal
// symptom was a corrupted switch frame with no other evidence.
//
// STACK TAG: written at the BOTTOM (lowest address) of a proc's kernel stack
// when its frame is built, re-checked when the scheduler is about to switch to
// it. It fires if two live procs ever share one kernel stack (the still-open
// question behind #446: both double-fault captures had the outgoing and the
// incoming switch frames on the SAME kernel stack a few hundred bytes apart)
// or if the stack was overflowed all the way down.
#define PROC_STACK_TAG_MAGIC 0x4D41595354414B01ULL   /* "MAYSTAK\1" */

void proc_stack_tag(process_t *p) {
    if (!p || !p->stack_base || p->stack_size < 32) return;
    volatile uint64_t *t = (volatile uint64_t *)p->stack_base;
    t[0] = PROC_STACK_TAG_MAGIC;
    t[1] = (uint64_t)p->pid;
}

static int g_schedbug_reports = 0;
#define SCHEDBUG_MAX 40

// Validate a proc the scheduler is about to switch TO: its saved rsp must lie
// inside its own kernel stack with room for a full switch frame, its FXSAVE
// area must be 16-byte aligned with a sane MXCSR/FCW, and its stack must still
// be tagged with its own pid.
// #75: the reproducer needs to answer "whose memory is this stray rsp",
// which means walking the table it cannot see. One accessor, no copy.
process_t *proc_table_ref(int i) {
    if (i < 0 || i >= MAX_PROCESSES) return 0;
    return &proc_table[i];
}

static void proc_check_switch_target(process_t *p, const char *when) {
    if (!p) return;
    // #446: pid 0's rsp still points at its dead synthetic frame until the
    // first switch out of it; nothing ever restores from that value.
    if (p->total_time == 0 && p->pid == 0) return;
    if (p->stack_base && p->stack_size >= 32) {
        uint64_t lo = (uint64_t)p->stack_base;
        uint64_t hi = lo + p->stack_size;
        if (p->rsp < lo || p->rsp + 136 > hi) {
            if (g_schedbug_reports < SCHEDBUG_MAX) { g_schedbug_reports++;
                kprintf("[SCHEDBUG] %s: pid=%u '%s' rsp=0x%lx OUTSIDE kernel stack [0x%lx,0x%lx)\n",
                        when, p->pid, p->name, p->rsp, lo, hi);
                // #610: WHOSE memory is it running on? A stray rsp is only
                // actionable if we know what owns the bytes under it. Two
                // answers matter: (a) another live process's kernel stack =>
                // two threads share one stack (the still-open #446), (b) a
                // slot whose stack was freed/recycled under a live thread.
                int found = 0;
                for (int _i = 0; _i < MAX_PROCESSES; _i++) {
                    process_t *q = &proc_table[_i];
                    if (q == p || !q->stack_base || q->stack_size < 32) continue;
                    uint64_t qlo = (uint64_t)q->stack_base;
                    uint64_t qhi = qlo + q->stack_size;
                    if (p->rsp >= qlo && p->rsp < qhi) {
                        kprintf("[SCHEDBUG]   -> rsp is INSIDE pid=%u '%s' stack "
                                "[0x%lx,0x%lx) state=%d\n",
                                q->pid, q->name, qlo, qhi, (int)q->state);
                        found = 1;
                    }
                }
                if (!found)
                    kprintf("[SCHEDBUG]   -> rsp belongs to NO process stack "
                            "(heap/other); delta from own stack_base=0x%lx\n",
                            p->rsp - lo);
            }
        }
        volatile uint64_t *t = (volatile uint64_t *)lo;
        if (t[0] != PROC_STACK_TAG_MAGIC || t[1] != (uint64_t)p->pid) {
            if (g_schedbug_reports < SCHEDBUG_MAX) { g_schedbug_reports++;
                kprintf("[SCHEDBUG] %s: pid=%u '%s' kstack 0x%lx tag=0x%lx owner=%lu "
                        "(expected pid %u) - SHARED STACK or deep overflow\n",
                        when, p->pid, p->name, lo, t[0], t[1], p->pid); }
        }
    }
    fxsave_area_t *fx = (fxsave_area_t *)p->fpu_area;
    if (((uint64_t)fx & (FPU_AREA_ALIGN - 1)) || (fx->mxcsr & 0xFFFF0000u) ||
        fx->fcw == 0) {
        if (g_schedbug_reports < SCHEDBUG_MAX) { g_schedbug_reports++;
            kprintf("[SCHEDBUG] %s: pid=%u '%s' invalid FPU area @0x%lx fcw=0x%x mxcsr=0x%x - reseeding\n",
                    when, p->pid, p->name, (uint64_t)fx, fx->fcw, fx->mxcsr); }
        proc_init_fpu_area(p);
    }
}

// #75: THE PROCESS TABLE NEEDS A LOCK, AND THE CLAIM MUST BE PART OF THE SEARCH.
//
// alloc_proc_slot() scanned for PROC_STATE_UNUSED and returned the slot WITHOUT
// marking it taken, and init_proc() then memset()s the whole PCB, which sets
// state back to 0 = UNUSED. So there were two windows in which a second core
// scanning the same table saw the same slot as free: between the test and the
// return, and again for the whole of init_proc().
//
// MEASURED (the #75 reproducer, first run): the AP idle process and the seclog
// kernel thread both ended up holding pid 1 - the same process_t. Each wrote
// its own stack pointer into the shared PCB, and the scheduler resumed seclog
// using the AP idle's boot-stack rsp:
//     incoming: 'seclog' pid=1 rsp=0xd3d4db8 stack=[0x10b80ec0,0x10b90ec0)
// with 0xd3d4db8 sitting just below 0xd3d5000, the AP's own stack top. A core
// resuming on another thread's stack pointer IS the "wild RIP on wild RSP" that
// panicked as Invalid Opcode.
//
// A scan that FINDS a free resource and a step that CLAIMS it must be one
// atomic operation. This lock makes them one, and the claim (a non-UNUSED
// state) is now written before the lock is dropped and deliberately preserved
// across init_proc()'s memset.
static spinlock_t g_proc_table_lock = SPINLOCK_INIT;

// #75 part 2: allocate a slot that is IMMEDIATELY marked as an idle process, so
// it can never be handed out again even in the window before the caller has
// finished initialising it. init_proc()'s memset clears is_idle, so the flag is
// re-asserted by the caller; between the two, the state claim keeps it safe.
static process_t *alloc_proc_slot(void);
static process_t *alloc_idle_slot(void) {
    process_t *p = alloc_proc_slot();
    if (p) p->is_idle = 1;
    return p;
}

static process_t *alloc_proc_slot(void) {
    // #745 (task 37): sweep FIRST, not only once the table is already full.
    // The old code reclaimed leaked zombies as a last resort, which meant a
    // zombie's 128 KB kernel stack stayed kmalloc'd until all 64 slots were
    // gone - up to ~8 MB of heap held by processes that had already exited.
    // Sweeping on every creation keeps the steady state at roughly one
    // unreaped worker, and costs a 64-entry snapshot on a path that is not
    // hot (process creation, not context switch).
    reap_orphan_zombies();

    // FIND AND CLAIM, under the lock and nothing else. cleanup_proc_slot() can
    // kfree(), so it stays OUTSIDE: the lock covers exactly the test and the
    // store that makes the slot no longer look free, which is the whole race.
    process_t *slot = NULL;
    uint64_t fl = spinlock_acquire_irqsave(&g_proc_table_lock);
    // #75 part 2: SLOT 0 AND ANY PER-CORE IDLE ARE NOT ALLOCATABLE, EVER.
    //
    // proc_table[0] is the BSP idle (pid 0). It is created once by proc_init()
    // and lives for the life of the machine, but it is not always in a state
    // this scan would reject, so a scan starting at 0 can hand out the PCB of
    // the process the boot CPU is standing on. sched_ap_enter()'s original
    // hand-rolled scan started at i=1 precisely to avoid that, and when I
    // replaced it with this shared allocator I moved the bug rather than fixing
    // it: the very next reproducer run reported
    //     incoming: 'idle' pid=0 rsp=0xd3d4db8 stack=[0x6b0ee00,0x6b1ee00)
    // with 0xd3d4db8 again the AP's boot stack - the AP idle had been given
    // slot 0 and had written its stack pointer into the BSP idle's PCB.
    //
    // The general rule, now enforced in the one place that allocates: an idle
    // process is a permanent per-core fixture, never a recyclable slot. Testing
    // is_idle as well as index 0 covers the AP idles too, which have ordinary
    // indices and must be just as untouchable.
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (proc_table[i].is_idle) continue;
        if (proc_table[i].state == PROC_STATE_UNUSED) {
            slot = &proc_table[i];
            // THE CLAIM. Any concurrent scan on another core now skips this
            // slot. PROC_STATE_READY is used rather than a new enum value
            // because nothing can select it: a process only becomes reachable
            // by the scheduler when it is put on a run queue, which the caller
            // does after init_proc(), and the reaper only looks at ZOMBIEs.
            slot->state = PROC_STATE_READY;
            break;
        }
    }
    spinlock_release_irqrestore(&g_proc_table_lock, fl);
    if (!slot) return NULL;

    // Clean up any leftover resources from the previous occupant.
    cleanup_proc_slot(slot);
    // #446: a recycled slot must never hand a stale/zero FXSAVE image to the
    // next fxrstor64. Seeded here AND in init_proc().
    fpu_area_init(slot->fpu_area);
    // cleanup_proc_slot() sets state UNUSED; re-assert the claim, or the slot
    // is free again for the whole of init_proc().
    slot->state = PROC_STATE_READY;
    return slot;
}

/**
 * Initialize a process structure
 */
static void init_proc(process_t *proc, const char *name, process_priority_t prio) {
    // #745 (local 75) CLASS FIX: the creator is the task running on THIS
    // cpu. Through the BSP-published global, a process created on an AP
    // inherited the BSP task's ppid, uid/gid/euid/egid, cwd and pgrp.
    process_t *creator = proc_current();

    // ======================================================================
    // #75 part 3: A memset THAT CLEARS THE FIELD USED FOR MUTUAL EXCLUSION.
    //
    // `state` is what alloc_proc_slot() scans for and what it writes to claim a
    // slot. memset() zeroes it, i.e. writes PROC_STATE_UNUSED, so for the window
    // between that store and the next line the slot LOOKS FREE to any core
    // scanning the table - even though it is already ours.
    //
    // Part 2 of this fix re-asserted the claim on the following line, which
    // narrowed the window from "the whole of init_proc()" to one or two
    // instructions. MEASURED: corrupt contexts fell from 6 of 6 gate-ON
    // reproducer runs to 3 of 6, with the IDENTICAL signature. Fewer is not
    // zero; a partial reduction in a race means the window got smaller, not
    // that it closed.
    //
    // The fix is to make the un-claimed instant UNOBSERVABLE rather than short:
    // zero the PCB under the SAME lock the scan takes. A scanner cannot look at
    // the table while we are inside this critical section, so the transient 0
    // cannot be seen by anyone. Routing around the window (claiming in some
    // field memset does not reach) would leave the booby trap in place for the
    // next person; this removes it.
    //
    // The section is a ~1 KB memset with interrupts masked. That is bounded and
    // is not on any hot path: process creation, not context switching.
    // ======================================================================
    {
        uint64_t __fl = spinlock_acquire_irqsave(&g_proc_table_lock);
        uint32_t __claim = (uint32_t)proc->state;
        memset(proc, 0, sizeof(process_t));
        // Preserve an existing claim; a caller that passed an unclaimed PCB
        // (proc_init's idle, which runs before any AP exists) gets one now.
        proc->state = (__claim == PROC_STATE_UNUSED) ? PROC_STATE_READY
                                                     : (process_state_t)__claim;
        // #75: pid allocation belongs in this same critical section. See below.
        // #75: pid is now assigned inside the locked block above. `next_pid++` is a
    // read-modify-write on a shared global; two cores both read 1 and both
    // write 2, which is exactly how the AP idle and seclog both emerged as
    // pid 1. That did not by itself corrupt a stack pointer, but it PROVED the
    // two allocations were genuinely concurrent rather than merely adjacent,
    // and duplicate pids would cause something else eventually.
        spinlock_release_irqrestore(&g_proc_table_lock, __fl);
    }
    // #446: the memset above zeroes fpu_area; a zeroed FXSAVE image is not a
    // valid FPU environment (FCW=0 / MXCSR=0 unmask every exception), so every
    // process gets the architectural default here regardless of which frame
    // builder runs next.
    fpu_area_init(proc->fpu_area);
    proc->pid = next_pid++;
    proc->ppid = creator ? creator->pid : 0;
    strncpy(proc->name, name, sizeof(proc->name) - 1);
    proc->state = PROC_STATE_READY;
    proc->priority = prio;
    proc->privilege = PRIV_KERNEL;  // Default to kernel mode
    proc->time_slice = TIME_SLICE_TICKS;
    proc->total_time = 0;
    // User identity: kernel processes default to root (0)
    proc->uid  = creator ? creator->uid  : 0;
    proc->gid  = creator ? creator->gid  : 0;
    proc->euid = creator ? creator->euid : 0;
    proc->egid = creator ? creator->egid : 0;
    proc->cr3 = 0;  // Will be set for user processes
    proc->user_stack_base = 0;
    proc->user_stack_size = 0;
    proc->user_rsp = 0;
    proc->user_rip = 0;
    proc->next = NULL;

    // Phase 0: default cwd for a new process is the filesystem root. Fork
    // overrides this by full-structure memcpy from the parent below.
    if (creator && creator->cwd[0]) {
        strncpy(proc->cwd, creator->cwd, PROC_CWD_MAX - 1);
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
    if (creator) {
        proc->pgrp    = creator->pgrp ? creator->pgrp : proc->pid;
        proc->session = creator->session ? creator->session : proc->pid;
    } else {
        proc->pgrp    = proc->pid;
        proc->session = proc->pid;
    }

    // The controlling terminal is inherited exactly like pgrp/session. It has
    // to be: a pipeline stage is spawned by the SHELL, not by the terminal, so
    // `less` in `ls | less` learns which pty it is attached to only by
    // inheritance. The pts binding below OVERRIDES this for a process being
    // started ON a fresh pty. -1 means the console.
    proc->ctty = creator ? creator->ctty : -1;

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
            // RECORD it as well as USE it. Before this line the index was
            // consumed and forgotten, which is why /dev/tty could not exist:
            // the one moment the kernel knew which terminal this process
            // belonged to was also the moment it discarded the fact.
            proc->ctty = g_tty_bind_pts_idx;
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

// ---------------------------------------------------------------------------
// #254/#601 anti-starvation.
//
// The ready queue is a single intrusive list kept sorted by priority and popped
// from the head: STRICT priority, no aging. Two consequences were MEASURED on
// build 969 during a 103 MB App Store install (serial [SW] instrumentation):
//
//   * the PRIO_LOW `cron` thread sat in PROC_STATE_READY for up to 1,180
//     consecutive ticks (4.7 s) at a stretch behind a Ring-3 app pegged at 99%;
//   * `idle` (proc_table[0]) is PRIO_NORMAL and sched_tick RE-QUEUED it every
//     time its slice expired, so it sat permanently AHEAD of every PRIO_LOW
//     thread. That alone is #601 ("a PRIO_LOW kernel thread starves
//     indefinitely once the desktop is running"): the fallback task was
//     competing in the run queue it is supposed to be the fallback FOR.
//
// Two fixes, both below:
//   1. idle is never placed on the ready queue. sched_schedule() already falls
//      back to proc_table[0] explicitly when the queue is empty, so idle does
//      not need to be in it, and being in it is what blocked everything below
//      PRIO_NORMAL.
//   2. a rate-limited aging sweep promotes any entry that has waited past
//      SCHED_STARVE_TICKS to the front, ONCE. The DECISION is
//      sched_age_select_rs() (rustkern/sched_age.rs); the list surgery is here.
//
// COST ON THE HOT PATH: one `sched_ticks - age_last < SCHED_AGE_PERIOD`
// comparison per sched_schedule() call. The sweep itself runs at most every
// 100 ms and walks at most SCHED_AGE_SCAN_MAX entries.
// ---------------------------------------------------------------------------
#define SCHED_STARVE_TICKS    125   // 500 ms at 250 Hz: promote after this wait
#define SCHED_AGE_PERIOD       25   // sweep at most every 100 ms
#define SCHED_AGE_MAXPROMOTE    4   // per sweep, so a burst cannot invert the queue
#define SCHED_AGE_SCAN_MAX     16   // ready-queue entries examined per sweep
#define SCHED_BOOST             8   // strictly above PRIO_REALTIME (4)

// Mirrors SchedAgeEnt in rustkern/sched_age.rs.
typedef struct {
    uint64_t ready_since;
    uint32_t prio;
    uint32_t boosted;
} sched_age_ent_t;
_Static_assert(sizeof(sched_age_ent_t) == 16,
               "sched_age_ent_t must match #[repr(C)] SchedAgeEnt in rustkern/sched_age.rs");

extern int sched_age_select_rs(const sched_age_ent_t *ents, uint32_t n, uint64_t now,
                               uint64_t bound, uint32_t max_promote,
                               uint8_t *out, uint32_t outcap);

// Number of anti-starvation promotions performed. Reported in the [HB] line so
// a long run can be audited: 0 means the fix never had to act on this workload.
volatile uint64_t g_sched_promotions = 0;

static inline int sched_eff_prio(const process_t *p) {
    return (int)p->priority + (p->prio_boost ? SCHED_BOOST : 0);
}

// ===========================================================================
// #67: SMP SCHEDULER - PER-CPU RUN QUEUES, SAFE HANDOFF, STORM DIAGNOSTIC
// ===========================================================================
//
// THE DEFECT THIS REPLACES. Everything below the `g_smp_user_sched` gate used
// to run against ONE global ready queue driven from every core. Two things
// were wrong with that, and they are independent:
//
//  1. NO LOCK. The queue's only protection was the #610 cli() at the top of
//     sched_schedule(). cli() masks THIS core's timer; it is not a lock and
//     says nothing about another core doing linked-list surgery on
//     ready_queue_head/tail at the same instant.
//
//  2. A PROCESS WAS PUBLISHED BEFORE ITS CONTEXT WAS SAVED. sched_schedule()
//     called add_to_ready_queue(prev) and THEN bkl_release_all() and THEN
//     context_switch(), and context_switch is what writes prev->rsp. In that
//     window another core could pop `prev` and switch to prev->rsp, which
//     still held the value from the PREVIOUS time prev was descheduled. Two
//     cores then execute one process on one kernel stack at two different
//     RIPs. That is the "hand off a half-saved context" in cpu/smp.c's own
//     comment, and it is unbounded memory corruption, not merely a storm.
//
// THE FIX has three parts:
//
//  A. OWNERSHIP RELEASE IN THE SWITCH ASM. process_t::sched_on_cpu is set
//     non-zero before a core switches away from a process, and cleared by
//     context_switch/context_start THEMSELVES, after the store of the outgoing
//     rsp and after fxsave64, at the point the core has already left the
//     outgoing stack. x86 stores are not reordered with other stores, so a
//     core that observes sched_on_cpu == 0 is guaranteed to see the final rsp.
//     sched_rq_pop() SKIPS any entry with sched_on_cpu != 0. This is what makes
//     the handoff safe, and it works identically for the context_start path,
//     which never returns to its caller and so cannot run any C "finish switch"
//     hook.
//
//  B. A REAL LOCK. g_rq_lock, taken irqsave, around all queue surgery.
//
//  C. PER-CPU QUEUES with a placement/steal policy (rustkern/schedwatch.rs), so
//     runnable work is spread over the cores instead of contending for one
//     list. This is also the prerequisite for ever narrowing the #279 BKL: with
//     the queues unlocked, the BKL was the only thing serialising them at all.
//
// GATE. All of this is inert unless g_smp_user_sched != 0. With the gate off,
// add_to_ready_queue()/remove_from_ready_queue() take the original global-queue
// path unchanged, which is what ships.
//
// The STORM DETECTOR below is NOT gated: a context-switch storm is a bug on one
// core too, and the recorded failure mode is a SILENT wedge with no panic and
// no log line. Detection is always on; only the reaction is gated
// (SCHEDSTORMPANIC=1 turns the report into a kpanic), exactly as
// sync/noblock.c does for #426.

// #143: this used to be "#define MAYTERA_MAX_CPUS 8", one of FIVE disagreeing
// answers to "how many CPUs" (see cpu/cpumax.h). It was not just the number of
// queues: sched_rq_cpu() folds any core id >= it onto queue 0 and
// sched_rq_set_consumer() ignores any core id >= it, so it was also the real,
// undocumented cap on which cores could schedule user work at all. There is now
// one run queue per SUPPORTED cpu, and one definition of that number.
#define SCHED_RQ_SCAN_MAX     64    // == MAX_PROCESSES: bounds every queue walk

typedef struct {
    process_t *head;
    process_t *tail;
    uint32_t   len;
} sched_rq_t;

static sched_rq_t g_rq[MAYTERA_MAX_CPUS];
static spinlock_t g_rq_lock = SPINLOCK_INIT;

// ---------------------------------------------------------------------------
// #130 SIGNATURE-A INSTRUMENT (2026-08-14). DIAGNOSTIC ONLY, not a fix.
// ---------------------------------------------------------------------------
// Three hang captures on a 4-vCPU SMP boot showed two cores spinning on this
// exact lock (RDI = &g_rq_lock) while the other two spun in bkl_take_locked.
// All four cores were accounted for and none progressing, so the holder of THIS
// lock had to be one of the BKL spinners. That is INFERENCE. These globals turn
// it into an identification: they name the owning core and the acquiring line,
// readable from a wedged guest over QMP.
//
// #118 TRAP, deliberately avoided: a tag written OUTSIDE the critical section,
// or by every path rather than only the winner, names whoever ran LAST instead
// of whoever HOLDS the lock. #118 lost a cycle to exactly that. So the owner is
// published only AFTER the acquire returns (only the winner reaches it) and
// cleared BEFORE the release, so a free lock can never name a stale owner.
extern uint32_t smp_this_cpu(void);
volatile int      g_rq_owner_cpu  = -1;
volatile int      g_rq_owner_line = 0;

// #143 part 2: MEASURE THE LOCK BEFORE RESTRUCTURING IT.
//
// The ticket asks for per-queue locking. The prerequisite is a number: how many
// acquisitions actually collide, and for how long is the lock held. Neither
// existed. g_rq_acquires (the counter this replaces) was a plain non-atomic ++
// on a shared word with no reader anywhere in the kernel, so it was neither
// accurate nor observable: it could not answer the question the ticket is about.
//
// These four quantities are what decide the design:
//
//   acquires/contended  whether cores collide at all. With the #67 gate OFF -
//                       the shipping default - only the BSP runs the scheduler,
//                       so this SHOULD be zero and a non-zero reading is itself
//                       a finding.
//   held total          what fraction of wall time the lock is held. This is the
//                       one that says whether splitting the lock can help: a
//                       lock held 2% of the time is not a ceiling however many
//                       cores want it.
//   held max + line     the longest single hold and WHICH RQ_LOCK() call site
//                       produced it. sched_rq_push() holds this lock while it
//                       walks every queue and calls into the Rust placement
//                       policy, so the critical section is O(ncpu * queue depth)
//                       with interrupts off. If that dominates, shortening the
//                       section beats splitting the lock, and it is a far
//                       smaller change to make on a config that already wedges.
//
// #118 TRAP, avoided the same way the owner tag above avoids it: the hold clock
// starts only AFTER the acquire returns, so only the winner ever writes it, and
// the elapsed time is computed BEFORE the release. A hold time sampled outside
// the critical section measures whoever ran last, not whoever held it.
//
// #130 INVARIANT: the core id is read once, after irq_save() inside the acquire,
// and reused at unlock from a variable the lock itself protects. It is never
// re-read across a point where interrupts could have been enabled.
static spin_acct_t g_rq_acct = SPIN_ACCT_INIT;
static volatile uint64_t g_rq_held_us     = 0;   // total us held, all cores
static volatile uint64_t g_rq_held_max    = 0;   // longest single hold, us
static volatile int      g_rq_held_max_ln = 0;   // RQ_LOCK() line that held it
static volatile int      g_rq_held_max_cpu = -1;
// Written only by the lock holder, read only by the lock holder. Protected by
// g_rq_lock itself, which is why they need no atomics.
static uint64_t g_rq_hold_t0  = 0;
static int      g_rq_hold_cpu = -1;

static inline uint64_t rq_lock_at(int line) {
    uint64_t f = spinlock_acquire_irqsave_acct(&g_rq_lock, &g_rq_acct);
    int c = (int)smp_this_cpu();
    g_rq_owner_cpu  = c;
    g_rq_owner_line = line;
    g_rq_hold_cpu   = c;
    g_rq_hold_t0    = mono_us();
    return f;
}
static inline void rq_unlock(uint64_t f) {
    uint64_t t0 = g_rq_hold_t0;
    uint64_t held = t0 ? (mono_us() - t0) : 0;
    g_rq_held_us += held;
    if (held > g_rq_held_max) {
        g_rq_held_max     = held;
        g_rq_held_max_ln  = g_rq_owner_line;
        g_rq_held_max_cpu = g_rq_hold_cpu;
    }
    g_rq_hold_t0    = 0;
    g_rq_owner_line = 0;
    g_rq_owner_cpu  = -1;
    spinlock_release_irqrestore(&g_rq_lock, f);
}
#define RQ_LOCK() rq_lock_at(__LINE__)

// #67: WHICH CORES ACTUALLY CONSUME THEIR RUN QUEUE. Bit N set means core N is
// driving sched_schedule() and will pop its own queue.
//
// MEASURED, on the first gate-ON boot of build 246: without this, the placement
// policy put work on cpu1 purely because cpu1's queue was shallower, cpu1's SMP
// work loop has no code path that pops a run queue, and the process NEVER RAN.
// The serial log showed "[SCHEDCORE] cpu0=.../0q cpu1=0%/0csw/1q" frozen for
// the whole run and the boot stopped after COMPOSITOR_UP with no DESKTOP_READY,
// no panic and no error - the silent-stranding failure this ticket is about,
// reproduced by the fix for it.
//
// Bit 0 is set from the start because the BSP has always been the scheduler. An
// AP sets its own bit only while it is genuinely running the scheduler
// (smp_ap_run_user), and clears it when it stops. Giving the APs a real
// scheduler entry point of their own is the REMAINING WORK on this ticket, and
// it needs a PER-CORE idle process first: sched_schedule()'s empty-queue
// fallback is the single global proc_table[0], so two cores reaching it at once
// would run one process on one kernel stack - the same class of bug as the
// half-saved handoff, just arrived at from the other direction.
volatile cpumask_t g_rq_consumers = 1u;

// #75 part 4: HAS proc_init() RUN YET.
//
// The AP bring-up (main.c, right after the FAT root is mounted so /SMPSCHED.TXT
// is readable) happens roughly 500 lines BEFORE proc_init(). An AP therefore
// called sched_ap_enter(), allocated a process-table slot for its idle process
// and took a pid from next_pid - and then proc_init() ran, set EVERY slot to
// PROC_STATE_UNUSED and reset next_pid to 1, silently destroying that claim.
// The slot was then handed to the next process created, which is why the AP
// idle and `seclog` both came out as pid 1 with one process_t between them.
//
// This is why making the claim atomic (part 1), making idle slots
// non-allocatable (part 2) and making pid allocation atomic (part 3) each
// reduced the corruption without eliminating it: they all defend the table
// against a CONCURRENT scanner, and this is a LATER WIPE by the owner of the
// table itself. No amount of locking helps against being reset afterwards.
//
// An AP that arrives early stays in its kernel-jobs-only loop and retries; it
// becomes a scheduler consumer on a later pass, once there is a process
// subsystem for it to join.
volatile int g_proc_init_done = 0;

// ===========================================================================
// #75: THE POP-TO-SWITCH WINDOW, INSTRUMENTED.
//
// The exit-unlink fix failed: 5 of 6 runs still corrupt, all reason 3
// ("incoming task is not RUNNING"). The pop-side assertion fired once (a dead
// task WAS still found in a queue) but did not prevent the corruption, and
// those two facts together localise the window: if the task had been dead when
// POPPED, the assertion would have skipped it. It was popped cleanly and was
// state=3 a moment later at the pre-switch validator. So THE TASK DIES BETWEEN
// THE POP AND THE SWITCH.
//
// Two candidates remained, and reasoning about which has been wrong three times
// in this ticket, so this measures instead:
//   (1) a task RUNNING on one core is simultaneously present in another core's
//       run queue - sched_on_cpu covers only the mid-switch window, not "is
//       running right now";
//   (2) exit does not synchronise with a core that has already SELECTED the
//       task, so unlinking it from the queue is too late.
//
// g_sel[cpu] holds the task this core has committed to running, from the moment
// remove_from_ready_queue() hands it over until the switch completes. Exit
// checks it and says so. If (2) is the mechanism, an exiting task will be found
// in some other core's g_sel[] slot; if (1) is, the task will be seen RUNNING
// at the moment it is popped.
// ===========================================================================
static inline uint32_t sched_rq_cpu(void);   // defined below
static process_t *volatile g_sel[MAYTERA_MAX_CPUS];

// #75: CLEAR IT ON EVERY PATH OUT. g_sel was set after the pop and cleared only
// at the END of sched_schedule() and on the abandon path - not on its early
// returns. It could therefore hold a stale pointer to a task selected long ago
// that had since run and been switched away from, and an exiting task matching
// that stale entry reported a race THAT NEVER HAPPENED. That false positive is
// what produced the "CANDIDATE 2 CONFIRMED 4 of 4" result I have since
// withdrawn. A variable meaning "currently" that is cleared only on the success
// path eventually means "at some point".
// ===========================================================================
// #75 THE FIX: A TASK MUST NEVER BE IN A RUN QUEUE WHILE IT IS STILL EXECUTING.
//
// ROOT CAUSE, measured. sched_tick() did:
//     cur->state = PROC_STATE_READY;
//     add_to_ready_queue(cur);          <- queued_by=0x5986e8
//     sched_schedule();
// For the interval between the enqueue and the switch, that task is in a run
// queue AND still running on this core. Another core pops it - finding it
// entirely valid, because it IS valid (enq=READY, pop=READY) - pins it, and
// commits to switching, while the task carries on executing here and writes its
// own state in wait_event() (BLOCKED, now=3) or finish_wait() (RUNNING, now=2).
// The same window exists at the second enqueue site: a task that has set itself
// SLEEPING but not yet switched away is seen by wake_sleeping_procs() on another
// core and enqueued (queued_by=0x5971aa).
//
// WHY NOT GUARD IT. The obvious fix is to arm an ownership flag earlier and have
// writers check it. There are 167 direct writes to ->state across 20 files; a
// guard each caller must honour is a convention, not an invariant, and this
// project's recurring failure mode is exactly that.
//
// WHAT THIS DOES INSTEAD. The enqueue is DEFERRED until the task has actually
// left the CPU, so the bad state cannot be represented: a still-running task is
// simply not in any queue for another core to find. There is then nothing to
// guard and nothing for a future writer to forget.
//
// WHERE THE DRAIN RUNS, and this is the part this ticket has got wrong three
// times: NOT after the switch call. context_start() never returns to its caller,
// so a C statement there executes on one path and not the other. The drain runs
// at the TOP of sched_schedule(), which every path reaches - the core that
// switched away comes back through here on its next schedule, and a core that
// IRETQ'd to Ring 3 re-enters on its next timer tick (<= 4 ms at 250 Hz). Both
// paths are covered by construction rather than by inspection.
#define SCHED_DEFER_MAX 4
static process_t *volatile g_defer[MAYTERA_MAX_CPUS][SCHED_DEFER_MAX];

// #75 EVIDENCE 2. The deferred-enqueue design routes every enqueue through
// add_to_ready_queue(), which refuses a task that is still executing. The
// wakeprobe shows that refusal working in the common case (5 of 6 runs saw the
// suspect decision, only 1 saw corruption), so the remaining failure is a
// one-in-five SUB-CASE. Two shapes would explain that, and they are different
// bugs:
//   (a) a route reaches sched_rq_push() WITHOUT passing through the funnel - a
//       single funnel with a side door, which is a common shape and would not
//       require anything to be subtly wrong inside the funnel at all; or
//   (b) the funnel is entered and WRONGLY ALLOWS the enqueue.
// These counters distinguish them directly rather than by argument.
volatile uint64_t g_enq_refused   = 0;   // funnel deferred a still-executing task
volatile uint64_t g_enq_allowed   = 0;   // funnel allowed one anyway (no defer room)
volatile uint64_t g_enq_sidedoor  = 0;   // reached sched_rq_push() outside the funnel
static uint8_t volatile g_in_funnel[MAYTERA_MAX_CPUS];

// ===========================================================================
// #167: THE DEFER TABLE RECORDS THAT AN ENQUEUE IS OWED, NOT WHAT WAS OWED.
//
// sched_defer_enqueue() stores a bare process_t*. sched_drain_deferred() then
// has to reconstruct, from p->state alone, whether the owed enqueue is still
// wanted - and p->state is a different variable that any of 167 writers can
// change in between. That reconstruction is wrong in BOTH directions:
//
//   LOST WAKE (the (b) half of this ticket). Every caller of
//   add_to_ready_queue() pre-sets state = READY before calling, EXCEPT
//   proc_wake(), which relies on the funnel to perform the transition (its own
//   comment says so: "add_to_ready_queue sets state = READY as a side effect").
//   The funnel's #75 defer path returns BEFORE that assignment. So a wake that
//   lands on a task in the window between sched_self_block() and the context
//   switch is deferred with the task still marked BLOCKED/SLEEPING, and the
//   drain's `if (p->state == PROC_STATE_READY)` then silently DROPS it. The
//   task ends up BLOCKED, wait_entry=0, wake_time=0, rq_queued=0, rq_wanted=0,
//   on no queue and claimed by no core: exactly the state sync/waitq.c
//   describes under #610 as "It is gone", and exactly the state #165 captured
//   twice for pid 21 'audioinit'.
//
//   SPURIOUS ENQUEUE (the (c) half). The mirror image: if the state is READY
//   at drain time for a task that is in fact executing, the drain queues a
//   RUNNING task and a second core can switch into its live kernel stack.
//
// These counters separate the two and are readable out of a HUNG guest over
// QMP, which the serial log is not (#307: the iMac has no serial at all, and
// under the GUI the console is silent). g_enqlost_* keeps the FIRST offender's
// identity for the same reason.
// ===========================================================================
volatile uint64_t g_enq_lost      = 0;   // drain dropped an owed WAKE: lost wakeup
volatile uint64_t g_enq_dropped   = 0;   // drain dropped an owed requeue (legitimate)
volatile uint64_t g_enq_wakefix   = 0;   // funnel performed the deferred transition
volatile uint32_t g_enqlost_pid   = 0;
volatile uint32_t g_enqlost_state = 0;   // state at the drain that dropped it
volatile uint32_t g_enqlost_atenq = 0;   // state the funnel saw when it deferred
volatile uint64_t g_enqlost_ra    = 0;   // return address of that funnel call

// ===========================================================================
// #167: IS THE DEADLINE ACTUALLY EXPIRED, BY THE CLOCK THE SWEEP CONSULTS?
//
// #165 captured every timed sleeper 16+ seconds past its deadline and still
// SLEEPING, and left the reason open. The question underneath it is more basic
// than "which path is missing": it is whether the sweep's own clock agrees that
// the deadline has passed at all. blame.md's timer-ticks-is-not-a-wall-clock
// entry is the prior - under KVM a starved vCPU gets its missed PIT ticks
// re-injected in a BURST, so a tick-derived deadline can be reached late,
// early, or in a jump - and #483/#499 moved these deadlines onto mono_ms() for
// exactly that reason. If sched_now_ms() and sched_ticks disagree, nothing
// downstream matters.
//
// So the sweep now publishes, every pass, the four numbers that settle it, all
// readable out of a HUNG guest over QMP with no serial console:
//   g_sweep_n        did the sweep run at all, and how often
//   g_sweep_now      the exact `now` it computed (sched_now_ms(), ms)
//   g_sweep_minleft  the EARLIEST deadline it looked at and chose NOT to wake
//   g_sweep_left     how many sleepers it left asleep on that pass
// g_sweep_now > g_sweep_minleft with the task still SLEEPING is a contradiction
// that can only mean the sweep is not reaching that entry. g_sweep_now BELOW
// the deadlines means the clock is behind and the deadline arithmetic is the
// bug. The two are no longer confusable, which they were in every #165 capture.
// ===========================================================================
volatile uint64_t g_sweep_n       = 0;   // wake_sleeping_procs() invocations
volatile uint64_t g_sweep_now     = 0;   // last sched_now_ms() it computed, ms
volatile uint64_t g_sweep_ticks   = 0;   // sched_ticks AT THAT SAME INSTANT
volatile uint64_t g_sweep_woke    = 0;   // sleepers woken, cumulative
volatile uint64_t g_sweep_minleft = 0;   // earliest deadline left asleep, ms
volatile uint32_t g_sweep_left    = 0;   // sleepers left asleep, last pass

// #167 GATE. One binary, both arms: default ON, cleared for a boot by dropping
// an empty /NOWAKEFIX.TXT on the ESP (main.c). #165's own control arm was
// weakened by needing two builds; the #67 gate's file design does not have that
// problem and is reused here deliberately.
int g_wake_defer_fix = 1;

// ===========================================================================
// #75 (enqrace75): THE SELECTION PIN GUARDS AGAINST EXIT BUT NOT AGAINST
// ENQUEUE.
//
// sched_pinned is set under g_rq_lock by sched_rq_pop_locked() and released
// inside the switch asm. It marks the window between a core POPPING a task and
// that core having SWITCHED to it. In that window the task is, by construction:
//     rq_queued   == 0   (the pop unlinked it)
//     sched_on_cpu == 0  (that flag is armed on the OUTGOING task, never the
//                         incoming one - see the store at the `prev` site)
//     sched_pinned != 0
// So every guard the enqueue path actually consults reads "this task is idle
// and queueable", and the one field that says otherwise is consulted by NOBODY
// on that path. Measured, not argued: the only readers of sched_pinned are the
// exit path's pin-wait, the switch asm's release, and four diagnostics.
// add_to_ready_queue() tests is_idle and sched_on_cpu; sched_rq_push() tests
// rq_queued. Neither tests sched_pinned.
//
// WHY THIS MATCHES THE FORENSICS AND THE EARLIER GUESSES DID NOT. The captured
// failure reads "popped 'condrain' pid=16 which is already RUNNING (on_cpu=0)"
// with FORENSICS "pinned=2" while the corruption was detected ON CPU 3. on_cpu
// is ZERO, which no still-executing story explains, and pinned names a
// DIFFERENT core from the one that faulted. Both are exactly what this window
// produces: cpu1 had selected the task, a third party put it back in a queue,
// cpu3 popped it and switched into a stack cpu1 was about to run on.
//
// WHY THE TEST MUST BE HERE AND NOT IN add_to_ready_queue(). The pin is set
// under the run-queue lock. A test in the funnel would be outside that lock and
// would reintroduce exactly the check-then-act window it is meant to close.
// This is the only point that is serialised against the pop.
volatile uint64_t g_enq_pinned = 0;   // enqueues attempted on a SELECTED task

// ===========================================================================
// #75 (enqrace75b): NOTHING ON THE ENQUEUE PATH ASKS WHETHER THE TASK IS
// RUNNING, BECAUSE sched_on_cpu DOES NOT ANSWER THAT QUESTION.
//
// sched_on_cpu is written in exactly four places and every one of them is a
// LEAVING event:
//     sched_self_block()    arms it, then writes SLEEPING/BLOCKED
//     sched_schedule()      arms it on the OUTGOING task (`prev`)
//     context_switch.asm    clears it once the outgoing context is saved
//     sched_self_running()  clears it when a task decides not to block
// It is NEVER armed on an incoming task: sched_schedule() writes
// `next->state = PROC_STATE_RUNNING` and switches, and next->sched_on_cpu stays
// 0 for the entire time that task is executing.
//
// So `sched_on_cpu != 0` means "mid-switch-out, or has announced it is about to
// block". It does not mean "running". One comment in this file already says so
// exactly ("sched_on_cpu covers only the mid-switch window, not 'is running
// right now'"), while add_to_ready_queue()'s refusal test, whose own comment
// reads "REFUSE TO QUEUE A TASK THAT IS STILL EXECUTING", is consulted
// everywhere else as if it meant the opposite. A task in the middle of its
// timeslice presents on_cpu == 0, sched_pinned == 0 and rq_queued == 0 to every
// guard on the enqueue path: precisely the "QUEUEABLE candidate" shape that the
// WAKEPROBE2 complement reported and that has never been examined.
//
// THE MISSING PREDICATE, and it is a direct question with a direct answer: a
// task is executing iff some core's per-cpu current_process points at it.
// smp_set_current() already maintains that at every switch, and sched_rq_push()
// ALREADY READS IT, three lines below where the enqueue is decided, to work out
// what it would be preempting. Nothing has to be inferred from a flag.
//
// These counters are a CENSUS, deliberately not a one-shot. A single sample
// cannot separate "this happens on one path" from "this happens on every path",
// which is exactly the error [WAKEPROBE] made.
volatile uint64_t g_enq_ok            = 0;  // enqueues that reached a run queue
volatile uint64_t g_enq_running       = 0;  //  ...of a task a core was RUNNING
volatile uint64_t g_enq_running_self  = 0;  //  ...running on the enqueueing core
volatile uint64_t g_enq_running_other = 0;  //  ...running on a different core
volatile uint64_t g_wake_cand_busy    = 0;  // wake sweep: the [WAKEPROBE] class
volatile uint64_t g_wake_cand_free    = 0;  // wake sweep: the [WAKEPROBE2] class

// #75 (enqrace75b): PER CALL SITE, BECAUSE "THE FORENSICS NAME THIS SITE" IS
// PARTLY A MEASUREMENT OF CALL FREQUENCY.
//
// wake_sleeping_procs() runs on every sched_schedule() on every core and walks
// the whole process table, so it is by a wide margin the most frequent caller of
// the funnel and wins the last-stamp race by default. This table keeps CALLS and
// ENQUEUES apart per return address, and records how many of each site's
// enqueues put a RUNNING task into a queue.
//
// It is also the validation harness for the running-owner probe. A probe that
// fires at every site, or at no site, discriminates nothing; this table shows
// which it is at n in the thousands rather than at n = 1.
//
// PRECISION, STATED PLAINLY: the CALL counter is incremented in the funnel
// OUTSIDE any lock, so concurrent cores can lose increments and can duplicate
// one address into two slots. It is a lower bound and a shape, never an exact
// count. The `ok` and `run` counters are incremented inside sched_rq_push()
// under g_rq_lock and are exact for the address they are filed under.
#define ENQ_RA_SLOTS 16
static void     *volatile g_enqra_addr[ENQ_RA_SLOTS];
static volatile uint64_t  g_enqra_call[ENQ_RA_SLOTS];
static volatile uint64_t  g_enqra_ok[ENQ_RA_SLOTS];
static volatile uint64_t  g_enqra_run[ENQ_RA_SLOTS];
volatile uint64_t g_enqra_over = 0;   // sites that did not fit in the table

// #75 (enqrace75b): THE POP-SIDE COMPLEMENT. An enqueue of a running task is
// the PRECONDITION for the #75 corruption, not the corruption: the task is only
// in danger if another core POPS it while it is still executing. That is a
// question about the pop, so it is asked at the pop.
//
// g_pop_running counts pops of a task some core is currently running.
// g_pop_running_other counts the subset where that core is NOT the popping one,
// which is two cores holding one task and one kernel stack: the state #75 has
// been chasing. The same-core subset is benign and expected - sched_schedule()
// pops on the core whose `cur` it may hand back, and takes the `next == cur`
// branch without switching.
//
// The report is emitted from sched_smp_report(), OUTSIDE g_rq_lock. A kprintf
// under the hottest lock in the scheduler is how the #67 pass-6 instrument
// became the fault it was measuring.
static int32_t sched_running_owner(const process_t *p);   // defined below
volatile uint64_t g_pop_running       = 0;
volatile uint64_t g_pop_running_other = 0;
// #75 (enqrace75b): the denominator. A probe that reads 0 is indistinguishable
// from a probe that is never reached, which is the trap this ticket has hit
// four times, so the number of pops it was reached on is printed beside it.
volatile uint64_t g_pop_total         = 0;
// #75 (enqrace75b): 1 while this core is between proc_yield()'s enqueue of a
// still-running task and sched_schedule() arming sched_on_cpu on it, i.e. while
// this core is holding the window open. Read at every pop to find out whether
// the two sides can be concurrent AT ALL.
volatile uint8_t  g_yield_gap[MAYTERA_MAX_CPUS];
volatile uint64_t g_pop_in_gap        = 0;   // pops taken while another core was in its gap
volatile uint64_t g_gap_opened        = 0;   // windows opened, the other denominator
static volatile uint32_t g_poprun_pid, g_poprun_owner, g_poprun_cpu, g_poprun_state;
static volatile void    *g_poprun_ra;
static volatile int      g_poprun_pending = 0;
static int               g_poprun_reported = 0;

static int enq_ra_slot(void *ra) {
    for (int i = 0; i < ENQ_RA_SLOTS; i++) {
        if (g_enqra_addr[i] == ra) return i;
        if (g_enqra_addr[i] == 0) { g_enqra_addr[i] = ra; return i; }
    }
    return -1;
}

// #75 GATE 1. One binary, both arms (the /NOWAKEFIX.TXT design, reused).
// Default ON; cleared for a boot by dropping /NOPINFIX.TXT on the ESP.
int g_enq_pin_fix = 1;

// #75 GATE 2, an INDEPENDENT defect found while reading this path.
// sched_on_cpu is documented as, and DECODED as, (cpu id + 1):
// add_to_ready_queue() computes `owner = sched_on_cpu - 1` and uses it to index
// the per-core defer table. The store on the outgoing task in sched_schedule()
// wrote a LITERAL 1 regardless of which core was running, so on cores 1..N
// every deferred enqueue from the voluntary-yield requeue was filed against
// CORE 0. With SCHED_DEFER_MAX == 4 and only core 0 draining core 0's table,
// all four cores' owed enqueues contend for four slots. Gated separately so it
// cannot confound the pin experiment. Default ON; /NOCPUFIX.TXT disables.
int g_enq_cpu_fix = 1;

// ===========================================================================
// #75 FINAL: "I AM ABOUT TO STOP RUNNING" IS ONE OPERATION, NOT TWO.
//
// THE REMAINING WINDOW. A task in the wait path writes its own state:
//     me->state = PROC_STATE_SLEEPING;   // sync/waitq.c
//     ... then eventually sched_schedule()
// Between that write and sched_schedule() arming sched_on_cpu, the task is
// SLEEPING BY ITS STATE FIELD AND STILL EXECUTING. wake_sleeping_procs() on
// another core walks the process table looking for exactly that - a SLEEPING
// task whose deadline has passed - and enqueues it. Another core then pops it,
// finds it valid (enq=READY, pop=READY), pins it, and commits to switching while
// it is still running here. MEASURED: after the sched_tick half was fixed, both
// surviving forensics lines named this one site,
// queued_by=0x597302 -> wake_sleeping_procs, proc/process.c:2702.
//
// WHY NOT JUST ARM EARLIER AT EACH SITE. That is a fix, and it is the wrong
// SHAPE: it has to be re-remembered at every present and future wait-path site
// (sync/waitq.c has two, sync/futex.c one, proc_sleep() another), which is the
// convention-not-invariant pattern this whole ticket exists to disprove. One
// operation that does both, in the right order, cannot be got wrong by a caller
// who has never heard of sched_on_cpu.
//
// ORDER MATTERS AND IS THE WHOLE POINT: ARM FIRST, THEN WRITE THE STATE. A core
// that observes the new state necessarily also observes the ownership flag,
// because on x86 stores are not reordered with other stores. add_to_ready_queue()
// then refuses the enqueue and records it as owed.
// #75 EVIDENCE 3: WHO MOVES A PINNED TASK OFF READY.
//
// Everything else is now eliminated by measurement: the enqueue was legitimate
// (enq_route=1, allowed_hot=0, sidedoor=0), the task was valid at the pop, it is
// not exit (the pin wait has never engaged), and there is no side door into the
// queue. What remains is simply WHICH writer changes ->state between the pop and
// the pre-switch check, and with 167 writers across 20 files a return address is
// worth more than any further reasoning about which it could be.
//
// This hook sits in sched_self_block(), which last pass became the single funnel
// for every wait-path self-block (sync/waitq.c x2, sync/futex.c, proc_sleep).
// So one capture covers all of them. Raw address only - resolved afterwards
// against kernel.dbg.elf, never symbolised in-kernel. Two frames, because these
// callers reach a shared helper and one frame would resolve to the same
// uninformative address for several of them. One-shot per boot so it cannot
// storm or perturb timing.
//
// The core fields matter: after the wakeprobe finding we can no longer assume
// the mutator is on a different core from the pinner. self_cpu is where this
// write is happening, pinned_by is the core that popped the task.
static int g_mutator_captured = 0;
static void sched_note_mutator(process_t *p, uint32_t new_state,
                               void *ra0, void *ra1) {
    if (g_mutator_captured || !p || p->sched_pinned == 0) return;
    g_mutator_captured = 1;
    kprintf("[MUTATOR] '%s' pid=%u state %u -> %u while PINNED by cpu%d; "
            "self_cpu=%u on_cpu=%d ra0=0x%lx ra1=0x%lx\n",
            p->name, p->pid, (uint32_t)p->state, new_state,
            p->sched_pinned - 1, sched_rq_cpu(), p->sched_on_cpu,
            (unsigned long)(uint64_t)ra0, (unsigned long)(uint64_t)ra1);
}

void sched_self_block(void *vp, uint32_t new_state) {
    process_t *p = (process_t *)vp;
    if (!p) return;
    // One frame only: -Werror=frame-address forbids __builtin_return_address(1),
    // and frame 0 is the wait-path caller of this funnel, which is what is wanted.
    sched_note_mutator(p, new_state, __builtin_return_address(0), (void *)0);
    uint32_t cpu = sched_rq_cpu();
    if (cpu < MAYTERA_MAX_CPUS) p->sched_on_cpu = (int32_t)cpu + 1;
    __asm__ volatile("" ::: "memory");   // arm is visible before the state write
    p->state = (process_state_t)new_state;
}

// The matching release, for a task that decides NOT to block after all (its
// condition came true, or the wait was interrupted). Without this the task would
// stay marked as executing for ever and add_to_ready_queue() would refuse it for
// ever - a task that can never be queued again is a HANG, which is strictly
// worse than the race being closed. Same hazard as the six early returns in
// sched_schedule(), one level out.
//
// Order is the mirror image: write the state first, THEN disarm, so no core can
// see a RUNNING task that is not marked as owned by this one.
void sched_self_running(void *vp) {
    process_t *p = (process_t *)vp;
    if (!p) return;
    p->state = PROC_STATE_RUNNING;
    __asm__ volatile("" ::: "memory");
    p->sched_on_cpu = 0;
}

// Owed enqueues for tasks that have now left the CPU. Called at scheduler entry.
static void sched_drain_deferred(uint32_t cpu) {
    if (cpu >= MAYTERA_MAX_CPUS) return;
    for (uint32_t i = 0; i < SCHED_DEFER_MAX; i++) {
        process_t *p = g_defer[cpu][i];
        if (!p) continue;
        // Still on a core? Leave it owed; we will be back.
        if (p->sched_on_cpu != 0) continue;
        // #75: or still SELECTED by a core that has not switched to it yet.
        // Paying here would put a task back in a queue while another core is
        // between its pop and its switch, which is the window this ticket is
        // about. Same "leave it owed" treatment, same guarantee it is paid.
        if (g_enq_pin_fix && p->sched_pinned != 0) continue;
        g_defer[cpu][i] = 0;
        p->rq_wanted = 0;
        // Only queue it if it still wants to run. A task that blocked or exited
        // after we deferred it must not be resurrected.
        if (p->state == PROC_STATE_READY) { add_to_ready_queue(p); continue; }

        // #167 INSTRUMENT. We are about to drop an owed enqueue. Whether that
        // is correct depends entirely on what the funnel was asked to do, and
        // sched_state_at_enq records exactly that (it is written by
        // add_to_ready_queue() BEFORE anything overwrites the state).
        //
        //   at_enq = BLOCKED or SLEEPING  -> the caller was proc_wake(), i.e.
        //       this WAS a wake, and dropping it deletes the task from the
        //       scheduler for ever. That is the (b) failure.
        //   at_enq = RUNNING or READY     -> the caller was a requeue and the
        //       task has since blocked or exited on purpose. Dropping is right.
        //
        // CAVEAT, stated because #165 lost days to instruments that were
        // confidently wrong: sched_state_at_enq is overwritten by any LATER
        // add_to_ready_queue() call on the same task, so between the defer and
        // this drain it can be re-stamped. It is a strong hint, not a proof, and
        // the counters below should be read together with g_enq_refused.
        if (p->sched_state_at_enq == (uint32_t)PROC_STATE_BLOCKED ||
            p->sched_state_at_enq == (uint32_t)PROC_STATE_SLEEPING) {
            g_enq_lost++;
            if (g_enqlost_pid == 0) {
                g_enqlost_pid   = p->pid;
                g_enqlost_state = (uint32_t)p->state;
                g_enqlost_atenq = p->sched_state_at_enq;
                g_enqlost_ra    = (uint64_t)p->sched_enq_ra;
            }
            static int lost_warned = 0;
            if (!lost_warned) {
                lost_warned = 1;
                static const char *st[6] = { "UNUSED", "READY", "RUNNING",
                                             "SLEEPING", "BLOCKED", "ZOMBIE" };
                uint32_t s_now = (uint32_t)p->state, s_enq = p->sched_state_at_enq;
                kprintf("[ENQLOST] #167: dropped an OWED WAKE for '%s' pid=%u: "
                        "state at defer=%s(%u), state now=%s(%u), drain_cpu=%u, "
                        "enq_ra=0x%lx. This task is now on no queue and no core; "
                        "nothing can wake it again.\n",
                        p->name, p->pid,
                        s_enq < 6 ? st[s_enq] : "?", s_enq,
                        s_now < 6 ? st[s_now] : "?", s_now,
                        cpu, (unsigned long)(uint64_t)p->sched_enq_ra);
            }
            // #167 FIX, arm 2 of 2. The transition belongs at the funnel (see
            // add_to_ready_queue), so with the fix ON this branch is dead. It is
            // kept as a BACKSTOP for any future caller that reaches the funnel
            // without pre-setting READY: recovering a lost task is strictly
            // better than deleting it, and the counter still records that it
            // happened rather than hiding it.
            if (g_wake_defer_fix) {
                p->state = PROC_STATE_READY;
                add_to_ready_queue(p);
            }
            continue;
        }
        g_enq_dropped++;
    }
}

// Record an owed enqueue. Returns 1 if taken, 0 if there was no room (in which
// case the caller must fall back to enqueuing directly).
static int sched_defer_enqueue(uint32_t cpu, process_t *p) {
    if (cpu >= MAYTERA_MAX_CPUS || !p) return 0;
    for (uint32_t i = 0; i < SCHED_DEFER_MAX; i++) {
        if (g_defer[cpu][i] == p) { p->rq_wanted = 1; return 1; }
        if (!g_defer[cpu][i]) { g_defer[cpu][i] = p; p->rq_wanted = 1; return 1; }
    }
    return 0;
}

#define SCHED_SEL_CLEAR() do { \
    uint32_t __sc = sched_rq_cpu(); \
    if (__sc < MAYTERA_MAX_CPUS) g_sel[__sc] = 0; \
} while (0)
static uint32_t            g_sel_state[MAYTERA_MAX_CPUS];
volatile uint64_t g_sel_exit_hits = 0;   // exits that raced a selection
volatile uint64_t g_sel_run_hits  = 0;   // pops that returned a RUNNING task

// Called by the exit path BEFORE the task stops being runnable.
// #75: WAIT FOR ANY CORE THAT HAS ALREADY SELECTED THIS TASK.
//
// WHO WAITS: the exiting task, not the selecting core. A core that has committed
// to running something is on the critical path for responsiveness; a task that
// is exiting has already finished its work and can afford to block. It also puts
// the wait on the rarer path.
//
// HOW LONG: the pop-to-switch interval is naturally sub-microsecond. It only
// looks long under `make SCHEDRACE=1`, which deliberately inserts up to
// SCHEDRACE_US at each of two sites to widen the race. The bound below is
// generous enough to cover the widened window as well, so the reproducer cannot
// turn a correct fix into an apparent hang - the harness must not be able to
// manufacture the failure it is testing for.
//
// ON EXPIRY: it does NOT proceed silently (that is the corruption) and it does
// NOT keep waiting (that is a hang, and this ticket has already measured a vCPU
// descheduled by the host for seconds - spinning unbounded on a pin held by a
// descheduled core would convert a rare corruption into a multi-second stall,
// which is exactly the class of defect being chased). It reports loudly and
// returns. Safety then falls to the selecting core, which re-checks the task
// immediately before switching and ABANDONS the switch if it is no longer
// runnable - a real recovery, not another test that can go stale, because by
// then the task's state can no longer change back.
//
// LOCK ORDERING: this wait holds NOTHING. Taking the run-queue lock here would
// deadlock against the pinning core, which needs that same lock to make
// progress. The pin is a plain volatile read.
#define SCHED_PIN_WAIT_US 20000   // 20 ms: ~4 orders above the real window, and
                                  // well above 2 x SCHEDRACE_US at any setting
volatile uint64_t g_pin_waits = 0, g_pin_timeouts = 0;

static void sched_wait_for_pin(process_t *p) {
    if (!p || !p->sched_pinned) return;
    g_pin_waits++;
    uint64_t t0 = mono_us();
    while (p->sched_pinned) {
        if (mono_us() - t0 >= SCHED_PIN_WAIT_US) {
            g_pin_timeouts++;
            kprintf("[SCHED75] pin wait TIMED OUT after %u us for '%s' pid=%u "
                    "(pinned by cpu%d). Proceeding; the selecting core will "
                    "abandon the switch.\n", (unsigned)SCHED_PIN_WAIT_US,
                    p->name, p->pid, p->sched_pinned - 1);
            return;
        }
        __asm__ volatile("pause");
    }
}

void sched_note_exit(void *vp) {
    process_t *p = (process_t *)vp;
    if (!p) return;
    uint32_t me = sched_rq_cpu();
    for (uint32_t c = 0; c < MAYTERA_MAX_CPUS; c++) {
        if (g_sel[c] != p) continue;
        // #167: THE SAME-CORE CASE IS NOT A RACE, AND IT WAS THE ONLY CASE THAT
        // EVER FIRED. g_sel[c] is set when core c POPS a task and is cleared at
        // core c's NEXT entry to sched_schedule(), not when the switch
        // completes. So a task that core c popped, switched to, and is now
        // RUNNING still reads g_sel[c] == p for its whole timeslice - and when
        // it exits, c == me and this test matched.
        //
        // MEASURED across #165's 55 boots: every one of the 14 CANDIDATE 2
        // lines is 'audioinit' with "state at pop=1, now=2", i.e. popped READY
        // and now RUNNING, which IS the benign shape - and they appear in runs
        // that BOOTED FINE (arm-gateon run01/02/06/07, arm-stall run05/07,
        // arm-base run10) as well as in the two that panicked. A detector that
        // fires just as often on healthy boots has no discriminating power, and
        // #165's conclusion that both panics were "CANDIDATE 2 on audioinit
        // exiting" therefore rests on a signal that is present either way.
        //
        // A core cannot be about to switch INTO a task while it is executing
        // that task's exit path. Skip self; the cross-core case, which is the
        // one the probe was built for, is unaffected and has never fired.
        if (c == me) continue;
        g_sel_exit_hits++;
        static int warned = 0;
        if (!warned) {
            warned = 1;
            kprintf("[SCHED75] CANDIDATE 2 CONFIRMED: '%s' pid=%u is EXITING on "
                    "cpu%u while cpu%u has already SELECTED it (state at pop=%u, "
                    "now=%u). Unlinking from the queue is too late: that core "
                    "already holds the pointer and is committed to switching.\n",
                    p->name, p->pid, me, c, g_sel_state[c], (uint32_t)p->state);
        }
    }
    // #75: and now WAIT for it, which is the actual fix. Everything above this
    // line is the diagnostic that identified the window.
    sched_wait_for_pin(p);
}

// #67 pass 2: PER-CORE IDLE PROCESSES.
//
// sched_schedule()'s empty-queue fallback used to be the single global
// proc_table[0]. With one core that is correct and has been for the life of the
// project. With two cores consuming, both can reach the fallback in the same
// instant and both switch to the SAME process_t, running one process on one
// kernel stack from two cores at two RIPs. That is the same class of corruption
// as the half-saved context handoff this ticket already fixed, arrived at from
// the opposite direction, and it is why the per-core idle had to land BEFORE an
// AP was allowed to run the scheduler at all.
//
// Slot 0 is the existing pid-0 idle and is unchanged. Each AP allocates its own
// in sched_ap_enter(). A NULL slot means "this core has no idle process", and
// the scheduler REFUSES to switch rather than falling back to somebody else's.
static process_t *g_cpu_idle[MAYTERA_MAX_CPUS];

// #67 pass 2: cross-core preemption. Set by sched_request_resched() when a
// process is placed on a core that is running something it outranks; consumed
// by that core's own sched_tick(). Bounded by one tick (4 ms at 250 Hz), which
// is why the IPI is also sent: it wakes a HALTED core immediately instead of
// leaving it asleep until its next timer interrupt.
static volatile uint8_t g_need_resched[MAYTERA_MAX_CPUS];

// Defined below; needed here so the resched poke can tell self from remote and
// so the AP idle-loop work hint can size its scan.
static inline uint32_t sched_rq_cpu(void);
static inline uint32_t sched_rq_ncpu(void);

void sched_request_resched(uint32_t cpu) {
    if (cpu >= MAYTERA_MAX_CPUS) return;
    g_need_resched[cpu] = 1;
    if (cpu != sched_rq_cpu()) {
        // #67 pass 8: DIRECTED. Broadcasting here interrupted every core on
        // every preemption request, and each wake handler takes the BKL.
        extern void smp_wake_cpu(uint32_t cpu);
        smp_wake_cpu(cpu);
    }
}

// #67 pass 4: cheap "is there anything for this core to do" hint, read WITHOUT
// the run-queue lock on purpose.
//
// sched_schedule() is not a cheap call: it walks all MAX_PROCESSES entries in
// wake_sleeping_procs(), reads the monotonic clock, and runs the aging sweep.
// On the BSP that happens once per timer tick. The AP idle loop, however, is
// woken by every IPI - and smp_work_submit() sends one on every job - so calling
// the full scheduler on each wake put a 64-entry table walk plus two shared
// spinlocks in a loop that runs thousands of times a second, ping-ponging
// cachelines with the BSP the whole time.
//
// An unlocked read of the queue length is a HINT and is allowed to be stale in
// both directions. Stale-high costs one wasted sched_schedule(). Stale-low
// cannot lose work: the entry that made it non-zero was published under the
// lock by sched_rq_push(), which also sends the wake IPI, so this core is woken
// again and re-reads it. It is never the only thing keeping a queue drained -
// the timer tick on the BSP and the steal path both reach it too.
// #167: AN OWED ENQUEUE IS WORK, AND ONLY THIS CORE CAN PAY IT.
//
// sched_drain_deferred() is reachable from exactly one place, sched_schedule(),
// and on an AP sched_schedule() is reached from sched_ap_enter()'s loop ONLY
// when sched_rq_has_work() says so - and that function looks at run-queue
// LENGTHS and knows nothing about the defer table. So an AP holding an owed
// enqueue, with nothing in any run queue, takes its "cli; re-check; sti; hlt"
// and never pays it. The task is runnable, wants a core, and is referenced by
// exactly one core that has just decided it has nothing to do.
//
// This is a SECOND lost-wake shape in the same table and it is NOT the one that
// deleted audioinit: there the owed entry is reached and discarded because the
// transition was missing, here it is never reached at all. A capture tells them
// apart in one field - a STRANDED task reads rq_wanted=1, a DROPPED one reads
// rq_wanted=0, and #165's audioinit read 0. It also matters more AFTER the
// transition fix than before it: previously a wake in this position was thrown
// away by the drain anyway, so the strand had nothing left to lose.
//
// ONLY PAYABLE entries count. An entry whose task is still executing somewhere
// cannot be paid yet, and reporting it as work would turn the idle loop into a
// spin on something it is not allowed to do. And no IPI is needed to wake this
// core: an entry becomes payable exactly when the owning core's own switch asm
// clears sched_on_cpu, which happens while that core is executing, before it
// next re-tests this function.
static int sched_defer_payable(uint32_t cpu) {
    if (cpu >= MAYTERA_MAX_CPUS) return 0;
    for (uint32_t i = 0; i < SCHED_DEFER_MAX; i++) {
        process_t *p = g_defer[cpu][i];
        if (p && p->sched_on_cpu == 0 &&
            !(g_enq_pin_fix && p->sched_pinned != 0)) return 1;   // #75
    }
    return 0;
}
// How many times an owed enqueue was the ONLY reason this core did not halt.
// Zero means the strand is theoretical on this workload; non-zero means a task
// would have been left runnable-but-unreachable that many times.
volatile uint64_t g_defer_strand = 0;

static inline int sched_rq_has_work(uint32_t cpu) {
    if (cpu < MAYTERA_MAX_CPUS && g_rq[cpu].len) return 1;
    if (g_wake_defer_fix && sched_defer_payable(cpu)) { g_defer_strand++; return 1; }
    // Nothing local: is there anything anywhere worth stealing? Same hint
    // semantics; the steal itself re-checks under the lock.
    uint32_t n = sched_rq_ncpu();
    for (uint32_t i = 0; i < n && i < MAYTERA_MAX_CPUS; i++)
        if (i != cpu && g_rq[i].len) return 1;
    return 0;
}

// This core's idle process, or NULL before it has one.
static inline process_t *sched_idle_for_cpu(uint32_t cpu) {
    if (cpu >= MAYTERA_MAX_CPUS) return g_cpu_idle[0];
    return g_cpu_idle[cpu];
}
void sched_rq_set_consumer(uint32_t cpu, int on) {
    if (cpu >= MAYTERA_MAX_CPUS) return;
    // #143: CPUMASK_BIT, not (1u << cpu). The old shift was of a uint32 and was
    // correct only because the guard above happened to be 8. Nothing connected
    // the guard to the mask width, so raising the cap past 32 would have been
    // silent UB: x86 masks the shift count, so core 32 would have aliased onto
    // core 0 and stranded its work with no log line. cpu/cpumax.h now fails the
    // build instead.
    if (on) g_rq_consumers |= CPUMASK_BIT(cpu);
    else    g_rq_consumers &= ~CPUMASK_BIT(cpu);
}

// --- Rust policy seam (rustkern/schedwatch.rs) ------------------------------
extern int sched_storm_verdict_rs(uint64_t switches, uint64_t ticks, uint32_t limit);
// #67 pass 2: per-core snapshot handed to the placement policy. Mirrors
// SchedCoreState in rustkern/schedwatch.rs.
typedef struct {
    uint32_t queue_len;   // entries waiting on this core
    uint32_t above_len;   // of those, how many OUTRANK the arriving process
    int32_t  cur_prio;    // eff prio running here, or SCHED_PRIO_NONE if idle
    uint32_t flags;       // bit0 = this core consumes its own queue
} sched_core_state_t;
_Static_assert(sizeof(sched_core_state_t) == 16,
               "sched_core_state_t must match #[repr(C)] SchedCoreState in rustkern/schedwatch.rs");
#define SCHED_PRIO_NONE   (-1)
#define SCHED_CORE_CONSUMER 1u
#define SCHED_PLACE_PREEMPT 0x100

extern int sched_place_rs(const sched_core_state_t *cores, uint32_t ncpu,
                          int32_t prio, uint32_t prev_cpu);
extern int sched_steal_rs(const sched_core_state_t *cores, uint32_t ncpu,
                          uint32_t self_cpu, const int32_t *top_prios);
extern int sched_core_pct_rs(const uint64_t *busy, uint32_t ncpu, uint64_t total,
                             uint32_t *out, uint32_t outcap);
extern uint32_t sched_watch_selftest_rs(void);

// --- storm detector state (per core) ---------------------------------------
#define SCHED_STORM_WINDOW_TICKS  250   // ~1 s at 250 Hz
// Switches per tick above which a window is a storm. MEASURED anchors: the
// #421 livelock ran at ~380/tick sustained (190,000 in 2 s); a busy MayteraOS
// desktop with a game running measures in single digits per tick. 80 sits an
// order of magnitude above the busy case and a factor of ~5 below the storm,
// so neither reading is near the line.
#define SCHED_STORM_PER_TICK       80
#define SCHED_STORM_REPORT_GAP   1000   // ticks between repeat reports per core

static uint64_t g_sw_count[MAYTERA_MAX_CPUS];      // total switches by this core
static uint64_t g_sw_win_sw[MAYTERA_MAX_CPUS];     // count at window open
static uint64_t g_sw_win_tick[MAYTERA_MAX_CPUS];   // tick at window open
static uint64_t g_sw_last_report[MAYTERA_MAX_CPUS];
static uint64_t g_core_busy[MAYTERA_MAX_CPUS];     // ticks running a non-idle proc
// #169: how many PREEMPTION TICKS each Application Processor has actually
// taken. This exists because "the timer is armed" and "the timer fires" are
// different claims and this project has repeatedly shipped the first while
// believing the second. A counter that stays at 0 on a core says the LVT write
// went nowhere; the [APTICK] line below is the only way to tell that apart from
// a core that simply had nothing to run.

// #169 FFI to rustkern/aptick.rs. The bit values are duplicated here because C
// cannot see Rust consts; they are checked by the APTICK bit self-test in
// main.c, which asks the Rust side for the same masks it returns.
#define APTICK_BUSY     (1u << 0)
#define APTICK_ACK_RESC (1u << 1)
#define APTICK_CHARGE   (1u << 2)
#define APTICK_SETSLICE (1u << 3)
#define APTICK_SCHED    (1u << 4)
extern uint32_t ap_tick_decide_rs(uint32_t preempt_enabled, uint32_t has_cur,
                                  uint32_t is_idle, uint32_t need_resched,
                                  uint32_t slice, uint32_t *out_slice);

// #169: PROVE THE C AND RUST SIDES AGREE ON THE BIT VALUES.
//
// The APTICK_* masks above are DUPLICATED from rustkern/aptick.rs, because C
// cannot see a Rust `const`. A duplicated constant that drifts does not fail to
// build and does not fail to link: it silently makes the scheduler act on the
// wrong bit, e.g. charge CPU time where it meant to preempt. This project has
// paid for exactly that shape (blame.md: a differential ran green because both
// arms shared the wrong constant), so the agreement is MEASURED on the live
// build rather than assumed from two files that look alike.
//
// Six cases, each with one unambiguous right answer, run once. Prints PASS with
// the count or FAIL with the first disagreement. Cheap: six calls to a leaf
// function, at the moment the first AP becomes a scheduler consumer.
void aptick_selftest(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    struct { uint32_t pe, cur, idle, nr, slice, want_act, want_slice; } v[] = {
        // no current process on this core: nothing to do at all
        { 1, 0, 0, 0, 0, 0u, 0u },
        // this core's idle process, nothing pending: account nothing, switch nothing
        { 1, 1, 1, 0, 4, 0u, 0u },
        // running, slice left: charge it and decrement, do NOT switch
        { 1, 1, 0, 0, 2, APTICK_BUSY|APTICK_CHARGE|APTICK_SETSLICE, 1u },
        // running, last tick of the slice: decrement to 0 and switch
        { 1, 1, 0, 0, 1, APTICK_BUSY|APTICK_CHARGE|APTICK_SETSLICE|APTICK_SCHED, 0u },
        // cross-core preemption request: expire the slice, ack it, switch
        { 1, 1, 0, 1, 5, APTICK_BUSY|APTICK_ACK_RESC|APTICK_CHARGE|
                         APTICK_SETSLICE|APTICK_SCHED, 0u },
        // preemption globally disabled: still count the core busy, never switch
        { 0, 1, 0, 0, 5, APTICK_BUSY, 0u },
    };
    for (unsigned i = 0; i < sizeof(v)/sizeof(v[0]); i++) {
        uint32_t ns = 0xDEADu;
        uint32_t got = ap_tick_decide_rs(v[i].pe, v[i].cur, v[i].idle,
                                         v[i].nr, v[i].slice, &ns);
        int slice_ok = (got & APTICK_SETSLICE) ? (ns == v[i].want_slice)
                                               : (ns == 0xDEADu);
        if (got != v[i].want_act || !slice_ok) {
            kprintf("[APTICK] *** SELFTEST FAIL case %u: act=0x%x want 0x%x, "
                    "slice=%u. The C APTICK_* masks and rustkern/aptick.rs "
                    "DISAGREE; AP preemption is acting on the wrong bits. ***\n",
                    i, got, v[i].want_act, ns);
            return;
        }
    }
    kprintf("[APTICK] selftest PASS: 6/6 cases, C masks == rustkern/aptick.rs\n");
}static volatile uint64_t g_ap_ticks[MAYTERA_MAX_CPUS];
// How many of those ticks ended in a preemption (slice expired -> switch).
static volatile uint64_t g_ap_preempts[MAYTERA_MAX_CPUS];
volatile uint64_t g_sched_storms = 0;           // storms observed, all cores

// Which run queue this core uses. Falls back to 0 before per-cpu data is live,
// which is also the single-core answer, so the gate-off path never depends on
// SMP state being initialised.
static inline uint32_t sched_rq_cpu(void) {
    if (!g_smp_current_ready) return 0;
    uint32_t c = smp_this_cpu();
    // #67 pass 12: clamp against the number of cores that are actually ONLINE,
    // not just the array bound. smp_this_cpu() reads a per-cpu id through GS,
    // and any path where GS is not yet the final value returns something that
    // is in range for the array but has no idle process and no consumer - which
    // routes work to a queue nobody drains and sends sched_schedule() down the
    // no-idle path above. Falling back to 0 (the BSP, which always has both) is
    // always safe.
    extern uint32_t smp_get_online_count(void);
    uint32_t online = smp_get_online_count();
    if (online == 0) online = 1;
    if (c >= online || c >= MAYTERA_MAX_CPUS) return 0;
    return c;
}

// Number of queues in play. One when AP user scheduling is off, so the policy
// functions degenerate to "queue 0" and cost one comparison.
static inline uint32_t sched_rq_ncpu(void) {
    extern int g_smp_user_sched;
    if (!g_smp_user_sched) return 1;
    uint32_t n = smp_get_cpu_count();
    if (n < 1) n = 1;
    return (n > MAYTERA_MAX_CPUS) ? MAYTERA_MAX_CPUS : n;
}

// Priority-ordered insert into queue `cpu`. Caller must hold g_rq_lock.
static void sched_rq_insert_locked(uint32_t cpu, process_t *proc) {
    sched_rq_t *q = &g_rq[cpu];
    proc->next = NULL;
    proc->rq_queued = 1;
    q->len++;
    if (q->head == NULL) { q->head = q->tail = proc; return; }
    if (sched_eff_prio(proc) > sched_eff_prio(q->head)) {
        proc->next = q->head;
        q->head = proc;
        return;
    }
    process_t *prev = NULL, *curr = q->head;
    uint32_t guard = 0;
    while (curr && sched_eff_prio(curr) >= sched_eff_prio(proc) &&
           guard++ < SCHED_RQ_SCAN_MAX) {
        prev = curr;
        curr = curr->next;
    }
    if (prev) {
        proc->next = prev->next;
        prev->next = proc;
        if (prev == q->tail) q->tail = proc;
    } else {
        // Only reachable if the guard tripped, i.e. a corrupted list. Put it at
        // the head rather than dropping it: losing a runnable process is a hang.
        proc->next = q->head;
        q->head = proc;
    }
}

// Unlink `p` from queue `cpu`. Caller must hold g_rq_lock. Returns 1 if found.
static int sched_rq_unlink_locked(uint32_t cpu, process_t *p) {
    sched_rq_t *q = &g_rq[cpu];
    process_t *prev = NULL, *c = q->head;
    uint32_t guard = 0;
    while (c && guard++ < SCHED_RQ_SCAN_MAX) {
        if (c == p) {
            if (prev) prev->next = c->next; else q->head = c->next;
            if (q->tail == c) q->tail = prev;
            c->next = NULL;
            c->rq_queued = 0;
            if (q->len) q->len--;
            return 1;
        }
        prev = c;
        c = c->next;
    }
    return 0;
}

// Pop the highest-priority ELIGIBLE entry from queue `cpu`. An entry whose
// sched_on_cpu is non-zero is still executing on another core: its saved rsp is
// not final, so taking it would be the #421 half-saved-context handoff. Such an
// entry is SKIPPED, never waited on - the other core clears the flag inside its
// own context switch, microseconds away, and a wait here would be the busy-poll
// this whole ticket is about. Caller must hold g_rq_lock.
static process_t *sched_rq_pop_locked(uint32_t cpu) {
    sched_rq_t *q = &g_rq[cpu];
    process_t *prev = NULL, *c = q->head;
    uint32_t guard = 0;
    while (c && guard++ < SCHED_RQ_SCAN_MAX) {
        // #75: sched_on_cpu answers "is another core mid-switch on this task".
        // It does NOT answer "is this task still alive". A LIVENESS GUARD IS NOT
        // A VALIDITY GUARD, and an exiting or ZOMBIE task still linked here was
        // a perfectly acceptable candidate until this check existed. The real
        // fix is sched_rq_remove() at exit, so such an entry cannot be here at
        // all; this is the assertion that says so if one ever is.
        if (c->state != PROC_STATE_READY && c->state != PROC_STATE_RUNNING) {
            static int warned_dead_in_q = 0;
            if (!warned_dead_in_q) {
                warned_dead_in_q = 1;
                kprintf("[SCHED] #75: refused a queued task that is not "
                        "runnable: '%s' pid=%u state=%u. sched_rq_remove() "
                        "should have taken it out at exit.\n",
                        c->name, c->pid, (uint32_t)c->state);
            }
            prev = c; c = c->next; continue;
        }
        if (c->sched_on_cpu == 0) {
            if (prev) prev->next = c->next; else q->head = c->next;
            if (q->tail == c) q->tail = prev;
            c->next = NULL;
            c->rq_queued = 0;
            if (q->len) q->len--;
            c->prio_boost = 0;   // #254: the promotion is ONE-SHOT
            // #75: PIN IT. From here until this core switches to it (or gives
            // up on it), exit must not tear it down. Taken under the run-queue
            // lock so the pin is published before the task is reachable by
            // anything else.
            // #75 (enqrace75) CORRECTED. This was `cpu + 1`, where `cpu` is
            // the QUEUE THIS ENTRY WAS TAKEN FROM. On the steal path
            // sched_rq_pop() calls this as sched_rq_pop_locked(victim), so a
            // stolen task recorded the VICTIM's core id and not the core that
            // had actually selected it and was about to run it.
            //
            // The field is documented as "cpu id + 1 from the instant a core
            // POPS this task off a run queue until IT has switched to it", and
            // the core that switches to it is the one calling this, always.
            //
            // THIS MATTERS BEYOND TIDINESS: it is the field the #75 forensics
            // have been read through. The capture that motivated this ticket
            // shows FORENSICS "pinned=2" while the corruption was detected ON
            // CPU 3, which reads as two different cores holding one task. It is
            // not: it is ONE core (3) stealing from core 1's queue and stamping
            // core 1's id. Any inference of the form "pinned names a different
            // core from the faulting one, therefore two cores held it" is
            // unsound on every stolen task, and the steal path is precisely the
            // cross-core case the ticket is about.
            c->sched_pinned = (int32_t)sched_rq_cpu() + 1;
            c->sched_state_at_pop = (uint32_t)c->state;   // #75
            // #75 (enqrace75b): IS THIS TASK EXECUTING ON A CORE RIGHT NOW?
            // The pop already refuses c->sched_on_cpu != 0, and that flag is 0
            // for the whole of a task's timeslice, so this check is not
            // redundant with the one above it - it is the one the loop above
            // was believed to be making.
            //
            // Note also that the existing [SCHED75] CANDIDATE 1 probe cannot
            // see this class: it tests next->state == PROC_STATE_RUNNING, and
            // proc_yield() sets state = READY BEFORE enqueueing, so a task
            // queued while running presents state READY at the pop.
            g_pop_total++;
            {   // #75 (enqrace75b): is ANOTHER core holding the yield window
                // open at this instant? Separates "the hole is unreachable
                // because the kernel is serialised" from "the cores do overlap
                // and the task is simply on a queue nobody looked at".
                uint32_t __pn = sched_rq_ncpu();
                if (__pn > MAYTERA_MAX_CPUS) __pn = MAYTERA_MAX_CPUS;
                uint32_t __self = sched_rq_cpu();
                for (uint32_t __i = 0; __i < __pn; __i++)
                    if (__i != __self && g_yield_gap[__i]) { g_pop_in_gap++; break; }
            }
            {
                int32_t __pro = sched_running_owner(c);
                if (__pro != 0) {
                    g_pop_running++;
                    if ((uint32_t)(__pro - 1) != sched_rq_cpu()) {
                        g_pop_running_other++;
                        if (!g_poprun_reported && !g_poprun_pending) {
                            g_poprun_pid   = c->pid;
                            g_poprun_owner = (uint32_t)(__pro - 1);
                            g_poprun_cpu   = sched_rq_cpu();
                            g_poprun_state = (uint32_t)c->state;
                            g_poprun_ra    = c->sched_enq_ra_ok;
                            g_poprun_pending = 1;
                        }
                    }
                }
            }
            return c;
        }
        prev = c;
        c = c->next;
    }
    return NULL;
}

// #75: TAKE A PROCESS OUT OF EVERY RUN QUEUE. Call before it stops being a
// thing that may be run (exit, teardown, slot recycle).
//
// THE DEFECT THIS FIXES. proc_exit() did NOTHING about queue membership: it set
// PROC_STATE_ZOMBIE and called sched_schedule(). A process that was still linked
// into a run queue therefore stayed there, and the only thing standing between
// it and being run was sched_rq_pop_locked()'s check of sched_on_cpu - which
// answers "is another core mid-switch on this task", not "is this task still
// alive". MEASURED by the #75 reproducer at SCHEDRACE_US=200, 3 of 3 booted
// runs, every one reason 3:
//     reason 3: incoming task is not RUNNING
//     incoming: 'xhci_evt' pid=3 state=3 rsp=0x10b991d0
// with the other core's ring showing, at the same instant:
//     [PROC] Process 'xhci_evt' (PID 3) exiting
// One core was committing to run a process the other was tearing down. Its rsp
// still looked valid at the moment of the switch, which is exactly how it turns
// into a wild RSP later, once that stack is freed and recycled.
//
// WHY THIS AND NOT ONLY A STATE CHECK AT THE POP. Filtering at the consumer
// leaves a queue that can contain garbage and a consumer that has to remember to
// look - the same shape as every other defect in this ticket, a guard at the
// caller instead of an invariant in the primitive. The queue should not be able
// to contain a dead process in the first place. The pop-side check below stays
// as an assertion, not as the fix.
void sched_rq_remove(void *vp) {
    process_t *p = (process_t *)vp;
    if (!p) return;
    uint64_t fl = RQ_LOCK();
    if (p->rq_queued) {
        // It can only be on one queue, but scan all of them: the entry may have
        // been placed by a different core than the one exiting it, and a
        // mis-tracked rq_queued must not leave a dangling node behind.
        for (uint32_t c = 0; c < MAYTERA_MAX_CPUS; c++)
            if (sched_rq_unlink_locked(c, p)) break;
    }
    p->rq_queued = 0;
    rq_unlock(fl);
}

// Place a newly-runnable process on a run queue, PRIORITY-AWARE.
//
// The first version of this chose the shallowest queue, and that was wrong in a
// way that only appears once there is more than one core: per-core queues are
// each kept sorted by effective priority, so every core is LOCALLY correct while
// the SYSTEM is globally wrong. A PRIO_REALTIME process could be queued second
// behind one entry while another core ran PRIO_LOW work, or idled. The policy
// (rustkern/schedwatch.rs) now prefers an idle core, then PREEMPTS a core
// running something the arrival outranks, and only then queues by shortest WAIT
// (entries that outrank it), not shortest queue.
// #75 (enqrace75b): WHAT IS CORE i RUNNING? The one canonical form.
//
// `smp_cpu_current(i)` with a `current_proc` fallback for the BSP existed as
// three hand-copied instances in this file (the sched_rq_push placement loop,
// the [CPUOBS] report, and now this). With AP user scheduling off, cpu0's
// per-cpu slot can be unset and current_proc is the authoritative answer for the
// BSP, so the fallback is not optional and a fourth copy that forgot it would
// silently report cpu0 as idle. Two of the three copies are replaced below.
static inline process_t *sched_core_running(uint32_t i) {
    process_t *r = (process_t *)smp_cpu_current(i);
    if (!r && i == 0) r = current_proc;
    return r;
}

// #75 (enqrace75b): IS THIS TASK EXECUTING ON A CORE RIGHT NOW?
// Returns cpu+1 if some core's current process is `p`, else 0. See the census
// block above for why no field already on the enqueue path answers this, and
// why sched_on_cpu is not that field.
static int32_t sched_running_owner(const process_t *p) {
    if (!p) return 0;
    uint32_t n = sched_rq_ncpu();
    if (n > MAYTERA_MAX_CPUS) n = MAYTERA_MAX_CPUS;
    for (uint32_t i = 0; i < n; i++)
        if ((const process_t *)sched_core_running(i) == p) return (int32_t)i + 1;
    return 0;
}

static void sched_rq_push(process_t *proc) {
    uint32_t n = sched_rq_ncpu();
    sched_core_state_t cs[MAYTERA_MAX_CPUS];
    int32_t prio = (int32_t)sched_eff_prio(proc);

    proc->next = NULL;
    proc->state = PROC_STATE_READY;
    proc->ready_since = sched_ticks;

    // #75 evidence: did this call arrive through add_to_ready_queue()?
    { uint32_t __fc = sched_rq_cpu();
      uint8_t viaf = (__fc < MAYTERA_MAX_CPUS) ? g_in_funnel[__fc] : 1;
      proc->enq_route = viaf ? 1 : 2;
      if (!viaf) {
          // #75 (enqrace75b): a side-door caller never passed the funnel, so it
          // never stamped sched_enq_ra_ok. Stamp it here or this enqueue would
          // be filed under whatever the funnel last saw, which is the exact
          // confusion this field exists to end.
          proc->sched_enq_ra_ok = __builtin_return_address(0);
          g_enq_sidedoor++;
          static int warned_side = 0;
          if (!warned_side) { warned_side = 1;
            kprintf("[ENQPROBE] SIDE DOOR: '%s' pid=%u reached sched_rq_push() "
                    "without passing add_to_ready_queue() (on_cpu=%d state=%u), "
                    "caller=0x%lx\n", proc->name, proc->pid, proc->sched_on_cpu,
                    (uint32_t)proc->state,
                    (unsigned long)(uint64_t)__builtin_return_address(0)); }
      } }

    uint64_t fl = RQ_LOCK();

    // Already linked into a queue: a second insert would splice one PCB into two
    // lists through a single `next` pointer. See process_t::rq_queued.
    if (proc->rq_queued) { rq_unlock(fl); return; }

    // #75: IS A CORE ALREADY COMMITTED TO RUNNING THIS TASK? Read under the
    // run-queue lock, which is the lock the pin is SET under, so this is
    // serialised against sched_rq_pop_locked() rather than racing it.
    // The report is emitted AFTER the unlock: a kprintf inside the run-queue
    // lock would be a long hold on the hottest lock in the scheduler, which is
    // the #67-pass-6 mistake of letting the instrument become the fault.
    {
        int32_t __pin = proc->sched_pinned;
        if (__pin != 0) {
            g_enq_pinned++;
            uint32_t __pst = (uint32_t)proc->state;
            int32_t  __poc = proc->sched_on_cpu;
            static int warned_pin = 0;
            int __report = 0;
            if (!warned_pin) { warned_pin = 1; __report = 1; }
            rq_unlock(fl);
            if (__report) {
                kprintf("[PINENQ] #75: '%s' pid=%u ENQUEUED while SELECTED by "
                        "cpu%d (this_cpu=%u state=%u on_cpu=%d rq_queued=0 "
                        "enq_ra=0x%lx). A task between pop and switch is in no "
                        "queue and must not be put back into one.\n",
                        proc->name, proc->pid, (int)(__pin - 1), sched_rq_cpu(),
                        __pst, __poc,
                        (unsigned long)(uint64_t)proc->sched_enq_ra);
            }
            if (g_enq_pin_fix) {
                // Owe it to the core that holds the pin. That core releases the
                // pin inside its own switch asm and drains on its next
                // scheduler entry, so the wake is paid, not dropped.
                //
                // This is only sound because the pin now records the core that
                // TOOK the task. Before the correction in sched_rq_pop_locked()
                // a stolen task named the victim's core here, so the debt would
                // have been filed against a core that never releases the pin and
                // may not drain for a long time - a strand, i.e. the #167 defect
                // reintroduced by a fix for #75. Noted because this patch
                // ORIGINALLY HAD THAT BUG and it was found by reading the pop,
                // not by any test: no arm distinguishes the two.
                if (sched_defer_enqueue((uint32_t)(__pin - 1), proc)) {
                    g_enq_refused++;
                    return;
                }
                // No room to defer. Falling through queues a selected task,
                // which is the very thing under test, so it is counted on the
                // SAME counter #84 reads rather than hidden.
                g_enq_allowed++;
                proc->enq_allowed_hot = 1;
            }
            fl = RQ_LOCK();
            if (proc->rq_queued) { rq_unlock(fl); return; }
        }
    }

    // #75 (enqrace75b) THE PROBE. Under g_rq_lock, on the path that is about to
    // INSERT, ask the question no guard here asks: is this task executing on a
    // core at this instant? Recorded, NOT acted on. This pass is instruments;
    // nothing is refused on the strength of a number that has not been shown to
    // mean what it is read as meaning.
    //
    // The report is emitted AFTER the unlock. A kprintf inside the run-queue
    // lock is about 87 us per character of THRE polling on the hottest lock in
    // the scheduler, which is how the #67 pass-6 instrument became the fault.
    int32_t  __ro    = sched_running_owner(proc);
    uint32_t __mecpu = sched_rq_cpu();
    int      __rslot = enq_ra_slot(proc->sched_enq_ra_ok);
    int      __rrep  = 0;
    proc->enq_running_owner = __ro;
    g_enq_ok++;
    if (__rslot >= 0) g_enqra_ok[__rslot]++; else g_enqra_over++;
    if (__ro != 0) {
        g_enq_running++;
        if ((uint32_t)(__ro - 1) == __mecpu) g_enq_running_self++;
        else                                 g_enq_running_other++;
        if (__rslot >= 0) g_enqra_run[__rslot]++;
        static int warned_run_enq = 0;
        if (!warned_run_enq) { warned_run_enq = 1; __rrep = 1; }
    }

    for (uint32_t i = 0; i < n; i++) {
        cs[i].queue_len = g_rq[i].len;
        cs[i].flags     = (g_rq_consumers & CPUMASK_BIT(i)) ? SCHED_CORE_CONSUMER : 0u;
        // How many queued entries OUTRANK the arrival: what it would actually
        // wait behind. Bounded by SCHED_RQ_SCAN_MAX like every other walk here.
        uint32_t above = 0, guard = 0;
        for (process_t *q = g_rq[i].head; q && guard++ < SCHED_RQ_SCAN_MAX; q = q->next)
            if ((int32_t)sched_eff_prio(q) >= prio) above++; else break;  // list is sorted
        cs[i].above_len = above;
        // What is running there. An idle core reports SCHED_PRIO_NONE so it
        // compares as preemptible against every real priority.
        process_t *run = sched_core_running(i);   // #75 (enqrace75b): one form
        cs[i].cur_prio = (!run || run->is_idle) ? SCHED_PRIO_NONE
                                                : (int32_t)sched_eff_prio(run);
    }

    // #83: the STICKY field. This asks "where did this task last run" about a
    // task that is being enqueued, i.e. one that is by definition not running,
    // so the live running_cpu would be -1 here every time and the hint would be
    // dead code. This line is why last_cpu has to exist separately.
    uint32_t prev_cpu = (proc->last_cpu >= 0) ? (uint32_t)proc->last_cpu : n;
    int r = sched_place_rs(cs, n, prio, prev_cpu);
    int target_was_idle = 0;
    int preempt = 0;
    uint32_t target;
    if (r < 0) {
        // No eligible core: fall back to queue 0, the BSP.
        //
        // #130 (2026-08-14): THE COMMENT THAT USED TO BE HERE WAS WRONG, AND THE
        // BUG WAS THE MISSING LINE BELOW. It read "Queue 0 is the BSP, always a
        // consumer, so falling back there can strand nothing", and on that basis
        // this branch left target_was_idle at 0. But being a scheduler CONSUMER
        // is not the same as being AWAKE: sched_schedule()'s no_ready path puts a
        // core into "sti; hlt". So a task placed here while cpu0 was halted was
        // never woken, because the wake below is gated on target_was_idle.
        //
        // MEASURED, 4-vCPU throwaway VM, repeatedly: "[SCHEDCORE] cpu0=0%/0csw/3q
        // cpu1=0%/0csw/0q cpu2=0%/0csw/0q cpu3=0%/0csw/0q consumers=0xf storms=0
        // bkl=1001/0c/0s" - three tasks queued on cpu0, ZERO context switches on
        // any core, all four cores registered as consumers, and the BKL showing
        // zero contention and zero spins. Not a deadlock: a lost wakeup.
        target = 0;
        target_was_idle = (cs[0].cur_prio == SCHED_PRIO_NONE);
    } else {
        preempt = (r & SCHED_PLACE_PREEMPT) ? 1 : 0;
        target  = (uint32_t)(r & 0xFF);
        if (target >= n) target = 0;
        target_was_idle = (cs[target].cur_prio == SCHED_PRIO_NONE);
    }
    sched_rq_insert_locked(target, proc);
    rq_unlock(fl);

    // #75 (enqrace75b): first enqueue of a task that a core was RUNNING.
    if (__rrep) {
        kprintf("[RUNENQ] #75: '%s' pid=%u ENQUEUED while cpu%d was RUNNING it "
                "(this_cpu=%u state_at_enq=%u on_cpu=%d pinned=%d rq_wanted=%u "
                "enq_ra_ok=0x%lx). sched_on_cpu is 0 for a running task, so no "
                "guard on this path refused it.\n",
                proc->name, proc->pid, (int)(__ro - 1), __mecpu,
                proc->sched_state_at_enq, proc->sched_on_cpu, proc->sched_pinned,
                (unsigned)proc->rq_wanted,
                (unsigned long)(uint64_t)proc->sched_enq_ra_ok);
    }

    // #67 pass 3: wake ONLY when it can achieve something. The first version
    // broadcast a wake IPI on every remote placement. Every IPI runs an ISR on
    // the target, and idt.c wraps every ISR in bkl_acquire(), so a needless
    // wake is a needless whole-kernel-lock acquisition on the core you just
    // interrupted - the opposite of the goal. A core that is already running
    // something will reach the scheduler on its own; only a HALTED idle core
    // needs poking.
    if (preempt) {
        sched_request_resched(target);
    } else if (n > 1 && target != sched_rq_cpu() && target_was_idle) {
        extern void smp_wake_cpu(uint32_t cpu);
        smp_wake_cpu(target);   // #67 pass 8: directed, and only if it is halted
    }
}

// Take the next process for THIS core: own queue first, then one steal.
static process_t *sched_rq_pop(void) {
    uint32_t cpu = sched_rq_cpu();
    uint32_t n = sched_rq_ncpu();
    if (cpu >= n) cpu = 0;

    uint64_t fl = RQ_LOCK();
    process_t *p = sched_rq_pop_locked(cpu);
    if (!p && n > 1) {
        sched_core_state_t cs[MAYTERA_MAX_CPUS];
        int32_t top[MAYTERA_MAX_CPUS];
        for (uint32_t i = 0; i < n; i++) {
            cs[i].queue_len = g_rq[i].len;
            cs[i].above_len = 0;
            cs[i].cur_prio  = SCHED_PRIO_NONE;
            cs[i].flags     = (g_rq_consumers & CPUMASK_BIT(i)) ? SCHED_CORE_CONSUMER : 0u;
            top[i] = g_rq[i].head ? (int32_t)sched_eff_prio(g_rq[i].head) : SCHED_PRIO_NONE;
        }
        // Take the HIGHEST-PRIORITY waiting process in the system, not one off
        // the deepest queue: this core is idle, so whatever it takes runs now.
        int victim = sched_steal_rs(cs, n, cpu, top);
        if (victim >= 0 && (uint32_t)victim < n) p = sched_rq_pop_locked((uint32_t)victim);
    }
    rq_unlock(fl);
    // #83: THE POP DELIBERATELY PUBLISHES NOTHING. This line used to be
    // `p->running_cpu = (int)cpu`, and it was the ONLY write to the field in
    // the whole kernel. Being popped off a run queue is not the same event as
    // being run: the caller may still bail out before switching (the corrupted
    // IRET-frame path in sched_schedule() does exactly that), and nothing here
    // or anywhere else ever invalidated the value afterwards, so a task carried
    // the core that last picked it up for the rest of its life. Both fields are
    // now published together by sched_publish_cpu(), at the switch.
    (void)cpu;
    return p;
}

// #67 DIAGNOSTIC. Called on every real context switch. One increment and one
// comparison on the common path; the window only closes once per second.
//
// This is the instrument the #421 record says did not exist: "it just stops",
// "heartbeat dead, no panic". A storm is now a LOUD, ADDRESSED serial line
// naming the core, the rate and the process, and under `make SCHEDSTORMPANIC=1`
// a kpanic, because a panic with state is strictly better than a hang.
// ===========================================================================
// #83: PUBLISH WHICH CORE THIS TASK IS ON. The single writer of running_cpu.
//
// Called from sched_schedule() immediately before each of the four switch
// invocations, i.e. at the last point on every path where the core is
// COMMITTED to the switch. Placing it here rather than earlier is deliberate:
// sched_schedule() has an exit between selecting `next` and switching to it
// (the corrupted-IRET-frame bail-out), and a publish before that point would
// leave `next` claiming a core it never reached.
//
// `cpu` MUST be read by the caller inside sched_schedule()'s cli() region and
// passed straight in. That is the #130 invariant: a core id is valid only while
// interrupts are masked and must never be carried across an sti. #130 was a
// hang caused by exactly that mistake one level down, in the BKL acquire, and
// the reason the parameter is not read from smp_this_cpu() in here is so the
// read and the store are visibly in the same masked region at the call site.
//
// Ordering note. prev is cleared BEFORE the switch asm has saved its context,
// so for that window prev reports -1 while it is in fact still executing.
// Conservative on purpose: this field may under-claim but must never name a
// core a task is not on. The mid-switch window is made SAFE by sched_on_cpu
// (cleared inside the switch asm), which is a different field with a different
// job, and nothing keys a correctness decision on running_cpu.
// ===========================================================================
extern void sched_cpuobs_note_rs(uint32_t pid, int32_t from_cpu, uint32_t to_cpu);

static inline void sched_publish_cpu(process_t *prev, process_t *next, uint32_t cpu) {
    if (prev) prev->running_cpu = -1;
    if (next) {
        int32_t from = (int32_t)next->last_cpu;
        next->running_cpu = (int)cpu;
        next->last_cpu    = (int)cpu;
        sched_cpuobs_note_rs(next->pid, from, cpu);
    }
}

static void sched_storm_note(uint32_t cpu, const process_t *cur, const process_t *next) {
    extern volatile uint64_t timer_ticks;
    if (cpu >= MAYTERA_MAX_CPUS) cpu = 0;
    uint64_t t = timer_ticks;
    g_sw_count[cpu]++;

    if (g_sw_win_tick[cpu] == 0) {
        g_sw_win_tick[cpu] = t;
        g_sw_win_sw[cpu]   = g_sw_count[cpu];
        return;
    }
    uint64_t dt = t - g_sw_win_tick[cpu];
    if (dt < SCHED_STORM_WINDOW_TICKS) return;

    uint64_t dsw = g_sw_count[cpu] - g_sw_win_sw[cpu];
    g_sw_win_tick[cpu] = t;
    g_sw_win_sw[cpu]   = g_sw_count[cpu];

    if (sched_storm_verdict_rs(dsw, dt, SCHED_STORM_PER_TICK) == 0) return;

    g_sched_storms++;
    if ((t - g_sw_last_report[cpu]) < SCHED_STORM_REPORT_GAP && g_sw_last_report[cpu]) return;
    g_sw_last_report[cpu] = t;

    extern int g_smp_user_sched;
    unsigned long rate = (unsigned long)(dsw / (dt ? dt : 1));
    // #67 pass 6: same reason as sched_smp_report() - a ~130 character serial
    // line is ~10 ms of polled UART writes, and this one prints from inside
    // sched_schedule(). Drop the giant lock across it.
    extern uint32_t bkl_release_all(void); extern void bkl_reacquire(uint32_t);
    extern int g_smp_bkl_full;
    uint32_t __sd = g_smp_bkl_full ? bkl_release_all() : 0;
    kprintf("[SCHEDSTORM] cpu=%u %lu switches in %lu ticks (%lu/tick, limit %u) "
            "cur=%s/%u next=%s/%u usersched=%d\n",
            cpu, (unsigned long)dsw, (unsigned long)dt, rate,
            (unsigned)SCHED_STORM_PER_TICK,
            cur ? cur->name : "?",  cur ? cur->pid : 0,
            next ? next->name : "?", next ? next->pid : 0,
            g_smp_user_sched);
    bkl_reacquire(__sd);
#ifdef SCHEDSTORMPANIC
    {
        extern void kpanic(const char *fmt, ...);
        kpanic("[SCHEDSTORM] context-switch storm on cpu %u (%lu/tick): the #421 "
               "SMP scheduler livelock signature. Panicking instead of wedging.",
               cpu, rate);
    }
#endif
}

// #67: the per-core measurement. "Exactly 50% of a 2-core VM" is one core
// saturated, and a whole-machine aggregate cannot tell that apart from two
// cores at half load - which is the ambiguity that produced this ticket. This
// prints the per-core split on the serial console so the answer is a number the
// Proxmox graph can be checked against. Called from sched_tick().
// #118: must equal BKLSITE_N in cpu/smp.c and BKLSITE_MAX in rustkern/bklsite.rs.
// A guessed constant does not fail loudly, so it is named once here and checked
// against the table's real extent by the Rust side (bklsite_top rejects n > MAX).
#define BKLSITE_REPORT_N 48u

// #169: PROOF THE AP PREEMPTION TICK IS LIVE, printed by the BSP.
//
// Reported separately from [SCHEDCORE] rather than folded into it: that line is
// already 512 bytes with a documented truncation history at 12 cores (#143), and
// a diagnostic that proves a mechanism works must not be the field that gets
// dropped. Fixed shape, greppable: [APTICK] cpu1=T/P ... where T is ticks taken
// in this window and P is how many expired a slice and switched.
//
// cpu0 is deliberately absent: the BSP does not take this tick, and printing a
// permanent 0 for it would read as "armed but never fires", which is exactly
// the failure this line exists to detect on the APs.
static void sched_aptick_report(uint64_t window) {
    static uint64_t last_t[MAYTERA_MAX_CPUS], last_p[MAYTERA_MAX_CPUS];
    uint32_t n = sched_rq_ncpu();
    if (n <= 1) return;                 // single core: nothing to say
    char line[256];
    int o = 0;
    uint64_t total = 0;
    o += snprintf(line + o, sizeof(line) - (size_t)o, "[APTICK]");
    for (uint32_t i = 1; i < n && i < MAYTERA_MAX_CPUS; i++) {
        uint64_t t = g_ap_ticks[i]    - last_t[i];  last_t[i] = g_ap_ticks[i];
        uint64_t p = g_ap_preempts[i] - last_p[i];  last_p[i] = g_ap_preempts[i];
        total += t;
        if (o >= (int)sizeof(line) - 32) continue;
        o += snprintf(line + o, sizeof(line) - (size_t)o, " cpu%u=%lu/%lu",
                      i, (unsigned long)t, (unsigned long)p);
    }
    // The window is in BSP ticks and the AP timer is armed at the SAME rate, so
    // a healthy AP reads close to the window. Saying so in the line means the
    // reader does not have to know that to judge it.
    o += snprintf(line + o, sizeof(line) - (size_t)o,
                  " (ticks/preempts per %lu-tick window)", (unsigned long)window);
    if (total == 0)
        o += snprintf(line + o, sizeof(line) - (size_t)o,
                      " *** NO AP TOOK A TICK: preemption on the APs is "
                      "COOPERATIVE ONLY (#169) ***");
    kprintf("%s\n", line);
}

static void sched_smp_report(void) {
    static uint64_t last = 0, last_busy[MAYTERA_MAX_CPUS], last_sw[MAYTERA_MAX_CPUS];
    if ((sched_ticks - last) < 1000) return;      // ~4 s at 250 Hz
    uint64_t window = sched_ticks - last;
    last = sched_ticks;

    uint32_t n = sched_rq_ncpu();
    uint64_t busy[MAYTERA_MAX_CPUS];
    uint32_t pct[MAYTERA_MAX_CPUS];
    for (uint32_t i = 0; i < n; i++) {
        busy[i] = g_core_busy[i] - last_busy[i];
        last_busy[i] = g_core_busy[i];
    }
    if (sched_core_pct_rs(busy, n, window, pct, MAYTERA_MAX_CPUS) != 0) return;
    sched_aptick_report(window);   // #169: before the early return below

    // One line, fixed shape, greppable: [SCHEDCORE] cpu0=NN%/SW ...
    //
    // #143: THIS BUFFER SILENTLY TRUNCATED. At 192 bytes it held about ten
    // per-core fields, which was invisible while the cap was 8 and became
    // reachable the moment #143 raised it to 32. MEASURED on a 12-vCPU boot
    // before this fix: the line stopped after cpu9 with no indication, so two
    // real cores were missing from the one diagnostic that exists to show
    // per-core behaviour, and nothing said so.
    //
    // Two changes, and the second matters more than the first. The buffer is
    // bigger, AND the truncation is now VISIBLE: a report that quietly stops
    // early cannot be told apart from a machine that has fewer cores, which is
    // precisely the silent-guard trap this project keeps paying for. The loop
    // still refuses to overrun; it just says so afterwards.
    char line[512];
    int o = 0;
    uint32_t shown = 0;
    o += snprintf(line + o, sizeof(line) - (size_t)o, "[SCHEDCORE]");
    for (uint32_t i = 0; i < n; i++) {
        uint64_t sw = g_sw_count[i] - last_sw[i];
        last_sw[i] = g_sw_count[i];
        // Every core's counter is consumed even if its field does not fit, so a
        // truncated line never corrupts the NEXT window's deltas.
        if (o >= (int)sizeof(line) - 40) continue;
        o += snprintf(line + o, sizeof(line) - (size_t)o, " cpu%u=%u%%/%lucsw/%uq",
                       i, pct[i], (unsigned long)sw, g_rq[i].len);
        shown++;
    }
    if (shown < n && o < (int)sizeof(line) - 24)
        o += snprintf(line + o, sizeof(line) - (size_t)o, " +%u-more", n - shown);
    // #67 pass 3: BKL contention per window. "contended" is how many kernel
    // entries had to WAIT for the whole-kernel lock; "spins" is how many pause()
    // iterations were burned doing so. If those are large next to the switch
    // counts, the BKL is the ceiling and narrowing it is part of this task.
    //
    // #166 THIS LOOP WAS THE BUG. It read:
    //
    //     for (uint32_t i = 0; i < n && i < MAYTERA_MAX_CPUS; i++) {
    //         acq += g_bkl_acq_pc[i]; ... g_bkl_hold_max[i] = 0;
    //
    // with `n = sched_rq_ncpu()` (bounded by MAYTERA_MAX_CPUS = 32) over arrays
    // that cpu/smp.c sized with its own BKL_STAT_CPUS = 8. On a 12-vCPU boot
    // that is a four-element out-of-bounds READ of six counter arrays and an
    // out-of-bounds WRITE of zero into a seventh. See cpu/smp.c's BKL_STAT_CPUS
    // comment for the measured .bss aliasing and for the exact arithmetic that
    // turns it into `held=18446743107341936826us`.
    //
    // The summation is now in the file that DECLARES the arrays and is bounded
    // by sizeof() of one of them; this function no longer supplies a CPU count
    // at all, because a caller that cannot state a count cannot state a wrong
    // one. The window arithmetic and the invariants are in
    // rustkern/bklstat.rs.
    bkl_totals_t bt;
    bkl_stat_totals(&bt);
    static uint64_t lb_bkl_us;
    uint64_t now_bkl_us = mono_us();
    // 0 on the first window means "unknown length"; bklstat.rs withholds the
    // duration-based verdicts rather than judging against a made-up number.
    uint64_t d_bkl_us = lb_bkl_us ? (now_bkl_us - lb_bkl_us) : 0;
    lb_bkl_us = now_bkl_us;
    bkl_window_t bw;
    { extern int g_smp_bkl_full;
      (void)bkl_window_rs(&bt, d_bkl_us, n, g_smp_bkl_full, &bw); }
    uint64_t d_acq  = bw.acquires,   d_con  = bw.contended,
             d_spin = bw.spins,      d_hsum = bw.held_us,
             d_long = bw.long_holds, hmax   = bw.max_us;
    uint32_t hres = bw.max_reason;
    // held=<total us the lock was held this window> maxhold=<longest single
    // hold>@<reason>. Reason 0x1NN = interrupt vector NN, 0x2NN = syscall NN.
    // #67 pass 6: DROP THE GIANT LOCK ACROSS THE PRINT.
    //
    // kprintf() goes to the serial console, and serial_write() POLLS the UART's
    // THRE bit for every single character. At 115200 baud that is about 87 us
    // per character, so this ~120-character line is about 10 MILLISECONDS of
    // busy-polling. sched_smp_report() is called from sched_tick(), which runs
    // inside the timer ISR, which idt.c wraps in bkl_acquire(). So the
    // diagnostic built to find out who holds the Big Kernel Lock too long was
    // itself holding it for ten milliseconds at a time - the third instrument in
    // this ticket to become the thing it was measuring.
    //
    // MEASURED before this change: "maxhold=1775us@0x120", where 0x120 is
    // interrupt vector 0x20, the timer. On the single-core path this costs
    // nothing measurable, because no other core is waiting; with a second core
    // scheduling it is most of why the gate-ON path crawled.
    //
    // The report reads only its own statics and the per-cpu counters, so nothing
    // it touches needs the giant lock. This is the same release/reacquire the
    // scheduler already performs around a context switch.
    { extern uint32_t bkl_release_all(void); extern void bkl_reacquire(uint32_t);
      extern int g_smp_bkl_full;
      uint32_t __d = g_smp_bkl_full ? bkl_release_all() : 0;
      // maxhold now names the CORE and whether the hold began on the far side
      // of a context switch (sw=1), which is the distinction the previous
      // instrument could not make and which sent pass 8 after a phantom.
      // #75: PIN OBSERVABILITY. g_pin_waits existed but was never printed, and the
    // only string containing "pin wait" was the TIMEOUT message - so a shell
    // check for it reported "0" for a healthy run and I read that as "the wait
    // never ran". A pin never observed HELD is indistinguishable from one never
    // TAKEN, which is the silent-guard trap this ticket has now hit four times.
    // No version of the fix may be judged until these are non-zero.
    extern volatile uint64_t g_pin_waits, g_pin_timeouts;
    static uint64_t lb_pw, lb_pt;
    uint64_t d_pw = g_pin_waits - lb_pw, d_pt = g_pin_timeouts - lb_pt;
    lb_pw = g_pin_waits; lb_pt = g_pin_timeouts;
    // #166: cpu%u is now bw.max_cpu, the ARRAY INDEX that recorded this
    // window's longest hold, not the old g_bkl_hold_cpu global. That global has
    // one writer and no invalidator, so it named whichever core last set a new
    // maximum at any point since boot - the exact defect #83 found in
    // running_cpu. Same for sw%u.
    kprintf("%s consumers=0x%llx storms=%lu bkl=%lu/%luc/%lus rec=%lu held=%luus "
              "maxhold=%luus@0x%x/cpu%u/sw%u long=%lu pin=%lu/%luto bklcpus=%u\n",
              line, (unsigned long long)g_rq_consumers,
              (unsigned long)g_sched_storms, (unsigned long)d_acq,
              (unsigned long)d_con, (unsigned long)d_spin,
              (unsigned long)bw.recursive,
              (unsigned long)d_hsum, (unsigned long)hmax, hres,
              bw.max_cpu, bw.max_from_switch, (unsigned long)d_long,
              (unsigned long)d_pw, (unsigned long)d_pt, bw.ncpu);
    // #166: THE VERDICT, ON ITS OWN LINE, WITH THE RAW NUMBERS BESIDE IT.
    //
    // Not a clamp. A clamped display of a broken counter is strictly worse than
    // an obviously broken one, because it looks trustworthy and the next person
    // sizes real work from it. If an invariant fails, the numbers above are
    // printed exactly as computed and THIS line says which invariant and how
    // many windows have failed since boot.
    //
    // It is printed on EVERY window, not only on failure. A check that only
    // speaks when it has something to say cannot be told apart from one that is
    // not running - the silent-guard trap this project has paid for repeatedly
    // (see the pin= counters above, and #69/#83/#167).
    { uint64_t nbad = bkl_window_bad_rs();
      if (bw.flags)
        kprintf("[BKLSTAT] BROKEN flags=0x%x badwindows=%lu (numbers above are "
                "RAW and NOT clamped; see rustkern/bklstat.rs) ncpu=%u win=%luus\n",
                bw.flags, (unsigned long)nbad, bw.ncpu, (unsigned long)d_bkl_us);
      else
        kprintf("[BKLSTAT] ok flags=0 badwindows=%lu ncpu=%u win=%luus\n",
                (unsigned long)nbad, bw.ncpu, (unsigned long)d_bkl_us); }

    // #75 (enqrace75b) THE ENQUEUE CENSUS. Per call site: funnel CALLS /
    // enqueues that reached a run queue / how many of those enqueued a task a
    // core was RUNNING at that instant.
    //
    // Printed on EVERY window and not only when a number is non-zero. A probe
    // that only speaks when it has something to say cannot be told apart from a
    // probe that is not running - the trap this file already records against the
    // pin= counters, against #167 and against #69/#83.
    { char l2[768]; int p2 = 0;
      p2 += snprintf(l2 + p2, sizeof(l2) - (size_t)p2,
                     "[ENQCENSUS] enq=%lu run=%lu(self=%lu other=%lu) "
                     "pop_run=%lu(other=%lu)/%lu popingap=%lu gaps=%lu "
                     "refused=%lu allowed=%lu sidedoor=%lu pinenq=%lu "
                     "wakecand=%lu/%lu(busy/free) over=%lu |",
                     (unsigned long)g_enq_ok, (unsigned long)g_enq_running,
                     (unsigned long)g_enq_running_self,
                     (unsigned long)g_enq_running_other,
                     (unsigned long)g_pop_running,
                     (unsigned long)g_pop_running_other,
                     (unsigned long)g_pop_total,
                     (unsigned long)g_pop_in_gap,
                     (unsigned long)g_gap_opened,
                     (unsigned long)g_enq_refused, (unsigned long)g_enq_allowed,
                     (unsigned long)g_enq_sidedoor, (unsigned long)g_enq_pinned,
                     (unsigned long)g_wake_cand_busy,
                     (unsigned long)g_wake_cand_free,
                     (unsigned long)g_enqra_over);
      for (int i = 0; i < ENQ_RA_SLOTS; i++) {
          if (!g_enqra_addr[i]) continue;
          if (p2 >= (int)sizeof(l2) - 48) {
              p2 += snprintf(l2 + p2, sizeof(l2) - (size_t)p2, " +more");
              break;
          }
          p2 += snprintf(l2 + p2, sizeof(l2) - (size_t)p2, " 0x%lx:%lu/%lu/%lu",
                         (unsigned long)(uint64_t)g_enqra_addr[i],
                         (unsigned long)g_enqra_call[i],
                         (unsigned long)g_enqra_ok[i],
                         (unsigned long)g_enqra_run[i]);
      }
      kprintf("%s (site:calls/enq/enq-of-running)\n", l2); }

    // #75 (enqrace75b): the first cross-core pop of a task another core was
    // RUNNING, printed here because the pop itself holds g_rq_lock.
    if (g_poprun_pending) {
        g_poprun_pending = 0;
        g_poprun_reported = 1;
        kprintf("[RUNPOP] #75: cpu%u POPPED pid=%u which cpu%u was RUNNING "
                "(state_at_pop=%u enq_ra_ok=0x%lx). Two cores now hold one task "
                "and one kernel stack.\n",
                g_poprun_cpu, g_poprun_pid, g_poprun_owner, g_poprun_state,
                (unsigned long)(uint64_t)g_poprun_ra);
    }
    // #143 part 2: THE RUN-QUEUE LOCK, on its own line and in the same shape as
    // the BKL line above, so the two are directly comparable. Printed
    // unconditionally rather than only when it looks bad: a contention
    // instrument that only speaks up when it has something to say cannot be
    // told apart from one that is not running, which is the silent-guard trap
    // this project has hit repeatedly (see the pin= counters above).
    { extern int rqlock_verdict_rs(uint64_t, uint64_t, uint64_t, uint64_t);
      extern uint32_t rqlock_contended_pct_rs(uint64_t, uint64_t);
      extern uint32_t rqlock_held_pct_rs(uint64_t, uint64_t);
      static uint64_t lr_acq, lr_con, lr_held, lr_us;
      uint64_t now_us = mono_us();
      uint64_t d_racq = g_rq_acct.acquires  - lr_acq;
      uint64_t d_rcon = g_rq_acct.contended - lr_con;
      uint64_t d_rheld = g_rq_held_us       - lr_held;
      uint64_t d_rus  = now_us - lr_us;
      lr_acq = g_rq_acct.acquires; lr_con = g_rq_acct.contended;
      lr_held = g_rq_held_us;      lr_us = now_us;
      uint64_t rmax = g_rq_held_max; int rln = g_rq_held_max_ln;
      int rcpu = g_rq_held_max_cpu;
      g_rq_held_max = 0;   // per-window maximum, like the BKL one
      kprintf("[RQLOCK] acq=%lu con=%lu(%u%%) held=%luus(%u%% of %luus) "
              "maxhold=%luus@process.c:%d/cpu%d verdict=%d queues=%u\n",
              (unsigned long)d_racq, (unsigned long)d_rcon,
              rqlock_contended_pct_rs(d_racq, d_rcon),
              (unsigned long)d_rheld, rqlock_held_pct_rs(d_rheld, d_rus),
              (unsigned long)d_rus, (unsigned long)rmax, rln, rcpu,
              rqlock_verdict_rs(d_racq, d_rcon, d_rheld, d_rus), n); }

    // #83 EVIDENCE. Two independent statements, because they fail differently.
    //
    // live: what each core's CURRENTLY RUNNING task says its own running_cpu
    // is, sampled across all cores at one instant. live2=1 means two cores
    // disagreed in that single sample, which a field stuck at a constant can
    // never produce however long you watch it. This is the strong form.
    //
    // distinct/mig accumulate over the boot: how many different cores have ever
    // been published, and how many switch-ins moved a task from one core to
    // another. drops must be 0; non-zero means a core id was out of range and
    // the other counts are LOW.
    { extern uint32_t sched_cpuobs_distinct_rs(void);
      extern uint64_t sched_cpuobs_migrations_rs(void);
      extern uint64_t sched_cpuobs_switchins_rs(void);
      extern uint64_t sched_cpuobs_drops_rs(void);
      extern void sched_cpuobs_last_mig_rs(uint32_t *, int32_t *, int32_t *);
      extern int sched_cpuobs_live_verdict_rs(const int32_t *, uint32_t);
      int32_t live[MAYTERA_MAX_CPUS];
      char lb[224]; int lo = 0;
      lo += snprintf(lb + lo, sizeof(lb) - (size_t)lo, "[CPUOBS] live:");
      uint32_t ln = (n < MAYTERA_MAX_CPUS) ? n : MAYTERA_MAX_CPUS;
      for (uint32_t i = 0; i < ln; i++) {
          process_t *r = sched_core_running(i);   // #75 (enqrace75b): one form
          live[i] = r ? (int32_t)r->running_cpu : -1;
          if (lo < (int)sizeof(lb) - 40)
              lo += snprintf(lb + lo, sizeof(lb) - (size_t)lo, " cpu%u=%d/%s",
                             i, live[i], r ? r->name : "-");
      }
      uint32_t mpid = 0; int32_t mfrom = -1, mto = -1;
      sched_cpuobs_last_mig_rs(&mpid, &mfrom, &mto);
      kprintf("%s live2=%d distinct=%u mig=%lu in=%lu drops=%lu lastmig=pid%u:%d->%d\n",
              lb, sched_cpuobs_live_verdict_rs(live, ln),
              sched_cpuobs_distinct_rs(),
              (unsigned long)sched_cpuobs_migrations_rs(),
              (unsigned long)sched_cpuobs_switchins_rs(),
              (unsigned long)sched_cpuobs_drops_rs(),
              mpid, mfrom, mto); }

    // #118: WHO actually held the lock, ranked two ways.
    //
    // The @0xNNN tag on the line above names the last interrupt to fire during
    // the hold, NOT the holder (see rustkern/bklsite.rs). These return
    // addresses do name the holder: addr2line them against THIS build's
    // kernel.elf. gap= is the worst interval between interrupt entries during
    // that site's worst hold; near the 4 ms tick period means the core was
    // genuinely executing throughout, near the hold length means it was not
    // running at all.
    { typedef struct { uint64_t ra, count, total_us, max_us, max_gap_us, worst_syscall; } bklsite_t;
      extern bklsite_t g_bkl_sites[]; extern volatile uint64_t g_bklsite_drops;
      extern uint32_t bklsite_top(const bklsite_t *, uint32_t, int, uint32_t *, uint32_t);
      uint32_t idx[3], k;
      // There is deliberately no single "[BKLMAX] ra=..." line. The first pass
      // had one, reading a global recorded at max-hold time, and it was WRONG in
      // a way worth writing down: sched_smp_report() zeroes g_bkl_hold_max[] and
      // then calls bkl_release_all() to drop the lock across its own print, and
      // that release is itself accounted - against a now-zero maximum, so it
      // always won and always overwrote the global with the report's own call
      // site. It printed isr_handler, gap=0, every single time, which looks like
      // a finding rather than like an instrument eating itself. The per-site
      // table below cannot have that bug: each site keeps its own maximum.
      k = bklsite_top(g_bkl_sites, BKLSITE_REPORT_N, 0, idx, 3);
      for (uint32_t i = 0; i < k; i++) { bklsite_t *e = &g_bkl_sites[idx[i]];
        if (!e->total_us) break;
        kprintf("[BKLSITE-TOTAL] #%u ra=0x%lx n=%lu total=%luus max=%luus gap=%luus sc=%lu\n",
                i, (unsigned long)e->ra, (unsigned long)e->count,
                (unsigned long)e->total_us, (unsigned long)e->max_us,
                (unsigned long)e->max_gap_us, (unsigned long)e->worst_syscall); }
      k = bklsite_top(g_bkl_sites, BKLSITE_REPORT_N, 1, idx, 3);
      for (uint32_t i = 0; i < k; i++) { bklsite_t *e = &g_bkl_sites[idx[i]];
        if (!e->max_us) break;
        kprintf("[BKLSITE-MAX] #%u ra=0x%lx n=%lu total=%luus max=%luus gap=%luus sc=%lu\n",
                i, (unsigned long)e->ra, (unsigned long)e->count,
                (unsigned long)e->total_us, (unsigned long)e->max_us,
                (unsigned long)e->max_gap_us, (unsigned long)e->worst_syscall); }
      if (g_bklsite_drops)
        kprintf("[BKLSITE] TABLE FULL, dropped=%lu samples (totals are LOW)\n",
                (unsigned long)g_bklsite_drops);

    // #143 re-measure: THE SAME HOLD TIME, RANKED BY THE PROCESS THAT HELD IT.
    //
    // WHY THIS LINE EXISTS. [BKLSITE-TOTAL] above answers "which call site" with
    // 99% of all hold time at ONE address (proc/process.c:4220, the scheduler's
    // bkl_reacquire() after context_switch()). That is not a bug in the table:
    // proc_wrapper() takes the BKL for a kernel thread's entire life, so the
    // retake after a switch genuinely does cover the resumed thread's whole
    // residency. It just means the call site cannot name a holder, for ANY
    // workload, and #118's rule is to name the holder rather than report an
    // anonymous duration. The process can.
    //
    // Same table type, same Rust ranking, keyed on pid+1. Cumulative since boot,
    // like [BKLSITE-TOTAL], so a late reader gets the whole run and not a window.
    { extern bklsite_t g_bkl_pids[]; extern volatile uint64_t g_bklpid_drops;
      extern const char *sched_pid_name_for_report(uint32_t);
      k = bklsite_top(g_bkl_pids, BKLSITE_REPORT_N, 0, idx, 3);
      for (uint32_t i = 0; i < k; i++) { bklsite_t *e = &g_bkl_pids[idx[i]];
        if (!e->total_us) break;
        uint32_t pid = (uint32_t)(e->ra - 1);       // key was pid + 1
        kprintf("[BKLPID-TOTAL] #%u pid=%u name=%s n=%lu total=%luus max=%luus gap=%luus sc=%lu\n",
                i, pid, sched_pid_name_for_report(pid), (unsigned long)e->count,
                (unsigned long)e->total_us, (unsigned long)e->max_us,
                (unsigned long)e->max_gap_us, (unsigned long)e->worst_syscall); }
      if (g_bklpid_drops)
        kprintf("[BKLPID] TABLE FULL, dropped=%lu samples (totals are LOW)\n",
                (unsigned long)g_bklpid_drops); } }

    // #121: WHICH SYSCALL, and what it was doing. The return addresses above
    // name the acquire in proc/syscall.asm, which every syscall in the kernel
    // shares; these name the syscall number and split its worst call across
    // named phases. Printed here, inside the same released-BKL window, so the
    // report cannot become the hold it is reporting (that is #67 pass 6, and
    // it has already happened once in this ticket family).
    { extern void scp_report(void); scp_report(); }

    // #75 evidence 2: refusal / allowed / side-door totals.
    { extern volatile uint64_t g_enq_refused, g_enq_allowed, g_enq_sidedoor;
      extern volatile uint64_t g_enq_pinned;   // #75
      extern int g_enq_pin_fix, g_enq_cpu_fix;
      static uint64_t lr, la, ls, lp;
      kprintf("[ENQ] refused=%lu allowed=%lu sidedoor=%lu pinned=%lu "
              "(window +%lu/+%lu/+%lu/+%lu) pinfix=%d cpufix=%d\n",
              (unsigned long)g_enq_refused, (unsigned long)g_enq_allowed,
              (unsigned long)g_enq_sidedoor, (unsigned long)g_enq_pinned,
              (unsigned long)(g_enq_refused - lr), (unsigned long)(g_enq_allowed - la),
              (unsigned long)(g_enq_sidedoor - ls), (unsigned long)(g_enq_pinned - lp),
              g_enq_pin_fix, g_enq_cpu_fix);
      lr = g_enq_refused; la = g_enq_allowed; ls = g_enq_sidedoor; lp = g_enq_pinned; }
    // #75: halts taken while OWNING the BKL, per site, against halts taken
    // correctly. A non-zero left-hand number is the wedge mechanism firing.
    { extern volatile uint64_t g_haltbkl[4], g_haltbkl_ok[4];
      kprintf("[HALTBKL] owned/total ap=%lu/%lu noidle=%lu/%lu nocur=%lu/%lu "
              "bsp=%lu/%lu\n",
              (unsigned long)g_haltbkl[0], (unsigned long)(g_haltbkl[0] + g_haltbkl_ok[0]),
              (unsigned long)g_haltbkl[1], (unsigned long)(g_haltbkl[1] + g_haltbkl_ok[1]),
              (unsigned long)g_haltbkl[2], (unsigned long)(g_haltbkl[2] + g_haltbkl_ok[2]),
              (unsigned long)g_haltbkl[3], (unsigned long)(g_haltbkl[3] + g_haltbkl_ok[3])); }
      bkl_reacquire(__d); }
}

// ===========================================================================
// #745 (#75) HALTING WHILE OWNING THE BIG KERNEL LOCK.
//
// The measured wedge is one core halted with the BKL held while the other spins
// in bkl_take_locked(). Nothing can then become runnable, so the halted core is
// never woken and the machine is dead with both run queues empty.
//
// Every site in this kernel that puts a core to sleep is checked here, and each
// one COUNTS separately, so "which halt" is a measurement rather than an
// inference. The first few occurrences also print the return address of every
// outstanding acquire, which is what names the unbalanced one.
// ===========================================================================
volatile uint64_t g_haltbkl[4];        // owned the BKL at this halt site
volatile uint64_t g_haltbkl_ok[4];     // reached this halt site not owning it

static const char *haltbkl_site(uint32_t s) {
    switch (s) {
        case 0: return "sched_ap_enter idle loop";
        case 1: return "sched_schedule no-idle-process halt";
        case 2: return "sched_schedule next==cur no_ready halt";
        default: return "idle_process (BSP)";
    }
}

// may_print: 0 at sites reached with IF=0, where a kprintf would only queue.
static void sched_halt_bkl_note(uint32_t site, int may_print) {
    void *ra[4] = {0,0,0,0}; uint8_t via[4] = {0,0,0,0};
    uint32_t d = bkl_self_forensics(ra, via, 4);
    if (site > 3) return;
    if (!d) { g_haltbkl_ok[site]++; return; }
    g_haltbkl[site]++;
    static uint32_t printed[4];
    if (may_print && printed[site] < 3) {
        printed[site]++;
        kprintf("[HALTBKL] cpu %u is about to HALT at %s while OWNING the BKL: "
                "depth=%u ra0=0x%lx/v%u ra1=0x%lx/v%u ra2=0x%lx/v%u "
                "(v1=bkl_acquire v2=bkl_reacquire)\n",
                smp_this_cpu(), haltbkl_site(site), d,
                (unsigned long)ra[0], via[0], (unsigned long)ra[1], via[1],
                (unsigned long)ra[2], via[2]);
    }
}

// ===========================================================================
// #67 pass 2: MAKE AN APPLICATION PROCESSOR A REAL SCHEDULER CONSUMER.
// ===========================================================================
//
// Until this existed, turning the gate on started the APs but left them running
// only the smp_work_* job ring. Anything the placement policy put on an AP's run
// queue was never popped: MEASURED as "[SCHEDCORE] cpu1=0%/0csw/1q" frozen for a
// whole boot, with COMPOSITOR_UP and no DESKTOP_READY, no panic and no error.
//
// The core becomes an ordinary scheduler participant by running an idle process
// of its own, exactly as the BSP does. Two details make that safe here:
//
//  * NO SYNTHETIC CONTEXT FRAME IS BUILT. The AP enters the scheduler by
//    CALLING it from its own boot stack, so the first context_switch away saves
//    a perfectly ordinary C call frame into idle->rsp, and the switch back
//    returns here. Hand-built frames are what #446/#588 kept getting wrong.
//  * THE IDLE PROCESS IS NEVER QUEUED. sched_rq_push() refuses idle processes
//    via is_idle, and the empty-queue fallback reaches it directly, so it can
//    never be stolen by another core.
//
// Does not return.
void sched_ap_enter(uint32_t cpu) {
    // #75 part 4: refuse until the process subsystem exists. proc_init() wipes
    // the whole process table and resets next_pid, so anything claimed before
    // it runs is silently un-claimed. See g_proc_init_done.
    if (!g_proc_init_done) {
        static int warned_early = 0;
        if (!warned_early) {
            warned_early = 1;
            kprintf("[SCHED] cpu %u reached the scheduler before proc_init(); "
                    "running kernel jobs only until the process table exists "
                    "(#75)\n", cpu);
        }
        return;
    }
    if (cpu == 0 || cpu >= MAYTERA_MAX_CPUS) {
        kprintf("[SCHED] AP enter refused for cpu %u (out of range)\n", cpu);
        return;
    }
    // #75: USE THE SHARED ALLOCATOR. This used to be its own unlocked linear
    // scan for a PROC_STATE_UNUSED slot, racing proc_create() doing the same
    // scan on the other core, with nothing reserving the slot in between. That
    // handed one process_t to two threads and is the root cause the reproducer
    // found. Reaching around a shared primitive to do its job by hand is this
    // project's most-repeated failure mode - the same shape as the duplicated
    // bkl_acquire/bkl_reacquire whose copy lost its wait loop - and here it
    // cost a PCB.
    process_t *idle = alloc_idle_slot();
    if (!idle) {
        kprintf("[SCHED] cpu %u: no free slot for an idle process; this core "
                "will NOT schedule (running kernel jobs only)\n", cpu);
        return;
    }
    char nm[16]; snprintf(nm, sizeof(nm), "idle%u", cpu);
    init_proc(idle, nm, PRIO_IDLE);
    idle->is_idle     = 1;   // #75: init_proc's memset cleared it; restore now
    idle->privilege   = PRIV_KERNEL;
    idle->state       = PROC_STATE_RUNNING;
    idle->running_cpu = (int)cpu;   // #83: live - it is running, here, now
    idle->last_cpu    = (int)cpu;   // #83: sticky
    idle->time_slice  = TIME_SLICE_TICKS;
    // The core's boot stack IS this process's kernel stack: we are standing on
    // it. Recorded so proc_check_switch_target() and the TSS logic see a sane
    // range rather than zeroes.
    extern per_cpu_t *smp_get_current_cpu(void);
    per_cpu_t *me = smp_get_current_cpu();
    { uint64_t top = me ? me->stack_top : 0;
      if (top) { idle->stack_base = (void *)(top - PROCESS_STACK_SIZE);
                 idle->stack_size = PROCESS_STACK_SIZE;
                 // #130: TAG IT. Every other stack-owning path calls
                 // proc_stack_tag() (process.c:2411, 2521, 3971, 4242, 4415);
                 // this one never did, so the AP idle threads' kernel stacks
                 // carried tag=0 owner=0 forever and proc_check_switch_target()
                 // reported "SHARED STACK or deep overflow" on EVERY switch to
                 // idle1/idle2/idle3.
                 //
                 // MEASURED on a 4-vCPU boot: 34 + 28 + 18 = 80 such reports,
                 // against a SCHEDBUG_MAX budget of 40. The false positives
                 // EXHAUSTED the detector, so a genuine shared-stack report
                 // could never be printed. This is a diagnostic-correctness
                 // fix: it removes the noise so the detector can speak. It is
                 // NOT itself a fix for the SMP corruption.
                 //
                 // SAFE: SMP_STACK_SIZE == PROCESS_STACK_SIZE == 16KB (smp.h:24,
                 // process.h:25), so (top - PROCESS_STACK_SIZE) is exactly this
                 // core's own stack_base, and the stack grows DOWN from top, so
                 // the low 16 bytes are the last memory the core would reach.
                 proc_stack_tag(idle); } }

    g_cpu_idle[cpu] = idle;
    smp_set_current(idle);
    sched_rq_set_consumer(cpu, 1);   // ONLY now may work be placed here
    kprintf("[SCHED] cpu %u is now a scheduler consumer (idle pid %u), "
            "consumers=0x%llx\n", cpu, idle->pid, (unsigned long long)g_rq_consumers);
    aptick_selftest();   // #169: once, on the first AP to get here

    // The idle loop.
    //
    // #67 pass 2, CORRECTED AFTER MEASUREMENT: this loop originally just did
    // sti+hlt and relied on sched_tick() to preempt the core into work, the way
    // the BSP's idle_process() does. THAT DOES NOT WORK ON AN AP: the periodic
    // tick is the legacy PIT on IRQ0, delivered to the BSP ONLY. The AP halted
    // forever with no tick and never entered the scheduler at all. MEASURED on
    // build 248: "[SCHEDCORE] cpu1=0%/0csw/4q" held for the whole AssaultCube
    // burn - work was correctly PLACED on this core and never RUN. The system
    // did not wedge, because cpu0 steals cpu1's queue whenever cpu0 goes idle;
    // during a sustained burn cpu0 never does, so the queue just sat there.
    //
    // So the idle process CALLS the scheduler instead of waiting to be
    // interrupted into it. sched_schedule() halts this core itself when the
    // queue is empty (its no_ready path ends in "sti; hlt"), so this is a HALT
    // loop and not a spin: the core burns no host CPU while idle, and the wake
    // IPI from sched_rq_push() brings it straight back round.
    //
    // #169, DONE: this core now takes its OWN preemption tick. tick_ap_arm()
    // (cpu/isr.c) arms this AP's LAPIC timer on vector 0x42 in ap_entry(), and
    // sched_tick_ap() decrements the running process's slice and preempts it.
    // The note that used to be here said preemption on an AP was COOPERATIVE
    // and that a CPU-bound process would hold the core until it finished. That
    // was correct and it was MEASURED at 837x between two identical Ring-3
    // workers (#168 Job 1). It also warned that the fix must not double-count
    // timer_ticks; it does not - sched_tick_ap() touches no global tick state
    // at all, which is the whole reason it is a separate function.
    while (me && !me->should_halt) {
        // Keep the #279 role: drain kernel jobs first, so making this core a
        // scheduler does not cost the parallel work ring.
        { extern int smp_work_run_one(void); if (smp_work_run_one()) continue; }
        // #67 pass 4: only enter the scheduler when there is plausibly work.
        // See sched_rq_has_work() for why an unlocked hint is sound here.
        if (sched_rq_has_work(cpu)) sched_schedule();
        // #75: about to halt. Checked HERE, with IF still set, so the report can
        // reach the wire; nothing between this point and the hlt below takes the
        // BKL, and the count at the hlt itself (site 0, silent) confirms it.
        sched_halt_bkl_note(0, 1);
        // #67 pass 11: HALT WITHOUT LOSING A WAKE.
        //
        // This used to be a bare "sti; hlt". sti+hlt is atomic, so a wake IPI
        // that arrives at the sti is taken before the halt - but the HANDLER
        // returns to the hlt, which then executes and sleeps until the NEXT
        // interrupt. So a wake that lands between the work check above and this
        // instruction is CONSUMED BY THE HANDLER AND LOST, and this core sleeps
        // with work sitting in its run queue.
        //
        // MEASURED on build 254, on an otherwise quiet host so the noise was
        // gone: "[SCHEDCORE] cpu0=100%/2000csw/0q cpu1=0%/0csw/3q" held for the
        // rest of the run. Three processes queued on cpu1, cpu1 idle, zero
        // context switches - the stranding shape again, from a different cause.
        //
        // The fix is the standard sequence, the same one cpu/smp.c's original AP
        // work loop already used and which I did not carry over: mask, RE-CHECK
        // under the mask, and only halt if there is still nothing. A wake that
        // arrives before the cli is caught by the re-check; one that arrives
        // after is delivered by the sti and taken after the hlt.
        __asm__ volatile("cli");
        if (!sched_rq_has_work(cpu) && !me->should_halt) {
            __asm__ volatile("sti; hlt");
        } else {
            __asm__ volatile("sti");
        }
    }
    kprintf("[SCHED] cpu %u leaving the scheduler (halt requested)\n", cpu);
    sched_rq_set_consumer(cpu, 0);
    g_cpu_idle[cpu] = NULL;
}

// Boot-time proof that the policy in rustkern/schedwatch.rs still holds. A
// scheduler policy regression otherwise shows up as a mis-scheduled process
// three subsystems away, which is unattributable.
void sched_smp_selftest(void) {
    uint32_t fail = sched_watch_selftest_rs();
    if (fail) {
        kprintf("[SCHEDWATCH] SELFTEST FAILED, mask=0x%x - scheduler policy is "
                "WRONG; do not trust g_smp_user_sched=1 on this build\n", fail);
    } else {
        kprintf("[SCHEDWATCH] selftest OK (storm verdict, placement, steal, "
                "per-core percent)\n");
    }

    // #83: the same proof for the running_cpu verdicts. These are pure
    // functions, so they can be PROVEN on this exact build rather than argued
    // about, and the cases that matter are the ones that would let a BROKEN
    // field look proven: an all-equal snapshot (exactly what the pre-#83
    // constant produced) must NOT read as two cores disagreeing.
    { extern int sched_cpuobs_selftest_rs(void);
      int cf = sched_cpuobs_selftest_rs();
      if (cf) kprintf("[CPUOBS] SELFTEST FAILED at check %d - the running_cpu "
                      "evidence line cannot be trusted on this build\n", cf);
      else    kprintf("[CPUOBS] selftest OK (live-disagreement verdict: "
                      "all-equal, mixed, idle-skip, degenerate)\n"); }

    // #143 part 2: the same proof for the run-queue lock verdict. This decides
    // whether the [RQLOCK] line below is worth reading at all, and every one of
    // its cases asserts a SPECIFIC classification, including the ones that must
    // NOT fire: a threshold that reports HOT for everything would make the
    // whole measurement worthless while looking like it was working.
    { extern uint32_t rqlock_selftest_rs(void);
      uint32_t rf = rqlock_selftest_rs();
      if (rf) kprintf("[RQLOCK] SELFTEST FAILED at check %u - the contention "
                      "verdict on this build is WRONG; do not size the #143 "
                      "lock work from its numbers\n", rf);
      else    kprintf("[RQLOCK] selftest OK (idle, busy-uncontended, hot, "
                      "below-threshold, saturated, percentages, corrupt-pair)\n"); }
    // #166: the same proof for the BKL statistics. Every case asserts a
    // SPECIFIC outcome, including the ones that must NOT fire and including
    // "nothing moved", because a counter stuck at zero satisfies every other
    // invariant in that file and would otherwise read as healthy.
    { uint32_t bf = bkl_stat_selftest_rs();
      if (bf) kprintf("[BKLSTAT] SELFTEST FAILED at check %u - the BKL window "
                      "arithmetic on this build is WRONG; do not size any BKL "
                      "narrowing from its numbers (#166)\n", bf);
      else    kprintf("[BKLSTAT] selftest OK (healthy, held-underflow, "
                      "con>acq, maxhold>window, held>capacity, at-capacity-ok, "
                      "long>acq, stalled, disarmed, first-window, spin-underflow, "
                      "unknown-window)\n"); }
}

/**
 * Add a process to the ready queue (priority-based insertion)
 */
static void add_to_ready_queue(process_t *proc) {
    // ---------------------------------------------------------------------
    // #130 (2026-08-15): AN IDLE PROCESS MUST NEVER ENTER THE SHARED QUEUE.
    // ---------------------------------------------------------------------
    // The guard for this existed at exactly ONE of this function's callers
    // (sched_schedule's switch-out path, "#67: never queue an idle proc"). Every
    // other caller could queue an idle proc, and one did: wake_sleeping_procs()
    // at process.c:3138 enqueues any PROC_STATE_SLEEPING entry whose deadline
    // expired, with no is_idle test.
    //
    // CAUGHT IN THE ACT by the kernel's own detector:
    //   [SCHEDRACE] *** CORRUPT CONTEXT DETECTED at pre-switch on cpu 0 ***
    //   reason 2: incoming task still marked on-cpu (half-saved context)
    //   FORENSICS 'idle' pid=0: enq=1 pop=1 now=2 queued_by=0x59b0b4 pinned=0
    //   incoming: 'idle' pid=0 state=2 on_cpu=3 rsp=0x6b6dfc8
    //                                  stack=[0x6b5e640,0x6b6e640)
    //   outgoing: ''     pid=0 state=0 on_cpu=1 rsp=0x0 stack=[0x0,0x0)
    // queued_by resolves to wake_sleeping_procs (process.c:3138). enq=1/pop=1
    // means pid 0 went THROUGH the shared queue and was popped by another core,
    // so cpu0 switched to it while cpu3 still held it.
    //
    // pid 0 is the BSP desktop/idle context and owns ONE fixed kernel stack.
    // Two cores running it shred each other's return addresses on that stack.
    // The result is the repeating wild-RIP fault seen across boots:
    //   [KERNEL PANIC] Invalid Opcode at RIP=0x45   (twice, identical value)
    // with the faulting RSP inside pid 0's own stack range both times.
    //
    // Fixed HERE, in the shared primitive, rather than by adding a tenth
    // is_idle test at a tenth call site: this makes it true BY CONSTRUCTION for
    // every present and future caller. Idle procs are never reached through the
    // queue anyway - they are dispatched directly via g_cpu_idle[] and the
    // empty-queue fallback - so refusing them here removes nothing.
    if (proc && proc->is_idle) return;

    // #75: capture the state the caller handed us, BEFORE anything below
    // overwrites it to READY, plus who the caller was. If a task is ever queued
    // in a state that is not queueable, this is where it becomes visible.
    if (proc) {
        proc->sched_state_at_enq = (uint32_t)proc->state;
        proc->sched_enq_ra       = __builtin_return_address(0);
        // #75 (enqrace75b): count the CALL here and the ENQUEUE in
        // sched_rq_push(), so a site's refusals are call minus enq rather than
        // being invisible. See the ENQ_RA_SLOTS block for the precision caveat.
        int __cslot = enq_ra_slot(proc->sched_enq_ra);
        if (__cslot >= 0) g_enqra_call[__cslot]++; else g_enqra_over++;
    }
    // #75: REFUSE TO QUEUE A TASK THAT IS STILL EXECUTING. This is the single
    // funnel every enqueue goes through, which is why the check is tractable
    // here and was not tractable at the 167 places that write ->state. The owed
    // enqueue is recorded against the core that is running it and paid when that
    // core next enters the scheduler, by which time the task has left.
    { uint32_t __fc = sched_rq_cpu(); if (__fc < MAYTERA_MAX_CPUS) g_in_funnel[__fc] = 1; }
    // #167: ONE READ, NOT TWO. This tested proc->sched_on_cpu and then re-read
    // it to compute `owner`. The owning core can clear it to 0 between the two
    // reads (its switch asm completes), which makes owner = (uint32_t)0 - 1 =
    // 0xFFFFFFFF, and sched_defer_enqueue() then refuses that out-of-range cpu
    // and returns 0 - so the funnel falls through, counts g_enq_allowed, and
    // prints "ALLOWED ... while still executing on cpu4294967295".
    //
    // MEASURED, not hypothetical: that exact line, with that exact cpu number,
    // is in #165's arm-schedrace/run12 serial log, and it is the ONLY time
    // g_enq_allowed has ever been non-zero in 55 boots. #84 is ticketed on that
    // counter, so a false increment on it is expensive: it is the one signal
    // that says the defer table overflowed.
    //
    // The fall-through itself was SAFE - owner can only go out of range because
    // the task stopped executing, and queueing a task that is not executing is
    // exactly right - so this is a counter-honesty fix, not a corruption fix.
    // Say which it is, because #84 will be read off this number.
    int32_t __oc = proc ? proc->sched_on_cpu : 0;
    if (proc && __oc != 0) {
        uint32_t owner = (uint32_t)__oc - 1;
        if (sched_defer_enqueue(owner, proc)) {
            g_enq_refused++;
            // ===============================================================
            // #167 FIX, arm 1 of 2. THE QUEUE INSERTION IS DEFERRABLE; THE
            // STATE TRANSITION IS NOT.
            //
            // What #75 has to prevent is a still-executing task appearing in a
            // RUN QUEUE, where another core can pop it and switch into its live
            // kernel stack. Deferring the INSERTION achieves that completely.
            // Deferring the STATE WRITE achieves nothing extra - a task that is
            // in no queue cannot be found by any core - and it destroys the only
            // record that a wake ever happened, because sched_drain_deferred()
            // reconstructs "is this still wanted?" from ->state.
            //
            // Restricted to BLOCKED/SLEEPING on purpose. That is precisely the
            // proc_wake() case, the one caller that does not pre-set READY. A
            // requeue of a RUNNING/READY task already carries its own
            // transition and must not be touched here.
            //
            // PAIRED WITH sync/waitq.c's __wait_finish(). A task woken this way
            // may never sleep at all: it can observe its entry unlinked, break
            // out of __wait_event_wait() and run on with state READY. If nothing
            // undid that, the next drain would enqueue a RUNNING task, which is
            // the (c) corruption. __wait_finish()'s #610 un-park now covers
            // READY for exactly this reason, and sched_self_running()'s
            // store order (state first, THEN sched_on_cpu = 0) is what makes it
            // safe: a drain that observes sched_on_cpu == 0 necessarily also
            // observes state == RUNNING and therefore drops the stale entry.
            // ===============================================================
            if (g_wake_defer_fix &&
                (proc->state == PROC_STATE_BLOCKED ||
                 proc->state == PROC_STATE_SLEEPING)) {
                proc->state       = PROC_STATE_READY;
                proc->ready_since = sched_ticks;   // #254: the wait starts now
                g_enq_wakefix++;
            }
            { uint32_t __fc = sched_rq_cpu(); if (__fc < MAYTERA_MAX_CPUS) g_in_funnel[__fc] = 0; }
            return;
        }
        // No room to defer: we are about to enqueue a task that is STILL
        // EXECUTING. Record it - this is shape (b), the funnel allowing one.
        g_enq_allowed++;
        proc->enq_allowed_hot = 1;
        { static int warned_allow = 0;
          if (!warned_allow) { warned_allow = 1;
            kprintf("[ENQPROBE] funnel ALLOWED '%s' pid=%u while still executing "
                    "on cpu%u (defer table full)\n", proc->name, proc->pid, owner); } }
        // No room to defer: fall through and queue it. Bounded and vanishingly
        // rare (SCHED_DEFER_MAX outstanding switches on one core), and losing a
        // runnable task would be worse than the window we are closing.
    }
    // #75 (enqrace75b): FROM HERE THIS CALL ENQUEUES. Everything above either
    // returned or fell through deliberately, so this is the last point at which
    // the caller's return address still describes an enqueue that HAPPENS.
    if (proc) proc->sched_enq_ra_ok = proc->sched_enq_ra;

    {   // #67: per-cpu run queues, only when AP user scheduling is enabled.
        extern int g_smp_user_sched;
        if (g_smp_user_sched) {
            sched_rq_push(proc);
            { uint32_t __fc = sched_rq_cpu(); if (__fc < MAYTERA_MAX_CPUS) g_in_funnel[__fc] = 0; }
            return;
        }
    }
    { uint32_t __fc = sched_rq_cpu(); if (__fc < MAYTERA_MAX_CPUS) g_in_funnel[__fc] = 0; }
    proc->next = NULL;
    proc->state = PROC_STATE_READY;
    proc->ready_since = sched_ticks;   // #254: when the wait for the CPU began

    if (ready_queue_head == NULL) {
        ready_queue_head = ready_queue_tail = proc;
        return;
    }

    // Simple priority queue: insert based on priority
    // Higher priority goes first
    if (sched_eff_prio(proc) > sched_eff_prio(ready_queue_head)) {
        proc->next = ready_queue_head;
        ready_queue_head = proc;
        return;
    }

    // Find insertion point
    process_t *prev = NULL;
    process_t *curr = ready_queue_head;
    while (curr && sched_eff_prio(curr) >= sched_eff_prio(proc)) {
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
    {   // #67: per-cpu run queues, only when AP user scheduling is enabled.
        extern int g_smp_user_sched;
        if (g_smp_user_sched) return sched_rq_pop();
    }
    if (ready_queue_head == NULL) {
        return NULL;
    }

    process_t *proc = ready_queue_head;
    ready_queue_head = proc->next;
    if (ready_queue_head == NULL) {
        ready_queue_tail = NULL;
    }
    proc->next = NULL;
    // #254: the promotion is ONE-SHOT. It bought this turn; from here on the
    // process sorts by its real priority again.
    proc->prio_boost = 0;
    return proc;
}

/**
 * #254/#601 anti-starvation sweep. Promotes any ready-queue entry that has
 * waited longer than SCHED_STARVE_TICKS to the front of the queue, once.
 *
 * Called from sched_schedule() with interrupts already off (#610), so the list
 * surgery below is in the same critical section as the rest of the scheduler's
 * queue manipulation and cannot be re-entered by a timer tick.
 */
// #67: the per-cpu-queue form of the same sweep. Without this, turning
// g_smp_user_sched on would SILENTLY DELETE the #254/#601 anti-starvation fix:
// the global list is empty under the gate, so the sweep below would return at
// its first test and the measured 4.7 s PRIO_LOW starvation would come straight
// back with nothing in the logs to say so. Runs on the calling core's own queue
// only: a starving entry is starving relative to the queue it is IN, and
// touching another core's queue here would need the steal path's policy, not
// this one's.
static void sched_age_rq(uint32_t cpu) {
    sched_age_ent_t ents[SCHED_AGE_SCAN_MAX];
    process_t      *nodes[SCHED_AGE_SCAN_MAX];
    uint8_t         sel[SCHED_AGE_SCAN_MAX];
    uint32_t n = 0;

    uint64_t fl = RQ_LOCK();
    for (process_t *p = g_rq[cpu].head; p && n < SCHED_AGE_SCAN_MAX; p = p->next) {
        nodes[n] = p;
        ents[n].ready_since = p->ready_since;
        ents[n].prio        = (uint32_t)p->priority;
        ents[n].boosted     = p->prio_boost ? 1u : 0u;
        n++;
    }
    if (n < 2 ||
        sched_age_select_rs(ents, n, sched_ticks, SCHED_STARVE_TICKS,
                            SCHED_AGE_MAXPROMOTE, sel, SCHED_AGE_SCAN_MAX) <= 0) {
        rq_unlock(fl);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (!sel[i]) continue;
        process_t *p = nodes[i];
        if (!sched_rq_unlink_locked(cpu, p)) continue;   // moved under us: skip
        p->prio_boost = 1;
        sched_rq_insert_locked(cpu, p);                  // re-sorts by eff prio
        g_sched_promotions++;
    }
    rq_unlock(fl);
}

static void sched_age_ready_queue(void) {
    // #67 pass 2: PER-CORE rate limiter. A single static here would let whichever
    // core happened to sweep first suppress every other core's sweep for the
    // next 100 ms, which silently disables anti-starvation on all but one core.
    static uint64_t age_last[MAYTERA_MAX_CPUS];
    uint32_t rc = sched_rq_cpu();
    if (rc >= MAYTERA_MAX_CPUS) rc = 0;
    if ((sched_ticks - age_last[rc]) < SCHED_AGE_PERIOD) return;  // the whole hot-path cost
    age_last[rc] = sched_ticks;
    {   // #67
        extern int g_smp_user_sched;
        if (g_smp_user_sched) { sched_age_rq(rc); return; }
    }
    if (!ready_queue_head || !ready_queue_head->next) return;  // 0 or 1 entry cannot starve

    sched_age_ent_t ents[SCHED_AGE_SCAN_MAX];
    process_t      *nodes[SCHED_AGE_SCAN_MAX];
    uint8_t         sel[SCHED_AGE_SCAN_MAX];
    uint32_t n = 0;
    for (process_t *p = ready_queue_head; p && n < SCHED_AGE_SCAN_MAX; p = p->next) {
        nodes[n] = p;
        ents[n].ready_since = p->ready_since;
        ents[n].prio        = (uint32_t)p->priority;
        ents[n].boosted     = p->prio_boost ? 1u : 0u;
        n++;
    }
    if (sched_age_select_rs(ents, n, sched_ticks, SCHED_STARVE_TICKS,
                            SCHED_AGE_MAXPROMOTE, sel, SCHED_AGE_SCAN_MAX) <= 0)
        return;

    for (uint32_t i = 0; i < n; i++) {
        if (!sel[i]) continue;
        process_t *p = nodes[i];
        // Unlink p from wherever it currently sits.
        if (ready_queue_head == p) {
            ready_queue_head = p->next;
            if (ready_queue_head == NULL) ready_queue_tail = NULL;
        } else {
            process_t *q = ready_queue_head;
            while (q && q->next != p) q = q->next;
            if (!q) continue;                       // not found: leave the list alone
            q->next = p->next;
            if (ready_queue_tail == p) ready_queue_tail = q;
        }
        p->next = NULL;
        // Re-insert with the boost set: sched_eff_prio() now puts it above every
        // unboosted entry, so add_to_ready_queue() places it at the front and
        // the list stays sorted by EFFECTIVE priority.
        p->prio_boost = 1;
        add_to_ready_queue(p);
        g_sched_promotions++;
    }
}

// ============================================================================
// Process Creation and Lifecycle
// ============================================================================

/**
 * Idle process - runs when no other processes are ready
 */
static void idle_process(void *arg) {
    extern void desktop_process_tick(void);
    // #745 task #59: "is a userland compositor holding the screen" is now a
    // question for the ownership latch (gui/fbown.h), not a raw global that
    // stayed non-zero after the compositor died.
    extern uint32_t fb_owner_pid(void);
    (void)arg;
    while (1) {
        // Under the userland compositor (it grabs input + owns the screen) the
        // kernel desktop tick is redundant; skipping it lets the core actually
        // HLT instead of polling input every tick (#102 host-CPU).
        if (fb_owner_pid() == 0) desktop_process_tick();
        // Atomically enable interrupts and HALT the core. sti has a 1-instr
        // delay so hlt executes before any IRQ fires - this guarantees the vCPU
        // actually halts until the next interrupt (the old hlt();proc_yield()
        // sequence ran cli() in proc_yield and let hlt return immediately,
        // pegging the host at ~100%). The timer IRQ + sched_tick reschedule us
        // to any process that becomes ready.
        sched_halt_bkl_note(3, 1);   // #75
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
    // #745 (local 75) CLASS FIX: the task being started is THIS cpu's, and
    // this read decides whose entry_point gets called.
    process_t *proc = proc_current();
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

// ---------------------------------------------------------------------------
// #254 boot self-test for the Rust aging policy. Runs before any scheduling, so
// a broken policy is caught at boot rather than as a mysterious freeze under
// load. Deliberately NOT a differential against a C twin: there is no C twin,
// and a differential between two copies of the same mistake proves nothing.
// These are invariants: what the queue is allowed to do to a waiter.
// ---------------------------------------------------------------------------
static void sched_age_selftest(void) {
    sched_age_ent_t e[6];
    uint8_t o[6];
    int checks = 0, fail = 0;
#define SA_CHK(c) do { checks++; if (!(c)) { fail++; \
    kprintf("[RUST-SCHED] FAIL at line %d\n", __LINE__); } } while (0)

    // Bad arguments are refused, and n > outcap is refused (no overrun).
    SA_CHK(sched_age_select_rs(0, 2, 100, 10, 4, o, 6) == -1);
    SA_CHK(sched_age_select_rs(e, 2, 100, 10, 4, 0, 6) == -1);
    SA_CHK(sched_age_select_rs(e, 7, 100, 10, 4, o, 6) == -1);
    SA_CHK(sched_age_select_rs(e, 0, 100, 10, 4, o, 6) == 0);

    // The head is never promoted: it is what the next pop returns.
    e[0].ready_since = 0;   e[0].prio = 1; e[0].boosted = 0;
    e[1].ready_since = 0;   e[1].prio = 1; e[1].boosted = 0;
    o[0] = o[1] = 0xAA;
    SA_CHK(sched_age_select_rs(e, 2, 1000, 125, 4, o, 6) == 1);
    SA_CHK(o[0] == 0 && o[1] == 1);

    // Exactly at the bound promotes; one tick short does not.
    e[1].ready_since = 1000 - 125;
    SA_CHK(sched_age_select_rs(e, 2, 1000, 125, 4, o, 6) == 1 && o[1] == 1);
    e[1].ready_since = 1000 - 124;
    SA_CHK(sched_age_select_rs(e, 2, 1000, 125, 4, o, 6) == 0 && o[1] == 0);

    // An already-boosted entry is never promoted again (the boost is one-shot;
    // re-promoting it every sweep would let it hold the head for ever).
    e[1].ready_since = 0; e[1].boosted = 1;
    SA_CHK(sched_age_select_rs(e, 2, 1000, 125, 4, o, 6) == 0 && o[1] == 0);
    e[1].boosted = 0;

    // max_promote caps one sweep, and the entries before the cap are the ones
    // marked (queue order is preserved).
    for (int i = 0; i < 6; i++) { e[i].ready_since = 0; e[i].prio = 1; e[i].boosted = 0; }
    SA_CHK(sched_age_select_rs(e, 6, 1000, 125, 2, o, 6) == 2);
    SA_CHK(o[0] == 0 && o[1] == 1 && o[2] == 1 && o[3] == 0 && o[4] == 0 && o[5] == 0);

    // A ready_since in the FUTURE (an entry queued after `now` was sampled)
    // must be refused, not treated as a near-infinite age.
    e[1].ready_since = 1001;
    SA_CHK(sched_age_select_rs(e, 6, 1000, 125, 4, o, 6) >= 0 && o[1] == 0);

    // A zero bound must not promote everything.
    for (int i = 0; i < 6; i++) e[i].ready_since = 0;
    SA_CHK(sched_age_select_rs(e, 6, 1000, 0, 4, o, 6) == 0);

    // Every output slot is written on a success path (no stale bytes).
    for (int i = 0; i < 6; i++) o[i] = 0xAA;
    SA_CHK(sched_age_select_rs(e, 6, 1000, 0, 4, o, 6) == 0);
    for (int i = 0; i < 6; i++) SA_CHK(o[i] == 0);

    kprintf("[RUST-SCHED] sched_age %d checks %d fail %s\n",
            checks, fail, fail ? "FAIL" : "PASS");
#undef SA_CHK
}

void proc_init(void) {
    kprintf("[PROC] Initializing process subsystem...\n");
    sched_age_selftest();   // #254 aging policy invariants

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

    // #230: the child-exit wait queue. Initialised before any process can
    // exit, and gated by g_child_exit_wq_ready so an exit that somehow beats
    // this is a no-op rather than a walk through an uninitialised lock.
    wait_queue_head_init(&g_child_exit_wq);
    g_child_exit_wq_ready = 1;

    // Create idle process (PID 0)
    process_t *idle = &proc_table[0];
    init_proc(idle, "idle", PRIO_NORMAL);  // PID 0 runs the desktop, needs normal priority
    idle->pid = 0;  // Special PID for idle
    next_pid = 1;   // Reset for next regular process
    // #67 pass 2: "pid == 0" and "is the idle process" stopped being the same
    // question when the APs got idle processes of their own. This is the BSP's.
    idle->is_idle = 1;
    g_cpu_idle[0] = idle;

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
    // #151: SysV requires a function to be ENTERED with RSP == 8 (mod 16), as if
    // it were reached by a CALL from a 16-aligned RSP. context_switch reaches the
    // entry fn via RET, so without this it starts at RSP == 0 (mod 16) and every
    // 16-byte-aligned slot the compiler places on its frame is 8 bytes off.
    stack_top -= 8;

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

    // #446: the switch frame is exactly RFLAGS + 15 GPRs + return address.
    // The FXSAVE image lives in idle->fpu_area, NOT carved off this stack.
    idle->rsp = stack_top;
    proc_init_fpu_area(idle);

    // #446 MEASURED (build 943 instrumentation): the synthetic frame above is
    // DEAD. pid 0 is not a process that gets started; it IS the boot thread.
    // current_proc is set to idle below while the CPU is still running main()
    // on the entry.asm boot stack, so the FIRST context_switch out of pid 0
    // overwrites idle->rsp with the live boot-stack pointer and the frame built
    // here is never popped. From then on pid 0 (which also runs the desktop)
    // lives on the 64 KB static boot stack forever, while stack_base/stack_size
    // still described the 16 KB kmalloc'd block nobody uses. That lie is why
    // every #446 double-fault capture showed pid 0's switch frame on a "shared"
    // kernel stack: it was the boot stack, shared with every pre-scheduler
    // kernel context, and no stack accounting or overflow check could see it.
    // Point stack_base/stack_size at the stack pid 0 ACTUALLY uses. The 16 KB
    // block stays allocated (never freed, one-time, pid 0 never exits) because
    // idle->rsp still refers to it until the first switch out.
    {
        extern char kernel_stack_top[];
        idle->stack_base = (void *)kernel_stack_bottom;
        idle->stack_size = (uint64_t)(kernel_stack_top - kernel_stack_bottom);
        kprintf("[PROC] pid 0 runs on the boot stack [0x%lx,0x%lx) (%lu KB)\n",
                (uint64_t)kernel_stack_bottom, (uint64_t)kernel_stack_top,
                idle->stack_size / 1024);
    }
    proc_stack_tag(idle);
    idle->context = NULL;  // Not using struct anymore
    idle->entry_point = idle_process;
    idle->entry_arg = NULL;
    idle->state = PROC_STATE_READY;

    // Set idle as current process initially
    current_proc = idle;
    current_process = idle;
    // #75 part 4: the process table and the BSP idle now exist and will not be
    // wiped again. Only from here may an AP join the scheduler.
    g_proc_init_done = 1;
    idle->state = PROC_STATE_RUNNING;

    // #745 (task 37): prove the reclaim policy on this exact build before any
    // process is created. Case 4 is the regression case (a detached zombie
    // whose parent is alive must be reclaimable); if it ever regresses the App
    // Store stops loading ~40 fetches into a boot, which is a symptom nobody
    // connects back to here.
    {
        uint32_t rc = proc_reap_selftest_rs();
        if (rc == 0) kprintf("[PROC] procreap selftest OK (11 cases)\n");
        else         kprintf("[PROC] procreap selftest FAILED at case %u\n", (unsigned)rc);
    }

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

    // #745 (task 37): every process created here is a KERNEL WORKER. init_proc()
    // has just set ppid from current_proc, which for a worker started inside a
    // syscall (async_fetch_worker / async_post_worker) is the Ring 3 caller.
    // That caller never learns this pid and never wait()s for it, so without
    // this flag the worker's zombie was permanent and 41 fetches exhausted the
    // 64-slot table. Marking it detached makes it invisible to proc_wait() and
    // reclaimable by reap_orphan_zombies(). See rustkern/procreap.rs.
    proc->detached = 1;

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
    // #151: SysV requires a function to be ENTERED with RSP == 8 (mod 16), as if
    // it were reached by a CALL from a 16-aligned RSP. context_switch reaches the
    // entry fn via RET, so without this it starts at RSP == 0 (mod 16) and every
    // 16-byte-aligned slot the compiler places on its frame is 8 bytes off.
    stack_top -= 8;

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

    // #446: the switch frame is exactly RFLAGS + 15 GPRs + return address.
    // The FXSAVE image lives in proc->fpu_area, NOT carved off this stack.
    proc->rsp = stack_top;
    proc_init_fpu_area(proc);
    proc_stack_tag(proc);
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
    // #745 (local 75) CLASS FIX: exit the task running on THIS cpu.
    // Through the BSP-published global, proc_exit() on an AP zombified the
    // BSP's task, closed ITS fds and freed ITS windows, and left the real
    // caller running.
    process_t *me = proc_current();

    if (me) me->exit_code = exit_code;

    if (!me || me->pid == 0) {
        // Can't exit idle process
        kprintf("[PROC] Cannot exit idle process\n");
        return;
    }

    kprintf("[PROC] Process '%s' (PID %u) exiting\n",
            me->name, me->pid);
    // #134: and to the PERSISTENT log. This line is half of the evidence for a
    // whole class of real-hardware faults - an app that launches and instantly
    // dies (#153) - and until now it existed only on serial, which the iMac14,4
    // does not have. proc_exit() runs as an ordinary thread with interrupts on
    // (the cli() is further down), so the normal bootlog_write() is correct
    // here; it also carries its own no-block guard if that ever stops being
    // true. Process exits are not a high-rate event, so this is one appended
    // line per exit, not a trace.
    bootlog_write("[PROC] '%s' (PID %u) exiting, code %d", me->name, me->pid,
                  me->exit_code);

    // #430: CLONE_CHILD_CLEARTID - a thread created via clone() with a
    // clear_child_tid address must, on exit, zero that word (in the still-live
    // shared address space) and futex-wake anyone blocked on it. This is how
    // pthread_join() learns the thread has finished. Done BEFORE cli() so the
    // shared cr3 is active and the write lands on the right page.
    if (me->clear_child_tid) {
        uint32_t *ctid = me->clear_child_tid;
        me->clear_child_tid = NULL;
        // #19/#645: `ctid` is a Ring-3 address in the still-live shared address
        // space. This was the unconverted twin of thread.c's exit path; use the
        // SAME canonical primitive it uses rather than a private raw store, so
        // this site gets the U/S check, the fault fixup and the AC bracket that
        // copy_to_user already carries.
        uint32_t __zero = 0;
        if (copy_to_user(ctid, &__zero, sizeof(__zero)) != 0) {
            kprintf("[PROC] clear_child_tid %p not writable at exit; skipping\n",
                    (void *)ctid);
        }
        extern void futex_wake_addr(uint32_t *addr, int count);
        futex_wake_addr(ctid, 1);
    }

    // Disable interrupts during cleanup
    cli();

    // Phase A1: drop every open file descriptor so reference counts on
    // struct files are correct. fd_close_all operates on the current
    // we are the current process (about to become a zombie). Done under cli.
    extern void fd_close_all(void);
    fd_close_all();

    // Mark as zombie (cleanup will happen later)
    // #75: leave every run queue BEFORE becoming a zombie, so no core can pop
    // this PCB and switch to a stack that is about to be freed.
    sched_note_exit(me);   // #75: is anyone already committed to us?
    sched_rq_remove(me);
    me->state = PROC_STATE_ZOMBIE;

    // #230: wake any parent parked in proc_wait(). Under cli(), which is fine:
    // wake_up_all() uses spinlock_acquire_irqsave and only marks processes
    // runnable, it never sleeps.
    proc_child_exit_notify();

    // Clean up user windows for this process
    extern void cleanup_user_windows_for_process(uint32_t pid);
    cleanup_user_windows_for_process(me->pid);

    // Tear down any Ring-3 PCM stream this process owned but never closed. The
    // music player force-kills its /APPS/MUSICPLR --play helper with SIGKILL on
    // a manual track switch, so a stream outliving its owner is a NORMAL path,
    // not an edge case: without this the next track would hit EBUSY until the
    // pump's backstop expired. Only sets flags and wakes the pump (safe under
    // cli(); the pump thread does the actual teardown and frees the ring).
    extern void audio_pcm_proc_exit(uint32_t pid);
    audio_pcm_proc_exit(me->pid);

    // #205: if THIS was the DOS OPL2 synthesiser (/APPS/FMSYNTH), let the DOS
    // layer know so it stops reporting an FM chip it can no longer sound. The
    // flag that said "a synthesiser is running" was set once and cleared never,
    // so every DOS game after the first in a boot session played its music into
    // a queue nobody drained. Flag-only, no blocking: this runs under cli().
    extern void dos_fm_proc_exit(uint32_t pid);
    dos_fm_proc_exit(me->pid);

    // #745 (task #59): release the FRAMEBUFFER ownership latch if this process
    // held it. Switch User and Log Out both end a session by exiting
    // /APPS/COMPOSIT, and the latch used to be cleared only by
    // fb_syscall_init() at boot - so from the first Log Out onwards every
    // relaunched compositor was refused sys_fb_map() and bounced back to the
    // login gate, which under autologin=root is an infinite respawn loop.
    // This is the same chokepoint the three hooks around it use, for the same
    // reason: EVERY termination path funnels through proc_exit() (a voluntary
    // exit, a fatal signal via signal.c's proc_exit(128+signo), and the crash
    // handler in cpu/idt.c).
    //
    // NOT gated on shares_vm: the latch names one exact pid (the thread that
    // called sys_fb_map), so a worker thread exiting releases nothing, and
    // gating on the group leader would MISS a compositor that mapped the
    // framebuffer from a thread. Same cli()-safe contract as its neighbours:
    // one atomic compare-exchange, no allocation, no block.
    extern void fb_owner_proc_exit(uint32_t pid);
    fb_owner_proc_exit(me->pid);

    // #fdguard: poison any legacy fd slots this process group still owns, so
    // a future process handed the same pid cannot reach a leaked slot. Only
    // on a group-leader / ordinary-process exit: a worker thread shares the
    // leader's tgid and must NOT poison the group's still-open fds. cli()-safe
    // (one atomic per slot, no block). See proc/fdlayer.c.
    if (!me->shares_vm) {
        extern void fdown_proc_exit(uint32_t owner);
        fdown_proc_exit(me->tgid ? me->tgid : me->pid);
    }

    // #COMPRESPAWN: TEAR DOWN THE sys_fb_map() WINDOW BEFORE THE ADDRESS SPACE
    // IS DESTROYED. This is not tidiness; leaving it mapped corrupts the kernel
    // heap.
    //
    // sys_fb_map() (gui/fb_syscall.c) maps the framebuffer BACK BUFFER into the
    // compositor's address space at 0x0000600000000000 with VMM_USER_RW. The
    // back buffer is kmalloc_aligned() KERNEL HEAP memory (video/framebuffer.c),
    // used as a physical address because the kernel runs on the UEFI identity
    // map. Those PTEs are therefore PRESENT|USER and point at live kernel heap.
    //
    // vmm_destroy_user_space() (mm/vmm.c) walks every PML4 slot the reference
    // address space does not have - and the kernel has no PML4[192], which is
    // where 0x600000000000 lands - and calls vmm_free_user_page_cow() on every
    // PRESENT|USER leaf it finds. So on every compositor death the reap handed
    // ~8 MB of LIVE KERNEL HEAP (1920x1080x4) to the PMM free list while the
    // heap allocator still considered it allocated. The PMM then reissues those
    // pages to the next allocation, including the ELF image of the compositor
    // being relaunched. That is silent, unbounded corruption on precisely the
    // path a crashed compositor takes.
    //
    // Clearing the PTEs here makes the leaves invisible to that walk, so the
    // generic destroy rule stays generic and the frames stay owned by the heap.
    // Same cli()-safe contract as the hooks around it: clears PTEs and invlpg,
    // no allocation, no lock, no block.
    { extern void fb_unmap_proc_exit(uint32_t pid, uint64_t cr3);
      fb_unmap_proc_exit(me->pid, me->cr3); }

    // #745 (task #36): release any async HTTP job slots this process owned.
    // Without this a process that died mid-fetch left its slot allocated for
    // ever, and six of those exhausted the six-slot table until reboot: a
    // trivial denial of service from an unprivileged app.
    //
    // Only the GROUP LEADER sweeps. A job is owned by the thread group (so a
    // fetch started on one pthread can be polled from another), and a worker
    // thread exiting must not take the whole process's downloads with it.
    // Same cli()-safe contract as the two calls above: flags and kfree only,
    // no blocking.
    if (!me->shares_vm) {
        extern void async_http_proc_exit(uint32_t owner);
        async_http_proc_exit(me->tgid ? me->tgid
                                                : me->pid);
    }

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
#ifdef ENQ_PROBE_SELFTEST
// ===========================================================================
// #75 (enqrace75b) PROBE SELF-TEST. `make ENQTEST=1`. NEVER shipped.
//
// WHY THIS EXISTS. Three of the fields #75 has reasoned from for four campaigns
// did not mean what they were read as meaning, and one of the probes could only
// ever produce the conclusion that was drawn from it: [WAKEPROBE] fires only on
// (sched_on_cpu != 0 || rq_queued || rq_wanted), so "the candidate is always
// still executing" was entailed by its own predicate. A probe in that condition
// is worse than no probe, because it answers confidently.
//
// The test for a probe is therefore: CONSTRUCT the state it claims to detect and
// show it fires, then construct the neighbouring state and show it does not.
// That is what this does. It runs once, in process context, from proc_yield(),
// the first place after SMP is up where a real task is executing and can be
// experimented on.
//
// The [PINENQ] guard is the reason this is not optional. It has never fired,
// across a 180 s SCHEDRACE_US=200 reproducer with roughly 57 s of cumulative
// window exposure. "Never fired" is only evidence about the world if the probe
// CAN fire; until then it is equally consistent with a probe that is dead.
static int g_enqtest_done = 0;
static void enq_probe_selftest(void) {
    extern int g_smp_user_sched;
    if (g_enqtest_done || !g_smp_user_sched) return;
    process_t *me = proc_current();
    if (!me || me->is_idle) return;
    g_enqtest_done = 1;

    // ---- 1. THE PREDICATE --------------------------------------------------
    // POSITIVE, by construction: this task is executing - it is the one calling.
    // NEGATIVE, by construction: an UNUSED process slot is not any core's
    // current. Neither is a sample; both are states we know the answer for.
    int32_t own_self = sched_running_owner(me);
    process_t *dead = 0;
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (proc_table[i].state == PROC_STATE_UNUSED) { dead = &proc_table[i]; break; }
    int32_t own_dead = dead ? sched_running_owner(dead) : -1;
    kprintf("[ENQTEST] predicate sched_running_owner: RUNNING task '%s' -> %d "
            "(want non-zero), UNUSED slot -> %d (want 0): %s\n",
            me->name, own_self, own_dead,
            (own_self != 0 && own_dead == 0) ? "PASS" : "FAIL");

    // ---- 2. THE [PINENQ] PROBE, WHICH HAS NEVER FIRED ----------------------
    // Construct exactly what it claims to detect: a task carrying a live
    // selection pin arriving at the enqueue path.
    //
    // HAZARD, and why there is almost none. With the pin fix ON this is
    // hazard-free: sched_rq_push() defers and never inserts, so no task is put
    // into a queue while pinned. With the fix OFF the entry IS inserted, and the
    // sched_rq_remove() below takes it straight back out; that leaves a window of
    // a few instructions, once per boot, in a test-gated build. It is disclosed
    // rather than hidden because the window is the thing under study.
    uint64_t p0 = g_enq_pinned;
    me->enq_probe_tag = 1;
    me->sched_pinned  = (int32_t)sched_rq_cpu() + 1;
    me->state         = PROC_STATE_READY;
    add_to_ready_queue(me);
    me->sched_pinned  = 0;
    sched_rq_remove(me);           // no-op when the push correctly deferred
    me->state         = PROC_STATE_RUNNING;
    me->enq_probe_tag = 0;
    kprintf("[ENQTEST] [PINENQ] on a task with a live pin: g_enq_pinned %lu -> "
            "%lu: %s\n", (unsigned long)p0, (unsigned long)g_enq_pinned,
            (g_enq_pinned > p0) ? "PASS (the probe fires)"
                                : "FAIL (the probe is silent on its own state)");
    kprintf("[ENQTEST] neighbouring state = the whole rest of this boot: every "
            "other enqueue has sched_pinned == 0, so pinenq= in [ENQCENSUS] must "
            "stay at %lu.\n", (unsigned long)g_enq_pinned);
    kprintf("[ENQTEST] [RUNENQ] has no synthetic control and needs none: its "
            "positive and negative arms are per-call-site in [ENQCENSUS] "
            "(site:calls/enq/enq-of-running), at n in the thousands.\n");
}
#endif

void proc_yield(void) {
#ifdef ENQ_PROBE_SELFTEST
    enq_probe_selftest();
#endif
    // #745 (local 75) CLASS FIX: yield the task running on THIS cpu.
    // Through the BSP-published global, an AP-side yield re-queued a task
    // that was still RUNNING on cpu0, the #421 phase 7 hazard.
    process_t *me = proc_current();

    cli();

    // =====================================================================
    // #174: BOTH STATEMENTS BELOW ARE LOAD-BEARING, AND THEY ARE LOAD-BEARING
    // FOR TWO DIFFERENT REASONS. DO NOT REMOVE EITHER ONE ALONE.
    //
    // #75 measured that this enqueue puts a task a core is RUNNING into a run
    // queue on 100% of its enqueues, and that no guard on the path refuses it.
    // That is true, and it reads like a bug to be removed. It is not: it is how
    // yield works, and each half of this pair defeats a DIFFERENT early return
    // in sched_schedule(). Both removals were BUILT AND BOOTED at #174 rather
    // than argued about (build 1989, 4 vCPU + DOS regime, 170-200 s captures,
    // ENQTEST=1 SCHEDRACE=1 SCHEDRACE_US=200, against 4 clean baseline runs).
    //
    // DROP THE ENQUEUE, KEEP state = READY:
    //   the yielder is left READY, on no queue and owned by no core, because
    //   sched_schedule()'s own requeue of prev is gated on
    //   prev->state == PROC_STATE_RUNNING and therefore does not fire. The task
    //   is gone. MEASURED, 2 of 2 runs: the boot still reaches DESKTOP_READY
    //   with no panic, no [ENQLOST] and no [SCHEDRACE] corruption, and then the
    //   machine dies quietly. Framebuffer flips over the run fell from 2695 to
    //   52-59, host CPU from 2.71 cores to 0.55, and top= went from dos:86 to
    //   idle:95. gaps= fell from 35170 to 2, i.e. after the first two yields
    //   nothing was left alive to yield. A does-it-boot test PASSES this.
    //   Only a liveness metric catches it.
    //
    // DROP BOTH, AND LET sched_schedule() REQUEUE prev INSTEAD:
    //   this does close the #75 hole (run=0, and the prev-requeue site defers
    //   every one of its 2072 enqueues), and it ran at baseline throughput in
    //   3 of 4 runs. It is still wrong, for a reason no arm at that n could
    //   have shown: leaving state at RUNNING re-opens
    //     if (!preemption_enabled && cur && cur->state == PROC_STATE_RUNNING)
    //   near the top of sched_schedule(), and the no-idle-process early return
    //   below it, BOTH of which return without switching. drivers/usb_msc.c
    //   calls proc_yield() specifically in the !preemption_enabled case
    //   (msc_cmd_lock_noblock: "if (sched_preemption_enabled()) proc_sleep(1);
    //   else proc_yield();"), so for that caller yield would become a no-op and
    //   its loop a 100% spin, which is the #67 pass-12 failure. Writing READY
    //   first is what makes those early returns unreachable from a yield.
    //
    // WHAT THE ENQUEUE BUYS, in the tree's own words (dos/dosexec.c, THE YIELD
    // DISCIPLINE): "proc_yield() is a HANDOFF, not a wait ... If the ready
    // queue is empty the scheduler hands the core straight back, so the 38-45%
    // that used to go to hlt now goes to the guest." Being in the queue is what
    // makes the yielder a candidate in the very selection it triggers, so an
    // empty queue takes the next == cur fast path instead of switching to the
    // idle task and halting until the next timer tick.
    //
    // SO WHAT IS THE HOLE WORTH TODAY? Nothing, and that is measured too:
    // pop_run=0 of 96377 pops and popingap=0 across 35170 deliberately widened
    // windows. The enqueue and the pop are both inside the BKL, so no other
    // core can be in them at the same time. This is a landmine for the
    // BKL-narrowing work (#67/#118), not a live defect. Whoever narrows the BKL
    // must give the yielder a way to stay a selection candidate WITHOUT sitting
    // in a queue another core can pop, and must keep a caller that yields with
    // preemption disabled out of both early returns. Deleting statements from
    // here is not that fix.
    // =====================================================================
    if (me && me->state == PROC_STATE_RUNNING) {
        me->state = PROC_STATE_READY;
        add_to_ready_queue(me);
        { uint32_t __yc = sched_rq_cpu();
          if (__yc < MAYTERA_MAX_CPUS) { g_yield_gap[__yc] = 1; g_gap_opened++; } }
        // #75 (enqrace75b): me is now IN A RUN QUEUE AND STILL EXECUTING, and
        // stays that way until sched_schedule() below arms me->sched_on_cpu.
        // Under `make SCHEDRACE=1` only, hold that window open so the pop-side
        // probe can be seen to fire. Never compiled into a shipping kernel.
        schedrace_delay(SR_SITE_YIELD_ENQ);
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

// #483/#499: THE deadline clock for sleeps/alarms/timed waits, in milliseconds.
// timer_ticks counts ticks DELIVERED, not time ELAPSED: under KVM a starved
// vCPU gets its missed PIT ticks re-injected in a BURST, so a `timer_ticks + N`
// deadline collapses to ~0 real time and the scheduler livelocks (94-96% host
// CPU, the heartbeat stops). mono_ms() is the TSC-backed monotonic clock
// (cpu/mono.h), calibrated at boot before proc_init(), so it measures REAL
// elapsed ms and a tick burst can no longer shorten a deadline. Falls back to
// tick-derived ms only if the monotonic clock is not yet calibrated (early
// boot, before any process sleeps), so behaviour is never worse than before.
uint64_t sched_now_ms(void) {
    if (mono_ready()) return mono_ms();
    extern volatile uint64_t timer_ticks;
    extern uint32_t g_timer_hz;
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    return (timer_ticks * 1000ULL) / hz;
}

/**
 * Sleep for specified milliseconds
 */
void proc_sleep(uint32_t ms) {
    // #745 (local 75) FIX: resolve the sleeper with the per-cpu accessor,
    // not the BSP-published global. sched_schedule() only writes that global
    // when sched_rq_cpu()==0 or the AP gate is off, by design, so on an AP it
    // named the BSP's task: arming wake_time on it and calling
    // sched_self_block() on it left the CALLER running and force-slept an
    // innocent cpu0 task. Measured on an AP: 444 records, caller still
    // RUNNING 443/443, victim a different task 100%.
    process_t *me = proc_current();

    if (!me || ms == 0) return;

    cli();

    // #483/#499: arm the wake against the MONOTONIC ms clock, not timer_ticks.
    // wake_time is now an absolute mono_ms() deadline in MILLISECONDS (see
    // wake_sleeping_procs). Using real elapsed ms means a KVM tick burst can no
    // longer collapse this deadline to ~0 and livelock the scheduler.
    me->wake_time = sched_now_ms() + (uint64_t)ms;
    sched_self_block(me, PROC_STATE_SLEEPING);   // #75: one operation

    sched_schedule(); // Run new process
}


// ===========================================================================
// #230: proc_wait() used to BUSY-POLL for a child to exit.
//
// The old loop was: scan proc_table for a zombie child; if none, call
// sched_schedule() and scan again, forever. That is the banned #426
// anti-pattern in its purest form. Every process blocked in wait() consumed a
// full scheduler slice every time it was picked, so a parent "waiting" for a
// 30-second child burned ~30 seconds of CPU doing nothing but re-reading a
// table that had not changed. The Terminal blocks in sys_waitpid() for the
// whole lifetime of every command it runs, so this was on the single most
// common interactive path in the OS.
//
// Now the parent parks on g_child_exit_wq and consumes nothing.
//
// WAKE SOURCES - deliberately REDUNDANT (CLAUDE.md preference order, option 1:
// an always-armed wake so no wake can ever be lost, rather than a timeout that
// hides a broken waker):
//
//   1. proc_child_exit_notify() is called at EVERY transition to
//      PROC_STATE_ZOMBIE (normal proc_exit, and the corrupted-IRET-frame kill
//      in sched_schedule). This is the precise wake.
//   2. sched_tick() also wakes the queue every CHILD_WQ_SWEEP_TICKS ticks,
//      unconditionally. This is the backstop: if a future exit path is ever
//      added and forgets to call the notify, a waiter is delayed by at most a
//      quarter of a second instead of hanging forever. wake_up_all() on an
//      empty queue is a lock, a NULL test and an unlock, so the cost when
//      nobody is waiting (the normal case) is nothing.
//
// This is exactly the shape hda_space_wq() uses (ISR wake PLUS a 10ms poll
// worker), for exactly the same reason.
//
// The condition below is the SAME predicate the loop body acts on, so we never
// park with work already pending, and the wait_event macro re-tests it after
// queueing us, which closes the lost-wake window against a child exiting
// between our scan and our sleep.
// ===========================================================================
// Wake everyone in wait() - a child somewhere just became reapable.
// Safe from IRQ context and with interrupts already off (proc_exit runs under
// cli()): wake_up_all() takes the queue lock with irqsave.
void proc_child_exit_notify(void) {
    if (!g_child_exit_wq_ready) return;
    wake_up_all(&g_child_exit_wq);
}

// True when proc_wait() has something to DO: either a matching zombie to reap,
// or no matching children left at all (in which case it must return -1 rather
// than sleep forever). Cheap, side-effect free, no locks - it is a wait_event
// condition.
static int child_wait_ready(process_t *parent, int pid) {
    int saw_live = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *c = &proc_table[i];
        if (c->state == PROC_STATE_UNUSED) continue;
        if (c->ppid != parent->pid) continue;
        if (c->shares_vm) continue;                 // #430: threads are not children
        if (c->detached) continue;                  // #745 task 37: kernel workers are not children
        if (pid > 0 && c->pid != (uint32_t)pid) continue;
        if (c->state == PROC_STATE_ZOMBIE) return 1;   // reapable now
        saw_live = 1;
    }
    return saw_live ? 0 : 1;   // no matching children at all -> return -1
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
            // #745 (task 37): nor is a detached kernel worker. Without this, an
            // app that called waitpid(-1) could reap (or block on) an async
            // fetch worker it never created and cannot see.
            if (child->detached) continue;
            if (pid > 0 && child->pid != (uint32_t)pid) continue;  // Wrong child
            
            if (child->state == PROC_STATE_ZOMBIE) {
                // Child has exited - reap it
                int exit_code = child->exit_code;
                if (status) *status = exit_code;
                uint32_t child_pid = child->pid;
                
                // Clean up zombie resources
                if (cleanup_proc_slot(child))   // #167(c): still on a core
                child->state = PROC_STATE_UNUSED;
                
                return (int)child_pid;
            }
        }
        
        // No zombie child found, check if we have any children at all
        bool has_children = false;
        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t *child = &proc_table[i];
            if (child->state != PROC_STATE_UNUSED && child->ppid == current->pid &&
                !child->shares_vm &&  // #430: threads are not wait()-able children
                !child->detached) {   // #745 task 37: nor are kernel workers
                if (pid <= 0 || child->pid == (uint32_t)pid) {
                    has_children = true;
                    break;
                }
            }
        }
        if (!has_children) return -1;  // No matching children

        // #230: BLOCK. Was `sched_schedule();` - a bare yield-spin that made
        // every waiting parent a CPU hog. Non-interruptible on purpose: the
        // old loop ignored signals too, so this changes CPU cost and nothing
        // else. wake_up_process() (signal delivery) still unlinks us; we
        // simply re-scan and re-park, which is what the spin did.
        (void)wait_event(&g_child_exit_wq, child_wait_ready(current, pid));
    }
}


// ---------------------------------------------------------------------------
// #230 A/B BENCHMARK. DEBUG ONLY (-DPROCWAIT_BENCH, `make PROCWAITBENCH=1`).
//
// STATUS 2026-08-02: BUILT, RUN, AND NOT YET TRUSTWORTHY. On its first run
// (build 982, VM <vmid>) it printed 0 ticks for BOTH arms, which is not a result:
// 0 for the legacy spin is impossible over a real 2-second wait, so the harness
// did not actually wait - most likely both proc_wait calls returned -1
// immediately because the freshly proc_create()d kernel thread was not visible
// to the parent as a wait()-able child at that instant. It is left in the tree,
// gated off, as groundwork rather than deleted, but DO NOT quote its numbers
// until it has been fixed to print elapsed ms alongside the tick counts and
// shown to actually block. The #230 CPU claim in the CHANGELOG is therefore
// stated as code-reading, not as a measurement.
//
// The claim "proc_wait() burned a core" needs a number, and the honest way to
// get one is to measure BOTH loops in the SAME build on the SAME box: first a
// faithful re-creation of the pre-#230 scan+sched_schedule() spin, then the new
// wait_event() version, each waiting on an identical 2-second child. The metric
// is the PARENT'S OWN per-process CPU tick counter (total_time, incremented by
// sched_tick for whoever is current), i.e. exactly the per-process CPU sample
// Task Manager shows.
// ---------------------------------------------------------------------------
#ifdef PROCWAIT_BENCH
#define PWB_CHILD_MS 2000
static void pwb_child(void *arg) { (void)arg; proc_sleep(PWB_CHILD_MS); }

// Faithful re-creation of the loop this commit deleted.
static int proc_wait_legacy_spin(int pid, int *status) {
    process_t *current = proc_current();
    if (!current) return -1;
    while (1) {
        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t *child = &proc_table[i];
            if (child->state == PROC_STATE_UNUSED) continue;
            if (child->ppid != current->pid) continue;
            if (child->shares_vm) continue;
            if (pid > 0 && child->pid != (uint32_t)pid) continue;
            if (child->state == PROC_STATE_ZOMBIE) {
                if (status) *status = child->exit_code;
                uint32_t cp = child->pid;
                if (cleanup_proc_slot(child))   // #167(c): still on a core
                child->state = PROC_STATE_UNUSED;
                return (int)cp;
            }
        }
        bool has = false;
        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t *c = &proc_table[i];
            if (c->state != PROC_STATE_UNUSED && c->ppid == current->pid && !c->shares_vm) {
                if (pid <= 0 || c->pid == (uint32_t)pid) { has = true; break; }
            }
        }
        if (!has) return -1;
        sched_schedule();     // THE SPIN
    }
}

void procwait_bench(void) {
    process_t *me = proc_current();
    if (!me) return;
    uint64_t a0, a1, b0, b1;

    proc_create("pwb_child_a", pwb_child, NULL, PRIO_NORMAL);
    a0 = me->total_time;
    (void)proc_wait_legacy_spin(-1, NULL);
    a1 = me->total_time;

    proc_create("pwb_child_b", pwb_child, NULL, PRIO_NORMAL);
    b0 = me->total_time;
    (void)proc_wait(-1, NULL);
    b1 = me->total_time;

    kprintf("[PWBENCH] #230 parent CPU over an identical %ums child wait, "
            "per-process ticks (sched_tick total_time):\n", PWB_CHILD_MS);
    kprintf("[PWBENCH]   pre-#230 scan+sched_schedule() spin : %lu ticks\n",
            (unsigned long)(a1 - a0));
    kprintf("[PWBENCH]   #230 wait_event(g_child_exit_wq)    : %lu ticks\n",
            (unsigned long)(b1 - b0));
}
#endif

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
    // #483/#499: all deadlines below are absolute mono_ms() values (ms of REAL
    // elapsed time), NOT timer_ticks. Sample the monotonic clock ONCE for the
    // whole sweep so a tick burst cannot make deadlines look expired.
    //
    // #167: PUBLISH WHAT THE SWEEP SAW. See the g_sweep_* block above for why
    // the count alone is not enough and what each number rules out.
    g_sweep_n++;
    uint64_t now = sched_now_ms();
    uint64_t __sminleft = ~0ULL;
    uint32_t __sleft = 0, __swoke = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        // #513: alarm(2) deadline sweep, folded into the table walk that was
        // already happening here rather than adding a second O(MAX_PROCESSES)
        // pass. Checked for EVERY live process, not just SLEEPING ones: SIGALRM
        // must fire whatever the target is doing, which is exactly why it needs
        // its own field and cannot ride on wake_time. Disarm BEFORE raising so
        // the one-shot cannot re-fire if sig_raise reschedules us, and so a
        // handler calling alarm() again is not immediately clobbered.
        if (proc_table[i].state != PROC_STATE_UNUSED && proc_table[i].alarm_time != 0 &&
            (int64_t)(now - proc_table[i].alarm_time) >= 0) {   // mono ms, signed: wrap-safe
            proc_table[i].alarm_time = 0;
            extern void sig_raise(process_t *target, int signo);
            sig_raise(&proc_table[i], 14 /* SIGALRM */);
        }
        if (proc_table[i].state == PROC_STATE_SLEEPING) {
            if (now >= proc_table[i].wake_time) {   // mono ms; NEVER=UINT64_MAX stays unreachable
                // ===========================================================
                // #75 EVIDENCE. This is the surviving enqueue site: the
                // forensics have named wake_sleeping_procs across three
                // campaigns with an unchanged signature (enq=READY, pop=READY,
                // now=3, pinned=1). Two passes have now guessed WHICH window
                // inside this function is open and both were wrong, so this
                // records what the code actually SAW when it decided, rather
                // than what I believe it must have seen.
                //
                // Captured at the instant of the decision:
                //   on_cpu   - is the candidate still executing on some core?
                //              non-zero means it is, and the enqueue must be
                //              refused and deferred.
                //   queued   - is it ALREADY linked into a run queue? A double
                //              enqueue would put a task in a queue it should
                //              not be in without anyone breaking an ordering
                //              rule, which is a completely different bug.
                //   wanted   - does it already have an owed (deferred) enqueue?
                //   last_cpu - which core it last ran on, against which core is
                //              running this sweep. Cross-core is the
                //              interesting case; same-core would mean the sweep
                //              is finding a task this very core is running.
                // One-shot per boot, so it cannot flood or perturb timing.
                // ===========================================================
                {
                    process_t *c = &proc_table[i];
                    static int wake_probe_done = 0;
                    if (!wake_probe_done &&
                        (c->sched_on_cpu != 0 || c->rq_queued || c->rq_wanted)) {
                        wake_probe_done = 1;
                        kprintf("[WAKEPROBE] '%s' pid=%u: state=%u on_cpu=%d "
                                "queued=%u wanted=%u last_cpu=%d sweep_cpu=%u "
                                "pinned=%d\n",
                                c->name, c->pid, (uint32_t)c->state,
                                c->sched_on_cpu, (uint32_t)c->rq_queued,
                                (uint32_t)c->rq_wanted, c->last_cpu,
                                sched_rq_cpu(), c->sched_pinned);
                    }
                }
                // #75 (enqrace75) CORRECTION TO THE PROBE ABOVE. Its firing
                // predicate is (on_cpu != 0 || rq_queued || rq_wanted), so it
                // CANNOT observe a candidate that is none of those - and that
                // is precisely the candidate whose enqueue SUCCEEDS, because
                // add_to_ready_queue() only refuses when sched_on_cpu != 0.
                // The earlier pass read its output as "the candidate is ALWAYS
                // still executing (on_cpu non-zero in every capture)". That is
                // true BY CONSTRUCTION of the predicate, not by measurement:
                // the probe instruments only the cases that are correctly
                // refused and deferred, and is blind to every case that
                // actually reaches a run queue. This second one-shot covers the
                // complement, so the pair is no longer circular.
                {
                    process_t *c2 = &proc_table[i];
                    static int wake_probe2_done = 0;
                    if (!wake_probe2_done && c2->sched_on_cpu == 0 &&
                        !c2->rq_queued && !c2->rq_wanted) {
                        wake_probe2_done = 1;
                        kprintf("[WAKEPROBE2] '%s' pid=%u: QUEUEABLE candidate "
                                "state=%u on_cpu=0 queued=0 wanted=0 pinned=%d "
                                "last_cpu=%d sweep_cpu=%u\n",
                                c2->name, c2->pid, (uint32_t)c2->state,
                                c2->sched_pinned, c2->last_cpu, sched_rq_cpu());
                    }
                }
                // #75 (enqrace75b): COUNT BOTH CLASSES ON EVERY SWEEP.
                // [WAKEPROBE] and [WAKEPROBE2] are one-shots: between them they
                // sample two events per boot and cannot say whether the queueable
                // class is one candidate in a thousand or nine hundred of them.
                // The ratio is the whole point, so it is counted, not sampled.
                { process_t *cc = &proc_table[i];
                  if (cc->sched_on_cpu != 0 || cc->rq_queued || cc->rq_wanted)
                      g_wake_cand_busy++;
                  else
                      g_wake_cand_free++; }
                proc_table[i].state = PROC_STATE_READY;
                add_to_ready_queue(&proc_table[i]);
                __swoke++;
            } else {
                // #167: NOT expired by THIS clock. Remember the earliest one so
                // a hung-guest dump can be compared against g_sweep_now without
                // walking proc_table.
                __sleft++;
                if (proc_table[i].wake_time < __sminleft)
                    __sminleft = proc_table[i].wake_time;
            }
        }
    }
    // Sampled in the SAME pass, so the pair is simultaneous by construction:
    // "sched_now_ms() alongside sched_ticks" is only meaningful if the two are
    // read at one instant, and sched_ticks is static to this file.
    g_sweep_now     = now;
    g_sweep_ticks   = sched_ticks;
    g_sweep_woke   += __swoke;
    g_sweep_left    = __sleft;
    g_sweep_minleft = __sminleft;
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
extern void context_start(uint64_t *old_rsp, uint64_t new_rsp, uint64_t new_cr3, void *old_fpu,
                          volatile int32_t *old_release, volatile int32_t *new_unpin);
/* #83: running_cpu=-1 below is already correct under the new semantics - a
   task parked on the migration queue is runnable but running nowhere. */
void smp_migq_push(void *vp){ process_t *p=(process_t*)vp; spinlock_acquire(&migq_lock); p->state=PROC_STATE_READY; p->running_cpu=-1; p->next=migq_head; migq_head=p; spinlock_release(&migq_lock); { extern void smp_wake_aps(void); smp_wake_aps(); } }
void *smp_ap_take_migratable(void){ process_t *p=NULL; spinlock_acquire(&migq_lock); if(migq_head){ p=migq_head; migq_head=p->next; p->next=NULL; p->last_cpu=(int)smp_this_cpu(); p->state=PROC_STATE_RUNNING; } spinlock_release(&migq_lock); return p; }
void smp_ap_run_user(void *vp){ process_t *p=(process_t*)vp; static uint64_t ap_sched_rsp[64]; static uint8_t ap_fpu[64][512] __attribute__((aligned(16))); uint32_t cpu=smp_this_cpu()&63; g_ap_running_user[cpu]=1; sched_rq_set_consumer(cpu,1); kprintf("[SMP] CPU %u now running user proc '%s' (PID %u)\n", cpu, p->name, p->pid); smp_set_current(p); cpu_set_kernel_stack((uint64_t)p->stack_base+p->stack_size); p->total_time=1; context_start(&ap_sched_rsp[cpu], p->rsp, p->cr3, ap_fpu[cpu], (volatile int32_t *)0, &p->sched_pinned); sched_rq_set_consumer(cpu,0); g_ap_running_user[cpu]=0; }

// #421 phase 7: THIS cpu's currently-running process, per-cpu-correct on SMP.
// The scheduler MUST NOT use the single global current_proc for its own
// outgoing (prev) process: when g_smp_user_sched lets an Application Processor
// run a user proc CONCURRENTLY with the BSP, sched_schedule()/sched_tick() on
// each core would read the OTHER core's value of the (globally clobbered)
// current_proc as its own prev, then re-queue a proc that is still RUNNING on
// the other core. That schedules one proc on TWO cores and cycles the ready
// queue, producing a context-switch storm livelock that wedges the whole box
// (both cores spinning in sched_schedule, heartbeat dead, no panic). Measured:
// reproduced on build 912 AND on the first (g_proc_mm_lock-only) 913, both
// cores' RIPs cycling sched_schedule/wake_sleeping_procs/context_switch, a
// ctxsw spike to ~190k/2s, a few seconds after AssaultCube's pthread starts
// running on the AP past map load. Mirrors proc_current()'s own per-cpu gating
// (the per-cpu slot is already maintained by smp_set_current(); it just was not
// READ in the hot scheduling path). Falls back to the global before per-cpu
// data is live / when AP user-scheduling is off, so single-core behaviour is
// byte-for-byte unchanged.
static inline process_t *sched_cpu_current(void) {
    extern int g_smp_user_sched;
    if (g_smp_current_ready && g_smp_user_sched) {
        process_t *p = (process_t *)smp_cpu_current(smp_this_cpu());
        if (p) return p;
    }
    return current_proc;
}

// #143 re-measure: THE PID OF THE TASK THIS CORE IS RUNNING, FOR BKL ATTRIBUTION.
//
// Called from bkl_hold_account() in cpu/smp.c, which runs inside the BKL release
// path with interrupts masked. It therefore MUST NOT take a lock, and it does
// not: sched_cpu_current() is a per-cpu array read with a global fallback.
// Anything added here that can block or spin would be spinning inside the
// release of the giant lock, which is the deadlock shape #130 spent a ticket on.
//
// Returns 0 when there is no current task (very early boot). 0 is a real pid
// here, not an error code, so the caller keys on pid+1 and the report subtracts
// it back; a bucket that means "unattributed" and a bucket that means "pid 0"
// must not share a value (blame.md: NEVER MEASURED and HEALTHY must differ).
uint32_t sched_cpu_current_pid(void) {
    process_t *p = sched_cpu_current();
    return p ? (uint32_t)p->pid : 0;
}

// Name for a pid, for the [BKLPID] report only. Linear scan of the process
// table, called at most three times per ~4 s window from the report path.
const char *sched_pid_name_for_report(uint32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (proc_table[i].state != PROC_STATE_UNUSED && (uint32_t)proc_table[i].pid == pid)
            return proc_table[i].name;
    return "(gone)";
}

void sched_schedule(void) {
    // ======================================================================
    // #610: THE SCHEDULER CRITICAL SECTION MUST NOT BE RE-ENTRANT.
    //
    // Everything below - wake_sleeping_procs(), remove_from_ready_queue(),
    // publishing `next` into current_proc/current_process/smp_set_current(),
    // add_to_ready_queue(prev), and finally context_switch() - used to run
    // with INTERRUPTS ENABLED. sched_schedule() only ever did sti() on its
    // exits; nothing ever did cli() on entry.
    //
    // The timer ISR calls sched_tick() -> sched_schedule(). So a tick landing
    // anywhere in that window RE-ENTERS this function on the SAME cpu, and the
    // nested call reads cur = sched_cpu_current() = the process we had ALREADY
    // PUBLISHED as current, even though the cpu is still physically executing
    // on the OUTGOING process's kernel stack. The nested context_switch then
    // stores the live rsp - a pointer into the OUTGOING process's stack - into
    // the INCOMING process's saved rsp.
    //
    // The result is two processes sharing one kernel stack. Measured directly
    // on build 958 during a 103MB App Store install:
    //   [SCHEDBUG] context_switch: pid=20 'haservice' rsp=0x10d8c088
    //              OUTSIDE kernel stack [0x10d37e00,0x10d47e00)
    //   [SCHEDBUG]   -> rsp is INSIDE pid=21 'AICHAT' stack
    //              [0x10d7cd10,0x10d8cd10) state=4
    // followed by a Double Fault. This is the long-open #446 signature ("both
    // captures had the outgoing and the incoming switch frames on the SAME
    // kernel stack a few hundred bytes apart"); the two stray frames are 0xA0
    // apart, one switch frame.
    //
    // The same window also let an IRQ-context proc_wake() -> add_to_ready_queue()
    // interleave with this function's own ready-queue list surgery.
    //
    // Why the App Store install is what finds it: it drives both a very high
    // context-switch rate and a very high interrupt rate (NIC RX plus block
    // completion) for minutes on end, which is exactly the coincidence needed.
    // It is a race, not a boundary: it landed at ~26, ~108 and ~279 chunks on
    // three runs of the same image.
    //
    // Fix: run the whole thing with interrupts off. That is not a new contract:
    // every exit from this function already ends in sti() (callers such as
    // __wait_event_wait_deadline() deliberately call in with IF=0 and document
    // that "the scheduler restores IF"), so entering with cli() only closes the
    // window and changes nothing a caller could observe.
    // ======================================================================
    cli();

    // #75: CLEAR THE SELECTION AT ENTRY, not only at the exits.
    // Clearing on every RETURN path still leaves one hole: context_start() never
    // returns to its caller, so a C statement after it can never run - which is
    // the same defect that put the pin release in the wrong place. Clearing on
    // ENTRY covers every path unconditionally, including the one that does not
    // come back, because the next pass through this function on this core always
    // reaches here. g_sel is then a true "currently selected" for the interval
    // that matters: pop -> switch.
    SCHED_SEL_CLEAR();

    process_t *cur = sched_cpu_current();
    // #75: ARM OWNERSHIP BEFORE ANYTHING CAN ENQUEUE US. sched_on_cpu used to be
    // set further down, after sched_tick() had already enqueued this task - the
    // guard existed and the enqueue happened before it was armed. Setting it here
    // means any enqueue attempt from this point on, on any core, sees that this
    // task is still executing.
    { uint32_t __ac = sched_rq_cpu();
      if (cur && __ac < MAYTERA_MAX_CPUS) cur->sched_on_cpu = (int32_t)__ac + 1;
      // #75 (enqrace75b): THE WINDOW proc_yield() OPENED CLOSES EXACTLY HERE,
      // on every path, because this is the store that makes the queued task
      // ineligible to sched_rq_pop_locked(). Cleared unconditionally rather
      // than only when this core opened it: a flag that means "currently" and
      // is cleared only on the path that set it eventually means "at some
      // point", which is the g_sel defect recorded earlier in this file.
      { extern volatile uint8_t g_yield_gap[MAYTERA_MAX_CPUS];
        if (__ac < MAYTERA_MAX_CPUS) g_yield_gap[__ac] = 0; } }
    // #75: pay any enqueues owed from the last time this core switched away.
    sched_drain_deferred(sched_rq_cpu());   // #421 p7: per-cpu, NOT global
    if (!preemption_enabled && cur &&
        cur->state == PROC_STATE_RUNNING) {
        // Preemption disabled and current process still running
        SCHED_SEL_CLEAR();   // #75: no selection is live on this core
        if (cur) cur->sched_on_cpu = 0;   /* #75: no switch happened */ 
        sti();
        return;
    }

    // Wake any sleeping processes
    wake_sleeping_procs();

    // #254/#601: promote anything that has been passed over past the bound.
    // Rate-limited internally; on the overwhelming majority of calls this is a
    // single tick comparison.
    sched_age_ready_queue();

    // Get next process from ready queue
    process_t *next = remove_from_ready_queue();
    // #75: this core is now COMMITTED to `next`. Publish that, so the exit path
    // can see it and say whether it is tearing down a task somebody has already
    // picked. Cleared once the switch has happened (or we bail out below).
    { uint32_t __sc = sched_rq_cpu();
      if (__sc < MAYTERA_MAX_CPUS && next) {
          g_sel[__sc] = next;
          g_sel_state[__sc] = (uint32_t)next->state;
          // CANDIDATE 1 probe: a task that is RUNNING has no business being in
          // a run queue at all. If the pop hands one back, that is mechanism 1.
          if (next->state == PROC_STATE_RUNNING) {
              g_sel_run_hits++;
              static int warned_run = 0;
              if (!warned_run) {
                  warned_run = 1;
                  // #167: PRINT WHO QUEUED IT. CANDIDATE 1 fired exactly once
                  // in #165's 55 boots - in arm-gateon/run05, the stock-kernel
                  // run that then panicked at the wild RIP #75 first reported -
                  // and unlike CANDIDATE 2 it did NOT fire on any healthy boot.
                  // It is the discriminating signal for the (c) half, and the
                  // one thing it did not say was WHERE the enqueue came from.
                  // Every field below is already recorded on the task by
                  // add_to_ready_queue(); printing them costs nothing and turns
                  // the next occurrence into an address to resolve.
                  kprintf("[SCHED75] CANDIDATE 1: popped '%s' pid=%u which is "
                          "already RUNNING (on_cpu=%d). A running task must not "
                          "be queued. state_at_enq=%u enq_route=%u "
                          "allowed_hot=%u enq_ra=0x%lx pinned_by=%d "
                          "pinenq=%lu\n",
                          next->name, next->pid, next->sched_on_cpu,
                          next->sched_state_at_enq, (unsigned)next->enq_route,
                          (unsigned)next->enq_allowed_hot,
                          (unsigned long)(uint64_t)next->sched_enq_ra,
                          (int)(next->sched_pinned - 1),
                          (unsigned long)g_enq_pinned);
              }
          }
      } }

    bool no_ready = false;
    if (!next) {
        // No ready processes - fall back to THIS CORE'S idle process.
        //
        // #67 pass 2: this used to be the single global proc_table[0]. Two cores
        // reaching it together would have run one process_t on one kernel stack
        // from two RIPs. A core with no idle process of its own must NOT borrow
        // another core's; it simply does not switch.
        no_ready = true;
        next = sched_idle_for_cpu(sched_rq_cpu());
        if (!next || next->state == PROC_STATE_UNUSED) {
            // #67 pass 12: A "SCHEDULE" THAT DOES NOT SCHEDULE IS A SPIN IN ITS
            // CALLER, and this early return was the reason nothing became
            // runnable under the gate.
            //
            // wait_event() marks the task blocked and then calls here to give up
            // the CPU. Returning without switching hands control straight back
            // to its loop, which re-tests the condition and calls again: a
            // 100%-CPU spin inside the wait primitive, with the task still
            // flagged as blocked so nothing else can be scheduled either.
            //
            // MEASURED on build 255: "top=seclog:99" for a whole 213-second run
            // with flips=0 and boot never completing, and ZERO
            // "[SECLOG-DIAG] worker woke" lines - the worker never came back OUT
            // of wait_event() even once, while burning a core. Both run queues
            // read 0q the whole time, which is what "nothing is becoming
            // runnable" looks like from the scheduler's side.
            //
            // A caller that is still RUNNING asked for a voluntary reschedule
            // and a no-op is correct for it. A caller that has already changed
            // its own state has committed to blocking and MUST NOT be returned
            // to on the same stack. Halt instead: an interrupt (timer, IPI, or
            // the wake it is waiting for) resumes us, and the caller's loop
            // re-tests having cost nothing.
            if (cur && cur->state != PROC_STATE_RUNNING) {
                static int warned_no_idle = 0;
                if (!warned_no_idle) {
                    warned_no_idle = 1;
                    kprintf("[SCHED] cpu %u has NO idle process and '%s' is "
                            "blocked; halting rather than returning into its "
                            "wait loop (#67 pass 12)\n",
                            sched_rq_cpu(), cur->name);
                }
                SCHED_SEL_CLEAR();   // #75
                if (cur) cur->sched_on_cpu = 0;   /* #75: no switch happened */ 
                sched_halt_bkl_note(1, 0);        // #75
                __asm__ volatile("sti; hlt");
                return;
            }
            SCHED_SEL_CLEAR();   // #75
            if (cur) cur->sched_on_cpu = 0;   /* #75: no switch happened */ 
            sti();
            return;
        }
    }

    // If same process, just continue
    if (next == cur) {                      // #421 p7: per-cpu prev
        if (next) next->sched_pinned = 0;   // #75: not switching; drop the pin
        SCHED_SEL_CLEAR();                  // #75: and no selection is live
        if (cur) cur->sched_on_cpu = 0;   /* #75: no switch happened */ 
        cur->time_slice = TIME_SLICE_TICKS;
        cur->state = PROC_STATE_RUNNING;
        if (no_ready) {
            sched_halt_bkl_note(2, 0);   // #75
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
    process_t *prev = cur;                  // #421 p7: per-cpu prev, NOT global
    // #67: CLAIM OWNERSHIP OF prev's CONTEXT until the switch asm has actually
    // saved it. Between here and the `mov [rdi], rsp` inside
    // context_switch/context_start, prev->rsp still holds the value from the
    // PREVIOUS time prev was descheduled. add_to_ready_queue(prev) below (and
    // any concurrent proc_wake()) can publish prev where another core can see
    // it, and switching to a stale rsp puts two cores on one kernel stack. The
    // asm clears this flag after the save (see proc/context_switch.asm), and
    // sched_rq_pop() refuses any entry whose flag is set. x86 does not reorder
    // stores, so observing 0 implies observing the final rsp.
    // #75: the field is DECODED as (cpu id + 1) by add_to_ready_queue(), which
    // does `owner = sched_on_cpu - 1` and indexes the per-core defer table with
    // it. A literal 1 therefore named CORE 0 from every core. See g_enq_cpu_fix.
    if (prev) {
        uint32_t __pc = sched_rq_cpu();
        prev->sched_on_cpu = (g_enq_cpu_fix && __pc < MAYTERA_MAX_CPUS)
                             ? (int32_t)__pc + 1 : 1;
    }
    // #67 diagnostic: always on, both gate states. See sched_storm_note().
    sched_storm_note(sched_rq_cpu(), prev, next);
    // #67 pass 2: the per-cpu slot is authoritative once more than one core
    // schedules. current_proc/current_process are read all over the kernel by
    // code that predates SMP; letting an AP write them would make the BSP's own
    // view of "the current process" flip to whatever the AP just picked.
    // proc_current() and sched_cpu_current() already prefer the per-cpu slot
    // when the gate is on, so the globals stay the BSP's answer.
    smp_set_current(next);  // #279 3b-3: mirror into this cpu's per-cpu slot
    { extern int g_smp_user_sched;
      if (!g_smp_user_sched || sched_rq_cpu() == 0) {
          current_proc = next;
          current_process = next;
      } }
    next->state = PROC_STATE_RUNNING;
    next->time_slice = TIME_SLICE_TICKS;

    // Re-queue prev if it was RUNNING (voluntary yield). Callers that want to
    // block prev (e.g. wait-queue sleep, proc_exit) set a different state
    // before calling sched_schedule, so they are not affected.
    if (prev && prev != next && prev->state == PROC_STATE_RUNNING) {
        sched_note_mutator(prev, PROC_STATE_READY, __builtin_return_address(0),
                           (void *)0);   // #75 evidence 3
        prev->state = PROC_STATE_READY;
        // #601: the idle process is the FALLBACK, chosen explicitly above when
        // the queue is empty. It must never occupy a slot in the ready queue:
        // at PRIO_NORMAL it sorted ahead of every PRIO_LOW thread and blocked
        // them for ever.
        // #75: prev is STILL EXECUTING here - we have not switched yet - so this
        // goes through the same refusal path as every other enqueue and is paid
        // when this core next enters the scheduler, after the switch.
        if (!prev->is_idle) add_to_ready_queue(prev);   // #67: never queue an idle proc
    }

    // #75: THE TASK DIED WHILE WE HELD IT SELECTED. Exit waits for our pin, so
    // this can only be reached if that wait timed out. Abandon the switch rather
    // than resuming a dead task: drop the pin, let go of `next`, and take the
    // idle process instead. This is a recovery and not a test - by this point
    // the task's state cannot change back to runnable.
    if (next && next != cur &&
        next->state != PROC_STATE_RUNNING && next->state != PROC_STATE_READY) {
        kprintf("[SCHED75] abandoning switch to '%s' pid=%u: state=%u, it died "
                "while selected\n", next->name, next->pid, (uint32_t)next->state);
        next->sched_pinned = 0;
        SCHED_SEL_CLEAR();
        if (cur) cur->sched_on_cpu = 0;   /* #75: no switch happened */ 
        sti();
        return;
    }
    // #75 CORRECTED: the release used to be HERE, justified by "from here
    // sched_on_cpu protects the context". That was FALSE - sched_on_cpu is set
    // on PREV, and protects the OUTGOING task until the switch asm has saved it.
    // It says nothing about the INCOMING task, so releasing next's pin here left
    // exactly the window the corruption was measured in. The release now happens
    // INSIDE the switch assembly, next to the sched_on_cpu store: the only point
    // that runs on BOTH the context_switch and context_start paths (a C release
    // after the call cannot, because context_start never returns) and the only
    // point that knows the switch has actually completed.
    //
    // This is the first time the two pins are genuinely symmetric:
    //   sched_on_cpu  protects the OUTGOING task until the asm has SAVED it;
    //   sched_pinned  protects the INCOMING task until the asm has SWITCHED to it.

    // #75 REPRODUCER. `next` is now published as this core's current process and
    // `prev` has been queued, but the switch has not happened. This is the gap
    // the #421 comment describes and the one that survived the #67 handoff fix,
    // so it is both widened and validated here.
    {
        uint32_t __sc = sched_rq_cpu();
        schedrace_delay(SR_SITE_AFTER_PUBLISH);
        schedrace_note(__sc, prev, next);
        schedrace_check(__sc, prev, next, "pre-switch");
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
                sched_rq_remove(next);   // #75: and out of every run queue
                next->state = PROC_STATE_ZOMBIE;
                next->sched_pinned = 0;     // #75: never switched to it
                SCHED_SEL_CLEAR();          // #75
                if (cur) cur->sched_on_cpu = 0;   /* #75: no switch happened */ 
                proc_child_exit_notify();   // #230: this is a child dying too
                sti();   // #610: every exit from sched_schedule() re-enables IF
                return;
            }
        }
        next->total_time = 1;
        proc_check_switch_target(next, "context_start");
        if (prev && prev->state != PROC_STATE_UNUSED) {
            proc_check_switch_target(prev, "context_start/prev");
            { sched_publish_cpu(prev, next, sched_rq_cpu());   /* #83, IF=0 here */
              uint32_t __bd = bkl_release_all(); schedrace_delay(SR_SITE_AFTER_BKL_DROP); context_start(&prev->rsp, next->rsp, next->cr3, prev->fpu_area, &prev->sched_on_cpu, &next->sched_pinned); bkl_reacquire(__bd); }
        } else {
            static uint64_t dummy_rsp;
            { sched_publish_cpu((process_t *)0, next, sched_rq_cpu());   /* #83: no outgoing task on this path, same as the NULL release pointer */
              uint32_t __bd = bkl_release_all(); schedrace_delay(SR_SITE_AFTER_BKL_DROP); context_start(&dummy_rsp, next->rsp, next->cr3, g_dummy_fpu_area, (volatile int32_t *)0, &next->sched_pinned); bkl_reacquire(__bd); }
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

        proc_check_switch_target(next, "context_switch");
        if (prev && prev->state != PROC_STATE_UNUSED) {
            { sched_publish_cpu(prev, next, sched_rq_cpu());   /* #83, IF=0 here */
              uint32_t __bd = bkl_release_all(); schedrace_delay(SR_SITE_AFTER_BKL_DROP); context_switch(&prev->rsp, next->rsp, prev->fpu_area, next->fpu_area, &prev->sched_on_cpu, &next->sched_pinned); bkl_reacquire(__bd); }
        } else {
            static uint64_t dummy_rsp;
            { sched_publish_cpu((process_t *)0, next, sched_rq_cpu());   /* #83: no outgoing task on this path */
              uint32_t __bd = bkl_release_all(); schedrace_delay(SR_SITE_AFTER_BKL_DROP); context_switch(&dummy_rsp, next->rsp, g_dummy_fpu_area, next->fpu_area, (volatile int32_t *)0, &next->sched_pinned); bkl_reacquire(__bd); }
        }
    }

    // #75: the switch is done; this core is no longer committed to `next`.
    SCHED_SEL_CLEAR();

    // After returning from context switch, re-enable interrupts
    sti();
}

/**
 * Timer tick handler for scheduler
 * Called from timer interrupt
 */
// #745 (#62): how many times sched_tick() has RUN, which is the rate at which
// per-process CPU time is sampled (`cur->total_time++` below, and the thread
// equivalent in proc/thread.c). It is NOT the same as timer_ticks once the #62
// redundant tick source is carrying the system: that source fires at 50 Hz and
// advances timer_ticks by however many 250 Hz ticks REAL TIME says have
// elapsed, so timer_ticks keeps correct time while sched_tick runs five times
// less often. A CPU percentage computed as (process ticks / timer_ticks delta)
// then under-reports by exactly that ratio - MEASURED on VM <vmid> with IRQ0
// deliberately masked, where a genuinely idle machine reported top=idle:19
// instead of idle:98, which is 98/5. The heartbeat's top= field divides by
// THIS instead, so the percentage is correct under either tick source.
volatile uint64_t g_sched_tick_samples = 0;

void sched_tick(void) {
    g_sched_tick_samples++;
    sched_ticks++;
    // #230 BACKSTOP WAKE. proc_child_exit_notify() at every ZOMBIE transition
    // is the precise wake; this is the redundant always-armed one, so a future
    // exit path that forgets to notify costs a waiter a quarter second rather
    // than hanging it forever (CLAUDE.md preference order option 1: make the
    // wake impossible to lose instead of covering the gap with a timeout).
    // wake_up_all() on an empty queue is a lock, a NULL test and an unlock.
    #define CHILD_WQ_SWEEP_TICKS 64
    if ((sched_ticks % CHILD_WQ_SWEEP_TICKS) == 0) proc_child_exit_notify();
    { extern void proc_mm_lock_watchdog(void); proc_mm_lock_watchdog(); } // #421 p7
    { extern void cron_tick(void); cron_tick(); }  // #265 scheduler hook
    { extern void futex_tick(void); futex_tick(); } // #430 futex timeout hook
    // #167: an IRQ-context wake into the block window, so the block/wake race is
    // reachable at 1 vCPU as well as 4. No-op unless /WAKELOSS.TXT is present.
    { extern void wakeloss_tick(void); wakeloss_tick(); }
    g_cpu_total_acc++;
    process_t *cur = sched_cpu_current();   // #421 p7: per-cpu, NOT global
    if (!cur || cur->is_idle) g_cpu_idle_acc++;   // #67: per-core idle, not pid 0
    // #67: PER-CORE busy accounting. The aggregate above cannot distinguish
    // "one core saturated" from "all cores half loaded", which is exactly the
    // reading that opened this ticket.
    { uint32_t __c = sched_rq_cpu();
      if (cur && !cur->is_idle && __c < MAYTERA_MAX_CPUS) g_core_busy[__c]++; }
    // #67 pass 2: one core owns the report; its statics are not shared state.
    if (sched_rq_cpu() == 0) sched_smp_report();

    // #67 pass 2: consume a cross-core preemption request. Placement asks for
    // this when a process lands on a core that is running something it
    // outranks, so the higher-priority process starts within a tick instead of
    // waiting out the current time slice.
    // #67 pass 7, CORRECTED: this block used to `return` after rescheduling,
    // and that return is BEFORE the time-slice accounting further down. Whenever
    // a cross-core preemption request was pending, sched_tick() therefore skipped
    // `cur->time_slice--` entirely. With requests arriving regularly the slice
    // never reached zero, so ordinary round-robin preemption STOPPED and the only
    // context switches left were the explicit ones. MEASURED on build 253:
    // ctxsw=557 in 42 s of guest uptime (about 13/s), against roughly 350/s on
    // the single-core path, with two processes sitting in the run queue the whole
    // time. A scheduler that stops preempting looks exactly like a slow machine.
    //
    // The flag is now consumed by SETTING THE SLICE TO ZERO and falling through
    // to the normal path, so the preemption happens through the one piece of
    // code that already knows how to do it correctly.
    { uint32_t __rc = sched_rq_cpu();
      if (__rc < MAYTERA_MAX_CPUS && g_need_resched[__rc]) {
          g_need_resched[__rc] = 0;
          if (cur && !cur->is_idle) cur->time_slice = 0;   // expire it now
      } }
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

    if (!cur || !preemption_enabled) {
        return;
    }

    // Update time
    cur->total_time++;

    // Decrement time slice
    if (cur->time_slice > 0) {
        cur->time_slice--;
    }

    // If time slice expired, reschedule
    // Allow ALL processes including PID 0 to be preempted
    if (cur->time_slice == 0) {

        // #75: DO NOT ENQUEUE THE TASK WE ARE STILL RUNNING ON. This site is
        // queued_by=0x5986e8 in the forensics and is half the root cause: it put
        // `cur` on a run queue and only then called the scheduler, leaving it
        // queued-and-running for the whole interval in between.
        //
        // sched_schedule() already re-queues `prev` correctly - it tests
        // prev->state == PROC_STATE_RUNNING and requeues from there - so setting
        // the state to READY here was ALSO what stopped it doing so, which is
        // why this site had to enqueue by hand in the first place. Leaving the
        // state alone hands the whole job to the one place that knows when the
        // switch has actually happened.
        sched_schedule(); // Run new process
    }
}

// ===========================================================================
// #169: THE SCHEDULER TICK TAKEN ON AN APPLICATION PROCESSOR.
//
// Called from ap_preempt_tick_handler() (cpu/isr.c) on the AP's own LAPIC timer
// at the same rate as the BSP's PIT tick. See that file for why the PIT stays
// the global clock and this is a preemption-only tick.
//
// WHAT IT DELIBERATELY DOES NOT DO, and why each one would be a bug:
//   timer_ticks            four writers => the wall clock runs 4x fast and every
//                          `timer_ticks + N` deadline in the tree fires early.
//   sched_ticks            the divisor of every rate in sched_smp_report() and
//                          the base of the #254 starvation bound.
//   g_sched_tick_samples   the heartbeat's top= divisor; 4x it and every process
//                          CPU percentage under-reports by 4x.
//   g_cpu_idle_acc/_total  the aggregate behind g_cpu_pct: mixing four cores'
//                          samples into a counter that resets every 250 makes
//                          the taskbar gauge meaningless.
//   cron_tick, futex_tick, proc_child_exit_notify, proc_mm_lock_watchdog,
//   wakeloss_tick, sched_smp_report
//                          once-per-tick GLOBAL hooks. Four callers = four times
//                          the rate, which for the sweeps is wasted work and for
//                          anything that counts ticks is simply wrong.
//
// The DECISION is rustkern/aptick.rs; everything here is the mutation it
// authorises, in the order the C side owns.
//
// IRQ-CONTEXT RULE (#426): nothing here waits. No lock is taken, no queue is
// polled. The one call that can block the core is sched_schedule(), which is
// what the BSP's timer tick already does from the identical context.
// ===========================================================================
void sched_tick_ap(void) {
    uint32_t cpu = sched_rq_cpu();
    // Never on the BSP. sched_rq_cpu() also returns 0 before per-cpu GS is
    // final, so this doubles as the "too early to be trusted" guard.
    if (cpu == 0 || cpu >= MAYTERA_MAX_CPUS) return;

    g_ap_ticks[cpu]++;      // counted FIRST, so "the timer fires" is provable
                            // even on a core that never has anything to run.

    process_t *cur = sched_cpu_current();

    uint32_t new_slice = 0;
    uint32_t act = ap_tick_decide_rs(preemption_enabled ? 1u : 0u,
                                     cur ? 1u : 0u,
                                     (cur && cur->is_idle) ? 1u : 0u,
                                     g_need_resched[cpu] ? 1u : 0u,
                                     cur ? (uint32_t)cur->time_slice : 0u,
                                     &new_slice);

    if (act & APTICK_BUSY)     g_core_busy[cpu]++;
    if (act & APTICK_ACK_RESC) g_need_resched[cpu] = 0;
    if (!cur) return;
    if (act & APTICK_CHARGE)   cur->total_time++;
    if (act & APTICK_SETSLICE) cur->time_slice = new_slice;
    if (act & APTICK_SCHED) {
        g_ap_preempts[cpu]++;
        // Does not return to this frame when it switches; the EOI is already
        // sent (cpu/isr.c), so nothing is owed to the LAPIC from here.
        sched_schedule();
    }
}

// #171: "HAS THE SCHEDULER STARTED" AND "IS PREEMPTION SUPPRESSED RIGHT NOW"
// ARE DIFFERENT QUESTIONS, AND preemption_enabled ONLY ANSWERS THE SECOND.
//
// preemption_enabled is false for two completely unrelated reasons. Before
// proc_init() it is false because the scheduler has never run and there is
// nothing to switch to. After that it goes false and true again, for a few
// microseconds at a time, every time a caller brackets a critical section with
// sched_set_preemption(false) ... sched_set_preemption(old) - proc_create_user()
// and the spawn path both do.
//
// sync/noblock.c used sched_preemption_enabled() as its proxy for the FIRST
// question, which is only defensible while exactly one core runs threads. With
// the #67 gate on, three more cores are running threads that have nothing to do
// with the BSP's critical section, and they read a global that is momentarily
// false because of it. MEASURED (#171): 0 to 3 spurious
// "[WQBLOCK] ... [SCHEDULER-NOT-LIVE]" reports per gate-ON boot and exactly
// zero per gate-OFF boot, across 17 boots, naming innocent third-party threads
// ('condrain', 'seclog', and once the Ring-3 'SETUP' process) - never the
// thread that was inside the critical section. addr2line on the five distinct
// reported call sites gives console_drain_worker (serial.c:768),
// sys_win_get_event (proc/syscall.c:6470, i.e. an ordinary Ring-3 app waiting
// for its next window event), seclog_worker and seclog_pending
// (security/seclog.c:275,113) and ext2_lock_at (fs/ext2.c:198). Five routine
// blocking waits, none of them inside a critical section of its own.
//
// The window they collide with is not hypothetical either: sys_spawn brackets
// an ENTIRE ELF LOAD in sched_set_preemption(false) (proc/syscall.c:3258..3325),
// which is milliseconds, and proc_create_user()/proc_create_ex() bracket their
// own setup. With the gate off those windows are invisible because the one core
// that runs threads is the one inside the bracket. With the gate on, three more
// cores are running ordinary threads straight through them.
//
// They are FALSE POSITIVES, and that is a claim about the code rather than an
// inference from who got named: __wait_prepare() sets PROC_STATE_BLOCKED via
// sched_self_block() BEFORE __wait_event_wait() calls sched_schedule(), so the
// "preemption off and the current task is still RUNNING" early return in
// sched_schedule() is not taken, the waiter really is switched away, and
// proc_wake() -> add_to_ready_queue() can still reach it. Blocking there is
// safe. The assertion was reporting a hazard that does not exist.
//
// WHY IT MATTERS ENOUGH TO FIX BEFORE THE GATE COULD SHIP. wq_assert_may_block()
// is the ONLY live runtime guard for the #426 bug class, and #514's whole
// finding was that a guardrail nobody trusts is a guardrail that is not there.
// Turning the gate on would put two or three of these in every shipping boot
// log, and the first thing anyone would learn is to ignore the line.
//
// THE FIX IS A STICKY FLAG, not a cleverer read of the volatile one: once the
// scheduler has been enabled even once, it is live for the rest of the boot.
// Nothing ever un-starts it.
static bool sched_ever_live = false;

/**
 * Enable/disable preemption
 */
bool sched_set_preemption(bool enable) {
    bool old = preemption_enabled;
    preemption_enabled = enable;
    if (enable) sched_ever_live = true;
    return old;
}

// #171: the durable "the scheduler exists and can switch" predicate. Distinct
// from sched_preemption_enabled(), which is the momentary suppressor and which
// every existing caller (xhci, usb_msc) genuinely wants, because those callers
// are asking "will proc_sleep() make progress right now", not "may I park".
bool sched_is_live(void) { return sched_ever_live; }

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
// setup_user_stack: write argc/argv/envp onto a user-mode stack in a foreign
// address space (target_cr3).  Returns the new user RSP value, or 0 on
// refusal (the caller must abandon the spawn).
//
// Uses a temporary CR3 switch to write directly to user virtual addresses.
// The kernel stack and code are in kernel upper-half mappings (PML4 256-511)
// which are shared between all address spaces, so they remain valid.
//
// #112: THIS FUNCTION USED TO BE setup_user_argv() AND WROTE NO ENVIRONMENT.
// It reserved the slot and labelled it "envp terminator (future)", which is
// why every MayteraOS process started with an empty `environ` and why
// /APPS/ENV refused NAME=VALUE. The slot is now filled.
//
// Stack layout, high address to low (SysV x86-64 initial process stack, and
// exactly what libc/crt0.S now reads):
//
//   High addr:
//     <env strings>         envc NUL-terminated "NAME=VALUE" strings
//     <argv strings>        argc NUL-terminated strings
//     <padding to 8-byte>
//     NULL                  envp terminator
//     &envp[envc-1] .. &envp[0]
//     NULL                  argv terminator
//     &argv[argc-1] .. &argv[0]
//     argc (uint64_t)       <-- RSP points here
//   Low addr (16-byte aligned)
//
// THE ARITHMETIC IS IN RUST (rustkern/envblock.rs), the stores are here. The
// split is deliberate and is the fsperm.rs / spawnid.rs pattern: the sizes are
// Ring-3 chosen and a wrapped total is how a 16KB block becomes an 8-byte
// reservation, so every length computation is a checked_add/checked_mul over
// there. The stores stay in C because they run inside a foreign-CR3 window
// with interrupts masked and SMAP AC set, and reimplementing that machinery in
// Rust would fork a primitive instead of reusing one.
//
// `argv` and `envp` are KERNEL arrays of KERNEL strings. Every caller has
// already bounced them (spawn_impl copies each Ring-3 string into a kernel
// buffer). This function never dereferences a Ring-3 pointer.
// ============================================================================

// rustkern/envblock.rs. Keep these in step with the Rust signatures; the
// symbol names are locked by kernel/rust-symbols.manifest.
typedef struct {
    uint64_t sp;
    uint64_t argv_slots;
    uint64_t envp_slots;
    uint64_t argv_strings;
    uint64_t env_strings;
    uint64_t total;
} rs_stack_plan_t;

extern int32_t user_stack_plan_rs(uint64_t stack_top, uint64_t stack_size,
                                  uint32_t argc, uint64_t argv_bytes,
                                  uint32_t envc, uint64_t env_bytes,
                                  rs_stack_plan_t *out);
extern int32_t env_entry_len_rs(const uint8_t *p, uint64_t cap);
extern const uint8_t *env_default_rs(uint32_t idx);
extern uint32_t envblock_selftest_rs(void);

#define SUS_MAX_ENTRIES 64
#define SUS_ARG_CAP     256    // spawn_impl's per-argument kernel buffer
#define SUS_ENV_CAP     512    // ENV_MAX_ENTRY in envblock.rs

// Run once at boot so the layout policy is proven LIVE on this build rather
// than merely compiled in. Loud on purpose: a non-zero mask means the
// arithmetic that bounds a Ring-3-sized allocation is wrong.
void envblock_selftest(void) {
    uint32_t fails = envblock_selftest_rs();
    if (fails == 0) kprintf("[ENVBLK] stack/env policy self-test PASS (rust)\n");
    else            kprintf("[ENVBLK] stack/env policy self-test FAIL mask=0x%x\n", fails);
}

// The environment a process the KERNEL launches starts with. See the
// ENV_DEFAULTS block in rustkern/envblock.rs for why it is this short and why
// HOME and USER are deliberately absent. Fills `slots` and returns the count.
static int env_kernel_defaults(const char **slots, int cap) {
    int n = 0;
    while (n < cap) {
        const uint8_t *e = env_default_rs((uint32_t)n);
        if (!e) break;
        slots[n++] = (const char *)e;
    }
    return n;
}

static uint64_t setup_user_stack(uint64_t target_cr3, uint64_t stack_top,
                                 int argc, char **argv,
                                 int envc, char **envp) {
    // ---- Phase 1: measure, in kernel memory, before touching the child -----
    if (argc < 0 || !argv) argc = 0;
    if (argc > SUS_MAX_ENTRIES) argc = SUS_MAX_ENTRIES;

    uint64_t arg_len[SUS_MAX_ENTRIES];
    uint64_t argv_bytes = 0;
    for (int i = 0; i < argc; i++) {
        const char *s = argv[i] ? argv[i] : "";
        uint64_t len = 0;
        while (len < SUS_ARG_CAP - 1 && s[len]) len++;
        arg_len[i] = len + 1;               // with the NUL
        argv_bytes += arg_len[i];
    }

    // envc < 0 means "no environment was supplied": use the kernel defaults,
    // which is what a process the kernel itself launches gets. envc == 0 with
    // a NULL envp is a DIFFERENT request - a deliberately EMPTY environment,
    // which is what `env -i` means - and it is honoured as such.
    const char *defslots[SUS_MAX_ENTRIES];
    const char **env = (const char **)envp;
    if (envc < 0) {
        envc = env_kernel_defaults(defslots, SUS_MAX_ENTRIES);
        env  = defslots;
    }
    if (envc > SUS_MAX_ENTRIES) envc = SUS_MAX_ENTRIES;
    if (envc > 0 && !env) envc = 0;

    uint64_t env_len[SUS_MAX_ENTRIES];
    uint64_t env_bytes = 0;
    for (int i = 0; i < envc; i++) {
        // A malformed entry REFUSES THE SPAWN. It is not dropped: an entry
        // with no '=' can never be found by getenv(), so carrying it would
        // consume a slot and read back as absent, and dropping it silently
        // would run the child with an environment the caller did not ask for.
        int32_t n = env_entry_len_rs((const uint8_t *)(env[i] ? env[i] : ""),
                                     SUS_ENV_CAP);
        if (n < 0) return 0;
        env_len[i] = (uint64_t)n;
        env_bytes += env_len[i];
    }

    rs_stack_plan_t plan;
    if (user_stack_plan_rs(stack_top, USER_STACK_SIZE,
                           (uint32_t)argc, argv_bytes,
                           (uint32_t)envc, env_bytes, &plan) != 0) {
        return 0;
    }

    // ---- Phase 2: switch to the target address space and write everything --
    // Mask interrupts across the foreign-CR3 window (see elf_load_user): an IRQ
    // handler must never run while CR3 points at the child address space.
    uint64_t old_cr3, rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags) :: "memory");
    __asm__ volatile("cli");
    __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(target_cr3) : "memory");

    // #19/#645: from here to the matching uaccess_end() every dereference of a
    // plan address is a store to the child's USER stack (U/S=1), which #PFs
    // under CR4.SMAP without AC. The bracket spans the STORES, not the
    // function: the size arithmetic above ran before it and the CR3 restore
    // runs after it. Reads of argv[i]/env[i] inside are KERNEL memory and are
    // unaffected by AC.
    uaccess_ac_t __ac = uaccess_begin();

    // argc
    *(volatile uint64_t *)plan.sp = (uint64_t)argc;

    // argv strings, then their pointers
    uint64_t cur = plan.argv_strings;
    for (int i = 0; i < argc; i++) {
        *(volatile uint64_t *)(plan.argv_slots + (uint64_t)i * 8) = cur;
        const char *s = argv[i] ? argv[i] : "";
        for (uint64_t j = 0; j + 1 < arg_len[i]; j++)
            *(volatile uint8_t *)(cur + j) = (uint8_t)s[j];
        *(volatile uint8_t *)(cur + arg_len[i] - 1) = 0;
        cur += arg_len[i];
    }
    *(volatile uint64_t *)(plan.argv_slots + (uint64_t)argc * 8) = 0;  // argv NULL

    // env strings, then their pointers
    cur = plan.env_strings;
    for (int i = 0; i < envc; i++) {
        *(volatile uint64_t *)(plan.envp_slots + (uint64_t)i * 8) = cur;
        const char *s = env[i] ? env[i] : "";
        for (uint64_t j = 0; j + 1 < env_len[i]; j++)
            *(volatile uint8_t *)(cur + j) = (uint8_t)s[j];
        *(volatile uint8_t *)(cur + env_len[i] - 1) = 0;
        cur += env_len[i];
    }
    *(volatile uint64_t *)(plan.envp_slots + (uint64_t)envc * 8) = 0;  // envp NULL

    uaccess_end(__ac);

    // Switch back to kernel address space
    __asm__ volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
    __asm__ volatile("push %0; popfq" : : "r"(rflags) : "cc", "memory");

    return plan.sp;
}


// ---------------------------------------------------------------------------
// #692: spawn identity. The POLICY is rustkern/spawnid.rs; these are the two
// C data shims it calls, and the one C entry point the spawn path uses.
//
// Keeping the decision in Rust and the data access in C is the fsperm.rs
// pattern: the PASSWD table and the process table own their layouts and stay
// C, while the rule ("gid follows uid", "an unset identity is refused", "a
// kernel thread is not an identity to inherit") lives in one auditable place.
// ---------------------------------------------------------------------------

// Returns 0 and fills uid/gid iff there is a CURRENT process and it is Ring 3.
// A kernel thread returns non-zero and writes nothing: that is the whole point,
// because inheriting from a kernel thread is what silently produced uid 0.
int spawnid_caller_ident(uint32_t *uid_out, uint32_t *gid_out) {
    process_t *cur = proc_current();
    if (!cur || cur->privilege != PRIV_USER) return -1;
    *uid_out = cur->euid;
    *gid_out = cur->egid;
    return 0;
}

// user_lookup_uid(uid)->gid, or 0xFFFFFFFF when the uid has no PASSWD entry.
// The Rust side turns that sentinel into gid == uid; it must NEVER become 0.
uint32_t spawnid_gid_for_uid(uint32_t uid) {
    extern user_entry_t *user_lookup_uid(uint32_t uid);
    user_entry_t *u = user_lookup_uid(uid);
    return u ? u->gid : 0xFFFFFFFFu;
}

extern int spawn_ident_resolve_rs(uint32_t kind, uint32_t want_uid,
                                  uint32_t *out_uid, uint32_t *out_gid);
extern uint32_t spawn_ident_selftest_rs(void);

// Run once at boot so the policy is proven LIVE on this build rather than
// merely compiled in. Prints one line either way; a non-zero mask is a bug in
// the policy itself and is loud on purpose.
void spawn_ident_selftest(void) {
    uint32_t fails = spawn_ident_selftest_rs();
    if (fails == 0) kprintf("[SPAWNID] policy self-test PASS (rust)\n");
    else            kprintf("[SPAWNID] policy self-test FAIL mask=0x%x\n", fails);
}

int proc_create_user_as(const char *name, void *elf_data, uint64_t elf_size,
                        char **argv, char **envp, proc_ident_t ident) {
    // #112: envp is a KERNEL array of KERNEL "NAME=VALUE" strings, or NULL.
    //
    // NULL and an EMPTY VECTOR are different requests and are answered
    // differently. NULL means "nobody supplied an environment", which is the
    // case for every process the KERNEL launches (the compositor from
    // gui/desktop.c, a service, a cron job): those get the small default block
    // in rustkern/envblock.rs, so the inheritance tree has a root. A non-NULL
    // envp whose first element is NULL means "an empty environment on
    // purpose", which is what "env -i" asks for, and it is honoured.
    //
    // Until this ticket the whole parameter was a single (void)envp cast.
    int envc = -1;
    if (envp) { envc = 0; while (envp[envc] && envc < 64) envc++; }

    // #692: RESOLVE THE IDENTITY FIRST, and abandon the spawn if it cannot be
    // resolved. This runs before anything is allocated, so a refusal costs
    // nothing and leaks nothing. It is deliberately not recoverable: a process
    // whose uid nobody chose must not start, because the only "safe" fallback
    // available (inherit from the caller) is precisely the bug being removed.
    uint32_t new_uid = 0, new_gid = 0;
    int idrc = spawn_ident_resolve_rs(ident.kind, ident.uid, &new_uid, &new_gid);
    if (idrc != 0) {
        kprintf("[PROC] REFUSED spawn of '%s': unresolvable identity "
                "(kind=%u uid=%u rc=%d)\n", name, ident.kind, ident.uid, idrc);
        LOG_ERROR("[Process] spawn refused: no resolvable identity");
        return -1;
    }

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

    // #692: OVERWRITE the credentials init_proc() inherited from
    // proc_current(). All four fields are set together, from one resolved
    // pair, so uid/gid can no longer disagree the way services.c's
    // uid-only stamp made them (uid 0 with gid 1000).
    proc->uid  = new_uid;
    proc->gid  = new_gid;
    proc->euid = new_uid;
    proc->egid = new_gid;

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
    scp_span_t __sus = scp_begin();   // #121
    int __usr = vmm_alloc_user_pages(proc->cr3, proc->user_stack_base, stack_pages,
                                     VMM_USER_RW);
    scp_end(SCP_VMMSTACK, __sus);
    if (__usr != 0) {
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
    // #640: pass the process name so a loader rejection names the app rather
    // than only an address (the four 0x400000 binaries of #633 were found by
    // measurement, not by anyone reading a log line).
    scp_span_t __sel = scp_begin();   // #121
    int elf_result = elf_load_user_named(elf_data, elf_size, proc->cr3,
                                         &entry_point, &load_base, &load_end, name);
    scp_end(SCP_ELFLOAD, __sel);
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

    // #COMPRESPAWN: keep the base. Under PIE + ASLR this is the ONLY thing that
    // makes a faulting RIP resolvable to a source line later; see the field's
    // comment in process.h. It costs two stores on a cold path.
    proc->image_base = load_base;
    proc->image_end  = load_end;

    // Set up user mode entry point and stack
    proc->user_rip = entry_point;

    // Set up user stack with argc/argv/envp
    int argc = 0;
    if (argv) { while (argv[argc] && argc < 64) argc++; }
    uint64_t user_sp = setup_user_stack(proc->cr3, USER_STACK_TOP,
                                        argc, argv, envc, envp);

    // #112: a refused layout ABANDONS THE SPAWN. setup_user_stack() returns 0
    // when the environment does not fit its budget or an entry is not
    // NAME=VALUE. Starting the child anyway would give it RSP 0 and a #PF on
    // its first instruction, which is a far more confusing failure than a
    // spawn that did not happen.
    if (user_sp == 0) {
        kprintf("[PROC] %s: refused initial stack (argc=%d envc=%d)\n",
                name, argc, envc);
        vmm_free_user_pages(proc->cr3, proc->user_stack_base, stack_pages);
        kfree(proc->stack_base);
        vmm_destroy_user_space(proc->cr3);
        proc->state = PROC_STATE_UNUSED;
        sched_set_preemption(old_preempt);
        return -1;
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

    // #446: this is the user first-entry (IRETQ) frame builder. context_start()
    // does not fxrstor64 the incoming proc, but the VERY NEXT context_switch()
    // does, so this path needs a valid seeded FXSAVE area exactly like the
    // others. It was the one builder both #588 and the first #446 draft left
    // unseeded.
    proc->rsp = kstack_top;
    proc_init_fpu_area(proc);
    proc_stack_tag(proc);
    proc->context = NULL;
    proc->entry_point = NULL;  // Not used for user processes
    proc->entry_arg = NULL;

    proc->running_cpu = -1;
    proc->last_cpu    = -1;   // #83: never run anywhere yet
    // #421 phase 7: only migrate to an AP if AP user-scheduling is enabled;
    // with it off (the default now, see cpu/smp.c), a migq push would strand the
    // proc (nothing pops it), so route every user proc to the BSP ready queue.
    extern int g_smp_user_sched;
    // #67 pass 2: the migq is the OLD #279 one-shot "launch this app on an AP"
    // hack, and it bypasses the scheduler entirely (smp_ap_run_user context_starts
    // straight into the process from the work loop). Now that APs are real
    // scheduler consumers, routing anything down it would take that process OUT
    // of the priority-ordered run queues and out of reach of preemption, aging
    // and stealing. Every user process goes through add_to_ready_queue(), which
    // is where the priority-aware placement lives.
    int __mig = 0;
    (void)g_smp_user_sched;
    g_next_user_migratable = 0;  // one-shot
    if (__mig) { proc->migratable=1; smp_migq_push(proc); }
    else { proc->migratable=0; add_to_ready_queue(proc); }

    kprintf("[PROC] Created user process '%s' (PID %u) CR3=0x%lx uid=%u gid=%u\n",
            name, proc->pid, proc->cr3, proc->uid, proc->gid);

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
int proc_create_user_tty_as(const char *name, void *elf_data, uint64_t elf_size,
                            int pts_idx, proc_ident_t ident) {
    if (pts_idx < 0 || pts_idx >= 8) return -1;
    bool old = sched_set_preemption(false);
    g_tty_bind_pts_idx = pts_idx;
    int pid = proc_create_user_as(name, elf_data, elf_size, NULL, NULL, ident);
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
    // #745 (local 75) CLASS FIX: fork the task running on THIS cpu. Through
    // the BSP-published global, an AP-side fork copied the BSP task's
    // process_t, cr3 and kernel stack, and returned that child to the caller.
    process_t *me = proc_current();

    if (!me) {
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
    memcpy(child, me, sizeof(process_t));
    { uint64_t __fl = spinlock_acquire_irqsave(&g_proc_table_lock);
      child->pid = next_pid++;   // #75: unlocked RMW on a shared global
      spinlock_release_irqrestore(&g_proc_table_lock, __fl); }
    child->ppid = me->pid;
    child->state = PROC_STATE_READY;
    child->next = NULL;
    // #67: the struct copy above duplicated the parent's ownership flag. The
    // child has never been on a core, so its context is trivially final; a
    // copied 1 here would make it PERMANENTLY ineligible in sched_rq_pop(),
    // i.e. a process that is created, queued, and never runs.
    child->sched_on_cpu = 0;
    child->sched_pinned = 0;   // #75: a copied pin would never be released
    child->rq_wanted = 0;   // #75: a copied owed-enqueue would never be paid
    child->sched_state_at_enq = 0; child->sched_state_at_pop = 0;
    child->sched_enq_ra = 0;   // #75: copied forensics would name the parent
    child->running_cpu = -1;
    child->last_cpu    = -1;   // #83
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
        child->mm = me->mm ? mm_dup(me->mm) : (void *)0;
    }

    // Clone address space (copy-on-write share of all user pages)
    if (me->privilege == PRIV_USER && me->cr3 != 0) {
        child->cr3 = vmm_clone_user_space_cow(me->cr3);  // #429 COW fork
        if (child->cr3 == 0) {
            child->state = PROC_STATE_UNUSED;
            sched_set_preemption(old_preempt);
            return -1;
        }
    }

    // Allocate new kernel stack for child
    child->stack_base = kmalloc(me->stack_size);
    if (!child->stack_base) {
        if (child->cr3) vmm_destroy_user_space(child->cr3);
        child->state = PROC_STATE_UNUSED;
        sched_set_preemption(old_preempt);
        return -1;
    }
    child->stack_size = me->stack_size;

    // Copy parent's kernel stack (includes the syscall entry frame)
    memcpy(child->stack_base, me->stack_base, me->stack_size);

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
    // #588: context_switch frame with a 16-aligned 512-byte FXSAVE area whose
    // base stashes a pointer to RFLAGS (see context_switch.asm). RFLAGS + 15
    // GPRs + retaddr sit in the top 256 bytes; child->rsp is the FXSAVE base.
    // #446: the synthetic switch frame is 17 qwords starting at rf, so rf must
    // sit at least 17 qwords BELOW the syscall entry frame that fork_child_return
    // returns through (that frame occupies the TOP 22 qwords). rf = ctop-256
    // (32 qwords) left the frame's top 7 qwords overlapping the syscall frame's
    // arg6/padding/R15/R14/R13/R12 slots, so the child returned to user mode
    // with those registers clobbered. 512 puts it clear with margin.
    uint64_t ctop = child_stack_top & ~0xFUL;
    uint64_t rf  = ctop - 512;
    *(uint64_t *)rf = 0x202;                     // RFLAGS (IF set)
    for (int i = 1; i <= 15; i++) *(uint64_t *)(rf + i * 8) = 0;  // 15 GPRs, RAX=0
    *(uint64_t *)(rf + 128) = (uint64_t)fork_child_return;        // return address
    // #446: no stack carve, no stashed RF pointer; the FXSAVE image lives in
    // child->fpu_area (see process.h).
    // #110: and it is seeded from the PARENT'S LIVE FPU/SSE(/AVX) registers,
    // not from the architectural default, so the child inherits the parent's
    // floating-point environment (MXCSR, x87 control word, XMM/YMM, x87 stack)
    // as POSIX requires. See fpu_capture_live() above.
    child->rsp = rf;
    proc_capture_fpu_from_current(child);
    proc_stack_tag(child);

    // Ensure child takes the context_switch path (not context_start/IRET)
    if (child->total_time == 0) child->total_time = 1;

    // Add child to ready queue
    add_to_ready_queue(child);

    kprintf("[PROC] Forked '%s' (PID %u -> child PID %u)\n",
            child->name, me->pid, child->pid);

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
    // #745 (local 75) CLASS FIX: clone the task running on THIS cpu. Through
    // the BSP-published global, an AP-side pthread_create() made a thread of
    // the BSP's task, sharing ITS address space and not the caller's.
    process_t *me = proc_current();

    (void)tls;  // CLONE_SETTLS: FS-base switching per task is not wired yet
                // (pthread_self() uses SYS_GETTID, TSD uses arrays), so a
                // NULL/zero tls is the only case the current libc exercises.
    if (!me) return -1;
    // A raw clone with no shared stack is just fork(); route it there.
    if (!(flags & CLONE_VM) || user_stack == NULL) {
        return proc_fork();
    }

    // #565: the child will run on user_stack (it is stored as the child's saved
    // user RSP below and returned onto via SYSRET). The syscall pointer
    // chokepoint's flat argtab CANNOT express a stack pointer: rsp points at the
    // TOP of the stack and the usable region is BELOW it, and userland passes
    // stack_base + stack_size (one past the allocation, userland/libc/pthread.c),
    // so validating bytes AT user_stack would false-reject every legal
    // pthread_create. Validate the first word the child will push onto -
    // [user_stack-8, user_stack) - which is the top of the stack region and
    // inside the caller's own mapping. This is a Ring-3-only path (proc_clone's
    // sole caller is the SYS_CLONE dispatcher), so validate_user_ptr walks the
    // caller's CR3. It closes the #500-class hole: a Ring-3 stack pointer that
    // names KERNEL or UNMAPPED memory is rejected here with -EFAULT and no
    // thread is spawned, rather than the child faulting (or worse) in Ring 0.
    if (validate_user_ptr((const void *)((uint64_t)user_stack - 8), 8,
                          ACCESS_RW_USER) != VALIDATE_OK) {
        return -14; // -EFAULT
    }

    bool old_preempt = sched_set_preemption(false);

    process_t *child = alloc_proc_slot();
    if (!child) {
        sched_set_preemption(old_preempt);
        return -1;
    }

    // Copy parent structure, then fix up the thread-specific fields.
    memcpy(child, me, sizeof(process_t));
    { uint64_t __fl = spinlock_acquire_irqsave(&g_proc_table_lock);
      child->pid = next_pid++;   // #75: unlocked RMW on a shared global
      spinlock_release_irqrestore(&g_proc_table_lock, __fl); }
    child->ppid = me->pid;
    child->state = PROC_STATE_READY;
    child->next = NULL;
    // #67: the struct copy above duplicated the parent's ownership flag. The
    // child has never been on a core, so its context is trivially final; a
    // copied 1 here would make it PERMANENTLY ineligible in sched_rq_pop(),
    // i.e. a process that is created, queued, and never runs.
    child->sched_on_cpu = 0;
    child->sched_pinned = 0;   // #75: a copied pin would never be released
    child->rq_wanted = 0;   // #75: a copied owed-enqueue would never be paid
    child->sched_state_at_enq = 0; child->sched_state_at_pop = 0;
    child->sched_enq_ra = 0;   // #75: copied forensics would name the parent
    child->running_cpu = -1;
    child->last_cpu    = -1;   // #83
    child->wait_entry = NULL;
    child->sig_pending = 0;
    child->return_work = 0;

    // Thread group: leader is the caller's tgid (or the caller itself).
    child->tgid = me->tgid ? me->tgid : me->pid;

    // Bump refcounts on inherited fds (CLONE_FILES sharing is approximated as
    // fork-style inheritance; adequate for the current libc).
    extern void file_get(struct file *f);
    for (int __fd = 0; __fd < MAX_FDS; __fd++) {
        if (child->fds[__fd]) file_get(child->fds[__fd]);
    }

    // (1) SHARE the address space - do NOT clone it. Mark the child so its
    // cleanup never destroys the shared cr3 or frees the shared user stack.
    child->cr3 = me->cr3;
    child->shares_vm = 1;
    child->user_stack_base = 0;   // owned by the leader, not this thread
    child->user_stack_size = 0;

    // #421 phase 5 follow-up: child->mm is already the SAME pointer as
    // me->mm (copied by the memcpy above) - this thread now holds
    // its own independent reference to it. Bump the shared mm's refcount so
    // cleanup_proc_slot() (via mm_put(), see demand.h) only actually frees it
    // once every process_t sharing it (leader AND every cloned thread) has
    // released its reference, instead of whichever one happens to be
    // !shares_vm freeing it unconditionally even while a sibling thread is
    // still alive and using it (the real use-after-free this pass found: a
    // crashed AssaultCube leader's cleanup froze the mm out from under a
    // just-cloned worker thread that was still running).
    if (child->mm) {
        extern void mm_get(void *mm);
        proc_mm_lock();
        mm_get(child->mm);
        proc_mm_unlock();
    }

    // Allocate + copy a fresh kernel stack (the child needs its own kernel
    // stack for syscalls; the copy carries the parent's syscall-entry frame).
    child->stack_base = kmalloc(me->stack_size);
    if (!child->stack_base) {
        child->state = PROC_STATE_UNUSED;
        child->cr3 = 0; child->shares_vm = 0;
        sched_set_preemption(old_preempt);
        return -1;
    }
    child->stack_size = me->stack_size;
    memcpy(child->stack_base, me->stack_base, me->stack_size);

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
    // #588: context_switch frame with a 16-aligned 512-byte FXSAVE area whose
    // base stashes a pointer to RFLAGS (see context_switch.asm). RFLAGS + 15
    // GPRs + retaddr sit in the top 256 bytes; child->rsp is the FXSAVE base.
    // #446: the synthetic switch frame is 17 qwords starting at rf, so rf must
    // sit at least 17 qwords BELOW the syscall entry frame that fork_child_return
    // returns through (that frame occupies the TOP 22 qwords). rf = ctop-256
    // (32 qwords) left the frame's top 7 qwords overlapping the syscall frame's
    // arg6/padding/R15/R14/R13/R12 slots, so the child returned to user mode
    // with those registers clobbered. 512 puts it clear with margin.
    uint64_t ctop = child_stack_top & ~0xFUL;
    uint64_t rf  = ctop - 512;
    *(uint64_t *)rf = 0x202;                     // RFLAGS (IF set)
    for (int i = 1; i <= 15; i++) *(uint64_t *)(rf + i * 8) = 0;  // 15 GPRs, RAX=0
    *(uint64_t *)(rf + 128) = (uint64_t)fork_child_return;        // return address
    // #446: no stack carve, no stashed RF pointer; the FXSAVE image lives in
    // child->fpu_area (see process.h).
    // #110: and it is seeded from the PARENT'S LIVE FPU/SSE(/AVX) registers,
    // not from the architectural default, so the child inherits the parent's
    // floating-point environment (MXCSR, x87 control word, XMM/YMM, x87 stack)
    // as POSIX requires. See fpu_capture_live() above.
    child->rsp = rf;
    proc_capture_fpu_from_current(child);
    proc_stack_tag(child);

    // Take the context_switch (not context_start/IRET) path.
    if (child->total_time == 0) child->total_time = 1;

    // (3) TID bookkeeping. Parent and child share the address space, so these
    // user-memory writes are valid from the parent's context here.
    if ((flags & CLONE_PARENT_SETTID) && parent_tid) {
        uint32_t v = child->pid;                       // #509 TOCTOU-safe write
        (void)copy_to_user(parent_tid, &v, sizeof(v));
    }
    if ((flags & CLONE_CHILD_SETTID) && child_tid) {
        uint32_t v = child->pid;                       // #509 TOCTOU-safe write
        (void)copy_to_user(child_tid, &v, sizeof(v));
    }
    child->clear_child_tid =
        (flags & CLONE_CHILD_CLEARTID) ? child_tid : NULL;

    add_to_ready_queue(child);

    kprintf("[PROC] Cloned thread of '%s' (PID %u -> tid %u) flags=0x%x\n",
            child->name, me->pid, child->pid, flags);

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

    // #700 B8: execute permission. execve() is the second way a Ring-3 process
    // starts a program, and a rule enforced on only one of the two ways is not
    // a rule. Same check, same bit, same reasoning as spawn_impl() in
    // syscall.c; see the long comment there.
    {
        extern int perms_check(const char *path, uint32_t uid, uint32_t gid, int access);
        if (perms_check(path, cur->euid, cur->egid, 1 /* X_OK */) != 0) return -1;
    }

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
    if (elf_load_user_named(data, size, new_cr3, &entry, &base, &end, path) != 0) {
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
    // #745 (local 75) CLASS FIX: the task entering Ring 3 is THIS cpu's.
    process_t *me = proc_current();

    // Set up TSS kernel stack for this process
    uint64_t kstack = (uint64_t)me->stack_base + me->stack_size;
    cpu_set_kernel_stack(kstack);

    // Switch to user address space
    if (me->cr3 != 0) {
        vmm_switch_pml4(me->cr3);
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
    // #745 (local 75) CLASS FIX: the privilege asked about is THIS cpu's.
    process_t *me = proc_current();

    return me && me->privilege == PRIV_USER;
}

/**
 * Get current process CR3
 */
uint64_t proc_get_cr3(void) {
    // #745 (local 75) CLASS FIX: the address space asked about is THIS cpu's.
    process_t *me = proc_current();

    return me ? me->cr3 : 0;
}

// Return current process name (for debugging)
const char *proc_current_name(void) {
    // #745 (local 75) CLASS FIX: the name asked for is THIS cpu's task.
    process_t *me = proc_current();

    if (me && me->name[0])
        return me->name;
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
        // #421 phase 5: capture-and-walk this process's mm under
        // g_proc_mm_lock, the same lock cleanup_proc_slot() holds across
        // mm_destroy()+null. See process.h for the full race writeup; this
        // is the fix for the panic that took down the whole VM the first
        // time AssaultCube crashed under load during bring-up.
        proc_mem_in_t mi;
        proc_mem_out_t mo;
        proc_mm_lock();   // #421 phase 7: irqsave (was raw spinlock_acquire)
        proc_mem_fill_in(p, &mi);
        int mem_ok = (proc_mem_account(&mi, &mo) == 1);
        proc_mm_unlock();
        out[n].mem_kb = mem_ok ? mo.working_set_kb : 0;
        out[n].running_cpu = p->running_cpu;
        // #145: publish the kernel-authoritative idle bit. sched_tick() credits
        // total_time to WHATEVER is current, idle included, so an idle process
        // accumulates ~one tick per tick it is on the core. That is correct as
        // an accounting record (it is what makes cpu_ticks sum to elapsed
        // capacity) but it made every ranked consumer list in the tree put
        // "idle" on top on any machine that was not saturated. Ring 3 could not
        // tell an idle row from a real one except by matching its NAME, so this
        // exports the flag process_t already carries.
        out[n].flags = p->is_idle ? PROC_INFO_F_IDLE : 0u;
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


// ===========================================================================
// #58: the calling process's working directory, or NULL when there is no
// current process (boot-time and kernel-thread callers).
//
// ONE definition, because the alternative is every path syscall writing
// `(p && p->cwd[0]) ? p->cwd : "/"` for itself, which is how sys_chdir ended
// up as the only site that knew the rule. NULL rather than "/" so the resolver
// can tell "no process" from "process at the root": they happen to resolve the
// same way today, and encoding that coincidence at 15 call sites would hide it.
//
// Justified-C, not Rust: this is a one-line read of an existing C struct field
// through an existing C accessor. A Rust FFI round trip for it would add a
// second definition of the thing whose single definition is the entire point.
// ===========================================================================
const char *proc_cwd(void) {
    process_t *p = proc_current();
    return (p && p->cwd[0]) ? p->cwd : (const char *)0;
}
