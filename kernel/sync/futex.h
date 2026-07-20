// futex.h - Fast Userspace Mutex (Futex) for MayteraOS
// Part of Task #25 (Threading with clone() syscall)
//
// Futexes provide efficient thread synchronization by only entering
// the kernel when contention occurs. The key insight is that uncontended
// locks can be acquired/released entirely in userspace.
//
// Futex operations:
// - FUTEX_WAIT: Sleep if *addr == val
// - FUTEX_WAKE: Wake up to N waiters
//
// This enables efficient implementation of:
// - Mutexes (pthread_mutex)
// - Condition variables (pthread_cond)
// - Semaphores
// - Barriers
// - Reader-writer locks

#ifndef FUTEX_H
#define FUTEX_H

#include "../types.h"
#include "spinlock.h"

// ============================================================================
// Futex Operations (compatible with Linux)
// ============================================================================

#define FUTEX_WAIT          0   // Sleep if *addr == val
#define FUTEX_WAKE          1   // Wake up to val waiters
#define FUTEX_FD            2   // (deprecated, not implemented)
#define FUTEX_REQUEUE       3   // Requeue waiters to another futex
#define FUTEX_CMP_REQUEUE   4   // Conditional requeue
#define FUTEX_WAKE_OP       5   // Wake with operation on second futex
#define FUTEX_LOCK_PI       6   // Priority inheritance lock
#define FUTEX_UNLOCK_PI     7   // Priority inheritance unlock
#define FUTEX_TRYLOCK_PI    8   // Try priority inheritance lock
#define FUTEX_WAIT_BITSET   9   // Wait with bitmask
#define FUTEX_WAKE_BITSET   10  // Wake with bitmask

// Futex flags (OR'd with operation)
#define FUTEX_PRIVATE_FLAG  128     // Don't share between processes
#define FUTEX_CLOCK_REALTIME 256    // Use realtime clock for timeout

// Operation mask
#define FUTEX_CMD_MASK      0x7F

// ============================================================================
// Futex Data Structures
// ============================================================================

// Maximum waiters per futex
#define FUTEX_MAX_WAITERS 64

// Forward decl: a futex waiter is a schedulable process/task (#430 - the
// futex layer blocks/wakes via the real process scheduler, not the disused
// thread.c scheduler).
struct process;

// Futex wait queue entry
typedef struct futex_waiter {
    uint32_t *addr;             // Futex address being waited on
    struct process *proc;       // Waiting task (process_t sharing an addr space)
    uint32_t bitset;            // Bitmask for selective wake
    uint64_t timeout;           // Absolute timeout in ticks (0 = infinite)
    bool timed_out;             // Set if wait timed out
    volatile int done;          // Set by wake/timeout to release the waiter
    volatile int on_bucket;     // 1 while linked into a bucket
    struct futex_waiter *next;  // Next waiter in queue
    struct futex_waiter *prev;  // Previous waiter in queue
} futex_waiter_t;

// Futex hash bucket
typedef struct futex_bucket {
    spinlock_t lock;            // Bucket lock
    futex_waiter_t *waiters;    // List of waiters
    uint32_t waiter_count;      // Number of waiters
} futex_bucket_t;

// Number of hash buckets (power of 2)
#define FUTEX_HASH_BUCKETS 256
#define FUTEX_HASH_MASK (FUTEX_HASH_BUCKETS - 1)

// ============================================================================
// Futex API
// ============================================================================

/**
 * Initialize the futex subsystem
 */
void futex_init(void);

/**
 * Main futex syscall handler
 *
 * @param addr      Futex address (must be 4-byte aligned)
 * @param op        Operation (FUTEX_WAIT, FUTEX_WAKE, etc.)
 * @param val       Value (meaning depends on operation)
 * @param timeout   Timeout in milliseconds (0 = infinite for WAIT, ignored for WAKE)
 * @param addr2     Second futex address (for REQUEUE operations)
 * @param val3      Third value (for CMP_REQUEUE)
 * @return          Number of woken threads (WAKE) or 0 (WAIT), -1 on error
 */
int64_t sys_futex(uint32_t *addr, int op, uint32_t val, uint64_t timeout,
                  uint32_t *addr2, uint32_t val3);

/**
 * Wait on a futex if *addr == val
 *
 * @param addr      Futex address
 * @param val       Expected value
 * @param timeout   Timeout in milliseconds (0 = infinite)
 * @param bitset    Bitmask for selective wake (0xFFFFFFFF = any)
 * @return          0 on wake, -ETIMEDOUT on timeout, -EAGAIN if *addr != val
 */
int futex_wait(uint32_t *addr, uint32_t val, uint64_t timeout, uint32_t bitset);

/**
 * Wake threads waiting on a futex
 *
 * @param addr      Futex address
 * @param count     Maximum threads to wake
 * @param bitset    Bitmask for selective wake (0xFFFFFFFF = any)
 * @return          Number of threads woken
 */
int futex_wake(uint32_t *addr, int count, uint32_t bitset);

/**
 * Wake one waiter and requeue others to a different futex
 *
 * @param addr      Source futex address
 * @param addr2     Destination futex address
 * @param wake_count Number of threads to wake (typically 1)
 * @param requeue_count Maximum threads to requeue
 * @return          Total number of threads woken + requeued
 */
int futex_requeue(uint32_t *addr, uint32_t *addr2, int wake_count, int requeue_count);

/**
 * Conditional requeue (compare before requeueing)
 *
 * @param addr      Source futex address
 * @param addr2     Destination futex address
 * @param expected  Expected value at *addr
 * @param wake_count Number of threads to wake
 * @param requeue_count Maximum threads to requeue
 * @return          Total count, or -EAGAIN if *addr != expected
 */
int futex_cmp_requeue(uint32_t *addr, uint32_t *addr2, uint32_t expected,
                      int wake_count, int requeue_count);

/**
 * Wake threads at an address (for CLONE_CHILD_CLEARTID)
 * Simplified wake for use by thread exit
 *
 * @param addr      Address to wake
 * @param count     Maximum threads to wake
 */
void futex_wake_addr(uint32_t *addr, int count);

// ============================================================================
// Futex Helper Macros (for userspace)
// ============================================================================

// Error codes
#define FUTEX_EAGAIN    11  // *addr != val at check time
#define FUTEX_ETIMEDOUT 110 // Wait timed out
#define FUTEX_EINVAL    22  // Invalid argument
#define FUTEX_EFAULT    14  // Bad address

// ============================================================================
// Debug Functions
// ============================================================================

/**
 * Print futex statistics
 */
void futex_print_stats(void);

/**
 * Print all waiters (for debugging)
 */
void futex_print_waiters(void);

#endif // FUTEX_H
