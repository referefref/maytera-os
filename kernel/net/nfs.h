// nfs.h - Network File System (NFS) Protocol Definitions
// Implements NFSv3 (RFC 1813) with optional NFSv4 support
#ifndef NFS_H
#define NFS_H

#include "../types.h"

// ============================================================================
// NFS Version and Port Definitions
// ============================================================================

#define NFS_PORT            2049    // Standard NFS port
#define NFS_PORTMAP_PORT    111     // Portmapper/rpcbind port

// NFS program numbers
#define NFS_PROGRAM         100003  // NFS program
#define MOUNT_PROGRAM       100005  // Mount program
#define PORTMAP_PROGRAM     100000  // Portmapper program

// NFS versions
#define NFS_V2              2
#define NFS_V3              3
#define NFS_V4              4

// Mount versions
#define MOUNT_V1            1
#define MOUNT_V3            3

// ============================================================================
// NFSv3 Procedure Numbers
// ============================================================================

typedef enum {
    NFSPROC3_NULL        = 0,
    NFSPROC3_GETATTR     = 1,
    NFSPROC3_SETATTR     = 2,
    NFSPROC3_LOOKUP      = 3,
    NFSPROC3_ACCESS      = 4,
    NFSPROC3_READLINK    = 5,
    NFSPROC3_READ        = 6,
    NFSPROC3_WRITE       = 7,
    NFSPROC3_CREATE      = 8,
    NFSPROC3_MKDIR       = 9,
    NFSPROC3_SYMLINK     = 10,
    NFSPROC3_MKNOD       = 11,
    NFSPROC3_REMOVE      = 12,
    NFSPROC3_RMDIR       = 13,
    NFSPROC3_RENAME      = 14,
    NFSPROC3_LINK        = 15,
    NFSPROC3_READDIR     = 16,
    NFSPROC3_READDIRPLUS = 17,
    NFSPROC3_FSSTAT      = 18,
    NFSPROC3_FSINFO      = 19,
    NFSPROC3_PATHCONF    = 20,
    NFSPROC3_COMMIT      = 21
} nfs3_proc_t;

// ============================================================================
// Mount Protocol Procedure Numbers
// ============================================================================

typedef enum {
    MOUNTPROC3_NULL      = 0,
    MOUNTPROC3_MNT       = 1,
    MOUNTPROC3_DUMP      = 2,
    MOUNTPROC3_UMNT      = 3,
    MOUNTPROC3_UMNTALL   = 4,
    MOUNTPROC3_EXPORT    = 5
} mount3_proc_t;

// ============================================================================
// NFS Status Codes (NFSv3)
// ============================================================================

typedef enum {
    NFS3_OK             = 0,
    NFS3ERR_PERM        = 1,
    NFS3ERR_NOENT       = 2,
    NFS3ERR_IO          = 5,
    NFS3ERR_NXIO        = 6,
    NFS3ERR_ACCES       = 13,
    NFS3ERR_EXIST       = 17,
    NFS3ERR_XDEV        = 18,
    NFS3ERR_NODEV       = 19,
    NFS3ERR_NOTDIR      = 20,
    NFS3ERR_ISDIR       = 21,
    NFS3ERR_INVAL       = 22,
    NFS3ERR_FBIG        = 27,
    NFS3ERR_NOSPC       = 28,
    NFS3ERR_ROFS        = 30,
    NFS3ERR_MLINK       = 31,
    NFS3ERR_NAMETOOLONG = 63,
    NFS3ERR_NOTEMPTY    = 66,
    NFS3ERR_DQUOT       = 69,
    NFS3ERR_STALE       = 70,
    NFS3ERR_REMOTE      = 71,
    NFS3ERR_BADHANDLE   = 10001,
    NFS3ERR_NOT_SYNC    = 10002,
    NFS3ERR_BAD_COOKIE  = 10003,
    NFS3ERR_NOTSUPP     = 10004,
    NFS3ERR_TOOSMALL    = 10005,
    NFS3ERR_SERVERFAULT = 10006,
    NFS3ERR_BADTYPE     = 10007,
    NFS3ERR_JUKEBOX     = 10008
} nfs3_status_t;

// ============================================================================
// Mount Protocol Status Codes
// ============================================================================

typedef enum {
    MNT3_OK             = 0,
    MNT3ERR_PERM        = 1,
    MNT3ERR_NOENT       = 2,
    MNT3ERR_IO          = 5,
    MNT3ERR_ACCES       = 13,
    MNT3ERR_NOTDIR      = 20,
    MNT3ERR_INVAL       = 22,
    MNT3ERR_NAMETOOLONG = 63,
    MNT3ERR_NOTSUPP     = 10004,
    MNT3ERR_SERVERFAULT = 10006
} mount3_status_t;

