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
// #234i: a MOUNTED DISK IMAGE is a removable volume too. It is not a physical
// device and drivers/hotplug.c knows nothing about it, but it is the same
// THING to a user: something that appears, is browsable, and is ejected. It
// therefore joins the same list rather than growing a second one, and these
// two fs_type values are how the marshaller tells the two classes of image
// apart (rustkern/hotplug.rs derives MOSVOL_OPTICAL / MOSVOL_FLOPPY /
// MOSVOL_READONLY from them). kernel/dos/diskimg.c is the producer.
#define HOTPLUG_FS_ISO9660      5  // mounted .iso / raw CD image, READ-ONLY
#define HOTPLUG_FS_FAT12        6  // mounted floppy image (.img / .ima)

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
// #250 GUI INTEGRATION: the parts that were written and never wired up
// =============================================================================
//
// Every function in the "Desktop Integration" and "File Browser Integration"
// blocks above was written for the IN-KERNEL desktop and file browser, which
// stopped being the shipping shell when /APPS/COMPOSIT took over. So they had
// no callers, and a hot-plugged stick mounted in silence: no sidebar row, no
// desktop icon, no way to eject. The shell is now a Ring-3 process, so the
// integration has to cross the syscall boundary, and these are the pieces
// that carry it. hotplug_get_sidebar_entries() above is still THE list
// builder; hotplug_vol_raw() is the same data in a form a #[repr(C)] Rust
// consumer can read without mirroring a union.

// Raw per-slot snapshot for the Rust volume marshaller (rustkern/hotplug.rs).
// Locked against drift by _Static_assert in drivers/hotplug.c.
typedef struct {
    int32_t  present;           // slot is occupied by a physical device
    int32_t  mounted;           // filesystem mounted
    int32_t  fs_type;           // HOTPLUG_FS_*
    int32_t  readable;          // file READS are implemented for this fs_type
    uint64_t capacity_bytes;
    uint64_t free_bytes;
    int32_t  free_known;        // 0 = free_bytes is not meaningful
    int32_t  reserved0;
    char     name[HOTPLUG_NAME_LEN];
    char     mount_point[HOTPLUG_MOUNT_PATH_LEN];
} hotplug_raw_t;

// Fill `out` for slot `index`. Returns 0 on success, -1 if the index is out of
// range. An unoccupied slot returns 0 with present == 0.
int hotplug_vol_raw(int index, hotplug_raw_t *out);

// #234i: the SAME snapshot shape, produced by kernel/dos/diskimg.c for a
// mounted disk image on DOS drive-letter index `idx` (0 = A .. 25 = Z).
// Returns 1 and fills `out` when an image is mounted there, 0 otherwise.
//
// DECLARED HERE, NOT IN dos/diskimg.h, on purpose: hotplug_raw_t is the
// contract, and a contract with two producers should have ONE declaration
// site or the two drift. diskimg.c includes this header to implement it.
//
// NON-BLOCKING. Every field is answered out of the mount table in RAM (see
// dos/diskimg.h: "Neither call reads a sector"), so this is safe from the
// same contexts hotplug_vol_raw() is.
int diskimg_vol_raw(int idx, hotplug_raw_t *out);

// Non-zero if this filesystem type can actually serve file reads. exFAT
// mounts and reports free space but every one of its file operations is
// unimplemented (fs/exfat.c), so a browsable-looking exFAT volume would be a
// lie. The ONE place that judgement is made.
int hotplug_fs_readable(int fs_type);

// Path routing. If `path` names a file on a mounted removable volume, return
// that volume's slot index and set *rel_out to the path RELATIVE to the
// volume root (always starting with '/'). Otherwise return -1 and leave
// *rel_out alone. Case-insensitive, because the mount point is spelled in
// FAT-style uppercase and userland paths arrive in both cases.
int hotplug_resolve_path(const char *path, const char **rel_out);

// The mounted filesystem for a slot, or NULL. Used by the fd layer to open a
// handle against the right volume: fat_open() is already parameterised by
// fat_fs_t, so no second open path is needed.
fat_fs_t *hotplug_volume_fat(int index);

// Eject by slot index, from a syscall. Flushes, unmounts, then tells the
// device it is safe to remove. Returns 0 on success.
int hotplug_eject_slot(int index);

// =============================================================================
// Debugging
// =============================================================================

// Print all devices
void hotplug_print_devices(void);

#endif // HOTPLUG_H
