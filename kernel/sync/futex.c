// futex.c - Fast Userspace Mutex implementation for MayteraOS
// Part of Task #25 (Threading with clone() syscall); #430 rework: the futex
// layer now blocks and wakes tasks through the REAL process scheduler
// (proc_current / sched_schedule / proc_wake), exactly like sync/waitq.c,
// instead of the disconnected thread.c ready queue it used to drive. This is
// what makes userland pthread mutex/cond/join actually block and wake.

#include "futex.h"
#include "waitq.h"          // #426: wq_ms_to_ticks() (the ONE ms->ticks helper)
#include "../security/validate.h"   // #503: futex addrs are USER memory, U/S-checked
#include "../proc/process.h"
#include "../serial.h"
#include "../string.h"

// External timer + frequency (ticks -> ms conversion).
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;

// Process scheduler primitives (the canonical block/wake mechanism; see
// sync/waitq.c which blocks the same way).
extern process_t *proc_current(void);
extern void sched_schedule(void);
extern void proc_wake(process_t *p);

// process_state_t values we touch (from process.h): PROC_STATE_BLOCKED etc.

// ============================================================================
// Futex Hash Table
// ============================================================================

static futex_bucket_t futex_buckets[FUTEX_HASH_BUCKETS];

static uint64_t futex_wait_count = 0;
static uint64_t futex_wake_count = 0;
static uint64_t futex_timeout_count = 0;

// Waiter pool (pre-allocated to avoid dynamic allocation in the wait path).
#define FUTEX_WAITER_POOL_SIZE 256
static futex_waiter_t waiter_pool[FUTEX_WAITER_POOL_SIZE];
static spinlock_t waiter_pool_lock = SPINLOCK_INIT;
static uint32_t waiter_pool_free_bitmap[FUTEX_WAITER_POOL_SIZE / 32];

// ============================================================================
// Internal helpers
// ============================================================================

static inline uint32_t futex_hash(uint32_t *addr) {
    uint64_t a = (uint64_t)addr;
    return (uint32_t)((a >> 2) ^ (a >> 12)) & FUTEX_HASH_MASK;
}

static futex_waiter_t *alloc_waiter(void) {
    uint64_t f = spinlock_acquire_irqsave(&waiter_pool_lock);
    for (int i = 0; i < FUTEX_WAITER_POOL_SIZE / 32; i++) {
        if (waiter_pool_free_bitmap[i] != 0xFFFFFFFF) {
            for (int j = 0; j < 32; j++) {
                if (!(waiter_pool_free_bitmap[i] & (1u << j))) {
                    waiter_pool_free_bitmap[i] |= (1u << j);
                    int idx = i * 32 + j;
                    spinlock_release_irqrestore(&waiter_pool_lock, f);
                    memset(&waiter_pool[idx], 0, sizeof(futex_waiter_t));
                    return &waiter_pool[idx];
                }
            }
        }
    }
    spinlock_release_irqrestore(&waiter_pool_lock, f);
    return NULL;
}

static void free_waiter(futex_waiter_t *waiter) {
    if (!waiter) return;
    int idx = (int)(waiter - waiter_pool);
    if (idx < 0 || idx >= FUTEX_WAITER_POOL_SIZE) return;
    uint64_t f = spinlock_acquire_irqsave(&waiter_pool_lock);
    waiter_pool_free_bitmap[idx / 32] &= ~(1u << (idx % 32));
    spinlock_release_irqrestore(&waiter_pool_lock, f);
}

// Bucket list ops. Caller holds bucket->lock.
static void add_waiter(futex_bucket_t *bucket, futex_waiter_t *waiter) {
    waiter->next = bucket->waiters;
    waiter->prev = NULL;
    if (bucket->waiters) bucket->waiters->prev = waiter;
    bucket->waiters = waiter;
    bucket->waiter_count++;
    waiter->on_bucket = 1;
}

