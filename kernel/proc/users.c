// users.c - User and group database for MayteraOS
// Manages /CONFIG/PASSWD, /CONFIG/SHADOW, /CONFIG/GROUP

#include "users.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../fs/perms.h"
#include "../fs/bootlog.h"
#include "../crypto/crypto.h"

// External filesystem
extern fat_fs_t g_fat_fs;

// User and group tables
static user_entry_t user_table[MAX_USERS];
static group_entry_t group_table[MAX_GROUPS];
static int user_count = 0;
static int group_count = 0;

// Shadow password table (kept separate for security)
typedef struct {
    char username[USERNAME_MAX];
    char hash[PASSWORD_HASH_SIZE + 1]; // SHA-256 hex string, or "*" for no-login
    uint8_t active;
} shadow_entry_t;

static shadow_entry_t shadow_table[MAX_USERS];
static int shadow_count = 0;

static bool users_initialized = false;

// ============================================================================
// Internal helpers
// ============================================================================

// Parse a decimal number from string, advance pointer past it
static uint32_t parse_uint_adv(const char **s) {
    uint32_t val = 0;
    while (**s >= '0' && **s <= '9') {
        val = val * 10 + (**s - '0');
        (*s)++;
    }
    return val;
}

// Copy string until delimiter or end, advance pointer past delimiter
static int copy_field(const char **src, char *dst, size_t dst_size, char delim) {
    size_t i = 0;
    while (**src && **src != delim && **src != '\n' && **src != '\r' && i < dst_size - 1) {
        dst[i++] = **src;
        (*src)++;
    }
    dst[i] = '\0';
    if (**src == delim) (*src)++;
    return (int)i;
}

// Compute SHA-256 hash of (password + username) and convert to hex
static void compute_password_hash(const char *password, const char *username,
                                   char *hex_out) {
    // Concatenate password + username for minimal salting
    char combined[256];
    size_t plen = strlen(password);
    size_t ulen = strlen(username);
    size_t total = plen + ulen;
    if (total > sizeof(combined) - 1) total = sizeof(combined) - 1;

    memcpy(combined, password, plen);
    memcpy(combined + plen, username, ulen);
    combined[total] = '\0';

    // SHA-256
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256(combined, total, digest);

    // Convert to hex string
    static const char hex_chars[] = "0123456789abcdef";
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++) {
        hex_out[i * 2]     = hex_chars[(digest[i] >> 4) & 0xf];
        hex_out[i * 2 + 1] = hex_chars[digest[i] & 0xf];
    }
    hex_out[SHA256_DIGEST_SIZE * 2] = '\0';

    // Zero out sensitive data
    memset(combined, 0, sizeof(combined));
    memset(digest, 0, sizeof(digest));
}

// #307 real-hardware robustness: a single failed/short USB-MSC read of a
// critical boot-path config file used to be indistinguishable from "the file
// doesn't exist" - load_passwd()/load_shadow()/load_groups() just returned
// on a NULL/zero-size fat_read_file() result, silently leaving 0 users
// loaded even though PASSWD/SHADOW really are present on disk (the physical
// iMac14,4 live-USB symptom: "No user accounts found" despite a verified-good
// image). Real USB-MSC devices can be slower/flakier than QEMU's virtual
// device under real timing (see xhci_delay() in drivers/xhci.c), so retry a
// bounded number of times with a short backoff, and log every attempt and
// outcome to the persistent boot log (bootlog_write) - this runs before
// sti() (main.c), so there is no scheduler yet and no proc_sleep(); the
// backoff is a plain io_wait() spin, matching the style already used
// elsewhere in this early-boot window (see main.c's spinner delays).
// Non-static: shared with gui/login.c via the prototype in fs/fat.h (#307).
void *fat_read_file_retry(fat_fs_t *fs, const char *path, uint32_t *size_out) {
    const int max_attempts = 3;
    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        uint32_t size = 0;
        void *data = fat_read_file(fs, path, &size);
        if (data && size > 0) {
            if (attempt > 1) {
                bootlog_write("[USERS] %s: read OK on attempt %d/%d (%u bytes)",
                              path, attempt, max_attempts, size);
            } else {
                bootlog_write("[USERS] %s: read OK (%u bytes)", path, size);
            }
            *size_out = size;
            return data;
        }
        if (data) kfree(data);  // zero-size result: free before retrying
        bootlog_write("[USERS] %s: read FAILED/empty on attempt %d/%d%s",
                      path, attempt, max_attempts,
                      attempt < max_attempts ? " - retrying" : " - giving up");
        if (attempt < max_attempts) {
            for (volatile uint32_t d = 0; d < 300000u * (uint32_t)attempt; d++) { io_wait(); }
        }
    }
    *size_out = 0;
    return NULL;
}

