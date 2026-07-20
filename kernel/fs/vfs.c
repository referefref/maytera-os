// vfs.c - Virtual file system layer implementation (Phase A1)
//
// See vfs.h for the overall picture. This translation unit owns:
//   - file_alloc / file_get / file_put refcounting
//   - the dispatcher helpers (file_read/write/seek/ioctl/poll)
//   - fd_alloc / fd_install / fd_get / fd_close against current_proc->fds[]
//
// All fd-table operations implicitly work on the current process. The fd
// table itself lives in process_t (see proc/process.h).

#include "vfs.h"
#include "../proc/process.h"
#include "../mm/heap.h"
#include "../serial.h"

// ============================================================================
// struct file lifecycle
// ============================================================================

file_t *file_alloc(const file_ops_t *ops, void *priv, int flags) {
    file_t *f = (file_t *)kmalloc(sizeof(file_t));
    if (!f) return NULL;
    f->ops      = ops;
    f->priv     = priv;
    f->flags    = flags;
    f->refcount = 1;
    // #487: kmalloc does not zero. Terminate the path so an un-recorded
    // description reads as "" rather than exposing stale heap bytes to the
    // Task Manager (an information leak, and a non-terminated read).
    f->path[0]  = '\0';
    return f;
}

// #487/#349: bounded, always-terminated path record. The live store is the Rust
// seam under -DRUST_VFS_PATH; vfs_path_store_c is the reference twin + rollback.
uint32_t vfs_path_store_c(char *dst, uint32_t cap, const char *src, uint32_t src_max) {
    if (!dst || cap == 0) return 0;
    if (!src || src_max == 0) { dst[0] = '\0'; return 0; }
    uint32_t room = cap - 1;          // reserve the terminator: the bug class
    uint32_t n = 0;
    while (n < room && n < src_max && src[n]) { dst[n] = src[n]; n++; }
    dst[n] = '\0';
    return n;
}

void file_set_path(file_t *f, const char *path) {
    if (!f) return;
    // VFS_FPATH_MAX bounds the source scan too: `path` is not trusted to be
    // terminated within any particular distance.
    vfs_path_store(f->path, VFS_FPATH_MAX, path, VFS_FPATH_MAX);
}

void file_get(file_t *f) {
    if (!f) return;
    f->refcount++;
}

void file_put(file_t *f) {
    if (!f) return;
    f->refcount--;
    if (f->refcount <= 0) {
        if (f->ops && f->ops->release) f->ops->release(f);
        kfree(f);
    }
}

// ============================================================================
// Dispatcher helpers
// ============================================================================

int64_t file_read(file_t *f, void *buf, size_t count) {
    if (!f || !f->ops || !f->ops->read) return -1;
    // Reject reads on write-only files.
    int acc = f->flags & O_ACCMODE;
    if (acc == O_WRONLY) return -1;
    return f->ops->read(f, buf, count);
}

int64_t file_write(file_t *f, const void *buf, size_t count) {
    if (!f || !f->ops || !f->ops->write) return -1;
    // Reject writes on read-only files.
    int acc = f->flags & O_ACCMODE;
    if (acc == O_RDONLY) return -1;
    return f->ops->write(f, buf, count);
}

int64_t file_seek(file_t *f, int64_t offset, int whence) {
    if (!f || !f->ops || !f->ops->seek) return -1;
    return f->ops->seek(f, offset, whence);
}

int file_ioctl(file_t *f, unsigned cmd, void *arg) {
    if (!f || !f->ops || !f->ops->ioctl) return -1;
    return f->ops->ioctl(f, cmd, arg);
}

int file_poll(file_t *f, int events) {
    if (!f || !f->ops || !f->ops->poll) return 0;
    return f->ops->poll(f, events);
}

// ============================================================================
// Per-process fd table
// ============================================================================
//
// NOTE: Access to current_proc->fds[] is not protected by a lock. This is
// acceptable today because the kernel is single-CPU and fd-table mutations
// happen either in-syscall (current process cannot be preempted out of a
// syscall into another syscall on itself) or during fork/exit (where the
// target is not running). When SMP lands, this table needs a per-process
// spinlock.

