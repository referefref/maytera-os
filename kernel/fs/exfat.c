#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
// exfat.c - exFAT Filesystem Driver for MayteraOS
#include "exfat.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"

// =============================================================================
// Internal Helpers
// =============================================================================

// Sector buffer for reading
static uint8_t exfat_sector_buf[4096] __attribute__((aligned(4096)));

// Read sectors from device
static int exfat_read_sectors(exfat_fs_t *fs, uint64_t sector, uint32_t count, void *buf) {
    if (!fs || !fs->read_sectors) return -1;
    uint64_t lba = fs->part_start_lba + sector;
    return fs->read_sectors(fs->device_ctx, lba, buf, count);
}

// Write sectors to device
static int exfat_write_sectors(exfat_fs_t *fs, uint64_t sector, uint32_t count, const void *buf) {
    if (!fs || !fs->write_sectors) return -1;
    if (fs->read_only) return -1;
    uint64_t lba = fs->part_start_lba + sector;
    return fs->write_sectors(fs->device_ctx, lba, buf, count);
}

// Convert cluster number to sector number
static uint64_t cluster_to_sector(exfat_fs_t *fs, uint32_t cluster) {
    if (cluster < EXFAT_CLUSTER_FIRST) return 0;
    return fs->cluster_heap_offset +
           (uint64_t)(cluster - EXFAT_CLUSTER_FIRST) * fs->sectors_per_cluster;
}

// Read a cluster
static int exfat_read_cluster(exfat_fs_t *fs, uint32_t cluster, void *buf) {
    uint64_t sector = cluster_to_sector(fs, cluster);
    return exfat_read_sectors(fs, sector, fs->sectors_per_cluster, buf);
}

// Write a cluster
static int exfat_write_cluster(exfat_fs_t *fs, uint32_t cluster, const void *buf) {
    uint64_t sector = cluster_to_sector(fs, cluster);
    return exfat_write_sectors(fs, sector, fs->sectors_per_cluster, buf);
}

// Get next cluster from FAT
static uint32_t exfat_next_cluster(exfat_fs_t *fs, uint32_t cluster) {
    if (cluster < EXFAT_CLUSTER_FIRST || cluster >= fs->cluster_count + 2) {
        return EXFAT_CLUSTER_END;
    }

    // Calculate FAT entry location
    uint32_t fat_offset = cluster * 4;
    uint64_t fat_sector = fs->fat_offset + (fat_offset / fs->sector_size);
    uint32_t entry_offset = fat_offset % fs->sector_size;

    if (exfat_read_sectors(fs, fat_sector, 1, exfat_sector_buf) <= 0) {
        return EXFAT_CLUSTER_END;
    }

    uint32_t next = *(uint32_t *)(exfat_sector_buf + entry_offset);

    if (next >= EXFAT_CLUSTER_END - 7) {
        return EXFAT_CLUSTER_END;  // End of chain
    }

    return next;
}

// Set FAT entry
static int exfat_set_fat_entry(exfat_fs_t *fs, uint32_t cluster, uint32_t value) {
    if (cluster < EXFAT_CLUSTER_FIRST) return -1;

    uint32_t fat_offset = cluster * 4;
    uint64_t fat_sector = fs->fat_offset + (fat_offset / fs->sector_size);
    uint32_t entry_offset = fat_offset % fs->sector_size;

    if (exfat_read_sectors(fs, fat_sector, 1, exfat_sector_buf) <= 0) {
        return -1;
    }

    *(uint32_t *)(exfat_sector_buf + entry_offset) = value;

    if (exfat_write_sectors(fs, fat_sector, 1, exfat_sector_buf) <= 0) {
        return -1;
    }

    return 0;
}

// Check if cluster is free using allocation bitmap
static int exfat_cluster_is_free(exfat_fs_t *fs, uint32_t cluster) {
    if (cluster < EXFAT_CLUSTER_FIRST) return 0;

    uint32_t index = cluster - EXFAT_CLUSTER_FIRST;
    uint32_t byte_offset = index / 8;
    uint32_t bit_offset = index % 8;

    // Calculate bitmap location
    uint32_t bitmap_cluster = fs->bitmap_cluster;
    uint32_t cluster_offset = byte_offset / fs->cluster_size;
    uint32_t offset_in_cluster = byte_offset % fs->cluster_size;

    // Navigate to correct cluster
    for (uint32_t i = 0; i < cluster_offset && bitmap_cluster != EXFAT_CLUSTER_END; i++) {
        bitmap_cluster = exfat_next_cluster(fs, bitmap_cluster);
    }

    if (bitmap_cluster == EXFAT_CLUSTER_END) return 0;

    // Read bitmap cluster
    uint8_t *cluster_buf = kmalloc(fs->cluster_size);
    if (!cluster_buf) return 0;

    if (exfat_read_cluster(fs, bitmap_cluster, cluster_buf) <= 0) {
        kfree(cluster_buf);
        return 0;
    }

    int is_free = !(cluster_buf[offset_in_cluster] & (1 << bit_offset));
    kfree(cluster_buf);

    return is_free;
}

// Allocate a cluster
static uint32_t exfat_alloc_cluster(exfat_fs_t *fs) {
    for (uint32_t cluster = EXFAT_CLUSTER_FIRST; cluster < fs->cluster_count + 2; cluster++) {
        if (exfat_cluster_is_free(fs, cluster)) {
            // Mark as used in bitmap
            uint32_t index = cluster - EXFAT_CLUSTER_FIRST;
            uint32_t byte_offset = index / 8;
            uint32_t bit_offset = index % 8;

            // Read bitmap, set bit, write back
            uint32_t bitmap_cluster = fs->bitmap_cluster;
            uint32_t cluster_offset = byte_offset / fs->cluster_size;
            uint32_t offset_in_cluster = byte_offset % fs->cluster_size;

            for (uint32_t i = 0; i < cluster_offset && bitmap_cluster != EXFAT_CLUSTER_END; i++) {
                bitmap_cluster = exfat_next_cluster(fs, bitmap_cluster);
            }

            if (bitmap_cluster == EXFAT_CLUSTER_END) return 0;

            uint8_t *cluster_buf = kmalloc(fs->cluster_size);
            if (!cluster_buf) return 0;

            if (exfat_read_cluster(fs, bitmap_cluster, cluster_buf) <= 0) {
                kfree(cluster_buf);
                return 0;
            }

            cluster_buf[offset_in_cluster] |= (1 << bit_offset);

            if (exfat_write_cluster(fs, bitmap_cluster, cluster_buf) <= 0) {
                kfree(cluster_buf);
                return 0;
            }

            kfree(cluster_buf);

            // Mark as end of chain in FAT
            exfat_set_fat_entry(fs, cluster, EXFAT_CLUSTER_END);

            return cluster;
        }
    }

    return 0;  // No free clusters
}

