// exfat.h - exFAT Filesystem Driver for MayteraOS
// Supports USB flash drives formatted with exFAT (common for drives >32GB)
#ifndef EXFAT_H
#define EXFAT_H

#include "../types.h"

// =============================================================================
// exFAT Constants
// =============================================================================

#define EXFAT_SIGNATURE         "EXFAT   "
#define EXFAT_SECTOR_SIZE_MIN   512
#define EXFAT_SECTOR_SIZE_MAX   4096
#define EXFAT_CLUSTER_SIZE_MIN  512
#define EXFAT_CLUSTER_SIZE_MAX  (32 * 1024 * 1024)

// Special cluster values
#define EXFAT_CLUSTER_FREE      0x00000000
#define EXFAT_CLUSTER_END       0xFFFFFFFF
#define EXFAT_CLUSTER_BAD       0xFFFFFFF7
#define EXFAT_CLUSTER_FIRST     2

// Directory entry types (entry_type field)
#define EXFAT_ENTRY_EOD             0x00    // End of directory
#define EXFAT_ENTRY_BITMAP          0x81    // Allocation bitmap
#define EXFAT_ENTRY_UPCASE          0x82    // Up-case table
#define EXFAT_ENTRY_VOLUME_LABEL    0x83    // Volume label
#define EXFAT_ENTRY_FILE            0x85    // File directory entry
#define EXFAT_ENTRY_STREAM          0xC0    // Stream extension
#define EXFAT_ENTRY_NAME            0xC1    // File name extension

// Deleted entry markers (bit 7 clear = deleted)
#define EXFAT_ENTRY_DELETED_MASK    0x80
#define EXFAT_ENTRY_IS_DELETED(t)   (!((t) & EXFAT_ENTRY_DELETED_MASK))

// File attributes (same as FAT)
#define EXFAT_ATTR_READ_ONLY    0x01
#define EXFAT_ATTR_HIDDEN       0x02
#define EXFAT_ATTR_SYSTEM       0x04
#define EXFAT_ATTR_VOLUME       0x08
#define EXFAT_ATTR_DIRECTORY    0x10
#define EXFAT_ATTR_ARCHIVE      0x20

// Flags for stream extension
#define EXFAT_FLAG_CONTIGUOUS   0x02    // No FAT chain, clusters are contiguous
#define EXFAT_FLAG_VALID_SIZE   0x01    // ValidDataLength is valid

// =============================================================================
// exFAT Boot Sector Structure
// =============================================================================

typedef struct {
    uint8_t  jump_boot[3];              // 0x00: Jump instruction
    uint8_t  fs_name[8];                // 0x03: "EXFAT   "
    uint8_t  must_be_zero[53];          // 0x0B: Must be zero
    uint64_t partition_offset;          // 0x40: Partition start (sectors)
    uint64_t volume_length;             // 0x48: Volume size (sectors)
    uint32_t fat_offset;                // 0x50: FAT start (sector offset)
    uint32_t fat_length;                // 0x54: FAT size (sectors)
    uint32_t cluster_heap_offset;       // 0x58: Cluster heap start (sector offset)
    uint32_t cluster_count;             // 0x5C: Total clusters
    uint32_t root_dir_cluster;          // 0x60: Root directory first cluster
    uint32_t volume_serial;             // 0x64: Volume serial number
    uint16_t fs_revision;               // 0x68: Filesystem revision (1.00 = 0x0100)
    uint16_t volume_flags;              // 0x6A: Volume flags
    uint8_t  bytes_per_sector_shift;    // 0x6C: Log2(bytes per sector) - 9..12
    uint8_t  sectors_per_cluster_shift; // 0x6D: Log2(sectors per cluster)
    uint8_t  num_fats;                  // 0x6E: Number of FATs (1 or 2)
    uint8_t  drive_select;              // 0x6F: INT 13h drive select
    uint8_t  percent_in_use;            // 0x70: Percent of heap in use
    uint8_t  reserved[7];               // 0x71: Reserved
    uint8_t  boot_code[390];            // 0x78: Boot code
    uint16_t boot_signature;            // 0x1FE: 0xAA55
} __attribute__((packed)) exfat_boot_sector_t;

// =============================================================================
// Directory Entry Structures (32 bytes each)
// =============================================================================

