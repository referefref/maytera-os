// rsa.c - RSA cryptographic operations for MayteraOS
// Implements RSA using basic big number arithmetic

#include "rsa.h"
#include "crypto.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../serial.h"
#include "fs/bootlog.h"   // secroad2: owning header for bootlog_write, NOT a private extern

// =============================================================================
// Big Number Operations
// =============================================================================

// Initialize bignum to zero
static void bn_zero(bignum_t *n) {
    memset(n->limbs, 0, sizeof(n->limbs));
    n->size = 1;
}

// Initialize bignum from bytes (big-endian input)
void bn_from_bytes(bignum_t *n, const uint8_t *bytes, size_t len) {
    bn_zero(n);
    
    // Skip leading zeros
    while (len > 0 && bytes[0] == 0) {
        bytes++;
        len--;
    }
    
    if (len == 0) return;
    
    // Convert big-endian bytes to little-endian limbs
    n->size = (len + 3) / 4;
    if (n->size > BIGNUM_MAX_LIMBS) n->size = BIGNUM_MAX_LIMBS;
    
    for (size_t i = 0; i < len && (len - 1 - i) / 4 < BIGNUM_MAX_LIMBS; i++) {
        size_t limb_idx = (len - 1 - i) / 4;
        size_t byte_idx = (len - 1 - i) % 4;
        n->limbs[limb_idx] |= ((uint32_t)bytes[i]) << (byte_idx * 8);
    }
    
    // Normalize size
    while (n->size > 1 && n->limbs[n->size - 1] == 0) {
        n->size--;
    }
}

// Export bignum to bytes (big-endian output)
void bn_to_bytes(const bignum_t *n, uint8_t *bytes, size_t len) {
    memset(bytes, 0, len);
    
    size_t bn_bytes = n->size * 4;
    size_t offset = (len > bn_bytes) ? (len - bn_bytes) : 0;
    
    for (int i = n->size - 1; i >= 0; i--) {
        size_t pos = offset + (n->size - 1 - i) * 4;
        if (pos < len) bytes[pos] = (n->limbs[i] >> 24) & 0xFF;
        if (pos + 1 < len) bytes[pos + 1] = (n->limbs[i] >> 16) & 0xFF;
        if (pos + 2 < len) bytes[pos + 2] = (n->limbs[i] >> 8) & 0xFF;
        if (pos + 3 < len) bytes[pos + 3] = n->limbs[i] & 0xFF;
    }
}

// Compare two bignums
int bn_compare(const bignum_t *a, const bignum_t *b) {
    if (a->size != b->size) {
        return (a->size > b->size) ? 1 : -1;
    }
    
    for (int i = a->size - 1; i >= 0; i--) {
        if (a->limbs[i] != b->limbs[i]) {
            return (a->limbs[i] > b->limbs[i]) ? 1 : -1;
        }
    }
    
    return 0;
}

// Add two bignums: result = a + b
//
// #fix-tls-certverify BUG: `result->size = max_size + (carry ? 1 : 0)` reads
// `carry` AFTER the loop, but when the addition overflows into an extra limb
// the loop runs one more iteration (i == max_size) specifically to WRITE that
// limb (result->limbs[max_size] = 1) and, in doing so, recomputes `carry` as
// the carry-OUT of that iteration (0, since limb value 1 doesn't overflow
// again). So by the time the loop exits, `carry` is back to 0 even though an
// extra limb full of real data was just written - result->size ends up
// max_size (the pre-overflow size), silently discarding the top limb.
// Concretely: adding two ~256-bit values whose sum needs a 257th bit
// (extremely common in modular field arithmetic, e.g. EC point add/double)
// truncated the result to its low 256 bits with NO error, no crash, just a
// silently wrong (and coincidentally still "plausible", same-magnitude)
// answer. This is exactly what a native test harness against a known-good
// P-256 point caught: y^2 mod p came out right (pure multiply+reduce, never
// hits this path) but x^3+a*x+b mod p came out wrong by exactly the dropped
// top bit (bn_add is used directly here, not just inside bn_mul_mod).
// RSA's own modexp path apparently never exercises this exact "carry lands
// exactly one bit past the operand width" shape often enough to have been
// noticed; crypto/ecdsa.c's EC math hits it constantly.
// Fix: track the number of limbs actually WRITTEN (the loop's own `i`
// counter) instead of re-deriving it from a post-loop carry flag.
void bn_add(bignum_t *result, const bignum_t *a, const bignum_t *b) {
    uint64_t carry = 0;
    int max_size = (a->size > b->size) ? a->size : b->size;
    int i;

    for (i = 0; i < max_size || carry; i++) {
        if (i >= BIGNUM_MAX_LIMBS) break;

        uint64_t sum = carry;
        if (i < a->size) sum += a->limbs[i];
        if (i < b->size) sum += b->limbs[i];

        result->limbs[i] = (uint32_t)sum;
        carry = sum >> 32;
    }

    result->size = (i > 0) ? i : 1;
    if (result->size > BIGNUM_MAX_LIMBS) result->size = BIGNUM_MAX_LIMBS;

    // Normalize
    while (result->size > 1 && result->limbs[result->size - 1] == 0) {
        result->size--;
    }
}

