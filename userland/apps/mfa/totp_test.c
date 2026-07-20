// totp_test.c - Host unit test for the MFA TOTP core against RFC 6238 Appendix B
// test vectors plus RFC 2202 HMAC and RFC 4648 base32 sanity checks.
// Build:  gcc -DMFA_HOST_TEST -o totp_test totp_test.c totp.c
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "totp.h"

static int g_fail = 0;

// RFC 6238 uses an ASCII seed; SHA1 seed is the 20-byte ASCII "1234567890"x2.
// Base32 of that seed is GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ.
static const char *SEED_B32_SHA1 = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";

// SHA256 RFC 6238 seed = 32 ASCII bytes "12345678901234567890123456789012".
static const char *SEED_B32_SHA256 = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQGEZA";

static void check_b32(void) {
    uint8_t buf[64];
    int n = base32_decode(SEED_B32_SHA1, buf, sizeof(buf));
    int ok = (n == 20) && memcmp(buf, "12345678901234567890", 20) == 0;
    printf("[%s] base32 decode SHA1 seed -> 20 bytes \"12345678901234567890\"\n",
           ok ? "PASS" : "FAIL");
    if (!ok) g_fail++;

    // case/space/padding tolerance
    int n2 = base32_decode("ge zd gnbv==", buf, sizeof(buf));
    int n3 = base32_decode("GEZDGNBV",     buf, sizeof(buf));
    int ok2 = (n2 == n3) && n2 > 0;
    printf("[%s] base32 ignores case/spaces/padding\n", ok2 ? "PASS" : "FAIL");
    if (!ok2) g_fail++;
}

// RFC 2202 HMAC-SHA1 test case 1: key=0x0b x20, data="Hi There"
static void check_hmac(void) {
    uint8_t key[20];
    memset(key, 0x0b, 20);
    uint8_t mac[20];
    hmac_sha1(key, 20, (const uint8_t *)"Hi There", 8, mac);
    static const uint8_t expect[20] = {
        0xb6,0x17,0x31,0x86,0x55,0x05,0x72,0x64,0xe2,0x8b,
        0xc0,0xb6,0xfb,0x37,0x8c,0x8e,0xf1,0x46,0xbe,0x00
    };
    int ok = memcmp(mac, expect, 20) == 0;
    printf("[%s] HMAC-SHA1 RFC 2202 case 1 (\"Hi There\")\n", ok ? "PASS" : "FAIL");
    if (!ok) g_fail++;
}

struct vec { uint64_t t; uint32_t code; const char *label; };

static void run_totp_sha1(void) {
    uint8_t key[64];
    int klen = base32_decode(SEED_B32_SHA1, key, sizeof(key));
    // RFC 6238 Appendix B (SHA1 column), 8-digit codes.
    struct vec v8[] = {
        {          59ULL, 94287082, "T=59" },
        {  1111111109ULL,  7081804, "T=1111111109" },
        {  1111111111ULL, 14050471, "T=1111111111" },
        {  1234567890ULL, 89005924, "T=1234567890" },
        {  2000000000ULL, 69279037, "T=2000000000" },
        { 20000000000ULL, 65353130, "T=20000000000" },
    };
    for (size_t i = 0; i < sizeof(v8)/sizeof(v8[0]); i++) {
        uint32_t got = totp_at_time(key, klen, v8[i].t, 30, 8, TOTP_SHA1);
        int ok = (got == v8[i].code);
        printf("[%s] TOTP SHA1 8-digit %-16s expect=%08u got=%08u\n",
               ok ? "PASS" : "FAIL", v8[i].label, v8[i].code, got);
        if (!ok) g_fail++;
    }
    // 6-digit forms explicitly mentioned in the task.
    struct vec v6[] = {
        {          59ULL, 287082, "T=59" },
        {  1111111109ULL,  81804, "T=1111111109" },
        {  1234567890ULL,   5924, "T=1234567890" },
    };
    for (size_t i = 0; i < sizeof(v6)/sizeof(v6[0]); i++) {
        uint32_t got = totp_at_time(key, klen, v6[i].t, 30, 6, TOTP_SHA1);
        int ok = (got == v6[i].code);
        printf("[%s] TOTP SHA1 6-digit %-16s expect=%06u got=%06u\n",
               ok ? "PASS" : "FAIL", v6[i].label, v6[i].code, got);
        if (!ok) g_fail++;
    }
}

static void run_totp_sha256(void) {
    uint8_t key[64];
    int klen = base32_decode(SEED_B32_SHA256, key, sizeof(key));
    // RFC 6238 Appendix B SHA256 column, 8-digit.
    struct vec v[] = {
        {          59ULL, 46119246, "T=59" },
        {  1111111109ULL, 68084774, "T=1111111109" },
        {  1234567890ULL, 91819424, "T=1234567890" },
        { 20000000000ULL, 77737706, "T=20000000000" },
    };
    for (size_t i = 0; i < sizeof(v)/sizeof(v[0]); i++) {
        uint32_t got = totp_at_time(key, klen, v[i].t, 30, 8, TOTP_SHA256);
        int ok = (got == v[i].code);
        printf("[%s] TOTP SHA256 8-digit %-16s expect=%08u got=%08u\n",
               ok ? "PASS" : "FAIL", v[i].label, v[i].code, got);
        if (!ok) g_fail++;
    }
}

int main(void) {
    printf("=== MFA TOTP host unit test (RFC 6238 / 4226 / 2202 / 4648) ===\n");
    check_b32();
    check_hmac();
    run_totp_sha1();
    run_totp_sha256();
    printf("=== %s ===\n", g_fail == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_fail ? 1 : 0;
}
