// fat.c - FAT12/FAT16/FAT32 Filesystem Driver
#include "fat.h"
#include "ext2.h"   // #99 Phase C: ext2-as-root hook (g_root_ext2, ext2_read_whole)

#include "../drivers/ata.h"
#include "blockdev.h"   // #307: route sector I/O to ATA or USB MSC root
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../proc/process.h"

// Sector buffer
static uint8_t sector_buf[512];

// ---------------------------------------------------------------------------
// FAT global lock (fixes filesystem corruption under concurrent writers).
//
// The driver uses the shared global `sector_buf` for read-modify-write of FAT
// table sectors AND directory sectors. The kernel scheduler is preemptive, and
// FAT operations are now issued from MANY concurrent contexts (the compositor
// saving /UIPROFIL.YML, background services writing logs, and especially Win16
// apps, which run as their own processes via proc_create and write /WIN16LOG.TXT
// frequently). With the shared buffer, one context could be preempted between
// the read of a directory/FAT sector and its write-back, another FAT writer
// would clobber sector_buf, and the first context then committed garbage to
// disk: kernel log-string fragments appearing as directory entries and
// cross-linked clusters (cross-linked /ICONS, /THEMES, truncated files). fsck
// reported "Both FATs appear to be corrupt".
//
// Fix: a single recursive lock held across each whole public FAT operation, so
// the entire sector_buf read-modify-write span (including multi-sector scan
// loops) runs without another FAT context interleaving. The lock yields the CPU
// while contended (it does NOT keep preemption disabled during disk I/O), so
// non-FAT work keeps running; preemption is only briefly disabled around the
// test-and-set itself. Recursive (owner-tracked) so nested public calls
// (fat_copy -> fat_read_file/fat_write_file, fat_move -> fat_rename) are safe.
static volatile void *g_fat_lock_owner = 0;
static volatile int   g_fat_lock_depth = 0;

static inline void *fat_lock_self(void) {
    void *p = (void *)proc_current();
    return p ? p : (void *)1;   // sentinel for the early-boot single-threaded path
}

static void fat_lock(void) {
    void *me = fat_lock_self();
    for (;;) {
        bool old = sched_set_preemption(false);
        if (g_fat_lock_owner == 0 || g_fat_lock_owner == me) {
            g_fat_lock_owner = me;
            g_fat_lock_depth++;
            sched_set_preemption(old);
            return;
        }
        sched_set_preemption(old);
        sched_schedule();   // another context holds it; yield and retry
    }
}

static void fat_unlock(void) {
    bool old = sched_set_preemption(false);
    if (g_fat_lock_depth > 0 && --g_fat_lock_depth == 0)
        g_fat_lock_owner = 0;
    sched_set_preemption(old);
}

// Inner (unlocked) implementations; the public wrappers below take the FAT lock.
static int      fat_open_inner(fat_fs_t *fs, const char *path, fat_file_t *file);
static int      fat_read_inner(fat_file_t *file, void *buffer, uint32_t size);
static int      fat_readdir_inner(fat_file_t *dir, fat_dir_entry_t *entry, char *name_out);
static void     fat_list_dir_inner(fat_fs_t *fs, const char *path);
static int      fat_create_inner(fat_fs_t *fs, const char *path);
static int      fat_mkdir_inner(fat_fs_t *fs, const char *path);
static int      fat_delete_inner(fat_fs_t *fs, const char *path);
static int      fat_rename_inner(fat_fs_t *fs, const char *old_path, const char *new_path);
static int      fat_write_inner(fat_file_t *file, const void *buffer, uint32_t size);
static int      fat_copy_inner(fat_fs_t *fs, const char *src_path, const char *dst_path);
static int      fat_move_inner(fat_fs_t *fs, const char *src_path, const char *dst_path);
static int      fat_write_file_inner(fat_fs_t *fs, const char *path, const void *data, uint32_t size);
static int      fat_exists_inner(fat_fs_t *fs, const char *path);
static uint32_t fat_get_free_clusters_inner(fat_fs_t *fs);

int fat_open(fat_fs_t *fs, const char *path, fat_file_t *file) {
    fat_lock(); int r = fat_open_inner(fs, path, file); fat_unlock(); return r;
}
int fat_read(fat_file_t *file, void *buffer, uint32_t size) {
    fat_lock(); int r = fat_read_inner(file, buffer, size); fat_unlock(); return r;
}
int fat_readdir(fat_file_t *dir, fat_dir_entry_t *entry, char *name_out) {
    fat_lock(); int r = fat_readdir_inner(dir, entry, name_out); fat_unlock(); return r;
}
void fat_list_dir(fat_fs_t *fs, const char *path) {
    fat_lock(); fat_list_dir_inner(fs, path); fat_unlock();
}
// #99 cutover: true when ext2 is the root fs and `path` is a normal "/" path
// that should be served from the ext2 volume. The UEFI ESP paths (/boot, /EFI)
// are never redirected; they must stay on the FAT ESP the firmware boots from.
static int fat_path_on_ext2(fat_fs_t *fs, const char *path) {
    extern fat_fs_t g_fat_fs;
    if (!g_root_ext2 || fs != &g_fat_fs || !path || path[0] != '/') return 0;
    if (path[1]=='b'&&path[2]=='o'&&path[3]=='o'&&path[4]=='t'&&(path[5]=='/'||path[5]==0)) return 0;
    if (path[1]=='E'&&path[2]=='F'&&path[3]=='I'&&(path[4]=='/'||path[4]==0)) return 0;
    return 1;
}

// #316: map a fat-layer path to its ext2-volume path. Userland strips the
// "/ext2" mount prefix in sys_open (ext2_relpath); kernel-internal callers of
// fat_*_file may still pass "/ext2/...", so strip it here too ("/ext2/a"->"/a",
// "/ext2"->"/"). All other paths (incl. /HOME, /CONFIG, /APPS) pass through
// unchanged, so existing ext2-root persistence is unaffected.
static const char *ext2_vol_path(const char *path) {
    if (path && path[0]=='/' && path[1]=='e' && path[2]=='x' && path[3]=='t' &&
        path[4]=='2' && (path[5]=='/' || path[5]==0))
        return (path[5]==0) ? "/" : (path+5);
    return path;
}

int fat_create(fat_fs_t *fs, const char *path) {
    // Create an (empty) file on ext2 when it is root; ext2 has no standalone
    // create, so an empty write produces a zero-length file.
    if (fat_path_on_ext2(fs, path)) return (ext2_write_file(ext2_vol_path(path), "", 0) == 0) ? 0 : -1;
    fat_lock(); int r = fat_create_inner(fs, path); fat_unlock(); return r;
}
int fat_mkdir(fat_fs_t *fs, const char *path) {
    if (fat_path_on_ext2(fs, path)) return (ext2_mkdir(ext2_vol_path(path)) == 0) ? 0 : -1;
    fat_lock(); int r = fat_mkdir_inner(fs, path); fat_unlock(); return r;
}
int fat_delete(fat_fs_t *fs, const char *path) {
    if (fat_path_on_ext2(fs, path)) {
        if (ext2_unlink(ext2_vol_path(path)) == 0) return 0;
        // not on ext2 (or failed): fall back to FAT (file may be ESP-only)
    }
    fat_lock(); int r = fat_delete_inner(fs, path); fat_unlock(); return r;
}
int fat_rename(fat_fs_t *fs, const char *old_path, const char *new_path) {
    // ext2 has no in-place rename; emulate with copy + delete (both of which
    // route to ext2 via the hooks above). Works for regular files.
    if (fat_path_on_ext2(fs, old_path) && fat_path_on_ext2(fs, new_path)) {
        if (fat_copy(fs, old_path, new_path) != 0) return -1;
        return fat_delete(fs, old_path);
    }
    fat_lock(); int r = fat_rename_inner(fs, old_path, new_path); fat_unlock(); return r;
}
int fat_write(fat_file_t *file, const void *buffer, uint32_t size) {
    fat_lock(); int r = fat_write_inner(file, buffer, size); fat_unlock(); return r;
}
int fat_copy(fat_fs_t *fs, const char *src_path, const char *dst_path) {
    // #316: fat_copy_inner uses raw FAT file handles (fat_open/read/write),
    // which cannot see files that live only on the ext2 root volume. When
    // either endpoint is on ext2, do a routed whole-file read+write so the
    // copy actually reads from / writes to ext2. This also fixes ext2 rename
    // (fat_rename does copy+delete for ext2 paths).
    if (fat_path_on_ext2(fs, src_path) || fat_path_on_ext2(fs, dst_path)) {
        uint32_t sz = 0;
        void *data = fat_read_file(fs, src_path, &sz);  // ext2-first read
        if (!data) return -1;
        int rc = fat_write_file(fs, dst_path, data, sz); // ext2 write if dst on ext2
        kfree(data);
        return rc;
    }
    fat_lock(); int r = fat_copy_inner(fs, src_path, dst_path); fat_unlock(); return r;
}
int fat_move(fat_fs_t *fs, const char *src_path, const char *dst_path) {
    fat_lock(); int r = fat_move_inner(fs, src_path, dst_path); fat_unlock(); return r;
}
int fat_write_file(fat_fs_t *fs, const char *path, const void *data, uint32_t size) {
    // (#133) Complete the ext2-root cutover: "/" writes must land on the ext2
    // root, matching fat_read_file (which already serves "/" from ext2). Without
    // this, writes went to the FAT ESP while reads came from ext2 - so a written
    // file (e.g. C:\WINDOWS\WIN.INI under /WINDIR) could never be read back.
    // /boot and /EFI stay on the FAT ESP (excluded by fat_path_on_ext2).
    if (fat_path_on_ext2(fs, path)) return (ext2_write_file(ext2_vol_path(path), data, size) == 0) ? 0 : -1;
    fat_lock(); int r = fat_write_file_inner(fs, path, data, size); fat_unlock(); return r;
}
int fat_exists(fat_fs_t *fs, const char *path) {
    fat_lock(); int r = fat_exists_inner(fs, path); fat_unlock(); return r;
}
uint32_t fat_get_free_clusters(fat_fs_t *fs) {
    fat_lock(); uint32_t r = fat_get_free_clusters_inner(fs); fat_unlock(); return r;
}

// Helper to extract channel and drive from drive ID
// Drive ID: 0=Primary Master, 1=Primary Slave, 2=Secondary Master, 3=Secondary Slave
static inline uint8_t drive_to_channel(int drive) {
    return (drive >> 1) & 1;  // 0,1 -> 0;  2,3 -> 1
}

static inline uint8_t drive_to_unit(int drive) {
    return drive & 1;  // 0,2 -> master(0);  1,3 -> slave(1)
}

// Read sector from partition
static int fat_read_sector(fat_fs_t *fs, uint32_t sector, void *buffer) {
    uint32_t lba = fs->part_start_lba + sector;
    uint8_t channel = drive_to_channel(fs->drive);
    uint8_t unit = drive_to_unit(fs->drive);
    return blk_read(channel, unit, lba, 1, buffer);
}

// Read multiple sectors
static int fat_read_sectors(fat_fs_t *fs, uint32_t sector, uint32_t count, void *buffer) {
    uint32_t lba = fs->part_start_lba + sector;
    uint8_t channel = drive_to_channel(fs->drive);
    uint8_t unit = drive_to_unit(fs->drive);
    return blk_read(channel, unit, lba, count, buffer);
}

// Get next cluster in chain.
//
// Uses a LOCAL sector buffer instead of the shared global sector_buf. This is
// called once per cluster while reading a file (the cluster-chain walk). With
// the global buffer, a concurrent FAT operation in another preemptible context
// (e.g. a background service writing its log, the compositor saving its
// profile) could clobber sector_buf between the FAT-table read and the
// next-cluster extraction, making the read jump into a different file's
// clusters. The file then decodes as fragments of several files stitched
// together (seen as a wallpaper made of shifting "magazine clippings" with
// wrong colors). A per-call buffer makes the chain walk race-free.
// Single-entry FAT-sector read cache. A contiguous file walks many clusters
// that all live in the same FAT sector (a FAT32 sector holds 128 entries), so
// caching the last-read FAT sector turns one disk read per cluster into one per
// 128 clusters. Invalidated on every FAT-region write (see fat_write_sector*).
static fat_fs_t *g_fatcache_fs = 0;
static uint32_t  g_fatcache_sector = 0xFFFFFFFFu;
static uint8_t   g_fatcache_buf[512];
static inline void fat_cache_invalidate(void) {
    g_fatcache_fs = 0;
    g_fatcache_sector = 0xFFFFFFFFu;
}
// Return a pointer to a (cached) FAT sector's 512 bytes, or NULL on read error.
// The single-entry cache is invalidated on every FAT-region write
// (fat_write_sector / fat_write_sectors), so a chain walk that follows a freshly
// written link always re-reads it. (Verified innocent of the multi-cluster
// write corruption, which was a navigation bug in fat_write, not the cache.)
static uint8_t *fat_fat_sector(fat_fs_t *fs, uint32_t fat_sector) {
    if (g_fatcache_fs == fs && g_fatcache_sector == fat_sector) {
        return g_fatcache_buf;
    }
    if (fat_read_sector(fs, fat_sector, g_fatcache_buf) <= 0) {
        fat_cache_invalidate();
        return 0;
    }
    g_fatcache_fs = fs;
    g_fatcache_sector = fat_sector;
    return g_fatcache_buf;
}