int fd_alloc(int min) {
    process_t *p = proc_current();
    if (!p) return -1;
    if (min < 0) min = 0;
    if (min >= MAX_FDS) return -1;
    for (int i = min; i < MAX_FDS; i++) {
        if (p->fds[i] == NULL) return i;
    }
    return -1;
}

int fd_install(int fd, file_t *f) {
    process_t *p = proc_current();
    if (!p) return -1;
    if (fd < 0 || fd >= MAX_FDS) return -1;
    // If the slot is already occupied, close the old one first. This matches
    // dup2 semantics; ordinary callers should target an empty slot.
    if (p->fds[fd]) {
        file_put(p->fds[fd]);
        p->fds[fd] = NULL;
    }
    p->fds[fd] = f;
    return 0;
}

file_t *fd_get(int fd) {
    process_t *p = proc_current();
    if (!p) return NULL;
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    return p->fds[fd];
}

int fd_alloc_install(file_t *f) {
    // Start at 3 to preserve the traditional 0/1/2 reservation until Phase A2
    // pre-opens /dev/console on them.
    int fd = fd_alloc(3);
    if (fd < 0) return -1;
    process_t *p = proc_current();
    p->fds[fd] = f;
    return fd;
}

int fd_close(int fd) {
    process_t *p = proc_current();
    if (!p) return -1;
    if (fd < 0 || fd >= MAX_FDS) return -1;
    file_t *f = p->fds[fd];
    if (!f) return -1;
    p->fds[fd] = NULL;
    // Clear CLOEXEC bit for this fd.
    p->fd_cloexec &= ~(1ULL << fd);
    file_put(f);
    return 0;
}

void fd_close_all(void) {
    process_t *p = proc_current();
    if (!p) return;
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fds[i]) {
            file_put(p->fds[i]);
            p->fds[i] = NULL;
        }
    }
    p->fd_cloexec = 0;
}

void fd_refcount_all_plus_plus(void) {
    process_t *p = proc_current();
    if (!p) return;
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fds[i]) file_get(p->fds[i]);
    }
}

// ============================================================================
// Phase A3: dup / dup2 / CLOEXEC
// ============================================================================

int fd_dup(int oldfd, int min) {
    process_t *p = proc_current();
    if (!p) return -1;
    if (oldfd < 0 || oldfd >= MAX_FDS) return -1;
    file_t *f = p->fds[oldfd];
    if (!f) return -1;
    int newfd = fd_alloc(min < 0 ? 0 : min);
    if (newfd < 0) return -1;
    file_get(f);
    p->fds[newfd] = f;
    // dup() never inherits CLOEXEC; F_DUPFD_CLOEXEC sets it at the syscall
    // layer after this call returns.
    p->fd_cloexec &= ~(1ULL << newfd);
    return newfd;
}

int fd_dup2(int oldfd, int newfd) {
    process_t *p = proc_current();
    if (!p) return -1;
    if (oldfd < 0 || oldfd >= MAX_FDS) return -1;
    if (newfd < 0 || newfd >= MAX_FDS) return -1;
    file_t *f = p->fds[oldfd];
    if (!f) return -1;
    // POSIX: dup2 with oldfd == newfd is a no-op that returns newfd.
    if (oldfd == newfd) return newfd;
    if (p->fds[newfd]) {
        file_put(p->fds[newfd]);
        p->fds[newfd] = NULL;
    }
    file_get(f);
    p->fds[newfd] = f;
    p->fd_cloexec &= ~(1ULL << newfd);
    return newfd;
}

void fd_close_cloexec(void) {
    process_t *p = proc_current();
    if (!p || p->fd_cloexec == 0) return;
    for (int i = 0; i < MAX_FDS; i++) {
        if ((p->fd_cloexec & (1ULL << i)) && p->fds[i]) {
            file_put(p->fds[i]);
            p->fds[i] = NULL;
        }
    }
    p->fd_cloexec = 0;
}