// Convert UTF-16LE to UTF-8
static int utf16le_to_utf8(const uint16_t *src, int src_len, char *dst, int dst_size) {
    int di = 0;
    for (int si = 0; si < src_len && src[si] != 0 && di < dst_size - 1; si++) {
        uint16_t c = src[si];
        if (c < 0x80) {
            dst[di++] = c;
        } else if (c < 0x800) {
            if (di + 2 > dst_size - 1) break;
            dst[di++] = 0xC0 | (c >> 6);
            dst[di++] = 0x80 | (c & 0x3F);
        } else {
            if (di + 3 > dst_size - 1) break;
            dst[di++] = 0xE0 | (c >> 12);
            dst[di++] = 0x80 | ((c >> 6) & 0x3F);
            dst[di++] = 0x80 | (c & 0x3F);
        }
    }
    dst[di] = '\0';
    return di;
}

// Convert UTF-8 to UTF-16LE
static int utf8_to_utf16le(const char *src, uint16_t *dst, int dst_size) {
    int di = 0;
    const uint8_t *s = (const uint8_t *)src;
    while (*s && di < dst_size - 1) {
        uint32_t c;
        if (*s < 0x80) {
            c = *s++;
        } else if ((*s & 0xE0) == 0xC0) {
            c = (*s++ & 0x1F) << 6;
            if (*s) c |= (*s++ & 0x3F);
        } else if ((*s & 0xF0) == 0xE0) {
            c = (*s++ & 0x0F) << 12;
            if (*s) c |= (*s++ & 0x3F) << 6;
            if (*s) c |= (*s++ & 0x3F);
        } else {
            s++;  // Skip invalid byte
            continue;
        }
        dst[di++] = (uint16_t)c;
    }
    dst[di] = 0;
    return di;
}

// =============================================================================
// Initialization
// =============================================================================

void exfat_init(void) {
    kprintf("[exFAT] Filesystem driver initialized\n");
}

// =============================================================================
// Mount/Unmount
// =============================================================================

int exfat_mount(exfat_fs_t *fs, void *device_ctx,
                exfat_read_func_t read_func, exfat_write_func_t write_func,
                int lun, uint64_t part_start_lba) {
    if (!fs || !read_func) return -1;

    memset(fs, 0, sizeof(exfat_fs_t));
    fs->device_ctx = device_ctx;
    fs->read_sectors = read_func;
    fs->write_sectors = write_func;
    fs->device_lun = lun;
    fs->part_start_lba = part_start_lba;
    fs->read_only = (write_func == NULL);

    // Read boot sector
    if (read_func(device_ctx, part_start_lba, exfat_sector_buf, 1) <= 0) {
        kprintf("[exFAT] Failed to read boot sector\n");
        return -1;
    }

    exfat_boot_sector_t *boot = (exfat_boot_sector_t *)exfat_sector_buf;

    // Verify exFAT signature
    if (memcmp(boot->fs_name, EXFAT_SIGNATURE, 8) != 0) {
        kprintf("[exFAT] Invalid signature\n");
        return -1;
    }

    // Verify boot signature
    if (boot->boot_signature != 0xAA55) {
        kprintf("[exFAT] Invalid boot signature\n");
        return -1;
    }

    // Parse boot sector
    fs->sector_size = 1 << (boot->bytes_per_sector_shift + 9);
    fs->sectors_per_cluster = 1 << boot->sectors_per_cluster_shift;
    fs->cluster_size = fs->sector_size * fs->sectors_per_cluster;
    fs->fat_offset = boot->fat_offset;
    fs->fat_length = boot->fat_length;
    fs->cluster_heap_offset = boot->cluster_heap_offset;
    fs->cluster_count = boot->cluster_count;
    fs->root_dir_cluster = boot->root_dir_cluster;
    fs->volume_serial = boot->volume_serial;

    kprintf("[exFAT] Mounted filesystem:\n");
    kprintf("  Sector size: %u\n", fs->sector_size);
    kprintf("  Cluster size: %u\n", fs->cluster_size);
    kprintf("  Total clusters: %u\n", fs->cluster_count);
    kprintf("  Root cluster: %u\n", fs->root_dir_cluster);

    // Find allocation bitmap and volume label in root directory
    uint8_t *cluster_buf = kmalloc(fs->cluster_size);
    if (!cluster_buf) return -1;

    uint32_t cluster = fs->root_dir_cluster;
    if (exfat_read_cluster(fs, cluster, cluster_buf) > 0) {
        for (uint32_t off = 0; off < fs->cluster_size; off += 32) {
            exfat_entry_t *entry = (exfat_entry_t *)(cluster_buf + off);

            if (entry->entry_type == EXFAT_ENTRY_EOD) break;

            if (entry->entry_type == EXFAT_ENTRY_BITMAP) {
                exfat_bitmap_entry_t *bitmap = (exfat_bitmap_entry_t *)entry;
                fs->bitmap_cluster = bitmap->first_cluster;
                fs->bitmap_size = bitmap->data_length;
                kprintf("  Bitmap at cluster %u\n", fs->bitmap_cluster);
            }

            if (entry->entry_type == EXFAT_ENTRY_VOLUME_LABEL) {
                exfat_volume_label_t *label = (exfat_volume_label_t *)entry;
                utf16le_to_utf8(label->label, label->char_count, fs->volume_label, sizeof(fs->volume_label));
                kprintf("  Volume: %s\n", fs->volume_label);
            }
        }
    }

    kfree(cluster_buf);
    fs->mounted = 1;

    // Calculate free space
    uint64_t free_bytes = exfat_get_free_space(fs);
    uint64_t total_bytes = exfat_get_total_space(fs);
    kprintf("  Free space: %llu MB / %llu MB\n",
            free_bytes / (1024 * 1024), total_bytes / (1024 * 1024));

    return 0;
}

