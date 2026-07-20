// arc_port.h - portability shims for the MayteraOS archiver core.
// The core (arc.c) uses no syscalls; it depends only on an allocator and the
// freestanding string functions, both of which exist in the kernel and the
// userland libc. Define ARC_KERNEL when building inside the kernel tree.
#ifndef ARC_PORT_H
#define ARC_PORT_H

#if defined(ARC_KERNEL)
// Kernel: use the kernel's own fixed-width types (do NOT pull in gcc <stddef.h>,
// whose size_t differs from the kernel's and would clash).
#include "../types.h"
#include "../mm/heap.h"
#include "../string.h"
#define ARC_MALLOC(n)      kmalloc(n)
#define ARC_REALLOC(p, n)  krealloc((p), (n))
#define ARC_FREE(p)        kfree(p)
#elif defined(ARC_HOST)
// Plain hosted build used for the gcc unit/interop harness on the build CT.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define ARC_MALLOC(n)      malloc(n)
#define ARC_REALLOC(p, n)  realloc((p), (n))
#define ARC_FREE(p)        free(p)
#else
// MayteraOS userland (freestanding libc).
#include "types.h"
#include "stdlib.h"
#include "string.h"
#define ARC_MALLOC(n)      malloc(n)
#define ARC_REALLOC(p, n)  realloc((p), (n))
#define ARC_FREE(p)        free(p)
#endif

#endif // ARC_PORT_H
