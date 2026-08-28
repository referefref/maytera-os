/* pipe.c - Kernel pipe implementation for MayteraOS
 *
 * Provides anonymous pipe pairs for inter-process communication.
 * Each pipe has a 64KB kernel ring buffer shared between a read end
 * and a write end. Data written to the write end can be read from
 * the read end (FIFO). When all write ends are closed, reads return
 * 0 (EOF) once the buffer is drained.
 */

#include "../types.h"
#include "../mm/heap.h"
#include "../proc/process.h"
#include "../sync/waitq.h"   /* #511: block on a wait queue, never poll (#426) */
#include "../sync/noblock.h" /* #111: degrade instead of parking a no-block context */
#include "../proc/signal.h"  /* #111: SIGPIPE */
#include "vfs.h"
#include "../security/uaccess_smap.h"  // #19/#645: AC bracket on the caller-buffer copy

#define PIPE_BUF_SIZE  65536  /* 64KB ring buffer */

/* #111: negative-errno returns. This kernel has no errno header; the values
 * are the ones userland/libc/errno.h defines, which is what write(2)'s raw
 * pass-through in userland/libc/stdlib.c delivers to Ring 3 unchanged. */
#define PIPE_EPIPE   (-32)   /* EPIPE:  the read end is gone */
#define PIPE_EINTR    (-4)   /* EINTR:  interrupted, nothing written */
#define PIPE_EAGAIN  (-11)   /* EAGAIN: would block, and this context may not */

/* #111(a)(b): the write-side decision machine lives in rustkern/pipewr.rs.
 * Actions and the returned pair are documented there; the numbering is locked
 * to it by the _Static_assert-style checks in that module's self-test, which
 * runs at boot. */
#define PW_COPY   0u
#define PW_BLOCK  1u
#define PW_DONE   2u
#define PW_SHORT  3u
#define PW_EPIPE  4u
#define PW_EINTR  5u
typedef struct { uint32_t action; uint32_t n; } pipe_write_step_t;
extern pipe_write_step_t pipe_write_step_rs(uint64_t requested, uint64_t done,
                                            uint32_t ring_size, uint32_t ring_count,
                                            int32_t readers, int32_t intr);
extern uint32_t pipe_write_step_selftest_rs(void);

typedef struct pipe_state {
    uint8_t  *buf;
    uint32_t  size;
    uint32_t  read_pos;
    uint32_t  write_pos;
    uint32_t  count;       /* bytes available to read */
    int       readers;     /* number of open read-end file_t's */
    int       writers;     /* number of open write-end file_t's */
    /* #511: readers park here for "data arrived" or "last writer closed".
     * Both wake sources are ours and both are unconditional:
     *   - pipe_write_fn()      wakes after every write that adds bytes
     *   - pipe_release_write() wakes ALL when the writer count drops (EOF)
     * so a reader cannot miss either event. This is class A per the wait
     * migration plan: the wake is ours, and waiting forever for a writer that
     * never writes is the CORRECT pipe semantic, so there is no timeout. */
    wait_queue_head_t read_wq;
    /* #111(a): writers park here for "space appeared" or "last reader closed".
     * Symmetric with read_wq above, and both wake sources are ours and
     * unconditional, so no wake can be lost and no timeout is needed:
     *   - pipe_read_fn()      wakes after every read that frees bytes
     *   - pipe_release_read() wakes ALL when the reader count drops
     * The SECOND one is what makes the untimed sleep safe rather than a new
     * hang: without it a writer blocked on a full pipe whose reader then died
     * would wait forever for space that can never appear. Adding a blocking
     * write WITHOUT that wake would have traded a CPU burn for a deadlock. */
    wait_queue_head_t write_wq;
} pipe_state_t;

/* Forward declarations for file_ops */
static int64_t pipe_read_fn(file_t *f, void *buf, size_t count);
static int64_t pipe_write_fn(file_t *f, const void *buf, size_t count);
static int     pipe_release_read(file_t *f);
static int     pipe_release_write(file_t *f);

