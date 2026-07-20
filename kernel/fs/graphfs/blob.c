// blob.c - Content-Addressed Blob Store for GraphFS
// Part of MayteraOS - The First LLM-Native Operating System

#include "blob.h"
#include "../../string.h"
#include "../../serial.h"
#include "../../mm/heap.h"
#include "../../crypto/crypto.h"

// ============================================================================
// Internal Helper Functions
// ============================================================================

// Build the full path for a blob file: /BLOBS/xx/yyyyy...
// Uses first 2 hex chars as subdirectory for distribution
static void blob_build_path(blob_store_t *store, const blob_hash_t *hash,
                            char *path_out, size_t path_size) {
    char hex[BLOB_HASH_HEX_SIZE];
    blob_hash_to_hex(hash, hex);

    // Format: base_path/XX/full_hash
    snprintf(path_out, path_size, "%s/%c%c/%s",
             store->base_path, hex[0], hex[1], hex);
}

// Build path for subdirectory: /BLOBS/xx
static void blob_build_dir_path(blob_store_t *store, const char *subdir,
                                char *path_out, size_t path_size) {
    snprintf(path_out, path_size, "%s/%s", store->base_path, subdir);
}

// Build path for index file
static void blob_build_index_path(blob_store_t *store, char *path_out, size_t path_size) {
    snprintf(path_out, path_size, "%s/INDEX.DAT", store->base_path);
}

// Build path for metadata file for a blob
static void blob_build_meta_path(blob_store_t *store, const blob_hash_t *hash,
                                 char *path_out, size_t path_size) {
    char hex[BLOB_HASH_HEX_SIZE];
    blob_hash_to_hex(hash, hex);

    // Metadata file is .meta suffix
    snprintf(path_out, path_size, "%s/%c%c/%s.meta",
             store->base_path, hex[0], hex[1], hex);
}

// Create a directory if it doesn't exist
static int ensure_directory(fat_fs_t *fs, const char *path) {
    if (fat_exists(fs, path) == 1) {
        return BLOB_OK;  // Already exists
    }
    if (fat_mkdir(fs, path) != 0) {
        return BLOB_ERR_IO;
    }
    return BLOB_OK;
}

// Read metadata from a .meta file
static int read_blob_metadata(blob_store_t *store, const blob_hash_t *hash,
                              blob_metadata_t *meta_out) {
    char meta_path[128];
    blob_build_meta_path(store, hash, meta_path, sizeof(meta_path));

    uint32_t size;
    void *data = fat_read_file(store->fs, meta_path, &size);
    if (!data) {
        return BLOB_ERR_NOT_FOUND;
    }

    if (size != sizeof(blob_metadata_t)) {
        kfree(data);
        return BLOB_ERR_CORRUPT;
    }

    memcpy(meta_out, data, sizeof(blob_metadata_t));
    kfree(data);
    return BLOB_OK;
}

// Write metadata to a .meta file
static int write_blob_metadata(blob_store_t *store, const blob_metadata_t *meta) {
    char meta_path[128];
    blob_build_meta_path(store, &meta->hash, meta_path, sizeof(meta_path));

    if (fat_write_file(store->fs, meta_path, meta, sizeof(blob_metadata_t)) != 0) {
        return BLOB_ERR_IO;
    }
    return BLOB_OK;
}

// Delete metadata file
static int delete_blob_metadata(blob_store_t *store, const blob_hash_t *hash) {
    char meta_path[128];
    blob_build_meta_path(store, hash, meta_path, sizeof(meta_path));
    fat_delete(store->fs, meta_path);
    return BLOB_OK;
}

// Get current timestamp (placeholder - would use RTC)
static uint64_t get_current_time(void) {
    // TODO: Implement proper RTC time reading
    // For now return a placeholder
    static uint64_t fake_time = 1700000000;  // Nov 2023
    return fake_time++;
}

