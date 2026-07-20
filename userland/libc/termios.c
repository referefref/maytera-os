// termios.c - POSIX terminal control wrappers
#include "termios.h"
#include "syscall.h"
#include "errno.h"

int tcgetattr(int fd, struct termios *t) {
    long r = syscall3(SYS_IOCTL, fd, TCGETS, (long)t);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

int tcsetattr(int fd, int actions, const struct termios *t) {
    unsigned long cmd = TCSETS + (unsigned long)actions; // 0x5402/0x5403/0x5404
    long r = syscall3(SYS_IOCTL, fd, cmd, (long)t);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

void cfmakeraw(struct termios *t) {
    t->c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
    t->c_cflag &= ~(CSIZE|PARENB);
    t->c_cflag |= CS8;
    t->c_cc[VMIN]  = 1;
    t->c_cc[VTIME] = 0;
}

speed_t cfgetispeed(const struct termios *t) { return t->c_ispeed; }
speed_t cfgetospeed(const struct termios *t) { return t->c_ospeed; }
int cfsetispeed(struct termios *t, speed_t s) { t->c_ispeed = s; return 0; }
int cfsetospeed(struct termios *t, speed_t s) { t->c_ospeed = s; return 0; }
int cfsetspeed(struct termios *t, speed_t s) {
    t->c_ispeed = s; t->c_ospeed = s; return 0;
}

int tcflush(int fd, int q) {
    (void)q;
    // Not implemented in kernel yet; request a no-op SETSF (flush-on-set) path via TCSETSF with zero termios.
    // For now, just succeed as a stub.
    (void)fd;
    return 0;
}

int tcdrain(int fd) { (void)fd; return 0; }
int tcsendbreak(int fd, int dur) { (void)fd; (void)dur; return 0; }
