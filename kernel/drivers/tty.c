// tty.c - TTY core + line discipline (Phase C1)
//
// Implements a minimal POSIX-compatible TTY with cooked/raw modes, basic
// line editing (erase, kill, werase, eof), OPOST output transforms, ISIG
// Ctrl-C/Ctrl-\/Ctrl-Z delivery to the foreground pgrp, and TCGETS/TCSETS/
// TIOCGWINSZ/TIOCSWINSZ/TIOCGPGRP/TIOCSPGRP ioctls. Used by the PTY driver
// (Phase F); not attached to any hardware console yet.

#include "tty.h"
#include "../types.h"
#include "../proc/signal.h"

// From libc equivalents.
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);

// --- ring helpers ------------------------------------------------------------

static int ring_put(tty_ring_t *r, uint8_t b) {
    if (r->count >= TTY_BUF_SIZE) return 0;
    r->buf[r->head] = b;
    r->head = (r->head + 1) % TTY_BUF_SIZE;
    r->count++;
    return 1;
}

static int ring_get(tty_ring_t *r, uint8_t *out) {
    if (r->count == 0) return 0;
    *out = r->buf[r->tail];
    r->tail = (r->tail + 1) % TTY_BUF_SIZE;
    r->count--;
    return 1;
}

// --- init --------------------------------------------------------------------

void tty_init(tty_t *t) {
    memset(t, 0, sizeof(*t));

    // POSIX sane defaults: cooked mode, echo on, 9600 baud, 24x80.
    t->termios.c_iflag = ICRNL | BRKINT | IGNPAR;
    t->termios.c_oflag = OPOST | ONLCR;
    t->termios.c_cflag = CS8 | CREAD | HUPCL;
    t->termios.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    t->termios.c_cc[VINTR]    = 3;    // ^C
    t->termios.c_cc[VQUIT]    = 0x1c; // ^backslash
    t->termios.c_cc[VERASE]   = 0x7f; // DEL
    t->termios.c_cc[VKILL]    = 0x15; // ^U
    t->termios.c_cc[VEOF]     = 4;    // ^D
    t->termios.c_cc[VSTART]   = 0x11; // ^Q
    t->termios.c_cc[VSTOP]    = 0x13; // ^S
    t->termios.c_cc[VSUSP]    = 0x1a; // ^Z
    t->termios.c_cc[VEOL]     = 0;
    t->termios.c_cc[VREPRINT] = 0x12; // ^R
    t->termios.c_cc[VWERASE]  = 0x17; // ^W
    t->termios.c_cc[VLNEXT]   = 0x16; // ^V
    t->termios.c_cc[VMIN]     = 1;
    t->termios.c_cc[VTIME]    = 0;
    t->termios.c_ispeed = 9600;
    t->termios.c_ospeed = 9600;

    t->winsize.ws_row = 24;
    t->winsize.ws_col = 80;

    wait_queue_head_init(&t->input_wq);
    wait_queue_head_init(&t->output_wq);
}

// --- echo helper -------------------------------------------------------------

static void tty_echo(tty_t *t, uint8_t b) {
    if (!(t->termios.c_lflag & ECHO)) return;
    // Echo control chars as "^X" (except \n, \t, \r).
    if (b < 0x20 && b != '\n' && b != '\t' && b != '\r') {
        if (t->termios.c_lflag & ECHOE) {
            ring_put(&t->output_ring, '^');
            ring_put(&t->output_ring, b + '@');
            return;
        }
    }
    // OPOST translation for the echoed byte:
    if ((t->termios.c_oflag & OPOST) && b == '\n' &&
        (t->termios.c_oflag & ONLCR)) {
        ring_put(&t->output_ring, '\r');
        ring_put(&t->output_ring, '\n');
        return;
    }
    ring_put(&t->output_ring, b);
}

static void tty_erase_echo(tty_t *t) {
    // Visual erase: backspace, space, backspace.
    if (t->termios.c_lflag & (ECHO | ECHOE)) {
        ring_put(&t->output_ring, '\b');
        ring_put(&t->output_ring, ' ');
        ring_put(&t->output_ring, '\b');
    }
}

// Commit the edit buffer to the input ring (atomic line visible to reader).
static void tty_commit_line(tty_t *t) {
    for (uint32_t i = 0; i < t->edit.len; i++) {
        ring_put(&t->input_ring, t->edit.buf[i]);
    }
    t->edit.len = 0;
    wake_up(&t->input_wq);
}