static void remove_waiter(futex_bucket_t *bucket, futex_waiter_t *waiter) {
    if (!waiter->on_bucket) return;
    if (waiter->prev) waiter->prev->next = waiter->next;
    else              bucket->waiters = waiter->next;
    if (waiter->next) waiter->next->prev = waiter->prev;
    bucket->waiter_count--;
    waiter->next = NULL;
    waiter->prev = NULL;
    waiter->on_bucket = 0;
}

// #503 / MAYTERA-SEC-2026-0016: this checked NULL and 4-byte alignment and
// NOTHING ELSE, so every futex op accepted any address Ring 3 named, including
// kernel memory. That is not just untidy: futex_wait() DEREFERENCES the address
// and compares *addr against the caller's `val`, then sleeps or returns
// -FUTEX_EWOULDBLOCK. Ring 3 could therefore point a futex at kernel memory and
// read the return code as an ORACLE, recovering an arbitrary kernel 32-bit word
// by bisection without ever needing a write primitive. The address is also the
// hash key, so an unvalidated one lets a process queue on and wake waiters at
// addresses it does not own.
//
// The fix is the shared validator, not a fourth hand-rolled test: only the U/S
// bit in the CALLER's page tables can answer this (an address range cannot, see
// drivers/audio_pcm.c). RW is demanded because a futex word is userspace-owned
// memory that userspace both reads and writes; the kernel only reads it here,
// but a futex on a read-only page is not a legitimate futex.
//
// Safe to tighten: futex_wait/wake/requeue have NO kernel-internal callers on
// kernel addresses. The one in-kernel caller, futex_wake_addr() from
// thread_exit(), passes clear_child_tid, which is itself now validated at store
// time and re-validated immediately before use (proc/thread.c).
static bool validate_futex_addr(uint32_t *addr) {
    if (!addr) return false;
    if ((uint64_t)addr & 0x3) return false;   // must be 4-byte aligned
    return validate_user_ptr(addr, sizeof(uint32_t), ACCESS_RW_USER) == VALIDATE_OK;
}

// #426: the private ms_to_ticks() that used to live here is gone. It was a
// character-for-character copy of sync/waitq.h's wq_ms_to_ticks() (same
// `hz ? hz : 250` default, same `(ms*hz + 999)/1000` round-up, same clamp to 1),
// differing only by an explicit (uint64_t) cast on hz that is a no-op: ms is
// already uint64_t, so hz promotes anyway. Provably behaviour-preserving to
// merge, so it is merged - this is the one piece of the futex/waitq overlap that
// genuinely WAS a forked private copy of a shared primitive. See the verdict
// above futex_wait() for why the timed park itself is NOT, and must not be.

// ============================================================================
// Init
// ============================================================================

void futex_init(void) {
    kprintf("[FUTEX] Initializing futex subsystem (process-scheduler backed)...\n");
    for (int i = 0; i < FUTEX_HASH_BUCKETS; i++) {
        spinlock_init(&futex_buckets[i].lock);
        futex_buckets[i].waiters = NULL;
        futex_buckets[i].waiter_count = 0;
    }
    spinlock_init(&waiter_pool_lock);
    memset(waiter_pool_free_bitmap, 0, sizeof(waiter_pool_free_bitmap));
    kprintf("[FUTEX] Futex subsystem initialized (%d buckets, %d max waiters)\n",
            FUTEX_HASH_BUCKETS, FUTEX_WAITER_POOL_SIZE);
}

// ============================================================================
// Wait / Wake
// ============================================================================