static uint32_t fat_next_cluster(fat_fs_t *fs, uint32_t cluster) {
    uint32_t fat_offset;
    uint32_t fat_sector;
    uint32_t entry_offset;
    uint32_t next_cluster;
    uint8_t *fbuf;        // points into the cached FAT-sector buffer

    if (fs->fat_type == FAT_TYPE_16) {
        fat_offset = cluster * 2;
        fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
        entry_offset = fat_offset % fs->bytes_per_sector;

        if (!(fbuf = fat_fat_sector(fs, fat_sector))) {
            return 0;
        }

        next_cluster = *(uint16_t *)(fbuf + entry_offset);
        if (next_cluster >= FAT16_EOC) {
            return 0;  // End of chain
        }
    } else if (fs->fat_type == FAT_TYPE_32) {
        fat_offset = cluster * 4;
        fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
        entry_offset = fat_offset % fs->bytes_per_sector;

        if (!(fbuf = fat_fat_sector(fs, fat_sector))) {
            return 0;
        }

        next_cluster = *(uint32_t *)(fbuf + entry_offset) & 0x0FFFFFFF;
        if (next_cluster >= FAT32_EOC) {
            return 0;  // End of chain
        }
    } else {
        // FAT12 - more complex
        fat_offset = cluster + (cluster / 2);
        fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
        entry_offset = fat_offset % fs->bytes_per_sector;

        if (!(fbuf = fat_fat_sector(fs, fat_sector))) {
            return 0;
        }

        if (cluster & 1) {
            next_cluster = (*(uint16_t *)(fbuf + entry_offset)) >> 4;
        } else {
            next_cluster = (*(uint16_t *)(fbuf + entry_offset)) & 0x0FFF;
        }

        if (next_cluster >= 0xFF8) {
            return 0;  // End of chain
        }
    }

    return next_cluster;
}

// Convert cluster to LBA
static uint32_t cluster_to_lba(fat_fs_t *fs, uint32_t cluster) {
    return fs->data_start_lba + (cluster - 2) * fs->sectors_per_cluster;
}

// #418: public wrapper so fs/panic.c can resolve a pre-allocated file's first
// cluster to a partition-relative sector number ONCE at boot (armed under the
// normal fat_lock()), then cache that number and call fat_write_sector()
// directly (unlocked) from fault context forever after. Returns the same
// partition-relative "sector" units fat_read_sector()/fat_write_sector() take
// (i.e. fs->part_start_lba has NOT been added yet).
uint32_t fat_cluster_to_sector(fat_fs_t *fs, uint32_t cluster) {
    return cluster_to_lba(fs, cluster);
}

// Read a cluster
static int fat_read_cluster(fat_fs_t *fs, uint32_t cluster, void *buffer) {
    uint32_t lba = cluster_to_lba(fs, cluster);
    return fat_read_sectors(fs, lba, fs->sectors_per_cluster, buffer);
}

// ============================================
// Write Helper Functions
// ============================================

// Write sector to partition
//
// #418: intentionally NOT static. fs/panic.c needs to overwrite an
// ALREADY-ALLOCATED file's first sector directly from fault context (no
// fat_lock(), no directory traversal, no delete+recreate - see fs/panic.c's
// file header for why fat_write_file_inner()'s normal delete+create+patch-size
// sequence is unsafe to reuse there). This is the same raw primitive every
// other write path in this file already funnels through; exposing it does not
// change any existing locked call site.
int fat_write_sector(fat_fs_t *fs, uint32_t sector, const void *buffer) {
    fat_cache_invalidate();   // a FAT-region write may change next-cluster links
    uint32_t lba = fs->part_start_lba + sector;
    uint8_t channel = drive_to_channel(fs->drive);
    uint8_t unit = drive_to_unit(fs->drive);
    return blk_write(channel, unit, lba, 1, buffer);
}

// Write multiple sectors
static int fat_write_sectors(fat_fs_t *fs, uint32_t sector, uint32_t count, const void *buffer) {
    fat_cache_invalidate();
    uint32_t lba = fs->part_start_lba + sector;
    uint8_t channel = drive_to_channel(fs->drive);
    uint8_t unit = drive_to_unit(fs->drive);
    return blk_write(channel, unit, lba, count, buffer);
}

// Write a cluster
static int fat_write_cluster(fat_fs_t *fs, uint32_t cluster, const void *buffer) {
    uint32_t lba = cluster_to_lba(fs, cluster);
    return fat_write_sectors(fs, lba, fs->sectors_per_cluster, buffer);
}

// Set FAT entry value (update the FAT table)
static int fat_set_fat_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value) {
    uint32_t fat_offset;
    uint32_t fat_sector;
    uint32_t entry_offset;

    if (fs->fat_type == FAT_TYPE_16) {
        fat_offset = cluster * 2;
    } else if (fs->fat_type == FAT_TYPE_32) {
        fat_offset = cluster * 4;
    } else {
        fat_offset = cluster + (cluster / 2);
    }

    fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
    entry_offset = fat_offset % fs->bytes_per_sector;

    // Read current FAT sector
    if (fat_read_sector(fs, fat_sector, sector_buf) <= 0) {
        return -1;
    }

    // Update entry
    if (fs->fat_type == FAT_TYPE_16) {
        *(uint16_t *)(sector_buf + entry_offset) = (uint16_t)value;
    } else if (fs->fat_type == FAT_TYPE_32) {
        // Preserve upper 4 bits
        uint32_t existing = *(uint32_t *)(sector_buf + entry_offset);
        *(uint32_t *)(sector_buf + entry_offset) = (existing & 0xF0000000) | (value & 0x0FFFFFFF);
    } else {
        // FAT12
        uint16_t existing = *(uint16_t *)(sector_buf + entry_offset);
        if (cluster & 1) {
            existing = (existing & 0x000F) | ((value & 0x0FFF) << 4);
        } else {
            existing = (existing & 0xF000) | (value & 0x0FFF);
        }
        *(uint16_t *)(sector_buf + entry_offset) = existing;
    }

    // Write back FAT sector (to both FAT copies)
    if (fat_write_sector(fs, fat_sector, sector_buf) <= 0) {
        return -1;
    }

    // Write to second FAT if present
    if (fs->num_fats > 1) {
        uint32_t fat2_sector = fat_sector + fs->fat_size;
        fat_write_sector(fs, fat2_sector, sector_buf);
    }

    return 0;
}

// Find a free cluster
static uint32_t fat_alloc_cluster(fat_fs_t *fs) {
    uint32_t current_sector = 0xFFFFFFFF;

    for (uint32_t cluster = 2; cluster < fs->cluster_count + 2; cluster++) {
        uint32_t fat_offset;
        uint32_t fat_sector;
        uint32_t entry_offset;
        uint32_t entry_value;

        if (fs->fat_type == FAT_TYPE_16) {
            fat_offset = cluster * 2;
        } else if (fs->fat_type == FAT_TYPE_32) {
            fat_offset = cluster * 4;
        } else {
            fat_offset = cluster + (cluster / 2);
        }

        fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
        entry_offset = fat_offset % fs->bytes_per_sector;

        if (fat_sector != current_sector) {
            if (fat_read_sector(fs, fat_sector, sector_buf) <= 0) {
                return 0;
            }
            current_sector = fat_sector;
        }

        if (fs->fat_type == FAT_TYPE_16) {
            entry_value = *(uint16_t *)(sector_buf + entry_offset);
        } else if (fs->fat_type == FAT_TYPE_32) {
            entry_value = *(uint32_t *)(sector_buf + entry_offset) & 0x0FFFFFFF;
        } else {
            if (cluster & 1) {
                entry_value = (*(uint16_t *)(sector_buf + entry_offset)) >> 4;
            } else {
                entry_value = (*(uint16_t *)(sector_buf + entry_offset)) & 0x0FFF;
            }
        }

        if (entry_value == 0) {
            // Found free cluster - mark as end of chain
            uint32_t eoc = (fs->fat_type == FAT_TYPE_32) ? FAT32_EOC :
                          (fs->fat_type == FAT_TYPE_16) ? FAT16_EOC : 0xFFF;
            if (fat_set_fat_entry(fs, cluster, eoc) == 0) {
                    if (fs->free_cluster_count > 0) fs->free_cluster_count--;
                return cluster;
            }
            return 0;
        }
    }

    return 0;  // No free clusters
}

// Free a cluster chain
static int fat_free_cluster_chain(fat_fs_t *fs, uint32_t start_cluster) {
    if (start_cluster < 2) return 0;

    uint32_t cluster = start_cluster;
    while (cluster != 0 && cluster < fs->cluster_count + 2) {
        uint32_t next = fat_next_cluster(fs, cluster);
        if (fat_set_fat_entry(fs, cluster, 0) != 0) {
            return -1;
        }
        fs->free_cluster_count++;
        cluster = next;
    }
    return 0;
}