// ============================================================================
// Load from disk
// ============================================================================

// Parse /CONFIG/PASSWD
// Format: username:uid:gid:home:display_name:shell
static void load_passwd(void) {
    uint32_t size = 0;
    void *data = fat_read_file_retry(&g_fat_fs, "/CONFIG/PASSWD", &size);
    if (!data || size == 0) return;

    const char *src = (const char *)data;
    const char *end = src + size;

    while (src < end && user_count < MAX_USERS) {
        // Skip empty lines
        while (src < end && (*src == '\n' || *src == '\r')) src++;
        if (src >= end) break;
        if (*src == '#') { while (src < end && *src != '\n') src++; continue; }

        user_entry_t *u = &user_table[user_count];
        memset(u, 0, sizeof(user_entry_t));

        // username
        copy_field(&src, u->username, sizeof(u->username), ':');
        if (!u->username[0]) break;

        // uid
        u->uid = parse_uint_adv(&src);
        if (*src == ':') src++;

        // gid
        u->gid = parse_uint_adv(&src);
        if (*src == ':') src++;

        // home
        copy_field(&src, u->home, sizeof(u->home), ':');

        // display_name
        copy_field(&src, u->display_name, sizeof(u->display_name), ':');

        // shell
        copy_field(&src, u->shell, sizeof(u->shell), ':');

        u->active = 1;
        user_count++;

        // Skip to next line
        while (src < end && *src != '\n' && *src != '\r') src++;
    }

    kfree(data);
    kprintf("[USERS] Loaded %d users from /CONFIG/PASSWD\n", user_count);
    bootlog_write("[USERS] Loaded %d user(s) from /CONFIG/PASSWD", user_count);
}

// Parse /CONFIG/SHADOW
// Format: username:sha256_hex (or "*" for no-login)
static void load_shadow(void) {
    uint32_t size = 0;
    void *data = fat_read_file_retry(&g_fat_fs, "/CONFIG/SHADOW", &size);
    if (!data || size == 0) return;

    const char *src = (const char *)data;
    const char *end = src + size;

    while (src < end && shadow_count < MAX_USERS) {
        while (src < end && (*src == '\n' || *src == '\r')) src++;
        if (src >= end) break;
        if (*src == '#') { while (src < end && *src != '\n') src++; continue; }

        shadow_entry_t *s = &shadow_table[shadow_count];
        memset(s, 0, sizeof(shadow_entry_t));

        // username
        copy_field(&src, s->username, sizeof(s->username), ':');
        if (!s->username[0]) break;

        // hash
        copy_field(&src, s->hash, sizeof(s->hash), ':');

        s->active = 1;
        shadow_count++;

        while (src < end && *src != '\n' && *src != '\r') src++;
    }

    kfree(data);
    kprintf("[USERS] Loaded %d shadow entries from /CONFIG/SHADOW\n", shadow_count);
    bootlog_write("[USERS] Loaded %d shadow entrie(s) from /CONFIG/SHADOW", shadow_count);
}

