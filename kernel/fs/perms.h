// perms.h - File permissions database for MayteraOS
// Provides UNIX-style file permissions on FAT32 via a sidecar database
#ifndef PERMS_H
#define PERMS_H

#include "../types.h"

// Hash table size and limits
#define PERM_TABLE_SIZE     512
#define MAX_PERM_ENTRIES    2048

// Access check flags (matching POSIX)
#define R_OK    4   // Read permission
#define W_OK    2   // Write permission
#define X_OK    1   // Execute permission
#define F_OK    0   // File existence

// Permission entry for a single file/directory
typedef struct perm_entry {
    char path[256];             // Absolute path (uppercase FAT names)
    uint32_t uid;               // Owner UID
    uint32_t gid;               // Owner GID
    uint16_t mode;              // Permission bits (rwxrwxrwx + setuid/setgid/sticky)
    struct perm_entry *next;    // Hash chain
} perm_entry_t;

// ============================================================================
// API Functions
// ============================================================================

// Initialize permissions subsystem, load from /CONFIG/PERMS.DB
void perms_init(void);

// Check if a user has access to a file
// Returns 0 on success, -1 (EACCES) on denied
int perms_check(const char *path, uint32_t uid, uint32_t gid, int access);

// Set permissions for a file
void perms_set(const char *path, uint32_t uid, uint32_t gid, uint16_t mode);

// Remove permissions entry for a file
void perms_remove(const char *path);

// Flush permissions database to disk
void perms_sync(void);

// Get permissions for a file
// Returns 0 on success, -1 if no entry exists
int perms_get(const char *path, uint32_t *uid, uint32_t *gid, uint16_t *mode);

// Change mode (chmod), respecting ownership
// Returns 0 on success, -1 on permission denied
int perms_chmod(const char *path, uint32_t caller_uid, uint16_t mode);

// Set default permissions for a newly created file
void perms_set_default(const char *path, uint32_t uid, uint32_t gid, int is_dir);

#endif // PERMS_H
