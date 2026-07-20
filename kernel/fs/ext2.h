// ext2.h - Minimal READ-ONLY ext2 filesystem driver for MayteraOS
#ifndef EXT2_H
#define EXT2_H

#include "../types.h"

// Parsed ext2 inode (only the fields we use).
typedef struct {
    uint16_t i_mode;        // file type + permissions
    uint32_t i_size;        // size in bytes (low 32 bits)
    uint32_t i_size_high;   // high 32 bits (i_dir_acl) for large_file
    uint32_t i_block[15];   // 12 direct, 1 singly, 1 doubly, 1 triply
} ext2_inode_t;

// Mounted ext2 filesystem state.
typedef struct {
    int      mounted;
    uint8_t  channel;
    uint8_t  drive;
    uint32_t block_size;          // bytes per block (1024 for this fs)
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t first_data_block;    // 1 for 1KB blocks
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;          // 256 for this fs
    uint32_t groups_count;        // number of block groups
    uint32_t bgd_table_block;     // block holding the group descriptor table
    uint32_t part_start_lba;      // #365: base LBA of the ext2 partition on its
                                  // device (0 for a whole-disk ext2 volume). Added
                                  // to every absolute block access so ext2 can live
                                  // in partition 2 of a GPT disk behind a FAT ESP.
} ext2_fs_t;

int     ext2_mount(uint8_t channel, uint8_t drive, uint32_t part_start_lba);
// #365: locate an ext2/Linux partition on a disk and return its starting LBA.
// Parses GPT (preferred) then MBR. Returns 0 and sets *out_base_lba on success.
int     ext2_find_partition(uint8_t channel, uint8_t drive, uint32_t *out_base_lba);
int     ext2_read_inode(uint32_t ino, ext2_inode_t *out);
int64_t ext2_read_file_ino(uint32_t ino, void *buf, uint64_t max);
int     ext2_lookup(uint32_t dir_ino, const char *name,
                    uint32_t *out_ino, uint8_t *out_type);
uint32_t ext2_resolve_path(const char *path);
void    ext2_selftest(void);

// #404 / #485 Phase C: directory-block entry scan strangler seam. ext2_lookup()
// calls ext2_dirblock_find() per block; it routes to the Rust port
// (ext2_dirblock_find_rs, rustkern.rs) under -DRUST_EXT2_DIRFIND, else to the
// #476-hardened C (ext2_dirblock_find_c). Returns 1 (found, fills *out_ino /
// *out_type) or 0. ci != 0 => case-insensitive compare (g_root_ext2).
int     ext2_dirblock_find(const uint8_t *blk, uint32_t block_size,
                           const char *name, uint32_t name_len,
                           int ci, uint32_t *out_ino, uint8_t *out_type);
int     ext2_dirblock_find_c(const uint8_t *blk, uint32_t block_size,
                             const char *name, uint32_t name_len,
                             int ci, uint32_t *out_ino, uint8_t *out_type);
// Boot-time differential self-test: ext2_dirblock_find_rs == ext2_dirblock_find_c
// over valid + malformed blocks. Logs one [RUST-DIFF] ext2_dir line.
void    ext2_dir_rust_selftest(void);

// Write support (#99 Phase A). Absolute ext2 paths on the mounted volume.
int     ext2_write_file(const char *path, const void *data, uint32_t len);
int     ext2_mkdir(const char *path);
int     ext2_unlink(const char *path);   // delete a regular file (#99 Phase C)

// Root-cutover (#99 Phase C): when g_root_ext2 != 0 the kernel uses ext2 as the
// root filesystem (FAT stays as the UEFI ESP). ext2_read_whole() loads an entire
// regular file into a kmalloc'd buffer (NULL if absent/dir/unmounted).
extern int g_root_ext2;
void   *ext2_read_whole(const char *path, uint32_t *size_out);
int     ext2_is_mounted(void);
// #539: absolute device byte offset just past the end of the mounted ext2
// volume (part_start_lba*512 + blocks_count*block_size); 0 if not mounted.
uint64_t ext2_end_bytes(void);
int     ext2_readdir_ino(uint32_t dir_ino, uint32_t *pos, char *name_out, int name_max, uint32_t *ino_out, uint8_t *type_out);

#endif // EXT2_H
