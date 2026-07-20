// netfs.h - Network Filesystem VFS Integration
// Provides unified interface for local and network file access
#ifndef NETFS_H
#define NETFS_H

#include "../types.h"
#include "fat.h"
#include "../net/nfs.h"

// ============================================================================
// Filesystem Types
// ============================================================================

typedef enum {
    FS_TYPE_LOCAL = 0,      // Local FAT filesystem
    FS_TYPE_NFS,            // Network File System
    FS_TYPE_SMB             // SMB/CIFS (future)
} fs_type_t;

// ============================================================================
// Unified File Handle
// ============================================================================

typedef struct {
    bool active;
    fs_type_t type;
    union {
        fat_file_t fat;     // Local file
        int nfs_fd;         // NFS file descriptor
    } handle;
    char path[512];         // Full path
    uint32_t mode;          // Open mode
} vfs_file_t;

// ============================================================================
// Unified Directory Handle
// ============================================================================

typedef struct {
    bool active;
    fs_type_t type;
    union {
        fat_file_t fat;     // Local directory
        int nfs_dirfd;      // NFS directory handle
    } handle;
    char path[512];
} vfs_dir_t;

// ============================================================================
// Unified Directory Entry
// ============================================================================

typedef struct {
    char name[256];         // Entry name
    bool is_dir;            // Is directory
    bool is_symlink;        // Is symbolic link
    uint64_t size;          // File size
    uint32_t mode;          // File mode/permissions
    uint32_t uid;           // Owner user ID
    uint32_t gid;           // Owner group ID
    uint64_t atime;         // Access time (unix timestamp)
    uint64_t mtime;         // Modification time
    uint64_t ctime;         // Change time
} vfs_entry_t;

// ============================================================================
// Unified File Attributes
// ============================================================================

typedef struct {
    bool is_dir;
    bool is_file;
    bool is_symlink;
    uint64_t size;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint64_t inode;
    uint32_t nlink;
} vfs_stat_t;

// ============================================================================
// Mount Point Management
// ============================================================================

#define VFS_MAX_MOUNTS      16

typedef struct {
    bool active;
    fs_type_t type;
    char mount_point[256];  // Local mount point path
    union {
        struct {
            fat_fs_t *fs;
        } fat;
        struct {
            uint32_t server_ip;
            char export_path[256];
        } nfs;
    } source;
} vfs_mount_t;

// ============================================================================
// VFS Initialization
// ============================================================================

// Initialize VFS subsystem
void vfs_init(void);

// Mount a filesystem
// For local: path="/" or drive, type=FS_TYPE_LOCAL
// For NFS:  path="nfs://host/export", type=FS_TYPE_NFS
int vfs_mount(const char *source, const char *mount_point, fs_type_t type);

// Unmount a filesystem
int vfs_unmount(const char *mount_point);

// List all mounts
void vfs_print_mounts(void);

// ============================================================================
// VFS Path Operations
// ============================================================================

// Check if path is on a network filesystem
bool vfs_is_network_path(const char *path);

// Get filesystem type for path
fs_type_t vfs_get_fs_type(const char *path);

// Resolve symbolic links in path
int vfs_realpath(const char *path, char *resolved, size_t size);

// ============================================================================
// VFS File Operations
// ============================================================================

// Open a file
// mode: "r" (read), "w" (write), "rw" (read/write)
// Returns file handle index or -1 on error
int vfs_open(const char *path, const char *mode);

// Close a file
int vfs_close(int fd);

// Read from file
ssize_t vfs_read(int fd, void *buffer, size_t count);

// Write to file
ssize_t vfs_write(int fd, const void *buffer, size_t count);

// Seek in file
int64_t vfs_seek(int fd, int64_t offset, int whence);

#define VFS_SEEK_SET    0
#define VFS_SEEK_CUR    1
#define VFS_SEEK_END    2

// Get file size
int64_t vfs_filesize(int fd);

// Truncate file
int vfs_truncate(int fd, uint64_t length);

// ============================================================================
// VFS Directory Operations
// ============================================================================

// Open directory
int vfs_opendir(const char *path);

// Read directory entry
// Returns 0 on success, -1 on error/end
int vfs_readdir(int dirfd, vfs_entry_t *entry);

// Close directory
int vfs_closedir(int dirfd);

// Create directory
int vfs_mkdir(const char *path, uint32_t mode);

// Remove directory
int vfs_rmdir(const char *path);

// ============================================================================
// VFS File Management
// ============================================================================

// Get file/directory attributes
int vfs_stat(const char *path, vfs_stat_t *stat);

// Create a file
int vfs_create(const char *path, uint32_t mode);

// Delete a file
int vfs_remove(const char *path);

// Rename file or directory
int vfs_rename(const char *oldpath, const char *newpath);

// Copy a file
int vfs_copy(const char *src, const char *dst);

// Create symbolic link
int vfs_symlink(const char *target, const char *linkpath);

// Read symbolic link
int vfs_readlink(const char *path, char *buffer, size_t size);

// Check if file exists
bool vfs_exists(const char *path);

// Check if path is a directory
bool vfs_is_dir(const char *path);

// Check if path is a regular file
bool vfs_is_file(const char *path);

// ============================================================================
// VFS Utility Functions
// ============================================================================

// Read entire file into memory (caller must free)
void *vfs_read_file(const char *path, size_t *size_out);

// Write entire file from memory
int vfs_write_file(const char *path, const void *data, size_t size);

// Get last error message
const char *vfs_strerror(int error);

// ============================================================================
// Network-Specific Operations
// ============================================================================

// List available NFS exports from server
char **vfs_list_nfs_exports(const char *host, int *count);

// Free export list
void vfs_free_export_list(char **exports, int count);

// Get NFS server information
int vfs_nfs_server_info(const char *mount_point, nfs_fsstat3_t *stat);

#endif // NETFS_H