// Convert string to 8.3 FAT name format
static void str_to_fat_name(const char *name, uint8_t *fat_name) {
    memset(fat_name, ' ', 11);

    int i = 0, j = 0;

    // Copy name part (up to 8 chars)
    while (name[i] && name[i] != '.' && j < 8) {
        char c = name[i++];
        if (c >= 'a' && c <= 'z') c -= 32;  // Uppercase
        fat_name[j++] = c;
    }

    // Skip to extension
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') i++;

    // Copy extension (up to 3 chars)
    j = 8;
    while (name[i] && j < 11) {
        char c = name[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        fat_name[j++] = c;
    }
}

// (fat_name_match removed: replaced by the LFN-aware fat_lookup / fat_ci_eq.)

// Convert 8.3 name to readable string
static void fat_name_to_str(const uint8_t *name83, char *str_out) {
    int i, j = 0;

    // Copy name
    for (i = 0; i < 8 && name83[i] != ' '; i++) {
        str_out[j++] = name83[i];
    }

    // Add extension
    if (name83[8] != ' ') {
        str_out[j++] = '.';
        for (i = 8; i < 11 && name83[i] != ' '; i++) {
            str_out[j++] = name83[i];
        }
    }

    str_out[j] = '\0';
}

// Initialize FAT driver
void fat_init(void) {
    kprintf("[FAT] FAT filesystem driver initialized\n");
}

// Mount a FAT partition
int fat_mount(int drive, int partition, fat_fs_t *fs) {
    memset(fs, 0, sizeof(fat_fs_t));
    fs->drive = drive;
    fs->partition = partition;

    uint8_t channel = drive_to_channel(drive);
    uint8_t unit = drive_to_unit(drive);

    kprintf("[FAT] Mounting drive %d (channel %d, unit %d), partition %d\n",
            drive, channel, unit, partition);

    // Read MBR
    if (blk_read(channel, unit, 0, 1, sector_buf) <= 0) {
        kprintf("[FAT] Failed to read MBR\n");
        return -1;
    }

    mbr_t *mbr = (mbr_t *)sector_buf;

    // Check signature
    if (mbr->signature != 0xAA55) {
        kprintf("[FAT] Invalid MBR signature\n");
        return -1;
    }

    // Get partition info
    if (partition < 0 || partition > 3) {
        kprintf("[FAT] Invalid partition number\n");
        return -1;
    }

    mbr_partition_t *part = &mbr->partitions[partition];
    if (part->type == 0) {
        kprintf("[FAT] Partition %d is empty\n", partition);
        return -1;
    }

    // Check for valid FAT partition types
    uint8_t ptype = part->type;
    if (ptype != 0x01 && ptype != 0x04 && ptype != 0x06 && ptype != 0x0B &&
        ptype != 0x0C && ptype != 0x0E && ptype != 0x0F) {
        kprintf("[FAT] Partition %d type 0x%02x is not FAT\n", partition, ptype);
        if (ptype == 0xEE) {
            kprintf("[FAT] GPT partition - not supported yet\n");
        }
        return -1;
    }

    fs->part_start_lba = part->start_lba;
    fs->part_sectors = part->sector_count;

    kprintf("[FAT] Partition %d: LBA %u, %u sectors, type 0x%02x\n",
            partition, fs->part_start_lba, fs->part_sectors, part->type);

    // Read boot sector
    if (fat_read_sector(fs, 0, sector_buf) <= 0) {
        kprintf("[FAT] Failed to read boot sector\n");
        return -1;
    }

    fat_boot_sector_t *bs = (fat_boot_sector_t *)sector_buf;

    // Parse BPB
    fs->bytes_per_sector = bs->bytes_per_sector;
    fs->sectors_per_cluster = bs->sectors_per_cluster;
    fs->reserved_sectors = bs->reserved_sectors;
    fs->num_fats = bs->num_fats;
    fs->root_entry_count = bs->root_entries;

    // Validate critical BPB fields to avoid divide by zero
    if (fs->bytes_per_sector == 0 || fs->sectors_per_cluster == 0) {
        kprintf("[FAT] Invalid BPB: bytes/sector=%u sectors/cluster=%u\n",
                fs->bytes_per_sector, fs->sectors_per_cluster);
        return -1;
    }

    if (bs->total_sectors_16 != 0) {
        fs->total_sectors = bs->total_sectors_16;
    } else {
        fs->total_sectors = bs->total_sectors_32;
    }

    if (bs->fat_size_16 != 0) {
        fs->fat_size = bs->fat_size_16;
    } else {
        fs->fat_size = bs->ext.fat32.fat_size_32;
    }

    // Calculate locations
    fs->fat_start_lba = fs->reserved_sectors;

    fs->root_dir_sectors = ((fs->root_entry_count * 32) + (fs->bytes_per_sector - 1))
                            / fs->bytes_per_sector;

    fs->root_start_lba = fs->reserved_sectors + (fs->num_fats * fs->fat_size);
    fs->data_start_lba = fs->root_start_lba + fs->root_dir_sectors;

    // Calculate cluster count and determine FAT type
    uint32_t data_sectors = fs->total_sectors - fs->data_start_lba;
    fs->cluster_count = data_sectors / fs->sectors_per_cluster;

    if (fs->cluster_count < 4085) {
        fs->fat_type = FAT_TYPE_12;
    } else if (fs->cluster_count < 65525) {
        fs->fat_type = FAT_TYPE_16;
    } else {
        fs->fat_type = FAT_TYPE_32;
        fs->root_cluster = bs->ext.fat32.root_cluster;
    }

    // Get volume label
    if (fs->fat_type == FAT_TYPE_32) {
        memcpy(fs->volume_label, bs->ext.fat32.volume_label, 11);
    } else {
        memcpy(fs->volume_label, bs->ext.fat16.volume_label, 11);
    }
    fs->volume_label[11] = '\0';

    // Trim trailing spaces
    for (int i = 10; i >= 0 && fs->volume_label[i] == ' '; i--) {
        fs->volume_label[i] = '\0';
    }

    fs->mounted = 1;
    // Count free clusters once at mount time (cached for taskbar gauge)
    fs->free_cluster_count = 0;
    {
        uint32_t cur_sec = 0xFFFFFFFF;
        for (uint32_t c = 2; c < fs->cluster_count + 2; c++) {
            uint32_t fo = (fs->fat_type == FAT_TYPE_32) ? c * 4 : c * 2;
            uint32_t fs_sec = fs->reserved_sectors + (fo / fs->bytes_per_sector);
            uint32_t eo = fo % fs->bytes_per_sector;
            if (fs_sec != cur_sec) {
                if (fat_read_sector(fs, fs_sec, sector_buf) <= 0) break;
                cur_sec = fs_sec;
            }
            uint32_t ev = (fs->fat_type == FAT_TYPE_32) ?
                (*(uint32_t *)(sector_buf + eo) & 0x0FFFFFFF) :
                *(uint16_t *)(sector_buf + eo);
            if (ev == 0) fs->free_cluster_count++;
        }
        kprintf("[FAT] Free clusters: %u\n", fs->free_cluster_count);
    }

    kprintf("[FAT] Mounted FAT%d filesystem: %s\n", fs->fat_type, fs->volume_label);
    kprintf("[FAT] %u clusters, %u bytes/cluster\n",
            fs->cluster_count, fs->sectors_per_cluster * fs->bytes_per_sector);

    return 0;
}

// Mount FAT from a specific LBA offset (for GPT partitions or raw FAT volumes)
int fat_mount_lba(int drive, uint32_t start_lba, fat_fs_t *fs) {
    memset(fs, 0, sizeof(fat_fs_t));
    fs->drive = drive;
    fs->partition = -1;  // Indicates raw LBA mount
    fs->part_start_lba = start_lba;

    uint8_t channel = drive_to_channel(drive);
    uint8_t unit = drive_to_unit(drive);

    kprintf("[FAT] Mounting from drive %d (channel %d, unit %d), LBA %u\n",
            drive, channel, unit, start_lba);

    // Read boot sector directly from specified LBA
    if (blk_read(channel, unit, start_lba, 1, sector_buf) <= 0) {
        kprintf("[FAT] Failed to read boot sector at LBA %u\n", start_lba);
        return -1;
    }

    // Debug: dump first 16 bytes of boot sector
    kprintf("[FAT] Boot sector bytes: ");
    for (int i = 0; i < 16; i++) {
        kprintf("%02x ", sector_buf[i]);
    }
    kprintf("\n");

    fat_boot_sector_t *bs = (fat_boot_sector_t *)sector_buf;

    // Check for FAT signature (some basic validation)
    // Valid FAT boot sector should have a jump instruction at start
    if (bs->jmp[0] != 0xEB && bs->jmp[0] != 0xE9) {
        kprintf("[FAT] Invalid boot sector (no jump instruction)\n");
        return -1;
    }

    // Parse BPB
    fs->bytes_per_sector = bs->bytes_per_sector;
    fs->sectors_per_cluster = bs->sectors_per_cluster;
    fs->reserved_sectors = bs->reserved_sectors;
    fs->num_fats = bs->num_fats;
    fs->root_entry_count = bs->root_entries;

    // Validate critical BPB fields
    if (fs->bytes_per_sector == 0 || fs->sectors_per_cluster == 0) {
        kprintf("[FAT] Invalid BPB: bytes/sector=%u sectors/cluster=%u\n",
                fs->bytes_per_sector, fs->sectors_per_cluster);
        return -1;
    }

    if (bs->total_sectors_16 != 0) {
        fs->total_sectors = bs->total_sectors_16;
    } else {
        fs->total_sectors = bs->total_sectors_32;
    }

    fs->part_sectors = fs->total_sectors;

    if (bs->fat_size_16 != 0) {
        fs->fat_size = bs->fat_size_16;
    } else {
        fs->fat_size = bs->ext.fat32.fat_size_32;
    }

    // Calculate locations
    fs->fat_start_lba = fs->reserved_sectors;

    fs->root_dir_sectors = ((fs->root_entry_count * 32) + (fs->bytes_per_sector - 1))
                            / fs->bytes_per_sector;

    fs->root_start_lba = fs->reserved_sectors + (fs->num_fats * fs->fat_size);
    fs->data_start_lba = fs->root_start_lba + fs->root_dir_sectors;

    // Calculate cluster count and determine FAT type
    uint32_t data_sectors = fs->total_sectors - fs->data_start_lba;
    fs->cluster_count = data_sectors / fs->sectors_per_cluster;

    if (fs->cluster_count < 4085) {
        fs->fat_type = FAT_TYPE_12;
    } else if (fs->cluster_count < 65525) {
        fs->fat_type = FAT_TYPE_16;
    } else {
        fs->fat_type = FAT_TYPE_32;
        fs->root_cluster = bs->ext.fat32.root_cluster;
    }

    // Copy volume label
    const uint8_t *label;
    if (fs->fat_type == FAT_TYPE_32) {
        label = bs->ext.fat32.volume_label;
    } else {
        label = bs->ext.fat16.volume_label;
    }
    memcpy(fs->volume_label, label, 11);
    fs->volume_label[11] = '\0';

    // Trim trailing spaces
    for (int i = 10; i >= 0 && fs->volume_label[i] == ' '; i--) {
        fs->volume_label[i] = '\0';
    }

    fs->mounted = 1;
    // Count free clusters once at mount time (cached for taskbar gauge)
    fs->free_cluster_count = 0;
    {
        uint32_t cur_sec = 0xFFFFFFFF;
        for (uint32_t c = 2; c < fs->cluster_count + 2; c++) {
            uint32_t fo = (fs->fat_type == FAT_TYPE_32) ? c * 4 : c * 2;
            uint32_t fs_sec = fs->reserved_sectors + (fo / fs->bytes_per_sector);
            uint32_t eo = fo % fs->bytes_per_sector;
            if (fs_sec != cur_sec) {
                if (fat_read_sector(fs, fs_sec, sector_buf) <= 0) break;
                cur_sec = fs_sec;
            }
            uint32_t ev = (fs->fat_type == FAT_TYPE_32) ?
                (*(uint32_t *)(sector_buf + eo) & 0x0FFFFFFF) :
                *(uint16_t *)(sector_buf + eo);
            if (ev == 0) fs->free_cluster_count++;
        }
        kprintf("[FAT] Free clusters: %u\n", fs->free_cluster_count);
    }

    kprintf("[FAT] Mounted FAT%d filesystem: %s\n", fs->fat_type, fs->volume_label);
    kprintf("[FAT] %u clusters, %u bytes/cluster\n",
            fs->cluster_count, fs->sectors_per_cluster * fs->bytes_per_sector);

    return 0;
}

// Unmount
void fat_unmount(fat_fs_t *fs) {
    if (fs->mounted) {
        fs->mounted = 0;
        kprintf("[FAT] Filesystem unmounted\n");
    }
}

// ===================== VFAT long-filename (LFN) core =====================
// Checksum of the 11-byte 8.3 name, stamped into every LFN entry of the set.
static uint8_t lfn_checksum(const uint8_t *short11) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + short11[i]);
    return sum;
}

// True if `name` cannot be stored as a clean uppercase 8.3 name (needs LFN).
static int fat_name_needs_lfn(const char *name) {
    int base = 0, ext = 0, dots = 0, in_ext = 0;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (c == '.') { dots++; in_ext = 1; continue; }
        if (c >= 'a' && c <= 'z') return 1;                 // preserve case
        if (c=='+'||c==','||c==';'||c=='='||c=='['||c==']'||c==' ') return 1;
        if (in_ext) ext++; else base++;
    }
    if (dots > 1 || base > 8 || ext > 3 || base == 0) return 1;
    return 0;
}

// Map a logical directory-entry index to its physical sector + byte offset.
// allocate=1 extends the directory's cluster chain when idx is past the end
// (cluster dirs only; the FAT16 root region is fixed-size). Returns 0 / -1.
static int dir_locate(fat_fs_t *fs, fat_file_t *dir, uint32_t idx,
                      uint32_t *sector, uint32_t *off, int allocate) {
    uint32_t per_sector = fs->bytes_per_sector / 32;
    if (dir->first_cluster == 0 && fs->fat_type != FAT_TYPE_32) {
        if (idx >= fs->root_entry_count) return -1;
        *sector = fs->root_start_lba + idx / per_sector;
        *off = (idx % per_sector) * 32;
        return 0;
    }
    uint32_t per_cluster = (fs->bytes_per_sector * fs->sectors_per_cluster) / 32;
    uint32_t cluster = dir->first_cluster;
    for (uint32_t k = 0; k < idx / per_cluster; k++) {
        uint32_t nxt = fat_next_cluster(fs, cluster);
        if (nxt == 0) {
            if (!allocate) return -1;
            uint32_t nc = fat_alloc_cluster(fs);
            if (!nc) return -1;
            fat_set_fat_entry(fs, cluster, nc);
            uint32_t cb = fs->bytes_per_sector * fs->sectors_per_cluster;
            uint8_t *zb = kmalloc(cb);
            if (zb) { memset(zb, 0, cb); fat_write_cluster(fs, nc, zb); kfree(zb); }
            nxt = nc;
        }
        cluster = nxt;
    }
    uint32_t within = idx % per_cluster;
    *sector = cluster_to_lba(fs, cluster) + within / per_sector;
    *off = (within % per_sector) * 32;
    return 0;
}

