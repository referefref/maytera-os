// fat_readahead.c - FAT Read-Ahead Buffering Implementation
// Sequential access detection, prefetching, and LRU cache with DMA support

#include "fat_readahead.h"
#include "fat.h"
#include "../drivers/ata.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../serial.h"
#include "../string.h"

// ============================================
// Global State
// ============================================

static buffer_cache_t *g_cache = NULL;
static int g_readahead_enabled = 1;
static uint32_t g_prefetch_depth = RA_DEFAULT_PREFETCH_CLUSTERS;

// Spinlock for cache access
static volatile int cache_lock = 0;

static void cache_acquire_lock(void) {
    while (__sync_lock_test_and_set(&cache_lock, 1)) {
        __asm__ volatile("pause");
    }
}

static void cache_release_lock(void) {
    __sync_lock_release(&cache_lock);
}

// Get current timestamp (simple counter for LRU)
static uint64_t get_timestamp(void) {
    if (g_cache) {
        return ++g_cache->timestamp;
    }
    return 0;
}

// Hash function for cluster lookup
static inline uint32_t cluster_hash(uint32_t cluster) {
    return cluster % CACHE_HASH_SIZE;
}

// ============================================
// Helper Functions
// ============================================

// Helper to extract channel and drive from drive ID
static inline uint8_t drive_to_channel(int drive) {
    return (drive >> 1) & 1;
}

static inline uint8_t drive_to_unit(int drive) {
    return drive & 1;
}

// Read cluster using DMA
static int read_cluster_dma(fat_fs_t *fs, uint32_t cluster, void *buffer) {
    if (!fs || cluster < 2) return -1;

    // Calculate LBA for cluster
    uint32_t data_start = fs->data_start_lba;
    uint32_t sectors_per_cluster = fs->sectors_per_cluster;
    uint32_t cluster_lba = data_start + (cluster - 2) * sectors_per_cluster;
    uint32_t abs_lba = fs->part_start_lba + cluster_lba;

    uint8_t channel = drive_to_channel(fs->drive);
    uint8_t unit = drive_to_unit(fs->drive);

    return ata_read_sectors_dma(channel, unit, abs_lba, sectors_per_cluster, buffer);
}

// Read multiple clusters using DMA
static int read_clusters_dma(fat_fs_t *fs, uint32_t start_cluster, uint32_t count,
                            void *buffer) {
    if (!fs || start_cluster < 2 || count == 0) return -1;

    uint8_t *buf = (uint8_t *)buffer;
    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
    int total_read = 0;

    // For now, read one cluster at a time
    // TODO: Optimize for contiguous clusters
    for (uint32_t i = 0; i < count; i++) {
        if (read_cluster_dma(fs, start_cluster + i, buf + i * cluster_size) <= 0) {
            break;
        }
        total_read++;
    }

    return total_read;
}

// ============================================
// Cache Entry Management
// ============================================

// Find cache entry by cluster
static cache_entry_t *cache_find(fat_fs_t *fs, uint32_t cluster) {
    uint32_t hash = cluster_hash(cluster);
    cache_entry_t *entry = g_cache->hash_table[hash];

    while (entry) {
        if (entry->fs == fs && entry->cluster == cluster && entry->state == CACHE_VALID) {
            return entry;
        }
        // Simple linear probing in hash bucket
        entry = NULL;  // Single entry per hash for simplicity
    }

    // Search all entries (fallback)
    for (uint32_t i = 0; i < g_cache->entry_count; i++) {
        entry = &g_cache->entries[i];
        if (entry->fs == fs && entry->cluster == cluster && entry->state == CACHE_VALID) {
            return entry;
        }
    }

    return NULL;
}