// ============================================================================
// NFS File Types
// ============================================================================

typedef enum {
    NF3REG  = 1,    // Regular file
    NF3DIR  = 2,    // Directory
    NF3BLK  = 3,    // Block device
    NF3CHR  = 4,    // Character device
    NF3LNK  = 5,    // Symbolic link
    NF3SOCK = 6,    // Socket
    NF3FIFO = 7     // Named pipe
} nfs3_ftype_t;

// ============================================================================
// NFS Access Permission Bits
// ============================================================================

#define NFS3_ACCESS_READ    0x0001  // Read data/list directory
#define NFS3_ACCESS_LOOKUP  0x0002  // Lookup in directory
#define NFS3_ACCESS_MODIFY  0x0004  // Modify existing file data
#define NFS3_ACCESS_EXTEND  0x0008  // Write to new areas
#define NFS3_ACCESS_DELETE  0x0010  // Delete file/directory entry
#define NFS3_ACCESS_EXECUTE 0x0020  // Execute file

// ============================================================================
// NFS Size Limits
// ============================================================================

#define NFS3_FHSIZE         64      // Max file handle size
#define NFS3_COOKIEVERFSIZE 8       // Cookie verifier size
#define NFS3_CREATEVERFSIZE 8       // Create verifier size
#define NFS3_WRITEVERFSIZE  8       // Write verifier size

#define NFS_MAXNAMLEN       255     // Max filename length
#define NFS_MAXPATHLEN      1024    // Max path length

// Buffer sizes
#define NFS_READ_SIZE       8192    // Default read size
#define NFS_WRITE_SIZE      8192    // Default write size
#define NFS_READDIR_SIZE    8192    // Default readdir size

// ============================================================================
// NFS File Handle
// ============================================================================

typedef struct {
    uint32_t len;                   // Length of file handle data
    uint8_t  data[NFS3_FHSIZE];     // File handle opaque data
} nfs_fh3_t;

// ============================================================================
// NFS Time Structure (NFSv3)
// ============================================================================

typedef struct {
    uint32_t seconds;       // Seconds since Jan 1, 1970
    uint32_t nseconds;      // Nanoseconds
} nfs_time3_t;

// ============================================================================
// NFS File Attributes (NFSv3)
// ============================================================================

typedef struct {
    nfs3_ftype_t type;      // File type
    uint32_t mode;          // Protection mode bits
    uint32_t nlink;         // Number of hard links
    uint32_t uid;           // Owner user ID
    uint32_t gid;           // Owner group ID
    uint64_t size;          // File size in bytes
    uint64_t used;          // Bytes used on disk
    uint32_t rdev_major;    // Device ID (for special files)
    uint32_t rdev_minor;
    uint64_t fsid;          // Filesystem ID
    uint64_t fileid;        // File ID (inode number)
    nfs_time3_t atime;      // Last access time
    nfs_time3_t mtime;      // Last modification time
    nfs_time3_t ctime;      // Last status change time
} nfs_fattr3_t;

// ============================================================================
// NFS Set Attributes Structure
// ============================================================================

// Which attributes to set
typedef struct {
    bool set_mode;
    uint32_t mode;
    bool set_uid;
    uint32_t uid;
    bool set_gid;
    uint32_t gid;
    bool set_size;
    uint64_t size;
    bool set_atime;
    nfs_time3_t atime;
    bool set_mtime;
    nfs_time3_t mtime;
} nfs_sattr3_t;

// ============================================================================
// NFS Directory Entry (for READDIR)
// ============================================================================

typedef struct nfs_entry3 {
    uint64_t fileid;                // File ID
    char name[NFS_MAXNAMLEN + 1];   // Entry name
    uint64_t cookie;                // Cookie for next entry
    struct nfs_entry3 *next;        // Next entry in list
} nfs_entry3_t;

// Extended entry with attributes (for READDIRPLUS)
typedef struct nfs_entryplus3 {
    uint64_t fileid;
    char name[NFS_MAXNAMLEN + 1];
    uint64_t cookie;
    bool attrs_valid;
    nfs_fattr3_t attrs;
    bool fh_valid;
    nfs_fh3_t fh;
    struct nfs_entryplus3 *next;
} nfs_entryplus3_t;

// ============================================================================
// NFS Filesystem Info (FSINFO response)
// ============================================================================

