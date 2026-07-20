// pthread.h - POSIX Threads API for MayteraOS
// Part of Task #25 (Threading with clone() syscall)
//
// Provides a pthread-compatible API built on top of MayteraOS's
// clone() syscall and futex synchronization primitives.

#ifndef _PTHREAD_H
#define _PTHREAD_H

#include "types.h"
#include "syscall.h"

// ============================================================================
// Thread Types
// ============================================================================

// Forward declaration so struct timespec has FILE scope (the definition lower
// in this header would otherwise give it prototype scope in
// pthread_cond_timedwait's declaration, a C tag-scope gotcha). #430.
struct timespec;

// Thread identifier. #430: this is the address of the kernel-cleared join
// word (see pthread_create/join), so it MUST be pointer-wide, not 32-bit -
// a 32-bit pthread_t truncated the handle and broke join.
typedef unsigned long pthread_t;

// Thread attributes
typedef struct {
    unsigned int flags;         // Creation flags
    void *stack_addr;           // Custom stack address (NULL = auto)
    unsigned long stack_size;   // Stack size (0 = default)
    int detach_state;           // PTHREAD_CREATE_JOINABLE or PTHREAD_CREATE_DETACHED
    int sched_policy;           // Scheduling policy
    int sched_priority;         // Scheduling priority
} pthread_attr_t;

// Mutex types
typedef struct {
    volatile unsigned int lock;     // Lock state (0=unlocked, 1=locked, 2=contended)
    volatile unsigned int owner;    // Owner thread ID
    unsigned int type;              // Mutex type
    unsigned int recursive_count;   // For recursive mutexes
} pthread_mutex_t;

// Mutex attributes
typedef struct {
    int type;                   // PTHREAD_MUTEX_NORMAL, RECURSIVE, etc.
    int pshared;               // Process-shared flag
} pthread_mutexattr_t;

// Condition variable
typedef struct {
    volatile unsigned int seq;      // Sequence number for wait/signal
    volatile unsigned int waiters;  // Number of waiting threads
    pthread_mutex_t *mutex;         // Associated mutex
} pthread_cond_t;

// Condition variable attributes
typedef struct {
    int pshared;               // Process-shared flag
} pthread_condattr_t;

// Read-write lock
typedef struct {
    volatile int readers;           // Number of readers (negative = writer waiting)
    volatile unsigned int writer;   // Writer thread ID (0 = no writer)
    pthread_mutex_t mutex;          // Internal mutex for serialization
    pthread_cond_t read_cond;       // Condition for readers
    pthread_cond_t write_cond;      // Condition for writers
} pthread_rwlock_t;

// Read-write lock attributes
typedef struct {
    int pshared;
} pthread_rwlockattr_t;

// Barrier
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned int count;             // Number of threads to wait for
    volatile unsigned int waiting;  // Current number waiting
    volatile unsigned int phase;    // Barrier phase (toggles each time)
} pthread_barrier_t;

// Barrier attributes
typedef struct {
    int pshared;
} pthread_barrierattr_t;

// Once control
typedef struct {
    volatile int done;
    pthread_mutex_t mutex;
} pthread_once_t;

// Thread-specific key
typedef unsigned int pthread_key_t;

// Spinlock
typedef volatile unsigned int pthread_spinlock_t;

// ============================================================================
// Constants
// ============================================================================

// Thread creation state
#define PTHREAD_CREATE_JOINABLE     0
#define PTHREAD_CREATE_DETACHED     1

// Mutex types
#define PTHREAD_MUTEX_NORMAL        0
#define PTHREAD_MUTEX_RECURSIVE     1
#define PTHREAD_MUTEX_ERRORCHECK    2
#define PTHREAD_MUTEX_DEFAULT       PTHREAD_MUTEX_NORMAL

// Process shared
#define PTHREAD_PROCESS_PRIVATE     0
#define PTHREAD_PROCESS_SHARED      1

// Cancellation
#define PTHREAD_CANCEL_ENABLE       0
#define PTHREAD_CANCEL_DISABLE      1
#define PTHREAD_CANCEL_DEFERRED     0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_CANCELED            ((void*)-1)

// Barrier wait return value for one thread
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)

// Static initializers
#define PTHREAD_MUTEX_INITIALIZER       { 0, 0, 0, 0 }
#define PTHREAD_COND_INITIALIZER        { 0, 0, 0 }
#define PTHREAD_RWLOCK_INITIALIZER      { 0, 0, PTHREAD_MUTEX_INITIALIZER, \
                                          PTHREAD_COND_INITIALIZER, \
                                          PTHREAD_COND_INITIALIZER }
#define PTHREAD_ONCE_INIT               { 0, PTHREAD_MUTEX_INITIALIZER }

// Default stack size
#define PTHREAD_STACK_MIN           (16 * 1024)     // 16KB minimum
#define PTHREAD_STACK_DEFAULT       (64 * 1024)     // 64KB default

// Maximum keys
#define PTHREAD_KEYS_MAX            128

// ============================================================================
// Thread Management
// ============================================================================

/**
 * Create a new thread
 *
 * @param thread    Pointer to store thread ID
 * @param attr      Thread attributes (NULL for defaults)
 * @param start     Thread entry function
 * @param arg       Argument to entry function
 * @return          0 on success, error code on failure
 */
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg);

/**
 * Wait for a thread to terminate
 *
 * @param thread    Thread to wait for
 * @param retval    Pointer to store return value (can be NULL)
 * @return          0 on success, error code on failure
 */
int pthread_join(pthread_t thread, void **retval);

/**
 * Detach a thread (resources freed on termination)
 *
 * @param thread    Thread to detach
 * @return          0 on success, error code on failure
 */
