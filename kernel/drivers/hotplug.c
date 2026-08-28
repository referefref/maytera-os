// hotplug.c - USB Hotplug Manager for MayteraOS
#include "hotplug.h"
#include "usb_msc.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../gui/desktop.h"
#include "../gui/icons.h"
#include "../gui/filebrowser.h"
#include "../fs/bootlog.h"

// =============================================================================
// Global State
// =============================================================================

static hotplug_device_t g_devices[HOTPLUG_MAX_DEVICES];
static int g_device_count = 0;
static hotplug_event_callback_t g_event_callback = NULL;

// Mount point counter for unique names
static int g_mount_counter = 0;

// #250 MOUNT GENERATION SOURCE. Monotonic, never reused, never 0. Stamped into
// each removable fat_fs_t at mount and BUMPED at teardown, which is what turns
// every fat_file_t still open on a pulled stick into a handle that fails
// (fat_handle_stale) instead of one that reads the next stick's sectors. A
// counter and not a flag, because the slot can be refilled: a flag would be
// cleared by the very next mount and the stale handle would come back to life.
static uint32_t g_vol_gen = 0;

_Static_assert(sizeof(hotplug_raw_t) == 136,
               "#250: rustkern/hotplug.rs HpRaw mirrors this; sizes must match");

// Forward declaration for desktop integration
extern int desktop_add_icon(const char *name, int icon_id, int grid_x, int grid_y, void (*launch)(void));
extern void desktop_remove_icon(int index);

// =============================================================================
// Helper Functions
// =============================================================================

// USB MSC read wrapper for FAT
static int __attribute__((unused)) fat_read_wrapper(void *ctx, uint64_t lba, void *buf, uint32_t count) {
    hotplug_device_t *dev = (hotplug_device_t *)ctx;
    usb_msc_device_t *msc = usb_msc_get_device(dev->msc_device_index);
    if (!msc) return -1;
    return usb_msc_read(msc, 0, lba, buf, count);
}

// USB MSC write wrapper for FAT
static int __attribute__((unused)) fat_write_wrapper(void *ctx, uint64_t lba, const void *buf, uint32_t count) {
    hotplug_device_t *dev = (hotplug_device_t *)ctx;
    usb_msc_device_t *msc = usb_msc_get_device(dev->msc_device_index);
    if (!msc) return -1;
    return usb_msc_write(msc, 0, lba, buf, count);
}

// USB MSC read wrapper for exFAT
static int exfat_read_wrapper(void *ctx, uint64_t lba, void *buf, uint32_t count) {
    // ctx is already the hotplug_device_t for exFAT
    hotplug_device_t *dev = (hotplug_device_t *)ctx;
    usb_msc_device_t *msc = usb_msc_get_device(dev->msc_device_index);
    if (!msc) return -1;
    return usb_msc_read(msc, 0, lba, buf, count);
}

// USB MSC write wrapper for exFAT
static int exfat_write_wrapper(void *ctx, uint64_t lba, const void *buf, uint32_t count) {
    hotplug_device_t *dev = (hotplug_device_t *)ctx;
    usb_msc_device_t *msc = usb_msc_get_device(dev->msc_device_index);
    if (!msc) return -1;
    return usb_msc_write(msc, 0, lba, buf, count);
}

// Fire event to registered callback
static void fire_event(int event, hotplug_device_t *device) {
    if (g_event_callback) {
        g_event_callback(event, device);
    }
}

// Find next available desktop icon position
static void find_icon_position(int *grid_x, int *grid_y) {
    // Start from column 1 (column 0 is for system icons)
    // and find first available slot
    for (int y = 0; y < 8; y++) {
        for (int x = 1; x < 3; x++) {
            bool occupied = false;
            for (int i = 0; i < HOTPLUG_MAX_DEVICES; i++) {
                if (g_devices[i].active && g_devices[i].desktop_icon_index >= 0 &&
                    g_devices[i].grid_x == x && g_devices[i].grid_y == y) {
                    occupied = true;
                    break;
                }
            }
            if (!occupied) {
                *grid_x = x;
                *grid_y = y;
                return;
            }
        }
    }
    *grid_x = 1;
    *grid_y = 0;
}

