// fs/blkmgr_mnt.c - Disk Manager Stage 4 (#404 disk-mgr): thin C shims the Rust
// mount table needs, and nothing more.
//
// WHY C HERE (the CLAUDE.md "new kernel code is Rust unless stated" rule):
//   1. The two BOOT mounts live in C globals (g_fat_fs is a fat_fs_t, g_ext2 is
//      an ext2_fs_t). Reading their fields is a handful of thin scalar accessors
//      over EXISTING C structs, exactly the "one-line euid accessor" precedent
//      blkmgr.rs already cites. Mirroring those struct layouts into Rust #[repr(C)]
//      would be fragile duplication for zero benefit; the accessors are the small,
//      stable FFI surface instead.
//   2. The aux mount table is shared mutable state under SMP and needs a real
//      spinlock. spinlock_t is a C type; hand-rolling the acquire loop in Rust
//      would be a NEW busy-wait that the concurrency-lint correctly forbids. So
//      the lock and its acquire/release stay in C (a zeroed spinlock is valid,
//      SPINLOCK_INIT = {.locked = 0}), and Rust calls these two void functions.
//
// None of this touches or reinvents the boot mounts; it only READS them.

#include "../types.h"
#include "fat.h"
#include "ext2.h"
#include "../sync/spinlock.h"

// --- boot mount: FAT ESP (g_fat_fs) ----------------------------------------
extern fat_fs_t g_fat_fs;
extern uint32_t fat_get_free_clusters(fat_fs_t *fs);

int blkmgr_esp_mounted_c(void) {
    return g_fat_fs.mounted ? 1 : 0;
}
uint32_t blkmgr_esp_part_lba_c(void) {
    return g_fat_fs.part_start_lba;
}
uint64_t blkmgr_esp_total_bytes_c(void) {
    return (uint64_t)g_fat_fs.cluster_count
         * g_fat_fs.sectors_per_cluster
         * g_fat_fs.bytes_per_sector;
}
uint64_t blkmgr_esp_free_bytes_c(void) {
    uint32_t fc = fat_get_free_clusters(&g_fat_fs);
    return (uint64_t)fc
         * g_fat_fs.sectors_per_cluster
         * g_fat_fs.bytes_per_sector;
}

// --- boot mount: ext2 root -------------------------------------------------
// g_ext2 is static in ext2.c, so read it through ext2.c's own thin accessors.
extern int      g_root_ext2;       // fs/ext2.h (root cutover flag; a real global)
extern uint64_t ext2_volume_bytes(void); // fs/ext2.c (#404 Stage 4)
extern uint32_t ext2_part_lba(void);     // fs/ext2.c (#404 Stage 4)

int blkmgr_root_ext2_mounted_c(void) {
    return ext2_is_mounted();
}
int blkmgr_root_is_ext2_c(void) {
    return g_root_ext2 ? 1 : 0;
}
uint32_t blkmgr_root_part_lba_c(void) {
    return ext2_part_lba();
}
uint64_t blkmgr_root_total_bytes_c(void) {
    return ext2_volume_bytes();
}

// --- aux mount table lock ---------------------------------------------------
// A zeroed spinlock is a valid unlocked spinlock (SPINLOCK_INIT = {.locked=0}),
// so no init is needed. Mount / unmount / list are rare, short, root-only
// syscall-context operations, so this lock is essentially uncontended; it exists
// purely for SMP correctness of the Rust static-mut table access.
static spinlock_t g_blkmgr_mnt_lock;

void blkmgr_mnt_lock_c(void)   { spinlock_acquire(&g_blkmgr_mnt_lock); }
void blkmgr_mnt_unlock_c(void) { spinlock_release(&g_blkmgr_mnt_lock); }
