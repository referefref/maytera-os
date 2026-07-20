// chown - Change file ownership for MayteraOS (root only)

#include "../../libc/maytera.h"
#include "../../libc/unistd.h"
#include "../../libc/pwd.h"
#include "../../libc/grp.h"

// Parse owner:group string
// Returns 0 on success, -1 on failure
static int parse_owner(const char *spec, uid_t *uid, gid_t *gid) {
    char owner[32] = {0};
    char group[32] = {0};

    // Find colon separator
    const char *colon = spec;
    while (*colon && *colon != ':') colon++;

    // Copy owner name
    int len = (int)(colon - spec);
    if (len >= 32) return -1;
    memcpy(owner, spec, len);
    owner[len] = '\0';

    // Copy group name if present
    if (*colon == ':' && *(colon + 1)) {
        strncpy(group, colon + 1, sizeof(group) - 1);
    }

    // Resolve owner
    if (owner[0]) {
        // Try as numeric UID first
        int numeric = 1;
        for (int i = 0; owner[i]; i++) {
            if (owner[i] < '0' || owner[i] > '9') { numeric = 0; break; }
        }
        if (numeric) {
            *uid = 0;
            const char *s = owner;
            while (*s) { *uid = *uid * 10 + (*s - '0'); s++; }
        } else {
            struct passwd *pw = getpwnam(owner);
            if (!pw) {
                printf("chown: unknown user '%s'\n", owner);
                return -1;
            }
            *uid = pw->pw_uid;
        }
    }

    // Resolve group
    if (group[0]) {
        int numeric = 1;
        for (int i = 0; group[i]; i++) {
            if (group[i] < '0' || group[i] > '9') { numeric = 0; break; }
        }
        if (numeric) {
            *gid = 0;
            const char *s = group;
            while (*s) { *gid = *gid * 10 + (*s - '0'); s++; }
        } else {
            struct group *gr = getgrnam(group);
            if (!gr) {
                printf("chown: unknown group '%s'\n", group);
                return -1;
            }
            *gid = gr->gr_gid;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: chown <owner[:group]> <file> [file2 ...]\n");
        return 1;
    }

    // Only root can chown
    if (geteuid() != 0) {
        printf("chown: operation not permitted (must be root)\n");
        return 1;
    }

    uid_t new_uid = (uid_t)-1;
    gid_t new_gid = (gid_t)-1;

    if (parse_owner(argv[1], &new_uid, &new_gid) != 0) {
        return 1;
    }

    int errors = 0;
    for (int i = 2; i < argc; i++) {
        if (chown(argv[i], new_uid, new_gid) != 0) {
            printf("chown: cannot change ownership of '%s'\n", argv[i]);
            errors++;
        }
    }

    return errors ? 1 : 0;
}