// --- line discipline: one input byte -----------------------------------------

void tty_input_byte(tty_t *t, uint8_t byte) {
    struct termios *tm = &t->termios;

    // Input transforms first.
    if (tm->c_iflag & ISTRIP) byte &= 0x7f;
    if ((tm->c_iflag & INLCR) && byte == '\n') byte = '\r';
    else if ((tm->c_iflag & ICRNL) && byte == '\r') byte = '\n';
    else if ((tm->c_iflag & IGNCR) && byte == '\r') return;

    // ISIG: ^C -> SIGINT, ^\ -> SIGQUIT, ^Z -> SIGTSTP to fg pgrp.
    if (tm->c_lflag & ISIG) {
        if (byte == tm->c_cc[VINTR]) {
            if (t->fg_pgrp) sig_raise_pgrp(t->fg_pgrp, SIGINT);
            return;
        }
        if (byte == tm->c_cc[VQUIT]) {
            if (t->fg_pgrp) sig_raise_pgrp(t->fg_pgrp, SIGQUIT);
            return;
        }
        if (byte == tm->c_cc[VSUSP]) {
            if (t->fg_pgrp) sig_raise_pgrp(t->fg_pgrp, SIGTSTP);
            return;
        }
    }

    // Raw mode (non-ICANON): byte goes straight into input ring.
    if (!(tm->c_lflag & ICANON)) {
        if (ring_put(&t->input_ring, byte)) {
            if (tm->c_lflag & ECHO) tty_echo(t, byte);
            wake_up(&t->input_wq);
        }
        return;
    }

    // Canonical mode: accumulate into edit buffer with editing.
    if (byte == tm->c_cc[VERASE] || byte == '\b') {
        if (t->edit.len > 0) {
            t->edit.len--;
            tty_erase_echo(t);
        }
        return;
    }
    if (byte == tm->c_cc[VKILL]) {
        while (t->edit.len > 0) {
            t->edit.len--;
            tty_erase_echo(t);
        }
        return;
    }
    if (byte == tm->c_cc[VWERASE] && (tm->c_lflag & IEXTEN)) {
        // Erase preceding run of non-space, then run of space.
        while (t->edit.len > 0 && t->edit.buf[t->edit.len - 1] == ' ') {
            t->edit.len--;
            tty_erase_echo(t);
        }
        while (t->edit.len > 0 && t->edit.buf[t->edit.len - 1] != ' ') {
            t->edit.len--;
            tty_erase_echo(t);
        }
        return;
    }
    if (byte == tm->c_cc[VEOF]) {
        // EOF: commit whatever is in the edit buffer (may be empty -> EOF).
        tty_commit_line(t);
        return;
    }
    if (byte == '\n' || byte == tm->c_cc[VEOL]) {
        if (t->edit.len < TTY_EDIT_SIZE) {
            t->edit.buf[t->edit.len++] = '\n';
        }
        tty_echo(t, '\n');
        tty_commit_line(t);
        return;
    }

    // Normal char: append to edit buffer if room.
    if (t->edit.len < TTY_EDIT_SIZE - 1) {
        t->edit.buf[t->edit.len++] = byte;
        tty_echo(t, byte);
    }
}

// --- consumer side: read -----------------------------------------------------

int64_t tty_read(tty_t *t, void *buf, size_t count, int nonblock) {
    if (count == 0) return 0;
    uint8_t *out = (uint8_t *)buf;
    size_t got = 0;

    while (t->input_ring.count == 0) {
        if (t->hangup) return 0;  // EOF from transport
        if (nonblock) return -11; // -EAGAIN
        int rc = wait_event_interruptible(&t->input_wq,
                                          t->input_ring.count > 0 || t->hangup);
        if (rc == WAIT_EINTR) return -4;  // -EINTR
    }

    while (got < count && t->input_ring.count > 0) {
        uint8_t b;
        ring_get(&t->input_ring, &b);
        out[got++] = b;
        // In canonical mode, stop at LF so read() doesn't cross lines.
        if ((t->termios.c_lflag & ICANON) && b == '\n') break;
    }
    return (int64_t)got;
}

// --- consumer side: write ----------------------------------------------------