typedef struct {
    uint32_t rtmax;         // Max read transfer size
    uint32_t rtpref;        // Preferred read transfer size
    uint32_t rtmult;        // Read transfer size multiple
    uint32_t wtmax;         // Max write transfer size
    uint32_t wtpref;        // Preferred write transfer size
    uint32_t wtmult;        // Write transfer size multiple
    uint32_t dtpref;        // Preferred READDIR request size
    uint64_t maxfilesize;   // Maximum file size
    nfs_time3_t time_delta; // Server time granularity
    uint32_t properties;    // Filesystem properties
} nfs_fsinfo3_t;

// FSINFO properties flags
#define FSF3_LINK       0x0001  // Hard links supported
#define FSF3_SYMLINK    0x0002  // Symbolic links supported
#define FSF3_HOMOGENEOUS 0x0008 // PATHCONF is the same for all files
#define FSF3_CANSETTIME 0x0010  // Server can set file times

// ============================================================================
// NFS Filesystem Statistics (FSSTAT response)
// ============================================================================

typedef struct {
    uint64_t tbytes;        // Total filesystem bytes
    uint64_t fbytes;        // Free bytes
    uint64_t abytes;        // Available bytes to user
    uint64_t tfiles;        // Total file slots
    uint64_t ffiles;        // Free file slots
    uint64_t afiles;        // Available file slots to user
    uint32_t invarsec;      // How long attributes are valid
} nfs_fsstat3_t;

// ============================================================================
// NFS Client State
// ============================================================================

// Maximum concurrent NFS mounts
#define NFS_MAX_MOUNTS      8

// NFS mount state
typedef struct {
    bool active;                    // Is this mount active?
    uint32_t server_ip;             // Server IP address
    uint16_t nfs_port;              // NFS port (usually 2049)
    uint16_t mount_port;            // Mount service port
    char export_path[NFS_MAXPATHLEN]; // Export path on server
    char mount_point[NFS_MAXPATHLEN]; // Local mount point
    nfs_fh3_t root_fh;              // Root file handle
    nfs_fsinfo3_t fsinfo;           // Filesystem info
    int tcp_socket;                 // TCP socket (or -1 for UDP)
    uint32_t xid;                   // Transaction ID counter
    bool nfs_v4;                    // Using NFSv4?
} nfs_mount_t;

// ============================================================================
// NFS Open File Handle
// ============================================================================

typedef struct {
    bool active;
    nfs_mount_t *mount;             // Associated mount
    nfs_fh3_t fh;                   // File handle
    nfs_fattr3_t attrs;             // Cached attributes
    uint64_t position;              // Current read/write position
    uint32_t flags;                 // Open flags
    char path[NFS_MAXPATHLEN];      // Path for debugging
} nfs_file_t;

#define NFS_MAX_OPEN_FILES  32

// Open flags
#define NFS_O_RDONLY    0x0001
#define NFS_O_WRONLY    0x0002
#define NFS_O_RDWR      0x0003
#define NFS_O_CREAT     0x0100
#define NFS_O_EXCL      0x0200
#define NFS_O_TRUNC     0x0400
#define NFS_O_APPEND    0x0800

// ============================================================================
// NFSv4 Definitions (Basic Support)
// ============================================================================

// NFSv4 operation codes
typedef enum {
    OP_ACCESS           = 3,
    OP_CLOSE            = 4,
    OP_COMMIT           = 5,
    OP_CREATE           = 6,
    OP_DELEGPURGE       = 7,
    OP_DELEGRETURN      = 8,
    OP_GETATTR          = 9,
    OP_GETFH            = 10,
    OP_LINK             = 11,
    OP_LOCK             = 12,
    OP_LOCKT            = 13,
    OP_LOCKU            = 14,
    OP_LOOKUP           = 15,
    OP_LOOKUPP          = 16,
    OP_NVERIFY          = 17,
    OP_OPEN             = 18,
    OP_OPENATTR         = 19,
    OP_OPEN_CONFIRM     = 20,
    OP_OPEN_DOWNGRADE   = 21,
    OP_PUTFH            = 22,
    OP_PUTPUBFH         = 23,
    OP_PUTROOTFH        = 24,
    OP_READ             = 25,
    OP_READDIR          = 26,
    OP_READLINK         = 27,
    OP_REMOVE           = 28,
    OP_RENAME           = 29,
    OP_RENEW            = 30,
    OP_RESTOREFH        = 31,
    OP_SAVEFH           = 32,
    OP_SECINFO          = 33,
    OP_SETATTR          = 34,
    OP_SETCLIENTID      = 35,
    OP_SETCLIENTID_CONFIRM = 36,
    OP_VERIFY           = 37,
    OP_WRITE            = 38,
    OP_RELEASE_LOCKOWNER = 39
} nfs4_op_t;

