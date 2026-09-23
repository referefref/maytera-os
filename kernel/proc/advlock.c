// proc/advlock.c - #404 Stage 6: POSIX (fcntl) + BSD (flock) advisory file
// locking, C glue over the Rust manager (rustkern/advlock.rs). See advlock.h.
//
// SPLIT RATIONALE (Rust vs C):
//   Rust (rustkern/advlock.rs): the lock TABLE and all decisions over it -
//   byte-range overlap, conflict detection, POSIX merge/split/replace of
//   same-owner ranges, and the wait-for cycle (deadlock) check. Pure integer
//   logic on a fixed table: the part Rust makes safe.
//   C (this file + a resolver in fdlayer.c): the parts genuinely entangled with
//   the C kernel and deliberately kept in C:
//     - copy_from_user()/copy_to_user() of the userland struct flock (SMAP
//       bracketed by the shared uaccess helper);
//     - fd -> canonical path -> file identity, which must read the static
//       legacy fd tables that live in fdlayer.c;
//     - l_whence resolution against the fd's position/size;
//     - the fcntl/flock syscall dispatch;
//     - the close and process-exit release hooks;
//     - the F_SETLKW sleep, which MUST go through the kernel wait queue
//       (sync/waitq.h wait_event_interruptible), a C primitive.
//
// CONCURRENCY: one global spinlock g_advlock_lock serialises EVERY call into
// the Rust manager (that is the manager's documented contract). It is held only
// for the tiny table operation, NEVER across the wait_event sleep: the F_SETLKW
// loop drops it before sleeping and re-takes it to retry, the #426 discipline.
#include "advlock.h"
#include "syscall.h"
#include "process.h"
#include "../types.h"
#include "../string.h"
#include "../serial.h"
#include "../sync/spinlock.h"
#include "../sync/waitq.h"
#include "../security/validate.h"   // copy_from_user / copy_to_user

// ---- errno (Linux numbers; the userland libc maps these, see errno.h) -------
#define ADV_EINTR    4
#define ADV_EBADF    9
#define ADV_EAGAIN   11
#define ADV_EFAULT   14
#define ADV_EINVAL   22
#define ADV_EDEADLK  35
#define ADV_ENOLCK   37

// ---- lock types, matching libc fcntl.h --------------------------------------
#define ADV_F_RDLCK  0
#define ADV_F_WRLCK  1
#define ADV_F_UNLCK  2

// ---- flock operations, matching libc sys/file.h -----------------------------
#define ADV_LOCK_SH  1
#define ADV_LOCK_EX  2
#define ADV_LOCK_NB  4
#define ADV_LOCK_UN  8

// ---- Rust manager result codes (rustkern/advlock.rs) ------------------------
#define ADV_R_GRANTED    0
#define ADV_R_WOULDBLOCK 1
#define ADV_R_DEADLK     2
#define ADV_R_NOSPACE    3
#define ADV_R_BADARG     4

#define ADV_U64MAX ((uint64_t)-1)

// ---- the userland struct flock ABI. Kept byte-identical to userland libc
// fcntl.h; the _Static_asserts fail the build if either side drifts. ----------
struct flock {
    short     l_type;
    short     l_whence;
    long long l_start;
    long long l_len;
    int       l_pid;
};
_Static_assert(sizeof(struct flock) == 32, "advlock: struct flock ABI drift (size)");
_Static_assert(__builtin_offsetof(struct flock, l_type)   == 0,  "advlock: l_type offset");
_Static_assert(__builtin_offsetof(struct flock, l_whence) == 2,  "advlock: l_whence offset");
_Static_assert(__builtin_offsetof(struct flock, l_start)  == 8,  "advlock: l_start offset");
_Static_assert(__builtin_offsetof(struct flock, l_len)    == 16, "advlock: l_len offset");
_Static_assert(__builtin_offsetof(struct flock, l_pid)    == 24, "advlock: l_pid offset");

