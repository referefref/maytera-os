/*
 * #232 kernel blast-radius audit: execution proof for the ONE dangerous
 * Ring-0 strncpy site found in the whole kernel tree,
 * proc/services.c:209-213 (svc_load_config).
 *
 * Those four fallbacks use  strncpy(dst, "literal", sizeof(dst))  - n ==
 * sizeof(dst) EXACTLY, the one shape the pre-#231 primitive overflows by a
 * byte. (Per blame.md #231: a SHORT NUL-terminated source is what overflows;
 * a long non-terminated one is safe. Do not reason from input length.)
 *
 * GROUND TRUTH FOR THE FRAME is objdump of the REAL kernel object, built with
 * the real Makefile flags (-O2 -fstack-protector-strong -mcmodel=kernel ...):
 *
 *   $ make proc/services.o
 *   $ objdump -d proc/services.o   # svc_load_config, sub $0x218,%rsp
 *     0x08 autostart tmp   0x0c enabled tmp   0x1c count   0x20 cur
 *     0x28 autof[8]        0x30 enf[8]        0x38 uidf[16]
 *     0x48 perms-token scratch[16] (inlined svc_parse_perms)
 *     0x58 name[24]        0x70 account[24]   0x88 exec[64]
 *     0xc8 permsf[64]      0x108 line[256]    0x208 stack canary
 *   Identified by the sizeof() immediates passed to svc_next_field
 *   (0x18,0x40,0x18,0x10,0x40,0x8,0x8) in source order.
 *
 * account[24] therefore ends at 0x88 - 1 and its one-past byte lands EXACTLY
 * on exec[0], the service ELF path. TEST 1 replays that exact frame byte for
 * byte. TEST 2 runs the real parse block in a natural hosted frame (whose
 * padding differs from the kernel s) and catches the enf -> uidf spill.
 */
#include <stdio.h>
#include <string.h>

char *mos_strncpy(char *dest, const char *src, unsigned long n);

#define OFF_AUTOF   0x28
#define OFF_ENF     0x30
#define OFF_UIDF    0x38
#define OFF_SCRATCH 0x48
#define OFF_NAME    0x58
#define OFF_ACCOUNT 0x70
#define OFF_EXEC    0x88
#define OFF_PERMSF  0xc8
#define FRAME_SZ    0x208

static int test_kernel_frame(void) {
    static char frame[FRAME_SZ];
    int fail = 0;
    memset(frame, 0x5A, sizeof(frame));

    /* Fields 1 and 2 parsed fine off a two-field line. */
    strcpy(frame + OFF_NAME, "netinfo");
    strcpy(frame + OFF_EXEC, "/APPS/NETINFO");

    /* Field 3 (account) missing -> proc/services.c:209 fallback fires. */
    mos_strncpy(frame + OFF_ACCOUNT, "service", 24);

    printf("  [T1] after account fallback: exec=\"%s\" (expect \"/APPS/NETINFO\")\n",
           frame + OFF_EXEC);
    if (strcmp(frame + OFF_EXEC, "/APPS/NETINFO") != 0) {
        printf("  [T1] CORRUPTED: exec[0] was overwritten with 0x%02x -> service exec path is now \"%s\"\n",
               (unsigned char)frame[OFF_EXEC], frame + OFF_EXEC);
        fail++;
    }

    /* Field 4 (uid) missing -> :210 fallback; one-past lands on the
       perms-token scratch at 0x48, which is re-initialised later. Benign. */
    mos_strncpy(frame + OFF_UIDF, "0", 16);
    if (frame[OFF_SCRATCH] != 0x5A)
        printf("  [T1] uidf fallback also zeroed the perms scratch byte at 0x48 (benign)\n");

    /* Field 6 (autostart) missing -> :212; one-past lands on enf[0], which
       :213 overwrites immediately after. Benign. */
    mos_strncpy(frame + OFF_AUTOF, "0", 8);
    /* Field 7 (enabled) missing -> :213; one-past lands on uidf[0]. */
    mos_strncpy(frame + OFF_ENF, "1", 8);
    if (frame[OFF_UIDF] == 0)
        printf("  [T1] enf fallback zeroed uidf[0]: uid string \"0\" -> \"\" (svc_atou both give 0, benign)\n");

    return fail;
}

