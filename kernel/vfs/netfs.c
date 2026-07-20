// Suppress warnings
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
// netfs.c - Network Filesystem VFS Layer Implementation for MayteraOS
// Provides unified interface for NFS and SMB network filesystems

#include "netfs.h"
#include "../net/smb.h"
#include "../net/nfs.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"

// ============================================================================
// Global State
// ============================================================================

static netfs_mount_t mounts[NETFS_MAX_MOUNTS];
static netfs_file_t open_files[NETFS_MAX_OPEN_FILES];
static netfs_credential_t credentials[NETFS_MAX_CREDENTIALS];
static netfs_bookmark_t bookmarks[NETFS_MAX_BOOKMARKS];
static int num_credentials = 0;
static int num_bookmarks = 0;
static bool netfs_initialized = false;

// ============================================================================
// Helper Functions
// ============================================================================

// Find free mount slot
static int alloc_mount(void) {
    for (int i = 0; i < NETFS_MAX_MOUNTS; i++) {
        if (!mounts[i].active) {
            memset(&mounts[i], 0, sizeof(netfs_mount_t));
            return i;
        }
    }
    return -1;
}

// Find free file slot
static int alloc_file(void) {
    for (int i = 0; i < NETFS_MAX_OPEN_FILES; i++) {
        if (!open_files[i].active) {
            memset(&open_files[i], 0, sizeof(netfs_file_t));
            return i;
        }
    }
    return -1;
}

// Get file by handle
static netfs_file_t *get_file(int fd) {
    if (fd < 0 || fd >= NETFS_MAX_OPEN_FILES) return NULL;
    if (!open_files[fd].active) return NULL;
    return &open_files[fd];
}

// Parse IP address from string
static uint32_t parse_ip(const char *str) {
    uint32_t ip = 0;
    int octets[4] = {0};
    int n = 0;

    while (*str && n < 4) {
        if (*str >= '0' && *str <= '9') {
            octets[n] = octets[n] * 10 + (*str - '0');
        } else if (*str == '.') {
            n++;
        } else {
            break;
        }
        str++;
    }

    if (n == 3) {
        ip = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    }
    return ip;
}

// ============================================================================
// Initialization
// ============================================================================

void netfs_init(void) {
    if (netfs_initialized) return;

    memset(mounts, 0, sizeof(mounts));
    memset(open_files, 0, sizeof(open_files));
    memset(credentials, 0, sizeof(credentials));
    memset(bookmarks, 0, sizeof(bookmarks));

    // Initialize underlying protocols
    smb_init();
    // nfs_init();  // Uncomment when NFS is implemented

    netfs_initialized = true;
    kprintf("[NetFS] Network filesystem layer initialized\n");
}

// ============================================================================
// Path Handling
// ============================================================================

bool netfs_is_network_path(const char *path) {
    // Check for URL prefixes
    if (strncmp(path, "smb://", 6) == 0 || strncmp(path, "nfs://", 6) == 0) {
        return true;
    }
    // Check for UNC path
    if (strncmp(path, "\\\\", 2) == 0 || strncmp(path, "//", 2) == 0) {
        return true;
    }
    // Check for mounted network paths
    if (strncmp(path, "/net/", 5) == 0) {
        return true;
    }
    // Check against known mounts
    for (int i = 0; i < NETFS_MAX_MOUNTS; i++) {
        if (mounts[i].active) {
            int len = strlen(mounts[i].mount_point);
            if (strncmp(path, mounts[i].mount_point, len) == 0 &&
                (path[len] == '/' || path[len] == '\\' || path[len] == 0)) {
                return true;
            }
        }
    }
    return false;
}

netfs_mount_t *netfs_get_mount(const char *path) {
    int best_match = -1;
    int best_len = 0;

    for (int i = 0; i < NETFS_MAX_MOUNTS; i++) {
        if (mounts[i].active) {
            int len = strlen(mounts[i].mount_point);
            if (strncmp(path, mounts[i].mount_point, len) == 0 &&
                (path[len] == '/' || path[len] == '\\' || path[len] == 0)) {
                if (len > best_len) {
                    best_match = i;
                    best_len = len;
                }
            }
        }
    }

    return (best_match >= 0) ? &mounts[best_match] : NULL;
}

// ============================================================================
// URL Parsing
// ============================================================================

