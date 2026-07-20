// fat.h - FAT12/FAT16/FAT32 Filesystem Driver
#ifndef FAT_H
#define FAT_H

#include "../types.h"

// FAT types
#define FAT_TYPE_12     12
#define FAT_TYPE_16     16
#define FAT_TYPE_32     32

// Directory entry attributes
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F  // Long filename entry

// Special cluster values
#define FAT16_EOC       0xFFF8  // End of chain (FAT16)
#define FAT32_EOC       0x0FFFFFF8  // End of chain (FAT32)
#define FAT_FREE        0x00000000  // Free cluster

// MBR Partition Entry
typedef struct {
    uint8_t  boot_flag;         // 0x80 = bootable
    uint8_t  start_chs[3];      // Starting CHS address
    uint8_t  type;              // Partition type
    uint8_t  end_chs[3];        // Ending CHS address
    uint32_t start_lba;         // Starting LBA
    uint32_t sector_count;      // Number of sectors
} __attribute__((packed)) mbr_partition_t;

// MBR Structure
typedef struct {
    uint8_t         bootstrap[446];
    mbr_partition_t partitions[4];
    uint16_t        signature;      // 0xAA55
} __attribute__((packed)) mbr_t;

// FAT Boot Sector (BIOS Parameter Block)
typedef struct {
    uint8_t  jmp[3];            // Jump instruction
    uint8_t  oem_name[8];       // OEM name
    uint16_t bytes_per_sector;  // Usually 512
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;  // Before first FAT
    uint8_t  num_fats;          // Usually 2
    uint16_t root_entries;      // FAT12/16 only
    uint16_t total_sectors_16;  // If 0, use total_sectors_32
    uint8_t  media_type;
    uint16_t fat_size_16;       // Sectors per FAT (FAT12/16)
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    // FAT32 extended fields
    union {
        struct {
            // FAT12/16
            uint8_t  drive_number;
            uint8_t  reserved;
            uint8_t  boot_sig;
            uint32_t volume_id;
            uint8_t  volume_label[11];
            uint8_t  fs_type[8];
        } __attribute__((packed)) fat16;

        struct {
            // FAT32
            uint32_t fat_size_32;
            uint16_t ext_flags;
            uint16_t fs_version;
            uint32_t root_cluster;
            uint16_t fs_info;
            uint16_t backup_boot;
            uint8_t  reserved[12];
            uint8_t  drive_number;
            uint8_t  reserved1;
            uint8_t  boot_sig;
            uint32_t volume_id;
            uint8_t  volume_label[11];
            uint8_t  fs_type[8];
        } __attribute__((packed)) fat32;
    } ext;
} __attribute__((packed)) fat_boot_sector_t;

// FAT Directory Entry (32 bytes)
typedef struct {
    uint8_t  name[11];          // 8.3 filename
    uint8_t  attr;              // Attributes
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_hi;        // High 16 bits of cluster (FAT32)
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t cluster_lo;        // Low 16 bits of cluster
    uint32_t file_size;
} __attribute__((packed)) fat_dir_entry_t;

// Long Filename Entry
typedef struct {
    uint8_t  order;             // Sequence number
    uint16_t name1[5];          // Characters 1-5
    uint8_t  attr;              // Always 0x0F
    uint8_t  type;              // Always 0
    uint8_t  checksum;
    uint16_t name2[6];          // Characters 6-11
    uint16_t cluster;           // Always 0
    uint16_t name3[2];          // Characters 12-13
} __attribute__((packed)) fat_lfn_entry_t;

// FAT Filesystem State
typedef struct {
    int      drive;             // ATA drive (0=primary master)
    int      partition;         // Partition number (0-3)
    uint32_t part_start_lba;    // Partition start LBA
    uint32_t part_sectors;      // Partition size in sectors

    uint8_t  fat_type;          // 12, 16, or 32
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t root_entry_count;  // FAT12/16 only
    uint32_t total_sectors;
    uint32_t fat_size;          // Sectors per FAT
    uint32_t root_cluster;      // FAT32 only

    uint32_t fat_start_lba;     // First FAT sector
    uint32_t root_start_lba;    // Root directory start (FAT12/16)
    uint32_t root_dir_sectors;  // Root directory sectors (FAT12/16)
    uint32_t data_start_lba;    // First data sector
    uint32_t cluster_count;     // Total data clusters

    uint8_t  volume_label[12];
    int      mounted;
    uint32_t free_cluster_count;  // Cached free clusters (updated incrementally)
} fat_fs_t;

