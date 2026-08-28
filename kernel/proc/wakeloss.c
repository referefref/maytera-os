// proc/wakeloss.c - #167: a TARGETED REPRODUCER for the block/wake race that
// deletes a task from the scheduler.
//
// WHY THIS EXISTS RATHER THAN "BOOT AND HOPE".
// #165 characterised the failure by booting a desktop 55 times and catching it
// twice. The failing task was always the same one, 'audioinit', and its shape
// is what makes it the victim: a short-lived boot-time kernel thread that
// blocks and sleeps repeatedly while other cores are actively scheduling, and
// then exits. #165's own conclusion was that a thread of that shape, run in a
// loop, is a far cheaper reproducer than a desktop boot. This is that thread.
//
// WHAT IT REPRODUCES, EXACTLY.
// A waiter parks with wait_event(). __wait_prepare() arms sched_on_cpu and sets
// state = BLOCKED as ONE operation (#75), then releases the queue lock. From
// that instant until the context switch has actually happened, the task is
// BLOCKED and STILL EXECUTING. A wake_up() from another core in that window
// reaches proc_wake() -> add_to_ready_queue(), which refuses to queue a task
// that is still executing and defers the enqueue. Before #167 the deferral did
// not carry the state transition with it, so the task stayed BLOCKED and
// sched_drain_deferred() later dropped the owed enqueue for exactly that
// reason. The task is then BLOCKED, on no queue, on no core, with wait_entry
// and wake_time both zero: sync/waitq.c's #610 comment calls that state "It is
// gone", and it is the state #165 captured twice for pid 21.
//
// WHAT MAKES IT A REPRODUCER AND NOT A STRESS TEST. The waiter's progress
// counter is the observable. A waiter that is merely slow keeps advancing; a
// waiter that has been deleted from the scheduler stops advancing FOREVER while
// the waker keeps kicking. Those two are distinguishable with no timing
// assumption at all, which is the property #165's silence detector and panic
// detector both lacked.
//
// OFF BY DEFAULT. Started only when /WAKELOSS.TXT is present on the ESP, the
// same one-binary-two-arms design as the #67 /SMPSCHED.TXT gate, so the arm
// with the reproducer and the arm without are the same kernel bytes.
#include "process.h"
#include "../sync/waitq.h"
#include "../types.h"
#include "../fs/bootlog.h"   // #167: the HEADER, never a private extern (persist-extern-gate)

extern int kprintf(const char *fmt, ...);

// Judgement lives in Rust (#404 policy: new kernel code is Rust unless there is
// a measured performance or entanglement reason). The thread bodies below stay
// C because they are nothing but calls to C macros that expand into the wait
// path (wait_event) and to proc_create/proc_sleep; the DECISION - which waiter
// has stopped making progress while the waker did not - is pure arithmetic over
// a counter array and is the part worth having checked by a type system.
extern int32_t wakeloss_verdict_rs(const uint64_t *rounds, uint64_t *last,
                                   uint32_t *stuck, uint32_t n,
                                   uint64_t kicks_delta, uint32_t thresh);
extern uint32_t wakeloss_selftest_rs(void);

#define WL_WAITERS   4
#define WL_STUCK_N   3    // consecutive 1 s samples with no progress = lost

int g_wakeloss_gate = 0;                  // set by main.c from /WAKELOSS.TXT

static wait_queue_head_t g_wl_wq;
static volatile uint32_t g_wl_gate_flag[WL_WAITERS];
static volatile uint64_t g_wl_rounds[WL_WAITERS];
static volatile uint64_t g_wl_kicks;
static volatile uint64_t g_wl_spawns;     // short-lived sleep-then-exit threads
static process_t        *g_wl_proc[WL_WAITERS];

// ---------------------------------------------------------------------------
// The waiters. wait_event() is the non-interruptible form on purpose: it is the
// form that has no timeout to paper over a lost wake, so a lost wake here is a
// permanent stop rather than a delay. That is the whole point.
// ---------------------------------------------------------------------------
static void wl_waiter(void *arg) {
    uint32_t id = (uint32_t)(uint64_t)arg;
    if (id >= WL_WAITERS) return;
    for (;;) {
        wait_event(&g_wl_wq, g_wl_gate_flag[id] != 0);
        g_wl_gate_flag[id] = 0;
        g_wl_rounds[id]++;
    }
}