// Parse /CONFIG/GROUP
// Format: groupname:gid:member1,member2,...
static void load_groups(void) {
    uint32_t size = 0;
    void *data = fat_read_file_retry(&g_fat_fs, "/CONFIG/GROUP", &size);
    if (!data || size == 0) return;

    const char *src = (const char *)data;
    const char *end = src + size;

    while (src < end && group_count < MAX_GROUPS) {
        while (src < end && (*src == '\n' || *src == '\r')) src++;
        if (src >= end) break;
        if (*src == '#') { while (src < end && *src != '\n') src++; continue; }

        group_entry_t *g = &group_table[group_count];
        memset(g, 0, sizeof(group_entry_t));

        // groupname
        copy_field(&src, g->groupname, sizeof(g->groupname), ':');
        if (!g->groupname[0]) break;

        // gid
        g->gid = parse_uint_adv(&src);
        if (*src == ':') src++;

        // members (comma-separated usernames, resolve to UIDs)
        char members_str[256];
        copy_field(&src, members_str, sizeof(members_str), ':');

        // Parse member list
        if (members_str[0]) {
            const char *m = members_str;
            while (*m && g->member_count < MAX_USERS) {
                char member_name[USERNAME_MAX];
                int mi = 0;
                while (*m && *m != ',' && mi < USERNAME_MAX - 1) {
                    member_name[mi++] = *m++;
                }
                member_name[mi] = '\0';
                if (*m == ',') m++;

                // Resolve username to UID
                user_entry_t *u = user_lookup_name(member_name);
                if (u) {
                    g->members[g->member_count++] = u->uid;
                }
            }
        }

        g->active = 1;
        group_count++;

        while (src < end && *src != '\n' && *src != '\r') src++;
    }

    kfree(data);
    kprintf("[USERS] Loaded %d groups from /CONFIG/GROUP\n", group_count);
    bootlog_write("[USERS] Loaded %d group(s) from /CONFIG/GROUP", group_count);
}

// ============================================================================
// Save to disk
// ============================================================================

static void save_passwd(void) {
    char *buf = kmalloc(8192);
    if (!buf) return;
    int pos = 0;

    for (int i = 0; i < user_count; i++) {
        user_entry_t *u = &user_table[i];
        if (!u->active) continue;

        // username:uid:gid:home:display_name:shell
        int n = snprintf(buf + pos, 8192 - pos, "%s:%u:%u:%s:%s:%s\n",
                         u->username, u->uid, u->gid, u->home,
                         u->display_name, u->shell);
        if (n > 0) pos += n;
    }

    fat_write_file(&g_fat_fs, "/CONFIG/PASSWD", buf, pos);
    kfree(buf);
}

static void save_shadow(void) {
    char *buf = kmalloc(8192);
    if (!buf) return;
    int pos = 0;

    for (int i = 0; i < shadow_count; i++) {
        shadow_entry_t *s = &shadow_table[i];
        if (!s->active) continue;

        int n = snprintf(buf + pos, 8192 - pos, "%s:%s\n",
                         s->username, s->hash);
        if (n > 0) pos += n;
    }

    fat_write_file(&g_fat_fs, "/CONFIG/SHADOW", buf, pos);
    // Ensure shadow file is root-only
    perms_set("/CONFIG/SHADOW", 0, 0, 0600);
    kfree(buf);
}

static void save_groups(void) {
    char *buf = kmalloc(8192);
    if (!buf) return;
    int pos = 0;

    for (int i = 0; i < group_count; i++) {
        group_entry_t *g = &group_table[i];
        if (!g->active) continue;

        int n = snprintf(buf + pos, 8192 - pos, "%s:%u:",
                         g->groupname, g->gid);
        if (n > 0) pos += n;

        // Members
        for (int j = 0; j < g->member_count; j++) {
            user_entry_t *u = user_lookup_uid(g->members[j]);
            if (u) {
                if (j > 0) buf[pos++] = ',';
                int len = strlen(u->username);
                memcpy(buf + pos, u->username, len);
                pos += len;
            }
        }
        buf[pos++] = '\n';
    }

    fat_write_file(&g_fat_fs, "/CONFIG/GROUP", buf, pos);
    kfree(buf);
}

