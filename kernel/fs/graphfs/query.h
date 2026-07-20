// query.h - Semantic Query and LLM Interface for GraphFS (Task #40)
// MayteraOS Immutable Versioned Filesystem
//
// This module provides:
// - Graph query language for structured queries
// - Tag and description search for semantic discovery
// - Relationship traversal for connected data
// - LLM-friendly natural language path resolution
// - Future: vector embeddings for similarity search
//
// Copyright (c) 2026 MayteraOS Project

#ifndef GRAPHFS_QUERY_H
#define GRAPHFS_QUERY_H

#include "../../types.h"
#include "node.h"
#include "version.h"

// =============================================================================
// Query Constants
// =============================================================================

// Maximum query results
#define GFS_MAX_QUERY_RESULTS       1024

// Maximum query depth for traversals
#define GFS_MAX_QUERY_DEPTH         16

// Maximum query string length
#define GFS_MAX_QUERY_STRING        1024

// Maximum filters in a query
#define GFS_MAX_QUERY_FILTERS       16

// Maximum tags in a tag query
#define GFS_MAX_QUERY_TAGS          32

// =============================================================================
// Query Filter Types
// =============================================================================

typedef enum {
    GFS_FILTER_NONE         = 0,
    GFS_FILTER_TYPE         = 1,    // Filter by node type
    GFS_FILTER_SIZE_MIN     = 2,    // Minimum size
    GFS_FILTER_SIZE_MAX     = 3,    // Maximum size
    GFS_FILTER_CREATED_AFTER = 4,   // Created after timestamp
    GFS_FILTER_CREATED_BEFORE = 5,  // Created before timestamp
    GFS_FILTER_MODIFIED_AFTER = 6,  // Modified after timestamp
    GFS_FILTER_MODIFIED_BEFORE = 7, // Modified before timestamp
    GFS_FILTER_MIME_TYPE    = 8,    // MIME type pattern
    GFS_FILTER_PATH_PATTERN = 9,    // Path glob pattern
    GFS_FILTER_HAS_TAG      = 10,   // Must have specific tag
    GFS_FILTER_HAS_RELATION = 11,   // Must have relation of type
    GFS_FILTER_OWNER        = 12,   // Owner UID
    GFS_FILTER_STATE        = 13,   // Node state
    GFS_FILTER_MAX          = 14
} gfs_filter_type_t;

// =============================================================================
// Query Filter Structure
// =============================================================================

typedef struct gfs_filter {
    gfs_filter_type_t type;         // Filter type
    union {
        gfs_node_type_t node_type;  // For FILTER_TYPE
        uint64_t size;              // For FILTER_SIZE_*
        uint64_t timestamp;         // For FILTER_*_AFTER/BEFORE
        uint32_t uid;               // For FILTER_OWNER
        gfs_node_state_t state;     // For FILTER_STATE
        char pattern[128];          // For string patterns
    } value;
    bool negate;                    // Negate this filter?
} gfs_filter_t;

// =============================================================================
// Query Sort Options
// =============================================================================

typedef enum {
    GFS_SORT_NONE           = 0,
    GFS_SORT_PATH_ASC       = 1,
    GFS_SORT_PATH_DESC      = 2,
    GFS_SORT_SIZE_ASC       = 3,
    GFS_SORT_SIZE_DESC      = 4,
    GFS_SORT_CREATED_ASC    = 5,
    GFS_SORT_CREATED_DESC   = 6,
    GFS_SORT_MODIFIED_ASC   = 7,
    GFS_SORT_MODIFIED_DESC  = 8,
    GFS_SORT_RELEVANCE      = 9,    // For search queries
    GFS_SORT_MAX            = 10
} gfs_sort_t;

// =============================================================================
// Query Structure
// =============================================================================

