// version.h - Version Chain Management for GraphFS (Task #39)
// MayteraOS Immutable Versioned Filesystem
//
// Every write to a file creates a new version. Old versions are preserved
// and accessible via version operations. This enables:
// - Full history of all changes
// - Point-in-time recovery
// - Diff between versions
// - Branching and merging (future)
//
// Copyright (c) 2026 MayteraOS Project

#ifndef GRAPHFS_VERSION_H
#define GRAPHFS_VERSION_H

#include "../../types.h"
#include "node.h"

// =============================================================================
// Version Constants
// =============================================================================

// Maximum versions to return in history query
#define GFS_MAX_HISTORY_RESULTS     1024

// Diff output modes
#define GFS_DIFF_UNIFIED            0   // Unified diff format
#define GFS_DIFF_SIDE_BY_SIDE       1   // Side by side
#define GFS_DIFF_BINARY             2   // Binary diff (patch format)
#define GFS_DIFF_STATS_ONLY         3   // Just statistics

// =============================================================================
// Version Info Structure
// =============================================================================

typedef struct gfs_version_info {
    uint64_t node_id;                           // Node ID of this version
    uint64_t version_number;                    // Version number
    uint64_t parent_version_node;               // Node ID of parent version (0 = first)
    uint64_t child_version_node;                // Node ID of next version (0 = latest)

    uint8_t content_hash[GFS_HASH_SIZE];        // Content hash
    uint64_t size;                              // Content size

    uint64_t created_at;                        // When this version was created
    uint64_t committer_pid;                     // Who created it
    char commit_message[GFS_MAX_COMMIT_MSG];    // Why

    bool is_latest;                             // Is this the current version?
    bool is_deleted;                            // Was this version deleted?
} gfs_version_info_t;

// =============================================================================
// Diff Result Structure
// =============================================================================

typedef struct gfs_diff_stats {
    uint64_t additions;             // Lines/bytes added
    uint64_t deletions;             // Lines/bytes deleted
    uint64_t modifications;         // Lines/bytes modified
    uint64_t unchanged;             // Lines/bytes unchanged
    bool binary;                    // Were files binary?
} gfs_diff_stats_t;

typedef struct gfs_diff_hunk {
    uint64_t old_start;             // Start line in old version
    uint64_t old_count;             // Number of lines from old
    uint64_t new_start;             // Start line in new version
    uint64_t new_count;             // Number of lines in new
    char *content;                  // Diff content (dynamically allocated)
    size_t content_len;             // Content length
} gfs_diff_hunk_t;

typedef struct gfs_diff_result {
    gfs_diff_stats_t stats;         // Overall statistics
    gfs_diff_hunk_t *hunks;         // Array of diff hunks
    uint32_t hunk_count;            // Number of hunks
    uint32_t hunk_capacity;         // Allocated hunks
    int mode;                       // Diff mode used
} gfs_diff_result_t;

// =============================================================================
// Branch Info (for future branching support)
// =============================================================================

typedef struct gfs_branch {
    uint64_t branch_id;             // Unique branch ID
    char name[64];                  // Branch name (e.g., "main", "feature-x")
    uint64_t head_node;             // Current head node ID
    uint64_t base_node;             // Node where branch was created
    uint64_t created_at;            // When branch was created
    uint64_t created_by_pid;        // Who created it
    bool is_default;                // Is this the default branch?
} gfs_branch_t;

// =============================================================================
// Version Chain API
// =============================================================================

/**
 * Initialize version subsystem
 */
void gfs_version_init(void);

/**
 * Get version history for a path
 * Returns all versions from newest to oldest
 * @param path File path
 * @param versions Output array
 * @param max_versions Maximum versions to return
 * @return Number of versions found, or negative error
 */
int gfs_history(const char *path, gfs_version_info_t *versions, size_t max_versions);

/**
 * Get version history by node ID
 * @param node_id Node ID (any version)
 * @param versions Output array
 * @param max_versions Maximum versions to return
 * @return Number of versions found, or negative error
 */
int gfs_history_by_node(uint64_t node_id, gfs_version_info_t *versions, size_t max_versions);

/**
 * Checkout a specific version (read-only view)
 * Returns a temporary node that can only be read
 * @param path File path
 * @param version Version number to checkout
 * @param node Output node pointer
 * @return 0 on success, negative error on failure
 */
int gfs_checkout(const char *path, uint64_t version, gfs_node_t **node);

/**
 * Checkout by node ID
 * @param node_id Node ID
 * @param version Version number
 * @param node Output node pointer
 * @return 0 on success, negative error on failure
 */
int gfs_checkout_by_node(uint64_t node_id, uint64_t version, gfs_node_t **node);

/**
 * Revert to a previous version
 * Creates a NEW version with the content of the old version
 * @param path File path
 * @param version Version number to revert to
 * @param commit_message Message explaining the revert
 * @param cap_id Capability for modification
 * @param new_version_id Output: ID of new version
 * @return 0 on success, negative error on failure
 */
int gfs_revert(const char *path, uint64_t version, const char *commit_message,
               uint64_t cap_id, uint64_t *new_version_id);