// ---------------------------------------------------------------------------
// The waker. Not a poll loop: it is a GENERATOR, and proc_sleep() here is the
// pacing of a stimulus, not a wait for a condition. It waits for nothing and
// tests nothing.
// ---------------------------------------------------------------------------
static void wl_waker(void *arg) {
    (void)arg;
    for (;;) {
        for (uint32_t i = 0; i < WL_WAITERS; i++) g_wl_gate_flag[i] = 1;
        wake_up_all(&g_wl_wq);
        g_wl_kicks++;
        proc_sleep(1);
    }
}

// ---------------------------------------------------------------------------
// THE SAME WAKE, RAISED FROM AN INTERRUPT, SO THE REPRODUCER IS NOT SMP-ONLY.
//
// The window is between __wait_prepare() (which arms sched_on_cpu and sets
// BLOCKED under the queue lock) and the context switch. A THREAD waker can only
// land in it from ANOTHER core, so a thread-only reproducer silently tests
// nothing at 1 vCPU and a clean single-core run would be a false negative - the
// exact shape #165 warned about with the rig.
//
// An INTERRUPT waker has no such restriction: the timer ISR runs on the waiter's
// own core, on its own stack, inside that window, on a uniprocessor. That is not
// an artificial construction either; it is what a NIC RX, an HDA BCIS or a block
// completion does every day, and those are the wakes that matter.
//
// Called from sched_tick(). It takes the queue spinlock (irqsave) and marks
// processes runnable; it never blocks, never allocates and never prints, which
// is the same contract wake_up_all() already has to satisfy for every driver ISR
// in the tree. The period is 7 ticks, deliberately co-prime with the time slice
// so it walks through phases instead of landing at the same point every time.
#define WL_TICK_PERIOD 7
void wakeloss_tick(void) {
    if (!g_wakeloss_gate) return;
    static uint32_t n = 0;
    if (++n % WL_TICK_PERIOD) return;
    for (uint32_t i = 0; i < WL_WAITERS; i++) g_wl_gate_flag[i] = 1;
    wake_up_all(&g_wl_wq);
    g_wl_kicks++;
}

// ---------------------------------------------------------------------------
// The (c) half: a thread of audioinit's exact shape. Sleep, then EXIT, while
// other cores are scheduling. #165 found BOTH of its context-corruption panics
// (one under SCHEDRACE=1, one on a stock kernel at the wild RIP #75 first
// reported) to be CANDIDATE 2 on this task exiting, so the exit path gets its
// own churn rather than riding on the waiters.
// ---------------------------------------------------------------------------
static void wl_shortlived(void *arg) {
    (void)arg;
    proc_sleep(3);
    // falls off the end -> proc_exit() via the thread trampoline
}

static void wl_churn(void *arg) {
    (void)arg;
    for (;;) {
        if (proc_create("wlshort", wl_shortlived, (void *)0, PRIO_LOW) >= 0)
            g_wl_spawns++;
        proc_sleep(5);
    }
}

