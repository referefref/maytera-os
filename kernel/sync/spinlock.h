// spinlock.h - SMP-safe spinlock primitives for MayteraOS
// Part of Task #41 (SMP Support)
//
// These spinlocks use atomic compare-and-swap operations to ensure
// mutual exclusion across multiple CPUs. There is ONE lock type here: the
// basic test-and-test-and-set spinlock, with an irqsave form and an optional
// contention-accounting form.
//
// TICKET (FAIR) LOCKS AND READER-WRITER LOCKS WERE HERE AND WERE DELETED
// (2026-08-23). Do not re-add either without first invalidating the
// measurements below. They were written for Task #41 alongside this file, were
// never acquired ONCE anywhere in the kernel, and could not have helped:
//
//  1. THE SHIPPING KERNEL RUNS ON ONE CPU. cpu/smp.c: g_smp_user_sched = 0,
//     and main.c gates smp_start_aps() behind it, so no application processor
//     is ever started unless /SMPSCHED.TXT is dropped on the ESP. With one
//     core there is no second core to read in parallel with (the rwlock has no
//     purpose) and none to queue behind (the ticket lock has no purpose). A
//     spinlock here is an interrupt-masking device, not a contention one.
//  2. EVEN WITH APs ON, a whole-kernel BKL serialises all kernel execution:
//     cpu/smp.c g_smp_bkl_full = 1, and cpu/idt.c:201 (every ISR),
//     proc/syscall.asm:89 (every SYSCALL) and proc/process.c:3473 (every
//     kernel thread) take it. Two cores cannot be inside a kernel read
//     critical section at the same time, so an rwlock cannot admit a second
//     reader no matter which table it guards.
//  3. THE TICKET LOCK IS STRUCTURALLY UNUSABLE AT THE ONE LOCK THAT DOES
//     CONTEND. The BKL (cpu/smp.c bkl_take_locked) is recursive, and its wait
//     loop deliberately runs with interrupts ENABLED, so a waiter can be
//     interrupted and cpu/idt.c will call bkl_acquire() again on the same
//     core. Under FIFO that nested acquire takes ticket N+1 while the
//     interrupted context still holds ticket N: now_serving reaches N and the
//     only context that can consume it is stuck below the nested wait for N+1.
//     That is a hard deadlock on the first preempted contended acquire.
//  4. THE RWLOCK HAD NO IRQSAVE FORM AT ALL, and 98 of the kernel's 125
//     spinlock acquisition sites are spinlock_acquire_irqsave precisely because
//     their data is touched from interrupt context. On a single-CPU kernel a
//     lock whose holder CAN be preempted is a hang generator: the waiter spins
//     forever because the holder can never be scheduled to release it.
//     Adopting the rwlock anywhere real meant first writing four new irqsave
//     entry points, i.e. new untested locking code to serve no measured need.
//
// If you need a read-mostly primitive, sync/seqlock.h already exists and is
// live (proc/syscall.c window content commit). If you need to wait, use
// sync/waitq.h or sync/futex.c. See CHANGELOG 2026-08-23 for the full survey.
//
// IMPORTANT: Never hold a spinlock across operations that might sleep!

#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "../types.h"

// ============================================================================
// Basic Spinlock
// ============================================================================

// Simple spinlock - uses atomic test-and-set
// WARNING: Not fair - can cause starvation under contention
typedef struct {
    volatile uint32_t locked;       // 0 = unlocked, 1 = locked
#ifdef SMP_DEBUG
    uint32_t owner_cpu;             // CPU that holds the lock (for debugging)
    const char *name;               // Lock name (for debugging)
    uint64_t acquire_count;         // Number of times acquired
    uint64_t spin_count;            // Total spins while waiting
#endif
} spinlock_t;

// Static initializer
#ifdef SMP_DEBUG
#define SPINLOCK_INIT { .locked = 0, .owner_cpu = 0xFFFFFFFF, .name = NULL, .acquire_count = 0, .spin_count = 0 }
#define SPINLOCK_INIT_NAMED(n) { .locked = 0, .owner_cpu = 0xFFFFFFFF, .name = (n), .acquire_count = 0, .spin_count = 0 }
#else
#define SPINLOCK_INIT { .locked = 0 }
#define SPINLOCK_INIT_NAMED(n) { .locked = 0 }
#endif

// Initialize a spinlock
void spinlock_init(spinlock_t *lock);

// Initialize with a name (for debugging)
void spinlock_init_named(spinlock_t *lock, const char *name);

// Acquire lock (busy-wait until acquired)
void spinlock_acquire(spinlock_t *lock);

// Try to acquire lock (non-blocking)
// Returns 1 if lock acquired, 0 if lock was held
int spinlock_try_acquire(spinlock_t *lock);

// Release lock
void spinlock_release(spinlock_t *lock);

// Check if lock is held (for assertions)
int spinlock_is_locked(spinlock_t *lock);

// ============================================================================
// Interrupt-safe Spinlock Operations
// ============================================================================

// These versions disable interrupts while holding the lock
// Use when lock may be acquired from interrupt context

// Save interrupt state, disable interrupts, then acquire lock
uint64_t spinlock_acquire_irqsave(spinlock_t *lock);