/* #745 (local 82). A pipe is THE one file kind for which "no ops->poll"
 * would have been an outright lie. poll(2) treats a file with no ops->poll
 * as a regular file, i.e. always ready, which is correct for FAT/ext2 files
 * and for /dev/null but is exactly wrong for an EMPTY pipe: the caller would
 * be told to read, would get zero bytes, and would spin. So the answer is
 * computed from the same three facts pipe_read_fn() blocks on.
 *
 * POLL_HUP on the read end means "no writers left": with the buffer drained
 * that is EOF, and reporting it is how a poll()-driven reader learns to stop
 * rather than waiting forever for a writer that will never come. */
static int pipe_read_poll(file_t *f, int events) {
    pipe_state_t *ps = (pipe_state_t *)f->priv;
    if (!ps) return POLL_ERR;
    int r = 0;
    if ((events & POLL_IN) && ps->count > 0) r |= POLL_IN;
    if (ps->writers <= 0) r |= POLL_HUP;
    return r;
}

static int pipe_write_poll(file_t *f, int events) {
    pipe_state_t *ps = (pipe_state_t *)f->priv;
    if (!ps) return POLL_ERR;
    int r = 0;
    /* Writable while the ring has room. #111: pipe_write_fn() now BLOCKS when
     * the ring is full instead of returning a short/zero count, so "room for
     * at least one byte" is exactly the condition under which a write will
     * make progress without sleeping. Same test, and now it is also the
     * condition the write queue is woken on, so poll() and write() agree. */
    if ((events & POLL_OUT) && ps->count < ps->size) r |= POLL_OUT;
    /* No readers left: a write would be a SIGPIPE case. POLL_ERR is what
     * poll(2) callers check before writing. */
    if (ps->readers <= 0) r |= POLL_ERR;
    return r;
}

/* The queue a poll(2) waiter parks on. The READ end is woken by
 * pipe_write_fn() on every write and by pipe_release_write() on EOF, so both
 * events a reader waits for reach it. */
static struct wait_queue_head *pipe_read_poll_wq(file_t *f, int events) {
    pipe_state_t *ps = (pipe_state_t *)f->priv;
    (void)events;
    if (!ps) return NULL;
    return &ps->read_wq;
}

/* #111: the WRITE end can now publish a queue too, and this comment used to
 * say why it could not. The old text was correct at the time and is worth
 * keeping as the reason this is safe NOW rather than deleting: "publishing a
 * queue nobody wakes would be worse than publishing none, because poll()
 * would sleep the full timeout believing it had an exact wake, instead of
 * bounding the sleep and re-scanning". write_wq has two unconditional wake
 * sources (see the struct comment), so it is no longer a queue nobody wakes,
 * and a poll(2) waiter on a write end gets an exact wake instead of a
 * bounded re-scan. */
static struct wait_queue_head *pipe_write_poll_wq(file_t *f, int events) {
    pipe_state_t *ps = (pipe_state_t *)f->priv;
    (void)events;
    if (!ps) return NULL;
    return &ps->write_wq;
}

static const file_ops_t pipe_read_ops = {
    .read    = pipe_read_fn,
    .write   = NULL,
    .seek    = NULL,
    .ioctl   = NULL,
    .release = pipe_release_read,
    .poll    = pipe_read_poll,
    .poll_wq = pipe_read_poll_wq,
};

static const file_ops_t pipe_write_ops = {
    .read    = NULL,
    .write   = pipe_write_fn,
    .seek    = NULL,
    .ioctl   = NULL,
    .release = pipe_release_write,
    .poll    = pipe_write_poll,
    .poll_wq = pipe_write_poll_wq,   /* #111: now a queue with real wakes */
};

/* Read from the pipe buffer. Blocks until data arrives or the last writer
 * closes. Returns 0 ONLY on real EOF (no writers left and buffer drained). */
