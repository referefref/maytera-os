// fdlayer.h - the contract between proc/syscall.c and proc/fdlayer.c (#746b).
//
// fdlayer.c holds the legacy kernel-wide fd table (FAT fd_table[], the ext2
// e2fd[] and the SMB/NFS smbfd[] parallel tables) and the open/close/read/
// write/seek/fcntl/fsync/readdir family that operates on them. syscall.c keeps
// the dispatcher.
//
// Only three things cross the boundary: the #572 streaming bounds (one caller,
// sys_pkg_write_stream, stayed behind), the dirent shape (the two boot
// self-tests stayed behind), and the two kernel-pointer cores that in-kernel
// callers use. sys_open_k and sys_readdir_k were `static`; they are the only
// two declarations here that are NEW text rather than moved lines, and they
// carry the same signatures they always had.
//
// Everything below the marker is byte-for-byte the code that was in
// proc/syscall.c.
#ifndef PROC_FDLAYER_H
#define PROC_FDLAYER_H

#include "../types.h"
#include "../fs/fat.h"   // #120: fd_legacy_stat_src hands back an open fat_file_t

// ---- moved verbatim from proc/syscall.c ------------------------------------
// #572 STREAMING file I/O bounds (see the ext2_fd_t comment below).
#define EXT2_RWIN_BYTES   (128u * 1024u)  // bounded per-fd read window
// #614: refill the read window in SLICES this size. ext2_read_file_range() runs
// entirely under the single global ext2_lock, so filling a whole 128 KB window
// in one call is one lock hold spanning every device round trip it needs, and
// every OTHER thread that touches a file (compositor, desktop, syslog) sits on
// g_ext2_wq for that whole span. That is why a large sequential read read like
// a desktop freeze rather than merely a slow installer. Slicing hands the lock
// back between slices. Same bytes, same window, just more release points.
#define EXT2_RSLICE_BYTES (32u * 1024u)
#define EXT2_WSPILL_BYTES (1024u * 1024u) // buffer small writes; stream past this
// File-scope dirent shape shared by the sys_readdir wrapper and its kernel core
// (was a local typedef in sys_readdir()). Layout is the userland dirent the
// #503 argtab sizeof-locks to 264 bytes.
typedef struct {
    char name[256];
    uint32_t type;    // 0 = file, 1 = directory
    uint32_t size;
} sc_dirent_t;
_Static_assert(sizeof(sc_dirent_t) == 264, "#503 argtab: SZ_DIRENT in rustkern.rs is stale");
// ---- NEW declarations (not moved): the two kernel-pointer cores. Both were
// `static int64_t` in syscall.c; sys_chdir and the two boot self-tests call
// them, and those callers stayed behind.
int64_t sys_open_k(const char *path, int flags);
// #745 (local 82). 1 when `fd` names an open slot in one of the legacy
// kernel-wide tables (FAT fd_used[], ext2 e2fd[], SMB/NFS smbfd[]). Those
// fds are not file_t's, so fd_get() returns NULL for them and poll(2) would
// otherwise report POLLNVAL for a perfectly good open file. All three kinds
// are regular files, which POSIX says are always ready.
int fd_legacy_is_open(int fd);

// ---------------------------------------------------------------------------
// #120: WHAT SYS_FSTAT NEEDS FROM A LEGACY fd, and why it needed anything.
//
// MEASURED, not predicted. sys_fstat was written against the per-process VFS
// table and verified on a booted VM: pipes and /dev/null reported correctly and
// EVERY regular file returned -9 EBADF. Regular files do not live in
// proc->fds[]; they live in the three system-wide tables in fdlayer.c, so
// fd_get() returns NULL for all of them. fd_legacy_is_open() above exists for
// EXACTLY this reason - #745 hit the same wall wiring up poll(2) - and its
// comment says so. Reading that comment was cheaper than the boot that found it.
//
// TWO SHAPES, because the two families can answer different questions:
//
//   FDL_STAT_FAT   the FAT table holds an OPEN fat_file_t and there is no path
//                  recorded anywhere. The handle is handed back so the caller
//                  fills from it DIRECTLY, using the same code sys_stat_path
//                  uses after its own fat_open(). That is why this returns a
//                  handle rather than a path: re-deriving a path to re-open a
//                  file we already have open would be both a second directory
//                  read and a second copy of the fill.
//   FDL_STAT_PATH  ext2, SMB and NFS all record their path, so the caller
//                  stats that path through the one shared fill.
//
// `live_size_out` is the size the DESCRIPTION knows and the medium does not, or
// -1 for "nothing more current than the medium". It is set only where a write
// is buffered, and the expressions are MIRRORED FROM sys_seek() in fdlayer.c so
// that fstat and lseek(SEEK_END) can never disagree about one fd.
// ---------------------------------------------------------------------------
#define FDL_STAT_NONE 0
#define FDL_STAT_FAT  1
#define FDL_STAT_PATH 2
int fd_legacy_stat_src(int fd, const fat_file_t **fat_out,
                       char *path_out, uint32_t cap, int64_t *live_size_out);
int64_t sys_readdir_k(int fd, sc_dirent_t *de);

#endif /* PROC_FDLAYER_H */