// ============================================================================
// Hash Utilities Implementation
// ============================================================================

void blob_hash_compute(const void *data, size_t size, blob_hash_t *hash_out) {
    sha256(data, size, hash_out->bytes);
}

void blob_hash_to_hex(const blob_hash_t *hash, char *hex_out) {
    static const char hex_chars[] = "0123456789abcdef";

    for (int i = 0; i < BLOB_HASH_SIZE; i++) {
        hex_out[i * 2]     = hex_chars[(hash->bytes[i] >> 4) & 0x0F];
        hex_out[i * 2 + 1] = hex_chars[hash->bytes[i] & 0x0F];
    }
    hex_out[BLOB_HASH_SIZE * 2] = '\0';
}

int blob_hash_from_hex(const char *hex, blob_hash_t *hash_out) {
    if (strlen(hex) != BLOB_HASH_SIZE * 2) {
        return BLOB_ERR_INVALID;
    }

    for (int i = 0; i < BLOB_HASH_SIZE; i++) {
        int high, low;

        char h = hex[i * 2];
        char l = hex[i * 2 + 1];

        // Parse high nibble
        if (h >= '0' && h <= '9')      high = h - '0';
        else if (h >= 'a' && h <= 'f') high = h - 'a' + 10;
        else if (h >= 'A' && h <= 'F') high = h - 'A' + 10;
        else return BLOB_ERR_INVALID;

        // Parse low nibble
        if (l >= '0' && l <= '9')      low = l - '0';
        else if (l >= 'a' && l <= 'f') low = l - 'a' + 10;
        else if (l >= 'A' && l <= 'F') low = l - 'A' + 10;
        else return BLOB_ERR_INVALID;

        hash_out->bytes[i] = (high << 4) | low;
    }

    return BLOB_OK;
}

int blob_hash_compare(const blob_hash_t *a, const blob_hash_t *b) {
    return memcmp(a->bytes, b->bytes, BLOB_HASH_SIZE);
}

int blob_hash_is_zero(const blob_hash_t *hash) {
    for (int i = 0; i < BLOB_HASH_SIZE; i++) {
        if (hash->bytes[i] != 0) {
            return 0;
        }
    }
    return 1;
}

void blob_hash_clear(blob_hash_t *hash) {
    memset(hash->bytes, 0, BLOB_HASH_SIZE);
}

// ============================================================================
// Initialization and Cleanup
// ============================================================================

int blob_store_init(blob_store_t *store, fat_fs_t *fs, const char *base_path) {
    if (!store || !fs || !base_path) {
        return BLOB_ERR_INVALID;
    }

    memset(store, 0, sizeof(blob_store_t));
    store->fs = fs;
    strncpy(store->base_path, base_path, sizeof(store->base_path) - 1);
    store->base_path[sizeof(store->base_path) - 1] = '\0';

    // Check if base directory exists
    if (fat_exists(fs, base_path) != 1) {
        kprintf("[BLOB] Base directory %s does not exist, creating...\n", base_path);
        if (blob_store_format(store) != BLOB_OK) {
            return BLOB_ERR_IO;
        }
    }

    // Load index if exists (for statistics)
    char index_path[128];
    blob_build_index_path(store, index_path, sizeof(index_path));

    uint32_t index_size;
    void *index_data = fat_read_file(fs, index_path, &index_size);
    if (index_data && index_size >= sizeof(blob_index_header_t)) {
        blob_index_header_t *header = (blob_index_header_t *)index_data;
        if (header->magic == BLOB_INDEX_MAGIC && header->version == BLOB_INDEX_VERSION) {
            store->blob_count = header->entry_count;
            store->total_size = header->total_data_size;
            store->dedup_savings = header->dedup_savings;
            kprintf("[BLOB] Loaded index: %u blobs, %llu bytes\n",
                    store->blob_count, store->total_size);
        }
        kfree(index_data);
    }

    store->initialized = 1;
    kprintf("[BLOB] Blob store initialized at %s\n", base_path);
    return BLOB_OK;
}

