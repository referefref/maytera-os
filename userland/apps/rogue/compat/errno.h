
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

#endif