// ============================================================================
// Create defaults on first boot
// ============================================================================

static void create_defaults(void) {
    kprintf("[USERS] Creating default user accounts...\n");

    // Create root user
    user_create("root", 0, 0, "/", "/APPS/MSH", "Root");
    user_set_password("root", "root");

    // Create admin user
    user_create("admin", 1000, 1000, "/HOME/ADMIN", "/APPS/MSH", "Admin");
    user_set_password("admin", "admin");

    // Create default groups
    group_entry_t *g;

    g = &group_table[group_count];
    memset(g, 0, sizeof(group_entry_t));
    g->gid = 0;
    strncpy(g->groupname, "root", sizeof(g->groupname) - 1);
    g->members[0] = 0;
    g->member_count = 1;
    g->active = 1;
    group_count++;

    g = &group_table[group_count];
    memset(g, 0, sizeof(group_entry_t));
    g->gid = 1000;
    strncpy(g->groupname, "users", sizeof(g->groupname) - 1);
    g->members[0] = 1000;
    g->member_count = 1;
    g->active = 1;
    group_count++;

    // Create /HOME/ADMIN directory + standard home-folder skeleton
    fat_mkdir(&g_fat_fs, "/HOME");
    fat_mkdir(&g_fat_fs, "/HOME/ADMIN");
    perms_set("/HOME/ADMIN", 1000, 1000, 0750);
    users_make_home_skeleton("/HOME/ADMIN", 1000, 1000);

    // Save to disk
    users_sync();
    kprintf("[USERS] Default accounts created (root/root, admin/admin)\n");
}

// ============================================================================
// Public API
// ============================================================================

void users_init(void) {
    kprintf("[USERS] Initializing user database...\n");

    memset(user_table, 0, sizeof(user_table));
    memset(group_table, 0, sizeof(group_table));
    memset(shadow_table, 0, sizeof(shadow_table));
    user_count = 0;
    group_count = 0;
    shadow_count = 0;

    if (!g_fat_fs.mounted) {
        kprintf("[USERS] No filesystem mounted, using defaults\n");
        bootlog_write("[USERS] No filesystem mounted; deferring to in-RAM defaults (no accounts persisted)");
        users_initialized = true;
        return;
    }

    // Ensure /CONFIG exists
    fat_mkdir(&g_fat_fs, "/CONFIG");

    // Try to load existing user database
    int passwd_exists = fat_exists(&g_fat_fs, "/CONFIG/PASSWD");
    bootlog_write("[USERS] fat_exists(/CONFIG/PASSWD) = %s", passwd_exists ? "yes" : "no");
    if (passwd_exists) {
        load_passwd();
        load_shadow();
        load_groups();

        // #307 real-hardware safety net: PASSWD demonstrably exists
        // (fat_exists said so) but zero ACTIVE users made it into the in-RAM
        // table even after fat_read_file_retry()'s bounded retries. This is
        // exactly the physical iMac14,4 symptom - "No user accounts found" at
        // the login screen despite a verified-good PASSWD/SHADOW on the
        // shipped image (i.e. sys_list_users() returning 0 is a downstream
        // SYMPTOM of this, not a bug in sys_list_users() itself). Rather than
        // leave the machine with no way to log in at all, fall back to the
        // standard root/admin defaults exactly as a first-boot would, and log
        // it loudly so the boot log makes the failure and the fallback
        // unmistakable for the next diagnosis pass.
        if (user_count == 0) {
            kprintf("[USERS] WARNING: /CONFIG/PASSWD exists but 0 users loaded "
                    "after retries - falling back to default accounts\n");
            bootlog_write("[USERS] WARNING: PASSWD exists but 0 users loaded after retries; "
                          "creating default root/admin accounts as a safety net so login is possible");
            memset(user_table, 0, sizeof(user_table));
            memset(group_table, 0, sizeof(group_table));
            memset(shadow_table, 0, sizeof(shadow_table));
            group_count = 0;
            shadow_count = 0;
            create_defaults();
        }
    } else {
        create_defaults();
    }

    // Ensure every existing user has the standard home-folder skeleton (covers
    // accounts created before this feature existed; fat_mkdir is idempotent).
    for (int i = 0; i < user_count; i++) {
        if (user_table[i].active && user_table[i].home[0])
            users_make_home_skeleton(user_table[i].home, user_table[i].uid, user_table[i].gid);
    }

    users_initialized = true;
    kprintf("[USERS] User database ready (%d users, %d groups)\n",
            user_count, group_count);
    bootlog_write("[USERS] User database ready: %d user(s), %d group(s)", user_count, group_count);
}