int netfs_parse_url(const char *url, netfs_type_t *type, char *server,
                    char *share, char *username, char *password) {
    // Default values
    *type = NETFS_TYPE_NONE;
    server[0] = 0;
    share[0] = 0;
    if (username) username[0] = 0;
    if (password) password[0] = 0;

    // Determine type from prefix
    if (strncmp(url, "smb://", 6) == 0) {
        *type = NETFS_TYPE_SMB;
        url += 6;
    } else if (strncmp(url, "nfs://", 6) == 0) {
        *type = NETFS_TYPE_NFS;
        url += 6;
    } else if (strncmp(url, "\\\\", 2) == 0 || strncmp(url, "//", 2) == 0) {
        *type = NETFS_TYPE_SMB;  // Assume SMB for UNC paths
        url += 2;
    } else {
        return NETFS_ERR_INVALID;
    }

    // Check for credentials (user:pass@)
    const char *at = strchr(url, '@');
    if (at && username && password) {
        const char *colon = strchr(url, ':');
        if (colon && colon < at) {
            int user_len = colon - url;
            int pass_len = at - colon - 1;
            strncpy(username, url, user_len < 63 ? user_len : 63);
            username[user_len < 63 ? user_len : 63] = 0;
            strncpy(password, colon + 1, pass_len < 63 ? pass_len : 63);
            password[pass_len < 63 ? pass_len : 63] = 0;
        } else {
            int user_len = at - url;
            strncpy(username, url, user_len < 63 ? user_len : 63);
            username[user_len < 63 ? user_len : 63] = 0;
        }
        url = at + 1;
    }

    // Extract server
    const char *slash = strchr(url, '/');
    if (!slash) slash = strchr(url, '\\');
    if (slash) {
        int srv_len = slash - url;
        strncpy(server, url, srv_len < NETFS_MAX_NAME - 1 ? srv_len : NETFS_MAX_NAME - 1);
        server[srv_len < NETFS_MAX_NAME - 1 ? srv_len : NETFS_MAX_NAME - 1] = 0;
        url = slash + 1;

        // Extract share
        slash = strchr(url, '/');
        if (!slash) slash = strchr(url, '\\');
        if (slash) {
            int share_len = slash - url;
            strncpy(share, url, share_len < NETFS_MAX_NAME - 1 ? share_len : NETFS_MAX_NAME - 1);
            share[share_len < NETFS_MAX_NAME - 1 ? share_len : NETFS_MAX_NAME - 1] = 0;
        } else {
            strncpy(share, url, NETFS_MAX_NAME - 1);
        }
    } else {
        strncpy(server, url, NETFS_MAX_NAME - 1);
    }

    return NETFS_OK;
}

int netfs_build_url(netfs_type_t type, const char *server, const char *share,
                    const char *username, char *url, size_t url_size) {
    const char *prefix = (type == NETFS_TYPE_SMB) ? "smb://" : "nfs://";

    if (username && username[0]) {
        snprintf(url, url_size, "%s%s@%s/%s", prefix, username, server, share);
    } else {
        snprintf(url, url_size, "%s%s/%s", prefix, server, share);
    }

    return NETFS_OK;
}

// ============================================================================
// Mount Operations
// ============================================================================

int netfs_mount(const char *url, const char *mount_point) {
    netfs_type_t type;
    char server[NETFS_MAX_NAME], share[NETFS_MAX_NAME];
    char username[64], password[64];

    if (netfs_parse_url(url, &type, server, share, username, password) < 0) {
        kprintf("[NetFS] Invalid URL: %s\n", url);
        return NETFS_ERR_INVALID;
    }

    uint32_t server_ip = parse_ip(server);
    if (server_ip == 0) {
        // TODO: DNS resolution
        kprintf("[NetFS] Cannot resolve server: %s\n", server);
        return NETFS_ERR_NOT_FOUND;
    }

    // Check for saved credentials
    if (username[0] == 0) {
        netfs_credential_t cred;
        if (netfs_get_credential(server_ip, type, &cred) == 0) {
            strncpy(username, cred.username, 63);
            strncpy(password, cred.password, 63);
        }
    }

    uint16_t port = (type == NETFS_TYPE_SMB) ? 445 : 2049;

    return netfs_mount_share(type, server_ip, port, share, mount_point, username, password);
}