// Generic entry header
typedef struct {
    uint8_t  entry_type;                // Entry type
    uint8_t  custom[31];                // Entry-specific data
} __attribute__((packed)) exfat_entry_t;

// Volume Label entry (0x83)
typedef struct {
    uint8_t  entry_type;                // 0x83
    uint8_t  char_count;                // Number of characters (0-11)
    uint16_t label[11];                 // Volume label (UTF-16LE)
    uint8_t  reserved[8];
} __attribute__((packed)) exfat_volume_label_t;

// Allocation Bitmap entry (0x81)
typedef struct {
    uint8_t  entry_type;                // 0x81
    uint8_t  bitmap_flags;              // Bit 0: which bitmap (0 or 1)
    uint8_t  reserved[18];
    uint32_t first_cluster;             // First cluster of bitmap
    uint64_t data_length;               // Bitmap size in bytes
} __attribute__((packed)) exfat_bitmap_entry_t;

// Up-case Table entry (0x82)
typedef struct {
    uint8_t  entry_type;                // 0x82
    uint8_t  reserved1[3];
    uint32_t table_checksum;            // Up-case table checksum
    uint8_t  reserved2[12];
    uint32_t first_cluster;             // First cluster of table
    uint64_t data_length;               // Table size in bytes
} __attribute__((packed)) exfat_upcase_entry_t;

// File Directory Entry (0x85)
typedef struct {
    uint8_t  entry_type;                // 0x85
    uint8_t  secondary_count;           // Number of secondary entries
    uint16_t set_checksum;              // Checksum of entry set
    uint16_t file_attributes;           // File attributes
    uint16_t reserved1;
    uint32_t create_timestamp;          // Creation time
    uint32_t modify_timestamp;          // Last modification time
    uint32_t access_timestamp;          // Last access time
    uint8_t  create_10ms;               // Creation time 10ms increment
    uint8_t  modify_10ms;               // Modification time 10ms increment
    uint8_t  create_tz_offset;          // Creation timezone offset
    uint8_t  modify_tz_offset;          // Modification timezone offset
    uint8_t  access_tz_offset;          // Access timezone offset
    uint8_t  reserved2[7];
} __attribute__((packed)) exfat_file_entry_t;

// Stream Extension Entry (0xC0)
typedef struct {
    uint8_t  entry_type;                // 0xC0
    uint8_t  general_flags;             // Flags (bit 1 = contiguous)
    uint8_t  reserved1;
    uint8_t  name_length;               // Filename length in characters
    uint16_t name_hash;                 // Filename hash
    uint16_t reserved2;
    uint64_t valid_data_length;         // Valid data length
    uint32_t reserved3;
    uint32_t first_cluster;             // First cluster of data
    uint64_t data_length;               // Allocated size
} __attribute__((packed)) exfat_stream_entry_t;

// File Name Extension Entry (0xC1)
typedef struct {
    uint8_t  entry_type;                // 0xC1
    uint8_t  general_flags;
    uint16_t name[15];                  // Filename characters (UTF-16LE)
} __attribute__((packed)) exfat_name_entry_t;

// =============================================================================
// exFAT Filesystem State
// =============================================================================

// Read/write function types for block device abstraction
typedef int (*exfat_read_func_t)(void *ctx, uint64_t lba, void *buf, uint32_t count);
typedef int (*exfat_write_func_t)(void *ctx, uint64_t lba, const void *buf, uint32_t count);

typedef struct {
    // Block device interface
    void *device_ctx;                   // Device context (e.g., usb_msc_device_t*)
    exfat_read_func_t read_sectors;     // Read function
    exfat_write_func_t write_sectors;   // Write function
    int device_lun;                     // LUN for USB devices

    // Partition info
    uint64_t part_start_lba;            // Partition start LBA

    // Volume parameters (from boot sector)
    uint32_t sector_size;               // Bytes per sector
    uint32_t cluster_size;              // Bytes per cluster
    uint32_t sectors_per_cluster;
    uint32_t fat_offset;                // FAT start (sector offset)
    uint32_t fat_length;                // FAT size (sectors)
    uint32_t cluster_heap_offset;       // Data region start (sector offset)
    uint32_t cluster_count;             // Total clusters
    uint32_t root_dir_cluster;          // Root directory first cluster
    uint32_t volume_serial;

    // Allocation bitmap
    uint32_t bitmap_cluster;            // First cluster of bitmap
    uint64_t bitmap_size;               // Bitmap size in bytes

    // Volume label
    char volume_label[24];              // Volume label (UTF-8)

    // State
    int mounted;
    int read_only;
} exfat_fs_t;

