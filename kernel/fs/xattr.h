// xattr.h - Extended File Attributes for MayteraOS
// Provides POSIX-like extended attributes with FAT filesystem storage
#ifndef XATTR_H
#define XATTR_H

#include "../types.h"
#include "fat.h"

// ============================================================================
// Extended Attribute Limits
// ============================================================================

#define XATTR_NAME_MAX          255     // Maximum attribute name length
#define XATTR_VALUE_MAX         65536   // Maximum attribute value size (64KB)
#define XATTR_LIST_MAX          65536   // Maximum total size of attribute names list
#define XATTR_PATH_MAX          512     // Maximum path length for xattr storage

// ============================================================================
// Attribute Namespaces
// ============================================================================
// Namespaces provide access control and organization for extended attributes.
// Format: "namespace.attribute_name"

#define XATTR_NS_USER           "user"      // User-defined attributes (accessible by owner)
#define XATTR_NS_SYSTEM         "system"    // System-defined attributes (kernel use)
#define XATTR_NS_SECURITY       "security"  // Security labels (capability system)
#define XATTR_NS_TRUSTED        "trusted"   // Trusted attributes (root/system only)

// Namespace prefix lengths (including the dot)
#define XATTR_NS_USER_PREFIX    "user."
#define XATTR_NS_SYSTEM_PREFIX  "system."
#define XATTR_NS_SECURITY_PREFIX "security."
#define XATTR_NS_TRUSTED_PREFIX "trusted."

#define XATTR_NS_USER_PREFIX_LEN    5
#define XATTR_NS_SYSTEM_PREFIX_LEN  7
#define XATTR_NS_SECURITY_PREFIX_LEN 9
#define XATTR_NS_TRUSTED_PREFIX_LEN 8

// Namespace identifiers
typedef enum {
    XATTR_NAMESPACE_USER = 0,
    XATTR_NAMESPACE_SYSTEM,
    XATTR_NAMESPACE_SECURITY,
    XATTR_NAMESPACE_TRUSTED,
    XATTR_NAMESPACE_INVALID
} xattr_namespace_t;

// ============================================================================
// Standard MayteraOS Extended Attributes
// ============================================================================
// These are well-known attributes used by the system and applications.

// User namespace attributes
#define XATTR_USER_MIME_TYPE    "user.mime_type"     // MIME type override (e.g., "image/png")
#define XATTR_USER_DESCRIPTION  "user.description"   // Human-readable file description
#define XATTR_USER_TAGS         "user.tags"          // Comma-separated tags for search
#define XATTR_USER_THUMBNAIL    "user.thumbnail"     // Path to cached thumbnail image
#define XATTR_USER_OPEN_WITH    "user.open_with"     // Preferred application path
#define XATTR_USER_AUTHOR       "user.author"        // Author/creator name
#define XATTR_USER_COMMENTS     "user.comments"      // User comments about file
#define XATTR_USER_RATING       "user.rating"        // File rating (1-5)
#define XATTR_USER_COLOR        "user.color"         // Label color (e.g., "red", "blue")
#define XATTR_USER_CHECKSUM     "user.checksum"      // MD5/SHA checksum for integrity

// System namespace attributes
#define XATTR_SYSTEM_CAPABILITY "system.capability"  // Required capabilities to access file
#define XATTR_SYSTEM_BACKUP     "system.backup"      // Backup timestamp (Unix time)
#define XATTR_SYSTEM_INDEXED    "system.indexed"     // Whether file is search-indexed
#define XATTR_SYSTEM_POSIX_ACL  "system.posix_acl"   // POSIX ACL data (future)

// Security namespace attributes
#define XATTR_SECURITY_LABEL    "security.label"     // Security label for capability system
#define XATTR_SECURITY_SELINUX  "security.selinux"   // SELinux-style label (future)
#define XATTR_SECURITY_SANDBOX  "security.sandbox"   // Sandbox restrictions

// Trusted namespace attributes (requires elevated privileges)
#define XATTR_TRUSTED_ORIGIN    "trusted.origin"     // Origin URL for downloaded files
#define XATTR_TRUSTED_VERIFIED  "trusted.verified"   // Digital signature verification status

// ============================================================================
// Error Codes
// ============================================================================

#define XATTR_SUCCESS           0       // Operation successful
#define XATTR_ENOATTR           (-1)    // Attribute not found
#define XATTR_EEXIST            (-2)    // Attribute already exists (when XATTR_CREATE)
#define XATTR_ENOENT            (-3)    // File not found
#define XATTR_ENOMEM            (-4)    // Out of memory
#define XATTR_ERANGE            (-5)    // Buffer too small
#define XATTR_EINVAL            (-6)    // Invalid argument
#define XATTR_ENOSPC            (-7)    // No space left on device
#define XATTR_EACCES            (-8)    // Permission denied
#define XATTR_ENOTDIR           (-9)    // Not a directory (for storage)
#define XATTR_EIO               (-10)   // I/O error

