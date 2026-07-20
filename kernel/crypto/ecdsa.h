// ecdsa.h - ECDSA (P-256 / P-384) signature verification for MayteraOS
// #fix-tls-certverify: real elliptic-curve signature verify, used by
// net/tls/cert_store.c to check X.509 certificate signatures signed with an
// ECDSA public key (replaces the old "return 0" placeholder).
//
// Verify-only (no key generation / signing): this is used purely to
// authenticate certificate chains and (future) TLS CertificateVerify
// messages, never to produce our own ECDSA signatures.
#ifndef ECDSA_H
#define ECDSA_H

#include "../types.h"

typedef enum {
    ECDSA_CURVE_P256,
    ECDSA_CURVE_P384
} ecdsa_curve_id_t;

// Verify an ECDSA signature (raw r,s big-endian integers, as pulled out of the
// DER ECDSA-Sig-Value SEQUENCE) over `hash` using the public key point (qx,qy).
//   curve     : which curve the key belongs to
//   hash      : message digest (SHA-256 for P-256, SHA-384 for P-384 by convention)
//   hash_len  : digest length in bytes
//   r, r_len  : signature r (big-endian, no padding required)
//   s, s_len  : signature s (big-endian, no padding required)
//   qx, qy    : public key affine coordinates, each coord_len bytes big-endian
//               (coord_len = 32 for P-256, 48 for P-384)
// Returns 1 if the signature is valid, 0 otherwise (including any malformed
// input - this function fails closed).
int ecdsa_verify(ecdsa_curve_id_t curve,
                  const uint8_t *hash, size_t hash_len,
                  const uint8_t *r, size_t r_len,
                  const uint8_t *s, size_t s_len,
                  const uint8_t *qx, const uint8_t *qy, size_t coord_len);

// =============================================================================
// #502: ECDH on the same curves, for the TLS 1.2 ECDHE key exchange.
//
// WHY this exists rather than "just use x25519": hnrss.org (Hacker News RSS)
// negotiates TLS 1.2 with secp384r1 ONLY. Probed against the real server:
// offering x25519 alone gets a fatal handshake_failure (alert 40), and so does
// offering P-256 alone. P-384 ECDH is therefore the only way to reach it. The
// point math is REUSED from ecdsa.c above (CLAUDE.md: improve/reuse the shared
// primitive, never fork a private copy).
//
// Keys are ephemeral, one per handshake.
//
// HONEST LIMITATION: the underlying ecp_scalar_mult is a plain double-and-add
// and is NOT constant-time, so the ephemeral private scalar is exposed to a
// local timing/cache side-channel observer. That is inherited from the existing
// verify-only code (where the scalar is public and it did not matter). It is
// acceptable here only because the scalar is per-handshake and discarded, and
// because this kernel has no untrusted local co-tenant. It is NOT suitable for
// a long-term/static ECDH key. Documented, not fixed.
// =============================================================================

// Generate an ephemeral ECDH keypair.
//   priv : receives coord_len bytes (32 for P-256, 48 for P-384), big-endian
//   pub  : receives the uncompressed point 0x04 || X || Y (1 + 2*coord_len)
// Returns 0 on success, negative on failure.
int ecdh_generate_keypair(ecdsa_curve_id_t curve,
                          uint8_t *priv, size_t priv_cap,
                          uint8_t *pub, size_t pub_cap, size_t *pub_len);

// Compute the ECDH shared secret = X coordinate of (priv * peer_point).
// `peer_point` must be an uncompressed point (0x04 || X || Y).
//
// SECURITY: this VALIDATES the peer point (0x04 form, X and Y both < p, and the
// point actually satisfies the curve equation, and the result is not the point
// at infinity) before and after the scalar multiply. Skipping that check is the
// classic invalid-curve attack, which can leak the private scalar.
// Returns 0 on success (out receives coord_len bytes), negative on failure.
int ecdh_compute_shared(ecdsa_curve_id_t curve,
                        const uint8_t *priv, size_t priv_len,
                        const uint8_t *peer_point, size_t peer_len,
                        uint8_t *out, size_t out_cap);

// Coordinate size of a curve in bytes (32 = P-256, 48 = P-384); 0 if unknown.
size_t ecdh_coord_len(ecdsa_curve_id_t curve);

#endif // ECDSA_H
