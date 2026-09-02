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
#include "../sync/spinlock.h"

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

// #695 Phase 2: returns the final flush status. Not-the-last-reference is 0,
// and so is a file kind with no release op. kfree() happens unconditionally: a
// failed flush must not leak the description.
int file_put(file_t *f) {
    if (!f) return 0;
    f->refcount--;
    if (f->refcount <= 0) {
        int rc = 0;
        if (f->ops && f->ops->release) rc = f->ops->release(f);
        kfree(f);
        return rc;
    }
    return 0;
}

// #695 Phase 1: flush without releasing. A file kind with no flush op buffers
// nothing, so 0 is the truth for it and not a stub.
int file_flush(file_t *f) {
    if (!f || !f->ops || !f->ops->flush) return 0;
    return f->ops->flush(f);
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

// #120. -1 means "this kind does not know its size", NOT "size zero": the two
// are different answers and a caller must be able to tell them apart. Callers
// that get -1 must leave st_size alone rather than write 0 into it.
int64_t file_size(file_t *f) {
    if (!f || !f->ops || !f->ops->size) return -1;
    return f->ops->size(f);
}

int file_ioctl(file_t *f, unsigned cmd, void *arg) {
    if (!f || !f->ops || !f->ops->ioctl) return -1;
    return f->ops->ioctl(f, cmd, arg);
}

int file_poll(file_t *f, int events) {
    if (!f || !f->ops || !f->ops->poll) return 0;
    return f->ops->poll(f, events);
}

// #745 (local 82): "does this file kind answer readiness questions at all?"
// Deliberately NOT folded into file_poll()'s return value: changing that to
// report always-ready for a NULL op would silently change select()'s answer
// for every existing caller, including the one live select() user. The
// distinction is exposed instead, and poll(2) applies the POSIX regular-file
// rule itself.
int file_has_poll(file_t *f) {
    return (f && f->ops && f->ops->poll) ? 1 : 0;
}

struct wait_queue_head *file_poll_wq(file_t *f, int events) {
    if (!f || !f->ops || !f->ops->poll_wq) return 0;
    return f->ops->poll_wq(f, events);
}

// ============================================================================
// Per-process fd table
// ============================================================================
//
// LOCKING (#SMPGLOBALS, 2026-08-30). Every process_t carries its own fd_lock
// (proc/process.h). It protects exactly two things: that process's fds[] slots
// and its fd_cloexec bitmap.
//
// IT IS THE SHARED spinlock_t FROM sync/spinlock.h. No private lock type was
// invented for the fd layer.
//
// IRQSAVE, because the table is mutated with interrupts already off:
// proc_exit() calls fd_close_all() under cli().
//
// LEAF: nothing is called while it is held that takes another lock or can
// sleep. file_put() is ALWAYS called after the lock is dropped, because a
// final put runs the description's release op, and for a file-backed
// description that writes to disk. Every function below is therefore the same
// shape: claim or clear the slot under the lock, remember what was evicted,
// drop the lock, then put.
//
// WHAT THIS BUYS, AND WHAT IT DOES NOT. Stated plainly, because a locking
// comment that overstates its reach is how the next person builds on sand:
//
//   IT DOES fix slot mutation. Two contexts can no longer both claim the same
//   fd, a close racing a close can no longer double-put one description, and
//   a reader walking another process's table (the Task Manager, procinfo.c)
//   can no longer see a slot mid-swap.
//
//   IT DOES NOT make fd_get()'s returned pointer safe to use after the lock is
//   dropped. The caller gets a raw file_t* and holds no reference, so closing
//   that description on another core while a first core is inside file_read()
//   on it is STILL a use-after-free. Closing that needs fd_get() to take a
//   reference and every one of its ~10 callers to put it back, which is a
//   wider change than this one and is deliberately NOT done here.
//
//   IT DOES NOT protect file_t::refcount itself, which is a plain ++/-- shared
//   between every process that inherited the description across fork. Two
//   cores putting the same description can still lose a decrement.
//
// The previous comment here said the table was unlocked, that this was
// "acceptable today because the kernel is single-CPU", and that "when SMP
// lands, this table needs a per-process spinlock". This is that spinlock.

// ---------------------------------------------------------------------------
// The lock goes through these two macros for one reason: `make FDRACETEST=1`
// arms a RED arm in which fdrace_test.c can turn the lock OFF at run time and
// re-run the identical code, so the harness can be SEEN to go red on the
// pre-2026-08-30 behaviour. A test that has only ever been observed passing
// says nothing about the thing it points at.
//
// In a normal build g_fdlock_off does not exist and these expand to the bare
// spinlock calls: no branch, no flag, no cost.
// ---------------------------------------------------------------------------
#ifdef FDRACE_TEST
int g_fdlock_off = 0;   // set from /FDLOCKOFF.TXT by main.c, FDRACETEST build only
#define FDL_ACQ(pp)     (g_fdlock_off ? (uint64_t)0 : spinlock_acquire_irqsave(&(pp)->fd_lock))
#define FDL_REL(pp, fl) do { if (!g_fdlock_off) spinlock_release_irqrestore(&(pp)->fd_lock, (fl)); } while (0)
#else
#define FDL_ACQ(pp)     spinlock_acquire_irqsave(&(pp)->fd_lock)
#define FDL_REL(pp, fl) spinlock_release_irqrestore(&(pp)->fd_lock, (fl))
#endif

// The find half of an allocation, with the lock already held. Split out so
// fd_alloc_install() can do find-and-claim atomically instead of racing
// itself between two locked sections.
static int fd_alloc_locked(process_t *p, int min) {
    if (min < 0) min = 0;
    if (min >= MAX_FDS) return -1;
    for (int i = min; i < MAX_FDS; i++) {
        if (p->fds[i] == NULL) return i;
    }
    return -1;
}

// ADVISORY. It reports a free slot, it does not reserve one: the slot can be
// taken again before the caller installs into it. Locking the read only stops
// it tearing against a concurrent mutation. Callers that mean "give me an fd
// for this description" must use fd_alloc_install(), which claims under one
// lock and cannot race.
int fd_alloc(int min) {
    process_t *p = proc_current();
    if (!p) return -1;
    uint64_t fl = FDL_ACQ(p);
    int fd = fd_alloc_locked(p, min);
    FDL_REL(p, fl);
    return fd;
}

int fd_install(int fd, file_t *f) {
    process_t *p = proc_current();
    if (!p) return -1;
    if (fd < 0 || fd >= MAX_FDS) return -1;

    // If the slot is already occupied, close the old one. This matches dup2
    // semantics; ordinary callers should target an empty slot.
    uint64_t fl = FDL_ACQ(p);
    file_t *evicted = p->fds[fd];
    p->fds[fd] = f;
    FDL_REL(p, fl);

    if (evicted) {
        // #695: slot eviction must SUCCEED even when the evicted description's
        // final flush failed, or dup2 / shell redirection would start failing
        // on a full disk. There is no recipient for this error here; log and
        // go on. Outside the lock: this can write to disk.
        int frc = file_put(evicted);
        if (frc != 0)
            kprintf("[VFS] fd_install: evicted fd %d final flush failed rc=%d\n", fd, frc);
    }
    return 0;
}

// See the "WHAT THIS BUYS" note above: the lock makes the READ of the slot
// atomic against a concurrent swap, it does NOT give the caller a reference.
file_t *fd_get(int fd) {
    process_t *p = proc_current();
    if (!p) return NULL;
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    uint64_t fl = FDL_ACQ(p);
    file_t *f = p->fds[fd];
    FDL_REL(p, fl);
    return f;
}

// Find AND claim under ONE lock. Two of these racing used to be able to return
// the same fd twice, with the loser's description leaked and both callers
// writing to one file.
//
// Split from its proc_current() wrapper so fdrace_test.c can drive this exact
// body against a chosen PCB from two cores. The shipping path is the wrapper;
// there is no second copy of the logic.
int fd_alloc_install_on(process_t *p, file_t *f) {
    if (!p) return -1;
    uint64_t fl = FDL_ACQ(p);
    int fd = fd_alloc_locked(p, 3);
#ifdef FDRACE_TEST
    // The find-then-store window, held open on purpose so fs/fdrace_test.c can
    // show what walks into it when the lock is off. Absent from a normal build.
    { extern void fdrace_window(void); fdrace_window(); }
#endif
    if (fd >= 0) p->fds[fd] = f;
    FDL_REL(p, fl);
    return fd;
}

int fd_alloc_install(file_t *f) {
    // Start at 3 to preserve the traditional 0/1/2 reservation until Phase A2
    // pre-opens /dev/console on them.
    return fd_alloc_install_on(proc_current(), f);
}

// Same split as fd_alloc_install_on(), same reason.
int fd_close_on(process_t *p, int fd) {
    if (!p) return -1;
    if (fd < 0 || fd >= MAX_FDS) return -1;

    // Claiming the slot under the lock is what makes a close racing a close
    // safe: exactly one of them comes away with the pointer, so exactly one
    // put happens.
    uint64_t fl = FDL_ACQ(p);
    file_t *f = p->fds[fd];
    if (f) {
        p->fds[fd] = NULL;
        p->fd_cloexec &= ~(1ULL << fd);
    }
    FDL_REL(p, fl);

    if (!f) return -1;
    // #695 Phase 2: THIS is the close() a user program sees (sys_close routes
    // here for every per-process fd), so the final flush status propagates.
    return file_put(f);
}

int fd_close(int fd) { return fd_close_on(proc_current(), fd); }

// #695 Phase 2: proc_exit() calls this, under cli(), with nobody left to tell.
// It therefore LOGS AND CONTINUES and stays void on purpose: propagating from
// here would turn "the disk is full" into a fault on every process exit, and
// stopping early would leak every remaining description.
//
// One slot per lock acquisition, deliberately. Snapshotting all 64 pointers
// under one acquire would need a 512-byte array on the exiting task's ring-0
// stack; taking and dropping the lock 64 times at process exit costs nothing
// that matters and keeps file_put() outside it.
void fd_close_all(void) {
    process_t *p = proc_current();
    if (!p) return;
    for (int i = 0; i < MAX_FDS; i++) {
        uint64_t fl = FDL_ACQ(p);
        file_t *f = p->fds[i];
        p->fds[i] = NULL;
        FDL_REL(p, fl);
        if (f) {
            int frc = file_put(f);
            if (frc != 0)
                kprintf("[VFS] proc exit: fd %d final flush failed rc=%d (data lost)\n", i, frc);
        }
    }
    uint64_t fl = FDL_ACQ(p);
    p->fd_cloexec = 0;
    FDL_REL(p, fl);
}

// file_get() is a bare refcount increment with no release op behind it, so it
// is the one thing that may run inside the lock.
void fd_refcount_all_plus_plus(void) {
    process_t *p = proc_current();
    if (!p) return;
    uint64_t fl = FDL_ACQ(p);
    for (int i = 0; i < MAX_FDS; i++) {
        if (p->fds[i]) file_get(p->fds[i]);
    }
    FDL_REL(p, fl);
}

// ============================================================================
// Phase A3: dup / dup2 / CLOEXEC
// ============================================================================

int fd_dup(int oldfd, int min) {
    process_t *p = proc_current();
    if (!p) return -1;
    if (oldfd < 0 || oldfd >= MAX_FDS) return -1;

    // Read, find and claim under ONE acquire. Split across two, the slot
    // fd_alloc_locked() reported could be taken before we install into it.
    uint64_t fl = FDL_ACQ(p);
    file_t *f = p->fds[oldfd];
    int newfd = f ? fd_alloc_locked(p, min < 0 ? 0 : min) : -1;
    if (newfd >= 0) {
        file_get(f);
        p->fds[newfd] = f;
        // dup() never inherits CLOEXEC; F_DUPFD_CLOEXEC sets it at the syscall
        // layer after this call returns.
        p->fd_cloexec &= ~(1ULL << newfd);
    }
    FDL_REL(p, fl);
    return newfd;
}

int fd_dup2(int oldfd, int newfd) {
    process_t *p = proc_current();
    if (!p) return -1;
    if (oldfd < 0 || oldfd >= MAX_FDS) return -1;
    if (newfd < 0 || newfd >= MAX_FDS) return -1;
    // POSIX: dup2 with oldfd == newfd is a no-op that returns newfd, but only
    // if oldfd is open. The open test is done under the lock below.
    uint64_t fl = FDL_ACQ(p);
    file_t *f = p->fds[oldfd];
    file_t *evicted = NULL;
    if (f && oldfd != newfd) {
        evicted = p->fds[newfd];
        file_get(f);
        p->fds[newfd] = f;
        p->fd_cloexec &= ~(1ULL << newfd);
    }
    FDL_REL(p, fl);

    if (!f) return -1;
    if (evicted) {
        // #695: same rule as fd_install - dup2 must still succeed when the
        // description it evicts could not be flushed.
        int frc = file_put(evicted);
        if (frc != 0)
            kprintf("[VFS] dup2: evicted fd %d final flush failed rc=%d\n", newfd, frc);
    }
    return newfd;
}

// #695 Phase 2: runs inside execve, which has already committed to replacing
// the image. Same reasoning as fd_close_all: log and continue, stay void.
void fd_close_cloexec(void) {
    process_t *p = proc_current();
    if (!p) return;
    // Same one-slot-per-acquire shape as fd_close_all(), for the same reason:
    // file_put() must not run with the lock held.
    for (int i = 0; i < MAX_FDS; i++) {
        uint64_t fl = FDL_ACQ(p);
        file_t *f = NULL;
        if ((p->fd_cloexec & (1ULL << i)) && p->fds[i]) {
            f = p->fds[i];
            p->fds[i] = NULL;
        }
        FDL_REL(p, fl);
        if (f) {
            int frc = file_put(f);
            if (frc != 0)
                kprintf("[VFS] execve: cloexec fd %d final flush failed rc=%d (data lost)\n", i, frc);
        }
    }
    uint64_t fl = FDL_ACQ(p);
    p->fd_cloexec = 0;
    FDL_REL(p, fl);
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