// ============================================================================
// NFS Client API
// ============================================================================

// Initialize NFS client subsystem
void nfs_init(void);

// Mount an NFS export
// Returns mount index on success, -1 on failure
int nfs_mount(uint32_t server_ip, const char *export_path, const char *mount_point);

// Mount using URL (nfs://host/export)
int nfs_mount_url(const char *url, const char *mount_point);

// Unmount an NFS filesystem
int nfs_unmount(const char *mount_point);

// Find mount by mount point
nfs_mount_t *nfs_find_mount(const char *path);

// List available exports from server (showmount -e equivalent)
// Returns array of export paths, sets count. Caller must free.
char **nfs_list_exports(uint32_t server_ip, int *count);

// Free export list
void nfs_free_exports(char **exports, int count);

// ============================================================================
// NFS File Operations
// ============================================================================

// Open a file
// Returns file index on success, -1 on failure
int nfs_open(const char *path, uint32_t flags);

// Close a file
int nfs_close(int fd);

// Read from file
// Returns bytes read, -1 on error
ssize_t nfs_read(int fd, void *buffer, size_t count);

// Write to file
// Returns bytes written, -1 on error
ssize_t nfs_write(int fd, const void *buffer, size_t count);

// Seek to position
// Returns new position, -1 on error
int64_t nfs_seek(int fd, int64_t offset, int whence);

#define NFS_SEEK_SET    0
#define NFS_SEEK_CUR    1
#define NFS_SEEK_END    2

// Get file attributes
int nfs_getattr(const char *path, nfs_fattr3_t *attrs);

// Set file attributes
int nfs_setattr(const char *path, nfs_sattr3_t *attrs);

// ============================================================================
// NFS Directory Operations
// ============================================================================

// Open a directory for reading
// Returns directory handle index, -1 on failure
int nfs_opendir(const char *path);

// Read next directory entry
// Returns entry or NULL if no more entries
nfs_entry3_t *nfs_readdir(int dirfd);

// Close directory
int nfs_closedir(int dirfd);

// Create a directory
int nfs_mkdir(const char *path, uint32_t mode);

// Remove a directory
int nfs_rmdir(const char *path);

// ============================================================================
// NFS File Management
// ============================================================================

// Create a file
int nfs_create(const char *path, uint32_t mode);

// Remove a file
int nfs_remove(const char *path);

// Rename a file or directory
int nfs_rename(const char *oldpath, const char *newpath);

// Create a symbolic link
int nfs_symlink(const char *target, const char *linkpath);

// Read a symbolic link
int nfs_readlink(const char *path, char *buffer, size_t size);

// Create a hard link
int nfs_link(const char *existing, const char *new_path);

// ============================================================================
// NFS Filesystem Operations
// ============================================================================

// Get filesystem info
int nfs_fsinfo(const char *path, nfs_fsinfo3_t *info);

// Get filesystem statistics
int nfs_fsstat(const char *path, nfs_fsstat3_t *stat);

// ============================================================================
// NFS Utility Functions
// ============================================================================

// Convert NFS error to string
const char *nfs_strerror(nfs3_status_t status);

// Print NFS file attributes
void nfs_print_attrs(const nfs_fattr3_t *attrs);

// Check if path is on an NFS mount
bool nfs_is_nfs_path(const char *path);

// Get mount info for debugging
void nfs_print_mounts(void);

// ============================================================================
// task #317 pass 3: VFS integration ("/NFS/<server>/<label>[/<relpath>]")
// ============================================================================

// True if path is under the /NFS/ namespace.
bool nfs_vfs_is_nfs_path(const char *path);

// Establish (and cache) an NFS mount of <export> on <server>. Writes the
// resulting /NFS/<server>/<label> mount-point into mp_out. Returns 0 / -1.
int nfs_vfs_mount(const char *server_str, const char *export_path,
                  char *mp_out, int mp_outsz);

// 0 if an NFS mount already covers `path` (must be mounted first), else -1.
int nfs_vfs_ensure_mount(const char *path);

// Whole-file read (kmalloc'd NUL-terminated buffer), fat_read_file-shaped.
void *nfs_vfs_read_whole(const char *path, uint32_t *size_out);

// Whole-file create/overwrite + upload. Returns 0 / -1.
int nfs_vfs_write_whole(const char *path, const void *data, uint32_t len);

// Boot self-test, gated on /CONFIG/NFSTEST.CFG.
void nfs_run_selftest(void);

#endif // NFS_H