// Subtract: result = a - b (assumes a >= b)
void bn_sub(bignum_t *result, const bignum_t *a, const bignum_t *b) {
    int64_t borrow = 0;
    
    for (int i = 0; i < a->size; i++) {
        int64_t diff = (int64_t)a->limbs[i] - borrow;
        if (i < b->size) diff -= b->limbs[i];
        
        if (diff < 0) {
            diff += 0x100000000LL;
            borrow = 1;
        } else {
            borrow = 0;
        }
        
        result->limbs[i] = (uint32_t)diff;
    }
    
    result->size = a->size;
    
    // Normalize
    while (result->size > 1 && result->limbs[result->size - 1] == 0) {
        result->size--;
    }
}

// Left shift by one bit
static void bn_shl1(bignum_t *n) {
    uint32_t carry = 0;
    
    for (int i = 0; i < n->size; i++) {
        uint32_t new_carry = n->limbs[i] >> 31;
        n->limbs[i] = (n->limbs[i] << 1) | carry;
        carry = new_carry;
    }
    
    if (carry && n->size < BIGNUM_MAX_LIMBS) {
        n->limbs[n->size++] = carry;
    }
}

// Right shift by one bit
void bn_shr1(bignum_t *n) {
    uint32_t carry = 0;
    
    for (int i = n->size - 1; i >= 0; i--) {
        uint32_t new_carry = n->limbs[i] & 1;
        n->limbs[i] = (n->limbs[i] >> 1) | (carry << 31);
        carry = new_carry;
    }
    
    // Normalize
    while (n->size > 1 && n->limbs[n->size - 1] == 0) {
        n->size--;
    }
}

// Check if bignum is zero
static int bn_is_zero(const bignum_t *n) {
    return (n->size == 1 && n->limbs[0] == 0);
}

// Check if bit at position is set
static int bn_get_bit(const bignum_t *n, int pos) {
    int limb = pos / 32;
    int bit = pos % 32;
    if (limb >= n->size) return 0;
    return (n->limbs[limb] >> bit) & 1;
}

// Get number of bits
static int bn_bit_length(const bignum_t *n) {
    if (n->size == 0 || (n->size == 1 && n->limbs[0] == 0)) return 0;
    
    int bits = (n->size - 1) * 32;
    uint32_t top = n->limbs[n->size - 1];
    
    while (top) {
        bits++;
        top >>= 1;
    }
    
    return bits;
}

// Modular reduction: result = a mod m, via binary long division (shift and
// conditionally subtract). O(bitlength) iterations - NOT O(quotient). The old
// naive "while (a>=m) a-=m" was ~2^bits iterations = effectively an infinite
// hang for crypto-sized operands (e.g. a 6144-bit product mod a 3072-bit m).
void bn_mod(bignum_t *result, const bignum_t *a, const bignum_t *m) {
    memcpy(result, a, sizeof(bignum_t));
    if (bn_is_zero(m)) return;
    int rbits = bn_bit_length(result);
    int mbits = bn_bit_length(m);
    if (rbits < mbits) return;          // already reduced
    int shift = rbits - mbits;

    // ms = m << shift  (fits because BIGNUM_MAX_LIMBS holds double-width values)
    bignum_t ms;
    memcpy(&ms, m, sizeof(bignum_t));
    for (int i = 0; i < shift; i++) bn_shl1(&ms);

    for (int i = shift; i >= 0; i--) {
        if (bn_compare(result, &ms) >= 0) bn_sub(result, result, &ms);
        bn_shr1(&ms);
    }
}

// Montgomery multiplication helper (simplified version)
// Computes result = (a * b) mod m using schoolbook multiplication + reduction
void bn_mul_mod(bignum_t *result, const bignum_t *a, const bignum_t *b, const bignum_t *m) {
    bignum_t product;
    bn_zero(&product);
    
    // Schoolbook multiplication
    for (int i = 0; i < a->size; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < b->size || carry; j++) {
            if (i + j >= BIGNUM_MAX_LIMBS) break;
            
            uint64_t prod = (uint64_t)product.limbs[i + j] + carry;
            if (j < b->size) {
                prod += (uint64_t)a->limbs[i] * b->limbs[j];
            }
            
            product.limbs[i + j] = (uint32_t)prod;
            carry = prod >> 32;
        }
        
    }

    // The product of an a-limb and a b-limb number occupies at most a+b limbs.
    // (The old per-iteration "product.size = i + b->size" undercounted by one
    // whenever the final partial-product carried into limb a+b-1, dropping the
    // most-significant limb and corrupting results for >=3072-bit operands.)
    product.size = a->size + b->size;
    if (product.size > BIGNUM_MAX_LIMBS) product.size = BIGNUM_MAX_LIMBS;
    
    // Normalize
    while (product.size > 1 && product.limbs[product.size - 1] == 0) {
        product.size--;
    }
    
    // Reduce mod m
    bn_mod(result, &product, m);
}

// Modular exponentiation using binary method (left-to-right)
void bn_mod_exp(bignum_t *result, const bignum_t *base, 
                const bignum_t *exp, const bignum_t *mod) {
    if (bn_is_zero(exp)) {
        bn_zero(result);
        result->limbs[0] = 1;
        return;
    }
    
    // Start with base
    bignum_t acc;
    memcpy(&acc, base, sizeof(bignum_t));
    bn_mod(&acc, &acc, mod);
    
    // Find highest bit
    int bits = bn_bit_length(exp);
    
    // Square and multiply (left to right)
    for (int i = bits - 2; i >= 0; i--) {
        // Square
        bn_mul_mod(&acc, &acc, &acc, mod);
        
        // Multiply if bit is set
        if (bn_get_bit(exp, i)) {
            bn_mul_mod(&acc, &acc, base, mod);
        }
    }
    
    memcpy(result, &acc, sizeof(bignum_t));
}