// Find free cache entry (or evict LRU)
static cache_entry_t *cache_alloc_entry(void) {
    // Look for empty entry
    for (uint32_t i = 0; i < g_cache->max_entries; i++) {
        if (g_cache->entries[i].state == CACHE_EMPTY) {
            return &g_cache->entries[i];
        }
    }

    // Evict LRU entry
    if (g_cache->lru_tail) {
        cache_entry_t *victim = g_cache->lru_tail;

        // Write back if dirty
        if (victim->dirty) {
            // TODO: Implement writeback
        }

        // Remove from LRU list
        if (victim->lru_prev) {
            victim->lru_prev->lru_next = NULL;
        }
        g_cache->lru_tail = victim->lru_prev;
        if (g_cache->lru_head == victim) {
            g_cache->lru_head = NULL;
        }

        // Remove from hash table
        uint32_t hash = cluster_hash(victim->cluster);
        if (g_cache->hash_table[hash] == victim) {
            g_cache->hash_table[hash] = NULL;
        }

        // Track if prefetched data was wasted
        if (victim->access_count == 0) {
            g_cache->prefetch_wastes++;
        }

        g_cache->evictions++;
        victim->state = CACHE_EMPTY;
        victim->fs = NULL;
        victim->cluster = 0;
        victim->lru_next = NULL;
        victim->lru_prev = NULL;

        return victim;
    }

    return NULL;
}

// Move entry to front of LRU list
static void cache_touch(cache_entry_t *entry) {
    if (!entry) return;

    entry->access_time = get_timestamp();
    entry->access_count++;

    // Already at head?
    if (g_cache->lru_head == entry) return;

    // Remove from current position
    if (entry->lru_prev) {
        entry->lru_prev->lru_next = entry->lru_next;
    }
    if (entry->lru_next) {
        entry->lru_next->lru_prev = entry->lru_prev;
    }
    if (g_cache->lru_tail == entry) {
        g_cache->lru_tail = entry->lru_prev;
    }

    // Insert at head
    entry->lru_prev = NULL;
    entry->lru_next = g_cache->lru_head;
    if (g_cache->lru_head) {
        g_cache->lru_head->lru_prev = entry;
    }
    g_cache->lru_head = entry;

    if (!g_cache->lru_tail) {
        g_cache->lru_tail = entry;
    }
}

// Add entry to cache
static cache_entry_t *cache_add(fat_fs_t *fs, uint32_t cluster, void *data, uint32_t size) {
    cache_entry_t *entry = cache_alloc_entry();
    if (!entry) return NULL;

    entry->fs = fs;
    entry->cluster = cluster;
    entry->state = CACHE_VALID;
    entry->access_time = get_timestamp();
    entry->access_count = 0;
    entry->dirty = 0;
    entry->size = size;

    // Copy data
    entry->data = (uint8_t *)g_cache->buffer_pool + g_cache->pool_used;
    if (g_cache->pool_used + size > g_cache->buffer_size) {
        // Pool full - need to recycle
        g_cache->pool_used = 0;
        entry->data = (uint8_t *)g_cache->buffer_pool;
    }
    memcpy(entry->data, data, size);
    g_cache->pool_used += size;

    // Add to hash table
    uint32_t hash = cluster_hash(cluster);
    g_cache->hash_table[hash] = entry;

    // Add to LRU
    entry->lru_prev = NULL;
    entry->lru_next = g_cache->lru_head;
    if (g_cache->lru_head) {
        g_cache->lru_head->lru_prev = entry;
    }
    g_cache->lru_head = entry;
    if (!g_cache->lru_tail) {
        g_cache->lru_tail = entry;
    }

    g_cache->entry_count++;
    return entry;
}

// ============================================
// Initialization and Configuration
// ============================================

