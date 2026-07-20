// totp.h - RFC 6238 TOTP core for MayteraOS MFA app.
// Self-contained: HMAC-SHA1, HMAC-SHA256, base32 decode, TOTP code generation.
// Designed to compile both freestanding (in the app) and with host gcc (unit test).
#ifndef MFA_TOTP_H
#define MFA_TOTP_H

#include <stdint.h>
#include <stddef.h>

// --- SHA-1 ---------------------------------------------------------------
#define SHA1_DIGEST_LEN 20
void sha1(const uint8_t *data, size_t len, uint8_t out[SHA1_DIGEST_LEN]);

// --- SHA-256 -------------------------------------------------------------
#define SHA256_DIGEST_LEN 32
void sha256(const uint8_t *data, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

// --- HMAC ----------------------------------------------------------------
void hmac_sha1(const uint8_t *key, size_t key_len,
               const uint8_t *msg, size_t msg_len,
               uint8_t out[SHA1_DIGEST_LEN]);
void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *msg, size_t msg_len,
                 uint8_t out[SHA256_DIGEST_LEN]);

// --- Base32 (RFC 4648) ---------------------------------------------------
// Decodes ASCII base32 into out. Ignores spaces, '=' padding, and case.
// Returns number of decoded bytes, or -1 on an invalid character.
int base32_decode(const char *in, uint8_t *out, size_t out_cap);

// --- TOTP (RFC 6238) -----------------------------------------------------
typedef enum { TOTP_SHA1 = 0, TOTP_SHA256 = 1 } totp_alg_t;

// Compute the TOTP value for a given counter (counter = unix_time / period).
// Returns the truncated value modulo 10^digits.
uint32_t totp_at_counter(const uint8_t *key, size_t key_len,
                         uint64_t counter, int digits, totp_alg_t alg);

// Convenience: compute TOTP for a unix timestamp.
uint32_t totp_at_time(const uint8_t *key, size_t key_len,
                      uint64_t unix_time, int period, int digits, totp_alg_t alg);

#endif // MFA_TOTP_H