int netfs_mount_share(netfs_type_t type, uint32_t server_ip, uint16_t port,
                      const char *share_name, const char *mount_point,
                      const char *username, const char *password) {
    int mount_idx = alloc_mount();
    if (mount_idx < 0) {
        kprintf("[NetFS] No free mount slots\n");
        return NETFS_ERR_NO_MEMORY;
    }

    netfs_mount_t *mnt = &mounts[mount_idx];
    mnt->type = type;
    mnt->server_ip = server_ip;
    mnt->port = port;
    strncpy(mnt->share_name, share_name, NETFS_MAX_NAME - 1);
    strncpy(mnt->mount_point, mount_point, NETFS_MAX_PATH - 1);
    if (username) strncpy(mnt->username, username, 63);
    mnt->authenticated = (username && username[0]);

    // Format server name
    snprintf(mnt->server_name, NETFS_MAX_NAME, "%d.%d.%d.%d",
             (server_ip >> 24) & 0xFF, (server_ip >> 16) & 0xFF,
             (server_ip >> 8) & 0xFF, server_ip & 0xFF);

    // Mount based on type
    int result = -1;
    if (type == NETFS_TYPE_SMB) {
        result = smb_mount_auth(server_ip, share_name,
                                "WORKGROUP", username, password, mount_point);
    } else if (type == NETFS_TYPE_NFS) {
        // result = nfs_mount(server_ip, share_name, mount_point);
        kprintf("[NetFS] NFS not yet implemented\n");
        result = NETFS_ERR_NOT_SUPPORTED;
    }

    if (result < 0) {
        mnt->active = false;
        return result;
    }

    mnt->protocol_handle = result;
    mnt->active = true;

    kprintf("[NetFS] Mounted %s://%s/%s at %s\n",
            netfs_type_name(type), mnt->server_name, share_name, mount_point);

    return mount_idx;
}

int netfs_unmount(const char *mount_point) {
    for (int i = 0; i < NETFS_MAX_MOUNTS; i++) {
        if (mounts[i].active && strcmp(mounts[i].mount_point, mount_point) == 0) {
            netfs_mount_t *mnt = &mounts[i];

            // Close all open files on this mount
            for (int j = 0; j < NETFS_MAX_OPEN_FILES; j++) {
                if (open_files[j].active && open_files[j].mount == mnt) {
                    netfs_close(j);
                }
            }

            // Unmount based on type
            if (mnt->type == NETFS_TYPE_SMB) {
                smb_unmount(mount_point);
            } else if (mnt->type == NETFS_TYPE_NFS) {
                // nfs_unmount(mount_point);
            }

            mnt->active = false;
            kprintf("[NetFS] Unmounted %s\n", mount_point);
            return NETFS_OK;
        }
    }

    return NETFS_ERR_NOT_MOUNTED;
}

int netfs_get_mount_info(const char *mount_point, netfs_mount_t *info) {
    for (int i = 0; i < NETFS_MAX_MOUNTS; i++) {
        if (mounts[i].active && strcmp(mounts[i].mount_point, mount_point) == 0) {
            memcpy(info, &mounts[i], sizeof(netfs_mount_t));
            return NETFS_OK;
        }
    }
    return NETFS_ERR_NOT_MOUNTED;
}

int netfs_list_mounts(netfs_mount_t *out_mounts, int max_mounts) {
    int count = 0;
    for (int i = 0; i < NETFS_MAX_MOUNTS && count < max_mounts; i++) {
        if (mounts[i].active) {
            memcpy(&out_mounts[count++], &mounts[i], sizeof(netfs_mount_t));
        }
    }
    return count;
}

// ============================================================================
// File Operations
// ============================================================================

// Get relative path within mount
static const char *get_relative_path(netfs_mount_t *mnt, const char *path) {
    int len = strlen(mnt->mount_point);
    if (strncmp(path, mnt->mount_point, len) != 0) {
        return path;
    }
    path += len;
    while (*path == '/' || *path == '\\') path++;
    return path;
}

