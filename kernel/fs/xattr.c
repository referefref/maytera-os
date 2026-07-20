// xattr.c - Extended File Attributes Implementation for MayteraOS
// Stores xattrs in /.xattr/ directory on FAT filesystem
#include "xattr.h"
#include "fat.h"
#include "../types.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../serial.h"

// #404 batch-2 / MAYTERA-SEC-2026-0011: on-disk xattr entry-walk seam. The
// strangler flag -DRUST_XATTR routes the live get/list walk to the Rust port
// xattr_entry_next_rs (rustkern.rs, confines the CWE-125 heap over-read);
// default stays on the verbatim C reference (xattr_entry_c.c). Prototypes live
// in xattr.h; the descriptor + on-disk header layouts are sizeof-locked below so
// the FFI can never silently drift.
_Static_assert(sizeof(xattr_entry_t) == 16, "xattr_entry_t FFI layout");
_Static_assert(sizeof(xattr_entry_header_t) == 8, "on-disk xattr entry header");
#ifdef RUST_XATTR
#define xattr_entry_next xattr_entry_next_rs
#else
#define xattr_entry_next xattr_entry_next_c
#endif

// External FAT filesystem
extern fat_fs_t g_fat_fs;

// External timer for cache timestamps
extern volatile uint64_t timer_ticks;

// Global xattr cache
static xattr_cache_t g_xattr_cache;

// Xattr storage directory path
#define XATTR_STORAGE_DIR   "/.xattr"

// ============================================================================
// Forward Declarations
// ============================================================================

static int xattr_ensure_storage_dir(void);
static uint32_t xattr_compute_checksum(const void *data, size_t len);
static int xattr_read_file(const char *xattr_path, xattr_file_header_t **header, 
                           uint8_t **data, size_t *total_size);
static int xattr_write_file(const char *xattr_path, xattr_file_header_t *header,
                            uint8_t *data, size_t data_size);
static xattr_cache_entry_t *xattr_cache_find(const char *path, const char *name);
static xattr_cache_entry_t *xattr_cache_alloc(const char *path __attribute__((unused)), const char *name __attribute__((unused)));
static void xattr_cache_insert(const char *path, const char *name, 
                               const void *value, size_t value_len);

// ============================================================================
// String Utility Functions (local implementations)
// ============================================================================

// Simple hash function for generating xattr storage filename
static uint32_t xattr_path_hash(const char *path) {
    uint32_t hash = 5381;
    while (*path) {
        hash = ((hash << 5) + hash) ^ (uint8_t)*path;
        path++;
    }
    return hash;
}

// Convert uint32 to hex string
static void xattr_u32_to_hex(uint32_t val, char *buf) {
    const char *hex = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    buf[8] = '\0';
}

// ============================================================================
// Initialization
// ============================================================================

void xattr_init(void) {
    kprintf("[XATTR] Initializing extended attributes subsystem...\n");
    
    // Clear cache
    memset(&g_xattr_cache, 0, sizeof(g_xattr_cache));
    
    // Ensure storage directory exists
    if (xattr_ensure_storage_dir() < 0) {
        kprintf("[XATTR] Warning: Could not create xattr storage directory\n");
    }
    
    kprintf("[XATTR] Extended attributes ready\n");
}

// Ensure /.xattr directory exists
static int xattr_ensure_storage_dir(void) {
    fat_file_t dir;
    
    // Check if directory exists
    if (fat_open(&g_fat_fs, XATTR_STORAGE_DIR, &dir) == 0) {
        fat_close(&dir);
        return 0;  // Already exists
    }
    
    // Create the directory
    if (fat_mkdir(&g_fat_fs, XATTR_STORAGE_DIR) < 0) {
        kprintf("[XATTR] Failed to create storage directory\n");
        return XATTR_EIO;
    }
    
    kprintf("[XATTR] Created storage directory: %s\n", XATTR_STORAGE_DIR);
    return 0;
}

// ============================================================================
// Namespace Handling
// ============================================================================