// #426 RECONCILIATION VERDICT (2026-07-16): this timed park and
// sync/waitq.h's wait_event_timeout() are NOT a "forked private copy" of one
// primitive, and must not be merged. Recorded here because the wait-migration
// plan (the internal wait-migration plan, Phase 0 follow-up 2) proposes exactly that
// merge, and the CLAUDE.md rule against duplicating a shared primitive makes it
// look mandatory. It is not. Evidence, all from source:
//
// 1. OPPOSITE WAITER LIFETIMES, and each is load-bearing. A waitq entry is
//    allocated on the WAITER'S KERNEL STACK (waitq.h:46). That is precisely why
//    waitq puts its deadline on the PROCESS (me->wake_time) and lets the
//    existing wake_sleeping_procs() sweep fire it: waitq.c:114-122 documents
//    that a tick touching the entry would be a use-after-free of stack memory a
//    concurrent wake may already have released. A futex_waiter_t is pool
//    allocated (alloc_waiter/free_waiter), so futex_tick() CAN safely walk and
//    mutate waiter records from IRQ context - and does (futex.c:311-330).
//    Neither model can adopt the other's without giving up why it exists.
//
// 2. FUTEX NEEDS SELECTIVE WAKE; WAITQ DOES NOT HAVE IT. futex_wake() wakes at
//    most `count` waiters matching BOTH an address and a bitset
//    (FUTEX_WAIT_BITSET/FUTEX_WAKE_BITSET). waitq offers wake_up() (head) and
//    wake_up_all() (all), with no filtering and no count. Adding address+bitset
//    matching to the general kernel wait queue to serve one caller would make
//    the shared primitive worse, which is the opposite of the rule's intent.
//
// 3. FUTEX NEEDS REQUEUE. FUTEX_REQUEUE/FUTEX_CMP_REQUEUE move waiters between
//    futexes WITHOUT waking them. waitq has no such operation, and it is not a
//    condition-wait concept.
//
// 4. THE PARK MUST BE ATOMIC WITH A USERSPACE VALUE CHECK. futex_wait's defining
//    race-closer is re-reading *addr == val UNDER the bucket lock before
//    enqueuing. waitq.h:163 explicitly forbids holding a lock across its waits.
//
// So: waitq is a kernel-OBJECT-keyed condition wait; futex is a userspace
// ADDRESS-keyed wait with selective wake and requeue. They overlap only in the
// generic "park until done or deadline" core. One mechanism count is the goal,
// but these are two mechanisms because they are two problems.
//
// WORTH DOING LATER, and genuinely reconcilable: futex's DEADLINE could move
// onto self->wake_time + PROC_STATE_SLEEPING, which would delete futex_tick()
// entirely (its per-tick O(buckets x waiters) scan) and leave the deadline in
// one place. The waiter record is pool-allocated so the stack hazard of (1)
// does not apply, and the "was I woken or did I time out" question is already
// answered by ->done. NOT done here: it is a behaviour change to verified,
// security-sensitive concurrency code (#430) and needs a VM, not a source
// argument. Note also line 168's caution that sched_tick re-queues on slice
// expiry regardless of state, which any such change must be checked against.
int futex_wait(uint32_t *addr, uint32_t val, uint64_t timeout, uint32_t bitset) {
    if (!validate_futex_addr(addr)) return -FUTEX_EFAULT;
    if (bitset == 0) return -FUTEX_EINVAL;

    process_t *self = proc_current();
    if (!self) return -FUTEX_EINVAL;

    futex_bucket_t *bucket = &futex_buckets[futex_hash(addr)];

    uint64_t f = spinlock_acquire_irqsave(&bucket->lock);

    // Atomically (under the bucket lock) re-check the value: if it no longer
    // equals val, the caller lost the race and must retry in userspace.
    if (*addr != val) {
        spinlock_release_irqrestore(&bucket->lock, f);
        return -FUTEX_EAGAIN;
    }

    futex_waiter_t *waiter = alloc_waiter();
    if (!waiter) {
        spinlock_release_irqrestore(&bucket->lock, f);
        return -FUTEX_EINVAL;
    }
    waiter->addr      = addr;
    waiter->proc      = self;
    waiter->bitset    = bitset;
    waiter->timed_out = false;
    waiter->done      = 0;
    waiter->timeout   = timeout ? (timer_ticks + wq_ms_to_ticks(timeout)) : 0;

    add_waiter(bucket, waiter);
    futex_wait_count++;
    spinlock_release_irqrestore(&bucket->lock, f);

    // Park the task until a waker (or the timeout) sets ->done. This is the
    // same block-and-reschedule mechanism proc_sleep()/waitq use: interrupts
    // are disabled around the done-check + state transition so a timer tick
    // (sched_tick re-queues on slice expiry regardless of state) cannot clobber
    // PROC_STATE_BLOCKED between the check and the reschedule. sched_schedule()
    // does not return until proc_wake() makes us runnable again, and it
    // re-enables interrupts on the way out - so this is a genuine block, not a
    // busy-poll. ->done persists across any race, so no wakeup is lost.
    for (;;) {
        __asm__ volatile("cli");
        if (waiter->done) { __asm__ volatile("sti"); break; }
        self->state = PROC_STATE_BLOCKED;
        sched_schedule();
    }

    // Unlink (idempotent - a waker may already have removed us) and reclaim.
    f = spinlock_acquire_irqsave(&bucket->lock);
    remove_waiter(bucket, waiter);
    bool timed_out = waiter->timed_out;
    spinlock_release_irqrestore(&bucket->lock, f);

    free_waiter(waiter);

    if (timed_out) {
        futex_timeout_count++;
        return -FUTEX_ETIMEDOUT;
    }
    return 0;
}