// =============================================================================
// RSA Operations
// =============================================================================

size_t rsa_key_size(const rsa_public_key_t *key) {
    return key->n_len;
}

// RSA public key operation: out = in^e mod n
int rsa_public(const rsa_public_key_t *key,
               const uint8_t *in, size_t in_len,
               uint8_t *out, size_t out_len) {
    if (in_len > key->n_len || out_len < key->n_len) {
        return RSA_ERR_INVALID_PARAM;
    }
    
    bignum_t base, exp, mod, result;
    
    // Convert inputs to bignums
    bn_from_bytes(&base, in, in_len);
    bn_from_bytes(&exp, key->e, key->e_len);
    bn_from_bytes(&mod, key->n, key->n_len);
    
    // Compute base^exp mod mod
    bn_mod_exp(&result, &base, &exp, &mod);
    
    // Convert result back to bytes
    bn_to_bytes(&result, out, key->n_len);
    
    return RSA_SUCCESS;
}

// PKCS#1 v1.5 encryption padding
// Format: 0x00 | 0x02 | PS (random non-zero) | 0x00 | M
int rsa_pkcs1_pad_encrypt(const uint8_t *msg, size_t msg_len,
                          uint8_t *padded, size_t padded_len) {
    // Need at least 11 bytes overhead: 0x00 0x02 [8+ bytes PS] 0x00
    if (msg_len > padded_len - 11) {
        return RSA_ERR_INVALID_PARAM;
    }
    
    size_t ps_len = padded_len - msg_len - 3;
    
    padded[0] = 0x00;
    padded[1] = 0x02;
    
    // Generate random non-zero padding bytes
    for (size_t i = 0; i < ps_len; i++) {
        do {
            rng_get_bytes(&padded[2 + i], 1);
        } while (padded[2 + i] == 0);
    }
    
    padded[2 + ps_len] = 0x00;
    memcpy(padded + 3 + ps_len, msg, msg_len);
    
    return RSA_SUCCESS;
}

// RSA PKCS#1 v1.5 encryption
int rsa_encrypt_pkcs1(const rsa_public_key_t *key,
                      const uint8_t *plaintext, size_t plaintext_len,
                      uint8_t *ciphertext, size_t ciphertext_len) {
    size_t key_size = rsa_key_size(key);
    
    if (ciphertext_len < key_size) {
        return RSA_ERR_INVALID_PARAM;
    }
    
    // Pad the message
    uint8_t *padded = kmalloc(key_size);
    if (!padded) return RSA_ERR_NO_MEMORY;
    
    int ret = rsa_pkcs1_pad_encrypt(plaintext, plaintext_len, padded, key_size);
    if (ret != RSA_SUCCESS) {
        kfree(padded);
        return ret;
    }
    
    // RSA encrypt
    ret = rsa_public(key, padded, key_size, ciphertext, key_size);
    
    crypto_zero(padded, key_size);
    kfree(padded);
    
    return ret;
}

// DigestInfo prefixes (PKCS#1 v1.5 signature), RFC 3447 / PKCS#1 v2.2 App B.1.
// 30 31 30 0d 06 09 60 86 48 01 65 03 04 02 01 05 00 04 20 [32-byte hash]
static const uint8_t SHA256_DIGEST_INFO_PREFIX[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
    0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
    0x00, 0x04, 0x20
};
// 30 41 30 0d 06 09 60 86 48 01 65 03 04 02 02 05 00 04 30 [48-byte hash]
static const uint8_t SHA384_DIGEST_INFO_PREFIX[] = {
    0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
    0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05,
    0x00, 0x04, 0x30
};
// 30 51 30 0d 06 09 60 86 48 01 65 03 04 02 03 05 00 04 40 [64-byte hash]
static const uint8_t SHA512_DIGEST_INFO_PREFIX[] = {
    0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
    0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05,
    0x00, 0x04, 0x40
};

