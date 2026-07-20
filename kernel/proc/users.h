// users.h - User and group database for MayteraOS
// Provides user authentication, lookup, and management
#ifndef USERS_H
#define USERS_H

#include "../types.h"

#define MAX_USERS   32
#define MAX_GROUPS  32
#define USERNAME_MAX 32
#define DISPLAY_NAME_MAX 64
#define HOME_PATH_MAX 64
#define SHELL_PATH_MAX 64
#define PASSWORD_HASH_SIZE 64   // SHA-256 hex string (64 chars)

// User entry
typedef struct {
    uint32_t uid;
    uint32_t gid;               // Primary group
    char username[USERNAME_MAX];
    char display_name[DISPLAY_NAME_MAX];
    char home[HOME_PATH_MAX];   // Home directory path
    char shell[SHELL_PATH_MAX]; // Login shell path
    uint8_t active;             // Slot in use?
} user_entry_t;

// Group entry
typedef struct {
    uint32_t gid;
    char groupname[USERNAME_MAX];
    uint32_t members[MAX_USERS]; // UIDs in this group
    int member_count;
    uint8_t active;
} group_entry_t;

// ============================================================================
// API Functions
// ============================================================================

// Initialize user database, load from /CONFIG/PASSWD, /CONFIG/SHADOW, /CONFIG/GROUP
void users_init(void);

// Look up user by UID
user_entry_t *user_lookup_uid(uint32_t uid);

// Look up user by username
user_entry_t *user_lookup_name(const char *name);
user_entry_t *users_all(int *count_out);
int user_delete_by_name(const char *username);

// Verify a user's password against /CONFIG/SHADOW
// Returns 0 on success, -1 on failure
int user_verify_password(const char *username, const char *password);

// Set/change a user's password
// Returns 0 on success, -1 on failure
int user_set_password(const char *username, const char *password);

// Create a new user
// Returns 0 on success, -1 on failure
int user_create(const char *username, uint32_t uid, uint32_t gid,
                const char *home, const char *shell, const char *display_name);

// Create the standard home-folder skeleton (Desktop/Documents/Downloads/...)
// for a user. Safe to call repeatedly; no-op for the root '/' home.
void users_make_home_skeleton(const char *home, uint32_t uid, uint32_t gid);

// Delete a user by UID
// Returns 0 on success, -1 on failure
int user_delete(uint32_t uid);

// Get the user table for enumeration
user_entry_t *users_get_table(int *count);

// Get the group table for enumeration
group_entry_t *groups_get_table(int *count);

// Look up group by GID
group_entry_t *group_lookup_gid(uint32_t gid);

// Look up group by name
group_entry_t *group_lookup_name(const char *name);

// Check if a user is in a group
int user_in_group(uint32_t uid, uint32_t gid);

// Save user database to disk
void users_sync(void);

#endif // USERS_H