user_entry_t *user_lookup_uid(uint32_t uid) {
    for (int i = 0; i < user_count; i++) {
        if (user_table[i].active && user_table[i].uid == uid) {
            return &user_table[i];
        }
    }
    return NULL;
}

user_entry_t *user_lookup_name(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < user_count; i++) {
        if (user_table[i].active && strcmp(user_table[i].username, name) == 0) {
            return &user_table[i];
        }
    }
    return NULL;
}

int user_verify_password(const char *username, const char *password) {
    if (!username || !password) return -1;

    // Find shadow entry
    shadow_entry_t *s = NULL;
    for (int i = 0; i < shadow_count; i++) {
        if (shadow_table[i].active &&
            strcmp(shadow_table[i].username, username) == 0) {
            s = &shadow_table[i];
            break;
        }
    }
    if (!s) return -1;

    // "*" means no-login
    if (strcmp(s->hash, "*") == 0) return -1;

    // Compute hash and compare
    char computed[PASSWORD_HASH_SIZE + 1];
    compute_password_hash(password, username, computed);

    int result = (strcmp(computed, s->hash) == 0) ? 0 : -1;

    // Zero out computed hash
    memset(computed, 0, sizeof(computed));

    return result;
}

int user_set_password(const char *username, const char *password) {
    if (!username || !password) return -1;

    // Find or create shadow entry
    shadow_entry_t *s = NULL;
    for (int i = 0; i < shadow_count; i++) {
        if (shadow_table[i].active &&
            strcmp(shadow_table[i].username, username) == 0) {
            s = &shadow_table[i];
            break;
        }
    }

    if (!s) {
        if (shadow_count >= MAX_USERS) return -1;
        s = &shadow_table[shadow_count++];
        memset(s, 0, sizeof(shadow_entry_t));
        strncpy(s->username, username, sizeof(s->username) - 1);
        s->active = 1;
    }

    // Compute and store hash
    compute_password_hash(password, username, s->hash);
    return 0;
}

int user_create(const char *username, uint32_t uid, uint32_t gid,
                const char *home, const char *shell, const char *display_name) {
    if (!username) return -1;
    if (user_count >= MAX_USERS) return -1;

    // Check for duplicate username or UID
    if (user_lookup_name(username)) return -1;
    if (user_lookup_uid(uid)) return -1;

    user_entry_t *u = &user_table[user_count++];
    memset(u, 0, sizeof(user_entry_t));

    u->uid = uid;
    u->gid = gid;
    strncpy(u->username, username, sizeof(u->username) - 1);
    if (display_name) strncpy(u->display_name, display_name, sizeof(u->display_name) - 1);
    if (home) strncpy(u->home, home, sizeof(u->home) - 1);
    if (shell) strncpy(u->shell, shell, sizeof(u->shell) - 1);
    else strncpy(u->shell, "/APPS/MSH", sizeof(u->shell) - 1);
    u->active = 1;

    kprintf("[USERS] Created user '%s' (uid=%u, gid=%u)\n", username, uid, gid);
    return 0;
}