void blob_store_shutdown(blob_store_t *store) {
    if (!store || !store->initialized) {
        return;
    }

    // Save index
    char index_path[128];
    blob_build_index_path(store, index_path, sizeof(index_path));

    blob_index_header_t header;
    header.magic = BLOB_INDEX_MAGIC;
    header.version = BLOB_INDEX_VERSION;
    header.entry_count = store->blob_count;
    header.reserved = 0;
    header.total_data_size = store->total_size;
    header.dedup_savings = store->dedup_savings;

    fat_write_file(store->fs, index_path, &header, sizeof(header));

    store->initialized = 0;
    kprintf("[BLOB] Blob store shutdown complete\n");
}

int blob_store_format(blob_store_t *store) {
    if (!store || !store->fs) {
        return BLOB_ERR_INVALID;
    }

    kprintf("[BLOB] Formatting blob store at %s\n", store->base_path);

    // Create base directory
    if (ensure_directory(store->fs, store->base_path) != BLOB_OK) {
        kprintf("[BLOB] Failed to create base directory\n");
        return BLOB_ERR_IO;
    }

    // Create 256 subdirectories (00-ff)
    char subdir[128];
    static const char hex_chars[] = "0123456789abcdef";

    for (int i = 0; i < 256; i++) {
        char dir_name[3];
        dir_name[0] = hex_chars[(i >> 4) & 0x0F];
        dir_name[1] = hex_chars[i & 0x0F];
        dir_name[2] = '\0';

        blob_build_dir_path(store, dir_name, subdir, sizeof(subdir));

        if (ensure_directory(store->fs, subdir) != BLOB_OK) {
            kprintf("[BLOB] Failed to create subdirectory %s\n", subdir);
            return BLOB_ERR_IO;
        }

        // Progress indication every 16 directories
        if ((i & 0x0F) == 0x0F) {
            kprintf("[BLOB] Created directories: %d/256\n", i + 1);
        }
    }

    // Create initial index file
    char index_path[128];
    blob_build_index_path(store, index_path, sizeof(index_path));

    blob_index_header_t header;
    memset(&header, 0, sizeof(header));
    header.magic = BLOB_INDEX_MAGIC;
    header.version = BLOB_INDEX_VERSION;
    header.entry_count = 0;
    header.total_data_size = 0;
    header.dedup_savings = 0;

    if (fat_write_file(store->fs, index_path, &header, sizeof(header)) != 0) {
        kprintf("[BLOB] Failed to create index file\n");
        return BLOB_ERR_IO;
    }

    store->blob_count = 0;
    store->total_size = 0;
    store->dedup_savings = 0;

    kprintf("[BLOB] Blob store formatted successfully\n");
    return BLOB_OK;
}

// ============================================================================
// Core Blob Operations
// ============================================================================