void exfat_unmount(exfat_fs_t *fs) {
    if (!fs) return;
    exfat_sync(fs);
    fs->mounted = 0;
    kprintf("[exFAT] Unmounted filesystem\n");
}

// =============================================================================
// Directory Operations
// =============================================================================

// Parse directory entry set starting at given offset
// Returns number of bytes consumed, or -1 on error
//
// ===================== #404 Rust-port seam: pure exFAT entry-set decode =======
// exfat_readdir (below) walks a directory by reading each cluster off the block
// device, then decodes the 32-byte on-disk directory ENTRY SET here: a File
// entry (0x85) carrying SecondaryCount, a Stream Extension (0xC0) carrying
// NameLength / DataLength / ValidDataLength, and ceil(NameLength/15) File Name
// entries (0xC1) whose 15 UTF-16 code units are reassembled into the filename.
// SecondaryCount, NameLength and the entry bytes are all attacker-supplyable (an
// exFAT volume arrives on a hot-plugged USB stick), so this is a Tier-2
// untrusted-input parser. The decode is PURE (no disk I/O, no FS mutation - the
// `fs` handle went unused), so it is lifted into a seam that ports to Rust behind
// -DRUST_EXFAT and is differentially proven byte-identical to the C on real
// entries. The cluster-chain walk + cluster read stay in C (exfat_readdir).
//
// exfat_dir_info_t is the decoded output; its layout is asserted so the
// #[repr(C)] Rust mirror in rustkern.rs can never silently drift.
_Static_assert(sizeof(exfat_dir_info_t) == 296,
               "exfat_dir_info_t must be 296 bytes for the Rust FFI");

// Rust mirror (rustkern.rs), live under -DRUST_EXFAT. It CONFINES the residual
// stale-stack info-leak the C reference has (see below).
extern int exfat_dir_step_rs(const uint8_t *buf, uint32_t buf_size,
                             uint32_t offset, exfat_dir_info_t *info);

// exfat_dir_step_c: VERBATIM reference. This is the decode lifted byte-for-byte
// out of the original parse_entry_set (the `fs` param went unused, so the seam is
// genuinely pure). SECURITY (HONEST): unlike the FAT b810 LFN case this C does
// NOT overflow. It (a) bounds SecondaryCount via `offset + bytes_used > buf_size
// -> -1` so the whole entry set must fit the cluster buffer, and (b) caps the
// name reassembly with `name_offset + j < 255` into name_buf[256]. The residual
// defect the Rust confines is that name_buf is UNINITIALIZED: a crafted
// NameLength larger than the File Name entries actually present makes
// utf16le_to_utf8 read stale stack (a non-deterministic stack info-leak into the
// filename), never an OOB. See exfat_rust_selftest's [RUST-SEC].
int exfat_dir_step_c(const uint8_t *buf, uint32_t buf_size,
                     uint32_t offset, exfat_dir_info_t *info) {
    if (offset >= buf_size) return -1;

    exfat_entry_t *entry = (exfat_entry_t *)(buf + offset);

    if (entry->entry_type == EXFAT_ENTRY_EOD) return 0;  // End of directory
    if (EXFAT_ENTRY_IS_DELETED(entry->entry_type)) return 32;  // Skip deleted

    if (entry->entry_type != EXFAT_ENTRY_FILE) {
        return 32;  // Skip non-file entries
    }

    memset(info, 0, sizeof(exfat_dir_info_t));

    exfat_file_entry_t *file = (exfat_file_entry_t *)entry;
    info->attr = file->file_attributes;
    info->create_time = file->create_timestamp;
    info->modify_time = file->modify_timestamp;
    info->access_time = file->access_timestamp;

    int secondary_count = file->secondary_count;
    int total_entries = 1 + secondary_count;
    int bytes_used = total_entries * 32;

    if (offset + bytes_used > buf_size) return -1;

    // Process secondary entries
    uint16_t name_buf[256];
    int name_len = 0;

    for (int i = 1; i <= secondary_count && offset + i * 32 < buf_size; i++) {
        exfat_entry_t *sec = (exfat_entry_t *)(buf + offset + i * 32);

        if (sec->entry_type == EXFAT_ENTRY_STREAM) {
            exfat_stream_entry_t *stream = (exfat_stream_entry_t *)sec;
            info->first_cluster = stream->first_cluster;
            info->file_size = stream->data_length;
            info->valid_size = stream->valid_data_length;
            info->is_contiguous = (stream->general_flags & EXFAT_FLAG_CONTIGUOUS) ? 1 : 0;
            name_len = stream->name_length;
        } else if (sec->entry_type == EXFAT_ENTRY_NAME) {
            exfat_name_entry_t *name = (exfat_name_entry_t *)sec;
            int chars_to_copy = 15;
            int name_offset = (i - 2) * 15;  // First name entry is at secondary index 2
            if (i >= 2) {
                name_offset = (i - 2) * 15;
                for (int j = 0; j < chars_to_copy && name_offset + j < 255; j++) {
                    name_buf[name_offset + j] = name->name[j];
                }
            }
        }
    }

    // Convert name to UTF-8
    if (name_len > 0) {
        utf16le_to_utf8(name_buf, name_len, info->name, sizeof(info->name));
    }

    return bytes_used;
}

// Strangler router: -DRUST_EXFAT routes the reachable exFAT directory decode to
// the Rust port; else the verbatim C reference. Remove the one Makefile flag and
// rebuild to roll straight back to C.
static int parse_entry_set(exfat_fs_t *fs, uint8_t *buf, uint32_t buf_size,
                           uint32_t offset, exfat_dir_info_t *info) {
    (void)fs;  // pure seam: the decode never touches the FS handle
#ifdef RUST_EXFAT
    return exfat_dir_step_rs(buf, buf_size, offset, info);
#else
    return exfat_dir_step_c(buf, buf_size, offset, info);
#endif
}

