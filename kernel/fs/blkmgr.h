#ifndef BLKMGR_H
#define BLKMGR_H

// fs/blkmgr.h - Disk Manager kernel floor (#404 disk-mgr), Stages 0-2.
//
// The FFI surface between Ring 3 / the boot harness and rustkern/blkmgr.rs.
// Every struct here is sizeof-locked against the Rust #[repr(C)] mirror by a
// _Static_assert (below) AND against the argtab SZ_* constants in
// proc/syscall_argtab_lock.c, so a struct that grows without the validator
// noticing is a BUILD FAILURE, not a silent unchecked tail (#503 pattern).
//
// SAFETY is enforced in the kernel, never trusting the Ring-3 descriptor: the
// caller passes only (kind,index) and the kernel re-resolves the device and
// re-derives boot status itself (the sys_inst_install precedent). See the
// blkmgr.rs header for the full as-built safety model.

#include "../types.h"

// Device kinds. 0/1/2 mirror INST_KIND_* (gui/installer.h); 3 is the synthetic
// RAM-backed scratch device blkmgr.rs adds so destructive tests never touch a
// real disk.
#define BLK_KIND_ATA     0
#define BLK_KIND_AHCI    1
#define BLK_KIND_USB     2
#define BLK_KIND_SCRATCH 3

// One block device, as SYS_BLK_ENUM reports it.
typedef struct {
    uint8_t  kind;          // BLK_KIND_*
    uint8_t  index;         // per-kind index (ATA slot / AHCI port / USB idx)
    uint8_t  is_boot;       // 1 = the disk we booted from; never writable
    uint8_t  is_removable;  // 1 = removable (USB)
    uint32_t sector_size;   // always 512 here
    uint64_t sectors;       // capacity in 512-byte sectors
    char     model[40];     // model/serial label, NUL-terminated
} blk_dev_t;
_Static_assert(sizeof(blk_dev_t) == 56, "blk_dev_t must match rustkern/blkmgr.rs BlkDev");

// One proposed partition in a SYS_PART_PREPARE / SYS_PART_WRITE layout.
typedef struct {
    uint8_t            type_guid[16]; // GPT PartitionTypeGUID
    uint64_t           start_lba;     // first LBA (>= 34)
    uint64_t           size_lba;      // length in sectors
    char               name[40];      // ASCII label -> UTF-16LE in the GPT entry
} part_spec_t;
_Static_assert(sizeof(part_spec_t) == 72, "part_spec_t must match rustkern/blkmgr.rs PartSpec");

// The SYS_PART_PREPARE result the caller echoes back to SYS_PART_WRITE.
typedef struct {
    uint8_t  nonce[16];    // opaque; echo to APPLY
    uint64_t layout_hash;  // hash of the proposed layout (for the UI to show)
    uint64_t cur_hash;     // hash of the current on-disk table at prepare time
    uint64_t dev_sectors;  // capacity the kernel resolved
} part_token_t;
_Static_assert(sizeof(part_token_t) == 40, "part_token_t must match rustkern/blkmgr.rs PartToken");

// blkmgr.rs exports (also declared in kernel/rust-symbols.manifest).
int64_t blk_enum_rs(void *buf, int max, int elem_size);
int64_t sys_blk_read_rs(int kind, int index, uint64_t lba, uint32_t count, void *buf);
int64_t sys_blk_write_rs(int kind, int index, uint64_t lba, uint32_t count, const void *buf);
int64_t sys_part_prepare_rs(int kind, int index, const void *layout, int nparts, void *out_token);
int64_t sys_part_write_rs(int kind, int index, const void *layout, int nparts, const void *nonce);

// Stage 3: create a filesystem in a partition. fs_kind is MKFS_FAT / MKFS_EXT2.
#define MKFS_FAT   1
#define MKFS_EXT2  2
int64_t sys_mkfs_rs(int kind, int index, int part_index, int fstype, const void *label);

// ---------------------------------------------------------------------------
// Stage 4: general MOUNT / UNMOUNT with a mount table.
//
// SCOPE, AS BUILT (bounded first cut, honest about the g_ext2 singleton). The
// two boot mounts (FAT ESP + ext2 root) stay EXACTLY where they are: fixed C
// globals (g_fat_fs / g_ext2) that this stage does NOT touch. A general
// multi-mount VFS table over those singletons is out of scope (mounting a second
// ext2 via ext2_mount would clobber the running root, blame.md 2026-09-19).
// Instead, dynamic mounts live in a SELF-CONTAINED Rust mount table whose file
// I/O routes through blk_rw_core (the same choke point the mkfs oracle uses), so
// it works uniformly on the RAM scratch AND real devices and can never reach the
// boot singletons or the global VFS path resolver. Dynamic mount points are
// STRUCTURALLY confined to a "/MNT/" prefix, so a dynamic mount can never shadow
// "/" or "/boot" and SYS_UMOUNT can never target a boot mount.
//
// SYS_MOUNT_LIST is the cheap read-only cut (the future GUI Mount tab): it lists
// the two boot mounts PLUS every dynamic aux mount. Removable / disk-image
// volumes remain enumerated by SYS_VOL_LIST (the tray already polls it), and are
// intentionally NOT duplicated here.

