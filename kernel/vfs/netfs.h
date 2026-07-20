// netfs.h - Network Filesystem VFS Layer for MayteraOS
// Provides unified interface for NFS and SMB network filesystems
#ifndef NETFS_H
#define NETFS_H

#include "../types.h"

// ============================================================================
// Network Filesystem Types
// ============================================================================

typedef enum {
    NETFS_TYPE_NONE = 0,
    NETFS_TYPE_NFS,         // NFS (Network File System)
    NETFS_TYPE_SMB,         // SMB/CIFS (Windows shares)
    NETFS_TYPE_FTP,         // FTP (read-only browsing)
    NETFS_TYPE_WEBDAV       // WebDAV (future)
} netfs_type_t;

// ============================================================================
// Network Mount Information
// ============================================================================

#define NETFS_MAX_MOUNTS        16
#define NETFS_MAX_PATH          1024
#define NETFS_MAX_NAME          256

// Network mount entry
typedef struct {
    bool active;
    netfs_type_t type;
    uint32_t server_ip;
    uint16_t port;
    char server_name[NETFS_MAX_NAME];   // Hostname or IP string
    char share_name[NETFS_MAX_NAME];    // Share/export name
    char mount_point[NETFS_MAX_PATH];   // Local mount point
    char username[64];
    bool authenticated;                  // Is user authenticated?
    bool read_only;                      // Mount read-only?
    int protocol_handle;                 // Handle from NFS/SMB layer
} netfs_mount_t;

// ============================================================================
// Network File Information
// ============================================================================

typedef struct {
    char name[NETFS_MAX_NAME];
    uint64_t size;
    uint64_t creation_time;     // Unix timestamp
    uint64_t modified_time;     // Unix timestamp
    uint64_t access_time;       // Unix timestamp
    uint32_t mode;              // Unix-style permissions
    uint32_t uid;
    uint32_t gid;
    bool is_directory;
    bool is_symlink;
    bool is_hidden;
    bool is_read_only;
} netfs_stat_t;

// ============================================================================
// Network File Handle
// ============================================================================

#define NETFS_MAX_OPEN_FILES    64

typedef struct {
    bool active;
    netfs_mount_t *mount;
    int protocol_fd;            // Handle from NFS/SMB layer
    uint64_t position;
    uint32_t flags;
    char path[NETFS_MAX_PATH];
    bool is_directory;
} netfs_file_t;

// Open flags
#define NETFS_O_RDONLY      0x0001
#define NETFS_O_WRONLY      0x0002
#define NETFS_O_RDWR        0x0003
#define NETFS_O_CREAT       0x0100
#define NETFS_O_EXCL        0x0200
#define NETFS_O_TRUNC       0x0400
#define NETFS_O_APPEND      0x0800

// Seek origins
#define NETFS_SEEK_SET      0
#define NETFS_SEEK_CUR      1
#define NETFS_SEEK_END      2

// ============================================================================
// Network Discovery
// ============================================================================

// Discovered network resource
typedef struct {
    netfs_type_t type;
    uint32_t server_ip;
    char server_name[NETFS_MAX_NAME];
    char share_name[NETFS_MAX_NAME];
    char description[NETFS_MAX_NAME];
    bool requires_auth;
} netfs_resource_t;

#define NETFS_MAX_DISCOVERED    64

// ============================================================================
// VFS Integration API
// ============================================================================

// Initialize network filesystem layer
void netfs_init(void);

// Check if path is a network path
bool netfs_is_network_path(const char *path);

// Get network mount for path (NULL if not mounted)
netfs_mount_t *netfs_get_mount(const char *path);

// ============================================================================
// Mount Operations
// ============================================================================

// Mount a network share using URL
// Formats: nfs://server/export, smb://user:pass@server/share
// Returns mount index on success, -1 on failure
int netfs_mount(const char *url, const char *mount_point);

// Mount with explicit parameters
int netfs_mount_share(netfs_type_t type, uint32_t server_ip, uint16_t port,
                      const char *share_name, const char *mount_point,
                      const char *username, const char *password);

// Unmount a network share
int netfs_unmount(const char *mount_point);

// Get mount information
int netfs_get_mount_info(const char *mount_point, netfs_mount_t *info);

// List all mounts
int netfs_list_mounts(netfs_mount_t *mounts, int max_mounts);

// ============================================================================
// File Operations
// ============================================================================

// Open a file
int netfs_open(const char *path, uint32_t flags);

// Close a file
int netfs_close(int fd);

// Read from file
ssize_t netfs_read(int fd, void *buffer, size_t count);

// Write to file
ssize_t netfs_write(int fd, const void *buffer, size_t count);

