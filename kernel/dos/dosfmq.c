// dosfmq.c - THE one FM event queue, and the only file that touches it.
//
// (#fmbridge) Split out of dos/dosexec.c, which is compiled BOTH into
// kernel.elf and, byte-identical, into /APPS/DOSUSER (the Ring-3 DOS host, see
// userland/apps/dosring3/mkgen.sh). While the queue lived in dosexec.c the
// Ring-3 build got its own private copy of it in its own address space: the
// guest's OPL2 register writes were queued perfectly and drained by nobody,
// because /APPS/FMSYNTH drains the KERNEL's queue through SYS_DOS_FM_EVENTS.
// See dos/dosfmq.h for the full argument, including why the transport is a
// PUSH here where #flipfix chose a PULL for the framebuffer flip counter.
//
// THIS FILE IS DELIBERATELY EXCLUDED FROM THE RING-3 BUILD TREE, by the same
// mechanism and for the same reason diskimg.c/imgfile.c/usbvol.c are (see the
// tail of mkgen.sh): a Ring-3 process must not be able to instantiate a second
// FM queue even by accident. The exclusion is checkable rather than asserted -
// `nm DOSUSER | grep g_dos_fmq` finds nothing, and the link would fail loudly
// if some other DOS source reached for the struct.
//
// WHY C AND NOT RUST, since the standing rule is Rust for new kernel code: the
// QUEUE ITSELF IS ALREADY RUST. rustkern/fmq.rs owns every ring-buffer
// operation, the drop policy, the sequence numbers and the self-test. What is
// here is the spinlock, the two pid latches and the syscall demultiplexer -
// the parts that are inseparable from Ring-0 locking and process lifetime, and
// which came verbatim out of dosexec.c rather than being written afresh.
// Re-implementing them in Rust would have been a rewrite of moved code, and it
// would have put an FFI boundary in the middle of a critical section.
#include "../types.h"
#include "../serial.h"
#include "../sync/spinlock.h"
#include "dosfmq.h"
// The SYS_DOS_FM_HOST op numbers. They live beside the syscall number itself,
// in the kernel ABI header, mirrored in userland/libc/syscall.h beside the same
// number: an op selector is part of a syscall ABI, not of the seam, and
// dos/dosfmq.h stays free of it so the Ring-3 shim (which includes that header)
// cannot start speaking op numbers across the kbridge wall.
#include "../proc/syscall.h"

// ===========================================================================
// (#182) THE QUEUE. Mirrors rustkern/fmq.rs, sizeof- and offsetof-locked below
// for the same reason dos_bus_t is: a silent drift here would not crash, it
// would feed the synthesiser garbage register writes and the machine would play
// wrong notes with nothing anywhere saying why.
//
// The kernel does NOT synthesise. It timestamps the guest's OPL2 register
// writes and queues them for Ring 3, where the one FM core lives
// (userland/lib/opl2). See rustkern/fmq.rs for why the timestamps are mono_us()
// and not timer_ticks, and why the drain is non-blocking.
// ===========================================================================
#define DOS_FMQ_CAP 1024
typedef struct {
    uint64_t t_us;
    uint8_t  reg;
    uint8_t  val;
    uint8_t  flags;
    uint8_t  _pad;
    uint32_t seq;
} dos_fm_event_t;
_Static_assert(sizeof(dos_fm_event_t) == 16, "dos_fm_event_t must match rustkern/fmq.rs FmEvent");
_Static_assert(__builtin_offsetof(dos_fm_event_t, t_us)  == 0,  "FmEvent.t_us");
_Static_assert(__builtin_offsetof(dos_fm_event_t, reg)   == 8,  "FmEvent.reg");
_Static_assert(__builtin_offsetof(dos_fm_event_t, val)   == 9,  "FmEvent.val");
_Static_assert(__builtin_offsetof(dos_fm_event_t, flags) == 10, "FmEvent.flags");
_Static_assert(__builtin_offsetof(dos_fm_event_t, seq)   == 12, "FmEvent.seq");

typedef struct {
    uint32_t head;
    uint32_t tail;
    uint32_t dropped;
    uint32_t next_seq;
    uint8_t  active;
    uint8_t  pending_reset;
    uint8_t  _pad[2];
    uint32_t n_pushed;
    uint32_t hi_used;      // (#187) high-water queue depth; see rustkern/fmq.rs
    uint32_t _pad2;        // keeps ev[] 8-byte aligned (FmEvent leads with u64)
    dos_fm_event_t ev[DOS_FMQ_CAP];
} dos_fm_queue_t;
_Static_assert(sizeof(dos_fm_queue_t) == 32 + 16 * DOS_FMQ_CAP,
               "dos_fm_queue_t must match rustkern/fmq.rs FmQueue");