// Read the 32-byte entry at logical index. Returns its first name byte, or 0x00
// (virtual free / end) if past the end. Copies bytes to out32 if non-NULL.
static int dir_read_entry(fat_fs_t *fs, fat_file_t *dir, uint32_t idx, uint8_t *out32) {
    uint32_t sec, off;
    if (dir_locate(fs, dir, idx, &sec, &off, 0) != 0) return 0x00;
    if (fat_read_sector(fs, sec, sector_buf) <= 0) return -1;
    if (out32) memcpy(out32, sector_buf + off, 32);
    return sector_buf[off];
}

// Write a 32-byte entry at logical index (extends the directory if needed).
static int dir_write_entry(fat_fs_t *fs, fat_file_t *dir, uint32_t idx, const uint8_t *in32) {
    uint32_t sec, off;
    if (dir_locate(fs, dir, idx, &sec, &off, 1) != 0) return -1;
    if (fat_read_sector(fs, sec, sector_buf) <= 0) return -1;
    memcpy(sector_buf + off, in32, 32);
    if (fat_write_sector(fs, sec, sector_buf) <= 0) return -1;
    return 0;
}

// Pull the (up to) 13 chars of one LFN entry into out (ASCII subset).
static int lfn_extract(const uint8_t *e, char *out) {
    static const int slot[13] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
    int n = 0;
    for (int k = 0; k < 13; k++) {
        uint16_t ch = (uint16_t)(e[slot[k]] | (e[slot[k]+1] << 8));
        if (ch == 0x0000 || ch == 0xFFFF) break;
        out[n++] = (ch < 0x80) ? (char)ch : '_';
    }
    return n;
}

static int fat_ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        a++; b++;
    }
    return *a == *b;
}

// ===================== #404 Rust-port seam: pure per-entry dir/LFN decode ====
// The VFAT directory walk (fat_readdir_inner, below) is an untrusted-input
// parser: it decodes 32-byte on-disk directory entries and reassembles VFAT
// long-file-name (LFN) fragments straight from an attacker-supplyable FAT image.
// The golden is FAT-root, so this runs at every boot (e.g. ttf_init() scans
// /FONTS) and from userland via SYS_READDIR, on bytes a malicious USB stick /
// disk fully controls. The per-entry decode + LFN reassembly is lifted here as a
// PURE seam (no disk I/O, no FS mutation) so it can be ported to Rust behind
// -DRUST_FAT and differentially proven byte-identical to the C on real entries.
// dir_read_entry() (the cluster-chain walk + sector read) stays in C.
//
// fat_lfn_state_t carries the LFN accumulation across the entries of one
// directory walk. fat_parsed_entry_t is the decoded output. Layout is asserted
// so the #[repr(C)] Rust mirror in rustkern.rs can never silently drift.
typedef struct {
    char longname[260];   // reassembled UTF-16 -> ASCII long name (in progress)
    int  have_long;       // 1 while an LFN set is being accumulated
} fat_lfn_state_t;

typedef struct {
    uint8_t raw[32];      // the short directory entry (valid on FAT_STEP_SHORT)
    char    name[260];    // decoded name (long or 8.3), NUL-terminated
} fat_parsed_entry_t;

_Static_assert(sizeof(fat_lfn_state_t) == 264, "fat_lfn_state_t must be 264 bytes for the Rust FFI");
_Static_assert(sizeof(fat_parsed_entry_t) == 292, "fat_parsed_entry_t must be 292 bytes for the Rust FFI");

#define FAT_STEP_CONTINUE 0   // LFN fragment or 0xE5 deleted entry consumed
#define FAT_STEP_SHORT    1   // short entry decoded into *out
#define FAT_STEP_END      2   // 0x00 end-of-directory marker

// Rust mirror (rustkern.rs), live under -DRUST_FAT. It CONFINES the reachable
// overflow the C reference has downstream (see below).
extern int fat_dir_step_rs(fat_lfn_state_t *st, const uint8_t *e, fat_parsed_entry_t *out);

// fat_dir_step_c: VERBATIM reference. This is the decode logic lifted, byte for
// byte, out of the original fat_readdir_inner loop: the 0x00 end marker, the
// 0xE5 deleted-entry reset, the e[11]==FAT_ATTR_LFN branch with the EXACT
// (seq-1)*13<255 bound the C already had, lfn_extract, and the short-entry
// long-vs-8.3 name selection. It does NOT validate the LFN checksum (neither did
// the original readdir/lookup) - kept identical so the differential is honest.
//
// The internal longname[260] reassembly is self-contained safe (the
// (seq-1)*13<255 guard caps the last write index at 259). The REACHABLE overflow
// is DOWNSTREAM: this reference can emit a name of up to 259 chars, and
// fat_readdir_inner then strcpy()s it into a caller buffer that is only 256 (and
// as small as 64 / 16 at two call sites). See fat_rust_selftest's [RUST-SEC].
int fat_dir_step_c(fat_lfn_state_t *st, const uint8_t *e, fat_parsed_entry_t *out) {
    uint8_t b = e[0];
    if (b == 0x00) return FAT_STEP_END;
    if (b == 0xE5) { st->have_long = 0; return FAT_STEP_CONTINUE; }
    if (e[11] == FAT_ATTR_LFN) {
        int seq = e[0] & 0x3F;
        if (e[0] & 0x40) { st->have_long = 1; memset(st->longname, 0, sizeof(st->longname)); }
        if (st->have_long && seq >= 1 && (seq - 1) * 13 < 255) {
            char piece[16]; int n = lfn_extract(e, piece);
            for (int j = 0; j < n; j++) st->longname[(seq - 1) * 13 + j] = piece[j];
        }
        return FAT_STEP_CONTINUE;
    }
    // short entry
    memcpy(out->raw, e, 32);
    if (st->have_long) { st->longname[259] = 0; strcpy(out->name, st->longname); }
    else fat_name_to_str(e, out->name);
    return FAT_STEP_SHORT;
}

// LFN-aware directory lookup. Matches `name` case-insensitively against either
// the reconstructed long name or the 8.3 (alias) name. On success returns 0,
// fills *entry_out (short entry, 32 bytes), and the logical indices of the short
// entry (*short_idx) and the first LFN entry of its set (*first_idx).
static int fat_lookup(fat_fs_t *fs, uint32_t dir_cluster, const char *name,
                      fat_dir_entry_t *entry_out, uint32_t *short_idx, uint32_t *first_idx) {
    fat_file_t d; memset(&d, 0, sizeof(d)); d.fs = fs; d.first_cluster = dir_cluster; d.is_dir = 1;
    char longname[260]; int have_long = 0; uint32_t lfn_start = 0;
    for (uint32_t idx = 0; idx < 200000; idx++) {
        uint8_t e[32];
        int b = dir_read_entry(fs, &d, idx, e);
        if (b < 0) return -1;
        if (b == 0x00) return -1;                  // end of directory
        if (b == 0xE5) { have_long = 0; continue; }
        if (e[11] == FAT_ATTR_LFN) {
            int seq = e[0] & 0x3F;
            if (e[0] & 0x40) { have_long = 1; lfn_start = idx; memset(longname, 0, sizeof(longname)); }
            if (have_long && seq >= 1 && (seq - 1) * 13 < 255) {
                char piece[16]; int n = lfn_extract(e, piece);
                for (int j = 0; j < n; j++) longname[(seq - 1) * 13 + j] = piece[j];
            }
            continue;
        }
        // short entry
        char s83[13]; fat_name_to_str(e, s83);
        int match = (have_long && fat_ci_eq(longname, name)) || fat_ci_eq(s83, name);
        if (match) {
            memcpy(entry_out, e, 32);
            if (short_idx) *short_idx = idx;
            if (first_idx) *first_idx = have_long ? lfn_start : idx;
            return 0;
        }
        have_long = 0;
    }
    return -1;
}

// Compatibility wrapper (same signature/semantics as before; now LFN-aware).
// out_lba/out_off point at the SHORT directory entry.
static int fat_find_in_dir(fat_fs_t *fs, uint32_t dir_cluster, const char *name,
                           fat_dir_entry_t *entry_out,
                           uint32_t *out_lba, uint32_t *out_off) {
    fat_dir_entry_t e; uint32_t sidx, fidx;
    if (fat_lookup(fs, dir_cluster, name, &e, &sidx, &fidx) != 0) return -1;
    *entry_out = e;
    if (out_lba) {
        fat_file_t d; memset(&d, 0, sizeof(d)); d.fs = fs; d.first_cluster = dir_cluster; d.is_dir = 1;
        uint32_t sec, off;
        if (dir_locate(fs, &d, sidx, &sec, &off, 0) == 0) { *out_lba = sec; *out_off = off; }
    }
    return 0;
}

// Build an uppercase 8.3 base + ext from a long name (for alias generation).
static void fat_make_short_base(const char *name, char *base, char *ext) {
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    int bi = 0;
    for (const char *p = name; *p && p != dot && bi < 8; p++) {
        char c = *p;
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='~')) c = '_';
        base[bi++] = c;
    }
    base[bi] = 0;
    int ei = 0;
    if (dot) for (const char *p = dot + 1; *p && ei < 3; p++) {
        char c = *p;
        if (c == ' ' || c == '.') continue;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='~')) c = '_';
        ext[ei++] = c;
    }
    ext[ei] = 0;
    if (bi == 0) { base[0] = '_'; base[1] = 0; }
}

// Generate a unique "BASE~N.EXT" 8.3 alias for a long name. out83 = 11 bytes.
static int fat_gen_short_alias(fat_fs_t *fs, uint32_t dir_cluster,
                               const char *longname, uint8_t out83[11]) {
    char base[16], ext[8];
    fat_make_short_base(longname, base, ext);
    int blen = 0; while (base[blen]) blen++;
    for (int n = 1; n < 1000000; n++) {
        char num[8]; int nl = 0, t = n; char tmp[8]; int ti = 0;
        do { tmp[ti++] = (char)('0' + (t % 10)); t /= 10; } while (t);
        while (ti) num[nl++] = tmp[--ti];
        int keep = 8 - 1 - nl; if (keep < 1) keep = 1;
        int bl = (blen > keep) ? keep : blen;
        memset(out83, ' ', 11);
        for (int i = 0; i < bl; i++) out83[i] = (uint8_t)base[i];
        out83[bl] = '~';
        for (int i = 0; i < nl; i++) out83[bl + 1 + i] = (uint8_t)num[i];
        for (int i = 0; ext[i] && i < 3; i++) out83[8 + i] = (uint8_t)ext[i];
        char alias[13]; fat_name_to_str(out83, alias);
        fat_dir_entry_t tmpe;
        if (fat_find_in_dir(fs, dir_cluster, alias, &tmpe, 0, 0) != 0) return 0;  // free
    }
    return -1;
}

// Write a directory record set for `longname`: N LFN entries (if needed) plus the
// short entry (whose .name is the 8.3 alias). Places them in a contiguous free
// run, extending the directory as needed. Returns 0 on success.
static int fat_write_entry_set(fat_fs_t *fs, fat_file_t *dir, const char *longname,
                               const fat_dir_entry_t *short_entry) {
    int need = fat_name_needs_lfn(longname);
    int namelen = 0; while (longname[namelen]) namelen++;
    if (namelen > 255) namelen = 255;
    int nlfn = need ? ((namelen + 12) / 13) : 0;
    int total = nlfn + 1;

    // Find a contiguous run of `total` free entries (0x00 end region or 0xE5 holes).
    uint32_t run_start = 0, run_len = 0, start = 0; int have = 0, reached_end = 0;
    for (uint32_t idx = 0; idx < 200000; idx++) {
        int b = dir_read_entry(fs, dir, idx, 0);
        if (b < 0) return -1;
        if (b == 0x00) { if (run_len == 0) run_start = idx; reached_end = 1; start = run_start; have = 1; break; }
        if (b == 0xE5) {
            if (run_len == 0) run_start = idx;
            if (++run_len == (uint32_t)total) { start = run_start; have = 1; break; }
        } else {
            run_len = 0;
        }
    }
    if (!have) return -1;
    if (dir->first_cluster == 0 && fs->fat_type != FAT_TYPE_32 &&
        start + (uint32_t)total + (reached_end ? 1 : 0) > fs->root_entry_count) return -1;

    uint8_t checksum = lfn_checksum(short_entry->name);
    static const int slot[13] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
    for (int seq = nlfn; seq >= 1; seq--) {
        uint8_t e[32]; memset(e, 0, 32);
        e[0] = (uint8_t)(seq | ((seq == nlfn) ? 0x40 : 0x00));
        e[11] = FAT_ATTR_LFN; e[12] = 0; e[13] = checksum;
        int cbase = (seq - 1) * 13;
        for (int k = 0; k < 13; k++) {
            int ci = cbase + k;
            uint16_t ch = (ci < namelen) ? (uint16_t)(uint8_t)longname[ci]
                        : (ci == namelen) ? 0x0000 : 0xFFFF;
            e[slot[k]] = (uint8_t)(ch & 0xFF);
            e[slot[k] + 1] = (uint8_t)(ch >> 8);
        }
        if (dir_write_entry(fs, dir, start + (uint32_t)(nlfn - seq), e) != 0) return -1;
    }
    if (dir_write_entry(fs, dir, start + (uint32_t)nlfn, (const uint8_t *)short_entry) != 0) return -1;
    if (reached_end) {
        uint8_t z[32]; memset(z, 0, 32);
        dir_write_entry(fs, dir, start + (uint32_t)total, z);  // preserve end marker
    }
    return 0;
}

