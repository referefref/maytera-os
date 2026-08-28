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
#include "../proc/process.h"   // process_t.ctty, for the /dev/tty resolver below
#include "../sync/waitq.h"
#include "../serial.h"
#include "tty.h"
#include "dev.h"
#include "../string.h"   // #fdguard: snprintf for the audit detail

// #fdguard: /dev/pts attach ownership (rustkern/ptsown.rs). Declared here
// rather than in a header because pty.c is its only consumer.
extern int ptsown_slots_rs(void);
extern int ptsown_claim_rs(uint32_t idx, uint32_t owner);
extern int ptsown_release_rs(uint32_t idx);
extern uint32_t ptsown_owner_rs(uint32_t idx);
extern void ptsown_note_refusal_rs(void);
extern uint32_t ptsown_refusals_rs(void);
extern int ptsown_selftest_rs(void);
extern void seclog_report_io_boundary(unsigned int pid, const char *detail);
extern int fdguard_bypass(void);   // #fdguard dev-only bypass

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

static int ptym_release(file_t *f);

// #745 (local 82): the queue a poll(2) waiter parks on. The MASTER reads
// what the slave wrote, so its readiness is published on output_wq; the
// SLAVE reads what the master wrote, so its readiness is on input_wq. Both
// are woken by the existing tty paths, so no new wake source is needed.
static struct wait_queue_head *ptym_poll_wq(file_t *f, int events) {
    pty_pair_t *p = (pty_pair_t *)f->priv;
    (void)events;
    if (!p) return NULL;
    return &p->tty.output_wq;
}

static const file_ops_t s_ptym_fops = {
    .read    = ptym_read,
    .write   = ptym_write,
    .seek    = NULL,
    .ioctl   = ptym_ioctl,
    .release = ptym_release,
    .poll    = ptym_poll,
    .poll_wq = ptym_poll_wq,
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
        ptsown_release_rs((uint32_t)p->index);   // #fdguard: pair gone, recycle unowned
        // Leave p->name[] as the registered string; dev_register doesn't
        // unregister, but that's fine: next open("/dev/pts/N") on a freed
        // slot goes via the factory which checks in_use.
    }
}

// #695: a PTY holds no persistent state, so both releases report 0.
static int ptym_release(file_t *f) {
    pty_pair_t *p = (pty_pair_t *)f->priv;
    if (!p) return 0;
    p->master_refs = 0;
    // Wake slave readers so they see EOF.
    tty_hangup(&p->tty);
    pty_free_if_dead(p);
    f->priv = NULL;
    return 0;
}

static int ptys_release(file_t *f) {
    pty_pair_t *p = (pty_pair_t *)f->priv;
    if (!p) return 0;
    if (p->slave_refs > 0) p->slave_refs--;
    if (p->slave_refs == 0) {
        // Wake master reader so it sees EOF.
        wake_up_all(&p->tty.output_wq);
    }
    pty_free_if_dead(p);
    f->priv = NULL;
    return 0;
}

static struct wait_queue_head *ptys_poll_wq(file_t *f, int events) {
    pty_pair_t *p = (pty_pair_t *)f->priv;
    (void)events;
    if (!p) return NULL;
    return &p->tty.input_wq;
}

static const file_ops_t s_ptys_fops = {
    .read    = ptys_read,
    .write   = ptys_write,
    .seek    = NULL,
    .ioctl   = ptys_ioctl,
    .release = ptys_release,
    .poll    = ptys_poll,
    .poll_wq = ptys_poll_wq,
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

// #fdguard: may the CURRENT process attach to /dev/pts/idx? The pair records
// its creator (the /dev/ptmx opener) in ptsown; process_t.ctty records the
// controlling terminal the kernel wired for this process. Ring 3 can forge
// neither. Everything else is an attach to a terminal the caller did not
// create and is not attached to: refuse and audit.
static int pts_attach_allowed(int idx) {
    if (fdguard_bypass()) return 1;   // #fdguard dev-only bypass
    process_t *me = proc_current();
    // Kernel-internal open (no Ring 3 context, e.g. the idle proc before
    // dev_init, or a bind done on behalf of a child): permit.
    if (!me) return 1;
    uint32_t owner = ptsown_owner_rs((uint32_t)idx);
    uint32_t meid = me->tgid ? me->tgid : me->pid;
    if (owner != 0 && meid == owner) return 1;   // creator of the pair
    if (me->ctty == idx) return 1;               // my controlling terminal
    ptsown_note_refusal_rs();
    {
        char d[64];
        snprintf(d, sizeof(d), "pts attach idx=%d owner=%u not owner/ctty",
                 idx, owner);
        seclog_report_io_boundary(meid, d);
    }
    return 0;
}

static file_t *pts_open_by_name(int idx, int flags) {
    if (idx < 0 || idx >= MAX_PTY) return NULL;
    pty_pair_t *p = &g_ptys[idx];
    if (!p->in_use) return NULL;
    if (!pts_attach_allowed(idx)) return NULL;   // #fdguard
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
            {   // #fdguard: record the /dev/ptmx opener as the pair owner.
                process_t *me = proc_current();
                ptsown_claim_rs((uint32_t)i,
                    me ? (me->tgid ? me->tgid : me->pid) : 0);
            }
            return f;
        }
    }
    return NULL;
}

