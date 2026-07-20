// filebrowser_nfs.h - NFS Network Location Support for File Browser
// Adds network filesystem browsing capabilities to the file browser
#ifndef FILEBROWSER_NFS_H
#define FILEBROWSER_NFS_H

#include "../types.h"
#include "filebrowser.h"
#include "../fs/netfs.h"

// ============================================================================
// Network Location Constants
// ============================================================================

#define FB_MAX_NETWORK_LOCATIONS    8
#define FB_MAX_SERVER_NAME          64
#define FB_MAX_EXPORT_PATH          256

// Sidebar width for network locations
#define FB_SIDEBAR_WIDTH            160
#define FB_SIDEBAR_ITEM_HEIGHT      24
#define FB_SIDEBAR_HEADER_HEIGHT    28

// Colors for network locations sidebar
#define FB_SIDEBAR_BG               0xFFF8F8F8
#define FB_SIDEBAR_HEADER_BG        0xFFE8E8E8
#define FB_SIDEBAR_HEADER_TEXT      0xFF404040
#define FB_SIDEBAR_ITEM_TEXT        0xFF303030
#define FB_SIDEBAR_ITEM_HOVER       0xFFE0E0FF
#define FB_SIDEBAR_ITEM_SELECTED    0xFF0078D7
#define FB_SIDEBAR_SEP_COLOR        0xFFD0D0D0
#define FB_NETWORK_ICON_COLOR       0xFF4080FF

// ============================================================================
// Network Location Types
// ============================================================================

typedef enum {
    NFS_LOC_TYPE_LOCAL,         // Local filesystem (root)
    NFS_LOC_TYPE_NFS,           // NFS share
    NFS_LOC_TYPE_SMB,           // SMB/CIFS share (future)
    NFS_LOC_TYPE_WEBDAV         // WebDAV (future)
} network_loc_type_t;

// Network location entry
typedef struct {
    bool active;
    network_loc_type_t type;
    char name[FB_MAX_SERVER_NAME];      // Display name
    char server[FB_MAX_SERVER_NAME];    // Server hostname/IP
    char export_path[FB_MAX_EXPORT_PATH]; // Export/share path
    char mount_point[FB_MAX_PATH];      // Local mount point
    bool connected;                      // Is currently mounted?
    uint32_t server_ip;                 // Resolved server IP
} network_location_t;

// ============================================================================
// Mount NFS Dialog
// ============================================================================

typedef struct {
    bool visible;
    window_t *dialog_window;
    char server_input[FB_MAX_SERVER_NAME];
    char export_input[FB_MAX_EXPORT_PATH];
    char mount_input[FB_MAX_PATH];
    int focus_field;        // 0=server, 1=export, 2=mount point
    int cursor_pos;
    char error_message[128];
} nfs_mount_dialog_t;

// ============================================================================
// Extended File Browser State for NFS
// ============================================================================

typedef struct {
    // Network locations
    network_location_t locations[FB_MAX_NETWORK_LOCATIONS];
    int location_count;
    int selected_location;      // -1 for none, 0+ for location index
    
    // Sidebar state
    bool sidebar_visible;
    int sidebar_hover_item;     // -1 for none
    
    // Mount dialog
    nfs_mount_dialog_t mount_dialog;
    
    // Current network context
    bool is_network_path;       // True if browsing network location
    int current_location_index; // Which location we're browsing
} fb_nfs_state_t;

// ============================================================================
// Initialization
// ============================================================================

// Initialize NFS support for file browser
void fb_nfs_init(filebrowser_t *fb);

// Cleanup NFS resources
void fb_nfs_cleanup(filebrowser_t *fb);

// Get NFS state for file browser
fb_nfs_state_t *fb_nfs_get_state(filebrowser_t *fb);

// ============================================================================
// Network Locations Management
// ============================================================================

// Add a network location (returns location index or -1)
int fb_nfs_add_location(fb_nfs_state_t *state, network_loc_type_t type,
                        const char *name, const char *server, 
                        const char *export_path, const char *mount_point);

// Remove a network location
int fb_nfs_remove_location(fb_nfs_state_t *state, int index);

// Connect to a network location (mount)
int fb_nfs_connect_location(fb_nfs_state_t *state, int index);

// Disconnect from a network location (unmount)
int fb_nfs_disconnect_location(fb_nfs_state_t *state, int index);

// Check if location is connected
bool fb_nfs_is_connected(fb_nfs_state_t *state, int index);

// Save locations to config file
int fb_nfs_save_locations(fb_nfs_state_t *state);

// Load locations from config file
int fb_nfs_load_locations(fb_nfs_state_t *state);

// ============================================================================
// Sidebar Drawing
// ============================================================================

// Draw the network locations sidebar
void fb_nfs_draw_sidebar(filebrowser_t *fb, int32_t x, int32_t y, 
                         int32_t width, int32_t height);

// Handle sidebar mouse events
bool fb_nfs_handle_sidebar_click(filebrowser_t *fb, int32_t x, int32_t y);

// Handle sidebar hover
void fb_nfs_handle_sidebar_hover(filebrowser_t *fb, int32_t x, int32_t y);

// ============================================================================
// Mount Dialog
// ============================================================================

// Show the "Mount NFS Share" dialog
void fb_nfs_show_mount_dialog(filebrowser_t *fb);

// Hide the mount dialog
void fb_nfs_hide_mount_dialog(filebrowser_t *fb);

// Draw the mount dialog
void fb_nfs_draw_mount_dialog(filebrowser_t *fb);

// Handle mount dialog events
bool fb_nfs_handle_mount_dialog_event(filebrowser_t *fb, gui_event_t *event);

// ============================================================================
// File Browser Integration
// ============================================================================

// Navigate to a network location
int fb_nfs_navigate_to_location(filebrowser_t *fb, int location_index);

// Check if current path is a network path
bool fb_nfs_is_current_network(filebrowser_t *fb);

// Get network info for current location (for status bar)
const char *fb_nfs_get_location_info(filebrowser_t *fb);

// Override filebrowser_navigate for network paths
int fb_nfs_navigate(filebrowser_t *fb, const char *path);

// Override filebrowser_refresh for network paths
void fb_nfs_refresh(filebrowser_t *fb);

// ============================================================================
// Network Icons
// ============================================================================

// Draw network location icon (16x16)
void fb_draw_network_icon(int32_t x, int32_t y, network_loc_type_t type, 
                          bool connected);

// Draw large network icon (32x32)
void fb_draw_network_icon_large(int32_t x, int32_t y, network_loc_type_t type,
                                 bool connected);

// ============================================================================
// Export List Dialog
// ============================================================================

// Show available NFS exports from a server
int fb_nfs_show_exports(filebrowser_t *fb, const char *server);

// ============================================================================
// Utility Functions
// ============================================================================

// Format server info for display
void fb_nfs_format_server_info(network_location_t *loc, char *buf, size_t size);

// Parse NFS URL (nfs://host/export)
bool fb_nfs_parse_url(const char *url, char *server, char *export_path);

// Generate mount point path from server and export
void fb_nfs_generate_mount_point(const char *server, const char *export_path,
                                  char *mount_point, size_t size);

#endif // FILEBROWSER_NFS_H
