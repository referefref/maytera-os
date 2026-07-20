#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
// netfs.c - Network Filesystem VFS Integration
// Provides unified interface for local FAT and network NFS file access

#include "netfs.h"
#include "fat.h"
#include "../net/nfs.h"
#include "../net/dns.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../serial.h"

// ============================================================================
// Global State
// ============================================================================

static vfs_mount_t vfs_mounts[VFS_MAX_MOUNTS];
static vfs_file_t vfs_files[NFS_MAX_OPEN_FILES];
static vfs_dir_t vfs_dirs[NFS_MAX_OPEN_FILES];
static bool vfs_initialized = false;

// Reference to global FAT filesystem
extern fat_fs_t g_fat_fs;

// ============================================================================
// Initialization
// ============================================================================

void vfs_init(void) {
    if (vfs_initialized) return;
    
    memset(vfs_mounts, 0, sizeof(vfs_mounts));
    memset(vfs_files, 0, sizeof(vfs_files));
    memset(vfs_dirs, 0, sizeof(vfs_dirs));
    
    // Initialize NFS subsystem
    nfs_init();
    
    // Mount root filesystem (local FAT) by default
    vfs_mounts[0].active = true;
    vfs_mounts[0].type = FS_TYPE_LOCAL;
    strcpy(vfs_mounts[0].mount_point, "/");
    vfs_mounts[0].source.fat.fs = &g_fat_fs;
    
    vfs_initialized = true;
    kprintf("[VFS] Initialized with root filesystem\n");
}

// ============================================================================
// Helper Functions
// ============================================================================

// Parse IP address from string
static uint32_t parse_ip_addr(const char *str) {
    uint32_t ip = 0;
    int octet = 0;
    int shift = 24;
    
    while (*str && shift >= 0) {
        if (*str >= '0' && *str <= '9') {
            octet = octet * 10 + (*str - '0');
        } else if (*str == '.') {
            ip |= (octet & 0xFF) << shift;
            shift -= 8;
            octet = 0;
        }
        str++;
    }
    ip |= (octet & 0xFF) << shift;
    
    return ip;
}

// Find mount for a given path
static vfs_mount_t *find_mount_for_path(const char *path, const char **relative_path) {
    size_t best_len = 0;
    vfs_mount_t *best_mount = NULL;
    
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!vfs_mounts[i].active) continue;
        
        size_t mp_len = strlen(vfs_mounts[i].mount_point);
        if (strncmp(path, vfs_mounts[i].mount_point, mp_len) == 0) {
            if (path[mp_len] == '/' || path[mp_len] == '\0') {
                if (mp_len > best_len) {
                    best_len = mp_len;
                    best_mount = &vfs_mounts[i];
                }
            }
        }
    }
    
    if (best_mount && relative_path) {
        *relative_path = path + best_len;
        if (**relative_path == '/') (*relative_path)++;
        if (**relative_path == '\0') *relative_path = NULL;
    }
    
    return best_mount;
}

// Convert NFS attributes to VFS stat
static void nfs_attrs_to_vfs_stat(const nfs_fattr3_t *nfs, vfs_stat_t *vfs) {
    vfs->is_dir = (nfs->type == NF3DIR);
    vfs->is_file = (nfs->type == NF3REG);
    vfs->is_symlink = (nfs->type == NF3LNK);
    vfs->size = nfs->size;
    vfs->mode = nfs->mode;
    vfs->uid = nfs->uid;
    vfs->gid = nfs->gid;
    vfs->atime = nfs->atime.seconds;
    vfs->mtime = nfs->mtime.seconds;
    vfs->ctime = nfs->ctime.seconds;
    vfs->inode = nfs->fileid;
    vfs->nlink = nfs->nlink;
}

// Convert FAT attributes to VFS stat
static void fat_entry_to_vfs_stat(const fat_dir_entry_t *fat, vfs_stat_t *vfs) {
    vfs->is_dir = (fat->attr & FAT_ATTR_DIRECTORY) != 0;
    vfs->is_file = !vfs->is_dir;
    vfs->is_symlink = false;
    vfs->size = fat->file_size;
    vfs->mode = vfs->is_dir ? 0755 : 0644;
    vfs->uid = 0;
    vfs->gid = 0;
    // Convert DOS time to Unix time (simplified)
    vfs->mtime = ((fat->create_date - 0x21) * 86400ULL) + 
                 ((fat->create_time >> 11) * 3600) +
                 (((fat->create_time >> 5) & 0x3F) * 60);
    vfs->atime = vfs->mtime;
    vfs->ctime = vfs->mtime;
    vfs->inode = 0;
    vfs->nlink = 1;
}

