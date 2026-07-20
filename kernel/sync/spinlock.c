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
// Ticket Spinlock Implementation
// ============================================================================

void ticket_lock_init(ticket_lock_t *lock) {
    lock->next_ticket = 0;
    lock->now_serving = 0;
#ifdef SMP_DEBUG
    lock->owner_cpu = 0xFFFFFFFF;
    lock->name = NULL;
    lock->acquire_count = 0;
    lock->spin_count = 0;
#endif
}

void ticket_lock_acquire(ticket_lock_t *lock) {
    // Get our ticket number (atomic increment)
    uint32_t my_ticket = atomic_fetch_add32(&lock->next_ticket, 1);
    
    // Wait until it's our turn
    while (atomic_load32(&lock->now_serving) != my_ticket) {
        pause();
#ifdef SMP_DEBUG
        lock->spin_count++;
#endif
    }
    
    compiler_barrier();
    
#ifdef SMP_DEBUG
    lock->owner_cpu = smp_get_cpu_id();
    lock->acquire_count++;
#endif
}

int ticket_lock_try_acquire(ticket_lock_t *lock) {
    uint32_t current_ticket = atomic_load32(&lock->next_ticket);
    uint32_t serving = atomic_load32(&lock->now_serving);
    
    // Only try if no one is waiting
    if (current_ticket == serving) {
        // Try to get the next ticket
        if (atomic_cas32(&lock->next_ticket, current_ticket, current_ticket + 1) == current_ticket) {
            // We got it!
            compiler_barrier();
#ifdef SMP_DEBUG
            lock->owner_cpu = smp_get_cpu_id();
            lock->acquire_count++;
#endif
            return 1;
        }
    }
    return 0;
}

void ticket_lock_release(ticket_lock_t *lock) {
    compiler_barrier();
    
#ifdef SMP_DEBUG
    lock->owner_cpu = 0xFFFFFFFF;
#endif
    
    // Advance to next ticket
    atomic_inc32(&lock->now_serving);
}

// ============================================================================
// Read-Write Spinlock Implementation
// ============================================================================

void rwlock_init(rwlock_t *lock) {
    lock->readers = 0;
    lock->writer = 0;
    spinlock_init(&lock->wait_lock);
#ifdef SMP_DEBUG
    lock->name = NULL;
#endif
}

void rwlock_read_acquire(rwlock_t *lock) {
    while (1) {
        // Wait for no active or waiting writers
        while (atomic_load32(&lock->writer) != 0) {
            pause();
        }
        
        // Increment reader count
        atomic_inc32((volatile uint32_t *)&lock->readers);
        
        // Check if a writer snuck in
        if (atomic_load32(&lock->writer) == 0) {
            // Success!
            break;
        }
        
        // Writer is present, back off
        atomic_dec32((volatile uint32_t *)&lock->readers);
    }
    
    compiler_barrier();
}

void rwlock_read_release(rwlock_t *lock) {
    compiler_barrier();
    atomic_dec32((volatile uint32_t *)&lock->readers);
}

void rwlock_write_acquire(rwlock_t *lock) {
    // Serialize writers
    spinlock_acquire(&lock->wait_lock);
    
    // Signal writer waiting (prevents new readers)
    atomic_store32(&lock->writer, 1);
    
    // Wait for all readers to finish
    while (atomic_load32((volatile uint32_t *)&lock->readers) != 0) {
        pause();
    }
    
    compiler_barrier();
}

void rwlock_write_release(rwlock_t *lock) {
    compiler_barrier();
    
    // Clear writer flag
    atomic_store32(&lock->writer, 0);
    
    // Release serialization lock
    spinlock_release(&lock->wait_lock);
}

int rwlock_try_read_acquire(rwlock_t *lock) {
    // Check for writer
    if (atomic_load32(&lock->writer) != 0) {
        return 0;
    }
    
    // Try to increment reader count
    atomic_inc32((volatile uint32_t *)&lock->readers);
    
    // Verify no writer
    if (atomic_load32(&lock->writer) == 0) {
        compiler_barrier();
        return 1;
    }
    
    // Writer present, back off
    atomic_dec32((volatile uint32_t *)&lock->readers);
    return 0;
}

int rwlock_try_write_acquire(rwlock_t *lock) {
    // Try to get the serialization lock
    if (!spinlock_try_acquire(&lock->wait_lock)) {
        return 0;
    }
    
    // Signal writer
    atomic_store32(&lock->writer, 1);
    
    // Check for readers
    if (atomic_load32((volatile uint32_t *)&lock->readers) == 0) {
        compiler_barrier();
        return 1;
    }
    
    // Readers present, back off
    atomic_store32(&lock->writer, 0);
    spinlock_release(&lock->wait_lock);
    return 0;
}

// ============================================================================
// Interrupt-safe Spinlock Operations
// ============================================================================

uint64_t spinlock_acquire_irqsave(spinlock_t *lock) {
    uint64_t flags = irq_save();
    spinlock_acquire(lock);
    return flags;
}

void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags) {
    spinlock_release(lock);
    irq_restore(flags);
}

uint64_t ticket_lock_acquire_irqsave(ticket_lock_t *lock) {
    uint64_t flags = irq_save();
    ticket_lock_acquire(lock);
    return flags;
}

void ticket_lock_release_irqrestore(ticket_lock_t *lock, uint64_t flags) {
    ticket_lock_release(lock);
    irq_restore(flags);
}
