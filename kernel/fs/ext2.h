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
    // #610 dirty detection. Captured at mount BEFORE we overwrite s_state, so
    // ext2_fsck_needed() can answer "was this volume closed cleanly last time?"
    // for the whole uptime. Before #610 the driver read none of these fields.
    uint16_t sb_state_at_mount;   // s_state as found on disk at mount
    uint16_t sb_mnt_count;        // s_mnt_count AFTER our bump
    int16_t  sb_max_mnt_count;    // s_max_mnt_count (<=0 means "never force")
    uint16_t sb_pad610;
    uint32_t sb_lastcheck;        // s_lastcheck (read + reported, never written:
    uint32_t sb_checkinterval;    // s_checkinterval  no wall clock in this kernel)
    uint32_t sb_first_ino;        // s_first_ino (11 on rev1, 11 assumed on rev0)
    uint32_t sb_reserved_gdt;     // s_reserved_gdt_blocks (resize_inode)
    uint32_t sb_sparse_super;     // 1 if RO_COMPAT_SPARSE_SUPER
    char     sb_label[17];        // s_volume_name, for the check summary line
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
// #554: thin is-directory accessor for rustkern/fsperm.rs; see ext2.c for why
// this exists instead of mirroring ext2_inode_t's layout in Rust.
int     ext2_get_is_dir(uint32_t ino, int *is_dir_out);
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

// #605: directory-entry INSERT strangler seam (the WRITE twin of the #485 scan
// above). PURE and I/O-FREE: it walks the rec_len chain inside ONE directory
// block buffer and, if a slot has enough slack, splits it and writes the new
// record. Nothing about locking, block I/O, inode/bitmap allocation or i_size
// crosses this boundary. ext2_dir_add() calls ext2_dirblock_insert(), which
// routes to the Rust port (ext2_dirblock_insert_rs, rustkern/ext2.rs) under
// -DRUST_EXT2_DIRADD, else to the C reference (ext2_dirblock_insert_c).
//   Returns  1 = inserted, `blk` mutated, caller writes the block back
//            0 = no room in this block, `blk` unmodified
//           -1 = corrupt geometry / invalid argument, `blk` unmodified
int     ext2_dirblock_insert(uint8_t *blk, uint32_t block_size,
                             const char *name, uint32_t name_len,
                             uint32_t child_ino, uint8_t ftype);
int     ext2_dirblock_insert_c(uint8_t *blk, uint32_t block_size,
                               const char *name, uint32_t name_len,
                               uint32_t child_ino, uint8_t ftype);
// Boot-time self-tests for the above. Logs one [RUST-DIFF] ext2_dirinsert line
// (byte-identical resulting blocks + identical return codes over generated and
// fuzzed blocks) and one [RUST-SEC] ext2_dirinsert line (the #597 hostile block
// whose record's name_len does not fit its own rec_len).
void    ext2_dirinsert_rust_selftest(void);

// #572 STREAMING READ (bounded RAM). Read up to `len` bytes at byte offset
// `off` from inode `ino` into `dst`, touching only the covering blocks. Returns
// bytes read (0 at EOF) or <0. `ext2_block_size()` is the mounted block size.
int64_t  ext2_read_file_range(uint32_t ino, uint64_t off, uint64_t len, void *dst);
uint32_t ext2_block_size(void);

// #572 STREAMING WRITE (bounded RAM). Append a file block by block, flushing
// each full block to disk as it fills. Final on-disk result is byte-for-byte
// identical to ext2_write_file(path, whole_buffer, total). See ext2.c.
typedef struct {
    uint32_t ino;        // target inode (created/truncated at begin)
    uint32_t next_lblk;  // next logical block to append
    uint64_t written;    // bytes committed so far (becomes i_size at finish)
    int      ok;         // 1 while the stream is healthy
} ext2_wstream_t;
int  ext2_wstream_begin(const char *path, ext2_wstream_t *ws);
int  ext2_wstream_block(ext2_wstream_t *ws, const void *data, uint32_t len);
int  ext2_wstream_finish(ext2_wstream_t *ws);
void ext2_wstream_abort(ext2_wstream_t *ws);

