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

    // #250 REMOVABLE-VOLUME BACKING. Which BLOCK DEVICE this mount's sectors
    // come from. ZERO means the ROOT device (blk_read/blk_write, which is what
    // every mount in this kernel was before #250, so a memset-to-zero mount is
    // unchanged by construction). A non-zero value N means USB MSC device
    // index N-1, reached through blk_read_aux/blk_write_aux, which bypass the
    // root device's LBA-keyed RAM cache. The +1 bias is not decoration: both
    // mount entry points memset the struct, so a plain index field would make
    // "device 0" and "never set" the same bit pattern, and USB MSC index 0 is
    // exactly the boot stick.
    uint32_t usb_vol_p1;

    // #250 MOUNT GENERATION, the same mechanism as fat_file_t::img_gen (#739)
    // and for the same reason. A hot-plugged volume can be pulled out while
    // handles onto it are open, and the hotplug slot it occupied can then be
    // filled by a DIFFERENT stick. Without a generation, a stale fat_file_t
    // still points at this fat_fs_t and would happily read the new volume's
    // sectors and report them as the old file. Stamped onto every handle by
    // fat_open(); checked by fat_read/fat_seek/fat_readdir_n. 0 on every
    // fixed mount, which is why those paths cost a compare against 0.
    uint32_t vol_gen;
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
    // #115: the entry's packed FAT date/time words, carried on the handle so a
    // stat does not have to re-read the directory sector it has already read.
    // 0 in mtime_date means UNSTAMPED, which is what every entry this kernel
    // created before #115 has on disk, and it must stay distinguishable from a
    // real date: ktime_dos_to_unix_rs() maps it to 0 = "this filesystem does
    // not know", never to 1980-01-01. Meaningless (and left 0) on an ext2- or
    // image-backed handle, exactly like dirent_lba/dirent_off above.
    uint16_t mtime_date;        // dirent modify_date  (bits: 0-4 day, 5-8 month, 9-15 year-1980)
    uint16_t mtime_time;        // dirent modify_time  (bits: 0-4 sec/2, 5-10 min, 11-15 hour)
    uint16_t atime_date;        // dirent access_date  (FAT records no access TIME)
    uint16_t ctime_date;        // dirent create_date
    uint16_t ctime_time;        // dirent create_time
    // #725 ext2-root backing. A non-zero ext2_ino means this handle is served
    // from the ext2 ROOT volume, not from a FAT cluster chain: first_cluster,
    // current_cluster, dirent_lba and dirent_off are then meaningless and must
    // not be used. Only fat_open() sets this, and every handle operation
    // (fat_read/fat_seek/fat_readdir_n/fat_write) branches on it. See the
    // "ONE PLACE" comment above fat_open() in fat.c for why the redirect lives
    // there and not in each caller.
    uint32_t ext2_ino;
    uint32_t ext2_dirpos;       // ext2 readdir byte cursor (directories only)
    // #196 disk-image backing, the THIRD backing this handle can have. Non-zero
    // img_drive (an uppercase drive letter, A or E) means this handle is served
    // from the disk IMAGE mounted on that removable drive, not from a FAT
    // cluster chain and not from ext2: first_cluster/current_cluster/dirent_*
    // and ext2_ino are then all meaningless. img_rel is the path INSIDE the
    // image. Set ONLY by fat_open(), for exactly the same reason the ext2
    // redirect lives there (see the "ONE PLACE" comment in fat.c): putting it
    // anywhere else means some caller forgets it. #725 is the cautionary tale,
    // and #196 shipped the identical defect in miniature - the image was hooked
    // into fat_read_file() but NOT fat_open(), so a whole-file read of a file on
    // a mounted CD worked while INT 21h 3Dh open + 3Fh read of the same file saw
    // nothing at all.
    char     img_drive;
    char     img_rel[128];
    // #739 MOUNT GENERATION. Which disc-in-this-drive the handle was opened
    // against. diskimg re-resolves img_rel on every read, so without this a
    // disc SWAP would silently serve the new disc's file of the same name: both
    // Red Alert discs contain \MAIN.MIX, and the wrong 500 MB archive comes
    // back with no error anywhere. Stamped by fat_open(), checked by fat_read()
    // and fat_readdir_n(), cleared by fat_close(). 0 means "unstamped", which
    // only a handle that was never image-backed can be.
    uint32_t img_gen;
    // #250: the fat_fs_t::vol_gen this handle was opened against. See the
    // vol_gen comment on fat_fs_t. A mismatch means the medium this handle
    // describes is GONE; every operation on it fails rather than reading
    // whatever is in that slot now.
    uint32_t vol_gen;
} fat_file_t;

