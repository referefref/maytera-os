
#ifndef COMPAT_ERRNO_H
#define COMPAT_ERRNO_H

static int _errno_val = 0;
#define errno _errno_val

#define ENOENT   2
#define EACCES  13
#define EEXIST  17
#define ENOSPC  28
#define EROFS   30
#define ENOSYS  38


/* strerror lives in the shared libc (errno.c) but the real libc errno.h is
 * shadowed by this compat shim on the app include path, and libc string.h
 * does not declare it. Without a prototype strerror is implicitly int, which
 * truncates its char* return to 32 bits; passed to a %s it faults on deref
 * (open_score() launch crash, RIP image+0x262ef). Declare it here so both
 * mach_dep.c and save.c (which include <errno.h>) get the real 64-bit pointer. */
char *strerror(int err);

#endif