int fat_readahead_init(uint32_t buffer_size) {
    if (g_cache) {
        kprintf("[RA] Already initialized\n");
        return 0;
    }

    // Clamp buffer size
    if (buffer_size == 0) {
        buffer_size = RA_DEFAULT_BUFFER_SIZE;
    } else if (buffer_size < RA_MIN_BUFFER_SIZE) {
        buffer_size = RA_MIN_BUFFER_SIZE;
    } else if (buffer_size > RA_MAX_BUFFER_SIZE) {
        buffer_size = RA_MAX_BUFFER_SIZE;
    }

    kprintf("[RA] Initializing read-ahead with %u KB buffer\n", buffer_size / 1024);

    // Allocate cache structure
    g_cache = kmalloc(sizeof(buffer_cache_t));
    if (!g_cache) {
        kprintf("[RA] Failed to allocate cache structure\n");
        return -1;
    }
    memset(g_cache, 0, sizeof(buffer_cache_t));

    // Allocate buffer pool using PMM for DMA compatibility
    uint32_t pages = (buffer_size + 4095) / 4096;
    uint64_t pool_phys = pmm_alloc_pages(pages);
    if (pool_phys == 0) {
        kprintf("[RA] Failed to allocate buffer pool (%u pages)\n", pages);
        kfree(g_cache);
        g_cache = NULL;
        return -1;
    }
    g_cache->buffer_pool = (uint8_t *)pool_phys;  // Identity mapped
    g_cache->buffer_size = buffer_size;
    g_cache->pool_used = 0;

    // Calculate max entries based on typical cluster size (4KB)
    g_cache->cluster_size = 4096;  // Will be updated per-filesystem
    g_cache->max_entries = buffer_size / g_cache->cluster_size;
    if (g_cache->max_entries > 256) {
        g_cache->max_entries = 256;  // Limit for memory efficiency
    }

    // Allocate cache entries
    g_cache->entries = kmalloc(g_cache->max_entries * sizeof(cache_entry_t));
    if (!g_cache->entries) {
        pmm_free_pages(pool_phys, pages);
        kfree(g_cache);
        g_cache = NULL;
        return -1;
    }
    memset(g_cache->entries, 0, g_cache->max_entries * sizeof(cache_entry_t));

    // Clear hash table
    memset(g_cache->hash_table, 0, sizeof(g_cache->hash_table));

    g_cache->lru_head = NULL;
    g_cache->lru_tail = NULL;
    g_cache->entry_count = 0;
    g_cache->timestamp = 0;

    kprintf("[RA] Initialized: %u KB buffer, %u max entries\n",
            buffer_size / 1024, g_cache->max_entries);

    return 0;
}

void fat_readahead_shutdown(void) {
    if (!g_cache) return;

    kprintf("[RA] Shutting down, flushing cache...\n");

    // Flush dirty entries
    fat_cache_flush(NULL);

    // Free buffer pool
    if (g_cache->buffer_pool) {
        uint32_t pages = (g_cache->buffer_size + 4095) / 4096;
        pmm_free_pages((uint64_t)g_cache->buffer_pool, pages);
    }

    // Free entries
    if (g_cache->entries) {
        kfree(g_cache->entries);
    }

    kfree(g_cache);
    g_cache = NULL;

    kprintf("[RA] Shutdown complete\n");
}

void fat_readahead_set_prefetch(uint32_t clusters) {
    if (clusters > RA_MAX_PREFETCH_CLUSTERS) {
        clusters = RA_MAX_PREFETCH_CLUSTERS;
    }
    g_prefetch_depth = clusters;
}

void fat_readahead_enable(int enable) {
    g_readahead_enabled = enable;
    kprintf("[RA] Read-ahead %s\n", enable ? "enabled" : "disabled");
}

// ============================================
// Read-Ahead Context Management
// ============================================

readahead_context_t *fat_readahead_open(fat_file_t *file) {
    if (!file || !file->open) return NULL;

    readahead_context_t *ctx = kmalloc(sizeof(readahead_context_t));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(readahead_context_t));
    ctx->file = file;
    ctx->last_cluster = file->first_cluster;
    ctx->last_position = 0;
    ctx->sequential_count = 0;
    ctx->prefetch_enabled = g_readahead_enabled;
    ctx->prefetch_depth = g_prefetch_depth;

    return ctx;
}

void fat_readahead_close(readahead_context_t *ctx) {
    if (!ctx) return;

    // Log statistics
    if (ctx->bytes_prefetched > 0 || ctx->cache_hits > 0) {
        kprintf("[RA] File stats: prefetched %lu KB, hits %lu, misses %lu\n",
                ctx->bytes_prefetched / 1024, ctx->cache_hits, ctx->cache_misses);
    }

    kfree(ctx);
}

// ============================================
// Read with Read-Ahead
// ============================================

// Get next cluster in chain (with FAT caching)
static uint32_t get_next_cluster(fat_fs_t *fs __attribute__((unused)), uint32_t cluster) {
    // This should use the FAT table - simplified for now
    // In a complete implementation, we'd cache the FAT itself
    return cluster + 1;  // Assume contiguous (wrong, but for demo)
}

