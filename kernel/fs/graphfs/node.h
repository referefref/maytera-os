// node.h - Graph Node Structures for GraphFS (Task #39)
// MayteraOS Immutable Versioned Filesystem
//
// Each file/directory in GraphFS is represented as a node in a directed graph.
// Nodes contain:
// - Content hash (SHA-256) pointing to blob store
// - Version chain linking to parent versions
// - Semantic metadata for LLM discovery
// - Edges representing relationships to other nodes
//
// Copyright (c) 2026 MayteraOS Project

#ifndef GRAPHFS_NODE_H
#define GRAPHFS_NODE_H

#include "../../types.h"
#include "../../crypto/crypto.h"

// =============================================================================
// GraphFS Constants
// =============================================================================

// Maximum nodes in the system
#define GFS_MAX_NODES               65536

// Maximum edges per node
#define GFS_MAX_EDGES_PER_NODE      64

// Maximum tags per node
#define GFS_MAX_TAGS_PER_NODE       32

// Maximum tag length
#define GFS_MAX_TAG_LENGTH          64

// Maximum description length
#define GFS_MAX_DESCRIPTION         512

// Maximum path length
#define GFS_MAX_PATH                256

// Maximum commit message length
#define GFS_MAX_COMMIT_MSG          256

// Maximum relation type length
#define GFS_MAX_RELATION_TYPE       32

// Maximum MIME type length
#define GFS_MAX_MIME_TYPE           64

// Hash size (SHA-256)
#define GFS_HASH_SIZE               32

// Embedding dimensions (for vector similarity search)
#define GFS_EMBEDDING_DIMS          128

// Node ID special values
#define GFS_NODE_INVALID            0
#define GFS_NODE_ROOT               1

// Version special values
#define GFS_VERSION_FIRST           1
#define GFS_VERSION_NONE            0

// =============================================================================
// Node Types
// =============================================================================

typedef enum {
    GFS_NODE_TYPE_FILE          = 0,    // Regular file
    GFS_NODE_TYPE_DIRECTORY     = 1,    // Directory
    GFS_NODE_TYPE_SYMLINK       = 2,    // Symbolic link
    GFS_NODE_TYPE_SPECIAL       = 3,    // Special file (device, socket, etc.)
    GFS_NODE_TYPE_MAX           = 4
} gfs_node_type_t;

// =============================================================================
// Node State
// =============================================================================

typedef enum {
    GFS_STATE_INVALID           = 0,    // Not allocated
    GFS_STATE_ACTIVE            = 1,    // Current version, active
    GFS_STATE_HISTORICAL        = 2,    // Old version (read-only)
    GFS_STATE_DELETED           = 3,    // Soft-deleted (recoverable)
    GFS_STATE_PENDING           = 4,    // Being written
} gfs_node_state_t;

// =============================================================================
// Relation Types (predefined common types)
// =============================================================================

// Common relationship types (users can define custom types)
#define GFS_REL_REFERENCES      "references"        // A references B
#define GFS_REL_DERIVED_FROM    "derived_from"      // A was created from B
#define GFS_REL_CONFIG_FOR      "config_for"        // A is config for B
#define GFS_REL_DEPENDENCY      "dependency"        // A depends on B
#define GFS_REL_INCLUDES        "includes"          // A includes/imports B
#define GFS_REL_PARENT          "parent"            // A is parent dir of B
#define GFS_REL_RELATED         "related"           // Generic relation
#define GFS_REL_COPY_OF         "copy_of"           // A is a copy of B
#define GFS_REL_BACKUP_OF       "backup_of"         // A is backup of B
#define GFS_REL_ANNOTATES       "annotates"         // A is annotation of B

// =============================================================================
// Edge Structure (Node Relationship)
// =============================================================================

typedef struct gfs_edge {
    uint64_t edge_id;                           // Unique edge ID
    uint64_t from_node;                         // Source node ID
    uint64_t to_node;                           // Target node ID
    char relation_type[GFS_MAX_RELATION_TYPE];  // Relationship type
    float weight;                               // Relationship strength (0.0-1.0)
    char description[GFS_MAX_DESCRIPTION];      // Optional description
    uint64_t created_at;                        // When edge was created
    uint64_t created_by_pid;                    // Who created it
    bool bidirectional;                         // Can traverse both ways?
} gfs_edge_t;

// =============================================================================
// Vector Embedding (for semantic similarity search)
// =============================================================================

