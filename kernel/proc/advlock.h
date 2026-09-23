// proc/advlock.h - #404 Stage 6: POSIX (fcntl) + BSD (flock) advisory file
// locking, C glue over the Rust manager (rustkern/advlock.rs).
//
// The pure lock-table logic (overlap math, conflict detection, merge/split of
// same-owner ranges, deadlock-cycle check) lives in Rust. This C layer owns the
// parts entangled with the C kernel: copy_from_user of struct flock, fd->path
// identity resolution, l_whence resolution, syscall dispatch, the close/exit
// release hooks, and the wait_event_interruptible sleep for F_SETLKW.
#ifndef PROC_ADVLOCK_H
#define PROC_ADVLOCK_H

#include "../types.h"

// Longest canonical path we key a lock on. The legacy fd families store paths
// up to 260 bytes (smb_fd_t.path[260]); this must be >= that.
#define ADVLOCK_PATH_MAX 260

// The fcntl locking commands, routed here from sys_fcntl (proc/fdlayer.c). Kept
// in sync with userland libc fcntl.h.
#define ADV_F_GETLK   5
#define ADV_F_SETLK   6
#define ADV_F_SETLKW  7

// One-time boot self-test + capacity check. Prints an [ADVLOCK] line. Called
// from main.c beside fdown_boot_check().
void advlock_boot_check(void);

// fcntl(fd, {F_GETLK|F_SETLK|F_SETLKW}, struct flock *). `uptr` is the userland
// struct flock pointer (the raw fcntl arg). Returns 0 on success or -errno.
int64_t advlock_fcntl(int fd, int cmd, const void *uptr);

// flock(fd, operation): BSD whole-file lock. Returns 0 or -errno. Declared here
// and dispatched from proc/syscall.c (SYS_FLOCK).
int64_t sys_flock(int fd, int operation);

// Release hook: called from sys_close() BEFORE the legacy fd slot is torn down,
// with the USERLAND fd number. Drops this process's POSIX locks on the file the
// fd names (POSIX: any close of any fd on the file releases them) and any flock
// held on this fd slot, then wakes blocked waiters. A no-op for a non-lockable
// or not-owned fd.
void advlock_on_close(int fd_userland);

// Release hook: called from proc_exit() (group-leader exit only), with the
// exiting thread group's owner id. Drops every advisory lock it held and wakes
// waiters. cli()-safe: a bounded table sweep plus wake_up_all, no block.
void advlock_proc_exit(uint32_t owner);

#endif // PROC_ADVLOCK_H