xattr_namespace_t xattr_parse_namespace(const char *name) {
    if (!name) return XATTR_NAMESPACE_INVALID;
    
    if (XATTR_IS_USER_NS(name)) return XATTR_NAMESPACE_USER;
    if (XATTR_IS_SYSTEM_NS(name)) return XATTR_NAMESPACE_SYSTEM;
    if (XATTR_IS_SECURITY_NS(name)) return XATTR_NAMESPACE_SECURITY;
    if (XATTR_IS_TRUSTED_NS(name)) return XATTR_NAMESPACE_TRUSTED;
    
    return XATTR_NAMESPACE_INVALID;
}

int xattr_check_permission(xattr_namespace_t ns) {
    // TODO: Implement proper permission checking based on process privileges
    // For now, allow user and system namespaces, deny trusted namespace
    switch (ns) {
        case XATTR_NAMESPACE_USER:
        case XATTR_NAMESPACE_SYSTEM:
        case XATTR_NAMESPACE_SECURITY:
            return 1;  // Allowed
        case XATTR_NAMESPACE_TRUSTED:
            // Would need root/elevated privileges
            return 1;  // For now, allow all
        default:
            return 0;
    }
}

// ============================================================================
// Storage Path Generation
// ============================================================================

int xattr_get_storage_path(const char *file_path, char *xattr_path, size_t buf_size) {
    if (!file_path || !xattr_path || buf_size < 32) {
        return XATTR_EINVAL;
    }
    
    // Generate hash from file path
    uint32_t hash = xattr_path_hash(file_path);
    char hex[16];
    xattr_u32_to_hex(hash, hex);
    
    // Build xattr file path: /.xattr/XXXXXXXX.xat
    size_t needed = strlen(XATTR_STORAGE_DIR) + 1 + 8 + 4 + 1;  // dir + / + hash + .xat + null
    if (needed > buf_size) {
        return XATTR_ERANGE;
    }
    
    strcpy(xattr_path, XATTR_STORAGE_DIR);
    strcat(xattr_path, "/");
    strcat(xattr_path, hex);
    strcat(xattr_path, ".xat");
    
    return 0;
}

// ============================================================================
// Checksum Computation
// ============================================================================

static uint32_t xattr_compute_checksum(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = ((sum << 5) + sum) + bytes[i];
    }
    return sum;
}

// ============================================================================
// Cache Management
// ============================================================================

static xattr_cache_entry_t *xattr_cache_find(const char *path, const char *name) {
    for (int i = 0; i < XATTR_CACHE_SIZE; i++) {
        xattr_cache_entry_t *entry = &g_xattr_cache.entries[i];
        if ((entry->flags & XATTR_CACHE_VALID) &&
            strcmp(entry->path, path) == 0 &&
            strcmp(entry->name, name) == 0) {
            entry->access_time = g_xattr_cache.clock++;
            g_xattr_cache.hit_count++;
            return entry;
        }
    }
    g_xattr_cache.miss_count++;
    return NULL;
}

static xattr_cache_entry_t *xattr_cache_alloc(const char *path __attribute__((unused)), const char *name __attribute__((unused))) {
    xattr_cache_entry_t *oldest = NULL;
    uint64_t oldest_time = 0xFFFFFFFFFFFFFFFFULL;
    
    // Find empty slot or oldest entry
    for (int i = 0; i < XATTR_CACHE_SIZE; i++) {
        xattr_cache_entry_t *entry = &g_xattr_cache.entries[i];
        if (!(entry->flags & XATTR_CACHE_VALID)) {
            return entry;  // Empty slot found
        }
        if (entry->access_time < oldest_time) {
            oldest_time = entry->access_time;
            oldest = entry;
        }
    }
    
    // Evict oldest entry
    if (oldest && (oldest->flags & XATTR_CACHE_DIRTY)) {
        // TODO: Write back dirty entry before eviction
    }
    
    return oldest;
}

