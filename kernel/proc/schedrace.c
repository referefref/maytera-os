// proc/schedrace.c - #75: reproducer + state capture for the SMP context
// corruption fault. See schedrace.h for the fault and the method.
#include "schedrace.h"
#include "process.h"
#include "../serial.h"
#include "../string.h"
#include "../cpu/smp.h"
#include "../cpu/mono.h"

extern void kpanic(const char *fmt, ...);

// ---------------------------------------------------------------------------
// 1. THE RING
// ---------------------------------------------------------------------------
typedef struct {
    uint64_t seq;
    uint64_t us;         // mono_us() at the switch
    uint32_t from_pid;
    uint32_t to_pid;
    uint64_t from_rsp;   // prev->rsp BEFORE the switch (still the old value)
    uint64_t to_rsp;     // next->rsp we are about to load
    uint32_t to_state;
    int32_t  to_on_cpu;  // sched_on_cpu of the incoming task: MUST be 0
    char     from_name[16];
    char     to_name[16];
} sr_ent_t;

static sr_ent_t  g_ring[SCHEDRACE_CPUS][SCHEDRACE_RING];
static uint32_t  g_head[SCHEDRACE_CPUS];
static uint64_t  g_seq;                       // global order across cores
static int       g_reported;                  // one full dump is enough

