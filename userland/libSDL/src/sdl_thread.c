/* sdl_thread.c - SDL_Thread/SDL_mutex/SDL_sem/SDL_cond over the REAL
 * userland/libc/pthread.h primitives. Part of the MayteraOS SDL 1.2 backend
 * (task #745, docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md Tier 2 #7).
 *
 * This is a genuine implementation, not a stub: the two pre-existing
 * sdlshim.cpp files had to bounce every thread/lock call through a private
 * C-linkage bridge (mos_pthread_bridge.c) because they were C++ translation
 * units avoiding a direct libc include. libSDL is plain C, so it links
 * userland/libc/pthread.h directly and there is no bridge to reinvent.
 *
 * SDL_sem does not exist as a MayteraOS primitive (no sem_t in libc), so it
 * is built the standard way, from the mutex+cond that DO exist (a counting
 * semaphore is a textbook mutex+cond+counter), rather than adding a new
 * kernel-level semaphore just for this.
 */
#include "sdl_priv.h"
#include "pthread.h"
#include "time.h"

struct SDL_Thread {
    pthread_t tid;
    int (SDLCALL *fn)(void *);
    void *data;
    int retval;
};
static void *thread_trampoline(void *arg) {
    SDL_Thread *t = (SDL_Thread *)arg;
    t->retval = t->fn(t->data);
    return 0;
}
SDL_Thread *SDL_CreateThread(int (SDLCALL *fn)(void *), void *data) {
    SDL_Thread *t = (SDL_Thread *)SDL_malloc(sizeof(SDL_Thread));
    if (!t) { sdlpriv_set_error("Out of memory"); return 0; }
    t->fn = fn; t->data = data; t->retval = 0;
    if (pthread_create(&t->tid, 0, thread_trampoline, t) != 0) {
        sdlpriv_set_error("pthread_create failed");
        SDL_free(t);
        return 0;
    }
    return t;
}
Uint32 SDL_ThreadID(void) { return (Uint32)(unsigned long)pthread_self(); }
Uint32 SDL_GetThreadID(SDL_Thread *thread) { return thread ? (Uint32)(unsigned long)thread->tid : SDL_ThreadID(); }
void SDL_WaitThread(SDL_Thread *thread, int *status) {
    if (!thread) return;
    pthread_join(thread->tid, 0);
    if (status) *status = thread->retval;
    SDL_free(thread);
}
void SDL_KillThread(SDL_Thread *thread) {
    /* No thread-cancellation primitive in userland/libc/pthread.h; detaching
     * is the closest honest behaviour (the thread runs to completion but we
     * stop tracking it), rather than pretending to force-kill it. */
    if (!thread) return;
    pthread_detach(thread->tid);
    SDL_free(thread);
}

/* ---------------------------------------------------------------------
 * Mutex
 * ------------------------------------------------------------------- */
struct SDL_mutex { pthread_mutex_t m; };
SDL_mutex *SDL_CreateMutex(void) {
    SDL_mutex *m = (SDL_mutex *)SDL_malloc(sizeof(SDL_mutex));
    if (!m) return 0;
    pthread_mutex_init(&m->m, 0);
    return m;
}
int SDL_mutexP(SDL_mutex *mutex) { return mutex ? pthread_mutex_lock(&mutex->m) : -1; }
int SDL_mutexV(SDL_mutex *mutex) { return mutex ? pthread_mutex_unlock(&mutex->m) : -1; }
void SDL_DestroyMutex(SDL_mutex *mutex) { if (!mutex) return; pthread_mutex_destroy(&mutex->m); SDL_free(mutex); }

/* ---------------------------------------------------------------------
 * Semaphore, built on mutex+cond+counter.
 * ------------------------------------------------------------------- */