static void xattr_cache_insert(const char *path, const char *name, 
                               const void *value, size_t value_len) {
    // Only cache small values
    if (value_len > XATTR_CACHE_VALUE_MAX) return;
    
    xattr_cache_entry_t *entry = xattr_cache_alloc(path, name);
    if (!entry) return;
    
    strncpy(entry->path, path, XATTR_PATH_MAX - 1);
    entry->path[XATTR_PATH_MAX - 1] = '\0';
    strncpy(entry->name, name, XATTR_NAME_MAX - 1);
    entry->name[XATTR_NAME_MAX - 1] = '\0';
    
    if (value && value_len > 0) {
        memcpy(entry->value, value, value_len);
        entry->value_len = value_len;
        entry->flags = XATTR_CACHE_VALID;
    } else {
        entry->value_len = 0;
        entry->flags = XATTR_CACHE_VALID | XATTR_CACHE_NEGATIVE;
    }
    
    entry->access_time = g_xattr_cache.clock++;
}

void xattr_cache_invalidate(const char *path) {
    for (int i = 0; i < XATTR_CACHE_SIZE; i++) {
        xattr_cache_entry_t *entry = &g_xattr_cache.entries[i];
        if ((entry->flags & XATTR_CACHE_VALID) &&
            strcmp(entry->path, path) == 0) {
            entry->flags = 0;
        }
    }
}

void xattr_cache_flush(void) {
    for (int i = 0; i < XATTR_CACHE_SIZE; i++) {
        xattr_cache_entry_t *entry = &g_xattr_cache.entries[i];
        if (entry->flags & XATTR_CACHE_DIRTY) {
            // TODO: Write back to disk
            entry->flags &= ~XATTR_CACHE_DIRTY;
        }
    }
}

void xattr_cache_stats(uint32_t *hits, uint32_t *misses) {
    if (hits) *hits = g_xattr_cache.hit_count;
    if (misses) *misses = g_xattr_cache.miss_count;
}

// ============================================================================
// File I/O for Xattr Storage
// ============================================================================

static int xattr_read_file(const char *xattr_path, xattr_file_header_t **header,
                           uint8_t **data, size_t *total_size) {
    fat_file_t file;
    
    if (fat_open(&g_fat_fs, xattr_path, &file) < 0) {
        return XATTR_ENOENT;
    }
    
    uint32_t file_size = fat_size(&file);
    if (file_size < sizeof(xattr_file_header_t)) {
        fat_close(&file);
        return XATTR_EIO;
    }
    
    // Allocate buffer for entire file
    uint8_t *buf = kmalloc(file_size);
    if (!buf) {
        fat_close(&file);
        return XATTR_ENOMEM;
    }
    
    // Read file content
    int bytes_read = fat_read(&file, buf, file_size);
    fat_close(&file);
    
    if (bytes_read < (int)sizeof(xattr_file_header_t)) {
        kfree(buf);
        return XATTR_EIO;
    }
    
    // Validate header
    xattr_file_header_t *hdr = (xattr_file_header_t *)buf;
    if (hdr->magic != XATTR_FILE_MAGIC) {
        kfree(buf);
        return XATTR_EIO;
    }
    
    *header = hdr;
    *data = buf;
    *total_size = file_size;
    
    return 0;
}

static int xattr_write_file(const char *xattr_path, xattr_file_header_t *header,
                            uint8_t *data, size_t data_size) {
    // Update checksum
    header->checksum = xattr_compute_checksum(data + sizeof(xattr_file_header_t),
                                               data_size - sizeof(xattr_file_header_t));
    
    // Write to file
    if (fat_write_file(&g_fat_fs, xattr_path, data, data_size) < 0) {
        return XATTR_EIO;
    }
    
    return 0;
}

// ============================================================================
// Core Xattr Functions
// ============================================================================