static void sr_copy_name(char *dst, const char *src) {
    int i = 0;
    if (src) for (; i < 15 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

void schedrace_note(uint32_t cpu, const void *prevv, const void *nextv) {
    const process_t *prev = (const process_t *)prevv;
    const process_t *next = (const process_t *)nextv;
    if (cpu >= SCHEDRACE_CPUS) cpu = 0;
    uint32_t h = g_head[cpu];
    sr_ent_t *e = &g_ring[cpu][h];
    e->seq      = __sync_add_and_fetch(&g_seq, 1);
    e->us       = mono_us();
    e->from_pid = prev ? prev->pid : 0xFFFFFFFFu;
    e->to_pid   = next ? next->pid : 0xFFFFFFFFu;
    e->from_rsp = prev ? prev->rsp : 0;
    e->to_rsp   = next ? next->rsp : 0;
    e->to_state = next ? (uint32_t)next->state : 0xFFFFFFFFu;
    e->to_on_cpu= next ? next->sched_on_cpu : -1;
    sr_copy_name(e->from_name, prev ? prev->name : "-");
    sr_copy_name(e->to_name,   next ? next->name : "-");
    g_head[cpu] = (h + 1) % SCHEDRACE_RING;
}

void schedrace_dump(const char *why) {
    kprintf("[SCHEDRACE] ==== switch history (%s) ====\n", why ? why : "?");
    for (uint32_t c = 0; c < SCHEDRACE_CPUS; c++) {
        // Skip cores that never switched, so the dump stays readable.
        int used = 0;
        for (uint32_t i = 0; i < SCHEDRACE_RING; i++) if (g_ring[c][i].seq) { used = 1; break; }
        if (!used) continue;
        kprintf("[SCHEDRACE] --- cpu%u (oldest first) ---\n", c);
        for (uint32_t i = 0; i < SCHEDRACE_RING; i++) {
            uint32_t idx = (g_head[c] + i) % SCHEDRACE_RING;
            sr_ent_t *e = &g_ring[c][idx];
            if (!e->seq) continue;
            kprintf("[SCHEDRACE] #%lu %luus %s/%u rsp=0x%lx -> %s/%u rsp=0x%lx "
                    "state=%u on_cpu=%d\n",
                    (unsigned long)e->seq, (unsigned long)e->us,
                    e->from_name, e->from_pid, (unsigned long)e->from_rsp,
                    e->to_name, e->to_pid, (unsigned long)e->to_rsp,
                    e->to_state, e->to_on_cpu);
        }
    }
    kprintf("[SCHEDRACE] ==== end ====\n");
}

// ---------------------------------------------------------------------------
// 2. THE VALIDATOR
// ---------------------------------------------------------------------------
// Everything here must be true of a context we are about to resume. Each check
// is a separate reason code so a report names WHICH invariant broke, not just
// "something is wrong".
enum {
    SR_OK = 0,
    SR_BAD_RSP_RANGE = 1,   // rsp outside the incoming task's own kernel stack
    SR_BAD_ON_CPU    = 2,   // still owned by a core mid-switch (#67 handoff)
    SR_BAD_STATE     = 3,   // not RUNNING, though we just published it
    SR_BAD_RETADDR   = 4,   // saved return address is not in kernel text
    SR_BAD_SELF      = 5,   // switching to the task we are switching from
};

static const char *sr_reason(int r) {
    switch (r) {
        case SR_BAD_RSP_RANGE: return "rsp outside the incoming task's kernel stack";
        case SR_BAD_ON_CPU:    return "incoming task still marked on-cpu (half-saved context)";
        case SR_BAD_STATE:     return "incoming task is not RUNNING";
        case SR_BAD_RETADDR:   return "saved return address is not in kernel text";
        case SR_BAD_SELF:      return "switching a task to itself";
        default:               return "?";
    }
}

// Kernel text bounds, from the linker script.
// Kernel TEXT bounds from linker.ld (__text_start/__text_end). Text only: a
// saved return address must point at code, and using the whole image would
// accept a value that lands in .bss.
extern char __text_start[], __text_end[];

static int sr_is_kernel_text(uint64_t a) {
    uint64_t lo = (uint64_t)__text_start, hi = (uint64_t)__text_end;
    return a >= lo && a < hi;
}

// #75: a switch frame is 15 GPRs + RFLAGS, then the return address, so the
// return address sits at [rsp + 16*8]. See proc/context_switch.asm.
#define SR_RETADDR_OFF (16 * 8)

static int sr_validate(const process_t *prev, const process_t *next, int deep) {
    if (!next) return SR_OK;
    if (prev == next) return SR_BAD_SELF;
    if (next->sched_on_cpu != 0) return SR_BAD_ON_CPU;
    if (next->state != PROC_STATE_RUNNING) return SR_BAD_STATE;

    if (next->stack_base && next->stack_size >= 256) {
        uint64_t lo = (uint64_t)next->stack_base;
        uint64_t hi = lo + next->stack_size;
        if (next->rsp < lo || next->rsp + SR_RETADDR_OFF + 8 > hi)
            return SR_BAD_RSP_RANGE;
        // Only read through the frame once the range is known good, and only
        // for a task that has actually been switched out before (a first-entry
        // user task's frame is an IRET frame, a different shape).
        if (deep && next->total_time != 0 && next->privilege == PRIV_KERNEL) {
            uint64_t ret = *(volatile uint64_t *)(next->rsp + SR_RETADDR_OFF);
            if (!sr_is_kernel_text(ret)) return SR_BAD_RETADDR;
        }
    }
    return SR_OK;
}

int schedrace_check(uint32_t cpu, const void *prevv, const void *nextv,
                    const char *when) {
    const process_t *prev = (const process_t *)prevv;
    const process_t *next = (const process_t *)nextv;
    int r = sr_validate(prev, next, 1);
    if (r == SR_OK) return 0;

    if (!g_reported) {
        g_reported = 1;
        kprintf("[SCHEDRACE] *** CORRUPT CONTEXT DETECTED at %s on cpu %u ***\n",
                when ? when : "?", cpu);
        kprintf("[SCHEDRACE] reason %d: %s\n", r, sr_reason(r));
        if (next) {
            // #75: the line that separates (a) from (b). enq=<state when it was
            // queued> pop=<state when this core took it> now=<state at the
            // pre-switch check>. If enq or pop is already a non-runnable state,
            // the queue accepted something it should not have (b). If both are
            // runnable and only `now` is wrong, something changed it under us (a).
            kprintf("[SCHEDRACE] FORENSICS '%s' pid=%u: enq=%u pop=%u now=%u "
                    "queued_by=0x%lx pinned=%d\n",
                    next->name, next->pid, next->sched_state_at_enq,
                    next->sched_state_at_pop, (uint32_t)next->state,
                    (unsigned long)(uint64_t)next->sched_enq_ra,
                    next->sched_pinned);
            // #75 evidence 2: did the enqueue that put this task here go through
            // the funnel (route=1) or bypass it (route=2), and was it allowed
            // while still executing? Different bugs, distinguished directly.
            kprintf("[SCHEDRACE] ROUTE '%s' pid=%u: enq_route=%u allowed_hot=%u\n",
                    next->name, next->pid, next->enq_route, next->enq_allowed_hot);
            kprintf("[SCHEDRACE] incoming: '%s' pid=%u state=%u on_cpu=%d "
                    "rsp=0x%lx stack=[0x%lx,0x%lx) cr3=0x%lx priv=%u tt=%lu\n",
                    next->name, next->pid, (uint32_t)next->state,
                    next->sched_on_cpu, (unsigned long)next->rsp,
                    (unsigned long)next->stack_base,
                    (unsigned long)((uint64_t)next->stack_base + next->stack_size),
                    (unsigned long)next->cr3, (uint32_t)next->privilege,
                    (unsigned long)next->total_time);
        }
        if (prev) {
            kprintf("[SCHEDRACE] outgoing: '%s' pid=%u state=%u on_cpu=%d "
                    "rsp=0x%lx stack=[0x%lx,0x%lx)\n",
                    prev->name, prev->pid, (uint32_t)prev->state,
                    prev->sched_on_cpu, (unsigned long)prev->rsp,
                    (unsigned long)prev->stack_base,
                    (unsigned long)((uint64_t)prev->stack_base + prev->stack_size));
        }
        // WHOSE memory is the bad rsp, if anyone's? A stray rsp is only
        // actionable once you know what owns the bytes under it.
        if (next && next->rsp) {
            extern process_t *proc_table_ref(int i);
            for (int i = 0; i < MAX_PROCESSES; i++) {
                process_t *q = proc_table_ref(i);
                if (!q || q == next || !q->stack_base || q->stack_size < 256) continue;
                uint64_t qlo = (uint64_t)q->stack_base, qhi = qlo + q->stack_size;
                if (next->rsp >= qlo && next->rsp < qhi)
                    kprintf("[SCHEDRACE]   -> that rsp is INSIDE '%s' pid=%u "
                            "stack [0x%lx,0x%lx) state=%u\n", q->name, q->pid,
                            (unsigned long)qlo, (unsigned long)qhi,
                            (uint32_t)q->state);
            }
        }
        schedrace_dump("at corruption");
    }

#ifdef SCHEDRACE
    kpanic("[SCHEDRACE] corrupt context on cpu %u at %s: %s. This is the #75 "
           "reproducer firing; the dump above is the state at the fault.",
           cpu, when ? when : "?", sr_reason(r));
#endif
    return r;
}

// ---------------------------------------------------------------------------
// 3. THE WINDOW WIDENER
// ---------------------------------------------------------------------------
#ifdef SCHEDRACE
#ifndef SCHEDRACE_US
#define SCHEDRACE_US 40    // microseconds of delay per site
#endif
void schedrace_delay(schedrace_site_t site) {
    // Only widen when a second core can actually be in the kernel; on a single
    // core this would just make the machine slow for no reason.
    extern int g_smp_user_sched;
    if (!g_smp_user_sched) return;
    if (site >= SR_SITE_MAX) return;
    // Bounded by the monotonic clock, not an iteration count. Interrupts are
    // OFF here by construction (both sites are inside sched_schedule()'s cli
    // region), which is exactly the point: it holds the window open against the
    // other core while this one cannot be preempted out of it.
    uint64_t t0 = mono_us();
    while (mono_us() - t0 < SCHEDRACE_US) { __asm__ volatile("pause"); }
}
#else
void schedrace_delay(schedrace_site_t site) { (void)site; }
#endif

// ---------------------------------------------------------------------------
// 4. PROVE THE DETECTOR BEFORE TRUSTING IT
// ---------------------------------------------------------------------------
// #67 believed four instruments that were wrong. This one is checked against a
// known-good and four known-bad contexts at boot, and says so on the console.
void schedrace_selftest(void) {
    process_t good, bad;
    static uint8_t stack[4096] __attribute__((aligned(16)));
    uint32_t fail = 0;

    memset(&good, 0, sizeof(good));
    sr_copy_name(good.name, "srtest");
    good.pid         = 4242;
    good.state       = PROC_STATE_RUNNING;
    good.sched_on_cpu= 0;
    good.privilege   = PRIV_KERNEL;
    good.total_time  = 0;                 // skips the deep return-address read
    good.stack_base  = stack;
    good.stack_size  = sizeof(stack);
    good.rsp         = (uint64_t)stack + 1024;

    // KNOWN GOOD: must be silent.
    if (sr_validate(NULL, &good, 1) != SR_OK) fail |= 1u << 0;

    // KNOWN BAD 1: rsp below its stack (the shape of RSP=0x10002).
    bad = good; bad.rsp = 0x10002;
    if (sr_validate(NULL, &bad, 0) != SR_BAD_RSP_RANGE) fail |= 1u << 1;

    // KNOWN BAD 2: rsp past the end of its stack.
    bad = good; bad.rsp = (uint64_t)stack + sizeof(stack) - 8;
    if (sr_validate(NULL, &bad, 0) != SR_BAD_RSP_RANGE) fail |= 1u << 2;

    // KNOWN BAD 3: still owned by a core mid-switch.
    bad = good; bad.sched_on_cpu = 1;
    if (sr_validate(NULL, &bad, 0) != SR_BAD_ON_CPU) fail |= 1u << 3;

    // KNOWN BAD 4: a saved return address that is not kernel text. Build a real
    // frame so the deep check runs against real memory.
    bad = good; bad.total_time = 1;
    *(volatile uint64_t *)(bad.rsp + SR_RETADDR_OFF) = 0x45;   // the observed RIP
    if (sr_validate(NULL, &bad, 1) != SR_BAD_RETADDR) fail |= 1u << 4;

    // ...and the same frame with a REAL kernel address must pass, or the check
    // is just rejecting everything.
    *(volatile uint64_t *)(bad.rsp + SR_RETADDR_OFF) = (uint64_t)&schedrace_selftest;
    if (sr_validate(NULL, &bad, 1) != SR_OK) fail |= 1u << 5;

    if (fail) {
        kprintf("[SCHEDRACE] SELFTEST FAILED mask=0x%x - the corruption "
                "detector is WRONG; do not trust its counts.\n", fail);
    } else {
        kprintf("[SCHEDRACE] selftest OK: detector accepts a good context and "
                "rejects wild-rsp, past-end, still-on-cpu and non-text-retaddr "
                "(kernel text [0x%lx,0x%lx))\n",
                (unsigned long)(uint64_t)__text_start,
                (unsigned long)(uint64_t)__text_end);
    }
#ifdef SCHEDRACE
    kprintf("[SCHEDRACE] REPRODUCER BUILD: window widened by %d us at %d sites; "
            "first corrupt context will PANIC with the switch history.\n",
            (int)SCHEDRACE_US, (int)SR_SITE_MAX);
#endif
}