typedef struct gfs_query {
    // Filters
    gfs_filter_t filters[GFS_MAX_QUERY_FILTERS];
    uint32_t filter_count;

    // Starting point (optional)
    uint64_t start_node;            // 0 = search all
    char start_path[GFS_MAX_PATH];  // Alternative: start from path

    // Traversal options (for relationship queries)
    char relation_types[8][GFS_MAX_RELATION_TYPE];  // Filter by relation types
    uint32_t relation_type_count;
    uint32_t max_depth;             // Max traversal depth (0 = no traversal)
    bool include_start;             // Include starting node in results?
    bool bidirectional;             // Traverse edges both ways?

    // Text search (optional)
    char search_text[256];          // Search in descriptions
    bool search_paths;              // Also search in paths?
    bool search_tags;               // Also search in tags?
    bool search_commits;            // Also search in commit messages?

    // Tag filter (optional)
    char tags[GFS_MAX_QUERY_TAGS][GFS_MAX_TAG_LENGTH];
    uint32_t tag_count;
    bool tags_match_all;            // true = AND, false = OR

    // Pagination
    uint32_t offset;                // Skip first N results
    uint32_t limit;                 // Maximum results (0 = GFS_MAX_QUERY_RESULTS)

    // Sorting
    gfs_sort_t sort;                // Sort order

    // Capability for access control
    uint64_t cap_id;                // Capability (filters results by permission)
} gfs_query_t;

// =============================================================================
// Query Result Structure
// =============================================================================

typedef struct gfs_query_result_item {
    uint64_t node_id;               // Node ID
    char path[GFS_MAX_PATH];        // Node path
    gfs_node_type_t node_type;      // Node type
    uint64_t size;                  // Content size
    uint64_t version;               // Current version number
    uint64_t modified_at;           // Last modified
    char description[128];          // Truncated description
    float relevance;                // Relevance score (0.0-1.0)
    uint32_t depth;                 // Traversal depth (0 = start node)
    char via_relation[GFS_MAX_RELATION_TYPE];  // How we got here
} gfs_query_result_item_t;

typedef struct gfs_query_result {
    gfs_query_result_item_t *items; // Result items (dynamically allocated)
    uint32_t count;                 // Number of results
    uint32_t capacity;              // Allocated capacity
    uint32_t total_count;           // Total matches (before pagination)
    bool truncated;                 // Were results truncated?
    uint64_t execution_time_us;     // Query execution time in microseconds
} gfs_query_result_t;

// =============================================================================
// Related Nodes Query (simplified traversal)
// =============================================================================

typedef struct gfs_related_opts {
    const char *relation_type;      // Filter by relation type (NULL = all)
    uint32_t depth;                 // Traversal depth (1 = direct relations)
    bool bidirectional;             // Traverse both directions?
    bool include_self;              // Include the starting node?
    uint32_t limit;                 // Maximum results
    uint64_t cap_id;                // Capability for access control
} gfs_related_opts_t;

// =============================================================================
// Semantic Search Options
// =============================================================================

typedef struct gfs_search_opts {
    bool search_descriptions;       // Search in node descriptions
    bool search_paths;              // Search in paths
    bool search_tags;               // Search in tags
    bool search_commits;            // Search in commit messages
    bool search_content;            // Search in file content (expensive!)
    bool case_sensitive;            // Case-sensitive search
    bool whole_word;                // Match whole words only
    bool regex;                     // Treat as regex pattern
    uint32_t limit;                 // Maximum results
    uint64_t cap_id;                // Capability for access control
} gfs_search_opts_t;

// =============================================================================
// LLM Interface Structures
// =============================================================================

// Natural language path resolution request
typedef struct gfs_nlp_request {
    char query[512];                // Natural language query
    char context[1024];             // Optional context about what user is doing
    uint64_t cwd_node;              // Current working directory node (hint)
    char model_id[64];              // LLM model making the request
    uint64_t cap_id;                // Capability for access control
} gfs_nlp_request_t;

// Natural language path resolution response
typedef struct gfs_nlp_response {
    uint64_t node_id;               // Resolved node (0 if not found)
    char path[GFS_MAX_PATH];        // Resolved path
    float confidence;               // Confidence (0.0-1.0)
    char explanation[256];          // Why this path was chosen
    char alternatives[4][GFS_MAX_PATH];  // Alternative matches
    uint32_t alternative_count;     // Number of alternatives
} gfs_nlp_response_t;