// ---- Rust FFI (rustkern/advlock.rs). All but advlock_fileid_rs require
// g_advlock_lock held across the call. -----------------------------------------
extern uint64_t advlock_fileid_rs(const uint8_t *path, uint64_t len);
extern int  advlock_try_rs(uint64_t file_id, uint32_t owner, int ltype, uint64_t start, uint64_t end);
extern int  advlock_setw_step_rs(uint64_t file_id, uint32_t owner, int ltype, uint64_t start, uint64_t end);
extern int  advlock_grantable_locked_rs(uint64_t file_id, uint32_t owner, int ltype, uint64_t start, uint64_t end);
extern int  advlock_wait_end_rs(uint32_t owner);
extern int  advlock_unlock_rs(uint64_t file_id, uint32_t owner, uint64_t start, uint64_t end);
extern int  advlock_getlk_rs(uint64_t file_id, uint32_t owner, int ltype, uint64_t start, uint64_t end,
                             int *out_type, uint64_t *out_start, uint64_t *out_end, uint32_t *out_pid);
extern int  advlock_flock_try_rs(uint64_t file_id, int fd_slot, uint32_t owner, int want);
extern int  advlock_flock_grantable_locked_rs(uint64_t file_id, int fd_slot, int want);
extern int  advlock_release_fdslot_rs(int fd_slot);
extern int  advlock_release_owner_file_rs(uint64_t file_id, uint32_t owner);
extern int  advlock_release_owner_all_rs(uint32_t owner);
extern uint32_t advlock_stat_rs(int sel);
extern int  advlock_capacity_rs(void);
extern int  advlock_selftest_rs(void);

// Resolver exported from proc/fdlayer.c (it reads the static legacy fd tables).
// Fills pathbuf with the canonical path the lock is keyed on, *slot_out with the
// legacy table index (the flock identity), and best-effort *size_out/*pos_out
// for l_whence resolution. Returns 0 on success, -1 if `fd` is not a lockable,
// caller-owned legacy file fd.
extern int advlock_resolve_fd(int fd, char *pathbuf, int cap, int *slot_out,
                              uint64_t *size_out, uint64_t *pos_out);

// ---- the single serialising lock + the shared wait queue --------------------
static spinlock_t g_advlock_lock = SPINLOCK_INIT;
static wait_queue_head_t g_advlock_wq;
static int g_advlock_wq_ready = 0;

// Owner identity: the thread group, so pthreads of one process share locks and a
// different process does not. Identical rule to fdlayer.c's legacy_owner_id().
static uint32_t advlock_owner_id(void) {
    process_t *p = proc_current();
    if (!p) return 0;
    return p->tgid ? p->tgid : p->pid;
}

// wait_event condition wrapper: takes the lock, asks the manager, releases. A
// leaf; it never sleeps, so it is safe to evaluate inside the wait_event macro.
static int advlock_cond_posix(uint64_t fid, uint32_t owner, int lt, uint64_t s, uint64_t e) {
    uint64_t f = spinlock_acquire_irqsave(&g_advlock_lock);
    int g = advlock_grantable_locked_rs(fid, owner, lt, s, e);
    spinlock_release_irqrestore(&g_advlock_lock, f);
    return g;
}
static int advlock_cond_flock(uint64_t fid, int slot, int want) {
    uint64_t f = spinlock_acquire_irqsave(&g_advlock_lock);
    int g = advlock_flock_grantable_locked_rs(fid, slot, want);
    spinlock_release_irqrestore(&g_advlock_lock, f);
    return g;
}

// Compute the [start,end) byte range from a struct flock plus the fd's base
// (resolved from l_whence). end == ADV_U64MAX means "to EOF" (l_len == 0).
// Returns 0 on success, -EINVAL on a malformed (negative) range.
static int advlock_range(const struct flock *fl, uint64_t base, uint64_t *start, uint64_t *end) {
    long long astart = (long long)base + fl->l_start;
    long long len = fl->l_len;
    if (len == 0) {
        if (astart < 0) return -ADV_EINVAL;
        *start = (uint64_t)astart;
        *end = ADV_U64MAX;
    } else if (len > 0) {
        if (astart < 0) return -ADV_EINVAL;
        *start = (uint64_t)astart;
        *end = (uint64_t)astart + (uint64_t)len;
    } else {
        // Negative len: the range is [astart+len, astart) (POSIX).
        long long s2 = astart + len;
        if (s2 < 0) return -ADV_EINVAL;
        *start = (uint64_t)s2;
        *end = (uint64_t)astart;
    }
    return 0;
}

