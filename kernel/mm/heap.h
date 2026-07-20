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

// Free allocated memory
void kfree(void *ptr);

// Get heap statistics
size_t heap_get_total_size(void);
size_t heap_get_used_size(void);
size_t heap_get_free_size(void);

// Debug: print heap statistics
void heap_print_stats(void);

#endif // HEAP_H
