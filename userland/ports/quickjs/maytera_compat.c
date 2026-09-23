/* ports/quickjs/maytera_compat.c - MayteraOS libc compatibility shim for the
 * QuickJS-ng engine (Tier 2 item #8).
 *
 * This file supplies the ONE symbol the four engine translation units reference
 * that the MayteraOS freestanding libc does not define: pthread_condattr_setclock.
 * cutils.h's js_cond_init() calls it to run its condition variables off
 * CLOCK_MONOTONIC; that path is reached only by Atomics.wait(). It is defined
 * WEAK so that:
 *   - if a future libc grows a real pthread_condattr_setclock, that strong
 *     definition wins at link time and this becomes inert, and
 *   - no other app that links libc is perturbed by this port.
 *
 * The MayteraOS libc's pthread_cond_timedwait uses a fixed clock, so recording
 * a clock choice on the attr object is a no-op here; returning 0 (success) is
 * the correct, harmless behaviour.
 *
 * This is a one-symbol compat shim scoped to libquickjs.a, NOT a private fork of
 * a shared primitive: it does not reimplement any pthread routine, it fills a
 * single gap the engine happens to reach. Kept out of the shared libc on purpose
 * (see the port's build.sh header and docs/MPORTS.md owner rule 1).
 */
#include <pthread.h>

#pragma weak pthread_condattr_setclock
int pthread_condattr_setclock(pthread_condattr_t *attr, int clock_id)
{
    (void)attr;
    (void)clock_id;
    return 0;
}