// Shared PKCS#1 v1.5 verify: decrypts `signature` with the RSA public key,
// then checks the 0x00 0x01 PS(0xFF..) 0x00 DigestInfo(hash) padding.
static int rsa_verify_pkcs1_digest(const rsa_public_key_t *key,
                                    const uint8_t *hash, size_t hash_len,
                                    const uint8_t *di_prefix, size_t di_prefix_len,
                                    const uint8_t *signature, size_t sig_len) {
#ifdef RSA_KAT_FAULT
    /* HOST KAT RED-TEAM ONLY. The kernel build NEVER defines this flag
       (grep the Makefiles: it appears nowhere). rsa_verify_kat.sh compiles a
       second copy WITH it to prove rsa_verify_selftest()'s forgery-rejection
       checks actually FIRE: an unconditional-accept PKCS#1 verifier is the
       single most dangerous verify bug (== a cert_store.c stub that
       authenticates every RSA certificate). A KAT that could not catch it
       would be a no-op. See secroad2 / blame.md. */
    (void)key; (void)hash; (void)hash_len; (void)di_prefix;
    (void)di_prefix_len; (void)signature; (void)sig_len;
    return RSA_SUCCESS;
#endif
    size_t key_size = rsa_key_size(key);
    if (sig_len != key_size) {
        return RSA_ERR_INVALID_PARAM;
    }
    size_t di_len = di_prefix_len + hash_len;
    if (key_size < di_len + 11) {
        return RSA_ERR_INVALID_PARAM;   // key too small to hold this DigestInfo
    }

    // RSA verify: compute signature^e mod n
    uint8_t *decrypted = kmalloc(key_size);
    if (!decrypted) return RSA_ERR_NO_MEMORY;

    int ret = rsa_public(key, signature, sig_len, decrypted, key_size);
    if (ret != RSA_SUCCESS) {
        kfree(decrypted);
        return ret;
    }
    // Verify PKCS#1 v1.5 signature padding
    // Format: 0x00 | 0x01 | PS (0xFF bytes) | 0x00 | DigestInfo

    size_t expected_ps_len = key_size - 3 - di_len;

    ret = RSA_ERR_VERIFY;

    // Check format
    if (decrypted[0] != 0x00 || decrypted[1] != 0x01) {
        goto done;
    }

    // Check PS (all 0xFF)
    for (size_t i = 0; i < expected_ps_len; i++) {
        if (decrypted[2 + i] != 0xFF) {
            goto done;
        }
    }

    // Check separator
    if (decrypted[2 + expected_ps_len] != 0x00) {
        goto done;
    }

    // Check DigestInfo prefix
    size_t di_offset = 3 + expected_ps_len;
    if (crypto_memcmp(decrypted + di_offset, di_prefix, di_prefix_len) != 0) {
        goto done;
    }

    // Check hash
    if (crypto_memcmp(decrypted + di_offset + di_prefix_len, hash, hash_len) != 0) {
        goto done;
    }

    ret = RSA_SUCCESS;

done:
    crypto_zero(decrypted, key_size);
    kfree(decrypted);
    return ret;
}

// =============================================================================
// #502: RSASSA-PSS verification (RFC 8017 8.1.2).
//
// Split of responsibility: the modular exponentiation s^e mod n uses the
// existing audited C bignum (rsa_public) - it is a REUSED shared primitive, not
// new code. The EMSA-PSS-VERIFY decode of the resulting attacker-controlled
// encoded message is the Rust port emsa_pss_verify_rs (rustkern.rs): that is
// where every length/format check lives and where a missed bound is a signature
// forgery. Validated offline against real `openssl dgst -sigopt
// rsa_padding_mode:pss` signatures (RSA-2048/3072 x SHA-256/384), plus negative
// controls (bit-flipped signature, wrong digest, stripped 0xbc trailer, wrong
// salt length, all-zero block) - all correctly REJECTED.
// =============================================================================
extern int emsa_pss_verify_rs(const uint8_t *em, size_t em_len, uint32_t em_bits,
                              const uint8_t *mhash, size_t hash_len, size_t salt_len);

// Bit length of the modulus (ignores any leading zero bytes in the buffer).
static size_t rsa_modulus_bits(const rsa_public_key_t *key) {
    size_t i = 0;
    while (i < key->n_len && key->n[i] == 0x00) i++;
    if (i >= key->n_len) return 0;
    size_t bits = (key->n_len - i - 1) * 8;
    uint8_t top = key->n[i];
    while (top) { bits++; top >>= 1; }
    return bits;
}

static int rsa_verify_pss_digest(const rsa_public_key_t *key,
                                  const uint8_t *hash, size_t hash_len,
                                  const uint8_t *signature, size_t sig_len) {
    if (!key || !key->n || !hash || !signature) return RSA_ERR_INVALID_PARAM;

    size_t key_size = rsa_key_size(key);
    if (sig_len != key_size) return RSA_ERR_INVALID_PARAM;

    size_t mod_bits = rsa_modulus_bits(key);
    if (mod_bits < 512) return RSA_ERR_INVALID_PARAM;

    // RFC 8017 8.1.2: emBits = modBits - 1, emLen = ceil(emBits / 8).
    uint32_t em_bits = (uint32_t)(mod_bits - 1);
    size_t em_len = ((size_t)em_bits + 7) / 8;
    if (em_len > key_size) return RSA_ERR_INVALID_PARAM;

    uint8_t *decrypted = kmalloc(key_size);
    if (!decrypted) return RSA_ERR_NO_MEMORY;

    int ret = rsa_public(key, signature, sig_len, decrypted, key_size);
    if (ret != RSA_SUCCESS) {
        kfree(decrypted);
        return ret;
    }

    // rsa_public writes a key_size-byte big-endian result. EM is its rightmost
    // em_len bytes; when em_len < key_size (modBits-1 a multiple of 8) the
    // leading bytes MUST be zero, otherwise the integer does not fit emLen
    // octets and the signature is invalid.
    ret = RSA_ERR_VERIFY;
    for (size_t i = 0; i < key_size - em_len; i++) {
        if (decrypted[i] != 0x00) goto done;
    }
    // salt length == digest length (TLS rsa_pss_rsae_*, RFC 8446 4.2.3).
    if (emsa_pss_verify_rs(decrypted + (key_size - em_len), em_len, em_bits,
                           hash, hash_len, hash_len) == 0) {
        ret = RSA_SUCCESS;
    }

done:
    crypto_zero(decrypted, key_size);
    kfree(decrypted);
    return ret;
}