// Check if access is sequential
static int is_sequential_access(readahead_context_t *ctx, uint32_t position) {
    fat_fs_t *fs = ctx->file->fs;
    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;

    // Check if position follows last read
    if (position == ctx->last_position) {
        return 1;  // Continued read
    }

    // Check if within same or next cluster
    uint32_t last_cluster_num = ctx->last_position / cluster_size;
    uint32_t curr_cluster_num = position / cluster_size;

    if (curr_cluster_num == last_cluster_num ||
        curr_cluster_num == last_cluster_num + 1) {
        return 1;
    }

    return 0;
}

// Trigger prefetch of upcoming clusters
static void trigger_prefetch(readahead_context_t *ctx, uint32_t current_cluster) {
    if (!ctx->prefetch_enabled || !g_cache) return;

    fat_fs_t *fs = ctx->file->fs;
    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;

    // Prefetch next N clusters
    uint32_t prefetch_count = ctx->prefetch_depth;
    uint32_t cluster = current_cluster;

    for (uint32_t i = 0; i < prefetch_count; i++) {
        uint32_t next_cluster = get_next_cluster(fs, cluster);
        if (next_cluster < 2 || next_cluster >= fs->cluster_count + 2) {
            break;  // End of chain or invalid
        }

        // Check if already cached
        cache_acquire_lock();
        if (cache_find(fs, next_cluster)) {
            cache_release_lock();
            cluster = next_cluster;
            continue;
        }
        cache_release_lock();

        // Allocate buffer and read
        void *buffer = g_cache->buffer_pool + g_cache->pool_used;
        if (g_cache->pool_used + cluster_size > g_cache->buffer_size) {
            // Need to evict
            fat_cache_evict_lru();
            buffer = g_cache->buffer_pool + g_cache->pool_used;
        }

        if (read_cluster_dma(fs, next_cluster, buffer) > 0) {
            cache_acquire_lock();
            cache_add(fs, next_cluster, buffer, cluster_size);
            g_cache->prefetch_requests++;
            cache_release_lock();

            ctx->bytes_prefetched += cluster_size;
        }

        cluster = next_cluster;
    }
}

int fat_readahead_read(readahead_context_t *ctx, void *buffer, uint32_t size) {
    if (!ctx || !ctx->file || !buffer || size == 0) return -1;
    if (!g_cache) {
        // Fallback to regular read
        return fat_read(ctx->file, buffer, size);
    }

    fat_file_t *file = ctx->file;
    fat_fs_t *fs = file->fs;
    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
    uint32_t position = file->position;
    uint8_t *out = (uint8_t *)buffer;
    uint32_t bytes_read = 0;

    // Detect sequential access
    if (is_sequential_access(ctx, position)) {
        ctx->sequential_count++;
        if (ctx->sequential_count >= RA_SEQUENTIAL_THRESHOLD && !ctx->prefetch_enabled) {
            ctx->prefetch_enabled = 1;
            kprintf("[RA] Sequential access detected, enabling prefetch\n");
        }
    } else {
        ctx->sequential_count = 0;
        if (ctx->prefetch_enabled) {
            // Random access - might want to disable prefetch
            ctx->sequential_count = 0;
        }
    }

    g_cache->total_reads++;

    // Limit read to file size
    if (position + size > file->file_size) {
        size = file->file_size - position;
    }

    // Read cluster by cluster
    while (bytes_read < size && file->current_cluster != 0) {
        uint32_t cluster = file->current_cluster;
        uint32_t cluster_offset = position % cluster_size;
        uint32_t bytes_in_cluster = cluster_size - cluster_offset;
        uint32_t to_copy = (size - bytes_read < bytes_in_cluster) ?
                          (size - bytes_read) : bytes_in_cluster;

        // Check cache first
        cache_acquire_lock();
        cache_entry_t *entry = cache_find(fs, cluster);
        if (entry) {
            // Cache hit
            cache_touch(entry);
            memcpy(out + bytes_read, entry->data + cluster_offset, to_copy);
            g_cache->cache_hits++;
            ctx->cache_hits++;

            // Check if this was prefetched data
            if (entry->access_count == 1) {
                g_cache->prefetch_hits++;
            }

            cache_release_lock();
        } else {
            cache_release_lock();

            // Cache miss - read from disk
            g_cache->cache_misses++;
            ctx->cache_misses++;

            void *cluster_buf = kmalloc(cluster_size);
            if (!cluster_buf) return bytes_read;

            if (read_cluster_dma(fs, cluster, cluster_buf) <= 0) {
                kfree(cluster_buf);
                break;
            }

            // Copy requested data
            memcpy(out + bytes_read, (uint8_t *)cluster_buf + cluster_offset, to_copy);

            // Add to cache
            cache_acquire_lock();
            cache_add(fs, cluster, cluster_buf, cluster_size);
            cache_release_lock();

            kfree(cluster_buf);
        }

        bytes_read += to_copy;
        position += to_copy;

        // Update file position
        file->position = position;

        // Move to next cluster if needed
        if (position % cluster_size == 0 && bytes_read < size) {
            // Get next cluster from FAT
            // Note: This is simplified - real implementation uses fat_next_cluster()
            file->current_cluster = get_next_cluster(fs, cluster);
        }

        ctx->last_cluster = cluster;
    }

    ctx->last_position = file->position;

    // Trigger prefetch if sequential
    if (ctx->sequential_count >= RA_SEQUENTIAL_THRESHOLD) {
        trigger_prefetch(ctx, file->current_cluster);
    }

    return bytes_read;
}

