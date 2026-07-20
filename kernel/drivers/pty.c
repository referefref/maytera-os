// pty.c - Pseudo-terminal master/slave pairs (Phase F)
//
// Opening /dev/ptmx allocates a fresh pty pair:
//   - master:  the process that holds this fd drives the tty (writes keystrokes,
//              reads program output). Typically the GUI terminal or the
//              remote-shell pump loop.
//   - slave:   a struct tty with a line discipline; it is attached to the
//              user program (shell, vi, ...) via open("/dev/pts/N") and
//              dup2'd onto fds 0/1/2.
//
// Lifecycle:
//   - ptmx_open creates pair, registers "pts/N", returns master file_t
//   - open("/dev/pts/N") returns a slave file_t that references the same pair
//   - When master closes: slave gets hangup (SIGHUP to fg_pgrp + reads EOF)
//   - When last slave closes: master reads hit EOF
//   - When BOTH sides are gone, the pair + registration are torn down.
//
// Master read  = drain tty->output_ring (program output to terminal).
// Master write = for each byte: feed tty_input_byte (into ldisc).
// Slave  read  = tty_read (returns canonical lines or raw bytes).
// Slave  write = tty_write (adds to output_ring via OPOST).

#include "../types.h"
#include "../fs/vfs.h"
#include "../mm/heap.h"
#include "../proc/signal.h"
#include "../sync/waitq.h"
#include "../serial.h"
#include "tty.h"
#include "dev.h"

extern void *kmalloc(size_t);
extern void kfree(void *);
extern void kprintf(const char *, ...);
extern int dev_register(const char *name, struct file *(*open)(int flags));

#define MAX_PTY 8

typedef struct pty_pair {
    tty_t tty;              // the slave-side line discipline
    int   master_refs;      // 1 if master fd alive
    int   slave_refs;       // count of open slave fds
    int   index;            // "pts/N"
    char  name[16];         // "pts/N" storage
    int   in_use;
} pty_pair_t;

static pty_pair_t g_ptys[MAX_PTY];

// --- master fops -------------------------------------------------------------

static int64_t ptym_read(file_t *f, void *buf, size_t count) {
    pty_pair_t *p = (pty_pair_t *)f->priv;
    if (!p) return -1;
    if (count == 0) return 0;

    uint8_t *out = (uint8_t *)buf;
    size_t got = 0;
    // Wait for output bytes from the slave; if no slaves are attached AND
    // the output buffer is empty, return 0 (EOF).
    while (p->tty.output_ring.count == 0) {
        if (p->slave_refs == 0) return 0;
        if (f->flags & O_NONBLOCK) return -11;
        int rc = wait_event_interruptible(&p->tty.output_wq,
                                          p->tty.output_ring.count > 0 ||
                                          p->slave_refs == 0);
        if (rc == WAIT_EINTR) return -4;
    }
    got = tty_output_drain(&p->tty, out, count);
    return (int64_t)got;
}

static int64_t ptym_write(file_t *f, const void *buf, size_t count) {
    pty_pair_t *p = (pty_pair_t *)f->priv;
    if (!p) return -1;
    if (p->slave_refs == 0) {
        // Writing to a master whose slave is gone: report EIO.
        return -5;
    }
    const uint8_t *in = (const uint8_t *)buf;
    for (size_t i = 0; i < count; i++) {
        tty_input_byte(&p->tty, in[i]);
    }
    return (int64_t)count;
}

static int ptym_ioctl(file_t *f, unsigned cmd, void *arg) {
    pty_pair_t *p = (pty_pair_t *)f->priv;
    if (!p) return -1;
    // Forward TTY ioctls to the slave tty so the master can query winsize,
    // set pgrp, etc. Plus a tiny extension: TIOCGPTN returns the pts index.
    #define TIOCGPTN 0x80045430
    if (cmd == TIOCGPTN) {
        if (!arg) return -1;
        *(int *)arg = p->index;
        return 0;
    }
    return tty_ioctl(&p->tty, cmd, arg);
}

static int ptym_poll(file_t *f, int events) {
    pty_pair_t *p = (pty_pair_t *)f->priv;
    if (!p) return POLL_ERR;
    int r = 0;
    if ((events & POLL_IN) && p->tty.output_ring.count > 0) r |= POLL_IN;
    if (events & POLL_OUT) r |= POLL_OUT; // master-write is always ok
    if (p->slave_refs == 0) r |= POLL_HUP;
    return r;
}

static void ptym_release(file_t *f);

static const file_ops_t s_ptym_fops = {
    .read    = ptym_read,
    .write   = ptym_write,
    .seek    = NULL,
    .ioctl   = ptym_ioctl,
    .release = ptym_release,
    .poll    = ptym_poll,
};

// --- slave fops --------------------------------------------------------------

static int64_t ptys_read(file_t *f, void *buf, size_t count) {
    pty_pair_t *p = (pty_pair_t *)f->priv;
    if (!p) return -1;
    return tty_read(&p->tty, buf, count, (f->flags & O_NONBLOCK) != 0);
}