// ============================================================================
// Mount/Unmount
// ============================================================================

int vfs_mount(const char *source, const char *mount_point, fs_type_t type) {
    if (!vfs_initialized) vfs_init();
    
    // Find free mount slot
    int idx = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!vfs_mounts[i].active) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) {
        kprintf("[VFS] No free mount slots\n");
        return -1;
    }
    
    vfs_mount_t *mnt = &vfs_mounts[idx];
    memset(mnt, 0, sizeof(vfs_mount_t));
    
    mnt->type = type;
    strncpy(mnt->mount_point, mount_point, sizeof(mnt->mount_point) - 1);
    
    if (type == FS_TYPE_NFS) {
        // Parse NFS source: nfs://host/export or host:/export
        char host[128] = {0};
        char export_path[256] = {0};
        const char *p = source;
        
        // Skip nfs:// prefix
        if (strncmp(source, "nfs://", 6) == 0) {
            p += 6;
        }
        
        // Extract host
        int i = 0;
        while (*p && *p != ':' && *p != '/' && i < 127) {
            host[i++] = *p++;
        }
        host[i] = '\0';
        
        if (*p == ':') p++;
        
        // Extract export path
        strncpy(export_path, p, sizeof(export_path) - 1);
        if (export_path[0] == '\0') {
            strcpy(export_path, "/");
        }
        
        // Resolve hostname
        uint32_t server_ip = dns_resolve(host, NULL);
        if (server_ip == 0) {
            server_ip = parse_ip_addr(host);
        }
        
        if (server_ip == 0) {
            kprintf("[VFS] Cannot resolve host: %s\n", host);
            return -1;
        }
        
        // Mount NFS
        int nfs_ret = nfs_mount(server_ip, export_path, mount_point);
        if (nfs_ret < 0) {
            kprintf("[VFS] NFS mount failed\n");
            return -1;
        }
        
        mnt->source.nfs.server_ip = server_ip;
        strncpy(mnt->source.nfs.export_path, export_path, sizeof(mnt->source.nfs.export_path) - 1);
        
    } else if (type == FS_TYPE_LOCAL) {
        mnt->source.fat.fs = &g_fat_fs;
    }
    
    mnt->active = true;
    
    kprintf("[VFS] Mounted %s on %s (type=%d)\n", source, mount_point, type);
    return idx;
}

int vfs_unmount(const char *mount_point) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (vfs_mounts[i].active && 
            strcmp(vfs_mounts[i].mount_point, mount_point) == 0) {
            
            if (vfs_mounts[i].type == FS_TYPE_NFS) {
                nfs_unmount(mount_point);
            }
            
            vfs_mounts[i].active = false;
            kprintf("[VFS] Unmounted %s\n", mount_point);
            return 0;
        }
    }
    
    return -1;
}

void vfs_print_mounts(void) {
    kprintf("[VFS] Mount table:\n");
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (vfs_mounts[i].active) {
            const char *type_str = "local";
            if (vfs_mounts[i].type == FS_TYPE_NFS) type_str = "nfs";
            else if (vfs_mounts[i].type == FS_TYPE_SMB) type_str = "smb";
            
            kprintf("  %s [%s]\n", vfs_mounts[i].mount_point, type_str);
        }
    }
}

// ============================================================================
// Path Operations
// ============================================================================

bool vfs_is_network_path(const char *path) {
    vfs_mount_t *mnt = find_mount_for_path(path, NULL);
    return mnt && (mnt->type == FS_TYPE_NFS || mnt->type == FS_TYPE_SMB);
}

fs_type_t vfs_get_fs_type(const char *path) {
    vfs_mount_t *mnt = find_mount_for_path(path, NULL);
    return mnt ? mnt->type : FS_TYPE_LOCAL;
}

int vfs_realpath(const char *path, char *resolved, size_t size) {
    // TODO: Implement symlink resolution
    strncpy(resolved, path, size - 1);
    resolved[size - 1] = '\0';
    return 0;
}