// #695 Phase 0. Distinct failure codes for the write API. The first three keep
// exactly the meanings they already had, so existing callers are unaffected;
// -3 NARROWS to "kernel allocation failed" now that out-of-space and media
// failure have codes of their own. Callers must treat ANY negative value as
// failure: this set may grow.
//   -4 is the code ext2_set_state() already returns when its superblock
//   blk_write() fails, so "-4 means the device write did not land" is
//   consistent across this file rather than a second convention.
// Why the split matters: the correct application response differs. NOSPC means
// free something and retry; NOMEM means retry later; IO means the medium is
// failing and retrying will not help. A merged code cannot be acted on.
#define EXT2_E_INVAL  (-1)   // not mounted, or a path that cannot be resolved
#define EXT2_E_ISDIR  (-2)   // target exists and is a directory
#define EXT2_E_NOMEM  (-3)   // kmalloc failed
#define EXT2_E_IO     (-4)   // the block device rejected a write (media error)
#define EXT2_E_NOSPC  (-5)   // filesystem full

// Write support (#99 Phase A). Absolute ext2 paths on the mounted volume.
MUST_CHECK int     ext2_write_file(const char *path, const void *data, uint32_t len);
int     ext2_mkdir(const char *path);
int     ext2_unlink(const char *path);   // delete a regular file (#99 Phase C)
// #736: remove an EMPTY directory. 0 ok, -3 not empty, -1 otherwise. Separate
// from ext2_unlink() because the two differ in what e2fsck checks afterwards:
// the parent's link count and the group's used_dirs_count.
int     ext2_rmdir(const char *path);

// #746 ATOMIC REPLACE. Rename a regular file by RELINKING its directory entry;
// no file data is read or written. Replacing an existing destination is a
// single directory-block write, so the destination name always resolves to a
// complete file - which is what makes the write-temp-then-rename safe-save
// pattern actually safe on this volume (it was not: the old emulation was
// fat_copy + fat_delete, and a copy that fails half way leaves a TRUNCATED
// destination). Returns 0 ok, -2 if either endpoint is a directory (not
// supported, reported honestly so the caller can fall back), -1 otherwise.
int     ext2_rename(const char *old_path, const char *new_path);

// #746: block-level "point this existing record at a different inode", the
// atomic step of the above. Rust under -DRUST_EXT2_DIRREPOINT, C reference
// otherwise; ext2_dirrepoint_rust_selftest() compares them at boot either way.
// 1 = repointed, 0 = name not in this block, -1 = record geometry not walkable.
int     ext2_dirblock_repoint(uint8_t *blk, uint32_t block_size,
                              const char *name, uint32_t name_len,
                              uint32_t new_ino, uint8_t ftype);
int     ext2_dirblock_repoint_c(uint8_t *blk, uint32_t block_size,
                                const char *name, uint32_t name_len,
                                uint32_t new_ino, uint8_t ftype);
void    ext2_dirrepoint_rust_selftest(void);

// #598 O_APPEND: append to the end of an existing regular file (creating it if
// absent) without reading it back into kernel RAM. Fixes fopen(path,"a")
// silently TRUNCATING the file, which it did because sys_open/sys_close routed
// every write-flagged ext2 open through ext2_write_file() (whole-file replace).
MUST_CHECK int     ext2_append_file(const char *path, const void *data, uint32_t len);

// #609: how many blocks the streaming writer appends per metadata transaction.
// 32 x 4KB = 128KB of payload per bitmap/descriptor/superblock/pointer/inode
// update, instead of one transaction per 4KB block.
#define EXT2_WRUN_BLOCKS 32u

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

// ===========================================================================
// #610 ON-DEVICE FILESYSTEM CHECKER. Report-only by design: there is no repair
// path here, because #609 is this codebase's own cautionary tale about a
// repairing fsck (`e2fsck -p` would have cloned or deleted blocks off link
// counts while the BITMAP was the trustworthy source).
//
// The three structs below are the FFI contract with rustkern/ext2fsck.rs, which
// owns all parsing and all verdicts. They are #[repr(C)] on the Rust side and
// their sizes are locked with _Static_assert in ext2.c AND re-checked at boot
// against ext2_fsck_sizeof_rs().
// ===========================================================================

