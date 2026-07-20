// pthread.c - POSIX Threads implementation for MayteraOS
// Part of Task #25 (Threading with clone() syscall)

#include "pthread.h"
#include "syscall.h"
#include "stdlib.h"
#include "string.h"

// ============================================================================
// Internal Helpers
// ============================================================================

// Atomic compare-and-swap
static inline int atomic_cas(volatile unsigned int *ptr, unsigned int expected, unsigned int desired) {
    unsigned int old;
    __asm__ volatile(
        "lock cmpxchgl %2, %1"
        : "=a"(old), "+m"(*ptr)
        : "r"(desired), "0"(expected)
        : "memory"
    );
    return old == expected;
}

// Atomic compare-and-swap returning the OLD value (Drepper-style, for the
// 3-state futex mutex below). #430: the mutex code needs the previous value,
// not the boolean atomic_cas above (which is correct for try/spin locks).
static inline unsigned int cmpxchg_val(volatile unsigned int *ptr, unsigned int expected, unsigned int desired) {
    unsigned int old;
    __asm__ volatile(
        "lock cmpxchgl %2, %1"
        : "=a"(old), "+m"(*ptr)
        : "r"(desired), "0"(expected)
        : "memory"
    );
    return old;
}

// Atomic exchange
static inline unsigned int atomic_xchg(volatile unsigned int *ptr, unsigned int val) {
    unsigned int old;
    __asm__ volatile(
        "xchgl %0, %1"
        : "=r"(old), "+m"(*ptr)
        : "0"(val)
        : "memory"
    );
    return old;
}

// Atomic fetch-and-add
static inline unsigned int atomic_fetch_add(volatile unsigned int *ptr, unsigned int val) {
    unsigned int old;
    __asm__ volatile(
        "lock xaddl %0, %1"
        : "=r"(old), "+m"(*ptr)
        : "0"(val)
        : "memory"
    );
    return old;
}

// Memory barrier
static inline void memory_barrier(void) {
    __asm__ volatile("mfence" ::: "memory");
}

// CPU pause (for spin loops)
static inline void cpu_pause(void) {
    __asm__ volatile("pause");
}

// futex_wait()/futex_wake() are provided by syscall.h (the kernel masks off
// the PRIVATE flag with FUTEX_CMD_MASK, so plain FUTEX_WAIT/WAKE are fine).

// ============================================================================
// Thread Wrapper
// ============================================================================

// Thread startup data (passed through clone)
typedef struct {
    void *(*start_routine)(void *);
    void *arg;
    unsigned int *tid_ptr;      // For CLONE_CHILD_SETTID
    unsigned int *clear_tid;    // For CLONE_CHILD_CLEARTID
    int detached;
} thread_start_t;

// #430: robust clone trampoline (clone_asm.asm). Stashes entry+arg on the
// child stack so the child never depends on C stack locals surviving clone.
extern long __clone_thread(unsigned int flags, void *child_stack_top,
                           unsigned int *ptid, unsigned int *ctid,
                           void (*entry)(void *), void *arg);

// Thread entry point wrapper
static void thread_entry_wrapper(void *data) {
    thread_start_t *start = (thread_start_t *)data;

    // Call the actual thread function
    void *retval = start->start_routine(start->arg);

    // Thread is ending - pthread_exit handles cleanup
    pthread_exit(retval);
}