// ============================================================================
// File Operations
// ============================================================================

int vfs_open(const char *path, const char *mode) {
    if (!vfs_initialized) vfs_init();
    
    // Find free file slot
    int fd = -1;
    for (int i = 0; i < NFS_MAX_OPEN_FILES; i++) {
        if (!vfs_files[i].active) {
            fd = i;
            break;
        }
    }
    
    if (fd < 0) return -1;
    
    vfs_file_t *file = &vfs_files[fd];
    memset(file, 0, sizeof(vfs_file_t));
    
    // Parse mode
    uint32_t flags = 0;
    if (strchr(mode, 'r') && strchr(mode, 'w')) flags = NFS_O_RDWR;
    else if (strchr(mode, 'w')) flags = NFS_O_WRONLY | NFS_O_CREAT | NFS_O_TRUNC;
    else flags = NFS_O_RDONLY;
    if (strchr(mode, 'a')) flags |= NFS_O_APPEND;
    
    const char *rel_path;
    vfs_mount_t *mnt = find_mount_for_path(path, &rel_path);
    
    if (!mnt) {
        kprintf("[VFS] No mount for path: %s\n", path);
        return -1;
    }
    
    file->type = mnt->type;
    strncpy(file->path, path, sizeof(file->path) - 1);
    file->mode = flags;
    
    if (mnt->type == FS_TYPE_NFS) {
        int nfs_fd = nfs_open(path, flags);
        if (nfs_fd < 0) return nfs_fd;
        file->handle.nfs_fd = nfs_fd;
        
    } else {
        // Local FAT
        const char *fat_path = rel_path ? rel_path : "/";
        if (fat_open(mnt->source.fat.fs, fat_path, &file->handle.fat) < 0) {
            return -1;
        }
    }
    
    file->active = true;
    return fd;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= NFS_MAX_OPEN_FILES) return -1;
    if (!vfs_files[fd].active) return -1;
    
    vfs_file_t *file = &vfs_files[fd];
    
    if (file->type == FS_TYPE_NFS) {
        nfs_close(file->handle.nfs_fd);
    } else {
        fat_close(&file->handle.fat);
    }
    
    file->active = false;
    return 0;
}

ssize_t vfs_read(int fd, void *buffer, size_t count) {
    if (fd < 0 || fd >= NFS_MAX_OPEN_FILES) return -1;
    if (!vfs_files[fd].active) return -1;
    
    vfs_file_t *file = &vfs_files[fd];
    
    if (file->type == FS_TYPE_NFS) {
        return nfs_read(file->handle.nfs_fd, buffer, count);
    } else {
        return fat_read(&file->handle.fat, buffer, (uint32_t)count);
    }
}

ssize_t vfs_write(int fd, const void *buffer, size_t count) {
    if (fd < 0 || fd >= NFS_MAX_OPEN_FILES) return -1;
    if (!vfs_files[fd].active) return -1;
    
    vfs_file_t *file = &vfs_files[fd];
    
    if (file->type == FS_TYPE_NFS) {
        return nfs_write(file->handle.nfs_fd, buffer, count);
    } else {
        return fat_write(&file->handle.fat, buffer, (uint32_t)count);
    }
}

int64_t vfs_seek(int fd, int64_t offset, int whence) {
    if (fd < 0 || fd >= NFS_MAX_OPEN_FILES) return -1;
    if (!vfs_files[fd].active) return -1;
    
    vfs_file_t *file = &vfs_files[fd];
    
    if (file->type == FS_TYPE_NFS) {
        return nfs_seek(file->handle.nfs_fd, offset, whence);
    } else {
        // FAT seek
        int ret = fat_seek(&file->handle.fat, (uint32_t)offset);
        return ret < 0 ? ret : offset;
    }
}

int64_t vfs_filesize(int fd) {
    if (fd < 0 || fd >= NFS_MAX_OPEN_FILES) return -1;
    if (!vfs_files[fd].active) return -1;
    
    vfs_file_t *file = &vfs_files[fd];
    
    if (file->type == FS_TYPE_NFS) {
        nfs_fattr3_t attrs;
        if (nfs_getattr(file->path, &attrs) < 0) return -1;
        return (int64_t)attrs.size;
    } else {
        return fat_size(&file->handle.fat);
    }
}

