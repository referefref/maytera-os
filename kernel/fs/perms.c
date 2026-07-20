// perms.c - File permissions database for MayteraOS
// Stores file ownership and permission bits in a hash table, backed by /CONFIG/PERMS.DB

#include "perms.h"
#include "fat.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"

// External filesystem
extern fat_fs_t g_fat_fs;

// Hash table buckets
static perm_entry_t *perm_table[PERM_TABLE_SIZE];

// Pre-allocated entry pool (avoid per-entry kmalloc in freestanding environment)
static perm_entry_t perm_pool[MAX_PERM_ENTRIES];
static int perm_pool_next = 0;

// Dirty flag for sync
static bool perms_dirty = false;
static bool perms_initialized = false;

// ============================================================================
// Internal helpers
// ============================================================================

// DJB2 hash
static uint32_t path_hash(const char *path) {
    uint32_t hash = 5381;
    while (*path) {
        hash = ((hash << 5) + hash) + (uint8_t)*path;
        path++;
    }
    return hash % PERM_TABLE_SIZE;
}

// Normalize path to uppercase for FAT consistency
static void normalize_path(const char *src, char *dst, size_t dst_size) {
    size_t i = 0;
    while (src[i] && i < dst_size - 1) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[i] = c;
        i++;
    }
    dst[i] = '\0';
}

// Allocate a permission entry from the pool
static perm_entry_t *alloc_entry(void) {
    if (perm_pool_next >= MAX_PERM_ENTRIES) {
        return NULL;
    }
    perm_entry_t *e = &perm_pool[perm_pool_next++];
    memset(e, 0, sizeof(perm_entry_t));
    return e;
}

// Look up a permission entry by path
static perm_entry_t *perms_lookup(const char *path) {
    char norm[256];
    normalize_path(path, norm, sizeof(norm));

    uint32_t h = path_hash(norm);
    perm_entry_t *e = perm_table[h];
    while (e) {
        if (strcmp(e->path, norm) == 0) {
            return e;
        }
        e = e->next;
    }
    return NULL;
}

// Parse an octal string (e.g., "0755")
static uint16_t parse_octal(const char *s) {
    uint16_t val = 0;
    while (*s >= '0' && *s <= '7') {
        val = (val << 3) | (*s - '0');
        s++;
    }
    return val;
}

// Parse a decimal string
static uint32_t parse_uint(const char *s) {
    uint32_t val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return val;
}

// Format octal for output (writes 4 chars like "0755")
static void format_octal(uint16_t mode, char *buf) {
    buf[0] = '0' + ((mode >> 9) & 7);
    buf[1] = '0' + ((mode >> 6) & 7);
    buf[2] = '0' + ((mode >> 3) & 7);
    buf[3] = '0' + (mode & 7);
    buf[4] = '\0';
}

// ============================================================================
// Load / Save
// ============================================================================

// Parse a single line from PERMS.DB
// Format: /PATH:UID:GID:MODE
static void parse_perms_line(const char *line) {
    if (!line || line[0] == '\0' || line[0] == '#') return;

    // Find first colon (end of path)
    const char *p = line;
    const char *path_start = p;
    while (*p && *p != ':') p++;
    if (*p != ':') return;

    char path[256];
    size_t path_len = (size_t)(p - path_start);
    if (path_len >= sizeof(path)) return;
    memcpy(path, path_start, path_len);
    path[path_len] = '\0';

    p++;  // skip ':'

    // Parse UID
    uint32_t uid = parse_uint(p);
    while (*p && *p != ':') p++;
    if (*p != ':') return;
    p++;

    // Parse GID
    uint32_t gid = parse_uint(p);
    while (*p && *p != ':') p++;
    if (*p != ':') return;
    p++;

    // Parse mode (octal)
    uint16_t mode = parse_octal(p);

    // Store in hash table
    perms_set(path, uid, gid, mode);
}