static int64_t pipe_read_fn(file_t *f, void *buf, size_t count) {
    pipe_state_t *ps = (pipe_state_t *)f->priv;
    if (!ps || count == 0) return 0;

    /* #511 (was: proc_yield() spin with `if (++spins > 100000) return 0;`).
     * That "safety timeout" was SILENT DATA LOSS: 0 is EOF in the pipe
     * protocol, so a pipe that was merely SLOW (a writer descheduled under
     * load, a big/blocked producer) was indistinguishable from a closed
     * writer. The reader got a truncated stream with no error and no way to
     * tell. A read must never invent an EOF that did not happen.
     *
     * Now we block on read_wq instead. There is no timeout by design: the only
     * two things that can end this wait are events we own and always signal
     * (data written, or the last writer closing), and blocking forever on a
     * writer that has not written yet is exactly what a pipe is supposed to
     * do. Interruptible, so a signal still unblocks with -EINTR (a real error
     * the caller can distinguish from EOF, which is the whole point). */
    while (ps->count == 0) {
        if (ps->writers <= 0) return 0;   /* EOF: all writers closed */
        int rc = wait_event_interruptible(&ps->read_wq,
                                          ps->count > 0 || ps->writers <= 0);
        if (rc == WAIT_EINTR) return -4;  /* -EINTR, NOT a fake EOF */
    }

    uint8_t *out = (uint8_t *)buf;
    uint32_t avail = ps->count;
    uint32_t to_read = ((uint32_t)count < avail) ? (uint32_t)count : avail;

    /* #19/#645: every store lands in the caller's buffer (Ring-3 via
       sys_read), so the bracket is the copy loop. */
    {   uaccess_ac_t __ac = uaccess_begin();
        for (uint32_t i = 0; i < to_read; i++) {
            out[i] = ps->buf[ps->read_pos];
            ps->read_pos = (ps->read_pos + 1) % ps->size;
        }
        uaccess_end(__ac); }
    ps->count -= to_read;
    /* #111(a): space just appeared, so wake anyone blocked in pipe_write_fn().
     * Unconditional and done AFTER ps->count is published, exactly mirroring
     * the reader wake in pipe_write_fn(), so a writer that wakes always
     * observes the room. wake_up_all because several writers may share the
     * write end and each re-tests its own condition. */
    if (to_read > 0) wake_up_all(&ps->write_wq);
    return (int64_t)to_read;
}

/* Write to the pipe buffer.
 *
 * #111(a)(b). WHAT THIS USED TO DO AND WHY IT WAS WRONG, both halves MEASURED
 * on golden 1993 before the change:
 *
 *   (a) It wrote whatever fitted and returned that count, so a FULL ring
 *       returned 0. Zero is not a legal blocking-write result for a non-zero
 *       count, and userland/libc/stdio_file.c's flush_writes() loops on a
 *       short write, so a producer that outran its consumer span in userland.
 *       Measured: 4,616,023 zero-returns to push 256 KB through a 64 KB pipe,
 *       with the producer burning CPU at 97% of the rate of a deliberate busy
 *       loop. It was not a hang, it was a whole core spent on nothing.
 *
 *   (b) With no readers left it returned a bare -1 and raised nothing. A
 *       producer that CHECKS its write result stopped; one that does not,
 *       `yes` being the shipped example, looped forever, so `yes | head -1`
 *       never terminated.
 *
 * Both are fixed here, and they had to be fixed TOGETHER: a blocking write
 * whose reader can vanish without waking it is a permanent deadlock, which
 * would have been a worse bug than the spin it replaced. The reader-close
 * wake in pipe_release_read() is the other half of this function.
 *
 * Returns bytes written (possibly short, see rule 2 in rustkern/pipewr.rs),
 * or a negative errno. */