// Relationship explanation request
typedef struct gfs_explain_request {
    uint64_t from_node;             // Source node
    uint64_t to_node;               // Target node
    uint32_t max_paths;             // Maximum paths to find
    uint64_t cap_id;                // Capability for access control
} gfs_explain_request_t;

// Relationship explanation response
typedef struct gfs_explain_path {
    uint64_t nodes[16];             // Nodes in the path
    char relations[16][GFS_MAX_RELATION_TYPE];  // Relations between nodes
    uint32_t length;                // Path length
} gfs_explain_path_t;

typedef struct gfs_explain_response {
    gfs_explain_path_t paths[8];    // Paths found
    uint32_t path_count;            // Number of paths
    bool connected;                 // Are the nodes connected at all?
    char summary[512];              // Natural language summary
} gfs_explain_response_t;

// =============================================================================
// Query API
// =============================================================================

/**
 * Initialize query subsystem
 */
void gfs_query_init(void);

/**
 * Execute a query
 * @param query Query parameters
 * @param result Output result (caller must free)
 * @return 0 on success, negative error on failure
 */
int gfs_query(const gfs_query_t *query, gfs_query_result_t *result);

/**
 * Free query result
 * @param result Result to free
 */
void gfs_query_free(gfs_query_result_t *result);

/**
 * Create empty query with defaults
 * @param query Query to initialize
 */
void gfs_query_init_default(gfs_query_t *query);

/**
 * Add filter to query
 * @param query Query to modify
 * @param filter Filter to add
 * @return 0 on success, -1 if too many filters
 */
int gfs_query_add_filter(gfs_query_t *query, const gfs_filter_t *filter);

// =============================================================================
// Graph Traversal API
// =============================================================================

/**
 * Get related nodes by traversing relationships
 * @param node_id Starting node
 * @param opts Traversal options
 * @param result Output result (caller must free)
 * @return 0 on success, negative error on failure
 */
int gfs_get_related(uint64_t node_id, const gfs_related_opts_t *opts,
                    gfs_query_result_t *result);

/**
 * Get related nodes by path
 * @param path Starting path
 * @param relation_type Relation type filter (NULL = all)
 * @param depth Traversal depth
 * @param result Output result
 * @return 0 on success, negative error on failure
 */
int gfs_get_related_by_path(const char *path, const char *relation_type,
                            uint32_t depth, gfs_query_result_t *result);

// =============================================================================
// Tag Search API
// =============================================================================

/**
 * Search nodes by tags
 * @param tags Array of tag names
 * @param tag_count Number of tags
 * @param match_all true = must have ALL tags, false = must have ANY tag
 * @param result Output result
 * @return 0 on success, negative error on failure
 */
int gfs_search_tags(const char **tags, size_t tag_count, bool match_all,
                    gfs_query_result_t *result);

/**
 * Get all unique tags in the system
 * @param tags Output array of tag names
 * @param max_tags Maximum tags to return
 * @return Number of tags found, or negative error
 */
int gfs_tags_list(char (*tags)[GFS_MAX_TAG_LENGTH], size_t max_tags);

/**
 * Get nodes with a specific tag
 * @param tag Tag name
 * @param result Output result
 * @return 0 on success, negative error on failure
 */
int gfs_search_tag(const char *tag, gfs_query_result_t *result);

// =============================================================================
// Text Search API
// =============================================================================

/**
 * Search in node descriptions
 * @param text Search text
 * @param opts Search options
 * @param result Output result
 * @return 0 on success, negative error on failure
 */
int gfs_search_description(const char *text, const gfs_search_opts_t *opts,
                           gfs_query_result_t *result);

/**
 * Search across all text fields
 * @param text Search text
 * @param opts Search options
 * @param result Output result
 * @return 0 on success, negative error on failure
 */
int gfs_search(const char *text, const gfs_search_opts_t *opts,
               gfs_query_result_t *result);

/**
 * Full-text search in file content (expensive!)
 * Only searches files accessible via capability
 * @param text Search text
 * @param path_pattern Optional path pattern to limit search
 * @param cap_id Capability for access control
 * @param result Output result
 * @return 0 on success, negative error on failure
 */
