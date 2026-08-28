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

// Check if a user has access to a file, with POSIX path resolution (#674):
// the path is canonicalized (made absolute, "." dropped, ".." popped, "//"
// collapsed) exactly the way the filesystem resolves it, and SEARCH (x) is
// required on every directory component before `access` is applied to the
// object itself. Root (uid 0) and pre-perms_init callers bypass, unchanged.
// Returns 0 on success, -1 (EACCES) on denied.
int perms_check(const char *path, uint32_t uid, uint32_t gid, int access);

// #674: the single-object decision, for ONE already-canonical path. No
// traversal, and no uid-0 / !perms_initialized bypass (perms_check owns those).
// Exposed for the Rust path walker in rustkern/permpath.rs; not a general API.
int perms_check_leaf(const char *path, uint32_t uid, uint32_t gid, int access);

// #674: boot self-test. Proves canonicalization and directory traversal against
// the LIVE database, in both directions (denied stays denied, allowed stays
// allowed). Called from perms_init(); logs [PERMS-SELFTEST].
void perms_selftest(void);

// #PERMSKIP: the SESSION-SCOPED run of the same vectors, against THE ACTUAL
// LOGGED-IN USER'S HOME, whatever the first-boot wizard let the owner call the
// account. perms_init() runs long before any session exists, so the boot run
// above can only enumerate what /CONFIG/PERMS.DB already holds; this one is
// called from kernel/main.c straight after the login-time home claim (#745) and
// is the definitive run. `home` is the session user's home ("/" for root, which
// is NOT a home directory: see the comment on the definition). Logs
// [PERMS-SELFTEST], durably.
void perms_selftest_session(const char *home, uint32_t uid, uint32_t gid);

// Set permissions for a file
void perms_set(const char *path, uint32_t uid, uint32_t gid, uint16_t mode);

// Remove permissions entry for a file
void perms_remove(const char *path);

// Flush permissions database to disk
MUST_CHECK int perms_sync(void);   // #693: 0 only if PERMS.DB is on the medium

// #679: coalesced write-back, called from the heartbeat worker. Without it,
// ownership recorded at create time never reaches /CONFIG/PERMS.DB and is lost
// at reboot, so a user could write a file they created only until they rebooted.
MUST_CHECK int perms_sync_if_dirty(void);   // #693

// Get permissions for a file
// Returns 0 on success, -1 if no entry exists
int perms_get(const char *path, uint32_t *uid, uint32_t *gid, uint16_t *mode);

// Change mode (chmod), respecting ownership
// Returns 0 on success, -1 on permission denied
int perms_chmod(const char *path, uint32_t caller_uid, uint16_t mode);

// Set default permissions for a newly created file
void perms_set_default(const char *path, uint32_t uid, uint32_t gid, int is_dir);

// #679 prereq: record create-time ownership, the way Linux gives a new inode
// its creator's uid/gid. Call at EVERY create point with the creating process's
// euid/egid. Does NOT overwrite an existing entry, so a seeded 0600 or an
// operator chmod survives a delete-and-rewrite by its Ring-0 owner.
// Without this, perms_check()'s root-owned no-entry default denies W_OK to
// every non-root process for every path it creates, including in its own home.
void perms_on_create(const char *path, uint32_t uid, uint32_t gid, int is_dir);

#endif // PERMS_H
