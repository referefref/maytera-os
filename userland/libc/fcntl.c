// fcntl.c
#include "fcntl.h"
#include "stdlib.h"
#include "syscall.h"
#include "errno.h"
#include <stdarg.h>

// open() is declared in stdlib.h and defined in stdlib.c (pre-existing).

int creat(const char *path, int mode) {
    (void)mode;
    return open(path, O_WRONLY | O_CREAT | O_TRUNC);
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    long r = syscall3(SYS_FCNTL, fd, cmd, arg);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}
