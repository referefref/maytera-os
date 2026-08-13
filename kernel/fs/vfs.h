// vfs.h - Virtual file system layer (Phase A1)
//
// Introduces a struct file with a function-ops vtable and a per-process file
// descriptor table. Every kind of open object in the kernel (regular FAT file,
// /dev/console, pipe, pty master/slave, socket) plugs in the same way: it
// provides a struct file_ops, stores its own state in file->priv, and callers
// use fd-indexed routines that dispatch through file->ops.
//
// Phase A1 only provides a FAT adapter (fs/fat_vfs.c). Later phases add
// /dev/console (A2), dup/dup2/fcntl (A3), pipes (E), PTYs (F), ioctl (C2),
// sockets (G? or whenever).

#ifndef FS_VFS_H
#define FS_VFS_H

#include "../types.h"

struct file;

// File operations vtable. Implementations that are truly non-blocking should
// check file->flags & O_NONBLOCK.
//
// #695: this comment used to say close() "must never sleep". That has been
// FALSE since the ext2 adapter landed: e2v_release() calls ext2_write_file(),
// which takes ext2_lock(), which is a wait_event() ticket lock and therefore
// sleeps under contention. A rule the code already broke is worse than no rule,
// because the next reader designs against it. The rule that is actually true,
// and the reason sock_file_release() still runs cli + CR3-switched and
// non-blocking, is narrower and is about the CALLER, not about close():
//
//     a release() reached from a context that cannot block must not block.
//
// proc_exit()'s fd_close_all() is exactly such a context (it runs under cli()),
// so any file kind that can still be open at process exit owes that path a
// non-sleeping teardown. sync/noblock.c's wq_assert_may_block() is what reports
// a violation ([WQBLOCK] on serial with a caller address).
// #745 (local 82): poll_wq below returns one of these. Forward-declared
// rather than including sync/waitq.h, which includes proc headers and
// would drag a cycle into every filesystem that includes vfs.h.
struct wait_queue_head;

typedef struct file_ops {
    int64_t (*read)(struct file *f, void *buf, size_t count);
    int64_t (*write)(struct file *f, const void *buf, size_t count);
    int64_t (*seek)(struct file *f, int64_t offset, int whence);
    int     (*ioctl)(struct file *f, unsigned cmd, void *arg);
    // #695 Phase 1. OPTIONAL. Commit buffered state to the medium WITHOUT
    // releasing the description, so the caller can check the result and still
    // act on it:
    //     write -> fsync -> check -> ONLY THEN clear the dirty flag
    // close() cannot support that pattern. POSIX consumes the fd whether or not
    // close() reports an error, so a caller that learns from close() has no
    // handle left: it cannot retry, cannot re-flush, and must not close() again
    // (a second close can close another thread's fd). That is why fsync, not the
    // close() signature, is the half of #695 that protects data.
    // Returns 0 only if every byte is on the medium. MUST be idempotent: after a
    // 0 return the description is clean, so a second flush and the eventual
    // release are both no-ops returning 0 and NEVER write the same bytes twice.
    // NULL means "this file kind buffers nothing"; file_flush() reports 0.
    int     (*flush)(struct file *f);
    // Called exactly once, when refcount reaches zero. Must free any
    // ops-private state reachable via f->priv. After return, the caller
    // kfree()s the struct file itself.
    // #695 Phase 2: returns 0, or negative if the final flush failed. The two
    // filesystem adapters define release() AS flush() + teardown, so there is
    // exactly ONE flush implementation per filesystem and a close and an fsync
    // cannot drift apart. Teardown ALWAYS completes: a failed flush must never
    // skip a free, or an unwritable disk becomes a memory leak.
    int     (*release)(struct file *f);
    // Optional: returns a bitmask of ready POLL_* events (POLL_IN/OUT).
    int     (*poll)(struct file *f, int events);
    // Optional (#745 local 82, added for poll(2)). The wait queue that is
    // woken when this file's readiness for `events` may have changed.
    // poll(2) parks on it for the WHOLE timeout when every polled fd
    // publishes the same queue, which is the difference between an exact
    // wake and a 20ms re-scan. NULL means "this file kind publishes no
    // queue", which is not an error: poll() then bounds its sleep.
    struct wait_queue_head *(*poll_wq)(struct file *f, int events);
} file_ops_t;

// POSIX-ish open flags. Match the values the existing userland uses.
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_ACCMODE   0x0003
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_CLOEXEC   0x80000