/* ---- Test 2: the real parse block, natural hosted frame ---- */

#define SVC_NAME_MAX    24
#define SVC_EXEC_MAX    64
#define SVC_ACCOUNT_MAX 24

static int svc_next_field(const char **pp, char *out, int outlen) {
    const char *p = *pp;
    while (*p == 32 || *p == 9) p++;
    if (*p == 0 || *p == 10 || *p == 13) { *pp = p; return 0; }
    int n = 0;
    while (*p && *p != 32 && *p != 9 && *p != 10 && *p != 13) {
        if (n < outlen - 1) out[n++] = *p;
        p++;
    }
    out[n] = 0;
    *pp = p;
    return 1;
}

static char r_exec[SVC_EXEC_MAX], r_acct[SVC_ACCOUNT_MAX], r_uidf[16];
static long r_delta;

__attribute__((noinline))
static void parse_one(const char *cfgline) {
    const char *cur = cfgline;
    char name[SVC_NAME_MAX], exec[SVC_EXEC_MAX], account[SVC_ACCOUNT_MAX];
    char uidf[16], permsf[64], autof[8], enf[8];
    memset(name,0x5A,sizeof(name)); memset(exec,0x5A,sizeof(exec));
    memset(account,0x5A,sizeof(account)); memset(uidf,0x5A,sizeof(uidf));
    memset(permsf,0x5A,sizeof(permsf)); memset(autof,0x5A,sizeof(autof));
    memset(enf,0x5A,sizeof(enf));
    r_delta = (long)(exec - account);

    if (!svc_next_field(&cur, name, sizeof(name)))   return;
    if (!svc_next_field(&cur, exec, sizeof(exec)))   return;
    if (!svc_next_field(&cur, account, sizeof(account))) mos_strncpy(account, "service", sizeof(account));
    if (!svc_next_field(&cur, uidf, sizeof(uidf)))   mos_strncpy(uidf, "0", sizeof(uidf));
    if (!svc_next_field(&cur, permsf, sizeof(permsf))) permsf[0] = 0;
    if (!svc_next_field(&cur, autof, sizeof(autof))) mos_strncpy(autof, "0", sizeof(autof));
    if (!svc_next_field(&cur, enf, sizeof(enf)))     mos_strncpy(enf, "1", sizeof(enf));

    memcpy(r_exec, exec, sizeof(exec));
    memcpy(r_acct, account, sizeof(account));
    memcpy(r_uidf, uidf, sizeof(uidf));
}

static int test_natural_frame(void) {
    int fail = 0;
    parse_one("heartbeat /APPS/SVCHB svc_hb 0 fs 1 1");
    printf("  [T2] shipped 7-field line: exec=\"%s\" account=\"%s\" uidf=\"%s\" (no fallback fires)\n",
           r_exec, r_acct, r_uidf);
    if (strcmp(r_exec, "/APPS/SVCHB")) { printf("  [T2] FAIL\n"); fail++; }

    parse_one("netinfo /APPS/NETINFO");
    printf("  [T2] 2-field line (hosted exec-account delta=%ld, kernel=24): exec=\"%s\" uidf=\"%s\"\n",
           r_delta, r_exec, r_uidf);
    if (r_uidf[0] == 0) {
        printf("  [T2] CORRUPTED: enf fallback spilled a NUL onto uidf[0] (\"0\" -> \"\")\n");
        fail++;
    }
    return fail;
}

int main(void) {
    int fail = 0;
    printf("TEST 1: objdump-derived REAL kernel frame, replayed byte for byte\n");
    fail += test_kernel_frame();
    printf("TEST 2: real parse block, natural hosted frame\n");
    fail += test_natural_frame();
    printf("%s (%d)\n", fail ? "RESULT: OVERFLOW OBSERVED" : "RESULT: no corruption", fail);
    return fail ? 1 : 0;
}