int gfs_search_content(const char *text, const char *path_pattern,
                       uint64_t cap_id, gfs_query_result_t *result);

// =============================================================================
// LLM Interface API
// =============================================================================

/**
 * Resolve natural language path query
 * Uses fuzzy matching, synonyms, and context
 * @param request NLP request
 * @param response Output response
 * @return 0 on success, negative error on failure
 */
int gfs_nlp_resolve(const gfs_nlp_request_t *request, gfs_nlp_response_t *response);

/**
 * Explain relationship between two nodes
 * Finds paths connecting them and generates explanation
 * @param request Explain request
 * @param response Output response
 * @return 0 on success, negative error on failure
 */
int gfs_explain_relationship(const gfs_explain_request_t *request,
                             gfs_explain_response_t *response);

/**
 * Suggest related files based on current context
 * @param current_node Node being worked on
 * @param action What the user is doing (e.g., "editing", "reading")
 * @param suggestions Output array of suggested node IDs
 * @param max_suggestions Maximum suggestions
 * @param cap_id Capability for access control
 * @return Number of suggestions, or negative error
 */
int gfs_suggest_related(uint64_t current_node, const char *action,
                        uint64_t *suggestions, size_t max_suggestions,
                        uint64_t cap_id);

/**
 * Get file summary for LLM context
 * Returns a condensed representation suitable for LLM prompts
 * @param node_id Node to summarize
 * @param summary Output buffer
 * @param summary_size Buffer size
 * @param cap_id Capability for access control
 * @return 0 on success, negative error on failure
 */
int gfs_get_summary(uint64_t node_id, char *summary, size_t summary_size,
                    uint64_t cap_id);

// =============================================================================
// Vector Similarity Search (Future - not fully implemented)
// =============================================================================

/**
 * Search by vector similarity (requires embeddings)
 * @param query_embedding Query vector
 * @param threshold Minimum similarity (0.0-1.0)
 * @param result Output result
 * @return 0 on success, negative error on failure
 */
int gfs_search_similar(const gfs_embedding_t *query_embedding, float threshold,
                       gfs_query_result_t *result);

/**
 * Find similar files to a given file
 * @param node_id Reference node
 * @param threshold Minimum similarity
 * @param result Output result
 * @return 0 on success, negative error on failure
 */
int gfs_find_similar(uint64_t node_id, float threshold, gfs_query_result_t *result);

// =============================================================================
// Query Statistics
// =============================================================================

typedef struct gfs_query_stats {
    uint64_t total_queries;         // Total queries executed
    uint64_t cache_hits;            // Queries served from cache
    uint64_t cache_misses;          // Queries requiring full scan
    uint64_t avg_execution_us;      // Average execution time
    uint64_t max_execution_us;      // Maximum execution time
    uint64_t total_results;         // Total results returned
    uint64_t nlp_queries;           // Natural language queries
} gfs_query_stats_t;

/**
 * Get query statistics
 * @param stats Output statistics
 */
void gfs_query_get_stats(gfs_query_stats_t *stats);

/**
 * Reset query statistics
 */
void gfs_query_reset_stats(void);

// =============================================================================
// Error Codes (in addition to node.h and version.h errors)
// =============================================================================

#define GFS_ERR_QUERY_SYNTAX        -30     // Invalid query syntax
#define GFS_ERR_QUERY_TIMEOUT       -31     // Query took too long
#define GFS_ERR_TOO_MANY_FILTERS    -32     // Too many query filters
#define GFS_ERR_INVALID_FILTER      -33     // Invalid filter type/value
#define GFS_ERR_NO_EMBEDDINGS       -34     // Node has no embeddings
#define GFS_ERR_NLP_FAILED          -35     // Natural language parsing failed

// =============================================================================
// Debug Functions
// =============================================================================

/**
 * Print query (for debugging)
 * @param query Query to print
 */
void gfs_query_print(const gfs_query_t *query);

/**
 * Print query result (for debugging)
 * @param result Result to print
 */
void gfs_query_result_print(const gfs_query_result_t *result);

#endif // GRAPHFS_QUERY_H
