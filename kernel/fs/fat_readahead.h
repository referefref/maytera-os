// fat_readahead.h - FAT Read-Ahead Buffering for MayteraOS
// Implements sequential access detection, prefetching, and LRU cache
#ifndef FAT_READAHEAD_H
#define FAT_READAHEAD_H

#include "../types.h"
#include "fat.h"

// Read-ahead configuration
#define RA_DEFAULT_BUFFER_SIZE      (256 * 1024)    // 256KB default buffer
#define RA_MIN_BUFFER_SIZE          (64 * 1024)     // 64KB minimum
#define RA_MAX_BUFFER_SIZE          (1024 * 1024)   // 1MB maximum
#define RA_DEFAULT_PREFETCH_CLUSTERS 8              // Prefetch 8 clusters ahead
#define RA_MAX_PREFETCH_CLUSTERS    32              // Maximum prefetch depth
#define RA_SEQUENTIAL_THRESHOLD     3               // Consecutive accesses to trigger prefetch

// Cache entry states
#define CACHE_EMPTY         0
#define CACHE_LOADING       1
#define CACHE_VALID         2
#define CACHE_DIRTY         3

// Forward declaration
typedef struct cache_entry cache_entry_t;

// Cache entry structure
struct cache_entry {
    fat_fs_t *fs;               // Filesystem this entry belongs to
    uint32_t cluster;           // Cluster number cached
    uint32_t state;             // Cache state (CACHE_*)
    uint64_t access_time;       // Last access timestamp (for LRU)
    uint32_t access_count;      // Number of accesses
    uint8_t *data;              // Pointer to cached data
    uint32_t size;              // Size of cached data
    int dirty;                  // Modified (needs writeback)
    cache_entry_t *lru_next;
    cache_entry_t *lru_prev;
};
// Read-ahead context for a file
typedef struct {
    fat_file_t *file;           // Associated file handle
    uint32_t last_cluster;      // Last accessed cluster
    uint32_t last_position;     // Last read position
    uint32_t sequential_count;  // Count of sequential accesses
    int prefetch_enabled;       // Whether prefetch is active
    uint32_t prefetch_depth;    // How many clusters to prefetch
    uint32_t prefetch_pending;  // Clusters being prefetched
    uint64_t bytes_prefetched;  // Total bytes prefetched
    uint64_t cache_hits;        // Number of cache hits
    uint64_t cache_misses;      // Number of cache misses
} readahead_context_t;

// Buffer cache manager
typedef struct {
    // Configuration
    uint32_t buffer_size;           // Total buffer size
    uint32_t cluster_size;          // Size of one cluster
    uint32_t max_entries;           // Maximum cache entries

    // Cache entries
    cache_entry_t *entries;         // Array of cache entries
    uint32_t entry_count;           // Current number of entries

    // LRU list (doubly linked)
    cache_entry_t *lru_head;        // Most recently used
    cache_entry_t *lru_tail;        // Least recently used

    // Hash table for fast lookup (cluster -> entry)
    #define CACHE_HASH_SIZE     256
    cache_entry_t *hash_table[CACHE_HASH_SIZE];

    // Statistics
    uint64_t total_reads;           // Total read requests
    uint64_t cache_hits;            // Requests satisfied from cache
    uint64_t cache_misses;          // Requests that required disk I/O
    uint64_t evictions;             // Cache evictions
    uint64_t prefetch_requests;     // Prefetch operations started
    uint64_t prefetch_hits;         // Prefetched data that was used
    uint64_t prefetch_wastes;       // Prefetched data that was evicted unused

    // Memory pool
    uint8_t *buffer_pool;           // Memory for cached data
    uint32_t pool_used;             // Bytes used in pool

    // Global timestamp for LRU
    uint64_t timestamp;
} buffer_cache_t;

// DMA prefetch request
typedef struct {
    fat_fs_t *fs;
    uint32_t cluster;
    uint32_t count;
    void *buffer;
    int complete;
    int error;
} prefetch_request_t;

// ============================================
// Initialization and Configuration
// ============================================

// Initialize read-ahead subsystem with specified buffer size
// buffer_size: Total bytes for cache (0 for default)
// Returns 0 on success, -1 on failure
int fat_readahead_init(uint32_t buffer_size);

// Shutdown read-ahead subsystem
void fat_readahead_shutdown(void);

// Configure prefetch depth (clusters to read ahead)
void fat_readahead_set_prefetch(uint32_t clusters);

// Enable/disable read-ahead globally
void fat_readahead_enable(int enable);

// ============================================
// File Operations with Read-Ahead
// ============================================

// Create read-ahead context for a file
readahead_context_t *fat_readahead_open(fat_file_t *file);

// Close read-ahead context
void fat_readahead_close(readahead_context_t *ctx);

// Read with read-ahead support
// Returns bytes read, -1 on error
int fat_readahead_read(readahead_context_t *ctx, void *buffer, uint32_t size);

// Seek in file (updates read-ahead context)
int fat_readahead_seek(readahead_context_t *ctx, uint32_t position);

// Hint sequential access (triggers aggressive prefetch)
void fat_readahead_hint_sequential(readahead_context_t *ctx);

// Hint random access (disables prefetch)
void fat_readahead_hint_random(readahead_context_t *ctx);

// ============================================
// Cache Management
// ============================================

// Get cached cluster data (NULL if not cached)
void *fat_cache_get(fat_fs_t *fs, uint32_t cluster);

// Put cluster data in cache
int fat_cache_put(fat_fs_t *fs, uint32_t cluster, void *data, uint32_t size);

// Invalidate cached cluster
void fat_cache_invalidate(fat_fs_t *fs, uint32_t cluster);

// Invalidate all entries for a filesystem
void fat_cache_invalidate_fs(fat_fs_t *fs);

// Flush dirty cache entries to disk
int fat_cache_flush(fat_fs_t *fs);

// Evict least recently used entry
void fat_cache_evict_lru(void);

// ============================================
// Async Prefetch (DMA)
// ============================================

// Start asynchronous prefetch of clusters
// Returns request handle, NULL on failure
prefetch_request_t *fat_prefetch_start(fat_fs_t *fs, uint32_t start_cluster,
                                       uint32_t count);

// Check if prefetch is complete
int fat_prefetch_complete(prefetch_request_t *req);

// Wait for prefetch to complete
int fat_prefetch_wait(prefetch_request_t *req);

// Cancel prefetch request
void fat_prefetch_cancel(prefetch_request_t *req);

// Process completed prefetch (move to cache)
int fat_prefetch_process(prefetch_request_t *req);

// ============================================
// Statistics and Debugging
// ============================================

// Get cache statistics
void fat_cache_get_stats(uint64_t *hits, uint64_t *misses, uint64_t *evictions);

// Print cache statistics
void fat_cache_print_stats(void);

// Print read-ahead context info
void fat_readahead_print_context(readahead_context_t *ctx);

// Dump cache contents (for debugging)
void fat_cache_dump(void);

#endif // FAT_READAHEAD_H