ssize_t xattr_get(const char *path, const char *name, void *value, size_t size) {
    if (!path || !name) return XATTR_EINVAL;
    
    // Validate namespace
    xattr_namespace_t ns = xattr_parse_namespace(name);
    if (ns == XATTR_NAMESPACE_INVALID) {
        return XATTR_EINVAL;
    }
    
    // Check permission
    if (!xattr_check_permission(ns)) {
        return XATTR_EACCES;
    }
    
    // Check cache first
    xattr_cache_entry_t *cached = xattr_cache_find(path, name);
    if (cached) {
        if (cached->flags & XATTR_CACHE_NEGATIVE) {
            return XATTR_ENOATTR;
        }
        if (size == 0) {
            return cached->value_len;
        }
        if (size < cached->value_len) {
            return XATTR_ERANGE;
        }
        memcpy(value, cached->value, cached->value_len);
        return cached->value_len;
    }
    
    // Check if source file exists
    fat_file_t check_file;
    if (fat_open(&g_fat_fs, path, &check_file) < 0) {
        return XATTR_ENOENT;
    }
    fat_close(&check_file);
    
    // Get xattr storage path
    char xattr_path[XATTR_PATH_MAX];
    if (xattr_get_storage_path(path, xattr_path, sizeof(xattr_path)) < 0) {
        return XATTR_EINVAL;
    }
    
    // Read xattr file
    xattr_file_header_t *header;
    uint8_t *file_data;
    size_t file_size;
    
    int ret = xattr_read_file(xattr_path, &header, &file_data, &file_size);
    if (ret < 0) {
        xattr_cache_insert(path, name, NULL, 0);  // Negative cache
        return XATTR_ENOATTR;
    }
    
    // Search for the attribute via the entry-walk seam (Rust under -DRUST_XATTR,
    // else the verbatim C reference). The seam decides each entry's extent once
    // and rejects (rc != 1) a crafted name_len/value_len that would run past the
    // block BEFORE any attr_name/attr_value is dereferenced (MAYTERA-SEC-2026-0011).
    const uint8_t *entries = file_data + sizeof(xattr_file_header_t);
    uint32_t elen = (uint32_t)(file_size - sizeof(xattr_file_header_t));
    uint32_t pos = 0;
    ssize_t result = XATTR_ENOATTR;

    for (uint32_t i = 0; i < header->attr_count; i++) {
        xattr_entry_t ent;
        if (xattr_entry_next(entries, elen, &pos, &ent) != 1) break; // end or reject
        const char    *attr_name  = (const char *)(entries + ent.name_off);
        const uint8_t *attr_value = entries + ent.value_off;

        // Check if this is the attribute we're looking for
        if (ent.name_len == strlen(name) + 1 &&
            strcmp(attr_name, name) == 0) {
            // Found it
            if (size == 0) {
                result = ent.value_len;
            } else if (size < ent.value_len) {
                result = XATTR_ERANGE;
            } else {
                memcpy(value, attr_value, ent.value_len);
                result = ent.value_len;
            }

            // Cache the value
            xattr_cache_insert(path, name, attr_value, ent.value_len);
            break;
        }
    }
    
    kfree(file_data);
    
    if (result == XATTR_ENOATTR) {
        xattr_cache_insert(path, name, NULL, 0);  // Negative cache
    }
    
    return result;
}

