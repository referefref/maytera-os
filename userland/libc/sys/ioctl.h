// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/ioctl.h
#ifndef LIBC_SYS_IOCTL_H
#define LIBC_SYS_IOCTL_H

#include "../termios.h"

int ioctl(int fd, unsigned long cmd, ...);

#endif