int netfs_open(const char *path, uint32_t flags) {
    netfs_mount_t *mnt = netfs_get_mount(path);
    if (!mnt) {
        return NETFS_ERR_NOT_MOUNTED;
    }

    int fd = alloc_file();
    if (fd < 0) {
        return NETFS_ERR_NO_MEMORY;
    }

    netfs_file_t *file = &open_files[fd];
    file->mount = mnt;
    file->flags = flags;
    strncpy(file->path, path, NETFS_MAX_PATH - 1);

    // Convert flags to protocol-specific
    int result = -1;
    if (mnt->type == NETFS_TYPE_SMB) {
        uint32_t access = 0;
        uint32_t disposition = FILE_OPEN;

        if (flags & NETFS_O_RDONLY) access |= FILE_READ_DATA;
        if (flags & NETFS_O_WRONLY) access |= FILE_WRITE_DATA;
        if (flags & NETFS_O_RDWR) access |= FILE_READ_DATA | FILE_WRITE_DATA;
        if (flags & NETFS_O_CREAT) disposition = FILE_OPEN_IF;
        if (flags & NETFS_O_TRUNC) disposition = FILE_OVERWRITE_IF;
        if (flags & NETFS_O_EXCL) disposition = FILE_CREATE;

        result = smb_open(path, access, disposition);
    } else if (mnt->type == NETFS_TYPE_NFS) {
        // result = nfs_open(path, flags);
        result = NETFS_ERR_NOT_SUPPORTED;
    }

    if (result < 0) {
        return result;
    }

    file->protocol_fd = result;
    file->active = true;
    file->position = 0;

    return fd;
}

int netfs_close(int fd) {
    netfs_file_t *file = get_file(fd);
    if (!file) return NETFS_ERR_INVALID;

    if (file->mount->type == NETFS_TYPE_SMB) {
        smb_close(file->protocol_fd);
    } else if (file->mount->type == NETFS_TYPE_NFS) {
        // nfs_close(file->protocol_fd);
    }

    file->active = false;
    return NETFS_OK;
}

ssize_t netfs_read(int fd, void *buffer, size_t count) {
    netfs_file_t *file = get_file(fd);
    if (!file) return NETFS_ERR_INVALID;

    ssize_t result = -1;
    if (file->mount->type == NETFS_TYPE_SMB) {
        result = smb_read(file->protocol_fd, buffer, count);
    } else if (file->mount->type == NETFS_TYPE_NFS) {
        // result = nfs_read(file->protocol_fd, buffer, count);
    }

    if (result > 0) {
        file->position += result;
    }
    return result;
}

ssize_t netfs_write(int fd, const void *buffer, size_t count) {
    netfs_file_t *file = get_file(fd);
    if (!file) return NETFS_ERR_INVALID;

    ssize_t result = -1;
    if (file->mount->type == NETFS_TYPE_SMB) {
        result = smb_write(file->protocol_fd, buffer, count);
    } else if (file->mount->type == NETFS_TYPE_NFS) {
        // result = nfs_write(file->protocol_fd, buffer, count);
    }

    if (result > 0) {
        file->position += result;
    }
    return result;
}

int64_t netfs_seek(int fd, int64_t offset, int whence) {
    netfs_file_t *file = get_file(fd);
    if (!file) return NETFS_ERR_INVALID;

    int64_t result = -1;
    if (file->mount->type == NETFS_TYPE_SMB) {
        result = smb_seek(file->protocol_fd, offset, whence);
    } else if (file->mount->type == NETFS_TYPE_NFS) {
        // result = nfs_seek(file->protocol_fd, offset, whence);
    }

    if (result >= 0) {
        file->position = result;
    }
    return result;
}