// File handle
typedef struct {
    exfat_fs_t *fs;
    uint32_t first_cluster;
    uint32_t current_cluster;
    uint64_t file_size;
    uint64_t valid_size;                // Valid data length
    uint64_t position;                  // Current read/write position
    uint16_t attr;
    char name[256];
    int is_dir;
    int is_contiguous;                  // Clusters are contiguous (no FAT chain)
    int open;
} exfat_file_t;

// Directory iteration
typedef struct {
    exfat_fs_t *fs;
    uint32_t cluster;
    uint32_t byte_offset;               // Offset within cluster
    int at_end;
} exfat_dir_iter_t;

// Directory entry info (combined from entry set)
typedef struct {
    char name[256];                     // Filename (UTF-8)
    uint16_t attr;                      // Attributes
    uint32_t first_cluster;
    uint64_t file_size;
    uint64_t valid_size;
    int is_contiguous;
    uint32_t create_time;
    uint32_t modify_time;
    uint32_t access_time;
} exfat_dir_info_t;

// =============================================================================
// Function Prototypes
// =============================================================================

// Initialize exFAT driver
void exfat_init(void);

// Mount exFAT filesystem
int exfat_mount(exfat_fs_t *fs, void *device_ctx,
                exfat_read_func_t read_func, exfat_write_func_t write_func,
                int lun, uint64_t part_start_lba);

// Unmount filesystem
void exfat_unmount(exfat_fs_t *fs);

// Open file/directory
int exfat_open(exfat_fs_t *fs, const char *path, exfat_file_t *file);

// Close file
void exfat_close(exfat_file_t *file);

// Read from file
int exfat_read(exfat_file_t *file, void *buffer, uint64_t size);

// Write to file
int exfat_write(exfat_file_t *file, const void *buffer, uint64_t size);

// Get file size
uint64_t exfat_size(exfat_file_t *file);

// Seek in file
int exfat_seek(exfat_file_t *file, uint64_t position);

// Check if is directory
int exfat_is_dir(exfat_file_t *file);

// Begin directory iteration
int exfat_opendir(exfat_fs_t *fs, const char *path, exfat_dir_iter_t *iter);

// Read next directory entry
int exfat_readdir(exfat_dir_iter_t *iter, exfat_dir_info_t *info);

// Close directory iteration
void exfat_closedir(exfat_dir_iter_t *iter);

// Create file
int exfat_create(exfat_fs_t *fs, const char *path);

// Create directory
int exfat_mkdir(exfat_fs_t *fs, const char *path);

// Delete file or empty directory
int exfat_delete(exfat_fs_t *fs, const char *path);

// Rename file or directory
int exfat_rename(exfat_fs_t *fs, const char *old_path, const char *new_path);

// Read entire file
void *exfat_read_file(exfat_fs_t *fs, const char *path, uint64_t *size_out);

// Write entire file
int exfat_write_file(exfat_fs_t *fs, const char *path, const void *data, uint64_t size);

// Check if path exists
int exfat_exists(exfat_fs_t *fs, const char *path);

// Get free space
uint64_t exfat_get_free_space(exfat_fs_t *fs);

// Get total space
uint64_t exfat_get_total_space(exfat_fs_t *fs);

// Print filesystem info
void exfat_print_info(exfat_fs_t *fs);

// List directory contents
void exfat_list_dir(exfat_fs_t *fs, const char *path);

// Sync (flush caches)
int exfat_sync(exfat_fs_t *fs);

// =============================================================================
// Utility Functions
// =============================================================================

// Convert exFAT timestamp to Unix timestamp
uint64_t exfat_timestamp_to_unix(uint32_t exfat_time);

// Convert Unix timestamp to exFAT timestamp
uint32_t unix_to_exfat_timestamp(uint64_t unix_time);

// Calculate filename hash (for verification)
uint16_t exfat_name_hash(const uint16_t *name, int len);

#endif // EXFAT_H