static int64_t ptys_write(file_t *f, const void *buf, size_t count) {
    pty_pair_t *p = (pty_pair_t *)f->priv;
    if (!p) return -1;
    int64_t rc = tty_write(&p->tty, buf, count);
    return rc;
}

static int ptys_ioctl(file_t *f, unsigned cmd, void *arg) {
    pty_pair_t *p = (pty_pair_t *)f->priv;
    if (!p) return -1;
    return tty_ioctl(&p->tty, cmd, arg);
}

static int ptys_poll(file_t *f, int events) {
    pty_pair_t *p = (pty_pair_t *)f->priv;
    if (!p) return POLL_ERR;
    int r = 0;
    if ((events & POLL_IN) && p->tty.input_ring.count > 0) r |= POLL_IN;
    if (events & POLL_OUT) r |= POLL_OUT; // slave-write (to output ring) always ok
    if (p->master_refs == 0 || p->tty.hangup) r |= POLL_HUP;
    return r;
}

static void pty_free_if_dead(pty_pair_t *p) {
    if (p->master_refs == 0 && p->slave_refs == 0) {
        p->in_use = 0;
        // Leave p->name[] as the registered string; dev_register doesn't
        // unregister, but that's fine: next open("/dev/pts/N") on a freed
        // slot goes via the factory which checks in_use.
    }
}

static void ptym_release(file_t *f) {
    pty_pair_t *p = (pty_pair_t *)f->priv;
    if (!p) return;
    p->master_refs = 0;
    // Wake slave readers so they see EOF.
    tty_hangup(&p->tty);
    pty_free_if_dead(p);
    f->priv = NULL;
}

static void ptys_release(file_t *f) {
    pty_pair_t *p = (pty_pair_t *)f->priv;
    if (!p) return;
    if (p->slave_refs > 0) p->slave_refs--;
    if (p->slave_refs == 0) {
        // Wake master reader so it sees EOF.
        wake_up_all(&p->tty.output_wq);
    }
    pty_free_if_dead(p);
    f->priv = NULL;
}

static const file_ops_t s_ptys_fops = {
    .read    = ptys_read,
    .write   = ptys_write,
    .seek    = NULL,
    .ioctl   = ptys_ioctl,
    .release = ptys_release,
    .poll    = ptys_poll,
};

// --- ptmx open factory -------------------------------------------------------

static file_t *pts_open_by_name(int idx, int flags);

// Forward declarations for dev.c registration.
static file_t *pts_open_0(int f) { return pts_open_by_name(0, f); }
static file_t *pts_open_1(int f) { return pts_open_by_name(1, f); }
static file_t *pts_open_2(int f) { return pts_open_by_name(2, f); }
static file_t *pts_open_3(int f) { return pts_open_by_name(3, f); }
static file_t *pts_open_4(int f) { return pts_open_by_name(4, f); }
static file_t *pts_open_5(int f) { return pts_open_by_name(5, f); }
static file_t *pts_open_6(int f) { return pts_open_by_name(6, f); }
static file_t *pts_open_7(int f) { return pts_open_by_name(7, f); }

static dev_open_fn s_pts_openers[MAX_PTY] = {
    pts_open_0, pts_open_1, pts_open_2, pts_open_3,
    pts_open_4, pts_open_5, pts_open_6, pts_open_7,
};
static const char *s_pts_names[MAX_PTY] = {
    "pts/0", "pts/1", "pts/2", "pts/3",
    "pts/4", "pts/5", "pts/6", "pts/7",
};

static file_t *pts_open_by_name(int idx, int flags) {
    if (idx < 0 || idx >= MAX_PTY) return NULL;
    pty_pair_t *p = &g_ptys[idx];
    if (!p->in_use) return NULL;
    file_t *f = file_alloc(&s_ptys_fops, p, flags);
    if (!f) return NULL;
    p->slave_refs++;
    return f;
}

static file_t *ptmx_open(int flags) {
    // Find a free slot.
    for (int i = 0; i < MAX_PTY; i++) {
        pty_pair_t *p = &g_ptys[i];
        if (!p->in_use) {
            tty_init(&p->tty);
            p->index = i;
            p->in_use = 1;
            p->master_refs = 1;
            p->slave_refs = 0;
            file_t *f = file_alloc(&s_ptym_fops, p, flags);
            if (!f) { p->in_use = 0; return NULL; }
            return f;
        }
    }
    return NULL;
}

// --- init --------------------------------------------------------------------

void pty_init(void) {
    for (int i = 0; i < MAX_PTY; i++) {
        g_ptys[i].in_use = 0;
        g_ptys[i].index = i;
    }
    dev_register("ptmx", ptmx_open);
    for (int i = 0; i < MAX_PTY; i++) {
        dev_register(s_pts_names[i], s_pts_openers[i]);
    }
    kprintf("[PTY] ptmx + %d slaves registered\n", MAX_PTY);
}