// Detect filesystem type from boot sector
static int detect_filesystem(usb_msc_device_t *msc, uint64_t part_start) {
    uint8_t *sector = kmalloc(512);
    if (!sector) return HOTPLUG_FS_UNKNOWN;

    if (usb_msc_read(msc, 0, part_start, sector, 1) < 0) {
        kfree(sector);
        return HOTPLUG_FS_UNKNOWN;
    }

    // Check for exFAT signature
    if (memcmp(sector + 3, "EXFAT   ", 8) == 0) {
        kfree(sector);
        return HOTPLUG_FS_EXFAT;
    }

    // Check for FAT boot signature
    if (sector[510] == 0x55 && sector[511] == 0xAA) {
        // Check OEM name for NTFS
        if (memcmp(sector + 3, "NTFS    ", 8) == 0) {
            kfree(sector);
            return HOTPLUG_FS_NTFS;
        }

        // FAT type detection
        uint16_t bytes_per_sector = *(uint16_t *)(sector + 11);
        uint8_t sectors_per_cluster = sector[13];
        uint16_t reserved_sectors = *(uint16_t *)(sector + 14);
        uint8_t num_fats = sector[16];
        uint16_t root_entries = *(uint16_t *)(sector + 17);
        uint16_t total_sectors_16 = *(uint16_t *)(sector + 19);
        uint32_t total_sectors_32 = *(uint32_t *)(sector + 32);
        uint16_t fat_size_16 = *(uint16_t *)(sector + 22);
        uint32_t fat_size_32 = *(uint32_t *)(sector + 36);

        uint32_t fat_size = fat_size_16 ? fat_size_16 : fat_size_32;
        uint32_t total_sectors = total_sectors_16 ? total_sectors_16 : total_sectors_32;

        uint32_t root_dir_sectors = ((root_entries * 32) + (bytes_per_sector - 1)) / bytes_per_sector;
        uint32_t data_sectors = total_sectors - (reserved_sectors + (num_fats * fat_size) + root_dir_sectors);
        uint32_t cluster_count = data_sectors / sectors_per_cluster;

        kfree(sector);

        if (cluster_count < 4085) {
            return HOTPLUG_FS_FAT16;  // Actually FAT12, but we'll call it FAT16
        } else if (cluster_count < 65525) {
            return HOTPLUG_FS_FAT16;
        } else {
            return HOTPLUG_FS_FAT32;
        }
    }

    kfree(sector);
    return HOTPLUG_FS_UNKNOWN;
}

// Find partition start (basic MBR parsing)
static uint64_t find_partition_start(usb_msc_device_t *msc) {
    uint8_t *mbr = kmalloc(512);
    if (!mbr) return 0;

    if (usb_msc_read(msc, 0, 0, mbr, 1) < 0) {
        kfree(mbr);
        return 0;
    }

    // Check for MBR signature
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        kfree(mbr);
        return 0;  // No MBR, might be a superfloppy
    }

    // Check first partition
    uint8_t *part = mbr + 446;
    uint8_t part_type = part[4];
    uint32_t part_start = *(uint32_t *)(part + 8);

    kfree(mbr);

    // Known FAT/exFAT partition types
    if (part_type == 0x01 ||    // FAT12
        part_type == 0x04 ||    // FAT16 <32MB
        part_type == 0x06 ||    // FAT16 >32MB
        part_type == 0x0B ||    // FAT32 CHS
        part_type == 0x0C ||    // FAT32 LBA
        part_type == 0x0E ||    // FAT16 LBA
        part_type == 0x07) {    // exFAT/NTFS
        return part_start;
    }

    // No recognized partition, check if it's a superfloppy (no partition table)
    int fs_type = detect_filesystem(msc, 0);
    if (fs_type != HOTPLUG_FS_UNKNOWN) {
        return 0;  // Filesystem at sector 0
    }

    return part_start;
}

// =============================================================================
// Initialization
// =============================================================================