int pthread_detach(pthread_t thread);

/**
 * Terminate the calling thread
 *
 * @param retval    Return value
 */
void pthread_exit(void *retval) __attribute__((noreturn));

/**
 * Get the calling thread's ID
 *
 * @return          Thread ID
 */
pthread_t pthread_self(void);

/**
 * Compare two thread IDs
 *
 * @param t1, t2    Thread IDs to compare
 * @return          Non-zero if equal, 0 if different
 */
int pthread_equal(pthread_t t1, pthread_t t2);

/**
 * Yield the processor to another thread
 *
 * @return          0 on success
 */
int pthread_yield(void);

// ============================================================================
// Thread Attributes
// ============================================================================

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate);
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate);
int pthread_attr_setstacksize(pthread_attr_t *attr, unsigned long stacksize);
int pthread_attr_getstacksize(const pthread_attr_t *attr, unsigned long *stacksize);
int pthread_attr_setstack(pthread_attr_t *attr, void *stackaddr, unsigned long stacksize);
int pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr, unsigned long *stacksize);

// ============================================================================
// Mutex Operations
// ============================================================================

/**
 * Initialize a mutex
 *
 * @param mutex     Mutex to initialize
 * @param attr      Mutex attributes (NULL for defaults)
 * @return          0 on success, error code on failure
 */
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);

/**
 * Destroy a mutex
 *
 * @param mutex     Mutex to destroy
 * @return          0 on success, error code on failure
 */
int pthread_mutex_destroy(pthread_mutex_t *mutex);

/**
 * Lock a mutex (blocking)
 *
 * @param mutex     Mutex to lock
 * @return          0 on success, error code on failure
 */
int pthread_mutex_lock(pthread_mutex_t *mutex);

/**
 * Try to lock a mutex (non-blocking)
 *
 * @param mutex     Mutex to lock
 * @return          0 if locked, EBUSY if already locked
 */
int pthread_mutex_trylock(pthread_mutex_t *mutex);

/**
 * Unlock a mutex
 *
 * @param mutex     Mutex to unlock
 * @return          0 on success, error code on failure
 */
int pthread_mutex_unlock(pthread_mutex_t *mutex);

// Mutex attributes
int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type);

// ============================================================================
// Condition Variable Operations
// ============================================================================

/**
 * Initialize a condition variable
 *
 * @param cond      Condition variable to initialize
 * @param attr      Attributes (NULL for defaults)
 * @return          0 on success
 */
int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);

/**
 * Destroy a condition variable
 *
 * @param cond      Condition variable to destroy
 * @return          0 on success
 */
int pthread_cond_destroy(pthread_cond_t *cond);

/**
 * Wait on a condition variable
 * Atomically unlocks mutex and blocks until signaled
 *
 * @param cond      Condition variable
 * @param mutex     Mutex (must be locked)
 * @return          0 on success
 */
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);

/**
 * Wait on a condition variable with timeout
 *
 * @param cond      Condition variable
 * @param mutex     Mutex (must be locked)
 * @param abstime   Absolute timeout
 * @return          0 on success, ETIMEDOUT on timeout
 */
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime);

/**
 * Signal one waiting thread
 *
 * @param cond      Condition variable
 * @return          0 on success
 */
int pthread_cond_signal(pthread_cond_t *cond);

/**
 * Signal all waiting threads
 *
 * @param cond      Condition variable
 * @return          0 on success
 */
int pthread_cond_broadcast(pthread_cond_t *cond);

// Condition variable attributes
int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);

// ============================================================================
// Read-Write Lock Operations
// ============================================================================

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr);
int pthread_rwlock_destroy(pthread_rwlock_t *rwlock);
int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_unlock(pthread_rwlock_t *rwlock);

// ============================================================================
// Barrier Operations
// ============================================================================

int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr,
                         unsigned int count);
int pthread_barrier_destroy(pthread_barrier_t *barrier);
int pthread_barrier_wait(pthread_barrier_t *barrier);

// ============================================================================
// Once Control
// ============================================================================

/**
 * Execute initialization function exactly once
 *
 * @param once      Once control variable
 * @param init      Initialization function
 * @return          0 on success
 */
int pthread_once(pthread_once_t *once, void (*init)(void));

// ============================================================================
// Thread-Specific Data (TSD)
// ============================================================================

int pthread_key_create(pthread_key_t *key, void (*destructor)(void*));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);

// ============================================================================
// Spinlock Operations
// ============================================================================

int pthread_spin_init(pthread_spinlock_t *lock, int pshared);
int pthread_spin_destroy(pthread_spinlock_t *lock);
int pthread_spin_lock(pthread_spinlock_t *lock);
int pthread_spin_trylock(pthread_spinlock_t *lock);
int pthread_spin_unlock(pthread_spinlock_t *lock);

// ============================================================================
// Cancellation (stubs for compatibility)
// ============================================================================

int pthread_setcancelstate(int state, int *oldstate);
int pthread_setcanceltype(int type, int *oldtype);
void pthread_testcancel(void);

// ============================================================================
// Error Codes
// ============================================================================

#define EAGAIN      11
#define EBUSY       16
#define EINVAL      22
#define ENOMEM      12
#define EPERM       1
#define ESRCH       3
#define ETIMEDOUT   110
#define EDEADLK     35

// ============================================================================
// Timespec structure (if not defined elsewhere)
// ============================================================================

#ifndef _TIMESPEC_DEFINED
#define _TIMESPEC_DEFINED
struct timespec {
    long tv_sec;    // Seconds
    long tv_nsec;   // Nanoseconds
};
#endif

#endif // _PTHREAD_H
