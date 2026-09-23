// heap.h - Kernel Heap Allocator
#ifndef HEAP_H
#define HEAP_H

#include "../types.h"

// Initialize the kernel heap
void heap_init(void);

// Allocate memory from the heap
void *kmalloc(size_t size);

// Allocate aligned memory
void *kmalloc_aligned(size_t size, size_t alignment);

// Allocate zeroed memory
void *kzalloc(size_t size);

// Allocate zeroed aligned memory
void *kzalloc_aligned(size_t size, size_t alignment);

// Reallocate memory
void *krealloc(void *ptr, size_t new_size);

// Free allocated memory.
//
// #dosmem: this ALSO frees a pointer from kmalloc_aligned()/kzalloc_aligned()
// correctly. Until 2026-09-03 it did not, and there was no kfree_aligned()
// either, so all twenty aligned call sites in the tree leaked their whole
// block (MEASURED on a healthy boot: drivers/hda.c:1015 and :1019). The fix is
// in the allocator, not in the callers, so no call site has to know.
void kfree(void *ptr);

// Explicit spelling for a kmalloc_aligned()/kzalloc_aligned() result. A direct
// alias of kfree(), which already handles it; it exists so new code can say
// what it means, NOT as a second implementation.
void kfree_aligned(void *ptr);

// Get heap statistics
size_t heap_get_total_size(void);
size_t heap_get_used_size(void);
size_t heap_get_free_size(void);

// Debug: print heap statistics
void heap_print_stats(void);

#endif // HEAP_H
