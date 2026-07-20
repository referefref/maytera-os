// sys/ioctl.c
#include "ioctl.h"
#include "../syscall.h"
#include "../errno.h"
#include <stdarg.h>

int ioctl(int fd, unsigned long cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    long r = syscall3(SYS_IOCTL, fd, (long)cmd, (long)arg);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}