void perms_init(void) {
    kprintf("[PERMS] Initializing permissions database...\n");

    // Clear hash table
    memset(perm_table, 0, sizeof(perm_table));
    perm_pool_next = 0;
    perms_dirty = false;

    // Try to load /CONFIG/PERMS.DB
    if (!g_fat_fs.mounted) {
        kprintf("[PERMS] No filesystem mounted, using defaults\n");
        perms_initialized = true;
        return;
    }

    // Ensure /CONFIG directory exists
    fat_mkdir(&g_fat_fs, "/CONFIG");

    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, "/CONFIG/PERMS.DB", &size);
    if (data && size > 0) {
        kprintf("[PERMS] Loading permissions from /CONFIG/PERMS.DB (%u bytes)\n", size);

        // Parse line by line
        char line[512];
        int line_pos = 0;
        const char *src = (const char *)data;
        int entry_count = 0;

        for (uint32_t i = 0; i <= size; i++) {
            if (i == size || src[i] == '\n' || src[i] == '\r') {
                line[line_pos] = '\0';
                if (line_pos > 0) {
                    parse_perms_line(line);
                    entry_count++;
                }
                line_pos = 0;
            } else if (line_pos < (int)sizeof(line) - 1) {
                line[line_pos++] = src[i];
            }
        }

        kfree(data);
        kprintf("[PERMS] Loaded %d permission entries\n", entry_count);
        perms_dirty = false;  // Just loaded, not dirty
    } else {
        kprintf("[PERMS] No PERMS.DB found, creating defaults\n");

        // Set up default permissions
        perms_set("/", 0, 0, 0755);
        perms_set("/APPS", 0, 0, 0755);
        perms_set("/BOOT", 0, 0, 0755);
        perms_set("/CONFIG", 0, 0, 0700);
        perms_set("/CONFIG/SHADOW", 0, 0, 0600);
        perms_set("/HOME", 0, 0, 0755);

        perms_sync();
    }

    perms_initialized = true;
    kprintf("[PERMS] Permissions database ready\n");
}

void perms_sync(void) {
    if (!perms_dirty || !g_fat_fs.mounted) return;

    kprintf("[PERMS] Syncing permissions to disk...\n");

    // Build the file content
    char *buf = kmalloc(64 * 1024);  // 64KB buffer
    if (!buf) {
        kprintf("[PERMS] Failed to allocate sync buffer\n");
        return;
    }

    int pos = 0;
    int count = 0;

    for (int i = 0; i < PERM_TABLE_SIZE; i++) {
        perm_entry_t *e = perm_table[i];
        while (e) {
            // Format: /PATH:UID:GID:MODE\n
            char mode_str[8];
            format_octal(e->mode, mode_str);

            // Build line manually
            int line_len = strlen(e->path);
            if (pos + line_len + 32 >= 64 * 1024) break;  // Buffer full

            memcpy(buf + pos, e->path, line_len);
            pos += line_len;
            buf[pos++] = ':';

            // UID
            char num[16];
            int n = 0;
            uint32_t v = e->uid;
            if (v == 0) { num[n++] = '0'; }
            else {
                char tmp[16]; int t = 0;
                while (v) { tmp[t++] = '0' + (v % 10); v /= 10; }
                while (t > 0) num[n++] = tmp[--t];
            }
            memcpy(buf + pos, num, n);
            pos += n;
            buf[pos++] = ':';

            // GID
            n = 0;
            v = e->gid;
            if (v == 0) { num[n++] = '0'; }
            else {
                char tmp[16]; int t = 0;
                while (v) { tmp[t++] = '0' + (v % 10); v /= 10; }
                while (t > 0) num[n++] = tmp[--t];
            }
            memcpy(buf + pos, num, n);
            pos += n;
            buf[pos++] = ':';

            // Mode (octal)
            memcpy(buf + pos, mode_str, 4);
            pos += 4;
            buf[pos++] = '\n';

            count++;
            e = e->next;
        }
    }

    // Write to disk
    fat_write_file(&g_fat_fs, "/CONFIG/PERMS.DB", buf, pos);
    kfree(buf);
    perms_dirty = false;
    kprintf("[PERMS] Synced %d entries to /CONFIG/PERMS.DB\n", count);
}