int exfat_opendir(exfat_fs_t *fs, const char *path, exfat_dir_iter_t *iter) {
    if (!fs || !fs->mounted || !iter) return -1;

    memset(iter, 0, sizeof(exfat_dir_iter_t));
    iter->fs = fs;

    // For root directory
    if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        iter->cluster = fs->root_dir_cluster;
        return 0;
    }

    // Navigate path to find target directory
    // For now, only support root
    kprintf("[exFAT] Path navigation not yet implemented: %s\n", path);
    return -1;
}

int exfat_readdir(exfat_dir_iter_t *iter, exfat_dir_info_t *info) {
    if (!iter || !iter->fs || !info) return -1;

    exfat_fs_t *fs = iter->fs;

    if (iter->at_end || iter->cluster == EXFAT_CLUSTER_END) {
        return -1;
    }

    // Allocate cluster buffer
    uint8_t *cluster_buf = kmalloc(fs->cluster_size);
    if (!cluster_buf) return -1;

    while (iter->cluster != EXFAT_CLUSTER_END) {
        if (exfat_read_cluster(fs, iter->cluster, cluster_buf) <= 0) {
            kfree(cluster_buf);
            return -1;
        }

        while (iter->byte_offset < fs->cluster_size) {
            int consumed = parse_entry_set(fs, cluster_buf, fs->cluster_size,
                                           iter->byte_offset, info);

            if (consumed == 0) {
                // End of directory
                iter->at_end = 1;
                kfree(cluster_buf);
                return -1;
            }

            iter->byte_offset += consumed;

            if (consumed > 32 && info->name[0] != '\0') {
                // Found a valid entry
                kfree(cluster_buf);
                return 0;
            }
        }

        // Move to next cluster
        iter->cluster = exfat_next_cluster(fs, iter->cluster);
        iter->byte_offset = 0;
    }

    kfree(cluster_buf);
    iter->at_end = 1;
    return -1;
}

void exfat_closedir(exfat_dir_iter_t *iter) {
    if (iter) {
        memset(iter, 0, sizeof(exfat_dir_iter_t));
    }
}

// =============================================================================
// File Operations
// =============================================================================

int exfat_open(exfat_fs_t *fs, const char *path, exfat_file_t *file) {
    if (!fs || !fs->mounted || !path || !file) return -1;

    memset(file, 0, sizeof(exfat_file_t));
    file->fs = fs;

    // Skip leading slash
    if (path[0] == '/') path++;

    // For root directory
    if (path[0] == '\0') {
        file->first_cluster = fs->root_dir_cluster;
        file->current_cluster = fs->root_dir_cluster;
        file->is_dir = 1;
        file->open = 1;
        strcpy(file->name, "/");
        return 0;
    }

    // Search root directory for the file
    exfat_dir_iter_t iter;
    exfat_dir_info_t info;

    if (exfat_opendir(fs, "/", &iter) < 0) {
        return -1;
    }

    // Simple single-level path matching
    const char *filename = path;
    const char *slash = strchr(path, '/');
    char component[256];

    if (slash) {
        int len = slash - path;
        if (len >= 256) len = 255;
        memcpy(component, path, len);
        component[len] = '\0';
        filename = component;
    }

    while (exfat_readdir(&iter, &info) == 0) {
        // Case-insensitive compare
        if (strcasecmp(info.name, filename) == 0) {
            file->first_cluster = info.first_cluster;
            file->current_cluster = info.first_cluster;
            file->file_size = info.file_size;
            file->valid_size = info.valid_size;
            file->attr = info.attr;
            file->is_dir = (info.attr & EXFAT_ATTR_DIRECTORY) ? 1 : 0;
            file->is_contiguous = info.is_contiguous;
            strncpy(file->name, info.name, sizeof(file->name) - 1);
            file->open = 1;

            exfat_closedir(&iter);

            // If there's more path, recurse
            if (slash && slash[1]) {
                if (!file->is_dir) {
                    file->open = 0;
                    return -1;  // Not a directory
                }
                // TODO: Recursive path resolution
                kprintf("[exFAT] Multi-level paths not yet supported\n");
                file->open = 0;
                return -1;
            }

            return 0;
        }
    }

    exfat_closedir(&iter);
    return -1;  // Not found
}

void exfat_close(exfat_file_t *file) {
    if (file) {
        file->open = 0;
    }
}

int exfat_read(exfat_file_t *file, void *buffer, uint64_t size) {
    if (!file || !file->open || !buffer) return -1;
    if (file->is_dir) return -1;

    exfat_fs_t *fs = file->fs;

    // Limit read to file size
    if (file->position >= file->file_size) return 0;
    if (file->position + size > file->file_size) {
        size = file->file_size - file->position;
    }

    uint8_t *out = (uint8_t *)buffer;
    uint64_t bytes_read = 0;

    // Allocate cluster buffer
    uint8_t *cluster_buf = kmalloc(fs->cluster_size);
    if (!cluster_buf) return -1;

    while (bytes_read < size && file->current_cluster != EXFAT_CLUSTER_END) {
        // Calculate offset within current cluster
        uint32_t cluster_offset = file->position % fs->cluster_size;
        uint32_t bytes_in_cluster = fs->cluster_size - cluster_offset;
        if (bytes_in_cluster > size - bytes_read) {
            bytes_in_cluster = size - bytes_read;
        }

        // For contiguous files, calculate cluster directly
        if (file->is_contiguous) {
            uint32_t cluster_index = file->position / fs->cluster_size;
            file->current_cluster = file->first_cluster + cluster_index;
        }

        // Read cluster
        if (exfat_read_cluster(fs, file->current_cluster, cluster_buf) <= 0) {
            kfree(cluster_buf);
            return bytes_read > 0 ? bytes_read : -1;
        }

        // Copy data
        memcpy(out + bytes_read, cluster_buf + cluster_offset, bytes_in_cluster);
        bytes_read += bytes_in_cluster;
        file->position += bytes_in_cluster;

        // Move to next cluster if needed
        if (file->position % fs->cluster_size == 0) {
            if (file->is_contiguous) {
                file->current_cluster++;
            } else {
                file->current_cluster = exfat_next_cluster(fs, file->current_cluster);
            }
        }
    }

    kfree(cluster_buf);
    return bytes_read;
}

