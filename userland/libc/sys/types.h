// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/types.h - POSIX types for MayteraOS libc (#359 Phase 2).
// Complements the pre-existing libc types.h / unistd.h type set; every type is
// guarded so including this alongside those headers does not redefine anything.
// Each POSIX typedef is now INDIVIDUALLY guarded (task #581) so an application
// that ships its own <sys/types.h> compat shim (e.g. grep-gnu) can define the
// matching _<TYPE>_DEFINED macro and have its own typedef win inside its own
// translation unit, exactly the way a real libc lets app-side POSIX typedefs
// take precedence. This is ABI-safe: struct stat in <sys/stat.h> uses concrete
// widths (unsigned int st_nlink, ...), never these typedefs, so the typedef
// width is purely decorative.
#ifndef LIBC_SYS_TYPES_H
#define LIBC_SYS_TYPES_H

#include "../types.h"   // size_t, ssize_t, off_t, pid_t, intptr_t, uintptr_t

// BSD unsigned type aliases, added for the libedit port (#745): vis.c uses
// u_char and u_int32_t. Guarded so a consumer that already defines them wins.
#ifndef __BSD_U_TYPES_DEFINED
#define __BSD_U_TYPES_DEFINED 1
typedef unsigned char      u_char;
typedef unsigned short     u_short;
typedef unsigned int       u_int;
typedef unsigned long      u_long;
typedef unsigned char      u_int8_t;
typedef unsigned short     u_int16_t;
typedef unsigned int       u_int32_t;
typedef unsigned long long u_int64_t;
#endif

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
#ifndef _DEV_T_DEFINED
#define _DEV_T_DEFINED
typedef unsigned long dev_t;
#endif
#ifndef _INO_T_DEFINED
#define _INO_T_DEFINED
typedef unsigned long ino_t;
#endif
#ifndef _NLINK_T_DEFINED
#define _NLINK_T_DEFINED
typedef unsigned int  nlink_t;
#endif
#ifndef _BLKSIZE_T_DEFINED
#define _BLKSIZE_T_DEFINED
typedef long          blksize_t;
#endif
#ifndef _BLKCNT_T_DEFINED
#define _BLKCNT_T_DEFINED
typedef long          blkcnt_t;
#endif
#ifndef _USECONDS_T_DEFINED
#define _USECONDS_T_DEFINED
typedef unsigned int  useconds_t;
#endif
#ifndef _SUSECONDS_T_DEFINED
#define _SUSECONDS_T_DEFINED
typedef long          suseconds_t;
#endif
#ifndef _CLOCKID_T_DEFINED
#define _CLOCKID_T_DEFINED
typedef int           clockid_t;
#endif
#ifndef _ID_T_DEFINED
#define _ID_T_DEFINED
typedef unsigned int  id_t;
#endif
#ifndef _KEY_T_DEFINED
#define _KEY_T_DEFINED
typedef long          key_t;
#endif

// BSD short-name integer typedefs (#745 darkhttpd port). Individually guarded
// like the POSIX types above so an app carrying its own <sys/types.h> shim can
// still win. darkhttpd casts sin_family to (u_char).
#ifndef _U_CHAR_DEFINED
#define _U_CHAR_DEFINED
typedef unsigned char      u_char;
#endif
#ifndef _U_SHORT_DEFINED
#define _U_SHORT_DEFINED
typedef unsigned short     u_short;
#endif
#ifndef _U_INT_DEFINED
#define _U_INT_DEFINED
typedef unsigned int       u_int;
#endif
#ifndef _U_LONG_DEFINED
#define _U_LONG_DEFINED
typedef unsigned long      u_long;
#endif

#endif // LIBC_SYS_TYPES_H