// ---------------------------------------------------------------------------
// #746: ONE decoding of the flags word above, for every fd family in the
// kernel.
//
// WHY THIS EXISTS. The flags were decoded ad hoc wherever an fd is created, and
// the two shipping filesystems ended up disagreeing about the SAME bits. The
// ext2 branch of sys_open_k() tested `flags & 0x3` and treated anything that
// was not exactly O_RDONLY as "replace this file", so O_RDWR TRUNCATED the file
// it had just opened and then returned an fd that could not read: the POSIX
// read-modify-write idiom destroyed the data it opened to modify, and the open
// SUCCEEDED, so nothing reported it. The legacy FAT branch went the other way
// and ignored the flags word entirely, so O_TRUNC left a stale tail and
// O_APPEND overwrote from byte 0.
//
// Neither is a caller bug and neither is visible at the call site. A shared
// decoder is the only form of the fix that cannot drift apart again: a new fd
// family ASKS these questions instead of re-deriving the answers from bit
// arithmetic that only its author has read.
// ---------------------------------------------------------------------------
typedef struct {
    int can_read;   // the fd must be able to serve read()
    int can_write;  // the fd must be able to serve write()
    int rdwr;       // BOTH: needs one buffer and ONE shared file position
    int trunc;      // O_TRUNC: emptied AT OPEN, and only once the open succeeded
    int append;     // O_APPEND: every write lands at end-of-file
    int create;     // O_CREAT
} open_mode_t;

static inline open_mode_t open_mode_decode(int flags) {
    open_mode_t m;
    int acc = flags & O_ACCMODE;
    // O_ACCMODE == 3 is not a legal access mode in POSIX. It is decoded as
    // read-write rather than rejected: rejecting it here would turn a caller's
    // typo into a brand new failure mode, and read-write is the only decoding
    // of it that cannot LOSE data.
    m.rdwr      = (acc >= O_RDWR);
    m.can_read  = (acc == O_RDONLY) || m.rdwr;
    m.can_write = (acc == O_WRONLY) || m.rdwr;
    m.trunc     = (flags & O_TRUNC)  != 0;
    m.append    = (flags & O_APPEND) != 0;
    m.create    = (flags & O_CREAT)  != 0;
    // POSIX leaves O_TRUNC on a read-only fd undefined. This kernel has always
    // treated it as a request to empty the file and the /CONFIG writers depend
    // on that, so it keeps meaning "write" - but it does NOT take away the read.
    if (m.trunc) m.can_write = 1;
    // O_TRUNC and O_APPEND together: POSIX truncates at open and then appends,
    // which on a now-empty file is plain sequential writing. Said once, here,
    // instead of once per fd family (it was only ever said in the ext2 one).
    if (m.trunc) m.append = 0;
    return m;
}

// POSIX-ish seek whences.
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

// POSIX-ish poll events.
#define POLL_IN     0x01
#define POLL_OUT    0x04
#define POLL_ERR    0x08
#define POLL_HUP    0x10

// POSIX-ish fcntl commands (Phase A3).
#define F_DUPFD     0   // Duplicate fd (arg = min fd)
#define F_GETFD     1   // Get FD flags (returns FD_CLOEXEC bit)
#define F_SETFD     2   // Set FD flags
#define F_GETFL     3   // Get file status flags (returns file->flags)
#define F_SETFL     4   // Set file status flags (only O_NONBLOCK / O_APPEND mutable)
#define F_DUPFD_CLOEXEC 1030 // Duplicate fd and set FD_CLOEXEC on the new fd

// FD flags (bit set in process::fd_cloexec for fd i if F_CLOEXEC is on).
#define FD_CLOEXEC  1

// An open file description. Refcounted because a single description can be
// referenced by multiple fds (across dup/fork). Once refcount hits zero the
// release op runs and the struct itself is freed.
// #487/#349: max bytes (including the NUL) of the recorded open path. Sized to
// name real paths without bloating file_t, which is kmalloc'd per open. Paths
// longer than this are truncated, never over-run.
#define VFS_FPATH_MAX 128

typedef struct file {
    const file_ops_t *ops;
    void            *priv;
    int              flags;         // O_RDONLY / O_WRONLY / O_RDWR / O_APPEND / O_NONBLOCK
    int              refcount;
    // #487/#349: the path (or device name) this description was opened with, so
    // Task Manager / Process Explorer can NAME a process's open handles instead
    // of rendering a bare "fd 5". Recorded at open time by the open choke points
    // (fat_vfs_open / ext2_vfs_open) and the device openers. Empty string means
    // "not recorded" (e.g. a pipe or an anonymous description), never garbage:
    // file_alloc() always terminates it.
    char             path[VFS_FPATH_MAX];
} file_t;

// #487: record `path` on `f`, bounded and always NUL-terminated. Safe to call
// with a NULL f or path. `src_max` bounds the scan of `path` so a source that is
// not NUL-terminated cannot drive an unbounded read.
void file_set_path(file_t *f, const char *path);