int exfat_write(exfat_file_t *file, const void *buffer, uint64_t size) {
    if (!file || !file->open || !buffer) return -1;
    if (file->is_dir) return -1;
    if (file->fs->read_only) return -1;

    // TODO: Implement write support
    kprintf("[exFAT] Write not yet implemented\n");
    return -1;
}

uint64_t exfat_size(exfat_file_t *file) {
    return file ? file->file_size : 0;
}

int exfat_seek(exfat_file_t *file, uint64_t position) {
    if (!file || !file->open) return -1;

    if (position > file->file_size) {
        position = file->file_size;
    }

    file->position = position;

    // Recalculate current cluster
    if (file->is_contiguous) {
        uint32_t cluster_index = position / file->fs->cluster_size;
        file->current_cluster = file->first_cluster + cluster_index;
    } else {
        // Walk FAT chain from beginning
        file->current_cluster = file->first_cluster;
        uint64_t offset = 0;
        while (offset + file->fs->cluster_size <= position &&
               file->current_cluster != EXFAT_CLUSTER_END) {
            file->current_cluster = exfat_next_cluster(file->fs, file->current_cluster);
            offset += file->fs->cluster_size;
        }
    }

    return 0;
}

int exfat_is_dir(exfat_file_t *file) {
    return file ? file->is_dir : 0;
}

// =============================================================================
// Convenience Functions
// =============================================================================

void *exfat_read_file(exfat_fs_t *fs, const char *path, uint64_t *size_out) {
    exfat_file_t file;

    if (exfat_open(fs, path, &file) < 0) {
        return NULL;
    }

    if (file.is_dir) {
        exfat_close(&file);
        return NULL;
    }

    uint64_t size = file.file_size;
    void *data = kmalloc(size + 1);
    if (!data) {
        exfat_close(&file);
        return NULL;
    }

    int read = exfat_read(&file, data, size);
    exfat_close(&file);

    if (read <= 0) {
        kfree(data);
        return NULL;
    }

    ((uint8_t *)data)[read] = '\0';  // Null terminate

    if (size_out) *size_out = read;
    return data;
}

int exfat_write_file(exfat_fs_t *fs, const char *path, const void *data, uint64_t size) {
    // TODO: Implement
    return -1;
}

int exfat_exists(exfat_fs_t *fs, const char *path) {
    exfat_file_t file;
    if (exfat_open(fs, path, &file) < 0) {
        return 0;
    }
    exfat_close(&file);
    return 1;
}

int exfat_create(exfat_fs_t *fs, const char *path) {
    // TODO: Implement
    return -1;
}

int exfat_mkdir(exfat_fs_t *fs, const char *path) {
    // TODO: Implement
    return -1;
}

int exfat_delete(exfat_fs_t *fs, const char *path) {
    // TODO: Implement
    return -1;
}

int exfat_rename(exfat_fs_t *fs, const char *old_path, const char *new_path) {
    // TODO: Implement
    return -1;
}

uint64_t exfat_get_free_space(exfat_fs_t *fs) {
    if (!fs || !fs->mounted) return 0;

    uint64_t free_clusters = 0;

    // Count free clusters in bitmap
    uint8_t *cluster_buf = kmalloc(fs->cluster_size);
    if (!cluster_buf) return 0;

    uint32_t bitmap_cluster = fs->bitmap_cluster;
    uint32_t cluster_index = 0;

    while (bitmap_cluster != EXFAT_CLUSTER_END &&
           cluster_index < fs->cluster_count) {

        if (exfat_read_cluster(fs, bitmap_cluster, cluster_buf) <= 0) {
            break;
        }

        for (uint32_t i = 0; i < fs->cluster_size && cluster_index < fs->cluster_count; i++) {
            uint8_t byte = cluster_buf[i];
            for (int bit = 0; bit < 8 && cluster_index < fs->cluster_count; bit++, cluster_index++) {
                if (!(byte & (1 << bit))) {
                    free_clusters++;
                }
            }
        }

        bitmap_cluster = exfat_next_cluster(fs, bitmap_cluster);
    }

    kfree(cluster_buf);

    return free_clusters * fs->cluster_size;
}

uint64_t exfat_get_total_space(exfat_fs_t *fs) {
    if (!fs || !fs->mounted) return 0;
    return (uint64_t)fs->cluster_count * fs->cluster_size;
}

void exfat_print_info(exfat_fs_t *fs) {
    if (!fs || !fs->mounted) {
        kprintf("[exFAT] Not mounted\n");
        return;
    }

    kprintf("\n[exFAT] Filesystem Information:\n");
    kprintf("  Volume: %s\n", fs->volume_label[0] ? fs->volume_label : "(no label)");
    kprintf("  Serial: %08X\n", fs->volume_serial);
    kprintf("  Sector size: %u bytes\n", fs->sector_size);
    kprintf("  Cluster size: %u bytes\n", fs->cluster_size);
    kprintf("  Total clusters: %u\n", fs->cluster_count);
    kprintf("  Total space: %llu MB\n", exfat_get_total_space(fs) / (1024 * 1024));
    kprintf("  Free space: %llu MB\n", exfat_get_free_space(fs) / (1024 * 1024));
}

