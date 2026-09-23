/* randombytes_maytera.c - MayteraOS randombytes backend for libsodium.
 *
 * MayteraOS-authored translation unit, NOT upstream code. It REPLACES upstream
 * src/libsodium/randombytes/sysrandom/randombytes_sysrandom.c (excluded from
 * this port's build) while keeping the EXACT public symbol libsodium's
 * randombytes core resolves its default to,
 *     randombytes_sysrandom_implementation,
 * so sodium_init()/randombytes_buf() draw from the kernel CSPRNG with NO call to
 * randombytes_set_implementation() required. This is the decisive correctness
 * point of the port: libsodium's RNG MUST come from the one system CSPRNG
 * (kernel crypto/csprng.c, HMAC-DRBG), reached through the SYS_GETRANDOM syscall
 * via the libc getrandom() wrapper (userland/libc/sys/random.c).
 *
 * FAIL CLOSED: getrandom() over our CSPRNG never returns a short count for a
 * valid buffer, but if it EVER reports an error this backend abort()s rather
 * than handing libsodium non-random bytes. A crypto library must never silently
 * degrade its entropy source; that is exactly why libsodium was deferred until
 * a real userland CSPRNG existed.
 */
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/random.h>

#include "randombytes.h"
#include "randombytes_sysrandom.h"

static const char *
randombytes_maytera_implementation_name(void)
{
    return "maytera_getrandom";
}

static void
randombytes_maytera_buf(void * const buf, const size_t size)
{
    unsigned char *p = (unsigned char *) buf;
    size_t        off = 0;

    while (off < size) {
        ssize_t r = getrandom(p + off, size - off, 0);
        if (r < 0) {
            abort(); /* fail closed: never hand back non-random bytes */
        }
        off += (size_t) r;
    }
}

static uint32_t
randombytes_maytera_random(void)
{
    uint32_t v;

    randombytes_maytera_buf(&v, sizeof v);

    return v;
}

static void
randombytes_maytera_stir(void)
{
    /* The kernel HMAC-DRBG reseeds itself on a call-count / tick budget
     * (crypto/csprng.c); there is no userland entropy pool to stir. */
}

static int
randombytes_maytera_close(void)
{
    return 0;
}

struct randombytes_implementation randombytes_sysrandom_implementation = {
    .implementation_name = randombytes_maytera_implementation_name,
    .random              = randombytes_maytera_random,
    .stir                = randombytes_maytera_stir,
    .uniform             = NULL, /* libsodium supplies a uniform() over random() */
    .buf                 = randombytes_maytera_buf,
    .close               = randombytes_maytera_close
};