// ---------------------------------------------------------------------------
// The detector.
// ---------------------------------------------------------------------------
static void wl_monitor(void *arg) {
    (void)arg;
    uint64_t last[WL_WAITERS];
    uint32_t stuck[WL_WAITERS];
    uint64_t snap[WL_WAITERS];
    uint64_t last_kicks = 0;
    for (uint32_t i = 0; i < WL_WAITERS; i++) { last[i] = 0; stuck[i] = 0; }

    for (;;) {
        proc_sleep(1000);
        for (uint32_t i = 0; i < WL_WAITERS; i++) snap[i] = g_wl_rounds[i];
        uint64_t k = g_wl_kicks;
        uint64_t kd = k - last_kicks;
        last_kicks = k;

        int32_t lost = wakeloss_verdict_rs(snap, last, stuck, WL_WAITERS,
                                           kd, WL_STUCK_N);
        {
            extern volatile uint64_t g_enq_refused, g_enq_lost, g_enq_dropped;
            extern volatile uint64_t g_enq_wakefix, g_wq_unpark_rescues;
            kprintf("[WAKELOSS] kicks=%lu rounds=%lu/%lu/%lu/%lu spawns=%lu "
                    "enq=%lu/%lu/%lu/%lu wqr=%lu\n",
                    (unsigned long)k,
                    (unsigned long)snap[0], (unsigned long)snap[1],
                    (unsigned long)snap[2], (unsigned long)snap[3],
                    (unsigned long)g_wl_spawns,
                    (unsigned long)g_enq_refused, (unsigned long)g_enq_lost,
                    (unsigned long)g_enq_dropped, (unsigned long)g_enq_wakefix,
                    (unsigned long)g_wq_unpark_rescues);
        }
        if (lost >= 0 && lost < WL_WAITERS) {
            process_t *p = g_wl_proc[lost];
            static const char *st[6] = { "UNUSED", "READY", "RUNNING",
                                         "SLEEPING", "BLOCKED", "ZOMBIE" };
            uint32_t s = p ? (uint32_t)p->state : 0u;
            kprintf("[WAKELOSS] *** WAITER %d IS GONE *** rounds stuck at %lu "
                    "for %u samples while the waker kicked %lu times.\n",
                    (int)lost, (unsigned long)snap[lost], WL_STUCK_N,
                    (unsigned long)kd);
            if (p) {
                kprintf("[WAKELOSS]   pid=%u '%s' state=%s(%u) wait_entry=%p "
                        "wake_time=%lu on_cpu=%d rq_queued=%u rq_wanted=%u "
                        "pinned=%d last_cpu=%d\n",
                        p->pid, p->name, s < 6 ? st[s] : "?", s,
                        (void *)p->wait_entry, (unsigned long)p->wake_time,
                        p->sched_on_cpu, (unsigned)p->rq_queued,
                        (unsigned)p->rq_wanted, p->sched_pinned, p->last_cpu);
                bootlog_write("[WAKELOSS] waiter %d pid=%u state=%u wait_entry=%p "
                              "wake_time=%lu on_cpu=%d rqQ=%u rqW=%u",
                              (int)lost, p->pid, s, (void *)p->wait_entry,
                              (unsigned long)p->wake_time, p->sched_on_cpu,
                              (unsigned)p->rq_queued, (unsigned)p->rq_wanted);
            }
            // Report once per waiter and keep running: a second waiter going
            // the same way is information, and stopping here would hide it.
            stuck[lost] = 0;
        }
    }
}

// PROVE THE DETECTOR BEFORE TRUSTING IT. #165 had five of its own instruments
// confidently wrong before it suspected the kernel, including a state-name map
// with SLEEPING and BLOCKED swapped. A detector nobody has seen go red is not a
// detector, so the verdict function is exercised against a synthetic stall and
// a synthetic healthy run at boot, on every boot, gate or no gate.
void wakeloss_selftest(void) {
    uint32_t bad = wakeloss_selftest_rs();
    if (bad) {
        kprintf("[WAKELOSS] SELFTEST FAILED: %u case(s) wrong. The #167 "
                "detector is NOT trustworthy on this build.\n", bad);
        bootlog_write("[WAKELOSS] SELFTEST FAILED (%u cases)", bad);
    } else {
        kprintf("[WAKELOSS] selftest OK (red on a synthetic stall, green on a "
                "synthetic healthy run)\n");
    }
}

void wakeloss_start(void) {
    if (!g_wakeloss_gate) return;
    wait_queue_head_init(&g_wl_wq);
    for (uint32_t i = 0; i < WL_WAITERS; i++) {
        char nm[16];
        nm[0] = 'w'; nm[1] = 'l'; nm[2] = 'w'; nm[3] = (char)('0' + i); nm[4] = 0;
        int pid = proc_create(nm, wl_waiter, (void *)(uint64_t)i, PRIO_LOW);
        g_wl_proc[i] = (pid >= 0) ? proc_get((uint32_t)pid) : (process_t *)0;
        kprintf("[WAKELOSS] waiter %u -> pid %d\n", i, pid);
    }
    proc_create("wlwake", wl_waker, (void *)0, PRIO_LOW);
    proc_create("wlchurn", wl_churn, (void *)0, PRIO_LOW);
    proc_create("wlmon", wl_monitor, (void *)0, PRIO_LOW);
    bootlog_write("[WAKELOSS] #167 reproducer started: %d waiters, 1 waker, "
                  "1 exit-churn, 1 monitor", WL_WAITERS);
}