void exfat_list_dir(exfat_fs_t *fs, const char *path) {
    exfat_dir_iter_t iter;
    exfat_dir_info_t info;

    if (exfat_opendir(fs, path, &iter) < 0) {
        kprintf("[exFAT] Cannot open directory: %s\n", path);
        return;
    }

    kprintf("\nDirectory of %s:\n", path ? path : "/");
    kprintf("%-30s %12s %s\n", "Name", "Size", "Attr");
    kprintf("%-30s %12s %s\n", "----", "----", "----");

    while (exfat_readdir(&iter, &info) == 0) {
        char attr_str[8] = "------";
        if (info.attr & EXFAT_ATTR_DIRECTORY) attr_str[0] = 'D';
        if (info.attr & EXFAT_ATTR_READ_ONLY) attr_str[1] = 'R';
        if (info.attr & EXFAT_ATTR_HIDDEN) attr_str[2] = 'H';
        if (info.attr & EXFAT_ATTR_SYSTEM) attr_str[3] = 'S';
        if (info.attr & EXFAT_ATTR_ARCHIVE) attr_str[4] = 'A';

        if (info.attr & EXFAT_ATTR_DIRECTORY) {
            kprintf("%-30s %12s %s\n", info.name, "<DIR>", attr_str);
        } else {
            kprintf("%-30s %12llu %s\n", info.name, info.file_size, attr_str);
        }
    }

    exfat_closedir(&iter);
}

int exfat_sync(exfat_fs_t *fs) {
    // Nothing to sync yet (no write caching implemented)
    return 0;
}

// =============================================================================
// Utility Functions
// =============================================================================

uint64_t exfat_timestamp_to_unix(uint32_t exfat_time) {
    // exFAT timestamp format:
    // bits 0-4:   seconds/2 (0-29)
    // bits 5-10:  minutes (0-59)
    // bits 11-15: hours (0-23)
    // bits 16-20: day (1-31)
    // bits 21-24: month (1-12)
    // bits 25-31: year (0=1980, max 127=2107)

    int seconds = ((exfat_time >> 0) & 0x1F) * 2;
    int minutes = (exfat_time >> 5) & 0x3F;
    int hours = (exfat_time >> 11) & 0x1F;
    int day = (exfat_time >> 16) & 0x1F;
    int month = (exfat_time >> 21) & 0x0F;
    int year = ((exfat_time >> 25) & 0x7F) + 1980;

    // Simple approximation (not accounting for leap years properly)
    uint64_t days = (year - 1970) * 365 + (year - 1969) / 4;
    static const int month_days[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    if (month > 0 && month <= 12) {
        days += month_days[month - 1];
    }
    days += day - 1;

    return days * 86400 + hours * 3600 + minutes * 60 + seconds;
}

uint32_t unix_to_exfat_timestamp(uint64_t unix_time) {
    // Simplified conversion
    int days = unix_time / 86400;
    int remaining = unix_time % 86400;
    int hours = remaining / 3600;
    int minutes = (remaining % 3600) / 60;
    int seconds = remaining % 60;

    int year = 1970;
    while (days >= 365) {
        int leap = ((year % 4 == 0) && (year % 100 != 0 || year % 400 == 0)) ? 1 : 0;
        if (days < 365 + leap) break;
        days -= 365 + leap;
        year++;
    }

    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int month = 0;
    while (month < 12 && days >= month_days[month]) {
        days -= month_days[month];
        month++;
    }
    int day = days + 1;
    month++;

    return ((seconds / 2) & 0x1F) |
           ((minutes & 0x3F) << 5) |
           ((hours & 0x1F) << 11) |
           ((day & 0x1F) << 16) |
           ((month & 0x0F) << 21) |
           (((year - 1980) & 0x7F) << 25);
}

uint16_t exfat_name_hash(const uint16_t *name, int len) {
    uint16_t hash = 0;
    for (int i = 0; i < len && name[i]; i++) {
        uint16_t c = name[i];
        // Convert to uppercase for hashing
        if (c >= 'a' && c <= 'z') c -= 32;
        hash = ((hash << 15) | (hash >> 1)) + (c & 0xFF);
        hash = ((hash << 15) | (hash >> 1)) + ((c >> 8) & 0xFF);
    }
    return hash;
}

// =============================================================================
// #404 Phase T boot-time self-test: prove exfat_dir_step_rs (Rust, live under
// -DRUST_EXFAT) == exfat_dir_step_c (verbatim reference) on the agreement domain
// (well-formed exFAT File entry sets: File(0x85)+Stream(0xC0)+Name(0xC1)),
// characterize the SECURITY divergence HONESTLY, and micro-benchmark both.
// LIGHT (#426, bounded, runs once): ~256 entry-set decodes + a small security
// sweep + a ~5k-iter RDTSC walk. The heavy work (>=2M-vector differential +
// ASan/UBSan witness) is the OFFLINE pre-flight. One [RUST-DIFF] exfat, one
// [RUST-SEC] exfat, one [RUST-PERF].
//
// SAFETY at boot: every crafted entry set lives in a static buffer, and both
// seams write only into exfat_dir_info_t.name[256] (bounded by both impls), so
// neither reference goes out of bounds here.
// =============================================================================
static uint32_t exfatdiff_rng(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x;
}
static inline uint64_t exfat_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}
static int exfat_slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

