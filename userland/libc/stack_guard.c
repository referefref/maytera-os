// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// stack_guard.c - userland stack-protector runtime (#651)
//
// Before this file, userland had NO stack protector at all: every one of the
// 142 app Makefiles carried -fno-stack-protector, and libc defined neither
// __stack_chk_guard nor __stack_chk_fail. Flipping the compiler flag without
// this file would fail to link every app.
//
// WHY -mstack-protector-guard=global AND NOT gcc's DEFAULT:
// gcc defaults to reading the canary from the TLS block at %fs:0x28. MayteraOS
// userland does not set up a TLS block or an %fs base for app processes, so the
// default lowering would dereference an unmapped address on entry to every
// guarded function. The global variant reads a plain global instead, which is
// why the app Makefiles must pass -mstack-protector-guard=global alongside
// -fstack-protector-strong.
//
// ENTROPY (#654). The guard is seeded from the kernel CSPRNG, mixed with
// locally observable variation:
//
//   0. 8 bytes from /dev/urandom  - the kernel HMAC-DRBG (crypto/csprng.c),
//                                   the same source the password salts use
//   1. rdtsc                      - varies per boot and per process start
//   2. the address of a function  - the image base is ASLR-randomized as of
//                                   #640, ~9 bits, different every spawn
//   3. the address of a local     - stack placement
//
// CORRECTION to what #651 shipped, recorded because the mistake is instructive.
// The comment here used to say the kernel CSPRNG was "not exposed to Ring 3"
// and that a getrandom syscall was needed first. That was wrong: /dev/urandom
// has been backed by the DRBG since #427 (drivers/dev.c devurandom_read ->
// csprng_bytes), and CPython's launcher already read it. I searched for a
// syscall, found none, and concluded the capability was absent instead of
// checking whether it was reachable another way.
//
// 0 is XORed into 1-3 rather than replacing them. /dev/urandom can legitimately
// be unavailable (a process spawned before dev_init, or an image with no /dev),
// and on that path the guard must degrade to the #651 behaviour rather than to
// a zeroed buffer. When it IS available, the extra terms cost nothing and mean
// no single source is a point of failure.
//
// The low byte is forced to 0x00 on purpose. That is the standard "NUL byte"
// trick: a canary containing a NUL terminates str*() copies, so an overflow
// driven through a string function cannot rewrite the canary with its own
// correct value.

#include <stdint.h>
#include "stdlib.h"     // write()
#include "unistd.h"     // _exit(), read(), close()
#include "fcntl.h"      // O_RDONLY
#include "syscall.h"    // sys_bootlog(): see __stack_chk_fail() below

// The symbol gcc emits references to under -mstack-protector-guard=global.
// Must NOT be static, and must not be const.
uintptr_t __stack_chk_guard = 0;

static inline uint64_t sg_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

// Called from __libc_init() in libc_init.c, which crt0.S really does call.
// (Note for anyone auditing: userland/libc/crt0.asm is DEAD - the Makefile
// builds crt0.o from crt0.S. crt0.asm still has its _init_heap call commented
// out, which reads as "libc init never runs". It does run; read crt0.S.)
// Draw 8 bytes from the kernel CSPRNG. Returns 0 if unavailable, which is a
// legitimate outcome (no /dev yet), not an error worth reporting: the caller
// XORs the result in, so 0 simply contributes nothing.
static uint64_t sg_kernel_random(void) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;

    unsigned char buf[8];
    uint64_t out = 0;
    long got = read(fd, buf, sizeof(buf));
    close(fd);

    // Tolerate a short read: contribute whatever arrived. No retry loop - a
    // canary is not worth spinning for at every single process start, and the
    // TSC/address terms below are an adequate floor.
    if (got > 0) {
        if (got > (long)sizeof(buf)) got = (long)sizeof(buf);
        for (long i = 0; i < got; i++) out |= (uint64_t)buf[i] << (i * 8);
    }
    return out;
}

void __mos_stack_guard_init(void) {
    uint64_t local;                       // address = stack placement
    uint64_t v = sg_rdtsc();

    v ^= sg_kernel_random();                             // kernel HMAC-DRBG
    v ^= (uint64_t)(uintptr_t)&local;                    // stack address
    v ^= (uint64_t)(uintptr_t)&__mos_stack_guard_init;   // ASLR'd image base
    v *= 0x9E3779B97F4A7C15ULL;                          // mix (splitmix64)
    v ^= v >> 30;
    v *= 0xBF58476D1CE4E5B9ULL;
    v ^= v >> 27;
    v *= 0x94D049BB133111EBULL;
    v ^= v >> 31;

    v &= ~0xFFULL;                        // NUL low byte, see header comment
    if (v == 0) v = 0xD15EA5E00ULL & ~0xFFULL;   // never leave it zero
    __stack_chk_guard = (uintptr_t)v;
    (void)local;
}

// gcc calls this when a canary check fails. It must not return.
void __stack_chk_fail(void) {
    // Deliberately minimal and dependency-free: the stack is already known to
    // be corrupt, so calling into printf (which formats into stack buffers)
    // could fault before the message lands. Write straight to fd 2 and exit.
    static const char msg[] =
        "*** stack smashing detected: __stack_chk_fail, terminating ***\n";
    write(2, msg, sizeof(msg) - 1);
    // #307: AND SAY IT SOMEWHERE THAT SURVIVES A MACHINE WITH NO SERIAL PORT.
    //
    // fd 2 for a process with no stdout falls through to the kernel console,
    // i.e. serial. The owner's iMac14,4 has no serial port, so on the one
    // machine where this fires in the field the message above goes nowhere.
    // Measured there on golden 2039: 'COMPOSIT' exited with code 127 twice in
    // 15.6 hours, and /BOOTLOG.TXT could say only "code 127" - which in this
    // libc means a canary trip HERE, an assert()/abort(), or a Rust
    // panic_handler, three different bugs sharing one number.
    //
    // sys_bootlog() is a static-inline syscall1 over a string constant: no
    // formatting, no stack buffer, no allocation, nothing this already-corrupt
    // stack has to survive. It runs AFTER the write above so the serial path is
    // never made worse, and its failure is ignored because there is nothing
    // sensible left to do about it.
    (void)sys_bootlog("libc: STACK SMASHING DETECTED (__stack_chk_fail); "
                      "process terminating with exit 127");
    _exit(127);
    for (;;) { }                          // _exit does not return; belt and braces
}

// Some gcc configurations emit __stack_chk_fail_local for -fpic code.
void __stack_chk_fail_local(void) { __stack_chk_fail(); }
