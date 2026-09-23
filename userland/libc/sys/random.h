// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sys/random.h - getrandom(2) / getentropy(3) over the kernel CSPRNG.
//
// Both draw from the kernel HMAC-DRBG (crypto/csprng.c) through SYS_GETRANDOM.
// There is no userland RNG fallback and no second PRNG: the ONE cryptographic
// random source in the system is the kernel CSPRNG, per the "never hand-roll a
// second RNG" rule. libsodium's randombytes is wired to getrandom() (see
// userland/ports/libsodium).
#ifndef _SYS_RANDOM_H
#define _SYS_RANDOM_H

#include "../types.h"   /* size_t, ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

/* getrandom() flags. Our CSPRNG is always seeded and never blocks, so these are
 * accepted for source compatibility but do not change behaviour: GRND_RANDOM
 * and GRND_NONBLOCK both return the same bytes from the same DRBG. Unknown bits
 * are rejected with EINVAL, matching Linux getrandom(2). */
#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002
#define GRND_INSECURE 0x0004

/* Fill up to len bytes of buf with cryptographically secure random bytes.
 * Returns the number of bytes written (== len on success), or -1 with errno set
 * (EINVAL on a bad flag or an oversized request, EFAULT on a bad buffer). */
ssize_t getrandom(void *buf, size_t len, unsigned int flags);

/* Fill exactly len bytes (len <= 256) with secure random bytes. Returns 0 on
 * success, -1 with errno set (EINVAL if len > 256). */
int getentropy(void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_RANDOM_H */
