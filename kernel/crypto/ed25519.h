// ed25519.h - Ed25519 signature verification for MayteraOS (RFC 8032)
#ifndef ED25519_H
#define ED25519_H

#include "../types.h"

// Verify an Ed25519 signature. Returns 1 if valid, 0 otherwise.
//   sig  : 64-byte signature (R || S)
//   m    : message bytes (mlen)
//   pk   : 32-byte public key (compressed Edwards point)
int ed25519_verify(const uint8_t sig[64], const uint8_t *m, size_t mlen,
                   const uint8_t pk[32]);

// #404 batch-3: boot-time [RUST-DIFF] point-decode self-test (unpack25519).
void ed25519_decode_selftest(void);

#endif // ED25519_H