int fat_readahead_seek(readahead_context_t *ctx, uint32_t position) {
    if (!ctx || !ctx->file) return -1;

    fat_file_t *file = ctx->file;

    // Check if seeking backwards or far forward resets sequential detection
    if (position < ctx->last_position ||
        position > ctx->last_position + 2 * ctx->prefetch_depth *
                   (file->fs->sectors_per_cluster * file->fs->bytes_per_sector)) {
        ctx->sequential_count = 0;
    }

    int result = fat_seek(file, position);
    if (result == 0) {
        ctx->last_position = position;
        ctx->last_cluster = file->current_cluster;
    }

    return result;
}

void fat_readahead_hint_sequential(readahead_context_t *ctx) {
    if (!ctx) return;
    ctx->prefetch_enabled = 1;
    ctx->sequential_count = RA_SEQUENTIAL_THRESHOLD;
    ctx->prefetch_depth = g_prefetch_depth * 2;  // More aggressive
}

void fat_readahead_hint_random(readahead_context_t *ctx) {
    if (!ctx) return;
    ctx->prefetch_enabled = 0;
    ctx->sequential_count = 0;
    ctx->prefetch_depth = 1;  // Minimal prefetch
}

// ============================================
// Cache Public API
// ============================================

void *fat_cache_get(fat_fs_t *fs, uint32_t cluster) {
    if (!g_cache || !fs) return NULL;

    cache_acquire_lock();
    cache_entry_t *entry = cache_find(fs, cluster);
    if (entry) {
        cache_touch(entry);
        void *data = entry->data;
        cache_release_lock();
        return data;
    }
    cache_release_lock();
    return NULL;
}

int fat_cache_put(fat_fs_t *fs, uint32_t cluster, void *data, uint32_t size) {
    if (!g_cache || !fs || !data) return -1;

    cache_acquire_lock();
    cache_entry_t *entry = cache_add(fs, cluster, data, size);
    cache_release_lock();

    return entry ? 0 : -1;
}

void fat_cache_invalidate(fat_fs_t *fs, uint32_t cluster) {
    if (!g_cache) return;

    cache_acquire_lock();
    cache_entry_t *entry = cache_find(fs, cluster);
    if (entry) {
        entry->state = CACHE_EMPTY;

        // Remove from LRU list
        if (entry->lru_prev) entry->lru_prev->lru_next = entry->lru_next;
        if (entry->lru_next) entry->lru_next->lru_prev = entry->lru_prev;
        if (g_cache->lru_head == entry) g_cache->lru_head = entry->lru_next;
        if (g_cache->lru_tail == entry) g_cache->lru_tail = entry->lru_prev;

        // Remove from hash table
        uint32_t hash = cluster_hash(cluster);
        if (g_cache->hash_table[hash] == entry) {
            g_cache->hash_table[hash] = NULL;
        }

        g_cache->entry_count--;
    }
    cache_release_lock();
}

