// grp.h - Group database access for MayteraOS
// Provides POSIX-like getgrgid/getgrnam by reading /CONFIG/GROUP
#ifndef _GRP_H
#define _GRP_H

#include "types.h"

#ifndef _GID_T_DEFINED
#define _GID_T_DEFINED
typedef unsigned int gid_t;
#endif

#define MAX_GRP_MEMBERS 16

// Group database entry
struct group {
    char    *gr_name;                   // Group name
    gid_t    gr_gid;                   // Group ID
    char    *gr_mem[MAX_GRP_MEMBERS];  // Array of member names (NULL-terminated)
};

// Look up group by GID
struct group *getgrgid(gid_t gid);

// Look up group by name
struct group *getgrnam(const char *name);

#endif // _GRP_H