int xattr_set(const char *path, const char *name, const void *value, size_t size, int flags) {
    if (!path || !name || (!value && size > 0)) return XATTR_EINVAL;
    if (strlen(name) >= XATTR_NAME_MAX) return XATTR_EINVAL;
    if (size > XATTR_VALUE_MAX) return XATTR_EINVAL;
    
    // Validate namespace
    xattr_namespace_t ns = xattr_parse_namespace(name);
    if (ns == XATTR_NAMESPACE_INVALID) {
        return XATTR_EINVAL;
    }
    
    // Check permission
    if (!xattr_check_permission(ns)) {
        return XATTR_EACCES;
    }
    
    // Check if source file exists
    fat_file_t check_file;
    if (fat_open(&g_fat_fs, path, &check_file) < 0) {
        return XATTR_ENOENT;
    }
    fat_close(&check_file);
    
    // Invalidate cache for this path
    xattr_cache_invalidate(path);
    
    // Get xattr storage path
    char xattr_path[XATTR_PATH_MAX];
    if (xattr_get_storage_path(path, xattr_path, sizeof(xattr_path)) < 0) {
        return XATTR_EINVAL;
    }
    
    // Try to read existing xattr file
    xattr_file_header_t *header = NULL;
    uint8_t *file_data = NULL;
    size_t file_size = 0;
    int existing = 0;
    
    if (xattr_read_file(xattr_path, &header, &file_data, &file_size) == 0) {
        existing = 1;
    }
    
    // Calculate new file size
    size_t name_len = strlen(name) + 1;  // Include null terminator
    size_t new_entry_size = sizeof(xattr_entry_header_t) + name_len + size;
    
    // Search for existing attribute and calculate removal if found
    int attr_found = 0;
    size_t old_entry_size = 0;
    uint8_t *old_entry_ptr = NULL;
    
    if (existing) {
        uint8_t *ptr = file_data + sizeof(xattr_file_header_t);
        uint8_t *end = file_data + file_size;
        
        for (uint32_t i = 0; i < header->attr_count && ptr < end; i++) {
            xattr_entry_header_t *entry = (xattr_entry_header_t *)ptr;
            char *attr_name = (char *)(ptr + sizeof(xattr_entry_header_t));
            
            if (entry->name_len == name_len && strcmp(attr_name, name) == 0) {
                attr_found = 1;
                old_entry_ptr = ptr;
                old_entry_size = sizeof(xattr_entry_header_t) + entry->name_len + entry->value_len;
                break;
            }
            
            ptr += sizeof(xattr_entry_header_t) + entry->name_len + entry->value_len;
        }
    }
    
    // Check flags
    if ((flags & XATTR_CREATE) && attr_found) {
        if (file_data) kfree(file_data);
        return XATTR_EEXIST;
    }
    if ((flags & XATTR_REPLACE) && !attr_found) {
        if (file_data) kfree(file_data);
        return XATTR_ENOATTR;
    }
    
    // Calculate new file size
    size_t new_file_size;
    if (existing) {
        if (attr_found) {
            new_file_size = file_size - old_entry_size + new_entry_size;
        } else {
            new_file_size = file_size + new_entry_size;
        }
    } else {
        new_file_size = sizeof(xattr_file_header_t) + new_entry_size;
    }
    
    // Allocate new buffer
    uint8_t *new_data = kmalloc(new_file_size);
    if (!new_data) {
        if (file_data) kfree(file_data);
        return XATTR_ENOMEM;
    }
    
    // Build new xattr file
    xattr_file_header_t *new_header = (xattr_file_header_t *)new_data;
    new_header->magic = XATTR_FILE_MAGIC;
    new_header->version = 1;
    new_header->total_size = new_file_size;
    memset(new_header->reserved, 0, sizeof(new_header->reserved));
    
    uint8_t *write_ptr = new_data + sizeof(xattr_file_header_t);
    uint32_t attr_count = 0;
    
    // Copy existing attributes (except the one being replaced)
    if (existing) {
        uint8_t *ptr = file_data + sizeof(xattr_file_header_t);
        uint8_t *end = file_data + file_size;
        
        for (uint32_t i = 0; i < header->attr_count && ptr < end; i++) {
            xattr_entry_header_t *entry = (xattr_entry_header_t *)ptr;
            size_t entry_size = sizeof(xattr_entry_header_t) + entry->name_len + entry->value_len;
            
            if (ptr != old_entry_ptr) {
                memcpy(write_ptr, ptr, entry_size);
                write_ptr += entry_size;
                attr_count++;
            }
            
            ptr += entry_size;
        }
    }
    
    // Add new/updated attribute
    xattr_entry_header_t *new_entry = (xattr_entry_header_t *)write_ptr;
    new_entry->name_len = name_len;
    new_entry->value_len = size;
    new_entry->namespace_id = (uint8_t)ns;
    new_entry->reserved = 0;
    write_ptr += sizeof(xattr_entry_header_t);
    
    memcpy(write_ptr, name, name_len);
    write_ptr += name_len;
    
    if (size > 0 && value) {
        memcpy(write_ptr, value, size);
    }
    attr_count++;
    
    new_header->attr_count = attr_count;
    
    // Write file
    int result = xattr_write_file(xattr_path, new_header, new_data, new_file_size);
    
    // Update cache
    if (result == 0) {
        xattr_cache_insert(path, name, value, size);
    }
    
    kfree(new_data);
    if (file_data) kfree(file_data);
    
    return result;
}