// Open file/directory
static int fat_open_inner(fat_fs_t *fs, const char *path, fat_file_t *file) {
    if (!fs->mounted) {
        return -1;
    }


    memset(file, 0, sizeof(fat_file_t));
    file->fs = fs;

    // Handle root directory
    if (path[0] == '/' && path[1] == '\0') {
        file->is_dir = 1;
        file->open = 1;
        file->attr = FAT_ATTR_DIRECTORY;
        if (fs->fat_type == FAT_TYPE_32) {
            file->first_cluster = fs->root_cluster;
            file->current_cluster = fs->root_cluster;
        } else {
            file->first_cluster = 0;
            file->current_cluster = 0;
        }
        strcpy(file->name, "/");
        return 0;
    }

    // Parse path
    const char *p = path;
    if (*p == '/') p++;

    uint32_t current_cluster = (fs->fat_type == FAT_TYPE_32) ? fs->root_cluster : 0;
    fat_dir_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    int found = 0;
    uint32_t found_lba = 0, found_off = 0;

    while (*p) {
        // Extract next path component
        char component[256];
        int i = 0;
        while (*p && *p != '/' && i < 255) {
            component[i++] = *p++;
        }
        component[i] = '\0';

        if (*p == '/') p++;

        // Find in current directory
        if (fat_find_in_dir(fs, current_cluster, component, &entry, &found_lba, &found_off) != 0) {
            return -1;  // Not found
        }
        found = 1;

        // Get cluster for this entry
        current_cluster = ((uint32_t)entry.cluster_hi << 16) | entry.cluster_lo;

        // If there's more path, this must be a directory
        if (*p && !(entry.attr & FAT_ATTR_DIRECTORY)) {
            return -1;  // Not a directory
        }
    }

    // Must have found something
    if (!found) {
        return -1;
    }

    // Fill file structure
    file->first_cluster = ((uint32_t)entry.cluster_hi << 16) | entry.cluster_lo;
    file->current_cluster = file->first_cluster;
    file->file_size = entry.file_size;
    file->attr = entry.attr;
    file->is_dir = (entry.attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
    file->position = 0;
    file->open = 1;
    file->dirent_lba = found_lba;
    file->dirent_off = found_off;
    fat_name_to_str(entry.name, file->name);

    return 0;
}

// Close file
void fat_close(fat_file_t *file) {
    file->open = 0;
}

// Read from file
static int fat_read_inner(fat_file_t *file, void *buffer, uint32_t size) {
    if (!file->open || file->is_dir) return -1;

    fat_fs_t *fs = file->fs;
    uint32_t bytes_per_cluster = fs->sectors_per_cluster * fs->bytes_per_sector;
    uint8_t *cluster_buf = kmalloc(bytes_per_cluster);
    if (!cluster_buf) return -1;

    uint32_t bytes_read = 0;
    uint8_t *out = (uint8_t *)buffer;
    // kprintf("[FAT] buffer=%p out=%p\n", buffer, out);
    uint32_t progress_interval = size / 10;  // Report every 10%
    uint32_t next_progress = progress_interval;

    // Limit read to file size
    if (file->position + size > file->file_size) {
        size = file->file_size - file->position;
    }

    while (bytes_read < size && file->current_cluster != 0) {
        // Read current cluster
        if (fat_read_cluster(fs, file->current_cluster, cluster_buf) <= 0) {
            kprintf("[FAT] Read failed at cluster %u\n", file->current_cluster);
            break;
        }

        // Calculate offset in cluster
        uint32_t cluster_offset = file->position % bytes_per_cluster;
        uint32_t bytes_in_cluster = bytes_per_cluster - cluster_offset;
        uint32_t bytes_to_copy = (size - bytes_read < bytes_in_cluster) ?
                                  (size - bytes_read) : bytes_in_cluster;

        // kprintf("[FAT] memcpy dest=%p src=%p len=%u\n", out + bytes_read, cluster_buf + cluster_offset, bytes_to_copy);
        memcpy(out + bytes_read, cluster_buf + cluster_offset, bytes_to_copy);
        bytes_read += bytes_to_copy;
        file->position += bytes_to_copy;

        // Progress indicator
        if (bytes_read >= next_progress) {
            // (Progress logging disabled)
            // (Progress logging disabled)
            next_progress += progress_interval;
        }

        // Move to next cluster if needed
        if (file->position % bytes_per_cluster == 0) {
            file->current_cluster = fat_next_cluster(fs, file->current_cluster);
        }
    }

    kfree(cluster_buf);
    return bytes_read;
}

// Get file size
uint32_t fat_size(fat_file_t *file) {
    return file->file_size;
}

// Seek in file
int fat_seek(fat_file_t *file, uint32_t position) {
    if (!file->open) return -1;
    if (position > file->file_size) position = file->file_size;

    fat_fs_t *fs = file->fs;
    uint32_t bytes_per_cluster = fs->sectors_per_cluster * fs->bytes_per_sector;

    // Reset to start
    file->current_cluster = file->first_cluster;
    file->position = 0;

    // Skip clusters
    while (file->position + bytes_per_cluster <= position && file->current_cluster != 0) {
        file->position += bytes_per_cluster;
        file->current_cluster = fat_next_cluster(fs, file->current_cluster);
    }

    file->position = position;
    return 0;
}

// Read directory entry
static int fat_readdir_inner(fat_file_t *dir, fat_dir_entry_t *entry, char *name_out) {
    if (!dir->open || !dir->is_dir) return -1;

    fat_fs_t *fs = dir->fs;
    uint32_t idx = dir->position / 32;
    // #404: the per-entry decode + LFN reassembly moved into the pure seam
    // fat_dir_step_c / fat_dir_step_rs (routed by -DRUST_FAT). The b<0 (disk read
    // error) and b==0x00 (end; e may be uninitialized on the dir_locate-fail
    // path) checks stay on the RETURN VALUE here, exactly as before, so the seam
    // is only ever handed a fully-read 32-byte entry.
    fat_lfn_state_t st; st.have_long = 0;

    for (;; idx++) {
        uint8_t e[32];
        int b = dir_read_entry(fs, dir, idx, e);
        if (b < 0) { dir->position = idx * 32; return -1; }
        if (b == 0x00) { dir->position = (idx + 1) * 32; return -1; }   // end

        fat_parsed_entry_t pe;
#ifdef RUST_FAT
        int r = fat_dir_step_rs(&st, e, &pe);
#else
        int r = fat_dir_step_c(&st, e, &pe);
#endif
        if (r == FAT_STEP_END) { dir->position = (idx + 1) * 32; return -1; }
        if (r == FAT_STEP_SHORT) {
            memcpy(entry, pe.raw, 32);
            // Under -DRUST_FAT pe.name is confined to <=255 chars (fits every
            // >=256-byte caller). The C reference can emit up to 259 chars here
            // (the reachable overflow this port removes on the live path).
            strcpy(name_out, pe.name);
            dir->position = (idx + 1) * 32;
            return 0;
        }
        // FAT_STEP_CONTINUE: LFN fragment or deleted entry consumed.
    }
}

// ============================================================================
// #404 Phase S boot-time self-test: prove fat_dir_step_rs (Rust, live under
// -DRUST_FAT) == fat_dir_step_c (verbatim reference) on the agreement domain
// (well-formed VFAT directories: LFN sets + short entries, deleted 0xE5,
// orphan LFN, bad seq numbers), characterize the SECURITY divergence HONESTLY,
// and micro-benchmark both. LIGHT (#426, bounded, runs once): ~1000 file
// decodes + a small security sweep + a ~5k-iter RDTSC walk. The heavy work
// (>=2M-vector differential + ASan/UBSan witness of the C overflow) is the
// OFFLINE pre-flight. One [RUST-DIFF] fat, one [RUST-SEC] fat, one [RUST-PERF].
//
// SAFETY at boot: every crafted directory lives in a static buffer, and the seam
// writes only into fat_parsed_entry_t.name[260], which holds even the maximal
// 259-char reference name without overflow. The REACHABLE overflow is the
// DOWNSTREAM strcpy into an undersized caller buffer; this self-test never does
// that copy (it reads pe.name directly), so the C reference stays in-bounds here.
// ============================================================================
static uint32_t fatdiff_rng(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x;
}
static inline uint64_t fat_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

// Build one 32-byte LFN entry for sequence `seq` (1-based) of `name`.
static void fat_build_lfn_entry(uint8_t *e, int seq, int is_last,
                                const char *name, int namelen) {
    static const int slot[13] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
    memset(e, 0, 32);
    e[0] = (uint8_t)(seq | (is_last ? 0x40 : 0x00));
    e[11] = FAT_ATTR_LFN;
    int start = (seq - 1) * 13;
    for (int k = 0; k < 13; k++) {
        int ci = start + k;
        uint16_t ch;
        if (ci < namelen)       ch = (uint16_t)(uint8_t)name[ci];
        else if (ci == namelen) ch = 0x0000;
        else                    ch = 0xFFFF;
        e[slot[k]]     = (uint8_t)(ch & 0xFF);
        e[slot[k] + 1] = (uint8_t)((ch >> 8) & 0xFF);
    }
}
static void fat_build_short(uint8_t *e, const char *n83, uint8_t attr) {
    memset(e, 0, 32);
    for (int i = 0; i < 11; i++) e[i] = n83[i] ? (uint8_t)n83[i] : ' ';
    e[11] = attr;
}
// Emit a full VFAT record set (N LFN entries top-down + 1 short) for `name`
// into buf at *pos. namelen chars, ASCII. Returns via *pos advance.
static void fat_emit_file(uint8_t *buf, int *pos, const char *name, int namelen,
                          const char *n83) {
    int nlfn = (namelen + 12) / 13;
    if (nlfn < 1) nlfn = 1;
    for (int seq = nlfn; seq >= 1; seq--) {
        fat_build_lfn_entry(buf + (*pos) * 32, seq, seq == nlfn, name, namelen);
        (*pos)++;
    }
    fat_build_short(buf + (*pos) * 32, n83, FAT_ATTR_ARCHIVE);
    (*pos)++;
}
// Walk a directory of `nent` 32-byte entries with the C or Rust step, copying
// each decoded short-entry name into out_names[]. Returns the count.
static int fat_walk(const uint8_t *dir, int nent, char out_names[][260], int use_rust) {
    fat_lfn_state_t st; st.have_long = 0;
    int cnt = 0;
    for (int i = 0; i < nent && cnt < 8; i++) {
        fat_parsed_entry_t pe;
        int r = use_rust ? fat_dir_step_rs(&st, dir + i * 32, &pe)
                         : fat_dir_step_c(&st, dir + i * 32, &pe);
        if (r == FAT_STEP_END) break;
        if (r == FAT_STEP_SHORT) { strcpy(out_names[cnt], pe.name); cnt++; }
    }
    return cnt;
}

void fat_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    static uint8_t buf[2048];
    static char names_c[8][260];
    static char names_r[8][260];
    static char nm[300];

    // Force-reference the Rust symbol so its archive member always links
    // (matches the icmp/arp/dns/dhcp/url/elf/pe pattern), regardless of RUST_FAT.
    { fat_lfn_state_t st; st.have_long = 0; fat_parsed_entry_t pe;
      uint8_t z[32]; memset(z, 0, 32); fat_dir_step_rs(&st, z, &pe); }

    // Part 1: agreement domain. Random well-formed directories (1..3 files, each
    // a random-length ASCII long name 1..250 chars + short alias), plus periodic
    // structural mutations (deleted 0xE5, orphan LFN with no short, bad seq).
    // Both walks must produce the identical sequence of decoded names.
    uint32_t seed = 0xfa7c0de1u;
    uint32_t files = 0, mismatches = 0;
    int first_bad = -1;
    for (uint32_t iter = 0; iter < 400; iter++) {
        int pos = 0;
        int nfiles = 1 + (int)(fatdiff_rng(&seed) % 3);
        for (int f = 0; f < nfiles && pos < 56; f++) {
            int len = 1 + (int)(fatdiff_rng(&seed) % 250);
            for (int c = 0; c < len; c++) {
                // printable ASCII 0x21..0x7E (avoid space / control)
                nm[c] = (char)(0x21 + (fatdiff_rng(&seed) % (0x7E - 0x21)));
            }
            nm[len] = 0;
            char n83[12]; for (int i = 0; i < 11; i++) n83[i] = (char)('A' + (i + f) % 26); n83[11] = 0;
            fat_emit_file(buf, &pos, nm, len, n83);
        }
        // structural mutations on some iterations (still must AGREE)
        uint32_t mut = fatdiff_rng(&seed) % 6;
        if (mut == 1 && pos > 0) buf[0] = 0xE5;                 // first entry deleted
        else if (mut == 2 && pos > 1) buf[(pos - 1) * 32] = 0xE5; // final short deleted (orphan LFN)
        else if (mut == 3 && pos > 0) buf[0] = (buf[0] & 0xC0) | 0x3F; // seq -> 63 (guard rejects)
        else if (mut == 4 && pos > 0) buf[0] = (buf[0] & 0x40);        // seq -> 0 (guard rejects)
        memset(buf + pos * 32, 0, 32);                          // 0x00 terminator
        int nent = pos + 1;

        int cc = fat_walk(buf, nent, names_c, 0);
        int rc = fat_walk(buf, nent, names_r, 1);
        int bad = (cc != rc);
        for (int i = 0; i < cc && i < rc && !bad; i++)
            if (strcmp(names_c[i], names_r[i]) != 0) bad = 1;
        files += (cc > rc ? rc : cc);
        if (bad) { mismatches++; if (first_bad < 0) first_bad = (int)iter; }
    }
    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] fat: %u files, %u mismatches -> %s\n", files, mismatches, verdict);
    bootlog_write("[RUST-DIFF] fat: %u files, %u mismatches -> %s", files, mismatches, verdict);
    if (mismatches != 0) {
        kprintf("[RUST-DIFF] fat FIRST MISMATCH iter=%d\n", first_bad);
        bootlog_write("[RUST-DIFF] fat FIRST MISMATCH iter=%d", first_bad);
    }

    // Part 2: SECURITY posture (HONEST). The reachable class: an LFN name of
    // 256..259 chars. The verbatim C emits the full name (up to 259 chars); the
    // downstream strcpy in fat_readdir_inner then overflows a 256-byte name_buf
    // (SYS_READDIR), a 64-byte buffer (fat_delete's dir-empty scan) or a 16-byte
    // one (dosexec find), by up to 4 / 195 / 243 bytes. The Rust confines the
    // decoded name to <=255 chars so that downstream copy can never exceed 256.
    // (Reading pe.name here is in-bounds for both; the ASan witness of the heap
    // overflow is the OFFLINE pre-flight.)
    {
        uint32_t n = 0, c_overlong = 0, r_overlong = 0, divergences = 0;
        uint32_t s2 = 0x5ec0fa7u;
        for (uint32_t r = 0; r < 200; r++) {
            int len = 256 + (int)(fatdiff_rng(&s2) % 4);   // 256..259
            for (int c = 0; c < len; c++) nm[c] = (char)(0x41 + (fatdiff_rng(&s2) % 26));
            nm[len] = 0;
            int pos = 0;
            fat_emit_file(buf, &pos, nm, len, "OVERLONGTXT");
            memset(buf + pos * 32, 0, 32);
            fat_lfn_state_t st; fat_parsed_entry_t pe;
            // C reference
            st.have_long = 0; int lc = 0;
            for (int i = 0; i <= pos; i++) { int rr = fat_dir_step_c(&st, buf + i * 32, &pe);
                if (rr == FAT_STEP_SHORT) { lc = (int)strlen(pe.name); break; }
                if (rr == FAT_STEP_END) break; }
            // Rust
            st.have_long = 0; int lr = 0;
            for (int i = 0; i <= pos; i++) { int rr = fat_dir_step_rs(&st, buf + i * 32, &pe);
                if (rr == FAT_STEP_SHORT) { lr = (int)strlen(pe.name); break; }
                if (rr == FAT_STEP_END) break; }
            n++;
            if (lc > 255) c_overlong++;
            if (lr > 255) r_overlong++;
            if (lc != lr) divergences++;
        }
        kprintf("[RUST-SEC] fat: REACHABLE - C emits >255-char LFN name %u/%u (overflows 256-byte name_buf downstream); Rust confined %u/%u; divergences=%u\n",
                c_overlong, n, r_overlong, n, divergences);
        bootlog_write("[RUST-SEC] fat: C overlong %u/%u (reachable name_buf overflow) Rust confined %u/%u div=%u",
                      c_overlong, n, r_overlong, n, divergences);
    }

    // Part 3: RDTSC micro-benchmark over a fixed well-formed directory. LIGHT: 5k.
    {
        const int iters = 5000;
        int pos = 0;
        fat_emit_file(buf, &pos, "ExampleLongFileName.txt", 23, "EXAMPLETXT");
        fat_emit_file(buf, &pos, "readme.md", 9, "READMEMD ");
        memset(buf + pos * 32, 0, 32);
        int nent = pos + 1;

        for (int i = 0; i < 300; i++) { fat_walk(buf, nent, names_c, 0); fat_walk(buf, nent, names_r, 1); }
        uint64_t t0 = fat_tsc_serialized();
        for (int i = 0; i < iters; i++) fat_walk(buf, nent, names_c, 0);
        uint64_t t1 = fat_tsc_serialized();
        for (int i = 0; i < iters; i++) fat_walk(buf, nent, names_r, 1);
        uint64_t t2 = fat_tsc_serialized();
        uint64_t c_cyc = (t1 - t0) / iters;
        uint64_t r_cyc = (t2 - t1) / iters;
        uint64_t ratio100 = (c_cyc != 0) ? (r_cyc * 100ULL / c_cyc) : 0;
        kprintf("[RUST-PERF] fat: C=%llu cyc/walk RS=%llu cyc/walk ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
        bootlog_write("[RUST-PERF] fat: C=%llu RS=%llu ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}

// Check if directory
int fat_is_dir(fat_file_t *file) {
    return file->is_dir;
}

// Print filesystem info
void fat_print_info(fat_fs_t *fs) {
    if (!fs->mounted) {
        kprintf("[FAT] Not mounted\n");
        return;
    }

    kprintf("\n[FAT] Filesystem Information:\n");
    kprintf("  Type:           FAT%d\n", fs->fat_type);
    kprintf("  Volume Label:   %s\n", fs->volume_label);
    kprintf("  Bytes/Sector:   %u\n", fs->bytes_per_sector);
    kprintf("  Sectors/Cluster: %u\n", fs->sectors_per_cluster);
    kprintf("  Total Clusters: %u\n", fs->cluster_count);
    kprintf("  Total Size:     %u MB\n",
            (fs->cluster_count * fs->sectors_per_cluster * fs->bytes_per_sector) / (1024*1024));
}

// List directory
static void fat_list_dir_inner(fat_fs_t *fs, const char *path) {
    fat_file_t dir;
    if (fat_open(fs, path, &dir) != 0) {
        kprintf("[FAT] Cannot open: %s\n", path);
        return;
    }

    if (!dir.is_dir) {
        kprintf("[FAT] Not a directory: %s\n", path);
        fat_close(&dir);
        return;
    }

    kprintf("\nDirectory of %s\n", path);
    kprintf("%-12s %10s  %s\n", "Name", "Size", "Attr");
    kprintf("%-12s %10s  %s\n", "----", "----", "----");

    fat_dir_entry_t entry;
    char name[256];
    int count = 0;

    while (fat_readdir(&dir, &entry, name) == 0) {
        char attr_str[8] = "------";
        if (entry.attr & FAT_ATTR_DIRECTORY) attr_str[0] = 'D';
        if (entry.attr & FAT_ATTR_READ_ONLY) attr_str[1] = 'R';
        if (entry.attr & FAT_ATTR_HIDDEN) attr_str[2] = 'H';
        if (entry.attr & FAT_ATTR_SYSTEM) attr_str[3] = 'S';
        if (entry.attr & FAT_ATTR_ARCHIVE) attr_str[4] = 'A';

        if (entry.attr & FAT_ATTR_DIRECTORY) {
            kprintf("%-12s %10s  %s\n", name, "<DIR>", attr_str);
        } else {
            kprintf("%-12s %10u  %s\n", name, entry.file_size, attr_str);
        }
        count++;
    }

    kprintf("\n%d entries\n", count);
    fat_close(&dir);
}

// Read entire file
void *fat_read_file(fat_fs_t *fs, const char *path, uint32_t *size_out) {
    extern fat_fs_t g_fat_fs;
    // task #317: network-filesystem routing. Paths under "/SMB/<server>/<share>"
    // are served by the SMB2 client (net/smb.c), mirroring the ext2 "/ext2"
    // redirect below. The share is mounted on demand on first access.
    if (path && (path[0]=='/') &&
        (path[1]=='S'||path[1]=='s') && (path[2]=='M'||path[2]=='m') &&
        (path[3]=='B'||path[3]=='b') && (path[4]=='/' || path[4]==0)) {
        extern void *smb_vfs_read_whole(const char *p, uint32_t *sz);
        return smb_vfs_read_whole(path, size_out);
    }
    // task #317 pass 3: NFSv3 routing. Paths under "/NFS/<server>/<label>" are
    // served by the NFS client (net/nfs.c). The mount must already exist (no
    // lazy auto-mount, since the export path is not encoded in the /NFS path).
    if (path && (path[0]=='/') &&
        (path[1]=='N'||path[1]=='n') && (path[2]=='F'||path[2]=='f') &&
        (path[3]=='S'||path[3]=='s') && (path[4]=='/' || path[4]==0)) {
        extern void *nfs_vfs_read_whole(const char *p, uint32_t *sz);
        return nfs_vfs_read_whole(path, size_out);
    }
    // #196: a removable drive (A:/E:) with a disk image mounted serves its files
    // from the image, not the /WINDIR folder. diskimg_try_read returns non-NULL
    // ONLY for "/WINDIR/DRIVE_E|A/.." paths when an image is mounted there; it is
    // inert (returns NULL) for every other path, so the no-mount case is unchanged.
    if (fs == &g_fat_fs && path) {
        extern void *diskimg_try_read(const char *p, unsigned int *sz);
        unsigned int isz = 0;
        void *ib = diskimg_try_read(path, &isz);
        if (ib) { if (size_out) *size_out = isz; return ib; }
    }
    // #99 Phase C: with ext2 as the root fs, serve "/" reads from the ext2
    // volume first; fall back to FAT when a file is not on ext2. The FAT ESP /
    // boot paths (/boot, /EFI) are never redirected (UEFI loads from them).
    if (g_root_ext2 && fs == &g_fat_fs && path && path[0] == '/' &&
        !(path[1]=='b' && path[2]=='o' && path[3]=='o' && path[4]=='t') &&
        !(path[1]=='E' && path[2]=='F' && path[3]=='I')) {
        uint32_t esz = 0;
        void *eb = ext2_read_whole(ext2_vol_path(path), &esz);
        if (eb) { if (size_out) *size_out = esz; return eb; }
        // not present on ext2: fall through to the FAT read below
    }
    fat_file_t file;
    kprintf("[FAT] fat_read_file: opening %s\n", path);
    if (fat_open(fs, path, &file) != 0) {
        kprintf("[FAT] fat_read_file: open failed\n");
        return NULL;
    }
    kprintf("[FAT] fat_read_file: opened, is_dir=%d\n", file.is_dir);

    if (file.is_dir) {
        fat_close(&file);
        return NULL;
    }

    uint32_t size = fat_size(&file);
    kprintf("[FAT] fat_read_file: size=%u bytes, allocating\n", size);
    void *buffer = kmalloc(size + 1);  // +1 for null terminator
    if (!buffer) {
        kprintf("[FAT] fat_read_file: kmalloc failed for %u bytes\n", size);
        fat_close(&file);
        return NULL;
    }
    kprintf("[FAT] fat_read_file: allocated at %p, reading...\n", buffer);

    int bytes_read = fat_read(&file, buffer, size);
    kprintf("[FAT] fat_read_file: read %d bytes\n", bytes_read);
    fat_close(&file);

    if (bytes_read != (int)size) {
        kprintf("[FAT] fat_read_file: read mismatch (%d != %u)\n", bytes_read, size);
        kfree(buffer);
        return NULL;
    }

    ((uint8_t *)buffer)[size] = 0;  // Null terminate

    if (size_out) *size_out = size;
    return buffer;
}

// ============================================
// Write Operations
// ============================================

// Helper: Find parent directory and get filename
static int fat_split_path(const char *path, char *parent_out, char *name_out) {
    int len = strlen(path);
    if (len == 0) return -1;

    // Find last slash
    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') {
            last_slash = i;
            break;
        }
    }

    if (last_slash < 0) {
        // No slash - file in root
        strcpy(parent_out, "/");
        strcpy(name_out, path);
    } else if (last_slash == 0) {
        // File in root (e.g., "/file.txt")
        strcpy(parent_out, "/");
        strcpy(name_out, path + 1);
    } else {
        // File in subdirectory
        strncpy(parent_out, path, last_slash);
        parent_out[last_slash] = '\0';
        strcpy(name_out, path + last_slash + 1);
    }

    return 0;
}

// Helper: Find empty directory entry in a directory
static int fat_find_free_dir_entry(fat_fs_t *fs, fat_file_t *dir, uint32_t *sector_out, uint32_t *offset_out) {
    uint8_t *cluster_buf = kmalloc(fs->bytes_per_sector * fs->sectors_per_cluster);
    if (!cluster_buf) return -1;

    uint32_t cluster = dir->first_cluster;
    uint32_t prev_cluster = 0;

    // For root directory in FAT16
    if (cluster == 0 && fs->fat_type != FAT_TYPE_32) {
        for (uint32_t s = 0; s < fs->root_dir_sectors; s++) {
            if (fat_read_sector(fs, fs->root_start_lba + s, sector_buf) <= 0) {
                kfree(cluster_buf);
                return -1;
            }

            for (uint32_t i = 0; i < fs->bytes_per_sector; i += 32) {
                fat_dir_entry_t *entry = (fat_dir_entry_t *)(sector_buf + i);
                if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
                    *sector_out = fs->root_start_lba + s;
                    *offset_out = i;
                    kfree(cluster_buf);
                    return 0;
                }
            }
        }
        kfree(cluster_buf);
        return -1;  // Root directory full
    }

    // Traverse cluster chain for FAT32 or subdirectories
    while (cluster != 0) {
        if (fat_read_cluster(fs, cluster, cluster_buf) <= 0) {
            kfree(cluster_buf);
            return -1;
        }

        uint32_t cluster_bytes = fs->bytes_per_sector * fs->sectors_per_cluster;
        for (uint32_t i = 0; i < cluster_bytes; i += 32) {
            fat_dir_entry_t *entry = (fat_dir_entry_t *)(cluster_buf + i);
            if (entry->name[0] == 0x00 || entry->name[0] == 0xE5) {
                *sector_out = cluster_to_lba(fs, cluster) + (i / fs->bytes_per_sector);
                *offset_out = i % fs->bytes_per_sector;
                kfree(cluster_buf);
                return 0;
            }
        }

        prev_cluster = cluster;
        cluster = fat_next_cluster(fs, cluster);
    }

    // No free entry - need to allocate new cluster for directory
    uint32_t new_cluster = fat_alloc_cluster(fs);
    if (new_cluster == 0) {
        kfree(cluster_buf);
        return -1;
    }

    // Link new cluster to chain
    if (prev_cluster != 0) {
        fat_set_fat_entry(fs, prev_cluster, new_cluster);
    }

    // Clear new cluster
    memset(cluster_buf, 0, fs->bytes_per_sector * fs->sectors_per_cluster);
    fat_write_cluster(fs, new_cluster, cluster_buf);

    *sector_out = cluster_to_lba(fs, new_cluster);
    *offset_out = 0;

    kfree(cluster_buf);
    return 0;
}

