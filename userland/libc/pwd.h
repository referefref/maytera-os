// pwd.h - Password database access for MayteraOS
// Provides POSIX-like getpwuid/getpwnam by reading /CONFIG/PASSWD
#ifndef _PWD_H
#define _PWD_H

#include "types.h"

#ifndef _UID_T_DEFINED
#define _UID_T_DEFINED
typedef unsigned int uid_t;
#endif
#ifndef _GID_T_DEFINED
#define _GID_T_DEFINED
typedef unsigned int gid_t;
#endif

// Password database entry
struct passwd {
    char    *pw_name;       // Username
    uid_t    pw_uid;        // User ID
    gid_t    pw_gid;        // Primary group ID
    char    *pw_dir;        // Home directory
    char    *pw_gecos;      // Display name / GECOS field
    char    *pw_shell;      // Login shell
};

// Look up user by UID
// Returns pointer to static struct (overwritten on next call), or NULL
struct passwd *getpwuid(uid_t uid);

// Look up user by name
// Returns pointer to static struct (overwritten on next call), or NULL
struct passwd *getpwnam(const char *name);

#endif // _PWD_H