int futex_wake(uint32_t *addr, int count, uint32_t bitset) {
    if (!validate_futex_addr(addr)) return -FUTEX_EFAULT;
    if (count <= 0) return 0;
    if (bitset == 0) return -FUTEX_EINVAL;

    futex_bucket_t *bucket = &futex_buckets[futex_hash(addr)];

    uint64_t f = spinlock_acquire_irqsave(&bucket->lock);

    int woken = 0;
    futex_waiter_t *waiter = bucket->waiters;
    while (waiter && woken < count) {
        futex_waiter_t *next = waiter->next;
        if (waiter->addr == addr && (waiter->bitset & bitset)) {
            remove_waiter(bucket, waiter);
            waiter->done = 1;           // release the sleeper (persists across
                                        // any scheduling race - no lost wakeup)
            if (waiter->proc) proc_wake(waiter->proc);
            woken++;
            futex_wake_count++;
        }
        waiter = next;
    }

    spinlock_release_irqrestore(&bucket->lock, f);
    return woken;
}

int futex_requeue(uint32_t *addr, uint32_t *addr2, int wake_count, int requeue_count) {
    if (!validate_futex_addr(addr) || !validate_futex_addr(addr2)) return -FUTEX_EFAULT;

    uint32_t h1 = futex_hash(addr), h2 = futex_hash(addr2);
    futex_bucket_t *b1 = &futex_buckets[h1];
    futex_bucket_t *b2 = &futex_buckets[h2];

    uint64_t f;
    if (h1 == h2) {
        f = spinlock_acquire_irqsave(&b1->lock);
        b2 = b1;
    } else if (h1 < h2) {
        f = spinlock_acquire_irqsave(&b1->lock);
        spinlock_acquire(&b2->lock);
    } else {
        f = spinlock_acquire_irqsave(&b2->lock);
        spinlock_acquire(&b1->lock);
    }

    int woken = 0, requeued = 0;
    futex_waiter_t *waiter = b1->waiters;
    while (waiter) {
        futex_waiter_t *next = waiter->next;
        if (waiter->addr == addr) {
            if (woken < wake_count) {
                remove_waiter(b1, waiter);
                waiter->done = 1;
                if (waiter->proc) proc_wake(waiter->proc);
                woken++;
                futex_wake_count++;
            } else if (requeued < requeue_count) {
                remove_waiter(b1, waiter);
                waiter->addr = addr2;
                add_waiter(b2, waiter);
                requeued++;
            }
        }
        waiter = next;
    }

    if (h1 == h2) {
        spinlock_release_irqrestore(&b1->lock, f);
    } else if (h1 < h2) {
        spinlock_release(&b2->lock);
        spinlock_release_irqrestore(&b1->lock, f);
    } else {
        spinlock_release(&b1->lock);
        spinlock_release_irqrestore(&b2->lock, f);
    }
    return woken + requeued;
}

