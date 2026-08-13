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
int64_t sys_readdir_k(int fd, sc_dirent_t *de);

#endif /* PROC_FDLAYER_H */