int xattr_remove(const char *path, const char *name) {
    if (!path || !name) return XATTR_EINVAL;
    
    // Validate namespace
    xattr_namespace_t ns = xattr_parse_namespace(name);
    if (ns == XATTR_NAMESPACE_INVALID) {
        return XATTR_EINVAL;
    }
    
    // Check permission
    if (!xattr_check_permission(ns)) {
        return XATTR_EACCES;
    }
    
    // Invalidate cache
    xattr_cache_invalidate(path);
    
    // Get xattr storage path
    char xattr_path[XATTR_PATH_MAX];
    if (xattr_get_storage_path(path, xattr_path, sizeof(xattr_path)) < 0) {
        return XATTR_EINVAL;
    }
    
    // Read existing xattr file
    xattr_file_header_t *header;
    uint8_t *file_data;
    size_t file_size;
    
    if (xattr_read_file(xattr_path, &header, &file_data, &file_size) < 0) {
        return XATTR_ENOATTR;
    }
    
    // Find the attribute
    size_t name_len = strlen(name) + 1;
    uint8_t *ptr = file_data + sizeof(xattr_file_header_t);
    uint8_t *end = file_data + file_size;
    uint8_t *found_entry = NULL;
    size_t found_entry_size = 0;
    
    for (uint32_t i = 0; i < header->attr_count && ptr < end; i++) {
        xattr_entry_header_t *entry = (xattr_entry_header_t *)ptr;
        char *attr_name = (char *)(ptr + sizeof(xattr_entry_header_t));
        size_t entry_size = sizeof(xattr_entry_header_t) + entry->name_len + entry->value_len;
        
        if (entry->name_len == name_len && strcmp(attr_name, name) == 0) {
            found_entry = ptr;
            found_entry_size = entry_size;
            break;
        }
        
        ptr += entry_size;
    }
    
    if (!found_entry) {
        kfree(file_data);
        return XATTR_ENOATTR;
    }
    
    // If this is the last attribute, delete the whole file
    if (header->attr_count == 1) {
        kfree(file_data);
        fat_delete(&g_fat_fs, xattr_path);
        return 0;
    }
    
    // Create new file without the removed attribute
    size_t new_file_size = file_size - found_entry_size;
    uint8_t *new_data = kmalloc(new_file_size);
    if (!new_data) {
        kfree(file_data);
        return XATTR_ENOMEM;
    }
    
    // Copy header
    xattr_file_header_t *new_header = (xattr_file_header_t *)new_data;
    memcpy(new_header, header, sizeof(xattr_file_header_t));
    new_header->attr_count--;
    new_header->total_size = new_file_size;
    
    // Copy attributes, skipping the removed one
    uint8_t *write_ptr = new_data + sizeof(xattr_file_header_t);
    ptr = file_data + sizeof(xattr_file_header_t);
    
    for (uint32_t i = 0; i < header->attr_count && ptr < end; i++) {
        xattr_entry_header_t *entry = (xattr_entry_header_t *)ptr;
        size_t entry_size = sizeof(xattr_entry_header_t) + entry->name_len + entry->value_len;
        
        if (ptr != found_entry) {
            memcpy(write_ptr, ptr, entry_size);
            write_ptr += entry_size;
        }
        
        ptr += entry_size;
    }
    
    // Write file
    int result = xattr_write_file(xattr_path, new_header, new_data, new_file_size);
    
    kfree(new_data);
    kfree(file_data);
    
    return result;
}