// Directory iteration
typedef struct {
    fat_fs_t *fs;
    uint32_t cluster;
    uint32_t entry_index;
    uint32_t sector_in_cluster;
} fat_dir_iter_t;

// ===========================================================================
// #725 SHARED ext2-ROOT ROUTING PREDICATE.
//
// When ext2 is the root filesystem (g_root_ext2 != 0) a normal "/" path must be
// served from the ext2 volume, not from the FAT ESP. Historically EACH fat_*
// entry point carried its own copy of that decision, and the copies drifted:
// fat_read_file() had the redirect while fat_open() (the streaming open behind
// INT 21h 3Dh) did not, so on an ext2-root build a DOS program's .EXE loaded
// and every data file it opened failed (#725, Commander Keen 5 EGAGRAPH.CK5).
//
// There is now exactly ONE predicate and ONE path mapper, and they are exported
// so nothing has to hand-roll a third copy. Use these; do not re-derive them.
//   fat_path_on_ext2(fs, path) -> 1 if `path` should be served from ext2.
//     /boot and /EFI are NEVER redirected (UEFI loads the kernel from the ESP).
//   fat_ext2_vol_path(path)    -> the same path as the ext2 volume sees it
//     (strips a "/ext2" mount prefix; everything else passes through).
// Callers keep the ext2-first-then-FAT fallback order: a file that is not on
// ext2 may still be ESP-only, so a miss falls through to FAT.
// ===========================================================================
int         fat_path_on_ext2(fat_fs_t *fs, const char *path);
const char *fat_ext2_vol_path(const char *path);

// Initialize FAT driver
void fat_init(void);

// Mount a FAT partition
int fat_mount(int drive, int partition, fat_fs_t *fs);

// Mount FAT from a specific LBA offset (for GPT partitions or raw FAT)
int fat_mount_lba(int drive, uint32_t start_lba, fat_fs_t *fs);

// #250: mount a FAT volume that lives on a HOT-PLUGGED USB MSC device rather
// than on the root block device. `usb_index` indexes the USB MSC device table.
// Identical to fat_mount_lba() except that every sector this mount ever reads
// or writes is routed to that device (blk_read_aux/blk_write_aux) and the mount
// is stamped with a fresh generation so handles onto it can be invalidated when
// the medium is pulled. `gen` must be non-zero and unique per mount.
int fat_mount_lba_usb(int usb_index, uint32_t start_lba, uint32_t gen, fat_fs_t *fs);

// #250: non-zero if this handle's medium has been removed (its mount
// generation no longer matches the filesystem's). The single definition of
// "this handle is stale"; every fat_* operation asks it.
int fat_handle_stale(const fat_file_t *file);

// Unmount
void fat_unmount(fat_fs_t *fs);

// Open file/directory
int fat_open(fat_fs_t *fs, const char *path, fat_file_t *file);

// Close file
void fat_close(fat_file_t *file);

// #115: set the modify (and optionally access) date on an existing FAT entry,
// the FAT half of utime(2). `atime`/`mtime` are seconds since the UNIX epoch;
// pass -1 to leave one unchanged. FAT's access field stores a DATE ONLY, so an
// atime set here is truncated to midnight - stated because that is a real
// limitation of the on-disk format, not a shortcut. Returns 0 on success.
int fat_set_times(fat_fs_t *fs, const char *path, int64_t atime, int64_t mtime);

// Read from file
int fat_read(fat_file_t *file, void *buffer, uint32_t size);

// Get file size
uint32_t fat_size(fat_file_t *file);

// Seek in file
int fat_seek(fat_file_t *file, uint32_t position);

// Read directory entry (bounded). #591: name_out is filled with the entry's
// reconstructed VFAT long name (up to 255 chars) OR its 8.3 name, TRUNCATED to
// name_cap-1 chars + NUL. It NEVER writes past name_cap regardless of the LFN
// length, so no caller buffer can overflow. name_cap == 0 writes nothing.
int fat_readdir_n(fat_file_t *dir, fat_dir_entry_t *entry, char *name_out, size_t name_cap);