int blob_store(blob_store_t *store, const void *data, size_t size,
               blob_hash_t *hash_out) {
    if (!store || !store->initialized || !data || !hash_out) {
        return BLOB_ERR_INVALID;
    }

    if (size > BLOB_MAX_SIZE) {
        return BLOB_ERR_TOO_LARGE;
    }

    if (size == 0) {
        // Empty blob - return hash of empty data
        blob_hash_compute(data, 0, hash_out);
        return BLOB_OK;
    }

    // Compute hash
    blob_hash_t hash;
    blob_hash_compute(data, size, &hash);

    // Check if blob already exists (deduplication)
    if (blob_exists(store, &hash) == 1) {
        // Blob exists - increment refcount and return existing hash
        blob_ref(store, &hash);
        memcpy(hash_out, &hash, sizeof(blob_hash_t));
        store->dedup_savings += size;

        char hex[BLOB_HASH_HEX_SIZE];
        blob_hash_to_hex(&hash, hex);
        kprintf("[BLOB] Deduplicated %.16s... (%zu bytes)\n", hex, size);
        return BLOB_OK;
    }

    // Build path for new blob
    char blob_path[128];
    blob_build_path(store, &hash, blob_path, sizeof(blob_path));

    // Ensure subdirectory exists (should exist from format, but be safe)
    char hex[BLOB_HASH_HEX_SIZE];
    blob_hash_to_hex(&hash, hex);
    char subdir[128];
    snprintf(subdir, sizeof(subdir), "%s/%c%c", store->base_path, hex[0], hex[1]);
    ensure_directory(store->fs, subdir);

    // Write blob data
    if (fat_write_file(store->fs, blob_path, data, size) != 0) {
        kprintf("[BLOB] Failed to write blob to %s\n", blob_path);
        return BLOB_ERR_IO;
    }

    // Create and write metadata
    blob_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    memcpy(&meta.hash, &hash, sizeof(blob_hash_t));
    meta.size = (uint32_t)size;
    meta.refcount = 1;
    meta.created_time = get_current_time();
    meta.flags = BLOB_FLAG_NONE;
    meta.stored_size = (uint32_t)size;

    if (write_blob_metadata(store, &meta) != BLOB_OK) {
        // Clean up blob file on metadata failure
        fat_delete(store->fs, blob_path);
        kprintf("[BLOB] Failed to write metadata\n");
        return BLOB_ERR_IO;
    }

    // Update statistics
    store->blob_count++;
    store->total_size += size;

    // Return hash
    memcpy(hash_out, &hash, sizeof(blob_hash_t));

    kprintf("[BLOB] Stored %.16s... (%zu bytes)\n", hex, size);
    return BLOB_OK;
}

int blob_load(blob_store_t *store, const blob_hash_t *hash,
              void *buffer, size_t buffer_size, size_t *actual_size) {
    if (!store || !store->initialized || !hash || !buffer) {
        return BLOB_ERR_INVALID;
    }

    // Build path
    char blob_path[128];
    blob_build_path(store, hash, blob_path, sizeof(blob_path));

    // Read blob data
    uint32_t file_size;
    void *data = fat_read_file(store->fs, blob_path, &file_size);
    if (!data) {
        return BLOB_ERR_NOT_FOUND;
    }

    // Check buffer size
    if (file_size > buffer_size) {
        kfree(data);
        return BLOB_ERR_INVALID;  // Buffer too small
    }

    // Verify hash matches
    blob_hash_t computed_hash;
    blob_hash_compute(data, file_size, &computed_hash);
    if (blob_hash_compare(&computed_hash, hash) != 0) {
        kfree(data);
        kprintf("[BLOB] Hash mismatch - data corruption detected!\n");
        return BLOB_ERR_CORRUPT;
    }

    // Copy to buffer
    memcpy(buffer, data, file_size);
    if (actual_size) {
        *actual_size = file_size;
    }

    kfree(data);

    char hex[BLOB_HASH_HEX_SIZE];
    blob_hash_to_hex(hash, hex);
    kprintf("[BLOB] Loaded %.16s... (%u bytes)\n", hex, file_size);

    return BLOB_OK;
}

int blob_exists(blob_store_t *store, const blob_hash_t *hash) {
    if (!store || !store->initialized || !hash) {
        return BLOB_ERR_INVALID;
    }

    char blob_path[128];
    blob_build_path(store, hash, blob_path, sizeof(blob_path));

    return fat_exists(store->fs, blob_path);
}

int blob_get_metadata(blob_store_t *store, const blob_hash_t *hash,
                      blob_metadata_t *metadata_out) {
    if (!store || !store->initialized || !hash || !metadata_out) {
        return BLOB_ERR_INVALID;
    }

    return read_blob_metadata(store, hash, metadata_out);
}