// ---------------------------------------------------------------------------
// #143: OPTIONAL CONTENTION ACCOUNTING, on the shared primitive.
// ---------------------------------------------------------------------------
// #143 was raised as "8 run queues share one global lock". Before restructuring
// the lock, the contention had to be a measured number rather than an adjective,
// and there was no way to get one: the BKL has per-core acquire/contend/spin
// counters (cpu/smp.c) but every other spinlock in the kernel has none outside
// the SMP_DEBUG build, which is not what ships.
//
// This is deliberately an OPTION ON THE SHARED LOCK, not a private lock type in
// the scheduler. Forking a counting spinlock into proc/ would have been the
// exact anti-pattern this project keeps paying for; the primitive gets better
// for everyone instead.
//
// It adds NO new spin loop. The accounting variant tries once, and on failure
// falls into the SAME spinlock_acquire() every other caller uses, so there is
// still one spin implementation in the kernel and the concurrency lint has
// nothing new to look at.
//
// The counters are per-LOCK, not per-core, and are updated with lock-prefixed
// atomics because the contended case is by definition concurrent. They are
// DIAGNOSTIC: a torn read of a pair is acceptable and the consumer
// (rustkern/rqlock.rs) is written to tolerate contended > acquires rather than
// assume it cannot happen.
typedef struct {
    volatile uint64_t acquires;    // acquisitions attempted and completed
    volatile uint64_t contended;   // of those, how many found the lock held
} spin_acct_t;

#define SPIN_ACCT_INIT { .acquires = 0, .contended = 0 }

// Exactly spinlock_acquire_irqsave(), plus accounting. A NULL acct is legal and
// makes this identical to the plain form.
uint64_t spinlock_acquire_irqsave_acct(spinlock_t *lock, spin_acct_t *acct);


// Release lock and restore interrupt state
void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags);

// ============================================================================
// Atomic Operations
// ============================================================================

// Atomic compare-and-swap (CAS)
// Returns old value; if old == expected, sets *ptr = new_val
static inline uint32_t atomic_cas32(volatile uint32_t *ptr, uint32_t expected, uint32_t new_val) {
    uint32_t old;
    __asm__ volatile(
        "lock cmpxchgl %2, %1"
        : "=a"(old), "+m"(*ptr)
        : "r"(new_val), "0"(expected)
        : "memory"
    );
    return old;
}

static inline uint64_t atomic_cas64(volatile uint64_t *ptr, uint64_t expected, uint64_t new_val) {
    uint64_t old;
    __asm__ volatile(
        "lock cmpxchgq %2, %1"
        : "=a"(old), "+m"(*ptr)
        : "r"(new_val), "0"(expected)
        : "memory"
    );
    return old;
}

// Atomic exchange (swap)
static inline uint32_t atomic_xchg32(volatile uint32_t *ptr, uint32_t new_val) {
    uint32_t old;
    __asm__ volatile(
        "xchgl %0, %1"
        : "=r"(old), "+m"(*ptr)
        : "0"(new_val)
        : "memory"
    );
    return old;
}

static inline uint64_t atomic_xchg64(volatile uint64_t *ptr, uint64_t new_val) {
    uint64_t old;
    __asm__ volatile(
        "xchgq %0, %1"
        : "=r"(old), "+m"(*ptr)
        : "0"(new_val)
        : "memory"
    );
    return old;
}

// Atomic fetch-and-add
static inline uint32_t atomic_fetch_add32(volatile uint32_t *ptr, uint32_t val) {
    uint32_t old;
    __asm__ volatile(
        "lock xaddl %0, %1"
        : "=r"(old), "+m"(*ptr)
        : "0"(val)
        : "memory"
    );
    return old;
}

static inline uint64_t atomic_fetch_add64(volatile uint64_t *ptr, uint64_t val) {
    uint64_t old;
    __asm__ volatile(
        "lock xaddq %0, %1"
        : "=r"(old), "+m"(*ptr)
        : "0"(val)
        : "memory"
    );
    return old;
}

// Atomic increment/decrement
static inline void atomic_inc32(volatile uint32_t *ptr) {
    __asm__ volatile("lock incl %0" : "+m"(*ptr) : : "memory");
}

static inline void atomic_dec32(volatile uint32_t *ptr) {
    __asm__ volatile("lock decl %0" : "+m"(*ptr) : : "memory");
}

static inline void atomic_inc64(volatile uint64_t *ptr) {
    __asm__ volatile("lock incq %0" : "+m"(*ptr) : : "memory");
}

static inline void atomic_dec64(volatile uint64_t *ptr) {
    __asm__ volatile("lock decq %0" : "+m"(*ptr) : : "memory");
}

// Atomic load (with memory barrier)
static inline uint32_t atomic_load32(volatile uint32_t *ptr) {
    uint32_t val;
    __asm__ volatile(
        "movl %1, %0"
        : "=r"(val)
        : "m"(*ptr)
        : "memory"
    );
    return val;
}

static inline uint64_t atomic_load64(volatile uint64_t *ptr) {
    uint64_t val;
    __asm__ volatile(
        "movq %1, %0"
        : "=r"(val)
        : "m"(*ptr)
        : "memory"
    );
    return val;
}

// Atomic store (with memory barrier)
static inline void atomic_store32(volatile uint32_t *ptr, uint32_t val) {
    __asm__ volatile(
        "movl %1, %0"
        : "=m"(*ptr)
        : "r"(val)
        : "memory"
    );
}

static inline void atomic_store64(volatile uint64_t *ptr, uint64_t val) {
    __asm__ volatile(
        "movq %1, %0"
        : "=m"(*ptr)
        : "r"(val)
        : "memory"
    );
}

// ============================================================================
// Memory Barriers
// ============================================================================

// Full memory barrier (serialize all memory operations)
static inline void memory_barrier(void) {
    __asm__ volatile("mfence" ::: "memory");
}

// Read memory barrier (serialize reads)
static inline void read_barrier(void) {
    __asm__ volatile("lfence" ::: "memory");
}

// Write memory barrier (serialize writes)
static inline void write_barrier(void) {
    __asm__ volatile("sfence" ::: "memory");
}

// Compiler barrier only (prevents reordering by compiler)
static inline void compiler_barrier(void) {
    __asm__ volatile("" ::: "memory");
}

#endif // SPINLOCK_H