int rsa_verify_pss_sha256(const rsa_public_key_t *key,
                          const uint8_t *hash, size_t hash_len,
                          const uint8_t *signature, size_t sig_len) {
    if (hash_len != 32) return RSA_ERR_INVALID_PARAM;
    return rsa_verify_pss_digest(key, hash, 32, signature, sig_len);
}

int rsa_verify_pss_sha384(const rsa_public_key_t *key,
                          const uint8_t *hash, size_t hash_len,
                          const uint8_t *signature, size_t sig_len) {
    if (hash_len != 48) return RSA_ERR_INVALID_PARAM;
    return rsa_verify_pss_digest(key, hash, 48, signature, sig_len);
}

// RSA PKCS#1 v1.5 signature verification for SHA-256
int rsa_verify_pkcs1_sha256(const rsa_public_key_t *key,
                            const uint8_t *hash, size_t hash_len,
                            const uint8_t *signature, size_t sig_len) {
    if (hash_len != 32) return RSA_ERR_INVALID_PARAM;
    return rsa_verify_pkcs1_digest(key, hash, 32,
                                    SHA256_DIGEST_INFO_PREFIX, sizeof(SHA256_DIGEST_INFO_PREFIX),
                                    signature, sig_len);
}

// RSA PKCS#1 v1.5 signature verification for SHA-384
int rsa_verify_pkcs1_sha384(const rsa_public_key_t *key,
                            const uint8_t *hash, size_t hash_len,
                            const uint8_t *signature, size_t sig_len) {
    if (hash_len != 48) return RSA_ERR_INVALID_PARAM;
    return rsa_verify_pkcs1_digest(key, hash, 48,
                                    SHA384_DIGEST_INFO_PREFIX, sizeof(SHA384_DIGEST_INFO_PREFIX),
                                    signature, sig_len);
}

// RSA PKCS#1 v1.5 signature verification for SHA-512
int rsa_verify_pkcs1_sha512(const rsa_public_key_t *key,
                            const uint8_t *hash, size_t hash_len,
                            const uint8_t *signature, size_t sig_len) {
    if (hash_len != 64) return RSA_ERR_INVALID_PARAM;
    return rsa_verify_pkcs1_digest(key, hash, 64,
                                    SHA512_DIGEST_INFO_PREFIX, sizeof(SHA512_DIGEST_INFO_PREFIX),
                                    signature, sig_len);
}

// Full-width multiply (no reduction): result = a * b.
static void bn_mul(bignum_t *result, const bignum_t *a, const bignum_t *b) {
    bignum_t product; bn_zero(&product);
    for (int i = 0; i < a->size; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < b->size || carry; j++) {
            if (i + j >= BIGNUM_MAX_LIMBS) break;
            uint64_t prod = (uint64_t)product.limbs[i + j] + carry;
            if (j < b->size) prod += (uint64_t)a->limbs[i] * b->limbs[j];
            product.limbs[i + j] = (uint32_t)prod; carry = prod >> 32;
        }
    }
    product.size = a->size + b->size;
    if (product.size > BIGNUM_MAX_LIMBS) product.size = BIGNUM_MAX_LIMBS;
    while (product.size > 1 && product.limbs[product.size - 1] == 0) product.size--;
    memcpy(result, &product, sizeof(bignum_t));
}

// CRT signing: out = m^d mod n via the Chinese Remainder Theorem (~4x faster than
// a full-width modexp). Requires p,q,dp,dq,qinv. Returns 0 on success.
//   m1 = m^dp mod p ; m2 = m^dq mod q
//   h  = qinv*(m1 - m2) mod p ; out = m2 + h*q
static int rsa_crt(const rsa_private_key_t *k, const bignum_t *m, bignum_t *out) {
    bignum_t *w = kmalloc(9 * sizeof(bignum_t));
    if (!w) return -1;
    bignum_t *P=&w[0],*Q=&w[1],*DP=&w[2],*DQ=&w[3],*QI=&w[4],*m1=&w[5],*m2=&w[6],*t=&w[7],*tmp=&w[8];
    bn_from_bytes(P, k->p, k->p_len);   bn_from_bytes(Q, k->q, k->q_len);
    bn_from_bytes(DP, k->dp, k->dp_len); bn_from_bytes(DQ, k->dq, k->dq_len);
    bn_from_bytes(QI, k->qinv, k->qinv_len);
    bn_mod(tmp, m, P); bn_mod_exp(m1, tmp, DP, P);     // m1 = (m mod p)^dp mod p
    bn_mod(tmp, m, Q); bn_mod_exp(m2, tmp, DQ, Q);     // m2 = (m mod q)^dq mod q
    bn_mod(tmp, m2, P);                                 // tmp = m2 mod p
    if (bn_compare(m1, tmp) >= 0) bn_sub(t, m1, tmp);
    else { bn_add(DP, m1, P); bn_sub(t, DP, tmp); }     // t = (m1 - m2) mod p  (DP reused as scratch)
    bn_mul_mod(m1, QI, t, P);                            // m1 = qinv*(m1-m2) mod p  (= h)
    bn_mul(DQ, m1, Q);                                   // DQ = h*q  (reused as scratch)
    bn_add(out, m2, DQ);                                 // out = m2 + h*q
    kfree(w);
    return 0;
}