ssize_t xattr_list(const char *path, char *list, size_t size) {
    if (!path) return XATTR_EINVAL;
    
    // Check if source file exists
    fat_file_t check_file;
    if (fat_open(&g_fat_fs, path, &check_file) < 0) {
        return XATTR_ENOENT;
    }
    fat_close(&check_file);
    
    // Get xattr storage path
    char xattr_path[XATTR_PATH_MAX];
    if (xattr_get_storage_path(path, xattr_path, sizeof(xattr_path)) < 0) {
        return XATTR_EINVAL;
    }
    
    // Read xattr file
    xattr_file_header_t *header;
    uint8_t *file_data;
    size_t file_size;
    
    if (xattr_read_file(xattr_path, &header, &file_data, &file_size) < 0) {
        return 0;  // No attributes
    }
    
    // Calculate total size needed and optionally copy names, via the entry-walk
    // seam (Rust under -DRUST_XATTR, else the C reference). Confines the crafted
    // name_len/value_len heap over-read on this reachable read path
    // (MAYTERA-SEC-2026-0011): a rejected entry (rc != 1) stops the walk before
    // any name is read/copied.
    size_t total_len = 0;
    const uint8_t *entries = file_data + sizeof(xattr_file_header_t);
    uint32_t elen = (uint32_t)(file_size - sizeof(xattr_file_header_t));
    uint32_t pos = 0;

    for (uint32_t i = 0; i < header->attr_count; i++) {
        xattr_entry_t ent;
        if (xattr_entry_next(entries, elen, &pos, &ent) != 1) break; // end or reject
        const char *attr_name = (const char *)(entries + ent.name_off);

        // Check namespace permissions
        xattr_namespace_t ns = (xattr_namespace_t)ent.namespace_id;
        if (xattr_check_permission(ns)) {
            if (size > 0 && list) {
                if (total_len + ent.name_len > size) {
                    kfree(file_data);
                    return XATTR_ERANGE;
                }
                memcpy(list + total_len, attr_name, ent.name_len);
            }
            total_len += ent.name_len;
        }
    }
    
    kfree(file_data);
    return total_len;
}

// ============================================================================
// Helper Functions
// ============================================================================

int xattr_remove_all(const char *path) {
    if (!path) return XATTR_EINVAL;
    
    // Invalidate cache
    xattr_cache_invalidate(path);
    
    // Get xattr storage path
    char xattr_path[XATTR_PATH_MAX];
    if (xattr_get_storage_path(path, xattr_path, sizeof(xattr_path)) < 0) {
        return XATTR_EINVAL;
    }
    
    // Delete the xattr file
    fat_delete(&g_fat_fs, xattr_path);
    return 0;
}

int xattr_copy_all(const char *src_path, const char *dst_path) {
    if (!src_path || !dst_path) return XATTR_EINVAL;
    
    // Get source xattr storage path
    char src_xattr_path[XATTR_PATH_MAX];
    if (xattr_get_storage_path(src_path, src_xattr_path, sizeof(src_xattr_path)) < 0) {
        return XATTR_EINVAL;
    }
    
    // Read source xattr file
    xattr_file_header_t *header;
    uint8_t *file_data;
    size_t file_size;
    
    if (xattr_read_file(src_xattr_path, &header, &file_data, &file_size) < 0) {
        return 0;  // No attributes to copy
    }
    
    // Get destination xattr storage path
    char dst_xattr_path[XATTR_PATH_MAX];
    if (xattr_get_storage_path(dst_path, dst_xattr_path, sizeof(dst_xattr_path)) < 0) {
        kfree(file_data);
        return XATTR_EINVAL;
    }
    
    // Write to destination
    int result = xattr_write_file(dst_xattr_path, header, file_data, file_size);
    int count = (result == 0) ? (int)header->attr_count : result;
    
    kfree(file_data);
    return count;
}