int64_t tty_write(tty_t *t, const void *buf, size_t count) {
    if (t->hangup) return -5;  // -EIO
    const uint8_t *in = (const uint8_t *)buf;
    size_t put = 0;
    while (put < count) {
        uint8_t b = in[put];
        int crlf = (t->termios.c_oflag & OPOST) && b == '\n' &&
                   (t->termios.c_oflag & ONLCR);
        int needed = crlf ? 2 : 1;

        if (t->output_ring.count + needed > TTY_BUF_SIZE) {
            // #442: this used to just `break` here, silently returning a
            // short (or zero) count once the ring filled -- no error, no
            // indication a retry might help. A fast producer (e.g. `cat`
            // streaming a large file through the SSHD exec channel's pty)
            // easily fills this 4096-byte ring faster than the pty master
            // side drains it over the network, and cat's own write() loop
            // treats a zero return as a hard failure and gives up. That is
            // the real cause of the SSHD exec channel's ~4KB output cap
            // (one layer below the TCP send-buffer fix in net/tcp.c). Block
            // for room instead, mirroring tty_read()'s existing wait on
            // input_wq; tty_output_drain() below wakes this once the reader
            // (ptym_read -> file_read(master,...)) makes space.
            if (t->hangup) return put > 0 ? (int64_t)put : -5;
            int rc = wait_event_interruptible(&t->output_wq,
                                              t->output_ring.count + needed <= TTY_BUF_SIZE ||
                                              t->hangup);
            if (rc == WAIT_EINTR) return put > 0 ? (int64_t)put : -4;  // -EINTR
            continue;  // re-check hangup/space at the top of the loop
        }

        if (crlf) {
            ring_put(&t->output_ring, '\r');
            ring_put(&t->output_ring, '\n');
        } else {
            ring_put(&t->output_ring, b);
        }
        put++;
    }
    if (put > 0) wake_up(&t->output_wq);
    return (int64_t)put;
}

size_t tty_output_drain(tty_t *t, void *buf, size_t max) {
    uint8_t *out = (uint8_t *)buf;
    size_t got = 0;
    while (got < max && t->output_ring.count > 0) {
        uint8_t b;
        ring_get(&t->output_ring, &b);
        out[got++] = b;
    }
    // #442: wake any tty_write() blocked waiting for room -- without this,
    // draining the ring never told a blocked writer that space opened back
    // up, so the write()-side wait above would need a spurious wake to ever
    // make progress.
    if (got > 0) wake_up(&t->output_wq);
    return got;
}

// --- ioctl -------------------------------------------------------------------

int tty_ioctl(tty_t *t, unsigned cmd, void *arg) {
    switch (cmd) {
        case TCGETS:
            if (!arg) return -1;
            memcpy(arg, &t->termios, sizeof(struct termios));
            return 0;
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
            if (!arg) return -1;
            memcpy(&t->termios, arg, sizeof(struct termios));
            if (cmd == TCSETSF) {
                // Flush pending input.
                t->input_ring.head = t->input_ring.tail = t->input_ring.count = 0;
                t->edit.len = 0;
            }
            return 0;
        case TIOCGWINSZ:
            if (!arg) return -1;
            memcpy(arg, &t->winsize, sizeof(struct winsize));
            return 0;
        case TIOCSWINSZ: {
            if (!arg) return -1;
            memcpy(&t->winsize, arg, sizeof(struct winsize));
            if (t->fg_pgrp) sig_raise_pgrp(t->fg_pgrp, SIGWINCH);
            return 0;
        }
        case TIOCGPGRP:
            if (!arg) return -1;
            *(uint32_t *)arg = t->fg_pgrp;
            return 0;
        case TIOCSPGRP:
            if (!arg) return -1;
            t->fg_pgrp = *(uint32_t *)arg;
            return 0;
        case FIONREAD:
            if (!arg) return -1;
            *(int *)arg = (int)t->input_ring.count;
            return 0;
        case TIOCSCTTY:
        case TIOCNOTTY:
            return 0;  // stub
    }
    return -25;  // -ENOTTY
}

void tty_hangup(tty_t *t) {
    t->hangup = 1;
    if (t->fg_pgrp) sig_raise_pgrp(t->fg_pgrp, SIGHUP);
    wake_up_all(&t->input_wq);
    wake_up_all(&t->output_wq);
}
