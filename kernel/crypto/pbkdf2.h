// crypto/pbkdf2.h - PBKDF2-HMAC-SHA256 (RFC 8018), the kernel's ONE key
// derivation function. #secretuse.
//
// WHY THIS FILE EXISTS. Until #secretuse this code was a FILE-STATIC function
// inside proc/users.c, reachable only by the SHADOW password verifier in that
// same translation unit. The encrypted secret store (rustkern/secstore.rs) needs
// the SAME derivation from the SAME login password, and the only two honest ways
// to get it were to promote this function or to write a second PBKDF2. A second
// PBKDF2 is the exact failure this project has a standing rule against: two
// implementations of one primitive drift, and the one with fewer callers is the
// one nobody notices is wrong. So it is promoted.
//
// WHAT MOVED AND WHAT DID NOT. Only pbkdf2_hmac_sha256() and the three geometry
// constants moved. The SHADOW RECORD format ("pbkdf2$<iters>$<salthex>$<hashhex>"),
// make_pbkdf2_record() and verify_against_record() stay file-static in users.c,
// because a policy bypass must not be reachable from another translation unit
// (the same reasoning that made user_verify_password() static in #745). This
// header exposes the KDF, not the authenticator.
//
// THE KAT MOVED WITH IT, WHICH IS THE POINT.
// crypto/pbkdf2_shadow_kat.sh extracts a MARKED REGION from users.c verbatim.
// Moving code out of that region would have silently stopped testing it: the
// region would still compile, the oracle would still print PASS, and the
// promoted function would be covered by nothing. This repo has already had a
// KAT rot unnoticed (ecdsa_verify_kat.sh, see run-all-kats.sh's header), so the
// oracle now ALSO compiles this file, REFUSES to run if the region has
// re-defined the function locally, and carries a THIRD build,
// -DPBKDF2_CORE_FAULT, that faults the loop IN THIS FILE and must be caught.
// That third build is the evidence that the KAT still covers the code after the
// move, rather than a claim that it does.
//
// C, NOT RUST, STATED PER THE RUST-FIRST MANDATE. This is a RELOCATION of the
// login trust anchor, byte-for-byte: the function body below is character
// identical to the one that has been verifying every password since #566, and
// the KAT's GREEN run over the RFC vectors is what proves that. Rewriting the
// password KDF in a different language in the same change that moves it would
// mean a login regression could have come from either the move or the rewrite,
// and there is no way to tell which from a machine that will not let you log in.
// A Rust port of this function is a separate, differential-gated change
// (RUST_PORT_LEDGER.md). New code in #secretuse that is NOT a relocation
// (rustkern/totp.rs: base32, TOTP, the store record parser) is Rust.
#ifndef CRYPTO_PBKDF2_H
#define CRYPTO_PBKDF2_H

#include "../types.h"

// PBKDF2-HMAC-SHA256 work factor (#566). Chosen to land ~150-250ms per attempt
// on the iMac14,4 soft-float target (measured ~0.0016ms/iter on the the build host/kvm64
// build+VM host => ~80ms there; the real target is ~2.5-3x slower). SHA-256 is
// pure integer math so the -mno-sse soft-float kernel target does not penalize
// it. This is UX-bounded and sits below OWASP's 600k desktop guidance on
// purpose: the constrained target cannot afford 600k (~1s) per login. The
// random per-account salt, 0600 root-only SHADOW, and the rate-limit/lockout in
// users.c are the compensating controls. Retune if login feels sluggish on real
// hardware, and note that raising it invalidates nothing: every SHADOW record
// and every secret-store header carries the iteration count it was made with.
#define PBKDF2_ITERATIONS 50000u
#define PBKDF2_SALT_LEN   16
#define PBKDF2_DK_LEN     32

// PBKDF2-HMAC-SHA256 with dkLen == hLen == 32 (a single output block, so no
// block-index loop is needed). RFC 8018.
//
// `iters` must be >= 1; the caller is responsible for the work factor, because
// the two callers legitimately differ (a SHADOW record and a secret-store header
// each record their own, so a future raise is per-record and detectable rather
// than a silent global change that breaks every existing file).
void pbkdf2_hmac_sha256(const uint8_t *pw, size_t pwlen,
                        const uint8_t *salt, size_t saltlen,
                        uint32_t iters, uint8_t out[PBKDF2_DK_LEN]);

#endif // CRYPTO_PBKDF2_H