// Create a new file
static int fat_create_inner(fat_fs_t *fs, const char *path) {
    if (!fs || !fs->mounted || !path) return -1;

    char parent_path[256];
    char filename[256];

    if (fat_split_path(path, parent_path, filename) != 0) {
        return -1;
    }

    // Check if file already exists
    fat_file_t test_file;
    if (fat_open(fs, path, &test_file) == 0) {
        fat_close(&test_file);
        return -1;  // Already exists
    }

    // Open parent directory
    fat_file_t parent_dir;
    if (fat_open(fs, parent_path, &parent_dir) != 0) {
        return -1;  // Parent doesn't exist
    }

    if (!fat_is_dir(&parent_dir)) {
        fat_close(&parent_dir);
        return -1;  // Parent is not a directory
    }

    // Build the short directory entry (8.3 alias if the name needs an LFN), then
    // write it together with any LFN entries as a contiguous set.
    fat_dir_entry_t se; memset(&se, 0, sizeof(se));
    if (fat_name_needs_lfn(filename)) {
        if (fat_gen_short_alias(fs, parent_dir.first_cluster, filename, se.name) != 0) {
            fat_close(&parent_dir);
            return -1;
        }
    } else {
        str_to_fat_name(filename, se.name);
    }
    se.attr = FAT_ATTR_ARCHIVE;

    if (fat_write_entry_set(fs, &parent_dir, filename, &se) != 0) {
        fat_close(&parent_dir);
        return -1;
    }

    fat_close(&parent_dir);
    kprintf("[FS] Created file: %s\n", path);
    return 0;
}

