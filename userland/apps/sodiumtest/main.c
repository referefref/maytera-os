// sodiumtest - running proof for the mports libsodium port (userland/ports/libsodium).
//
// Links the static libsodium.a mports built from the sha256-pinned ISC tarball
// and exercises the WHOLE stack on MayteraOS: sodium_init(), the RNG (proving it
// is OUR kernel CSPRNG, not a stub), and one full round-trip of each crypto
// family the port promises (secretbox, box, sign) INCLUDING negative cases
// (tamper/wrong-key must be rejected). Same output discipline as md4ctest: one
// write(2) per serial record. Launched on a throwaway VM via AUTORUN.CFG; its
// fd 2 (/dev/console) reaches the serial port.
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include <sodium.h>

static int g_pass = 0, g_fail = 0;
static void line(const char *s) { write(2, s, strlen(s)); }
static void ck(const char *w, int ok) {
    char b[256];
    if (ok) g_pass++; else g_fail++;
    snprintf(b, sizeof b, "[SODIUMTEST] %s %s\n", ok ? "PASS" : "FAIL", w);
    line(b);
}
static void hexline(const char *l, const unsigned char *p, int n) {
    char b[320]; int o = 0;
    o += snprintf(b + o, sizeof b - o, "[SODIUMTEST] %s ", l);
    for (int i = 0; i < n && o < (int) sizeof b - 3; i++)
        o += snprintf(b + o, sizeof b - o, "%02x", p[i]);
    snprintf(b + o, sizeof b - o, "\n");
    line(b);
}

int main(void) {
    line("[SODIUMTEST] start: libsodium over kernel CSPRNG\n");

    int si = sodium_init();
    ck("sodium_init() >= 0", si >= 0);
    {
        char b[192];
        snprintf(b, sizeof b, "[SODIUMTEST] version=%s rng=%s\n",
                 sodium_version_string(), randombytes_implementation_name());
        line(b);
    }
    /* DECISIVE: the RNG backend is our getrandom()/kernel CSPRNG, not a stub. */
    ck("randombytes backend is maytera_getrandom (kernel CSPRNG)",
       strcmp(randombytes_implementation_name(), "maytera_getrandom") == 0);

    unsigned char r1[32], r2[32];
    memset(r1, 0, 32); memset(r2, 0, 32);
    randombytes_buf(r1, sizeof r1);
    randombytes_buf(r2, sizeof r2);
    hexline("rand1", r1, 32);
    hexline("rand2", r2, 32);
    { int nz = 0; for (int i = 0; i < 32; i++) if (r1[i]) nz = 1;
      ck("randombytes_buf not all-zero", nz); }
    ck("two randombytes_buf draws differ", memcmp(r1, r2, 32) != 0);

    /* crypto_secretbox (XSalsa20-Poly1305) */
    {
        unsigned char key[crypto_secretbox_KEYBYTES];
        unsigned char nonce[crypto_secretbox_NONCEBYTES];
        const unsigned char msg[] = "MayteraOS libsodium secretbox plaintext";
        unsigned long long mlen = sizeof msg - 1;
        unsigned char ct[sizeof msg - 1 + crypto_secretbox_MACBYTES];
        unsigned char pt[sizeof msg - 1];
        crypto_secretbox_keygen(key);
        randombytes_buf(nonce, sizeof nonce);
        ck("crypto_secretbox_easy ok",
           crypto_secretbox_easy(ct, msg, mlen, nonce, key) == 0);
        ck("crypto_secretbox_open_easy ok",
           crypto_secretbox_open_easy(pt, ct, mlen + crypto_secretbox_MACBYTES, nonce, key) == 0);
        ck("secretbox decrypt == original plaintext", memcmp(pt, msg, mlen) == 0);
        ct[0] ^= 1;
        ck("secretbox rejects tampered ciphertext",
           crypto_secretbox_open_easy(pt, ct, mlen + crypto_secretbox_MACBYTES, nonce, key) != 0);
    }

    /* crypto_box (X25519 + XSalsa20-Poly1305) */
    {
        unsigned char apk[crypto_box_PUBLICKEYBYTES], ask[crypto_box_SECRETKEYBYTES];
        unsigned char bpk[crypto_box_PUBLICKEYBYTES], bsk[crypto_box_SECRETKEYBYTES];
        crypto_box_keypair(apk, ask);
        crypto_box_keypair(bpk, bsk);
        unsigned char nonce[crypto_box_NONCEBYTES];
        const unsigned char msg[] = "MayteraOS libsodium box message";
        unsigned long long mlen = sizeof msg - 1;
        unsigned char ct[sizeof msg - 1 + crypto_box_MACBYTES];
        unsigned char pt[sizeof msg - 1];
        randombytes_buf(nonce, sizeof nonce);
        ck("crypto_box_easy ok",
           crypto_box_easy(ct, msg, mlen, nonce, bpk, ask) == 0);
        ck("crypto_box_open_easy ok",
           crypto_box_open_easy(pt, ct, mlen + crypto_box_MACBYTES, nonce, apk, bsk) == 0);
        ck("box decrypt == original", memcmp(pt, msg, mlen) == 0);
    }

    /* crypto_sign (Ed25519) */
    {
        unsigned char pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES];
        crypto_sign_keypair(pk, sk);
        const unsigned char msg[] = "MayteraOS libsodium signed message";
        unsigned long long mlen = sizeof msg - 1;
        unsigned char sm[sizeof msg - 1 + crypto_sign_BYTES];
        unsigned long long smlen = 0;
        crypto_sign(sm, &smlen, msg, mlen, sk);
        unsigned char out[sizeof msg - 1];
        unsigned long long outlen = 0;
        ck("crypto_sign_open verifies valid signature",
           crypto_sign_open(out, &outlen, sm, smlen, pk) == 0 &&
           outlen == mlen && memcmp(out, msg, mlen) == 0);
        sm[crypto_sign_BYTES] ^= 1;
        ck("crypto_sign_open rejects tampered message",
           crypto_sign_open(out, &outlen, sm, smlen, pk) != 0);

        unsigned char pk2[crypto_sign_PUBLICKEYBYTES], sk2[crypto_sign_SECRETKEYBYTES];
        crypto_sign_keypair(pk2, sk2);
        unsigned char sig[crypto_sign_BYTES];
        unsigned long long siglen = 0;
        crypto_sign_detached(sig, &siglen, msg, mlen, sk);
        ck("crypto_sign_verify_detached accepts correct key",
           crypto_sign_verify_detached(sig, msg, mlen, pk) == 0);
        ck("crypto_sign_verify_detached rejects wrong key",
           crypto_sign_verify_detached(sig, msg, mlen, pk2) != 0);
    }

    char sb[160];
    snprintf(sb, sizeof sb, "[SODIUMTEST] DONE pass=%d fail=%d %s\n",
             g_pass, g_fail, g_fail == 0 ? "ALL-PASS" : "HAVE-FAILURES");
    line(sb);
    return g_fail == 0 ? 0 : 1;
}
