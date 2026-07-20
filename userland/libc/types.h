// types.h - Basic type definitions for MayteraOS libc
// This header provides types when system headers are not available,
// but defers to system headers when they are included.
#ifndef _TYPES_H
#define _TYPES_H

// Null pointer
#ifndef NULL
#define NULL ((void *)0)
#endif

// Boolean
#ifndef __cplusplus
#ifndef __bool_true_false_are_defined
typedef _Bool bool;
#define true 1
#define false 0
#define __bool_true_false_are_defined 1
#endif
#endif

// Fixed-width integer types (only define if not already defined by stdint.h)
#ifndef _STDINT_H
#ifndef __int8_t_defined
typedef signed char         int8_t;
typedef unsigned char       uint8_t;
typedef signed short        int16_t;
typedef unsigned short      uint16_t;
typedef signed int          int32_t;
typedef unsigned int        uint32_t;
typedef signed long         int64_t;
typedef unsigned long       uint64_t;
#define __int8_t_defined 1
#endif
#endif

// Size types (only define if not already defined by stddef.h)
#ifndef _STDDEF_H
#ifndef __size_t_defined
typedef unsigned long       size_t;
typedef signed long         ssize_t;
typedef signed long         ptrdiff_t;
#define __size_t_defined 1
#endif
#endif

typedef unsigned long       uintptr_t;
typedef signed long         intptr_t;

// File offset type
typedef signed long long    off_t;

// Process ID type
typedef int                 pid_t;

#endif // _TYPES_H
