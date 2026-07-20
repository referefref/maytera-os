// grp.c - Group database access for MayteraOS
// Reads /CONFIG/GROUP to provide getgrgid/getgrnam

#include "grp.h"
#include "syscall.h"
#include "string.h"

// Static storage
static struct group grp_result;
static char grp_name_buf[32];
static char grp_member_bufs[MAX_GRP_MEMBERS][32];

// Parse /CONFIG/GROUP
// Format: groupname:gid:member1,member2,...
static struct group *parse_group(const char *match_name, int match_gid, int by_gid) {
    int fd = sys_open("/CONFIG/GROUP", 0);
    if (fd < 0) return NULL;

    char buf[2048];
    long nread = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);

    if (nread <= 0) return NULL;
    buf[nread] = '\0';

    char *line = buf;
    while (*line) {
        while (*line == '\n' || *line == '\r') line++;
        if (!*line) break;

        // Parse: groupname:gid:members
        char *name_start = line;
        char *p = line;

        // Find group name
        while (*p && *p != ':' && *p != '\n') p++;
        if (*p != ':') { while (*p && *p != '\n') p++; line = p; continue; }
        *p++ = '\0';

        // Parse gid
        int gid = 0;
        while (*p >= '0' && *p <= '9') { gid = gid * 10 + (*p - '0'); p++; }
        if (*p == ':') p++;

        // Members string
        char *members_start = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        if (*p) *p++ = '\0';
        line = p;

        // Check match
        int match = 0;
        if (by_gid && gid == match_gid) match = 1;
        if (!by_gid && match_name && strcmp(name_start, match_name) == 0) match = 1;

        if (match) {
            strncpy(grp_name_buf, name_start, sizeof(grp_name_buf) - 1);
            grp_result.gr_name = grp_name_buf;
            grp_result.gr_gid = (gid_t)gid;

            // Parse members
            int mi = 0;
            char *m = members_start;
            while (*m && mi < MAX_GRP_MEMBERS - 1) {
                char *ms = m;
                while (*m && *m != ',') m++;
                int len = (int)(m - ms);
                if (len > 0 && len < 32) {
                    memcpy(grp_member_bufs[mi], ms, len);
                    grp_member_bufs[mi][len] = '\0';
                    grp_result.gr_mem[mi] = grp_member_bufs[mi];
                    mi++;
                }
                if (*m == ',') m++;
            }
            grp_result.gr_mem[mi] = NULL;

            return &grp_result;
        }
    }

    return NULL;
}

struct group *getgrgid(gid_t gid) {
    return parse_group(NULL, (int)gid, 1);
}

struct group *getgrnam(const char *name) {
    return parse_group(name, 0, 0);
}