void fat_cache_invalidate_fs(fat_fs_t *fs) {
    if (!g_cache || !fs) return;

    cache_acquire_lock();
    for (uint32_t i = 0; i < g_cache->max_entries; i++) {
        cache_entry_t *entry = &g_cache->entries[i];
        if (entry->fs == fs && entry->state != CACHE_EMPTY) {
            entry->state = CACHE_EMPTY;
            entry->fs = NULL;
            g_cache->entry_count--;
        }
    }

    // Rebuild LRU list (simplified - just clear it)
    g_cache->lru_head = NULL;
    g_cache->lru_tail = NULL;

    // Clear hash table for this fs
    for (int i = 0; i < CACHE_HASH_SIZE; i++) {
        if (g_cache->hash_table[i] && g_cache->hash_table[i]->fs == fs) {
            g_cache->hash_table[i] = NULL;
        }
    }
    cache_release_lock();
}

int fat_cache_flush(fat_fs_t *fs) {
    if (!g_cache) return 0;

    int flushed = 0;
    cache_acquire_lock();

    for (uint32_t i = 0; i < g_cache->max_entries; i++) {
        cache_entry_t *entry = &g_cache->entries[i];
        if (entry->state == CACHE_DIRTY) {
            if (fs == NULL || entry->fs == fs) {
                // Write back dirty data
                // TODO: Implement actual writeback using fat_write_cluster
                entry->dirty = 0;
                entry->state = CACHE_VALID;
                flushed++;
            }
        }
    }

    cache_release_lock();
    return flushed;
}

void fat_cache_evict_lru(void) {
    if (!g_cache || !g_cache->lru_tail) return;

    cache_acquire_lock();
    cache_entry_t *victim = g_cache->lru_tail;

    if (victim->dirty) {
        // TODO: Writeback
    }

    // Remove from LRU
    if (victim->lru_prev) {
        victim->lru_prev->lru_next = NULL;
    }
    g_cache->lru_tail = victim->lru_prev;
    if (g_cache->lru_head == victim) {
        g_cache->lru_head = NULL;
    }

    // Remove from hash
    uint32_t hash = cluster_hash(victim->cluster);
    if (g_cache->hash_table[hash] == victim) {
        g_cache->hash_table[hash] = NULL;
    }

    if (victim->access_count == 0) {
        g_cache->prefetch_wastes++;
    }

    victim->state = CACHE_EMPTY;
    victim->fs = NULL;
    g_cache->evictions++;
    g_cache->entry_count--;

    cache_release_lock();
}

// ============================================
// Async Prefetch (DMA)
// ============================================

#define MAX_PREFETCH_REQUESTS   16
static prefetch_request_t prefetch_requests[MAX_PREFETCH_REQUESTS];
static int prefetch_request_count = 0;

prefetch_request_t *fat_prefetch_start(fat_fs_t *fs, uint32_t start_cluster,
                                       uint32_t count) {
    if (!g_cache || !fs || count == 0) return NULL;

    // Find free request slot
    prefetch_request_t *req = NULL;
    for (int i = 0; i < MAX_PREFETCH_REQUESTS; i++) {
        if (prefetch_requests[i].fs == NULL) {
            req = &prefetch_requests[i];
            break;
        }
    }

    if (!req) return NULL;  // No free slots

    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
    uint32_t total_size = cluster_size * count;

    // Allocate buffer
    req->buffer = kmalloc(total_size);
    if (!req->buffer) return NULL;

    req->fs = fs;
    req->cluster = start_cluster;
    req->count = count;
    req->complete = 0;
    req->error = 0;

    // Start async DMA read
    // For now, this is synchronous - a real implementation would use
    // interrupt-driven DMA with completion callbacks
    int read = read_clusters_dma(fs, start_cluster, count, req->buffer);
    if (read <= 0) {
        req->error = 1;
    }
    req->complete = 1;  // Synchronous for now

    prefetch_request_count++;
    g_cache->prefetch_requests++;

    return req;
}

int fat_prefetch_complete(prefetch_request_t *req) {
    return req ? req->complete : 1;
}

int fat_prefetch_wait(prefetch_request_t *req) {
    if (!req) return -1;

    // For synchronous implementation, just return status
    while (!req->complete) {
        __asm__ volatile("pause");
    }

    return req->error ? -1 : 0;
}

void fat_prefetch_cancel(prefetch_request_t *req) {
    if (!req) return;

    // For synchronous implementation, just free resources
    if (req->buffer) {
        kfree(req->buffer);
    }

    req->fs = NULL;
    req->buffer = NULL;
    req->complete = 1;
    prefetch_request_count--;
}