ssize_t blob_get_size(blob_store_t *store, const blob_hash_t *hash) {
    blob_metadata_t meta;
    int ret = blob_get_metadata(store, hash, &meta);
    if (ret != BLOB_OK) {
        return ret;
    }
    return (ssize_t)meta.size;
}

// ============================================================================
// Reference Counting
// ============================================================================

int blob_ref(blob_store_t *store, const blob_hash_t *hash) {
    if (!store || !store->initialized || !hash) {
        return BLOB_ERR_INVALID;
    }

    blob_metadata_t meta;
    int ret = read_blob_metadata(store, hash, &meta);
    if (ret != BLOB_OK) {
        return ret;
    }

    meta.refcount++;
    ret = write_blob_metadata(store, &meta);
    if (ret != BLOB_OK) {
        return ret;
    }

    return (int)meta.refcount;
}

int blob_unref(blob_store_t *store, const blob_hash_t *hash) {
    if (!store || !store->initialized || !hash) {
        return BLOB_ERR_INVALID;
    }

    blob_metadata_t meta;
    int ret = read_blob_metadata(store, hash, &meta);
    if (ret != BLOB_OK) {
        return ret;
    }

    if (meta.refcount > 0) {
        meta.refcount--;
    }

    ret = write_blob_metadata(store, &meta);
    if (ret != BLOB_OK) {
        return ret;
    }

    return (int)meta.refcount;
}

int blob_get_refcount(blob_store_t *store, const blob_hash_t *hash) {
    blob_metadata_t meta;
    int ret = blob_get_metadata(store, hash, &meta);
    if (ret != BLOB_OK) {
        return ret;
    }
    return (int)meta.refcount;
}

// ============================================================================
// Garbage Collection
// ============================================================================

int blob_gc(blob_store_t *store, uint32_t max_delete, uint32_t *deleted_out) {
    if (!store || !store->initialized) {
        return BLOB_ERR_INVALID;
    }

    uint32_t deleted = 0;
    static const char hex_chars[] = "0123456789abcdef";

    kprintf("[BLOB] Starting garbage collection...\n");

    // Iterate through all subdirectories
    for (int i = 0; i < 256 && (max_delete == 0 || deleted < max_delete); i++) {
        char dir_name[3];
        dir_name[0] = hex_chars[(i >> 4) & 0x0F];
        dir_name[1] = hex_chars[i & 0x0F];
        dir_name[2] = '\0';

        char subdir[128];
        blob_build_dir_path(store, dir_name, subdir, sizeof(subdir));

        // Open directory and iterate entries
        fat_file_t dir;
        if (fat_open(store->fs, subdir, &dir) != 0) {
            continue;
        }

        fat_dir_entry_t entry;
        char name[256];
        while (fat_readdir(&dir, &entry, name) == 0) {
            // Skip . and ..
            if (name[0] == '.') continue;

            // Skip metadata files
            if (strstr(name, ".meta") != NULL) continue;

            // This should be a blob file (hash as filename)
            if (strlen(name) != 64) continue;  // SHA-256 hex = 64 chars

            // Parse hash
            blob_hash_t hash;
            if (blob_hash_from_hex(name, &hash) != BLOB_OK) {
                continue;
            }

            // Check refcount
            blob_metadata_t meta;
            if (read_blob_metadata(store, &hash, &meta) == BLOB_OK) {
                if (meta.refcount == 0) {
                    // Delete blob and metadata
                    char blob_path[128];
                    blob_build_path(store, &hash, blob_path, sizeof(blob_path));
                    fat_delete(store->fs, blob_path);
                    delete_blob_metadata(store, &hash);

                    store->blob_count--;
                    store->total_size -= meta.size;
                    deleted++;

                    kprintf("[BLOB] GC: Deleted %.16s...\n", name);

                    if (max_delete > 0 && deleted >= max_delete) {
                        break;
                    }
                }
            }
        }

        fat_close(&dir);
    }

    if (deleted_out) {
        *deleted_out = deleted;
    }

    kprintf("[BLOB] Garbage collection complete: %u blobs deleted\n", deleted);
    return BLOB_OK;
}

