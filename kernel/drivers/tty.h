// tty.h - TTY core + termios (Phase C1)
//
// A TTY is a bidirectional byte stream with a line discipline on the input
// side. Input comes in from a producer (keyboard, PTY master, serial UART)
// and is transformed by the ldisc before being read by the consumer
// (usually a user process via read(fd, ...)). Output bytes go the other
// direction, through a much simpler OPOST transformer.
//
// For MayteraOS MVP the only transport is the PTY (Phase F) which owns a
// pair of struct tty + struct pty_master. The GUI terminal and remote
// shell both open a PTY and then fork+exec a shell with the slave as
// stdin/stdout/stderr. There is no direct /dev/tty0 in MVP.

#ifndef DRIVERS_TTY_H
#define DRIVERS_TTY_H

#include "../types.h"
#include "../sync/waitq.h"

// --- termios (POSIX subset) --------------------------------------------------

typedef uint32_t tcflag_t;
typedef uint8_t  cc_t;
typedef uint32_t speed_t;

#define NCCS 20

// c_iflag bits
#define BRKINT  0x0002
#define ICRNL   0x0100
#define IGNBRK  0x0001
#define IGNCR   0x0080
#define IGNPAR  0x0004
#define INLCR   0x0040
#define INPCK   0x0010
#define ISTRIP  0x0020
#define IUTF8   0x4000
#define IXANY   0x0800
#define IXOFF   0x1000
#define IXON    0x0400
#define PARMRK  0x0008

// c_oflag bits
#define OPOST   0x0001
#define ONLCR   0x0004
#define OCRNL   0x0008
#define ONOCR   0x0010
#define ONLRET  0x0020

// c_cflag bits
#define CS8     0x0030
#define CSIZE   0x0030
#define CREAD   0x0080
#define HUPCL   0x0400
#define CLOCAL  0x0800

// c_lflag bits
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHOK   0x0020
#define ECHONL  0x0040
#define NOFLSH  0x0080
#define TOSTOP  0x0100
#define IEXTEN  0x8000

// c_cc indices
#define VINTR     0
#define VQUIT     1
#define VERASE    2
#define VKILL     3
#define VEOF      4
#define VTIME     5
#define VMIN      6
#define VSTART    8
#define VSTOP     9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VWERASE  14
#define VLNEXT   15

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

// TTY ioctls (Linux-ish numeric values so BusyBox is happy).
#define TCGETS       0x5401
#define TCSETS       0x5402
#define TCSETSW      0x5403
#define TCSETSF      0x5404
#define TIOCGPGRP    0x540F
#define TIOCSPGRP    0x5410
#define TIOCGWINSZ   0x5413
#define TIOCSWINSZ   0x5414
#define TIOCSCTTY    0x540E
#define TIOCNOTTY    0x5422
#define FIONREAD     0x541B

// --- struct tty --------------------------------------------------------------

#define TTY_BUF_SIZE 4096

typedef struct tty_ring {
    uint8_t  buf[TTY_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} tty_ring_t;

// Canonical-mode edit buffer: accumulates bytes between line terminators.
// When a LF/VEOL/VEOF arrives the whole line is dumped into the input_ring
// in one atomic commit so read() never returns a partial line.
#define TTY_EDIT_SIZE 1024
typedef struct tty_edit {
    uint8_t  buf[TTY_EDIT_SIZE];
    uint32_t len;
} tty_edit_t;

typedef struct tty {
    struct termios termios;
    struct winsize winsize;

    tty_ring_t input_ring;     // ldisc-cooked bytes ready for read()
    tty_ring_t output_ring;    // OPOST-cooked bytes ready for the transport
    tty_edit_t edit;           // canonical-mode line buffer

    wait_queue_head_t input_wq;
    wait_queue_head_t output_wq;

    uint32_t fg_pgrp;          // foreground process group (0 = none)
    uint32_t session;          // controlling session (0 = none)

    int hangup;                // transport gone; read returns 0, write -EIO
} tty_t;

// --- API ---------------------------------------------------------------------

// Initialize a TTY with POSIX default termios (cooked mode, 24x80, fg=0).
void tty_init(tty_t *t);

// Producer side (keyboard/PTY master/serial): feed one raw byte to the ldisc.
// May echo to output_ring, may signal fg pgrp (ISIG), may commit a line.
void tty_input_byte(tty_t *t, uint8_t byte);

// Consumer side (user read()): drain up to `count` bytes. Blocks until data
// or signal. Returns bytes read, 0 on hangup, -EINTR on signal.
int64_t tty_read(tty_t *t, void *buf, size_t count, int nonblock);

// Consumer side (user write()): enqueue into output_ring via OPOST. Returns
// bytes written. Blocks if output ring fills (not expected for MVP).
int64_t tty_write(tty_t *t, const void *buf, size_t count);

// Transport side: drain output bytes. Returns count drained.
size_t tty_output_drain(tty_t *t, void *buf, size_t max);

// ioctl dispatch. cmd is TCGETS etc.
int tty_ioctl(tty_t *t, unsigned cmd, void *arg);

// Notify the ldisc that the transport closed (PTY master close etc).
void tty_hangup(tty_t *t);

#endif // DRIVERS_TTY_H