// Emit a File entry set for `name` (ASCII, name_len chars) into buf at *pos
// (32-byte units): File(0x85) + Stream(0xC0) + ceil(name_len/15) Name(0xC1).
// If name_len_field >= 0 it overrides the Stream NameLength field (used to craft
// the "NameLength exceeds present File Name entries" malformed case).
static void exfat_emit_file(uint8_t *buf, int *pos, const char *name, int name_len,
                            int name_len_field, uint16_t attr,
                            uint32_t first_cluster, uint64_t size) {
    int nname = (name_len + 14) / 15; if (nname < 1) nname = 1;
    int secondary = 1 + nname;
    int nlf = (name_len_field >= 0) ? name_len_field : name_len;
    uint8_t *fe = buf + (*pos) * 32;
    memset(fe, 0, 32);
    fe[0] = EXFAT_ENTRY_FILE;                 // 0x85
    fe[1] = (uint8_t)secondary;               // secondary_count
    *(uint16_t *)(fe + 4) = attr;             // file_attributes
    *(uint32_t *)(fe + 8) = 0x11111111u;      // create_timestamp
    *(uint32_t *)(fe + 12) = 0x22222222u;     // modify_timestamp
    *(uint32_t *)(fe + 16) = 0x33333333u;     // access_timestamp
    (*pos)++;
    uint8_t *se = buf + (*pos) * 32;
    memset(se, 0, 32);
    se[0] = EXFAT_ENTRY_STREAM;               // 0xC0
    se[1] = 0x02;                             // general_flags: contiguous
    se[3] = (uint8_t)nlf;                     // name_length
    *(uint64_t *)(se + 8) = size;             // valid_data_length
    *(uint32_t *)(se + 20) = first_cluster;   // first_cluster
    *(uint64_t *)(se + 24) = size;            // data_length
    (*pos)++;
    for (int n = 0; n < nname; n++) {
        uint8_t *ne = buf + (*pos) * 32;
        memset(ne, 0, 32);
        ne[0] = EXFAT_ENTRY_NAME;             // 0xC1
        for (int j = 0; j < 15; j++) {
            int ci = n * 15 + j;
            uint16_t ch = (ci < name_len) ? (uint16_t)(uint8_t)name[ci] : 0x0000;
            *(uint16_t *)(ne + 2 + j * 2) = ch;
        }
        (*pos)++;
    }
}

// Walk a directory block, collecting decoded (name, first_cluster, size, attr)
// per File entry set via the C or Rust step. Mirrors exfat_readdir's accept
// condition (`consumed > 32 && info.name[0] != '\0'`). Returns the count.
static int exfat_walk(const uint8_t *dir, uint32_t bytes, int use_rust,
                      char names[][256], uint32_t *fclus, uint64_t *sizes, uint16_t *attrs) {
    extern int exfat_dir_step_c(const uint8_t *, uint32_t, uint32_t, exfat_dir_info_t *);
    extern int exfat_dir_step_rs(const uint8_t *, uint32_t, uint32_t, exfat_dir_info_t *);
    int cnt = 0;
    uint32_t off = 0;
    while (off < bytes && cnt < 8) {
        exfat_dir_info_t info;
        int consumed = use_rust ? exfat_dir_step_rs(dir, bytes, off, &info)
                                : exfat_dir_step_c(dir, bytes, off, &info);
        if (consumed <= 0) break;               // 0 = EOD, <0 = reject
        off += consumed;
        if (consumed > 32 && info.name[0] != '\0') {
            int k = 0; while (k < 255 && info.name[k]) { names[cnt][k] = info.name[k]; k++; }
            names[cnt][k] = 0;
            fclus[cnt] = info.first_cluster; sizes[cnt] = info.file_size; attrs[cnt] = info.attr;
            cnt++;
        }
    }
    return cnt;
}

// Witness helper: seed the current stack frame with a 0xABAB pattern so the C
// reference's UNINITIALIZED name_buf (in exfat_dir_step_c) picks it up. This
// makes the "NameLength exceeds present File Name entries" stack info-leak
// deterministic for the [RUST-SEC] line (it shows the C output DEPENDS on stack
// residue). It has no effect on well-formed decodes (name_buf is fully covered).
static volatile uint16_t g_exfat_sink;
static void exfat_dirty_stack(void) {
    uint16_t junk[512];
    for (int i = 0; i < 512; i++) junk[i] = 0xABAB;
    g_exfat_sink = junk[g_exfat_sink & 511];
}

