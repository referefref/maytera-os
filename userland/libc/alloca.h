// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// alloca.h - stack allocation for MayteraOS userland.
//
// Header only. alloca is a gcc intrinsic that expands to a stack-pointer
// adjustment, so there is no libc object behind it and it cannot be missing at
// link time. It is provided because a great deal of portable C includes this
// header unconditionally, not because it is encouraged: a MayteraOS user
// process gets USER_STACK_SIZE (2 MB) of stack with no guard page below it,
// so an alloca of attacker-influenced size runs off the end silently. Size it
// from a constant, or use malloc.
#ifndef LIBC_ALLOCA_H
#define LIBC_ALLOCA_H

#include <stddef.h>

#ifndef alloca
#define alloca(size) __builtin_alloca(size)
#endif

#endif // LIBC_ALLOCA_H
