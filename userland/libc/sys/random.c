// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/random.c - getrandom()/getentropy() over SYS_GETRANDOM (kernel HMAC-DRBG,
// crypto/csprng.c). No userland fallback and no second PRNG by design: the one
// cryptographic random source is the kernel CSPRNG.
#include "random.h"
#include "../syscall.h"
#include "../errno.h"

ssize_t getrandom(void *buf, size_t len, unsigned int flags) {
    long r = syscall3(SYS_GETRANDOM, (long)buf, (long)len, (long)flags);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (ssize_t)r;
}

int getentropy(void *buf, size_t len) {
    if (len > 256) { errno = EINVAL; return -1; }
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t r = getrandom(p + got, len - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) { errno = EIO; return -1; }
        got += (size_t)r;
    }
    return 0;
}