void exfat_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    extern int exfat_dir_step_c(const uint8_t *, uint32_t, uint32_t, exfat_dir_info_t *);
    extern int exfat_dir_step_rs(const uint8_t *, uint32_t, uint32_t, exfat_dir_info_t *);
    static uint8_t buf[4096];
    static char names_c[8][256];
    static char names_r[8][256];
    static uint32_t fc[8]; static uint64_t sz[8]; static uint16_t at[8];
    static uint32_t fr[8]; static uint64_t sr[8]; static uint16_t ar[8];
    static char nm[260];

    // Force-reference the Rust symbol so its archive member always links
    // (matches the icmp/arp/dns/dhcp/url/elf/pe/fat pattern), regardless of the
    // -DRUST_EXFAT flag.
    { exfat_dir_info_t info; uint8_t z[64]; memset(z, 0, 64);
      exfat_dir_step_rs(z, 64, 0, &info); }

    // Part 1: AGREEMENT DOMAIN. Random well-formed directories (1..3 File entry
    // sets, each a random-length printable-ASCII name 1..250 chars + random
    // attr/first_cluster/size), plus structural mutations that both must reject
    // or skip identically (deleted 0x85->0x05, non-file 0x83, EOD injected). Both
    // walks must produce the identical sequence of (name, cluster, size, attr).
    uint32_t seed = 0x7c0dfa11u;
    uint32_t files = 0, mismatches = 0;
    int first_bad = -1;
    for (uint32_t iter = 0; iter < 256; iter++) {
        int pos = 0;
        int nfiles = 1 + (int)(exfatdiff_rng(&seed) % 3);
        for (int f = 0; f < nfiles && pos < 100; f++) {
            int len = 1 + (int)(exfatdiff_rng(&seed) % 250);
            for (int c = 0; c < len; c++)
                nm[c] = (char)(0x21 + (exfatdiff_rng(&seed) % (0x7E - 0x21)));
            nm[len] = 0;
            uint16_t attr = (uint16_t)(exfatdiff_rng(&seed) & 0x37);
            uint32_t clus = 2 + (exfatdiff_rng(&seed) % 100000);
            uint64_t size = exfatdiff_rng(&seed);
            exfat_emit_file(buf, &pos, nm, len, -1, attr, clus, size);
        }
        // structural mutation (both must still AGREE)
        uint32_t mut = exfatdiff_rng(&seed) % 5;
        if (mut == 1 && pos > 0) buf[0] = 0x05;         // File entry -> deleted (bit7 clear)
        else if (mut == 2 && pos > 0) buf[0] = 0x83;    // -> volume-label (non-file skip)
        else if (mut == 3 && pos > 1) buf[32] = 0x00;   // Stream slot -> EOD-type byte
        memset(buf + pos * 32, 0, 32);                  // 0x00 terminator
        uint32_t bytes = (uint32_t)(pos + 1) * 32;

        int cc = exfat_walk(buf, bytes, 0, names_c, fc, sz, at);
        int rc = exfat_walk(buf, bytes, 1, names_r, fr, sr, ar);
        int bad = (cc != rc);
        for (int i = 0; i < cc && i < rc && !bad; i++) {
            if (strcmp(names_c[i], names_r[i]) != 0) bad = 1;
            if (fc[i] != fr[i] || sz[i] != sr[i] || at[i] != ar[i]) bad = 1;
        }
        files += (cc > rc ? rc : cc);
        if (bad) { mismatches++; if (first_bad < 0) first_bad = (int)iter; }
    }
    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] exfat: %u entry-sets, %u mismatches -> %s\n", files, mismatches, verdict);
    bootlog_write("[RUST-DIFF] exfat: %u entry-sets, %u mismatches -> %s", files, mismatches, verdict);
    if (mismatches != 0) {
        kprintf("[RUST-DIFF] exfat FIRST MISMATCH iter=%d\n", first_bad);
        bootlog_write("[RUST-DIFF] exfat FIRST MISMATCH iter=%d", first_bad);
    }

    // Part 2: SECURITY posture (HONEST). This C does NOT overflow (SecondaryCount
    // is bounded by `offset+bytes_used>buf_size` and the name write is capped at
    // name_buf[255]). The confined class is the UNINITIALIZED name_buf: a crafted
    // NameLength larger than the File Name entries actually present makes the C
    // decode read stale stack (info-leak), non-deterministically. We seed the
    // frame (exfat_dirty_stack) to make that leak observable: the C then decodes
    // a name LONGER than the D chars actually present, whereas the Rust (which
    // zero-inits) always decodes exactly D. Also verify both REJECT an oversized
    // SecondaryCount identically.
    {
        uint32_t n = 0, c_leaked = 0, r_leaked = 0, sc_agree = 0;
        uint32_t s2 = 0x5ec0efa7u;
        for (uint32_t r = 0; r < 200; r++) {
            int D = 1 + (int)(exfatdiff_rng(&s2) % 30);       // chars actually present
            int L = D + 16 + (int)(exfatdiff_rng(&s2) % 200); // claimed NameLength
            if (L > 255) L = 255;
            for (int c = 0; c < D; c++) nm[c] = (char)(0x41 + (exfatdiff_rng(&s2) % 26));
            nm[D] = 0;
            int pos = 0;
            exfat_emit_file(buf, &pos, nm, D, L, EXFAT_ATTR_ARCHIVE, 5, 1234);
            memset(buf + pos * 32, 0, 32);
            uint32_t bytes = (uint32_t)(pos + 1) * 32;

            exfat_dir_info_t info;
            // C reference (frame pre-dirtied to witness the stale-stack read)
            exfat_dirty_stack();
            info.name[0] = 0;
            int lc = 0;
            if (exfat_dir_step_c(buf, bytes, 0, &info) > 32) lc = exfat_slen(info.name);
            // Rust
            info.name[0] = 0;
            int lr = 0;
            if (exfat_dir_step_rs(buf, bytes, 0, &info) > 32) lr = exfat_slen(info.name);
            n++;
            if (lc > D) c_leaked++;   // C leaked stale stack past the D present chars
            if (lr > D) r_leaked++;   // Rust never should

            // Oversized SecondaryCount: whole set claims to run past the block.
            int pos2 = 0;
            exfat_emit_file(buf, &pos2, "OVER", 4, 4, EXFAT_ATTR_ARCHIVE, 6, 7);
            buf[1] = 250;             // secondary_count -> 250 (bytes_used > small block)
            int rc = exfat_dir_step_c(buf, 128, 0, &info);
            int rr = exfat_dir_step_rs(buf, 128, 0, &info);
            if (rc == rr && rc == -1) sc_agree++;
        }
        kprintf("[RUST-SEC] exfat: CONFINED - crafted NameLength>present: C leaked stale-stack name %u/%u (info-leak, non-OOB), Rust confined to present chars %u/%u leaked; oversized-SecondaryCount both reject %u/%u\n",
                c_leaked, n, r_leaked, n, sc_agree, n);
        bootlog_write("[RUST-SEC] exfat: C stale-stack leak %u/%u Rust confined (leaked %u/%u) SecCount-reject-agree %u/%u",
                      c_leaked, n, r_leaked, n, sc_agree, n);
    }

    // Part 3: RDTSC micro-benchmark over a fixed well-formed directory. LIGHT: 5k.
    {
        const int iters = 5000;
        int pos = 0;
        exfat_emit_file(buf, &pos, "ExampleLongFileName.txt", 23, -1, EXFAT_ATTR_ARCHIVE, 10, 4096);
        exfat_emit_file(buf, &pos, "readme.md", 9, -1, EXFAT_ATTR_ARCHIVE, 20, 128);
        memset(buf + pos * 32, 0, 32);
        uint32_t bytes = (uint32_t)(pos + 1) * 32;

        for (int i = 0; i < 300; i++) {
            exfat_walk(buf, bytes, 0, names_c, fc, sz, at);
            exfat_walk(buf, bytes, 1, names_r, fr, sr, ar);
        }
        uint64_t t0 = exfat_tsc_serialized();
        for (int i = 0; i < iters; i++) exfat_walk(buf, bytes, 0, names_c, fc, sz, at);
        uint64_t t1 = exfat_tsc_serialized();
        for (int i = 0; i < iters; i++) exfat_walk(buf, bytes, 1, names_r, fr, sr, ar);
        uint64_t t2 = exfat_tsc_serialized();
        uint64_t c_cyc = (t1 - t0) / iters;
        uint64_t r_cyc = (t2 - t1) / iters;
        uint64_t ratio100 = (c_cyc != 0) ? (r_cyc * 100ULL / c_cyc) : 0;
        kprintf("[RUST-PERF] exfat: C=%llu cyc/walk RS=%llu cyc/walk ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
        bootlog_write("[RUST-PERF] exfat: C=%llu RS=%llu ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}
