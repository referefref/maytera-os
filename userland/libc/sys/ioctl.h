// sys/ioctl.h
#ifndef LIBC_SYS_IOCTL_H
#define LIBC_SYS_IOCTL_H

#include "../termios.h"

int ioctl(int fd, unsigned long cmd, ...);

#endif