// The F_SETLKW blocking wait: attempt under the lock; if it would block, sleep
// on the wait queue (dropping the lock first) and retry on wake. Deadlock ->
// EDEADLK, signal -> EINTR. This is the #426-sanctioned pattern: the ONLY wait
// is wait_event_interruptible; there is no busy-poll and no proc_yield.
static int64_t advlock_posix_setlkw(uint64_t fid, uint32_t owner, int lt,
                                    uint64_t start, uint64_t end) {
    int64_t rc;
    for (;;) {
        uint64_t f = spinlock_acquire_irqsave(&g_advlock_lock);
        int r = advlock_setw_step_rs(fid, owner, lt, start, end);
        spinlock_release_irqrestore(&g_advlock_lock, f);
        if (r == ADV_R_GRANTED) { rc = 0; break; }
        if (r == ADV_R_DEADLK)  { rc = -ADV_EDEADLK; break; }
        if (r == ADV_R_NOSPACE) { rc = -ADV_ENOLCK; break; }
        if (r != ADV_R_WOULDBLOCK) { rc = -ADV_EINVAL; break; }
        // Sleep until a conflicting lock is released (or a signal arrives). The
        // condition is re-checked by the macro; the real grant happens in the
        // next advlock_setw_step_rs above.
        int wr = wait_event_interruptible(&g_advlock_wq,
                     advlock_cond_posix(fid, owner, lt, start, end));
        if (wr == WAIT_EINTR) { rc = -ADV_EINTR; break; }
        // else: woke because possibly grantable - loop and re-attempt.
    }
    // Drop this waiter's deadlock edges regardless of how the loop ended.
    uint64_t f = spinlock_acquire_irqsave(&g_advlock_lock);
    advlock_wait_end_rs(owner);
    spinlock_release_irqrestore(&g_advlock_lock, f);
    return rc;
}

int64_t advlock_fcntl(int fd, int cmd, const void *uptr) {
    struct flock fl;
    if (!uptr) return -ADV_EFAULT;
    if (copy_from_user(&fl, uptr, sizeof(fl)) != 0) return -ADV_EFAULT;

    char path[ADVLOCK_PATH_MAX];
    int slot;
    uint64_t sz = 0, pos = 0;
    if (advlock_resolve_fd(fd, path, sizeof(path), &slot, &sz, &pos) != 0)
        return -ADV_EBADF;

    uint64_t base;
    switch (fl.l_whence) {
        case 0: base = 0;   break;  // SEEK_SET
        case 1: base = pos; break;  // SEEK_CUR
        case 2: base = sz;  break;  // SEEK_END
        default: return -ADV_EINVAL;
    }

    uint64_t start, end;
    int rr = advlock_range(&fl, base, &start, &end);
    if (rr != 0) return rr;

    uint32_t owner = advlock_owner_id();
    uint64_t fid = advlock_fileid_rs((const uint8_t *)path, (uint64_t)strlen(path));
    int lt = fl.l_type;

    if (cmd == ADV_F_GETLK) {
        int rtype = ADV_F_UNLCK; uint64_t rstart = 0, rend = 0; uint32_t rpid = 0;
        uint64_t f = spinlock_acquire_irqsave(&g_advlock_lock);
        int hit = advlock_getlk_rs(fid, owner, lt, start, end, &rtype, &rstart, &rend, &rpid);
        spinlock_release_irqrestore(&g_advlock_lock, f);
        if (hit) {
            fl.l_type = (short)rtype;
            fl.l_whence = 0; // SEEK_SET
            fl.l_start = (long long)rstart;
            fl.l_len = (rend == ADV_U64MAX) ? 0 : (long long)(rend - rstart);
            fl.l_pid = (int)rpid;
        } else {
            fl.l_type = ADV_F_UNLCK;
        }
        if (copy_to_user((void *)uptr, &fl, sizeof(fl)) != 0) return -ADV_EFAULT;
        return 0;
    }

    if (lt == ADV_F_UNLCK) {
        uint64_t f = spinlock_acquire_irqsave(&g_advlock_lock);
        int ch = advlock_unlock_rs(fid, owner, start, end);
        spinlock_release_irqrestore(&g_advlock_lock, f);
        if (ch) wake_up_all(&g_advlock_wq);
        return 0;
    }

    if (lt != ADV_F_RDLCK && lt != ADV_F_WRLCK) return -ADV_EINVAL;

    if (cmd == ADV_F_SETLK) {
        uint64_t f = spinlock_acquire_irqsave(&g_advlock_lock);
        int r = advlock_try_rs(fid, owner, lt, start, end);
        spinlock_release_irqrestore(&g_advlock_lock, f);
        if (r == ADV_R_GRANTED) return 0;
        if (r == ADV_R_WOULDBLOCK) return -ADV_EAGAIN;
        if (r == ADV_R_NOSPACE) return -ADV_ENOLCK;
        return -ADV_EINVAL;
    }

    if (cmd == ADV_F_SETLKW) {
        return advlock_posix_setlkw(fid, owner, lt, start, end);
    }
    return -ADV_EINVAL;
}

