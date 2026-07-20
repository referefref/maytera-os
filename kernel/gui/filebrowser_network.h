// filebrowser_network.h - Network filesystem integration for MayteraOS File Browser
#ifndef FILEBROWSER_NETWORK_H
#define FILEBROWSER_NETWORK_H

#include "filebrowser.h"
#include "../vfs/netfs.h"

// ============================================================================
// Network Path Constants
// ============================================================================

#define FB_NETWORK_PATH     "/net"
#define FB_NETWORK_SMB_PATH "/net/smb"
#define FB_NETWORK_NFS_PATH "/net/nfs"

// ============================================================================
// Network Location Management
// ============================================================================

// Network location type for sidebar
typedef enum {
    NET_LOC_ROOT,           // Network root (/net)
    NET_LOC_SMB_ROOT,       // SMB root (/net/smb)
    NET_LOC_NFS_ROOT,       // NFS root (/net/nfs)
    NET_LOC_MOUNTED_SHARE,  // A mounted network share
    NET_LOC_DISCOVERED      // A discovered but unmounted share
} net_location_type_t;

// Network location entry
typedef struct {
    net_location_type_t type;
    char name[64];
    char path[256];
    uint32_t server_ip;
    bool mounted;
} net_location_t;

// ============================================================================
// API Functions
// ============================================================================

// Initialize network filesystem support for file browser
void filebrowser_network_init(void);

// Check if path is a network path
bool fb_is_network_path(const char *path);

// Navigate to a network location
int fb_navigate_network(filebrowser_t *fb, const char *path);

// Extended navigate that handles both local and network paths
int filebrowser_navigate_extended(filebrowser_t *fb, const char *path);

// Show "Connect to Server" dialog
void filebrowser_connect_to_server(filebrowser_t *fb);

// Get network locations for sidebar display
// Returns number of locations filled
int fb_get_network_locations(net_location_t *locations, int max_locations);

// ============================================================================
// Network File Operations
// ============================================================================

// Open file (network-aware)
int fb_open_file_extended(const char *path, uint32_t flags);

// Read file (network-aware)
ssize_t fb_read_file_extended(int fd, void *buffer, size_t count, bool is_network);

// Write file (network-aware)
ssize_t fb_write_file_extended(int fd, const void *buffer, size_t count, bool is_network);

// Close file (network-aware)
int fb_close_file_extended(int fd, bool is_network);

// ============================================================================
// Network Share Discovery
// ============================================================================

// Scan for available network shares
int fb_discover_network_shares(netfs_resource_t *shares, int max_shares, int timeout_ms);

// List shares on a specific server
int fb_list_server_shares(const char *server, netfs_resource_t *shares, int max_shares);

// ============================================================================
// Bookmark/Favorite Management
// ============================================================================

// Add a network location bookmark
int fb_add_network_bookmark(const char *name, const char *url, bool auto_mount);

// Remove a network location bookmark
int fb_remove_network_bookmark(const char *name);

// Get bookmarks for sidebar
int fb_get_network_bookmarks(netfs_bookmark_t *bookmarks, int max_bookmarks);

// ============================================================================
// Credential Management
// ============================================================================

// Show credentials prompt dialog
// Returns true if user provided credentials
bool fb_show_credentials_dialog(const char *server, char *username, char *password);

// Save credentials for a server
int fb_save_server_credentials(const char *server, const char *username, const char *password);

// Check if credentials are saved for a server
bool fb_has_saved_credentials(const char *server);

#endif // FILEBROWSER_NETWORK_H