// File handle
typedef struct {
    fat_fs_t *fs;
    uint32_t first_cluster;
    uint32_t current_cluster;
    uint32_t file_size;
    uint32_t position;          // Current read position
    uint8_t  attr;
    char     name[256];
    int      is_dir;
    int      open;
    uint32_t dirent_lba;        // absolute LBA of the sector holding this file's
    uint32_t dirent_off;        // directory entry, and byte offset within it (0=unknown)
} fat_file_t;

// Directory iteration
typedef struct {
    fat_fs_t *fs;
    uint32_t cluster;
    uint32_t entry_index;
    uint32_t sector_in_cluster;
} fat_dir_iter_t;

// Initialize FAT driver
void fat_init(void);

// Mount a FAT partition
int fat_mount(int drive, int partition, fat_fs_t *fs);

// Mount FAT from a specific LBA offset (for GPT partitions or raw FAT)
int fat_mount_lba(int drive, uint32_t start_lba, fat_fs_t *fs);

// Unmount
void fat_unmount(fat_fs_t *fs);

// Open file/directory
int fat_open(fat_fs_t *fs, const char *path, fat_file_t *file);

// Close file
void fat_close(fat_file_t *file);

// Read from file
int fat_read(fat_file_t *file, void *buffer, uint32_t size);

// Get file size
uint32_t fat_size(fat_file_t *file);

// Seek in file
int fat_seek(fat_file_t *file, uint32_t position);

// Read directory entry
int fat_readdir(fat_file_t *dir, fat_dir_entry_t *entry, char *name_out);

// Check if entry is directory
int fat_is_dir(fat_file_t *file);

// Get filesystem info
void fat_print_info(fat_fs_t *fs);

// List directory contents
void fat_list_dir(fat_fs_t *fs, const char *path);

// Read entire file into buffer (allocates memory)
void *fat_read_file(fat_fs_t *fs, const char *path, uint32_t *size_out);

// Read a critical config file with bounded retry + short backoff (#307).
// Real USB-MSC/ATA hardware can return a transient NULL/zero-size result on a
// single read where QEMU's virtual disk never misses; this wrapper retries a
// few times and logs every attempt to the persistent boot log. Shared by
// proc/users.c (PASSWD/SHADOW/GROUP) and gui/login.c (LOGIN.CFG) so there is
// ONE implementation, not a forked copy. Returns NULL and *size_out=0 on
// persistent failure.
void *fat_read_file_retry(fat_fs_t *fs, const char *path, uint32_t *size_out);

// ============================================
// Write Operations
// ============================================

// Create a new file
// Returns 0 on success, -1 on failure
int fat_create(fat_fs_t *fs, const char *path);

// Create a new directory
// Returns 0 on success, -1 on failure
int fat_mkdir(fat_fs_t *fs, const char *path);

// Delete a file or empty directory
// Returns 0 on success, -1 on failure
int fat_delete(fat_fs_t *fs, const char *path);

// Rename a file or directory
// Returns 0 on success, -1 on failure
int fat_rename(fat_fs_t *fs, const char *old_path, const char *new_path);

// Write data to a file at current position
// Returns bytes written, -1 on failure
int fat_write(fat_file_t *file, const void *buffer, uint32_t size);

// Write entire buffer to a file (creates/overwrites)
// Returns 0 on success, -1 on failure
int fat_write_file(fat_fs_t *fs, const char *path, const void *data, uint32_t size);

// Copy a file from src_path to dst_path
// Returns 0 on success, -1 on failure
int fat_copy(fat_fs_t *fs, const char *src_path, const char *dst_path);

// Move a file from src_path to dst_path
// Returns 0 on success, -1 on failure
int fat_move(fat_fs_t *fs, const char *src_path, const char *dst_path);

// Check if a path exists
// Returns 1 if exists, 0 if not, -1 on error
int fat_exists(fat_fs_t *fs, const char *path);

// Get free cluster count
uint32_t fat_get_free_clusters(fat_fs_t *fs);

// Print cache statistics
void fat_cache_stats(void);

// #418: raw, UNLOCKED single-sector primitives for fs/panic.c's on-fault
// panic-log path. These bypass fat_lock()/fat_open()/directory traversal
// entirely and must only be used against a file whose first cluster was
// already resolved earlier (under the normal locked API) - see fs/panic.c.
// `sector` is partition-relative (same units as fat_read_sector() internally
// uses, i.e. fs->part_start_lba has not been added yet).
uint32_t fat_cluster_to_sector(fat_fs_t *fs, uint32_t cluster);
int fat_write_sector(fat_fs_t *fs, uint32_t sector, const void *buffer);

#endif // FAT_H
