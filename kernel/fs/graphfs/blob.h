// blob.h - Content-Addressed Blob Store for GraphFS
// Part of MayteraOS - The First LLM-Native Operating System
//
// Blobs are immutable, content-addressed data blocks identified by their
// SHA-256 hash. This provides:
// - Automatic deduplication (identical content stored once)
// - Data integrity verification
// - Immutable history (blobs are never modified, only added/removed)
//
// Storage format: /BLOBS/ab/cdef1234...
// The first 2 hex characters form a directory for distributing files.

#ifndef GRAPHFS_BLOB_H
#define GRAPHFS_BLOB_H

#include "../../types.h"
#include "../../crypto/crypto.h"
#include "../fat.h"

// Hash size in bytes (SHA-256 = 32 bytes = 256 bits)
#define BLOB_HASH_SIZE      32
#define BLOB_HASH_HEX_SIZE  (BLOB_HASH_SIZE * 2 + 1)  // +1 for null terminator

// Maximum blob size (for safety, 16MB per blob)
#define BLOB_MAX_SIZE       (16 * 1024 * 1024)

// Blob store magic number for index file
#define BLOB_INDEX_MAGIC    0x424C4F42  // "BLOB"
#define BLOB_INDEX_VERSION  1

// Blob flags
#define BLOB_FLAG_NONE      0x00
#define BLOB_FLAG_COMPRESSED 0x01  // Future: blob is compressed

// Error codes
#define BLOB_OK             0
#define BLOB_ERR_INVALID   -1   // Invalid parameters
#define BLOB_ERR_NOT_FOUND -2   // Blob not found
#define BLOB_ERR_IO        -3   // I/O error
#define BLOB_ERR_NOMEM     -4   // Out of memory
#define BLOB_ERR_EXISTS    -5   // Blob already exists
#define BLOB_ERR_CORRUPT   -6   // Data corruption detected
#define BLOB_ERR_FULL      -7   // Blob store full
#define BLOB_ERR_TOO_LARGE -8   // Blob exceeds max size

// Blob hash (SHA-256)
typedef struct {
    uint8_t bytes[BLOB_HASH_SIZE];
} blob_hash_t;

// Blob metadata stored in index
typedef struct {
    blob_hash_t hash;           // SHA-256 hash (also serves as ID)
    uint32_t    size;           // Original data size
    uint32_t    refcount;       // Reference count for garbage collection
    uint64_t    created_time;   // Unix timestamp when blob was created
    uint32_t    flags;          // BLOB_FLAG_* flags
    uint32_t    stored_size;    // Size on disk (may differ if compressed)
} __attribute__((packed)) blob_metadata_t;

// Blob index header (stored at start of index file)
typedef struct {
    uint32_t magic;             // BLOB_INDEX_MAGIC
    uint32_t version;           // BLOB_INDEX_VERSION
    uint32_t entry_count;       // Number of blobs in store
    uint32_t reserved;          // Padding for alignment
    uint64_t total_data_size;   // Total size of all blob data
    uint64_t dedup_savings;     // Bytes saved by deduplication
} __attribute__((packed)) blob_index_header_t;

// Blob store state
typedef struct {
    fat_fs_t   *fs;             // Underlying FAT filesystem
    char        base_path[64];  // Base path for blob storage (e.g., "/BLOBS")
    int         initialized;    // Is store initialized?

    // Statistics
    uint32_t    blob_count;     // Number of blobs
    uint64_t    total_size;     // Total data stored
    uint64_t    dedup_savings;  // Bytes saved by deduplication

    // Index cache (small in-memory cache for frequently accessed metadata)
    // Future: implement LRU cache for blob metadata
} blob_store_t;

// ============================================================================
// Initialization and Cleanup
// ============================================================================

// Initialize the blob store on a FAT filesystem
// base_path: Directory to store blobs (e.g., "/BLOBS")
// Returns: BLOB_OK on success, error code on failure
int blob_store_init(blob_store_t *store, fat_fs_t *fs, const char *base_path);

// Shutdown the blob store and flush any pending data
void blob_store_shutdown(blob_store_t *store);

// Format/create the blob store directory structure
// This creates the base directory and subdirectories (00-ff)
int blob_store_format(blob_store_t *store);