int64_t sys_flock(int fd, int operation) {
    int nb = (operation & ADV_LOCK_NB) ? 1 : 0;
    int op = operation & ~ADV_LOCK_NB;

    char path[ADVLOCK_PATH_MAX];
    int slot;
    uint64_t sz = 0, pos = 0;
    if (advlock_resolve_fd(fd, path, sizeof(path), &slot, &sz, &pos) != 0)
        return -ADV_EBADF;

    uint32_t owner = advlock_owner_id();
    uint64_t fid = advlock_fileid_rs((const uint8_t *)path, (uint64_t)strlen(path));

    int want;
    switch (op) {
        case ADV_LOCK_SH: want = ADV_F_RDLCK; break;
        case ADV_LOCK_EX: want = ADV_F_WRLCK; break;
        case ADV_LOCK_UN: {
            uint64_t f = spinlock_acquire_irqsave(&g_advlock_lock);
            int n = advlock_flock_try_rs(fid, slot, owner, ADV_F_UNLCK);
            spinlock_release_irqrestore(&g_advlock_lock, f);
            (void)n;
            wake_up_all(&g_advlock_wq);
            return 0;
        }
        default: return -ADV_EINVAL;
    }

    for (;;) {
        uint64_t f = spinlock_acquire_irqsave(&g_advlock_lock);
        int r = advlock_flock_try_rs(fid, slot, owner, want);
        spinlock_release_irqrestore(&g_advlock_lock, f);
        if (r == ADV_R_GRANTED) return 0;
        if (r == ADV_R_NOSPACE) return -ADV_ENOLCK;
        if (r != ADV_R_WOULDBLOCK) return -ADV_EINVAL;
        if (nb) return -ADV_EAGAIN;   // LOCK_NB: do not block
        int wr = wait_event_interruptible(&g_advlock_wq,
                     advlock_cond_flock(fid, slot, want));
        if (wr == WAIT_EINTR) return -ADV_EINTR;
        // else retry
    }
}

void advlock_on_close(int fd_userland) {
    char path[ADVLOCK_PATH_MAX];
    int slot;
    uint64_t sz = 0, pos = 0;
    if (advlock_resolve_fd(fd_userland, path, sizeof(path), &slot, &sz, &pos) != 0)
        return;   // not a lockable / not-owned fd: nothing to release
    uint32_t owner = advlock_owner_id();
    uint64_t fid = advlock_fileid_rs((const uint8_t *)path, (uint64_t)strlen(path));
    uint64_t f = spinlock_acquire_irqsave(&g_advlock_lock);
    int a = advlock_release_owner_file_rs(fid, owner);  // POSIX: any close frees them
    int b = advlock_release_fdslot_rs(slot);            // this fd's flock
    spinlock_release_irqrestore(&g_advlock_lock, f);
    if (a || b) wake_up_all(&g_advlock_wq);
}

void advlock_proc_exit(uint32_t owner) {
    if (!owner) return;
    uint64_t f = spinlock_acquire_irqsave(&g_advlock_lock);
    int n = advlock_release_owner_all_rs(owner);
    spinlock_release_irqrestore(&g_advlock_lock, f);
    if (n) {
        // Waiters blocked on this owner's now-gone locks must be woken.
        if (g_advlock_wq_ready) wake_up_all(&g_advlock_wq);
    }
}

void advlock_boot_check(void) {
    wait_queue_head_init(&g_advlock_wq);
    g_advlock_wq_ready = 1;
    int cap = advlock_capacity_rs();
    int st = advlock_selftest_rs();
    kprintf("[ADVLOCK] selftest=%s capacity=%d (fcntl+flock advisory locking)\n",
            st == 0 ? "PASS" : "FAIL", cap);
    if (st != 0)
        kprintf("[ADVLOCK] SELF-TEST FAILED step=%d - advisory file locking is "
                "NOT trustworthy on this build\n", st);
}
