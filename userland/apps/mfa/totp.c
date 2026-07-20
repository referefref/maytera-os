// totp.c - RFC 6238 TOTP core implementation (HMAC-SHA1/SHA256, base32, TOTP).
// Self-contained, freestanding-friendly: no libc dependency beyond memcpy/memset,
// which are provided by both the host (string.h) and the MayteraOS libc.
#include "totp.h"

#ifdef MFA_HOST_TEST
#include <string.h>
#else
// Freestanding build: libc provides memcpy/memset; declare them here so we do
// not pull in <string.h> from the host toolchain.
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
#endif

// ============================================================================
// SHA-1
// ============================================================================

static uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void sha1(const uint8_t *data, size_t len, uint8_t out[SHA1_DIGEST_LEN]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476, h4 = 0xC3D2E1F0;

    // Total message length in bits (big-endian, appended at the end).
    uint64_t bitlen = (uint64_t)len * 8;

    // Process in 64-byte blocks; we stream a small padded tail buffer.
    uint8_t block[64];
    size_t i = 0;

    // Whole blocks straight from the input.
    while (len - i >= 64) {
        memcpy(block, data + i, 64);
        i += 64;

        uint32_t w[80];
        for (int t = 0; t < 16; t++) {
            w[t] = ((uint32_t)block[t * 4] << 24) | ((uint32_t)block[t * 4 + 1] << 16) |
                   ((uint32_t)block[t * 4 + 2] << 8) | ((uint32_t)block[t * 4 + 3]);
        }
        for (int t = 16; t < 80; t++)
            w[t] = rotl32(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int t = 0; t < 80; t++) {
            uint32_t f, k;
            if (t < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999; }
            else if (t < 40) { f = b ^ c ^ d;                      k = 0x6ED9EBA1; }
            else if (t < 60) { f = (b & c) | (b & d) | (c & d);    k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                      k = 0xCA62C1D6; }
            uint32_t tmp = rotl32(a, 5) + f + e + k + w[t];
            e = d; d = c; c = rotl32(b, 30); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    // Build the final padded tail (the remaining <64 bytes plus 0x80, zeros,
    // and the 8-byte length). This may be one or two blocks.
    uint8_t tail[128];
    size_t rem = len - i;
    memcpy(tail, data + i, rem);
    tail[rem] = 0x80;
    size_t tail_len = rem + 1;
    size_t pad_to = (rem + 1 <= 56) ? 56 : 120;  // leave room for 8-byte length
    while (tail_len < pad_to) tail[tail_len++] = 0;
    for (int b = 7; b >= 0; b--) tail[tail_len++] = (uint8_t)(bitlen >> (b * 8));

    for (size_t off = 0; off < tail_len; off += 64) {
        uint32_t w[80];
        for (int t = 0; t < 16; t++) {
            w[t] = ((uint32_t)tail[off + t * 4] << 24) | ((uint32_t)tail[off + t * 4 + 1] << 16) |
                   ((uint32_t)tail[off + t * 4 + 2] << 8) | ((uint32_t)tail[off + t * 4 + 3]);
        }
        for (int t = 16; t < 80; t++)
            w[t] = rotl32(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int t = 0; t < 80; t++) {
            uint32_t f, k;
            if (t < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999; }
            else if (t < 40) { f = b ^ c ^ d;                      k = 0x6ED9EBA1; }
            else if (t < 60) { f = (b & c) | (b & d) | (c & d);    k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                      k = 0xCA62C1D6; }
            uint32_t tmp = rotl32(a, 5) + f + e + k + w[t];
            e = d; d = c; c = rotl32(b, 30); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    uint32_t hh[5] = { h0, h1, h2, h3, h4 };
    for (int j = 0; j < 5; j++) {
        out[j * 4]     = (uint8_t)(hh[j] >> 24);
        out[j * 4 + 1] = (uint8_t)(hh[j] >> 16);
        out[j * 4 + 2] = (uint8_t)(hh[j] >> 8);
        out[j * 4 + 3] = (uint8_t)(hh[j]);
    }
}

// ============================================================================
// SHA-256
// ============================================================================

static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int t = 0; t < 16; t++)
        w[t] = ((uint32_t)p[t*4] << 24) | ((uint32_t)p[t*4+1] << 16) |
               ((uint32_t)p[t*4+2] << 8) | ((uint32_t)p[t*4+3]);
    for (int t = 16; t < 64; t++) {
        uint32_t s0 = rotr32(w[t-15],7) ^ rotr32(w[t-15],18) ^ (w[t-15] >> 3);
        uint32_t s1 = rotr32(w[t-2],17) ^ rotr32(w[t-2],19) ^ (w[t-2] >> 10);
        w[t] = w[t-16] + s0 + w[t-7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int t = 0; t < 64; t++) {
        uint32_t S1 = rotr32(e,6) ^ rotr32(e,11) ^ rotr32(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + S1 + ch + SHA256_K[t] + w[t];
        uint32_t S0 = rotr32(a,2) ^ rotr32(a,13) ^ rotr32(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

void sha256(const uint8_t *data, size_t len, uint8_t out[SHA256_DIGEST_LEN]) {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint64_t bitlen = (uint64_t)len * 8;
    size_t i = 0;
    while (len - i >= 64) { sha256_block(h, data + i); i += 64; }

    uint8_t tail[128];
    size_t rem = len - i;
    memcpy(tail, data + i, rem);
    tail[rem] = 0x80;
    size_t tail_len = rem + 1;
    size_t pad_to = (rem + 1 <= 56) ? 56 : 120;
    while (tail_len < pad_to) tail[tail_len++] = 0;
    for (int b = 7; b >= 0; b--) tail[tail_len++] = (uint8_t)(bitlen >> (b * 8));
    for (size_t off = 0; off < tail_len; off += 64) sha256_block(h, tail + off);

    for (int j = 0; j < 8; j++) {
        out[j*4]   = (uint8_t)(h[j] >> 24);
        out[j*4+1] = (uint8_t)(h[j] >> 16);
        out[j*4+2] = (uint8_t)(h[j] >> 8);
        out[j*4+3] = (uint8_t)(h[j]);
    }
}

// ============================================================================
// HMAC
// ============================================================================

void hmac_sha1(const uint8_t *key, size_t key_len,
               const uint8_t *msg, size_t msg_len,
               uint8_t out[SHA1_DIGEST_LEN]) {
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (key_len > 64) {
        sha1(key, key_len, k);   // SHA1 digest is 20 bytes, rest stays zero
    } else {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int j = 0; j < 64; j++) { ipad[j] = k[j] ^ 0x36; opad[j] = k[j] ^ 0x5c; }

    uint8_t inner_in[64 + 64];   // ipad || msg (msg here is always 8 bytes)
    memcpy(inner_in, ipad, 64);
    memcpy(inner_in + 64, msg, msg_len);
    uint8_t inner[SHA1_DIGEST_LEN];
    sha1(inner_in, 64 + msg_len, inner);

    uint8_t outer_in[64 + SHA1_DIGEST_LEN];
    memcpy(outer_in, opad, 64);
    memcpy(outer_in + 64, inner, SHA1_DIGEST_LEN);
    sha1(outer_in, 64 + SHA1_DIGEST_LEN, out);
}

void hmac_sha256(const uint8_t *key, size_t key_len,
                 const uint8_t *msg, size_t msg_len,
                 uint8_t out[SHA256_DIGEST_LEN]) {
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (key_len > 64) {
        sha256(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int j = 0; j < 64; j++) { ipad[j] = k[j] ^ 0x36; opad[j] = k[j] ^ 0x5c; }

    uint8_t inner_in[64 + 64];
    memcpy(inner_in, ipad, 64);
    memcpy(inner_in + 64, msg, msg_len);
    uint8_t inner[SHA256_DIGEST_LEN];
    sha256(inner_in, 64 + msg_len, inner);

    uint8_t outer_in[64 + SHA256_DIGEST_LEN];
    memcpy(outer_in, opad, 64);
    memcpy(outer_in + 64, inner, SHA256_DIGEST_LEN);
    sha256(outer_in, 64 + SHA256_DIGEST_LEN, out);
}

// ============================================================================
// Base32 (RFC 4648)
// ============================================================================

static int b32_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';   // case-insensitive
    if (c >= '2' && c <= '7') return c - '2' + 26;
    return -1;
}

int base32_decode(const char *in, uint8_t *out, size_t out_cap) {
    uint32_t buffer = 0;
    int bits = 0;
    size_t n = 0;
    for (const char *p = in; *p; p++) {
        char c = *p;
        if (c == '=' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '-')
            continue;   // ignore padding, whitespace, common separators
        int v = b32_val(c);
        if (v < 0) return -1;
        buffer = (buffer << 5) | (uint32_t)v;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (n >= out_cap) return -1;
            out[n++] = (uint8_t)((buffer >> bits) & 0xFF);
        }
    }
    return (int)n;
}

// ============================================================================
// TOTP (RFC 6238 / HOTP RFC 4226)
// ============================================================================

static uint32_t ipow10(int n) {
    uint32_t r = 1;
    while (n-- > 0) r *= 10;
    return r;
}

uint32_t totp_at_counter(const uint8_t *key, size_t key_len,
                         uint64_t counter, int digits, totp_alg_t alg) {
    uint8_t msg[8];
    for (int b = 7; b >= 0; b--) { msg[b] = (uint8_t)(counter & 0xFF); counter >>= 8; }

    uint8_t mac[SHA256_DIGEST_LEN];
    int mac_len;
    if (alg == TOTP_SHA256) { hmac_sha256(key, key_len, msg, 8, mac); mac_len = 32; }
    else                    { hmac_sha1(key, key_len, msg, 8, mac);   mac_len = 20; }

    int offset = mac[mac_len - 1] & 0x0F;   // dynamic truncation
    uint32_t bin = ((uint32_t)(mac[offset] & 0x7F) << 24) |
                   ((uint32_t)mac[offset + 1] << 16) |
                   ((uint32_t)mac[offset + 2] << 8)  |
                   ((uint32_t)mac[offset + 3]);
    return bin % ipow10(digits);
}

uint32_t totp_at_time(const uint8_t *key, size_t key_len,
                      uint64_t unix_time, int period, int digits, totp_alg_t alg) {
    if (period <= 0) period = 30;
    return totp_at_counter(key, key_len, unix_time / (uint64_t)period, digits, alg);
}