// ============================================================================
// Thread Management
// ============================================================================

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    if (!thread || !start_routine) {
        return EINVAL;
    }

    // Determine stack size
    unsigned long stack_size = PTHREAD_STACK_DEFAULT;
    int detached = PTHREAD_CREATE_JOINABLE;

    if (attr) {
        if (attr->stack_size > 0) {
            stack_size = attr->stack_size;
        }
        detached = attr->detach_state;
    }

    // Allocate stack
    void *stack = malloc(stack_size);
    if (!stack) {
        return ENOMEM;
    }

    // Stack grows down, so pass a 16-byte-aligned top of stack.
    void *stack_top = (char *)stack + stack_size;
    stack_top = (void *)((uintptr_t)stack_top & ~(uintptr_t)0xF);

    // Allocate startup data (will be freed by thread)
    thread_start_t *start_data = (thread_start_t *)malloc(sizeof(thread_start_t));
    if (!start_data) {
        free(stack);
        return ENOMEM;
    }

    start_data->start_routine = start_routine;
    start_data->arg = arg;
    start_data->detached = detached;

    // Allocate the join word. It stays NONZERO while the thread runs and the
    // kernel (CLONE_CHILD_CLEARTID) zeroes it + futex-wakes on thread exit;
    // pthread_join() blocks on it. The pthread_t handle IS this address.
    unsigned int *tid_ptr = (unsigned int *)malloc(sizeof(unsigned int));
    if (!tid_ptr) {
        free(start_data);
        free(stack);
        return ENOMEM;
    }
    *tid_ptr = 1;  // nonzero = alive (also overwritten with the real tid by
                   // CLONE_PARENT_SETTID); the kernel clears it to 0 on exit.
    start_data->tid_ptr = tid_ptr;
    start_data->clear_tid = tid_ptr;

    // Set up clone flags for pthread.
    unsigned int flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                         CLONE_THREAD | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;

    // Robust clone: the child runs thread_entry_wrapper(start_data) on its own
    // stack via the asm trampoline; the parent gets the new tid back.
    long ret = __clone_thread(flags, stack_top, tid_ptr, tid_ptr,
                              thread_entry_wrapper, start_data);
    if (ret < 0) {
        free(tid_ptr);
        free(start_data);
        free(stack);
        return EAGAIN;
    }

    // The join handle is the address of the tid word.
    *thread = (pthread_t)(uintptr_t)tid_ptr;
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    if (thread == 0) {
        return EINVAL;
    }

    // The handle IS the address of the join word. Block until the kernel
    // clears it to 0 (on thread exit) and futex-wakes us. Pass the CURRENT
    // value as the futex "expected" so we only sleep while it is still set;
    // the kernel returns EAGAIN immediately if it already changed (no lost
    // wakeup, no busy spin).
    volatile unsigned int *tid_addr = (volatile unsigned int *)(uintptr_t)thread;
    unsigned int v;
    while ((v = *tid_addr) != 0) {
        futex_wait(tid_addr, v, 0);  // infinite timeout
    }

    // Return value plumbing is not implemented (pthread_exit does not stash it).
    if (retval) {
        *retval = NULL;
    }

    // The join word was malloc'd in pthread_create; the thread is gone now.
    free((void *)(uintptr_t)thread);
    return 0;
}

int pthread_detach(pthread_t thread) {
    if (thread == 0) {
        return EINVAL;
    }

    // Mark thread as detached via syscall
    // For now, just return success (thread will clean up on exit)
    return 0;
}

void pthread_exit(void *retval) {
    (void)retval;  // TODO: Store for pthread_join

    // Exit via syscall
    syscall1(SYS_EXIT, 0);

    // Should never reach here
    while (1) {
        __asm__ volatile("hlt");
    }
    __builtin_unreachable();
}

pthread_t pthread_self(void) {
    return (pthread_t)syscall0(SYS_GETTID);
}

int pthread_equal(pthread_t t1, pthread_t t2) {
    return t1 == t2;
}

int pthread_yield(void) {
    syscall0(SYS_YIELD);
    return 0;
}

// ============================================================================
// Thread Attributes
// ============================================================================

int pthread_attr_init(pthread_attr_t *attr) {
    if (!attr) return EINVAL;

    attr->flags = 0;
    attr->stack_addr = NULL;
    attr->stack_size = PTHREAD_STACK_DEFAULT;
    attr->detach_state = PTHREAD_CREATE_JOINABLE;
    attr->sched_policy = 0;
    attr->sched_priority = 0;

    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate) {
    if (!attr) return EINVAL;
    if (detachstate != PTHREAD_CREATE_JOINABLE &&
        detachstate != PTHREAD_CREATE_DETACHED) {
        return EINVAL;
    }
    attr->detach_state = detachstate;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate) {
    if (!attr || !detachstate) return EINVAL;
    *detachstate = attr->detach_state;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, unsigned long stacksize) {
    if (!attr) return EINVAL;
    if (stacksize < PTHREAD_STACK_MIN) return EINVAL;
    attr->stack_size = stacksize;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, unsigned long *stacksize) {
    if (!attr || !stacksize) return EINVAL;
    *stacksize = attr->stack_size;
    return 0;
}

int pthread_attr_setstack(pthread_attr_t *attr, void *stackaddr, unsigned long stacksize) {
    if (!attr) return EINVAL;
    if (stacksize < PTHREAD_STACK_MIN) return EINVAL;
    attr->stack_addr = stackaddr;
    attr->stack_size = stacksize;
    return 0;
}