// Rust port (rustkern.rs) + C reference twin of the bounded store. Return the
// bytes stored, excluding the NUL.
uint32_t vfs_path_store_rs(char *dst, uint32_t cap, const char *src, uint32_t src_max);
uint32_t vfs_path_store_c(char *dst, uint32_t cap, const char *src, uint32_t src_max);

#ifdef RUST_VFS_PATH
#define vfs_path_store(d, c, s, m) vfs_path_store_rs((d), (c), (s), (m))
#else
#define vfs_path_store(d, c, s, m) vfs_path_store_c((d), (c), (s), (m))
#endif

// #404 boot-time [RUST-DIFF] differential for the vfs_path seam.
void vfs_path_selftest(void);

// MAX_FDS is defined in proc/process.h (since the fd array lives in
// process_t). Include it only where the fd helpers are used.

// ---- struct file lifecycle ----

// Allocate a new file with refcount=1. Caller fills in ops+priv+flags.
file_t *file_alloc(const file_ops_t *ops, void *priv, int flags);

// Refcount management. file_put may call ops->release() and free.
//
// #695 Phase 2: file_put returns ops->release()'s status (0 when this was not
// the last reference), so the owner of the last reference learns that the final
// flush failed. Callers on TEARDOWN paths - proc_exit's fd_close_all, execve's
// fd_close_cloexec, dup2/fd_install slot eviction, pipe_create's error unwind -
// have NO recipient for that error and must LOG AND CONTINUE, never propagate.
// Wiring one of those into a fault turns a full disk into a fault on every
// process exit.
void file_get(file_t *f);
MUST_CHECK int  file_put(file_t *f);

// #695 Phase 1: commit f's buffered state to the medium without releasing it.
// 0 means and only means "every byte is on the medium". On non-zero the
// destination may be EMPTY or ABSENT and is NEVER the previous contents: see
// the full contract on sys_fsync() in proc/syscall.c.
MUST_CHECK int  file_flush(file_t *f);

// Call ops->read/write/seek/ioctl/poll. NULL ops return -1.
int64_t file_read(file_t *f, void *buf, size_t count);
MUST_CHECK int64_t file_write(file_t *f, const void *buf, size_t count);
int64_t file_seek(file_t *f, int64_t offset, int whence);
int     file_ioctl(file_t *f, unsigned cmd, void *arg);
int     file_poll(file_t *f, int events);
// #745 (local 82). 1 when this file kind implements ops->poll at all.
// file_poll() returns 0 both for "asked, nothing ready" and for "this kind
// never answers", and poll(2) must tell them apart: the second case is a
// regular file, which POSIX says is ALWAYS ready, and treating it as never
// ready would block every poll() on a file for its full timeout.
int     file_has_poll(file_t *f);
// #745 (local 82). ops->poll_wq, or NULL. See the op comment above.
struct wait_queue_head *file_poll_wq(file_t *f, int events);

// ---- per-process fd table (operates on current_proc->fds[]) ----

// Find the lowest free fd >= min, or -1 if the table is full.
int fd_alloc(int min);

// Put f in current_proc->fds[fd]. Returns 0 on success, -1 on bad fd.
// Does NOT bump refcount; the allocator is responsible.
int fd_install(int fd, file_t *f);

// Get file for fd or NULL. Does NOT bump refcount. Caller must not drop the
// file after the syscall returns; use file_get if you need to keep it.
file_t *fd_get(int fd);

// Atomic "allocate lowest free fd and install f into it". On success returns
// fd, on failure returns -1 and the caller still owns f's refcount.
int fd_alloc_install(file_t *f);

// Close fd: remove from fd table, drop the refcount. Returns 0 on success,
// -1 if fd was already empty.
int fd_close(int fd);

// Close every fd in the current process's table. Used from proc_exit.
void fd_close_all(void);

// Bump the refcount on every open fd in the fd table. Used from proc_fork
// so the child inherits each parent file description with a fresh reference.
// (A3 will add CLOEXEC handling and move this into proc_copy_files.)
void fd_refcount_all_plus_plus(void);

// Duplicate oldfd to the lowest available fd >= min. Returns the new fd or
// -1 on error. Bumps the underlying file_t refcount.
int fd_dup(int oldfd, int min);

// Duplicate oldfd onto newfd. If newfd is already open, it is closed first
// (per dup2(3) semantics). If oldfd == newfd, returns newfd without doing
// anything. Bumps the refcount. Returns newfd on success, -1 on error.
int fd_dup2(int oldfd, int newfd);

// Close every fd whose FD_CLOEXEC bit is set. Called from execve() between
// mm-swap and SYSRET. Safe to call from syscall context. No-op if no fds
// have the bit set.
void fd_close_cloexec(void);

#endif // FS_VFS_H

// Pipe support
int pipe_create(int pipefd[2]);