// ============================================================================
// Core Blob Operations
// ============================================================================

// Store data as a blob, returning its hash
// data: Pointer to data to store
// size: Size of data in bytes
// hash_out: Output parameter for the computed hash
// Returns: BLOB_OK on success, error code on failure
// Note: If identical content exists, returns existing hash (deduplication)
int blob_store(blob_store_t *store, const void *data, size_t size,
               blob_hash_t *hash_out);

// Load blob data by hash
// hash: The hash of the blob to load
// buffer: Buffer to receive data (must be large enough)
// buffer_size: Size of buffer
// actual_size: Output parameter for actual blob size
// Returns: BLOB_OK on success, error code on failure
int blob_load(blob_store_t *store, const blob_hash_t *hash,
              void *buffer, size_t buffer_size, size_t *actual_size);

// Check if a blob exists
// Returns: 1 if exists, 0 if not, negative on error
int blob_exists(blob_store_t *store, const blob_hash_t *hash);

// Get blob metadata without loading data
int blob_get_metadata(blob_store_t *store, const blob_hash_t *hash,
                      blob_metadata_t *metadata_out);

// Get blob size without loading data
// Returns: size in bytes, or negative error code
ssize_t blob_get_size(blob_store_t *store, const blob_hash_t *hash);

// ============================================================================
// Reference Counting (for Garbage Collection)
// ============================================================================

// Increment reference count for a blob
// Returns: New reference count, or negative error code
int blob_ref(blob_store_t *store, const blob_hash_t *hash);

// Decrement reference count for a blob
// If count reaches 0, blob becomes eligible for garbage collection
// Returns: New reference count (0 means blob can be deleted), or negative error
int blob_unref(blob_store_t *store, const blob_hash_t *hash);

// Get current reference count
// Returns: Reference count, or negative error code
int blob_get_refcount(blob_store_t *store, const blob_hash_t *hash);

// ============================================================================
// Garbage Collection
// ============================================================================

// Delete blobs with refcount == 0
// max_delete: Maximum number of blobs to delete (0 = unlimited)
// deleted_out: Output parameter for number of blobs deleted
// Returns: BLOB_OK on success, error code on failure
int blob_gc(blob_store_t *store, uint32_t max_delete, uint32_t *deleted_out);

// ============================================================================
// Hash Utilities
// ============================================================================

// Compute SHA-256 hash of data
void blob_hash_compute(const void *data, size_t size, blob_hash_t *hash_out);

// Convert hash to hex string
// hex_out must be at least BLOB_HASH_HEX_SIZE bytes
void blob_hash_to_hex(const blob_hash_t *hash, char *hex_out);

// Parse hex string to hash
// Returns: BLOB_OK on success, BLOB_ERR_INVALID if string is invalid
int blob_hash_from_hex(const char *hex, blob_hash_t *hash_out);

// Compare two hashes
// Returns: 0 if equal, non-zero if different
int blob_hash_compare(const blob_hash_t *a, const blob_hash_t *b);

// Check if hash is all zeros (invalid/empty hash)
int blob_hash_is_zero(const blob_hash_t *hash);

// Set hash to all zeros
void blob_hash_clear(blob_hash_t *hash);

// ============================================================================
// Statistics and Debugging
// ============================================================================

// Get blob store statistics
void blob_store_stats(blob_store_t *store, uint32_t *blob_count,
                      uint64_t *total_size, uint64_t *dedup_savings);

// Print blob store information to debug output
void blob_store_print_info(blob_store_t *store);

// Verify integrity of a blob (recompute hash and compare)
// Returns: BLOB_OK if valid, BLOB_ERR_CORRUPT if data mismatch
int blob_verify(blob_store_t *store, const blob_hash_t *hash);

// Verify integrity of entire blob store
// Returns: Number of corrupt blobs found (0 = all good), negative on error
int blob_store_verify_all(blob_store_t *store);

// ============================================================================
// Testing
// ============================================================================

// Run full test suite on an initialized blob store
// Returns: Number of failed tests (0 = all passed)
int blob_store_run_tests(blob_store_t *store);

// Initialize and run self-test (convenience function)
// Returns: Number of failed tests (0 = all passed)
int blob_store_self_test(fat_fs_t *fs, const char *base_path);

#endif // GRAPHFS_BLOB_H