int pthread_attr_getstack(const pthread_attr_t *attr, void **stackaddr, unsigned long *stacksize) {
    if (!attr || !stackaddr || !stacksize) return EINVAL;
    *stackaddr = attr->stack_addr;
    *stacksize = attr->stack_size;
    return 0;
}

// ============================================================================
// Mutex Operations
// ============================================================================

// Mutex states:
// 0 = unlocked
// 1 = locked, no waiters
// 2 = locked, has waiters

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    if (!mutex) return EINVAL;

    mutex->lock = 0;
    mutex->owner = 0;
    mutex->type = attr ? attr->type : PTHREAD_MUTEX_DEFAULT;
    mutex->recursive_count = 0;

    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    if (mutex->lock != 0) return EBUSY;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;

    unsigned int self = pthread_self();

    // Handle recursive mutex
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
        if (mutex->owner == self) {
            mutex->recursive_count++;
            return 0;
        }
    }

    // Drepper 3-state futex mutex (0=free, 1=locked, 2=locked+waiters).
    // #430: use cmpxchg_val (OLD value), not the boolean atomic_cas - the
    // previous code compared the boolean result against 0/2 and never actually
    // acquired an uncontended lock.
    unsigned int c = cmpxchg_val(&mutex->lock, 0, 1);
    if (c != 0) {
        // Contended. Mark as "locked with waiters" and block until free.
        if (c != 2) {
            c = atomic_xchg(&mutex->lock, 2);
        }
        while (c != 0) {
            futex_wait(&mutex->lock, 2, 0);       // sleeps in the kernel
            c = atomic_xchg(&mutex->lock, 2);     // retry, keep waiters flag
        }
    }

    mutex->owner = self;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;

    unsigned int self = pthread_self();

    // Handle recursive mutex
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
        if (mutex->owner == self) {
            mutex->recursive_count++;
            return 0;
        }
    }

    // Try to acquire lock (0 -> 1)
    if (atomic_cas(&mutex->lock, 0, 1)) {
        mutex->owner = self;
        return 0;
    }

    return EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;

    unsigned int self = pthread_self();

    // Check ownership for errorcheck mutex
    if (mutex->type == PTHREAD_MUTEX_ERRORCHECK) {
        if (mutex->owner != self) {
            return EPERM;
        }
    }

    // Handle recursive mutex
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE && mutex->recursive_count > 0) {
        mutex->recursive_count--;
        return 0;
    }

    mutex->owner = 0;

    // Release lock
    if (atomic_fetch_add(&mutex->lock, -1) != 1) {
        // There were waiters - set to 0 and wake one
        mutex->lock = 0;
        futex_wake(&mutex->lock, 1);
    }

    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    if (!attr) return EINVAL;
    attr->type = PTHREAD_MUTEX_DEFAULT;
    attr->pshared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
    if (!attr) return EINVAL;
    if (type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK) {
        return EINVAL;
    }
    attr->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type) {
    if (!attr || !type) return EINVAL;
    *type = attr->type;
    return 0;
}

// ============================================================================
// Condition Variable Operations
// ============================================================================

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    (void)attr;
    if (!cond) return EINVAL;

    cond->seq = 0;
    cond->waiters = 0;
    cond->mutex = NULL;

    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    if (cond->waiters > 0) return EBUSY;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    if (!cond || !mutex) return EINVAL;

    // Save the current sequence number
    unsigned int seq = cond->seq;

    // Remember the mutex for potential broadcast requeue
    cond->mutex = mutex;

    // Increment waiter count
    atomic_fetch_add(&cond->waiters, 1);

    // Unlock the mutex
    pthread_mutex_unlock(mutex);

    // Wait for signal (seq to change)
    futex_wait(&cond->seq, seq, 0);

    // Decrement waiter count
    atomic_fetch_add(&cond->waiters, -1);

    // Reacquire mutex
    pthread_mutex_lock(mutex);

    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime) {
    if (!cond || !mutex || !abstime) return EINVAL;

    // Save the current sequence number
    unsigned int seq = cond->seq;
    cond->mutex = mutex;

    // Increment waiter count
    atomic_fetch_add(&cond->waiters, 1);

    // Unlock the mutex
    pthread_mutex_unlock(mutex);

    // Calculate timeout in milliseconds
    // Note: This is a simplification - real implementation would get current time
    unsigned long timeout_ms = abstime->tv_sec * 1000 + abstime->tv_nsec / 1000000;

    // Wait with timeout
    long ret = futex_wait(&cond->seq, seq, timeout_ms);

    // Decrement waiter count
    atomic_fetch_add(&cond->waiters, -1);

    // Reacquire mutex
    pthread_mutex_lock(mutex);

    // Check for timeout
    if (ret == -ETIMEDOUT) {
        return ETIMEDOUT;
    }

    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond) {
    if (!cond) return EINVAL;

    // Only signal if there are waiters
    if (cond->waiters > 0) {
        // Increment sequence to wake waiters
        atomic_fetch_add(&cond->seq, 1);

        // Wake one waiter
        futex_wake(&cond->seq, 1);
    }

    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    if (!cond) return EINVAL;

    // Only broadcast if there are waiters
    if (cond->waiters > 0) {
        // Increment sequence to wake waiters
        atomic_fetch_add(&cond->seq, 1);

        // Wake all waiters
        futex_wake(&cond->seq, 0x7FFFFFFF);  // Wake "all"
    }

    return 0;
}