/**
 * Revert by node ID
 * @param node_id Node ID
 * @param version Version number to revert to
 * @param commit_message Message explaining the revert
 * @param cap_id Capability for modification
 * @param new_version_id Output: ID of new version
 * @return 0 on success, negative error on failure
 */
int gfs_revert_by_node(uint64_t node_id, uint64_t version, const char *commit_message,
                       uint64_t cap_id, uint64_t *new_version_id);

/**
 * Get diff between two versions
 * @param path File path
 * @param version1 First version (0 = current)
 * @param version2 Second version
 * @param mode Diff mode (GFS_DIFF_*)
 * @param result Output diff result (caller must free)
 * @return 0 on success, negative error on failure
 */
int gfs_diff(const char *path, uint64_t version1, uint64_t version2,
             int mode, gfs_diff_result_t *result);

/**
 * Get diff between two nodes
 * @param node1 First node ID
 * @param node2 Second node ID
 * @param mode Diff mode
 * @param result Output diff result
 * @return 0 on success, negative error on failure
 */
int gfs_diff_nodes(uint64_t node1, uint64_t node2, int mode, gfs_diff_result_t *result);

/**
 * Free diff result
 * @param result Diff result to free
 */
void gfs_diff_free(gfs_diff_result_t *result);

/**
 * Get version info for specific version
 * @param path File path
 * @param version Version number (0 = latest)
 * @param info Output version info
 * @return 0 on success, negative error on failure
 */
int gfs_version_info(const char *path, uint64_t version, gfs_version_info_t *info);

/**
 * Get version count for a path
 * @param path File path
 * @return Version count, or negative error
 */
int gfs_version_count(const char *path);

/**
 * Find version by timestamp
 * Returns the version that was current at the given time
 * @param path File path
 * @param timestamp Timestamp to search for
 * @param version Output version number
 * @return 0 on success, negative error on failure
 */
int gfs_version_at_time(const char *path, uint64_t timestamp, uint64_t *version);

/**
 * Compact version history (admin operation)
 * Removes versions older than threshold, keeping every Nth version
 * @param path File path
 * @param keep_every Keep every Nth version
 * @param max_age Maximum age in seconds (older versions removed)
 * @param cap_id Admin capability
 * @param removed_count Output: number of versions removed
 * @return 0 on success, negative error on failure
 */
int gfs_version_compact(const char *path, uint32_t keep_every, uint64_t max_age,
                        uint64_t cap_id, uint32_t *removed_count);

// =============================================================================
// Branch API (Future - marked as not implemented)
// =============================================================================

/**
 * Create a new branch (NOT YET IMPLEMENTED)
 * @param path File path
 * @param branch_name Branch name
 * @param from_version Create from this version (0 = latest)
 * @param cap_id Capability
 * @param branch_id Output: new branch ID
 * @return GFS_ERR_NOT_IMPLEMENTED
 */
int gfs_branch_create(const char *path, const char *branch_name,
                      uint64_t from_version, uint64_t cap_id, uint64_t *branch_id);

/**
 * Merge branch (NOT YET IMPLEMENTED)
 * @param path File path
 * @param source_branch Branch to merge from
 * @param target_branch Branch to merge into
 * @param commit_message Merge commit message
 * @param cap_id Capability
 * @return GFS_ERR_NOT_IMPLEMENTED
 */
int gfs_branch_merge(const char *path, const char *source_branch,
                     const char *target_branch, const char *commit_message,
                     uint64_t cap_id);

/**
 * List branches (NOT YET IMPLEMENTED)
 * @param path File path
 * @param branches Output array
 * @param max_branches Maximum branches to return
 * @return GFS_ERR_NOT_IMPLEMENTED
 */
int gfs_branch_list(const char *path, gfs_branch_t *branches, size_t max_branches);

// =============================================================================
// Error Codes (in addition to node.h errors)
// =============================================================================

#define GFS_ERR_VERSION_NOT_FOUND   -20     // Version number not found
#define GFS_ERR_DIFF_BINARY         -21     // Cannot diff binary files in text mode
#define GFS_ERR_BRANCH_EXISTS       -22     // Branch already exists
#define GFS_ERR_BRANCH_NOT_FOUND    -23     // Branch not found
#define GFS_ERR_MERGE_CONFLICT      -24     // Merge conflict detected
#define GFS_ERR_NOT_IMPLEMENTED     -25     // Feature not yet implemented

// =============================================================================
// Utility Functions
// =============================================================================

/**
 * Compare two content hashes
 * @param hash1 First hash
 * @param hash2 Second hash
 * @return 0 if equal, non-zero if different
 */
int gfs_hash_compare(const uint8_t hash1[GFS_HASH_SIZE],
                     const uint8_t hash2[GFS_HASH_SIZE]);

/**
 * Print version info (for debugging)
 * @param info Version info to print
 */
void gfs_version_print(const gfs_version_info_t *info);

/**
 * Print diff result (for debugging)
 * @param result Diff result to print
 */
void gfs_diff_print(const gfs_diff_result_t *result);

#endif // GRAPHFS_VERSION_H