static int64_t pipe_write_fn(file_t *f, const void *buf, size_t count) {
    pipe_state_t *ps = (pipe_state_t *)f->priv;
    if (!ps) return -1;
    if (count == 0) return 0;

    const uint8_t *in = (const uint8_t *)buf;
    uint64_t done = 0;
    int32_t  intr = 0;

    /* Not a poll loop: every iteration either COPIES bytes (progress) or
     * BLOCKS on write_wq (a real sleep with two unconditional wake sources),
     * and the decision function has no path that returns PW_COPY with n == 0.
     * There is therefore no state in which this loop spins. */
    for (;;) {
        pipe_write_step_t st = pipe_write_step_rs((uint64_t)count, done,
                                                  ps->size, ps->count,
                                                  ps->readers, intr);
        switch (st.action) {

        case PW_DONE:
        case PW_SHORT:
            return (int64_t)st.n;

        case PW_EPIPE:
            /* #111(b). RAISING IS NOT DELIVERING, and this kernel has form:
             * #161 found SIGKILL was raised but never reached a busy app. It
             * works here because sys_write returns through syscall.asm's
             * return-work hook, which is where proc/signal.c actually decides
             * and takes the action. Default action for SIGPIPE is terminate
             * (proc/signal.c default_action(): SIGPIPE is not in the ignore
             * set), giving the conventional 128+13 = 141 exit code. A process
             * that installed SIG_IGN or a handler survives and gets -EPIPE
             * back instead, which is the POSIX behaviour and is what stops
             * this from being an unconditional kill. PROVEN both ways by
             * userland/apps/pipeprobe CHECK B and CHECK B2. */
            {   process_t *me = proc_current();
                if (me) sig_raise(me, SIGPIPE);   }
            return PIPE_EPIPE;

        case PW_EINTR:
            return PIPE_EINTR;

        case PW_BLOCK:
            /* #426/#514: a context that must not block gets the OLD partial
             * behaviour rather than a park that would deadlock it. This is a
             * degradation, not a fix, and it is deliberately reported as
             * EAGAIN (a retryable "would block") rather than 0, so it can
             * never be confused with the defect above. wq_assert_may_block()
             * does not catch a plain non-irqsave spinlock (its documented
             * gap), so this is an explicit check and not a reliance on it. */
            if (!wq_may_block())
                return done ? (int64_t)done : PIPE_EAGAIN;
            {   int rc = wait_event_interruptible(&ps->write_wq,
                             ps->count < ps->size || ps->readers <= 0);
                if (rc == WAIT_EINTR) intr = 1;   }
            break;

        case PW_COPY:
            /* #19/#645: every load comes from the caller's buffer (Ring-3 via
               sys_write), so the bracket is the copy loop. */
            {   uaccess_ac_t __ac = uaccess_begin();
                for (uint32_t i = 0; i < st.n; i++) {
                    ps->buf[ps->write_pos] = in[done + i];
                    ps->write_pos = (ps->write_pos + 1) % ps->size;
                }
                uaccess_end(__ac); }
            ps->count += st.n;
            done += st.n;
            /* #511: wake the reader. Unconditional on "we added bytes" and done
             * AFTER ps->count is published, so a reader that wakes always
             * observes the data. wake_up_all (not wake_up): more than one
             * reader may share the read end, and every waiter re-tests its own
             * condition, so a spurious wake is free. */
            wake_up_all(&ps->read_wq);
            break;

        default:
            /* Unreachable unless the Rust and C action numbering drift apart,
             * which the boot self-test exists to catch. Fail closed. */
            return done ? (int64_t)done : -1;
        }
    }
}

static void pipe_maybe_free(pipe_state_t *ps) {
    if (ps->readers <= 0 && ps->writers <= 0) {
        if (ps->buf) kfree(ps->buf);
        kfree(ps);
    }
}