// RSA PKCS#1 v1.5 signature generation for SHA-256.
// sig = ( 0x00 0x01 0xFF..0xFF 0x00 DigestInfo(SHA256) hash )^d mod n
int rsa_sign_pkcs1_sha256(const rsa_private_key_t *key,
                          const uint8_t *hash, size_t hash_len,
                          uint8_t *signature, size_t sig_len) {
    if (hash_len != 32) return RSA_ERR_INVALID_PARAM;

    // modulus size in bytes (strip a leading zero if present)
    const uint8_t *n = key->n; size_t n_len = key->n_len;
    while (n_len > 1 && n[0] == 0) { n++; n_len--; }
    size_t key_size = n_len;
    if (sig_len < key_size) return RSA_ERR_INVALID_PARAM;

    size_t di_len = sizeof(SHA256_DIGEST_INFO_PREFIX) + 32;
    if (key_size < di_len + 11) return RSA_ERR_KEY_SIZE;
    size_t ps_len = key_size - 3 - di_len;

    uint8_t *em = kmalloc(key_size);
    if (!em) return RSA_ERR_NO_MEMORY;
    em[0] = 0x00; em[1] = 0x01;
    for (size_t i = 0; i < ps_len; i++) em[2 + i] = 0xFF;
    em[2 + ps_len] = 0x00;
    memcpy(em + 3 + ps_len, SHA256_DIGEST_INFO_PREFIX, sizeof(SHA256_DIGEST_INFO_PREFIX));
    memcpy(em + 3 + ps_len + sizeof(SHA256_DIGEST_INFO_PREFIX), hash, 32);

    // signature = em^d mod n  (CRT path when the key carries p/q/dp/dq/qinv)
    bignum_t base, result;
    bn_from_bytes(&base, em, key_size);
    int used_crt = 0;
    if (key->p && key->q && key->dp && key->dq && key->qinv) {
        if (rsa_crt(key, &base, &result) == 0) used_crt = 1;
    }
    if (!used_crt) {
        bignum_t exp, mod;
        bn_from_bytes(&exp, key->d, key->d_len);
        bn_from_bytes(&mod, n, n_len);
        bn_mod_exp(&result, &base, &exp, &mod);
    }
    bn_to_bytes(&result, signature, key_size);

    crypto_zero(em, key_size);
    kfree(em);
    return RSA_SUCCESS;
}

