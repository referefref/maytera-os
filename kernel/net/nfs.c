// nfs.c - Network File System (NFS) Client Implementation
// Implements NFSv3 (RFC 1813) for MayteraOS

#include "nfs.h"
#include "rpc.h"
#include "dns.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../serial.h"

// ============================================================================
// Global State
// ============================================================================

static nfs_mount_t nfs_mounts[NFS_MAX_MOUNTS];
static nfs_file_t nfs_files[NFS_MAX_OPEN_FILES];
static bool nfs_initialized = false;
static bool nfs_debug = false;

// task #317 pass 3: net-stack pump primitives (same as net/smb.c / net/rpc.c).
extern void net_poll(void);
extern void tcp_timer(void);
extern void proc_sleep(uint32_t ms);

// Directory handle for readdir operations
typedef struct {
    bool active;
    nfs_mount_t *mount;
    nfs_fh3_t dir_fh;
    uint64_t cookie;
    uint8_t cookieverf[NFS3_COOKIEVERFSIZE];
    bool eof;
    // Current entry list
    nfs_entry3_t *entries;
    nfs_entry3_t *current;
} nfs_dir_t;

static nfs_dir_t nfs_dirs[NFS_MAX_OPEN_FILES];

// ============================================================================
// Helper Functions
// ============================================================================