// #695: a pipe buffer is memory, not a medium, so both releases report 0.
static int pipe_release_read(file_t *f) {
    pipe_state_t *ps = (pipe_state_t *)f->priv;
    if (!ps) return 0;
    ps->readers--;
    /* #111(a): THE WAKE THAT MAKES THE BLOCKING WRITE SAFE. A writer parked on
     * write_wq is waiting for space that a departed reader can never create,
     * so without this line the blocking write added by #111 would be a
     * permanent deadlock instead of the CPU burn it replaced. The woken writer
     * re-tests readers <= 0 and takes the EPIPE or short-count path.
     *
     * MUST precede pipe_maybe_free(), which kfree()s ps when the last
     * reference goes: touching ps->write_wq after that is a use-after-free.
     * Same ordering, and same reason, as pipe_release_write() below. */
    wake_up_all(&ps->write_wq);
    pipe_maybe_free(ps);
    return 0;
}

static int pipe_release_write(file_t *f) {
    pipe_state_t *ps = (pipe_state_t *)f->priv;
    if (!ps) return 0;
    ps->writers--;
    /* #511: this is the EOF wake, and it is what makes the reader's untimed
     * block safe: a reader parked on read_wq re-tests writers<=0 and returns a
     * REAL 0/EOF. It MUST happen before pipe_maybe_free(), which kfree()s ps
     * when the last reference goes: touching ps->read_wq after that would be a
     * use-after-free. (In that case there is no waiter anyway, since a waiter
     * implies an open read end, so this costs a lock and a NULL check.) */
    wake_up_all(&ps->read_wq);
    pipe_maybe_free(ps);
    return 0;
}

/* Create an anonymous pipe. On success, pipefd[0] = read end,
 * pipefd[1] = write end, returns 0. On failure returns -1. */
int pipe_create(int pipefd[2]) {
    pipe_state_t *ps = (pipe_state_t *)kmalloc(sizeof(pipe_state_t));
    if (!ps) return -1;

    ps->buf = (uint8_t *)kmalloc(PIPE_BUF_SIZE);
    if (!ps->buf) { kfree(ps); return -1; }

    ps->size      = PIPE_BUF_SIZE;
    ps->read_pos  = 0;
    ps->write_pos = 0;
    ps->count     = 0;
    ps->readers   = 1;
    ps->writers   = 1;
    /* #511: explicit init, NOT a memset/zero-fill. A zeroed spinlock is only
     * accidentally valid: SPINLOCK_INIT sets owner_cpu = 0xFFFFFFFF on a
     * debug build, which zero would misread as "CPU 0 holds this lock". */
    wait_queue_head_init(&ps->read_wq);
    wait_queue_head_init(&ps->write_wq);   /* #111(a): same reasoning */

    file_t *rf = file_alloc(&pipe_read_ops, ps, O_RDONLY);
    if (!rf) { kfree(ps->buf); kfree(ps); return -1; }

    file_t *wf = file_alloc(&pipe_write_ops, ps, O_WRONLY);
    // #695: pipe cleanup stays UNCONDITIONAL and its status is deliberately
    // dropped. A pipe release cannot fail (it flushes nothing), and making any
    // of these puts conditional would be a leak on one branch and a double free
    // on the other.
    if (!wf) { IGNORE_RESULT("pipe release flushes nothing and cannot fail; "
                             "a conditional put here would leak on one branch "
                             "and double-free on the other (#695)", file_put(rf));
                   return -1; }

    // #487/#349: pipes have no filesystem path; name them so the Task Manager
    // renders "pipe:[read]" rather than a blank handle (matching the way
    // Process Explorer names anonymous objects by type).
    file_set_path(rf, "pipe:[read]");
    file_set_path(wf, "pipe:[write]");

    int rfd = fd_alloc_install(rf);
    if (rfd < 0) { IGNORE_RESULT("pipe release cannot fail; unwind must be unconditional (#695)", file_put(rf));
                   IGNORE_RESULT("pipe release cannot fail; unwind must be unconditional (#695)", file_put(wf));
                   return -1; }

    int wfd = fd_alloc_install(wf);
    if (wfd < 0) { (void)fd_close(rfd);
                   IGNORE_RESULT("pipe release cannot fail; unwind must be unconditional (#695)", file_put(wf));
                   return -1; }

    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}