// mount_ent_t flags.
#define MNT_F_BOOT     0x01  // a fixed boot mount; NEVER unmountable
#define MNT_F_RO       0x02  // mounted read-only (no write path in this cut)
#define MNT_F_DYNAMIC  0x04  // added via SYS_MOUNT (lives in the aux table)
#define MNT_F_ROOT     0x08  // the "/" root mount

// mount_ent_t fs kinds (fstype[] carries the human string; this is the numeric).
#define MNT_FS_UNKNOWN 0
#define MNT_FS_FAT16   1
#define MNT_FS_FAT32   2
#define MNT_FS_EXT2    3

// One mount, as SYS_MOUNT_LIST reports it.
typedef struct {
    char     path[32];       // mount point, NUL-terminated (e.g. "/", "/boot", "/MNT/data")
    char     fstype[8];      // "fat16" / "fat32" / "ext2", NUL-terminated
    uint8_t  kind;           // BLK_KIND_* backing device (0xFF if not a blkmgr device)
    uint8_t  index;          // backing device index
    uint8_t  part_index;     // partition index, 0xFF = whole-device / not applicable
    uint8_t  flags;          // MNT_F_*
    uint32_t _pad;
    uint64_t total_bytes;    // volume size in bytes (0 if unknown)
    uint64_t free_bytes;     // free bytes (0 if not computed in this cut)
    uint64_t start_lba;      // absolute partition start LBA
} mount_ent_t;
_Static_assert(sizeof(mount_ent_t) == 72, "mount_ent_t must match rustkern/blkmgr.rs MountEnt");

// blkmgr.rs Stage 4 exports.
int64_t sys_mount_list_rs(void *buf, int max, int elem_size);
int64_t sys_mount_rs(int kind, int index, int part_index, const void *path);
int64_t sys_umount_rs(const void *path);

// Stage 4 mount oracle (scratch only), run from the marker-gated boot harness.
int  blkmgr_mount_selftest_rs(uint32_t *out_passed, uint32_t *out_total);

// ---------------------------------------------------------------------------
// #404 Stage 4b: the C face the VFS path resolver (proc/fdlayer.c) uses to route
// open/read/write/readdir on a "/MNT/<name>/..." path through an aux mount.
// ---------------------------------------------------------------------------
// Filled by blkmgr_aux_resolve_c(). Locked against rustkern/blkmgr.rs AuxMntInfo.
typedef struct {
    uint8_t  found;      // 1 if the path is under a mounted /MNT/<name>
    uint8_t  fstype;     // MNT_FS_FAT16 / MNT_FS_FAT32 / MNT_FS_EXT2
    uint8_t  kind;       // blkmgr device kind
    uint8_t  index;      // blkmgr device index
    uint32_t rel_off;    // byte offset in the caller path where the mount-relative part starts
    uint64_t pstart;     // partition start LBA (device-absolute)
    uint64_t psize;      // partition size in sectors
} aux_mnt_info_t;
_Static_assert(sizeof(aux_mnt_info_t) == 24, "aux_mnt_info_t must match rustkern/blkmgr.rs AuxMntInfo");

// Unified KERNEL-buffer device I/O (the only path that reaches an aux volume on
// AHCI/USB/RAM scratch). Used by fs/ext2.c's aux driver. Writes run the boot-disk
// lockout. Returns sectors transferred (>0) or a negative sentinel.
int64_t blkmgr_dev_rw_c(uint8_t kind, uint8_t index, uint64_t lba, uint32_t count,
                        void *buf, int write);
// Longest-prefix resolve a KERNEL path against the aux mount table. 1 = found.
int  blkmgr_aux_resolve_c(const char *path, aux_mnt_info_t *out);
// FAT (16/32) aux root-directory file ops, by mount path + mount-relative path.
int64_t blkmgr_aux_fat_read_c(const char *mnt, const char *rel, void *buf, uint32_t cap);
int64_t blkmgr_aux_fat_write_c(const char *mnt, const char *rel, const void *buf, uint32_t len);
int  blkmgr_aux_fat_readdir_c(const char *mnt, uint32_t *pos, char *name_out,
                              int name_max, int *is_dir, uint32_t *size_out);

// Stage 0 scratch device + oracle (called only from the marker-gated boot harness).
int  blkmgr_scratch_init_rs(uint32_t sectors);
int  blkmgr_scratch_present_rs(void);
int  blkmgr_selftest_rs(uint32_t *out_passed, uint32_t *out_total);
// Stage 3 mkfs oracle (scratch only) + host-verification image dump.
int  blkmgr_mkfs_selftest_rs(uint32_t *out_passed, uint32_t *out_total);
int  blkmgr_mkfs_dump_rs(void);

#endif // BLKMGR_H
