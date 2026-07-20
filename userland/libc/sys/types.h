// sys/types.h - POSIX types for MayteraOS libc (#359 Phase 2).
// Complements the pre-existing libc types.h / unistd.h type set; every type is
// guarded so including this alongside those headers does not redefine anything.
#ifndef LIBC_SYS_TYPES_H
#define LIBC_SYS_TYPES_H

#include "../types.h"   // size_t, ssize_t, off_t, pid_t, intptr_t, uintptr_t

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

typedef unsigned long dev_t;
typedef unsigned long ino_t;
typedef unsigned int  nlink_t;
typedef long          blksize_t;
typedef long          blkcnt_t;
typedef unsigned int  useconds_t;
typedef long          suseconds_t;
typedef int           clockid_t;
typedef unsigned int  id_t;
typedef long          key_t;

#endif // LIBC_SYS_TYPES_H