// ---------------------------------------------------------------------------
// #487/#349 boot-time [RUST-DIFF] differential for the vfs_path_store seam.
//
// Corpus design: the states a naive C copy gets wrong, each reached
// deliberately and counted (blame.md: report coverage, not just vector counts):
//   - src longer than cap        (truncation; strcpy would overflow)
//   - src exactly cap-1          (exact fit, terminator lands on the last byte)
//   - src exactly cap            (off-by-one boundary; strncpy drops the NUL)
//   - src NOT NUL-terminated     (unbounded read; bounded by src_max here)
//   - cap == 1                   (room for the terminator only)
//   - cap == 0 / NULL src / NULL dst (contract edges)
// A canary byte after each destination proves neither implementation writes
// past `cap`, so the test measures memory safety, not just equal output.
// ---------------------------------------------------------------------------
static uint32_t vp_rng = 0xC0FFEEu;
static uint32_t vp_rand(void) {
    vp_rng ^= vp_rng << 13; vp_rng ^= vp_rng >> 17; vp_rng ^= vp_rng << 5;
    return vp_rng;
}

void vfs_path_selftest(void) {
    int mism = 0, vecs = 0, canary = 0;
    int cov_trunc = 0, cov_exact = 0, cov_unterm = 0, cov_cap1 = 0;

    for (int iter = 0; iter < 800; iter++) {
        char src[160];
        uint32_t slen = vp_rand() % 150;
        for (uint32_t i = 0; i < slen; i++) src[i] = (char)('a' + (vp_rand() % 26));
        int unterm = (iter % 9 == 0);
        if (unterm) { for (uint32_t i = slen; i < sizeof(src); i++) src[i] = 'X'; cov_unterm++; }
        else src[slen] = '\0';

        uint32_t cap = 1 + (vp_rand() % 130);
        if (iter % 7 == 0) cap = 1;                       // terminator-only
        if (cap == 1) cov_cap1++;
        if (slen >= cap) cov_trunc++;
        if (slen == cap - 1) cov_exact++;

        // 8-byte canary after each buffer; both must leave it untouched.
        char dc[160 + 8], dr[160 + 8];
        for (int i = 0; i < 160 + 8; i++) { dc[i] = 0x7E; dr[i] = 0x7E; }
        uint32_t src_max = unterm ? (uint32_t)sizeof(src) : slen + 1;

        uint32_t nc = vfs_path_store_c(dc, cap, src, src_max);
        uint32_t nr = vfs_path_store_rs(dr, cap, src, src_max);
        vecs++;
        if (nc != nr) mism++;
        else {
            for (uint32_t i = 0; i < cap; i++) {
                if (dc[i] != dr[i]) { mism++; break; }
            }
        }
        for (uint32_t i = cap; i < cap + 8; i++) {
            if (dc[i] != 0x7E || dr[i] != 0x7E) { canary++; break; }
        }
    }

    // Contract edges.
    { char d[8]; d[0] = 'z';
      if (vfs_path_store_c(d, 0, "x", 2) != 0 || vfs_path_store_rs(d, 0, "x", 2) != 0) mism++;
      vecs++; }
    { char dc[8], dr[8];
      if (vfs_path_store_c(dc, 8, 0, 4) != 0 || vfs_path_store_rs(dr, 8, 0, 4) != 0) mism++;
      if (dc[0] != '\0' || dr[0] != '\0') mism++;   // NULL src must still terminate
      vecs++; }
    if (vfs_path_store_c(0, 8, "x", 2) != 0 || vfs_path_store_rs(0, 8, "x", 2) != 0) mism++;
    vecs++;

    kprintf("[RUST-DIFF] vfs_path: %d vecs mism=%d canary=%d %s (LIVE=%s)\n",
            vecs, mism, canary, (mism || canary) ? "MISMATCH" : "MATCH",
#ifdef RUST_VFS_PATH
            "rust"
#else
            "c"
#endif
    );
    kprintf("[RUST-DIFF] vfs_path coverage: trunc=%d exact_fit=%d unterminated=%d cap1=%d\n",
            cov_trunc, cov_exact, cov_unterm, cov_cap1);
}