void hotplug_init(void) {
    kprintf("[Hotplug] Initializing USB hotplug manager\n");

    // #250 BOOT PROOF. There is no C twin to run a differential against, so
    // what stands in for one is a vector test over the pure routing core, run
    // on every boot with one line of output. It exists because the thing it
    // protects cannot be exercised without a second physical USB stick: the
    // shipping VMs boot from one device, and a routing regression would show
    // up as a file silently opening on the WRONG volume, which is exactly the
    // shape that does not announce itself.
    {
        extern uint32_t hotplug_selftest_rs(uint32_t *out_checks);
        uint32_t checks = 0;
        uint32_t fails = hotplug_selftest_rs(&checks);
        if (fails == 0) {
            kprintf("[Hotplug] volume/path self-test PASS (%u checks)\n", checks);
            bootlog_write("[Hotplug] volume/path self-test PASS (%u checks)", checks);
        } else {
            kprintf("[Hotplug] *** volume/path self-test FAIL: %u/%u ***\n", fails, checks);
            bootlog_write("[Hotplug] *** volume/path self-test FAIL %u/%u ***", fails, checks);
        }
    }

    memset(g_devices, 0, sizeof(g_devices));
    g_device_count = 0;
    g_event_callback = NULL;
    g_mount_counter = 0;

    // Register with USB MSC driver
    usb_msc_register_hotplug_callback(hotplug_handle_usb_event);
}

void hotplug_register_callback(hotplug_event_callback_t callback) {
    g_event_callback = callback;
}

void hotplug_unregister_callback(void) {
    g_event_callback = NULL;
}

// =============================================================================
// USB Event Handling
// =============================================================================

void hotplug_handle_usb_event(usb_msc_event_t *event) {
    if (!event) return;

    switch (event->type) {
        case USB_MSC_EVENT_INSERTED:
            kprintf("[Hotplug] USB device inserted (index %d)\n", event->device_index);
            // Device detected but not yet ready
            break;

        case USB_MSC_EVENT_MOUNT_READY: {
            kprintf("[Hotplug] USB device ready for mount (index %d)\n", event->device_index);

            // Find free slot
            int slot = -1;
            for (int i = 0; i < HOTPLUG_MAX_DEVICES; i++) {
                if (!g_devices[i].active) {
                    slot = i;
                    break;
                }
            }

            if (slot < 0) {
                kprintf("[Hotplug] No free device slots\n");
                break;
            }

            // #250 SAFETY: NEVER take a hotplug slot for the BOOT MEDIUM.
            //
            // This does not fire today, and that is exactly why the guard
            // exists. main.c registers this callback (hotplug_init, line
            // ~1067) AFTER usb_init() has already enumerated the boot stick,
            // so its MOUNT_READY event was fired before anyone was listening.
            // That is an ORDERING accident, not an invariant: the port re-scan
            // worker re-enumerates any port whose enumerated flag is cleared,
            // and a single confirmed flap on the boot port would deliver this
            // event for the root device.
            //
            // The consequence would not be cosmetic. The volume would appear
            // in the Files sidebar and on the desktop with an Eject button,
            // and eject issues SCSI STOP UNIT on the medium the running
            // system's root filesystem is served from.
            {
                extern int blk_root_is_usb(void);
                extern int blk_root_usb_index(void);
                if (blk_root_is_usb() && event->device_index == blk_root_usb_index()) {
                    kprintf("[Hotplug] index %d is the BOOT medium; not a removable volume\n",
                            event->device_index);
                    break;
                }
            }

            hotplug_device_t *dev = &g_devices[slot];
            memset(dev, 0, sizeof(hotplug_device_t));

            usb_msc_device_t *msc = event->device;
            if (!msc) break;

            dev->active = 1;
            dev->status = HOTPLUG_STATUS_CONNECTED;
            dev->msc_device_index = event->device_index;
            dev->slot_id = msc->slot_id;
            dev->desktop_icon_index = -1;

            // Copy device info
            strncpy(dev->vendor, msc->vendor, sizeof(dev->vendor) - 1);
            strncpy(dev->product, msc->product, sizeof(dev->product) - 1);

            // Create display name
            if (dev->vendor[0] && dev->product[0]) {
                snprintf(dev->name, sizeof(dev->name), "%s %s", dev->vendor, dev->product);
            } else if (dev->product[0]) {
                strncpy(dev->name, dev->product, sizeof(dev->name) - 1);
            } else {
                snprintf(dev->name, sizeof(dev->name), "USB Drive %d", g_mount_counter);
            }

            // Storage info
            dev->capacity_bytes = msc->num_blocks * msc->block_size;
            dev->block_size = msc->block_size;

            // Create mount point
            // Uppercase, like every other top-level path this OS ships
            // (/APPS, /CONFIG, /FONTS). Lowercase worked, but the Files app
            // shows the literal string and a lone lowercase row next to
            // uppercase ones reads as a bug.
            snprintf(dev->mount_point, sizeof(dev->mount_point), "/USB%d", g_mount_counter++);

            g_device_count++;

            // Fire event
            fire_event(HOTPLUG_EVENT_INSERTED, dev);

            // Auto-mount
            hotplug_mount(slot);
            break;
        }

        case USB_MSC_EVENT_REMOVED: {
            kprintf("[Hotplug] USB device removed (index %d)\n", event->device_index);

            // Find and remove device
            for (int i = 0; i < HOTPLUG_MAX_DEVICES; i++) {
                if (g_devices[i].active &&
                    g_devices[i].msc_device_index == event->device_index) {

                    hotplug_device_t *dev = &g_devices[i];

                    // Remove desktop icon if present
                    if (dev->desktop_icon_index >= 0) {
                        hotplug_remove_desktop_icon(i);
                    }

                    // #250 SURPRISE REMOVAL. The slot used to be cleared with
                    // the filesystem still marked mounted, so a fat_file_t
                    // opened on it kept a live fat_fs_t pointer describing a
                    // medium that is no longer there, and the next stick to
                    // land in this slot would be read through it.
                    //
                    // Two things fix that, in this order:
                    //   1. mounted = 0, so no NEW handle can be opened.
                    //   2. a fresh generation, so every EXISTING handle is
                    //      stale and every read/seek/readdir on it returns -1.
                    // Nothing is written to the device: it is gone, and a write
                    // attempt would only stall the caller on a dead endpoint.
                    // An application with a file open on the volume therefore
                    // sees read errors, which is the truth, rather than
                    // silence or another volume's bytes.
                    if (dev->status == HOTPLUG_STATUS_MOUNTED) {
                        switch (dev->fs_type) {
                            case HOTPLUG_FS_FAT16:
                            case HOTPLUG_FS_FAT32:
                                fat_unmount(&dev->fs.fat);
                                if (++g_vol_gen == 0) g_vol_gen = 1;
                                dev->fs.fat.vol_gen = g_vol_gen;
                                break;
                            case HOTPLUG_FS_EXFAT:
                                exfat_unmount(&dev->fs.exfat);
                                break;
                        }
                        kprintf("[Hotplug] %s torn down on surprise removal\n",
                                dev->mount_point);
                    }

                    // Fire event
                    fire_event(HOTPLUG_EVENT_REMOVED, dev);

                    // Clear slot
                    dev->active = 0;
                    dev->status = HOTPLUG_STATUS_DISCONNECTED;
                    if (g_device_count > 0) g_device_count--;
                    break;
                }
            }
            break;
        }

        case USB_MSC_EVENT_MEDIA_CHANGE:
            kprintf("[Hotplug] Media change detected\n");
            // TODO: Handle media change for card readers
            break;

        default:
            break;
    }
}

