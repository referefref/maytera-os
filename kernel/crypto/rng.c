// rng.c - compatibility shim over the audited HMAC-DRBG (crypto/csprng.c).
//
// #tls-rngfix: WHAT THIS FILE USED TO BE, AND WHY IT IS NOT THAT ANY MORE.
//
// This was a second, weaker CSPRNG. It used RDRAND directly when CPUID
// advertised it, and otherwise fell back to a 256-byte entropy pool seeded from
// exactly two things at boot, `timer_ticks` and one `rdtsc`, stirred with an
// XOR-and-rotate and squeezed through SHA-256. Two facts made that serious:
//
//   1. THE FALLBACK WAS THE LIVE PATH ON EVERY MayteraOS VM. CLAUDE.md requires
//      cpu=kvm64 (cpu=host crashes the compositor with AVX), and kvm64 does not
//      expose RDRAND. So the machines used for all development and testing ran
//      on the weak pool, always.
//   2. TLS EPHEMERAL KEYS CAME FROM HERE. net/tls/tls13.c drew the X25519
//      private key from rng_get_bytes(), so session confidentiality rested on
//      that pool. See the comment on x25519_generate_keypair().
//
// crypto/csprng.c already existed, already backed /dev/urandom, ASLR and
// password salts, and already gathers RDSEED, RDRAND, RDTSC jitter, tick counts
// and address-layout entropy through a real HMAC-DRBG. The entropy work had
// been done and TLS had simply never been moved onto it.
//
// THE FIX IS DELETION, NOT MIGRATION. Rewriting the eleven call sites to say
// csprng_bytes() would have left this generator sitting in the tree for the
// twelfth caller to find, and "remember to use the good one" is not a control.
// There is now exactly ONE CSPRNG in the kernel; these entry points remain only
// so that existing callers keep compiling, and every one of them is the DRBG.
//
// Sites that were on the weak path when this was written (measured, not
// estimated): net/tls/tls13.c x25519 ephemeral key; net/tls/tls.c client_random
// x2; crypto/ecdsa.c ECDSA private key generation; crypto/rsa.c PKCS#1 v1.5
// padding; net/ssh/ssh2.c padding and KEXINIT cookie; net/ssh/ssh2_server.c
// padding and KEXINIT cookie; bt/pair.c BLE pairing random; net/dhcp.c
// transaction id.

#include "crypto.h"
#include "csprng.h"
#include "../string.h"
#include "../serial.h"

void rng_init(void) {
    // csprng_init() is idempotent and csprng_bytes() self-instantiates anyway,
    // so this is safe to call early and safe to call twice.
    csprng_init();
    kprintf("[RNG] routed to the HMAC-DRBG (crypto/csprng.c)\n");
}

void rng_add_entropy(const void *data, size_t length) {
    csprng_add_entropy(data, length);
}

int rng_get_bytes(void *buffer, size_t length) {
    csprng_bytes(buffer, length);
    return 0;
}

uint32_t rng_get_u32(void) {
    uint32_t v;
    csprng_bytes(&v, sizeof(v));
    return v;
}

uint64_t rng_get_u64(void) {
    uint64_t v;
    csprng_bytes(&v, sizeof(v));
    return v;
}