// ============================================================================
// Syscall Implementations
// ============================================================================

int64_t sys_getxattr(const char *path, const char *name, void *value, size_t size) {
    // TODO: Add user-space pointer validation
    return xattr_get(path, name, value, size);
}

int64_t sys_setxattr(const char *path, const char *name, const void *value,
                     size_t size, int flags) {
    // TODO: Add user-space pointer validation
    return xattr_set(path, name, value, size, flags);
}

int64_t sys_removexattr(const char *path, const char *name) {
    // TODO: Add user-space pointer validation
    return xattr_remove(path, name);
}

int64_t sys_listxattr(const char *path, char *list, size_t size) {
    // TODO: Add user-space pointer validation
    return xattr_list(path, list, size);
}

// ============================================================================
// #404 batch-2 / MAYTERA-SEC-2026-0011 boot-time self-test. Proves
// xattr_entry_next_rs == xattr_entry_next_c on a well-formed 2-entry block
// (comparing the descriptor FIELD-BY-FIELD, not memcmp - the #[repr(C)] struct's
// tail padding can differ C-vs-Rust), and witnesses the confinement: a crafted
// lone header claiming a huge name+value in a tiny block makes the C return 1
// with offsets PAST the block (its caller over-reads), while the Rust REJECTS
// (-1) before any name/value is dereferenced. Bounded, runs once at boot; logs
// one [RUST-DIFF] + one [RUST-SEC] xattr_entry line to serial + /BOOTLOG.
// ============================================================================
void xattr_entry_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    // One well-formed 2-entry block (entries region only).
    static const uint8_t good[] = {
        // entry0: name_len=5 value_len=3 ns=0 resv=0 | "user\0" (5 bytes) | val
        5,0, 3,0,0,0, 0,0, 'u','s','e','r',0, 0x11,0x22,0x33,
        // entry1: name_len=9 value_len=2 ns=1 resv=0 | "system.x\0" (9 bytes) | val
        9,0, 2,0,0,0, 1,0, 's','y','s','t','e','m','.','x',0, 0xAA,0xBB,
    };
    uint32_t pc = 0, pr = 0; int entries = 0, mism = 0;
    for (;;) {
        xattr_entry_t ec, er;
        int rc = xattr_entry_next_c (good, sizeof(good), &pc, &ec);
        int rr = xattr_entry_next_rs(good, sizeof(good), &pr, &er);
        if (rc != rr || pc != pr) { mism++; break; }
        if (rc != 1) break;
        if (ec.name_off != er.name_off || ec.value_off != er.value_off ||
            ec.value_len != er.value_len || ec.name_len != er.name_len ||
            ec.namespace_id != er.namespace_id) mism++;
        entries++;
    }
    // Malicious: a lone header claiming a huge name+value in a tiny 8-byte block.
    static const uint8_t evil[] = { 0x40,0x00, 0x00,0x10,0x00,0x00, 0,0 };
    uint32_t ecp = 0, erp = 0; xattr_entry_t oe;
    int rc_evil = xattr_entry_next_c (evil, sizeof(evil), &ecp, &oe);
    int rr_evil = xattr_entry_next_rs(evil, sizeof(evil), &erp, &oe);
    kprintf("[RUST-DIFF] xattr_entry: %d entries, mism=%d %s (LIVE=%s)\n",
            entries, mism, mism ? "*** MISMATCH ***" : "MATCH",
#ifdef RUST_XATTR
            "rust");
#else
            "c");
#endif
    bootlog_write("[RUST-DIFF] xattr_entry: %d entries mism=%d %s",
                  entries, mism, mism ? "MISMATCH" : "MATCH");
    kprintf("[RUST-SEC]  xattr_entry: evil name_len/value_len -> c rc=%d "
            "(offsets past block, caller over-reads), rust rc=%d (confined)\n",
            rc_evil, rr_evil);
}