// =============================================================================
// Device Access
// =============================================================================

hotplug_device_t *hotplug_get_device(int index) {
    if (index < 0 || index >= HOTPLUG_MAX_DEVICES) return NULL;
    if (!g_devices[index].active) return NULL;
    return &g_devices[index];
}

int hotplug_get_device_count(void) {
    return g_device_count;
}

hotplug_device_t *hotplug_get_device_by_mount(const char *mount_point) {
    if (!mount_point) return NULL;

    for (int i = 0; i < HOTPLUG_MAX_DEVICES; i++) {
        if (g_devices[i].active && g_devices[i].status == HOTPLUG_STATUS_MOUNTED) {
            if (strcmp(g_devices[i].mount_point, mount_point) == 0) {
                return &g_devices[i];
            }
        }
    }
    return NULL;
}

// =============================================================================
// Mount/Unmount Operations
// =============================================================================

int hotplug_mount(int device_index) {
    hotplug_device_t *dev = hotplug_get_device(device_index);
    if (!dev) return -1;

    if (dev->status == HOTPLUG_STATUS_MOUNTED) {
        return 0;  // Already mounted
    }

    usb_msc_device_t *msc = usb_msc_get_device(dev->msc_device_index);
    if (!msc || !msc->ready) {
        kprintf("[Hotplug] Device not ready\n");
        return -1;
    }

    // Find partition start
    uint64_t part_start = find_partition_start(msc);
    kprintf("[Hotplug] Partition start: LBA %llu\n", part_start);

    // Detect filesystem type
    dev->fs_type = detect_filesystem(msc, part_start);
    kprintf("[Hotplug] Detected filesystem: %s\n", hotplug_fs_type_name(dev->fs_type));

    int result = -1;

    switch (dev->fs_type) {
        case HOTPLUG_FS_FAT16:
        case HOTPLUG_FS_FAT32:
            // #250 THE BUG THIS REPLACES. This used to be
            //     fat_mount_lba(0, part_start, &dev->fs.fat);
            //     dev->fs.fat.drive = -1;   // "Mark as USB drive"
            // and the comment was the whole of the USB routing: nothing ever
            // read drive == -1. fat_mount_lba(0, ...) reads through blk_read(),
            // which serves the ROOT block device, so this mounted whatever
            // happened to sit at that LBA on the BOOT medium and then called it
            // the stick. On a USB-booted golden it produced a plausible FAT
            // mount of the boot volume, and a write to a "file on the stick"
            // would have gone to the boot volume's sectors.
            //
            // fat_mount_lba_usb() binds the mount to THIS device for every
            // sector it will ever touch, and stamps a generation so handles
            // can be invalidated when it is pulled.
            if (++g_vol_gen == 0) g_vol_gen = 1;
            result = fat_mount_lba_usb(dev->msc_device_index, (uint32_t)part_start,
                                       g_vol_gen, &dev->fs.fat);
            if (result >= 0) {
                kprintf("[Hotplug] FAT filesystem mounted at %s (usb dev %d, gen %u)\n",
                        dev->mount_point, dev->msc_device_index, g_vol_gen);
            }
            break;

        case HOTPLUG_FS_EXFAT:
            // Mount as exFAT
            result = exfat_mount(&dev->fs.exfat, dev,
                                 exfat_read_wrapper, exfat_write_wrapper,
                                 0, part_start);
            if (result >= 0) {
                kprintf("[Hotplug] exFAT filesystem mounted at %s\n", dev->mount_point);
            }
            break;

        case HOTPLUG_FS_NTFS:
            kprintf("[Hotplug] NTFS not supported\n");
            break;

        default:
            kprintf("[Hotplug] Unknown filesystem\n");
            break;
    }

    if (result >= 0) {
        dev->status = HOTPLUG_STATUS_MOUNTED;

        // Calculate free space.
        // #250: on a removable FAT volume this is 0 and means UNKNOWN, not
        // "full". fat_mount_lba_usb deliberately skips the whole-FAT free
        // scan, because on a cacheless aux volume it is one SCSI round trip
        // per FAT sector (~16,000 of them on a 32 GB stick) between plugging
        // the drive in and it appearing. The UI reports the size and says the
        // free figure is unknown rather than showing a wrong one.
        switch (dev->fs_type) {
            case HOTPLUG_FS_FAT16:
            case HOTPLUG_FS_FAT32:
                dev->free_bytes = (uint64_t)fat_get_free_clusters(&dev->fs.fat) *
                                  dev->fs.fat.sectors_per_cluster *
                                  dev->fs.fat.bytes_per_sector;
                break;
            case HOTPLUG_FS_EXFAT:
                dev->free_bytes = exfat_get_free_space(&dev->fs.exfat);
                break;
        }

        // Add desktop icon
        hotplug_add_desktop_icon(device_index);

        // Fire event
        fire_event(HOTPLUG_EVENT_MOUNTED, dev);
    }

    return result;
}