// ============================================================================
// Flags for xattr_set()
// ============================================================================

#define XATTR_CREATE            0x01    // Create only, fail if exists
#define XATTR_REPLACE           0x02    // Replace only, fail if not exists
#define XATTR_NOFOLLOW          0x04    // Don't follow symlinks (future)

// ============================================================================
// In-Memory Xattr Cache
// ============================================================================
// Cache recently accessed xattrs to reduce disk I/O

#define XATTR_CACHE_SIZE        64      // Number of cached entries
#define XATTR_CACHE_VALUE_MAX   256     // Max value size to cache (larger values not cached)

// Cache entry flags
#define XATTR_CACHE_VALID       0x01    // Entry is valid
#define XATTR_CACHE_DIRTY       0x02    // Entry needs to be written to disk
#define XATTR_CACHE_NEGATIVE    0x04    // Negative cache (attribute confirmed not to exist)

typedef struct {
    char path[XATTR_PATH_MAX];          // File path
    char name[XATTR_NAME_MAX];          // Attribute name
    uint8_t value[XATTR_CACHE_VALUE_MAX]; // Attribute value
    uint32_t value_len;                 // Value length
    uint32_t flags;                     // Cache flags
    uint64_t access_time;               // Last access time (for LRU eviction)
} xattr_cache_entry_t;

typedef struct {
    xattr_cache_entry_t entries[XATTR_CACHE_SIZE];
    uint32_t hit_count;                 // Cache hits
    uint32_t miss_count;                // Cache misses
    uint64_t clock;                     // Logical clock for LRU
} xattr_cache_t;

// ============================================================================
// Xattr Storage Structure (on disk)
// ============================================================================
// For FAT filesystem, xattrs are stored in /.xattr/ directory.
// Each file's attributes are stored in a file named by its inode/cluster hash.

// Magic number for xattr files
#define XATTR_FILE_MAGIC        0x58415452  // "XATR"

// Xattr file header (stored at start of each .xattr file)
typedef struct {
    uint32_t magic;             // XATTR_FILE_MAGIC
    uint32_t version;           // Format version (currently 1)
    uint32_t attr_count;        // Number of attributes
    uint32_t total_size;        // Total size of all attribute data
    uint32_t checksum;          // Simple checksum for integrity
    uint8_t reserved[12];       // Reserved for future use
} __attribute__((packed)) xattr_file_header_t;

// Individual attribute entry (follows header)
typedef struct {
    uint16_t name_len;          // Length of attribute name
    uint32_t value_len;         // Length of attribute value
    uint8_t namespace_id;       // Namespace identifier
    uint8_t reserved;           // Padding
    // Followed by: name (name_len bytes), value (value_len bytes)
} __attribute__((packed)) xattr_entry_header_t;

// ============================================================================
// #404 batch-2 / MAYTERA-SEC-2026-0011: on-disk xattr entry-walk seam.
// Parsed-entry descriptor crossing the C<->Rust FFI. Offsets are RELATIVE to the
// entries region (file_data + sizeof(xattr_file_header_t)); the caller computes
// attr_name = entries + name_off, attr_value = entries + value_off. #[repr(C)],
// sizeof LOCKED == 16 (asserted in fs/xattr.c).
// ============================================================================
typedef struct {
    uint32_t name_off;      // offset of name within the entries region
    uint32_t value_off;     // offset of value within the entries region
    uint32_t value_len;     // value length (bytes)
    uint16_t name_len;      // name length (bytes, incl NUL on well-formed)
    uint8_t  namespace_id;  // namespace identifier
    uint8_t  _pad;          // padding, always 0
} xattr_entry_t;

// Returns 1 = one entry decoded (*pos advanced past it, *out filled),
//         0 = clean end (*pos >= len, nothing left),
//        -1 = malformed / would-be-OOB (Rust confines; the C reference never
//             returns this - it trusts the on-disk lengths, which is the bug).
int xattr_entry_next_c (const uint8_t *block, uint32_t len, uint32_t *pos, xattr_entry_t *out);
int xattr_entry_next_rs(const uint8_t *block, uint32_t len, uint32_t *pos, xattr_entry_t *out);

// ============================================================================
// Xattr List Entry (for listxattr return)
// ============================================================================

typedef struct xattr_list_entry {
    char *name;                         // Attribute name (with namespace prefix)
    struct xattr_list_entry *next;      // Next entry in list
} xattr_list_entry_t;

// ============================================================================
// API Functions
// ============================================================================

/**
 * Initialize the extended attributes subsystem
 * Creates the /.xattr/ directory if it doesn't exist
 */
void xattr_init(void);