// Create a new directory
static int fat_mkdir_inner(fat_fs_t *fs, const char *path) {
    if (!fs || !fs->mounted || !path) return -1;

    char parent_path[256];
    char dirname[256];

    if (fat_split_path(path, parent_path, dirname) != 0) {
        return -1;
    }

    // Check if directory already exists
    fat_file_t test_dir;
    if (fat_open(fs, path, &test_dir) == 0) {
        fat_close(&test_dir);
        return -1;  // Already exists
    }

    // Open parent directory
    fat_file_t parent_dir;
    if (fat_open(fs, parent_path, &parent_dir) != 0) {
        return -1;
    }

    if (!fat_is_dir(&parent_dir)) {
        fat_close(&parent_dir);
        return -1;
    }

    // Allocate a cluster for the new directory
    uint32_t new_cluster = fat_alloc_cluster(fs);
    if (new_cluster == 0) {
        fat_close(&parent_dir);
        return -1;
    }

    // Build the directory's short entry (8.3 alias if the name needs an LFN) and
    // write it together with any LFN entries.
    fat_dir_entry_t se; memset(&se, 0, sizeof(se));
    if (fat_name_needs_lfn(dirname)) {
        if (fat_gen_short_alias(fs, parent_dir.first_cluster, dirname, se.name) != 0) {
            fat_set_fat_entry(fs, new_cluster, 0);
            fat_close(&parent_dir);
            return -1;
        }
    } else {
        str_to_fat_name(dirname, se.name);
    }
    se.attr = FAT_ATTR_DIRECTORY;
    se.cluster_hi = (new_cluster >> 16) & 0xFFFF;
    se.cluster_lo = new_cluster & 0xFFFF;
    if (fat_write_entry_set(fs, &parent_dir, dirname, &se) != 0) {
        fat_set_fat_entry(fs, new_cluster, 0);
        fat_close(&parent_dir);
        return -1;
    }

    // Initialize new directory with . and .. entries
    uint8_t *dir_buf = kmalloc(fs->bytes_per_sector * fs->sectors_per_cluster);
    if (!dir_buf) {
        fat_close(&parent_dir);
        return -1;
    }

    memset(dir_buf, 0, fs->bytes_per_sector * fs->sectors_per_cluster);

    // . entry
    fat_dir_entry_t *dot = (fat_dir_entry_t *)dir_buf;
    memset(dot->name, ' ', 11);
    dot->name[0] = '.';
    dot->attr = FAT_ATTR_DIRECTORY;
    dot->cluster_hi = (new_cluster >> 16) & 0xFFFF;
    dot->cluster_lo = new_cluster & 0xFFFF;

    // .. entry
    fat_dir_entry_t *dotdot = (fat_dir_entry_t *)(dir_buf + 32);
    memset(dotdot->name, ' ', 11);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = FAT_ATTR_DIRECTORY;
    dotdot->cluster_hi = (parent_dir.first_cluster >> 16) & 0xFFFF;
    dotdot->cluster_lo = parent_dir.first_cluster & 0xFFFF;

    // Write new directory cluster
    fat_write_cluster(fs, new_cluster, dir_buf);

    kfree(dir_buf);
    fat_close(&parent_dir);
    kprintf("[FS] Created directory: %s\n", path);
    return 0;
}

// Delete a file or empty directory
static int fat_delete_inner(fat_fs_t *fs, const char *path) {
    if (!fs || !fs->mounted || !path) return -1;

    char parent_path[256];
    char filename[256];

    if (fat_split_path(path, parent_path, filename) != 0) {
        return -1;
    }

    // Open the file to get its cluster chain
    fat_file_t file;
    if (fat_open(fs, path, &file) != 0) {
        return -1;  // File doesn't exist
    }

    // If it's a directory, check if it's empty
    if (fat_is_dir(&file)) {
        fat_dir_entry_t entry;
        // #404 plain-C fix: fat_readdir reconstructs VFAT long names of up to 255
        // chars, so this MUST be a full 256-byte buffer. The old char name[64]
        // let a directory containing a long-named entry overflow this stack
        // buffer via the strcpy in fat_readdir_inner (reachable on rmdir of a
        // crafted FAT dir). See blame.md / [RUST-SEC] fat.
        char name[256];
        int count = 0;

        while (fat_readdir(&file, &entry, name) == 0) {
            if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
                count++;
                break;
            }
        }

        if (count > 0) {
            fat_close(&file);
            return -1;  // Directory not empty
        }

        // Reset position for finding the entry
        file.position = 0;
    }

    // Free the cluster chain
    if (file.first_cluster >= 2) {
        fat_free_cluster_chain(fs, file.first_cluster);
    }

    fat_close(&file);

    // Find the entry (LFN-aware) and erase its whole set: the LFN entries plus
    // the short entry (logical indices first_idx..short_idx).
    fat_file_t parent_dir;
    if (fat_open(fs, parent_path, &parent_dir) != 0) {
        return -1;
    }

    fat_dir_entry_t e; uint32_t sidx = 0, fidx = 0;
    if (fat_lookup(fs, parent_dir.first_cluster, filename, &e, &sidx, &fidx) != 0) {
        fat_close(&parent_dir);
        return -1;  // Entry not found
    }
    for (uint32_t i = fidx; i <= sidx; i++) {
        uint8_t buf[32];
        if (dir_read_entry(fs, &parent_dir, i, buf) < 0) break;
        buf[0] = 0xE5;
        dir_write_entry(fs, &parent_dir, i, buf);
    }
    fat_close(&parent_dir);
    kprintf("[FS] Deleted: %s\n", path);
    return 0;
}