// Read directory entry (convenience wrapper). #591 STRUCTURAL GUARD: name MUST
// be a real array of >= 256 bytes. The negative-array-size check turns a smaller
// buffer, or a decayed char * (sizeof == 8), into a COMPILE error - closing the
// recurrence class behind #404 (fat_delete 64->256) and #490 (dosexec 16->256):
// a length cap only protects the SMALLEST downstream buffer, so a sub-256 sink
// must never compile silently. For a deliberately smaller/bounded buffer, call
// fat_readdir_n(dir, entry, buf, sizeof buf) explicitly (it truncates, safely).
#define FAT_READDIR_REQUIRE_256(name) ((void)sizeof(char[(sizeof(name) >= 256) ? 1 : -1]))
#define fat_readdir(dir, entry, name) \
    (FAT_READDIR_REQUIRE_256(name), fat_readdir_n((dir), (entry), (name), sizeof(name)))

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
MUST_CHECK int fat_write(fat_file_t *file, const void *buffer, uint32_t size);
// #745 local 109: shrink an open FAT file to `new_size` bytes. This is the ONE
// FAT truncation primitive; fat_truncate() below is now fat_truncate_to(f, 0).
//
// WHY IT HAD TO BE GENERALISED. fat_truncate() could only produce an EMPTY
// file, so the kernel had no way to answer ftruncate(fd, n) for n > 0 and the
// userland libc answered it with a no-op that RETURNED SUCCESS. busybox vi
// deliberately opens without O_TRUNC and calls ftruncate() after writing ("we
// do not open file with O_TRUNC ... might reduce amount of data lost on power
// fail"), so on the FAT ESP every save that made a file SHORTER left the old
// tail on disk and reported success.
//
// Growing a file is NOT implemented and is refused rather than silently
// ignored: a grow has to write zero bytes to the medium, which is a different
// operation from freeing clusters.
// Returns 0 on success, -1 on refusal or I/O failure.
int fat_truncate_to(fat_file_t *file, uint32_t new_size);

// #746: empty an open FAT file (O_TRUNC). Frees the cluster chain and persists
// size 0 / cluster 0 to the directory entry. Returns 0, or -1 if the file is
// still not empty on disk - which the caller MUST treat as a failed open, since
// the caller asked for an empty file and did not get one.
MUST_CHECK int fat_truncate(fat_file_t *file);

// Write entire buffer to a file (creates/overwrites)
// Returns 0 on success, -1 on failure
MUST_CHECK int fat_write_file(fat_fs_t *fs, const char *path, const void *data, uint32_t size);

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
// RETURNS 0 ON SUCCESS, negative on failure - the SAME polarity as every other
// status-returning fat_* entry point in this header.
//
// #742: it used to return blk_write()'s SECTOR COUNT (1 = success, <= 0 =
// failure) while fat_write_file() two screens up returned 0 = success. Two
// polarities behind one MUST_CHECK attribute in one header is not a naming
// nit: fs/panic.c wrote `if (fat_write_sector(...) != 0) kprintf("FAILED")`
// and so printed a disk-failure alarm on every SUCCESSFUL panic-slot write,
// six or more times per boot, which is how a real alarm gets trained out of
// people. The attribute forced the call sites to LOOK at the value; it could
// not tell them which way round it was. Normalising the polarity removes the
// choice. All call sites are in fs/fat.c and fs/panic.c.
MUST_CHECK int fat_write_sector(fat_fs_t *fs, uint32_t sector, const void *buffer);

// #554: genuine FAT (ESP: /boot, /EFI) permission/attribute support. FAT has
// no uid/gid/mode - these operate on the real on-disk FAT_ATTR_* byte in the
// directory entry, unlike perms.c's path-keyed overlay (which is what backs
// ext2/POSIX paths). Both operate against the single mounted g_fat_fs.
// fat_set_readonly: toggle FAT_ATTR_READ_ONLY on disk. Returns 0 on success.
int fat_set_readonly(const char *path, int readonly);
// fat_get_attr_info: read back the raw attribute byte + directory bit for
// display (Files Properties, details columns, ls-style tools). Returns 0 on
// success, -1 if the path does not exist on the FAT volume.
int fat_get_attr_info(const char *path, uint8_t *attr_out, int *is_dir_out);

#endif // FAT_H