_Static_assert(__builtin_offsetof(dos_fm_queue_t, head)          == 0,  "FmQueue.head");
_Static_assert(__builtin_offsetof(dos_fm_queue_t, tail)          == 4,  "FmQueue.tail");
_Static_assert(__builtin_offsetof(dos_fm_queue_t, dropped)       == 8,  "FmQueue.dropped");
_Static_assert(__builtin_offsetof(dos_fm_queue_t, next_seq)      == 12, "FmQueue.next_seq");
_Static_assert(__builtin_offsetof(dos_fm_queue_t, active)        == 16, "FmQueue.active");
_Static_assert(__builtin_offsetof(dos_fm_queue_t, pending_reset) == 17, "FmQueue.pending_reset");
_Static_assert(__builtin_offsetof(dos_fm_queue_t, n_pushed)      == 20, "FmQueue.n_pushed");
_Static_assert(__builtin_offsetof(dos_fm_queue_t, hi_used)       == 24, "FmQueue.hi_used");
_Static_assert(__builtin_offsetof(dos_fm_queue_t, ev)            == 32, "FmQueue.ev");

extern void     dos_fmq_open_rs(dos_fm_queue_t *q);
extern void     dos_fmq_close_rs(dos_fm_queue_t *q);
extern int      dos_fmq_push_rs(dos_fm_queue_t *q, uint8_t reg, uint8_t val, uint64_t t_us);
extern uint32_t dos_fmq_drain_rs(dos_fm_queue_t *q, dos_fm_event_t *out, uint32_t max);
extern uint64_t dos_fmq_status_rs(const dos_fm_queue_t *q);
extern int      dos_fmq_selftest_rs(dos_fm_queue_t *q, dos_fm_event_t *scratch, uint32_t n);

// dos/dosexec.c. THE answer to "is an in-kernel DOS guest running?" - the same
// g_dos_busy proc/dosroute.c consults to keep one-guest-at-a-time true across
// the two DOS paths. Asked rather than re-derived; see DOS_FM_HOST_OPEN below.
extern int dos_is_busy(void);

// dos/dosexec.c. THE answer to "is a synthesiser running, and if not, start
// one" - it owns g_dos_fm_ready and g_dos_fm_synth_pid, the pair whose
// staleness was #205, so the syscall must go through it rather than call
// fm_launch_synth() beside it and spawn a second synthesiser.
extern int dos_fm_synth_ensure(void);

// The queue is a SINGLETON: there is one OPL2 in this machine and one Ring-3
// synthesiser draining it. Guarded by its own irqsave spinlock rather than
// borrowing the DOS window lock, because the producer runs in the guest's
// interpreter (or, now, in a syscall from the Ring-3 host) and the consumer in
// an unrelated process's syscall; coupling those to the compositor handover
// lock would be a lock-ordering trap for no benefit.
//
// 16 KB of .bss. Not allocated per DOS task on purpose: a per-task queue would
// need the consumer to discover which task to drain, and the consumer is a
// separate process that has no business knowing about dos_task_t.
static dos_fm_queue_t g_dos_fmq;
static spinlock_t     g_dos_fmq_lock = SPINLOCK_INIT;

// The pid allowed to DRAIN. Latched by the first caller, the same pattern the
// compositor framebuffer latch uses, so an unrelated app cannot starve the
// synthesiser by draining events out from under it.
static uint32_t g_dos_fm_pid = 0;

// (#fmbridge) The pid allowed to PRODUCE through SYS_DOS_FM_HOST, latched by
// whoever opens the queue from Ring 3. Zero means "no Ring-3 producer": either
// nothing has the queue open, or the in-kernel DOS path does.
//
// This is a real boundary and not decoration. Without it any Ring-3 process
// could inject OPL2 register writes into the queue that /APPS/FMSYNTH renders,
// i.e. make arbitrary noise on the machine's speakers, and could close the
// queue out from under a running guest. With it, exactly one process at a time
// can, and only for as long as it holds the queue open.
static uint32_t g_dos_fmq_user_pid = 0;

// ===========================================================================
// THE SEAM (dos/dosfmq.h). dos/dosexec.c calls these in BOTH rings; in Ring 3
// the same names are implemented by apps/dosring3/shim/kshim.c on top of
// SYS_DOS_FM_HOST, which lands back here in dos_fm_host_call() below.
// ===========================================================================