// Rename a file or directory
static int fat_rename_inner(fat_fs_t *fs, const char *old_path, const char *new_path) {
    if (!fs || !fs->mounted || !old_path || !new_path) return -1;

    // For now, only support rename within same directory
    char old_parent[256], old_name[64];
    char new_parent[256], new_name[64];

    fat_split_path(old_path, old_parent, old_name);
    fat_split_path(new_path, new_parent, new_name);

    if (strcmp(old_parent, new_parent) != 0) {
        // Cross-directory move by relocating the directory entry (no data copy):
        // the file keeps its cluster chain; we add an entry in the new parent
        // pointing at the same clusters, then mark the old entry deleted.
        // Files only (directories would also need their ".." updated).
        fat_file_t src;
        if (fat_open(fs, old_path, &src) != 0) { kprintf("[FS] xdir: open old fail %s\n", old_path); return -1; }
        if (src.is_dir) { fat_close(&src); kprintf("[FS] xdir: src is dir\n"); return -1; }
        uint32_t first_cluster = src.first_cluster;
        uint32_t fsize = src.file_size;
        uint32_t old_lba = src.dirent_lba;
        uint32_t old_off = src.dirent_off;
        fat_close(&src);
        if (old_lba == 0) return -1;   // unknown source dir-entry location

        fat_file_t ndir;
        if (fat_open(fs, new_parent, &ndir) != 0) { kprintf("[FS] xdir: open newparent fail %s\n", new_parent); return -1; }
        uint32_t esec = 0, eoff = 0;
        if (fat_find_free_dir_entry(fs, &ndir, &esec, &eoff) != 0) { fat_close(&ndir); kprintf("[FS] xdir: no free slot in %s\n", new_parent); return -1; }
        fat_close(&ndir);

        // Write the relocated entry into the destination directory.
        if (fat_read_sector(fs, esec, sector_buf) <= 0) return -1;
        fat_dir_entry_t *ne = (fat_dir_entry_t *)(sector_buf + eoff);
        memset(ne, 0, 32);
        str_to_fat_name(new_name, ne->name);
        ne->attr = FAT_ATTR_ARCHIVE;
        ne->cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
        ne->cluster_hi = (uint16_t)((first_cluster >> 16) & 0xFFFF);
        ne->file_size = fsize;
        if (fat_write_sector(fs, esec, sector_buf) <= 0) return -1;

        // Mark the old entry free (0xE5). Clusters are now owned by the new
        // entry, so we deliberately do NOT free them.
        if (fat_read_sector(fs, old_lba, sector_buf) > 0) {
            sector_buf[old_off] = 0xE5;
            fat_write_sector(fs, old_lba, sector_buf);
        }
        kprintf("[FS] Moved (dirent): %s -> %s (cl=%u sz=%u)\n",
                old_path, new_path, first_cluster, fsize);
        return 0;
    }

    // Find the old entry and update its name
    fat_file_t parent_dir;
    if (fat_open(fs, old_parent, &parent_dir) != 0) {
        return -1;
    }

    uint8_t old_fat_name[11], new_fat_name[11];
    str_to_fat_name(old_name, old_fat_name);
    str_to_fat_name(new_name, new_fat_name);

    uint8_t *cluster_buf = kmalloc(fs->bytes_per_sector * fs->sectors_per_cluster);
    if (!cluster_buf) {
        fat_close(&parent_dir);
        return -1;
    }

    uint32_t cluster = parent_dir.first_cluster;

    // Handle FAT16 root directory
    if (cluster == 0 && fs->fat_type != FAT_TYPE_32) {
        for (uint32_t s = 0; s < fs->root_dir_sectors; s++) {
            if (fat_read_sector(fs, fs->root_start_lba + s, sector_buf) <= 0) {
                continue;
            }

            for (uint32_t i = 0; i < fs->bytes_per_sector; i += 32) {
                fat_dir_entry_t *entry = (fat_dir_entry_t *)(sector_buf + i);
                if (memcmp(entry->name, old_fat_name, 11) == 0) {
                    memcpy(entry->name, new_fat_name, 11);
                    fat_write_sector(fs, fs->root_start_lba + s, sector_buf);
                    kfree(cluster_buf);
                    fat_close(&parent_dir);
                    kprintf("[FS] Renamed: %s -> %s\n", old_path, new_path);
                    return 0;
                }
            }
        }
    } else {
        while (cluster != 0) {
            if (fat_read_cluster(fs, cluster, cluster_buf) <= 0) {
                break;
            }

            uint32_t cluster_bytes = fs->bytes_per_sector * fs->sectors_per_cluster;
            for (uint32_t i = 0; i < cluster_bytes; i += 32) {
                fat_dir_entry_t *entry = (fat_dir_entry_t *)(cluster_buf + i);
                if (memcmp(entry->name, old_fat_name, 11) == 0) {
                    memcpy(entry->name, new_fat_name, 11);
                    fat_write_cluster(fs, cluster, cluster_buf);
                    kfree(cluster_buf);
                    fat_close(&parent_dir);
                    kprintf("[FS] Renamed: %s -> %s\n", old_path, new_path);
                    return 0;
                }
            }

            cluster = fat_next_cluster(fs, cluster);
        }
    }

    kfree(cluster_buf);
    fat_close(&parent_dir);
    return -1;
}

// Write data to a file at current position
static int fat_write_inner(fat_file_t *file, const void *buffer, uint32_t size) {
    if (!file || !file->fs || !buffer || size == 0) return -1;

    fat_fs_t *fs = file->fs;
    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t bytes_written = 0;
    uint32_t cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;

    uint8_t *cluster_buf = kmalloc(cluster_size);
    if (!cluster_buf) return -1;

    // If file has no clusters yet, allocate the first one
    if (file->first_cluster < 2) {
        uint32_t new_cluster = fat_alloc_cluster(fs);
        if (new_cluster == 0) {
            kfree(cluster_buf);
            return -1;
        }
        file->first_cluster = new_cluster;
        file->current_cluster = new_cluster;

        // Update directory entry with new cluster
        // TODO: We need to store the directory entry location in fat_file_t
    }

    // Navigate to the cluster holding `position`, EXTENDING the chain (allocate
    // + link) whenever navigation runs past the current end. Previously, when
    // the chain was shorter than cluster_index, navigation left `cluster` at the
    // last existing cluster (or 0), so each append-call rewrote that one cluster
    // instead of growing the file -- multi-cluster writes (e.g. cp of a 3MB BMP,
    // which the cp app issues as many 4KB append calls) collapsed to a single
    // cluster and the rest were leaked. Now the chain is grown to reach the
    // target cluster.
    uint32_t cluster_index = file->position / cluster_size;
    uint32_t offset_in_cluster = file->position % cluster_size;

    uint32_t cluster = file->first_cluster;
    for (uint32_t i = 0; i < cluster_index; i++) {
        uint32_t next = fat_next_cluster(fs, cluster);
        if (next == 0) {
            uint32_t nc = fat_alloc_cluster(fs);
            if (nc == 0) { kfree(cluster_buf); return bytes_written; }
            fat_set_fat_entry(fs, cluster, nc);
            next = nc;
        }
        cluster = next;
    }

    // Write data
    while (bytes_written < size) {
        if (cluster == 0) {
            // Need new cluster
            uint32_t new_cluster = fat_alloc_cluster(fs);
            if (new_cluster == 0) break;

            if (file->first_cluster < 2) {
                file->first_cluster = new_cluster;
            } else {
                // Find last cluster and link
                uint32_t last = file->first_cluster;
                while (fat_next_cluster(fs, last) != 0) {
                    last = fat_next_cluster(fs, last);
                }
                fat_set_fat_entry(fs, last, new_cluster);
            }
            cluster = new_cluster;
            offset_in_cluster = 0;

            // Clear new cluster
            memset(cluster_buf, 0, cluster_size);
        } else {
            // Read existing cluster
            if (fat_read_cluster(fs, cluster, cluster_buf) <= 0) {
                break;
            }
        }

        // Calculate how much to write to this cluster
        uint32_t space_in_cluster = cluster_size - offset_in_cluster;
        uint32_t to_write = size - bytes_written;
        if (to_write > space_in_cluster) to_write = space_in_cluster;

        // Copy data
        memcpy(cluster_buf + offset_in_cluster, src + bytes_written, to_write);

        // Write cluster back
        if (fat_write_cluster(fs, cluster, cluster_buf) <= 0) {
            break;
        }

        bytes_written += to_write;
        file->position += to_write;
        offset_in_cluster = 0;

        // Move to next cluster if needed
        if (bytes_written < size) {
            uint32_t next = fat_next_cluster(fs, cluster);
            cluster = next;
        }
    }

    // Update file size if we extended it
    if (file->position > file->file_size) {
        file->file_size = file->position;
    }

    // Persist first_cluster + file_size back to the directory entry. Without
    // this, written data is invisible (the entry still reports size 0 / cluster 0)
    // and the allocated cluster is leaked. (Was an unfinished TODO.)
    if (file->dirent_lba && fs->bytes_per_sector <= 512) {
        uint8_t dbuf[512];
        if (fat_read_sector(fs, file->dirent_lba, dbuf) > 0 &&
            file->dirent_off + 32 <= fs->bytes_per_sector) {
            fat_dir_entry_t *de = (fat_dir_entry_t *)(dbuf + file->dirent_off);
            de->cluster_lo = (uint16_t)(file->first_cluster & 0xFFFF);
            de->cluster_hi = (uint16_t)(file->first_cluster >> 16);
            de->file_size  = file->file_size;
            fat_write_sector(fs, file->dirent_lba, dbuf);
        }
    }

    kfree(cluster_buf);
    return bytes_written;
}

// Copy a file from src_path to dst_path
static int fat_copy_inner(fat_fs_t *fs, const char *src_path, const char *dst_path) {
    if (!fs || !src_path || !dst_path) return -1;

    // Open source file
    fat_file_t src_file;
    if (fat_open(fs, src_path, &src_file) != 0) {
        return -1;
    }

    if (fat_is_dir(&src_file)) {
        fat_close(&src_file);
        return -1;  // Can't copy directories this way
    }

    // Create destination file
    if (fat_create(fs, dst_path) != 0) {
        fat_close(&src_file);
        return -1;
    }

    // Open destination file
    fat_file_t dst_file;
    if (fat_open(fs, dst_path, &dst_file) != 0) {
        fat_close(&src_file);
        return -1;
    }

    // Copy data in chunks
    uint8_t *buf = kmalloc(4096);
    if (!buf) {
        fat_close(&src_file);
        fat_close(&dst_file);
        return -1;
    }

    int total = 0;
    int bytes;
    while ((bytes = fat_read(&src_file, buf, 4096)) > 0) {
        int written = fat_write(&dst_file, buf, bytes);
        if (written != bytes) break;
        total += written;
    }

    kfree(buf);
    fat_close(&src_file);
    fat_close(&dst_file);

    kprintf("[FS] Copied %s to %s (%d bytes)\n", src_path, dst_path, total);
    return (total > 0) ? 0 : -1;
}

// Move a file from src_path to dst_path
static int fat_move_inner(fat_fs_t *fs, const char *src_path, const char *dst_path) {
    if (!fs || !src_path || !dst_path) return -1;

    // Simple implementation: copy then delete
    if (fat_copy(fs, src_path, dst_path) != 0) {
        return -1;
    }

    return fat_delete(fs, src_path);
}

// Write entire buffer to a file (creates if doesn't exist, truncates if does)
static int fat_write_file_inner(fat_fs_t *fs, const char *path, const void *data, uint32_t size) {
    if (!fs || !fs->mounted || !path || !data) return -1;

    // Delete existing file if present
    fat_file_t existing;
    if (fat_open(fs, path, &existing) == 0) {
        fat_close(&existing);
        if (fat_delete(fs, path) != 0) {
            return -1;
        }
    }

    // Create new file
    if (fat_create(fs, path) != 0) {
        return -1;
    }

    // Open for writing
    fat_file_t file;
    if (fat_open(fs, path, &file) != 0) {
        return -1;
    }

    // Write data
    int written = fat_write(&file, data, size);

    // Update the short directory entry with the real size + first cluster.
    // Uses the LFN-aware lookup so it works for both 8.3 and long (aliased) names.
    char parent_path[256], filename[256];
    fat_split_path(path, parent_path, filename);

    fat_file_t parent_dir;
    if (fat_open(fs, parent_path, &parent_dir) == 0) {
        fat_dir_entry_t e; uint32_t sidx = 0, fidx = 0;
        if (fat_lookup(fs, parent_dir.first_cluster, filename, &e, &sidx, &fidx) == 0) {
            uint32_t sec, off;
            if (dir_locate(fs, &parent_dir, sidx, &sec, &off, 0) == 0 &&
                fat_read_sector(fs, sec, sector_buf) > 0) {
                fat_dir_entry_t *de = (fat_dir_entry_t *)(sector_buf + off);
                de->file_size = size;
                de->cluster_hi = (file.first_cluster >> 16) & 0xFFFF;
                de->cluster_lo = file.first_cluster & 0xFFFF;
                fat_write_sector(fs, sec, sector_buf);
            }
        }
        fat_close(&parent_dir);
    }

    fat_close(&file);
    kprintf("[FS] Wrote file: %s (%d bytes)\n", path, written);
    return (written == (int)size) ? 0 : -1;
}

// Check if a path exists
static int fat_exists_inner(fat_fs_t *fs, const char *path) {
    if (!fs || !fs->mounted || !path) {
        return -1;
    }

    fat_file_t file;
    if (fat_open(fs, path, &file) == 0) {
        fat_close(&file);
        return 1;  // Exists
    }
    return 0;  // Does not exist
}

// Count free clusters in the FAT
static uint32_t fat_get_free_clusters_inner(fat_fs_t *fs) {
    if (!fs || !fs->mounted) return 0;
    return fs->free_cluster_count;
}