int pthread_condattr_init(pthread_condattr_t *attr) {
    if (!attr) return EINVAL;
    attr->pshared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr) {
    (void)attr;
    return 0;
}

// ============================================================================
// Read-Write Lock Operations
// ============================================================================

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr) {
    (void)attr;
    if (!rwlock) return EINVAL;

    rwlock->readers = 0;
    rwlock->writer = 0;
    pthread_mutex_init(&rwlock->mutex, NULL);
    pthread_cond_init(&rwlock->read_cond, NULL);
    pthread_cond_init(&rwlock->write_cond, NULL);

    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rwlock) {
    if (!rwlock) return EINVAL;

    pthread_mutex_destroy(&rwlock->mutex);
    pthread_cond_destroy(&rwlock->read_cond);
    pthread_cond_destroy(&rwlock->write_cond);

    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock) {
    if (!rwlock) return EINVAL;

    pthread_mutex_lock(&rwlock->mutex);

    // Wait while there's an active writer
    while (rwlock->writer != 0) {
        pthread_cond_wait(&rwlock->read_cond, &rwlock->mutex);
    }

    rwlock->readers++;

    pthread_mutex_unlock(&rwlock->mutex);
    return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock) {
    if (!rwlock) return EINVAL;

    if (pthread_mutex_trylock(&rwlock->mutex) != 0) {
        return EBUSY;
    }

    if (rwlock->writer != 0) {
        pthread_mutex_unlock(&rwlock->mutex);
        return EBUSY;
    }

    rwlock->readers++;

    pthread_mutex_unlock(&rwlock->mutex);
    return 0;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock) {
    if (!rwlock) return EINVAL;

    pthread_mutex_lock(&rwlock->mutex);

    // Wait while there are readers or a writer
    while (rwlock->readers > 0 || rwlock->writer != 0) {
        pthread_cond_wait(&rwlock->write_cond, &rwlock->mutex);
    }

    rwlock->writer = pthread_self();

    pthread_mutex_unlock(&rwlock->mutex);
    return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock) {
    if (!rwlock) return EINVAL;

    if (pthread_mutex_trylock(&rwlock->mutex) != 0) {
        return EBUSY;
    }

    if (rwlock->readers > 0 || rwlock->writer != 0) {
        pthread_mutex_unlock(&rwlock->mutex);
        return EBUSY;
    }

    rwlock->writer = pthread_self();

    pthread_mutex_unlock(&rwlock->mutex);
    return 0;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock) {
    if (!rwlock) return EINVAL;

    pthread_mutex_lock(&rwlock->mutex);

    if (rwlock->writer == pthread_self()) {
        // Writer unlock
        rwlock->writer = 0;
        // Wake all readers and one writer
        pthread_cond_broadcast(&rwlock->read_cond);
        pthread_cond_signal(&rwlock->write_cond);
    } else if (rwlock->readers > 0) {
        // Reader unlock
        rwlock->readers--;
        if (rwlock->readers == 0) {
            // Last reader - wake a waiting writer
            pthread_cond_signal(&rwlock->write_cond);
        }
    }

    pthread_mutex_unlock(&rwlock->mutex);
    return 0;
}

// ============================================================================
// Barrier Operations
// ============================================================================

int pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr,
                         unsigned int count) {
    (void)attr;
    if (!barrier || count == 0) return EINVAL;

    pthread_mutex_init(&barrier->mutex, NULL);
    pthread_cond_init(&barrier->cond, NULL);
    barrier->count = count;
    barrier->waiting = 0;
    barrier->phase = 0;

    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier) {
    if (!barrier) return EINVAL;

    pthread_mutex_destroy(&barrier->mutex);
    pthread_cond_destroy(&barrier->cond);

    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *barrier) {
    if (!barrier) return EINVAL;

    pthread_mutex_lock(&barrier->mutex);

    unsigned int phase = barrier->phase;
    barrier->waiting++;

    if (barrier->waiting == barrier->count) {
        // Last thread - release all
        barrier->waiting = 0;
        barrier->phase++;  // Toggle phase
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->mutex);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    } else {
        // Wait for other threads
        while (phase == barrier->phase) {
            pthread_cond_wait(&barrier->cond, &barrier->mutex);
        }
        pthread_mutex_unlock(&barrier->mutex);
        return 0;
    }
}

// ============================================================================
// Once Control
// ============================================================================

int pthread_once(pthread_once_t *once, void (*init_routine)(void)) {
    if (!once || !init_routine) return EINVAL;

    // Fast path - already done
    if (once->done) {
        return 0;
    }

    pthread_mutex_lock(&once->mutex);

    if (!once->done) {
        init_routine();
        memory_barrier();
        once->done = 1;
    }

    pthread_mutex_unlock(&once->mutex);

    return 0;
}

// ============================================================================
// Thread-Specific Data (simplified implementation)
// ============================================================================

// TSD storage (per-thread, accessed via TLS)
static void *tsd_values[PTHREAD_KEYS_MAX];
static void (*tsd_destructors[PTHREAD_KEYS_MAX])(void *);
static unsigned int tsd_keys_used = 0;
static pthread_mutex_t tsd_mutex = PTHREAD_MUTEX_INITIALIZER;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    if (!key) return EINVAL;

    pthread_mutex_lock(&tsd_mutex);

    if (tsd_keys_used >= PTHREAD_KEYS_MAX) {
        pthread_mutex_unlock(&tsd_mutex);
        return EAGAIN;
    }

    *key = tsd_keys_used;
    tsd_destructors[tsd_keys_used] = destructor;
    tsd_values[tsd_keys_used] = NULL;
    tsd_keys_used++;

    pthread_mutex_unlock(&tsd_mutex);
    return 0;
}

int pthread_key_delete(pthread_key_t key) {
    if (key >= PTHREAD_KEYS_MAX) return EINVAL;

    pthread_mutex_lock(&tsd_mutex);
    tsd_destructors[key] = NULL;
    tsd_values[key] = NULL;
    pthread_mutex_unlock(&tsd_mutex);

    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    if (key >= PTHREAD_KEYS_MAX) return NULL;
    return tsd_values[key];
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    if (key >= PTHREAD_KEYS_MAX) return EINVAL;
    tsd_values[key] = (void *)value;
    return 0;
}

// ============================================================================
// Spinlock Operations
// ============================================================================

int pthread_spin_init(pthread_spinlock_t *lock, int pshared) {
    (void)pshared;
    if (!lock) return EINVAL;
    *lock = 0;
    return 0;
}

int pthread_spin_destroy(pthread_spinlock_t *lock) {
    if (!lock) return EINVAL;
    return 0;
}

int pthread_spin_lock(pthread_spinlock_t *lock) {
    if (!lock) return EINVAL;

    while (!atomic_cas(lock, 0, 1)) {
        cpu_pause();
    }

    return 0;
}

int pthread_spin_trylock(pthread_spinlock_t *lock) {
    if (!lock) return EINVAL;

    if (atomic_cas(lock, 0, 1)) {
        return 0;
    }
    return EBUSY;
}

int pthread_spin_unlock(pthread_spinlock_t *lock) {
    if (!lock) return EINVAL;
    *lock = 0;
    memory_barrier();
    return 0;
}

// ============================================================================
// Cancellation (stubs)
// ============================================================================

int pthread_setcancelstate(int state, int *oldstate) {
    (void)state;
    if (oldstate) *oldstate = PTHREAD_CANCEL_DISABLE;
    return 0;
}

int pthread_setcanceltype(int type, int *oldtype) {
    (void)type;
    if (oldtype) *oldtype = PTHREAD_CANCEL_DEFERRED;
    return 0;
}

void pthread_testcancel(void) {
    // No-op - cancellation not implemented
}