// ============================================================================
// Statistics and Debugging
// ============================================================================

void blob_store_stats(blob_store_t *store, uint32_t *blob_count,
                      uint64_t *total_size, uint64_t *dedup_savings) {
    if (!store) return;

    if (blob_count) *blob_count = store->blob_count;
    if (total_size) *total_size = store->total_size;
    if (dedup_savings) *dedup_savings = store->dedup_savings;
}

void blob_store_print_info(blob_store_t *store) {
    if (!store || !store->initialized) {
        kprintf("[BLOB] Store not initialized\n");
        return;
    }

    kprintf("\n[BLOB] Blob Store Information:\n");
    kprintf("  Base Path:      %s\n", store->base_path);
    kprintf("  Blob Count:     %u\n", store->blob_count);
    kprintf("  Total Size:     %llu bytes\n", store->total_size);
    kprintf("  Dedup Savings:  %llu bytes\n", store->dedup_savings);

    // Calculate efficiency
    if (store->total_size + store->dedup_savings > 0) {
        uint64_t total = store->total_size + store->dedup_savings;
        uint32_t efficiency = (uint32_t)((store->dedup_savings * 100) / total);
        kprintf("  Dedup Rate:     %u%%\n", efficiency);
    }
}

int blob_verify(blob_store_t *store, const blob_hash_t *hash) {
    if (!store || !store->initialized || !hash) {
        return BLOB_ERR_INVALID;
    }

    // Get metadata for size
    blob_metadata_t meta;
    int ret = read_blob_metadata(store, hash, &meta);
    if (ret != BLOB_OK) {
        return ret;
    }

    // Allocate buffer
    void *buffer = kmalloc(meta.size);
    if (!buffer) {
        return BLOB_ERR_NOMEM;
    }

    // Load and verify
    size_t actual_size;
    ret = blob_load(store, hash, buffer, meta.size, &actual_size);

    kfree(buffer);
    return ret;
}

int blob_store_verify_all(blob_store_t *store) {
    if (!store || !store->initialized) {
        return BLOB_ERR_INVALID;
    }

    int corrupt_count = 0;
    static const char hex_chars[] = "0123456789abcdef";

    kprintf("[BLOB] Verifying all blobs...\n");

    // Iterate through all subdirectories
    for (int i = 0; i < 256; i++) {
        char dir_name[3];
        dir_name[0] = hex_chars[(i >> 4) & 0x0F];
        dir_name[1] = hex_chars[i & 0x0F];
        dir_name[2] = '\0';

        char subdir[128];
        blob_build_dir_path(store, dir_name, subdir, sizeof(subdir));

        fat_file_t dir;
        if (fat_open(store->fs, subdir, &dir) != 0) {
            continue;
        }

        fat_dir_entry_t entry;
        char name[256];
        while (fat_readdir(&dir, &entry, name) == 0) {
            if (name[0] == '.') continue;
            if (strstr(name, ".meta") != NULL) continue;
            if (strlen(name) != 64) continue;

            blob_hash_t hash;
            if (blob_hash_from_hex(name, &hash) != BLOB_OK) {
                continue;
            }

            int ret = blob_verify(store, &hash);
            if (ret == BLOB_ERR_CORRUPT) {
                kprintf("[BLOB] CORRUPT: %.16s...\n", name);
                corrupt_count++;
            }
        }

        fat_close(&dir);

        // Progress every 16 directories
        if ((i & 0x0F) == 0x0F) {
            kprintf("[BLOB] Verified: %d/256 directories\n", i + 1);
        }
    }

    kprintf("[BLOB] Verification complete: %d corrupt blobs\n", corrupt_count);
    return corrupt_count;
}