void dos_fmq_host_open(void) {
    uint64_t fl = spinlock_acquire_irqsave(&g_dos_fmq_lock);
    dos_fmq_open_rs(&g_dos_fmq);
    g_dos_fmq_user_pid = 0;          // opened from Ring 0: no Ring-3 producer
    spinlock_release_irqrestore(&g_dos_fmq_lock, fl);
}

// Deliberately an UNLOCKED read of a uint8_t. It is the cheap pre-check on the
// guest's port-write path and the value is re-tested under the lock inside
// dos_fmq_push_rs(), so a race here can only cost one event at the exact moment
// a guest starts or exits, which is the same window the code it replaced had.
int dos_fmq_host_active(void) { return g_dos_fmq.active ? 1 : 0; }

void dos_fmq_host_push(uint8_t reg, uint8_t val, uint64_t t_us) {
    uint64_t fl = spinlock_acquire_irqsave(&g_dos_fmq_lock);
    dos_fmq_push_rs(&g_dos_fmq, reg, val, t_us);
    spinlock_release_irqrestore(&g_dos_fmq_lock, fl);
}

void dos_fmq_host_close(uint32_t *pushed, uint32_t *dropped) {
    uint64_t fl = spinlock_acquire_irqsave(&g_dos_fmq_lock);
    dos_fmq_close_rs(&g_dos_fmq);
    if (pushed)  *pushed  = g_dos_fmq.n_pushed;
    if (dropped) *dropped = g_dos_fmq.dropped;
    g_dos_fmq_user_pid = 0;
    spinlock_release_irqrestore(&g_dos_fmq_lock, fl);
}

void dos_fmq_host_stats(uint32_t *pushed, uint32_t *dropped,
                        uint32_t *peak, uint32_t *used) {
    uint64_t fl = spinlock_acquire_irqsave(&g_dos_fmq_lock);
    if (pushed)  *pushed  = g_dos_fmq.n_pushed;
    if (dropped) *dropped = g_dos_fmq.dropped;
    if (peak)    *peak    = g_dos_fmq.hi_used;
    if (used)    *used    = g_dos_fmq.head - g_dos_fmq.tail;
    spinlock_release_irqrestore(&g_dos_fmq_lock, fl);
}

uint32_t dos_fmq_host_capacity(void) { return (uint32_t)DOS_FMQ_CAP; }

int dos_fmq_host_selftest(void) {
    // The scratch is static rather than on the stack: 8 events is only 128
    // bytes, but this runs on the DOS interpreter thread and the self-test is a
    // diagnostic, not a reason to grow a kernel stack frame.
    static dos_fm_event_t scratch[8];
    int bad = dos_fmq_selftest_rs(&g_dos_fmq, scratch, 8);
    // The self-test leaves the queue CLOSED as a postcondition (a self-test must
    // not leave a device armed). The caller only ever runs it because a guest is
    // starting, so the re-open belongs here, next to the close that made it
    // necessary. MEASURED on VM <vmid> build 2001 before this existed: the caller
    // opened the queue, the test closed it, FMSYNTH's first drain returned
    // ENODEV and it exited, and Keen 5 wrote its whole 264-register instrument
    // bank into a queue nobody was reading.
    uint64_t fl = spinlock_acquire_irqsave(&g_dos_fmq_lock);
    dos_fmq_open_rs(&g_dos_fmq);
    spinlock_release_irqrestore(&g_dos_fmq_lock, fl);
    return bad;
}

// Called from proc_exit() (via dos_fm_proc_exit in dosexec.c) for EVERY
// process. Cheap pid compares; only a process that held one of the two latches
// does anything.
//
// The producer half is what makes a CRASHED Ring-3 DOS host survivable. Without
// it, a DOSUSER that dies without reaching dos_on_terminate() leaves the queue
// active forever, /APPS/FMSYNTH never sees ENODEV, never renders its tail and
// never exits, and the next guest inherits an armed queue with a stale
// synthesiser attached to it. That is the #205 failure with a different cause.
//
// MUST NOT BLOCK: proc_exit() runs under cli() and this takes the same irqsave
// spinlock the push path uses.
int dos_fmq_host_release_pid(uint32_t pid) {
    if (!pid) return 0;
    int did = 0;
    uint64_t fl = spinlock_acquire_irqsave(&g_dos_fmq_lock);
    if (g_dos_fm_pid == pid)       { g_dos_fm_pid = 0; did = 1; }
    if (g_dos_fmq_user_pid == pid) {
        g_dos_fmq_user_pid = 0;
        dos_fmq_close_rs(&g_dos_fmq);
        did = 1;
    }
    spinlock_release_irqrestore(&g_dos_fmq_lock, fl);
    return did;
}

