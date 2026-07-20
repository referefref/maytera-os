// pwd.c - Password database access for MayteraOS
// Reads /CONFIG/PASSWD to provide getpwuid/getpwnam

#include "pwd.h"
#include "syscall.h"
#include "string.h"
#include "stdio.h"

// Static storage for return values
static struct passwd pw_result;
static char pw_name_buf[32];
static char pw_dir_buf[64];
static char pw_gecos_buf[64];
static char pw_shell_buf[64];

// Read /CONFIG/PASSWD and parse it looking for a match
// Format: username:uid:gid:home:display_name:shell
static struct passwd *parse_passwd(const char *match_name, int match_uid, int by_uid) {
    int fd = sys_open("/CONFIG/PASSWD", 0);
    if (fd < 0) return NULL;

    // Read the entire file (small, fits in stack buffer)
    char buf[2048];
    long nread = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);

    if (nread <= 0) return NULL;
    buf[nread] = '\0';

    // Parse line by line
    char *line = buf;
    while (*line) {
        // Skip to start of content
        while (*line == '\n' || *line == '\r') line++;
        if (!*line) break;

        // Parse fields: username:uid:gid:home:display_name:shell
        char *fields[6];
        int field_count = 0;
        char *p = line;

        fields[0] = p;
        field_count = 1;

        while (*p && *p != '\n' && *p != '\r') {
            if (*p == ':' && field_count < 6) {
                *p = '\0';
                fields[field_count++] = p + 1;
            }
            p++;
        }
        if (*p) *p++ = '\0';  // Null-terminate last field
        line = p;

        if (field_count < 4) continue;

        // Parse uid and gid
        int uid = 0, gid = 0;
        const char *s;

        s = fields[1];
        while (*s >= '0' && *s <= '9') { uid = uid * 10 + (*s - '0'); s++; }

        s = fields[2];
        while (*s >= '0' && *s <= '9') { gid = gid * 10 + (*s - '0'); s++; }

        // Check match
        int match = 0;
        if (by_uid && uid == match_uid) match = 1;
        if (!by_uid && match_name && strcmp(fields[0], match_name) == 0) match = 1;

        if (match) {
            // Fill result
            strncpy(pw_name_buf, fields[0], sizeof(pw_name_buf) - 1);
            pw_result.pw_name = pw_name_buf;
            pw_result.pw_uid = (uid_t)uid;
            pw_result.pw_gid = (gid_t)gid;

            if (field_count > 3) {
                strncpy(pw_dir_buf, fields[3], sizeof(pw_dir_buf) - 1);
            } else {
                pw_dir_buf[0] = '/'; pw_dir_buf[1] = '\0';
            }
            pw_result.pw_dir = pw_dir_buf;

            if (field_count > 4) {
                strncpy(pw_gecos_buf, fields[4], sizeof(pw_gecos_buf) - 1);
            } else {
                pw_gecos_buf[0] = '\0';
            }
            pw_result.pw_gecos = pw_gecos_buf;

            if (field_count > 5) {
                strncpy(pw_shell_buf, fields[5], sizeof(pw_shell_buf) - 1);
            } else {
                strncpy(pw_shell_buf, "/APPS/MSH", sizeof(pw_shell_buf) - 1);
            }
            pw_result.pw_shell = pw_shell_buf;

            return &pw_result;
        }
    }

    return NULL;
}

struct passwd *getpwuid(uid_t uid) {
    return parse_passwd(NULL, (int)uid, 1);
}

struct passwd *getpwnam(const char *name) {
    return parse_passwd(name, 0, 0);
}