struct SDL_semaphore { pthread_mutex_t m; pthread_cond_t c; unsigned int value; };
SDL_sem *SDL_CreateSemaphore(Uint32 initial_value) {
    SDL_sem *s = (SDL_sem *)SDL_malloc(sizeof(SDL_sem));
    if (!s) return 0;
    pthread_mutex_init(&s->m, 0);
    pthread_cond_init(&s->c, 0);
    s->value = initial_value;
    return s;
}
void SDL_DestroySemaphore(SDL_sem *sem) {
    if (!sem) return;
    pthread_cond_destroy(&sem->c); pthread_mutex_destroy(&sem->m);
    SDL_free(sem);
}
int SDL_SemPost(SDL_sem *sem) {
    if (!sem) return -1;
    pthread_mutex_lock(&sem->m);
    sem->value++;
    pthread_cond_signal(&sem->c);
    pthread_mutex_unlock(&sem->m);
    return 0;
}
int SDL_SemWait(SDL_sem *sem) {
    if (!sem) return -1;
    pthread_mutex_lock(&sem->m);
    while (sem->value == 0) pthread_cond_wait(&sem->c, &sem->m);
    sem->value--;
    pthread_mutex_unlock(&sem->m);
    return 0;
}
int SDL_SemTryWait(SDL_sem *sem) {
    if (!sem) return -1;
    pthread_mutex_lock(&sem->m);
    int rc = SDL_MUTEX_TIMEDOUT;
    if (sem->value > 0) { sem->value--; rc = 0; }
    pthread_mutex_unlock(&sem->m);
    return rc;
}
int SDL_SemWaitTimeout(SDL_sem *sem, Uint32 ms) {
    if (!sem) return -1;
    if (ms == SDL_MUTEX_MAXWAIT) return SDL_SemWait(sem);
    struct timespec abst;
    clock_gettime(CLOCK_REALTIME, &abst);
    abst.tv_sec += (long)(ms / 1000);
    abst.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (abst.tv_nsec >= 1000000000L) { abst.tv_nsec -= 1000000000L; abst.tv_sec += 1; }
    pthread_mutex_lock(&sem->m);
    int rc = 0;
    while (sem->value == 0) {
        if (pthread_cond_timedwait(&sem->c, &sem->m, &abst) != 0) { rc = SDL_MUTEX_TIMEDOUT; break; }
    }
    if (rc == 0) sem->value--;
    pthread_mutex_unlock(&sem->m);
    return rc;
}
Uint32 SDL_SemValue(SDL_sem *sem) { return sem ? sem->value : 0; }

/* ---------------------------------------------------------------------
 * Condition variable
 * ------------------------------------------------------------------- */
struct SDL_cond { pthread_cond_t c; };
SDL_cond *SDL_CreateCond(void) {
    SDL_cond *c = (SDL_cond *)SDL_malloc(sizeof(SDL_cond));
    if (!c) return 0;
    pthread_cond_init(&c->c, 0);
    return c;
}
void SDL_DestroyCond(SDL_cond *cond) { if (!cond) return; pthread_cond_destroy(&cond->c); SDL_free(cond); }
int SDL_CondSignal(SDL_cond *cond) { return cond ? pthread_cond_signal(&cond->c) : -1; }
int SDL_CondBroadcast(SDL_cond *cond) { return cond ? pthread_cond_broadcast(&cond->c) : -1; }
int SDL_CondWait(SDL_cond *cond, SDL_mutex *mut) { return (cond && mut) ? pthread_cond_wait(&cond->c, &mut->m) : -1; }
int SDL_CondWaitTimeout(SDL_cond *cond, SDL_mutex *mutex, Uint32 ms) {
    if (!cond || !mutex) return -1;
    if (ms == SDL_MUTEX_MAXWAIT) return SDL_CondWait(cond, mutex);
    struct timespec abst;
    clock_gettime(CLOCK_REALTIME, &abst);
    abst.tv_sec += (long)(ms / 1000);
    abst.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (abst.tv_nsec >= 1000000000L) { abst.tv_nsec -= 1000000000L; abst.tv_sec += 1; }
    int rc = pthread_cond_timedwait(&cond->c, &mutex->m, &abst);
    return rc == 0 ? 0 : SDL_MUTEX_TIMEDOUT;
}