int fat_prefetch_process(prefetch_request_t *req) {
    if (!req || !req->complete || req->error) return -1;

    fat_fs_t *fs = req->fs;
    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;

    cache_acquire_lock();

    // Add each cluster to cache
    for (uint32_t i = 0; i < req->count; i++) {
        uint32_t cluster = req->cluster + i;
        void *data = (uint8_t *)req->buffer + i * cluster_size;

        // Skip if already cached
        if (!cache_find(fs, cluster)) {
            cache_add(fs, cluster, data, cluster_size);
        }
    }

    cache_release_lock();

    // Free request
    if (req->buffer) {
        kfree(req->buffer);
    }
    req->fs = NULL;
    req->buffer = NULL;
    prefetch_request_count--;

    return 0;
}

// ============================================
// Statistics and Debugging
// ============================================

void fat_cache_get_stats(uint64_t *hits, uint64_t *misses, uint64_t *evictions) {
    if (!g_cache) {
        if (hits) *hits = 0;
        if (misses) *misses = 0;
        if (evictions) *evictions = 0;
        return;
    }

    if (hits) *hits = g_cache->cache_hits;
    if (misses) *misses = g_cache->cache_misses;
    if (evictions) *evictions = g_cache->evictions;
}

void fat_cache_print_stats(void) {
    if (!g_cache) {
        kprintf("[RA] Cache not initialized\n");
        return;
    }

    uint64_t total = g_cache->cache_hits + g_cache->cache_misses;
    uint64_t hit_rate = total > 0 ? (g_cache->cache_hits * 100) / total : 0;

    kprintf("[RA] Read-Ahead Cache Statistics:\n");
    kprintf("  Buffer size: %u KB\n", g_cache->buffer_size / 1024);
    kprintf("  Entries: %u / %u\n", g_cache->entry_count, g_cache->max_entries);
    kprintf("  Total reads: %lu\n", g_cache->total_reads);
    kprintf("  Cache hits: %lu (%lu%%)\n", g_cache->cache_hits, hit_rate);
    kprintf("  Cache misses: %lu\n", g_cache->cache_misses);
    kprintf("  Evictions: %lu\n", g_cache->evictions);
    kprintf("  Prefetch requests: %lu\n", g_cache->prefetch_requests);
    kprintf("  Prefetch hits: %lu\n", g_cache->prefetch_hits);
    kprintf("  Prefetch wastes: %lu\n", g_cache->prefetch_wastes);
}

void fat_readahead_print_context(readahead_context_t *ctx) {
    if (!ctx) {
        kprintf("[RA] NULL context\n");
        return;
    }

    kprintf("[RA] Read-Ahead Context:\n");
    kprintf("  File: %s\n", ctx->file ? ctx->file->name : "(null)");
    kprintf("  Last cluster: %u\n", ctx->last_cluster);
    kprintf("  Last position: %u\n", ctx->last_position);
    kprintf("  Sequential count: %u\n", ctx->sequential_count);
    kprintf("  Prefetch enabled: %s\n", ctx->prefetch_enabled ? "yes" : "no");
    kprintf("  Prefetch depth: %u clusters\n", ctx->prefetch_depth);
    kprintf("  Bytes prefetched: %lu\n", ctx->bytes_prefetched);
    kprintf("  Cache hits: %lu, misses: %lu\n", ctx->cache_hits, ctx->cache_misses);
}

void fat_cache_dump(void) {
    if (!g_cache) {
        kprintf("[RA] Cache not initialized\n");
        return;
    }

    kprintf("[RA] Cache Dump (%u entries):\n", g_cache->entry_count);

    for (uint32_t i = 0; i < g_cache->max_entries && i < 20; i++) {
        cache_entry_t *entry = &g_cache->entries[i];
        if (entry->state != CACHE_EMPTY) {
            kprintf("  [%u] cluster %u, state %d, accesses %u, dirty %d\n",
                    i, entry->cluster, entry->state, entry->access_count, entry->dirty);
        }
    }

    if (g_cache->entry_count > 20) {
        kprintf("  ... and %u more entries\n", g_cache->entry_count - 20);
    }
}

// Compatibility function for existing fat.c
void fat_cache_stats(void) {
    fat_cache_print_stats();
}
