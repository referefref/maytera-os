
#ifndef COMPAT_FCNTL_H
#define COMPAT_FCNTL_H

#include "../../../libc/syscall.h"

#ifndef O_RDONLY
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0040
#define O_TRUNC  0x0200
#define O_APPEND 0x0400
#endif

/* MayteraOS port note: open() used to be duplicated here as a static
 * inline wrapper around SYS_OPEN that silently dropped the mode argument.
 * The shared libc (libc/stdlib.c) now provides a real
 * int open(const char *, int, ...) (declared in libc/stdlib.h) that also
 * propagates the kernel's negative errno (#359). Keeping this second,
 * conflicting declaration broke the build ("static declaration of 'open'
 * follows non-static declaration") once rip.c pulled in both headers.
 * Drop the duplicate and use the libc one. */

#endif