typedef struct gfs_embedding {
    float dims[GFS_EMBEDDING_DIMS];             // Vector dimensions
    uint64_t generated_at;                      // When embedding was computed
    char model[64];                             // Model used to generate
    bool valid;                                 // Is embedding valid?
} gfs_embedding_t;

// =============================================================================
// Tag Structure
// =============================================================================

typedef struct gfs_tag {
    char name[GFS_MAX_TAG_LENGTH];              // Tag name (e.g., "work", "urgent")
    uint64_t added_at;                          // When tag was added
    uint64_t added_by_pid;                      // Who added it
} gfs_tag_t;

// =============================================================================
// Graph Node Structure
// =============================================================================

typedef struct gfs_node {
    // === Identity (16 bytes) ===
    uint64_t node_id;                           // Unique node ID
    gfs_node_type_t node_type;                  // File, directory, etc.
    gfs_node_state_t state;                     // Current state

    // === Content Reference (40 bytes) ===
    uint8_t content_hash[GFS_HASH_SIZE];        // SHA-256 of content (blob store key)
    uint64_t size;                              // Content size in bytes

    // === Path (256 bytes) ===
    char path[GFS_MAX_PATH];                    // Virtual path (POSIX-style)

    // === Timestamps (32 bytes) ===
    uint64_t created_at;                        // When this node was created
    uint64_t modified_at;                       // When this VERSION was created
    uint64_t accessed_at;                       // Last access time
    uint64_t content_modified_at;               // When content last changed

    // === Version Chain (32 bytes) ===
    uint64_t version_number;                    // Version number (1 = first)
    uint64_t parent_version;                    // Node ID of previous version (0 = first)
    uint64_t latest_version;                    // Node ID of newest version (self if latest)
    uint64_t first_version;                     // Node ID of original version

    // === Commit Info (280 bytes) ===
    char commit_message[GFS_MAX_COMMIT_MSG];    // Why this change was made
    uint64_t committer_pid;                     // Process that made this version
    uint64_t committer_cap_id;                  // Capability used for commit

    // === Semantic Metadata (for LLM discovery) ===
    char description[GFS_MAX_DESCRIPTION];      // Natural language description
    char mime_type[GFS_MAX_MIME_TYPE];          // MIME type (e.g., "text/plain")
    gfs_tag_t tags[GFS_MAX_TAGS_PER_NODE];      // Searchable tags
    uint32_t tag_count;                         // Number of tags

    // === Edges/Relationships ===
    gfs_edge_t *edges;                          // Dynamically allocated edges
    uint32_t edge_count;                        // Number of edges
    uint32_t edge_capacity;                     // Allocated edge slots

    // === Vector Embedding (optional) ===
    gfs_embedding_t *embedding;                 // NULL if not computed

    // === Permissions (8 bytes) ===
    uint32_t owner_uid;                         // Owner user ID
    uint32_t permissions;                       // Unix-style permissions (rwxrwxrwx)

    // === Internal bookkeeping ===
    volatile uint32_t refcount;                 // Reference count
    volatile uint32_t lock;                     // Spinlock for thread safety

} gfs_node_t;

// =============================================================================
// Node Creation Options
// =============================================================================

typedef struct gfs_node_create_opts {
    gfs_node_type_t type;                       // Node type
    const char *path;                           // Virtual path
    const void *content;                        // Initial content (NULL for dirs)
    size_t content_size;                        // Content size
    const char *description;                    // Optional description
    const char *mime_type;                      // Optional MIME type
    const char **tags;                          // Optional tags
    uint32_t tag_count;                         // Number of tags
    const char *commit_message;                 // Commit message
    uint64_t cap_id;                            // Capability for creation
} gfs_node_create_opts_t;

// =============================================================================
// Node Update Options
// =============================================================================

typedef struct gfs_node_update_opts {
    const void *new_content;                    // New content (NULL = no change)
    size_t new_content_size;                    // New content size
    const char *description;                    // New description (NULL = no change)
    const char *commit_message;                 // Required commit message
    uint64_t cap_id;                            // Capability for update
} gfs_node_update_opts_t;

// =============================================================================
// Error Codes
// =============================================================================