int hotplug_unmount(int device_index) {
    hotplug_device_t *dev = hotplug_get_device(device_index);
    if (!dev) return -1;

    if (dev->status != HOTPLUG_STATUS_MOUNTED) {
        return 0;  // Not mounted
    }

    kprintf("[Hotplug] Unmounting %s\n", dev->mount_point);

    // Unmount filesystem
    switch (dev->fs_type) {
        case HOTPLUG_FS_FAT16:
        case HOTPLUG_FS_FAT32:
            fat_unmount(&dev->fs.fat);
            // #250: same reason as the surprise-removal path. An explicit
            // eject must invalidate open handles too, or an app that still
            // holds one keeps reading a volume the user was told was safe to
            // pull.
            if (++g_vol_gen == 0) g_vol_gen = 1;
            dev->fs.fat.vol_gen = g_vol_gen;
            break;
        case HOTPLUG_FS_EXFAT:
            exfat_unmount(&dev->fs.exfat);
            break;
    }

    dev->status = HOTPLUG_STATUS_CONNECTED;

    // Remove desktop icon
    if (dev->desktop_icon_index >= 0) {
        hotplug_remove_desktop_icon(device_index);
    }

    // Fire event
    fire_event(HOTPLUG_EVENT_UNMOUNTED, dev);

    return 0;
}

