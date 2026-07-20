/* sys/types.h - compat for MayteraOS. Pull in the base libc types and add a
 * few POSIX typedefs that grep/gnulib expect. */
#ifndef COMPAT_SYS_TYPES_H
#define COMPAT_SYS_TYPES_H

#include <stddef.h>
#include <types.h>   /* MayteraOS libc base: size_t, pid_t, ... */

/* libc/types.h guards size/ssize/off behind _STDDEF_H; gcc's <stddef.h>
 * trips that guard, so define the POSIX ones we need explicitly here.
 * (Duplicate typedefs of identical type are permitted in C11/GNU C.) */
typedef long ssize_t;
typedef long long off_t;

#ifndef _MODE_T_DEFINED
#define _MODE_T_DEFINED
typedef unsigned int mode_t;
#endif
#ifndef _UID_T_DEFINED
#define _UID_T_DEFINED
typedef unsigned int uid_t;
#endif
#ifndef _GID_T_DEFINED
#define _GID_T_DEFINED
typedef unsigned int gid_t;
#endif

typedef unsigned long ino_t;
typedef unsigned long dev_t;
typedef unsigned long nlink_t;
typedef long          time_t;
typedef long          blksize_t;
typedef long          blkcnt_t;

#endif /* COMPAT_SYS_TYPES_H */