// =============================================================================
// secroad2: RSA PKCS#1 v1.5 SHA-256 signature-verify KAT vector + self-test.
// =============================================================================
/* Message: b'MayteraOS RSA PKCS#1 v1.5 SHA-256 signature-verify KAT (secroad2)' */
/* n_len=256 e=010001 hash=SHA-256(msg) sig_len=256 */
static const uint8_t KAT_N[256] = {
    0xad, 0xbf, 0x11, 0x8b, 0xe8, 0xd1, 0xe7, 0xb7, 0xa0, 0x72, 0x2b, 0xb7,
    0x17, 0xa3, 0x03, 0x10, 0xb7, 0x7c, 0x5e, 0x60, 0x00, 0xbc, 0xa6, 0x0a,
    0xd9, 0x87, 0x40, 0x8d, 0xd1, 0xe7, 0x44, 0x4f, 0xf0, 0x34, 0x08, 0x69,
    0xf1, 0x00, 0x3d, 0xf1, 0xed, 0xb4, 0x03, 0x27, 0x80, 0x8b, 0x09, 0xeb,
    0xce, 0x69, 0xc6, 0xca, 0x0c, 0xd5, 0x7e, 0x6c, 0x3d, 0x94, 0x58, 0xa6,
    0x52, 0x57, 0x27, 0x8c, 0x62, 0x5a, 0x5e, 0x98, 0xf0, 0x0d, 0xab, 0x2d,
    0xd9, 0x4a, 0x5e, 0x48, 0xa8, 0xbf, 0x47, 0x4e, 0x23, 0x26, 0xec, 0x93,
    0x10, 0x27, 0x7f, 0x3a, 0xc9, 0xb6, 0xb1, 0x0a, 0x58, 0xa0, 0x0b, 0x3c,
    0xba, 0x25, 0xbe, 0x4c, 0x33, 0xb8, 0x31, 0x17, 0x02, 0x09, 0x55, 0x2c,
    0x32, 0xde, 0xfb, 0x6d, 0x44, 0x3d, 0xa7, 0x37, 0xaf, 0xfa, 0x40, 0x41,
    0x78, 0x8f, 0xd3, 0xf7, 0xff, 0xd2, 0x8e, 0x43, 0x7b, 0x4d, 0xf1, 0xb9,
    0x8c, 0x33, 0x05, 0xa2, 0xd7, 0xeb, 0xb3, 0xd7, 0x48, 0x8c, 0x14, 0xba,
    0x7d, 0xbc, 0x27, 0xb7, 0x9a, 0x83, 0xbc, 0xe2, 0x45, 0xb2, 0x9d, 0xe0,
    0xc3, 0x66, 0x9d, 0xf9, 0x08, 0x53, 0x59, 0xc5, 0x1b, 0xd2, 0x28, 0x0b,
    0x00, 0x1f, 0x38, 0x9a, 0x0b, 0x42, 0xcb, 0x1c, 0xdb, 0x50, 0x2f, 0x2f,
    0x2a, 0x77, 0x99, 0xcb, 0x5a, 0xa3, 0x0a, 0x0a, 0x68, 0x4b, 0xc1, 0xb7,
    0x3b, 0x57, 0x49, 0x82, 0x7a, 0x57, 0x5f, 0xe4, 0x35, 0xd5, 0x65, 0x0f,
    0x2d, 0x32, 0xda, 0xc2, 0x5a, 0xc7, 0xe3, 0x69, 0xd4, 0x17, 0x99, 0x6c,
    0x28, 0xa2, 0xb0, 0x46, 0x66, 0x48, 0x8d, 0xeb, 0x9f, 0xa5, 0x00, 0xc3,
    0xb0, 0xde, 0x24, 0xca, 0x8b, 0x75, 0x4d, 0x64, 0x80, 0xe4, 0xe7, 0x6b,
    0x0e, 0x35, 0x51, 0x9e, 0xa6, 0x34, 0xe1, 0x34, 0x73, 0xb8, 0x3f, 0xac,
    0xb1, 0x52, 0x39, 0x39,
};
static const uint8_t KAT_E[3] = { 0x01, 0x00, 0x01 };
static const uint8_t KAT_HASH[32] = {
    0x44, 0xfe, 0xec, 0x23, 0x7e, 0x8d, 0x6c, 0xaa, 0x1f, 0xdf, 0x7d, 0xba,
    0x0a, 0x44, 0xda, 0xd0, 0xff, 0x0a, 0x5e, 0xdf, 0x2a, 0x7d, 0x7f, 0xbe,
    0xd9, 0xe0, 0xe6, 0x72, 0x76, 0xec, 0x63, 0x1d,
};
static const uint8_t KAT_SIG[256] = {
    0x0d, 0x50, 0x04, 0x84, 0xb3, 0x8a, 0xa5, 0x0c, 0xe2, 0xa2, 0x3d, 0x2b,
    0xc8, 0x33, 0x3b, 0xd5, 0x76, 0xa2, 0xd6, 0x2b, 0x73, 0xc2, 0xf3, 0x58,
    0x18, 0x3d, 0xbd, 0x9b, 0xc7, 0x9c, 0xd4, 0x68, 0x7a, 0x0b, 0x2e, 0x3e,
    0xa4, 0x8e, 0xb8, 0x0c, 0x69, 0x70, 0x04, 0x5e, 0xcc, 0x0c, 0x35, 0x45,
    0xd3, 0xed, 0x54, 0x97, 0x55, 0x9e, 0x47, 0x32, 0x19, 0x14, 0x01, 0x72,
    0xcb, 0xfa, 0xda, 0x4f, 0x1b, 0x28, 0x2b, 0xe2, 0x06, 0x1f, 0x1a, 0xfc,
    0x69, 0x9e, 0x3a, 0xea, 0xa2, 0xf7, 0xe1, 0x1e, 0xc0, 0xc3, 0x30, 0xa4,
    0x0d, 0x92, 0xfa, 0x29, 0xb6, 0x11, 0x81, 0xff, 0x07, 0x5d, 0x5c, 0x02,
    0x35, 0xbd, 0xbc, 0xc3, 0xa0, 0x3c, 0x0f, 0x02, 0x00, 0x3c, 0x56, 0x2a,
    0x56, 0x8a, 0x42, 0x87, 0x7a, 0x78, 0x30, 0xbb, 0xcb, 0x6c, 0x9a, 0x5f,
    0x29, 0x4c, 0xd6, 0xca, 0x9b, 0x44, 0xd8, 0x49, 0x69, 0xa3, 0x2e, 0x59,
    0x97, 0x9d, 0xc0, 0xd6, 0x65, 0x1f, 0xf0, 0x8a, 0xbb, 0x4b, 0xa6, 0x46,
    0xd3, 0x99, 0xfc, 0x4c, 0xbb, 0x55, 0xfe, 0x69, 0xef, 0xc6, 0x6c, 0xfc,
    0x13, 0xcb, 0x9a, 0xc3, 0x6d, 0x63, 0x88, 0x89, 0x8b, 0xa9, 0x57, 0xa1,
    0x4c, 0xd3, 0x2a, 0x64, 0x7a, 0xe6, 0xb2, 0xa3, 0x20, 0x66, 0xfa, 0xb9,
    0x20, 0x34, 0x61, 0xdd, 0xe7, 0x39, 0x94, 0xbc, 0x5c, 0xaa, 0x0b, 0x4e,
    0x16, 0xf3, 0xc1, 0x54, 0xc2, 0xdc, 0xa5, 0xd0, 0xcb, 0xde, 0xcb, 0x97,
    0x6c, 0xc5, 0xfa, 0xd4, 0xa7, 0x7e, 0x94, 0x37, 0x85, 0x79, 0xc7, 0x12,
    0xc9, 0x64, 0xf6, 0x9b, 0x92, 0xd6, 0x16, 0x07, 0xe7, 0x09, 0xa7, 0x11,
    0x0c, 0xd1, 0x06, 0x4f, 0x09, 0x86, 0xab, 0xa0, 0xd9, 0x8f, 0x04, 0xa9,
    0x87, 0xa8, 0xe4, 0xe1, 0x29, 0x83, 0xf3, 0x10, 0xbd, 0xdd, 0xc8, 0x42,
    0xce, 0x31, 0x21, 0xa9,
};