#define GFS_SUCCESS             0
#define GFS_ERR_INVALID_NODE    -1      // Invalid node ID
#define GFS_ERR_NOT_FOUND       -2      // Node/path not found
#define GFS_ERR_EXISTS          -3      // Path already exists
#define GFS_ERR_NO_MEMORY       -4      // Out of memory
#define GFS_ERR_NO_SPACE        -5      // No space in blob store
#define GFS_ERR_PERMISSION      -6      // Permission denied
#define GFS_ERR_INVALID_PATH    -7      // Invalid path format
#define GFS_ERR_NOT_DIR         -8      // Expected directory
#define GFS_ERR_IS_DIR          -9      // Expected file, got directory
#define GFS_ERR_NOT_EMPTY       -10     // Directory not empty
#define GFS_ERR_TOO_MANY_NODES  -11     // Max nodes reached
#define GFS_ERR_TOO_MANY_EDGES  -12     // Max edges per node reached
#define GFS_ERR_INVALID_VERSION -13     // Invalid version number
#define GFS_ERR_READONLY        -14     // Historical node is read-only
#define GFS_ERR_NO_CAPABILITY   -15     // Missing required capability
#define GFS_ERR_INVALID_ARG     -16     // Invalid argument
#define GFS_ERR_BLOB_STORE      -17     // Blob store error
#define GFS_ERR_HASH_MISMATCH   -18     // Content hash mismatch
#define GFS_ERR_NOT_INIT        -19     // GraphFS not initialized

// =============================================================================
// Node API
// =============================================================================

/**
 * Initialize the GraphFS node subsystem
 * Creates root node and prepares data structures
 */
void gfs_node_init(void);

/**
 * Create a new node
 * @param opts Creation options
 * @param node_id Output: new node ID
 * @return 0 on success, negative error on failure
 */
int gfs_node_create(const gfs_node_create_opts_t *opts, uint64_t *node_id);

/**
 * Get node by ID
 * Increments reference count (must call gfs_node_release)
 * @param node_id Node ID
 * @return Pointer to node, or NULL if not found
 */
gfs_node_t *gfs_node_get(uint64_t node_id);

/**
 * Get node by path (latest version)
 * Increments reference count
 * @param path Virtual path
 * @return Pointer to node, or NULL if not found
 */
gfs_node_t *gfs_node_get_by_path(const char *path);

/**
 * Release reference to node
 * @param node Node to release
 */
void gfs_node_release(gfs_node_t *node);

/**
 * Update node content (creates new version)
 * @param node_id Node to update
 * @param opts Update options
 * @param new_version_id Output: ID of new version
 * @return 0 on success, negative error on failure
 */
int gfs_node_update(uint64_t node_id, const gfs_node_update_opts_t *opts,
                    uint64_t *new_version_id);

/**
 * Delete node (soft delete - marks as deleted, recoverable)
 * @param node_id Node to delete
 * @param cap_id Capability for deletion
 * @return 0 on success, negative error on failure
 */
int gfs_node_delete(uint64_t node_id, uint64_t cap_id);

/**
 * Permanently purge deleted node (requires admin capability)
 * @param node_id Node to purge
 * @param cap_id Admin capability
 * @return 0 on success, negative error on failure
 */
int gfs_node_purge(uint64_t node_id, uint64_t cap_id);

/**
 * Restore a deleted node
 * @param node_id Node to restore
 * @param cap_id Capability for restoration
 * @return 0 on success, negative error on failure
 */
int gfs_node_restore(uint64_t node_id, uint64_t cap_id);

// =============================================================================
// Edge/Relationship API
// =============================================================================

/**
 * Create edge between nodes
 * @param from_node Source node
 * @param to_node Target node
 * @param relation_type Relationship type
 * @param weight Relationship strength (0.0-1.0)
 * @param description Optional description
 * @param bidirectional Allow traversal both ways?
 * @param edge_id Output: new edge ID
 * @return 0 on success, negative error on failure
 */
int gfs_edge_create(uint64_t from_node, uint64_t to_node,
                    const char *relation_type, float weight,
                    const char *description, bool bidirectional,
                    uint64_t *edge_id);

/**
 * Remove edge
 * @param edge_id Edge to remove
 * @return 0 on success, negative error on failure
 */
int gfs_edge_remove(uint64_t edge_id);

/**
 * Get edges from a node
 * @param node_id Source node
 * @param relation_type Filter by type (NULL = all)
 * @param edges Output array
 * @param max_edges Maximum edges to return
 * @return Number of edges found, or negative error
 */