typedef struct {
    uint32_t block_size;
    uint32_t blocks_count;
    uint32_t inodes_count;
    uint32_t first_data_block;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t groups_count;
    uint32_t bgd_table_block;
    uint32_t reserved_gdt_blocks;
    uint32_t first_ino;
    uint32_t sparse_super;
    uint32_t sb_free_blocks;
    uint32_t sb_free_inodes;
    uint32_t flags;
    uint32_t reserved0;
    int (*read_block)(uint32_t block, uint8_t *dst);   // the ONE capability Rust gets
    // progress(pass, current, total). Non-zero return ABORTS the scan (ESC).
    // An aborted scan reports completed = 0 and is never treated as clean.
    int (*progress)(uint32_t pass, uint32_t current, uint32_t total);
} ext2_fsck_geom_t;

typedef struct {
    uint8_t  *blk_used;      // bmap_bytes
    uint8_t  *blk_dup;       // bmap_bytes
    uint8_t  *ino_used;      // imap_bytes
    uint8_t  *ino_isdir;     // imap_bytes
    uint16_t *links;         // links_entries entries
    uint8_t  *itbuf;         // block_size
    uint8_t  *dirbuf;        // block_size
    uint8_t  *ind0;          // block_size
    uint8_t  *ind1;          // block_size
    uint8_t  *ind2;          // block_size
    uint8_t  *bmapbuf;       // block_size
    uint8_t  *gdbuf;         // block_size
    uint32_t  bmap_bytes;
    uint32_t  imap_bytes;
    uint32_t  links_entries;
    uint32_t  reserved0;
} ext2_fsck_scratch_t;

typedef struct {
    uint32_t completed;                  // 1 = the scan ran to the end
    uint32_t inodes_used;
    uint32_t blocks_used;
    uint32_t dirs_used;
    uint32_t e_bad_block_ptr;
    uint32_t e_dup_block;                // #609 signature
    uint32_t e_phantom_inode;            // #609 EXACTLY: in use, free in bitmap
    uint32_t e_leaked_inode;
    uint32_t e_block_used_bitmap_free;
    uint32_t e_block_free_bitmap_used;
    uint32_t e_bad_dirent;               // #476 / #597
    uint32_t e_orphan_inode;             // lost+found candidates
    uint32_t e_link_mismatch;
    uint32_t e_group_free_bad;
    uint32_t e_sb_free_bad;
    uint32_t e_bad_inode;
    uint32_t e_io;
    uint32_t total;
    uint8_t  first_msg[128];
} ext2_fsck_report_t;

// Run a full READ-ONLY check. 0 = the scan completed (read `out` for the
// verdict); negative = it could not run, which is NEVER "clean".
int      ext2_fsck_run(ext2_fsck_report_t *out);
// `ui` = 1 paints named per-pass progress on the boot splash and honours ESC.
// Used by the boot-time check; the Terminal path passes 0 (no splash exists).
int      ext2_fsck_run_ex(ext2_fsck_report_t *out, int ui);
// Milliseconds the last completed check took (0 if none has run).
uint32_t ext2_fsck_last_ms(void);
// Wall time spent PAINTING the progress display during that check, and how
// many repaints that was. Reported on screen so the instrumentation's cost is
// measured rather than assumed.
uint32_t ext2_fsck_last_paint_ms(void);
uint32_t ext2_fsck_last_paints(void);
uint32_t ext2_fsck_print(const ext2_fsck_report_t *r, int to_bootlog);
// Bit 0 = not cleanly unmounted, 1 = error recorded, 2 = max mount count.
uint32_t ext2_fsck_needed(void);
// Boot gate. Runs a check only if the superblock asks for one. `skip_requested`
// (the /NOFSCK ESP marker, tested by the caller) forces a skip.
void     ext2_fsck_boot_check(int skip_requested, int bench);
void     ext2_fsck_selftest(void);
// Superblock state writes (the only #610 writes to the filesystem).
void     ext2_mark_clean(void);
void     ext2_mark_error(const char *why);
uint16_t ext2_state_at_mount(void);
uint16_t ext2_mnt_count(void);

#endif // EXT2_H