// Seek in file
int64_t netfs_seek(int fd, int64_t offset, int whence);

// Get file information
int netfs_stat(const char *path, netfs_stat_t *stat);

// ============================================================================
// Directory Operations
// ============================================================================

// Open directory for reading
int netfs_opendir(const char *path);

// Read directory entry
// Returns 0 on success, 1 on end of directory, -1 on error
int netfs_readdir(int dirfd, netfs_stat_t *entry);

// Close directory
int netfs_closedir(int dirfd);

// Create directory
int netfs_mkdir(const char *path, uint32_t mode);

// Remove directory
int netfs_rmdir(const char *path);

// ============================================================================
// File Management
// ============================================================================

// Delete a file
int netfs_unlink(const char *path);

// Rename a file or directory
int netfs_rename(const char *oldpath, const char *newpath);

// Create symbolic link (NFS only)
int netfs_symlink(const char *target, const char *linkpath);

// Read symbolic link (NFS only)
int netfs_readlink(const char *path, char *buffer, size_t size);

// ============================================================================
// Network Discovery
// ============================================================================

// Scan network for available servers
// Returns number of servers found
int netfs_discover_servers(netfs_resource_t *resources, int max_resources, int timeout_ms);

// List shares/exports on a server
int netfs_list_server_shares(uint32_t server_ip, netfs_type_t type,
                              netfs_resource_t *resources, int max_resources);

// Discover using mDNS/DNS-SD (if available)
int netfs_mdns_discover(netfs_resource_t *resources, int max_resources);

// ============================================================================
// Credentials Management
// ============================================================================

// Saved credential entry
typedef struct {
    uint32_t server_ip;
    char server_name[NETFS_MAX_NAME];
    char username[64];
    char password[64];      // In real implementation, would be encrypted
    netfs_type_t type;
} netfs_credential_t;

#define NETFS_MAX_CREDENTIALS   32

// Save credentials for a server
int netfs_save_credential(const netfs_credential_t *cred);

// Get saved credentials for a server
int netfs_get_credential(uint32_t server_ip, netfs_type_t type, netfs_credential_t *cred);

// Remove saved credentials
int netfs_remove_credential(uint32_t server_ip, netfs_type_t type);

// List saved credentials (passwords masked)
int netfs_list_credentials(netfs_credential_t *creds, int max_creds);

// ============================================================================
// Bookmarks/Favorites
// ============================================================================

// Network location bookmark
typedef struct {
    char name[64];              // Display name
    char url[NETFS_MAX_PATH];   // Full URL (smb://... or nfs://...)
    netfs_type_t type;
    bool auto_mount;            // Auto-mount on startup?
} netfs_bookmark_t;

#define NETFS_MAX_BOOKMARKS     32

// Add bookmark
int netfs_add_bookmark(const netfs_bookmark_t *bookmark);

// Remove bookmark
int netfs_remove_bookmark(const char *name);

// Get bookmark by name
int netfs_get_bookmark(const char *name, netfs_bookmark_t *bookmark);

// List all bookmarks
int netfs_list_bookmarks(netfs_bookmark_t *bookmarks, int max_bookmarks);

// ============================================================================
// Utility Functions
// ============================================================================

// Parse network URL into components
int netfs_parse_url(const char *url, netfs_type_t *type, char *server,
                    char *share, char *username, char *password);

// Build URL from components
int netfs_build_url(netfs_type_t type, const char *server, const char *share,
                    const char *username, char *url, size_t url_size);

// Get human-readable type name
const char *netfs_type_name(netfs_type_t type);

// Convert error code to string
const char *netfs_strerror(int error);

// Get mount point prefix for type (e.g., "/net/smb/", "/net/nfs/")
const char *netfs_mount_prefix(netfs_type_t type);

// Print debug information
void netfs_debug_info(void);

// ============================================================================
// Error Codes
// ============================================================================

#define NETFS_OK                0
#define NETFS_ERR_INVALID       -1
#define NETFS_ERR_NO_MEMORY     -2
#define NETFS_ERR_NOT_FOUND     -3
#define NETFS_ERR_EXISTS        -4
#define NETFS_ERR_ACCESS        -5
#define NETFS_ERR_TIMEOUT       -6
#define NETFS_ERR_NETWORK       -7
#define NETFS_ERR_AUTH          -8
#define NETFS_ERR_NOT_MOUNTED   -9
#define NETFS_ERR_BUSY          -10
#define NETFS_ERR_NOT_DIR       -11
#define NETFS_ERR_IS_DIR        -12
#define NETFS_ERR_NOT_EMPTY     -13
#define NETFS_ERR_IO            -14
#define NETFS_ERR_NOT_SUPPORTED -15

#endif // NETFS_H