// Parse IP address from string (e.g., "192.0.2.100")
static uint32_t parse_ip(const char *str) {
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

// Find mount point for a path
static nfs_mount_t *find_mount_for_path(const char *path, const char **relative_path) {
    size_t best_len = 0;
    nfs_mount_t *best_mount = NULL;
    
    for (int i = 0; i < NFS_MAX_MOUNTS; i++) {
        if (!nfs_mounts[i].active) continue;
        
        size_t mp_len = strlen(nfs_mounts[i].mount_point);
        if (strncmp(path, nfs_mounts[i].mount_point, mp_len) == 0) {
            // Check for exact match or path separator
            if (path[mp_len] == '/' || path[mp_len] == '\0') {
                if (mp_len > best_len) {
                    best_len = mp_len;
                    best_mount = &nfs_mounts[i];
                }
            }
        }
    }
    
    if (best_mount && relative_path) {
        *relative_path = path + best_len;
        if (**relative_path == '/') (*relative_path)++;
    }
    
    return best_mount;
}

// Free entry list
static void free_entries(nfs_entry3_t *entries) {
    while (entries) {
        nfs_entry3_t *next = entries->next;
        kfree(entries);
        entries = next;
    }
}

// ============================================================================
// NFS Initialization
// ============================================================================

void nfs_init(void) {
    if (nfs_initialized) return;
    
    memset(nfs_mounts, 0, sizeof(nfs_mounts));
    memset(nfs_files, 0, sizeof(nfs_files));
    memset(nfs_dirs, 0, sizeof(nfs_dirs));
    
    rpc_init();
    
    nfs_initialized = true;
    kprintf("[NFS] Client initialized\n");
}

// ============================================================================
// Mount Protocol
// ============================================================================

// Get the root file handle for an export
static int nfs_mount_get_fh(uint32_t server_ip, const char *export_path, 
                            nfs_fh3_t *root_fh) {
    // Get mount daemon port from portmapper
    int mount_port = rpc_get_port(server_ip, MOUNT_PROGRAM, MOUNT_V3, true);
    if (mount_port <= 0) {
        kprintf("[NFS] Failed to get mount port\n");
        return -1;
    }
    
    if (nfs_debug) {
        kprintf("[NFS] Mount daemon on port %d\n", mount_port);
    }
    
    // Create RPC client for mount protocol
    int client = rpc_create_client(server_ip, MOUNT_PROGRAM, MOUNT_V3, true);
    if (client < 0) return -1;
    
    rpc_set_port(client, (uint16_t)mount_port);
    
    // Call MNT procedure
    xdr_t *xdr = rpc_call_begin(client, MOUNTPROC3_MNT);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    // Encode dirpath (export path)
    char path_buf[NFS_MAXPATHLEN];
    strncpy(path_buf, export_path, sizeof(path_buf) - 1);
    path_buf[sizeof(path_buf) - 1] = '\0';
    xdr_string(xdr, path_buf, NFS_MAXPATHLEN);
    
    // Send and receive
    xdr_t *reply = rpc_call_send(client, 10000);
    if (!reply) {
        kprintf("[NFS] Mount RPC failed\n");
        rpc_destroy_client(client);
        return -1;
    }
    
    // Decode mountres3
    uint32_t status;
    xdr_uint32(reply, &status);
    
    if (status != MNT3_OK) {
        kprintf("[NFS] Mount failed with status %u\n", status);
        rpc_destroy_client(client);
        return -1;
    }
    
    // Decode file handle
    if (!xdr_nfs_fh3(reply, root_fh->data, &root_fh->len)) {
        kprintf("[NFS] Failed to decode file handle\n");
        rpc_destroy_client(client);
        return -1;
    }
    
    // Skip auth flavors
    uint32_t auth_count;
    xdr_uint32(reply, &auth_count);
    for (uint32_t i = 0; i < auth_count; i++) {
        uint32_t flavor;
        xdr_uint32(reply, &flavor);
    }
    
    rpc_destroy_client(client);
    
    if (nfs_debug) {
        kprintf("[NFS] Got root file handle, len=%u\n", root_fh->len);
    }
    
    return 0;
}

// ============================================================================
// Mount/Unmount
// ============================================================================

int nfs_mount(uint32_t server_ip, const char *export_path, const char *mount_point) {
    if (!nfs_initialized) nfs_init();
    
    // Find free mount slot
    int idx = -1;
    for (int i = 0; i < NFS_MAX_MOUNTS; i++) {
        if (!nfs_mounts[i].active) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0) {
        kprintf("[NFS] No free mount slots\n");
        return -1;
    }
    
    nfs_mount_t *mnt = &nfs_mounts[idx];
    memset(mnt, 0, sizeof(nfs_mount_t));
    
    mnt->server_ip = server_ip;
    strncpy(mnt->export_path, export_path, sizeof(mnt->export_path) - 1);
    strncpy(mnt->mount_point, mount_point, sizeof(mnt->mount_point) - 1);
    mnt->tcp_socket = -1;
    mnt->xid = 1;
    
    // Get root file handle
    if (nfs_mount_get_fh(server_ip, export_path, &mnt->root_fh) < 0) {
        kprintf("[NFS] Failed to get root file handle\n");
        return -1;
    }
    
    // Get NFS port (usually 2049)
    int nfs_port = rpc_get_port(server_ip, NFS_PROGRAM, NFS_V3, true);
    if (nfs_port <= 0) {
        // Try default port
        nfs_port = NFS_PORT;
    }
    mnt->nfs_port = (uint16_t)nfs_port;
    
    mnt->active = true;
    
    kprintf("[NFS] Mounted %u.%u.%u.%u:%s on %s (port %d)\n",
                 (server_ip >> 24) & 0xFF,
                 (server_ip >> 16) & 0xFF,
                 (server_ip >> 8) & 0xFF,
                 server_ip & 0xFF,
                 export_path, mount_point, nfs_port);
    
    return idx;
}

int nfs_mount_url(const char *url, const char *mount_point) {
    // Parse nfs://host/export or host:/export format
    char host[128] = {0};
    char export[NFS_MAXPATHLEN] = {0};
    
    const char *p = url;
    
    // Skip nfs:// prefix if present
    if (strncmp(url, "nfs://", 6) == 0) {
        p += 6;
    }
    
    // Extract host (until : or /)
    int i = 0;
    while (*p && *p != ':' && *p != '/' && i < 127) {
        host[i++] = *p++;
    }
    host[i] = '\0';
    
    // Skip : if present
    if (*p == ':') p++;
    
    // Rest is export path
    strncpy(export, p, sizeof(export) - 1);
    
    if (export[0] == '\0') {
        strcpy(export, "/");
    }
    
    // Resolve hostname to IP
    uint32_t server_ip = 0; dns_resolve(host, &server_ip);
    if (server_ip == 0) {
        // Try parsing as IP address
        server_ip = parse_ip(host);
    }
    
    if (server_ip == 0) {
        kprintf("[NFS] Cannot resolve host: %s\n", host);
        return -1;
    }
    
    return nfs_mount(server_ip, export, mount_point);
}

int nfs_unmount(const char *mount_point) {
    for (int i = 0; i < NFS_MAX_MOUNTS; i++) {
        if (nfs_mounts[i].active && 
            strcmp(nfs_mounts[i].mount_point, mount_point) == 0) {
            
            // Close any open files on this mount
            for (int j = 0; j < NFS_MAX_OPEN_FILES; j++) {
                if (nfs_files[j].active && nfs_files[j].mount == &nfs_mounts[i]) {
                    nfs_files[j].active = false;
                }
            }
            
            // TODO: Send UMNT RPC to server
            
            memset(&nfs_mounts[i], 0, sizeof(nfs_mount_t));
            kprintf("[NFS] Unmounted %s\n", mount_point);
            return 0;
        }
    }
    
    kprintf("[NFS] Mount point not found: %s\n", mount_point);
    return -1;
}

nfs_mount_t *nfs_find_mount(const char *path) {
    return find_mount_for_path(path, NULL);
}

// ============================================================================
// NFS Operations Helper
// ============================================================================

// Create an RPC client for NFS operations
static int nfs_create_client(nfs_mount_t *mount) {
    int client = rpc_create_client(mount->server_ip, NFS_PROGRAM, NFS_V3, true);
    if (client < 0) return -1;
    rpc_set_port(client, mount->nfs_port);
    return client;
}

// Lookup a path component
static int nfs_lookup_one(nfs_mount_t *mount, nfs_fh3_t *dir_fh, 
                         const char *name, nfs_fh3_t *result_fh,
                         nfs_fattr3_t *attrs) {
    int client = nfs_create_client(mount);
    if (client < 0) return -1;
    
    xdr_t *xdr = rpc_call_begin(client, NFSPROC3_LOOKUP);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    // Encode LOOKUP3args: dir file handle + name
    xdr_nfs_fh3(xdr, dir_fh->data, &dir_fh->len);
    
    char name_buf[NFS_MAXNAMLEN + 1];
    strncpy(name_buf, name, sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';
    xdr_string(xdr, name_buf, NFS_MAXNAMLEN);
    
    xdr_t *reply = rpc_call_send(client, 5000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    // Decode LOOKUP3res
    uint32_t status;
    xdr_uint32(reply, &status);
    
    if (status != NFS3_OK) {
        rpc_destroy_client(client);
        return -(int)status;
    }
    
    // Decode result file handle
    if (!xdr_nfs_fh3(reply, result_fh->data, &result_fh->len)) {
        rpc_destroy_client(client);
        return -1;
    }
    
    // Decode post_op_attr (optional attributes)
    uint32_t has_attrs;
    xdr_uint32(reply, &has_attrs);
    
    if (has_attrs && attrs) {
        // Decode fattr3
        uint32_t ftype;
        xdr_uint32(reply, &ftype);
        attrs->type = (nfs3_ftype_t)ftype;
        xdr_uint32(reply, &attrs->mode);
        xdr_uint32(reply, &attrs->nlink);
        xdr_uint32(reply, &attrs->uid);
        xdr_uint32(reply, &attrs->gid);
        xdr_uint64(reply, &attrs->size);
        xdr_uint64(reply, &attrs->used);
        xdr_uint32(reply, &attrs->rdev_major);
        xdr_uint32(reply, &attrs->rdev_minor);
        xdr_uint64(reply, &attrs->fsid);
        xdr_uint64(reply, &attrs->fileid);
        xdr_nfs_time3(reply, &attrs->atime.seconds, &attrs->atime.nseconds);
        xdr_nfs_time3(reply, &attrs->mtime.seconds, &attrs->mtime.nseconds);
        xdr_nfs_time3(reply, &attrs->ctime.seconds, &attrs->ctime.nseconds);
    }
    
    rpc_destroy_client(client);
    return 0;
}

// Lookup full path to get file handle
// Forward declaration
static int nfs_getattr_fh(nfs_mount_t *mount, nfs_fh3_t *fh, nfs_fattr3_t *attrs);

static int nfs_lookup_path(nfs_mount_t *mount, const char *path,
                          nfs_fh3_t *result_fh, nfs_fattr3_t *attrs) {
    // Start with root file handle
    memcpy(result_fh, &mount->root_fh, sizeof(nfs_fh3_t));
    
    if (path == NULL || path[0] == '\0') {
        // Root of export
        if (attrs) {
            return nfs_getattr_fh(mount, result_fh, attrs);
        }
        return 0;
    }
    
    // Skip leading slash
    if (*path == '/') path++;
    
    char component[NFS_MAXNAMLEN + 1];
    nfs_fh3_t current_fh;
    memcpy(&current_fh, &mount->root_fh, sizeof(nfs_fh3_t));
    
    while (*path) {
        // Extract next path component
        int i = 0;
        while (*path && *path != '/' && i < NFS_MAXNAMLEN) {
            component[i++] = *path++;
        }
        component[i] = '\0';
        
        if (i == 0) {
            if (*path == '/') path++;
            continue;
        }
        
        // Lookup this component
        int ret = nfs_lookup_one(mount, &current_fh, component, result_fh,
                                (*path == '\0' || path[1] == '\0') ? attrs : NULL);
        if (ret < 0) {
            return ret;
        }
        
        memcpy(&current_fh, result_fh, sizeof(nfs_fh3_t));
        
        if (*path == '/') path++;
    }
    
    return 0;
}

// Get attributes for file handle
static int nfs_getattr_fh(nfs_mount_t *mount, nfs_fh3_t *fh, nfs_fattr3_t *attrs) {
    int client = nfs_create_client(mount);
    if (client < 0) return -1;
    
    xdr_t *xdr = rpc_call_begin(client, NFSPROC3_GETATTR);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    xdr_nfs_fh3(xdr, fh->data, &fh->len);
    
    xdr_t *reply = rpc_call_send(client, 5000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    uint32_t status;
    xdr_uint32(reply, &status);
    
    if (status != NFS3_OK) {
        rpc_destroy_client(client);
        return -(int)status;
    }
    
    // Decode fattr3
    uint32_t ftype;
    xdr_uint32(reply, &ftype);
    attrs->type = (nfs3_ftype_t)ftype;
    xdr_uint32(reply, &attrs->mode);
    xdr_uint32(reply, &attrs->nlink);
    xdr_uint32(reply, &attrs->uid);
    xdr_uint32(reply, &attrs->gid);
    xdr_uint64(reply, &attrs->size);
    xdr_uint64(reply, &attrs->used);
    xdr_uint32(reply, &attrs->rdev_major);
    xdr_uint32(reply, &attrs->rdev_minor);
    xdr_uint64(reply, &attrs->fsid);
    xdr_uint64(reply, &attrs->fileid);
    xdr_nfs_time3(reply, &attrs->atime.seconds, &attrs->atime.nseconds);
    xdr_nfs_time3(reply, &attrs->mtime.seconds, &attrs->mtime.nseconds);
    xdr_nfs_time3(reply, &attrs->ctime.seconds, &attrs->ctime.nseconds);
    
    rpc_destroy_client(client);
    return 0;
}

// ============================================================================
// File Operations
// ============================================================================

int nfs_open(const char *path, uint32_t flags) {
    const char *rel_path;
    nfs_mount_t *mount = find_mount_for_path(path, &rel_path);
    
    if (!mount) {
        kprintf("[NFS] No mount for path: %s\n", path);
        return -1;
    }
    
    // Find free file slot
    int fd = -1;
    for (int i = 0; i < NFS_MAX_OPEN_FILES; i++) {
        if (!nfs_files[i].active) {
            fd = i;
            break;
        }
    }
    
    if (fd < 0) {
        kprintf("[NFS] No free file slots\n");
        return -1;
    }
    
    nfs_file_t *file = &nfs_files[fd];
    memset(file, 0, sizeof(nfs_file_t));
    
    // Lookup file
    int ret = nfs_lookup_path(mount, rel_path, &file->fh, &file->attrs);
    
    if (ret < 0 && (flags & NFS_O_CREAT)) {
        // File doesn't exist, create it
        ret = nfs_create(path, 0644);
        if (ret < 0) return ret;
        
        // Lookup again
        ret = nfs_lookup_path(mount, rel_path, &file->fh, &file->attrs);
    }
    
    if (ret < 0) {
        return ret;
    }
    
    file->active = true;
    file->mount = mount;
    file->position = 0;
    file->flags = flags;
    strncpy(file->path, path, sizeof(file->path) - 1);
    
    // Handle truncate
    if (flags & NFS_O_TRUNC) {
        nfs_sattr3_t sattr = {0};
        sattr.set_size = true;
        sattr.size = 0;
        nfs_setattr(path, &sattr);
        file->attrs.size = 0;
    }
    
    // Handle append
    if (flags & NFS_O_APPEND) {
        file->position = file->attrs.size;
    }
    
    if (nfs_debug) {
        kprintf("[NFS] Opened %s as fd %d (size=%llu)\n", 
                     path, fd, file->attrs.size);
    }
    
    return fd;
}

int nfs_close(int fd) {
    if (fd < 0 || fd >= NFS_MAX_OPEN_FILES) return -1;
    if (!nfs_files[fd].active) return -1;
    
    nfs_files[fd].active = false;
    return 0;
}

ssize_t nfs_read(int fd, void *buffer, size_t count) {
    if (fd < 0 || fd >= NFS_MAX_OPEN_FILES) return -1;
    if (!nfs_files[fd].active) return -1;
    
    nfs_file_t *file = &nfs_files[fd];
    
    // Check EOF
    if (file->position >= file->attrs.size) {
        return 0;
    }
    
    // Limit read to file size
    if (file->position + count > file->attrs.size) {
        count = file->attrs.size - file->position;
    }
    
    int client = nfs_create_client(file->mount);
    if (client < 0) return -1;
    
    xdr_t *xdr = rpc_call_begin(client, NFSPROC3_READ);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    // Encode READ3args
    xdr_nfs_fh3(xdr, file->fh.data, &file->fh.len);
    xdr_uint64(xdr, &file->position);
    uint32_t count32 = (count > NFS_READ_SIZE) ? NFS_READ_SIZE : (uint32_t)count;
    xdr_uint32(xdr, &count32);
    
    xdr_t *reply = rpc_call_send(client, 10000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    uint32_t status;
    xdr_uint32(reply, &status);
    
    if (status != NFS3_OK) {
        rpc_destroy_client(client);
        return -(int)status;
    }
    
    // Skip post_op_attr
    uint32_t has_attrs;
    xdr_uint32(reply, &has_attrs);
    if (has_attrs) {
        xdr_skip(reply, 84);  // Size of fattr3
    }
    
    // Decode READ3resok
    xdr_uint32(reply, &count32);  // count
    uint32_t eof;
    xdr_uint32(reply, &eof);
    
    // Decode data
    uint32_t data_len;
    xdr_uint32(reply, &data_len);
    // MAYTERA-SEC-2026-0012 (CWE-787, remote OOB write): a malicious/compromised
    // NFS server can return data_len far larger than the count we requested;
    // xdr_opaque only bounds the SOURCE read against reply->size, so an unclamped
    // data_len over-WRITES the caller destination `buffer` (which is `count`
    // bytes; count was clamped to the file size above). Clamp the delivered
    // length to the destination capacity before the copy. ASan-proven; the
    // source-bounded XDR (Rust or C) does NOT confine this destination write.
    // Plain-C hardening ticket #509.
    if (data_len > count) {
        data_len = (uint32_t)count;
    }
    if (data_len > 0) {
        xdr_opaque(reply, buffer, data_len);
    }

    file->position += data_len;
    
    rpc_destroy_client(client);
    
    return (ssize_t)data_len;
}

ssize_t nfs_write(int fd, const void *buffer, size_t count) {
    if (fd < 0 || fd >= NFS_MAX_OPEN_FILES) return -1;
    if (!nfs_files[fd].active) return -1;
    
    nfs_file_t *file = &nfs_files[fd];
    
    if ((file->flags & NFS_O_RDWR) == 0 && (file->flags & NFS_O_WRONLY) == 0) {
        return -1;  // Not opened for writing
    }
    
    int client = nfs_create_client(file->mount);
    if (client < 0) return -1;
    
    xdr_t *xdr = rpc_call_begin(client, NFSPROC3_WRITE);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    // Encode WRITE3args
    xdr_nfs_fh3(xdr, file->fh.data, &file->fh.len);
    xdr_uint64(xdr, &file->position);
    
    uint32_t count32 = (count > NFS_WRITE_SIZE) ? NFS_WRITE_SIZE : (uint32_t)count;
    xdr_uint32(xdr, &count32);  // count
    
    // stable = FILE_SYNC (2) for synchronous writes
    uint32_t stable = 2;
    xdr_uint32(xdr, &stable);
    
    // Encode data
    xdr_uint32(xdr, &count32);  // data length
    xdr_opaque(xdr, (uint8_t *)buffer, count32);
    
    xdr_t *reply = rpc_call_send(client, 10000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    uint32_t status;
    xdr_uint32(reply, &status);
    
    if (status != NFS3_OK) {
        rpc_destroy_client(client);
        return -(int)status;
    }
    
    // Decode WRITE3resok. It begins with file_wcc (wcc_data), which the
    // original code failed to skip - so it read the pre_op_attr value_follows
    // flag as the written count and corrupted the position. (task #317 pass 3)
    //   wcc_data = pre_op_attr  : value_follows(4) [+ wcc_attr 24 if set]
    //              post_op_attr : value_follows(4) [+ fattr3   84 if set]
    {
        uint32_t vf;
        xdr_uint32(reply, &vf);                 // pre_op_attr value_follows
        if (vf) xdr_skip(reply, 24);            // wcc_attr (size8+mtime8+ctime8)
        xdr_uint32(reply, &vf);                 // post_op_attr value_follows
        if (vf) xdr_skip(reply, 84);            // fattr3
    }

    // WRITE3resok: count, committed, verf
    uint32_t written;
    xdr_uint32(reply, &written);
    
    file->position += written;
    if (file->position > file->attrs.size) {
        file->attrs.size = file->position;
    }
    
    rpc_destroy_client(client);
    
    return (ssize_t)written;
}

int64_t nfs_seek(int fd, int64_t offset, int whence) {
    if (fd < 0 || fd >= NFS_MAX_OPEN_FILES) return -1;
    if (!nfs_files[fd].active) return -1;
    
    nfs_file_t *file = &nfs_files[fd];
    int64_t new_pos;
    
    switch (whence) {
        case NFS_SEEK_SET:
            new_pos = offset;
            break;
        case NFS_SEEK_CUR:
            new_pos = (int64_t)file->position + offset;
            break;
        case NFS_SEEK_END:
            new_pos = (int64_t)file->attrs.size + offset;
            break;
        default:
            return -1;
    }
    
    if (new_pos < 0) return -1;
    
    file->position = (uint64_t)new_pos;
    return new_pos;
}

int nfs_getattr(const char *path, nfs_fattr3_t *attrs) {
    const char *rel_path;
    nfs_mount_t *mount = find_mount_for_path(path, &rel_path);
    
    if (!mount) return -1;
    
    nfs_fh3_t fh;
    return nfs_lookup_path(mount, rel_path, &fh, attrs);
}

int nfs_setattr(const char *path, nfs_sattr3_t *sattr) {
    const char *rel_path;
    nfs_mount_t *mount = find_mount_for_path(path, &rel_path);
    
    if (!mount) return -1;
    
    // Get file handle
    nfs_fh3_t fh;
    if (nfs_lookup_path(mount, rel_path, &fh, NULL) < 0) {
        return -1;
    }
    
    int client = nfs_create_client(mount);
    if (client < 0) return -1;
    
    xdr_t *xdr = rpc_call_begin(client, NFSPROC3_SETATTR);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    // Encode SETATTR3args
    xdr_nfs_fh3(xdr, fh.data, &fh.len);
    
    // Encode sattr3
    xdr_bool(xdr, &sattr->set_mode);
    if (sattr->set_mode) xdr_uint32(xdr, &sattr->mode);
    
    xdr_bool(xdr, &sattr->set_uid);
    if (sattr->set_uid) xdr_uint32(xdr, &sattr->uid);
    
    xdr_bool(xdr, &sattr->set_gid);
    if (sattr->set_gid) xdr_uint32(xdr, &sattr->gid);
    
    xdr_bool(xdr, &sattr->set_size);
    if (sattr->set_size) xdr_uint64(xdr, &sattr->size);
    
    // atime and mtime set_it enum: DONT_CHANGE=0, SET_TO_SERVER_TIME=1, SET_TO_CLIENT_TIME=2
    uint32_t time_how = sattr->set_atime ? 2 : 0;
    xdr_uint32(xdr, &time_how);
    if (sattr->set_atime) {
        xdr_nfs_time3(xdr, &sattr->atime.seconds, &sattr->atime.nseconds);
    }
    
    time_how = sattr->set_mtime ? 2 : 0;
    xdr_uint32(xdr, &time_how);
    if (sattr->set_mtime) {
        xdr_nfs_time3(xdr, &sattr->mtime.seconds, &sattr->mtime.nseconds);
    }
    
    // sattrguard3: check = false
    bool guard = false;
    xdr_bool(xdr, &guard);
    
    xdr_t *reply = rpc_call_send(client, 5000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    uint32_t status;
    xdr_uint32(reply, &status);
    
    rpc_destroy_client(client);
    return (status == NFS3_OK) ? 0 : -(int)status;
}

// ============================================================================
// Directory Operations
// ============================================================================

int nfs_opendir(const char *path) {
    const char *rel_path;
    nfs_mount_t *mount = find_mount_for_path(path, &rel_path);
    
    if (!mount) return -1;
    
    // Find free dir slot
    int dirfd = -1;
    for (int i = 0; i < NFS_MAX_OPEN_FILES; i++) {
        if (!nfs_dirs[i].active) {
            dirfd = i;
            break;
        }
    }
    
    if (dirfd < 0) return -1;
    
    nfs_dir_t *dir = &nfs_dirs[dirfd];
    memset(dir, 0, sizeof(nfs_dir_t));
    
    // Lookup directory
    nfs_fattr3_t attrs;
    if (nfs_lookup_path(mount, rel_path, &dir->dir_fh, &attrs) < 0) {
        return -1;
    }
    
    // Verify it's a directory
    if (attrs.type != NF3DIR) {
        return -1;
    }
    
    dir->active = true;
    dir->mount = mount;
    dir->cookie = 0;
    dir->eof = false;
    dir->entries = NULL;
    dir->current = NULL;
    
    return dirfd;
}

static int nfs_fetch_dir_entries(nfs_dir_t *dir) {
    // Free old entries
    free_entries(dir->entries);
    dir->entries = NULL;
    dir->current = NULL;
    
    if (dir->eof) return 0;
    
    int client = nfs_create_client(dir->mount);
    if (client < 0) return -1;
    
    xdr_t *xdr = rpc_call_begin(client, NFSPROC3_READDIR);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    // Encode READDIR3args
    xdr_nfs_fh3(xdr, dir->dir_fh.data, &dir->dir_fh.len);
    xdr_uint64(xdr, &dir->cookie);
    xdr_opaque(xdr, dir->cookieverf, NFS3_COOKIEVERFSIZE);
    uint32_t count = NFS_READDIR_SIZE;
    xdr_uint32(xdr, &count);
    
    xdr_t *reply = rpc_call_send(client, 10000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    uint32_t status;
    xdr_uint32(reply, &status);
    
    if (status != NFS3_OK) {
        rpc_destroy_client(client);
        return -(int)status;
    }
    
    // Skip dir_attributes (post_op_attr)
    uint32_t has_attrs;
    xdr_uint32(reply, &has_attrs);
    if (has_attrs) {
        xdr_skip(reply, 84);  // Size of fattr3
    }
    
    // Decode cookieverf
    xdr_opaque(reply, dir->cookieverf, NFS3_COOKIEVERFSIZE);
    
    // Decode entries
    nfs_entry3_t *tail = NULL;
    
    while (1) {
        uint32_t value_follows;
        xdr_uint32(reply, &value_follows);
        
        if (!value_follows) break;
        
        nfs_entry3_t *entry = kzalloc(sizeof(nfs_entry3_t));
        if (!entry) break;
        
        xdr_uint64(reply, &entry->fileid);
        xdr_string(reply, entry->name, NFS_MAXNAMLEN);
        xdr_uint64(reply, &entry->cookie);
        
        entry->next = NULL;
        
        if (tail) {
            tail->next = entry;
        } else {
            dir->entries = entry;
        }
        tail = entry;
        
        dir->cookie = entry->cookie;
    }
    
    // Check EOF
    xdr_bool(reply, &dir->eof);
    
    dir->current = dir->entries;
    
    rpc_destroy_client(client);
    return 0;
}

nfs_entry3_t *nfs_readdir(int dirfd) {
    if (dirfd < 0 || dirfd >= NFS_MAX_OPEN_FILES) return NULL;
    if (!nfs_dirs[dirfd].active) return NULL;
    
    nfs_dir_t *dir = &nfs_dirs[dirfd];
    
    // Fetch more entries if needed
    if (dir->current == NULL && !dir->eof) {
        if (nfs_fetch_dir_entries(dir) < 0) {
            return NULL;
        }
    }
    
    if (dir->current == NULL) {
        return NULL;  // No more entries
    }
    
    nfs_entry3_t *entry = dir->current;
    dir->current = dir->current->next;
    
    return entry;
}

int nfs_closedir(int dirfd) {
    if (dirfd < 0 || dirfd >= NFS_MAX_OPEN_FILES) return -1;
    if (!nfs_dirs[dirfd].active) return -1;
    
    nfs_dir_t *dir = &nfs_dirs[dirfd];
    free_entries(dir->entries);
    memset(dir, 0, sizeof(nfs_dir_t));
    
    return 0;
}

int nfs_mkdir(const char *path, uint32_t mode) {
    // Split path into parent directory and new directory name
    char parent[NFS_MAXPATHLEN];
    char name[NFS_MAXNAMLEN + 1];
    
    strncpy(parent, path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    
    // Find last /
    char *last_slash = strrchr(parent, '/');
    if (!last_slash) {
        strcpy(name, parent);
        strcpy(parent, "/");
    } else {
        strncpy(name, last_slash + 1, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        if (last_slash == parent) {
            parent[1] = '\0';
        } else {
            *last_slash = '\0';
        }
    }
    
    const char *rel_path;
    nfs_mount_t *mount = find_mount_for_path(parent, &rel_path);
    if (!mount) return -1;
    
    // Get parent directory file handle
    nfs_fh3_t parent_fh;
    if (nfs_lookup_path(mount, rel_path, &parent_fh, NULL) < 0) {
        return -1;
    }
    
    int client = nfs_create_client(mount);
    if (client < 0) return -1;
    
    xdr_t *xdr = rpc_call_begin(client, NFSPROC3_MKDIR);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    // Encode MKDIR3args
    xdr_nfs_fh3(xdr, parent_fh.data, &parent_fh.len);
    xdr_string(xdr, name, NFS_MAXNAMLEN);
    
    // Encode sattr3
    bool set_mode = true;
    xdr_bool(xdr, &set_mode);
    xdr_uint32(xdr, &mode);
    
    bool no = false;
    xdr_bool(xdr, &no);  // set_uid = false
    xdr_bool(xdr, &no);  // set_gid = false
    xdr_bool(xdr, &no);  // set_size = false
    
    uint32_t time_how = 1;  // SET_TO_SERVER_TIME
    xdr_uint32(xdr, &time_how);  // atime
    xdr_uint32(xdr, &time_how);  // mtime
    
    xdr_t *reply = rpc_call_send(client, 5000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    uint32_t status;
    xdr_uint32(reply, &status);
    
    rpc_destroy_client(client);
    return (status == NFS3_OK) ? 0 : -(int)status;
}

int nfs_rmdir(const char *path) {
    char parent[NFS_MAXPATHLEN];
    char name[NFS_MAXNAMLEN + 1];
    
    strncpy(parent, path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    
    char *last_slash = strrchr(parent, '/');
    if (!last_slash) {
        strcpy(name, parent);
        strcpy(parent, "/");
    } else {
        strncpy(name, last_slash + 1, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        if (last_slash == parent) {
            parent[1] = '\0';
        } else {
            *last_slash = '\0';
        }
    }
    
    const char *rel_path;
    nfs_mount_t *mount = find_mount_for_path(parent, &rel_path);
    if (!mount) return -1;
    
    nfs_fh3_t parent_fh;
    if (nfs_lookup_path(mount, rel_path, &parent_fh, NULL) < 0) {
        return -1;
    }
    
    int client = nfs_create_client(mount);
    if (client < 0) return -1;
    
    xdr_t *xdr = rpc_call_begin(client, NFSPROC3_RMDIR);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    xdr_nfs_fh3(xdr, parent_fh.data, &parent_fh.len);
    xdr_string(xdr, name, NFS_MAXNAMLEN);
    
    xdr_t *reply = rpc_call_send(client, 5000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    uint32_t status;
    xdr_uint32(reply, &status);
    
    rpc_destroy_client(client);
    return (status == NFS3_OK) ? 0 : -(int)status;
}

// ============================================================================
// File Management
// ============================================================================

int nfs_create(const char *path, uint32_t mode) {
    char parent[NFS_MAXPATHLEN];
    char name[NFS_MAXNAMLEN + 1];
    
    strncpy(parent, path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    
    char *last_slash = strrchr(parent, '/');
    if (!last_slash) {
        strcpy(name, parent);
        strcpy(parent, "/");
    } else {
        strncpy(name, last_slash + 1, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        if (last_slash == parent) {
            parent[1] = '\0';
        } else {
            *last_slash = '\0';
        }
    }
    
    const char *rel_path;
    nfs_mount_t *mount = find_mount_for_path(parent, &rel_path);
    if (!mount) return -1;
    
    nfs_fh3_t parent_fh;
    if (nfs_lookup_path(mount, rel_path, &parent_fh, NULL) < 0) {
        return -1;
    }
    
    int client = nfs_create_client(mount);
    if (client < 0) return -1;
    
    xdr_t *xdr = rpc_call_begin(client, NFSPROC3_CREATE);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    // Encode CREATE3args
    xdr_nfs_fh3(xdr, parent_fh.data, &parent_fh.len);
    xdr_string(xdr, name, NFS_MAXNAMLEN);
    
    // createhow3: UNCHECKED=0, GUARDED=1, EXCLUSIVE=2
    uint32_t how = 0;  // UNCHECKED
    xdr_uint32(xdr, &how);
    
    // sattr3 for UNCHECKED/GUARDED
    bool set_mode = true;
    xdr_bool(xdr, &set_mode);
    xdr_uint32(xdr, &mode);
    
    bool no = false;
    xdr_bool(xdr, &no);  // set_uid
    xdr_bool(xdr, &no);  // set_gid
    
    bool set_size = true;
    uint64_t zero = 0;
    xdr_bool(xdr, &set_size);
    xdr_uint64(xdr, &zero);  // size = 0
    
    uint32_t time_how = 1;  // SET_TO_SERVER_TIME
    xdr_uint32(xdr, &time_how);  // atime
    xdr_uint32(xdr, &time_how);  // mtime
    
    xdr_t *reply = rpc_call_send(client, 5000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    uint32_t status;
    xdr_uint32(reply, &status);
    
    rpc_destroy_client(client);
    return (status == NFS3_OK) ? 0 : -(int)status;
}

int nfs_remove(const char *path) {
    char parent[NFS_MAXPATHLEN];
    char name[NFS_MAXNAMLEN + 1];
    
    strncpy(parent, path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    
    char *last_slash = strrchr(parent, '/');
    if (!last_slash) {
        strcpy(name, parent);
        strcpy(parent, "/");
    } else {
        strncpy(name, last_slash + 1, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        if (last_slash == parent) {
            parent[1] = '\0';
        } else {
            *last_slash = '\0';
        }
    }
    
    const char *rel_path;
    nfs_mount_t *mount = find_mount_for_path(parent, &rel_path);
    if (!mount) return -1;
    
    nfs_fh3_t parent_fh;
    if (nfs_lookup_path(mount, rel_path, &parent_fh, NULL) < 0) {
        return -1;
    }
    
    int client = nfs_create_client(mount);
    if (client < 0) return -1;
    
    xdr_t *xdr = rpc_call_begin(client, NFSPROC3_REMOVE);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    xdr_nfs_fh3(xdr, parent_fh.data, &parent_fh.len);
    xdr_string(xdr, name, NFS_MAXNAMLEN);
    
    xdr_t *reply = rpc_call_send(client, 5000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    uint32_t status;
    xdr_uint32(reply, &status);
    
    rpc_destroy_client(client);
    return (status == NFS3_OK) ? 0 : -(int)status;
}

int nfs_rename(const char *oldpath, const char *newpath) {
    // Extract parent directories and names
    char old_parent[NFS_MAXPATHLEN], old_name[NFS_MAXNAMLEN + 1];
    char new_parent[NFS_MAXPATHLEN], new_name[NFS_MAXNAMLEN + 1];
    
    strncpy(old_parent, oldpath, sizeof(old_parent) - 1);
    char *slash = strrchr(old_parent, '/');
    if (slash) {
        strncpy(old_name, slash + 1, sizeof(old_name) - 1);
        *slash = '\0';
    } else {
        strcpy(old_name, old_parent);
        strcpy(old_parent, "/");
    }
    
    strncpy(new_parent, newpath, sizeof(new_parent) - 1);
    slash = strrchr(new_parent, '/');
    if (slash) {
        strncpy(new_name, slash + 1, sizeof(new_name) - 1);
        *slash = '\0';
    } else {
        strcpy(new_name, new_parent);
        strcpy(new_parent, "/");
    }
    
    const char *old_rel, *new_rel;
    nfs_mount_t *old_mount = find_mount_for_path(old_parent, &old_rel);
    nfs_mount_t *new_mount = find_mount_for_path(new_parent, &new_rel);
    
    if (!old_mount || !new_mount || old_mount != new_mount) {
        return -1;  // Cross-mount rename not supported
    }
    
    nfs_fh3_t old_fh, new_fh;
    if (nfs_lookup_path(old_mount, old_rel, &old_fh, NULL) < 0) return -1;
    if (nfs_lookup_path(new_mount, new_rel, &new_fh, NULL) < 0) return -1;
    
    int client = nfs_create_client(old_mount);
    if (client < 0) return -1;
    
    xdr_t *xdr = rpc_call_begin(client, NFSPROC3_RENAME);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    // Encode RENAME3args: from_dir, from_name, to_dir, to_name
    xdr_nfs_fh3(xdr, old_fh.data, &old_fh.len);
    xdr_string(xdr, old_name, NFS_MAXNAMLEN);
    xdr_nfs_fh3(xdr, new_fh.data, &new_fh.len);
    xdr_string(xdr, new_name, NFS_MAXNAMLEN);
    
    xdr_t *reply = rpc_call_send(client, 5000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    uint32_t status;
    xdr_uint32(reply, &status);
    
    rpc_destroy_client(client);
    return (status == NFS3_OK) ? 0 : -(int)status;
}

int nfs_symlink(const char *target, const char *linkpath) {
    char parent[NFS_MAXPATHLEN];
    char name[NFS_MAXNAMLEN + 1];
    
    strncpy(parent, linkpath, sizeof(parent) - 1);
    char *last_slash = strrchr(parent, '/');
    if (last_slash) {
        strncpy(name, last_slash + 1, sizeof(name) - 1);
        *last_slash = '\0';
    } else {
        strcpy(name, parent);
        strcpy(parent, "/");
    }
    
    const char *rel_path;
    nfs_mount_t *mount = find_mount_for_path(parent, &rel_path);
    if (!mount) return -1;
    
    nfs_fh3_t parent_fh;
    if (nfs_lookup_path(mount, rel_path, &parent_fh, NULL) < 0) return -1;
    
    int client = nfs_create_client(mount);
    if (client < 0) return -1;
    
    xdr_t *xdr = rpc_call_begin(client, NFSPROC3_SYMLINK);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    // Encode SYMLINK3args
    xdr_nfs_fh3(xdr, parent_fh.data, &parent_fh.len);
    xdr_string(xdr, name, NFS_MAXNAMLEN);
    
    // symlink_data: sattr3 + path_data
    // sattr3 (all defaults)
    bool no = false;
    xdr_bool(xdr, &no);  // set_mode
    xdr_bool(xdr, &no);  // set_uid
    xdr_bool(xdr, &no);  // set_gid
    xdr_bool(xdr, &no);  // set_size
    uint32_t time_how = 0;  // DONT_CHANGE
    xdr_uint32(xdr, &time_how);  // atime
    xdr_uint32(xdr, &time_how);  // mtime
    
    // symlink target path
    char target_buf[NFS_MAXPATHLEN];
    strncpy(target_buf, target, sizeof(target_buf) - 1);
    xdr_string(xdr, target_buf, NFS_MAXPATHLEN);
    
    xdr_t *reply = rpc_call_send(client, 5000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    uint32_t status;
    xdr_uint32(reply, &status);
    
    rpc_destroy_client(client);
    return (status == NFS3_OK) ? 0 : -(int)status;
}

int nfs_readlink(const char *path, char *buffer, size_t size) {
    const char *rel_path;
    nfs_mount_t *mount = find_mount_for_path(path, &rel_path);
    if (!mount) return -1;
    
    nfs_fh3_t fh;
    nfs_fattr3_t attrs;
    if (nfs_lookup_path(mount, rel_path, &fh, &attrs) < 0) return -1;
    
    if (attrs.type != NF3LNK) return -1;
    
    int client = nfs_create_client(mount);
    if (client < 0) return -1;
    
    xdr_t *xdr = rpc_call_begin(client, NFSPROC3_READLINK);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    xdr_nfs_fh3(xdr, fh.data, &fh.len);
    
    xdr_t *reply = rpc_call_send(client, 5000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    uint32_t status;
    xdr_uint32(reply, &status);
    
    if (status != NFS3_OK) {
        rpc_destroy_client(client);
        return -(int)status;
    }
    
    // Skip post_op_attr
    uint32_t has_attrs;
    xdr_uint32(reply, &has_attrs);
    if (has_attrs) xdr_skip(reply, 84);
    
    // Decode path
    xdr_string(reply, buffer, (uint32_t)size);
    
    rpc_destroy_client(client);
    return 0;
}

int nfs_link(const char *existing, const char *new_path) {
    const char *exist_rel;
    nfs_mount_t *exist_mount = find_mount_for_path(existing, &exist_rel);
    if (!exist_mount) return -1;
    
    nfs_fh3_t exist_fh;
    if (nfs_lookup_path(exist_mount, exist_rel, &exist_fh, NULL) < 0) return -1;
    
    char parent[NFS_MAXPATHLEN];
    char name[NFS_MAXNAMLEN + 1];
    
    strncpy(parent, new_path, sizeof(parent) - 1);
    char *last_slash = strrchr(parent, '/');
    if (last_slash) {
        strncpy(name, last_slash + 1, sizeof(name) - 1);
        *last_slash = '\0';
    } else {
        strcpy(name, parent);
        strcpy(parent, "/");
    }
    
    const char *parent_rel;
    nfs_mount_t *parent_mount = find_mount_for_path(parent, &parent_rel);
    if (!parent_mount || parent_mount != exist_mount) return -1;
    
    nfs_fh3_t parent_fh;
    if (nfs_lookup_path(parent_mount, parent_rel, &parent_fh, NULL) < 0) return -1;
    
    int client = nfs_create_client(exist_mount);
    if (client < 0) return -1;
    
    xdr_t *xdr = rpc_call_begin(client, NFSPROC3_LINK);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    // Encode LINK3args: file, link (dir + name)
    xdr_nfs_fh3(xdr, exist_fh.data, &exist_fh.len);
    xdr_nfs_fh3(xdr, parent_fh.data, &parent_fh.len);
    xdr_string(xdr, name, NFS_MAXNAMLEN);
    
    xdr_t *reply = rpc_call_send(client, 5000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    uint32_t status;
    xdr_uint32(reply, &status);
    
    rpc_destroy_client(client);
    return (status == NFS3_OK) ? 0 : -(int)status;
}

// ============================================================================
// Filesystem Operations
// ============================================================================

int nfs_fsinfo(const char *path, nfs_fsinfo3_t *info) {
    const char *rel_path;
    nfs_mount_t *mount = find_mount_for_path(path, &rel_path);
    if (!mount) return -1;
    
    int client = nfs_create_client(mount);
    if (client < 0) return -1;
    
    xdr_t *xdr = rpc_call_begin(client, NFSPROC3_FSINFO);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    xdr_nfs_fh3(xdr, mount->root_fh.data, &mount->root_fh.len);
    
    xdr_t *reply = rpc_call_send(client, 5000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    uint32_t status;
    xdr_uint32(reply, &status);
    
    if (status != NFS3_OK) {
        rpc_destroy_client(client);
        return -(int)status;
    }
    
    // Skip post_op_attr
    uint32_t has_attrs;
    xdr_uint32(reply, &has_attrs);
    if (has_attrs) xdr_skip(reply, 84);
    
    // Decode FSINFO3resok
    xdr_uint32(reply, &info->rtmax);
    xdr_uint32(reply, &info->rtpref);
    xdr_uint32(reply, &info->rtmult);
    xdr_uint32(reply, &info->wtmax);
    xdr_uint32(reply, &info->wtpref);
    xdr_uint32(reply, &info->wtmult);
    xdr_uint32(reply, &info->dtpref);
    xdr_uint64(reply, &info->maxfilesize);
    xdr_nfs_time3(reply, &info->time_delta.seconds, &info->time_delta.nseconds);
    xdr_uint32(reply, &info->properties);
    
    rpc_destroy_client(client);
    return 0;
}

int nfs_fsstat(const char *path, nfs_fsstat3_t *stat) {
    const char *rel_path;
    nfs_mount_t *mount = find_mount_for_path(path, &rel_path);
    if (!mount) return -1;
    
    int client = nfs_create_client(mount);
    if (client < 0) return -1;
    
    xdr_t *xdr = rpc_call_begin(client, NFSPROC3_FSSTAT);
    if (!xdr) {
        rpc_destroy_client(client);
        return -1;
    }
    
    xdr_nfs_fh3(xdr, mount->root_fh.data, &mount->root_fh.len);
    
    xdr_t *reply = rpc_call_send(client, 5000);
    if (!reply) {
        rpc_destroy_client(client);
        return -1;
    }
    
    uint32_t status;
    xdr_uint32(reply, &status);
    
    if (status != NFS3_OK) {
        rpc_destroy_client(client);
        return -(int)status;
    }
    
    // Skip post_op_attr
    uint32_t has_attrs;
    xdr_uint32(reply, &has_attrs);
    if (has_attrs) xdr_skip(reply, 84);
    
    // Decode FSSTAT3resok
    xdr_uint64(reply, &stat->tbytes);
    xdr_uint64(reply, &stat->fbytes);
    xdr_uint64(reply, &stat->abytes);
    xdr_uint64(reply, &stat->tfiles);
    xdr_uint64(reply, &stat->ffiles);
    xdr_uint64(reply, &stat->afiles);
    xdr_uint32(reply, &stat->invarsec);
    
    rpc_destroy_client(client);
    return 0;
}

// ============================================================================
// Utility Functions
// ============================================================================

const char *nfs_strerror(nfs3_status_t status) {
    switch (status) {
        case NFS3_OK:             return "Success";
        case NFS3ERR_PERM:        return "Not owner";
        case NFS3ERR_NOENT:       return "No such file or directory";
        case NFS3ERR_IO:          return "I/O error";
        case NFS3ERR_NXIO:        return "No such device";
        case NFS3ERR_ACCES:       return "Permission denied";
        case NFS3ERR_EXIST:       return "File exists";
        case NFS3ERR_XDEV:        return "Cross-device link";
        case NFS3ERR_NODEV:       return "No such device";
        case NFS3ERR_NOTDIR:      return "Not a directory";
        case NFS3ERR_ISDIR:       return "Is a directory";
        case NFS3ERR_INVAL:       return "Invalid argument";
        case NFS3ERR_FBIG:        return "File too large";
        case NFS3ERR_NOSPC:       return "No space left on device";
        case NFS3ERR_ROFS:        return "Read-only file system";
        case NFS3ERR_MLINK:       return "Too many hard links";
        case NFS3ERR_NAMETOOLONG: return "Filename too long";
        case NFS3ERR_NOTEMPTY:    return "Directory not empty";
        case NFS3ERR_DQUOT:       return "Disk quota exceeded";
        case NFS3ERR_STALE:       return "Stale file handle";
        case NFS3ERR_REMOTE:      return "Too many levels of remote in path";
        case NFS3ERR_BADHANDLE:   return "Illegal NFS file handle";
        case NFS3ERR_NOT_SYNC:    return "Update synchronization mismatch";
        case NFS3ERR_BAD_COOKIE:  return "Bad cookie for READDIR";
        case NFS3ERR_NOTSUPP:     return "Operation not supported";
        case NFS3ERR_TOOSMALL:    return "Buffer too small";
        case NFS3ERR_SERVERFAULT: return "Server fault";
        case NFS3ERR_BADTYPE:     return "Type not supported";
        case NFS3ERR_JUKEBOX:     return "Request queued (try again)";
        default:                   return "Unknown NFS error";
    }
}

void nfs_print_attrs(const nfs_fattr3_t *attrs) {
    const char *type_str;
    switch (attrs->type) {
        case NF3REG:  type_str = "REG";  break;
        case NF3DIR:  type_str = "DIR";  break;
        case NF3BLK:  type_str = "BLK";  break;
        case NF3CHR:  type_str = "CHR";  break;
        case NF3LNK:  type_str = "LNK";  break;
        case NF3SOCK: type_str = "SOCK"; break;
        case NF3FIFO: type_str = "FIFO"; break;
        default:      type_str = "???";  break;
    }
    
    kprintf("  type=%s mode=%04o nlink=%u uid=%u gid=%u\n",
                 type_str, attrs->mode, attrs->nlink, attrs->uid, attrs->gid);
    kprintf("  size=%llu used=%llu fileid=%llu\n",
                 attrs->size, attrs->used, attrs->fileid);
}

bool nfs_is_nfs_path(const char *path) {
    return find_mount_for_path(path, NULL) != NULL;
}

void nfs_print_mounts(void) {
    kprintf("[NFS] Active mounts:\n");
    for (int i = 0; i < NFS_MAX_MOUNTS; i++) {
        if (nfs_mounts[i].active) {
            kprintf("  %s -> %u.%u.%u.%u:%s\n",
                         nfs_mounts[i].mount_point,
                         (nfs_mounts[i].server_ip >> 24) & 0xFF,
                         (nfs_mounts[i].server_ip >> 16) & 0xFF,
                         (nfs_mounts[i].server_ip >> 8) & 0xFF,
                         nfs_mounts[i].server_ip & 0xFF,
                         nfs_mounts[i].export_path);
        }
    }
}

// ============================================================================
// Export Listing
// ============================================================================

char **nfs_list_exports(uint32_t server_ip, int *count) {
    *count = 0;
    
    // Get mount daemon port
    int mount_port = rpc_get_port(server_ip, MOUNT_PROGRAM, MOUNT_V3, true);
    if (mount_port <= 0) return NULL;
    
    int client = rpc_create_client(server_ip, MOUNT_PROGRAM, MOUNT_V3, true);
    if (client < 0) return NULL;
    
    rpc_set_port(client, (uint16_t)mount_port);
    
    xdr_t *xdr = rpc_call_begin(client, MOUNTPROC3_EXPORT);
    if (!xdr) {
        rpc_destroy_client(client);
        return NULL;
    }
    
    xdr_t *reply = rpc_call_send(client, 10000);
    if (!reply) {
        rpc_destroy_client(client);
        return NULL;
    }
    
    // Count exports first
    int num_exports = 0;
    size_t save_pos = xdr_getpos(reply);
    
    while (1) {
        uint32_t value_follows;
        if (!xdr_uint32(reply, &value_follows)) break;
        if (!value_follows) break;
        
        // Skip export entry (dir + groups)
        char dir[NFS_MAXPATHLEN];
        xdr_string(reply, dir, NFS_MAXPATHLEN);
        
        // Skip groups list
        while (1) {
            uint32_t grp_follows;
            if (!xdr_uint32(reply, &grp_follows)) break;
            if (!grp_follows) break;
            char grp[64];
            xdr_string(reply, grp, 64);
        }
        
        num_exports++;
    }
    
    if (num_exports == 0) {
        rpc_destroy_client(client);
        return NULL;
    }
    
    // Allocate array
    char **exports = kmalloc(sizeof(char *) * num_exports);
    if (!exports) {
        rpc_destroy_client(client);
        return NULL;
    }
    
    // Reset and read again
    xdr_setpos(reply, save_pos);
    int idx = 0;
    
    while (idx < num_exports) {
        uint32_t value_follows;
        if (!xdr_uint32(reply, &value_follows) || !value_follows) break;
        
        char dir[NFS_MAXPATHLEN];
        xdr_string(reply, dir, NFS_MAXPATHLEN);
        
        exports[idx] = kmalloc(strlen(dir) + 1);
        if (exports[idx]) {
            strcpy(exports[idx], dir);
        }
        idx++;
        
        // Skip groups
        while (1) {
            uint32_t grp_follows;
            if (!xdr_uint32(reply, &grp_follows) || !grp_follows) break;
            char grp[64];
            xdr_string(reply, grp, 64);
        }
    }
    
    *count = idx;
    rpc_destroy_client(client);
    return exports;
}

void nfs_free_exports(char **exports, int count) {
    if (!exports) return;
    for (int i = 0; i < count; i++) {
        if (exports[i]) kfree(exports[i]);
    }
    kfree(exports);
}

// ===========================================================================
// task #317 pass 3: VFS integration for "/NFS/<server>/<label>[/<relpath>]"
// ---------------------------------------------------------------------------
// Unlike SMB shares (single-token names), NFS exports are server-side absolute
// paths (e.g. "/srv/share"), so an /NFS path cannot encode the export inline.
// Instead an NFS mount maps a local mount-point /NFS/<server>/<label> (label =
// the export's last path component) to (server_ip, export_path). The mount must
// be established explicitly first (boot self-test, NETMOUNTS.CFG at boot, or the
// Files "Network" menu via SYS_NET_MOUNT) - there is no lazy auto-mount because
// the export path is unknown from the /NFS path alone. find_mount_for_path()
// then resolves the longest matching mount-point prefix and the remainder is the
// path within the export.
// ===========================================================================

#include "../fs/fat.h"
extern fat_fs_t g_fat_fs;

bool nfs_vfs_is_nfs_path(const char *path) {
    return path && (strncmp(path, "/NFS/", 5) == 0 || strncmp(path, "/nfs/", 5) == 0);
}

// Derive the local mount-point /NFS/<serverstr>/<label> for (server_ip, export).
static void nfs_make_mount_point(const char *server_str, const char *export_path,
                                 char *out, int outsz) {
    // label = basename of export_path (last non-empty path component).
    const char *base = export_path;
    for (const char *p = export_path; *p; p++) {
        if (*p == '/' && p[1] != '\0' && p[1] != '/') base = p + 1;
    }
    char label[128]; int li = 0;
    while (base[li] && base[li] != '/' && li < (int)sizeof(label) - 1) {
        label[li] = base[li]; li++;
    }
    label[li] = 0;
    if (li == 0) { label[0] = 'r'; label[1] = 'o'; label[2] = 'o'; label[3] = 't'; label[4] = 0; }
    int n = 0;
    const char *pfx = "/NFS/";
    while (pfx[n] && n < outsz - 1) { out[n] = pfx[n]; n++; }
    int k = 0;
    while (server_str[k] && n < outsz - 1) out[n++] = server_str[k++];
    if (n < outsz - 1) out[n++] = '/';
    k = 0;
    while (label[k] && n < outsz - 1) out[n++] = label[k++];
    out[n] = 0;
}

// Establish (and cache) an NFS mount; writes the resulting /NFS/<server>/<label>
// mount-point into mp_out (so callers can navigate). Returns 0 / -1.
int nfs_vfs_mount(const char *server_str, const char *export_path,
                  char *mp_out, int mp_outsz) {
    if (!nfs_initialized) nfs_init();
    if (!server_str || !export_path || !server_str[0] || !export_path[0]) return -1;

    uint32_t ip = 0;
    dns_resolve(server_str, &ip);
    if (ip == 0) ip = parse_ip(server_str);
    if (ip == 0) return -1;

    char mp[NFS_MAXPATHLEN];
    nfs_make_mount_point(server_str, export_path, mp, sizeof(mp));
    if (mp_out && mp_outsz > 0) {
        int i = 0; while (mp[i] && i < mp_outsz - 1) { mp_out[i] = mp[i]; i++; } mp_out[i] = 0;
    }
    if (nfs_find_mount(mp)) return 0;   // already mounted

    // Warm the net stack / ARP so the first portmap SYN goes out with a known MAC.
    for (int i = 0; i < 100; i++) { net_poll(); tcp_timer(); proc_sleep(1); }

    int idx = nfs_mount(ip, export_path, mp);
    return (idx >= 0) ? 0 : -1;
}

// Ensure a mount exists for an /NFS path (it must already be mounted). 0 / -1.
int nfs_vfs_ensure_mount(const char *path) {
    return nfs_find_mount(path) ? 0 : -1;
}

// Whole-file read, fat_read_file-shaped (kmalloc'd, NUL-terminated buffer).
void *nfs_vfs_read_whole(const char *path, uint32_t *size_out) {
    if (size_out) *size_out = 0;
    if (!nfs_find_mount(path)) return NULL;

    int fd = nfs_open(path, NFS_O_RDONLY);
    if (fd < 0) return NULL;

    nfs_file_t *f = &nfs_files[fd];
    uint64_t fsize64 = f->attrs.size;
    if (fsize64 > 0xF0000000ULL) { nfs_close(fd); return NULL; }
    uint32_t cap = (uint32_t)fsize64;

    uint8_t *buf = (uint8_t *)kmalloc(cap + 1);
    if (!buf) { nfs_close(fd); return NULL; }

    uint32_t got = 0;
    while (got < cap) {
        uint32_t want = cap - got;
        if (want > NFS_READ_SIZE) want = NFS_READ_SIZE;
        ssize_t r = nfs_read(fd, buf + got, want);
        if (r <= 0) break;
        got += (uint32_t)r;
    }
    buf[got] = 0;
    nfs_close(fd);
    if (size_out) *size_out = got;
    return buf;
}

// Whole-file write/upload (create+truncate, stream in NFS_WRITE_SIZE chunks).
int nfs_vfs_write_whole(const char *path, const void *data, uint32_t len) {
    const char *rel;
    nfs_mount_t *mount = find_mount_for_path(path, &rel);
    if (!mount) return -1;

    // Create the file if absent, then open RW + truncate.
    nfs_fh3_t fh; nfs_fattr3_t at;
    if (nfs_lookup_path(mount, rel, &fh, &at) < 0) {
        if (nfs_create(path, 0644) < 0) return -1;
    }
    int fd = nfs_open(path, NFS_O_RDWR | NFS_O_TRUNC);
    if (fd < 0) return -1;

    const uint8_t *p = (const uint8_t *)data;
    uint32_t off = 0; int ok = 1;
    while (off < len) {
        uint32_t want = len - off;
        if (want > NFS_WRITE_SIZE) want = NFS_WRITE_SIZE;
        ssize_t w = nfs_write(fd, p + off, want);
        if (w <= 0) { ok = 0; break; }
        off += (uint32_t)w;
    }
    nfs_close(fd);
    return ok ? 0 : -1;
}

// ===========================================================================
// Boot self-test (task #317 pass 3) - gated on /CONFIG/NFSTEST.CFG.
// Format (one key=value per line):
//   ip=192.0.2.219
//   export=/srv/share
//   file=TEST.TXT
// Safe to leave compiled in: does nothing when the config file is absent.
// ===========================================================================

static void nfstest_get(const char *buf, const char *key, char *out, int outsz) {
    out[0] = 0;
    int klen = strlen(key);
    const char *p = buf;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            int i = 0;
            while (*p && *p != '\n' && *p != '\r' && i < outsz - 1) out[i++] = *p++;
            out[i] = 0;
            return;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
}

void nfs_run_selftest(void) {
    uint32_t cfgsz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/NFSTEST.CFG", &cfgsz);
    if (!cfg) return;  // no config -> silent

    char ipstr[64], expt[256], file[128];
    nfstest_get(cfg, "ip", ipstr, sizeof(ipstr));
    nfstest_get(cfg, "export", expt, sizeof(expt));
    nfstest_get(cfg, "file", file, sizeof(file));
    kfree(cfg);

    if (!ipstr[0] || !expt[0]) {
        kprintf("[NFSTEST] config present but missing ip/export\n");
        return;
    }
    if (!file[0]) strcpy(file, "TEST.TXT");

    kprintf("\n========== NFS SELFTEST (task #317 pass 3) ==========\n");
    kprintf("[NFSTEST] target=%s export=%s file=%s\n", ipstr, expt, file);

    nfs_init();
    for (int i = 0; i < 200; i++) { net_poll(); tcp_timer(); proc_sleep(1); }

    // 1) MOUNT: portmap -> mountd MNT (root fh) -> portmap NFS port.
    char mp[NFS_MAXPATHLEN];
    if (nfs_vfs_mount(ipstr, expt, mp, sizeof(mp)) != 0) {
        kprintf("[NFSTEST] mount FAILED\n========== NFS SELFTEST: FAIL ==========\n");
        return;
    }
    kprintf("[NFSTEST] MOUNT OK at %s\n", mp);

    // 2) READDIR the export root.
    kprintf("[NFSTEST] --- directory listing of %s ---\n", mp);
    int nent = 0;
    int dir = nfs_opendir(mp);
    if (dir >= 0) {
        nfs_entry3_t *e;
        while ((e = nfs_readdir(dir)) != NULL && nent < 128) {
            kprintf("[NFSTEST]   %s\n", e->name);
            nent++;
        }
        nfs_closedir(dir);
        kprintf("[NFSTEST] enumerated %d entries\n", nent);
    } else {
        kprintf("[NFSTEST] opendir FAILED\n");
    }

    // 3) READ the test file via nfs_open/nfs_read.
    char fpath[NFS_MAXPATHLEN];
    snprintf(fpath, sizeof(fpath), "%s/%s", mp, file);
    kprintf("[NFSTEST] --- reading %s ---\n", fpath);
    int read_ok = 0;
    int fd = nfs_open(fpath, NFS_O_RDONLY);
    if (fd >= 0) {
        static uint8_t fbuf[4096];
        ssize_t got = nfs_read(fd, fbuf, sizeof(fbuf) - 1);
        if (got > 0) {
            fbuf[got] = 0;
            kprintf("[NFSTEST] read %d bytes:\n----8<----\n%s\n----8<----\n", (int)got, (char *)fbuf);
            read_ok = 1;
        } else {
            kprintf("[NFSTEST] read returned %d\n", (int)got);
        }
        nfs_close(fd);
    } else {
        kprintf("[NFSTEST] open FAILED\n");
    }

    // 4) WRITE/UPLOAD a file, then read it back.
    int write_ok = 0;
    {
        char wpath[NFS_MAXPATHLEN];
        snprintf(wpath, sizeof(wpath), "%s/P3NFS.TXT", mp);
        const char *payload = "MayteraOS task #317 pass-3 NFSv3 upload OK\n";
        kprintf("[NFSTEST] --- WRITE %s ---\n", wpath);
        if (nfs_vfs_write_whole(wpath, payload, (uint32_t)strlen(payload)) == 0) {
            uint32_t rb = 0;
            char *back = (char *)nfs_vfs_read_whole(wpath, &rb);
            if (back) {
                kprintf("[NFSTEST] readback %u bytes:\n----8<----\n%s----8<----\n", rb, back);
                if (rb == (uint32_t)strlen(payload)) write_ok = 1;
                kfree(back);
            } else {
                kprintf("[NFSTEST] readback FAILED\n");
            }
        } else {
            kprintf("[NFSTEST] write FAILED\n");
        }
    }

    if (read_ok && nent > 0 && write_ok) {
        kprintf("========== NFS SELFTEST: PASS ==========\n\n");
    } else {
        kprintf("========== NFS SELFTEST: FAIL ==========\n\n");
    }
}
