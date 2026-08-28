// spinlock.c - SMP-safe spinlock primitives implementation
// Part of Task #41 (SMP Support)

#include "spinlock.h"
#include "../serial.h"

// Forward declaration for smp_get_cpu_id() - will be in smp.h
extern uint32_t smp_get_cpu_id(void);

// ============================================================================
// Interrupt Flag Management
// ============================================================================

// Save interrupt state and disable interrupts
static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile(
        "pushfq\n\t"
        "popq %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );
    return flags;
}

// Restore interrupt state
static inline void irq_restore(uint64_t flags) {
    __asm__ volatile(
        "pushq %0\n\t"
        "popfq"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

// ============================================================================
// Basic Spinlock Implementation
// ============================================================================

void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
#ifdef SMP_DEBUG
    lock->owner_cpu = 0xFFFFFFFF;
    lock->name = NULL;
    lock->acquire_count = 0;
    lock->spin_count = 0;
#endif
}

void spinlock_init_named(spinlock_t *lock, const char *name) {
    spinlock_init(lock);
#ifdef SMP_DEBUG
    lock->name = name;
#else
    (void)name;  // Suppress unused warning
#endif
}

void spinlock_acquire(spinlock_t *lock) {
    // Spin until we acquire the lock
    // Use test-and-test-and-set for better performance:
    // First test without atomic op, then try atomic swap
    while (1) {
        // Spin while lock appears held (cache-friendly)
        while (atomic_load32(&lock->locked)) {
            // Hint to CPU that we're in a spin loop
            pause();
#ifdef SMP_DEBUG
            lock->spin_count++;
#endif
        }
        
        // Try to acquire with atomic swap
        if (atomic_xchg32(&lock->locked, 1) == 0) {
            // Successfully acquired
            break;
        }
        // Someone else grabbed it, keep spinning
    }
    
    // Memory barrier to ensure subsequent reads see effects of prior writes
    compiler_barrier();
    
#ifdef SMP_DEBUG
    lock->owner_cpu = smp_get_cpu_id();
    lock->acquire_count++;
#endif
}

int spinlock_try_acquire(spinlock_t *lock) {
    // Try once, return immediately
    if (atomic_xchg32(&lock->locked, 1) == 0) {
        compiler_barrier();
#ifdef SMP_DEBUG
        lock->owner_cpu = smp_get_cpu_id();
        lock->acquire_count++;
#endif
        return 1;  // Acquired
    }
    return 0;  // Not acquired
}

void spinlock_release(spinlock_t *lock) {
    // Memory barrier to ensure all writes complete before releasing
    compiler_barrier();
    
#ifdef SMP_DEBUG
    lock->owner_cpu = 0xFFFFFFFF;
#endif
    
    // Simple store to release (no atomic needed for release)
    atomic_store32(&lock->locked, 0);
}

int spinlock_is_locked(spinlock_t *lock) {
    return atomic_load32(&lock->locked) != 0;
}

// ============================================================================
// Interrupt-safe Spinlock Operations
// ============================================================================

uint64_t spinlock_acquire_irqsave(spinlock_t *lock) {
    uint64_t flags = irq_save();
    spinlock_acquire(lock);
    return flags;
}

// #143: spinlock_acquire_irqsave() with contention accounting. See spinlock.h.
//
// The single xchg below is the ordinary uncontended fast path, not a new spin:
// if it wins, we hold the lock and are done; if it loses, we have learned that
// the lock was contended and hand off to the shared spinlock_acquire(), which
// is the only spin loop in this file. Losing the race here and then winning
// immediately inside spinlock_acquire() counts as contended, which is correct:
// the acquisition did have to wait for another core, however briefly.
uint64_t spinlock_acquire_irqsave_acct(spinlock_t *lock, spin_acct_t *acct) {
    uint64_t flags = irq_save();
    if (!acct) {
        spinlock_acquire(lock);
        return flags;
    }
    atomic_inc64(&acct->acquires);
    if (atomic_xchg32(&lock->locked, 1) == 0) {
        return flags;                      // uncontended
    }
    atomic_inc64(&acct->contended);
    spinlock_acquire(lock);
    return flags;
}

void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags) {
    spinlock_release(lock);
    irq_restore(flags);
}
