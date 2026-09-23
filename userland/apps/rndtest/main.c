// rndtest - running proof for SYS_GETRANDOM (getrandom(2)/getentropy(3) over the
// kernel CSPRNG, crypto/csprng.c). Same discipline as md4ctest/zlibtest: one
// write(2) per serial record so a line built up over several calls is never
// shredded across syslog records.
//
// It exercises the WHOLE Ring-3 path (dispatcher -> rustkern/getrandom.rs ->
// copy_to_user), which the kernel boot self-test (getrandom_selftest_rs) cannot:
// that runs in Ring 0 against csprng_bytes() directly. Launched on a throwaway
// VM via /CONFIG/AUTORUN.CFG = "/APPS/RNDTEST"; its stdout/stderr are /dev/console,
// whose write path (drivers/console.c console_write -> kputc) reaches the serial
// port, so [RNDTEST] lines are observable over the VM serial socket.
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "errno.h"
#include "sys/random.h"

static int g_pass = 0, g_fail = 0;
static void line(const char *s) { write(2, s, strlen(s)); }
static void ck(const char *what, int ok) {
    char b[256];
    if (ok) g_pass++; else g_fail++;
    snprintf(b, sizeof(b), "[RNDTEST] %s %s\n", ok ? "PASS" : "FAIL", what);
    line(b);
}
static void hexline(const char *label, const unsigned char *p, int n) {
    char b[320]; int o = 0;
    o += snprintf(b + o, sizeof(b) - o, "[RNDTEST] %s ", label);
    for (int i = 0; i < n && o < (int)sizeof(b) - 3; i++)
        o += snprintf(b + o, sizeof(b) - o, "%02x", p[i]);
    snprintf(b + o, sizeof(b) - o, "\n");
    line(b);
}
static int all_same(const unsigned char *p, int n) {
    for (int i = 1; i < n; i++) if (p[i] != p[0]) return 0;
    return 1;
}
static int all_zero(const unsigned char *p, int n) {
    for (int i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

int main(void) {
    line("[RNDTEST] start: SYS_GETRANDOM over kernel CSPRNG\n");

    unsigned char a[32], b[32];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    ssize_t r1 = getrandom(a, sizeof(a), 0);
    ssize_t r2 = getrandom(b, sizeof(b), 0);
    ck("getrandom(32) returned 32 (call 1)", r1 == 32);
    ck("getrandom(32) returned 32 (call 2)", r2 == 32);
    hexline("A", a, 32);
    hexline("B", b, 32);
    ck("buffer A is not all-zero", !all_zero(a, 32));
    ck("buffer A is not a single repeated byte", !all_same(a, 32));
    ck("two successive draws differ", memcmp(a, b, 32) != 0);

    ck("getrandom(len=0) returned 0", getrandom(a, 0, 0) == 0);

    errno = 0;
    ssize_t rb = getrandom(a, 8, 0x80000000u);
    ck("getrandom(bad flag) -> -1 / EINVAL", rb == -1 && errno == EINVAL);

    unsigned char e[64];
    memset(e, 0, sizeof(e));
    int ge = getentropy(e, sizeof(e));
    ck("getentropy(64) returned 0", ge == 0);
    ck("getentropy buffer is not all-zero", !all_zero(e, 64));

    errno = 0;
    unsigned char big[300];
    int gb = getentropy(big, sizeof(big));
    ck("getentropy(300) -> -1 / EINVAL", gb == -1 && errno == EINVAL);

    // Statistical sanity: a good CSPRNG fills 4096 bytes with a wide spread of
    // byte values. A stuck/constant source would show very few distinct values.
    unsigned char big2[4096];
    ssize_t rbig = getrandom(big2, sizeof(big2), 0);
    ck("getrandom(4096) returned 4096", rbig == 4096);
    unsigned char seen[256];
    memset(seen, 0, sizeof(seen));
    int uniq = 0;
    for (int i = 0; i < 4096; i++) {
        if (!seen[big2[i]]) { seen[big2[i]] = 1; uniq++; }
    }
    {
        char b2[128];
        snprintf(b2, sizeof(b2), "[RNDTEST] distinct byte values in 4096 draws: %d/256\n", uniq);
        line(b2);
    }
    ck("distinct byte values > 200 of 256", uniq > 200);

    char sb[160];
    snprintf(sb, sizeof(sb), "[RNDTEST] DONE pass=%d fail=%d %s\n",
             g_pass, g_fail, g_fail == 0 ? "ALL-PASS" : "HAVE-FAILURES");
    line(sb);
    return g_fail == 0 ? 0 : 1;
}