// ============================================================================
// Permission checking
// ============================================================================

int perms_check(const char *path, uint32_t proc_uid, uint32_t proc_gid, int access) {
    // Root bypasses all checks
    if (proc_uid == 0) return 0;

    // Kernel processes (called before perms_init) always pass
    if (!perms_initialized) return 0;

    perm_entry_t *e = perms_lookup(path);
    if (!e) {
        // No entry means default permissions: owned by root, mode 0755
        // Everyone can read/execute, only root can write
        if (access & W_OK) return -1;  // EACCES
        return 0;
    }

    uint16_t mode = e->mode;
    uint16_t bits;

    if (proc_uid == e->uid)       bits = (mode >> 6) & 7;  // Owner bits
    else if (proc_gid == e->gid)  bits = (mode >> 3) & 7;  // Group bits
    else                          bits = mode & 7;          // Other bits

    if ((access & R_OK) && !(bits & 4)) return -1;  // EACCES
    if ((access & W_OK) && !(bits & 2)) return -1;  // EACCES
    if ((access & X_OK) && !(bits & 1)) return -1;  // EACCES
    return 0;
}

// ============================================================================
// Permission management
// ============================================================================

void perms_set(const char *path, uint32_t uid, uint32_t gid, uint16_t mode) {
    char norm[256];
    normalize_path(path, norm, sizeof(norm));

    // Check if entry already exists
    perm_entry_t *e = perms_lookup(norm);
    if (e) {
        e->uid = uid;
        e->gid = gid;
        e->mode = mode;
        perms_dirty = true;
        return;
    }

    // Allocate new entry
    e = alloc_entry();
    if (!e) {
        kprintf("[PERMS] Permission table full\n");
        return;
    }

    strncpy(e->path, norm, sizeof(e->path) - 1);
    e->uid = uid;
    e->gid = gid;
    e->mode = mode;

    // Insert at head of hash chain
    uint32_t h = path_hash(norm);
    e->next = perm_table[h];
    perm_table[h] = e;

    perms_dirty = true;
}

void perms_remove(const char *path) {
    char norm[256];
    normalize_path(path, norm, sizeof(norm));

    uint32_t h = path_hash(norm);
    perm_entry_t *prev = NULL;
    perm_entry_t *e = perm_table[h];

    while (e) {
        if (strcmp(e->path, norm) == 0) {
            if (prev) prev->next = e->next;
            else perm_table[h] = e->next;
            // Note: we don't free pool entries (they're a simple bump allocator)
            // In a full implementation, we'd use a free list
            perms_dirty = true;
            return;
        }
        prev = e;
        e = e->next;
    }
}

int perms_get(const char *path, uint32_t *uid, uint32_t *gid, uint16_t *mode) {
    perm_entry_t *e = perms_lookup(path);
    if (!e) return -1;

    if (uid) *uid = e->uid;
    if (gid) *gid = e->gid;
    if (mode) *mode = e->mode;
    return 0;
}

int perms_chmod(const char *path, uint32_t caller_uid, uint16_t mode) {
    // Root can chmod anything
    if (caller_uid == 0) {
        perm_entry_t *e = perms_lookup(path);
        if (e) {
            e->mode = mode;
            perms_dirty = true;
        } else {
            perms_set(path, 0, 0, mode);
        }
        return 0;
    }

    // Non-root: must own the file
    perm_entry_t *e = perms_lookup(path);
    if (!e) return -1;  // No entry, default root-owned
    if (e->uid != caller_uid) return -1;  // EPERM

    e->mode = mode;
    perms_dirty = true;
    return 0;
}

void perms_set_default(const char *path, uint32_t uid, uint32_t gid, int is_dir) {
    uint16_t mode = is_dir ? 0755 : 0644;
    perms_set(path, uid, gid, mode);
}