/**
 * Get an extended attribute value
 * @param path      Path to the file
 * @param name      Attribute name (with namespace prefix)
 * @param value     Buffer to store the value
 * @param size      Size of the buffer (or 0 to query size)
 * @return          Value size on success, negative error code on failure
 *                  If size is 0, returns the required buffer size
 */
ssize_t xattr_get(const char *path, const char *name, void *value, size_t size);

/**
 * Set an extended attribute value
 * @param path      Path to the file
 * @param name      Attribute name (with namespace prefix)
 * @param value     Attribute value
 * @param size      Size of the value
 * @param flags     XATTR_CREATE, XATTR_REPLACE, or 0
 * @return          0 on success, negative error code on failure
 */
int xattr_set(const char *path, const char *name, const void *value, size_t size, int flags);

/**
 * Remove an extended attribute
 * @param path      Path to the file
 * @param name      Attribute name (with namespace prefix)
 * @return          0 on success, negative error code on failure
 */
int xattr_remove(const char *path, const char *name);

/**
 * List all extended attributes for a file
 * @param path      Path to the file
 * @param list      Buffer to store null-separated attribute names
 * @param size      Size of the buffer (or 0 to query size)
 * @return          Total size of attribute names on success, negative on error
 *                  If size is 0, returns the required buffer size
 */
ssize_t xattr_list(const char *path, char *list, size_t size);

// ============================================================================
// Syscall Wrappers
// ============================================================================

/**
 * System call: Get extended attribute
 * @param path      User-space path string
 * @param name      User-space attribute name
 * @param value     User-space buffer for value
 * @param size      Buffer size
 * @return          Value size or negative error
 */
int64_t sys_getxattr(const char *path, const char *name, void *value, size_t size);

/**
 * System call: Set extended attribute
 * @param path      User-space path string
 * @param name      User-space attribute name
 * @param value     User-space value buffer
 * @param size      Value size
 * @param flags     Operation flags
 * @return          0 on success or negative error
 */
int64_t sys_setxattr(const char *path, const char *name, const void *value, 
                     size_t size, int flags);

/**
 * System call: Remove extended attribute
 * @param path      User-space path string
 * @param name      User-space attribute name
 * @return          0 on success or negative error
 */
int64_t sys_removexattr(const char *path, const char *name);

/**
 * System call: List extended attributes
 * @param path      User-space path string
 * @param list      User-space buffer for attribute names
 * @param size      Buffer size
 * @return          Total size of names or negative error
 */
int64_t sys_listxattr(const char *path, char *list, size_t size);

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Parse namespace from attribute name
 * @param name      Full attribute name (e.g., "user.mime_type")
 * @return          Namespace identifier
 */
xattr_namespace_t xattr_parse_namespace(const char *name);

/**
 * Check if caller has permission for the given namespace
 * @param ns        Namespace to check
 * @return          1 if allowed, 0 if denied
 */
int xattr_check_permission(xattr_namespace_t ns);

/**
 * Get the xattr storage path for a file
 * @param file_path Original file path
 * @param xattr_path Buffer to store the xattr file path
 * @param buf_size   Size of buffer
 * @return          0 on success, negative on error
 */
int xattr_get_storage_path(const char *file_path, char *xattr_path, size_t buf_size);

/**
 * Flush dirty cache entries to disk
 */
void xattr_cache_flush(void);

/**
 * Invalidate cache entries for a file
 * @param path      File path to invalidate
 */
void xattr_cache_invalidate(const char *path);

/**
 * Get cache statistics
 * @param hits      Pointer to store hit count
 * @param misses    Pointer to store miss count
 */
void xattr_cache_stats(uint32_t *hits, uint32_t *misses);

/**
 * Remove all xattrs for a file (called when file is deleted)
 * @param path      Path to the deleted file
 * @return          0 on success, negative on error
 */
int xattr_remove_all(const char *path);

/**
 * Copy all xattrs from one file to another
 * @param src_path  Source file path
 * @param dst_path  Destination file path
 * @return          Number of attributes copied, negative on error
 */
int xattr_copy_all(const char *src_path, const char *dst_path);

// ============================================================================
// Helper Macros
// ============================================================================

// Check if a name starts with a namespace prefix
#define XATTR_IS_USER_NS(name)     (strncmp(name, XATTR_NS_USER_PREFIX, XATTR_NS_USER_PREFIX_LEN) == 0)
#define XATTR_IS_SYSTEM_NS(name)   (strncmp(name, XATTR_NS_SYSTEM_PREFIX, XATTR_NS_SYSTEM_PREFIX_LEN) == 0)
#define XATTR_IS_SECURITY_NS(name) (strncmp(name, XATTR_NS_SECURITY_PREFIX, XATTR_NS_SECURITY_PREFIX_LEN) == 0)
#define XATTR_IS_TRUSTED_NS(name)  (strncmp(name, XATTR_NS_TRUSTED_PREFIX, XATTR_NS_TRUSTED_PREFIX_LEN) == 0)

#endif // XATTR_H
