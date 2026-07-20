// csprng.h - Cryptographically-secure PRNG for MayteraOS
//
// Fix for the fake-audit CRITICAL finding: drivers/dev.c's old /dev/urandom
// was a bare xorshift64 seeded ONCE from RDTSC at first use (deterministic
// and predictable across boots once the boot-time TSC value is known/guessed,
// and write()s to stir the pool were silently discarded).
//
// This is an HMAC-DRBG (NIST SP 800-90A style construction, built on the
// kernel's existing HMAC-SHA256 primitive from crypto/hmac.c) seeded from
// multiple independent entropy sources at init and reseeded periodically
// (call-count and wall-clock/tick based). RDRAND/RDSEED are used when the
// CPU advertises them (CPUID) and skipped gracefully otherwise (e.g. under
// QEMU without those flags) in favor of RDTSC jitter + PIT tick entropy.
//
// Callers should use csprng_bytes() for anything security sensitive
// (keys, nonces, IVs, session tokens). csprng_add_entropy() lets a caller
// (e.g. a write() to /dev/urandom) stir additional material into the pool.
#ifndef CRYPTO_CSPRNG_H
#define CRYPTO_CSPRNG_H

#include "../types.h"

// Explicit (re)initialization. Safe to call more than once; a fresh
// entropy gather + DRBG instantiate is performed each time. Also called
// lazily by csprng_bytes()/csprng_add_entropy() on first use, so callers
// don't strictly need to call this, but dev_init() calls it explicitly so
// the boot log records CSPRNG status (RDRAND/RDSEED availability) up front.
void csprng_init(void);

// Fill `len` bytes of `buf` with CSPRNG output. Automatically reseeds from
// fresh entropy periodically (both after N generate calls and after a
// tick-based time budget elapses), per HMAC-DRBG reseed_counter semantics.
// Always succeeds (falls back to RDTSC/tick-derived entropy if RDRAND and
// RDSEED are both unavailable, so it never blocks or returns an error).
void csprng_bytes(void *buf, size_t len);

// Force an immediate reseed from fresh entropy sources.
void csprng_reseed(void);

// Stir caller-supplied bytes into the DRBG state as additional entropy.
// Used to honor write()s to /dev/urandom instead of discarding them. The
// data does NOT replace internal entropy gathering; it's additional input
// on top of it, so a malicious/predictable writer can only strengthen (or
// at worst not weaken) the pool, per HMAC-DRBG's additional_input model.
void csprng_add_entropy(const void *data, size_t len);

#endif // CRYPTO_CSPRNG_H