int vfs_truncate(int fd, uint64_t length) {
    if (fd < 0 || fd >= NFS_MAX_OPEN_FILES) return -1;
    if (!vfs_files[fd].active) return -1;
    
    vfs_file_t *file = &vfs_files[fd];
    
    if (file->type == FS_TYPE_NFS) {
        nfs_sattr3_t sattr = {0};
        sattr.set_size = true;
        sattr.size = length;
        return nfs_setattr(file->path, &sattr);
    }
    
    // FAT truncate not yet implemented
    return -1;
}

// ============================================================================
// Directory Operations
// ============================================================================

int vfs_opendir(const char *path) {
    if (!vfs_initialized) vfs_init();
    
    int dirfd = -1;
    for (int i = 0; i < NFS_MAX_OPEN_FILES; i++) {
        if (!vfs_dirs[i].active) {
            dirfd = i;
            break;
        }
    }
    
    if (dirfd < 0) return -1;
    
    vfs_dir_t *dir = &vfs_dirs[dirfd];
    memset(dir, 0, sizeof(vfs_dir_t));
    
    const char *rel_path;
    vfs_mount_t *mnt = find_mount_for_path(path, &rel_path);
    
    if (!mnt) return -1;
    
    dir->type = mnt->type;
    strncpy(dir->path, path, sizeof(dir->path) - 1);
    
    if (mnt->type == FS_TYPE_NFS) {
        int nfs_dirfd = nfs_opendir(path);
        if (nfs_dirfd < 0) return nfs_dirfd;
        dir->handle.nfs_dirfd = nfs_dirfd;
        
    } else {
        const char *fat_path = rel_path ? rel_path : "/";
        if (fat_open(mnt->source.fat.fs, fat_path, &dir->handle.fat) < 0) {
            return -1;
        }
        if (!fat_is_dir(&dir->handle.fat)) {
            fat_close(&dir->handle.fat);
            return -1;
        }
    }
    
    dir->active = true;
    return dirfd;
}

int vfs_readdir(int dirfd, vfs_entry_t *entry) {
    if (dirfd < 0 || dirfd >= NFS_MAX_OPEN_FILES) return -1;
    if (!vfs_dirs[dirfd].active) return -1;
    
    vfs_dir_t *dir = &vfs_dirs[dirfd];
    
    if (dir->type == FS_TYPE_NFS) {
        nfs_entry3_t *nfs_entry = nfs_readdir(dir->handle.nfs_dirfd);
        if (!nfs_entry) return -1;
        
        strncpy(entry->name, nfs_entry->name, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';
        
        // Get attributes for this entry
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir->path, nfs_entry->name);
        
        nfs_fattr3_t attrs;
        if (nfs_getattr(full_path, &attrs) == 0) {
            entry->is_dir = (attrs.type == NF3DIR);
            entry->is_symlink = (attrs.type == NF3LNK);
            entry->size = attrs.size;
            entry->mode = attrs.mode;
            entry->uid = attrs.uid;
            entry->gid = attrs.gid;
            entry->atime = attrs.atime.seconds;
            entry->mtime = attrs.mtime.seconds;
            entry->ctime = attrs.ctime.seconds;
        } else {
            // Basic info from fileid
            entry->is_dir = false;
            entry->is_symlink = false;
            entry->size = 0;
            entry->mode = 0644;
        }
        
        return 0;
        
    } else {
        fat_dir_entry_t fat_entry;
        char name[256];
        
        if (fat_readdir(&dir->handle.fat, &fat_entry, name) < 0) {
            return -1;
        }
        
        strncpy(entry->name, name, sizeof(entry->name) - 1);
        entry->is_dir = (fat_entry.attr & FAT_ATTR_DIRECTORY) != 0;
        entry->is_symlink = false;
        entry->size = fat_entry.file_size;
        entry->mode = entry->is_dir ? 0755 : 0644;
        entry->uid = 0;
        entry->gid = 0;
        entry->atime = entry->mtime = entry->ctime = 0;
        
        return 0;
    }
}

int vfs_closedir(int dirfd) {
    if (dirfd < 0 || dirfd >= NFS_MAX_OPEN_FILES) return -1;
    if (!vfs_dirs[dirfd].active) return -1;
    
    vfs_dir_t *dir = &vfs_dirs[dirfd];
    
    if (dir->type == FS_TYPE_NFS) {
        nfs_closedir(dir->handle.nfs_dirfd);
    } else {
        fat_close(&dir->handle.fat);
    }
    
    dir->active = false;
    return 0;
}