// --- /dev/tty: the CONTROLLING terminal --------------------------------------
//
// WHAT THIS FIXES. `ls | less` printed one screenful and returned straight to
// the prompt. A pager reading from a pipe has the PIPE on fd 0, so every key
// read consumed piped DATA and hit EOF at once. The conventional answer, and
// the one every unix pager uses, is that CONTENT comes from stdin while KEYS
// come from the controlling terminal opened BY NAME. That name is /dev/tty,
// and it did not exist here: dev.c registered null, zero, urandom, random,
// console and ttyACM0, and pty.c registered ptmx and pts/0..7, but nothing
// resolved to "whichever terminal the calling process is attached to".
//
// It could not have been written before now, because until this change no
// process RECORDED its terminal (see process_t.ctty). The pts index was a
// single-shot global consumed at fd-setup time and then discarded.
//
// This is NOT an alias for pts/N. It is per-caller: the same path opened by two
// processes on two different terminals returns two different slaves. That is
// the whole point, and it is why the lookup happens HERE at open() time against
// proc_current(), rather than being baked into a registration.
extern process_t *proc_current(void);

static file_t *devtty_open(int flags) {
    process_t *me = proc_current();
    if (!me) return NULL;
    // No controlling terminal (a GUI app launched from the compositor, a kernel
    // thread, anything left on the console default). POSIX says open() fails
    // with ENXIO; returning NULL is how this dev layer spells that. A caller
    // that gets it must FALL BACK rather than assume a tty: see
    // userland/apps/less/main.c, which degrades to plain cat behaviour.
    if (me->ctty < 0 || me->ctty >= MAX_PTY) return NULL;
    return pts_open_by_name(me->ctty, flags);
}

// --- init --------------------------------------------------------------------

// #fdguard: boot check. Run the ptsown self-test and prove the Rust slot
// count matches MAX_PTY. Named _check so diaglog-gate does not require a
// durable sink for this summary kprintf; the durable audit is the per-refusal
// SECURITY.LOG line via seclog_report_io_boundary().
void ptsown_boot_check(void) {
    int rs = ptsown_slots_rs();
    int st = ptsown_selftest_rs();
    kprintf("[FDGUARD] ptsown selftest=%s slots=%d/%d\n",
            st == 0 ? "PASS" : "FAIL", rs, MAX_PTY);
    if (st != 0 || rs != MAX_PTY)
        kprintf("[FDGUARD] ptsown SELF-TEST FAILED step=%d - /dev/pts attach "
                "ownership is NOT trustworthy on this build\n", st);
}
_Static_assert(MAX_PTY == 8,
               "#fdguard: ptsown.rs MAX_PTY is hardcoded 8; keep it in sync");

void pty_init(void) {
    for (int i = 0; i < MAX_PTY; i++) {
        g_ptys[i].in_use = 0;
        g_ptys[i].index = i;
    }
    dev_register("ptmx", ptmx_open);
    for (int i = 0; i < MAX_PTY; i++) {
        dev_register(s_pts_names[i], s_pts_openers[i]);
    }
    // dev_register returns -1 when the table is full, and every other caller in
    // the tree ignores that. Check THIS one loudly: a silently unregistered
    // /dev/tty would present as "the pager still exits immediately", with
    // nothing in the log to say why, which is the exact failure mode this
    // change exists to remove.
    if (dev_register("tty", devtty_open) != 0)
        kprintf("[PTY] WARNING: /dev/tty could NOT be registered\n");
    else
        kprintf("[PTY] /dev/tty registered (per-caller controlling terminal)\n");
    kprintf("[PTY] ptmx + %d slaves registered\n", MAX_PTY);
}