int hotplug_eject(int device_index) {
    hotplug_device_t *dev = hotplug_get_device(device_index);
    if (!dev) return -1;

    kprintf("[Hotplug] Ejecting device %s\n", dev->name);

    // #250 THE BUG. These two statements used to be the other way round:
    //
    //     dev->status = HOTPLUG_STATUS_EJECTING;
    //     if (dev->status == HOTPLUG_STATUS_MOUNTED) hotplug_unmount(...);
    //
    // The test could never be true, because the line above it had just made
    // it false. So eject NEVER unmounted: the filesystem stayed mounted, the
    // handles stayed live, and the only thing that happened was the SCSI
    // START STOP UNIT. The device reported "safe to remove" without anything
    // having been flushed or torn down. Read the status FIRST.
    int was_mounted = (dev->status == HOTPLUG_STATUS_MOUNTED);
    dev->status = HOTPLUG_STATUS_EJECTING;

    if (was_mounted) {
        // hotplug_unmount() refuses on a status that is not MOUNTED (it is
        // also the "already unmounted" fast path), so restore it for the call.
        dev->status = HOTPLUG_STATUS_MOUNTED;
        hotplug_unmount(device_index);
        dev->status = HOTPLUG_STATUS_EJECTING;
    }

    // Safe eject USB device. usb_msc_safe_remove -> usb_msc_eject issues
    // SYNCHRONIZE CACHE on every LUN before ALLOW MEDIUM REMOVAL, so this is
    // where the flush to the physical medium actually happens.
    usb_msc_safe_remove(dev->msc_device_index);

    // Fire event
    fire_event(HOTPLUG_EVENT_EJECT_SAFE, dev);

    // The slot is released so the volume leaves the UI immediately. The
    // physical device is still attached and still enumerated by USB MSC; if it
    // is now pulled, the REMOVED event simply finds no active hotplug slot.
    dev->active = 0;
    dev->status = HOTPLUG_STATUS_DISCONNECTED;
    if (g_device_count > 0) g_device_count--;

    kprintf("[Hotplug] Device %s can be safely removed\n", dev->name);
    return 0;
}

int hotplug_is_mounted(int device_index) {
    hotplug_device_t *dev = hotplug_get_device(device_index);
    if (!dev) return 0;
    return (dev->status == HOTPLUG_STATUS_MOUNTED);
}

const char *hotplug_fs_type_name(int fs_type) {
    switch (fs_type) {
        case HOTPLUG_FS_FAT16: return "FAT16";
        case HOTPLUG_FS_FAT32: return "FAT32";
        case HOTPLUG_FS_EXFAT: return "exFAT";
        case HOTPLUG_FS_NTFS:  return "NTFS";
        default: return "Unknown";
    }
}

// =============================================================================
// Desktop Integration
// =============================================================================

// Launcher function for desktop icon (stores device index in a static)
static int g_launch_device_index = -1;

static void usb_icon_launch(void) {
    if (g_launch_device_index >= 0) {
        hotplug_icon_activated(g_launch_device_index);
    }
}

int hotplug_add_desktop_icon(int device_index) {
    hotplug_device_t *dev = hotplug_get_device(device_index);
    if (!dev) return -1;

    if (dev->desktop_icon_index >= 0) {
        return dev->desktop_icon_index;  // Already has icon
    }

    // Find icon position
    int grid_x, grid_y;
    find_icon_position(&grid_x, &grid_y);

    dev->grid_x = grid_x;
    dev->grid_y = grid_y;

    // Create desktop icon
    // Use USB drive icon (ICON_USB_DRIVE or similar)
    // For now, use folder icon as fallback
    int icon_id = ICON_USB_DRIVE;  // USB flash drive icon

    g_launch_device_index = device_index;
    int icon_index = desktop_add_icon(dev->name, icon_id, grid_x, grid_y, usb_icon_launch);

    if (icon_index >= 0) {
        dev->desktop_icon_index = icon_index;
        kprintf("[Hotplug] Added desktop icon for %s at (%d, %d)\n",
                dev->name, grid_x, grid_y);
    }

    return icon_index;
}