int vfs_mkdir(const char *path, uint32_t mode) {
    const char *rel_path;
    vfs_mount_t *mnt = find_mount_for_path(path, &rel_path);
    
    if (!mnt) return -1;
    
    if (mnt->type == FS_TYPE_NFS) {
        return nfs_mkdir(path, mode);
    } else {
        return fat_mkdir(mnt->source.fat.fs, rel_path ? rel_path : path);
    }
}

int vfs_rmdir(const char *path) {
    const char *rel_path;
    vfs_mount_t *mnt = find_mount_for_path(path, &rel_path);
    
    if (!mnt) return -1;
    
    if (mnt->type == FS_TYPE_NFS) {
        return nfs_rmdir(path);
    } else {
        return fat_delete(mnt->source.fat.fs, rel_path ? rel_path : path);
    }
}

// ============================================================================
// File Management
// ============================================================================

int vfs_stat(const char *path, vfs_stat_t *stat) {
    const char *rel_path;
    vfs_mount_t *mnt = find_mount_for_path(path, &rel_path);
    
    if (!mnt) return -1;
    
    memset(stat, 0, sizeof(vfs_stat_t));
    
    if (mnt->type == FS_TYPE_NFS) {
        nfs_fattr3_t nfs_attrs;
        int ret = nfs_getattr(path, &nfs_attrs);
        if (ret < 0) return ret;
        nfs_attrs_to_vfs_stat(&nfs_attrs, stat);
        return 0;
        
    } else {
        fat_file_t file;
        const char *fat_path = rel_path ? rel_path : "/";
        if (fat_open(mnt->source.fat.fs, fat_path, &file) < 0) {
            return -1;
        }
        
        stat->is_dir = fat_is_dir(&file);
        stat->is_file = !stat->is_dir;
        stat->is_symlink = false;
        stat->size = fat_size(&file);
        stat->mode = stat->is_dir ? 0755 : 0644;
        stat->nlink = 1;
        
        fat_close(&file);
        return 0;
    }
}

int vfs_create(const char *path, uint32_t mode) {
    const char *rel_path;
    vfs_mount_t *mnt = find_mount_for_path(path, &rel_path);
    
    if (!mnt) return -1;
    
    if (mnt->type == FS_TYPE_NFS) {
        return nfs_create(path, mode);
    } else {
        // Create empty file via FAT
        return fat_write_file(mnt->source.fat.fs, rel_path ? rel_path : path, "", 0);
    }
}

int vfs_remove(const char *path) {
    const char *rel_path;
    vfs_mount_t *mnt = find_mount_for_path(path, &rel_path);
    
    if (!mnt) return -1;
    
    if (mnt->type == FS_TYPE_NFS) {
        return nfs_remove(path);
    } else {
        return fat_delete(mnt->source.fat.fs, rel_path ? rel_path : path);
    }
}

int vfs_rename(const char *oldpath, const char *newpath) {
    const char *old_rel, *new_rel;
    vfs_mount_t *old_mnt = find_mount_for_path(oldpath, &old_rel);
    vfs_mount_t *new_mnt = find_mount_for_path(newpath, &new_rel);
    
    if (!old_mnt || !new_mnt || old_mnt != new_mnt) {
        // Cross-filesystem rename not supported
        return -1;
    }
    
    if (old_mnt->type == FS_TYPE_NFS) {
        return nfs_rename(oldpath, newpath);
    } else {
        return fat_rename(old_mnt->source.fat.fs, 
                         old_rel ? old_rel : oldpath,
                         new_rel ? new_rel : newpath);
    }
}

int vfs_copy(const char *src, const char *dst) {
    // Read source file
    size_t size;
    void *data = vfs_read_file(src, &size);
    if (!data) return -1;
    
    // Write to destination
    int ret = vfs_write_file(dst, data, size);
    
    kfree(data);
    return ret;
}

int vfs_symlink(const char *target, const char *linkpath) {
    const char *rel_path;
    vfs_mount_t *mnt = find_mount_for_path(linkpath, &rel_path);
    
    if (!mnt || mnt->type != FS_TYPE_NFS) {
        // Symlinks only supported on NFS
        return -1;
    }
    
    return nfs_symlink(target, linkpath);
}

