// hotplug.h - USB Hotplug Manager for MayteraOS
// Handles device insertion/removal events and desktop integration
#ifndef HOTPLUG_H
#define HOTPLUG_H

#include "../types.h"
#include "usb_msc.h"
#include "../fs/fat.h"
#include "../fs/exfat.h"

// =============================================================================
// Constants
// =============================================================================

#define HOTPLUG_MAX_DEVICES     8
#define HOTPLUG_MOUNT_PATH_LEN  32
#define HOTPLUG_NAME_LEN        64

// Device status
#define HOTPLUG_STATUS_DISCONNECTED 0
#define HOTPLUG_STATUS_CONNECTED    1
#define HOTPLUG_STATUS_MOUNTED      2
#define HOTPLUG_STATUS_EJECTING     3

// Filesystem types
#define HOTPLUG_FS_UNKNOWN      0
#define HOTPLUG_FS_FAT16        1
#define HOTPLUG_FS_FAT32        2
#define HOTPLUG_FS_EXFAT        3
#define HOTPLUG_FS_NTFS         4  // Not supported, just detected

// =============================================================================
// Data Structures
// =============================================================================

// Mounted device information
typedef struct {
    int active;                         // Slot is in use
    int status;                         // Device status

    // USB device info
    int msc_device_index;               // Index into USB MSC device array
    int slot_id;                        // USB slot ID
    uint16_t vendor_id;
    uint16_t product_id;

    // Device identification
    char vendor[16];
    char product[32];
    char name[HOTPLUG_NAME_LEN];        // Display name

    // Storage info
    uint64_t capacity_bytes;
    uint64_t free_bytes;
    uint32_t block_size;

    // Filesystem info
    int fs_type;                        // FAT16, FAT32, exFAT
    char mount_point[HOTPLUG_MOUNT_PATH_LEN];

    // Filesystem state (union since only one active at a time)
    union {
        fat_fs_t fat;
        exfat_fs_t exfat;
    } fs;

    // Desktop icon
    int desktop_icon_index;             // -1 if no icon
    int grid_x, grid_y;                 // Desktop icon position
} hotplug_device_t;

// Callback for device events
typedef void (*hotplug_event_callback_t)(int event, hotplug_device_t *device);

// Event types for callbacks
#define HOTPLUG_EVENT_INSERTED      1
#define HOTPLUG_EVENT_REMOVED       2
#define HOTPLUG_EVENT_MOUNTED       3
#define HOTPLUG_EVENT_UNMOUNTED     4
#define HOTPLUG_EVENT_EJECT_SAFE    5   // Safe to remove

// =============================================================================
// Function Prototypes
// =============================================================================

// Initialize hotplug manager
void hotplug_init(void);

// Register callback for device events
void hotplug_register_callback(hotplug_event_callback_t callback);

// Unregister callback
void hotplug_unregister_callback(void);

// Poll for device changes (call periodically)
void hotplug_poll(void);

// Handle USB MSC event (called from USB MSC driver)
void hotplug_handle_usb_event(usb_msc_event_t *event);

// Get device by index
hotplug_device_t *hotplug_get_device(int index);

// Get device count
int hotplug_get_device_count(void);

// Get device by mount point
hotplug_device_t *hotplug_get_device_by_mount(const char *mount_point);

// Mount device filesystem
int hotplug_mount(int device_index);

// Unmount device filesystem
int hotplug_unmount(int device_index);

// Safe eject (unmount + allow removal)
int hotplug_eject(int device_index);

// Check if device is mounted
int hotplug_is_mounted(int device_index);

// Get filesystem type name
const char *hotplug_fs_type_name(int fs_type);

// =============================================================================
// Desktop Integration
// =============================================================================

// Add USB drive icon to desktop
int hotplug_add_desktop_icon(int device_index);

// Remove USB drive icon from desktop
void hotplug_remove_desktop_icon(int device_index);

// Handle desktop icon double-click (opens file browser)
void hotplug_icon_activated(int device_index);

// Handle desktop icon right-click menu
void hotplug_show_context_menu(int device_index, int x, int y);

// =============================================================================
// File Browser Integration
// =============================================================================

// Structure for sidebar device entry
typedef struct {
    int device_index;
    char name[HOTPLUG_NAME_LEN];
    char mount_point[HOTPLUG_MOUNT_PATH_LEN];
    uint64_t total_bytes;
    uint64_t free_bytes;
    int is_mounted;
    int is_removable;
} hotplug_sidebar_entry_t;

// Get list of mounted devices for file browser sidebar
int hotplug_get_sidebar_entries(hotplug_sidebar_entry_t *entries, int max_entries);

// Handle eject button click from file browser
int hotplug_eject_from_browser(const char *mount_point);

// =============================================================================
// Debugging
// =============================================================================

// Print all devices
void hotplug_print_devices(void);

#endif // HOTPLUG_H
