/* assert.h - freestanding shim for MayteraOS NetSurf port.
 * On failure prints and calls abort(). Honors NDEBUG. C only. */
#ifndef MAYTERA_SHIM_ASSERT_H
#define MAYTERA_SHIM_ASSERT_H

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#include <stdio.h>
#include <stdlib.h>
#define assert(expr) \
    ((expr) ? (void)0 : \
     (fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__), abort()))
#endif

#endif