void hotplug_remove_desktop_icon(int device_index) {
    hotplug_device_t *dev = hotplug_get_device(device_index);
    if (!dev || dev->desktop_icon_index < 0) return;

    desktop_remove_icon(dev->desktop_icon_index);
    dev->desktop_icon_index = -1;

    kprintf("[Hotplug] Removed desktop icon for %s\n", dev->name);
}

void hotplug_icon_activated(int device_index) {
    hotplug_device_t *dev = hotplug_get_device(device_index);
    if (!dev) return;

    kprintf("[Hotplug] Opening file browser for %s\n", dev->mount_point);

    // Launch file browser and navigate to mount point
    filebrowser_t *fb = filebrowser_create();
    if (fb) {
        filebrowser_navigate(fb, dev->mount_point);
        filebrowser_run(fb);
    }
}

void hotplug_show_context_menu(int device_index, int x, int y) {
    // TODO: Show context menu with options:
    // - Open
    // - Eject
    // - Properties
    kprintf("[Hotplug] Context menu for device %d at (%d, %d)\n", device_index, x, y);
}

// =============================================================================
// File Browser Integration
// =============================================================================

// #250: ONE fill, two shapes. hotplug_vol_raw() below is the single reader of
// g_devices[]; this function, which predates it and was written for the
// in-kernel file browser, is now expressed in terms of it. Two independent
// copies of "what does the UI show for a volume" is exactly the shape that
// gave this tree two Task Managers and two g_wallpapers[] arrays.
int hotplug_get_sidebar_entries(hotplug_sidebar_entry_t *entries, int max_entries) {
    if (!entries || max_entries <= 0) return 0;

    int count = 0;
    for (int i = 0; i < HOTPLUG_MAX_DEVICES && count < max_entries; i++) {
        hotplug_raw_t raw;
        if (hotplug_vol_raw(i, &raw) != 0 || !raw.present) continue;
        hotplug_sidebar_entry_t *entry = &entries[count];
        entry->device_index = i;
        strncpy(entry->name, raw.name, sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';
        strncpy(entry->mount_point, raw.mount_point, sizeof(entry->mount_point) - 1);
        entry->mount_point[sizeof(entry->mount_point) - 1] = '\0';
        entry->total_bytes = raw.capacity_bytes;
        entry->free_bytes = raw.free_known ? raw.free_bytes : 0;
        entry->is_mounted = raw.mounted;
        entry->is_removable = 1;  // All USB devices are removable
        count++;
    }
    return count;
}

// =============================================================================
// #250: the pieces that carry the above across the syscall boundary
// =============================================================================

int hotplug_fs_readable(int fs_type) {
    // exFAT is deliberately NOT readable here even though it mounts and
    // reports free space. fs/exfat.c implements mount, unmount and
    // get_free_space and NOTHING ELSE: exfat_read_file() has a definition, a
    // prototype and no callers anywhere in the tree, and there is no exFAT
    // branch in the fd layer. Reporting it as browsable would put a volume in
    // the sidebar whose every file fails to open with no explanation. The
    // volume is still listed and still ejectable; it is labelled read-only-
    // unsupported instead of being hidden, because a stick the user can see
    // in their hand and cannot see on screen is the complaint this task
    // exists to fix.
    switch (fs_type) {
        case HOTPLUG_FS_FAT16:
        case HOTPLUG_FS_FAT32:
            return 1;
        default:
            return 0;
    }
}

int hotplug_vol_raw(int index, hotplug_raw_t *out) {
    if (!out || index < 0 || index >= HOTPLUG_MAX_DEVICES) return -1;
    memset(out, 0, sizeof(*out));

    hotplug_device_t *dev = &g_devices[index];
    if (!dev->active) return 0;          // present stays 0

    out->present = 1;
    out->mounted = (dev->status == HOTPLUG_STATUS_MOUNTED) ? 1 : 0;
    out->fs_type = dev->fs_type;
    out->readable = hotplug_fs_readable(dev->fs_type);
    out->capacity_bytes = dev->capacity_bytes;
    out->free_bytes = dev->free_bytes;
    // See the free-space comment in hotplug_mount(): a removable FAT volume
    // does not pay for the whole-FAT scan, so 0 means UNKNOWN there. exFAT
    // computes it from the allocation bitmap header at mount, cheaply.
    out->free_known = (dev->free_bytes > 0) ? 1 : 0;
    strncpy(out->name, dev->name, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
    strncpy(out->mount_point, dev->mount_point, sizeof(out->mount_point) - 1);
    out->mount_point[sizeof(out->mount_point) - 1] = '\0';
    return 0;
}

fat_fs_t *hotplug_volume_fat(int index) {
    hotplug_device_t *dev = hotplug_get_device(index);
    if (!dev) return NULL;
    if (dev->status != HOTPLUG_STATUS_MOUNTED) return NULL;
    if (!hotplug_fs_readable(dev->fs_type)) return NULL;
    if (!dev->fs.fat.mounted) return NULL;
    return &dev->fs.fat;
}

int hotplug_eject_slot(int index) {
    if (!hotplug_get_device(index)) return -1;
    return hotplug_eject(index);
}

int hotplug_resolve_path(const char *path, const char **rel_out) {
    if (!path || path[0] != '/') return -1;

    for (int i = 0; i < HOTPLUG_MAX_DEVICES; i++) {
        hotplug_device_t *dev = &g_devices[i];
        if (!dev->active || dev->status != HOTPLUG_STATUS_MOUNTED) continue;
        if (!dev->mount_point[0]) continue;
        // The match itself lives in Rust: "does /USB1 prefix-match /USB10"
        // is exactly the off-by-one a hand-rolled strncmp gets wrong, and it
        // would silently route one volume's paths to another.
        extern int hotplug_path_split_rs(const char *path, const char *mount, int *rel_off);
        int rel_off = -1;
        if (hotplug_path_split_rs(path, dev->mount_point, &rel_off) == 1) {
            // rel_off == -1 means `path` IS the mount point, i.e. the volume
            // root, which has no offset inside `path` at all. The relative
            // path is then the literal "/".
            static const char root_rel[] = "/";
            if (rel_out) *rel_out = (rel_off < 0) ? root_rel : (path + rel_off);
            return i;
        }
    }
    return -1;
}

int hotplug_eject_from_browser(const char *mount_point) {
    hotplug_device_t *dev = hotplug_get_device_by_mount(mount_point);
    if (!dev) return -1;

    // Find device index
    for (int i = 0; i < HOTPLUG_MAX_DEVICES; i++) {
        if (&g_devices[i] == dev) {
            return hotplug_eject(i);
        }
    }
    return -1;
}

// =============================================================================
// Polling and Debugging
// =============================================================================

void hotplug_poll(void) {
    // Poll USB MSC for hotplug events
    usb_msc_poll_hotplug();
}

void hotplug_print_devices(void) {
    kprintf("\n[Hotplug] Device List:\n");
    kprintf("%-20s %-10s %-10s %s\n", "Name", "Mount", "FS", "Status");
    kprintf("%-20s %-10s %-10s %s\n", "----", "-----", "--", "------");

    for (int i = 0; i < HOTPLUG_MAX_DEVICES; i++) {
        if (g_devices[i].active) {
            hotplug_device_t *dev = &g_devices[i];
            const char *status;
            switch (dev->status) {
                case HOTPLUG_STATUS_CONNECTED: status = "Connected"; break;
                case HOTPLUG_STATUS_MOUNTED: status = "Mounted"; break;
                case HOTPLUG_STATUS_EJECTING: status = "Ejecting"; break;
                default: status = "Unknown"; break;
            }

            kprintf("%-20s %-10s %-10s %s\n",
                    dev->name, dev->mount_point,
                    hotplug_fs_type_name(dev->fs_type), status);

            if (dev->status == HOTPLUG_STATUS_MOUNTED) {
                kprintf("  Capacity: %llu MB, Free: %llu MB\n",
                        dev->capacity_bytes / (1024 * 1024),
                        dev->free_bytes / (1024 * 1024));
            }
        }
    }

    if (g_device_count == 0) {
        kprintf("  (no devices)\n");
    }
}