// ===========================================================================
// (#182) SYS_DOS_FM_EVENTS backend. Copies at most `max` events into the KERNEL
// buffer `out`; the caller (proc/syscall.c) owns the copy to Ring 3.
//
// Returns the count, or DOS_FM_ENODEV when the guest is gone AND the queue is
// empty, which is how /APPS/FMSYNTH learns to render its tail and exit rather
// than feeding the sink silence forever.
// ===========================================================================
#define DOS_FM_ENODEV (-6)
#define DOS_FM_EPERM  (-5)
int dos_fm_drain(dos_fm_event_t *out, uint32_t max, uint32_t pid, uint32_t *dropped) {
    if (!out || max == 0) return -1;
    uint64_t fl = spinlock_acquire_irqsave(&g_dos_fmq_lock);
    if (g_dos_fm_pid == 0) g_dos_fm_pid = pid;
    if (g_dos_fm_pid != pid) {
        spinlock_release_irqrestore(&g_dos_fmq_lock, fl);
        return DOS_FM_EPERM;
    }
    uint32_t n = dos_fmq_drain_rs(&g_dos_fmq, out, max);
    uint8_t  active = g_dos_fmq.active;
    if (dropped) *dropped = g_dos_fmq.dropped;
    spinlock_release_irqrestore(&g_dos_fmq_lock, fl);
    if (n == 0 && !active) return DOS_FM_ENODEV;
    return (int)n;
}
size_t dos_fm_event_size(void) { return sizeof(dos_fm_event_t); }