int gfs_edges_get_outgoing(uint64_t node_id, const char *relation_type,
                           gfs_edge_t *edges, size_t max_edges);

/**
 * Get edges to a node (reverse lookup)
 * @param node_id Target node
 * @param relation_type Filter by type (NULL = all)
 * @param edges Output array
 * @param max_edges Maximum edges to return
 * @return Number of edges found, or negative error
 */
int gfs_edges_get_incoming(uint64_t node_id, const char *relation_type,
                           gfs_edge_t *edges, size_t max_edges);

// =============================================================================
// Tag API
// =============================================================================

/**
 * Add tag to node
 * @param node_id Node to tag
 * @param tag Tag name
 * @return 0 on success, negative error on failure
 */
int gfs_tag_add(uint64_t node_id, const char *tag);

/**
 * Remove tag from node
 * @param node_id Node
 * @param tag Tag name
 * @return 0 on success, negative error on failure
 */
int gfs_tag_remove(uint64_t node_id, const char *tag);

/**
 * Check if node has tag
 * @param node_id Node
 * @param tag Tag name
 * @return 1 if has tag, 0 if not, negative error
 */
int gfs_tag_has(uint64_t node_id, const char *tag);

// =============================================================================
// Metadata API
// =============================================================================

/**
 * Set node description
 * @param node_id Node
 * @param description New description
 * @return 0 on success, negative error on failure
 */
int gfs_node_set_description(uint64_t node_id, const char *description);

/**
 * Set node MIME type
 * @param node_id Node
 * @param mime_type New MIME type
 * @return 0 on success, negative error on failure
 */
int gfs_node_set_mime_type(uint64_t node_id, const char *mime_type);

/**
 * Set node embedding (for semantic search)
 * @param node_id Node
 * @param embedding Embedding vector
 * @return 0 on success, negative error on failure
 */
int gfs_node_set_embedding(uint64_t node_id, const gfs_embedding_t *embedding);

// =============================================================================
// Utility Functions
// =============================================================================

/**
 * Compute content hash
 * @param content Content data
 * @param size Content size
 * @param hash Output hash (32 bytes)
 */
void gfs_compute_hash(const void *content, size_t size, uint8_t hash[GFS_HASH_SIZE]);

/**
 * Convert hash to hex string
 * @param hash Hash bytes
 * @param hex Output string (65 bytes including null)
 */
void gfs_hash_to_hex(const uint8_t hash[GFS_HASH_SIZE], char hex[65]);

/**
 * Parse path into components
 * @param path Path to parse
 * @param components Output array of component pointers
 * @param max_components Maximum components
 * @return Number of components, or negative error
 */
int gfs_path_parse(const char *path, const char **components, size_t max_components);

/**
 * Validate path format
 * @param path Path to validate
 * @return 0 if valid, negative error if invalid
 */
int gfs_path_validate(const char *path);

/**
 * Get parent path
 * @param path Input path
 * @param parent Output buffer for parent path
 * @param parent_size Buffer size
 * @return 0 on success, negative error on failure
 */
int gfs_path_parent(const char *path, char *parent, size_t parent_size);

/**
 * Get filename from path
 * @param path Input path
 * @return Pointer to filename part
 */
const char *gfs_path_filename(const char *path);

/**
 * Lock node for exclusive access
 * @param node Node to lock
 */
void gfs_node_lock(gfs_node_t *node);

/**
 * Unlock node
 * @param node Node to unlock
 */
void gfs_node_unlock(gfs_node_t *node);

// =============================================================================
// Statistics
// =============================================================================

typedef struct gfs_stats {
    uint64_t total_nodes;           // Total nodes allocated
    uint64_t active_nodes;          // Active (non-deleted) nodes
    uint64_t historical_nodes;      // Historical versions
    uint64_t deleted_nodes;         // Soft-deleted nodes
    uint64_t total_edges;           // Total edges
    uint64_t total_tags;            // Total tags across all nodes
    uint64_t total_content_bytes;   // Total content size
    uint64_t unique_content_bytes;  // Unique content (deduplicated)
} gfs_stats_t;

/**
 * Get GraphFS statistics
 * @param stats Output statistics
 */
void gfs_node_get_stats(gfs_stats_t *stats);

/**
 * Print node information (for debugging)
 * @param node_id Node to print
 */
void gfs_node_print(uint64_t node_id);

#endif // GRAPHFS_NODE_H