// Create the standard per-user home subfolders. FAT is 8.3-only (LFN pending)
// so the on-disk names are <=8 chars; the Files app maps them to friendly
// labels (Documents/Downloads/...). No-op for the root '/' home.
void users_make_home_skeleton(const char *home, uint32_t uid, uint32_t gid) {
    if (!home || !home[0] || !g_fat_fs.mounted) return;
    if (home[0] == '/' && home[1] == '\0') return;
    char path[160];
    // Create each parent component first (e.g. /HOME before /HOME/ADMIN); the
    // disk may not have /HOME yet, in which case mkdir(/HOME/ADMIN) would fail.
    int n = 0;
    for (int i = 0; home[i] && n < 158; i++) {
        path[n++] = home[i];
        if (home[i] == '/' && n > 1) { path[n - 1] = '\0'; fat_mkdir(&g_fat_fs, path); path[n - 1] = '/'; }
    }
    path[n] = '\0';
    fat_mkdir(&g_fat_fs, path);          // the home dir itself
    perms_set(path, uid, gid, 0750);
    // standard subfolders
    static const char *subs[] = { "DESKTOP", "DOCUMENT", "DOWNLOAD", "PICTURES", "MUSIC", "VIDEOS" };
    for (int i = 0; i < 6; i++) {
        int m = 0;
        for (; home[m] && m < 120; m++) path[m] = home[m];
        if (m > 0 && path[m - 1] != '/') path[m++] = '/';
        for (int j = 0; subs[i][j] && m < 158; j++) path[m++] = subs[i][j];
        path[m] = '\0';
        fat_mkdir(&g_fat_fs, path);
        perms_set(path, uid, gid, 0750);
    }
}

int user_delete(uint32_t uid) {
    for (int i = 0; i < user_count; i++) {
        if (user_table[i].active && user_table[i].uid == uid) {
            // Also remove shadow entry
            for (int j = 0; j < shadow_count; j++) {
                if (shadow_table[j].active &&
                    strcmp(shadow_table[j].username, user_table[i].username) == 0) {
                    shadow_table[j].active = 0;
                    break;
                }
            }
            user_table[i].active = 0;
            return 0;
        }
    }
    return -1;
}

user_entry_t *users_get_table(int *count) {
    if (count) *count = user_count;
    return user_table;
}

group_entry_t *groups_get_table(int *count) {
    if (count) *count = group_count;
    return group_table;
}

group_entry_t *group_lookup_gid(uint32_t gid) {
    for (int i = 0; i < group_count; i++) {
        if (group_table[i].active && group_table[i].gid == gid) {
            return &group_table[i];
        }
    }
    return NULL;
}

group_entry_t *group_lookup_name(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < group_count; i++) {
        if (group_table[i].active && strcmp(group_table[i].groupname, name) == 0) {
            return &group_table[i];
        }
    }
    return NULL;
}

int user_in_group(uint32_t uid, uint32_t gid) {
    // Check primary group first
    user_entry_t *u = user_lookup_uid(uid);
    if (u && u->gid == gid) return 1;

    // Check group membership
    group_entry_t *g = group_lookup_gid(gid);
    if (!g) return 0;

    for (int i = 0; i < g->member_count; i++) {
        if (g->members[i] == uid) return 1;
    }
    return 0;
}

void users_sync(void) {
    if (!g_fat_fs.mounted) return;
    save_passwd();
    save_shadow();
    save_groups();
    perms_sync();
    kprintf("[USERS] User database synced to disk\n");
}

// Expose the user table for SYS_LIST_USERS (caller skips inactive slots).
user_entry_t *users_all(int *count_out) {
    if (count_out) *count_out = user_count;
    return user_table;
}

// Delete a user by name (mark inactive + persist). Never deletes root (uid 0).
int user_delete_by_name(const char *username) {
    for (int i = 0; i < user_count; i++) {
        if (user_table[i].active && strcmp(user_table[i].username, username) == 0) {
            if (user_table[i].uid == 0) return -1;
            user_table[i].active = 0;
            save_passwd();
            save_shadow();
            perms_sync();
            return 0;
        }
    }
    return -1;
}
