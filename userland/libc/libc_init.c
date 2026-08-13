// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// libc_init.c - libc startup / shutdown hooks
#include "stdio.h"

// AssaultCube bring-up (2026-07): run C++ global/static dynamic initializers
// (.init_array) before main(). user.ld now emits __init_array_start/_end
// around a real .init_array output section (previously absent, so the
// section was silently orphan-placed and never executed - see user.ld for
// the full rationale). This is a no-op for every existing C app: they have
// zero .init_array entries, so start == end and the loop below runs zero
// times. It is load-bearing for any C++ port whose globals have dynamic
// initializers, e.g. AssaultCube's COMMAND/VAR/VARP console-registration
// macros (source/src/command.h), which expand to exactly that pattern.
typedef void (*mos_init_func_t)(void);
extern mos_init_func_t __init_array_start[];
extern mos_init_func_t __init_array_end[];

// #651: userland stack protector. Seed the guard BEFORE anything else so it
// is live before any guarded function can return. See stack_guard.c for the
// entropy caveat (interim: rdtsc + ASLR'd addresses, not the kernel CSPRNG -
// there is no getrandom syscall yet).
void __mos_stack_guard_init(void);

void __libc_init(void) {
    __mos_stack_guard_init();
    __stdio_init();

    for (mos_init_func_t *f = __init_array_start; f < __init_array_end; f++) {
        (*f)();
    }
}

void __libc_fini(void) {
    fflush(0);
}