// ===========================================================================
// (#fmbridge) SYS_DOS_FM_HOST - the PRODUCER door, for the Ring-3 DOS host.
//
// SYS_DOS_FM_EVENTS above is the consumer door and has existed since #182. This
// is its counterpart: it lets /APPS/DOSUSER feed the same queue the in-kernel
// interpreter feeds, so the two DOS paths converge at the earliest possible
// point and share every line of code downstream of it - the drop accounting,
// the sequence numbers, the `active` teardown, the drain latch and FMSYNTH
// itself.
//
// ALL ARGUMENTS AND RETURNS ARE SCALARS. There is no pointer in this call, so
// it needs no rustkern/argtab.rs descriptor and can never be the site of a
// copy_to_user fault: the same property that let SYS_FB_FLIP_COUNT (#flipfix)
// go in without one. A negative return is always an error; every successful
// return is >= 0.
//
// THE OWNERSHIP RULE, and why it is not decoration. Any Ring-3 process can
// issue this syscall. Without a latch, any app could inject OPL2 register
// writes that /APPS/FMSYNTH renders (arbitrary noise on the machine's speakers)
// or close the queue under a running guest. So OPEN latches the caller's pid
// and every other op requires it, the first opener wins, and an in-kernel guest
// holding the queue refuses a Ring-3 opener outright. The latch is released on
// CLOSE and, if the host dies without one, by dos_fmq_host_release_pid() from
// proc_exit().
// ===========================================================================
int64_t dos_fm_host_call(uint32_t op, uint64_t a1, uint64_t a2, uint64_t a3,
                         uint32_t pid) {
    if (!pid) return -1;

    switch (op) {
    case DOS_FM_HOST_OPEN: {
        // "IS AN IN-KERNEL DOS GUEST RUNNING?" IS ASKED OF THE THING THAT
        // ALREADY KNOWS. The first version of this inferred it from the queue's
        // own `active` flag - a SECOND answer to a question dos_is_busy()
        // already answers, and the two disagreed the first time they were
        // measured against each other. A failed in-kernel launch had armed the
        // queue and returned without closing it (now fixed at the source, in
        // dos_run_file), so `active` said "a guest is running" about a guest
        // that had never started, and this door refused the Ring-3 host with
        // EBUSY. The result was a Ring-3 guest with no music and a diagnostic
        // that blamed a guest which did not exist.
        //
        // g_dos_busy is what the whole DOS subsystem is written against and
        // what proc/dosroute.c already consults to keep one-guest-at-a-time
        // true ACROSS the two paths. Asking it is reuse; the flag test was a
        // fork, and it behaved like one.
        if (dos_is_busy()) {
            kprintf("[dos] (#fmbridge) Ring-3 pid %u asked for the FM queue "
                    "while an IN-KERNEL DOS guest holds it: refused. One guest "
                    "at a time.\n", pid);
            return -16;   // EBUSY
        }
        uint64_t fl = spinlock_acquire_irqsave(&g_dos_fmq_lock);
        dos_fmq_open_rs(&g_dos_fmq);
        g_dos_fmq_user_pid = pid;
        spinlock_release_irqrestore(&g_dos_fmq_lock, fl);
        kprintf("[dos] (#fmbridge) FM queue opened by the Ring-3 DOS host "
                "(pid %u); its guest's OPL2 writes now reach the SAME queue "
                "/APPS/FMSYNTH drains.\n", pid);
        return 0;
    }

    case DOS_FM_HOST_PUSH: {
        uint64_t fl = spinlock_acquire_irqsave(&g_dos_fmq_lock);
        if (g_dos_fmq_user_pid != pid) {
            spinlock_release_irqrestore(&g_dos_fmq_lock, fl);
            return -13;   // EACCES: not the process that opened the queue
        }
        dos_fmq_push_rs(&g_dos_fmq, (uint8_t)a1, (uint8_t)a2, a3);
        spinlock_release_irqrestore(&g_dos_fmq_lock, fl);
        return 0;
    }

    case DOS_FM_HOST_CLOSE: {
        uint64_t fl = spinlock_acquire_irqsave(&g_dos_fmq_lock);
        if (g_dos_fmq_user_pid != pid) {
            spinlock_release_irqrestore(&g_dos_fmq_lock, fl);
            return -13;
        }
        dos_fmq_close_rs(&g_dos_fmq);
        g_dos_fmq_user_pid = 0;
        spinlock_release_irqrestore(&g_dos_fmq_lock, fl);
        return 0;
    }

    // The four counters, one op each and each a plain non-negative scalar.
    // Split rather than packed into one return so that "error" stays a
    // negative value with no bit-pattern ambiguity, and readable AFTER a close:
    // dos_fmq_close_rs() clears only `active`, so n_pushed and dropped survive
    // until the next open and the exit summary can still read them.
    case DOS_FM_HOST_STAT_PUSHED:
    case DOS_FM_HOST_STAT_DROPPED:
    case DOS_FM_HOST_STAT_PEAK:
    case DOS_FM_HOST_STAT_USED:
    case DOS_FM_HOST_ACTIVE: {
        uint64_t fl = spinlock_acquire_irqsave(&g_dos_fmq_lock);
        uint32_t v = 0;
        switch (op) {
        case DOS_FM_HOST_STAT_PUSHED:  v = g_dos_fmq.n_pushed; break;
        case DOS_FM_HOST_STAT_DROPPED: v = g_dos_fmq.dropped;  break;
        case DOS_FM_HOST_STAT_PEAK:    v = g_dos_fmq.hi_used;  break;
        case DOS_FM_HOST_STAT_USED:    v = g_dos_fmq.head - g_dos_fmq.tail; break;
        default:                       v = g_dos_fmq.active ? 1u : 0u; break;
        }
        spinlock_release_irqrestore(&g_dos_fmq_lock, fl);
        return (int64_t)v;
    }

    case DOS_FM_HOST_CAPACITY:
        return (int64_t)DOS_FMQ_CAP;

    case DOS_FM_HOST_SELFTEST: {
        // Owner-only: it CLOSES and re-opens the queue, so an unrelated caller
        // could otherwise reset a running guest's music mid-song.
        uint64_t fl = spinlock_acquire_irqsave(&g_dos_fmq_lock);
        int mine = (g_dos_fmq_user_pid == pid);
        spinlock_release_irqrestore(&g_dos_fmq_lock, fl);
        if (!mine) return -13;
        return (int64_t)dos_fmq_host_selftest();
    }

    case DOS_FM_HOST_LAUNCH: {
        // Owner-only, for the same reason: this spawns a system process.
        uint64_t fl = spinlock_acquire_irqsave(&g_dos_fmq_lock);
        int mine = (g_dos_fmq_user_pid == pid);
        spinlock_release_irqrestore(&g_dos_fmq_lock, fl);
        if (!mine) return -13;
        // dos_fm_synth_ensure() (dos/dosexec.c) is THE liveness question, and
        // it bottoms out in fm_launch_synth() (gui/desktop.c), the same
        // launcher the in-kernel path uses - including the no-audio-sink
        // refusal that stops the OPL2 advertising itself with nothing behind
        // it. A Ring-3 guest therefore gets exactly the same honest ABSENT on
        // a machine with no DAC, and never a second synthesiser beside a live
        // one.
        return (int64_t)dos_fm_synth_ensure();
    }

    default:
        return -1;
    }
}
