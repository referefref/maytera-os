// crypto/pbkdf2.c - PBKDF2-HMAC-SHA256 (RFC 8018). #secretuse.
//
// Promoted VERBATIM out of proc/users.c, where it was file-static and reachable
// only by the SHADOW verifier. See crypto/pbkdf2.h for why it moved, why it is
// still C, and how crypto/pbkdf2_shadow_kat.sh proves it is still under test
// after the move rather than merely asserting so.
//
// The function body below is character-identical to the one that has verified
// every password since #566. If you change it, the KAT's RFC-vector GREEN run
// is what tells you whether you changed its OUTPUT, and an unchanged GREEN run
// over vectors reproduced independently in Python is the only acceptable
// evidence that a change to this file is safe.

#include "pbkdf2.h"
#include "crypto.h"
#include "../string.h"   // memcpy; the owning header, not a private extern

void pbkdf2_hmac_sha256(const uint8_t *pw, size_t pwlen,
                        const uint8_t *salt, size_t saltlen,
                        uint32_t iters, uint8_t out[PBKDF2_DK_LEN]) {
#ifdef PBKDF2_CORE_FAULT
    /* HOST KAT RED-TEAM ONLY, and it is a DIFFERENT red than PBKDF2_KAT_FAULT.
       The kernel build NEVER defines this (it appears in no Makefile; grep the
       tree). crypto/pbkdf2_shadow_kat.sh compiles a THIRD copy WITH it.

       WHY A SECOND RED EXISTS AT ALL. The KAT's original RED faulted
       verify_against_record() inside the users.c marked region. That red still
       fires whether or not this file is even on the compile line, so on its own
       it would have kept printing PASS after the promotion while covering this
       file with nothing. This fault lives HERE: it collapses the iteration loop
       to a single HMAC, which is a real and plausible bug (an off-by-one on the
       loop bound, or a "temporarily lower the cost for testing" edit that ships)
       and which produces a perfectly well-formed 32-byte output that only a
       known-answer vector can distinguish from the correct one.

       So: GREEN proves the vectors match. This RED proves the vectors are
       computed BY THIS FILE. Without it the oracle could not tell a promotion
       from a deletion. */
    hmac_sha256_ctx_t fctx;
    hmac_sha256_init(&fctx, pw, pwlen);
    hmac_sha256_update(&fctx, salt, saltlen);
    const uint8_t fblk[4] = { 0, 0, 0, 1 };
    hmac_sha256_update(&fctx, fblk, 4);
    hmac_sha256_final(&fctx, out);
    (void)iters;
    return;
#else
    uint8_t U[32], T[32];
    // U1 = HMAC(pw, salt || INT_BE32(1))
    hmac_sha256_ctx_t ctx;
    hmac_sha256_init(&ctx, pw, pwlen);
    hmac_sha256_update(&ctx, salt, saltlen);
    const uint8_t blk_idx[4] = { 0, 0, 0, 1 };
    hmac_sha256_update(&ctx, blk_idx, 4);
    hmac_sha256_final(&ctx, U);
    memcpy(T, U, 32);
    for (uint32_t i = 1; i < iters; i++) {
        // U_c = HMAC(pw, U_{c-1})
        hmac_sha256(pw, pwlen, U, 32, U);
        for (int j = 0; j < 32; j++) T[j] ^= U[j];
    }
    memcpy(out, T, 32);
    crypto_zero(U, sizeof(U));
    crypto_zero(T, sizeof(T));
#endif
}
