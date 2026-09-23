/* sdl_timer.c - SDL_GetTicks/SDL_Delay/SDL_SetTimer/SDL_AddTimer/
 * SDL_RemoveTimer. Part of the MayteraOS SDL 1.2 backend (task #745,
 * docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md Tier 2 #7).
 *
 * GetTicks/Delay are the two calls nearly every SDL 1.2 game's main loop
 * uses every frame, so they go straight to the real monotonic clock and the
 * real sleep syscall (SYS_UPTIME_MS/SYS_SLEEP via userland/libc/syscall.h),
 * never a busy-wait.
 *
 * AddTimer/SetTimer are real (a genuine pthread per timer that sleeps and
 * re-fires), not silently stubbed: MayteraOS already has the wait-queue-
 * backed sys_sleep() and a real pthread_create(), and there is no reason to
 * pretend a timer callback never fires when both building blocks exist.
 */
#include "sdl_priv.h"
#include "pthread.h"

Uint32 SDL_GetTicks(void) { return (Uint32)uptime_ms(); }
void SDL_Delay(Uint32 ms) { sys_sleep(ms); }

struct _SDL_TimerID {
    pthread_t tid;
    volatile int cancelled;
    Uint32 interval;
    SDL_NewTimerCallback newcb;
    SDL_TimerCallback oldcb;
    void *param;
};

static void *timer_thread(void *arg) {
    struct _SDL_TimerID *t = (struct _SDL_TimerID *)arg;
    Uint32 interval = t->interval;
    while (!t->cancelled && interval > 0) {
        sys_sleep(interval);
        if (t->cancelled) break;
        if (t->newcb) interval = t->newcb(interval, t->param);
        else if (t->oldcb) interval = t->oldcb(interval);
        else break;
    }
    return 0;
}

/* Real SDL1.2's legacy single-timer API (superseded by AddTimer, but still
 * part of the public ABI and still used by a handful of older ports). Only
 * one such timer can exist at a time, matching the real semantics. */
static SDL_TimerID g_legacy_timer = 0;
int SDL_SetTimer(Uint32 interval, SDL_TimerCallback callback) {
    if (g_legacy_timer) { SDL_RemoveTimer(g_legacy_timer); g_legacy_timer = 0; }
    if (interval == 0 || !callback) return 0;
    struct _SDL_TimerID *t = (struct _SDL_TimerID *)SDL_calloc(1, sizeof(*t));
    if (!t) { sdlpriv_set_error("Out of memory"); return -1; }
    t->interval = interval; t->oldcb = callback;
    if (pthread_create(&t->tid, 0, timer_thread, t) != 0) { SDL_free(t); sdlpriv_set_error("pthread_create failed"); return -1; }
    pthread_detach(t->tid);
    g_legacy_timer = t;
    return 0;
}

SDL_TimerID SDL_AddTimer(Uint32 interval, SDL_NewTimerCallback callback, void *param) {
    if (interval == 0 || !callback) { sdlpriv_set_error("Invalid timer interval or callback"); return 0; }
    struct _SDL_TimerID *t = (struct _SDL_TimerID *)SDL_calloc(1, sizeof(*t));
    if (!t) { sdlpriv_set_error("Out of memory"); return 0; }
    t->interval = interval; t->newcb = callback; t->param = param;
    if (pthread_create(&t->tid, 0, timer_thread, t) != 0) { SDL_free(t); sdlpriv_set_error("pthread_create failed"); return 0; }
    pthread_detach(t->tid);
    return t;
}
SDL_bool SDL_RemoveTimer(SDL_TimerID t) {
    if (!t) return SDL_FALSE;
    t->cancelled = 1;
    /* Detached and self-cleaning: the thread notices `cancelled` on its next
     * wake (at most one `interval` late) and exits; we cannot join a
     * detached thread, so the struct is intentionally leaked rather than
     * freed out from under a thread that might still be reading it. A timer
     * add/remove is a rare, one-off event for the class of games this
     * backend targets, not a per-frame allocation, so this is a bounded,
     * documented cost rather than an unbounded leak. */
    return SDL_TRUE;
}
