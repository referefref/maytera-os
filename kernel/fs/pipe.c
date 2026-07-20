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
#include "vfs.h"

#define PIPE_BUF_SIZE  65536  /* 64KB ring buffer */

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
} pipe_state_t;

/* Forward declarations for file_ops */
static int64_t pipe_read_fn(file_t *f, void *buf, size_t count);
static int64_t pipe_write_fn(file_t *f, const void *buf, size_t count);
static void    pipe_release_read(file_t *f);
static void    pipe_release_write(file_t *f);

static const file_ops_t pipe_read_ops = {
    .read    = pipe_read_fn,
    .write   = NULL,
    .seek    = NULL,
    .ioctl   = NULL,
    .release = pipe_release_read,
    .poll    = NULL,
};

static const file_ops_t pipe_write_ops = {
    .read    = NULL,
    .write   = pipe_write_fn,
    .seek    = NULL,
    .ioctl   = NULL,
    .release = pipe_release_write,
    .poll    = NULL,
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

    for (uint32_t i = 0; i < to_read; i++) {
        out[i] = ps->buf[ps->read_pos];
        ps->read_pos = (ps->read_pos + 1) % ps->size;
    }
    ps->count -= to_read;
    return (int64_t)to_read;
}

/* Write to the pipe buffer. Returns bytes actually written (may be partial). */
static int64_t pipe_write_fn(file_t *f, const void *buf, size_t count) {
    pipe_state_t *ps = (pipe_state_t *)f->priv;
    if (!ps || count == 0) return 0;
    if (ps->readers <= 0) return -1;  /* Broken pipe: no readers */

    const uint8_t *in = (const uint8_t *)buf;
    uint32_t free_space = ps->size - ps->count;
    uint32_t to_write = ((uint32_t)count < free_space) ? (uint32_t)count : free_space;

    for (uint32_t i = 0; i < to_write; i++) {
        ps->buf[ps->write_pos] = in[i];
        ps->write_pos = (ps->write_pos + 1) % ps->size;
    }
    ps->count += to_write;
    /* #511: wake the reader. Unconditional on "we added bytes" and done AFTER
     * ps->count is published, so a reader that wakes always observes the data.
     * wake_up_all (not wake_up): more than one reader may share the read end,
     * and every waiter re-tests its own condition, so a spurious wake is free. */
    if (to_write > 0) wake_up_all(&ps->read_wq);
    return (int64_t)to_write;
}

static void pipe_maybe_free(pipe_state_t *ps) {
    if (ps->readers <= 0 && ps->writers <= 0) {
        if (ps->buf) kfree(ps->buf);
        kfree(ps);
    }
}

static void pipe_release_read(file_t *f) {
    pipe_state_t *ps = (pipe_state_t *)f->priv;
    if (!ps) return;
    ps->readers--;
    pipe_maybe_free(ps);
}

static void pipe_release_write(file_t *f) {
    pipe_state_t *ps = (pipe_state_t *)f->priv;
    if (!ps) return;
    ps->writers--;
    /* #511: this is the EOF wake, and it is what makes the reader's untimed
     * block safe: a reader parked on read_wq re-tests writers<=0 and returns a
     * REAL 0/EOF. It MUST happen before pipe_maybe_free(), which kfree()s ps
     * when the last reference goes: touching ps->read_wq after that would be a
     * use-after-free. (In that case there is no waiter anyway, since a waiter
     * implies an open read end, so this costs a lock and a NULL check.) */
    wake_up_all(&ps->read_wq);
    pipe_maybe_free(ps);
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

    file_t *rf = file_alloc(&pipe_read_ops, ps, O_RDONLY);
    if (!rf) { kfree(ps->buf); kfree(ps); return -1; }

    file_t *wf = file_alloc(&pipe_write_ops, ps, O_WRONLY);
    if (!wf) { file_put(rf); return -1; }

    // #487/#349: pipes have no filesystem path; name them so the Task Manager
    // renders "pipe:[read]" rather than a blank handle (matching the way
    // Process Explorer names anonymous objects by type).
    file_set_path(rf, "pipe:[read]");
    file_set_path(wf, "pipe:[write]");

    int rfd = fd_alloc_install(rf);
    if (rfd < 0) { file_put(rf); file_put(wf); return -1; }

    int wfd = fd_alloc_install(wf);
    if (wfd < 0) { fd_close(rfd); file_put(wf); return -1; }

    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}