int futex_cmp_requeue(uint32_t *addr, uint32_t *addr2, uint32_t expected,
                      int wake_count, int requeue_count) {
    if (!validate_futex_addr(addr)) return -FUTEX_EFAULT;
    if (*addr != expected) return -FUTEX_EAGAIN;
    return futex_requeue(addr, addr2, wake_count, requeue_count);
}

void futex_wake_addr(uint32_t *addr, int count) {
    futex_wake(addr, count, 0xFFFFFFFF);
}

// ============================================================================
// Syscall handler
// ============================================================================

int64_t sys_futex(uint32_t *addr, int op, uint32_t val, uint64_t timeout,
                  uint32_t *addr2, uint32_t val3) {
    int cmd = op & FUTEX_CMD_MASK;
    switch (cmd) {
        case FUTEX_WAIT:        return futex_wait(addr, val, timeout, 0xFFFFFFFF);
        case FUTEX_WAKE:        return futex_wake(addr, (int)val, 0xFFFFFFFF);
        case FUTEX_REQUEUE:     return futex_requeue(addr, addr2, (int)val, (int)timeout);
        case FUTEX_CMP_REQUEUE: return futex_cmp_requeue(addr, addr2, val3, (int)val, (int)timeout);
        case FUTEX_WAIT_BITSET: return futex_wait(addr, val, timeout, val3);
        case FUTEX_WAKE_BITSET: return futex_wake(addr, (int)val, val3);
        default:
            kprintf("[FUTEX] Unknown operation: %d\n", cmd);
            return -FUTEX_EINVAL;
    }
}

// ============================================================================
// Timer tick - fire timed-out waiters. Called from sched_tick().
// ============================================================================

void futex_tick(void) {
    for (int i = 0; i < FUTEX_HASH_BUCKETS; i++) {
        futex_bucket_t *bucket = &futex_buckets[i];
        if (bucket->waiter_count == 0) continue;   // cheap unlocked early-out

        uint64_t f = spinlock_acquire_irqsave(&bucket->lock);
        futex_waiter_t *waiter = bucket->waiters;
        while (waiter) {
            futex_waiter_t *next = waiter->next;
            if (waiter->timeout > 0 && timer_ticks >= waiter->timeout && !waiter->done) {
                remove_waiter(bucket, waiter);
                waiter->timed_out = true;
                waiter->done = 1;
                if (waiter->proc) proc_wake(waiter->proc);
            }
            waiter = next;
        }
        spinlock_release_irqrestore(&bucket->lock, f);
    }
}

// ============================================================================
// Debug
// ============================================================================

void futex_print_stats(void) {
    kprintf("\n=== Futex Statistics ===\n");
    kprintf("Wait calls:    %lu\n", futex_wait_count);
    kprintf("Wake calls:    %lu\n", futex_wake_count);
    kprintf("Timeouts:      %lu\n", futex_timeout_count);
    uint32_t total = 0;
    for (int i = 0; i < FUTEX_HASH_BUCKETS; i++) total += futex_buckets[i].waiter_count;
    kprintf("Current waiters: %u\n", total);
    kprintf("========================\n\n");
}

void futex_print_waiters(void) {
    kprintf("\n=== Futex Waiters ===\n");
    for (int i = 0; i < FUTEX_HASH_BUCKETS; i++) {
        futex_bucket_t *bucket = &futex_buckets[i];
        if (bucket->waiter_count > 0) {
            uint64_t f = spinlock_acquire_irqsave(&bucket->lock);
            kprintf("Bucket %d (%u waiters):\n", i, bucket->waiter_count);
            for (futex_waiter_t *w = bucket->waiters; w; w = w->next) {
                kprintf("  addr=%p pid=%u bitset=0x%x timeout=%lu\n",
                        w->addr, w->proc ? w->proc->pid : 0, w->bitset, w->timeout);
            }
            spinlock_release_irqrestore(&bucket->lock, f);
        }
    }
    kprintf("=====================\n\n");
}
