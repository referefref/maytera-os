// rsa.h - RSA cryptographic operations for MayteraOS
// Provides RSA encryption, decryption, and signature verification
#ifndef RSA_H
#define RSA_H

#include "../types.h"

// RSA error codes
#define RSA_SUCCESS              0
#define RSA_ERR_NO_MEMORY       -1
#define RSA_ERR_INVALID_PARAM   -2
#define RSA_ERR_KEY_SIZE        -3
#define RSA_ERR_PADDING         -4
#define RSA_ERR_VERIFY          -5

// Maximum key sizes
#define RSA_MAX_KEY_BYTES       512  // 4096-bit keys

// RSA public key
typedef struct {
    uint8_t *n;      // Modulus
    size_t n_len;
    uint8_t *e;      // Public exponent
    size_t e_len;
} rsa_public_key_t;

// RSA private key (modulus + private exponent; big-endian byte buffers).
// Optional CRT parameters: if p/q/dp/dq/qinv are all set, signing uses the ~4x
// faster CRT path; otherwise it falls back to m^d mod n.
typedef struct {
    uint8_t *n;      // Modulus
    size_t n_len;
    uint8_t *d;      // Private exponent
    size_t d_len;
    uint8_t *p, *q, *dp, *dq, *qinv;            // CRT params (NULL if absent)
    size_t  p_len, q_len, dp_len, dq_len, qinv_len;
} rsa_private_key_t;

// =============================================================================
// Big Number Operations (simplified for RSA)
// =============================================================================

// Big number structure (little-endian, 32-bit limbs).
// Must hold the DOUBLE-WIDTH product of two max-size operands (a 4096-bit square
// is 8192 bits = 256 limbs) plus a shift-reduction headroom limb, otherwise
// bn_mul_mod/bn_mod truncate intermediate products and corrupt the result (this
// is why 3072-bit modexp failed while 2048-bit happened to fit in the old 129).
#define BIGNUM_MAX_LIMBS    (RSA_MAX_KEY_BYTES / 4 * 2 + 2)

typedef struct {
    uint32_t limbs[BIGNUM_MAX_LIMBS];
    int size;  // Number of limbs in use
} bignum_t;

// Initialize bignum from bytes (big-endian)
void bn_from_bytes(bignum_t *n, const uint8_t *bytes, size_t len);

// Export bignum to bytes (big-endian)
void bn_to_bytes(const bignum_t *n, uint8_t *bytes, size_t len);

// Compare two bignums (returns -1, 0, or 1)
int bn_compare(const bignum_t *a, const bignum_t *b);

// Modular exponentiation: result = base^exp mod mod
void bn_mod_exp(bignum_t *result, const bignum_t *base,
                const bignum_t *exp, const bignum_t *mod);

// Additional bignum primitives (exposed for crypto/ecdsa.c's EC point math -
// #fix-tls-certverify). result = a + b / a - b (a >= b) / a * b mod m / a mod m.
void bn_add(bignum_t *result, const bignum_t *a, const bignum_t *b);
void bn_sub(bignum_t *result, const bignum_t *a, const bignum_t *b);
void bn_mod(bignum_t *result, const bignum_t *a, const bignum_t *m);
void bn_mul_mod(bignum_t *result, const bignum_t *a, const bignum_t *b, const bignum_t *m);
void bn_shr1(bignum_t *n);   // n >>= 1 (halve)

// =============================================================================
// RSA Operations
// =============================================================================

// RSA public key operation (encryption/verification)
// out = in^e mod n
int rsa_public(const rsa_public_key_t *key, 
               const uint8_t *in, size_t in_len,
               uint8_t *out, size_t out_len);

// PKCS#1 v1.5 encryption padding
int rsa_pkcs1_pad_encrypt(const uint8_t *msg, size_t msg_len,
                          uint8_t *padded, size_t padded_len);

// PKCS#1 v1.5 encryption (for TLS pre-master secret)
int rsa_encrypt_pkcs1(const rsa_public_key_t *key,
                      const uint8_t *plaintext, size_t plaintext_len,
                      uint8_t *ciphertext, size_t ciphertext_len);

// PKCS#1 v1.5 signature verification
// Returns 0 if signature is valid
int rsa_verify_pkcs1_sha256(const rsa_public_key_t *key,
                            const uint8_t *hash, size_t hash_len,
                            const uint8_t *signature, size_t sig_len);

// Same, for SHA-384 / SHA-512 digests (used by cert_store.c chain verification;
// some CA intermediates/roots sign with SHA-384).
int rsa_verify_pkcs1_sha384(const rsa_public_key_t *key,
                            const uint8_t *hash, size_t hash_len,
                            const uint8_t *signature, size_t sig_len);
int rsa_verify_pkcs1_sha512(const rsa_public_key_t *key,
                            const uint8_t *hash, size_t hash_len,
                            const uint8_t *signature, size_t sig_len);

// =============================================================================
// RSASSA-PSS signature verification (RFC 8017 8.1.2) - #502.
//
// Needed for TLS 1.2: the sigalgs we advertise put rsa_pss_rsae_sha256 (0x0804)
// ahead of rsa_pkcs1_sha256 (0x0401), so an RSA-cert server signs its
// ServerKeyExchange with PSS. Verified against the real targets: BOTH xkcd.com
// and hnrss.org choose 0x0804. Without PSS there is no TLS 1.2 to those hosts.
// (Reordering the sigalgs to force PKCS#1 is NOT an option: TLS 1.3 requires
// rsa_pss_rsae_* for RSA certs, so dropping PSS would break 1.3 everywhere.)
//
// salt length is taken to equal the digest length, which is what TLS's
// rsa_pss_rsae_* schemes mandate (RFC 8446 4.2.3).
//
// The bignum s^e mod n stays in the existing audited C (rsa_public); the
// EMSA-PSS-VERIFY *decode* of the attacker-supplied block is Rust
// (emsa_pss_verify_rs, rustkern.rs) where every bound is checked by
// construction. Returns RSA_SUCCESS (0) if valid, negative otherwise. Fails
// closed on every unexpected input.
int rsa_verify_pss_sha256(const rsa_public_key_t *key,
                          const uint8_t *hash, size_t hash_len,
                          const uint8_t *signature, size_t sig_len);
int rsa_verify_pss_sha384(const rsa_public_key_t *key,
                          const uint8_t *hash, size_t hash_len,
                          const uint8_t *signature, size_t sig_len);

// PKCS#1 v1.5 signature generation for SHA-256.
// `hash` is the 32-byte SHA-256 digest of the message; writes key_size bytes to
// `signature` (must be >= modulus size). Returns 0 on success.
int rsa_sign_pkcs1_sha256(const rsa_private_key_t *key,
                          const uint8_t *hash, size_t hash_len,
                          uint8_t *signature, size_t sig_len);

// Get key size in bytes
size_t rsa_key_size(const rsa_public_key_t *key);

#endif // RSA_H