int netfs_stat(const char *path, netfs_stat_t *stat) {
    netfs_mount_t *mnt = netfs_get_mount(path);
    if (!mnt) {
        return NETFS_ERR_NOT_MOUNTED;
    }

    memset(stat, 0, sizeof(netfs_stat_t));

    if (mnt->type == NETFS_TYPE_SMB) {
        smb_dirent_t smb_file_stat;
        int result = smb_stat(path, &smb_file_stat);
        if (result < 0) return result;

        strncpy(stat->name, smb_file_stat.name, NETFS_MAX_NAME - 1);
        stat->size = smb_file_stat.size;
        stat->creation_time = smb_file_stat.creation_time;
        stat->modified_time = smb_file_stat.last_write_time;
        stat->access_time = smb_file_stat.last_access_time;
        stat->is_directory = smb_file_stat.is_directory;
        stat->is_hidden = (smb_file_stat.attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
        stat->is_read_only = (smb_file_stat.attributes & FILE_ATTRIBUTE_READONLY) != 0;
    } else if (mnt->type == NETFS_TYPE_NFS) {
        // nfs_fattr3_t nfs_attr;
        // int result = nfs_getattr(path, &nfs_attr);
        return NETFS_ERR_NOT_SUPPORTED;
    }

    return NETFS_OK;
}

// ============================================================================
// Directory Operations
// ============================================================================

int netfs_opendir(const char *path) {
    netfs_mount_t *mnt = netfs_get_mount(path);
    if (!mnt) {
        return NETFS_ERR_NOT_MOUNTED;
    }

    int fd = alloc_file();
    if (fd < 0) {
        return NETFS_ERR_NO_MEMORY;
    }

    netfs_file_t *file = &open_files[fd];
    file->mount = mnt;
    file->is_directory = true;
    strncpy(file->path, path, NETFS_MAX_PATH - 1);

    int result = -1;
    if (mnt->type == NETFS_TYPE_SMB) {
        result = smb_opendir(path);
    } else if (mnt->type == NETFS_TYPE_NFS) {
        // result = nfs_opendir(path);
    }

    if (result < 0) {
        return result;
    }

    file->protocol_fd = result;
    file->active = true;

    return fd;
}

int netfs_readdir(int dirfd, netfs_stat_t *entry) {
    netfs_file_t *file = get_file(dirfd);
    if (!file || !file->is_directory) return NETFS_ERR_INVALID;

    memset(entry, 0, sizeof(netfs_stat_t));

    if (file->mount->type == NETFS_TYPE_SMB) {
        smb_dirent_t smb_entry;
        int result = smb_readdir(file->protocol_fd, &smb_entry);
        if (result != 0) return result;  // 1 = end, -1 = error

        strncpy(entry->name, smb_entry.name, NETFS_MAX_NAME - 1);
        entry->size = smb_entry.size;
        entry->creation_time = smb_entry.creation_time;
        entry->modified_time = smb_entry.last_write_time;
        entry->access_time = smb_entry.last_access_time;
        entry->is_directory = smb_entry.is_directory;
        entry->is_hidden = (smb_entry.attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
        entry->is_read_only = (smb_entry.attributes & FILE_ATTRIBUTE_READONLY) != 0;
    } else if (file->mount->type == NETFS_TYPE_NFS) {
        // nfs_entry3_t *nfs_entry = nfs_readdir(file->protocol_fd);
        return NETFS_ERR_NOT_SUPPORTED;
    }

    return NETFS_OK;
}

int netfs_closedir(int dirfd) {
    netfs_file_t *file = get_file(dirfd);
    if (!file || !file->is_directory) return NETFS_ERR_INVALID;

    if (file->mount->type == NETFS_TYPE_SMB) {
        smb_closedir(file->protocol_fd);
    } else if (file->mount->type == NETFS_TYPE_NFS) {
        // nfs_closedir(file->protocol_fd);
    }

    file->active = false;
    return NETFS_OK;
}

int netfs_mkdir(const char *path, uint32_t mode) {
    netfs_mount_t *mnt = netfs_get_mount(path);
    if (!mnt) {
        return NETFS_ERR_NOT_MOUNTED;
    }

    if (mnt->type == NETFS_TYPE_SMB) {
        return smb_mkdir(path);
    } else if (mnt->type == NETFS_TYPE_NFS) {
        // return nfs_mkdir(path, mode);
    }

    return NETFS_ERR_NOT_SUPPORTED;
}

int netfs_rmdir(const char *path) {
    netfs_mount_t *mnt = netfs_get_mount(path);
    if (!mnt) {
        return NETFS_ERR_NOT_MOUNTED;
    }

    if (mnt->type == NETFS_TYPE_SMB) {
        return smb_rmdir(path);
    } else if (mnt->type == NETFS_TYPE_NFS) {
        // return nfs_rmdir(path);
    }

    return NETFS_ERR_NOT_SUPPORTED;
}

// ============================================================================
// File Management
// ============================================================================

int netfs_unlink(const char *path) {
    netfs_mount_t *mnt = netfs_get_mount(path);
    if (!mnt) {
        return NETFS_ERR_NOT_MOUNTED;
    }

    if (mnt->type == NETFS_TYPE_SMB) {
        return smb_delete(path);
    } else if (mnt->type == NETFS_TYPE_NFS) {
        // return nfs_remove(path);
    }

    return NETFS_ERR_NOT_SUPPORTED;
}

int netfs_rename(const char *oldpath, const char *newpath) {
    netfs_mount_t *mnt = netfs_get_mount(oldpath);
    if (!mnt) {
        return NETFS_ERR_NOT_MOUNTED;
    }

    // Both paths must be on same mount
    if (netfs_get_mount(newpath) != mnt) {
        return NETFS_ERR_INVALID;
    }

    if (mnt->type == NETFS_TYPE_SMB) {
        return smb_rename(oldpath, newpath);
    } else if (mnt->type == NETFS_TYPE_NFS) {
        // return nfs_rename(oldpath, newpath);
    }

    return NETFS_ERR_NOT_SUPPORTED;
}

// ============================================================================
// Credentials Management
// ============================================================================

int netfs_save_credential(const netfs_credential_t *cred) {
    // Check if already exists
    for (int i = 0; i < num_credentials; i++) {
        if (credentials[i].server_ip == cred->server_ip &&
            credentials[i].type == cred->type) {
            memcpy(&credentials[i], cred, sizeof(netfs_credential_t));
            return NETFS_OK;
        }
    }

    if (num_credentials >= NETFS_MAX_CREDENTIALS) {
        return NETFS_ERR_NO_MEMORY;
    }

    memcpy(&credentials[num_credentials++], cred, sizeof(netfs_credential_t));
    return NETFS_OK;
}

int netfs_get_credential(uint32_t server_ip, netfs_type_t type, netfs_credential_t *cred) {
    for (int i = 0; i < num_credentials; i++) {
        if (credentials[i].server_ip == server_ip && credentials[i].type == type) {
            memcpy(cred, &credentials[i], sizeof(netfs_credential_t));
            return NETFS_OK;
        }
    }
    return NETFS_ERR_NOT_FOUND;
}

int netfs_remove_credential(uint32_t server_ip, netfs_type_t type) {
    for (int i = 0; i < num_credentials; i++) {
        if (credentials[i].server_ip == server_ip && credentials[i].type == type) {
            // Shift remaining entries
            for (int j = i; j < num_credentials - 1; j++) {
                credentials[j] = credentials[j + 1];
            }
            num_credentials--;
            return NETFS_OK;
        }
    }
    return NETFS_ERR_NOT_FOUND;
}

int netfs_list_credentials(netfs_credential_t *creds, int max_creds) {
    int count = 0;
    for (int i = 0; i < num_credentials && count < max_creds; i++) {
        memcpy(&creds[count], &credentials[i], sizeof(netfs_credential_t));
        // Mask password
        memset(creds[count].password, '*', strlen(creds[count].password));
        count++;
    }
    return count;
}

// ============================================================================
// Bookmarks Management
// ============================================================================

int netfs_add_bookmark(const netfs_bookmark_t *bookmark) {
    // Check if already exists
    for (int i = 0; i < num_bookmarks; i++) {
        if (strcmp(bookmarks[i].name, bookmark->name) == 0) {
            memcpy(&bookmarks[i], bookmark, sizeof(netfs_bookmark_t));
            return NETFS_OK;
        }
    }

    if (num_bookmarks >= NETFS_MAX_BOOKMARKS) {
        return NETFS_ERR_NO_MEMORY;
    }

    memcpy(&bookmarks[num_bookmarks++], bookmark, sizeof(netfs_bookmark_t));
    return NETFS_OK;
}

int netfs_remove_bookmark(const char *name) {
    for (int i = 0; i < num_bookmarks; i++) {
        if (strcmp(bookmarks[i].name, name) == 0) {
            for (int j = i; j < num_bookmarks - 1; j++) {
                bookmarks[j] = bookmarks[j + 1];
            }
            num_bookmarks--;
            return NETFS_OK;
        }
    }
    return NETFS_ERR_NOT_FOUND;
}

int netfs_get_bookmark(const char *name, netfs_bookmark_t *bookmark) {
    for (int i = 0; i < num_bookmarks; i++) {
        if (strcmp(bookmarks[i].name, name) == 0) {
            memcpy(bookmark, &bookmarks[i], sizeof(netfs_bookmark_t));
            return NETFS_OK;
        }
    }
    return NETFS_ERR_NOT_FOUND;
}

int netfs_list_bookmarks(netfs_bookmark_t *out_bookmarks, int max_bookmarks) {
    int count = 0;
    for (int i = 0; i < num_bookmarks && count < max_bookmarks; i++) {
        memcpy(&out_bookmarks[count++], &bookmarks[i], sizeof(netfs_bookmark_t));
    }
    return count;
}

// ============================================================================
// Network Discovery
// ============================================================================

int netfs_discover_servers(netfs_resource_t *resources, int max_resources, int timeout_ms) {
    // Basic network scan - scan common ports on local subnet
    // In a full implementation, this would use mDNS/DNS-SD or NetBIOS name resolution
    kprintf("[NetFS] Network discovery not fully implemented\n");
    return 0;
}

int netfs_list_server_shares(uint32_t server_ip, netfs_type_t type,
                              netfs_resource_t *resources, int max_resources) {
    int count = 0;

    if (type == NETFS_TYPE_SMB) {
        // Use SMB to list shares
        char **shares;
        int num_shares;
        shares = smb_list_shares(server_ip, &num_shares);
        if (shares) {
            for (int i = 0; i < num_shares && count < max_resources; i++) {
                resources[count].type = NETFS_TYPE_SMB;
                resources[count].server_ip = server_ip;
                snprintf(resources[count].server_name, NETFS_MAX_NAME, "%d.%d.%d.%d",
                         (server_ip >> 24) & 0xFF, (server_ip >> 16) & 0xFF,
                         (server_ip >> 8) & 0xFF, server_ip & 0xFF);
                strncpy(resources[count].share_name, shares[i], NETFS_MAX_NAME - 1);
                resources[count].requires_auth = true;
                count++;
            }
            smb_free_shares(shares, num_shares);
        }
    } else if (type == NETFS_TYPE_NFS) {
        // Use NFS to list exports
        // char **exports;
        // int num_exports;
        // exports = nfs_list_exports(server_ip, &num_exports);
        // ...
    }

    return count;
}

// ============================================================================
// Utility Functions
// ============================================================================

const char *netfs_type_name(netfs_type_t type) {
    switch (type) {
        case NETFS_TYPE_SMB: return "smb";
        case NETFS_TYPE_NFS: return "nfs";
        case NETFS_TYPE_FTP: return "ftp";
        case NETFS_TYPE_WEBDAV: return "webdav";
        default: return "unknown";
    }
}

const char *netfs_strerror(int error) {
    switch (error) {
        case NETFS_OK: return "Success";
        case NETFS_ERR_INVALID: return "Invalid argument";
        case NETFS_ERR_NO_MEMORY: return "Out of memory";
        case NETFS_ERR_NOT_FOUND: return "Not found";
        case NETFS_ERR_EXISTS: return "Already exists";
        case NETFS_ERR_ACCESS: return "Access denied";
        case NETFS_ERR_TIMEOUT: return "Timeout";
        case NETFS_ERR_NETWORK: return "Network error";
        case NETFS_ERR_AUTH: return "Authentication failed";
        case NETFS_ERR_NOT_MOUNTED: return "Not mounted";
        case NETFS_ERR_BUSY: return "Resource busy";
        case NETFS_ERR_NOT_DIR: return "Not a directory";
        case NETFS_ERR_IS_DIR: return "Is a directory";
        case NETFS_ERR_NOT_EMPTY: return "Directory not empty";
        case NETFS_ERR_IO: return "I/O error";
        case NETFS_ERR_NOT_SUPPORTED: return "Not supported";
        default: return "Unknown error";
    }
}

const char *netfs_mount_prefix(netfs_type_t type) {
    switch (type) {
        case NETFS_TYPE_SMB: return "/net/smb/";
        case NETFS_TYPE_NFS: return "/net/nfs/";
        case NETFS_TYPE_FTP: return "/net/ftp/";
        default: return "/net/";
    }
}

void netfs_debug_info(void) {
    kprintf("\n=== NetFS Debug Info ===\n");
    kprintf("Active Mounts:\n");
    for (int i = 0; i < NETFS_MAX_MOUNTS; i++) {
        if (mounts[i].active) {
            kprintf("  [%d] %s -> %s://%s/%s\n", i,
                    mounts[i].mount_point,
                    netfs_type_name(mounts[i].type),
                    mounts[i].server_name,
                    mounts[i].share_name);
        }
    }

    kprintf("\nOpen Files:\n");
    int open_count = 0;
    for (int i = 0; i < NETFS_MAX_OPEN_FILES; i++) {
        if (open_files[i].active) {
            kprintf("  [%d] %s (%s)\n", i,
                    open_files[i].path,
                    open_files[i].is_directory ? "dir" : "file");
            open_count++;
        }
    }
    if (open_count == 0) {
        kprintf("  (none)\n");
    }

    kprintf("\nCredentials: %d stored\n", num_credentials);
    kprintf("Bookmarks: %d stored\n", num_bookmarks);
    kprintf("========================\n\n");
}