int vfs_readlink(const char *path, char *buffer, size_t size) {
    const char *rel_path;
    vfs_mount_t *mnt = find_mount_for_path(path, &rel_path);
    
    if (!mnt || mnt->type != FS_TYPE_NFS) {
        return -1;
    }
    
    return nfs_readlink(path, buffer, size);
}

bool vfs_exists(const char *path) {
    vfs_stat_t stat;
    return vfs_stat(path, &stat) == 0;
}

bool vfs_is_dir(const char *path) {
    vfs_stat_t stat;
    if (vfs_stat(path, &stat) < 0) return false;
    return stat.is_dir;
}

bool vfs_is_file(const char *path) {
    vfs_stat_t stat;
    if (vfs_stat(path, &stat) < 0) return false;
    return stat.is_file;
}

// ============================================================================
// Utility Functions
// ============================================================================

void *vfs_read_file(const char *path, size_t *size_out) {
    const char *rel_path;
    vfs_mount_t *mnt = find_mount_for_path(path, &rel_path);
    
    if (!mnt) return NULL;
    
    if (mnt->type == FS_TYPE_NFS) {
        // Get file size first
        nfs_fattr3_t attrs;
        if (nfs_getattr(path, &attrs) < 0) return NULL;
        
        size_t size = (size_t)attrs.size;
        void *buffer = kmalloc(size + 1);
        if (!buffer) return NULL;
        
        int fd = nfs_open(path, NFS_O_RDONLY);
        if (fd < 0) {
            kfree(buffer);
            return NULL;
        }
        
        size_t total = 0;
        while (total < size) {
            ssize_t n = nfs_read(fd, (uint8_t *)buffer + total, size - total);
            if (n <= 0) break;
            total += n;
        }
        
        nfs_close(fd);
        
        ((char *)buffer)[total] = '\0';
        if (size_out) *size_out = total;
        return buffer;
        
    } else {
        uint32_t fat_size;
        void *data = fat_read_file(mnt->source.fat.fs, 
                                   rel_path ? rel_path : path, &fat_size);
        if (size_out) *size_out = fat_size;
        return data;
    }
}

int vfs_write_file(const char *path, const void *data, size_t size) {
    const char *rel_path;
    vfs_mount_t *mnt = find_mount_for_path(path, &rel_path);
    
    if (!mnt) return -1;
    
    if (mnt->type == FS_TYPE_NFS) {
        int fd = nfs_open(path, NFS_O_WRONLY | NFS_O_CREAT | NFS_O_TRUNC);
        if (fd < 0) return fd;
        
        size_t total = 0;
        while (total < size) {
            ssize_t n = nfs_write(fd, (const uint8_t *)data + total, size - total);
            if (n <= 0) {
                nfs_close(fd);
                return -1;
            }
            total += n;
        }
        
        nfs_close(fd);
        return 0;
        
    } else {
        return fat_write_file(mnt->source.fat.fs, 
                             rel_path ? rel_path : path, data, (uint32_t)size);
    }
}

const char *vfs_strerror(int error) {
    if (error >= 0) return "Success";
    
    // Convert to NFS error code
    nfs3_status_t nfs_err = (nfs3_status_t)(-error);
    return nfs_strerror(nfs_err);
}

// ============================================================================
// Network-Specific Operations
// ============================================================================

char **vfs_list_nfs_exports(const char *host, int *count) {
    uint32_t server_ip = dns_resolve(host, NULL);
    if (server_ip == 0) {
        server_ip = parse_ip_addr(host);
    }
    
    if (server_ip == 0) {
        *count = 0;
        return NULL;
    }
    
    return nfs_list_exports(server_ip, count);
}

void vfs_free_export_list(char **exports, int count) {
    nfs_free_exports(exports, count);
}

int vfs_nfs_server_info(const char *mount_point, nfs_fsstat3_t *stat) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (vfs_mounts[i].active && 
            vfs_mounts[i].type == FS_TYPE_NFS &&
            strcmp(vfs_mounts[i].mount_point, mount_point) == 0) {
            return nfs_fsstat(mount_point, stat);
        }
    }
    return -1;
}