// =============================================================================
// rsa_verify_selftest() - RSA PKCS#1 v1.5 SHA-256 signature-verify known-answer
// test (secroad2). rsa_verify_pkcs1_sha256() is the X.509 certificate SIGNATURE
// verifier for RSA-signed certificates: net/tls/cert_store.c:rsa_verify_pkcs1()
// dispatches to it to authenticate every RSA cert in a TLS chain, and RSA is the
// most common CA signature algorithm on the public web. It is a "is this the
// real server" trust anchor exactly like ecdsa_verify (#659), yet until secroad2
// the tree had NO known-answer test of it and nothing proving it REJECTS a
// forgery. (The pre-existing [TLS1.2-SELFTEST] exercises only the PSS path, not
// the PKCS#1 v1.5 path real certs use.)
//
// The vector is a genuine RSA-2048 PKCS#1 v1.5 SHA-256 signature produced by
// openssl (openssl dgst -sha256 -sign; cross-checked with openssl dgst -verify
// == "Verified OK"); only the PUBLIC modulus n, public exponent e=65537, the
// SHA-256 message digest and the signature are embedded - no private key. A
// correct verifier ACCEPTS this triple iff the signature is valid under (n,e),
// and the tamper cases below prove it REJECTS forgeries, so a passing GREEN run
// also validates the embedded vector. Built as C (not Rust) deliberately: this
// is a self-test OF the existing C rsa.c that drives its real public API; new
// Rust here would test nothing that ships. rsa_verify_kat.sh proves it goes RED
// under -DRSA_KAT_FAULT.
// =============================================================================
int rsa_verify_selftest(void) {
    // kprintf is declared by ../serial.h; bootlog_write by fs/bootlog.h (both
    // included above) - no private extern.
    rsa_public_key_t key;
    key.n = (uint8_t *)KAT_N; key.n_len = sizeof(KAT_N);
    key.e = (uint8_t *)KAT_E; key.e_len = sizeof(KAT_E);

    int fail = 0, checks = 0, k;
    uint8_t sig[256], hash[32], nbuf[256];

    // (a) ACCEPT: the canonical signature must verify.
    checks++;
    if (rsa_verify_pkcs1_sha256(&key, KAT_HASH, sizeof(KAT_HASH),
                                KAT_SIG, sizeof(KAT_SIG)) != RSA_SUCCESS) fail++;

    // (b) REJECT: tampers and structural corruptions must all FAIL. This is the
    // security property; the accept check alone would pass an always-accept stub.
    for (k = 0; k < 256; k++) sig[k] = KAT_SIG[k];
    sig[0] ^= 0x01;                                  // flip a bit in the signature MSB
    checks++;
    if (rsa_verify_pkcs1_sha256(&key, KAT_HASH, 32, sig, 256) == RSA_SUCCESS) fail++;

    for (k = 0; k < 256; k++) sig[k] = KAT_SIG[k];
    sig[255] ^= 0x01;                                // flip a bit in the signature LSB
    checks++;
    if (rsa_verify_pkcs1_sha256(&key, KAT_HASH, 32, sig, 256) == RSA_SUCCESS) fail++;

    for (k = 0; k < 32; k++) hash[k] = KAT_HASH[k];
    hash[16] ^= 0x01;                                // wrong message hash
    checks++;
    if (rsa_verify_pkcs1_sha256(&key, hash, 32, KAT_SIG, 256) == RSA_SUCCESS) fail++;

    // wrong signature length (truncated) must be rejected
    checks++;
    if (rsa_verify_pkcs1_sha256(&key, KAT_HASH, 32, KAT_SIG, 255) == RSA_SUCCESS) fail++;

    // all-0xFF signature (decrypts to non-PKCS#1 garbage) must be rejected
    for (k = 0; k < 256; k++) sig[k] = 0xFF;
    checks++;
    if (rsa_verify_pkcs1_sha256(&key, KAT_HASH, 32, sig, 256) == RSA_SUCCESS) fail++;

    // all-zero signature must be rejected
    for (k = 0; k < 256; k++) sig[k] = 0x00;
    checks++;
    if (rsa_verify_pkcs1_sha256(&key, KAT_HASH, 32, sig, 256) == RSA_SUCCESS) fail++;

    // wrong public key (one modulus bit flipped) must not verify the signature
    for (k = 0; k < 256; k++) nbuf[k] = KAT_N[k];
    nbuf[255] ^= 0x01;
    {
        rsa_public_key_t bad;
        bad.n = nbuf; bad.n_len = 256;
        bad.e = (uint8_t *)KAT_E; bad.e_len = sizeof(KAT_E);
        checks++;
        if (rsa_verify_pkcs1_sha256(&bad, KAT_HASH, 32, KAT_SIG, 256) == RSA_SUCCESS) fail++;
    }

    kprintf("[RSA-KAT] verify PKCS1v1.5-SHA256 accept+reject : %s (checks=%d fail=%d)\n",
            fail ? "FAIL" : "PASS", checks, fail);
    bootlog_write("[RSA-KAT] verify %s checks=%d fail=%d",
                  fail ? "FAIL" : "PASS", checks, fail);
    return fail;
}
