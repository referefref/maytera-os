// spinlock.h - SMP-safe spinlock primitives for MayteraOS
// Part of Task #41 (SMP Support)
//
// These spinlocks use atomic compare-and-swap operations to ensure
// mutual exclusion across multiple CPUs. They include:
// - Basic spinlock (busy-wait)
// - Ticket spinlock (fair, FIFO ordering)
// - Read-write spinlock (multiple readers, single writer)
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
// Ticket Spinlock (Fair)
// ============================================================================

// Ticket spinlock - guarantees FIFO ordering
// Uses ticket/turn mechanism like a deli counter
typedef struct {
    volatile uint32_t next_ticket;  // Next ticket to be issued
    volatile uint32_t now_serving;  // Current ticket being served
#ifdef SMP_DEBUG
    uint32_t owner_cpu;
    const char *name;
    uint64_t acquire_count;
    uint64_t spin_count;
#endif
} ticket_lock_t;

// Static initializer
#ifdef SMP_DEBUG
#define TICKET_LOCK_INIT { .next_ticket = 0, .now_serving = 0, .owner_cpu = 0xFFFFFFFF, .name = NULL, .acquire_count = 0, .spin_count = 0 }
#else
#define TICKET_LOCK_INIT { .next_ticket = 0, .now_serving = 0 }
#endif

// Initialize a ticket lock
void ticket_lock_init(ticket_lock_t *lock);

// Acquire ticket lock (waits in FIFO order)
void ticket_lock_acquire(ticket_lock_t *lock);

// Try to acquire ticket lock (non-blocking)
int ticket_lock_try_acquire(ticket_lock_t *lock);

// Release ticket lock
void ticket_lock_release(ticket_lock_t *lock);

// ============================================================================
// Read-Write Spinlock
// ============================================================================

// Read-write spinlock - allows multiple readers OR single writer
// Writers have priority to prevent starvation
typedef struct {
    volatile int32_t readers;       // Number of readers (negative = writer waiting)
    volatile uint32_t writer;       // 0 = no writer, 1 = writer active
    spinlock_t wait_lock;           // Serialize writer access
#ifdef SMP_DEBUG
    const char *name;
#endif
} rwlock_t;

// Static initializer
#ifdef SMP_DEBUG
#define RWLOCK_INIT { .readers = 0, .writer = 0, .wait_lock = SPINLOCK_INIT, .name = NULL }
#else
#define RWLOCK_INIT { .readers = 0, .writer = 0, .wait_lock = SPINLOCK_INIT }
#endif

// Initialize a read-write lock
void rwlock_init(rwlock_t *lock);

// Acquire read lock (shared access)
void rwlock_read_acquire(rwlock_t *lock);

// Release read lock
void rwlock_read_release(rwlock_t *lock);

// Acquire write lock (exclusive access)
void rwlock_write_acquire(rwlock_t *lock);

// Release write lock
void rwlock_write_release(rwlock_t *lock);

// Try to acquire read lock (non-blocking)
int rwlock_try_read_acquire(rwlock_t *lock);

// Try to acquire write lock (non-blocking)
int rwlock_try_write_acquire(rwlock_t *lock);

// ============================================================================
// Interrupt-safe Spinlock Operations
// ============================================================================

// These versions disable interrupts while holding the lock
// Use when lock may be acquired from interrupt context

// Save interrupt state, disable interrupts, then acquire lock
uint64_t spinlock_acquire_irqsave(spinlock_t *lock);

// Release lock and restore interrupt state
void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags);

// Same for ticket locks
uint64_t ticket_lock_acquire_irqsave(ticket_lock_t *lock);
void ticket_lock_release_irqrestore(ticket_lock_t *lock, uint64_t flags);

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
